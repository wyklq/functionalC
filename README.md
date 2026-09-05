functionalC
===========

Not because it is good, but because we can... 

(Current blog: https://charlescary.com/)

e.g. Don't you always wish that you could write like this when you are programming in C?

```c
int
main(int argc, char **argv) {
  gc_init(); //initialize the garbage collector
  list_register_tracer();
  closure_register_tracers(); //how to walk our types

  iter(map(range(0, 10), dbl,NULL), printint, NULL);
  iter(filter(range(0, 10), odd, NULL), printint, NULL); 

  gc_collect(); //nothing from those two lines is reachable any more
  gc_stats();   //...so it is all gone

  //Darker magic?  Not really...
  closure *addtwo = bind(NULL, add, liftint(2));
  closure *addten = bind(NULL, add, liftint(10));
  gc_root(addtwo);
  gc_root(addten); //these live in locals, so root them

  printf("%d\n", *(int *)call(addtwo, liftint(3)));
  printf("%d\n", *(int *)call(addten, liftint(3)));

  //all together now, with pseudo types everywhere woopie!!!
  list *vars = liftlist(range(0, 10), sizeof(int));
  gc_root(vars);
  gc_collect();
  printf("vars is still there: %d\n", *(int *)((envobj *)vars->val)->val);

  list *res = lmap(vars, addtwo);
  gc_push(res); //keep the result alive until we are done with it
  iter(res, printint, NULL);

  gc_collect(); //anything left?  only what is rooted

  gc_unroot(vars);
  gc_unroot(addtwo);
  gc_unroot(addten);
  gc_collect(); //now it is all gone

  gc_destroy(); //free whatever is left
  exit(0);
}

```

No?  Neither did I.  This is horrible.  

Functional C Programming Doc
============================

# The Basics

Our basic type is the list.

```c
struct list {
  void *val;
  list *next;
};
```

All of our functions use this type.  There are helper functions to work with lists like newitem, copyitem, and append.  Look at the code.

Some standard functional programming functions:

```c
//iterates through a list calling fn
void
iter(list *l, void (*fn)(void *, void *), void *args)
```
```c
//yeah, it's map
list *
map(list *l, void *(*fn)(void *, void *), void *args)
```

```c
//lifted map is so that you can map closures; maybe we'll unify the type system later...
list *
lmap(list *l, closure *cl)
```

```c
//yup, and filter too
list *
filter(list *l, bool (*fn)(void *, void *), void *args)
```

```c
//for fun
list *
range(int start, int end)
```

# Closures

Closures are built around two types: a closure and an environment variable

```c
struct closure {
  void *(*fn)(list *);
  list *env;
};

struct envobj {
  void *val;
  ssize_t size;
};
```

A closure is a function that is bound to an environment.  To bind an environment variable to a closure, use the bind function.

```c
closure *
bind(closure *c, void *(*fn)(list *), envobj *env);
```

To call this closed function, we use the call function:

```c
void *
call(closure *c, envobj *env);
```

To make environment variables, we need to lift our types into the environment.  

```c
//returns a lifted integer
envobj *
liftint(int a);
```

```c
//this transforms a list into an environment
list *
liftlist(list *l, ssize_t s) 
```

# Garbage Collector

The collector in `gc.c` is a **tracing** (mark & sweep), non-moving, non-compacting collector.  It tracks every object you allocate through it and frees whatever it can no longer reach.

It used to be a flat "free everything I was told to free" list.  It is now a real collector:

* every object allocated by the library (`newitem`, `liftint`, `range`, ...) **is** tracked
* objects know how to find the objects they reference, so a live list keeps its payload and its tail alive, a closure keeps its environment, ...
* it collects **cycles**, which reference counting never can
* unreachable objects die on their own, so you rarely have to free anything by hand
* lookups are O(1) (a hash table), so registering 20k objects is not 20k²

## The three rules

**1. Allocate through the collector.**

```c
//malloc + register in one step; zero filled, exits on OOM
void *gc_alloc(size_t size, TYPE type);

//or track something you allocated yourself
void gc_register(void *obj, TYPE type);
```

`lift(size)` is a shorthand for `gc_alloc(size, STANDARD)` and is what `dbl()`, `add()` and `range()` use for their integer payloads.

**2. Tell it how your type points at other objects** (once, at start-up):

```c
void list_trace(void *obj, void (*visit)(void *)) {
  list *l = obj;
  visit(l->val);   //the payload
  visit(l->next);  //the rest of the list
}
gc_register_tracer(LIST, list_trace);
```

`visit()` ignores anything that is not a tracked object, so hand it whatever you like.  The library ships tracers for `LIST`, `ENVOBJ` and `CLOSURE`; call `list_register_tracer()` and `closure_register_tracers()` in `main()`.

**3. Say what is alive, then collect.**  Three kinds of root:

```c
gc_root(obj);   //permanent: survives every collection until gc_unroot()
gc_push(obj);   //scoped: protected until gc_pop(obj) / gc_pop_n(n)
                //plain C locals count too (see "stack scanning" below)
gc_collect();
```

Everything reachable from a root survives; everything else is freed.

```c
list *vars = liftlist(range(0, 10), sizeof(int));
gc_root(vars);
gc_collect();                    //vars and all 10 ints survive
gc_unroot(vars);
gc_collect();                    //now they are gone
```

## API

```c
//life cycle
void gc_init(void);                       //call this first
void gc_destroy(void);                    //free everything that is left

//allocation
void *gc_alloc(size_t size, TYPE type);   //malloc + register
void gc_register(void *obj, TYPE type);   //idempotent, O(1)
void gc_remove(void *obj);                //stop tracking (does not free)

//types
void gc_register_destructor(TYPE, void (*)(void *));   //defaults to free()
void gc_register_tracer(TYPE, gc_trace_fn);            //how to find children

//roots
void gc_root(void *obj);
void gc_unroot(void *obj);
void gc_push(void *obj);
void gc_pop(void *obj);
void gc_pop_n(size_t n);

//collection
void gc_collect(void);                    //mark & sweep, stack included
void gc_collect_roots(void);              //only your explicit roots
void gc_set_stack_scanning(bool enabled);

//introspection
void gc_print(void);
void gc_stats(void);
bool gc_is_tracked(void *obj);
bool gc_is_rooted(void *obj);
size_t gc_object_count(void);
size_t gc_bytes(void);
```

Supported types (add your own to `enum TYPE` in `gc.h`, bump `TYPE_COUNT`, then register a destructor and a tracer):

```c
enum TYPE {
  LIST,
  ENVOBJ,
  CLOSURE,
  STANDARD //gc's an generic obj
};
```

## Stack scanning

`gc_collect()` also scans the C stack conservatively: it walks the stack word by word and treats every word that happens to be the address of a tracked object as a root.  That is what lets a plain local protect its objects with no rooting at all.

Two things worth knowing:

* **It can only over-approximate liveness.**  A stale stack slot from a call that already returned may keep a dead object alive.  That is a leak, never a dangling pointer — the safe way to be wrong.  Only the frames of live calls are scanned (the collector walks the frame pointer chain to find where they end), which keeps this rare.
* **It is disabled under AddressSanitizer.**  ASan poisons frame redzones and dead frames, which is exactly what a conservative scan reads.  Root explicitly with `gc_root`/`gc_push` if you want the same guarantees in an ASan build.

Turn it off with `gc_set_stack_scanning(false)`, or use `gc_collect_roots()` to ignore the stack for one collection.

## Tests

```sh
make check    #builds and runs gctest
```

`gctest.c` covers tracking, sweeping, tracing, cycles, roots, idempotent registration, a 200k-deep list (marking is iterative, so it cannot overflow the C stack) and the functional layer.  `make check` passes clean under `-fsanitize=address,undefined`.

# Issue fixed with LLM
* 已修复并提交的问题

 1. heap-buffer-overflow in filter (f71a268) — filter 错误地将 int* payload 传给 copyitem，导致越界读取
 2. Makefile 依赖和编译标志 (f71a268) — main.o 未使用 CFLAGS，头文件依赖缺失
 3. concat NULL 崩溃 (660c694) — concat(NULL, t) 会解引用空指针
 4. call NULL 保护 (01cd95b) — call(NULL, ...) 现在返回 NULL 而不是崩溃
 5. gc_mark/gc_unmark 缺失对象处理 (9d74271) — 当对象不在列表中时，会损坏链表
 6. remove_ 函数 NULL 解引用和逻辑问题* (5508832) — 简化并修复了脆弱的尾节点特殊处理逻辑
 7. list_free 过时注释 (b3767c2) — 更正了注释以反映实际行为
 8. gc_register 重复注册 (ce00026) — 添加去重检查，防止 double-free
 9. printint 和 odd 清理 (089dcdb) — 移除冗余转换，明确负数处理
 10. .gitignore (a4cbe58) — 忽略构建产物
 11. GC 重写为 tracing (mark & sweep) 收集器 — 见下

## GC 重写

原来的收集器只是一个"释放所有未标记对象"的链表：它不跟踪 `range()`/`dbl()`/`add()`
里 malloc 出来的 int payload（泄漏 57 处 / 228 字节），不知道对象之间的引用关系，
也无法处理循环引用。现在：

 * 所有库内分配（`newitem`/`copyitem`/`liftint`/`range`/`bind`/...）都走 `gc_alloc()`，全部被跟踪
 * 每种类型注册 tracer（`list_trace`/`envobj_trace`/`closure_trace`），
   存活的 list 会保住它的 payload 和后续节点，closure 会保住它的环境
 * 真正的 mark & sweep：不可达对象自动回收，**可以回收循环引用**
 * 三种 root：永久（`gc_root`）、作用域（`gc_push`/`gc_pop`）、以及保守式 C 栈扫描
   （普通局部变量自动成为 root）
 * 用哈希表做 ptr->ref 查找，把原来的 O(n) `is_registered()` 变成 O(1)，
   注册 2 万个对象不再是 O(n²)
 * 标记用显式 worklist，20 万层深的链表也不会撑爆 C 栈
 * ASan 下自动关闭栈扫描（保守扫描读原始栈字，正是 ASan 要抓的东西）

验证：`make check` 运行 `gctest.c`（57 项断言，覆盖跟踪/回收/ tracing/循环引用/
root/重复注册/深链表/函数式接口），在 `-fsanitize=address,undefined` 下同样全绿；
demo 程序的泄漏从 228 字节降到 0。


 所有修复都经过编译验证和 AddressSanitizer 测试。程序运行正常，只剩下预期的内存泄漏（GC 本身就不完整）。

License
=======

Copyright (c) 2013 Charles Cary

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

