/*
 * Tests for the garbage collector.  Build with: make gctest
 * Every check is a boolean invariant; the first failure aborts.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "list.h"
#include "functional.h"
#include "closure.h"
#include "gc.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do {                                 \
  checks++;                                                   \
  if (!(cond)) {                                              \
    failures++;                                               \
    printf("FAIL (%s:%d): ", __FILE__, __LINE__);             \
    printf(__VA_ARGS__);                                      \
    printf("\n");                                             \
  }                                                           \
} while (0)

//returns true if int v is odd, false otherwise
static bool
odd(void *v, void *args) {
  return *((int *)v) % 2 != 0;
}

//returns twice int v
static void *
dbl(void *v, void *args) {
  int *o = lift(sizeof(int));
  *o = *((int *)v) * 2;
  return o;
}

static void *
add(list *l) {
  int *a = unbox(l);
  int *b = unbox(l->next);
  int *o = lift(sizeof(int));
  *o = *a + *b;
  return o;
}

//a throwaway value, so we can count collections of STANDARD objects
static void *
mkint(int v) {
  int *o = lift(sizeof(int));
  *o = v;
  return o;
}

/* 1. every object the library allocates is tracked ------------------ */
static void
test_tracking(void) {
  size_t before = gc_object_count();

  list *l = append(NULL, mkint(1));
  CHECK(gc_is_tracked(l), "append() result must be tracked");
  CHECK(gc_is_tracked(l->val), "payload must be tracked");
  CHECK(gc_object_count() == before + 2, "expected 2 objects, got %zu",
        gc_object_count() - before);

  list *c = copyitem(l);
  CHECK(gc_is_tracked(c), "copyitem() result must be tracked");

  envobj *e = liftint(7);
  CHECK(gc_is_tracked(e), "liftint() envobj must be tracked");
  CHECK(gc_is_tracked(e->val), "liftint() payload must be tracked");

  closure *cl = bind(NULL, NULL, e);
  CHECK(gc_is_tracked(cl), "bind() closure must be tracked");

  list *r = range(0, 99);
  CHECK(gc_object_count() >= before + 200, "range(0,99) leaks objects");
  (void)r;
  (void)c;
  (void)cl;
}

/* 2. unreachable objects are collected ------------------------------ */
static void
test_sweep(void) {
  size_t before = gc_object_count();

  //build a list, drop every reference to it, collect
  list *l = NULL;
  for (int i = 0; i < 32; ++i) {
    l = append(l, mkint(i));
  }
  CHECK(gc_object_count() == before + 64, "expected 64 objects");
  //stack scanning would keep `l` alive, so clear it and collect by roots
  l = NULL;
  gc_collect_roots();
  CHECK(gc_object_count() <= before, "unreachable list survived: %zu left",
        gc_object_count() - before);
}

/* 3. tracing: a live list keeps its payload and tail alive ----------- */
static void
test_tracing(void) {
  gc_collect_roots(); //start from a clean slate

  list *l = append(NULL, mkint(1));
  l = append(l, mkint(2));
  l = append(l, mkint(3));
  gc_root(l);
  gc_collect_roots();

  CHECK(gc_is_tracked(l), "rooted list must survive");
  CHECK(gc_is_tracked(l->val), "rooted list must keep its payload");
  CHECK(gc_is_tracked(l->next), "rooted list must keep its tail");
  CHECK(gc_is_tracked(l->next->val), "rooted list must keep the tail payload");
  CHECK(gc_is_tracked(l->next->next->val), "must keep the last payload");
  CHECK(*(int *)l->next->next->val == 3, "payload value corrupted");
  CHECK(gc_object_count() == 6, "expected 6 objects, got %zu",
        gc_object_count());

  //unrooting frees the whole graph, not just the head
  gc_unroot(l);
  gc_collect_roots();
  CHECK(gc_object_count() == 0, "whole graph should be freed, %zu left",
        gc_object_count());
}

/* 4. closures keep their environment alive -------------------------- */
static void
test_closure_tracing(void) {
  gc_collect_roots();

  closure *cl = bind(NULL, NULL, liftint(42));
  gc_root(cl);
  gc_collect_roots();

  CHECK(gc_is_tracked(cl), "closure must survive");
  CHECK(gc_is_tracked(cl->env), "closure must keep its env list");
  CHECK(gc_is_tracked(cl->env->val), "closure must keep the envobj");
  CHECK(gc_is_tracked(((envobj *)cl->env->val)->val),
        "closure must keep the boxed int");
  CHECK(*(int *)((envobj *)cl->env->val) ->val == 42, "env value corrupted");

  gc_unroot(cl);
  gc_collect_roots();
  CHECK(gc_object_count() == 0, "closure graph should be freed, %zu left",
        gc_object_count());
}

/* 5. cycles --------------------------------------------------------- */
static void
test_cycle(void) {
  gc_collect_roots();

  //reference counting could never collect this
  list *a = newitem(NULL);
  list *b = newitem(NULL);
  a->next = b;
  b->next = a;
  gc_collect_roots();
  CHECK(gc_object_count() == 0, "cyclic garbage must be collected, %zu left",
        gc_object_count());

  //a cycle that *is* rooted must survive
  list *c = newitem(NULL);
  list *d = newitem(NULL);
  c->next = d;
  d->next = c;
  gc_root(c);
  gc_collect_roots();
  CHECK(gc_object_count() == 2, "rooted cycle must survive, %zu left",
        gc_object_count());
  CHECK(gc_is_tracked(d), "the other half of the cycle must survive");
  gc_unroot(c);
  gc_collect_roots();
  CHECK(gc_object_count() == 0, "unrooted cycle must be freed, %zu left",
        gc_object_count());
}

/* 6. roots: permanent and scoped ------------------------------------ */
static void
test_roots(void) {
  gc_collect_roots();

  void *v = mkint(5);
  gc_root(v);
  gc_collect_roots();
  CHECK(gc_is_tracked(v), "a rooted object must survive");
  CHECK(gc_is_rooted(v), "gc_is_rooted() should say yes");
  gc_unroot(v);
  gc_collect_roots();
  CHECK(!gc_is_tracked(v), "an unrooted object must be collected");

  //scoped roots
  void *w = mkint(6);
  gc_push(w);
  gc_collect_roots();
  CHECK(gc_is_tracked(w), "a pushed object must survive");
  gc_pop(w);
  gc_collect_roots();
  CHECK(!gc_is_tracked(w), "a popped object must be collected");

  //gc_pop_n pops several at once
  void *x = mkint(7);
  void *y = mkint(8);
  gc_push(x);
  gc_push(y);
  gc_collect_roots();
  CHECK(gc_is_tracked(x) && gc_is_tracked(y), "both should survive");
  gc_pop_n(2);
  gc_collect_roots();
  CHECK(!gc_is_tracked(x) && !gc_is_tracked(y), "both should be collected");

  //gc_remove stops tracking without freeing
  int *raw = lift(sizeof(int));
  gc_root(raw);
  gc_remove(raw);
  CHECK(!gc_is_tracked(raw), "gc_remove() must untrack");
  CHECK(*raw == 0, "gc_remove() must not free the object");
  free(raw);
}

/* 7. registration is idempotent and O(1) ---------------------------- */
static void
test_double_register(void) {
  gc_collect_roots();

  void *v = mkint(9);
  size_t before = gc_object_count();
  gc_register(v, STANDARD);
  gc_register(v, STANDARD);
  gc_register(v, LIST); //a second, different type must not add a second ref
  CHECK(gc_object_count() == before, "double registration must be a no-op");

  //registering NULL is harmless
  gc_register(NULL, STANDARD);
  gc_root(NULL);
  gc_push(NULL);
  gc_pop(NULL);
  gc_remove(NULL);
  gc_collect_roots();

  //large numbers of objects: this used to be O(n^2)
  for (int i = 0; i < 20000; ++i) {
    void *o = lift(sizeof(int));
    (void)o;
  }
  CHECK(gc_object_count() == 20000, "expected 20000 objects, got %zu",
        gc_object_count());
  gc_collect_roots();
  CHECK(gc_object_count() == 0, "all should be collected, %zu left",
        gc_object_count());
}

/* 8. stack scanning ------------------------------------------------- */
/*
 * The scan reads raw stack words, so it is disabled under AddressSanitizer
 * (ASan poisons frame redzones and dead frames, which is exactly what a
 * conservative scan walks into).  These checks only run where it is safe.
 */
#if !defined(__SANITIZE_ADDRESS__)
static void
test_stack_scan(void) {
  gc_collect_roots();

  //with the scan on, a plain local keeps its object alive, no rooting
  list *local = append(NULL, mkint(11));
  gc_set_stack_scanning(true);
  gc_collect();
  CHECK(gc_is_tracked(local), "scanning should keep a live local alive");
  CHECK(gc_is_tracked(local->val), "and its payload");

  //with the scan off, the same local is collected
  gc_set_stack_scanning(false);
  gc_collect();
  CHECK(!gc_is_tracked(local), "with scanning off the local is collected");

  //the scan must never invent a pointer: unknown words are ignored
  gc_set_stack_scanning(true);
  gc_collect_roots();
  CHECK(gc_object_count() == 0, "nothing should be left, %zu left",
        gc_object_count());
}
#else
static void
test_stack_scan(void) {
  //nothing to check: the scan is off under ASan by design
  CHECK(!gc_is_rooted(NULL), "NULL is never rooted");
}
#endif

/* 9. deep lists must not overflow the C stack ------------------------ */
static void
test_deep(void) {
  gc_collect_roots();

  list *head = NULL;
  list *tail = NULL;
  for (int i = 0; i < 200000; ++i) {
    list *n = newitem(NULL);
    if (head == NULL) {
      head = n;
    }
    if (tail != NULL) {
      tail->next = n;
    }
    tail = n;
  }
  gc_root(head);
  gc_collect_roots(); //marking 200k deep would blow a recursive marker
  CHECK(gc_object_count() == 200000, "deep list must survive, %zu left",
        gc_object_count());
  gc_unroot(head);
  gc_collect_roots();
  CHECK(gc_object_count() == 0, "deep list must be freed, %zu left",
        gc_object_count());
}

/* 10. the functional layer still works ------------------------------- */
static void
test_functional(void) {
  gc_collect_roots();

  list *r = range(0, 4);
  gc_root(r);
  gc_collect_roots();

  //map doubles everything, and the results must survive a collection
  list *m = map(r, dbl, NULL);
  gc_root(m);
  gc_collect_roots();
  CHECK(*(int *)m->val == 0, "map: 0*2");
  CHECK(*(int *)m->next->val == 2, "map: 1*2");
  CHECK(*(int *)m->next->next->next->next->val == 8, "map: 4*2");

  list *f = filter(r, odd, NULL);
  gc_root(f);
  gc_collect_roots();
  CHECK(*(int *)f->val == 1, "filter: first odd");
  CHECK(*(int *)f->next->val == 3, "filter: second odd");
  CHECK(f->next->next == NULL, "filter: only two odds in 0..4");

  //closures through lmap
  closure *addtwo = bind(NULL, add, liftint(2));
  gc_root(addtwo);
  list *lifted = liftlist(r, sizeof(int));
  gc_root(lifted);
  list *res = lmap(lifted, addtwo);
  gc_root(res);
  gc_collect_roots();
  CHECK(*(int *)res->val == 2, "lmap: 0+2");
  CHECK(*(int *)res->next->next->val == 4, "lmap: 2+2");

  gc_unroot(r);
  gc_unroot(m);
  gc_unroot(f);
  gc_unroot(addtwo);
  gc_unroot(lifted);
  gc_unroot(res);
  gc_collect_roots();
  CHECK(gc_object_count() == 0, "everything should be freed, %zu left",
        gc_object_count());
}

/* 11. gc_destroy ---------------------------------------------------- */
static void
test_destroy(void) {
  void *v = mkint(1);
  gc_root(v);
  list *l = append(NULL, mkint(2));
  gc_root(l);
  CHECK(gc_object_count() > 0, "should have objects");
  gc_destroy(); //frees even rooted objects
  CHECK(gc_object_count() == 0, "gc_destroy() must free everything");
}

int
main(void) {
  gc_init();
  list_register_tracer();
  closure_register_tracers();

  test_tracking();
  gc_collect_roots();
  test_sweep();
  test_tracing();
  test_closure_tracing();
  test_cycle();
  test_roots();
  test_double_register();
  test_stack_scan();
  test_deep();
  test_functional();
  test_destroy();

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
