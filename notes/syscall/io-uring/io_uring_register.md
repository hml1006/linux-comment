# io_uring_register - 注册/注销 io_uring 资源

## 概述

`io_uring_register` 是 io_uring 的资源管理系统调用，用于注册和管理各种内核资源以优化 I/O 性能。通过预注册文件描述符、缓冲区和其他资源，可以消除每次 I/O 操作中的 fget/fput、DMA 映射等运行时开销，显著提升性能。

该函数支持 30+ 种操作码（opcode），涵盖文件注册、缓冲区注册、eventfd 通知、io-wq 配置、NAPI 设置等。

## 函数原型

```c
SYSCALL_DEFINE4(io_uring_register, unsigned int, fd, unsigned int, opcode,
        void __user *, arg, unsigned int, nr_args);
```

## 参数详解

| 参数 | 说明 |
|------|------|
| fd | io_uring_setup 返回的文件描述符（或 -1 用于盲注册操作） |
| opcode | 操作码（IORING_REGISTER_* 或 IORING_UNREGISTER_*） |
| arg | 操作参数指针（struct 指针，取决于 opcode） |
| nr_args | 参数数量，含义取决于操作码 |

### 特殊说明

- 当 `fd == -1` 时，用于"盲注册"操作：`IORING_REGISTER_SEND_MSG_RING`、`IORING_REGISTER_QUERY`、`IORING_REGISTER_RESTRICTIONS`、`IORING_REGISTER_BPF_FILTER` 等无需 ring 上下文的操作。
- 当 `opcode` 设置了 `IORING_REGISTER_USE_REGISTERED_RING` 标志位时，`fd` 参数解释为已注册的 ring 索引。

## 操作码详解

### 文件注册

| 操作码 | 说明 | arg 类型 | nr_args |
|--------|------|----------|---------|
| IORING_REGISTER_FILES | 注册固定 fd 表 | __s32[] | fd 数量 |
| IORING_UNREGISTER_FILES | 注销固定 fd 表 | NULL | 0 |
| IORING_REGISTER_FILES_UPDATE | 更新固定 fd 表 | struct io_uring_rsrc_update | 1 |
| IORING_REGISTER_FILES2 | 带标签的注册 | struct io_uring_rsrc_register | 1 |
| IORING_REGISTER_FILES_UPDATE2 | 带标签的更新 | struct io_uring_rsrc_update2 | 1 |

### 缓冲区注册

| 操作码 | 说明 | arg 类型 | nr_args |
|--------|------|----------|---------|
| IORING_REGISTER_BUFFERS | 注册固定缓冲区（锁定用户页） | struct iovec[] | iovec 数量 |
| IORING_UNREGISTER_BUFFERS | 注销固定缓冲区 | NULL | 0 |
| IORING_REGISTER_BUFFERS2 | 带标签的注册 | struct io_uring_rsrc_register | 1 |
| IORING_REGISTER_BUFFERS_UPDATE | 更新固定缓冲区 | struct io_uring_rsrc_update2 | 1 |

### 事件通知

| 操作码 | 说明 | arg 类型 | nr_args |
|--------|------|----------|---------|
| IORING_REGISTER_EVENTFD | 注册 eventfd 完成通知 | int | 1 |
| IORING_UNREGISTER_EVENTFD | 注销 eventfd | NULL | 0 |
| IORING_REGISTER_EVENTFD_ASYNC | 异步事件 fd 注册 | int | 1 |

### 提供缓冲区 (Provided Buffers)

| 操作码 | 说明 | arg 类型 | nr_args |
|--------|------|----------|---------|
| IORING_REGISTER_PBUF_RING | 注册提供缓冲区环 | struct io_uring_buf_reg | 1 |
| IORING_UNREGISTER_PBUF_RING | 注销提供缓冲区环 | struct io_uring_buf_reg | 1 |
| IORING_REGISTER_PBUF_STATUS | 查询缓冲区组状态 | struct io_uring_buf_status | 1 |

### 工作队列控制

| 操作码 | 说明 | arg 类型 | nr_args |
|--------|------|----------|---------|
| IORING_REGISTER_IOWQ_AFF | 设置 io-wq CPU 亲和性 | cpumask | 1 |
| IORING_UNREGISTER_IOWQ_AFF | 取消 io-wq 亲和性 | NULL | 0 |
| IORING_REGISTER_IOWQ_MAX_WORKERS | 设置最大 worker 数 | __u32[2] | 2 |

### Ring 管理

| 操作码 | 说明 | arg 类型 | nr_args |
|--------|------|----------|---------|
| IORING_REGISTER_ENABLE_RINGS | 启用已禁用的 ring | NULL | 0 |
| IORING_REGISTER_RING_FDS | 注册 ring fd 表 | struct io_uring_rsrc_update[] | fd 数量 |
| IORING_UNREGISTER_RING_FDS | 注销 ring fd | struct io_uring_rsrc_update[] | fd 数量 |
| IORING_REGISTER_RESTRICTIONS | 注册限制 | struct io_uring_restriction[] | 限制数量 |
| IORING_REGISTER_CLOCK | 注册时钟源 | struct io_uring_clock_register | 1 |
| IORING_REGISTER_RESIZE_RINGS | 调整 CQ ring 大小 | struct io_uring_rsrc_update | 1 |
| IORING_REGISTER_MEM_REGION | 注册内存区域 | struct io_uring_mem_region_reg | 1 |
| IORING_REGISTER_QUERY | 查询 io_uring 能力 | union io_query_data | 1 |

### 取消操作

| 操作码 | 说明 | arg 类型 | nr_args |
|--------|------|----------|---------|
| IORING_REGISTER_SYNC_CANCEL | 同步取消请求 | struct io_uring_sync_cancel_reg | 1 |

### 零拷贝接收

| 操作码 | 说明 | arg 类型 | nr_args |
|--------|------|----------|---------|
| IORING_REGISTER_ZCRX_IFQ | 注册零拷贝接收队列 | struct io_uring_zcrx_ifq_reg | 1 |
| IORING_REGISTER_ZCRX_CTRL | 零拷贝接收控制 | struct zcrx_ctrl | 1 |

### 其他

| 操作码 | 说明 | arg 类型 | nr_args |
|--------|------|----------|---------|
| IORING_REGISTER_PROBE | 探测支持的 opcode | struct io_uring_probe | 1 |
| IORING_REGISTER_PERSONALITY | 注册执行身份 | __u32 | 1 |
| IORING_UNREGISTER_PERSONALITY | 注销身份 | __u32 | 1 |
| IORING_REGISTER_CLONE_BUFFERS | 克隆缓冲区 | struct io_uring_clone_buffers | 1 |
| IORING_REGISTER_SEND_MSG_RING | 发送消息到另一 ring | struct io_uring_sqe | 1 |
| IORING_REGISTER_NAPI | 注册 NAPI 设置 | struct io_uring_napi | 1 |
| IORING_UNREGISTER_NAPI | 注销 NAPI 设置 | struct io_uring_napi | 1 |
| IORING_REGISTER_BPF_FILTER | 注册 BPF 过滤程序 | struct io_uring_bpf_filter | 1 |
| IORING_REGISTER_FILE_ALLOC_RANGE | 注册文件分配范围 | struct io_uring_file_index_range | 1 |

## 完整调用链分析

### 主入口

```
io_uring_register(fd, opcode, arg, nr_args)
  │
  ├─ 处理 IORING_REGISTER_USE_REGISTERED_RING 标志
  │    ├─ use_registered_ring = !!(opcode & IORING_REGISTER_USE_REGISTERED_RING)
  │    └─ opcode &= ~IORING_REGISTER_USE_REGISTERED_RING
  │
  ├─ 检查 opcode 范围: if (opcode >= IORING_REGISTER_LAST) → -EINVAL
  │
  ├─ if (fd == -1):
  │    └─ io_uring_register_blind(opcode, arg, nr_args)  // 盲注册
  │         ├─ IORING_REGISTER_SEND_MSG_RING
  │         ├─ IORING_REGISTER_QUERY
  │         ├─ IORING_REGISTER_RESTRICTIONS
  │         └─ IORING_REGISTER_BPF_FILTER
  │
  ├─ 获取文件:
  │    └─ io_uring_register_get_file(fd, use_registered_ring)
  │         ├─ if registered: tctx->registered_rings[fd]
  │         └─ else: fget(fd)
  │    └─ ctx = file->private_data
  │
  ├─ mutex_lock(&ctx->uring_lock)
  │
  ├─ __io_uring_register(ctx, opcode, arg, nr_args)
  │    ├─ 检查 percpu_ref_is_dying(&ctx->refs) → -ENXIO
  │    ├─ 检查 submitter_task → -EEXIST (单提交者模式)
  │    ├─ 检查限制 (restrictions)
  │    └─ switch (opcode):
  │         ├─ IORING_REGISTER_BUFFERS → io_sqe_buffers_register()
  │         ├─ IORING_UNREGISTER_BUFFERS → io_sqe_buffers_unregister()
  │         ├─ IORING_REGISTER_FILES → io_sqe_files_register()
  │         ├─ IORING_UNREGISTER_FILES → io_sqe_files_unregister()
  │         ├─ IORING_REGISTER_FILES_UPDATE → io_register_files_update()
  │         ├─ IORING_REGISTER_EVENTFD → io_eventfd_register()
  │         ├─ IORING_REGISTER_PBUF_RING → io_register_pbuf_ring()
  │         ├─ IORING_REGISTER_ENABLE_RINGS → io_register_enable_rings()
  │         ├─ IORING_REGISTER_IOWQ_AFF → io_register_iowq_aff()
  │         ├─ IORING_REGISTER_IOWQ_MAX_WORKERS → io_register_iowq_max_workers()
  │         ├─ IORING_REGISTER_RING_FDS → io_ringfd_register()
  │         ├─ IORING_REGISTER_NAPI → io_register_napi()
  │         ├─ IORING_REGISTER_CLOCK → io_register_clock()
  │         ├─ IORING_REGISTER_QUERY → io_query()
  │         ├─ IORING_REGISTER_SYNC_CANCEL → io_sync_cancel()
  │         ├─ IORING_REGISTER_MEM_REGION → io_register_mem_region()
  │         ├─ IORING_REGISTER_BPF_FILTER → io_register_bpf_filter()
  │         └─ ... 更多操作码
  │
  ├─ trace_io_uring_register()
  ├─ mutex_unlock(&ctx->uring_lock)
  └─ fput(file)
```

### 核心操作详细流程

#### 固定文件注册 (IORING_REGISTER_FILES)

```
io_sqe_files_register(ctx, arg, nr_args)
  ├─ 分配 io_file_table (file_table)
  ├─ 循环注册每个 fd:
  │    ├─ fget(fd) → file
  │    ├─ 检查文件类型是否支持
  │    ├─ io_install_fixed_file(ctx, file, ...)
  │    └─ 将 file 存入 ctx->file_table
  └─ 设置 ctx->file_table.bitmap
```

#### 固定缓冲区注册 (IORING_REGISTER_BUFFERS)

```
io_sqe_buffers_register(ctx, arg, nr_args)
  ├─ 对每个 iovec:
  │    ├─ pin_user_pages_fast(addr, len, ...)  // 锁定用户页帧
  │    │    └─ 防止页面被换出，保证 DMA 安全
  │    ├─ 创建 struct io_mapped_ubuf (imu)
  │    ├─ 记录 imu 到 ctx->user_bufs
  │    └─ 累加已锁定的内存量
  └─ 返回注册的缓冲区数量
```

#### Eventfd 注册 (IORING_REGISTER_EVENTFD)

```
io_eventfd_register(ctx, arg, async)
  ├─ eventfd_ctx_fdget(arg) → ctx->io_ev_fd
  ├─ 设置 ctx->has_evfd = 1
  └─ 完成事件时: eventfd_signal(ctx->io_ev_fd)
```

#### 启用 Ring (IORING_REGISTER_ENABLE_RINGS)

```
io_register_enable_rings(ctx)
  ├─ 检查 IORING_SETUP_R_DISABLED → -EBADFD
  ├─ 若 SINGLE_ISSUER: 设置 submitter_task
  ├─ 清除 IORING_SETUP_R_DISABLED 标志
  └─ 唤醒可能的 SQPOLL 等待者
```

#### 注册 Ring FD (IORING_REGISTER_RING_FDS)

```
io_ringfd_register(ctx, arg, nr_args)
  ├─ __io_uring_add_tctx_node(ctx)  // 确保 tctx 存在
  ├─ 循环处理每个注册请求:
  │    ├─ copy_from_user(&reg, &arg[i])
  │    ├─ io_ring_add_registered_fd(tctx, fd, start, end)
  │    │    └─ 将 fd 存入 tctx->registered_rings[]
  │    └─ copy_to_user 返回 offset
  └─ 返回处理的数量
```

#### 同步取消 (IORING_REGISTER_SYNC_CANCEL)

```
io_sync_cancel(ctx, arg)
  ├─ copy_from_user(&reg, arg)
  ├─ 根据 reg.flags 确定匹配条件:
  │    ├─ IORING_ASYNC_CANCEL_ALL: 取消所有匹配请求
  │    ├─ IORING_ASYNC_CANCEL_FD: 按 fd 取消
  │    ├─ IORING_ASYNC_CANCEL_OP: 按 opcode 取消
  │    └─ IORING_ASYNC_CANCEL_ANY: 取消任意请求
  ├─ io_try_cancel() 遍历各取消路径
  │    ├─ io_async_cancel_one()  // 直接取消
  │    ├─ io_poll_cancel()       // 取消 poll 请求
  │    ├─ io_waitid_cancel()     // 取消 waitid 请求
  │    ├─ io_futex_cancel()      // 取消 futex 请求
  │    └─ io_timeout_cancel()    // 取消超时请求
  └─ 返回取消的请求数
```

## 关键数据结构

### struct io_ring_ctx 相关字段

```c
struct io_ring_ctx {
    // ... 其他字段

    struct io_file_table file_table;     // 固定文件表
    // 内部结构:
    //   struct io_rsrc_data data;    // 文件资源数据
    //   unsigned long *bitmap;       // 分配位图
    //   unsigned int alloc_hint;     // 分配提示

    struct io_rsrc_data buf_table;      // 缓冲区资源表
    //   unsigned int nr;             // 缓冲区数量
    //   struct io_rsrc_node **nodes; // 缓冲区节点数组

    struct io_ev_fd __rcu *io_ev_fd;    // eventfd 上下文
    unsigned int has_evfd:1;            // 是否注册了 eventfd

    struct io_restriction restrictions;  // 操作限制
    //   DECLARE_BITMAP(register_op, IORING_REGISTER_LAST);
    //   DECLARE_BITMAP(sqe_op, IORING_OP_LAST);
    //   struct io_bpf_filters *bpf_filters;
    //   u8 sqe_flags_allowed;
    //   u8 sqe_flags_required;

    struct io_mapped_region param_region; // 参数区域（用于固定等待参数）
    // ... 更多字段
};
```

### struct io_uring_task（每个任务的 io_uring 上下文）

```c
struct io_uring_task {
    int                  cached_refs;           // 缓存的 task 引用
    const struct io_ring_ctx *last;             // 最后使用的 ctx
    struct task_struct   *task;                 // 所属任务
    struct io_wq         *io_wq;                // io-wq 实例
    struct file          *registered_rings[IO_RINGFD_REG_MAX]; // 注册的 ring fd 表
    struct xarray        xa;                    // 各种资源索引
    struct wait_queue_head wait;                // 等待队列
    atomic_t             in_cancel;             // 取消中标记
    atomic_t             inflight_tracked;      // 飞行中请求计数
    struct percpu_counter inflight;             // per-CPU 飞行计数
    struct {
        struct llist_head   task_list;          // task_work 链表
        struct callback_head task_work;         // task_work 回调
    } ____cacheline_aligned_in_smp;
};
```

### struct io_uring_rsrc_register（资源注册参数）

```c
struct io_uring_rsrc_register {
    __u32 nr;              // 资源数量
    __u32 flags;           // 标志 (IORING_RSRC_REGISTER_SPARSE)
    __u64 resv2;           // 保留
    __aligned_u64 data;    // 数据指针（fd 数组或 iovec 数组）
    __aligned_u64 tags;    // 标签数组（可选）
};
```

### struct io_uring_rsrc_update（资源更新参数）

```c
struct io_uring_rsrc_update {
    __u32 offset;          // 要更新的索引
    __u32 resv;            // 保留
    __aligned_u64 data;    // 新数据值
};
```

## 流程图

### 固定文件注册流程

```
用户态                                         内核态
  |                                             |
  | int fds[] = {fd1, fd2, fd3};                |
  | io_uring_register(ring_fd,                  |
  |   IORING_REGISTER_FILES,                    |
  |   fds, 3)                                   |
  |------------------------------------------->|
  |                                             |
  |                 ├─ io_sqe_files_register()  |
  |                 │    ├─ 分配 file_table      |
  |                 │    ├─ fget(fd1) → file1   |
  |                 │    ├─ fget(fd2) → file2   |
  |                 │    ├─ fget(fd3) → file3   |
  |                 │    └─ 存入 ctx->file_table |
  |                 └─ 返回 0                   |
  |<-------------------------------------------|
  |                                             |
  | 后续 SQE 设置 IOSQE_FIXED_FILE 标志        |
  | 并使用 file_index 而非 fd                   |
  |                                             |
  | 每个 I/O 操作节省:                          |
  |   - fget/fput 开销                          |
  |   - 文件引用计数原子操作                     |
```

### 固定缓冲区注册流程

```
用户态                                         内核态
  |                                             |
  | struct iovec iov[] = {                      |
  |   {.iov_base=buf1, .iov_len=4096},          |
  |   {.iov_base=buf2, .iov_len=8192}};         |
  |                                             |
  | io_uring_register(ring_fd,                  |
  |   IORING_REGISTER_BUFFERS,                  |
  |   iov, 2)                                   |
  |------------------------------------------->|
  |                                             |
  |                 ├─ io_sqe_buffers_register()|
  |                 │    ├─ pin_user_pages(buf1) │
  |                 │    │    └─ 锁定 1 页      |
  |                 │    ├─ pin_user_pages(buf2) │
  |                 │    │    └─ 锁定 2 页      |
  |                 │    ├─ 创建 imu 结构       |
  |                 │    └─ 存入 ctx->buf_table  |
  |                 └─ 返回 2                   |
  |<-------------------------------------------|
  |                                             |
  | 后续 I/O 设置 buf_index                     |
  | 使用固定缓冲区，节省:                        |
  |   - 每次 DMA 映射/取消映射开销               |
  |   - 页锁定/解锁开销                          |
```

## 错误处理

| 错误码 | 条件 |
|--------|------|
| -EBADF | fd 无效或不是 io_uring 文件 |
| -EINVAL | opcode 无效、参数组合无效、opcode >= IORING_REGISTER_LAST |
| -EFAULT | copy_from_user 失败 |
| -ENOMEM | 内存分配失败 |
| -ENXIO | 引用计数已 dying 或 ATTACH_WQ 目标已死 |
| -EEXIST | 单提交者模式但调用者错误 |
| -EACCES | 操作被限制 (restrictions) 阻止 |
| -EBADFD | 启用 ring 时 ring 未处于禁用状态 |
| -EOPNOTSUPP | 操作不被支持（如不兼容的 fd 类型） |
| -ENOSPC | 固定文件表已满 |
| -EOVERFLOW | 计算溢出 |

## 使用示例

### 注册固定文件

```c
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main(void)
{
    struct io_uring ring;
    int fds[2];
    int ret;

    // 初始化 ring
    io_uring_queue_init(32, &ring, 0);

    // 打开文件
    fds[0] = open("/path/to/file1", O_RDONLY);
    fds[1] = open("/path/to/file2", O_RDONLY);
    if (fds[0] < 0 || fds[1] < 0) {
        perror("open");
        return 1;
    }

    // 注册固定文件
    ret = io_uring_register_files(&ring, fds, 2);
    if (ret < 0) {
        fprintf(stderr, "register files failed: %s\n", strerror(-ret));
        return 1;
    }

    // 使用固定文件提交 I/O
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    // 注意: 使用 IOSQE_FIXED_FILE 标志和 file_index
    io_uring_prep_read_fixed(sqe, 0, /* file_index=0 */,
                             buf, sizeof(buf), 0);
    // 或:
    io_uring_prep_read(sqe, fds[0], buf, sizeof(buf), 0);
    sqe->flags |= IOSQE_FIXED_FILE;
    sqe->fd = 0;  // 固定文件索引

    io_uring_submit(&ring);
    // ... 处理完成事件

    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
    return 0;
}
```

### 注册固定缓冲区

```c
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_SIZE 4096

int main(void)
{
    struct io_uring ring;
    struct iovec iov;
    char *buf;
    int ret;

    // 初始化 ring
    io_uring_queue_init(32, &ring, 0);

    // 分配缓冲区
    buf = aligned_alloc(4096, BUF_SIZE);
    if (!buf) {
        perror("aligned_alloc");
        return 1;
    }
    memset(buf, 0, BUF_SIZE);

    // 注册固定缓冲区
    iov.iov_base = buf;
    iov.iov_len = BUF_SIZE;
    ret = io_uring_register_buffers(&ring, &iov, 1);
    if (ret < 0) {
        fprintf(stderr, "register buffers failed: %s\n", strerror(-ret));
        return 1;
    }

    // 使用固定缓冲区提交 I/O
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read_fixed(sqe, fd, buf, BUF_SIZE, 0, 0);
    // buf_index=0 指向注册的第一个缓冲区

    io_uring_submit(&ring);
    // ... 处理完成事件

    // 注销缓冲区
    io_uring_unregister_buffers(&ring);

    io_uring_queue_exit(&ring);
    free(buf);
    return 0;
}
```

### 注册 eventfd 通知

```c
#include <liburing.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    struct io_uring ring;
    int efd;

    io_uring_queue_init(32, &ring, 0);

    // 创建 eventfd
    efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) {
        perror("eventfd");
        return 1;
    }

    // 注册 eventfd 完成通知
    // 当有 CQE 产生时，内核会向 eventfd 写入
    io_uring_register_eventfd(&ring, efd);

    // 现在可以在另一个线程/进程中使用 epoll/poll/select
    // 监听 eventfd 来获取完成通知

    // 注销
    io_uring_unregister_eventfd(&ring);
    close(efd);
    io_uring_queue_exit(&ring);
    return 0;
}
```

### 启用延迟 Ring

```c
#include <liburing.h>
#include <stdio.h>

int main(void)
{
    struct io_uring ring;

    // 创建时禁用 ring
    io_uring_queue_init(32, &ring, IORING_SETUP_R_DISABLED);

    // 执行一些初始化设置（注册文件、缓冲区、限制等）
    // ...

    // 启用 ring，开始接受 I/O 请求
    io_uring_register_ring_fd(&ring, -1);  // 或 io_uring_enable_rings(&ring)

    // 现在可以正常使用
    // ...

    io_uring_queue_exit(&ring);
    return 0;
}
```

## 性能优化要点

1. **固定文件**: 注册后使用 `IOSQE_FIXED_FILE` 标志，节省每次 I/O 的 `fget/fput` 原子操作
2. **固定缓冲区**: 注册后 DMA 映射只需一次，后续 I/O 复用映射，避免页锁定/解锁开销
3. **Eventfd 通知**: 适用于多线程场景，避免所有线程都在 io_uring_enter 上阻塞
4. **Ring FD 注册**: 注册 ring fd 后使用 `IORING_ENTER_REGISTERED_RING` 标志，节省 `fget/fput` 开销
5. **BPF 过滤**: 通过注册 BPF 程序在提交路径上执行过滤，避免不必要的请求处理
6. **内存区域注册**: 注册固定等待参数区域，避免每次等待时从用户态拷贝参数
7. **延迟初始化**: 使用 `IORING_SETUP_R_DISABLED` 创建 ring，注册所有资源后再启用，确保资源全部就绪

## 设计要点

1. **资源预注册**: 将运行时开销转移到初始化阶段，通过预注册消除每个 I/O 操作的固定开销
2. **引用计数优化**: 固定文件避免每次 I/O 的 fget/fput，固定缓冲区避免每次 I/O 的页锁定
3. **批量操作**: 支持批量注册和更新文件/缓冲区，减少系统调用次数
4. **资源隔离**: 每个 io_uring 实例有独立的文件表和缓冲区表，互不干扰
5. **安全限制**: 通过 IORING_REGISTER_RESTRICTIONS 限制可用的 opcode 和 flags，增强安全性
6. **BPF 集成**: 支持 BPF 过滤程序，在提交路径上实现灵活的访问控制