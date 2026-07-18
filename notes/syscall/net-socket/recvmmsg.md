# recvmmsg 系统调用分析

## 1. 概述

`recvmmsg` 系统调用在一次调用中批量接收多个消息，减少了系统调用次数，从而提高性能。它是 `recvmsg` 的批量版本。

**原型：**

```c
#include <sys/socket.h>

int recvmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
             int flags, struct timespec *timeout);
```

**参数：**
- `sockfd`：套接字文件描述符
- `msgvec`：指向 `struct mmsghdr` 数组的指针
- `vlen`：`msgvec` 数组的大小（最大 `UIO_MAXIOV`）
- `flags`：消息标志
- `timeout`：超时时间（可为 NULL，表示无限等待）

**返回值：**
- 成功：返回接收到的消息数量
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:3039
SYSCALL_DEFINE5(recvmmsg, int, fd, struct mmsghdr __user *, mmsg,
        unsigned int, vlen, unsigned int, flags,
        struct __kernel_timespec __user *, timeout)
{
    if (flags & MSG_CMSG_COMPAT)
        return -EINVAL;
    return __sys_recvmmsg(fd, mmsg, vlen, flags, timeout, NULL);
}
```

## 3. 详细的函数调用链

```
recvmmsg (系统调用入口)
└── __sys_recvmmsg(fd, mmsg, vlen, flags, timeout, NULL)  [net/socket.c:3008]
    ├── if (timeout) get_timespec64(&timeout_sys, timeout)  → 复制超时参数
    │
    ├── if (!timeout)
    │   └── return do_recvmmsg(fd, mmsg, vlen, flags, NULL)
    │
    ├── datagrams = do_recvmmsg(fd, mmsg, vlen, flags, &timeout_sys)
    │
    └── if (datagrams <= 0) return datagrams
    └── put_timespec64(&timeout_sys, timeout)  → 写回剩余超时时间
    └── return datagrams
    │
    └── do_recvmmsg(fd, mmsg, vlen, flags, timeout)  [net/socket.c:2900]
        ├── CLASS(fd, f)(fd)  → 获取 struct file
        ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
        ├── if (!sock) return -ENOTSOCK
        │
        ├── if (timeout)  → 初始化超时
        │   └── end_time = timespec64_add(*timeout, ktime_get_ts64())
        │
        ├── datagrams = 0
        │
        ├── while (datagrams < vlen):
        │   ├── if (MSG_CMSG_COMPAT & flags):
        │   │   ____sys_recvmsg(sock, &msg_sys, (struct user_msghdr *)compat_entry, ...)
        │   │   put_user(err, &compat_entry->msg_len)
        │   │   compat_entry++
        │   │
        │   ├── else:
        │   │   ____sys_recvmsg(sock, &msg_sys, (struct user_msghdr *)entry, ...)
        │   │   put_user(err, &entry->msg_len)
        │   │   entry++
        │   │
        │   ├── if (err < 0):
        │   │   ├── if (datagrams) break  → 已收到至少一个消息，返回已收数量
        │   │   └── else return err  → 第一个消息就失败
        │   │
        │   ├── datagrams++
        │   │
        │   ├── 检查 timeout 是否到期
        │   │   └── if (timeout && ktime_get_ts64() >= end_time) break
        │   │
        │   └── cond_resched()  → 内核抢占点
        │
        └── return datagrams
```

## 4. 关键数据结构

### struct mmsghdr — 批量消息头

```c
// include/uapi/linux/socket.h
struct mmsghdr {
    struct msghdr msg_hdr;   // 消息头（同 recvmsg 的 msghdr）
    unsigned int  msg_len;   // 接收到的字节数（内核写回）
};
```

## 5. 流程图

```
用户态: recvmmsg(fd, msgvec, vlen, flags, timeout)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE5(recvmmsg)          │  net/socket.c:3039
   │  ├─ 检查 MSG_CMSG_COMPAT            │
   │  └─ __sys_recvmmsg()                │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_recvmmsg()                   │  net/socket.c:3008
   │  ├─ 复制 timeout                    │
   │  └─ do_recvmmsg()                   │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  do_recvmmsg()                      │  net/socket.c:2900
   │  ├─ sock_from_file() → 获取 socket  │
   │  │                                  │
   │  ├─ while (datagrams < vlen):       │
   │  │   └─ ____sys_recvmsg()           │  ← 循环调用
   │  │      └─ → tcp_recvmsg/udp_recvmsg│
   │  │   └─ datagrams++                 │
   │  │   └─ cond_resched()              │
   │  │                                  │
   │  └─ return datagrams               │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 接收到的消息数量             │
   └─────────────────────────────────────┘
```

## 6. recvmmsg 的性能优势

| 对比 | 系统调用次数 | 内核入口/出口次数 |
|------|-------------|------------------|
| 接收 N 个消息 (recvmsg × N) | N 次 | N 次 |
| 接收 N 个消息 (recvmmsg × 1) | 1 次 | 1 次 |

**核心优化：**
- `CLASS(fd, f)` 只调用一次，避免重复查找 fd 表
- 使用 `sock_from_file` 只获取一次 socket
- 循环中调用 `cond_resched()` 避免长时间占用 CPU

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EINVAL` | 无效参数 | `flags` 包含 `MSG_CMSG_COMPAT` 或 `vlen` 无效 |
| `EAGAIN` | 资源暂时不可用 | 非阻塞模式且无数据可读 |
| `EFAULT` | 地址指针无效 | `msgvec` 或 `timeout` 指向不可访问的区域 |
| `EINTR` | 被信号中断 | 阻塞等待时收到信号 |

**注意：** 如果至少收到一个消息后发生错误，`recvmmsg` 返回已收到的消息数量（而非错误码）。只有第一个消息就失败时，才返回负的错误码。

## 8. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>

int main() {
    int sockfd;
    struct sockaddr_in addr;
    struct mmsghdr msgs[4];
    struct iovec iov[4];
    char bufs[4][1024];
    struct timespec timeout = { .tv_sec = 1, .tv_nsec = 0 };  // 1秒超时

    // 创建 UDP 套接字
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8888);
    bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));

    // 初始化 4 个消息结构
    for (int i = 0; i < 4; i++) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len = sizeof(bufs[i]);
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    // 批量接收消息（最多 4 个，1 秒超时）
    int ret = recvmmsg(sockfd, msgs, 4, 0, &timeout);
    if (ret < 0) { perror("recvmmsg"); exit(1); }

    printf("Received %d messages\n", ret);
    for (int i = 0; i < ret; i++) {
        printf("  msg[%d]: %u bytes\n", i, msgs[i].msg_len);
    }

    close(sockfd);
    return 0;
}
```

## 9. recvmmsg vs recvmsg

| 特性 | recvmsg | recvmmsg |
|------|---------|----------|
| 单次调用接收 | 1 个消息 | 多个消息 |
| 超时支持 | 通过 `SO_RCVTIMEO` | 支持 `timeout` 参数 |
| 内核入口 | 每次调用 1 次 | 1 次处理多个消息 |
| 适用场景 | 通用接收 | 高性能批量接收 |
| Linux 引入版本 | 早期 | 2.6.33 |

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`
- `man 2 recvmmsg`
- Linux 2.6.33 引入