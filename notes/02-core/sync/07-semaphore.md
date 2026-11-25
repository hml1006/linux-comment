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

## 7.7 使用场景

| 场景 | 说明 |
|------|------|
| 有限资源池 | 控制最多 N 个并发访问 |
| 二值信号量 | 替代 mutex 的旧代码 |
| 驱动兼容 | 旧设备驱动代码 |
| 通知机制 | 简单的信号通知 |

**现代内核推荐：** 除非需要计数语义 (多个凭据)，否则优先使用 `mutex` 或 `rwsem`。

## 7.8 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/semaphore.h](file:///home/louis/code/linux/include/linux/semaphore.h) | semaphore API |
| [kernel/locking/semaphore.c](file:///home/louis/code/linux/kernel/locking/semaphore.c) | semaphore 实现 |