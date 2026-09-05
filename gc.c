#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "gc.h"
#include "closure.h"
#include "list.h"

/*
 * A tracing (mark & sweep), non-moving garbage collector.
 *
 *   gc_alloc()/gc_register()  -> every object lands in one intrusive list
 *   gc_register_tracer()      -> every type knows how to find its children
 *   gc_root()/gc_push()       -> what is alive
 *   gc_collect()              -> mark from the roots, sweep the rest
 *
 * Roots come from three places:
 *   1. gc_root()   - permanent (globals, long lived data)
 *   2. gc_push()   - scoped (a temporary, popped before you return)
 *   3. the C stack - scanned conservatively, so plain locals count as roots
 *
 * Conservative means a word that merely *looks* like a pointer to a
 * tracked object keeps that object alive.  That failure mode is a leak,
 * never a dangling pointer, which is the right trade off for C.
 */

#define GC_TABLE_BITS  12u
#define GC_TABLE_SIZE  (1u << GC_TABLE_BITS)
#define GC_ROOTS_MAX   512
//how far above the stack pointer we are willing to look (bytes)
#define GC_SCAN_SLACK  (2u * 1024u * 1024u)

/*
 * Conservative stack scanning reads raw stack words, which is exactly the
 * kind of thing AddressSanitizer exists to catch: it poisons the redzones
 * around live locals and the frames of functions that already returned.
 * So under ASan the scan defaults to off (call gc_set_stack_scanning(true)
 * to force it).  Everywhere else it defaults to on, because it is what
 * makes ordinary C locals act as roots.
 */
#if defined(__SANITIZE_ADDRESS__)
#define GC_STACK_SCAN_DEFAULT false
#else
#define GC_STACK_SCAN_DEFAULT true
#endif

//everything the collector knows about one object
typedef struct ref ref;
struct ref {
  void *ptr;
  TYPE type;
  bool marked;   //reachable during the current collection
  bool rooted;   //permanent root (gc_root)
  ref *next;     //all objects
  ref *bucket;   //hash table chain
};

typedef struct gc gc;
struct gc {
  ref *objects;                 //every registered object
  size_t count;
  ref *table[GC_TABLE_SIZE];    //ptr -> ref, O(1) lookups
  void (*destructor_table[TYPE_COUNT])(void *);
  gc_trace_fn trace_table[TYPE_COUNT];
  void *pinned[GC_ROOTS_MAX];   //scoped roots (gc_push)
  size_t pinned_count;
  bool stack_scan;
  size_t swept;                 //freed by the last collection
  size_t kept;                  //kept by the last collection
  ref **work;                   //marking worklist
  size_t work_len;
  size_t work_cap;
  bool ready;
};

//this IS the garbage collector
static gc _gc;

/* private helpers */
static void standard_free(void *ptr);
static void table_insert(ref *r);
static void table_remove(ref *r);
static ref *find_ref(void *obj);
static void work_push(ref *r);
static ref *work_pop(void);
static void mark_visit(void *obj);
static void mark_seed(void *obj);
static void drain(void);
static void mark_stack(void);
static void sweep(void);
static const char *type_name(TYPE type);
static size_t object_size(TYPE type);

/* ------------------------------------------------------------------ */
/* hash table: ptr -> ref                                              */
/* ------------------------------------------------------------------ */

static size_t
ptr_hash(void *p) {
  uintptr_t v = (uintptr_t)p >> 4; //low bits of a malloced pointer are noise
  v *= 0x9E3779B97F4A7C15ull;
  return (size_t)(v >> (64 - GC_TABLE_BITS));
}

static void
table_insert(ref *r) {
  size_t h = ptr_hash(r->ptr);
  r->bucket = _gc.table[h];
  _gc.table[h] = r;
}

static void
table_remove(ref *r) {
  ref **link = &_gc.table[ptr_hash(r->ptr)];
  while (*link != NULL) {
    if (*link == r) {
      *link = r->bucket;
      r->bucket = NULL;
      return;
    }
    link = &(*link)->bucket;
  }
}

//O(1); this replaced the old O(n) is_registered() scan
static ref *
find_ref(void *obj) {
  if (obj == NULL) {
    return NULL;
  }
  for (ref *r = _gc.table[ptr_hash(obj)]; r != NULL; r = r->bucket) {
    if (r->ptr == obj) {
      return r;
    }
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

static void
standard_free(void *ptr) {
  free(ptr);
}

static const char *
type_name(TYPE type) {
  switch (type) {
    case LIST: return "LIST";
    case ENVOBJ: return "ENVOBJ";
    case CLOSURE: return "CLOSURE";
    case STANDARD: return "STANDARD";
    default: return "?";
  }
}

static size_t
object_size(TYPE type) {
  switch (type) {
    case LIST: return sizeof(list);
    case ENVOBJ: return sizeof(envobj);
    case CLOSURE: return sizeof(closure);
    default: return 0;
  }
}

/* ------------------------------------------------------------------ */
/* life cycle                                                          */
/* ------------------------------------------------------------------ */

void
gc_init(void) {
  memset(&_gc, 0, sizeof(_gc));
  for (size_t i = 0; i < TYPE_COUNT; ++i) {
    _gc.destructor_table[i] = standard_free;
    _gc.trace_table[i] = NULL;
  }
  gc_register_destructor(ENVOBJ, envobj_free);
  gc_register_destructor(CLOSURE, closure_free);
  gc_register_destructor(LIST, list_free);
  gc_register_destructor(STANDARD, standard_free);
  _gc.stack_scan = GC_STACK_SCAN_DEFAULT;
  _gc.ready = true;
}

void
gc_destroy(void) {
  ref *curr = _gc.objects;
  while (curr != NULL) {
    ref *next = curr->next;
    (*(_gc.destructor_table[curr->type]))(curr->ptr);
    free(curr);
    curr = next;
  }
  free(_gc.work);
  memset(&_gc, 0, sizeof(_gc));
}

/* ------------------------------------------------------------------ */
/* allocation / registration                                           */
/* ------------------------------------------------------------------ */

//the one allocation entry point: whatever you allocate here is tracked
void *
gc_alloc(size_t size, TYPE type) {
  void *p = malloc(size);
  if (p == NULL) {
    exit(1);
  }
  //the stack scanner must not see whatever malloc left behind
  memset(p, 0, size);
  gc_register(p, type);
  return p;
}

void
gc_register(void *obj, TYPE type) {
  if (obj == NULL || type >= TYPE_COUNT) {
    return;
  }
  //registering twice would mean a double free; the first registration wins
  if (find_ref(obj) != NULL) {
    return;
  }
  ref *r = malloc(sizeof(ref));
  if (r == NULL) {
    exit(1);
  }
  r->ptr = obj;
  r->type = type;
  r->marked = false;
  r->rooted = false;
  r->next = _gc.objects;
  r->bucket = NULL;
  _gc.objects = r;
  table_insert(r);
  _gc.count++;
}

void
gc_remove(void *obj) {
  ref *r = find_ref(obj);
  if (r == NULL) {
    return;
  }
  ref **link = &_gc.objects;
  while (*link != NULL) {
    if (*link == r) {
      *link = r->next;
      break;
    }
    link = &(*link)->next;
  }
  table_remove(r);
  for (size_t i = 0; i < _gc.pinned_count; ++i) {
    if (_gc.pinned[i] == obj) {
      memmove(&_gc.pinned[i], &_gc.pinned[i + 1],
              (_gc.pinned_count - i - 1) * sizeof(void *));
      _gc.pinned_count--;
      break;
    }
  }
  _gc.count--;
  free(r);
}

/* ------------------------------------------------------------------ */
/* type tables                                                         */
/* ------------------------------------------------------------------ */

void
gc_register_destructor(TYPE type, void (*destructor)(void *)) {
  if (type < TYPE_COUNT && destructor != NULL) {
    _gc.destructor_table[type] = destructor;
  }
}

void
gc_register_tracer(TYPE type, gc_trace_fn tracer) {
  if (type < TYPE_COUNT) {
    _gc.trace_table[type] = tracer;
  }
}

/* ------------------------------------------------------------------ */
/* roots                                                               */
/* ------------------------------------------------------------------ */

void
gc_root(void *obj) {
  ref *r = find_ref(obj);
  if (r != NULL) {
    r->rooted = true;
  }
}

void
gc_unroot(void *obj) {
  ref *r = find_ref(obj);
  if (r != NULL) {
    r->rooted = false;
  }
}

bool
gc_is_rooted(void *obj) {
  ref *r = find_ref(obj);
  if (r == NULL) {
    return false;
  }
  if (r->rooted) {
    return true;
  }
  for (size_t i = 0; i < _gc.pinned_count; ++i) {
    if (_gc.pinned[i] == obj) {
      return true;
    }
  }
  return false;
}

//scoped roots: a small stack of temporaries protected until you pop them
void
gc_push(void *obj) {
  if (obj == NULL) {
    return;
  }
  if (_gc.pinned_count >= GC_ROOTS_MAX) {
    fprintf(stderr, "gc: root stack overflow\n");
    exit(1);
  }
  _gc.pinned[_gc.pinned_count++] = obj;
}

void
gc_pop(void *obj) {
  for (size_t i = _gc.pinned_count; i > 0; --i) {
    if (_gc.pinned[i - 1] == obj) {
      memmove(&_gc.pinned[i - 1], &_gc.pinned[i],
              (_gc.pinned_count - i) * sizeof(void *));
      _gc.pinned_count--;
      return;
    }
  }
}

void
gc_pop_n(size_t n) {
  if (n > _gc.pinned_count) {
    n = _gc.pinned_count;
  }
  _gc.pinned_count -= n;
}

//old names, kept so existing code keeps working
void
gc_mark(void *obj) {
  gc_root(obj);
}

void
gc_unmark(void *obj) {
  gc_unroot(obj);
}

/* ------------------------------------------------------------------ */
/* marking                                                             */
/* ------------------------------------------------------------------ */

//an explicit worklist, so a very deep list cannot blow the C stack
static void
work_push(ref *r) {
  if (_gc.work_len == _gc.work_cap) {
    size_t cap = _gc.work_cap == 0 ? 64 : _gc.work_cap * 2;
    ref **items = realloc(_gc.work, cap * sizeof(ref *));
    if (items == NULL) {
      exit(1);
    }
    _gc.work = items;
    _gc.work_cap = cap;
  }
  _gc.work[_gc.work_len++] = r;
}

static ref *
work_pop(void) {
  if (_gc.work_len == 0) {
    return NULL;
  }
  return _gc.work[--_gc.work_len];
}

static void
mark_seed(void *obj) {
  ref *r = find_ref(obj);
  if (r == NULL || r->marked) {
    return;
  }
  r->marked = true;
  work_push(r);
}

//handed to every tracer; ignores anything that is not a tracked object
static void
mark_visit(void *obj) {
  mark_seed(obj);
}

static void
drain(void) {
  ref *r;
  while ((r = work_pop()) != NULL) {
    gc_trace_fn trace = _gc.trace_table[r->type];
    if (trace != NULL) {
      trace(r->ptr, mark_visit);
    }
  }
}

static void
mark_explicit_roots(void) {
  for (ref *r = _gc.objects; r != NULL; r = r->next) {
    if (r->rooted) {
      mark_seed(r->ptr);
    }
  }
  for (size_t i = 0; i < _gc.pinned_count; ++i) {
    mark_seed(_gc.pinned[i]);
  }
}

#if defined(__GNUC__)
#define GC_HAVE_FRAME_ADDR 1
#endif

//the outermost frame we can reach by walking the frame pointer chain.
//everything above it belongs to calls that have already returned.
static uintptr_t
frame_walk_top(void) {
  uintptr_t top = (uintptr_t)&top;
#ifdef GC_HAVE_FRAME_ADDR
  uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
  for (int i = 0; i < 1024; ++i) {
    if (fp == 0 || fp < (uintptr_t)&top || fp % sizeof(uintptr_t) != 0) {
      break;
    }
    if (fp > top) {
      top = fp;
    }
    uintptr_t next = *(uintptr_t *)fp; //saved frame pointer of the caller
    //frames only ever grow upwards and stay small; bail out on garbage
    if (next <= fp || next - fp > (1u << 20)) {
      break;
    }
    fp = next;
  }
#endif
  return top;
}

/*
 * Bounds of the region of the C stack we are willing to read.
 *
 * Only the part *above* the stack pointer matters: that is where the
 * frames of our callers live.  Reading below it would mean reading dead
 * frames, which is useless (nothing down there can be a live local) and
 * unsafe.
 *
 * How far up we go is the interesting part.  The whole [stack] mapping
 * would include frames of functions that have already returned, and those
 * still hold stale copies of pointers: scanning them keeps dead objects
 * alive for no reason.  So we stop at the outermost *live* frame, which
 * the frame pointer chain gives us exactly.  Everything above `top` is a
 * stale slot from a call that is long gone.
 *
 * A conservative scan can only over-approximate liveness, never
 * under-approximate it: it may keep an object alive that a precise
 * collector would free (a leak), but it can never free something that is
 * still referenced (a crash).  That is the right trade off for C.
 */
static void
stack_bounds(uintptr_t *lo, uintptr_t *hi) {
  uintptr_t sp = (uintptr_t)&sp;
  *lo = sp & ~(uintptr_t)(sizeof(uintptr_t) - 1);
  *hi = sp; //nothing to scan unless we learn the extent below

#ifdef __linux__
  FILE *f = fopen("/proc/self/maps", "r");
  if (f != NULL) {
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
      unsigned long long a = 0, b = 0;
      if (sscanf(line, "%llx-%llx", &a, &b) == 2) {
        if (sp >= (uintptr_t)a && sp < (uintptr_t)b) {
          uintptr_t end = (uintptr_t)b;
          //the frame chain ends the useful region; dead frames above it
          //hold nothing live, and may hold values from ASan-poisoned frames
          uintptr_t top = frame_walk_top();
          if (top > sp && top < end) {
            end = top + sizeof(uintptr_t) * 4;
          }
          if (end - sp > GC_SCAN_SLACK) {
            end = sp + GC_SCAN_SLACK;
          }
          *hi = end;
          fclose(f);
          return;
        }
      }
    }
    fclose(f);
  }
#endif

  uintptr_t top = frame_walk_top();
  if (top > sp) {
    *hi = top + sizeof(uintptr_t) * 4;
  }
}

static void
mark_stack(void) {
  if (!_gc.stack_scan) {
    return;
  }
  uintptr_t lo, hi;
  stack_bounds(&lo, &hi);
  if (hi <= lo) {
    return;
  }
  uintptr_t *p = (uintptr_t *)lo;
  uintptr_t *end = (uintptr_t *)hi;
  for (; p < end; ++p) {
    mark_seed((void *)*p);
  }
  drain(); //newly found objects can themselves hold references
}

/* ------------------------------------------------------------------ */
/* sweeping                                                            */
/* ------------------------------------------------------------------ */

static void
sweep(void) {
  ref **link = &_gc.objects;
  size_t swept = 0;
  size_t kept = 0;
  while (*link != NULL) {
    ref *r = *link;
    if (r->marked) {
      r->marked = false;
      link = &r->next;
      kept++;
      continue;
    }
    *link = r->next;
    table_remove(r);
    (*(_gc.destructor_table[r->type]))(r->ptr);
    free(r);
    swept++;
  }
  _gc.count = kept;
  _gc.swept = swept;
  _gc.kept = kept;
}

void
gc_collect_roots(void) {
  if (!_gc.ready) {
    return;
  }
  mark_explicit_roots();
  drain();
  sweep();
}

void
gc_collect(void) {
  if (!_gc.ready) {
    return;
  }
  mark_explicit_roots();
  drain();
  mark_stack();
  sweep();
}

void
gc_set_stack_scanning(bool enabled) {
  _gc.stack_scan = enabled;
}

/* ------------------------------------------------------------------ */
/* introspection                                                       */
/* ------------------------------------------------------------------ */

bool
gc_is_tracked(void *obj) {
  return find_ref(obj) != NULL;
}

size_t
gc_object_count(void) {
  return _gc.count;
}

size_t
gc_bytes(void) {
  size_t bytes = 0;
  for (ref *r = _gc.objects; r != NULL; r = r->next) {
    bytes += object_size(r->type);
  }
  return bytes;
}

void
gc_print(void) {
  printf("TRACKED OBJECTS (%zu):\n", _gc.count);
  for (ref *r = _gc.objects; r != NULL; r = r->next) {
    printf("  %-8s at %p%s\n", type_name(r->type), r->ptr,
           r->rooted ? " [rooted]" : "");
  }
  printf("ROOTS:\n");
  for (ref *r = _gc.objects; r != NULL; r = r->next) {
    if (r->rooted) {
      printf("  %-8s at %p\n", type_name(r->type), r->ptr);
    }
  }
  for (size_t i = 0; i < _gc.pinned_count; ++i) {
    printf("  pinned    at %p\n", _gc.pinned[i]);
  }
}

void
gc_stats(void) {
  size_t counts[TYPE_COUNT] = {0};
  size_t bytes[TYPE_COUNT] = {0};
  for (ref *r = _gc.objects; r != NULL; r = r->next) {
    counts[r->type]++;
    bytes[r->type] += object_size(r->type);
  }
  printf("gc: %zu objects", _gc.count);
  for (int i = 0; i < TYPE_COUNT; ++i) {
    if (counts[i] != 0) {
      printf(", %zu %s", counts[i], type_name((TYPE)i));
    }
  }
  printf(" (%zu bytes of headers/values)\n", gc_bytes());
  printf("gc: last collection freed %zu, kept %zu; stack scanning %s\n",
         _gc.swept, _gc.kept, _gc.stack_scan ? "on" : "off");
}
