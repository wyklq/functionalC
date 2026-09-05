#include <stdlib.h>
#include <stdio.h>
#include "list.h"
#include "functional.h"
#include "closure.h"
#include "gc.h"

// a function to play with iter
void
printint(void *v, void *args) {
  printf("%d\n", *(int *)v);
}

//returns true if int v is odd, false otherwise
bool
odd(void *v, void *args) {
  return *((int *)v) % 2 != 0;
}

//returns twice int v
//boy is this terrible
void *
dbl(void *v, void *args) {
  int *o = lift(sizeof(int));
  *o = *((int *)v) * 2;
  return o;
}

//we'll use this to play with closures
void *
add(list *l) {
  int *a = unbox(l);
  int *b = unbox(l->next);
  int *o = lift(sizeof(int));
  *o = *a + *b;
  return o; 
}

int
main(int argc, char **argv) {
  
  gc_init(); //initialize the garbage collector

  //tell the collector how to walk our types, so a live list keeps its
  //payload and its tail alive, a closure keeps its environment, ...
  list_register_tracer();
  closure_register_tracers();

  iter(map(range(0, 10), dbl,NULL), printint, NULL);
  iter(filter(range(0, 10), odd, NULL), printint, NULL); 
  
  //this is what the collector is for: 33 ints, 33 list nodes, ... none of
  //them reachable once the iter() call returns
  gc_collect();
  gc_stats();

  //Darker magic?  Not really...
  //these live in locals, so root them: an explicit root is correct with or
  //without stack scanning
  closure *addtwo = bind(NULL, add, liftint(2));
  closure *addten = bind(NULL, add, liftint(10));
  gc_root(addtwo);
  gc_root(addten);

  printf("%d\n", *(int *)call(addtwo, liftint(3)));
  printf("%d\n", *(int *)call(addten, liftint(3)));

  //all together now, with pseudo types everywhere woopie!!!
  list *vars = liftlist(range(0, 10), sizeof(int));

  //a rooted object, and everything it references, survives every collection
  gc_root(vars);
  gc_collect();
  printf("after collect, vars is still there: %d\n", *(int *)((envobj *)vars->val)->val);

  //a scoped root protects a temporary just long enough
  list *vars2 = liftlist(range(100, 102), sizeof(int));
  gc_push(vars2);
  gc_collect();
  printf("after collect, vars2 is still there: %d\n", *(int *)((envobj *)vars2->val)->val);
  gc_pop(vars2);
  gc_collect(); //vars2 is unprotected now, so it goes away

  list *res = lmap(vars, addtwo);
  gc_push(res); //keep the result alive until we are done printing it

  iter(res, printint, NULL);

  gc_print(); //show eveything currently in the garbage collector

  gc_collect(); //you can guess what this does

  gc_print(); //anything left?  only what is rooted / still reachable

  gc_stats();

  gc_unroot(vars);
  gc_unroot(addtwo);
  gc_unroot(addten);
  gc_collect(); //nothing is rooted any more, so it all goes away
  gc_stats();

  gc_destroy(); //free everything that is left

  exit(0);
}
