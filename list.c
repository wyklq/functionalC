#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "list.h"
#include "gc.h"

list *
newitem(void *v) {
  list *o = gc_alloc(sizeof(list), LIST);
  o->val = v;
  o->next = NULL;
  return o;
}

list *
copyitem(list *i) {
  if (i == NULL) {
    return NULL;
  }
  //copyitem is a node allocator: register it, just like newitem
  list *o = gc_alloc(sizeof(list), LIST);
  o->val = i->val;
  o->next = NULL;
  return o;
}

list *
append(list *l, void *v) {
  list *ni = newitem(v);
  if (l == NULL) {
    return ni;
  }
  list *curr;
  for (curr = l; curr->next != NULL; curr = curr->next)
    ;
  curr->next = ni;
  return l;
}

list *
concat(list *h, list *t) {
  if (h == NULL) {
    return t;
  }
  list *curr;
  for (curr = h; curr->next != NULL; curr = curr->next)
	  ;
  curr->next = t;
	return h;	
}

//shallow copy
list *
copy(list *l) {
  list *o = NULL;
  list *curr;
  for (curr = l; curr != NULL; curr = curr->next) {
    o = append(o, curr->val);
  }
  return o; 
}

//frees a single list node; the payload it points to is not freed here
//(the payload is tracked separately by the garbage collector)
void
list_free(void *_l) {
  list *l = _l;
  free(l); 
}

//a list keeps two things alive: its payload and the rest of the list
void
list_trace(void *_obj, void (*visit)(void *)) {
  list *l = _obj;
  if (l == NULL) {
    return;
  }
  visit(l->val);
  visit(l->next);
}

void
list_register_tracer(void) {
  gc_register_tracer(LIST, list_trace);
}


