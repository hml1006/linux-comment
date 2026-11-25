# BPF — 内核可编程框架

## 概述

BPF（Berkeley Packet Filter）是 Linux 内核提供的一种高级内核可编程框架。它允许用户在不修改内核源码的情况下，向内核注入安全、高性能的程序。BPF 最初是为网络包过滤设计的，现在已经扩展到内核的多个子系统，包括网络、跟踪、安全、存储等。

BPF 的核心特点：
- **安全执行**：所有 BPF 程序必须通过验证器检查，确保不会破坏内核稳定性
- **高性能**：支持 JIT 编译，将 BPF 字节码编译为本地机器码
- **零开销**：未启用的 BPF 程序不会产生任何性能影响
- **灵活性**：支持多种程序类型和 Map 类型，可用于多种场景

## 架构设计

```
┌──────────────────────────────────────────────────────────────────────┐
│                        BPF 架构                                     │
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐          │
│  │   用户空间    │    │    内核空间   │    │    硬件层    │          │
│  ├──────────────┤    ├──────────────┤    ├──────────────┤          │
│  │              │    │              │    │              │          │
│  │ bpftool      │    │              │    │              │          │
│  │ bpftrace     │    │  BPF Syscall │    │  NIC offload │          │
│  │ bcc/libbpf   │────▶│  (sys_bpf)  │    │              │          │
│  │              │    │              │    │              │          │
│  │              │    │  BPF Verifier│    │              │          │
│  │              │    │  (安全检查)   │    │              │          │
│  │              │    │              │    │              │          │
│  │              │    │  BPF JIT     │    │              │          │
│  │              │    │  (编译优化)   │    │              │          │
│  │              │    │              │    │              │          │
│  │              │    │  BPF Map     │    │              │          │
│  │              │    │  (数据存储)   │    │              │          │
│  │              │    │              │    │              │          │
│  └──────────────┘    │  BPF Program │    └──────────────┘          │
│                      │  (执行引擎)   │                               │
│                      │              │                               │
│                      │  ┌────────┐  │                               │
│                      │  │网络过滤│  │                               │
│                      │  │跟踪点  │  │                               │
│                      │  │Cgroup  │  │                               │
│                      │  │XDP     │  │                               │
│                      │  │LSM     │  │                               │
│                      │  └────────┘  │                               │
│                      └──────────────┘                               │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

## 核心数据结构

### struct bpf_insn — BPF 指令

```c
struct bpf_insn {
    __u8    code;       /* 操作码 */
    __u8    dst_reg:4;  /* 目标寄存器 */
    __u8    src_reg:4;  /* 源寄存器 */
    __s16   off;        /* 有符号偏移 */
    __s32   imm;        /* 有符号立即数 */
};
```

**寄存器说明**：
- R0：返回寄存器
- R1-R5：参数传递寄存器
- R6-R9：被调用者保存寄存器
- R10：帧指针（只读）

### struct bpf_prog — BPF 程序

```c
struct bpf_prog {
    u16                     pages;              /* 程序占用的页数 */
    u8                      *insns;             /* BPF 指令数组 */
    u32                     len;                /* 指令数量 */
    enum bpf_prog_type      type;               /* 程序类型 */
    enum bpf_attach_type    expected_attach_type;
    struct bpf_prog_aux     *aux;               /* 辅助信息 */
    struct bpf_prog         *next_jit;          /* JIT 编译后的程序 */
    u8                      *jited;             /* JIT 代码指针 */
    bool                    jit_requested;      /* 是否请求 JIT */
    bool                    blinding_requested; /* 是否请求代码混淆 */
    u8 __percpu             *active;            /* per-CPU 活跃计数 */
};
```

### struct bpf_prog_aux — BPF 程序辅助信息

```c
struct bpf_prog_aux {
    struct bpf_prog         *prog;              /* 指向所属程序 */
    struct bpf_prog_aux     *main_prog_aux;     /* 主程序辅助信息 */
    struct list_head         used_maps;         /* 使用的 Map 列表 */
    struct mutex            used_maps_mutex;    /* Map 列表互斥锁 */
    struct bpf_map          *map_ptr;           /* 关联的 Map */
    struct list_head         link_list;         /* 链接列表 */
    enum bpf_attach_type    attach_type;        /* 附加类型 */
    struct bpf_cgroup_storage *cgroup_storage;
    const struct bpf_func_proto *(*get_func_proto)(enum bpf_func_id func_id,
                                                   const struct bpf_prog *prog);
    struct bpf_verifier_log *log;               /* 验证器日志 */
    u32                     id;                 /* 程序 ID */
    u32                     tag[8];             /* 程序标签 */
    char                    name[BPF_OBJ_NAME_LEN]; /* 程序名称 */
    struct bpf_prog_info    *info;              /* 程序信息 */
};
```

### struct bpf_map — BPF Map

```c
struct bpf_map {
    atomic64_t              refcnt;             /* 引用计数 */
    enum bpf_map_type       map_type;           /* Map 类型 */
    u32                     key_size;           /* 键大小 */
    u32                     value_size;         /* 值大小 */
    u32                     max_entries;        /* 最大条目数 */
    u32                     map_flags;          /* Map 标志 */
    struct bpf_map_ops      *ops;               /* Map 操作函数集 */
    struct bpf_map_memory   *memory;            /* 内存信息 */
    struct bpf_prog         *owner;             /* 所属程序 */
    struct list_head         map_list;          /* Map 列表 */
    struct hlist_node        hash;              /* 哈希表节点 */
    u32                     id;                 /* Map ID */
    char                    name[BPF_OBJ_NAME_LEN]; /* Map 名称 */
    struct btf              *btf;               /* BTF 信息 */
    struct btf_type         *value_type_btf;    /* 值类型 BTF */
    u32                     value_type_btf_id;  /* 值类型 BTF ID */
};
```

### struct bpf_map_ops — Map 操作函数集

```c
struct bpf_map_ops {
    struct bpf_map *(*map_alloc)(union bpf_attr *attr);
    void            (*map_free)(struct bpf_map *map);
    void            *(*map_lookup_elem)(struct bpf_map *map, void *key);
    int             (*map_update_elem)(struct bpf_map *map, void *key,
                                       void *value, u64 flags);
    int             (*map_delete_elem)(struct bpf_map *map, void *key);
    int             (*map_push_elem)(struct bpf_map *map, void *value, u64 flags);
    int             (*map_pop_elem)(struct bpf_map *map, void *value);
    int             (*map_peek_elem)(struct bpf_map *map, void *value);
    void            *(*map_lookup_percpu_elem)(struct bpf_map *map, void *key, u32 cpu);
    int             (*map_freeze)(struct bpf_map *map);
    u64             (*map_mem_usage)(const struct bpf_map *map);
    bool            (*map_meta_equal)(const struct bpf_map *a, const struct bpf_map *b);
    int             (*map_check_btf)(const struct bpf_map *map,
                                     const struct btf *btf,
                                     const struct btf_type *key_type,
                                     const struct btf_type *value_type);
};
```

## BPF 指令集

### 指令格式

```
| code (8 bits) | dst_reg (4 bits) | src_reg (4 bits) | off (16 bits) | imm (32 bits) |
```

### 指令分类

| 类型 | 操作码范围 | 说明 |
|------|-----------|------|
| BPF_LD | 0x00-0x07 | 加载指令 |
| BPF_LDX | 0x08-0x0f | 索引加载指令 |
| BPF_ST | 0x10-0x17 | 存储指令 |
| BPF_STX | 0x18-0x1f | 索引存储指令 |
| BPF_ALU | 0x40-0x4f | 32位算术运算 |
| BPF_JMP | 0x50-0x5f | 32位跳转指令 |
| BPF_ALU64 | 0x70-0x7f | 64位算术运算 |
| BPF_JMP32 | 0x60-0x6f | 32位跳转指令 |

### 常用指令

```c
/* 加载立即数到寄存器 */
BPF_MOV64_IMM(BPF_REG_0, 0)        // R0 = 0
BPF_LD_MAP_FD(BPF_REG_1, map_fd)   // R1 = map 指针

/* 算术运算 */
BPF_ALU64_ADD(BPF_REG_0, BPF_REG_1) // R0 += R1
BPF_ALU64_SUB_IMM(BPF_REG_0, 1)    // R0 -= 1

/* 内存访问 */
BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_0, -4)  // *(R10-4) = R0 (4字节)
BPF_LDX_MEM(BPF_W, BPF_REG_0, BPF_REG_10, -4)  // R0 = *(R10-4) (4字节)

/* 跳转指令 */
BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 2)  // 如果 R0 == 0，跳转 +2
BPF_JMP_REG(BPF_JNE, BPF_REG_0, BPF_REG_1, 1) // 如果 R0 != R1，跳转 +1

/* 函数调用 */
BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem)

/* 程序退出 */
BPF_EXIT_INSN()
```

## BPF 验证器

### 验证器工作流程

```
用户提交 BPF 程序
        │
        ▼
    第一遍：DAG 检查
        │
        ├── 检查指令数量 (<= BPF_MAXINSNS)
        ├── 检测循环 (回边检测)
        ├── 检查不可达指令
        └── 检查跳转边界
        │
        ▼
    第二遍：状态分析
        │
        ├── 跟踪每个寄存器的类型
        ├── 跟踪栈状态
        ├── 检查内存访问安全性
        ├── 检查辅助函数调用参数
        └── 分析所有执行路径
        │
        ▼
    第三遍：代码优化
        │
        ├── 死代码消除
        ├── 指令合并
        └── 常量传播
        │
        ▼
    验证通过 / 失败
```

### 寄存器类型系统

验证器使用类型系统来跟踪寄存器的状态：

| 类型 | 说明 |
|------|------|
| SCALAR_VALUE | 标量值，非指针 |
| PTR_TO_CTX | 指向 BPF 上下文的指针 |
| PTR_TO_MAP_KEY | 指向 Map 键的指针 |
| PTR_TO_MAP_VALUE | 指向 Map 值的指针 |
| PTR_TO_STACK | 指向栈的指针 |
| CONST_PTR_TO_MAP | 指向 Map 的常量指针 |
| PTR_TO_SOCKET | 指向 Socket 的指针 |
| FRAME_PTR | 帧指针（R10） |

### 内存访问检查

验证器在检查内存访问时确保：
1. 基址寄存器具有有效的指针类型
2. 访问范围在有效边界内
3. 指针没有经过不安全的运算（如指针 + 指针）

## BPF 辅助函数

### 辅助函数定义

```c
BPF_CALL_2(bpf_map_lookup_elem, struct bpf_map *, map, void *, key)
{
    WARN_ON_ONCE(!bpf_rcu_lock_held());
    return (unsigned long) map->ops->map_lookup_elem(map, key);
}

const struct bpf_func_proto bpf_map_lookup_elem_proto = {
    .func           = bpf_map_lookup_elem,
    .gpl_only       = false,
    .pkt_access     = true,
    .ret_type       = RET_PTR_TO_MAP_VALUE_OR_NULL,
    .arg1_type      = ARG_CONST_MAP_PTR,
    .arg2_type      = ARG_PTR_TO_MAP_KEY,
};
```

### 常用辅助函数分类

| 分类 | 函数 |
|------|------|
| Map 操作 | bpf_map_lookup_elem, bpf_map_update_elem, bpf_map_delete_elem |
| 哈希表 | bpf_hash_update, bpf_hash_delete |
| 数组 | bpf_array_get, bpf_array_set |
| 时间 | bpf_ktime_get_ns, bpf_get_cycle_counter |
| 网络 | bpf_skb_load_bytes, bpf_skb_store_bytes, bpf_skb_adjust_room |
| 字符串 | bpf_probe_read_str, bpf_get_current_comm |
| 栈 | bpf_get_stackid |
| CPU | bpf_get_smp_processor_id |
| 随机数 | bpf_get_prandom_u32 |

## BPF Map 类型

### 基础 Map 类型

| 类型 | 说明 | 特点 |
|------|------|------|
| BPF_MAP_TYPE_HASH | 哈希表 | 支持快速查找、插入、删除 |
| BPF_MAP_TYPE_ARRAY | 数组 | 固定大小，通过索引访问 |
| BPF_MAP_TYPE_PERCPU_HASH | per-CPU 哈希表 | 无锁访问，每个 CPU 独立 |
| BPF_MAP_TYPE_PERCPU_ARRAY | per-CPU 数组 | 无锁访问，每个 CPU 独立 |
| BPF_MAP_TYPE_LRU_HASH | LRU 哈希表 | 自动淘汰最久未使用的条目 |
| BPF_MAP_TYPE_LRU_PERCPU_HASH | LRU per-CPU 哈希表 | 结合 LRU 和 per-CPU |
| BPF_MAP_TYPE_PROG_ARRAY | 程序数组 | 用于尾调用 |
| BPF_MAP_TYPE_PERF_EVENT_ARRAY | perf 事件数组 | 用于将数据发送到用户空间 |

### 高级 Map 类型

| 类型 | 说明 |
|------|------|
| BPF_MAP_TYPE_CGROUP_ARRAY | cgroup 数组 | 用于 cgroup 绑定 |
| BPF_MAP_TYPE_CGROUP_STORAGE | cgroup 存储 | 每个 cgroup 私有数据 |
| BPF_MAP_TYPE_TASK_STORAGE | 任务存储 | 每个任务私有数据 |
| BPF_MAP_TYPE_INODE_STORAGE | inode 存储 | 每个 inode 私有数据 |
| BPF_MAP_TYPE_RINGBUF | 环形缓冲区 | 高效的用户空间通信 |
| BPF_MAP_TYPE_LPM_TRIE | 最长前缀匹配 | 用于路由表查找 |
| BPF_MAP_TYPE_BLOOM_FILTER | 布隆过滤器 | 快速存在性检测 |
| BPF_MAP_TYPE_QUEUE | 队列 | FIFO 数据结构 |
| BPF_MAP_TYPE_STACK | 栈 | LIFO 数据结构 |

## BPF 程序类型

### 网络类

| 类型 | 说明 | 附加点 |
|------|------|--------|
| BPF_PROG_TYPE_SOCKET_FILTER | Socket 过滤 | sk_filter |
| BPF_PROG_TYPE_XDP | 快速数据路径 | XDP |
| BPF_PROG_TYPE_TC | 流量控制 | TC 钩子 |
| BPF_PROG_TYPE_SK_SKB | Socket SKB | Socket |
| BPF_PROG_TYPE_SK_MSG | Socket 消息 | Socket 消息 |
| BPF_PROG_TYPE_FLOW_DISSECTOR | 流解析器 | 流解析 |

### 跟踪类

| 类型 | 说明 | 附加点 |
|------|------|--------|
| BPF_PROG_TYPE_KPROBE | 内核探针 | kprobe |
| BPF_PROG_TYPE_UPROBE | 用户空间探针 | uprobe |
| BPF_PROG_TYPE_TRACEPOINT | 跟踪点 | tracepoint |
| BPF_PROG_TYPE_RAW_TRACEPOINT | 原始跟踪点 | raw tracepoint |
| BPF_PROG_TYPE_FPROBE | 函数探针 | fprobe |

### 安全类

| 类型 | 说明 | 附加点 |
|------|------|--------|
| BPF_PROG_TYPE_CGROUP_SKB | cgroup SKB | cgroup |
| BPF_PROG_TYPE_CGROUP_SOCK | cgroup Socket | cgroup |
| BPF_PROG_TYPE_CGROUP_DEVICE | cgroup 设备 | cgroup |
| BPF_PROG_TYPE_SECURITY | 安全模块 | LSM |

### 其他

| 类型 | 说明 |
|------|------|
| BPF_PROG_TYPE_STRUCT_OPS | 结构体操作 | 替换内核函数 |
| BPF_PROG_TYPE_EXT | 扩展程序 | 扩展其他程序 |
| BPF_PROG_TYPE_LIRC_MODE2 | LIRC 模式 | 红外遥控 |

## BPF Syscall

### sys_bpf 系统调用

```c
long sys_bpf(int cmd, union bpf_attr *attr, unsigned int size);
```

### 命令类型

| 命令 | 说明 |
|------|------|
| BPF_MAP_CREATE | 创建 Map |
| BPF_MAP_LOOKUP_ELEM | 查找 Map 元素 |
| BPF_MAP_UPDATE_ELEM | 更新 Map 元素 |
| BPF_MAP_DELETE_ELEM | 删除 Map 元素 |
| BPF_MAP_GET_NEXT_KEY | 获取下一个键 |
| BPF_PROG_LOAD | 加载 BPF 程序 |
| BPF_PROG_ATTACH | 附加 BPF 程序 |
| BPF_PROG_DETACH | 分离 BPF 程序 |
| BPF_PROG_TEST_RUN | 测试运行 BPF 程序 |
| BPF_PROG_GET_NEXT_ID | 获取下一个程序 ID |
| BPF_MAP_GET_NEXT_ID | 获取下一个 Map ID |
| BPF_LINK_CREATE | 创建链接 |
| BPF_LINK_UPDATE | 更新链接 |
| BPF_LINK_DETACH | 分离链接 |
| BPF_BTF_LOAD | 加载 BTF 信息 |
| BPF_OBJ_PIN | 将对象固定到文件系统 |
| BPF_OBJ_GET | 获取固定的对象 |

## BPF JIT 编译

### JIT 工作流程

```
BPF 字节码
    │
    ▼
BPF 验证器
    │
    ▼
JIT 编译
    │
    ├── 指令选择
    ├── 寄存器分配
    ├── 代码生成
    └── 代码混淆 (可选)
    │
    ▼
本地机器码
    │
    ▼
执行
```

### JIT 架构支持

| 架构 | 支持状态 |
|------|---------|
| x86-64 | 完全支持 |
| ARM64 | 完全支持 |
| ARM | 完全支持 |
| PowerPC | 完全支持 |
| RISC-V | 完全支持 |
| s390 | 完全支持 |

## BTF (BPF Type Format)

### BTF 概述

BTF 是一种调试信息格式，用于描述 BPF 程序和 Map 中使用的数据类型。它类似于 DWARF，但更加轻量和简洁。

### BTF 数据结构

```c
struct btf {
    struct list_head        list;
    u32                     nr_types;
    struct btf_type         *types;
    const char              *strings;
    u32                     str_len;
    u32                     id;
    struct btf_ext          *ext;
    struct btf_member       *members;
    struct btf_var_secinfo  *secinfo;
};

struct btf_type {
    u32                     name_off;
    u32                     info;
    union {
        u32                 size;
        u32                 type;
    };
};
```

## 使用示例

### 简单的 kprobe 程序

```c
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, u64);
} exec_count SEC(".maps");

SEC("kprobe/sys_execve")
int BPF_KPROBE(sys_execve)
{
    u32 key = bpf_get_current_pid_tgid() >> 32;
    u64 *count, zero = 0;
    
    count = bpf_map_lookup_or_try_init(&exec_count, &key, &zero);
    if (count)
        (*count)++;
    
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
```

### 使用 bpftool 加载程序

```bash
# 编译 BPF 程序
clang -target bpf -O2 -c prog.c -o prog.o

# 加载程序
bpftool prog load prog.o /sys/fs/bpf/prog

# 查看程序信息
bpftool prog show id 1

# 附加到 kprobe
bpftool prog attach id 1 kprobe func sys_execve

# 查看 Map
bpftool map show id 1
```

### 使用 bpftrace

```bash
# 跟踪系统调用
bpftrace -e 'kprobe:sys_execve { @[comm] = count(); }'

# 跟踪进程创建
bpftrace -e 'tracepoint:sched:sched_process_fork { @forks = count(); }'

# 查看内存分配
bpftrace -e 'kprobe:kmalloc { @size = hist(args->size); }'
```

## 编译配置

| 配置项 | 说明 |
|--------|------|
| CONFIG_BPF | 启用 BPF 支持 |
| CONFIG_BPF_SYSCALL | 启用 BPF 系统调用 |
| CONFIG_BPF_JIT | 启用 BPF JIT 编译 |
| CONFIG_BPF_JIT_ALWAYS_ON | 始终启用 JIT |
| CONFIG_BPF_JIT_DEFAULT_ON | 默认启用 JIT |
| CONFIG_BPF_LSM | 启用 BPF LSM 支持 |
| CONFIG_BPF_PRELOAD | 启用 BPF 预加载 |
| CONFIG_XDP_SOCKETS | 启用 XDP Socket |
| CONFIG_CGROUP_BPF | 启用 cgroup BPF |
| CONFIG_BPF_EVENTS | 启用 BPF 事件 |
| CONFIG_BPF_KPROBE_OVERRIDE | 启用 kprobe 覆盖 |

## 代码位置

| 文件 | 说明 |
|------|------|
| `kernel/bpf/syscall.c` | BPF 系统调用实现 |
| `kernel/bpf/verifier.c` | BPF 验证器 |
| `kernel/bpf/core.c` | BPF 核心功能 |
| `kernel/bpf/helpers.c` | BPF 辅助函数 |
| `kernel/bpf/JIT/` | JIT 编译器 |
| `kernel/bpf/btf.c` | BTF 处理 |
| `kernel/bpf/map_in_map.c` | Map-in-Map 支持 |
| `include/uapi/linux/bpf.h` | 用户空间 BPF 头文件 |
| `include/linux/bpf.h` | 内核空间 BPF 头文件 |
| `include/linux/bpf_verifier.h` | BPF 验证器头文件 |