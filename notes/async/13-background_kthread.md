# 后台内核线程 — 异步内存管理

## 1 概述

Linux 内核中有多个专用后台内核线程，负责异步执行内存管理相关任务。它们在内核初始化时创建，在后台持续运行，根据系统状态自动触发。

| 线程 | 功能 | 源文件 |
|--|--|--|
| kswapd | 异步内存回收 | `mm/vmscan.c` |
| kcompactd | 异步内存压缩 | `mm/compaction.c` |
| flusher/writeback | 异步脏页回写 | `mm/page-writeback.c`, `fs/fs-writeback.c` |
| khugepaged | 透明大页合并 | `mm/khugepaged.c` |
| oom_reaper | OOM 后异步杀死进程 | `mm/oom_kill.c` |

## 2 各机制详解

### 2.1 kswapd — 内存回收线程

**功能**: 当内存水位低于 `WMARK_LOW` 时，kswapd 被唤醒，异步回收页面直到水位恢复到 `WMARK_HIGH`。

**唤醒路径**:

```
__alloc_pages_nodemask()
  └─ 水位检查 (zone_watermark_fast())
       └─ 低于 WMARK_LOW
            └─ wakeup_kswapd(zone, gfp_mask, order, highest_zoneidx)
                 └─ wake_up_interruptible(&pgdat->kswapd_wait)
```

**执行流程**:

```
kswapd()
  │
  ├─ 1. 等待 pgdat->kswapd_wait 被唤醒
  │
  ├─ 2. balance_pgdat(pgdat, order, highest_zoneidx)
  │    ├─ 扫描所有 zone
  │    ├─ 从 LRU 链表回收页面
  │    │    ├─ 匿名页 → swap
  │    │    └─ 文件页 → 回写/释放
  │    └─ 直到水位恢复到 WMARK_HIGH
  │
  └─ 3. 回到等待状态
```

**关键数据结构**:

```c
// 每个 NUMA 节点一个 pglist_data
struct pglist_data {
    struct task_struct *kswapd;          // kswapd 内核线程
    wait_queue_head_t kswapd_wait;       // 唤醒等待队列
    spinlock_t kswapd_lock;              // 保护 kswapd 状态
    ...
};
```

### 2.2 kcompactd — 内存压缩线程

**功能**: 当内存碎片化严重时，kcompactd 被唤醒，通过页面迁移和伙伴系统合并来减少碎片。

**唤醒路径**:

```
__alloc_pages_slowpath()
  └─ __alloc_pages_direct_compact()
       └─ 压缩失败但需要继续
            └─ wakeup_kcompactd(pgdat, order, highest_zoneidx)
                 └─ wake_up_interruptible(&pgdat->kcompactd_wait)
```

**执行流程**:

```
kcompactd()
  │
  ├─ 1. 等待 pgdat->kcompactd_wait 被唤醒
  │
  ├─ 2. kcompactd_do_work(pgdat)
  │    ├─ 扫描所有 zone
  │    ├─ compact_zone() 执行页面迁移
  │    │    ├─ 扫描页面并迁移
  │    │    └─ 释放后形成连续大块
  │    └─ 直到满足所需 order
  │
  └─ 3. 回到等待状态
```

**关键数据结构**:

```c
struct pglist_data {
    struct task_struct *kcompactd;          // kcompactd 内核线程
    wait_queue_head_t kcompactd_wait;       // 唤醒等待队列
    int kcompactd_max_order;                // 所需的最高 order
    int kcompactd_highest_zoneidx;           // 最高 zone 索引
    bool proactive_compact_trigger;          // 主动压缩触发标志
    ...
};
```

### 2.3 flusher / writeback — 脏页回写线程

**功能**: 当脏页数量超过阈值或脏页存在时间超过超时时间时，回写线程将脏页异步写入磁盘。

**触发条件**:

| 条件 | 阈值 | 触发方式 |
|--|--|--|
| 脏页比例超限 | `dirty_background_ratio` (默认 10%) | `wb_start_background_writeback()` |
| 脏页超时 | `dirty_expire_interval` (默认 30 秒) | `wb_wakeup_delayed()` |
| 周期性回写 | `dirty_writeback_interval` (默认 5 秒) | 定时器触发 |

**唤醒路径**:

```
balance_dirty_pages()
  └─ nr_dirty > gdtc->bg_thresh
       └─ wb_start_background_writeback(wb)
            └─ wb_wakeup(wb)
                 └─ wake_up_process(wb->bdi->wb.task)
```

**执行流程**:

```
writeback 线程 (每个 bdi 一个)
  │
  ├─ 1. 等待被唤醒
  │
  ├─ 2. wb_do_writeback(wb)
  │    ├─ wb_check_background_flush()
  │    │    └─ 回写所有脏页
  │    ├─ wb_check_old_data_flush()
  │    │    └─ 回写超时脏页
  │    └─ wb_check_start_all()
  │         └─ 回写指定 inode 的脏页
  │
  └─ 3. 回到等待状态
```

### 2.4 khugepaged — 透明大页合并线程

**功能**: 扫描内存，将符合条件的连续小页面合并为透明大页 (THP)。

**唤醒路径**: khugepaged 周期性扫描，或在 `madvise(MADV_HUGEPAGE)` 时触发。

**关键数据结构**:

```c
struct mm_slot {
    struct hlist_node hash;              // 哈希链表节点
    struct list_head mm_node;            // 全局链表节点
    struct mm_struct *mm;                // 目标进程地址空间
};
```

### 2.5 oom_reaper — OOM 后清理线程

**功能**: 当 OOM killer 选中一个进程后，oom_reaper 异步扫描该进程的地址空间，释放其内存页。

**唤醒路径**:

```
oom_kill_process()
  └─ wake_oom_reaper(tsk)
       └─ wake_up(&oom_reaper_wait)
```

**执行流程**:

```
oom_reaper()
  ├─ 等待 oom_reaper_wait 唤醒
  ├─ oom_reap_task_mm(tsk)
  │    └─ 遍历进程的 VMA，释放匿名页
  └─ 回到等待状态
```

## 3 综合调用栈

```
内存压力增加
  │
  ├─ kswapd 回收
  │    └─ wakeup_kswapd() → balance_pgdat() → shrink_node() → 页面回收
  │
  ├─ kcompactd 压缩
  │    └─ wakeup_kcompactd() → compact_zone() → 页面迁移
  │
  └─ writeback 回写
       └─ wb_start_background_writeback() → wb_do_writeback() → 脏页回写
```

## 4 与 Workqueue 的关系

这些后台内核线程是独立创建的 `kthread`，而不是通过 Workqueue 管理。其优势在于：

- **专用控制**: 每个线程有独立的等待队列和唤醒条件
- **确定性调度**: 可设置调度策略（如 kswapd 使用 `SCHED_FIFO`）
- **资源隔离**: 不会与其他 workqueue 工作竞争 worker 线程

## 5 关键 API

| 线程 | 创建位置 | 唤醒函数 | 停止函数 |
|--|--|--|--|
| kswapd | `pgdat_init_internals()` | `wakeup_kswapd()` | `kswapd_stop()` |
| kcompactd | `pgdat_init_internals()` | `wakeup_kcompactd()` | `kcompactd_stop()` |
| writeback | `bdi_register()` | `wb_wakeup()` | `bdi_unregister()` |
| khugepaged | `khugepaged_init()` | `wakeup_khugepaged()` | `khugepaged_stop()` |
| oom_reaper | `oom_init()` | `wake_oom_reaper()` | `oom_reaper_stop()` |