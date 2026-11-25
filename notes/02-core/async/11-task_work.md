# task_work — 返回用户态时执行

## 1 概述

task_work 是一种在任务（进程/线程）即将从内核态返回用户态时执行回调的异步机制。它利用了内核的 `TIF_NOTIFY_RESUME` 标志位，在 `exit_to_user_mode()` 路径中触发回调执行。

- **文件**: `kernel/task_work.c`, `include/linux/task_work.h`
- **执行上下文**: 进程上下文（可睡眠）
- **触发点**: `exit_to_user_mode()` → `task_work_run()`

## 2 实现原理

### 2.1 核心数据结构

```c
// include/linux/types.h
struct callback_head {
    struct callback_head *next;      // 链表节点，连接多个 task_work
    void (*func)(struct callback_head *head);  // 回调函数
} __attribute__((aligned(sizeof(void *))));
```

每个 `task_struct` 中有一个 `task_works` 字段，指向一个由 `struct callback_head` 组成的单链表（LIFO 顺序）：

```c
// include/linux/sched.h
struct task_struct {
    struct callback_head *task_works;  // 待执行的 task_work 链表头
    ...
};
```

### 2.2 通知模式

task_work_add() 支持多种通知模式，决定如何唤醒目标任务：

| 模式 | 说明 |
|--|--|
| `TWA_NONE` | 不通知，等待目标任务自行调度 |
| `TWA_RESUME` | 设置 `TIF_NOTIFY_RESUME`，在返回用户态时执行 |
| `TWA_SIGNAL` | 发送信号打断目标任务，强制其返回用户态执行 |
| `TWA_SIGNAL_NO_IPI` | 类似 `TWA_SIGNAL`，但不发送 IPI |
| `TWA_NMI_CURRENT` | 仅用于当前任务，且当前上下文为 NMI |

### 2.3 执行流程

```
task_work_add(task, work, notify)
  │
  ├─ 1. 将 work 通过 CAS 原子操作插入 task->task_works 链表头 (LIFO)
  │
  └─ 2. 根据 notify 模式设置通知：
       ├─ TWA_RESUME       → set_notify_resume(task) → 设置 TIF_NOTIFY_RESUME
       ├─ TWA_SIGNAL       → set_notify_signal(task) → 发送信号
       ├─ TWA_SIGNAL_NO_IPI → __set_notify_signal(task)
       └─ TWA_NMI_CURRENT  → 设置 TIF_NOTIFY_RESUME + irq_work_queue (IPI)
```

```
task_work_run()  — 在 exit_to_user_mode() 路径中调用
  │
  ├─ 1. 通过 CAS 原子摘取整个 task->task_works 链表
  ├─ 2. 遍历链表，逐个调用 work->func(work)
  └─ 3. 如果链表为空且任务正在退出 (PF_EXITING)，设置 work_exited 哨兵
```

### 2.4 执行流程图

```
用户态 syscall 进入内核
  │
  ├─ syscall 处理 (可能调用 task_work_add 添加 work)
  │
  └─ syscall 返回用户态
       │
       └─ exit_to_user_mode()
            │
            └─ 检查 TIF_NOTIFY_RESUME
                 │
                 └─ true → task_work_run()
                      │
                      ├─ CAS 摘取 task->task_works 链表
                      ├─ 遍历链表
                      │    └─ work->func(work)  ← 可睡眠，可添加新的 task_work
                      └─ 链表为空 → 返回用户态
```

## 3 使用场景

| 场景 | 说明 |
|--|--|
| 延迟 fput() | 文件引用计数归零后，延迟释放文件结构体 |
| 信号投递 | SIGURG 等信号通过 task_work 在安全点投递 |
| io_uring 完成通知 | 异步 I/O 完成时，通过 task_work 唤醒用户态 |
| POSIX 定时器 | CPU 定时器到期时，通过 task_work 处理 |
| cpuset 内存迁移 | 内存迁移完成后，通过 task_work 刷新 |

## 4 关键 API

| API | 说明 |
|--|--|
| `task_work_add(task, work, notify)` | 添加 task_work，返回 0 成功或 -ESRCH |
| `task_work_run()` | 执行当前任务所有待处理的 task_work |
| `task_work_cancel(task, cb)` | 取消指定 work |
| `task_work_cancel_func(task, func)` | 取消匹配指定回调函数的 work |
| `init_task_work(work, func)` | 静态初始化 task_work |

## 5 代码调用栈

### 5.1 添加 task_work (io_uring 完成通知示例)

```
io_req_task_complete()
  └─ io_req_task_work_add(req)
       └─ task_work_add(current, &req->io_task_work, TWA_RESUME)
            ├─ CAS 插入链表
            └─ set_notify_resume(current)
```

### 5.2 执行 task_work

```
el0_sync()           // ARM64 系统调用返回
  └─ ret_to_user()
       └─ exit_to_user_mode()
            ├─ ct_work_flush()
            └─ task_work_run()
                 └─ work->func(work)
                      └─ io_req_task_complete()
```

## 6 与其他异步机制的对比

| 特性 | task_work | irq_work | Workqueue |
|--|--|--|--|
| 执行上下文 | 进程（可睡眠） | NMI/硬中断 | 进程（可睡眠） |
| 触发时机 | 返回用户态 | 中断返回路径 | kworker 调度 |
| 延迟 | 低 | 极低 | 中 |
| 指定目标任务 | 是（按 task） | 否（按 CPU） | 否（按 worker） |
| 使用场景 | 任务本地异步操作 | 跨 CPU 通知 | 通用延迟工作 |