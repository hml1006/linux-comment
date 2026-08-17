# recvmsg 系统调用分析

## 1. 概述

`recvmsg` 是接收数据的通用接口，通过 `struct msghdr` 结构可以同时接收数据、源地址和辅助数据（ancillary data / control messages）。它比 `recvfrom` 更灵活，支持 `struct iovec` 散射/聚集 I/O 和辅助数据（如 `SCM_RIGHTS`、`SCM_CREDENTIALS`、时间戳等）。

**原型：**

```c
#include <sys/socket.h>

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);
```

**参数：**
- `sockfd`：套接字文件描述符
- `msg`：指向 `struct msghdr` 的指针，包含：
  - `msg_name`：指向地址缓冲区（用于接收源地址）
  - `msg_namelen`：地址缓冲区大小 / 返回实际地址长度
  - `msg_iov`：IO 向量数组（散射缓冲区）
  - `msg_iovlen`：IO 向量数量
  - `msg_control`：辅助数据缓冲区
  - `msg_controllen`：辅助数据缓冲区大小
  - `msg_flags`：返回的消息标志
- `flags`：消息标志（`MSG_DONTWAIT`、`MSG_PEEK`、`MSG_OOB`、`MSG_ERRQUEUE` 等）

**返回值：**
- 成功：返回接收到的字节数
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2890
SYSCALL_DEFINE3(recvmsg, int, fd, struct user_msghdr __user *, msg,
        unsigned int, flags)
{
    return __sys_recvmsg(fd, msg, flags, true);
}
```

## 3. 详细的函数调用链

```
recvmsg (系统调用入口)
└── __sys_recvmsg(fd, msg, flags, true)  [net/socket.c:2870]
    ├── if (forbid_cmsg_compat && (flags & MSG_CMSG_COMPAT)) return -EINVAL
    │   → MSG_CMSG_COMPAT 是内部标志，用户态不允许使用
    │
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    │
    └── return ___sys_recvmsg(sock, msg, &msg_sys, flags, 0)
        │
        └── ___sys_recvmsg(sock, msg, &msg_sys, flags, 0)  [net/socket.c:2842]
            ├── struct iovec iovstack[UIO_FASTIOV], *iov = iovstack
            │   → 小缓冲区优化（避免堆分配）
            │
            ├── err = recvmsg_copy_msghdr(msg_sys, msg, flags, &uaddr, &iov)
            │   [net/socket.c:2765]
            │   ├── 从用户态复制 msghdr
            │   ├── 若 MSG_CMSG_COMPAT → 兼容模式
            │   │   └── get_compat_msghdr(msg, msg_compat, &uaddr, &iov)
            │   └── 否则:
            │       └── copy_msghdr_from_user(msg, umsg, &uaddr, &iov)
            │           ├── copy_from_user(msg, umsg, sizeof(*umsg))
            │           ├── 分配/复制 iovec 数组
            │           └── 保存 uaddr 指针
            │
            └── ____sys_recvmsg(sock, msg_sys, msg, uaddr, flags, 0)
                [net/socket.c:2786]
                ├── msg_sys->msg_name = &addr  → 指向内核态 sockaddr_storage
                ├── cmsg_ptr = (unsigned long)msg_sys->msg_control
                ├── msg_sys->msg_flags = flags & (MSG_CMSG_CLOEXEC|MSG_CMSG_COMPAT)
                ├── msg_sys->msg_namelen = 0
                │
                ├── if (sock->file->f_flags & O_NONBLOCK)
                │   flags |= MSG_DONTWAIT
                │
                ├── if (unlikely(nosec))
                │   err = sock_recvmsg_nosec(sock, msg_sys, flags)
                │ else
                │   err = sock_recvmsg(sock, msg_sys, flags)
                │   → 进入协议层（同 recvfrom 的 tcp_recvmsg / udp_recvmsg）
                │
                ├── if (err < 0) goto out
                │
                ├── len = err
                │
                ├── 若 uaddr != NULL:
                │   ├── move_addr_to_user(&addr, msg_sys->msg_namelen, uaddr, uaddr_len)
                │   └── 将源地址复制到用户空间
                │
                ├── __put_user(msg_sys->msg_flags, COMPAT_FLAGS(msg))
                │   → 将 msg_flags 写回用户空间
                │
                ├── 若 MSG_CMSG_COMPAT:
                │   __put_user((unsigned long)msg_sys->msg_control - cmsg_ptr,
                │              &msg_compat->msg_controllen)
                │ 否则:
                │   __put_user((unsigned long)msg_sys->msg_control - cmsg_ptr,
                │              &msg->msg_controllen)
                │   → 更新辅助数据实际长度
                │
                ├── err = len
                └── out:
                    └── return err
```

## 4. 关键数据结构

### struct msghdr — 消息头（用户态版本）

```c
// include/uapi/linux/socket.h
struct user_msghdr {
    void __user   *msg_name;       // 地址
    int           msg_namelen;     // 地址长度
    struct iovec  *msg_iov;        // 散射/聚集 IO 向量
    __kernel_size_t msg_iovlen;    // IO 向量数量
    void __user   *msg_control;    // 辅助数据
    __kernel_size_t msg_controllen;// 辅助数据长度
    unsigned int  msg_flags;       // 消息标志
};
```

### struct msghdr — 消息头（内核态版本）

```c
// include/linux/socket.h:71
struct msghdr {
    void        *msg_name;          // 源地址
    int         msg_namelen;        // 地址长度
    int         msg_inq;            // 剩余数据查询
    struct iov_iter msg_iter;       // 数据迭代器
    union {
        void        *msg_control;   // 辅助数据缓冲
        void __user *msg_control_user;
    };
    bool        msg_control_is_user : 1;
    bool        msg_get_inq : 1;
    unsigned int    msg_flags;      // 消息标志
    __kernel_size_t msg_controllen; // 辅助数据长度
    struct kiocb    *msg_iocb;
    struct ubuf_info *msg_ubuf;
    int (*sg_from_iter)(struct sk_buff *skb, struct iov_iter *from, size_t length);
};
```

### struct iovec — 散射/聚集 IO 向量

```c
// include/uapi/linux/uio.h
struct iovec {
    void __user *iov_base;  // 缓冲区起始地址
    __kernel_size_t iov_len; // 缓冲区长度
};
```

## 5. 辅助数据（Control Messages / Ancillary Data）

`recvmsg` 可以通过辅助数据接收多种附加信息：

```c
// 辅助数据格式
struct cmsghdr {
    socklen_t cmsg_len;    // 辅助数据长度（包含头部）
    int       cmsg_level;  // 协议级别（SOL_SOCKET, IPPROTO_IP, ...）
    int       cmsg_type;   // 类型
    // 紧随其后的数据...
};

// 使用宏遍历辅助数据
struct cmsghdr *cmsg;
for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        // 接收文件描述符
    }
}
```

### 常见辅助数据类型

| 级别 | 类型 | 说明 |
|------|------|------|
| `SOL_SOCKET` | `SCM_RIGHTS` | 传递文件描述符（UNIX socket） |
| `SOL_SOCKET` | `SCM_CREDENTIALS` | 传递进程凭据（UNIX socket） |
| `SOL_SOCKET` | `SCM_SECURITY` | 安全上下文（SELinux） |
| `SOL_SOCKET` | `SCM_TIMESTAMP` | 接收时间戳（32位 timeval） |
| `SOL_SOCKET` | `SCM_TIMESTAMPING` | 精确时间戳（timespec 数组） |
| `SOL_SOCKET` | `SCM_PIDFD` | 接收 pidfd（Linux 5.10+） |
| `IPPROTO_IP` | `IP_PKTINFO` | 接收包信息（源地址、接口索引） |
| `IPPROTO_IPV6` | `IPV6_PKTINFO` | IPv6 包信息 |

## 6. 流程图

```
用户态: recvmsg(fd, &msg, flags)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE3(recvmsg)           │  net/socket.c:2890
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_recvmsg()                    │  net/socket.c:2870
   │  ├─ CLASS(fd) → 获取 socket         │
   │  └─ ___sys_recvmsg()                │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  ___sys_recvmsg()                   │  net/socket.c:2842
   │  ├─ recvmsg_copy_msghdr()           │
   │  │  ├─ 复制 msghdr 从用户态到内核   │
   │  │  ├─ 复制 iovec 数组              │
   │  │  └─ 保存 uaddr 指针              │
   │  └─ ____sys_recvmsg()               │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  ____sys_recvmsg()                  │  net/socket.c:2786
   │  ├─ msg_sys->msg_name = &addr       │
   │  ├─ 继承 O_NONBLOCK → MSG_DONTWAIT  │
   │  ├─ sock_recvmsg(sock, msg, flags)  │
   │  │  └─ → tcp_recvmsg/udp_recvmsg    │
   │  ├─ move_addr_to_user()（可选）     │
   │  ├─ 写回 msg_flags                  │
   │  └─ 更新 msg_controllen             │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  协议层: tcp_recvmsg / udp_recvmsg  │
   │  └─ skb_copy_datagram_msg()         │
   │     └─ copy_page_to_iter()          │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 接收到的字节数               │
   └─────────────────────────────────────┘
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EAGAIN` | 资源暂时不可用 | 非阻塞模式且无数据可读 |
| `EWOULDBLOCK` | 同 EAGAIN | 非阻塞模式 |
| `EINTR` | 被信号中断 | 阻塞等待时收到信号 |
| `EFAULT` | 地址指针无效 | `msg` 指向不可访问的区域 |
| `ENOTCONN` | 未连接 | TCP 套接字未建立连接 |
| `ECONNRESET` | 连接被重置 | 对端发送了 RST |
| `EINVAL` | 无效参数 | `msg_iovlen` 无效或 flags 无效 |
| `EMSGSIZE` | 消息长度错误 | 辅助数据缓冲区太小 |
| `ENOMEM` | 内存不足 | 无法分配 iovec 或辅助数据缓冲区 |

## 8. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main() {
    int sockfd;
    struct sockaddr_in server_addr;
    struct msghdr msg = {0};
    struct iovec iov[1];
    char buf[1024];
    char cmsg_buf[CMSG_SPACE(sizeof(struct ucred))];

    // 创建 UNIX 本地套接字（演示辅助数据）
    sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    // 设置接收缓冲区
    iov[0].iov_base = buf;
    iov[0].iov_len = sizeof(buf);

    msg.msg_name = NULL;          // 不需要源地址
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;   // 辅助数据缓冲区
    msg.msg_controllen = sizeof(cmsg_buf);

    // 接收消息
    ssize_t n = recvmsg(sockfd, &msg, 0);
    if (n < 0) { perror("recvmsg"); exit(1); }

    printf("Received %zd bytes: %.*s\n", n, (int)n, buf);

    // 遍历辅助数据
    struct cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
            struct ucred *cred = (struct ucred *)CMSG_DATA(cmsg);
            printf("Credentials: pid=%d, uid=%d, gid=%d\n",
                   cred->pid, cred->uid, cred->gid);
        }
    }

    close(sockfd);
    return 0;
}
```

## 9. recvmsg 与相关系统调用的关系

| 系统调用 | 特点 | 适用场景 |
|----------|------|----------|
| `read` | 最简单，无 flags | 已连接 TCP 套接字 |
| `recv` | 支持 flags | TCP 需要 flags 时 |
| `recvfrom` | 支持 flags + 源地址 | UDP 需要源地址 |
| `recvmsg` | 支持 flags + 地址 + 辅助数据 + 散射 IO | 最通用的接收接口 |
| `recvmmsg` | 批量接收多个 recvmsg | 高性能场景 |

## 10. 性能优化

1. **UIO_FASTIOV 优化**：内核在栈上预分配 `iovstack[UIO_FASTIOV]`（通常为 8 个），避免大多数情况下的堆分配
2. **辅助数据内核缓冲区**：`ctl[sizeof(struct cmsghdr) + 20]` 栈上预分配，够用时避免堆分配
3. **零拷贝**：`skb_copy_datagram_msg` 使用 `copy_page_to_iter`，避免不必要的中间拷贝
4. **NOSEC 优化**：`sock_recvmsg_nosec` 绕过 LSM 检查（如果已检查过）

## 11. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/tcp.c`、`net/ipv4/udp.c`
- `man 2 recvmsg`
- `man 3 cmsg` — CMSG 辅助数据宏