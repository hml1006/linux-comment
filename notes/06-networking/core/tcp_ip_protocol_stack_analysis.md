# Linux 内核 TCP/IP 协议栈分析

## 目录

1. [概述](#1-概述)
2. [TCP/IP 协议栈架构](#2-tcpip-协议栈架构)
3. [TCP 核心数据结构](#3-tcp-核心数据结构)
   - 3.1 [struct tcp_sock](#31-struct-tcp_sock)
   - 3.2 [struct tcp_options_received](#32-struct-tcp_options_received)
   - 3.3 [struct tcp_congestion_ops](#33-struct-tcp_congestion_ops)
   - 3.4 [struct tcp_skb_cb](#34-struct-tcp_skb_cb)
   - 3.5 [struct tcp_request_sock](#35-struct-tcp_request_sock)
4. [TCP 连接管理](#4-tcp-连接管理)
   - 4.1 [三次握手流程](#41-三次握手流程)
   - 4.2 [四次挥手流程](#42-四次挥手流程)
   - 4.3 [连接状态转换](#43-连接状态转换)
5. [TCP 数据发送路径](#5-tcp-数据发送路径)
   - 5.1 [tcp_sendmsg 发送入口](#51-tcp_sendmsg-发送入口)
   - 5.2 [__tcp_transmit_skb 发送核心](#52-__tcp_transmit_skb-发送核心)
   - 5.3 [发送路径完整调用链](#53-发送路径完整调用链)
6. [TCP 数据接收路径](#6-tcp-数据接收路径)
   - 6.1 [tcp_v4_rcv 接收入口](#61-tcp_v4_rcv-接收入口)
   - 6.2 [tcp_v4_do_rcv 接收分派](#62-tcp_v4_do_rcv-接收分派)
   - 6.3 [tcp_rcv_established 快速路径](#63-tcp_rcv_established-快速路径)
   - 6.4 [tcp_data_queue 数据入队](#64-tcp_data_queue-数据入队)
   - 6.5 [接收路径完整调用链](#65-接收路径完整调用链)
7. [TCP 拥塞控制](#7-tcp-拥塞控制)
   - 7.1 [拥塞控制框架](#71-拥塞控制框架)
   - 7.2 [RTT 估计与拥塞窗口调整](#72-rtt-估计与拥塞窗口调整)
   - 7.3 [慢启动与拥塞避免](#73-慢启动与拥塞避免)
   - 7.4 [快速重传与快速恢复](#74-快速重传与快速恢复)
8. [TCP 定时器](#8-tcp-定时器)
   - 8.1 [重传定时器](#81-重传定时器)
   - 8.2 [延迟 ACK 定时器](#82-延迟-ack-定时器)
   - 8.3 [保活定时器](#83-保活定时器)
   - 8.4 [TLP 定时器](#84-tlp-定时器)
9. [IPv4 协议层](#9-ipv4-协议层)
   - 9.1 [IP 层接收路径](#91-ip-层接收路径)
   - 9.2 [IP 层发送路径](#92-ip-层发送路径)
10. [UDP 协议实现](#10-udp-协议实现)
    - 10.1 [UDP 核心数据结构](#101-udp-核心数据结构)
    - 10.2 [UDP 发送与接收](#102-udp-发送与接收)
11. [关键函数接口](#11-关键函数接口)
    - 11.1 [TCP 协议操作函数](#111-tcp-协议操作函数)
    - 11.2 [INET 连接操作函数](#112-inet-连接操作函数)
12. [附录：关键文件列表](#12-附录关键文件列表)

---

## 1. 概述

TCP/IP 协议栈是 Linux 网络子系统的核心，实现了传输层（TCP/UDP）和网络层（IPv4/IPv6）协议。本分析基于内核源码 `net/ipv4/` 目录，涵盖 TCP 协议的状态机、数据发送/接收路径、拥塞控制、定时器管理以及 IPv4 协议层。

TCP/IP 协议栈在整体网络架构中的位置：

```
用户空间 (socket API)
    ↓
VFS Socket 层 (net/socket.c)
    ↓
Socket 核心层 (net/core/sock.c)
    ↓
协议族层 (net/ipv4/af_inet.c)  ← TCP/IP 协议栈入口
    ↓
传输层 (net/ipv4/tcp.c / udp.c)  ← TCP/UDP 协议实现
    ↓
网络层 (net/ipv4/ip_input.c / ip_output.c)  ← IP 协议实现
    ↓
邻居子系统 (net/core/neighbour.c)
    ↓
网络设备层 (net/core/dev.c)
    ↓
网卡驱动
```

---

## 2. TCP/IP 协议栈架构

TCP/IP 协议栈在内核中采用分层实现，各层通过函数调用和回调机制交互：

```
┌─────────────────────────────────────────────────────┐
│                 用户空间应用程序                       │
│    socket(AF_INET, SOCK_STREAM, 0)                  │
│    connect() / send() / recv() / close()            │
└──────────────┬──────────────────────────────────────┘
               │ 系统调用
┌──────────────▼──────────────────────────────────────┐
│          VFS Socket 层 (net/socket.c)                │
│    __sys_socket → sock_alloc → __sock_create        │
│    __sys_connect → inet_stream_connect              │
└──────────────┬──────────────────────────────────────┘
               │ proto_ops 操作向量
┌──────────────▼──────────────────────────────────────┐
│         INET 协议族层 (net/ipv4/af_inet.c)           │
│    inet_create → 绑定 inet_stream_ops + tcp_prot    │
│    inet_sendmsg → tcp_sendmsg                       │
│    inet_recvmsg → tcp_recvmsg                       │
└──────────────┬──────────────────────────────────────┘
               │ prot 操作向量
┌──────────────▼──────────────────────────────────────┐
│          TCP 传输层 (net/ipv4/tcp_*.c)               │
│    ┌───────────────┐  ┌───────────────┐             │
│    │  tcp_input.c  │  │ tcp_output.c │             │
│    │  tcp_rcv_*    │  │ tcp_sendmsg   │             │
│    │  tcp_data_*   │  │ tcp_transmit  │             │
│    └───────┬───────┘  └───────┬───────┘             │
│            │                  │                      │
│    ┌───────▼───────┐  ┌───────▼───────┐             │
│    │  tcp_timer.c  │  │ tcp_cong.c   │             │
│    │  定时器管理    │  │ 拥塞控制      │             │
│    └───────────────┘  └───────────────┘             │
└──────────────┬──────────────────────────────────────┘
               │ icsk_af_ops->queue_xmit / ip_local_deliver
┌──────────────▼──────────────────────────────────────┐
│          IP 网络层 (net/ipv4/ip_*.c)                 │
│    ip_queue_xmit → ip_local_out → ip_output         │
│    ip_rcv → ip_rcv_finish → ip_local_deliver        │
└──────────────┬──────────────────────────────────────┘
               │ netif_receive_skb / dev_queue_xmit
┌──────────────▼──────────────────────────────────────┐
│          网络设备层 (net/core/dev.c)                  │
│    __netif_receive_skb_core → 协议分派              │
│    dev_queue_xmit → __dev_xmit_skb                 │
└─────────────────────────────────────────────────────┘
```

---

## 3. TCP 核心数据结构

### 3.1 struct tcp_sock

定义在 [include/linux/tcp.h](file:///home/louis/code/linux/include/linux/tcp.h) 中，是 TCP 协议的核心结构体，继承自 `inet_connection_sock`。包含连接状态、序列号、窗口、拥塞控制、SACK 等所有 TCP 协议状态。

```c
struct tcp_sock {
    /* inet_connection_sock 必须是 tcp_sock 的第一个成员 */
    struct inet_connection_sock    inet_conn;

    /* TX 读多写少热路径缓存行 */
    __cacheline_group_begin(tcp_sock_read_tx);
    u32    max_window;      /* 从对端看到的最大窗口 */
    u32    rcv_ssthresh;    /* 当前窗口阈值 */
    u32    reordering;      /* 数据包乱序度量 */
    u32    notsent_lowat;   /* TCP_NOTSENT_LOWAT */
    u16    gso_segs;        /* 每个 GSO 包的最大段数 */
    struct sk_buff *retransmit_skb_hint; /* 重传队列提示 */
    __cacheline_group_end(tcp_sock_read_tx);

    /* TXRX 读多写少热路径缓存行 */
    __cacheline_group_begin(tcp_sock_read_txrx);
    u32    tsoffset;        /* 时间戳偏移 */
    u32    snd_wnd;         /* 期望接收的窗口 */
    u32    mss_cache;       /* 缓存的 MSS，不含 SACK */
    u32    snd_cwnd;        /* 发送拥塞窗口 */
    u32    lost_out;        /* 丢失的数据包数 */
    u32    sacked_out;      /* 被 SACK 确认的数据包数 */
    u16    tcp_header_len;  /* TCP 头部字节数 */
    u8     scaling_ratio;   /* 窗口缩放比例 */
    __cacheline_group_end(tcp_sock_read_txrx);

    /* RX 读多写少热路径缓存行 */
    __cacheline_group_begin(tcp_sock_read_rx);
    u32    copied_seq;      /* 已读数据的头部 */
    u32    snd_wl1;         /* 窗口更新序列号 */
    u32    rttvar_us;       /* RTT 方差 */
    u32    retrans_out;     /* 重传数据包数 */
    u16    advmss;          /* 通告的 MSS */
    struct rb_root out_of_order_queue; /* 乱序数据包红黑树 */
    __cacheline_group_end(tcp_sock_read_rx);

    /* TX 读写热路径缓存行 */
    __cacheline_group_begin(tcp_sock_write_tx);
    u32    write_seq;       /* 发送缓冲区中数据的尾部(+1) */
    u32    pushed_seq;      /* 最后一次推送的序列号 */
    u32    lsndtime;        /* 最后发送数据包的时间戳 */
    struct list_head tsorted_sent_queue; /* 按时间排序的已发送未 SACK 队列 */
    struct sk_buff *highest_sack;        /* 最高 SACK 的 skb */
    __cacheline_group_end(tcp_sock_write_tx);

    /* TXRX 读写热路径缓存行 */
    __cacheline_group_begin(tcp_sock_write_txrx);
    u32    rcv_nxt;         /* 期望接收的下一个序列号 */
    u32    snd_nxt;         /* 发送的下一个序列号 */
    u32    snd_una;         /* 等待 ACK 的第一个字节 */
    u32    window_clamp;    /* 通告的最大窗口 */
    u32    srtt_us;         /* 平滑 RTT (微秒 << 3) */
    u32    packets_out;     /* 已发送但未确认的数据包数 */
    u32    delivered;       /* 已递送的数据包总数 */
    u32    app_limited;     /* 应用层受限，直到 delivered 达到此值 */
    u32    rcv_wnd;         /* 当前接收窗口 */
    struct tcp_options_received rx_opt;  /* 接收到的选项 */
    __cacheline_group_end(tcp_sock_write_txrx);

    /* RX 读写热路径缓存行 */
    __cacheline_group_begin(tcp_sock_write_rx);
    u32    rcv_wup;         /* 上次窗口更新时的 rcv_nxt */
    u32    max_packets_out; /* 上次窗口中的最大 packets_out */
    struct {
        u32    rtt_us;
        u32    seq;
        u64    time;
    } rcv_rtt_est;          /* 接收端 RTT 估计 */
    __cacheline_group_end(tcp_sock_write_rx);

    /* --- 以下为慢路径字段 --- */

    /* RFC793 标准变量 */
    u32    dsack_dups;      /* DSACK 块计数 */
    struct list_head tsq_node;  /* tsq_tasklet 链表锚点 */

    /* RACK (Recent ACKnowledgment) 检测 */
    struct tcp_rack {
        u64 mstamp;         /* 发送/重传时间戳 */
        u32 rtt_us;         /* 关联 RTT */
        u32 end_seq;        /* 结束序列号 */
        u8  reo_wnd_steps;  /* 允许的乱序窗口步数 */
    } rack;

    /* 慢启动和拥塞控制 */
    u32    snd_cwnd_cnt;    /* 线性递增计数器 */
    u32    snd_cwnd_clamp;  /* snd_cwnd 上限 */
    u32    prior_cwnd;      /* 进入恢复前的 cwnd */

    /* SACK 数据 */
    struct tcp_sack_block duplicate_sack[1];  /* D-SACK 块 */
    struct tcp_sack_block selective_acks[4];  /* SACK 块 */
    struct tcp_sack_block recv_sack_cache[4]; /* SACK 缓存 */

    u32    high_seq;        /* 拥塞发送时的 snd_nxt */
    u32    retrans_stamp;   /* 最后重传的时间戳 */

    /* 定时器 */
    struct hrtimer pacing_timer;           /* 调速定时器 */
    struct hrtimer compressed_ack_timer;   /* 压缩 ACK 定时器 */

    /* TCP 快速打开 */
    struct tcp_fastopen_request *fastopen_req;
    struct request_sock __rcu *fastopen_rsk;
};
```

**关键字段说明：**

| 字段 | 说明 |
|------|------|
| `snd_una` | 最早未确认的序列号，发送窗口的左边界 |
| `snd_nxt` | 下一个要发送的序列号 |
| `rcv_nxt` | 期望接收的下一个序列号，接收窗口左边界 |
| `snd_wnd` | 对端通告的接收窗口大小 |
| `rcv_wnd` | 本端接收窗口大小 |
| `snd_cwnd` | 拥塞窗口，控制发送速率 |
| `srtt_us` | 平滑 RTT，用于 RTO 计算 |
| `packets_out` | 网络中正在传输的包数 |
| `out_of_order_queue` | 乱序数据包的红黑树根 |
| `write_seq` | 发送缓冲区中数据的尾部 |

### 3.2 struct tcp_options_received

定义在 [include/linux/tcp.h](file:///home/louis/code/linux/include/linux/tcp.h) 中，记录接收到的 TCP 选项信息。

```c
struct tcp_options_received {
    int    ts_recent_stamp;  /* 存储 ts_recent 的时间（用于老化） */
    u32    ts_recent;        /* 要回显的时间戳 */
    u32    rcv_tsval;        /* 收到的时间戳值 */
    u32    rcv_tsecr;        /* 收到的时间戳回显 */
    u16    saw_tstamp : 1,   /* 上一个包中看到 TIMESTAMP */
           tstamp_ok : 1,    /* SYN 包中看到 TIMESTAMP */
           wscale_ok : 1,    /* SYN 包中看到 Wscale */
           sack_ok : 3,      /* SYN 包中看到 SACK */
           snd_wscale : 4,   /* 发送方窗口缩放因子 */
           rcv_wscale : 4;   /* 接收方窗口缩放因子 */
    u8     num_sacks;        /* SACK 块数量 */
    u16    user_mss;         /* 用户通过 ioctl 请求的 MSS */
    u16    mss_clamp;        /* 协商的最大 MSS */
};
```

### 3.3 struct tcp_congestion_ops

定义在 [include/net/tcp.h](file:///home/louis/code/linux/include/net/tcp.h) 中，TCP 拥塞控制算法的操作接口。

```c
struct tcp_congestion_ops {
    /* 快速路径字段放在最前面以填满一个缓存行 */

    /* (a) "经典"响应：计算新的 cwnd */
    void (*cong_avoid)(struct sock *sk, u32 ack, u32 acked);

    /* (b) "自定义"响应：数据包递送时更新 cwnd 和速率 */
    void (*cong_control)(struct sock *sk, u32 ack, int flag,
                         const struct rate_sample *rs);

    /* 返回慢启动阈值（必需） */
    u32 (*ssthresh)(struct sock *sk);

    /* 状态转换前调用（可选） */
    void (*set_state)(struct sock *sk, u8 new_state);

    /* cwnd 事件发生时调用（可选） */
    void (*cwnd_event)(struct sock *sk, enum tcp_ca_event ev);

    /* ACK 到达时调用（可选） */
    void (*in_ack_event)(struct sock *sk, u32 flags);

    /* 数据包 ACK 记账（可选） */
    void (*pkts_acked)(struct sock *sk, const struct ack_sample *sample);

    /* 丢失后新 cwnd 值（必需） */
    u32 (*undo_cwnd)(struct sock *sk);

    /* 慢路径 */
    size_t (*get_info)(struct sock *sk, u32 ext, int *attr,
                       union tcp_cc_info *info);
    char name[TCP_CA_NAME_MAX];  /* 算法名称 */
    struct module *owner;
    struct list_head list;       /* 链表 */
    u32 key;                     /* 用于 bpf 查找 */
};
```

### 3.4 struct tcp_skb_cb

TCP 协议的控制块，存储在 `sk_buff->cb[]` 数组中，通过 `TCP_SKB_CB(skb)` 宏访问。

```c
#define TCP_SKB_CB(__skb)  ((struct tcp_skb_cb *)&((__skb)->cb[0]))
```

`struct tcp_skb_cb` 包含 TCP 协议处理所需的字段：
- `seq` / `end_seq`：数据包的序列号范围
- `tcp_flags`：TCP 标志位（SYN、FIN、ACK、PSH 等）
- `sacked`：SACK 状态位（如 `TCPCB_SACKED_ACKED`、`TCPCB_SACKED_RETRANS`、`TCPCB_LOST`）
- `txstamp_ack` / `sacked`：与发送时间戳和 SACK 相关的状态
- `tcp_gso_size`：GSO 段大小

### 3.5 struct tcp_request_sock

定义在 [include/linux/tcp.h](file:///home/louis/code/linux/include/linux/tcp.h) 中，用于三次握手过程中的请求 socket 结构。

```c
struct tcp_request_sock {
    struct inet_request_sock     req;        /* 继承 INET 请求 sock */
    const struct tcp_request_sock_ops *af_specific; /* AF 特定操作 */
    u64                          snt_synack; /* 首次 SYNACK 发送时间 */
    bool                         tfo_listener;
    bool                         is_mptcp;
    u32                          txhash;
    u32                          rcv_isn;    /* 接收的初始序列号 */
    u32                          snt_isn;    /* 发送的初始序列号 */
    u32                          ts_off;     /* 时间戳偏移 */
    u32                          rcv_nxt;    /* 收到的 ACK 序列号 */
    u8                           syn_tos;
    u32                          last_oow_ack_time; /* 最后 SYNACK 时间 */
};
```

---

## 4. TCP 连接管理

### 4.1 三次握手流程

```
CLOSED                              LISTEN
   |                                  |
   |  connect()                       |  accept() 阻塞等待
   |  tcp_connect()                   |
   ▼                                  ▼
SYN_SENT ── send SYN ──────────────► SYN_RCVD
   │                                  │
   │  ◄─── recv SYN+ACK ─────────────│
   │                                  │
   ▼                                  ▼
   │  tcp_rcv_synsent_state_process() │
   │  send ACK ───────────────────────┤
   │                                  │
   ▼                                  ▼
ESTABLISHED                      ESTABLISHED
```

**客户端调用链：**

```
tcp_connect()  [tcp_output.c:4296]
  │
  ├── tcp_connect_init() → 初始化序列号、窗口、MSS
  ├── tcp_send_synack() → 分配 skb，填充 TCP 选项
  ├── __tcp_transmit_skb() → 构建并发送 SYN 包
  └── inet_csk_reset_xmit_timer() → 启动 SYN 超时定时器

收到 SYN+ACK 后：
tcp_rcv_synsent_state_process()  [tcp_input.c]
  │
  ├── tcp_ack() → 处理 ACK
  ├── tcp_finish_connect() → 设置 snd_una = snd_nxt
  └── tcp_init_transfer() → 初始化拥塞控制状态

tcp_send_ack() → 发送最后的 ACK
```

**服务端调用链：**

```
tcp_v4_do_rcv(sk, skb)  [tcp_ipv4.c:1859]
  │
  ├── sk->sk_state == TCP_LISTEN
  │
  └── tcp_v4_cookie_check() → tcp_check_req()
      │
      ├── tcp_conn_request()  [tcp_input.c]
      │   ├── tcp_parse_options() → 解析 SYN 选项
      │   ├── reqsk_alloc() → 分配 request_sock
      │   ├── tcp_openreq_init_rwin() → 初始化窗口
      │   └── tcp_v4_send_synack() → 发送 SYN+ACK
      │
      └── tcp_child_process() → 处理最后的 ACK
          └── tcp_rcv_state_process() → 建立连接
```

### 4.2 四次挥手流程

```
ESTABLISHED                    ESTABLISHED
   |                               |
   |  close()                       |
   |  tcp_close() → tcp_send_fin()  |
   |  send FIN ────────────────────► |
   ▼                               ▼
FIN_WAIT1                     CLOSE_WAIT
   |                               |
   |  ◄─── recv ACK ───────────────|
   |                               |
   ▼                               ▼
FIN_WAIT2                     (继续发送数据)
   |                               |
   |  ◄─── recv FIN ───────────────┤  close()
   |                               |  tcp_send_fin()
   ▼                               ▼
TIME_WAIT ── send ACK ──────────► LAST_ACK
   │                               │
   │  (2MSL 超时)                  │  ◄─── recv ACK
   ▼                               ▼
CLOSED                         CLOSED
```

### 4.3 连接状态转换

TCP 连接状态定义在 [include/net/tcp_states.h](file:///home/louis/code/linux/include/net/tcp_states.h) 中，共 11 种状态：

```c
typedef __u8 __bitwise tcp_state_t;
enum {
    TCP_ESTABLISHED = 1,    // 连接已建立
    TCP_SYN_SENT,           // 主动连接：已发送 SYN，等待 SYN+ACK
    TCP_SYN_RECV,           // 被动连接：已收到 SYN，发送 SYN+ACK，等待 ACK
    TCP_FIN_WAIT1,          // 主动关闭：已发送 FIN，等待 ACK
    TCP_FIN_WAIT2,          // 主动关闭：已收到 FIN 的 ACK，等待 FIN
    TCP_TIME_WAIT,          // 主动关闭：收到 FIN，等待 2MSL
    TCP_CLOSE,              // 无连接状态
    TCP_CLOSE_WAIT,         // 被动关闭：收到 FIN，等待应用层 close
    TCP_LAST_ACK,           // 被动关闭：应用层 close 后发送 FIN，等待 ACK
    TCP_LISTEN,             // 等待连接请求
    TCP_CLOSING,            // 同时关闭：FIN 已发且已收，等待 ACK
};
```

#### 4.3.1 完整状态变迁图

```
                                   ┌──────────────────────────────────────────────────┐
                                   │                                                  │
                                   ▼                                                  │
 ┌───────────┐   passive open   ┌───────────┐   ┌────── recv SYN ──────┐             │
 │   CLOSED   │ ───────────────► │  LISTEN   │   │    send SYN+ACK      │             │
 └─────┬─────┘                  └───────────┘   │                      ▼             │
       │                                         │                  ┌──────────┐      │
       │  active open                            │                  │ SYN_RCVD │      │
       │  send SYN                               │                  └────┬─────┘      │
       ▼                                        │                       │            │
 ┌───────────┐   ◄──── recv SYN+ACK ────────────┘                       │            │
 │ SYN_SENT  │ ──── send ACK ────────────────┐                         │            │
 └─────┬─────┘                               │                         │            │
       │                                     │                         │            │
       │  recv SYN (同时打开)                 │                         │            │
       │  send SYN+ACK                       │                         │            │
       ▼                                     ▼                         ▼            │
 ┌───────────┐                           ┌──────────────┐                          │
 │ SYN_RCVD  │                           │ ESTABLISHED  │  ◄── recv ACK ────────────┘
 └─────┬─────┘                           └──────┬───────┘
       │                                        │
       │  recv RST                              │  close()
       │                                        │  send FIN
       ▼                                        ▼
 ┌───────────┐                           ┌──────────────┐
 │  LISTEN   │                           │  FIN_WAIT1   │
       └─────────────── recv RST ────────┘              │
                                         └──────────────┘
                                               │      │
                                    recv ACK   │      │  recv FIN
                                    (被动FIN已收)│      │  send ACK
                                               │      ▼
                                               │  ┌──────────────┐
                                               │  │  CLOSING     │
                                               │  └──────┬───────┘
                                               │         │
                                               ▼         │  recv ACK
                                         ┌──────────────┐ │
                                         │  FIN_WAIT2   │ │
                                         └──────┬───────┘ │
                                                │         │
                                    recv FIN    │         │
                                    send ACK    │         │
                                                ▼         ▼
                                          ┌──────────────────┐
                                          │   TIME_WAIT       │
                                          └────────┬─────────┘
                                                   │
                                         2MSL 超时 │
                                                   ▼
                                            ┌───────────┐
                                            │   CLOSED   │
                                            └───────────┘


 ESTABLISHED ──── recv FIN ────► ┌──────────────┐
                                 │ CLOSE_WAIT    │
                                 └──────┬───────┘
                                        │
                                        │  close()
                                        │  send FIN
                                        ▼
                                 ┌──────────────┐
                                 │  LAST_ACK     │
                                 └──────┬───────┘
                                        │
                                        │  recv ACK
                                        ▼
                                 ┌──────────────┐
                                 │   CLOSED      │
                                 └──────────────┘
```

**图例说明：**
- `CLOSED → LISTEN`：被动打开（服务器），send 无
- `CLOSED → SYN_SENT`：主动打开（客户端），send SYN
- `LISTEN → SYN_RCVD`：收到 SYN，send SYN+ACK
- `SYN_SENT → ESTABLISHED`：收到 SYN+ACK，send ACK
- `SYN_SENT → SYN_RCVD`：同时打开，收到 SYN，send SYN+ACK
- `SYN_RCVD → ESTABLISHED`：收到 ACK（三次握手完成）
- `SYN_RCVD → LISTEN`：收到 RST（连接被拒绝）
- `ESTABLISHED → FIN_WAIT1`：主动关闭，send FIN
- `ESTABLISHED → CLOSE_WAIT`：被动关闭，收到 FIN
- `FIN_WAIT1 → FIN_WAIT2`：收到 FIN 的 ACK
- `FIN_WAIT1 → CLOSING`：收到 FIN（同时关闭），send ACK
- `FIN_WAIT1 → TIME_WAIT`：收到 FIN+ACK，send ACK
- `FIN_WAIT2 → TIME_WAIT`：收到 FIN，send ACK
- `CLOSE_WAIT → LAST_ACK`：应用层 close，send FIN
- `CLOSING → TIME_WAIT`：收到 ACK
- `LAST_ACK → CLOSED`：收到 ACK
- `TIME_WAIT → CLOSED`：2MSL 超时

#### 4.3.2 状态变迁与数据包收发关系

每种状态变迁都对应特定数据包的发送或接收，下表总结了所有变迁与数据包的关系：

| 变迁 | 触发动作 | 发送包 | 接收包 | 方向 | 内核处理函数 |
|------|---------|--------|--------|------|-------------|
| CLOSED → LISTEN | 应用调用 `listen()` | — | — | 服务端 | `inet_listen()` → `tcp_set_state(sk, TCP_LISTEN)` |
| CLOSED → SYN_SENT | 应用调用 `connect()` | SYN | — | 客户端 | `tcp_connect()` → `tcp_transmit_skb()` |
| LISTEN → SYN_RCVD | 收到 SYN 包 | SYN+ACK | SYN | 服务端 | `tcp_conn_request()` → `tcp_v4_send_synack()` |
| SYN_SENT → ESTABLISHED | 收到 SYN+ACK | ACK | SYN+ACK | 客户端 | `tcp_rcv_synsent_state_process()` → `tcp_send_ack()` |
| SYN_SENT → SYN_RCVD | 收到 SYN（同时打开） | SYN+ACK | SYN | 双方 | `tcp_rcv_synsent_state_process()` → `tcp_send_synack()` |
| SYN_RCVD → ESTABLISHED | 收到 ACK | — | ACK | 服务端 | `tcp_rcv_state_process()` → `tcp_child_process()` |
| SYN_RCVD → LISTEN | 收到 RST | — | RST | 服务端 | `tcp_rcv_state_process()` → 重置到 LISTEN |
| ESTABLISHED → FIN_WAIT1 | 应用调用 `close()` | FIN | — | 主动方 | `tcp_close()` → `tcp_send_fin()` |
| ESTABLISHED → CLOSE_WAIT | 收到 FIN | ACK | FIN | 被动方 | `tcp_rcv_state_process()` → `tcp_data_queue()` |
| FIN_WAIT1 → FIN_WAIT2 | 收到 FIN 的 ACK | — | ACK | 主动方 | `tcp_rcv_state_process()` → `tcp_ack()` |
| FIN_WAIT1 → CLOSING | 收到 FIN（同时关闭） | ACK | FIN | 双方 | `tcp_rcv_state_process()` → `tcp_send_ack()` |
| FIN_WAIT1 → TIME_WAIT | 收到 FIN+ACK | ACK | FIN+ACK | 主动方 | `tcp_rcv_state_process()` → `tcp_time_wait()` |
| FIN_WAIT2 → TIME_WAIT | 收到 FIN | ACK | FIN | 主动方 | `tcp_rcv_state_process()` → `tcp_time_wait()` |
| CLOSE_WAIT → LAST_ACK | 应用调用 `close()` | FIN | — | 被动方 | `tcp_close()` → `tcp_send_fin()` |
| CLOSING → TIME_WAIT | 收到 ACK | — | ACK | 双方 | `tcp_rcv_state_process()` → `tcp_time_wait()` |
| LAST_ACK → CLOSED | 收到 ACK | — | ACK | 被动方 | `tcp_rcv_state_process()` → `tcp_done()` |
| TIME_WAIT → CLOSED | 2MSL 超时 | — | — | — | `tcp_time_wait()` → `inet_twsk_deschedule()` |
| ESTABLISHED → CLOSED | 收到 RST | — | RST | 任意 | `tcp_rcv_state_process()` → `tcp_done()` |
| SYN_RCVD → CLOSED | 收到 RST | — | RST | 服务端 | `tcp_rcv_state_process()` → `tcp_done()` |

#### 4.3.3 内核状态转换函数调用链

TCP 状态转换的核心入口函数是 [tcp_rcv_state_process()](file:///home/louis/code/linux/net/ipv4/tcp_input.c#L7170)，它通过 switch 语句分发到各子状态处理函数：

```
tcp_v4_rcv()                  ←  TCP 包接收入口
  │
  ├── __inet_lookup_skb()     ← 查找对应的 sock
  │
  └── tcp_v4_do_rcv()         ← 根据 socket 状态分派
        │
        ├── sk->sk_state == TCP_LISTEN
        │     └── tcp_v4_cookie_check() → tcp_check_req()
        │           └── tcp_conn_request() → 创建 request_sock，发 SYN+ACK
        │
        └── tcp_rcv_state_process(sk, skb)    ← 处理非 ESTABLISHED 状态
              │
              └── switch (sk->sk_state) {
                    │
                    case TCP_SYN_SENT:
                    │   └── tcp_rcv_synsent_state_process()
                    │         ├── 收到 SYN+ACK → tcp_finish_connect() → ESTABLISHED
                    │         └── 收到 SYN（同时打开）→ 发 SYN+ACK → SYN_RCVD
                    │
                    case TCP_SYN_RECV:
                    │   └── tcp_rcv_synrecv_state_process()
                    │         ├── 收到 ACK → tcp_init_transfer() → ESTABLISHED
                    │         └── 收到 RST → LISTEN
                    │
                    case TCP_FIN_WAIT1:
                    │   ├── 收到 ACK → FIN_WAIT2
                    │   ├── 收到 FIN（同时关闭）→ CLOSING
                    │   └── 收到 FIN+ACK → TIME_WAIT
                    │
                    case TCP_FIN_WAIT2:
                    │   └── 收到 FIN → TIME_WAIT
                    │
                    case TCP_CLOSE_WAIT:
                    │   └── 收到 FIN → 处理带外数据
                    │
                    case TCP_CLOSING:
                    │   └── 收到 ACK → TIME_WAIT
                    │
                    case TCP_LAST_ACK:
                    │   └── 收到 ACK → tcp_done() → CLOSED
                    │
                    case TCP_TIME_WAIT:
                    │   └── 收到 ACK → 重传 TIME_WAIT 的 ACK
                    │
                    case TCP_LISTEN:
                    │   └── 收到 SYN → tcp_conn_request()
                    │
                    default:
                    │   └── tcp_send_ack() → 发送确认
                    }

tcp_close()                   ← 应用层调用 close() 时触发
  │
  ├── tcp_send_fin()          ← 发送 FIN 包
  │
  └── tcp_set_state(sk, TCP_FIN_WAIT1)
        │
        └── 收到 ACK 后 → FIN_WAIT2
              │
              └── 收到 FIN 后 → TIME_WAIT
```

#### 4.3.4 关键数据包与状态变迁的对应关系

每个 TCP 数据包类型都会触发不同的状态变迁，内核通过 `tcp_rcv_state_process()` 中的 switch 语句统一处理：

```
SYN 包  ───► LISTEN 状态下收到 → SYN_RCVD（被动打开）
       ───► SYN_SENT 状态下收到 → SYN_RCVD（同时打开）

SYN+ACK ───► SYN_SENT 状态下收到 → ESTABLISHED（主动打开成功）

ACK 包  ───► SYN_RCVD 状态下收到 → ESTABLISHED（三次握手完成）
       ───► FIN_WAIT1 状态下收到 → FIN_WAIT2
       ───► CLOSING 状态下收到 → TIME_WAIT
       ───► LAST_ACK 状态下收到 → CLOSED

FIN 包  ───► ESTABLISHED 状态下收到 → CLOSE_WAIT（被动关闭）
       ───► FIN_WAIT1 状态下收到 → CLOSING（同时关闭）
       ───► FIN_WAIT2 状态下收到 → TIME_WAIT

RST 包  ───► SYN_RCVD 状态下收到 → LISTEN（连接被拒绝）
       ───► ESTABLISHED 状态下收到 → CLOSED（连接重置）
       ───► 任何非同步状态收到 → 释放连接
```

#### 4.3.5 特殊状态说明

**TIME_WAIT 状态（2MSL 等待）：**
- **持续时间**：2 × MSL（Maximum Segment Lifetime），通常为 60 秒
- **目的**：确保被动关闭方收到最后的 ACK（若丢失则重传 FIN）；防止旧连接数据包干扰新连接
- **内核实现**：`tcp_time_wait()` 创建 `struct inet_timewait_sock`，`tcp_twsk_destructor()` 释放

**CLOSING 状态（同时关闭）：**
- 双方同时调用 `close()`，各自发送 FIN 并收到对方的 FIN
- 此时双方都处于 CLOSING 状态，收到 ACK 后进入 TIME_WAIT
- 这是 RFC 793 定义的对称关闭场景

**状态转换验证宏：**
```c
// include/net/tcp_states.h
#define TCP_ACTION_FIN  (1 << TCP_CLOSE)  // FIN 动作掩码

// 用于快速判断状态是否可发送/接收数据的位图
enum {
    TCPF_ESTABLISHED = (1 << TCP_ESTABLISHED),
    TCPF_SYN_SENT    = (1 << TCP_SYN_SENT),
    TCPF_SYN_RECV    = (1 << TCP_SYN_RECV),
    TCPF_FIN_WAIT1   = (1 << TCP_FIN_WAIT1),
    TCPF_FIN_WAIT2   = (1 << TCP_FIN_WAIT2),
    TCPF_TIME_WAIT   = (1 << TCP_TIME_WAIT),
    TCPF_CLOSE       = (1 << TCP_CLOSE),
    TCPF_CLOSE_WAIT  = (1 << TCP_CLOSE_WAIT),
    TCPF_LAST_ACK    = (1 << TCP_LAST_ACK),
    TCPF_LISTEN      = (1 << TCP_LISTEN),
    TCPF_CLOSING     = (1 << TCP_CLOSING),
};
```

---

## 5. TCP 数据发送路径

### 5.1 tcp_sendmsg 发送入口

定义在 [net/ipv4/tcp.c](file:///home/louis/code/linux/net/ipv4/tcp.c) 中，是 TCP 发送系统调用的最终入口。

```c
int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
```

**发送流程：**

```
tcp_sendmsg()
  │
  ├── tcp_sendmsg_locked()  [tcp.c]
  │   │
  │   └── while (msg_data_left(msg)) 循环
  │       │
  │       ├── sk_stream_alloc_skb() → 分配 skb
  │       │
  │       ├── skb_add_data_nocache() → 从用户空间拷贝数据
  │       │
  │       └── tcp_push() → 决定是否立即发送
  │           │
  │           ├── __tcp_push_pending_frames()
  │           │   │
  │           │   └── tcp_write_xmit()  [tcp_output.c]
  │           │       │
  │           │       ├── while (true) 循环发送
  │           │       │   │
  │           │       │   ├── tcp_snd_test() → 检查发送条件
  │           │       │   │   ├── 拥塞窗口检查 (cwnd)
  │           │       │   │   ├── 接收窗口检查 (snd_wnd)
  │           │       │   │   └── Nagle 算法检查
  │           │       │   │
  │           │       │   ├── tcp_mss_split_point() → TSO 分段
  │           │       │   │
  │           │       │   ├── __tcp_transmit_skb() → 发送单个 skb
  │           │       │   │
  │           │       │   └── tcp_event_new_data_sent() → 更新状态
  │           │       │
  │           │       └── tcp_cwnd_validate() → 验证 cwnd 限制
  │           │
  │           └── tcp_schedule_loss_probe() → 调度 TLP 探测
  │
  └── tcp_push_pending_frames() → 推送所有待发帧
```

### 5.2 __tcp_transmit_skb 发送核心

定义在 [net/ipv4/tcp_output.c](file:///home/louis/code/linux/net/ipv4/tcp_output.c) 中，是 TCP 数据包发送的核心函数，构建 TCP 头部并调用 IP 层发送。

```c
static int __tcp_transmit_skb(struct sock *sk, struct sk_buff *skb,
                              int clone_it, gfp_t gfp_mask, u32 rcv_nxt)
{
    struct inet_sock *inet;
    struct tcp_sock *tp;
    struct tcp_skb_cb *tcb;
    struct tcphdr *th;

    // 1. 克隆 skb（重传时）
    if (clone_it) {
        oska = skb;
        skb = skb_clone(oskb, gfp_mask);
    }

    // 2. 构建 TCP 选项
    tcp_options_size = tcp_established_options(sk, skb, &opts, &key);
    tcp_header_size = tcp_options_size + sizeof(struct tcphdr);

    // 3. 构建 TCP 头部
    __skb_push(skb, tcp_header_size);
    th = (struct tcphdr *)skb->data;
    th->source  = inet->inet_sport;
    th->dest    = inet->inet_dport;
    th->seq     = htonl(tcb->seq);
    th->ack_seq = htonl(rcv_nxt);
    th->window  = htons(tcp_select_window(sk));

    // 4. 写入 TCP 选项
    tcp_options_write(th, tp, NULL, &opts, &key);

    // 5. 计算校验和
    INDIRECT_CALL_INET(icsk->icsk_af_ops->send_check,
                       tcp_v6_send_check, tcp_v4_send_check, sk, skb);

    // 6. 调用 IP 层发送
    err = INDIRECT_CALL_INET(icsk->icsk_af_ops->queue_xmit,
                             inet6_csk_xmit, ip_queue_xmit,
                             sk, skb, &inet->cork.fl);
    return err;
}
```

### 5.3 发送路径完整调用链

```
用户空间: write() / send() / sendmsg()
    │
    ▼
系统调用: __sys_sendto() → sock_sendmsg()
    │
    ▼
VFS: sock_write_iter() → __sock_sendmsg()
    │
    ▼
proto_ops: inet_sendmsg() → tcp_sendmsg()
    │
    ▼
TCP: tcp_sendmsg_locked() → tcp_push()
    │
    ▼
TCP: __tcp_push_pending_frames() → tcp_write_xmit()
    │
    ▼
TCP: __tcp_transmit_skb() → 构建 TCP 头部
    │
    ▼
INET: icsk_af_ops->queue_xmit → ip_queue_xmit()  [net/ipv4/ip_output.c]
    │
    ▼
IP: ip_local_out() → __ip_local_out() → dst_output()
    │
    ▼
IP: ip_output() → dev_queue_xmit()  [net/core/dev.c]
    │
    ▼
设备层: __dev_queue_xmit() → dev_hard_start_xmit()
    │
    ▼
驱动: e1000_xmit_frame() → 写入硬件 TX 描述符
```

---

## 6. TCP 数据接收路径

### 6.1 tcp_v4_rcv 接收入口

定义在 [net/ipv4/tcp_ipv4.c](file:///home/louis/code/linux/net/ipv4/tcp_ipv4.c) 中，是 TCP 数据包从 IP 层递送后的入口函数。

```c
int tcp_v4_rcv(struct sk_buff *skb)
{
    struct net *net = dev_net_rcu(skb->dev);
    const struct iphdr *iph;
    const struct tcphdr *th;
    struct sock *sk = NULL;
    bool refcounted;
    int ret;

    // 1. 基本校验
    if (skb->pkt_type != PACKET_HOST)
        goto discard_it;
    __TCP_INC_STATS(net, TCP_MIB_INSEGS);

    // 2. 校验 TCP 头部长度
    if (!pskb_may_pull(skb, sizeof(struct tcphdr)))
        goto discard_it;
    th = (const struct tcphdr *)skb->data;
    if (unlikely(th->doff < sizeof(struct tcphdr) / 4))
        goto bad_packet;

    // 3. 校验和检查
    if (skb_checksum_init(skb, IPPROTO_TCP, inet_compute_pseudo))
        goto csum_error;

    // 4. 查找 socket
lookup:
    sk = __inet_lookup_skb(skb, __tcp_hdrlen(th), th->source,
                           th->dest, sdif, &refcounted);
    if (!sk)
        goto no_tcp_socket;  // 无对应 socket，发送 RST

    // 5. 处理 TIME_WAIT 状态
    if (sk->sk_state == TCP_TIME_WAIT)
        goto do_time_wait;

    // 6. 处理 TCP_NEW_SYN_RECV 状态
    if (sk->sk_state == TCP_NEW_SYN_RECV) {
        // 从 request_sock 获取 listener
        // 调用 tcp_check_req() 完成握手
    }

    // 7. 安全检查（防火墙、min_ttl、xfrm）
process:
    if (tcp_filter(sk, skb, &drop_reason))
        goto discard_and_relse;

    // 8. 填充 TCP 控制块
    tcp_v4_fill_cb(skb, iph, th);

    // 9. 分派处理
    if (sk->sk_state == TCP_LISTEN) {
        ret = tcp_v4_do_rcv(sk, skb);  // 监听 socket 处理
    } else {
        bh_lock_sock_nested(sk);
        if (!sock_owned_by_user(sk)) {
            ret = tcp_v4_do_rcv(sk, skb);  // 快速路径
        } else {
            tcp_add_backlog(sk, skb, &drop_reason);  // backlog 排队
        }
        bh_unlock_sock(sk);
    }
    return 0;
}
```

### 6.2 tcp_v4_do_rcv 接收分派

定义在 [net/ipv4/tcp_ipv4.c](file:///home/louis/code/linux/net/ipv4/tcp_ipv4.c) 中，根据 socket 状态分派到不同的处理路径。

```c
int tcp_v4_do_rcv(struct sock *sk, struct sk_buff *skb)
{
    if (sk->sk_state == TCP_ESTABLISHED) {
        /* 快速路径：已建立连接 */
        tcp_rcv_established(sk, skb);
        return 0;
    }

    // 校验和检查
    if (tcp_checksum_complete(skb))
        goto csum_err;

    if (sk->sk_state == TCP_LISTEN) {
        // 处理 SYN 包，创建新连接
        struct sock *nsk = tcp_v4_cookie_check(sk, skb);
        if (nsk != sk) {
            reason = tcp_child_process(sk, nsk, skb);
        }
    } else {
        // 其他状态（SYN_SENT, CLOSE_WAIT, FIN_WAIT 等）
        reason = tcp_rcv_state_process(sk, skb);
    }
    return 0;
}
```

### 6.3 tcp_rcv_established 快速路径

定义在 [net/ipv4/tcp_input.c](file:///home/louis/code/linux/net/ipv4/tcp_input.c) 中，是 TCP 快速路径处理函数，采用 Van Jacobson 头部预测算法。

```c
void tcp_rcv_established(struct sock *sk, struct sk_buff *skb)
{
    struct tcp_sock *tp = tcp_sk(sk);
    const struct tcphdr *th = (const struct tcphdr *)skb->data;
    unsigned int len = skb->len;

    // 头部预测检查
    if ((tcp_flag_word(th) & TCP_HP_BITS) == tp->pred_flags &&
        TCP_SKB_CB(skb)->seq == tp->rcv_nxt &&
        !after(TCP_SKB_CB(skb)->ack_seq, tp->snd_nxt)) {

        /* 快速路径：头部预测命中 */
        if (len <= tcp_header_len) {
            // 纯 ACK 包
            tcp_ack(sk, skb, flag);
            __kfree_skb(skb);
            tcp_data_snd_check(sk);
            return;
        } else {
            /* 数据包 */
            if (tcp_header_len == sizeof(struct tcphdr) + TCPOLEN_TSTAMP_ALIGNED) {
                // 时间戳检查
                if (!tcp_parse_aligned_timestamp(tp, th))
                    goto slow_path;
            }
            // 直接接收数据
            tcp_data_queue(sk, skb);
        }
    } else {
slow_path:
        /* 慢速路径：头部预测失败 */
        tcp_ack(sk, skb, FLAG_SLOWPATH);
        tcp_data_queue(sk, skb);
    }
}
```

### 6.4 tcp_data_queue 数据入队

定义在 [net/ipv4/tcp_input.c](file:///home/louis/code/linux/net/ipv4/tcp_input.c) 中，处理数据包的入队和重组。

```c
static void tcp_data_queue(struct sock *sk, struct sk_buff *skb)
{
    struct tcp_sock *tp = tcp_sk(sk);

    if (TCP_SKB_CB(skb)->seq == tp->rcv_nxt) {
        /* 有序数据：直接入队 */
        if (tcp_try_rmem_schedule(sk, skb, skb->truesize)) {
            // 内存不足，触发零窗口通告
            inet_csk(sk)->icsk_ack.pending |= (ICSK_ACK_NOMEM | ICSK_ACK_NOW);
            return;
        }

        eaten = tcp_queue_rcv(sk, skb, &fragstolen);
        if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
            tcp_fin(sk);  // 处理 FIN 标志

        // 处理乱序队列中的新数据
        if (!RB_EMPTY_ROOT(&tp->out_of_order_queue)) {
            tcp_ofo_queue(sk);
        }
    } else if (after(TCP_SKB_CB(skb)->seq, tp->rcv_nxt)) {
        /* 乱序数据：插入乱序红黑树 */
        tcp_data_queue_ofo(sk, skb);
    } else {
        /* 重复包或窗口外数据 */
        tcp_data_queue_ofo(sk, skb);  // 或直接丢弃
    }
}
```

### 6.5 接收路径完整调用链

```
网卡硬件 → DMA 写入 RX 环
    │
    ▼
e1000_intr() → napi_schedule() → e1000_clean()  [中断处理]
    │
    ▼
e1000_clean_rx_irq() → e1000_receive_skb() → napi_gro_receive()
    │
    ▼
GRO 处理 → netif_receive_skb()  [net/core/dev.c]
    │
    ▼
__netif_receive_skb_core() → ip_rcv()  [协议分派]
    │
    ▼
ip_rcv_finish() → ip_local_deliver() → ip_local_deliver_finish()
    │
    ▼
tcp_v4_rcv()  [net/ipv4/tcp_ipv4.c:2147]
    │
    ├── __inet_lookup_skb() → 查找 socket
    ├── tcp_filter() → BPF 过滤
    │
    ▼
tcp_v4_do_rcv()  [tcp_ipv4.c:1859]
    │
    ├── [ESTABLISHED] → tcp_rcv_established()  [tcp_input.c:6519]
    │   │
    │   ├── 头部预测命中 → 快速路径
    │   │   ├── 纯 ACK → tcp_ack() → tcp_data_snd_check()
    │   │   └── 数据包 → tcp_data_queue()
    │   │
    │   └── 头部预测未命中 → 慢速路径
    │       ├── tcp_ack() with FLAG_SLOWPATH
    │       └── tcp_data_queue()
    │
    ├── [LISTEN] → tcp_v4_cookie_check() → tcp_check_req()
    │
    └── [其他状态] → tcp_rcv_state_process()
        │
        ├── SYN_SENT → tcp_rcv_synsent_state_process()
        ├── FIN_WAIT1 → tcp_rcv_state_process()
        ├── CLOSE_WAIT → tcp_rcv_state_process()
        └── LAST_ACK → tcp_rcv_state_process()
```

---

## 7. TCP 拥塞控制

### 7.1 拥塞控制框架

TCP 拥塞控制通过可插拔的 `tcp_congestion_ops` 结构实现。内核提供多种算法：
- **CUBIC**：默认算法，适合高带宽长距离网络
- **BBR**：基于瓶颈带宽和 RTT 的模型
- **Reno**：传统算法
- **DCTCP**：数据中心 TCP

**拥塞控制状态机：**

```
    ┌──────────────────────────────────────────────────┐
    │                                                   │
    ▼                                                   │
  Open ──(丢包/ECN)──► Disorder ──(3个重复ACK)──► Recovery
    ▲                                                   │
    │                                                   │
    └───────────────────(恢复完成)───────────────────────┘
                                                         │
    Loss ────(RTO超时)───────────────────────────────────┘
```

**拥塞状态定义：**

```c
enum tcp_ca_state {
    TCP_CA_Open     = 0,  // 正常状态
    TCP_CA_Disorder = 1,  // 乱序（可能丢包）
    TCP_CA_CWR      = 2,  // 拥塞窗口缩减
    TCP_CA_Recovery = 3,  // 快速恢复
    TCP_CA_Loss     = 4,  // 超时恢复
};
```

### 7.2 RTT 估计与拥塞窗口调整

RTT 估计由 `tcp_ack()` 处理过程中调用 `tcp_rcv_rtt_measure_ts()` 更新：

```c
// tcp_input.c 中的 RTT 采样
// srtt_us = 平滑 RTT (微秒 << 3)
// mdev_us = 平均偏差
// rttvar_us = RTT 方差

// RTO 计算：RTO = srtt + max(G, 4 * rttvar)
// 其中 G 是时钟粒度
```

### 7.3 慢启动与拥塞避免

**慢启动阶段：**
- 每收到一个 ACK，`snd_cwnd` 增加 1 个 MSS
- 指数增长，直到达到 `snd_ssthresh`

**拥塞避免阶段：**
- CUBIC：使用三次函数模型，在丢包附近保持保守，在带宽探测时积极增长
- 每收到一个 ACK，`snd_cwnd` 增加 `MSS * (MSS / cwnd)` (Reno 风格)

### 7.4 快速重传与快速恢复

当收到 3 个重复 ACK 时触发快速重传：

```
tcp_ack() → tcp_fastretrans_alert()
  │
  ├── tcp_time_to_recover() → 判断是否进入恢复
  │
  ├── tcp_enter_recovery() → 进入 Recovery 状态
  │   ├── ssthresh = cwnd / 2
  │   ├── cwnd = ssthresh + 3  (Reno 风格)
  │   └── tcp_reset_reno_sack() → 重置 SACK 计数
  │
  ├── tcp_retransmit_skb() → 重传丢失的数据包
  │
  └── tcp_try_undo_recovery() → 退出恢复
```

---

## 8. TCP 定时器

### 8.1 重传定时器

定义在 `inet_connection_sock->icsk_retransmit_timer`，使用 `tcp_write_timer()` 处理。

- RTO 指数退避：`timeout = min(1000ms, srtt + max(G, 4*rttvar)) << backoff`
- 达到 `tcp_retries1` 时通知 IP 层更新路由
- 达到 `tcp_retries2` 时断开连接

### 8.2 延迟 ACK 定时器

定义在 `inet_connection_sock->icsk_delack_timer`，使用 `tcp_delack_timer()` 处理。

- 默认延迟 40ms 发送 ACK
- 收到第二个数据包时立即发送 ACK (pingpong 模式)
- 定时器到期时强制发送 ACK

### 8.3 保活定时器

定义在 `sock->sk_timer`，使用 `tcp_keepalive_timer()` 处理。

- `tcp_keepalive_time` (默认 7200s) 无数据后启动
- `tcp_keepalive_intvl` (默认 75s) 探测间隔
- `tcp_keepalive_probes` (默认 9) 失败后断开

### 8.4 TLP 定时器

Tail Loss Probe (TLP) 使用 `tcp_schedule_loss_probe()` 调度。

- 在发送窗口空闲时发送最后一个数据的探测包
- 通过 RACK 机制检测是否丢包

---

## 9. IPv4 协议层

### 9.1 IP 层接收路径

```
ip_rcv()  [net/ipv4/ip_input.c]
  │
  ├── skb_checksum_simple_validate() → 校验和检查
  ├── ip_rcv_finish_core() → 路由查找
  └── ip_rcv_finish()
      │
      ├── dst_input(skb) → ip_local_deliver()
      │   │
      │   └── ip_local_deliver_finish()
      │       │
      │       ├── ip_protocol_deliver_rcu() → 按协议分派
      │       │   ├── IPPROTO_TCP → tcp_v4_rcv()
      │       │   ├── IPPROTO_UDP → udp_rcv()
      │       │   └── IPPROTO_ICMP → icmp_rcv()
      │       │
      │       └── RAW socket 递送
      │
      └── ip_forward() → 转发
```

### 9.2 IP 层发送路径

```
ip_queue_xmit()  [net/ipv4/ip_output.c]
  │
  ├── ip_local_out() → __ip_local_out()
  │   │
  │   └── dst_output() → ip_output()
  │       │
  │       └── ip_finish_output()
  │           │
  │           ├── ip_finish_output2() → 邻居子系统
  │           │   └── neigh_output() → dev_queue_xmit()
  │           │
  │           └── ip_fragment() → 分片（如果需要）
  │
  └── 路由缓存更新
```

---

## 10. UDP 协议实现

### 10.1 UDP 核心数据结构

UDP 使用 `struct udp_sock` (定义在 `include/linux/udp.h`)，继承自 `struct inet_sock`。

```c
struct udp_sock {
    struct inet_sock    inet;          /* 继承 INET sock */
    int                 pending;       /* 待发送的任何帧 */
    __u8                corkflag;      /* UDP_CORK 标志 */
    __u8                encap_type;    /* 封装类型 */
    __u16               len;           /* 总长度 */
    // ... 其他字段
};
```

### 10.2 UDP 发送与接收

**发送：** `udp_sendmsg()` → `udp_push_pending_frames()`
**接收：** `udp_rcv()` → `__udp4_lib_rcv()` → `udp_queue_rcv_skb()`

---

## 11. 关键函数接口

### 11.1 TCP 协议操作函数

定义在 `net/ipv4/tcp_ipv4.c` 中的 `tcp_prot` 结构：

```c
struct proto tcp_prot = {
    .name           = "TCP",
    .owner          = THIS_MODULE,
    .close          = tcp_close,
    .connect        = tcp_v4_connect,
    .disconnect     = tcp_disconnect,
    .accept         = inet_csk_accept,
    .ioctl          = tcp_ioctl,
    .init           = tcp_v4_init_sock,
    .destroy        = tcp_v4_destroy_sock,
    .shutdown       = tcp_shutdown,
    .setsockopt     = tcp_setsockopt,
    .getsockopt     = tcp_getsockopt,
    .keepalive      = tcp_set_keepalive,
    .recvmsg        = tcp_recvmsg,
    .sendmsg        = tcp_sendmsg,
    .splice_read    = tcp_splice_read,
    .read_sock      = tcp_read_sock,
    .read_skb       = tcp_read_skb,
    .sendpage       = tcp_sendpage,
    .backlog_rcv    = tcp_v4_do_rcv,
    .release_cb     = tcp_release_cb,
    .hash           = inet_hash,
    .unhash         = inet_unhash,
    .get_port       = inet_csk_get_port,
    // ... 其他字段
};
```

### 11.2 INET 连接操作函数

定义在 `net/ipv4/af_inet.c` 中的 `inet_stream_ops` 结构：

```c
const struct proto_ops inet_stream_ops = {
    .family     = PF_INET,
    .owner      = THIS_MODULE,
    .release    = inet_release,          // close 系统调用
    .bind       = inet_bind,             // bind 系统调用
    .connect    = inet_stream_connect,   // connect 系统调用
    .accept     = inet_accept,           // accept 系统调用
    .listen     = inet_listen,           // listen 系统调用
    .poll       = tcp_poll,              // poll/select/epoll
    .sendmsg    = inet_sendmsg,          // send/sendto/sendmsg
    .recvmsg    = inet_recvmsg,          // recv/recvfrom/recvmsg
    .mmap       = tcp_mmap,              // mmap 零拷贝
    .splice_read = tcp_splice_read,      // splice 零拷贝
};
```

---

## 12. 附录：关键文件列表

| 文件路径 | 说明 |
|---------|------|
| [net/ipv4/tcp.c](file:///home/louis/code/linux/net/ipv4/tcp.c) | TCP 核心协议实现（sendmsg、recvmsg、setsockopt 等） |
| [net/ipv4/tcp_input.c](file:///home/louis/code/linux/net/ipv4/tcp_input.c) | TCP 输入处理（tcp_rcv_established、tcp_data_queue 等） |
| [net/ipv4/tcp_output.c](file:///home/louis/code/linux/net/ipv4/tcp_output.c) | TCP 输出处理（tcp_write_xmit、__tcp_transmit_skb 等） |
| [net/ipv4/tcp_ipv4.c](file:///home/louis/code/linux/net/ipv4/tcp_ipv4.c) | TCP/IPv4 协议处理（tcp_v4_rcv、tcp_v4_connect 等） |
| [net/ipv4/tcp_timer.c](file:///home/louis/code/linux/net/ipv4/tcp_timer.c) | TCP 定时器管理 |
| [net/ipv4/tcp_cong.c](file:///home/louis/code/linux/net/ipv4/tcp_cong.c) | TCP 拥塞控制框架 |
| [net/ipv4/tcp_fastopen.c](file:///home/louis/code/linux/net/ipv4/tcp_fastopen.c) | TCP 快速打开 |
| [net/ipv4/udp.c](file:///home/louis/code/linux/net/ipv4/udp.c) | UDP 协议实现 |
| [net/ipv4/ip_input.c](file:///home/louis/code/linux/net/ipv4/ip_input.c) | IP 层输入处理 |
| [net/ipv4/ip_output.c](file:///home/louis/code/linux/net/ipv4/ip_output.c) | IP 层输出处理 |
| [include/linux/tcp.h](file:///home/louis/code/linux/include/linux/tcp.h) | tcp_sock 等核心数据结构定义 |
| [include/net/tcp.h](file:///home/louis/code/linux/include/net/tcp.h) | TCP 协议头文件（拥塞控制、函数声明等） |
| [include/net/inet_connection_sock.h](file:///home/louis/code/linux/include/net/inet_connection_sock.h) | 面向连接 INET socket 结构 |
| [include/uapi/linux/tcp.h](file:///home/louis/code/linux/include/uapi/linux/tcp.h) | TCP 头部结构 (tcphdr) 和用户空间 API 定义 |