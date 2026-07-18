# shutdown 系统调用分析

## 1. 概述

`shutdown` 系统调用关闭套接字的部分或全部连接通道。与 `close` 不同，`shutdown` 不销毁套接字描述符，而是只关闭数据发送/接收通道，且不依赖于文件描述符引用计数。

**原型：**

```c
#include <sys/socket.h>

int shutdown(int sockfd, int how);
```

**参数：**
- `sockfd`：套接字文件描述符
- `how`：关闭方式：
  - `SHUT_RD` (0)：关闭读通道
  - `SHUT_WR` (1)：关闭写通道
  - `SHUT_RDWR` (2)：关闭读写通道

**返回值：**
- 成功：返回 0
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2451
SYSCALL_DEFINE2(shutdown, int, fd, int, how)
{
    return __sys_shutdown(fd, how);
}
```

## 3. 详细的函数调用链

```
shutdown (系统调用入口)
└── __sys_shutdown(fd, how)  [net/socket.c:2437]
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    └── return __sys_shutdown_sock(sock, how)  [net/socket.c:2426]
        ├── err = security_socket_shutdown(sock, how)  → LSM 检查
        └── if (!err)
            err = READ_ONCE(sock->ops)->shutdown(sock, how)  → 多态分发
                │
                └── inet_shutdown(sock, how)  [net/ipv4/af_inet.c:905]
                    ├── sk = sock->sk
                    │
                    ├── how++  → 内核巧妙转换:
                    │   │   SHUT_RD(0) → 1
                    │   │   SHUT_WR(1) → 2
                    │   │   SHUT_RDWR(2) → 3
                    │   │   (映射后: bit 0 = 读, bit 1 = 写)
                    │   │
                    │   └── if ((how & ~SHUTDOWN_MASK) || !how) return -EINVAL
                    │       → SHUTDOWN_MASK = 3，超出范围则无效
                    │
                    ├── lock_sock(sk)
                    │
                    ├── if (sock->state == SS_CONNECTING):
                    │   if (TCPF_SYN_SENT|TCPF_SYN_RECV|TCPF_CLOSE & (1 << sk->sk_state))
                    │       sock->state = SS_DISCONNECTING
                    │   else
                    │       sock->state = SS_CONNECTED
                    │
                    ├── switch (sk->sk_state):
                    │   ├── case TCP_CLOSE:
                    │   │   err = -ENOTCONN
                    │   │   fallthrough  → 仍执行默认操作
                    │   │
                    │   └── default:
                    │       WRITE_ONCE(sk->sk_shutdown, sk->sk_shutdown | how)
                    │       → 设置 sk_shutdown 标志位
                    │
                    ├── release_sock(sk)
                    │
                    └── if (sk->sk_prot->shutdown):
                        └── sk->sk_prot->shutdown(sk, how)
                            → 协议层关闭处理
                            │
                            ├── tcp_shutdown(sk, how)  [net/ipv4/tcp.c]
                            │   ├── if (how & SEND_SHUTDOWN):
                            │   │   └── tcp_send_fin(sk)  → 发送 FIN 包
                            │   │       [net/ipv4/tcp_output.c]
                            │   │       ├── skb = tcp_write_queue_tail(sk)
                            │   │       ├── TCP_SKB_CB(skb)->tcp_flags |= TCPHDR_FIN
                            │   │       └── __tcp_transmit_skb(sk, skb, ...)
                            │   │           → 发送 FIN 包
                            │   │
                            │   └── if (how & RCV_SHUTDOWN):
                            │       └── sk->sk_shutdown |= RCV_SHUTDOWN
                            │       └── tcp_done(sk)  → 可能重置连接
                            │
                            └── udp_shutdown(sk, how)  [net/ipv4/udp.c]
                                ├── 仅设置 sk->sk_shutdown 标志
                                └── 不发送任何网络包（UDP 无连接）
```

## 4. how 参数的内核映射

```c
// include/linux/net.h
#define SHUTDOWN_MASK 3

// 内核中的转换逻辑
// net/ipv4/af_inet.c:913
how++;  // 用户态 → 内核态映射:
        // SHUT_RD (0) → 1  (bit 0: RCV_SHUTDOWN)
        // SHUT_WR (1) → 2  (bit 1: SEND_SHUTDOWN)
        // SHUT_RDWR (2) → 3 (bit 0 + bit 1)

// include/linux/net.h
#define RCV_SHUTDOWN  1  // bit 0 - 关闭读
#define SEND_SHUTDOWN 2  // bit 1 - 关闭写
```

## 5. 关键数据结构

### sk_shutdown 标志位

```c
// include/net/sock.h
struct sock {
    // ...
    int sk_shutdown;  // 使用位掩码:
                      // bit 0 (RCV_SHUTDOWN): 读已关闭
                      // bit 1 (SEND_SHUTDOWN): 写已关闭
    // ...
};
```

## 6. 流程图

```
用户态: shutdown(fd, how)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE2(shutdown)          │  net/socket.c:2451
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_shutdown()                   │  net/socket.c:2437
   │  ├─ CLASS(fd) → 获取 file           │
   │  ├─ sock_from_file() → 获取 socket  │
   │  └─ __sys_shutdown_sock()           │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_shutdown_sock()              │  net/socket.c:2426
   │  ├─ security_socket_shutdown()      │
   │  └─ ops->shutdown() → 多态分发      │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_shutdown()                    │  net/ipv4/af_inet.c:905
   │  ├─ how++ (0→1, 1→2, 2→3)          │
   │  ├─ lock_sock(sk)                   │
   │  ├─ sk->sk_shutdown |= how          │
   │  └─ release_sock(sk)               │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  sk->sk_prot->shutdown()            │
   │  ├─ TCP: tcp_shutdown()             │
   │  │  ├─ SHUT_WR → tcp_send_fin(FIN)  │
   │  │  └─ SHUT_RD → 丢弃接收数据      │
   │  │                                  │
   │  └─ UDP: udp_shutdown()             │
   │     └─ 仅设置标志位，不发包         │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 0 (成功)                    │
   └─────────────────────────────────────┘
```

## 7. shutdown vs close

| 特性 | shutdown | close |
|------|---------|-------|
| 选择关闭方向 | 支持（RD/WR/RDWR） | 只能完全关闭 |
| 释放文件描述符 | 否 | 是 |
| 引用计数 | 不依赖 | 依赖（多进程共享时） |
| 发送 FIN/RST | 根据 how 参数 | 根据 SO_LINGER |
| 对已关闭的 fd 操作 | 可以 | 不能再操作 |
| 多进程/多线程 | 影响所有共享者 | 只影响当前引用 |

### 典型场景

```
场景 1: 半关闭（TCP 的 EOF 语义）
    shutdown(fd, SHUT_WR)  → 发送 FIN，表示"我写完了"
    read(fd, ...)          → 仍可读取对端数据
    close(fd)              → 最终释放

场景 2: 关闭读但保留写
    shutdown(fd, SHUT_RD)  → 丢弃所有接收数据
    write(fd, ...)         → 仍可发送数据

场景 3: 彻底关闭
    shutdown(fd, SHUT_RDWR) → 关闭读写，但不释放 fd
    close(fd)               → 释放文件描述符
```

## 8. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EINVAL` | 无效参数 | `how` 取值无效（不是 0, 1, 2） |
| `ENOTCONN` | 未连接 | 套接字未处于已连接状态 |
| `EACCES` | 权限不足 | LSM 拒绝 shutdown 操作 |

## 9. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

int main() {
    int sockfd;
    struct sockaddr_in addr;
    char buf[1024];
    ssize_t n;

    // 创建连接
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    inet_pton(AF_INET, "93.184.216.34", &addr.sin_addr);
    connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));

    // 发送 HTTP 请求
    const char *req = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    write(sockfd, req, strlen(req));

    // 半关闭写通道：发送 FIN，表示请求已发送完毕
    shutdown(sockfd, SHUT_WR);
    // 此时服务器会收到 EOF，开始发送响应

    // 仍可读取响应
    while ((n = read(sockfd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, n);
    }

    // 最终关闭
    close(sockfd);
    return 0;
}
```

## 10. TCP 四次挥手中的 shutdown

```
客户端主动关闭写通道 (SHUT_WR):

客户端                             服务器
  │                                  │
  │────── FIN (seq=x) ──────────────▶│  (1) tcp_send_fin()
  │◀───── ACK (ack=x+1) ────────────│  (2) FIN_WAIT1 → FIN_WAIT2
  │◀───── FIN (seq=y) ──────────────│  (3) 服务器发送 FIN
  │────── ACK (ack=y+1) ────────────▶│  (4) TIME_WAIT → CLOSE
  │                                  │

shutdown(fd, SHUT_WR) 触发步骤 (1)
shutdown(fd, SHUT_RD) 不发送 FIN，只丢弃接收数据
```

## 11. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`、`net/ipv4/tcp.c`
- `man 2 shutdown`
- `man 7 tcp` — TCP 半关闭说明