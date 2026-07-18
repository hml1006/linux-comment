# Linux 内核网络数据收发完整流程分析

## 目录

1. [系统总览](#1-系统总览)
2. [发送路径](#2-发送路径)
3. [接收路径](#3-接收路径)
4. [关键硬件-软件交互点](#4-关键硬件-软件交互点)
5. [核心数据结构 sk_buff 在各层的变换](#5-核心数据结构-sk_buff-在各层的变换)
6. [性能优化机制](#6-性能优化机制)
7. [附：关键代码路径速查表](#7-附关键代码路径速查表)

---

## 1. 系统总览

### 1.1 整体架构图

Linux 网络数据收发从硬件（网卡）到软件（用户态 socket）贯穿 7 个层次，每个层次职责分明、接口清晰。

```
                         用户态应用程序
                     ┌───────────────────────┐
                     │  send() / recv()      │    用户空间
                     │  epoll / select       │
                     └──────────┬────────────┘
         ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
                                │ 系统调用 (syscall)
                     ┌──────────▼────────────┐
                     │  VFS Socket 层         │
                     │  net/socket.c          │
                     │  sock_read_iter /      │
                     │  sock_write_iter       │
                     └──────────┬────────────┘
                                │ sock->ops->sendmsg / recvmsg
                     ┌──────────▼────────────┐
                     │  Socket 核心层         │
                     │  net/core/sock.c       │
                     │  struct sock           │
                     │  sk_receive_queue /    │
                     │  sk_write_queue        │
                     └──────────┬────────────┘
                                │ sk->sk_prot->sendmsg / recvmsg
                     ┌──────────▼────────────┐
                     │  传输层 (TCP/UDP)      │
                     │  net/ipv4/tcp.c       │
                     │  net/ipv4/udp.c       │
                     │  tcp_sendmsg /         │
                     │  tcp_recvmsg           │
                     └──────────┬────────────┘
                                │ icsk_af_ops->queue_xmit
                                │ / ip_local_deliver
                     ┌──────────▼────────────┐
                     │  网络层 (IP)           │
                     │  net/ipv4/ip_output.c │
                     │  net/ipv4/ip_input.c  │
                     │  ip_queue_xmit /       │
                     │  ip_rcv                │
                     └──────────┬────────────┘
                                │ dst_output / netif_receive_skb
                     ┌──────────▼────────────┐
                     │  网络设备层            │
                     │  net/core/dev.c       │
                     │  dev_queue_xmit /      │
                     │  __netif_receive_skb   │
                     │  GRO / GSO / Qdisc    │
                     └──────────┬────────────┘
                                │ ndo_start_xmit
                     ┌──────────▼────────────┐
                     │  网卡驱动层 (e1000)    │
                     │  drivers/net/.../     │
                     │  e1000_xmit_frame /   │
                     │  e1000_clean_rx_irq   │
                     │  TX/RX Ring / NAPI    │
                     └──────────┬────────────┘
                                │ MMIO / DMA / 中断
                     ┌──────────▼────────────┐
                     │  网卡硬件层            │
                     │  PHY / MAC / DMA      │
                     │  PCIe 总线             │
                     └───────────────────────┘
```

### 1.2 各层文件映射

| 层次 | 核心文件 | 关键结构/函数 |
|------|---------|-------------|
| 用户空间 | `libc` | `write()`, `send()`, `sendmsg()`, `recv()`, `recvmsg()` |
| VFS Socket 层 | [net/socket.c](file:///home/louis/code/linux/net/socket.c) | `__sys_sendto()`, `__sys_recvfrom()`, `sock_write_iter()`, `sock_read_iter()`, `struct socket` |
| Socket 核心层 | [net/core/sock.c](file:///home/louis/code/linux/net/core/sock.c) | `struct sock`, `sk_alloc()`, `sock_init_data()`, `sk_receive_queue`, `sk_write_queue` |
| TCP 传输层 | [net/ipv4/tcp.c](file:///home/louis/code/linux/net/ipv4/tcp.c), [net/ipv4/tcp_output.c](file:///home/louis/code/linux/net/ipv4/tcp_output.c), [net/ipv4/tcp_input.c](file:///home/louis/code/linux/net/ipv4/tcp_input.c) | `tcp_sendmsg()`, `tcp_write_xmit()`, `__tcp_transmit_skb()`, `tcp_recvmsg()`, `tcp_v4_rcv()`, `tcp_rcv_established()` |
| UDP 传输层 | [net/ipv4/udp.c](file:///home/louis/code/linux/net/ipv4/udp.c) | `udp_sendmsg()`, `udp_rcv()`, `udp_queue_rcv_skb()` |
| IP 网络层 | [net/ipv4/ip_output.c](file:///home/louis/code/linux/net/ipv4/ip_output.c), [net/ipv4/ip_input.c](file:///home/louis/code/linux/net/ipv4/ip_input.c) | `ip_queue_xmit()`, `ip_local_out()`, `ip_output()`, `ip_rcv()`, `ip_rcv_finish()`, `ip_local_deliver()` |
| 邻居子系统 | [net/core/neighbour.c](file:///home/louis/code/linux/net/core/neighbour.c) | `neigh_output()`, `neigh_resolve_output()`, `arp_rcv()` |
| 网络设备层 | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) | `dev_queue_xmit()`, `__dev_xmit_skb()`, `netif_receive_skb()`, `__netif_receive_skb_core()` |
| GRO/GSO | [net/core/gro.c](file:///home/louis/code/linux/net/core/gro.c), [net/core/gso.c](file:///home/louis/code/linux/net/core/gso.c) | `napi_gro_receive()`, `dev_gro_receive()`, `skb_gso_segment()` |
| 网卡驱动 | [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) | `e1000_xmit_frame()`, `e1000_intr()`, `e1000_clean()`, `e1000_clean_rx_irq()`, `e1000_receive_skb()` |
| 硬件抽象 | [drivers/net/ethernet/intel/e1000/e1000_hw.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.h) | `struct e1000_hw`, `er32()`, `ew32()`, `struct e1000_tx_desc`, `struct e1000_rx_desc` |

### 1.3 数据流全景图

```
发送路径 (TX)                                    接收路径 (RX)
                                                                             
用户态 write()                                   用户态 read()
    │                                                  ▲
    ▼                                                  │
[VFS] sock_write_iter()                    [VFS] sock_read_iter()
    │                                                  ▲
    ▼                                                  │
[Socket] inet_sendmsg()                    [Socket] inet_recvmsg()
    │                                                  ▲
    ▼                                                  │
[TCP] tcp_sendmsg() ──────┐        ┌──── [TCP] tcp_recvmsg()
    │                      │        │          ▲
    ▼                      │        │          │
[TCP] tcp_write_xmit()     │        │  [TCP] tcp_data_queue()
    │                      │        │          ▲
    ▼                      │        │          │
[TCP] __tcp_transmit_skb() │        │  [TCP] tcp_rcv_established()
    │                      │        │          ▲
    ▼                      │        │          │
[IP] ip_queue_xmit()       │        │  [TCP] tcp_v4_do_rcv()
    │                      │        │          ▲
    ▼                      │        │          │
[IP] ip_local_out()        │        │  [TCP] tcp_v4_rcv()
    │                      │        │          ▲
    ▼                      │        │          │
[IP] ip_output()           │        │  [IP] ip_local_deliver()
    │                      │        │          ▲
    ▼                      │        │          │
[Neigh] neigh_output()     │        │  [IP] ip_rcv_finish()
    │                      │        │          ▲
    ▼                      │        │          │
[DEV] dev_queue_xmit()     │        │  [DEV] __netif_receive_skb_core()
    │                      │        │          ▲
    ▼                      │        │          │
[DEV] Qdisc enqueue        │        │  [DEV] netif_receive_skb()
    │                      │        │          ▲
    ▼                      │        │          │
[DEV] sch_direct_xmit()    │        │  [GRO] napi_gro_receive()
    │                      │        │          ▲
    ▼                      │        │          │
[DRV] e1000_xmit_frame()   │        │  [DRV] e1000_receive_skb()
    │                      │        │          ▲
    ▼                      │        │          │
[DRV] 填充 TX Descriptor   │        │  [DRV] e1000_clean_rx_irq()
    │                      │        │          ▲
    ▼                      │        │          │
[DRV] writel(TDT) ────────►├───────►│  [DRV] napi_schedule() → e1000_clean()
    │                      │        │          ▲
    ▼                      │        │          │
┌──────────────┐           │        │  [DRV] e1000_intr() (中断)
│ 硬件 DMA 读   │           │        │          ▲
│ TX 描述符     │           │        │          │
│ DMA 取数据   │           │        │  ┌──────────────┐
│ MAC 添加头   │           │        │  │ 硬件 DMA 写入  │
│ PHY 发送     │           │        │  │ RX 缓冲区     │
│ ──► 网线     │           │        │  │ MAC 校验地址  │
└──────────────┘           │        │  │ PHY 接收信号  │
                           │        │  │ 网线 ──►      │
  ─────────────────────────┘        └──┴──────────────┘
```

---

## 2. 发送路径

### 2.1 发送路径总览

数据从用户态 `write(fd, buf, len)` 到网线发出的完整旅程：

```
write(fd, buf, len)
  │  [系统调用] 用户空间 → 内核空间
  ▼
__sys_sendto()                [net/socket.c]
  │  [VFS] 通过 fd 获取 struct socket
  ▼
sock_sendmsg()                [net/socket.c]
  │  [权限检查 + 安全模块]
  ▼
sock->ops->sendmsg()          [inet_sendmsg in af_inet.c]
  │  [协议族分派]
  ▼
sk->sk_prot->sendmsg()        [tcp_sendmsg in tcp.c]
  │  [TCP 层：拷贝数据、构建段]
  ▼
tcp_write_xmit()              [tcp_output.c]
  │  [拥塞窗口控制、TSO 分片]
  ▼
__tcp_transmit_skb()          [tcp_output.c]
  │  [构建 TCP 头部、校验和]
  ▼
icsk_af_ops->queue_xmit()     [ip_queue_xmit in ip_output.c]
  │  [IP 层：路由查找、构建 IP 头]
  ▼
ip_local_out() / ip_output()  [ip_output.c]
  │  [Netfilter 钩子 NF_INET_LOCAL_OUT / POST_ROUTING]
  ▼
ip_finish_output()            [ip_output.c]
  │  [分片检查、邻居子系统]
  ▼
neigh_output()                [neighbour.c]
  │  [L2 地址解析：ARP/NDISC]
  ▼
dev_queue_xmit()              [net/core/dev.c]
  │  [设备层：Qdisc 流量控制]
  ▼
__dev_xmit_skb()              [net/core/dev.c]
  │  [Qdisc 入队/出队]
  ▼
sch_direct_xmit()             [net/core/dev.c]
  │  [GSO 分片]
  ▼
dev_hard_start_xmit()         [net/core/dev.c]
  │  [调用驱动 ndo_start_xmit]
  ▼
e1000_xmit_frame()            [e1000_main.c]
  │  [驱动层：填充 TX 描述符]
  ▼
e1000_tx_queue()              [e1000_main.c]
  │  [DMA 映射、写描述符环]
  ▼
writel(TDT, reg)              [e1000_main.c]
  │  [MMIO 写：通知硬件]
  ▼
┌─────────────────────────────────────────────────────┐
│ 硬件行为                                              │
│ 1. 硬件 DMA 引擎读取 TX 描述符                          │
│ 2. DMA 从系统内存读取数据到 FIFO                       │
│ 3. MAC 添加前导码、帧起始定界符 (SFD)                  │
│ 4. 插入 VLAN 标签（若启用）                             │
│ 5. 计算并插入 FCS (CRC-32)                            │
│ 6. 将帧发送到 PHY 层                                  │
│ 7. PHY 进行 PCS 编码 (8B/10B 或 64B/66B)              │
│ 8. 通过 MDI 差分对发送到网线                           │
└─────────────────────────────────────────────────────┘
```

### 2.2 用户空间 → VFS Socket 层

#### 2.2.1 系统调用入口

用户态调用 `write(fd, buf, len)` 或 `sendmsg(fd, msg, flags)` 时，进入内核的入口函数：

```c
// net/socket.c
static ssize_t sock_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct socket *sock = iocb->ki_filp->private_data;
    // ... 构造 struct msghdr
    return sock_sendmsg(sock, msg);
}
```

系统调用 → VFS 的关键路径：

```
write(fd, buf, len)
    → ksys_write()               [fs/read_write.c]
        → vfs_write()            [fs/read_write.c]
            → new_sync_write()   [fs/read_write.c]
                → file->f_op->write_iter()  → sock_write_iter()
                    → sock_sendmsg(sock, msg)
                        → sock->ops->sendmsg(sock, msg, len)
```

#### 2.2.2 协议族分派

`inet_sendmsg()` 是 INET 协议族的通用发送入口，它根据 `sk->sk_prot` 分派到具体的传输层协议：

```c
// net/ipv4/af_inet.c
int inet_sendmsg(struct socket *sock, struct msghdr *msg, size_t size)
{
    struct sock *sk = sock->sk;
    // 若 socket 未绑定，自动绑定临时端口
    if (unlikely(!inet_sk(sk)->inet_num))
        inet_autobind(sk);
    // 分派到具体协议
    return INDIRECT_CALL_2(sk->sk_prot->sendmsg,
                           tcp_sendmsg, udp_sendmsg, sk, msg, size);
}
```

### 2.3 TCP 发送路径

#### 2.3.1 发送入口：tcp_sendmsg 和 tcp_sendmsg_locked

定义在 [net/ipv4/tcp.c](file:///home/louis/code/linux/net/ipv4/tcp.c)。

```c
int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
{
    int ret;
    // 获取 socket 锁
    lock_sock(sk);
    ret = tcp_sendmsg_locked(sk, msg, size);
    release_sock(sk);
    return ret;
}
```

`tcp_sendmsg_locked()` 是核心实现，它循环从用户空间拷贝数据到 skb：

```
tcp_sendmsg_locked()
    │
    ├── tcp_send_mss() → 计算当前 MSS
    │
    └── while (msg_data_left(msg))  // 循环拷贝所有数据
        │
        ├── sk_stream_alloc_skb(sk, size, ...) → 分配 skb
        │   └── alloc_skb_fclone() + skb_reserve()
        │
        ├── skb_add_data_nocache(sk, skb, from, copy) → 从用户态拷贝数据
        │   └── copy_from_user()  // 实际数据拷贝
        │
        ├── skb->len += copy  → 更新长度
        │
        └── tcp_push(sk, skb, mss_now, nonagle, size_goal)
            │
            └── 判断是否应该立即发送：
                ├── 是 → __tcp_push_pending_frames()
                └── 否 → 等待更多数据 (Nagle 算法)
```

#### 2.3.2 发送引擎：tcp_write_xmit

定义在 [net/ipv4/tcp_output.c](file:///home/louis/code/linux/net/ipv4/tcp_output.c)，是 TCP 发送的核心引擎。

```c
static bool tcp_write_xmit(struct sock *sk, unsigned int mss_now,
                           int nonagle, int push_one, gfp_t gfp)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct sk_buff *skb;
    unsigned int tso_segs, sent_pkts;
    int cwnd_quota;

    while ((skb = tcp_send_head(sk))) {
        // 1. TSO 分片数计算
        tso_segs = tcp_tso_segs(sk, skb, mss_now);

        // 2. 拥塞窗口检查
        cwnd_quota = tcp_cwnd_test(tp, skb);
        if (!cwnd_quota)
            break;

        // 3. 接收窗口检查
        if (unlikely(!tcp_window_allows(tp, skb, mss_now, cwnd_quota)))
            break;

        // 4. Nagle 算法检查
        if (tcp_may_send_now(sk, skb, nonagle, push_one, mss_now))
            break;

        // 5. 发送 skb 到 IP 层
        if (unlikely(tcp_transmit_skb(sk, skb, 1, gfp)))
            break;

        // 6. 更新 TCP 状态
        tcp_event_new_data_sent(sk, skb);
    }
    return false;
}
```

**发送条件检查顺序**：

```
tcp_write_xmit() 发送条件检查
    │
    ├── tcp_snd_test()  ← 综合检查
    │   ├── tcp_cwnd_test()    → 拥塞窗口是否足够？
    │   │   └── packets_out + tso_segs < cwnd
    │   │
    │   ├── tcp_window_allows() → 接收窗口是否足够？
    │   │   └── snd_nxt - snd_una + len < snd_wnd
    │   │
    │   └── tcp_may_send_now()  → Nagle 是否允许立即发送？
    │       └── 非 Nagle 模式 / 已推送 / 数据已满 MSS
    │
    └── tcp_mss_split_point()  → TSO 分段点计算
```

#### 2.3.3 构建 TCP 头部：__tcp_transmit_skb

定义在 [net/ipv4/tcp_output.c](file:///home/louis/code/linux/net/ipv4/tcp_output.c)。

```c
static int __tcp_transmit_skb(struct sock *sk, struct sk_buff *skb,
                              int clone_it, gfp_t gfp_mask, u32 rcv_nxt)
{
    struct inet_sock *inet = inet_sk(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
    struct tcphdr *th;
    const struct tcp_options_received rx_opt = tp->rx_opt;
    int tcp_header_size;

    // 1. 若是重传，克隆 skb（避免影响原始 skb）
    if (clone_it) {
        skb = skb_clone(skb, gfp_mask);
        if (unlikely(!skb))
            return -ENOBUFS;
    }

    // 2. 计算 TCP 选项长度
    tcp_options_size = tcp_established_options(sk, skb, &opts, &key);
    tcp_header_size = sizeof(struct tcphdr) + tcp_options_size;

    // 3. 在 skb 头部预留空间，构建 TCP 头
    __skb_push(skb, tcp_header_size);
    th = (struct tcphdr *)skb->data;
    th->source  = inet->inet_sport;    // 源端口
    th->dest    = inet->inet_dport;    // 目的端口
    th->seq     = htonl(tcb->seq);     // 序列号
    th->ack_seq = htonl(rcv_nxt);      // 确认号
    th->window  = htons(tcp_select_window(sk));  // 滑动窗口
    th->check   = 0;                   // 校验和暂置 0

    // 4. 写入 TCP 选项（时间戳、SACK、MSS 等）
    tcp_options_write(th, tp, &opts, &key);

    // 5. 计算校验和（硬件卸载或软件计算）
    INDIRECT_CALL_INET(icsk->icsk_af_ops->send_check,
                       tcp_v6_send_check, tcp_v4_send_check, sk, skb);

    // 6. 调用 IP 层发送
    err = INDIRECT_CALL_INET(icsk->icsk_af_ops->queue_xmit,
                             inet6_csk_xmit, ip_queue_xmit,
                             sk, skb, &inet->cork.fl);
    return err;
}
```

### 2.4 IP 发送路径

#### 2.4.1 ip_queue_xmit

定义在 [net/ipv4/ip_output.c](file:///home/louis/code/linux/net/ipv4/ip_output.c)。

```
ip_queue_xmit(sk, skb, fl)
    │
    ├── rt = ip_route_output_flow(sk_net(sk), fl, sk)  → 路由查找
    │   └── 若路由失败，释放 skb，返回错误
    │
    ├── skb->destructor = tcp_wfree  → 设置 skb 释放回调
    │
    ├── 构建 IP 头部：
    │   ├── ip_hdr(skb)->version = 4
    │   ├── ip_hdr(skb)->tos = inet->tos
    │   ├── ip_hdr(skb)->ttl = ip_select_ttl()
    │   ├── ip_hdr(skb)->protocol = IPPROTO_TCP
    │   ├── ip_hdr(skb)->saddr = fl4->saddr
    │   └── ip_hdr(skb)->daddr = fl4->daddr
    │
    ├── 设置 skb 的 dst 路由缓存
    │
    └── ip_local_out(net, sk, skb)  → 发送
```

#### 2.4.2 ip_local_out → ip_output

```
ip_local_out(net, sk, skb)
    │
    └── __ip_local_out(skb)
        │
        ├── Netfilter 钩子: NF_INET_LOCAL_OUT
        │   └── nf_hook(NFPROTO_IPV4, NF_INET_LOCAL_OUT, ...)
        │
        └── dst_output(skb)  → skb->dst->output(skb)
            │
            └── ip_output(skb)
                │
                ├── Netfilter 钩子: NF_INET_POST_ROUTING
                │
                └── ip_finish_output(skb)
                    │
                    ├── 检查是否需要分片：
                    │   ├── skb->len > dst_mtu → ip_fragment()
                    │   └── 否则 → ip_finish_output2()
                    │
                    └── ip_finish_output2(skb)
                        │
                        ├── 构建 L2 头部
                        │
                        └── neigh_output(skb)  → 邻居子系统
```

### 2.5 邻居子系统

定义在 [net/core/neighbour.c](file:///home/louis/code/linux/net/core/neighbour.c)。

```
neigh_output(skb)
    │
    ├── neigh = dst_neigh_lookup(skb->dst, &daddr)  → 查找邻居表
    │
    ├── 若邻居已解析 (NUD_REACHABLE)：
    │   └── neigh_hh_output(skb)  → 直接使用硬件头部缓存
    │
    ├── 若邻居未解析：
    │   └── neigh_resolve_output(skb)
    │       ├── __neigh_event_send()  → 触发 ARP 请求
    │       └── dev_queue_xmit(skb)  → 使用已解析的 L2 地址发送
    │
    └── ARP 解析流程：
        └── arp_solicit() → 发送 ARP 请求
            └── dev_queue_xmit() → 发送 ARP 广播帧
```

### 2.6 网络设备层

#### 2.6.1 dev_queue_xmit

定义在 [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c)。

```
dev_queue_xmit(skb)
    │
    └── __dev_queue_xmit(skb, NULL)
        │
        ├── 选择 TX 队列：
        │   └── skb_get_queue_mapping(skb) → 选择 netdev_queue
        │
        ├── 处理 Qdisc 流量控制：
        │   └── __dev_xmit_skb(skb, dev, txq, q)
        │       │
        │       ├── 若 qdisc 是 noqueue 或 noop：
        │       │   └── dev_hard_start_xmit(skb, dev, txq)
        │       │
        │       └── 否则 (有 Qdisc)：
        │           ├── q->enqueue(skb, q)  → 入队
        │           ├── qdisc_run(dev, q)   → 尝试出队
        │           │   └── __qdisc_run(q)
        │           │       └── while (qdisc_restart(q))  → 循环出队
        │           │           └── sch_direct_xmit(skb, q, dev, txq, ...)
        │           │               └── dev_hard_start_xmit(skb, dev, txq)
        │           └── 若入队失败 → skb_to_free(skb)
        │
        └── dev_hard_start_xmit(skb, dev, txq)
            │
            ├── 若 skb 启用了 GSO：
            │   └── dev_gso_segment(skb) → skb_gso_segment()
            │       └── 对每个 GSO 段调用 xmit_one()
            │
            └── xmit_one(skb, dev, txq, ...)
                └── netdev_start_xmit(skb, dev, txq)
                    └── dev->netdev_ops->ndo_start_xmit(skb, dev)
                        └── e1000_xmit_frame(skb, dev)  → 驱动入口
```

#### 2.6.2 Qdisc 流量控制

Qdisc (Queueing Discipline) 是 Linux 流量控制的核心，支持多种调度算法：

```
                  ┌──────────────────────┐
                  │      dev_queue_xmit()  │
                  │        选择 TX 队列     │
                  └──────────┬───────────┘
                             │
                  ┌──────────▼───────────┐
                  │      __dev_xmit_skb() │
                  └──────────┬───────────┘
                             │
              ┌──────────────┴──────────────┐
              │                              │
              ▼                              ▼
    ┌─────────────────┐          ┌──────────────────────┐
    │   Qdisc 入队     │          │  noqueue (直接发送)   │
    │  enqueue(skb)    │          │  dev_hard_start_xmit │
    └────────┬────────┘          └──────────────────────┘
             │
             ▼
    ┌─────────────────┐
    │  Qdisc 出队调度  │
    │  __qdisc_run()   │
    │  ┌────────────┐  │
    │  │pfifo_fast  │  │  三种优先级 Band 0/1/2
    │  │fq_codel    │  │  公平排队 + CoDel AQM
    │  │fq          │  │  公平排队 (TSO 友好)
    │  │htb         │  │  层次令牌桶
    │  │bfifo       │  │  简单 FIFO
    │  └────────────┘  │
    └────────┬─────────┘
             │
             ▼
    ┌─────────────────┐
    │  sch_direct_xmit │
    │  dev_hard_start  │
    └─────────────────┘
```

### 2.7 驱动发送层

#### 2.7.1 e1000_xmit_frame

定义在 [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) (约 3097 行)。

```
e1000_xmit_frame(skb, netdev)
    │
    ├── eth_skb_pad(skb)  → 填充短包到 60 字节（以太网最小帧长）
    │
    ├── 计算 TSO 参数：
    │   ├── mss = skb_shinfo(skb)->gso_size
    │   └── 根据 mss 调整 max_per_txd
    │
    ├── 计算所需描述符数量：
    │   ├── count++ (offload context 描述符，若 TSO)
    │   ├── count += TXD_USE_COUNT(skb_headlen(skb))  → 头部数据
    │   └── count += TXD_USE_COUNT(frag->size)        → 各分片
    │
    ├── e1000_maybe_stop_tx(tx_ring, count)  → 检查 TX 环空间
    │   └── 若空间不足 → 停用队列 (netif_stop_queue)，返回 NETDEV_TX_BUSY
    │
    ├── e1000_tx_map()  → DMA 映射并填充描述符
    │   ├── e1000_tx_queue()  → 填充描述符数据
    │   │   ├── 设置 buffer_addr (DMA 地址)
    │   │   ├── 设置 length (数据长度)
    │   │   ├── 设置 cmd (EOP, RS, IC, IFCS 等标志)
    │   │   └── 设置 cso/css (校验和偏移)
    │   │
    │   ├── dma_map_single()  → 映射 skb->data
    │   └── dma_map_page()    → 映射各 frag 分片
    │
    ├── wmb()  → 写内存屏障，确保描述符写入完成才通知硬件
    │
    └── writel(i, hw->hw_addr + tx_ring->tdt)  → 更新 TDT 寄存器
        └── 硬件 DMA 引擎检测到 TDT 变化，开始传输
```

#### 2.7.2 TX 描述符环结构

```
                  TX 描述符环 (DMA 一致内存)
                 ┌──────────────────────────┐
                 │  描述符 0                 │
                 │  buffer_addr: 0x7f...100 │──────► skb->data
                 │  length: 1460            │       (DMA 映射)
                 │  cmd: EOP | RS | IC      │
                 │  status: 0               │
                 ├──────────────────────────┤
                 │  描述符 1                 │
                 │  buffer_addr: 0x7f...200 │──────► skb_frag 0
                 │  length: 540             │       (DMA 映射)
                 │  cmd: EOP | RS | IC      │
                 │  status: 0               │
                 ├──────────────────────────┤
                 │  ...                     │
                 ├──────────────────────────┤
                 │  描述符 N-1               │
                 │  (空闲)                   │
                 └──────────────────────────┘
                        ▲            ▲
                        │            │
                     TDH 指针     TDT 指针
                   (硬件读取)    (驱动写入)
```

**关键寄存器交互**：
- `TDH` (Transmit Descriptor Head)：硬件消费完描述符后更新，指向硬件正在处理的描述符
- `TDT` (Transmit Descriptor Tail)：驱动写入，通知硬件有新数据待发送
- 当 `TDH == TDT` 时，TX 环为空

### 2.8 发送完成清理

```
e1000_clean_tx_irq(adapter, tx_ring)  [e1000_main.c:3827]
    │
    ├── 读取 TDH 寄存器 → 确定硬件已处理的描述符位置
    │
    └── while (i != tx_ring->next_to_use) 循环
        │
        ├── 检查描述符 status 中的 DD (Descriptor Done) 位
        │   ├── DD=0 → 硬件尚未处理完，退出循环
        │   └── DD=1 → 处理完成
        │
        ├── dma_unmap_single() / dma_unmap_page()  → 解除 DMA 映射
        │
        ├── dev_kfree_skb_any(skb)  → 释放 skb（引用计数归零）
        │
        └── 更新 buffer_info 状态
    │
    ├── tx_ring->next_to_clean = i  → 更新清理指针
    │
    └── netif_wake_queue(dev)  → 若之前被停用，重新唤醒 TX 队列
```

---

## 3. 接收路径

### 3.1 接收路径总览

数据从网线到达用户态 `read(fd, buf, len)` 的完整旅程：

```
┌─────────────────────────────────────────────────────┐
│ 硬件行为                                              │
│ 1. PHY 从 RJ45 接收模拟信号，解码为数字位流             │
│ 2. 自动协商速率和双工模式                               │
│ 3. MAC 检测帧起始定界符 (SFD)，开始接收帧               │
│ 4. 检查目标 MAC 地址是否匹配 (或广播/多播/混杂模式)     │
│ 5. CRC 校验                                           │
│ 6. DMA 将数据写入 RX 描述符指向的缓冲区                 │
│ 7. 写入完成后，设置描述符 DD 位和 status 字段          │
│ 8. 更新 RDH 指针                                       │
│ 9. 触发中断 (ICR 中相应位置位)                         │
└─────────────────────────────────────────────────────┘
    │
    ▼
e1000_intr()                    [e1000_main.c]
    │  [读取 ICR 识别中断源，禁用中断]
    ▼
__napi_schedule()               [net/core/dev.c]
    │  [将 napi 结构加入当前 CPU 的 poll_list]
    ▼
NET_RX_SOFTIRQ 软中断           [net/core/dev.c]
    │  [net_rx_action() 遍历 poll_list]
    ▼
e1000_clean()                   [e1000_main.c]
    │  [NAPI poll 回调：清理 TX + 处理 RX]
    ▼
e1000_clean_rx_irq()            [e1000_main.c]
    │  [遍历 RX 描述符，构建 skb]
    ▼
e1000_receive_skb()             [e1000_main.c]
    │  [eth_type_trans() 设置协议类型]
    ▼
napi_gro_receive()              [net/core/gro.c]
    │  [GRO 合并：将多个小包合并为一个大包]
    ▼
netif_receive_skb()             [net/core/dev.c]
    │  [设备层接收入口]
    ▼
__netif_receive_skb_core()      [net/core/dev.c]
    │  [XDP → tc ingress → rx_handler → 协议分派]
    ▼
ip_rcv()                        [net/ipv4/ip_input.c]
    │  [IP 层入口：校验和、Netfilter NF_INET_PRE_ROUTING]
    ▼
ip_rcv_finish()                 [net/ipv4/ip_input.c]
    │  [路由查找：确定是本地递送还是转发]
    ▼
ip_local_deliver()              [net/ipv4/ip_input.c]
    │  [Netfilter NF_INET_LOCAL_IN]
    ▼
ip_local_deliver_finish()       [net/ipv4/ip_input.c]
    │  [按协议分派：TCP/UDP/ICMP]
    ▼
tcp_v4_rcv()                    [net/ipv4/tcp_ipv4.c]
    │  [TCP 入口：socket 查找、校验和、backlog]
    ▼
tcp_v4_do_rcv()                 [net/ipv4/tcp_ipv4.c]
    │  [根据连接状态分派]
    ▼
tcp_rcv_established()           [net/ipv4/tcp_input.c]
    │  [快速路径头部预测]
    ▼
tcp_data_queue()                [net/ipv4/tcp_input.c]
    │  [数据入队到 sk_receive_queue]
    ▼
sk->sk_data_ready(sk)           [net/core/sock.c]
    │  [sock_def_readable() 唤醒等待进程]
    ▼
┌─────────────────────────────────────────────────────┐
│ 用户态 read() 返回                                    │
│ 1. 进程从等待队列被唤醒                                │
│ 2. __sys_recvfrom() → sock_recvmsg()                  │
│ 3. inet_recvmsg() → tcp_recvmsg()                     │
│ 4. 从 sk_receive_queue 取出 skb                      │
│ 5. 拷贝数据到用户空间 (copy_to_user)                   │
│ 6. 释放 skb                                           │
└─────────────────────────────────────────────────────┘
```

### 3.2 硬件接收与中断处理

#### 3.2.1 硬件接收过程

```
网线上信号到达
    │
    ▼
[PHY 层]
    ├── 接收模拟信号，自动增益控制 (AGC)
    ├── 解码 (10Base-T: Manchester, 100Base-T: MLT-3, 1000Base-T: 4D-PAM5)
    ├── 自动协商检测 (Auto-Negotiation)
    └── 恢复时钟和数据
    │
    ▼
[MAC 层]
    ├── 检测帧起始定界符 (SFD: 0xAB)
    ├── 接收目标 MAC 地址
    ├── 地址过滤 (UCST/MCST/BCST 匹配)
    ├── 接收数据 + 填充到 FIFO
    ├── CRC 校验 (若失败则丢弃)
    └── 检查帧长度合法性
    │
    ▼
[DMA 引擎]
    ├── 从 FIFO 读取数据
    ├── 检查 RX 描述符环是否有空闲描述符
    ├── 通过 PCIe 总线将数据写入系统内存
    ├── 更新 RX 描述符的 status 字段 (DD=1)
    └── 写 E1000_ICR 寄存器触发中断
```

#### 3.2.2 中断处理函数

定义在 [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) (约 3746 行)。

```
e1000_intr(irq, data)
    │
    ├── icr = er32(ICR)  → 读取中断原因寄存器（读清除）
    │
    ├── if (icr == 0) → return IRQ_NONE  (非本设备中断)
    │
    ├── if (icr & E1000_ICR_LSC)  → 链路状态变化
    │   ├── hw->get_link_status = 1
    │   └── schedule_delayed_work(&adapter->watchdog_task, 1)
    │
    ├── if (icr & E1000_ICR_RXSEQ)  → RX 序列错误
    │   └── schedule_work(&adapter->reset_task)
    │
    ├── ew32(IMC, ~0)  → 禁用所有中断（防止中断风暴）
    ├── E1000_WRITE_FLUSH()  → 写刷新（确保 IMC 写入完成）
    │
    └── if (napi_schedule_prep(&adapter->napi))
        └── __napi_schedule(&adapter->napi)
            └── 将 adapter->napi 加入当前 CPU 的 softnet_data->poll_list
                └── __raise_softirq_irqoff(NET_RX_SOFTIRQ)  → 触发软中断
```

### 3.3 NAPI 轮询机制

#### 3.3.1 软中断处理

```
NET_RX_SOFTIRQ 软中断
    │
    ▼
net_rx_action(softirq_data)  [net/core/dev.c]
    │
    ├── list = &sd->poll_list  → 获取当前 CPU 的 NAPI 列表
    │
    ├── sd->time = jiffies + 2  → 设定软中断时间预算
    │
    └── while (list 不为空 && 有预算)
        │
        ├── napi = list_first_entry(list, struct napi_struct, poll_list)
        │
        ├── work = napi->poll(napi, budget)  → 调用驱动的 poll 函数
        │   └── e1000_clean(napi, budget)
        │
        ├── 若 work == budget (预算用完，还有更多包)：
        │   └── 继续轮询，下次软中断再处理
        │
        └── 若 work < budget (包处理完)：
            └── napi_complete_done(napi, work)
                └── 驱动重新启用中断
```

#### 3.3.2 e1000_clean (NAPI poll 回调)

定义在 [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) (约 3827 行)。

```
e1000_clean(napi, budget)
    │
    ├── adapter = container_of(napi, struct e1000_adapter, napi)
    │
    ├── e1000_clean_tx_irq(adapter, adapter->tx_ring)  → 清理 TX 完成
    │
    ├── adapter->clean_rx(adapter, adapter->rx_ring, &work_done, budget)
    │   ├── e1000_clean_rx_irq()  → 标准包处理
    │   └── e1000_clean_jumbo_rx_irq()  → 巨帧包处理
    │
    ├── if (work_done == budget)  → 还有更多包待处理
    │   └── return budget  (保持 NAPI 调度，不启用中断)
    │
    └── else  → 包处理完毕
        ├── napi_complete_done(napi, work_done)  → 退出 NAPI 轮询
        ├── e1000_set_itr(adapter)  → 动态调整中断节流
        └── e1000_irq_enable(adapter)  → 重新启用硬件中断
```

### 3.4 e1000_clean_rx_irq 接收处理

定义在 [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) (约 4356 行)。

```
e1000_clean_rx_irq(adapter, rx_ring, work_done, work_to_do)
    │
    └── while (rx_desc->status & E1000_RXD_STAT_DD)  → 循环处理所有 DD 描述符
        │
        ├── dma_rmb()  → DMA 读内存屏障 (确保读取最新数据)
        │
        ├── 读取 status、length、csum 等字段
        │
        ├── e1000_copybreak()  → 小包拷贝优化 (默认 256 字节阈值)
        │   ├── 若数据长度 < copybreak 阈值：
        │   │   ├── 分配新 skb
        │   │   ├── 从原缓冲区拷贝数据到新 skb
        │   │   └── 原缓冲区 re-use (避免重新分配)
        │   │
        │   └── 否则：
        │       └── napi_build_skb()  → 直接包装缓冲区为 skb
        │           └── 原缓冲区需要重新分配
        │
        ├── 检查 EOP (End of Packet) 位：
        │   ├── 未设置 → 多描述符帧 (Jumbo Frame)
        │   │   ├── 启用 discarding 标志
        │   │   └── 继续下一个描述符
        │   └── 已设置 → 包完整，继续处理
        │
        ├── 检查 errors 字段：
        │   ├── errors & E1000_RXD_ERR_CE  → CRC 错误，丢弃
        │   ├── errors & E1000_RXD_ERR_SE  → 符号错误，丢弃
        │   ├── errors & E1000_RXD_ERR_TCPE  → TCP 校验和错误
        │   └── 无错误 → 继续
        │
        ├── skb_put(skb, length - 4)  → 设置 skb 数据长度（减去 FCS 4 字节）
        │
        ├── e1000_rx_checksum(adapter, skb, rx_desc)  → 硬件校验和验证
        │   └── 若硬件校验和通过，设置 skb->ip_summed = CHECKSUM_UNNECESSARY
        │
        ├── skb->protocol = eth_type_trans(skb, netdev)  → 确定 L3 协议
        │
        ├── e1000_receive_skb(adapter, netdev, skb, rx_desc)  → 递送
        │   └── napi_gro_receive(&adapter->napi, skb)  → 送入 GRO
        │
        └── e1000_alloc_rx_buffers()  → 重新分配 RX 缓冲区
```

### 3.5 GRO (Generic Receive Offload)

定义在 [net/core/gro.c](file:///home/louis/code/linux/net/core/gro.c)。

```
napi_gro_receive(napi, skb)
    │
    ├── 检查 skb 是否适合 GRO：
    │   ├── 设备特性支持 NETIF_F_GRO
    │   └── skb->len > 0
    │
    ├── gro_result = dev_gro_receive(napi, skb)
    │   │
    │   ├── 遍历 napi->gro_hash[] 哈希表
    │   │
    │   ├── 对于每个匹配的 GRO 流：
    │   │   ├── 检查是否可以合并 (同流、同协议、时间窗口内)
    │   │   ├── gro_cells_receive() → 合并到现有 skb
    │   │   └── 合并成功 → 返回 GRO_MERGED
    │   │
    │   └── 若没有匹配的流或不满足合并条件：
    │       ├── gro_normal_one() → 添加到正常处理队列
    │       └── 返回 GRO_NORMAL
    │
    └── 若返回 GRO_NORMAL：
        └── netif_receive_skb(skb)  → 正常路径
```

**GRO 合并示意图**：

```
没有 GRO                          有 GRO
┌─────┐  ┌─────┐  ┌─────┐       ┌─────────────────┐
│Pkt 1│  │Pkt 2│  │Pkt 3│       │    合并大包       │
├─────┤  ├─────┤  ├─────┤       │ ┌─────┬─────┬──┐  │
│TCP  │  │TCP  │  │TCP  │       │ │Pkt 1│Pkt 2│..│  │
│IP   │  │IP   │  │IP   │       │ └─────┴─────┴──┘  │
│ETH  │  │ETH  │  │ETH  │       └─────────────────┘
└─────┘  └─────┘  └─────┘
   3次协议栈处理                   1次协议栈处理
```

### 3.6 网络设备层接收

定义在 [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c)。

```
netif_receive_skb(skb)
    │
    └── __netif_receive_skb(skb)
        │
        └── __netif_receive_skb_core(skb, false)
            │
            ├── 1. XDP 处理 (若设备绑定了 XDP 程序)
            │   └── xdp_do_generic_redirect() → XDP_REDIRECT 等
            │
            ├── 2. tc ingress 钩子 (tc 分类器/动作)
            │   └── tcf_classify()
            │
            ├── 3. rx_handler (网桥/OVS 等)
            │   └── dev->rx_handler(skb) → br_handle_frame()
            │
            ├── 4. ptype_all → 原始套接字 (AF_PACKET, tcpdump)
            │   └── deliver_skb(skb, pt_prev, orig_dev)
            │
            └── 5. ptype_specific → 按协议类型分派
                │
                ├── skb->protocol == ETH_P_IP
                │   └── ip_rcv(skb)  → IPv4 处理
                │
                ├── skb->protocol == ETH_P_IPV6
                │   └── ipv6_rcv(skb)  → IPv6 处理
                │
                ├── skb->protocol == ETH_P_ARP
                │   └── arp_rcv(skb)  → ARP 处理
                │
                └── 其他协议 → 对应注册的 packet_type
```

### 3.7 IP 接收路径

#### 3.7.1 ip_rcv

定义在 [net/ipv4/ip_input.c](file:///home/louis/code/linux/net/ipv4/ip_input.c)。

```
ip_rcv(skb)
    │
    ├── 基本合法性检查：
    │   ├── skb->pkt_type == PACKET_HOST (非本机包丢弃)
    │   ├── pskb_may_pull(skb, sizeof(struct iphdr)) (头部长度检查)
    │   └── iphdr(skb)->version == 4 (IPv4 版本检查)
    │
    ├── skb_checksum_simple_validate(skb)  → IP 头部校验和验证
    │
    └── NF_HOOK(NFPROTO_IPV4, NF_INET_PRE_ROUTING, ...)
        │
        └── ip_rcv_finish(skb)  → Netfilter 钩子通过后调用
```

#### 3.7.2 ip_rcv_finish

```
ip_rcv_finish(skb)
    │
    ├── ip_rcv_finish_core(skb)  → 核心处理
    │   ├── skb_dst_set(skb, ...)  → 设置路由缓存
    │   └── 更新统计计数
    │
    └── dst_input(skb)
        │
        └── skb->dst->input(skb)  → 路由决定的目的函数
            │
            ├── 若目的地是本机 → ip_local_deliver(skb)
            │   │
            │   └── ip_local_deliver_finish(skb)
            │       │
            │       └── ip_protocol_deliver_rcu(skb)
            │           │
            │           ├── IPPROTO_TCP → tcp_v4_rcv(skb)
            │           ├── IPPROTO_UDP → udp_rcv(skb)
            │           ├── IPPROTO_ICMP → icmp_rcv(skb)
            │           └── RAW socket → raw_v4_input(skb)
            │
            └── 若是转发 → ip_forward(skb)
                │
                ├── TTL 递减检查
                ├── Netfilter NF_INET_FORWARD
                └── ip_forward_finish() → dst_output()
```

### 3.8 TCP 接收路径

#### 3.8.1 tcp_v4_rcv

定义在 [net/ipv4/tcp_ipv4.c](file:///home/louis/code/linux/net/ipv4/tcp_ipv4.c) (约 2147 行)。

```
tcp_v4_rcv(skb)
    │
    ├── 1. 基本校验
    │   ├── skb->pkt_type != PACKET_HOST → discard
    │   └── __TCP_INC_STATS(net, TCP_MIB_INSEGS)
    │
    ├── 2. TCP 头部长度检查
    │   ├── pskb_may_pull(skb, sizeof(struct tcphdr))
    │   └── th->doff >= sizeof(struct tcphdr) / 4
    │
    ├── 3. 校验和检查
    │   └── skb_checksum_init(skb, IPPROTO_TCP, inet_compute_pseudo)
    │
    ├── 4. Socket 查找
    │   └── sk = __inet_lookup_skb(skb, __tcp_hdrlen(th),
    │                                th->source, th->dest, ...)
    │       ├── 未找到 → 发送 RST (no_tcp_socket)
    │       └── 找到 → 继续
    │
    ├── 5. TIME_WAIT 处理
    │   └── if (sk->sk_state == TCP_TIME_WAIT) → tcp_timewait_process()
    │
    ├── 6. TCP_NEW_SYN_RECV 处理 (三次握手)
    │   └── tcp_check_req() → 完成握手
    │
    ├── 7. 安全检查 (BPF 过滤)
    │   └── tcp_filter(sk, skb, ...)
    │
    ├── 8. 填充 TCP 控制块
    │   └── tcp_v4_fill_cb(skb, iph, th)
    │
    └── 9. 分派处理
        │
        ├── if (sk->sk_state == TCP_LISTEN)
        │   └── tcp_v4_do_rcv(sk, skb)  → 监听 socket 处理 SYN
        │
        └── else
            ├── bh_lock_sock_nested(sk)  → 软中断中获取锁
            │
            ├── if (!sock_owned_by_user(sk))
            │   └── tcp_v4_do_rcv(sk, skb)  → 快速路径
            │
            └── else  → 用户态持有锁
                └── tcp_add_backlog(sk, skb, &drop_reason)
                    └── 放入 sk_backlog 队列，延迟处理
```

#### 3.8.2 tcp_v4_do_rcv

```
tcp_v4_do_rcv(sk, skb)
    │
    ├── if (sk->sk_state == TCP_ESTABLISHED)
    │   └── tcp_rcv_established(sk, skb)  → 快速路径
    │       return 0
    │
    ├── if (tcp_checksum_complete(skb))  → 校验和完整检查
    │   goto csum_err
    │
    ├── if (sk->sk_state == TCP_LISTEN)
    │   └── tcp_v4_cookie_check(sk, skb) → tcp_conn_request() 等
    │
    └── else  → 其他状态
        └── tcp_rcv_state_process(sk, skb)
            └── 根据 sk->sk_state 的 switch 分派
                ├── TCP_SYN_SENT → tcp_rcv_synsent_state_process()
                ├── TCP_FIN_WAIT1 → 处理 FIN/ACK
                ├── TCP_CLOSE_WAIT → 处理 FIN
                └── ...
```

#### 3.8.3 tcp_rcv_established (快速路径)

定义在 [net/ipv4/tcp_input.c](file:///home/louis/code/linux/net/ipv4/tcp_input.c) (约 6519 行)。

```
tcp_rcv_established(sk, skb)
    │
    ├── 头部预测 (Van Jacobson 算法)
    │   │
    │   └── if ((tcp_flag_word(th) & TCP_HP_BITS) == tp->pred_flags
    │           && TCP_SKB_CB(skb)->seq == tp->rcv_nxt
    │           && !after(TCP_SKB_CB(skb)->ack_seq, tp->snd_nxt))
    │       │
    │       ├── 快速路径 - 头部预测命中
    │       │
    │       ├── if (len <= tcp_header_len)  → 纯 ACK 包
    │       │   ├── tcp_ack(sk, skb, flag)  → 处理 ACK
    │       │   ├── __kfree_skb(skb)  → 释放 skb
    │       │   └── tcp_data_snd_check(sk)  → 检查是否可发新数据
    │       │
    │       └── else  → 数据包
    │           ├── 时间戳检查 (若启用)
    │           └── tcp_data_queue(sk, skb)  → 数据入队
    │
    └── else  → 慢速路径 (头部预测未命中)
        ├── tcp_ack(sk, skb, FLAG_SLOWPATH)  → 慢路径 ACK 处理
        └── tcp_data_queue(sk, skb)  → 数据入队
```

#### 3.8.4 tcp_data_queue (数据入队)

定义在 [net/ipv4/tcp_input.c](file:///home/louis/code/linux/net/ipv4/tcp_input.c)。

```
tcp_data_queue(sk, skb)
    │
    ├── if (TCP_SKB_CB(skb)->seq == tp->rcv_nxt)
    │   │  → 有序数据：期望的序列号
    │   │
    │   ├── tcp_try_rmem_schedule(sk, skb, skb->truesize)
    │   │   └── 若内存不足 → 设置零窗口标志，跳过
    │   │
    │   ├── eaten = tcp_queue_rcv(sk, skb, &fragstolen)
    │   │   └── 将数据放入 sk_receive_queue
    │   │       ├── 若 skb 可被 sock 直接"吃掉"（低延迟）
    │   │       └── 否则 skb 入队等待 recvmsg 读取
    │   │
    │   ├── if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
    │   │   └── tcp_fin(sk)  → 处理 FIN 标志
    │   │
    │   └── if (!RB_EMPTY_ROOT(&tp->out_of_order_queue))
    │       └── tcp_ofo_queue(sk)  → 处理乱序队列中的新数据
    │           └── 检查是否有之前乱序的包现在可以入队
    │
    ├── else if (after(TCP_SKB_CB(skb)->seq, tp->rcv_nxt))
    │   │  → 乱序数据：序列号大于期望值
    │   └── tcp_data_queue_ofo(sk, skb)
    │       └── 插入到 out_of_order_queue (红黑树)
    │           ├── tcp_sack_new_ofo_skb()  → 更新 SACK 块
    │           └── tcp_grow_window()  → 调整窗口
    │
    └── else  → 重复包或窗口外数据
        └── tcp_data_queue_ofo(sk, skb) 或直接丢弃
```

#### 3.8.5 数据就绪通知

```
tcp_data_queue() 将数据入队后
    │
    └── tcp_data_ready(sk)
        │
        └── sk->sk_data_ready(sk)  → 默认为 sock_def_readable()
            │
            ├── wake_up_interruptible_sync_poll(sk->sk_wq, EPOLLIN)
            │   └── 唤醒等待在 poll/select/epoll 上的进程
            │
            └── sk_wake_async(sk, SOCK_WAKE_WAITD, POLL_IN)
                └── 发送 SIGIO 信号 (若设置了 FASYNC)
```

### 3.9 用户态接收

```
用户态: read(fd, buf, len) 或 recvmsg(fd, msg, flags)
    │
    ▼
__sys_recvfrom()  [net/socket.c]
    │
    ├── sockfd_lookup_light(fd)  → 通过 fd 获取 struct socket
    │
    └── sock_recvmsg(sock, msg, flags)
        │
        └── sock->ops->recvmsg(sock, msg, ...)
            │
            └── inet_recvmsg(sock, msg, ...)
                │
                └── sk->sk_prot->recvmsg(sk, msg, len, ...)
                    │
                    └── tcp_recvmsg(sk, msg, len, ...)  [tcp.c]
                        │
                        ├── lock_sock(sk)  → 获取 socket 锁
                        │
                        ├── while (目标长度未满足)
                        │   │
                        │   ├── skb = skb_peek(&sk->sk_receive_queue)
                        │   │   → 从接收队列头部取 skb
                        │   │
                        │   ├── __skb_datagram_iter(skb, offset, ...)
                        │   │   → skb_copy_datagram_msg()
                        │   │       → copy_to_user() 拷贝到用户空间
                        │   │
                        │   ├── sk_eat_skb(sk, skb)  → 消费完的 skb 释放
                        │   │
                        │   └── 若队列为空且无更多数据:
                        │       └── sk_wait_data(sk, &timeo)  → 阻塞等待
                        │
                        ├── release_sock(sk)  → 释放锁
                        │
                        └── return len  → 返回实际读取的字节数
```

---

## 4. 关键硬件-软件交互点

### 4.1 DMA (Direct Memory Access)

DMA 是网卡硬件直接访问系统内存的机制，是数据通路中最重要的硬件-软件交互点。

#### 4.1.1 DMA 映射流程

**发送方向**：

```
e1000_xmit_frame()
    │
    ├── skb->data 的 DMA 映射：
    │   └── dma = dma_map_single(dev, skb->data, skb_headlen(skb), DMA_TO_DEVICE)
    │       ├── 将虚拟地址转换为物理地址
    │       ├── 刷新/无效 CPU 缓存 (若需要)
    │       └── 返回 DMA 地址 → 写入 TX 描述符的 buffer_addr
    │
    └── skb frag 分片的 DMA 映射：
        └── dma = dma_map_page(dev, page, offset, size, DMA_TO_DEVICE)
            └── 类似处理，返回 DMA 地址
```

**接收方向**：

```
e1000_alloc_rx_buffers()
    │
    └── dma = dma_map_single(dev, skb->data, skb->len, DMA_FROM_DEVICE)
        ├── 将 skb 数据缓冲区映射给 DMA
        └── 写入 RX 描述符的 buffer_addr
```

#### 4.1.2 DMA 地址拓扑

```
                    ┌──────────────────────┐
                    │    CPU / 系统内存      │
                    │                      │
                    │  ┌────────────────┐  │
                    │  │  TX 描述符环    │  │  ← DMA 一致内存 (dma_alloc_coherent)
                    │  │  (DMA 可读)     │  │
                    │  └────────────────┘  │
                    │  ┌────────────────┐  │
                    │  │  RX 描述符环    │  │  ← DMA 一致内存
                    │  │  (DMA 可写)     │  │
                    │  └────────────────┘  │
                    │  ┌────────────────┐  │
                    │  │  TX 数据缓冲区  │  │  ← DMA 流式映射 (dma_map_single)
                    │  │  (DMA 读取)     │  │
                    │  └────────────────┘  │
                    │  ┌────────────────┐  │
                    │  │  RX 数据缓冲区  │  │  ← DMA 流式映射 (dma_map_single)
                    │  │  (DMA 写入)     │  │
                    │  └────────────────┘  │
                    └────────┬─────────────┘
                             │ PCIe 总线
                    ┌────────▼─────────────┐
                    │     网卡硬件          │
                    │  ┌────────────────┐  │
                    │  │  DMA 引擎       │  │
                    │  │  ┌──────────┐  │  │
                    │  │  │ TX 引擎  │  │  │
                    │  │  │ 读取描述符│  │  │
                    │  │  │ 读取数据  │  │  │
                    │  │  └──────────┘  │  │
                    │  │  ┌──────────┐  │  │
                    │  │  │ RX 引擎  │  │  │
                    │  │  │ 读取描述符│  │  │
                    │  │  │ 写入数据  │  │  │
                    │  │  └──────────┘  │  │
                    │  └────────────────┘  │
                    └──────────────────────┘
```

#### 4.1.3 DMA 一致性与内存屏障

```c
// 发送时的写内存屏障 (确保描述符写入完成后再通知硬件)
wmb();  // 写内存屏障
writel(i, hw->hw_addr + tx_ring->tdt);  // 更新 TDT

// 接收时的读内存屏障 (确保读取到的是 DMA 写入后的最新数据)
dma_rmb();  // DMA 读内存屏障
status = rx_desc->status;  // 读取 DD 位
```

### 4.2 MMIO (Memory-Mapped I/O)

MMIO 是驱动与硬件寄存器通信的主要方式。

#### 4.2.1 MMIO 映射

```c
// e1000_probe() 中建立 MMIO 映射
hw->hw_addr = pci_ioremap_bar(pdev, BAR_0);
// hw_addr 是一个 void __iomem * 指针
```

#### 4.2.2 MMIO 读写

```c
// 读 MMIO 寄存器
#define er32(reg)   readl(hw->hw_addr + reg)

// 写 MMIO 寄存器
#define ew32(reg, value)  writel((value), hw->hw_addr + reg)

// 写刷新 (确保前面所有 MMIO 写都已到达硬件)
#define E1000_WRITE_FLUSH()  er32(STATUS)
```

#### 4.2.3 关键 MMIO 操作

**发送通知**：
```c
// 驱动写入 TDT 通知硬件有新数据
writel(tx_ring->next_to_use, hw->hw_addr + E1000_TDT);
// 硬件检测到 TDT 变化，开始 DMA 传输
```

**中断控制**：
```c
// 禁用所有中断
ew32(IMC, ~0);
E1000_WRITE_FLUSH();

// 启用特定中断
ew32(IMS, E1000_ICS_RXT0 | E1000_ICS_TXQE);
```

**读取中断状态**：
```c
// 读取中断原因寄存器 (读清除)
icr = er32(ICR);
// 读取后硬件自动清除 ICR 中的对应位
```

### 4.3 中断

#### 4.3.1 中断类型

| 中断类型 | 描述 | e1000 支持 |
|---------|------|-----------|
| 传统 INTx | PCI 共享中断线 | 是 (fallback) |
| MSI | Message Signaled Interrupt | 是 (首选) |
| MSI-X | 多队列 MSI | 部分 e1000 变体支持 |

#### 4.3.2 中断处理流程

```
硬件触发中断
    │
    ▼
[CPU 中断控制器]
    │
    ▼
do_IRQ()  [arch/x86/kernel/irq.c]
    │
    ▼
handle_irq()  → 调度到对应的中断处理函数
    │
    ▼
e1000_intr(irq, data)  → 读取 ICR，识别中断源
    │
    ├── 链路状态变化 (LSC)
    │   └── schedule_delayed_work(&watchdog_task, 1)
    │
    ├── 接收事件 (RXT0)
    │   └── __napi_schedule()  → 调度 NAPI
    │
    └── 发送完成 (TXQE)
        └── __napi_schedule()  → 调度 NAPI
```

#### 4.3.3 中断-NAPI 协同

```
中断使能状态       NAPI 状态           数据包
─────────────────────────────────────────────
[中断使能]         [NAPI 未调度]
    │                   │
    │  包到达 ──────────┤
    │                   │
    ▼                   │
中断触发               │
ew32(IMC) 禁中断       │
__napi_schedule() ─────► [NAPI 已调度]
    │                   │
    │                   ▼
    │            net_rx_action() 轮询
    │                   │
    │            e1000_clean() 返回 budget
    │                   │  (更多包待处理)
    │                   │
    │           [NAPI 保持调度]
    │                   │
    │           包到达 → 无需中断，直接轮询处理
    │                   │
    │           没有更多包了
    │                   │
    │           napi_complete_done()
    │                   │
    │◄── e1000_irq_enable() 重新使能中断
    │                   │
[中断使能]         [NAPI 未调度]
```

### 4.4 描述符环

描述符环是驱动和硬件之间共享的环形缓冲区，是数据交换的核心数据结构。

#### 4.4.1 TX 描述符环

```
                    TX 描述符环 (DMA 一致内存)
                    count = 256 (默认)

     next_to_clean                     next_to_use
     (驱动清理指针)                    (驱动生产指针)
         │                                  │
         ▼                                  ▼
    ┌──────┬──────┬──────┬──────┬──────┬──────┐
    │ Done │ Done │ Free │ Free │ Free │ Free │
    │  Pkt │  Pkt │      │      │      │      │
    └──────┴──────┴──────┴──────┴──────┴──────┘
         ▲
         │
        TDH (硬件消费指针)
```

**关键指针**：
- `next_to_use`：驱动写入，指向下一个要填充的描述符
- `next_to_clean`：驱动读取，指向下一个要检查完成的描述符
- `TDH`：硬件更新，指向硬件正在处理的描述符（硬件消费端）
- `TDT`：驱动写入，驱动更新此指针通知硬件有新数据（硬件生产端）

#### 4.4.2 RX 描述符环

```
                    RX 描述符环 (DMA 一致内存)
                    count = 256 (默认)

     next_to_clean                     next_to_use
     (驱动消费指针)                    (驱动生产指针)
         │                                  │
         ▼                                  ▼
    ┌──────┬──────┬──────┬──────┬──────┬──────┐
    │ Done │ Done │ Free │ Free │ Free │ Free │
    │  Pkt │  Pkt │      │      │      │      │
    └──────┴──────┴──────┴──────┴──────┴──────┘
         ▲
         │
        RDH (硬件消费指针)
```

**关键指针**：
- `next_to_use`：驱动写入，指向下一个要分配缓冲区的描述符
- `next_to_clean`：驱动读取，指向下一个要检查 DD 位的描述符
- `RDH`：硬件更新，指向硬件已读取的描述符（硬件消费端）
- `RDT`：驱动写入，驱动更新此指针通知硬件有新缓冲区可用（硬件生产端）

#### 4.4.3 描述符格式

**TX 描述符** ([e1000_hw.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.h))：

```c
struct e1000_tx_desc {
    __le64 buffer_addr;   // 数据缓冲区 DMA 地址
    union {
        struct {
            __le16 length;  // 数据长度 (字节)
            u8 cso;         // 校验和偏移
            u8 cmd;         // 命令: EOP(包结束), RS(报告状态), IC(插入校验和), IFCS(插入FCS)
        } flags;
        __le32 data;
    } lower;
    union {
        struct {
            u8 status;      // 状态: DD(描述符完成)
            u8 css;         // 校验和开始
            __le16 special; // VLAN 标签等
        } fields;
        __le32 data;
    } upper;
};
```

**RX 描述符**：

```c
struct e1000_rx_desc {
    __le64 buffer_addr;   // 数据缓冲区 DMA 地址 (硬件写入数据)
    __le16 length;         // DMA 写入的数据长度
    __le16 csum;           // 硬件计算的校验和
    u8 status;             // 状态: DD(完成), EOP(包结束), TCPCS(TCP校验和), IPCS(IP校验和)
    u8 errors;             // 错误: CE(CRC), SE(符号), TCPE(TCP校验和), IPE(IP校验和)
    __le16 special;        // VLAN 标签等
};
```

---

## 5. 核心数据结构 sk_buff 在各层的变换

### 5.1 sk_buff 概述

`struct sk_buff` 是贯穿整个网络协议栈的数据包表示，定义在 [include/linux/skbuff.h](file:///home/louis/code/linux/include/linux/skbuff.h) (约 885 行)。

```
sk_buff 内存布局
┌─────────────────────────────────────────────────────────────────────┐
│ head                                                                 │
│  ┌───────────┬───────────┬───────────┬───────────┬────────────────┐│
│  │ headroom  │ L2 头     │ L3 头     │ L4 头     │  payload       ││
│  │ (预留)    │ (mac)     │ (net)     │ (trans)   │  (数据)        ││
│  │           │           │           │           │                ││
│  │           │mac_header │net_header │trans_hdr  │ data           ││
│  │           │▼          │▼          │▼          │ ▼              ││
│  │           │           │           │           │                ││
│  │           │           │           │           │                ││
│  │           │           │           │           │ tail           ││
│  │           │           │           │           │ ▼              ││
│  └───────────┴───────────┴───────────┴───────────┴────────────────┘│
│ end                                                                  │
└─────────────────────────────────────────────────────────────────────┘
```

**关键指针操作宏**：

```c
// 在头部预留空间 (通常用于添加协议头)
skb_reserve(skb, len);      // 移动 data 指针，创建 headroom

// 在头部添加数据 (构建协议头时使用)
skb_push(skb, len);         // data 指针前移，增加头部

// 去掉头部数据 (解析协议头时使用)
skb_pull(skb, len);         // data 指针后移，减少头部

// 在尾部添加数据 (添加 payload 时使用)
skb_put(skb, len);          // tail 指针后移，增加数据

// 在尾部移除数据
skb_trim(skb, len);         // tail 指针前移，减少数据
```

### 5.2 发送路径中 sk_buff 的变换

```
发送路径各层对 skb 的操作

[用户空间]
    buf = "Hello World"  (用户态数据)
    │
    ▼
[TCP 层分配 skb]
    skb = alloc_skb(MAX_TCP_HEADER + len, GFP_KERNEL)
    skb_reserve(skb, MAX_TCP_HEADER)  → 预留头部空间
    skb_put(skb, len)                 → 放入数据
    skb_copy_from_user(skb, buf, len) → 拷贝数据
    │
    ▼
[TCP 层构建 TCP 头部]
    skb_push(skb, tcp_header_size)    → 为 TCP 头部腾空间
    // 填充 tcphdr: source, dest, seq, ack_seq, window, check
    th = (struct tcphdr *)skb->data;
    skb->transport_header = skb->data;
    │
    ▼
[IP 层构建 IP 头部]
    skb_push(skb, sizeof(struct iphdr))  → 为 IP 头部腾空间
    // 填充 iphdr: version, tos, ttl, protocol, saddr, daddr
    iph = (struct iphdr *)skb->data;
    skb->network_header = skb->data;
    │
    ▼
[邻居子系统/设备层构建 L2 头部]
    skb_push(skb, dev->hard_header_len)  → 为 MAC 头部腾空间
    // 填充 ethhdr: dmac, smac, protocol
    eth = (struct ethhdr *)skb->data;
    skb->mac_header = skb->data;
    │
    ▼
[驱动层]  → 通过 DMA 将 skb->data 指向的数据发送给硬件
```

**发送路径 skb 头部指针变化示意图**：

```
阶段1: TCP 分配 skb 后
┌──────────────────────────────────────────────────────┐
│ headroom         │         payload "Hello World"     │
│ ↑head            │         ↑data         ↑tail       │
│                  │                                   │
└──────────────────────────────────────────────────────┘

阶段2: TCP 构建头部后 (skb_push)
┌──────────────────────────────────────────────────────┐
│ headroom  │ TCP header  │ payload "Hello World"      │
│ ↑head     │ ↑data       │                ↑tail       │
│           │ ↑transport_header                        │
└──────────────────────────────────────────────────────┘

阶段3: IP 构建头部后 (skb_push)
┌──────────────────────────────────────────────────────┐
│ headroom  │ IP header │ TCP header │ payload          │
│ ↑head     │ ↑data     │            │        ↑tail     │
│           │ ↑network_header                           │
│           │           ↑transport_header               │
└──────────────────────────────────────────────────────┘

阶段4: MAC 构建头部后 (skb_push)
┌──────────────────────────────────────────────────────┐
│ head │ MAC hdr │ IP header │ TCP header │ payload     │
│ ↑head│ ↑data   │           │            │    ↑tail    │
│      │ ↑mac_header                                   │
│      │         ↑network_header                        │
│      │                   ↑transport_header            │
└──────────────────────────────────────────────────────┘

最终 DMA 发送时，硬件读取 data 到 tail 之间的所有数据
```

### 5.3 接收路径中 sk_buff 的变换

```
接收路径各层对 skb 的操作 (逆过程)

[驱动层]  → DMA 将数据写入 RX 缓冲区，napi_build_skb() 包装为 skb
    skb->data 指向 L2 头部起始位置
    skb->tail 指向数据结束位置
    │
    ▼
[驱动层]  eth_type_trans(skb, dev)
    skb_pull(skb, ETH_HLEN)       → 去掉 MAC 头部
    skb->mac_header 记录 MAC 头位置
    │
    ▼
[IP 层]  ip_rcv()
    skb_pull(skb, sizeof(struct iphdr))  → 去掉 IP 头部
    skb->network_header 记录 IP 头位置
    │
    ▼
[TCP 层]  tcp_v4_rcv()
    skb_pull(skb, tcp_header_size)  → 去掉 TCP 头部
    skb->transport_header 记录 TCP 头位置
    │
    ▼
[TCP 层]  tcp_data_queue()
    skb 数据入队到 sk_receive_queue
    │
    ▼
[用户态]  tcp_recvmsg()
    skb_copy_datagram_msg()  → 将数据拷贝到用户空间
    kfree_skb()  → 释放 skb
```

**接收路径 skb 头部指针变化示意图**：

```
阶段1: 驱动 DMA 写入后，skb 刚构建
┌──────────────────────────────────────────────────────┐
│ MAC hdr │ IP header │ TCP header │ payload "Hello"   │
│ ↑data    │           │            │           ↑tail   │
│ ↑mac_header? (未设置)                                │
└──────────────────────────────────────────────────────┘

阶段2: eth_type_trans 后 (skb_pull 去掉 MAC 头)
┌──────────────────────────────────────────────────────┐
│ MAC hdr │ IP header │ TCP header │ payload "Hello"   │
│          ↑data      │            │           ↑tail   │
│ ↑mac_header                                          │
└──────────────────────────────────────────────────────┘

阶段3: ip_rcv 后 (skb_pull 去掉 IP 头)
┌──────────────────────────────────────────────────────┐
│ MAC hdr │ IP header │ TCP header │ payload "Hello"   │
│                     ↑data        │           ↑tail   │
│ ↑mac_header                                          │
│          ↑network_header                              │
└──────────────────────────────────────────────────────┘

阶段4: tcp_v4_rcv 后 (skb_pull 去掉 TCP 头)
┌──────────────────────────────────────────────────────┐
│ MAC hdr │ IP header │ TCP header │ payload "Hello"   │
│                                   ↑data      ↑tail   │
│ ↑mac_header                                          │
│          ↑network_header                              │
│                     ↑transport_header                  │
└──────────────────────────────────────────────────────┘

阶段5: copy_to_user 后，skb 释放
```

### 5.4 sk_buff 的克隆与拷贝

在协议栈中，skb 的克隆和拷贝用于以下场景：

```
场景1: TCP 重传 (skb_clone)
    tcp_transmit_skb() 中，若 clone_it=1：
        skb_clone(skb, gfp_mask)  → 创建轻量级副本
        └── 克隆的 skb 与原始 skb 共享数据区
            ├── 原始 skb：保留在重传队列中
            └── 克隆 skb：发送到 IP 层，释放后不影响原始 skb

场景2: AF_PACKET 抓包 (skb_clone)
    __netif_receive_skb_core() 中：
        deliver_skb(skb, pt_prev, ...)  → clone 后递送给原始套接字
        └── 抓包程序和正常协议栈各得到一份

场景3: 多播/广播 (skb_clone 或 skb_copy)
    每个接收 socket 需要独立的数据副本

场景4: 硬件 offload 失败时的回退 (skb_checksum_help)
    若硬件不支持校验和计算，内核需要软件计算
```

---

## 6. 性能优化机制

### 6.1 NAPI (New API)

NAPI 是 Linux 网络驱动的中断-轮询混合接收模型，定义在 [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c)。

#### 6.1.1 NAPI 工作原理

```
低负载时：中断模式 (低延迟)
    包到达 → 中断 → 处理 → 中断
    优点：延迟低，包到达即处理
    缺点：高负载时中断风暴

高负载时：轮询模式 (高吞吐)
    包到达 → 中断 → 禁用中断 → 轮询处理所有包 → 启用中断
    优点：批量处理，避免中断风暴
    缺点：处理完所有包后才启用中断，可能增加延迟

NAPI 动态切换
    处理完 budget 个包 → 保持轮询 (不启用中断)
    处理完所有包，work_done < budget → 退出轮询，启用中断
```

#### 6.1.2 NAPI 核心 API

```c
// 初始化 NAPI
netif_napi_add(dev, napi, poll_func, budget);
// 默认 budget = 64 (每个 NAPI 轮询最多处理的包数)

// 调度 NAPI (通常从中断处理函数中调用)
if (napi_schedule_prep(napi)) {
    __napi_schedule(napi);
}

// 驱动 poll 回调函数原型
int poll_func(struct napi_struct *napi, int budget);

// 退出 NAPI 轮询
napi_complete_done(napi, work_done);

// 启用/禁用 NAPI
napi_enable(napi);
napi_disable(napi);
```

### 6.2 GRO (Generic Receive Offload)

GRO 将多个相似的小包合并为一个大包，减少协议栈处理开销，定义在 [net/core/gro.c](file:///home/louis/code/linux/net/core/gro.c)。

#### 6.2.1 GRO 合并条件

```
GRO 合并条件 (所有条件必须满足):
    ├── 同一网络设备
    ├── 同一 IP 协议 (IPv4/IPv6)
    ├── 同一传输层协议 (TCP/UDP)
    ├── 同一 IP 源/目的地址
    ├── 同一 TCP/UDP 源/目的端口
    ├── 同一 VLAN 标签
    ├── TCP 序列号连续
    └── 时间窗口内 (NAPI 轮询周期内)
```

#### 6.2.2 GRO 性能对比

```
场景: 接收 1000 个 1460 字节的 TCP 数据包

没有 GRO:
    1000 次中断 (或 NAPI 轮询)
    → 1000 次 IP 层处理
    → 1000 次 TCP 层处理
    → 1000 次 skb 入队
    → 1000 次 wake_up 通知用户态
    总开销: 1000 × 协议栈处理开销

有 GRO (合并为 10 个大包, 每个 146000 字节):
    10 次 IP 层处理
    → 10 次 TCP 层处理
    → 10 次 skb 入队
    → 10 次 wake_up 通知用户态
    总开销: 10 × 协议栈处理开销 (节省约 99%)
```

### 6.3 GSO (Generic Segment Offload)

GSO 是发送方向的分段卸载技术，将 TCP 大段切分为 MTU 大小的小段，定义在 [net/core/gso.c](file:///home/louis/code/linux/net/core/gso.c)。

#### 6.3.1 GSO 分层

```
                    ┌──────────────────────────────────┐
                    │    TCP 层 (tcp_write_xmit)        │
                    │    构建大段 (最大 64KB)            │
                    │    skb_shinfo(skb)->gso_size = MSS│
                    └──────────────┬───────────────────┘
                                   │
                    ┌──────────────▼───────────────────┐
                    │    IP 层 (ip_queue_xmit)          │
                    │    不进行分片，传递大段给设备层     │
                    └──────────────┬───────────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              │                    │                    │
              ▼                    ▼                    ▼
┌─────────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│ 硬件 TSO 卸载        │  │ 软件 GSO 分片    │  │ 无 GSO           │
│ (e1000 不支持 TSO)   │  │ (skb_gso_segment)│  │ (每个段独立发送)  │
│                     │  │                 │  │                  │
│ 硬件将大段拆分为     │  │ 内核将大段拆分   │  │ TCP 层直接分段    │
│ MTU 大小的段         │  │ 为 MTU 大小段   │  │ (低效)           │
│ 并添加各自头部       │  │ 逐个发送         │  │                  │
└─────────────────────┘  └──────────────────┘  └──────────────────┘
```

#### 6.3.2 GSO 分段流程

```
dev_hard_start_xmit() 中:
    │
    └── if (skb_is_gso(skb))
        │
        ├── if (dev->features & NETIF_F_TSO)
        │   └── 硬件 TSO: 传递整个大段给驱动，硬件分段
        │
        └── else
            └── dev_gso_segment(skb)
                └── skb_gso_segment(skb, features)
                    ├── 遍历 skb_shinfo(skb)->frag_list
                    ├── 对每个段:
                    │   ├── 拷贝协议头部
                    │   ├── 调整 IP ID、TCP 序列号
                    │   └── 调整校验和
                    └── 逐个调用 xmit_one() 发送
```

### 6.4 中断节流 (Interrupt Throttling)

中断节流通过控制中断频率来平衡延迟和吞吐量。

#### 6.4.1 e1000 ITR 机制

```
e1000 的 ITR (Interrupt Throttling Rate) 寄存器:
    │
    ├── 值 = 中断间隔 (微秒)
    ├── 范围: 10us ~ 10000us
    ├── 对应中断频率: 100,000 ~ 100 次/秒
    │
    └── 动态调整算法 (e1000_set_itr()):
        │
        ├── 计算平均包间隔 (基于上次中断以来的包数)
        │
        ├── if (包间隔 < 10us) → 高负载
        │   └── ITR = 最大 (降低中断频率)
        │
        ├── if (包间隔 < 20us) → 中高负载
        │   └── ITR = 中等
        │
        └── else → 低负载
            └── ITR = 最小 (提高中断频率，降低延迟)
```

### 6.5 零拷贝技术

#### 6.5.1 splice (管道零拷贝)

```c
// splice 系统调用：在内核空间直接传递数据，无需拷贝到用户态
// 文件描述符 → 管道 → socket 描述符
// 或 socket 描述符 → 管道 → 文件描述符

// 发送方向：splice(fd_file, ..., fd_socket, ..., len, flags)
// 接收方向：splice(fd_socket, ..., fd_file, ..., len, flags)

// 内核实现：splice_to_socket() / tcp_splice_read()
// 使用 pipe_buffer 引用页面，避免数据拷贝
```

#### 6.5.2 TCP mmap

```c
// mmap 映射 socket 接收缓冲区到用户空间
// 用户态可以直接访问 skb 数据，无需 copy_to_user

// 内核实现: tcp_mmap() (net/ipv4/tcp.c)
// 通过 tcp_recvmsg_locked() 中的 VM_DONTEXPAND 映射
```

#### 6.5.3 sendfile (文件到 socket)

```c
// sendfile 系统调用：直接将文件数据发送到 socket
// 使用 splice 机制，在内核空间完成
// 避免了：文件 → 用户态 → 内核态的两次拷贝
```

### 6.6 RSS (Receive Side Scaling)

RSS 通过多队列将接收负载分散到多个 CPU 核心。

```
┌─────────────────────────────────────────────────────┐
│  网卡硬件 RSS 哈希引擎                               │
│                                                      │
│  输入: IP 源/目的地址 + TCP/UDP 源/目的端口          │
│  哈希算法: Toeplitz 哈希                             │
│  输出: 4 位哈希值 (0-15) → 映射到 RX 队列           │
│                                                      │
│  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐            │
│  │RX Q0 │  │RX Q1 │  │RX Q2 │  │RX Q3 │            │
│  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘            │
│     │         │         │         │                  │
└─────┼─────────┼─────────┼─────────┼──────────────────┘
      │         │         │         │
      ▼         ▼         ▼         ▼
   CPU 0     CPU 1     CPU 2     CPU 3
    ┌──┐      ┌──┐      ┌──┐      ┌──┐
    │NAPI│    │NAPI│    │NAPI│    │NAPI│
    │IRQ│     │IRQ│     │IRQ│     │IRQ│
    └──┘      └──┘      └──┘      └──┘
```

**RSS 的好处**：
- 多 CPU 核心分担接收负载
- 同一 TCP 连接的所有包在同一个 CPU 处理（避免缓存颠簸）
- 提高整体吞吐量

### 6.7 TCP 小包合并 (tcp_copybreak)

```
e1000_copybreak 优化:
    │
    ├── 默认阈值: 256 字节
    │
    ├── 若接收包大小 < 256 字节:
    │   ├── 分配新的小 skb
    │   ├── 从 DMA 缓冲区拷贝数据到新 skb
    │   └── 原 DMA 缓冲区回收复用 (避免重新分配)
    │   └── 优点: 减少 DMA 缓冲区分配/释放开销
    │
    └── 若接收包大小 >= 256 字节:
        └── 直接使用 DMA 缓冲区作为 skb 数据区
            └── 优点: 避免数据拷贝
```

### 6.8 发送路径优化 (TSO / 批量发送)

```
发送路径优化总结：

1. TSO (TCP Segmentation Offload)
   - TCP 层构建大段 (最大 64KB)
   - 硬件或内核 GSO 分段
   - 减少 TCP 层循环次数

2. 批量发送
   - tcp_write_xmit() 中 while 循环批量发送
   - 一次 lock_sock() 发送多个包
   - 减少锁竞争

3. 写合并
   - 多个 skb 的 TDT 写入合并
   - 减少 MMIO 写次数

4. SG (Scatter/Gather) DMA
   - skb 的 frag 分片直接 DMA
   - 避免数据拷贝到连续缓冲区
```

---

## 7. 附：关键代码路径速查表

### 7.1 发送路径函数调用链

| 步骤 | 函数 | 文件 |
|------|------|------|
| 1 | `__sys_sendto()` | [net/socket.c](file:///home/louis/code/linux/net/socket.c) |
| 2 | `sock_sendmsg()` | [net/socket.c](file:///home/louis/code/linux/net/socket.c) |
| 3 | `inet_sendmsg()` | [net/ipv4/af_inet.c](file:///home/louis/code/linux/net/ipv4/af_inet.c) |
| 4 | `tcp_sendmsg()` | [net/ipv4/tcp.c](file:///home/louis/code/linux/net/ipv4/tcp.c) |
| 5 | `tcp_sendmsg_locked()` | [net/ipv4/tcp.c](file:///home/louis/code/linux/net/ipv4/tcp.c) |
| 6 | `tcp_push()` | [net/ipv4/tcp.c](file:///home/louis/code/linux/net/ipv4/tcp.c) |
| 7 | `__tcp_push_pending_frames()` | [net/ipv4/tcp_output.c](file:///home/louis/code/linux/net/ipv4/tcp_output.c) |
| 8 | `tcp_write_xmit()` | [net/ipv4/tcp_output.c](file:///home/louis/code/linux/net/ipv4/tcp_output.c) |
| 9 | `__tcp_transmit_skb()` | [net/ipv4/tcp_output.c](file:///home/louis/code/linux/net/ipv4/tcp_output.c) |
| 10 | `ip_queue_xmit()` | [net/ipv4/ip_output.c](file:///home/louis/code/linux/net/ipv4/ip_output.c) |
| 11 | `__ip_local_out()` | [net/ipv4/ip_output.c](file:///home/louis/code/linux/net/ipv4/ip_output.c) |
| 12 | `ip_output()` | [net/ipv4/ip_output.c](file:///home/louis/code/linux/net/ipv4/ip_output.c) |
| 13 | `ip_finish_output()` | [net/ipv4/ip_output.c](file:///home/louis/code/linux/net/ipv4/ip_output.c) |
| 14 | `ip_finish_output2()` | [net/ipv4/ip_output.c](file:///home/louis/code/linux/net/ipv4/ip_output.c) |
| 15 | `neigh_output()` | [net/core/neighbour.c](file:///home/louis/code/linux/net/core/neighbour.c) |
| 16 | `dev_queue_xmit()` | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) |
| 17 | `__dev_queue_xmit()` | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) |
| 18 | `__dev_xmit_skb()` | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) |
| 19 | `sch_direct_xmit()` | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) |
| 20 | `dev_hard_start_xmit()` | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) |
| 21 | `e1000_xmit_frame()` | [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) |
| 22 | `e1000_tx_map()` | [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) |
| 23 | `writel(TDT)` | [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) |

### 7.2 接收路径函数调用链

| 步骤 | 函数 | 文件 |
|------|------|------|
| 1 | `e1000_intr()` | [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) |
| 2 | `__napi_schedule()` | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) |
| 3 | `net_rx_action()` | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) |
| 4 | `e1000_clean()` | [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) |
| 5 | `e1000_clean_rx_irq()` | [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) |
| 6 | `e1000_receive_skb()` | [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) |
| 7 | `napi_gro_receive()` | [net/core/gro.c](file:///home/louis/code/linux/net/core/gro.c) |
| 8 | `dev_gro_receive()` | [net/core/gro.c](file:///home/louis/code/linux/net/core/gro.c) |
| 9 | `netif_receive_skb()` | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) |
| 10 | `__netif_receive_skb_core()` | [net/core/dev.c](file:///home/louis/code/linux/net/core/dev.c) |
| 11 | `ip_rcv()` | [net/ipv4/ip_input.c](file:///home/louis/code/linux/net/ipv4/ip_input.c) |
| 12 | `ip_rcv_finish()` | [net/ipv4/ip_input.c](file:///home/louis/code/linux/net/ipv4/ip_input.c) |
| 13 | `ip_local_deliver()` | [net/ipv4/ip_input.c](file:///home/louis/code/linux/net/ipv4/ip_input.c) |
| 14 | `ip_local_deliver_finish()` | [net/ipv4/ip_input.c](file:///home/louis/code/linux/net/ipv4/ip_input.c) |
| 15 | `tcp_v4_rcv()` | [net/ipv4/tcp_ipv4.c](file:///home/louis/code/linux/net/ipv4/tcp_ipv4.c) |
| 16 | `tcp_v4_do_rcv()` | [net/ipv4/tcp_ipv4.c](file:///home/louis/code/linux/net/ipv4/tcp_ipv4.c) |
| 17 | `tcp_rcv_established()` | [net/ipv4/tcp_input.c](file:///home/louis/code/linux/net/ipv4/tcp_input.c) |
| 18 | `tcp_data_queue()` | [net/ipv4/tcp_input.c](file:///home/louis/code/linux/net/ipv4/tcp_input.c) |
| 19 | `tcp_queue_rcv()` | [net/ipv4/tcp_input.c](file:///home/louis/code/linux/net/ipv4/tcp_input.c) |
| 20 | `sock_def_readable()` | [net/core/sock.c](file:///home/louis/code/linux/net/core/sock.c) |
| 21 | `__sys_recvfrom()` | [net/socket.c](file:///home/louis/code/linux/net/socket.c) |
| 22 | `tcp_recvmsg()` | [net/ipv4/tcp.c](file:///home/louis/code/linux/net/ipv4/tcp.c) |
| 23 | `skb_copy_datagram_msg()` | [net/core/datagram.c](file:///home/louis/code/linux/net/core/datagram.c) |
| 24 | `copy_to_user()` | (体系结构相关) |

### 7.3 关键数据结构定义

| 数据结构 | 头文件 | 行号 | 说明 |
|---------|--------|------|------|
| `struct socket` | [include/linux/net.h](file:///home/louis/code/linux/include/linux/net.h) | 110 | VFS socket 层表示 |
| `struct sock` | [include/net/sock.h](file:///home/louis/code/linux/include/net/sock.h) | 360 | 协议无关 socket 内部表示 |
| `struct inet_sock` | [include/net/inet_sock.h](file:///home/louis/code/linux/include/net/inet_sock.h) | 218 | INET 协议族扩展 |
| `struct tcp_sock` | [include/linux/tcp.h](file:///home/louis/code/linux/include/linux/tcp.h) | 138 | TCP 协议专用结构 |
| `struct sk_buff` | [include/linux/skbuff.h](file:///home/louis/code/linux/include/linux/skbuff.h) | 885 | 网络数据包表示 |
| `struct net_device` | [include/linux/netdevice.h](file:///home/louis/code/linux/include/linux/netdevice.h) | 2109 | 网络设备抽象 |
| `struct e1000_adapter` | [drivers/net/ethernet/intel/e1000/e1000.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000.h) | - | e1000 驱动私有数据 |
| `struct e1000_hw` | [drivers/net/ethernet/intel/e1000/e1000_hw.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.h) | - | e1000 硬件抽象层 |
| `struct e1000_tx_desc` | [drivers/net/ethernet/intel/e1000/e1000_hw.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.h) | - | TX 描述符格式 |
| `struct e1000_rx_desc` | [drivers/net/ethernet/intel/e1000/e1000_hw.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.h) | - | RX 描述符格式 |

### 7.4 关键操作向量

| 操作向量 | 类型 | 定义位置 | 说明 |
|---------|------|---------|------|
| `socket_file_ops` | `struct file_operations` | [net/socket.c](file:///home/louis/code/linux/net/socket.c) | VFS 文件操作 → socket 映射 |
| `inet_stream_ops` | `struct proto_ops` | [net/ipv4/af_inet.c](file:///home/louis/code/linux/net/ipv4/af_inet.c) | TCP 流式 socket 操作 |
| `inet_dgram_ops` | `struct proto_ops` | [net/ipv4/af_inet.c](file:///home/louis/code/linux/net/ipv4/af_inet.c) | UDP 数据报 socket 操作 |
| `tcp_prot` | `struct proto` | [net/ipv4/tcp_ipv4.c](file:///home/louis/code/linux/net/ipv4/tcp_ipv4.c) | TCP 协议操作 |
| `udp_prot` | `struct proto` | [net/ipv4/udp.c](file:///home/louis/code/linux/net/ipv4/udp.c) | UDP 协议操作 |
| `e1000_netdev_ops` | `struct net_device_ops` | [e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) | e1000 设备操作 |
| `e1000_driver` | `struct pci_driver` | [e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) | e1000 PCI 驱动 |

### 7.5 关键寄存器速查

| 寄存器 | 偏移 | 访问 | 说明 |
|--------|------|------|------|
| `E1000_CTRL` | 0x0000 | R/W | 设备控制 (复位、全双工、链路建立) |
| `E1000_STATUS` | 0x0008 | R | 设备状态 (链路速度、双工模式) |
| `E1000_ICR` | 0x00C0 | R (读清除) | 中断原因寄存器 |
| `E1000_IMS` | 0x00D0 | R/W | 中断掩码设置 |
| `E1000_IMC` | 0x00D8 | W | 中断掩码清除 |
| `E1000_ITR` | 0x00C4 | R/W | 中断节流率 |
| `E1000_RCTL` | 0x0100 | R/W | 接收控制 |
| `E1000_TCTL` | 0x0400 | R/W | 发送控制 |
| `E1000_TDBAL` | 0x0408 | R/W | TX 描述符基址低 32 位 |
| `E1000_TDBAH` | 0x040C | R/W | TX 描述符基址高 32 位 |
| `E1000_TDLEN` | 0x0410 | R/W | TX 描述符环长度 |
| `E1000_TDH` | 0x0418 | R/W | TX 描述符头指针 (硬件消费端) |
| `E1000_TDT` | 0x041C | R/W | TX 描述符尾指针 (驱动生产端) |
| `E1000_RDBAL` | 0x2800 | R/W | RX 描述符基址低 32 位 |
| `E1000_RDBAH` | 0x2804 | R/W | RX 描述符基址高 32 位 |
| `E1000_RDLEN` | 0x2808 | R/W | RX 描述符环长度 |
| `E1000_RDH` | 0x2810 | R/W | RX 描述符头指针 (硬件消费端) |
| `E1000_RDT` | 0x2818 | R/W | RX 描述符尾指针 (驱动生产端) |

### 7.6 参考文档

| 文档 | 路径 |
|------|------|
| 网络子系统总览 | [network_subsystem_analysis.md](file:///home/louis/code/linux/notes/network/network_subsystem_analysis.md) |
| VFS 与 Socket 层分析 | [socket_layer_analysis.md](file:///home/louis/code/linux/notes/network/socket_layer_analysis.md) |
| TCP/IP 协议栈分析 | [tcp_ip_protocol_stack_analysis.md](file:///home/louis/code/linux/notes/network/tcp_ip_protocol_stack_analysis.md) |
| Intel e1000 网卡驱动分析 | [intel_nic_driver_analysis.md](file:///home/louis/code/linux/notes/network/intel_nic_driver_analysis.md) |