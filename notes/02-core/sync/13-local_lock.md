# 13. 本地锁 (local_lock)

## 13.1 概述

Local lock 是 PREEMPT_RT 内核引入的机制，用于替代在非 RT 内核中通过"禁用抢占 + per-CPU 访问"实现的隐式锁定模式。

**核心问题：**
- 非 RT 内核中，`preempt_disable()` 可保护 per-CPU 数据
- PREEMPT_RT 下 spin_lock 不再禁用抢占，需要显式机制
- local_lock 提供显式的 per-CPU 锁语义

## 13.2 关键数据结构

定义在 [include/linux/local_lock.h](file:///home/louis/code/linux/include/linux/local_lock.h)：

```c
// include/linux/local_lock.h
typedef struct {
    /*
     * 非 RT: 空结构体 (仅编译时检查)
     * RT:    包含 spinlock_t 实现
     */
#ifdef CONFIG_PREEMPT_RT
    spinlock_t          lock;       // RT 下实际的锁
#endif
    struct lockdep_map  dep_map;    // Lockdep 依赖图
} local_lock_t;

// 静态初始化
#define LOCAL_LOCK_INIT(name) {                        \
    .dep_map = { .name = #name },                      \
}

#define DEFINE_LOCAL_LOCK(name)                        \
    local_lock_t name = LOCAL_LOCK_INIT(name)

// 动态初始化
#define local_lock_init(lock)                          \
    do {                                               \
        static struct lock_class_key _key;             \
        lockdep_init_map(&(lock)->dep_map, #lock, &_key, 0); \
    } while (0)
```

### 13.2.1 本树 7.2 的真实定义

上面的结构图是示意。实际代码在 `include/linux/local_lock_internal.h`，两条分支差别很大：

```c
#ifndef CONFIG_PREEMPT_RT

context_lock_struct(local_lock) {
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
	struct task_struct	*owner;
#endif
};
typedef struct local_lock local_lock_t;

context_lock_struct(local_trylock) {
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
	struct task_struct	*owner;
#endif
	u8		acquired;
};
typedef struct local_trylock local_trylock_t;

#else /* !CONFIG_PREEMPT_RT */

/*
 * On PREEMPT_RT local_lock maps to a per CPU spinlock, which protects the
 * critical section while staying preemptible.
 */
typedef spinlock_t local_lock_t;
typedef spinlock_t local_trylock_t;

#endif
```

几个关键事实：

1. **非 RT 且未开 `CONFIG_DEBUG_LOCK_ALLOC` 时，`local_lock_t` 是空结构**。GCC 允许 `sizeof(struct{}) == 0`，因此它不占空间、也不初始化任何东西——`local_lock` 彻底退化为 `preempt_disable()`，零开销。本树 `CONFIG_DEBUG_LOCK_ALLOC` 未启用，正处于这一形态。
2. **非 RT 的 `local_trylock_t` 才带 `u8 acquired`**，因为 trylock 必须记住"本 CPU 上是否已持有"，才能在 `local_unlock` 时正确释放。
3. **RT 下两者都是 `spinlock_t`**，而本树 `CONFIG_PREEMPT_RT=y` 且 `spinlock_t` 是 `rt_mutex_base` 的封装，所以 RT 的 local_lock 实际是一把 per-CPU 的 rtmutex。
4. 本树**没有** `DEFINE_LOCAL_LOCK()` 宏。静态写法是用 `INIT_LOCAL_LOCK(name)` 作为 per-CPU 变量的初始化器：

```c
static DEFINE_PER_CPU(struct mlock_fbatch, mlock_fbatch) = {
	.lock = INIT_LOCAL_LOCK(lock),
};
```

动态初始化用 `local_lock_init(lock)`（RT 下即 `local_spin_lock_init()`）。

`context_lock_struct` 来自 `include/linux/compiler-context-analysis.h`：未开启 `WARN_CONTEXT_ANALYSIS` 时它展开成 `struct name`（后接结构体体）；开启时还会顺带生成 `__acquire_ctx_lock()`/`__try_acquire_ctx_lock()` 等一组重载辅助函数，供编译器做上下文检查。因此不能假定 `local_lock_t` 一定有字段。

## 13.3 核心 API

```c
// include/linux/local_lock.h

// 获取本地锁
void local_lock(local_lock_t *lock);
// 非 RT: preempt_disable()
// RT:    spin_lock(&lock->lock) + migrate_disable()

// 释放本地锁
void local_unlock(local_lock_t *lock);
// 非 RT: preempt_enable()
// RT:    spin_unlock(&lock->lock) + migrate_enable()

// 带 irq 保护的变体
void local_lock_irq(local_lock_t *lock);
void local_unlock_irq(local_lock_t *lock);
void local_lock_irqsave(local_lock_t *lock, unsigned long flags);
void local_unlock_irqrestore(local_lock_t *lock, unsigned long flags);

// 带 softirq 保护的变体
void local_lock_bh(local_lock_t *lock);
void local_unlock_bh(local_lock_t *lock);
```

## 13.4 实现原理

### 13.4.1 非 RT 实现

```c
// 非 RT: local_lock 就是 preempt_disable/enable
#define local_lock(lock)                          \
    do {                                          \
        migrate_disable();                        \
        lock_acquire(&(lock)->dep_map, 0, 0, 0, 1, NULL, _THIS_IP_); \
    } while (0)

#define local_unlock(lock)                        \
    do {                                          \
        lock_release(&(lock)->dep_map, _THIS_IP_); \
        migrate_enable();                         \
    } while (0)
```

### 13.4.2 PREEMPT_RT 实现

```c
// PREEMPT_RT: local_lock 是真实的 spinlock
// 但这是 per-CPU spinlock, 不会跨 CPU 竞争
// 仅用于防止同 CPU 上的抢占/中断/softirq 竞争

#define local_lock(lock)                          \
    do {                                          \
        spin_lock(&(lock)->lock);                 \
        lock_acquire(&(lock)->dep_map, 0, 0, 0, 1, NULL, _THIS_IP_); \
    } while (0)

#define local_unlock(lock)                        \
    do {                                          \
        lock_release(&(lock)->dep_map, _THIS_IP_); \
        spin_unlock(&(lock)->lock);               \
    } while (0)
```

### 13.4.3 非 RT 的真实宏展开

`include/linux/local_lock.h` 的公开宏只是一个"取本 CPU 实例"的壳：

```c
#define local_lock(lock)		__local_lock(__this_cpu_local_lock(lock))
#define local_lock_irq(lock)		__local_lock_irq(__this_cpu_local_lock(lock))
#define local_lock_irqsave(lock, flags)				\
	__local_lock_irqsave(__this_cpu_local_lock(lock), flags)
```

`__this_cpu_local_lock(base)` 就是 `this_cpu_ptr(base)`（未开启上下文分析时）。真正的实现在 `local_lock_internal.h`：

```c
#define __local_lock(lock)					\
	do {							\
		preempt_disable();				\
		__local_lock_acquire(lock);			\
		__acquire(lock);				\
	} while (0)

#define __local_lock_irq(lock)					\
	do {							\
		local_irq_disable();				\
		__local_lock_acquire(lock);			\
		__acquire(lock);				\
	} while (0)

#define __local_lock_irqsave(lock, flags)			\
	do {							\
		local_irq_save(flags);				\
		__local_lock_acquire(lock);			\
		__acquire(lock);				\
	} while (0)
```

`__local_lock_acquire()` 在 `CONFIG_DEBUG_LOCK_ALLOC` 下才做实事：`lock_map_acquire(&l->dep_map)`、`DEBUG_LOCKS_WARN_ON(l->owner)`、`l->owner = current`。本树该配置关闭，于是它展开为空，`local_lock()` 就是一条 `preempt_disable()`。

### 13.4.4 RT 的真实宏展开

```c
#define __local_lock(__lock)					\
	do {							\
		migrate_disable();				\
		spin_lock((__lock));				\
	} while (0)

#define __local_lock_irq(lock)			__local_lock(lock)

#define __local_lock_irqsave(lock, flags)			\
	do {							\
		typecheck(unsigned long, flags);		\
		flags = 0;					\
		__local_lock(lock);				\
	} while (0)

#define __local_unlock(__lock)					\
	do {							\
		spin_unlock((__lock));				\
		migrate_enable();				\
	} while (0)
```

两个必须注意的语义变化：

1. **`_irq` / `_irqsave` 在 RT 下并不关中断**。`__local_lock_irq` 直接等于 `__local_lock`，`_irqsave` 只是把 `flags` 置 0。串行化完全交给 rtmutex：同一 CPU 上的中断处理线程也会去拿同一把 per-CPU 锁，谁先拿到谁执行。这是 RT 把中断线程化的前提之一；如果代码依赖"关中断后硬件不会打断我"来做别的事（例如访问需要原子性的设备寄存器），必须改用 `raw_spin_lock_irqsave()`。
2. **`migrate_disable()` 而不是 `preempt_disable()`**。RT 的 spinlock 可睡眠，睡眠意味着可能迁移 CPU，而 per-CPU 数据不能迁移。`include/linux/preempt.h` 里有一段长注释解释这个取舍：`migrate_disable()` 只禁止迁移、仍允许抢占，因此高优先级任务不会被低优先级任务的 per-CPU 临界区阻塞；代价是被迁移限制的任务可能"顶住"别的任务，降低带宽。注释明确说这是"temporary workaround"，最终目标是消灭对 per-CPU 的隐式假设。

### 13.4.5 local_trylock 与 local_lock_is_locked

```c
#define __local_trylock(lock)					\
	__try_acquire_ctx_lock(lock, context_unsafe(({		\
		int __locked;					\
								\
		if (in_nmi() | in_hardirq()) {			\
			__locked = 0;				\
		} else {					\
			migrate_disable();			\
			__locked = spin_trylock((lock));	\
			if (!__locked)				\
				migrate_enable();		\
		}						\
		__locked;					\
	})))
```

- RT 下 `in_nmi() | in_hardirq()` 直接返回失败：rtmutex 不能在 NMI/硬中断里获取（它需要睡眠）。`local_lock.h` 的注释也写明"The function ... will _always_ fail to acquire the lock in NMI or HARDIRQ context on PREEMPT_RT"。
- 非 RT 下的 trylock 用 `local_trylock_t` 的 `acquired` 字节做本 CPU 的互斥，并配合 `preempt_disable()`。
- `local_lock_is_locked()` 要求调用者**已关闭抢占/迁移**：非 RT 下读 `this_cpu_ptr(lock)->acquired`，因此只对 `local_trylock_t` 有意义（`local_lock_t` 没有该字段）；RT 下用 `rt_mutex_owner(&this_cpu_ptr(lock)->lock) == current` 判断。

## 13.5 使用场景

| 场景 | 说明 |
|------|------|
| Per-CPU 统计 | 保护 per-CPU 统计计数器 |
| 本地资源池 | 保护 per-CPU 内存池 |
| 网络数据包处理 | 保护 per-CPU 网络队列 |
| 调度器本地数据 | 保护 per-CPU 调度数据 |

### 13.5.1 与相邻机制的对比

| 机制 | 跨 CPU 互斥 | 阻止本 CPU 抢占 | 阻止迁移 | 阻止中断 | 可睡眠 | 开销 |
|--|--|--|--|--|--|--|
| `preempt_disable()` | 否 | 是 | 是（隐式） | 否 | 否 | 1 条指令 |
| `migrate_disable()` | 否 | 否 | 是 | 否 | 是 | 计数；`migrate_enable()` 时可能触发迁移 |
| `this_cpu_ptr()`/`this_cpu_inc()` | 否 | 否（需自行保证） | 否 | 否 | 是 | 1 条指令 |
| `local_lock()` | 否 | 非 RT 是；RT 否 | 非 RT 是；RT 是 | 否 | RT 是 | 0（非 RT）/ rtmutex 快路径（RT） |
| `local_lock_irqsave()` | 否 | 非 RT 是 | 同上 | 非 RT 是；RT **否** | RT 是 | 关中断（非 RT） |
| `spinlock_t`（全局） | 是 | 非 RT 是；RT 否 | 非 RT 是 | 否 | RT 是 | 原子操作 + 可能自旋 |
| `raw_spinlock_t` | 是 | 是 | 是 | 否（`_irq` 变体是） | 否 | 原子操作 + 自旋 |

一句话总结：`local_lock` 提供的是"局部串行化"，不是"互斥"。它把"我这段代码只允许在本 CPU 上不被打断地跑"这个隐含约定**显式化**，从而让 RT 内核可以把同一段代码在非 RT 下的 `preempt_disable()` 语义替换成"可抢占但不可迁移"。

### 13.5.2 真实使用者

`mm/mlock.c` 是最干净的示例——per-CPU 的 `folio_batch` 必须在操作期间不被本 CPU 的其他路径插入：

```c
struct mlock_fbatch {
	local_lock_t lock;
	struct folio_batch fbatch;
};

static DEFINE_PER_CPU(struct mlock_fbatch, mlock_fbatch) = {
	.lock = INIT_LOCAL_LOCK(lock),
};

void mlock_folio(struct folio *folio)
{
	struct folio_batch *fbatch;

	local_lock(&mlock_fbatch.lock);
	fbatch = this_cpu_ptr(&mlock_fbatch.fbatch);
	...
	local_unlock(&mlock_fbatch.lock);
}
```

`mm/swap.c` 则展示了"两种保护强度分组"的写法，注释直接说明了意图：

```c
struct cpu_fbatches {
	/*
	 * The following folio batches are grouped together because they are protected
	 * by disabling preemption (and interrupts remain enabled).
	 */
	local_lock_t lock;
	struct folio_batch lru_add;
	...
	/* Protecting the following batches which require disabling interrupts */
	local_lock_t lock_irq;
	struct folio_batch lru_move_tail;
};
```

- `lock` 走 `local_lock()`：只需挡抢占，中断仍可进来（中断路径不碰这些 batch）。
- `lock_irq` 走 `local_lock_irqsave()`：`lru_move_tail` 会被中断路径修改，因此必须关中断。

`net/core/skbuff.c` 的 `napi_alloc_cache` 用 `local_lock_bh()` 保护 per-CPU skb 缓存：NAPI 轮询在 softirq 上下文，用户态路径进入时必须关 BH 才能与它串行。

### 13.5.3 为什么不能直接用 spinlock 替代

在非 RT 内核里，`spin_lock()` 恰好也关抢占，很多人因此直接用全局 spinlock 保护 per-CPU 数据。这在 RT 上会立刻出错：

- RT 的 `spin_lock()` 是 rtmutex，**不关抢占也不阻止迁移**；
- 一旦在持锁期间被抢占并迁移到另一个 CPU，`this_cpu_ptr()` 就指向了新 CPU 的实例，原来保护的那个实例被"半改半不改"；
- 又因为 rtmutex 可睡眠，持锁期间还可能被调度出去，临界区被无限拉长。

`local_lock` 的存在就是为了在两种内核上表达同一件事：**"我只操作本 CPU 的数据，需要的是不被本 CPU 的其他上下文打断，而不是跨 CPU 互斥。"**

## 13.6 使用示例

```c
// 示例: per-CPU 统计计数器
DEFINE_LOCAL_LOCK(lock);
DEFINE_PER_CPU(struct stats, cpu_stats);

void update_stats(size_t bytes)
{
    struct stats *stats;

    local_lock(&lock);
    stats = this_cpu_ptr(&cpu_stats);
    stats->packets++;
    stats->bytes += bytes;
    local_unlock(&lock);
}

void read_stats(void)
{
    unsigned long total_packets = 0;
    unsigned long total_bytes = 0;
    int cpu;

    for_each_possible_cpu(cpu) {
        struct stats *stats = per_cpu_ptr(&cpu_stats, cpu);
        total_packets += stats->packets;
        total_bytes += stats->bytes;
    }
    printk("total: %lu packets, %lu bytes\n", total_packets, total_bytes);
}
```

### 13.6.1 锁必须位于 per-CPU 区

`local_lock(lock)` 会展开成 `__local_lock(__this_cpu_local_lock(lock))`，而 `__this_cpu_local_lock(base)` 是 `this_cpu_ptr(base)`——它会给地址加上"本 CPU 的 per-CPU 偏移"。因此传入的 `lock` 必须本身就在 per-CPU 区，否则取到的是无效地址。

两种正确写法：

```c
/* 写法一：锁独立成为 per-CPU 变量（传 per-CPU 符号本身，不要先 this_cpu_ptr） */
static DEFINE_PER_CPU(local_lock_t, my_lock) = INIT_LOCAL_LOCK(my_lock);

void f(void)
{
	local_lock(&my_lock);
	...
	local_unlock(&my_lock);
}

/* 写法二（更常见）：锁嵌在 per-CPU 结构体里 */
struct my_state {
	local_lock_t lock;
	u64 counter;
};
static DEFINE_PER_CPU(struct my_state, my_state) = {
	.lock = INIT_LOCAL_LOCK(lock),
};

void g(void)
{
	local_lock(&my_state.lock);      /* &my_state 是 per-CPU 地址 */
	this_cpu_ptr(&my_state)->counter++;
	local_unlock(&my_state.lock);
}
```

上面 13.6 的示例把 `DEFINE_LOCAL_LOCK(lock)` 声明成普通全局变量，单独使用在本树是行不通的；必须像 `mm/mlock.c` 那样把锁放进 `DEFINE_PER_CPU` 的结构体。

### 13.6.2 作用域守卫

`include/linux/local_lock.h` 用 `DEFINE_LOCK_GUARD_1()` 生成了 guard 类，可避免忘记解锁：

```c
DEFINE_LOCK_GUARD_1(local_lock, local_lock_t __percpu,
		    local_lock(_T->lock),
		    local_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(local_lock_irqsave, local_lock_t __percpu,
		    local_lock_irqsave(_T->lock, _T->flags),
		    local_unlock_irqrestore(_T->lock, _T->flags),
		    unsigned long flags)
```

使用 `guard(local_lock)(&my_state.lock);` 或 `scoped_guard(local_lock_irqsave, &my_state.lock) { ... }`，函数返回时自动 `local_unlock()`。这与 `mutex`/`spinlock` 的 guard 用法一致，详见 [17-guard-scope.md](17-guard-scope.md)。

### 13.6.3 配置与开销现状

| 配置/条件 | 本树取值 | 对 local_lock 的影响 |
|--|--|--|
| `CONFIG_PREEMPT_RT` | `y` | `local_lock_t` = `spinlock_t`（rtmutex），`local_lock()` = `migrate_disable()+spin_lock()` |
| `CONFIG_DEBUG_LOCK_ALLOC` | 未启用 | 非 RT 下 `dep_map`/`owner` 不编译，也无 `lock_map_*` 检查 |
| `CONFIG_SMP` | `y` | `this_cpu_ptr()` 需要真实的 per-CPU 偏移，迁移问题成立 |
| `WARN_CONTEXT_ANALYSIS` | 未启用 | 没有编译期的"是否持有 ctx lock"检查，`__this_cpu_local_lock()` 就是普通 `this_cpu_ptr()` |

因此在本树上，一次 `local_lock()/local_unlock()` 的真实代价约为：`migrate_disable()`（preempt count 自增）+ 一把 per-CPU rtmutex 的快速获取/释放。它比非 RT 的 `preempt_disable()` 贵，但换来的是临界区可被高优先级任务抢占——这正是 RT 的取舍。

## 13.7 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/local_lock.h](file:///home/louis/code/linux/include/linux/local_lock.h) | local_lock API |