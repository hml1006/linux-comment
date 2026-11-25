# 14. Per-CPU RWSEM (percpu-rwsem)

## 14.1 概述

Per-CPU RWSEM 是一种基于 per-CPU 计数器的读写信号量，针对"读非常频繁、写极少"的场景优化。读者仅操作 per-CPU 计数器（无竞争），写者需要等待所有 CPU 的读者完成。

**核心特性：**
- 读者极快：仅操作 per-CPU 计数器，无需原子操作
- 写者批量等待：写者等待所有读者释放
- 适合读极多写极少的场景

## 14.2 关键数据结构

定义在 [include/linux/percpu-rwsem.h](file:///home/louis/code/linux/include/linux/percpu-rwsem.h)：

```c
// include/linux/percpu-rwsem.h
struct percpu_rw_semaphore {
    /*
     * rcu_sync 用于协调读者和写者之间的 RCU 宽限期,
     * 确保写者看到所有读者已完成
     */
    struct rcu_sync          rss;        // RCU 同步

    /*
     * read_count 是 per-CPU 变量,
     * 每个 CPU 维护一个本地读者计数
     */
    unsigned int __percpu    *read_count; // per-CPU 读者计数

    /*
     * 写者锁, 使用 rt_mutex (PREEMPT_RT) 或 rwsem
     */
    struct rcuwait           writer;     // 写者等待队列
    wait_queue_head_t        waiters;    // 等待者队列
    struct rw_semaphore      rw_sem;     // 底层 rwsem (非 RT)

    /*
     * 写者状态
     * 0 = 无写者, 1 = 有写者或等待
     */
    atomic_t                 block;      // 写者阻塞标志
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map       dep_map;
#endif
};
```

### 14.2.1 本树真实结构：没有 rw_sem

上面是概念图。本树 `include/linux/percpu-rwsem.h` 的定义更精简——**没有** `struct rw_semaphore rw_sem` 成员：

```c
struct percpu_rw_semaphore {
	struct rcu_sync		rss;
	unsigned int __percpu	*read_count;
	struct rcuwait		writer;
	wait_queue_head_t	waiters;
	atomic_t		block;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

| 字段 | 作用 |
|--|--|
| `rss` | `rcu_sync` 状态机：`GP_IDLE`（快路径开启）↔ `GP_ENTER/GP_PASSED/GP_EXIT`（读者必须走慢路径） |
| `read_count` | `alloc_percpu(int)` 得到的 per-CPU 读者计数，快路径只做 `this_cpu_inc()` |
| `writer` | `rcuwait`，写者等待"所有读者退出的那个时刻"；读者退出时 `rcuwait_wake_up()` |
| `waiters` | 普通等待队列，存放"等待获取"的读者/写者（FIFO） |
| `block` | `atomic_t`，0 = 允许读者走快路径；1 = 有写者（或写者正在等待） |
| `dep_map` | lockdep 映射，本树 `CONFIG_DEBUG_LOCK_ALLOC` 未启用，不编译 |

静态定义宏也体现了 `read_count` 必须是独立的 per-CPU 变量：

```c
#define __DEFINE_PERCPU_RWSEM(name, is_static)				\
static DEFINE_PER_CPU(unsigned int, __percpu_rwsem_rc_##name);		\
is_static struct percpu_rw_semaphore name = {				\
	.rss = __RCU_SYNC_INITIALIZER(name.rss),			\
	.read_count = &__percpu_rwsem_rc_##name,			\
	.writer = __RCUWAIT_INITIALIZER(name.writer),			\
	.waiters = __WAIT_QUEUE_HEAD_INITIALIZER(name.waiters),		\
	.block = ATOMIC_INIT(0),					\
	__PERCPU_RWSEM_DEP_MAP_INIT(name)				\
}
```

对比传统 `rw_semaphore`：后者在非 RT 下用 `atomic_long_t count/owner` + `optimistic_spin_queue`，RT 下用 `rwbase_rt`；percpu-rwsem 完全绕开了这些，读者的"锁"只是一次 per-CPU 自增。

## 14.3 核心 API

```c
// include/linux/percpu-rwsem.h

// 初始化
void percpu_free_rwsem(struct percpu_rw_semaphore *sem);
int percpu_init_rwsem(struct percpu_rw_semaphore *sem);

// 读者操作
void percpu_down_read(struct percpu_rw_semaphore *sem);
void percpu_up_read(struct percpu_rw_semaphore *sem);

// 写者操作
void percpu_down_write(struct percpu_rw_semaphore *sem);
void percpu_up_write(struct percpu_rw_semaphore *sem);
```

## 14.4 工作原理

### 14.4.1 读者路径

```
percpu_down_read(sem):
  │
  ├── rcu_read_lock()
  │     └── 在 RCU 读端临界区内保证 per-CPU 访问安全
  │
  ├── if (atomic_read(&sem->block) == 0):
  │     └── __this_cpu_inc(sem->read_count)  → 本地计数 +1 (无原子操作)
  │
  └── else:
        └── 有写者 → 进入慢速路径
              └── percpu_down_read_slowpath(sem)
                    ├── __this_cpu_dec(sem->read_count)  → 撤销计数
                    └── down_read(&sem->rw_sem)          → 获取 rwsem 读锁

percpu_up_read(sem):
  │
  └── __this_cpu_dec(sem->read_count)  → 本地计数 -1 (无原子操作)
        │
        └── rcu_read_unlock()
```

### 14.4.2 写者路径

```
percpu_down_write(sem):
  │
  ├── down_write(&sem->rw_sem)         → 获取 rwsem 写锁
  │
  ├── atomic_set(&sem->block, 1)       → 设置阻塞标志
  │
  ├── synchronize_rcu()                → 等待 RCU 宽限期
  │     └── 确保所有读者看到 block=1
  │
  └── 遍历所有 CPU:
        └── while (per_cpu(*sem->read_count, cpu) > 0):
              └── 等待该 CPU 上的读者释放

percpu_up_write(sem):
  │
  ├── atomic_set(&sem->block, 0)       → 清除阻塞标志
  │
  └── up_write(&sem->rw_sem)           → 释放 rwsem 写锁
```

### 14.4.3 本树读者的快路径

上面两节是"早期实现"的直觉模型。本树 7.2 的读者 fast path 完全在头文件里内联，且用 `rcu_sync_is_idle()` 代替"读 `block`"：

```c
static inline void percpu_down_read_internal(struct percpu_rw_semaphore *sem,
					     bool freezable)
{
	might_sleep();

	rwsem_acquire_read(&sem->dep_map, 0, 0, _RET_IP_);

	preempt_disable();
	/*
	 * We are in an RCU-sched read-side critical section, so the writer
	 * cannot both change sem->state from readers_fast and start checking
	 * counters while we are here. ...
	 */
	if (likely(rcu_sync_is_idle(&sem->rss)))
		this_cpu_inc(*sem->read_count);
	else
		__percpu_down_read(sem, false, freezable); /* Unconditional memory barrier */
	preempt_enable();
}
```

要点：

1. `preempt_disable()` 同时充当 RCU-sched 读端临界区——这保证"自增所在 CPU"与"后续 `preempt_enable()` 之后才可能被写者看到"之间的顺序。
2. 快路径只有 `this_cpu_inc()`（单条 per-CPU 加法，无原子前缀、无 cacheline 竞争）。
3. `rcu_sync_is_idle()` 为假时进入 `__percpu_down_read()`，那条路径自带 `smp_mb()`（"Unconditional memory barrier"注释），保证读者要么看到 `block=0` 走快路径，要么看到 `block=1` 走慢路径。
4. `rcu_sync` 的状态只有 `GP_IDLE / GP_ENTER / GP_PASSED / GP_EXIT / GP_REPLAY`（`kernel/rcu/sync.c`），`rcu_sync_is_idle()` 就是 `!READ_ONCE(rsp->gp_state)`。写者 `rcu_sync_enter()` 把状态推离 IDLE，由此触发一次宽限期；`rcu_sync_exit()` 在宽限期结束后回到 IDLE。

慢路径本身很短：

```c
bool __sched __percpu_down_read(struct percpu_rw_semaphore *sem, bool try,
				bool freeze)
{
	if (__percpu_down_read_trylock(sem))
		return true;

	if (try)
		return false;

	trace_contention_begin(sem, LCB_F_PERCPU | LCB_F_READ);
	preempt_enable();
	percpu_rwsem_wait(sem, /* .reader = */ true, freeze);
	preempt_disable();
	trace_contention_end(sem, 0);

	return true;
}
```

注意 `preempt_enable()` → 睡眠等待 → `preempt_disable()` 的配对：调用者（`percpu_down_read_internal`）期望"返回时仍处于 preempt-disabled 状态"，慢路径通过重新关抢占来维持这个契约。

### 14.4.4 写者如何等读者退完

```c
void __sched percpu_down_write(struct percpu_rw_semaphore *sem)
{
	bool contended = false;

	might_sleep();
	rwsem_acquire(&sem->dep_map, 0, 0, _RET_IP_);

	/* Notify readers to take the slow path. */
	rcu_sync_enter(&sem->rss);

	/*
	 * Try set sem->block; this provides writer-writer exclusion.
	 * Having sem->block set makes new readers block.
	 */
	if (!__percpu_down_write_trylock(sem)) {
		trace_contention_begin(sem, LCB_F_PERCPU | LCB_F_WRITE);
		percpu_rwsem_wait(sem, /* .reader = */ false, false);
		contended = true;
	}

	/* Wait for all active readers to complete. */
	rcuwait_wait_event(&sem->writer, readers_active_check(sem), TASK_UNINTERRUPTIBLE);
	if (contended)
		trace_contention_end(sem, 0);
}
```

逻辑分三步：

1. `rcu_sync_enter()`：让之后进入的读者一律走慢路径，同时启动宽限期。
2. `__percpu_down_write_trylock()`：`atomic_xchg(&sem->block, 1) == 0`，成功即拿到"写者唯一性"；失败说明已有写者，进 `waiters` 队列排队。
3. `rcuwait_wait_event(&sem->writer, readers_active_check(sem), ...)`：反复检查"所有 CPU 的 `read_count` 之和为 0"，直到条件成立。**没有 `synchronize_rcu()` 也没有 in-flight 标志**——宽限期由 `rcu_sync` 管理，读者退出的计数由 `read_count` 汇总。

条件函数是：

```c
static bool readers_active_check(struct percpu_rw_semaphore *sem)
{
	if (per_cpu_sum(*sem->read_count) != 0)
		return false;

	/*
	 * If we observed the decrement; ensure we see the entire critical
	 * section.
	 */
	smp_mb(); /* C matches B */

	return true;
}
```

`per_cpu_sum()` 是 `for_each_possible_cpu()` 逐 CPU 相加，并带 `compiletime_assert_atomic_type()`：它要求累加值类型是"单次读取即原子"的（`unsigned int`/`int`），所以不能用 `unsigned long`。

`percpu_up_write()` 的顺序同样重要：

```c
void percpu_up_write(struct percpu_rw_semaphore *sem)
{
	rwsem_release(&sem->dep_map, _RET_IP_);

	if (trace_contended_release_enabled() && wq_has_sleeper(&sem->waiters))
		trace_call__contended_release(sem);

	atomic_set_release(&sem->block, 0);
	__wake_up(&sem->waiters, TASK_NORMAL, 1, sem);
	rcu_sync_exit(&sem->rss);
}
```

`atomic_set_release()` 保证写者的临界区写入对"之后看到 `block==0` 的读者"可见；但**只有等 `rcu_sync_exit()` 完成宽限期后，快路径才重新开放**——这正是 `percpu_up_write()` 里注释所解释的：不能立刻翻回 fast 状态，否则新读者可能看不到写者的修改。

### 14.4.5 等待队列为什么能用 FIFO 同时服务读者和写者

普通 `wait_queue_head_t` 的唤醒函数只能"唤醒一个 EXCLUSIVE 等待者"，而 percpu-rwsem 需要在一次 `__wake_up()` 中唤醒**多个读者直到遇到一个写者**。本树用一个自定义唤醒函数 + `WQ_FLAG_CUSTOM` 实现：

```c
static int percpu_rwsem_wake_function(struct wait_queue_entry *wq_entry,
				      unsigned int mode, int wake_flags,
				      void *key)
{
	bool reader = wq_entry->flags & WQ_FLAG_CUSTOM;
	struct percpu_rw_semaphore *sem = key;
	struct task_struct *p;

	/* concurrent against percpu_down_write(), can get stolen */
	if (!__percpu_rwsem_trylock(sem, reader))
		return 1;

	p = get_task_struct(wq_entry->private);
	list_del_init(&wq_entry->entry);
	smp_store_release(&wq_entry->private, NULL);

	wake_up_process(p);
	put_task_struct(p);

	return !reader; /* wake (readers until) 1 writer */
}
```

- 入队时设置 `WQ_FLAG_EXCLUSIVE | reader * WQ_FLAG_CUSTOM`：读者带 `CUSTOM` 位，写者不带。
- 唤醒函数返回 0 表示"继续唤醒下一个"，返回 1 表示"到此为止"；`!reader` 恰好实现"连续唤醒读者，遇到第一个写者就停"。
- 每个被唤醒者先做一次 `trylock`（读者自增/写者 xchg），失败说明许可证被别人抢走，直接返回 1 停止批量唤醒，避免惊群。

### 14.4.6 freezable、trylock 与 guard

| 接口 | 特点 |
|--|--|
| `percpu_down_read_freezable(sem, freeze)` | 慢路径睡眠时带 `TASK_FREEZABLE`，可被系统冻结（用于文件系统冻结/休眠流程） |
| `percpu_down_read_trylock(sem)` | 快路径直接成功；慢路径失败返回 false，返回前撤销计数并 `rcuwait_wake_up()` |
| `DEFINE_GUARD(percpu_read, ...)` | `guard(percpu_read)(&sem)` 自动配对 up |
| `DEFINE_GUARD(percpu_write, ...)` | 同上，写侧 |
| `DEFINE_GUARD_COND(percpu_read, _try, ...)` | `scoped_cond_guard(percpu_read_try, ...)` 支持 trylock 语义 |

`__percpu_up_read()` 里还有一个细节：

```c
void __percpu_up_read(struct percpu_rw_semaphore *sem)
{
	lockdep_assert_preemption_disabled();
	...
	smp_mb(); /* B matches C */
	this_cpu_dec(*sem->read_count);
	rcuwait_wake_up(&sem->writer);
}
```

`smp_mb()`（B）与 `readers_active_check()` 里的 `smp_mb()`（C）配对：如果写者观察到了计数减到 0，就一定也能看到读者临界区内的全部写入。

## 14.5 使用场景

| 场景 | 说明 |
|------|------|
| 文件系统挂载状态 | 检查文件系统是否挂载 (读极多, 写极少) |
| 设备热插拔 | 检查设备是否在线 |
| 模块引用计数 | 检查模块是否已卸载 |
| 全局配置 | 检查配置是否正在变更 |

### 14.5.1 内核里的真实使用者

本树中 `DEFINE_STATIC_PERCPU_RWSEM` / `percpu_init_rwsem()` 的使用者都能用一条标准解释：**读侧每秒几十万次，写侧几乎不发生**。

| 实例 | 位置 | 读侧场景 | 写侧场景 |
|--|--|--|--|
| `cpu_hotplug_lock` | `kernel/cpu.c` | `cpus_read_lock()`：任何需要"CPU 拓扑不变"的路径 | CPU 上线/下线 |
| `cgroup_threadgroup_rwsem` | `kernel/cgroup/cgroup.c` | fork/exit 时检查线程组 | cgroup 迁移 |
| `file_rwsem` | `fs/locks.c` | 读 `/proc/locks`、遍历 per-CPU `file_lock_list` | 修改全局锁链表 |
| `sb->s_writers.rw_sem[3]` | `fs/super.c`、`include/linux/fs/super_types.h` | 每次写文件/缺页（`sb_start_write()`） | 冻结/解冻文件系统 |
| `dup_mmap_sem` | `kernel/events/uprobes.c` | fork 时检查 uprobe 状态 | 注册/注销 uprobe |
| `scx_fork_rwsem` | `kernel/sched/ext/ext.c` | sched_ext fork 路径 | 调度器切换 |

`cpu_hotplug_lock` 的包装最能说明问题：

```c
DEFINE_STATIC_PERCPU_RWSEM(cpu_hotplug_lock);

void cpus_read_lock(void)
{
	percpu_down_read(&cpu_hotplug_lock);
}
...
void cpus_write_lock(void)
{
	percpu_down_write(&cpu_hotplug_lock);
}
```

`cpus_read_lock()` 遍布调度器、RCU、workqueue、cpufreq 等路径，如果它是普通 `rw_semaphore`，`atomic_long_t count` 会成为全局 cacheline 争用点；换成 percpu 计数后，每个 CPU 只碰自己的 cacheline。

文件系统冻结的保护链路更复杂，`sb_writers` 有三级：

```c
enum {
	SB_FREEZE_WRITE		= 1,	/* Writes, dir ops, ioctls frozen */
	SB_FREEZE_PAGEFAULT	= 2,	/* Page faults stopped as well */
	SB_FREEZE_FS		= 3,	/* For internal FS use */
};
...
struct sb_writers {
	...
	struct percpu_rw_semaphore	rw_sem[SB_FREEZE_LEVELS];
};

static inline void __sb_start_write(struct super_block *sb, int level)
{
	percpu_down_read_freezable(sb->s_writers.rw_sem + level - 1, true);
}
```

注意这里用的是 `freezable` 变体：文件系统已经冻结时，写者必须能被系统休眠流程"冻住"而不是无限等待。`alloc_super()` 里对三级分别调用 `__percpu_init_rwsem()`。

> 一个常见误解：`super_block.s_umount` 历史上曾是 percpu-rwsem，但本树 `include/linux/fs/super_types.h` 里它是普通的 `struct rw_semaphore`（`sync_filesystem()` 用 `down_read(&sb->s_umount)` 持有的就是它）。真正用 percpu-rwsem 的是 `s_writers.rw_sem[]`。

### 14.5.2 与 rwsem / SRCU / RCU 的取舍

| 维度 | percpu_rw_semaphore | rw_semaphore | SRCU | RCU |
|--|--|--|--|--|
| 读侧开销 | 1 次 per-CPU 自增 | 1 次原子加（可能争用） | 1 次 per-CPU 计数 | 0（仅编译器屏障） |
| 读侧可睡眠 | 快路径否，慢路径是 | 是 | 是 | 否 |
| 写侧等待 | 等所有 CPU 计数归零 + RCU 宽限期 | 等 owner 释放 | 等所有读者 `srcu_read_unlock()` | 等宽限期 |
| 写侧互斥 | `atomic_xchg(&block,1)` | 写锁 | `synchronize_srcu()` 串行 | `synchronize_rcu()` 串行 |
| 读侧是否持有指针引用 | 否 | 否 | 否 | 否 |
| 适用 | 读极多、写极少、临界区可长 | 读写均衡 | 读侧可睡眠 | 指针替换 |

与 SRCU 的区别：SRCU 的读者也可能睡眠，但 `srcu_read_unlock()` 需要把 `idx` 传回去，且写者 `synchronize_srcu()` 的成本随读者数增长；percpu-rwsem 的读者退出只是 `this_cpu_dec()` + 必要时唤醒写者，且读者在慢路径上会真正排队，因此**写者能"锁住"临界区**，语义更接近读写锁。RCU 则完全没有"排他"概念——写者不会阻止新读者，只等旧读者离开。

选择建议：

- 需要"写者看到所有旧读者离开，且新读者被挡住" → `percpu_rw_semaphore`
- 只需要"旧读者离开"、允许新读者继续 → RCU / SRCU
- 读写比例不极端 → 普通 `rw_semaphore`

## 14.6 使用示例

```c
// 示例: 保护文件系统挂载状态
struct percpu_rw_semaphore sb_lock;  // superblock 操作锁
struct super_block *sb;

// 读者: 检查挂载状态
int is_mounted(struct super_block *sb)
{
    int ret;

    percpu_down_read(&sb_lock);
    ret = sb->s_flags & SB_ACTIVE;  // 读挂载标志
    percpu_up_read(&sb_lock);

    return ret;
}

// 写者: 卸载
void unmount_sb(struct super_block *sb)
{
    percpu_down_write(&sb_lock);
    sb->s_flags &= ~SB_ACTIVE;      // 清除挂载标志
    // 执行卸载操作...
    percpu_up_write(&sb_lock);
}
```

注意示例里的 `struct percpu_rw_semaphore sb_lock` 必须用 `percpu_init_rwsem(&sb_lock)` 动态初始化（否则 `read_count` 为 NULL），或改用 `DEFINE_STATIC_PERCPU_RWSEM(sb_lock)`。初始化失败的唯一原因是 `alloc_percpu(int)` 返回 NULL。

### 14.6.1 初始化与释放

```c
int __percpu_init_rwsem(struct percpu_rw_semaphore *sem,
			const char *name, struct lock_class_key *key)
{
	sem->read_count = alloc_percpu(int);
	if (unlikely(!sem->read_count))
		return -ENOMEM;

	rcu_sync_init(&sem->rss);
	rcuwait_init(&sem->writer);
	init_waitqueue_head(&sem->waiters);
	atomic_set(&sem->block, 0);
	...
}
```

`percpu_free_rwsem()` 里有一段注释解释了为什么它必须容忍"半初始化"对象：

```c
	/*
	 * XXX: temporary kludge. The error path in alloc_super()
	 * assumes that percpu_free_rwsem() is safe after kzalloc().
	 */
	if (!sem->read_count)
		return;
```

即 `alloc_super()` 在初始化第 N 个 `s_writers.rw_sem[]` 失败时，会对尚未初始化的成员调用 `percpu_free_rwsem()`，所以函数入口必须判空。释放顺序是 `rcu_sync_dtor()` → `free_percpu()` → 把 `read_count` 置 NULL（"catch use after free bugs"）。

### 14.6.2 调试与配置现状

| 配置项 | 本树取值 | 影响 |
|--|--|--|
| `CONFIG_DEBUG_LOCK_ALLOC` | 未启用 | `dep_map` 不编译，`rwsem_acquire/release` 全为空 |
| `CONFIG_PROVE_LOCKING` | 未启用 | 不会检查 percpu-rwsem 与其它锁的依赖环 |
| `CONFIG_DEBUG_LOCKDEP` | 未启用 | 无 lockdep 自检 |
| `CONFIG_PREEMPT_RT` | `y` | 无影响：percpu-rwsem 的实现不区分 RT，因为它的睡眠点由 `waiters`/`rcuwait` 自己管理 |
| `CONFIG_SMP` | `y` | 需要真实 per-CPU 计数；UP 下 `this_cpu_inc` 退化为普通自增 |
| `CONFIG_TRACEPOINTS` | 未启用 | `trace_contention_begin(LCB_F_PERCPU\|...)` 打点不生效 |

与 `rw_semaphore` 不同，percpu-rwsem 没有 `CONFIG_DEBUG_RWSEMS` 那样的 magic 校验；它能提供的可观测性主要来自 trace 与 `percpu_is_read_locked()` / `percpu_is_write_locked()` 这两个查询接口（后者只是 `atomic_read(&sem->block)`）。

### 14.6.3 常见误用

| 误用 | 后果 | 说明 |
|--|--|--|
| 用 `DEFINE_PERCPU_RWSEM` 却手工 `kzalloc` 结构体 | `read_count == NULL`，第一次读就 oops | 必须让宏或 `percpu_init_rwsem()` 分配 per-CPU 计数 |
| 在中断/原子上下文调用 `percpu_down_read()` | `might_sleep()` 报警；写者在场时慢路径直接睡眠，属于非法上下文 | 读快路径虽轻，但 API 契约是"可睡眠" |
| 把 `percpu_rwsem` 当成"不会阻塞写者" | 写者仍要等所有读者退出 | 它不阻止写者排队，只是让写者等待更便宜 |
| 读侧临界区里做长时间阻塞 I/O | 写者被拖住，且 `percpu_up_read` 无法及时执行 | 快路径虽轻，但读者的"存在"本身就会阻塞写者 |
| 用 `percpu_down_read_trylock()` 后不检查返回值 | 未持锁就访问共享数据 | trylock 失败返回 false，且不改变 `block` |

最后一条尤其隐蔽：`percpu_down_read_trylock()` 在慢路径失败时会把刚加的计数减回去并 `rcuwait_wake_up()`，但**不会**给调用者任何"部分持锁"的状态——返回值 false 就是完全没拿到。

## 14.7 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/percpu-rwsem.h](file:///home/louis/code/linux/include/linux/percpu-rwsem.h) | percpu-rwsem API |
| [kernel/locking/percpu-rwsem.c](file:///home/louis/code/linux/kernel/locking/percpu-rwsem.c) | percpu-rwsem 实现 |