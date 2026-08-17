# socketpair 系统调用分析

## 1. 概述

`socketpair` 系统调用创建一对已连接的匿名套接字。这对套接字可以用于进程间通信（IPC），类似于 `pipe`，但支持双向通信和多种协议族。

**原型：**

```c
#include <sys/socket.h>

int socketpair(int domain, int type, int protocol, int sv[2]);
```

**参数：**
- `domain`：协议域（通常为 `AF_UNIX` 或 `AF_LOCAL`）
- `type`：套接字类型（`SOCK_STREAM`、`SOCK_DGRAM` 等）
- `protocol`：协议（通常为 0）
- `sv`：输出参数，返回两个文件描述符 `sv[0]` 和 `sv[1]`

**返回值：**
- 成功：返回 0
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:1860
SYSCALL_DEFINE4(socketpair, int, family, int, type, int, protocol,
        int __user *, usockvec)
{
    return __sys_socketpair(family, type, protocol, usockvec);
}
```

## 3. 详细的函数调用链

```
socketpair (系统调用入口)
└── __sys_socketpair(family, type, protocol, usockvec)  [net/socket.c:1768]
    ├── flags = type & ~SOCK_TYPE_MASK  → 提取 SOCK_NONBLOCK / SOCK_CLOEXEC
    │
    ├── if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) return -EINVAL
    │
    ├── type &= SOCK_TYPE_MASK  → 去掉 flags 位
    │
    ├── if (SOCK_NONBLOCK != O_NONBLOCK && (flags & SOCK_NONBLOCK))
    │   flags = (flags & ~SOCK_NONBLOCK) | O_NONBLOCK  → 兼容转换
    │
    ├── fd1 = get_unused_fd_flags(flags)  → 分配第一个 fd
    │   if (fd1 < 0) return fd1
    │
    ├── fd2 = get_unused_fd_flags(flags)  → 分配第二个 fd
    │   if (fd2 < 0) { put_unused_fd(fd1); return fd2; }
    │
    ├── put_user(fd1, &usockvec[0])  → 写入用户空间数组
    ├── put_user(fd2, &usockvec[1])  → 写入用户空间数组
    │
    ├── err = sock_create(family, type, protocol, &sock1)  → 创建第一个 socket
    │   → 同 socket() 系统调用的 sock_create
    │
    ├── err = sock_create(family, type, protocol, &sock2)  → 创建第二个 socket
    │
    ├── err = security_socket_socketpair(sock1, sock2)  → LSM 检查
    │
    ├── err = READ_ONCE(sock1->ops)->socketpair(sock1, sock2)  → 多态分发
    │   │
    │   ├── AF_UNIX: unix_socketpair(sock1, sock2)
    │   │   [net/unix/af_unix.c]
    │   │   ├── 将两个 socket 互相连接
    │   │   ├── sock1->sk 和 sock2->sk 建立双向连接
    │   │   └── 不需要网络通信
    │   │
    │   └── AF_INET: sock_no_socketpair(sock1, sock2)
    │       → 返回 -EOPNOTSUPP（INET 不支持 socketpair）
    │
    ├── newfile1 = sock_alloc_file(sock1, flags, NULL)  → 创建 file 结构
    │
    ├── newfile2 = sock_alloc_file(sock2, flags, NULL)  → 创建 file 结构
    │
    ├── fd_install(fd1, newfile1)  → 安装第一个文件描述符
    ├── fd_install(fd2, newfile2)  → 安装第二个文件描述符
    │
    └── return 0

out:  → 错误处理
    ├── put_unused_fd(fd2)
    ├── put_unused_fd(fd1)
    └── return err
```

## 4. AF_UNIX 的 socketpair 实现

```c
// net/unix/af_unix.c
static int unix_socketpair(struct socket *socka, struct socket *sockb)
{
    struct sock *ska = socka->sk, *skb = sockb->sk;

    // 互相连接两个 socket
    // ska 的 peer 指向 skb
    // skb 的 peer 指向 ska
    ska->sk_peer = skb;
    skb->sk_peer = ska;

    // 设置连接状态
    ska->sk_state = TCP_ESTABLISHED;
    skb->sk_state = TCP_ESTABLISHED;

    // 设置 socket 状态
    socka->state = SS_CONNECTED;
    sockb->state = SS_CONNECTED;

    return 0;
}
```

## 5. 关键数据结构

```c
// 用户空间传递的描述符数组
int sv[2];  // sv[0] 和 sv[1] 是一对已连接的套接字
            // sv[0] 可以读/写 sv[1]
            // sv[1] 可以读/写 sv[0]
```

## 6. 流程图

```
用户态: socketpair(domain, type, protocol, sv)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE4(socketpair)        │  net/socket.c:1860
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_socketpair()                 │  net/socket.c:1768
   │  ├─ 提取 flags，校验                │
   │  ├─ get_unused_fd_flags() × 2       │
   │  ├─ put_user(fd1, &sv[0])           │
   │  ├─ put_user(fd2, &sv[1])           │
   │  ├─ sock_create() × 2               │
   │  ├─ security_socket_socketpair()    │
   │  ├─ ops->socketpair()               │
   │  ├─ sock_alloc_file() × 2           │
   │  ├─ fd_install() × 2                │
   │  └─ return 0                        │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  ops->socketpair()                  │
   │  ├─ AF_UNIX: unix_socketpair()      │
   │  │  ├─ ska->peer = skb              │
   │  │  ├─ skb->peer = ska              │
   │  │  ├─ sk_state = ESTABLISHED       │
   │  │  └─ state = SS_CONNECTED         │
   │  │                                  │
   │  └─ AF_INET: sock_no_socketpair()   │
   │     → return -EOPNOTSUPP            │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 0 (sv[0] 和 sv[1] 已填充)   │
   └─────────────────────────────────────┘
```

## 7. socketpair vs pipe

| 特性 | socketpair | pipe |
|------|-----------|------|
| 通信方向 | 双向 | 单向（需两个 pipe 实现双向） |
| 地址族 | AF_UNIX / AF_INET 等 | 仅本地 |
| 数据类型 | SOCK_STREAM / SOCK_DGRAM | 流式 |
| 复杂度 | 中等 | 简单 |
| 可移植性 | POSIX | POSIX |
| 非阻塞支持 | SOCK_NONBLOCK | 需 fcntl |
| 辅助数据 | 支持 (SCM_RIGHTS 等) | 不支持 |

## 8. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EAFNOSUPPORT` | 地址族不支持 | `domain` 不支持 socketpair（如 AF_INET） |
| `EINVAL` | 无效参数 | `flags` 无效或 `protocol` 无效 |
| `EMFILE` | 进程文件描述符表满 | 达到 `RLIMIT_NOFILE` 限制 |
| `ENFILE` | 系统文件表满 | 系统级文件描述符上限 |
| `ENOMEM` | 内存不足 | 无法分配 socket 结构 |
| `EOPNOTSUPP` | 不支持的操作 | 协议族不支持 socketpair |
| `EPROTONOSUPPORT` | 协议不支持 | `type` 和 `protocol` 组合无效 |
| `EFAULT` | 地址指针无效 | `usockvec` 指向不可访问的区域 |

## 9. 使用示例

```c
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

int main() {
    int sv[2];
    pid_t pid;
    char buf[1024];

    // 创建一对已连接的 UNIX 流套接字
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair"); exit(1);
    }

    pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }

    if (pid == 0) {
        // 子进程
        close(sv[0]);  // 关闭写端

        // 从父进程接收数据
        ssize_t n = read(sv[1], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("Child received: %s\n", buf);
        }

        // 回复父进程
        const char *reply = "Hello from child!";
        write(sv[1], reply, strlen(reply) + 1);

        close(sv[1]);
        exit(0);
    }

    // 父进程
    close(sv[1]);  // 关闭读端

    // 发送数据给子进程
    const char *msg = "Hello from parent!";
    write(sv[0], msg, strlen(msg) + 1);

    // 接收子进程回复
    ssize_t n = read(sv[0], buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("Parent received: %s\n", buf);
    }

    close(sv[0]);
    wait(NULL);
    return 0;
}
```

## 10. socketpair vs socket + connect + bind

| 方式 | 系统调用次数 | 适用场景 |
|------|-------------|----------|
| `socketpair` | 1 次 | 本地进程间通信（强烈推荐） |
| `socket + bind + listen + accept + connect` | 5 次 | 网络通信 |
| `pipe` | 1 次 | 简单单向通信 |

**使用 socketpair 的优势：**
1. 一次系统调用创建一对已连接的套接字
2. 无需显式绑定和监听
3. 支持双向通信
4. 支持 `SOCK_NONBLOCK` 和 `SOCK_CLOEXEC`
5. 支持辅助数据（`SCM_RIGHTS` 传递文件描述符）

## 11. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/unix/af_unix.c`
- `man 2 socketpair`
- `man 7 unix` — UNIX 域套接字