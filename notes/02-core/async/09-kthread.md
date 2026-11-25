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