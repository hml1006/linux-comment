# Linux 7.0 内存管理（Memory Management）代码分析报告

## 目录

### Part I: 概述与核心框架

1. [总体概览](#1-总体概览)
2. [核心数据结构](#2-核心数据结构)
3. [物理内存管理（Buddy System）](#3-物理内存管理buddy-system)
4. [SLUB 分配器与 Sheaves 缓存机制](#4-slub-分配器与-sheaves-缓存机制)
5. [虚拟内存管理](#5-虚拟内存管理)
6. [VMA 管理](#6-vma-管理)
7. [DMA 一致性内存与流式内存](#7-dma-一致性内存与流式内存)

### Part II: 内存回收与交换

8. [页面回收（Reclaim）](#8-页面回收reclaim)
9. [交换（Swap）](#9-交换swap)
10. [压缩与页面迁移](#10-压缩与页面迁移)
11. [Zswap 与 Zsmalloc](#11-zswap-与-zsmalloc)

### Part III: 内核对象与页缓存

12. [文件页缓存（Page Cache）](#12-文件页缓存page-cache)
13. [写回机制（Writeback）](#13-写回机制writeback)
14. [Mempool 内存池](#14-mempool-内存池)
15. [Per-CPU 分配器](#15-per-cpu-分配器)

### Part IV: 控制与隔离

16. [Memory Cgroup](#16-memory-cgroup)
17. [NUMA 与内存策略](#17-numa-与内存策略)
18. [CMA 连续内存分配器](#18-cma-连续内存分配器)
19. [内存热插拔](#19-内存热插拔)

### Part V: 高级特性

20. [透明大页（THP）与 KSM](#20-透明大页thp与-ksm)
21. [内存错误处理（OOM/故障/泄漏）](#21-内存错误处理oom故障泄漏)
22. [DAMON 数据访问监控](#22-damon-数据访问监控)
23. [调试与安全工具](#23-调试与安全工具)

### Part VI: 附录

24. [总结](#24-总结)

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
    // alloc_vmap_area 在 vmap 地址空间红黑树中查找空闲区间
    // 成功后将 va 插入 busy 树，并关联到 area
    return area;
}
```

**`alloc_vmap_area`** — 在 vmap 地址空间分配空闲区间：

```c
// mm/vmalloc.c
static struct vmap_area *alloc_vmap_area(unsigned long size,
        unsigned long align, unsigned long vstart, unsigned long vend,
        int node, gfp_t gfp_mask, unsigned long va_flags, struct vm_struct *vm)
{
    // 1. 尝试从 vmap_node pool 分配（大小分级存储，快速路径）
    va = node_alloc(size, align, vstart, vend, &addr, &vn_id);
    if (!va) {
        // 2. 分配新的 vmap_area 结构体
        va = kmem_cache_alloc_node(vmap_area_cachep, gfp_mask, node);
    }

    // 3. 在全局空闲红黑树中查找满足条件的空闲区间
    addr = __alloc_vmap_area(&free_vmap_area_root, &free_vmap_area_list,
                             size, align, vstart, vend);

    // 4. 初始化 va 并插入忙碌树
    va->va_start = addr;
    va->va_end = addr + size;
    insert_vmap_area(va, &vn->busy, ...);
    setup_vmalloc_vm(va, vm, ...);  // 关联 vm_struct
    return va;
}
```

**`__vmalloc_area_node`** — 分配物理页面并建立映射：

```c
// mm/vmalloc.c
static void *__vmalloc_area_node(struct vm_struct *area, gfp_t gfp_mask,
                                 pgprot_t prot, unsigned int page_shift, int node)
{
    unsigned int nr_small_pages = size >> PAGE_SHIFT;

    // 1. 分配 pages 数组（存储物理页指针）
    if (array_size > PAGE_SIZE)
        area->pages = __vmalloc_node(array_size, ...);  // 大数组递归
    else
        area->pages = kmalloc_node(array_size, ...);    // 小数组直接 kmalloc

    // 2. 批量分配物理页面
    area->nr_pages = vm_area_alloc_pages(gfp, node, page_order,
                                         nr_small_pages, area->pages);

    // 3. 建立页表映射（虚拟地址 → 物理地址）
    vmap_pages_range(addr, addr + size, prot, area->pages, page_shift);

    return area->addr;
}
```

**`vm_area_alloc_pages`** — 批量分配物理页面：

```c
// mm/vmalloc.c
vm_area_alloc_pages(gfp_t gfp, int nid, unsigned int order,
                    unsigned int nr_pages, struct page **pages)
{
    // 1. 尝试大 order 分配（巨页优化）
    while (large_order > order && nr_remaining) {
        page = alloc_pages_noprof(large_gfp, large_order);
        if (page) {
            split_page(page, large_order);  // 拆分复合页
            // 填充 pages[] 数组
        }
    }

    // 2. order-0 批量分配（一次最多 100 页）
    while (nr_allocated < nr_pages) {
        nr = alloc_pages_bulk_array_mempolicy(gfp, ...);  // 批量
        // 批量不足时逐页回退
    }
}
```

#### 5.1.5 vfree 释放函数调用栈

```
vfree(addr)                                              [mm/vmalloc.c:3442]
  │
  ├─ in_interrupt() ? → vfree_atomic(addr)              // 中断上下文走异步路径
  │
  ├─ remove_vm_area(addr)                                // 从忙碌树移除
  │    └─ find_vmap_area(addr)                           // 红黑树查找
  │         └─ __find_vmap_area(addr, &vn->busy.root)
  │    └─ unmap_vmap_area(va)                            // 清除页表映射
  │         └─ vunmap_p4d_range() → ... → pte_clear()
  │    └─ vn->busy 中移除 va
  │
  ├─ [VM_FLUSH_RESET_PERMS] → vm_reset_perms(vm)        // 重置权限
  │
  ├─ 逐页释放物理页面:
  │    for i = 0 .. vm->nr_pages:
  │        __free_page(vm->pages[i])                     // 伙伴系统回收
  │
  ├─ atomic_long_sub(nr_pages, &nr_vmalloc_pages)        // 更新统计
  │
  ├─ kvfree(vm->pages)                                   // 释放 pages 数组
  │    └─ is_vmalloc_addr ? vfree : kfree                // 自动选择
  │
  └─ kfree(vm)                                           // 释放 vm_struct
```

#### 5.1.6 vmap_node 多节点架构

Linux 7.0+ 的 vmalloc 引入了 **vmap_node** 机制，将全局 vmap 地址空间划分为多个节点，每个节点管理自己的空闲/忙碌红黑树，减少全局锁竞争。

```
传统模型（单节点）:
  [全局 free_vmap_area_root + free_vmap_area_lock]
       ↑ 所有 CPU 竞争同一把锁

vmap_node 模型（多节点）:
  [vmap_nodes[0]]    [vmap_nodes[1]]    [vmap_nodes[N]]
  ├─ busy 树         ├─ busy 树         ├─ busy 树
  ├─ lazy 树         ├─ lazy 树         ├─ lazy 树
  ├─ pool[0..255]    ├─ pool[0..255]    ├─ pool[0..255]
  └─ pool_lock       └─ pool_lock       └─ pool_lock
```

**地址到节点映射**：`addr_to_node_id(addr) = (addr / vmap_zone_size) % nr_vmap_nodes`

**大小分级存储（pool）**：`vmap_pool` 数组包含 256 个链表，每个链表存储特定大小范围（页数+1）的空闲 vmap_area，避免频繁红黑树操作。

#### 5.1.7 关键接口汇总

| 接口 | 功能 | 底层实现 |
|------|------|----------|
| `vmalloc(size)` | 分配虚拟连续内存（GFP_KERNEL） | `__vmalloc_node_range()` |
| `vzalloc(size)` | 分配并清零 | `vmalloc()` + `__GFP_ZERO` |
| `vfree(addr)` | 释放 vmalloc 内存 | `remove_vm_area()` + `__free_page()` |
| `vmap(pages, count, flags, prot)` | 映射一组已有页面 | `alloc_vmap_area()` + `vmap_pages_range()` |
| `vunmap(addr)` | 解除映射（不释放物理页） | `remove_vm_area()` + `unmap_vmap_area()` |
| `ioremap(phys_addr, size)` | 映射设备 I/O 内存 | `__ioremap()` → `vmap()` |
| `__vmalloc(size, gfp)` | 指定 GFP 标志的 vmalloc | `__vmalloc_node_range()` |
| `vmalloc_huge_node(size, flags, node)` | 允许巨页的 vmalloc | `__vmalloc_node_range()` + `VM_ALLOW_HUGE_VMAP` |

#### 5.1.8 性能优化

| 优化 | 机制 | 收益 |
|------|------|------|
| **vmap_node 分区** | 地址空间分片，每片独立红黑树 | 减少全局锁竞争 |
| **大小分级存储 (pool)** | ≤256 页的空闲 vmap_area 按大小分类缓存 | 避免红黑树查找 |
| **巨页分配** | 大 order 物理页分配 + split_page | 减少页表层级深度 |
| **批量页面分配** | alloc_pages_bulk_array_mempolicy() | 减少伙伴系统锁竞争 |
| **惰性释放** | lazy 树 + purge_work 异步清理 | 延迟 TLB flush 批量处理 |

### 5.2 缺页处理

文件：`mm/memory.c`（7,493 行），`arch/arm64/mm/fault.c`（987 行）

缺页异常（Page Fault）是虚拟内存的核心机制，CPU 在访问一个虚拟地址时，若该地址对应的页表项不存在或权限不匹配，则触发缺页异常，由内核的缺页处理程序负责分配物理页面、建立页表映射。

#### 5.2.1 架构概述

缺页处理的整体架构分为三层：

1. **CPU 异常入口层**（arch-specific）：捕获缺页异常，从 FAR_EL1 寄存器读取故障地址，解析 ESR（Exception Syndrome Register），通过 `fault_info` 表分派到具体的处理函数
2. **内核通用处理层**（mm/memory.c）：VMA 查找、权限检查、页表遍历、根据缺页类型（匿名/文件/交换/COW/NUMA）分派到具体处理函数
3. **具体缺页处理层**：分配物理页面、建立页表映射、处理反向映射、更新统计信息

**ARM64 异常入口**：ARM64 的缺页异常通过异常向量表分派到不同的入口函数：

| 异常类型 | EC 编码 | 入口函数 | 描述 |
|----------|---------|----------|------|
| DABT_LOW | 0x24 | `el0_da()` | EL0 数据访问中止（用户态读写） |
| IABT_LOW | 0x20 | `el0_ia()` | EL0 指令访问中止（用户态取指） |
| DABT_CUR | 0x25 | `el1_abort()` | EL1 数据访问中止（内核态读写） |
| IABT_CUR | 0x21 | `el1_abort()` | EL1 指令访问中止（内核态取指） |

所有入口最终调用 `do_mem_abort(far, esr, regs)`，通过 ESR 的 FSC（Fault Status Code）字段索引 `fault_info` 表分派。

**ARM64 ESR 寄存器**（`arch/arm64/include/asm/esr.h`）：ESR_ELx 寄存器编码了异常类型和原因，关键字段如下：

```
ESR_ELx 寄存器格式：
  [31:26]  EC  - Exception Class（异常类别）
  [25]     IL  - Instruction Length（指令长度）
  [24:0]   ISS - Instruction Specific Syndrome（指令相关信息）

对于 Data Abort（DABT），ISS 字段包含：
  [24]     ISV  - Instruction Specific Syndrome Valid
  [23]     SAS  - Syndrome Access Size
  [22]     SSE  - Syndrome Sign Extend
  [21]     SF   - Sixty Four bit register
  [20]     AR   - Acquire Release
  [15:14]  CM   - Cache Maintenance
  [13]     WnR  - Write not Read（1=写，0=读）
  [12]     TnD  - Tag not Data (MTE)
  [11]     TagAccess - Tag access (MTE)
  [10]     GCS  - Guarded Control Stack
  [9]      Overlay - Overlay access
  [8]      DirtyBit - Dirty bit access
  [6:0]    FSC  - Fault Status Code

FSC 编码（fault_info 表索引）：
  0x00-0x03  Address size fault
  0x04-0x07  Translation fault（缺页）
  0x08-0x0B  Access flag fault
  0x0C-0x0F  Permission fault
  0x10       Alignment fault
  0x11       TLB conflict abort
  0x14       Synchronous external abort
  0x15       Synchronous tag check fault (MTE)
  0x18-0x1F  SError / SEA
```

**ARM64 fault_info 处理表**（`arch/arm64/mm/fault.c`）：

```
FSC  | 处理函数               | 信号         | 描述
─────|────────────────────────|──────────────|──────────────────────────
0x00-0x03 | do_bad             | SIGKILL      | 地址大小故障
0x04-0x07 | do_translation_fault | SIGSEGV    | 转换故障（缺页）
0x08-0x0B | do_page_fault      | SIGSEGV      | 访问标志故障
0x0C-0x0F | do_page_fault      | SIGSEGV      | 权限故障
0x10      | do_alignment_fault | SIGBUS       | 对齐故障
0x11      | do_bad             | SIGKILL      | TLB 冲突
0x14      | do_sea             | SIGBUS       | 同步外部异常（RAS）
0x15      | do_tag_check_fault | SIGSEGV      | MTE 标签检查故障
0x18-0x1F | do_sea             | SIGKILL/SIGBUS | 同步外部异常（页表遍历）
```

**FAULT_FLAG 标志**（`include/linux/mm_types.h`）：

```
FAULT_FLAG_WRITE        = 1 << 0    // 写缺页
FAULT_FLAG_MKWRITE      = 1 << 1    // 使页面可写
FAULT_FLAG_ALLOW_RETRY  = 1 << 2    // 允许重试
FAULT_FLAG_RETRY_NOWAIT = 1 << 3    // 重试但不等待
FAULT_FLAG_KILLABLE     = 1 << 4    // 可被致命信号中断
FAULT_FLAG_TRIED        = 1 << 5    // 已重试过一次
FAULT_FLAG_USER         = 1 << 6    // 来自用户态
FAULT_FLAG_REMOTE       = 1 << 7    // 远程地址空间
FAULT_FLAG_INSTRUCTION  = 1 << 8    // 取指令缺页
FAULT_FLAG_INTERRUPTIBLE= 1 << 9    // 可被信号中断（非致命）
FAULT_FLAG_UNSHARE      = 1 << 10   // 解除共享（KSM）
FAULT_FLAG_ORIG_PTE_VALID = 1 << 11 // orig_pte 有效
FAULT_FLAG_VMA_LOCK     = 1 << 12   // 使用 Per-VMA Lock
```

#### 5.2.2 完整调用栈

```
ARM64 缺页异常入口
────────────────────────────────────────────────────────────────────────
异常向量表 → el1_sync / el0_sync           // ARM64 异常向量表入口
  ├─ [EL1 DABT/IABT] → el1_abort()         // 内核态缺页
  │    ├─ enter_from_kernel_mode()          // 异常上下文管理
  │    ├─ read_sysreg(far_el1)              // 读取故障地址寄存器
  │    ├─ do_mem_abort(far, esr, regs)      // 中央分派点
  │    │    ├─ esr_to_fault_info(esr)       // 解析 ESR 获取处理函数
  │    │    ├─ inf->fn(far, esr, regs)      // 调用具体处理函数
  │    │    │    ├─ [FSC=0x04-0x07] → do_translation_fault()
  │    │    │    │    ├─ [is_ttbr0_addr] → do_page_fault()
  │    │    │    │    └─ [内核地址] → do_bad_area()
  │    │    │    ├─ [FSC=0x08-0x0F] → do_page_fault()  // 访问标志/权限故障
  │    │    │    ├─ [FSC=0x14] → do_sea()               // 同步外部异常
  │    │    │    ├─ [FSC=0x15] → do_tag_check_fault()   // MTE 标签故障
  │    │    │    ├─ [FSC=0x10] → do_alignment_fault()   // 对齐故障
  │    │    │    └─ [其他] → do_bad()                    // 未识别故障
  │    │    └─ [用户态 && 未处理] → arm64_notify_die()
  │    │    └─ [内核态] → die_kernel_fault() / __do_kernel_fault()
  │    ├─ local_daif_mask()
  │    └─ exit_to_kernel_mode()
  │
  └─ [EL0 DABT] → el0_da() / [EL0 IABT] → el0_ia()  // 用户态缺页
       ├─ arm64_enter_from_user_mode()      // 用户态异常进入
       ├─ [el0_ia && 内核地址] → arm64_apply_bp_hardening()  // 分支预测加固
       ├─ do_mem_abort(far, esr, regs)      // 中央分派点（同上）
       └─ arm64_exit_to_user_mode()

ARM64 do_page_fault() 核心处理
────────────────────────────────────────────────────────────────────────
do_page_fault(far, esr, regs)                // arch/arm64/mm/fault.c
  ├─ kprobe_page_fault()                    // kprobes 钩子
  ├─ faulthandler_disabled() || !mm → no_context  // 中断上下文直接 Oops
  ├─ 解析 ESR 获取 vm_flags 和 mm_flags:
  │    ├─ is_el0_instruction_abort() → VM_EXEC + FAULT_FLAG_INSTRUCTION
  │    ├─ is_gcs_fault() → VM_WRITE + FAULT_FLAG_WRITE (GCS)
  │    └─ is_write_abort() → VM_WRITE/VM_READ + FAULT_FLAG_WRITE
  ├─ is_el1_permission_fault() → die_kernel_fault()  // 内核访问用户内存检查
  ├─ perf_sw_event(PGFAULT)                 // 缺页性能事件
  │
  ├─ [Per-VMA Lock 快速路径]                // 用户态缺页优先尝试
  │    ├─ lock_vma_under_rcu(mm, addr)      // 获取 VMA 读锁
  │    │    ├─ [成功] → 权限检查 + handle_mm_fault(FAULT_FLAG_VMA_LOCK)
  │    │    │    ├─ [VM_FAULT_RETRY] → 降级到 mmap_lock 慢路径
  │    │    │    └─ [成功] → vma_end_read() + done
  │    │    └─ [失败] → 降级到 mmap_lock 慢路径
  │    └─ [signals] → 信号处理
  │
  └─ [mmap_lock 慢路径]
       ├─ lock_mm_and_find_vma(mm, addr, regs)  // 获取 mmap_lock + 查找 VMA
       │    ├─ [VMA 不存在] → SEGV_MAPERR
       │    └─ [VMA 权限不匹配] → SEGV_ACCERR
       ├─ handle_mm_fault(vma, addr, mm_flags, regs)
       │    └─ ... (见下文)
       ├─ [VM_FAULT_RETRY] → FAULT_FLAG_TRIED + 重试
       ├─ [VM_FAULT_COMPLETED] → 直接返回
       └─ mmap_read_unlock(mm)

核心缺页处理（通用层，与 x86 相同）
────────────────────────────────────────────────────────────────────────
handle_mm_fault(vma, address, flags, regs)  // mm/memory.c
  ├─ sanitize_fault_flags()                // 校验 FAULT_FLAG 合法性
  ├─ arch_vma_access_permitted()           // 架构级访问权限检查
  ├─ mem_cgroup_enter_user_fault()         // memcg OOM 处理
  ├─ lru_gen_enter_fault()                 // LRU 代际标记
  ├─ is_vm_hugetlb_page(vma)?              // 巨页判断
  │    ├─ [是] → hugetlb_fault()           // 巨页缺页
  │    └─ [否] → __handle_mm_fault()       // 普通页缺页
  ├─ lru_gen_exit_fault()
  └─ mm_account_fault()                    // 缺页统计（major/minor）

__handle_mm_fault(vma, address, flags)      // mm/memory.c
  ├─ 初始化 struct vm_fault vmf             // 填充故障上下文
  ├─ pgd_offset() → pgd_alloc()            // PGD 级（不存在则分配）
  ├─ pud_alloc()                           // PUD 级
  │    ├─ pud_none() && THP PUD 允许 → create_huge_pud()
  │    └─ pud_trans_huge() → wp_huge_pud() / huge_pud_set_accessed()
  ├─ pmd_alloc()                           // PMD 级
  │    ├─ pmd_none() && THP PMD 允许 → create_huge_pmd()
  │    ├─ pmd_none() → fallback 到 PTE 级
  │    ├─ pmd_trans_huge()
  │    │    ├─ pmd_protnone() → do_huge_pmd_numa_page()
  │    │    ├─ pmd_write() 失败 → wp_huge_pmd()
  │    │    └─ 正常 → huge_pmd_set_accessed()
  │    └─ pmd_device_private() → do_huge_pmd_device_private()
  └─ handle_pte_fault(&vmf)                // PTE 级缺页处理

handle_pte_fault(vmf)                       // mm/memory.c
  ├─ pmd_none() → vmf->pte = NULL          // PTE 页表不存在
  ├─ pte_offset_map_rw_nolock()            // 映射 PTE 页表
  │    └─ pte_none() → vmf->pte = NULL     // PTE 条目为空
  │
  ├─ [PTE 不存在] → do_pte_missing(vmf)    // 缺页（页面不存在）
  │    ├─ vma_is_anonymous() → do_anonymous_page()
  │    └─ 文件映射 → do_fault()
  │         ├─ 读缺页 → do_read_fault()
  │         │    ├─ do_fault_around()      // 预读周围页面
  │         │    ├─ __do_fault() → vma->vm_ops->fault()
  │         │    └─ finish_fault()         // 建立 PTE 映射
  │         ├─ 写时复制 → do_cow_fault()
  │         │    ├─ folio_prealloc()       // 分配匿名页
  │         │    ├─ __do_fault()           // 读取原始页
  │         │    ├─ copy_mc_user_highpage()// 拷贝数据
  │         │    └─ finish_fault()         // 建立 PTE 映射
  │         └─ 共享写 → do_shared_fault()
  │              ├─ __do_fault()
  │              ├─ do_page_mkwrite()      // 通知文件系统
  │              ├─ finish_fault()
  │              └─ fault_dirty_shared_page() // 标记脏页
  │
  ├─ [PTE 存在但非 Present] → do_swap_page(vmf)  // 换出页
  │    ├─ softleaf_is_swap() → swap 路径
  │    │    ├─ swap_cache_get_folio()      // 检查 swap cache
  │    │    ├─ swapin_readahead()          // 从交换区读入
  │    │    └─ swap_read_folio() + finish_fault()
  │    ├─ softleaf_is_migration() → 迁移等待
  │    ├─ softleaf_is_device_private() → migrate_to_ram
  │    ├─ softleaf_is_hwpoison() → VM_FAULT_HWPOISON
  │    └─ softleaf_is_marker() → handle_pte_marker()
  │
  ├─ [PTE protnone] → do_numa_page(vmf)   // NUMA 均衡缺页
  │    ├─ numa_migrate_check()            // NUMA 迁移决策
  │    └─ migrate_misplaced_folio()       // 迁移到正确节点
  │
  └─ [PTE Present + 权限匹配] → 正常访问
       ├─ FAULT_FLAG_WRITE/UNSHARE 且 !pte_write()
       │    └─ do_wp_page(vmf)            // 写时复制
       │         ├─ userfaultfd_wp() → handle_userfault()
       │         ├─ wp_page_reuse()        // 可重用（独占匿名页）
       │         └─ wp_page_copy()         // 复制页面
       │              ├─ folio_prealloc()  // 分配新页
       │              ├─ copy_user_highpage() // 拷贝数据
       │              ├─ page_add_new_anon_rmap()  // 反向映射
       │              └─ set_pte_at()      // 建立新 PTE
       └─ pte_mkyoung() + ptep_set_access_flags()  // 更新访问位
```

#### 5.2.3 核心数据结构

**struct vm_fault** — 缺页上下文（`include/linux/mm.h`）：

```c
struct vm_fault {
    // 不可变部分（由 __handle_mm_fault 初始化）
    struct vm_area_struct *vma;       // 目标 VMA
    gfp_t gfp_mask;                   // 分配掩码
    pgoff_t pgoff;                    // 逻辑页偏移（基于 vma）
    unsigned long address;            // 故障地址（页对齐）
    unsigned long real_address;       // 故障地址（原始值）

    // 标志位
    enum fault_flag flags;            // FAULT_FLAG_xxx

    // 页表指针
    pmd_t *pmd;                       // PMD 条目指针
    pud_t *pud;                       // PUD 条目指针

    // 原始页表条目值
    union {
        pte_t orig_pte;               // PTE 故障时的原始值
        pmd_t orig_pmd;               // PMD 故障时的原始值
    };

    // 页面处理
    struct page *cow_page;            // COW 时预分配的页面
    struct page *page;                // 处理程序返回的页面

    // 仅在持有 ptl 锁时有效
    pte_t *pte;                       // PTE 条目指针
    spinlock_t *ptl;                  // 页表锁
    pgtable_t prealloc_pte;           // 预分配的 PTE 页表
};
```

**vm_fault_t 返回值**（`include/linux/mm_types.h`）：

```
VM_FAULT_OOM            = 0x000001   // 内存耗尽
VM_FAULT_SIGBUS         = 0x000002   // 总线错误
VM_FAULT_MAJOR          = 0x000004   // 主缺页（磁盘 I/O）
VM_FAULT_HWPOISON       = 0x000010   // 硬件损坏页
VM_FAULT_HWPOISON_LARGE = 0x000020   // 大页硬件损坏
VM_FAULT_SIGSEGV        = 0x000040   // 段错误
VM_FAULT_NOPAGE         = 0x000100   // 无需返回页面
VM_FAULT_LOCKED         = 0x000200   // 页面已锁定
VM_FAULT_RETRY          = 0x000400   // 需要重试
VM_FAULT_FALLBACK       = 0x000800   // 回退到更小粒度
VM_FAULT_DONE_COW       = 0x001000   // COW 已完成
VM_FAULT_NEEDDSYNC      = 0x002000   // 需要同步
VM_FAULT_COMPLETED      = 0x004000   // 完全完成（含锁释放）

VM_FAULT_ERROR = OOM|SIGBUS|SIGSEGV|HWPOISON|HWPOISON_LARGE|FALLBACK
```

**struct vm_operations_struct** — VMA 缺页操作函数（`include/linux/mm.h`）：

```c
struct vm_operations_struct {
    void (*open)(struct vm_area_struct *area);       // VMA 打开
    void (*close)(struct vm_area_struct *area);      // VMA 关闭
    int (*may_split)(struct vm_area_struct *area, unsigned long addr);
    int (*mremap)(struct vm_area_struct *area);
    int (*mprotect)(struct vm_area_struct *vma, unsigned long start,
                    unsigned long end, unsigned long newflags);
    vm_fault_t (*fault)(struct vm_fault *vmf);       // 核心缺页处理
    vm_fault_t (*huge_fault)(struct vm_fault *vmf, unsigned int order); // 巨页缺页
    vm_fault_t (*map_pages)(struct vm_fault *vmf,    // 批量映射页面
                            pgoff_t start_pgoff, pgoff_t end_pgoff);
    unsigned long (*pagesize)(struct vm_area_struct *area);
    vm_fault_t (*page_mkwrite)(struct vm_fault *vmf); // 页面即将可写时通知
    vm_fault_t (*pfn_mkwrite)(struct vm_fault *vmf);  // PFN 映射可写通知
    int (*access)(struct vm_area_struct *vma, unsigned long addr,
                  void *buf, int len, int write);
};
```

#### 5.2.4 缺页处理流程图

```
                    ┌─────────────────────────────────┐
                    │  异常向量表 → el1_sync/el0_sync │
                    │  FAR_EL1 ← 故障地址             │
                    │  ESR ← 异常原因                 │
                    └──────────┬──────────────────────┘
                               │
                    ┌──────────▼──────────┐
                    │  el1_abort()        │
                    │  el0_da() / el0_ia()│
                    │  (arch/arm64/mm)    │
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │  do_mem_abort()     │
                    │  esr_to_fault_info()│
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │  fault_info[FSC]    │
                    └──────────┬──────────┘
                               │
         ┌─────────────────────┼─────────────────────┐
         │                     │                     │
  ┌──────▼──────┐    ┌────────▼────────┐    ┌───────▼───────┐
  │ Translation │    │Access Flag /    │    │ SEA / Tag /   │
  │ Fault (0x04)│    │Permission Fault │    │ Alignment /   │
  │  └→ do_     │    │(0x08-0x0F)      │    │ Bad (other)   │
  │  translation│    │  └→ do_page_    │    │  └→ do_sea()  │
  │  _fault()   │    │     fault()     │    │     do_tag_   │
  └──────┬──────┘    └────────┬────────┘    │     check_    │
         │                    │              │     fault()   │
    ┌────▼────┐               │              │     do_align_ │
    │TTBR0    │               │              │     fault()   │
    │地址?    │               │              │     do_bad()  │
    ├─是→继续 │               │              └───────────────┘
    │ └→do_   │               │
    │  page_  │               │
    │  fault()│               │
    └─否→do_  │               │
       bad_   │               │
       area() │               │
              │               │
              └───────┬───────┘
                      │
                      ▼
             ┌──────────────────┐
             │  do_page_fault() │
             │  (核心处理)      │
             └──────────────────┘
                      │
         ┌────────────┴────────────┐
         │                         │
  ┌──────▼──────┐         ┌───────▼────────────┐
  │  Per-VMA    │         │  mmap_lock 慢路径   │
  │  Lock 快速  │         │                    │
  │  路径       │         │ lock_mm_and_find_  │
  │ lock_vma_   │         │ vma()              │
  │ under_rcu() │         └───────┬────────────┘
  └──────┬──────┘                 │
         │                        │
  ┌──────▼──────┐         ┌───────▼────────────┐
  │ 权限检查    │         │ 权限检查 + VMA 查找 │
  │ access_error│         │ handle_mm_fault()  │
  │ handle_mm_  │         └───────┬────────────┘
  │ fault()     │                 │
  └──────┬──────┘                 │
         │                        │
         └────────────┬───────────┘
                      │
             ┌────────▼──────────┐
             │  handle_mm_fault  │
             │  (mm/memory.c)    │
             └────────┬──────────┘
                      │
             ┌────────▼──────────┐
             │  __handle_mm_     │
             │  fault()          │
             │  (页表遍历)       │
             └────────┬──────────┘
                      │
             ┌────────▼──────────┐
             │  巨页?            │
             ├─是→hugetlb_fault  │
             │ 否→继续 PTE 级    │
             └────────┬──────────┘
                      │
             ┌────────▼──────────┐
             │  handle_pte_fault │
             └────────┬──────────┘
                      │
            ┌─────────┼──────────┬───────────┬──────────┬──────────┐
            │         │          │           │          │          │
       ┌────▼───┐ ┌──▼────┐ ┌──▼──────┐ ┌───▼───┐ ┌───▼───┐ ┌───▼────┐
       │PTE 不存在│ │非Present│ │protnone│ │写权限 │ │读权限  │ │其他处理 │
       │do_pte_ │ │do_swap│ │do_numa │ │不足   │ │匹配   │ │(ufd/   │
       │missing │ │_page  │ │_page   │ │do_wp  │ │正常   │ │marker) │
       └───┬────┘ └───┬───┘ └───┬────┘ │_page  │ │访问   │ └────────┘
           │          │         │      └──┬────┘ └───┬───┘
      ┌────┴────┐     │         │         │          │
  ┌───▼───┐ ┌──▼──┐  │         │    ┌────▼────┐     │
  │匿名页  │ │文件 │  │         │    │wp_page_ │     │
  │do_anon_│ │映射 │  │         │    │copy()   │     │
  │ymous_  │ │do_  │  │         │    │┌ 分配新页│     │
  │page()  │ │fault│  │         │    │├ 拷贝数据│     │
  └───┬────┘ └──┬──┘  │         │    │├ 建立映射│     │
      │         │     │         │    │└ 释放旧页│     │
      │    ┌────┴──┐  │         │    └─────────┘     │
      │    │do_read│  │         │                     │
      │    │_fault │  │         │                     │
      │    │do_cow │  │         │                     │
      │    │_fault │  │         │                     │
      │    │do_    │  │         │                     │
      │    │shared │  │         │                     │
      │    │_fault │  │         │                     │
      │    └───────┘  │         │                     │
      │               │         │                     │
      └───────────────┴─────────┴─────────────────────┘
                      │
            ┌─────────▼──────────┐
            │  mm_account_fault  │
            │  (major/minor 统计) │
            └─────────┬──────────┘
                      │
            ┌─────────▼──────────┐
            │  返回用户态重试指令  │
            └────────────────────┘
```

#### 5.2.5 各缺页类型详细分析

**1. 匿名页面缺页（do_anonymous_page）**

当进程访问一个匿名映射（如堆、栈、BSS 段）中尚未分配的页面时触发。

```
do_anonymous_page(vmf)
  ├─ VM_SHARED 检查                     // 匿名映射不能是共享的
  ├─ pte_alloc()                        // 分配 PTE 页表（如果不存在）
  ├─ [读缺页 && 允许零页]               // 读零页优化
  │    ├─ pte_mkspecial(my_zero_pfn())  // 使用全局零页（ZERO_PAGE）
  │    ├─ pte_offset_map_lock()         // 获取 PTE 锁
  │    ├─ userfaultfd_missing() → handle_userfault()
  │    └─ setpte: 设置 PTE 并更新 MMU 缓存
  │
  └─ [写缺页]                           // 写时分配新页
       ├─ vmf_anon_prepare()            // 延迟初始化 anon_vma
       ├─ alloc_anon_folio(vmf)         // 分配匿名 folio（支持 mTHP）
       ├─ __folio_mark_uptodate()       // 标记为最新
       ├─ folio_mk_pte()                // 创建 PTE 条目
       ├─ pte_offset_map_lock()
       ├─ userfaultfd_missing() → handle_userfault()
       ├─ folio_add_new_anon_rmap()     // 添加匿名反向映射
       ├─ folio_add_lru_vma()           // 加入 LRU 链表
       └─ set_ptes() + update_mmu_cache_range()
```

**关键优化**：读缺页使用全局共享的 ZERO_PAGE（所有读零页映射到同一物理零页），避免不必要的物理页分配。

**2. 文件映射缺页（do_fault）**

当进程访问文件映射（mmap 文件）中尚未加载的页面时触发。

```
do_fault(vmf)
  ├─ [!vma->vm_ops->fault] → VM_FAULT_SIGBUS  // 缺页函数未注册
  │
  ├─ [读缺页] → do_read_fault(vmf)
  │    ├─ should_fault_around() → do_fault_around()  // 预读周围页面
  │    │    └─ vma->vm_ops->map_pages()               // 批量建立映射
  │    ├─ __do_fault() → vma->vm_ops->fault(vmf)      // 文件系统读页
  │    │    └─ 如: filemap_fault() → 从 page cache 读取
  │    └─ finish_fault(vmf)                           // 建立 PTE 映射
  │
  ├─ [写时复制] → do_cow_fault(vmf)                   // 私有映射写
  │    ├─ vmf_anon_prepare()                          // 准备 anon_vma
  │    ├─ folio_prealloc()                            // 分配匿名页
  │    ├─ __do_fault()                                // 读取原始文件页
  │    ├─ copy_mc_user_highpage()                     // 拷贝数据到匿名页
  │    └─ finish_fault()                              // 建立 PTE 映射
  │
  └─ [共享写] → do_shared_fault(vmf)                  // 共享映射写
       ├─ __do_fault()                                // 读取文件页
       ├─ do_page_mkwrite()                           // 通知文件系统写就绪
       └─ fault_dirty_shared_page()                   // 标记页面为脏
```

**3. 交换缺页（do_swap_page）**

当访问的页面已被换出到交换区（swap）时触发。

```
do_swap_page(vmf)
  ├─ softleaf_from_pte() 解析 PTE 中的 swap entry
  │
  ├─ [非 swap 条目]
  │    ├─ 迁移条目 → migration_entry_wait()     // 等待迁移完成
  │    ├─ device_private → migrate_to_ram()     // 设备私有页迁移
  │    ├─ hwpoison → VM_FAULT_HWPOISON          // 硬件损坏
  │    └─ marker → handle_pte_marker()          // PTE 标记处理
  │
  └─ [swap 条目]
       ├─ get_swap_device()                     // 防止 swapoff
       ├─ swap_cache_get_folio()                // 检查 swap cache 中是否已有
       │    ├─ [cache 命中] → 直接使用
       │    └─ [cache 未命中] → 从交换区读入
       │         ├─ SWP_SYNCHRONOUS_IO(zram) → alloc_swap_folio + swapin_folio
       │         └─ 普通 swap → swapin_readahead()
       │
       ├─ folio_lock_or_retry()                 // 锁定 folio
       ├─ cgroup 和 memcg 处理
       ├─ folio_add_lru_vma()                   // 加入 LRU
       ├─ swap_read_folio()                     // 从交换设备读取数据
       ├─ swap_free()                           // 释放 swap 槽位
       └─ finish_fault()                        // 建立 PTE 映射
```

**4. 写时复制缺页（do_wp_page）**

当写入一个只读的共享页面（如 fork 后的父子进程共享页）时触发。

```
do_wp_page(vmf)
  ├─ [userfaultfd wp] → handle_userfault()     // 用户态缺页处理
  │
  ├─ [共享映射] → wp_page_shared()
  │    ├─ wp_pfn_shared()                       // PFN 映射处理
  │    └─ do_page_mkwrite()                     // 通知文件系统页可写
  │
  └─ [私有映射]
       ├─ [PageAnonExclusive] → wp_page_reuse() // 独占匿名页直接重用
       │    └─ pte_mkdirty() + pte_mkwrite()
       │
       └─ wp_page_copy()                        // 复制页面
            ├─ folio_prealloc()                 // 分配新匿名页
            ├─ copy_user_highpage()             // 拷贝原页数据
            ├─ __folio_mark_uptodate()
            ├─ page_add_new_anon_rmap()         // 新页反向映射
            ├─ set_pte_at()                     // 设置新 PTE
            ├─ page_remove_rmap()               // 原页反向映射删除
            ├─ tlb_flush()                      // TLB 刷新
            └─ folio_put()                      // 释放原页引用
```

**5. NUMA 均衡缺页（do_numa_page）**

当 NUMA 平衡扫描将页表标记为 protnone 后，访问该页面时触发。

```
do_numa_page(vmf)
  ├─ pte_offset_map_lock()                     // 获取 PTE 锁
  ├─ pte_same() 检查是否被修改
  ├─ vm_normal_page() 获取页面
  ├─ 记录 last_cpupid (上次访问的 CPU/PID)
  ├─ pte_mkold() 清除访问位
  ├─ pte_unmap_unlock()
  └─ numa_migrate_check()                      // 判断是否需要迁移
       └─ [需要迁移] → migrate_misplaced_folio() // 迁移到正确 NUMA 节点
```

#### 5.2.6 Per-VMA Lock 快速路径

在 `do_page_fault()` 中，首先尝试 Per-VMA Lock 快速路径：

```
do_page_fault(far, esr, regs)                   // arch/arm64/mm/fault.c
  ├─ [用户态缺页] → lock_vma_under_rcu(mm, address)  // 尝试获取 VMA 读锁
  │    ├─ [成功] → 权限检查 (vma->vm_flags & vm_flags)
  │    │    ├─ [通过] → handle_mm_fault(FAULT_FLAG_VMA_LOCK)
  │    │    │    ├─ [VM_FAULT_RETRY] → 降级到 mmap_lock 重试
  │    │    │    ├─ [VM_FAULT_COMPLETED] → 直接返回
  │    │    │    └─ [成功] → vma_end_read() + done
  │    │    └─ [失败] → bad_area (SEGV_ACCERR)
  │    └─ [失败] → lock_mmap: 获取 mmap_lock 慢路径
  │
  └─ lock_mmap 慢路径:
       ├─ lock_mm_and_find_vma(mm, addr, regs)  // 获取 mmap_lock + 查找 VMA
       ├─ handle_mm_fault(vma, addr, mm_flags, regs)
       ├─ [VM_FAULT_RETRY] → FAULT_FLAG_TRIED + 重试
       ├─ [VM_FAULT_COMPLETED] → 直接返回
       └─ mmap_read_unlock(mm)
```

#### 5.2.7 缺页统计

```
mm_account_fault(mm, regs, address, flags, ret)
  ├─ [VM_FAULT_RETRY] → 跳过（不完整缺页）
  ├─ count_vm_event(PGFAULT)                  // 总缺页计数
  ├─ [VM_FAULT_ERROR] → 跳过（不计数）
  ├─ major = (ret & VM_FAULT_MAJOR) || TRIED  // 主缺页判定
  ├─ current->maj_flt++ 或 current->min_flt++  // 进程级统计
  └─ perf_sw_event()                          // perf 事件
```

- **主缺页（Major Fault）**：需要从磁盘读取数据（文件映射首次读或 swap 读入），有 I/O 等待
- **次缺页（Minor Fault）**：不需要磁盘 I/O（匿名页分配、ZERO_PAGE、swap cache 命中、COW 复制）

#### 5.2.8 关键设计要点

1. **ZERO_PAGE 优化**：匿名页读缺页时，映射到全局唯一零页，写时触发 COW 才分配真实页
2. **Per-VMA Lock**：6.1+ 引入读者锁替代 mmap_lock 读锁，大幅减少缺页路径锁竞争
3. **fault_around**：文件映射读缺页时，预读 fault_around 阶（默认 256KB）页面，批量建立映射
4. **Swap Cache 去重**：多个进程共享的匿名页换出后，swap cache 中只有一份，缺页时共享
5. **mTHP 支持**：匿名缺页尝试分配 2M/64K/16K/4K 等多尺寸透明巨页，失败时回退到更小尺寸
6. **VM_FAULT_RETRY 机制**：文件系统缺页可能触发 I/O，通过 RETRY 释放 mmap_lock 允许并发
7. **userfaultfd**：用户态缺页处理机制，用于虚拟机迁移、垃圾回收等场景
8. **ARM64 MTE 支持**：ARMv8.5+ 内存标签扩展，缺页时通过 `do_tag_check_fault` 处理同步标签检查异常（SIGSEGV/SEGV_MTESERR），匿名页分配时通过 `vma_alloc_zeroed_movable_folio` 使用 `__GFP_ZEROTAGS` 初始化标签
9. **ARM64 GCS 支持**：Guarded Control Stack（硬件安全栈），GCS 权限缺页作为写缺页处理（`is_gcs_fault` → FAULT_FLAG_WRITE + VM_WRITE），GCS 违规通过 `is_invalid_gcs_access` 检查
10. **ARM64 ESR 集中分派**：不同于 x86 的 `exc_page_fault` 按地址空间手动分派，ARM64 通过 `fault_info` 表 + FSC 索引实现集中式分派，减少条件分支，便于扩展新的故障类型
11. **ARM64 lock_mm_and_find_vma**：ARM64 使用 `lock_mm_and_find_vma()` 一次性获取 `mmap_lock` 并查找 VMA，相比 x86 的 `mmap_read_lock` + `find_vma` 两步操作，减少了一次 VMA 查找的锁竞争窗口

#### 5.2.9 __ex_table 内核异常修复表

文件：`arch/arm64/mm/extable.c`（116 行），`arch/arm64/include/asm/extable.h`，`lib/extable.c`，`kernel/extable.c`（170 行）

**机制概述**

`__ex_table`（Exception Table）是 Linux 内核用于处理**内核态缺页**的异常修复机制。当内核代码（如 `copy_from_user`）访问用户空间地址时，如果该地址无效，MMU 会触发缺页异常。内核通过异常修复表找到对应的修复代码，阻止内核崩溃，并返回错误码给调用者。

与用户态缺页不同，内核态缺页**不能**通过分配物理页面来解决——因为用户空间地址无效表示用户态未映射该页，内核不应擅自创建映射。正确的做法是跳过故障指令并返回错误。

**核心数据结构**

```c
// arch/arm64/include/asm/extable.h
struct exception_table_entry {
    int insn;         // 故障指令地址（相对于表项的偏移量）
    int fixup;        // 修复代码地址（相对于表项的偏移量）
    short type;       // 异常类型（EX_TYPE_xxx）
    short data;       // 额外数据（编码寄存器编号等信息）
};

// 使用 ARCH_HAS_RELATIVE_EXTABLE，地址存储为相对偏移量
// 实际地址计算方式：
//   insn_addr = (unsigned long)&entry->insn + entry->insn
//   fixup_addr = (unsigned long)&entry->fixup + entry->fixup
```

**ARM64 异常类型**（`arch/arm64/include/asm/asm-extable.h`）：

| 类型 | 值 | 描述 | 处理函数 |
|------|-----|------|----------|
| `EX_TYPE_NONE` | 0 | 无效类型 | — |
| `EX_TYPE_BPF` | 1 | BPF 程序异常 | `ex_handler_bpf()` |
| `EX_TYPE_UACCESS_ERR_ZERO` | 2 | 用户态访问：设置错误码 + 清零目标寄存器 | `ex_handler_uaccess_err_zero()` |
| `EX_TYPE_KACCESS_ERR_ZERO` | 3 | 内核态访问：设置错误码 + 清零目标寄存器 | `ex_handler_uaccess_err_zero()` |
| `EX_TYPE_UACCESS_CPY` | 4 | 批量拷贝（如 `copy_from_user`） | `ex_handler_uaccess_cpy()` |
| `EX_TYPE_LOAD_UNALIGNED_ZEROPAD` | 5 | 非对齐加载零填充 | `ex_handler_load_unaligned_zeropad()` |

**异常表的数据字段编码**：

```
EX_TYPE_UACCESS_ERR_ZERO / EX_TYPE_KACCESS_ERR_ZERO:
  data[4:0]   = ERR: 存放 -EFAULT 错误码的寄存器编号
  data[9:5]   = ZERO: 清零的目标寄存器编号

EX_TYPE_UACCESS_CPY:
  data[0]     = UACCESS_WRITE: 1=写操作, 0=读操作

EX_TYPE_LOAD_UNALIGNED_ZEROPAD:
  data[4:0]   = DATA: 存放加载数据的寄存器编号
  data[9:5]   = ADDR: 存放源地址的寄存器编号
```

**完整调用栈**

```
内核态缺页异常修复流程
────────────────────────────────────────────────────────────────
do_mem_abort(far, esr, regs)                 // 异常中央分派点
  ├─ esr_to_fault_info(esr) → fault_info[FSC]
  │    └─ [Translation/Permission Fault]
  │         └─ do_page_fault()
  │              └─ [内核态 && 无 VMA] → no_context → __do_kernel_fault()
  │
  └─ __do_kernel_fault(addr, esr, regs)      // 内核缺页处理
       ├─ [!is_el1_instruction_abort]        // 指令故障不可修复
       │    └─ fixup_exception(regs, esr)    // 检查异常修复表
       │         ├─ search_exception_tables(regs->pc)  // 查找当前指令
       │         │    ├─ search_kernel_exception_table()  // 内核 __ex_table
       │         │    │    └─ search_extable()            // 二分查找
       │         │    ├─ search_module_extables()         // 模块 __ex_table
       │         │    └─ search_bpf_extables()            // BPF __ex_table
       │         │
       │         └─ [找到] → 根据 ex->type 分派处理函数
       │              ├─ EX_TYPE_UACCESS_ERR_ZERO
       │              │    └─ ex_handler_uaccess_err_zero()
       │              │         ├─ pt_regs_write_reg(reg, ERR, -EFAULT)
       │              │         ├─ pt_regs_write_reg(reg, ZERO, 0)
       │              │         └─ regs->pc = get_ex_fixup(ex)
       │              │
       │              ├─ EX_TYPE_KACCESS_ERR_ZERO
       │              │    └─ ex_handler_uaccess_err_zero()  // 同上
       │              │
       │              ├─ EX_TYPE_UACCESS_CPY
       │              │    └─ ex_handler_uaccess_cpy(ex, regs, esr)
       │              │         ├─ cpy_faulted_on_uaccess()  // 检查是否 uaccess 故障
       │              │         │    └─ (ex->data & WRITE) == (esr & WNR)
       │              │         ├─ [是 uaccess 故障] → regs->pc = fixup
       │              │         └─ [内核内存故障] → return false（不修复）
       │              │
       │              ├─ EX_TYPE_LOAD_UNALIGNED_ZEROPAD
       │              │    └─ ex_handler_load_unaligned_zeropad()
       │              │         ├─ 从 reg_addr 读取故障地址
       │              │         ├─ 对齐到 8 字节边界读取数据
       │              │         ├─ 移位截取有效字节，零填充高位
       │              │         ├─ 写入 reg_data 寄存器
       │              │         └─ regs->pc = fixup
       │              │
       │              └─ EX_TYPE_BPF
       │                   └─ ex_handler_bpf()
       │                        └─ regs->pc = fixup + 跳过 BPF 指令
       │
       ├─ [fixup 未找到] → 检查假缺页、MTE 标签故障
       ├─ [fixup 未找到] → die_kernel_fault()  // 内核 Oops
       └─ [用户态] → 信号处理（SIGSEGV/SIGBUS）
```

**异常表查找流程**

```
search_exception_tables(addr)                 // kernel/extable.c
  ├─ search_kernel_exception_table(addr)      // 内核内置异常表
  │    └─ search_extable(__start___ex_table,  // 二分查找
  │                      __stop___ex_table,
  │                      addr)
  ├─ search_module_extables(addr)             // 可加载模块异常表
  │    └─ 遍历模块列表 → 每个模块的 extable
  └─ search_bpf_extables(addr)               // BPF JIT 异常表
       └─ 遍历 bpf_prog 列表

search_extable(base, num, value)              // lib/extable.c
  └─ bsearch(&value, base, num,               // 标准二分查找
              sizeof(struct exception_table_entry),
              cmp_ex_search)
```

**异常表排序与初始化**：

```
start_kernel()
  └─ sort_main_extable()                     // kernel/extable.c
       ├─ main_extable_sort_needed 检查       // 构建时可能已排序
       ├─ sort_extable(__start___ex_table,   // 对 __ex_table 排序
       │               __stop___ex_table)
       │    └─ sort() + cmp_ex_sort + swap_ex
       └─ main_extable_sort_needed = 0

// 构建时排序（CONFIG_BUILDTIME_TABLE_SORT）：
// scripts/sorttable.c 在链接后对 __ex_table 排序
// 设置 main_extable_sort_needed = 0，跳过运行时排序
```

**ARM64 汇编宏生成异常表条目原理机制**

`__ex_table` 条目在内核中有两种生成方式：**汇编文件**（`.S`）中的宏展开和 **C 内联汇编**（`asm goto()` / `asm volatile()`）中的宏展开。两者最终都通过 `__ASM_EXTABLE_RAW` 基元在 `__ex_table` section 中生成 4 字（16 字节）的异常表条目。

---

### 宏层次架构

```
层次 1:  __ASM_EXTABLE_RAW(insn, fixup, type, data)          ← 基元，直接生成 .section 指令
    │
    ├── _ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, err, zero)  ← type=EX_TYPE_UACCESS_ERR_ZERO
    ├── _ASM_EXTABLE_KACCESS_ERR_ZERO(insn, fixup, err, zero)  ← type=EX_TYPE_KACCESS_ERR_ZERO  
    ├── _ASM_EXTABLE_UACCESS_CPY(insn, fixup, write)           ← type=EX_TYPE_UACCESS_CPY
    └── _ASM_EXTABLE_LOAD_UNALIGNED_ZEROPAD(insn, fixup, data, addr)  ← type=EX_TYPE_LOAD_UNALIGNED_ZEROPAD

层次 2（简化封装）:
    _ASM_EXTABLE_UACCESS(insn, fixup)       = _ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, wzr, wzr)
    _ASM_EXTABLE_UACCESS_ERR(insn, fixup, err) = _ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, err, wzr)
    _ASM_EXTABLE_KACCESS(insn, fixup)       = _ASM_EXTABLE_KACCESS_ERR_ZERO(insn, fixup, wzr, wzr)
    _ASM_EXTABLE_KACCESS_ERR(insn, fixup, err) = _ASM_EXTABLE_KACCESS_ERR_ZERO(insn, fixup, err, wzr)

层次 3（汇编 .macro 封装）:
    _asm_extable_uaccess(insn, fixup)       → _ASM_EXTABLE_UACCESS(insn, fixup)
    _cond_uaccess_extable(insn, fixup)      → 条件化：仅当 fixup 非空时生成
    _asm_extable_uaccess_cpy(insn, fixup, uaccess_is_write) → 直接 __ASM_EXTABLE_RAW

层次 4（高级 uaccess 宏，在 asm-uaccess.h 中定义）:
    USER(l, x...)              → 单指令 uaccess + _asm_extable_uaccess
    USER_CPY(l, write, x...)   → 单指令 uaccess + _asm_extable_uaccess_cpy
    user_ldst(l, inst, reg, addr, post_inc)   → 单寄存器 ldtr/sttr
    user_ldp(l, reg1, reg2, addr, post_inc)   → 双寄存器 ldtr
    user_stp(l, reg1, reg2, addr, post_inc)   → 双寄存器 sttr

层次 5（C 内联汇编宏，在 uaccess.h 中定义）:
    __get_mem_asm(load, reg, x, addr, label, type)   → 生成 _ASM_EXTABLE_##type##ACCESS
    __put_mem_asm(store, reg, x, addr, label, type)  → 生成 _ASM_EXTABLE_##type##ACCESS
```

---

### 基元宏：`__ASM_EXTABLE_RAW` 的精确展开

```asm
// 定义在 arch/arm64/include/asm/asm-extable.h
// 这是整个异常表机制的"原子操作"，所有其他宏最终都展开为此宏。

#define __ASM_EXTABLE_RAW(insn, fixup, type, data)	\
	.pushsection	__ex_table, "a";		\  // 切换到 __ex_table section
	.align		2;				\  // 4 字节对齐（条目大小 16 字节）
	.long		((insn) - .);			\  // insn 偏移：当前指令地址 - 当前位置
	.long		((fixup) - .);			\  // fixup 偏移：修复地址 - 当前位置
	.short		(type);				\  // 异常类型（2 字节）
	.short		(data);				\  // 额外数据（2 字节）
	.popsection;					\  // 恢复之前的 section

// 生成的二进制布局（16 字节）：
// 偏移  大小  内容          说明
// ─────────────────────────────────────────────
// +0    4     insn 偏移    故障指令地址 - 条目地址
// +4    4     fixup 偏移   修复代码地址 - 条目地址
// +8    2     type         异常类型（EX_TYPE_xxx）
// +10   2     data         类型相关的额外数据
// ─────────────────────────────────────────────
// 总计 16 字节
```

**关键设计：相对偏移**

`((insn) - .)` 中的 `.` 是当前汇编位置计数器（即 `entry->insn` 字段的地址）。因此：
- `insn` 的实际地址 = `entry->insn` 地址 + `entry->insn` 值
- 这种相对偏移编码使 `__ex_table` 段在 KASLR 和模块加载时**无需重定位**
- 32 位偏移量（`long`）可覆盖 ±2GB 范围，足以覆盖整个内核地址空间

---

### 寄存器编号编码机制（`gpr-num.h`）

```asm
// arch/arm64/include/asm/gpr-num.h
// 为每个通用寄存器定义编号符号

	.irp	num,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	.equ	.L__gpr_num_x\num, \num     // x0 ~ x30 的编号
	.equ	.L__gpr_num_w\num, \num     // w0 ~ w30 的编号（与 x 相同）
	.endr
	.equ	.L__gpr_num_xzr, 31         // 零寄存器 xzr 编号 31
	.equ	.L__gpr_num_wzr, 31         // 零寄存器 wzr 编号 31

// 使用方式：
// EX_DATA_REG(ERR, x0)  →  .L__gpr_num_x0 << EX_DATA_REG_ERR_SHIFT  =  0 << 0 = 0
// EX_DATA_REG(ZERO, w1) →  .L__gpr_num_w1 << EX_DATA_REG_ZERO_SHIFT =  1 << 5 = 32
// 组合：data = 0 | 32 = 32
// 含义：err 寄存器 = x0, zero 寄存器 = w1
```

`data` 字段的位布局：

```
EX_TYPE_UACCESS_ERR_ZERO / EX_TYPE_KACCESS_ERR_ZERO:
  data[4:0]   = ERR 寄存器编号（0~31）
  data[9:5]   = ZERO 寄存器编号（0~31）
  data[15:10] = 未使用

EX_TYPE_LOAD_UNALIGNED_ZEROPAD:
  data[4:0]   = DATA 寄存器编号（存放加载结果）
  data[9:5]   = ADDR 寄存器编号（存放源地址）
  data[15:10] = 未使用

EX_TYPE_UACCESS_CPY:
  data[0]     = UACCESS_WRITE: 1=写操作(sttr), 0=读操作(ldtr)
  data[15:1]  = 未使用
```

---

### 汇编高级宏（`asm-uaccess.h`）详解

#### `USER` 宏 — 最常用的单指令 uaccess 封装

```asm
// arch/arm64/include/asm/asm-uaccess.h
#define USER(l, x...)				\
9999:	x;					\     // 生成故障指令
	_asm_extable_uaccess	9999b, l	      // 生成 __ex_table 条目

// 使用示例（clear_user.S）：
USER(9f, sttr	xzr, [x0])
// 展开为：
// 9999:  sttr    xzr, [x0]          // 可能故障的指令
//        _asm_extable_uaccess 9999b, 9f
//          → _ASM_EXTABLE_UACCESS(9999b, 9f)
//            → __ASM_EXTABLE_RAW(9999b, 9f, EX_TYPE_UACCESS_ERR_ZERO, (0 | 0))
//              → .pushsection __ex_table, "a"
//                  .align 2
//                  .long (9999b - .)      // insn: 指向 sttr 指令
//                  .long (9f - .)         // fixup: 指向修复代码
//                  .short 2               // type: EX_TYPE_UACCESS_ERR_ZERO
//                  .short 0               // data: err=wzr, zero=wzr
//                .popsection

// 注意事项：
// 1. 标签 9999 由汇编器自动生成唯一编号
// 2. l 参数是修复代码标签（如 9f 表示向后跳转到标签 9）
// 3. 故障时：ex_handler_uaccess_err_zero 设置 regs->pc = 9f
```

#### `USER_CPY` 宏 — 带写/读过滤的 uaccess 封装

```asm
#define USER_CPY(l, uaccess_is_write, x...)	\
9999:	x;					\     // 生成故障指令
	_asm_extable_uaccess_cpy 9999b, l, uaccess_is_write  // 生成 EX_TYPE_UACCESS_CPY 条目

// 使用示例（copy_from_user.S 中的 MOPS 指令）：
USER_CPY(9997f, 0, cpyfprt [dst]!, [src]!, count!)
// 展开为：
// 9999:  cpyfprt [dst]!, [src]!, count!
//        __ASM_EXTABLE_RAW(9999b, 9997f, EX_TYPE_UACCESS_CPY, 0)
// 含义：type=EX_TYPE_UACCESS_CPY, data=0（读操作）

// 对比 copy_to_user.S：
USER_CPY(9997f, 1, cpyfpwt [dst]!, [src]!, count!)
// type=EX_TYPE_UACCESS_CPY, data=1（写操作）

// 运行时过滤逻辑（arch/arm64/mm/extable.c）：
// cpy_faulted_on_uaccess(ex, esr):
//   uaccess_is_write = (ex->data & 1)   // 条目中记录的写/读标志
//   fault_on_write   = (esr & ESR_ELx_WNR)  // 实际故障的写/读属性
//   return uaccess_is_write == fault_on_write
// 只有当两者匹配时才修复，否则返回 false 让内核 Oops
```

#### `user_ldst` / `user_ldp` / `user_stp` 宏 — 批量 uaccess 的精细条目

```asm
// 单寄存器加载/存储（带后递增）
.macro user_ldst l, inst, reg, addr, post_inc
8888:		\inst		\reg, [\addr];	  // 故障指令
		add		\addr, \addr, \post_inc;
		_asm_extable_uaccess	8888b, \l;  // 每个指令一个条目
.endm

// 双寄存器加载（ldtr 没有后递增变体，需手动展开）
.macro user_ldp l, reg1, reg2, addr, post_inc
8888:		ldtr	\reg1, [\addr];		 // 第一次加载可能故障
8889:		ldtr	\reg2, [\addr, #8];	 // 第二次加载可能故障
		add	\addr, \addr, \post_inc;

		_asm_extable_uaccess	8888b, \l; // 条目 1
		_asm_extable_uaccess	8889b, \l; // 条目 2
.endm

// 双寄存器存储
.macro user_stp l, reg1, reg2, addr, post_inc
8888:		sttr	\reg1, [\addr];		 // 第一次存储可能故障
8889:		sttr	\reg2, [\addr, #8];	 // 第二次存储可能故障
		add	\addr, \addr, \post_inc;

		_asm_extable_uaccess	8888b,\l; // 条目 1
		_asm_extable_uaccess	8889b,\l; // 条目 2
.endm

// 关键设计点：
// 1. 使用 8888/8889 固定标签而非 9999 自动编号，确保两个指令的条目
//    指向同一个 fixup 标签
// 2. 每条故障指令独立生成一个条目，即使它们共享同一个修复代码
// 3. 如果第一次加载成功而第二次失败，修复代码会将 dst 回退到
//    正确位置（因为 add 已执行，但 dst 值仍可用）
// 4. 由于 ldtr/sttr 没有后递增变体，需手动添加 add 指令
```

#### `_cond_uaccess_extable` 宏 — 条件化异常表生成

```asm
.macro _cond_uaccess_extable, insn, fixup
	.ifnc			\fixup,               // 如果 fixup 参数非空
	_asm_extable_uaccess	\insn, \fixup         // 生成异常表条目
	.endif
.endm

// 使用场景：某些指令可能不需要异常处理，调用者通过传递空 fixup
// 来跳过条目生成。这避免了在调用处写 if/else 分支逻辑。
```

---

### C 内联汇编宏（`uaccess.h`）详解

#### `__get_mem_asm` — 带 goto 输出的内联版本

```c
// 新版本（CONFIG_CC_HAS_ASM_GOTO_OUTPUT）：
#define __get_mem_asm(load, reg, x, addr, label, type)		\
	asm_goto_output(					\
	"1:	" load "	" reg "0, [%1]\n"		\
	_ASM_EXTABLE_##type##ACCESS(1b, %l2)			\  // 拼接为 _ASM_EXTABLE_UACCESS
	: "=r" (x)						\
	: "r" (addr) : : label)

// 展开示例（type=U, load=ldtr, reg=%w, size=4）：
// asm_goto_output(
//     "1:  ldtr    %w0, [%1]\n"
//     _ASM_EXTABLE_UACCESS(1b, %l2)     // → __ASM_EXTABLE_RAW(1b, %l2, 2, 0)
//     : "=r" (__gu_val)
//     : "r" (ptr)
//     : : __gu_failed)

// 老版本（无 asm_goto_output，使用输出寄存器保存错误码）：
#define __get_mem_asm(load, reg, x, addr, label, type) do {	\
	int __gma_err = 0;					\
	asm volatile(						\
	"1:	" load "	" reg "1, [%2]\n"		\
	"2:\n"							\
	_ASM_EXTABLE_##type##ACCESS_ERR_ZERO(1b, 2b, %w0, %w1)	\  // 拼接为 _ASM_EXTABLE_UACCESS_ERR_ZERO
	: "+r" (__gma_err), "=r" (x)				\
	: "r" (addr));						\
	if (__gma_err) goto label; } while (0)

// 展开示例：
// asm volatile(
//     "1:  ldtr    %w1, [%2]\n"
//     "2:\n"
//     _ASM_EXTABLE_UACCESS_ERR_ZERO(1b, 2b, %w0, %w1)
//       // → __ASM_EXTABLE_RAW(1b, 2b, EX_TYPE_UACCESS_ERR_ZERO,
//       //      (EX_DATA_REG(ERR, %w0) | EX_DATA_REG(ZERO, %w1)))
//       //   data = (__gma_err 寄存器编号) | (x 寄存器编号 << 5)
//     : "+r" (__gma_err), "=r" (x)
//     : "r" (addr));
// 故障时：ex_handler_uaccess_err_zero 将 __gma_err 设为 -EFAULT，
//         将 x 清零，然后跳转到 2b（即修复后继续执行）
// 调用者检查 __gma_err != 0 来跳转到错误处理标签
```

#### `__put_mem_asm` — 存储操作的内联版本

```c
#define __put_mem_asm(store, reg, x, addr, label, type)		\
	asm goto(						\
	"1:	" store "	" reg "0, [%1]\n"		\
	"2:\n"							\
	_ASM_EXTABLE_##type##ACCESS(1b, %l2)			\  // 拼接为 _ASM_EXTABLE_UACCESS
	: : "rZ" (x), "r" (addr) : : label)

// 展开示例（type=U, store=sttr, reg=%w, size=4）：
// asm goto(
//     "1:  sttr    %w0, [%1]\n"
//     _ASM_EXTABLE_UACCESS(1b, %l2)     // data=0（err=wzr, zero=wzr）
//     : : "rZ" (__pu_val), "r" (ptr)
//     : : __pu_failed)
// 注意：存储操作不需要设置错误码和清零（存储的值已写入内存），
//       所以直接使用 _ASM_EXTABLE_UACCESS 简化版本
```

#### 完整的 `get_user` 宏展开流程

```
get_user(val, ptr)
  └─ __get_user(val, ptr)
       └─ __get_user_error(val, ptr, err)
            └─ __raw_get_user(x, ptr, label)
                 ├─ uaccess_ttbr0_enable()           // PAN 使能用户空间访问
                 ├─ __raw_get_mem("ldtr", x, ptr, label, U)
                 │    └─ switch(sizeof(*ptr))
                 │         ├─ case 1: __get_mem_asm("ldtrb", "%w", x, ptr, label, U)
                 │         ├─ case 2: __get_mem_asm("ldtrh", "%w", x, ptr, label, U)
                 │         ├─ case 4: __get_mem_asm("ldtr",  "%w", x, ptr, label, U)
                 │         └─ case 8: __get_mem_asm("ldtr",  "%x", x, ptr, label, U)
                 │              └─ asm_goto_output(
                 │                   "1:  ldtr    %0, [%1]\n"
                 │                   _ASM_EXTABLE_UACCESS(1b, %l2))
                 │                   // 生成条目：insn=1b, fixup=label, type=2, data=0
                 └─ uaccess_ttbr0_disable()          // PAN 禁用用户空间访问
```

---

### 完整汇编文件示例

#### `copy_from_user.S` 三级修复策略

```asm
// arch/arm64/lib/copy_from_user.S
// 拷贝来自用户空间的数据，返回值 = 未拷贝的字节数

SYM_FUNC_START(__arch_copy_from_user)
	add	end, x0, x2		// end = dst + n
	mov	srcin, x1
#include "copy_template.S"		// 批量拷贝核心
	mov	x0, #0			// 全部拷贝成功
	ret

// 异常修复代码（三级策略）：
// 第一级：批量拷贝中故障 (9996b)
9996:	b.cs	9997f			// 超过计数 → 跳转
	add	dst, dst, count		// 回退到已拷贝位置

// 第二级：零字节拷贝故障 (9997b)
9997:	cmp	dst, dstin		// 是否一个字节都没拷贝？
	b.ne	9998f			// 有拷贝 → 直接返回已拷贝数
	// 尝试至少拷贝一个字节
USER(9998f, ldtrb tmp1w, [srcin])	// 生成 __ex_table 条目
	strb	tmp1w, [dst], #1

// 第三级：单字节拷贝故障 (9998b)
9998:	sub	x0, end, dst		// 返回未拷贝字节数
	ret
SYM_FUNC_END(__arch_copy_from_user)

// 生成的 __ex_table 条目分布：
// 位置         故障指令        修复目标   类型
// ─────────────────────────────────────────────────────
// copy_template 中  ldtr 指令群   9997f     EX_TYPE_UACCESS_ERR_ZERO
// 9997 处的      ldtrb          9998f     EX_TYPE_UACCESS_ERR_ZERO
// 9998 处的      ldtrb          9998f     EX_TYPE_UACCESS_ERR_ZERO
//
// 注意：copy_template.S 中的每个 ldrb1/ldrh1/ldr1/ldp1 宏调用
// 都会生成一个独立的 __ex_table 条目，因此条目数量 = 拷贝循环中
// 所有加载指令的总数
```

#### `clear_user.S` 精细修复

```asm
// arch/arm64/lib/clear_user.S
// 清除用户空间内存，返回值 = 未清除的字节数

SYM_FUNC_START(__arch_clear_user)
	add	x2, x0, x1		// end = addr + sz

	// 批量清零（8 字节步进）
1:	.p2align 4
USER(9f, sttr	xzr, [x0])		// 可能故障，修复到 9f
	add	x0, x0, #8
	subs	x1, x1, #8
	b.hi	1b
	// 写入最后一个 8 字节（即使已对齐也写入，确保覆盖所有字节）
USER(9f, sttr	xzr, [x2, #-8])
	mov	x0, #0
	ret

	// 尾部处理（1/2/4 字节）
2:	tbz	x1, #2, 3f
USER(9f, sttr	wzr, [x0])
USER(8f, sttr	wzr, [x2, #-4])
	mov	x0, #0
	ret
3:	tbz	x1, #1, 4f
USER(9f, sttrh	wzr, [x0])		// 2 字节写入
4:	tbz	x1, #0, 5f
USER(7f, sttrb	wzr, [x2, #-1])	// 1 字节写入
5:	mov	x0, #0
	ret

	// 修复代码（多级标签）：
6:	b.cs	9f			// MOPS 指令修复
	add	x0, x0, x1
	b	9f
7:	sub	x0, x2, #5		// 单字节故障：end - 5
8:	add	x0, x0, #4		// 4 字节故障：当前 + 4
9:	sub	x0, x2, x0		// 返回未清除字节数
	ret
SYM_FUNC_END(__arch_clear_user)

// 修复代码设计要点：
// - 标签 7: 单字节故障发生在地址 x2-1，x0 当前 = x2-1，剩余 = (x2 - (x2-1)) = 1
//   但实际当前地址是 x2-5（因为从 x2-4 调整），所以 sub x0, x2, #5 将 x0 设为 x2-5
//   然后 sub x0, x2, x0 = 5 字节未清除
// - 标签 8: 4 字节写入故障，x0 未递增，直接 add x0, x0, #4 跳过故障写入
//   然后 sub x0, x2, x0 返回剩余字节数
```

---

### `load_unaligned_zeropad` 特殊处理

```c
// arch/arm64/include/asm/word-at-a-time.h
// 用于字符串处理（strncpy_from_user/strnlen_user）中的非对齐加载

static inline unsigned long load_unaligned_zeropad(const void *addr)
{
    unsigned long ret;

    __mte_enable_tco_async();
    asm(
    "1:	ldr	%0, %2\n"			// 非对齐 8 字节加载
    "2:\n"
    _ASM_EXTABLE_LOAD_UNALIGNED_ZEROPAD(1b, 2b, %0, %1)  // 特殊类型
    : "=&r" (ret)
    : "r" (addr), "Q" (*(unsigned long *)addr));

    __mte_disable_tco_async();
    return ret;
}

// 故障时处理（arch/arm64/mm/extable.c）：
ex_handler_load_unaligned_zeropad(ex, regs):
  └─ addr = pt_regs_read_reg(regs, reg_addr)  // 读取故障地址
  └─ offset = addr & 0x7                       // 未对齐偏移
  └─ addr &= ~0x7                              // 对齐到 8 字节边界
  └─ data = *(unsigned long*)addr              // 安全加载 8 字节
  └─ data >>= 8 * offset                       // 右移有效字节
  └─ pt_regs_write_reg(regs, reg_data, data)   // 返回部分有效数据
  └─ regs->pc = get_ex_fixup(ex)               // 继续执行

// 设计意图：字符串处理函数在扫描用户空间字符串时，可能跨越页边界。
// 如果下一页未映射，普通加载会触发缺页。这个修复机制允许：
// 1. 加载跨页的 8 字节
// 2. 如果故障发生在页边界，从已映射的页中提取有效字节
// 3. 未映射部分的字节被零填充（zeropad）
// 这避免了字符串处理函数需要逐字节扫描的性能开销
```

---

### `__ex_table` 段在内核镜像中的布局

```c
// 链接脚本：include/asm-generic/vmlinux.lds.h
#define EXCEPTION_TABLE(align)					\
	. = ALIGN(align);					\
	__ex_table : AT(ADDR(__ex_table) - LOAD_OFFSET) {	\
		BOUNDED_SECTION_BY(__ex_table, ___ex_table)	\
	}

// 展开后生成：
// . = ALIGN(4);                          // 4 字节对齐
// __ex_table : {                         // 段开始
//     __start___ex_table = .;            // 起始符号
//     KEEP(*(__ex_table))                // 所有 __ex_table 条目
//     __stop___ex_table = .;             // 结束符号
// }

// 内核代码中引用：
// extern struct exception_table_entry __start___ex_table[];
// extern struct exception_table_entry __stop___ex_table[];
// search_kernel_exception_table() 使用这两个符号定位异常表
```

---

### 完整示例：`get_user` 从宏展开到最终处理的完整链路

```
get_user(val, ptr)                        // C 宏调用
  │
  ▼
__get_mem_asm("ldtr", "%w", x, ptr, __gu_failed, U)
  │
  ├─ asm_goto_output("1: ldtr %w0, [%1]\n"          // 生成 ARM64 指令
  │                  _ASM_EXTABLE_UACCESS(1b, %l2))  // 生成 __ex_table 条目
  │
  ▼
链接后的二进制布局：
  .text 段:
    1:  ldtr  w0, [x1]        ← 可能故障的指令 (PC=0x1000)
    2:  ...                    ← 修复后继续执行的代码

  __ex_table 段:
    entry@0x2000:             ← 16 字节
      insn  = 0x1000 - 0x2000 = -0x1000 (0xFFFFF000)
      fixup = 0x1004 - 0x2000 = -0x0FFC (0xFFFFF004)
      type  = 2               ← EX_TYPE_UACCESS_ERR_ZERO
      data  = 0               ← err=wzr, zero=wzr

  │
  ▼
运行时缺页触发：
  do_mem_abort(esr=0x96000021, regs->pc=0x1000)  // 故障在 0x1000
  │
  ├─ __do_kernel_fault(addr, esr, regs)
  │    └─ fixup_exception(regs, esr)
  │         └─ search_exception_tables(0x1000)
  │              └─ search_kernel_exception_table(0x1000)
  │                   └─ bsearch() → 找到 entry@0x2000
  │
  ├─ entry->insn = 0x1000, entry->fixup = 0x1004
  │  entry->type = EX_TYPE_UACCESS_ERR_ZERO, entry->data = 0
  │
  ├─ ex_handler_uaccess_err_zero(entry, regs)
  │    ├─ pt_regs_write_reg(regs, 31, -EFAULT)  // 写 wzr → 无效果
  │    ├─ pt_regs_write_reg(regs, 31, 0)         // 写 wzr → 无效果
  │    └─ regs->pc = 0x2000 + 0xFFFFF004 = 0x1004  // 跳过故障指令
  │
  ▼
继续执行：
  regs->pc = 0x1004, 返回到调用者，检查返回值
```

**异常修复表与用户态缺页的交互**

```
do_page_fault(far, esr, regs)
  ├─ [用户态缺页] → handle_mm_fault()  // 正常分配物理页面
  │
  └─ [内核态缺页]
       ├─ is_el1_permission_fault()     // 内核访问用户内存?
       │    └─ insn_may_access_user()   // 检查指令是否在 uaccess 区域内
       │         └─ search_exception_tables(regs->pc)  // 查找异常表
       │              ├─ [找到 → EX_TYPE_UACCESS_CPY] → cpy_faulted_on_uaccess()
       │              │    └─ 仅当写/读匹配时才认为是 uaccess 故障
       │              └─ [找到 → 其他类型] → return true
       │
       ├─ [是 uaccess 且无异常表] → die_kernel_fault("access to user memory outside uaccess routines")
       │
       └─ [非 uaccess 缺页]
            └─ __do_kernel_fault()
                 └─ fixup_exception()    // 查找异常表修复
```

**关键设计要点**

1. **相对偏移存储**：`insn` 和 `fixup` 存储的是相对于条目自身的偏移量（`ARCH_HAS_RELATIVE_EXTABLE`），而非绝对地址。这使得 `__ex_table` 段在 KASLR 和模块加载时无需重定位，同时也减少了条目的体积（32 位偏移即可覆盖整个内核地址空间）

2. **二分查找**：`__ex_table` 在启动时（或构建时）排序，运行时使用 `bsearch()` 二分查找，时间复杂度 O(log n)，远优于线性扫描

3. **三级查找**：`search_exception_tables()` 按顺序搜索内核内置表、模块表、BPF 表，覆盖所有可能的内核代码路径

4. **类型化处理**：ARM64 的异常表条目包含 `type` 字段，支持多种处理策略（错误码+清零、批量拷贝过滤、非对齐加载零填充），比早期内核仅支持跳转修复更加灵活

5. **uaccess 过滤**：`EX_TYPE_UACCESS_CPY` 结合 ESR 的 WnR 位，区分用户态访问故障和内核内存故障。对于 `copy_from_user` 中的写操作（`sttr`），只有写故障才修复，读故障（内核空间故障）则让内核崩溃以暴露 bug

6. **insn_may_access_user 安全检查**：`do_page_fault()` 中通过 `is_el1_permission_fault()` 检测内核是否访问了用户内存，配合 `insn_may_access_user()` 检查该指令是否在异常表中，防止内核意外访问用户空间而不自知

7. **BPF 扩展**：BPF JIT 编译的代码也通过 `search_bpf_extables()` 集成到异常表机制中，确保 BPF 程序的内存访问安全

8. **模块支持**：可加载内核模块的 `__ex_table` 在模块加载时注册，卸载时清理，`search_module_extables()` 遍历所有模块的异常表

**模块异常表注册流程**：

```
模块加载过程（kernel/module/main.c）
────────────────────────────────────────────────────────────────
load_module()
  ├─ layout_and_allocate() → mod->extable = section_objs(info, "__ex_table")
  │    └─ 从 ELF 的 __ex_table section 中解析异常表条目
  │        mod->extable = 指向 section 数据
  │        mod->num_exentries = 条目数量
  │
  └─ post_relocation() → sort_extable(mod->extable, ...)
       └─ 重定位完成后，对异常表进行排序
            （模块异常表包含绝对地址，但排序后使用二分查找）

运行时查找：
search_module_extables(addr)
  └─ guard(rcu)()
  └─ mod = __module_address(addr)     // 通过地址查找所属模块
  └─ search_extable(mod->extable, mod->num_exentries, addr)  // 二分查找

模块卸载时：
free_module() / module_deallocate()
  └─ mod->extable 随模块内存一起释放（无需显式注销）
```

**BPF 异常表注册流程**：

```
BPF 程序加载过程（kernel/bpf/core.c）
────────────────────────────────────────────────────────────────
bpf_prog_select_runtime()
  └─ bpf_jit_binary_alloc()  → 分配 JIT 编译代码内存
  └─ bpf_prog_ksym_set_addr() → 注册符号

JIT 编译时（arm64 架构）：
  bpf_int_jit_compile()
    └─ build_body() → 生成 BPF 指令对应的 ARM64 指令
    └─ 为可能故障的内存访问生成 __ex_table 条目
    └─ prog->aux->extable = 指向异常表
    └─ prog->aux->num_exentries = 条目数量

运行时查找：
search_bpf_extables(addr)
  └─ rcu_read_lock()
  └─ prog = bpf_prog_ksym_find(addr)  // 通过地址查找 BPF 程序
  └─ search_extable(prog->aux->extable, prog->aux->num_exentries, addr)
  └─ rcu_read_unlock()

EX_TYPE_BPF 处理：
ex_handler_bpf(ex, regs)
  └─ regs->pc = get_ex_fixup(ex)  // 跳过故障的 BPF 指令
  └─ return true
```

---

**insn_may_access_user 安全检查详解**

`do_page_fault()` 中在调用 `__do_kernel_fault()` 之前，有一个重要的安全过滤环节：

```c
// do_page_fault() 中的安全过滤
if (is_ttbr0_addr(addr) && is_el1_permission_fault(addr, esr, regs)) {
    // 内核态访问用户空间地址，但触发了权限故障
    if (is_el1_instruction_abort(esr))
        die_kernel_fault("execution of user memory", ...);

    // 关键检查：当前指令是否在异常表中？
    if (!insn_may_access_user(regs->pc, esr))
        die_kernel_fault(
            "access to user memory outside uaccess routines", ...);
}
```

`insn_may_access_user()` 的过滤逻辑：

```
insn_may_access_user(addr, esr)
  ├─ search_exception_tables(addr)  // 查找当前指令
  │
  ├─ [未找到异常表条目] → return false
  │    └─ 内核在非 uaccess 区域访问了用户内存 → 内核 Bug! → die
  │
  └─ [找到异常表条目]
       ├─ EX_TYPE_UACCESS_CPY:
       │    └─ cpy_faulted_on_uaccess(ex, esr)
       │         ├─ 检查 ESR 的 WnR 位是否与 ex->data 中的 WRITE 标志匹配
       │         └─ 匹配 → return true  （是合法的 uaccess 故障）
       │         └─ 不匹配 → return false （内核内存故障，不修复）
       │
       └─ 其他类型 → return true
            └─ 条目在异常表中，说明是合法的 uaccess 访问
```

**设计意图**：如果内核代码在未使用 `copy_from_user`/`copy_to_user` 等 uaccess 辅助函数的情况下，直接解引用用户空间指针（如 `*user_ptr = val`），MMU 会因为 EL1 访问 EL0 地址且权限不匹配而触发权限故障。由于这条指令没有对应的异常表条目，`insn_may_access_user()` 返回 false，内核直接 die，从而暴露编程错误。这个机制防止了内核意外地信任用户空间指针。

---

**die_kernel_fault 与 Oops 处理流程**

当 `fixup_exception()` 未能找到修复条目时，`__do_kernel_fault()` 会经过多级降级处理：

```
__do_kernel_fault(addr, esr, regs)
  ├─ [第一级] fixup_exception(regs, esr)  // __ex_table 修复
  │    └─ 成功 → return（正常修复，继续执行）
  │
  ├─ [第二级] is_spurious_el1_translation_fault(addr, esr, regs)
  │    └─ 检测假缺页（TLB 未刷新导致的过期故障）
  │    └─ 成功 → WARN + return（仅警告）
  │
  ├─ [第三级] is_el1_mte_sync_tag_check_fault(esr)
  │    └─ MTE（内存标记扩展）标签检查故障
  │    └─ do_tag_recovery()  → 尝试 MTE 恢复
  │
  ├─ [第四级] efi_runtime_fixup_exception(regs, msg)
  │    └─ 仅当故障发生在 EFI 运行时服务中
  │    └─ 成功 → 设置返回值为 EFI_ABORTED, 跳转到恢复代码
  │
  └─ [最终] die_kernel_fault(msg, addr, esr, regs)
       └─ die_kernel_fault() 的实现：
            ├─ bust_spinlocks(1)             // 释放自旋锁限制
            ├─ pr_alert("Unable to handle kernel %s at ...")  // 打印故障信息
            ├─ kasan_non_canonical_hook(addr) // KASAN 检查
            ├─ mem_abort_decode(esr)          // 解码 ESR 寄存器
            ├─ show_pte(addr)                 // 显示页表信息
            ├─ die("Oops", regs, esr)         // 内核 Oops
            │    └─ __die() → 打印 CPU 寄存器、栈回溯
            │    └─ oops_enter()/oops_exit()
            │    └─ 如果 oops_in_progress > 0 → 递归死机
            ├─ bust_spinlocks(0)
            └─ make_task_dead(SIGKILL)        // 终止当前任务
```

---

**EFI 运行时服务的特殊处理**

EFI 运行时服务（如 `GetVariable`、`SetVariable`）运行在虚拟地址模式，可能因固件 Bug 触发缺页。ARM64 的 `efi_runtime_fixup_exception()` 提供了特殊的修复机制：

```c
// arch/arm64/kernel/efi.c
bool efi_runtime_fixup_exception(struct pt_regs *regs, const char *msg)
{
    // 检查是否在 EFI 运行时服务中
    if (!current_in_efi() || regs->pc >= TASK_SIZE_64)
        return false;

    // 打印固件 Bug 警告
    pr_err(FW_BUG "Unable to handle %s in EFI runtime service\n", msg);
    add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK);

    // 标记 EFI 运行时服务不可用
    clear_bit(EFI_RUNTIME_SERVICES, &efi.flags);

    // 修复寄存器状态，返回 EFI_ABORTED
    regs->regs[0]  = EFI_ABORTED;           // 返回值
    regs->regs[30] = efi_rt_stack_top[-1];  // LR 恢复
    regs->pc       = (u64)__efi_rt_asm_recover;  // 跳转到恢复代码

    return true;
}
```

**关键设计**：EFI 修复不依赖 `__ex_table`，而是直接在 `__do_kernel_fault()` 的降级路径中处理。这是因为 EFI 固件代码不在内核控制范围内，无法为其生成异常表条目。

---

**构建时排序机制（Build-Time Sort）**

为了减少启动时的性能开销，Linux 内核支持在构建时对 `__ex_table` 进行排序，避免运行时排序：

```
构建流程（scripts/link-vmlinux.sh + scripts/sorttable.c）
────────────────────────────────────────────────────────────────
1. 链接 vmlinux（包含未排序的 __ex_table 段）
2. 运行 scripts/sorttable 对 vmlinux 进行后处理：
   scripts/sorttable -s .tmp_vmlinux.nm-sort vmlinux
3. sorttable.c 的工作方式：
   ├─ 解析 ELF 文件，找到 __ex_table section
   ├─ 读取所有 exception_table_entry
   ├─ 使用 qsort() 按 insn 地址排序
   └─ 将排序后的表写回 ELF 文件
4. 设置内核符号 main_extable_sort_needed = 0
   （表示表已排序，跳过运行时排序）

启动时验证：
sort_main_extable()  // kernel/extable.c
  └─ if (main_extable_sort_needed)  // 构建时已排序，通常为 false
  └─ sort_extable(__start___ex_table, __stop___ex_table)

CONFIG_BUILDTIME_TABLE_SORT 配置选项控制此行为。
```

---

**copy_from_user 完整汇编示例**

ARM64 的 `copy_from_user` 使用了精细的异常处理，每个加载指令都对应一个异常表条目：

```asm
// arch/arm64/lib/copy_from_user.S
// 以单字节拷贝为例：

    .macro ldrb1 reg, ptr, val
    user_ldst 9998f, ldtrb, \reg, \ptr, \val
    .endm
    // 展开为：
    // 9998:  ldtrb   reg, [ptr]       // 从用户空间加载（可能故障）
    //         add     ptr, ptr, val
    //        _asm_extable_uaccess 9998b, l  // 生成 __ex_table 条目

    // 批量拷贝结尾的异常处理：
    // 9996:  b.cs    9997f            // 超过计数 → 跳转
    // 9997:  cmp     dst, dstin
    //         b.ne    9998f
    //         // 尝试至少拷贝一个字节
    // 9998:  ldtrb   tmp1w, [srcin]   // 可能再次故障
    //         strb    tmp1w, [dst], #1
    // 9998:  sub     x0, end, dst     // 返回未拷贝的字节数
    //         ret

    // 生成的 __ex_table 条目（按出现顺序）：
    // .section __ex_table, "a"
    // 条目1:  故障指令=9998b, 修复=9997f, type=EX_TYPE_UACCESS_ERR_ZERO, data=err=x0, zero=wzr
    // 条目2:  故障指令=9997b, 修复=9998f, type=EX_TYPE_UACCESS_ERR_ZERO, data=err=x0, zero=wzr
    // ...
    // 条目N:  故障指令=9998b(ldtrb), 修复=9998f, ...
    // .previous

// 修复代码统一处理：
// 9998:  sub     x0, end, dst     // x0 = 剩余未拷贝字节数
//         ret
```

**多级修复策略**：`copy_from_user` 使用了三级异常修复：
1. **批量拷贝中故障**：跳转到 `9997f`，检查是否至少拷贝了一个字节
2. **零字节拷贝故障**：跳转到 `9998f`，尝试使用 `ldtrb` 单字节拷贝
3. **单字节拷贝故障**：直接返回已拷贝字节数

这种设计确保了即使故障发生在批量拷贝的中间，也能返回部分拷贝结果，而不是全部失败。

---

**历史演变**

`__ex_table` 机制从 Linux 早期版本发展至今，经历了多次重要演进：

| 时期 | 版本 | 变化 | 描述 |
|------|------|------|------|
| 早期 | 1.x | 简单跳转表 | `insn` 和 `fixup` 两个字段，故障后直接跳转到修复代码 |
| 相对偏移 | 2.4+ | `ARCH_HAS_RELATIVE_EXTABLE` | 使用相对偏移替代绝对地址，消除 KASLR 重定位需求 |
| 二分查找 | 2.6+ | `search_extable()` | 从线性扫描改为二分查找，大幅提升性能 |
| 构建时排序 | 2.6.26+ | `CONFIG_BUILDTIME_TABLE_SORT` | 构建时排序，避免启动时开销 |
| 模块支持 | 早期 | `search_module_extables()` | 可加载模块的异常表支持 |
| 类型化处理 | 4.x+ | ARM64 引入 `type`/`data` 字段 | 从简单的跳转修复，演变为类型化处理（`EX_TYPE_UACCESS_ERR_ZERO`、`EX_TYPE_UACCESS_CPY` 等） |
| BPF 扩展 | 5.x+ | `search_bpf_extables()` | BPF JIT 程序的异常表支持 |
| 非对齐加载 | 5.x+ | `EX_TYPE_LOAD_UNALIGNED_ZEROPAD` | 非对齐内存访问的零填充修复 |
| MTE 集成 | 5.10+ | MTE 标签检查修复 | 在 `__do_kernel_fault()` 中增加 MTE 恢复路径 |

**关键演进趋势**：
- **精度提升**：从简单的"跳转到修复代码"演变为"根据故障类型选择不同的修复策略"
- **覆盖范围扩展**：从仅内核代码扩展到模块和 BPF JIT 代码
- **性能优化**：线性扫描 → 二分查找 → 构建时排序
- **安全增强**：`insn_may_access_user()` 等安全检查，防止非 uaccess 区域访问用户空间

---

### 5.3 mmap 系统调用

文件：`mm/mmap.c`（1,922 行），`mm/vma.c`（3,309 行）

```
SYSCALL_DEFINE6(mmap, ...)
  └─ vm_mmap_pgoff(file, addr, len, prot, flags, pgoff)
       └─ do_mmap(file, addr, len, ...)
            ├─ get_unmapped_area()     // 查找空闲区域
            ├─ mmap_region()           // 创建 VMA
            │    └─ vma_merge()        // 尝试合并相邻 VMA
            └─ file->f_op->mmap()      // 文件系统特定 mmap
```

### 5.4 mremap 与 mprotect

- `mm/mremap.c`（1,998 行）：`mremap()` 系统调用，移动/调整 VMA 映射
- `mm/mprotect.c`（1,007 行）：`mprotect()` 系统调用，修改 VMA 访问权限
- `mm/mlock.c`（825 行）：`mlock()` 系统调用，锁定页面在内存中
- `mm/madvise.c`（2,254 行）：`madvise()` 系统调用，向内核提供内存使用建议
- `mm/mseal.c`：`mseal()` 系统调用，锁定 VMA 布局防止修改

### 5.5 get_user_pages 框架

文件：`mm/gup.c`（3,557 行）

```
get_user_pages(start, nr_pages, gup_flags, pages, vmas)
  └─ __get_user_pages(mm, start, nr_pages, gup_flags, pages, vmas, locked)
       ├─ follow_page_mask()          // 检查页面是否已在
       └─ faultin_page()             // 触发缺页
            └─ handle_mm_fault()
```

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

## Part II: 内存回收与交换

## 8. 页面回收（Reclaim）

### 8.1 概述

文件：`mm/vmscan.c`（~7,910 行，vmscan 核心）、`mm/vmpressure.c`（vmpressure 压力通知）、`mm/shrinker.c`（shrink_slab 缓存收缩）、`mm/page_alloc.c`（直接回收触发）

页面回收是 Linux 内存管理最复杂的子系统之一，负责在内存压力下回收页面以释放内存给新的分配请求。页面回收系统的架构分为以下几个层次：

| 层次 | 组件 | 职责 |
|------|------|------|
| **触发层** | kswapd 内核线程、直接回收路径 | 检测内存压力，发起回收请求 |
| **控制层** | scan_control、shrink_node | 制定回收策略，控制扫描比例 |
| **执行层** | shrink_lruvec、shrink_folio_list | 遍历 LRU 链表，回收页面 |
| **通知层** | vmpressure、shrink_slab | 通知用户空间和 slab 缓存收缩 |

核心机制包括：

- **kswapd**：每个 NUMA 节点一个内核线程，异步回收，当 zone 水位低于 WMARK_LOW 时被唤醒
- **直接回收**：分配路径中同步回收，当 kswapd 无法满足需求时触发
- **LRU 链表**：按最近使用时间组织页面，分为匿名/文件、活跃/非活跃
- **Multi-Gen LRU**：基于代际（generation）的多级 LRU，更细粒度地跟踪页面活跃度
- **slab 收缩**：通过 shrink_slab() 收缩可回收的内核 slab 缓存

### 8.2 核心数据结构

#### 8.2.1 struct scan_control——回收控制参数

`mm/vmscan.c`，回收扫描的控制参数，贯穿整个回收路径：

```c
struct scan_control {
    // 目标回收页面数，shrink_list() 应回收的页数
    unsigned long nr_to_reclaim;

    // 允许扫描的节点掩码，为 NULL 时扫描所有节点
    nodemask_t *nodemask;

    // 达到限制的 memcg，作为回收的主要目标
    struct mem_cgroup *target_mem_cgroup;

    // 匿名页和文件页的扫描成本，用于平衡回收比例
    unsigned long anon_cost;
    unsigned long file_cost;

    // 主动回收（proactive reclaim）的 swappiness 值
    int *proactive_swappiness;

    // 活跃页是否可以降级为非活跃
    // DEACTIVATE_ANON = 1, DEACTIVATE_FILE = 2
    unsigned int may_deactivate:2;
    unsigned int force_deactivate:1;   // 强制降级
    unsigned int skipped_deactivate:1; // 跳过了降级

    unsigned int may_writepage:1;  // 是否允许写回脏页
    unsigned int may_unmap:1;      // 是否允许取消映射
    unsigned int may_swap:1;       // 是否允许交换

    unsigned int no_cache_trim_mode:1;        // 不允许缓存修剪模式
    unsigned int cache_trim_mode_failed:1;    // 缓存修剪模式失败过
    unsigned int proactive:1;                 // 用户空间发起的主动回收

    unsigned int memcg_low_reclaim:1;  // 强制回收 memory.low 保护区域
    unsigned int memcg_low_skipped:1;  // 跳过了 memory.low 保护区域
    unsigned int memcg_full_walk:1;    // 需要完整遍历 cgroup 树

    unsigned int hibernation_mode:1;   // 休眠模式
    unsigned int compaction_ready:1;   // 某 zone 准备好压缩
    unsigned int cache_trim_mode:1;    // 存在易回收的冷缓存
    unsigned int file_is_tiny:1;       // 文件页太少

    unsigned int no_demotion:1;  // 不允许降级到低层内存

    s8 order;          // 分配顺序
    s8 priority;       // 扫描优先级（0~DEF_PRIORITY=12，越低越激进）
    s8 reclaim_idx;    // 最高回收的 zone 索引
    gfp_t gfp_mask;    // GFP 掩码

    unsigned long nr_scanned;    // 已扫描页数
    unsigned long nr_reclaimed;  // 已回收页数

    // 回收统计——脏页、写回页、拥塞等
    struct {
        unsigned int dirty;
        unsigned int unqueued_dirty;
        unsigned int congested;
        unsigned int writeback;
        unsigned int immediate;
        unsigned int file_taken;
        unsigned int taken;
    } nr;

    struct reclaim_state reclaim_state;  // slab 回收状态
};
```

#### 8.2.2 struct lruvec——LRU 向量

`include/linux/mmzone.h`，每个 memcg 在每个 NUMA 节点上的 LRU 管理结构：

```c
struct lruvec {
    // LRU 链表数组：INACTIVE_ANON / ACTIVE_ANON / INACTIVE_FILE / ACTIVE_FILE / UNEVICTABLE
    struct list_head lists[NR_LRU_LISTS];

    // per-lruvec 的 LRU 自旋锁
    spinlock_t lru_lock;

    // 回收成本跟踪——用于调整匿名/文件页扫描比例
    unsigned long anon_cost;
    unsigned long file_cost;

    // 非驻留页年龄，由 LRU 移动驱动
    atomic_long_t nonresident_age;

    // 上次回收周期时的 refault 计数
    unsigned long refaults[ANON_AND_FILE];

    // 各种 lruvec 状态标志（如 LRUVEC_CGROUP_CONGESTED）
    unsigned long flags;

#ifdef CONFIG_LRU_GEN
    // Multi-Gen LRU 状态
    struct lru_gen_folio lrugen;
#endif
};
```

#### 8.2.3 LRU 链表枚举

`include/linux/mmzone.h`：

```c
enum lru_list {
    LRU_INACTIVE_ANON = LRU_BASE,             // 非活跃匿名页
    LRU_ACTIVE_ANON   = LRU_BASE + LRU_ACTIVE, // 活跃匿名页
    LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,   // 非活跃文件页
    LRU_ACTIVE_FILE   = LRU_BASE + LRU_FILE + LRU_ACTIVE, // 活跃文件页
    LRU_UNEVICTABLE,   // 不可回收页（mlock、ramfs 等）
    NR_LRU_LISTS
};
```

#### 8.2.4 enum scan_balance——扫描平衡策略

`mm/vmscan.c`：

```c
enum scan_balance {
    SCAN_EQUAL,  // 平等扫描匿名和文件页（OOM 边缘时）
    SCAN_FRACT,  // 按 swappiness 和成本比例扫描
    SCAN_ANON,   // 仅扫描匿名页
    SCAN_FILE,   // 仅扫描文件页
};
```

#### 8.2.5 struct lru_gen_folio——Multi-Gen LRU 核心结构

`include/linux/mmzone.h`，MGLRU 的代际管理结构：

```c
struct lru_gen_folio {
    // 最新代序号，aging 递增此值
    unsigned long max_seq;
    // 最老代序号，eviction 递增此值（匿名/文件各一个）
    unsigned long min_seq[ANON_AND_FILE];
    // 每个代际的出生时间（jiffies）
    unsigned long timestamps[MAX_NR_GENS];
    // 多代 LRU 链表：[generation][anon/file][zone]
    struct list_head folios[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
    // 多代 LRU 大小（最终一致性）
    long nr_pages[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
    // refault 的指数移动平均
    unsigned long avg_refaulted[ANON_AND_FILE][MAX_NR_TIERS];
    // evicted+protected 的指数移动平均
    unsigned long avg_total[ANON_AND_FILE][MAX_NR_TIERS];
    // protected 计数（需 LRU 锁保护）
    unsigned long protected[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS];
    // evicted 计数（无需锁，原子操作）
    atomic_long_t evicted[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS];
    // refaulted 计数（无需锁，原子操作）
    atomic_long_t refaulted[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS];
    // 是否启用 MGLRU
    bool enabled;
    // 所属的 memcg generation
    u8 gen;
    // 所属的 list segment
    u8 seg;
    // per-node 的 lru_gen_folio 链表（用于全局回收）
    struct hlist_nulls_node list;
};
```

### 8.3 kswapd 内核线程

#### 8.3.1 初始化与主循环

`mm/vmscan.c:kswapd()`：

```
kswapd 初始化：
  kswapd_init()
    └─ kswapd_run(nid)           // 每个 NUMA 节点启动一个 kswapd 线程
         └─ kswapd()             // 主循环
              ├─ 设置 PF_MEMALLOC | PF_KSWAPD 标志
              ├─ 循环：
              │    ├─ kswapd_try_to_sleep()  // 尝试睡眠，等待水位线触发
              │    ├─ 读取 kswapd_order 和 kswapd_highest_zoneidx
              │    ├─ 写入 0 和 MAX_NR_ZONES 以重置
              │    └─ balance_pgdat(pgdat, alloc_order, highest_zoneidx)
              │         └─ 返回 reclaim_order
              │              ├─ 如果 reclaim_order < alloc_order
              │              │   → 重新进入 kswapd_try_sleep
              │              └─ 否则继续循环
              └─ 清除 PF_MEMALLOC | PF_KSWAPD 标志

kswapd 唤醒条件：
  - 分配页面时 zone 水位低于 WMARK_LOW
  - 通过 wakeup_kswapd() 函数唤醒
```

#### 8.3.2 balance_pgdat——平衡节点内存

`mm/vmscan.c:balance_pgdat()`，这是 kswapd 的核心函数：

```c
static int balance_pgdat(pg_data_t *pgdat, int order, int highest_zoneidx)
{
    // 1. 初始化 scan_control，设置 gfp_mask=GFP_KERNEL, order, may_unmap=1
    // 2. 收集 zone 的 watermark_boost（内存压缩加速导致的提升）
    // 3. 进入优先级循环，从 DEF_PRIORITY(=12) 递减到 0

    do {
        // 3.1 检查 pgdat 是否平衡（pgdat_balanced）
        //     如果 boosting 没激活且已平衡，直接退出
        // 3.2 如果 buffer_heads 超过限制，扩大 reclaim_idx

        // 3.3 kswapd_age_node() — 后台老化处理
        //     将活跃页降级到非活跃，给页面被引用机会

        // 3.4 memcg1_soft_limit_reclaim() — 软限制回收

        // 3.5 计算是否需要提升优先级
        //     通过 pgdat_balanced() 检查水位线

        // 3.6 调用 kswapd_shrink_node() 或 shrink_node() 回收页面
        //     实际执行 shrink_node(pgdat, &sc)

        // 3.7 检查是否达到平衡，如果达到则 break
        //     否则降低优先级继续
    } while (--sc.priority >= 0);

    // 4. 完成后重置 zone 的 watermark_boost
    // 5. 如果 reclaim_order < alloc_order，唤醒 kcompactd 进行压缩
}
```

关键点：

- **boosting 机制**：当 zone 有 watermark_boost 时，kswapd 以更高优先级回收，但限制 may_writepage 和 may_swap 以避免 IO
- **buffer_heads_over_limit**：当 buffer_heads 过多时，扩大回收范围到所有 zone
- **kswapd_age_node**：在正式回收前先做后台老化，将活跃页降级到非活跃

#### 8.3.3 kswapd_age_node——后台老化

`mm/vmscan.c:kswapd_age_node()`：

```c
static void kswapd_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
    if (lru_gen_enabled()) {
        lru_gen_age_node(pgdat, sc);  // MGLRU 的老化路径
        return;
    }

    // 传统 LRU 老化：
    // 1. 检查匿名页是否可老化（can_age_anon_pages）
    // 2. 检查非活跃匿名页是否太少（inactive_is_low）
    // 3. 遍历所有 memcg，对每个 lruvec 调用 shrink_active_list()
    //    将活跃匿名页降级到非活跃
}
```

### 8.4 直接回收路径

#### 8.4.1 触发条件

当页面分配路径 `__alloc_pages_slowpath` 中，快速路径和慢速路径均失败后，进入直接回收：

```
__alloc_pages_slowpath()
  ├─ 第一次尝试 get_page_from_freelist() 失败
  ├─ 尝试直接压缩（直接回收前先尝试压缩）
  ├─ 如果都失败，进入直接回收：
  │   └─ __alloc_pages_direct_reclaim(gfp_mask, order, alloc_flags, ac)
  │        ├─ __perform_reclaim()
  │        │    └─ try_to_free_pages(ac->zonelist, order, gfp_mask, ac->nodemask)
  │        └─ get_page_from_freelist()  // 回收后再次尝试分配
  ├─ 如果回收后仍然失败，检查 should_reclaim_retry()
  │   └─ 如果还有可回收页面，增加 no_progress_loops 后重试
  └─ 如果 MAX_RECLAIM_RETRIES 次后仍失败，触发 OOM
```

#### 8.4.2 __alloc_pages_direct_reclaim

`mm/page_alloc.c`：

```c
static struct page *__alloc_pages_direct_reclaim(gfp_t gfp_mask, unsigned int order,
        unsigned int alloc_flags, const struct alloc_context *ac,
        unsigned long *did_some_progress)
{
    struct page *page = NULL;
    bool drained = false;

    *did_some_progress = __perform_reclaim(gfp_mask, order, ac);
    if (unlikely(!(*did_some_progress)))
        goto out;

retry:
    // 回收后重新尝试分配
    page = get_page_from_freelist(gfp_mask, order, alloc_flags, ac);

    // 如果仍然失败，可能是 per-CPU 页或高原子预留导致
    if (!page && !drained) {
        unreserve_highatomic_pageblock(ac, false);  // 释放高原子预留
        drain_all_pages(NULL);                       // 刷新 per-CPU 页
        drained = true;
        goto retry;
    }
out:
    return page;
}
```

#### 8.4.3 try_to_free_pages → do_try_to_free_pages

`mm/vmscan.c`：

```c
unsigned long try_to_free_pages(struct zonelist *zonelist, int order,
                                gfp_t gfp_mask, nodemask_t *nodemask)
{
    struct scan_control sc = {
        .nr_to_reclaim = SWAP_CLUSTER_MAX,  // 默认回收 32 页
        .gfp_mask = current_gfp_context(gfp_mask),
        .reclaim_idx = gfp_zone(gfp_mask),
        .order = order,
        .nodemask = nodemask,
        .priority = DEF_PRIORITY,      // 初始优先级 12
        .may_writepage = 1,            // 允许写回
        .may_unmap = 1,                // 允许取消映射
        .may_swap = 1,                 // 允许交换
    };

    // 限流检查：如果当前进程被限流，直接返回 1 避免 OOM
    if (throttle_direct_reclaim(sc.gfp_mask, zonelist, nodemask))
        return 1;

    nr_reclaimed = do_try_to_free_pages(zonelist, &sc);
    return nr_reclaimed;
}
```

`do_try_to_free_pages()` 的优先级循环：

```
do_try_to_free_pages(zonelist, sc)
  └─ 循环：优先级从 DEF_PRIORITY(12) 递减到 0
       ├─ vmpressure_prio()  // 更新压力优先级
       ├─ shrink_zones(zonelist, sc)  // 遍历所有 zone 回收
       ├─ 如果 sc->nr_reclaimed >= nr_to_reclaim，跳出
       └─ 如果 sc->compaction_ready，跳出（压缩就绪时不回收）
  └─ 循环结束后：
       ├─ 刷新每个 pgdat 的 refault 快照
       ├─ 如果未回收任何页面：
       │    ├─ 如果 compaction_ready → 返回 1（不触发 OOM）
       │    ├─ 如果 memcg_full_walk 未设置 → 设标志后重试
       │    ├─ 如果 skipped_deactivate → 强制降级后重试
       │    └─ 如果 memcg_low_skipped → 强制回收后重试
       └─ 返回 sc->nr_reclaimed
```

#### 8.4.4 should_reclaim_retry——回收重试判断

`mm/page_alloc.c`：

```c
static bool should_reclaim_retry(gfp_t gfp_mask, unsigned order,
        struct alloc_context *ac, int alloc_flags,
        bool did_some_progress, int *no_progress_loops)
{
    // 低成本分配（order <= PAGE_ALLOC_COSTLY_ORDER=3）且有进展：重置计数器
    // 否则：递增计数器
    if (did_some_progress && order <= PAGE_ALLOC_COSTLY_ORDER)
        *no_progress_loops = 0;
    else
        (*no_progress_loops)++;

    // 超过最大重试次数（MAX_RECLAIM_RETRIES=16），返回 false → OOM
    if (*no_progress_loops > MAX_RECLAIM_RETRIES)
        return false;

    // 遍历所有 zone，检查是否有至少一个 zone 满足：
    //   available = zone_reclaimable_pages(zone) + NR_FREE_PAGES
    //   如果 available >= min_wmark，则可以重试
    for_each_zone_zonelist_nodemask(...) {
        available = reclaimable = zone_reclaimable_pages(zone);
        available += zone_page_state_snapshot(zone, NR_FREE_PAGES);
        wmark = __zone_watermark_ok(zone, order, min_wmark, ..., available);
        if (wmark)
            return true;
    }
    return false;
}
```

### 8.5 shrink_node——核心回收函数

#### 8.5.1 整体流程

`mm/vmscan.c:shrink_node()`：

```
shrink_node(pgdat, sc)
  │
  ├─ [MGLRU 路径] 如果 lru_gen_enabled() && root_reclaim(sc)
  │    └─ lru_gen_shrink_node(pgdat, sc)  // MGLRU 专用回收
  │         └─ 直接返回
  │
  └─ [传统 LRU 路径]
       ├─ 1. prepare_scan_control(pgdat, sc)
       │    └─ 初始化 anon_cost/file_cost、may_deactivate、cache_trim_mode、file_is_tiny
       │
       ├─ 2. shrink_node_memcgs(pgdat, sc)
       │    └─ 遍历 memcg，对每个调用 shrink_lruvec()
       │
       ├─ 3. flush_reclaim_state(sc)
       │
       ├─ 4. vmpressure()  — 记录回收效率
       │
       ├─ 5. kswapd 特殊处理：
       │    ├─ 如果所有回收的页面都在写回 → 设置 PGDAT_WRITEBACK 标志
       │    └─ 如果 nr_immediate > 0 → 限流等待写回完成
       │
       ├─ 6. 拥塞处理：
       │    ├─ 如果所有脏页都拥塞 → 设置 LRUVEC_CGROUP/NODE_CONGESTED
       │    └─ 直接回收者遇到拥塞 → reclaim_throttle(CONGESTED)
       │
       └─ 7. should_continue_reclaim() 检查
            └─ 如果回收/压缩模式需要，且非活跃页足够多，重试
```

#### 8.5.2 prepare_scan_control——扫描参数准备

`mm/vmscan.c`：

```c
static void prepare_scan_control(pg_data_t *pgdat, struct scan_control *sc)
{
    // 1. 读取 anon_cost 和 file_cost（从目标 lruvec）
    sc->anon_cost = target_lruvec->anon_cost;
    sc->file_cost = target_lruvec->file_cost;

    // 2. 确定 may_deactivate（是否允许活跃→非活跃降级）
    //    - 如果观察到 refault（新的工作集建立），强制降级
    //    - 如果非活跃页太少（inactive_is_low），允许降级
    //    - 否则禁止降级（保护活跃页）

    // 3. cache_trim_mode — 缓存修剪模式
    //    如果非活跃文件页足够多（file >> sc->priority），
    //    且不活跃文件页不需要降级，则优先回收文件页缓存

    // 4. file_is_tiny — 文件页太小检测
    //    如果 (file + free) <= total_high_wmark，说明文件页很少
    //    此时应扫描匿名页以避免文件页抖动
}
```

**inactive_is_low** 判断逻辑：

```c
static bool inactive_is_low(struct lruvec *lruvec, enum lru_list inactive_lru)
{
    // 计算活跃与非活跃页的比例
    // inactive_ratio = sqrt(10 * GB)，其中 GB = (inactive + active) >> (30 - PAGE_SHIFT)
    // 如果 inactive * inactive_ratio < active，认为非活跃页太少
    // 例如：4GB 内存时，inactive_ratio ≈ 6，active 应不超过 inactive 的 6 倍
}
```

#### 8.5.3 shrink_node_memcgs——遍历 memcg 回收

`mm/vmscan.c`：

```c
static void shrink_node_memcgs(pg_data_t *pgdat, struct scan_control *sc)
{
    // kswapd 总是做完整遍历；直接回收者可以做部分遍历（partial walk）
    // 部分遍历在回收达到 nr_to_reclaim 后中断，以降低延迟

    memcg = mem_cgroup_iter(target_memcg, NULL, partial);
    do {
        // 1. 计算 memcg 的 memory.min/memory.low 保护
        mem_cgroup_calculate_protection(target_memcg, memcg);

        // 2. 如果低于 memory.min → 硬保护，跳过（但不跳过 OOM 检查）
        // 3. 如果低于 memory.low → 软保护，除非强制回收（memcg_low_reclaim）

        // 4. 对每个 lruvec 调用：
        shrink_lruvec(lruvec, sc);              // LRU 页面回收
        shrink_slab(sc->gfp_mask, pgdat->node_id, memcg, sc->priority);  // slab 收缩

        // 5. vmpressure() 记录回收效率

        // 6. 部分遍历：如果达到 nr_to_reclaim，中断
    } while ((memcg = mem_cgroup_iter(target_memcg, memcg, partial)));
}
```

#### 8.5.4 shrink_lruvec——回收单个 LRU 向量

`mm/vmscan.c`：

```c
static void shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
    // 1. get_scan_count() 计算每个 LRU 链表应扫描的页数
    //    结果保存在 nr[NR_LRU_LISTS] 数组中

    // 2. 循环扫描直到所有链表都扫完或达到回收目标
    while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] || nr[LRU_INACTIVE_FILE]) {
        for_each_evictable_lru(lru) {
            if (nr[lru]) {
                nr_to_scan = min(nr[lru], SWAP_CLUSTER_MAX);  // 每次最多 32 页
                nr[lru] -= nr_to_scan;
                nr_reclaimed += shrink_list(lru, nr_to_scan, lruvec, sc);
            }
        }

        // 3. 如果达到回收目标，且不是直接回收 DEF_PRIORITY 模式：
        //    调整比例，优先回收剩余较多的链表
        if (nr_reclaimed >= nr_to_reclaim && !proportional_reclaim) {
            // 按比例停止扫描较小的 LRU
            percentage = smaller_lru * 100 / scan_target;
            nr[larger_lru] = targets[larger_lru] * (100 - percentage) / 100;
        }
    }

    // 4. 如果匿名页可老化且非活跃太少，调用 shrink_active_list()
    //    以平衡匿名页的活跃/非活跃比例
}
```

#### 8.5.5 get_scan_count——计算扫描比例

`mm/vmscan.c`：

```c
static void get_scan_count(struct lruvec *lruvec, struct scan_control *sc,
                           unsigned long *nr)
{
    int swappiness = sc_swappiness(sc, memcg);

    // 决策树：
    // 1. 如果没有 swap 空间 → SCAN_FILE（只扫描文件页）
    // 2. cgroup 回收且 swappiness=0 → SCAN_FILE
    // 3. swappiness == SWAPPINESS_ANON_ONLY → SCAN_ANON（仅匿名页）
    // 4. priority == 0（OOM 边缘）且 swappiness 非零 → SCAN_EQUAL（平等扫描）
    // 5. file_is_tiny → SCAN_ANON（文件页太少，强制扫描匿名页）
    // 6. cache_trim_mode → SCAN_FILE（有足够缓存，优先回收文件页）
    // 7. 默认 → SCAN_FRACT（按 swappiness 和成本比例扫描）

    // SCAN_FRACT 模式：
    //   calculate_pressure_balance() 计算匿名/文件页的比例
    //   公式：
    //     total_cost = anon_cost + file_cost
    //     anon_cost = total_cost + anon_cost
    //     file_cost = total_cost + file_cost
    //     ap = swappiness * (total_cost + 1) / (anon_cost + 1)
    //     fp = (MAX_SWAPPINESS - swappiness) * (total_cost + 1) / (file_cost + 1)
    //     fraction[ANON] = ap, fraction[FILE] = fp, denominator = ap + fp

    // 对每个 LRU 链表：
    //   scan = lruvec_size >> sc->priority  // 按优先级缩小扫描范围
    //   scan = apply_proportional_protection()  // 应用 memory.min/low 保护
    //   根据 scan_balance 类型调整 scan 值
}
```

### 8.6 LRU 链表管理

#### 8.6.1 shrink_list——LRU 分发函数

`mm/vmscan.c`：

```c
static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan,
                                 struct lruvec *lruvec, struct scan_control *sc)
{
    if (is_active_lru(lru)) {
        // 活跃链表：只有在 may_deactivate 允许时才做降级
        if (sc->may_deactivate & (1 << is_file_lru(lru)))
            shrink_active_list(nr_to_scan, lruvec, sc, lru);
        else
            sc->skipped_deactivate = 1;  // 标记跳过了降级
        return 0;  // 活跃链表回收返回 0（降级不直接回收）
    }

    // 非活跃链表：直接回收
    return shrink_inactive_list(nr_to_scan, lruvec, sc, lru);
}
```

#### 8.6.2 shrink_inactive_list——回收非活跃链表

`mm/vmscan.c`：

```
shrink_inactive_list(nr_to_scan, lruvec, sc, lru)
  │
  ├─ 1. 检查是否 too_many_isolated()（太多页面正在被隔离）
  │    └─ 如果是，限流等待（VMSCAN_THROTTLE_ISOLATED）
  │
  ├─ 2. lru_add_drain()  — 刷新 per-CPU 页面缓存
  │
  ├─ 3. isolate_lru_folios()  — 从 LRU 链表中隔离页面
  │    └─ 从链表尾部获取 nr_to_scan 个页面，移到临时链表
  │
  ├─ 4. 更新 NR_ISOLATED_ANON/FILE 统计
  │
  ├─ 5. shrink_folio_list()  — 核心回收，处理隔离出的页面
  │    └─ 返回回收的页数
  │
  ├─ 6. move_folios_to_lru()  — 将未回收的页面放回 LRU
  │
  ├─ 7. lru_note_cost_unlock_irq()  — 更新回收成本
  │
  └─ 8. 如果所有隔离页面都是未排队脏页 → 唤醒 flusher 线程
```

#### 8.6.3 shrink_active_list——活跃→非活跃降级

`mm/vmscan.c`：

```
shrink_active_list(nr_to_scan, lruvec, sc, lru)
  │
  ├─ 1. isolate_lru_folios()  — 从活跃链表中隔离页面
  │
  ├─ 2. 对每个隔离的页面：
  │    ├─ folio_referenced()  — 检查是否被引用
  │    ├─ 如果被引用且是 VM_EXEC 文件页：
  │    │   └─ 放回活跃链表（可执行代码多一次机会）
  │    └─ 否则：
  │         ├─ folio_clear_active()  // 清除活跃标志
  │         ├─ folio_set_workingset()  // 标记为工作集
  │         └─ 移到非活跃链表
  │
  ├─ 3. move_folios_to_lru()  — 将页面放回 LRU（活跃/非活跃分别处理）
  │
  └─ 4. lru_note_cost()  — 更新旋转成本
```

#### 8.6.4 isolate_lru_folios——页面隔离

`mm/vmscan.c`：

```c
static unsigned long isolate_lru_folios(unsigned long nr_to_scan,
        struct lruvec *lruvec, struct list_head *dst,
        unsigned long *nr_scanned, struct scan_control *sc,
        enum lru_list lru)
{
    // 从 LRU 链表尾部扫描，逐个获取 folio
    while (scan < nr_to_scan && !list_empty(src)) {
        folio = lru_to_folio(src);

        // 跳过条件：
        // 1. zone 索引 > reclaim_idx（不在回收范围内）
        // 2. 不是 LRU 页（已被其他线程隔离）
        // 3. may_unmap=0 但页面被映射
        // 4. 无法获取 folio 引用（正在被释放）

        // 获取成功：folio_test_clear_lru() 清除 LRU 标志
        // 移到目的链表
        nr_taken += nr_pages;
    }

    // 跳过的页面拼接到 LRU 链表头部
    // 更新统计（PGSCAN_SKIP）
}
```

#### 8.6.5 lru_note_cost——回收成本跟踪

`mm/swap.c`：

```c
void lru_note_cost_unlock_irq(struct lruvec *lruvec, bool file,
        unsigned int nr_io, unsigned int nr_rotated)
{
    // cost = nr_io * SWAP_CLUSTER_MAX + nr_rotated
    // 如果 file→file_cost += cost；否则 anon_cost += cost
    //
    // 衰减：当 (file_cost + anon_cost) > lrusize / 4 时，两者都除以 2
    // 这实现了浮动平均，新事件权重更高
    //
    // 向上遍历父 lruvec，将成本同步到上层
}
```

### 8.7 shrink_folio_list——页面级核心回收

#### 8.7.1 整体流程

`mm/vmscan.c`（约 500 行），这是页面回收的最终执行函数：

```
shrink_folio_list(folio_list, pgdat, sc, stat, ignore_references, memcg)
  │
  └─ 循环处理每个 folio：
       │
       ├─ 1. folio_trylock() 锁定页面
       ├─ 2. 跳过 hwpoison 页面
       │
       ├─ 3. 检查页面是否可回收（folio_evictable）
       │    └─ 不可回收 → 激活
       │
       ├─ 4. 检查 may_unmap → 如果页面被映射且不允许取消映射 → 保留
       │
       ├─ 5. 检测脏页和写回状态
       │    ├─ 写回检查：
       │    │   ├─ Case 1: kswapd + reclaim 标志 + PGDAT_WRITEBACK → 激活（避免无限等待）
       │    │   ├─ Case 2: 非立即回收或无 FS 访问 → 设置 reclaim 标志后激活
       │    │   └─ Case 3: legacy memcg + reclaim 标志 → 等待写回完成
       │    └─ 脏页检查：如果脏且非写回 → 统计 nr_unqueued_dirty
       │
       ├─ 6. folio_check_references() 引用检查
       │    ├─ FOLIOREF_ACTIVATE → 激活
       │    ├─ FOLIOREF_KEEP → 保留
       │    └─ FOLIOREF_RECLAIM / FOLIOREF_RECLAIM_CLEAN → 尝试回收
       │
       ├─ 7. demotion 检查：如果支持降级（到 PMEM 等慢速内存），先尝试降级
       │
       ├─ 8. [匿名页处理] 如果 folio_test_anon() && folio_test_swapbacked() && !swapcache
       │    ├─ 检查 DMA 钉住 → 激活
       │    ├─ 大页拆分 → 分别处理
       │    ├─ folio_alloc_swap() 分配 swap 空间
       │    │   ├─ 成功 → 继续
       │    │   └─ 失败 → 激活
       │    └─ folio_mark_dirty() 标记脏（处理 MADV_FREE 页面）
       │
       ├─ 9. [取消映射] 如果页面被映射
       │    ├─ try_to_unmap() 取消所有进程的映射
       │    └─ 如果仍然被映射 → 激活（取消映射失败）
       │
       ├─ 10. [文件页脏页处理] 如果 folio_test_dirty()
       │     ├─ 文件页脏 → 设置 NR_VMSCAN_IMMEDIATE 后激活
       │     └─ 匿名页脏 → pageout() 写回
       │          ├─ PAGE_KEEP → 保留
       │          ├─ PAGE_ACTIVATE → 激活
       │          └─ PAGE_SUCCESS → 统计后继续
       │
       ├─ 11. [释放前准备] 
       │     ├─ filemap_release_folio() 释放缓冲区
       │     └─ __remove_mapping() 从页面缓存/swap 缓存中移除
       │
       └─ 12. [释放页面]
              ├─ folio_unqueue_deferred_split()
              ├─ 批量释放（folio_batch → free_unref_folios）
              └─ nr_reclaimed += nr_pages
```

#### 8.7.2 匿名页处理详细代码

```c
// shrink_folio_list() 中的匿名页处理
if (folio_test_anon(folio) && folio_test_swapbacked(folio) &&
        !folio_test_swapcache(folio)) {
    // 1. 需要 __GFP_IO 才能分配 swap
    if (!(sc->gfp_mask & __GFP_IO))
        goto keep_locked;

    // 2. DMA 钉住的页面不能回收
    if (folio_maybe_dma_pinned(folio))
        goto keep_locked;

    // 3. 大页处理：尝试拆分，拆分成小页后分别处理
    if (folio_test_large(folio)) {
        // 检查引用计数，如果只有隔离引用，尝试拆分
        if (folio_expected_ref_count(folio) != folio_ref_count(folio) - 1)
            goto activate_locked;
        // 拆分部分映射的大页
        if (data_race(!list_empty(&folio->_deferred_list) &&
            folio_test_partially_mapped(folio)) &&
            split_folio_to_list(folio, folio_list))
            goto activate_locked;
    }

    // 4. 分配 swap 空间
    if (folio_alloc_swap(folio)) {
        // 大页 swap 分配失败时回退到拆分后小页
        if (!folio_test_large(folio))
            goto activate_locked_split;
        if (split_folio_to_list(folio, folio_list))
            goto activate_locked;
        // 再次尝试分配 swap
        if (folio_alloc_swap(folio))
            goto activate_locked_split;
    }

    // 5. MADV_FREE 页面处理：确保脏标志正确
    folio_mark_dirty(folio);
}
```

#### 8.7.3 文件页处理详细流程

```c
// shrink_folio_list() 中的文件页脏页处理
if (folio_test_dirty(folio)) {
    if (folio_is_file_lru(folio)) {
        // 文件脏页：立即回收标志，等待 flusher 写回
        node_stat_mod_folio(folio, NR_VMSCAN_IMMEDIATE, nr_pages);
        if (!folio_test_reclaim(folio))
            folio_set_reclaim(folio);
        goto activate_locked;  // 激活后等待 flusher 处理
    }

    // 匿名脏页（shmem）：通过 pageout() 写回
    if (references == FOLIOREF_RECLAIM_CLEAN)
        goto keep_locked;
    if (!may_enter_fs(folio, sc->gfp_mask))
        goto keep_locked;
    if (!sc->may_writepage)
        goto keep_locked;

    // 写回前刷新 TLB
    try_to_unmap_flush_dirty();
    switch (pageout(folio, mapping, &plug, folio_list)) {
    case PAGE_KEEP:    goto keep_locked;
    case PAGE_ACTIVATE: goto activate_locked;
    case PAGE_SUCCESS:  // 写回成功，继续尝试释放
    case PAGE_CLEAN:    // 页面已干净，尝试释放
    }
}

// 干净文件页的释放路径：
// 1. filemap_release_folio() 释放 buffer_head 等附属结构
// 2. __remove_mapping() 从 address_space 的页面缓存中移除
// 3. folio 放入批量释放队列，最终通过 free_unref_folios() 释放
```

### 8.8 slab 缓存收缩

#### 8.8.1 shrink_slab 接口

`mm/shrinker.c`：

```c
unsigned long shrink_slab(gfp_t gfp_mask, int nid, struct mem_cgroup *memcg,
                          int priority)
{
    // 非 root memcg → 走 shrink_slab_memcg() 路径
    if (!mem_cgroup_disabled() && !mem_cgroup_is_root(memcg))
        return shrink_slab_memcg(gfp_mask, nid, memcg, priority);

    // 全局 slab 收缩：
    // 1. 遍历 shrinker_list 中的所有 shrinker
    // 2. 对每个 shrinker：
    //    - 通过 shrinker_try_get() 获取引用
    //    - do_shrink_slab() 执行收缩
    //    - shrinker_put() 释放引用
    // 3. 使用 RCU 锁保护遍历期间的 shrinker 存活
}
```

#### 8.8.2 回收路径中的调用

在 `shrink_node_memcgs()` 中，对每个 memcg 执行完 `shrink_lruvec()` 后立即调用 `shrink_slab()`：

```c
shrink_lruvec(lruvec, sc);  // 回收 LRU 页面
shrink_slab(sc->gfp_mask, pgdat->node_id, memcg, sc->priority);  // 收缩 slab
```

### 8.9 内存压力通知（vmpressure）

#### 8.9.1 数据结构和阈值

`mm/vmpressure.c`：

```c
enum vmpressure_levels {
    VMPRESSURE_LOW = 0,      // 轻度压力
    VMPRESSURE_MEDIUM,       // 中度压力
    VMPRESSURE_CRITICAL,     // 严重压力
    VMPRESSURE_NUM_LEVELS,
};

// 阈值配置
static const unsigned int vmpressure_level_med = 60;       // 60% → MEDIUM
static const unsigned int vmpressure_level_critical = 95;   // 95% → CRITICAL
static const unsigned int vmpressure_level_critical_prio = ilog2(100 / 10);  // priority 3
```

#### 8.9.2 压力计算

```c
static enum vmpressure_levels vmpressure_calc_level(unsigned long scanned,
                                                    unsigned long reclaimed)
{
    unsigned long scale = scanned + reclaimed;

    // 如果 reclaimed >= scanned（回收了 slab 等不计入 scanned 的页面）
    if (reclaimed >= scanned)
        goto out;  // 压力为 0

    // pressure = (scale - reclaimed * scale / scanned) * 100 / scale
    // 即：未被回收的比例（百分比）
    pressure = scale - (reclaimed * scale / scanned);
    pressure = pressure * 100 / scale;

out:
    if (pressure >= 95)  return VMPRESSURE_CRITICAL;
    if (pressure >= 60)  return VMPRESSURE_MEDIUM;
    return VMPRESSURE_LOW;
}
```

#### 8.9.3 调用点

- **`shrink_node()`**：每次回收完一个节点后调用 `vmpressure(sc->gfp_mask, sc->target_mem_cgroup, true, ...)`
- **`shrink_node_memcgs()`**：对每个 memcg 调用 `vmpressure(sc->gfp_mask, memcg, false, ...)`
- **`do_try_to_free_pages()`**：每次优先级循环前调用 `vmpressure_prio()`，传递当前优先级

### 8.10 Multi-Gen LRU（MGLRU）

#### 8.10.1 架构概述

MGLRU 将页面分为多个代际（generation），每个代际代表一个时间段的页面集合。与传统双链表 LRU 相比：

| 特性 | 传统 LRU | MGLRU |
|------|----------|-------|
| 页面组织 | active/inactive 双链表 | 多个代际（MAX_NR_GENS=4） |
| 活跃度跟踪 | 引用位 + 两次机会 | 代际间移动，精细分级 |
| 扫描效率 | 需要扫描整个链表 | 直接定位最老代际 |
| refault 检测 | workingset 检测 | 分层 tier 统计 |
| 公平性 | 全局扫描 | per-memcg 独立代际 |

#### 8.10.2 回收路径

当 `lru_gen_enabled() && root_reclaim(sc)` 时，`shrink_node()` 直接调用 MGLRU 路径：

```c
static void lru_gen_shrink_node(struct pglist_data *pgdat, struct scan_control *sc)
{
    lru_add_drain();
    blk_start_plug(&plug);
    set_mm_walk(pgdat, sc->proactive);
    set_initial_priority(pgdat, sc);

    if (mem_cgroup_disabled())
        shrink_one(&pgdat->__lruvec, sc);  // 全局回收
    else
        shrink_many(pgdat, sc);             // 遍历 memcg 回收

    blk_finish_plug(&plug);
}
```

对于非 root 的 memcg 回收（`shrink_lruvec` 中），MGLRU 也提供了专用路径：

```c
static void shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
    if (lru_gen_enabled() && !root_reclaim(sc)) {
        lru_gen_shrink_lruvec(lruvec, sc);  // MGLRU 的 memcg 回收
        return;
    }
    // ... 传统 LRU 路径 ...
}
```

#### 8.10.3 代际管理

MGLRU 通过 `max_seq` 和 `min_seq` 管理代际：

- **max_seq**：递增代表新代际的创建（aging 操作）
- **min_seq**：递增代表最老代际被回收（eviction 操作）
- 可回收的代际范围：`[min_seq, max_seq)`
- 每个代际的页面按类型（anon/file）和 zone 组织在 `folios[gen][type][zone]` 链表中

### 8.11 回收限流机制

#### 8.11.1 限流类型

`include/linux/mmzone.h`：

```c
enum vmscan_throttle_state {
    VMSCAN_THROTTLE_WRITEBACK,   // 太多页面在写回
    VMSCAN_THROTTLE_ISOLATED,    // 太多页面正在被隔离
    VMSCAN_THROTTLE_NOPROGRESS,  // 回收无进展
    VMSCAN_THROTTLE_CONGESTED,   // IO 拥塞
    NR_VMSCAN_THROTTLE,
};
```

#### 8.11.2 限流条件

| 限流类型 | 触发条件 | 触发者 |
|----------|----------|--------|
| WRITEBACK | kswapd 遇到太多立即回收标志的写回页 | kswapd |
| ISOLATED | shrink_inactive_list 中 too_many_isolated() | 直接回收 |
| NOPROGRESS | shrink_node 中 priority=1 且无回收 | 直接回收 |
| CONGESTED | lruvec/node 被标记为拥塞 | 直接回收 |

### 8.12 OOM 触发条件

当直接回收路径 `__alloc_pages_slowpath()` 经过多次重试后仍无法满足分配时，触发 OOM：

```
__alloc_pages_slowpath()
  └─ 循环重试：
       ├─ __alloc_pages_direct_reclaim()  // 直接回收
       ├─ __alloc_pages_direct_compact()   // 直接压缩
       ├─ should_reclaim_retry()           // 检查是否可重试
       │    └─ 如果超过 MAX_RECLAIM_RETRIES(=16) 次无进展 → 中断
       └─ 如果所有重试都失败 → __alloc_pages_may_oom()
            └─ out_of_memory()  // OOM killer
```

### 8.13 完整回收路径总结

```
┌─────────────────────────────────────────────────────────────────┐
│                    页面回收触发路径总图                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  [kswapd 路径]                      [直接回收路径]              │
│  wakeup_kswapd()                    __alloc_pages_slowpath()    │
│       │                                      │                  │
│       ▼                                      ▼                  │
│  kswapd()                          __alloc_pages_direct_reclaim │
│       │                                      │                  │
│       ▼                                      ▼                  │
│  balance_pgdat()                   try_to_free_pages()          │
│       │                                      │                  │
│       └──────────┬───────────────────────────┘                  │
│                  ▼                                              │
│          do_try_to_free_pages()                                 │
│                  │                                              │
│                  ▼                                              │
│            shrink_zones()                                       │
│                  │                                              │
│                  ▼                                              │
│  ┌─ shrink_node() ──────────────────────────────────────┐     │
│  │  ├─ MGLRU 路径 (lru_gen_enabled)                     │     │
│  │  │    └─ lru_gen_shrink_node()                       │     │
│  │  │         ├─ shrink_one() / shrink_many()           │     │
│  │  │         └─ 基于代际回收页面                       │     │
│  │  │                                                    │     │
│  │  └─ 传统 LRU 路径                                    │     │
│  │       ├─ prepare_scan_control()  ← 确定扫描策略      │     │
│  │       └─ shrink_node_memcgs()    ← 遍历 memcg       │     │
│  │            └─ shrink_lruvec()    ← 回收单个 LRU     │     │
│  │                 ├─ get_scan_count()  ← 计算比例      │     │
│  │                 └─ shrink_list()    ← 执行回收      │     │
│  │                      ├─ shrink_active_list()        │     │
│  │                      │    └─ 活跃→非活跃降级        │     │
│  │                      └─ shrink_inactive_list()      │     │
│  │                           ├─ isolate_lru_folios()   │     │
│  │                           └─ shrink_folio_list()    │     │
│  │                                ├─ 匿名页：swap 分配 │     │
│  │                                ├─ 文件页：写回/释放 │     │
│  │                                └─ 释放到 Buddy      │     │
│  └──────────────────────────────────────────────────────┘     │
│                                                                 │
│  [回收后处理]                                                    │
│  ├─ shrink_slab()  ← 收缩 slab 缓存                            │
│  ├─ vmpressure()   ← 计算压力等级                              │
│  ├─ reclaim_throttle() ← 限流                                  │
│  └─ should_reclaim_retry() ← 判断是否 OOM                     │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 8.14 关键函数调用链汇总

| 函数 | 文件 | 行数 | 功能 |
|------|------|------|------|
| `kswapd()` | `mm/vmscan.c` | ~7280 | kswapd 主循环，每个 NUMA 节点一个 |
| `balance_pgdat()` | `mm/vmscan.c` | ~6950 | kswapd 平衡节点内存，优先级循环 |
| `kswapd_age_node()` | `mm/vmscan.c` | ~6702 | kswapd 后台老化，活跃→非活跃降级 |
| `try_to_free_pages()` | `mm/vmscan.c` | ~6566 | 直接回收入口，初始化 scan_control |
| `do_try_to_free_pages()` | `mm/vmscan.c` | ~6344 | 优先级循环，多轮重试 |
| `shrink_zones()` | `mm/vmscan.c` | ~6221 | 遍历 zonelist 回收 |
| `shrink_node()` | `mm/vmscan.c` | ~6039 | 节点级回收核心函数 |
| `shrink_node_memcgs()` | `mm/vmscan.c` | ~5960 | 遍历 memcg 回收 |
| `shrink_lruvec()` | `mm/vmscan.c` | ~5772 | 单个 LRU 向量回收 |
| `get_scan_count()` | `mm/vmscan.c` | ~2527 | 计算匿名/文件页扫描比例 |
| `prepare_scan_control()` | `mm/vmscan.c` | ~2317 | 准备扫描参数和策略 |
| `calculate_pressure_balance()` | `mm/vmscan.c` | ~2421 | 计算回收压力平衡 |
| `shrink_list()` | `mm/vmscan.c` | ~2249 | 根据 LRU 类型分发 |
| `shrink_inactive_list()` | `mm/vmscan.c` | ~1977 | 回收非活跃链表 |
| `shrink_active_list()` | `mm/vmscan.c` | ~2098 | 活跃→非活跃降级 |
| `isolate_lru_folios()` | `mm/vmscan.c` | ~1710 | 从 LRU 隔离页面 |
| `shrink_folio_list()` | `mm/vmscan.c` | ~1083 | 页面级核心回收 |
| `inactive_is_low()` | `mm/vmscan.c` | ~2291 | 检查非活跃页是否太少 |
| `should_continue_reclaim()` | `mm/vmscan.c` | ~5908 | 决定是否继续回收 |
| `shrink_slab()` | `mm/shrinker.c` | ~614 | 收缩 slab 缓存 |
| `vmpressure()` | `mm/vmpressure.c` | ~239 | 计算内存压力等级 |
| `vmpressure_calc_level()` | `mm/vmpressure.c` | ~120 | 根据扫描/回收比计算压力 |
| `lru_note_cost_unlock_irq()` | `mm/swap.c` | ~240 | 更新回收成本 |
| `__alloc_pages_direct_reclaim()` | `mm/page_alloc.c` | ~4437 | 直接回收触发 |
| `should_reclaim_retry()` | `mm/page_alloc.c` | ~4600 | 回收重试判断 |
| `__perform_reclaim()` | `mm/page_alloc.c` | ~4411 | 执行回收，包装 try_to_free_pages |

---

## 9. 交换（Swap）

### 9.1 概述

交换子系统（Swap）是 Linux 虚拟内存管理的核心组成部分，负责将不常用的匿名页从内存换出到块设备（或文件），并在需要时换回内存。它由多个文件共同实现：

| 文件 | 行数 | 功能 |
|------|------|------|
| `mm/swapfile.c` | 3,966 | 交换设备/文件管理，槽位分配/释放，swapon/swapoff |
| `mm/swap_state.c` | 982 | 交换缓存（Swap Cache）管理 |
| `mm/swap.c` | 1,117 | 页面交换核心操作，LRU 批量操作，初始化 |
| `mm/page_io.c` | 464 | 交换 I/O 操作（读写块设备/文件） |
| `include/linux/swap.h` | 639 | 核心数据结构与 API 声明 |
| `include/linux/swapops.h` | 364 | 交换条目编码/解码 |
| `mm/swap_slots.c` | (内联) | 交换槽位缓存（Per-CPU 快速分配） |

**交换子系统整体架构**：

```
内存页（匿名页）
    │
    ├─ 换出路径（页面回收时触发）
    │   shrink_folio_list()
    │     └─ folio_alloc_swap()       ← 分配交换槽位，添加到交换缓存
    │            ├─ swap_alloc_fast()  ← Per-CPU 快速分配
    │            └─ swap_alloc_slow() ← 慢速分配（遍历可用设备）
    │     └─ swap_writeout()          ← 执行换出
    │            ├─ is_folio_zero_filled() → zeromap（零页跳过 I/O）
    │            ├─ zswap_store()     → 压缩存储
    │            └─ __swap_writepage() → 块设备 I/O
    │
    ├─ 换入路径（缺页异常时触发）
    │   do_swap_page()
    │     └─ swapin_readahead()       ← 预读
    │            ├─ swap_vma_readahead()  ← VMA 模式预读
    │            └─ swap_cluster_readahead() ← 簇模式预读
    │     └─ swap_read_folio()        ← 读入单个页面
    │            ├─ swap_read_folio_zeromap() → 零页直接填充
    │            ├─ zswap_load()      → 从压缩缓存加载
    │            └─ 块设备 I/O
    │
    └─ 交换槽位管理
           swapon() / swapoff()       ← 设备生命周期
           swap_info_struct            ← 每设备管理结构
           swap_cluster_info           ← 簇（256页）管理
           swap_map[]                  ← 每槽位使用计数
           zeromap[]                   ← 零页位图
```

### 9.2 核心数据结构

#### 9.2.1 swap_info_struct——交换设备管理

文件：[`include/linux/swap.h`](file:///home/louis/code/linux/include/linux/swap.h)（第 245-303 行）

每个交换设备（或文件）对应一个 `swap_info_struct`，管理设备的所有元数据：

```c
struct swap_info_struct {
    struct percpu_ref users;       /* 引用计数，防止 swapoff 期间被释放 */
    unsigned long   flags;         /* SWP_USED, SWP_WRITEOK 等标志位 */
    signed short    prio;          /* 交换优先级 */
    struct plist_node list;        /* 在 swap_active_head 中的节点 */
    signed char     type;          /* 交换类型索引（0 ~ MAX_SWAPFILES-1） */
    unsigned int    max;           /* swap_map 的总大小（槽位总数） */
    unsigned char  *swap_map;      /* vmalloc'ed 数组，每个槽位一个使用计数 */
    unsigned long  *zeromap;       /* kvmalloc'ed 位图，跟踪零页 */
    struct swap_cluster_info *cluster_info; /* 每个簇的管理结构（仅 SSD） */
    struct list_head free_clusters;     /* 空闲簇链表 */
    struct list_head full_clusters;     /* 已满簇链表 */
    struct list_head nonfull_clusters[SWAP_NR_ORDERS]; /* 部分空闲簇 */
    struct list_head frag_clusters[SWAP_NR_ORDERS];    /* 碎片化簇 */
    unsigned int    pages;         /* 可用页面总数 */
    atomic_long_t   inuse_pages;   /* 当前已使用的页面数 */
    struct swap_sequential_cluster *global_cluster; /* 旋转设备全局簇 */
    spinlock_t      global_cluster_lock; /* 旋转设备串行化锁 */
    struct rb_root  swap_extent_root;   /* 交换 extent 红黑树 */
    struct block_device *bdev;     /* 块设备（或交换文件的 bdev） */
    struct file     *swap_file;    /* 交换文件 */
    struct completion comp;        /* 同步完成量 */
    spinlock_t      lock;          /* 保护 swap_map、inuse_pages、簇链表 */
    spinlock_t      cont_lock;     /* 保护计数延续页链表 */
    struct work_struct discard_work;  /* discard 工作项 */
    struct work_struct reclaim_work;  /* 回收工作项 */
    struct list_head discard_clusters; /* 待 discard 簇链表 */
    struct plist_node avail_list;     /* 在 swap_avail_head 中的节点 */
};
```

**flags 标志位**：

| 标志 | 值 | 含义 |
|------|-----|------|
| `SWP_USED` | 1 << 0 | 槽位被使用 |
| `SWP_WRITEOK` | 1 << 1 | 可写入 |
| `SWP_DISCARDABLE` | 1 << 2 | 块设备支持 discard |
| `SWP_DISCARDING` | 1 << 3 | 正在 discard |
| `SWP_SOLIDSTATE` | 1 << 4 | SSD 设备（寻道廉价） |
| `SWP_CONTINUED` | 1 << 5 | 有计数延续页 |
| `SWP_BLKDEV` | 1 << 6 | 块设备 |
| `SWP_ACTIVATED` | 1 << 7 | swap_activate 成功 |
| `SWP_FS_OPS` | 1 << 8 | 通过文件系统 IO |
| `SWP_STABLE_WRITES` | 1 << 11 | 不允许覆盖 PG_writeback 页 |
| `SWP_SYNCHRONOUS_IO` | 1 << 12 | 同步 IO 高效 |

**swap_map 计数约定**：

| 值 | 含义 |
|-----|------|
| 0 | 空闲槽位 |
| 1 ~ 0x3e (SWAP_MAP_MAX) | 使用计数（共享映射数） |
| 0x3f (SWAP_MAP_BAD) | 坏块标记 |
| 0x80 (COUNT_CONTINUED) | 计数溢出，需要延续页 |

#### 9.2.2 swap_cluster_info——簇管理

文件：[`mm/swap.h`](file:///home/louis/code/linux/mm/swap.h)（第 24-41 行）

交换设备的页面被划分为簇（cluster），每个簇包含 `SWAPFILE_CLUSTER`（256 或 HPAGE_PMD_NR）个页面：

```c
struct swap_cluster_info {
    spinlock_t lock;            /* 保护簇内字段和对应 swap_map 元素 */
    u16 count;                  /* 已使用槽位数 */
    u8  flags;                  /* CLUSTER_FLAG_FREE/FULL/NONFULL/FRAG/DISCARD */
    u8  order;                  /* 簇匹配的分配阶 */
    atomic_long_t __rcu *table; /* 交换表（swap_table），见 mm/swap_table.h */
    struct list_head list;      /* 链接到 free/full/nonfull/frag 链表 */
};
```

**簇状态转换**：

```
CLUSTER_FLAG_FREE ──alloc──→ CLUSTER_FLAG_NONFULL ──full──→ CLUSTER_FLAG_FULL
                        ↑                                      │
                        │           free                        │
                        └──────── partial_free ────────────────┘
                                                                   │
                                                              discard
                                                                   ↓
                                                      CLUSTER_FLAG_DISCARD
                                                           │
                                                      discard完成
                                                           ↓
                                                      CLUSTER_FLAG_FREE
```

**簇链表说明**：

| 链表 | 说明 |
|------|------|
| `free_clusters` | 完全空闲的簇 |
| `full_clusters` | 所有槽位都被占用的簇 |
| `nonfull_clusters[order]` | 部分空闲的簇，按分配阶组织 |
| `frag_clusters[order]` | 碎片化簇（有空洞），按分配阶组织 |
| `discard_clusters` | 等待 discard 操作的簇 |

#### 9.2.3 swp_entry_t——交换条目编码

文件：[`include/linux/swapops.h`](file:///home/louis/code/linux/include/linux/swapops.h)

```c
// 交换条目编码（架构无关格式）
// 高位: type (5 bits), 低位: offset (27 bits on 32-bit)
#define SWP_TYPE_SHIFT  (BITS_PER_XA_VALUE - MAX_SWAPFILES_SHIFT)
#define SWP_OFFSET_MASK ((1UL << SWP_TYPE_SHIFT) - 1)

static inline swp_entry_t swp_entry(unsigned long type, pgoff_t offset)
{
    swp_entry_t ret;
    ret.val = (type << SWP_TYPE_SHIFT) | (offset & SWP_OFFSET_MASK);
    return ret;
}

static inline unsigned swp_type(swp_entry_t entry) {
    return (entry.val >> SWP_TYPE_SHIFT);
}

static inline pgoff_t swp_offset(swp_entry_t entry) {
    return entry.val & SWP_OFFSET_MASK;
}
```

**特殊交换条目类型**：

| 类型 | 用途 |
|------|------|
| `SWP_MIGRATION_READ` / `SWP_MIGRATION_WRITE` | 页面迁移占位 |
| `SWP_MIGRATION_READ_EXCLUSIVE` | 独占的迁移读占位 |
| `SWP_DEVICE_READ` / `SWP_DEVICE_WRITE` | 设备私有内存 |
| `SWP_DEVICE_EXCLUSIVE` | 设备独占访问 |
| `SWP_HWPOISON` | 硬件损坏页 |
| `SWP_PTE_MARKER` | PTE 标记（软脏页、UFFD 等） |

`MAX_SWAPFILES` 计算：`(1 << 5) - SWP_DEVICE_NUM - SWP_MIGRATION_NUM - SWP_HWPOISON_NUM - SWP_PTE_MARKER_NUM`，保留 5 位给 type，最多 32 个交换设备，减去特殊类型后实际可用约 24 个。

#### 9.2.4 swap_extent——块映射

文件：[`include/linux/swap.h`](file:///home/louis/code/linux/include/linux/swap.h)（第 157-163 行）

```c
struct swap_extent {
    struct rb_node rb_node;      /* 红黑树节点 */
    pgoff_t start_page;          /* 起始页号 */
    pgoff_t nr_pages;            /* 页数 */
    sector_t start_block;        /* 起始磁盘块号 */
};
```

Swap Extent 将交换文件的连续页范围映射到磁盘上的连续块范围。对于块设备交换文件，只有一个 extent。对于常规文件，通过 `setup_swap_extents()` 或 `generic_swapfile_activate()` 构建整个 extent 树。

#### 9.2.5 swap_table——交换表

文件：[`mm/swap_table.h`](file:///home/louis/code/linux/mm/swap_table.h)

```c
struct swap_table {
    atomic_long_t entries[SWAPFILE_CLUSTER];  // 每个簇 256 条
};
```

每个簇有一个 `swap_table`，每个条目记录对应槽位的状态：
- **NULL**：空闲
- **folio 指针**：该槽位被交换缓存中的 folio 占用
- **XA_VALUE（shadow）**：阴影条目，用于 workingset 检测

`swap_table` 与 `swap_map` 协同工作：`swap_map` 记录引用计数，`swap_table` 记录缓存 folio 或阴影。

### 9.3 交换设备管理

#### 9.3.1 swapon——启用交换设备

文件：[`mm/swapfile.c`](file:///home/louis/code/linux/mm/swapfile.c)（第 3328 行）

```
SYSCALL_DEFINE2(swapon, specialfile, swap_flags)
  ├─ alloc_swap_info()             ← 分配/初始化 swap_info_struct
  ├─ file_open_name()              ← 打开交换文件/块设备
  ├─ claim_swapfile()              ← 独占声明（阻塞写）
  ├─ read_mapping_folio()          ← 读取交换头（第 0 页）
  ├─ read_swap_header()            ← 解析交换头（magic/version/badpages）
  ├─ setup_swap_extents()          ← 建立 extent 映射树
  │    ├─ 块设备: add_swap_extent(sis, 0, sis->max, 0)
  │    └─ 文件:   generic_swapfile_activate() 或 swap_activate
  ├─ vzalloc(maxpages)             ← 分配 swap_map 数组
  ├─ kvmalloc(zeromap)             ← 分配零页位图
  ├─ setup_clusters()              ← 初始化簇管理结构
  ├─ 设置 flags: 
  │    SWP_SOLIDSTATE (SSD) / SWP_SYNCHRONOUS_IO / SWP_STABLE_WRITES
  ├─ discard 处理
  ├─ zswap_swapon()                ← 通知 zswap
  ├─ inode->i_flags |= S_SWAPFILE  ← 标记为交换文件
  └─ enable_swap_info()            ← 加入全局列表
       ├─ 设置优先级
       ├─ atomic_long_add(nr_swap_pages)
       ├─ plist_add(&si->list, &swap_active_head)
       └─ add_to_avail_list()      ← 加入可用设备列表
```

**交换头格式**：

```c
union swap_header {
    struct {
        char reserved[PAGE_SIZE - 10];
        char magic[10];           /* "SWAP-SPACE" 或 "SWAPSPACE2" */
    } magic;
    struct {
        char   bootbits[1024];
        __u32  version;
        __u32  last_page;
        __u32  nr_badpages;
        unsigned char sws_uuid[16];
        unsigned char sws_volume[16];
        __u32  padding[117];
        __u32  badpages[];        /* 坏块列表 */
    } info;
};
```

#### 9.3.2 swapoff——停用交换设备

文件：[`mm/swapfile.c`](file:///home/louis/code/linux/mm/swapfile.c)（第 2767 行）

```
SYSCALL_DEFINE1(swapoff, specialfile)
  ├─ 从 swap_active_head 找到目标设备
  ├─ del_from_avail_list(p, true)  ← 从可用列表移除
  ├─ plist_del(&p->list, &swap_active_head)
  ├─ atomic_long_sub(p->pages, &nr_swap_pages)
  ├─ wait_for_allocation(p)        ← 等待正在进行的分配完成
  ├─ set_current_oom_origin()
  ├─ try_to_unuse(p->type)         ← 将所有换出页换回内存
  │    └─ 遍历所有 mm 的 VMA
  │         └─ unuse_pte_range()   ← 逐 PTE 解除交换映射
  │              ├─ swap_cache_get_folio() → 找到或读入 folio
  │              └─ unuse_pte()    ← 建立页表映射
  ├─ clear_current_oom_origin()
  ├─ percpu_ref_kill() + synchronize_rcu()  ← 等待 RCU 读者
  ├─ 清理资源:
  │    destroy_swap_extents()
  │    free_swap_count_continuations()
  │    vfree(swap_map) / kvfree(zeromap)
  │    free_cluster_info()
  │    swap_cgroup_swapoff()
  └─ inode->i_flags &= ~S_SWAPFILE
```

#### 9.3.3 设备列表管理

交换设备通过两个优先级列表管理：

```c
// 所有活跃设备（按优先级排序）
static PLIST_HEAD(swap_active_head);
// 有空闲空间的活跃设备（按优先级排序）
static PLIST_HEAD(swap_avail_head);
static DEFINE_SPINLOCK(swap_avail_lock);
```

- `swap_active_head`：包含所有 `SWP_WRITEOK` 的设备，用于 `swapoff` 查找。
- `swap_avail_head`：仅包含有可用槽位的设备，使用 `SWAP_USAGE_OFFLIST_BIT` 嵌入在 `inuse_pages` 原子计数中判断设备是否在列表中。

**`SWAP_USAGE_OFFLIST_BIT` 原子管理**：利用 `inuse_pages` 的次高位作为"不在可用列表"标记，使设备满时无需加锁即可从列表移除，提升分配路径性能。

### 9.4 交换槽位分配与释放

交换槽位分配是 Swap 子系统的核心路径，采用三级分配策略：Per-CPU 快速分配 → 设备级慢速分配 → 跨设备轮转。

#### 9.4.1 簇分配核心函数

文件：[`mm/swapfile.c`](file:///home/louis/code/linux/mm/swapfile.c)（第 1041 行）

```c
static unsigned long cluster_alloc_swap_entry(struct swap_info_struct *si,
                                              struct folio *folio)
{
    // 1. 旋转设备（HDD）使用全局簇指针
    if (!(si->flags & SWP_SOLIDSTATE)) {
        spin_lock(&si->global_cluster_lock);
        offset = si->global_cluster->next[order];
        found = alloc_swap_scan_cluster(si, ci, folio, offset);
        goto done;
    }

    // 2. SSD 设备按优先级搜索簇链表
    //    a) 支持 discard 时优先用空闲簇（减少磨损）
    //    b) 搜索 nonfull_clusters[order]
    //    c) 搜索 free_clusters
    //    d) 回收已满簇中的缓存页
    //    e) 搜索 frag_clusters（碎片化簇）
    //    f) 从高阶簇中"窃取" order-0 槽位
}
```

**簇内扫描**（`alloc_swap_scan_cluster`）：

```
alloc_swap_scan_cluster(si, ci, folio, offset):
  ├─ 检查簇是否可用（cluster_is_usable）
  ├─ 在簇内逐槽位扫描（已对齐到分配阶）
  │    ├─ cluster_scan_range()    ← 检查槽位是否空闲
  │    └─ cluster_alloc_range()   ← 分配并设置 swap_map/swap_cache
  └─ relocate_cluster()           ← 根据使用量重新分类簇
       ├─ count == 0        → free_clusters
       ├─ count < CLUSTER   → nonfull/frag_clusters
       └─ count == CLUSTER  → full_clusters
```

#### 9.4.2 Per-CPU 快速分配

文件：[`mm/swapfile.c`](file:///home/louis/code/linux/mm/swapfile.c)（第 1318 行）

```c
static DEFINE_PER_CPU(struct percpu_swap_cluster, percpu_swap_cluster) = {
    .si = { NULL },
    .offset = { SWAP_ENTRY_INVALID },
};

struct percpu_swap_cluster {
    struct swap_info_struct *si[SWAP_NR_ORDERS];
    unsigned long offset[SWAP_NR_ORDERS];
    local_lock_t lock;
};

static bool swap_alloc_fast(struct folio *folio)
{
    // 1. 从 Per-CPU 缓存读取设备指针和偏移量
    si = this_cpu_read(percpu_swap_cluster.si[order]);
    offset = this_cpu_read(percpu_swap_cluster.offset[order]);

    // 2. 尝试从缓存的簇中分配
    ci = swap_cluster_lock(si, offset);
    if (cluster_is_usable(ci, order))
        alloc_swap_scan_cluster(si, ci, folio, offset);

    return folio_test_swapcache(folio);
}
```

**Per-CPU 缓存更新时机**：每次 `alloc_swap_scan_cluster()` 成功分配后，如果簇内还有剩余空间，将下一个可用偏移量写回 Per-CPU 变量，下次分配可直接继续。

#### 9.4.3 慢速分配

文件：[`mm/swapfile.c`](file:///home/louis/code/linux/mm/swapfile.c)（第 1346 行）

当 Per-CPU 快速分配失败时，进入慢速路径：

```c
static void swap_alloc_slow(struct folio *folio)
{
    spin_lock(&swap_avail_lock);
    plist_for_each_entry_safe(si, next, &swap_avail_head, avail_list) {
        plist_requeue(&si->avail_list, &swap_avail_head);  // 轮转设备
        spin_unlock(&swap_avail_lock);
        if (get_swap_device_info(si)) {
            cluster_alloc_swap_entry(si, folio);  // 尝试分配
            put_swap_device(si);
            if (folio_test_swapcache(folio))
                return;  // 分配成功
        }
        // 继续下一个设备...
    }
}
```

#### 9.4.4 folio_alloc_swap——换出入口

文件：[`mm/swapfile.c`](file:///home/louis/code/linux/mm/swapfile.c)（第 1482 行）

```c
int folio_alloc_swap(struct folio *folio)
{
    // 1. 检查大页支持（CONFIG_THP_SWAP）
    // 2. 快速分配（Per-CPU）
    local_lock(&percpu_swap_cluster.lock);
    if (!swap_alloc_fast(folio))
        swap_alloc_slow(folio);
    local_unlock(&percpu_swap_cluster.lock);

    // 3. 分配失败重试（同步 discard）
    if (!order && unlikely(!folio_test_swapcache(folio)))
        if (swap_sync_discard())
            goto again;

    // 4. memcg 交换记账
    if (mem_cgroup_try_charge_swap(folio, folio->swap))
        swap_cache_del_folio(folio);  // 记账失败则回滚

    return folio_test_swapcache(folio) ? 0 : -ENOMEM;
}
```

**分配结果**：成功时，folio 的 `->swap` 字段被设置为 `swp_entry_t`（type + offset），并添加到交换缓存（swap cache）。

#### 9.4.5 folio_dup_swap / folio_put_swap——引用计数管理

文件：[`mm/swapfile.c`](file:///home/louis/code/linux/mm/swapfile.c)（第 1559-1649 行）

```c
// 增加引用计数（在 unmap 建立页表交换条目时调用）
int folio_dup_swap(struct folio *folio, struct page *subpage)
{
    // 对每个子页：si->swap_map[offset]++
    // 如果计数器溢出（>= SWAP_MAP_MAX），使用 COUNT_CONTINUED 延续页
}

// 减少引用计数（在页表条目被替换时调用）
void folio_put_swap(struct folio *folio, struct page *subpage)
{
    // 对每个子页：si->swap_map[offset]--
    // 如果 count 降为 0 且不在缓存中，则释放槽位
}
```

**引用计数状态机**：

```
swap_map[offset] == 0: 空闲槽位
    ↑ swap_cache_del       ↓ folio_alloc_swap (分配并加入缓存)
swap_map[offset] == 0: 缓存独占（folio 在 swap cache 中）
    ↑ folio_put_swap       ↓ folio_dup_swap (建立页表映射)
swap_map[offset] == N: 有 N 个页表映射共享此槽位
    ↑ folio_put_swap       ↓ folio_dup_swap
swap_map[offset] == SWAP_MAP_MAX: 溢出，使用延续页
```

#### 9.4.6 folio_free_swap——释放交换槽位

文件：[`mm/swapfile.c`](file:///home/louis/code/linux/mm/swapfile.c)（第 1868 行）

```c
bool folio_free_swap(struct folio *folio)
{
    if (!folio_swapcache_freeable(folio))  // 检查是否可释放
        return false;
    if (folio_swapped(folio))              // 还有页表引用
        return false;

    swap_cache_del_folio(folio);           // 从交换缓存删除
    folio_set_dirty(folio);                // 标记脏（保留数据）
    return true;
}
```

### 9.5 交换缓存（Swap Cache）

#### 9.5.1 数据结构

文件：[`mm/swap_state.c`](file:///home/louis/code/linux/mm/swap_state.c)

交换缓存使用全局 `address_space` 结构：

```c
struct address_space swap_space __read_mostly = {
    .a_ops = &swap_aops,
};

static const struct address_space_operations swap_aops = {
    .dirty_folio    = noop_dirty_folio,
    .migrate_folio  = migrate_folio,
};
```

每个簇的 `swap_table` 记录槽位对应的 folio 指针，替代了传统的 `swapper_spaces` XArray 管理。

#### 9.5.2 核心操作

```c
// 查找：通过 swap entry 找到缓存的 folio
struct folio *swap_cache_get_folio(swp_entry_t entry)
{
    // 1. 通过簇表找到 swp_tb（folio 指针或 shadow）
    // 2. folio_try_get() 增加引用计数
    // 3. 返回 folio（可能为 NULL）
}

// 添加：将 folio 加入交换缓存
static int swap_cache_add_folio(struct folio *folio, swp_entry_t entry, void **shadowp)
{
    // 1. 锁定簇，检查 swap_map 计数
    // 2. 检查 slot 是否已被其他 folio 占用
    // 3. __swap_cache_add_folio() 写入 swap_table
    // 4. 返回 shadow（用于 workingset 检测）
}

// 删除：从交换缓存移除
void swap_cache_del_folio(struct folio *folio)
{
    // 1. 写入 shadow 到 swap_table
    // 2. 清除 folio 的 swapbacked 和 swapcache 标志
    // 3. 释放 swap 槽位（swap_entries_free）
}
```

**交换缓存的作用**：
1. **防重复换入**：同一页被多个进程同时缺页时，只需读一次
2. **写回合并**：页在 swap cache 中，多个进程的修改可以合并
3. **workingset 检测**：shadow 条目记录页面最近被驱逐的历史

### 9.6 换出路径

#### 9.6.1 触发时机

换出由页面回收路径触发，核心调用链：

```
shrink_folio_list()
  └─ 处理匿名页:
       if (folio_test_anon(folio) && folio_test_swapbacked(folio)) {
           if (folio_alloc_swap(folio))    ← 分配交换槽位
               goto activate_locked;       ← 分配失败，跳过
           folio_mark_dirty(folio);          ← 标记脏（写回前需脏）
       }
  └─ 最终调用 swap_writeout() 执行写回
```

#### 9.6.2 swap_writeout——写回决策

文件：[`mm/page_io.c`](file:///home/louis/code/linux/mm/page_io.c)（第 249 行）

```c
int swap_writeout(struct folio *folio, struct swap_iocb **swap_plug)
{
    // 1. 尝试释放交换缓存（如果页已被重新映射到内存）
    if (folio_free_swap(folio))
        goto out_unlock;  // 无需写回

    // 2. 架构特定的写前准备
    ret = arch_prepare_to_swap(folio);

    // 3. 零页检测（zeromap）：全零页无需实际 I/O
    if (is_folio_zero_filled(folio)) {
        swap_zeromap_folio_set(folio);  // 标记 zeromap 位图
        goto out_unlock;                // 跳过 I/O
    }

    // 4. 清除 zeromap（防止旧数据读取）
    swap_zeromap_folio_clear(folio);

    // 5. 尝试 zswap 压缩存储
    if (zswap_store(folio))
        goto out_unlock;

    // 6. 实际 I/O 写回
    __swap_writepage(folio, swap_plug);
    return 0;
}
```

#### 9.6.3 zeromap 零页优化

文件：[`mm/page_io.c`](file:///home/louis/code/linux/mm/page_io.c)（第 182-245 行）

```c
// 零页检测：逐字检查页面内容
static bool is_folio_zero_filled(struct folio *folio)
{
    // 检查每页的最后一个字（快速失败路径）
    // 如果最后字为零，再逐字确认
    // 对透明大页，检查所有子页
}

// 设置 zeromap 位图
static void swap_zeromap_folio_set(struct folio *folio)
{
    for (i = 0; i < folio_nr_pages(folio); i++)
        set_bit(swp_offset(entry), sis->zeromap);
    count_vm_events(SWPOUT_ZERO, nr_pages);  // 统计
}

// 清除 zeromap 位图
static void swap_zeromap_folio_clear(struct folio *folio)
{
    for (i = 0; i < folio_nr_pages(folio); i++)
        clear_bit(swp_offset(entry), sis->zeromap);
}
```

**zeromap 意义**：大量匿名页初始化为零（如 `calloc`），换出这些零页无需实际 I/O，换入时直接 `folio_zero_range()` 填充，显著减少交换 I/O 量。

#### 9.6.4 zswap 压缩存储

`zswap` 是内存压缩交换缓存，在写回路径中先尝试压缩：

```
swap_writeout()
  └─ zswap_store(folio)
       ├─ 在内存中压缩 folio 数据
       ├─ 存储到 zswap 树（zswap_entry）
       └─ 返回 true 表示压缩成功，跳过磁盘 I/O
```

zswap 与 zeromap 协同工作：zeromap 处理零页（压缩比为无穷大），zswap 处理非零页（通常 2x-3x 压缩比）。

#### 9.6.5 __swap_writepage——I/O 路径

文件：[`mm/page_io.c`](file:///home/louis/code/linux/mm/page_io.c)（第 447 行）

```c
void __swap_writepage(struct folio *folio, struct swap_iocb **swap_plug)
{
    struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);

    if (sis->flags & SWP_FS_OPS)
        swap_writepage_fs(folio, swap_plug);       // 文件系统 I/O
    else if (sis->flags & SWP_SYNCHRONOUS_IO)
        swap_writepage_bdev_sync(folio, sis);      // 同步块设备 I/O
    else
        swap_writepage_bdev_async(folio, sis);     // 异步块设备 I/O
}
```

**三种 I/O 路径**：

| 路径 | 适用场景 | 说明 |
|------|----------|------|
| `swap_writepage_fs` | 交换文件（如 btrfs） | 通过 `swap_rw` 文件操作，支持 plug 批量合并 |
| `swap_writepage_bdev_sync` | 同步块设备（如 zram） | `submit_bio_wait()` 同步等待 |
| `swap_writepage_bdev_async` | 异步块设备（SSD/HDD） | `submit_bio()` 异步，回调 `end_swap_bio_write` |

**I/O 完成处理**：

```c
static void __end_swap_bio_write(struct bio *bio)
{
    struct folio *folio = bio_first_folio_all(bio);
    if (bio->bi_status) {
        folio_mark_dirty(folio);           // 写失败：重新脏化
        folio_clear_reclaim(folio);        // 清除回收标志
        pr_alert("Write-error on swap-device\n");
    }
    folio_end_writeback(folio);            // 结束写回
}
```

### 9.7 换入路径

#### 9.7.1 do_swap_page——缺页入口

文件：[`mm/memory.c`](file:///home/louis/code/linux/mm/memory.c)（第 4706 行）

```
do_swap_page(vmf)
  ├─ get_swap_device(entry)          ← 防止 swapoff
  ├─ swap_cache_get_folio(entry)     ← 查找交换缓存
  │
  ├─ [缓存未命中] 读入页面:
  │    ├─ [同步设备] alloc_swap_folio + swapin_folio
  │    │    ├─ swap_cache_alloc_folio()  → 分配 folio
  │    │    └─ swapin_folio()            → 加入缓存 + 读入
  │    └─ [异步设备] swapin_readahead()
  │         ├─ swap_vma_readahead()      → VMA 模式预读
  │         └─ swap_cluster_readahead()  → 簇模式预读
  │
  ├─ [缓存命中] 直接使用缓存的 folio
  │
  ├─ folio_lock_or_retry()           ← 锁定 folio
  ├─ 验证 folio 仍匹配 swap entry
  │
  ├─ KSM 拷贝处理（ksm_might_need_to_copy）
  │
  ├─ 建立页表映射:
  │    └─ set_pte_at()               ← 设置 PTE
  │    └─ folio_add_anon_rmap_pte()  ← 反向映射
  │    └─ folio_put_swap()           ← 减少 swap 引用计数
  │
  └─ 返回 VM_FAULT_MAJOR / VM_FAULT_MINOR
```

**do_swap_page 的 PTE 验证**：在建立页表映射前，必须重新获取 PTE 并与 `vmf->orig_pte` 比较，确保在等待 I/O 期间 PTE 未被其他线程修改（防止 race with swapoff 或并发缺页）。

#### 9.7.2 swap_read_folio——I/O 路径

文件：[`mm/page_io.c`](file:///home/louis/code/linux/mm/page_io.c)（第 609 行）

```c
void swap_read_folio(struct folio *folio, struct swap_iocb **plug)
{
    // 1. zeromap 零页直接填充
    if (swap_read_folio_zeromap(folio)) {
        // folio_zero_range() + folio_mark_uptodate() + folio_unlock()
        goto finish;
    }

    // 2. zswap 压缩加载
    if (zswap_load(folio) != -ENOENT)
        goto finish;  // 从压缩缓存加载成功

    // 3. 实际磁盘 I/O
    if (sis->flags & SWP_FS_OPS)
        swap_read_folio_fs(folio, plug);       // 文件系统
    else if (synchronous)
        swap_read_folio_bdev_sync(folio, sis); // 同步
    else
        swap_read_folio_bdev_async(folio, sis);// 异步

finish:
    // 统计 thrashing 延迟
}
```

#### 9.7.3 zeromap 零页换入

```c
static bool swap_read_folio_zeromap(struct folio *folio)
{
    // 检查 zeromap 位图
    if (swap_zeromap_batch(folio->swap, nr_pages, &is_zeromap) == nr_pages && is_zeromap) {
        folio_zero_range(folio, 0, folio_size(folio));  // 直接填零
        folio_mark_uptodate(folio);                     // 标记完成
        folio_unlock(folio);
        return true;
    }
    return false;
}
```

### 9.8 交换预读

文件：[`mm/swap_state.c`](file:///home/louis/code/linux/mm/swap_state.c)（第 913 行）

```c
struct folio *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask, struct vm_fault *vmf)
{
    folio = swap_use_vma_readahead() ?
        swap_vma_readahead(entry, gfp_mask, mpol, ilx, vmf) :  // VMA 模式
        swap_cluster_readahead(entry, gfp_mask, mpol, ilx);     // 簇模式
    return folio;
}
```

#### 9.8.1 簇预读（swap_cluster_readahead）

```
swap_cluster_readahead(entry, gfp_mask, mpol, ilx)
  ├─ swapin_nr_pages(offset)         ← 计算预读窗口大小
  │    ├─ 基于历史命中次数动态调整（hits + 2，指数增长）
  │    └─ 受 page_cluster 限制（默认 3，即 8 页）
  ├─ 计算起始偏移（对齐到 page_cluster 边界）
  ├─ 循环预读相邻页面:
  │    └─ swap_cache_alloc_folio() + swap_read_folio()
  └─ 返回目标页面 folio
```

#### 9.8.2 VMA 预读（swap_vma_readahead）

VMA 模式利用进程的虚拟地址空间局部性，比簇模式更精确：

```
swap_vma_readahead(entry, gfp_mask, mpol, ilx, vmf)
  ├─ swap_vma_ra_win()               ← 计算预读窗口
  │    ├─ 从 vma->swap_readahead_info 读取历史信息
  │    │    ├─ prev_faddr: 上次缺页地址
  │    │    ├─ prev_win: 上次窗口大小
  │    │    └─ hits: 预读命中次数
  │    └─ __swapin_nr_pages() 动态调整窗口
  ├─ 在 VMA 中查找相邻页面（向前/向后）
  └─ 批量读入
```

**预读窗口调整**：通过 `vma->swap_readahead_info` 编码地址、窗口大小和命中次数（3 个字段压缩到一个 long 中），自适应调整预读窗口。

### 9.9 交换槽位回收

#### 9.9.1 __try_to_reclaim_swap——回收缓存 folio

文件：[`mm/swapfile.c`](file:///home/louis/code/linux/mm/swapfile.c)（第 202 行）

```c
static int __try_to_reclaim_swap(struct swap_info_struct *si,
                                 unsigned long offset, unsigned long flags)
{
    // 1. 查找 swap cache 中的 folio
    folio = swap_cache_get_folio(entry);

    // 2. 尝试锁定 folio
    if (!folio_trylock(folio))
        goto out;

    // 3. 判断是否需要回收
    need_reclaim = (flags & TTRS_ANYWAY) ||
                   ((flags & TTRS_UNMAPPED) && !folio_mapped(folio)) ||
                   ((flags & TTRS_FULL) && mem_cgroup_swap_full(folio));

    // 4. 回收：从 swap cache 删除，脏化 folio
    swap_cache_del_folio(folio);
    folio_set_dirty(folio);
    return nr_pages;  // 返回回收的页数
}
```

**触发标志**：

| 标志 | 含义 |
|------|------|
| `TTRS_ANYWAY` | 无条件回收 |
| `TTRS_UNMAPPED` | 仅当页面未被映射时回收 |
| `TTRS_FULL` | 交换设备快满时回收 |

#### 9.9.2 swap_reclaim_work——后台回收工作项

当交换设备使用率超过 50%（`vm_swap_full()`），`swap_range_alloc()` 会调度 `reclaim_work`：

```c
static void swap_range_alloc(struct swap_info_struct *si, unsigned int nr_entries)
{
    if (swap_usage_add(si, nr_entries)) {
        if (vm_swap_full())          // nr_swap_pages * 2 < total_swap_pages
            schedule_work(&si->reclaim_work);  // 调度回收工作项
    }
}
```

`swap_reclaim_work` 回收已满簇中的 swap cache 页面，释放槽位。

### 9.10 统计信息与接口

**内核统计**：

| 计数器 | 文件 | 含义 |
|--------|------|------|
| `nr_swap_pages` | `swapfile.c` | 全局空闲交换槽位数 |
| `total_swap_pages` | `swapfile.c` | 全局交换总页数 |
| `NR_SWAPCACHE` | `swap_state.c` | 交换缓存中的页数 |
| `PSWPIN` | `page_io.c` | 换入页数 |
| `PSWPOUT` | `page_io.c` | 换出页数 |
| `SWAP_RA` | `swap_state.c` | 预读页数 |
| `SWPIN_ZERO` | `page_io.c` | 零页换入 |
| `SWPOUT_ZERO` | `page_io.c` | 零页换出 |
| `nr_rotate_swap` | `swapfile.c` | 旋转设备数 |

**用户空间接口**：

| 接口 | 功能 |
|------|------|
| `swapon()` | 启用交换设备/文件 |
| `swapoff()` | 停用交换设备 |
| `si_swapinfo()` | 获取交换统计信息 |
| `/proc/swaps` | 交换设备状态 |
| `/proc/meminfo` | SwapTotal / SwapFree |
| `/sys/kernel/mm/swap/` | vma_ra_enabled 等可调参数 |

### 9.11 关键函数调用链汇总

**换出路径**：

```
shrink_folio_list() [mm/vmscan.c]
  └─ folio_alloc_swap() [mm/swapfile.c:1482]
       ├─ swap_alloc_fast() [mm/swapfile.c:1318]    ← Per-CPU 快速分配
       │    └─ alloc_swap_scan_cluster() [mm/swapfile.c:892]
       │         └─ cluster_alloc_range() [mm/swapfile.c:870]
       │              ├─ __swap_cache_add_folio()    ← 加入交换缓存
       │              └─ swap_range_alloc() [mm/swapfile.c:1249]
       │                   └─ schedule_work(&reclaim_work)  ← 满时调度回收
       └─ swap_alloc_slow() [mm/swapfile.c:1346]    ← 慢速分配
            └─ cluster_alloc_swap_entry() [mm/swapfile.c:1041]
                 ├─ alloc_swap_scan_list(&nonfull_clusters)
                 ├─ alloc_swap_scan_list(&free_clusters)
                 └─ alloc_swap_scan_list(&frag_clusters)
  └─ swap_writeout() [mm/page_io.c:249]
       ├─ is_folio_zero_filled() → swap_zeromap_folio_set()  ← 零页跳过 I/O
       ├─ zswap_store()                                    ← 压缩存储
       └─ __swap_writepage() [mm/page_io.c:447]
            ├─ swap_writepage_fs()     ← 文件系统 I/O
            ├─ swap_writepage_bdev_sync()  ← 同步块设备 I/O
            └─ swap_writepage_bdev_async() ← 异步块设备 I/O
```

**换入路径**：

```
do_swap_page() [mm/memory.c:4706]
  ├─ swap_cache_get_folio() [mm/swap_state.c]         ← 查找缓存
  ├─ swapin_readahead() [mm/swap_state.c:913]
  │    ├─ swap_vma_readahead() [mm/swap_state.c]      ← VMA 模式预读
  │    │    ├─ swap_vma_ra_win()                      ← 计算窗口
  │    │    └─ swap_cache_alloc_folio() + swap_read_folio()
  │    └─ swap_cluster_readahead() [mm/swap_state.c]  ← 簇模式预读
  │         ├─ swapin_nr_pages()                      ← 计算窗口
  │         └─ 循环预读相邻页面
  └─ swap_read_folio() [mm/page_io.c:609]
       ├─ swap_read_folio_zeromap()    ← 零页直接填充
       ├─ zswap_load()                 ← 从压缩缓存加载
       ├─ swap_read_folio_fs()         ← 文件系统 I/O
       ├─ swap_read_folio_bdev_sync()  ← 同步块设备 I/O
       └─ swap_read_folio_bdev_async() ← 异步块设备 I/O
```

**设备管理路径**：

```
swapon() [mm/swapfile.c:3328]
  ├─ alloc_swap_info()
  ├─ read_swap_header() → 解析交换头
  ├─ setup_swap_extents() → 建立 extent 树
  ├─ vzalloc(swap_map) + kvmalloc(zeromap)
  ├─ setup_clusters() → 初始化簇管理
  └─ enable_swap_info() → 加入全局列表

swapoff() [mm/swapfile.c:2767]
  ├─ del_from_avail_list() + plist_del()
  ├─ wait_for_allocation()
  ├─ try_to_unuse() → 换回所有页面
  │    ├─ unuse_pte_range()
  │    │    ├─ swap_cache_get_folio() / swapin_readahead()
  │    │    └─ unuse_pte() → 建立页表映射
  │    └─ folio_free_swap() → 释放槽位
  ├─ synchronize_rcu() + wait_for_completion()
  └─ 清理资源
```

## 10. 压缩与页面迁移

### 10.1 内存压缩（Compaction）

文件：`mm/compaction.c`（3,334 行），`include/linux/compaction.h`

#### 10.1.1 概述

内存压缩（Compaction）通过移动页面来创建连续物理内存区域，为高阶分配（如 THP 分配）服务。核心思想是使用**两个扫描器**（Two-Scanner Approach）在两个方向同时扫描 zone：

- **迁移扫描器（Migration Scanner）**：从 zone 低地址向高地址扫描，查找可移动的页面
- **空闲扫描器（Free Scanner）**：从 zone 高地址向低地址扫描，查找空闲页面

两个扫描器相向而行，迁移扫描器找到的已分配页面被移动到空闲扫描器找到的空闲位置，最终在扫描器交汇处形成一个连续的物理内存区域。

#### 10.1.2 核心数据结构

**`struct compact_control`**（`mm/internal.h`）— 压缩控制结构，管理一次压缩运行的所有状态：

```c
struct compact_control {
    struct list_head freepages[NR_PAGE_ORDERS]; /* 隔离的空闲页面链表（按 order 分组） */
    struct list_head migratepages;              /* 待迁移的页面链表 */
    unsigned int nr_freepages;                  /* 已隔离空闲页面计数 */
    unsigned int nr_migratepages;               /* 待迁移页面计数 */
    unsigned long free_pfn;                     /* 空闲扫描器当前 PFN */
    unsigned long migrate_pfn;                  /* 迁移扫描器当前 PFN（in/out 参数） */
    unsigned long fast_start_pfn;               /* 快速查找起始 PFN */
    struct zone *zone;                          /* 目标 zone */
    unsigned long total_migrate_scanned;        /* 总计扫描的迁移页面数 */
    unsigned long total_free_scanned;           /* 总计扫描的空闲页面数 */
    unsigned short fast_search_fail;            /* 快速查找连续失败次数 */
    short search_order;                         /* 快速查找起始 order */
    const gfp_t gfp_mask;                       /* 直接压缩的 GFP 掩码 */
    int order;                                  /* 申请 order */
    int migratetype;                            /* 迁移类型 */
    const unsigned int alloc_flags;             /* 分配标志 */
    const int highest_zoneidx;                  /* 最高 zone 索引 */
    enum migrate_mode mode;                     /* 同步/异步迁移模式 */
    bool ignore_skip_hint;                      /* 忽略跳过的 pageblock */
    bool no_set_skip_hint;                      /* 不标记跳过 pageblock */
    bool ignore_block_suitable;                 /* 扫描不合适的 block */
    bool direct_compaction;                     /* 来自直接压缩（非 kcompactd） */
    bool proactive_compaction;                  /* 来自主动压缩 */
    bool whole_zone;                            /* 扫描整个 zone */
    bool contended;                             /* 锁竞争标志 */
    bool finish_pageblock;                      /* 完成当前 pageblock 扫描 */
    bool alloc_contig;                          /* 来自 alloc_contig_range */
};
```

**`struct capture_control`**（`mm/internal.h`）— 捕获控制，用于直接压缩时捕获刚释放的页面：

```c
struct capture_control {
    struct compact_control *cc;  /* 关联的压缩控制结构 */
    struct page *page;           /* 捕获的页面 */
};
```

#### 10.1.3 压缩优先级与返回值

**压缩优先级**（`enum compact_priority`）从高到低：

| 优先级 | 值 | 说明 |
|--------|-----|------|
| `COMPACT_PRIO_SYNC_FULL` | 0 | 最高优先级，同步完整扫描，可用于昂贵操作 |
| `COMPACT_PRIO_SYNC_LIGHT` | 1 | 轻量同步，可等待锁但不等待 I/O |
| `COMPACT_PRIO_ASYNC` | 2 | 异步模式，不阻塞，快速失败 |

**压缩结果**（`enum compact_result`）：

| 返回值 | 说明 |
|--------|------|
| `COMPACT_NOT_SUITABLE_ZONE` | zone 不适合压缩 |
| `COMPACT_SKIPPED` | 压缩被跳过 |
| `COMPACT_DEFERRED` | 压缩被延迟（之前失败） |
| `COMPACT_NO_SUITABLE_PAGE` | 没有合适的页面 |
| `COMPACT_CONTINUE` | 需要继续压缩 |
| `COMPACT_COMPLETE` | 整个 zone 扫描完成 |
| `COMPACT_PARTIAL_SKIPPED` | 部分扫描完成 |
| `COMPACT_CONTENDED` | 因锁竞争终止 |
| `COMPACT_SUCCESS` | 压缩成功，分配应可满足 |

#### 10.1.4 compact_zone — 核心压缩主循环

`compact_zone()` 是压缩的核心函数，实现两个扫描器（迁移和空闲）的协调运行：

```c
static enum compact_result compact_zone(struct compact_control *cc,
                                        struct capture_control *capc)
{
    // 1. 初始化：设置扫描起始 PFN、缓存 PFN、whole_zone 标志
    //    迁移扫描器从 zone_start_pfn 或缓存位置开始
    //    空闲扫描器从 zone_end_pfn 开始
    if (!cc->migrate_pfn) {
        cc->migrate_pfn = cc->zone->zone_start_pfn;
        ...
    }
    if (cc->migrate_pfn <= cc->zone->compact_init_migrate_pfn)
        cc->whole_zone = true;

    // 2. 主循环：迁移和空闲扫描器交替工作
    while ((ret = compact_finished(cc)) == COMPACT_CONTINUE) {
        // 2a. 检查是否需要完成当前 pageblock 的重扫
        //     当上次迁移的 pageblock 与当前扫描器所在的 pageblock 相同时
        //     设置 finish_pageblock=true，强迫完成整个 pageblock 的扫描
        cc->finish_pageblock = false;
        if (pageblock_start_pfn(last_migrated_pfn) ==
            pageblock_start_pfn(iteration_start_pfn))
            cc->finish_pageblock = true;

    rescan:
        // 2b. 隔离可迁移页面
        switch (isolate_migratepages(cc)) {
        case ISOLATE_ABORT:  // 锁竞争，终止
            ret = COMPACT_CONTENDED;
            putback_movable_pages(&cc->migratepages);
            goto out;
        case ISOLATE_NONE:   // 没有可迁移页面
            goto check_drain;  // 检查是否需要 drain pcplists
        case ISOLATE_SUCCESS: // 成功隔离
            update_cached = false;
            last_migrated_pfn = ...;
        }

        // 2c. 执行迁移
        nr_migratepages = cc->nr_migratepages;
        err = migrate_pages(&cc->migratepages, compaction_alloc,
                compaction_free, (unsigned long)cc, cc->mode,
                MR_COMPACTION, &nr_succeeded);

        // 2d. 处理迁移失败
        if (err) {
            putback_movable_pages(&cc->migratepages);
            // 内存不足时检查扫描器是否已交汇
            if (err == -ENOMEM && !compact_scanners_met(cc)) {
                ret = COMPACT_CONTENDED;
                goto out;
            }
            // 非同步模式下，如果在 pageblock 中间失败且未设置 ignore_skip_hint
            // 则重扫当前 pageblock 的剩余部分，将其标记为 skip
            if (!pageblock_aligned(cc->migrate_pfn) &&
                !cc->ignore_skip_hint && !cc->finish_pageblock &&
                (cc->mode < MIGRATE_SYNC)) {
                cc->finish_pageblock = true;
                goto rescan;  // 重扫当前 pageblock
            }
        }
    }

    // 3. 释放捕获的页面（capture_control 用于直接压缩场景）
    if (cc->direct_compaction && capc)
        capture_free_pages(&cc->freepages, capc, cc->order);
}
```

**关键设计要点**：
- **重扫（Rescan）机制**：当异步模式迁移失败时，通过 `finish_pageblock` 标志强迫完成整个 pageblock 的扫描，然后将该 pageblock 标记为 skip，避免 `fast_find_migrateblock()` 未来再次访问
- **捕获（Capture）机制**：直接压缩时，`compaction_free()` 释放的页面可被 `capture_control` 捕获，立即用于分配，减少竞争
- **迁移回调**：`compaction_alloc()` 从 `cc->freepages` 中取页面，`compaction_free()` 将失败页面放回

#### 10.1.5 完整函数调用链

```
__alloc_pages_slowpath() [mm/page_alloc.c]
  └─ __alloc_pages_direct_compact()    // 直接压缩尝试
       └─ try_to_compact_pages()       // 入口，遍历 zonelist
            ├─ compaction_suitable()   // 检查 zone 是否适合压缩
            └─ compact_zone_order()    // 压缩单个 zone
                 └─ compact_zone()     // 核心压缩循环
                      ├─ isolate_migratepages()    // 隔离可移动页面
                      │    ├─ fast_find_migrateblock()  // 快速查找迁移 block
                      │    └─ isolate_migratepages_block() // 逐 pageblock 隔离
                      │         ├─ too_many_isolated()  // 检查是否过多隔离
                      │         ├─ skip_isolation_on_order()  // 跳过大 folio
                      │         ├─ lruvec_del_folio()   // 从 LRU 移除
                      │         └─ folio_test_clear_lru()  // 清除 LRU 标志
                      ├─ isolate_freepages()      // 隔离空闲页面
                      │    ├─ fast_isolate_freepages() // 快速从 freelist 查找
                      │    └─ isolate_freepages_block()  // 逐 pageblock 隔离
                      │         └─ __isolate_free_page()  // 从 Buddy 系统隔离
                      ├─ migrate_pages()          // 执行页面迁移
                      │    └─ migrate_pages_batch()  // 批量迁移
                      │         ├─ migrate_folio_unmap()  // 取消映射
                      │         └─ migrate_folio_move()   // 移动页面
                      └─ compact_finished()       // 检查是否完成
                           └─ __compact_finished()
                                ├─ compact_scanners_met() // 扫描器是否交汇
                                ├─ fragmentation_score_zone() // 碎片评分
                                └─ find_suitable_fallback() // 查找合适的 fallback

kcompactd_do_work() [mm/compaction.c]  // kcompactd 内核线程
  └─ compact_zone()                     // 后台压缩
```

#### 10.1.6 隔离可迁移页面

`isolate_migratepages()` 是迁移扫描器的入口。它首先尝试 `fast_find_migrateblock()` 快速定位可能包含可迁移页面的 pageblock，然后调用 `isolate_migratepages_block()` 逐 pageblock 扫描。

**`fast_find_migrateblock()`** — 快速查找可迁移 pageblock（算法核心）：

```c
static unsigned long fast_find_migrateblock(struct compact_control *cc)
{
    // 1. 快速路径排除条件
    if (cc->ignore_skip_hint)  return pfn;  // 忽略 skip hint 时不用快速查找
    if (cc->finish_pageblock)  return pfn;  // 正在完成 pageblock 时不用
    if (cc->order <= PAGE_ALLOC_COSTLY_ORDER)  return pfn;  // 小 order 直接线性扫描

    // 2. 计算搜索范围：迁移扫描器前方距离的 1/2（首次）或 1/8（续扫）
    distance = (cc->free_pfn - cc->migrate_pfn) >> 1;
    if (cc->migrate_pfn != cc->zone->zone_start_pfn)
        distance >>= 2;  // 续扫时缩小到 1/8
    high_pfn = pageblock_start_pfn(cc->migrate_pfn + distance);

    // 3. 从高到低遍历 order（从 cc->order-1 到 PAGE_ALLOC_COSTLY_ORDER）
    for (order = cc->order - 1; order >= ...; order--) {
        // 在 MIGRATE_MOVABLE 的 freelist 中反向遍历
        list_for_each_entry(freepage, freelist, buddy_list) {
            free_pfn = page_to_pfn(freepage);
            if (free_pfn < high_pfn) {
                // 找到！将 freelist 重新排序（LRU 旋转）
                move_freelist_tail(freelist, freepage);
                // 从 free_pfn 向前偏移一个 pageblock 作为迁移扫描起始点
                pfn = pageblock_start_pfn(free_pfn - pageblock_nr_pages);
                ...
            }
        }
    }
    return pfn;
}
```

**算法要点**：
- 仅对 `order > PAGE_ALLOC_COSTLY_ORDER` 的请求启用（THP 等大块分配）
- 仅用于 `MIGRATE_MOVABLE` 类型的 pageblock（避免迁移不可移动页面）
- 搜索范围动态缩小：首次扫描 1/2 距离，续扫缩小到 1/8
- 通过 `move_freelist_tail()` 旋转 freelist，实现简单的 LRU 替换策略

**`isolate_migratepages_block()`** 核心流程：

1. **检查隔离上限**：`too_many_isolated()` — 如果 LRU 上已有过多页面被隔离，则等待
2. **逐页扫描**：遍历 pageblock 内的每个页面
3. **跳过检查**：
   - 跳过空闲页面（PageBuddy）、LRU 页面、unevictable 页面
   - 异步模式跳过脏页和写回页面
   - 跳过不可访问映射的页面
4. **隔离操作**：`folio_test_clear_lru()` 将页面从 LRU 移除，加入 `cc->migratepages` 链表
5. **批量限制**：每次最多隔离 `COMPACT_CLUSTER_MAX`（32）个页面

**迁移隔离模式**：
```c
const isolate_mode_t isolate_mode =
    (sysctl_compact_unevictable_allowed ? ISOLATE_UNEVICTABLE : 0) |
    (cc->mode != MIGRATE_SYNC ? ISOLATE_ASYNC_MIGRATE : 0);
```

- `ISOLATE_ASYNC_MIGRATE`：异步模式，跳过脏页和写回页面
- `ISOLATE_UNEVICTABLE`：允许隔离 unevictable 页面（CMA 场景需要）

#### 10.1.7 隔离空闲页面

`isolate_freepages()` 是空闲扫描器的入口。它首先尝试 `fast_isolate_freepages()` 快速查找，然后回退到逐 pageblock 扫描。

**`fast_isolate_freepages()`** — 快速空闲页面查找：

1. 计算首选扫描区域（zone 顶部 1/4 区域）和最小区域（zone 顶部 1/2 区域）
2. 从 `cc->search_order` 开始，从高到低遍历 order
3. 在 `MIGRATE_MOVABLE` 的 freelist 中反向遍历
4. 找到符合 PFN 范围的页面后，调用 `__isolate_free_page()` 从 Buddy 系统隔离
5. 将 freelist 重新排序，使未来搜索跳过最近使用过的页面

**`isolate_freepages_block()`** — 逐 pageblock 隔离空闲页面：

- 使用 `isolate_freepages_block()` 扫描 pageblock 内所有页面
- 检查 PageBuddy 标志，确认是空闲页面
- 调用 `__isolate_free_page()` 将页面从 Buddy 系统取出
- 按 order 加入 `cc->freepages[order]` 链表

#### 10.1.8 压缩完成判断

`compact_finished()` → `__compact_finished()` 判断压缩是否完成：

1. **扫描器交汇**：`compact_scanners_met()` 返回 true 时，表示整个 zone 已完成扫描
2. **主动压缩**：检查碎片化评分是否低于阈值
3. **手动触发**：`/proc/sys/vm/compact_memory` 触发时，继续扫描
4. **页面可用性检查**：检查是否有足够的高阶空闲页面满足需求
   - 检查目标 migratetype 的空闲链表
   - CMA 可回退到 MIGRATE_MOVABLE 的 fallback
   - 检查 `find_suitable_fallback()` 是否有合适的 fallback

#### 10.1.9 压缩延迟机制

当压缩失败时，`defer_compaction()` 会延迟未来的压缩尝试：

```c
static void defer_compaction(struct zone *zone, int order)
{
    zone->compact_considered = 0;
    zone->compact_defer_shift++;
    if (zone->compact_defer_shift > COMPACT_MAX_DEFER_SHIFT)
        zone->compact_defer_shift = COMPACT_MAX_DEFER_SHIFT;
}
```

- 延迟次数指数增长：`1 << compact_defer_shift`
- 最大延迟：`COMPACT_MAX_DEFER_SHIFT`
- 成功分配后，`compaction_defer_reset()` 重置延迟计数

#### 10.1.10 主动压缩与 kcompactd

**kcompactd 内核线程**：每个 NUMA 节点一个，在后台异步执行压缩。

```c
kcompactd_do_work(pgdat)
{
    // 遍历 zone，从低到高
    for (zoneid = 0; zoneid <= cc.highest_zoneidx; zoneid++) {
        // 检查是否适合压缩
        ret = compaction_suit_allocation_order(...);
        if (ret != COMPACT_CONTINUE)
            continue;
        // 执行压缩
        cc.zone = zone;
        status = compact_zone(&cc, NULL);
        // 成功则重置延迟，失败则延迟
        if (status == COMPACT_SUCCESS)
            compaction_defer_reset(zone, cc.order, false);
        else if (status == COMPACT_PARTIAL_SKIPPED || status == COMPACT_COMPLETE)
            defer_compaction(zone, cc.order);
    }
}
```

**主动压缩（Proactive Compaction）**：由 `sysctl_compaction_proactiveness`（默认 20%）控制，当碎片化评分超过阈值时，kcompactd 主动压缩以降低碎片化程度。

```c
// 碎片化评分检查
if (cc->proactive_compaction) {
    score = fragmentation_score_zone(cc->zone);
    wmark_low = fragmentation_score_wmark(true);
    if (score > wmark_low)
        ret = COMPACT_CONTINUE;  // 继续压缩
    else
        ret = COMPACT_SUCCESS;   // 碎片化已达标
}
```

#### 10.1.11 碎片化评分与策略

**碎片化评分**（`fragmentation_score_zone()`）基于 `extfrag_for_order()` 计算，范围 [0, 1000]：

- 评分 =（不可用页帧数 / 总页帧数）× 1000
- 阈值：`sysctl_extfrag_threshold`（默认 500）
- 主动压缩阈值：`fragmentation_score_wmark(true)` = `sysctl_compaction_proactiveness * 10`

**内存碎片规避策略**：`defrag_mode` 控制 kcompactd 的 watermark 策略：
- 启用时：使用 `ALLOC_WMARK_HIGH`，更积极地预留页面
- 禁用时：使用 `ALLOC_WMARK_MIN`，标准水位线

#### 10.1.12 接口与可调参数

| 接口 | 路径 | 说明 |
|------|------|------|
| 手动触发 | `/proc/sys/vm/compact_memory` | 写入 1 触发全系统压缩 |
| 主动压缩比例 | `/proc/sys/vm/compaction_proactiveness` | 默认 20，范围 [0, 100] |
| 碎片阈值 | `/proc/sys/vm/extfrag_threshold` | 默认 500，范围 [1, 1000] |
| 允许隔离 unevictable | `/proc/sys/vm/compact_unevictable_allowed` | 默认 0（关闭） |

### 10.2 页面迁移（Page Migration）

文件：`mm/migrate.c`（2,750 行），`mm/migrate_device.c`（1,491 行），`include/linux/migrate.h`，`include/linux/migrate_mode.h`

#### 10.2.1 概述

页面迁移（Page Migration）是将一个物理页面（folio）的内容移动到另一个物理页面的过程。它是内存压缩、NUMA 平衡、CMA、内存热插拔、内存故障处理等核心功能的基础。

页面迁移的**核心原则**：迁移过程中，页面内容不能丢失，访问页面的进程不能感知到页面物理位置的改变（通过迁移 PTE 实现透明性）。

#### 10.2.2 核心数据结构

**迁移模式**（`enum migrate_mode`）：

```c
enum migrate_mode {
    MIGRATE_ASYNC,       /* 永不阻塞，快速失败 */
    MIGRATE_SYNC_LIGHT,  /* 允许阻塞大多数操作，但不等待 I/O */
    MIGRATE_SYNC,        /* 完全同步，可等待 I/O 完成 */
};
```

**迁移原因**（`enum migrate_reason`）：

```c
enum migrate_reason {
    MR_COMPACTION,      /* 内存压缩 */
    MR_MEMORY_FAILURE,  /* 内存故障处理（HWPOISON） */
    MR_MEMORY_HOTPLUG,  /* 内存热插拔 */
    MR_SYSCALL,         /* move_pages() 系统调用 */
    MR_MEMPOLICY_MBIND, /* mbind() 内存策略绑定 */
    MR_NUMA_MISPLACED,  /* NUMA 页面错位 */
    MR_CONTIG_RANGE,    /* 连续物理内存分配 */
    MR_LONGTERM_PIN,    /* 长期 pin 的页面 */
    MR_DEMOTION,        /* 大页面降级 */
    MR_DAMON,           /* DAMON 主动迁移 */
    MR_TYPES
};
```

**回调函数类型**：

```c
typedef struct folio *(*new_folio_t)(struct folio *folio, unsigned long private);
typedef void (*free_folio_t)(struct folio *folio, unsigned long private);
```

- `get_new_folio`：为目标页面分配新页面的回调
- `put_new_folio`：迁移失败时释放目标页面的回调

**`struct movable_operations`** — 驱动页面迁移操作接口：

```c
struct movable_operations {
    bool (*isolate_page)(struct page *, isolate_mode_t);
    int  (*migrate_page)(struct page *dst, struct page *src, enum migrate_mode);
    void (*putback_page)(struct page *);
};
```

**`struct migrate_vma`** — 设备内存迁移参数：

```c
struct migrate_vma {
    struct vm_area_struct *vma;
    unsigned long *dst;          /* 目标 PFN 数组 */
    unsigned long *src;          /* 源 PFN 数组 */
    unsigned long cpages;        /* 成功迁移的页面数 */
    unsigned long npages;        /* 总页面数 */
    unsigned long start;         /* 起始虚拟地址 */
    unsigned long end;           /* 结束虚拟地址 */
    void *pgmap_owner;           /* 设备私有内存所有者 */
    unsigned long flags;         /* 迁移方向标志 */
    struct page *fault_page;     /* 缺页时触发的迁移页面 */
};
```

**MIGRATE_PFN 标志位**：

```c
#define MIGRATE_PFN_VALID     (1UL << 0)  /* PFN 有效 */
#define MIGRATE_PFN_MIGRATE   (1UL << 1)  /* 需要迁移 */
#define MIGRATE_PFN_WRITE     (1UL << 3)  /* 可写 */
#define MIGRATE_PFN_COMPOUND  (1UL << 4)  /* 复合页面 */
#define MIGRATE_PFN_SHIFT     6
```

#### 10.2.3 完整函数调用链

```
migrate_pages() [mm/migrate.c]
  ├─ migrate_hugetlbs()                        // 处理 hugetlb 页面
  ├─ migrate_pages_batch()                     // 批量迁移（异步模式）
  │    ├─ [Phase 1: Unmap] migrate_folio_unmap()  // 取消映射原始页面
  │    │    ├─ get_new_folio()                    // 分配目标页面
  │    │    ├─ folio_trylock() / folio_lock()      // 锁定源页面
  │    │    ├─ __folio_migrate_mapping()          // 迁移映射关系
  │    │    └─ try_to_migrate()                   // 替换 PTE 为迁移 PTE
  │    └─ [Phase 2: Move] migrate_folio_move()    // 执行迁移
  │         ├─ move_to_new_folio()                // 拷贝数据到目标页面
  │         │    ├─ mapping->a_ops->migrate_folio()  // 文件系统回调
  │         │    └─ fallback_migrate_folio()         // 通用回退
  │         ├─ remove_migration_ptes()             // 移除迁移 PTE，建立新映射
  │         └─ folio_add_lru()                    // 加入 LRU
  │
  └─ migrate_pages_sync()                        // 同步模式（先异步尝试，再逐个同步）
       └─ migrate_pages_batch()                 // 先异步批量尝试
            └─ migrate_pages_batch()            // 失败页面逐个同步重试

migrate_vma_setup() [mm/migrate_device.c]  // 设备迁移路径
  ├─ migrate_vma_collect()                     // 收集 VMA 范围内的页面
  ├─ migrate_vma_unmap()                       // 取消映射
  └─ migrate_vma_prepare()                     // 准备迁移

migrate_vma_pages()                            // 迁移设备页面元数据
migrate_vma_finalize()                         // 完成迁移，清理
```

#### 10.2.4 migrate_pages 入口

`migrate_pages()` 是页面迁移的顶层入口函数：

```c
int migrate_pages(struct list_head *from,
    new_folio_t get_new_folio,
    free_folio_t put_new_folio,
    unsigned long private,
    enum migrate_mode mode,
    int reason,
    unsigned int *ret_succeeded)
```

核心流程：
1. 先处理 hugetlb 页面（`migrate_hugetlbs()`）
2. 按 `NR_MAX_BATCHED_MIGRATION`（通常 64）批量切分输入链表
3. 异步模式（MIGRATE_ASYNC）直接调用 `migrate_pages_batch()`
4. 同步模式（SYNC_LIGHT/SYNC）调用 `migrate_pages_sync()`（先异步批量尝试，同步逐个重试失败页面）
5. 处理 THP 拆分后的重试：`migrate_pages_batch()` 以 MIGRATE_ASYNC 模式重试拆分后的普通页面
6. 统计成功/失败计数（`PGMIGRATE_SUCCESS` / `PGMIGRATE_FAIL`）

**批量迁移切分策略**：
- 每次从 `from` 链表切分最多 `NR_MAX_BATCHED_MIGRATION`（64）个页面
- 切分后通过 `list_cut_before()` 将子链表移到 `folios` 临时链表
- 异步模式直接调用 `migrate_pages_batch()`，同步模式调用 `migrate_pages_sync()`
- 每次 batch 完成后，检查 `from` 是否还有剩余页面，有则 `goto again`

#### 10.2.5 migrate_pages_batch 批量迁移

`migrate_pages_batch()` 是页面迁移的核心实现，采用两阶段（Phase）设计，支持多轮重试：

```c
static int migrate_pages_batch(struct list_head *from, ...)
{
    // 多轮重试循环（最多 nr_pass 次）
    for (pass = 0; pass < nr_pass && retry; pass++) {
        retry = 0;
        LIST_HEAD(unmap_folios);   // 已取消映射的源 folio 链表
        LIST_HEAD(dst_folios);     // 对应的目标 folio 链表

        // Phase 1: Unmap — 遍历所有 folio，取消映射
        list_for_each_entry_safe(folio, folio2, from, lru) {
            // 1a. 处理 deferred split list 上的大 folio
            //     如果 folio 在延迟拆分链表上且部分映射，立即拆分
            if (nr_pages > 2 &&
                !list_empty(&folio->_deferred_list) &&
                folio_test_partially_mapped(folio)) {
                try_split_folio(folio, split_folios, mode);
                continue;
            }

            // 1b. 检查 THP 迁移支持
            if (!thp_migration_supported() && is_thp) {
                // 不支持 THP 迁移时，尝试拆分大 folio
                // 拆分失败则标记为失败
                ...
            }

            // 1c. 调用 migrate_folio_unmap() 取消映射
            if (migrate_folio_unmap(get_new_folio, put_new_folio, private,
                        folio, &dst, mode, ret_folios) == 0) {
                list_move(&folio->lru, &unmap_folios);
                list_add(&dst->lru, &dst_folios);
            }
        }

        // Phase 2: Move — 刷新 TLB 后执行数据迁移
        try_to_unmap_flush();
        migrate_folios_move(&unmap_folios, &dst_folios, put_new_folio,
                private, mode, reason, ret_folios, stats,
                &retry, &thp_retry, &nr_failed, &nr_retry_pages);
    }
}
```

**Phase 1: Unmap（取消映射）**
- 遍历 `from` 链表，对每个 folio 调用 `migrate_folio_unmap()`
- 将成功取消映射的 folio 加入 `unmap_folios` 链表
- 目标 folio 加入 `dst_folios` 链表
- **重要预处理**：
  - 检查 `_deferred_list`：如果 folio 在延迟拆分链表上且部分映射，调用 `try_split_folio()` 立即拆分
  - 检查 THP 迁移支持：如果架构不支持 THP 迁移（`!thp_migration_supported()`），尝试拆分大 folio

**Phase 2: Move（执行迁移）**
- `try_to_unmap_flush()` — 刷新 TLB，确保所有 CPU 看到新映射
- 遍历 `unmap_folios`，对每个 folio 调用 `migrate_folio_move()`
- 成功迁移的从链表中移除，失败的恢复到原始链表

**同步模式限制**：`VM_WARN_ON_ONCE(mode != MIGRATE_ASYNC && !list_is_singular(from))` — 非异步模式下，链表长度必须 ≤ 1，避免死锁。

#### 10.2.6 migrate_folio_unmap 映射取消

`migrate_folio_unmap()` 负责取消源页面的映射并为迁移做准备，包含复杂的锁策略：

```c
static int migrate_folio_unmap(new_folio_t get_new_folio, ...)
{
    // 1. 分配目标页面
    dst = get_new_folio(src, private);
    dst->private = NULL;

    // 2. 锁定源页面 — 三层锁策略
    if (!folio_trylock(src)) {
        // 第 1 层：MIGRATE_ASYNC — 直接失败
        if (mode == MIGRATE_ASYNC)  goto out;

        // 第 2 层：PF_MEMALLOC（直接压缩）— 避免死锁，直接失败
        // 原因：readahead 可能合并多个页面到同一个 bio，当前进程
        // 可能尝试锁定已经在同一 bio 中正在等待的页面
        if (current->flags & PF_MEMALLOC)  goto out;

        // 第 3 层：MIGRATE_SYNC_LIGHT — 等待短暂锁但不等待 I/O
        if (mode == MIGRATE_SYNC_LIGHT && !folio_test_uptodate(src))
            goto out;  // 不是 uptodate 说明需要 I/O，不等待

        // MIGRATE_SYNC — 完全等待
        folio_lock(src);
    }

    // 3. 处理写回页面
    if (folio_test_writeback(src)) {
        switch (mode) {
        case MIGRATE_SYNC:
            folio_wait_writeback(src);  // 完全同步 → 等待写回完成
            break;
        default:
            rc = -EBUSY;
            goto out;  // 异步/轻量同步 → 跳过
        }
    }

    // 4. 获取 anon_vma 引用（延迟释放，防止迁移期间 anon_vma 被释放）
    if (folio_test_anon(src))
        anon_vma = folio_get_anon_vma(src);

    // 5. 锁定目标页面
    folio_lock(dst);

    // 6. 迁移映射关系（mapping 转移）
    __folio_migrate_mapping(..., dst, src);

    // 7. 取消 PTE 映射：将所有 PTE 替换为迁移 PTE
    try_to_migrate(src, TTU_IGNORE_MLOCK);

    // 8. 检查是否完全取消映射
    if (folio_mapped(src))
        goto out_unlock;  // 还有映射未取消，回退

    // 9. 保存状态（供后续 migrate_folio_move 使用）
    dst->private = (void *)old_page_state;
    return 0;  // 成功
}
```

**三层锁策略总结**：

| 模式 | folio_trylock 失败 | 写回页面 | 适用场景 |
|------|-------------------|----------|----------|
| MIGRATE_ASYNC | 直接返回 -EAGAIN | 跳过 | 直接压缩、kswapd |
| MIGRATE_SYNC_LIGHT | 非 uptodate 则跳过，否则 folio_lock | 跳过 | NUMA 平衡 |
| MIGRATE_SYNC | folio_lock | folio_wait_writeback | 热插拔、move_pages |
| PF_MEMALLOC | 直接返回 -EAGAIN | 跳过 | 直接压缩（避免 deadlock） |

**关键设计**：
- `PF_MEMALLOC` 特殊处理：直接压缩路径设置了 `PF_MEMALLOC` 标志，此时 `folio_lock()` 可能导致死锁（readahead 合并多个页面到同一 bio 的场景），因此必须跳过
- `anon_vma` 引用：`folio_get_anon_vma()` 延迟释放 anon_vma，防止迁移期间 anon_vma 被其他地方释放
- `try_to_migrate()` 使用 `TTU_IGNORE_MLOCK` 标志，忽略 mlock 限制

#### 10.2.7 migrate_folio_move 页面迁移

`migrate_folio_move()` 负责完成数据迁移：

```c
static int migrate_folio_move(free_folio_t put_new_folio, ...)
{
    // 1. 提取状态（从 dst->private 中恢复 Phase 1 保存的状态）
    __migrate_folio_extract(dst, &old_page_state, &anon_vma);

    // 2. 特殊路径：movable_operations（Zsmalloc 等自定义迁移）
    if (unlikely(page_has_movable_ops(&src->page))) {
        rc = migrate_movable_ops_page(&dst->page, &src->page, mode);
        if (rc)  goto out;
        goto out_unlock_both;  // 跳过标准的 move_to_new_folio
    }

    // 3. 标准路径：通过 mapping 回调或通用拷贝迁移数据
    rc = move_to_new_folio(dst, src, mode);

    // 4. 将目标页面加入 LRU（确保 mlock 计数正确）
    folio_add_lru(dst);
    if (old_page_state & PAGE_WAS_MLOCKED)
        lru_add_drain();

    // 5. 移除迁移 PTE，建立新映射
    if (old_page_state & PAGE_WAS_MAPPED)
        remove_migration_ptes(src, dst, 0);

    // 6. 释放目标页面引用（新映射持有引用）
    folio_put(dst);

    // 7. 清理源页面
    list_del(&src->lru);
    if (anon_vma)  put_anon_vma(anon_vma);
    folio_unlock(src);
    return 0;
}
```

**关键流程**：
1. **提取状态**：`__migrate_folio_extract()` 从目标 folio 的 private 字段提取 Phase 1 保存的 `old_page_state` 和 `anon_vma`
2. **movable_operations 路径**：如果页面注册了 `movable_operations`（如 Zsmalloc 的 zspage），调用 `migrate_movable_ops_page()` 走自定义迁移路径，跳过标准的数据拷贝
3. **标准路径**：`move_to_new_folio()` — 通过 `mapping->a_ops->migrate_folio()` 回调或通用 `folio_copy()` 拷贝数据
4. **加入 LRU**：`folio_add_lru(dst)` — 将新页面加入 LRU（必须在 `remove_migration_ptes()` 之前，确保 mlock 计数正确）
5. **移除迁移 PTE**：`remove_migration_ptes()` — 将所有迁移 PTE 替换为指向新页面的 PTE
6. **清理源页面**：`folio_unlock(src)` → 源页面引用计数归零，被释放回 Buddy 系统

#### 10.2.8 move_to_new_folio 数据迁移

`move_to_new_folio()` 是实际拷贝数据的函数：

```c
static int move_to_new_folio(struct folio *dst, struct folio *src,
                             enum migrate_mode mode)
{
    struct address_space *mapping = folio_mapping(src);
    int rc = -EAGAIN;

    if (!mapping)
        rc = migrate_folio(mapping, dst, src, mode);  // 匿名页面
    else if (mapping_inaccessible(mapping))
        rc = -EOPNOTSUPP;
    else if (mapping->a_ops->migrate_folio)
        rc = mapping->a_ops->migrate_folio(...);  // 文件系统回调
    else
        rc = fallback_migrate_folio(mapping, dst, src, mode);  // 通用回退
    ...
}
```

**迁移路径**：
- **匿名页面**：`migrate_folio()` → `__migrate_folio()` → `folio_copy(folio_dst, folio_src)` 直接拷贝数据
- **文件缓存页面**：通过 `a_ops->migrate_folio()` 回调（如 `filemap_migrate_folio()`）
- **buffer head 页面**：`buffer_migrate_folio()` 或 `buffer_migrate_folio_norefs()`

#### 10.2.9 folio_migrate_mapping 映射转移

`__folio_migrate_mapping()` 负责将 folio 从一个 `address_space` 转移到另一个，这是最关键的原子操作：

1. **匿名 folio**（无 mapping）：冻结引用计数，更新 `newfolio->mapping` 和 `newfolio->index`，处理大 folio 的 deferred split 队列
2. **交换缓存 folio**：通过 `swap_cluster_get_and_lock_irq()` 锁定交换簇，在 XArray 中替换旧 folio 为新 folio
3. **文件缓存 folio**：通过 `xas_lock_irq()` 锁定 XArray，在 `mapping->i_pages` 中替换旧 folio 为新 folio
4. **引用计数处理**：`folio_ref_freeze()` 冻结引用，确保迁移期间没有新访问

#### 10.2.10 设备迁移（migrate_device.c）

设备迁移支持 CPU 与 GPU 等设备之间的页面迁移，用于 HMM（Heterogeneous Memory Management）。

**migrate_vma_setup()** 流程：
1. `migrate_vma_collect()` — 遍历 VMA 范围内的所有 PTE，收集页面的 PFN 存入 `src` 数组
2. `migrate_vma_unmap()` — 锁定页面，用迁移 PTE 替换原 PTE，检查是否被 pin
3. `migrate_vma_prepare()` — 隔离页面到 LRU，锁定目标页面

**migrate_vma_pages()**：迁移页面元数据，将源页面中的 `struct page` 元数据迁移到目标页面。

**migrate_vma_finalize()**：完成迁移，清理迁移 PTE，建立新映射。

**migrate_device_range()**：基于 PFN 范围的设备迁移（不依赖 VMA），用于驱动卸载等场景：
1. 遍历 PFN 范围，锁定并获取页面
2. 取消映射（`migrate_device_unmap()`）
3. 驱动拷贝数据
4. 调用 `migrate_device_pages()` 迁移元数据

#### 10.2.11 迁移统计与事件

| 事件 | 计数变量 | 说明 |
|------|----------|------|
| `PGMIGRATE_SUCCESS` | `stats.nr_succeeded` | 成功迁移的页面数 |
| `PGMIGRATE_FAIL` | `stats.nr_failed_pages` | 迁移失败的页面数 |
| `THP_MIGRATION_SUCCESS` | `stats.nr_thp_succeeded` | 成功迁移的 THP 数 |
| `THP_MIGRATION_FAIL` | `stats.nr_thp_failed` | 迁移失败的 THP 数 |
| `THP_MIGRATION_SPLIT` | `stats.nr_thp_split` | 迁移时被拆分的 THP 数 |

#### 10.2.12 迁移调用场景

| 场景 | 入口函数 | 迁移原因 | 模式 |
|------|----------|----------|------|
| 内存压缩 | `compact_zone()` → `migrate_pages()` | MR_COMPACTION | ASYNC/SYNC_LIGHT |
| NUMA 平衡 | `migrate_misplaced_folio()` | MR_NUMA_MISPLACED | ASYNC |
| 内存热插拔 | `do_migrate_range()` | MR_MEMORY_HOTPLUG | SYNC |
| move_pages 系统调用 | `kernel_move_pages()` | MR_SYSCALL | SYNC |
| mbind 系统调用 | `migrate_to_node()` | MR_MEMPOLICY_MBIND | SYNC |
| CMA 连续分配 | `alloc_contig_range()` | MR_CONTIG_RANGE | ASYNC/SYNC |
| 长期 pin | `check_and_migrate_movable_pages()` | MR_LONGTERM_PIN | ASYNC |
| 内存故障 | `memory_failure()` | MR_MEMORY_FAILURE | SYNC |
| 大页面降级 | `folio_defer_degraded()` | MR_DEMOTION | ASYNC |
| 设备迁移 | `migrate_vma_setup()` | — | SYNC |

---

## 11. Zswap 与 Zsmalloc

### 11.1 Zswap — 压缩交换缓存

文件：`mm/zswap.c`（1,851 行）

#### 11.1.1 概述

Zswap 在内存中维护一个压缩缓存，拦截换出页面，压缩后存储在内存中，减少对慢速交换设备的 I/O。当内存压力增大时，Zswap 通过 shrinker 将最旧的压缩页面写回实际交换设备，实现"内存作为交换设备的缓存"。

**架构要点**：
- 拦截 `__swap_writepage()` 的换出路径
- 使用 `crypto_acomp` 异步压缩接口（实际同步等待）
- 后端存储使用 Zsmalloc 分配器
- 支持动态 shrinker 回收
- 每个 swap 类型有独立的 XArray 树管理压缩条目

#### 11.1.2 核心数据结构

**`struct zswap_pool`** — 压缩池：

```c
struct zswap_pool {
    struct zs_pool *zs_pool;                           /* Zsmalloc 池 */
    struct crypto_acomp_ctx __percpu *acomp_ctx;       /* Per-CPU 压缩上下文 */
    struct percpu_ref ref;                             /* 引用计数 */
    struct list_head list;                             /* 全局池链表 */
    struct work_struct release_work;                   /* 释放工作项 */
    struct hlist_node node;                            /* CPU hotplug 节点 */
    char tfm_name[CRYPTO_MAX_ALG_NAME];                /* 压缩算法名称 */
};
```

**`struct crypto_acomp_ctx`** — Per-CPU 压缩上下文：

```c
struct crypto_acomp_ctx {
    struct crypto_acomp *acomp;     /* 异步压缩算法实例 */
    struct acomp_req *req;          /* 压缩请求 */
    struct crypto_wait wait;        /* 同步等待 */
    u8 *buffer;                     /* 压缩输出缓冲区 (PAGE_SIZE) */
    struct mutex mutex;             /* 互斥锁 */
};
```

**`struct zswap_entry`** — 单个压缩页面的元数据：

```c
struct zswap_entry {
    swp_entry_t swpentry;           /* 关联的交换条目 */
    unsigned int length;            /* 压缩后数据长度（字节） */
    bool referenced;                /* 最近被引用标志 */
    struct zswap_pool *pool;        /* 所属压缩池 */
    unsigned long handle;           /* Zsmalloc 分配句柄 */
    struct obj_cgroup *objcg;       /* 内存 cgroup 计费 */
    struct list_head lru;           /* LRU 链表节点 */
};
```

**全局 XArray 管理**：

```c
static struct xarray *zswap_trees[MAX_SWAPFILES];  // 每个 swap 类型一个 XArray 数组
```

每个交换设备被划分为多个地址空间（`ZSWAP_ADDRESS_SPACE_SHIFT = 14`，即 64M 一个），每个地址空间一个 XArray，以 `swp_offset` 的偏移量索引。

#### 11.1.3 可调参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `enabled` | `CONFIG_ZSWAP_DEFAULT_ON` | 启用/禁用 Zswap |
| `compressor` | `CONFIG_ZSWAP_COMPRESSOR_DEFAULT` | 压缩算法（lzo/lz4/zstd 等） |
| `max_pool_percent` | 20 | 压缩池最大内存占比（%） |
| `accept_threshold_percent` | 90 | 达到上限后重新接受新页面的阈值（% of max） |
| `shrinker_enabled` | `CONFIG_ZSWAP_SHRINKER_DEFAULT_ON` | 启用 shrinker 回收 |

#### 11.1.4 压缩存储路径

`zswap_store()` 是 Zswap 的入口，由 `__swap_writepage()` 调用：

```c
bool zswap_store(struct folio *folio)
{
    // 1. 检查 Zswap 是否启用
    if (!zswap_enabled) goto check_old;

    // 2. 获取 cgroup 对象并检查限制
    objcg = get_obj_cgroup_from_folio(folio);
    if (objcg && !obj_cgroup_may_zswap(objcg))
        shrink_memcg(memcg);  // 回收 cgroup 内存

    // 3. 检查全局限制
    if (zswap_check_limits()) goto put_objcg;

    // 4. 获取当前压缩池
    pool = zswap_pool_current_get();

    // 5. 逐 page 压缩存储
    for (index = 0; index < nr_pages; ++index) {
        page = folio_page(folio, index);
        zswap_store_page(page, objcg, pool);
    }

    // 6. 更新统计
    count_vm_events(ZSWPOUT, nr_pages);
}
```

**`zswap_store_page()`** 核心流程：

1. 分配 `zswap_entry` 元数据（`zswap_entry_cache_alloc()`）
2. **压缩**：`zswap_compress()` — 使用 crypto_acomp 压缩页面数据
3. **存储到 XArray**：`xa_store()` — 将 entry 存入 `swap_zswap_tree()`
4. **处理陈旧条目**：如果存在旧的 entry，释放它
5. **初始化 entry**：设置 `pool`、`swpentry`、`objcg`、`referenced = true`
6. **加入 LRU**：`zswap_lru_add()` — 加入全局 LRU 链表，用于后续写回

#### 11.1.5 压缩实现

`zswap_compress()` 使用内核的 crypto_acomp 异步压缩框架：

```c
static bool zswap_compress(struct page *page, struct zswap_entry *entry,
                           struct zswap_pool *pool)
{
    // 1. 获取 Per-CPU 压缩上下文
    acomp_ctx = acomp_ctx_get_cpu_lock(pool);

    // 2. 设置输入 SG（源页面）和输出 SG（缓冲区）
    sg_set_page(&input, page, PAGE_SIZE, 0);
    sg_init_one(&output, dst, PAGE_SIZE);

    // 3. 执行压缩（同步等待异步完成）
    comp_ret = crypto_wait_req(
        crypto_acomp_compress(acomp_ctx->req), &acomp_ctx->wait);

    // 4. 处理不可压缩页面
    if (dlen > PAGE_SIZE) {
        // 如果 writeback 启用，存储未压缩副本
        // 如果 writeback 禁用，拒绝该页面
    }

    // 5. 分配 Zsmalloc 空间存储压缩数据
    handle = zs_malloc(pool->zs_pool, dlen, GFP_KERNEL, ...);
    zs_obj_write(pool->zs_pool, handle, dst, dlen);
}
```

**不可压缩页面处理**：如果压缩后数据大于等于 `PAGE_SIZE`，则存储未压缩的副本（`length = PAGE_SIZE`），以保持 LRU 写回顺序。如果 writeback 被禁用，则拒绝该页面。

#### 11.1.6 解压读取路径

`zswap_load()` 由 `swap_read_folio()` 调用，从 Zswap 读取压缩数据：

```c
int zswap_load(struct folio *folio)
{
    // 1. 从 XArray 查找 entry
    entry = xa_load(tree, offset);
    if (!entry) return -ENOENT;

    // 2. 解压
    if (!zswap_decompress(entry, folio)) {
        folio_unlock(folio);
        return -EIO;
    }

    // 3. 标记 up-to-date
    folio_mark_uptodate(folio);

    // 4. 如果读取到 swapcache 中，释放 Zswap entry
    //    （swapcache 成为数据的权威所有者）
    if (swapcache) {
        folio_mark_dirty(folio);
        xa_erase(tree, offset);
        zswap_entry_free(entry);
    }

    folio_unlock(folio);
    return 0;
}
```

**关键设计**：当读取到 swapcache 时，Zswap 会释放 entry，因为 swapcache 和 Zswap 同时持有相同数据副本没有意义。

#### 11.1.7 解压实现

`zswap_decompress()` 与压缩对称，但有特殊处理：

```c
static bool zswap_decompress(struct zswap_entry *entry, struct folio *folio)
{
    // 1. 读取 SG 列表（Zsmalloc 可能返回 1-2 个 SG 条目）
    zs_obj_read_sg_begin(pool->zs_pool, entry->handle, input, entry->length);

    // 2. 处理未压缩的页面（length == PAGE_SIZE）
    if (entry->length == PAGE_SIZE) {
        memcpy_from_sglist(dst, input, 0, PAGE_SIZE);  // 直接拷贝
    } else {
        // 3. 执行解压（同步等待）
        sg_set_folio(&output, folio, PAGE_SIZE, 0);
        ret = crypto_wait_req(crypto_acomp_decompress(acomp_ctx->req), ...);
    }

    zs_obj_read_sg_end(pool->zs_pool, entry->handle);
}
```

#### 11.1.8 写回机制

当 Zswap 池达到 `max_pool_percent` 限制时，需要将最旧的压缩页面写回实际交换设备。

**收缩触发条件**：
- **全局限制**：`zswap_pool_total_size > max_pool_size`（`max_pool_percent` 占总内存的比例）
- **memcg 限制**：`obj_cgroup_may_zswap()` 检查 cgroup 是否达到限制，调用 `shrink_memcg()` 回收
- **Shrinker 回调**：内核内存压力通过 shrinker 通知 Zswap 收缩

**`shrink_worker()`** — 后台写回工作线程：

```c
static void shrink_worker(struct work_struct *w)
{
    // 1. 遍历所有 memcg，找到需要回收的
    while (zswap_shrink_pages) {
        // 2. 从 LRU 中获取最旧的 entry
        entry = list_lru_get_next(&zswap_list_lru, ...);

        // 3. Referenced 位旋转策略
        if (entry->referenced) {
            // 给第二次机会：清除 referenced 位，将 entry 旋转到 LRU 尾部
            entry->referenced = false;
            continue;
        }

        // 4. 执行写回
        zswap_writeback_entry(entry, ...);
    }
}
```

**Referenced 位旋转策略**：
- 新写入的 entry 在 `zswap_store_page()` 中设置 `entry->referenced = true`
- 每次页面被访问时（通过 `zswap_load()` 或类似路径），`entry->referenced` 被置位
- Shrinker 扫描到 referenced 的 entry 时，清除 referenced 位并将 entry 旋转到 LRU 尾部，**不立即写回**
- 下次扫描到同一个 entry 时，如果 referenced 已被清除且没有再次被访问，则执行写回
- 这实现了"第二次机会"（Second Chance）算法，主动保护近期活跃页面

**`zswap_writeback_entry()`** 写回流程：

```c
static int zswap_writeback_entry(struct zswap_entry *entry, ...)
{
    // 1. 分配 swapcache folio（将数据从 Zswap 转移到 swapcache）
    folio = __read_swap_cache_async(entry->swpentry, ...);
    if (!folio)  return -ENOMEM;

    // 2. 从 Zswap 解压数据到 folio
    zswap_decompress(entry, folio);

    // 3. 从 XArray 中删除 entry（使 Zswap 不再持有该数据）
    xa_erase(tree, offset);

    // 4. 释放 Zswap entry（释放 Zsmalloc 空间）
    zswap_entry_free(entry);

    // 5. 将数据写入实际交换设备（回退到标准 swap I/O 路径）
    __swap_writepage(folio, ...);
}
```

**写回流程关键点**：
1. **先解压后写回**：Zswap 先将压缩数据解压到 swapcache folio，再通过 `__swap_writepage()` 写入磁盘
2. **原子删除**：`xa_erase()` 删除 XArray 中的 entry，确保并发读取不会看到过期数据
3. **空间释放**：`zswap_entry_free()` 释放 Zsmalloc 分配的空间，更新统计计数器
4. **写回后 swapcache 接管**：数据写入磁盘后，swapcache 成为权威所有者，后续缺页直接通过 swap 读取

**Shrinker 动态调整**（基于三个因素）：
1. **Referenced 位**：每个 entry 有 referenced 位，shrinker 将其清除后旋转，给第二次机会。新写入的页面受到保护，老化页面被优先回收
2. **Swapin 计数器**：观察到 swapin 信号说明过度收缩，应减慢速度
3. **压缩率**：压缩率越好，写回带来的收益越小，应减少回收

**memcg 交互**：
- Zswap 支持 cgroup v2 的 `memory.zswap.current` 和 `memory.zswap.max` 接口
- 每个 memcg 有独立的 LRU 链表（通过 `list_lru` 实现 memcg-aware LRU）
- 写回时优先回收使用量最多的 memcg 的 entry，避免单个 cgroup 占用过多压缩池
- `shrink_memcg()` 在 `zswap_store()` 路径中被调用，当 cgroup 接近限制时主动回收

#### 11.1.9 相同页面去重

Zswap 内部会自动检测并去重内容完全相同的页面（same-filled pages），这是通过 `zswap_compress()` 中的检测实现的——如果压缩后数据为全零填充，Zswap 可以特殊处理，但这些逻辑在 `zsmalloc` 层面透明处理。

#### 11.1.10 统计信息

Zswap 维护以下统计计数器（通过 `debugfs` 查看）：

```c
// 全局统计
atomic_long_t zswap_stored_pages;                    // 当前存储的页面数
static atomic_long_t zswap_stored_incompressible_pages; // 不可压缩页面数

// 失败计数器
static u64 zswap_pool_limit_hit;                     // 池上限命中
static u64 zswap_written_back_pages;                 // 写回页面数
static u64 zswap_reject_reclaim_fail;                // 回收失败
static u64 zswap_reject_compress_fail;               // 压缩失败
static u64 zswap_reject_compress_poor;               // 压缩率太差
static u64 zswap_decompress_fail;                    // 解压失败
static u64 zswap_reject_alloc_fail;                  // 分配失败
static u64 zswap_reject_kmemcache_fail;              // 元数据分配失败
```

**debugfs 接口**（挂载后 `/sys/kernel/debug/zswap/`）：

| 文件 | 说明 |
|------|------|
| `total_size` | 压缩池总大小（字节） |
| `stored_pages` | 当前存储的页面数 |
| `stored_incompressible_pages` | 不可压缩页面数 |
| `pool_limit_hit` | 池上限命中次数 |
| `written_back_pages` | 写回页面数 |
| `reject_compress_fail` | 压缩失败次数 |
| `reject_alloc_fail` | 分配失败次数 |
| `reject_kmemcache_fail` | 元数据分配失败次数 |

#### 11.1.11 初始化与池管理

**`zswap_setup()`** 初始化流程：
1. 创建 `zswap_entry` kmem_cache
2. 注册 CPU hotplug 回调（Per-CPU 压缩上下文准备）
3. 创建 shrinker 工作队列
4. 注册 shrinker 和 LRU
5. 创建初始压缩池

**`zswap_pool_create()`** 池创建：
1. 创建 Zsmalloc 池（`zs_create_pool()`）
2. 分配 Per-CPU 压缩上下文
3. 初始化 percpu_ref 引用计数
4. 注册 CPU hotplug 状态

**池切换**：通过 `zswap_pool_current_get()` 获取当前池，使用 RCU 保护。多个池可以共存，但只有一个活跃池。

### 11.2 Zsmalloc — 小对象压缩分配器

文件：`mm/zsmalloc.c`（2,258 行）

#### 11.2.1 概述

Zsmalloc 是专门为 Zswap 和 Zram 设计的压缩内存分配器，将多个小对象存储在一个物理页面（或一组物理页面）中，显著减少内部碎片。与 SLUB 分配器相比，Zsmalloc 允许对象跨页面边界，从而支持更大的对象密度。

**关键设计理念**：
- 使用**句柄（Handle）**而非直接指针，支持页面迁移和压缩
- 对象可以跨物理页面边界（zspage 多页链）
- 通过大小分类（Size Class）管理不同大小的对象
- 支持 compaction 和 shrinker 回收

#### 11.2.2 核心数据结构

**`struct zs_pool`** — 分配池：

```c
struct zs_pool {
    const char *name;                                           /* 池名称 */
    struct size_class *size_class[ZS_SIZE_CLASSES];             /* 大小分类数组 */
    atomic_long_t pages_allocated;                              /* 已分配页面数 */
    struct zs_pool_stats stats;                                 /* 统计信息 */
    struct shrinker *shrinker;                                  /* Shrinker */
#ifdef CONFIG_COMPACTION
    struct work_struct free_work;                               /* 延迟释放工作 */
#endif
    rwlock_t lock;                                              /* 保护 zspage 迁移 */
    atomic_t compaction_in_progress;                            /* 压缩进行中标志 */
};
```

**`struct size_class`** — 大小分类：

```c
struct size_class {
    spinlock_t lock;                                            /* 保护本 class */
    struct list_head fullness_list[NR_FULLNESS_GROUPS];         /* 按 fullness 分组的 zspage 链表 */
    int size;                                                   /* 对象大小（字节） */
    int objs_per_zspage;                                        /* 每个 zspage 的对象数 */
    int pages_per_zspage;                                       /* 每个 zspage 的页面数 */
    unsigned int index;                                         /* class 索引 */
    struct zs_size_stat stats;                                  /* 统计 */
};
```

**`struct zspage`** — 压缩页组：

```c
struct zspage {
    unsigned int huge:HUGE_BITS;               /* 大对象标志（单页） */
    unsigned int fullness:FULLNESS_BITS;       /* 丰满度（4 bit） */
    unsigned int class:CLASS_BITS + 1;         /* 所属 class 索引 */
    unsigned int magic:MAGIC_VAL_BITS;         /* 魔数校验 */
    unsigned int inuse;                        /* 已使用对象数 */
    unsigned int freeobj;                      /* 空闲对象链表头 */
    struct zpdesc *first_zpdesc;               /* 第一个 zpdesc */
    struct list_head list;                     /* 在 fullness_list 中的节点 */
    struct zs_pool *pool;                      /* 所属池 */
    struct zspage_lock zsl;                    /* 读写锁 */
};
```

**Zspage 布局**：zspage 可以包含 1 到 `ZS_MAX_PAGES_PER_ZSPAGE`（`CONFIG_ZSMALLOC_CHAIN_SIZE`，默认 4）个物理页面。页面通过 `zpdesc->next` 链接，形成一个单向链表，每个页面存储连续的对象。

#### 11.2.3 Size Class 组织

Zsmalloc 将对象大小划分为多个 size class，以平衡内部碎片和利用率：

- 类数量：`ZS_SIZE_CLASSES` = 255（4K 页面系统）
- 最小大小：`ZS_MIN_ALLOC_SIZE` = max(32, ...)
- 最大大小：`ZS_MAX_ALLOC_SIZE` = PAGE_SIZE
- 增量：`ZS_SIZE_CLASS_DELTA`
- 合并机制：如果相邻 class 的 pages_per_zspage 和 objs_per_zspage 相同，可以合并（`can_merge()`）

**Size Class 分配计算**（`zs_create_pool()`）：

```c
// 反向迭代，从大到小计算每个 class 的 pages_per_zspage 和 objs_per_zspage
for (i = ZS_SIZE_CLASSES - 1; i >= 0; i--) {
    size = ZS_MIN_ALLOC_SIZE + i * ZS_SIZE_CLASS_DELTA;
    // 计算最优的 pages_per_zspage 和 objs_per_zspage
    // 使得浪费空间最小化
    ...
}
```

#### 11.2.4 Zspage 布局与 Fullness 分组

**Fullness 分组**：zspage 按使用率（inuse/objs_per_zspage）分为多个 fullness 组：

```c
enum zs_fullness_group {
    ZS_INUSE_RATIO_0,     /* 0% */
    ZS_INUSE_RATIO_10,    /* 10% */
    ZS_INUSE_RATIO_20,    /* 20% */
    ...
    ZS_INUSE_RATIO_100,   /* 100% (full) */
    NR_FULLNESS_GROUPS,
};
```

每个 size_class 维护 `NR_FULLNESS_GROUPS` 个链表，zspage 根据当前使用率动态迁移。分配时优先从低使用率的 zspage 分配，释放时如果 zspage 变空则归还给 Buddy 系统。

**`init_zspage()`** 初始化新 zspage：
1. 遍历所有 zpdesc，为每个对象建立一个空闲对象链表（`link_free`）
2. 每个对象位置存储下一个空闲对象的索引
3. 最后一个对象标记为链表结束（`-1UL << OBJ_TAG_BITS`）

#### 11.2.5 Handle 机制

Zsmalloc 使用 Handle（句柄）而非直接指针来访问对象，这是支持页面迁移的关键：

```c
// Handle 编码：高位为 PFN（物理页框号），低位为对象索引
#define OBJ_INDEX_BITS  (BITS_PER_LONG - _PFN_BITS)
#define OBJ_INDEX_MASK  ((1UL << OBJ_INDEX_BITS) - 1)

// Handle → 对象位置（两步转换）
obj = handle_to_obj(handle);          // 去掉 Handle 自身的标记位
obj_to_location(obj, &zpdesc, &obj_idx);  // 解码为 zpdesc + 对象索引
```

**Handle 编码结构**（64 位系统，4K 页面，PFN_BITS = 52）：

```
Handle (64 bits):
  [63:52]  PFN_BITS (12 bits)  — 通常不使用，用于对齐
  [51:0]   编码后的对象位置
    其中:
      [51:12]  PFN (40 bits)   — 物理页面号（zpdesc 索引）
      [11:0]   对象索引 (12 bits) — 在 zspage 内的对象偏移
```

**Handle 分配**：`cache_alloc_handle()` 从 `handle_cachep`（kmem_cache）分配，确保 Handle 本身是 4 字节对齐的，低位 2 bit 可用于 OBJ_TAG。

**对象头部标记**：
```c
#define OBJ_ALLOCATED_TAG 1  // 最低位标记对象已分配
```

**`obj_malloc()` 内部实现**：

```c
static void obj_malloc(struct zs_pool *pool, struct zspage *zspage,
                       unsigned long handle)
{
    // 1. 从 zspage 的空闲链表中获取下一个空闲对象索引
    freeobj = get_freeobj(zspage);

    // 2. 计算空闲对象在物理页面中的位置
    //    class_size = 该 size class 的对象大小
    //    obj_offset = freeobj * class_size + 每个对象在页面内的偏移
    offset = offset_in_page(class_size * freeobj);

    // 3. 映射页面并写入 Handle 值
    //    写入 Handle 的目的是：释放时通过 Handle 可以反向定位到 zspage
    vaddr = kmap_local_zpdesc(f_zpdesc);
    link = (struct link_free *)(vaddr + offset);
    handle |= OBJ_ALLOCATED_TAG;  // 标记对象已分配
    link->next = handle;           // 在对象头部写入 Handle

    // 4. 更新 zspage 的空闲链表头
    //    从空闲对象的位置读取下一个空闲对象的索引
    freeobj = link->next >> OBJ_TAG_BITS;
    set_freeobj(zspage, freeobj);

    // 5. 更新使用计数
    zspage->inuse++;
    kunmap_local(vaddr);
}
```

**关键设计**：
- **Handle 写入对象头部**：每个对象的前几个字节用于存储 Handle 值，这样释放时通过 Handle 可以直接定位到 zspage 和 class
- **空闲链表**：`zspage->freeobj` 指向下一个空闲对象索引，空闲对象之间通过 `link->next` 链接，形成隐式链表
- **`OBJ_ALLOCATED_TAG`**：标记该对象已分配，释放时通过 `obj_free()` 清除该标记并将对象插回空闲链表

#### 11.2.6 对象分配：zs_malloc

`zs_malloc()` 从池中分配一个指定大小的对象：

```c
unsigned long zs_malloc(struct zs_pool *pool, size_t size, gfp_t gfp, int nid)
{
    // 1. 分配 Handle
    handle = cache_alloc_handle(gfp);
    if (!handle) return ERR_PTR(-ENOMEM);

    // 2. 查找对应的 Size Class（size + ZS_HANDLE_SIZE）
    size += ZS_HANDLE_SIZE;
    class = pool->size_class[get_size_class_index(size)];

    // 3. 从现有 zspage 分配
    spin_lock(&class->lock);
    zspage = find_get_zspage(class);  // 找到有空间的 zspage
    if (zspage) {
        obj_malloc(pool, zspage, handle);  // 从空闲链表取一个对象
        fix_fullness_group(class, zspage); // 更新 fullness 分组
        goto out;
    }
    spin_unlock(&class->lock);

    // 4. 没有可用 zspage，分配新页面
    zspage = alloc_zspage(pool, class, gfp, nid);
    if (!zspage) { ... }  // 失败

    // 5. 在新 zspage 上分配
    spin_lock(&class->lock);
    obj_malloc(pool, zspage, handle);
    insert_zspage(class, zspage, ZS_INUSE_RATIO_10);
    ...
}
```

**`obj_malloc()`** 内部流程：
1. 从 `zspage->freeobj` 获取空闲对象索引
2. 计算对象在物理页面中的偏移量
3. 读取下一个空闲对象索引，更新 `zspage->freeobj`
4. 在对象头部写入 Handle 值（用于释放时反向查找）
5. 标记 `OBJ_ALLOCATED_TAG`

#### 11.2.7 对象释放：zs_free

`zs_free()` 释放一个 Handle 指定的对象：

```c
void zs_free(struct zs_pool *pool, unsigned long handle)
{
    // 1. 从 Handle 定位 zspage 和 class
    read_lock(&pool->lock);  // 保护迁移
    obj = handle_to_obj(handle);
    obj_to_zpdesc(obj, &f_zpdesc);
    zspage = get_zspage(f_zpdesc);
    class = zspage_class(pool, zspage);
    spin_lock(&class->lock);
    read_unlock(&pool->lock);

    // 2. 释放对象
    obj_free(class->size, obj);  // 将对象插回空闲链表

    // 3. 更新 fullness
    fullness = fix_fullness_group(class, zspage);
    if (fullness == ZS_INUSE_RATIO_0)
        free_zspage(pool, class, zspage);  // 空 zspage 归还

    spin_unlock(&class->lock);
    cache_free_handle(handle);
}
```

**`obj_free()`** 将对象插回 zspage 的空闲链表，更新 `zspage->freeobj`。

#### 11.2.8 对象映射读写

Zsmalloc 提供 `zs_obj_read_begin/end` 和 `zs_obj_write` 接口用于读写对象数据：

**`zs_obj_read_begin()`** 读取流程：
1. 从 Handle 获取 `zspage` 和 `zpdesc`
2. 获取读锁（`zspage_read_lock()`），防止迁移期间移动页面
3. 计算对象在页面内的偏移量
4. 如果对象完全在一个页面内：`kmap_local_zpdesc()` 直接映射
5. 如果对象跨两个页面：`memcpy_from_page()` 将数据拷贝到 `local_copy` 缓冲区

**`zs_obj_write()`** 写入流程：
1. 获取读锁（`zspage_read_lock()`）
2. 计算偏移量
3. 如果对象完全在一个页面内：`kmap_local_zpdesc()` 直接写入
4. 如果对象跨两个页面：`memcpy_to_page()` 分别写入两个页面

#### 11.2.9 压缩与页面迁移支持

Zsmalloc 支持两种形式的压缩：

**内部压缩（`zs_compact()`）**：
- 遍历所有 size class，找到 `OBJ_ALLOCATED` 少（即 inuse 低）的 zspage
- 将对象迁移到使用率更高的 zspage 中
- 释放完全空闲的 zspage 回 Buddy 系统

```c
static unsigned long zs_can_compact(struct size_class *class)
{
    // 计算可以释放的页面数
    // = total_pages - ceil(objs_allocated / objs_per_zspage) * pages_per_zspage
    unsigned long total_pages = 0;
    for (i = 0; i < NR_FULLNESS_GROUPS; i++) {
        // 遍历每个 fullness 组的 zspage
        // 统计总的页面数
    }
    // 减去最低需要的页面数（所有对象紧凑排列所需的最小页面数）
    obj_wasted = class_stat_get(class, ZS_OBJS_ALLOCATED) % class->objs_per_zspage;
    if (obj_wasted)
        total_pages -= obj_wasted;
    return total_pages;
}

static void zs_compact(struct zs_pool *pool, struct size_class *class)
{
    // 从低使用率 group（ZS_INUSE_RATIO_0/10/20）开始遍历
    // 将对象从 src_zspage 逐个迁移到 dst_zspage
    // 每次迁移后更新 fullness 分组
    // 当 src_zspage 变空时，释放回 Buddy 系统
    struct zspage *src_zspage, *dst_zspage;

    // 遍历所有非 FULL 的 fullness 组
    for (fg = ZS_INUSE_RATIO_0; fg < ZS_INUSE_RATIO_100; fg++) {
        // 对每个 src_zspage，尝试迁移其所有对象
        // 找到有空闲空间的 dst_zspage
        // 使用 obj_malloc/obj_free 进行对象级迁移
        // 迁移完成后更新 fullness 统计
    }
}
```

**压缩流程**：
1. 从 `ZS_INUSE_RATIO_0` 开始，找到使用率最低的 zspage（`src_zspage`）
2. 在同一个 size class 中寻找有空闲空间的 zspage（`dst_zspage`）
3. 对 `src_zspage` 中的每个已分配对象，调用 `obj_malloc()` 在 `dst_zspage` 中分配，然后拷贝数据
4. 释放 `src_zspage` 中的对象（`obj_free()`）
5. 当 `src_zspage` 的 `inuse` 降到 0 时，释放整个 zspage 回 Buddy 系统
6. 继续处理下一个低使用率 zspage

**页面迁移支持**：通过 `set_movable_ops()` 注册 `zsmalloc_mops`，使 Zsmalloc 的页面支持内核的通用页面迁移机制：

```c
static const struct movable_operations zsmalloc_mops = {
    .isolate_page = zs_page_isolate,
    .migrate_page = zs_page_migrate,
    .putback_page = zs_page_putback,
};
```

- `zs_page_isolate()`：检查页面是否可迁移（没有正在进行的 I/O，没有被锁定）
- `zs_page_migrate()`：更新 zspage 内的 zpdesc 指针，将 Handle 重定向到新页面
- `zs_page_putback()`：迁移失败时恢复

#### 11.2.10 Shrinker 接口

Zsmalloc 注册 shrinker 以响应系统内存压力：

```c
static unsigned long zs_shrinker_scan(struct shrinker *shrinker,
        struct shrink_control *sc)
{
    // 对每个 size class 执行 zs_compact()
    for (i = ZS_SIZE_CLASSES - 1; i >= 0; i--) {
        class = pool->size_class[i];
        pages_freed += zs_compact(class);  // 压缩并释放页面
    }
    return pages_freed;
}
```

#### 11.2.11 统计信息

通过 `zs_pool_stats()` 获取性能和状态信息：

```c
struct zs_pool_stats {
    unsigned long pages_compacted;  /* 压缩释放的页面数 */
};
```

`/sys/kernel/debug/zsmalloc/` 下有详细的统计输出，包括每个 size class 的：
- class 索引、对象大小
- 各 fullness 组的 zspage 数量
- 已分配对象数、已使用对象数、使用的页面数
- pages_per_zspage
- 可释放的页面数

#### 11.2.12 初始化与销毁

**`zs_init()`** 模块初始化：
1. 创建 handle 和 zspage 的 kmem_cache
2. 注册 `zsmalloc_mops` 移动操作接口（使 Zsmalloc 页面可被内核迁移）
3. 初始化统计目录

**`zs_create_pool()`** 池创建：
1. 分配 `zs_pool` 结构体
2. 初始化 lock、work 结构
3. 反向迭代创建所有 size class（带合并优化）
4. 创建统计目录
5. 注册 shrinker

**`zs_destroy_pool()`** 池销毁：
1. 释放所有 size class 中的 zspage
2. 释放 size class 结构
3. 注销 shrinker
4. 释放池名称和统计目录

---

## Part III: 内核对象与页缓存

## 12. 文件页缓存（Page Cache）

### 12.1 概述

文件：`mm/filemap.c`（4,829 行），`mm/readahead.c`（841 行）

Page Cache 是 Linux 文件 I/O 的核心机制，通过 `address_space` 管理文件数据与内存页面的映射。它使用 **XArray** 实现高效的页面查找，通过 **address_space_operations** 接口与具体文件系统解耦。

### 12.2 核心数据结构

#### 12.2.1 address_space

`include/linux/fs.h` 中定义：

```c
struct address_space {
    struct inode              *host;              // 关联的 inode
    struct xarray             i_pages;            // 页面缓存 XArray 树
    struct rw_semaphore       invalidate_lock;    // 缓存失效锁
    gfp_t                     gfp_mask;           // 页面分配 GFP 标志
    atomic_t                  i_mmap_writable;    // VM_SHARED 映射计数
    struct rb_root_cached     i_mmap;             // 反向映射红黑树
    unsigned long             nrpages;            // 页面总数
    pgoff_t                   writeback_index;    // 写回起始位置
    const struct address_space_operations *a_ops; // 文件系统操作表
    unsigned long             flags;              // AS_* 标志位
    errseq_t                  wb_err;             // 最近的写回错误
    spinlock_t                i_private_lock;     // 私有数据锁
    struct list_head          i_private_list;     // 私有数据链表
    struct rw_semaphore       i_mmap_rwsem;       // 反向映射锁
    void                     *i_private_data;     // 私有数据指针
} __randomize_layout;
```

关键字段说明：
- **`i_pages`**：XArray 树，存储所有缓存页面，以 `pgoff_t`（文件偏移 >> PAGE_SHIFT）为索引
- **`i_mmap`**：红黑树，管理所有映射到此 address_space 的 VMA，用于反向映射
- **`invalidate_lock`**：保护文件偏移到磁盘块映射的一致性，shared 模式用于读，exclusive 模式用于 truncate/invalidate
- **`flags`**：包括 `AS_EIO`（I/O 错误）、`AS_ENOSPC`（空间不足）等
- **`writeback_index`**：增量写回起始位置，实现循环写回

#### 12.2.2 address_space_operations

```c
struct address_space_operations {
    int      (*read_folio)(struct file *, struct folio *);
    int      (*writepages)(struct address_space *, struct writeback_control *);
    bool     (*dirty_folio)(struct address_space *, struct folio *);
    void     (*readahead)(struct readahead_control *);
    int      (*write_begin)(const struct kiocb *, struct address_space *,
                             loff_t, unsigned, struct folio **, void **);
    int      (*write_end)(const struct kiocb *, struct address_space *,
                           loff_t, unsigned, unsigned, struct folio *, void *);
    void     (*invalidate_folio)(struct folio *, size_t, size_t);
    bool     (*release_folio)(struct folio *, gfp_t);
    void     (*free_folio)(struct folio *);
    int      (*migrate_folio)(struct address_space *, struct folio *,
                               struct folio *, enum migrate_mode);
    int      (*launder_folio)(struct folio *);
    // ... 其他回调
};
```

#### 12.2.3 XArray Tags

Page Cache 使用 XArray 的 tag 机制标记页面状态：

```c
#define PAGECACHE_TAG_DIRTY      XA_MARK_0   // 脏页标记
#define PAGECACHE_TAG_WRITEBACK  XA_MARK_1   // 写回中标记
#define PAGECACHE_TAG_TOWRITE    XA_MARK_2   // 待写回标记（写回期间使用）
```

### 12.3 读路径

#### 12.3.1 filemap_read 主流程

```c
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter,
                     ssize_t already_read)
{
    do {
        // 1. 获取页面批次
        error = filemap_get_pages(iocb, iter->count, &fbatch, false);

        // 2. 检查文件大小边界
        isize = i_size_read(inode);
        end_offset = min(isize, iocb->ki_pos + iter->count);

        // 3. 批量拷贝数据到用户空间
        for (i = 0; i < folio_batch_count(&fbatch); i++) {
            copied = copy_folio_to_iter(folio, offset, bytes, iter);
            already_read += copied;
            iocb->ki_pos += copied;
        }
    } while (iov_iter_count(iter));
}
```

**关键行为**：
- 循环读取，每次处理一个 `folio_batch`（一组连续页面）
- 异步读取（`IOCB_WAITQ`）一旦拷贝了数据，降级为 `IOCB_NOWAIT`
- 对每个页面调用 `folio_mark_accessed()` 通知 LRU 管理器
- 可写映射时执行 `flush_dcache_folio()` 处理缓存一致性

#### 12.3.2 filemap_get_pages 页面获取

```c
static int filemap_get_pages(struct kiocb *iocb, size_t count,
                             struct folio_batch *fbatch, bool need_uptodate)
{
retry:
    // 1. 从 XArray 批量查找已缓存的页面
    filemap_get_read_batch(mapping, index, last_index - 1, fbatch);

    // 2. 未命中时触发同步预读
    if (!folio_batch_count(fbatch)) {
        page_cache_sync_ra(&ractl, last_index - index);
        filemap_get_read_batch(mapping, index, last_index - 1, fbatch);
    }

    // 3. 仍未命中（XArray 中无页面），创建新页面
    if (!folio_batch_count(fbatch)) {
        err = filemap_create_folio(iocb, fbatch);
        if (err == AOP_TRUNCATED_PAGE)
            goto retry;
    }

    // 4. 检查最后一个页面是否有 PG_readahead 标记（触发异步预读）
    if (folio_test_readahead(folio))
        filemap_readahead(iocb, filp, mapping, folio, last_index);

    // 5. 确保页面内容是最新的（Uptodate）
    if (!folio_test_uptodate(folio))
        err = filemap_update_page(iocb, mapping, count, folio, need_uptodate);
}
```

**FGP 标志**：`filemap_get_read_batch` 内部使用 `FGP_ACCESSED | FGP_LOCK | FGP_HEAD` 等标志控制查找行为。

#### 12.3.3 页面创建与读取

```
filemap_create_folio(iocb, fbatch)
  └─ filemap_alloc_folio()          // 分配 folio（从 Buddy 系统）
  └─ filemap_add_folio()            // 加入 XArray（需持有 invalidate_lock shared）
  └─ filemap_read_folio()           // 读取磁盘数据
       └─ a_ops->read_folio()       // 文件系统特定读取
```

### 12.4 预读（Readahead）

文件：`mm/readahead.c`

预读将数据提前读入 Page Cache，隐藏磁盘 I/O 延迟。

#### 12.4.1 同步预读 page_cache_sync_ra

```c
void page_cache_sync_ra(struct readahead_control *ractl, unsigned long req_count)
{
    max_pages = ractl_max_pages(ractl, req_count);
    prev_index = ra->prev_pos >> PAGE_SHIFT;

    // 1. 强制随机读：直接 force_page_cache_ra（仅读请求页面）
    if (do_forced_ra) {
        force_page_cache_ra(ractl, req_count);
        return;
    }

    // 2. 顺序读检测：文件开头、超大请求、连续缺页
    if (!index || req_count > max_pages || index - prev_index <= 1UL) {
        ra->start = index;
        ra->size = get_init_ra_size(req_count, max_pages);
        ra->async_size = ra->size > req_count ? ra->size - req_count : ra->size >> 1;
        goto readit;
    }

    // 3. 查询 page cache 历史足迹
    miss = page_cache_prev_miss(mapping, index - 1, max_pages);
    contig_count = index - miss - 1;

    // 4. 独立随机读：不污染预读状态
    if (contig_count <= req_count) {
        do_page_cache_ra(ractl, req_count, 0);
        return;
    }

    // 5. 文件从头缓存：放大 contig_count
    if (miss == ULONG_MAX)
        contig_count *= 2;

    ra->start = index;
    ra->size = min(contig_count + req_count, max_pages);
    ra->async_size = 1;
readit:
    page_cache_ra_order(ractl, ra);
}
```

#### 12.4.2 初始窗口大小

```c
static unsigned long get_init_ra_size(unsigned long size, unsigned long max)
{
    unsigned long newsize = roundup_pow_of_two(size);

    if (newsize <= max / 32)        newsize = newsize * 4;  // 1-2 page → 16k
    else if (newsize <= max / 4)    newsize = newsize * 2;  // 3-4 page → 32k
    else                            newsize = max;           // > 8 page → 128k
    return newsize;
}
```

#### 12.4.3 窗口增长策略

```c
static unsigned long get_next_ra_size(struct file_ra_state *ra, unsigned long max)
{
    unsigned long cur = ra->size;

    if (cur < max / 16)    return 4 * cur;   // 小窗口：4 倍增长
    if (cur <= max / 2)    return 2 * cur;   // 中等窗口：2 倍增长
    return max;                                // 达到上限
}
```

#### 12.4.4 异步预读 page_cache_async_ra

当读路径发现页面有 `PG_readahead` 标记时触发：

```c
void page_cache_async_ra(struct readahead_control *ractl,
                         struct folio *folio, unsigned long req_count)
{
    // 1. 检查期望的回调索引
    expected = round_down(ra->start + ra->size - ra->async_size, folio_nr_pages(folio));

    // 2. 顺序访问：扩大窗口
    if (index == expected) {
        ra->start += ra->size;
        ra->size = max(ra->size, get_next_ra_size(ra, max_pages));
        goto readit;
    }

    // 3. 非顺序访问：缩小窗口，重设为初始大小
    ra->start = index;
    ra->size = get_init_ra_size(req_count, max_pages);
    ra->async_size = ra->size >> 1;
readit:
    page_cache_ra_order(ractl, ra);
}
```

#### 12.4.5 read_pages 提交 I/O

```c
static void read_pages(struct readahead_control *rac)
{
    if (aops->readahead) {
        aops->readahead(rac);  // 文件系统批量提交
        // 清理残留页面
    } else {
        while ((folio = readahead_folio(rac)))
            aops->read_folio(rac->file, folio);  // 逐个回退
    }
}
```

### 12.5 写路径

#### 12.5.1 generic_perform_write 主流程

```c
ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *i)
{
    do {
        // 1. 限速检查
        balance_dirty_pages_ratelimited(mapping);

        // 2. 写前准备
        status = a_ops->write_begin(iocb, mapping, pos, bytes, &folio, &fsdata);

        // 3. 原子拷贝用户数据
        copied = copy_folio_from_iter_atomic(folio, offset, bytes, i);

        // 4. 写完成处理
        status = a_ops->write_end(iocb, mapping, pos, bytes, copied, folio, fsdata);

        // 5. 处理短拷贝（chunk 减半重试）
        if (unlikely(status == 0)) {
            if (chunk > PAGE_SIZE)
                chunk /= 2;
            if (copied) {
                bytes = copied;
                goto retry;  // 重试写入
            }
        }
    } while (iov_iter_count(i));
}
```

**关键设计**：
- 使用 `copy_folio_from_iter_atomic()` 而非 `copy_from_iter()`，避免在持有 folio 锁时递归进入 page fault，防止死锁
- `write_begin`/`write_end` 两个回调分离，允许文件系统在写入前后执行元数据操作
- `balance_dirty_pages_ratelimited()` 每页写入前检查脏页限速

#### 12.5.2 写路径典型流程

```
generic_perform_write()
  ├─ balance_dirty_pages_ratelimited()     // 脏页限速
  ├─ a_ops->write_begin()                  // 获取 folio（缓存未命中则分配）
  │    └─ grab_cache_page_write_begin()
  │         ├─ filemap_grab_folio()         // XArray 查找或分配
  │         └─ __folio_start_writeback()    // 若被写回，等待完成
  ├─ copy_folio_from_iter_atomic()         // 用户数据拷贝
  ├─ a_ops->write_end()                    // 写完成
  │    ├─ __folio_mark_dirty()             // 标记脏页
  │    └─ folio_mark_accessed()            // 更新 LRU 访问位
  └─ (循环)
```

### 12.6 XArray 操作

Page Cache 的核心数据操作：

| 操作 | 函数 | 说明 |
|------|------|------|
| 查找 | `filemap_get_folio()` | 按 index 查找 folio |
| 批量查找 | `filemap_get_read_batch()` | 批量获取连续页面 |
| 添加 | `filemap_add_folio()` | 将 folio 加入 XArray |
| 删除 | `filemap_remove_folio()` | 从 XArray 移除 |
| 标记脏 | `__folio_mark_dirty()` | 设置 `PAGECACHE_TAG_DIRTY` |
| 标记写回 | `folio_start_writeback()` | 设置 `PAGECACHE_TAG_WRITEBACK` |
| 等待写回 | `folio_wait_writeback()` | 等待写回完成 |

---

## 13. 写回机制（Writeback）

### 13.1 概述

文件：`mm/page-writeback.c`（3,114 行），`fs/fs-writeback.c`（2,500+ 行），`mm/backing-dev.c`（1,222 行）

写回机制负责将脏页数据写回持久存储设备，通过多级阈值控制和动态调速算法平衡内存使用与 I/O 带宽。

### 13.2 核心数据结构

#### 13.2.1 backing_dev_info

```c
struct backing_dev_info {
    struct list_head          bdi_list;        // 全局 BDI 链表
    struct bdi_writeback      wb;              // 默认写回控制
    unsigned long             ra_pages;        // 最大预读页数
    unsigned long             io_pages;        // 最优 I/O 大小（页）
    unsigned long             capabilities;    // BDI_CAP_* 标志
    struct device            *dev;             // 关联块设备
};
```

#### 13.2.2 bdi_writeback

```c
struct bdi_writeback {
    struct list_head          b_dirty;          // 脏页 inode 链表
    struct list_head          b_io;             // 正在进行 I/O 的 inode 链表
    struct list_head          b_more_io;        // 等待更多 I/O 的 inode 链表
    unsigned long             nr_pages_dirty;   // 脏页计数（估算）
    unsigned long             last_old_flush;   // 上次过期刷新时间
    struct delayed_work       dwork;            // 定时写回工作项
    unsigned long             dirty_ratelimit;  // 当前脏页速率限制（页/秒）
    unsigned long             dirty_exceeded;   // 是否超过阈值
    unsigned long             bw_time_stamp;    // 带宽估算时间戳
};
```

**三链表机制**：
- `b_dirty`：所有脏 inode，按过期时间排序
- `b_io`：当前正在写回的 inode 子集
- `b_more_io`：本轮写回未完成的 inode，等待下一轮

### 13.3 脏页限速算法

#### 13.3.1 balance_dirty_pages_ratelimited_flags

每页写入前调用，通过速率限制减少性能开销：

```c
int balance_dirty_pages_ratelimited_flags(struct address_space *mapping,
                                          unsigned int flags)
{
    ratelimit = current->nr_dirtied_pause;
    if (wb->dirty_exceeded)
        ratelimit = min(ratelimit, 32 >> (PAGE_SHIFT - 10));  // 超过阈值时收紧

    // 1. Per-CPU 速率限制，防止多任务同时触发
    p = this_cpu_ptr(&bdp_ratelimits);
    if (unlikely(current->nr_dirtied >= ratelimit))
        *p = 0;
    else if (unlikely(*p >= ratelimit_pages)) {
        *p = 0;
        ratelimit = 0;  // 强制触发 balance_dirty_pages
    }

    // 2. 继承已退出任务的脏页泄漏
    p = this_cpu_ptr(&dirty_throttle_leaks);
    if (*p > 0 && current->nr_dirtied < ratelimit) {
        nr_pages_dirtied = min(*p, ratelimit - current->nr_dirtied);
        *p -= nr_pages_dirtied;
        current->nr_dirtied += nr_pages_dirtied;
    }

    // 3. 达到阈值时调用真正的限速函数
    if (unlikely(current->nr_dirtied >= ratelimit))
        ret = balance_dirty_pages(wb, current->nr_dirtied, flags);
}
```

#### 13.3.2 balance_dirty_pages 核心限速

```c
static int balance_dirty_pages(struct bdi_writeback *wb,
                               unsigned long pages_dirtied, unsigned int flags)
{
    for (;;) {
        nr_dirty = global_node_page_state(NR_FILE_DIRTY);

        // 1. 计算全局和 memcg 域的脏页阈值
        balance_domain_limits(gdtc, strictlimit);
        if (mdtc)
            balance_domain_limits(mdtc, strictlimit);

        // 2. 超过后台阈值时启动写回
        if (nr_dirty > gdtc->bg_thresh && !writeback_in_progress(wb))
            wb_start_background_writeback(wb);

        // 3. Free-run 区间：未超过阈值，计算下次轮询间隔
        if (gdtc->freerun && (!mdtc || mdtc->freerun)) {
            current->nr_dirtied_pause = min(intv, mdtc_intv);
            break;
        }

        // 4. 计算 pos_ratio（位置比例），选择最严格的域
        balance_wb_limits(gdtc, strictlimit);
        if (mdtc && mdtc->pos_ratio < gdtc->pos_ratio)
            sdtc = mdtc;
        else
            sdtc = gdtc;

        // 5. 定时更新带宽估算
        __wb_update_bandwidth(gdtc, mdtc, true);

        // 6. 计算睡眠时间
        dirty_ratelimit = wb->dirty_ratelimit;
        task_ratelimit = dirty_ratelimit * sdtc->pos_ratio >> RATELIMIT_CALC_SHIFT;
        max_pause = wb_max_pause(wb, sdtc->wb_dirty);
        min_pause = wb_min_pause(wb, max_pause, task_ratelimit,
                                  dirty_ratelimit, &nr_dirtied_pause);

        period = HZ * pages_dirtied / task_ratelimit;
        pause = period;

        // 7. 睡眠限速
        if (pause >= min_pause && pause <= max_pause) {
            __set_current_state(TASK_KILLABLE);
            io_schedule_timeout(pause);
            current->nr_dirtied = 0;
            current->nr_dirtied_pause = nr_dirtied_pause;
        }
    }
}
```

**限速算法核心**：
- **pos_ratio**：当前脏页量相对于阈值的比例，范围 [0, 1]，越接近阈值越小
- **task_ratelimit**：`dirty_ratelimit × pos_ratio`，限制任务脏页速率
- **带宽自适应**：`__wb_update_bandwidth()` 每 `BANDWIDTH_INTERVAL` 更新 `dirty_ratelimit`
- **睡眠时间**：`pause = dirtied_pages / task_ratelimit`，最小 `min_pause`，最大 `max_pause`

### 13.4 写回触发条件

| 触发条件 | 触发路径 | 说明 |
|----------|----------|------|
| **后台阈值** | `balance_dirty_pages()` → `wb_start_background_writeback()` | nr_dirty > bg_thresh |
| **同步限速** | `balance_dirty_pages()` → 睡眠等待 | 超过 dirty_thresh 时强制限速 |
| **定时写回** | `wb_workfn()` → `wb_do_writeback()` | 默认每 5 秒（dirty_writeback_interval） |
| **脏页过期** | `wb_workfn()` → `wb_writeback()` (for_kupdate) | 默认 30 秒（dirty_expire_interval） |
| **显式同步** | `sync()` / `fsync()` → `wakeup_flusher_threads()` | 系统调用显式触发 |
| **内存回收** | `shrink_folio_list()` → `folio_wait_writeback()` | 回收脏页前等待写回完成 |

### 13.5 写回执行流程

#### 13.5.1 wb_workfn 定时写回

```c
void wb_workfn(struct work_struct *work)
{
    do {
        pages_written = wb_do_writeback(wb);  // 执行写回
    } while (!list_empty(&wb->work_list));

    if (!list_empty(&wb->work_list))
        wb_wakeup(wb);           // 立即唤醒
    else if (wb_has_dirty_io(wb) && dirty_writeback_interval)
        wb_wakeup_delayed(wb);   // 延迟唤醒（5 秒后）
}
```

#### 13.5.2 wb_writeback 核心循环

```c
static long wb_writeback(struct bdi_writeback *wb, struct wb_writeback_work *work)
{
    for (;;) {
        // 1. 检查停止条件
        if (work->nr_pages <= 0) break;
        if ((work->for_background || work->for_kupdate) &&
            !list_empty(&wb->work_list)) break;  // 让给其他工作
        if (work->for_background && !wb_over_bg_thresh(wb)) break;

        // 2. 准备 I/O 队列
        if (list_empty(&wb->b_io)) {
            if (work->for_kupdate)
                dirtied_before = jiffies - (dirty_expire_interval * 10);
            else if (work->for_background)
                dirtied_before = jiffies;
            queue_io(wb, work, dirtied_before);  // b_dirty → b_io
        }

        // 3. 执行写回
        if (work->sb)
            progress = writeback_sb_inodes(work->sb, wb, work);
        else
            progress = __writeback_inodes_wb(wb, work);

        // 4. 未完成处理
        if (!progress && !list_empty(&wb->b_more_io)) {
            inode = wb_inode(wb->b_more_io.prev);
            inode_sleep_on_writeback(inode);  // 等待 inode 可写回
        }
    }
}
```

**写回工作类型**：

| 工作类型 | work->for_* 标志 | 行为 |
|----------|-----------------|------|
| 后台写回 | `for_background` | 写回直到低于 bg_thresh |
| 过期写回 | `for_kupdate` | 写回所有 dirty_expire 时间以上的脏页 |
| 同步写回 | `for_sync` | 写回所有脏页，直到页数/时间限制 |
| 显式范围 | `sb` 指定 | 写回特定超级块的所有脏页 |

### 13.6 阈值控制参数

```c
// /proc/sys/vm/ 相关参数
dirty_background_ratio = 10;     // 后台写回触发：总内存的 10%
dirty_ratio = 20;                // 同步限速触发：总内存的 20%
dirty_background_bytes = 0;      // 后台写回字节阈值（与 ratio 互斥）
dirty_bytes = 0;                 // 同步限速字节阈值（与 ratio 互斥）
dirty_expire_centisecs = 3000;   // 脏页过期时间（30 秒，1/100 秒为单位）
dirty_writeback_centisecs = 500; // 写回线程唤醒周期（5 秒）
```

**双域控制**：同时支持 **全局域**（系统级）和 **memcg 域**（cgroup 级），取两者中更严格的 `pos_ratio`。

---

## 14. Mempool 内存池

### 14.1 概述

文件：`mm/mempool.c`（468 行）

Mempool 是一种内存分配可靠性保障机制，在正常内存分配失败时提供预留的后备对象，确保关键路径上的分配不会失败。它不提供独立的分配算法，而是包装现有的分配器（slab、页分配器等）。

### 14.2 核心数据结构

```c
typedef struct mempool {
    spinlock_t          lock;       // 保护 elements 数组的自旋锁
    int                 min_nr;     // 最小预留元素数量
    int                 curr_nr;    // 当前池中元素数量
    void              **elements;   // 元素指针数组（栈结构）
    void               *pool_data;  // 传递给 alloc/free 的私有数据
    mempool_alloc_t    *alloc;      // 后备分配函数
    mempool_free_t     *free;       // 后备释放函数
    wait_queue_head_t   wait;       // 等待队列（元素可用时唤醒）
} mempool_t;
```

**设计要点**：
- `elements` 数组作为 LIFO 栈，使用 `remove_element`（取 `elements[--curr_nr]`）和 `add_element`（放 `elements[curr_nr++]`）
- 不维护自己的内存池，而是通过 `alloc`/`free` 回调依赖底层分配器
- `min_nr` 为 0 时仍预分配 1 个元素，保证至少有一个后备

### 14.3 创建与初始化

#### 14.3.1 mempool_create

```c
struct mempool *mempool_create_node_noprof(int min_nr, mempool_alloc_t *alloc_fn,
        mempool_free_t *free_fn, void *pool_data, gfp_t gfp_mask, int node_id)
{
    pool = kmalloc_node(sizeof(*pool), gfp_mask | __GFP_ZERO, node_id);
    if (!pool) return NULL;

    if (mempool_init_node(pool, min_nr, alloc_fn, free_fn, pool_data,
                          gfp_mask, node_id)) {
        kfree(pool);
        return NULL;
    }
    return pool;
}
```

#### 14.3.2 mempool_init_node

```c
int mempool_init_node(struct mempool *pool, int min_nr, ...)
{
    // 1. 分配 elements 指针数组
    pool->elements = kmalloc_array_node(max(1, min_nr), sizeof(void *), gfp_mask, node_id);
    if (!pool->elements) return -ENOMEM;

    // 2. 预分配 min_nr 个元素
    while (pool->curr_nr < max(1, pool->min_nr)) {
        element = pool->alloc(gfp_mask, pool->pool_data);
        if (!element) {
            mempool_exit(pool);
            return -ENOMEM;
        }
        add_element(pool, element);
    }
    return 0;
}
```

### 14.4 分配路径

#### 14.4.1 mempool_alloc_noprof

```c
void *mempool_alloc_noprof(struct mempool *pool, gfp_t gfp_mask)
{
    gfp_temp = mempool_adjust_gfp(&gfp_mask);  // 第一轮去除非 __GFP_DIRECT_RECLAIM

repeat_alloc:
    // 1. 优先尝试正常分配（通过底层 alloc 回调）
    element = pool->alloc(gfp_temp, pool->pool_data);

    if (unlikely(!element)) {
        // 2. 正常分配失败，从池中取后备
        if (!mempool_alloc_from_pool(pool, &element, 1, 0, gfp_temp)) {
            // 3. 第一轮（无 __GFP_DIRECT_RECLAIM）失败，重试带回收
            if (gfp_temp != gfp_mask) {
                gfp_temp = gfp_mask;
                goto repeat_alloc;
            }
            // 4. 允许回收则重试
            if (gfp_mask & __GFP_DIRECT_RECLAIM)
                goto repeat_alloc;
        }
    }
    return element;
}
```

**分配策略层级**：
1. 正常分配（`pool->alloc(gfp_temp, ...)`）
2. 从池中取预分配元素（`mempool_alloc_from_pool`）
3. 池空且允许回收 → 重试正常分配
4. 池空且不允许回收 → 返回 NULL

#### 14.4.2 mempool_alloc_from_pool

```c
static unsigned int mempool_alloc_from_pool(struct mempool *pool, void **elems,
        unsigned int count, unsigned int allocated, gfp_t gfp_mask)
{
    spin_lock_irqsave(&pool->lock, flags);
    if (unlikely(pool->curr_nr < count - allocated))
        goto fail;  // 池中元素不足

    for (i = 0; i < count; i++) {
        if (!elems[i]) {
            elems[i] = remove_element(pool);  // LIFO 取元素
            allocated++;
        }
    }
    spin_unlock_irqrestore(&pool->lock, flags);
    smp_wmb();  // 与 mempool_free 的 rmb 配对
    return allocated;

fail:
    if (gfp_mask & __GFP_DIRECT_RECLAIM) {
        // 等待其他线程释放元素，超时 5 秒
        io_schedule_timeout(5 * HZ);
    }
    return allocated;
}
```

#### 14.4.3 mempool_adjust_gfp 标志调整

```c
static inline gfp_t mempool_adjust_gfp(gfp_t *gfp_mask)
{
    // 第一轮：清除 __GFP_DIRECT_RECLAIM，避免正常分配与池争抢内存
    gfp_temp = *gfp_mask & ~__GFP_DIRECT_RECLAIM;
    *gfp_mask &= ~__GFP_ZERO;  // 禁止 __GFP_ZERO（池中元素已初始化）
    return gfp_temp;
}
```

### 14.5 释放路径

```c
void mempool_free(void *element, struct mempool *pool)
{
    if (likely(element) && !mempool_free_bulk(pool, &element, 1))
        pool->free(element, pool->pool_data);  // 池满 → 正常释放
}

unsigned int mempool_free_bulk(struct mempool *pool, void **elems,
                               unsigned int count)
{
    spin_lock_irqsave(&pool->lock, flags);
    for (i = 0; i < count; i++) {
        if (pool->curr_nr < pool->min_nr) {
            add_element(pool, elems[i]);  // 池未满 → 放回池
            elems[i] = NULL;
            freed++;
        }
    }
    if (freed)
        wake_up_all(&pool->wait);  // 唤醒等待分配的线程
    spin_unlock_irqrestore(&pool->lock, flags);
    return freed;
}
```

### 14.6 常用 Helper 函数

| Helper | 底层分配器 | pool_data 含义 |
|--------|-----------|---------------|
| `mempool_alloc_slab` / `mempool_free_slab` | `kmem_cache_alloc` | `struct kmem_cache *` |
| `mempool_kmalloc` / `mempool_kfree` | `kmalloc` | `size_t`（分配大小） |
| `mempool_alloc_pages` / `mempool_free_pages` | `alloc_pages` | `int`（order） |
| `mempool_alloc_pages_io` | `alloc_pages`（GFP_IO） | `int`（order） |

### 14.7 典型使用场景

```c
// 示例：块设备 I/O 请求的内存池
static struct kmem_cache *blk_request_cachep;
mempool_t blk_request_pool;

mempool_init(&blk_request_pool, 128,
             mempool_alloc_slab, mempool_free_slab,
             blk_request_cachep);

// 分配（优先从 slab 分配，失败时从池取）
req = mempool_alloc(&blk_request_pool, GFP_NOIO);

// 释放（优先放回池，池满时还给 slab）
mempool_free(req, &blk_request_pool);
```

---

## 15. Per-CPU 分配器

### 15.1 概述

文件：`mm/percpu.c`（3,388 行），`mm/percpu-internal.h`

Per-CPU 分配器为每个 CPU 分配独立的数据副本，消除锁竞争和缓存伪共享（false sharing）。它管理静态（编译时定义）和动态（运行时分配）的 Per-CPU 变量。

### 15.2 核心数据结构

#### 15.2.1 pcpu_chunk

`mm/percpu-internal.h` 中定义：

```c
struct pcpu_chunk {
    struct list_head        list;            // 链接到 pcpu_chunk_lists[slot]
    int                     free_bytes;      // 空闲字节数
    struct pcpu_block_md    chunk_md;        // chunk 级元数据
    unsigned long          *bound_map;       // 边界位图（仅分配时更新）
    void                   *base_addr;       // 基地址（Per-CPU 映射起始）
    unsigned long          *alloc_map;       // 分配位图
    struct pcpu_block_md   *md_blocks;       // 每块元数据数组
    void                   *data;            // chunk 数据
    bool                    immutable;       // 禁止 [de]population
    bool                    isolated;        // 从活跃 slot 隔离
    int                     start_offset;    // 与前一个区域的页对齐重叠
    int                     end_offset;      // 确保页对齐的额外区域
    int                     nr_pages;        // 管理的页面数
    int                     nr_populated;    // 已 populate 的页面数
    int                     nr_empty_pop_pages; // 空 populate 页面数
    unsigned long           populated[];     // populate 位图
};
```

#### 15.2.2 pcpu_block_md 元数据

```c
struct pcpu_block_md {
    int  scan_hint;           // 扫描提示（已知最大连续空闲区域）
    int  scan_hint_start;     // 扫描提示起始位置
    int  contig_hint;         // 连续空闲大小提示
    int  contig_hint_start;   // 连续空闲起始位置
    int  left_free;           // 块左侧空闲大小
    int  right_free;          // 块右侧空闲大小
    int  first_free;          // 第一个空闲位位置
    int  nr_bits;             // 总位数
};
```

### 15.3 Chunk 组织

#### 15.3.1 Slot 机制

Chunk 按空闲大小组织到多个 slot 中：

```c
#define PCPU_SLOT_BASE_SHIFT    5   // slot 粒度：32 字节
int pcpu_nr_slots;                  // 总 slot 数
struct list_head *pcpu_chunk_lists; // slot 数组

// slot 分类
pcpu_free_slot;              // 完全空闲的 chunk
pcpu_sidelined_slot;         // 低空闲 chunk（等待回收）
pcpu_to_depopulate_slot;     // 待释放页面的 chunk
```

#### 15.3.2 地址映射

```
Per-CPU 地址空间布局：

  ┌─────────────────────┐
  │  静态 Per-CPU 数据   │  ← __per_cpu_start
  ├─────────────────────┤
  │  保留区（模块）       │
  ├─────────────────────┤
  │  动态分配区          │  ← 由 pcpu_chunk 管理
  └─────────────────────┘

CPU → Unit 映射：
  pcpu_unit_map[cpu] = unit_id
  addr = pcpu_base_addr + pcpu_unit_offsets[cpu] + offset
```

### 15.4 分配路径

#### 15.4.1 pcpu_alloc_noprof 主流程

```c
void __percpu *pcpu_alloc_noprof(size_t size, size_t align, bool reserved, gfp_t gfp)
{
    // 1. 对齐和大小检查
    size = ALIGN(size, PCPU_MIN_ALLOC_SIZE);  // 最小对齐
    bits = size >> PCPU_MIN_ALLOC_SHIFT;
    bit_align = align >> PCPU_MIN_ALLOC_SHIFT;

    // 2. 保留区分配
    if (reserved && pcpu_reserved_chunk) {
        off = pcpu_find_block_fit(chunk, bits, bit_align, is_atomic);
        off = pcpu_alloc_area(chunk, bits, bit_align, off);
        if (off >= 0) goto area_found;
    }

restart:
    // 3. 搜索正常 chunk（从最紧凑的 slot 开始）
    for (slot = pcpu_size_to_slot(size); slot <= pcpu_free_slot; slot++) {
        list_for_each_entry_safe(chunk, next, &pcpu_chunk_lists[slot], list) {
            off = pcpu_find_block_fit(chunk, bits, bit_align, is_atomic);
            if (off < 0) {
                if (slot < PCPU_SLOT_FAIL_THRESHOLD)
                    pcpu_chunk_move(chunk, 0);  // 降级到低 slot
                continue;
            }
            off = pcpu_alloc_area(chunk, bits, bit_align, off);
            if (off >= 0) {
                pcpu_reintegrate_chunk(chunk);  // 更新 slot 位置
                goto area_found;
            }
        }
    }

    // 4. 无空间 → 创建新 chunk
    chunk = pcpu_create_chunk(pcpu_gfp);
    if (!chunk) goto fail;
    pcpu_chunk_relocate(chunk, -1);
    goto restart;

area_found:
    // 5. 确保页面已 populate
    for (cpu = 0; cpu < nr_cpu_ids; cpu++)
        pcpu_populate_chunk(chunk, off, size, cpu, pcpu_gfp);
    ptr = __addr_to_pcpu_ptr(chunk->base_addr + off);
    return ptr;
}
```

**分配策略**：
- 从最紧凑的 slot（最小的空闲大小）开始搜索，优先利用已有空间
- `pcpu_size_to_slot(size)` 将分配大小映射到 slot 索引
- 原子分配（`is_atomic`）不持有 `pcpu_alloc_mutex`，但限制更多
- `pcpu_find_block_fit` 利用 `pcpu_block_md` 元数据快速定位

#### 15.4.2 pcpu_find_block_fit 查找

利用元数据层次结构避免全位图扫描：

```
pcpu_find_block_fit(chunk, bits, align, is_atomic)
  ├─ chunk_md.contig_hint ≥ bits?  → 快速拒绝
  └─ 遍历 md_blocks[]
       └─ block->contig_hint ≥ bits? → 扫描该块的位图区域
            └─ alloc_map 中查找连续空闲位
```

### 15.5 释放路径

```c
void free_percpu(void __percpu *ptr)
{
    addr = __pcpu_ptr_to_addr(ptr);
    chunk = pcpu_chunk_addr_search(addr);  // 通过反向映射查找 chunk
    off = addr - chunk->base_addr;

    size = pcpu_free_area(chunk, off);     // 更新 alloc_map 和元数据
    pcpu_reintegrate_chunk(chunk);         // 重新计算空闲大小，更新 slot

    // 完全空闲的 chunk 触发后台平衡
    if (!chunk->isolated && chunk->free_bytes == pcpu_unit_size) {
        need_balance = true;
        pcpu_schedule_balance_work();  // 唤醒 pcpu_balance_workfn
    }
}
```

### 15.6 后台平衡

```c
static void pcpu_balance_workfn(struct work_struct *work)
{
    mutex_lock(&pcpu_alloc_mutex);
    spin_lock_irq(&pcpu_lock);

    // 1. 释放多余的空 populate 页面
    pcpu_balance_free(false);
    // 2. 回收完全空闲的 chunk
    pcpu_reclaim_populated();
    // 3. 补充不足的 populate 页面
    pcpu_balance_populated();
    // 4. 再次尝试释放（可能回收了新的空闲 chunk）
    pcpu_balance_free(true);

    spin_unlock_irq(&pcpu_lock);
    mutex_unlock(&pcpu_alloc_mutex);
}
```

**平衡策略**：
- `pcpu_balance_free`：当 `nr_empty_pop_pages > PCPU_EMPTY_POP_PAGES_HIGH` 时，释放多余的 empty populate 页面
- `pcpu_reclaim_populated`：将 `pcpu_to_depopulate_slot` 中的 chunk 页面返还给 VM
- `pcpu_balance_populated`：当 `nr_empty_pop_pages < PCPU_EMPTY_POP_PAGES_LOW` 时，为 chunk 补充页面

### 15.7 NUMA 支持

Per-CPU 分配器将 CPU 按 NUMA 节点分组：

```
Group 0 (Node 0):     Group 1 (Node 1):
  ┌─ u0 ─ u1 ┐         ┌─ u2 ─ u3 ┐
  │  CPU 0   │         │  CPU 2   │
  │  CPU 1   │         │  CPU 3   │
  └──────────┘         └──────────┘
```

- 每组拥有独立的内存映射（`pcpu_group_offsets`、`pcpu_group_sizes`）
- 每个 chunk 在所有 unit 上分配相同偏移量，实现跨 CPU 对称访问
- `pcpu_alloc_alloc_info` 分配 NUMA 感知的 unit 映射信息

### 15.8 接口与可调参数

| 接口 | 功能 |
|------|------|
| `__alloc_percpu(size, align)` | 分配 Per-CPU 内存（GFP_KERNEL） |
| `__alloc_percpu_gfp(size, align, gfp)` | 分配 Per-CPU 内存（指定 GFP） |
| `alloc_percpu(type)` | 类型安全的 Per-CPU 分配 |
| `free_percpu(ptr)` | 释放 Per-CPU 内存 |
| `per_cpu_ptr(ptr, cpu)` | 获取指定 CPU 的指针 |
| `pcpu_nr_pages()` | 返回 Per-CPU 分配器总页面数 |

**可调参数**（`/sys/devices/system/cpu`）：
- `PCPU_MIN_ALLOC_SIZE`：最小分配粒度（默认 8 字节）
- `PCPU_SLOT_BASE_SHIFT`：Slot 粒度（默认 5，即 32 字节）
- `PCPU_EMPTY_POP_PAGES_LOW` / `PCPU_EMPTY_POP_PAGES_HIGH`：empty populate 页面控制阈值

---

## Part IV: 控制与隔离

## 16. Memory Cgroup

### 16.1 概述

文件：`mm/memcontrol.c`（5,679 行），`mm/memcontrol-v1.c`（2,243 行）

Memory Cgroup（memcg）是 Linux 内存资源控制的核心机制，支持 cgroup v1 和 v2 两种接口。其核心设计目标：

- **资源隔离**：限制每个 cgroup 的内存使用上限
- **公平分配**：通过保护机制（`memory.min`/`low`）保障关键任务
- **OOM 控制**：超限时触发回收或 OOM Killer
- **统计透明**：细粒度内存使用统计

### 16.2 核心数据结构

#### 16.2.1 struct mem_cgroup — 完整结构

```c
struct mem_cgroup {
    struct cgroup_subsys_state css;          // cgroup 子系统状态

    /* Private memcg ID. Used to ID objects that outlive the cgroup */
    struct mem_cgroup_private_id id;

    /* Accounted resources */
    struct page_counter memory;              // 内存使用计数（v1 & v2）

    union {
        struct page_counter swap;            // 交换计数（v2）
        struct page_counter memsw;           // 内存+交换计数（v1）
    };

    /* Range enforcement for interrupt charges */
    struct work_struct high_work;            // memory.high 超限异步回收

#ifdef CONFIG_ZSWAP
    unsigned long zswap_max;                 // zswap 最大使用量
    bool zswap_writeback;                    // 是否允许 zswap 写回
#endif

    /* vmpressure notifications */
    struct vmpressure vmpressure;             // 内存压力通知

    bool oom_group;                          // OOM 时是否 kill 整个 cgroup
    int swappiness;                          // 该 cgroup 的交换倾向

    /* memory.events and memory.events.local */
    struct cgroup_file events_file;
    struct cgroup_file events_local_file;
    struct cgroup_file swap_events_file;

    struct memcg_vmstats *vmstats;           // Per-memcg 统计

    /* memory.events */
    atomic_long_t memory_events[MEMCG_NR_MEMORY_EVENTS];
    atomic_long_t memory_events_local[MEMCG_NR_MEMORY_EVENTS];

    int kmemcg_id;                           // Kernel memory cgroup ID
    struct obj_cgroup __rcu *objcg;          // 对象级 cgroup 记账
    struct obj_cgroup *orig_objcg;           // 原始 objcg 引用

    struct memcg_vmstats_percpu __percpu *vmstats_percpu; // Per-CPU 统计

#ifdef CONFIG_CGROUP_WRITEBACK
    struct list_head cgwb_list;              // 写回相关链表
    struct wb_domain cgwb_domain;            // 写回域
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
    struct deferred_split deferred_split_queue; // 延迟分裂 THP 队列
#endif

#ifdef CONFIG_LRU_GEN_WALKS_MMU
    struct lru_gen_mm_list mm_list;           // Per-memcg mm 列表
#endif

#ifdef CONFIG_MEMCG_V1
    /* Legacy v1-only counters */
    struct page_counter kmem;                // v1 kernel memory
    struct page_counter tcpmem;              // v1 TCP memory

    unsigned long soft_limit;                // 软限制

    bool oom_lock;
    int under_oom;
    int oom_kill_disable;                    // 是否禁用 OOM Killer

    struct mem_cgroup_thresholds thresholds;  // 阈值事件
    struct mem_cgroup_thresholds memsw_thresholds;
#endif
};
```

#### 16.2.2 struct page_counter — 页面计数与限制

```c
struct page_counter {
    unsigned long min;               // 最低保障（硬性保证）
    unsigned long low;               // 低优先级保护（尽力保证）
    unsigned long high;              // 软限制（触发异步回收）
    unsigned long max;               // 硬限制（超过触发 OOM）
    atomic_long_t usage;             // 当前使用量
    unsigned long watermarks[NR_WMARK]; // 历史水位线
    unsigned long min_usage;         // 最小使用量（低水位）
    unsigned long max_usage;         // 最大使用量（高水位）
    struct page_counter *parent;     // 父计数器的指针
};
```

**三级限制语义**：

| 限制 | 行为 | 场景 |
|------|------|------|
| `min` | 保证可用，不可被回收 | 关键任务保护 |
| `low` | 尽力保证，空闲时被回收 | 普通任务保护 |
| `high` | 触发异步回收，但不阻止分配 | 回收压力反馈 |
| `max` | 严格上限，超限触发 OOM | 硬性隔离 |

#### 16.2.3 Per-CPU Stock 缓存

```c
struct memcg_stock_pcp {
    local_trylock_t lock;
    uint8_t nr_pages[NR_MEMCG_STOCK];       // 缓存页数
    struct mem_cgroup *cached[NR_MEMCG_STOCK]; // 缓存的 memcg
    struct work_struct work;                 // 排空 work
    unsigned long flags;
};
```

Stock 缓存是 memcg 的核心性能优化：每次 charge 不直接操作全局计数器，而是先尝试从本地 Per-CPU 缓存消耗。避免频繁获取全局锁，显著提升多 CPU 场景性能。

### 16.3 Charge/Uncharge 路径

#### 16.3.1 分配记账路径（charge）

```
__mem_cgroup_charge(folio, mm, gfp)
  └─ get_mem_cgroup_from_mm(mm)          // 获取当前进程所属 memcg
  └─ charge_memcg(folio, memcg, gfp)
       ├─ try_charge(memcg, gfp, nr_pages)  // 核心记账逻辑
       │    ├─ consume_stock(memcg, nr_pages)  // [快路径] 从 Per-CPU 缓存消耗
       │    │    └─ 本地 memcg_stock 中有足够缓存 → 直接返回成功
       │    ├─ page_counter_try_charge(&memcg->memory, batch, &counter)
       │    │    └─ 尝试从全局计数器扣减，失败则：
       │    │         ├─ 触发 memory.high 事件
       │    │         └─ try_to_free_mem_cgroup_pages()  // 回收
       │    ├─ drain_all_stock(mem_over_limit)          // 排空所有 CPU 缓存
       │    ├─ 多轮重试 (MAX_RECLAIM_RETRIES)
       │    └─ mem_cgroup_oom()                        // 触发 OOM
       ├─ css_get(&memcg->css)            // 增加 css 引用计数
       ├─ commit_charge(folio, memcg)     // 将 folio 关联到 memcg
       └─ memcg1_commit_charge(folio)     // v1 兼容处理
```

**try_charge 核心代码**：

```c
static int try_charge_memcg(struct mem_cgroup *memcg, gfp_t gfp_mask,
                            unsigned int nr_pages)
{
    unsigned int batch = max(MEMCG_CHARGE_BATCH, nr_pages);
    int nr_retries = MAX_RECLAIM_RETRIES;

retry:
    // 第 1 步：尝试从 Per-CPU stock 缓存快速消耗
    if (consume_stock(memcg, nr_pages))
        return 0;

    // 第 2 步：尝试从 page_counter 扣减
    if (page_counter_try_charge(&memcg->memory, batch, &counter))
        goto done_restock;

    // 第 3 步：触发回收
    if (batch > nr_pages) {
        batch = nr_pages;
        goto retry;
    }

    // 第 4 步：OOM 路径
    if (unlikely(current->flags & PF_MEMALLOC))
        goto force;  // 绕过限制

    if (!gfpflags_allow_blocking(gfp_mask))
        goto nomem;  // 不可阻塞，直接失败

    // 尝试回收
    nr_reclaimed = try_to_free_mem_cgroup_pages(mem_over_limit, nr_pages,
                                                gfp_mask, reclaim_options, NULL);

    if (!drained) {
        drain_all_stock(mem_over_limit);  // 排空所有 CPU stock
        drained = true;
        goto retry;
    }

    // 多轮重试，最终触发 OOM
    if (mem_cgroup_oom(mem_over_limit, gfp_mask, get_order(nr_pages * PAGE_SIZE))) {
        passed_oom = true;
        nr_retries = MAX_RECLAIM_RETRIES;
        goto retry;
    }

nomem:
    if (!(gfp_mask & (__GFP_NOFAIL | __GFP_HIGH)))
        return -ENOMEM;
force:
    // 强制分配（即使超限）
    page_counter_charge(&memcg->memory, nr_pages);
    return 0;
}
```

#### 16.3.2 释放记账路径（uncharge）

```c
void __mem_cgroup_uncharge(struct folio *folio)
{
    // 预检查：未关联 memcg 则跳过
    if (!folio_memcg_charged(folio))
        return;

    uncharge_gather_clear(&ug);
    uncharge_folio(folio, &ug);   // 收集释放信息
    uncharge_batch(&ug);          // 批量释放
}
```

**uncharge_batch** 批量操作：
- `page_counter_uncharge()`：扣减全局计数器
- `refill_stock()`：将释放的页面回填到 Per-CPU stock 缓存
- `css_put()`：释放 css 引用

#### 16.3.3 迁移记账

```c
void mem_cgroup_migrate(struct folio *old, struct folio *new)
{
    memcg = folio_memcg(old);
    // 直接将 old 的记账转移到 new
    commit_charge(new, memcg);
    old->memcg_data = 0;  // 清除 old 的 memcg 关联
}
```

### 16.4 Per-CPU Stock 缓存机制

Stock 缓存是 memcg 性能的关键设计，避免每次 charge 都操作全局计数器：

```
# 分配时（charge）
consume_stock(memcg, nr_pages)
  └─ 检查本地 memcg_stock 是否有缓存
       ├─ 有：直接消耗，返回 true
       └─ 无：返回 false，走全局路径

# 释放时（uncharge）
refill_stock(memcg, nr_pages)
  └─ 将释放的页面放入本地 memcg_stock
       ├─ 累计到 MEMCG_CHARGE_BATCH（32 页）时
       └─ 触发 stock 刷新到全局计数器

# 全局排空（回收压力时）
drain_all_stock(root_memcg)
  └─ 遍历所有 CPU
       ├─ 对每个 CPU 的 memcg_stock 和 obj_stock
       └─ 调度 work 将缓存刷回全局计数器
```

**关键参数**：
- `MEMCG_CHARGE_BATCH = 32`：stock 批量大小
- `NR_MEMCG_STOCK = 2`：每个 stock 支持两个缓存的 memcg

### 16.5 保护计算与回收

#### 16.5.1 保护计算

```c
void mem_cgroup_calculate_protection(struct mem_cgroup *root,
                                     struct mem_cgroup *memcg)
{
    bool recursive_protection =
        cgrp_dfl_root.flags & CGRP_ROOT_MEMORY_RECURSIVE_PROT;

    page_counter_calculate_protection(&root->memory, &memcg->memory,
                                      recursive_protection);
}
```

保护计算决定了每个 cgroup 在回收时的"豁免"程度：
- **`memory.min`**：硬性保证，其他 cgroup 无法回收这部分内存
- **`memory.low`**：尽力保证，只有所有 cgroup 都低于 `low` 时才回收
- 递归保护模式：子 cgroup 可以继承父 cgroup 的保护

#### 16.5.2 回收路径中的 memcg

```
shrink_node_memcgs(pgdat, sc)                  // mm/vmscan.c
  └─ mem_cgroup_iter(root, prev, reclaim)      // 遍历 memcg 树
       ├─ css_next_descendant_pre()            // 前序遍历 cgroup 层级
       ├─ 跳过根 memcg（已在全局路径处理）
       └─ shrink_lruvec(lruvec, sc)            // 回收该 memcg 的 LRU 链表
            ├─ 根据保护值计算 should_skip
            ├─ 遍历 LRU 链表（anon/file）
            └─ 执行页面回收
```

**mem_cgroup_iter 迭代器**：
- 支持并发 reclaim 的 cookie 机制，避免争用
- 每个 node 维护独立的 `nodeinfo[nid]->iter` 迭代器
- 使用 `cmpxchg` 原子操作更新迭代位置

### 16.6 关键接口汇总

| 接口 | 文件 | 功能 |
|------|------|------|
| `__mem_cgroup_charge()` | `memcontrol.c:4755` | 为新页面分配记账 |
| `__mem_cgroup_uncharge()` | `memcontrol.c:4926` | 释放页面记账 |
| `mem_cgroup_migrate()` | `memcontrol.c:5006` | 页面迁移时转移记账 |
| `mem_cgroup_replace_folio()` | `memcontrol.c:5048` | 替换页面时转移记账 |
| `mem_cgroup_charge_hugetlb()` | `memcontrol.c:4785` | Hugetlb 页面的记账 |
| `mem_cgroup_swapin_charge_folio()` | `memcontrol.c:4825` | 换入页面的记账 |
| `mem_cgroup_calculate_protection()` | `memcontrol.c:4724` | 计算保护值 |
| `mem_cgroup_iter()` | `memcontrol.c:1021` | 遍历 memcg 树 |
| `get_mem_cgroup_from_current()` | `memcontrol.c:968` | 获取当前进程的 memcg |
| `get_mem_cgroup_from_folio()` | `memcontrol.c:992` | 获取 folio 所属的 memcg |
| `try_charge_memcg()` | `memcontrol.c:2355` | 核心记账逻辑 |
| `consume_stock()` | `memcontrol.c:1841` | 从 Per-CPU 缓存消耗 |
| `drain_all_stock()` | `memcontrol.c:2036` | 排空所有 CPU 缓存 |
| `try_to_free_mem_cgroup_pages()` | `memcontrol.c` | 触发 memcg 回收 |

### 16.7 cgroup v2 接口文件

| 文件 | 说明 | 类型 |
|------|------|------|
| `memory.current` | 当前内存使用量 | 只读 |
| `memory.min` | 最低保障内存 | 读写 |
| `memory.low` | 低优先级保护 | 读写 |
| `memory.high` | 软限制（触发回收） | 读写 |
| `memory.max` | 硬限制（触发 OOM） | 读写 |
| `memory.swap.current` | 当前交换使用量 | 只读 |
| `memory.swap.max` | 交换硬限制 | 读写 |
| `memory.stat` | 详细统计信息 | 只读 |
| `memory.events` | 内存事件计数器 | 只读 |
| `memory.reclaim` | 手动触发回收 | 读写 |
| `memory.pressure` | PSI 内存压力 | 只读 |
| `memory.oom.group` | OOM 是否 kill 整个 cgroup | 读写 |

---

## 17. NUMA 与内存策略

### 17.1 概述

| 文件 | 行数 | 功能 |
|------|------|------|
| `mm/mempolicy.c` | 3,945 | NUMA 内存策略核心实现 |
| `mm/memory-tiers.c` | 1,009 | 内存层级管理 |
| `mm/numa.c` | 1,208 | NUMA 页面管理 |
| `mm/numa_memblks.c` | 1,240 | NUMA 内存块描述 |
| `mm/numa_emulation.c` | 904 | NUMA 模拟 |

NUMA 内存策略允许用户空间和内核控制进程/VMA 的内存分配节点选择，支持 7 种策略模式和内存层级（Memory Tier）管理。

### 17.2 核心数据结构

#### 17.2.1 struct mempolicy — 内存策略

```c
struct mempolicy {
    atomic_t refcnt;              // 引用计数
    unsigned short mode;          // 策略模式（MPOL_*）
    unsigned short flags;         // 策略标志（MPOL_F_*）
    nodemask_t nodes;             // 目标节点集
    int home_node;                // 首选 home 节点（MPOL_BIND/PREFERRED_MANY）

    union {
        nodemask_t cpuset_mems_allowed;  // cpuset 限制的节点集
        nodemask_t user_nodemask;        // 用户传入的节点集
    } w;
    struct rcu_head rcu;          // RCU 延迟释放
};
```

#### 17.2.2 策略模式

```c
enum {
    MPOL_DEFAULT,               // 默认策略（分配在本地节点，回退到 cpuset）
    MPOL_PREFERRED,             // 首选节点（优先使用指定节点）
    MPOL_BIND,                  // 绑定到指定节点集（无回退）
    MPOL_INTERLEAVE,            // 在节点集之间交错分配
    MPOL_LOCAL,                 // 本地节点分配（忽略 cpuset）
    MPOL_PREFERRED_MANY,        // 多个首选节点（逐个回退）
    MPOL_WEIGHTED_INTERLEAVE,   // 加权交错分配（按权重分配不同节点）
};
```

**策略标志**：

| 标志 | 含义 |
|------|------|
| `MPOL_F_STATIC_NODES` | 节点集是绝对编号，不随 cpuset 重映射 |
| `MPOL_F_RELATIVE_NODES` | 节点集是相对编号，映射到 cpuset 中的节点 |
| `MPOL_F_SHARED` | 共享策略（用于共享内存） |
| `MPOL_F_MOF` | 策略迁移允许（允许页面迁移到新节点） |
| `MPOL_F_MORON` | 策略迁移未优化（避免不必要的迁移） |

### 17.3 策略层次与优先级

策略按优先级从高到低：

```
VMA 策略 (vma->vm_policy)          ← 最高优先级，仅对特定 VMA 有效
    │
任务策略 (current->mempolicy)      ← 进程级默认策略
    │
系统策略 (default_policy)          ← 最低优先级，MPOL_DEFAULT
```

**分配路径**：

```c
struct folio *vma_alloc_folio_noprof(gfp_t gfp, int order,
        struct vm_area_struct *vma, unsigned long addr)
{
    pol = get_vma_policy(vma, addr, order, &ilx);   // 获取 VMA 策略
    folio = folio_alloc_mpol_noprof(gfp, order, pol, ilx, numa_node_id());
    mpol_cond_put(pol);
    return folio;
}

struct page *alloc_pages_noprof(gfp_t gfp, unsigned int order)
{
    pol = get_task_policy(current);                  // 获取任务策略
    return alloc_pages_mpol(gfp, order, pol, NO_INTERLEAVE_INDEX, numa_node_id());
}
```

### 17.4 策略核心函数

#### 17.4.1 设置策略

```c
static long do_set_mempolicy(unsigned short mode, unsigned short flags,
                             nodemask_t *nodes)
{
    new = mpol_new(mode, flags, nodes);     // 创建新策略对象
    ret = mpol_set_nodemask(new, nodes, scratch);  // 设置节点掩码
    old = current->mempolicy;
    current->mempolicy = new;               // 替换当前策略
    if (new && (new->mode == MPOL_INTERLEAVE ||
                new->mode == MPOL_WEIGHTED_INTERLEAVE)) {
        current->il_prev = MAX_NUMNODES-1;  // 重置交错游标
        current->il_weight = 0;
    }
    mpol_put(old);                          // 释放旧策略
    return 0;
}
```

#### 17.4.2 策略分配辅助函数

**policy_nodemask** — 根据策略计算分配节点掩码：

```c
static nodemask_t *policy_nodemask(gfp_t gfp, struct mempolicy *pol,
                                   pgoff_t ilx, int *nid)
{
    switch (pol->mode) {
    case MPOL_PREFERRED:
        *nid = first_node(pol->nodes);
        return NULL;                       // 指定首选节点
    case MPOL_BIND:
        return &pol->nodes;                // 限制在绑定节点集
    case MPOL_INTERLEAVE:
        *nid = interleave_nid(pol, ilx);   // 按偏移量计算交错节点
        return NULL;
    case MPOL_WEIGHTED_INTERLEAVE:
        *nid = weighted_interleave_nid(pol, ilx); // 加权交错
        return NULL;
    case MPOL_PREFERRED_MANY:
        *nid = first_node(pol->nodes);     // 首选第一个节点
        return &pol->nodes;                // 回退到完整节点集
    default:
        return NULL;
    }
}
```

### 17.5 内存层级（Memory Tiers）

#### 17.5.1 核心数据结构

```c
struct memory_tier {
    struct list_head list;              // 链接到全局 memory_tiers 链表
    struct list_head memory_types;      // 属于该层级的内存类型列表
    int adistance_start;                // 抽象距离起始值
    struct device dev;                  // 设备模型
    nodemask_t lower_tier_mask;         // 所有更低层级节点的掩码
};

struct node_memory_type_map {
    struct memory_dev_type *memtype;    // 节点对应的内存类型
    int map_count;                      // 映射计数
};
```

#### 17.5.2 层级划分

| 层级 | 抽象距离 | 类型 | 典型延迟 |
|------|----------|------|----------|
| Tier 0 | 0–511 | DRAM（本地） | <100ns |
| Tier 1 | 512–1023 | DRAM（远程） | 100–300ns |
| Tier 2 | 1024–1535 | PMEM（持久内存） | 300–1000ns |
| Tier 3 | 1536–2047 | GPU 内存 / CXL 内存 | >1µs |

**降级/提升机制**：

```
# 页面层级降级（从 Tier 0 到 Tier 1/2）
folio_use_access_time(folio)  → 检查是否不是 toptier
NUMA 页面迁移              → 将冷页面迁移到低层级内存

# 页面层级提升（从 Tier 1/2 到 Tier 0）
NUMA 页面访问统计          → 识别热页面
migrate_misplaced_folio()  → 将热页面迁移回高层级
```

### 17.6 关键 syscall 接口

| 系统调用 | 功能 | 实现 |
|----------|------|------|
| `mbind()` | 设置 VMA 策略 | `kernel_mbind()` → `mbind_range()` |
| `set_mempolicy()` | 设置任务策略 | `kernel_set_mempolicy()` → `do_set_mempolicy()` |
| `get_mempolicy()` | 获取当前策略 | `do_get_mempolicy()` |
| `migrate_pages()` | 按策略迁移页面 | `kernel_migrate_pages()` |
| `move_pages()` | 迁移指定页面 | `kernel_move_pages()` |

### 17.7 mbind 实现路径

```
mbind(addr, len, mode, nmask, flags)
  └─ kernel_mbind()
       └─ do_mbind()
            ├─ mpol_new(mode, flags, nmask)              // 创建策略
            ├─ mmap_write_lock(mm)
            ├─ mbind_range(vmi, vma, addr, end, new)      // 应用到 VMA
            │    └─ vma_set_policy(vma, new)              // 设置 vm_policy
            ├─ queue_pages_range(mm, addr, end, &qp)      // 收集已分配的页面
            │    └─ walk_page_range() + queue_folio_required()
            ├─ migrate_to_node(mm, qp.pagelist, target)   // 迁移页面
            └─ mmap_write_unlock(mm)
```

---

## 18. CMA 连续内存分配器

### 18.1 概述

文件：`mm/cma.c`（1,143 行），`mm/cma_debug.c`，`mm/cma_sysfs.c`

CMA（Contiguous Memory Allocator）为需要大块连续物理内存的设备预留内存区域，**在不使用时可供可移动页面使用**，实现内存利用率与设备需求的平衡。

**设计目标**：
- 为 DMA/GPU/摄像头等设备提供大块连续内存
- 空闲时被伙伴系统用于可移动页面分配
- 需要时通过页面迁移腾出空间

### 18.2 核心数据结构

#### 18.2.1 struct cma — CMA 区域

```c
struct cma {
    unsigned long   count;              // 页面总数
    unsigned long   available_count;    // 当前可用页面数
    unsigned int    order_per_bit;      // 每 bit 代表的页面阶（2^order_per_bit 页）
    spinlock_t      lock;               // 位图操作锁
    struct mutex    alloc_mutex;        // 分配互斥锁（保护 alloc_contig_range）
    char            name[CMA_MAX_NAME];  // 区域名称
    int             nranges;            // 物理内存范围数
    struct cma_memrange ranges[CMA_MAX_RANGES]; // 多物理范围支持
    int             nid;                // 所属 NUMA 节点
    unsigned long   flags;              // CMA 标志位
#ifdef CONFIG_CMA_SYSFS
    atomic64_t nr_pages_succeeded;       // 成功分配页数
    atomic64_t nr_pages_failed;          // 失败分配页数
    atomic64_t nr_pages_released;        // 释放页数
    struct cma_kobject *cma_kobj;        // sysfs 接口
#endif
};
```

#### 18.2.2 struct cma_memrange — 物理内存范围

```c
struct cma_memrange {
    unsigned long base_pfn;         // 起始 PFN
    unsigned long count;            // 页面数
    union {
        unsigned long early_pfn;    // 启动时未保留的起始 PFN
        unsigned long *bitmap;      // 分配位图（每个 bit 代表 2^order_per_bit 页）
    };
};
```

**多范围支持**：`CMA_MAX_RANGES = 8`，适用于大块 CMA 区域无法在单个连续物理内存段中分配的场景。

### 18.3 初始化流程

#### 18.3.1 预留 CMA 区域

```
cma_declare_contiguous_nid(base, size, limit, alignment, order_per_bit, fixed, name, res_cma, nid)
  └─ __cma_declare_contiguous_nid(basep, size, limit, alignment, ...)
       ├─ 参数校验（对齐、大小、范围）
       ├─ [固定模式] cma_fixed_reserve(base, size)     // 在指定地址预留
       │    └─ memblock_reserve(base, size)
       ├─ [自动模式] cma_alloc_mem(base, size, align, limit, nid)
       │    ├─ 优先从 4GB 以上区域分配（bottom-up 模式）
       │    └─ memblock_alloc_range_nid()
       └─ cma_init_reserved_mem(base, size, order_per_bit, name, res_cma)
            └─ 初始化 cma 结构体，设置 ranges[0]
```

**启动激活**：

```
cma_init_reserved_areas()           // core_initcall
  └─ for_each CMA area:
       cma_activate_area(cma)
         ├─ 初始化 ranges 的 bitmap（调用 cma_activate_range）
         │    └─ 将 ranges 中的页面标记为 MIGRATE_CMA
         └─ 初始化 alloc_mutex
```

### 18.4 分配路径

#### 18.4.1 cma_alloc 完整流程

```
cma_alloc(cma, count, align, no_warn)
  └─ cma_alloc_frozen(cma, count, align, no_warn)
       └─ __cma_alloc_frozen(cma, count, align, gfp)
            └─ for_each range in cma->ranges:
                 cma_range_alloc(cma, &cmr, count, align, &page, gfp)
```

**cma_range_alloc 核心逻辑**：

```c
static int cma_range_alloc(struct cma *cma, struct cma_memrange *cmr,
                           unsigned long count, unsigned int align,
                           struct page **pagep, gfp_t gfp)
{
    // 第 1 步：计算位图参数
    mask = cma_bitmap_aligned_mask(cma, align);
    offset = cma_bitmap_aligned_offset(cma, cmr, align);
    bitmap_maxno = cma_bitmap_maxno(cma, cmr);
    bitmap_count = cma_bitmap_pages_to_bits(cma, count);

    for (start = 0; ; start = bitmap_no + mask + 1) {
        // 第 2 步：检查可用页数
        if (count > cma->available_count)
            break;

        // 第 3 步：位图查找空闲区域
        bitmap_no = bitmap_find_next_zero_area_off(cmr->bitmap,
                bitmap_maxno, start, bitmap_count, mask, offset);
        if (bitmap_no >= bitmap_maxno)
            break;

        pfn = cmr->base_pfn + (bitmap_no << cma->order_per_bit);
        page = pfn_to_page(pfn);

        // 第 4 步：检查页面连续性
        if (!page_range_contiguous(page, count))
            continue;

        // 第 5 步：标记位图为已分配
        bitmap_set(cmr->bitmap, bitmap_no, bitmap_count);
        cma->available_count -= count;

        // 第 6 步：迁移现有页面（核心操作）
        spin_unlock_irq(&cma->lock);
        mutex_lock(&cma->alloc_mutex);
        ret = alloc_contig_frozen_range(pfn, pfn + count, ACR_FLAGS_CMA, gfp);
        mutex_unlock(&cma->alloc_mutex);
        if (!ret)
            break;  // 成功

        // 迁移失败，回退
        cma_clear_bitmap(cma, cmr, pfn, count);
        if (ret != -EBUSY)
            break;
    }
}
```

**alloc_contig_frozen_range 页面迁移**：

```
alloc_contig_frozen_range(pfn, pfn + count, ACR_FLAGS_CMA, gfp)
  └─ __alloc_contig_migrate_range(cc, start, end)
       ├─ isolate_migratepages_range()     // 隔离范围内所有可移动页面
       └─ migrate_pages()                  // 迁移到其他位置
            ├─ alloc_migration_target()    // 分配新的页面
            └─ __unmap_and_move()          // 解除旧映射并拷贝数据
```

#### 18.4.2 释放路径

```c
bool cma_release(struct cma *cma, const struct page *pages, unsigned long count)
{
    // 第 1 步：查找页面所属的 memory range
    cmr = find_cma_memrange(cma, pages, count);

    // 第 2 步：检查页面的引用计数
    for (i = 0; i < count; i++, pfn++)
        ret += !put_page_testzero(pfn_to_page(pfn));

    // 第 3 步：释放
    __cma_release_frozen(cma, cmr, pages, count);
    //  └─ free_contig_frozen_range(pfn, count)  // 归还给伙伴系统
    //  └─ cma_clear_bitmap(cma, cmr, pfn, count) // 清除位图
    //  └─ cma_sysfs_account_release_pages()
}
```

### 18.5 关键接口汇总

| 接口 | 功能 |
|------|------|
| `cma_declare_contiguous_nid()` | 启动时预留 CMA 区域 |
| `cma_declare_contiguous_multi()` | 多范围预留（当单范围不够时） |
| `cma_init_reserved_mem()` | 从已预留的内存创建 CMA 区域 |
| `cma_alloc()` | 从 CMA 分配连续内存 |
| `cma_alloc_frozen()` | 分配但不设置引用计数 |
| `cma_release()` | 释放 CMA 内存 |
| `cma_release_frozen()` | 释放 frozen 分配 |
| `cma_get_base()` | 获取 CMA 区域基地址 |
| `cma_get_size()` | 获取 CMA 区域大小 |
| `cma_for_each_area()` | 遍历所有 CMA 区域 |

### 18.6 性能特征

| 方面 | 说明 |
|------|------|
| **分配延迟** | 高（涉及页面迁移），取决于区域内可移动页面数量 |
| **释放延迟** | 低（仅位图操作 + 伙伴系统释放） |
| **碎片化影响** | 位图分配保证 O(1) 查找，迁移失败时回退 |
| **并发安全** | `alloc_mutex` 串行化分配，`spinlock` 保护位图 |
| **内存利用率** | 空闲时被可移动页面使用，不浪费 |

---

## 19. 内存热插拔

### 19.1 概述

文件：`mm/memory_hotplug.c`（2,435 行）

内存热插拔支持在系统运行时添加或移除物理内存，是虚拟化、云计算和内存分层的关键特性。

**核心概念**：

| 概念 | 说明 |
|------|------|
| **Memory Block** | 用户可见的最小热插拔单元（通常 128MB） |
| **Section** | 架构相关的最小内存段（ARM64: 1GB x86: 128MB） |
| **Online/Offline** | 内存的在线/离线状态转换 |
| **vmemmap** | 虚拟内存映射的 page struct 数组 |

### 19.2 核心数据结构

#### 19.2.1 struct memory_block — 内存块

```c
struct memory_block {
    unsigned long start_section_nr;  // 起始 section 号
    unsigned long state;             // 状态 (offline/online/going_offline)
    int section_count;               // 包含的 section 数量
    int nid;                         // 所属 NUMA 节点
    struct zone *zone;               // 所属 zone
    struct device dev;               // 设备模型（sysfs 接口）
};
```

### 19.3 热添加路径

#### 19.3.1 完整函数调用栈

```
add_memory(nid, start, size, mhp_flags)
  └─ __add_memory(nid, start, size, mhp_flags)
       ├─ register_memory_resource(start, size, "System RAM")  // 注册资源
       └─ add_memory_resource(nid, res, mhp_flags)
            ├─ memblock_add_node(start, size, nid, flags)     // 添加到 MemBlock
            ├─ __try_online_node(nid, false)                  // 在线化 NUMA 节点
            │    └─ node_set_online(nid)
            │    └─ register_node(nid)
            ├─ [MHP_MEMMAP_ON_MEMORY]
            │    └─ create_altmaps_and_memory_blocks()        // 自托管 vmemmap
            ├─ [默认]
            │    ├─ arch_add_memory(nid, start, size, &params) // 架构特定：添加 vmemmap
            │    │    └─ 创建线性映射的页表项
            │    └─ create_memory_block_devices()              // 创建 memory block 设备
            ├─ firmware_map_add_hotplug()                      // 更新固件映射
            └─ walk_memory_blocks() → online_memory_block()   // 自动 online
                 └─ device_online()
                      └─ memory_block_online()
                           └─ online_pages(pfn, nr_pages, zone, group)
```

#### 19.3.2 online_pages 核心逻辑

```c
int online_pages(unsigned long pfn, unsigned long nr_pages, struct zone *zone,
                 struct memory_group *group)
{
    // 第 1 步：将 PFN 范围关联到 zone
    move_pfn_range_to_zone(zone, pfn, nr_pages, NULL, MIGRATE_MOVABLE, true);

    // 第 2 步：首次添加内存到 NUMA 节点时通知
    if (!node_state(nid, N_MEMORY)) {
        node_notify(NODE_ADDING_FIRST_MEMORY, &node_arg);
    }

    // 第 3 步：通知其他子系统（MEM_GOING_ONLINE）
    memory_notify(MEM_GOING_ONLINE, &mem_arg);

    // 第 4 步：在线化页面并释放到伙伴系统
    online_pages_range(pfn, nr_pages);
    //  └─ 按 MAX_PAGE_ORDER 对齐的块释放
    //  └─ (*online_page_callback)(page, order) 释放页面

    // 第 5 步：更新 present_pages 计数
    adjust_present_page_count(pfn_to_page(pfn), group, nr_pages);

    // 第 6 步：更新节点状态
    node_set_state(nid, N_MEMORY);

    // 第 7 步：重建 zonelist（如果 zone 首次被 populate）
    if (!populated_zone(zone))
        build_all_zonelists(NULL);

    // 第 8 步：通知内存在线完成（MEM_ONLINE）
    memory_notify(MEM_ONLINE, &mem_arg);
}
```

### 19.4 热移除路径

#### 19.4.1 完整函数调用栈

```
remove_memory(start, size)
  └─ try_remove_memory(start, size)
       ├─ walk_memory_blocks() → check_memblock_offlined_cb()  // 检查所有块已 offline
       ├─ firmware_map_remove(start, end, "System RAM")       // 移除固件映射
       │
       ├─ [无 altmap]
       │    ├─ remove_memory_block_devices(start, size)       // 移除设备
       │    └─ arch_remove_memory(start, size, NULL)          // 架构特定：移除 vmemmap
       │
       ├─ [有 altmap]
       │    └─ remove_memory_blocks_and_altmaps(start, size)  // 移除 altmap 和设备
       │
       ├─ memblock_remove(start, size)                        // 从 MemBlock 移除
       ├─ release_mem_region_adjustable(start, size)          // 释放资源
       └─ try_offline_node(nid)                               // 清理空节点
```

#### 19.4.2 offline_pages 核心逻辑

```c
int offline_pages(unsigned long start_pfn, unsigned long nr_pages, struct zone *zone,
                  struct memory_group *group)
{
    // 第 1 步：禁用 Per-CPU 页面列表和 LRU 缓存
    zone_pcp_disable(zone);
    lru_cache_disable();

    // 第 2 步：隔离页面范围
    ret = start_isolate_page_range(start_pfn, end_pfn, PB_ISOLATE_MODE_MEM_OFFLINE);

    // 第 3 步：检查节点是否变空
    if (nr_pages >= pgdat->node_present_pages)
        node_notify(NODE_REMOVING_LAST_MEMORY, &node_arg);

    // 第 4 步：通知子系统（MEM_GOING_OFFLINE）
    memory_notify(MEM_GOING_OFFLINE, &mem_arg);

    // 第 5 步：循环迁移所有可移动页面
    do {
        // 扫描可移动页面
        ret = scan_movable_pages(pfn, end_pfn, &pfn);
        if (!ret) {
            // 迁移范围中的页面到其他位置
            do_migrate_range(pfn, end_pfn);
        }
        // 检查是否所有页面已隔离
        ret = test_pages_isolated(start_pfn, end_pfn, PB_ISOLATE_MODE_MEM_OFFLINE);
    } while (ret);

    // 第 6 步：释放隔离的页面
    managed_pages = __offline_isolated_pages(start_pfn, end_pfn);

    // 第 7 步：更新计数
    adjust_managed_page_count(pfn_to_page(start_pfn), -managed_pages);
    adjust_present_page_count(pfn_to_page(start_pfn), group, -nr_pages);

    // 第 8 步：重建水位线
    init_per_zone_wmark_min();

    // 第 9 步：通知子系统（MEM_OFFLINE）
    memory_notify(MEM_OFFLINE, &mem_arg);
}
```

### 19.5 关键设计点

#### 19.5.1 memmap_on_memory

特殊标志 `MHP_MEMMAP_ON_MEMORY` 将 vmemmap（page struct 数组）放在新添加的内存上，而非现有内存中：

```
add_memory_resource()
  ├─ [MHP_MEMMAP_ON_MEMORY]
  │    └─ create_altmaps_and_memory_blocks()
  │         ├─ 在新内存头部预留空间给 vmemmap
  │         └─ arch_add_memory() 使用 altmap 参数
  └─ [默认]
       └─ arch_add_memory() 在现有内存中分配 vmemmap
```

**优势**：热添加时不需要消耗现有内存用于 vmemmap。

#### 19.5.2 状态转换

```
                 +---------+
                 | OFFLINE |  ← 初始状态
                 +---------+
                      │
               device_online()
                      │
                      v
                 +---------+
          +----->| ONLINE  |<-----+
          |      +---------+      |
          |           │           |
          |    device_offline()   |
          |           │           |
          |           v           |
          |      +-----------+    |
          +------| GOING_OFF |    |
                 +-----------+    |
                      │           |
                 offline_pages()  |
                      │           |
                      v           |
                 +---------+      |
                 | OFFLINE |------+
                 +---------+
```

### 19.6 关键接口汇总

| 接口 | 功能 |
|------|------|
| `add_memory()` | 添加物理内存（入口） |
| `add_memory_resource()` | 核心添加逻辑 |
| `add_memory_driver_managed()` | 驱动管理的内存添加 |
| `remove_memory()` | 移除物理内存（入口） |
| `try_remove_memory()` | 核心移除逻辑 |
| `__remove_memory()` | 强制移除（BUG_ON 失败） |
| `online_pages()` | 在线化页面（释放到伙伴系统） |
| `offline_pages()` | 离线化页面（迁移并隔离） |
| `online_pages_range()` | 按最大对齐块释放到伙伴系统 |
| `scan_movable_pages()` | 扫描可移动页面 |
| `do_migrate_range()` | 迁移范围内的页面 |

---

## Part V: 高级特性

## 20. 透明大页（THP）与 KSM

### 20.1 透明大页（THP）

文件：`mm/huge_memory.c`（4,978 行），`mm/khugepaged.c`（2,873 行）

THP（Transparent Huge Pages）自动将连续的 2MB（PMD 级别）或 1GB（PUD 级别）页面映射为巨页，减少 TLB 缺失，提高大内存工作负载的性能。

#### 20.1.1 sysfs 控制接口

透明大页有三种模式，通过 `/sys/kernel/mm/transparent_hugepage/enabled` 控制：

```c
// mm/huge_memory.c 枚举定义
enum {
    TRANSPARENT_HUGEPAGE_NEVER_ALWAYS,  // 总是启用（默认）
    TRANSPARENT_HUGEPAGE_NEVER_MADVISE, // 仅 MADV_HUGEPAGE 区域
    TRANSPARENT_HUGEPAGE_NEVER_NEVER,   // 从不启用
};
```

附加控制文件：
- `defrag`：THP 分配时的碎片整理策略（always / defer / defer+madvise / madvise / never）
- `khugepaged/defrag`：khugepaged 合并时的碎片整理开关
- `shmem_enabled`：tmpfs/shmem 的 THP 策略
- `use_zero_page`：是否使用零页 THP 映射

#### 20.1.2 初始化流程

```c
// mm/huge_memory.c:906
static int __init hugepage_init(void)
{
    // 1. 检查硬件支持（has_transparent_hugepage）
    // 2. 创建 sysfs 接口（hugepage_init_sysfs）
    // 3. 初始化 khugepaged（khugepaged_init）
    // 4. 注册 shrinker（thp_shrinker_init）
    // 5. 小内存系统（<512MB）自动禁用
    // 6. 启动 khugepaged 内核线程（start_stop_khugepaged）
}
subsys_initcall(hugepage_init);
```

#### 20.1.3 匿名页 THP 缺页分配路径

**读缺页** (`do_huge_pmd_anonymous_page`，位于 `mm/huge_memory.c:1461`)：

```
do_huge_pmd_anonymous_page(vmf)
  ├─ thp_vma_suitable_order(vma, haddr, PMD_ORDER)  // 检查 VMA 是否适合 THP
  ├─ vmf_anon_prepare(vmf)                           // 准备匿名页
  ├─ khugepaged_enter_vma(vma, vm_flags)             // 通知 khugepaged
  ├─ 可选：写时缺页且非 MAP_PRIVATE → 使用零页 THP
  │    └─ set_huge_zero_folio(pgtable, mm, vma, haddr, pmd, zero_folio)
  └─ __do_huge_pmd_anonymous_page(vmf)               // 实际分配
       ├─ vma_alloc_anon_folio_pmd(vma, addr)        // 分配 2MB folio
       │    └─ 内部调用 __alloc_pages(HPAGE_PMD_ORDER)
       ├─ pte_alloc_one(mm)                          // 分配后备页表
       ├─ 检查 pmd_none 和地址空间稳定性
       ├─ userfaultfd 处理
       ├─ pgtable_trans_huge_deposit(mm, pmd, pgtable)  // 存储页表
       └─ map_anon_folio_pmd_pf(folio, pmd, vma, haddr) // 建立 PMD 映射
```

```c
// mm/huge_memory.c:1323 — __do_huge_pmd_anonymous_page 核心代码
static vm_fault_t __do_huge_pmd_anonymous_page(struct vm_fault *vmf)
{
    folio = vma_alloc_anon_folio_pmd(vma, vmf->address);
    if (unlikely(!folio))
        return VM_FAULT_FALLBACK;  // 分配失败 → 回退到小页

    pgtable = pte_alloc_one(vma->vm_mm);
    vmf->ptl = pmd_lock(vma->vm_mm, vmf->pmd);
    if (unlikely(!pmd_none(*vmf->pmd)))   // 竞态检查
        goto unlock_release;
    ret = check_stable_address_space(vma->vm_mm);  // KSM 检查
    if (ret) goto unlock_release;

    pgtable_trans_huge_deposit(vma->vm_mm, vmf->pmd, pgtable);
    map_anon_folio_pmd_pf(folio, vmf->pmd, vma, haddr);
    mm_inc_nr_ptes(vma->vm_mm);
    spin_unlock(vmf->ptl);
    return 0;
}
```

**写时复制 THP** (`do_huge_pmd_wp_page`，位于 `mm/huge_memory.c:2060`)：

```
do_huge_pmd_wp_page(vmf)
  ├─ 零页 THP → do_huge_zero_wp_pmd()          // 零页特殊处理
  │    ├─ 分配新 THP → 拷贝数据 → 建立映射
  │    └─ 分配失败 → fallback 到 split
  ├─ 锁定 PMD → 检查 pmd_same
  ├─ 检查 PageAnonExclusive                    // 已独占则直接重用
  ├─ 否则：
  │    ├─ 分配新 THP (vma_alloc_anon_folio_pmd)
  │    ├─ 拷贝旧页数据
  │    └─ 替换 PMD 映射
  └─ fallback: 拆分 PMD 为小页 → 逐页 COW
```

**NUMA 迁移 THP** (`do_huge_pmd_numa_page`，位于 `mm/huge_memory.c:2185`)：

```
do_huge_pmd_numa_page(vmf)
  ├─ 获取 folio 和当前 numa 节点
  ├─ 检查是否应迁移到目标节点
  └─ migrate_misplaced_folio(folio, target_nid)  // 迁移整个 THP
```

#### 20.1.4 THP 拆分

当 THP 需要被回收、拆分或迁移时，内核将其拆分为普通小页：

```
__split_huge_page(page, list, end)
  ├─ 遍历所有子页，设置映射信息
  ├─ 更新 folio 引用计数
  ├─ 拆分后：
  │    ├─ 各个子页变为独立 4KB 页面
  │    └─ 可独立换出、LRU 管理
  └─ split_huge_page_to_list() 入口
```

### 20.2 khugepaged 内核线程

khugepaged 是一个内核守护线程，在后台扫描进程地址空间，将符合条件的连续小页合并为 THP。

#### 20.2.1 线程主循环

```c
// mm/khugepaged.c:2612
static int khugepaged(void *none)
{
    while (!kthread_should_stop()) {
        khugepaged_do_scan(cc);            // 执行扫描
        khugepaged_wait_work();            // 等待下一个周期
    }
}
```

**扫描参数**（通过 `/sys/kernel/mm/transparent_hugepage/khugepaged/` 控制）：
| 参数 | 默认值 | 说明 |
|------|--------|------|
| `pages_to_scan` | 4096 | 每次扫描的页面数 |
| `scan_sleep_millisecs` | 10000 | 扫描间隔（ms） |
| `alloc_sleep_millisecs` | 60000 | 分配失败后等待时间（ms） |
| `max_ptes_none` | 511 | 允许的空白 PTE 数 |
| `max_ptes_swap` | 511 | 允许的换出 PTE 数 |
| `max_ptes_shared` | 511 | 允许的共享 PTE 数 |

#### 20.2.2 扫描调度

```c
// mm/khugepaged.c:2544
static void khugepaged_do_scan(struct collapse_control *cc)
{
    lru_add_drain_all();                     // 刷新 LRU 缓存
    while (true) {
        spin_lock(&khugepaged_mm_lock);
        // 遍历 mm_slot 链表，扫描每个进程地址空间
        if (khugepaged_has_work())
            progress += khugepaged_scan_mm_slot(pages - progress, &result, cc);
        spin_unlock(&khugepaged_mm_lock);
        if (progress >= pages) break;
        // 分配失败时休眠等待
        if (result == SCAN_ALLOC_HUGE_PAGE_FAIL)
            khugepaged_alloc_sleep();
    }
}
```

#### 20.2.3 扫描进程地址空间

`khugepaged_scan_mm_slot`（位于 `mm/khugepaged.c:2388`）遍历进程的 VMA：

```
khugepaged_scan_mm_slot(pages, result, cc)
  └─ 遍历进程 VMA：
       ├─ 匿名 VMA → hpage_collapse_scan_pmd(mm, vma, addr, mmap_locked, cc)
       │    └─ 检查 PMD 范围内页面是否适合合并
       │         ├─ 检查引用计数、LRU 状态、脏页
       │         └─ 返回 SCAN_SUCCEED 或各种失败原因
       └─ 文件 VMA → hpage_collapse_scan_file(mm, addr, file, pgoff, cc)
            └─ 扫描 page cache 中的连续页面
```

#### 20.2.4 合并流程 — collapse_huge_page

```c
// mm/khugepaged.c:1078
static enum scan_result collapse_huge_page(struct mm_struct *mm,
        unsigned long address, int referenced, int unmapped,
        struct collapse_control *cc)
{
    // 1. 释放 mmap_read_lock（分配可能耗时）
    mmap_read_unlock(mm);

    // 2. 分配 THP 页面并 charge 到 memcg
    result = alloc_charge_folio(&folio, mm, cc);

    // 3. 重新获取 mmap_read_lock，验证 VMA
    mmap_read_lock(mm);
    result = hugepage_vma_revalidate(mm, address, true, &vma, cc);

    // 4. 查找 PMD 位置
    result = find_pmd_or_thp_or_none(mm, address, &pmd);

    // 5. 如果需要，换入被换出的页面
    if (unmapped)
        result = __collapse_huge_page_swapin(mm, vma, address, pmd, referenced);

    // 6. 升级为 mmap_write_lock 防止并发修改
    mmap_write_lock(mm);
    result = hugepage_vma_revalidate(mm, address, true, &vma, cc);

    // 7. 隔离源页面（PTE 级别）
    result = __collapse_huge_page_isolate(vma, address, pte, cc, &compound_pagelist);

    // 8. 拷贝数据到新 THP 页面
    result = __collapse_huge_page_copy(pte, folio, pmd, _pmd, vma, address,
                                       pte_ptl, &compound_pagelist);

    // 9. 安装 PMD 映射
    __folio_mark_uptodate(folio);
    pgtable_trans_huge_deposit(mm, pmd, pgtable);
    map_anon_folio_pmd_nopf(folio, pmd, vma, address);
}
```

**关键步骤流程**：
```
collapse_huge_page()
  ├─ [1] 释放读锁 → 分配 THP
  ├─ [2] 重新获取读锁 → 验证 VMA 未变化
  ├─ [3] 查找 PMD → 检查 THP 或映射是否存在
  ├─ [4] 换入换出页（如需要）
  ├─ [5] 升级为写锁 → 全面锁定地址空间
  ├─ [6] __collapse_huge_page_isolate()
  │    ├─ 遍历 PMD 范围内所有 PTE
  │    ├─ 检查页面引用计数（page_count == 1 + mapcount）
  │    ├─ 检查页面是否在 LRU 上
  │    ├─ 隔离页面（folio_isolate_lru）
  │    └─ 检查是否有足够的年轻 PTE 引用
  ├─ [7] __collapse_huge_page_copy()
  │    ├─ 逐页拷贝数据（copy_mc_user_highpage，处理内存错误）
  │    └─ 清理旧 PTE 映射
  └─ [8] 安装 PMD 映射 → 合并完成
```

#### 20.2.5 文件页与 shmem 合并 — collapse_file

```c
// mm/khugepaged.c:1824
static enum scan_result collapse_file(struct mm_struct *mm, unsigned long addr,
        struct file *file, pgoff_t start, struct collapse_control *cc)
{
    // 1. 分配新 THP folio
    result = alloc_charge_folio(&new_folio, mm, cc);

    // 2. 锁定 page cache（xas_lock_irq）
    // 3. 遍历 HPAGE_PMD_NR 个页缓存项：
    //    - 跳过空洞（记录 nr_none）
    //    - 锁定现有页面
    //    - 验证页面属于同一 folio
    // 4. 将新 folio 插入 page cache（replace folio）
    // 5. 释放旧页面
    // 6. 清除页面映射 → 后续缺页时自动建立 PMD 映射
}
```

#### 20.2.6 PTE 映射 THP 合并 — collapse_pte_mapped_thp

```
collapse_pte_mapped_thp(mm, addr, install_pmd)
  └─ try_collapse_pte_mapped_thp(mm, addr, install_pmd)
       ├─ 检查 PMD 范围内所有 PTE 是否指向同一个 THP
       ├─ 锁住页表
       ├─ 收回页表（retract_page_tables）
       └─ 可选：安装 PMD 映射
```

### 20.3 KSM — 内核同页合并

文件：`mm/ksm.c`（4,019 行）

KSM（Kernel Same-page Merging）通过比较页面内容，将内容相同的页面合并为写时复制（COW）共享页面，主要用于虚拟化环境（如 KVM）中的内存超分。

#### 20.3.1 核心数据结构

```c
// mm/ksm.c:159
struct ksm_stable_node {
    union {
        struct rb_node node;        // 稳定树节点
        struct {                    // 迁移时使用
            struct list_head *head;
            struct {
                struct hlist_node hlist_dup;
                struct list_head list;
            };
        };
    };
    struct hlist_head hlist;        // 该稳定节点关联的 rmap_item 链表
    union {
        unsigned long kpfn;         // 稳定页面的 PFN
        unsigned long chain_prune_time;
    };
    int rmap_hlist_len;             // 共享计数（负值表示链式节点）
    int nid;                        // NUMA 节点
};

// mm/ksm.c:201
struct ksm_rmap_item {
    struct ksm_rmap_item *rmap_list;   // mm_slot 中的 rmap 链表
    union {
        struct anon_vma *anon_vma;     // 稳定树中的反向映射
        int nid;                       // 不稳定树中的 NUMA 节点
    };
    struct mm_struct *mm;              // 所属进程
    unsigned long address;             // 虚拟地址
    unsigned int oldchecksum;          // 旧的校验和
    rmap_age_t age;                    // 扫描年龄
    rmap_age_t remaining_skips;        // 剩余跳过次数
    union {
        struct rb_node node;           // 不稳定树节点
        struct {                       // 稳定树链表节点
            struct ksm_stable_node *head;
            struct hlist_node hlist;
        };
    };
};
```

**数据结构层次**：
```
mm_slot (哈希表)
  └─ rmap_list (链表) → 多个 ksm_rmap_item
       ├─ 稳定树中的 rmap_item → hlist 链入 ksm_stable_node
       └─ 不稳定树中的 rmap_item → rb_node 链入红黑树

stable_tree (红黑树，按 PFN 排序)
  └─ ksm_stable_node
       ├─ kpfn: 稳定页面物理帧号
       └─ hlist: 共享该页面的所有 rmap_item

unstable_tree (红黑树，按校验和排序)
  └─ ksm_rmap_item (通过 node 字段)
       └─ oldchecksum: 待比较的校验和
```

#### 20.3.2 KSM 内核线程

```c
// mm/ksm.c:2801
static int ksm_scan_thread(void *nothing)
{
    set_user_nice(current, 5);  // 低优先级
    while (!kthread_should_stop()) {
        mutex_lock(&ksm_thread_mutex);
        if (ksmd_should_run())
            ksm_do_scan(ksm_thread_pages_to_scan);  // 默认 100 页
        mutex_unlock(&ksm_thread_mutex);
        // 休眠等待下一个周期
        wait_event_freezable_timeout(ksm_iter_wait, ...);
    }
}

// mm/ksm.c:2780
static void ksm_do_scan(unsigned int scan_npages)
{
    while (scan_npages-- && likely(!freezing(current))) {
        cond_resched();
        rmap_item = scan_get_next_rmap_item(&page);  // 获取下一个待扫描页面
        if (!rmap_item) return;
        cmp_and_merge_page(page, rmap_item);          // 比较并尝试合并
        put_page(page);
        ksm_pages_scanned++;
    }
}
```

**sysfs 控制参数**（`/sys/kernel/mm/ksm/`）：
| 参数 | 默认值 | 说明 |
|------|--------|------|
| `run` | 0 | 0=停止, 1=运行, 2=取消合并 |
| `sleep_millisecs` | 20 | 扫描间隔（ms） |
| `pages_to_scan` | 100 | 每次扫描页数 |
| `max_page_sharing` | 256 | 单页最大共享数 |
| `merge_across_nodes` | 1 | 是否跨 NUMA 节点合并 |
| `use_zero_pages` | 0 | 是否合并零页 |

#### 20.3.3 页面比较与合并

```c
// mm/ksm.c:2248
static void cmp_and_merge_page(struct page *page, struct ksm_rmap_item *rmap_item)
{
    // 1. 检查是否已在稳定树中
    stable_node = folio_stable_node(folio);
    if (stable_node) {
        if (stable_node->head != &migrate_nodes)  // 检查 NUMA 有效性
            return;
    }

    // 2. 计算校验和
    checksum = calc_checksum(page);
    if (rmap_item->oldchecksum != checksum) {
        rmap_item->oldchecksum = checksum;  // 页面变化频繁，暂不处理
        return;
    }

    // 3. 尝试合并到零页
    try_to_merge_with_zero_page(rmap_item, page);

    // 4. 在稳定树中搜索相同页面
    kfolio = stable_tree_search(page);
    if (kfolio) {
        err = try_to_merge_with_ksm_page(rmap_item, page, &kfolio->page);
        if (!err) {
            stable_tree_append(rmap_item, folio_stable_node(kfolio), ...);
        }
        return;
    }

    // 5. 在不稳定树中搜索或插入
    tree_rmap_item = unstable_tree_search_insert(rmap_item, page, &tree_page);
    if (tree_rmap_item) {
        try_to_merge_two_pages(rmap_item, page, tree_rmap_item, tree_page);
        // 合并成功后将稳定节点插入稳定树
    }
}
```

**KSM 合并流程**：
```
cmp_and_merge_page()
  ├─ 已在稳定树？→ 跳过（已合并）
  ├─ 校验和变化？→ 更新校验和，等待下次扫描
  ├─ 尝试合并到零页
  ├─ 稳定树搜索：找到相同页面？
  │    └─ 是 → try_to_merge_with_ksm_page → 加入稳定树
  └─ 不稳定树搜索：
       └─ 找到匹配？→ try_to_merge_two_pages → 合并 → 移入稳定树
       └─ 未找到 → 将当前页面插入不稳定树

try_to_merge_one_page(vma, page, kpage)
  ├─ 检查 page != kpage
  ├─ 锁定 folio
  ├─ 拆分大页（如需要）
  ├─ write_protect_page(vma, folio, &orig_pte)  // 写保护源页
  └─ pages_identical(page, kpage)  // 逐字节比较页面内容
       └─ 匹配 → replace_page(vma, page, kpage, orig_pte)  // 替换 PTE
```

#### 20.3.4 稳定树与不稳定树

- **稳定树（stable tree）**：按 `kpfn`（物理帧号）排序的红黑树，存储已合并的 KSM 页面
  - 每个节点（`ksm_stable_node`）通过 `hlist` 关联所有映射到此页面的 `rmap_item`
  - 页面被共享后，写访问会触发 COW 分离
- **不稳定树（unstable tree）**：按校验和排序的红黑树，存储待比较的候选页面
  - 每次扫描后会重建，防止过期页面积聚
  - 找到两个校验和相同的页面后，进行逐字节比较

### 20.4 Large Folios 优化

#### 批量引用检查优化

Linux 7.0 在文件页缓存中增强了对大页（Large Folios）的支持，folio 大小从单页扩展到 2~64 页不等。

```
传统路径（逐页检查）：
  folio_referenced()
    └─ 遍历每个子页 → 检查每个 PTE → 累加引用计数

优化路径（批量检查）：
  folio_referenced()
    └─ 批量检查 → 一次遍历多个 PTE → 提前终止
```

**性能提升**：大文件页回收速度提升 **50% 至 75%**，尤其对数据库、大数据分析等大内存工作负载效果显著。

#### Gigantic Folio 分配加速

针对 1GB 巨页分配，Linux 7.0 通过优化避免不必要的工作：

| 版本 | 分配 120 个 1GB 巨页所需时间 |
|------|------|
| 旧版 | 3.6 秒 |
| Linux 7.0 | **0.43 秒**（8.4x 加速） |

**优化方法**：
- 减少页面初始化时的冗余操作
- 优化大页分配路径，跳过不必要的检查
- 改进伙伴系统的连续页面分配算法

---

## 21. 内存错误处理（OOM/故障/泄漏）

### 21.1 OOM Killer

文件：`mm/oom_kill.c`（1,273 行）

当内存严重不足且无法通过回收释放时，OOM Killer 选择一个进程终止以释放内存。

#### 21.1.1 触发流程

```
__alloc_pages_may_oom(gfp_mask, order, ...)    // 页面分配器检测到 OOM
  └─ out_of_memory(oc)                          // 进入 OOM 主逻辑
       ├─ select_bad_process(oc)                // 选择要杀死的进程
       │    └─ oom_evaluate_task(p, oc)         // 评估每个进程
       │         └─ oom_badness(p, oc)          // 计算 badness 分数
       ├─ dump_tasks(oc)                        // 打印所有进程内存状态
       ├─ oom_kill_process(oc, message)          // 发送 SIGKILL
       │    └─ __oom_kill_process(victim, msg)  // 实际执行 kill
       └─ 唤醒 oom_reaper 回收页面
```

#### 21.1.2 进程选择 — oom_badness

```c
// mm/oom_kill.c:365 — 选择过程
static void select_bad_process(struct oom_control *oc)
{
    oc->chosen_points = LONG_MIN;
    if (is_memcg_oom(oc))
        mem_cgroup_scan_tasks(oc->memcg, oom_evaluate_task, oc);
    else {
        // 遍历所有进程
        for_each_process(p)
            if (oom_evaluate_task(p, oc))
                break;
    }
}
```

**badness 分数计算**：通过 `oom_evaluate_task` → `oom_badness` 计算，分数越高越可能被选：

```
badness(p) = (total_vm / 2) + (rss) + (swapents) + pte_mapped
             最终分数 = 原始分数 / sqrt(cpu_time_seconds + 1)
```

评分规则：
- `oom_score_adj` 为 `OOM_SCORE_ADJ_MIN`（-1000）的进程被豁免
- `oom_score_adj` 为 `OOM_SCORE_ADJ_MAX`（+1000）的进程总是被选中
- 长时间运行的进程获得惩罚降低（`/ sqrt(cpu_time + 1)`）
- root 进程获得 3% 的分数加成
- 子进程较多的进程获得额外加分

#### 21.1.3 执行杀死

```c
// mm/oom_kill.c:928
static void __oom_kill_process(struct task_struct *victim, const char *message)
{
    // 1. 查找有 mm_struct 的线程
    p = find_lock_task_mm(victim);

    // 2. 发送 SIGKILL 信号
    do_send_sig_info(SIGKILL, SEND_SIG_PRIV, victim, PIDTYPE_TGID);
    mark_oom_victim(victim);

    // 3. 杀死共享 mm 的兄弟进程（防止 mmap_lock 死锁）
    for_each_process(p) {
        if (process_shares_mm(p, mm))
            do_send_sig_info(SIGKILL, ...);
    }

    // 4. 唤醒 oom_reaper 回收页面
    if (can_oom_reap)
        queue_oom_reaper(victim);
}
```

#### 21.1.4 OOM Reaper 内核线程

OOM killer 发送 SIGKILL 后，可能需要等待进程退出释放内存。OOM Reaper 是一个独立内核线程，负责异步回收 victim 的内存。

```
oom_reaper()                          // 内核线程
  └─ wait_event(oom_reaper_wait)      // 等待 victim
  └─ oom_reap_task(tsk)               // 回收 victim 的内存
       └─ oom_reap_task_mm(tsk, mm)   // 实际回收
            ├─ 尝试 mmap_read_trylock(mm)
            ├─ 遍历 VMA，unmap 所有匿名页
            ├─ 释放页面到伙伴系统
            └─ 设置 MMF_OOM_SKIP 标记
```

```c
// mm/oom_kill.c:619
static void oom_reap_task(struct task_struct *tsk)
{
    int attempts = 0;
    // 最多重试 MAX_OOM_REAP_RETRIES（10）次
    while (attempts++ < MAX_OOM_REAP_RETRIES && !oom_reap_task_mm(tsk, mm))
        schedule_timeout_idle(HZ/10);  // 每次等待 100ms
    // 设置 MMF_OOM_SKIP 防止重复回收
    mm_flags_set(MMF_OOM_SKIP, mm);
}
```

#### 21.1.5 OOM 控制结构

```c
// include/linux/oom.h
struct oom_control {
    struct zonelist *zonelist;       // 内存域列表
    nodemask_t *nodemask;            // 允许的 NUMA 节点
    struct mem_cgroup *memcg;        // memcg 上下文（可为 NULL）
    const gfp_t gfp_mask;            // 分配掩码
    const int order;                 // 分配阶
    const enum oom_constraint constraint;  // OOM 约束类型
    struct task_struct *chosen;      // 选中的 victim
    long chosen_points;              // victim 的分数
};
```

**OOM 约束类型**：
| 约束 | 说明 |
|------|------|
| CONSTRAINT_NONE | 无约束，全局 OOM |
| CONSTRAINT_CPUSET | 受 cpuset 限制 |
| CONSTRAINT_MEMORY_POLICY | 受 NUMA 内存策略限制 |
| CONSTRAINT_MEMCG | 受 memcg 限制 |

### 21.2 内存故障处理（HWPoison）

文件：`mm/memory-failure.c`（2,970 行），`mm/hwpoison-inject.c`

处理硬件内存错误（ECC 校验失败、MCE 等），标记损坏页面，通知相关进程。

#### 21.2.1 入口函数

```c
// mm/memory-failure.c:2342
int memory_failure(unsigned long pfn, int flags)
{
    // 1. 检查 sysctl_memory_failure_recovery（是否允许恢复）
    if (!sysctl_memory_failure_recovery)
        panic("Memory failure on page %lx", pfn);

    // 2. 获取 PFN 对应的页面
    p = pfn_to_online_page(pfn);
    if (!p) {
        // 处理非在线页面（设备内存、平台页面等）
        if (pfn_valid(pfn))
            res = memory_failure_dev_pagemap(pfn, flags, pgmap);
        else if (!pfn_valid(pfn) && !arch_is_platform_page(PFN_PHYS(pfn)))
            res = memory_failure_pfn(pfn, flags);
        goto unlock_mutex;
    }

try_again:
    // 3. 处理 hugetlb 页面
    res = try_memory_failure_hugetlb(pfn, flags, &hugetlb);
    if (hugetlb) goto unlock_mutex;

    // 4. 标记页面为 HWPoison
    if (TestSetPageHWPoison(p)) {
        // 已标记 → 如果是 MF_ACTION_REQUIRED 则立即杀死进程
        if (flags & MF_ACTION_REQUIRED)
            res = kill_accessing_process(current, pfn, flags);
        goto unlock_mutex;
    }

    // 5. 获取页面引用，处理不同页面状态
    res = get_hwpoison_page(p, flags);
    if (!res) {
        // 空闲页面 → 从伙伴系统移除
        if (is_free_buddy_page(p)) {
            if (take_page_off_buddy(p)) {
                page_ref_inc(p);
                res = MF_RECOVERED;
            }
        }
    }
    // ... 后续处理
}
```

#### 21.2.2 完整故障处理流程

```
memory_failure(pfn, flags)
  ├─ pfn_to_online_page(pfn)             // 获取 struct page
  ├─ TestSetPageHWPoison(p)              // 原子标记为 HWPoison
  ├─ get_hwpoison_page(p, flags)         // 尝试获取页面引用
  ├─ hwpoison_user_mappings(p, pfn, flags)  // 处理用户空间映射
  │    └─ try_to_unmap(folio, flags)     // 断开所有映射
  │         ├─ anon_vma → rmap_walk     // 遍历匿名页反向映射
  │         └─ 如有映射 → SIGBUS 通知进程
  ├─ hwpoison_isolate_page(p)            // 从 LRU 中隔离页面
  ├─ 根据页面类型处理：
  │    ├─ 匿名页 → 将页面加入伙伴系统（或保留）
  │    ├─ 文件页 → 截断页面缓存
  │    └─ 脏页 → 等待写回完成
  └─ 返回结果
```

#### 21.2.3 结果码

```c
// mm/memory-failure.c 返回值含义
MF_IGNORED  = 0   // 已标记但无法隔离，可能再次触发 MCE
MF_FAILED   = 1   // 已标记但隔离失败，访问会立即触发 MCE
MF_DELAYED  = 2   // 已标记、已 unmap、已从 LRU 移除，下次访问触发缺页
MF_RECOVERED = 3  // 完全恢复：已隔离、已从伙伴系统移除
```

#### 21.2.4 杀死访问进程

```c
// mm/memory-failure.c:823
static int kill_accessing_process(struct task_struct *p, unsigned long pfn,
                                  int flags)
{
    // 通过 page walk 查找损坏页面的映射地址
    walk_page_range(p->mm, 0, TASK_SIZE, &hwpoison_walk_ops, (void *)&priv);
    if (ret == 1 && priv.tk.addr)
        kill_proc(&priv.tk, pfn, flags);  // 发送 SIGBUS
}
```

**注入测试**：`mm/hwpoison-inject.c` 提供调试接口，可通过 debugfs 模拟硬件错误：

```
# 注入一个页面的硬件错误
echo 0x12345 > /sys/kernel/debug/hwpoison/corrupt-pfn
```

### 21.3 kmemleak — 内核内存泄漏检测

文件：`mm/kmemleak.c`（2,346 行）

kmemleak 通过扫描内核内存，检测未引用的已分配对象。它使用"灰阶扫描"算法（类似三色标记 GC）。

#### 21.3.1 核心原理

```
kmemleak 扫描算法：
  1. 将所有已分配对象标记为"灰色"
  2. 扫描所有已知的内存区域（数据段、堆栈、动态分配的内存）
  3. 对于每个指针，如果指向某个灰色对象，将该对象标记为"白色"（已引用）
  4. 扫描结束后，仍为"灰色"的对象 → 泄漏报告
```

#### 21.3.2 扫描流程

```c
// mm/kmemleak.c:1694
static void kmemleak_scan(void)
{
    // 1. 准备：清除所有对象的灰色标记
    // 2. 创建灰色列表
    // 3. 扫描已知内存区域：
    scan_block(_sdata, _edata, NULL, ...);    // 数据段
    scan_block(__bss_start, __bss_stop, ...); // BSS 段
    // 遍历所有任务的内核栈
    for_each_process(p)
        scan_block(ksp, ksp + THREAD_SIZE, ...);
    // 4. 扫描灰色列表（传递引用）
    scan_gray_list();
    // 5. 报告未引用的对象
    // 6. 清除误报
}
```

#### 21.3.3 使用方法

```
# 触发扫描
echo scan > /sys/kernel/debug/kmemleak

# 查看泄漏报告
cat /sys/kernel/debug/kmemleak

# 清除误报
echo clear > /sys/kernel/debug/kmemleak

# 关闭自动扫描
echo 0 > /sys/kernel/debug/kmemleak_scan_enabled
```

#### 21.3.4 关键数据结构

```c
// mm/kmemleak.c 内部对象
struct kmemleak_object {
    spinlock_t lock;
    unsigned long flags;             // 对象状态标志
    unsigned long pointer;           // 分配的内存地址
    size_t size;                     // 分配大小
    int min_count;                   // 最小引用计数
    int count;                       // 引用计数
    u32 checksum;                    // 内容校验和（防误报）
    depot_stack_handle_t trace_handle; // 分配栈跟踪
    struct list_head object_list;    // 全局对象链表
    struct list_head gray_list;      // 灰色列表
    struct rcu_head rcu;
};
```

---

## 22. DAMON 数据访问监控

### 22.1 概述

DAMON（Data Access Monitoring）是 Linux 内核的数据访问监控框架，位于 `mm/damon/` 子目录，共 11 个源文件。

| 文件 | 行数 | 功能 |
|------|------|------|
| `mm/damon/core.c` | 3,032 | 核心监控逻辑 |
| `mm/damon/sysfs-schemes.c` | 2,876 | sysfs 方案接口 |
| `mm/damon/sysfs.c` | 2,117 | sysfs 控制接口 |
| `mm/damon/vaddr.c` | 1,036 | 虚拟地址监控 |
| `mm/damon/lru_sort.c` | 498 | LRU 排序方案 |
| `mm/damon/reclaim.c` | 400 | 主动回收方案 |
| `mm/damon/paddr.c` | 383 | 物理地址监控 |
| `mm/damon/stat.c` | 322 | 统计信息 |

### 22.2 核心数据结构

#### 22.2.1 damon_ctx — 监控上下文

```c
// include/linux/damon.h:780
struct damon_ctx {
    struct damon_attrs attrs;               // 监控参数（采样间隔、聚合间隔等）

    // 内部状态
    unsigned long passed_sample_intervals;  // 已通过的采样间隔数
    unsigned long next_aggregation_sis;     // 下次聚合的采样间隔编号
    unsigned long next_ops_update_sis;      // 下次操作更新的采样间隔编号

    // 同步控制
    struct completion kdamond_started;      // 等待 kdamond 启动完成
    struct mutex kdamond_lock;              // 保护 kdamond 字段
    struct task_struct *kdamond;            // 监控内核线程

    // 方案控制
    struct list_head call_controls;         // 回调控制列表
    struct mutex call_controls_lock;
    struct damos_walk_control *walk_control;
    struct mutex walk_control_lock;

    // 操作接口
    struct damon_operations ops;            // 监控操作（vaddr/paddr）
    unsigned long addr_unit;                // 地址单位
    unsigned long min_region_sz;            // 最小区域大小

    // 目标和方案
    struct list_head adaptive_targets;      // 监控目标列表
    struct list_head schemes;               // 操作方案列表
};
```

**监控参数（attrs）**：
```c
struct damon_attrs {
    unsigned long sample_interval;     // 采样间隔（us），默认 5000
    unsigned long aggr_interval;       // 聚合间隔（us），默认 100000
    unsigned long ops_update_interval; // 操作更新间隔（us），默认 60000000（60s）
    unsigned long min_nr_regions;      // 最小区域数，默认 10
    unsigned long max_nr_regions;      // 最大区域数，默认 1000
    struct damon_intervals_goal intervals_goal; // 间隔调整目标
};
```

#### 22.2.2 damon_region — 监控区域

```c
// include/linux/damon.h:76
struct damon_region {
    struct damon_addr_range ar;          // 地址范围 [start, end)
    unsigned long sampling_addr;         // 下次采样的地址
    unsigned int nr_accesses;            // 访问频率（聚合间隔内）
    unsigned int nr_accesses_bp;         // 基点表示的访问频率（0.01%）
    struct list_head list;               // 区域链表节点
    unsigned int age;                    // 区域年龄（聚合间隔数）
    unsigned int last_nr_accesses;       // 上一次的访问频率
};
```

#### 22.2.3 damos — 操作方案

```c
// include/linux/damon.h:494
struct damos {
    struct damos_access_pattern pattern;  // 目标访问模式
    enum damos_action action;             // 操作类型
    unsigned long apply_interval_us;      // 应用间隔（us）
    struct damos_quota quota;             // 配额控制
    struct damos_watermarks wmarks;       // 水位线控制
    union {
        struct { int target_nid; struct damos_migrate_dests migrate_dests; };  // 迁移目标
    };
    struct list_head core_filters;        // 核心过滤器
    struct list_head ops_filters;         // 操作层过滤器
    struct damos_stat stat;               // 统计信息
    unsigned long max_nr_snapshots;       // 快照上限
    struct list_head list;                // 方案链表节点
};
```

**访问模式**：
```c
struct damos_access_pattern {
    unsigned long min_sz_region;     // 区域最小大小
    unsigned long max_sz_region;     // 区域最大大小
    unsigned int min_nr_accesses;    // 最小访问频率
    unsigned int max_nr_accesses;    // 最大访问频率
    unsigned int min_age_region;     // 区域最小年龄
    unsigned int max_age_region;     // 区域最大年龄
};
```

**操作类型**：
```c
enum damos_action {
    DAMOS_ACTION_WILLNEED,       // 预取到内存
    DAMOS_ACTION_COLD,           // 标记为冷页
    DAMOS_ACTION_PAGEOUT,        // 换出到 swap
    DAMOS_ACTION_HUGEPAGE,       // 合并为 THP
    DAMOS_ACTION_NOHUGEPAGE,     // 拆分 THP
    DAMOS_ACTION_LRU_PRIO,       // LRU 优先级提升
    DAMOS_ACTION_LRU_DEPRIO,     // LRU 优先级降级
    DAMOS_ACTION_MIGRATE_HOT,    // 迁移热页
    DAMOS_ACTION_MIGRATE_COLD,   // 迁移冷页
    DAMOS_ACTION_STAT,           // 仅统计
};
```

### 22.3 核心监控循环

#### 22.3.1 kdamond 内核线程

```c
// mm/damon/core.c:2746
static int kdamond_fn(void *data)
{
    struct damon_ctx *ctx = data;

    kdamond_init_ctx(ctx);                   // 初始化上下文
    if (ctx->ops.init) ctx->ops.init(ctx);   // 初始化监控操作

    while (!kdamond_need_stop(ctx)) {
        // 1. 准备访问检查
        if (ctx->ops.prepare_access_checks)
            ctx->ops.prepare_access_checks(ctx);

        // 2. 等待采样间隔
        kdamond_usleep(sample_interval);
        ctx->passed_sample_intervals++;

        // 3. 检查访问
        if (ctx->ops.check_accesses)
            max_nr_accesses = ctx->ops.check_accesses(ctx);

        // 4. 聚合间隔 → 合并区域
        if (ctx->passed_sample_intervals >= next_aggregation_sis)
            kdamond_merge_regions(ctx, max_nr_accesses / 10, sz_limit);

        // 5. 应用方案
        kdamond_call(ctx, false);
        if (!list_empty(&ctx->schemes))
            kdamond_apply_schemes(ctx);

        // 6. 聚合间隔 → 重置统计 & 分割区域
        if (ctx->passed_sample_intervals >= next_aggregation_sis) {
            kdamond_reset_aggregated(ctx);
            kdamond_split_regions(ctx);
        }

        // 7. 操作更新间隔 → 更新操作
        if (ctx->passed_sample_intervals >= next_ops_update_sis) {
            if (ctx->ops.update)
                ctx->ops.update(ctx);
        }
    }
}
```

**主循环时序**：
```
采样间隔 (5ms) → 检查访问 → 累加 nr_accesses
     │
     ▼
聚合间隔 (100ms) → 合并相似区域 → 重置 nr_accesses → 分割过大区域
     │
     ▼
操作更新间隔 (60s) → 更新监控操作（如 VMA 列表）
     │
     ▼
方案应用间隔 → 匹配访问模式 → 执行操作
```

#### 22.3.2 区域合并

```
kdamond_merge_regions(ctx, nr_accesses/10, sz_limit)
  └─ 遍历相邻区域：
       ├─ 如果 nr_accesses 相似（差异 ≤ 阈值）
       ├─ 且合并后不超过 sz_limit
       └─ 合并：damon_merge_two_regions(t, l, r)
            ├─ nr_accesses = 加权平均
            └─ age = 加权平均
```

#### 22.3.3 区域分割

```
kdamond_split_regions(ctx)
  └─ 遍历所有区域：
       └─ 如果 nr_regions < min_nr_regions 或区域过大
            └─ damon_split_region_at(t, r, new_size)
                 ├─ 创建新区域
                 ├─ 设置新地址范围
                 └─ 插入链表
```

### 22.4 方案应用

#### 22.4.1 方案调度

```c
// mm/damon/core.c:2355
static void kdamond_apply_schemes(struct damon_ctx *c)
{
    // 1. 调整配额
    damon_for_each_scheme(s, c)
        damos_adjust_quota(c, s);

    // 2. 遍历所有目标和区域
    damon_for_each_target(t, c) {
        damon_for_each_region_safe(r, next_r, t)
            damon_do_apply_schemes(c, t, r);
    }

    // 3. 完成 walking，更新统计
    damon_for_each_scheme(s, c) {
        damos_walk_complete(c, s);
        s->next_apply_sis = ...;  // 更新下次应用时间
    }
}
```

#### 22.4.2 方案匹配与执行

```c
// mm/damon/core.c:1998
static void damon_do_apply_schemes(struct damon_ctx *c,
                                   struct damon_target *t,
                                   struct damon_region *r)
{
    damon_for_each_scheme(s, c) {
        // 1. 检查是否到应用时间
        if (c->passed_sample_intervals < s->next_apply_sis)
            continue;

        // 2. 检查水位线
        if (!s->wmarks.activated)
            continue;

        // 3. 检查配额
        if (quota->esz && quota->charged_sz >= quota->esz)
            continue;

        // 4. 检查是否已处理过该区域（避免重复）
        if (damos_skip_charged_region(t, &r, s, c->min_region_sz))
            continue;

        // 5. 匹配访问模式 → 执行操作
        if (damos_valid_target(c, t, r, s))
            damos_apply_scheme(c, t, r, s);
    }
}
```

#### 22.4.3 配额控制

```c
struct damos_quota {
    unsigned long ms;             // 时间配额（ms/聚合间隔）
    unsigned long sz;             // 大小配额（bytes/聚合间隔）
    unsigned long reset_interval; // 配额重置间隔

    // 优先权（基于目标评分）
    unsigned long weight_sz;      // 区域大小权重
    unsigned long weight_nr_accesses; // 访问频率权重
    unsigned long weight_age;     // 年龄权重

    // 内部状态
    unsigned long charged_sz;     // 已配给大小
    unsigned long charged_from;   // 开始时间
    unsigned long esz;            // 有效配额大小
};
```

**配额调整算法**（`damos_adjust_quota`）：基于反馈循环的 PID 控制器

```
damos_adjust_quota(c, s)
  ├─ 计算本周期消耗配额（charged_sz）
  ├─ 计算目标分数（基于 sz/nr_accesses/age 权重）
  └─ damon_feed_loop_next_input(last_input, score)
       ├─ 目标分数 = 10000
       ├─ 如果 score > goal → 减少配额
       ├─ 如果 score < goal → 增加配额
       └─ 反馈调整公式：next = last * ((goal - score) / goal + 1)
```

### 22.5 监控操作实现

#### 22.5.1 虚拟地址空间监控（vaddr）

文件：`mm/damon/vaddr.c`（1,036 行）

```
vaddr 操作：
  init() → 初始化，设置初始监控区域（VMA 列表）
  prepare_access_checks() → 记录每个区域的采样地址
  check_accesses() → 检查 PTE 的 Accessed 位
  target_valid() → 验证目标进程是否仍然存在
  cleanup() → 清理资源
```

**访问检查机制**：
```
prepare_access_checks():
  └─ 对每个区域，选择一个采样地址
  └─ 记录该地址对应的 PTE

check_accesses():
  └─ 检查之前记录的 PTE 的 Accessed 位
  └─ 如果被访问过 → nr_accesses++
  └─ 清除 PTE 的 Accessed 位
```

#### 22.5.2 物理地址空间监控（paddr）

文件：`mm/damon/paddr.c`（383 行）

```
paddr 操作：
  prepare_access_checks() → 保留页面（阻止回收）
  check_accesses() → 检查页面空闲位图
  target_valid() → 始终有效
```

### 22.6 内置方案

#### 22.6.1 DAMON_LRU_SORT

文件：`mm/damon/lru_sort.c`（498 行）

将冷热页分离到不同的 LRU 链表，优化页面回收效率：

```
damon_lru_sort() 内核线程：
  ├─ scheme1: 热页 → LRU 优先级提升（LRU_PRIO）
  │    └─ 条件：nr_accesses > 热阈值, age > 年龄阈值
  └─ scheme2: 冷页 → LRU 优先级降级（LRU_DEPRIO）
       └─ 条件：nr_accesses == 0, age > 冷阈值
```

#### 22.6.2 DAMON_RECLAIM

文件：`mm/damon/reclaim.c`（400 行）

主动回收冷内存页到 swap：

```
damon_reclaim() 内核线程：
  └─ scheme: 冷页 → PAGEOUT（换出）
       └─ 条件：nr_accesses == 0, age > 冷阈值
       └─ 配额控制：默认 10ms/聚合间隔，防止过度回收
```

### 22.7 sysfs 接口

DAMON 通过 `/sys/kernel/mm/damon/` 暴露用户空间接口：

```
/sys/kernel/mm/damon/
  ├─ admin/
  │    ├─ contexts/        # 监控上下文列表
  │    │    └─ N/
  │    │         ├─ monitoring_attrs/  # 监控参数
  │    │         ├─ targets/          # 监控目标
  │    │         │    └─ N/
  │    │         │         ├─ pid_target  # 目标 PID
  │    │         │         └─ regions/   # 手动指定区域
  │    │         └─ schemes/          # 操作方案
  │    │              └─ N/
  │    │                   ├─ action      # 操作类型
  │    │                   ├─ access_pattern/  # 访问模式
  │    │                   ├─ quotas/    # 配额
  │    │                   ├─ watermarks/ # 水位线
  │    │                   └─ filters/  # 过滤器
  │    └─ kdamond/        # kdamond 控制
  ├─ lru_sort/            # LRU 排序控制
  └─ reclaim/             # 主动回收控制
```

**DAMON 架构总览**：
```
┌─────────────────────────────────────────────────────────┐
│                   用户空间接口                            │
│  sysfs (/sys/kernel/mm/damon/)                          │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│                    DAMON Core                            │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐  │
│  │ kdamond_fn() │  │ 区域管理     │  │ 方案管理      │  │
│  │ 采样/聚合/   │  │ 合并/分割    │  │ 匹配/执行/    │  │
│  │ 周期性循环   │  │ 自适应      │  │ 配额/水位线  │  │
│  └──────┬───────┘  └──────────────┘  └───────────────┘  │
└─────────┼────────────────────────────────────────────────┘
          │
┌─────────▼────────────────────────────────────────────────┐
│              DAMON Operations Layer                      │
│  ┌──────────────────┐  ┌──────────────────┐             │
│  │ vaddr (virt addr) │  │ paddr (phys addr)│             │
│  │ - VMA 跟踪       │  │ - 空闲页检查     │             │
│  │ - PTE Accessed 位 │  │ - 页面迁移       │             │
│  └──────────────────┘  └──────────────────┘             │
└──────────────────────────────────────────────────────────┘

---

## 23. 调试与安全工具

### 23.1 KASAN — 内核地址消毒器

文件：`mm/kasan/` 子目录，共 13 个源文件 + 头文件

KASAN（Kernel Address Sanitizer）检测内存越界访问和 use-after-free 错误，是内核开发中最常用的内存错误检测工具。

#### 23.1.1 检测原理：影子内存

KASAN 使用影子内存（shadow memory）跟踪每个内存字节的可访问性：

```c
// 核心检测原理
// 每 8 字节内存对应 1 字节影子，标记可访问性
#define KASAN_SHADOW_SCALE_SHIFT 3   // 8:1 映射比例
#define KASAN_SHADOW_OFFSET _AC(CONFIG_KASAN_SHADOW_OFFSET, UL)

// 影子字节编码
#define KASAN_SHADOW_FREE      0xFF  // 已释放（use-after-free 检测）
#define KASAN_SHADOW_POISON    0xFE  // 已毒化（越界检测）
#define KASAN_SHADOW_SLAB_FREE_META 0xFC  // slab 释放元数据
#define KASAN_SHADOW_REDZONE   0xFA  // 红色区域（越界检测）
#define KASAN_SHADOW_STACK     0xF3  // 栈变量
```

**检测原理**：

```
内存布局：
  [对象数据 (8字节)] [红色区域 (8字节)] [对象数据 (8字节)] ...
         │                    │                    │
         ▼                    ▼                    ▼
  影子内存：  0x00            0xFA                0x00

越界访问检测：
  访问偏移 8 字节 → 读取影子字节 0xFA → 检测到越界 → 报告错误

Use-after-free 检测：
  释放后访问 → 读取影子字节 0xFF → 检测到释放 → 报告错误
```

#### 23.1.2 元数据结构

```c
// mm/kasan/kasan.h:264
struct kasan_alloc_meta {
    struct kasan_track alloc_track;          // 分配栈跟踪
    depot_stack_handle_t aux_stack[2];       // 辅助栈信息
};

// mm/kasan/kasan.h:289
struct kasan_free_meta {
    struct qlist_node quarantine_link;       // 隔离队列链接
    struct kasan_track free_track;           // 释放栈跟踪
};
```

#### 23.1.3 核心 API

```c
// 分配/释放检测
void __kasan_unpoison_range(const void *address, size_t size);  // 取消毒化（分配后）
void __kasan_poison_pages(struct page *page, unsigned int order, bool init);  // 毒化页面
void __kasan_poison_slab(struct slab *slab);                      // 毒化 slab
void __kasan_unpoison_new_object(struct kmem_cache *cache, void *object); // 新对象
void __kasan_poison_new_object(struct kmem_cache *cache, void *object);  // 释放对象
void __kasan_kfree_large(void *ptr, unsigned long ip);            // 大对象释放
void __kasan_mempool_unpoison_pages(struct page *page, unsigned int order, ...); // mempool

// 栈跟踪
void __asan_*_store(void *addr);    // 写访问检测
void __asan_*_load(void *addr);     // 读访问检测
```

#### 23.1.4 支持模式

| 模式 | 文件名 | 检测范围 | 性能开销 | 硬件要求 |
|------|--------|----------|----------|----------|
| Generic | `generic.c` | 越界 + UAF + 栈 + 全局 | ~2x | 无 |
| SW_TAGS | `sw_tags.c` | 越界 + UAF | ~30% | ARM64 |
| HW_TAGS | `hw_tags.c` | 越界 + UAF | ~1% | ARM64 MTE |

**Generic 模式特点**：
- 红色区域（redzone）：在对象周围插入毒化区域
- 隔离队列（quarantine）：释放后延迟重用，检测 UAF
- 栈跟踪：记录分配和释放调用栈
- 检测粒度：字节级别

**HW_TAGS 模式特点**：
- 利用 ARM64 MTE（Memory Tagging Extension）硬件
- 分配随机标签，标签不匹配时触发硬件异常
- 无红色区域和隔离队列开销

### 23.2 KFENCE — 内核门栏

文件：`mm/kfence/` 子目录，共 5 个源文件

| 文件 | 行数 | 功能 |
|------|------|------|
| `core.c` | ~1335 | KFENCE 核心逻辑：内存池管理、分配/释放、页错误处理、初始化、sysfs 接口 |
| `kfence.h` | ~150 | 核心数据结构定义：`kfence_metadata`、`kfence_track`、错误类型枚举 |
| `report.c` | ~332 | 错误报告生成：越界、UAF、内存损坏、无效释放等错误输出 |
| `kfence_test.c` | ~1200 | KFENCE 单元测试套件 |
| `Makefile` | 构建配置 | |

KFENCE（Kernel Electric Fence）是低开销的内存错误检测器，使用**采样方式**检测越界访问和 use-after-free，适合生产环境部署。

#### 23.2.1 核心数据结构

```c
// mm/kfence/kfence.h:57
struct kfence_metadata {
    struct list_head list;              // 空闲链表节点（受 kfence_freelist_lock 保护）
    struct rcu_head rcu_head;           // RCU 延迟释放（SLAB_TYPESAFE_BY_RCU 支持）

    raw_spinlock_t lock;                // 保护以下数据（并发：alloc/free/page_fault 三者互斥）

    enum kfence_object_state state;     // 对象状态：UNUSED / ALLOCATED / RCU_FREEING / FREED
    unsigned long addr;                 // 对象地址（页对齐，不变式：ALIGN_DOWN(addr, PAGE_SIZE) 恒定）
    size_t size;                        // 原始分配大小
    struct kmem_cache *cache;           // 所属 slab 缓存（为 NULL 表示缓存已销毁）
    unsigned long unprotected_page;     // 触发错误的页面（仅记录一个地址）

    struct kfence_track alloc_track;    // 分配栈跟踪
    struct kfence_track free_track;     // 释放栈跟踪
    u32 alloc_stack_hash;               // 分配栈哈希（用于覆盖检查 Bloom 过滤器）
#ifdef CONFIG_MEMCG
    struct slabobj_ext obj_exts;        // Memory cgroup 对象扩展
#endif
};

// 栈跟踪信息结构体
struct kfence_track {
    pid_t pid;                          // 进程 PID
    int cpu;                            // CPU 编号
    u64 ts_nsec;                        // 时间戳（纳秒，与 printk 同源）
    int num_stack_entries;              // 栈条目数
    unsigned long stack_entries[KFENCE_STACK_DEPTH];  // 栈帧数组（最大 64 层）
};
```

**对象状态机**：

```
UNUSED ──→ ALLOCATED ──→ RCU_FREEING ──→ FREED ──→ UNUSED (循环)
                │             │
                └─────────────┘ (直接释放)
```

**地址到元数据映射**：

```c
// 内存池布局（每对象占用 2 页）：
//   [保护页] [对象页] [保护页] [对象页] ...
//     index 0    1       2       3
//
// 元数据索引：index = (addr - pool_base) / (PAGE_SIZE * 2) - 1
static inline struct kfence_metadata *addr_to_metadata(unsigned long addr)
{
    long index;
    if (!is_kfence_address((void *)addr))
        return NULL;
    index = (addr - (unsigned long)__kfence_pool) / (PAGE_SIZE * 2) - 1;
    if (index < 0 || index >= CONFIG_KFENCE_NUM_OBJECTS)
        return NULL;
    return &kfence_metadata[index];
}
```

#### 23.2.2 内存池布局

```
KFENCE 内存池（__kfence_pool）：
  ┌──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
  │ 前保护页 │ 对象 0   │ 后保护页 │ 对象 1   │ 后保护页 │ ...      │ 共 N 个对象
  │ (2页)    │ (1页)    │ (1页)    │ (1页)    │ (1页)    │          │
  └──────────┴──────────┴──────────┴──────────┴──────────┴──────────┘
  │← 2页 →│←─── 每对象 2 页 ──→│←─── 每对象 2 页 ──→│

总大小：KFENCE_POOL_SIZE = (CONFIG_KFENCE_NUM_OBJECTS + 1) * 2 * PAGE_SIZE

对象在页内的分配位置随机化：
  - 左侧分配：对象从页起始地址开始
  - 右侧分配：对象从页末尾对齐地址开始（meta->addr += PAGE_SIZE - size）
```

**保护机制**：
- 每个对象两侧的保护页通过 `kfence_protect_page()` 设置为 unmapped
- 越界访问 → 访问保护页 → 触发页错误 → `kfence_handle_page_fault()` 报告
- 释放后对象页被保护 → 释放后访问 → 触发页错误 → 报告 UAF

#### 23.2.3 采样机制

KFENCE 使用**泊松过程采样**，默认每 100ms 允许一次 KFENCE 分配：

```c
// include/linux/kfence.h:83 — 分配快速路径（内联，静态分支）
static __always_inline void *kfence_alloc(struct kmem_cache *s, size_t size, gfp_t flags)
{
    // 1. 静态分支检查：大部分时间跳过
    if (!static_branch_unlikely(&kfence_allocation_key))
        return NULL;
    // 2. 快速原子检查：kfence_allocation_gate > 0 时跳过
    if (likely(atomic_read(&kfence_allocation_gate) > 0))
        return NULL;
    // 3. 慢速路径：原子递增后检查是否命中
    return __kfence_alloc(s, size, flags);
}
```

**定时器驱动采样**：

```
toggle_allocation_gate()  [定时器回调，每 sample_interval 触发一次]
  │
  ├─ atomic_set(&kfence_allocation_gate, -kfence_burst)  // 重置门控计数器
  │
  ├─ static_branch_enable(&kfence_allocation_key)         // 启用静态分支
  │
  ├─ wait_event_idle(...)  // 等待一次分配命中
  │
  ├─ static_branch_disable(&kfence_allocation_key)        // 禁用静态分支
  │
  └─ queue_delayed_work(..., kfence_sample_interval)       // 重新调度定时器
```

**分配门控时序**：

```
时间线：    采样间隔 (100ms)         采样间隔 (100ms)
           │                        │
 门控值：  -kfence_burst → 0 → 1 → 2 → ... → reset → -burst → 0 → 1 → ...

 关键点：
   - kfence_allocation_gate = 1 时，下一次 __kfence_alloc() 会命中
   - 原子操作确保只有一个线程能命中（atomic_inc_return == 1）
   - 命中后通过 irq_work 唤醒定时器线程
```

#### 23.2.4 分配路径

```c
// mm/kfence/core.c:1090
void *__kfence_alloc(struct kmem_cache *s, size_t size, gfp_t flags)
{
    // 1. 快速排除检查
    if (size > PAGE_SIZE)                    // 大对象跳过
        return NULL;
    if ((flags & GFP_ZONEMASK) || ...)       // 非默认 zone 跳过
        return NULL;
    if (s->flags & SLAB_SKIP_KFENCE)         // 标记跳过的缓存
        return NULL;

    // 2. 门控检查（原子递增）
    allocation_gate = atomic_inc_return(&kfence_allocation_gate);
    if (allocation_gate > 1)
        return NULL;                         // 只有 == 1 才命中

    // 3. 覆盖检查（Counting Bloom Filter）
    alloc_stack_hash = get_alloc_stack_hash(stack_entries, num_entries);
    if (should_skip_covered() && alloc_covered_contains(alloc_stack_hash))
        return NULL;                         // 同类型分配过多，跳过

    // 4. 执行受保护分配
    return kfence_guarded_alloc(s, size, flags, stack_entries, ...);
}
```

**kfence_guarded_alloc 核心流程**：

```c
static void *kfence_guarded_alloc(struct kmem_cache *cache, size_t size, gfp_t gfp,
                                  unsigned long *stack_entries, size_t num_stack_entries,
                                  u32 alloc_stack_hash)
{
    // 1. 从空闲链表获取元数据对象
    raw_spin_lock(&kfence_freelist_lock);
    meta = list_first_entry_or_null(&kfence_freelist, ...);
    list_del_init(&meta->list);
    raw_spin_unlock(&kfence_freelist_lock);
    if (!meta) { /* 池满，跳过敏计数器 */ return NULL; }

    // 2. 计算对象地址（随机选择左侧/右侧分配）
    meta->addr = metadata_to_pageaddr(meta);       // 页基地址
    if (random_right_allocate)                      // 50% 概率
        meta->addr += PAGE_SIZE - size;            // 右侧分配
    meta->addr = ALIGN_DOWN(meta->addr, cache->align);

    // 3. 初始化 slab 字段
    slab = virt_to_slab(addr);
    slab->slab_cache = cache;
    slab->objects = 1;

    // 4. 设置金丝雀值（对象两侧填充检测字节）
    set_canary(meta);

    // 5. 内存初始化
    if (slab_want_init_on_alloc(gfp, cache))
        memzero_explicit(addr, size);
    if (cache->ctor)
        cache->ctor(addr);                         // 构造函数

    // 6. 压力测试：随机保护对象本身（模拟故障）
    if (random_fault)
        kfence_protect(meta->addr);

    // 7. 更新统计
    atomic_long_inc(&counters[KFENCE_COUNTER_ALLOCATED]);
    atomic_long_inc(&counters[KFENCE_COUNTER_ALLOCS]);
    return addr;
}
```

#### 23.2.5 释放路径

```c
static void kfence_guarded_free(void *addr, struct kfence_metadata *meta, bool zombie)
{
    // 1. 有效性检查
    raw_spin_lock(&meta->lock);
    if (!kfence_obj_allocated(meta) || meta->addr != (unsigned long)addr) {
        // 无效释放或双释放 → 报告 KFENCE_ERROR_INVALID_FREE
        kfence_report_error(addr, ...);
        return;
    }

    // 2. KCSAN 断言：检测竞态 UAF
    kcsan_begin_scoped_access(addr, PAGE_SIZE, ...);

    // 3. 清除 OOB 保护页
    if (meta->unprotected_page) {
        memzero_explicit(unprotected_page, PAGE_SIZE);
        kfence_protect(unprotected_page);
    }

    // 4. 更新状态为 FREED
    metadata_update_state(meta, KFENCE_OBJECT_FREED, NULL, 0);
    raw_spin_unlock(&meta->lock);

    // 5. 移除 Bloom 过滤器条目
    alloc_covered_add(alloc_stack_hash, -1);

    // 6. 检查金丝雀值（检测内存损坏）
    check_canary(meta);

    // 7. 保护对象页（检测 UAF）
    kfence_protect(meta->addr);

    // 8. 归还到空闲链表
    if (!zombie) {
        list_add_tail(&meta->list, &kfence_freelist);
        atomic_long_dec(&counters[KFENCE_COUNTER_ALLOCATED]);
        atomic_long_inc(&counters[KFENCE_COUNTER_FREES]);
    }
}
```

**RCU 延迟释放**：

```c
void __kfence_free(void *addr)
{
    meta = addr_to_metadata(addr);
    if (unlikely(meta->cache && (meta->cache->flags & SLAB_TYPESAFE_BY_RCU))) {
        // RCU 保护：先标记为 RCU_FREEING，延迟释放
        metadata_update_state(meta, KFENCE_OBJECT_RCU_FREEING, NULL, 0);
        call_rcu(&meta->rcu_head, rcu_guarded_free);
    } else {
        kfence_guarded_free(addr, meta, false);
    }
}
```

#### 23.2.6 金丝雀检测（Canary）

KFENCE 在对象两侧填充金丝雀检测字节，释放时检查是否被修改：

```c
// 金丝雀模式：地址相关，每字节模式 = 0xAA ^ (addr & 0x7)
#define KFENCE_CANARY_PATTERN_U8(addr)  ((u8)0xaa ^ (u8)((unsigned long)(addr) & 0x7))
#define KFENCE_CANARY_PATTERN_U64       ((u64)0xaaaaaaaaaaaaaaaa ^ (u64)(0x0706050403020100))

// 设置金丝雀（分配时调用）
static inline void set_canary(const struct kfence_metadata *meta)
{
    // 对象左侧：从页起始到 meta->addr
    for (addr = pageaddr; addr < meta->addr; addr += sizeof(u64))
        *((u64 *)addr) = KFENCE_CANARY_PATTERN_U64;

    // 对象右侧：从 meta->addr + size 到页末尾
    for (addr = ALIGN_DOWN(meta->addr + size, 8); addr - pageaddr < PAGE_SIZE; addr += 8)
        *((u64 *)addr) = KFENCE_CANARY_PATTERN_U64;
}

// 检查金丝雀（释放时调用）
static void check_canary(const struct kfence_metadata *meta)
{
    // 先按 64 位批量检查，发现不一致再逐字节检查
    for (addr = pageaddr; meta->addr - addr >= 8; addr += 8)
        if (unlikely(*((u64 *)addr) != KFENCE_CANARY_PATTERN_U64))
            break;  // 进入逐字节检查

    for (; addr < meta->addr; addr++)
        if (unlikely(!check_canary_byte((u8 *)addr)))
            break;  // → 报告 KFENCE_ERROR_CORRUPTION
}
```

#### 23.2.7 页错误处理

```c
// mm/kfence/core.c:1240
bool kfence_handle_page_fault(unsigned long addr, bool is_write, struct pt_regs *regs)
{
    page_index = (addr - __kfence_pool) / PAGE_SIZE;

    if (page_index % 2) {
        // ── 奇数页：保护页 → 越界访问（OOB） ──
        // 检查左右两侧的邻接对象
        meta = addr_to_metadata(addr - PAGE_SIZE);
        if (meta && kfence_obj_allocated(meta))
            to_report = meta;  // 右侧越界
        meta = addr_to_metadata(addr + PAGE_SIZE);
        if (meta && kfence_obj_allocated(meta))
            if (!to_report || distance > ...)
                to_report = meta;  // 左侧越界

        error_type = KFENCE_ERROR_OOB;
    } else {
        // ── 偶数页：对象页 → Use-after-free ──
        to_report = addr_to_metadata(addr);
        error_type = KFENCE_ERROR_UAF;
    }

    if (to_report) {
        raw_spin_lock(&to_report->lock);
        to_report->unprotected_page = unprotected_page;
        kfence_report_error(addr, is_write, regs, to_report, error_type);
        raw_spin_unlock(&to_report->lock);
    } else {
        kfence_report_error(addr, is_write, regs, NULL, KFENCE_ERROR_INVALID);
    }

    return kfence_unprotect(addr);  // 解除保护，让访问继续执行
}
```

#### 23.2.8 报告生成

```c
// mm/kfence/report.c
void kfence_report_error(unsigned long address, bool is_write, struct pt_regs *regs,
                         const struct kfence_metadata *meta, enum kfence_error_type type)
{
    // 获取访问栈跟踪
    if (regs)
        stack_trace_save_regs(regs, stack_entries, ...);
    else
        stack_trace_save(stack_entries, ...);

    lockdep_off();  // 避免死锁

    // 打印错误头
    switch (type) {
    case KFENCE_ERROR_OOB:
        pr_err("BUG: KFENCE: out-of-bounds %s in %pS\n", get_access_type(is_write), ...);
        // 显示越界方向和距离
        break;
    case KFENCE_ERROR_UAF:
        pr_err("BUG: KFENCE: use-after-free %s in %pS\n", ...);
        break;
    case KFENCE_ERROR_CORRUPTION:
        pr_err("BUG: KFENCE: memory corruption in %pS\n", ...);
        print_diff_canary(address, 16, meta);  // 显示损坏的字节
        break;
    case KFENCE_ERROR_INVALID_FREE:
        pr_err("BUG: KFENCE: invalid free in %pS\n", ...);
        break;
    case KFENCE_ERROR_INVALID:
        pr_err("BUG: KFENCE: invalid %s in %pS\n", ...);
        break;
    }

    // 打印栈跟踪和对象信息
    stack_trace_print(stack_entries + skipnr, ...);
    kfence_print_object(NULL, meta);  // 打印分配/释放栈

    pr_err("==================================================================\n");
    lockdep_on();
    add_taint(TAINT_BAD_PAGE, LOCKDEP_STILL_OK);
}
```

#### 23.2.9 初始化流程

```c
// 1. 早期分配内存池（mm_init 阶段）
void __init kfence_alloc_pool_and_metadata(void)
{
    __kfence_pool = memblock_alloc(KFENCE_POOL_SIZE, PAGE_SIZE);      // 内存池
    kfence_metadata_init = memblock_alloc(KFENCE_METADATA_SIZE, PAGE_SIZE);  // 元数据
}

// 2. 初始化内存池
static unsigned long kfence_init_pool(void)
{
    // a) 调用 arch_kfence_init_pool() 设置页属性
    // b) 将所有对象页标记为 __SetPageSlab（防止被伙伴系统回收）
    // c) 保护前 2 页（扩展保护页）
    // d) 初始化每个元数据对象，设置保护页
    // e) 随机化空闲链表顺序（避免确定性分配模式）
    for (i = CONFIG_KFENCE_NUM_OBJECTS; i > 0; i--) {
        rand = get_random_u32_below(i);
        swap(kfence_metadata[i-1].addr, kfence_metadata[rand].addr);
    }
    // f) 构建空闲链表
    // g) smp_store_release(&kfence_metadata, ...) 发布元数据指针
}

// 3. 启用 KFENCE
void __init kfence_init(void)
{
    stack_hash_seed = get_random_u32();
    if (!kfence_init_pool_early())
        return;
    kfence_init_enable();
}

// 4. 启用定时器
static void kfence_init_enable(void)
{
    // 初始化定时器（可延迟或普通）
    if (kfence_deferrable)
        INIT_DEFERRABLE_WORK(&kfence_timer, toggle_allocation_gate);
    else
        INIT_DELAYED_WORK(&kfence_timer, toggle_allocation_gate);

    // 注册 panic 时的金丝雀检查
    if (kfence_check_on_panic)
        atomic_notifier_chain_register(&panic_notifier_list, ...);

    WRITE_ONCE(kfence_enabled, true);
    queue_delayed_work(system_dfl_wq, &kfence_timer, 0);
}
```

#### 23.2.10 覆盖控制（Counting Bloom Filter）

KFENCE 使用 Counting Bloom Filter 跟踪**同类型分配**的覆盖情况，避免单一分配源占满整个池：

```c
// 参数
#define ALLOC_COVERED_HNUM    2           // 哈希函数数量
#define ALLOC_COVERED_ORDER   (ilog2(N) + 2)  // 过滤器大小
#define ALLOC_COVERED_SIZE    (1 << ALLOC_COVERED_ORDER)
static atomic_t alloc_covered[ALLOC_COVERED_SIZE];  // 计数器数组

// 添加/移除
static void alloc_covered_add(u32 hash, int val) {
    for (i = 0; i < ALLOC_COVERED_HNUM; i++) {
        atomic_add(val, &alloc_covered[hash & MASK]);
        hash = hash_32(hash, ALLOC_COVERED_ORDER);  // 第二个哈希
    }
}

// 检查
static bool alloc_covered_contains(u32 hash) {
    for (i = 0; i < ALLOC_COVERED_HNUM; i++) {
        if (!atomic_read(&alloc_covered[hash & MASK]))
            return false;
        hash = hash_32(hash, ALLOC_COVERED_ORDER);
    }
    return true;  // 所有计数器都 > 0 → 已覆盖
}

// 跳过阈值：池使用率 > 75% 时启用覆盖检查
static inline bool should_skip_covered(void) {
    return atomic_long_read(&counters[ALLOCATED]) > N * 75 / 100;
}
```

#### 23.2.11 sysfs 接口

```
/sys/kernel/debug/kfence/
├── stats          # 统计信息（只读）
├── objects        # 所有 KFENCE 对象详情（只读）

/sys/module/kfence/parameters/
├── sample_interval     # 采样间隔（ms），0=禁用，可动态调整
├── skip_covered_thresh # 覆盖跳过阈值（默认 75%）
├── burst               # 每次采样允许的额外分配数
├── deferrable          # 是否使用可延迟定时器
└── check_on_panic      # panic 时是否检查所有金丝雀
```

**统计计数器**：

| 计数器 | 含义 |
|--------|------|
| `currently allocated` | 当前已分配的对象数 |
| `total allocations` | 历史总分配数 |
| `total frees` | 历史总释放数 |
| `zombie allocations` | 缓存销毁时的僵尸分配数 |
| `total bugs` | 检测到的总错误数 |
| `skipped allocations (incompatible)` | 因不兼容跳过（大小、zone 等） |
| `skipped allocations (capacity)` | 因池满跳过 |
| `skipped allocations (covered)` | 因覆盖策略跳过 |

### 23.3 KMSAN — 内核未初始化内存检测

文件：`mm/kmsan/` 子目录，共 8 个源文件

| 文件 | 行数 | 功能 |
|------|------|------|
| `core.c` | ~200 | KMSAN 运行时核心：poison/unpoison/chain origin/memmove 元数据 |
| `shadow.c` | ~200 | 影子内存管理：地址到元数据的映射，页面元数据操作 |
| `hooks.c` | ~442 | 内核子系统钩子：slab 分配/释放、page alloc、DMA、URB、ioremap、copy_to_user |
| `instrumentation.c` | ~300 | 编译器插桩接口：`__msan_*` 函数，处理 LLVM 生成的元数据访问 |
| `init.c` | ~300 | 初始化：shadow 内存分配，memblock 元数据管理 |
| `report.c` | ~200 | 错误报告生成：origin 链解析、栈跟踪打印 |
| `kmsan.h` | ~190 | 内部头文件：上下文结构体、运行时守卫、内部函数声明 |
| `kmsan_test.c` | ~500 | KMSAN 单元测试 |

KMSAN（Kernel Memory Sanitizer）检测内核中**未初始化内存的使用**，通过 Clang 编译器的 `-fsanitize=kernel-memory` 插桩实现。

#### 23.3.1 影子内存模型

KMSAN 为每个物理字节维护两个元数据字节：

```
每个物理字节 → 两个影子字节：
  ┌──────────────┐  ┌──────────────┐
  │ shadow 影子  │  │ origin 影子  │
  │ 跟踪是否初始化 │  │ 记录未初始化   │
  │              │  │ 的来源（栈 ID）│
  │ 0 = 已初始化  │  │ 4 字节/每 4  │
  │ -1 = 未初始化 │  │ 字节物理内存   │
  └──────────────┘  └──────────────┘

映射比例：
  shadow:  1 字节影子 / 1 字节物理内存  (1:1)
  origin:  4 字节影子 / 4 字节物理内存  (1:4, 对齐到 4)
```

**影子值编码**：

```c
// mm/kmsan/kmsan.h
#define KMSAN_POISON_NOCHECK 0x0   // 不对齐毒化进行检查
#define KMSAN_POISON_CHECK   0x1   // 毒化时检查
#define KMSAN_POISON_FREE    0x2   // 标记为释放（UAF 检测标志）
```

#### 23.3.2 内核上下文结构

```c
// mm/kmsan/kmsan.h 中定义的 per-task/per-CPU 上下文
struct kmsan_ctx {
    bool kmsan_in_runtime;          // 是否在 KMSAN 运行时中（防止递归）
    int depth;                      // 禁用深度（kmsan_disable_current 嵌套计数）
    struct kmsan_ctx_state cstate;  // 编译器状态（参数/返回值影子 TLS）
};

// 每个 task 和每个 CPU 各有一个上下文
DECLARE_PER_CPU(struct kmsan_ctx, kmsan_percpu_ctx);
// task_struct 中：current->kmsan_ctx

// 获取当前上下文
static __always_inline struct kmsan_ctx *kmsan_get_context(void)
{
    return in_task() ? &current->kmsan_ctx : raw_cpu_ptr(&kmsan_percpu_ctx);
}
```

**运行时守卫**：

```c
// 防止递归进入 KMSAN 运行时
static __always_inline bool kmsan_in_runtime(void)
{
    if ((hardirq_count() >> HARDIRQ_SHIFT) > 1)  // 嵌套硬中断
        return true;
    if (in_nmi())                                  // NMI 上下文
        return true;
    return kmsan_get_context()->kmsan_in_runtime;  // 已进入运行时
}

// 进入/离开运行时
static __always_inline void kmsan_enter_runtime(void)
{
    kmsan_get_context()->kmsan_in_runtime = true;
}
static __always_inline void kmsan_leave_runtime(void)
{
    kmsan_get_context()->kmsan_in_runtime = false;
}
```

#### 23.3.3 内存布局与地址映射

**物理页面元数据**：

```c
// 每个物理页面有两个关联的元数据页面
// 通过 page 结构体中的两个字段存储
struct page {
    ...
    struct page *kmsan_shadow;   // 影子内存页面
    struct page *kmsan_origin;   // origin 内存页面
    ...
};

// 获取元数据指针
static void *shadow_ptr_for(struct page *page) {
    return page_address(page->kmsan_shadow);
}
static void *origin_ptr_for(struct page *page) {
    return page_address(page->kmsan_origin);
}
```

**vmalloc/modules 元数据映射**：

```c
// vmalloc 和模块区域使用保留的虚拟地址空间
static unsigned long vmalloc_meta(void *addr, bool is_origin)
{
    if (kmsan_internal_is_vmalloc_addr(addr)) {
        off = addr - VMALLOC_START;
        return off + (is_origin ? KMSAN_VMALLOC_ORIGIN_START :
                                  KMSAN_VMALLOC_SHADOW_START);
    }
    if (kmsan_internal_is_module_addr(addr)) {
        off = addr - MODULES_VADDR;
        return off + (is_origin ? KMSAN_MODULES_ORIGIN_START :
                                  KMSAN_MODULES_SHADOW_START);
    }
    return 0;
}
```

**元数据查找函数**：

```c
// shadow.c:107 — 获取任意地址的 shadow/origin 指针
void *kmsan_get_metadata(void *address, bool is_origin)
{
    u64 addr = (u64)address;
    struct page *page;

    if (is_origin)
        addr = ALIGN_DOWN(addr, KMSAN_ORIGIN_SIZE);  // origin 4 字节对齐

    // 1. vmalloc 或模块区域
    if (kmsan_internal_is_vmalloc_addr(address) ||
        kmsan_internal_is_module_addr(address))
        return (void *)vmalloc_meta(address, is_origin);

    // 2. 架构特定元数据（如线性映射区域）
    ret = arch_kmsan_get_meta_or_null(address, is_origin);
    if (ret)
        return ret;

    // 3. 常规物理页面
    page = virt_to_page_or_null(address);
    if (!page || !page_has_metadata(page))
        return NULL;

    return (is_origin ? origin_ptr_for(page) : shadow_ptr_for(page)) + off;
}
```

#### 23.3.4 编译器插桩接口

Clang 编译器在编译时插入 `__msan_*` 函数调用，KMSAN 通过 `instrumentation.c` 实现这些接口：

```c
// 获取元数据指针（用于内存访问检测）
// 编译器的每个内存访问前插入，获取 shadow/origin 地址
struct shadow_origin_ptr {
    void *shadow, *origin;
};

// 变长访问
struct shadow_origin_ptr __msan_metadata_ptr_for_load_n(void *addr, uintptr_t size);
struct shadow_origin_ptr __msan_metadata_ptr_for_store_n(void *addr, uintptr_t size);

// 定长访问（1/2/4/8 字节）
DECLARE_METADATA_PTR_GETTER(1);
DECLARE_METADATA_PTR_GETTER(2);
DECLARE_METADATA_PTR_GETTER(4);
DECLARE_METADATA_PTR_GETTER(8);

// 内联汇编存储处理
void __msan_instrument_asm_store(void *addr, uintptr_t size);
// 保守地 unpoison 汇编输出，防止误报

// LLVM 内存操作内联函数替换
void *__msan_memcpy(void *dst, const void *src, uintptr_t n);
void *__msan_memmove(void *dst, const void *src, uintptr_t n);
void *__msan_memset(void *s, int c, uintptr_t n);
// 替换 llvm.memcpy/memmove/memset 内联函数
// 实际执行未插桩的 __memcpy/__memmove，然后复制元数据
```

**编译器插桩工作流程**：

```
原始代码:                    int x = *p;  // 从内存加载

编译器插桩后:
  shadow = __msan_metadata_ptr_for_load_4(p);  // 获取 shadow 地址
  if (*shadow != 0) {                          // 检查是否未初始化
    origin = *(u32 *)__msan_metadata_ptr_for_load_4(p)->origin;
    __msan_warning(origin, p, 4);             // 报告未初始化使用
  }
  x = *p;                                      // 实际加载
```

#### 23.3.5 核心运行时函数

**毒化（Poison）**：标记内存为未初始化

```c
// core.c:41
void kmsan_internal_poison_memory(void *address, size_t size, gfp_t flags,
                                  unsigned int poison_flags)
{
    u32 extra_bits = kmsan_extra_bits(/*depth*/ 0, poison_flags & KMSAN_POISON_FREE);
    bool checked = poison_flags & KMSAN_POISON_CHECK;
    depot_stack_handle_t handle;

    // 保存当前调用栈（存入 stack depot）
    handle = kmsan_save_stack_with_flags(flags, extra_bits);

    // 设置 shadow = -1, origin = handle
    kmsan_internal_set_shadow_origin(address, size, -1, handle, checked);
}
```

**反毒化（Unpoison）**：标记内存为已初始化

```c
// core.c:52
void kmsan_internal_unpoison_memory(void *address, size_t size, bool checked)
{
    // 设置 shadow = 0, origin = 0
    kmsan_internal_set_shadow_origin(address, size, 0, 0, checked);
}
```

**元数据移动（memmove）**：复制内存时同步复制元数据

```c
// core.c:68
void kmsan_internal_memmove_metadata(void *dst, void *src, size_t n)
{
    // 1. 检查 dst 和 src 的元数据是否可访问
    shadow_dst = kmsan_get_metadata(dst, KMSAN_META_SHADOW);
    shadow_src = kmsan_get_metadata(src, KMSAN_META_SHADOW);
    if (!shadow_src) {
        // src 无元数据（如硬件内存）→ dst 标记为已初始化
        kmsan_internal_unpoison_memory(dst, n, false);
        return;
    }

    // 2. 逐字节复制 shadow，处理 origin 链
    for (i = 0; i < n; i++) {
        if (!shadow_src[iter]) {
            shadow_dst[iter] = 0;               // 已初始化 → 清空
            continue;
        }
        shadow_dst[iter] = shadow_src[iter];    // 复制 shadow
        // origin 链连接：记录"从 src 复制"这一事件
        origin_dst[oiter_dst] = kmsan_internal_chain_origin(old_origin);
    }
}
```

**origin 链**：追踪未初始化数据的传播路径

```c
// core.c:140
depot_stack_handle_t kmsan_internal_chain_origin(depot_stack_handle_t id)
{
    // 停止条件：深度达到 KMSAN_MAX_ORIGIN_DEPTH (7)
    if (depth == KMSAN_MAX_ORIGIN_DEPTH)
        return id;

    // 构建链：entries[0] = 魔术值 KMSAN_CHAIN_MAGIC_ORIGIN
    //          entries[1] = 当前操作栈
    //          entries[2] = 上一个 origin
    entries[0] = KMSAN_CHAIN_MAGIC_ORIGIN;
    entries[1] = kmsan_save_stack_with_flags(__GFP_HIGH, 0);  // 当前操作
    entries[2] = id;                            // 原始 origin
    handle = stack_depot_save(entries, 3, __GFP_HIGH);
    return stack_depot_set_extra_bits(handle, extra_bits);
}
```

**origin 链结构**：

```
origin 链（深度最多 7 层）：

  origin = stack_depot_handle
    │
    ├─ entries[0] = KMSAN_CHAIN_MAGIC_ORIGIN  (0xabcd0200)
    ├─ entries[1] = memcpy 操作栈              ← 最近的传播事件
    ├─ entries[2] = 上一个 origin
                      │
                      ├─ entries[0] = KMSAN_CHAIN_MAGIC_ORIGIN
                      ├─ entries[1] = 更早的 memcpy 操作栈
                      ├─ entries[2] = 原始 origin
                                      │
                                      ├─ entries[0] = 分配栈跟踪  ← 创建未初始化值的地方
                                      ├─ entries[1..N] = 栈帧
```

#### 23.3.6 核心钩子函数

**Slab 分配/释放钩子**：

```c
// hooks.c:37
void kmsan_slab_alloc(struct kmem_cache *s, void *object, gfp_t flags)
{
    if (s->ctor || (s->flags & SLAB_TYPESAFE_BY_RCU))
        return;  // 有构造函数或 RCU 缓存 → 保持原有状态

    if (flags & __GFP_ZERO)
        kmsan_internal_unpoison_memory(object, s->object_size, ...);  // 已清零 → 已初始化
    else
        kmsan_internal_poison_memory(object, s->object_size, ...);    // 未初始化 → 毒化
}

// hooks.c:67
void kmsan_slab_free(struct kmem_cache *s, void *object)
{
    if (unlikely(s->flags & SLAB_TYPESAFE_BY_RCU))
        return;     // RCU 缓存：合法地在释放后使用
    if (s->ctor)
        return;     // 有构造函数：保持状态

    // 毒化 + 标记 UAF
    kmsan_internal_poison_memory(object, s->object_size, ...,
                                 KMSAN_POISON_CHECK | KMSAN_POISON_FREE);
}
```

**页面分配钩子**：

```c
// shadow.c:180
void kmsan_alloc_page(struct page *page, unsigned int order, gfp_t flags)
{
    bool initialized = (flags & __GFP_ZERO) || !kmsan_enabled;

    if (initialized) {
        // 清零页面 → shadow 全 0, origin 全 0
        __memset(page_address(shadow), 0, PAGE_SIZE * pages);
        __memset(page_address(origin), 0, PAGE_SIZE * pages);
        return;
    }

    // 未初始化 → shadow 全 -1, origin 保存分配栈
    __memset(page_address(shadow), -1, PAGE_SIZE * pages);
    handle = kmsan_save_stack_with_flags(flags, 0);
    // 填充 origin 页面
    for (i = 0; i < (PAGE_SIZE * pages) / sizeof(u32); i++)
        ((u32 *)page_address(origin))[i] = handle;
}
```

**copy_to_user 钩子**：

```c
// hooks.c:267
void kmsan_copy_to_user(void __user *to, const void *from, size_t to_copy, size_t left)
{
    if (!to_copy || to_copy <= left)
        return;  // 未复制任何数据

    // 检查从内核拷贝到用户空间的数据是否包含未初始化值
    // 如果包含 → 报告 kernel-infoleak
    kmsan_internal_check_memory((void *)from, to_copy - left, to, REASON_COPY_TO_USER);
}
```

**DMA 钩子**：

```c
// hooks.c:344
void kmsan_handle_dma(phys_addr_t phys, size_t size, enum dma_data_direction dir)
{
    switch (dir) {
    case DMA_BIDIRECTIONAL:
        kmsan_internal_check_memory(addr, size, ...);    // 检查源数据
        kmsan_internal_unpoison_memory(addr, size, ...);  // 设备可能写入
        break;
    case DMA_TO_DEVICE:
        kmsan_internal_check_memory(addr, size, ...);    // 检查发送给设备的数据
        break;
    case DMA_FROM_DEVICE:
        kmsan_internal_unpoison_memory(addr, size, ...);  // 设备写入 → 已初始化
        break;
    case DMA_NONE:
        break;
    }
}
```

**USB URB 钩子**：

```c
// hooks.c:327
void kmsan_handle_urb(const struct urb *urb, bool is_out)
{
    if (is_out)
        // 发送给 USB 设备 → 检查数据是否已初始化
        kmsan_internal_check_memory(urb->transfer_buffer, ...);
    else
        // 从 USB 设备接收 → 标记为已初始化
        kmsan_internal_unpoison_memory(urb->transfer_buffer, ...);
}
```

**ioremap 钩子**：

```c
// hooks.c:195
int kmsan_ioremap_page_range(unsigned long start, unsigned long end,
                             phys_addr_t phys_addr, pgprot_t prot,
                             unsigned int page_shift)
{
    // 为 ioremap 区域分配 shadow/origin 页面
    for (i = 0; i < nr; i++) {
        shadow = alloc_pages(gfp_mask, 1);   // 分配 2 页
        origin = alloc_pages(gfp_mask, 1);
        __vmap_pages_range_noflush(vmalloc_shadow(start + off), ..., &shadow, ...);
        __vmap_pages_range_noflush(vmalloc_origin(start + off), ..., &origin, ...);
    }
}
```

#### 23.3.7 初始化流程

```c
// init.c
// 1. 记录需要 shadow 内存的区域
void __init kmsan_init_shadow(void)
{
    // 为所有预留内存创建 shadow/origin
    for_each_reserved_mem_range(loop, &p_start, &p_end)
        kmsan_record_future_shadow_range(phys_to_virt(p_start), phys_to_virt(p_end));
    // 为 .data 段创建 shadow
    kmsan_record_future_shadow_range(_sdata, _edata);
    // 为所有 NODE_DATA 创建 shadow
    for_each_online_node(nid)
        kmsan_record_future_shadow_range(NODE_DATA(nid), NODE_DATA(nid) + nd_size);
    // 分配 shadow 页面
    for (int i = 0; i < future_index; i++)
        kmsan_init_alloc_meta_for_range(start, end);
}

// 2. memblock 释放页面时的元数据分配
// 策略：每 3 批页面，用 2 批作为 shadow/origin，1 批作为实际内存
bool kmsan_memblock_free_pages(struct page *page, unsigned int order)
{
    if (!held_back[order].shadow) {
        held_back[order].shadow = page;  // 第 1 次：保留作为 shadow
        return false;
    }
    if (!held_back[order].origin) {
        held_back[order].origin = page;  // 第 2 次：保留作为 origin
        return false;
    }
    // 第 3 次：为实际页面关联 shadow/origin
    kmsan_setup_meta(page, held_back[order].shadow, held_back[order].origin, order);
    held_back[order].shadow = NULL;
    held_back[order].origin = NULL;
    return true;  // 允许实际页面被释放到伙伴系统
}
```

#### 23.3.8 错误报告

```c
// report.c
void kmsan_report(depot_stack_handle_t origin, void *address, int size,
                  int off_first, int off_last, const void __user *user_addr,
                  enum kmsan_bug_reason reason)
{
    is_uaf = kmsan_uaf_from_eb(stack_depot_get_extra_bits(origin));

    switch (reason) {
    case REASON_ANY:
        bug_type = is_uaf ? "use-after-free" : "uninit-value";
        break;
    case REASON_COPY_TO_USER:
        bug_type = is_uaf ? "kernel-infoleak-after-free" : "kernel-infoleak";
        break;
    case REASON_SUBMIT_URB:
        bug_type = is_uaf ? "kernel-usb-infoleak-after-free" : "kernel-usb-infoleak";
        break;
    }

    pr_err("BUG: KMSAN: %s\n", bug_type);
    stack_trace_print(...);            // 打印当前访问栈
    kmsan_print_origin(origin);        // 打印 origin 链
    if (size)
        pr_err("Bytes %d-%d of %d are uninitialized\n", off_first, off_last, size);
}

// origin 链解析
void kmsan_print_origin(depot_stack_handle_t origin)
{
    while (true) {
        nr_entries = stack_depot_fetch(origin, &entries);
        magic = entries[0];

        if (magic == KMSAN_ALLOCA_MAGIC_ORIGIN) {
            // 栈变量：打印函数名和局部变量名
            pr_err("Local variable %s created at:\n", pretty_descr(descr));
            break;
        }
        if (magic == KMSAN_CHAIN_MAGIC_ORIGIN) {
            // 链节点：打印传播操作，继续追踪
            head = entries[1];  // 当前操作栈
            origin = entries[2]; // 前一个 origin
            pr_err("Uninit was stored to memory at:\n");
            stack_trace_print(head, ...);
            continue;  // 继续追踪
        }
        // 叶子节点：打印原始分配栈
        pr_err("Uninit was created at:\n");
        stack_trace_print(entries, ...);
        break;
    }
}
```

#### 23.3.9 检测场景

| 场景 | 检测机制 | 报告类型 |
|------|----------|----------|
| 未初始化变量使用 | 编译器插桩检查 shadow | `uninit-value` |
| Use-after-free | 释放后毒化 + POISON_FREE 标志 | `use-after-free` |
| 内核信息泄露 | copy_to_user 时检查 shadow | `kernel-infoleak` |
| USB 信息泄露 | URB 提交时检查 shadow | `kernel-usb-infoleak` |
| DMA 数据泄露 | DMA_TO_DEVICE 时检查 | 同 `REASON_ANY` |
| 栈变量未初始化 | alloca 时毒化栈帧 | `uninit-value` + 局部变量名 |

### 23.4 page_owner — 页面分配者跟踪

文件：`mm/page_owner.c`（605 行）

`page_owner` 记录每个页面的分配调用栈，用于定位内存泄漏和页面分配来源。

#### 23.4.1 核心机制

```c
// mm/page_owner.c:605
void __dump_page_owner(const struct page *page)
{
    // 1. 读取页面 owner 信息
    // 2. 打印分配栈、释放栈、分配时间、迁移类型
    // 3. 读取栈跟踪（depot stack handles）
    stack_depot_snprint(handle, buf, SIZE, 0);
    pr_alert("page_owner: %s\n", buf);
}
```

**工作原理**：

```
页面分配时：
  set_page_owner(page, order, gfp_mask)
    └─ 分配页扩展结构（page_ext）
    └─ 记录分配栈（stack_depot_save）
    └─ 记录分配标志和时间

页面释放时：
  __set_page_owner_migrate_reason(page, reason)  // 迁移原因
  split_page_owner(page, order)                    // 大页拆分
  copy_page_owner(dst, src)                        // 拷贝 owner 信息

调试输出：
  cat /sys/kernel/debug/page_owner
  └─ 按分配顺序输出所有未释放页面的 owner 信息
  └─ 用于分析内存泄漏：哪些页面未释放，谁分配的
```

### 23.5 page_poison — 页面毒化

文件：`mm/page_poison.c`

页面释放后填充特定值，分配时检查，检测 use-after-free 和未初始化使用。

```c
// 释放后填充值
#define PAGE_POISON 0xAA  // 释放后页面的填充字节

// 分配时检查
if (page_poisoning_enabled())
    check_poison_mem(page_address(page), PAGE_SIZE << order);
```

**工作流程**：
```
free_pages() → fill_page_poison(page)     // 填充 0xAA
alloc_pages() → check_page_poison(page)   // 验证 0xAA 未被修改
                                            // 如果被修改 → 检测到 UAF
```

### 23.6 其他调试与安全工具

| 文件 | 行数 | 功能 | 原理 |
|------|------|------|------|
| `mm/debug.c` | 245 | 通用调试辅助 | `dump_page()` 打印页面信息 |
| `mm/debug_page_alloc.c` | 50 | 页面分配调试 | 分配时填充毒化模式 |
| `mm/debug_page_ref.c` | 60 | 页面引用计数调试 | 跟踪 get_page/put_page 调用 |
| `mm/debug_vm_pgtable.c` | 450 | 页表调试 | 验证页表操作的正确性 |
| `mm/page_table_check.c` | 350 | 页表一致性检查 | 跟踪 PTE 映射的 PFN，检测重复映射 |
| `mm/page_idle.c` | 120 | 空闲页面跟踪 | 通过 /sys/kernel/mm/page_idle/ 接口 |
| `mm/page_reporting.c` | 300 | 空闲页面报告 | 将空闲页面报告给虚拟化层（virtio-balloon） |
| `mm/ptdump.c` | 200 | 页表转储 | /sys/kernel/debug/kernel_page_tables |
| `mm/rodata_test.c` | 50 | 只读数据测试 | 验证 .rodata 段是否确实只读 |
| `mm/shrinker_debug.c` | 300 | shrinker 调试 | /sys/kernel/debug/shrinker/ 接口 |

#### 23.6.1 page_table_check — 页表一致性检查

```c
// mm/page_table_check.c 核心功能
// 1. 跟踪每个 PFN 被映射到多少个 PTE
// 2. 检查非法映射（如同一页面映射为可写 + 可执行）
// 3. 检测页面映射泄漏

// 插入 PTE 时：
page_table_check_pte_set(mm, paddr, pte)
  └─ page_table_check_set(pfn, PAGE_SIZE, PTE)
       └─ 递增 PFN 映射计数

// 清除 PTE 时：
page_table_check_pte_clear(mm, paddr, pte)
  └─ page_table_check_clear(pfn, PAGE_SIZE, PTE)
       └─ 递减 PFN 映射计数
```

#### 23.6.2 page_idle — 空闲页面跟踪

通过 `/sys/kernel/mm/page_idle/bitmap_N` 接口，用户空间可查询和设置页面的"空闲"和"年轻"位：

```
page_idle_bitmap_walk():
  └─ 读取页面的 Accessed 位（硬件）
  └─ 通过 /sys/kernel/mm/page_idle/ 暴露
  └─ 用于 NUMA 平衡、页面迁移决策
```

#### 23.6.3 page_reporting — 空闲页面报告

```
page_reporting 工作流程：
  └─ 伙伴系统释放页面时（__free_one_page）
       └─ 如果页面足够大（≥ PAGE_REPORTING_MIN_ORDER）
            └─ 将页面添加到报告链表
            └─ 通知虚拟化层（virtio-balloon）
                 └─ 虚拟机管理程序可以回收这些页面
```

### 23.7 调试工具对比

| 工具 | 检测类型 | 运行开销 | 内存开销 | 部署场景 |
|------|----------|----------|----------|----------|
| KASAN Generic | 越界 + UAF + 栈 + 全局 | ~2x | ~2x | 开发测试 |
| KASAN SW_TAGS | 越界 + UAF | ~30% | ~20% | 开发测试 |
| KASAN HW_TAGS | 越界 + UAF | ~1% | ~20% | 生产环境 |
| KFENCE | 越界 + UAF（采样） | ~1% | <1MB | 生产环境 |
| KMSAN | 未初始化内存 | ~3x | ~3x | 开发测试 |
| page_owner | 内存泄漏 | 小 | 可配置 | 生产诊断 |
| page_poison | UAF | <1% | 0 | 生产环境 |
| page_table_check | 映射错误 | ~5% | 按页面 | 生产环境 |

---

## Part VI: 附录

## 24. 总结

### 24.1 架构层次

```
用户空间 (malloc/free/mmap/munmap)
        │
        ▼
┌──────────────────────────────────────┐
│  系统调用层                            │
│  (mmap, mremap, mprotect, mlock,     │
│   madvise, mseal, mbind, ...)         │
├──────────────────────────────────────┤
│  VMA 管理 (mmap.c, vma.c, vma_exec.c) │
│  └─ 红黑树查找/合并/分裂/扩展/收缩     │
├──────────────────────────────────────┤
│  缺页处理 (memory.c)                  │
│  ├─ do_anonymous_page / do_fault      │
│  ├─ do_swap_page / do_numa_page       │
│  └─ wp_page_copy (COW)               │
├──────────────────────────────────────┤
│  Page Cache (filemap.c, readahead.c)  │
│  └─ XArray 页面查找/预读/写回
├──────────────────────────────────────┤
│  SLUB (slub.c) / vmalloc (vmalloc.c) │
│  ├─ kmalloc/kfree (小对象快速分配)     │
│  └─ vmalloc/vfree (大块虚拟连续)      │
├──────────────────────────────────────┤
│  伙伴系统 (page_alloc.c)              │
│  ├─ 2^N 阶空闲链表 / 迁移类型         │
│  ├─ Per-CPU pageset (order-0 加速)    │
│  └─ 水位线控制 / 回收触发             │
├──────────────────────────────────────┤
│  MemBlock (memblock.c)               │
│  └─ 启动时物理内存分配器              │
└──────────────────────────────────────┘
```

### 24.2 关键平衡策略

1. **内存 vs 性能**：Per-CPU pageset、SLUB cpu_slab 等缓存机制减少锁竞争
2. **内存 vs I/O**：Page Cache 缓存文件数据，脏页写回平衡内存与磁盘
3. **回收 vs 分配**：kswapd 异步回收 vs 直接回收，水位线控制阈值
4. **压缩 vs 碎片**：kcompactd 后台压缩 vs 分配时直接压缩
5. **公平 vs 效率**：memcg 按 cgroup 限制，LRU 按活跃度排序

### 24.3 代码量统计

| 模块 | 行数 | 占比 |
|------|------|------|
| 核心分配器（slub + page_alloc） | 17,695 | 8.2% |
| 虚拟内存管理（memory + vma + vmalloc + mmap） | 17,789 | 8.3% |
| 页面回收与交换（vmscan + swap*） | 12,993 | 6.1% |
| 大页（hugetlb + huge_memory + khugepaged + ksm） | 19,210 | 9.0% |
| 文件缓存（filemap + writeback + readahead） | 8,784 | 4.1% |
| 控制组（memcontrol + memcontrol-v1 + vmpressure） | 8,022 | 3.7% |
| 调试工具（kasan + kfence + kmsan + debug*） | ~15,000 | 7.0% |
| DAMON（core + sysfs + vaddr + paddr） | ~10,000 | 4.7% |
| NUMA/策略（mempolicy + memory-tiers + numa*） | ~7,000 | 3.3% |
| 其他（migrate + compaction + mmap_lock + cma + zswap + 等） | ~50,000 | 23.3% |
| 头文件（include/linux/mm*.h + include/linux/*.h） | ~30,000 | 14.0% |
| **总计** | ~214,524 | 100% |

### 24.4 架构设计原则

- **分层抽象**：从伙伴系统到 SLUB/vmalloc 再到用户空间 VMA，层层封装
- **惰性分配**：页表、物理页面在真正访问时才分配（缺页处理）
- **批量操作**：Per-CPU pageset 批量填充、swap 簇预读、LRU 批量隔离
- **异步处理**：kswapd、kcompactd、khugepaged、DAMON 等内核线程异步执行
- **可配置性**：大量 Kconfig 选项和 sysctl 参数，适应不同场景
- **可观测性**：/proc/meminfo、/sys/devices/system/node/、memcg 统计、tracepoints

### 24.5 Linux 7.0 内存管理关键更新汇总

| 特性 | 模块 | 收益 |
|------|------|------|
| **kmalloc_obj API** | SLUB | 基于类型的分配，更精细的内存管理 |
| **Sheaves 缓存** | SLUB | 降低锁开销，提高 NUMA 扩展性 |
| **Large Folios 批量回收** | vmscan | 大页回收速度提升 50%~75% |
| **Gigantic Folio 分配加速** | page_alloc | 1GB 巨页分配 8.4x 加速 |
| **Swap 代码清理** | swap | Redis 基准测试性能提升 20% |
| **Zram 写回** | zram | 更好的内存压力管理，能效提升 |
| **PT_RECLAIM** | mm/memory | 更高效地回收页表内存 |
| **更智能的分配策略** | page_alloc | 减少碎片，提高分配成功率 |

### 24.6 启动内存管理总结

Linux 内核启动内存管理是 **从 memblock 到 Buddy + Slab 的平稳过渡**：

```
start_kernel
  │
  ├─ setup_arch()
  │    └─ arm64_memblock_init()    // memblock 启动分配器
  │    └─ paging_init()            // 建立内核页表映射
  │
  └─ mm_init()
       └─ kmem_cache_init()        // SLUB 初始化
       └─ mem_init()               // memblock → 伙伴系统移交
       └─ page_alloc_init()        // 伙伴系统初始化
```

三个阶段：
1. **memblock 阶段**：极简分配器，管理物理内存
2. **地址映射建立**：打通虚拟地址到物理内存的通道
3. **核心分配器就绪**：伙伴系统管理物理页，SLUB 管理小对象