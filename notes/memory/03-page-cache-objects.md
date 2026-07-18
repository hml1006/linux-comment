# 内存管理 — 内核对象与页缓存 (Part III)

> 本文档拆分自 [memory_management.md](memory_management.md) Part III，涵盖文件页缓存、写回机制、Mempool内存池、Per-CPU分配器

### Part III: 内核对象与页缓存

12. [文件页缓存（Page Cache）](#12-文件页缓存page-cache)
13. [写回机制（Writeback）](#13-写回机制writeback)
14. [Mempool 内存池](#14-mempool-内存池)
15. [Per-CPU 分配器](#15-per-cpu-分配器)

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