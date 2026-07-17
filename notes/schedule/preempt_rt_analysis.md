# PREEMPT_RT 实时抢占内核分析

> 本文档分析 CONFIG_PREEMPT_RT=y 开启后，对 Linux 内核各子系统的影响
> 基于当前项目代码的静态分析

## 目录

- [1. 概述](#1-概述)
- [2. 调度子系统中的 PREEMPT_RT 变化](#2-调度子系统中的-preempt_rt-变化)
  - [2.1 schedule_rtlock() —— RT 锁调度入口](#21-schedule_rtlock--rt-锁调度入口)
  - [2.2 TASK_RTLOCK_WAIT 任务状态](#22-task_rtlock_wait-任务状态)
  - [2.3 TTWU_QUEUE 特性关闭](#23-ttwu_queue-特性关闭)
  - [2.4 SCHED_NR_MIGRATE_BREAK 减小](#24-sched_nr_migrate_break-减小)
  - [2.5 动态抢占模式禁用](#25-动态抢占模式禁用)
- [3. 锁机制的变化](#3-锁机制的变化)
  - [3.1 spinlock_t → rt_mutex 基础](#31-spinlock_t--rt_mutex-基础)
  - [3.2 raw_spinlock_t 保持不变](#32-raw_spinlock_t-保持不变)
  - [3.3 rwlock_t → rwbase_rt](#33-rwlock_t--rwbase_rt)
  - [3.4 mutex → rt_mutex 基础](#34-mutex--rt_mutex-基础)
  - [3.5 local_lock 行为变化](#35-locallock-行为变化)
- [4. 中断与软中断的变化](#4-中断与软中断的变化)
  - [4.1 强制中断线程化 (force_irqthreads)](#41-强制中断线程化-force_irqthreads)
  - [4.2 软中断处理变化](#42-软中断处理变化)
  - [4.3 local_bh 禁用变化](#43-local_bh-禁用变化)
- [5. 高精度定时器 (hrtimer) 的变化](#5-高精度定时器-hrtimer-的变化)
  - [5.1 HRTIMER_MODE_HARD 区分](#51-hrtimermodehard-区分)
  - [5.2 hrtimer_cancel_wait_running](#52-hrtimercancelwaitrunning)
- [6. RCU 的变化](#6-rcu-的变化)
- [7. task_struct 中的 PREEMPT_RT 字段](#7-task_struct-中的-preempt_rt-字段)
- [8. 内存管理的变化](#8-内存管理的变化)
- [9. migrate_disable 机制](#9-migrate_disable-机制)
- [10. 调试与跟踪变化](#10-调试与跟踪变化)
- [11. 总结：PREEMPT_RT 内核架构变化总览](#11-总结preempt_rt-内核架构变化总览)

---

## 1. 概述

当前项目 `.config` 配置为：

```c
CONFIG_PREEMPT_BUILD=y
CONFIG_PREEMPT=y
# CONFIG_PREEMPT_LAZY is not set
CONFIG_PREEMPT_RT=y        // <-- 已启用
CONFIG_PREEMPT_COUNT=y
CONFIG_PREEMPTION=y
// CONFIG_PREEMPT_DYNAMIC is not set    // 动态抢占模式关闭
CONFIG_PREEMPT_RCU=y
```

PREEMPT_RT 的核心目标是**将内核转变为几乎完全可抢占**，使实时任务能获得极低且确定的调度延迟。它通过以下关键手段实现：

1. **锁的可抢占化**：将大多数自旋锁替换为可睡眠的 RT-mutex 实现
2. **中断线程化**：所有中断（除极少数外）运行在可抢占的内核线程上下文中
3. **优先级继承**：通过 RT-mutex 的优先级继承机制防止优先级反转
4. **迁移禁用**：用 `migrate_disable()` 替代 `preempt_disable()` 保护 per-CPU 数据

---

## 2. 调度子系统中的 PREEMPT_RT 变化

### 2.1 schedule_rtlock() —— RT 锁调度入口

**文件**: `kernel/sched/core.c:7069-7075`

```c
#ifdef CONFIG_PREEMPT_RT
void __sched notrace schedule_rtlock(void)
{
    __schedule_loop(SM_RTLOCK_WAIT);
}
NOKPROBE_SYMBOL(schedule_rtlock);
#endif
```

`schedule_rtlock()` 是 PREEMPT_RT 专用的调度入口，用于 RT 锁（如 `spinlock_t` 在 RT 下的实现）等待时的调度。它使用 `SM_RTLOCK_WAIT` 调度模式，该模式：

- **在 `__schedule()` 中**：`sched_mode > SM_NONE` 判断为抢占，因此 `schedule_debug()` 和 RCU 将其视为一次抢占（`kernel/sched/core.c:6768`）
- **非抢占含义**：虽然是"抢占"语义，但实际上是任务主动让出 CPU 等待锁，与普通抢占的触发路径不同

### 2.2 TASK_RTLOCK_WAIT 任务状态

**文件**: `include/linux/sched.h:123`

```c
#define TASK_RTLOCK_WAIT    0x00001000
```

新增的任务状态，用于 RT 锁等待：

**状态转换流程** (`include/linux/sched.h:296-304`):

```c
current_save_and_set_rtlock_wait_state()
    raw_spin_lock(&current->pi_lock)
    current->saved_state = current->__state    // 保存原状态
    debug_rtlock_wait_set_state()
    trace_set_current_state(TASK_RTLOCK_WAIT)
    WRITE_ONCE(current->__state, TASK_RTLOCK_WAIT)  // 设置为 RT 锁等待
    raw_spin_unlock(&current->pi_lock)
```

**恢复流程**:

```c
current_restore_rtlock_saved_state()
    // 从 saved_state 恢复原始状态
```

**与唤醒机制的交互** (`kernel/sched/core.c:3933`):

```
对于 PREEMPT_RT，锁等待和锁唤醒通过 TASK_RTLOCK_WAIT 进行
没有其他位被设置，这使得可以区分所有唤醒场景
```

`ttwu_state_match()` 在处理 `TASK_RTLOCK_WAIT` 时，将其视为不可中断等待，与 `TASK_FROZEN` 类似（`kernel/sched/core.c:3930-3938`）。

### 2.3 TTWU_QUEUE 特性关闭

**文件**: `kernel/sched/features.h:74-77`

```c
#ifdef CONFIG_PREEMPT_RT
SCHED_FEAT(TTWU_QUEUE, false)
#else
SCHED_FEAT(TTWU_QUEUE, true)
#endif
```

PREEMPT_RT 下关闭 TTWU_QUEUE 特性：

- **TTWU_QUEUE (Try-To-Wake-Up Queue)**：在目标 CPU 上排队处理远程唤醒，减少 IPI
- **为什么关闭**：RT 任务需要**最低的唤醒延迟**，TTWU_QUEUE 的排队机制会增加延迟的不确定性
- **影响**：远程唤醒直接发送 IPI，而不是通过 ksoftirqd 排队处理

### 2.4 SCHED_NR_MIGRATE_BREAK 减小

**文件**: `kernel/sched/sched.h:3007-3011`

```c
#ifdef CONFIG_PREEMPT_RT
# define SCHED_NR_MIGRATE_BREAK 8
#else
# define SCHED_NR_MIGRATE_BREAK 32
#endif
```

- **作用**：负载均衡时，每次迁移最多处理的 task 数量上限，防止长时间持有 rq lock
- **RT 下减少到 8**：减少每次迁移操作的时间，降低锁持有时间，从而降低调度延迟
- 对应 `kernel/sched/core.c:2718-2725` 中 `kfree_rcu()` 的使用：因为 PREEMPT_RT 下 pi_lock 持有期间不能调用 `kfree()`，因此使用 `kfree_rcu()` 延迟释放

### 2.5 动态抢占模式禁用

**文件**: `kernel/sched/core.c:7599`

```c
#if !(defined(CONFIG_PREEMPT_RT) || defined(CONFIG_ARCH_HAS_PREEMPT_LAZY))
    if (!strcmp(str, "none"))
        return preempt_dynamic_none;
    if (!strcmp(str, "voluntary"))
        return preempt_dynamic_voluntary;
#endif
```

- 由于 `CONFIG_PREEMPT_DYNAMIC` 未设置，且 `CONFIG_PREEMPT_RT` 已启用，抢占模式固定为 `PREEMPT_FULL`，无法运行时切换
- `preempt_model_str()` 显示模型时增加括号区分（`kernel/sched/core.c:7760-7762`）

---

## 3. 锁机制的变化

### 3.1 spinlock_t → rt_mutex 基础

**文件**: `include/linux/spinlock.h:292-461`

PREEMPT_RT 下 `spinlock_t` 的行为发生根本性变化：

```c
#ifndef CONFIG_PREEMPT_RT
// 非 RT 内核：spin_lock 直接映射到 raw_spin_lock
static __always_inline raw_spinlock_t *spinlock_check(spinlock_t *lock)
{
    return &lock->rlock;
}
// spin_lock → raw_spin_lock
// spin_unlock → raw_spin_unlock
#else
// RT 内核：spin_lock 映射到可睡眠的 rt_spin_lock
#include <linux/spinlock_rt.h>
// spin_lock → rt_spin_lock (可睡眠)
// spin_unlock → rt_spin_unlock
#endif
```

**关键变化**：

| 操作 | !PREEMPT_RT | PREEMPT_RT |
|------|-------------|------------|
| `spin_lock()` | 关抢占 + 忙等 | 可睡眠 (rt_mutex) |
| `spin_unlock()` | 开抢占 | 唤醒等待者 |
| `spin_lock_irq()` | 关本地中断 + 忙等 | 关中断 + 可睡眠 |
| `raw_spin_lock()` | 关抢占 + 忙等 | 关抢占 + 忙等 **(不变)** |

### 3.2 raw_spinlock_t 保持不变

`raw_spinlock_t` 在 PREEMPT_RT 下行为**不变**，仍然是关抢占 + 忙等的自旋锁。这是内核中需要真正原子上下文的代码路径（如调度器内部、中断处理中不能睡眠的路径）使用的锁类型。

**设计原则**：
- 驱动程序使用 `spinlock_t`（RT 下变为可睡眠）
- 内核核心路径使用 `raw_spinlock_t`（RT 下仍为自旋）

### 3.3 rwlock_t → rwbase_rt

**文件**: `include/linux/rwlock_types.h:18-78`

```c
#ifndef CONFIG_PREEMPT_RT
// 非 RT：传统的读写自旋锁
typedef struct {
    arch_rwlock_t raw_lock;
} rwlock_t;
#else
// RT：基于 rwbase_rt 的可睡眠读写锁
#include <linux/rwbase_rt.h>
typedef struct {
    struct rwbase_rt    rwbase;
    atomic_t            readers;
} rwlock_t;
#endif
```

**rwlock_t 在 RT 下的行为**：
- 读锁：多个读者可以同时持有（可睡眠）
- 写锁：互斥访问（可睡眠）
- 内部使用 RT-mutex 机制，支持优先级继承

### 3.4 mutex → rt_mutex 基础

**文件**: `include/linux/mutex.h:78-150`

```c
#ifndef CONFIG_PREEMPT_RT
// 非 RT：传统的 mutex
struct mutex {
    atomic_long_t       owner;
    raw_spinlock_t      wait_lock;
    struct list_head    wait_list;
    ...
};
#else
// RT：基于 rt_mutex 的 mutex
struct mutex {
    struct rt_mutex_base    rtmutex;
    ...
};
#endif
```

**效果**：PREEMPT_RT 下 `mutex` 直接使用 `rt_mutex` 实现，获得完整的优先级继承和 PI (Priority Inheritance) 支持。

### 3.5 local_lock 行为变化

**文件**: `include/linux/local_lock.h`

`local_lock` 在 PREEMPT_RT 下会尝试获取锁，而在非 RT 下仅禁用抢占：

- 在 NMI/HARDIRQ 上下文中，RT 下 `local_trylock()` **总会失败**（`local_lock.h:67`）
- 这是因为 RT 下 local_lock 需要实际的锁操作，而中断上下文不能睡眠

---

## 4. 中断与软中断的变化

### 4.1 强制中断线程化 (force_irqthreads)

**文件**: `include/linux/interrupt.h:511-516`

```c
#ifdef CONFIG_PREEMPT_RT
# define force_irqthreads()    (true)
#else
DECLARE_STATIC_KEY_FALSE(force_irqthreads_key);
# define force_irqthreads()    (static_branch_unlikely(&force_irqthreads_key))
#endif
```

**PREEMPT_RT 下 `force_irqthreads()` 始终返回 true**，意味着所有中断处理程序（除标记为 `IRQF_NO_THREAD` 的核心里断外）都运行在**内核线程上下文**中。

**影响**：
- 中断处理程序变为可抢占的
- 中断处理程序可以睡眠（使用可睡眠锁）
- 中断延迟增加，但**确定性更好**（不会被其他中断无限阻塞）

### 4.2 软中断处理变化

**文件**: `include/linux/interrupt.h:598-603`

```c
#ifdef CONFIG_PREEMPT_RT
extern void do_softirq_post_smp_call_flush(unsigned int was_pending);
#else
static inline void do_softirq_post_smp_call_flush(unsigned int unused)
{
    do_softirq();
}
#endif
```

PREEMPT_RT 下软中断处理有重要变化：

- **非 RT**：软中断在 `do_IRQ()` 返回时、`local_bh_enable()` 时直接执行
- **RT 下**：软中断主要在 `ksoftirqd` 内核线程中执行，而不是在中断返回路径中
- 更多定时器被移到软中断上下文（`interrupt.h:619-620`）：`HRTIMER_MODE_HARD` 标志的定时器在硬中断中执行，其余在软中断中执行

### 4.3 local_bh 禁用变化

**文件**: `include/linux/bottom_half.h:8-9`

```c
#if defined(CONFIG_PREEMPT_RT) || defined(CONFIG_TRACE_IRQFLAGS)
extern void __local_bh_disable_ip(unsigned long ip, unsigned int cnt);
#else
static __always_inline void __local_bh_disable_ip(unsigned long ip, unsigned int cnt)
{
    preempt_count_add(cnt);
    barrier();
}
#endif
```

- 非 RT 下：`local_bh_disable()` 仅增加 `preempt_count` 的 softirq 计数
- RT 下：`__local_bh_disable_ip()` 是外部函数，需要额外的锁操作，不再是简单的内联计数
- `local_bh_blocked()` 函数在 RT 下提供真正的阻塞状态查询（`bottom_half.h:36-40`）

---

## 5. 高精度定时器 (hrtimer) 的变化

### 5.1 HRTIMER_MODE_HARD 区分

**文件**: `include/linux/hrtimer.h:33`

```c
enum hrtimer_mode {
    ...
    HRTIMER_MODE_HARD   = 0x08,  // 在 RT 下也在硬中断上下文执行
    ...
};
```

- **非 RT 下**：所有 hrtimer 回调都在硬中断上下文执行
- **RT 下**：默认 hrtimer 回调在软中断上下文（`ksoftirqd`）执行
- `HRTIMER_MODE_HARD` 标志：强制在硬中断上下文执行，即使 RT 下也如此
- `interrupt.h:619-620`：RT 下更多 hrtimer 被移到软中断处理，包括所有未明确标记 `HRTIMER_MODE_HARD` 的定时器

### 5.2 hrtimer_cancel_wait_running

**文件**: `include/linux/hrtimer.h:198-203`

```c
#ifdef CONFIG_PREEMPT_RT
void hrtimer_cancel_wait_running(const struct hrtimer *timer);
#else
static inline void hrtimer_cancel_wait_running(struct hrtimer *timer)
{
    cpu_relax();
}
#endif
```

- **非 RT**：取消定时器时如果回调正在运行，忙等待（`cpu_relax()`）
- **RT 下**：使用真正的等待机制（可能睡眠），避免忙等影响实时性

---

## 6. RCU 的变化

**文件**: `include/linux/rcupdate.h:248-255`

```c
// RT 下不需要 rcu_softirq_qs_periodic 宏
#define rcu_softirq_qs_periodic(old_ts) \
do { \
    if (!IS_ENABLED(CONFIG_PREEMPT_RT) && \
        time_after(jiffies, (old_ts) + HZ / 10)) { \
        preempt_disable(); \
        rcu_softirq_qs(); \
        preempt_enable(); \
        (old_ts) = jiffies; \
    } \
} while (0)
```

**PREEMPT_RT 下 RCU 的变化**：

- **RCU 软中断处理**：RT 内核有更多机会调用 `schedule()`，自然提供 RCU 所需的 quiescent states，因此不需要 `rcu_softirq_qs_periodic()` 的周期性检查
- **RCU-bh 检查**：`rcu_sleep_check()` 在 RT 下跳过 RCU-bh 的锁持有检查（`rcupdate.h:400`），因为 RT 下 spinlock 可睡眠，bh 语义已改变
- `CONFIG_PREEMPT_RCU=y` 已启用，支持抢占 RCU 读侧临界区

---

## 7. task_struct 中的 PREEMPT_RT 字段

**文件**: `include/linux/sched.h`

| 字段 | 位置 | 说明 |
|------|------|------|
| `net_xmit` | 1050 | 网络发送 RT 专用数据 |
| `softirq_disable_cnt` | 1263 | 软中断禁用计数（RT 下需要额外跟踪） |
| `cg_dead_lnode` | 1325 | CGroup 延迟释放链表节点 |
| `saved_state_change` | 1555 | DEBUG_ATOMIC_SLEEP 下保存的状态变化位置 |

这些字段在 PREEMPT_RT 下用于：
- 跟踪 RT 特有的 per-task 状态
- 支持延迟释放（`kfree_rcu` 等）
- 调试 RT 相关的原子睡眠问题

---

## 8. 内存管理的变化

**文件**: `include/linux/sched/mm.h:58-63`

```c
#ifdef CONFIG_PREEMPT_RT
// RCU 回调延迟释放 mm_struct
static inline void __mmdrop_delayed(struct rcu_head *rhp)
{
    ...
}
#endif
```

- `mmdrop()` 在 RT 下不能直接释放（可能在不可睡眠上下文），使用 `__mmdrop_delayed()` 通过 RCU 回调延迟释放
- `include/linux/gfp_types.h` 中的 GFP 标志在 RT 下有不同行为
- `include/linux/highmem.h` 中 kmap 相关操作在 RT 下有特殊处理

---

## 9. migrate_disable 机制

**文件**: `include/linux/preempt.h:372-401`

`migrate_disable()` 是 PREEMPT_RT 的核心机制之一：

```c
// 在 PREEMPT_RT 下，许多原语使用 migrate_disable() 替代 preempt_disable()
// 用于保护 per-CPU 数据访问
```

**设计原理**：

| 传统 (非 RT) | PREEMPT_RT |
|-------------|------------|
| `preempt_disable()` | `migrate_disable()` |
| 禁用抢占，防止迁移 | 允许抢占，但防止迁移到其他 CPU |
| 同时保护 per-CPU 和原子性 | 仅保护 per-CPU 数据，不阻止抢占 |

**为什么需要 migrate_disable**：

> PREEMPT_RT 通过强制许多原语变为可抢占，它们也会允许迁移。这破坏了大量的 per-CPU 使用模式。为此，所有这些原语都使用 `migrate_disable()` 来恢复此隐含假设。
>
> 这是一个"临时"的权宜之计。正确的解决方案是重构 per-CPU 数据使用模式。

**性能权衡**：
- 更高优先级的任务：减少唤醒延迟（不会因为低优先级任务关抢占而被阻塞）
- 更低优先级的任务：可能被限制在某个 CPU 上，降低可用带宽

---

## 10. 调试与跟踪变化

**文件**: `kernel/sched/debug.c`

调度器调试接口中包含 PREEMPT_RT 特定的调试信息。

**文件**: `kernel/sched/syscalls.c`

系统调用接口中处理 PREEMPT_RT 相关的调度策略和参数。

---

## 11. 总结：PREEMPT_RT 内核架构变化总览

```
PREEMPT_RT 内核架构变化

┌─────────────────────────────────────────────────────┐
│                   用户态进程                          │
└───────────────────────┬─────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────┐
│              系统调用 / 异常处理                       │
│              (可抢占, 可睡眠)                         │
└───────────────────────┬─────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────┐
│              内核线程 (包括中断线程)                   │
│              ┌─────────────────────┐                 │
│              │ ksoftirqd           │ ← 软中断在此执行 │
│              │ irq/xxx-N           │ ← 线程化中断处理 │
│              └─────────────────────┘                 │
└───────────────────────┬─────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────┐
│              锁机制层                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │ spinlock_t   │  │ rwlock_t     │  │ mutex      │ │
│  │ (rt_mutex)   │  │ (rwbase_rt)  │  │ (rt_mutex) │ │
│  │ 可睡眠       │  │ 可睡眠       │  │ PI 支持    │ │
│  └──────────────┘  └──────────────┘  └────────────┘ │
│  ┌──────────────┐                                    │
│  │ raw_spinlock │ (不变, 仍为忙等)                    │
│  └──────────────┘                                    │
└───────────────────────┬─────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────┐
│              调度器层                                  │
│  ┌─────────────────────────────────────────────────┐ │
│  │ schedule_rtlock()  ← TASK_RTLOCK_WAIT          │ │
│  │ TTWU_QUEUE = off   (直接 IPI 唤醒)              │ │
│  │ SCHED_NR_MIGRATE_BREAK = 8 (更小批量)           │ │
│  │ migrate_disable() 替代 preempt_disable()         │ │
│  └─────────────────────────────────────────────────┘ │
└───────────────────────┬─────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────┐
│              硬件中断层                                │
│  ┌─────────────────────────────────────────────────┐ │
│  │ 中断 → 线程化中断处理程序 (可抢占)                │ │
│  │ 只保留必要的中断在硬中断上下文                     │ │
│  │ HRTIMER_MODE_HARD 标志控制定时器上下文            │ │
│  └─────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘
```

**核心变化总结**：

| 维度 | 传统内核 | PREEMPT_RT 内核 |
|------|---------|----------------|
| **抢占模型** | `PREEMPT_VOLUNTARY` / `PREEMPT` | `PREEMPT_FULL`（固定） |
| **spinlock_t** | 忙等 + 关抢占 | 可睡眠 (rt_mutex) |
| **raw_spinlock_t** | 忙等 + 关抢占 | 忙等 + 关抢占（不变） |
| **rwlock_t** | 读写自旋锁 | 可睡眠读写锁 (rwbase_rt) |
| **mutex** | 普通互斥锁 | rt_mutex（PI 支持） |
| **中断处理** | 硬中断上下文 | 线程化 |
| **软中断** | 直接执行 | ksoftirqd 线程 |
| **hrtimer 回调** | 硬中断上下文 | 软中断上下文（默认） |
| **任务唤醒** | 可能排队 | 直接 IPI |
| **负载均衡** | 32 个/批 | 8 个/批 |
| **per-CPU 保护** | preempt_disable | migrate_disable |
| **RCU** | 需要周期性 QS | 自然提供 QS |
| **动态抢占** | 支持切换 | 固定 FULL |
| **mm 释放** | 直接释放 | 延迟释放 |