# io_setup 系统调用分析

## 1. 概述

`io_setup` 用于创建一个 AIO（异步 I/O）上下文。每个上下文包含一个可映射到用户空间的环形缓冲区，以及用于管理 I/O 请求的内部数据结构。

**原型：**

```c
SYSCALL_DEFINE2(io_setup, unsigned, nr_events, aio_context_t __user *, ctxp)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `nr_events` | 上下文可同时处理的最大事件数 |
| `ctxp` | 输出参数，接收创建的上下文 ID |

## 3. 函数调用链

```
io_setup (系统调用入口)
  │
  ├─ ioctx = ioctx_alloc(nr_events)                     // 分配 AIO 上下文
  │    │
  │    ├─ kzalloc(sizeof(*ctx), GFP_KERNEL)              // 分配 kioctx
  │    │
  │    ├─ aio_setup_ring(ctx, nr_events)                 // 分配环形缓冲区
  │    │    ├─ 计算需要的页数 (nr_events * sizeof(io_event) + sizeof(aio_ring))
  │    │    ├─ 分配物理页
  │    │    ├─ aio_ring_mmap(ctx, ctx->mmap_base, ...)   // mmap 映射到用户空间
  │    │    └─ 初始化环形缓冲区头
  │    │
  │    ├─ INIT_KFIFO(ctx->completed_events)              // 初始化完成事件 FIFO
  │    ├─ spin_lock_init(&ctx->ctx_lock)
  │    ├─ ctx->max_reqs = nr_events
  │    ├─ percpu_ref_init(&ctx->users, ...)              // 初始化引用计数
  │    │
  │    └─ 返回 ioctx
  │
  ├─ ret = put_user(ioctx->user_id, ctxp)                // 返回 ctx ID 到用户空间
  │
  └─ return 0
```

## 4. 关键数据结构

### 4.1 struct kioctx（AIO 上下文）

```c
// fs/aio.c
struct kioctx {
    struct percpu_ref    users;           /* 用户引用计数 */
    atomic_t             dead;            /* 标记是否已销毁 */

    struct percpu_ref    reqs;            /* 请求引用计数 */

    unsigned long        user_id;         /* 用户态 ctx ID（返回给用户） */

    struct kioctx_cpu __percpu *cpu;      /* per-CPU 数据 */

    unsigned             req_batch;       /* 请求批处理大小 */
    unsigned             max_reqs;        /* 最大请求数 */

    /* 环形缓冲区信息 */
    struct aio_ring_info ring_info;

    /* 完成事件 FIFO */
    struct __kfifo       completed_events;

    /* 等待队列 */
    wait_queue_head_t    wait;

    spinlock_t           ctx_lock;        /* 上下文锁 */
    struct list_head     active_reqs;     /* 活跃请求链表 */

    /* ... 更多字段 ... */
};
```

### 4.2 struct aio_ring_info（环形缓冲区）

```c
// fs/aio.c
struct aio_ring_info {
    unsigned long        mmap_base;       /* 用户空间 mmap 基址 */
    unsigned long        mmap_size;       /* mmap 大小 */

    struct page         **ring_pages;     /* 环形缓冲区页数组 */
    long                 nr_pages;        /* 页数 */

    unsigned             nr;              /* 事件总数 */
    unsigned             head;            /* 头指针 */
    unsigned             tail;            /* 尾指针 */

    struct aio_ring      *ring;           /* 指向环形缓冲区的指针 */
};
```

## 5. 流程图

```
用户态调用 io_setup(nr_events, ctxp)
  │
  v
ioctx_alloc(nr_events)
  │
  ├── kzalloc() 分配 kioctx 结构
  │
  ├── aio_setup_ring() 分配环形缓冲区
  │    ├── 计算所需页数
  │    ├── alloc_page() 分配物理页
  │    └── mmap 到用户空间
  │
  ├── 初始化 FIFO 和锁
  │
  ├── 设置 max_reqs = nr_events
  │
  ├── percpu_ref_init 引用计数
  │
  ├── 将 ioctx 添加到当前进程的 ctx_table
  │
  └── put_user(user_id, ctxp) 返回给用户
```

## 6. 环形缓冲区布局

```
用户空间可访问的环形缓冲区:

  ┌──────────────────────────────────────────┐
  │ struct aio_ring (头部信息)                │
  │   - id (对应 ctx_id)                      │
  │   - nr (事件总数)                         │
  │   - head (内核写入位置)                    │
  │   - tail (用户读取位置)                    │
  ├──────────────────────────────────────────┤
  │ struct io_event[0]                        │
  │ struct io_event[1]                        │
  │ ...                                       │
  │ struct io_event[nr_events - 1]            │
  └──────────────────────────────────────────┘

  用户空间可以直接读取 io_event 而无需系统调用
  （通过比较 head 和 tail 检查是否有新事件）
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | nr_events 超出限制（< 1 或 > AIO_MAX） |
| `ENOMEM` | 内存不足 | 无法分配 kioctx 或环形缓冲区页面 |
| `EFAULT` | 地址错误 | ctxp 指针不可访问 |
| `EAGAIN` | 暂时不可用 | 系统级 AIO 上下文数量已达上限 |

## 8. 使用示例

```c
#include <linux/aio_abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main() {
    aio_context_t ctx = 0;

    // 创建可处理 128 个事件的 AIO 上下文
    if (io_setup(128, &ctx) < 0) {
        perror("io_setup");
        exit(1);
    }
    printf("AIO context created: id=%ld\n", ctx);

    // 使用 ctx 提交请求...

    io_destroy(ctx);
    return 0;
}
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#异步-i/o-aio)
- 源码位置：`fs/aio.c`（第 1381 行）
- 用户态头文件：`linux/aio_abi.h`