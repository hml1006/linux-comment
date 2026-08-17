# sendmsg 系统调用分析

## 1. 概述

`sendmsg` 是发送数据的通用接口，通过 `struct msghdr` 结构可以同时发送数据、目标地址和辅助数据（ancillary data）。它比 `sendto` 更灵活，支持 `struct iovec` 散射/聚集 I/O 和辅助数据（如 `SCM_RIGHTS`、`SCM_CREDENTIALS` 等）。

**原型：**

```c
#include <sys/socket.h>

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
```

**参数：**
- `sockfd`：套接字文件描述符
- `msg`：指向 `struct msghdr` 的指针，包含：
  - `msg_name`：目标地址（对于已连接套接字可为 NULL）
  - `msg_namelen`：目标地址长度
  - `msg_iov`：IO 向量数组
  - `msg_iovlen`：IO 向量数量
  - `msg_control`：辅助数据
  - `msg_controllen`：辅助数据长度
  - `msg_flags`：消息标志
- `flags`：消息标志（`MSG_DONTWAIT`、`MSG_NOSIGNAL`、`MSG_MORE`、`MSG_ZEROCOPY` 等）

**返回值：**
- 成功：返回发送的字节数
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2681
SYSCALL_DEFINE3(sendmsg, int, fd, struct user_msghdr __user *, msg, unsigned int, flags)
{
    return __sys_sendmsg(fd, msg, flags, true);
}
```

## 3. 详细的函数调用链

```
sendmsg (系统调用入口)
└── __sys_sendmsg(fd, msg, flags, true)  [net/socket.c:2661]
    ├── if (forbid_cmsg_compat && (flags & MSG_CMSG_COMPAT)) return -EINVAL
    │
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    │
    └── return ___sys_sendmsg(sock, msg, &msg_sys, flags, NULL, 0)
        │
        └── ___sys_sendmsg(sock, msg, &msg_sys, flags, NULL, 0)  [net/socket.c:2631]
            ├── struct iovec iovstack[UIO_FASTIOV], *iov = iovstack
            │   → 栈上预分配小缓冲区优化
            │
            ├── msg_sys->msg_name = &address  → 指向内核态 sockaddr_storage
            │
            ├── err = sendmsg_copy_msghdr(msg_sys, msg, flags, &iov)
            │   [net/socket.c:2611]
            │   ├── 若 MSG_CMSG_COMPAT → 兼容模式
            │   │   └── get_compat_msghdr(msg, msg_compat, &iov)
            │   └── 否则:
            │       └── copy_msghdr_from_user(msg, umsg, &iov)
            │           ├── copy_from_user(msg, umsg, sizeof(*umsg))
            │           └── 分配/复制 iovec 数组
            │
            └── ____sys_sendmsg(sock, msg_sys, flags, NULL, 0)
                [net/socket.c:2535]
                ├── unsigned char ctl[sizeof(struct cmsghdr) + 20]
                │   → 栈上预分配辅助数据缓冲区
                │
                ├── 处理辅助数据 (control messages):
                │   ├── if (msg_sys->msg_controllen > INT_MAX) goto out → -ENOBUFS
                │   ├── flags |= (msg_sys->msg_flags & allowed_msghdr_flags)
                │   ├── ctl_len = msg_sys->msg_controllen
                │   │
                │   ├── if (MSG_CMSG_COMPAT && ctl_len):
                │   │   └── cmsghdr_from_user_compat_to_kern(...)
                │   │
                │   ├── else if (ctl_len):
                │   │   ├── if (ctl_len > sizeof(ctl))
                │   │   │   ctl_buf = sock_kmalloc(sk, ctl_len, GFP_KERNEL)
                │   │   ├── copy_from_user(ctl_buf, msg_sys->msg_control_user, ctl_len)
                │   │   └── msg_sys->msg_control = ctl_buf
                │   │
                │   ├── flags &= ~MSG_INTERNAL_SENDMSG_FLAGS
                │   └── msg_sys->msg_flags = flags
                │
                ├── if (sock->file->f_flags & O_NONBLOCK)
                │   msg_sys->msg_flags |= MSG_DONTWAIT
                │
                ├── 地址缓存优化（sendmmsg 路径）:
                │   if (used_address && msg_sys->msg_name &&
                │       used_address->name_len == msg_sys->msg_namelen &&
                │       !memcmp(&used_address->name, msg_sys->msg_name,
                │               used_address->name_len))
                │       err = sock_sendmsg_nosec(sock, msg_sys)  → 跳过 LSM
                │   else
                │       err = __sock_sendmsg(sock, msg_sys)  → 含 LSM
                │
                └── 正常路径（sendmsg）:
                    err = __sock_sendmsg(sock, msg_sys)
                    │
                    └── __sock_sendmsg(sock, msg)  [net/socket.c:737]
                        ├── err = security_socket_sendmsg(sock, msg, msg_data_left(msg))
                        │   → LSM 检查
                        └── sock_sendmsg_nosec(sock, msg)  [net/socket.c:725]
                            └── INDIRECT_CALL_INET(ops->sendmsg, inet6_sendmsg,
                                                   inet_sendmsg, sock, msg, size)
                                → 多态分发
                                │
                                └── inet_sendmsg(sock, msg, size)  [net/ipv4/af_inet.c:858]
                                    ├── if (inet_send_prepare(sk)) return -EAGAIN
                                    │   → 检查 socket 是否就绪
                                    │
                                    └── INDIRECT_CALL_2(sk->sk_prot->sendmsg,
                                                       tcp_sendmsg, udp_sendmsg,
                                                       sk, msg, size)
                                        → 二级分发
                                        │
                                        ├── TCP: tcp_sendmsg(sk, msg, size)
                                        │   [net/ipv4/tcp.c]
                                        │   ├── lock_sock(sk)
                                        │   ├── tcp_sendmsg_locked(sk, msg, size)
                                        │   │   ├── 将用户数据复制到 skb
                                        │   │   ├── tcp_push(sk, flags, mss_now, ...)
                                        │   │   └── __tcp_push_pending_frames(sk)
                                        │   │       → tcp_write_xmit(sk)
                                        │   │       → __tcp_transmit_skb(sk, skb, ...)
                                        │   │       → ip_queue_xmit(sk, skb, fl)
                                        │   │       → IP 层输出
                                        │   └── release_sock(sk)
                                        │
                                        └── UDP: udp_sendmsg(sk, msg, size)
                                            [net/ipv4/udp.c]
                                            ├── 若 msg_name 非 NULL → 设置目标地址
                                            ├── 路由查找 (ip_route_output_flow)
                                            └── udp_send_skb(skb, fl4)
                                                → ip_local_out → IP 层输出
```

## 4. 关键数据结构

### struct msghdr — 消息头

```c
// include/linux/socket.h:71
struct msghdr {
    void        *msg_name;          // 目标地址
    int         msg_namelen;        // 地址长度
    int         msg_inq;            // 输入查询
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
    struct ubuf_info *msg_ubuf;     // 零拷贝（MSG_ZEROCOPY）
    int (*sg_from_iter)(struct sk_buff *skb, struct iov_iter *from, size_t length);
};
```

## 5. 辅助数据发送

```c
// 发送辅助数据示例
struct msghdr msg = {0};
char cmsg_buf[CMSG_SPACE(sizeof(struct ucred))];
struct cmsghdr *cmsg;

// 设置辅助数据
msg.msg_control = cmsg_buf;
msg.msg_controllen = sizeof(cmsg_buf);

cmsg = CMSG_FIRSTHDR(&msg);
cmsg->cmsg_level = SOL_SOCKET;
cmsg->cmsg_type = SCM_CREDENTIALS;
cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
*(struct ucred *)CMSG_DATA(cmsg) = (struct ucred){ .pid = getpid(), .uid = getuid(), .gid = getgid() };
msg.msg_controllen = cmsg->cmsg_len;

// 发送
sendmsg(fd, &msg, 0);
```

## 6. 流程图

```
用户态: sendmsg(fd, &msg, flags)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE3(sendmsg)           │  net/socket.c:2681
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_sendmsg()                    │  net/socket.c:2661
   │  ├─ CLASS(fd) → 获取 socket         │
   │  └─ ___sys_sendmsg()                │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  ___sys_sendmsg()                   │  net/socket.c:2631
   │  ├─ sendmsg_copy_msghdr()           │
   │  │  ├─ 复制 msghdr 从用户态到内核   │
   │  │  └─ 复制 iovec 数组              │
   │  └─ ____sys_sendmsg()               │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  ____sys_sendmsg()                  │  net/socket.c:2535
   │  ├─ 处理辅助数据 (cmsg)             │
   │  ├─ 继承 O_NONBLOCK → MSG_DONTWAIT  │
   │  └─ __sock_sendmsg()                │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sock_sendmsg()                   │  net/socket.c:737
   │  ├─ security_socket_sendmsg() → LSM │
   │  └─ sock_sendmsg_nosec()            │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  ops->sendmsg() → 多态分发          │
   │  └─ inet_sendmsg()                  │  net/ipv4/af_inet.c:858
   │     └─ sk->sk_prot->sendmsg()       │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  TCP: tcp_sendmsg()                 │  net/ipv4/tcp.c
   │  │  → tcp_sendmsg_locked()          │
   │  │  → tcp_push()                    │
   │  │  → __tcp_push_pending_frames()   │
   │  │  → tcp_write_xmit()              │
   │  │  → __tcp_transmit_skb()          │
   │  │  → ip_queue_xmit()               │
   │  │                                  │
   │  UDP: udp_sendmsg()                 │  net/ipv4/udp.c
   │     → 路由查找                      │
   │     → udp_send_skb()                │
   │     → ip_local_out()                │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 发送的字节数                 │
   └─────────────────────────────────────┘
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EAGAIN` | 资源暂时不可用 | 非阻塞模式且发送缓冲区满 |
| `EWOULDBLOCK` | 同 EAGAIN | 非阻塞模式 |
| `EINTR` | 被信号中断 | 阻塞发送时收到信号 |
| `EFAULT` | 地址指针无效 | `msg` 指向不可访问的区域 |
| `EMSGSIZE` | 消息长度错误 | 消息大于套接字发送缓冲区或路径 MTU |
| `ENOTCONN` | 未连接 | TCP 套接字未建立连接 |
| `ECONNRESET` | 连接被重置 | 对端发送了 RST |
| `ENOBUFS` | 缓冲区不足 | 内核内存不足 |
| `EOPNOTSUPP` | 不支持的操作 | 指定的 flags 或辅助数据类型不被支持 |
| `ENOMEM` | 内存不足 | 无法分配 skb 或辅助数据缓冲区 |
| `EHOSTUNREACH` | 主机不可达 | 路由不可达 |
| `ENETUNREACH` | 网络不可达 | 无路由到目标网络 |

## 8. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

int main() {
    int sockfd;
    struct sockaddr_in addr;
    struct msghdr msg = {0};
    struct iovec iov[2];  // 两个不连续的缓冲区
    char *part1 = "Hello, ";
    char *part2 = "World!";

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // 设置散射 I/O：两个不连续的缓冲区
    iov[0].iov_base = part1;
    iov[0].iov_len = strlen(part1);
    iov[1].iov_base = part2;
    iov[1].iov_len = strlen(part2);

    msg.msg_name = &addr;
    msg.msg_namelen = sizeof(addr);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    // 发送数据（内核会自动将两个缓冲区合并发送）
    ssize_t n = sendmsg(sockfd, &msg, 0);
    if (n < 0) { perror("sendmsg"); exit(1); }

    printf("Sent %zd bytes\n", n);
    close(sockfd);
    return 0;
}
```

## 9. sendmsg 与相关系统调用的关系

| 系统调用 | 特点 | 适用场景 |
|----------|------|----------|
| `write(fd, buf, len)` | 最简单，无 flags 和地址 | 已连接 TCP 套接字 |
| `send(fd, buf, len, flags)` | 支持 flags | TCP 需要 flags 时 |
| `sendto(fd, buf, len, flags, &addr, addrlen)` | 支持 flags + 目标地址 | UDP 需要指定地址 |
| `sendmsg(fd, &msg, flags)` | 最通用，支持辅助数据 + 散射 IO | 通用发送接口 |
| `sendmmsg(fd, msgvec, vlen, flags)` | 批量发送多个消息 | 高性能场景 |

## 10. 性能优化

1. **UIO_FASTIOV 优化**：栈上预分配 8 个 iovec，避免大多数情况下的堆分配
2. **辅助数据缓冲区**：栈上预分配 `ctl[sizeof(struct cmsghdr) + 20]`（20 字节足够容纳 ipv6_pktinfo）
3. **MSG_ZEROCOPY**：支持零拷贝发送（通过 `msg_ubuf` 字段）
4. **NOSEC 优化**：`sock_sendmsg_nosec` 绕过 LSM 检查（如果已检查过）
5. **INDIRECT_CALL_INET**：间接调用优化，提高多态分发性能

## 11. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`、`net/ipv4/tcp.c`、`net/ipv4/udp.c`
- `man 2 sendmsg`
- `man 3 cmsg` — CMSG 辅助数据宏