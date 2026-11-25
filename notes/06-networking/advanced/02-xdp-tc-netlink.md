# XDP / tc / netlink

## 1. 概述

本文覆盖三条与数据面可编程和配置下发相关的主线：

1. **XDP**：在驱动 NAPI 收包最早期执行 eBPF 程序（`BPF_PROG_TYPE_XDP`），
   可在 skb 分配前完成丢弃、转发、重定向；
2. **tc/qdisc**：流量控制与分类（`TC`），包括 ingress/clsact、classifier（`cls_*`）、
   action（`act_*`）与 qdisc 排队规则；
3. **netlink/rtnetlink**：用户态通过这些 netlink 家族创建链接、qdisc、filter，
   并在 netns 之间隔离配置。

源码位置：

```
net/core/dev.c            XDP 挂载/执行（dev_xdp_attach、netif_receive_generic_xdp、sch_handle_ingress）
net/core/filter.c         eBPF 辅助函数、XDP/TC 相关的 verifier 辅助
kernel/bpf/               BPF 核心：syscall.c（bpf_prog_load）、verifier、devmap/cpumap/xdp redirect
net/xdp/                  AF_XDP（xsk.c、xdp_umem.c、xsk_buff_pool.c）
net/sched/                qdisc、classifier、action（sch_*.c、cls_*.c、act_*.c）
net/netlink/              netlink 协议族与 socket
net/core/rtnetlink.c      rtnetlink 消息处理（RTM_NEWLINK / RTM_*QDISC / RTM_*TFILTER 分发）
include/net/xdp.h         struct xdp_buff、bpf_prog_run_xdp
include/net/sch_generic.h Qdisc/tcf_proto 等 tc 核心结构
include/uapi/linux/bpf.h  BPF 指令、程序类型、XDP 返回值
include/uapi/linux/if_link.h  XDP_FLAGS_*、IFLA_XDP_*
include/uapi/linux/pkt_sched.h / pkt_cls.h  tc netlink 属性
```

当前 `.config`（**源码存在 ≠ 已启用**）：

```
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_BPF_JIT_DEFAULT_ON=y
# CONFIG_BPF_JIT_ALWAYS_ON is not set
CONFIG_BPF_UNPRIV_DEFAULT_OFF=y
CONFIG_NET_NS=y
CONFIG_NET_SCHED=y
CONFIG_NET_SCH_INGRESS=y
CONFIG_NET_CLS_ACT=y
CONFIG_NET_CLS_FLOWER=y
CONFIG_NET_ACT_GACT=y

# CONFIG_XDP_SOCKETS is not set
# CONFIG_NET_CLS_BPF is not set
# CONFIG_NET_ACT_BPF is not set
# CONFIG_NET_CLS_U32 / MATCHALL / FW is not set
# CONFIG_NET_SCH_FQ / FQ_CODEL / HTB is not set
# CONFIG_IFB is not set
# CONFIG_BPF_STREAM_PARSER is not set
```

即：BPF 子系统、XDP 驱动钩子、tc 框架（ingress + flower + gact）可用；
AF_XDP、tc 侧的 BPF 分类器/动作不可用。

## 2. XDP 挂载点与模式

### 2.1 三种模式

`enum bpf_xdp_mode`（`include/linux/netdevice.h`）：

```c
enum bpf_xdp_mode {
    XDP_MODE_SKB = 0,   /* generic XDP：在 skb 已分配后执行 */
    XDP_MODE_DRV = 1,   /* native XDP：驱动 NAPI 轮询中执行 */
    XDP_MODE_HW  = 2,   /* offload：由网卡固件执行 */
    __MAX_XDP_MODE
};
```

用户态 flags（`include/uapi/linux/if_link.h`）：`XDP_FLAGS_UPDATE_IF_NOEXIST`(bit0)、
`XDP_FLAGS_SKB_MODE`(bit1)、`XDP_FLAGS_DRV_MODE`(bit2)、`XDP_FLAGS_HW_MODE`(bit3)，
`XDP_FLAGS_MODES` 为三者掩码。

### 2.2 挂载流程

```
bpf(BPF_LINK_CREATE, BPF_XDP) 或 netlink(RTM_SETLINK + IFLA_XDP)
  └─ bpf_xdp_link_attach()                     net/core/dev.c
      └─ dev_xdp_attach_link() → dev_xdp_attach(dev, extack, link, NULL, NULL, flags)
          ├─ 校验 mode 与 dev 支持（netif_is_bareudp 等）
          ├─ dev_xdp_bpf_op(dev, mode) 取 ndo_bpf / ndo_bpf 兼容 ops
          └─ dev_xdp_install(dev, mode, bpf_op, extack, flags, new_prog)
              ├─ xdp.command = XDP_SETUP_PROG 或 XDP_SETUP_PROG_HW
              ├─ xdp.prog = prog，bpf_prog_inc(prog)
              └─ bpf_op(dev, &xdp) → 驱动的 ndo_bpf()
```

`dev_xdp_install()`（`net/core/dev.c`）在非 HW 模式下还会调用
`bpf_prog_change_xdp()` 更新静态键，供数据面快速判断是否需要执行 XDP。
`dev_xdp_uninstall()` 在设备注销时遍历 `XDP_MODE_SKB..__MAX_XDP_MODE` 卸载并
自动解绑 `bpf_xdp_link`。

网卡驱动通常用 `netdev_bpf` 的 `XDP_SETUP_PROG`、`XDP_QUERY_PROG`、
`XDP_SETUP_XSK_POOL` 等命令实现；驱动需保证 `xdp_buff` 的
`data_hard_start`/`frame_sz` 与页模型一致。

### 2.3 挂载位置

- **native**：驱动在 `napi_poll()` 中构造 `xdp_buff` 后直接调用 `bpf_prog_run_xdp()`，此时还没有 skb；
- **generic**：`__netif_receive_skb_core()` 在协议栈入口看到 `dev->xdp_prog`（SKB 模式）时调用 `do_xdp_generic()`；
- **offload**：驱动把程序下发到硬件，包在网卡内部处理。

## 3. bpf_prog 与 XDP 执行

### 3.1 xdp_buff

`include/net/xdp.h`：

```c
struct xdp_buff {
    void *data;              /* 当前包起始（可被 bpf_xdp_adjust_head 移动） */
    void *data_end;
    void *data_meta;         /* 元数据区尾部（bpf_xdp_adjust_meta） */
    void *data_hard_start;   /* 页/帧起点 */
    struct xdp_rxq_info *rxq;
    struct xdp_txq_info *txq;
    union {
        struct { u32 frame_sz; u32 flags; };
        u64 frame_sz_flags_init;   /* 小端下一次性初始化 */
    };
};
```

`flags` 由 `XDP_FLAGS_HAS_FRAGS`、`XDP_FLAGS_FRAGS_PF_MEMALLOC`、
`XDP_FLAGS_FRAGS_UNREADABLE` 组成，配合 `xdp_buff_has_frags()` 处理
jumbo frame 与多缓冲区场景。

### 3.2 执行

```c
static __always_inline u32 bpf_prog_run_xdp(const struct bpf_prog *prog,
                                            struct xdp_buff *xdp)
{
    u32 act = __bpf_prog_run(prog, xdp, BPF_DISPATCHER_FUNC(xdp));

    if (static_branch_unlikely(&bpf_master_redirect_enabled_key)) {
        if (act == XDP_TX && netif_is_bond_slave(xdp->rxq->dev))
            act = xdp_master_redirect(xdp);
    }
    return act;
}
```

`__bpf_prog_run()` 通过 BPF dispatcher（`BPF_DISPATCHER_FUNC`）间接跳转，
由 static call 机制选择解释器/JIT 入口；XDP 在 NAPI 的 `local_bh_disable()`
上下文中运行，因此 map 访问依赖的 RCU 保护天然成立。

### 3.3 generic XDP 的特殊处理

`bpf_prog_run_generic_xdp()`（`net/core/dev.c`）把 skb 线性化后填充 `xdp_buff`：

- `mac_len = skb->data - skb_mac_header(skb)`，让 XDP 从 MAC 头开始；
- 若 skb 非线性，设置 frags 标志并把 `skb_shinfo(skb)->xdp_frags_size` 交给 XDP；
- 执行后比较 `xdp->data`/`xdp->data_end` 与原始值，反向同步到 skb
  （`__skb_pull`/`__skb_push`/`skb_set_tail_pointer`），并修正
  `skb->mac_header`、`skb->data_len`。

`do_xdp_generic()` 根据返回值处理：`XDP_TX` → `generic_xdp_tx()` 回环发送，
`XDP_REDIRECT` → `xdp_do_redirect()` 走 map 重定向。

## 4. XDP 动作与重定向

`include/uapi/linux/bpf.h`：

```c
enum xdp_action {
    XDP_ABORTED = 0,
    XDP_DROP,      /* 丢弃 */
    XDP_PASS,      /* 交给协议栈 */
    XDP_TX,        /* 从同一网卡发回 */
    XDP_REDIRECT,  /* 通过 map 重定向 */
};
```

重定向实现：

- `bpf_redirect_map(map, index, flags)` 把目标写入 per-cpu
  `struct bpf_redirect_info`，返回 `XDP_REDIRECT`；
- map 类型决定落点：`BPF_MAP_TYPE_DEVMAP`/`DEVMAP_HASH`（`kernel/bpf/devmap.c`）、
  `BPF_MAP_TYPE_CPUMAP`（`kernel/bpf/cpumap.c`）、
  `BPF_MAP_TYPE_XSKMAP`（AF_XDP，当前未编译）；
- `xdp_do_redirect()` → `dev_map_enqueue()`/`cpu_map_enqueue()`，
  批量场景 `xdp_do_flush()` 收尾；
- 若目标是 bond slave，`bpf_prog_run_xdp()` 会调用 `xdp_master_redirect()`
  把 `XDP_TX` 改投到 master。

XDP 程序可见的辅助函数由 `net/core/filter.c` 的 `xdp_func_proto()` 白名单给出，
例如 `bpf_xdp_adjust_head`、`bpf_xdp_adjust_tail`、`bpf_xdp_adjust_meta`、
`bpf_xdp_store_bytes`、`bpf_redirect_map`、`bpf_fib_lookup` 等。

## 5. AF_XDP（源码存在，当前未启用）

`CONFIG_XDP_SOCKETS` 未设置，`net/xdp/` 不编译，`AF_XDP` 地址族不存在。

源码结构：

```c
/* include/net/xdp_sock.h */
struct xdp_umem {
    void *addrs;
    u32 chunk_size;
    u32 chunks;
    u32 npgs;
    struct user_struct *user;
    refcount_t users;
    u8 flags;                  /* XDP_UMEM_UNALIGNED_CHUNK_FLAG */
    bool zc;                   /* zero-copy */
    struct page **pgs;
    u32 nr_pgs;
    ...
};

struct xdp_sock {
    struct sock sk;
    struct xsk_queue *rx, *tx, *fr, *cr;   /* RX/TX/FILL/COMPLETION ring */
    struct xdp_umem *umem;
    struct xsk_buff_pool *pool;
    u16 queue_id;
    bool zc;
    ...
};
```

用户态流程：`socket(AF_XDP, SOCK_RAW, 0)` → `setsockopt(SOL_XDP, XDP_UMEM_REG)` 注册
用户页 → `XDP_RX_RING`/`XDP_TX_RING`/`XDP_UMEM_FILL_RING`/`XDP_UMEM_COMPLETION_RING`
建立四个环 → `bind()` 到 {ifindex, queue_id} → `recvmsg()`/`sendmsg()` 收发。

`xsk_bind()`（`net/xdp/xsk.c`）把 socket 与 `xsk_buff_pool` 绑定，
驱动通过 `XDP_SETUP_XSK_POOL` 建立 zero-copy 通道；`xsk_sendmsg()`/`xsk_recvmsg()`
在用户态 ring 与内核 pool 之间搬运描述符。

## 6. tc/qdisc 框架

### 6.1 Qdisc

`include/net/sch_generic.h`：

```c
struct Qdisc {
    int (*enqueue)(struct sk_buff *skb, struct Qdisc *sch,
                   struct sk_buff **to_free);
    struct sk_buff *(*dequeue)(struct Qdisc *sch);
    unsigned int flags;            /* TCQ_F_* */
    u32 limit;
    const struct Qdisc_ops *ops;
    u32 handle;
    u32 parent;
    struct netdev_queue *dev_queue;
    refcount_t refcnt;
    ...
};

struct Qdisc_ops {
    struct Qdisc_ops *next;
    const struct Qdisc_class_ops *cl_ops;
    char id[IFNAMSIZ];
    int priv_size;
    unsigned int static_flags;
    int (*enqueue)(struct sk_buff *, struct Qdisc *, struct sk_buff **);
    struct sk_buff *(*dequeue)(struct Qdisc *);
    struct sk_buff *(*peek)(struct Qdisc *);
    int (*init)(struct Qdisc *, struct nlattr *, struct netlink_ext_ack *);
    void (*destroy)(struct Qdisc *);
    int (*dump)(struct Qdisc *, struct sk_buff *);
    ...
};
```

`TCQ_F_INGRESS`、`TCQ_F_CAN_BYPASS`、`TCQ_F_NOLOCK`、`TCQ_F_OFFLOADED` 等标志
影响排队与锁策略；`qdisc_priv(sch)` 返回紧随 `Qdisc` 的私有区。

发送路径：`__dev_queue_xmit()`（`net/core/dev.c`）→
`dev_qdisc_enqueue()` → `qdisc->enqueue()`；随后 `__qdisc_run()` 反复
`dequeue()` 直到空或配额耗尽，必要时 `netif_schedule_queue()` 触发软中断。

### 6.2 分类器框架

```c
struct tcf_proto {
    struct tcf_proto __rcu *next;
    void __rcu *root;
    int (*classify)(struct sk_buff *, const struct tcf_proto *,
                    struct tcf_result *);
    __be16 protocol;
    u32 prio;
    void *data;
    const struct tcf_proto_ops *ops;
    struct tcf_chain *chain;
    spinlock_t lock;
    refcount_t refcnt;
    struct rcu_head rcu;
};
```

`struct tcf_proto_ops` 以 `kind`（"flower"、"u32"、"bpf"、"matchall" 等）注册，
提供 `classify/init/destroy/get/put/change/delete/walk/reoffload`。
`tcf_classify()`（`net/sched/cls_api.c`）按 `prio` 遍历链上的 `tcf_proto`，
命中后返回 `TC_ACT_*`。

`tcf_block`/`tcf_chain` 是过滤器容器：一个 qdisc 可挂 block，
block 内多条 chain（按 `TC_H_MIN` 区分），每条 chain 是一串 `tcf_proto`。

### 6.3 ingress / clsact

`net/sched/sch_ingress.c` 提供两个特殊 qdisc：

- `ingress_qdisc_ops`：`id = "ingress"`，`TCQ_F_INGRESS | TCQ_F_CPUSTATS`，
  `ingress_init()` 调 `tcf_block_get_ext()` 与 `mini_qdisc_pair_block_init()`；
- `clsact_qdisc_ops`：`id = "clsact"`，同时支持 ingress 与 egress 两个 block，
  `clsact_find()`/`clsact_tcf_block()` 对应 `TC_H_MIN_INGRESS`/`TC_H_MIN_EGRESS`。

在 7.2 中 ingress 路径还叠加了 **tcx**（tc express）机制：
`ingress_init()` 会 `tcx_miniq_inc(dev->tcx_ingress)`，
数据面 `sch_handle_ingress()`（`net/core/dev.c`）先调 `tcx_run()` 执行 tcx
（`bpf_mprog_entry`，用于高效挂载 eBPF），再走传统 `tcf_classify()`。
`ingress_destroy()` 相应做 `tcx_miniq_dec()`。

`sch_handle_ingress()` 在 `__netif_receive_skb_core()` 中被调用，
返回值 `TC_ACT_SHOT` 直接丢包，`TC_ACT_REDIRECT` 交给 `sch_handle_egress()`
或 redirect 逻辑。

## 7. cls_bpf 与 tc BPF（源码存在，当前未启用）

`CONFIG_NET_CLS_BPF`、`CONFIG_NET_ACT_BPF` 均未设置，`net/sched/cls_bpf.c`
与 `act_bpf.c` 不编译。

```c
/* net/sched/cls_bpf.c */
struct cls_bpf_prog {
    struct bpf_prog *filter;
    struct list_head link;
    struct tcf_result res;
    struct tcf_exts exts;
    u32 handle;
    u16 bpf_num_ops;
    struct sock_filter *bpf_ops;
    const char *bpf_name;
    struct tcf_proto *tp;
    struct rcu_work rwork;
};
```

`cls_bpf_classify()` 遍历 `cls_bpf_head->plist`，对每个 `cls_bpf_prog`
调用 `bpf_prog_run()`（`BPF_PROG_TYPE_SCHED_CLS`），把返回值经
`cls_bpf_exec_opcode()` 规范到 `TC_ACT_OK/SHOT/STOLEN/TRAP/REDIRECT/UNSPEC`；
`cls_bpf_change()` 解析 `TCA_BPF_FD`、`TCA_BPF_OPS`、`TCA_BPF_FLAGS`、
`TCA_BPF_NAME`，并处理 offload（`reoffload`/`hw_add`/`hw_del`）。
`cls_bpf_ops` 的 `kind` 为 `"bpf"`。

tc BPF 与 XDP BPF 的区别：前者拿到的上下文是 `struct __sk_buff`（含
`tc_classid`、`tc_index`、`vlan_*`、`tstamp` 等），由
`skb_func_proto()` 提供辅助函数；后者是 `struct xdp_md`。

## 8. tc action

`net/sched/act_api.c` 是 action 框架：`struct tc_action_ops` 以 `kind` 注册
（"gact"、"mirred"、"pedit"、"vlan"、"csum" 等），提供
`init/cleanup/lookup/act/dump/walk`。

`CONFIG_NET_ACT_GACT=y`：`net/sched/act_gact.c` 的 `tcf_gact()` 根据
`TC_ACT_SHOT`/`TC_ACT_OK` 等判决并结合概率（`parm.rand`）丢包或放行，
常用于限速/采样。

`CONFIG_NET_CLS_FLOWER=y`：`net/sched/cls_flower.c` 以
`TCA_FLOWER_KEY_*` 描述五元组匹配，支持 `TCA_FLOWER_FLAGS`（skip_sw/skip_hw）
与 `reoffload()` 下发硬件。`CONFIG_NET_ACT_BPF=n` 意味着无法用 BPF 程序作为
tc action。

## 9. netlink 与 rtnetlink

### 9.1 netlink 核心

```
socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)
  → netlink_create()                        net/netlink/af_netlink.c
sendmsg()
  → netlink_sendmsg()
      ├─ netlink_unicast() → netlink_unicast_kernel()
      │     └─ sk->sk_data_ready()（内核端 netlink_kernel_create 注册的输入）
      └─ 或 netlink_broadcast()
```

`struct netlink_kernel_cfg`（`include/linux/netlink.h`）指定 `input`、
`cb_mutex`、`bind` 等；`struct netlink_sock` 保存 `portid`、`groups`、
`rx_skb_hook` 与 `netlink_table`。

`rtnetlink` 是 `NETLINK_ROUTE` 家族，`rtnetlink_rcv_msg()`（`net/core/rtnetlink.c`）
按 `nlmsghdr->nlmsg_type` 查 `rtnl_msg_handlers` 表（`rtnl_register_many()` 注册）
分发：

| 消息 | 处理函数 |
| --- | --- |
| `RTM_NEWLINK` | `rtnl_newlink()` → `rtnl_newlink_create()` |
| `RTM_SETLINK` | `rtnl_setlink()` |
| `RTM_DELLINK` | `rtnl_dellink()` |
| `RTM_NEWADDR` / `RTM_NEWROUTE` | `inet_rtm_newaddr()` / `inet_rtm_newroute()` |
| `RTM_NEWQDISC` / `RTM_NEWTFILTER` | 由 qdisc/class 模块注册的处理函数 |

`rtnl_register_internal()` 以 `(protocol, msgtype, doit, dumpit)` 建表；
`rtnl_link_ops` 由具体链路类型（veth、bridge、vlan…）通过
`rtnl_link_register()` 注册，`rtnl_newlink()` 按 `IFLA_INFO_KIND` 选择。

### 9.2 配置 XDP 与 qdisc 的 netlink 属性

- XDP：`RTM_SETLINK` + `IFLA_XDP` 嵌套，内含 `IFLA_XDP_FD`、`IFLA_XDP_FLAGS`、`IFLA_XDP_EXPECTED_FD`；也可用 `bpf(BPF_LINK_CREATE, ...)` 得到 link fd；
- qdisc：`RTM_NEWQDISC`，顶层 `struct tcmsg`，属性 `TCA_KIND`（"ingress"/"clsact"）、`TCA_OPTIONS`、`TCA_PARENT`；
- filter：`RTM_NEWTFILTER`，`TCA_KIND`（"flower"/"bpf"）、`TCA_CHAIN`、`TCA_OPTIONS`、`TCA_ACT_*`。

`genetlink`（`include/net/genetlink.h` 的 `genl_family`/`genl_ops`）用于非
rtnetlink 子系统（devlink、ethtool netlink、nf_tables 等）。

## 10. netns 交互

- `struct net`（`include/net/net_namespace.h`）持有路由表、设备列表、`netns_nf`、bpf 程序数组（`net->bpf.run_array[NETNS_BPF_*]`）等；
- `dev_net(dev)` 取设备所属 netns，`dev_net_set()` 用于迁移；
- `struct pernet_operations` 的 `init`/`exit`/`pre_exit` 在 netns 创建/销毁时
  被调用，qdisc/rtnetlink/fib 规则都按 netns 独立维护；
- `netlink` socket 绑定到创建它的 netns，除非指定 `NETLINK_LISTEN_ALL_NSID`
  并设置 `nl_groups` 与 `NETLINK_ADD_MEMBERSHIP`；
- 网络设备可跨 netns 移动（`RTM_SETLINK` + `IFLA_NET_NS_FD`），
  移动时 qdisc 与 XDP 程序的处理遵循 `dev_change_net_namespace()` 逻辑。

`CONFIG_NET_NS=y`，上述隔离能力均可用。

## 11. 端到端链路

```
XDP native:
  驱动 napi_poll → 构造 xdp_buff → bpf_prog_run_xdp
    → XDP_PASS    : 构造 skb → napi_gro_receive → 协议栈
    → XDP_DROP    : page 归还 → 结束
    → XDP_TX      : 驱动 xmit 路径直接发回
    → XDP_REDIRECT: bpf_redirect_map → xdp_do_redirect → dev_map_enqueue → xdp_do_flush

XDP generic:
  __netif_receive_skb_core → do_xdp_generic → bpf_prog_run_generic_xdp → bpf_prog_run_xdp
    → 同步 data/data_end 回 skb → 继续协议栈 / 丢弃 / generic_xdp_tx

tc ingress:
  驱动 → netif_receive_skb → __netif_receive_skb_core
    → sch_handle_ingress → tcx_run（tcx, eBPF mprog）
    → tcf_classify → cls_flower/cls_u32 等
    → 命中 action（gact/mirred…）→ TC_ACT_SHOT/REDIRECT/OK

配置下发:
  ip link set dev eth0 xdp obj prog.o
    → netlink(RTM_SETLINK + IFLA_XDP) → rtnl_setlink → do_setlink
    → dev_xdp_attach → dev_xdp_install → ndo_bpf
  tc qdisc add dev eth0 clsact
    → netlink(RTM_NEWQDISC) → tc_modify_qdisc → qdisc_create
  tc filter add dev eth0 ingress bpf da obj prog.o
    → netlink(RTM_NEWTFILTER) → tc_new_tfilter → tcf_proto_create("bpf")
```

## 12. 当前配置下的能力边界

| 功能 | 源码 | 当前配置 |
| --- | --- | --- |
| BPF syscall / JIT / verifier | `kernel/bpf/` | 启用 |
| XDP native / generic / offload 框架 | `net/core/dev.c`、`include/net/xdp.h` | 启用 |
| XDP redirect（devmap/cpumap） | `kernel/bpf/devmap.c`、`cpumap.c` | 启用 |
| AF_XDP | `net/xdp/` | 未编译（`XDP_SOCKETS=n`） |
| tc 框架 + ingress/clsact | `net/sched/sch_*.c` | 启用 |
| cls_flower / act_gact | `cls_flower.c` / `act_gact.c` | 启用 |
| cls_bpf / act_bpf | `cls_bpf.c` / `act_bpf.c` | 未编译 |
| cls_u32 / cls_matchall / ifb | 对应源文件 | 未编译 |
| netlink + rtnetlink + netns | `net/netlink/`、`net/core/rtnetlink.c` | 启用 |

结论：XDP 与 tc ingress/clsact 可用，可挂载 eBPF XDP 程序并用 flower/gact
做分类与动作；不能用 AF_XDP 零拷贝、不能用 tc BPF 分类器/动作。

## 13. 关键文件清单

| 路径 | 作用 |
| --- | --- |
| `include/uapi/linux/bpf.h` | BPF 指令、`enum bpf_prog_type`、`enum xdp_action` |
| `include/uapi/linux/if_link.h` | `XDP_FLAGS_*`、`IFLA_XDP_*` |
| `include/uapi/linux/pkt_sched.h` / `pkt_cls.h` | `tcmsg`、qdisc 属性与 `TCA_*`/`TC_ACT_*` |
| `include/net/xdp.h` | `struct xdp_buff`、`bpf_prog_run_xdp` |
| `include/net/xdp_sock.h` | `struct xdp_sock`、`struct xdp_umem`（AF_XDP，未编译） |
| `include/linux/netdevice.h` | `enum bpf_xdp_mode`、`struct netdev_bpf` |
| `include/net/sch_generic.h` | `Qdisc`、`Qdisc_ops`、`tcf_proto`、`tcf_proto_ops` |
| `include/net/netlink.h` / `include/linux/netlink.h` | netlink 消息与 `netlink_kernel_cfg` |
| `include/net/genetlink.h` | `genl_family`、`genl_ops` |
| `include/net/net_namespace.h` | `struct net` 与 pernet 框架 |
| `net/core/dev.c` | `dev_xdp_attach/install`、generic XDP、`sch_handle_ingress`、`__dev_queue_xmit` |
| `net/core/filter.c` | XDP/TC eBPF 辅助函数与 verifier 辅助 |
| `net/core/rtnetlink.c` | `rtnetlink_rcv_msg`、`rtnl_newlink/setlink/dellink` |
| `net/netlink/af_netlink.c` | netlink socket、`netlink_sendmsg/unicast` |
| `net/sched/sch_api.c` / `cls_api.c` | qdisc 创建、`tcf_classify`、`tc_new_tfilter` |
| `net/sched/sch_ingress.c` | ingress/clsact qdisc、tcx 配合 |
| `net/sched/cls_bpf.c` / `act_bpf.c` | tc BPF 分类器与 action（未编译） |
| `net/sched/cls_flower.c` / `act_gact.c` | flower 分类器与 gact 动作 |
| `net/xdp/xsk.c` / `xsk_buff_pool.c` | AF_XDP socket 与缓冲区池（未编译） |
| `kernel/bpf/syscall.c` | `bpf_prog_load`、`bpf_prog_attach`、`bpf_link_create` |
| `kernel/bpf/devmap.c` / `cpumap.c` | XDP redirect map 实现 |
| `kernel/bpf/verifier.c` | eBPF 校验器 |
