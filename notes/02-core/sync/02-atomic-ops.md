# 2. 原子操作

## 2.1 概述

原子操作是内核同步机制的最底层基础，由 CPU 硬件指令保证操作的"不可分割性"。在多处理器环境中，原子操作用于实现无锁的计数器、标志位和简单状态机。

**核心特性：**
- 不可中断：操作在执行过程中不会被其他 CPU 或线程干扰
- 内存序控制：支持不同级别的内存排序语义
- 架构无关：内核提供统一的 API，各架构通过汇编实现

## 2.2 关键数据结构

### 2.2.1 atomic_t

定义在 [include/linux/types.h](file:///home/louis/code/linux/include/linux/types.h)：

```c
typedef struct {
    int counter;
} atomic_t;

typedef struct {
    s64 counter;
} atomic64_t;

#ifdef CONFIG_64BIT
typedef atomic64_t atomic_long_t;
#else
typedef atomic_t atomic_long_t;
#endif
```

### 2.2.2 atomic_t 的内存布局

```
atomic_t (32-bit):
┌─────────────────────────────────────────────────┐
│                  counter                         │
│                 (int, 32-bit)                    │
└─────────────────────────────────────────────────┘
  位 31                                 位 0

atomic64_t (64-bit):
┌─────────────────────────────────────────────────┐
│                  counter                         │
│                (s64, 64-bit)                     │
└─────────────────────────────────────────────────┘
  位 63                                 位 0
```

## 2.3 核心 API

### 2.3.1 基本读写

```c
// include/linux/atomic.h

// 读操作 (返回 counter 的值)
int atomic_read(const atomic_t *v);

// 写操作 (设置 counter 的值)
void atomic_set(atomic_t *v, int i);
```

### 2.3.2 原子算术操作

```c
// 无返回值
void atomic_add(int i, atomic_t *v);       // v->counter += i
void atomic_sub(int i, atomic_t *v);       // v->counter -= i
void atomic_inc(atomic_t *v);              // v->counter++
void atomic_dec(atomic_t *v);              // v->counter--

// 有返回值
int atomic_add_return(int i, atomic_t *v); // 加并返回新值
int atomic_sub_return(int i, atomic_t *v); // 减并返回新值
int atomic_inc_return(atomic_t *v);        // 自增并返回新值
int atomic_dec_return(atomic_t *v);        // 自减并返回新值

// 条件操作
int atomic_cmpxchg(atomic_t *v, int old, int new);  // CAS
int atomic_xchg(atomic_t *v, int new);               // 交换
int atomic_try_cmpxchg(atomic_t *v, int *old, int new); // 优化版 CAS
```

### 2.3.3 原子位操作

定义在 [include/linux/bitops.h](file:///home/louis/code/linux/include/linux/bitops.h)：

```c
void set_bit(unsigned int nr, volatile unsigned long *addr);
                                         // 置位: *addr |= (1 << nr)
void clear_bit(unsigned int nr, volatile unsigned long *addr);
                                         // 清位: *addr &= ~(1 << nr)
void change_bit(unsigned int nr, volatile unsigned long *addr);
                                         // 翻转: *addr ^= (1 << nr)

int test_and_set_bit(unsigned int nr, volatile unsigned long *addr);
int test_and_clear_bit(unsigned int nr, volatile unsigned long *addr);
int test_and_change_bit(unsigned int nr, volatile unsigned long *addr);
```

## 2.4 内存序变体

内核提供四种内存序后缀，定义在 [include/linux/atomic.h](file:///home/louis/code/linux/include/linux/atomic.h)：

### 2.4.1 四种变体

```c
// 1. 完全有序 (默认, 无后缀) — ACQUIRE + RELEASE 语义
atomic_add_return(i, v);

// 2. Acquire — 屏障后的读写不能重排到屏障前
atomic_add_return_acquire(i, v);
// 用于: 获取锁后读取共享数据

// 3. Release — 屏障前的读写不能重排到屏障后
atomic_add_return_release(i, v);
// 用于: 写入共享数据后释放锁

// 4. Relaxed — 无内存排序保证, 仅保证原子性
atomic_add_return_relaxed(i, v);
// 用于: 统计计数器 (不需要排序)
```

### 2.4.2 架构无关的实现框架

```c
// 内核通过宏组合实现不同内存序
#define __atomic_op_acquire(op, args...)                \
({                                                      \
    typeof(op##_relaxed(args)) __ret = op##_relaxed(args); \
    __atomic_acquire_fence();   /* 插入 acquire 屏障 */  \
    __ret;                                              \
})

#define __atomic_op_release(op, args...)                \
({                                                      \
    __atomic_release_fence();   /* 插入 release 屏障 */  \
    op##_relaxed(args);                                 \
})

#define __atomic_op_fence(op, args...)                  \
({                                                      \
    typeof(op##_relaxed(args)) __ret;                   \
    __atomic_pre_full_fence();                          \
    __ret = op##_relaxed(args);                         \
    __atomic_post_full_fence();                         \
    __ret;                                              \
})
```

## 2.5 ARM64 架构实现

ARM64 使用 `ldxr/stxr` 指令对实现原子操作：

```asm
// ARM64 atomic_add_return() 实现
// arch/arm64/include/asm/atomic.h

static inline int atomic_add_return(int i, atomic_t *v)
{
    unsigned long tmp;
    int result;

    asm volatile("// atomic_add_return\n"
"1:    ldxr    %w0, %2\n"          // 独占加载: result = *v
"       add     %w0, %w0, %w1\n"   // result += i
"       stlxr   %w3, %w0, %2\n"    // 独占存储: *v = result, 返回状态
"       cbnz    %w3, 1b\n"         // 存储失败则重试
"       dmb     ish\n"             // 数据内存屏障
    : "=&r" (result), "=&r" (tmp)
    : "Q" (v->counter), "Ir" (i)
    : "memory");

    return result;
}
```

**ARM64 原子操作硬件流程：**

```
CPU 执行 atomic_add_return():
  │
  ├── ldxr (Load Exclusive):
  │     从内存独占加载 counter 值
  │     在 CPU 的独占监视器中标记该地址
  │
  ├── add: 执行加法运算
  │
  ├── stlxr (Store Exclusive):
  │     尝试写入新值
  │     如果独占监视器未失效 → 写入成功, 返回 0
  │     如果独占监视器已失效 → 写入失败, 返回 1
  │       (其他 CPU 写入了同一地址)
  │
  ├── cbnz: 检查 stlxr 返回值
  │     如果失败 → 跳转到 1b 重试
  │     如果成功 → 继续执行
  │
  └── dmb ish: 数据内存屏障
       确保所有之前的存储操作对其他 CPU 可见
```

## 2.6 调用栈流程

### 2.6.1 atomic_inc_and_test 调用链

```
atomic_inc_and_test(v)
  │
  ├── 架构无关层 (include/linux/atomic.h)
  │     └── atomic_add_return(v, 1) → 返回新值
  │
  ├── 架构相关层 (arch/arm64/include/asm/atomic.h)
  │     └── ldxr/add/stlxx/cbnz 循环
  │
  └── 检查返回值
        └── return (atomic_add_return(v, 1) == 0)
```

### 2.6.2 典型使用场景

```c
// 引用计数 (内核中最常见的原子操作使用场景)
// 文件: include/linux/kref.h

struct kref {
    atomic_t refcount;
};

void kref_get(struct kref *kref)
{
    WARN_ON(!atomic_read(&kref->refcount));
    atomic_inc(&kref->refcount);
}

int kref_put(struct kref *kref, void (*release)(struct kref *kref))
{
    if (atomic_dec_and_test(&kref->refcount)) {
        release(kref);
        return 1;
    }
    return 0;
}
```

## 2.7 使用场景

| 场景 | 推荐 API | 说明 |
|------|---------|------|
| 引用计数 | `atomic_inc()/atomic_dec_and_test()` | kref, 文件引用 |
| 序列号生成 | `atomic_inc_return()` | 单调递增 ID |
| 统计计数器 | `atomic_inc()/atomic_add()` | 性能统计 |
| 忙等标志 | `atomic_read()/atomic_set()` | 状态标志 |
| 无锁栈/队列 | `atomic_cmpxchg()` | 无锁数据结构 |
| 位图操作 | `set_bit()/clear_bit()` | 资源分配位图 |
| 内存分配 | `test_and_set_bit()` | 物理页管理 |

## 2.8 使用注意事项

```c
// 错误用法: 非原子操作复合
// 以下代码在多 CPU 下不安全:
if (atomic_read(&v) == 0)     // 此处读取为 0
    atomic_set(&v, 1);        // 但另一 CPU 可能已修改

// 正确用法: 使用原子条件操作
int old = 0;
if (atomic_try_cmpxchg(&v, &old, 1))   // 仅当 v==0 时设为 1
    // 获取成功
```

## 2.9 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/atomic.h](file:///home/louis/code/linux/include/linux/atomic.h) | 原子操作 API 定义 |
| [include/linux/atomic-arch-fallback.h](file:///home/louis/code/linux/include/linux/atomic-arch-fallback.h) | 架构回退实现 |
| [include/linux/bitops.h](file:///home/louis/code/linux/include/linux/bitops.h) | 原子位操作 API |
| [arch/arm64/include/asm/atomic.h](file:///home/louis/code/linux/arch/arm64/include/asm/atomic.h) | ARM64 原子操作实现 |
| [include/linux/kref.h](file:///home/louis/code/linux/include/linux/kref.h) | 基于 atomic_t 的引用计数 |