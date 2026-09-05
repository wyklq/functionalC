#include <stdlib.h>
#include <unistd.h>
#include "list.h"
#include "closure.h"
#include "gc.h"

envobj *
envitem(void *var, ssize_t size) {
  envobj *env = gc_alloc(sizeof(envobj), ENVOBJ);
  env->val = var;
  env->size = size;
  return env;
}

void *
unbox(list *l) {
  envobj *env = (envobj *)l->val;
  return env->val; 
}

closure *
bind(closure *c, void *(*fn)(list *), envobj *env) {
  closure *cl;
  if (c == NULL) {
    cl = gc_alloc(sizeof(closure), CLOSURE);
    cl->env = NULL;
    cl->fn = fn;
  }
  else {
    cl = c;
  }
  cl->env = append(cl->env, (void *)env); 
  return cl;
}

void *
call(closure *c, envobj *env) {
  if (c == NULL) {
    return NULL;
  }
  //build the argument list out of temporaries, so protect them for the
  //duration of the call: a collection triggered inside fn must not free them
  gc_push(c);
  gc_push(env);
  list *copylist = copy(c->env);
  copylist = append(copylist, (void *)env);
  gc_push(copylist);
  void *result = c->fn(copylist);
  gc_pop(copylist);
  gc_pop(env);
  gc_pop(c);
  return result;
}

//helper functions (syntactic sugar...erm...i guess...)
//these make using closures easier
//allocate a raw value and let the collector track it
void *
lift(size_t size) {
  return gc_alloc(size, STANDARD);
}

envobj *
liftint(int a) {
  int *v = lift(sizeof(int));
  *v = a;
  envobj *o = envitem((void *)v, sizeof(int)); 
  return o;
}

void
envobj_free(void *_obj) {
  envobj *obj = _obj;
  free(obj);
}

list *
liftlist(list *l, ssize_t s) {
  list *o = NULL;
  list *curr;

  for (curr = l; curr != NULL; curr = curr->next) {
     envobj *lifted = envitem(curr->val, s); 
     o = append(o, (void *)lifted);
  }
  return o;
}

void
closure_free(void *_c) {
  closure *c = _c;
  free(c);
}

//an envobj keeps the value it boxes alive
void
envobj_trace(void *_obj, void (*visit)(void *)) {
  envobj *obj = _obj;
  if (obj == NULL) {
    return;
  }
  visit(obj->val);
}

//a closure keeps its environment list alive
void
closure_trace(void *_obj, void (*visit)(void *)) {
  closure *c = _obj;
  if (c == NULL) {
    return;
  }
  visit(c->env);
}

void
closure_register_tracers(void) {
  gc_register_tracer(ENVOBJ, envobj_trace);
  gc_register_tracer(CLOSURE, closure_trace);
}
