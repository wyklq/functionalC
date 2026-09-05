#ifndef GC_H
#define GC_H

#include <stdbool.h>
#include <stddef.h>

typedef enum TYPE TYPE;

enum TYPE {
  LIST,
  ENVOBJ,
  CLOSURE,
  STANDARD //gc's an generic obj
};

#define TYPE_COUNT 4

/*
 * A tracer walks the outgoing references of an object.
 *
 *   void list_trace(void *obj, gc_visit_fn visit) {
 *     list *l = obj;
 *     visit(l->val);
 *     visit(l->next);
 *   }
 *
 * `visit` ignores anything that is not a tracked object, so it is always
 * safe to hand it a pointer you are not sure about (an interior value, a
 * pointer to a static, an integer that looks like a pointer, ...).
 */
typedef void (*gc_visit_fn)(void *obj);
typedef void (*gc_trace_fn)(void *obj, gc_visit_fn visit);

/* life cycle ------------------------------------------------------- */

//starts the garbage collector.  Call this before anything else.
void gc_init(void);
//frees every tracked object (rooted or not) and resets the collector.
void gc_destroy(void);

/* allocation ------------------------------------------------------- */

/*
 * malloc + gc_register in one step.  This is the only allocation function
 * you need: every object that goes through it is tracked.
 * Exits with status 1 when the system is out of memory.
 */
void *gc_alloc(size_t size, TYPE type);

/* registration ----------------------------------------------------- */

//track an object you allocated yourself.  If the object is already known
//to the collector the call is a no-op (the first registration wins).
void gc_register(void *obj, TYPE type);
//stop tracking an object.  The object itself is *not* freed.
void gc_remove(void *obj);

/* type tables ------------------------------------------------------ */

//how to destroy an object of `type` (defaults to free() when unset)
void gc_register_destructor(TYPE type, void (*destructor)(void *));
//how to find the objects referenced by an object of `type`
void gc_register_tracer(TYPE type, gc_trace_fn tracer);

/* roots ------------------------------------------------------------ */

//permanent root: the object (and everything it references) survives every
//collection until it is unrooted.  Use this for globals / long lived data.
void gc_root(void *obj);
void gc_unroot(void *obj);

//scoped root (a stack): protects a temporary until you pop it.
void gc_push(void *obj);
void gc_pop(void *obj);
//pops the n most recently pushed roots
void gc_pop_n(size_t n);

/* collection -------------------------------------------------------- */

//mark & sweep.  Roots are the permanent roots, the scoped roots and
//(when stack scanning is enabled) anything that looks like a tracked
//object on the C stack.
void gc_collect(void);
//mark & sweep using only the roots you registered explicitly
void gc_collect_roots(void);
//conservative C stack scanning, on by default
void gc_set_stack_scanning(bool enabled);

/* introspection ----------------------------------------------------- */

//prints every tracked object (debugging)
void gc_print(void);
//prints a summary: object/byte counts per type
void gc_stats(void);
bool gc_is_tracked(void *obj);
bool gc_is_rooted(void *obj);
size_t gc_object_count(void);
size_t gc_bytes(void);

/* deprecated aliases ------------------------------------------------ */
//gc_mark/gc_unmark are the old names for gc_root/gc_unroot
void gc_mark(void *obj);
void gc_unmark(void *obj);

#endif
