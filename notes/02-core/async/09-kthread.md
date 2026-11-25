# kthread — 内核线程

## 1 实现原理

kthread 是 Linux 内核中创建和管理内核线程的通用框架。内核线程是独立调度的执行单元，运行在内核空间，可访问内核地址空间。核心设计如下：

- **kthread_create() vs kthread_run()**：`kthread_create()` 创建线程但暂不运行，`kthread_run()` 创建并立即唤醒。
- **kthread_worker 框架**：在普通 kthread 基础上提供工作队列风格的接口，支持 `kthread_work` 和 `kthread_delayed_work`。
- **生命周期管理**：`kthread_stop()` 通过设置 `KTHREAD_SHOULD_STOP` 标志通知线程退出。
- **线程函数结构**：线程函数循环检查 `kthread_should_stop()`，并在适当位置退出。

## 2 使用场景

- **长时间后台任务**：watchdog、migration 线程、内存回收（kswapd）。
- **专用 I/O 处理**：每个设备一个线程，如 RAID 重建、加密/压缩。
- **周期性监控**：定期检查系统状态并执行操作。
- **kthread_worker**：需要串行化工作处理的场景，类似 per-CPU workqueue。

## 3 代码调用栈

```
创建线程:
kthread_run(threadfn, data, "name/%u", cpu)
  └→ kthread_create(threadfn, data, "name/%u", cpu)
      └→ __kthread_create_on_node()
          └→ kthread_create_on_node()
              └→ alloc_kthread()  // 分配 kthread 结构
              └→ kthreadd 进程 fork 出新线程
              └→ wait_for_completion(&create.done)  // 等待初始化完成
  └→ wake_up_process(kthread_task)  // 唤醒线程

线程函数:
threadfn(data)
  └→ while (!kthread_should_stop()) {
          // 处理任务
          schedule_timeout_interruptible(HZ);  // 可睡眠
      }
  └→ do_exit(0)  // 线程退出

停止线程:
kthread_stop(task)
  └→ set_bit(KTHREAD_SHOULD_STOP, &kthread->flags)
  └→ wake_up_process(task)
  └→ wait_for_completion(&kthread->exited)  // 等待线程退出
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    kthread 生命周期                               │
│                                                                   │
│  kthread_run(threadfn, data, "name")                             │
│    │                                                              │
│    ▼                                                              │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │  创建阶段                                                │     │
│  │  1. alloc_kthread() 分配 struct kthread                  │     │
│  │  2. kthreadd 进程 fork 新线程 (CLONE_VM|CLONE_UNTRACED) │     │
│  │  3. 新线程执行 kthread() 内部函数                        │     │
│  │  4. 等待创建完成信号                                      │     │
│  │  5. wake_up_process() 唤醒                               │     │
│  └─────────────────────────────────────────────────────────┘     │
│    │                                                              │
│    ▼                                                              │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │  执行阶段                                                │     │
│  │  while (!kthread_should_stop()) {                       │     │
│  │      // 用户提供的 threadfn(data)                       │     │
│  │      // 可睡眠、可被中断                                  │     │
│  │      schedule() / schedule_timeout()                    │     │
│  │  }                                                       │     │
│  └─────────────────────────────────────────────────────────┘     │
│    │                                                              │
│    ▼                                                              │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │  停止阶段                                                │     │
│  │  kthread_stop():                                         │     │
│  │  1. 设置 KTHREAD_SHOULD_STOP 标志                        │     │
│  │  2. wake_up_process() 唤醒线程                           │     │
│  │  3. wait_for_completion() 等待线程退出                   │     │
│  │  4. 线程 do_exit(0)                                     │     │
│  └─────────────────────────────────────────────────────────┘     │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct kthread` — 内核线程内部结构

```c
// kernel/kthread.c (内部结构)
struct kthread {
    unsigned long flags;                // 标志位: KTHREAD_SHOULD_STOP, KTHREAD_IS_PER_CPU 等
    unsigned int cpu;                   // 绑定 CPU
    void *data;                         // 传递给线程函数的数据
    struct completion parked;           // park 同步
    struct completion exited;           // 退出同步
    struct completion *done;            // 创建完成信号
    int result;                         // 线程函数返回值
};
```

### `struct kthread_worker` — kthread 工作者

```c
// include/linux/kthread.h
struct kthread_worker {
    unsigned int flags;                 // 标志位
    raw_spinlock_t lock;                // 保护锁
    struct list_head work_list;         // 待处理工作链表
    struct list_head delayed_work_list; // 延迟工作链表 (需要定时器触发的)
    struct task_struct *task;           // 绑定到的工作线程
    struct kthread_work *current_work;  // 当前正在处理的工作
};
```

### `struct kthread_work` — kthread 工作项

```c
// include/linux/kthread.h
struct kthread_work {
    struct list_head node;              // 链表节点
    kthread_work_func_t func;           // 工作函数
    struct kthread_worker *worker;      // 所属 worker
    int canceling;                      // 正在取消的线程数
};

struct kthread_delayed_work {
    struct kthread_work work;           // 内嵌工作项
    struct timer_list timer;            // 延迟定时器
};
```

### 关键 API

```c
// 创建/运行线程
struct task_struct *kthread_create(int (*threadfn)(void *data),
                                   void *data, const char namefmt[], ...);
struct task_struct *kthread_run(int (*threadfn)(void *data),
                                void *data, const char namefmt[], ...);

// kthread_worker 接口
struct kthread_worker *kthread_create_worker(unsigned int flags,
                                             const char namefmt[], ...);
bool kthread_queue_work(struct kthread_worker *worker,
                        struct kthread_work *work);
bool kthread_queue_delayed_work(struct kthread_worker *worker,
                                struct kthread_delayed_work *dwork,
                                unsigned long delay);

// 停止/控制
int kthread_stop(struct task_struct *k);
bool kthread_should_stop(void);
int kthread_park(struct task_struct *k);
void kthread_unpark(struct task_struct *k);
```

## 6 核心结构核实与创建路径

本树 `struct kthread`（`kernel/kthread.c`）比第 5 节示意更丰富，且通过 `task->worker_private` 关联（`to_kthread()`），标志位枚举为 `KTHREAD_IS_PER_CPU=0`、`KTHREAD_SHOULD_STOP`、`KTHREAD_SHOULD_PARK`：

```c
// kernel/kthread.c
struct kthread {
    unsigned long flags;
    unsigned int cpu;
    unsigned int node;
    int started;
    int result;
    int (*threadfn)(void *);
    void *data;
    struct completion parked;     // park 同步
    struct completion exited;     // 退出同步（也作 vfork_done）
    char *full_name;              // comm 截断时保存全名
    struct task_struct *task;
    struct list_head affinity_node;
    struct cpumask *preferred_affinity;
};
```

创建走 `kthread_create_on_node()` → `__kthread_create_on_node()`：分配 `kthread_create_info` 挂到 `kthread_create_list`，`wake_up_process(kthreadd_task)` 通知 PID 2 的 `kthreadd`，然后 `wait_for_completion_killable(&done)` 等待。`kthreadd` 侧 `create_kthread()` 调 `kernel_thread(kthread, create, CLONE_FS|CLONE_FILES|SIGCHLD)` fork 出新线程。注意第 3 节示意的 `alloc_kthread()` 在本树并不存在，实际是 `set_kthread_struct()` 在 fork 时分配并设 `p->vfork_done = &kthread->exited`。

## 7 kthread 主函数与首次唤醒

新线程入口是 `kthread()`（`kernel/kthread.c`），它先拷贝创建信息、把调度策略重置为 `SCHED_NORMAL`，然后**停在 TASK_UNINTERRUPTIBLE 等首次唤醒**：

```c
// kernel/kthread.c: kthread() 片段
__set_current_state(TASK_UNINTERRUPTIBLE);
create->result = current;
preempt_disable();
complete(done);                 // 通知创建者"已就绪"
schedule_preempt_disabled();    // 停在这里，直到 wake_up_process
preempt_enable();
self->started = 1;
if (!(current->flags & PF_NO_SETAFFINITY) && !self->preferred_affinity)
    kthread_affine_node();      // 默认 NUMA 亲和
if (!test_bit(KTHREAD_SHOULD_STOP, &self->flags)) {
    __kthread_parkme(self);
    ret = threadfn(data);       // 用户线程函数
}
kthread_exit(ret);
```

这解释了 `kthread_create()` 与 `kthread_run()` 的区别：前者创建后线程停在 schedule，需调用者 `wake_up_process()`；后者宏内直接唤醒。`kthread_should_stop()` 即 `test_bit(KTHREAD_SHOULD_STOP, &to_kthread(current)->flags)`。

## 8 park/unpark 与 stop

park 用于 CPU 热插拔等场景临时挂起线程而不销毁：

```c
// kernel/kthread.c: kthread_park()
set_bit(KTHREAD_SHOULD_PARK, &kthread->flags);
if (k != current) {
    wake_up_process(k);
    wait_for_completion(&kthread->parked);          // 等进入 TASK_PARKED
    WARN_ON_ONCE(!wait_task_inactive(k, TASK_PARKED));
}
```

线程侧 `__kthread_parkme()` 循环 `set_special_state(TASK_PARKED)`，若 SHOULD_PARK 仍置位则 `complete(&self->parked)` 后 `schedule_preempt_disabled()`。`kthread_unpark()` 对 per-CPU 线程先 `__kthread_bind()` 重新绑核，再清 SHOULD_PARK 并 `wake_up_state(k, TASK_PARKED)`。

`kthread_stop()` 是销毁路径：

```c
// kernel/kthread.c: kthread_stop()
get_task_struct(k);
set_bit(KTHREAD_SHOULD_STOP, &kthread->flags);
kthread_unpark(k);
set_tsk_thread_flag(k, TIF_NOTIFY_SIGNAL);   // 打断可中断睡眠
wake_up_process(k);
wait_for_completion(&kthread->exited);       // 等线程退出
ret = kthread->result;
put_task_struct(k);
```

`exited` 完成量由 `kthread_exit()` 触发，返回值即 `threadfn` 的返回值。`kthread_stop_put()` 额外释放调用者持有的引用。

## 9 kthread_worker 框架

`kthread_worker` 在单个 kthread 上提供工作队列式接口。`kthread_create_worker_on_node()` 用 `__kthread_create_on_node(kthread_worker_fn, worker, ...)` 创建线程，主循环 `kthread_worker_fn()`：

```c
// kernel/kthread.c: kthread_worker_fn() 骨架
set_current_state(TASK_INTERRUPTIBLE);
if (kthread_should_stop()) { worker->task = NULL; return 0; }
raw_spin_lock_irq(&worker->lock);
if (!list_empty(&worker->work_list)) {
    work = list_first_entry(&worker->work_list, struct kthread_work, node);
    list_del_init(&work->node);
}
worker->current_work = work;
raw_spin_unlock_irq(&worker->lock);
if (work) { work->func(work); }
else if (!freezing(current)) schedule();
try_to_freeze(); cond_resched(); goto repeat;
```

`kthread_queue_work()` 把 work 挂到 `work_list` 并唤醒线程；`kthread_delayed_work` 内嵌 `timer_list` 到期后再入队。与 workqueue 不同，kthread_worker 始终单线程串行、无并发管理。

## 10 与 workqueue 的对比

| 维度 | kthread | kthread_worker | workqueue |
|--|--|--|--|
| 执行单元 | 自定义线程函数 | 单线程跑 work | 每 pool 多 kworker |
| 并发 | 自行管理 | 严格串行 | max_active 控制 |
| 睡眠 | 可 | 可 | 可 |
| 绑核 | `kthread_bind` | 随线程 | per-CPU pool 天然绑核 |
| 内存回收保障 | 无 | 无 | `WQ_MEM_RECLAIM` rescuer |
| 适用 | 长驻后台循环 | 少量串行工作 | 通用延迟工作 |

ksoftirqd、rcuc、irq_workd 等每 CPU 线程都经 `smpboot_register_percpu_thread()` 创建，底层复用 kthread 的 park/unpark 实现热插拔。需要睡眠的通用延迟工作优先用 [工作队列](07-workqueue.md)。

## 11 配置、调试与关键文件

- `trace_sched_kthread_stop/stop_ret`、`trace_sched_kthread_work_execute_start/end`：`CONFIG_FTRACE` 当前配置未启用，源码存在、运行时不可用。
- `ps -eLf`、`/proc/<pid>/status` 观察内核线程；`kthread_full_name` 保留被 comm 截断的全名（`get_kthread_comm()`）。
- `kthread_park()` 对已退出线程返回 `-ENOSYS`，重复 park 返回 `-EBUSY`。

| 文件 | 作用 |
|--|--|
| `kernel/kthread.c` | 创建/主函数/park/stop/kthread_worker/kthreadd |
| `include/linux/kthread.h` | API 声明、`kthread_run` 宏、worker 结构 |
| `kernel/smpboot.c` | 每 CPU 线程框架（复用 park/unpark） |

相关笔记：[工作队列](07-workqueue.md)、[线程化中断](08-threaded-irq.md)、[RCU 回调](06-rcu-callback.md)、[异步执行概要](async-execution-summary.md)。