# setsockopt 系统调用分析

## 1. 概述

`setsockopt` 系统调用设置套接字选项。套接字选项分为两层：`SOL_SOCKET` 层（通用套接字选项）和协议层（如 `IPPROTO_TCP`、`IPPROTO_IP` 等）。

**原型：**

```c
#include <sys/socket.h>

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen);
```

**参数：**
- `sockfd`：套接字文件描述符
- `level`：选项级别（`SOL_SOCKET`、`IPPROTO_TCP`、`IPPROTO_IP` 等）
- `optname`：选项名称
- `optval`：指向选项值的缓冲区
- `optlen`：选项值长度

**返回值：**
- 成功：返回 0
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2350
SYSCALL_DEFINE5(setsockopt, int, fd, int, level, int, optname,
        char __user *, optval, int, optlen)
{
    return __sys_setsockopt(fd, level, optname, optval, optlen);
}
```

## 3. 详细的函数调用链

```
setsockopt (系统调用入口)
└── __sys_setsockopt(fd, level, optname, optval, optlen)  [net/socket.c:2333]
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    └── return do_sock_setsockopt(sock, in_compat_syscall(), level, optname,
                                  USER_SOCKPTR(optval), optlen)
        │
        └── do_sock_setsockopt(sock, compat, level, optname, optval, optlen)
            [net/socket.c:2289]
            ├── if (optlen < 0) return -EINVAL  → 校验长度
            │
            ├── err = security_socket_setsockopt(sock, level, optname)  → LSM
            │
            ├── if (!compat)
            │   err = BPF_CGROUP_RUN_PROG_SETSOCKOPT(sk, &level, &optname,
            │                                        optval, &optlen, &kernel_optval)
            │   → BPF cgroup 钩子，可修改选项
            │   ├── if (err < 0) goto out_put  → BPF 拒绝
            │   └── if (err > 0) { err = 0; goto out_put; }  → BPF 已处理
            │
            ├── if (kernel_optval)
            │   optval = KERNEL_SOCKPTR(kernel_optval)  → BPF 修改后的值
            │
            ├── ops = READ_ONCE(sock->ops)
            │
            ├── if (level == SOL_SOCKET && !sock_use_custom_sol_socket(sock))
            │   └── err = sock_setsockopt(sock, level, optname, optval, optlen)
            │       → 处理 SOL_SOCKET 层通用选项
            │       [net/core/sock.c]
            │       ├── 根据 optname 分派:
            │       │   ├── SO_DEBUG → sk->sk_debug = val
            │       │   ├── SO_REUSEADDR → sk->sk_reuse = (val == 1)
            │       │   ├── SO_REUSEPORT → sk->sk_reuseport = (val == 1)
            │       │   ├── SO_KEEPALIVE → sk->sk_protocol... (TCP keepalive)
            │       │   ├── SO_DONTROUTE → sk->sk_no_check_tx = val
            │       │   ├── SO_BROADCAST → sk->sk_broadcast = val
            │       │   ├── SO_SNDBUF → sk->sk_sndbuf = max(val, SOCK_MIN_SNDBUF)
            │       │   ├── SO_RCVBUF → sk->sk_rcvbuf = max(val, SOCK_MIN_RCVBUF)
            │       │   ├── SO_SNDLOWAT → sk->sk_sndlowat = val
            │       │   ├── SO_RCVLOWAT → sk->sk_rcvlowat = val
            │       │   ├── SO_LINGER → sk->sk_lingertime = l_linger
            │       │   ├── SO_PASSCRED → sock->flags |= SOCK_PASSCRED
            │       │   ├── SO_ATTACH_FILTER → 安装 BPF 过滤器
            │       │   ├── SO_DETACH_FILTER → 移除 BPF 过滤器
            │       │   ├── SO_PRIORITY → sk->sk_priority = val
            │       │   ├── SO_TXTIME → sk->sk_txtime... (发送时间)
            │       │   ├── SO_BINDTODEVICE → sk->sk_bound_dev_if = ifindex
            │       │   ├── SO_MARK → sk->sk_mark = val
            │       │   ├── SO_INCOMING_CPU → sk->sk_incoming_cpu = val
            │       │   ├── SO_ZEROCOPY → sock->sk->sk_flags ... (MSG_ZEROCOPY)
            │       │   └── ... 更多选项
            │       └── return err
            │
            ├── else if (unlikely(!ops->setsockopt))
            │   └── err = -EOPNOTSUPP
            │
            └── else
                └── err = ops->setsockopt(sock, level, optname, optval, optlen)
                    → 协议特定选项
                    │
                    ├── 若 level == SOL_SOCKET 且 sock_use_custom_sol_socket():
                    │   └── ops->setsockopt → 自定义 SOL_SOCKET 处理
                    │
                    ├── 若 level == IPPROTO_TCP:
                    │   └── tcp_setsockopt(sk, level, optname, optval, optlen)
                    │       [net/ipv4/tcp.c]
                    │       ├── TCP_NODELAY → tp->nonagle = 0/1
                    │       ├── TCP_CORK → tp->nonagle |= TCP_NAGLE_CORK
                    │       ├── TCP_KEEPIDLE → tp->keepalive_time = val
                    │       ├── TCP_KEEPINTVL → tp->keepalive_intvl = val
                    │       ├── TCP_KEEPCNT → tp->keepalive_probes = val
                    │       ├── TCP_CONGESTION → 切换拥塞控制算法
                    │       ├── TCP_MAXSEG → tp->mss_cache = val
                    │       ├── TCP_WINDOW_CLAMP → tp->window_clamp = val
                    │       ├── TCP_QUICKACK → tp->quickack = val
                    │       ├── TCP_DEFER_ACCEPT → tp->defer_accept = val
                    │       ├── TCP_FASTOPEN → tp->fastopen... = val
                    │       ├── TCP_FASTOPEN_CONNECT → tp->fastopen_connect = 1
                    │       ├── TCP_NOTSENT_LOWAT → tp->notsent_lowat = val
                    │       └── ...
                    │
                    ├── 若 level == IPPROTO_IP:
                    │   └── ip_setsockopt(sk, level, optname, optval, optlen)
                    │       [net/ipv4/ip_sockglue.c]
                    │       ├── IP_TTL → inet->uc_ttl = val
                    │       ├── IP_MULTICAST_TTL → inet->mc_ttl = val
                    │       ├── IP_MULTICAST_IF → inet->mc_index = ifindex
                    │       ├── IP_ADD_MEMBERSHIP → 加入多播组
                    │       ├── IP_DROP_MEMBERSHIP → 离开多播组
                    │       ├── IP_PKTINFO → inet->cmsg_flags |= IP_CMSG_PKTINFO
                    │       ├── IP_RECVTTL → inet->cmsg_flags |= IP_CMSG_TTL
                    │       └── ...
                    │
                    └── 若 level == SOL_SOCKET 且 sock_use_custom_sol_socket():
                        └── ops->setsockopt → 处理自定义 SOL_SOCKET 选项
```

## 4. 选项分发表

### SOL_SOCKET 层通用选项

| optname | 数据结构字段 | 数据类型 | 说明 |
|---------|-------------|---------|------|
| `SO_DEBUG` | `sk->sk_debug` | int | 调试信息开关 |
| `SO_REUSEADDR` | `sk->sk_reuse` | int | 允许地址重用（TIME_WAIT 时） |
| `SO_REUSEPORT` | `sk->sk_reuseport` | int | 允许端口重用（负载均衡） |
| `SO_KEEPALIVE` | `sk->sk_protocol` | int | TCP 保活探测 |
| `SO_DONTROUTE` | `sk->sk_no_check_tx` | int | 绕过路由表（直接发送） |
| `SO_BROADCAST` | `sk->sk_broadcast` | int | 允许发送广播包 |
| `SO_SNDBUF` | `sk->sk_sndbuf` | int | 发送缓冲区大小（下限 `SOCK_MIN_SNDBUF`） |
| `SO_RCVBUF` | `sk->sk_rcvbuf` | int | 接收缓冲区大小（下限 `SOCK_MIN_RCVBUF`） |
| `SO_SNDLOWAT` | `sk->sk_sndlowat` | int | 发送低水位 |
| `SO_RCVLOWAT` | `sk->sk_rcvlowat` | int | 接收低水位（默认 1） |
| `SO_LINGER` | `sk->sk_lingertime` | struct linger | close 时等待数据发送 |
| `SO_PASSCRED` | `sock->flags` | int | 接收 SCM_CREDENTIALS |
| `SO_ATTACH_FILTER` | `sk->sk_filter` | struct sock_fprog | 安装 BPF 过滤器 |
| `SO_PRIORITY` | `sk->sk_priority` | int | 套接字优先级 |
| `SO_BINDTODEVICE` | `sk->sk_bound_dev_if` | char[] | 绑定到特定网络设备 |
| `SO_MARK` | `sk->sk_mark` | int | 套接字标记（路由策略） |
| `SO_TIMESTAMP` | `sk->sk_tsflags` | int | 接收时间戳 |
| `SO_ZEROCOPY` | `sk->sk_flags` | int | 启用零拷贝发送 |
| `SO_TXTIME` | `sk->sk_txtime` | int | 发送时间调度 |

### IPPROTO_TCP 层选项

| optname | 数据结构字段 | 数据类型 | 说明 |
|---------|-------------|---------|------|
| `TCP_NODELAY` | `tp->nonagle` | int | 禁用 Nagle 算法 |
| `TCP_CORK` | `tp->nonagle` | int | 启用 Cork（累积发送） |
| `TCP_KEEPIDLE` | `tp->keepalive_time` | int | 保活空闲时间（秒） |
| `TCP_KEEPINTVL` | `tp->keepalive_intvl` | int | 保活探测间隔（秒） |
| `TCP_KEEPCNT` | `tp->keepalive_probes` | int | 保活探测次数 |
| `TCP_CONGESTION` | `tp->ca_ops` | char[] | 拥塞控制算法名称 |
| `TCP_MAXSEG` | `tp->mss_cache` | int | 最大分段大小 |
| `TCP_WINDOW_CLAMP` | `tp->window_clamp` | int | 接收窗口上限 |
| `TCP_QUICKACK` | `tp->quickack` | int | 快速 ACK 模式 |
| `TCP_DEFER_ACCEPT` | `tp->defer_accept` | int | 延迟 accept |
| `TCP_FASTOPEN` | `tp->fastopen` | int | 启用 TFO |
| `TCP_FASTOPEN_CONNECT` | `tp->fastopen_connect` | int | 客户端 TFO |
| `TCP_NOTSENT_LOWAT` | `tp->notsent_lowat` | int | 未发送数据低水位 |

## 5. 流程图

```
用户态: setsockopt(fd, level, optname, optval, optlen)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE5(setsockopt)        │  net/socket.c:2350
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_setsockopt()                 │  net/socket.c:2333
   │  ├─ CLASS(fd) → 获取 file           │
   │  ├─ sock_from_file() → 获取 socket  │
   │  └─ do_sock_setsockopt()            │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  do_sock_setsockopt()               │  net/socket.c:2289
   │  ├─ security_socket_setsockopt()    │
   │  ├─ BPF_CGROUP_RUN_PROG_SETSOCKOPT  │
   │  └─ 判断 level:                     │
   │     ├─ SOL_SOCKET → sock_setsockopt │  net/core/sock.c
   │     └─ 其他 → ops->setsockopt       │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  根据 level 和 optname 分发:         │
   │  ├─ SOL_SOCKET: sock_setsockopt()   │
   │  ├─ IPPROTO_TCP: tcp_setsockopt()   │
   │  ├─ IPPROTO_IP: ip_setsockopt()     │
   │  └─ 其他协议: 对应处理函数           │
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
| `ENOPROTOOPT` | 协议不可用 | `level` 和 `optname` 的组合无效 |
| `EFAULT` | 地址指针无效 | `optval` 指向不可访问的区域 |
| `EINVAL` | 无效参数 | `optlen` 无效或选项值无效 |
| `EOPNOTSUPP` | 不支持的操作 | 该选项不被支持 |
| `EACCES` | 权限不足 | LSM 拒绝设置该选项（如 `SO_BINDTODEVICE`） |
| `ENOMEM` | 内存不足 | 无法分配内存（如 `SO_ATTACH_FILTER`） |
| `EISCONN` | 已连接 | 某些选项在连接后无法更改 |
| `EDOM` | 参数超出范围 | 选项值超出有效范围 |

## 7. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    // 1. 设置 SO_REUSEADDR，允许地址重用
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
    }

    // 2. 设置发送缓冲区大小
    int sndbuf = 262144;  // 256KB
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
        perror("setsockopt SO_SNDBUF");
    }

    // 3. 禁用 Nagle 算法
    opt = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        perror("setsockopt TCP_NODELAY");
    }

    // 4. 设置 TCP 保活参数
    int keepalive = 1;
    int keepidle = 60;    // 60 秒
    int keepintvl = 10;   // 10 秒间隔
    int keepcnt = 3;      // 3 次探测

    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    // 5. 设置 SO_LINGER（关闭时立即发送 RST，而非 FIN）
    struct linger ling = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));

    close(sockfd);
    return 0;
}
```

## 8. BPF cgroup 钩子

`setsockopt` 支持 BPF cgroup 程序拦截和修改选项：

```c
// do_sock_setsockopt 中的 BPF 钩子
err = BPF_CGROUP_RUN_PROG_SETSOCKOPT(sk, &level, &optname,
                                      optval, &optlen, &kernel_optval);
```

- BPF 程序可以**拒绝**（返回负值）、**允许并修改**（提供新的 optval）或**直接处理**（返回正值）
- 这为容器场景提供了细粒度的套接字选项控制能力

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/core/sock.c`、`net/ipv4/tcp.c`、`net/ipv4/ip_sockglue.c`
- `man 2 setsockopt`
- `man 7 socket` — SOL_SOCKET 选项详细说明
- `man 7 tcp` — TCP 协议选项详细说明
- `man 7 ip` — IP 协议选项详细说明