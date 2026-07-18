# sendto 系统调用分析

## 1. 概述

`sendto` 系统调用从套接字发送数据到指定的目标地址。对于 UDP 套接字，`sendto` 是最常用的发送方式，因为每次发送可以指定不同的目标地址。对于已连接的 TCP 套接字，`addr` 参数必须为 NULL 或忽略。

**原型：**

```c
#include <sys/socket.h>

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
```

**参数：**
- `sockfd`：套接字文件描述符
- `buf`：指向发送数据缓冲区的指针
- `len`：数据长度
- `flags`：消息标志（`MSG_DONTWAIT`、`MSG_NOSIGNAL`、`MSG_MORE` 等）
- `dest_addr`：指向目标地址的指针（对于已连接套接字可为 NULL）
- `addrlen`：目标地址长度

**返回值：**
- 成功：返回发送的字节数
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2209
SYSCALL_DEFINE6(sendto, int, fd, void __user *, buff, size_t, len,
        unsigned int, flags, struct sockaddr __user *, addr,
        int, addr_len)
{
    return __sys_sendto(fd, buff, len, flags, addr, addr_len);
}
```

## 3. 详细的函数调用链

```
sendto (系统调用入口)
└── __sys_sendto(fd, buff, len, flags, addr, addr_len)  [net/socket.c:2171]
    ├── struct msghdr msg = {0};
    │
    ├── err = import_ubuf(ITER_SOURCE, buff, len, &msg.msg_iter)
    │   → 将用户缓冲区映射到 msg_iter（ITER_SOURCE 表示读取方向）
    │
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    │
    ├── msg.msg_name = NULL;  → 初始化无地址
    ├── msg.msg_control = NULL;
    ├── msg.msg_controllen = 0;
    ├── msg.msg_namelen = 0;
    ├── msg.msg_ubuf = NULL;
    │
    ├── if (addr):  → 如果指定了目标地址
    │   ├── err = move_addr_to_kernel(addr, addr_len, &address)
    │   │   → 将地址从用户态复制到内核态
    │   ├── if (err < 0) return err
    │   ├── msg.msg_name = (struct sockaddr *)&address
    │   └── msg.msg_namelen = addr_len
    │
    ├── flags &= ~MSG_INTERNAL_SENDMSG_FLAGS  → 清除内部标记
    │
    ├── if (sock->file->f_flags & O_NONBLOCK)
    │   flags |= MSG_DONTWAIT  → 继承非阻塞标志
    │
    ├── msg.msg_flags = flags
    │
    └── return __sock_sendmsg(sock, &msg)  [net/socket.c:737]
        ├── err = security_socket_sendmsg(sock, msg, msg_data_left(msg))
        │   → LSM 检查
        └── sock_sendmsg_nosec(sock, msg)  [net/socket.c:725]
            └── INDIRECT_CALL_INET(ops->sendmsg, inet6_sendmsg,
                                   inet_sendmsg, sock, msg, size)
                → 多态分发
                │
                └── inet_sendmsg(sock, msg, size)  [net/ipv4/af_inet.c:858]
                    ├── if (inet_send_prepare(sk)) return -EAGAIN
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
                        │   │   ├── 循环从 msg_iter 读取数据
                        │   │   ├── tcp_push(sk, flags, mss_now, ...)
                        │   │   └── __tcp_push_pending_frames(sk)
                        │   │       → tcp_write_xmit(sk)
                        │   │       → __tcp_transmit_skb(sk, skb, ...)
                        │   │       → ip_queue_xmit(sk, skb, fl)
                        │   └── release_sock(sk)
                        │
                        └── UDP: udp_sendmsg(sk, msg, size)
                            [net/ipv4/udp.c]
                            ├── 若 msg->msg_name 非 NULL:
                            │   → 使用 msg->msg_name 作为目标地址
                            │   → 否则使用已 connect 的地址
                            ├── ip_route_output_flow(...)  → 路由查找
                            ├── udp_send_skb(skb, fl4)  → 构建并发送 UDP 数据报
                            └── ip_local_out(net, sk, skb)
                                → IP 层输出
```

## 4. 关键数据结构

### struct msghdr — sendto 简化版本

```c
// sendto 内部构造的 msg 结构
struct msghdr msg = {
    .msg_name = (addr) ? &address : NULL,   // 目标地址
    .msg_namelen = (addr) ? addr_len : 0,   // 地址长度
    .msg_iter = { .iter_type = ITER_IOVEC,  // 通过 import_ubuf 设置
                  .data_source = ITER_SOURCE,
                  .count = len,
                  .iov = &(struct iovec){ .iov_base = buff, .iov_len = len },
                  .iov_offset = 0 },
    .msg_control = NULL,
    .msg_controllen = 0,
    .msg_flags = flags,
};
```

## 5. 流程图

```
用户态: sendto(fd, buf, len, flags, &addr, addrlen)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE6(sendto)            │  net/socket.c:2209
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_sendto()                     │  net/socket.c:2171
   │  ├─ import_ubuf() → 映射用户缓冲区  │
   │  ├─ CLASS(fd) → 获取 socket         │
   │  ├─ 若 addr 非 NULL:                │
   │  │  move_addr_to_kernel() → 复制地址│
   │  ├─ 继承 O_NONBLOCK → MSG_DONTWAIT  │
   │  └─ __sock_sendmsg()                │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sock_sendmsg()                   │  net/socket.c:737
   │  ├─ security_socket_sendmsg() → LSM │
   │  └─ sock_sendmsg_nosec()            │
   │     └─ ops->sendmsg() → 多态分发    │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_sendmsg()                     │  net/ipv4/af_inet.c:858
   │  └─ sk->sk_prot->sendmsg()          │
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

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EAGAIN` | 资源暂时不可用 | 非阻塞模式且发送缓冲区满 |
| `EWOULDBLOCK` | 同 EAGAIN | 非阻塞模式 |
| `EINTR` | 被信号中断 | 阻塞发送时收到信号 |
| `EFAULT` | 地址指针无效 | `buf` 或 `dest_addr` 指向不可访问的区域 |
| `EMSGSIZE` | 消息长度错误 | 消息大于套接字发送缓冲区或路径 MTU |
| `ENOTCONN` | 未连接 | TCP 套接字未建立连接，或 UDP 未 connect 且未指定地址 |
| `ECONNRESET` | 连接被重置 | 对端发送了 RST |
| `EDESTADDRREQ` | 需要目标地址 | 套接字未连接且未指定目标地址 |
| `EINVAL` | 无效参数 | `flags` 包含无效标志 |
| `EISCONN` | 已连接 | 已连接套接字指定了目标地址 |
| `EHOSTUNREACH` | 主机不可达 | 路由不可达 |
| `ENETUNREACH` | 网络不可达 | 无路由到目标网络 |

## 7. 使用示例

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
    char *message = "Hello, UDP server!";

    // 创建 UDP 套接字
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    // 设置目标地址
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // 发送数据
    ssize_t n = sendto(sockfd, message, strlen(message), 0,
                       (struct sockaddr *)&addr, sizeof(addr));
    if (n < 0) { perror("sendto"); exit(1); }

    printf("Sent %zd bytes to 127.0.0.1:8888\n", n);
    close(sockfd);
    return 0;
}
```

## 8. sendto 与相关系统调用的关系

| 系统调用 | 区别 |
|----------|------|
| `write(fd, buf, len)` | 最基本的写操作，无 flags 和地址参数 |
| `send(fd, buf, len, flags)` | 有 flags 但无地址参数 |
| `sendto(fd, buf, len, flags, &addr, addrlen)` | 支持 flags + 目标地址 |
| `sendmsg(fd, &msg, flags)` | 最通用版，通过 msghdr 支持辅助数据和散射 IO |

**内部实现关系：**
- `write` → `__sys_sendto(fd, buf, len, 0, NULL, 0)`
- `send` → `__sys_sendto(fd, buf, len, flags, NULL, 0)`
- `sendto` → `__sys_sendto(fd, buf, len, flags, addr, addr_len)`
- `sendmsg` → `___sys_sendmsg()` → 支持更复杂的 msghdr 结构

## 9. UDP 的 sendto 行为

### UDP 未连接（unconnected）的 sendto

```
UDP socket (未 connect)
    │
    ├── sendto(fd, buf, len, 0, &addr, addrlen)
    │   ├── 使用 addr 作为目标地址
    │   ├── 路由查找 (ip_route_output_flow)
    │   ├── 构建 UDP 数据报
    │   └── 发送
    │
    └── 每次 sendto 可以指定不同的目标地址
```

### UDP 已连接（connected）的 sendto

```
UDP socket (已 connect 到 1.2.3.4:80)
    │
    ├── sendto(fd, buf, len, 0, NULL, 0)  → 发送到已连接的地址
    │
    └── sendto(fd, buf, len, 0, &addr, addrlen)
        → 如果 addr 与已连接地址不同，某些系统可能返回 EISCONN
        → Linux 会使用新地址，但行为依赖于实现
```

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`、`net/ipv4/tcp.c`、`net/ipv4/udp.c`
- `man 2 sendto`