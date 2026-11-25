# Linux 内核并发与同步机制分析

## 目录

1. [概述](#1-概述)
2. [原子操作](#2-原子操作)
   - 2.1 [atomic_t 与 atomic_long_t](#21-atomic_t-与-atomic_long_t)
   - 2.2 [原子操作的内存序](#22-原子操作的内存序)
   - 2.3 [原子位操作](#23-原子位操作)
3. [自旋锁](#3-自旋锁)
   - 3.1 [raw_spinlock_t 原始自旋锁](#31-raw_spinlock_t-原始自旋锁)
   - 3.2 [spinlock_t 自旋锁](#32-spinlock_t-自旋锁)
   - 3.3 [PREEMPT_RT 下的自旋锁](#33-preempt_rt-下的自旋锁)
   - 3.4 [读写自旋锁 rwlock_t](#34-读写自旋锁-rwlock_t)
   - 3.5 [MCS 锁与 qspinlock 排队自旋锁](#35-mcs-锁与-qspinlock-排队自旋锁)
   - 3.6 [qrwlock 排队读写锁](#36-qrwlock-排队读写锁)
4. [互斥锁](#4-互斥锁)
   - 4.1 [struct mutex](#41-struct-mutex)
   - 4.2 [mutex 自适应自旋](#42-mutex-自适应自旋)
   - 4.3 [OSQ 乐观自旋队列](#43-osq-乐观自旋队列)
   - 4.4 [PREEMPT_RT 下的 mutex](#44-preempt_rt-下的-mutex)
5. [RT 互斥锁与优先级继承](#5-rt-互斥锁与优先级继承)
   - 5.1 [struct rt_mutex_base](#51-struct-rt_mutex_base)
   - 5.2 [优先级继承机制](#52-优先级继承机制)
   - 5.3 [rtmutex 在 PREEMPT_RT 中的核心作用](#53-rtmutex-在-preempt_rt-中的核心作用)
6. [读写信号量 rw_semaphore](#6-读写信号量-rw_semaphore)
   - 6.1 [struct rw_semaphore](#61-struct-rw_semaphore)
   - 6.2 [乐观自旋优化](#62-乐观自旋优化)
   - 6.3 [PREEMPT_RT 下的 rwsem](#63-preempt_rt-下的-rwsem)
7. [信号量 semaphore](#7-信号量-semaphore)
8. [RCU 机制](#8-rcu-机制)
   - 8.1 [RCU 核心原理](#81-rcu-核心原理)
   - 8.2 [RCU 核心 API](#82-rcu-核心-api)
   - 8.3 [Tree RCU 实现](#83-tree-rcu-实现)
   - 8.4 [SRCU 可睡眠 RCU](#84-srcu-可睡眠-rcu)
   - 8.5 [Tasks RCU](#85-tasks-rcu)
   - 8.6 [RCU 与 PREEMPT_RT](#86-rcu-与-preempt_rt)
9. [顺序锁 seqlock](#9-顺序锁-seqlock)
   - 9.1 [seqcount_t 基础类型](#91-seqcount_t-基础类型)
   - 9.2 [seqlock_t 与 spinlock 组合](#92-seqlock_t-与-spinlock-组合)
10. [Per-CPU 变量](#10-per-cpu-变量)
11. [完成量 completion](#11-完成量-completion)
12. [等待队列 wait_queue](#12-等待队列-wait_queue)
13. [本地锁 local_lock](#13-本地锁-local_lock)
14. [Per-CPU RWSEM](#14-per-cpu-rwsem)
15. [Lockdep 锁验证](#15-lockdep-锁验证)
    - 15.1 [锁类与依赖图](#151-锁类与依赖图)
    - 15.2 [死锁检测类型](#152-死锁检测类型)
16. [内存屏障](#16-内存屏障)
    - 16.1 [屏障类型](#161-屏障类型)
    - 16.2 [内核屏障 API](#162-内核屏障-api)
17. [Guard 作用域管理](#17-guard-作用域管理)
18. [PREEMPT_RT 同步机制总结](#18-preempt_rt-同步机制总结)
19. [附录：关键文件列表](#19-附录关键文件列表)

---

## 1. 概述

> **详细分析文档：** [01-overview.md](./01-overview.md)

Linux 内核运行在多处理器（SMP）环境中，需要处理来自多个 CPU 的并发访问，以及中断上下文、软中断（softirq）、tasklet、进程上下文等多级嵌套。内核提供了一套层次化的同步机制，从底层的原子操作到高层的 RCU，覆盖不同的并发场景。

**同步机制层次结构：**

```
┌────────────────────────────────────────────────────────────┐
│                    应用层 / 系统调用                        │
├────────────────────────────────────────────────────────────┤
│  Guard/Scoped API  │  Completions  │  Wait Queues          │
├────────────────────────────────────────────────────────────┤
│  RCU  │  Seqlock  │  Per-CPU  │  Local Lock              │
├───────────────────┬────────────────────────────────────────┤
│  Mutex  │  RWSEM  │  RT-Mutex  │  Semaphore  │  percpu-   │
│         │         │  (PI)      │             │  rwsem     │
├───────────────────┴────────────────────────────────────────┤
│  Spinlock  │  Raw Spinlock  │  RWlock  │  MCS/OSQ Lock    │
│  (qspinlock)│  (qspinlock)  │ (qrwlock)│                   │
├────────────────────────────────────────────────────────────┤
│  Atomic Operations (atomic_t, atomic_long_t, bitops)       │
├────────────────────────────────────────────────────────────┤
│  硬件层面: 内存屏障 (DMB/DSB/ISB)  │  Cache Coherency     │
└────────────────────────────────────────────────────────────┘
```

**同步机制选择指南：**

| 场景 | 推荐机制 | 说明 |
|------|---------|------|
| 简单计数器 | `atomic_t` | 无锁、无阻塞 |
| 短临界区、中断上下文 | `spinlock_t` / `raw_spinlock_t` | 自旋等待 |
| 长临界区、可睡眠 | `mutex` | 阻塞、可睡眠 |
| 读多写少 | `rwlock_t` / `rwsem` / `RCU` | 读端可并发 |
| 读极多写极少 | `RCU` | 读者无锁开销 |
| 大量读、偶发写、可容忍重试 | `seqlock_t` | 读者无锁，写者互斥 |
| 单 CPU 数据 | `per-CPU` 变量 | 完全无锁 |
| 任务间同步 | `completion` / `wait_queue` | 事件驱动 |
| 调试死锁 | `LOCKDEP` | 运行时验证 |

---

## 2. 原子操作

> **详细分析文档：** [02-atomic-ops.md](./02-atomic-ops.md)

原子操作是内核同步机制的最底层基础，无需锁即可安全地执行"读-改-写"操作。

### 2.1 atomic_t 与 atomic_long_t

定义在 [include/linux/types.h](file:///home/louis/code/linux/include/linux/types.h) 中。

```c
// include/linux/types.h
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

**核心 API 定义在 [include/linux/atomic.h](file:///home/louis/code/linux/include/linux/atomic.h)：**

```c
// 读/写
int atomic_read(const atomic_t *v);
void atomic_set(atomic_t *v, int i);

// 原子加减
void atomic_add(int i, atomic_t *v);
void atomic_sub(int i, atomic_t *v);
void atomic_inc(atomic_t *v);
void atomic_dec(atomic_t *v);

// 原子操作并返回结果
int atomic_add_return(int i, atomic_t *v);
int atomic_sub_return(int i, atomic_t *v);
int atomic_inc_return(atomic_t *v);
int atomic_dec_return(atomic_t *v);

// 原子条件操作
int atomic_cmpxchg(atomic_t *v, int old, int new);
int atomic_xchg(atomic_t *v, int new);
int atomic_try_cmpxchg(atomic_t *v, int *old, int new);

// 原子位操作
int atomic_fetch_add(int i, atomic_t *v);
int atomic_fetch_sub(int i, atomic_t *v);
int atomic_fetch_and(int i, atomic_t *v);
int atomic_fetch_or(int i, atomic_t *v);
int atomic_fetch_xor(int i, atomic_t *v);
```

### 2.2 原子操作的内存序

内核提供四种内存序后缀（定义在 [include/linux/atomic.h](file:///home/louis/code/linux/include/linux/atomic.h)）：

```c
// 1. 完全有序（默认，无后缀）：同时提供 ACQUIRE + RELEASE 语义
atomic_add_return(i, v);

// 2. Acquire：之后的读操作不能重排到之前
atomic_add_return_acquire(i, v);

// 3. Release：之前的写操作不能重排到之后
atomic_add_return_release(i, v);

// 4. Relaxed：无排序保证
atomic_add_return_relaxed(i, v);
```

**架构无关的原子操作实现框架：**

```
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
```

### 2.3 原子位操作

定义在 [include/linux/bitops.h](file:///home/louis/code/linux/include/linux/bitops.h) 中：

```c
void set_bit(unsigned int nr, volatile unsigned long *addr);     // 原子置位
void clear_bit(unsigned int nr, volatile unsigned long *addr);   // 原子清位
void change_bit(unsigned int nr, volatile unsigned long *addr);  // 原子翻转
int test_and_set_bit(unsigned int nr, volatile unsigned long *addr);
int test_and_clear_bit(unsigned int nr, volatile unsigned long *addr);
int test_and_change_bit(unsigned int nr, volatile unsigned long *addr);
```

---

## 3. 自旋锁

> **详细分析文档：** [03-spinlock.md](./03-spinlock.md)

自旋锁是最基本的锁机制，在等待锁释放时忙等（spin）。适用于临界区非常短的场景。

### 3.1 raw_spinlock_t 原始自旋锁

定义在 [include/linux/spinlock_types_raw.h](file:///home/louis/code/linux/include/linux/spinlock_types_raw.h)：

```c
// include/linux/spinlock_types_raw.h
context_lock_struct(raw_spinlock) {
    arch_spinlock_t raw_lock;           // 架构相关实现
#ifdef CONFIG_DEBUG_SPINLOCK
    unsigned int magic, owner_cpu;      // 调试信息
    void *owner;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map dep_map;         // Lockdep 依赖图
#endif
};
```

**核心 API（[include/linux/spinlock.h](file:///home/louis/code/linux/include/linux/spinlock.h)）：**

```c
// 初始化
raw_spin_lock_init(lock);

// 基本操作
raw_spin_lock(lock);                    // 获取锁
raw_spin_unlock(lock);                  // 释放锁
raw_spin_trylock(lock);                 // 尝试获取，失败返回 0

// 变体：关闭中断
raw_spin_lock_irq(lock);                // 获取锁 + 关闭本地中断
raw_spin_unlock_irq(lock);              // 释放锁 + 开启本地中断
raw_spin_lock_irqsave(lock, flags);     // 获取锁 + 保存并关闭中断
raw_spin_unlock_irqrestore(lock, flags);// 释放锁 + 恢复中断

// 变体：关闭软中断
raw_spin_lock_bh(lock);                 // 获取锁 + 关闭 softirq
raw_spin_unlock_bh(lock);               // 释放锁 + 开启 softirq
```

**raw_spinlock 是"最底层"的自旋锁，在 PREEMPT_RT 下也不会被替换为可睡眠锁。** 它用于：
- 中断处理程序中
- 调度器核心代码中
- 任何不允许睡眠的原子上下文中

### 3.2 spinlock_t 自旋锁

定义在 [include/linux/spinlock_types.h](file:///home/louis/code/linux/include/linux/spinlock_types.h)：

```c
// 非 PREEMPT_RT：spinlock_t 直接包装 raw_spinlock_t
#ifndef CONFIG_PREEMPT_RT

context_lock_struct(spinlock) {
    union {
        struct raw_spinlock rlock;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
        struct {
            u8 __padding[LOCK_PADSIZE];
            struct lockdep_map dep_map;
        };
#endif
    };
};

// 非 RT 下，spin_lock() 直接映射为 raw_spin_lock()
static __always_inline void spin_lock(spinlock_t *lock)
{
    raw_spin_lock(&lock->rlock);
}

#else  /* CONFIG_PREEMPT_RT */
// RT 下，spinlock_t 基于 rt_mutex（见下文 3.3 节）
#endif
```

### 3.3 PREEMPT_RT 下的自旋锁

在 PREEMPT_RT 内核中，`spinlock_t` 被替换为基于 `rt_mutex` 的可睡眠锁，但 `raw_spinlock_t` 保持不变。

实现文件 [kernel/locking/spinlock_rt.c](file:///home/louis/code/linux/kernel/locking/spinlock_rt.c)：

```c
// PREEMPT_RT 下的 spinlock_t 定义
context_lock_struct(spinlock) {
    struct rt_mutex_base    lock;       // 基于 rt_mutex
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map      dep_map;
#endif
};

// RT 下 spin_lock 的实现
static __always_inline void __rt_spin_lock(spinlock_t *lock)
{
    rtlock_might_resched();              // 检查是否需要调度
    rtlock_lock(&lock->lock);            // 获取 rt_mutex
    rcu_read_lock();                     // 模拟 spin_lock 的 RCU 保护
    migrate_disable();                   // 禁止迁移
}

void __sched rt_spin_lock(spinlock_t *lock)
{
    spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
    __rt_spin_lock(lock);
}
```

**关键差异：**

| 特性 | 非 RT | PREEMPT_RT |
|------|-------|------------|
| spinlock_t 实现 | 基于 qspinlock/arch_spinlock | 基于 rt_mutex |
| 等待行为 | 自旋（忙等） | 可睡眠（阻塞） |
| 抢占禁用 | 锁定期间禁用抢占 | 不禁用抢占 |
| 迁移禁用 | 通过禁用抢占隐式实现 | 显式 `migrate_disable()` |
| RCU 读端 | 隐式保护 | 显式 `rcu_read_lock()` |
| 适用上下文 | 进程/softirq/硬中断 | 仅进程上下文 |

### 3.4 读写自旋锁 rwlock_t

定义在 [include/linux/rwlock_types.h](file:///home/louis/code/linux/include/linux/rwlock_types.h)：

```c
// 非 PREEMPT_RT 定义
#ifndef CONFIG_PREEMPT_RT
context_lock_struct(rwlock) {
    arch_rwlock_t raw_lock;
#ifdef CONFIG_DEBUG_SPINLOCK
    unsigned int magic, owner_cpu;
    void *owner;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map dep_map;
#endif
};

#else  /* CONFIG_PREEMPT_RT */
// RT 下基于 rwbase_rt
context_lock_struct(rwlock) {
    struct rwbase_rt    rwbase;
    atomic_t            readers;
};
#endif
```

**API：**

```c
// 读端
void read_lock(rwlock_t *lock);
void read_unlock(rwlock_t *lock);
void read_lock_irqsave(rwlock_t *lock, unsigned long flags);
void read_unlock_irqrestore(rwlock_t *lock, unsigned long flags);

// 写端
void write_lock(rwlock_t *lock);
void write_unlock(rwlock_t *lock);
void write_lock_irqsave(rwlock_t *lock, unsigned long flags);
void write_unlock_irqrestore(rwlock_t *lock, unsigned long flags);
```

### 3.5 MCS 锁与 qspinlock 排队自旋锁

MCS（Mellor-Crummey and Scott）锁是一种公平的排队自旋锁。每个 CPU 在本地变量上自旋，避免了传统自旋锁的缓存行乒乓问题。

**MCS 锁节点定义（[kernel/locking/mcs_spinlock.h](file:///home/louis/code/linux/kernel/locking/mcs_spinlock.h)）：**

```c
struct mcs_spinlock {
    struct mcs_spinlock *next;      // 链表中的下一个节点
    int locked;                     // 1 = 锁已获取
    int count;                      // 嵌套计数
};
```

**qspinlock（Queued Spinlock）实现（[kernel/locking/qspinlock.c](file:///home/louis/code/linux/kernel/locking/qspinlock.c)）：**

qspinlock 将 MCS 锁的思想压缩到 4 字节的 `arch_spinlock_t` 中：

```
┌─────────────────────────────────────────────────────┐
│                  arch_spinlock_t (32-bit)            │
├──────────┬──────────────────────────────────────────┤
│   lock   │                  tail                    │
│  (1 byte)│              (3 bytes)                   │
├──────────┼──────────────────────────────────────────┤
│  锁持有者│  尾部 CPU 编号 + 嵌套层                  │
│  标志位  │  (编码了等待队列中的最后一个 CPU)        │
└──────────┴──────────────────────────────────────────┘
```

**qspinlock 工作原理：**

```
lock() 调用:
  1. 尝试 atomic_compare_and_exchange() 获取锁
  2. 成功 → 锁已获取，直接返回
  3. 失败 → 进入排队路径
     a. 编码当前 CPU 到 tail 字段
     b. 将自己的节点加入等待队列
     c. 在本地节点上 spin (MCS 风格)

unlock() 调用:
  1. 如果 tail 指向自己 → 无竞争者，清除锁位即可
  2. 如果有等待者 → 设置下一个等待者的 locked 位
```

### 3.6 qrwlock 排队读写锁

qrwlock（Queued Read/Write Lock）实现（[kernel/locking/qrwlock.c](file:///home/louis/code/linux/kernel/locking/qrwlock.c)）：

```c
// include/asm-generic/qrwlock_types.h
typedef struct qrwlock {
    union {
        atomic_t cnts;          // 32-bit 计数器
        struct {
#ifdef __LITTLE_ENDIAN
            u8 wlocked;         // 写锁定标志
            u8 __lstate[3];
#else
            u8 __lstate[3];
            u8 wlocked;
#endif
        };
    };
    arch_spinlock_t wait_lock;  // 等待队列保护锁
} arch_rwlock_t;
```

**工作原理：**

```
读锁获取:
  1. atomic_add(_QR_BIAS, &cnts)  — 增加读计数
  2. 检查 wlocked 位，若为 0 则获取成功
  3. 若写锁定，则进入 slowpath 等待

写锁获取:
  1. atomic_cmpxchg(&cnts, 0, _QW_LOCKED)  — 尝试获取
  2. 失败则进入 slowpath
  3. 在 slowpath 中设置 wlocked 标志
  4. 等待所有读者释放
  5. 获取 wait_lock 保护，排队等待
```

---

## 4. 互斥锁

Mutex 是可睡眠的互斥锁，适用于临界区较长的场景。当锁被持有时，等待者会睡眠而不是自旋。

### 4.1 struct mutex

定义在 [include/linux/mutex_types.h](file:///home/louis/code/linux/include/linux/mutex_types.h)：

```c
// 非 PREEMPT_RT 定义
#ifndef CONFIG_PREEMPT_RT

context_lock_struct(mutex) {
    atomic_long_t           owner;          // 锁持有者（含标志位）
    raw_spinlock_t          wait_lock;      // 保护 wait_list 的自旋锁
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
    struct optimistic_spin_queue osq;       // 乐观自旋队列
#endif
    struct list_head        wait_list;      // 等待者链表
#ifdef CONFIG_DEBUG_MUTEXES
    void                    *magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map      dep_map;
#endif
};

#else  /* CONFIG_PREEMPT_RT */
// PREEMPT_RT 下 mutex 基于 rt_mutex
#include <linux/rtmutex.h>
#endif
```

**owner 字段编码：**

```
┌─────────────────────────────────────────────────────┐
│              atomic_long_t owner                    │
├─────────────────────────────────────────────────────┤
│ 位 [63:3]  │  位 [2]  │  位 [1]  │  位 [0]         │
│  task_struct  │  HAS_DOWN  │  HANDOFF  │  WAITERS   │
│  指针         │  读端锁定  │  移交标志  │  等待者标志 │
└──────────────┴───────────┴──────────┴──────────────┘
```

### 4.2 mutex 自适应自旋

Mutex 实现了自适应自旋（adaptive spinning）优化，实现在 [kernel/locking/mutex.c](file:///home/louis/code/linux/kernel/locking/mutex.c)：

```
mutex_lock() 调用链:
  1. __mutex_fastpath_lock()  → 尝试快速路径（atomic cmpxchg）
  2. 失败 → __mutex_lock_slowpath()
     a. mutex_optimistic_spin()  → 乐观自旋
        │
        ├── osq_lock()  → 加入乐观自旋队列
        │
        └── while (true):
            ├── 如果持有者正在运行 → 自旋等待（MCS 风格）
            ├── 如果持有者已睡眠 → 退出自旋
            └── 如果锁被释放 → 尝试获取
     b. 自旋超时 → 进入睡眠等待
        ├── 加入 wait_list
        ├── set_current_state(TASK_UNINTERRUPTIBLE)
        └── schedule()  → 调度出去
```

**自适应自旋的条件：**

```c
// 只有当持有者正在 CPU 上运行时才自旋
static inline bool mutex_spin_on_owner(struct mutex *lock,
                                       struct task_struct *owner)
{
    // 检查持有者是否仍在运行
    // 如果持有者被调度出去，停止自旋立即睡眠
    while (true) {
        if (__mutex_owner(lock) != owner)
            return false;       // 锁被释放或易手
        if (!owner_on_cpu(owner))
            return false;       // 持有者不在 CPU 上
        cpu_relax();
    }
}
```

### 4.3 OSQ 乐观自旋队列

OSQ（Optimistic Spinning Queue）是 MCS 锁的变体，专门为可睡眠锁（mutex、rwsem）的乐观自旋优化。

定义在 [kernel/locking/osq_lock.c](file:///home/louis/code/linux/kernel/locking/osq_lock.c)：

```c
struct optimistic_spin_node {
    struct optimistic_spin_node *next, *prev;
    int locked;     // 1 if lock acquired
    int cpu;        // 编码的 CPU 编号
};

// 每个 CPU 一个节点
static DEFINE_PER_CPU_SHARED_ALIGNED(
    struct optimistic_spin_node, osq_node);
```

### 4.4 PREEMPT_RT 下的 mutex

在 PREEMPT_RT 下，`struct mutex` 直接映射到 `struct rt_mutex`，实现优先级继承：

```c
// include/linux/mutex_types.h (CONFIG_PREEMPT_RT)
#include <linux/rtmutex.h>

struct mutex {
    struct rt_mutex_base    lock;       // rt_mutex 替代
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map      dep_map;
#endif
};
```

---

## 5. RT 互斥锁与优先级继承

RT-mutex 是支持优先级继承（Priority Inheritance）的互斥锁，用于解决优先级反转问题。

### 5.1 struct rt_mutex_base

定义在 [include/linux/rtmutex.h](file:///home/louis/code/linux/include/linux/rtmutex.h)：

```c
struct rt_mutex_base {
    raw_spinlock_t          wait_lock;      // 保护等待队列
    struct rb_root_cached   waiters;        // 红黑树等待队列（按优先级排序）
    struct task_struct      *owner;         // 当前持有者（含标志位）
};
```

**owner 字段编码：**

```
┌─────────────────────────────────────────────────────┐
│              struct task_struct *owner              │
├─────────────────────────────────────────────────────┤
│ 位 [63:2]  │  位 [1]  │  位 [0]                     │
│  指针       │  BLOCKED  │  PENDING                  │
│            │  阻塞标志  │  挂起标志                  │
└────────────┴───────────┴────────────────────────────┘
```

### 5.2 优先级继承机制

实现于 [kernel/locking/rtmutex.c](file:///home/louis/code/linux/kernel/locking/rtmutex.c)：

```
优先级反转问题:
  低优先级任务 L 持有锁
  高优先级任务 H 等待锁（被阻塞）
  中优先级任务 M 抢占 L（M 不需要锁，但优先级高于 L）
  → H 被间接阻塞，优先级反转

优先级继承解决:
  当 H 等待 L 持有的锁时:
  1. L 临时继承 H 的优先级（priority inheritance）
  2. L 以高优先级运行，快速完成临界区
  3. L 释放锁后恢复原始优先级
  4. H 获得锁继续执行
```

**优先级继承调用链：**

```
rt_mutex_setprio()  [kernel/sched/core.c]
  │
  ├── task->prio = newprio          → 更新持有者优先级
  ├── task->normal_prio = newprio
  └── enqueue_task() / dequeue_task() → 重新放入调度队列
      │
      └── 如果持有者也在等待高优先级锁:
          └── 递归向上传播优先级继承
              └── rt_mutex_adjust_prio_chain()
```

### 5.3 rtmutex 在 PREEMPT_RT 中的核心作用

在 PREEMPT_RT 内核中，rtmutex 是多种同步机制的基础：

```
PREEMPT_RT 锁体系:
  ┌──────────────────────────────────────────────────┐
  │                    rtmutex                       │
  ├──────────────────────────────────────────────────┤
  │                                                    │
  ├── spinlock_t (spinlock_rt.c)                      │
  │     └── struct rt_mutex_base lock                 │
  │                                                    │
  ├── rwlock_t (rwbase_rt.c)                          │
  │     └── struct rwbase_rt { struct rt_mutex_base }  │
  │                                                    │
  ├── mutex (mutex_types.h)                           │
  │     └── struct rt_mutex_base lock                 │
  │                                                    │
  └── rw_semaphore (rwsem.c)                          │
        └── struct rwbase_rt { struct rt_mutex_base }  │
  └──────────────────────────────────────────────────┘
```

---

## 6. 读写信号量 rw_semaphore

> **详细分析文档：** [06-rwsem.md](./06-rwsem.md)

RWSEM 是一种允许多个读者并发访问、写者独占访问的同步机制。

### 6.1 struct rw_semaphore

定义在 [include/linux/rwsem.h](file:///home/louis/code/linux/include/linux/rwsem.h)：

```c
// 非 PREEMPT_RT 定义
#ifndef CONFIG_PREEMPT_RT

context_lock_struct(rw_semaphore) {
    atomic_long_t count;          // 读写计数（含状态位）
    atomic_long_t owner;          // 持有者（含标志位）
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
    struct optimistic_spin_queue osq;  // 乐观自旋队列
#endif
    raw_spinlock_t wait_lock;     // 保护 wait_list
    struct list_head wait_list;   // 等待队列
#ifdef CONFIG_DEBUG_RWSEMS
    void *magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map dep_map;
#endif
};

#else  /* CONFIG_PREEMPT_RT */
// PREEMPT_RT 下基于 rwbase_rt
#include <linux/rwbase_rt.h>
#endif
```

**count 字段编码：**

```
┌─────────────────────────────────────────────────────┐
│              atomic_long_t count                    │
├─────────────────────────────────────────────────────┤
│  位 [63:1]  │  位 [0]                               │
│  读者计数   │  WRITER_LOCKED (写锁定标志)           │
│  (每读 +1)  │                                       │
└────────────┴────────────────────────────────────────┘
```

### 6.2 乐观自旋优化

RWSEM 实现了写者乐观自旋，当写者发现持有者正在 CPU 上运行时，会在本地自旋等待而不是立即睡眠：

```
down_write() 慢路径:
  1. rwsem_down_write_slowpath()
     ├── osq_lock()  → 加入乐观自旋队列
     ├── rwsem_optimistic_spin()
     │   ├── 检查持有者是否在 CPU 上运行
     │   └── 是在自旋等待，否则进入睡眠
     └── schedule()  → 加入 wait_list 睡眠
```

### 6.3 PREEMPT_RT 下的 rwsem

在 PREEMPT_RT 下，rwsem 基于 `rwbase_rt` 实现（[kernel/locking/rwbase_rt.c](file:///home/louis/code/linux/kernel/locking/rwbase_rt.c)）：

```c
// 核心设计思路
/*
 * down_write/write_lock()
 *  1) Lock rtmutex
 *  2) Remove the reader BIAS to force readers into the slow path
 *  3) Wait until all readers have left the critical section
 *  4) Mark it write locked
 *
 * down_read/read_lock()
 *  1) Try fast path acquisition (reader BIAS is set)
 *  2) Take rtmutex::wait_lock, which protects the writelocked flag
 *  3) If !writelocked, acquire it for read
 *  4) If writelocked, block on rtmutex
 *  5) Unlock rtmutex, goto 1)
 */
```

---

## 7. 信号量 semaphore

> **详细分析文档：** [07-semaphore.md](./07-semaphore.md)

信号量是内核中较老的同步机制，实现于 [kernel/locking/semaphore.c](file:///home/louis/code/linux/kernel/locking/semaphore.c)：

```c
// include/linux/semaphore.h
struct semaphore {
    raw_spinlock_t      lock;       // 保护 count 和 wait_list
    unsigned int        count;      // 可用资源计数
    struct list_head    wait_list;  // 等待队列
};
```

**API：**

```c
void down(struct semaphore *sem);              // P 操作（获取），可睡眠
int down_interruptible(struct semaphore *sem);  // 可被信号中断
int down_trylock(struct semaphore *sem);        // 尝试获取，不睡眠
void up(struct semaphore *sem);                 // V 操作（释放）
```

> **注意：** 新代码应优先使用 mutex 而非 semaphore，除非需要计数语义。

---

## 8. RCU 机制

RCU（Read-Copy Update）是一种针对读多写少场景的高性能同步机制，读者无需任何锁开销。

### 8.1 RCU 核心原理

```
RCU 核心思想:
  读者: 进入临界区 → 读数据 → 退出临界区（无锁，无阻塞）
  写者: 复制数据 → 修改副本 → 更新指针 → 等待宽限期 → 释放旧数据

宽限期 (Grace Period):
  所有在读的读者完成退出后的一段时期
  写者必须等待宽限期结束才能释放旧数据
```

**RCU 宽限期示意图：**

```
CPU 0: ──RCU读───RCU读───────RCU读───
          │      │              │
CPU 1:    ──RCU读───RCU读──────────────
          │         │              │
CPU 2:    ──syn───[移除指针]────[等待GP]───[释放旧数据]──
          │                    │
          └── 宽限期 (GP) ─────┘
               所有 CPU 的 RCU 读端退出后结束
```

### 8.2 RCU 核心 API

定义在 [include/linux/rcupdate.h](file:///home/louis/code/linux/include/linux/rcupdate.h)：

```c
// 读者端 - 极低开销
void rcu_read_lock(void);           // 进入 RCU 读端临界区
void rcu_read_unlock(void);         // 退出 RCU 读端临界区

// 在 non-PREEMPT 内核中，rcu_read_lock() 仅仅是禁止抢占
// 在 PREEMPT 内核中，会启用/禁用抢占计数

// 写者端 - 更新操作
void synchronize_rcu(void);         // 同步等待宽限期结束
void call_rcu(struct rcu_head *head, rcu_callback_t func);
                                    // 异步：注册回调，宽限期后调用
void rcu_barrier(void);             // 等待所有 call_rcu 回调完成

// 指针发布
#define rcu_assign_pointer(p, v)    // 安全更新 RCU 保护的指针
#define rcu_dereference(p)          // 安全读取 RCU 保护的指针
```

### 8.3 Tree RCU 实现

Tree RCU 是内核默认的 RCU 实现，适用于大型 SMP 系统，文件在 [kernel/rcu/tree.c](file:///home/louis/code/linux/kernel/rcu/tree.c)：

**核心数据结构：**

```c
// kernel/rcu/tree.h
struct rcu_node {
    raw_spinlock_t __private lock;  // 保护本级节点
    unsigned long gp_seq;           // 宽限期序列号
    unsigned long qsmask;           // 还需等待的 CPU 位图
    unsigned long qsmaskinit;       // 在线 CPU 位图
    struct rcu_node *parent;        // 父节点（树形结构）
};

struct rcu_data {
    unsigned long gp_seq;           // 本 CPU 感知的宽限期
    unsigned long completed;        // 已完成宽限期数
    struct rcu_head *nocb_head;     // NOCB 回调头
    bool cpu_no_qs;                 // 本 CPU 未完成静默状态
};
```

**Tree RCU 节点层次结构：**

```
               ┌──────────────┐
               │  root_node   │
               │  (level 0)   │
               └──────┬───────┘
                     /    \
           ┌────────┐    ┌────────┐
           │  node  │    │  node  │
           │ (lev1) │    │ (lev1) │
           └───┬────┘    └───┬────┘
              /  \          /  \
          ┌───┐ ┌───┐  ┌───┐ ┌───┐
          │rdp│ │rdp│  │rdp│ │rdp│
          │CPU│ │CPU│  │CPU│ │CPU│
          │ 0 │ │ 1 │  │ 2 │ │ 3 │
          └───┘ └───┘  └───┘ └───┘
```

**宽限期推进流程：**

```
synchronize_rcu()  [kernel/rcu/update.c]
  │
  ├── rcu_gp_init()  → 启动新宽限期
  │     ├── 递增 gp_seq 序列号
  │     └── 遍历 rcu_node 树，设置 qsmask
  │
  ├── 等待所有 CPU 报告 quiescent state
  │     ├── 每个 CPU 在 tick、schedule()、用户态返回时检测
  │     └── rcu_report_qs_rdp() → 报告静默状态
  │
  └── rcu_gp_cleanup() → 宽限期结束
        └── 执行所有等待的回调
```

### 8.4 SRCU 可睡眠 RCU

SRCU（Sleepable RCU）允许读者在临界区内睡眠，适用于长读端场景：

```c
// include/linux/srcu.h
struct srcu_struct {
    unsigned long completed;
    struct srcu_array __percpu *per_cpu_ref;
    spinlock_t lock;
    // ...
};

// API
int srcu_read_lock(struct srcu_struct *ss);   // 返回索引
void srcu_read_unlock(struct srcu_struct *ss, int idx);
void synchronize_srcu(struct srcu_struct *ss);
void call_srcu(struct srcu_struct *ss, struct rcu_head *head,
               rcu_callback_t func);
```

### 8.5 Tasks RCU

Tasks RCU 用于跟踪 `do_exit()` 路径中的任务，确保在任务退出时 RCU 宽限期完成：

```c
// kernel/rcu/tasks.h
void synchronize_rcu_tasks(void);
void call_rcu_tasks(struct rcu_head *head, rcu_callback_t func);
```

### 8.6 RCU 与 PREEMPT_RT

在 PREEMPT_RT 下，RCU 的默认行为发生变化：

```c
// kernel/rcu/update.c
static int rcu_normal_after_boot = IS_ENABLED(CONFIG_PREEMPT_RT);

// PREEMPT_RT 下，默认使用普通宽限期（非加速模式）
// 避免在实时任务中长时间禁用抢占
```

---

## 9. 顺序锁 seqlock

Seqlock 是一种读者无锁、写者互斥的同步机制，适用于读多写少的场景。读者在读取后需要验证序列号是否变化。

### 9.1 seqcount_t 基础类型

定义在 [include/linux/seqlock.h](file:///home/louis/code/linux/include/linux/seqlock.h)：

```c
// include/linux/seqlock_types.h
typedef struct seqcount {
    unsigned sequence;              // 序列号，奇数 = 写者锁定
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map dep_map;
#endif
} seqcount_t;
```

**读者伪代码：**

```
do {
    seq = read_seqcount_begin(&seq);   // 读取序列号（偶数时有效）
    // 读取数据...
} while (read_seqcount_retry(&seq, seq));  // 检查序列号是否变化
```

**写者：**

```
write_seqcount_begin(&seq);   // sequence++ (变为奇数，锁定)
// 写数据...
write_seqcount_end(&seq);     // sequence++ (变为偶数，解锁)
```

### 9.2 seqlock_t 与 spinlock 组合

```c
// seqlock_t 包装了 seqcount 和 spinlock，写者串行化
typedef struct {
    struct seqcount seqcount;
    spinlock_t lock;
} seqlock_t;

// API
void write_seqlock(seqlock_t *sl);
void write_sequnlock(seqlock_t *sl);
unsigned read_seqbegin(const seqlock_t *sl);
bool read_seqretry(const seqlock_t *sl, unsigned start);
```

**典型应用场景：** `jiffies_64` 读取、`gettimeofday()` 的 `tk_core` 保护。

---

## 10. Per-CPU 变量

Per-CPU 变量为每个 CPU 提供独立的数据副本，避免缓存行乒乓，实现无锁的 CPU 本地数据访问。

定义在 [include/linux/percpu.h](file:///home/louis/code/linux/include/linux/percpu.h)：

```c
// 静态定义
DEFINE_PER_CPU(type, name);             // 定义 per-CPU 变量
DECLARE_PER_CPU(type, name);            // 声明 per-CPU 变量
DEFINE_PER_CPU_ALIGNED(type, name);     // 缓存行对齐

// 动态分配
void *alloc_percpu(type);               // 动态分配 per-CPU 变量
void free_percpu(void *__pdata);        // 释放

// 访问
get_cpu_var(name);                      // 获取当前 CPU 的变量（禁止抢占）
put_cpu_var(name);                      // 完成访问（允许抢占）
per_cpu_ptr(ptr, cpu);                  // 获取指定 CPU 的指针
this_cpu_ptr(ptr);                      // 获取当前 CPU 的指针

// 原子 per-CPU 操作
this_cpu_add(ptr, val);                 // 当前 CPU 原子加
this_cpu_inc(ptr);                      // 当前 CPU 原子自增
this_cpu_xchg(ptr, val);               // 当前 CPU 原子交换
```

**Per-CPU 变量的内存布局：**

```
┌──────────────┐  CPU 0 的副本
│  per-CPU 数据 │  ← this_cpu_ptr()
├──────────────┤
│  per-CPU 数据 │  CPU 1 的副本
├──────────────┤
│  per-CPU 数据 │  CPU 2 的副本
├──────────────┤
│     ...      │
└──────────────┘
  每份副本位于不同的缓存行，避免伪共享
```

---

## 11. 完成量 completion

> **详细分析文档：** [11-completion.md](./11-completion.md)

Completion 是轻量级的任务同步机制，用于一个任务等待另一个任务完成某事件。

定义在 [include/linux/completion.h](file:///home/louis/code/linux/include/linux/completion.h)：

```c
struct completion {
    unsigned int done;              // 完成计数
    struct swait_queue_head wait;   // 等待队列（simple waitqueue）
};

// 初始化
init_completion(struct completion *x);
DECLARE_COMPLETION(work);           // 静态声明

// 等待端
void wait_for_completion(struct completion *x);              // 不可中断等待
int wait_for_completion_interruptible(struct completion *x); // 可被信号中断
unsigned long wait_for_completion_timeout(struct completion *x,
                                          unsigned long timeout); // 超时等待
int wait_for_completion_killable(struct completion *x);     // 可被杀等待

// 完成端
void complete(struct completion *x);     // 唤醒一个等待者
void complete_all(struct completion *x); // 唤醒所有等待者
```

**典型使用模式：**

```
发起者:                             执行者:
┌─────────────────┐               ┌─────────────────┐
│ init_completion  │               │                  │
│ (&comp)          │               │  执行任务...      │
│ 启动任务         │──────────────►│                  │
│                  │               │  complete(&comp)  │
│ wait_for_completion│             │                  │
│ (&comp)          │◄──────────────│                  │
│ 继续执行         │               │                  │
└─────────────────┘               └─────────────────┘
```

---

## 12. 等待队列 wait_queue

> **详细分析文档：** [12-waitqueue.md](./12-waitqueue.md)

等待队列是 Linux 内核中最基本的唤醒机制，用于进程等待某个条件成立。

定义在 [include/linux/wait.h](file:///home/louis/code/linux/include/linux/wait.h)：

```c
struct wait_queue_head {
    spinlock_t      lock;           // 保护等待队列
    struct list_head head;          // 等待者链表
};

struct wait_queue_entry {
    unsigned int        flags;      // WQ_FLAG_* 标志
    void                *private;   // 通常指向 task_struct
    wait_queue_func_t   func;       // 唤醒函数
    struct list_head    entry;      // 链表节点
};
```

**核心 API：**

```c
// 初始化
DECLARE_WAIT_QUEUE_HEAD(name);      // 静态声明
init_waitqueue_head(q);             // 动态初始化

// 等待
wait_event(wq, condition);           // 不可中断等待
wait_event_interruptible(wq, cond);  // 可中断等待
wait_event_timeout(wq, cond, to);    // 超时等待
wait_event_killable(wq, cond);       // 可被杀等待

// 唤醒
wake_up(wq);                         // 唤醒 TASK_NORMAL 状态的任务
wake_up_interruptible(wq);           // 唤醒 TASK_INTERRUPTIBLE 的任务
wake_up_all(wq);                     // 唤醒所有等待者
```

**等待队列工作原理：**

```
wait_event_interruptible(wq, condition):
  │
  ├── if (condition)  → 条件已满足，直接返回
  │
  └── while (!condition):
        ├── DEFINE_WAIT(__wait)         → 创建 wait_queue_entry
        ├── add_wait_queue(&wq, &__wait) → 加入等待队列
        ├── set_current_state(TASK_INTERRUPTIBLE) → 设置状态
        ├── if (condition)  → 检查条件（防止信号丢失）
        │     └── break
        ├── schedule()                  → 调度出去
        └── remove_wait_queue(&wq, &__wait) → 移除等待队列

wake_up_interruptible(wq):
  │
  └── 遍历等待队列:
        └── default_wake_function()
              └── try_to_wake_up()  → 唤醒目标进程
```

---

## 13. 本地锁 local_lock

local_lock 是 per-CPU 的轻量级锁，确保临界区在同一个 CPU 上执行，不会迁移。

定义在 [include/linux/local_lock.h](file:///home/louis/code/linux/include/linux/local_lock.h)：

```c
// 声明
DEFINE_LOCAL_LOCK(lock);            // 静态定义

// API
local_lock(lock);                    // 加锁（禁止迁移）
local_unlock(lock);                  // 解锁（允许迁移）
local_lock_irq(lock);                // 加锁 + 关中断
local_unlock_irq(lock);              // 解锁 + 开中断
local_lock_irqsave(lock, flags);     // 加锁 + 保存中断
local_unlock_irqrestore(lock, flags);
```

**实现原理：**

```c
// 非 PREEMPT_RT:
#define local_lock(lock)    preempt_disable()   // 仅禁止抢占

// PREEMPT_RT:
#define local_lock(lock)    spin_lock(lock)     // 基于 spinlock
```

---

## 14. Per-CPU RWSEM

percpu-rwsem 是一种针对读多写少场景优化的读写信号量，读者使用 per-CPU 计数器实现无锁读。

实现于 [kernel/locking/percpu-rwsem.c](file:///home/louis/code/linux/kernel/locking/percpu-rwsem.c)：

```c
// include/linux/percpu-rwsem.h
struct percpu_rw_semaphore {
    struct rcu_sync       rss;          // RCU 同步
    int __percpu          *read_count;  // per-CPU 读计数
    struct rcuwait        writer;       // 写者等待
    wait_queue_head_t     waiters;      // 等待者队列
    atomic_t              block;        // 写者阻塞标志
};
```

**工作原理：**

```
读者:
  percpu_down_read():
    1. this_cpu_inc(read_count)    → 本地计数 +1
    2. smp_mb()                    → 内存屏障
    3. if (!block)  → 成功获取读锁，立即返回
    4. 否则 → 慢路径等待

写者:
  percpu_down_write():
    1. rcu_sync_enter()            → 等待所有 RCU 读端
    2. atomic_set(block, 1)         → 设置阻塞标志
    3. synchronize_rcu()            → 等待宽限期
    4. for_each_online_cpu(cpu):
         wait for read_count[cpu] == 0  → 等待所有读者退出
```

---

## 15. Lockdep 锁验证

Lockdep 是内核的运行时锁依赖验证器，在运行时检测死锁风险。

### 15.1 锁类与依赖图

实现于 [kernel/locking/lockdep.c](file:///home/louis/code/linux/kernel/locking/lockdep.c)：

```c
// 锁类（每个锁对象对应一个类）
struct lock_class {
    struct list_head        hash_entry;
    struct list_head        lock_entry;
    struct lockdep_subclass  subclass;
    unsigned long           usage_mask;       // 锁使用场景位图
    struct stack_trace      name;             // 获取点回溯
    unsigned long           version;
    // ...
};

// 锁依赖边
struct lock_list {
    struct list_head        entry;
    struct lock_class       *class;
    struct lock_trace       trace;
    int                     distance;
    // ...
};
```

**Lockdep 检测的锁顺序：**

```
Lockdep 构建有向图:
  锁 A → 锁 B (表示在持有 A 的情况下获取 B)

  检测循环依赖:
  如果图中存在 A → B → C → A，则报告死锁风险

  检测场景:
  - 软硬中断嵌套: hardirq-safe / hardirq-unsafe 检测
  - 递归锁检测
  - 锁持有时间分析
```

### 15.2 死锁检测类型

```c
// 1. 递归锁检测
// 同一任务重复获取同一锁
--------------------------------------------
[  INFO: possible recursive locking detected  ]
--------------------------------------------

// 2. 锁反转检测
// 两个任务以不同顺序获取锁
--------------------------------------------
[  INFO: possible circular locking dependency detected  ]
--------------------------------------------

// 3. 中断上下文不安全
// 在可能被中断上下文获取的锁又去获取中断上下文持有的锁
--------------------------------------------
[  INFO: hardirq-safe -> hardirq-unsafe lock order  ]
--------------------------------------------
```

---

## 16. 内存屏障

内存屏障用于控制 CPU 和编译器对内存访问的排序，是多处理器环境下同步机制的基础。

### 16.1 屏障类型

```
┌─────────────────────────────────────────────────────┐
│                  屏障类型                            │
├──────────┬──────────────────────────────────────────┤
│  类型     │  效果                                    │
├──────────┼──────────────────────────────────────────┤
│ ACQUIRE  │ 之后的读写操作不能重排到屏障前            │
│ RELEASE  │ 之前的读写操作不能重排到屏障后            │
│ FULL     │ 所有操作不能跨越屏障重排                  │
│ 读屏障   │ 之前的读操作在之后的读前完成              │
│ 写屏障   │ 之前的写操作在之后的写前完成              │
│ 编译器   │ 防止编译器优化重排（不影响硬件）          │
└──────────┴──────────────────────────────────────────┘
```

### 16.2 内核屏障 API

```c
// 通用屏障
smp_mb();                           // SMP 全屏障
smp_rmb();                          // SMP 读屏障
smp_wmb();                          // SMP 写屏障
smp_mb__before_atomic();            // 原子操作前屏障
smp_mb__after_atomic();             // 原子操作后屏障

// ACQUIRE / RELEASE 语义
smp_load_acquire(p);                // ACQUIRE 读
smp_store_release(p, v);            // RELEASE 写

// 编译器屏障
barrier();                          // 防止编译器重排

// DMA/设备内存屏障
mmiowb();                           // PCI I/O 写屏障
dma_rmb();                          // DMA 读屏障
dma_wmb();                          // DMA 写屏障
```

---

## 17. Guard 作用域管理

> **详细分析文档：** [17-guard-scope.md](./17-guard-scope.md)

内核提供基于 `__attribute__((cleanup))` 的 Guard API，自动管理锁的生命周期。

定义在 [include/linux/cleanup.h](file:///home/louis/code/linux/include/linux/cleanup.h)：

```c
// 作用域锁
DEFINE_LOCK_GUARD_1(spinlock, spinlock_t,
                    spin_lock(_T->lock),
                    spin_unlock(_T->lock));

DEFINE_LOCK_GUARD_1_COND(spinlock, _try, spin_trylock(_T->lock));

// 使用示例
void example(void)
{
    // 进入作用域自动 spin_lock，离开自动 spin_unlock
    guard(spinlock)(&my_lock);
    // 临界区代码...
    // 无需手动解锁
}

// 条件锁定
void example_try(void)
{
    // 尝试获取锁，失败则返回
    guard(spinlock_try)(&my_lock) ?: return -EBUSY;
    // 临界区代码...
}

// scoped_guard: 显式作用域
void example_scope(void)
{
    // 前一半代码无需锁
    {
        scoped_guard(spinlock, &my_lock) {
            // 临界区代码
            // 离开此作用域自动解锁
        }
    }
    // 后一半代码无需锁
}
```

**支持的 Guard 类型：**

```c
// 各类锁的 Guard 定义
DEFINE_LOCK_GUARD_1(raw_spinlock, raw_spinlock_t, ...);
DEFINE_LOCK_GUARD_1(mutex, struct mutex, ...);
DEFINE_LOCK_GUARD_1(rwsem_read, struct rw_semaphore, ...);
DEFINE_LOCK_GUARD_1(rwsem_write, struct rw_semaphore, ...);
DEFINE_LOCK_GUARD_1(rcu, void, rcu_read_lock(), rcu_read_unlock());
```

---

## 18. PREEMPT_RT 同步机制总结

PREEMPT_RT 对内核同步机制进行了全面的改造，核心原则是将不可抢占的临界区可抢占化。

**PREEMPT_RT 锁替换对照表：**

| 非 RT 机制 | PREEMPT_RT 实现 | 行为变化 |
|-----------|----------------|---------|
| `spinlock_t` | rt_mutex（可睡眠） | 自旋→阻塞睡眠 |
| `rwlock_t` | rwbase_rt（基于 rt_mutex） | 自旋→阻塞睡眠 |
| `mutex` | rt_mutex | 行为基本一致 |
| `rwsem` | rwbase_rt（基于 rt_mutex） | 非 writer-fair |
| `raw_spinlock_t` | 不变 | 仍为自旋锁 |
| `local_lock` | spinlock_t | 非 RT 仅为 preempt_disable |

**PREEMPT_RT 下的锁体系架构：**

```
PREEMPT_RT
  │
  ├── raw_spinlock_t  ──── 不变，中断上下文仍使用
  │
  ├── spinlock_t      ──── rt_mutex → 可睡眠
  ├── rwlock_t        ──── rwbase_rt → 可睡眠
  ├── mutex           ──── rt_mutex → 优先级继承
  ├── rwsem           ──── rwbase_rt → 可睡眠
  │
  ├── local_lock      ──── spinlock_t → 可睡眠
  │
  └── RCU:
        ├── rcu_read_lock()  → 显式跟踪（不再禁抢占）
        ├── synchronize_rcu() → 默认非加速模式
        └── SRCU → 不变
```

**PREEMPT_RT 锁的嵌套规则：**

```
raw_spinlock_t 可以在任何上下文中持有
spinlock_t 不能在 raw_spinlock_t 临界区内持有（RT 下会导致死锁）

正确嵌套顺序:
  raw_spin_lock() → spin_lock()     ✓
  spin_lock() → raw_spin_lock()     ✗ (RT 下禁止)
```

---

## 19. 附录：关键文件列表

| 文件 | 说明 |
|------|------|
| [include/linux/atomic.h](file:///home/louis/code/linux/include/linux/atomic.h) | 原子操作 API |
| [include/linux/spinlock.h](file:///home/louis/code/linux/include/linux/spinlock.h) | 自旋锁 API |
| [include/linux/spinlock_types.h](file:///home/louis/code/linux/include/linux/spinlock_types.h) | 自旋锁数据结构 |
| [include/linux/spinlock_types_raw.h](file:///home/louis/code/linux/include/linux/spinlock_types_raw.h) | raw_spinlock_t 数据结构 |
| [include/linux/mutex.h](file:///home/louis/code/linux/include/linux/mutex.h) | 互斥锁 API |
| [include/linux/mutex_types.h](file:///home/louis/code/linux/include/linux/mutex_types.h) | mutex 数据结构 |
| [include/linux/rwsem.h](file:///home/louis/code/linux/include/linux/rwsem.h) | 读写信号量 API |
| [include/linux/rcupdate.h](file:///home/louis/code/linux/include/linux/rcupdate.h) | RCU 核心 API |
| [include/linux/seqlock.h](file:///home/louis/code/linux/include/linux/seqlock.h) | 顺序锁 |
| [include/linux/wait.h](file:///home/louis/code/linux/include/linux/wait.h) | 等待队列 |
| [include/linux/completion.h](file:///home/louis/code/linux/include/linux/completion.h) | 完成量 |
| [include/linux/percpu.h](file:///home/louis/code/linux/include/linux/percpu.h) | Per-CPU 变量 |
| [include/linux/local_lock.h](file:///home/louis/code/linux/include/linux/local_lock.h) | 本地锁 |
| [include/linux/percpu-rwsem.h](file:///home/louis/code/linux/include/linux/percpu-rwsem.h) | Per-CPU 读写信号量 |
| [include/linux/cleanup.h](file:///home/louis/code/linux/include/linux/cleanup.h) | Guard 作用域管理 |
| [include/linux/rtmutex.h](file:///home/louis/code/linux/include/linux/rtmutex.h) | RT-mutex 定义 |
| [include/linux/rwbase_rt.h](file:///home/louis/code/linux/include/linux/rwbase_rt.h) | RT 读写锁基础 |
| [include/linux/lockdep_types.h](file:///home/louis/code/linux/include/linux/lockdep_types.h) | Lockdep 类型定义 |
| [kernel/locking/spinlock.c](file:///home/louis/code/linux/kernel/locking/spinlock.c) | 自旋锁实现 |
| [kernel/locking/spinlock_rt.c](file:///home/louis/code/linux/kernel/locking/spinlock_rt.c) | PREEMPT_RT 自旋锁实现 |
| [kernel/locking/mutex.c](file:///home/louis/code/linux/kernel/locking/mutex.c) | 互斥锁实现 |
| [kernel/locking/rtmutex.c](file:///home/louis/code/linux/kernel/locking/rtmutex.c) | RT-mutex 实现（优先级继承） |
| [kernel/locking/rwsem.c](file:///home/louis/code/linux/kernel/locking/rwsem.c) | 读写信号量实现 |
| [kernel/locking/rwbase_rt.c](file:///home/louis/code/linux/kernel/locking/rwbase_rt.c) | PREEMPT_RT 读写锁实现 |
| [kernel/locking/qspinlock.c](file:///home/louis/code/linux/kernel/locking/qspinlock.c) | 排队自旋锁 |
| [kernel/locking/qrwlock.c](file:///home/louis/code/linux/kernel/locking/qrwlock.c) | 排队读写锁 |
| [kernel/locking/osq_lock.c](file:///home/louis/code/linux/kernel/locking/osq_lock.c) | 乐观自旋队列 |
| [kernel/locking/mcs_spinlock.h](file:///home/louis/code/linux/kernel/locking/mcs_spinlock.h) | MCS 锁头文件 |
| [kernel/locking/percpu-rwsem.c](file:///home/louis/code/linux/kernel/locking/percpu-rwsem.c) | Per-CPU RWSEM 实现 |
| [kernel/locking/semaphore.c](file:///home/louis/code/linux/kernel/locking/semaphore.c) | 信号量实现 |
| [kernel/locking/lockdep.c](file:///home/louis/code/linux/kernel/locking/lockdep.c) | Lockdep 锁验证器 |
| [kernel/rcu/tree.c](file:///home/louis/code/linux/kernel/rcu/tree.c) | Tree RCU 实现 |
| [kernel/rcu/srcutree.c](file:///home/louis/code/linux/kernel/rcu/srcutree.c) | SRCU 实现 |
| [kernel/rcu/tasks.h](file:///home/louis/code/linux/kernel/rcu/tasks.h) | Tasks RCU 实现 |
| [kernel/rcu/update.c](file:///home/louis/code/linux/kernel/rcu/update.c) | RCU 更新端 API |