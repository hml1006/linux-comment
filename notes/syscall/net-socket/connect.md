# connect 系统调用分析

## 1. 概述

`connect` 系统调用将套接字连接到指定的远程地址。对于 TCP（流式套接字），`connect` 触发三次握手过程；对于 UDP（数据报套接字），`connect` 只是设置默认的目标地址，用于后续的 `send`/`recv` 操作。

**原型：**

```c
#include <sys/socket.h>

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

**参数：**
- `sockfd`：套接字文件描述符
- `addr`：指向目标地址的指针
- `addrlen`：地址结构的长度

**返回值：**
- 成功：返回 0
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2111
SYSCALL_DEFINE3(connect, int, fd, struct sockaddr __user *, uservaddr,
        int, addrlen)
{
    return __sys_connect(fd, uservaddr, addrlen);
}
```

## 3. 详细的函数调用链

### 3.1 TCP (SOCK_STREAM) 路径

```
connect (系统调用入口)
└── __sys_connect(fd, uservaddr, addrlen)  [net/socket.c:2095]
    ├── CLASS(fd, f)(fd)  → 获取 struct file
    ├── if (fd_empty(f)) return -EBADF
    ├── ret = move_addr_to_kernel(uservaddr, addrlen, &address)  → 复制地址
    └── return __sys_connect_file(fd_file(f), &address, addrlen, 0)  [net/socket.c:2072]
        ├── sock = sock_from_file(file)  → 获取 struct socket
        ├── if (!sock) return -ENOTSOCK
        ├── security_socket_connect(sock, (struct sockaddr *)address, addrlen)  → LSM
        └── err = READ_ONCE(sock->ops)->connect(sock, addr, addrlen, file_flags)
            │
            └── inet_stream_connect(sock, uaddr, addr_len, flags)  [net/ipv4/af_inet.c:750]
                ├── lock_sock(sock->sk)  → 获取 sock 锁
                └── __inet_stream_connect(sock, uaddr, addr_len, flags, 0)  [net/ipv4/af_inet.c]
                    ├── 检查 sock->state 状态
                    ├── 若已连接或正在连接中，返回对应错误
                    ├── sock->state = SS_CONNECTING
                    │
                    ├── sk->sk_prot->connect(sk, uaddr, addr_len)  → 二级分发
                    │   └── tcp_v4_connect(sk, uaddr, addr_len)  [net/ipv4/tcp_ipv4.c]
                    │       ├── inet = inet_sk(sk)
                    │       ├── 设置 daddr (目标 IP) 和 dport (目标端口)
                    │       ├── tcp_set_state(sk, TCP_SYN_SENT)  → 设置状态
                    │       │
                    │       ├── ip_route_connect(&fl4, ...)  → 路由查找
                    │       │   [net/ipv4/route.c]
                    │       │   ├── 确定源 IP、网络设备、下一跳等
                    │       │   └── 若未指定源 IP，根据路由表自动选择
                    │       │
                    │       ├── sk->sk_dst_cache = &rt->dst  → 缓存路由
                    │       │
                    │       ├── tcp_connect(sk)  [net/ipv4/tcp_output.c]
                    │       │   ├── tcp_connect_init(sk)  → 初始化 TCP 参数
                    │       │   │   ├── 计算 MSS (最大分段大小)
                    │       │   │   ├── 初始化发送/接收序列号
                    │       │   │   ├── 设置 TSQ (TCP Small Queues)
                    │       │   │   └── 设置初始拥塞窗口
                    │       │   │
                    │       │   ├── skb = tcp_stream_alloc_skb(sk, ...)  → 分配 SYN 包
                    │       │   │
                    │       │   ├── tcp_init_nondata_skb(skb, tcp_sequence_number_ns, ...)
                    │       │   │   → 初始化 SYN 包的 TCP 头部
                    │       │   │
                    │       │   ├── tcp_mstamp_refresh(sk)  → 时间戳刷新
                    │       │   │
                    │       │   └── __tcp_transmit_skb(sk, skb, ...)  [net/ipv4/tcp_output.c]
                    │       │       ├── 构建 TCP 头部（源端口、目地端口、序列号等）
                    │       │       ├── 设置 TCP_SKB_CB(skb)->tcp_flags = TCPHDR_SYN
                    │       │       ├── TCP_SKB_CB(skb)->when = tcp_time_stamp_ts(tp)
                    │       │       │
                    │       │       └── ip_queue_xmit(sk, skb, &fl4)  → 进入 IP 层
                    │       │           [net/ipv4/ip_output.c]
                    │       │           ├── ip_local_out(net, sk, skb)
                    │       │           │   ├── __ip_local_out(net, sk, skb)
                    │       │           │   │   └── nf_hook(NFPROTO_IPV4, NF_INET_LOCAL_OUT, ...)
                    │       │           │   │       → Netfilter 钩子
                    │       │           │   └── dst_output(net, sk, skb)
                    │       │           │       └── ip_output(net, sk, skb)
                    │       │           │           ├── NF_HOOK(NF_INET_POST_ROUTING, ...)
                    │       │           │           └── dev_queue_xmit(skb)  → 进入设备层
                    │       │           │               [net/core/dev.c]
                    │       │           │               └── ndo_start_xmit(skb, dev)  → NIC 驱动
                    │       │           └── ...
                    │       │
                    │       └── return 0
                    │
                    ├── 若 connect 返回 -EINPROGRESS:
                    │   └── 非阻塞 connect 场景，connect 系统调用返回 -1/EINPROGRESS
                    │
                    └── release_sock(sock->sk)
                    └── return err
```

### 3.2 UDP (SOCK_DGRAM) 路径

```
connect (系统调用入口)
└── ... → ops->connect()
    └── inet_dgram_connect(sock, uaddr, addr_len, flags)  [net/ipv4/af_inet.c]
        └── sk->sk_prot->connect(sk, uaddr, addr_len)
            └── udp_connect(sk, uaddr, addr_len)  [net/ipv4/udp.c]
                ├── inet = inet_sk(sk)
                ├── 设置 inet->inet_daddr = addr->sin_addr.s_addr
                ├── 设置 inet->inet_dport = addr->sin_port
                ├── sk->sk_state = TCP_ESTABLISHED  (UDP 也被标记为 ESTABLISHED)
                ├── ip_route_connect(...)  → 路由缓存
                └── return 0
```

UDP 的 `connect` 不会发送任何网络包，只是在内核中记录目标地址，后续 `send`/`recv` 可以直接使用。

## 4. 关键数据结构

### struct inet_sock — 保存连接的四元组信息

```c
// include/net/inet_sock.h
struct inet_sock {
    struct sock         sk;
    __be32              inet_saddr;     // 源 IP 地址
    __be32              inet_rcv_saddr; // 接收地址
    __be16              inet_sport;     // 源端口
    __be16              inet_dport;     // 目标端口（connect 设置）
    __be32              inet_daddr;     // 目标 IP 地址（connect 设置）
    __u16               inet_num;       // 源端口（主机序）
    // ...
};
```

### struct tcp_sock — TCP 协议控制块

```c
// include/net/tcp.h
struct tcp_sock {
    struct inet_connection_sock inet_conn;
    // 发送端
    u32     snd_una;        // 已发送但未确认的最小序号
    u32     snd_nxt;        // 下一个要发送的序号
    u32     write_seq;      // 写操作写入的序列号
    // 接收端
    u32     rcv_nxt;        // 期望接收的下一个序号
    u32     copied_seq;     // 已复制到用户空间的序号
    // 拥塞控制
    u32     snd_cwnd;       // 拥塞窗口
    u32     snd_ssthresh;   // 慢启动阈值
    // 时间戳
    u32     tcp_clock_cache; // 缓存的 TCP 时钟
    // ...
};
```

### struct tcp_skb_cb — skb 的 TCP 控制块

```c
// include/net/tcp.h
struct tcp_skb_cb {
    __u32      seq;         // 起始序列号
    __u32      end_seq;     // 结束序列号
    __u32      tcp_tw_isn;  // TIME_WAIT 中的初始序列号
    __u32      when;        // 发送时间戳（用于 RTT 计算和 RTO）
    __u8       tcp_flags;   // TCP 标志位（SYN, ACK, FIN, ...）
    // ...
};
```

## 5. 阻塞 vs 非阻塞 connect

### 阻塞 connect
```
connect(fd, ...)         → 阻塞直到三次握手完成
    ├── 发送 SYN
    ├── 等待 SYN+ACK     ← 阻塞在此
    ├── 发送 ACK
    └── 返回 0 (ESTABLISHED)
```

### 非阻塞 connect
```
fcntl(fd, F_SETFL, O_NONBLOCK);
connect(fd, ...)         → 立即返回
    ├── 发送 SYN
    ├── 返回 -1 / EINPROGRESS  ← 连接进行中
    │
    └── 后续: poll/epoll 监听可写事件
        ├── POLLOUT 触发 → 连接成功
        │   └── getsockopt(fd, SOL_SOCKET, SO_ERROR, ...) 检查错误
        └── POLLERR 触发 → 连接失败
```

## 6. 流程图

```
用户态: connect(sockfd, &addr, addrlen)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE3(connect)           │  net/socket.c:2111
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_connect()                    │  net/socket.c:2095
   │  ├─ move_addr_to_kernel()           │
   │  └─ __sys_connect_file()            │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_connect_file()               │  net/socket.c:2072
   │  ├─ sock_from_file()                │
   │  ├─ security_socket_connect()       │
   │  └─ ops->connect()                  │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  inet_stream_connect()              │  net/ipv4/af_inet.c:750
   │  ├─ lock_sock(sk)                   │
   │  └─ __inet_stream_connect()         │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  tcp_v4_connect()                   │  net/ipv4/tcp_ipv4.c
   │  ├─ 设置目标 IP/端口                │
   │  ├─ tcp_set_state(TCP_SYN_SENT)     │
   │  ├─ ip_route_connect() → 路由查找   │
   │  └─ tcp_connect() → 发送 SYN        │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  tcp_connect()                      │  net/ipv4/tcp_output.c
   │  ├─ tcp_connect_init()              │
   │  ├─ tcp_stream_alloc_skb()          │
   │  ├─ tcp_init_nondata_skb(SYN)       │
   │  └─ __tcp_transmit_skb() → 发 SYN  │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  ip_queue_xmit()                    │  net/ipv4/ip_output.c
   │  ├─ ip_local_out()                  │
   │  │  ├─ __ip_local_out()             │
   │  │  └─ nf_hook(NF_INET_LOCAL_OUT)   │
   │  └─ dst_output() → ip_output()      │
   │     └─ dev_queue_xmit() → NIC       │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  等待 SYN+ACK（阻塞或非阻塞）       │
   │  收到后 → tcp_rcv_synsent_state_process()
   │  → 建立连接 → ESTABLISHED           │
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
| `EINVAL` | 无效参数 | 地址长度错误或套接字已连接 |
| `EISCONN` | 已连接 | 套接字已经处于连接状态 |
| `ECONNREFUSED` | 连接被拒绝 | 目标端口无监听服务 |
| `ETIMEDOUT` | 连接超时 | SYN 重传超时 |
| `EHOSTUNREACH` | 主机不可达 | 路由不可达或 ARP 解析失败 |
| `ENETUNREACH` | 网络不可达 | 无路由到目标网络 |
| `EADDRNOTAVAIL` | 地址不可用 | 无法分配本地地址/端口 |
| `EAFNOSUPPORT` | 地址族不支持 | `sin_family` 不是 `AF_INET` |
| `EINPROGRESS` | 进行中 | 非阻塞模式，连接正在建立 |
| `EALREADY` | 操作已在进行 | 非阻塞模式下，上一个 connect 仍未完成 |
| `EACCES` | 权限不足 | 防火墙规则禁止连接 |
| `EPERM` | 操作不允许 | BPF 或 SELinux 禁止连接 |
| `EFAULT` | 地址指针无效 | `uservaddr` 指向不可访问的区域 |

## 8. 使用示例

### TCP 阻塞连接

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>

int main() {
    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(80);
    inet_pton(AF_INET, "93.184.216.34", &server_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("connect");
        exit(1);
    }

    printf("Connected to server\n");
    // 使用 sockfd 进行通信...
    close(sockfd);
    return 0;
}
```

### TCP 非阻塞连接

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/poll.h>

int main() {
    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    inet_pton(AF_INET, "93.184.216.34", &addr.sin_addr);

    int ret = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        perror("connect"); exit(1);
    }

    if (ret == 0) {
        printf("Connected immediately\n");
    } else {
        // EINPROGRESS - 连接正在进行
        struct pollfd pfd = { .fd = sockfd, .events = POLLOUT };
        poll(&pfd, 1, 5000);  // 等待 5 秒

        int err;
        socklen_t errlen = sizeof(err);
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err == 0) {
            printf("Connected successfully (non-blocking)\n");
        } else {
            printf("Connection failed: %s\n", strerror(err));
        }
    }

    close(sockfd);
    return 0;
}
```

## 9. connect 与相关系统调用的关系

- **connect / bind**: connect 连接远端，bind 绑定本地
- **connect / accept**: connect 是客户端发起连接，accept 是服务器接受连接
- **connect / sendto**: 对于 UDP，connect 设置默认目标地址（类似 sendto 的地址参数），后续可用 send 替代 sendto
- **connect / shutdown**: shutdown 可以部分关闭已建立的连接（SHUT_RD, SHUT_WR, SHUT_RDWR）

## 10. 性能注意事项

1. **TCP 连接建立开销**：一次 connect 涉及三次握手，至少 1 个 RTT（往返时间），在广域网中可能达到数百毫秒
2. **非阻塞 connect**：避免阻塞线程，适合事件驱动架构
3. **连接池**：频繁建立/关闭连接开销大，建议使用连接池复用
4. **TCP_FASTOPEN**：Linux 3.7+ 支持 TFO（TCP Fast Open），可在 SYN 中携带数据，减少 1 个 RTT

## 11. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`、`net/ipv4/af_inet.c`、`net/ipv4/tcp_ipv4.c`、`net/ipv4/tcp_output.c`
- `man 2 connect`
- RFC 793 (TCP)