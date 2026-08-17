# epoll_create1 系统调用分析

## 1. 概述

`epoll_create1` 用于创建一个新的 epoll 实例，返回一个文件描述符。该实例可用于监控多个文件描述符的 I/O 事件。它是 `epoll_create` 的增强版本，支持通过 `flags` 参数设置 `EPOLL_CLOEXEC` 标志。

**原型：**

```c
#include <sys/epoll.h>

int epoll_create1(int flags);
```

**内核入口：**

```c
// fs/eventpoll.c:2202
SYSCALL_DEFINE1(epoll_create1, int, flags)
{
    return do_epoll_create(flags);
}
```

## 2. 使用场景

- **高性能 I/O 事件监控**：创建 epoll 实例以监控大量文件描述符的 I/O 事件
- **网络服务器**：作为事件驱动网络服务器的基础（如 Nginx、Node.js）
- **避免 fd 泄漏**：使用 `EPOLL_CLOEXEC` 标志确保 exec 时自动关闭

## 3. 函数调用栈

```
epoll_create1(flags)                                    // 系统调用入口
  └─ do_epoll_create(flags)                             // fs/eventpoll.c:2171
       ├─ BUILD_BUG_ON(EPOLL_CLOEXEC != O_CLOEXEC)      // 编译时检查
       ├─ flags & ~EPOLL_CLOEXEC → 返回 -EINVAL         // 无效标志
       │
       ├─ ep_alloc(&ep)                                 // 分配 struct eventpoll
       │    ├─ kzalloc(sizeof(*ep), GFP_KERNEL)          // 分配内存
       │    ├─ mutex_init(&ep->mtx)                     // 主互斥锁
       │    ├─ init_waitqueue_head(&ep->wq)             // 等待队列（epoll_wait 阻塞）
       │    ├─ init_waitqueue_head(&ep->poll_wait)      // poll 等待队列
       │    ├─ INIT_LIST_HEAD(&ep->rdllist)             // 就绪链表
       │    ├─ ep->rbr = RB_ROOT                        // 红黑树根
       │    └─ ep->ovflist = EP_UNACTIVE_PTR            // 溢出链表
       │
       ├─ anon_inode_getfile("[eventpoll]", &eventpoll_fops, ep, ...)  // 创建匿名文件
       │    └─ 失败 → ep_clear_and_put(ep); return error
       │
       └─ fd_publish(fdf)                               // 分配并发布 fd
            └─ 返回 fd
```

**核心实现源码：**

```c
// fs/eventpoll.c:2171
static int do_epoll_create(int flags)
{
    int error;
    struct eventpoll *ep;

    /* Check the EPOLL_* constant for consistency.  */
    BUILD_BUG_ON(EPOLL_CLOEXEC != O_CLOEXEC);

    if (flags & ~EPOLL_CLOEXEC)
        return -EINVAL;

    error = ep_alloc(&ep);
    if (error < 0)
        return error;

    FD_PREPARE(fdf, O_RDWR | (flags & O_CLOEXEC),
               anon_inode_getfile("[eventpoll]", &eventpoll_fops, ep,
                                  O_RDWR | (flags & O_CLOEXEC)));
    if (fdf.err) {
        ep_clear_and_put(ep);
        return fdf.err;
    }
    ep->file = fd_prepare_file(fdf);
    return fd_publish(fdf);
}
```

## 4. 关键数据结构

### 4.1 struct eventpoll（epoll 实例核心结构）

```c
// fs/eventpoll.c
struct eventpoll {
    struct mutex mtx;                   // 主互斥锁（保护 epoll 数据结构）
    wait_queue_head_t wq;               // epoll_wait 等待队列
    wait_queue_head_t poll_wait;        // poll 等待队列
    struct list_head rdllist;           // 就绪事件链表
    struct rb_root_cached rbr;          // 所有监控 fd 的红黑树（缓存版本）
    struct epitem *ovflist;             // 溢出链表（中断上下文使用）
    struct file *file;                  // epoll 文件指针
};
```

### 4.2 struct epitem（epoll 监控项）

```c
// fs/eventpoll.c
struct epitem {
    struct rb_node rbn;                 // 红黑树节点
    struct list_head rdllink;           // 就绪链表节点
    struct epoll_filefd ffd;            // 被监控的文件描述符
    struct eventpoll *ep;               // 所属 epoll 实例
    struct list_head fllink;            // 文件关联链表
    struct wait_queue_entry wait;       // 等待队列条目（回调上下文）
    struct epoll_event event;           // 注册的事件
};
```

### 4.3 struct epoll_event（用户空间事件结构）

```c
// include/uapi/linux/eventpoll.h:83
struct epoll_event {
    __poll_t events;     // 事件掩码（EPOLLIN, EPOLLOUT, EPOLLERR 等）
    __u64 data;          // 用户数据
} EPOLL_PACKED;
```

## 5. 流程图

```
用户态调用 epoll_create1(flags)
    │
    ▼
SYSCALL_DEFINE1(epoll_create1)
    │
    └─ do_epoll_create(flags)
        │
        ├─ 验证 flags
        │   └─ flags 包含非 EPOLL_CLOEXEC 位 → 返回 -EINVAL
        │
        ├─ ep_alloc(&ep)
        │   ├─ kzalloc 分配 struct eventpoll
        │   ├─ 初始化互斥锁 ep->mtx
        │   ├─ 初始化等待队列 ep->wq, ep->poll_wait
        │   ├─ 初始化就绪链表 ep->rdllist
        │   ├─ 初始化红黑树 ep->rbr
        │   └─ 设置 ep->ovflist = EP_UNACTIVE_PTR
        │
        ├─ anon_inode_getfile("[eventpoll]", ...)
        │   ├─ 创建匿名 inode 和 file 结构
        │   ├─ 关联 eventpoll_fops 操作函数集
        │   └─ 设置 file->private_data = ep
        │
        └─ fd_publish(fdf)
            ├─ 分配空闲 fd
            ├─ 安装 fd 到当前进程的 fdtable
            └─ 返回 fd
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EINVAL` | 无效参数 | `flags` 包含除 `EPOLL_CLOEXEC` 之外的标志 |
| `EMFILE` | 文件描述符过多 | 已达到进程级文件描述符限制 |
| `ENFILE` | 系统文件表满 | 系统级文件描述符耗尽 |
| `ENOMEM` | 内存不足 | 无法分配内核内存 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

int main(void)
{
    int epfd;

    // 创建 epoll 实例，设置 close-on-exec 标志
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    printf("epoll 实例创建成功，fd=%d\n", epfd);

    // 获取 epoll 实例的一些信息
    int flags = fcntl(epfd, F_GETFD);
    if (flags != -1) {
        printf("FD_CLOEXEC 已设置: %s\n",
               (flags & FD_CLOEXEC) ? "是" : "否");
    }

    close(epfd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#事件通知-epoll)
- 内核源码：`fs/eventpoll.c`
- 用户空间 API：`include/uapi/linux/eventpoll.h`