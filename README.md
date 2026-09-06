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

# Review results and TODO with LLM (Kimi-K3)
 全面 review 完成（gc.h / gc.c 共约 600 行，已通读 + 构建验证）。测试现状：57 项断言全部通过，包括 -fsanitize=address,undefined 和
 -O2 -fomit-frame-pointer。

 总体评价

 作为一个"在 C 里做到 Boehm 式 tracing GC"的库，当前的标记-清除核心（root/tracer/worklist/cycle 收集）是正确的，保守扫描的安全哲学
 （"宁可泄漏、不可悬垂"）也是对的，测试覆盖在这个规模下算不错。但从生产实践标准看，有几处不能忽略的隐患——它们不会让测试失败，但会在
 真实负载下咬人。

 严重问题（生产前必须处理）

 ### 1. gc_collect_roots() 是 API 陷阱（gc.c）

 这是最大的设计风险。gc.h 写着"only the roots you registered explicitly"，但常驻对象如果只存在普通 C 局部变量里（没有 gc_root()），
 调用这个函数会被静默释放。

 典型事故模式：

 ```c
   list *global_view = compute_something();  // 只靠局部变量活着
   // ...某个深调用栈里有人调了 gc_collect_roots()...
   *global_view;  // 释放后使用，或数据损坏
 ```

 Boehm GC 也会扫栈，原因就在于 C 的语义——你没法区分"一个指针真的只是局部变量"和"一个指针就是程序的根"。建议：要么删掉这个函数（只留
 gc_collect()），要么改名 + 注释强调"unsafe unless every live object is explicitly rooted"。

 ### 2. closure.c 里的 own-destructor 泄漏（closure.c:91-99）

 envobj_free 和 closure_free 目前制式化的调用 free(c)，但不释放 payload。这没错（因为 payload 走 gc_alloc/追踪），但容易误导后续维护
 者以为"destructor = 所有权终点"。更大风险是：如果以后有人给 STANDARD 注册"自定义 destructor"忘了调用 free(obj) 本体，就成了沉默泄漏
 ——gc_stats 甚至不会上升，因为是系统层泄漏。

 至少要在 closure.h / gc.h 明确注释："destructor 只负责释放自身，不 touch 已追踪的 children"。

 ### 3. 全局状态与线程（gc.c:70-86）

 static gc _gc; 是单线程前提。没有 mutex、没有 atomic、没有 thread-local。这在 README 里没有特别明确指出，但生产环境一旦有人从
 worker thread 调 gc_alloc()，就会出现 ref 链表撕裂/哈希表竞争。

 建议：
 - 在 gc.h 头部和 README 里明确写 "single-threaded only"。
 - 或者（如果要生产化）至少加一把 pthread_mutex_t（Linux-only 也可以接受）保护 _gc 的写路径；栈扫描本身就锁死了多线程标记的复杂方案
   ，互斥 + STW（stop-all-threads）是唯一现实的路线。

 ### 4. TYPE 是硬编码的 enum，限制了通用性（gc.h）

 当前你写死了 LIST | ENVOBJ | CLOSURE | STANDARD，而且 object_size(TYPE type) 里靠 switch case 硬编码这些 type 的大小。生产级 GC 不
 应该知道 "closure" 或 "list" 这种业务类型。

 改进方向：
 - gc_register_type(const char *name, size_t size, trace_fn, destructor)  → 返回一个 int type id。
 - 这样 functionalC 的头部只是用户而不是所有者。

 这不仅是清洁问题——destructor_table 和 trace_table 已经是 runtime table，再加一步就彻底解耦了。

 ### 5. gc_object_count() 和 gc_bytes() 代价高（gc.c:417, 428）

 现在它们每次调用都遍历整个 ref 链表。生产环境如果想做 "every N allocations or when bytes > X" 的启发式 GC 触发，这两个接口不能是
 O(n)。

 修法：
 - 维护 _gc.total_bytes 和 _gc.total_atoms 增量计数（注册时加、释放时减）→ 全成 O(1)。
 - 这也是实现"自动 GC 触发"的前提。

 中等问题（建议处理）

 ### 6. gc_pop(obj) 是 O(n) 且语义可疑（gc.c:248-259）

 栈式 root（push/pop）通常应该 LIFO——gc_pop_n(1)。当前 gc_pop(ptr) 是"删除任意匹配项"，这可能导致你 pop 掉一个之前压入的同名指针，而
 不是最近的。如果有嵌套调用 gc_push(a); gc_push(a); gc_pop(a); 它会删除最早的而不是最近的，然后第二次 gc_pop(a) 才删除最近的。虽然实
 际正确性取决于你的用法，但这个行为既是 O(n)，又不直观。

 建议： 统一使用 gc_push(obj) / gc_pop_n(k) 配对的 LIFO 语义。gc_pop(obj) 可以保留作为兼容 API，但注释清楚"移除最旧匹配项"。

 ### 7. frame_walk_top 的脆弱性（gc.c:196-220）

 你已经在用 frame pointer hack 确定扫描上限了，但这依赖：
 - 编译器不生成 -fomit-frame-pointer（你测试过了，能工作）
 - 但如果有间接调用 / setjmp / 汇编 / 某些优化把 fp 链弄断，就会 fallback 到扫描整个 2MB slack。

 这个 slack 扫描实际上是默认的保底，其实 OK，但值得注释说明 "scan 区域上限 2MB，深调用链可能截断根部"。

 另外，frame_walk_top 的 magic numbers（1024 步、1u << 20）可以被用户覆盖宏会更有用。

 ### 8. 内存压力时 GC 完全不自愈

 gc_alloc() 在 OOM 时直接 exit(1)；它甚至在 exit 前都不先 gc_collect_roots() 尝试释放可回收对象。生产环境会更喜欢：

 ```c
   void *p = malloc(size);
   if (p == NULL) {
     gc_collect();  // 试一次
     p = malloc(size);
     if (p == NULL) exit(1);  // 再不行才能死
   }
 ```

 同样，没有"分配 N 字节后自动 collect"的阈值——全靠用户显式调用。这是可用性，不是正确性，但生产环境会期望至少有 hook。

 设计确认（做对了的部分）

 - 显式 worklist：200k 深度递归能过，这是关键。
 - gc_alloc 里 memset(p, 0, size)：这个细节非常聪明，防止了栈扫描时看到未初始化垃圾里的随机指针。值得在 gc.h 里强调这是契约的一部分
   （"所有 gc 对象必须零初始化"）。
 - 监听 ASan 并自动关栈扫描：这是正确的工程主义，伪指针和 poison 区绝不兼容。
 - 保持 gc_register 幂等 + O(1)：哈希表 lookup 是必需的改进。

 建议的生产化路线（按优先级）

 1. 立即：文档明确线程模型；删除/重命名 gc_collect_roots()；gc.h 明确 "destructor contract"。
 2. 短期内：*_count/bytes → O(1)；gc_alloc OOM 前先试 GC；删除硬编码 type enum，改为 register-type API。
 3. 可选：gc_push/pop 严格 LIFO；frame_walk_top 步数做成宏可配；gc_stats 输出到 FILE* 以便日志集成。
 4. 长期（如果要真正多线程）：互斥 + stop-the-world，而不是当前的单线程假设。

 最后

 这套东西现在是一个很不错的教学/demo 级 tracing GC，代码干净，测试覆盖到位。"生产实践"的瓶颈不是它正确不正确，而是 ① 线程模型 ② 对未
 显式 root 的对象的隐式释放 ③ 通用 type 注册 API。这三点修了，配合 Boehm 级别的文档警告，就是合格的轻量级生产 GC 了。
 
License
=======

Copyright (c) 2013 Charles Cary

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

