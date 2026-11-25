# io_uring_enter - 提交 SQE 并/或等待完成事件

## 概述

`io_uring_enter` 是 io_uring 的核心系统调用，用于执行以下操作：
1. **提交 SQE**: 将 SQ ring 中待处理的 SQE 提交给内核执行
2. **等待完成**: 等待 I/O 操作完成并获取 CQE
3. **唤醒 SQPOLL**: 在 SQPOLL 模式下唤醒内核轮询线程

该函数可以只提交、只等待、或同时提交和等待，通过 flags 参数控制。

## 函数原型

```c
SYSCALL_DEFINE6(io_uring_enter, unsigned int, fd, u32, to_submit,
        u32, min_complete, u32, flags, const void __user *, argp,
        size_t, argsz);
```

## 参数详解

| 参数 | 说明 |
|------|------|
| fd | io_uring_setup 返回的文件描述符（或已注册的 ring 索引） |
| to_submit | 从 SQ ring 中取出的待提交 SQE 数量 |
| min_complete | 等待的最小完成事件数（与 IORING_ENTER_GETEVENTS 一起使用） |
| flags | 操作标志位 |
| argp | 扩展参数指针（与 IORING_ENTER_EXT_ARG 一起使用） |
| argsz | 扩展参数大小 |

### 标志位 (flags)

| 标志 | 值 | 说明 |
|------|------|------|
| IORING_ENTER_GETEVENTS | (1U << 0) | 等待完成事件 |
| IORING_ENTER_SQ_WAKEUP | (1U << 1) | 唤醒 SQPOLL 线程处理新提交 |
| IORING_ENTER_SQ_WAIT | (1U << 2) | 等待 SQ ring 有空闲位置 |
| IORING_ENTER_EXT_ARG | (1U << 3) | 使用扩展参数（struct io_uring_getevents_arg） |
| IORING_ENTER_REGISTERED_RING | (1U << 4) | fd 是已注册的 ring 索引 |
| IORING_ENTER_ABS_TIMER | (1U << 5) | 超时为绝对时间 |
| IORING_ENTER_EXT_ARG_REG | (1U << 6) | 使用注册的固定等待参数区域 |
| IORING_ENTER_NO_IOWAIT | (1U << 7) | 不设置进程的 iowait 标志 |

## 完整调用链分析

```
io_uring_enter(fd, to_submit, min_complete, flags, argp, argsz)
  │
  ├─ 获取文件/上下文
  │    ├─ if IORING_ENTER_REGISTERED_RING:
  │    │    └─ 从 tctx->registered_rings[fd] 获取 file
  │    └─ else:
  │         └─ fget(fd) → file
  │    └─ ctx = file->private_data
  │
  ├─ 检查 IORING_SETUP_R_DISABLED → -EBADFD
  │
  ├─ SQPOLL 模式 (IORING_SETUP_SQPOLL):
  │    ├─ 检查 sq_data->thread 是否存在 → -EOWNERDEAD
  │    ├─ if IORING_ENTER_SQ_WAKEUP:
  │    │    └─ wake_up(&ctx->sq_data->wait)    // 唤醒 SQPOLL 线程
  │    ├─ if IORING_ENTER_SQ_WAIT:
  │    │    └─ io_sqpoll_wait_sq(ctx)          // 等待 SQ 有空位
  │    │         └─ 循环等待直到 !io_sqring_full(ctx)
  │    └─ ret = to_submit
  │
  ├─ 非 SQPOLL 模式 (to_submit > 0):
  │    ├─ io_uring_add_tctx_node(ctx)           // 确保 task 已绑定
  │    ├─ mutex_lock(&ctx->uring_lock)
  │    ├─ io_submit_sqes(ctx, to_submit)        // 提交 SQE
  │    │    ├─ entries = min(to_submit, io_sqring_entries(ctx))
  │    │    ├─ io_get_task_refs(entries)        // 获取 task 引用
  │    │    ├─ io_submit_state_start(...)        // 开始批量提交
  │    │    ├─ 循环处理每个 SQE:
  │    │    │    ├─ io_alloc_req(ctx, &req)     // 从缓存分配 io_kiocb
  │    │    │    ├─ io_get_sqe(ctx, &sqe)       // 从 SQ ring 读取 SQE
  │    │    │    └─ io_submit_sqe(ctx, req, sqe, &left)  // 提交单个 SQE
  │    │    │         ├─ io_init_req(ctx, req, sqe)       // 初始化请求
  │    │    │         │    └─ io_req_prep(req, sqe)       // 按 opcode 准备
  │    │    │         ├─ (可选) BPF 过滤检查
  │    │    │         └─ io_queue_sqe(req, ...)           // 排队请求
  │    │    │              ├─ __io_queue_sqe(req)         // 尝试直接执行
  │    │    │              │    ├─ io_issue_sqe(req, IO_URING_F_NONBLOCK)
  │    │    │              │    │    └─ __io_issue_sqe → opcode 分发
  │    │    │              │    │         ├─ io_read/io_write (rw.c)
  │    │    │              │    │         ├─ io_send/io_recv (net.c)
  │    │    │              │    │         ├─ io_poll_add (poll.c)
  │    │    │              │    │         ├─ io_openat/io_close (openclose.c)
  │    │    │              │    │         └─ ... 其他 opcode
  │    │    │              │    │
  │    │    │              │    └─ 若无法立即完成 (返回 -EAGAIN):
  │    │    │              │         ├─ io_arm_poll_handler(req)  // 注册 poll
  │    │    │              │         └─ 或 io_queue_async_work(req)  // 丢入 workqueue
  │    │    │              │
  │    │    │              └─ io_submit_flush_completions()     // flush 完成事件
  │    │    │
  │    │    ├─ io_submit_state_end(ctx)         // 结束批量提交
  │    │    └─ io_commit_sqring(ctx)            // 更新 SQ head
  │    │
  │    ├─ if IORING_ENTER_GETEVENTS && syscall_iopoll:
  │    │    └─ io_run_local_work_locked()       // 处理延期任务
  │    └─ mutex_unlock(&ctx->uring_lock)
  │
  ├─ IORING_ENTER_GETEVENTS:
  │    ├─ IOPOLL 模式 (ctx->syscall_iopoll):
  │    │    ├─ mutex_lock(&ctx->uring_lock)
  │    │    ├─ io_validate_ext_arg()            // 校验扩展参数
  │    │    └─ io_iopoll_check(ctx, min_complete)  // 轮询完成事件
  │    │         ├─ 检查溢出和丢弃事件
  │    │         ├─ 若已有事件 → 直接返回
  │    │         └─ 循环轮询直到 nr_events >= min_complete:
  │    │              ├─ io_run_local_work_locked()  // 运行本地任务
  │    │              ├─ io_do_iopoll(ctx, ...)       // 调用 file->f_op->iopoll
  │    │              │    └─ 遍历 ctx->iopoll_list
  │    │              │    └─ 对每个请求调用驱动的 iopoll 回调
  │    │              └─ 收集完成事件
  │    │
  │    └─ 中断模式 (!IOPOLL):
  │         ├─ io_get_ext_arg()                 // 解析扩展参数
  │         └─ io_cqring_wait(ctx, min_complete, flags, &ext_arg)
  │              ├─ 检查本地任务和 task_work
  │              ├─ 处理 CQ 溢出
  │              ├─ 若已有足够事件 → 直接返回
  │              ├─ 初始化 io_wait_queue
  │              ├─ 设置超时 (如果指定)
  │              ├─ 设置信号掩码 (如果指定)
  │              ├─ io_napi_busy_loop()         // NAPI 忙轮询
  │              └─ 等待循环:
  │                   ├─ prepare_to_wait_exclusive(&ctx->cq_wait)
  │                   ├─ io_cqring_wait_schedule()
  │                   │    ├─ 检查 check_cq/task_work/信号
  │                   │    └─ schedule() 或 hrtimer 睡眠
  │                   ├─ 运行 task_work
  │                   ├─ 处理溢出/丢弃事件
  │                   ├─ 检查 io_should_wake()
  │                   └─ cond_resched()
  │
  └─ fput(file) 或 registered_ring (无需释放)
```

## 提交路径详解

### io_submit_sqes 详细流程

```
io_submit_sqes(ctx, nr)
  │
  ├─ 计算可提交数: entries = min(nr, io_sqring_entries(ctx))
  ├─ io_get_task_refs(entries)          // 预取 task 引用计数
  ├─ io_submit_state_start(...)          // 初始化提交状态
  │
  ├─ 循环:
  │    ├─ io_alloc_req(ctx, &req)        // 从 slab 缓存分配 io_kiocb
  │    │    └─ 若缓存空 → __io_alloc_req_refill() 批量分配
  │    ├─ io_get_sqe(ctx, &sqe)          // 从 SQ ring 读取 SQE 指针
  │    │    └─ 通过 sq_array 索引获取 sq_sqes 中的位置
  │    ├─ io_submit_sqe(ctx, req, sqe)   // 初始化和提交
  │    │    ├─ io_init_req()             // 设置 opcode/flags/file/ctx
  │    │    ├─ req->prep()               // 调用 opcode 特定的 prep 函数
  │    │    ├─ (可选) BPF 过滤
  │    │    └─ io_queue_sqe()            // 执行或排队
  │    └─ 处理链接请求 (IOSQE_IO_LINK)
  │
  └─ io_commit_sqring(ctx)              // 更新 cached_sq_head
```

### io_issue_sqe 的 opcode 分发

```c
// 根据 req->opcode 分发到不同的处理函数
static int __io_issue_sqe(struct io_kiocb *req, unsigned issue_flags)
{
    switch (req->opcode) {
    case IORING_OP_READV:   return io_read(req, issue_flags);
    case IORING_OP_WRITEV:  return io_write(req, issue_flags);
    case IORING_OP_READ:    return io_read(req, issue_flags);
    case IORING_OP_WRITE:   return io_write(req, issue_flags);
    case IORING_OP_SEND:    return io_send(req, issue_flags);
    case IORING_OP_RECV:    return io_recv(req, issue_flags);
    case IORING_OP_ACCEPT:  return io_accept(req, issue_flags);
    case IORING_OP_POLL_ADD: return io_poll_add(req, issue_flags);
    case IORING_OP_NOP:     return io_nop(req, issue_flags);
    // ... 50+ 种 opcode
    }
}
```

## 完成路径详解

### 中断模式完成路径

```
I/O 完成 (驱动中断回调)
  └─ kiocb->ki_complete() (如 aio_complete_rw)
       └─ io_complete_rw(req, res, res2)
            ├─ io_req_complete(req, res, res2)
            │    ├─ io_fill_cqe_req(req, res, cflags)   // 填充 CQE
            │    │    └─ io_cqring_fill_event(ctx, ...)
            │    │         ├─ cqe->user_data = req->cqe.user_data
            │    │         ├─ cqe->res = res
            │    │         ├─ cqe->flags = cflags
            │    │         └─ smp_wmb() 保证顺序
            │    └─ io_cqring_ev_posted(ctx)             // 通知等待者
            │         ├─ io_cqring_wake(ctx)              // 唤醒 cq_wait
            │         │    └─ __wake_up(&ctx->cq_wait, ...)
            │         ├─ eventfd_signal(ctx->cq_ev_fd)   // eventfd 通知
            │         └─ io_commit_cqring_flush(ctx)      // flush 操作
            └─ io_free_req(req)                           // 释放请求
```

### IOPOLL 模式完成路径

```
应用调用 io_uring_enter(GETEVENTS)
  └─ io_iopoll_check(ctx, min_complete)
       └─ 循环:
            ├─ io_do_iopoll(ctx, ...)
            │    └─ 遍历 ctx->iopoll_list:
            │         ├─ io_uring_classic_poll(req, &iob, poll_flags)
            │         │    └─ file->f_op->iopoll(&rw->kiocb, &iob, poll_flags)
            │         │         └─ 块设备驱动轮询完成
            │         └─ 收集完成的请求
            └─ 处理完成的请求:
                 ├─ io_complete_rw_iopoll(req, res, res2)
                 │    ├─ req->iopoll_completed = 1
                 │    └─ io_req_complete(req, res, res2)
                 └─ 从 iopoll_list 移除
```

## 关键数据结构

### struct io_kiocb（I/O 请求）

```c
struct io_kiocb {
    struct file         *file;          // 目标文件
    u8                   opcode;        // 操作码
    u8                   iopoll_completed; // IOPOLL 完成标记
    u16                  buf_index;     // 固定缓冲区索引
    io_req_flags_t       flags;         // REQ_F_* 标志
    struct io_cqe        cqe;           // 完成事件数据
    struct io_ring_ctx   *ctx;          // 所属上下文
    struct io_uring_task *tctx;         // 所属 task 上下文
    struct io_task_work  io_task_work;  // 任务工作
    struct io_kiocb      *link;         // 链接的下一个请求
    atomic_t             refs;          // 引用计数
    struct io_wq_work    work;          // workqueue 工作
    void                 *async_data;   // 异步数据
    struct async_poll    *apoll;        // 异步 poll 结构
    // ...
};
```

### struct io_wait_queue（等待队列）

```c
struct io_wait_queue {
    struct wait_queue_entry wq;          // 标准等待队列条目
    struct io_ring_ctx     *ctx;         // 等待的上下文
    int                    cq_tail;      // 期望的 CQ tail 值
    int                    cq_min_tail;  // 最小超时时的 tail
    int                    nr_timeouts;  // 开始时的超时计数
    ktime_t                timeout;      // 超时时间
    ktime_t                min_timeout;  // 最小超时时间
    bool                   hit_timeout;  // 是否已超时
    struct hrtimer         t;            // 高精度定时器
};
```

### struct ext_arg（扩展等待参数）

```c
struct ext_arg {
    size_t argsz;              // 参数大小
    struct timespec64 ts;      // 超时时间
    const sigset_t __user *sig; // 信号掩码
    ktime_t min_time;          // 最小等待时间
    bool ts_set;               // 是否设置了超时
    bool iowait;               // 是否标记 iowait
};
```

## 流程图

```
非 SQPOLL 模式提交 + 等待:

用户态                                         内核态
  |                                             |
  | 1. 写 SQE 到 SQ ring                       |
  | 2. 更新 SQ tail                             |
  |                                             |
  | io_uring_enter(fd, to_submit=1,             |
  |                min_complete=1,              |
  |                flags=GETEVENTS)             |
  |------------------------------------------->|
  |                                             |
  |                 |-- io_uring_add_tctx_node() |
  |                 |-- mutex_lock(uring_lock)   |
  |                 |-- io_submit_sqes()         |
  |                 |    |-- io_alloc_req()      |
  |                 |    |-- io_get_sqe()        |
  |                 |    |-- io_submit_sqe()     |
  |                 |    |    |-- io_init_req()  |
  |                 |    |    |-- io_queue_sqe() |
  |                 |    |    |    |-- io_issue_sqe()  → 非阻塞执行
  |                 |    |    |    |-- 若 -EAGAIN → 注册 poll 或丢入 workqueue
  |                 |    |    |    └-- io_submit_flush_completions()
  |                 |    └-- io_commit_sqring()  |
  |                 |-- mutex_unlock(uring_lock)  |
  |                 |                            |
  |                 |-- io_cqring_wait()          |
  |                 |    |-- prepare_to_wait()    |
  |                 |    |-- schedule()           |  ← 进程睡眠
  |                 |    |                        |
  |                 |    |  [I/O 完成, 驱动中断]  |
  |                 |    |    → io_complete_rw()  |
  |                 |    |    → io_fill_cqe_req() |  ← 写 CQE
  |                 |    |    → io_cqring_wake()  |  ← 唤醒进程
  |                 |    |                        |
  |                 |    |-- 被唤醒              |
  |                 |    |-- 读取 CQE             |
  |                 |    └-- 返回 0               |
  |<-------------------------------------------|
  | 3. 读取 CQ ring 获取结果                     |
```

## 错误处理

| 错误码 | 条件 |
|--------|------|
| -EBADF | fd 无效或不是 io_uring 文件 |
| -EBADFD | ring 处于禁用状态 (IORING_SETUP_R_DISABLED) |
| -EOWNERDEAD | SQPOLL 线程已退出 |
| -EINVAL | flags 包含无效位 |
| -EFAULT | 扩展参数 argp 指向无效内存 |
| -EAGAIN | 无法分配请求且未提交任何 SQE |
| -EINTR | 等待时被信号中断 |
| -ETIME | 等待超时 |
| -EBADR | CQE 被丢弃（需检查溢出） |
| -EEXIST | 单提交者模式但调用者不是提交者任务 |

## 使用示例

```c
#include <liburing.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

int main(void)
{
    struct io_uring ring;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    char buf[4096];
    int fd, ret;

    // 初始化 io_uring
    io_uring_queue_init(32, &ring, 0);

    // 打开文件
    fd = open("/path/to/file", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // 获取 SQE 并设置读操作
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, fd, buf, sizeof(buf), 0);

    // 提交 SQE 并等待完成
    ret = io_uring_submit_and_wait(&ring, 1);
    if (ret < 0) {
        fprintf(stderr, "submit failed: %s\n", strerror(-ret));
        return 1;
    }

    // 获取完成事件
    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "wait_cqe failed: %s\n", strerror(-ret));
        return 1;
    }

    // 处理结果
    if (cqe->res < 0) {
        fprintf(stderr, "IO failed: %s\n", strerror(-cqe->res));
    } else {
        printf("Read %d bytes\n", cqe->res);
    }

    // 确认 CQE
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    close(fd);
    return 0;
}
```

### 直接使用系统调用的示例

```c
#include <sys/syscall.h>
#include <linux/io_uring.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>

int main(void)
{
    struct io_uring_params params = {0};
    int ring_fd, ret;
    char buf[4096];

    // 创建 ring
    ring_fd = syscall(__NR_io_uring_setup, 32, &params);
    if (ring_fd < 0) {
        perror("io_uring_setup");
        return 1;
    }

    // 假设已经 mmap 并初始化了 SQ/CQ 环
    // ...

    // 提交 1 个 SQE 并等待完成
    ret = syscall(__NR_io_uring_enter, ring_fd, 1, 1,
                  IORING_ENTER_GETEVENTS, NULL, 0);
    if (ret < 0) {
        perror("io_uring_enter");
        return 1;
    }

    close(ring_fd);
    return 0;
}
```

## 设计要点

1. **批量提交**: 单次 io_uring_enter 可提交多个 SQE，减少系统调用次数
2. **提交与等待合并**: 通过 IORING_ENTER_GETEVENTS 将提交和等待合并为一次系统调用
3. **非阻塞执行**: 所有请求首先尝试非阻塞执行，仅在无法立即完成时才异步化
4. **请求链接**: 通过 IOSQE_IO_LINK 将多个请求链接为原子序列（前一个成功后执行下一个）
5. **快速 Poll**: 支持对文件描述符的快速 poll，避免请求进入 workqueue 的开销
6. **SQPOLL 优化**: SQPOLL 模式下，应用只需 wake_up 而不需完整提交路径
7. **延迟完成**: 使用 DEFER_TASKRUN 模式可减少完成事件的处理开销