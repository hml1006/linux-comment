# 块层 — I/O 调度与策略控制 (Part II)

> 本文档拆分自 [block_layer_analysis.md](block_layer_analysis.md) Part II，涵盖I/O调度器、I/O合并与分段、Flush/FUA屏障、QoS与资源控制、Cgroup集成

## Part II: I/O 调度与策略控制

## 6. I/O 调度器

### 6.1 elevator.c — 调度器框架（895 行）

文件：`block/elevator.c`

提供统一的 I/O 调度器接口：

- `elv_rqhash_add()` — 将请求添加到调度器哈希表，用于合并查找。
- `elv_rqhash_del()` — 从哈希表中移除请求。
- `elv_merge()` — 查找可合并的请求。
- `elevator_init()` / `elevator_exit()` — 调度器的初始化和卸载。
- 调度器切换：通过 sysfs 的 `/sys/block/<dev>/queue/scheduler` 实现。

### 6.2 BFQ 调度器（Budget Fair Queueing）

| 文件 | 行数 | 功能 |
|------|------|------|
| bfq-iosched.c | 7,682 | BFQ 主实现（块层最大文件） |
| bfq-wf2q.c | 1,701 | WF2Q+ 算法实现 |
| bfq-cgroup.c | 1,440 | BFQ 的 cgroup 分层调度支持 |
| bfq-iosched.h | 1,202 | BFQ 内部头文件 |

**总代码量**：约 12,025 行，是块层中最复杂的调度器。

BFQ 特点：
- 比例份额（proportional-share）I/O 调度
- 低延迟能力
- 通过 cgroup 支持完整的分层调度
- 基于 WF2Q+（Worst-case Fair Weighted Fair Queueing+）算法
- 适合桌面和交互式应用场景

### 6.3 MQ-Deadline 调度器（1,029 行）

文件：`block/mq-deadline.c`

为多队列设计的 Deadline 调度器，核心参数：
- **读超时**：`HZ/2`（500ms）
- **写超时**：`5*HZ`（5s）
- **写饥饿阈值**：`writes_starved=2`（最多允许读请求连续饿死写请求 2 次）
- **FIFO 批量**：`fifo_batch=16`（连续请求处理批数）

特点：简单、低开销，适合大多数服务器场景。

### 6.4 Kyber 调度器（1,033 行）

文件：`block/kyber-iosched.c`

面向延迟的调度器，将请求分为 4 个域：
- `KYBER_READ` — 读请求
- `KYBER_WRITE` — 写请求
- `KYBER_DISCARD` — 丢弃请求
- `KYBER_OTHER` — 其他请求

通过动态调整每个域的派发深度来控制延迟。

### 6.5 none 调度器 — 无调度器模式

"none" 不是真正的调度器实现，而是**不使用任何调度器**的模式。当选择 "none" 时，`q->elevator` 被设为 `NULL`，请求绕过调度器，直接从软件队列下发给硬件。

**适用场景**：
- 多队列设备（如 NVMe SSD），硬件已有足够的并行能力，软件调度反而增加延迟
- 设置了 `BLK_MQ_F_NO_SCHED_BY_DEFAULT` 标志的设备（如 virtio-blk）

**默认策略**（[elevator.c](file:///home/louis/code/linux/block/elevator.c)）：

```c
// 单队列设备默认 mq-deadline；多队列设备默认 none
void elevator_set_default(struct request_queue *q)
{
    if (q->tag_set->flags & BLK_MQ_F_NO_SCHED_BY_DEFAULT)
        return;  // 不设置任何调度器，保持 none

    // 仅单队列或共享 tags 时使用 mq-deadline
    if ((q->nr_hw_queues == 1 ||
         blk_mq_is_shared_tags(q->tag_set->flags))) {
        elevator_change(q, &ctx);  // 尝试 mq-deadline
    }
}
```

**切换流程**（[elevator.c](file:///home/louis/code/linux/block/elevator.c)）：

```
elevator_switch(q, ctx)
  │
  ├─ strncmp(ctx->name, "none", 4) == 0:
  │     new_e = NULL                         // 不查找 elevator_type
  │
  ├─ if (q->elevator): elevator_exit(q)       // 卸载旧调度器
  │
  ├─ if (new_e):
  │     blk_mq_init_sched(q, new_e)           // 有调度器：初始化
  │
  └─ else:  // "none" 路径
        ├─ blk_queue_flag_clear(QUEUE_FLAG_SQ_SCHED, q)  // 清除单队列调度标志
        ├─ q->elevator = NULL                              // 无 elevator
        └─ q->nr_requests = q->tag_set->queue_depth        // 请求数 = 硬件队列深度
```

**插入路径**（直接入队，无调度器参与）（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）：

```
blk_mq_insert_request(rq, flags)
  │
  ├─ blk_rq_is_passthrough(rq) → blk_mq_request_bypass_insert()  直通请求 → dispatch 队列
  ├─ req_op(rq) == REQ_OP_FLUSH  → blk_mq_request_bypass_insert()  flush 请求 → dispatch 队列头部
  │
  ├─ if (q->elevator):                          // 有调度器
  │     q->elevator->type->ops.insert_requests(hctx, &list, flags)
  │
  └─ else:                                      // "none" 调度器
        ├─ trace_block_rq_insert(rq)
        ├─ if (flags & BLK_MQ_INSERT_AT_HEAD):
        │     list_add(&rq->queuelist, &ctx->rq_lists[hctx->type])  // 加入 ctx 队列头部
        └─ else:
              list_add_tail(&rq->queuelist, &ctx->rq_lists[hctx->type])  // 加入 ctx 队列尾部
              blk_mq_hctx_mark_pending(hctx, ctx)  // 标记该 ctx 有待派发请求
```

**派发路径**（无调度器时直接从 ctx 队列取请求）（[blk-mq-sched.c](file:///home/louis/code/linux/block/blk-mq-sched.c)）：

```
__blk_mq_sched_dispatch_requests(hctx)
  │
  ├─ 优先处理 hctx->dispatch 列表中的残留请求
  │
  ├─ if (hctx->queue->elevator):
  │     └─ blk_mq_do_dispatch_sched(hctx)   // 有调度器：从调度器取请求
  │
  └─ else:  // "none" 路径
        └─ blk_mq_do_dispatch_ctx(hctx)      // 直接从 ctx 软件队列取请求
              │
              └─ do {
                    ├─ blk_mq_get_dispatch_budget(q)           // 获取派发预算
                    ├─ rq = blk_mq_dequeue_from_ctx(hctx, ctx) // 从 ctx 出队一个请求
                    ├─ blk_mq_set_rq_budget_token(rq, token)   // 设置预算 token
                    ├─ list_add(&rq->queuelist, &rq_list)      // 加入派发列表
                    ├─ ctx = blk_mq_next_ctx(hctx, rq->mq_ctx) // Round-Robin 轮转 ctx
                    └─ blk_mq_dispatch_rq_list(hctx, &rq_list) // 派发给驱动
                       → nvme_queue_rq() → 硬件 SQ
                 } while (...)
```

**关键区别对比**：

| 特性 | 有调度器（如 mq-deadline） | none 调度器 |
|------|--------------------------|-------------|
| `q->elevator` | 指向 elevator_queue | `NULL` |
| 请求入队 | 进入调度器内部队列（红黑树/FIFO） | 直接进入 ctx 软件队列 |
| 请求派发 | 调度器决定顺序（排序/合并/优先级） | Round-Robin FIFO，无排序 |
| 合并支持 | 调度器提供 bio_merge / request_merge | 无合并（只能靠 bio 层合并） |
| sysfs 显示 | `[mq-deadline] kyber bfq none` | `[none] mq-deadline kyber bfq` |
| 开销 | 有调度逻辑开销 | 几乎零开销 |
| 适用设备 | 单队列 HDD/SATA SSD | 多队列 NVMe SSD |

**小结**：none 调度器的核心思想是 "硬件已经足够快，不需要软件调度"。对于 NVMe 这种多队列设备，请求直接下发到硬件队列，由硬件内部的命令调度器处理，避免了软件调度的 CPU 开销和延迟。

### 6.6 Plug 机制 — 批量提交优化

#### 6.6.1 概述

Plug 机制是块层的一个重要性能优化——将多个 I/O 请求先暂存到当前进程的 `current->plug` 中，延迟到 `blk_finish_plug()` 时再**批量下发**，从而：

1. **合并相邻请求**：plug 列表中的请求可以被后续 bio 合并，减少实际下发到设备的请求数
2. **批量派发**：一次 unlock 将所有请求派发给驱动，减少锁开销
3. **批量分配 tag**：利用 `cached_rqs` 预分配请求，减少 tag 分配开销
4. **利用 `queue_rqs`**：NVMe 驱动支持 `queue_rqs`，可一次下发多个命令到硬件 SQ

**核心数据结构**（[blkdev.h](file:///home/louis/code/linux/include/linux/blkdev.h)）：

```c
struct blk_plug {
    struct rq_list mq_list;         // 暂存的请求链表（核心）
    struct rq_list cached_rqs;      // 预分配的缓存请求（复用 tag）
    u64 cur_ktime;                  // 插桩时间戳
    unsigned short nr_ios;          // 剩余可缓存的请求数（用于 tag 批量分配）
    unsigned short rq_count;        // 当前 mq_list 中的请求数
    bool multiple_queues;           // 是否包含来自多个 request_queue 的请求
    bool has_elevator;              // 是否包含调度器分配的请求（影响派发路径）
    struct list_head cb_list;       // 回调链（md/dm 等堆叠设备使用）
};
```

**plug 存储在 `task_struct->plug`** 中，每个进程只有一个 plug，保证了自然的作用域和生命周期。

#### 6.6.2 触发刷新的阈值

| 常量 | 值 | 含义 |
|------|----|------|
| `BLK_MAX_REQUEST_COUNT` | 32 | 单队列最多堆积 32 个请求后自动刷新 |
| `BLK_PLUG_FLUSH_SIZE` | 128KB | 单个请求超过 128KB 时自动刷新 |

当 `rq_count >= 32`（多队列时 64）或上一个请求的字节数 >= 128KB 时，plug 列表会被自动刷新。

#### 6.6.3 生命周期与自动刷新

```
blk_start_plug(&plug)          // 开始插桩，current->plug = &plug
    │
    ├─ submit_bio() → ... → blk_add_rq_to_plug(plug, rq)
    │     │
    │     ├─ rq_count >= 32 || last_rq_bytes >= 128KB → 自动刷新
    │     └─ 否则：rq 加入 mq_list 尾部
    │
    ├─ 更多 submit_bio() ...
    │
    ├─ 【如果进程进入睡眠】
    │     schedule() → blk_flush_plug(tsk->plug, true)
    │     io_schedule() → blk_flush_plug(current->plug, true)
    │     （防止死锁：回收内存时需要等待 plug 中的请求完成）
    │
    └─ blk_finish_plug(&plug)   // 结束插桩，强制刷新所有剩余请求
          current->plug = NULL
```

#### 6.6.4 关键函数分析

**blk_start_plug_nr_ios**（[blk-core.c](file:///home/louis/code/linux/block/blk-core.c)）：
```c
void blk_start_plug_nr_ios(struct blk_plug *plug, unsigned short nr_ios)
{
    struct task_struct *tsk = current;

    if (tsk->plug)          // 嵌套插桩：直接返回，不覆盖外层
        return;

    plug->cur_ktime = 0;
    rq_list_init(&plug->mq_list);
    rq_list_init(&plug->cached_rqs);
    plug->nr_ios = min_t(unsigned short, nr_ios, BLK_MAX_REQUEST_COUNT);
    plug->rq_count = 0;
    plug->multiple_queues = false;
    plug->has_elevator = false;
    INIT_LIST_HEAD(&plug->cb_list);

    tsk->plug = plug;       // 关键：将 plug 挂到当前进程
}
```

**blk_add_rq_to_plug**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）：
```c
static void blk_add_rq_to_plug(struct blk_plug *plug, struct request *rq)
{
    struct request *last = rq_list_peek(&plug->mq_list);

    if (!plug->rq_count) {
        trace_block_plug(rq->q);
    } else if (plug->rq_count >= blk_plug_max_rq_count(plug) ||
               (!blk_queue_nomerges(rq->q) &&
                blk_rq_bytes(last) >= BLK_PLUG_FLUSH_SIZE)) {
        blk_mq_flush_plug_list(plug, false);  // 达到阈值：立即刷新
        last = NULL;
    }

    // 检查是否来自多个队列
    if (!plug->multiple_queues && last && last->q != rq->q)
        plug->multiple_queues = true;
    // 检查是否有调度器分配的请求
    if (!plug->has_elevator && (rq->rq_flags & RQF_SCHED_TAGS))
        plug->has_elevator = true;
    rq_list_add_tail(&plug->mq_list, rq);
    plug->rq_count++;
}
```

**blk_mq_flush_plug_list**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）— 核心派发逻辑：

```c
void blk_mq_flush_plug_list(struct blk_plug *plug, bool from_schedule)
{
    unsigned int depth;

    if (plug->rq_count == 0)
        return;
    depth = plug->rq_count;
    plug->rq_count = 0;         // 清空计数，防止递归重入

    // 情况1：无调度器 + 非调度触发 → 批量派发优化
    if (!plug->has_elevator && !from_schedule) {
        if (plug->multiple_queues) {
            // 多队列：按 queue 分组，每组调用 queue_rqs()
            blk_mq_dispatch_multiple_queue_requests(&plug->mq_list);
            return;
        }
        // 单队列：直接调用 queue_rqs() 批量下发
        blk_mq_dispatch_queue_requests(&plug->mq_list, depth);
        if (rq_list_empty(&plug->mq_list))
            return;             // 全部下发成功
    }

    // 情况2：有调度器 或 调度触发 → 逐个派发
    do {
        blk_mq_dispatch_list(&plug->mq_list, from_schedule);
    } while (!rq_list_empty(&plug->mq_list));
}
```

**派发路径对比**：

```
无调度器 + 单队列：
  blk_mq_dispatch_queue_requests()
    ├─ q->mq_ops->queue_rqs()   // 如果驱动支持 → 一次下发整个列表
    │   └─ nvme_queue_rqs() → 批量写入 Doorbell
    └─ blk_mq_issue_direct()    // 否则逐个下发

无调度器 + 多队列：
  blk_mq_dispatch_multiple_queue_requests()
    └─ 按 queue 分组 → 每组调用 blk_mq_dispatch_queue_requests()

有调度器 或 from_schedule：
  blk_mq_dispatch_list()
    ├─ is_passthrough → 加入 hctx->dispatch
    ├─ has_elevator   → elevator->insert_requests() 进入调度器
    └─ else           → blk_mq_insert_requests() 进入 ctx 软件队列
```

#### 6.6.5 Plug 合并

在 `blk_mq_submit_bio` 中，bio 提交后会先尝试合并到 plug 列表中的已有请求（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）：

```c
bool blk_attempt_plug_merge(struct request_queue *q, struct bio *bio,
        unsigned int nr_segs)
{
    struct blk_plug *plug = current->plug;
    struct request *rq;

    if (!plug || rq_list_empty(&plug->mq_list))
        return false;

    // 优先检查 mq_list 尾部请求（最常见的合并场景）
    rq = plug->mq_list.tail;
    if (rq->q == q)
        return blk_attempt_bio_merge(q, rq, bio, nr_segs, false) == BIO_MERGE_OK;

    // 多队列场景：遍历整个 plug 列表查找同队列的请求
    if (!plug->multiple_queues)
        return false;
    rq_list_for_each(&plug->mq_list, rq) {
        if (rq->q != q)
            continue;
        if (blk_attempt_bio_merge(q, rq, bio, nr_segs, false) == BIO_MERGE_OK)
            return true;
        break;
    }
    return false;
}
```

合并成功后，bio 被合并到已有 request 中，无需创建新 request，也无需分配新的 tag。

#### 6.6.6 cached_rqs — 请求预分配

plug 机制还支持**预分配多个 request**，避免每次提交 bio 都需要重新分配 tag：

```c
// blk_mq_get_new_requests() 中：
if (plug) {
    data.nr_tags = plug->nr_ios;            // 首次请求：预分配多个 tag
    plug->nr_ios = 1;                        // 后续请求：使用缓存
    data.cached_rqs = &plug->cached_rqs;     // 预分配的请求存入 cached_rqs
}
```

后续 bio 提交时，`blk_mq_peek_cached_request()` 从 `cached_rqs` 中取出已分配好的 request，复用 tag，无需再走 tag 分配路径。

#### 6.6.7 调用者示例

**文件系统 DIO 路径**（[fops.c](file:///home/louis/code/linux/block/fops.c)）— 典型用法：

```c
blk_start_plug(&plug);

for (;;) {
    // 构造 bio ...
    submit_bio(bio);           // bio → request → blk_add_rq_to_plug()
    // 分配下一个 bio ...
}

blk_finish_plug(&plug);       // 批量提交所有请求
```

**直接 I/O 提交**（[blk-execute_rq_nowait](file:///home/louis/code/linux/block/blk-mq.c)）— 直通请求也使用 plug：

```c
if (current->plug && !at_head) {
    blk_add_rq_to_plug(current->plug, rq);  // 加入 plug，延迟下发
    return;
}
blk_mq_insert_request(rq, ...);             // 无 plug：直接下发
```

#### 6.6.8 死锁防护

进程在持有 plug 期间如果进入睡眠（等待内存分配、I/O 完成等），调度器会自动刷新 plug：

```c
// kernel/sched/core.c  schedule() 中：
blk_flush_plug(tsk->plug, true);   // 睡眠前提交所有 pending 请求

// kernel/sched/core.c  io_schedule_prepare() 中：
blk_flush_plug(current->plug, true);  // I/O 等待前提交
```

**原因**：如果请求在 plug 中未提交，而内存回收路径需要等待该请求完成才能释放页面，就会形成死锁。

#### 6.6.9 完整调用链

```
用户态 read/write
  → blkdev_read_iter / blkdev_write_iter  (fops.c)
    → blk_start_plug(&plug)
    → 循环:
        submit_bio(bio)
          → blk_mq_submit_bio(bio)        (blk-mq.c)
            → blk_mq_attempt_bio_merge()   // 先尝试合并到 plug 列表
            → blk_mq_get_new_requests()    // 分配 request（可能使用 cached_rqs）
            → blk_mq_bio_to_request()      // bio → request
            → blk_add_rq_to_plug(plug, rq) // 加入 plug 列表
    → blk_finish_plug(&plug)
      → __blk_flush_plug(plug, false)
        → flush_plug_callbacks()           // md/dm 回调
        → blk_mq_flush_plug_list(plug, false)
          ├─ [无调度器] blk_mq_dispatch_queue_requests()
          │     → q->mq_ops->queue_rqs()    // NVMe: nvme_queue_rqs() 批量下发
          │         → 循环: nvme_submit_cmd(nvmeq, cmnd, ...)
          │         → nvme_write_sq_db()    // 一次 Doorbell 更新
          ├─ [有调度器] blk_mq_dispatch_list()
          │     → elevator->insert_requests() 或 blk_mq_insert_requests()
          └─ blk_mq_free_plug_rqs()         // 释放未使用的缓存请求
```

#### 6.6.10 对 NVMe 的性能影响

对于 NVMe 设备，plug 机制的两个关键优化：

1. **`nvme_queue_rqs()`**：当 plug 中所有请求属于同一个 NVMe 队列时，一次调用即可写入多个 SQ 条目，最后只更新一次 Doorbell 寄存器，大幅减少 MMIO 写操作。

2. **批量 tag 分配**：`cached_rqs` 预分配 tag，后续 bio 无需重复获取 tag，减少了 `sbitmap` 操作的开销。

---

## 7. I/O 合并与分段

### 7.1 概述

I/O 合并与分段是块层的核心性能优化。文件 `block/blk-merge.c`（1,171 行）、`block/blk-mq-dma.c` 及其相关头文件实现了：

- **Bio 合并**：将多个连续的 bio 合并到一个 request 中（减少 request 数量）
- **Request 合并**：将两个已存在的 request 合并（在调度器中）
- **Bio 分段**：将一个 bio 按照队列限制（max_sectors, max_segments 等）拆分为多个
- **SG 映射**：将 request 的 bio 链表转换为 scatter-gather 列表，供 DMA 使用

**合并的层次**：

```
bio (page 集合)
  ↓ bio_attempt_back_merge / bio_attempt_front_merge
request (多个 bio 的集合)
  ↓ attempt_merge (request 合并)
更大的 request
  ↓ __blk_rq_map_sg
scatterlist[] (DMA 描述符)
```

### 7.2 合并前置条件

#### 7.2.1 可合并性检查

**bio 可合并**（[blk-mq-sched.h](file:///home/louis/code/linux/block/blk-mq-sched.h)）：
```c
static inline bool bio_mergeable(struct bio *bio)
{
    return !(bio->bi_opf & REQ_NOMERGE_FLAGS);  // 检查 REQ_NOMERGE 标志
}
```

**request 可合并**（[blk.h](file:///home/louis/code/linux/block/blk.h)）：
```c
static inline bool rq_mergeable(struct request *rq)
{
    if (blk_rq_is_passthrough(rq))  return false;  // 直通请求不可合并
    if (req_op(rq) == REQ_OP_FLUSH) return false;  // FLUSH 不可合并
    if (req_op(rq) == REQ_OP_WRITE_ZEROES) return false;
    if (req_op(rq) == REQ_OP_ZONE_APPEND) return false;
    // ... 其他检查
}
```

#### 7.2.2 `blk_rq_merge_ok` — bio 与 request 的合并前置检查

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）检查以下条件：

| 检查项 | 含义 |
|--------|------|
| `rq_mergeable(rq) && bio_mergeable(bio)` | 两者都可合并 |
| `req_op(rq) == bio_op(bio)` | 操作类型相同 |
| `blk_cgroup_mergeable(rq, bio)` | 同一 cgroup |
| `blk_integrity_merge_bio()` | 完整性元数据兼容 |
| `bio_crypt_rq_ctx_compatible()` | 加密上下文兼容 |
| `rq->bio->bi_write_hint == bio->bi_write_hint` | 写入提示相同 |
| `rq->bio->bi_write_stream == bio->bi_write_stream` | 写入流相同 |
| `rq->bio->bi_ioprio == bio->bi_ioprio` | I/O 优先级相同 |
| `blk_atomic_write_mergeable_rq_bio()` | 原子写兼容性 |

### 7.3 合并方向判断：`blk_try_merge`

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）判断 bio 与 request 的合并方向：

```c
enum elv_merge blk_try_merge(struct request *rq, struct bio *bio)
{
    if (blk_discard_mergable(rq))
        return ELEVATOR_DISCARD_MERGE;
    // 后向合并：rq 的结束扇区 == bio 的起始扇区
    else if (blk_rq_pos(rq) + blk_rq_sectors(rq) == bio->bi_iter.bi_sector)
        return ELEVATOR_BACK_MERGE;
    // 前向合并：rq 的起始扇区 - bio 大小 == bio 的起始扇区
    else if (blk_rq_pos(rq) - bio_sectors(bio) == bio->bi_iter.bi_sector)
        return ELEVATOR_FRONT_MERGE;
    return ELEVATOR_NO_MERGE;
}
```

合并方向示意：

```
前向合并 (FRONT):  bio → [bio | rq 原有数据]
后向合并 (BACK):   [rq 原有数据 | bio] ← bio
```

### 7.4 Bio 合并流程

#### 7.4.1 后向合并（Back Merge）

最常见的合并场景（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）：

```
bio_attempt_back_merge(req, bio, nr_segs)
  │
  ├─ ll_back_merge_fn(req, bio, nr_segs)     // 检查合并可行性
  │     ├─ req_gap_back_merge()              // 虚拟边界间隙检查
  │     ├─ integrity_req_gap_back_merge()    // 完整性间隙检查
  │     ├─ bio_crypt_ctx_back_mergeable()    // 加密兼容性
  │     ├─ 总扇区数 > max_sectors → 标记 nomerge
  │     └─ ll_new_hw_segment()               // 检查段数是否超限
  │           ├─ blk_cgroup_mergeable()
  │           ├─ blk_integrity_merge_bio()
  │           ├─ nr_phys_segments + nr_segs > max_segments → nomerge
  │           └─ req->nr_phys_segments += nr_phys_segs  // 累加段数
  │
  ├─ rq_qos_merge()                          // QoS 层通知
  ├─ blk_update_mixed_merge()                // 更新 failfast 混合标记
  ├─ req->biotail->bi_next = bio             // 将 bio 链到 request 尾部
  ├─ req->biotail = bio                      // 更新尾指针
  ├─ req->__data_len += bio->bi_iter.bi_size // 累加数据长度
  └─ bio_crypt_free_ctx(bio)                 // 释放被合并 bio 的加密上下文
```

#### 7.4.2 前向合并（Front Merge）

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）与后向对称，但 bio 插入到 request 头部：

```c
bio->bi_next = req->bio;          // bio 指向原头部
req->bio = bio;                   // 头部更新为 bio
req->__sector = bio->bi_iter.bi_sector;  // 起始扇区前移
req->__data_len += bio->bi_iter.bi_size;
```

### 7.5 Request 合并流程：`attempt_merge`

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）在调度器中将两个已存在的 request 合并：

```
attempt_merge(q, req, next)
  │
  ├─ rq_mergeable(req) && rq_mergeable(next)   // 两者可合并
  ├─ req_op(req) == req_op(next)               // 操作类型相同
  ├─ 检查 write_hint / write_stream / ioprio   // 属性相同
  ├─ blk_atomic_write_mergeable_rqs()          // 原子写兼容
  │
  ├─ blk_try_req_merge(req, next)              // 判断合并方向
  │     ├─ DISCARD_MERGE → req_attempt_discard_merge()
  │     │     └─ 检查 discard 段数限制
  │     └─ BACK_MERGE → ll_merge_requests_fn()
  │           ├─ 总扇区数 > max_sectors → 失败
  │           ├─ 总物理段数 > max_segments → 失败
  │           ├─ cgroup / integrity / crypto 检查
  │           └─ req->nr_phys_segments += next->nr_phys_segments
  │
  ├─ blk_rq_set_mixed_merge()                  // 混合合并标记
  │     └─ 将 failfast 属性分发到每个 bio
  │
  ├─ req->biotail->bi_next = next->bio         // 链接 bio 链表
  ├─ req->biotail = next->biotail              // 更新尾指针
  ├─ req->__data_len += blk_rq_bytes(next)     // 累加数据长度
  ├─ elv_merge_requests(q, req, next)          // 通知调度器更新内部状态
  └─ next->bio = NULL; return next;            // 返回 next 供调用者释放
```

### 7.6 合并的 6 条路径

bio 提交过程中，有 6 处尝试合并的位置：

| 路径 | 函数 | 位置 | 说明 |
|------|------|------|------|
| 1. Plug 合并 | `blk_attempt_plug_merge` | blk-merge.c | 合并到 plug 列表中已有的 request |
| 2. 调度器合并 | `blk_mq_sched_try_merge` | blk-merge.c | 通过 `elv_merge()` 查找调度器中的可合并 request |
| 3. 调度器 bio 合并 | `blk_mq_sched_bio_merge` | blk-mq-sched.c | 以 bio 为单位尝试合并到调度器中的 request |
| 4. bio 列表合并 | `blk_bio_list_merge` | blk-merge.c | 在 bio 列表（倒序最多 8 个）中查找合并 |
| 5. Request 后向合并 | `attempt_back_merge` | blk-merge.c | 在调度器中与后一个 request 合并 |
| 6. Request 前向合并 | `attempt_front_merge` | blk-merge.c | 在调度器中与前一个 request 合并 |

在 `blk_mq_submit_bio` 中的调用顺序：

```
blk_mq_submit_bio(bio)
  ├─ blk_mq_attempt_bio_merge(q, bio, nr_segs)
  │     ├─ blk_attempt_plug_merge(q, bio, nr_segs)     // 路径1: plug 合并
  │     └─ blk_mq_sched_bio_merge(q, bio, nr_segs)     // 路径3: 调度器 bio 合并
  │
  └─ 如果合并失败，分配新 request，然后：
        blk_mq_sched_try_merge(q, bio, nr_segs, &rq)    // 路径2: 调度器合并
          → elv_merge() 遍历调度器红黑树查找
```

### 7.7 段管理与 SG 映射

#### 7.7.1 段数计算：`blk_recalc_rq_segments`

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）重新计算一个 request 的物理段数：

```c
unsigned int blk_recalc_rq_segments(struct request *rq)
{
    rq_for_each_bvec(bv, rq, iter)
        bvec_split_segs(&rq->q->limits, &bv, &nr_phys_segs, &bytes,
                        UINT_MAX, BIO_MAX_SIZE);
    return nr_phys_segs;
}
```

通过遍历 request 的所有 bio_vec，用 `bvec_split_segs()` 按 `max_segment_size` 和 `max_segments` 拆分计算。

#### 7.7.2 `bvec_split_segs` — 段拆分核心

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）判断一个 bio_vec 是否需要被拆分为多个段：

```c
static bool bvec_split_segs(const struct queue_limits *lim,
        const struct bio_vec *bv, unsigned *nsegs, unsigned *bytes,
        unsigned max_segs, unsigned max_bytes)
{
    while (len && *nsegs < max_segs) {
        seg_size = get_max_segment_size(lim, bvec_phys(bv) + total_len, len);
        (*nsegs)++;            // 每拆出一个段，计数+1
        total_len += seg_size;
        len -= seg_size;
        // 虚拟边界检查：如果跨越边界，停止
        if ((bv->bv_offset + total_len) & lim->virt_boundary_mask)
            break;
    }
    return len > 0 || bv->bv_len > max_bytes;  // 是否需要拆分
}
```

#### 7.7.3 SG 列表映射：`__blk_rq_map_sg`

（[blk-mq-dma.c](file:///home/louis/code/linux/block/blk-mq-dma.c)）将 request 的 bio 链转换为 scatter-gather 列表：

```c
int __blk_rq_map_sg(struct request *rq, struct scatterlist *sglist,
                    struct scatterlist **last_sg)
{
    blk_rq_map_iter_init(rq, &iter);
    while (blk_map_iter_next(rq, &iter, &vec)) {
        // 合并相邻的物理连续页
        *last_sg = blk_next_sg(last_sg, sglist);
        sg_set_page(*last_sg, phys_to_page(vec.paddr), vec.len,
                    offset_in_page(vec.paddr));
        nsegs++;
    }
    sg_mark_end(*last_sg);
    return nsegs;
}
```

`blk_map_iter_next()` 内部会合并物理连续的 bio_vec，减少最终 SG 条目数。

#### 7.7.4 队列限制（`queue_limits`）中的相关字段

（[blkdev.h](file:///home/louis/code/linux/include/linux/blkdev.h)）影响合并与分段的关键限制：

| 字段 | 含义 | 典型 NVMe 值 |
|------|------|-------------|
| `max_sectors` | 单个 request 最大扇区数 | 1024 (512KB) |
| `max_segments` | 单个 request 最大 SG 段数 | 128 |
| `max_segment_size` | 单个段的最大字节数 | 65536 (64KB) |
| `seg_boundary_mask` | 段边界对齐掩码 | 0xffff (64KB 边界) |
| `virt_boundary_mask` | 虚拟边界掩码 | 0 (NVMe 无) |
| `logical_block_size` | 逻辑块大小 | 512 |
| `physical_block_size` | 物理块大小 | 4096 |
| `chunk_sectors` | RAID chunk 大小 | 0 (单盘) |

### 7.8 虚拟边界间隙检查：`bio_will_gap`

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）某些设备（如 SATA AHCI）要求 SG 列表中的段之间不能有太大的物理间隙。如果前一个 bio 的最后一个 bvec 的物理地址与后一个 bio 的第一个 bvec 之间的偏移跨越了 `virt_boundary_mask` 边界，则不能合并。

```c
static inline bool bio_will_gap(struct request_queue *q,
        struct request *prev_rq, struct bio *prev, struct bio *next)
{
    if (!bio_has_data(prev) || !queue_virt_boundary(q))
        return false;                           // 无虚拟边界限制则放行

    // 如果第一个 bio 的起始偏移不在边界上 → 不能合并
    if (pb.bv_offset & queue_virt_boundary(q))
        return true;

    // 检查 last_bvec(prev) 和 first_bvec(next) 是否物理连续
    if (biovec_phys_mergeable(q, &pb, &nb))
        return false;
    return __bvec_gap_to_prev(&q->limits, &pb, nb.bv_offset);
}
```

对应前向/后向合并的包装函数：
- `req_gap_back_merge(req, bio)` — 后向合并间隙检查
- `req_gap_front_merge(req, bio)` — 前向合并间隙检查

### 7.9 Bio 分段流程

#### 7.9.1 分段入口：`bio_split_to_limits`

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）将 bio 按队列限制拆分：

```
bio_split_to_limits(bio)
  → bio_split_rw(bio, lim, &nr_segs)
    → bio_split_rw_at(bio, lim, nr_segs, max_io_size)
      → bio_split_io_at(bio, lim, &segs, max_bytes, alignment_mask)
        → 遍历 bio 的每个 bvec:
            ├─ 检查 DMA 对齐
            ├─ 检查虚拟边界间隙 → 需要拆分
            ├─ 如果 nsegs < max_segments && 字节数 < max_bytes
            │     → 累积到当前段
            └─ 否则 → bvec_split_segs() 按 max_segment_size 拆分
                          → 如果仍需拆分，goto split
      → 返回 split_sectors（0 = 不需要拆分，>0 = 拆分位置，<0 = 错误）
    → 如果需要拆分：bio_submit_split() 创建新 bio
```

#### 7.9.2 `get_max_io_size` — 计算最大 I/O 大小

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c)）根据操作类型和队列限制计算最大扇区数：

```c
static inline unsigned get_max_io_size(struct bio *bio,
                                       const struct queue_limits *lim)
{
    if (bio_op(bio) == REQ_OP_WRITE_ZEROES)
        max_sectors = lim->max_write_zeroes_sectors;
    else if (is_atomic)
        max_sectors = lim->atomic_write_max_sectors;
    else
        max_sectors = lim->max_sectors;

    // 对齐到 physical_block_size 边界
    end = (start + max_sectors) & ~(pbs - 1);
    if (end > start) return end - start;
    return max_sectors & ~(lbs - 1);
}
```

#### 7.9.3 分段类型

| 操作类型 | 分段函数 | 说明 |
|----------|---------|------|
| 普通读写 | `bio_split_rw` | 按 max_sectors / max_segments 拆分 |
| Discard | `bio_split_discard` | 按 discard_granularity 对齐拆分 |
| Zone Append | `bio_split_zone_append` | 禁止拆分（必须单次提交） |
| Write Zeroes | `bio_split_write_zeroes` | 按 max_write_zeroes_sectors 拆分 |

### 7.10 完整合并与分段调用链

```
用户态 write()
  → blkdev_write_iter(fops.c)
    → blk_start_plug(&plug)
    → 循环:
        submit_bio(bio)
          → blk_mq_submit_bio(bio)
            │
            ├─ blk_mq_attempt_bio_merge(q, bio, nr_segs)
            │     ├─ blk_attempt_plug_merge()        // [合并] Plug 列表合并
            │     │     → blk_attempt_bio_merge()
            │     │       → blk_try_merge()          // 判断方向
            │     │       → bio_attempt_back_merge() // 执行合并
            │     │           → ll_back_merge_fn()
            │     │             → ll_new_hw_segment()
            │     └─ blk_mq_sched_bio_merge()        // [合并] 调度器合并
            │
            ├─ [合并失败] 分配新 request
            │     blk_mq_get_new_requests()
            │
            ├─ blk_mq_sched_try_merge()              // [合并] 调度器 request 合并
            │     → elv_merge() 遍历红黑树
            │
            ├─ [如果 bio 太大] 分段
            │     bio = bio_split_to_limits(bio)
            │       → bio_split_rw() → bio_split_io_at()
            │         → 遍历 bvec，按 limits 拆分
            │
            └─ 提交到 plug 或直接下发
    → blk_finish_plug(&plug)
      → 批量派发所有请求
        → nvme_queue_rq(rq)
          → __blk_rq_map_sg(rq, sglist, &last_sg)   // [SG映射] 生成 DMA 描述符
            → 遍历 bio 链表
            → 合并物理连续的 bvec
            → 设置 sg_set_page() 每条 SG 条目
            → sg_mark_end()
```

---

## 8. 刷新与屏障（Flush/FUA）

### 8.1 概述

文件 `block/blk-flush.c`（540 行）实现了块层的刷新（Flush）与强制单元访问（FUA）机制。在有回写缓存（Write-back Cache）的设备上，写数据可能只到达了设备缓存而非持久介质，刷新机制确保数据被持久化（写入非易失性介质）。

**核心概念**：

| 概念 | 标志 | 含义 |
|------|------|------|
| PREFLUSH | `REQ_PREFLUSH` | 在数据写入前先刷新设备缓存 |
| FUA | `REQ_FUA` | 数据写入必须直接到达非易失性介质（绕过缓存） |
| POSTFLUSH | 无独立标志 | 在数据写入后刷新设备缓存（FUA 的软件模拟） |

**语义**：
- 仅有 `REQ_PREFLUSH` 无数据 → 单纯的缓存刷新（如 `sync()`）
- `REQ_PREFLUSH` + 数据 → 数据写入前先刷新（保证写入前缓存是干净的）
- 数据 + `REQ_FUA` → 数据本身必须落到持久介质
- 数据 + `REQ_PREFLUSH` + `REQ_FUA` → 写入前刷新，写入本身也必须持久化

### 8.2 屏障（Barrier）与 FUA 深度解析

#### 8.2.1 什么是"屏障"

在存储领域的语境中，"屏障"（Barrier）是一个比"刷新"（Flush）更古老的概念。它的核心语义是：**确保屏障之前的 I/O 全部落地到持久介质后，才允许屏障之后的 I/O 开始执行**。

在 Linux 2.6.37 之前，块层使用专门的屏障机制（`REQ_HARDBARRIER`、`REQ_SOFTBARRIER`、`blk_queue_ordered()` 等），其工作方式为：

```
旧 Barrier 机制（已废弃）：
  1. 排空（drain）所有正在进行的 I/O
  2. 下发硬件刷新命令（SYNCHRONIZE_CACHE / FLUSH_CACHE）
  3. 等待刷新完成
  4. 执行屏障写请求
  5. 下发硬件刷新命令
  6. 等待刷新完成
  7. 恢复正常的 I/O 调度
```

**问题**：这种机制要求所有 I/O 串行化，即使访问的是不同的磁盘区域，也必须等待屏障完成。这在高并发场景下造成了严重的性能损失。

2011 年，Tejun Heo 用新的 Flush/FUA 机制替换了旧的 Barrier 机制，核心思想是：**将屏障语义拆分为两个独立的原语——PREFLUSH（前刷新）和 FUA（强制单元访问），允许它们与数据请求灵活组合，而不再需要全局串行化。**

#### 8.2.2 FUA（Forced Unit Access）的本质

FUA 是 **硬件层面的命令标志**，而不是内核自己实现的软件机制。它的含义是：**这条命令携带的数据必须直接写入非易失性介质，不得只停留在设备缓存中就报告完成**。

**FUA 在不同协议中的实现**：

| 协议 | 命令 | FUA 位位置 | 行为 |
|------|------|-----------|------|
| NVMe | Write | CDW12 bit 9 (`NVME_RW_FUA`) | 数据直接写入 NAND，绕过 DRAM 缓存 |
| SCSI | WRITE(10/16) | CDB Byte 1 bit 3 | 逻辑块直接写入非易失性介质 |
| ATA/NCQ | WRITE FPDMA | 命令码 0x3D (FUA EXT) | 数据直接写入盘片，绕过磁盘缓存 |
| SATA | WRITE DMA FUA EXT | 命令码 0x3D | 同上 |

**FUA 与 Flush 的本质区别**：

```
                            PREFLUSH（缓存刷新）              FUA（强制单元访问）
作用范围：                    清空整个设备缓存                  仅影响本条命令携带的数据
性能影响：                    较大（需等待所有缓存数据落盘）      较小（仅本条命令绕过缓存）
串行化要求：                   需要与其他命令互斥                 可以与其他命令并发
硬件支持：                    所有带缓存的设备都支持             需要设备声明支持（BLK_FEAT_FUA）
典型使用场景：                  fsync 前确保之前的数据都落盘       journal 提交时确保本事务落盘
```

**NVMe 协议中的 FUA 示例**（[NVMe 规范](https://nvmexpress.org/)）：

```
NVMe Write Command Dword 12:
  Bits 31:16  - 保留
  Bit 15      - Limited Retry (LR)
  Bit 14      - Deallocate
  Bit 9       - Force Unit Access (FUA)  ← 本条命令的数据必须持久化
  Bit 8       - Protection Information Check
  Bits 7:0    - Protection Information Field
```

内核中 NVMe 驱动将块层 FUA 标志映射到 NVMe 命令（[core.c](file:///home/louis/code/linux/drivers/nvme/host/core.c)）：

```c
if (req->cmd_flags & REQ_FUA)
    control |= NVME_RW_FUA;  // 设置 NVMe 命令的 FUA 位
```

#### 8.2.3 屏障语义的现代实现

旧的屏障语义（"前序 I/O 全部落盘 → 本请求 → 后序 I/O 才能开始"）现在通过组合 PREFLUSH 和 FUA 来实现：

```
旧屏障:                    [之前I/O] [屏障] [之后I/O]
                              ↓
新实现:  REQ_PREFLUSH + REQ_FUA + 数据
                              ↓
            阶段1: PREFLUSH → 发射 REQ_OP_FLUSH 清空缓存
            阶段2: DATA     → 发射 REQ_OP_WRITE + REQ_FUA（数据绕过缓存写入）
                              ↓
            效果：前序 I/O 已落盘（PREFLUSH），本请求数据已落盘（FUA）
```

**为什么拆分后性能更好**：

1. **PREFLUSH 和 DATA 可以与其他请求并发**：PREFLUSH 只刷新缓存，不阻塞其他不相关的读写请求
2. **FUA 不需要全局排空**：只有带 FUA 的请求需要绕过缓存，其他请求可以正常使用缓存
3. **多个请求可以共享一个 PREFLUSH/POSTFLUSH**：比如连续的 journal 写入，多个请求可以共享同一个 POSTFLUSH，减少 flush 命令数量

#### 8.2.4 FUA 与 POSTFLUSH 的等价性

当设备不声明 `BLK_FEAT_FUA` 时，块层用 POSTFLUSH 模拟 FUA 的效果：

```
FUA 模式（硬件支持）:          POSTFLUSH 模式（软件模拟）:
  WRITE + FUA                    WRITE
  ↓ 数据直接落盘                  ↓ 数据可能仅到缓存
  → 完成                          → 完成（仅通知刷新状态机）
                                  ↓ 追加 REQ_OP_FLUSH
                                  → 缓存全体落盘
                                  → 真正完成

  优点：仅本条命令受影响         优点：兼容所有设备
       性能开销小                      不需要硬件支持 FUA
  缺点：需要硬件支持 FUA        缺点：性能开销大（整个缓存都要刷）
```

**关键区别**：FUA 只保证本条命令的数据持久化；POSTFLUSH 保证整个缓存都持久化（包括同队列中其他请求的数据）。因此，POSTFLUSH 的副作用更大，但语义更强。

#### 8.2.5 文件系统的典型使用模式

以 ext4 的 journal 提交为例：

```
// 场景1: 提交 journal descriptor
bio = REQ_OP_WRITE | REQ_FUA
// 语义: journal 描述符必须立即落盘，否则崩溃后无法恢复

// 场景2: fsync() 操作
bio = REQ_OP_WRITE | REQ_PREFLUSH | REQ_FUA
// 语义: 先清空缓存（确保之前的数据都落盘）→ 写入本事务数据并落盘

// 场景3: 纯 sync/flush
bio = REQ_OP_WRITE | REQ_PREFLUSH (空数据)
// 语义: 仅清空缓存，不写入任何数据
```

#### 8.2.6 barrier 与 flush 的命名澄清

在 Linux 内核语境中：

| 术语 | 时期 | 含义 |
|------|------|------|
| **Barrier** | 2.6.37 之前 | 完整的 I/O 屏障：排空 → 刷新 → 写入 → 刷新 → 恢复 |
| **Flush** | 2.6.37 至今 | 仅刷新设备缓存（`REQ_OP_FLUSH`） |
| **FUA** | 2.6.37 至今 | 强制单元访问，本条命令的数据绕过缓存 |
| **PREFLUSH** | 2.6.37 至今 | 在数据写入前先刷新缓存 |
| **POSTFLUSH** | 2.6.37 至今 | 在数据写入后刷新缓存（FUA 的软件回退） |

**文档中的"屏障"**是指现代意义上的 Flush + FUA 组合，它实现了旧屏障的语义，但实现方式完全不同。由于历史原因，很多文档仍沿用"屏障"这个词，但代码中已不再使用。

---

### 8.3 关键数据结构

#### 8.3.1 `blk_flush_queue` — 刷新队列

（[blk.h](file:///home/louis/code/linux/block/blk.h)）每个硬件队列（`blk_mq_hw_ctx`）拥有一个刷新队列：

```c
struct blk_flush_queue {
    spinlock_t      mq_flush_lock;         // 保护刷新队列的自旋锁
    unsigned int    flush_pending_idx:1;   // 等待刷新的请求在哪个队列（0或1）
    unsigned int    flush_running_idx:1;   // 正在刷新的请求在哪个队列（0或1）
    blk_status_t    rq_status;             // 刷新完成状态
    unsigned long   flush_pending_since;   // 开始等待刷新的时间戳（jiffies）
    struct list_head flush_queue[2];       // 乒乓缓冲队列
    unsigned long   flush_data_in_flight;  // 正在执行 DATA 阶段的请求数
    struct request  *flush_rq;             // 预分配的刷新请求
    struct rcu_head rcu_head;              // RCU 延迟释放
};
```

**双缓冲机制**：`flush_queue[0]` 和 `flush_queue[1]` 交替使用。当 `pending_idx != running_idx` 时，表示有一个刷新正在进行中，新的请求被排队到 `flush_queue[pending_idx]`。

#### 8.3.2 请求内置的刷新字段

每个 `struct request` 中内嵌了刷新状态追踪字段：

```c
struct request {
    struct {
        unsigned int seq;          // 刷新序列阶段（位掩码）
        struct request *rq_next;   // 双缓冲队列中的链表指针
        rq_end_io_fn *saved_end_io; // 保存原始的 end_io 回调
    } flush;
    // ...
};
```

### 8.4 刷新序列阶段

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c)）一个请求的刷新序列是 PREFLUSH → DATA → POSTFLUSH 的子集：

```c
enum {
    REQ_FSEQ_PREFLUSH  = (1 << 0),  // 预刷新阶段
    REQ_FSEQ_DATA      = (1 << 1),  // 数据写入阶段
    REQ_FSEQ_POSTFLUSH = (1 << 2),  // 后刷新阶段
    REQ_FSEQ_DONE      = (1 << 3),  // 全部完成

    REQ_FSEQ_ACTIONS   = REQ_FSEQ_PREFLUSH | REQ_FSEQ_DATA |
                         REQ_FSEQ_POSTFLUSH,
    FLUSH_PENDING_TIMEOUT = 5 * HZ,  // 5秒饥饿超时
};
```

`rq->flush.seq` 记录当前已完成哪些阶段。`blk_flush_cur_seq(rq)` 通过 `ffz()` 找到第一个未完成的阶段。

### 8.5 策略转换：`blk_insert_flush`

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c)）这是刷新状态机的入口。根据请求的标志和设备能力，决定需要执行哪些阶段：

```
blk_insert_flush(rq)
  │
  ├─ 根据请求标志计算 policy：
  │     rq->cmd_flags & REQ_PREFLUSH → policy |= REQ_FSEQ_PREFLUSH
  │     rq->cmd_flags & REQ_FUA && !supports_fua → policy |= REQ_FSEQ_POSTFLUSH
  │     有数据 → policy |= REQ_FSEQ_DATA
  │
  ├─ 清除 driver 可能不理解的标志：
  │     rq->cmd_flags &= ~REQ_PREFLUSH
  │     if (!supports_fua) rq->cmd_flags &= ~REQ_FUA
  │
  └─ 根据 policy 分发：
        case 0:                              // 无任何操作需要
            blk_mq_end_request(rq, 0);       // 直接完成
            return true;
        case REQ_FSEQ_DATA:                  // 仅数据，无刷新
            return false;                    // 正常处理
        case REQ_FSEQ_DATA|REQ_FSEQ_POSTFLUSH: // 数据 + 后刷新
            blk_rq_init_flush(rq);           // 初始化刷新状态
            rq->flush.seq |= REQ_FSEQ_PREFLUSH; // 标记 PREFLUSH 已"完成"
            return false;                    // 先正常下发数据
        default:                             // 含 PREFLUSH 的序列
            blk_rq_init_flush(rq);
            blk_flush_complete_seq(rq, fq, REQ_FSEQ_ACTIONS & ~policy, 0);
            return true;                     // 接管请求
```

**关键行为**：
- `blk_rq_init_flush(rq)` 保存原始 `end_io` 为 `saved_end_io`，替换为 `mq_flush_data_end_io`
- `blk_flush_complete_seq(..., REQ_FSEQ_ACTIONS & ~policy, 0)` 立即标记不需要的阶段为已完成，并触发第一个需要的阶段

### 8.6 三阶段刷新序列

#### 8.6.1 场景 1：设备有回写缓存 + 支持 FUA（如 NVMe）

```
请求: REQ_PREFLUSH | REQ_FUA | REQ_OP_WRITE + 数据
  ↓ blk_insert_flush
policy = PREFLUSH | DATA  (FUA 直接随 DATA 下发，无需 POSTFLUSH)
  ↓
阶段1: PREFLUSH → 下发 REQ_OP_FLUSH 命令
  ↓ 完成
阶段2: DATA → 下发 REQ_OP_WRITE | REQ_FUA 命令（FUA 位直接传递给 NVMe）
  ↓ 完成
请求完成，通知上层
```

#### 8.6.2 场景 2：设备有回写缓存 + 不支持 FUA（如老式 SATA）

```
请求: REQ_PREFLUSH | REQ_FUA | REQ_OP_WRITE + 数据
  ↓ blk_insert_flush
policy = PREFLUSH | DATA | POSTFLUSH  (FUA 用 POSTFLUSH 模拟)
  ↓
阶段1: PREFLUSH → 下发 REQ_OP_FLUSH 命令
  ↓ 完成
阶段2: DATA → 下发 REQ_OP_WRITE 命令（无 FUA 标志）
  ↓ 完成（仅通知刷新状态机，不通知上层）
阶段3: POSTFLUSH → 下发 REQ_OP_FLUSH 命令
  ↓ 完成
blk_flush_restore_request(rq) → 恢复原始 end_io
blk_mq_end_request(rq) → 通知上层完成
```

#### 8.6.3 场景 3：简单 flush（无数据）

```
请求: REQ_OP_WRITE | REQ_PREFLUSH（无数据，如 sync）
  ↓ blk_insert_flush
policy = PREFLUSH
  ↓
阶段1: PREFLUSH → 下发 REQ_OP_FLUSH
  ↓ 完成
blk_flush_restore_request(rq) → 恢复
blk_mq_end_request(rq) → 完成
```

#### 8.6.4 场景 4：无回写缓存设备

```
blk_queue_write_cache() == false → 设备无写缓存
  ↓ blk_insert_flush
policy = DATA  (PREFLUSH 和 FUA 被忽略)
  ↓
return false → 正常处理数据请求
```

### 8.7 刷新触发机制：`blk_kick_flush`

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c)）当刷新状态变化时，检查是否需要下发新的刷新命令：

```
blk_kick_flush(q, fq, cmd_flags)
  │
  ├─ C1 检查：pending_idx == running_idx && pending 不为空
  │     如果 pending_idx != running_idx → 已有刷新在进行中，跳过
  │
  ├─ C2 检查：如果有 flush_data_in_flight 且未超时
  │     存在正在执行 DATA 的请求 → 延迟刷新（等待合并）
  │
  ├─ C3 检查：flush_pending_since + FLUSH_PENDING_TIMEOUT < jiffies
  │     等待超过 5 秒 → 强制刷新，防止饥饿
  │
  └─ 下发刷新：
        fq->flush_pending_idx ^= 1;          // 切换 pending 队列
        flush_rq = fq->flush_rq;             // 使用预分配的 flush_rq
        flush_rq->cmd_flags = REQ_OP_FLUSH | REQ_PREFLUSH;
        flush_rq->end_io = flush_end_io;      // 设置完成回调
        list_add_tail(&flush_rq->queuelist, &q->flush_list);
        blk_mq_kick_requeue_list(q);          // 触发 requeue work
```

**三个条件的设计意图**：
- **C1**：互斥性，同一 hctx 同时只有一个 flush 命令在飞行
- **C2**：合并优化，多个请求共享一个 POSTFLUSH（如果 DATA 还在执行，则 POSTFLUSH 可以等到所有 DATA 完成后再一起执行）
- **C3**：防饥饿，即使一直有 DATA 在执行，5 秒后也必须下发刷新

### 8.8 刷新完成处理：`flush_end_io`

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c)）刷新命令完成时的回调：

```
flush_end_io(flush_rq, error)
  │
  ├─ 获取 fq->mq_flush_lock 自旋锁
  ├─ blk_account_io_flush(flush_rq)           // 统计刷新 I/O
  ├─ 标记 flush_rq->state = MQ_RQ_IDLE
  │
  ├─ 切换 running_idx：
  │     fq->flush_running_idx ^= 1;           // 与 pending_idx 对齐
  │
  └─ 遍历 running 队列中所有等待的请求：
        for each rq in flush_queue[old_running_idx]:
            seq = blk_flush_cur_seq(rq);       // 获取当前阶段
            blk_flush_complete_seq(rq, fq, seq, error);
            // 推进到下一个阶段
```

### 8.9 `blk_flush_complete_seq` — 阶段推进

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c)）每个阶段完成后，推进到下一个阶段：

```
blk_flush_complete_seq(rq, fq, seq, error)
  │
  ├─ rq->flush.seq |= seq;                    // 标记当前阶段完成
  ├─ seq = blk_flush_cur_seq(rq);             // 获取下一个阶段
  │
  └─ switch (seq):
        case REQ_FSEQ_PREFLUSH:
        case REQ_FSEQ_POSTFLUSH:
            list_add_tail(&rq->queuelist, pending);  // 加入等待队列
            break;
        case REQ_FSEQ_DATA:
            fq->flush_data_in_flight++;              // 计数+1
            list_move(&rq->queuelist, &q->requeue_list);  // 加入 requeue 列表
            blk_mq_kick_requeue_list(q);             // 触发下发
            break;
        case REQ_FSEQ_DONE:
            list_del_init(&rq->queuelist);
            blk_flush_restore_request(rq);           // 恢复原始 end_io
            blk_mq_end_request(rq, error);           // 最终完成通知
            break;
```

### 8.10 数据阶段完成：`mq_flush_data_end_io`

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c)）当 DATA 阶段完成时触发：

```
mq_flush_data_end_io(rq, error)
  │
  ├─ fq->flush_data_in_flight--;              // 计数-1
  ├─ blk_flush_complete_seq(rq, fq, REQ_FSEQ_DATA, error);
  │     └─ 如果下一个阶段是 POSTFLUSH → 加入 pending 队列
  │     └─ 如果下一个阶段是 DONE → 恢复并完成请求
  │
  └─ blk_mq_sched_restart(hctx);              // 重新调度硬件队列
```

**注意**：DATA 阶段完成时，并不会通知上层（bio 的 submitter）。这是因为请求可能还有 POSTFLUSH 阶段要执行。只有整个序列完成（`REQ_FSEQ_DONE`）时，才会调用 `blk_mq_end_request()` 最终通知上层。

### 8.11 硬件队列中的刷新路由

#### 8.11.1 刷新请求直接 bypass 到 dispatch 队列

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）在 `blk_mq_insert_request` 中，FLUSH 请求被特殊处理：

```c
if (req_op(rq) == REQ_OP_FLUSH) {
    // 直接插入 hctx->dispatch 队列头部
    // 好处：在 NCQ 设备上，FLUSH 是非 NCQ 命令，插入头部可以减少延迟
    blk_mq_request_bypass_insert(rq, BLK_MQ_INSERT_AT_HEAD);
}
```

#### 8.11.2 刷新请求的 requeue 路径

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）`blk_mq_requeue_work` 分别处理 `requeue_list` 和 `flush_list`：

```
blk_mq_requeue_work(work)
  ├─ 从 requeue_list 取出请求 → blk_mq_insert_request()
  └─ 从 flush_list 取出请求 → blk_mq_insert_request()
  └─ blk_mq_run_hw_queues(q, false)
```

### 8.12 NVMe 驱动中的 FUA 支持

（[core.c](file:///home/louis/code/linux/drivers/nvme/host/core.c)）NVMe 设备通过 `Identify Controller` 的 VWC（Volatile Write Cache）字段声明支持：

```c
if ((ns->ctrl->vwc & NVME_CTRL_VWC_PRESENT) && !info->no_vwc)
    lim.features |= BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA;
else
    lim.features &= ~(BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA);
```

（[core.c](file:///home/louis/code/linux/drivers/nvme/host/core.c)）在构造 NVMe 命令时，FUA 标志直接映射到 NVMe 协议：

```c
if (req->cmd_flags & REQ_FUA)
    control |= NVME_RW_FUA;  // 设置 NVMe 命令的 FUA 位
```

NVMe 的 FUA 位指示控制器将数据直接写入非易失性介质，无需额外的 FLUSH 命令。

### 8.13 用户态接口

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c)）`blkdev_issue_flush()` 让内核其他子系统（如 fsync、journal）发出刷新：

```c
int blkdev_issue_flush(struct block_device *bdev)
{
    struct bio bio;
    bio_init(&bio, bdev, NULL, 0, REQ_OP_WRITE | REQ_PREFLUSH);
    return submit_bio_wait(&bio);
}
```

文件系统层（如 ext4）在 `fsync` 时生成 `REQ_PREFLUSH | REQ_FUA` 请求，确保数据和元数据都持久化。

### 8.14 完整刷新序列调用链

```
用户态 fsync(fd)
  → __x64_sys_fsync
    → ext4_sync_file
      → blkdev_issue_flush(bdev) 或 提交 REQ_PREFLUSH|REQ_FUA bio
        → submit_bio(bio)
          → blk_mq_submit_bio(bio)
            → blk_mq_get_new_requests(bio)
            → op_is_flush(bio->bi_opf) && blk_insert_flush(rq)
              │
              ├─ 计算 policy（PREFLUSH, DATA, POSTFLUSH）
              ├─ 初始化 rq->flush：
              │     saved_end_io = rq->end_io
              │     rq->end_io = mq_flush_data_end_io
              │
              ├─ 标记已完成阶段 → blk_flush_complete_seq()
              │     │
              │     ├─ 阶段1: PREFLUSH → 加入 fq->flush_queue[pending_idx]
              │     │     └─ blk_kick_flush() 检查 C1/C2/C3
              │     │         └─ 下发 REQ_OP_FLUSH 到硬件
              │     │           └─ 硬件完成 → flush_end_io()
              │     │               └─ 遍历 running 队列
              │     │                   └─ blk_flush_complete_seq(rq, PREFLUSH)
              │     │
              │     ├─ 阶段2: DATA → 加入 requeue_list
              │     │     └─ blk_mq_kick_requeue_list()
              │     │         └─ blk_mq_requeue_work()
              │     │           └─ blk_mq_insert_request()
              │     │             └─ nvme_queue_rq() → 提交 NVMe 写命令
              │     │               └─ 硬件完成 → mq_flush_data_end_io()
              │     │                   └─ blk_flush_complete_seq(rq, DATA)
              │     │
              │     ├─ 阶段3: POSTFLUSH → 加入 fq->flush_queue[pending_idx]
              │     │     └─ blk_kick_flush() → 下发 REQ_OP_FLUSH
              │     │         └─ 硬件完成 → flush_end_io()
              │     │             └─ blk_flush_complete_seq(rq, POSTFLUSH)
              │     │
              │     └─ REQ_FSEQ_DONE:
              │           blk_flush_restore_request(rq)
              │           blk_mq_end_request(rq) → bio_endio() → 唤醒 fsync
              │
              └─ return true (已被刷新状态机接管)
```

---

## 9. QoS 与资源控制

### 9.1 概述：rq_qos 框架架构

块层 QoS 的实现基于 **rq_qos（Request Quality of Service）** 框架，定义在 `block/blk-rq-qos.h` 和 `block/blk-rq-qos.c`。

#### 9.1.1 核心数据结构

```c
// block/blk-rq-qos.h

// rq_wait — 每个 QoS 策略的等待队列，用于限制并发请求数
struct rq_wait {
    wait_queue_head_t wait;       // 等待队列（进程在此睡眠等待）
    atomic_t inflight;            // 当前正在处理的请求数（原子计数）
};

// rq_qos — QoS 策略节点，通过单向链表链接多个策略
struct rq_qos {
    const struct rq_qos_ops *ops; // 策略操作函数表
    struct gendisk *disk;         // 关联的磁盘
    enum rq_qos_id id;            // 策略标识：WBT / LATENCY / COST
    struct rq_qos *next;          // 下一个策略（链表头：q->rq_qos）
#ifdef CONFIG_BLK_DEBUG_FS
    struct dentry *debugfs_dir;   // debugfs 目录（用于导出内部状态）
#endif
};

// rq_qos_ops — 每个策略必须实现的操作钩子
struct rq_qos_ops {
    void (*throttle)(struct rq_qos *, struct bio *);   // 限流：bio 提交时
    void (*track)(struct rq_qos *, struct request *, struct bio *);  // 追踪：bio→request 绑定
    void (*merge)(struct rq_qos *, struct request *, struct bio *);  // 合并：bio 合并到 request
    void (*issue)(struct rq_qos *, struct request *);  // 下发：request 开始执行
    void (*requeue)(struct rq_qos *, struct request *); // 重入队
    void (*done)(struct rq_qos *, struct request *);    // 完成：request 完成
    void (*done_bio)(struct rq_qos *, struct bio *);    // bio 完成
    void (*cleanup)(struct rq_qos *, struct bio *);     // 清理
    void (*queue_depth_changed)(struct rq_qos *);       // 队列深度变化
    void (*exit)(struct rq_qos *);                      // 退出
    const struct blk_mq_debugfs_attr *debugfs_attrs;   // debugfs 属性表（调试用）
};

// rq_depth — 请求深度控制（用于 WBT 和 iolatency 的动态缩放）
struct rq_depth {
    unsigned int max_depth;       // 当前最大深度
    int scale_step;               // 缩放步长（正=缩小，负=放大）
    bool scaled_max;              // 是否已达最大
    unsigned int queue_depth;     // 设备队列深度
    unsigned int default_depth;   // 默认深度
};

// 策略 ID 枚举
enum rq_qos_id {
    RQ_QOS_WBT,      // Writeback Throttling
    RQ_QOS_LATENCY,  // IO Latency
    RQ_QOS_COST,     // IO Cost Model
};
```

#### 9.1.2 链式结构与生命周期

多个 QoS 策略以链表形式组织在 `request_queue->rq_qos` 上：

```
request_queue->rq_qos
    ┌───────────┐    next    ┌───────────┐    next    ┌───────────┐
    │  rq_qos   │ ────────→  │  rq_qos   │ ────────→  │  rq_qos   │
    │ id=WBT    │            │ id=LATENCY│            │ id=COST    │
    │ ops=wbt.. │            │ ops=iolat │            │ ops=iocost│
    └───────────┘            └───────────┘            └───────────┘
```

当调用 `rq_qos_add()` 添加策略时，**冻结队列**（`blk_mq_freeze_queue`）确保无 I/O 飞行，然后插入链表头部并设置 `QUEUE_FLAG_QOS_ENABLED`。

QoS 操作通过 `__rq_qos_*` 函数遍历链表，依次调用每个策略的同名钩子：

```c
// block/blk-rq-qos.c
void __rq_qos_throttle(struct rq_qos *rqos, struct bio *bio)
{
    do {
        if (rqos->ops->throttle)
            rqos->ops->throttle(rqos, bio);
        rqos = rqos->next;   // 遍历链表
    } while (rqos);
}
```

#### 9.1.3 QoS 在 I/O 路径中的完整集成点

rq_qos 的钩子函数被内联在 blk-mq 的各个关键路径上，通过 `QUEUE_FLAG_QOS_ENABLED` 标志快速跳过（无 QoS 时无额外开销）：

```
I/O 提交路径（submit_bio → blk_mq_submit_bio）：
  ┌─────────────────────────────────────────────────────────┐
  │ submit_bio()                                            │
  │   ├─ bio_set_ioprio()          ← blk-ioprio: 设置优先级  │
  │   └─ submit_bio_noacct()                                │
  │        └─ blk_throtl_bio()     ← blk-throttle: 带宽限流  │
  │             └─ submit_bio_noacct_nocheck()              │
  │                  └─ __submit_bio_noacct_mq()            │
  │                       └─ blk_mq_submit_bio()            │
  │                            ├─ rq_qos_throttle()         │
  │                            │    ← WBT/iolatency/iocost  │
  │                            ├─ __blk_mq_alloc_requests() │
  │                            │    └─ 若失败:              │
  │                            │      rq_qos_cleanup()      │
  │                            └─ rq_qos_track()            │
  │                                 ← 绑定 bio→request      │
  └─────────────────────────────────────────────────────────┘

I/O 下发路径（blk_mq_start_request）：
  ┌─────────────────────────────────────────────────────────┐
  │ blk_mq_start_request()                                  │
  │   └─ rq_qos_issue()              ← 通知策略：请求已下发  │
  └─────────────────────────────────────────────────────────┘

I/O 完成路径（blk_mq_free_request → bio_endio）：
  ┌─────────────────────────────────────────────────────────┐
  │ blk_mq_free_request()                                   │
  │   └─ rq_qos_done()              ← 通知策略：请求已完成   │
  │                                                          │
  │ bio_endio() → ... → bio_put() → bio_free()               │
  │   └─ rq_qos_done_bio()          ← 通知策略：bio 已释放   │
  └─────────────────────────────────────────────────────────┘

I/O 重入队/合并路径：
  ┌─────────────────────────────────────────────────────────┐
  │ blk_mq_requeue_request()                                 │
  │   └─ rq_qos_requeue()           ← 通知策略：请求重入队   │
  │                                                          │
  │ bio_attempt_*_merge()                                    │
  │   └─ rq_qos_merge()             ← 通知策略：bio 已合并   │
  └─────────────────────────────────────────────────────────┘
```

#### 9.1.4 `rq_qos_wait()` — 统一限流原语

`block/blk-rq-qos.c` 中的 `rq_qos_wait()` 提供了统一的限流睡眠机制，WBT、iolatency、iocost 均使用此原语：

```c
void rq_qos_wait(struct rq_wait *rqw, void *private_data,
         acquire_inflight_cb_t *acquire_inflight_cb,
         cleanup_cb_t *cleanup_cb)
{
    // 1. 如果等待队列为空，尝试递增 inflight（非阻塞）
    if (!waitqueue_active(&rqw->wait) && acquire_inflight_cb(rqw, private_data))
        return;

    // 2. 否则，加入等待队列，进入 TASK_UNINTERRUPTIBLE 睡眠
    init_wait_func(&data.wq, rq_qos_wake_function);
    prepare_to_wait_exclusive(&rqw->wait, &data.wq, TASK_UNINTERRUPTIBLE);

    // 3. 再次尝试获取（避免竞态）
    if (acquire_inflight_cb(rqw, private_data)) {
        finish_wait(&rqw->wait, &data.wq);
        return;
    }

    // 4. 真正睡眠，等待 waker 唤醒
    io_schedule();
}
```

**工作流程**：

```
进程尝试获取 inflight 槽位
  │
  ├─ 槽位可用 → 递增 inflight → 直接返回（不发请求 → 不睡眠）
  │
  └─ 槽位不足 → 加入等待队列 → 睡眠
       │
       └─ 其他请求完成 → wbt_rqw_done() / iocg_wake_fn()
            └─ wake_up_all(&rqw->wait) → 唤醒等待进程
                 └─ 重试 acquire_inflight_cb → 成功则返回
```

---

### 9.2 blk-throttle.c — 带宽节流（1,849 行）

#### 9.2.1 概述

文件：`block/blk-throttle.c` + `block/blk-throttle.h`

实现基于 cgroup 的块 I/O 带宽限制，通过 **令牌桶（Token Bucket）** 算法限制 BPS（字节/秒）和 IOPS（操作/秒）。支持读写分离的带宽限制，并支持 cgroup 层级传递。

#### 9.2.2 关键数据结构

```c
// block/blk-throttle.h

// throtl_qnode — 按来源分组的 bio 队列节点
struct throtl_qnode {
    struct list_head node;           // 链接到 service_queue->queued[]
    struct bio_list  bios_bps;       // 等待 BPS 限流的 bio
    struct bio_list  bios_iops;      // 等待 IOPS 限流的 bio
    struct throtl_grp *tg;           // 所属 throtl_grp
};

// throtl_service_queue — 服务队列（层级式调度基本单元）
struct throtl_service_queue {
    struct throtl_service_queue *parent_sq;  // 父服务队列（向上传递）
    struct list_head queued[2];              // throtl_qnode 链表 [READ/WRITE]
    unsigned int nr_queued_bps[2];           // BPS 队列中的 bio 数量
    unsigned int nr_queued_iops[2];          // IOPS 队列中的 bio 数量
    struct rb_root_cached pending_tree;      // 活跃子 tg 的红黑树（按 disptime 排序）
    unsigned int nr_pending;                 // 等待调度的 tg 数量
    unsigned long first_pending_disptime;    // 最早调度时间
    struct timer_list pending_timer;         // 调度定时器
};

// throtl_grp — 每个 cgroup 的限流组
struct throtl_grp {
    struct blkg_policy_data pd;              // cgroup 策略数据基类
    struct rb_node rb_node;                  // 在 pending_tree 中的节点
    struct throtl_data *td;                  // 所属 throtl_data
    struct throtl_service_queue service_queue; // 本组的服务队列

    // 自队列和父队列的 qnode
    struct throtl_qnode qnode_on_self[2];    // 本地 bio 入队用
    struct throtl_qnode qnode_on_parent[2];  // 向上传递用

    unsigned long disptime;                  // 下次可调度时间（jiffies）
    unsigned int flags;                      // 状态标志
    bool has_rules_bps[2];                   // 是否设置了 BPS 规则
    bool has_rules_iops[2];                  // 是否设置了 IOPS 规则
    uint64_t bps[2];                         // BPS 限制 [READ/WRITE]
    unsigned int iops[2];                    // IOPS 限制 [READ/WRITE]

    // 统计：当前切片内的已消费量
    uint64_t bytes_disp[2];                  // 已消费字节
    unsigned int io_disp[2];                 // 已消费 IO 次数
    unsigned long slice_start[2];            // 当前切片开始时间
    unsigned long slice_end[2];              // 当前切片结束时间
};

// throtl_data — 每个 request_queue 的限流数据
struct throtl_data {
    struct throtl_service_queue service_queue; // 顶层服务队列（根）
    struct request_queue *queue;               // 反向指向 request_queue
    unsigned int nr_queued[2];                 // 总排队 bio 数
    struct work_struct dispatch_work;          // 派发工作（kthrotld 工作队列）
};
```

#### 9.2.3 层级调度架构

throtl 实现了**自底向上**的层级调度：

```
                    ┌──────────────────────┐
                    │  throtl_data         │
                    │  service_queue (根)  │ ← 顶层调度
                    └──────────┬───────────┘
                               │ parent_sq
              ┌────────────────┼─────────────────┐
              │                │                  │
    ┌─────────▼──────┐  ┌─────▼────────┐  ┌─────▼────────┐
    │ throtl_grp A   │  │ throtl_grp B │  │ throtl_grp C │
    │ bps=100MB/s    │  │ iops=10000    │  │ (无限制)     │
    │ iops=5000      │  │              │  │              │
    │ service_queue  │  │ service_queue│  │ service_queue│
    └───────┬────────┘  └──────────────┘  └──────────────┘
            │ parent_sq
    ┌───────▼────────┐
    │ throtl_grp A1  │
    │ bps=50MB/s     │
    │ service_queue  │
    └────────────────┘
```

**层级传递规则**：
1. bio 进入 `throtl_grp` 的 `service_queue`，检查是否在限流范围内
2. 如果在范围内 → 消费配额 → 向上传递到父 `service_queue`
3. 如果超出范围 → 排队等待（定时器到期后重试）
4. 到达顶层 `throtl_data->service_queue` → 通过 `kthrotld` 工作队列发起到 `submit_bio_noacct_nocheck()`

#### 9.2.4 令牌桶算法

```
时间切片 = DFL_THROTL_SLICE = HZ/10 = 100ms (HZ=1000)

每个切片预算：
  BPS 预算 = bps_limit * DFL_THROTL_SLICE / HZ
  IOPS 预算 = iops_limit * DFL_THROTL_SLICE / HZ

判断是否超限：
  tg_within_bps_limit(tg, bio, bps_limit):
    bytes_disp + bio_size <= bytes_per_slice ? 通过 : 等待

  tg_within_iops_limit(tg, bio, iops_limit):
    io_disp + 1 <= ios_per_slice ? 通过 : 等待

  tg_within_limit(tg, bio, rw):
    tg_within_bps_limit && tg_within_iops_limit ? 通过 : 等待
```

**BPS 与 IOPS 双队列分离**：

```c
// block/blk-throttle.c
static void throtl_qnode_add_bio(struct bio *bio, struct throtl_qnode *qn,
                 struct throtl_service_queue *sq)
{
    // 如果已通过 BPS 限流（BIO_BPS_THROTTLED），直接进入 IOPS 队列
    if (bio_flagged(bio, BIO_TG_BPS_THROTTLED) ||
        bio_flagged(bio, BIO_BPS_THROTTLED)) {
        bio_list_add(&qn->bios_iops, bio);     // IOPS 队列
        sq->nr_queued_iops[rw]++;
    } else {
        bio_list_add(&qn->bios_bps, bio);       // BPS 队列
        sq->nr_queued_bps[rw]++;
    }
}
```

#### 9.2.5 核心函数调用栈

```
submit_bio_noacct()
  └─ blk_throtl_bio(bio)                     // blk-throttle.h: inline
       └─ __blk_throtl_bio(bio)              // blk-throttle.c
            ├─ while (true):
            │    ├─ tg_within_limit(tg, bio, rw)  // 在限流范围内？
            │    │    ├─ 是 → throtl_charge_iops_bio()  // 消耗 IOPS 配额
            │    │    │     └─ 向上传递：qn = &tg->qnode_on_parent[rw]
            │    │    │         sq = sq->parent_sq
            │    │    │         tg = sq_to_tg(sq)
            │    │    │         continue       // 继续检查父组
            │    │    │
            │    │    └─ 否 → bio_issue_as_root_blkg()?  // 优先级反转保护
            │    │         ├─ 是 → throtl_charge_bps_bio()  // 直接消费（记账）
            │    │         │     └─ 向上传递
            │    │         └─ 否 → break       // 超出限流，需要排队
            │    │
            │    └─ 到达顶层(tg==NULL) → bio_set_flag(BIO_BPS_THROTTLED)
            │         └─ goto out_unlock      // 直接下发
            │
            ├─ throtl_add_bio_tg(bio, qn, tg)  // 加入限流队列
            │    └─ throtl_qnode_add_bio()      // 加入 BPS 或 IOPS 队列
            │    └─ throtl_enqueue_tg(tg)       // 将 tg 加入 pending_tree
            │         └─ rb_add(&tg->rb_node, &sq->pending_tree, ...)
            │
            └─ throtl_schedule_next_dispatch()  // 调度定时器
                 └─ mod_timer(&sq->pending_timer, disptime)

定时器到期 → throtl_pending_timer_fn()
  └─ throtl_select_dispatch(sq)                // 选择可调度的 tg
       └─ tg_dispatch_one_bio(tg, rw)          // 从 tg 弹出一个 bio
            ├─ throtl_pop_queued(sq, ...)       // 弹出 bio
            ├─ throtl_charge_bps_bio(tg, bio)   // 消耗 BPS 配额
            │     └─ tg->bytes_disp[rw] += bio_size
            ├─ 向上传递到父 tg
            │    └─ throtl_add_bio_tg(bio, &tg->qnode_on_parent[rw], parent_tg)
            └─ 到达顶层 → bio_list_add(&bio_list_on_stack, bio)

kthrotld 工作队列 → blk_throtl_dispatch_work_fn()
  └─ 遍历 bio_list_on_stack
       └─ submit_bio_noacct_nocheck(bio, false)  // 最终下发
```

#### 9.2.6 用户接口

```bash
# 通过 cgroup v2 接口设置
echo "8:0 rbps=10485760 wbps=20971520 riops=1000 wiops=500" > \
    /sys/fs/cgroup/<group>/io.max

# 参数格式：<major>:<minor> rbps=<bytes> wbps=<bytes> riops=<num> wiops=<num>
# 使用 "max" 表示无限制
```

---

### 9.3 blk-iolatency.c — 延迟控制（1,068 行）

#### 9.3.1 概述

文件：`block/blk-iolatency.c`

基于 cgroup 的 I/O 延迟目标控制。通过监控每个 cgroup 的 I/O 完成延迟，自动调整该 cgroup 的并发请求深度，以将延迟控制在目标范围内。

**与 WBT 的区别**：
- 基于 **bio** 而非 request，延迟覆盖整个块层 + 设备时间
- 使用 **均值延迟**（100ms 窗口），而非最小延迟
- 支持 **cgroup 层级结构**，每个节点独立控制

#### 9.3.2 关键数据结构

```c
// blk_iolatency — 每个 request_queue 的延迟控制数据
struct blk_iolatency {
    struct rq_qos rqos;                  // rq_qos 基类
    struct timer_list timer;             // 定时器（100ms 周期统计）
    struct work_struct enable_work;      // 启用工作
    u64 cur_lat;                         // 当前延迟目标
    bool enabled;                        // 是否启用
};

// iolatency_grp — 每个 cgroup 的延迟控制组
struct iolatency_grp {
    struct blkg_policy_data pd;          // cgroup 策略数据
    struct rq_wait rq_wait;             // 等待队列（限流用）
    atomic_t child_inc_inflight;         // 子组 inflight 计数
    struct blk_iolatency *blkiolat;      // 所属 blk_iolatency

    u64 min_lat_nsec;                    // 延迟目标（纳秒）
    u64 cur_win_nsec;                    // 当前窗口大小

    atomic_t scale_cookie;               // 缩放控制
    unsigned int max_depth;              // 当前最大深度

    // 统计
    struct blk_rq_stat *stats;           // per-CPU 统计
    int stats_array_size;                // 统计数组大小
};
```

#### 9.3.3 两种限流机制

```
1) 队列深度限流（Queue Depth Throttling）
   ┌────────────────────────────────────────────┐
   │ max_depth 从 UINT_MAX 开始                  │
   │   延迟超过目标 → scale_down → max_depth /= 2│
   │   延迟低于目标 → scale_up   → max_depth *= 2│
   │   最小 = 1                                  │
   └────────────────────────────────────────────┘

2) 诱导延迟限流（Induced Delay Throttling）
   ┌────────────────────────────────────────────┐
   │ 用于优先级反转场景（REQ_META / REQ_SWAP）    │
   │ 当 max_depth 已 = 1 仍需降速时：             │
   │   total_time += min_lat_nsec - actual_lat   │
   │   throttle_time = min(total_time, NSEC_PER_SEC) │
   │   在用户态返回时强制延迟（blkcg_schedule_throttle）│
   └────────────────────────────────────────────┘
```

#### 9.3.4 延迟统计与缩放

```
100ms 定时器周期执行：
  ┌──────────────────────────────────────────────┐
  │ blkcg_iolatency_timer_fn()                   │
  │   ├─ 遍历所有活跃的 iolatency_grp             │
  │   ├─ 计算均值延迟：mean = sum / nr_samples    │
  │   ├─ 比较 mean vs min_lat_nsec               │
  │   │    ├─ mean > target → scale_down()       │
  │   │    │    └─ max_depth = max(max_depth/2, 1)│
  │   │    └─ mean < target → scale_up()         │
  │   │         └─ max_depth = min(max_depth*2, UINT_MAX)│
  │   └─ 更新 rq_wait 的限流阈值                  │
  └──────────────────────────────────────────────┘
```

#### 9.3.5 用户接口

```bash
# 设置延迟目标（单位：us）
echo "target=10000" > /sys/fs/cgroup/<group>/io.latency
# 目标 = 10000us = 10ms
```

---

### 9.4 blk-iocost.c — IO 成本模型（3,551 行）

#### 9.4.1 概述

文件：`block/blk-iocost.c`

块层最大最复杂的 QoS 策略（3,551 行），基于 **IO 成本模型** 实现按比例分配设备 I/O 时间。核心思想是：**将不同的 I/O 操作量化为设备时间成本，按照 cgroup 权重比例分配**。

#### 9.4.2 成本模型

```
线性成本模型：

  单次 IO 的成本 = 基础成本（seq/rand） + 大小相关成本

  成本单位：VTIME_PER_SEC（虚拟时间，每秒固定值）

  参数通过 /sys/fs/cgroup/io.cost.model 配置：
    ctrl=user model=linear bps=<max_Bps> seqiops=<max_seq> randiops=<max_rand>

  计算方式（calc_lcoefs）：
    page_cost   = 1s / (bps / 4096)          # 每页成本
    seqio_cost  = max(1s / seqiops - page_cost, 0)  # 顺序 IO 基础成本
    randio_cost = max(1s / randiops - page_cost, 0) # 随机 IO 基础成本
```

#### 9.4.3 关键数据结构

```c
// ioc — 每个 request_queue 的 IO 成本控制器
struct ioc {
    struct rq_qos rqos;                 // rq_qos 基类
    struct blkcg_gq *root_iocg;         // 根 cgroup 的 iocg
    struct ioc_params params;           // 成本模型参数
    struct ioc_now now;                 // 当前时间

    bool enabled;                       // 是否启用
    bool running;                       // 是否运行中
    atomic64_t vtime_rate;             // 当前 vtime 速率（核心控制变量）
    s64 vtime_err;                      // vtime 累计误差

    u64 period_at;                      // 当前周期开始时间
    u64 period_us;                      // 周期长度（默认 100ms）
    u64 vtime_base_rate;                // 基准 vtime 速率
    spinlock_t lock;
    struct timer_list timer;            // 周期定时器
    struct iocg_pcpu_stat __percpu *pcpu_stat; // per-CPU 统计
};

// ioc_gq — 每个 cgroup 的 IO 成本数据
struct ioc_gq {
    struct blkg_policy_data pd;          // cgroup 策略数据
    struct ioc *ioc;                     // 所属 ioc

    u32 weight;                          // 权重（有效值）
    u32 active;                          // 活跃权重
    u32 inuse;                           // 实际使用权重（捐赠调整后）

    atomic64_t vtime;                    // 本组 vtime 游标
    atomic64_t done_vtime;               // 已完成 IO 的 vtime
    u64 abs_vdebt;                       // 绝对 vtime 债务

    struct wait_queue_head waitq;        // 等待队列（超预算时阻塞）
    u64 delay;                           // 当前延迟
    sector_t cursor;                     // 上次访问的扇区（检测随机 IO）
    struct iocg_stat last_stat;          // 上次统计

    // 激活相关
    bool activated;                      // 是否已激活
    u64 activated_at;                    // 激活时间
};

// ioc_cgrp — 每个 cgroup 的配置
struct ioc_cgrp {
    struct blkcg_policy_data cpd;
    unsigned int dfl_weight;             // 默认权重
};
```

#### 9.4.4 三部分控制策略

**1) Vtime 分配 — 按权重比例分配**

```
           root (weight=100)
         /                \
    A (weight=100)      B (weight=300)    ← B 闲置时
    /       \
A0 (w=100)  A1 (w=100)                    ← 各 50%

B 开始活跃后：
  B 份额 = 300/(100+300) = 75%
  A0+A1 各 = (100/200) * 25% = 12.5%

hweight（层级权重）：
  A0 的 hweight = 100/100 * 100/200 = 0.5  → 50% of A
  A 的 hweight  = 100/400 = 0.25            → 25% of root
  A0 的全局 hweight = 0.5 * 0.25 = 0.125   → 12.5%

vtime 运行速度与 hweight 成反比：
  A0 的 vtime 速度 = 全局 vtime / hweight
  设备 10ms 的 IO → A0 看来 = 10ms / 0.125 = 80ms
```

**2) Vrate 调整 — 自适应设备速率**

```
vrate = 设备 vtime 相对于真实时间的速率

设备饱和信号：
  ├─ rq_wait：硬件/软件队列满 → 请求等待 → 降低 vrate
  └─ 完成延迟：N% 分位延迟超过设定点 → 降低 vrate

vrate 调整：
  ioc_refresh_vrate():
    vcomp = -vtime_err / pleft           # 误差补偿
    vtime_rate = vtime_base_rate + vcomp  # 新速率
    vtime_err = clamp(vtime_err, -vperiod, vperiod)  # 限制误差累积
```

**3) Work Conservation — 工作守恒**

```
当 cgroup 未用完其份额时，捐赠给其他 cgroup：

  A 只用 10% 容量，B 需要更多：
    A 的 inuse 权重从 100 降低 → 接近实际使用
    B 的有效份额增加 → 充分利用设备

  捐赠机制（propagate_weights）：
    - 使用 inuse（实际使用权重）而非 active（配置权重）
    - 快速回弹：当 A 需要更多时，立即恢复 inuse
    - 实现细节：Andy's method（见 iocost 源码注释引用）
```

#### 9.4.5 核心函数调用栈

```
bio 提交 → ioc_rqos_throttle()
  ├─ calc_vtime_cost(bio, iocg)           # 计算成本
  ├─ iocg_activate(iocg, &now)            # 激活 iocg（首次使用时）
  ├─ adjust_inuse_and_calc_cost()         # 调整 inuse 权重
  ├─ 预算充足 → iocg_commit_bio()         # 直接下发
  │    └─ atomic64_add(cost, &iocg->vtime)  # 记录 vtime
  │
  └─ 预算不足：
       ├─ 优先级反转 → 记入债务（abs_vdebt）
       └─ 正常情况 → 加入等待队列
            ├─ __add_wait_queue_entry_tail(&iocg->waitq, &wait.wait)
            ├─ iocg_kick_waitq()           # 调度定时器
            └─ io_schedule()               # 睡眠等待

定时器 → ioc_timer_fn()
  ├─ ioc_refresh_vrate()                  # 调整 vrate
  ├─ 遍历所有 iocg
  │    ├─ transfer-weights: 捐赠/回收
  │    └─ iocg_kick_waitq()              # 唤醒可下发的组
  └─ 重新调度定时器

IO 完成 → ioc_rqos_done()
  ├─ 更新 surpluses（捐赠计算）
  └─ 触发 iocg_kick_waitq() 的可能

IO 完成 → ioc_rqos_done_bio()
  └─ atomic64_add(cost, &iocg->done_vtime)  # 记录完成 vtime
```

#### 9.4.6 用户接口

```bash
# 配置成本模型
echo "ctrl=user model=linear bps=2000000000 seqiops=300000 randiops=100000" \
    > /sys/fs/cgroup/io.cost.model

# 配置延迟 QoS
echo "rl=99:50000 rpct=0 wpct=0 min=1000000 max=2000000" \
    > /sys/fs/cgroup/io.cost.qos

# 配置权重
echo "8:0 weight=100" > /sys/fs/cgroup/<group>/io.weight

# 监控（使用 drgn 脚本）
# tools/cgroup/iocost_monitor.py
```

---

### 9.5 blk-wbt.c — 写回节流（1,025 行）

#### 9.5.1 概述

文件：`block/blk-wbt.c`

Writeback Throttling（WBT），基于 **CoDel（Controlled Delay）** 算法思想，通过监控读请求的完成延迟来限制缓冲写（buffered write）的速率，防止写请求堆积导致读延迟飙升。

#### 9.5.2 核心数据结构

```c
// rq_wb — 每个 request_queue 的 WBT 数据
struct rq_wb {
    unsigned int wb_background;          // 后台写限制（最低）
    unsigned int wb_normal;              // 正常写限制
    short enable_state;                  // 启用状态

    unsigned int unknown_cnt;            // 不确定周期计数
    u64 win_nsec;                        // 默认窗口（100ms）
    u64 cur_win_nsec;                    // 当前窗口（缩放后）

    struct blk_stat_callback *cb;        // 统计回调

    u64 sync_issue;                      // 最近同步读下发时间
    void *sync_cookie;                   // 对应 request 指针

    unsigned long last_issue;            // 最近读下发时间
    unsigned long last_comp;             // 最近读完成时间
    unsigned long min_lat_nsec;          // 延迟目标（默认 2ms SSD / 75ms HDD）

    struct rq_qos rqos;                  // rq_qos 基类
    struct rq_wait rq_wait[WBT_NUM_RWQ]; // 每个读写类型一个等待队列
    struct rq_depth rq_depth;            // 深度控制
};

// 等待队列类型
enum {
    WBT_RWQ_BG,       // 后台写
    WBT_RWQ_NORMAL,   // 正常写
    WBT_NUM_RWQ,      // 数量
};
```

#### 9.5.3 算法流程

```
WBT 算法（基于 CoDel）：

  1. 监控窗口：100ms
  2. 在每个窗口内，统计所有读请求的完成延迟
  3. 如果窗口内读的"最小延迟"超过目标（2ms SSD）：
     → 判定为延迟违规
     → scale_step++（缩小深度）
     → 下一窗口大小 = 100ms / sqrt(scale_step + 1)
  4. 如果窗口内延迟正常：
     → scale_step--（或放大深度）
     → 返回默认窗口大小
  5. 如果只有写没有读：
     → 允许 scale_step 为负（临时提升写性能）
     → 但一旦有读请求出现，立即回弹
```

**深度缩放关系**：

```
scale_step > 0（延迟超标，缩小）：
  depth = 1 + (default_depth - 1) >> scale_step
  例：default_depth=16, scale_step=1 → depth=1+15/2=8

scale_step < 0（只有写，放大）：
  depth = 1 + (default_depth - 1) << -scale_step
  max_depth = min(depth, 3/4 * queue_depth)

scale_step = 0（正常状态）：
  depth = min(default_depth, queue_depth)
```

#### 9.5.4 限流阈值

```c
// 计算不同写类型的限流阈值
static unsigned int get_limit(struct rq_wb *rwb, blk_opf_t opf)
{
    if (REQ_OP_DISCARD)
        return rwb->wb_background;          // 最低优先级

    if (REQ_HIPRIO || wb_recent_wait(rwb))
        return rwb->rq_depth.max_depth;      // 高优先级 = 最大深度

    if (REQ_BACKGROUND || close_io(rwb))
        return rwb->wb_background;           // 后台写 = 最低

    return rwb->wb_normal;                   // 正常写 = 中等
}
```

#### 9.5.5 核心函数调用栈

```
bio 提交 → wbt_wait()
  ├─ bio_to_wbt_flags() → 判断是否为 WBT_TRACKED 类型
  │    ├─ REQ_OP_WRITE 且非 DIRECT → WBT_TRACKED
  │    ├─ REQ_OP_DISCARD → WBT_TRACKED
  │    └─ 读请求 → WBT_READ（仅记录时间戳）
  │
  ├─ __wbt_wait(rwb, flags, opf)
  │    └─ rq_qos_wait(rqw, &data, wbt_inflight_cb, wbt_cleanup_cb)
  │         ├─ wbt_inflight_cb: 检查 inflight < get_limit()
  │         │    └─ rq_wait_inc_below(rqw, limit) → 原子递增
  │         └─ 失败 → 睡眠等待
  │
  └─ rwb_arm_timer(rwb) → 启动统计定时器

request 完成 → wbt_done()
  ├─ WBT_TRACKED → __wbt_done() → wbt_rqw_done()
  │    └─ atomic_dec(&rqw->inflight)
  │    └─ 如果 inflight 降到阈值以下 → wake_up_all(&rqw->wait)
  │
  └─ WBT_READ → 记录完成时间戳 → wb_timestamp(&rwb->last_comp)

定时器到期 → wb_timer_fn()
  └─ latency_exceeded(rwb, cb->stat) → 返回 LAT_OK / LAT_EXCEEDED / LAT_UNKNOWN
       ├─ LAT_EXCEEDED → scale_down(rwb, true)    # 硬节流
       ├─ LAT_OK       → scale_up(rwb)             # 恢复
       └─ LAT_UNKNOWN  → 增加 unknown_cnt 或缓慢回中
```

#### 9.5.6 用户接口

```bash
# 通过 sysfs 查看/设置
cat /sys/block/<dev>/queue/wbt_lat_usec          # 查看延迟目标
echo 2000 > /sys/block/<dev>/queue/wbt_lat_usec  # 设置 2ms

# 通过 debugfs 查看内部状态
cat /sys/kernel/debug/block/<dev>/rqos/inflight
cat /sys/kernel/debug/block/<dev>/rqos/min_lat_nsec
```

---

### 9.6 blk-ioprio.c — I/O 优先级（179 行）

#### 9.6.1 概述

文件：`block/blk-ioprio.c`

最简单的 rq_qos 策略，基于 cgroup 为 bio 设置 I/O 优先级类，影响底层调度器（如 mq-deadline）和驱动对请求的处理顺序。

#### 9.6.2 策略枚举

```c
enum prio_policy {
    POLICY_NO_CHANGE    = 0,   // 默认：不修改优先级
    POLICY_PROMOTE_TO_RT = 1,  // 提升到 RT 类
    POLICY_RESTRICT_TO_BE = 2, // 限制到 BE 类
    POLICY_ALL_TO_IDLE  = 3,   // 全部降为 IDLE
    POLICY_NONE_TO_RT   = 4,   // NONE 提升到 RT
};
```

#### 9.6.3 调用路径

```
submit_bio()
  └─ bio_set_ioprio(bio)
       ├─ 设置基于 task nice 的默认优先级
       └─ blkcg_set_ioprio(bio)   ← blk-ioprio 钩子
            └─ 根据 cgroup 策略修改 bio->bi_ioprio
                 ├─ PROMOTE_TO_RT: 非 RT → IOPRIO_CLASS_RT | level=4
                 ├─ RESTRICT_TO_BE: RT/NONE → IOPRIO_CLASS_BE
                 ├─ ALL_TO_IDLE: → IOPRIO_CLASS_IDLE
                 └─ NO_CHANGE: 不做修改
```

#### 9.6.4 用户接口

```bash
echo "restrict-to-be" > /sys/fs/cgroup/<group>/io.prio.class
```

---

### 9.7 QoS 策略对比总结

| 策略 | 文件 | 行数 | 控制目标 | 算法 | 粒度 | 用户接口 |
|------|------|------|----------|------|------|----------|
| **blk-throttle** | blk-throttle.c | 1,849 | BPS / IOPS | 令牌桶 | cgroup 层级 | `io.max` |
| **blk-iolatency** | blk-iolatency.c | 1,068 | 延迟目标 | 均值延迟 + 深度缩放 | cgroup 层级 | `io.latency` |
| **blk-iocost** | blk-iocost.c | 3,551 | 权重比例 | 成本模型 + vtime | cgroup 层级 | `io.weight` + `io.cost.model` |
| **blk-wbt** | blk-wbt.c | 1,025 | 读延迟保护 | CoDel 变种 | device 级别 | `wbt_lat_usec` |
| **blk-ioprio** | blk-ioprio.c | 179 | 优先级标记 | 策略映射 | cgroup 层级 | `io.prio.class` |

---

## 10. Cgroup 集成

### 10.1 概述

块层 cgroup 集成（`blkcg`）是 Linux 块 I/O 控制系统的基础设施，为所有基于 cgroup 的 QoS 策略（throttle、iolatency、iocost、ioprio）提供统一的框架。核心文件 `block/blk-cgroup.c`（2,250 行）和 `block/blk-cgroup.h`（503 行）实现了：

- **blkcg 子系统**：注册为 `io` cgroup 子系统（`io_cgrp_subsys`）
- **blkg 管理**：per-cgroup per-device 的关联对象生命周期
- **I/O 统计**：per-CPU 统计收集与层级聚合
- **策略框架**：策略注册、激活、去激活
- **诱导延迟**：cgroup 级别的延迟注入机制

### 10.2 核心数据结构

#### 10.2.1 `struct blkcg` — 块 I/O cgroup

```c
// block/blk-cgroup.h
struct blkcg {
    struct cgroup_subsys_state css;              // cgroup 子系统状态（嵌入 cgroup 核心）
    spinlock_t lock;                             // 保护 blkg_tree 和 blkg_list
    refcount_t online_pin;                       // 在线引脚计数（延迟销毁）
    atomic_t congestion_count;                   // 本 cgroup 的拥塞计数

    struct radix_tree_root blkg_tree;            // radix 树：按 queue->id 索引 blkg
    struct blkcg_gq __rcu *blkg_hint;            // 最近访问的 blkg 缓存（快速路径）
    struct hlist_head blkg_list;                 // 所有 blkg 的哈希链表

    struct blkcg_policy_data *cpd[BLKCG_MAX_POLS]; // 各策略的 per-cgroup 数据
    struct list_head all_blkcgs_node;            // 全局 all_blkcgs 链表节点

    struct llist_head __percpu *lhead;           // per-CPU 锁释放列表（统计加速）
#ifdef CONFIG_BLK_CGROUP_FC_APPID
    char fc_app_id[FC_APPID_LEN];                // FC 应用标识符
#endif
#ifdef CONFIG_CGROUP_WRITEBACK
    struct list_head cgwb_list;                  // cgroup writeback 链表
#endif
};
```

#### 10.2.2 `struct blkcg_gq` — blkg（核心关联对象）

```c
// block/blk-cgroup.h
struct blkcg_gq {
    /* 关联关系 */
    struct request_queue *q;                     // 所属 request_queue
    struct blkcg *blkcg;                         // 所属 blkcg
    struct blkcg_gq *parent;                     // 父 blkg（cgroup 层级，根为 NULL）

    /* 链表节点 */
    struct list_head q_node;                     // 链接到 request_queue->blkg_list
    struct hlist_node blkcg_node;                // 链接到 blkcg->blkg_list

    /* 生命周期 */
    struct percpu_ref refcnt;                    // 引用计数（percpu 优化）
    bool online;                                 // 是否在线

    /* I/O 统计 */
    struct blkg_iostat_set __percpu *iostat_cpu; // per-CPU 统计（无锁热点路径）
    struct blkg_iostat_set iostat;               // 聚合统计（全局快照）

    /* 策略数据指针 */
    struct blkg_policy_data *pd[BLKCG_MAX_POLS]; // 各策略的 per-device 数据

#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
    spinlock_t async_bio_lock;                   // 异步 bio 锁
    struct bio_list async_bios;                  // 异步 bio 队列
#endif
    union {
        struct work_struct async_bio_work;       // 异步 bio 提交工作
        struct work_struct free_work;            // 释放工作
    };

    /* 诱导延迟 */
    atomic_t use_delay;                          // 是否使用诱导延迟（>0 = 启用）
    atomic64_t delay_nsec;                       // 累计延迟纳秒数
    atomic64_t delay_start;                      // 延迟开始时间
    u64 last_delay;                              // 上次延迟值
    int last_use;                                // 上次 use_delay 值

    struct rcu_head rcu_head;                    // RCU 回调
};
```

#### 10.2.3 `struct blkg_iostat_set` — per-CPU 统计单元

```c
// block/blk-cgroup.h
enum blkg_iostat_type {
    BLKG_IOSTAT_READ,       // 读
    BLKG_IOSTAT_WRITE,      // 写
    BLKG_IOSTAT_DISCARD,    // 丢弃
    BLKG_IOSTAT_NR,         // 总数
};

struct blkg_iostat {
    u64 bytes[BLKG_IOSTAT_NR];  // 字节数
    u64 ios[BLKG_IOSTAT_NR];    // I/O 次数
};

struct blkg_iostat_set {
    struct u64_stats_sync sync;    // 64 位原子性同步
    struct blkcg_gq *blkg;        // 所属 blkg
    struct llist_node lnode;      // 锁释放链表节点（无锁统计队列）
    int lqueued;                  // 是否已在锁释放链表中
    struct blkg_iostat cur;       // 当前统计值
    struct blkg_iostat last;      // 上次快照值（用于计算 delta）
};
```

#### 10.2.4 `struct blkg_policy_data` / `struct blkcg_policy_data` — 策略数据基类

```c
// block/blk-cgroup.h
// per-blkg per-policy 数据（嵌入各策略的私有数据）
struct blkg_policy_data {
    struct blkcg_gq *blkg;    // 所属 blkg
    int plid;                 // 策略 ID
    bool online;              // 是否在线
};

// per-blkcg per-policy 数据（嵌入各策略的 cgroup 级私有数据）
struct blkcg_policy_data {
    struct blkcg *blkcg;      // 所属 blkcg
    int plid;                 // 策略 ID
};
```

**典型嵌入示例**：
```
struct throtl_grp {                // throttle 策略的 per-blkg 数据
    struct blkg_policy_data pd;    // ← 基类必须位于开头
    ...                            // 私有字段
};

struct ioc_cgrp {                  // iocost 策略的 per-blkcg 数据
    struct blkcg_policy_data cpd;  // ← 基类
    unsigned int dfl_weight;
};
```

#### 10.2.5 `struct blkcg_policy` — 策略注册描述符

```c
// block/blk-cgroup.h
struct blkcg_policy {
    int plid;                                    // 策略 ID（0~BLKCG_MAX_POLS-1）
    struct cftype *dfl_cftypes;                  // cgroup v2 文件接口
    struct cftype *legacy_cftypes;               // cgroup v1 文件接口（blkio.*）

    /* per-blkcg 回调（cgroup 级） */
    blkcg_pol_alloc_cpd_fn  *cpd_alloc_fn;       // 分配 per-blkcg 数据
    blkcg_pol_free_cpd_fn   *cpd_free_fn;         // 释放 per-blkcg 数据

    /* per-blkg 回调（device 级） */
    blkcg_pol_alloc_pd_fn   *pd_alloc_fn;         // 分配 per-blkg 数据
    blkcg_pol_init_pd_fn    *pd_init_fn;          // 初始化
    blkcg_pol_online_pd_fn  *pd_online_fn;        // 上线
    blkcg_pol_offline_pd_fn *pd_offline_fn;       // 下线
    blkcg_pol_free_pd_fn    *pd_free_fn;          // 释放
    blkcg_pol_reset_pd_stats_fn *pd_reset_stats_fn; // 重置统计
    blkcg_pol_stat_pd_fn    *pd_stat_fn;          // 输出统计到 seq_file
};
```

### 10.3 Bio 与 Cgroup 的关联流程

每个 bio 在提交时都需要关联到正确的 cgroup，这是块层 cgroup 控制的入口。

#### 10.3.1 关联入口

```
bio 分配路径（bio_alloc → bio_alloc_bioset）：
  ┌─────────────────────────────────────────────────────┐
  │ bio_alloc_bioset()                                   │
  │   └─ bio_init()                                      │
  │        └─ if (blkcg_css()):                          │
  │             └─ bio_associate_blkg(bio)   ← 关联 blkg  │
  └─────────────────────────────────────────────────────┘

bio 复用路径（bio_init → bio_init_fields）：
  bio.c:bio_init_fields()
    └─ bio_associate_blkg(bio)
```

#### 10.3.2 `bio_associate_blkg()` — 完整关联

```c
// block/blk-cgroup.c
void bio_associate_blkg(struct bio *bio)
{
    struct cgroup_subsys_state *css;

    /* passthrough 请求不关联 cgroup */
    if (blk_op_is_passthrough(bio->bi_opf))
        return;

    rcu_read_lock();

    /* 获取当前任务的 blkcg css */
    if (bio->bi_blkg)
        css = bio_blkcg_css(bio);   // 已有关联 → 复用 css
    else
        css = blkcg_css();          // 首次关联 → 获取当前任务 css

    bio_associate_blkg_from_css(bio, css);

    rcu_read_unlock();
}
```

#### 10.3.3 `blkcg_css()` — 获取当前任务的 blkcg

```c
// block/blk-cgroup.c
static struct cgroup_subsys_state *blkcg_css(void)
{
    struct cgroup_subsys_state *css;

    css = kthread_blkcg();          // 内核线程有专属 blkcg？
    if (css)
        return css;
    return task_css(current, io_cgrp_id);  // 否则取当前任务的 io cgroup
}
```

#### 10.3.4 `bio_associate_blkg_from_css()` — CSS 到 blkg 的查找/创建

```c
// block/blk-cgroup.c
void bio_associate_blkg_from_css(struct bio *bio,
                 struct cgroup_subsys_state *css)
{
    if (bio->bi_blkg)
        blkg_put(bio->bi_blkg);       // 释放旧的 blkg 引用

    if (css && css->parent) {
        // 非 root cgroup → 查找或创建 blkg，失败时向上回溯到最近的活跃 blkg
        bio->bi_blkg = blkg_tryget_closest(bio, css);
    } else {
        // root cgroup → 直接取 root_blkg
        blkg_get(bdev_get_queue(bio->bi_bdev)->root_blkg);
        bio->bi_blkg = bdev_get_queue(bio->bi_bdev)->root_blkg;
    }
}
```

**`blkg_tryget_closest()` 向上回溯机制**：

```
bio_associate_blkg_from_css(bio, css)
  └─ blkg_tryget_closest(bio, css)       // block/blk-cgroup.c
       ├─ blkg = blkg_lookup_create(css->blkcg, disk)  // 查找或创建
       │    ├─ 找到 → 返回
       │    └─ 创建失败（cgroup 正在销毁）→ 返回 NULL
       │
       └─ while (blkg):
            ├─ blkg_tryget(blkg) 成功？→ 返回 blkg
            └─ 失败 → blkg = blkg->parent  // 向上回溯到父 blkg
                 └─ 继续尝试
```

**关键设计**：当 cgroup 正在销毁时，blkg 的 `percpu_ref` 已变为原子模式，`blkg_tryget()` 可能失败。此时自动向上回溯到父 cgroup 的 blkg，确保 bio 始终能关联到一个有效的 blkg。

#### 10.3.5 `blkg_lookup_create()` — 递归创建 blkg

```
blkg_lookup_create(blkcg, disk)           // block/blk-cgroup.c
  ├─ blkg_lookup(blkcg, q)                // 1. 先尝试查找
  │    └─ radix_tree_lookup(&blkcg->blkg_tree, q->id)
  │
  ├─ 找到？→ 返回
  │
  └─ 未找到 → 持有 queue_lock 后：
       └─ while (true):
            ├─ 从 blkcg 向上遍历到 blkcg_root
            │  找到第一个已创建 blkg 的祖先节点 pos
            │
            ├─ blkg_create(pos, disk, NULL)  // 创建 pos→disk 的 blkg
            │    ├─ 分配 blkcg_gq
            │    ├─ 调用各策略的 pd_alloc_fn()
            │    ├─ 链接到父 blkg
            │    ├─ 调用 pd_init_fn() + pd_online_fn()
            │    └─ 插入 radix_tree + blkg_list
            │
            └─ pos == blkcg？→ 创建完毕，返回
```

### 10.4 blkg 生命周期管理

#### 10.4.1 `blkg_alloc()` — blkg 分配完整流程

```c
// block/blk-cgroup.c
static struct blkcg_gq *blkg_alloc(struct blkcg *blkcg, struct gendisk *disk,
                                   gfp_t gfp_mask)
{
    struct blkcg_gq *blkg;
    int i, cpu;

    /* Step 1: 分配 blkg 本体（NUMA 亲和） */
    blkg = kzalloc_node(sizeof(*blkg), gfp_mask, disk->queue->node);
    if (!blkg)
        return NULL;

    /* Step 2: 初始化 percpu 引用计数（释放时调用 blkg_release） */
    if (percpu_ref_init(&blkg->refcnt, blkg_release, 0, gfp_mask))
        goto out_free_blkg;

    /* Step 3: 分配 per-CPU 统计数组 */
    blkg->iostat_cpu = alloc_percpu_gfp(struct blkg_iostat_set, gfp_mask);
    if (!blkg->iostat_cpu)
        goto out_exit_refcnt;

    /* Step 4: 增加 request_queue 引用（防止 queue 先于 blkg 释放） */
    if (!blk_get_queue(disk->queue))
        goto out_free_iostat;

    /* Step 5: 初始化基础字段 */
    blkg->q = disk->queue;
    INIT_LIST_HEAD(&blkg->q_node);
    blkg->blkcg = blkcg;
    blkg->iostat.blkg = blkg;

    /* Step 6: 初始化 per-CPU 统计的 u64_stats_sync */
    u64_stats_init(&blkg->iostat.sync);
    for_each_possible_cpu(cpu) {
        u64_stats_init(&per_cpu_ptr(blkg->iostat_cpu, cpu)->sync);
        per_cpu_ptr(blkg->iostat_cpu, cpu)->blkg = blkg;
    }

    /* Step 7: 为每个已激活的策略分配 per-blkg 数据 */
    for (i = 0; i < BLKCG_MAX_POLS; i++) {
        struct blkcg_policy *pol = blkcg_policy[i];
        struct blkg_policy_data *pd;

        if (!blkcg_policy_enabled(disk->queue, pol))
            continue;

        pd = pol->pd_alloc_fn(disk, blkcg, gfp_mask);
        if (!pd)
            goto out_free_pds;
        blkg->pd[i] = pd;
        pd->blkg = blkg;
        pd->plid = i;
        pd->online = false;
    }

    return blkg;

    /* 错误处理：逆序释放已分配的资源 */
out_free_pds:
    while (--i >= 0)
        if (blkg->pd[i])
            blkcg_policy[i]->pd_free_fn(blkg->pd[i]);
    blk_put_queue(disk->queue);
out_free_iostat:
    free_percpu(blkg->iostat_cpu);
out_exit_refcnt:
    percpu_ref_exit(&blkg->refcnt);
out_free_blkg:
    kfree(blkg);
    return NULL;
}
```

#### 10.4.2 创建：从 `device_add_disk` 到 `blkcg_init_disk`

```
device_add_disk(disk)
  └─ blkcg_init_disk(disk)                     // block/blk-cgroup.c
       ├─ blkg_alloc(&blkcg_root, disk, GFP_KERNEL)  // 分配 root blkg（见 10.4.1）
       │
       └─ blkg_create(&blkcg_root, disk, new_blkg)   // 创建 root blkg
            ├─ 检查 blk_queue_dying() → 拒绝创建
            ├─ css_tryget_online(&blkcg->css)         // 持有 blkcg 引用
            ├─ 链接父 blkg（root 无父）
            ├─ 调用各策略的 pd_init_fn()
            ├─ spin_lock(&blkcg->lock)
            │    ├─ radix_tree_insert(&blkcg->blkg_tree, q->id, blkg)
            │    ├─ hlist_add_head_rcu(&blkg->blkcg_node, &blkcg->blkg_list)
            │    ├─ list_add(&blkg->q_node, &q->blkg_list)
            │    ├─ 调用各策略的 pd_online_fn()，设置 pd->online = true
            │    └─ blkg->online = true
            └─ spin_unlock(&blkcg->lock)
       q->root_blkg = blkg
```

#### 10.4.3 销毁：从 `del_gendisk` 到 `blkcg_exit_disk`

```
del_gendisk(disk)
  └─ blkcg_exit_disk(disk)                     // block/blk-cgroup.c
       └─ blkg_destroy_all(disk)               // 销毁所有 blkg
            ├─ 遍历 request_queue->blkg_list
            │    └─ blkg_destroy(blkg)         // 逐个销毁
            │         ├─ 调用各策略的 pd_offline_fn()
            │         ├─ blkg->online = false
            │         ├─ radix_tree_delete()
            │         ├─ hlist_del_init_rcu()
            │         └─ percpu_ref_kill(&blkg->refcnt)  // 触发释放
            │
            ├─ __clear_bit(pol->plid, q->blkcg_pols)     // 标记策略已去激活
            └─ q->root_blkg = NULL

  └─ blk_throtl_exit(disk)                     // 清理 throttle 特有数据
```

**blkg 释放的异步链**：

```
percpu_ref_kill(&blkg->refcnt)
  └─ 所有引用释放后 → blkg_release(ref)
       └─ call_rcu(&blkg->rcu_head, __blkg_release)  // RCU 宽限期后
            └─ __blkg_release()
                 ├─ 对所有 CPU 调用 __blkcg_rstat_flush()  // 刷新残留统计
                 ├─ css_put(&blkg->blkcg->css)             // 释放 blkcg 引用
                 └─ blkg_free(blkg)
                      └─ schedule_work(&blkg->free_work)   // 异步释放
                           └─ blkg_free_workfn()
                                ├─ pd_free_fn() for each policy
                                ├─ blkg_put(parent)
                                ├─ list_del_init(&blkg->q_node)
                                ├─ blk_put_queue(q)
                                ├─ free_percpu(iostat_cpu)
                                ├─ percpu_ref_exit(&blkg->refcnt)
                                └─ kfree(blkg)
```

#### 10.4.4 blkcg 销毁三阶段

```
Stage 1: blkcg_css_offline()
  ├─ wb_blkcg_offline(css)     // 下线 writeback，等待 cgwb 完成
  └─ blkcg_unpin_online(css)   // 释放 online_pin
       └─ online_pin == 0 → blkcg_destroy_blkgs(blkcg)
            ├─ 遍历 blkcg->blkg_list
            └─ 对每个 blkg 调用 blkg_destroy()

Stage 2: blkcg_destroy_blkgs()
  └─ 释放所有 blkg，percpu_ref_kill 触发异步释放
       blkg 释放后 → css_put(&blkcg->css) → blkcg 引用归零

Stage 3: blkcg_css_free()
  ├─ 调用各策略的 cpd_free_fn()
  ├─ free_percpu(blkcg->lhead)
  └─ kfree(blkcg)
```

#### 10.4.5 blkcg 的 CSS 生命周期（cgroup 核心回调）

blkcg 作为 cgroup 子系统，通过 `io_cgrp_subsys` 注册了完整的 CSS 生命周期回调：

```
blkcg_css_alloc(parent_css)             // block/blk-cgroup.c
  ├─ root cgroup？→ 使用静态 blkcg_root
  │   └─ 非 root → kzalloc(new blkcg)
  │
  ├─ init_blkcg_llists(blkcg)           // 分配 per-CPU lhead（锁释放链表）
  │
  ├─ for (i = 0; i < BLKCG_MAX_POLS; i++)  // 为每个已注册策略分配 cpd
  │    └─ pol->cpd_alloc_fn(GFP_KERNEL)
  │
  └─ return &blkcg->css

blkcg_css_online(css)                   // block/blk-cgroup.c
  └─ if (parent):
       └─ blkcg_pin_online(&parent->css)  // pin 父 blkcg，保证销毁有序
            └─ refcount_inc(&parent->online_pin)

blkcg_css_offline(css)                  // block/blk-cgroup.c
  ├─ wb_blkcg_offline(css)               // 下线 cgroup writeback
  └─ blkcg_unpin_online(css)             // 释放 online_pin
       └─ do {
            ├─ refcount_dec_and_test(&blkcg->online_pin) == 0? → break
            ├─ parent = blkcg_parent(blkcg)
            ├─ blkcg_destroy_blkgs(blkcg)  // 销毁所有 blkg
            └─ blkcg = parent
          } while (blkcg)               // 级联向上：子销毁后，父的
                                         // online_pin 可能归零，继续销毁

blkcg_css_free(css)                     // block/blk-cgroup.c
  ├─ list_del(&blkcg->all_blkcgs_node)  // 从全局链表移除
  ├─ for (i = 0; i < BLKCG_MAX_POLS; i++)  // 释放各策略的 cpd
  │    └─ pol->cpd_free_fn(blkcg->cpd[i])
  ├─ free_percpu(blkcg->lhead)
  └─ kfree(blkcg)
```

**`online_pin` 级联机制**：

```
创建时：blkcg_css_alloc()
  ├─ blkcg_root.online_pin = 1
  │
  └─ 子 cgroup → blkcg_css_online()
       └─ blkcg_pin_online(parent)  → parent.online_pin++

销毁时：blkcg_css_offline()
  └─ blkcg_unpin_online()           → blkcg.online_pin--
       └─ online_pin == 0？
            ├─ 销毁自己的 blkg
            └─ 继续 unpin parent → parent.online_pin--
                 └─ parent.online_pin == 0？
                      └─ 继续向上...

关键设计：
- 子 cgroup 上线时 pin 父 cgroup，保证父不会先于子销毁
- 子 cgroup 下线时 unpin 父，只有所有子都下线后父的
  online_pin 才归零，触发父的 blkg 销毁
```

### 10.5 I/O 统计收集与聚合

#### 10.5.1 `blk_cgroup_io_type()` — Bio I/O 类型映射

```c
// block/blk-cgroup.c
static int blk_cgroup_io_type(struct bio *bio)
{
    if (op_is_discard(bio->bi_opf))
        return BLKG_IOSTAT_DISCARD;   // 丢弃操作
    if (op_is_write(bio->bi_opf))
        return BLKG_IOSTAT_WRITE;     // 写操作
    return BLKG_IOSTAT_READ;          // 读操作
}
```

**统计类型枚举**：
```c
enum blkg_iostat_type {
    BLKG_IOSTAT_READ,       // 0: 读
    BLKG_IOSTAT_WRITE,      // 1: 写
    BLKG_IOSTAT_DISCARD,    // 2: 丢弃（TRIM/DISCARD）
    BLKG_IOSTAT_NR,         // 3: 类型总数
};
```

#### 10.5.2 统计更新路径：`blk_cgroup_bio_start()`

```
submit_bio_noacct_nocheck(bio)          // block/blk-core.c
  └─ blk_cgroup_bio_start(bio)          // block/blk-cgroup.c
       │
       ├─ blkcg = bio->bi_blkg->blkcg   // 获取 blkcg
       ├─ rwd = blk_cgroup_io_type(bio) // 确定方向：READ/WRITE/DISCARD
       │
       ├─ cpu = get_cpu()
       ├─ bis = per_cpu_ptr(blkg->iostat_cpu, cpu)  // per-CPU 统计
       │
       ├─ u64_stats_update_begin(&bis->sync)
       │    ├─ 未标记 BIO_CGROUP_ACCT？→ bis->cur.bytes[rwd] += bio->bi_iter.bi_size
       │    │     bio_set_flag(bio, BIO_CGROUP_ACCT)  // 防止 split bio 重复记账
       │    └─ bis->cur.ios[rwd]++                    // 增加 I/O 计数
       │
       ├─ if (!bis->lqueued):                          // 首次更新？→ 加入锁释放链表
       │    ├─ lhead = this_cpu_ptr(blkcg->lhead)
       │    ├─ llist_add(&bis->lnode, lhead)           // 无锁入队
       │    └─ WRITE_ONCE(bis->lqueued, true)
       │
       └─ css_rstat_updated(&blkcg->css, cpu)          // 通知 rstat 系统
```

**关键设计**：
- **per-CPU 统计**：避免多核竞争，每个 CPU 独立计数
- **锁释放链表（llist）**：`blk_cgroup_bio_start()` 是热点路径，使用无锁 `llist_add()` 记录已更新的 `blkg_iostat_set`，避免在 flush 时遍历所有 blkg
- **`BIO_CGROUP_ACCT` 标志**：bio split 后只对原始大小记账一次，子 bio 跳过

#### 10.5.3 统计聚合路径：`__blkcg_rstat_flush()`

```
rstat 周期刷新（cgroup 后台）→ blkcg_rstat_flush(css, cpu)
  └─ __blkcg_rstat_flush(blkcg, cpu)     // block/blk-cgroup.c
       │
       ├─ lhead = per_cpu_ptr(blkcg->lhead, cpu)
       ├─ lnode = llist_del_all(lhead)    // 取出所有待刷新的条目
       │
       └─ 遍历 llist：
            for_each (bisc, lnode):
              ├─ WRITE_ONCE(bisc->lqueued, false)
              │
              ├─ if (bisc == &blkg->iostat):  // 这是父 blkg 的传播标记？
              │    └─ goto propagate_up       // 直接上传
              │
              ├─ 读取 per-CPU 值（u64_stats_fetch 保证原子性）
              │    cur = bisc->cur
              │
              └─ blkcg_iostat_update(blkg, &cur, &bisc->last)
                   ├─ delta = cur - last       // 计算增量
                   ├─ blkg->iostat.cur += delta  // 累加到全局统计
                   └─ bisc->last = cur          // 更新快照

              propagate_up:
              └─ if (parent && parent->parent):  // 非 root → 向上传播
                   ├─ blkcg_iostat_update(parent, &blkg->iostat.cur, &blkg->iostat.last)
                   │    // parent 的 iostat 增加本 blkg 的增量
                   │
                   └─ if (!parent->iostat.lqueued):
                        ├─ llist_add(&parent->iostat.lnode, parent->lhead)
                        └─ parent->iostat.lqueued = true
                        // 标记 parent 需要继续向上传播
```

**层级传播示意**：

```
                    ┌───────────────┐
                    │  root blkcg   │  ← 统计来自全局 disk_stats
                    │  (无 parent)   │
                    └───────┬───────┘
                            │ parent
                    ┌───────▼───────┐
                    │  blkcg A      │  ← 聚合 B + C 的统计
                    │  iostat.cur   │
                    └───┬───────┬───┘
                        │       │ parent
              ┌─────────▼─┐  ┌──▼──────────┐
              │ blkcg B   │  │ blkcg C     │  ← per-CPU 统计来源
              │ iostat_cpu│  │ iostat_cpu  │
              └───────────┘  └─────────────┘
```

#### 10.5.4 Root cgroup 统计的特殊处理

```c
// block/blk-cgroup.c
static void blkcg_fill_root_iostats(void)
{
    // Root cgroup 不从 per-CPU 统计聚合，而是直接从全局 disk_stats 读取
    for_each_disk:
        blkg = disk->queue->root_blkg
        for_each_cpu:
            tmp.ios[READ]  += cpu_dkstats->ios[STAT_READ]
            tmp.bytes[READ] += cpu_dkstats->sectors[STAT_READ] << 9
            // 类似地处理 WRITE / DISCARD
        blkg->iostat.cur = tmp
}
```

**设计原因**：当没有 cgroup 配置时，避免无谓的双重统计开销。

#### 10.5.5 用户空间读取：`io.stat`

```
cat /sys/fs/cgroup/<group>/io.stat
  └─ blkcg_print_stat()
       ├─ root cgroup → blkcg_fill_root_iostats()
       └─ 非 root → css_rstat_flush()  // 先刷新再读取
       └─ 遍历 blkcg->blkg_list:
            └─ blkcg_print_one_stat(blkg, sf)
                 ├─ 读取 blkg->iostat.cur（u64_stats_fetch 保证原子性）
                 └─ 输出格式：
                      "8:0 rbytes=1048576 wbytes=2097152 rios=100 wios=200
                       dbytes=0 dios=0"
                 └─ 调用各策略的 pd_stat_fn() 输出策略特有统计
```

### 10.6 策略注册与激活

#### 10.6.1 `blkcg_policy_register()` — 策略注册完整实现

```c
// block/blk-cgroup.c
int blkcg_policy_register(struct blkcg_policy *pol)
{
    struct blkcg *blkcg;
    int i, ret;

    /* 校验：cpd/pd 的 alloc 和 free 必须成对出现 */
    if ((!pol->cpd_alloc_fn ^ !pol->cpd_free_fn) ||
        (!pol->pd_alloc_fn ^ !pol->pd_free_fn))
        return -EINVAL;

    mutex_lock(&blkcg_pol_register_mutex);
    mutex_lock(&blkcg_pol_mutex);

    /* Step 1: 寻找空闲的 plid 槽位 */
    for (i = 0; i < BLKCG_MAX_POLS; i++)
        if (!blkcg_policy[i])
            break;
    if (i >= BLKCG_MAX_POLS) {
        ret = -ENOSPC;
        goto err_unlock;
    }

    /* Step 2: 注册策略到全局数组 */
    pol->plid = i;
    blkcg_policy[pol->plid] = pol;

    /* Step 3: 为所有已存在的 blkcg 分配 per-cgroup 数据 (cpd) */
    if (pol->cpd_alloc_fn) {
        list_for_each_entry(blkcg, &all_blkcgs, all_blkcgs_node) {
            struct blkcg_policy_data *cpd;

            cpd = pol->cpd_alloc_fn(GFP_KERNEL);
            if (!cpd) {
                ret = -ENOMEM;
                goto err_free_cpds;     // 回滚已分配的 cpd
            }
            blkcg->cpd[pol->plid] = cpd;
            cpd->blkcg = blkcg;
            cpd->plid = pol->plid;
        }
    }

    mutex_unlock(&blkcg_pol_mutex);

    /* Step 4: 注册 cgroup 文件接口（v2 和/或 v1 legacy） */
    if (pol->dfl_cftypes == pol->legacy_cftypes)
        cgroup_add_cftypes(&io_cgrp_subsys, pol->dfl_cftypes);
    else {
        cgroup_add_dfl_cftypes(&io_cgrp_subsys, pol->dfl_cftypes);
        cgroup_add_legacy_cftypes(&io_cgrp_subsys, pol->legacy_cftypes);
    }

    mutex_unlock(&blkcg_pol_register_mutex);
    return 0;

    /* 错误处理：释放所有已分配的 cpd，清空槽位 */
err_free_cpds:
    if (pol->cpd_free_fn)
        blkcg_free_all_cpd(pol);
    blkcg_policy[pol->plid] = NULL;
err_unlock:
    mutex_unlock(&blkcg_pol_mutex);
    mutex_unlock(&blkcg_pol_register_mutex);
    return ret;
}
EXPORT_SYMBOL_GPL(blkcg_policy_register);
```

**各策略注册调用点**：
```c
// 各策略模块初始化时调用
blkcg_policy_register(&blkcg_policy_throtl);     // plid=0, block/blk-throttle.c
blkcg_policy_register(&blkcg_policy_iolatency);  // plid=1, block/blk-iolatency.c
blkcg_policy_register(&blkcg_policy_iocost);     // plid=2, block/blk-iocost.c
blkcg_policy_register(&ioprio_policy);           // plid=3, block/blk-ioprio.c
```

#### 10.6.2 `blkcg_policy_unregister()` — 策略注销

```c
// block/blk-cgroup.c
void blkcg_policy_unregister(struct blkcg_policy *pol)
{
    mutex_lock(&blkcg_pol_register_mutex);

    /* Step 1: 移除 cgroup 文件接口 */
    if (pol->dfl_cftypes)
        cgroup_rm_cftypes(pol->dfl_cftypes);
    if (pol->legacy_cftypes)
        cgroup_rm_cftypes(pol->legacy_cftypes);

    /* Step 2: 释放所有 blkcg 的 cpd */
    mutex_lock(&blkcg_pol_mutex);
    if (pol->cpd_free_fn)
        blkcg_free_all_cpd(pol);

    /* Step 3: 清空全局槽位 */
    blkcg_policy[pol->plid] = NULL;
    mutex_unlock(&blkcg_pol_mutex);

out_unlock:
    mutex_unlock(&blkcg_pol_register_mutex);
}
EXPORT_SYMBOL_GPL(blkcg_policy_unregister);
```

#### 10.6.3 `blkcg_activate_policy()` — 策略激活完整实现

策略注册后不会自动生效，需要由设备初始化时激活。激活过程**冻结队列**以确保无 I/O 飞行，然后为所有已存在的 blkg 分配 per-blkg 策略数据。

```c
// block/blk-cgroup.c
int blkcg_activate_policy(struct gendisk *disk, const struct blkcg_policy *pol)
{
    struct request_queue *q = disk->queue;
    struct blkg_policy_data *pd_prealloc = NULL;
    struct blkcg_gq *blkg, *pinned_blkg = NULL;
    int ret;

    if (blkcg_policy_enabled(q, pol))
        return 0;                      // 已激活，直接返回

    if (WARN_ON_ONCE(!pol->pd_alloc_fn || !pol->pd_free_fn))
        return -EINVAL;                // 无 pd 函数的策略无需激活（如 ioprio）

    if (queue_is_mq(q))
        memflags = blk_mq_freeze_queue(q);  // 冻结队列，阻止新 I/O 进入

retry:
    spin_lock_irq(&q->queue_lock);

    /* 逆序遍历 blkg_list（先初始化父 blkg，再初始化子 blkg） */
    list_for_each_entry_reverse(blkg, &q->blkg_list, q_node) {
        struct blkg_policy_data *pd;

        if (blkg->pd[pol->plid])
            continue;                  // 已分配，跳过

        /* 使用预分配或 GFP_NOWAIT 即时分配 */
        if (blkg == pinned_blkg) {
            pd = pd_prealloc;          // 使用上次 GFP_KERNEL 预分配的 pd
            pd_prealloc = NULL;
        } else {
            pd = pol->pd_alloc_fn(disk, blkg->blkcg, GFP_NOWAIT);
        }

        if (!pd) {
            /* GFP_NOWAIT 分配失败 → 记录当前 blkg，释放锁后重试 */
            if (pinned_blkg)
                blkg_put(pinned_blkg);
            blkg_get(blkg);
            pinned_blkg = blkg;         // 标记需要重试的 blkg

            spin_unlock_irq(&q->queue_lock);

            if (pd_prealloc)
                pol->pd_free_fn(pd_prealloc);
            pd_prealloc = pol->pd_alloc_fn(disk, blkg->blkcg, GFP_KERNEL);
            if (pd_prealloc)
                goto retry;            // 预分配成功，重试
            else
                goto enomem;           // 预分配也失败 → 内存不足，回滚
        }

        /* 初始化 pd 并关联到 blkg */
        spin_lock(&blkg->blkcg->lock);
        pd->blkg = blkg;
        pd->plid = pol->plid;
        blkg->pd[pol->plid] = pd;

        if (pol->pd_init_fn)
            pol->pd_init_fn(pd);
        if (pol->pd_online_fn)
            pol->pd_online_fn(pd);
        pd->online = true;
        spin_unlock(&blkg->blkcg->lock);
    }

    __set_bit(pol->plid, q->blkcg_pols);  // 标记策略已激活
    ret = 0;
    spin_unlock_irq(&q->queue_lock);
    goto out;

enomem:
    /* 内存分配失败 → 回滚：销毁已分配的所有 pd */
    spin_lock_irq(&q->queue_lock);
    list_for_each_entry(blkg, &q->blkg_list, q_node) {
        struct blkcg *blkcg = blkg->blkcg;
        struct blkg_policy_data *pd;

        spin_lock(&blkcg->lock);
        pd = blkg->pd[pol->plid];
        if (pd) {
            if (pd->online && pol->pd_offline_fn)
                pol->pd_offline_fn(pd);
            pol->pd_free_fn(pd);
            blkg->pd[pol->plid] = NULL;
        }
        spin_unlock(&blkcg->lock);
    }
    ret = -ENOMEM;
    spin_unlock_irq(&q->queue_lock);

out:
    if (queue_is_mq(q))
        blk_mq_unfreeze_queue(q, memflags);  // 解冻队列
    if (pinned_blkg)
        blkg_put(pinned_blkg);
    if (pd_prealloc)
        pol->pd_free_fn(pd_prealloc);
    return ret;
}
```

**`blkcg_policy_enabled()` 检查**：
```c
static inline bool blkcg_policy_enabled(struct request_queue *q,
                const struct blkcg_policy *pol)
{
    return pol && test_bit(pol->plid, q->blkcg_pols);
}
```

#### 10.6.4 `blkcg_deactivate_policy()` — 策略去激活

```
blkcg_deactivate_policy(disk, pol)     // block/blk-cgroup.c
  ├─ blk_mq_freeze_queue(q)            // 冻结队列（无 I/O 飞行）
  │
  ├─ __clear_bit(pol->plid, q->blkcg_pols)  // 清除激活标志
  │
  ├─ 遍历 queue->blkg_list:
  │    └─ 对每个 blkg：
  │         ├─ pol->pd_offline_fn(pd)       // 下线策略数据
  │         └─ pol->pd_free_fn(pd)          // 立即释放 pd
  │         └─ blkg->pd[plid] = NULL
  │
  └─ blk_mq_unfreeze_queue(q)           // 解冻队列
```

### 10.7 诱导延迟机制

当 QoS 策略（如 iolatency、iocost）检测到延迟超标时，可以通过诱导延迟机制在用户态返回时强制等待，而无需在内核 I/O 路径中阻塞。

```
QoS 策略检测到需要延迟：
  blkcg_schedule_throttle(disk, use_memdelay)   // block/blk-cgroup.c
    ├─ current->throttle_disk = disk              // 记录需要节流的磁盘
    └─ set_notify_resume(current)                // 设置 TIF_NOTIFY_RESUME 标志
         │
         └─ 进程返回用户态时（ret_to_user）：
              └─ blkcg_maybe_throttle_current()   // block/blk-cgroup.c
                   ├─ blkg = blkg_lookup(blkcg, disk->queue)
                   ├─ blkcg_maybe_throttle_blkg(blkg, use_memdelay)
                   │    └─ 读取 blkg->delay_nsec
                   │    └─ 如果 delay > 0:
                   │         ├─ 如果 use_memdelay → psi_memstall_enter()
                   │         └─ usleep_range(delay/1000, delay/1000 * 2)
                   │         └─ atomic64_set(&blkg->delay_nsec, 0)
                   │
                   └─ put_disk(disk)
```

**延迟累积**：
```
QoS 策略调用：
  blkcg_add_delay(blkg, now, delta)    // block/blk-cgroup.c
    └─ blkcg_scale_delay(blkg, now)     // 指数衰减历史延迟
    └─ atomic64_add(delta, &blkg->delay_nsec)  // 累加新延迟

使用场景：
  blk-iolatency: 诱导延迟限流（max_depth = 1 时）
  blk-iocost:    超预算 cgroup 的延迟惩罚
```

### 10.8 完整 Bio I/O 路径中的 Cgroup 集成

```
submit_bio(bio)
  │
  ├─ bio_set_ioprio(bio)                ← blk-ioprio: 设置 cgroup 优先级
  │
  └─ submit_bio_noacct(bio)
       │
       ├─ blk_throtl_bio(bio)           ← blk-throttle: cgroup 带宽限流
       │    (可能排队等待，参见 9.2 节)
       │
       └─ submit_bio_noacct_nocheck(bio)
            │
            ├─ blk_cgroup_bio_start(bio)  ← 统计记账（per-CPU 累加）
            │
            ├─ __submit_bio_noacct_mq(bio)
            │    └─ blk_mq_submit_bio(bio)
            │         ├─ rq_qos_throttle(bio)  ← WBT/iolatency/iocost
            │         │    (可能通过 blkcg_schedule_throttle 注入延迟)
            │         ├─ __blk_mq_alloc_request()
            │         │    └─ rq_qos_track()   ← 关联 bio→request
            │         └─ blk_mq_start_request()
            │              └─ rq_qos_issue()   ← 通知策略下发
            │
            └─ I/O 完成：
                 └─ blk_mq_free_request()
                      └─ rq_qos_done()         ← 通知策略完成
                           └─ 可能触发 blkcg_schedule_throttle()
```

### 10.9 Cgroup 写回集成（CONFIG_CGROUP_WRITEBACK）

```c
// block/blk-cgroup.h
struct blkcg {
    ...
#ifdef CONFIG_CGROUP_WRITEBACK
    struct list_head cgwb_list;    // 本 cgroup 的 writeback 设备链表
#endif
};
```

当启用了 `CONFIG_CGROUP_WRITEBACK` 时，blkcg 与内存 cgroup（memcg）协同工作：

- **依赖关系**：`io_cgrp_subsys.depends_on = 1 << memory_cgrp_id`，确保 memcg 自动启用
- **作用**：脏页回写（writeback）可以关联到正确的 cgroup，使得回写 I/O 受到对应 cgroup 的 QoS 限制
- **生命周期集成**：`blkcg_css_offline()` 首先调用 `wb_blkcg_offline()` 下线 writeback，等待所有 cgwb 完成后再销毁 blkg

### 10.10 辅助 Cgroup 文件

| 文件 | 行数 | 功能 |
|------|------|------|
| blk-cgroup-rwstat.c | 124 | 读写统计辅助函数（`blkg_rwstat_read`、`blkg_prfill_rwstat`、`blkg_rwstat_recursive_sum`） |
| blk-cgroup-rwstat.h | 150 | 读写统计头文件，定义 `blkg_rwstat` 结构 |
| blk-cgroup-fc-appid.c | 57 | FC（Fibre Channel）应用 ID 管理（`blkcg_set_fc_appid`/`blkcg_get_fc_appid`） |

### 10.11 Cgroup v2 接口汇总

| 接口文件 | 所属策略 | 功能 |
|----------|----------|------|
| `io.stat` | blkcg 核心 | 显示 per-device I/O 统计（rbytes/wbytes/rios/wios/dbytes/dios） |
| `io.max` | blk-throttle | 设置 BPS/IOPS 上限（`8:0 rbps=10485760 wbps=20971520`） |
| `io.latency` | blk-iolatency | 设置延迟目标（`target=10000`，单位 us） |
| `io.weight` | blk-iocost | 设置权重比例（`8:0 weight=100`） |
| `io.cost.model` | blk-iocost | 配置成本模型参数 |
| `io.cost.qos` | blk-iocost | 配置延迟 QoS 参数 |
| `io.prio.class` | blk-ioprio | 设置优先级策略（`restrict-to-be` / `promote-to-rt` 等） |

### 10.12 `io_cgrp_subsys` — cgroup 子系统定义

blkcg 通过 `io_cgrp_subsys` 注册为 `io` cgroup 子系统，这是整个块层 cgroup 集成的入口：

```c
// block/blk-cgroup.c
struct cgroup_subsys io_cgrp_subsys = {
    .css_alloc      = blkcg_css_alloc,       // 创建 blkcg（分配 cpd、初始化字段）
    .css_online     = blkcg_css_online,      // blkcg 上线（pin 父 blkcg）
    .css_offline    = blkcg_css_offline,     // blkcg 下线（下线 writeback、销毁 blkg）
    .css_free       = blkcg_css_free,        // 释放 blkcg（释放 cpd、lhead、本体）
    .css_rstat_flush = blkcg_rstat_flush,    // 刷新统计（触发 per-CPU → 全局聚合）
    .dfl_cftypes    = blkcg_files,           // cgroup v2 文件接口（io.*）
    .legacy_cftypes = blkcg_legacy_files,    // cgroup v1 文件接口（blkio.*）
    .legacy_name    = "blkio",               // v1 兼容名称
    .exit           = blkcg_exit,            // 任务退出清理

#ifdef CONFIG_MEMCG
    .depends_on = 1 << memory_cgrp_id,       // 依赖 memcg（回写统计需要）
#endif
};
```

**`blkcg_exit()` — 任务退出时清理**：
```c
// block/blk-cgroup.c
static void blkcg_exit(struct task_struct *tsk)
{
    if (tsk->throttle_disk)
        put_disk(tsk->throttle_disk);  // 释放诱导延迟中持有的 disk 引用
    tsk->throttle_disk = NULL;
}
```

### 10.13 关键设计要点总结

| 设计点 | 机制 | 目的 |
|--------|------|------|
| **per-CPU 统计** | `blkg_iostat_set __percpu *iostat_cpu` | 避免多核统计竞争 |
| **锁释放链表** | `blkcg->lhead` + `llist_add()` | 无锁入队，减少 flush 开销 |
| **向上回溯** | `blkg_tryget_closest()` 沿 parent 链回溯 | cgroup 销毁时 bio 不丢失 |
| **诱导延迟** | `set_notify_resume()` + 用户态返回时等待 | 避免在 I/O 路径中阻塞 |
| **异步释放** | percpu_ref → RCU → workqueue 三级异步 | 避免在原子上下文中持有锁释放 |
| **per-CPU 引用计数** | `percpu_ref` 管理 blkg 生命周期 | 高性能引用计数，适合热点路径 |
| **冻结队列** | `blk_mq_freeze_queue()` 保护策略激活 | 确保无 I/O 飞行时安全修改策略数据 |
| **root cgroup 优化** | 直接从 `disk_stats` 读取，不走 per-CPU | 无 cgroup 时零额外开销 |

---