# getsockopt 系统调用分析

## 1. 概述

`getsockopt` 系统调用获取套接字选项的当前值。套接字选项分为两层：`SOL_SOCKET` 层（通用套接字选项）和协议层（如 `IPPROTO_TCP`、`IPPROTO_IP` 等）。

**原型：**

```c
#include <sys/socket.h>

int getsockopt(int sockfd, int level, int optname,
               void *optval, socklen_t *optlen);
```

**参数：**
- `sockfd`：套接字文件描述符
- `level`：选项级别（`SOL_SOCKET`、`IPPROTO_TCP`、`IPPROTO_IP` 等）
- `optname`：选项名称
- `optval`：指向缓冲区，用于接收选项值
- `optlen`：值-结果参数，调用时指定缓冲区大小，返回时指示实际数据大小

**返回值：**
- 成功：返回 0
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2416
SYSCALL_DEFINE5(getsockopt, int, fd, int, level, int, optname,
        char __user *, optval, int __user *, optlen)
{
    return __sys_getsockopt(fd, level, optname, optval, optlen);
}
```

## 3. 详细的函数调用链

```
getsockopt (系统调用入口)
└── __sys_getsockopt(fd, level, optname, optval, optlen)  [net/socket.c:2400]
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    └── return do_sock_getsockopt(sock, in_compat_syscall(), level, optname,
                                  USER_SOCKPTR(optval), USER_SOCKPTR(optlen))
        │
        └── do_sock_getsockopt(sock, compat, level, optname, optval, optlen)
            [net/socket.c:2359]
            ├── err = security_socket_getsockopt(sock, level, optname)  → LSM
            │
            ├── if (!compat)
            │   └── copy_from_sockptr(&max_optlen, optlen, sizeof(int))
            │
            ├── ops = READ_ONCE(sock->ops)
            │
            ├── if (level == SOL_SOCKET && !sock_use_custom_sol_socket(sock))
            │   └── err = sock_getsockopt(sock, level, optname, optval, optlen)
            │       → 处理 SOL_SOCKET 层通用选项
            │       [net/core/sock.c]
            │       ├── 根据 optname 分派:
            │       │   ├── SO_DEBUG → sk->sk_debug
            │       │   ├── SO_REUSEADDR → sk->sk_reuse
            │       │   ├── SO_TYPE → sock->type
            │       │   ├── SO_ERROR → sk->sk_err (读取后清零)
            │       │   ├── SO_DONTROUTE → sk->sk_no_check_tx
            │       │   ├── SO_BROADCAST → sk->sk_broadcast
            │       │   ├── SO_SNDBUF → sk->sk_sndbuf
            │       │   ├── SO_RCVBUF → sk->sk_rcvbuf
            │       │   ├── SO_KEEPALIVE → sk->sk_protocol
            │       │   ├── SO_OOBINLINE → sk->sk_oob
            │       │   ├── SO_LINGER → sk->sk_lingertime
            │       │   ├── SO_RCVLOWAT → sk->sk_rcvlowat
            │       │   ├── SO_RCVTIMEO → sk->sk_rcvtimeo
            │       │   ├── SO_SNDTIMEO → sk->sk_sndtimeo
            │       │   ├── SO_BINDTODEVICE → sk->sk_bound_dev_if
            │       │   ├── SO_ATTACH_FILTER / SO_DETACH_FILTER → BPF
            │       │   ├── SO_PEERCRED → 对端进程凭据 (UNIX socket)
            │       │   ├── SO_DOMAIN → sock->ops->family
            │       │   ├── SO_PROTOCOL → sk->sk_protocol
            │       │   └── ... 更多选项
            │       └── return err
            │
            ├── else if (unlikely(!ops->getsockopt))
            │   └── err = -EOPNOTSUPP
            │
            └── else
                └── err = ops->getsockopt(sock, level, optname, optval, optlen)
                    → 协议特定选项
                    │
                    ├── 若 level == IPPROTO_TCP:
                    │   └── tcp_getsockopt(sk, level, optname, optval, optlen)
                    │       [net/ipv4/tcp.c]
                    │       ├── TCP_NODELAY → tp->nonagle
                    │       ├── TCP_CORK → tp->nonagle & TCP_NAGLE_CORK
                    │       ├── TCP_KEEPIDLE → tp->keepalive_time
                    │       ├── TCP_KEEPINTVL → tp->keepalive_intvl
                    │       ├── TCP_KEEPCNT → tp->keepalive_probes
                    │       ├── TCP_INFO → tcp_get_info()  (TCP 连接状态)
                    │       ├── TCP_CONGESTION → tp->ca_ops->name
                    │       ├── TCP_MAXSEG → tp->mss_cache
                    │       ├── TCP_WINDOW_CLAMP → tp->window_clamp
                    │       └── ...
                    │
                    ├── 若 level == IPPROTO_IP:
                    │   └── ip_getsockopt(sk, level, optname, optval, optlen)
                    │       [net/ipv4/ip_sockglue.c]
                    │       ├── IP_TTL → inet->uc_ttl
                    │       ├── IP_MULTICAST_TTL → inet->mc_ttl
                    │       ├── IP_OPTIONS → ip_options_get()
                    │       └── ...
                    │
                    └── 若 level == SOL_SOCKET 且 sock_use_custom_sol_socket():
                        └── ops->getsockopt → 处理自定义 SOL_SOCKET 选项
```

## 4. 选项分发表

### SOL_SOCKET 层通用选项

| optname | 数据结构字段 | 数据类型 | 说明 |
|---------|-------------|---------|------|
| `SO_DEBUG` | `sk->sk_debug` | int | 调试信息开关 |
| `SO_REUSEADDR` | `sk->sk_reuse` | int | 地址重用 |
| `SO_REUSEPORT` | `sk->sk_reuseport` | int | 端口重用 |
| `SO_TYPE` | `sock->type` | int | 套接字类型（只读） |
| `SO_ERROR` | `sk->sk_err` | int | 待处理错误（读取后清零） |
| `SO_DONTROUTE` | `sk->sk_no_check_tx` | int | 不路由 |
| `SO_BROADCAST` | `sk->sk_broadcast` | int | 广播许可 |
| `SO_SNDBUF` | `sk->sk_sndbuf` | int | 发送缓冲区大小 |
| `SO_RCVBUF` | `sk->sk_rcvbuf` | int | 接收缓冲区大小 |
| `SO_KEEPALIVE` | `sk->sk_protocol` | int | TCP 保活 |
| `SO_OOBINLINE` | `sk->sk_oob` | int | 带外数据内联 |
| `SO_LINGER` | `sk->sk_lingertime` | struct linger | 关闭等待 |
| `SO_RCVLOWAT` | `sk->sk_rcvlowat` | int | 接收低水位 |
| `SO_RCVTIMEO` | `sk->sk_rcvtimeo` | struct timeval | 接收超时 |
| `SO_SNDTIMEO` | `sk->sk_sndtimeo` | struct timeval | 发送超时 |
| `SO_BINDTODEVICE` | `sk->sk_bound_dev_if` | char[] | 绑定设备 |
| `SO_DOMAIN` | `sock->ops->family` | int | 协议域（只读） |
| `SO_PROTOCOL` | `sk->sk_protocol` | int | 协议类型（只读） |
| `SO_PEERCRED` | UNIX 特有 | struct ucred | 对端凭据 |

### IPPROTO_TCP 层选项

| optname | 数据结构字段 | 数据类型 | 说明 |
|---------|-------------|---------|------|
| `TCP_NODELAY` | `tp->nonagle` | int | 禁用 Nagle 算法 |
| `TCP_CORK` | `tp->nonagle & TCP_NAGLE_CORK` | int | 启用 Cork |
| `TCP_KEEPIDLE` | `tp->keepalive_time` | int | 保活空闲时间 |
| `TCP_KEEPINTVL` | `tp->keepalive_intvl` | int | 保活探测间隔 |
| `TCP_KEEPCNT` | `tp->keepalive_probes` | int | 保活探测次数 |
| `TCP_INFO` | 动态构造 | struct tcp_info | TCP 连接统计 |
| `TCP_CONGESTION` | `tp->ca_ops->name` | char[] | 拥塞控制算法 |
| `TCP_MAXSEG` | `tp->mss_cache` | int | 最大分段大小 |
| `TCP_WINDOW_CLAMP` | `tp->window_clamp` | int | 窗口上限 |

## 5. 流程图

```
用户态: getsockopt(fd, level, optname, optval, optlen)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE5(getsockopt)        │  net/socket.c:2416
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_getsockopt()                 │  net/socket.c:2400
   │  ├─ CLASS(fd) → 获取 file           │
   │  ├─ sock_from_file() → 获取 socket  │
   │  └─ do_sock_getsockopt()            │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  do_sock_getsockopt()               │  net/socket.c:2359
   │  ├─ security_socket_getsockopt()    │
   │  └─ 判断 level:                     │
   │     ├─ SOL_SOCKET → sock_getsockopt │  net/core/sock.c
   │     └─ 其他 → ops->getsockopt       │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  根据 level 和 optname 分发:         │
   │  ├─ SOL_SOCKET: sock_getsockopt()   │
   │  ├─ IPPROTO_TCP: tcp_getsockopt()   │
   │  ├─ IPPROTO_IP: ip_getsockopt()     │
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
| `EFAULT` | 地址指针无效 | `optval` 或 `optlen` 指向不可访问的区域 |
| `EINVAL` | 无效参数 | `optlen` 无效 |
| `EOPNOTSUPP` | 不支持的操作 | 协议不支持该选项 |
| `EACCES` | 权限不足 | LSM 拒绝访问 |
| `ECONNRESET` | 连接被重置 | 连接已断开 |

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

    // 获取套接字类型
    int type;
    socklen_t len = sizeof(type);
    if (getsockopt(sockfd, SOL_SOCKET, SO_TYPE, &type, &len) == 0) {
        printf("Socket type: %s\n", type == SOCK_STREAM ? "SOCK_STREAM" :
                                     type == SOCK_DGRAM ? "SOCK_DGRAM" : "other");
    }

    // 获取发送缓冲区大小
    int sndbuf;
    len = sizeof(sndbuf);
    if (getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len) == 0) {
        printf("Send buffer size: %d\n", sndbuf);
    }

    // 获取 TCP 相关信息（需要在连接建立后调用）
    // struct tcp_info info;
    // len = sizeof(info);
    // if (getsockopt(sockfd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0) {
    //     printf("RTT: %u usec\n", info.tcpi_rtt);
    // }

    close(sockfd);
    return 0;
}
```

## 8. SO_ERROR 的特殊行为

`SO_ERROR` 选项用于获取套接字上待处理的错误，并**自动清零**：

```c
// 获取并清除错误状态
int err;
socklen_t errlen = sizeof(err);
getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errlen);
// err 中返回待处理的错误码，0 表示无错误
// 读取后 sk->sk_err 被清零
```

典型用途：非阻塞 `connect` 后，通过 `poll`/`epoll` 检测到 `POLLOUT` 事件后，用 `SO_ERROR` 检查连接是否成功。

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/core/sock.c`、`net/ipv4/tcp.c`、`net/ipv4/ip_sockglue.c`
- `man 2 getsockopt`
- `man 7 socket` — SOL_SOCKET 选项说明
- `man 7 tcp` — TCP 协议选项说明