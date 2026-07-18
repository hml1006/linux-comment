# bind 系统调用分析

## 1. 概述

`bind` 系统调用将套接字与一个本地地址（IP 地址和端口号）绑定。对于服务器端 TCP 套接字，`bind` 在 `listen` 之前调用，用于指定服务器监听的地址和端口。

**原型：**

```c
#include <sys/socket.h>

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

**参数：**
- `sockfd`：套接字文件描述符
- `addr`：指向 `struct sockaddr` 的指针，包含要绑定的本地地址
- `addrlen`：地址结构的长度

**返回值：**
- 成功：返回 0
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:1908
SYSCALL_DEFINE3(bind, int, fd, struct sockaddr __user *, umyaddr, int, addrlen)
{
    return __sys_bind(fd, umyaddr, addrlen);
}
```

## 3. 详细的函数调用链

```
bind (系统调用入口)
└── __sys_bind(fd, umyaddr, addrlen)  [net/socket.c:1888]
    ├── CLASS(fd, f)(fd)  → 获取 fd 对应的 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    ├── err = move_addr_to_kernel(umyaddr, addrlen, &address)  → 从用户态复制地址
    └── return __sys_bind_socket(sock, &address, addrlen)  [net/socket.c:1866]
        ├── err = security_socket_bind(sock, (struct sockaddr *)address, addrlen)  → LSM 检查
        └── if (!err)
            err = READ_ONCE(sock->ops)->bind(sock, (struct sockaddr_unsized *)address, addrlen)
                │
                └── inet_bind(sock, uaddr, addr_len)  [net/ipv4/af_inet.c:473]
                    └── inet_bind_sk(sock->sk, uaddr, addr_len)  → 实际绑定逻辑
                        └── __inet_bind(sk, uaddr, addr_len, 0)  [net/ipv4/af_inet.c:479]
                            ├── addr = (struct sockaddr_in *)uaddr
                            ├── inet = inet_sk(sk)  → 获取 inet_sock
                            ├── net = sock_net(sk)
                            ├── 校验 sin_family (AF_INET 或 AF_UNSPEC + INADDR_ANY)
                            ├── chk_addr_ret = inet_addr_type_table(net, addr->sin_addr.s_addr, tb_id)
                            ├── 校验地址是否有效 (inet_addr_valid_or_nonlocal)
                            ├── snum = ntohs(addr->sin_port)
                            ├── 若 snum 非零:
                            │   ├── snum < PROT_SOCK && !capable(CAP_NET_BIND_SERVICE)
                            │   │   → return -EACCES (特权端口保护)
                            │   └── 检查端口是否可用
                            ├── 若 SO_REUSEADDR 或 SO_REUSEPORT 设置:
                            │   └── 检查端口复用条件
                            ├── inet->inet_rcv_saddr = inet->inet_saddr = addr->sin_addr.s_addr
                            ├── sk->sk_rcv_saddr = addr->sin_addr.s_addr
                            ├── inet->inet_sport = htons(snum)  → 保存端口（网络字节序）
                            ├── sk->sk_prot->put_port(sk) / sk->sk_prot->hash(sk)  → 端口分配
                            └── return 0
```

## 4. 关键数据结构

### struct sockaddr_in — IPv4 套接字地址

```c
// include/uapi/linux/in.h
struct sockaddr_in {
    __kernel_sa_family_t sin_family;  // AF_INET (2)
    __be16              sin_port;     // 端口号（网络字节序）
    struct in_addr      sin_addr;     // IPv4 地址
    unsigned char       __pad[8];     // 填充
};
```

### struct inet_sock — INET 层套接字扩展

```c
// include/net/inet_sock.h
struct inet_sock {
    struct sock         sk;             // 基础 sock 结构
    struct ip_options_rcu __rcu *inet_opt;
    __be32              inet_saddr;     // 源地址（绑定后设置）
    __be32              inet_rcv_saddr; // 接收地址（绑定地址）
    __be16              inet_sport;     // 源端口（网络字节序）
    __be16              inet_dport;     // 目标端口（连接后设置）
    __be32              inet_daddr;     // 目标地址（连接后设置）
    __u16               inet_num;       // 源端口（主机字节序）
    // ...
};
```

### struct proto_ops — bind 操作

```c
// include/linux/net.h
struct proto_ops {
    // ...
    int (*bind)(struct socket *sock, struct sockaddr_unsized *myaddr, int sockaddr_len);
    // ...
};
```

对于 TCP 和 UDP，`inet_stream_ops` 和 `inet_dgram_ops` 中的 `.bind` 都指向 `inet_bind`。

## 5. 流程图

```
用户态: bind(sockfd, &addr, addrlen)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE3(bind)              │  net/socket.c:1908
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_bind()                       │  net/socket.c:1888
   │  ├─ CLASS(fd) → 获取 file           │
   │  ├─ sock_from_file() → 获取 socket  │
   │  └─ move_addr_to_kernel()           │
   │    用户态 sockaddr → 内核态 address  │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_bind_socket()                │  net/socket.c:1866
   │  ├─ security_socket_bind() → LSM    │
   │  └─ ops->bind() → 多态分发          │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_bind() → inet_bind_sk()       │  net/ipv4/af_inet.c:473
   │  → __inet_bind()                    │  net/ipv4/af_inet.c:479
   │  ├─ 校验地址族                      │
   │  ├─ 校验地址有效性                  │
   │  ├─ 校验端口（特权端口检查）        │
   │  ├─ 检查端口复用 (REUSEADDR)        │
   │  ├─ 设置 inet_saddr/rcv_saddr       │
   │  ├─ 设置 inet_sport                 │
   │  └─ sk->prot->hash() → 加入哈希表   │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 0 (成功)                    │
   └─────────────────────────────────────┘
```

## 6. 关键实现细节

### 6.1 地址从用户态复制到内核态

```c
// net/socket.c 中的 move_addr_to_kernel 函数
// 将用户态 sockaddr 安全复制到内核态的 sockaddr_storage
// 这也是一种安全检查，防止用户传入无效指针
err = move_addr_to_kernel(umyaddr, addrlen, &address);
if (unlikely(err))
    return err;
```

### 6.2 特权端口保护

```c
// net/ipv4/af_inet.c
snum = ntohs(addr->sin_port);
if (snum && snum < PROT_SOCK && !capable(CAP_NET_BIND_SERVICE))
    return -EACCES;
```

`PROT_SOCK` 通常为 1024。只有特权进程（`CAP_NET_BIND_SERVICE`）才能绑定到 1024 以下的端口。

### 6.3 地址有效性检查

```c
// net/ipv4/af_inet.c
chk_addr_ret = inet_addr_type_table(net, addr->sin_addr.s_addr, tb_id);
err = -EADDRNOTAVAIL;
if (!inet_addr_valid_or_nonlocal(net, inet, addr->sin_addr.s_addr, chk_addr_ret))
    goto out;
```

检查地址是否有效（本地地址、广播地址、多播地址等），如果启用了 `ip_nonlocal_bind`，也允许绑定非本地地址。

### 6.4 端口哈希

绑定端口后，协议层通过 `sk->sk_prot->hash(sk)` 将 socket 加入协议端口哈希表，这样后续的 `connect` 请求才能查找到该 socket。

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EINVAL` | 无效参数 | 地址长度错误或套接字已绑定 |
| `EADDRNOTAVAIL` | 地址不可用 | 请求的地址不是本地地址 |
| `EADDRINUSE` | 地址已使用 | 端口已被其他套接字占用（且未设置 `SO_REUSEADDR`） |
| `EACCES` | 权限不足 | 尝试绑定特权端口（< 1024）但无 `CAP_NET_BIND_SERVICE` 能力 |
| `EAFNOSUPPORT` | 地址族不支持 | `sin_family` 不是 `AF_INET` |
| `EFAULT` | 地址指针无效 | `umyaddr` 指向用户空间不可访问的区域 |

## 8. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    int sockfd;
    struct sockaddr_in addr;

    // 创建 TCP 套接字
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    // 设置 SO_REUSEADDR 允许端口重用（避免 TIME_WAIT 导致绑定失败）
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt"); exit(1);
    }

    // 绑定到 0.0.0.0:8080
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网络接口
    addr.sin_port = htons(8080);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    printf("Bound to port 8080\n");

    // 继续调用 listen...
    close(sockfd);
    return 0;
}
```

## 9. bind 与相关系统调用的关系

- **socket → bind → listen → accept**: 标准的 TCP 服务器流程
- **bind vs connect**: bind 绑定本地地址，connect 连接远端地址
- **bind 与 INADDR_ANY**: 绑定到 `0.0.0.0` 表示监听所有网络接口
- **bind 与 SO_REUSEADDR**: 允许在 `TIME_WAIT` 状态下重用端口，避免服务器重启后绑定失败
- **bind 与 SO_REUSEPORT**: 允许多个进程/线程绑定到同一端口，实现内核级负载均衡

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`
- `man 2 bind`
- `man 7 ip` — 关于 `ip_nonlocal_bind` 等高级选项