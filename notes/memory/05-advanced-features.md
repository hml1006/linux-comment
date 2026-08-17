# 内存管理 — 高级特性 (Part V)

> 本文档拆分自 [memory_management.md](memory_management.md) Part V，涵盖透明大页(THP)与KSM、OOM/内存故障处理、DAMON数据访问监控、调试与安全工具

### Part V: 高级特性

20. [透明大页（THP）与 KSM](#20-透明大页thp与-ksm)
21. [内存错误处理（OOM/故障/泄漏）](#21-内存错误处理oom故障泄漏)
22. [DAMON 数据访问监控](#22-damon-数据访问监控)
23. [调试与安全工具](#23-调试与安全工具)

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