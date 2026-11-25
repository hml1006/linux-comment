# Netfilter

## 1. 概述

Netfilter 是 Linux 内核的包过滤/改包框架：在协议栈关键位置放置 hook 点，允许模块
（iptables、nftables、conntrack、NAT、ipset、flowtable 等）注册回调，对 skb 做检查、
修改、丢弃或排队给用户态。

```
net/netfilter/                   核心：hook 框架、conntrack、NAT、nftables、x_tables 通用层、ipset、flowtable
net/ipv4/netfilter/              IPv4：ip_tables、iptable_*、nf_defrag_ipv4
net/ipv6/netfilter/              IPv6 对应实现
net/bridge/netfilter/            桥接 netfilter（ebtables）
include/linux/netfilter.h        hook 框架内部接口
include/net/netfilter/*.h        conntrack/NAT/nftables/flowtable 内部结构
include/uapi/linux/netfilter*.h  用户态 ABI（sockopt 与 nfnetlink）
```

当前 `.config` 的关键状态（**源码存在 ≠ 已启用**）：

```
CONFIG_NETFILTER=y
CONFIG_NETFILTER_ADVANCED=y
CONFIG_NF_CONNTRACK=y
CONFIG_NF_CONNTRACK_EVENTS=y
CONFIG_NETFILTER_XTABLES=y
CONFIG_NETFILTER_XT_MATCH_CONNTRACK=y
CONFIG_NETFILTER_XT_MATCH_ADDRTYPE=y
CONFIG_NETFILTER_XT_TARGET_LOG=y
CONFIG_IP_NF_IPTABLES=y
CONFIG_IP6_NF_IPTABLES=y
CONFIG_NF_DEFRAG_IPV4=y
CONFIG_NF_DEFRAG_IPV6=y
CONFIG_BRIDGE=y
CONFIG_BRIDGE_NETFILTER=y
CONFIG_NETFILTER_FAMILY_BRIDGE=y

# CONFIG_NF_NAT is not set
# CONFIG_NF_TABLES is not set
# CONFIG_IP_SET is not set
# CONFIG_NF_CONNTRACK_BRIDGE is not set
# CONFIG_BRIDGE_NF_EBTABLES is not set
# CONFIG_NETFILTER_NETLINK_QUEUE / LOG / ACCT 均未启用
```

因此 hook 框架、conntrack、x_tables/iptables 过滤可用；NAT、nftables、ipset、
flowtable 源码完整存在，但当前不参与编译，对应功能不可用。

## 2. hook 框架

### 2.1 数据结构

`include/linux/netfilter.h`：

```c
struct nf_hook_state {
    u8 hook;                  /* NF_INET_* */
    u8 pf;                    /* NFPROTO_IPV4 等 */
    struct net_device *in, *out;
    struct sock *sk;
    struct net *net;
    int (*okfn)(struct net *, struct sock *, struct sk_buff *);
};

typedef unsigned int nf_hookfn(void *priv, struct sk_buff *skb,
                               const struct nf_hook_state *state);

struct nf_hook_ops {
    struct list_head list;
    struct rcu_head rcu;
    nf_hookfn *hook;
    struct net_device *dev;                  /* 仅 ingress 使用 */
    void *priv;
    u8 pf;
    enum nf_hook_ops_type hook_ops_type:8;   /* NORMAL / NF_TABLES / BPF / NFT_FT */
    unsigned int hooknum;
    int priority;                            /* 升序执行 */
};

struct nf_hook_entries {
    u16 num_hook_entries;
    struct nf_hook_entry hooks[];            /* 每项 { hook, priv } */
    /* trailer: orig_ops[] + nf_hook_entries_rcu_head */
};
```

hook 点（`include/uapi/linux/netfilter.h`）：`NF_INET_PRE_ROUTING`(0)、
`NF_INET_LOCAL_IN`(1)、`NF_INET_FORWARD`(2)、`NF_INET_LOCAL_OUT`(3)、
`NF_INET_POST_ROUTING`(4)、`NF_INET_INGRESS`(5，`NFPROTO_NETDEV`)。
判决值：`NF_DROP`(0)、`NF_ACCEPT`(1)、`NF_STOLEN`(2)、`NF_QUEUE`(3)、
`NF_REPEAT`(4)、`NF_STOP`(5，已废弃)；`NF_VERDICT_MASK` 之外的高位可携带
queue number（`NF_QUEUE_NR()`）或 errno（`NF_DROP_ERR()`）。

### 2.2 注册与调用

```
nf_register_net_hook(net, ops)                net/netfilter/core.c
  └─ __nf_register_net_hook(net, pf, reg)
      ├─ nf_hook_entries_grow(p, reg)         /* 复制旧项，按 priority 插入排序 */
      └─ rcu_assign_pointer(*pp, new_hooks)   /* RCU 发布 */
```

`NFPROTO_INET` 的 hook 会同时展开注册到 IPv4 与 IPv6 数组。
hook 数组保存在 per-netns 的 `struct netns_nf`（`include/net/netns/netfilter.h`）：
`hooks_ipv4[NF_INET_NUMHOOKS]`、`hooks_ipv6[]`、`hooks_bridge[]`、`hooks_arp[]`。

包路径通过 `NF_HOOK()` 宏调用：

```c
ret = nf_hook_slow(skb, &state, hook_head, 0);
if (ret == 1) okfn(net, sk, skb);
```

`nf_hook_slow()`（`net/netfilter/core.c`）逐个执行 `entries->hooks[]`：

- `NF_ACCEPT` → 继续下一个；
- `NF_DROP` → `kfree_skb_reason(SKB_DROP_REASON_NETFILTER_DROP)`，返回负 errno；
- `NF_QUEUE` → `nf_queue(skb, state, s, verdict)` 交付用户态；
- `NF_STOLEN` → 返回错误码，调用者不再持有 skb；
- 全部通过返回 1，由调用者执行 `okfn`。

`nf_hook_slow_list()` 处理 GRO 后的 skb_list，逐包调用 `nf_hook_slow()`。

### 2.3 优先级

IPv4 优先级（`include/uapi/linux/netfilter_ipv4.h`）：

```
RAW_BEFORE_DEFRAG -450 | CONNTRACK_DEFRAG -400 | RAW -300 | CONNTRACK -200
MANGLE -150 | NAT_DST -100 | FILTER 0 | SECURITY 50 | NAT_SRC 100
CONNTRACK_HELPER 300 | CONNTRACK_CONFIRM INT_MAX
```

`xt_hook_ops_alloc()` 按表的 `valid_hooks` 生成 `nf_hook_ops[]`，
`ipt_do_table()` 作为回调。

## 3. conntrack

### 3.1 数据结构

`include/net/netfilter/nf_conntrack_tuple.h`：

```c
struct nf_conntrack_tuple {
    struct nf_conntrack_man src;         /* 源地址/端口，可被 NAT 改写 */
    struct {
        union nf_inet_addr u3;           /* 目的地址 */
        union {
            __be16 all;
            struct { __be16 port; } tcp;      /* udp/dccp/sctp 同构 */
            struct { u_int8_t type, code; } icmp;
            struct { __be16 key; } gre;
        } u;
        u_int8_t protonum;
        struct { } __nfct_hash_offsetend;
        u_int8_t dir;
    } dst;
};

struct nf_conntrack_tuple_hash {
    struct hlist_nulls_node hnnode;
    struct nf_conntrack_tuple tuple;
};
```

`include/net/netfilter/nf_conntrack.h`：

```c
struct nf_conn {
    struct nf_conntrack ct_general;      /* 引用计数 */
    spinlock_t lock;
    u32 timeout;
    struct nf_conntrack_tuple_hash tuplehash[IP_CT_DIR_MAX]; /* original + reply */
    unsigned long status;                /* IPS_* 位 */
    possible_net_t ct_net;
    struct nf_conn *master;              /* expectation 的父连接 */
    struct nf_ct_ext *ext;               /* 扩展：helper、acct、labels… */
    union nf_conntrack_proto proto;      /* 协议私有状态 */
};
```

状态位包括 `IPS_EXPECTED`、`IPS_SEEN_REPLY`、`IPS_CONFIRMED`、`IPS_SRC_NAT`、
`IPS_DST_NAT`、`IPS_DYING`、`IPS_TEMPLATE`、`IPS_OFFLOAD`；`nf_ct_get(skb, &ctinfo)`
返回 `IP_CT_NEW`/`IP_CT_ESTABLISHED`/`IP_CT_RELATED`/`IP_CT_INVALID` 等。

### 3.2 创建与查找

```
nf_conntrack_in(skb, state)                  nf_conntrack_core.c
  ├─ nf_ct_get_tuple() 解析 original tuple
  ├─ resolve_normal_ct(tmpl, skb, ...)
  │    ├─ nf_conntrack_find_get() 查 nf_conntrack_hash
  │    └─ 未命中 → init_conntrack()
  │         ├─ nf_conntrack_alloc()（slab: nf_conntrack_cachep）
  │         ├─ l4proto->new()/packet() 协议校验与状态初始化
  │         ├─ nf_ct_ext_add()
  │         └─ nf_conntrack_hash_check_insert()
  └─ 返回 -NF_ACCEPT / -NF_DROP
```

`nf_conntrack_hash` 是 `hlist_nulls_head` 数组，索引来自
`hash_conntrack_raw()` 的 siphash 结果；桶数 `nf_conntrack_htable_size` 可按 netns 调整。

`nf_conntrack_confirm(skb)` 在 `LOCAL_IN`/`POST_ROUTING` 的
`NF_IP_PRI_CONNTRACK_CONFIRM` 处调用（`__nf_conntrack_confirm()`），置
`IPS_CONFIRMED` 并正式插入 hash；未 confirm 的连接超时后被回收。

### 3.3 协议跟踪与回收

`struct nf_conntrack_l4proto` 由各协议实现：

- `nf_conntrack_proto_tcp.c`：`tcp_conntracks[2][6][TCP_CONNTRACK_MAX]` 状态机，
  处理 SYN/ACK/FIN/RST、窗口缩放与序列号调整（`nf_conntrack_seqadj`）；
- `nf_conntrack_proto_udp.c` / `nf_conntrack_proto_icmp.c` / `nf_conntrack_proto_generic.c`；
- `nf_conntrack_proto.c` 维护 L3/L4 协议注册表，`nf_ct_netns_get()` 负责按需
  拉入 defrag 与协议模块。

helper（`nf_conntrack_helper.h`）用于 FTP/SIP/H.323 等多连接协议，配合
`nf_conntrack_expect.c` 的 expectation 工作；当前配置下这些 helper 均未启用。

`gc_worker()`（delayed work）周期扫描 hash，对超时的 `IPS_CONFIRMED` 连接
`nf_ct_delete()`（置 `IPS_DYING`，`call_rcu` 释放）。`NF_CONNTRACK_EVENTS` 已启用，
`nf_conntrack_ecache.c` 会向 netlink 订阅者广播 `IPCT_NEW/UPDATE/DESTROY`。

## 4. NAT（源码存在，当前未编译）

实现位于 `nf_nat_core.c`、`nf_nat_masquerade.c`、`nf_nat_helper.c` 与各协议
`nf_nat_proto_*`。核心结构（`include/net/netfilter/nf_nat.h`）：

```c
struct nf_conn_nat {
    union {
        struct hlist_node bysource;   /* 按源地址/端口挂到 nf_nat_bysource */
        struct nf_nat_range2 range;
    };
    u32 seq_scale;
};

struct nf_nat_range2 {
    unsigned int flags;               /* NF_NAT_RANGE_MAP_IPS / PROTO_SPECIFIED 等 */
    union nf_inet_addr min_addr, max_addr;
    union nf_conntrack_man_proto min_proto, max_proto;
    union nf_inet_addr base_proto;
};
```

流程：

1. `nf_nat_inet_fn()` 以 `NAT_DST`(-100) / `NAT_SRC`(100) 优先级注册 hook；
2. `nf_nat_setup_info(ct, range, manip)` 在连接上记录 NAT 绑定，设置
   `IPS_SRC_NAT`/`IPS_DST_NAT`，必要时在 `nf_nat_bysource` 上做冲突探测；
3. `nf_nat_packet(ct, ctinfo, hooknum, skb)` → `nf_nat_manip_pkt()` 改写 IP/端口并
   更新校验和；
4. `nf_nat_masquerade_ipv4()` 用出口设备地址构造 range 实现 SNAT masquerade；
5. 端口分配走 `nf_nat_l4proto` 回调与 `nf_nat_alloc_null_binding()`。

当前 `CONFIG_NF_NAT`/`CONFIG_IP_NF_NAT` 均未设置，`nf_nat_*` 不编译，
`iptable_nat`、`MASQUERADE`、`SNAT`/`DNAT` 不可用。

## 5. iptables / x_tables

### 5.1 表、链与规则

`include/linux/netfilter/x_tables.h`：

```c
struct xt_table {
    struct list_head list;
    unsigned int valid_hooks;          /* 关心的 NF_INET_* 位 */
    struct xt_table_info __rcu *private;
    struct module *me;
    u_int8_t af;                       /* NFPROTO_IPV4 */
    int priority;
    const char *name;                  /* "filter" / "mangle" / "raw" */
};

struct xt_table_info {
    unsigned int size, number, initial_entries;
    unsigned int hook_entry[NF_INET_NUMHOOKS];
    unsigned int underflow[NF_INET_NUMHOOKS];
    unsigned int stacksize;
    void ***jumpstack;                 /* per-cpu 跳转栈 */
    unsigned char entries[] __aligned(8);
};
```

规则 blob 中每条 `struct ipt_entry`（`include/uapi/linux/netfilter_ipv4/ip_tables.h`）：

```c
struct ipt_entry {
    struct ipt_ip ip;                  /* 源/目的 IP、掩码、接口、协议 */
    unsigned int nfcache;
    __u16 target_offset;               /* 到 target 的偏移 */
    __u16 next_offset;                 /* 到下一条规则的偏移 */
    unsigned int comefrom;
    struct xt_counters counters;
    unsigned char elems[];             /* 后接 match 与 target */
};
```

`struct xt_entry_match` / `xt_entry_target`（`netfilter/x_tables.h`）中
`u.user.name` 是扩展名，`u.kernel.match/target` 由 `translate_table()` 经
`xt_request_find_match()`/`xt_request_find_target()` 解析填入。

### 5.2 注册与替换

```
ipt_register_table(net, table, repl, ops)
  └─ translate_table(net, newinfo, entry0, ...)
      ├─ 校验 hook_entry / underflow / target_offset
      ├─ xt_check_entry_offsets()
      ├─ xt_check_match() / xt_check_target() 调扩展 checkentry
      └─ 填充 xt_table_info
```

用户态 `setsockopt(IPT_SO_SET_REPLACE)` → `do_replace()`：
`xt_alloc_table_info()` → `copy_from_user()` → `translate_table()` →
`__do_replace()` → `xt_replace_table()`（RCU 替换 `table->private`）。
`IPT_SO_GET_INFO`/`GET_ENTRIES`/`ADD_COUNTERS` 分别取元信息、规则 blob 与计数器。
`x_tables.c` 的 `xt_register_target()`/`xt_register_match()` 维护
`(name, revision, family)` 扩展链表。

### 5.3 数据面

`ipt_do_table(priv, skb, state)`（`net/ipv4/netfilter/ip_tables.c`）：

1. `local_bh_disable()` 并 `xt_write_recseq_begin()`；
2. 取 `private->entries` 与 `hook_entry[hook]` 得到起始 `ipt_entry`；
3. 每条规则：`ip_packet_match()` → `xt_ematch_foreach()` 依次调 match
   → 命中则 `ADD_COUNTER()` → 执行 `target()`；
4. target 返回 `XT_CONTINUE` 则下一条，否则携带判决返回；
5. 标准 target（`xt_standard_target`）的 `verdict` 为 `-NF_ACCEPT-1` 等负值，
   表示终止表内遍历并把判决交回 `nf_hook_slow`。

`iptable_filter.c` 定义 `packet_filter` 表，`iptable_filter_init()` 用
`xt_hook_ops_alloc(&packet_filter, ipt_do_table)` 生成 LOCAL_IN/FORWARD/LOCAL_OUT
三个 hook，优先级 `NF_IP_PRI_FILTER`。当前仅 filter 表及少量 match/target 启用。

## 6. nftables（源码存在，当前未启用）

`CONFIG_NF_TABLES` 未设置，`nf_tables_api.c`、`nf_tables_core.c`、`nft_*.c` 不编译。

```c
struct nft_chain {
    struct nft_rule_blob __rcu *blob_gen_0;   /* 双缓冲 + genmask 原子切换 */
    struct nft_rule_blob __rcu *blob_gen_1;
    struct list_head rules;
    struct nft_table *table;
    u64 handle;
    u32 use;
    u8 flags:5, bound:1, genmask:2;           /* NFT_CHAIN_BASE / HW_OFFLOAD */
    char *name;
};

struct nft_rule {
    struct list_head list;
    u64 handle:42, genmask:2, dlen:12, udata:1;
    unsigned char data[] __aligned(__alignof__(struct nft_expr));
};

struct nft_expr {
    const struct nft_expr_ops *ops;
    unsigned char data[] __aligned(__alignof__(u64));
};
```

规则是“表达式数组”：`nft_expr_first()`/`nft_expr_next()` 按 `ops->size` 遍历，
末尾通常是携带判决的 `immediate` 表达式。`struct nft_expr_ops` 提供
`eval/init/destroy/dump/validate/offload/gc`。

控制面走 nfnetlink：`nf_tables_newtable()`、`nf_tables_newchain()`、
`nf_tables_newrule()`、`nf_tables_newset()` 处理 `NFT_MSG_*`，commit 阶段切换
`genmask` 实现无锁读/原子替换；base chain 的 `nft_chain_type.hooks[]`
经 `nf_tables_register_hook()` 以 `NF_HOOK_OP_NF_TABLES` 类型注册。

数据面 `nft_do_chain(pkt, priv)`（`nf_tables_core.c`）：选 `blob_gen_0/1`
→ 遍历 `nft_rule_dp` → 逐个 `ops->eval()` → 处理
`NFT_CONTINUE`/`NFT_BREAK`/`NFT_JUMP`/`NFT_GOTO`/`NFT_RETURN` 与绝对判决。

## 7. ipset（源码存在，当前未启用）

`net/netfilter/ipset/` 提供 `hash:ip`、`hash:net`、`list:set`、`bitmap:*` 等集合，
作为 iptables `-m set` 的后端。

```c
struct ip_set {
    struct rcu_work rwork;
    char name[IPSET_MAXNAMELEN];
    spinlock_t lock;
    u32 ref;
    struct ip_set_type *type;                  /* "hash:ip" 等 */
    const struct ip_set_type_variant *variant; /* test/add/del 实现 */
    u8 family, revision, extensions, flags;
};

struct ip_set_type {
    struct list_head list;
    char name[IPSET_MAXNAMELEN];
    u8 protocol, dimension, family;
    u8 revision_min, revision_max;
    int (*create)(struct net *net, struct ip_set *set,
                  struct nlattr *tb[], u32 flags);
};
```

集合类型经 `ip_set_type_register()` 注册，用户态通过 `NFNL_SUBSYS_IPSET` 的
netlink 执行 create/add/del/test；数据面由 `ip_set_test()` 供 `xt_set` match 调用。
当前 `CONFIG_IP_SET` 未设置，`ipset` 工具与 `-m set` 不可用。

## 8. flowtable（源码存在，当前未启用）

`nf_flow_table_*.c` 实现软件快速转发：conntrack 确认后的流被卸载到 flowtable，
后续包绕过完整协议栈直接 `dev_queue_xmit()`。

```c
struct nf_flowtable {
    unsigned int flags;              /* NF_FLOWTABLE_HW_OFFLOAD / COUNTER */
    int priority;
    struct rhashtable rhashtable;    /* 流表，读多写少 */
    struct list_head list;
    const struct nf_flowtable_type *type;
    struct delayed_work gc_work;
    struct flow_block flow_block;
    struct rw_semaphore flow_block_lock;
    possible_net_t net;
};
```

`nf_flow_table_init()` 建立 rhashtable 与 GC work；
`nf_flow_offload_ip_hook()`（`nf_flow_table_ip.c`）处理
`FLOW_OFFLOAD_DIR_ORIGINAL`/`REPLY` 两个方向；`nf_flow_table_offload.c` 通过
`flow_block` 把流下发到硬件（tc offload）。flowtable 通常由 nftables 的
`flowtable` 对象创建并依赖 conntrack，因此当前 `NF_TABLES=n` 时整条链路不存在。

## 9. IPv4 收包路径上的 hook 顺序

```
网卡收包 → ip_rcv → NF_HOOK(PRE_ROUTING, ip_rcv_finish)
    RAW(-300) → CONNTRACK_DEFRAG(-400, nf_defrag_ipv4)
    → CONNTRACK(-200, nf_conntrack_in) → MANGLE(-150)
    → NAT_DST(-100, 当前未编译)
→ 路由决策
    本机: NF_HOOK(LOCAL_IN)  → FILTER(0, iptable_filter)
    转发: NF_HOOK(FORWARD)   → FILTER(0)
→ NF_HOOK(POST_ROUTING, ip_output)
    MANGLE(-150) → NAT_SRC(100, 未编译) → CONNTRACK_CONFIRM(INT_MAX)
本机发出 → ip_local_out → NF_HOOK(LOCAL_OUT)
    CONNTRACK(-200) → MANGLE(-150) → NAT_DST(-100) → FILTER(0)
```

## 10. 当前配置下的能力边界

| 功能 | 源码 | 当前配置 |
| --- | --- | --- |
| hook 框架 / `nf_hook_slow` | `net/netfilter/core.c` | 启用 |
| conntrack（TCP/UDP/ICMP） | `nf_conntrack_*.c` | 启用 |
| conntrack 事件 | `nf_conntrack_ecache.c` | 启用 |
| NAT | `nf_nat_core.c` | 未编译（`NF_NAT=n`） |
| iptables 核心 + filter 表 | `ip_tables.c` / `iptable_filter.c` | 启用 |
| iptables mangle/raw 表 | `iptable_mangle.c` / `iptable_raw.c` | 未编译 |
| nftables | `nf_tables_api.c` / `nf_tables_core.c` | 未编译 |
| ipset | `net/netfilter/ipset/` | 未编译 |
| flowtable | `nf_flow_table_*.c` | 未编译 |
| ebtables / bridge conntrack | `net/bridge/netfilter/` | 未编译 |

结论：当前内核能做“有状态过滤”（conntrack + iptables filter），不能做 NAT、
nftables、set 与 flowtable 加速。

## 11. 关键文件清单

| 路径 | 作用 |
| --- | --- |
| `include/linux/netfilter.h` | `nf_hook_ops`、`nf_hook_state`、`NF_HOOK`、`nf_hook_slow` |
| `include/net/netns/netfilter.h` | per-netns hook 数组 `netns_nf` |
| `include/uapi/linux/netfilter.h` | `NF_INET_*` hook 编号与判决值 |
| `include/uapi/linux/netfilter_ipv4.h` | IPv4 hook 优先级常量 |
| `include/net/netfilter/nf_conntrack.h` | `struct nf_conn`、`nf_ct_get/put` |
| `include/net/netfilter/nf_conntrack_tuple.h` | `nf_conntrack_tuple` 与 tuple hash |
| `include/net/netfilter/nf_conntrack_l4proto.h` | L4 协议回调表 |
| `include/net/netfilter/nf_nat.h` | `nf_conn_nat`、`nf_nat_range2` |
| `include/net/netfilter/nf_tables.h` | `nft_table/chain/rule/expr`、`nft_expr_ops` |
| `include/net/netfilter/nf_flow_table.h` | `nf_flowtable`、`nf_flow_route` |
| `include/linux/netfilter/x_tables.h` | `xt_table`、`xt_table_info`、`xt_match/target` |
| `include/linux/netfilter/ipset/ip_set.h` | `struct ip_set`、`struct ip_set_type` |
| `include/uapi/linux/netfilter_ipv4/ip_tables.h` | `ipt_entry`、`IPT_SO_SET_REPLACE` |
| `net/netfilter/core.c` | hook 注册/注销、`nf_hook_slow` |
| `net/netfilter/nf_conntrack_core.c` | 连接查找/创建/确认/GC |
| `net/netfilter/nf_conntrack_proto_tcp.c` | TCP 状态机 |
| `net/netfilter/nf_conntrack_expect.c` | expectation |
| `net/netfilter/nf_conntrack_ecache.c` | 事件缓存与 netlink 通知 |
| `net/netfilter/nf_nat_core.c` / `nf_nat_masquerade.c` | NAT 绑定、改包与 masquerade（未编译） |
| `net/netfilter/x_tables.c` | 扩展注册、`xt_replace_table`、blob 校验 |
| `net/netfilter/nf_tables_api.c` / `nf_tables_core.c` | nftables 控制面与 `nft_do_chain`（未编译） |
| `net/netfilter/nf_flow_table_core.c` / `nf_flow_table_ip.c` | flowtable 核心与 offload hook（未编译） |
| `net/netfilter/ipset/ip_set_core.c` | ipset 核心 |
| `net/ipv4/netfilter/ip_tables.c` | `ipt_do_table`、`do_replace`、`translate_table` |
| `net/ipv4/netfilter/iptable_filter.c` | filter 表与 hook 注册 |
| `net/ipv4/netfilter/nf_defrag_ipv4.c` | IPv4 分片重组 |
| `net/netfilter/nf_conntrack_proto_icmp.c` | ICMP 连接跟踪 |
