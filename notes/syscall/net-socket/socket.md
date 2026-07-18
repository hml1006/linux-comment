# socket 系统调用分析

## 1. 概述

`socket` 系统调用创建一个新的套接字，返回一个文件描述符。它是所有网络通信的起点，创建一个通信端点。

**原型：**

```c
#include <sys/socket.h>

int socket(int domain, int type, int protocol);
```

**参数：**
- `domain`：协议域（地址族）：
  - `AF_INET` (2)：IPv4
  - `AF_INET6` (10)：IPv6
  - `AF_UNIX` (1)：UNIX 本地套接字
  - `AF_NETLINK` (16)：内核用户通信
  - 等
- `type`：套接字类型：
  - `SOCK_STREAM` (1)：流式套接字（TCP）
  - `SOCK_DGRAM` (2)：数据报套接字（UDP）
  - `SOCK_RAW` (3)：原始套接字
  - `SOCK_SEQPACKET` (5)：有序分组套接字
  - 可按位或 `SOCK_NONBLOCK` 和 `SOCK_CLOEXEC`
- `protocol`：协议（通常为 0，表示使用默认协议）

**返回值：**
- 成功：返回新的套接字文件描述符
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:1759
SYSCALL_DEFINE3(socket, int, family, int, type, int, protocol)
{
    return __sys_socket(family, type, protocol);
}
```

## 3. 详细的函数调用链

```
socket (系统调用入口)
└── __sys_socket(family, type, protocol)  [net/socket.c:1742]
    ├── sock = __sys_socket_create(family, type, protocol)  → 创建套接字
    │   │
    │   └── __sys_socket_create(family, type, protocol)  [net/socket.c:1685]
    │       ├── BUILD_BUG_ON 检查: 验证 SOCK_* 常量一致性
    │       │
    │       ├── 校验 flags:
    │       │   if ((type & ~SOCK_TYPE_MASK) & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
    │       │       return ERR_PTR(-EINVAL)
    │       │   type &= SOCK_TYPE_MASK  → 提取类型（去掉 flags）
    │       │
    │       ├── sock_create(family, type, protocol, &sock)  [net/socket.c]
    │       │   ├── if (family < 0 || family >= NPROTO) return -EAFNOSUPPORT
    │       │   │
    │       │   ├── if (protocol < 0) return -EINVAL  → 协议号校验
    │       │   │
    │       │   ├── sock = sock_alloc()  → 分配 struct socket
    │       │   │   ├── inode = new_inode_pseudo(sock_mnt->mnt_sb)
    │       │   │   ├── sock = SOCKET_I(inode)  → 从 inode 中获取 socket
    │       │   │   ├── sock->type = type
    │       │   │   ├── sock->state = SS_UNCONNECTED
    │       │   │   └── sock->file = NULL
    │       │   │
    │       │   ├── security_socket_create(family, type, protocol, kern)
    │       │   │   → LSM 检查
    │       │   │
    │       │   ├── net_families[family]->create(net, sock, protocol, kern)
    │       │   │   → 地址族注册的创建函数
    │       │   │   │
    │       │   │   └── inet_create(net, sock, protocol, kern)
    │       │   │       [net/ipv4/af_inet.c:260]
    │       │   │       ├── 遍历 inetsw[] 查找匹配的协议
    │       │   │       │   (inetsw 是 INET 协议开关表)
    │       │   │       │
    │       │   │       ├── sock->ops = answer->ops  → 设置 proto_ops
    │       │   │       │   ├── SOCK_STREAM → inet_stream_ops
    │       │   │       │   └── SOCK_DGRAM → inet_dgram_ops
    │       │   │       │
    │       │   │       ├── answer_prot = answer->prot  → 获取协议 proto
    │       │   │       │   ├── SOCK_STREAM → tcp_prot
    │       │   │       │   └── SOCK_DGRAM → udp_prot
    │       │   │       │
    │       │   │       ├── sk = sk_alloc(net, PF_INET, GFP_KERNEL, answer_prot, kern)
    │       │   │       │   → 分配 struct sock
    │       │   │       │   → 设置 sk->sk_prot = answer_prot
    │       │   │       │
    │       │   │       ├── sock_init_data(sock, sk)  → 初始化 sock 数据
    │       │   │       │   [net/core/sock.c]
    │       │   │       │   ├── sk->sk_sendmsg = 默认发送函数
    │       │   │       │   ├── sk->sk_recvmsg = 默认接收函数
    │       │   │       │   ├── skb_queue_head_init(&sk->sk_receive_queue)
    │       │   │       │   ├── skb_queue_head_init(&sk->sk_write_queue)
    │       │   │       │   ├── sk->sk_rcvbuf = sysctl_rmem_default
    │       │   │       │   ├── sk->sk_sndbuf = sysctl_wmem_default
    │       │   │       │   ├── sk->sk_state = TCP_CLOSE
    │       │   │       │   ├── sk->sk_userlocks = sk->sk_userlocks
    │       │   │       │   ├── sk->sk_protocol = protocol
    │       │   │       │   └── sock->sk = sk  → 建立 socket ↔ sock 关联
    │       │   │       │
    │       │   │       ├── inet = inet_sk(sk)  → 初始化 INET 特定字段
    │       │   │       │   ├── inet->inet_num = 0
    │       │   │       │   ├── inet->inet_saddr = 0
    │       │   │       │   ├── inet->inet_rcv_saddr = 0
    │       │   │       │   ├── inet->inet_dport = 0
    │       │   │       │   ├── inet->inet_daddr = 0
    │       │   │       │   └── inet->mc_ttl = 1
    │       │   │       │
    │       │   │       └── return 0
    │       │   │
    │       │   └── return 0
    │       │
    │       └── return sock
    │
    ├── flags = type & ~SOCK_TYPE_MASK  → 提取 SOCK_NONBLOCK / SOCK_CLOEXEC
    │
    ├── if (SOCK_NONBLOCK != O_NONBLOCK && (flags & SOCK_NONBLOCK))
    │   flags = (flags & ~SOCK_NONBLOCK) | O_NONBLOCK  → 兼容转换
    │
    └── return sock_map_fd(sock, flags & (O_CLOEXEC | O_NONBLOCK))
        [net/socket.c:504]
        ├── fd = get_unused_fd_flags(flags)  → 分配文件描述符
        │
        ├── newfile = sock_alloc_file(sock, flags, NULL)  → 创建 file 结构
        │   ├── file = alloc_file_pseudo(SOCK_INODE(sock), ...)
        │   ├── file->f_op = &socket_file_ops  → 设置文件操作表
        │   └── sock->file = file  → 建立 socket ↔ file 关联
        │
        ├── fd_install(fd, newfile)  → 安装文件描述符
        │
        └── return fd  → 返回文件描述符
```

## 4. 关键数据结构

### struct socket — 通用套接字

```c
// include/linux/net.h:116
struct socket {
    socket_state        state;     // SS_UNCONNECTED (初始状态)
    short               type;      // SOCK_STREAM / SOCK_DGRAM / SOCK_RAW
    unsigned long       flags;     // SOCK_NOSPACE, SOCK_PASSCRED, ...
    struct file         *file;     // 对应的文件结构（通过 sock_map_fd 关联）
    struct sock         *sk;       // 底层协议无关的 sock 结构
    const struct proto_ops *ops;   // 协议操作表（多态分发关键）
    struct socket_wq    wq;        // 等待队列
};
```

### struct sock — 协议无关的套接字表示

```c
// include/net/sock.h
struct sock {
    struct sk_buff_head sk_receive_queue;  // 接收队列
    struct sk_buff_head sk_write_queue;    // 发送队列
    int           sk_rcvbuf;               // 接收缓冲区大小
    int           sk_sndbuf;               // 发送缓冲区大小
    int           sk_state;                // TCP_CLOSE (初始状态)
    int           sk_protocol;             // 协议号
    unsigned long sk_flags;                // 各种标志
    const struct proto *sk_prot;           // 协议操作表（二级分发）
    // ...
};
```

### struct proto_ops — 协议操作表（一级分发）

```c
// include/linux/net.h:160
struct proto_ops {
    int family;
    int (*bind)(struct socket *, struct sockaddr_unsized *, int);
    int (*connect)(struct socket *, struct sockaddr_unsized *, int, int);
    int (*accept)(struct socket *, struct socket *, struct proto_accept_arg *);
    int (*listen)(struct socket *, int);
    int (*shutdown)(struct socket *, int);
    int (*sendmsg)(struct socket *, struct msghdr *, size_t);
    int (*recvmsg)(struct socket *, struct msghdr *, size_t, int);
    // ...
};
```

### struct proto — 协议操作表（二级分发）

```c
// include/net/sock.h
struct proto {
    char name[32];
    int (*sendmsg)(struct sock *, struct msghdr *, size_t);
    int (*recvmsg)(struct sock *, struct msghdr *, size_t, int, int *);
    int (*connect)(struct sock *, struct sockaddr_unsized *, int);
    int (*accept)(struct sock *, struct proto_accept_arg *);
    int (*shutdown)(struct sock *, int);
    int (*close)(struct sock *, long);
    int (*hash)(struct sock *);
    void (*unhash)(struct sock *);
    // ...
};
```

### 双重多态架构

```
socket() 创建:
  struct socket
    ├── ops → inet_stream_ops (TCP) 或 inet_dgram_ops (UDP)
    │            └── 一级分发: socket API → 协议族通用操作
    │
    └── sk → struct sock
               └── sk_prot → tcp_prot 或 udp_prot
                               └── 二级分发: 协议族通用 → 具体协议
```

## 5. inet_create 详细流程

```
inet_create(net, sock, protocol, kern)
    │
    ├── 查找 inetsw[] 协议开关表
    │   └── 匹配 type (SOCK_STREAM / SOCK_DGRAM / SOCK_RAW)
    │
    ├── sock->ops = answer->ops
    │   ├── SOCK_STREAM → inet_stream_ops
    │   │   ├── .bind = inet_bind
    │   │   ├── .connect = inet_stream_connect
    │   │   ├── .accept = inet_accept
    │   │   ├── .listen = inet_listen
    │   │   ├── .sendmsg = inet_sendmsg
    │   │   └── .recvmsg = inet_recvmsg
    │   │
    │   └── SOCK_DGRAM → inet_dgram_ops
    │       ├── .bind = inet_bind
    │       ├── .connect = inet_dgram_connect
    │       ├── .accept = sock_no_accept (UDP 不支持)
    │       ├── .sendmsg = inet_sendmsg
    │       └── .recvmsg = inet_recvmsg
    │
    ├── sk_alloc(PF_INET, GFP_KERNEL, answer_prot)
    │   ├── tcp_prot (TCP)
    │   │   ├── .name = "TCP"
    │   │   ├── .sendmsg = tcp_sendmsg
    │   │   ├── .recvmsg = tcp_recvmsg
    │   │   ├── .connect = tcp_v4_connect
    │   │   └── .shutdown = tcp_shutdown
    │   │
    │   └── udp_prot (UDP)
    │       ├── .name = "UDP"
    │       ├── .sendmsg = udp_sendmsg
    │       ├── .recvmsg = udp_recvmsg
    │       └── .connect = udp_connect
    │
    ├── sock_init_data(sock, sk)
    │   └── 初始化 sk 字段: 队列、缓冲区、状态等
    │
    └── 初始化 inet_sock 字段
```

## 6. 流程图

```
用户态: socket(domain, type, protocol)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE3(socket)            │  net/socket.c:1759
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_socket()                     │  net/socket.c:1742
   │  ├─ __sys_socket_create()           │
   │  │  ├─ 校验 type flags              │
   │  │  ├─ sock_create()                │
   │  │  │  ├─ sock_alloc() → 分配 socket│
   │  │  │  ├─ security_socket_create()  │
   │  │  │  └─ net_families[family]      │
   │  │  │     → create()                │
   │  │  │     └─ inet_create()          │
   │  │  │        ├─ 匹配 inetsw[]       │
   │  │  │        ├─ sock->ops = ops     │
   │  │  │        ├─ sk_alloc() → sk     │
   │  │  │        ├─ sock_init_data()    │
   │  │  │        └─ 初始化 inet_sock    │
   │  │  └─ return sock                  │
   │  ├─ 转换 flags                      │
   │  └─ sock_map_fd()                   │
   │     ├─ get_unused_fd_flags()        │
   │     ├─ sock_alloc_file()            │
   │     └─ fd_install()                 │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 新的文件描述符               │
   └─────────────────────────────────────┘
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EAFNOSUPPORT` | 地址族不支持 | `domain` 不是有效的地址族 |
| `EINVAL` | 无效参数 | `type` 包含无效标志，或 `protocol` 无效 |
| `EMFILE` | 进程文件描述符表满 | 达到 `RLIMIT_NOFILE` 限制 |
| `ENFILE` | 系统文件表满 | 系统级文件描述符上限 |
| `ENOBUFS` | 缓冲区不足 | 内核内存不足 |
| `ENOMEM` | 内存不足 | 无法分配 socket 或 sock 结构 |
| `EPROTONOSUPPORT` | 协议不支持 | `type` 和 `protocol` 组合无效 |
| `EACCES` | 权限不足 | LSM 拒绝创建（如原始套接字需要 `CAP_NET_RAW`） |

## 8. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    int tcp_fd, udp_fd, raw_fd;

    // 创建 TCP 套接字
    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) { perror("tcp socket"); exit(1); }
    printf("TCP socket fd: %d\n", tcp_fd);

    // 创建 UDP 套接字
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("udp socket"); exit(1); }
    printf("UDP socket fd: %d\n", udp_fd);

    // 创建带 SOCK_NONBLOCK 的 TCP 套接字
    int nbio_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (nbio_fd < 0) { perror("nonblock socket"); exit(1); }
    printf("Non-blocking TCP socket fd: %d\n", nbio_fd);

    // 创建原始套接字（需要 root 权限）
    // raw_fd = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    // if (raw_fd < 0) { perror("raw socket"); }

    close(tcp_fd);
    close(udp_fd);
    close(nbio_fd);
    return 0;
}
```

## 9. socket 与相关系统调用的关系

- **socket → bind → listen → accept**: 标准 TCP 服务器流程
- **socket → connect**: 标准 TCP 客户端流程
- **socket → sendto/recvfrom**: UDP 通信流程
- **socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)**: 创建非阻塞套接字（Linux 2.6.27+）
- **socketpair**: 创建一对已连接的套接字（比 socket 更适合本地通信）

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`、`net/core/sock.c`
- `man 2 socket`
- `man 7 socket` — 套接字接口概述