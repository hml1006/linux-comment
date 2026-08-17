# 内存管理 — 内存回收与交换 (Part II)

> 本文档拆分自 [memory_management.md](memory_management.md) Part II，涵盖页面回收、交换机制、压缩与页面迁移、Zswap与Zsmalloc

### Part II: 内存回收与交换

8. [页面回收（Reclaim）](#8-页面回收reclaim)
9. [交换（Swap）](#9-交换swap)
10. [压缩与页面迁移](#10-压缩与页面迁移)
11. [Zswap 与 Zsmalloc](#11-zswap-与-zsmalloc)

---

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
                list_move(&folio