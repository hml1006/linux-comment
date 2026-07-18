# getsockname 系统调用分析

## 1. 概述

`getsockname` 系统调用获取套接字的本地地址（即本地绑定的 IP 地址和端口号）。对于未通过 `bind` 显式绑定的 TCP 套接字，`connect` 成功后内核会自动分配本地地址和端口，可通过 `getsockname` 获取。

**原型：**

```c
#include <sys/socket.h>

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
```

**参数：**
- `sockfd`：套接字文件描述符
- `addr`：指向 `struct sockaddr` 的指针，用于接收本地地址
- `addrlen`：值-结果参数，调用时指向 `addr` 缓冲区大小，返回时指示实际地址长度

**返回值：**
- 成功：返回 0
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2154
SYSCALL_DEFINE3(getsockname, int, fd, struct sockaddr __user *, usockaddr,
        int __user *, usockaddr_len)
{
    return __sys_getsockname(fd, usockaddr, usockaddr_len, 0);
    //                                        peer=0 表示获取本地地址
}
```

## 3. 详细的函数调用链

```
getsockname (系统调用入口)
└── __sys_getsockname(fd, usockaddr, usockaddr_len, 0)  [net/socket.c:2140]
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    └── return do_getsockname(sock, 0, usockaddr, usockaddr_len)  [net/socket.c:2117]
        ├── struct sockaddr_storage address;
        │
        ├── err = security_socket_getsockname(sock)  → LSM 检查
        │   （peer=0 时调用 getsockname 钩子）
        │
        ├── err = READ_ONCE(sock->ops)->getname(sock, (struct sockaddr *)&address, 0)
        │   │
        │   └── inet_getname(sock, uaddr, 0)  [net/ipv4/af_inet.c:809]
        │       ├── sk = sock->sk
        │       ├── inet = inet_sk(sk)  → 获取 inet_sock
        │       ├── sin = (struct sockaddr_in *)uaddr
        │       ├── sin->sin_family = AF_INET
        │       │
        │       ├── lock_sock(sk)
        │       │
        │       ├── peer == 0 (本地):
        │       │   ├── addr = inet->inet_rcv_saddr
        │       │   ├── if (!addr) addr = inet->inet_saddr
        │       │   │   （优先使用 rcv_saddr（bind 指定的地址），
        │       │   │    若未设置则使用系统自动选择的源地址）
        │       │   ├── sin->sin_port = inet->inet_sport  → 本地端口
        │       │   ├── sin->sin_addr.s_addr = addr  → 本地 IP
        │       │   └── BPF_CGROUP_RUN_SA_PROG(sk, sin, ...)  → BPF 钩子
        │       │       CGROUP_INET4_GETSOCKNAME
        │       │
        │       └── release_sock(sk)
        │       └── return sizeof(struct sockaddr_in)
        │
        ├── if (err < 0) return err
        │
        └── return move_addr_to_user(&address, err, usockaddr, usockaddr_len)
            → 将地址从内核态复制到用户态
```

## 4. 内核实现细节

### inet_getname 的本地地址获取逻辑

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
        // getpeername 路径 (略)
    } else {
        // getsockname 路径
        __be32 addr = inet->inet_rcv_saddr;  // 优先使用 bind 绑定的接收地址
        if (!addr)
            addr = inet->inet_saddr;         // 否则使用路由选择的源地址
        sin->sin_port = inet->inet_sport;    // 本地端口（网络字节序）
        sin->sin_addr.s_addr = addr;
        BPF_CGROUP_RUN_SA_PROG(sk, sin, &sin_addr_len,
                       CGROUP_INET4_GETSOCKNAME);
    }
    release_sock(sk);
    return sin_addr_len;
}
```

**关键点：**
- `inet->inet_rcv_saddr`：通过 `bind` 显式绑定的地址，若未 bind 则为 0
- `inet->inet_saddr`：连接时由路由系统自动选择的源地址
- 优先级：`rcv_saddr` > `saddr`
- `inet->inet_sport`：本地端口，**网络字节序**（与 `inet->inet_num` 的主机字节序不同）

### 地址从内核到用户的复制

```c
// net/socket.c
static int move_addr_to_user(struct sockaddr_storage *kaddr, int klen,
                             struct sockaddr __user *uaddr,
                             int __user *uaddr_len)
{
    // 1. 从用户空间读取 addr_len 缓冲区长度
    // 2. 取内核地址长度与用户缓冲区长度的较小值
    // 3. 复制地址数据到用户空间
    // 4. 将实际地址长度写回用户空间
}
```

## 5. 流程图

```
用户态: getsockname(sockfd, &addr, &addrlen)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE3(getsockname)       │  net/socket.c:2154
   │  调用 __sys_getsockname(fd, ..., 0) │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_getsockname()                │  net/socket.c:2140
   │  ├─ CLASS(fd) → 获取 file           │
   │  ├─ sock_from_file() → 获取 socket  │
   │  └─ do_getsockname(sock, peer=0)    │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  do_getsockname()                   │  net/socket.c:2117
   │  ├─ security_socket_getsockname()   │
   │  └─ ops->getname(sock, &addr, 0)    │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_getname(sock, uaddr, 0)       │  net/ipv4/af_inet.c:809
   │  ├─ inet = inet_sk(sk)              │
   │  ├─ addr = inet_rcv_saddr ?: saddr  │
   │  ├─ sin->sin_port = inet_sport      │
   │  ├─ sin->sin_addr = addr            │
   │  └─ BPF_CGROUP_RUN_SA_PROG(...)     │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  move_addr_to_user()                │
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

int main() {
    int sockfd;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    // 创建 TCP 套接字（不显式 bind）
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    // 连接前 getsockname 可能返回 0.0.0.0:0
    // 或连接后自动分配

    // 连接到远程服务器
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(80);
    inet_pton(AF_INET, "93.184.216.34", &server.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect"); exit(1);
    }

    // 获取内核自动分配的本地地址和端口
    if (getsockname(sockfd, (struct sockaddr *)&addr, &addr_len) == 0) {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        printf("Local address: %s:%d\n",
               buf, ntohs(addr.sin_port));
    }

    close(sockfd);
    return 0;
}
```

## 8. getsockname 的典型使用场景

1. **获取自动分配的端口号**：`bind` 时指定端口为 0，之后通过 `getsockname` 获取实际分配的端口
2. **获取自动分配的源 IP**：连接后查看内核选择的是哪个本地 IP 地址
3. **诊断和调试**：确认套接字绑定的地址和端口
4. **多宿主主机**：确定连接到特定对端时使用的是哪个本地接口

```c
// 绑定端口 0 后获取实际分配端口
struct sockaddr_in addr;
socklen_t addr_len = sizeof(addr);

bind(sockfd, (struct sockaddr *)&myaddr, sizeof(myaddr));
// 此时 myaddr.sin_port = htons(0)

getsockname(sockfd, (struct sockaddr *)&addr, &addr_len);
printf("Assigned port: %d\n", ntohs(addr.sin_port));
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`
- `man 2 getsockname`