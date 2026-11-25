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