# io_uring — 异步 I/O 框架

## 1 概述

io_uring 是 Linux 内核提供的高性能异步 I/O 框架，通过共享 ring buffer 在用户态和内核态之间传递 I/O 请求和完成事件，避免了传统系统调用的开销。

- **文件**: `io_uring/` 目录（约 30+ 源文件）
- **系统调用**: `io_uring_setup()`, `io_uring_enter()`, `io_uring_register()`
- **执行上下文**: 进程上下文，支持 SQPOLL 内核线程轮询

## 2 实现原理

### 2.1 架构设计

io_uring 的核心设计是使用共享内存的 ring buffer 实现零拷贝通信：

```
用户态                          内核态
  │                               │
  ├─ 提交队列 (SQ) ──mmap共享──→  io_uring 实例
  │    │                          │
  │    │  SQ ring (head/tail)     │  io_ring_ctx 上下文
  │    │  SQ entries (SQE)        │    ├─ 提交队列 (SQ)
  │    │                          │    ├─ 完成队列 (CQ)
  ├─ 完成队列 (CQ) ←──mmap共享───│    ├─ 文件表 (registered files)
  │    │                          │    ├─ 缓冲区表 (buffers)
  │    │  CQ ring (head/tail)     │    └─ io_kiocb 请求池
  │    │  CQ entries (CQE)        │
  │    │                          │
  └─ io_uring_enter() ──syscall─→│ 处理 SQ 中的请求
                                  │ 完成后写入 CQ
```

### 2.2 关键数据结构

**io_ring_ctx — 核心上下文**:

```c
// io_uring/io_uring_types.h
struct io_ring_ctx {
    struct io_rings *rings;            // 共享 ring buffer（SQ + CQ）
    struct io_sq_rings *sq_rings;      // 提交队列 ring
    struct io_cq_rings *cq_rings;      // 完成队列 ring
    unsigned int flags;                // IORING_SETUP_* 标志
    unsigned int sq_entries;           // SQ 条目数
    unsigned int cq_entries;           // CQ 条目数

    struct io_submit_state submit_state;  // 提交状态
    struct io_uring_task *submitter_task; // 提交者任务

    struct xarray io_bl_xa;            // 固定缓冲区映射
    struct file_table file_table;      // 固定文件表

    struct mutex uring_lock;           // 保护提交路径的锁
    spinlock_t completion_lock;        // 完成路径锁

    struct list_head defer_list;       // 延期请求链表
    struct list_head timeout_list;     // 超时请求链表
    struct list_head iopoll_list;      // IOPOLL 请求链表

    struct wait_queue_head cq_wait;    // CQ 等待队列
    struct wait_queue_head poll_wq;    // 轮询等待队列

    struct percpu_ref refs;            // 引用计数
    struct completion ref_comp;        // 引用完成同步
    struct task_struct *sqo_task;      // SQPOLL 线程

    struct io_alloc_cache req_cache;   // 请求对象缓存
    struct io_alloc_cache apoll_cache; // 异步 poll 缓存
    struct io_alloc_cache netmsg_cache; // 网络消息缓存
    struct io_alloc_cache rsrc_cache;  // 资源缓存
    struct io_alloc_cache cmd_cache;   // 命令缓存
    ...
};
```

**io_kiocb — 请求对象**:

```c
// io_uring/io_uring_types.h
struct io_kiocb {
    union {
        struct file *file;             // 目标文件
        struct io_rw rw;               // 读写操作
        struct io_poll poll;           // poll 操作
        struct io_accept accept;       // accept 操作
        struct io_connect connect;     // connect 操作
        struct io_send_recv send_recv; // send/recv 操作
        struct io_open open;           // open 操作
        ...
    };

    u64 user_data;                     // 用户数据 (CQE 中返回)
    struct io_ring_ctx *ctx;           // 所属上下文

    struct io_task_work io_task_work;  // task_work 完成处理
    struct io_wq_work wq_work;         // workqueue 异步处理

    unsigned int flags;                // REQ_F_* 标志
    int cqe_res;                       // 完成结果
    u32 cqe_flags;                     // 完成标志
    ...
};
```

### 2.3 提交路径

```
io_uring_enter() 系统调用
  │
  ├─ 1. io_submit_sqes(ctx, nr)
  │    ├─ 从 SQ ring 读取 SQE
  │    ├─ io_init_req() 初始化 io_kiocb
  │    ├─ io_submit_sqe() 提交请求
  │    │    ├─ 直接执行 (inline)
  │    │    ├─ 异步排队到 io-wq
  │    │    └─ 需要 poll → 注册 poll 回调
  │    └─ 提交完成后更新 SQ head
  │
  └─ 2. 返回 CQ 事件数
```

### 2.4 完成路径

```
请求完成 (硬件中断或 worker 完成)
  │
  ├─ 1. 填充 CQE 到 CQ ring
  │
  ├─ 2. io_commit_cqring() 更新 CQ tail
  │
  ├─ 3. 唤醒等待 CQ 的进程
  │    └─ wake_up(&ctx->cq_wait)
  │
  └─ 4. 或通过 task_work 通知提交者
       └─ io_req_task_complete()
            └─ task_work_add(current, TWA_RESUME)
```

### 2.5 工作模式

| 模式 | 说明 | 适用场景 |
|--|--|--|
| 中断驱动 | 默认模式，I/O 完成后通过中断通知 | 通用场景 |
| IOPOLL | `IORING_SETUP_IOPOLL`，轮询完成状态 | 高速存储设备 |
| SQPOLL | `IORING_SETUP_SQPOLL`，内核线程轮询 SQ | 减少系统调用 |
| DEFER_TASKRUN | `IORING_SETUP_DEFER_TASKRUN`，延迟 task_work 到下次 enter | 批处理优化 |

## 3 使用场景

| 场景 | 说明 |
|--|--|
| 高性能存储 | 数据库、分布式存储系统 |
| 网络服务器 | 高并发网络 I/O（nginx、redis 等） |
| 零拷贝数据传输 | splice、sendfile 等操作 |
| 文件系统操作 | open/read/write/fsync 等 |
| poll/accept/connect | 网络事件处理 |

## 4 关键 API

| 系统调用 | 说明 |
|--|--|
| `io_uring_setup(entries, params)` | 创建 io_uring 实例，返回 fd |
| `io_uring_enter(fd, to_submit, min_complete, flags)` | 提交 SQE 并等待完成 |
| `io_uring_register(fd, opcode, arg, nr_args)` | 注册文件、缓冲区等资源 |

## 5 内部执行流程

```
io_uring_enter(fd, to_submit, min_complete, flags)
  │
  ├─ io_submit_sqes(ctx, nr)
  │    │
  │    ├─ 循环读取 SQE
  │    │    └─ io_init_req() → 初始化 io_kiocb
  │    │    └─ io_submit_sqe()
  │    │         ├─ 直接处理 (非阻塞)
  │    │         └─ 异步提交 → io_queue_sqe()
  │    │              ├─ 需要 poll → io_arm_poll_handler()
  │    │              └─ 需要异步 worker → io_queue_iowq()
  │    │
  │    └─ 更新 SQ head
  │
  └─ io_cqring_wait() (如果 min_complete > 0)
       └─ wait_event_interruptible(ctx->cq_wait, 条件)
```

## 6 与传统异步 I/O 对比

| 特性 | io_uring | AIO (libaio) | epoll + 线程池 |
|--|--|--|--|
| 系统调用开销 | 低（共享 ring） | 高（每次调用） | 中 |
| 支持 buffered I/O | 是 | 否 | 是 |
| 支持网络 I/O | 是 | 否 | 是 |
| 支持文件操作 | 全部 | read/write/fsync | 仅 read/write |
| 零拷贝支持 | 是 | 否 | 否 |
| 用户态接口 | ring buffer | 系统调用 | 系统调用 |