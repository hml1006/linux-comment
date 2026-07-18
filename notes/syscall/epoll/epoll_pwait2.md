# epoll_pwait2 系统调用分析

## 1. 概述

`epoll_pwait2` 是 `epoll_pwait` 的增强版本，主要区别在于超时参数使用 `struct timespec`（纳秒精度）而非 `int`（毫秒精度）。它允许在等待 I/O 事件的同时原子性地设置信号掩码，并提供纳秒级的超时精度。

**原型：**

```c
#include <sys/epoll.h>

int epoll_pwait2(int epfd, struct epoll_event *events,
                 int maxevents, const struct timespec *timeout,
                 const sigset_t *sigmask, size_t sigsetsize);
```

**内核入口：**

```c
// fs/eventpoll.c:2514
SYSCALL_DEFINE6(epoll_pwait2, int, epfd, struct epoll_event __user *, events,
                int, maxevents, const struct __kernel_timespec __user *, timeout,
                const sigset_t __user *, sigmask, size_t, sigsetsize)
```

## 2. 使用场景

- **高精度超时等待**：需要纳秒级超时精度的 I/O 事件等待
- **信号安全的事件等待**：在等待 I/O 事件时临时替换信号掩码
- **低延迟系统**：金融交易系统、实时音频/视频处理等需要精确时间控制的场景
- **替代 epoll_pwait**：当需要更精确的超时控制时使用

## 3. 函数调用栈

```
epoll_pwait2(epfd, events, maxevents, timeout, sigmask, sigsetsize)  // 系统调用入口
  │
  ├─ [timeout != NULL]
  │   ├─ get_timespec64(&ts, timeout)                    // 从用户空间读取超时时间
  │   │   └─ 失败 → 返回 -EFAULT
  │   ├─ to = &ts
  │   └─ poll_select_set_timeout(to, ts.tv_sec, ts.tv_nsec)  // 验证超时值
  │       └─ 无效 → 返回 -EINVAL
  │
  └─ do_epoll_pwait(epfd, events, maxevents, to,         // 核心实现
  │                 sigmask, sigsetsize)
       │
       ├─ set_user_sigmask(sigmask, sigsetsize)          // 临时设置信号掩码
       │
       ├─ do_epoll_wait(epfd, events, maxevents, to)     // 基础 epoll_wait
       │    │
       │    └─ ep_poll(ep, events, maxevents, to)        // fs/eventpoll.c:1940
       │         ├─ ep_events_available(ep)               // 快速检查
       │         └─ while (1) {
       │              ├─ ep_send_events()                 // 发送事件到用户
       │              ├─ if (timed_out) return 0
       │              ├─ if (signal_pending) return -EINTR
       │              ├─ 加入等待队列
       │              ├─ set_current_state(TASK_INTERRUPTIBLE)
       │              └─ schedule_hrtimeout_range(to, ...)  // hrtimer 调度
       │         }
       │
       └─ restore_saved_sigmask_unless(error == -EINTR)  // 恢复信号掩码
```

**核心实现源码：**

```c
// fs/eventpoll.c:2514
SYSCALL_DEFINE6(epoll_pwait2, int, epfd, struct epoll_event __user *, events,
                int, maxevents, const struct __kernel_timespec __user *, timeout,
                const sigset_t __user *, sigmask, size_t, sigsetsize)
{
    struct timespec64 ts, *to = NULL;

    if (timeout) {
        if (get_timespec64(&ts, timeout))
            return -EFAULT;
        to = &ts;
        if (poll_select_set_timeout(to, ts.tv_sec, ts.tv_nsec))
            return -EINVAL;
    }

    return do_epoll_pwait(epfd, events, maxevents, to,
                          sigmask, sigsetsize);
}
```

## 4. 关键数据结构

### 4.1 struct __kernel_timespec（用户空间时间结构）

```c
// include/uapi/linux/time_types.h:7
struct __kernel_timespec {
    __kernel_time64_t tv_sec;   /* 秒 */
    long long tv_nsec;          /* 纳秒 */
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
用户态调用 epoll_pwait2(epfd, events, maxevents, &timeout, sigmask, sigsetsize)
    │
    ▼
SYSCALL_DEFINE6(epoll_pwait2)
    │
    ├─ timeout 参数处理
    │   ├─ timeout == NULL → 无限等待 (to = NULL)
    │   ├─ get_timespec64(&ts, timeout) → 失败 → -EFAULT
    │   ├─ poll_select_set_timeout(to, ts.tv_sec, ts.tv_nsec)
    │   │   ├─ tv_sec < 0 → -EINVAL
    │   │   ├─ tv_nsec < 0 || tv_nsec >= NSEC_PER_SEC → -EINVAL
    │   │   └─ 转换为 jiffies 或 hrtimer 时间
    │   └─ to = &ts
    │
    ▼
do_epoll_pwait(epfd, events, maxevents, to, sigmask, sigsetsize)
    │
    ├─ set_user_sigmask()  // 原子性设置信号掩码
    │
    ├─ do_epoll_wait()     // 等待事件（与 epoll_wait 相同）
    │   └─ ep_poll()       // 使用 to 作为超时
    │
    └─ restore_saved_sigmask_unless()  // 恢复信号掩码
```

### 与 epoll_pwait 的区别

| 特性 | epoll_pwait | epoll_pwait2 |
|--|--|--|
| 超时类型 | `int timeout`（毫秒） | `const struct timespec *timeout`（秒+纳秒） |
| 精度 | 毫秒级 | 纳秒级 |
| 无限等待 | `timeout = -1` | `timeout = NULL` |
| 立即返回 | `timeout = 0` | `timeout = &(struct timespec){0, 0}` |
| 内核版本 | Linux 2.6.19+ | Linux 5.11+ |

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EBADF` | 无效 fd | `epfd` 不是有效的文件描述符 |
| `EINVAL` | 无效参数 | `epfd` 不是 epoll 实例；`maxevents` <= 0；`timeout` 中的值无效（负秒或纳秒超出范围） |
| `EFAULT` | 地址错误 | `events`、`timeout` 或 `sigmask` 指向无效的用户空间地址 |
| `EINTR` | 信号中断 | 等待期间被信号中断 |
| `ENOMEM` | 内存不足 | 无法分配内部数据结构 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define MAX_EVENTS 10

int main(void)
{
    int epfd, nfds;
    struct epoll_event ev, events[MAX_EVENTS];
    struct timespec timeout;
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

    // 设置 100 毫秒超时（纳秒精度）
    timeout.tv_sec = 0;
    timeout.tv_nsec = 100000000;  // 100ms

    // 空信号掩码
    sigemptyset(&sigmask);

    printf("等待事件，100ms 超时（纳秒精度）...\n");

    // 使用 epoll_pwait2 等待
    nfds = epoll_pwait2(epfd, events, MAX_EVENTS, &timeout, &sigmask,
                        sizeof(sigmask));
    if (nfds == -1) {
        printf("epoll_pwait2 错误: %s\n", strerror(errno));
    } else if (nfds == 0) {
        printf("100ms 超时，无事件\n");
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
- 时间结构：`include/uapi/linux/time_types.h`