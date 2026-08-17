# 内存管理 — 控制与隔离 (Part IV)

> 本文档拆分自 [memory_management.md](memory_management.md) Part IV，涵盖Memory Cgroup、NUMA与内存策略、CMA连续内存分配器、内存热插拔

### Part IV: 控制与隔离

16. [Memory Cgroup](#16-memory-cgroup)
17. [NUMA 与内存策略](#17-numa-与内存策略)
18. [CMA 连续内存分配器](#18-cma-连续内存分配器)
19. [内存热插拔](#19-内存热插拔)

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