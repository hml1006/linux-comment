# recvfrom 系统调用分析

## 1. 概述

`recvfrom` 系统调用从套接字接收数据，并可以获取发送方的源地址。对于 UDP 套接字，`recvfrom` 可以获取每个数据报的源地址；对于 TCP 套接字，地址参数通常设为 NULL（因为 TCP 连接已建立，对端地址固定）。

**原型：**

```c
#include <sys/socket.h>

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
```

**参数：**
- `sockfd`：套接字文件描述符
- `buf`：指向接收数据缓冲区的指针
- `len`：缓冲区长度
- `flags`：消息标志（`MSG_DONTWAIT`、`MSG_PEEK`、`MSG_OOB`、`MSG_WAITALL` 等）
- `src_addr`：指向 `struct sockaddr` 的指针，用于接收源地址（可为 NULL）
- `addrlen`：值-结果参数，指定/返回地址长度

**返回值：**
- 成功：返回接收到的字节数
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2267
SYSCALL_DEFINE6(recvfrom, int, fd, void __user *, ubuf, size_t, size,
        unsigned int, flags, struct sockaddr __user *, addr,
        int __user *, addr_len)
{
    return __sys_recvfrom(fd, ubuf, size, flags, addr, addr_len);
}
```

## 3. 详细的函数调用链

### TCP 路径

```
recvfrom (系统调用入口)
└── __sys_recvfrom(fd, ubuf, size, flags, addr, addr_len)  [net/socket.c:2231]
    ├── 初始化 msg 结构:
    │   msg.msg_name = addr ? &address : NULL  → 若需要源地址
    │
    ├── err = import_ubuf(ITER_DEST, ubuf, size, &msg.msg_iter)
    │   → 将用户缓冲区映射到 msg_iter（ITER_DEST 表示写入方向）
    │
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    │
    ├── if (sock->file->f_flags & O_NONBLOCK)
    │   flags |= MSG_DONTWAIT  → 继承非阻塞标志
    │
    ├── err = sock_recvmsg(sock, &msg, flags)  [net/socket.c:1096]
    │   ├── security_socket_recvmsg(sock, msg, msg_data_left(msg), flags)
    │   │   → LSM 检查
    │   │
    │   └── sock_recvmsg_nosec(sock, msg, flags)  [net/socket.c:1075]
    │       └── INDIRECT_CALL_INET(ops->recvmsg, inet6_recvmsg,
    │                              inet_recvmsg, sock, msg, size, flags)
    │           → 多态分发到协议层
    │           │
    │           └── inet_recvmsg(sock, msg, size, flags)  [net/ipv4/af_inet.c:887]
    │               ├── sock_rps_record_flow(sk)  → RFS (Receive Flow Steering)
    │               │
    │               └── INDIRECT_CALL_2(sk->sk_prot->recvmsg,
    │                                   tcp_recvmsg, udp_recvmsg,
    │                                   sk, msg, size, flags, &addr_len)
    │                   → 二级分发
    │                   │
    │                   └── tcp_recvmsg(sk, msg, len, flags, &addr_len)
    │                       [net/ipv4/tcp.c]
    │                       ├── lock_sock(sk)
    │                       ├── tcp_recvmsg_locked(sk, msg, len, flags, ...)
    │                       │   [net/ipv4/tcp.c]
    │                       │   ├── while (数据未满足需求):
    │                       │   │   ├── skb = tcp_recv_skb(sk, ...)
    │                       │   │   │   → 从 sk_receive_queue 获取 skb
    │                       │   │   │
    │                       │   │   ├── 若 MSG_PEEK: 窥视数据（不移除 skb）
    │                       │   │   │
    │                       │   │   ├── skb_copy_datagram_msg(skb, offset, msg)
    │                       │   │   │   [net/core/datagram.c]
    │                       │   │   │   └── copy_page_to_iter(page, offset, len, &msg->msg_iter)
    │                       │   │   │       [lib/iov_iter.c]
    │                       │   │   │       → 将 skb 数据复制到用户缓冲区
    │                       │   │   │
    │                       │   │   ├── tcp_cleanup_rbuf(sk, len)  → 更新接收窗口
    │                       │   │   │
    │                       │   │   └── 若队列为空且非阻塞:
    │                       │   │       └── break → -EAGAIN
    │                       │   │
    │                       │   └── return 接收到的字节数
    │                       │
    │                       ├── release_sock(sk)
    │                       └── return err
    │
    ├── if (err >= 0 && addr != NULL):
    │   ├── err2 = move_addr_to_user(&address, msg.msg_namelen, addr, addr_len)
    │   │   → 将源地址从内核态复制到用户态
    │   └── if (err2 < 0) err = err2  → 地址复制失败时返回错误
    │
    └── return err
```

### UDP 路径概要

```
recvfrom → ... → inet_recvmsg → ... → udp_recvmsg(sk, msg, len, flags, &addr_len)
    [net/ipv4/udp.c]
    ├── 若 msg->msg_name 非 NULL:
    │   └── 将 UDP 数据报的源地址写入 msg->msg_name
    │       (msg->msg_namelen = sizeof(struct sockaddr_in))
    │
    ├── skb = skb_dequeue(&sk->sk_receive_queue)  → 取一个数据报
    │
    ├── skb_copy_datagram_msg(skb, 0, msg)  → 复制数据
    │
    └── return 接收到的字节数
```

## 4. 关键数据结构

### struct msghdr — 消息头结构

```c
// include/linux/socket.h:71
struct msghdr {
    void        *msg_name;          // 指向地址（recv 时接收源地址）
    int         msg_namelen;        // 地址长度
    int         msg_inq;            // 套接字中剩余数据量
    struct iov_iter msg_iter;       // 数据迭代器（描述数据缓冲区）
    union {
        void        *msg_control;   // 辅助数据（内核缓冲区）
        void __user *msg_control_user; // 辅助数据（用户缓冲区）
    };
    bool        msg_control_is_user : 1;
    bool        msg_get_inq : 1;
    unsigned int    msg_flags;      // 消息标志
    __kernel_size_t msg_controllen; // 辅助数据长度
    struct kiocb    *msg_iocb;      // 异步 I/O 控制块
    struct ubuf_info *msg_ubuf;     // 零拷贝信息
    int (*sg_from_iter)(struct sk_buff *skb, struct iov_iter *from, size_t length);
};
```

### struct iov_iter — 数据迭代器

```c
// include/linux/uio.h
struct iov_iter {
    u8          iter_type;     // ITER_IOVEC, ITER_KVEC, ITER_BVEC, ITER_XARRAY
    bool        copy_mc;       // 机器检查复制
    bool        nofault;       // 禁止缺页
    bool        data_source;   // ITER_SOURCE(发送) / ITER_DEST(接收)
    size_t      iov_offset;    // 当前偏移
    size_t      count;         // 剩余字节数
    // ...
};
```

## 5. 流程图

```
用户态: recvfrom(fd, buf, len, flags, &addr, &addrlen)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE6(recvfrom)          │  net/socket.c:2267
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_recvfrom()                   │  net/socket.c:2231
   │  ├─ 初始化 msg 结构                 │
   │  ├─ import_ubuf() → 映射用户缓冲区  │
   │  ├─ CLASS(fd) → 获取 socket         │
   │  ├─ 继承 O_NONBLOCK → MSG_DONTWAIT  │
   │  └─ sock_recvmsg()                  │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  sock_recvmsg()                     │  net/socket.c:1096
   │  ├─ security_socket_recvmsg() → LSM │
   │  └─ sock_recvmsg_nosec()            │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  ops->recvmsg() → 多态分发          │
   │  └─ inet_recvmsg()                  │  net/ipv4/af_inet.c:887
   │     └─ sk->sk_prot->recvmsg()       │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  TCP: tcp_recvmsg()                 │  net/ipv4/tcp.c
   │  UDP: udp_recvmsg()                 │  net/ipv4/udp.c
   │  ├─ 从 sk_receive_queue 取 skb      │
   │  ├─ skb_copy_datagram_msg() → 复制  │
   │  └─ copy_page_to_iter() → 用户空间  │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  若 addr 非 NULL:                   │
   │  move_addr_to_user() → 源地址       │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 接收到的字节数               │
   └─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EAGAIN` | 资源暂时不可用 | 非阻塞模式且无数据可读 |
| `EWOULDBLOCK` | 同 EAGAIN | 非阻塞模式 |
| `EINTR` | 被信号中断 | 阻塞等待时收到信号 |
| `EFAULT` | 地址指针无效 | `buf`、`src_addr` 或 `addrlen` 指向不可访问的区域 |
| `ENOTCONN` | 未连接 | TCP 套接字未建立连接 |
| `ECONNRESET` | 连接被重置 | 对端发送了 RST |
| `EINVAL` | 无效参数 | `flags` 包含无效标志 |
| `ENOMEM` | 内存不足 | 无法分配内核内存 |
| `EOPNOTSUPP` | 不支持的操作 | 指定的 flags 组合不被支持 |

## 7. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buf[1024];

    // 创建 UDP 套接字
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8888);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); exit(1);
    }

    printf("UDP server listening on port 8888...\n");

    // 接收数据并获取源地址
    ssize_t n = recvfrom(sockfd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&client_addr, &addr_len);
    if (n < 0) { perror("recvfrom"); exit(1); }

    buf[n] = '\0';
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
    printf("Received %zd bytes from %s:%d: %s\n",
           n, addr_str, ntohs(client_addr.sin_port), buf);

    close(sockfd);
    return 0;
}
```

## 8. recvfrom 与相关系统调用的关系

| 系统调用 | 区别 |
|----------|------|
| `read(fd, buf, len)` | 最基本的读操作，无 flags 和地址参数 |
| `recv(fd, buf, len, flags)` | 有 flags 但无地址参数 |
| `recvfrom(fd, buf, len, flags, &addr, &addrlen)` | 完整版，可获取源地址 |
| `recvmsg(fd, &msg, flags)` | 最通用版，通过 msghdr 支持辅助数据 |

**内部实现关系：**
- `read` → `__sys_recvfrom(fd, buf, len, 0, NULL, NULL)`
- `recv` → `__sys_recvfrom(fd, buf, len, flags, NULL, NULL)`
- `recvfrom` → `__sys_recvfrom(fd, buf, len, flags, addr, addr_len)`
- `recvmsg` → `___sys_recvmsg()` → 支持更复杂的 msghdr 结构

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`、`net/ipv4/tcp.c`、`net/ipv4/udp.c`
- `man 2 recvfrom`