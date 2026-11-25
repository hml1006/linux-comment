# BH Workqueue — 下半部工作队列

## 1 实现原理

BH Workqueue 是 Linux 内核中用于替代 Tasklet 的新标准下半部机制，通过 workqueue 接口封装 softirq 执行上下文。核心设计如下：

- **基于 Softirq 的 workqueue**：`system_bh_wq` 和 `system_bh_highpri_wq` 是内建的工作队列，其工作项在 `TASKLET_SOFTIRQ` softirq 上下文中执行。
- **每 CPU 串行**：每个 CPU 上的 BH workqueue 是串行执行的，但不同 CPU 之间可并行。
- **不可睡眠**：执行上下文是 softirq（硬中断），回调函数不能睡眠。
- **支持 lockdep**：相比于 tasklet，BH workqueue 完全集成 lockdep 死锁检测。
- **接口统一**：使用标准的 `queue_work()` / `INIT_WORK()` API，与普通 workqueue 一致。

## 2 使用场景

- **替代 Tasklet 的新标准下半部机制**：`system_bh_wq` 用于普通下半部，`system_bh_highpri_wq` 用于高优先级下半部。
- **驱动下半部**：网卡驱动、USB 驱动等需要串行执行的下半部处理。
- **需要 lockdep 检测的场景**：相比 tasklet，BH workqueue 能更好地检测死锁。

## 3 代码调用栈

```
注册 BH work:
INIT_WORK(&work, my_bh_handler);
queue_work(system_bh_wq, &work);

执行路径:
softirq 触发 (TASKLET_SOFTIRQ)
  └→ tasklet_action() (softirq 回调)
      └→ workqueue_softirq_action(false)    // 处理 system_bh_wq
          └→ 遍历 per-CPU BH work 链表
              └→ 执行每个 work 的 func

高优先级版本:
queue_work(system_bh_highpri_wq, &work);
  └→ 通过 tasklet_hi_action() 在 HI_SOFTIRQ 中执行
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    BH Workqueue 工作流程                          │
│                                                                   │
│  注册阶段:                                                        │
│  ┌──────────────┐    ┌──────────────────┐                        │
│  │ INIT_WORK()  │───→│ 定义 work_struct  │                        │
│  └──────────────┘    └──────────────────┘                        │
│         │                                                         │
│         ▼                                                         │
│  ┌──────────────────────┐                                        │
│  │ queue_work(system_bh │                                        │
│  │ _wq, &work)          │                                        │
│  └──────────────────────┘                                        │
│         │                                                         │
│         ▼                                                         │
│  标记 TASKLET_SOFTIRQ pending                                    │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    执行阶段 (softirq 上下文)                      │
│                                                                   │
│  irq_exit() → __do_softirq()                                     │
│    └→ TASKLET_SOFTIRQ 被触发                                     │
│        └→ tasklet_action()                                       │
│            └→ workqueue_softirq_action(false)                    │
│                └→ 遍历 per-CPU BH work 链表                      │
│                    └→ work->func(work)  (不可睡眠)               │
│                                                                   │
│  高优先级:                                                        │
│  HI_SOFTIRQ → tasklet_hi_action()                                │
│    └→ workqueue_softirq_action(true)                             │
│        └→ system_bh_highpri_wq 的工作项                          │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### 系统 BH Workqueue 定义

```c
// include/linux/workqueue.h
extern struct workqueue_struct *system_bh_wq;          // 普通 BH workqueue
extern struct workqueue_struct *system_bh_highpri_wq;  // 高优先级 BH workqueue

// 注释说明:
// system_bh[_highpri]_wq are convenience interface to softirq.
// BH work items are executed in the queueing CPU's BH context
// in the queueing order.
```

### 与 Softirq 的集成

```c
// kernel/softirq.c
// tasklet_action() 中调用 workqueue_softirq_action()
// 将 BH workqueue 的工作项在 TASKLET_SOFTIRQ 上下文中执行

static __latent_entropy void tasklet_action(void)
{
    workqueue_softirq_action(false);  // 处理 system_bh_wq
    tasklet_action_common(this_cpu_ptr(&tasklet_vec), TASKLET_SOFTIRQ);
}

static __latent_entropy void tasklet_hi_action(void)
{
    workqueue_softirq_action(true);   // 处理 system_bh_highpri_wq
    tasklet_action_common(this_cpu_ptr(&tasklet_hi_vec), HI_SOFTIRQ);
}
```

### 标准 work_struct（复用普通 workqueue 数据结构）

```c
// include/linux/workqueue_types.h
struct work_struct {
    atomic_long_t data;       // 工作状态和工作池指针
    struct list_head entry;   // 链表节点
    work_func_t func;         // 回调函数
#ifdef CONFIG_LOCKDEP
    struct lockdep_map lockdep_map;
#endif
};
```

## 6 BH worker pool 与伪 worker

BH workqueue 并不创建真正的 kworker 线程，而是复用 softirq 执行上下文。`kernel/workqueue.c` 为每 CPU 维护两个 BH worker pool（普通与高优先级），并打上 `POOL_BH` 标志：

```c
// kernel/workqueue.c
static DEFINE_PER_CPU_SHARED_ALIGNED(struct worker_pool [NR_STD_WORKER_POOLS], bh_worker_pools);
static DEFINE_PER_CPU_SHARED_ALIGNED(struct irq_work [NR_STD_WORKER_POOLS], bh_pool_irq_works);
```

`workqueue_init_early()` 里 `for_each_bh_worker_pool()` 调 `init_cpu_worker_pool()` 并 `pool->flags |= POOL_BH`，同时 `init_irq_work(bh_pool_irq_work(pool), irq_work_fns[i])` 初始化用于跨 CPU 唤醒的 irq_work。`workqueue_init()` 阶段为所有 possible CPU 的 BH pool `create_worker()`——注释明确这是一个"伪 worker"（pseudo worker），代表共享的 BH 执行上下文，不随 CPU 热插拔增删，也没有对应的 `task_struct`。

系统 BH 队列在 `workqueue_init_early()` 末尾创建：

```c
// kernel/workqueue.c
system_bh_wq = alloc_workqueue("events_bh", WQ_BH | WQ_PERCPU, 0);
system_bh_highpri_wq = alloc_workqueue("events_bh_highpri",
                                       WQ_BH | WQ_HIGHPRI | WQ_PERCPU, 0);
```

## 7 WQ_BH 标志与创建约束

`WQ_BH` 定义在 `include/linux/workqueue.h`：`WQ_BH = 1 << 0`，注释为"execute in bottom half (softirq) context"。允许的组合由 `__WQ_BH_ALLOWS = WQ_BH | WQ_HIGHPRI | WQ_PERCPU` 限定。`__alloc_workqueue()` 对 BH 队列有专门校验：

```c
// kernel/workqueue.c: __alloc_workqueue()
if (flags & WQ_BH) {
    if (WARN_ON_ONCE(flags & ~__WQ_BH_ALLOWS))
        return NULL;
    if (WARN_ON_ONCE(max_active))
        return NULL;
}
...
if (flags & WQ_BH) {
    /* BH workqueues always share a single execution context per CPU
     * and don't impose any max_active limit. */
    max_active = INT_MAX;
}
```

即 BH 队列不能带 `WQ_UNBOUND`/`WQ_MEM_RECLAIM` 等标志，也不能设 `max_active`；每 CPU 只有一个执行上下文，天然串行，故无需并发上限。

## 8 入队唤醒：kick_bh_pool 与 softirq 衔接

普通 workqueue 入队后 `kick_pool()` 唤醒 kworker；BH pool 走专门分支：

```c
// kernel/workqueue.c: kick_pool()
if (pool->flags & POOL_BH) {
    kick_bh_pool(pool);
    return true;
}
```

`kick_bh_pool()` 区分本 CPU 与远端 CPU：

```c
// kernel/workqueue.c: kick_bh_pool()
if (unlikely(pool->cpu != smp_processor_id() &&
             !(pool->flags & POOL_BH_DRAINING))) {
    irq_work_queue_on(bh_pool_irq_work(pool), pool->cpu);  // 远端：发 irq_work
    return;
}
if (pool->attrs->nice == HIGHPRI_NICE_LEVEL)
    raise_softirq_irqoff(HI_SOFTIRQ);
else
    raise_softirq_irqoff(TASKLET_SOFTIRQ);
```

本 CPU 直接 raise 对应 softirq；远端 CPU 则借助 [irq_work](01-irq_work.md) 发 IPI，由目标 CPU 的中断返回路径再 raise softirq。这正是 BH workqueue 与 softirq 的衔接点。

## 9 执行：workqueue_softirq_action 与 bh_worker

`TASKLET_SOFTIRQ`/`HI_SOFTIRQ` 的回调 `tasklet_action()`/`tasklet_hi_action()`（`kernel/softirq.c`）**先**调 `workqueue_softirq_action(false/true)` 处理 BH work，**再**处理传统 tasklet：

```c
// kernel/workqueue.c
void workqueue_softirq_action(bool highpri)
{
    struct worker_pool *pool =
        &per_cpu(bh_worker_pools, smp_processor_id())[highpri];
    if (need_more_worker(pool))
        bh_worker(list_first_entry(&pool->workers, struct worker, node));
}
```

`bh_worker()` 结构仿照 `worker_thread()`，但带与 softirq 一致的限额：

```c
// kernel/workqueue.c
#define BH_WORKER_JIFFIES   msecs_to_jiffies(2)
#define BH_WORKER_RESTARTS  10
// bh_worker() 内：
do {
    work = list_first_entry(&pool->worklist, struct work_struct, entry);
    if (assign_work(work, worker, NULL))
        process_scheduled_works(worker);
} while (keep_working(pool) && --nr_restarts && time_before(jiffies, end));
```

源码注释（`workqueue_softirq_action` 上方）写明这是过渡形态：待所有 tasklet 用户转换到 workqueue 后，将直接用 softirq action 承载 BH worker。lockdep 上 BH work 用 `bh_key`、普通线程化 work 用 `thr_key` 区分锁上下文。

## 10 CPU 下线排空与配置调试

CPU 下线时 `takeover_tasklets()`（`kernel/softirq.c`）先调 `workqueue_softirq_dead(cpu)`。后者对每个仍有 pending 的 BH pool，在**别的 CPU** 的 BH 队列上排一个 `drain_dead_softirq_workfn`，置 `POOL_BH_DRAINING` 标志后 `bh_worker()` 排空；若一次没跑完（撞限额）则重新入队自己，避免霸占当前 CPU 的 BH，最后 `complete()` 通知等待者。

- 本树 `CONFIG_WQ_WATCHDOG` 未启用（源码存在，当前配置未启用），否则可检测 work 卡死。
- `CONFIG_PROVE_LOCKING`/`CONFIG_DEBUG_LOCK_ALLOC` 未启用，lockdep 的 `bh_key` 标注运行时不生效。
- BH work 回调运行于 softirq，`in_serving_softirq()` 为真，不可睡眠；需要睡眠应改用普通 workqueue（见 [工作队列](07-workqueue.md)）。

## 11 关键文件清单

| 文件 | 作用 |
|--|--|
| `kernel/workqueue.c` | `bh_worker_pools`/`kick_bh_pool`/`bh_worker`/`workqueue_softirq_action`/`workqueue_softirq_dead` |
| `include/linux/workqueue.h` | `WQ_BH`、`__WQ_BH_ALLOWS`、`system_bh_wq` 声明 |
| `kernel/softirq.c` | `tasklet_action`/`tasklet_hi_action` 调用入口、`takeover_tasklets` |
| `kernel/irq_work.c` | 跨 CPU `kick_bh_pool` 依赖的 irq_work |

## 12 与 tasklet 的对比

| 维度 | tasklet | BH workqueue |
|--|--|--|
| 接口 | `tasklet_setup`/`tasklet_schedule` | `INIT_WORK`/`queue_work` |
| 状态管理 | `state` 位 + `count` 禁用计数 | 复用 workqueue 的 `data` 状态位 |
| 取消/同步 | `tasklet_kill`/`tasklet_disable` | `cancel_work_sync`/`flush_work` |
| lockdep | 支持有限 | 完整集成（`bh_key`） |
| 执行顺序 | 同一 softirq 向量内 | 先于 tasklet 执行 |
| 定位 | 已弃用 | 推荐替代 |

二者共享 `TASKLET_SOFTIRQ`/`HI_SOFTIRQ` 向量与 softirq 限额，因此 BH workqueue 可视为"带 workqueue 生命周期管理的 tasklet"。

相关笔记：[软中断](02-softirq.md)、[tasklet](04-tasklet.md)、[工作队列](07-workqueue.md)、[异步执行概要](async-execution-summary.md)。