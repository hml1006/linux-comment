# accept4 系统调用分析

## 1. 概述

`accept4` 系统调用从监听套接字的已完成连接队列中取出第一个连接，创建一个新的已连接套接字，并返回其文件描述符。与 `accept` 的区别在于支持通过 `flags` 参数直接设置 `SOCK_NONBLOCK` 和 `SOCK_CLOEXEC` 标志，避免了额外的 `fcntl` 调用。

**原型：**

```c
#include <sys/socket.h>

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
```

**参数：**
- `sockfd`：监听套接字的文件描述符
- `addr`：指向 `struct sockaddr` 的指针，用于接收对端地址（可为 NULL）
- `addrlen`：值-结果参数，指向缓冲区大小，返回时指示实际地址长度
- `flags`：以下标志的按位或：
  - `SOCK_NONBLOCK`：新套接字设置为非阻塞模式
  - `SOCK_CLOEXEC`：新套接字设置 `FD_CLOEXEC`（执行时关闭）

**返回值：**
- 成功：返回新的已连接套接字文件描述符（非负整数）
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2048
SYSCALL_DEFINE4(accept4, int, fd, struct sockaddr __user *, upeer_sockaddr,
		int __user *, upeer_addrlen, int, flags)
{
	return __sys_accept4(fd, upeer_sockaddr, upeer_addrlen, flags);
}
```

## 3. 详细的函数调用链

```
accept4 (系统调用入口)
└── __sys_accept4(fd, upeer_sockaddr, upeer_addrlen, flags)  [net/socket.c:2037]
    ├── CLASS(fd, f)(fd)  → 获取 fd 对应的 struct file
    └── if (fd_empty(f)) return -EBADF
    └── __sys_accept4_file(fd_file(f), upeer_sockaddr, upeer_addrlen, flags)  [net/socket.c:2011]
        ├── flags 校验: 只允许 SOCK_CLOEXEC | SOCK_NONBLOCK
        ├── SOCK_NONBLOCK != O_NONBLOCK 时的兼容转换
        └── FD_ADD(flags, do_accept(file, &arg, upeer_sockaddr, upeer_addrlen, flags))
            │
            └── do_accept()  [net/socket.c:1951]
                ├── 1. sock_from_file(file) → 获取监听 socket
                ├── 2. sock_alloc() → 分配新的 struct socket (newsock)
                ├── 3. 继承: newsock->type = sock->type, newsock->ops = ops
                ├── 4. __module_get(ops->owner) → 增加协议模块引用计数
                ├── 5. sock_alloc_file(newsock, flags, ...) → 创建 file，flags 含 SOCK_CLOEXEC
                ├── 6. security_socket_accept(sock, newsock) → LSM 钩子
                ├── 7. arg->flags |= sock->file->f_flags  → 继承监听套接字的文件标志
                ├── 8. ops->accept(sock, newsock, &arg) → 多态分发
                │   └── inet_accept(sock, newsock, &arg)  [net/ipv4/af_inet.c:788]
                │       ├── sk2 = sk1->sk_prot->accept(sk1, &arg)
                │       │   └── inet_csk_accept(sk, &arg)
                │       │       ├── lock_sock(sk)
                │       │       ├── 等待 sk_receive_queue 非空（阻塞或非阻塞）
                │       │       ├── skb = skb_dequeue(&sk->sk_receive_queue)
                │       │       ├── newsk = skb->sk
                │       │       ├── release_sock(sk)
                │       │       └── return newsk
                │       ├── lock_sock(sk2)
                │       ├── __inet_accept(sock, newsock, sk2)  [net/ipv4/af_inet.c:762]
                │       │   ├── sock_graft(newsk, newsock)  → 建立关联
                │       │   └── newsock->state = SS_CONNECTED
                │       └── release_sock(sk2)
                │       └── return 0
                ├── 9. 若 upeer_sockaddr 非 NULL:
                │   ├── ops->getname(newsock, &address, 2) → 获取对端地址
                │   └── move_addr_to_user(&address, len, upeer_sockaddr, upeer_addrlen)
                └── 10. return newfile
```

## 4. 关键数据结构

### struct proto_accept_arg

```c
// 在 include/linux/net.h 中定义
struct proto_accept_arg {
    int flags;    // 从 socket 继承的 f_flags
    int err;      // 错误码（由协议层设置）
};
```

### struct socket — 通用套接字抽象

```c
// include/linux/net.h:116
struct socket {
    socket_state        state;    // 套接字状态
    short               type;     // 套接字类型
    unsigned long       flags;    // 套接字标志
    struct file         *file;    // 文件结构反向引用
    struct sock         *sk;      // 协议层 sock
    const struct proto_ops *ops;  // 协议操作表
    struct socket_wq    wq;       // 等待队列
};
```

### inet_stream_ops — TCP 的 proto_ops 实例

```c
// net/ipv4/af_inet.c:1066
const struct proto_ops inet_stream_ops = {
    .family     = PF_INET,
    .accept     = inet_accept,
    .getname    = inet_getname,
    // ...
};
```

## 5. accept4 的 flags 处理

```c
// net/socket.c:2016
if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
    return -EINVAL;

// SOCK_NONBLOCK 与 O_NONBLOCK 的值可能不同，需要转换
// 在某些架构上 SOCK_NONBLOCK != O_NONBLOCK
if (SOCK_NONBLOCK != O_NONBLOCK && (flags & SOCK_NONBLOCK))
    flags = (flags & ~SOCK_NONBLOCK) | O_NONBLOCK;

// FD_ADD 宏将 flags 传递给 sock_alloc_file
// 其中 O_CLOEXEC 控制 close-on-exec 标志
// O_NONBLOCK 控制新 fd 的非阻塞标志
```

## 6. 流程图

```
用户态: accept4(sockfd, &addr, &addrlen, flags)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE4(accept4)           │  net/socket.c:2048
   │  调用 __sys_accept4()               │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_accept4()                    │  net/socket.c:2037
   │  CLASS(fd, f) → 获取 file           │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_accept4_file()               │  net/socket.c:2011
   │  校验 flags (SOCK_CLOEXEC|NONBLOCK) │
   │  兼容性转换                         │
   │  FD_ADD(flags, do_accept())         │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  do_accept()                        │  net/socket.c:1951
   │  ┌─ 分配 newsock                    │
   │  ├─ 继承 ops                        │
   │  ├─ sock_alloc_file(flags)          │
   │  ├─ security_socket_accept()        │
   │  ├─ ops->accept()                   │
   │  ├─ ops->getname()                  │
   │  └─ move_addr_to_user()             │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_accept()                      │  net/ipv4/af_inet.c:788
   │  ┌─ inet_csk_accept() → 取连接      │
   │  ├─ __inet_accept() → 建立关联      │
   │  └─ 返回 0                          │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return new_fd (带有 SOCK_CLOEXEC   │
   │  / SOCK_NONBLOCK 标志)              │
   └─────────────────────────────────────┘
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EINVAL` | 无效参数 | flags 包含 `SOCK_CLOEXEC`/`SOCK_NONBLOCK` 之外的标志 |
| `EOPNOTSUPP` | 操作不支持 | 套接字类型不支持 accept |
| `EMFILE` | 进程文件描述符表满 | 达到 `RLIMIT_NOFILE` 限制 |
| `ENFILE` | 系统文件表满 | 系统级文件描述符上限 |
| `ENOMEM` | 内存不足 | 无法分配内存 |
| `EAGAIN` | 资源暂时不可用 | 非阻塞模式且无已完成连接 |
| `ECONNABORTED` | 连接已中止 | 连接在获取地址时断开 |
| `EINTR` | 被信号中断 | 阻塞等待时收到信号 |

## 8. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

int main() {
    int listen_fd, conn_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);

    bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(listen_fd, 10);

    printf("Server listening on port 8080...\n");

    // 使用 accept4 直接设置非阻塞和 close-on-exec
    conn_fd = accept4(listen_fd, (struct sockaddr *)&client_addr,
                      &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn_fd < 0) { perror("accept4"); exit(1); }

    // conn_fd 已经自动设置为 O_NONBLOCK 和 FD_CLOEXEC
    // 无需额外调用 fcntl

    printf("Accepted connection from %s:%d (non-blocking, close-on-exec)\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    close(conn_fd);
    close(listen_fd);
    return 0;
}
```

## 9. accept4 vs accept vs accept with fcntl

| 方式 | 系统调用次数 | 特征 |
|------|-------------|------|
| `accept() + fcntl(F_SETFL, O_NONBLOCK)` | 2 次 | 存在竞态条件（非阻塞设置在连接之后） |
| `accept4(fd, ..., SOCK_NONBLOCK)` | 1 次 | 原子设置，无竞态，Linux 2.6.28+ |

**竞态条件说明：** 使用 `accept` + `fcntl` 设置非阻塞时，在 accept 返回和 fcntl 执行之间，可能有其他代码（如继承自父进程的 fd）访问该套接字，导致意外阻塞。`accept4` 解决了这个问题。

## 10. 线程安全与并发

- `accept`/`accept4` 是线程安全的
- 多个线程可以在同一个监听套接字上同时调用 accept，内核保证每个连接只被一个线程获取
- 这种模式称为 **惊群（thundering herd）** 的经典场景，现代内核通过 `SO_REUSEPORT` 和 `EPOLLEXCLUSIVE` 进行了优化

## 11. 性能考虑

- `accept4` 比 `accept` + `fcntl` 少一次系统调用
- 新的套接字继承监听套接字的 `f_flags`（通过 `arg->flags |= sock->file->f_flags`）
- `sock_alloc_file` 内部使用 `alloc_file_pseudo`，属于较重的分配操作

## 12. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`、`net/ipv4/inet_connection_sock.c`
- `man 2 accept4`
- Linux 2.6.28 引入，glibc 2.10+