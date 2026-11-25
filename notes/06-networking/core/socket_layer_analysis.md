# Linux 内核 VFS 与 Socket 层分析

## 目录

1. [概述](#1-概述)
2. [VFS Socket 层 (net/socket.c)](#2-vfs-socket-层-netsocketc)
   - 2.1 [socket 文件系统](#21-socket-文件系统)
   - 2.2 [socket 系统调用入口](#22-socket-系统调用入口)
   - 2.3 [socket 创建流程](#23-socket-创建流程)
   - 2.4 [socket 文件操作](#24-socket-文件操作)
3. [Socket 核心层 (net/core/sock.c)](#3-socket-核心层-netcoresockc)
   - 3.1 [socket 分配与释放](#31-socket-分配与释放)
   - 3.2 [socket 锁机制](#32-socket-锁机制)
   - 3.3 [内存管理](#33-内存管理)
   - 3.4 [等待队列与通知机制](#34-等待队列与通知机制)
4. [协议族注册与分派](#4-协议族注册与分派)
   - 4.1 [net_proto_family 注册](#41-net_proto_family-注册)
   - 4.2 [inet_register_protosw](#42-inet_register_protosw)
   - 4.3 [proto_ops 操作向量](#43-proto_ops-操作向量)
5. [关键数据结构与注释](#5-关键数据结构与注释)
   - 5.1 [struct socket](#51-struct-socket)
   - 5.2 [struct sock_common](#52-struct-sock_common)
   - 5.3 [struct sock](#53-struct-sock)
   - 5.4 [struct proto_ops](#54-struct-proto_ops)
   - 5.5 [struct proto](#55-struct-proto)
   - 5.6 [struct inet_sock](#56-struct-inet_sock)
6. [函数调用流程](#6-函数调用流程)
   - 6.1 [socket() → 创建完整的调用链](#61-socket--创建完整的调用链)
   - 6.2 [send/recv 完整调用链](#62-sendrecv-完整调用链)
   - 6.3 [bind/listen/accept 调用链](#63-bindlistenaccept-调用链)

---

## 1. 概述

Socket 层是 Linux 网络子系统的顶层接口，连接用户空间的系统调用和内核协议栈。它分为两层：

- **VFS Socket 层** (`net/socket.c`)：处理系统调用入口、VFS 文件操作绑定、socket 生命周期管理
- **Socket 核心层** (`net/core/sock.c`)：协议无关的 socket 内部表示、内存管理、锁机制、等待队列

---

## 2. VFS Socket 层 (net/socket.c)

`net/socket.c` 实现了 BSD socket 系统调用与 VFS 的集成，是用户态网络编程的入口。

### 2.1 socket 文件系统

Socket 在 VFS 中表现为特殊文件，通过伪文件系统 `sockfs` 管理：

```c
// net/socket.c: socket_file_ops
static const struct file_operations socket_file_ops = {
    .owner =        THIS_MODULE,
    .read_iter =    sock_read_iter,     // 映射到 recvmsg
    .write_iter =   sock_write_iter,    // 映射到 sendmsg
    .poll =         sock_poll,          // 映射到 poll/select/epoll
    .unlocked_ioctl = sock_ioctl,
    .mmap =         sock_mmap,
    .release =      sock_close,         // 映射到 close
    .fasync =       sock_fasync,
    .splice_read =  sock_splice_read,
    .splice_write = splice_to_socket,
    .show_fdinfo =  sock_show_fdinfo,
};
```

**关键设计**：`read/write` 系统调用通过 VFS 的 `file_operations` 映射到 socket 的 `send/recv`，因此用户态可以直接用 `read(fd, buf, len)` 和 `write(fd, buf, len)` 操作 socket。

### 2.2 socket 系统调用入口

核心系统调用函数（均定义在 `net/socket.c`）：

| 系统调用 | 函数 | 行号 | 功能 |
|---------|------|------|------|
| `socket()` | `__sys_socket()` | 1759 | 创建 socket |
| `bind()` | `__sys_bind()` | 1908 | 绑定地址 |
| `listen()` | `__sys_listen()` | 1946 | 监听连接 |
| `accept()` | `__sys_accept4()` | 2054 | 接受连接 |
| `connect()` | `__sys_connect()` | 2111 | 连接 |
| `sendto()` | `__sys_sendto()` | — | 发送数据 |
| `recvfrom()` | `__sys_recvfrom()` | — | 接收数据 |
| `setsockopt()` | `__sys_setsockopt()` | — | 设置选项 |
| `getsockopt()` | `__sys_getsockopt()` | — | 获取选项 |
| `shutdown()` | `__sys_shutdown()` | — | 关闭连接 |
| `close()` | `sock_close()` | — | 释放 socket |

### 2.3 socket 创建流程

```
socket(family, type, protocol)
    │
    ▼
__sys_socket(family, type, protocol)  [net/socket.c:1759]
    │
    ├── sock_alloc()  → 分配 struct socket + inode
    │       │
    │       └── SOCKET_I(inode) 获取 socket 指针
    │
    ├── __sock_create()  [net/socket.c:1500+]
    │       │
    │       ├── net_families[family] 查找协议族
    │       │       │
    │       │       └── 如 PF_INET → inet_family_ops
    │       │
    │       └── pf->create(net, sock, protocol, kern)
    │               │
    │               └── inet_create()  [af_inet.c:260]
    │                       │
    │                       ├── 根据 type+protocol 查找 inetsw 链表
    │                       │       │
    │                       │       ├── SOCK_STREAM + IPPROTO_TCP
    │                       │       │   → answer->ops = inet_stream_ops
    │                       │       │   → answer->prot = &tcp_prot
    │                       │       │
    │                       │       ├── SOCK_DGRAM + IPPROTO_UDP
    │                       │       │   → answer->ops = inet_dgram_ops
    │                       │       │   → answer->prot = &udp_prot
    │                       │       │
    │                       │       └── SOCK_RAW + IPPROTO_IP
    │                       │           → answer->ops = inet_sockraw_ops
    │                       │           → answer->prot = &raw_prot
    │                       │
    │                       ├── sock->ops = answer->ops  // 绑定协议族操作
    │                       │
    │                       └── sk = sk_alloc(net, PF_INET, ...)
    │                               │
    │                               └── sock_init_data(sock, sk)
    │                                       │
    │                                       ├── sk->sk_data_ready = sock_def_readable
    │                                       ├── sk->sk_write_space = sock_def_write_space
    │                                       └── sk->sk_state_change = sock_def_error_report
    │
    └── sock_map_fd(sock, flags & SOCK_CLOEXEC)
            │
            ├── get_unused_fd_flags()  → 分配 fd
            ├── sock_alloc_file(sock, flags)  → 分配 struct file
            │       │
            │       └── file->f_op = &socket_file_ops
            │       └── file->private_data = sock
            │
            └── fd_install(fd, file)  → 安装到进程 fd 表
```

### 2.4 socket 文件操作

`struct file_operations` 中的操作映射到 socket 操作：

```c
// read(fd) 实际上调用 recvmsg
static ssize_t sock_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct socket *sock = iocb->ki_filp->private_data;
    // ... 调用 sock_recvmsg(sock, msg, flags)
}

// write(fd) 实际上调用 sendmsg
static ssize_t sock_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct socket *sock = iocb->ki_filp->private_data;
    // ... 调用 sock_sendmsg(sock, msg)
}
```

---

## 3. Socket 核心层 (net/core/sock.c)

`net/core/sock.c` 提供协议无关的 socket 支持例程。

### 3.1 socket 分配与释放

```c
// 分配 struct sock
struct sock *sk_alloc(struct net *net, int family, gfp_t priority,
                      struct proto *prot, int kern)
{
    struct sock *sk;

    sk = xmalloc_node(prot->obj_size, priority, ...);  // 分配 sock 对象
    sock_net_set(sk, net);                             // 设置网络命名空间
    sk->sk_prot = sk->sk_prot_creator = prot;           // 绑定协议
    refcount_set(&sk->sk_refcnt, 1);                    // 初始引用计数=1

    return sk;
}

// 初始化 socket 数据
void sock_init_data(struct socket *sock, struct sock *sk)
{
    sk->sk_socket = sock;               // 双向关联
    if (sock)
        sock->sk = sk;

    sk->sk_state = TCP_CLOSE;           // 初始状态
    sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;  // 默认接收超时
    sk->sk_sndtimeo = MAX_SCHEDULE_TIMEOUT;  // 默认发送超时

    skb_queue_head_init(&sk->sk_receive_queue);  // 初始化接收队列
    skb_queue_head_init(&sk->sk_write_queue);    // 初始化发送队列
    skb_queue_head_init(&sk->sk_error_queue);    // 初始化错误队列

    sk->sk_data_ready = sock_def_readable;       // 数据就绪回调
    sk->sk_write_space = sock_def_write_space;   // 发送空间就绪回调
    sk->sk_error_report = sock_def_error_report; // 错误报告回调

    // 初始化 socket 锁
    sk->sk_lock.owned = 0;
    spin_lock_init(&sk->sk_lock.slock);
    init_waitqueue_head(&sk->sk_lock.wq);
}
```

### 3.2 socket 锁机制

```c
typedef struct {
    spinlock_t      slock;          // 自旋锁，保护快速路径
    int             owned;          // 是否被用户态持有
    wait_queue_head_t wq;           // 等待队列（锁竞争时等待）
} socket_lock_t;
```

**锁使用模式**：

```
lock_sock(sk)
    ├── spin_lock(&sk->sk_lock.slock)
    ├── while (sk->sk_lock.owned)  // 如果被其他人持有
    │       └── wait on sk->sk_lock.wq
    ├── sk->sk_lock.owned = 1
    └── spin_unlock(&sk->sk_lock.slock)

release_sock(sk)
    ├── spin_lock(&sk->sk_lock.slock)
    ├── sk->sk_lock.owned = 0
    ├── process_backlog(sk)  // 处理 backlog 队列中的包
    ├── wake_up(&sk->sk_lock.wq)
    └── spin_unlock(&sk->sk_lock.slock)
```

**BH (Bottom Half) 上下文锁**：
- `bh_lock_sock(sk)` — 软中断中尝试获取锁
- 软中断中如果 `owned=1`（用户态持有），包被放入 `sk_backlog` 队列
- 用户态释放锁时处理 backlog 队列

### 3.3 内存管理

Socket 有独立的接收/发送缓冲区管理：

```c
// 接收缓冲区大小
sk->sk_rcvbuf = sysctl_rmem_default;  // 默认 212992 字节

// 发送缓冲区大小
sk->sk_sndbuf = sysctl_wmem_default;  // 默认 212992 字节

// 预分配内存
sk->sk_forward_alloc  // 已预分配但未使用的内存

// 接收队列内存
sk_rmem_alloc  // 接收队列中 skb 占用的内存总量

// 发送队列内存
sk_wmem_queued  // 发送队列中 skb 占用的内存总量
sk_wmem_alloc  // 正在 DMA 传输的 skb 引用计数
```

**内存压力管理**：
- 当 `sk_rmem_alloc > sk_rcvbuf` 时，丢弃接收包
- 当 `sk_wmem_queued > sk_sndbuf` 时，写操作阻塞

### 3.4 等待队列与通知机制

```c
// 数据就绪通知路径
tcp_data_ready(sk)  // TCP 层收到数据
    │
    └── sk->sk_data_ready(sk)  // 默认为 sock_def_readable()
            │
            ├── wake_up_interruptible_sync_poll(sk->sk_wq, EPOLLIN)
            │       // 唤醒等待在 poll/select/epoll 上的进程
            │
            └── sock_def_readable() 的额外处理
                    └── sk_wake_async(sk, SOCK_WAKE_WAITD, POLL_IN)
                            // 发送 SIGIO 信号（若设置了 FASYNC）
```

---

## 4. 协议族注册与分派

### 4.1 net_proto_family 注册

```c
// net/socket.c
static const struct net_proto_family __rcu *net_families[NPROTO] __read_mostly;

int sock_register(const struct net_proto_family *ops)
{
    // 将 ops 注册到 net_families[family] 数组
    // 如 PF_INET → net_families[AF_INET] = &inet_family_ops
}

// 创建时查找
pf = rcu_dereference(net_families[family]);
pf->create(net, sock, protocol, kern);
```

### 4.2 inet_register_protosw

```c
// net/ipv4/af_inet.c:1205
void inet_register_protosw(struct inet_protosw *p)
{
    // 将协议类型 + 协议号 → ops/prot 映射注册到 inetsw 链表
    // 如 SOCK_STREAM + IPPROTO_TCP → inet_stream_ops / tcp_prot
}
```

### 4.3 proto_ops 操作向量

`proto_ops` 是协议族层面的操作接口，每个协议族（INET/INET6/UNIX）提供自己的实现：

```c
// IPv4 TCP 流式 socket 操作
const struct proto_ops inet_stream_ops = {
    .family     = PF_INET,
    .release    = inet_release,        // close 系统调用
    .bind       = inet_bind,           // bind 系统调用
    .connect    = inet_stream_connect, // connect 系统调用
    .accept     = inet_accept,         // accept 系统调用
    .listen     = inet_listen,         // listen 系统调用
    .poll       = tcp_poll,            // poll/select/epoll
    .sendmsg    = inet_sendmsg,        // send/sendto/sendmsg
    .recvmsg    = inet_recvmsg,        // recv/recvfrom/recvmsg
    .mmap       = tcp_mmap,            // mmap 零拷贝
    .splice_read = tcp_splice_read,    // splice 零拷贝
};

// IPv4 UDP 数据报 socket 操作
const struct proto_ops inet_dgram_ops = {
    .family     = PF_INET,
    .connect    = inet_dgram_connect,  // UDP 的 connect 仅设置默认目标
    .poll       = udp_poll,
    .sendmsg    = inet_sendmsg,
    .recvmsg    = inet_recvmsg,
    // 无 accept/listen
};
```

---

## 5. 关键数据结构与注释

### 5.1 struct socket

文件：`include/linux/net.h:110`

```c
/**
 * struct socket - general BSD socket
 * @state: socket state (%SS_CONNECTED, etc)
 * @type: socket type (%SOCK_STREAM, etc)
 * @flags: socket flags (%SOCK_NOSPACE, etc)
 * @ops: protocol specific socket operations
 * @file: File back pointer for gc
 * @sk: internal networking protocol agnostic socket representation
 * @wq: wait queue for several uses
 */
struct socket {
    socket_state        state;      // 连接状态
    short               type;       // SOCK_STREAM / SOCK_DGRAM / SOCK_RAW
    unsigned long       flags;      // SOCK_NOSPACE, SOCKWQ_ASYNC_NOSPACE 等
    struct file         *file;      // 关联的 VFS file 结构
    struct sock         *sk;        // 内部协议层 socket 表示
    const struct proto_ops *ops;    // 协议族操作函数表
    struct socket_wq    wq;         // 等待队列
};
```

**socket_state 定义** (`include/uapi/linux/net.h`):
```c
typedef enum {
    SS_FREE = 0,        // 未分配
    SS_UNCONNECTED,     // 未连接
    SS_CONNECTING,      // 连接中
    SS_CONNECTED,       // 已连接
    SS_DISCONNECTING    // 断开中
} socket_state;
```

### 5.2 struct sock_common

文件：`include/net/sock.h:130`

```c
/**
 * struct sock_common - minimal network layer representation of sockets
 * @skc_daddr: Foreign IPv4 addr
 * @skc_rcv_saddr: Bound local IPv4 addr
 * @skc_addrpair: 8-byte-aligned __u64 union of @skc_daddr & @skc_rcv_saddr
 * @skc_hash: hash value used with various protocol lookup tables
 * @skc_u16hashes: two u16 hash values used by UDP lookup tables
 * @skc_dport: placeholder for inet_dport/tw_dport
 * @skc_num: placeholder for inet_num/tw_num
 * @skc_portpair: __u32 union of @skc_dport & @skc_num
 * @skc_family: network address family
 * @skc_state: Connection state
 * @skc_reuse: %SO_REUSEADDR setting
 * @skc_reuseport: %SO_REUSEPORT setting
 * @skc_ipv6only: socket is IPV6 only
 * @skc_net_refcnt: socket is using net ref counting
 * @skc_bound_dev_if: bound device index if != 0
 * @skc_bind_node: bind hash linkage for various protocol lookup tables
 * @skc_prot: protocol handlers inside a network family
 * @skc_net: reference to the network namespace of this socket
 * @skc_v6_daddr: IPV6 destination address
 * @skc_v6_rcv_saddr: IPV6 source address
 * @skc_cookie: socket's cookie value
 * @skc_node: main hash linkage for various protocol lookup tables
 * @skc_nulls_node: main hash linkage for TCP/UDP/UDP-Lite protocol
 * @skc_tx_queue_mapping: tx queue number for this connection
 * @skc_rx_queue_mapping: rx queue number for this connection
 * @skc_flags: place holder for sk_flags
 * @skc_listener: connection request listener socket (aka rsk_listener)
 * @skc_tw_dr: (aka tw_dr) ptr to &struct inet_timewait_death_row
 * @skc_incoming_cpu: record/match cpu processing incoming packets
 * @skc_rcv_wnd: (aka rsk_rcv_wnd) TCP receive window size (possibly scaled)
 * @skc_tw_rcv_nxt: (aka tw_rcv_nxt) TCP window next expected seq number
 * @skc_refcnt: reference count
 *
 * This is the minimal network layer representation of sockets, the header
 * for struct sock and struct inet_timewait_sock.
 */
struct sock_common {
    // 地址对（IPv4 地址 64bit 对齐存储）
    union {
        __addrpair      skc_addrpair;
        struct {
            __be32      skc_daddr;      // 远端 IPv4 地址
            __be32      skc_rcv_saddr;  // 本地绑定 IPv4 地址
        };
    };
    // 哈希值
    union {
        unsigned int    skc_hash;
        __u16           skc_u16hashes[2];  // UDP 双哈希
    };
    // 端口对
    union {
        __portpair      skc_portpair;
        struct {
            __be16      skc_dport;      // 远端端口
            __u16       skc_num;        // 本地端口
        };
    };

    unsigned short      skc_family;         // 协议族 (AF_INET/AF_INET6)
    volatile unsigned char skc_state;        // 连接状态 (TCP_ESTABLISHED 等)
    unsigned char       skc_reuse:4;         // SO_REUSEADDR
    unsigned char       skc_reuseport:1;     // SO_REUSEPORT
    unsigned char       skc_ipv6only:1;
    unsigned char       skc_net_refcnt:1;
    int                 skc_bound_dev_if;    // 绑定的设备索引
    struct proto        *skc_prot;           // 协议处理函数
    possible_net_t      skc_net;             // 网络命名空间
    // ... 其他字段
};
```

### 5.3 struct sock

文件：`include/net/sock.h:360`

详见 [网络子系统概览文档](file:///home/louis/code/linux/notes/network/network_subsystem_analysis.md#52-struct-sock)，此处补充关键说明：

**缓存行分组设计**：`struct sock` 按访问频率和方向将字段分组到不同的缓存行，避免多核竞争时的缓存颠簸：

| 分组 | 方向 | 读/写 | 说明 |
|------|------|-------|------|
| `sock_write_rx` | RX 写 | 接收路径 | 放包到接收队列时写 |
| `sock_read_rx` | RX 读 | 接收路径 | 从接收队列取包时读 |
| `sock_read_rxtx` | 共享 | 双向 | 错误状态、memcg 等 |
| `sock_write_rxtx` | 共享写 | 双向 | socket 锁、预分配内存 |
| `sock_write_tx` | TX 写 | 发送路径 | 发送队列、定时器 |
| `sock_read_tx` | TX 读 | 发送路径 | 路由缓存、发送缓冲区大小 |

### 5.4 struct proto_ops

文件：`include/linux/net.h:160`

```c
struct proto_ops {
    int     family;                         // 协议族编号
    struct module *owner;
    int     (*release)   (struct socket *sock);
    int     (*bind)      (struct socket *sock, struct sockaddr_unsized *myaddr, int sockaddr_len);
    int     (*connect)   (struct socket *sock, struct sockaddr_unsized *vaddr, int sockaddr_len, int flags);
    int     (*socketpair)(struct socket *sock1, struct socket *sock2);
    int     (*accept)    (struct socket *sock, struct socket *newsock, struct proto_accept_arg *arg);
    int     (*getname)   (struct socket *sock, struct sockaddr *addr, int peer);
    __poll_t (*poll)     (struct file *file, struct socket *sock, struct poll_table_struct *wait);
    int     (*ioctl)     (struct socket *sock, unsigned int cmd, unsigned long arg);
    int     (*gettstamp) (struct socket *sock, void __user *userstamp, bool timeval, bool time32);
    int     (*listen)    (struct socket *sock, int len);
    int     (*shutdown)  (struct socket *sock, int flags);
    int     (*setsockopt)(struct socket *sock, int level, int optname, sockptr_t optval, unsigned int optlen);
    int     (*getsockopt)(struct socket *sock, int level, int optname, char __user *optval, int __user *optlen);
    int     (*sendmsg)   (struct socket *sock, struct msghdr *m, size_t total_len);
    int     (*recvmsg)   (struct socket *sock, struct msghdr *m, size_t total_len, int flags);
    int     (*mmap)      (struct file *file, struct socket *sock, struct vm_area_struct *vma);
    ssize_t (*splice_read)(struct socket *sock, loff_t *ppos, struct pipe_inode_info *pipe, size_t len, unsigned int flags);
    // ... 其他方法
};
```

### 5.5 struct proto

文件：`include/net/sock.h`

```c
struct proto {
    void     (*close)(struct sock *sk, long timeout);
    int      (*connect)(struct sock *sk, struct sockaddr *uaddr, int addr_len);
    int      (*disconnect)(struct sock *sk, int flags);
    struct sock *(*accept)(struct sock *sk, struct proto_accept_arg *arg);
    int      (*ioctl)(struct sock *sk, int cmd, int *karg);
    int      (*init)(struct sock *sk);              // socket 初始化
    void     (*destroy)(struct sock *sk);            // socket 销毁
    void     (*shutdown)(struct sock *sk, int how);
    int      (*setsockopt)(struct sock *sk, int level, int optname, sockptr_t optval, unsigned int optlen);
    int      (*getsockopt)(struct sock *sk, int level, int optname, char __user *optval, int __user *optlen);
    int      (*sendmsg)(struct sock *sk, struct msghdr *msg, size_t len);
    int      (*recvmsg)(struct sock *sk, struct msghdr *msg, size_t len, int flags, int *addr_len);
    int      (*backlog_rcv)(struct sock *sk, struct sk_buff *skb);  // backlog 处理
    void     (*release_cb)(struct sock *sk);         // 释放锁时的回调
    void     (*hash)(struct sock *sk);
    void     (*unhash)(struct sock *sk);
    int      (*get_port)(struct sock *sk, unsigned short snum);

    // 内存管理
    void     (*enter_memory_pressure)(struct sock *sk);
    void     (*leave_memory_pressure)(struct sock *sk);
    long     (*stream_memory_free)(const struct sock *sk);

    // 协议统计
    struct percpu_counter *sockets_allocated;
    struct percpu_counter *memory_allocated;
    int      *memory_pressure;

    // 协议配置
    int      sysctl_mem[3];
    int      sysctl_wmem_offset;
    int      sysctl_rmem_offset;
    int      max_header;
    int      obj_size;               // struct sock 子类大小 (如 sizeof(struct tcp_sock))
    int      slab_flags;             // SLAB_TYPESAFE_BY_RCU 等

    // 时间等待和请求 sock 操作
    struct twsk_prot *twsk_prot;
    struct request_sock_ops *rsk_prot;
};
```

**TCP 协议实例** (`net/ipv4/tcp_ipv4.c:3416`):
```c
struct proto tcp_prot = {
    .name           = "TCP",
    .close          = tcp_close,
    .connect        = tcp_v4_connect,
    .accept         = inet_csk_accept,
    .init           = tcp_v4_init_sock,
    .destroy        = tcp_v4_destroy_sock,
    .shutdown       = tcp_shutdown,
    .setsockopt     = tcp_setsockopt,
    .getsockopt     = tcp_getsockopt,
    .recvmsg        = tcp_recvmsg,
    .sendmsg        = tcp_sendmsg,
    .backlog_rcv    = tcp_v4_do_rcv,        // backlog 中的包处理
    .release_cb     = tcp_release_cb,
    .hash           = inet_hash,
    .unhash         = inet_unhash,
    .get_port       = inet_csk_get_port,
    .obj_size       = sizeof(struct tcp_sock),  // TCP sock 对象大小
    .slab_flags     = SLAB_TYPESAFE_BY_RCU,     // RCU 安全回收
    .twsk_prot      = &tcp_timewait_sock_ops,   // TIME_WAIT sock 操作
    .rsk_prot       = &tcp_request_sock_ops,    // 请求 sock 操作
    .no_autobind    = true,                     // TCP 不自动绑定
};
```

### 5.6 struct inet_sock

文件：`include/net/inet_sock.h:218`

```c
/**
 * struct inet_sock - representation of INET sockets
 * @sk - ancestor class
 * @pinet6 - pointer to IPv6 control block
 * @inet_daddr - Foreign IPv4 addr
 * @inet_rcv_saddr - Bound local IPv4 addr
 * @inet_dport - Destination port
 * @inet_num - Local port
 * @inet_flags - various atomic flags
 * @inet_saddr - Sending source
 * @uc_ttl - Unicast TTL
 * @inet_sport - Source port
 * @inet_id - ID counter for DF pkts
 * @tos - TOS
 * @mc_ttl - Multicasting TTL
 * @uc_index - Unicast outgoing device index
 * @mc_index - Multicast device index
 * @mc_list - Group array
 * @cork - info to build ip hdr on each ip frag while socket is corked
 */
struct inet_sock {
    struct sock             sk;             // 继承自 sock (必须是第一个成员)
    struct ipv6_pinfo       *pinet6;        // IPv6 扩展（若启用）
    // 宏快捷访问 sk_common 中的 INET 字段
    #define inet_daddr      sk.__sk_common.skc_daddr
    #define inet_rcv_saddr  sk.__sk_common.skc_rcv_saddr
    #define inet_dport      sk.__sk_common.skc_dport
    #define inet_num        sk.__sk_common.skc_num

    unsigned long           inet_flags;     // INET 标志位 (INET_FLAGS_PKTINFO 等)
    __be32                  inet_saddr;     // 发送源地址
    __s16                   uc_ttl;         // 单播 TTL
    __be16                  inet_sport;     // 源端口
    struct ip_options_rcu __rcu *inet_opt;  // IP 选项
    atomic_t                inet_id;        // IP ID 计数器
    __u8                    tos;            // Type of Service
    __u8                    min_ttl;        // 最小 TTL
    __u8                    mc_ttl;         // 组播 TTL
    __u8                    pmtudisc;       // PMTU 发现策略
    __u8                    rcv_tos;        // 接收 TOS
    int                     uc_index;       // 单播出接口索引
    int                     mc_index;       // 组播出接口索引
    __be32                  mc_addr;        // 组播地址
    u32                     local_port_range; // 本地端口范围 (high << 16 | low)
    struct ip_mc_socklist __rcu *mc_list;   // 组播组成员列表
    struct inet_cork_full   cork;           // IP 分片控制
};
```

**继承层次总结**：
```
struct sock_common      最小公共表示 (与 timewait_sock 共享)
    └── struct sock         通用 socket (锁、队列、缓冲区、回调)
            └── struct inet_sock     INET 扩展 (IP 地址、端口、选项)
                    └── struct inet_connection_sock 面向连接扩展 (重传、拥塞控制、延迟 ACK)
                            └── struct tcp_sock      TCP 协议 (序列号、窗口、SACK、重传队列)
```

---

## 6. 函数调用流程

### 6.1 socket() 创建完整的调用链

```
用户空间                         内核空间
┌─────────┐    ┌─────────────────────────────────────────────────┐
│socket()  │───▶│sys_socket() → __sys_socket()                  │
│AF_INET    │    │   ├── sock_alloc()                            │
│SOCK_STREAM│    │   │   ├── new_inode_pseudo(sock_mnt->mnt_sb) │
│0          │    │   │   └── SOCKET_I(inode)→sock               │
└─────────┘    │   ├── __sock_create()                          │
               │   │   ├── net_families[AF_INET]->create()      │
               │   │   │   └── inet_create()                    │
               │   │   │       ├── lookup inetsw[type]           │
               │   │   │       ├── sock->ops = inet_stream_ops  │
               │   │   │       ├── sk_alloc() → tcp_prot        │
               │   │   │       └── sock_init_data()             │
               │   │   └── sock->ops->owner 模块引用            │
               │   └── sock_map_fd()                            │
               │       ├── sock_alloc_file() → file_operations  │
               │       └── fd_install(fd, file)                 │
               └─────────────────────────────────────────────────┘
```

### 6.2 send/recv 完整调用链

**发送路径**:
```
write(fd, buf, len)
    → sock_write_iter()
        → sock_sendmsg()
            → sock->ops->sendmsg()   // inet_sendmsg
                → sk->sk_prot->sendmsg()  // tcp_sendmsg / udp_sendmsg

sendto(fd, buf, len, flags, dest, addrlen)
    → __sys_sendto()
        → sock_sendmsg()
            → sock->ops->sendmsg()
                → sk->sk_prot->sendmsg()
```

**接收路径**:
```
read(fd, buf, len)
    → sock_read_iter()
        → sock_recvmsg()
            → sock->ops->recvmsg()   // inet_recvmsg
                → sk->sk_prot->recvmsg()  // tcp_recvmsg / udp_recvmsg

recvfrom(fd, buf, len, flags, src, addrlen)
    → __sys_recvfrom()
        → sock_recvmsg()
            → sock->ops->recvmsg()
                → sk->sk_prot->recvmsg()
```

### 6.3 bind/listen/accept 调用链

```
bind(fd, addr, addrlen)
    → __sys_bind()
        → sock->ops->bind()   // inet_bind
            → sk->sk_prot->get_port()  // inet_csk_get_port

listen(fd, backlog)
    → __sys_listen()
        → sock->ops->listen()  // inet_listen
            → sk->sk_prot->init() 早已设置
            → inet_csk_listen_start()

accept(fd, addr, addrlen)
    → __sys_accept4()
        → sock->ops->accept()  // inet_accept
            → sk->sk_prot->accept()  // inet_csk_accept
                → dequeue from icsk_accept_queue
```