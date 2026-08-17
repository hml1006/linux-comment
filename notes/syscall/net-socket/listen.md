# listen 系统调用分析

## 1. 概述

`listen` 系统调用将套接字标记为被动套接字（即监听模式），使其能够接受传入的连接请求。该调用必须在 `bind` 之后、`accept` 之前调用。

**原型：**

```c
#include <sys/socket.h>

int listen(int sockfd, int backlog);
```

**参数：**
- `sockfd`：套接字文件描述符（已通过 `bind` 绑定到本地地址）
- `backlog`：已完成连接队列（ESTABLISHED）的最大长度

**返回值：**
- 成功：返回 0
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:1946
SYSCALL_DEFINE2(listen, int, fd, int, backlog)
{
    return __sys_listen(fd, backlog);
}
```

## 3. 详细的函数调用链

```
listen (系统调用入口)
└── __sys_listen(fd, backlog)  [net/socket.c:1932]
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    └── return __sys_listen_socket(sock, backlog)  [net/socket.c:1918]
        ├── somaxconn = READ_ONCE(sock_net(sock->sk)->core.sysctl_somaxconn)
        │   → 读取内核参数 net.core.somaxconn（默认 4096）
        │
        ├── if ((unsigned int)backlog > somaxconn)
        │   backlog = somaxconn  → 静默截断到 somaxconn
        │
        ├── err = security_socket_listen(sock, backlog)  → LSM 检查
        │
        └── if (!err)
            err = READ_ONCE(sock->ops)->listen(sock, backlog)  → 多态分发
                │
                └── inet_listen(sock, backlog)  [net/ipv4/af_inet.c:238]
                    ├── sk = sock->sk
                    │
                    ├── lock_sock(sk)  → 获取 sock 锁
                    │
                    ├── 状态检查:
                    │   ├── if (sock->state != SS_UNCONNECTED) → goto out → -EINVAL
                    │   └── if (sock->type != SOCK_STREAM) → goto out → -EINVAL
                    │   （只有 SOCK_STREAM 类型且未连接状态才能 listen）
                    │
                    ├── err = __inet_listen_sk(sk, backlog)  [net/ipv4/af_inet.c]
                    │   ├── 若 sk->sk_state == TCP_LISTEN:
                    │   │   └── 更新监听队列大小即可
                    │   │   └── sk->sk_max_ack_backlog = backlog
                    │   │   └── return 0
                    │   │
                    │   ├── 若 sk->sk_state != TCP_CLOSE:
                    │   │   └── return -EINVAL
                    │   │
                    │   ├── sk->sk_state = TCP_LISTEN  → 设置状态为监听
                    │   │
                    │   ├── inet_csk_listen_start(sk, backlog)  → 启动监听
                    │   │   [net/ipv4/inet_connection_sock.c]
                    │   │   ├── 检查是否有未完成连接请求
                    │   │   ├── reqsk_queue_alloc()  → 分配请求队列
                    │   │   │   ├── 分配 request_sock 队列
                    │   │   │   ├── 哈希表 (ehash) 用于 SYN_RECV 状态查找
                    │   │   │   └── 设置队列大小相关参数
                    │   │   │
                    │   │   └── sk->sk_ack_backlog = 0  → 初始化已完成连接计数
                    │   │   sk->sk_max_ack_backlog = backlog  → 设置最大队列长度
                    │   │
                    │   └── return 0
                    │
                    ├── out:
                    └── release_sock(sk)  → 释放锁
                    └── return err
```

## 4. 关键数据结构

### TCP 监听队列结构

```
TCP 监听状态有两个队列：

  1. SYN 队列（未完成连接队列 / request_sock 队列）
     ┌─────────────────────────────────────────────┐
     │ 存储 SYN_RECV 状态的连接（三次握手未完成）   │
     │ 结构: struct request_sock_queue              │
     │ 队列元素: struct request_sock                │
     │ 最大长度: net.ipv4.tcp_max_syn_backlog       │
     └─────────────────────────────────────────────┘

  2. accept 队列（已完成连接队列 / sk_receive_queue）
     ┌─────────────────────────────────────────────┐
     │ 存储 ESTABLISHED 状态的连接（三次握手已完成） │
     │ 结构: struct sk_buff_head                    │
     │ 队列元素: struct sk_buff (含 struct sock)    │
     │ 最大长度: backlog (静默截断到 somaxconn)      │
     └─────────────────────────────────────────────┘
```

### struct sock — TCP 监听状态下的关键字段

```c
// include/net/sock.h
struct sock {
    // ...
    struct sk_buff_head sk_receive_queue;  // accept 队列（已完成连接）
    int       sk_ack_backlog;              // 当前已完成连接数
    int       sk_max_ack_backlog;          // 最大已完成连接数（backlog）
    // ...
};

// include/net/inet_connection_sock.h
struct inet_connection_sock {
    struct inet_sock   icsk_inet;
    // ...
    struct request_sock_queue icsk_accept_queue;  // SYN 队列
    // ...
};
```

## 5. backlog 参数详解

### backlog 的截断逻辑

```c
// net/socket.c:1918
int __sys_listen_socket(struct socket *sock, int backlog)
{
    int somaxconn;

    somaxconn = READ_ONCE(sock_net(sock->sk)->core.sysctl_somaxconn);
    if ((unsigned int)backlog > somaxconn)
        backlog = somaxconn;

    err = security_socket_listen(sock, backlog);
    if (!err)
        err = READ_ONCE(sock->ops)->listen(sock, backlog);
    return err;
}
```

- `backlog` 被静默截断到 `net.core.somaxconn`（默认值通常为 4096）
- 从 Linux 2.4+ 起，`backlog` 仅表示已完成连接队列（ESTABLISHED）的最大长度
- 未完成连接队列（SYN_RECV）由 `net.ipv4.tcp_max_syn_backlog` 控制

### 历史行为变化

- **Linux < 2.2**: `backlog` 包括 SYN_RECV 和 ESTABLISHED 两个队列的总和
- **Linux 2.2+**: `backlog` 仅表示 ESTABLISHED 队列长度
- 实际行为依赖于 `TCP_DEATH_ROW` 和 `tcp_abort_on_overflow` 等参数

## 6. 流程图

```
用户态: listen(sockfd, backlog)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE2(listen)            │  net/socket.c:1946
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_listen()                     │  net/socket.c:1932
   │  ├─ CLASS(fd) → 获取 file           │
   │  ├─ sock_from_file() → 获取 socket  │
   │  └─ __sys_listen_socket()           │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_listen_socket()              │  net/socket.c:1918
   │  ├─ backlog 截断到 somaxconn        │
   │  ├─ security_socket_listen()        │
   │  └─ ops->listen() → 多态分发        │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_listen()                      │  net/ipv4/af_inet.c:238
   │  ├─ lock_sock(sk)                   │
   │  ├─ 检查状态: SS_UNCONNECTED        │
   │  ├─ 检查类型: SOCK_STREAM           │
   │  └─ __inet_listen_sk()              │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __inet_listen_sk()                 │  net/ipv4/af_inet.c
   │  ├─ 若已 TCP_LISTEN → 更新 backlog  │
   │  ├─ 检查状态: TCP_CLOSE             │
   │  ├─ sk->sk_state = TCP_LISTEN       │
   │  └─ inet_csk_listen_start()         │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_csk_listen_start()            │
   │  ├─ reqsk_queue_alloc() → 分配队列  │
   │  ├─ sk->sk_ack_backlog = 0          │
   │  └─ sk->sk_max_ack_backlog = backlog│
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 0 (成功)                    │
   └─────────────────────────────────────┘
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EINVAL` | 无效参数 | 套接字未绑定、类型不是 SOCK_STREAM、或已连接 |
| `EOPNOTSUPP` | 不支持的操作 | 套接字类型不支持 listen（如 SOCK_DGRAM） |
| `EACCES` | 权限不足 | LSM 拒绝 listen 操作 |
| `EADDRINUSE` | 地址已使用 | 绑定的地址和端口已被其他套接字监听 |

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

    // 1. 创建套接字
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    // 2. 设置 SO_REUSEADDR
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 绑定地址
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    // 4. 开始监听
    if (listen(sockfd, 128) < 0) {
        perror("listen"); exit(1);
    }
    printf("Listening on port 8080 with backlog=128\n");

    // 5. 接受连接
    // accept(sockfd, NULL, NULL);

    close(sockfd);
    return 0;
}
```

## 9. listen 与 accept 的关系

```
listen(fd, 128) → 设置已完成连接队列最大长度为 128
                    │
  三次握手完成 → 自动放入 accept 队列
                    │
accept(fd, ...) → 从队列头部取出一个连接
                    │
  若队列满 (sk_ack_backlog == sk_max_ack_backlog):
    └── 新连接被丢弃或拒绝（取决于 tcp_abort_on_overflow）
```

## 10. 相关内核参数

| 参数 | 路径 | 默认值 | 说明 |
|------|------|--------|------|
| `somaxconn` | `/proc/sys/net/core/somaxconn` | 4096 | backlog 上限 |
| `tcp_max_syn_backlog` | `/proc/sys/net/ipv4/tcp_max_syn_backlog` | 128/256 | SYN 队列最大长度 |
| `tcp_synack_retries` | `/proc/sys/net/ipv4/tcp_synack_retries` | 5 | SYN+ACK 重试次数 |
| `tcp_abort_on_overflow` | `/proc/sys/net/ipv4/tcp_abort_on_overflow` | 0 | 监听队列满时是否拒绝连接 |

## 11. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`、`net/ipv4/inet_connection_sock.c`
- `man 2 listen`
- `man 7 tcp` — TCP 协议详细说明