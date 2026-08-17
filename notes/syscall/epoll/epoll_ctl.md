# epoll_ctl 系统调用分析

## 1. 概述

`epoll_ctl` 用于控制 epoll 实例的事件监控操作。它允许向 epoll 实例添加、修改或删除对特定文件描述符的监控。这是 epoll 机制的核心控制接口，支持三种操作：`EPOLL_CTL_ADD`、`EPOLL_CTL_MOD` 和 `EPOLL_CTL_DEL`。

**原型：**

```c
#include <sys/epoll.h>

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
```

**内核入口：**

```c
// fs/eventpoll.c:2387
SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd,
                struct epoll_event __user *, event)
{
    struct epoll_event epds;

    if (ep_op_has_event(op) &&
        copy_from_user(&epds, event, sizeof(struct epoll_event)))
        return -EFAULT;

    return do_epoll_ctl(epfd, op, fd, &epds, false);
}
```

## 2. 使用场景

- **添加监控**：`EPOLL_CTL_ADD` — 将文件描述符添加到 epoll 监控
- **修改监控**：`EPOLL_CTL_MOD` — 修改已存在的监控事件类型
- **删除监控**：`EPOLL_CTL_DEL` — 从 epoll 实例中移除文件描述符
- **网络服务器**：管理大量客户端连接的事件监控

## 3. 函数调用栈

```
epoll_ctl(epfd, op, fd, event)                          // 系统调用入口
  │
  ├─ [op 需要 event 参数] copy_from_user(&epds, event, ...)
  │   └─ 失败 → 返回 -EFAULT
  │
  └─ do_epoll_ctl(epfd, op, fd, &epds, false)           // 核心实现
       │
       ├─ 获取 epoll 实例文件 epf
       │   ├─ epf.f_op != &eventpoll_fops → 返回 -EINVAL
       │   └─ ep = epf->private_data
       │
       ├─ 获取目标文件 tf
       │   ├─ epfd == fd → 返回 -EINVAL（不能监控自身）
       │   └─ tf->f_op == &eventpoll_fops → 检查嵌套深度
       │
       ├─ [EPOLL_CTL_ADD] ep_insert(ep, event, tf, fd, full_check)
       │   ├─ kmem_cache_alloc(epi_cache, GFP_KERNEL)   // 分配 epitem
       │   ├─ ep_rbtree_insert(ep, epi)                 // 红黑树插入
       │   ├─ init_waitqueue_func_entry(&epi->wait, ep_poll_callback)
       │   │   └─ 注册回调函数，事件发生时调用
       │   ├─ epi->next = EP_UNACTIVE_PTR
       │   ├─ vfs_poll(tf, &epi->pt)                    // 调用 f_op->poll 收集初始事件
       │   │   └─ ep_item_poll(epi, pt, flags)
       │   │        └─ 添加等待队列 + 检查就绪状态
       │   └─ wake_up(&ep->wq)                          // 唤醒等待的 epoll_wait
       │
       ├─ [EPOLL_CTL_DEL] ep_remove(ep, epi)
       │   ├─ ep_unregister_pollwait(ep, epi)           // 移除 poll 回调
       │   ├─ ep_rbtree_erase(ep, epi)                  // 红黑树删除
       │   └─ kmem_cache_free(epi_cache, epi)           // 释放 epitem
       │
       └─ [EPOLL_CTL_MOD] ep_modify(ep, epi, &epds)     // 修改事件
            ├─ ep_unregister_pollwait(ep, epi)           // 移除旧的 poll 回调
            ├─ epi->event = *event                       // 更新事件掩码
            ├─ init_waitqueue_func_entry(&epi->wait, ep_poll_callback)
            ├─ vfs_poll(tf, &epi->pt)                    // 重新 poll 收集状态
            └─ [就绪状态变化] 加入就绪链表
```

## 4. 关键数据结构

### 4.1 struct epitem（epoll 监控项）

```c
// fs/eventpoll.c
struct epitem {
    struct rb_node rbn;                 // 红黑树节点（用于快速查找）
    struct list_head rdllink;           // 就绪链表节点
    struct epoll_filefd ffd;            // 被监控的文件描述符
    struct eventpoll *ep;               // 所属 epoll 实例
    struct list_head fllink;            // 文件关联链表（同一文件被多个 epoll 监控）
    struct wait_queue_entry wait;       // 等待队列条目（回调上下文）
    struct epoll_event event;           // 注册的事件（用户指定的事件掩码和数据）
};
```

### 4.2 struct epoll_event（用户空间事件结构）

```c
// include/uapi/linux/eventpoll.h:83
struct epoll_event {
    __poll_t events;     // 事件掩码（EPOLLIN, EPOLLOUT, EPOLLERR, EPOLLET 等）
    __u64 data;          // 用户数据（通常为 fd 或指针）
} EPOLL_PACKED;
```

### 4.3 ep_poll_callback（事件回调函数）

```c
// fs/eventpoll.c
static int ep_poll_callback(wait_queue_entry_t *wait, unsigned mode,
                            int sync, void *key)
{
    struct epitem *epi = ep_item_from_wait(wait);
    struct eventpoll *ep = epi->ep;
    int ewake = 0;

    // 1. 检查事件是否匹配
    // 2. 将 epitem 加入就绪链表
    // 3. 检查是否在溢出链表管理中
    // 4. 唤醒等待在 epoll_wait 上的进程
    // 5. 检查是否触发 poll 唤醒
    return ewake;
}
```

## 5. 流程图

```
用户态调用 epoll_ctl(epfd, op, fd, &event)
    │
    ▼
SYSCALL_DEFINE4(epoll_ctl)
    │
    ├─ copy_from_user(&epds, event)  ── 失败 → -EFAULT
    │
    ▼
do_epoll_ctl(epfd, op, fd, &epds, false)
    │
    ├─ 验证 epfd 是否为 epoll 实例
    │   └─ 不是 → -EINVAL
    │
    ├─ 验证目标 fd 是否有效
    │   ├─ epfd == fd → -EINVAL
    │   └─ 嵌套 epoll 检查深度
    │
    ├─ op 操作分发
    │   │
    │   ├─ EPOLL_CTL_ADD ──────────────────────────────────────
    │   │   ├─ 检查是否已存在 → 已存在 → -EEXIST
    │   │   ├─ ep_insert()
    │   │   │   ├─ 分配 epitem
    │   │   │   ├─ 初始化回调: ep_poll_callback
    │   │   │   ├─ 红黑树插入
    │   │   │   ├─ 调用 vfs_poll() 收集初始事件
    │   │   │   │   ├─ sock_poll / tcp_poll → 返回当前状态
    │   │   │   │   └─ ep_item_poll → 添加等待队列
    │   │   │   ├─ 初始事件加入就绪链表
    │   │   │   └─ wake_up(&ep->wq) 唤醒 epoll_wait
    │   │   └─ 返回 0
    │   │
    │   ├─ EPOLL_CTL_DEL ──────────────────────────────────────
    │   │   ├─ 检查是否不存在 → 不存在 → -ENOENT
    │   │   ├─ ep_remove()
    │   │   │   ├─ ep_unregister_pollwait 移除回调
    │   │   │   ├─ 从就绪链表移除
    │   │   │   ├─ 红黑树删除
    │   │   │   └─ 释放 epitem
    │   │   └─ 返回 0
    │   │
    │   └─ EPOLL_CTL_MOD ──────────────────────────────────────
    │       ├─ 检查是否不存在 → 不存在 → -ENOENT
    │       ├─ ep_modify()
    │       │   ├─ 更新事件掩码
    │       │   ├─ 重新注册回调
    │       │   ├─ 重新调用 vfs_poll() 检查状态
    │       │   └─ 状态变化 → 加入就绪链表
    │       └─ 返回 0
    │
    └─ 返回错误码
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EBADF` | 无效 fd | `epfd` 或 `fd` 不是有效的文件描述符 |
| `EINVAL` | 无效参数 | `epfd` 不是 epoll 实例；`op` 无效；`epfd` 等于 `fd`；嵌套过深 |
| `EEXIST` | 已存在 | `EPOLL_CTL_ADD` 但 `fd` 已在该 epoll 中监控 |
| `ENOENT` | 不存在 | `EPOLL_CTL_MOD` 或 `EPOLL_CTL_DEL` 但 `fd` 未在该 epoll 中监控 |
| `ENOMEM` | 内存不足 | 无法分配 `epitem` 结构 |
| `ENOSPC` | 空间不足 | `/proc/sys/fs/epoll/max_user_watches` 限制达到 |
| `EFAULT` | 地址错误 | `event` 指向无效的用户空间地址 |
| `EPERM` | 权限不足 | 目标文件不支持 epoll（如 `/proc/` 下的文件） |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    int epfd, fd;
    struct epoll_event ev;
    struct epoll_event rev;

    // 创建 epoll 实例
    epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    // 打开一个文件用于监控
    fd = open("/dev/null", O_RDONLY);
    if (fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    // 添加监控：监控可读事件（边缘触发模式）
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        perror("epoll_ctl ADD");
        exit(EXIT_FAILURE);
    }
    printf("已添加 fd=%d 到 epoll 监控\n", fd);

    // 修改监控：改为水平触发模式
    ev.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        perror("epoll_ctl MOD");
        exit(EXIT_FAILURE);
    }
    printf("已修改 fd=%d 的事件类型\n", fd);

    // 删除监控
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        perror("epoll_ctl DEL");
        exit(EXIT_FAILURE);
    }
    printf("已从 epoll 移除 fd=%d\n", fd);

    close(fd);
    close(epfd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#事件通知-epoll)
- 内核源码：`fs/eventpoll.c`
- 用户空间 API：`include/uapi/linux/eventpoll.h`