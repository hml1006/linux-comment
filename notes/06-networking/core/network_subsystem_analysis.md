# Linux 内核网络子系统分析

## 目录

1. [概述](#1-概述)
2. [网络子系统整体架构](#2-网络子系统整体架构)
3. [核心层 net/core](#3-核心层-netcore)
   - 3.1 [socket 层](#31-socket-层)
   - 3.2 [设备管理层 (dev.c)](#32-设备管理层-devc)
   - 3.3 [sk_buff 管理](#33-sk_buff-管理)
   - 3.4 [网络协议注册](#34-网络协议注册)
   - 3.5 [邻居子系统 (neighbour)](#35-邻居子系统-neighbour)
   - 3.6 [过滤与分类 (filter)](#36-过滤与分类-filter)
   - 3.7 [GRO/GSO 卸载](#37-grogso-卸载)
   - 3.8 [RTNetlink](#38-rtnetlink)
4. [网络协议族](#4-网络协议族)
   - 4.1 [INET (IPv4)](#41-inet-ipv4)
   - 4.2 [INET6 (IPv6)](#42-inet6-ipv6)
   - 4.3 [UNIX 域套接字](#43-unix-域套接字)
   - 4.4 [PACKET 套接字](#44-packet-套接字)
   - 4.5 [Netlink](#45-netlink)
5. [关键数据结构](#5-关键数据结构)
   - 5.1 [struct socket](#51-struct-socket)
   - 5.2 [struct sock](#52-struct-sock)
   - 5.3 [struct sk_buff](#53-struct-sk_buff)
   - 5.4 [struct net_device](#54-struct-net_device)
   - 5.5 [struct net](#55-struct-net)
6. [数据包收发流程](#6-数据包收发流程)
   - 6.1 [发送路径概述](#61-发送路径概述)
   - 6.2 [接收路径概述](#62-接收路径概述)
7. [网络设备驱动模型](#7-网络设备驱动模型)
   - 7.1 [NAPI 机制](#71-napi-机制)
   - 7.2 [网络设备注册](#72-网络设备注册)
8. [配置与编译](#8-配置与编译)

---

## 1. 概述

Linux 网络子系统是一个层次化的协议栈实现，从底层的网络设备驱动到上层的 socket 系统调用接口，涵盖完整的网络通信功能。本分析基于内核源码 `net/` 目录，代码版本为 Linux 6.x。

网络子系统的主要构成：

```
net/
├── core/          # 核心层：socket、设备管理、sk_buff、邻居、过滤
├── ipv4/          # IPv4/TCP/UDP/ICMP 协议栈
├── ipv6/          # IPv6 协议栈
├── socket.c       # VFS 与 socket 层接口（顶层）
├── ethernet/      # 以太网帧处理
├── bridge/        # 网桥
├── netfilter/     # 防火墙/Netfilter
├── sched/         # 流量控制 (Qdisc)
├── xfrm/          # IPSec 加密
├── tls/           # TLS 卸载
├── unix/          # UNIX 域套接字
├── packet/        # AF_PACKET 原始包
├── netlink/       # Netlink 通信
├── 8021q/         # VLAN 802.1Q
├── wireless/      # 无线子系统
├── mac80211/      # 802.11 MAC 层
└── dsa/           # 分布式交换架构 (DSA)
```

内核编译配置（`.config` 中相关选项）：

```
CONFIG_NET=y
CONFIG_INET=y
CONFIG_TCP_CONG_CUBIC=y
CONFIG_E1000=y      # Intel PRO/1000 驱动
CONFIG_E1000E=y     # Intel PRO/1000e 驱动
CONFIG_IGB=y        # Intel Gigabit Ethernet 驱动
CONFIG_IGBVF=y      # Intel IGB 虚拟功能驱动
```

---

## 2. 网络子系统整体架构

Linux 网络子系统遵循 OSI 分层模型，从上到下分为 5 层：

```
┌─────────────────────────────────────────────────────┐
│               用户空间 (User Space)                   │
│  socket() bind() listen() connect() send() recv()   │
├─────────────────────────────────────────────────────┤
│  VFS 层 (net/socket.c)                              │
│  sys_socket → sock_create → sock_alloc               │
├─────────────────────────────────────────────────────┤
│  Socket 层 (net/core/sock.c)                         │
│  struct socket → struct sock → proto_ops             │
├─────────────────────────────────────────────────────┤
│  传输层 (net/ipv4/{tcp,udp}.c)                       │
│  TCP: 面向连接、可靠传输、拥塞控制                    │
│  UDP: 无连接、不可靠传输                              │
├─────────────────────────────────────────────────────┤
│  网络层 (net/ipv4/{ip_input,ip_output,route}.c)      │
│  IP 路由、分片重组、转发、netfilter 钩子               │
├─────────────────────────────────────────────────────┤
│  链路层 (net/core/dev.c, net/ethernet/)               │
│  设备管理、邻居协议 (ARP/NDISC)、GRO/GSO              │
├─────────────────────────────────────────────────────┤
│  网络设备驱动 (drivers/net/ethernet/intel/)           │
│  e1000/e1000e/igb — NAPI、 TX/RX Ring                │
└─────────────────────────────────────────────────────┘
```

### 2.1 分层职责

| 层 | 核心文件 | 主要职责 |
|----|---------|---------|
| VFS/Socket | `net/socket.c` | 系统调用入口、VFS 文件操作绑定、`struct socket` 管理 |
| 传输层 | `net/ipv4/tcp.c`, `udp.c` | 端到端通信、可靠传输、拥塞控制、流量控制 |
| 网络层 | `net/ipv4/ip_input.c`, `ip_output.c`, `route.c` | IP 路由、分片、组播、Netfilter 钩子 |
| 链路层 | `net/core/dev.c`, `net/ethernet/eth.c` | 设备发现、帧收发、协议派发、NAPI |
| 驱动层 | `drivers/net/ethernet/intel/e1000/` | 硬件初始化、DMA 传输、中断处理 |

### 2.2 关键抽象

网络子系统通过多层抽象实现协议无关性：

- **VFS 抽象**：socket 通过 `struct file` 与 VFS 集成，`read/write` 操作映射到 `send/recv`
- **Socket 抽象**：`struct socket` 是通用 socket 表示，`struct proto_ops` 提供协议族相关操作
- **Sock 抽象**：`struct sock` 是协议无关的 socket 内部表示，`struct proto` 提供传输层协议操作
- **SKB 抽象**：`struct sk_buff` 是贯穿整个协议栈的数据包表示
- **设备抽象**：`struct net_device` 统一所有网络接口

---

## 3. 核心层 net/core

`net/core/` 是网络子系统的核心基础设施，提供以下关键功能：

### 3.1 socket 层

文件：`net/core/sock.c` (4554 行)

提供通用 socket 支持例程：
- 内存分配器 (`sk_alloc`, `sk_free`)
- Socket 锁/释放处理 (`lock_sock`, `release_sock`)
- 通用选项处理器 (`sock_setsockopt`, `sock_getsockopt`)
- 等待队列管理 (`sk_sleep`, `sk_wait_event`)

### 3.2 设备管理层 (dev.c)

文件：`net/core/dev.c` — 网络设备管理的核心

关键功能：
- `dev_queue_xmit()` — 网络设备发送入口
- `netif_receive_skb()` — 接收路径分发
- `register_netdevice()` / `unregister_netdevice()` — 设备生命周期管理
- `__netif_rx_schedule()` — NAPI 调度
- `net_dev_init()` — 网络子系统初始化（软中断 `NET_RX_SOFTIRQ` / `NET_TX_SOFTIRQ`）

### 3.3 sk_buff 管理

文件：`net/core/skbuff.c` — 数据包缓冲区管理

- `alloc_skb()` — 分配 sk_buff
- `skb_copy()` / `skb_clone()` — 包复制/克隆
- `skb_push()` / `skb_pull()` / `skb_put()` / `skb_reserve()` — 缓冲区指针操作
- `skb_checksum_help()` — 校验和计算

### 3.4 网络协议注册

文件：`net/core/dev.c` — `dev_add_pack()` / `dev_remove_pack()`

通过 `struct packet_type` 注册协议处理函数：
```c
// net/core/dev.c
static struct packet_type ip_packet_type __read_mostly = {
    .type = cpu_to_be16(ETH_P_IP),
    .func = ip_rcv,           // IPv4 接收入口
    .list_func = ip_list_rcv, // GRO 批量接收
};
```

### 3.5 邻居子系统 (neighbour)

文件：`net/core/neighbour.c`

实现 ARP (IPv4) 和 NDISC (IPv6) 的邻居发现协议：
- `struct neighbour` — 邻居表项
- `neigh_lookup()` — 邻居查找
- `neigh_resolve_output()` — 邻居解析后的输出
- `arp_rcv()` — ARP 包接收处理

### 3.6 过滤与分类 (filter)

文件：`net/core/filter.c`

BPF (Berkeley Packet Filter) 过滤支持：
- `sk_attach_filter()` / `sk_detach_filter()` — Socket 过滤
- 用于 `tcpdump` 等抓包工具的内核支持

### 3.7 GRO/GSO 卸载

- **GRO (Generic Receive Offload)**：`net/core/gro.c`
  - 将多个相似的小包合并为一个大包，减少协议栈处理开销
  - `napi_gro_receive()` → `gro_cells_receive()` 入口
  - `dev_gro_receive()` — GRO 处理核心
  - `napi_skb_finish()` — 完成 GRO 或走正常路径

- **GSO (Generic Segment Offload)**：`net/core/gso.c`
  - 将大包分割成多个 MTU 大小的包
  - `skb_gso_segment()` — GSO 分割核心

### 3.8 RTNetlink

文件：`net/core/rtnetlink.c`

内核与用户空间通过 Netlink 进行网络配置通信：
- `rtmsg_ifinfo()` — 发送网络设备信息
- `do_setlink()` — 设置网络设备属性
- `rtnl_link_ops` 注册机制

---

## 4. 网络协议族

### 4.1 INET (IPv4)

`net/ipv4/` 是 TCP/IP 协议栈的 IPv4 实现，主要文件：

| 文件 | 功能 |
|------|------|
| `af_inet.c` | PF_INET 协议族创建、注册、`inet_init()` 初始化 |
| `tcp.c` | TCP 协议核心（发送/接收/状态机） |
| `tcp_input.c` | TCP 输入处理（ACK 处理、重传、窗口管理） |
| `tcp_output.c` | TCP 输出处理（发送段构造） |
| `tcp_timer.c` | TCP 定时器（RTO、keepalive、延迟 ACK） |
| `tcp_cong.c` | TCP 拥塞控制框架 |
| `tcp_cubic.c` | CUBIC 拥塞控制算法 |
| `udp.c` | UDP 协议实现 |
| `ip_input.c` | IP 层输入处理 |
| `ip_output.c` | IP 层输出处理 |
| `route.c` | IP 路由查找 |
| `fib_trie.c` | FIB (Forwarding Information Base) 路由表 |
| `arp.c` | ARP 协议实现 |
| `icmp.c` | ICMP 协议实现 |
| `raw.c` | RAW 套接字 |
| `ping.c` | Ping 套接字 |
| `netfilter.c` | IPv4 Netfilter 钩子注册 |

**初始化流程** (`inet_init` at `af_inet.c:1887`):

```
inet_init()
├── proto_register(&tcp_prot)    // 注册 TCP 协议
├── proto_register(&udp_prot)    // 注册 UDP 协议
├── proto_register(&raw_prot)    // 注册 RAW 协议
├── proto_register(&ping_prot)   // 注册 Ping 协议
├── sock_register(&inet_family_ops)  // 注册 PF_INET 协议族
├── inet_add_protocol(&icmp_protocol, IPPROTO_ICMP)
├── inet_add_protocol(&udp_protocol, IPPROTO_UDP)
├── inet_add_protocol(&tcp_protocol, IPPROTO_TCP)
├── inet_register_protosw()  // 注册 SOCK_STREAM/TCP, SOCK_DGRAM/UDP 等
└── dev_add_pack(&ip_packet_type)  // 注册 ETH_P_IP 协议处理器
```

### 4.2 INET6 (IPv6)

`net/ipv6/` 实现 IPv6 协议栈，核心文件包括：
- `af_inet6.c` — PF_INET6 协议族创建
- `ip6_input.c` / `ip6_output.c` — IPv6 收发
- `route.c` — IPv6 路由
- `addrconf.c` — IPv6 地址配置
- `ndisc.c` — 邻居发现协议 (NDP)

### 4.3 UNIX 域套接字

`net/unix/af_unix.c` — 本地进程间通信（IPC），不经过网络协议栈。

### 4.4 PACKET 套接字

`net/packet/af_packet.c` — 提供原始网络帧访问，用于 `tcpdump`、`libpcap` 等。

### 4.5 Netlink

`net/netlink/` — 内核与用户空间的通信机制，用于网络配置管理。

---

## 5. 关键数据结构

### 5.1 struct socket

文件：`include/linux/net.h:110`

```c
struct socket {
    socket_state        state;      // 连接状态 (SS_CONNECTED 等)
    short               type;       // 套接字类型 (SOCK_STREAM, SOCK_DGRAM 等)
    unsigned long       flags;      // 标志位 (SOCK_NOSPACE 等)
    struct file         *file;      // 关联的 VFS file 结构 (用于垃圾回收)
    struct sock         *sk;        // 内部协议无关的 socket 表示
    const struct proto_ops *ops;    // 协议族操作函数表 (如 inet_stream_ops)
    struct socket_wq    wq;         // 等待队列
};
```

**关键说明**：
- `state`：socket 连接状态，如 `SS_UNCONNECTED`、`SS_CONNECTING`、`SS_CONNECTED`、`SS_DISCONNECTING`
- `ops`：协议族操作向量，IPv4 TCP 对应 `inet_stream_ops`，IPv4 UDP 对应 `inet_dgram_ops`
- `sk`：指向 `struct sock` 的指针，是协议层的内部表示

### 5.2 struct sock

文件：`include/net/sock.h:360`

`struct sock` 是网络层 socket 的内部表示，包含大量字段，按缓存行分组：

```c
struct sock {
    struct sock_common   __sk_common;  // 公共字段（与 tw_sock 共享）
    // 宏定义访问 sk_common 中的字段
    // sk_node, sk_refcnt, sk_hash, sk_daddr, sk_rcv_saddr
    // sk_family, sk_state, sk_prot, sk_net 等

    /* 写频繁的 RX 路径 */
    __cacheline_group_begin(sock_write_rx);
    atomic_t        sk_drops;           // 丢弃计数
    struct sk_buff_head sk_error_queue;  // 错误队列
    struct sk_buff_head sk_receive_queue; // 接收队列
    struct {                            // 背压队列 (backlog)
        atomic_t    rmem_alloc;
        int         len;
        struct sk_buff *head, *tail;
    } sk_backlog;
    __cacheline_group_end(sock_write_rx);

    /* 读频繁的 RX 路径 */
    __cacheline_group_begin(sock_read_rx);
    struct dst_entry __rcu *sk_rx_dst;  // 接收路由缓存
    int         sk_rcvbuf;              // 接收缓冲区大小
    struct sk_filter __rcu *sk_filter;  // BPF 过滤
    void        (*sk_data_ready)(struct sock *sk); // 数据就绪回调
    long        sk_rcvtimeo;            // 接收超时
    __cacheline_group_end(sock_read_rx);

    /* 读写锁保护 */
    __cacheline_group_begin(sock_write_rxtx);
    socket_lock_t       sk_lock;        // socket 锁
    int         sk_forward_alloc;       // 预分配内存
    __cacheline_group_end(sock_write_rxtx);

    /* 写频繁的 TX 路径 */
    __cacheline_group_begin(sock_write_tx);
    int         sk_wmem_queued;         // 发送队列占用量
    refcount_t  sk_wmem_alloc;          // 发送中 skb 引用计数
    struct sk_buff_head sk_write_queue;  // 发送队列
    struct timer_list   sk_timer;       // 通用定时器 / TCP 重传定时器
    unsigned long       sk_pacing_rate; // 发送速率控制
    __cacheline_group_end(sock_write_tx);

    /* 读频繁的 TX 路径 */
    __cacheline_group_begin(sock_read_tx);
    u16         sk_protocol;            // 协议号 (IPPROTO_TCP/UDP)
    u16         sk_type;                // socket 类型
    struct dst_entry __rcu *sk_dst_cache; // 路由缓存
    int         sk_sndbuf;              // 发送缓冲区大小
    __cacheline_group_end(sock_read_tx);

    // 其他字段...
    struct proto *sk_prot_creator;      // 创建者协议
    u8          sk_shutdown;            // 关闭标志
    u32         sk_ack_backlog;         // 当前 listen backlog
    u32         sk_max_ack_backlog;     // 最大 listen backlog
};
```

**继承层次**:
```
struct sock_common
    └── struct sock        (通用 socket)
           └── struct inet_sock   (INET 协议族扩展)
                  └── struct inet_connection_sock  (面向连接扩展)
                         └── struct tcp_sock       (TCP 协议)
```

### 5.3 struct sk_buff

文件：`include/linux/skbuff.h:885`

`struct sk_buff` 是贯穿整个协议栈的数据包表示，是网络子系统中最重要的数据结构：

```c
struct sk_buff {
    union {
        struct {
            struct sk_buff  *next, *prev;  // 链表指针
            union {
                struct net_device *dev;    // 关联的网络设备
                unsigned long dev_scratch;
            };
        };
        struct rb_node      rbnode;        // 红黑树节点 (TCP 重传队列)
        struct list_head    list;
    };

    struct sock     *sk;                   // 关联的 socket

    union {
        ktime_t     tstamp;                // 时间戳
        u64         skb_mstamp_ns;
    };

    char            cb[48] __aligned(8);   // 控制缓冲区 (各层私有数据)
    // TCP: struct tcp_skb_cb
    // IP:  struct inet_skb_parm

    unsigned int    len;                   // 实际数据长度
    unsigned int    data_len;              // 非线性区数据长度
    __u16           mac_len;               // MAC 头长度
    __u16           hdr_len;               // 协议头长度 (用于 GSO)
    __u16           queue_mapping;         // 队列映射

    // 标志位
    __u8            cloned:1;              // 克隆副本
    __u8            nohdr:1;               // 仅头部引用
    __u8            pkt_type:3;            // 包类型 (PACKET_HOST/BROADCAST/MULTICAST)
    __u8            ip_summed:2;           // 校验和状态
    __u8            encapsulation:1;       // 封装标志 (隧道)

    // 头部指针
    union {
        __u8    *mac_header;               // L2 MAC 头
        __u8    *network_header;           // L3 网络头
        __u8    *transport_header;         // L4 传输头
    };

    // 数据区
    struct skb_shared_info *skb_shinfo(skb); // 共享信息 (frags, frag_list, gso_segs)
};
```

**sk_buff 数据布局**:
```
┌───────────────────────────────────────────────┐
│ head                                         │
│  ┌──────────┬──────────┬──────────┬────────┐ │
│  │ headroom │ L2 hdr   │ L3 hdr   │ L4 hdr │ │
│  │          │ (mac)    │ (net)    │ (trans)│ │
│  │          │ skb_push │          │        │ │
│  │          │▼         │▼         │▼       │ │
│  │          │mac_header│net_header│trans_hdr│ │
│  │          │          │          │         │ │
│  │          │          │          │ data    │ │
│  │          │          │          │ ▼       │ │
│  │          │          │          │ payload │ │
│  │          │          │          │         │ │
│  │          │          │          │ tail    │ │
│  │          │          │          │ ▼       │ │
│  └──────────┴──────────┴──────────┴────────┘ │
│ end                                           │
└───────────────────────────────────────────────┘
```

### 5.4 struct net_device

文件：`include/linux/netdevice.h:2109`

`struct net_device` 是所有网络设备的统一抽象，包含超过 200 个字段：

```c
struct net_device {
    // TX 读频繁热路径
    __cacheline_group_begin(net_device_read_tx);
    unsigned long       priv_flags:32;
    unsigned long       lltx:1;             // 低延迟 TX
    const struct net_device_ops *netdev_ops; // 设备操作函数表
    const struct header_ops *header_ops;     // 头部操作
    struct netdev_queue *_tx;                // 发送队列数组
    unsigned int        real_num_tx_queues;  // 实际 TX 队列数
    unsigned int        mtu;                 // 最大传输单元
    unsigned short      needed_headroom;    // 需要的前导空间
    __cacheline_group_end(net_device_read_tx);

    // TX/RX 读频繁热路径
    __cacheline_group_begin(net_device_read_txrx);
    unsigned long       state;              // 设备状态
    unsigned int        flags;              // 标志位 (IFF_UP, IFF_RUNNING 等)
    unsigned short      hard_header_len;    // 硬件头部长度
    netdev_features_t   features;           // 设备特性 (TSO, GSO, GRO 等)
    __cacheline_group_end(net_device_read_txrx);

    // RX 读频繁热路径
    __cacheline_group_begin(net_device_read_rx);
    struct bpf_prog __rcu *xdp_prog;        // XDP 程序
    struct list_head    ptype_specific;     // 特定协议处理器
    int                 ifindex;            // 接口索引
    unsigned int        real_num_rx_queues; // 实际 RX 队列数
    struct netdev_rx_queue *_rx;            // 接收队列数组
    unsigned int        gro_max_size;       // GRO 最大合并大小
    rx_handler_func_t __rcu *rx_handler;    // RX 处理器 (网桥/OVS 使用)
    __cacheline_group_end(net_device_read_rx);

    char        name[IFNAMSIZ];             // 设备名 (eth0, lo 等)
    unsigned long   mem_end, mem_start;     // 设备内存映射
    unsigned long   base_addr;              // 设备 I/O 基地址
    unsigned char   perm_addr[MAX_ADDR_LEN]; // 永久 MAC 地址
    unsigned char   addr_len;               // 地址长度
    unsigned short  type;                   // 硬件类型 (ARPHRD_ETHER 等)
    int             irq;                    // 中断号
    const struct ethtool_ops *ethtool_ops;  // ethtool 操作

    // 协议特定指针
    struct in_device __rcu *ip_ptr;         // IPv4 配置
    struct inet6_dev __rcu *ip6_ptr;        // IPv6 配置
};
```

### 5.5 struct net

文件：`include/net/net_namespace.h`

网络命名空间，实现网络资源的隔离（容器网络的基础）：
```c
struct net {
    struct user_namespace   *user_ns;       // 用户命名空间
    struct net_device       *loopback_dev;  // 回环设备
    struct list_head        dev_base_head;  // 设备链表
    struct hlist_head       *dev_index_head; // 设备索引哈希表
    struct sock             *nlsk;          // Netlink socket
    unsigned int            proc_inum;      // proc 文件系统 inode 号
    
    struct netns_ipv4       ipv4;           // IPv4 命名空间数据
    struct netns_ipv6       ipv6;           // IPv6 命名空间数据
    // ...
};
```

---

## 6. 数据包收发流程

### 6.1 发送路径概述

```
send() / write()
    │
    ▼
sys_sendto() [net/socket.c]
    │
    ▼
sock_sendmsg() → sock->ops->sendmsg()
    │
    ├── TCP: tcp_sendmsg() [net/ipv4/tcp.c]
    │       │
    │       ▼
    │   tcp_push() → tcp_write_xmit() → tcp_transmit_skb()
    │       │
    │       ▼
    │   ip_queue_xmit() [net/ipv4/ip_output.c]
    │
    └── UDP: udp_sendmsg() [net/ipv4/udp.c]
            │
            ▼
        ip_append_data() / ip_push_pending_frames()
            │
            ▼
        __ip_local_out() → dst_output()
            │
            ▼
    ip_local_out() → __ip_local_out()
            │
            ▼
    Netfilter HOOK: NF_INET_LOCAL_OUT / POST_ROUTING
            │
            ▼
    neigh_output() [net/core/neighbour.c]
            │
            ▼
    dev_queue_xmit() [net/core/dev.c]
            │
            ▼
    Qdisc (流量控制) → netdev_ops->ndo_start_xmit()
            │
            ▼
    [驱动层] e1000_xmit_frame() → DMA 发送
```

### 6.2 接收路径概述

```
[硬件] 网卡接收数据包
    │
    ▼
DMA 写入 Ring Buffer → 触发 IRQ
    │
    ▼
e1000_intr() / e1000_msix_ring() [驱动中断处理]
    │
    ▼
napi_schedule() → NET_RX_SOFTIRQ 软中断
    │
    ▼
e1000_clean() [NAPI poll 回调]
    │
    ▼
napi_gro_receive() → dev_gro_receive() [GRO 合并]
    │
    ▼
netif_receive_skb() [net/core/dev.c]
    │
    ├── XDP 处理 (若有)
    ├── tc ingress hook
    ├── rx_handler (网桥)
    │
    ▼
__netif_receive_skb_core()
    │
    ├── ptype_all → 原始套接字 (AF_PACKET)
    │
    ├── ptype_specific[ETH_P_IP] → ip_rcv()
    │       │
    │       ▼
    │   ip_rcv_finish() [net/ipv4/ip_input.c]
    │       │
    │       ▼
    │   Netfilter HOOK: NF_INET_PRE_ROUTING
    │       │
    │       ▼
    │   ip_route_input_noref() → dst_input()
    │       │
    │       ▼
    │   NF_INET_LOCAL_IN → tcp_v4_rcv() / udp_rcv()
    │       │
    │       ├── TCP: tcp_v4_rcv()
    │       │       │
    │       │       ▼
    │       │   tcp_v4_do_rcv() → tcp_rcv_established()
    │       │       │
    │       │       ▼
    │       │   sk->sk_data_ready() → sock_def_readable()
    │       │       │
    │       │       ▼
    │       │   wake_up_interruptible() → 用户态 recv() 返回
    │       │
    │       └── UDP: udp_rcv() → udp_unicast_rcv_skb()
    │               │
    │               ▼
    │           sk->sk_data_ready()
    │
    └── ptype_specific[ETH_P_ARP] → arp_rcv()
```

---

## 7. 网络设备驱动模型

### 7.1 NAPI 机制

NAPI (New API) 是 Linux 网络驱动的中断/轮询混合接收模型：

```
中断触发
    │
    ▼
napi_schedule() → 将 napi_struct 加入 CPU 的 poll_list
    │
    ▼
NET_RX_SOFTIRQ 软中断 → net_rx_action()
    │
    ▼
遍历 poll_list → 调用 napi->poll()
    │
    ▼
驱动 poll 回调 (如 e1000_clean())
    │
    ├── while (work_done < budget) 处理包
    ├── 处理完成后调用 napi_complete_done()
    └── 重新开启中断
```

**NAPI 优点**：
- 高负载下减少中断次数，避免中断风暴
- 通过轮询批量处理包，提高吞吐量
- 支持 GRO 合并，减少协议栈处理开销

### 7.2 网络设备注册

```
e1000_probe() [PCI 探测]
    │
    ├── alloc_etherdev() → 分配 net_device
    ├── 设置 netdev_ops (e1000_netdev_ops)
    ├── 设置 ethtool_ops
    ├── e1000_sw_init() → 软件初始化
    ├── e1000_setup_all_tx_resources() → TX 环初始化
    └── register_netdev() → 注册到内核网络子系统
            │
            ▼
        register_netdevice()
            │
            ├── dev->ifindex 分配
            ├── dev->dev_addr 设置
            ├── netdev_register_kobject() → sysfs 接口
            └── netif_carrier_off() → 初始链路状态
```

---

## 8. 配置与编译

当前内核配置中网络相关选项：

```
# 核心网络
CONFIG_NET=y
CONFIG_INET=y
CONFIG_IP_MULTICAST=y
CONFIG_IP_ADVANCED_ROUTER=y

# TCP/IP
CONFIG_TCP_CONG_ADVANCED=y
CONFIG_TCP_CONG_CUBIC=y
CONFIG_TCP_CONG_BBR=y
CONFIG_DEFAULT_CUBIC=y
CONFIG_TCP_MD5SIG=y
CONFIG_IPV6=y

# Intel 网卡驱动
CONFIG_E1000=y
CONFIG_E1000E=y
CONFIG_IGB=y
CONFIG_IGBVF=y

# 网络功能
CONFIG_NET_SCHED=y
CONFIG_NET_SCH_FQ=y
CONFIG_NET_SCH_FQ_CODEL=y
CONFIG_NET_CLS=y
CONFIG_NET_CLS_CGROUP=y
CONFIG_NETPRIO_CGROUP=y
CONFIG_BPF=y
CONFIG_NETFILTER=y
```

---

## 附：网络子系统核心文件清单

```
net/socket.c           # VFS socket 层 (系统调用入口)
net/core/sock.c        # 通用 socket 支持
net/core/dev.c         # 设备管理核心
net/core/skbuff.c      # sk_buff 管理
net/core/neighbour.c   # 邻居协议 (ARP/NDISC)
net/core/filter.c      # BPF 过滤
net/core/gro.c         # 通用接收卸载
net/core/gso.c         # 通用分段卸载
net/core/rtnetlink.c   # RTNetlink 配置
net/ethernet/eth.c     # 以太网帧处理
net/ipv4/af_inet.c     # PF_INET 协议族
net/ipv4/tcp.c         # TCP 协议
net/ipv4/tcp_input.c   # TCP 输入
net/ipv4/tcp_output.c  # TCP 输出
net/ipv4/udp.c         # UDP 协议
net/ipv4/ip_input.c    # IP 输入
net/ipv4/ip_output.c   # IP 输出
net/ipv4/route.c       # IP 路由
net/ipv4/arp.c         # ARP 协议
net/ipv4/icmp.c        # ICMP 协议
net/ipv4/fib_trie.c    # FIB 路由表
```