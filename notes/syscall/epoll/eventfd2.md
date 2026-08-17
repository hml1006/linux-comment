# eventfd2 系统调用分析

## 1. 概述

`eventfd2` 用于创建一个轻量级的事件通知文件描述符。它维护一个 64 位无符号计数器，用户空间可以通过 `read`/`write` 操作原子性地读取和增加该计数器，内核空间可以通过 `eventfd_signal` 函数原子性地递增计数器并唤醒等待进程。它常用于用户空间与内核空间之间的事件通知机制。

**原型：**

```c
#include <sys/eventfd.h>

int eventfd2(unsigned int initval, int flags);
```

**内核入口：**

```c
// fs/eventfd.c:414
SYSCALL_DEFINE2(eventfd2, unsigned int, count, int, flags)
{
    return do_eventfd(count, flags);
}
```

## 2. 使用场景

- **内核事件通知**：AIO、io_uring 等子系统使用 eventfd 通知 I/O 完成
- **用户空间事件通知**：跨线程/跨进程的事件通知
- **epoll 集成**：作为 epoll 监控的事件源，结合 epoll 实现多路复用
- **KVM 虚拟化**：KVM 使用 eventfd 通知虚拟机 I/O 事件
- **信号量**：使用 `EFD_SEMAPHORE` 标志实现类似信号量的行为

## 3. 函数调用栈

```
eventfd2(count, flags)                                  // 系统调用入口
  └─ do_eventfd(count, flags)                           // fs/eventfd.c:379
       ├─ BUILD_BUG_ON(EFD_CLOEXEC != O_CLOEXEC)        // 编译时常量检查
       ├─ BUILD_BUG_ON(EFD_NONBLOCK != O_NONBLOCK)
       ├─ BUILD_BUG_ON(EFD_SEMAPHORE != (1 << 0))
       │
       ├─ flags & ~EFD_FLAGS_SET → 返回 -EINVAL         // 无效标志
       │
       ├─ ctx = kmalloc(sizeof(*ctx), GFP_KERNEL)       // 分配 eventfd_ctx
       │   └─ 失败 → 返回 -ENOMEM
       │
       ├─ kref_init(&ctx->kref)                         // 引用计数初始化
       ├─ init_waitqueue_head(&ctx->wqh)                // 初始化等待队列
       ├─ ctx->count = count                            // 设置初始值
       ├─ ctx->flags = flags                            // 保存标志
       │
       ├─ anon_inode_getfile_fmode("[eventfd]", &eventfd_fops, ctx, ...)
       │   └─ 创建匿名文件，关联 eventfd 操作函数集
       │
       ├─ ctx->id = ida_alloc(&eventfd_ida, GFP_KERNEL) // 分配 ID
       └─ fd_install(fd, file)                          // 安装 fd
```

**核心实现源码：**

```c
// fs/eventfd.c:379
static int do_eventfd(unsigned int count, int flags)
{
    struct eventfd_ctx *ctx __free(kfree) = NULL;

    /* Check the EFD_* constants for consistency.  */
    BUILD_BUG_ON(EFD_CLOEXEC != O_CLOEXEC);
    BUILD_BUG_ON(EFD_NONBLOCK != O_NONBLOCK);
    BUILD_BUG_ON(EFD_SEMAPHORE != (1 << 0));

    if (flags & ~EFD_FLAGS_SET)
        return -EINVAL;

    ctx = kmalloc_obj(*ctx);
    if (!ctx)
        return -ENOMEM;

    kref_init(&ctx->kref);
    init_waitqueue_head(&ctx->wqh);
    ctx->count = count;
    ctx->flags = flags;

    flags &= EFD_SHARED_FCNTL_FLAGS;
    flags |= O_RDWR;

    FD_PREPARE(fdf, flags,
               anon_inode_getfile_fmode("[eventfd]", &eventfd_fops, ctx,
                                        flags, FMODE_NOWAIT));
    if (fdf.err)
        return fdf.err;

    ctx->id = ida_alloc(&eventfd_ida, GFP_KERNEL);
    // ...
    return fd_publish(fdf);
}
```

## 4. 关键数据结构

### 4.1 struct eventfd_ctx（eventfd 上下文）

```c
// fs/eventfd.c:30
struct eventfd_ctx {
    struct kref kref;                // 引用计数
    wait_queue_head_t wqh;           // 等待队列（read/poll/epoll 阻塞）
    /*
     * 每次 write(2) 操作时，写入的 __u64 值被加到 "count" 上，
     * 并在 "wqh" 上执行唤醒。如果未指定 EFD_SEMAPHORE 标志，
     * read(2) 将返回 "count" 值并将 "count" 重置为零。
     * 内核侧的 eventfd_signal() 也会增加 "count" 计数器并触发唤醒。
     */
    __u64 count;                     // 64 位计数器
    unsigned int flags;              // 创建标志
    int id;                          // eventfd ID（用于 IDA 管理）
};
```

### 4.2 eventfd 操作函数

```c
// eventfd 读操作（用户空间）
static ssize_t eventfd_read(struct file *file, char __user *buf, size_t count, ...)
{
    // 阻塞直到 count > 0
    // 非 EFD_SEMAPHORE 模式：返回 count 值，重置为 0
    // EFD_SEMAPHORE 模式：返回 1，count 减 1
}

// eventfd 写操作（用户空间）
static ssize_t eventfd_write(struct file *file, const char __user *buf, ...)
{
    // 读取用户写入的 __u64 值
    // 检查是否会溢出 (count + val > ULLONG_MAX - 1)
    // 阻塞直到不会溢出
    // 原子性增加 count
}

// eventfd 内核信号通知
void eventfd_signal_mask(struct eventfd_ctx *ctx, __poll_t mask)
{
    // 原子性增加 count
    // 唤醒等待队列中的进程
}
```

## 5. 流程图

```
用户态调用 eventfd2(initval, flags)
    │
    ▼
do_eventfd(count, flags)
    │
    ├─ 验证 flags
    │   └─ 无效标志 → -EINVAL
    │
    ├─ 分配 eventfd_ctx
    │   ├─ kmalloc(sizeof(*ctx))
    │   ├─ kref_init(&ctx->kref)
    │   ├─ init_waitqueue_head(&ctx->wqh)
    │   ├─ ctx->count = count
    │   └─ ctx->flags = flags
    │
    ├─ anon_inode_getfile_fmode("[eventfd]", ...)
    │   ├─ 创建匿名文件
    │   └─ 关联 eventfd_fops
    │
    └─ fd_install(fd, file)
        └─ 返回 fd
```

### eventfd 读写流程

```
eventfd 读操作 (read)
    │
    ├─ spin_lock_irq(&ctx->wqh.lock)
    │
    ├─ [count == 0 && 非阻塞] → 返回 -EAGAIN
    │
    ├─ [count == 0 && 阻塞] → 等待
    │   └─ 等待队列睡眠，直到 count > 0
    │
    ├─ [EFD_SEMAPHORE]
    │   ├─ 返回 1
    │   └─ count -= 1
    │
    └─ [非 EFD_SEMAPHORE]
        ├─ 返回 count 当前值
        └─ count = 0

eventfd 写操作 (write)
    │
    ├─ 从用户空间读取 __u64 值 n
    │
    ├─ spin_lock_irq(&ctx->wqh.lock)
    │
    ├─ [ULLONG_MAX - 1 - ctx->count < n && 非阻塞]
    │   └─ 返回 -EAGAIN
    │
    ├─ [ULLONG_MAX - 1 - ctx->count < n && 阻塞]
    │   └─ 等待队列睡眠，直到有空间
    │
    └─ ctx->count += n
        └─ wake_up_locked_poll(&ctx->wqh, EPOLLIN)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EINVAL` | 无效参数 | `flags` 包含除 `EFD_CLOEXEC`、`EFD_NONBLOCK`、`EFD_SEMAPHORE` 之外的标志 |
| `EMFILE` | 文件描述符过多 | 已达到进程级文件描述符限制 |
| `ENFILE` | 系统文件表满 | 系统级文件描述符耗尽 |
| `ENOMEM` | 内存不足 | 无法分配 `eventfd_ctx` |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

// 线程函数：等待事件通知
void *worker(void *arg)
{
    int efd = *(int *)arg;
    uint64_t val;
    ssize_t ret;

    printf("工作线程等待事件...\n");
    ret = read(efd, &val, sizeof(val));
    if (ret == sizeof(val)) {
        printf("工作线程收到事件通知，val=%lu\n", val);
    }
    return NULL;
}

int main(void)
{
    int efd;
    pthread_t thread;
    uint64_t val = 1;

    // 创建 eventfd（初始值 0）
    efd = eventfd2(0, EFD_NONBLOCK);
    if (efd == -1) {
        perror("eventfd2");
        exit(EXIT_FAILURE);
    }

    printf("eventfd 创建成功，fd=%d\n", efd);

    // 使用 EFD_SEMAPHORE 模式
    int efd_sem = eventfd2(0, EFD_SEMAPHORE);
    if (efd_sem == -1) {
        perror("eventfd2 (semaphore)");
        exit(EXIT_FAILURE);
    }

    // 写入事件通知
    val = 3;
    if (write(efd, &val, sizeof(val)) != sizeof(val)) {
        perror("write");
        exit(EXIT_FAILURE);
    }

    // 读取事件（非阻塞）
    val = 0;
    if (read(efd, &val, sizeof(val)) == sizeof(val)) {
        printf("读取到事件值: %lu（将被重置为 0）\n", val);
    }

    // 测试 EFD_SEMAPHORE 模式
    val = 3;
    write(efd_sem, &val, sizeof(val));
    for (int i = 0; i < 3; i++) {
        val = 0;
        read(efd_sem, &val, sizeof(val));
        printf("信号量模式 - 第 %d 次读取: %lu\n", i + 1, val);
    }

    close(efd);
    close(efd_sem);
    return 0;
}
```

**可能的输出：**

```
eventfd 创建成功，fd=3
读取到事件值: 3（将被重置为 0）
信号量模式 - 第 1 次读取: 1
信号量模式 - 第 2 次读取: 1
信号量模式 - 第 3 次读取: 1
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#事件通知-epoll)
- 内核源码：`fs/eventfd.c`
- 内核头文件：`include/linux/eventfd.h`