# sockfs — 套接字文件系统

## 1. 概述与实现机制

sockfs 是一个伪文件系统，使 socket 能够通过 VFS 文件接口访问。这就是为什么 `socket()` 系统调用返回一个文件描述符的原因。

### 核心原理

- **socket 作为文件**：通过 sockfs 将 socket 操作映射为 VFS 文件操作
- **file→private_data**：指向 `struct socket`
- **socket→sk**：指向 `struct sock`（协议栈核心）
- **file→f_op**：始终为 `socket_file_ops`

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  socket(AF_INET, SOCK_STREAM, 0) → 返回 fd                  │
│  read(fd, buf, len) → 通过 VFS 读取 socket 数据             │
└────────────────────────┬─────────────────────────────────────┘
                         │ 系统调用
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                    sockfs (net/socket.c)                     │
│  socket_file_ops → file_operations 映射表                    │
│  sock_alloc_file() → 创建 file + inode                      │
│  SOCK_INODE(sock) → 从 socket 获取 inode                    │
│  sockfs_dname() → 自定义 dentry 名称显示                    │
└────────────────────────┬─────────────────────────────────────┘
                         │ VFS 操作 → 协议栈
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                   协议栈层                                    │
│  sock_read_iter() → sock_recvmsg() → tcp_recvmsg()          │
│  sock_write_iter() → sock_sendmsg() → tcp_sendmsg()         │
│  sock_poll() → tcp_poll() → sk_data_ready() 唤醒            │
│  sock_ioctl() → dev_ioctl() / tcp_ioctl()                   │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 socket — 通用套接字结构

```c
// 文件: include/linux/net.h
struct socket {
    socket_state            state;      // 套接字状态 (SS_UNCONNECTED 等)
    short                   type;       // 套接字类型 (SOCK_STREAM 等)
    unsigned long            flags;     // 标志位 (SOCK_NONBLOCK 等)
    struct file             *file;      // 关联的 VFS file 对象
    struct sock             *sk;        // 协议栈核心结构
    const struct proto_ops  *ops;       // 协议操作函数表
    struct socket_wq        wq;         // 等待队列
};
```

### 2.2 sock — 协议栈 socket 核心

```c
// 文件: include/net/sock.h
struct sock {
    struct sk_buff_head     sk_receive_queue; // 接收队列
    struct sk_buff_head     sk_write_queue;   // 发送队列
    int                     sk_rcvbuf;       // 接收缓冲区大小
    int                     sk_sndbuf;       // 发送缓冲区大小
    int                     sk_rcvlowat;     // 接收低水位标记
    unsigned long            sk_flags;       // 标志位
    int                     sk_err;          // 错误码
    struct socket           *sk_socket;      // 指向通用 socket 结构
    struct proto            *sk_prot;        // 协议处理函数表
    union {
        struct tcp_sock     *sk_tcp;         // TCP 协议私有数据
        struct udp_sock     *sk_udp;         // UDP 协议私有数据
    };
    wait_queue_head_t       *sk_wq;          // 等待队列
    struct mem_cgroup       *sk_memcg;       // 内存 cgroup
    void                    *sk_security;    // 安全模块数据
    // ... (大量字段)
};
```

### 2.3 socket_file_ops — socket 文件操作

```c
// 文件: net/socket.c
static const struct file_operations socket_file_ops = {
    .owner          = THIS_MODULE,
    .read_iter      = sock_read_iter,      // 读取
    .write_iter     = sock_write_iter,     // 写入
    .poll           = sock_poll,           // 轮询
    .unlocked_ioctl = sock_ioctl,          // IOCTL
    .compat_ioctl   = compat_sock_ioctl,   // 32位兼容 IOCTL
    .uring_cmd      = io_uring_cmd_sock,   // io_uring 命令
    .mmap           = sock_mmap,           // 内存映射
    .release        = sock_close,          // 关闭
    .fasync         = sock_fasync,         // 异步通知
    .splice_write   = splice_to_socket,    // splice 写入
    .splice_read    = sock_splice_read,    // splice 读取
    .splice_eof     = sock_splice_eof,     // splice EOF
    .show_fdinfo    = sock_show_fdinfo,    // 显示 fd 信息
};
```

### 2.4 proto_ops — 协议操作函数表

```c
// 文件: include/linux/net.h
struct proto_ops {
    int     family;                     // 协议族 (AF_INET 等)
    struct module   *owner;             // 所属模块
    int     (*release)   (struct socket *sock);    // 释放
    int     (*bind)      (struct socket *sock, struct sockaddr *addr, int len); // 绑定
    int     (*connect)   (struct socket *sock, struct sockaddr *addr, int len, int flags); // 连接
    int     (*socketpair)(struct socket *sock1, struct socket *sock2); // 套接字对
    int     (*accept)    (struct socket *sock, struct socket *newsock, int flags, bool kern); // 接受
    int     (*getname)   (struct socket *sock, struct sockaddr *addr, int peer); // 获取名称
    __poll_t (*poll)     (struct file *file, struct socket *sock, struct poll_table_struct *wait); // 轮询
    int     (*ioctl)     (struct socket *sock, unsigned int cmd, unsigned long arg); // IOCTL
    int     (*listen)    (struct socket *sock, int len);    // 监听
    int     (*shutdown)  (struct socket *sock, int how);    // 关闭
    int     (*setsockopt)(struct socket *sock, int level, int optname, sockptr_t optval, unsigned int optlen); // 设置选项
    int     (*getsockopt)(struct socket *sock, int level, int optname, char __user *optval, int __user *optlen); // 获取选项
    int     (*sendmsg)   (struct socket *sock, struct msghdr *m, size_t total_len); // 发送消息
    int     (*recvmsg)   (struct socket *sock, struct msghdr *m, size_t total_len, int flags); // 接收消息
    int     (*mmap)      (struct file *file, struct socket *sock, struct vm_area_struct *vma); // 内存映射
    ssize_t (*read_iter) (struct kiocb *iocb, struct iov_iter *to); // 迭代读取
    ssize_t (*write_iter)(struct kiocb *iocb, struct iov_iter *from); // 迭代写入
    int     (*sendpage)  (struct socket *sock, struct page *page, int offset, size_t size, int flags); // 发送页
    int     (*splice_read)(struct socket *sock, loff_t *ppos, struct pipe_inode_info *pipe, size_t len, unsigned int flags); // splice 读取
};
```

---

## 3. API 与使用方法

### 3.1 核心 API

```c
#include <linux/net.h>
#include <net/sock.h>

// socket 创建
struct socket *sock_create(int family, int type, int protocol, struct socket **res);
struct socket *sock_create_kern(struct net *net, int family, int type, int protocol);
struct file *sock_alloc_file(struct socket *sock, int flags, const char *dname);

// socket 操作
int sock_sendmsg(struct socket *sock, struct msghdr *msg);
int sock_recvmsg(struct socket *sock, struct msghdr *msg, int flags);
int sock_mmap(struct file *file, struct socket *sock, struct vm_area_struct *vma);
int sock_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
__poll_t sock_poll(struct file *file, struct socket *sock, struct poll_table_struct *wait);

// 内核内部创建 socket
int kernel_socket(struct socket **sock, int family, int type, int protocol);
int kernel_bind(struct socket *sock, struct sockaddr *addr, int addrlen);
int kernel_connect(struct socket *sock, struct sockaddr *addr, int addrlen, int flags);
int kernel_listen(struct socket *sock, int backlog);
int kernel_accept(struct socket *sock, struct socket **newsock, int flags);
int kernel_sendmsg(struct socket *sock, struct msghdr *msg);
int kernel_recvmsg(struct socket *sock, struct msghdr *msg, ...);
int kernel_setsockopt(struct socket *sock, int level, int optname, sockptr_t optval, unsigned int optlen);
int kernel_getsockopt(struct socket *sock, int level, int optname, char *optval, int *optlen);
```

### 3.2 使用示例

```c
// 示例1: socket() 系统调用完整流程
// 用户空间:
int fd = socket(AF_INET, SOCK_STREAM, 0);

// 内核实现 (简化):
SYSCALL_DEFINE3(socket, int, family, int, type, int, protocol)
{
    struct socket *sock;
    struct file *file;
    int fd;

    // 1. 创建 socket 结构
    ret = sock_create(family, type, protocol, &sock);
    //   → net_families[family]->create(sock, protocol)  // 如 inet_create()
    //     → sk_alloc()  → 分配 struct sock
    //     → sock->ops = &inet_stream_ops  // 设置协议操作

    // 2. 分配文件描述符
    fd = get_unused_fd_flags(flags);

    // 3. 在 sockfs 中创建 file 对象
    file = sock_alloc_file(sock, flags, NULL);
    //   → alloc_file_pseudo(SOCK_INODE(sock), sock_mnt, dname, ...)
    //     → alloc_file()  // 分配 file 结构
    //     → file->f_op = &socket_file_ops  // 设置 socket 文件操作
    //     → file->private_data = sock  // 关联 socket
    //   → sock->file = file  // 反向关联

    // 4. 安装 fd
    fd_install(fd, file);
    return fd;
}
```

```c
// 示例2: 内核内部创建 TCP socket 并连接
static int kernel_tcp_example(void)
{
    struct socket *sock;
    struct sockaddr_in sin;
    struct msghdr msg;
    int ret;

    // 创建 TCP socket
    ret = sock_create_kern(current->nsproxy->net_ns, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
    if (ret < 0)
        return ret;

    // 连接服务器
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = in_aton("192.168.1.100");
    sin.sin_port = htons(8080);
    ret = kernel_connect(sock, (struct sockaddr *)&sin, sizeof(sin), 0);
    if (ret < 0)
        goto out;

    // 发送数据
    msg.msg_name = NULL;
    // ... 填充 msg 结构
    ret = kernel_sendmsg(sock, &msg, iov, iov_len, total_len);

out:
    sock_release(sock);
    return ret;
}
```

---

## 4. 函数调用栈

### 4.1 socket 创建流程

```
socket(AF_INET, SOCK_STREAM, 0)
  ↓
__sys_socket(family, type, protocol)
  → sock_create(family, type, protocol, &sock)
    → __sock_create(current->nsproxy->net_ns, ...)
      → net_families[family]->create(sock, protocol)  // AF_INET → inet_create
        → sk_alloc(net, PF_INET, GFP_KERNEL, &tcp_prot, ...)  // 分配 struct sock
        → sock->ops = &inet_stream_ops            // 设置 stream 操作
        → sk->sk_prot = &tcp_prot                 // 设置 TCP 协议操作
        → sock_init_data(sock, sk)                // 初始化 socket-sock 关联
          → sk->sk_socket = sock
          → sock->sk = sk
  → sock_map_fd(sock, flags)
    → get_unused_fd_flags(flags)                  // 分配 fd 号
    → sock_alloc_file(sock, flags, NULL)           // 在 sockfs 中创建 file
      → alloc_file_pseudo(SOCK_INODE(sock), sock_mnt, dname, ...)
        → alloc_file(&SOCK_INODE(sock)->i_mode, ...)
          → file->f_op = &socket_file_ops         // 设置 socket 文件操作
          → file->private_data = sock             // 关联 socket
      → sock->file = file                         // 反向关联
    → fd_install(fd, file)                        // 安装 fd
    → return fd
```

### 4.2 socket 读取流程

```
read(fd, buf, len)
  ↓ vfs_read() → file->f_op->read_iter()
    → sock_read_iter(iocb, to)
      → sock_recvmsg(sock, msg, flags)
        → sock->ops->recvmsg(sock, msg, ...)      // inet_recvmsg
          → tcp_recvmsg(sk, msg, ...)              // TCP 协议接收
            → skb = skb_dequeue(&sk->sk_receive_queue)  // 从接收队列取 skb
            → skb_copy_datagram_msg(skb, offset, msg, used)  // 拷贝数据到用户空间
            → tcp_cleanup_rbuf(sk, copied)         // 清理接收缓冲区
```

### 4.3 socket 写入流程

```
write(fd, buf, len)
  ↓ vfs_write() → file->f_op->write_iter()
    → sock_write_iter(iocb, from)
      → sock_sendmsg(sock, msg)
        → sock->ops->sendmsg(sock, msg, ...)      // inet_sendmsg
          → tcp_sendmsg(sk, msg, total_len)         // TCP 协议发送
            → tcp_sendmsg_locked(sk, msg, size)
              → skb = tcp_write_queue_tail(sk)     // 获取发送队列尾 skb
              → skb_copy_from_iter_nocache(skb, ...)  // 拷贝数据到 skb
              → tcp_push(sk, flags, mss_now, ...)  // 推送数据
                → __tcp_push_pending_frames(sk)    // 发送待发送帧
                  → tcp_write_xmit(sk)             // 实际发送
                    → tcp_transmit_skb(sk, skb)    // 传输 skb
                      → icsk->icsk_af_ops->queue_xmit(sk, skb)  // IP 层发送
```

---

## 5. 流程图

### 5.1 socket 文件系统架构

```
用户空间:
    fd = socket(AF_INET, SOCK_STREAM, 0);
         │
         ▼
内核:
    sock_create() → inet_create()
         │
         ├── alloc struct socket
         ├── alloc struct sock (struct tcp_sock)
         └── 设置 ops 指针
         │
         ▼
    sock_alloc_file()
         │
         ├── SOCK_INODE(sock) → sockfs 中的 inode
         ├── alloc_file() → file 结构
         │     ├── f_op = &socket_file_ops
         │     └── private_data = sock
         └── fd_install(fd, file)
         │
         ▼
    VFS 操作 → socket_file_ops → sock->ops → proto_ops → sk->sk_prot
         │
    read(fd) → sock_read_iter → sock_recvmsg → inet_recvmsg → tcp_recvmsg
    write(fd) → sock_write_iter → sock_sendmsg → inet_sendmsg → tcp_sendmsg
    poll(fd) → sock_poll → inet_poll → tcp_poll
    ioctl(fd) → sock_ioctl → inet_ioctl → tcp_ioctl
    close(fd) → sock_close → sock_release → inet_release → tcp_close
```

### 5.2 sockfs 关键数据流

```
                    ┌──────────────────┐
                    │    struct file    │
                    │  private_data ────┼──┐
                    │  f_op = sock_ops │  │
                    └──────────────────┘  │
                                          ▼
                    ┌──────────────────────────────┐
                    │     struct socket             │
                    │  sk ──────────────────────────┼──┐
                    │  ops = &inet_stream_ops       │  │
                    │  file ────→ 指向 file         │  │
                    └──────────────────────────────┘  │
                                                     ▼
                    ┌──────────────────────────────────────┐
                    │        struct sock (tcp_sock)        │
                    │  sk_receive_queue ── skb list        │
                    │  sk_write_queue   ── skb list        │
                    │  sk_prot = &tcp_prot                 │
                    │  sk_socket → 指向 socket             │
                    │  sk_wq → 等待队列                    │
                    └──────────────────────────────────────┘
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **网络通信** | TCP/UDP 网络数据传输 | 浏览器、服务器、聊天应用 |
| **内核网络服务** | 内核内部发起网络通信 | NFS 客户端、CIFS、iSCSI |
| **Unix Domain Socket** | 本地进程间通信 | `socket(AF_UNIX, ...)` |
| **io_uring 网络** | 异步网络 I/O | `io_uring_cmd_sock` |
| **splice 零拷贝** | 网络和文件间零拷贝传输 | `splice(fd_in, NULL, fd_out, ...)` |
| **网络监控** | 通过 packet socket 抓包 | `socket(AF_PACKET, ...)`、tcpdump |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `net/socket.c` | sockfs 核心实现、socket 系统调用、file_operations |
| `include/linux/net.h` | socket 结构体和 proto_ops 定义 |
| `include/net/sock.h` | sock 结构体和协议栈核心定义 |
| `net/ipv4/tcp.c` | TCP 协议 socket 操作（tcp_recvmsg, tcp_sendmsg） |
| `net/ipv4/udp.c` | UDP 协议 socket 操作 |
| `net/ipv4/af_inet.c` | AF_INET 协议族创建和操作（inet_create, inet_sendmsg） |