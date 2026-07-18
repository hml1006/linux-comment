# epoll_pwait 系统调用分析

## 1. 概述

`epoll_pwait` 用于等待 epoll 实例上的 I/O 事件，它允许在等待期间临时替换信号掩码。这是 `epoll_wait` 的增强版本，解决了信号处理中的竞态条件问题（即所谓的"signal race"）。该调用在等待 I/O 事件的同时可以原子性地设置信号掩码。

**原型：**

```c
#include <sys/epoll.h>

int epoll_pwait(int epfd, struct epoll_event *events,
                int maxevents, int timeout,
                const sigset_t *sigmask, size_t sigsetsize);
```

**内核入口：**

```c
// fs/eventpoll.c:2503
SYSCALL_DEFINE6(epoll_pwait, int, epfd, struct epoll_event __user *, events,
                int, maxevents, int, timeout, const sigset_t __user *, sigmask,
                size_t, sigsetsize)
```

## 2. 使用场景

- **信号安全的事件等待**：在等待 I/O 事件时，需要临时屏蔽某些信号以避免竞态
- **事件驱动服务器**：结合信号处理（如 SIGTERM、SIGINT）的 I/O 多路复用
- **线程安全**：在多线程环境中精确控制信号传递时机

## 3. 函数调用栈

```
epoll_pwait(epfd, events, maxevents, timeout, sigmask, sigsetsize)  // 系统调用入口
  │
  └─ do_epoll_pwait(epfd, events, maxevents,                        // 核心实现
  │                 ep_timeout_to_timespec(&to, timeout),
  │                 sigmask, sigsetsize)
  │    ├─ set_user_sigmask(sigmask, sigsetsize)      // 临时设置信号掩码
  │    │    └─ 保存当前信号掩码，设置新掩码
  │    │
  │    ├─ do_epoll_wait(epfd, events, maxevents, to)  // 基础 epoll_wait
  │    │    │
  │    │    └─ ep_poll(ep, events, maxevents, timeout)  // fs/eventpoll.c:1940
  │    │         │
  │    │         ├─ ep_events_available(ep)           // 快速检查就绪事件
  │    │         │
  │    │         └─ while (1) {                       // 等待循环
  │    │              │
  │    │              ├─ ep_try_send_events(ep, events, maxevents)  // 尝试发送事件
  │    │              │    └─ ep_send_events(ep, events, maxevents)
  │    │              │         └─ ep_scan_ready_list(ep, ep_send_events_proc)
  │    │              │              ├─ mutex_lock(&ep->mtx)
  │    │              │              ├─ list_splice_init(&ep->rdllist, &txlist)
  │    │              │              ├─ for each epi in txlist:
  │    │              │              │    ├─ ep_item_poll(epi, &pt, 1)  // 重新检查
  │    │              │              │    ├─ epoll_put_uevent(revents, data, events)
  │    │              │              │    └─ LT模式→插回rdllist
  │    │              │              └─ mutex_unlock(&ep->mtx)
  │    │              │
  │    │              ├─ if (timed_out) return 0       // 超时
  │    │              ├─ ep_busy_loop(ep)              // NAPI 忙等优化
  │    │              ├─ if (signal_pending(current)) return -EINTR  // 信号中断
  │    │              ├─ init_wait(&wait)              // 初始化等待队列条目
  │    │              ├─ __add_wait_queue_exclusive(&ep->wq, &wait)  // 加入等待队列
  │    │              ├─ set_current_state(TASK_INTERRUPTIBLE)       // 设置进程状态
  │    │              ├─ schedule_hrtimeout_range(to, slack, HRTIMER_MODE_ABS)  // 调度
  │    │              └─ } // end while
  │    │
  │    └─ restore_saved_sigmask_unless(error == -EINTR)  // 恢复信号掩码
  │
  └─ 返回事件数
```

## 4. 关键数据结构

### 4.1 struct eventpoll（epoll 实例核心结构）

```c
// fs/eventpoll.c
struct eventpoll {
    struct mutex mtx;                   // 主互斥锁
    wait_queue_head_t wq;               // epoll_wait 等待队列
    wait_queue_head_t poll_wait;        // poll 等待队列
    struct list_head rdllist;           // 就绪事件链表
    struct rb_root_cached rbr;          // 监控 fd 的红黑树
    struct epitem *ovflist;             // 溢出链表
    struct file *file;                  // epoll 文件
};
```

### 4.2 struct epoll_event（用户空间事件结构）

```c
// include/uapi/linux/eventpoll.h:83
struct epoll_event {
    __poll_t events;     // 事件掩码
    __u64 data;          // 用户数据
} EPOLL_PACKED;
```

## 5. 流程图

```
用户态调用 epoll_pwait(epfd, events, maxevents, timeout, sigmask, sigsetsize)
    │
    ▼
do_epoll_pwait()
    │
    ├─ set_user_sigmask(sigmask, sigsetsize)
    │   ├─ 保存当前信号掩码到 current->saved_sigmask
    │   └─ 设置新信号掩码
    │
    ├─ do_epoll_wait(epfd, events, maxevents, to)
    │   │
    │   └─ ep_poll(ep, events, maxevents, timeout)
    │       │
    │       ├─ 检查就绪事件
    │       │   ├─ 有就绪事件 → 立即发送并返回
    │       │   └─ 无就绪事件 → 进入等待循环
    │       │
    │       └─ 等待循环:
    │           │
    │           ├─ ep_send_events()  // 尝试发送事件
    │           │   └─ 扫描就绪链表，将事件复制到用户空间
    │           │
    │           ├─ 超时检查 → 返回 0
    │           ├─ 信号检查 → 返回 -EINTR
    │           │
    │           └─ 阻塞等待:
    │               ├─ 加入等待队列
    │               ├─ 设置 TASK_INTERRUPTIBLE
    │               ├─ schedule_hrtimeout_range()
    │               │   └─ 进程挂起，等待事件/超时/信号
    │               └─ 被唤醒后重新循环
    │
    └─ restore_saved_sigmask_unless(error == -EINTR)
        ├─ error == -EINTR → 不恢复（信号处理程序将恢复）
        └─ error != -EINTR → 恢复原始信号掩码
```

### 与 epoll_wait 的区别

```
epoll_wait(epfd, events, maxevents, timeout)
  └─ epoll_pwait(epfd, events, maxevents, timeout, NULL, 0)  // 等效

关键区别：
  - epoll_pwait 在等待期间临时替换信号掩码
  - 原子性操作：设置信号掩码 + 等待事件在一个系统调用中完成
  - 解决信号竞态：避免信号到达在 epoll_wait 之前但 sigwait 之后丢失
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EBADF` | 无效 fd | `epfd` 不是有效的文件描述符 |
| `EINVAL` | 无效参数 | `epfd` 不是 epoll 实例；`maxevents` <= 0；`sigmask` 大小无效 |
| `EFAULT` | 地址错误 | `events` 或 `sigmask` 指向无效的用户空间地址 |
| `EINTR` | 信号中断 | 等待期间被信号中断 |
| `ENOMEM` | 内存不足 | 无法分配内部数据结构 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define MAX_EVENTS 10

int main(void)
{
    int epfd, nfds;
    struct epoll_event ev, events[MAX_EVENTS];
    sigset_t sigmask;

    // 创建 epoll 实例
    epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    // 监控标准输入
    ev.events = EPOLLIN;
    ev.data.fd = STDIN_FILENO;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    // 设置信号掩码：在等待期间屏蔽 SIGINT
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGINT);

    printf("等待事件... (按 Ctrl+C 不会中断，输入内容后 Enter 触发)\n");

    // 使用 epoll_pwait 等待事件，同时屏蔽 SIGINT
    nfds = epoll_pwait(epfd, events, MAX_EVENTS, 5000, &sigmask, sizeof(sigmask));
    if (nfds == -1) {
        printf("epoll_pwait 错误: %s\n", strerror(errno));
    } else if (nfds == 0) {
        printf("5 秒超时，无事件\n");
    } else {
        for (int i = 0; i < nfds; i++) {
            printf("事件 %d: fd=%d, events=0x%x\n",
                   i, events[i].data.fd, events[i].events);
        }
    }

    close(epfd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#事件通知-epoll)
- 内核源码：`fs/eventpoll.c`
- 用户空间 API：`include/uapi/linux/eventpoll.h`