# 12. 等待队列 (wait_queue)

## 12.1 概述

等待队列是 Linux 内核中最基本的任务同步机制，用于实现条件等待：当某个条件不满足时，任务睡眠等待；当条件满足时，任务被唤醒。

**核心特性：**
- 条件等待：任务在条件不满足时睡眠
- 事件驱动：条件满足时唤醒等待者
- 灵活：支持独占/非独占等待、自动/手动唤醒
- 基础性：completion、select/poll、epoll 等机制都基于等待队列

## 12.2 关键数据结构

### 12.2.1 wait_queue_head_t

定义在 [include/linux/wait.h](file:///home/louis/code/linux/include/linux/wait.h)：

```c
// include/linux/wait.h
struct wait_queue_head {
    spinlock_t          lock;           // 保护队列操作的锁
    struct list_head    head;           // 等待者链表头
};
typedef struct wait_queue_head wait_queue_head_t;
```

### 12.2.2 wait_queue_entry_t

```c
// include/linux/wait.h
struct wait_queue_entry {
    unsigned int        flags;          // 等待标志
    /*
     * flags:
     *   WQ_FLAG_EXCLUSIVE = 1  → 独占等待 (一次只唤醒一个)
     *   0                       → 非独占等待 (广播唤醒)
     */
    void                *private;       // 指向等待任务 task_struct
    wait_queue_func_t   func;           // 唤醒时的回调函数
    struct list_head    entry;          // 链表节点
};
typedef struct wait_queue_entry wait_queue_entry_t;
```

### 12.2.3 等待者回调函数类型

```c
// include/linux/wait.h
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry,
                                 unsigned mode, int flags, void *key);

// 默认唤醒函数
int default_wake_function(struct wait_queue_entry *wq_entry,
                          unsigned mode, int flags, void *key);
```

## 12.3 核心 API

### 12.3.1 初始化

```c
// 静态初始化
DECLARE_WAIT_QUEUE_HEAD(name);

// 动态初始化
void init_waitqueue_head(wait_queue_head_t *wq_head);

// 初始化等待者条目
DECLARE_WAITQUEUE(name, tsk);   // 静态声明
void init_waitqueue_entry(wait_queue_entry_t *wq_entry, struct task_struct *p);
void init_waitqueue_func_entry(wait_queue_entry_t *wq_entry, wait_queue_func_t func);
```

### 12.3.2 等待

```c
// 1. 简单等待 (自动睡眠)
wait_event(wq_head, condition);
// 在 wq_head 上等待, 直到 condition 为真

wait_event_interruptible(wq_head, condition);
// 可被信号中断, 返回 -ERESTARTSYS

wait_event_timeout(wq_head, condition, timeout);
// 带超时, 返回剩余 jiffies

wait_event_interruptible_timeout(wq_head, condition, timeout);
// 可中断 + 超时

wait_event_killable(wq_head, condition);
// 仅被 SIGKILL 中断

wait_event_lock_irq(wq_head, condition, lock);
// 等待时释放锁, 唤醒后重新获取锁

// 2. 主动睡眠 (手动操作)
// 适用于需要检查条件后自行决定的场景
prepare_to_wait(wq_head, wq_entry, state);
// 将等待者加入队列, 设置任务状态

prepare_to_wait_exclusive(wq_head, wq_entry, state);
// 独占等待者

finish_wait(wq_head, wq_entry);
// 完成等待, 从队列移除
```

### 12.3.3 唤醒

```c
// 1. 唤醒所有非独占等待者 + 一个独占等待者
void wake_up(wait_queue_head_t *wq_head);
// 最常用, 适用于大多数场景

// 2. 唤醒所有等待者
void wake_up_all(wait_queue_head_t *wq_head);

// 3. 唤醒可中断的等待者
void wake_up_interruptible(wait_queue_head_t *wq_head);

// 4. 带锁的唤醒
void wake_up_locked(wait_queue_head_t *wq_head);
// 调用者已持有 wq_head->lock

// 5. 唤醒指定状态
void wake_up_state(wait_queue_head_t *wq_head, unsigned int state);
```

## 12.4 工作原理

### 12.4.1 等待/唤醒流程

```
等待者流程:
  wait_event(wq, condition):
    │
    ├── while (!(condition)):
    │     │
    │     ├── prepare_to_wait(&wq, &wait, TASK_UNINTERRUPTIBLE)
    │     │     └── 将等待者加入队列, 设置 TASK_UNINTERRUPTIBLE
    │     │
    │     └── schedule()
    │           └── 调度出去, 睡眠
    │
    └── finish_wait(&wq, &wait)
          └── 从队列移除, 设置 TASK_RUNNING

唤醒者流程:
  wake_up(&wq):
    │
    └── __wake_up(&wq, TASK_NORMAL, 1, NULL)
          │
          ├── spin_lock(&wq->lock)
          │
          ├── __wake_up_common(&wq, mode, nr_exclusive, 0, key)
          │     │
          │     └── 遍历等待队列:
          │           ├── func(wait, mode, flags, key)
          │           │     └── default_wake_function()
          │           │           └── try_to_wake_up(task, mode, wake_flags)
          │           │                 └── 唤醒任务
          │           │
          │           └── 如果是独占等待者且 nr_exclusive-- == 0
          │                 └── 停止遍历
          │
          └── spin_unlock(&wq->lock)
```

### 12.4.2 独占 vs 非独占

```
非独占等待 (flags=0):
  ┌─────────────────────────────────────┐
  │  wake_up() 唤醒所有非独占等待者     │
  │  所有等待者都被唤醒, 但只有一个能    │
  │  获取资源 (惊群效应)                 │
  └─────────────────────────────────────┘

独占等待 (flags=WQ_FLAG_EXCLUSIVE):
  ┌─────────────────────────────────────┐
  │  wake_up() 只唤醒一个独占等待者     │
  │  wake_up_all() 唤醒所有             │
  │  避免惊群效应                       │
  └─────────────────────────────────────┘

  应用场景:
    - 非独占: 广播事件 (如网络数据到达)
    - 独占:  互斥资源 (如 accept() 连接)
```

## 12.5 调用栈

### 12.5.1 wait_event_interruptible 调用链

```
wait_event_interruptible(wq, condition)       [include/linux/wait.h]
  │
  └── __wait_event_interruptible(wq, condition)
        │
        └── for (;;):
              ├── prepare_to_wait_event(&wq, &__wait, TASK_INTERRUPTIBLE)
              │     │
              │     ├── spin_lock(&wq->lock)
              │     │
              │     ├── __add_wait_queue_entry_tail(&wq, &__wait)
              │     │     └── list_add_tail(&__wait.entry, &wq->head)
              │     │
              │     ├── set_current_state(state)  → TASK_INTERRUPTIBLE
              │     │
              │     └── spin_unlock(&wq->lock)
              │
              ├── if (condition) → break
              │
              ├── schedule()
              │     └── 被唤醒后继续循环
              │
              └── finish_wait(&wq, &__wait)
                    └── 从队列移除, 设置 TASK_RUNNING
```

### 12.5.2 wake_up 调用链

```
wake_up(wq)                                    [include/linux/wait.h]
  │
  └── __wake_up(wq, TASK_NORMAL, 1, NULL)      [kernel/sched/wait.c]
        │
        └── __wake_up_common(wq, TASK_NORMAL, 1, 0, NULL)
              │
              └── __wake_up_common_lock(wq, TASK_NORMAL, 1, 0, NULL)
                    │
                    ├── spin_lock_irqsave(&wq->lock, flags)
                    │
                    ├── __wake_up_common(wq, mode, nr_exclusive, wake_flags, key)
                    │     │
                    │     └── list_for_each_entry_safe(curr, &wq->head, entry):
                    │           │
                    │           ├── curr->func(curr, mode, sync, key)
                    │           │     └── default_wake_function()
                    │           │           │
                    │           │           └── try_to_wake_up(task, mode, wake_flags)
                    │           │                 │
                    │           │                 ├── ttwu_queue(task, cpu)
                    │           │                 │     └── 将任务加入唤醒队列
                    │           │                 │
                    │           │                 └── ttwu_do_activate()
                    │           │                       └── 设置任务为 RUNNABLE
                    │           │
                    │           └── if (curr->flags & WQ_FLAG_EXCLUSIVE)
                    │                 && !--nr_exclusive:
                    │                 break;  → 独占标志, 停止唤醒
                    │
                    └── spin_unlock_irqrestore(&wq->lock, flags)
```

## 12.6 使用场景

| 场景 | 说明 |
|------|------|
| 设备驱动 | 等待设备 I/O 完成 |
| 网络协议栈 | 等待数据包到达 |
| 文件系统 | 等待 inode 状态变化 |
| 内核线程 | 等待条件触发执行 |
| select/poll | 等待文件描述符就绪 |

## 12.7 使用示例

```c
// 示例: 生产者-消费者模式
struct buffer {
    int data;
    bool ready;
    wait_queue_head_t wq;
    spinlock_t lock;
};

// 消费者
int consumer(struct buffer *buf)
{
    wait_event_interruptible(buf->wq, buf->ready);
    // 条件满足时被唤醒
    spin_lock(&buf->lock);
    int data = buf->data;
    buf->ready = false;
    spin_unlock(&buf->lock);
    return data;
}

// 生产者
void producer(struct buffer *buf, int data)
{
    spin_lock(&buf->lock);
    buf->data = data;
    buf->ready = true;
    spin_unlock(&buf->lock);
    wake_up(&buf->wq);  // 唤醒消费者
}
```

## 12.8 使用注意事项

```c
// 1. 条件检查必须在循环中
// 防止虚假唤醒 (spurious wakeup)
wait_event_interruptible(wq, condition);
// 等价于:
while (!(condition)) {
    prepare_to_wait_event(&wq, &wait, TASK_INTERRUPTIBLE);
    if (condition) break;
    schedule();
}
finish_wait(&wq, &wait);

// 2. 条件变量受外部锁保护时
// 使用 wait_event_lock_irq() 自动释放/重获取锁

// 3. 避免在条件中调用可能睡眠的函数

// 4. 独占等待 vs 非独占
// - 多个等待者等待同一事件 → 非独占
// - 多个等待者竞争同一资源 → 独占 (防止惊群)
```

## 12.9 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/wait.h](file:///home/louis/code/linux/include/linux/wait.h) | 等待队列 API |
| [kernel/sched/wait.c](file:///home/louis/code/linux/kernel/sched/wait.c) | 等待队列实现 |