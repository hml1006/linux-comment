# accept 系统调用分析

## 1. 概述

`accept` 系统调用从监听套接字的已完成连接队列中取出第一个连接，创建一个新的已连接套接字，并返回其文件描述符。

**原型：**

```c
#include <sys/socket.h>

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
```

**参数：**
- `sockfd`：监听套接字的文件描述符（已通过 `listen` 进入监听状态）
- `addr`：指向 `struct sockaddr` 的指针，用于接收对端地址（可为 NULL）
- `addrlen`：值-结果参数，调用时指向 `addr` 缓冲区大小，返回时指示实际地址长度

**返回值：**
- 成功：返回新的已连接套接字文件描述符（非负整数）
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

`accept` 在 `net/socket.c` 中定义为 `SYSCALL_DEFINE3(accept)`，实际实现与 `accept4` 共享核心逻辑，但 flags 固定为 0：

```c
// net/socket.c:2054
SYSCALL_DEFINE3(accept, int, fd, struct sockaddr __user *, upeer_sockaddr,
		int __user *, upeer_addrlen)
{
	return __sys_accept4(fd, upeer_sockaddr, upeer_addrlen, 0);
}
```

## 3. 详细的函数调用链

```
accept (系统调用入口)
└── __sys_accept4(fd, upeer_sockaddr, upeer_addrlen, 0)    [net/socket.c:2037]
    └── CLASS(fd, f)(fd)  → 获取 fd 对应的 struct file
    └── __sys_accept4_file(file, upeer_sockaddr, upeer_addrlen, 0)  [net/socket.c:2011]
        └── 校验 flags（accept 传入 0 总是合法）
        └── FD_ADD(flags, do_accept(file, &arg, ...))  [net/socket.c:2022]
            └── do_accept(file, &arg, upeer_sockaddr, upeer_addrlen, flags)  [net/socket.c:1951]
                ├── sock_from_file(file) → 获取 struct socket
                ├── sock_alloc() → 分配新的 struct socket (newsock)
                ├── newsock->type = sock->type
                ├── newsock->ops = READ_ONCE(sock->ops)  → 继承 proto_ops
                ├── __module_get(ops->owner)
                ├── sock_alloc_file(newsock, flags, ...) → 创建新 file 结构
                ├── security_socket_accept(sock, newsock)  → LSM 检查
                ├── ops->accept(sock, newsock, &arg)      → 多态分发
                │   └── inet_accept(sock, newsock, &arg)  [net/ipv4/af_inet.c:788]
                │       ├── sk2 = sk1->sk_prot->accept(sk1, &arg)
                │       │   └── inet_csk_accept(sk, &arg)  [net/ipv4/inet_connection_sock.c]
                │       │       ├── lock_sock(sk)
                │       │       ├── skb = skb_dequeue(&sk->sk_receive_queue)  → 取已完成连接
                │       │       ├── 若队列为空且非阻塞，返回 -EAGAIN
                │       │       ├── 若队列为空且阻塞，等待条件变量
                │       │       ├── newsk = skb->sk  → 获取子连接 sock
                │       │       ├── release_sock(sk)
                │       │       └── return newsk
                │       ├── lock_sock(sk2)
                │       ├── __inet_accept(sock, newsock, sk2)  [net/ipv4/af_inet.c:762]
                │       │   ├── sock_graft(newsk, newsock)  → 将 newsk 挂接到 newsock
                │       │   └── newsock->state = SS_CONNECTED
                │       └── release_sock(sk2)
                │       └── return 0
                ├── 若 upeer_sockaddr 非 NULL:
                │   ├── ops->getname(newsock, &address, 2)  → 获取对端地址
                │   │   └── inet_getname(sock, &address, 2)  [net/ipv4/af_inet.c:809]
                │   └── move_addr_to_user(&address, len, upeer_sockaddr, upeer_addrlen)
                └── return newfile
```

## 4. accept() vs accept4() 的区别

| 特性 | accept | accept4 |
|------|--------|---------|
| flags 参数 | 无（固定 0） | 支持 `SOCK_NONBLOCK` 和 `SOCK_CLOEXEC` |
| 内部实现 | `__sys_accept4(fd, ..., 0)` | `__sys_accept4(fd, ..., flags)` |
| 标准来源 | POSIX.1-2001 | POSIX.1-2008, Linux 2.6.28+ |
| 非阻塞设置 | 需额外 `fcntl(fd, F_SETFL, O_NONBLOCK)` | 可直接通过 flags 设置 |

## 5. 关键数据结构

### struct socket — 通用套接字抽象

```c
// include/linux/net.h:116
struct socket {
    socket_state        state;    // SS_UNCONNECTED, SS_CONNECTED, SS_DISCONNECTING, ...
    short               type;     // SOCK_STREAM, SOCK_DGRAM, SOCK_RAW, ...
    unsigned long       flags;    // SOCK_NOSPACE, SOCK_PASSCRED, ...
    struct file         *file;    // 对应的文件结构（通过 sock_map_fd 关联）
    struct sock         *sk;      // 底层协议无关的 sock 结构
    const struct proto_ops *ops;  // 协议操作表（多态分发关键）
    struct socket_wq    wq;       // 等待队列
};
```

### struct proto_ops — 协议操作表（多态接口）

```c
// include/linux/net.h:160
struct proto_ops {
    int     family;
    struct module *owner;
    int     (*release)  (struct socket *sock);
    int     (*bind)     (struct socket *sock, struct sockaddr_unsized *myaddr, int sockaddr_len);
    int     (*connect)  (struct socket *sock, struct sockaddr_unsized *vaddr, int sockaddr_len, int flags);
    int     (*socketpair)(struct socket *sock1, struct socket *sock2);
    int     (*accept)   (struct socket *sock, struct socket *newsock, struct proto_accept_arg *arg);
    int     (*getname)  (struct socket *sock, struct sockaddr *addr, int peer);
    __poll_t (*poll)    (struct file *file, struct socket *sock, struct poll_table_struct *wait);
    int     (*ioctl)    (struct socket *sock, unsigned int cmd, unsigned long arg);
    int     (*listen)   (struct socket *sock, int len);
    int     (*shutdown) (struct socket *sock, int flags);
    int     (*setsockopt)(struct socket *sock, int level, int optname, sockptr_t optval, unsigned int optlen);
    int     (*getsockopt)(struct socket *sock, int level, int optname, char __user *optval, int __user *optlen);
    int     (*sendmsg)  (struct socket *sock, struct msghdr *m, size_t total_len);
    int     (*recvmsg)  (struct socket *sock, struct msghdr *m, size_t total_len, int flags);
    // ... 更多方法
};
```

### inet_stream_ops — TCP 的 proto_ops 实例

```c
// net/ipv4/af_inet.c:1066
const struct proto_ops inet_stream_ops = {
    .family     = PF_INET,
    .owner      = THIS_MODULE,
    .release    = inet_release,
    .bind       = inet_bind,
    .connect    = inet_stream_connect,
    .socketpair = sock_no_socketpair,  // TCP 不支持 socketpair
    .accept     = inet_accept,
    .getname    = inet_getname,
    .poll       = tcp_poll,
    .listen     = inet_listen,
    .shutdown   = inet_shutdown,
    .setsockopt = sock_common_setsockopt,
    .getsockopt = sock_common_getsockopt,
    .sendmsg    = inet_sendmsg,
    .recvmsg    = inet_recvmsg,
    // ...
};
```

### struct proto_accept_arg — accept 参数传递结构

```c
// include/linux/net.h 中定义
struct proto_accept_arg {
    int flags;    // 接受标志
    int err;      // 错误码（由协议层设置）
};
```

## 6. 流程图

```
用户态: accept(sockfd, &addr, &addrlen)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE3(accept)            │  net/socket.c:2054
   │  调用 __sys_accept4(fd, ..., 0)     │
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
   │  校验 flags                         │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  do_accept()                        │  net/socket.c:1951
   │  1. sock_from_file() → 获取 socket  │
   │  2. sock_alloc() → 分配 newsock     │
   │  3. 继承 ops 和 type                │
   │  4. sock_alloc_file() → 创建新 file  │
   │  5. security_socket_accept() LSM    │
   │  6. ops->accept() → 协议层处理      │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_accept()                      │  net/ipv4/af_inet.c:788
   │  sk1->sk_prot->accept(sk1, &arg)    │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_csk_accept()                  │  net/ipv4/inet_connection_sock.c
   │  从 sk_receive_queue 取出已完成连接 │
   │  (三次握手已完成)                    │
   │  返回 newsk (struct sock *)         │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __inet_accept()                    │  net/ipv4/af_inet.c:762
   │  sock_graft(newsk, newsock)         │
   │  newsock->state = SS_CONNECTED      │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  获取对端地址（可选）               │
   │  ops->getname(newsock, &addr, 2)    │
   │  move_addr_to_user() → 复制到用户   │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  返回新的文件描述符                 │
   │  FD_ADD() → 返回 fd                 │
   └─────────────────────────────────────┘
                │
                ▼
用户态: 收到新的已连接套接字 fd
```

## 7. 三次握手与 accept 的关系

```
客户端                          服务器
  │                               │
  │────── SYN (seq=x) ──────────▶ │  (1) 服务器收到 SYN，进入 SYN_RECV
  │◀──── SYN+ACK (seq=y, ack=x+1)─│  (2) 服务器回复 SYN+ACK
  │────── ACK (ack=y+1) ────────▶ │  (3) 服务器收到 ACK，进入 ESTABLISHED
  │                               │     将连接放入 sk_receive_queue
  │                               │  (4) accept() 从队列中取出连接
  │                               │     创建新的 fd 返回给用户
```

**关键点：** `accept` 不参与三次握手过程。三次握手由 TCP 协议栈在内核中自动完成（通过 `tcp_v4_do_rcv` → `tcp_rcv_state_process` → `tcp_child_process`）。`accept` 只是从已完成连接队列中取出结果。

## 8. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | `sockfd` 指向的不是套接字 |
| `EOPNOTSUPP` | 操作不支持 | 该套接字类型不支持 accept（如 SOCK_DGRAM） |
| `EINVAL` | 无效参数 | 套接字未进入监听状态 |
| `EMFILE` | 进程文件描述符表满 | 达到 `RLIMIT_NOFILE` 限制 |
| `ENFILE` | 系统文件表满 | 系统级文件描述符上限 |
| `ENOMEM` | 内存不足 | 无法分配新的 socket/file 结构 |
| `EAGAIN` | 资源暂时不可用 | 非阻塞模式且无已完成连接 |
| `ECONNABORTED` | 连接已中止 | 获取地址时连接已断开 |
| `EINTR` | 被信号中断 | 阻塞等待时收到信号 |
| `EPROTO` | 协议错误 | 底层协议错误 |

## 9. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    int listen_fd, conn_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 创建监听套接字
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);

    // 绑定并监听
    bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(listen_fd, 10);

    printf("Server listening on port 8080...\n");

    // 接受连接
    conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (conn_fd < 0) { perror("accept"); exit(1); }

    printf("Accepted connection from %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // 使用 conn_fd 进行通信...
    close(conn_fd);
    close(listen_fd);
    return 0;
}
```

## 10. 与其他系统调用的关系

- **accept / accept4**: accept4 是 accept 的超集，支持额外 flags
- **accept / connect**: accept 是服务器端接受连接，connect 是客户端发起连接
- **accept / listen**: listen 设置监听队列大小，accept 从队列中取出连接
- **accept / poll / epoll**: 可以用 IO 多路复用监听 accept 的可读事件，当有已完成连接时，监听套接字变为可读

## 11. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`、`net/ipv4/inet_connection_sock.c`
- `man 2 accept`
- POSIX.1-2001, POSIX.1-2008