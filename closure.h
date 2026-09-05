#ifndef CLOSURE_H
#define CLOSURE_H
#include <unistd.h>
#include "list.h"

typedef struct closure closure;

struct closure {
  void *(*fn)(list *);
  list *env;
};

typedef struct envobj envobj;

struct envobj {
  void *val;
  ssize_t size;
};

envobj *envitem(void *var, ssize_t s);
void *unbox(list *l); 
closure *bind(closure *c, void *(*fn)(list *), envobj *env);
void *call(closure *c, envobj *env);
envobj *liftint(int a); 
list *liftlist(list *l, ssize_t s); 
void envobj_free(void *);
void closure_free(void *);

//tracers: what an envobj / a closure keeps alive
void envobj_trace(void *obj, void (*visit)(void *)); 
void closure_trace(void *obj, void (*visit)(void *)); 
void closure_register_tracers(void); 

//allocate a raw value and register it with the collector
void *lift(size_t size); 

#endif 
