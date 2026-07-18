# getpeername 系统调用分析

## 1. 概述

`getpeername` 系统调用获取已连接套接字的对端地址（即远程端的 IP 地址和端口号）。

**原型：**

```c
#include <sys/socket.h>

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
```

**参数：**
- `sockfd`：已连接套接字的文件描述符
- `addr`：指向 `struct sockaddr` 的指针，用于接收对端地址
- `addrlen`：值-结果参数，调用时指向 `addr` 缓冲区大小，返回时指示实际地址长度

**返回值：**
- 成功：返回 0
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2160
SYSCALL_DEFINE3(getpeername, int, fd, struct sockaddr __user *, usockaddr,
        int __user *, usockaddr_len)
{
    return __sys_getsockname(fd, usockaddr, usockaddr_len, 1);
    //                                        peer=1 表示获取对端地址
}
```

`getpeername` 和 `getsockname` 共享同一个内部函数 `__sys_getsockname`，区别仅在于最后一个参数 `peer`：
- `peer=0`：获取本地地址（getsockname）
- `peer=1`：获取对端地址（getpeername）

## 3. 详细的函数调用链

```
getpeername (系统调用入口)
└── __sys_getsockname(fd, usockaddr, usockaddr_len, 1)  [net/socket.c:2140]
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    └── return do_getsockname(sock, 1, usockaddr, usockaddr_len)  [net/socket.c:2117]
        ├── struct sockaddr_storage address;
        │
        ├── err = security_socket_getpeername(sock)  → LSM 检查
        │   （peer=1 时调用 getpeername 钩子）
        │
        ├── err = READ_ONCE(sock->ops)->getname(sock, (struct sockaddr *)&address, 1)
        │   │
        │   └── inet_getname(sock, uaddr, 1)  [net/ipv4/af_inet.c:809]
        │       ├── sk = sock->sk
        │       ├── inet = inet_sk(sk)  → 获取 inet_sock
        │       ├── sin = (struct sockaddr_in *)uaddr
        │       ├── sin->sin_family = AF_INET
        │       │
        │       ├── lock_sock(sk)
        │       │
        │       ├── peer == 1 (对端):
        │       │   ├── if (!inet->inet_dport)  → 未连接
        │       │   │   └── return -ENOTCONN
        │       │   ├── if (TCPF_CLOSE|TCPF_SYN_SENT & (1 << sk->sk_state))
        │       │   │   └── return -ENOTCONN
        │       │   ├── sin->sin_port = inet->inet_dport  → 对端端口
        │       │   ├── sin->sin_addr.s_addr = inet->inet_daddr  → 对端 IP
        │       │   └── BPF_CGROUP_RUN_SA_PROG(sk, sin, ...)  → BPF 钩子
        │       │       CGROUP_INET4_GETPEERNAME
        │       │
        │       └── release_sock(sk)
        │       └── return sizeof(struct sockaddr_in)
        │
        ├── if (err < 0) return err
        │   （err 实际上是地址长度，成功时为正数）
        │
        └── return move_addr_to_user(&address, err, usockaddr, usockaddr_len)
            → 将地址从内核态复制到用户态
```

## 4. 关键数据结构

### struct sockaddr_in — 返回给用户的 IPv4 地址

```c
// include/uapi/linux/in.h
struct sockaddr_in {
    __kernel_sa_family_t sin_family;  // AF_INET
    __be16              sin_port;     // 对端端口（网络字节序）
    struct in_addr      sin_addr;     // 对端 IP 地址
    unsigned char       __pad[8];
};
```

### inet_getname 的核心逻辑

```c
// net/ipv4/af_inet.c:809
int inet_getname(struct socket *sock, struct sockaddr *uaddr, int peer)
{
    struct sock *sk = sock->sk;
    struct inet_sock *inet = inet_sk(sk);
    DECLARE_SOCKADDR(struct sockaddr_in *, sin, uaddr);
    int sin_addr_len = sizeof(*sin);

    sin->sin_family = AF_INET;
    lock_sock(sk);
    if (peer) {
        // getpeername 路径
        if (!inet->inet_dport ||
            (((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_SYN_SENT)) &&
             peer == 1)) {
            release_sock(sk);
            return -ENOTCONN;
        }
        sin->sin_port = inet->inet_dport;
        sin->sin_addr.s_addr = inet->inet_daddr;
        BPF_CGROUP_RUN_SA_PROG(sk, sin, &sin_addr_len,
                       CGROUP_INET4_GETPEERNAME);
    } else {
        // getsockname 路径
        __be32 addr = inet->inet_rcv_saddr;
        if (!addr)
            addr = inet->inet_saddr;
        sin->sin_port = inet->inet_sport;
        sin->sin_addr.s_addr = addr;
        BPF_CGROUP_RUN_SA_PROG(sk, sin, &sin_addr_len,
                       CGROUP_INET4_GETSOCKNAME);
    }
    release_sock(sk);
    return sin_addr_len;
}
```

## 5. 流程图

```
用户态: getpeername(sockfd, &addr, &addrlen)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE3(getpeername)       │  net/socket.c:2160
   │  调用 __sys_getsockname(fd, ..., 1) │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_getsockname()                │  net/socket.c:2140
   │  ├─ CLASS(fd) → 获取 file           │
   │  ├─ sock_from_file() → 获取 socket  │
   │  └─ do_getsockname(sock, peer=1)    │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  do_getsockname()                   │  net/socket.c:2117
   │  ├─ security_socket_getpeername()   │
   │  └─ ops->getname(sock, &addr, 1)    │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_getname(sock, uaddr, 1)       │  net/ipv4/af_inet.c:809
   │  ├─ inet = inet_sk(sk)              │
   │  ├─ 检查是否已连接 (dport, state)   │
   │  ├─ sin->sin_port = inet_dport      │
   │  ├─ sin->sin_addr = inet_daddr      │
   │  └─ BPF_CGROUP_RUN_SA_PROG(...)     │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  move_addr_to_user()                │
   │  将地址从内核复制到用户空间          │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 0 (成功)                    │
   └─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `ENOTCONN` | 未连接 | 套接字未处于已连接状态 |
| `EFAULT` | 地址指针无效 | `usockaddr` 或 `usockaddr_len` 指向不可访问的区域 |
| `EINVAL` | 无效参数 | `addrlen` 指针无效 |
| `EOPNOTSUPP` | 不支持的操作 | 底层协议不支持 getname 操作 |

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
    struct sockaddr_in server_addr, peer_addr;
    socklen_t peer_len = sizeof(peer_addr);

    // 连接服务器
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(80);
    inet_pton(AF_INET, "93.184.216.34", &server_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("connect"); exit(1);
    }

    // 获取对端地址
    if (getpeername(sockfd, (struct sockaddr *)&peer_addr, &peer_len) == 0) {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer_addr.sin_addr, buf, sizeof(buf));
        printf("Connected to peer: %s:%d\n",
               buf, ntohs(peer_addr.sin_port));
    } else {
        perror("getpeername");
    }

    close(sockfd);
    return 0;
}
```

## 8. getpeername vs getsockname

| 特性 | getpeername | getsockname |
|------|-------------|-------------|
| 获取的信息 | 对端（远程）地址 | 本地地址 |
| peer 参数 | 1 | 0 |
| 使用的 BPF 钩子 | `CGROUP_INET4_GETPEERNAME` | `CGROUP_INET4_GETSOCKNAME` |
| 数据结构字段 | `inet_dport`, `inet_daddr` | `inet_sport`, `inet_rcv_saddr`/`inet_saddr` |
| 连接状态要求 | 必须已建立连接 | 只要已 bind 即可 |

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`
- `man 2 getpeername`