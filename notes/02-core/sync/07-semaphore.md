# 7. 信号量 (semaphore)

## 7.1 概述

信号量是一种经典的同步原语，由 Edsger Dijkstra 提出，使用一个计数器控制对共享资源的访问。

**核心特性：**
- 计数语义：允许多个线程同时访问有限资源
- 可睡眠：等待时阻塞而非自旋
- 无持有者概念：任何任务都可以释放信号量 (不同于 mutex)
- 在 Linux 内核中已基本被 mutex 和 rwsem 取代

## 7.2 关键数据结构

### 7.2.1 struct semaphore

定义在 [include/linux/semaphore.h](file:///home/louis/code/linux/include/linux/semaphore.h)：

```c
// include/linux/semaphore.h
struct semaphore {
    raw_spinlock_t          lock;           // 保护 count 和 wait_list
    unsigned int            count;          // 可用资源计数
    struct list_head        wait_list;      // 等待者链表 (FIFO)
};
```

**count 字段含义：**
```
count > 0 : 有 count 个可用资源, 获取不会阻塞
count = 0 : 无可用资源, 获取会阻塞
count < 0 : 不适用 (Linux semaphore 不设为负数)
```

### 7.2.2 本树 7.2 的实际字段

上面的 `wait_list` 是经典实现。本树 `include/linux/semaphore.h` 已改成"首等待者 + 单链表"的形式，并可选记录最后持有者：

```c
struct semaphore {
	raw_spinlock_t		lock;
	unsigned int		count;
	struct semaphore_waiter *first_waiter;

#ifdef CONFIG_DETECT_HUNG_TASK_BLOCKER
	unsigned long		last_holder;
#endif
};
```

差异点：

| 项 | 经典实现 | 本树 7.2 |
|--|--|--|
| 等待队列 | `struct list_head wait_list`（独立头节点） | `first_waiter` 指向首个 `semaphore_waiter`，其 `list` 兼作队列头 |
| 队列判空 | `list_empty(&sem->wait_list)` | `!sem->first_waiter` |
| 持有者记录 | 无 | `last_holder`（仅 `CONFIG_DETECT_HUNG_TASK_BLOCKER`） |
| 结构体大小 | 3 个成员 | 3~4 个成员，少一个 list_head |

`CONFIG_DETECT_HUNG_TASK_BLOCKER` 在本树 `.config` 中未启用，因此 `last_holder` 不存在，`hung_task_sem_set_holder()` 是空函数，`sem_last_holder()` 恒返回 `0UL`。开启后，`__sem_acquire()` 会把 `current` 写进 `last_holder`，供 hung task 报告"谁最后拿到了这把锁"。

## 7.3 核心 API

```c
// include/linux/semaphore.h

// 静态初始化
DEFINE_SEMAPHORE(name);              // count = 1, 相当于二值信号量
#define DEFINE_SEMAPHORE(_name) \
    struct semaphore _name = __SEMAPHORE_INITIALIZER(_name, 1)

// 带初始计数的初始化
#define DEFINE_SEMAPHORE(_name, _count) \
    struct semaphore _name = __SEMAPHORE_INITIALIZER(_name, _count)

// 动态初始化
void sema_init(struct semaphore *sem, int val);

// P 操作 (获取资源)
void down(struct semaphore *sem);                          // 获取, 不可中断
int down_interruptible(struct semaphore *sem);              // 可被信号中断
int down_killable(struct semaphore *sem);                   // 可被 SIGKILL 中断
int down_trylock(struct semaphore *sem);                    // 非阻塞尝试
int down_timeout(struct semaphore *sem, long jiffies);      // 超时等待

// V 操作 (释放资源)
void up(struct semaphore *sem);                             // 释放资源
```

### 7.3.1 初始化宏必须带计数

本树的 `DEFINE_SEMAPHORE` 只接受两个参数，没有"默认 1"的单参形式：

```c
#define DEFINE_SEMAPHORE(_name, _n)	\
	struct semaphore _name = __SEMAPHORE_INITIALIZER(_name, _n)
```

`__SEMAPHORE_INITIALIZER` 会同时初始化 `lock`、`count`、`first_waiter`，并在需要时补上 `.last_holder = 0UL`。动态初始化走 `sema_init()`，它额外做 `lockdep_init_map(&sem->lock.dep_map, "semaphore->lock", &__key, 0)`。本树未启用 `CONFIG_LOCKDEP` / `CONFIG_DEBUG_LOCK_ALLOC`，该宏展开为 `do { (void)(name); (void)(key); } while (0)`——连第一个参数 `lock` 都不使用，因此 `&sem->lock.dep_map` 这个表达式根本不会被编译，`raw_spinlock` 里没有 `dep_map` 成员也不影响。

另外注意 `sema_init()` 的 `val` 是 `int` 而 `count` 是 `unsigned int`：传负数会被解释成巨大的计数（实际上是调用者的 bug），内核不做检查。

## 7.4 down_interruptible 调用链

```
down_interruptible(sem)                          [kernel/locking/semaphore.c]
  │
  ├── 1. 尝试递减计数
  │     └── raw_spin_lock_irq(&sem->lock)
  │           │
  │           ├── if (sem->count > 0):
  │           │     sem->count--;                → 资源可用, 直接返回
  │           │     raw_spin_unlock_irq(&sem->lock)
  │           │     return 0;
  │           │
  │           └── else:                          → 资源不可用
  │                 └── 进入慢速路径
  │
  └── 2. 慢速路径: 等待
        └── __down_common(sem, TASK_INTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT)
              │
              ├── list_add_tail(&waiter.list, &sem->wait_list)
              │     └── 加入等待队列 (FIFO)
              │
              ├── while (true):
              │     ├── __set_current_state(state)   → 设置任务状态
              │     │
              │     ├── raw_spin_unlock_irq(&sem->lock)
              │     │
              │     ├── schedule()                   → 调度出去
              │     │
              │     ├── raw_spin_lock_irq(&sem->lock)
              │     │
              │     ├── if (waiter.up)               → 被唤醒
              │     │     return 0;
              │     │
              │     └── if (signal_pending(current))  → 被信号中断
              │           return -EINTR;
              │
              └── list_del(&waiter.list)              → 从等待队列移除
```

## 7.5 up 调用链

```
up(sem)                                            [kernel/locking/semaphore.c]
  │
  └── raw_spin_lock_irqsave(&sem->lock, flags)
        │
        ├── if (list_empty(&sem->wait_list)):
        │     sem->count++;                         → 无等待者, 增加计数
        │
        └── else:
              ├── waiter = list_first_entry(&sem->wait_list)  → 取第一个等待者
              ├── list_del(&waiter->list)                     → 出队
              ├── waiter->up = true                           → 标记唤醒
              └── wake_up_process(waiter->task)               → 唤醒等待任务
        │
        └── raw_spin_unlock_irqrestore(&sem->lock, flags)
```

### 7.5.1 本树 up() 的真实实现

7.5 描述的是经典 `wait_list` 版本。本树 `up()` 已经改用 wake_q 批量唤醒，并把唤醒动作移到解锁之后：

```c
void __sched up(struct semaphore *sem)
{
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irqsave(&sem->lock, flags);

	hung_task_sem_clear_if_holder(sem);

	if (likely(!sem->first_waiter))
		sem->count++;
	else
		__up(sem, &wake_q);

	if (trace_contended_release_enabled() && !wake_q_empty(&wake_q))
		trace_call__contended_release(sem);

	raw_spin_unlock_irqrestore(&sem->lock, flags);
	if (!wake_q_empty(&wake_q))
		wake_up_q(&wake_q);
}
```

`__up()` 只做"摘头 + 置 up 标志 + 把任务挂进 wake_q"：

```c
static noinline void __sched __up(struct semaphore *sem,
				  struct wake_q_head *wake_q)
{
	struct semaphore_waiter *waiter = sem->first_waiter;

	sem_del_waiter(sem, waiter);
	waiter->up = true;
	wake_q_add(wake_q, waiter->task);
}
```

wake_q 机制的意义：`wake_up_process()` 需要拿目标任务的 `pi_lock` 与 runqueue 锁，如果仍在 `sem->lock` 里做，会把信号量内部的 raw spinlock 临界区拉长，增加中断延迟（`up()` 可以从中断上下文调用）。所以先"记账"，解锁后再真正唤醒。

### 7.5.2 等待队列的结构：first_waiter 兼作队头

`___down_common()` 用了一个省内存的技巧——让**第一个等待者的 `list` 节点同时充当队列头**：

```c
static inline int __sched ___down_common(struct semaphore *sem, long state,
							long timeout)
{
	struct semaphore_waiter waiter, *first;

	first = sem->first_waiter;
	if (first) {
		list_add_tail(&waiter.list, &first->list);
	} else {
		INIT_LIST_HEAD(&waiter.list);
		sem->first_waiter = &waiter;
	}
	waiter.task = current;
	waiter.up = false;

	for (;;) {
		if (signal_pending_state(state, current))
			goto interrupted;
		if (unlikely(timeout <= 0))
			goto timed_out;
		__set_current_state(state);
		raw_spin_unlock_irq(&sem->lock);
		timeout = schedule_timeout(timeout);
		raw_spin_lock_irq(&sem->lock);
		if (waiter.up) {
			hung_task_sem_set_holder(sem);
			return 0;
		}
	}

 timed_out:
	sem_del_waiter(sem, &waiter);
	return -ETIME;

 interrupted:
	sem_del_waiter(sem, &waiter);
	return -EINTR;
}
```

于是 `sem->first_waiter` 既是"队头指针"又是"链表头所属节点"。摘除时必须区分两种情况：

```c
static inline
void sem_del_waiter(struct semaphore *sem, struct semaphore_waiter *waiter)
{
	if (list_empty(&waiter->list)) {
		sem->first_waiter = NULL;
		return;
	}

	if (sem->first_waiter == waiter) {
		sem->first_waiter = list_first_entry(&waiter->list,
						     struct semaphore_waiter, list);
	}
	list_del(&waiter->list);
}
```

- 只有自己一个等待者时，`list_empty(&waiter->list)` 为真（自环），直接清空 `first_waiter`。
- 自己是队头但后面还有人时，把 `first_waiter` 推进到下一个节点，再 `list_del()`。
- 自己不是队头时直接 `list_del()`，`first_waiter` 不动。

这也解释了为什么 `sem->first_waiter` 的判空写成 `!sem->first_waiter` 而不是 `list_empty()`：队列为空时它本来就是 NULL。

### 7.5.3 四种等待变体只差两个参数

`down()`/`down_interruptible()`/`down_killable()`/`down_timeout()` 的慢路径全部汇聚到 `__down_common(sem, state, timeout)`：

| API | state | timeout | 可被信号打断 | 失败返回 |
|--|--|--|--|--|
| `down()` | `TASK_UNINTERRUPTIBLE` | `MAX_SCHEDULE_TIMEOUT` | 否 | 不返回（无失败路径） |
| `down_interruptible()` | `TASK_INTERRUPTIBLE` | `MAX_SCHEDULE_TIMEOUT` | 是 | `-EINTR` |
| `down_killable()` | `TASK_KILLABLE` | `MAX_SCHEDULE_TIMEOUT` | 仅致命信号 | `-EINTR` |
| `down_timeout()` | `TASK_UNINTERRUPTIBLE` | 调用者给的 jiffies | 否 | `-ETIME` |

注意 `___down_common()` 在睡眠前就判 `timeout <= 0`：已经到期的请求会立刻返回 `-ETIME`，不会先入队再出队。判断顺序之所以是"先看信号、再看超时"，是因为对可中断变体而言信号优先；而 `down_timeout()` 用的是 `TASK_UNINTERRUPTIBLE`，`signal_pending_state()` 对它恒为假，所以它只会返回 0 或 `-ETIME`。

### 7.5.4 down_trylock 的返回值是反的

```c
int __sched down_trylock(struct semaphore *sem)
{
	unsigned long flags;
	int count;

	raw_spin_lock_irqsave(&sem->lock, flags);
	count = sem->count - 1;
	if (likely(count >= 0))
		__sem_acquire(sem);
	raw_spin_unlock_irqrestore(&sem->lock, flags);

	return (count < 0);
}
```

成功获取返回 **0**，失败返回 **1**——与 `mutex_trylock()`（成功 1、失败 0）和 `spin_trylock()` 相反。源码注释专门警告了这一点。由于返回值不是 `-EBUSY`，调用者不能把它当 errno 用。

同时它是唯一"可在任意上下文调用"的获取接口：`down_trylock()` 与 `up()` 都不睡眠，因此在中断上下文里也安全（`raw_spin_lock_irqsave` 会把中断关掉）。`up()` 甚至允许"从未调用过 down 的任务"释放，这一点与 mutex 完全不同。

## 7.6 信号量 vs mutex 对比

| 特性 | semaphore | mutex |
|------|-----------|-------|
| 计数 | 任意值 (资源计数) | 二值 (0/1) |
| 持有者 | 无 | 有 (task_struct) |
| 释放者 | 任何任务 | 仅持有者 |
| 互斥语义 | 弱 | 强 |
| 乐观自旋 | 无 | 有 |
| 调试支持 | 少 | 丰富 (lockdep) |
| 可递归 | 是 | 否 |
| 内核推荐度 | 不推荐 | 优先使用 |

### 7.6.1 与 mutex / rwsem / completion 的四方对比

| 维度 | semaphore | mutex | rwsem | completion |
|--|--|--|--|--|
| 计数 | N（`count`） | 1 | 1 写 / N 读 | 一次性事件计数 |
| 持有者 | 无 | `owner` | `owner`（写） | 无 |
| 可从中断上下文获取 | `down_trylock` 可以 | 不可以 | 不可以 | 不可以（`wait_for_completion` 睡眠） |
| 可从中断上下文释放 | 可以 | 不可以 | 不可以 | 可以（`complete`） |
| 优先级继承 | 无 | 有（RT 下） | 有（RT 下） | 无 |
| 公平性 | 严格 FIFO（`up()` 直接把许可交给队头，不增加 `count`） | 有 waiter 时排队 | 写者优先/排队 | 无 |
| 调试支持 | trace `contention_*`、hung task | lockdep、debugfs、hung task | lockdep、`DEBUG_RWSEMS` | 无 |
| 典型用途 | 资源池、跨上下文同步 | 互斥临界区 | 读多写少 | 等待某事件完成 |

与 completion 的关键区别：completion 记的是"事件发生次数"——`complete()` 让 `done++` 并唤醒一个等待者，若当时无人等待，这个"已发生"会保留给下一个 `wait_for_completion()`；`complete_all()` 把 `done` 直接设为 `UINT_MAX` 并 `swake_up_all_locked()`，此后所有等待者立即通过。semaphore 的 `down()` 则必须"消耗一个许可"，`up()` 产生的许可只会被队头等待者取走，后来的调用者无法插队。

### 7.6.2 内核里的真实使用者

| 使用者 | 位置 | 用法要点 |
|--|--|--|
| `console_sem` | `kernel/printk/printk.c` | `DEFINE_SEMAPHORE(console_sem, 1)`；`console_lock()` 用 `down()`，打印路径用 `down_trylock_console_sem()`（`printk` 可能在中断上下文，绝不能睡眠） |
| `kmod_concurrent_max` | `kernel/module/kmod.c` | `DEFINE_SEMAPHORE(kmod_concurrent_max, MAX_KMOD_CONCURRENT)`；`down_timeout()` 限制"同时运行的 modprobe 数量"，超时则放弃并告警 |
| `rxrpc_call_limiter` | `net/rxrpc/call_object.c` | 计数 1000，限制并发 RPC 调用数 |
| `efi_runtime_lock` | `drivers/firmware/efi/runtime-wrappers.c` | 二值信号量保护 EFI runtime service 调用 |

`console_sem` 是最能体现"信号量不可替代"的例子：`printk()` 在 NMI/中断上下文里也要尝试拿控制台锁，而 mutex 在原子上下文里会触发 `might_sleep()` 警告，`down_trylock()` 则是合法的。

## 7.7 使用场景

| 场景 | 说明 |
|------|------|
| 有限资源池 | 控制最多 N 个并发访问 |
| 二值信号量 | 替代 mutex 的旧代码 |
| 驱动兼容 | 旧设备驱动代码 |
| 通知机制 | 简单的信号通知 |

**现代内核推荐：** 除非需要计数语义 (多个凭据)，否则优先使用 `mutex` 或 `rwsem`。

### 7.7.1 PREEMPT_RT 下的信号量

本树 `CONFIG_PREEMPT_RT=y`。在这个配置下，`spinlock_t` 被映射成 `rt_mutex`（`include/linux/spinlock_types.h` 的 `#else` 分支），`rw_semaphore` 被映射成 `rwbase_rt`，但**信号量没有被重写**：`kernel/locking/semaphore.c` 里没有任何 `CONFIG_PREEMPT_RT` 条件编译，`struct semaphore` 内部仍然是 `raw_spinlock_t`。

这带来两个后果：

1. **`sem->lock` 临界区仍然不可抢占**。RT 内核里普通的 `spin_lock()` 会睡眠（变成 rtmutex），但 `raw_spin_lock_irqsave()` 依然关闭抢占/中断。信号量的临界区只有"改 count、挂/摘链表、把任务塞进 wake_q"几行，因此可以接受；代价是 `up()` 的唤醒必须在解锁后完成，这正是 7.5.1 中 wake_q 的作用。
2. **等待路径仍然可抢占**。`schedule_timeout()` 在 RT 下行为与普通内核一致，`down()` 让出 CPU 后高优先级任务可以立即运行。但信号量没有优先级继承：如果低优先级任务持有"许可"，高优先级任务在 `down()` 上等待时不会把低优先级任务"拉高"，这可能造成优先级反转。RT 场景下若要避免，应改用 `rt_mutex`（`mutex` 在 RT 下即 rtmutex）。

### 7.7.2 可观测性

| 手段 | 开启条件 | 能看到什么 |
|--|--|--|
| `trace_contention_begin/end` | `CONFIG_TRACEPOINTS` + `CONFIG_FTRACE`（本树未启用） | 谁在何时因哪个信号量阻塞、阻塞了多久 |
| `trace_contended_release` | 同上 | `up()` 唤醒等待者时打点（仅在有实际唤醒时） |
| `last_holder` / `sem_last_holder()` | `CONFIG_DETECT_HUNG_TASK_BLOCKER`（本树未启用） | 最后成功获取者 |
| hung task detector | `CONFIG_DETECT_HUNG_TASK`（本树未启用） | 任务 D 状态超时后打印栈，并指出阻塞在 semaphore |
| `lockdep` | `CONFIG_PROVE_LOCKING`（本树未启用） | 只对内部的 `raw_spinlock_t` 生效，不检查"谁该释放" |

本树 `include/generated/autoconf.h` 中这些符号都没有定义，因此上面的打点与检查全部是"源码存在、当前配置不启用"；计时代码被宏消掉后，`up()`/`down()` 的 fast path 只剩一次 `raw_spin_lock_irqsave` 和 `count` 运算。

`trace_contention_begin()` 的第二个参数是 `LCB_F_*` 标志位（`include/trace/events/lock.h`），信号量固定传 `0`，而 percpu-rwsem 会传 `LCB_F_PERCPU | LCB_F_READ` 这类组合——因此用 trace 过滤时可以把不同锁类型区分开。

### 7.7.3 常见误用

| 误用 | 后果 | 正确做法 |
|--|--|--|
| 用 `down()` 在原子上下文等待 | 调度器 BUG（`scheduling while atomic`） | 只用 `down_trylock()` |
| 把 `down_trylock()` 返回值当 errno | 把"成功"当成错误 | 记住 0=成功、1=失败 |
| 忘记 `up()` | 资源永久泄漏，等待者 hung | 用 cleanup guard 或成对审查 |
| 依赖"谁 down 谁 up" | 信号量无持有者，逻辑错误难以发现 | 需要严格配对时用 mutex |
| 把信号量当 completion 用 | `up()` 可能被"路过"的 `down()` 取走 | 事件通知用 completion |
| 在 RT 高优先级路径用信号量做互斥 | 优先级反转 | 用 mutex/rtmutex |

其中"信号量无持有者"是最容易埋坑的一条：`up()` 可以由完全无关的任务甚至中断调用，内核不会检查调用者是否曾经 `down()` 过。这把"锁"的语义削弱为"许可计数"，也意味着调试器无法用 owner 信息定位问题。

## 7.8 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/semaphore.h](file:///home/louis/code/linux/include/linux/semaphore.h) | semaphore API |
| [include/linux/sched/wake_q.h](file:///home/louis/code/linux/include/linux/sched/wake_q.h) | `up()` 使用的批量唤醒队列 |
| [include/linux/hung_task.h](file:///home/louis/code/linux/include/linux/hung_task.h) | `BLOCKER_TYPE_SEM` 与 `hung_task_set_blocker()` |
| [include/trace/events/lock.h](file:///home/louis/code/linux/include/trace/events/lock.h) | `contention_begin/end`、`contended_release` 事件定义 |
| [kernel/printk/printk.c](file:///home/louis/code/linux/kernel/printk/printk.c) | `console_sem` 的真实用法（含原子上下文 trylock） |
| [kernel/locking/semaphore.c](file:///home/louis/code/linux/kernel/locking/semaphore.c) | semaphore 实现 |