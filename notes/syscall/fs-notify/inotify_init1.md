# inotify_init1 系统调用分析

## 1. 概述

`inotify_init1` 用于创建一个新的 inotify 实例，返回一个文件描述符。该实例可用于监控文件系统事件。它是 `inotify_init` 的增强版本，支持通过 `flags` 参数设置 `IN_CLOEXEC` 和 `IN_NONBLOCK` 标志。

**原型：**

```c
#include <sys/inotify.h>

int inotify_init1(int flags);
```

**内核入口：**

```c
// fs/notify/inotify/inotify_user.c:719
SYSCALL_DEFINE1(inotify_init1, int, flags)
{
    return do_inotify_init(flags);
}
```

## 2. 使用场景

- **文件系统事件监控**：创建 inotify 实例以监控文件创建、删除、修改等事件
- **配置热加载**：结合 `epoll` 实现配置文件变更的自动检测
- **文件管理器**：实时监控文件系统变化

## 3. 函数调用栈

```
inotify_init1(flags)                                      // 系统调用入口
  │
  └─ do_inotify_init(flags)                               // fs/notify/inotify/inotify_user.c:694
       │
       ├─ BUILD_BUG_ON(IN_CLOEXEC != O_CLOEXEC)           // 编译时常量检查
       ├─ BUILD_BUG_ON(IN_NONBLOCK != O_NONBLOCK)
       │
       ├─ flags & ~(IN_CLOEXEC | IN_NONBLOCK) → 返回 -EINVAL  // 无效标志
       │
       ├─ inotify_new_group(inotify_max_queued_events)    // 创建新 inotify 组
       │   ├─ fsnotify_alloc_group(&inotify_fsnotify_ops)  // 分配 fsnotify_group
       │   │   └─ kzalloc(sizeof(struct fsnotify_group), ...)
       │   ├─ 初始化事件队列
       │   ├─ 设置最大事件数
       │   └─ 返回 group 指针
       │
       ├─ anon_inode_getfd("inotify", &inotify_fops, group, O_RDONLY | flags)
       │   ├─ 创建匿名文件
       │   ├─ 关联 inotify_fops 操作函数集
       │   └─ 设置 file->private_data = group
       │
       └─ 返回 fd
```

**核心实现源码：**

```c
// fs/notify/inotify/inotify_user.c:694
static int do_inotify_init(int flags)
{
    struct fsnotify_group *group;
    int ret;

    /* Check the IN_* constants for consistency.  */
    BUILD_BUG_ON(IN_CLOEXEC != O_CLOEXEC);
    BUILD_BUG_ON(IN_NONBLOCK != O_NONBLOCK);

    if (flags & ~(IN_CLOEXEC | IN_NONBLOCK))
        return -EINVAL;

    /* fsnotify_obtain_group took a reference to group, we put this
     * when we kill the file in the end */
    group = inotify_new_group(inotify_max_queued_events);
    if (IS_ERR(group))
        return PTR_ERR(group);

    ret = anon_inode_getfd("inotify", &inotify_fops, group,
                              O_RDONLY | flags);
    if (ret < 0)
        fsnotify_destroy_group(group);

    return ret;
}
```

## 4. 关键数据结构

### 4.1 struct fsnotify_group（通知组）

```c
// include/linux/fsnotify_backend.h
struct fsnotify_group {
    const struct fsnotify_ops *ops;           // 操作函数集
    struct fsnotify_event_queue event_list;   // 事件队列
    spinlock_t notification_lock;             // 通知锁
    wait_queue_head_t notification_waitq;     // 等待队列（read/poll 阻塞）
    struct list_head listener_list;           // 监听器链表
    atomic_t user_waits;                      // 用户等待计数
    unsigned int max_events;                  // 最大事件数
    struct mem_cgroup *memcg;                 // 内存 cgroup
    struct fwnode_handle *fanotify_data;      // fanotify 私有数据
    struct inotify_group_private_data *inotify_data; // inotify 私有数据
};
```

### 4.2 inotify 文件操作函数集

```c
// fs/notify/inotify/inotify_user.c
static const struct file_operations inotify_fops = {
    .show_fdinfo    = inotify_show_fdinfo,
    .poll           = inotify_poll,
    .read           = inotify_read,
    .release        = inotify_release,
    .unlocked_ioctl = inotify_ioctl,
    .compat_ioctl   = inotify_ioctl,
    .llseek         = noop_llseek,
};
```

## 5. 流程图

```
用户态调用 inotify_init1(flags)
    │
    ▼
SYSCALL_DEFINE1(inotify_init1)
    │
    └─ do_inotify_init(flags)
        │
        ├─ 验证 flags
        │   └─ 无效标志 → -EINVAL
        │
        ├─ inotify_new_group(max_queued_events)
        │   │
        │   ├─ fsnotify_alloc_group(&inotify_fsnotify_ops)
        │   │   ├─ kzalloc(sizeof(struct fsnotify_group))
        │   │   ├─ 初始化 spinlock
        │   │   ├─ 初始化等待队列
        │   │   ├─ 初始化事件队列
        │   │   ├─ 设置 max_events
        │   │   └─ 设置 ops = &inotify_fsnotify_ops
        │   │
        │   └─ 返回 group
        │
        ├─ anon_inode_getfd("inotify", &inotify_fops, group, O_RDONLY | flags)
        │   ├─ get_unused_fd_flags(flags)
        │   ├─ anon_inode_getfile("inotify", &inotify_fops, group, ...)
        │   │   ├─ 创建匿名 inode
        │   │   └─ 创建 file 结构，private_data = group
        │   └─ fd_install(fd, file)
        │
        └─ 返回 fd
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EINVAL` | 无效参数 | `flags` 包含除 `IN_CLOEXEC` 和 `IN_NONBLOCK` 之外的标志 |
| `EMFILE` | 文件描述符过多 | 已达到进程级文件描述符限制 |
| `ENFILE` | 系统文件表满 | 系统级文件描述符耗尽 |
| `ENOMEM` | 内存不足 | 无法分配 `fsnotify_group` 结构 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>

#define MAX_EVENTS 1024

int main(void)
{
    int inotify_fd;

    // 创建 inotify 实例（非阻塞模式）
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd == -1) {
        perror("inotify_init1");
        exit(EXIT_FAILURE);
    }

    printf("inotify 实例创建成功，fd=%d\n", inotify_fd);

    // 添加监控项
    int wd = inotify_add_watch(inotify_fd, "/tmp",
                                IN_CREATE | IN_DELETE | IN_MODIFY);
    if (wd == -1) {
        perror("inotify_add_watch");
        exit(EXIT_FAILURE);
    }

    // 可以结合 epoll 使用
    int epfd = epoll_create1(0);
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = inotify_fd,
    };
    epoll_ctl(epfd, EPOLL_CTL_ADD, inotify_fd, &ev);

    printf("等待事件...\n");

    // 读取事件
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *event;
    ssize_t len;

    // 非阻塞读取
    len = read(inotify_fd, buf, sizeof(buf));
    if (len == -1 && errno == EAGAIN) {
        printf("当前无事件（非阻塞模式）\n");
    } else if (len > 0) {
        for (char *ptr = buf; ptr < buf + len;
             ptr += sizeof(struct inotify_event) + event->len) {
            event = (const struct inotify_event *)ptr;
            printf("wd=%d mask=0x%x cookie=%u len=%u name=%s\n",
                   event->wd, event->mask, event->cookie,
                   event->len, event->name);
        }
    }

    close(inotify_fd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件与目录事件监控)
- 内核源码：`fs/notify/inotify/inotify_user.c`
- 内核头文件：`include/linux/fsnotify_backend.h`
- 用户空间 API：`include/uapi/linux/inotify.h`