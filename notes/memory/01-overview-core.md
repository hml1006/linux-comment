# 内存管理 — 概述与核心框架 (Part I)

> 本文档拆分自 [memory_management.md](memory_management.md) Part I，涵盖总体概览、核心数据结构、伙伴系统、SLUB分配器、vmalloc、VMA管理、DMA内存

## 目录

### Part I: 概述与核心框架

1. [总体概览](#1-总体概览)
2. [核心数据结构](#2-核心数据结构)
3. [物理内存管理（Buddy System）](#3-物理内存管理buddy-system)
4. [SLUB 分配器与 Sheaves 缓存机制](#4-slub-分配器与-sheaves-缓存机制)
5. [虚拟内存管理](#5-虚拟内存管理)
6. [VMA 管理](#6-vma-管理)
7. [DMA 一致性内存与流式内存](#7-dma-一致性内存与流式内存)

---

## Part I: 概述与核心框架

## 1. 总体概览

### 1.1 文件统计

Linux 内存管理子系统位于 `mm/` 目录下，包含 **149 个 .c 源文件** 和 **15 个 .h 头文件**，以及 `damon/`、`kasan/`、`kfence/`、`kmsan/` 四个子目录，总计约 **214,524 行代码**。

### 1.2 代码量排名（Top 20）

| 文件 | 行数 | 功能 |
|------|------|------|
| `mm/slub.c` | 9,839 | SLUB 分配器（核心） |
| `mm/vmscan.c` | 7,910 | 页面回收与 kswapd |
| `mm/page_alloc.c` | 7,856 | 伙伴系统分配器 |
| `mm/memory.c` | 7,493 | 核心内存管理（缺页、页表） |
| `mm/hugetlb.c` | 7,340 | 巨页（HugeTLB）管理 |
| `mm/shmem.c` | 6,022 | 共享内存（tmpfs） |
| `mm/memcontrol.c` | 5,679 | Memory Cgroup（v2） |
| `mm/vmalloc.c` | 5,485 | vmalloc 虚拟地址分配器 |
| `mm/huge_memory.c` | 4,978 | 透明大页（THP） |
| `mm/filemap.c` | 4,829 | 文件页缓存（Page Cache） |
| `mm/ksm.c` | 4,019 | 内核同页合并（KSM） |
| `mm/swapfile.c` | 3,966 | 交换文件/设备管理 |
| `mm/mempolicy.c` | 3,945 | NUMA 内存策略 |
| `mm/gup.c` | 3,557 | get_user_pages 框架 |
| `mm/percpu.c` | 3,388 | Per-CPU 分配器 |
| `mm/compaction.c` | 3,334 | 内存压缩 |
| `mm/vma.c` | 3,309 | VMA 操作 |
| `mm/rmap.c` | 3,147 | 反向映射 |
| `mm/page-writeback.c` | 3,114 | 页写回 |
| `mm/memory-failure.c` | 2,970 | 内存故障处理 |

### 1.3 架构层次

```
用户空间 (malloc/mmap/brk)
        │
        ▼
┌──────────────────────────────────────┐
│  VMA 层 (mmap.c, vma.c, vma_exec.c)  │  ← 虚拟地址空间管理
├──────────────────────────────────────┤
│  缺页处理 (memory.c)                  │  ← 按需调页
├──────────────────────────────────────┤
│  Page Cache (filemap.c, readahead.c)  │  ← 文件缓存
├──────────────────────────────────────┤
│  回收层 (vmscan.c, vmpressure.c)      │  ← 页面回收
├──────────────────────────────────────┤
│  Swap 层 (swap.c, swapfile.c)         │  ← 交换
├──────────────────────────────────────┤
│  SLUB (slub.c) / vmalloc (vmalloc.c)  │  ← 内核动态分配
├──────────────────────────────────────┤
│  伙伴系统 (page_alloc.c)              │  ← 物理页面分配
├──────────────────────────────────────┤
│  MemBlock (memblock.c)                │  ← 启动时分配器
└──────────────────────────────────────┘
```

### 1.4 关键配置项

| 配置项 | 功能 |
|--------|------|
| `CONFIG_MMU` | 启用 MMU（关闭则使用 nommu.c） |
| `CONFIG_SWAP` | 启用交换支持 |
| `CONFIG_ZSWAP` | 压缩交换缓存 |
| `CONFIG_ZSMALLOC` | 小对象压缩分配器 |
| `CONFIG_TRANSPARENT_HUGEPAGE` | 透明大页支持 |
| `CONFIG_HUGETLBFS` | hugetlbfs 巨页支持 |
| `CONFIG_MEMCG` | 内存控制组 v2 |
| `CONFIG_MEMCG_V1` | 内存控制组 v1 |
| `CONFIG_NUMA` | NUMA 支持 |
| `CONFIG_CMA` | 连续内存分配器 |
| `CONFIG_MEMORY_HOTPLUG` | 内存热插拔 |
| `CONFIG_COMPACTION` | 内存压缩 |
| `CONFIG_KSM` | 内核同页合并 |
| `CONFIG_MEMORY_FAILURE` | 内存故障处理 |
| `CONFIG_KASAN` | 内核地址消毒器 |
| `CONFIG_KFENCE` | 内核门栏错误检测 |
| `CONFIG_KMSAN` | 内核未初始化内存检测 |
| `CONFIG_DAMON` | 数据访问监控 |
| `CONFIG_PAGE_IDLE_FLAG` | 空闲页跟踪 |
| `CONFIG_ZONE_DEVICE` | 设备内存支持 |

---

## 2. 核心数据结构

### 2.1 struct page / struct folio — 物理页面描述

```c
// include/linux/mm_types.h
struct page {
    unsigned long flags;          // 页面标志 (PG_locked, PG_dirty, PG_uptodate 等)
    union {
        struct list_head lru;     // LRU 链表节点
        struct dev_pagemap *pgmap; // ZONE_DEVICE 映射
    };
    union {
        atomic_t _mapcount;       // 映射计数
        unsigned int page_type;   // 页面类型 (buddy, slab, huge)
    };
    atomic_t _refcount;           // 引用计数
    unsigned int _active;         // 活跃计数
    unsigned long private;        // 私有数据
    struct address_space *mapping; // 关联的 address_space
    pgoff_t index;                // 页内偏移
    union {
        struct folio *shadow;     // 影子 folio
    };
    struct mem_cgroup *memcg_data; // Memory cgroup 数据
};
```

**folio 封装**：`struct folio` 是 page 的封装，确保处理复合页（compound page）时类型安全。folio 的头页（head page）就是复合页的第一页。

```c
// include/linux/mm_types.h
struct folio {
    struct page page;  // 嵌入 page 结构
};
```

### 2.2 struct zone — 内存区域

```c
// include/linux/mmzone.h
enum zone_type {
    ZONE_DMA,       // DMA 可寻址区域
    ZONE_DMA32,     // 32位 DMA 可寻址区域
    ZONE_NORMAL,    // 普通内存区域
    ZONE_HIGHMEM,   // 高端内存（仅 32位）
    ZONE_MOVABLE,   // 可移动页面
    ZONE_DEVICE,    // 设备内存
    __MAX_NR_ZONES
};

struct zone {
    unsigned long _watermark[NR_WMARK];  // 水位线 (min/low/high)
    unsigned long watermark_boost;        // 水位线提升
    long lowmem_reserve[MAX_NR_ZONES];   // 低内存预留
    struct pglist_data *zone_pgdat;       // 所属节点
    struct per_cpu_pages __percpu *per_cpu_pageset; // Per-CPU 页面缓存
    /* 空闲区域管理 */
    struct free_area free_area[MAX_ORDER]; // 伙伴系统空闲链表
    spinlock_t lock;                      // 区域锁
    /* 统计信息 */
    atomic_long_t managed_pages;          // 管理的页面数
    unsigned long spanned_pages;          // 跨越的页面数
    unsigned long present_pages;          // 存在的页面数
    const char *name;                     // 区域名称
};
```

### 2.3 struct pglist_data — NUMA 节点

```c
// include/linux/mmzone.h
typedef struct pglist_data {
    struct zone node_zones[MAX_NR_ZONES];     // 节点包含的区域
    struct zonelist node_zonelists[MAX_ZONELISTS]; // 区域列表
    int nr_zones;                              // 已填充区域数
    unsigned long node_start_pfn;              // 节点起始 PFN
    unsigned long node_present_pages;          // 物理页面数
    unsigned long node_spanned_pages;          // 跨越的页面数
    int node_id;                               // 节点 ID
    wait_queue_head_t kswapd_wait;             // kswapd 等待队列
    struct task_struct *kswapd;                // kswapd 内核线程
    int kswapd_order;                          // kswapd 分配阶
    wait_queue_head_t kcompactd_wait;          // kcompactd 等待队列
    struct task_struct *kcompactd;             // kcompactd 内核线程
    unsigned long totalreserve_pages;          // 总预留页面数
    /* LRU 链表 */
    struct lruvec __lruvec;                    // LRU 向量
    /* 页面回收参数 */
    unsigned long flags;                       // 节点标志
    spinlock_t lru_lock;                       // LRU 锁
} pg_data_t;
```

### 2.4 struct vm_area_struct — 虚拟内存区域

```c
// include/linux/mm_types.h
struct vm_area_struct {
    unsigned long vm_start;          // 起始地址
    unsigned long vm_end;            // 结束地址
    struct mm_struct *vm_mm;         // 所属进程地址空间
    unsigned long vm_flags;          // 标志 (VM_READ, VM_WRITE, VM_EXEC 等)
    const struct vm_operations_struct *vm_ops; // VMA 操作
    unsigned long vm_pgoff;          // 文件偏移 (页粒度)
    struct file *vm_file;            // 映射的文件
    void *vm_private_data;           // 私有数据
    struct list_head anon_vma_chain; // 匿名 VMA 链
    struct anon_vma *anon_vma;       // 匿名 VMA
    /* 优先搜索树节点 */
    struct rb_node vm_rb;            // 红黑树节点
};
```

### 2.5 struct kmem_cache — SLUB 缓存

```c
// include/linux/slub_def.h (mm/slab.h)
struct kmem_cache {
    struct slab *cpu_slab;           // Per-CPU slab 指针
    unsigned long size;              // 对象大小（含对齐）
    unsigned int object_size;        // 原始对象大小
    unsigned int offset;             // 空闲指针偏移
    unsigned int cpu_partial;        // Per-CPU 部分 slab 数
    struct kmem_cache_node *node[MAX_NUMNODES]; // 每个 NUMA 节点管理
    slab_flags_t flags;              // 标志
    int refcount;                    // 引用计数
    const char *name;                // 缓存名称
    struct list_head list;           // 全局缓存链表
};
```

### 2.6 关键分配器对比

| 分配器 | 单位 | 适用场景 | 接口 |
|--------|------|----------|------|
| 伙伴系统 | 2^N 页 | 物理连续页面 | `alloc_pages()` / `free_pages()` |
| SLUB | 任意大小 | 小对象频繁分配 | `kmalloc()` / `kfree()` |
| vmalloc | 页 | 虚拟连续但不要求物理连续 | `vmalloc()` / `vfree()` |
| mempool | 任意大小 | 保证分配不失败 | `mempool_alloc()` / `mempool_free()` |
| percpu | 任意大小 | Per-CPU 数据 | `alloc_percpu()` / `free_percpu()` |

---

## 3. 物理内存管理（Buddy System）

### 3.1 概述

文件：`mm/page_alloc.c`（7,856 行）

伙伴系统（Buddy System）是 Linux 物理内存分配的核心，负责管理所有物理页面。它将物理内存按 2^N 页（order 0 到 MAX_ORDER-1，默认 MAX_ORDER=11，即最大 2MB 连续块）组织成链表，分配和释放时自动合并相邻空闲块。

### 3.2 核心数据结构（带注释）

#### 3.2.1 struct zone — 内存域

```c
// include/linux/mmzone.h
struct zone {
    /* ====== 读密集型字段（Read-mostly） ====== */

    // 水位线：WMARK_MIN / WMARK_LOW / WMARK_HIGH
    // 通过 _wmark_pages(zone) 宏访问
    unsigned long _watermark[NR_WMARK];
    unsigned long watermark_boost;       // 水位线提升值（临时提高水位）
    unsigned long nr_reserved_highatomic; // 为高优先级原子分配预留的页面数
    unsigned long nr_free_highatomic;    // 当前空闲的高原子页面数

    // 低内存保护：防止低 zone 被过度消耗
    // lowmem_reserve[ZONE_HIGH] = 在分配 ZONE_HIGH 内存时，需要保留的 low zone 页面数
    long lowmem_reserve[MAX_NR_ZONES];

#ifdef CONFIG_NUMA
    int node;                           // 所属 NUMA 节点 ID
#endif
    struct pglist_data  *zone_pgdat;    // 指向所属 NUMA 节点的 pglist_data
    struct per_cpu_pages __percpu *per_cpu_pageset;  // Per-CPU 页面缓存
    struct per_cpu_zonestat __percpu *per_cpu_zonestats; // Per-CPU zone 统计

    // Per-CPU pageset 的 high/batch 缓存值（加速访问）
    int pageset_high_min;
    int pageset_high_max;
    int pageset_batch;

    unsigned long zone_start_pfn;       // zone 起始物理页帧号

    // 页面计数
    // spanned_pages = zone_end_pfn - zone_start_pfn（含空洞）
    // present_pages = spanned_pages - absent_pages（实际存在的物理页面）
    // managed_pages = present_pages - reserved_pages（伙伴系统管理的页面）
    atomic_long_t managed_pages;

    unsigned long spanned_pages;
    unsigned long present_pages;
    unsigned long present_early_pages;  // 早期启动时就存在的页面（不含热插拔）

#ifdef CONFIG_CMA
    unsigned long cma_pages;            // CMA 连续内存分配器占用的页面
#endif

    seqlock_t span_seqlock;             // 保护 zone_start_pfn / spanned_pages 的顺序锁

    int initialized;                    // zone 是否已完成初始化

    /* ====== 写密集型字段（Write-intensive） ====== */
    CACHELINE_PADDING(_pad1_);          // 缓存行填充，避免伪共享

    // 伙伴系统空闲区域数组：free_area[order] 管理 order 对应的空闲链表
    struct free_area free_area[NR_PAGE_ORDERS];

#ifdef CONFIG_UNACCEPTED_MEMORY
    struct list_head unaccepted_pages;  // 待接受的内存页面（机密计算）
#endif

    unsigned long flags;                // zone 标志位（如 ZONE_BOOSTED_WATERMARK）
    spinlock_t lock;                    // 保护 free_area 的主锁（主要竞争点）

    struct llist_head trylock_free_pages; // trylock 失败时暂存的释放页面

    /* ====== 压缩和统计字段 ====== */
    CACHELINE_PADDING(_pad2_);

    // 以下字段用于 pagevec、vmstat、compaction 等
    // ...
};
```

#### 3.2.2 struct free_area — 空闲区域

```c
// include/linux/mmzone.h
struct free_area {
    struct list_head free_list[MIGRATE_TYPES];  // 按迁移类型分类的空闲链表
    unsigned long nr_free;                      // 该 order 的空闲页面总数
};
```

每个 order 对应一个 `free_area`，内部再按迁移类型（MIGRATE_UNMOVABLE/MOVABLE/RECLAIMABLE 等）分为多个链表，减少碎片化。

#### 3.2.3 struct pglist_data — NUMA 节点

```c
// include/linux/mmzone.h
typedef struct pglist_data {
    struct zone node_zones[MAX_NR_ZONES];    // 本节点的所有 zone
    struct zonelist node_zonelists[MAX_ZONELISTS]; // zonelist（按优先级排序的 zone 列表）

    int nr_zones;                            // 实际 populated 的 zone 数量

    unsigned long node_start_pfn;            // 节点起始页帧号
    unsigned long node_present_pages;        // 节点物理页面总数
    unsigned long node_spanned_pages;        // 节点页面范围（含空洞）

    int node_id;                             // 节点 ID

    // kswapd 相关
    wait_queue_head_t kswapd_wait;           // kswapd 等待队列
    wait_queue_head_t pfmemalloc_wait;       // 内存压力等待队列
    struct task_struct *kswapd;              // kswapd 内核线程
    int kswapd_order;                        // kswapd 要回收到的 order
    enum zone_type kswapd_highest_zoneidx;   // kswapd 要回收的最高 zone 类型

    // kcompactd 相关（内存压缩）
    int kcompactd_max_order;
    enum zone_type kcompactd_highest_zoneidx;
    wait_queue_head_t kcompactd_wait;
    struct task_struct *kcompactd;

    unsigned long totalreserve_pages;        // 总预留页面数（不可用于用户态分配）

#ifdef CONFIG_NUMA
    unsigned long min_unmapped_pages;        // 最小未映射页面数（节点回收触发条件）
    unsigned long min_slab_pages;            // 最小 slab 页面数
#endif

    // ... 其他字段
} pg_data_t;
```

#### 3.2.4 struct per_cpu_pages — Per-CPU 页面缓存

```c
// include/linux/mmzone.h
struct per_cpu_pages {
    spinlock_t lock;            // 保护 lists 的自旋锁
    int count;                  // 当前缓存的页面总数
    int high;                   // 高水位：超过此值时需要释放回伙伴系统
    int high_min;               // high 的最小值
    int high_max;               // high 的最大值
    int batch;                  // 批量填充/释放的块大小
    u8 flags;                   // 标志位（PCPF_PREV_FREE_HIGH_ORDER 等）
    u8 alloc_factor;            // 分配时的 batch 缩放因子
#ifdef CONFIG_NUMA
    u8 expire;                  // 过期计数（用于清理远程 pageset）
#endif
    short free_count;           // 连续释放计数

    // 按迁移类型和 order 分类的页面链表
    // 索引由 order_to_pindex(migratetype, order) 计算
    struct list_head lists[NR_PCP_LISTS];
} ____cacheline_aligned_in_smp;  // 缓存行对齐，避免伪共享
```

**PCP 设计要点**：
- 每个 CPU 核心拥有独立的 pageset，避免多核竞争 zone->lock
- 主要用于 order-0 分配加速，也支持 THP（透明大页）的快速分配
- `high` 动态调整：`decay_pcp_high()` 定期降低 high，空闲时释放多余页面
- `free_count` 跟踪连续释放频率，用于调整批量释放大小

#### 3.2.5 struct alloc_context — 分配上下文

```c
// mm/internal.h
struct alloc_context {
    struct zonelist *zonelist;          // zonelist（按优先级排序的 zone 列表）
    nodemask_t *nodemask;               // 允许分配的 NUMA 节点掩码
    struct zoneref *preferred_zoneref;  // 首选 zone 引用
    int migratetype;                    // 迁移类型（MIGRATE_*）
    enum zone_type highest_zoneidx;     // 最高可用的 zone 索引
    bool spread_dirty_pages;            // 是否在节点间分散脏页
};
```

#### 3.2.6 迁移类型（MIGRATE_TYPES）

```c
// include/linux/mmzone.h
enum migratetype {
    MIGRATE_UNMOVABLE,      // 不可移动（内核核心分配，如页表、VMA 结构体）
    MIGRATE_MOVABLE,        // 可移动（用户态页面，可被 compaction 迁移）
    MIGRATE_RECLAIMABLE,    // 可回收（如 dentry cache、inode cache）
    MIGRATE_PCPTYPES,       // 用于 Per-CPU 页面的数量（上限）
    MIGRATE_HIGHATOMIC,     // 高优先级原子分配预留（防止 atomic 分配失败）
    MIGRATE_CMA,            // CMA 连续内存分配区域
    MIGRATE_ISOLATE,        // 隔离状态（用于内存热插拔、page migration）
    MIGRATE_TYPES
};
```

**迁移类型回退链**：当首选迁移类型的空闲链表耗尽时，按以下顺序回退：

```c
static int fallbacks[MIGRATE_PCPTYPES][MIGRATE_PCPTYPES - 1] = {
    [MIGRATE_UNMOVABLE]   = { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE   },
    [MIGRATE_MOVABLE]     = { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE },
    [MIGRATE_RECLAIMABLE] = { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE   },
};
```

### 3.3 GFP 标志位

```c
#define GFP_KERNEL      (__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_ATOMIC      (__GFP_HIGH)
#define GFP_NOIO        (__GFP_RECLAIM)
#define GFP_NOFS        (__GFP_RECLAIM | __GFP_IO)
#define GFP_USER         (__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_HIGHUSER     (GFP_USER | __GFP_HIGHMEM)
#define GFP_DMA          (__GFP_DMA)
#define GFP_DMA32        (__GFP_DMA32)
#define GFP_NOWAIT       (__GFP_KSWAPD_RECLAIM)
```

### 3.4 水位线（Watermark）与回收触发

```c
enum zone_watermarks {
    WMARK_MIN,      // 最低水位（0.75 * sum(size)）：达到此值触发直接回收
    WMARK_LOW,      // 低水位（1.25 * sum(size)）：kswapd 开始异步回收
    WMARK_HIGH,     // 高水位（1.75 * sum(size)）：kswapd 停止回收
    NR_WMARK
};
```

| 水位 | 含义 | 触发动作 |
|------|------|----------|
| `WMARK_HIGH` | 充足 | kswapd 睡眠等待 |
| `WMARK_LOW` | 触发回收 | 唤醒 kswapd 异步回收页面 |
| `WMARK_MIN` | 最低保障 | 分配路径进入慢速路径，直接回收页面 |
| 低于 `WMARK_MIN` | 严重不足 | 触发 OOM（`__alloc_pages_may_oom`） |

**水位线计算**：`setup_per_zone_wmarks()` 基于 `min_free_kbytes` 计算各 zone 的水位线，并考虑 `watermark_scale_factor` 和 `lowmem_reserve` 的保护。

### 3.5 完整分配函数调用栈

```
alloc_pages(gfp_mask, order)                           [include/linux/gfp.h]
  └─ alloc_frozen_pages_noprof(gfp, order)             [mm/mempolicy.c]
       └─ alloc_pages_mpol(gfp, order, pol, ...)       [mm/mempolicy.c]
            └─ __alloc_frozen_pages_noprof(gfp, order, preferred_nid, nodemask)
                                                       [mm/page_alloc.c]
                 │
                 ├─ 1. prepare_alloc_pages()           // 初始化 alloc_context
                 │    ├─ 设置 ac->zonelist（从 preferred_nid 获取 zonelist）
                 │    ├─ 设置 ac->nodemask（允许的节点掩码）
                 │    ├─ 设置 ac->migratetype（gfpflags_to_migratetype 转换）
                 │    ├─ 设置 ac->highest_zoneidx（根据 gfp 确定最大 zone 类型）
                 │    └─ 设置 alloc_flags = ALLOC_WMARK_LOW（使用低水位线）
                 │
                 ├─ 2. get_page_from_freelist()        // ★ 快速路径
                 │    │
                 │    ├─ 遍历 zonelist（按优先级扫描 zone）
                 │    │    for_next_zone_zonelist_nodemask(zone, z, ...)
                 │    │    │
                 │    │    ├─ 检查 cpuset 权限
                 │    │    ├─ 检查脏页限制（spread_dirty_pages）
                 │    │    ├─ 检查内存节点 kswapd 活跃性
                 │    │    │
                 │    │    ├─ 水位线检查（关键入口）         [zone_watermark_fast]
                 │    │    │    └─ zone_watermark_ok()
                 │    │    │         └─ __zone_watermark_ok()
                 │    │    │              └─ 检查 free > watermark + free_pages_adjust
                 │    │    │                 其中 free_pages_adjust 考虑：
                 │    │    │                   - nr_reserved_highatomic
                 │    │    │                   - lowmem_reserve[ac->highest_zoneidx]
                 │    │    │                   - oom_reserve
                 │    │    │
                 │    │    └─ 若水位线满足 → try_this_zone:
                 │    │         └─ rmqueue()            // 实际分配
                 │    │              │
                 │    │              ├─ [PCP 路径] 若 order 支持 PCP（含 THP）:
                 │    │              │    rmqueue_pcplist()
                 │    │              │    └─ __rmqueue_pcplist()
                 │    │              │         ├─ 若 PCP 链表为空：
                 │    │              │         │    rmqueue_bulk()          // 从伙伴系统批量填充
                 │    │              │         │    └─ __rmqueue()          // 低层分配
                 │    │              │         │         └─ __rmqueue_smallest() // 从小 order 开始搜索
                 │    │              │         │              └─ page_del_and_expand() // 分割
                 │    │              │         └─ 从 PCP 链表头部取出页面
                 │    │              │
                 │    │              └─ [Buddy 路径] 若 order 不支持 PCP:
                 │    │                   rmqueue_buddy()
                 │    │                   └─ __rmqueue()  // 直接操作 zone->free_area
                 │    │                        └─ __rmqueue_smallest()  // 从 order 向上搜索
                 │    │                        └─ __rmqueue_claim()     // 从其他迁移类型 claim
                 │    │                        └─ __rmqueue_steal()     // 从其他迁移类型 steal
                 │    │
                 │    └─ 若所有 zone 水位都不满足：
                 │         → 返回 NULL（快速路径失败）
                 │
                 └─ 3. 若快速路径失败：
                      __alloc_pages_slowpath()          // ★ 慢速路径
                                                       [mm/page_alloc.c]
                      └─ gfp_to_alloc_flags()          // 提升 alloc_flags（可能允许使用 MIN 水位）
                      └─ 重试循环：
                           ├─ __alloc_pages_direct_compact()  // 尝试内存压缩
                           │    └─ try_to_compact_pages()     // 扫描并迁移可移动页面
                           │
                           ├─ __alloc_pages_direct_reclaim()  // 直接回收
                           │    └─ __perform_reclaim()
                           │         └─ try_to_free_pages()   // 扫描 LRU 链表回收页面
                           │
                           └─ __alloc_pages_may_oom()         // OOM 处理
                                └─ out_of_memory()            // 选择并杀死进程
```

### 3.6 分配路径详细流程图

```
__alloc_frozen_pages_noprof(gfp, order, preferred_nid, nodemask)
         │
         ▼
   prepare_alloc_pages()
   ────────────────────
   • 初始化 alloc_context
   • 设置 migratetype、highest_zoneidx
   • alloc_flags = ALLOC_WMARK_LOW
         │
         ▼
   get_page_from_freelist()  ──────────────────────────────── 快速路径
         │
         ▼
   for_each_zone_in_zonelist:
         │
         ├── zone_watermark_fast(zone, order, WMARK_LOW) ?
         │    ├── 是 → rmqueue()
         │    │    ├── order 支持 PCP? → rmqueue_pcplist()
         │    │    │    └── PCP 空? → rmqueue_bulk() → __rmqueue()
         │    │    └── 否则 → rmqueue_buddy() → __rmqueue()
         │    │         └── __rmqueue_smallest(order, migratetype)
         │    │              ├── free_area[order] 有空闲? → page_del_and_expand()
         │    │              ├── free_area[order+1] 有空闲? → 分割 (expand)
         │    │              ├── free_area[order+2] 有空闲? → 分割
         │    │              └── ... 直到 MAX_ORDER-1
         │    │         └── 失败 → __rmqueue_claim() / __rmqueue_steal()
         │    │              → 从其他迁移类型偷取页面
         │    └── 否 → 继续下一个 zone
         │
         └── 所有 zone 都失败 → 快速路径返回 NULL
                  │
                  ▼
         __alloc_pages_slowpath()  ────────────────────────── 慢速路径
                  │
                  ▼
         重试循环 (最多 16 次):
         ┌─────────────────────────────────────────────────┐
         │  1. gfp_to_alloc_flags() → 提升 alloc_flags     │
         │     (可能允许使用 ALLOC_NO_WATERMARKS)           │
         │                                                 │
         │  2. 尝试 get_page_from_freelist() 再次分配      │
         │     (使用更高权限的 alloc_flags)                 │
         │                                                 │
         │  3. 若失败 → __alloc_pages_direct_compact()     │
         │     (内存压缩，合并碎片)                          │
         │                                                 │
         │  4. 若失败 → __alloc_pages_direct_reclaim()     │
         │     (直接回收页面，扫描 LRU)                     │
         │                                                 │
         │  5. 若失败 → 重试或 OOM                         │
         │     └─ __alloc_pages_may_oom()                  │
         │          └─ out_of_memory() kill 进程           │
         └─────────────────────────────────────────────────┘
```

### 3.7 __rmqueue_smallest — 低层分配核心

```c
// mm/page_alloc.c
struct page *__rmqueue_smallest(struct zone *zone, unsigned int order,
                                int migratetype)
{
    unsigned int current_order;
    struct free_area *area;
    struct page *page;

    // 从请求的 order 开始，逐级向上搜索
    for (current_order = order; current_order < NR_PAGE_ORDERS; ++current_order) {
        area = &(zone->free_area[current_order]);

        // 从对应迁移类型的空闲链表获取页面
        page = get_page_from_free_area(area, migratetype);
        if (!page)
            continue;  // 当前 order 无空闲，尝试更大 order

        // 从空闲链表移除，并将剩余部分分割成更小的块
        // page_del_and_expand() 内部调用 expand() 分割
        page_del_and_expand(zone, page, order, current_order, migratetype);
        return page;
    }

    return NULL;  // 所有 order 都无空闲页面
}
```

**分割算法（expand）**：

```
假设请求 order=1，找到 order=3 的空闲块：

  order=3  [ | | | | | | | | ]   ← 找到的 8 页块（2^3）
            ↓ page_del_and_expand()
  order=2  [ | | | | ] [ | | | | ]   ← 分割为 2 个 4 页块
            ↓
  order=1  [ | | ] [ | | ] [ | | ] [ | | ]   ← 再分割为 4 个 2 页块
            ↑                        ↑
        返回给调用者              放入 free_area[1]

  expand() 将从高 order 向低 order 逐级分割：
  - 高半部分放入当前 order 的空闲链表
  - 低半部分继续向下分割
  - 直到达到请求的 order
```

### 3.8 完整释放函数调用栈

```
free_pages(page, order)                              [mm/page_alloc.c]
  └─ __free_pages(page, order)                       [mm/page_alloc.c]
       └─ put_page_testzero(page) → __free_frozen_pages(page, order, FPI_NONE)
            └─ free_one_page(zone, page, pfn, order, mt, fpi_flags)
                 │
                 ├─ 若 order 支持 PCP 且非孤立迁移类型:
                 │    free_unref_page(zone, page, order, mt)
                 │    └─ 放入 PCP 链表（per_cpu_pages->lists）
                 │    └─ 若 PCP->count > PCP->high:
                 │         free_pcppages_bulk(zone, ...)  // 批量释放回伙伴系统
                 │         └─ __free_one_page() 逐个释放
                 │
                 └─ 否则:
                      __free_one_page(page, pfn, zone, order, mt, fpi_flags)
                      // 直接合并伙伴并插入 zone->free_area
```

### 3.9 __free_one_page — 伙伴合并核心算法

```c
// mm/page_alloc.c
static inline void __free_one_page(struct page *page,
        unsigned long pfn, struct zone *zone, unsigned int order,
        int migratetype, fpi_t fpi_flags)
{
    // 从当前 order 开始，尝试向上合并
    while (order < MAX_PAGE_ORDER) {
        // 查找伙伴：find_buddy_page_pfn(page, pfn, order, &buddy_pfn)
        // 伙伴的 pfn = pfn ^ (1 << order)（异或翻转当前 order 的位）
        buddy = find_buddy_page_pfn(page, pfn, order, &buddy_pfn);
        if (!buddy)
            goto done_merging;  // 伙伴不在空闲链表中，停止合并

        // 检查伙伴的迁移类型是否兼容（防止跨 pageblock 的非法合并）
        if (unlikely(order >= pageblock_order)) {
            buddy_mt = get_pfnblock_migratetype(buddy, buddy_pfn);
            if (migratetype != buddy_mt &&
                (!migratetype_is_mergeable(migratetype) ||
                 !migratetype_is_mergeable(buddy_mt)))
                goto done_merging;  // 迁移类型不兼容，停止合并
        }

        // 从空闲链表中移除伙伴
        __del_page_from_free_list(buddy, zone, order, buddy_mt);

        // 计算合并后的页面
        // combined_pfn = buddy_pfn & pfn（取两者中较小的地址）
        combined_pfn = buddy_pfn & pfn;
        page = page + (combined_pfn - pfn);
        pfn = combined_pfn;
        order++;  // 提升 order，继续尝试合并
    }

done_merging:
    // 将合并后的块插入对应 order 的空闲链表
    __add_to_free_list(page, zone, order, migratetype, to_tail);
}
```

**伙伴查找算法**：

```
伙伴条件（两个块互为 buddy）：
  1. 大小相同：都是 2^N 页
  2. 物理地址连续：pfn_buddy = pfn ^ (1 << order)（异或运算）
  3. 地址对齐：pfn 的低 order 位必须为 0

示例（order=0, 4KB 页）：
  page 的 pfn = 0x100
  buddy 的 pfn = 0x100 ^ (1 << 0) = 0x101
  → 检查 pfn=0x101 的页面是否在 free_area[0] 中

示例（order=1, 8KB 块）：
  block 的 pfn = 0x100（2 页对齐）
  buddy 的 pfn = 0x100 ^ (1 << 1) = 0x102
  → 检查 pfn=0x102 开始的 2 页块是否在 free_area[1] 中
```

**合并示意图**：

```
释放前：
  free_area[0]: [PFN=0x101] [PFN=0x103]          ← 两个单页空闲
  free_area[1]: [PFN=0x100-0x101] 已被分配        ← 2 页块被占用
  free_area[2]: (空)

释放 PFN=0x100 (order=0)：
  步骤 1: 检查 buddy → PFN=0x101 在 free_area[0] 中
          → 合并为 order=1 块 [PFN=0x100-0x101]
  步骤 2: 检查 order=1 的 buddy → PFN=0x102 在 free_area[0] 中（注意这里是 0x102 不是 0x103）
          → 等等，PFN=0x102 在 free_area[0] 中，但 order=1 的 buddy 应该是一个 2 页块
          → 实际上 order=1 的 buddy 是 PFN=0x102-0x103
          → 但 PFN=0x103 也在 free_area[0] 中，不过 buddy 检查需要整个块都在 free_area[1] 中
          → 如果 PFN=0x102-0x103 被合并为 order=1 块之前，PFN=0x102 和 0x103 是独立的 order=0 页面
          → 所以合并会先检查 order=1 的 buddy 块是否在 free_area[1] 中
          → 如果不在，则停止合并，将 [PFN=0x100-0x101] 放入 free_area[1]
```

**关键设计要点**：
- 伙伴算法使用异或运算（`pfn ^ (1 << order)`）快速定位伙伴
- 合并从低 order 向高 order 逐级进行（while 循环）
- 到达 `pageblock_order` 时检查迁移类型兼容性，防止跨 pageblock 的非法合并
- 使用 `compaction_capture()` 检查是否可以捕获页面用于内存压缩

### 3.10 水位线检查流程

```
zone_watermark_fast(zone, order, mark, highest_zoneidx, alloc_flags, gfp_mask)
         │
         ▼
    ┌──────────────────────────────────────────────────────────┐
    │ 快速路径：检查 free_pages - nr_reserved_highatomic       │
    │          > mark + (1 << order)                           │
    └──────────────────────────────────────────────────────────┘
         │
         ▼  (若快速路径通过)
    ┌──────────────────────────────────────────────────────────┐
    │ 返回 true（水位线充足，可以进行分配）                      │
    └──────────────────────────────────────────────────────────┘
         │
         ▼  (若快速路径失败)
    __zone_watermark_ok(zone, order, mark, highest_zoneidx, alloc_flags, free_pages)
         │
         ▼
    ┌──────────────────────────────────────────────────────────┐
    │ 精确计算：                                                │
    │ free_pages -= nr_reserved_highatomic                      │
    │ free_pages -= lowmem_reserve[highest_zoneidx]             │
    │ free_pages -= oom_reserve（若 ALLOC_OOM）                 │
    │                                                          │
    │ 返回 free_pages > mark + (1 << order) - 1                │
    └──────────────────────────────────────────────────────────┘
```

### 3.11 分配标志位（ALLOC_FLAGS）

```c
#define ALLOC_WMARK_MIN       WMARK_MIN     // 使用 MIN 水位线
#define ALLOC_WMARK_LOW       WMARK_LOW     // 使用 LOW 水位线（默认）
#define ALLOC_WMARK_HIGH      WMARK_HIGH    // 使用 HIGH 水位线
#define ALLOC_NO_WATERMARKS   0x04          // 绕过水位线检查（仅 PF_MEMALLOC 使用）
#define ALLOC_HARDER          0x08          // 更努力分配（允许使用 HIGHATOMIC 预留）
#define ALLOC_HIGHATOMIC      0x10          // 高优先级原子分配
#define ALLOC_KSWAPD          0x40          // 唤醒 kswapd
#define ALLOC_CPUSET          0x80          // 检查 cpuset 权限
#define ALLOC_CMA             0x100         // 允许从 CMA 区域分配
#define ALLOC_OOM             0x200         // OOM 场景下的分配
#define ALLOC_NOFRAGMENT      0x400         // 避免碎片化（尽量从本地节点分配）
#define ALLOC_TRYLOCK         0x800         // 使用 trylock 获取 zone->lock
```

### 3.12 伙伴系统关键数据流总结

```
┌──────────────────────────────────────────────────────────────────┐
│                    伙伴系统完整数据流                              │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  分配路径：                                                      │
│  ┌──────────┐    ┌──────────────┐    ┌──────────────────────┐   │
│  │ 调用者    │───→│ alloc_context │───→│ get_page_from_       │   │
│  │ (GFP标志) │    │ (分配参数)    │    │ freelist()           │   │
│  └──────────┘    └──────────────┘    │ • 遍历 zonelist      │   │
│                                       │ • 检查水位线          │   │
│                                       │ • 调用 rmqueue()     │   │
│                                       └───────┬──────────────┘   │
│                                               │                   │
│                                  ┌────────────┴────────────┐     │
│                                  │  order 支持 PCP?        │     │
│                                  └───────┬────────┬────────┘     │
│                                     是／  否  │        │          │
│                                   ┌─┘        │        │          │
│                                   ▼          │        ▼          │
│                           ┌────────────┐     │  ┌────────────┐  │
│                           │ PCP链表    │     │  │ 伙伴系统    │  │
│                           │ • 快速     │     │  │ free_area  │  │
│                           │ • 无锁竞争  │     │  │ • 分割     │  │
│                           │ • order-0  │     │  │ • 合并     │  │
│                           └────────────┘     │  │ • 迁移回退  │  │
│                                               │  └────────────┘  │
│                                               │        │          │
│                                               └────────┘          │
│                                                        │          │
│                                                        ▼          │
│                                                 ┌──────────────┐ │
│                                                 │ 返回 page    │ │
│                                                 │ (prep_new_   │ │
│                                                 │  page初始化)  │ │
│                                                 └──────────────┘ │
│                                                                  │
│  释放路径：                                                      │
│  ┌──────────┐    ┌──────────────┐    ┌──────────────────────┐   │
│  │ free_    │───→│ 是否支持PCP? │───→│ 是 → PCP链表        │   │
│  │ pages()  │    │              │    │ 否 → __free_one_    │   │
│  └──────────┘    └──────────────┘    │     page()          │   │
│                                       │     • 查找伙伴      │   │
│                                       │     • 合并（递归）   │   │
│                                       │     • 插入 free_   │   │
│                                       │       area[order]  │   │
│                                       └──────────────────────┘   │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 3.13 伙伴系统关键性能优化

| 优化 | 机制 | 收益 |
|------|------|------|
| **Per-CPU Pageset** | 每个 CPU 独立缓存 order-0 页面 | 消除 zone->lock 竞争 |
| **迁移类型分离** | 按页面用途分链 | 减少碎片化，提高 compaction 效率 |
| **批量操作** | rmqueue_bulk / free_pcppages_bulk | 减少锁获取次数 |
| **水位线动态调整** | decay_pcp_high 随时间降低 PCP high | 空闲时释放缓存页面 |
| **watermark boost** | 大块分配时临时提升水位线 | 防止碎片化导致分配失败 |
| **compaction capture** | 释放时直接捕获页面用于压缩 | 减少碎片化，提高大块分配成功率 |
| **HIGMATOMIC 预留** | 预留页面给原子分配 | 防止 atomic 上下文分配失败 |

### 3.14 MemBlock 启动分配器

文件：`mm/memblock.c`（2,768 行）

在内核启动初期，伙伴系统尚未初始化时，使用 MemBlock 分配器管理物理内存。

```c
struct memblock {
    struct memblock_type memory;    // 可用物理内存区域
    struct memblock_type reserved;  // 预留内存区域
    struct memblock_type physmem;   // 完整物理内存（含不可用）
};
```

**关键接口**:

| 接口 | 功能 |
|------|------|
| `memblock_add(addr, size)` | 添加可用区域 |
| `memblock_reserve(addr, size)` | 预留内存 |
| `memblock_alloc(size, align)` | 分配内存 |
| `memblock_free(addr, size)` | 释放内存 |

**mm_init 初始化流程**:

```
start_kernel()
  └─ setup_arch()
       └─ arm64_memblock_init()       // 从设备树/ACPI 解析物理内存
  └─ mm_init()
       └─ mem_init()                  // 打印内存分布
       └─ kmem_cache_init()           // 初始化 SLUB
       └─ pgtable_init()              // 页表初始化
       └─ zonelists_build()           // 构建 zonelist
       └─ page_alloc_init()           // 伙伴系统初始化
```

### 3.15 启动内存管理三阶段

Linux 内核启动时的内存管理是一个**多阶段、逐层初始化**的过程，从简单的物理内存分配器逐步过渡到功能完备的虚拟内存管理子系统。

#### 第一阶段：memblock 早期分配器

在内核启动的最早期，`bootmem` 分配器已被弃用，统一使用 `memblock`。

- **核心任务**：伙伴系统和 SLUB 等核心组件尚未初始化，memblock 作为精简分配器管理所有物理内存
- **生命周期**：由 `setup_arch()` 完成初始化，在伙伴系统就绪后于 `mem_init()` 中逐步移除
- **分配特点**：不支持释放和回收，仅提供简单的线性分配

#### 第二阶段：建立地址映射

`setup_arch()` 中完成架构相关初始化，建立**物理内存到内核虚拟地址的直接映射**，打通内核访问所有物理内存的通道。

```
start_kernel()
  └─ setup_arch()
       └─ arm64_memblock_init()     // 从设备树/ACPI 解析物理内存分布
       └─ paging_init()             // 建立内核页表映射
  └─ mm_init()
       └─ mem_init()                // memblock → 伙伴系统移交
       └─ kmem_cache_init()         // 初始化 SLUB
       └─ pgtable_init()            // 页表初始化
       └─ zonelists_build()         // 构建 zonelist
       └─ page_alloc_init()         // 伙伴系统初始化
       └─ init_per_zone_wmark_min() // 计算水位线
```

#### 第三阶段：核心分配器就绪

| 步骤 | 函数 | 说明 |
|------|------|------|
| 伙伴系统初始化 | `page_alloc_init()` | 物理页框分配器就绪，管理所有物理内存页 |
| SLUB 初始化 | `kmem_cache_init()` | 建立在伙伴系统之上，管理小对象分配 |
| 迁移完成 | `mem_init()` | 释放 memblock 中未使用的内存给伙伴系统 |

**关键转变**：`mem_init()` 函数遍历所有 memblock 中的空闲区域，调用 `free_area_init()` 或 `__free_memory_core()` 将页面移交给伙伴系统，标志着 memblock 使命的完成。

### 3.16 memblock 详细流程分析

#### 3.16.1 核心数据结构（带注释）

```c
// include/linux/memblock.h

// memblock 类型枚举
enum memblock_flags {
    MEMBLOCK_NONE       = 0x0,   // 无特殊标志
    MEMBLOCK_HOTPLUG    = 0x1,   // 支持热插拔的区域
    MEMBLOCK_MIRROR     = 0x2,   // 镜像（redundant）区域
    MEMBLOCK_NOMAP      = 0x4,   // 不建立地址映射的区域
};

// memblock 区域描述：描述一段连续的物理内存
struct memblock_region {
    phys_addr_t    base;          // 物理基地址
    phys_addr_t    size;          // 区域大小
    enum memblock_flags flags;    // 区域标志（NOMAP/HOTPLUG/MIRROR）
#ifdef CONFIG_NUMA
    int            nid;           // NUMA 节点 ID（-1 表示未指定）
#endif
};

// memblock 类型：管理同类型区域的集合
struct memblock_type {
    unsigned long        cnt;         // 当前区域数量
    unsigned long        max;         // 最大可容纳区域数量
    phys_addr_t          total_size;  // 总大小
    struct memblock_region *regions;  // 区域数组指针
    char                 *name;       // 类型名称（"memory"/"reserved"/"physmem"）
};

// memblock 主结构：启动内存分配器的全局状态
struct memblock {
    bool                bottom_up;     // 分配方向：true=从低到高, false=从高到低
    phys_addr_t         current_limit; // 当前分配上限地址
    struct memblock_type memory;       // 可用物理内存区域列表
    struct memblock_type reserved;     // 已预留内存区域列表
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
    struct memblock_type physmem;      // 完整物理内存区域列表（含不可用区域）
#endif
};
```

#### 3.16.2 memblock 初始化完整函数调用栈

```
start_kernel()                          [init/main.c]
  └─ setup_arch()                       [arch/arm64/kernel/setup.c]
       └─ arm64_memblock_init()         [arch/arm64/mm/init.c]
            ├─ memblock_remove()        // 裁剪超出物理地址范围的内存
            ├─ memblock_start_of_DRAM() // 获取 DRAM 起始地址
            ├─ memblock_remove()        // 裁剪超出线性映射区范围的内存
            ├─ memblock_mem_limit_remove_map()  // 应用 memory_limit 限制
            ├─ memblock_add()           // 重新添加内核文本段
            ├─ memblock_reserve()       // 预留内核文本/数据段
            │    └─ memblock_add_range(&reserved, base, size, ...)
            ├─ early_init_fdt_scan_reserved_mem()  // 扫描设备树预留内存
            └─ memblock_dump_all()      // 打印 memblock 状态（bootmem_init 中调用）
       └─ paging_init()                 // 建立页表映射（见 3.11 节）
  └─ mm_init()                          [init/main.c]
       └─ bootmem_init()                [arch/arm64/mm/init.c]
            └─ memblock_dump_all()      // 打印 memblock 状态
       └─ mem_init()                    [arch/arm64/mm/init.c]
            └─ memblock_free_all()      [mm/memblock.c]
                 ├─ free_unused_memmap()     // 释放未使用的 memmap 数组
                 ├─ reset_all_zones_managed_pages()  // 重置各 zone 的 managed_pages
                 ├─ free_low_memory_core_early()     // 核心释放函数
                 │    └─ for_each_free_mem_range(i, ...)
                 │         └─ __free_memory_core(start, end)
                 │              └─ __free_pages_memory(start_pfn, end_pfn)
                 │                   └─ __free_one_page(page, zone, order, migratetype)
                 │                        // 最终将页面加入伙伴系统的空闲链表
                 └─ totalram_pages_add(pages)  // 更新全局内存统计
       └─ kmem_cache_init()             // SLUB 分配器初始化
       └─ page_alloc_init()             // 伙伴系统初始化
```

#### 3.16.3 memblock 三阶段状态转换图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    memblock 生命周期三阶段                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  第一阶段：初始化（arm64_memblock_init）                                   │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │  memblock.memory = [(base1,size1), (base2,size2), ...]         │   │
│  │  memblock.reserved = [(kernel_text,size), (initrd,size), ...]  │   │
│  │  memblock.bottom_up = false (默认从高地址分配)                   │   │
│  │  memblock.current_limit = 物理地址上限                           │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  第二阶段：使用（paging_init / 各子系统初始化）                            │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │  memblock_alloc()   → 分配页表、DMA缓冲区、各种驱动预留内存        │   │
│  │  memblock_reserve() → 预留特定地址范围                            │   │
│  │  memblock_phys_alloc_range() → 指定范围的物理内存分配             │   │
│  │  memblock_add()     → 添加新的内存区域（如设备树中发现的）         │   │
│  │  memblock_remove()  → 移除指定区域                              │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  第三阶段：移交（mem_init → memblock_free_all）                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │  for_each_free_mem_range() 遍历 memblock.memory 中未预留的区域   │   │
│  │       └─ __free_memory_core() → __free_pages_memory()           │   │
│  │            └─ __free_one_page()  → 加入伙伴系统空闲链表          │   │
│  │  memblock.memory 和 memblock.reserved 的 regions 数组被释放     │   │
│  │  memblock_discard() 释放 memblock 内部占用的内存（可选）          │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  最终状态：伙伴系统正式接管物理内存管理                                     │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │  struct zone.free_area[] 包含所有空闲物理页                     │   │
│  │  memblock 退化为只读状态，不再用于分配                            │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

#### 3.16.4 关键函数详细分析

##### arm64_memblock_init — ARM64 架构 memblock 初始化

```c
// arch/arm64/mm/init.c
void __init arm64_memblock_init(void)
{
    // 1. 计算线性映射区域大小
    s64 linear_region_size = PAGE_END - _PAGE_OFFSET(vabits_actual);

    // 2. 裁剪超出物理地址掩码范围的内存（如 52位PA 之外的地址）
    memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);

    // 3. 计算物理内存起始地址（对齐到 ARM64_MEMSTART_ALIGN）
    memstart_addr = round_down(memblock_start_of_DRAM(), ARM64_MEMSTART_ALIGN);

    // 4. 裁剪超出线性映射范围的内存（只保留可映射的部分）
    memblock_remove(max_t(u64, memstart_addr + linear_region_size,
                   __pa_symbol(_end)), ULLONG_MAX);

    // 5. 如果设置了 memory_limit，裁剪多余内存并重新添加内核区域
    if (memory_limit != PHYS_ADDR_MAX) {
        memblock_mem_limit_remove_map(memory_limit);
        memblock_add(__pa_symbol(_text), (resource_size_t)(_end - _text));
    }

    // 6. 预留内核镜像占用的内存
    memblock_reserve(__pa_symbol(_text), _end - _text);

    // 7. 扫描设备树中的预留内存节点
    early_init_fdt_scan_reserved_mem();
}
```

**关键操作说明**：
- **裁剪**：内核线性映射区只能覆盖 `PAGE_OFFSET ~ PAGE_END` 范围的虚拟地址，物理内存超出这个范围的部分无法直接映射，需要裁剪掉
- **预留**：内核代码段、数据段、initrd、页表等必须预留，防止被其他分配覆盖
- **memory_limit**：内核启动参数 `mem=XX` 可以限制可用内存量

##### memblock_add_range — 添加内存区域核心逻辑

```c
// mm/memblock.c
int __init_memblock memblock_add_range(struct memblock_type *type,
                phys_addr_t base, phys_addr_t size,
                int nid, enum memblock_flags flags)
{
    // 1. 计算新区域的结束地址
    phys_addr_t end = base + size;

    // 2. 遍历已有区域，处理重叠/包含关系
    for_each_memblock_type(idx, type, rgn) {
        // 如果新区域完全包含在已有区域中，跳过
        // 如果新区域与已有区域部分重叠，裁剪重叠部分
        // 如果新区域与已有区域相邻，合并它们
        ...
    }

    // 3. 如果 regions 数组空间不足，扩展数组
    //    使用 memblock_double_array() 重新分配

    // 4. 插入新的 region 条目到适当位置
    // 5. 合并相邻或重叠的 region（调用 memblock_merge_regions()）
    memblock_merge_regions(type, start_rgn, end_rgn);
    return 0;
}
```

**区域合并策略**：当两个 region 相邻（`rgn->base + rgn->size == new_base`）且具有相同的 NUMA 节点和标志时，它们会被合并为一个更大的 region，减少碎片化。

##### memblock_free_all — 移交内存给伙伴系统

```c
// mm/memblock.c
void __init memblock_free_all(void)
{
    unsigned long pages;

    // 1. 释放未使用的 struct page 数组占用的内存
    free_unused_memmap();

    // 2. 重置所有 zone 的 managed_pages 计数
    reset_all_zones_managed_pages();

    // 3. 遍历 memblock.memory 中所有未预留的区域
    //    调用 __free_memory_core() 将页面释放给伙伴系统
    pages = free_low_memory_core_early();

    // 4. 更新全局空闲页计数
    totalram_pages_add(pages);
}

// 核心释放函数
static unsigned long __init free_low_memory_core_early(void)
{
    unsigned long count = 0;
    phys_addr_t start, end;

    // 遍历 memblock 中所有空闲区域
    // NUMA_NO_NODE 表示不限定 NUMA 节点
    for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end, NULL)
        count += __free_memory_core(start, end);

    return count;
}

static unsigned long __init __free_memory_core(phys_addr_t start, phys_addr_t end)
{
    unsigned long start_pfn = PFN_UP(start);
    unsigned long end_pfn = PFN_DOWN(end);

    if (start_pfn >= end_pfn)
        return 0;

    // 将连续的物理页范围释放给伙伴系统
    __free_pages_memory(start_pfn, end_pfn);
    return end_pfn - start_pfn;
}
```

**移交的本质**：`memblock_free_all()` 遍历所有 memblock 中标记为 `memory` 且未被 `reserved` 的区域，将每个物理页通过 `__free_one_page()` 放入伙伴系统的 `free_area[]` 链表中。

#### 3.16.5 memblock 分配路径

```
memblock_alloc(size, align)                     [mm/memblock.c]
  └─ memblock_alloc_base(size, align, MEMBLOCK_ALLOC_ACCESSIBLE)
       └─ memblock_alloc_range_nid(size, align, 0, end, NUMA_NO_NODE, flags)
            ├─ memblock_find_in_range_node()    // 查找合适的空闲区域
            │    └─ memblock_search()           // 二分查找 region
            └─ memblock_reserve()                // 标记为已预留
                 └─ memblock_add_range(&reserved, ...)

分配方向控制：
  memblock.bottom_up = false  → 从高地址向下分配（默认，避免碎片化）
  memblock.bottom_up = true   → 从低地址向上分配（一些架构需要）
```

### 3.17 页表管理流程分析

#### 3.17.1 ARM64 页表层级结构

ARM64 架构支持 4KB/16KB/64KB 三种页面粒度，页表级数根据虚拟地址位数动态调整（最多 4 级页表）：

```
虚拟地址位数   页表级数   页粒度
─────────────────────────────────
 48-bit         4 级      4KB
 52-bit (LPA2)  4 级      4KB (使用更大的页表条目)
 48-bit         3 级      64KB
 39-bit         3 级      4KB (ARM64 早期默认)
 36-bit         2 级      64KB
```

4 级页表地址转换层次（以 4KB 页、48-bit VA 为例）：

```
  ┌──────────┬──────────┬──────────┬──────────┬──────────┐
  │  PGD[0]  │  P4D[0]  │  PUD[0]  │  PMD[0]  │  PTE[0]  │  ← 页表条目
  │   (9bit) │   (9bit) │   (9bit) │   (9bit) │   (9bit) │  ← 索引位数
  │  VA[47:39]│ VA[38:30]│ VA[29:21]│ VA[20:12]│ VA[11:0] │  ← 虚拟地址位域
  └──────────┴──────────┴──────────┴──────────┴──────────┘
        │          │          │          │          │
        ▼          ▼          ▼          ▼          ▼
     PGD表      P4D表       PUD表       PMD表      PTE表
     (512项)    (512项)     (512项)     (512项)    (512项)
     每项8B     每项8B      每项8B      每项8B     每项8B
```

**页表级别宏定义**（`arch/arm64/include/asm/pgtable-hwdef.h`）：

| 宏 | 值 | 说明 |
|----|-----|------|
| `PAGE_SHIFT` | 12 (4KB) | 页面偏移位数 |
| `PAGE_SIZE` | 4096 | 页面大小 |
| `PMD_SHIFT` | 21 (4KB) | PMD 覆盖的地址位数（2MB 块） |
| `PUD_SHIFT` | 30 (4KB) | PUD 覆盖的地址位数（1GB 块） |
| `PGDIR_SHIFT` | 39 (4KB, 3级) / 48 (4KB, 4级) | PGD 覆盖的地址位数 |
| `PTRS_PER_PTE` | 512 | PTE 表项数 |
| `PTRS_PER_PMD` | 512 | PMD 表项数 |
| `PTRS_PER_PUD` | 512 | PUD 表项数 |
| `PTRS_PER_P4D` | 512 | P4D 表项数 |
| `PTRS_PER_PGD` | 512 | PGD 表项数 |

#### 3.17.2 页表初始化完整函数调用栈

```
start_kernel()                              [init/main.c]
  └─ setup_arch()                           [arch/arm64/kernel/setup.c]
       └─ paging_init()                     [arch/arm64/mm/mmu.c]
            ├─ map_mem(swapper_pg_dir)      // 建立线性映射（直接映射区）
            │    │
            │    ├─ 1. 标记内核文本段为 NOMAP（临时跳过映射）
            │    │    memblock_mark_nomap(kernel_start, kernel_end - kernel_start)
            │    │
            │    ├─ 2. 遍历所有内存 bank，建立线性映射
            │    │    for_each_mem_range(i, &start, &end)
            │    │         └─ __map_memblock(pgdp, start, end, PAGE_KERNEL, flags)
            │    │              └─ early_create_pgd_mapping(pgdp, phys, virt, size, prot, alloc, flags)
            │    │                   └─ __create_pgd_mapping(pgdir, phys, virt, size, prot, alloc, flags)
            │    │                        └─ __create_pgd_mapping_locked()
            │    │                             └─ alloc_init_p4d(pgdp, addr, next, phys, prot, alloc, flags)
            │    │                                  └─ [若 PGD 为空] 分配 P4D 页表页
            │    │                                       pgtable_alloc(TABLE_P4D)
            │    │                                       └─ memblock_phys_alloc_range(PAGE_SIZE, ...)
            │    │                                  └─ alloc_init_pud(p4dp, addr, next, phys, prot, alloc, flags)
            │    │                                       └─ [若 P4D 为空] 分配 PUD 页表页
            │    │                                            pgtable_alloc(TABLE_PUD)
            │    │                                            └─ memblock_phys_alloc_range(PAGE_SIZE, ...)
            │    │                                       └─ alloc_init_cont_pmd(pudp, addr, next, phys, prot, alloc, flags)
            │    │                                            └─ [若 PUD 为空] 分配 PMD 页表页
            │    │                                                 pgtable_alloc(TABLE_PMD)
            │    │                                                 └─ memblock_phys_alloc_range(PAGE_SIZE, ...)
            │    │                                            └─ alloc_init_cont_pte(pmdp, addr, next, phys, prot, alloc, flags)
            │    │                                                 └─ [若 PMD 为空] 分配 PTE 页表页
            │    │                                                      pgtable_alloc(TABLE_PTE)
            │    │                                                      └─ memblock_phys_alloc_range(PAGE_SIZE, ...)
            │    │                                                 └─ init_pte(ptep, addr, next, phys, prot)
            │    │                                                      // 设置 PTE 条目，完成最终映射
            │    │
            │    ├─ 3. 映射内核文本段别名（非执行、非连续映射）
            │    │    __map_memblock(pgdp, kernel_start, kernel_end, PAGE_KERNEL, NO_CONT_MAPPINGS)
            │    │
            │    ├─ 4. 清除内核文本段的 NOMAP 标志
            │    │    memblock_clear_nomap(kernel_start, kernel_end - kernel_start)
            │    │
            │    └─ 5. 映射 KFENCE 内存池（如果启用了 KFENCE 早期初始化）
            │         arm64_kfence_map_pool(early_kfence_pool, pgdp)
            │
            ├─ memblock_allow_resize()       // 允许 memblock 重新分配 regions 数组
            │
            ├─ create_idmap()                // 创建恒等映射（identity map）
            │    └─ 建立 __cpu_setup 等早期启动代码的恒等映射
            │
            └─ declare_kernel_vmas()         // 声明内核各段的 VMA 信息
                 └─ 用于 /proc/vmallocinfo 等调试接口
```

#### 3.17.3 页表创建流程详细图

```
        物理内存                   虚拟地址空间 (线性映射区)
   ┌────────────────┐        ┌──────────────────────────────────┐
   │ 物理页框 0      │        │  PAGE_OFFSET                    │
   │  ...            │        │      │                          │
   │ 物理页框 N      │◄───────│      ▼                          │
   │  (DRAM)         │  映射  │  virt = PAGE_OFFSET + phys      │
   └────────────────┘        │                                  │
                             │  ┌────────────────────────────┐  │
建立映射的入口：              │  │  swapper_pg_dir (PGD)      │  │
                             │  │  ┌──────────────────┐      │  │
early_create_pgd_mapping()   │  │  │ PGD[0] → P4D表   │      │  │
  └─ __create_pgd_mapping()  │  │  │ PGD[1] → P4D表   │      │  │
       └─ alloc_init_p4d()   │  │  │ ...              │      │  │
            ├─ 分配 P4D 表   │  │  └──────────────────┘      │  │
            └─ alloc_init_pud()  └────────────────────────────┘  │
                 ├─ 分配 PUD 表        │                          │
                 └─ alloc_init_pmd()   │ P4D 表                   │
                      ├─ 分配 PMD 表   │ ┌──────────────────┐     │
                      │               │ │ P4D[0] → PUD表   │     │
                      │ 大块映射检查： │ │ P4D[1] → PUD表   │     │
                      │ ┌───────────┐ │ │ ...              │     │
                      │ │ 如果 PMD   │ │ └──────────────────┘     │
                      │ │ 覆盖范围   │        │                    │
                      │ │ 对齐且无   │        │ PUD 表            │
                      │ │ 大块映射   │        │ ┌────────────┐    │
                      │ │ 禁止标志 → │        │ │ PUD[0] →   │    │
                      │ │ 使用 PMD   │        │ │   PMD表    │    │
                      │ │ 块映射     │        │ │ PUD[1] →   │    │
                      │ │ (2MB)      │        │ │   PMD表    │    │
                      │ └───────────┘        │ │ ...        │    │
                      │                      │ └────────────┘    │
                      │ 否则 → 分配 PTE 表         │              │
                      └─ alloc_init_pte()    │ PMD 表            │
                           └─ init_pte()     │ ┌────────────┐    │
                                // 设置      │ │ PMD[0] →   │    │
                                PTE 条目     │ │   PTE表    │    │
                                // 映射到    │ │ PMD[1] →   │    │
                                // 物理页    │ │   PTE表    │    │
                                             │ │ ...        │    │
                                             │ └────────────┘    │
                                             │        │           │
                                             │ PTE 表            │
                                             │ ┌────────────┐    │
                                             │ │ PTE[0] →   │    │
                                             │ │   物理页X  │    │
                                             │ │ PTE[1] →   │    │
                                             │ │   物理页Y  │    │
                                             │ │ ...        │    │
                                             │ └────────────┘    │
                                             └──────────────────┘
```

#### 3.17.4 关键函数详细分析

##### paging_init — 页表初始化入口

```c
// arch/arm64/mm/mmu.c
void __init paging_init(void)
{
    // 1. 建立线性映射（直接映射区）
    //    swapper_pg_dir 是内核主页表（全局页表）
    //    将所有物理内存映射到 PAGE_OFFSET 开始的虚拟地址空间
    map_mem(swapper_pg_dir);

    // 2. 允许 memblock 在后续操作中重新分配 regions 数组
    //    paging_init 之后，memblock 的 regions 数组可能需要扩展
    memblock_allow_resize();

    // 3. 创建恒等映射（identity map）
    //    用于 __cpu_setup 等需要在 MMU 开启时以物理地址执行的代码
    create_idmap();

    // 4. 声明内核各段的 VMA 信息（用于调试/统计）
    declare_kernel_vmas();
}
```

##### map_mem — 建立线性映射

```c
// arch/arm64/mm/mmu.c
static void __init map_mem(pgd_t *pgdp)
{
    phys_addr_t kernel_start = __pa_symbol(_text);
    phys_addr_t kernel_end = __pa_symbol(__init_begin);
    phys_addr_t start, end;
    int flags = NO_EXEC_MAPPINGS;  // 线性映射区默认不可执行

    // 检查是否强制使用页级映射（PTE 粒度，用于调试/安全）
    if (force_pte_mapping())
        flags |= NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS;

    // 临时将内核文本段标记为 NOMAP，避免在循环中建立映射
    // 内核文本段需要特殊处理（先标记为不可写，后续再恢复）
    memblock_mark_nomap(kernel_start, kernel_end - kernel_start);

    // 遍历所有内存 bank，建立物理地址 → 虚拟地址的线性映射
    // for_each_mem_range 遍历 memblock.memory 中所有非 NOMAP 的区域
    for_each_mem_range(i, &start, &end) {
        if (start >= end)
            break;
        __map_memblock(pgdp, start, end,
                       pgprot_tagged(PAGE_KERNEL), flags);
    }

    // 单独映射内核文本段（非连续映射，避免后续 remap 问题）
    __map_memblock(pgdp, kernel_start, kernel_end,
                   PAGE_KERNEL, NO_CONT_MAPPINGS);
    // 清除 NOMAP 标志
    memblock_clear_nomap(kernel_start, kernel_end - kernel_start);
}
```

**设计要点**：
- **NO_EXEC_MAPPINGS**：线性映射区不需要执行代码，设置不可执行（PXN）提高安全性
- **NO_BLOCK_MAPPINGS**：强制使用页级映射（PTE），不创建 PMD/PUD 块映射，用于调试
- **NO_CONT_MAPPINGS**：禁止使用连续映射（PTE_CONT），便于后续修改属性
- **NOMAP 技巧**：临时标记内核文本段为 NOMAP，避免在 `for_each_mem_range` 循环中被映射，以便单独控制其映射属性

##### alloc_init_p4d/pud/pmd/pte — 页表层级分配

```c
// arch/arm64/mm/mmu.c

// P4D 级：处理 PGD 到 P4D 的映射
static int alloc_init_p4d(pgd_t *pgdp, unsigned long addr, unsigned long end,
                          phys_addr_t phys, pgprot_t prot,
                          phys_addr_t (*pgtable_alloc)(enum pgtable_type),
                          int flags)
{
    pgd_t pgd = READ_ONCE(*pgdp);

    if (pgd_none(pgd)) {
        // PGD 条目为空，需要分配新的 P4D 页表页
        // 从 memblock 分配一页物理内存作为 P4D 表
        p4d_phys = pgtable_alloc(TABLE_P4D);
        // 初始化 P4D 表（清零）
        p4dp = p4d_set_fixmap(p4d_phys);
        init_clear_pgtable(p4dp);
        // 将 P4D 表的物理地址填入 PGD 条目
        __pgd_populate(pgdp, p4d_phys, pgdval);
    } else {
        // PGD 已存在，获取 P4D 表的虚拟地址
        p4dp = p4d_set_fixmap_offset(pgdp, addr);
    }

    // 遍历当前 P4D 覆盖的地址范围，递归到下一级
    do {
        next = p4d_addr_end(addr, end);
        ret = alloc_init_pud(p4dp, addr, next, phys, prot, pgtable_alloc, flags);
        phys += next - addr;
    } while (p4dp++, addr = next, addr != end);
}

// PUD 级：处理 P4D 到 PUD 的映射，尝试 1GB 块映射
static int alloc_init_pud(p4d_t *p4dp, unsigned long addr, unsigned long end,
                          phys_addr_t phys, pgprot_t prot, ...)
{
    // 如果 P4D 为空，分配 PUD 页表页
    // ...

    do {
        next = pud_addr_end(addr, end);

        // 尝试 1GB 块映射（PUD 级大页）
        // 条件：支持 PUD 块映射 + 地址对齐 + 未禁止块映射
        if (pud_sect_supported() &&
            ((addr | next | phys) & ~PUD_MASK) == 0 &&
            (flags & NO_BLOCK_MAPPINGS) == 0) {
            // 直接设置 PUD 条目为 1GB 块映射
            pud_set_huge(pudp, phys, prot);
        } else {
            // 无法使用块映射，递归到 PMD 级
            ret = alloc_init_cont_pmd(pudp, addr, next, phys, prot, ...);
        }
        phys += next - addr;
    } while (pudp++, addr = next, addr != end);
}

// PMD 级：处理 PUD 到 PMD 的映射，尝试 2MB 块映射
// 与 PUD 级类似，尝试 2MB 块映射（pmd_sect），否则递归到 PTE 级

// PTE 级：建立最终的页级映射
static int alloc_init_cont_pte(pmd_t *pmdp, unsigned long addr, ...)
{
    // 如果 PMD 为空，分配 PTE 页表页
    // ...

    do {
        next = pte_cont_addr_end(addr, end);

        // 尝试连续映射（PTE_CONT，16 个连续 PTE 合并）
        if ((((addr | next | phys) & ~CONT_PTE_MASK) == 0) &&
            (flags & NO_CONT_MAPPINGS) == 0)
            __prot = __pgprot(pgprot_val(prot) | PTE_CONT);

        // 设置 PTE 条目，完成物理地址到虚拟地址的映射
        init_pte(ptep, addr, next, phys, __prot);

        phys += next - addr;
    } while (addr = next, addr != end);
}
```

**页表分配策略**：
- **块映射优化**：如果地址范围对齐，优先使用 PUD（1GB）或 PMD（2MB）块映射，减少页表级数和 TLB 压力
- **连续映射优化**：在 PTE 级，如果 16 个连续 PTE 对齐，使用 PTE_CONT 合并
- **惰性分配**：页表页只有在需要时才从 memblock 分配，不会预先分配所有级数的页表

#### 3.17.5 页表分配与块映射决策流程图

```
early_create_pgd_mapping(phys, virt, size, prot, alloc, flags)
         │
         ▼
  __create_pgd_mapping()
         │
         ▼
  for each PGD entry covering [virt, virt+size):
         │
         ▼
    alloc_init_p4d(pgdp, addr, next, ...)
         │
    ┌────┴────┐
    │ PGD 为空 │──→ 分配 P4D 页表页
    └────┬────┘    (memblock_phys_alloc_range)
         │
         ▼
    alloc_init_pud(p4dp, addr, next, ...)
         │
    ┌────┴────────────────────────────┐
    │ P4D 为空 │──→ 分配 PUD 页表页
    └────┬────────────────────────────┘
         │
         ▼
    ┌─────────────────────────────────────┐
    │ 地址 1GB 对齐且 NO_BLOCK_MAPPINGS=0 │
    └──────────┬──────────────────────────┘
         是／否
       ┌─┴──┐
      ┌┘    │
      ▼     │                           ← 1GB 块映射（PUD 级大页）
  设置 PUD   │
  条目为      │
  1GB 块     │
             ▼
      alloc_init_cont_pmd(pudp, addr, next, ...)
             │
        ┌────┴────────────────────────────┐
        │ PUD 为空 │──→ 分配 PMD 页表页
        └────┬────────────────────────────┘
             │
             ▼
        ┌─────────────────────────────────────┐
        │ 地址 2MB 对齐且 NO_BLOCK_MAPPINGS=0 │
        └──────────┬──────────────────────────┘
             是／否
           ┌─┴──┐
          ┌┘    │
          ▼     │                           ← 2MB 块映射（PMD 级大页）
      设置 PMD   │
      条目为      │
      2MB 块     │
                 ▼
          alloc_init_cont_pte(pmdp, addr, next, ...)
                 │
            ┌────┴────────────────────────────┐
            │ PMD 为空 │──→ 分配 PTE 页表页
            └────┬────────────────────────────┘
                 │
                 ▼
                 init_pte()                  ← 4KB 页级映射
                 设置 PTE 条目
                 物理页 → 虚拟页
```

#### 3.17.6 关键数据结构

```c
// arch/arm64/include/asm/pgtable-types.h

// 页表条目类型（每个条目 8 字节）
typedef struct { pteval_t pte; }     pte_t;   // PTE 条目
typedef struct { pmdval_t pmd; }     pmd_t;   // PMD 条目
typedef struct { pudval_t pud; }     pud_t;   // PUD 条目
typedef struct { p4dval_t p4d; }     p4d_t;   // P4D 条目
typedef struct { pgdval_t pgd; }     pgd_t;   // PGD 条目

// 内核主页表（全局页表，所有进程共享内核映射部分）
// arch/arm64/include/asm/mmu.h
extern pgd_t swapper_pg_dir[];       // 内核主页表

// 恒等映射页表（用于 MMU 开启时的过渡）
extern pgd_t idmap_pg_dir[];
```

**PTE 条目格式**（ARM64 4KB 页）：

```
63      54  53    51  50        12  11      10  9  8  7  6  5  4  3  2  1  0
┌─────────┬──────┬─────────────┬──────────┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
│  Upper  │  PFN │   RES0      │  SW      │PB│UA│NG│AF│SH│AP│AP│NS│PX│  │  │
│  Attrs  │      │             │  (bits)  │  │  │  │  │  │[2]│[1]│  │N │D │V │
│ XN:PXN  │      │             │          │  │  │  │  │  │   │   │  │  │  │  │
└─────────┴──────┴─────────────┴──────────┴──┴──┴──┴──┴──┴───┴───┴──┴──┴──┴──┘
  bit[0]  V      = 有效位 (Valid)
  bit[1]  D      = 脏页标志 (Dirty)
  bit[2]  PXN    = 特权执行禁止 (Privileged eXecute Never)
  bit[5]  NS     = 非安全 (Non-Secure)
  bit[6:7] AP[1:0] = 访问权限 (Access Permission)
  bit[8]  AP[2]  = 访问权限扩展
  bit[9]  SH     = 可共享属性 (Shareability)
  bit[10] AF     = 访问标志 (Access Flag)
  bit[11] NG     = 非全局页 (Not Global)
  bit[12] UA     = 用户空间访问 (User Accessible)
  bit[53] PBHA   = 页属性提示 (Page Based Hardware Attributes)
  bit[54] XN     = 执行禁止 (eXecute Never)
  bit[55:62] Upper Attrs = 保留/扩展属性
```

#### 3.17.7 内核页表布局

```
虚拟地址空间布局（ARM64, 48-bit VA, 4KB 页）:

  0x0000_0000_0000_0000
  ┌──────────────────────┐
  │  用户空间            │  TTBR0 页表（每个进程独立）
  │  (0 ~ 0x0000_FFFF_   │
  │   FFFF_FFFF)         │
  │  256TB               │
  └──────────────────────┘
  0xFFFF_0000_0000_0000
  ┌──────────────────────┐
  │  内核空间            │  TTBR1 页表（swapper_pg_dir）
  │                      │
  │  ┌────────────────┐  │
  │  │ 线性映射区      │  │  PAGE_OFFSET = 0xFFFF_8000_0000_0000
  │  │ (直接映射)      │  │  → 物理内存的直接映射
  │  │ phys→virt        │  │  → 大小取决于 VA_BITS
  │  │ virt = phys +    │  │
  │  │  PAGE_OFFSET     │  │
  │  └────────────────┘  │
  │  ┌────────────────┐  │
  │  │ vmalloc 区      │  │  VMALLOC_START
  │  │ (非连续映射)    │  │
  │  └────────────────┘  │
  │  ┌────────────────┐  │
  │  │ 固定映射区      │  │  FIXADDR_START
  │  │ (fixmap)        │  │
  │  └────────────────┘  │
  │  ┌────────────────┐  │
  │  │ PCI I/O 区      │  │  PCI_IO_START
  │  └────────────────┘  │
  │  ┌────────────────┐  │
  │  │ 模块区          │  │  MODULES_VADDR
  │  └────────────────┘  │
  └──────────────────────┘
  0xFFFF_FFFF_FFFF_FFFF

swapper_pg_dir 包含：
  ─ 线性映射区（map_mem 建立）：覆盖所有物理内存
  ─ vmalloc 区映射（动态建立）
  ─ 固定映射区（fixmap）
  ─ 模块区映射
  ─ 内核镜像映射（包含文本、数据、init 段等）
```

#### 3.17.8 页表管理总结

| 阶段 | 函数 | 功能 | 页表分配器 |
|------|------|------|-----------|
| 早期页表 | `__create_pgd_mapping` | 建立内核线性映射 | `memblock_phys_alloc_range` |
| 恒等映射 | `create_idmap` | 建立 MMU 开启时的过渡映射 | memblock |
| 运行时页表 | `handle_mm_fault` | 缺页处理，用户态页表 | 伙伴系统 + slab |
| 模块映射 | `module_alloc` | 内核模块加载 | vmalloc |
| 大页映射 | `alloc_init_pud`/`alloc_init_pmd` | 1GB/2MB 块映射 | memblock（早期）/ 伙伴系统（后期） |

**关键设计要点**：
1. **双重映射**：内核文本段既在线性映射区中，也有独立的映射，前者用于线性访问，后者用于执行
2. **块映射优化**：PUD 级（1GB）和 PMD 级（2MB）块映射减少 TLB 缺失和页表级数
3. **惰性分配**：页表页只在需要时分配，不会预先建立所有映射
4. **swapper_pg_dir**：内核主页表，所有进程的页表都共享内核部分（通过 `fork()` 复制或共享）

---

## 4. SLUB 分配器与 Sheaves 缓存机制

### 4.1 概述

文件：`mm/slub.c`（6,500+ 行），`mm/slab_common.c`（2,219 行），`include/linux/slab.h`

SLUB 是 Linux 内核默认的 slab 分配器，用于管理小对象（如 `task_struct`、`inode`、`dentry` 等）的内存分配。它替代了早期的 SLAB 分配器，具有更简洁的设计和更好的 NUMA 可扩展性。

**核心创新（Linux 7.0+）**：SLUB 引入了 **Sheaves 缓存机制**，用双数组（main/spare）的 Per-CPU 对象缓存替代了传统的 `cpu_slab->freelist` 单链表，并引入 **NUMA 节点共享的 barn** 来平衡 CPU 之间的对象缓存。这一设计大幅减少了锁竞争，使热路径（fast path）几乎完全无锁。

### 4.2 锁顺序

```c
// mm/slub.c 锁顺序定义
0.  cpu_hotplug_lock                    // CPU 热插拔全局锁
1.  slab_mutex                          // 全局互斥锁，保护缓存列表和元数据变更
2a. kmem_cache->cpu_sheaves->lock       // Per-CPU trylock（本地锁，非 RT 仅关抢占）
2b. node->barn->lock                    // 节点 barn 自旋锁
2c. node->list_lock                     // 节点 partial/full 链表自旋锁
3.  slab_lock(slab)                     // Slab 位自旋锁（仅部分架构需要）
4.  object_map_lock                     // 仅 debug 时使用
```

### 4.3 核心数据结构（带详细注释）

#### 4.3.1 struct kmem_cache — 缓存描述符

```c
// mm/slab.h
struct kmem_cache {
    struct slub_percpu_sheaves __percpu *cpu_sheaves;  // Per-CPU sheaves 数组
    slab_flags_t flags;                  // SLAB_* 标志位
    unsigned long min_partial;           // 节点 partial 链表最小 slab 数
    unsigned int size;                   // 对象大小（含元数据）
    unsigned int object_size;            // 对象大小（不含元数据）
    struct reciprocal_value reciprocal_size;  // 用于快速除法
    unsigned int offset;                 // 空闲指针在对象内的偏移量
    unsigned int sheaf_capacity;         // 每个 sheaf 可容纳的对象数（0=不使用 sheaves）

    struct kmem_cache_order_objects oo;  // 最优 slab 大小（order 和对象数）
    struct kmem_cache_order_objects min; // 最小 slab 大小
    gfp_t allocflags;                    // 从伙伴系统分配页面时使用的 GFP 标志
    int refcount;                        // 引用计数（用于销毁）
    void (*ctor)(void *object);          // 对象构造函数
    unsigned int inuse;                  // 元数据偏移量
    unsigned int align;                  // 对齐要求
    unsigned int red_left_pad;           // 左侧红区填充大小
    const char *name;                    // 缓存名称（用于 /proc/slabinfo）
    struct list_head list;               // 全局缓存链表

#ifdef CONFIG_NUMA
    unsigned int remote_node_defrag_ratio; // 远程节点分配的去碎片化比例
#endif

    struct kmem_cache_node *node[MAX_NUMNODES]; // Per-NUMA-节点数据
};
```

#### 4.3.2 struct slub_percpu_sheaves — Per-CPU Sheaves

```c
// mm/slub.c
struct slub_percpu_sheaves {
    local_trylock_t lock;           // 本地 trylock（非 RT 仅关抢占，无原子操作）
    struct slab_sheaf *main;        // 主 sheaf，永不为 NULL（未锁定时）
    struct slab_sheaf *spare;       // 备用 sheaf，可为 NULL（空或满）
    struct slab_sheaf *rcu_free;    // 用于批量处理 kfree_rcu() 的 sheaf
};
```

#### 4.3.3 struct slab_sheaf — 对象数组

```c
// mm/slub.c
struct slab_sheaf {
    union {
        struct rcu_head rcu_head;       // RCU 回收入口
        struct list_head barn_list;     // 挂入 barn 链表的节点
        struct {
            unsigned int capacity;      // 预填充 sheaf 的容量
            bool pfmemalloc;            // 是否来自 PF_MEMALLOC 预留
        };
    };
    struct kmem_cache *cache;           // 所属缓存
    unsigned int size;                  // 当前有效对象数
    int node;                           // 仅 rcu_sheaf 使用
    void *objects[];                    // 对象指针数组（柔性数组）
};
```

#### 4.3.4 struct node_barn — 节点级共享缓存

```c
// mm/slub.c
struct node_barn {
    spinlock_t lock;                    // 保护 barn 的自旋锁
    struct list_head sheaves_full;      // 满 sheaf 链表（可被 CPU 取走）
    struct list_head sheaves_empty;     // 空 sheaf 链表（可被 CPU 取走）
    unsigned int nr_full;               // 满 sheaf 数量
    unsigned int nr_empty;              // 空 sheaf 数量
};
```

**barn 设计要点**：当 CPU 的主 sheaf 变空时，尝试从 barn 获取一个满 sheaf 替换；当主 sheaf 变满时，尝试将满 sheaf 放回 barn 换取空 sheaf。这避免了直接访问伙伴系统或 partial 链表，大幅降低延迟。barn 的上限由 `MAX_FULL_SHEAVES`（10）和 `MAX_EMPTY_SHEAVES`（10）控制。

#### 4.3.5 struct kmem_cache_node — 节点级 slab 管理

```c
// mm/slub.c
struct kmem_cache_node {
    spinlock_t list_lock;               // 保护 partial/full 链表的自旋锁
    unsigned long nr_partial;            // partial slab 数量
    struct list_head partial;           // 部分空闲的 slab 链表
#ifdef CONFIG_SLUB_DEBUG
    atomic_long_t nr_slabs;             // slab 总数
    atomic_long_t total_objects;        // 对象总数
    struct list_head full;              // 满 slab 链表（仅 debug）
#endif
    struct node_barn *barn;             // 节点级 sheaf 缓存
};
```

#### 4.3.6 struct slab — Slab 页描述符

```c
// include/linux/slab.h (部分字段)
struct slab {
    struct kmem_cache *slab_cache;      // 所属 kmem_cache
    void *freelist;                     // 空闲对象链表（用于无 sheaves 的慢速路径）
    unsigned long counters;             // 包含：inuse（使用中对象数）、objects（总对象数）、frozen（冻结标志）
    struct list_head partial;           // 挂入节点 partial 链表的节点
    struct list_head full;              // 仅 debug 时使用
};
```

### 4.4 Sheaves 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    SLUB Sheaves 架构                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  CPU 0                     CPU 1                     CPU N      │
│  ┌─────────────────┐      ┌─────────────────┐      ┌─────────┐ │
│  │ cpu_sheaves:    │      │ cpu_sheaves:    │      │  ...    │ │
│  │  ┌─ main (满)  │      │  ┌─ main (空)  │      │         │ │
│  │  ├─ spare (空) │      │  ├─ spare (满) │      │         │ │
│  │  └─ rcu_free   │      │  └─ rcu_free   │      │         │ │
│  └─────────────────┘      └─────────────────┘      └─────────┘ │
│           │                          │                          │
│           ▼                          ▼                          │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │          Node Barn (Per-NUMA-Node)                       │    │
│  │  ┌─────────────────┐  ┌─────────────────┐               │    │
│  │  │ sheaves_full    │  │ sheaves_empty   │               │    │
│  │  │ (最多 10 个)    │  │ (最多 10 个)    │               │    │
│  │  └─────────────────┘  └─────────────────┘               │    │
│  └─────────────────────────────────────────────────────────┘    │
│                          │                                      │
│                          ▼                                      │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │          Partial Slab 链表 (kmem_cache_node)             │    │
│  │          (部分空闲的 slab 页，有至少一个空闲对象)          │    │
│  └─────────────────────────────────────────────────────────┘    │
│                          │                                      │
│                          ▼                                      │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │          伙伴系统 (Buddy System)                          │    │
│  │          (分配新 slab 页或释放全空 slab 页)                │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**Sheaves 工作流程**：

| 事件 | 操作 |
|------|------|
| **分配时 main 非空** | 直接从 main sheaf 取对象（`alloc_from_pcs`） |
| **分配时 main 为空** | 尝试用 spare 交换；若 spare 也为空，从 barn 取满 sheaf；若 barn 无满 sheaf，从 partial 链表批量分配填充新 sheaf |
| **释放时 main 未满** | 直接放入 main sheaf（`free_to_pcs`） |
| **释放时 main 已满** | 尝试用 spare 交换；若 spare 也为满，将满 sheaf 放入 barn 换空 sheaf；若 barn 已满，flush 一个满 sheaf 到 partial 链表 |
| **RCU 释放** | 放入 `rcu_free` sheaf，累积到一定量后批量处理 |

### 4.5 完整分配函数调用栈

```
kmem_cache_alloc_noprof(s, gfpflags)                [mm/slub.c]
  └─ slab_alloc_node(s, NULL, gfpflags, NUMA_NO_NODE, ...)
       │
       ├─ 1. slab_pre_alloc_hook()                  // 检查 should_failslab
       │
       ├─ 2. kfence_alloc()                         // KFENCE 错误检测（如有）
       │
       ├─ 3. alloc_from_pcs(s, gfpflags, node)      // ★ 快速路径：从 Per-CPU Sheaves 分配
       │    │
       │    ├─ local_trylock(&s->cpu_sheaves->lock)  // 本地 trylock（关抢占）
       │    ├─ if main->size == 0:
       │    │    └─ __pcs_replace_empty_main()       // 替换空 main sheaf
       │    │         ├─ 尝试用 spare 交换
       │    │         ├─ 尝试从 barn 取满 sheaf（barn_replace_empty_sheaf）
       │    │         ├─ 尝试从 barn 分配空 sheaf 并填充（refill_sheaf）
       │    │         └─ 尝试分配新 sheaf（alloc_full_sheaf）
       │    ├─ object = main->objects[main->size - 1] // 取尾部对象
       │    ├─ main->size--
       │    └─ local_unlock()
       │
       └─ 4. 若 alloc_from_pcs 失败:
            __slab_alloc_node(s, gfpflags, node, ...) // ★ 慢速路径
            └─ ___slab_alloc(s, gfpflags, node, ...)
                 │
                 ├─ get_from_partial()               // 从节点 partial 链表取 slab
                 │    └─ 遍历 partial 链表，取 slab 并分配对象
                 │
                 └─ 若 partial 为空:
                      new_slab(s, flags, node)        // 从伙伴系统分配新 slab 页
                      └─ alloc_slab_page()            // 调用 alloc_pages()
                      └─ alloc_single_from_new_slab() // 或 alloc_from_new_slab() 批量分配

                                                      // 初始化 slab 元数据
                                                      // 调用构造函数（如有）
```

### 4.6 完整释放函数调用栈

```
kmem_cache_free(s, x)                               [mm/slub.c]
  └─ slab_free(s, slab, x, _RET_IP_)
       │
       ├─ 1. memcg_slab_free_hook()                 // memcg 记账
       ├─ 2. alloc_tagging_slab_free_hook()          // 分配标签
       ├─ 3. slab_free_hook()                        // KASAN/KMSAN 等
       │
       ├─ 4. 若本地节点且非 PF_MEMALLOC:
       │    free_to_pcs(s, object, true)             // ★ 快速路径：释放到 Per-CPU Sheaves
       │    │
       │    ├─ local_trylock(&s->cpu_sheaves->lock)
       │    ├─ if main->size == capacity:
       │    │    └─ __pcs_replace_full_main()        // 替换满 main sheaf
       │    │         ├─ 尝试用 spare 交换
       │    │         ├─ 尝试从 barn 取空 sheaf（barn_get_empty_sheaf）
       │    │         ├─ 尝试 barn_replace_full_sheaf（放满取空）
       │    │         └─ 尝试分配空 sheaf 或 flush 满 sheaf
       │    ├─ main->objects[main->size++] = object
       │    └─ local_unlock()
       │
       └─ 5. 若 free_to_pcs 失败或远程节点:
            __slab_free(s, slab, object, ...)        // ★ 慢速路径：释放到 slab
            │
            ├─ cmpxchg_double 更新 slab->freelist    // 无锁更新空闲链表
            │    └─ 若 slab 从满变 partial:
            │         add_partial()                  // 加入节点 partial 链表
            │    └─ 若 slab 变空:
            │         remove_partial() + 释放回伙伴系统
            │
            └─ 若 cmpxchg 失败（架构不支持）:
                 slab_lock() + 更新 + slab_unlock()
```

### 4.7 kmalloc/kfree API 详细分析

#### 4.7.1 架构概述

`kmalloc` 是 Linux 内核中最常用的动态内存分配接口，底层基于 SLUB 分配器实现。其核心设计如下：

```
kmalloc(size, flags)
  │
  ├─ size > KMALLOC_MAX_CACHE_SIZE (= 2 * PAGE_SIZE)  ← 大对象
  │    └─ 直接走伙伴系统：alloc_frozen_pages_noprof()
  │
  └─ size <= KMALLOC_MAX_CACHE_SIZE                    ← 小对象
       └─ 通过 kmalloc_slab() 查找合适的 kmem_cache
            └─ 走 SLUB 分配路径（Sheaves 缓存）
```

**关键常量**：
- `KMALLOC_SHIFT_HIGH = PAGE_SHIFT + 1`（例如 4K 页 → 13，即 8KB）
- `KMALLOC_MAX_CACHE_SIZE = 1UL << KMALLOC_SHIFT_HIGH`（即 2 页大小）
- 阈值以上的分配退化为伙伴系统的页分配器

#### 4.7.2 核心数据结构

```c
// include/linux/slab.h

// 按 2^N 对齐的 kmem_cache 指针数组，每个桶对应一种对齐大小
typedef struct kmem_cache * kmem_buckets[KMALLOC_SHIFT_HIGH + 1];

// 全局 kmalloc 缓存数组，按类型分组
// kmalloc_caches[KMALLOC_NORMAL]   — 普通分配
// kmalloc_caches[KMALLOC_RECLAIM]  — __GFP_RECLAIMABLE 可回收分配
// kmalloc_caches[KMALLOC_DMA]      — __GFP_DMA DMA 区域分配
// kmalloc_caches[KMALLOC_CGROUP]   — __GFP_ACCOUNT 内存计费分配
extern kmem_buckets kmalloc_caches[NR_KMALLOC_TYPES];
```

**kmalloc 类型枚举**（`enum kmalloc_cache_type`）：

| 类型 | 触发条件 | 用途 |
|------|----------|------|
| `KMALLOC_NORMAL` | 无特殊 GFP 标志 | 默认普通分配 |
| `KMALLOC_RECLAIM` | `__GFP_RECLAIMABLE` | 可回收缓存（如 inode/dentry） |
| `KMALLOC_DMA` | `__GFP_DMA` | DMA 区域内存 |
| `KMALLOC_CGROUP` | `__GFP_ACCOUNT` | 内存 cgroup 计费 |

`kmalloc_type()` 函数根据 GFP 标志位选择类型，优先级：`__GFP_DMA` > `__GFP_RECLAIMABLE` > `__GFP_ACCOUNT`。

#### 4.7.3 完整分配函数调用栈

```
__kmalloc_noprof(size, flags)                            [mm/slub.c]
  └─ __do_kmalloc_node(size, b, flags, NUMA_NO_NODE, caller)  [mm/slub.c]
       │
       ├─ [大对象路径] size > KMALLOC_MAX_CACHE_SIZE
       │    └─ __kmalloc_large_node_noprof(size, flags, node)
       │         └─ ___kmalloc_large_node(size, flags, node)
       │              ├─ get_order(size)                     // 计算需要的 2^N order
       │              ├─ alloc_frozen_pages_noprof(flags, order)  // 伙伴系统分配
       │              │    └─ __alloc_frozen_pages_noprof()  // 同 3.5 节
       │              ├─ page_address(page)                  // 获取线性映射地址
       │              ├─ mod_lruvec_page_state(NR_SLAB_UNRECLAIMABLE_B, +)
       │              ├─ __SetPageLargeKmalloc(page)         // 标记为大 kmalloc
       │              └─ kasan_kmalloc_large()               // KASAN 追踪
       │
       └─ [小对象路径] size <= KMALLOC_MAX_CACHE_SIZE
            ├─ kmalloc_slab(size, b, flags, caller)          // 查找 kmem_cache
            │    ├─ b = b ? b : &kmalloc_caches[kmalloc_type(flags, caller)]
            │    ├─ size <= 192 ? kmalloc_size_index[]      // 小尺寸查表
            │    │              : fls(size - 1)              // 大尺寸计算 index
            │    └─ return (*b)[index]                       // 返回对应 kmem_cache *
            │
            └─ slab_alloc_node(s, NULL, flags, node, caller, size)
                 └─ [同 4.5 节 SLUB 分配路径]
                      ├─ alloc_from_pcs() — Sheaves 快速路径
                      ├─ ___slab_alloc() — 慢速路径
                      └─ ...
```

**关键函数详解**：

```c
// mm/slub.c — 大对象分配
static void *___kmalloc_large_node(size_t size, gfp_t flags, int node)
{
    struct page *page;
    unsigned int order = get_order(size);  // 计算需要 2^N 页

    flags |= __GFP_COMP;                   // 复合页标记
    if (node == NUMA_NO_NODE)
        page = alloc_frozen_pages_noprof(flags, order);  // 伙伴系统
    else
        page = __alloc_frozen_pages_noprof(flags, order, node, NULL);

    if (page) {
        ptr = page_address(page);           // 线性映射虚拟地址
        __SetPageLargeKmalloc(page);        // 标记，kfree 时识别
    }
    return ptr;
}
```

```c
// mm/slab.h — kmem_cache 查找
kmalloc_slab(size_t size, kmem_buckets *b, gfp_t flags, unsigned long caller)
{
    unsigned int index;
    if (!b)
        b = &kmalloc_caches[kmalloc_type(flags, caller)];  // 确定类型
    if (size <= 192)
        index = kmalloc_size_index[size_index_elem(size)];  // 小尺寸查表 O(1)
    else
        index = fls(size - 1);                               // 大尺寸求最高位
    return (*b)[index];  // 返回对应 kmem_cache，如 kmalloc-128
}
```

#### 4.7.4 kfree 释放函数调用栈

```
kfree(object)                                            [mm/slub.c]
  │
  ├─ 1. ZERO_OR_NULL_PTR(object) 检查
  ├─ 2. virt_to_page(object) → page_slab(page)            // 获取 slab 指针
  │
  ├─ [大对象路径] slab == NULL（非 SLUB 管理）
  │    └─ free_large_kmalloc(page, object)
  │         ├─ kmemleak_free() / kasan_kfree_large()
  │         ├─ mod_lruvec_page_state(NR_SLAB_UNRECLAIMABLE_B, -)
  │         ├─ __ClearPageLargeKmalloc(page)
  │         └─ free_frozen_pages(page, order)              // 伙伴系统释放
  │
  └─ [小对象路径] slab != NULL
       └─ slab_free(s, slab, x, _RET_IP_)                  // 走 SLUB 释放
            └─ [同 4.6 节 SLUB 释放路径]
                 ├─ free_to_pcs() — Sheaves 快速路径
                 └─ __slab_free() — 慢速路径
```

```c
// mm/slub.c — 大对象释放
static void free_large_kmalloc(struct page *page, void *object)
{
    unsigned int order = compound_order(page);  // 从复合页获取 order
    kmemleak_free(object);
    kasan_kfree_large(object);
    mod_lruvec_page_state(page, NR_SLAB_UNRECLAIMABLE_B, -(PAGE_SIZE << order));
    __ClearPageLargeKmalloc(page);
    free_frozen_pages(page, order);  // 伙伴系统回收
}
```

#### 4.7.5 kvfree 混合释放

```
kvfree(addr)                                             [mm/slub.c]
  │
  ├─ is_vmalloc_addr(addr) ? → vfree(addr)               // vmalloc 分配的
  └─ else                   → kfree(addr)                // kmalloc 分配的
```

`kvfree` 是 kmalloc 和 vmalloc 的统一释放接口，自动判断地址所属区域后调用对应的释放函数。

#### 4.7.6 kmalloc 类型选择流程图

```
kmalloc_type(flags, caller)
  │
  ├─ 无特殊标志（最常见路径）
  │    └─ return KMALLOC_NORMAL
  │         （若启用 RANDOM_KMALLOC_CACHES：随机化到 KMALLOC_RANDOM_START+N）
  │
  ├─ __GFP_DMA 设置
  │    └─ return KMALLOC_DMA
  │
  ├─ __GFP_RECLAIMABLE 设置
  │    └─ return KMALLOC_RECLAIM
  │
  └─ __GFP_ACCOUNT 设置
       └─ return KMALLOC_CGROUP
```

#### 4.7.7 kmalloc 大小与缓存映射

| 分配大小 | kmalloc 缓存 | 说明 |
|----------|-------------|------|
| 1~8 字节 | `kmalloc-8` | 8 字节对齐 |
| 9~16 字节 | `kmalloc-16` | 16 字节对齐 |
| 17~32 字节 | `kmalloc-32` | 32 字节对齐 |
| 33~64 字节 | `kmalloc-64` | 64 字节对齐 |
| 65~96 字节 | `kmalloc-96` | 96 字节（特殊 size） |
| 97~128 字节 | `kmalloc-128` | 128 字节对齐 |
| 129~192 字节 | `kmalloc-192` | 192 字节（特殊 size） |
| 193~256 字节 | `kmalloc-256` | 256 字节对齐 |
| ... | ... | 2^N 对齐 |
| > KMALLOC_MAX_CACHE_SIZE | 伙伴系统直接分配 | 页级分配 |

### 4.8 kmem_cache_create/destroy API

```
__kmem_cache_create_args(name, object_size, args, flags)  [mm/slab_common.c]
  │
  ├─ 1. 参数验证（kmem_cache_sanity_check）
  ├─ 2. 尝试合并（__kmem_cache_alias）
  │    └─ 若存在兼容缓存，直接返回（复用已有缓存）
  ├─ 3. 计算对齐（calculate_alignment）
  ├─ 4. 创建缓存（create_cache）
  │    ├─ 分配 kmem_cache 结构体
  │    ├─ 初始化 kmem_cache_node（每个 NUMA 节点）
  │    ├─ 初始化 node_barn（sheaves 共享池）
  │    ├─ 分配 per-CPU cpu_sheaves
  │    └─ 初始化 sheaves（main 分配空 sheaf，spare = NULL）
  └─ 5. 加入全局缓存链表

kmem_cache_destroy(s)                                [mm/slab_common.c]
  │
  ├─ 1. 检查 refcount，减少引用
  ├─ 2. 关闭缓存（__kmem_cache_shutdown）
  │    ├─ flush 所有 CPU 的 sheaves 到 partial 链表
  │    ├─ flush 所有 barn 中的 sheaves
  │    └─ 释放所有 partial slab 到伙伴系统
  ├─ 3. 释放 kmem_cache_node 和 cpu_sheaves
  └─ 4. 从全局链表移除
```

### 4.9 关键桶分配（kmem_buckets）

```c
// include/linux/slab.h
struct kmem_buckets {
    struct kmem_cache *buckets[KMALLOC_SHIFT_HIGH + 1]; // 按 2^N 对齐的缓存数组
};

// kmem_buckets_alloc() 从指定桶分配
// 系统默认桶 kmalloc_caches 在 mm/slab_common.c 初始化
// 分配大小被对齐到 2^N，然后查找对应 buckets 中的 kmem_cache
```

### 4.10 统计与监控

SLUB 通过 `enum stat_item` 跟踪详细的分配/释放事件：

```c
enum stat_item {
    ALLOC_FASTPATH,          // 从 Per-CPU sheaves 分配成功
    ALLOC_SLOWPATH,          // 从 partial 或新 slab 分配
    FREE_FASTPATH,           // 释放到 Per-CPU sheaves 成功
    FREE_SLOWPATH,           // 释放到 slab
    FREE_ADD_PARTIAL,        // 释放使 slab 加入 partial 链表
    FREE_REMOVE_PARTIAL,     // 释放使 slab 从 partial 链表移除
    ALLOC_SLAB,              // 从伙伴系统分配新 slab
    ALLOC_NODE_MISMATCH,     // 请求的节点与 CPU sheaf 不匹配
    FREE_SLAB,               // 将 slab 释放回伙伴系统
    SHEAF_FLUSH,             // Sheaf 中的对象被 flush 到 partial 链表
    SHEAF_REFILL,            // Sheaf 从 partial 链表批量填充
    BARN_GET,                // 从 barn 获取满 sheaf 成功
    BARN_GET_FAIL,           // 从 barn 获取满 sheaf 失败
    BARN_PUT,                // 将满 sheaf 放入 barn 成功
    BARN_PUT_FAIL,           // 将满 sheaf 放入 barn 失败（barn 容量满）
    NR_SLUB_STAT_ITEMS
};
```

通过 `CONFIG_SLUB_STATS` 启用，在 `/sys/kernel/slab/<cache>/alloc_fastpath` 等文件查看。

### 4.11 关键性能优化

| 优化 | 机制 | 收益 |
|------|------|------|
| **Sheaves 双缓存** | main + spare 两级 Per-CPU 对象数组 | 热路径无锁操作（仅关抢占） |
| **Node Barn** | 节点级 sheaf 共享池 | 减少 CPU 间 sheaf 重建开销 |
| **批量操作** | refill_sheaf / sheaf_flush_unused 批量处理 | 减少伙伴系统访问频率 |
| **RCU Sheaf** | 延迟释放 kfree_rcu 对象 | 减少 RCU 回调频率 |
| **cmpxchg_double** | 无锁更新 slab->freelist | 慢速路径免锁（部分架构） |
| **缓存合并** | __kmem_cache_alias 复用兼容缓存 | 减少缓存数量，降低内存开销 |
| **自适应批量** | alloc_from_new_slab 批量分配对象 | 减少 slab 初始化次数 |
| **local_trylock** | 非 RT 仅关抢占，无原子操作 | 热路径零开销 |

### 4.12 常用缓存

| 缓存名称 | 对象大小 | 用途 |
|----------|----------|------|
| `kmalloc-8` ~ `kmalloc-8k` | 8B ~ 8KB | 通用 kmalloc |
| `task_struct` | ~4KB | 进程描述符 |
| `mm_struct` | ~1KB | 内存描述符 |
| `inode_cache` | ~1KB | VFS inode |
| `dentry_cache` | ~256B | 目录项缓存 |

### 4.13 Linux 7.0 SLUB 新特性

#### kmalloc_obj 系列 API

Linux 7.0 引入了基于类型的 `kmalloc` 新 API，允许更精细化地管理内存和发现问题：

```c
// 基于类型的 kmalloc 封装
static inline void *kmalloc_obj(struct kmem_cache *cache, gfp_t flags)
{
    return kmem_cache_alloc(cache, flags);
}

// 对应的 kfree 封装
static inline void kmem_cache_free_obj(struct kmem_cache *cache, void *obj)
{
    kmem_cache_free(cache, obj);
}
```

**优势**：
- 编译时类型检查，减少类型不匹配错误
- 便于追踪特定类型对象的内存使用
- 为未来的内存安全特性（如 KASAN 增强）提供基础

#### Sheaves 缓存机制

**Sheaves** 是 SLUB 分配器中引入的新功能，旨在通过降低锁开销和简化代码来提升性能。

**核心思想**：Sheaves 在 Per-CPU 缓存（cpu_slab）和 Per-NUMA 节点缓存（kmem_cache_node）之间引入了一个中间层，减少对节点锁的竞争。

```
传统路径：
  cpu_slab → kmem_cache_node (node->list_lock)
        ↓
Sheaves 路径：
  cpu_slab → sheaves → kmem_cache_node (减少锁竞争)
```

**预期收益**：
- 降低多核并发下的锁争用
- 提高 NUMA 系统的扩展性
- 简化 partial slab 的管理逻辑

---

## 5. 虚拟内存管理

### 5.1 vmalloc 分配器

文件：`mm/vmalloc.c`（5,485 行）

#### 5.1.1 架构概述

vmalloc 提供**虚拟地址连续、物理地址不连续**的分配方式，适用于大块内存分配（如内核模块加载、设备驱动缓冲区）。与 kmalloc 的区别在于：

| 特性 | kmalloc | vmalloc |
|------|---------|---------|
| 物理连续性 | 连续（小对象在 SLUB，大对象在伙伴系统） | **不连续** |
| 虚拟连续性 | 线性映射（直接映射区） | 在 vmalloc 区域建立新页表映射 |
| 分配粒度 | 任意字节 | 页（PAGE_SIZE） |
| 适用场景 | 小对象、频繁分配 | 大块内存、不要求物理连续 |
| 性能 | 高（Sheaves 缓存无锁路径） | 较低（需要页表操作） |

**vmalloc 地址空间**：位于内核虚拟地址空间的 `VMALLOC_START` ~ `VMALLOC_END` 区间，每个 vmalloc 分配之间有一个 guard page（`VM_NO_GUARD` 可取消）。

#### 5.1.2 核心数据结构

```c
// include/linux/vmalloc.h

// 描述 vmalloc 分配元数据
struct vm_struct {
    union {
        struct vm_struct *next;       // 早期注册链表（启动阶段）
        struct llist_node llnode;     // 错误路径异步释放链表
    };
    void            *addr;            // 起始虚拟地址
    unsigned long    size;            // 大小（含 guard page）
    unsigned long    flags;           // VM_ALLOC / VM_IOREMAP / VM_MAP 等
    struct page    **pages;           // 物理页指针数组
    unsigned int     page_order;      // 巨页分配时的 order（HAVE_ARCH_HUGE_VMALLOC）
    unsigned int     nr_pages;        // 分配的物理页面数
    phys_addr_t      phys_addr;       // 物理地址（ioremap 时使用）
    const void      *caller;          // 调用者地址（用于调试 /proc/vmallocinfo）
    unsigned long    requested_size;  // 用户请求的原始大小（不含 guard）
};

// 描述 vmalloc 地址空间区域（用于分配/释放管理）
struct vmap_area {
    unsigned long va_start;           // 区域起始地址
    unsigned long va_end;             // 区域结束地址

    struct rb_node rb_node;           // 按地址排序的红黑树节点
    struct list_head list;            // 按地址排序的双向链表

    union {
        unsigned long subtree_max_size; // 空闲树中：子树最大空闲区间
        struct vm_struct *vm;           // 忙碌树中：关联的 vm_struct
    };
    unsigned long flags;              // 标记（如 VM_MAP_PUT_PAGES）
};

// vmalloc 节点（Per-CPU-ish 管理单元，减少全局锁竞争）
struct vmap_node {
    struct vmap_pool pool[MAX_VA_SIZE_PAGES];  // 大小分级存储（≤256页）
    spinlock_t pool_lock;                       // pool 保护锁
    bool skip_populate;                         // 跳过填充标记

    struct rb_list busy;                        // 忙碌 vmap_area 红黑树
    struct rb_list lazy;                        // 惰性释放 vmap_area 红黑树

    struct list_head purge_list;                // 待清理 vmap_area 链表
    struct work_struct purge_work;              // 清理工作项
    unsigned long nr_purged;                    // 已清理计数
};

// 大小分级存储池（每个 vmap_node 有 256 个）
struct vmap_pool {
    struct list_head head;            // 空闲 vmap_area 链表头
    unsigned long len;                // 链表长度
};

// 红黑树+链表组合结构
struct rb_list {
    struct rb_root root;              // 红黑树根
    struct list_head head;            // 双向链表头
    spinlock_t lock;                  // 保护锁
};
```

**内存布局关系**：

```
vmap_nodes[]                    ← 全局 vmap 节点数组（默认 1 个，可扩展到多节点）
  └─ vmap_node
       ├─ pool[0..255]          ← 大小分级存储（≤256页的 vmap_area 快速分配）
       ├─ busy.rb_list          ← 已分配 vmap_area（含关联的 vm_struct）
       ├─ lazy.rb_list          ← 惰性释放 vmap_area（等待 TLB flush）
       └─ purge_list            ← 待清理链表
```

#### 5.1.3 完整分配函数调用栈

```
vmalloc_noprof(size)                                       [mm/vmalloc.c]
  └─ __vmalloc_node_noprof(size, 1, GFP_KERNEL, NUMA_NO_NODE, caller)
       └─ __vmalloc_node_range_noprof(size, align,            [mm/vmalloc.c:3986]
                VMALLOC_START, VMALLOC_END, gfp_mask, ...)
            │
            ├─ [巨页路径] 若 VM_ALLOW_HUGE_VMAP 且 size >= PMD_SIZE
            │    └─ shift = PMD_SHIFT, align = max(align, 1UL << shift)
            │
            ├─ 1. __get_vm_area_node(size, align, shift, ...)   [mm/vmalloc.c:3203]
            │    │   // 在 vmalloc 地址空间分配虚拟区间
            │    │
            │    ├─ kzalloc_node(sizeof(*area), ...)            // 分配 vm_struct
            │    ├─ 若 !VM_NO_GUARD: size += PAGE_SIZE          // 添加 guard page
            │    │
            │    └─ alloc_vmap_area(size, align, vstart, vend,  [mm/vmalloc.c:2029]
            │             node, gfp_mask, 0, area)
            │         │   // 在 vmap 地址空间（红黑树）查找/分配空闲区间
            │         │
            │         ├─ node_alloc(size, align, ...)           // 尝试从 vmap_node pool 分配
            │         │    └─ 在 pool[MAX_VA_SIZE_PAGES] 中查找空闲 vmap_area
            │         │
            │         ├─ [pool 分配失败] kmem_cache_alloc(vmap_area_cachep, ...)
            │         │    └─ 从 vmap_area SLUB 缓存分配新 vmap_area
            │         │
            │         ├─ __alloc_vmap_area(&free_vmap_area_root, ...)  // 全局红黑树查找
            │         │    └─ 在 free_vmap_area_root 红黑树中查找最佳空闲区间
            │         │
            │         ├─ va->va_start = addr; va->va_end = addr + size
            │         ├─ insert_vmap_area(va, &vn->busy, ...)   // 插入忙碌树
            │         └─ setup_vmalloc_vm(va, area, ...)        // 关联 vm_struct
            │
            ├─ 2. __vmalloc_area_node(area, gfp_mask, prot, shift, node)  [mm/vmalloc.c:3827]
            │    │   // 分配物理页面并建立页表映射
            │    │
            │    ├─ 分配 pages 数组
            │    │    ├─ array_size > PAGE_SIZE ?
            │    │    │    └─ __vmalloc_node(array_size, ...)  // 递归分配（≥1页）
            │    │    └─ kmalloc_node(array_size, ...)         // 小数组用 kmalloc
            │    │
            │    ├─ vm_area_alloc_pages(gfp, node, page_order,  [mm/vmalloc.c:3641]
            │    │       nr_small_pages, area->pages)
            │    │    │   // 批量分配物理页面
            │    │    │
            │    │    ├─ [巨页路径] 尝试大 order 分配
            │    │    │    ├─ alloc_pages_noprof(large_gfp, large_order)
            │    │    │    └─ split_page(page, large_order)    // 拆分复合页
            │    │    │
            │    │    └─ [order-0 路径] 批量分配
            │    │         ├─ alloc_pages_bulk_array_mempolicy()  // 批量（一次 100 页）
            │    │         └─ alloc_page_noprof()               // 逐页回退
            │    │
            │    └─ vmap_pages_range(addr, addr + size, prot,   // 建立页表映射
            │             area->pages, page_shift)
            │          └─ vmap_pages_p4d()                       // 遍历页表层级
            │               └─ vmap_pages_pud()
            │                    └─ vmap_pages_pmd()
            │                         └─ vmap_pages_pte()
            │                              └─ set_pte_at()      // 设置 PTE
            │
            └─ 3. kasan_unpoison_vmalloc(area->addr, size, ...)  // KASAN 追踪
```

#### 5.1.4 关键函数详解

**`__get_vm_area_node`** — 分配虚拟地址区间：

```c
// mm/vmalloc.c
struct vm_struct *__get_vm_area_node(unsigned long size,
        unsigned long align, unsigned long shift, unsigned long flags,
        unsigned long start, unsigned long end, int node,
        gfp_t gfp_mask, const void *caller)
{
    struct vmap_area *va;
    struct vm_struct *area;

    size = ALIGN(size, 1ul << shift);          // 按 shift 对齐
    area = kzalloc_node(sizeof(*area), ...);    // 分配 vm_struct
    if (!(flags & VM_NO_GUARD))
        size += PAGE_SIZE;                      // 添加 guard page

    va = alloc_vmap_area(size, align, start, end, node, gfp_mask, 0, area);
    // alloc_vmap_area 在 vmap 地址空间红黑树中查找

---

## 6. VMA 管理

### 6.1 概述

VMA（Virtual Memory Area）是进程地址空间的基本管理单元，描述一段具有相同属性的连续虚拟地址区间。每个进程的 `mm_struct` 通过一棵 **Maple Tree**（`mm->mm_mt`）组织所有 VMA。

VMA 管理代码分布在以下文件中：

| 文件 | 行数 | 职责 |
|------|------|------|
| `mm/vma.c` | 3,309 | 核心 VMA 操作（合并、分裂、插入、删除） |
| `mm/vma_init.c` | — | VMA 分配/释放/初始化 |
| `mm/vma_exec.c` | — | 执行 mmap 文件映射 |
| `mm/mmap.c` | 1,922 | mmap/munmap 系统调用入口 |
| `mm/mremap.c` | 1,998 | mremap 系统调用 |
| `mm/mprotect.c` | 1,007 | mprotect 系统调用 |

**VMA 组织方式演变**：
- **Linux 6.0 之前**：红黑树（`vm_rb`） + 双向链表（`vm_next`/`vm_prev`）
- **Linux 6.1+**：**Maple Tree**（`mm->mm_mt`）替代红黑树，支持 O(log n) 的区间查询和高效的 RCU 遍历

### 6.2 核心数据结构（带详细注释）

#### 6.2.1 struct vm_area_struct — VMA 描述符

```c
// include/linux/mm_types.h
struct vm_area_struct {
    /* === 第一缓存行：VMA 树遍历所需 === */

    union {
        struct {
            unsigned long vm_start;      // VMA 起始地址（包含）
            unsigned long vm_end;        // VMA 结束地址（不包含）
        };
        freeptr_t vm_freeptr;            // SLAB_TYPESAFE_BY_RCU 释放指针
    };

    struct mm_struct *vm_mm;             // 所属进程的 mm_struct

    pgprot_t vm_page_prot;               // 页面访问权限（vm_flags 对应的页表保护位）

    union {
        const vm_flags_t vm_flags;       // VMA 标志位（旧式，只读访问）
        vma_flags_t flags;               // VMA 标志位（新式，可读写）
    };

#ifdef CONFIG_PER_VMA_LOCK
    unsigned int vm_lock_seq;            // 写锁序列号，用于 Per-VMA Lock
#endif

    /* 匿名页反向映射 */
    struct list_head anon_vma_chain;     // 匿名 VMA 链表（由 mmap_lock 保护）
    struct anon_vma *anon_vma;           // 匿名页反向映射结构

    /* 操作函数指针 */
    const struct vm_operations_struct *vm_ops;  // VMA 操作函数表

    /* 后备存储信息 */
    unsigned long vm_pgoff;              // 文件映射：文件内的页偏移
    struct file *vm_file;                // 映射的文件（NULL 表示匿名映射）
    void *vm_private_data;               // 私有数据（如 shared memory）

#ifdef CONFIG_SWAP
    atomic_long_t swap_readahead_info;   // 交换预读信息
#endif
#ifdef CONFIG_NUMA
    struct mempolicy *vm_policy;         // NUMA 内存策略
#endif
#ifdef CONFIG_NUMA_BALANCING
    struct vma_numab_state *numab_state; // NUMA 均衡状态
#endif

#ifdef CONFIG_PER_VMA_LOCK
    refcount_t vm_refcnt ____cacheline_aligned_in_smp;  // VMA 引用计数
    // 0 = 已分离, 1 = 已附加(无锁/写锁), >1 = 读锁持有
#endif

    /* 文件页反向映射（address_space 区间树） */
    struct {
        struct rb_node rb;
        unsigned long rb_subtree_last;
    } shared;

#ifdef CONFIG_ANON_VMA_NAME
    struct anon_vma_name *anon_name;     // 匿名 VMA 名称（可由 prctl 设置）
#endif
};
```

#### 6.2.2 struct mm_struct — VMA 相关字段

```c
// include/linux/mm_types.h
struct mm_struct {
    struct maple_tree mm_mt;             // VMA 存储的 Maple Tree（替代旧红黑树）

    unsigned long mmap_base;             // mmap 基地址
    unsigned long mmap_legacy_base;      // 传统 mmap 基地址（自底向上）
    unsigned long task_size;             // 用户虚拟地址空间大小
    pgd_t *pgd;                          // 页全局目录

    int map_count;                       // VMA 数量

    spinlock_t page_table_lock;          // 保护页表及部分计数器
    struct rw_semaphore mmap_lock;       // 保护地址空间布局（读/写信号量）

    unsigned long total_vm;              // 总映射页面数
    unsigned long locked_vm;             // mlock 锁定的页面数
    unsigned long data_vm;               // 数据段页面数
    unsigned long exec_vm;               // 可执行段页面数
    unsigned long stack_vm;              // 栈段页面数

    vm_flags_t def_flags;                // 默认 VMA 标志

    unsigned long start_code, end_code;  // 代码段范围
    unsigned long start_data, end_data;  // 数据段范围
    unsigned long start_brk, brk;        // 堆范围
    unsigned long start_stack;           // 栈起始地址
    unsigned long arg_start, arg_end;    // 命令行参数范围
    unsigned long env_start, env_end;    // 环境变量范围
};
```

#### 6.2.3 struct vma_iterator — VMA 迭代器

```c
// include/linux/mm_types.h
struct vma_iterator {
    struct ma_state mas;                 // Maple Tree 遍历状态
};

// 初始化宏
#define VMA_ITERATOR(name, __mm, __addr)                \
    struct vma_iterator name = {                        \
        .mas = {                                        \
            .tree = &(__mm)->mm_mt,                     \
            .index = __addr,                            \
            .node = NULL,                               \
            .status = ma_start,                         \
        },                                              \
    }
```

#### 6.2.4 struct vm_operations_struct — VMA 操作函数表

```c
// include/linux/mm.h
struct vm_operations_struct {
    void (*open)(struct vm_area_struct *area);           // VMA 打开时调用
    void (*close)(struct vm_area_struct *area);          // VMA 关闭时调用
    int (*may_split)(struct vm_area_struct *area,        // 分裂前检查
                     unsigned long addr);
    int (*mremap)(struct vm_area_struct *area);          // mremap 时调用
    int (*mprotect)(struct vm_area_struct *vma,          // mprotect 权限检查
                    unsigned long start, unsigned long end,
                    unsigned long newflags);
    vm_fault_t (*fault)(struct vm_fault *vmf);           // 缺页处理
    vm_fault_t (*huge_fault)(struct vm_fault *vmf,       // 巨页缺页
                             unsigned int order);
    vm_fault_t (*map_pages)(struct vm_fault *vmf,        // 批量映射页面
                            pgoff_t start_pgoff,
                            pgoff_t end_pgoff);
    unsigned long (*pagesize)(struct vm_area_struct *area);
    vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);    // 页面写时复制通知
};
```

### 6.3 VMA 标志位完整枚举

```c
// include/linux/mm.h
enum vma_flag {
    VMA_READ_BIT       = 0,   // 可读
    VMA_WRITE_BIT      = 1,   // 可写
    VMA_EXEC_BIT       = 2,   // 可执行
    VMA_SHARED_BIT     = 3,   // 共享映射
    VMA_MAYREAD_BIT    = 4,   // 将来可能可读
    VMA_MAYWRITE_BIT   = 5,   // 将来可能可写
    VMA_MAYEXEC_BIT    = 6,   // 将来可能可执行
    VMA_MAYSHARE_BIT   = 7,   // 将来可能共享
    VMA_GROWSDOWN_BIT  = 8,   // 可向下增长（栈）
    VMA_UFFD_MISSING_BIT = 9, // userfaultfd 缺失页跟踪
    VMA_PFNMAP_BIT     = 10,  // PFN 映射（无 struct page）
    VMA_MAYBE_GUARD_BIT= 11,  // 可能为 guard 页
    VMA_UFFD_WP_BIT    = 12,  // userfaultfd 写保护跟踪
    VMA_LOCKED_BIT     = 13,  // 锁定在内存中
    VMA_IO_BIT         = 14,  // 内存映射 I/O
    VMA_SEQ_READ_BIT   = 15,  // 顺序访问
    VMA_RAND_READ_BIT  = 16,  // 随机访问
    VMA_DONTCOPY_BIT   = 17,  // fork 时不复制
    VMA_DONTEXPAND_BIT = 18,  // 不能用 mremap 扩展
    VMA_LOCKONFAULT_BIT= 19,  // 缺页时锁定
    VMA_ACCOUNT_BIT    = 20,  // 内存计费
    VMA_NORESERVE_BIT  = 21,  // 禁止预留
    VMA_HUGETLB_BIT    = 22,  // 巨页
    VMA_SYNC_BIT       = 23,  // 同步缺页
    VMA_ARCH_1_BIT     = 24,  // 架构特定
    VMA_WIPEONFORK_BIT = 25,  // fork 时清空
    VMA_DONTDUMP_BIT   = 26,  // 不包含在 core dump 中
    VMA_SOFTDIRTY_BIT  = 27,  // 软脏页标记
    VMA_MIXEDMAP_BIT   = 28,  // 混合映射（struct page + PFN）
    VMA_HUGEPAGE_BIT   = 29,  // MADV_HUGEPAGE
    VMA_NOHUGEPAGE_BIT = 30,  // MADV_NOHUGEPAGE
    VMA_MERGEABLE_BIT  = 31,  // KSM 可合并
    VMA_HIGH_ARCH_0..6_BIT,   // 架构高端标志（32-38）
    VMA_ALLOW_ANY_UNCACHED_BIT = 39, // 允许任意非缓存映射
    VMA_DROPPABLE_BIT  = 40,  // 内核可丢弃（如 vDSO）
    VMA_UFFD_MINOR_BIT = 41,  // userfaultfd 次要缺页
    VMA_SEALED_BIT     = 42,  // 密封（不可修改）
};
```

**常用组合标志**：

| 宏 | 值 | 用途 |
|----|-----|------|
| `VM_ACCESS_FLAGS` | READ\|WRITE\|EXEC | 基本访问权限 |
| `VM_SPECIAL` | IO\|DONTEXPAND\|PFNMAP\|MIXEDMAP | 特殊 VMA（不可合并） |
| `VM_STACK_FLAGS` | GROWSDOWN + 默认栈标志 | 栈映射 |
| `VM_WRITE` | 0x00000002 | 可写 |
| `VM_EXEC` | 0x00000004 | 可执行 |
| `VM_SHARED` | 0x00000008 | 共享映射 |
| `VM_GROWSDOWN` | 0x00000100 | 可向下增长（栈） |
| `VM_LOCKED` | 0x00002000 | 锁定在内存中（mlock） |
| `VM_IO` | 0x00004000 | I/O 映射 |
| `VM_DONTCOPY` | 0x00020000 | fork 时跳过复制 |
| `VM_HUGETLB` | 0x04000000 | 巨页映射 |
| `VM_SEALED` | 0x000100000000 | 密封（不可修改） |

### 6.4 VMA 的存储组织：Maple Tree

Linux 6.1+ 使用 **Maple Tree** 替代红黑树来组织 VMA。Maple Tree 是一种 B 树风格的区间索引结构，具有以下特点：

```
Maple Tree (mm->mm_mt)
  │
  ├─ 节点类型：叶子节点（存储 VMA 指针）和内部节点（存储索引范围）
  ├─ 时间复杂度：O(log n) 查找/插入/删除
  ├─ RCU 友好：支持 RCU 遍历和并发更新
  └─ 区间存储：直接以 [vm_start, vm_end) 为键值，无需额外区间树

旧方案（< 6.0）:
  [红黑树 vm_rb] + [双向链表 vm_next/vm_prev] + [vma_cache]
       ↓
新方案（6.1+）:
  [Maple Tree mm_mt] + [VMA Iterator]
```

**VMA 迭代器操作**：

```c
// 查找 addr 所在 VMA
vma = vma_lookup(mm, addr);     // → mtree_load(&mm->mm_mt, addr)

// 查找第一个 >= addr 的 VMA
vma = find_vma(mm, addr);       // → mtree_find(&mm->mm_mt, addr)

// 查找与区间 [start, end) 相交的 VMA
vma = find_vma_intersection(mm, start, end);

// 遍历所有 VMA
VMA_ITERATOR(vmi, mm, 0);
for_each_vma(vmi, vma) {
    // 处理 vma
}
```

### 6.5 VMA 生命周期

```
vm_area_alloc(mm)              — 分配 VMA 结构体（SLUB kmem_cache）
    ↓
    ↓ vma_set_range()          — 设置 vm_start/vm_end
    ↓ vm_flags_init()          — 设置标志位
    ↓ vma_start_write()        — 获取写锁
    ↓ vma_iter_store()         — 插入 Maple Tree (mm->mm_mt)
    ↓ vma_link_file()          — 插入 address_space 区间树
    ↓
vm_area_free(vma)              — 释放 VMA 结构体
```

**VMA 分配**：

```c
// mm/vma_init.c
struct vm_area_struct *vm_area_alloc(struct mm_struct *mm)
{
    struct vm_area_struct *vma;

    vma = slab_vma_alloc(mm);           // 从 vma_cache 分配（SLUB）
    if (vma) {
        vma->vm_mm = mm;                // 绑定 mm_struct
        init_vma(vma);                  // 初始化字段
    }
    return vma;
}

void vm_area_free(struct vm_area_struct *vma)
{
    // 从 Maple Tree 移除
    // 释放 anon_vma 和 NUMA 状态
    kmem_cache_free(vm_area_cachep, vma);  // 归还给 SLUB
}
```

### 6.6 完整 mmap 函数调用栈

```
SYSCALL_DEFINE6(mmap, addr, len, prot, flags, fd, offset)   [mm/mmap.c]
  └─ vm_mmap_pgoff(file, addr, len, prot, flags, pgoff)
       └─ do_mmap(file, addr, len, prot, flags, vm_flags, pgoff, ...)  [mm/mmap.c:335]
            │
            ├─ 1. 参数验证
            │    ├─ 检查 len 有效性
            │    ├─ PAGE_ALIGN(len)
            │    ├─ mm->map_count > sysctl_max_map_count ?
            │    └─ get_unmapped_area()               // 查找空闲地址区间
            │
            ├─ 2. 前置处理
            │    ├─ calc_vm_flags()                   // prot/flags → vm_flags
            │    ├─ mmap_assert_write_locked(mm)      // 确保 mmap_lock 写锁
            │    └─ (MAP_FIXED) ? do_vmi_munmap() :   // 先解除已有映射
            │
            └─ 3. mmap_region(file, addr, len, vm_flags, pgoff, uf)  [mm/vma.c:2720]
                 │
                 └─ __mmap_region(file, addr, len, vm_flags, pgoff, uf)  [mm/vma.c:2720]
                      │
                      ├─ [1] __mmap_setup(&map, &desc, uf)              [mm/vma.c:2392]
                      │    ├─ vma_find(vmi, end)               // 查找区间重叠 VMA
                      │    ├─ init_vma_munmap(vms, ...)        // 初始化 unmap 状态
                      │    ├─ vms_gather_munmap_vmas(vms, ...) // 收集待移除 VMA
                      │    │    ├─ 遍历重叠 VMA
                      │    │    ├─ split_vma() 分裂边界 VMA
                      │    │    └─ vms_clean_up_area()         // 清除页表
                      │    ├─ may_expand_vm()                  // 检查地址空间限制
                      │    └─ security_vm_enough_memory_mm()   // 检查安全策略
                      │
                      ├─ [2] 尝试合并
                      │    └─ vma_merge_new_range(&vmg)        // 与相邻 VMA 合并
                      │         └─ vma_merge_existing_range()  // 核心合并逻辑
                      │              ├─ can_vma_merge_before/after()  // 检查合并条件
                      │              ├─ commit_merge()          // 执行合并
                      │              │    ├─ vma_prepare()     // 准备合并（加锁、移除旧区间树）
                      │              │    └─ vma_complete()    // 完成合并（解锁、更新新区间树）
                      │              └─ vma_expand() / vma_shrink()  // 扩展/收缩 VMA
                      │
                      ├─ [3] 合并失败，创建新 VMA
                      │    └─ __mmap_new_vma(&map, &vma)              [mm/vma.c:2506]
                      │         ├─ vm_area_alloc(mm)          // 分配 VMA 结构体
                      │         ├─ vma_set_range(vma, ...)    // 设置地址范围
                      │         ├─ vm_flags_init(vma, ...)    // 设置标志位
                      │         ├─ vma_iter_prealloc(vmi, vma) // 预分配 Maple Tree 节点
                      │         │
                      │         ├─ [文件映射] __mmap_new_file_vma()
                      │         │    └─ file->f_op->mmap(file, vma)  // 调用文件系统 mmap
                      │         │         ├─ 文件系统建立映射（如 ext4_file_mmap）
                      │         │         └─ vma->vm_ops = &file_operations
                      │         │
                      │         ├─ [共享匿名映射] shmem_zero_setup(vma)
                      │         │    └─ 映射到 /dev/zero
                      │         │
                      │         └─ [私有匿名映射] vma_set_anonymous(vma)
                      │              └─ vma->vm_ops = NULL
                      │
                      ├─ [4] 完成
                      │    ├─ vma_start_write(vma)            // 获取 VMA 写锁
                      │    ├─ vma_iter_store_new(vmi, vma)    // 插入 Maple Tree
                      │    ├─ map_count++                     // 递增 VMA 计数
                      │    ├─ vma_link_file(vma)              // 插入 address_space 区间树
                      │    └─ khugepaged_enter_vma(vma, ...)  // 通知 khugepaged
                      │
                      └─ [5] 清理
                           └─ vms_complete_munmap_vmas()      // 释放旧映射
```

### 6.7 完整 munmap 函数调用栈

```
SYSCALL_DEFINE2(munmap, addr, len)                         [mm/mmap.c]
  └─ do_vmi_munmap(&vmi, mm, addr, len, uf, /* unlock = */ false)  [mm/vma.c:1611]
       │
       └─ do_vmi_align_munmap(vmi, vma, mm, start, end, uf, unlock)  [mm/vma.c:1564]
            │
            ├─ vms_gather_munmap_vmas(vms, mas_detach)      [mm/vma.c:1379]
            │    ├─ 遍历目标区间内的 VMA
            │    ├─ split_vma() 分裂边界 VMA（部分重叠时）
            │    ├─ vma_start_write() 获取写锁
            │    ├─ vma_iter_store(vmi, NULL) 从 Maple Tree 移除
            │    └─ 收集到 mas_detach 临时树
            │
            ├─ vms_clean_up_area(vms, mas_detach)           [mm/vma.c:1288]
            │    └─ unmap_vmas() 清除页表映射
            │
            ├─ vms_complete_munmap_vmas(vms, mas_detach)    [mm/vma.c:1311]
            │    ├─ 更新 mm->map_count
            │    ├─ 更新 mm->total_vm 等统计
            │    ├─ 释放匿名页（延迟到 rmap 完成）
            │    └─ remove_vma_list() 释放 VMA 结构体
            │         └─ vm_area_free(vma) 逐 VMA 释放
            │
            └─ validate_mm(mm) 验证一致性
```

### 6.8 VMA 查找与遍历

```
// 精确查找 addr 所在的 VMA（O(log n)）
vma_lookup(mm, addr)
  └─ mtree_load(&mm->mm_mt, addr)          // Maple Tree 精确查找

// 查找第一个 >= addr 的 VMA
find_vma(mm, addr)
  └─ mtree_find(&mm->mm_mt, addr)          // Maple Tree 区间查找

// 查找与区间相交的 VMA
find_vma_intersection(mm, start, end)
  └─ vma_lookup(mm, start) ?? find_vma(mm, start)

// 扩展栈（自动增长）
find_extend_vma(mm, addr)
  ├─ vma_lookup(mm, addr)                  // 查找当前 VMA
  └─ expand_stack(vma, addr)               // 增长栈 VMA
       └─ anon_vma_interval_tree_pre_update_vma(vma)
            └─ vma->vm_start -= PAGE_SIZE  // 向下扩展
```

### 6.9 VMA 锁机制（Per-VMA Lock）

Linux 7.0+ 引入了 **Per-VMA Lock** 机制，允许在 mmap_lock 读锁下对 VMA 进行并发读操作，大幅提升多线程场景下的性能。

```
锁层次：
  mmap_lock (读写信号量)          ← 保护整个地址空间
    └─ Per-VMA Lock (vm_refcnt)  ← 保护单个 VMA
         └─ page_table_lock      ← 保护页表

VMA 引用计数 (vm_refcnt)：
  0  — 已分离（不可读）
  1  — 已附加，无锁或写锁持有
  >1 — 读锁持有中

关键 API：
  vma_start_read(vma)     — 获取 VMA 读锁（RCU 路径）
  vma_end_read(vma)       — 释放 VMA 读锁
  vma_start_write(vma)    — 获取 VMA 写锁
  vma_assert_write_locked(vma) — 断言写锁
```

**Per-VMA Lock 在缺页路径中的应用**：

```
handle_mm_fault(vma, addr, flags)
  │
  ├─ 持有 mmap_lock 读锁
  ├─ vma_start_read(vma)              // 获取 VMA 读锁
  │
  ├─ 若获取失败（写锁持有中）:
  │    └─ 回退到 mmap_lock 写锁路径
  │
  └─ 若获取成功:
       ├─ 执行缺页处理（不需要 mmap_lock）
       └─ vma_end_read(vma)           // 释放 VMA 读锁
```

### 6.10 VMA 核心操作汇总

| 操作 | 函数 | 文件 | 功能 |
|------|------|------|------|
| **分配** | `vm_area_alloc()` | vma_init.c | 从 SLUB 缓存分配 VMA 结构体 |
| **释放** | `vm_area_free()` | vma_init.c | 归还 VMA 结构体到 SLUB |
| **查找** | `vma_lookup()` | mm.h (内联) | Maple Tree 精确查找 |
| **查找** | `find_vma()` | mmap.c | 查找第一个 >= addr 的 VMA |
| **查找相交** | `find_vma_intersection()` | mmap.c | 查找与区间相交的 VMA |
| **插入** | `vma_iter_store()` | maple_tree.c | 将 VMA 插入 Maple Tree |
| **合并** | `vma_merge_new_range()` | vma.c | 将新区间合并到相邻 VMA |
| **分裂** | `split_vma()` | vma.c | 在指定地址分裂 VMA（调用 `__split_vma`） |
| **扩展** | `vma_expand()` | vma.c | 扩展 VMA 范围（扩大区间） |
| **收缩** | `vma_shrink()` | vma.c | 收缩 VMA 范围（缩小区间） |
| **修改标志** | `vma_modify_flags()` | vma.c | 修改 VMA 标志位 |
| **修改名称** | `vma_modify_name()` | vma.c | 修改匿名 VMA 名称 |
| **mmap 创建** | `mmap_region()` | vma.c | 创建新映射的完整流程 |
| **munmap 删除** | `do_vmi_munmap()` | vma.c | 删除映射的完整流程 |
| **设置页属性** | `vma_set_page_prot()` | mmap.c | 根据 vm_flags 更新 vm_page_prot |
| **验证** | `validate_mm()` | vma.c | 调试：验证 VMA 树一致性 |

### 6.11 VMA 操作流程示意图

```
mmap() 创建新 VMA:
  [系统调用] → [do_mmap] → [mmap_region] → [__mmap_region]
       │
       ├─ __mmap_setup: 确定地址区间，收集重叠 VMA
       ├─ vma_merge_new_range: 尝试与相邻 VMA 合并
       └─ __mmap_new_vma: 创建新 VMA，插入 Maple Tree

munmap() 删除 VMA:
  [系统调用] → [do_vmi_munmap] → [do_vmi_align_munmap]
       │
       ├─ vms_gather_munmap_vmas: 收集目标 VMA，分裂边界
       ├─ vms_clean_up_area: 清除页表映射
       └─ vms_complete_munmap_vmas: 释放 VMA 结构体

mprotect() 修改权限:
  [系统调用] → [do_mprotect_pkey]
       │
       ├─ 遍历目标区间 VMA
       ├─ split_vma: 分裂边界 VMA
       └─ 修改 VMA 标志位和页表权限

mremap() 移动/调整 VMA:
  [系统调用] → [do_mremap]
       │
       ├─ move_vma: 移动 VMA 到新地址
       └─ vma_expand / vma_shrink: 调整大小
```

### 6.12 VMA 类型与典型布局

```
进程虚拟地址空间（典型 64 位布局）:
  ┌──────────────────────┐ 0x0000000000000000
  │    代码段 (text)     │  [start_code, end_code], VM_READ|VM_EXEC
  ├──────────────────────┤
  │    数据段 (data)     │  [start_data, end_data], VM_READ|VM_WRITE
  ├──────────────────────┤
  │       堆 (heap)      │  [start_brk, brk], VM_READ|VM_WRITE
  │        ↓ ↑           │  可通过 brk() 扩展
  ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤
  │     mmap 区域        │  文件映射 + 匿名映射（可随机化）
  │        ↓ ↑           │
  ├──────────────────────┤
  │       栈 (stack)     │  [start_stack], VM_READ|VM_WRITE|VM_GROWSDOWN
  │        ↓             │  自动向下扩展
  ├──────────────────────┤
  │     vDSO /  vsyscall │  内核辅助映射
  └──────────────────────┘ 0x00007fffffffffff
```

| VMA 类型 | 典型标志 | 说明 |
|----------|----------|------|
| 匿名映射 | `VM_READ\|VM_WRITE` | 堆、栈、mmap 匿名映射 |
| 文件映射 | `VM_READ\|VM_WRITE\|VM_SHARED` | 文件 mmap、共享库 |
| 私有文件映射 | `VM_READ\|VM_WRITE` | 可执行文件、私有 mmap |
| 栈 | `VM_READ\|VM_WRITE\|VM_GROWSDOWN` | 自动向下扩展 |
| 堆 | `VM_READ\|VM_WRITE` | brk 系统调用管理 |
| 巨页 | `VM_HUGETLB` | 透明巨页或显式巨页 |
| I/O 映射 | `VM_IO\|VM_PFNMAP` | 设备内存映射 |
| 特殊映射 | `VM_DONTCOPY\|VM_DONTEXPAND` | vDSO、vsyscall |

### 6.13 关键性能优化

| 优化 | 机制 | 收益 |
|------|------|------|
| **Maple Tree** | B 树索引替代红黑树 | O(log n) 区间查询，RCU 友好 |
| **Per-VMA Lock** | 单个 VMA 读写锁 | 多线程并发缺页，减少 mmap_lock 竞争 |
| **VMA 合并** | vma_merge_new_range 自动合并相邻 VMA | 减少 VMA 数量，降低查找开销 |
| **SLAB 缓存** | vm_area_cachep 专用 SLUB 缓存 | VMA 结构体快速分配/释放 |
| **RCU 释放** | SLAB_TYPESAFE_BY_RCU 安全释放 | 无锁 RCU 遍历 VMA 树 |
| **VMA 迭代器** | 封装 Maple Tree 操作 | 简化代码，统一遍历接口 |

---

## 7. DMA 一致性内存与流式内存

### 7.1 概述

DMA（Direct Memory Access）是设备直接读写系统内存的机制，不需要 CPU 参与数据搬运。Linux 内核的 DMA API 层为设备驱动提供了统一的接口，屏蔽了不同架构（x86/ARM/RISC-V）和不同 IOMMU 配置的差异。

**两种 DMA 映射类型**：

| 类型 | 典型 API | 特点 |
|------|----------|------|
| **一致性映射 (Coherent/DMA_ATOMIC)** | `dma_alloc_coherent()` | 分配后 CPU 和设备同时可访问，不需要同步操作。要求硬件保证缓存一致性，或分配非缓存（Uncached）内存 |
| **流式映射 (Streaming)** | `dma_map_single()` / `dma_map_sg()` | 每次 DMA 传输前后需要显式同步（`dma_sync_*`），适合临时性、频繁的 DMA 操作 |

**DMA API 分层架构**：

```
驱动层 (Driver)
    │
    ├─ dma_alloc_coherent / dma_map_single / dma_map_sg  ← 通用 API
    │
    ├─ DMA API 核心层 (kernel/dma/mapping.c)
    │    ├─ 检查 per-device coherent pool (dma_alloc_from_dev_coherent)
    │    ├─ 选择实现路径：dma_direct / IOMMU / arch_ops
    │    └─ 调试跟踪 (debug_dma_*)
    │
    ├─ Direct DMA (kernel/dma/direct.c + direct.h)
    │    ├─ 物理地址直接映射（无 IOMMU）
    │    ├─ 缓存同步 (arch_sync_dma_for_device/cpu)
    │    └─ SWIOTLB 回退（地址空间不足时）
    │
    ├─ IOMMU DMA (drivers/iommu/dma-iommu.c)
    │    ├─ 通过 IOMMU/SMMU 映射
    │    ├─ 提供虚拟 DMA 地址空间
    │    └─ 支持分散/聚合映射
    │
    └─ 架构后端 (arch/*/kernel/dma.c)
         ├─ arch_dma_alloc / arch_dma_free
         ├─ arch_sync_dma_for_device / arch_sync_dma_for_cpu
         └─ arch_dma_set_uncached / arch_dma_clear_uncached
```

**代码文件分布**：

| 文件 | 行数 | 职责 |
|------|------|------|
| `kernel/dma/mapping.c` | 1,016 | DMA API 核心层，路由到具体实现 |
| `kernel/dma/direct.c` | 665 | 直接 DMA 映射（无 IOMMU） |
| `kernel/dma/direct.h` | 138 | 直接 DMA 内联函数（核心逻辑） |
| `kernel/dma/coherent.c` | 411 | per-device 一致性内存池 |
| `kernel/dma/pool.c` | 309 | 原子 DMA 池（atomic pool） |
| `kernel/dma/swiotlb.c` | — | 软件 IO TLB 反弹缓冲 |
| `kernel/dma/contiguous.c` | — | CMA 连续内存分配器 |
| `kernel/dma/remap.c` | — | DMA 原子池的 remap 辅助 |
| `kernel/dma/debug.c` | 1,628 | DMA API 调试跟踪 |
| `include/linux/dma-map-ops.h` | — | `struct dma_map_ops` 定义 |
| `include/linux/dma-mapping.h` | — | 用户态 DMA API 声明 |
| `include/linux/dma-direction.h` | — | DMA 方向枚举 |

### 7.2 硬件原理

#### 7.2.1 缓存一致性 (Cache Coherency)

```
CPU 侧视角                   设备侧视角
┌──────────────┐            ┌──────────────┐
│   L1/L2 Cache │            │  DMA Engine  │
│  (可能含脏数据)│            │              │
└──────┬───────┘            └──────┬───────┘
       │                           │
       │  缓存行 (Cache Line)       │
       │  (64 字节)                │
       ▼                           ▼
┌─────────────────────────────────────────┐
│             系统内存 (DRAM)             │
│  ┌──────┬──────┬──────┬──────┬──────┐  │
│  │ 数据 │ 数据 │ 数据 │ 数据 │ 数据 │  │
│  └──────┴──────┴──────┴──────┴──────┘  │
└─────────────────────────────────────────┘
```

**一致性问题**：当 CPU 写数据到内存时，数据可能停留在 CPU Cache 中（写回策略），尚未写入 DRAM。如果此时设备发起 DMA 读取，将读到过时的数据。反之亦然。

**硬件解决方案**：

| 机制 | 架构 | 原理 |
|------|------|------|
| **硬件一致性 (HW Coherent)** | ARM64 (CCI/CMN)、x86 (MESI) | 总线监听协议，DMA 传输时自动 snoop CPU Cache，保证一致性。CPU 和设备看到相同的内存视图 |
| **非缓存映射 (Non-cacheable)** | 所有架构 | 将 DMA 内存区域设置为不可缓存的页表属性，CPU 访问时直接读写 DRAM，绕过 Cache |
| **写合并 (Write-Combine)** | x86 WC | 允许 CPU 写操作合并，适合帧缓冲区等场景 |
| **SWIOTLB 反弹缓冲** | x86 Xen、虚拟化 | 当设备无法直接访问目标地址时，通过预分配的物理连续缓冲区中转 |

**软件解决方案（流式映射）**：

```c
// CPU → Device 方向：CPU 写数据后，需要刷 Cache 确保设备看到最新数据
dma_map_single(dev, buf, size, DMA_TO_DEVICE);  // 隐式调用 arch_sync_dma_for_device
dma_unmap_single(dev, dma_addr, size, DMA_TO_DEVICE);  // 隐式调用 arch_sync_dma_for_cpu

// Device → CPU 方向：设备 DMA 完成后，需使 CPU Cache 行失效
dma_map_single(dev, buf, size, DMA_FROM_DEVICE);  // 刷 Cache 行（避免脏数据干扰）
dma_unmap_single(dev, dma_addr, size, DMA_FROM_DEVICE);  // 使 Cache 行失效

// 双向：DMA_BIDIRECTIONAL 对两个方向都进行同步
```

#### 7.2.2 IOMMU/SMMU 原理

```
无 IOMMU（直接 DMA）:
  设备 DMA 地址 = 物理地址
  ┌──────┐        ┌───────────────┐
  │ 设备 │───────▶│  物理内存      │
  └──────┘        │  0x4000_0000  │
                   └───────────────┘

有 IOMMU/SMMU:
  设备 DMA 地址 = IOMMU 虚拟地址
  ┌──────┐    ┌──────────┐    ┌───────────────┐
  │ 设备 │───▶│ IOMMU    │───▶│  物理内存      │
  └──────┘    │ 页表映射  │    │  (可非连续)    │
               └──────────┘    └───────────────┘
```

IOMMU 的作用：
- **地址翻译**：将设备 DMA 地址翻译为物理地址
- **分散/聚合**：允许物理非连续内存呈现为设备视角的连续 DMA 地址
- **访问控制**：限制设备只能访问被授权的内存区域
- **隔离**：不同设备之间、设备与 OS 之间相互隔离

### 7.3 核心数据结构（带详细注释）

#### 7.3.1 struct dma_map_ops — DMA 映射操作函数表

```c
// include/linux/dma-map-ops.h
struct dma_map_ops {
    // 一致性分配/释放
    void *(*alloc)(struct device *dev, size_t size,
                   dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs);
    void (*free)(struct device *dev, size_t size, void *vaddr,
                 dma_addr_t dma_handle, unsigned long attrs);

    // 基于页面的分配/释放
    struct page *(*alloc_pages_op)(struct device *dev, size_t size,
                   dma_addr_t *dma_handle, enum dma_data_direction dir, gfp_t gfp);
    void (*free_pages)(struct device *dev, size_t size, struct page *vaddr,
                       dma_addr_t dma_handle, enum dma_data_direction dir);

    // 流式映射：单页
    dma_addr_t (*map_phys)(struct device *dev, phys_addr_t phys,
                           size_t size, enum dma_data_direction dir,
                           unsigned long attrs);
    void (*unmap_phys)(struct device *dev, dma_addr_t dma_handle,
                       size_t size, enum dma_data_direction dir,
                       unsigned long attrs);

    // 流式映射：散列表
    int (*map_sg)(struct device *dev, struct scatterlist *sg, int nents,
                  enum dma_data_direction dir, unsigned long attrs);
    void (*unmap_sg)(struct device *dev, struct scatterlist *sg, int nents,
                     enum dma_data_direction dir, unsigned long attrs);

    // 缓存同步（非一致性设备）
    void (*sync_single_for_cpu)(struct device *dev, dma_addr_t dma_handle,
                                size_t size, enum dma_data_direction dir);
    void (*sync_single_for_device)(struct device *dev, dma_addr_t dma_handle,
                                   size_t size, enum dma_data_direction dir);
    void (*sync_sg_for_cpu)(struct device *dev, struct scatterlist *sg,
                            int nents, enum dma_data_direction dir);
    void (*sync_sg_for_device)(struct device *dev, struct scatterlist *sg,
                               int nents, enum dma_data_direction dir);

    // 其他
    int (*mmap)(struct device *, struct vm_area_struct *,
                void *, dma_addr_t, size_t, unsigned long attrs);
    int (*get_sgtable)(struct device *dev, struct sg_table *sgt, ...);
    int (*dma_supported)(struct device *dev, u64 mask);
    u64 (*get_required_mask)(struct device *dev);
    size_t (*max_mapping_size)(struct device *dev);
};
```

#### 7.3.2 struct device — DMA 相关字段

```c
// include/linux/device.h
struct device {
    const struct dma_map_ops *dma_ops;          // 设备 DMA 操作函数（架构相关）
    u64 *dma_mask;                               // DMA 地址掩码指针（流式映射）
    u64 coherent_dma_mask;                       // 一致性 DMA 地址掩码
    struct device_dma_parameters *dma_parms;     // DMA 参数（最大段大小等）
    struct dma_coherent_mem *dma_mem;            // per-device 一致性内存池
    bool dma_skip_sync;                          // 是否可跳过同步（一致性设备）
    struct cma *cma_area;                        // 关联的 CMA 区域
};
```

#### 7.3.3 struct dma_coherent_mem — 设备一致性内存池

```c
// kernel/dma/coherent.c
struct dma_coherent_mem {
    void        *virt_base;        // 虚拟地址基址（memremap 映射）
    dma_addr_t  device_base;       // 设备 DMA 地址基址
    unsigned long pfn_base;        // 物理页帧号基址
    int         size;              // 页面总数
    unsigned long *bitmap;         // 分配位图
    spinlock_t  spinlock;          // 保护位图的锁
    bool        use_dev_dma_pfn_offset; // 是否使用设备 DMA PFN 偏移
};
```

#### 7.3.4 enum dma_data_direction — DMA 传输方向

```c
// include/linux/dma-direction.h
enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,  // 双向：CPU 和设备都读写
    DMA_TO_DEVICE     = 1,  // 主机到设备：CPU 写，设备读
    DMA_FROM_DEVICE   = 2,  // 设备到主机：设备写，CPU 读
    DMA_NONE          = 3,  // 无方向（用于调试）
};
```

#### 7.3.5 struct dma_debug_entry — DMA 调试跟踪条目

```c
// kernel/dma/debug.c
struct dma_debug_entry {
    struct list_head list;           // 哈希链表
    struct device    *dev;           // 所属设备
    u64              dev_addr;       // DMA 地址
    u64              size;           // 映射大小
    int              type;           // dma_debug_coherent / dma_debug_phy / dma_debug_sg
    int              direction;      // DMA 方向
    int              sg_call_ents;   // SG 调用时条目数
    int              sg_mapped_ents; // 实际映射条目数
    phys_addr_t      paddr;          // 物理地址
    enum map_err_types map_err_type; // 是否检查过 dma_mapping_error
    bool             is_cache_clean; // 是否保证不写（优化）
};
```

### 7.4 一致性 DMA 分配与释放

一致性映射的特点是：分配后 CPU 和设备可以同时访问，不需要显式同步。适用于 DMA 描述符环、网络数据包缓冲区等场景。

一致性分配有 3 种路径，按优先级依次尝试：

```
dma_alloc_coherent(dev, size, dma_handle, gfp)
    │
    ├─ [1] per-device 一致性内存池 (dma_alloc_from_dev_coherent)
    │    ├─ 由 dma_declare_coherent_memory() 提前声明
    │    ├─ 从位图管理的预分配池中分配
    │    └─ 内核引导时由设备树指定
    │
    └─ [2] dma_alloc_attrs() 通用路径
         │
         ├─ [2a] Direct DMA (dma_alloc_direct / arch_dma_alloc_direct)
         │    └─ dma_direct_alloc()
         │         │
         │         ├─ [非一致性设备 + arch_dma_alloc 存在]
         │         │    └─ arch_dma_alloc()            // 架构特定实现
         │         │
         │         ├─ [非一致性设备 + DMA_GLOBAL_POOL]
         │         │    └─ dma_alloc_from_global_coherent()
         │         │
         │         ├─ [非一致性设备 + 需要 remap 或 set_uncached]
         │         │    ├─ 原子池路径 (dma_direct_alloc_from_pool)
         │         │    │    └─ 从 gen_pool 分配（atomic pool）
         │         │    └─ 正常路径
         │         │         ├─ __dma_direct_alloc_pages()  // 分配物理页
         │         │         │    ├─ CMA 优先
         │         │         │    └─ alloc_pages_node() 回退
         │         │         ├─ arch_dma_prep_coherent()     // 清理缓存行
         │         │         ├─ dma_common_contiguous_remap() // 非缓存重映射
         │         │         └─ arch_dma_set_uncached()       // 设置非缓存属性
         │         │
         │         └─ [一致性设备]
         │              └─ __dma_direct_alloc_pages()  // 直接分配
         │                   └─ 返回 page_address() 线性地址
         │
         ├─ [2b] IOMMU 路径 (use_dma_iommu)
         │    └─ iommu_dma_alloc()
         │         ├─ 分配物理页（可有 CMA）
         │         ├─ IOMMU 映射（建立设备页表）
         │         └─ 返回 CPU 虚拟地址
         │
         └─ [2c] 架构特定 ops
              └─ ops->alloc()
```

#### 7.4.1 dma_alloc_coherent 完整调用栈

```
dma_alloc_coherent(dev, size, dma_handle, gfp)            [include/linux/dma-mapping.h]
  │
  └─ dma_alloc_attrs(dev, size, dma_handle, flag, attrs)   [kernel/dma/mapping.c:622]
       │
       ├─ [1] 尝试 per-device coherent pool
       │    └─ dma_alloc_from_dev_coherent(dev, size, dma_handle, &cpu_addr)  [coherent.c]
       │         └─ __dma_alloc_from_coherent(dev, mem, size, dma_handle)
       │              ├─ bitmap_find_free_region(mem->bitmap, mem->size, order)
       │              ├─ *dma_handle = mem->device_base + (pageno << PAGE_SHIFT)
       │              └─ memset(ret, 0, size)
       │
       ├─ [2] 选择实现路径
       │    │
       │    ├─ Direct DMA 路径:
       │    │    dma_direct_alloc(dev, size, dma_handle, flag, attrs)  [direct.c:210]
       │    │    │
       │    │    ├─ [非一致性设备 + arch_dma_alloc]
       │    │    │    └─ arch_dma_alloc(dev, size, dma_handle, gfp, attrs)  // 架构实现
       │    │    │
       │    │    ├─ [非一致性设备 + DMA_GLOBAL_POOL]
       │    │    │    └─ dma_alloc_from_global_coherent(dev, size, dma_handle)  [coherent.c]
       │    │    │
       │    │    └─ [默认路径]
       │    │         ├─ [非一致性设备 + 阻塞限制]
       │    │         │    └─ dma_direct_alloc_from_pool(dev, size, dma_handle, gfp)  [direct.c:168]
       │    │         │         └─ dma_alloc_from_pool(dev, size, &ret, gfp, ...)  [pool.c:274]
       │    │         │              └─ __dma_alloc_from_pool(dev, size, pool, cpu_addr, ...)  [pool.c]
       │    │         │                   └─ gen_pool_alloc(pool, size)  // 通用内存池分配
       │    │         │
       │    │         └─ [正常路径]
       │    │              ├─ __dma_direct_alloc_pages(dev, size, gfp, true)  [direct.c:130]
       │    │              │    ├─ dma_alloc_contiguous(dev, size, gfp)   // CMA 优先
       │    │              │    └─ alloc_pages_node(node, gfp, order)     // 伙伴系统回退
       │    │              │
       │    │              ├─ arch_dma_prep_coherent(page, size)            // 清理缓存行
       │    │              │
       │    │              ├─ [需要 remap 或 HighMem]
       │    │              │    ├─ dma_common_contiguous_remap(page, size, prot, ...) [remap.c]
       │    │              │    └─ → 返回 vmap 地址（非缓存映射）
       │    │              │
       │    │              ├─ [需要 set_uncached]
       │    │              │    └─ arch_dma_set_uncached(ret, size)  // 架构实现
       │    │              │
       │    │              └─ memset(ret, 0, size)         // 清零
       │    │
       │    ├─ IOMMU 路径:
       │    │    └─ iommu_dma_alloc(dev, size, dma_handle, flag, attrs)  [drivers/iommu/...]
       │    │         ├─ 分配物理页（CMA 或伙伴系统）
       │    │         ├─ iommu_map(domain, iova, phys, size, IOMMU_CACHE)
       │    │         └─ 返回 CPU 可访问的虚拟地址
       │    │
       │    └─ 架构 ops 路径:
       │         └─ ops->alloc(dev, size, dma_handle, flag, attrs)
       │
       └─ [3] 调试跟踪
            └─ debug_dma_alloc_coherent(dev, size, *dma_handle, cpu_addr, attrs)
```

#### 7.4.2 dma_free_coherent 完整调用栈

```
dma_free_coherent(dev, size, cpu_addr, dma_handle)          [dma-mapping.h]
  │
  └─ dma_free_attrs(dev, size, cpu_addr, dma_handle, 0)     [mapping.c]
       │
       ├─ [1] 尝试 per-device coherent pool
       │    └─ dma_release_from_dev_coherent(dev, order, cpu_addr)  [coherent.c]
       │         └─ __dma_release_from_coherent(mem, order, vaddr)
       │              ├─ 检查 vaddr 是否在 mem->virt_base 范围内
       │              └─ bitmap_release_region(mem->bitmap, page, order)
       │
       ├─ [2] Direct DMA 路径
       │    └─ dma_direct_free(dev, size, cpu_addr, dma_handle, attrs)  [direct.c:310]
       │         ├─ arch_dma_free()              // 架构释放
       │         ├─ dma_release_from_global_coherent()
       │         ├─ dma_free_from_pool()         // 原子池释放
       │         ├─ vunmap(cpu_addr)             // 如果是 remap 的地址
       │         └─ __dma_direct_free_pages()    // 释放物理页
       │              ├─ swiotlb_free()
       │              └─ dma_free_contiguous()
       │
       ├─ [3] IOMMU 路径
       │    └─ iommu_dma_free(...)
       │
       └─ [4] 调试跟踪
            └─ debug_dma_free_coherent(...)
```

### 7.5 流式 DMA 映射与解映射

流式映射的特点是：每次 DMA 传输前后需要显式调用同步函数，适用于临时性、频繁的 DMA 操作（如网络收发、块设备 IO）。

```
驱动使用流式映射的典型流程：

  [CPU 填充数据]
       │
       ▼
  dma_map_single(dev, cpu_addr, size, DMA_TO_DEVICE)
       │  └─ 刷 Cache（非一致性设备）
       │  └─ SWIOTLB 反弹（地址不匹配时）
       │  └─ IOMMU 映射（有 IOMMU 时）
       ▼
  [获取 DMA 地址，提交给设备]
       │
       ▼
  设备执行 DMA 传输
       │
       ▼
  dma_unmap_single(dev, dma_addr, size, DMA_TO_DEVICE)
       │  └─ 使 Cache 失效（非一致性设备）
       │  └─ SWIOTLB 同步回拷
       │  └─ IOMMU 解映射
       ▼
  [CPU 读取数据]
```

#### 7.5.1 dma_map_single 完整调用栈

```
dma_map_single(dev, cpu_addr, size, dir)                        [dma-mapping.h]
  │
  └─ dma_map_single_attrs(dev, cpu_addr, size, dir, 0)
       │
       └─ dma_map_page_attrs(dev, virt_to_page(cpu_addr),        [mapping.c:186]
                              offset_in_page(cpu_addr), size, dir, 0)
            │
            └─ dma_map_phys(dev, phys, size, dir, attrs)         [mapping.c:148]
                 │
                 ├─ {dma_go_direct} → Direct DMA 路径
                 │    └─ dma_direct_map_phys(dev, phys, size, dir, attrs)  [direct.h:80]
                 │         │
                 │         ├─ [SWIOTLB 强制反弹]
                 │         │    └─ swiotlb_map(dev, phys, size, dir, attrs)  [swiotlb.c]
                 │         │         ├─ swiotlb_alloc_bounce()  // 分配反弹缓冲区
                 │         │         ├─ 复制数据到反弹缓冲区
                 │         │         └─ 返回反弹缓冲区的 DMA 地址
                 │         │
                 │         ├─ [MMIO 映射]
                 │         │    └─ dma_addr = phys  // 直接使用物理地址
                 │         │
                 │         ├─ [正常路径]
                 │         │    ├─ dma_addr = phys_to_dma(dev, phys)  // 物理→DMA 地址转换
                 │         │    ├─ dma_capable() 检查是否在设备 DMA 范围内
                 │         │    ├─ [地址溢出] → swiotlb_map() 回退
                 │         │    └─ [非一致性设备] arch_sync_dma_for_device(phys, size, dir)
                 │         │         └─ 架构刷 Cache 操作
                 │         │
                 │         └─ 返回 DMA 地址
                 │
                 ├─ IOMMU 路径
                 │    └─ iommu_dma_map_phys(dev, phys, size, dir, attrs)  [iommu-dma]
                 │         ├─ iommu_map(domain, iova, phys, size, prot)
                 │         └─ 返回 IOVA 地址
                 │
                 └─ ops->map_phys() 路径
                      └─ 架构特定实现
```

#### 7.5.2 dma_map_sg 完整调用栈

```
dma_map_sg(dev, sg, nents, dir)                               [dma-mapping.h]
  │
  └─ dma_map_sg_attrs(dev, sg, nents, dir, 0)                  [mapping.c:234]
       │
       ├─ Direct DMA 路径
       │    └─ dma_direct_map_sg(dev, sgl, nents, dir, attrs)  [direct.c:454]
       │         ├─ 遍历每个 scatterlist 条目
       │         ├─ [P2PDMA 映射] 直接使用 PCIe 总线地址
       │         └─ [普通映射] dma_direct_map_phys() 逐条映射
       │
       ├─ IOMMU 路径
       │    └─ iommu_dma_map_sg(dev, sg, nents, dir, attrs)  [iommu-dma]
       │         ├─ 合并连续物理页面
       │         ├─ iommu_map_sg() 建立 IOVA 映射
       │         └─ 返回映射后的条目数
       │
       └─ ops->map_sg() 路径
```

#### 7.5.3 缓存同步 API

```c
// 将数据所有权从 CPU 转移到设备
// 刷 Cache，确保设备看到最新数据
void __dma_sync_single_for_device(dev, addr, size, dir)       [mapping.c:388]
    ├─ Direct DMA: dma_direct_sync_single_for_device(dev, addr, size, dir)  [direct.h:68]
    │    ├─ swiotlb_sync_single_for_device(dev, paddr, size, dir)
    │    └─ [非一致性设备] arch_sync_dma_for_device(paddr, size, dir)
    ├─ IOMMU: iommu_dma_sync_single_for_device(dev, addr, size, dir)
    └─ ops->sync_single_for_device()

// 将数据所有权从设备转移到 CPU
// 使 Cache 行失效，确保 CPU 看到设备写入的最新数据
void __dma_sync_single_for_cpu(dev, addr, size, dir)          [mapping.c:376]
    ├─ Direct DMA: dma_direct_sync_single_for_cpu(dev, addr, size, dir)  [direct.h:74]
    │    ├─ [非一致性设备] arch_sync_dma_for_cpu(paddr, size, dir)  // 使 Cache 失效
    │    └─ swiotlb_sync_single_for_cpu(dev, paddr, size, dir)  // 从反弹区拷回
    ├─ IOMMU: iommu_dma_sync_single_for_cpu(dev, addr, size, dir)
    └─ ops->sync_single_for_cpu()
```

### 7.6 原子 DMA 池 (Atomic Pool)

原子池用于在不允许阻塞（`GFP_ATOMIC`）的上下文中分配一致性内存。核心实现位于 `kernel/dma/pool.c`。

**初始化**：
- 默认大小：128KB/GB 内存，最小 128KB，最大 `MAX_PAGE_ORDER`
- 可通过内核参数 `coherent_pool=` 设置
- 分为 3 个 gen_pool：`atomic_pool_dma`、`atomic_pool_dma32`、`atomic_pool_kernel`

**架构**：
```
atomic_pool_dma (ZONE_DMA)    ← 用于 GFP_DMA 分配
atomic_pool_dma32 (ZONE_DMA32) ← 用于 GFP_DMA32 分配
atomic_pool_kernel (常规)      ← 用于其他 GFP 分配
```

**分配流程**：
```
dma_alloc_from_pool(dev, size, cpu_addr, gfp, phys_addr_ok)
    │
    └─ dma_guess_pool(pool, gfp)  // 根据 gfp 选择 pool
         │
         └─ __dma_alloc_from_pool(dev, size, pool, cpu_addr, phys_addr_ok)
              ├─ gen_pool_alloc(pool, size)  // 从通用池分配
              ├─ 检查物理地址是否在设备 DMA 范围内
              └─ 返回虚拟地址
```

### 7.7 SWIOTLB 反弹缓冲机制

SWIOTLB 是软件实现的 IO TLB（Translation Lookaside Buffer），用于以下场景：

- 设备 DMA 地址范围有限（如 32 位设备在 64 位系统上）
- 虚拟化环境（Xen/KVM）中设备无法访问客户机物理地址
- 需要强制反弹的统一设备架构

**核心原理**：
```
设备 DMA 地址范围: 0x0000_0000 ~ 0x00FF_FFFF (24位)

CPU 分配的数据缓冲区: 物理地址 0x1_0000_0000 (超出设备范围)
                                  │
                                  ▼
                        SWIOTLB 反弹缓冲区
                        (预分配，在设备 DMA 范围内)
                                  │
                        ┌────────┴────────┐
                        │                 │
                        ▼                 ▼
                    dma_map:          dma_unmap:
                    拷贝数据到         从反弹区拷回
                    SWIOTLB 区        数据到原缓冲区
                        │                 │
                        └────────┬────────┘
                                 │
                         设备 DMA 操作
```

### 7.8 DMA 方向与同步语义

```
DMA_TO_DEVICE (CPU → Device):
  dma_map:   arch_sync_dma_for_device()  → 刷 Cache（将 CPU 写的数据推到内存）
  dma_unmap: 不需要同步（或仅 SWIOTLB 同步）

DMA_FROM_DEVICE (Device → CPU):
  dma_map:   arch_sync_dma_for_device()  → 刷 Cache（丢弃可能存在的脏缓存行）
  dma_unmap: arch_sync_dma_for_cpu()     → 使 Cache 失效（读取设备写入的数据）

DMA_BIDIRECTIONAL:
  dma_map:   arch_sync_dma_for_device()  → 刷 Cache
  dma_unmap: arch_sync_dma_for_cpu()     → 使 Cache 失效
```

### 7.9 DMA 掩码 (DMA Mask)

设备通过 DMA 掩码声明其可以访问的地址范围：

```c
// 设备驱动初始化时调用
dma_set_mask(dev, DMA_BIT_MASK(32));           // 流式映射：32位地址
dma_set_coherent_mask(dev, DMA_BIT_MASK(32));  // 一致性映射：32位地址

// 或同时设置
dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));  // 64位

// 内核检查地址是否可达
dma_capable(dev, dma_addr, size, true)
    └─ dma_addr + size - 1 <= min(dev->dma_mask, dev->bus_dma_limit)
```

### 7.10 DMA 完整 API 接口汇总

| 类别 | API | 功能 |
|------|-----|------|
| **一致性分配** | `dma_alloc_coherent()` | 分配一致性内存 |
| | `dma_free_coherent()` | 释放一致性内存 |
| | `dma_alloc_noncontiguous()` | 分配非连续一致性内存（SG 表） |
| | `dma_free_noncontiguous()` | 释放非连续一致性内存 |
| | `dma_alloc_pages()` | 基于页面的分配 |
| | `dma_free_pages()` | 基于页面的释放 |
| **流式映射** | `dma_map_single()` | 映射单缓冲区 |
| | `dma_unmap_single()` | 解映射单缓冲区 |
| | `dma_map_page()` | 映射内存页 |
| | `dma_unmap_page()` | 解映射内存页 |
| | `dma_map_sg()` | 映射散列表 |
| | `dma_unmap_sg()` | 解映射散列表 |
| | `dma_map_sgtable()` | 映射 SG 表 |
| | `dma_unmap_sgtable()` | 解映射 SG 表 |
| | `dma_map_resource()` | 映射 MMIO 资源 |
| | `dma_unmap_resource()` | 解映射 MMIO 资源 |
| **缓存同步** | `dma_sync_single_for_cpu()` | 单缓冲区所有权→CPU |
| | `dma_sync_single_for_device()` | 单缓冲区所有权→设备 |
| | `dma_sync_sg_for_cpu()` | 散列表所有权→CPU |
| | `dma_sync_sg_for_device()` | 散列表所有权→设备 |
| **掩码设置** | `dma_set_mask()` | 设置流式映射 DMA 掩码 |
| | `dma_set_coherent_mask()` | 设置一致性映射 DMA 掩码 |
| | `dma_set_mask_and_coherent()` | 同时设置两种掩码 |
| | `dma_get_required_mask()` | 获取系统所需的最小掩码 |
| **调试** | `dma_mapping_error()` | 检查映射是否成功 |
| | `dma_debug_add()` | 添加调试条目 |
| **辅助** | `dma_alloc_attrs()` | 带属性的一致性分配 |
| | `dma_free_attrs()` | 带属性的一致性释放 |
| | `dma_mmap_coherent()` | 将一致性内存映射到用户空间 |
| | `dma_get_sgtable()` | 获取 SG 表 |
| | `dma_max_mapping_size()` | 获取设备最大映射大小 |

### 7.11 关键性能优化

| 优化 | 机制 | 收益 |
|------|------|------|
| **Per-Device 一致性池** | 预分配位图管理 | 避免每次分配走伙伴系统，低延迟 |
| **原子 DMA 池** | GFP_ATOMIC 时从 gen_pool 分配 | 不阻塞分配路径 |
| **CMA 连续内存** | 优先从 CMA 分配大块连续内存 | 减少碎片，提高分配成功率 |
| **SWIOTLB 缓存** | 复用反弹缓冲区 | 减少分配/释放开销 |
| **dma_skip_sync 优化** | 一致性设备跳过缓存同步 | 减少架构无关的 Cache 操作 |
| **IOMMU 绕过** | 直接映射时跳过 IOMMU | 减少页表操作延迟 |
| **缓存同步合并** | 批量刷 Cache 行 | 减少单次刷 Cache 的开销 |
| **P2PDMA 绕过** | PCIe P2P 直接使用总线地址 | 减少主机内存的 DMA 映射开销 |