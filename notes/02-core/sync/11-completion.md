# 11. 完成量 (completion)

## 11.1 概述

Completion 是一种轻量级的任务同步原语，用于一个任务等待另一个任务完成某个操作。典型的"生产者-消费者"模式：一个任务等待事件，另一个任务发出事件信号。

**核心特性：**
- 一对一同步：一个任务等待，另一个任务完成
- 轻量级：基于等待队列实现
- 可多次使用：可通过重新初始化重用
- 支持超时：可指定等待超时

## 11.2 关键数据结构

定义在 [include/linux/completion.h](file:///home/louis/code/linux/include/linux/completion.h)：

```c
// include/linux/completion.h
struct completion {
    unsigned int done;                // 完成标志/计数
    /*
     * done 含义:
     *   0         = 等待者会阻塞 (尚未完成)
     *   1 (或 >0) = 完成, 等待者不会阻塞
     *   complete() 调用时: done++
     *   wait 时: 如果 done > 0, 递减并返回
     *            否则加入等待队列
     */
    wait_queue_head_t wait;           // 等待队列
};
```

## 11.3 核心 API

```c
// include/linux/completion.h

// 初始化
void init_completion(struct completion *x);
void reinit_completion(struct completion *x);   // 重置 done = 0

// 静态初始化
DECLARE_COMPLETION(work);           // 静态声明
#define DECLARE_COMPLETION(work) \
    struct completion work = COMPLETION_INITIALIZER(work)

// 等待完成
void wait_for_completion(struct completion *x);          // 不可中断等待
void wait_for_completion_io(struct completion *x);       // IO 等待变体
int wait_for_completion_interruptible(struct completion *x);  // 可中断
int wait_for_completion_killable(struct completion *x);       // 可被杀死
unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout);
unsigned long wait_for_completion_io_timeout(struct completion *x, unsigned long timeout);
int wait_for_completion_interruptible_timeout(struct completion *x, unsigned long timeout);
long wait_for_completion_killable_timeout(struct completion *x, unsigned long timeout);

// 发出完成信号
void complete(struct completion *x);            // 唤醒一个等待者
void complete_all(struct completion *x);         // 唤醒所有等待者

// 检查是否完成
bool try_wait_for_completion(struct completion *x);  // 非阻塞检查
```

## 11.4 工作原理

```
completion 状态机:

  初始状态:
    done = 0, wait = 空队列

  等待者调用 wait_for_completion():
    ┌─────────────────────────────────────────┐
    │  if (done > 0):                         │
    │      done--;                            │
    │      return (立即返回, 无需等待)         │
    │  else:                                  │
    │      加入等待队列                        │
    │      schedule() → 睡眠等待               │
    └─────────────────────────────────────────┘

  完成者调用 complete():
    ┌─────────────────────────────────────────┐
    │  done++;                                │
    │  if (有等待者):                          │
    │      wake_up() → 唤醒一个等待者          │
    └─────────────────────────────────────────┘

  完成者调用 complete_all():
    ┌─────────────────────────────────────────┐
    │  done = UINT_MAX (防止再次等待)          │
    │  wake_up_all() → 唤醒所有等待者          │
    └─────────────────────────────────────────┘
```

## 11.5 调用链

### 11.5.1 wait_for_completion 调用链

```
wait_for_completion(x)                       [kernel/sched/completion.c]
  │
  └── __wait_for_common(x, schedule, TASK_UNINTERRUITIBLE, MAX_SCHEDULE_TIMEOUT)
        │
        ├── spin_lock_irq(&x->wait.lock)
        │
        ├── if (x->done > 0):
        │     x->done--;
        │     spin_unlock_irq(&x->wait.lock);
        │     return 0;                    → 已完成, 直接返回
        │
        ├── __add_wait_queue_entry_tail(&x->wait, &wait)
        │     └── 加入等待队列尾部
        │
        └── for (;;):
              ├── __set_current_state(TASK_UNINTERRUPTIBLE)
              │
              ├── spin_unlock_irq(&x->wait.lock)
              │
              ├── schedule()              → 调度出去
              │
              ├── spin_lock_irq(&x->wait.lock)
              │
              ├── if (x->done > 0):       → 被唤醒且完成
              │     x->done--;
              │     __set_current_state(TASK_RUNNING);
              │     ret = 0;
              │     break;
              │
              └── if (条件满足):           → 超时/信号
                    break;
```

### 11.5.2 complete 调用链

```
complete(x)                                  [kernel/sched/completion.c]
  │
  └── __complete(x, SWAKE_ONE)
        │
        ├── spin_lock_irqsave(&x->wait.lock, flags)
        │
        ├── x->done++;                       → 增加完成计数
        │
        ├── __wake_up_locked(&x->wait, TASK_NORMAL, 1)
        │     └── 唤醒一个等待者
        │
        └── spin_unlock_irqrestore(&x->wait.lock, flags)
```

## 11.6 使用场景

| 场景 | 说明 |
|------|------|
| 内核线程同步 | 等待内核线程完成初始化 |
| 子进程退出 | wait4() 等待子进程退出 |
| 异步 I/O 完成 | 等待 DMA 传输完成 |
| 设备驱动 | 等待设备操作完成 |
| 模块卸载 | 等待所有引用释放 |

## 11.7 使用示例

```c
// 示例: 内核线程同步
struct completion work_done;

// 驱动初始化
static int __init my_driver_init(void)
{
    struct task_struct *thread;

    init_completion(&work_done);

    // 创建内核线程
    thread = kthread_run(my_thread, NULL, "my-thread");
    if (IS_ERR(thread))
        return PTR_ERR(thread);

    // 等待内核线程完成初始化
    wait_for_completion(&work_done);

    return 0;
}

// 内核线程
static int my_thread(void *data)
{
    // 执行初始化
    do_init();

    // 通知完成
    complete(&work_done);

    // 继续执行
    while (!kthread_should_stop()) {
        // ...
    }
    return 0;
}
```

## 11.8 使用注意事项

```c
// 1. complete() 可以在任何上下文中调用
// 包括中断上下文, 因为不会睡眠
irqreturn_t irq_handler(int irq, void *dev_id)
{
    complete(&dev->done);  // 中断中安全
    return IRQ_HANDLED;
}

// 2. wait_for_completion() 不可在中断上下文中调用
// 会睡眠调度

// 3. 使用 complete_all() 后, 需要 reinit 才能重用
complete_all(&comp);
// 等待者全部被唤醒
reinit_completion(&comp);  // 重置 done=0
// 可以再次使用

// 4. 多个等待者时, complete() 一次只唤醒一个
// 需要多次调用 complete() 或使用 complete_all()
```

## 11.9 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/completion.h](file:///home/louis/code/linux/include/linux/completion.h) | completion API |
| [kernel/sched/completion.c](file:///home/louis/code/linux/kernel/sched/completion.c) | completion 实现 |