# Workqueue — 工作队列

## 1 实现原理

Workqueue 是 Linux 内核中最重要的进程上下文异步执行机制，允许在 kworker 线程中执行可睡眠的工作项。核心设计如下：

- **kworker 线程池**：系统维护多个 kworker 线程池（`worker_pool`），每个 pool 管理一组 worker 线程。
- **CMWQ（Concurrency Managed Workqueue）**：并发管理工作队列，自动管理 worker 线程数量，确保 CPU 不会因过多 worker 而超载。
- **多类型支持**：
  - `per-CPU wq`：绑定到特定 CPU，每个 CPU 有独立的 worker 池
  - `unbound wq`：不绑定 CPU，可由任意空闲 worker 处理
  - `ordered wq`：严格按入队顺序串行执行
  - `BH wq`：在 softirq 上下文执行（见 BH Workqueue）
- **工作项状态机**：`work_struct` 通过 `data` 字段的 `WORK_STRUCT_PENDING`、`WORK_STRUCT_PWQ` 等标志管理状态。

## 2 使用场景

- **需要睡眠的延迟工作**：文件系统回写、设备 probe、网络协议处理。
- **通用异步任务**：`schedule_work()` 快速调度一个工作项。
- **周期性任务**：`delayed_work` 支持延迟执行。
- **系统 wq 类型**：

| 系统 wq | 特性 |
|--|--|
| `system_wq` | 默认，per-CPU（已弃用，建议用 system_percpu_wq） |
| `system_percpu_wq` | per-CPU，通用 |
| `system_highpri_wq` | 高优先级 per-CPU |
| `system_long_wq` | 适合长时间运行的工作 |
| `system_unbound_wq` | 不绑定 CPU |
| `system_dfl_wq` | 默认 unbound，无并发管理 |
| `system_bh_wq` | BH softirq 上下文（见 BH Workqueue） |

## 3 代码调用栈

```
调度工作:
schedule_work(&work)
  └→ queue_work(system_percpu_wq, &work)
      └→ queue_work_on(WORK_CPU_UNBOUND, wq, &work)
          └→ __queue_work() 或 __queue_work_cpu()
              └→ 选择目标 worker_pool
              └→ 将 work 加入 pool->worklist
              └→ 如果需要 → wake_up_worker() 唤醒 kworker

kworker 线程处理:
worker_thread()
  └→ worker_process_works()
      └→ 从 pool->worklist 取一个 work
      └→ set_work_current(worker, work)
      └→ work->func(work)  ← 执行回调
      └→ set_work_current(worker, NULL)
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    Workqueue 调度与执行流程                        │
│                                                                   │
│  queue_work(wq, &work)                                            │
│    └→ 选择 target CPU 和 worker_pool                              │
│    └→ 将 work 加入 pool->worklist                                 │
│    └→ 如果 pool 中有空闲 worker → 唤醒                            │
│    └→ 如果无空闲 worker → 创建新 worker (管理者线程)              │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    kworker 线程处理流程                            │
│                                                                   │
│  ┌──────────────┐     ┌──────────────────┐                       │
│  │  idle 状态   │────→│  从 worklist 取  │                       │
│  │ (睡眠中)     │     │  一个 work       │                       │
│  └──────────────┘     └────────┬─────────┘                       │
│        ▲                       │                                  │
│        │                       ▼                                  │
│        │               ┌──────────────────┐                       │
│        │               │  set_work_current │                       │
│        │               │  (标记正在执行)   │                       │
│        │               └────────┬─────────┘                       │
│        │                       │                                  │
│        │               ┌──────────────────┐                       │
│        │               │  work->func(work) │  ← 执行回调          │
│        │               └────────┬─────────┘                       │
│        │                       │                                  │
│        │               ┌──────────────────┐                       │
│        │               │  检查是否需要    │                       │
│        │               │  创建新 worker   │                       │
│        │               └────────┬─────────┘                       │
│        │                       │                                  │
│        │               ┌──────────────────┐                       │
│        │               │  再次检查 worklist│                       │
│        │               └────────┬─────────┘                       │
│        │                       │                                  │
│        └────────────────────────┘                                  │
│            (worklist 为空 → 睡眠)                                 │
│                                                                   │
│  CMWQ 并发控制:                                                    │
│  - 一个 worker 处理一个 work 时，如果有其他 work 等待               │
│    且当前 worker 未 sleep，则不会创建新 worker                     │
│  - 如果 worker 在回调中 sleep，则创建新 worker 维持并发            │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct work_struct` — 工作项

```c
// include/linux/workqueue_types.h
struct work_struct {
    atomic_long_t data;         // 编码: 低比特位 = 标志, 高比特位 = worker_pool 指针
    struct list_head entry;     // 链表节点 (挂入 pool->worklist)
    work_func_t func;           // 回调函数: void (*work_func_t)(struct work_struct *work)
#ifdef CONFIG_LOCKDEP
    struct lockdep_map lockdep_map;
#endif
};
```

### `struct delayed_work` — 延迟工作项

```c
// include/linux/workqueue.h
struct delayed_work {
    struct work_struct work;        // 内嵌 work_struct
    struct timer_list timer;        // 延迟定时器
    struct workqueue_struct *wq;    // 目标 workqueue
    int cpu;                        // 目标 CPU
};
```

### `struct worker` — 工作者线程描述符

```c
// kernel/workqueue_internal.h
struct worker {
    union {
        struct list_head entry;     // 空闲时: 挂入 pool->idle_list
        struct hlist_node hentry;   // 繁忙时: 挂入 pool->busy_hash
    };
    struct work_struct *current_work;   // 当前正在处理的工作
    work_func_t current_func;           // 当前工作的回调函数
    struct pool_workqueue *current_pwq; // 当前工作的 pwq
    struct list_head scheduled;         // 已调度的工作链表
    struct task_struct *task;           // 对应的内核线程
    struct worker_pool *pool;           // 所属 worker_pool
    unsigned int flags;                 // 标志位
    int id;                             // worker ID
    // ...
};
```

### `struct worker_pool` — 工作者线程池

```c
// kernel/workqueue.c (内部定义)
struct worker_pool {
    spinlock_t lock;                    // 保护锁
    int id;                             // pool ID
    int cpu;                            // 绑定 CPU (-1 表示 unbound)
    unsigned int flags;                 // 标志位
    struct list_head worklist;          // 待处理的工作链表
    int nr_workers;                     // worker 总数
    int nr_idle;                        // 空闲 worker 数
    struct list_head idle_list;         // 空闲 worker 链表
    struct timer_list idle_timer;       // idle 超时定时器
    struct idr worker_idr;              // worker ID 分配器
    // ...
};
```

### 关键 API

```c
// 创建工作队列
struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...);
struct workqueue_struct *alloc_ordered_workqueue(const char *fmt, unsigned int flags, ...);

// 调度工作
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work);
bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
                        unsigned long delay);
bool schedule_work(struct work_struct *work);  // 使用 system_percpu_wq

// 取消/等待
bool cancel_work_sync(struct work_struct *work);
void flush_workqueue(struct workqueue_struct *wq);
```