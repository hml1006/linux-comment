# io_uring_setup - 创建 io_uring 实例

## 概述

`io_uring_setup` 是 io_uring 框架的入口系统调用，用于创建和初始化一个 io_uring 实例。它分配一个 `io_ring_ctx` 上下文结构，创建提交队列（SQ）和完成队列（CQ）的共享内存区域，并返回一个文件描述符供用户态通过 mmap 映射访问环形缓冲区。

io_uring 的设计目标是消除传统同步 I/O 系统调用的开销，通过共享内存的环形缓冲区在用户态和内核态之间高效传递 I/O 请求和完成事件。

## 函数原型

```c
SYSCALL_DEFINE2(io_uring_setup, u32, entries,
        struct io_uring_params __user *, params);
```

- **entries**: 请求的 SQ 条目数（内核会据此计算 CQ 大小，通常 CQ 条目数 = 2 * entries）
- **params**: 配置参数结构体指针，同时用于返回实际的环形缓冲区布局信息

## 参数详解

### struct io_uring_params

```c
struct io_uring_params {
    __u32 sq_entries;           // [输出] 实际 SQ 条目数
    __u32 cq_entries;           // [输出] 实际 CQ 条目数
    __u32 flags;                // [输入] 创建标志
    __u32 sq_thread_cpu;        // [输入] SQPOLL 线程绑定的 CPU
    __u32 sq_thread_idle;       // [输入] SQPOLL 线程空闲超时(毫秒)
    __u32 features;             // [输出] 内核支持的特性位图
    __u32 wq_fd;                // [输入] 要附加的现有 io_uring 实例的 fd
    __u32 resv[3];              // 保留字段，必须为 0
    struct io_sqring_offsets sq_off;  // [输出] SQ ring 的成员偏移量
    struct io_cqring_offsets cq_off;  // [输出] CQ ring 的成员偏移量
};
```

### 关键标志位 (flags)

| 标志 | 值 | 说明 |
|------|------|------|
| IORING_SETUP_IOPOLL | (1U << 0) | 使用 I/O 轮询模式（需设备支持） |
| IORING_SETUP_SQPOLL | (1U << 1) | 创建内核轮询线程自动提交 SQE |
| IORING_SETUP_SQ_AFF | (1U << 2) | 绑定 SQPOLL 线程到指定 CPU |
| IORING_SETUP_CQSIZE | (1U << 3) | 允许应用自定义 CQ 大小 |
| IORING_SETUP_CLAMP | (1U << 4) | 将 entries 限制在允许范围内 |
| IORING_SETUP_ATTACH_WQ | (1U << 5) | 附加到现有 io_uring 的 io-wq |
| IORING_SETUP_R_DISABLED | (1U << 6) | 创建时禁用 ring，需手动启用 |
| IORING_SETUP_SUBMIT_ALL | (1U << 7) | 出错时继续提交剩余 SQE |
| IORING_SETUP_COOP_TASKRUN | (1U << 8) | 协作式任务运行，减少 IPI |
| IORING_SETUP_TASKRUN_FLAG | (1U << 9) | 设置 SQ 标志位通知任务运行 |
| IORING_SETUP_SQE128 | (1U << 10) | SQE 大小为 128 字节 |
| IORING_SETUP_CQE32 | (1U << 11) | CQE 大小为 32 字节 |
| IORING_SETUP_SINGLE_ISSUER | (1U << 12) | 仅允许单个任务提交请求 |
| IORING_SETUP_DEFER_TASKRUN | (1U << 13) | 延迟执行任务工作 |
| IORING_SETUP_NO_MMAP | (1U << 14) | 应用提供 ring 内存 |
| IORING_SETUP_REGISTERED_FD_ONLY | (1U << 15) | 返回注册的 fd 索引而非 fd |
| IORING_SETUP_NO_SQARRAY | (1U << 16) | 移除 SQ 索引数组间接层 |
| IORING_SETUP_HYBRID_IOPOLL | (1U << 17) | 混合轮询模式 |
| IORING_SETUP_CQE_MIXED | (1U << 18) | 允许混合 16B/32B CQE |
| IORING_SETUP_SQE_MIXED | (1U << 19) | 允许混合 64B/128B SQE |
| IORING_SETUP_SQ_REWIND | (1U << 20) | 从索引 0 开始获取 SQE |

## 完整调用链分析

```
io_uring_setup(entries, params)
  ├─ io_uring_allowed()                                    // 权限检查
  │    ├─ sysctl_io_uring_disabled == 2 → -EPERM          // 全局禁用
  │    ├─ disabled == 0 || CAP_SYS_ADMIN → 通过
  │    ├─ in_group_p(io_uring_group) ?                    // 组检查
  │    └─ security_uring_allowed()                         // LSM 检查
  │
  └─ io_uring_setup(entries, params)
       └─ io_uring_create(&config)
            ├─ io_prepare_config(config)                   // 校验并计算布局
            │    ├─ 校验 flags 合法性
            │    ├─ io_uring_sanitise_params()             // 参数清理
            │    ├─ rings_size() → 计算 SQ/CQ 内存布局
            │    │    ├─ 计算 sq_size = sqe_size * sq_entries
            │    │    ├─ 计算 rings_size = sizeof(io_rings) + cqe_size * cq_entries
            │    │    └─ 对齐到缓存行
            │    └─ 设置 sq_entries/cq_entries
            │
            ├─ io_ring_ctx_alloc(p)                        // 分配上下文
            │    ├─ kzalloc(sizeof(*ctx))                   // 分配 io_ring_ctx
            │    ├─ xa_init(&ctx->io_bl_xa)                // 初始化 buffer list XArray
            │    ├─ io_alloc_hash_table(&ctx->cancel_table) // 分配取消哈希表
            │    ├─ percpu_ref_init(&ctx->refs)            // 初始化引用计数
            │    ├─ mutex_init(&ctx->uring_lock)           // 初始化各锁
            │    ├─ spin_lock_init(&ctx->completion_lock)
            │    ├─ raw_spin_lock_init(&ctx->timeout_lock)
            │    ├─ init_waitqueue_head(&ctx->cq_wait)     // 初始化等待队列
            │    ├─ init_waitqueue_head(&ctx->sqo_sq_wait)
            │    ├─ io_alloc_cache_init() 系列             // 初始化对象缓存
            │    └─ INIT_LIST_HEAD 系列                     // 初始化链表
            │
            ├─ 设置上下文属性
            │    ├─ ctx->clockid = CLOCK_MONOTONIC
            │    ├─ 根据 flags 设置 task_complete/lockless_cq/poll_activated
            │    ├─ 设置 syscall_iopoll (IOPOLL 且非 SQPOLL)
            │    ├─ 设置 notify_method (TWA_SIGNAL_NO_IPI 或 TWA_SIGNAL)
            │    ├─ 复制 task 限制 (如果有)
            │    └─ mmgrab(current->mm) 用于内存记账
            │
            ├─ io_allocate_scq_urings(ctx, config)          // 分配 SQ/CQ 共享内存
            │    ├─ io_create_region(&ctx->ring_region)     // 分配 rings 内存
            │    │    ├─ 包含 SQ head/tail, CQ head/tail, 标志位, CQE 数组
            │    │    └─ ctx->rings = 映射后的指针
            │    ├─ 设置 ctx->sq_array (SQ 索引数组, 位于 ring 内存尾部)
            │    └─ io_create_region(&ctx->sq_region)       // 分配 SQEs 内存
            │         └─ ctx->sq_sqes = 映射后的指针
            │
            ├─ io_sq_offload_create(ctx, p)                 // 创建 SQPOLL (可选)
            │    ├─ 若未设置 IORING_SETUP_SQPOLL → 直接返回
            │    ├─ security_uring_sqpoll()                 // LSM 检查
            │    ├─ io_get_sq_data(p, &attached)            // 获取/创建 sq_data
            │    │    ├─ 若设 ATTACH_WQ → 尝试附加到现有 sq_data
            │    │    └─ 否则 → kzalloc 新的 io_sq_data
            │    ├─ 初始化 sq_data (锁、等待队列、引用计数)
            │    ├─ 将 ctx 加入 sqd->ctx_list
            │    └─ 若创建新线程:
            │         ├─ set_cpus_allowed 绑定 CPU
            │         ├─ create_io_thread(io_sq_thread, sqd)  // 创建内核线程
            │         └─ wake_up_new_task(tsk)              // 启动线程
            │
            ├─ 设置 features 并 copy_to_user(params)         // 返回信息给用户
            │
            ├─ io_uring_get_file(ctx)                        // 创建匿名文件
            │    └─ anon_inode_create_getfile("[io_uring]", ...)
            │
            ├─ __io_uring_add_tctx_node(ctx)                 // 绑定 task 上下文
            │    └─ 创建/获取 io_uring_task (tctx)
            │
            └─ io_uring_install_fd(file)                     // 安装 fd
                 ├─ get_unused_fd_flags(O_RDWR | O_CLOEXEC)
                 └─ fd_install(fd, file)
```

## 关键数据结构

### struct io_ring_ctx（核心上下文）

```c
struct io_ring_ctx {
    /* 只读或热数据缓存行 */
    struct {
        unsigned int        flags;          // IORING_SETUP_* 标志
        unsigned int        task_complete:1; // 仅提交者任务完成 CQE
        unsigned int        lockless_cq:1;   // 无锁 CQ 操作
        unsigned int        syscall_iopoll:1;// 系统调用 IOPOLL 模式
        struct task_struct  *submitter_task; // 提交者任务
        struct io_rings     *rings;          // 指向共享 ring 内存
        struct percpu_ref   refs;            // 引用计数
        clockid_t           clockid;         // 时钟 ID
    } ____cacheline_aligned_in_smp;

    /* 提交数据缓存行 */
    struct {
        struct mutex        uring_lock;      // 提交序列化锁
        u32                 *sq_array;       // SQ 索引数组
        struct io_uring_sqe *sq_sqes;        // SQE 数组
        unsigned            cached_sq_head;  // 缓存的 SQ head
        unsigned            sq_entries;      // SQ 条目数
        struct list_head    iopoll_list;     // IOPOLL 请求链表
        struct io_file_table file_table;     // 固定文件表
        struct io_submit_state submit_state; // 提交状态
    } ____cacheline_aligned_in_smp;

    /* CQ 缓存行 */
    struct {
        struct io_uring_cqe *cqe_cached;     // 缓存的 CQE
        struct io_uring_cqe *cqe_sentinel;   // CQE 哨兵
        unsigned            cached_cq_tail;  // 缓存的 CQ tail
        unsigned            cq_entries;      // CQ 条目数
    } ____cacheline_aligned_in_smp;

    struct wait_queue_head  cq_wait;         // CQ 等待队列
    spinlock_t              completion_lock; // 完成锁
    struct list_head        cq_overflow_list; // CQ 溢出列表
    struct io_sq_data       *sq_data;        // SQPOLL 数据
    struct io_mapped_region sq_region;       // SQE 内存区域
    struct io_mapped_region ring_region;     // Ring 内存区域
    // ... 更多字段
};
```

### struct io_rings（共享内存环形缓冲区）

```c
struct io_rings {
    struct io_uring  sq;           // SQ head/tail
    struct io_uring  cq;           // CQ head/tail
    u32              sq_ring_mask;  // SQ 掩码 (entries - 1)
    u32              cq_ring_mask;  // CQ 掩码 (entries - 1)
    u32              sq_ring_entries; // SQ 条目数
    u32              cq_ring_entries; // CQ 条目数
    u32              sq_dropped;    // 丢弃的 SQ 条目计数
    atomic_t         sq_flags;      // SQ 标志位
    u32              cq_flags;      // CQ 标志位
    u32              cq_overflow;   // CQ 溢出计数
    struct io_uring_cqe cqes[];     // CQE 数组（柔性数组）
};
```

### struct io_uring_sqe（提交队列条目）

```c
struct io_uring_sqe {
    __u8    opcode;       // 操作码（IORING_OP_*）
    __u8    flags;        // IOSQE_* 标志
    __u16   ioprio;       // I/O 优先级
    __s32   fd;           // 文件描述符
    __u64   off;          // 文件偏移
    __u64   addr;         // 缓冲区地址或 iovec 指针
    __u32   len;          // 缓冲区长度或 iovec 数
    union { __u32 rw_flags; /* 操作特定标志 */ };
    __u64   user_data;    // 用户自定义数据（在 CQE 中原样返回）
    __u16   buf_index;    // 固定缓冲区索引
    __u16   personality;  // 执行身份
    union { __s32 splice_fd_in; /* 其他字段 */ };
    __u64   addr3;        // 第三地址字段
};
```

### struct io_uring_cqe（完成队列条目）

```c
struct io_uring_cqe {
    __u64   user_data;  // 对应 SQE 的 user_data
    __s32   res;        // 结果码（类似系统调用的返回值）
    __u32   flags;      // 标志位（IORING_CQE_F_*）
};
```

## 内存布局与 mmap 偏移

```
mmap 偏移:
  IORING_OFF_SQ_RING  (0x0)         → SQ ring (包含 head/tail/标志位)
  IORING_OFF_CQ_RING  (0x8000000)   → CQ ring (包含 CQE 数组)
  IORING_OFF_SQES     (0x10000000)  → SQE 数组

当 IORING_FEAT_SINGLE_MMAP 特性可用时:
  SQ ring 和 CQ ring 可以单次 mmap 映射（使用 SQ_RING 偏移映射 IORING_OFF_CQ_RING 大小）
```

## 流程图

```
用户态                                     内核态
  |                                         |
  | io_uring_setup(entries, &params)        |
  |---------------------------------------->|
  |                                         |-- io_uring_allowed() 权限检查
  |                                         |-- io_ring_ctx_alloc() 分配 ctx
  |                                         |-- io_allocate_scq_urings()
  |                                         |     |-- 分配 ring 共享内存
  |                                         |     |-- 分配 SQE 共享内存
  |                                         |-- io_sq_offload_create()
  |                                         |     |-- (可选) 创建 SQPOLL 内核线程
  |                                         |-- 创建匿名文件 [io_uring]
  |                                         |-- 安装 fd
  |<----------------------------------------|
  | 返回 fd                                 |
  |                                         |
  | mmap(fd, IORING_OFF_SQ_RING, size)      |
  |---------------------------------------->|
  |<----------------------------------------|
  | 返回 SQ ring 虚拟地址                    |
  |                                         |
  | mmap(fd, IORING_OFF_SQES, size)         |
  |---------------------------------------->|
  |<----------------------------------------|
  | 返回 SQE 虚拟地址                        |
  |                                         |
  | 现在可以开始提交 I/O 请求                 |
```

## 错误处理

| 错误码 | 条件 |
|--------|------|
| -EPERM | io_uring 被全局禁用 (sysctl_io_uring_disabled=2) 或用户不在允许组中，或 LSM 阻止 |
| -EINVAL | flags 包含无效位，或 resv 字段非零，或参数组合无效（如 SQ_AFF 无 SQPOLL） |
| -ENOMEM | 内存分配失败（ctx、ring 内存、SQE 内存等） |
| -EFAULT | copy_from_user(params) 失败 |
| -ENXIO | ATTACH_WQ 指定的 wq_fd 无效 |
| -EOPNOTSUPP | 在不支持的架构上调用 |

## 使用示例

```c
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct io_uring ring;
    struct io_uring_params params = {0};

    // 方式一：使用 liburing 简化 API
    if (io_uring_queue_init(32, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        return 1;
    }

    // 方式二：直接使用系统调用（等价于上述）
    int fd = io_uring_setup(32, &params);
    if (fd < 0) {
        perror("io_uring_setup");
        return 1;
    }

    // 检查特性支持
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        printf("支持单次 mmap\n");
    }

    // mmap 映射环形缓冲区
    size_t sq_size = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    size_t cq_size = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);

    void *sq_ptr = mmap(0, sq_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd,
                        IORING_OFF_SQ_RING);
    void *sqe_ptr = mmap(0, sq_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, fd,
                         IORING_OFF_SQES);
    void *cq_ptr = mmap(0, cq_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd,
                        IORING_OFF_CQ_RING);

    // ... 使用 ring 进行 I/O 操作

    close(fd);
    return 0;
}
```

## 设计要点

1. **共享内存模型**: SQ 和 CQ 通过 mmap 共享给用户态，避免了系统调用的数据拷贝开销
2. **无锁通信**: 用户态写 SQ tail，内核读；内核写 CQ tail，用户态读。通过内存屏障保证顺序
3. **SQPOLL 模式**: 内核线程主动轮询 SQ，应用无需调用 io_uring_enter，适合延迟敏感场景
4. **IOPOLL 模式**: 内核主动轮询设备完成，适合高速块设备
5. **固定文件/缓冲区**: 可通过 io_uring_register 预注册，消除每次 I/O 的 fget/fput 和 DMA 映射开销
6. **链接请求**: 通过 IOSQE_IO_LINK 将多个 SQE 链接成原子序列