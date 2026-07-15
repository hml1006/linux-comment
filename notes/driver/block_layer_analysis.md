# Linux 7.0 块层（Block Layer）代码分析报告

## 目录

1. [总体概览](#1-总体概览)
2. [核心数据结构](#2-核心数据结构)
3. [请求队列管理（Request Queue）](#3-请求队列管理)
4. [Bio 层（Block I/O）](#4-bio-层block-io)
    - [4.1 概述](#41-概述)
    - [4.2 核心数据结构](#42-核心数据结构)
    - [4.3 Bio 分配与释放](#43-bio-分配与释放)
    - [4.4 Bio 初始化与复用](#44-bio-初始化与复用)
    - [4.5 向 Bio 添加数据](#45-向-bio-添加数据)
    - [4.6 Bio 提交路径](#46-bio-提交路径)
    - [4.7 Bio 完成路径](#47-bio-完成路径)
    - [4.8 Bio 链式机制](#48-bio-链式机制bio_chain)
    - [4.9 Bio 克隆与分割](#49-bio-克隆与分割)
    - [4.10 Bio 数据拷贝](#410-bio-数据拷贝)
    - [4.11 页面管理与直接 I/O](#411-页面管理与直接-io)
    - [4.12 Bounce Buffer 机制](#412-bounce-buffer-机制)
    - [4.13 bio_set 初始化与销毁](#413-bio_set-初始化与销毁)
    - [4.14 辅助模块](#414-辅助模块)
    - [4.15 完整调用链总结](#415-完整调用链总结)
    - [4.16 边界处理与设备保护](#416-边界处理与设备保护)
    - [4.17 其他 Bio 操作](#417-其他-bio-操作)
    - [4.18 Per-CPU 分配缓存深度解析](#418-per-cpu-分配缓存深度解析)
5. [多队列框架（blk-mq）](#5-多队列框架blk-mq)
6. [I/O 调度器](#6-io-调度器)
7. [I/O 合并与分段](#7-io-合并与分段)
8. [刷新与屏障（Flush/FUA）](#8-刷新与屏障flushfua)
9. [QoS 与资源控制](#9-qos-与资源控制)
10. [Cgroup 集成](#10-cgroup-集成)
11. [分区管理](#11-分区管理)
12. [数据完整性与加密](#12-数据完整性与加密)
13. [Zoned 块设备](#13-zoned-块设备)
14. [Sysfs 与调试接口](#14-sysfs-与调试接口)
15. [其他辅助模块](#15-其他辅助模块)
16. [总结](#16-总结)
17. [NVMe 驱动：块设备注册与移除流程](#17-nvme-驱动块设备注册与移除流程)
    - [17.1 关键数据结构](#171-关键数据结构)
    - [17.2 注册流程（Probe）](#172-注册流程probe)
    - [17.3 移除流程（Remove）](#173-移除流程remove)
    - [17.4 与块层的交互接口](#174-与块层的交互接口)
    - [17.5 关键工作队列](#175-关键工作队列)
    - [17.6 涉及的文件清单](#176-涉及的文件清单)
18. [NVMe 驱动：块设备读写 I/O 流程](#18-nvme-驱动块设备读写-io-流程)
    - [18.1 I/O 流程总览](#181-io-流程总览)
    - [18.2 关键数据结构](#182-涉及的关键数据结构)
    - [18.3 详细函数调用栈](#183-详细函数调用栈)
    - [18.4 数据拷贝路径](#184-数据拷贝路径)
    - [18.5 PRP 与 SGL 数据描述符](#185-prp-与-sgl-数据描述符)
    - [18.6 完成路径详解](#186-完成路径详解)
    - [18.7 open/release 详细流程](#187-nvme-驱动openrelease-详细流程)
    - [18.8 批量提交优化](#188-批量提交优化)
    - [18.9 涉及的文件清单](#189-涉及的文件清单)

---

## 1. 总体概览

### 1.1 文件统计

块层代码位于 `block/` 目录下，包含 **55 个 .c 源文件** 和 **16 个 .h 头文件**，以及 `block/partitions/` 子目录下的 **18 个分区识别文件**，总计约 **71,423 行代码**。

### 1.2 代码规模排名（Top 15）

| 排名 | 文件 | 行数 | 功能 |
|------|------|------|------|
| 1 | bfq-iosched.c | 7,682 | BFQ I/O 调度器实现 |
| 2 | blk-mq.c | 5,365 | 多队列核心框架 |
| 3 | blk-iocost.c | 3,551 | IO 成本模型控制器 |
| 4 | sed-opal.c | 3,351 | TCG Opal 自加密驱动 |
| 5 | blk-zoned.c | 2,363 | 分区（Zoned）块设备支持 |
| 6 | blk-cgroup.c | 2,250 | 通用块 cgroup 接口 |
| 7 | bio.c | 2,065 | Bio 结构体管理 |
| 8 | blk-throttle.c | 1,849 | 块层节流控制 |
| 9 | bfq-wf2q.c | 1,701 | BFQ WF2Q+ 算法实现 |
| 10 | genhd.c | 1,584 | 通用磁盘（gendisk）管理 |
| 11 | blk-core.c | 1,410 | 块层核心请求处理 |
| 12 | blk-merge.c | 1,171 | I/O 合并与分段 |
| 13 | blk-iolatency.c | 1,068 | IO 延迟控制 |
| 14 | blk-settings.c | 1,062 | 队列参数设置 |
| 15 | kyber-iosched.c | 1,033 | Kyber 延迟调度器 |

---

## 2. 核心数据结构

### 2.1 关键头文件

块层核心数据结构定义在内部头文件 `block/blk.h`（763行）中，主要包括：

- **`struct request_queue`** — 请求队列，是块层的核心对象，每个块设备对应一个请求队列。包含队列标志位、调度器、统计信息、QoS 钩子等。
- **`struct blk_flush_queue`** — 刷新队列，管理 PREFLUSH/FUA 请求的双缓冲序列。
- **`struct blk_mq_ctx`** — 多队列上下文，维护每个 CPU 的软件队列状态。
- **`struct blk_mq_hw_ctx`** — 硬件队列上下文，表示一个硬件派发队列。

### 2.2 队列标志位

通过 `blk_queue_flag_set()` / `blk_queue_flag_clear()` 原子操作管理队列状态，关键标志包括：

| 标志 | 含义 |
|------|------|
| `QUEUE_FLAG_DYING` | 队列正在销毁 |
| `QUEUE_FLAG_NOMERGES` | 禁止合并 |
| `QUEUE_FLAG_SAME_COMP` | 必须在同一 CPU 上完成 |
| `QUEUE_FLAG_WC` | 写缓存使能 |
| `QUEUE_FLAG_FUA` | 支持 FUA |
| `QUEUE_FLAG_DAX` | DAX 设备 |
| `QUEUE_FLAG_STATS` | 启用统计 |
| `QUEUE_FLAG_POLL` | 支持轮询 |
| `QUEUE_FLAG_REGISTERED` | 已注册到 sysfs |
| `QUEUE_FLAG_QUIESCED` | 队列已静默 |

---

## 3. 请求队列管理

### 3.1 blk-core.c（1,410 行）

文件：`block/blk-core.c`

块层核心逻辑，处理所有块设备的读写请求。主要功能：

- **队列生命周期管理**：`blk_alloc_queue()` 分配请求队列，`blk_cleanup_queue()` 清理销毁。
- **队列标志操作**：`blk_queue_flag_set()` / `blk_queue_flag_clear()` 原子设置/清除标志位。
- **请求提交入口**：`submit_bio()` 是上层（文件系统）向块层提交 I/O 的标准入口。
- **kblockd 工作队列**：`kblockd_workqueue` 用于异步处理块层工作。
- **Tracepoint 导出**：导出 `block_bio_remap`、`block_rq_remap`、`block_bio_complete`、`block_split`、`block_unplug`、`block_rq_insert` 等跟踪点。

### 3.2 blk-settings.c（1,062 行）

文件：`block/blk-settings.c`

提供驱动设置队列属性的接口：

- `blk_queue_rq_timeout()` — 设置请求超时时间。
- `blk_set_stacking_limits()` — 为堆叠设备（如 DM/MD）设置默认限制。
- `blk_queue_max_hw_sectors()` — 设置最大硬件扇区数。
- `blk_queue_logical_block_size()` — 设置逻辑块大小。
- `blk_queue_physical_block_size()` — 设置物理块大小。
- `blk_queue_io_min()` / `blk_queue_io_opt()` — 设置最优 I/O 大小。

---

## 4. Bio 层（Block I/O）

### 4.1 概述

Bio（Block I/O）是 Linux 块层的核心 I/O 单元，表示一个块 I/O 操作。它位于文件系统/VFS 与块层请求队列之间，是上层（文件系统、直接 I/O）向块层提交 I/O 的标准接口。

**Bio 在 I/O 栈中的位置**：

```
用户空间        write() / read() / io_uring
   │
   ▼
VFS / 文件系统   构造 bio，设置 bi_bdev、bi_iter.bi_sector、bi_end_io
   │
   ▼
Bio 层          submit_bio() → submit_bio_noacct() → blk_mq_submit_bio()
   │
   ▼
块层 / blk-mq   将 bio 封装为 request，插入软件队列
   │
   ▼
驱动层          nvme_queue_rq() → 向硬件提交命令
```

**Bio 与 Request 的关系**：
- Bio 是上层提交的基本单位，Request 是块层内部处理的基本单位
- 一个 Request 可以包含多个 Bio（通过合并）
- Bio 是轻量级的，Request 是重量级的（包含 tag、超时、统计等）

**核心文件**：
| 文件 | 行数 | 功能 |
|------|------|------|
| `block/bio.c` | 2,065 | Bio 分配、释放、提交、拆分、数据拷贝 |
| `include/linux/bio.h` | 748 | Bio 内联函数、宏、bio_list、bio_set 定义 |
| `include/linux/blk_types.h` | — | `struct bio` 定义、`BIO_*` 标志、`REQ_OP_*` 操作码 |

### 4.2 核心数据结构

#### 4.2.1 `struct bio` — I/O 操作容器

（[blk_types.h](file:///home/louis/code/linux/include/linux/blk_types.h#L210)）表示一个块 I/O 请求：

```c
struct bio {
    struct bio          *bi_next;       /* 链表指针，用于 bio_list 串联 */
    struct block_device *bi_bdev;       /* 目标块设备 */
    blk_opf_t           bi_opf;         /* 低 8 位 REQ_OP_*, 高 24 位 req_flags */
    unsigned short      bi_flags;       /* BIO_* 状态标志（见 4.2.4） */
    unsigned short      bi_ioprio;      /* I/O 优先级 */
    enum rw_hint        bi_write_hint;  /* 写提示（冷热数据分级） */
    u8                  bi_write_stream;/* 写流 ID */
    blk_status_t        bi_status;      /* 完成状态（BLK_STS_OK = 成功） */
    u8                  bi_bvec_gap_bit;/* 虚拟地址间隙的最低置位 bit */

    atomic_t            __bi_remaining; /* 链式 bio 的剩余计数（见 4.10） */
    struct bio_vec      *bi_io_vec;     /* bio_vec 数组指针 */
    struct bvec_iter    bi_iter;        /* 迭代器：当前扇区、剩余大小、当前索引 */
    union {
        blk_qc_t        bi_cookie;      /* 轮询请求的 cookie */
        unsigned int    __bi_nr_segments;/* 分区写入的段数 */
    };
    bio_end_io_t        *bi_end_io;     /* 完成回调函数 */
    void                *bi_private;    /* 回调私有数据 */

#ifdef CONFIG_BLK_CGROUP
    struct blkcg_gq     *bi_blkg;       /* cgroup 关联 */
    u64                 issue_time_ns;  /* 提交时间戳 */
#endif
    // ... 加密、完整性等可选字段 ...

    unsigned short      bi_vcnt;        /* 当前使用的 bio_vec 数量 */
    unsigned short      bi_max_vecs;    /* 分配的 bio_vec 数组大小 */
    atomic_t            __bi_cnt;       /* 引用计数 */
    struct bio_set      *bi_pool;       /* 所属的 bio 池 */
};
```

**字段分组说明**：
- **设备与操作**：`bi_bdev`、`bi_opf`（操作码 + 标志）、`bi_iter.bi_sector`（起始扇区）
- **数据**：`bi_io_vec[]` + `bi_vcnt` 描述数据位置，`bi_iter.bi_size` 为总字节数
- **生命周期**：`__bi_cnt`（引用计数）、`bi_pool`（归还目标）、`bi_flags`（状态）
- **完成通知**：`bi_end_io` + `bi_private` + `bi_status`

#### 4.2.2 `struct bio_vec` — 数据段描述符

（[bvec.h](file:///home/louis/code/linux/include/linux/bvec.h)）表示一个连续的物理内存段：

```c
struct bio_vec {
    struct page *bv_page;    /* 页指针 */
    unsigned int bv_len;     /* 段长度（字节） */
    unsigned int bv_offset;  /* 页内偏移（字节） */
};
```

**物理地址计算**：`page_to_phys(bv_page) + bv_offset`

```
    ┌───────────┐
    │  page 0   │  bv_page = page 0, bv_offset = 512, bv_len = 2048
    │           │
    ├───────────┤
    │  page 1   │  bv_page = page 1, bv_offset = 0,   bv_len = 4096
    │           │
    ├───────────┤
    │  page 2   │  bv_page = page 2, bv_offset = 0,   bv_len = 1024
    └───────────┘
```

#### 4.2.3 `struct bvec_iter` — 迭代器

（[bio.h](file:///home/louis/code/linux/include/linux/bio.h#L107)）用于遍历 bio 中的数据段：

```c
struct bvec_iter {
    sector_t    bi_sector;      /* 当前设备扇区号 */
    unsigned int bi_size;       /* 剩余未处理字节数 */
    unsigned int bi_idx;        /* 当前 bio_vec 索引 */
    unsigned int bi_bvec_done;  /* 当前 bio_vec 中已处理字节数 */
};
```

**遍历宏**（[bio.h](file:///home/louis/code/linux/include/linux/bio.h#L149)）：
```c
// 标准遍历（遵守 bi_iter 偏移）
#define bio_for_each_segment(bvl, bio, iter) ...

// 遍历所有 bio_vec（忽略偏移，驱动不可用）
#define bio_for_each_segment_all(bvl, bio, iter) ...

// 遍历所有 folio（多页 bio_vec 支持）
#define bio_for_each_folio_all(fi, bio) ...
```

#### 4.2.4 BIO 标志（`bi_flags`）

（[blk_types.h](file:///home/louis/code/linux/include/linux/blk_types.h#L302)）

| 标志 | 含义 |
|------|------|
| `BIO_PAGE_PINNED` | 页面已被 pin（直接 I/O），需在 `bio_release_pages()` 中 unpin |
| `BIO_CLONED` | 克隆 bio，不拥有 bio_vec 数据所有权 |
| `BIO_QUIET` | 静默模式，抑制错误日志 |
| `BIO_CHAIN` | 链式 bio，`__bi_remaining` 生效 |
| `BIO_REFFED` | 引用计数已提升（`__bi_cnt > 1`） |
| `BIO_BPS_THROTTLED` | 已通过 BPS 节流 |
| `BIO_TRACE_COMPLETION` | 完成时需 trace 最终完成事件 |
| `BIO_CGROUP_ACCT` | 已记账到 cgroup |
| `BIO_REMAPPED` | 已通过分区重映射 |
| `BIO_ZONE_WRITE_PLUGGING` | 通过 zone write plugging 处理 |
| `BIO_EMULATES_ZONE_APPEND` | 模拟 zone append 操作 |

#### 4.2.5 操作码（`bi_opf` 低 8 位）

（[blk_types.h](file:///home/louis/code/linux/include/linux/blk_types.h#L340)）

| 操作码 | 含义 |
|--------|------|
| `REQ_OP_READ` (0) | 从设备读取扇区 |
| `REQ_OP_WRITE` (1) | 向设备写入扇区 |
| `REQ_OP_FLUSH` (2) | 刷新写缓存（仅用于 request，不可通过 bio 提交） |
| `REQ_OP_DISCARD` (3) | 丢弃扇区（TRIM/UNMAP） |
| `REQ_OP_SECURE_ERASE` (5) | 安全擦除扇区 |
| `REQ_OP_ZONE_APPEND` | 在分区写指针处追加写入 |
| `REQ_OP_WRITE_ZEROES` | 写零 |
| `REQ_OP_ZONE_RESET/OPEN/CLOSE/FINISH` | 分区管理操作 |

**方向判断**：`op_is_write(bio_op(bio))` — 最低位为 1 表示写入。

#### 4.2.6 `struct bio_set` — Bio 池

（[bio.h](file:///home/louis/code/linux/include/linux/bio.h#L686)）管理 bio 和 bio_vec 的内存池：

```c
struct bio_set {
    struct kmem_cache       *bio_slab;          /* bio 的 slab 缓存 */
    unsigned int            front_pad;          /* bio 前的填充字节（嵌入用） */
    struct bio_alloc_cache __percpu *cache;     /* per-CPU 分配缓存 */
    mempool_t               bio_pool;           /* bio 内存池（保证进程） */
    mempool_t               bvec_pool;          /* bio_vec 内存池 */
    unsigned int            back_pad;           /* bio 后的填充字节 */
    spinlock_t              rescue_lock;        /* 救援链表锁 */
    struct bio_list         rescue_list;        /* 等待救援的 bio 链表 */
    struct work_struct      rescue_work;        /* 救援工作队列项 */
    struct workqueue_struct *rescue_workqueue;  /* 救援工作队列 */
    struct hlist_node       cpuhp_dead;         /* CPU 热插拔通知 */
};
```

**front_pad 的作用**：
```
┌─────────────────────┐
│   struct dm_io      │ ← front_pad 区域（DM/MD 驱动使用）
├─────────────────────┤
│   struct bio        │ ← bio 指针指向这里
├─────────────────────┤
│   bio_vec[]         │
└─────────────────────┘
```
上层驱动（如 DM、MD）在 bio 前嵌入私有数据，使用 `bio_alloc_bioset()` 分配，`mempool_alloc()` 返回的实际地址是 `bio - front_pad`。

**全局 bio 池**：`fs_bio_set` — 文件系统通用 I/O 使用的默认 bio 池。

#### 4.2.7 `struct bio_list` — Bio 链表

（[bio.h](file:///home/louis/code/linux/include/linux/bio.h#L545)）用于 bio 的批量管理：

```c
struct bio_list {
    struct bio *head;   /* 链表头 */
    struct bio *tail;   /* 链表尾（快速追加入队） */
};
```

**关键操作**：
- `bio_list_add(bl, bio)` — 追加到尾部（O(1)）
- `bio_list_add_head(bl, bio)` — 插入到头部
- `bio_list_pop(bl)` — 从头部弹出（O(1)）
- `bio_list_merge(bl, bl2)` — 合并两个链表（O(1)）

### 4.3 Bio 分配与释放

#### 4.3.1 分配路径

```c
// 最常用：从 fs_bio_set 分配
struct bio *bio_alloc(struct block_device *bdev, unsigned short nr_vecs,
                      blk_opf_t opf, gfp_t gfp_mask);

// 从指定 bio_set 分配（DM/MD 等堆叠驱动使用）
struct bio *bio_alloc_bioset(struct block_device *bdev, unsigned short nr_vecs,
                             blk_opf_t opf, gfp_t gfp_mask, struct bio_set *bs);
```

**分配流程**（[bio.c](file:///home/louis/code/linux/block/bio.c#L551) `bio_alloc_bioset`）：

```
bio_alloc_bioset(bdev, nr_vecs, opf, gfp, bs)
  │
  ├─ [1] 若 bs->cache 存在且 nr_vecs ≤ BIO_INLINE_VECS
  │      → bio_alloc_percpu_cache() 尝试从 per-CPU 缓存获取
  │        ├─ 从 cache->free_list 取出（命中）
  │        └─ 若 free_list 为空，将 free_list_irq 搬入 free_list
  │
  ├─ [2] 若缓存未命中，从 mempool 分配
  │      → mempool_alloc(&bs->bio_pool, gfp_mask)
  │        ├─ 若失败且当前在 submit_bio_noacct 上下文中
  │        │   → punt_bios_to_rescuer(bs) 将等待的 bio 交给救援线程
  │        │   → 重试分配
  │        └─ 若仍失败 → 返回 NULL
  │
  ├─ [3] bio = p + bs->front_pad（跳过嵌入区域）
  │
  ├─ [4] 分配 bio_vec 数组
  │      ├─ nr_vecs > BIO_INLINE_VECS → bvec_alloc(&bs->bvec_pool)
  │      │   └─ 根据数量从 slab 分配（16/64/128/256 四个级别）
  │      └─ nr_vecs ≤ BIO_INLINE_VECS → 使用内联 bio_vec
  │
  └─ [5] bio_init(bio, bdev, bvl, nr_vecs, opf) 初始化字段
```

**per-CPU 缓存机制**（[bio.c](file:///home/louis/code/linux/block/bio.c#L38)）：
- `bio_alloc_cache`：每 CPU 一个缓存实例
- `free_list`：进程上下文释放的 bio 链表
- `free_list_irq`：中断上下文释放的 bio 链表（需要关中断访问）
- 阈值 `ALLOC_CACHE_THRESHOLD=16`：当 `free_list_irq` 积累超过 16 个时搬入 `free_list`
- 最大 `ALLOC_CACHE_MAX=256`：超过则直接释放回 mempool

**bio_vec slab 分级**（[bio.c](file:///home/louis/code/linux/block/bio.c#L46)）：

| 级别 | nr_vecs 范围 | slab 名称 |
|------|-------------|-----------|
| 0 | 16 | biovec-16 |
| 1 | 64 | biovec-64 |
| 2 | 128 | biovec-128 |
| 3 | 256（BIO_MAX_VECS） | biovec-max |

#### 4.3.2 释放路径

```c
void bio_put(struct bio *bio);
```

**释放流程**（[bio.c](file:///home/louis/code/linux/block/bio.c#L869)）：

```
bio_put(bio)
  │
  ├─ [1] 若 BIO_REFFED（引用计数>1）
  │      → atomic_dec_and_test(&__bi_cnt)
  │        └─ 若不为 0 → 直接返回（还有引用者）
  │
  ├─ [2] 若 REQ_ALLOC_CACHE 标志置位
  │      → bio_put_percpu_cache(bio)
  │        ├─ in_task() → 放入 free_list
  │        ├─ in_hardirq() → 放入 free_list_irq
  │        └─ 超过 ALLOC_CACHE_MAX → bio_free(bio)
  │
  └─ [3] 否则 → bio_free(bio)
       ├─ bio_uninit(bio)  // 释放 cgroup/加密/完整性
       ├─ bvec_free()      // 释放 bio_vec 数组
       └─ mempool_free()   // 释放 bio 回 mempool
```

### 4.4 Bio 初始化与复用

**新分配**（[bio.c](file:///home/louis/code/linux/block/bio.c#L254)）：
```c
void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,
              unsigned short max_vecs, blk_opf_t opf);
```
- 清零所有字段，设置 `__bi_cnt=1`、`__bi_remaining=1`
- 设置 `bi_bdev`、`bi_opf`、`bi_io_vec`、`bi_max_vecs`

**重置**（[bio.c](file:///home/louis/code/linux/block/bio.c#L308)）：
```c
void bio_reset(struct bio *bio, struct block_device *bdev, blk_opf_t opf);
```
- 保留 `bi_io_vec` 指针，清零 `BIO_RESET_BYTES` 之前的字段
- 用于 bio 池中取出的 bio 重新初始化

**复用**（[bio.c](file:///home/louis/code/linux/block/bio.c#L338)）：
```c
void bio_reuse(struct bio *bio, blk_opf_t opf);
```
- 保留 `bi_io_vec` 数据、`bi_end_io`、`bi_private`
- 重新计算 `bi_iter.bi_size`（遍历所有 bio_vec 求和）
- 典型场景：读取数据后直接写入另一个位置

### 4.5 向 Bio 添加数据

#### 4.5.1 `bio_add_page` — 添加页

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1082)）尝试将一页数据添加到 bio：

```c
int bio_add_page(struct bio *bio, struct page *page,
                 unsigned int len, unsigned int offset);
```

**流程**：
```
bio_add_page(bio, page, len, offset)
  │
  ├─ [1] 检查 BIO_CLONED（克隆 bio 不可添加数据）
  ├─ [2] 检查总大小是否超过 BIO_MAX_SIZE
  │
  ├─ [3] 若 bio->bi_vcnt > 0，尝试合并到最后一个 bio_vec
  │      → bvec_try_merge_page(last_bv, page, len, offset)
  │        ├─ 检查物理地址是否连续：vec_end_addr + 1 == page_addr + offset
  │        ├─ Xen 检查 xen_biovec_phys_mergeable()
  │        └─ 页面边界检查（跨页但同一物理连续区域）
  │      → 若合并成功：bv->bv_len += len，返回 len
  │
  ├─ [4] 检查 bio->bi_vcnt < bio->bi_max_vecs
  │
  └─ [5] __bio_add_page(bio, page, len, offset)
       ├─ 检查 PCI P2PDMA 页面 → 设置 REQ_NOMERGE
       ├─ bvec_set_page() 填充新的 bio_vec
       └─ bi_vcnt++, bi_size += len
```

**合并条件**（`bvec_try_merge_page`）：
- 物理地址连续：`page_to_phys(last_page) + last_offset + last_len == page_to_phys(new_page) + new_offset`
- 非跨页冲突：在同一物理连续区域内

#### 4.5.2 `bio_add_folio` — 添加 folio

```c
bool bio_add_folio(struct bio *bio, struct folio *folio, size_t len, size_t off);
```
- 文件系统使用 folio 时调用，内部转换为 `bio_add_page(folio_page(folio, nr), ...)`
- folio 是连续物理页的抽象，`folio_page()` 获取第 nr 页

#### 4.5.3 `bio_iov_iter_get_pages` — 从 iov_iter 填充

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1298)）用于直接 I/O 路径：

```c
int bio_iov_iter_get_pages(struct bio *bio, struct iov_iter *iter,
                           unsigned len_align_mask);
```

- 处理 bvec 迭代器：直接设置 `bio->bi_io_vec = iter->bvec`（零拷贝）
- 处理用户空间迭代器：`iov_iter_extract_bvecs()` 提取页面并 pin
- 设置 `BIO_PAGE_PINNED` 标志（直接 I/O）
- 支持 PCI P2PDMA 页面

### 4.6 Bio 提交路径

#### 4.6.1 完整调用链

```
用户空间 write() / read()
  │
  ▼
文件系统（ext4/xfs/...）
  │  bio_alloc() + bio_add_page() + 设置 bi_end_io
  │
  ▼
submit_bio(bio)                              [bio.c:992]
  │  task_io_account_read/write()  ← 进程 I/O 统计
  │  bio_set_ioprio()               ← 设置 I/O 优先级
  │
  ▼
submit_bio_noacct(bio)                       [blk-core.c:848]
  │  检查 REQ_NOWAIT / 加密 / 故障注入
  │  bio_check_ro()                  ← 只读设备检查
  │  bio_check_eod()                 ← 越界检查
  │  blk_partition_remap()           ← 分区 LBA 重映射
  │  Flush 检查：bdev_write_cache()
  │  blk_throtl_bio()                ← 节流控制
  │
  ▼
submit_bio_noacct_nocheck(bio, false)        [blk-core.c:766]
  │  blk_cgroup_bio_start()          ← cgroup 记账
  │  trace_block_bio_queue()         ← trace 入队
  │
  ├─ current->bio_list 不为空（堆叠驱动递归提交）
  │    → bio_list_add() 加入链表，延迟处理
  │
  └─ 否则 → __submit_bio_noacct_mq(bio)
       │
       ▼
     __submit_bio(bio)                       [blk-core.c:636]
       │  blk_start_plug(&plug)       ← 开启批量提交
       │
       ├─ BD_HAS_SUBMIT_BIO 未设置（NVMe 等）
       │    → blk_mq_submit_bio(bio)  ← 进入 blk-mq 路径
       │
       └─ BD_HAS_SUBMIT_BIO 已设置（bio-based 设备）
            → disk->fops->submit_bio(bio)
       │
       blk_finish_plug(&plug)         ← 结束批量提交，刷新 plug
```

#### 4.6.2 `submit_bio` — 统计 + 提交

（[blk-core.c](file:///home/louis/code/linux/block/blk-core.c#L992)）

```c
void submit_bio(struct bio *bio)
{
    if (bio_op(bio) == REQ_OP_READ) {
        task_io_account_read(bio->bi_iter.bi_size);  // 进程读统计
        count_vm_events(PGPGIN, bio_sectors(bio));   // 全局读页统计
    } else if (bio_op(bio) == REQ_OP_WRITE) {
        count_vm_events(PGPGOUT, bio_sectors(bio));  // 全局写页统计
    }
    bio_set_ioprio(bio);     // 基于进程 nice 值设置 I/O 优先级
    submit_bio_noacct(bio);  // 提交
}
```

#### 4.6.3 `submit_bio_noacct` — 检查与预处理

（[blk-core.c](file:///home/louis/code/linux/block/blk-core.c#L848)）执行以下检查：

1. **REQ_NOWAIT 检查**：设备不支持 NOWAIT 则返回 `-EOPNOTSUPP`
2. **加密检查**：验证内联加密上下文是否受硬件支持
3. **故障注入**：`should_fail_bio()` 模拟 I/O 错误
4. **只读检查**：`bio_check_ro()` 阻止写操作
5. **越界检查**：`bio_check_eod()` 检查是否超出设备容量
6. **分区重映射**：`blk_partition_remap()` 将分区内 LBA 转换为设备 LBA
7. **Flush 预处理**：若设备无写缓存，清除 `REQ_PREFLUSH | REQ_FUA`
8. **操作码检查**：`REQ_OP_FLUSH` 不可通过 bio 提交（仅限 request）
9. **节流控制**：`blk_throtl_bio(bio)` 若限流则暂存，否则继续提交

#### 4.6.4 `__submit_bio` — Plug + 派发

（[blk-core.c](file:///home/louis/code/linux/block/blk-core.c#L636)）

```c
static void __submit_bio(struct bio *bio)
{
    struct blk_plug plug;
    blk_start_plug(&plug);       // 开启 plug（批量提交优化）

    if (!bdev_test_flag(bio->bi_bdev, BD_HAS_SUBMIT_BIO)) {
        blk_mq_submit_bio(bio);  // NVMe 等 blk-mq 设备路径
    } else if (likely(bio_queue_enter(bio) == 0)) {
        struct gendisk *disk = bio->bi_bdev->bd_disk;
        disk->fops->submit_bio(bio);  // bio-based 设备路径
        blk_queue_exit(disk->queue);
    }

    blk_finish_plug(&plug);      // 结束 plug，批量派发请求
}
```

**两条路径**：
- **blk-mq 路径**（`BD_HAS_SUBMIT_BIO` 未设置）：bio 进入 blk-mq 框架，被封装为 request
- **bio-based 路径**（`BD_HAS_SUBMIT_BIO` 已设置）：bio 直接传递给设备驱动（如 DM、MD）

#### 4.6.5 递归提交处理

当 `current->bio_list` 不为空时（堆叠驱动在 `->submit_bio` 回调中又调用了 `submit_bio`），新 bio 不直接提交，而是加入 `current->bio_list` 链表，由外层循环依次处理。这避免了递归调用导致的栈溢出。

### 4.7 Bio 完成路径

#### 4.7.1 `bio_endio` — 完成通知

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1814)）

```c
void bio_endio(struct bio *bio)
{
again:
    if (!bio_remaining_done(bio))      // 链式 bio：检查 __bi_remaining
        return;
    if (!bio_integrity_endio(bio))      // 完整性校验失败
        return;

    blk_zone_bio_endio(bio);            // Zone 设备处理
    rq_qos_done_bio(bio);               // QoS 完成通知

    if (bio_flagged(bio, BIO_TRACE_COMPLETION)) {
        trace_block_bio_complete(...);   // trace 完成事件
        bio_clear_flag(bio, BIO_TRACE_COMPLETION);
    }

    if (bio->bi_end_io == bio_chain_endio) {
        bio = __bio_chain_endio(bio);   // 链式 bio：传播错误并释放
        goto again;                     // 继续处理父 bio
    }

    if (bio->bi_end_io)
        bio->bi_end_io(bio);            // 调用用户回调
}
```

**完成流程**：
```
bio_endio(bio)
  │
  ├─ [1] bio_remaining_done(bio)
  │      ├─ 非链式 → 直接返回 true
  │      └─ 链式 → atomic_dec_and_test(&__bi_remaining)
  │           ├─ 不为 0 → 返回 false（还有子 bio 未完成）
  │           └─ 为 0 → 清除 BIO_CHAIN，返回 true
  │
  ├─ [2] bio_integrity_endio() — 数据完整性校验
  ├─ [3] rq_qos_done_bio() — QoS 层通知
  ├─ [4] trace_block_bio_complete() — 追踪完成
  │
  ├─ [5] 若 bi_end_io == bio_chain_endio
  │      → __bio_chain_endio(bio)
  │        ├─ 传播错误到父 bio
  │        ├─ bio_put(bio) 释放当前 bio
  │        └─ 返回父 bio，goto again
  │
  └─ [6] 调用用户注册的 bi_end_io(bio)
```

#### 4.7.2 `submit_bio_wait` — 同步等待

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1547)）

```c
int submit_bio_wait(struct bio *bio)
{
    DECLARE_COMPLETION_ONSTACK_MAP(done, ...);
    bio->bi_private = &done;
    bio->bi_end_io = submit_bio_wait_endio;  // 回调中 complete(done)
    bio->bi_opf |= REQ_SYNC;
    submit_bio(bio);
    blk_wait_io(&done);                      // 等待完成
    return blk_status_to_errno(bio->bi_status);
}
```

### 4.8 Bio 链式机制（bio_chain）

（[bio.c](file:///home/louis/code/linux/block/bio.c#L381)）允许将多个 bio 串联，父 bio 在所有子 bio 完成后才通知上层：

```c
void bio_chain(struct bio *bio, struct bio *parent);
```

**原理**：
```
parent->__bi_remaining = 2   ← bio_inc_remaining(parent) 增加计数
bio->bi_private = parent
bio->bi_end_io = bio_chain_endio

子 bio 完成后：
  bio_chain_endio(bio)
    → __bio_chain_endio(bio)
      → 传播 bio->bi_status 到 parent->bi_status
      → bio_put(bio) 释放子 bio
      → bio_endio(parent) 继续处理父 bio
        → __bi_remaining 减 1
        → 若为 0，调用 parent->bi_end_io
```

**典型场景**：DM 层将一个 bio 拆分后，使用 bio_chain 串联所有子 bio，确保所有子 bio 完成后才通知上层。

### 4.9 Bio 克隆与分割

#### 4.9.1 `bio_alloc_clone` — 克隆 bio

（[bio.c](file:///home/louis/code/linux/block/bio.c#L920)）

```c
struct bio *bio_alloc_clone(struct block_device *bdev, struct bio *bio_src,
                            gfp_t gfp, struct bio_set *bs);
```

- 分配新 bio，共享 `bio_src->bi_io_vec`（不复制数据）
- 设置 `BIO_CLONED` 标志：克隆 bio 不拥有 bio_vec 的所有权
- 克隆 bio 不可调用 `bio_add_page()`（`BIO_CLONED` 检查）

#### 4.9.2 `bio_split` — 分割 bio

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1875)）

```c
struct bio *bio_split(struct bio *bio, int sectors,
                      gfp_t gfp, struct bio_set *bs);
```

**流程**：
```
bio_split(bio, sectors, gfp, bs)
  │
  ├─ [1] 检查：sectors > 0 且 < bio_sectors(bio)
  ├─ [2] 检查：不能分割 REQ_OP_ZONE_APPEND 和 REQ_ATOMIC
  │
  ├─ [3] split = bio_alloc_clone(bio->bi_bdev, bio, gfp, bs)
  │      └─ 共享 bio_vec，设置 bi_iter.bi_size = sectors << 9
  │
  ├─ [4] bio_advance(bio, split->bi_iter.bi_size)
  │      └─ 原始 bio 的 bi_iter 前移，跳过已分割部分
  │
  └─ [5] 返回 split（调用者负责释放）
```

**分割示意图**：
```
         sectors=4
         ←──────→
原始 bio: [bv0][bv1][bv2][bv3][bv4][bv5]
         └──────────┘ └──────────────────┘
           split bio      原始 bio（剩余）
          (bi_size=4)    (bi_size=2, 已前移)
```

#### 4.9.3 `bio_trim` — 裁剪 bio

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1925)）

```c
void bio_trim(struct bio *bio, sector_t offset, sector_t size);
```
- 从 bio 中裁剪出指定范围和长度的子区域
- 通过 `bio_advance()` 前移 + 设置 `bi_size` 实现

### 4.10 Bio 数据拷贝

#### 4.10.1 `bio_copy_data` — 全量拷贝

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1651)）

```c
void bio_copy_data(struct bio *dst, struct bio *src);
```

- 拷贝 `min(src->bi_size, dst->bi_size)` 字节
- 逐个 bio_vec 对比，使用 `bvec_kmap_local()` 临时映射

#### 4.10.2 `bio_copy_data_iter` — 迭代器拷贝

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1622)）

```c
void bio_copy_data_iter(struct bio *dst, struct bvec_iter *dst_iter,
                        struct bio *src, struct bvec_iter *src_iter);
```

- 使用迭代器指定拷贝起始位置，适用于部分拷贝
- 内部使用 `memcpy` + `kunmap_local`

### 4.11 页面管理与直接 I/O

#### 4.11.1 页面 Pin 机制

直接 I/O 使用 `bio_iov_iter_get_pages()` 将用户空间页面 pin 到 bio：

```c
bio_set_flag(bio, BIO_PAGE_PINNED);  // 标记页面已被 pin
```

**释放**（[bio.c](file:///home/louis/code/linux/block/bio.c#L1200)）：

```c
void __bio_release_pages(struct bio *bio, bool mark_dirty)
{
    bio_for_each_folio_all(fi, bio) {
        if (mark_dirty) {
            folio_lock(fi.folio);
            folio_mark_dirty(fi.folio);  // 标记脏页
            folio_unlock(fi.folio);
        }
        unpin_user_folio(fi.folio, nr_pages);  // 解除 pin
    }
}
```

#### 4.11.2 脏页处理（`bio_check_pages_dirty`）

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1724)）处理直接 I/O 读取的脏页问题：

```
bio_check_pages_dirty(bio)
  │
  ├─ 遍历所有 folio，检查是否脏
  │
  ├─ 全部脏 → bio_release_pages(bio, false) → bio_put(bio)
  │
  └─ 有非脏页 → 加入 bio_dirty_list → schedule_work(&bio_dirty_work)
       └─ bio_dirty_fn() 在进程上下文中重新标记脏页并释放
```

**背景**：中断上下文不能持有页锁，因此若发现页面已被回写（非脏），需要推迟到进程上下文处理。

### 4.12 Bounce Buffer 机制

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1409)）当需要对数据进行额外处理（如校验和）时使用：

```c
int bio_iov_iter_bounce(struct bio *bio, struct iov_iter *iter);
void bio_iov_iter_unbounce(struct bio *bio, bool is_error, bool mark_dirty);
```

**写入路径**（`bio_iov_iter_bounce_write`）：
- 分配 folio 作为 bounce buffer
- `copy_from_iter()` 从用户空间拷贝数据到 bounce buffer
- `bio_add_folio_nofail()` 将 bounce buffer 添加到 bio

**读取路径**（`bio_iov_iter_bounce_read`）：
- 分配一个 bounce buffer folio 作为 bio_vec[0]
- 用户数据页面作为 bio_vec[1..]
- 完成时 `bio_iov_iter_unbounce()` 将 bounce buffer 数据拷贝回用户空间

### 4.13 bio_set 初始化与销毁

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1983)）

```c
int bioset_init(struct bio_set *bs, unsigned int pool_size,
                unsigned int front_pad, int flags);
void bioset_exit(struct bio_set *bs);
```

**初始化标志**：
| 标志 | 含义 |
|------|------|
| `BIOSET_NEED_BVECS` | 需要独立的 bio_vec 内存池 |
| `BIOSET_NEED_RESCUER` | 创建救援工作队列（堆叠驱动需要） |
| `BIOSET_PERCPU_CACHE` | 启用 per-CPU 分配缓存 |

**死锁避免**（rescuer 机制）：
当在 `submit_bio_noacct()` 上下文中（`current->bio_list` 不为空）分配 bio 时，若 mempool 耗尽，无法直接回收（因为当前上下文的 bio 还没提交完成）。此时会将等待的 bio 交给 `rescue_workqueue` 救援线程处理，释放 mempool 空间。

### 4.14 辅助模块

| 文件 | 行数 | 功能 |
|------|------|------|
| `block/blk-map.c` | 677 | 将内核/用户空间内存映射到 bio（`blk_rq_map_user`、`blk_rq_map_kern`） |
| `block/blk-lib.c` | 369 | 块层辅助函数（`__blkdev_issue_discard`、`__blkdev_issue_write_zeroes` 等） |
| `block/bdev.c` | 1,382 | 块设备（bdev）文件操作和地址空间管理 |
| `block/bio-integrity.c` | — | 数据完整性（DIF/DIX）支持 |
| `block/bio-integrity-auto.c` | — | 自动完整性元数据生成 |

### 4.15 完整调用链总结

**从 write() 到 NVMe 硬件命令**：

```
write(fd, buf, len)
  │
  ├─ VFS: vfs_write()
  ├─ 文件系统: ext4_file_write_iter()
  │    ├─ bio_alloc(bdev, nr_vecs, REQ_OP_WRITE, GFP_KERNEL)
  │    │    └─ bio_alloc_bioset() → per-CPU cache / mempool
  │    ├─ bio_add_page(bio, page, len, offset)
  │    │    └─ bvec_try_merge_page() 尝试合并
  │    │    └─ __bio_add_page() 添加新 bio_vec
  │    ├─ bio->bi_end_io = ext4_end_bio
  │    └─ submit_bio(bio)
  │
  ├─ submit_bio(bio)                    [blk-core.c:992]
  │    ├─ task_io_account_write()       进程统计
  │    └─ submit_bio_noacct(bio)        [blk-core.c:848]
  │         ├─ bio_check_ro()           只读检查
  │         ├─ bio_check_eod()          越界检查
  │         ├─ blk_partition_remap()    分区重映射
  │         └─ submit_bio_noacct_nocheck()[blk-core.c:766]
  │              └─ __submit_bio_noacct_mq(bio)
  │                   └─ __submit_bio(bio) [blk-core.c:636]
  │                        ├─ blk_start_plug()
  │                        ├─ blk_mq_submit_bio(bio)  ← 进入 blk-mq
  │                        └─ blk_finish_plug()
  │
  └─ blk-mq: bio → request → nvme_queue_rq()
       └─ 硬件提交 SQ Entry
```

**完成路径**：

```
硬件中断 → nvme_irq()
  │
  ├─ nvme_process_cq()
  ├─ nvme_handle_cqe()
  ├─ blk_mq_complete_request(req)
  │    └─ req->end_io(req, error) → blk_mq_end_request()
  │         └─ blk_update_request() → req_bio_endio()
  │              └─ bio_endio(bio)          [bio.c:1814]
  │                   ├─ bio_remaining_done()
  │                   ├─ rq_qos_done_bio()
  │                   ├─ trace_block_bio_complete()
  │                   └─ bio->bi_end_io(bio)  ← 用户回调
  │                        └─ ext4_end_bio()
  │                             └─ bio_put(bio) → per-CPU cache / mempool
  └─ 唤醒等待者
```

### 4.16 边界处理与设备保护

#### 4.16.1 `guard_bio_eod` — 越界保护

（[bio.c](file:///home/louis/code/linux/block/bio.c#L739)）当 bio 的最后几个扇区超出设备容量时，截断 bio 而不是直接返回错误，允许对设备尾部非对齐扇区进行 I/O：

```c
void guard_bio_eod(struct bio *bio)
{
    sector_t maxsector = bdev_nr_sectors(bio->bi_bdev);

    if (!maxsector)
        return;

    // 整个 bio 的起始扇区已超出设备 → 让 IO 层返回 EIO
    if (unlikely(bio->bi_iter.bi_sector >= maxsector))
        return;

    maxsector -= bio->bi_iter.bi_sector;  // 剩余可访问扇区数
    if (likely((bio->bi_iter.bi_size >> 9) <= maxsector))
        return;                           // 完全在范围内，无需处理

    bio_truncate(bio, maxsector << 9);    // 截断超出部分
}
```

**典型场景**：设备容量为 1000.5 个扇区，bio 请求读取扇区 999-1001（共 3 个扇区）。`guard_bio_eod()` 将 bio 截断为 999-1000.5（1.5 个扇区），超出部分清零（读）或丢弃（写）。

#### 4.16.2 `bio_truncate` — 截断 bio

（[bio.c](file:///home/louis/code/linux/block/bio.c#L675)）将 bio 截断到指定大小，对于读操作将超出部分清零：

```c
static void bio_truncate(struct bio *bio, unsigned new_size)
{
    if (new_size >= bio->bi_iter.bi_size)
        return;

    // 读操作：将超出部分的内存清零（防止泄露旧数据）
    if (bio_op(bio) != REQ_OP_READ)
        goto exit;

    bio_for_each_segment(bv, bio, iter) {
        if (done + bv.bv_len > new_size) {
            unsigned int offset = truncated ? 0 : new_size - done;
            memzero_page(bv.bv_page, bv.bv_offset + offset,
                         bv.bv_len - offset);
            truncated = true;
        }
        done += bv.bv_len;
    }

exit:
    bio->bi_iter.bi_size = new_size;  // 只更新 bi_size，不修改 bio_vec 数组
}
```

**关键设计**：`bio_truncate()` 只修改 `bi_iter.bi_size`，不修改 `bio_vec[]` 数组。这是因为文件系统的 `bi_end_io` 回调可能使用 `bio_for_each_segment_all()` 遍历所有 bio_vec 释放页面，修改数组会导致不一致。

### 4.17 其他 Bio 操作

#### 4.17.1 `bio_kmalloc` — kmalloc 分配 bio

（[bio.c](file:///home/louis/code/linux/block/bio.c#L661)）绕过 mempool，直接用 kmalloc 分配 bio（含内联 bio_vec）：

```c
struct bio *bio_kmalloc(unsigned short nr_vecs, gfp_t gfp_mask)
{
    if (nr_vecs > BIO_MAX_INLINE_VECS)   // 不能超过内联 vec 数量
        return NULL;
    return kmalloc(sizeof(*bio) + nr_vecs * sizeof(struct bio_vec), gfp_mask);
}
```

- **与 `bio_alloc` 的区别**：不经过 mempool，无死锁保护，可能失败
- **释放方式**：先 `bio_uninit()`，再 `kfree(bio)`
- **使用场景**：非文件系统 I/O 路径，如驱动初始化阶段的临时 bio

#### 4.17.2 `bio_init_clone` — 原地克隆

（[bio.c](file:///home/louis/code/linux/block/bio.c#L945)）在调用者提供的内存上初始化一个克隆 bio，共享源 bio 的 `bi_io_vec`：

```c
int bio_init_clone(struct block_device *bdev, struct bio *bio,
                   struct bio *bio_src, gfp_t gfp)
{
    bio_init(bio, bdev, bio_src->bi_io_vec, 0, bio_src->bi_opf);
    return __bio_clone(bio, bio_src, gfp);
}
```

**与 `bio_alloc_clone` 的区别**：

| 特性 | `bio_alloc_clone` | `bio_init_clone` |
|------|-------------------|------------------|
| bio 内存 | 从 bio_set 分配 | 调用者提供 |
| 释放方式 | `bio_put()` | `bio_uninit()` |
| 使用场景 | 通用克隆 | 栈上/嵌入 bio |

**典型场景**：DM/MD 层在栈上分配 bio 结构体，使用 `bio_init_clone()` 初始化后直接提交。

#### 4.17.3 `bio_set_pages_dirty` — 标记脏页

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1700)）在直接 I/O 写入前，将 bio 中所有页面标记为脏页：

```c
void bio_set_pages_dirty(struct bio *bio)
{
    struct folio_iter fi;

    bio_for_each_folio_all(fi, bio) {
        folio_lock(fi.folio);
        folio_mark_dirty(fi.folio);
        folio_unlock(fi.folio);
    }
}
```

**与 `bio_check_pages_dirty` 的配合**：

```
直接 I/O 写路径：
  bio_set_pages_dirty(bio)     ← 提交前标记脏页（进程上下文）
  submit_bio(bio)
  ...
  [中断上下文]
  bio_check_pages_dirty(bio)   ← 完成后检查是否仍为脏页
    ├─ 全部脏 → 直接释放
    └─ 有非脏页 → 推迟到 bio_dirty_fn() 进程上下文中重新标记
```

**为什么需要二次检查**：在 I/O 执行期间，回写线程可能已经将这些页面写出并清除了脏标志。`bio_check_pages_dirty()` 在中断上下文中检查，若发现页面已变干净，则推迟到 `bio_dirty_work` 工作队列在进程上下文中重新标记（因为 `folio_mark_dirty()` 需要获取可能睡眠的锁）。

#### 4.17.4 `bio_await_chain` — 等待链式 bio 完成

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1598)）提交 bio 并等待其及所有子 bio 完成：

```c
void bio_await_chain(struct bio *bio)
{
    DECLARE_COMPLETION_ONSTACK_MAP(done,
            bio->bi_bdev->bd_disk->lockdep_map);

    bio->bi_private = &done;
    bio->bi_end_io = bio_wait_end_io;    // 回调中 complete(done) + bio_put(bio)
    bio_endio(bio);                       // 触发完成流程
    blk_wait_io(&done);                   // 等待
}
```

- **与 `submit_bio_wait` 的区别**：`submit_bio_wait` 提交新 bio，`bio_await_chain` 等待已存在的 bio（通常是链式 bio 的父 bio）
- `bio_wait_end_io` 在 complete 后还会调用 `bio_put(bio)`，与 `submit_bio_wait_endio`（仅 complete）不同

#### 4.17.5 `bdev_rw_virt` — 同步块设备读写

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1573)）对块设备执行同步读写，数据位于内核直接映射区：

```c
int bdev_rw_virt(struct block_device *bdev, sector_t sector, void *data,
                 size_t len, enum req_op op)
{
    struct bio_vec bv;
    struct bio bio;

    bio_init(&bio, bdev, &bv, 1, op);          // 栈上分配 bio
    bio.bi_iter.bi_sector = sector;
    bio_add_virt_nofail(&bio, data, len);       // 内核虚拟地址直接添加
    return submit_bio_wait(&bio);               // 同步提交等待
}
```

**使用场景**：驱动初始化时读取设备元数据（如分区表、超级块），无需手动管理 bio 生命周期。

#### 4.17.6 `zero_fill_bio_iter` — 零填充

（[bio.c](file:///home/louis/code/linux/block/bio.c#L666)）从指定迭代器位置开始，将 bio 数据清零：

```c
void zero_fill_bio_iter(struct bio *bio, struct bvec_iter start)
{
    struct bio_vec bv;
    struct bvec_iter iter;

    __bio_for_each_segment(bv, bio, iter, start)
        memzero_bvec(&bv);                     // 逐段清零
}
```

**使用场景**：`bio_truncate()` 中的读操作清零、`REQ_OP_WRITE_ZEROES` 回退到软件清零。

#### 4.17.7 `bio_free_pages` — 释放 bio 所有页面

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1685)）遍历 bio 中所有 bio_vec，释放每个页面：

```c
void bio_free_pages(struct bio *bio)
{
    struct bio_vec *bvec;
    struct bvec_iter_all iter_all;

    bio_for_each_segment_all(bvec, bio, iter_all)
        __free_page(bvec->bv_page);
}
```

**使用场景**：错误路径中释放已分配但未提交的 bio 页面。

#### 4.17.8 `bio_add_vmalloc` — 添加 vmalloc 区域

（[bio.c](file:///home/louis/code/linux/block/bio.c#L1186)）将 vmalloc 区域的数据添加到 bio：

```c
bool bio_add_vmalloc(struct bio *bio, void *vaddr, unsigned int len)
{
    do {
        unsigned int added = bio_add_vmalloc_chunk(bio, vaddr, len);
        if (!added)
            return false;
        vaddr += added;
        len -= added;
    } while (len);
    return true;
}
```

内部调用 `bio_add_vmalloc_chunk()`，每次添加一页（`vmalloc_to_page()` 转换），写操作时调用 `flush_kernel_vmap_range()` 确保数据对 DMA 可见。

### 4.18 Per-CPU 分配缓存深度解析

#### 4.18.1 `bio_alloc_cache` 结构

（[bio.c](file:///home/louis/code/linux/block/bio.c#L30)）每个 CPU 拥有一个缓存实例，加速 bio 分配与释放：

```c
struct bio_alloc_cache {
    struct bio      *free_list;       // 进程上下文释放的 bio 链表
    struct bio      *free_list_irq;   // 中断上下文释放的 bio 链表
    unsigned int    nr;               // free_list 中 bio 数量
    unsigned int    nr_irq;           // free_list_irq 中 bio 数量
};
```

**双链表设计的原因**：中断上下文释放的 bio 放入 `free_list_irq`（无需关中断），进程上下文释放的放入 `free_list`。当 `free_list` 为空且 `free_list_irq` 积累超过阈值 `ALLOC_CACHE_THRESHOLD=16` 时，通过 `bio_alloc_irq_cache_splice()` 将 `free_list_irq` 整体搬入 `free_list`（关中断保护）。

**关键常量**：

| 常量 | 值 | 含义 |
|------|-----|------|
| `ALLOC_CACHE_THRESHOLD` | 16 | `free_list_irq` 搬入 `free_list` 的阈值 |
| `ALLOC_CACHE_MAX` | 256 | 缓存总容量上限（`nr + nr_irq`），超过则直接释放回 mempool |

#### 4.18.2 分配路径中的缓存交互

（[bio.c](file:///home/louis/code/linux/block/bio.c#L490) `bio_alloc_percpu_cache`）：

```
bio_alloc_percpu_cache(bdev, nr_vecs, opf, gfp, bs)
  │
  ├─ cache = per_cpu_ptr(bs->cache, get_cpu())   // 获取当前 CPU 缓存
  │
  ├─ 若 cache->free_list 为空：
  │   └─ 若 cache->nr_irq >= ALLOC_CACHE_THRESHOLD (16)
  │       → bio_alloc_irq_cache_splice(cache)     // 将 irq 链表搬入 free_list
  │         ├─ local_irq_save
  │         ├─ free_list = free_list_irq
  │         ├─ free_list_irq = NULL
  │         ├─ nr += nr_irq, nr_irq = 0
  │         └─ local_irq_restore
  │
  ├─ bio = cache->free_list                       // 从链表头部取出
  ├─ cache->free_list = bio->bi_next
  ├─ cache->nr--
  ├─ put_cpu()
  │
  └─ bio_init_inline(bio, bdev, nr_vecs, opf)     // 重新初始化
```

**缓存命中条件**：仅当 `nr_vecs <= BIO_INLINE_VECS` 时使用缓存（内联 bio_vec 的 bio 大小固定，可以复用）。

#### 4.18.3 释放路径中的缓存交互

（[bio.c](file:///home/louis/code/linux/block/bio.c#L836) `bio_put_percpu_cache`）：

```
bio_put_percpu_cache(bio)
  │
  ├─ cache = per_cpu_ptr(bio->bi_pool->cache, get_cpu())
  │
  ├─ 若 nr_irq + nr > ALLOC_CACHE_MAX (256) → bio_free(bio) 直接释放
  │
  ├─ 若 in_task()：放入 free_list
  │   ├─ bio_uninit(bio)
  │   ├─ bio->bi_next = cache->free_list
  │   ├─ cache->free_list = bio
  │   └─ cache->nr++
  │
  ├─ 若 in_hardirq()：放入 free_list_irq
  │   ├─ bio_uninit(bio)
  │   ├─ bio->bi_next = cache->free_list_irq
  │   ├─ cache->free_list_irq = bio
  │   └─ cache->nr_irq++
  │
  └─ 否则（软中断等）→ bio_free(bio) 直接释放
```

**性能收益**：分配和释放均在 per-CPU 缓存中操作，无需锁，无需访问 mempool 的全局数据结构，显著减少分配延迟。

#### 4.18.4 CPU 热插拔处理

（[bio.c](file:///home/louis/code/linux/block/bio.c#L805)）当 CPU 下线时，通过 `CPUHP_BIO_DEAD` 回调清理该 CPU 的缓存：

```c
static int bio_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
    struct bio_set *bs;
    bs = hlist_entry_safe(node, struct bio_set, cpuhp_dead);
    if (bs->cache) {
        struct bio_alloc_cache *cache = per_cpu_ptr(bs->cache, cpu);
        bio_alloc_cache_prune(cache, -1U);  // 释放所有缓存 bio
    }
    return 0;
}
```

`bio_alloc_cache_prune(cache, -1U)` 将 `free_list` 和 `free_list_irq` 中的所有 bio 释放回 mempool，防止内存泄漏。

#### 4.18.5 `bio_alloc_cache_destroy` — 缓存销毁

（[bio.c](file:///home/louis/code/linux/block/bio.c#L817)）在 `bioset_exit()` 时调用，注销 CPU 热插拔回调，遍历所有 possible CPU 释放缓存 bio，最后释放 per-CPU 内存。

#### 4.18.6 全局 bio 池初始化

（[bio.c](file:///home/louis/code/linux/block/bio.c#L2041)）系统启动时通过 `subsys_initcall(init_bio)` 初始化：

```c
static int __init init_bio(void)
{
    // 创建 bio_vec slab（16/64/128/256 四个级别）
    for (i = 0; i < ARRAY_SIZE(bvec_slabs); i++)
        kmem_cache_create(bvec_slabs[i].name, ...);

    // 注册 CPU 热插拔回调
    cpuhp_setup_state_multi(CPUHP_BIO_DEAD, "block/bio:dead", NULL, bio_cpu_dead);

    // 初始化全局 fs_bio_set（启用 per-CPU 缓存）
    bioset_init(&fs_bio_set, BIO_POOL_SIZE, 0,
                BIOSET_NEED_BVECS | BIOSET_PERCPU_CACHE);
}
```

---

## 5. 多队列框架（blk-mq）

### 5.1 架构概述：单队列 vs 多队列

传统单队列（SQ）块层架构存在一个核心瓶颈：**所有 CPU 共享一个请求队列，由一个全局锁保护**。当多个 CPU 提交 I/O 时，锁竞争成为性能瓶颈，尤其在高 IOPS 的 NVMe SSD 上更为明显。

blk-mq（Multi-Queue）从 3.13 内核引入，将单个请求队列拆分为两级：

```
                     ┌──────────┐  ┌──────────┐  ┌──────────┐
  CPU 0 ───────────→ │ ctx[0]   │  │ ctx[1]   │  │ ctx[2]   │  ← 软件队列（per-CPU）
  CPU 1 ───────────→ │ ctx[0]   │  │  ...     │  │  ...     │     blk_mq_ctx
  CPU 2 ───────────→ │ ctx[0]   │  │          │  │          │
                     └────┬─────┘  └────┬─────┘  └────┬─────┘
                          │             │             │
                          ▼             ▼             ▼
                     ┌──────────┐  ┌──────────┐  ┌──────────┐
                     │ hctx[0]  │  │ hctx[1]  │  │ hctx[2]  │  ← 硬件队列
                     └────┬─────┘  └────┬─────┘  └────┬─────┘  blk_mq_hw_ctx
                          │             │             │
                          ▼             ▼             ▼
                     ┌──────────────────────────────────────┐
                     │         NVMe SSD / 硬件控制器          │
                     └──────────────────────────────────────┘
```

**关键设计原则**：

| 特性 | 单队列 (SQ) | 多队列 (blk-mq) |
|------|-----------|---------------|
| 请求队列 | 1 个全局队列 | 多个 per-CPU 软件队列 + 多个硬件队列 |
| 锁竞争 | 全局锁，严重竞争 | per-CPU 锁，无竞争 |
| Tag 分配 | 单一路径 | 多路径并行，per-hctx bitmap |
| 中断处理 | 单 CPU 处理 | 中断可路由到不同 CPU |
| 缓存局部性 | 差（跨 CPU 跳跃） | 好（请求在分配 CPU 上完成） |
| 扩展性 | 有限（~8 核） | 线性扩展至数百核 |

### 5.2 核心数据结构

#### 5.2.1 `blk_mq_ctx` — 软件队列（面向 CPU）

（[blk-mq.h](file:///home/louis/code/linux/block/blk-mq.h#L17)）每个 CPU 拥有一个软件队列上下文，用于接收来自该 CPU 的 I/O 提交：

```c
struct blk_mq_ctx {
    struct {
        spinlock_t  lock;                              // per-CPU 锁，保护本软件队列
        struct list_head rq_lists[HCTX_MAX_TYPES];     // 请求链表，按队列类型分桶
    } ____cacheline_aligned_in_smp;                    // 对齐到缓存行，避免伪共享

    unsigned int        cpu;                           // 绑定的 CPU 编号
    unsigned short      index_hw[HCTX_MAX_TYPES];      // 在各硬件队列 bitmap 中的位索引
    struct blk_mq_hw_ctx *hctxs[HCTX_MAX_TYPES];       // 映射到的硬件队列指针
    struct request_queue *queue;                       // 所属请求队列
    struct blk_mq_ctxs  *ctxs;                         // 所有 ctx 的容器
    struct kobject      kobj;                          // sysfs 对象
};
```

**关键字段解析**：
- `rq_lists[HCTX_MAX_TYPES]`：按 `HCTX_TYPE_DEFAULT`、`HCTX_TYPE_READ`、`HCTX_TYPE_POLL` 分成三个链表，不同 I/O 类型路由到不同硬件队列
- `index_hw[HCTX_MAX_TYPES]`：记录本 ctx 在对应硬件队列的 `ctx_map` bitmap 中的位索引，用于快速标记"有待派发请求"
- `hctxs[HCTX_MAX_TYPES]`：直接指向目标硬件队列，通过 `blk_mq_map_queue()` 查找

#### 5.2.2 `blk_mq_hw_ctx` — 硬件队列（面向设备）

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h#L322)）每个硬件队列对应设备的一个提交队列（如 NVMe SQ），是实际向硬件提交命令的实体：

```c
struct blk_mq_hw_ctx {
    struct {
        spinlock_t      lock;                   // 保护 dispatch 链表
        struct list_head dispatch;              // 待派发但暂缺资源（如 tag）的请求
        unsigned long   state;                  // BLK_MQ_S_* 状态标志
    } ____cacheline_aligned_in_smp;

    struct delayed_work run_work;               // 延迟运行工作队列
    cpumask_var_t       cpumask;                // 可运行此 hctx 的 CPU 集合
    int                 next_cpu;               // Round-Robin CPU 选择游标
    int                 next_cpu_batch;         // 每批剩余请求数

    unsigned long       flags;                  // BLK_MQ_F_* 行为标志
    void                *sched_data;            // I/O 调度器私有数据
    struct request_queue *queue;                // 所属请求队列
    struct blk_flush_queue *fq;                 // 刷新队列（Flush 状态机）
    void                *driver_data;           // 驱动私有数据

    struct sbitmap      ctx_map;                // 位图：标记哪些 ctx 有待派发请求
    struct blk_mq_ctx   *dispatch_from;         // 无调度器时从哪个 ctx 派发
    unsigned int        dispatch_busy;          // EWMA 繁忙度

    unsigned short      type;                   // 队列类型（DEFAULT/READ/POLL）
    unsigned short      nr_ctx;                 // 绑定的 ctx 数量
    struct blk_mq_ctx   **ctxs;                 // 绑定的 ctx 数组

    struct blk_mq_tags  *tags;                  // 驱动 tag 池（派发时分配）
    struct blk_mq_tags  *sched_tags;            // 调度器 tag 池（分配请求时分配）

    unsigned int        numa_node;              // NUMA 节点
    unsigned int        queue_num;              // 硬件队列编号
    atomic_t            nr_active;              // 活跃请求计数（共享 tag 时使用）
    struct kobject      kobj;                   // sysfs 对象
};
```

**关键字段解析**：
- `dispatch`：当请求无法立即派发（如无 tag）时暂存于此，下次 `run_queue` 优先处理这些请求，保证公平性
- `ctx_map`：sbitmap，记录哪些 ctx 的 `rq_lists` 中有待派发请求，`blk_mq_hctx_has_pending()` 通过检查此 bitmap 快速判断是否有工作
- `dispatch_busy`：EWMA（指数加权移动平均）算法估算的繁忙度，用于 `blk_mq_update_dispatch_busy()` 判断是否应继续从此 hctx 派发更多请求
- `tags` vs `sched_tags`：有调度器时，请求分配时先从 `sched_tags` 获取 tag，派发时再换到 `tags`；无调度器时只用 `tags`

#### 5.2.3 `blk_mq_tag_set` — 标签集（设备级）

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h#L534)）`blk_mq_tag_set` 是设备级的配置，一个设备有一个 `tag_set`，可以跨多个 `request_queue` 共享（如 NVMe 多命名空间）：

```c
struct blk_mq_tag_set {
    const struct blk_mq_ops *ops;               // 驱动回调函数表
    struct blk_mq_queue_map map[HCTX_MAX_TYPES];// CPU→硬件队列的映射表
    unsigned int    nr_maps;                    // 使用的映射表数量
    unsigned int    nr_hw_queues;               // 硬件队列总数
    unsigned int    queue_depth;                // 每队列的队列深度（tag 数量）
    unsigned int    reserved_tags;              // 预留 tag 数量
    unsigned int    cmd_size;                   // 驱动命令结构体大小
    int             numa_node;                  // NUMA 节点
    unsigned int    timeout;                    // 请求超时时间（ms）
    unsigned int    flags;                      // BLK_MQ_F_* 标志
    void            *driver_data;               // 驱动私有数据

    struct blk_mq_tags **tags;                  // 指向各 hctx 的 tag 池
    struct blk_mq_tags *shared_tags;            // 共享 tag 池（多队列共享）

    struct mutex        tag_list_lock;          // 保护 tag_list
    struct list_head    tag_list;               // 使用此 tag_set 的 request_queue 列表
    struct srcu_struct  *srcu;                  // SRCU 保护 tag_list 遍历
    struct srcu_struct  tags_srcu;              // SRCU 保护 tags 重分配
    struct rw_semaphore update_nr_hwq_lock;     // 保护 nr_hw_queues 更新
};
```

#### 5.2.4 `blk_mq_tags` — 标签池

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h#L774)）每个硬件队列拥有一个 tag 池，tag 是硬件命令标识符，类似于 NVMe 的 Command ID：

```c
struct blk_mq_tags {
    unsigned int        nr_tags;                // 总 tag 数量
    unsigned int        nr_reserved_tags;       // 预留 tag 数量
    atomic_t            active_queues;          // 活跃队列计数（共享 tag 时使用）
    struct sbitmap_queue bitmap_tags;           // 普通 tag 位图
    struct sbitmap_queue breserved_tags;        // 预留 tag 位图
    struct request **rqs;                       // tag → request 的映射数组
    struct request **static_rqs;                // 静态分配的 request 数组
    struct list_head    page_list;              // 用于分配 request 的页面列表
    spinlock_t          lock;                   // 保护 active_queues
};
```

**Tag 的作用**：Tag 是硬件命令标识符 —— 驱动向硬件提交命令时，需要一个 tag 作为命令 ID。硬件完成命令时，通过 tag 查找对应的 request。Tag 数量受 `queue_depth` 限制，耗尽时新请求需要等待。

#### 5.2.5 `blk_mq_ops` — 驱动回调函数表

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h#L578)）驱动通过 `blk_mq_ops` 向块层注册回调，实现硬件交互：

```c
struct blk_mq_ops {
    blk_status_t (*queue_rq)(struct blk_mq_hw_ctx *,
                             const struct blk_mq_queue_data *);
    void (*commit_rqs)(struct blk_mq_hw_ctx *);
    void (*queue_rqs)(struct rq_list *rqlist);
    int  (*get_budget)(struct request_queue *);
    void (*put_budget)(struct request_queue *, int);
    void (*set_rq_budget_token)(struct request *, int);
    int  (*get_rq_budget_token)(struct request *);
    enum blk_eh_timer_return (*timeout)(struct request *);
    int  (*poll)(struct blk_mq_hw_ctx *, struct io_comp_batch *);
    void (*complete)(struct request *);
    int  (*init_hctx)(struct blk_mq_hw_ctx *, void *, unsigned int);
    void (*exit_hctx)(struct blk_mq_hw_ctx *, unsigned int);
    int  (*init_request)(struct blk_mq_tag_set *, struct request *, unsigned int, unsigned int);
    void (*exit_request)(struct blk_mq_tag_set *, struct request *, unsigned int);
    int  (*busy)(struct request *);
    int  (*map_queues)(struct blk_mq_tag_set *);
    void (*show_rq)(struct seq_file *, struct request *);
};
```

| 回调 | 作用 |
|------|------|
| `queue_rq` | 向硬件提交单个请求（核心接口） |
| `queue_rqs` | 向硬件批量提交请求列表（NVMe 使用此接口实现 Doorbell 批量更新） |
| `commit_rqs` | 当 `bd.last` 为 true 时通知驱动提交（如 Doorbell 写） |
| `complete` | 命令完成时的回调 |
| `get_budget` / `put_budget` | 资源预算控制（SCSI 使用） |
| `poll` | 轮询模式下的完成检查 |
| `timeout` | 请求超时处理 |

#### 5.2.6 `blk_mq_queue_map` — CPU 映射表

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h#L475)）`mq_map` 是一个长度为 `nr_cpu_ids` 的数组，`mq_map[cpu]` 返回该 CPU 应使用的硬件队列编号：

```c
struct blk_mq_queue_map {
    unsigned int *mq_map;       // CPU ID → 硬件队列索引的映射表
    unsigned int nr_queues;     // 此类型硬件队列的数量
    unsigned int queue_offset;  // 硬件队列偏移（如 NVMe 将 poll 队列放在后面）
};
```

#### 5.2.7 队列类型（`hctx_type`）

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h#L488)）硬件队列按功能分为三种类型：

```c
enum hctx_type {
    HCTX_TYPE_DEFAULT,          // 默认队列（处理写请求和未分类的请求）
    HCTX_TYPE_READ,             // 读队列（只有读请求路由到此）
    HCTX_TYPE_POLL,             // 轮询队列（处理 REQ_POLLED 请求）
    HCTX_MAX_TYPES,             // 类型总数 = 3
};
```

**路由规则**（[blk-mq.h](file:///home/louis/code/linux/block/blk-mq.h#L90)）：

```c
static inline enum hctx_type blk_mq_get_hctx_type(blk_opf_t opf)
{
    if (opf & REQ_POLLED)
        return HCTX_TYPE_POLL;        // 轮询请求 → poll 队列
    else if ((opf & REQ_OP_MASK) == REQ_OP_READ)
        return HCTX_TYPE_READ;        // 读请求 → read 队列
    return HCTX_TYPE_DEFAULT;         // 默认（写请求等）
}
```

### 5.3 CPU 到硬件队列的映射

（[blk-mq-cpumap.c](file:///home/louis/code/linux/block/blk-mq-cpumap.c#L61)）`blk_mq_map_queues()` 建立 CPU 与硬件队列的映射关系：

```c
void blk_mq_map_queues(struct blk_mq_queue_map *qmap)
{
    const struct cpumask *masks;
    masks = group_cpus_evenly(qmap->nr_queues, &nr_masks);
    // 将 CPU 均匀分配到各硬件队列
    for (queue = 0; queue < qmap->nr_queues; queue++) {
        for_each_cpu(cpu, &masks[queue % nr_masks])
            qmap->mq_map[cpu] = qmap->queue_offset + queue;
    }
}
```

**映射策略**：
- `group_cpus_evenly()` 将 CPU 按 NUMA 拓扑均匀分组，确保同一 NUMA 节点的 CPU 映射到同一硬件队列
- 硬件队列总数由 `blk_mq_num_possible_queues()` 确定：`min(online_cpus, max_queues)`
- 如果有 8 个 CPU 和 4 个硬件队列，映射结果类似：`[0,0,1,1,2,2,3,3]`

**查找硬件队列**（[blk-mq.h](file:///home/louis/code/linux/block/blk-mq.h#L83)）：

```c
static inline struct blk_mq_hw_ctx *blk_mq_map_queue_type(
    struct request_queue *q, enum hctx_type type, unsigned int cpu)
{
    return queue_hctx(q, q->tag_set->map[type].mq_map[cpu]);
}
// 等价于：q->tag_set->map[type].mq_map[cpu] → 硬件队列索引
//        → q->queue_hw_ctx[hardware_queue_index] → 硬件队列指针
```

### 5.4 Tag 分配与管理

（[blk-mq-tag.c](file:///home/louis/code/linux/block/blk-mq-tag.c)）Tag 是硬件命令标识符，受 `queue_depth` 限制。Tag 管理是 blk-mq 最核心的并发控制机制之一。

#### 5.4.1 Tag 分配流程

（[blk-mq-tag.c](file:///home/louis/code/linux/block/blk-mq-tag.c#L137)）`blk_mq_get_tag()` 从 `sbitmap_queue` 中分配 tag：

```
blk_mq_get_tag(data)
  ├─ 确定 tag 来源：普通 tag（bitmap_tags）或预留 tag（breserved_tags）
  ├─ __blk_mq_get_tag(data, bt)                 // 尝试快速分配
  │   ├─ sbitmap_queue_get(bt)                   // 原子获取一位
  │   └─ 成功 → 返回 tag
  ├─ 失败，且 BLK_MQ_REQ_NOWAIT → 返回 BLK_MQ_NO_TAG
  └─ 失败，需等待 → 循环等待：
      ├─ blk_mq_run_hw_queue(hctx)               // 先触发一次硬件队列运行，可能释放 tag
      ├─ __blk_mq_get_tag(data, bt)              // 重试
      ├─ sbitmap_prepare_to_wait(bt, ws, &wait)  // 准备睡眠等待
      ├─ __blk_mq_get_tag(data, bt)              // 再试一次（避免竞态）
      ├─ io_schedule()                           // 睡眠等待
      └─ sbitmap_finish_wait(bt, ws, &wait)      // 被唤醒后清理
```

**Tag 两类分配时机**：

| 时机 | 使用的 tag 池 | 阶段 |
|------|-------------|------|
| 请求分配时（有调度器） | `hctx->sched_tags` | `blk_mq_alloc_request()` → `__blk_mq_alloc_requests()` |
| 命令派发时 | `hctx->tags` | `blk_mq_dispatch_rq_list()` → `blk_mq_prep_dispatch_rq()` |

#### 5.4.2 Tag 释放

（[blk-mq-tag.c](file:///home/louis/code/linux/block/blk-mq-tag.c#L230)）`blk_mq_put_tag()` 在请求完成时释放 tag，唤醒等待者：

```c
void blk_mq_put_tag(struct blk_mq_tags *tags, struct blk_mq_ctx *ctx,
                    unsigned int tag)
{
    if (!blk_mq_tag_is_reserved(tags, tag)) {
        sbitmap_queue_clear(&tags->bitmap_tags, tag, ctx->cpu);
        // 唤醒等待 tag 的请求
        if (sbitmap_any_bit_waiting(&tags->bitmap_tags))
            blk_mq_tag_wakeup_all(tags, false);
    }
}
```

**批量释放优化**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L1198)）：`blk_mq_end_request_batch()` 批量收集最多 32 个完成的 tag（`TAG_COMP_BATCH`），一次性调用 `blk_mq_put_tags()` 释放，减少锁操作。

#### 5.4.3 活跃队列跟踪

```c
void __blk_mq_tag_busy(struct blk_mq_hw_ctx *hctx)
{
    // 标记队列为活跃，增加 active_queues 计数
    // 重新计算 sbitmap_queue 的唤醒批次大小
    // 共享 tag 时，活跃队列越多，wake_batch 越大，减少唤醒频率
}
```

这确保共享 tag 集时，多个队列公平分配 tag 资源。

### 5.5 请求分配流程

#### 5.5.1 请求分配入口

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L3047)）`blk_mq_get_new_requests()` 在 `blk_mq_submit_bio` 中被调用，为 bio 分配 request：

```c
static struct request *blk_mq_get_new_requests(struct request_queue *q,
                                               struct blk_plug *plug,
                                               struct bio *bio)
{
    struct blk_mq_alloc_data data = {
        .q        = q,
        .cmd_flags = bio->bi_opf,
        .nr_tags  = 1,
    };
    struct request *rq;

    rq_qos_throttle(q, bio);                    // 1. QoS 限流

    if (plug) {
        data.nr_tags = plug->nr_ios;            // 2. Plug 批量预分配
        plug->nr_ios = 1;                       //    从 plug 中取一个
        data.cached_rqs = &plug->cached_rqs;    //    缓存请求链表
    }

    rq = __blk_mq_alloc_requests(&data);        // 3. 核心分配函数
    if (!rq)
        rq_qos_cleanup(q, bio);
    return rq;
}
```

#### 5.5.2 Plug 缓存请求机制

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L3086)）`blk_mq_peek_cached_request()` 实现请求缓存复用：

```c
static struct request *blk_mq_peek_cached_request(struct blk_plug *plug,
        struct request_queue *q, blk_opf_t opf)
{
    // 1. 检查 plug 中是否有缓存请求
    rq = rq_list_peek(&plug->cached_rqs);
    if (!rq || rq->q != q)
        return NULL;
    // 2. 检查队列类型是否匹配（READ vs DEFAULT）
    if (type != rq->mq_hctx->type &&
        (type != HCTX_TYPE_READ || rq->mq_hctx->type != HCTX_TYPE_DEFAULT))
        return NULL;
    // 3. 检查 flush 标志一致性
    if (op_is_flush(rq->cmd_flags) != op_is_flush(opf))
        return NULL;
    return rq;
}
```

**缓存复用优势**：如果两个连续的 bio 属于同一队列且类型兼容，可直接复用已分配的 request，省去 tag 分配开销。`plug->nr_ios` 控制预分配数量，从 1 开始指数增长。

### 5.6 I/O 提交路径：`blk_mq_submit_bio`

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L3151)）这是块层 I/O 提交的入口，每个 bio 都经过此函数转换为 request 并投递到队列：

```
blk_mq_submit_bio(bio)
  │
  ├─ 1. 获取请求队列
  │     q = bdev_get_queue(bio->bi_bdev)
  │     plug = current->plug
  │
  ├─ 2. 尝试复用 Plug 中缓存的 request
  │     rq = blk_mq_peek_cached_request(plug, q, bio->bi_opf)
  │     if (rq) → blk_mq_use_cached_rq(rq, plug, bio)
  │
  ├─ 3. 队列引用计数
  │     if (!rq) → bio_queue_enter(bio)        // 获取 q_usage_counter，失败则返回
  │
  ├─ 4. 对齐检查
  │     bio_unaligned(bio, q) → submit_bio_noacct_nocheck()
  │
  ├─ 5. I/O 合并尝试
  │     blk_mq_attempt_bio_merge(q, bio, nr_segs)  // Plug 合并
  │     → 成功则 return（bio 已被合并，无需分配 request）
  │
  ├─ 6. 分配 request（如未缓存）
  │     if (!rq) → rq = blk_mq_get_new_requests(q, plug, bio)
  │
  ├─ 7. bio → request 转换
  │     blk_mq_bio_to_request(rq, bio, nr_segs)
  │     // 设置 rq->__sector, rq->__data_len, rq->nr_phys_segments 等
  │
  ├─ 8. 加密/完整性处理
  │     blk_crypto_rq_bio_prep(rq, bio)
  │     blk_integrity_prep(rq)
  │
  ├─ 9. Plug 插入（如果有 plug）
  │     if (plug) {
  │         blk_add_rq_to_plug(plug, rq)        // 尝试合并到 plug 列表
  │         → 成功则 return
  │         // 未合并：放回 cached_rqs 头部，等待下次提交
  │     }
  │
  └─ 10. 直接插入（无 plug 或 plug 已满）
        blk_mq_insert_request(rq, BLK_MQ_INSERT_AT_HEAD)
```

### 5.7 请求插入与派发

#### 5.7.1 请求插入：`blk_mq_insert_request`

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L2624)）根据请求类型和队列配置，选择不同的插入路径：

```
blk_mq_insert_request(rq, flags)
  │
  ├─ blk_rq_is_passthrough(rq)
  │   → blk_mq_request_bypass_insert(rq, flags)  // 直通请求 → dispatch 队列
  │
  ├─ req_op(rq) == REQ_OP_FLUSH
  │   → blk_mq_request_bypass_insert(rq, BLK_MQ_INSERT_AT_HEAD)
  │     // Flush 请求 → dispatch 队列头部，优先执行
  │
  ├─ q->elevator != NULL（有 I/O 调度器）
  │   → elevator->type->ops.insert_requests(hctx, &list, flags)
  │     // 插入调度器内部队列（红黑树、多级队列等）
  │
  └─ 无调度器（none）
      → ctx->rq_lists[hctx->type]（软件队列）
        trace_block_rq_insert(rq)
        list_add(&rq->queuelist, &ctx->rq_lists[hctx->type])
        blk_mq_hctx_mark_pending(hctx, ctx)      // 标记 ctx_map 位
```

**`blk_mq_hctx_mark_pending`**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L73)）：

```c
static void blk_mq_hctx_mark_pending(struct blk_mq_hw_ctx *hctx,
                                     struct blk_mq_ctx *ctx)
{
    const int bit = ctx->index_hw[hctx->type];
    if (!sbitmap_test_bit(&hctx->ctx_map, bit))
        sbitmap_set_bit(&hctx->ctx_map, bit);
}
```

设置 `hctx->ctx_map` 中对应 ctx 的位，后续 `blk_mq_hctx_has_pending()` 可快速检查是否有待派发请求。

#### 5.7.2 硬件队列运行：`blk_mq_run_hw_queue`

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L2353)）触发硬件队列处理待派发请求：

```c
void blk_mq_run_hw_queue(struct blk_mq_hw_ctx *hctx, bool async)
{
    need_run = blk_mq_hw_queue_need_run(hctx);
    if (!need_run) {
        // 再次检查（持锁），避免竞态漏掉请求
        spin_lock(&hctx->queue->queue_lock);
        need_run = blk_mq_hw_queue_need_run(hctx);
        spin_unlock(&hctx->queue->queue_lock);
        if (!need_run) return;
    }

    if (async || !cpumask_test_cpu(smp_processor_id(), hctx->cpumask))
        blk_mq_delay_run_hw_queue(hctx, 0);     // 异步：调度 work
    else
        blk_mq_sched_dispatch_requests(hctx);    // 同步：直接派发
}
```

**异步 vs 同步运行**：
- 同步：当前 CPU 在 `hctx->cpumask` 中，直接调用派发函数
- 异步：通过 `run_work`（delayed_work）在目标 CPU 上执行派发

#### 5.7.3 调度器派发：`blk_mq_sched_dispatch_requests`

（[blk-mq-sched.c](file:///home/louis/code/linux/block/blk-mq-sched.c#L268)）这是真正的派发引擎：

```
__blk_mq_sched_dispatch_requests(hctx)
  │
  ├─ 1. 优先处理 dispatch 队列（之前的未完成请求）
  │     if (!list_empty(&hctx->dispatch))
  │         → blk_mq_dispatch_rq_list(hctx, &hctx->dispatch, ...)
  │
  ├─ 2. 有调度器
  │     if (blk_mq_has_scheduler(q))
  │         blk_mq_do_dispatch_sched(hctx)
  │         // 从调度器内部队列取请求 → dispatch 队列 → 派发
  │
  └─ 3. 无调度器（none）
        blk_mq_do_dispatch_ctx(hctx)
        // 从 ctx 软件队列取请求，Round-Robin 轮转
        for each ctx {
            rq = blk_mq_dequeue_from_ctx(hctx, ctx)
            rq_list_add_tail(&rq_list, rq)
        }
        blk_mq_dispatch_rq_list(hctx, &rq_list, ...)
```

### 5.8 命令派发：`blk_mq_dispatch_rq_list`

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L2117)）将请求列表逐个提交给硬件驱动：

```
blk_mq_dispatch_rq_list(hctx, list, get_budget)
  │
  └─ 循环处理 list 中的每个请求：
      │
      ├─ 1. 准备派发
      │     blk_mq_prep_dispatch_rq(rq, get_budget)
      │     ├─ 获取 tag（如果还没有）
      │     ├─ 获取 budget（SCSI 等需要）
      │     └─ 返回 PREP_DISPATCH_OK / NO_TAG / NO_BUDGET
      │
      ├─ 2. 调用驱动 queue_rq
      │     bd.rq = rq
      │     bd.last = list_empty(list)          // 最后一个设置 last 标志
      │     ret = q->mq_ops->queue_rq(hctx, &bd)
      │     ├─ BLK_STS_OK       → 成功，继续下一个
      │     ├─ BLK_STS_RESOURCE → 资源不足，剩余请求放入 dispatch
      │     ├─ BLK_STS_DEV_RESOURCE → 设备资源不足
      │     └─ other            → 失败，结束请求
      │
      └─ 3. 触发 commit_rqs（如有）
           if (!list_empty(list) || ret != BLK_STS_OK)
               blk_mq_commit_rqs(hctx, queued, false)
           // 通知驱动：已经提交了 queued 个请求，可以提交到硬件了
```

**`bd.last` 的作用**：当 `bd.last == true` 时，驱动知道这是这批的最后一个请求，可以批量提交（如 NVMe 写一次 Doorbell 而不是每个命令都写一次）。

**`queue_rqs` 批量提交**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L3002)）：`blk_mq_dispatch_multiple_queue_requests()` 按 queue 分组请求列表，每组调用 `queue_rqs()` 一次提交多个命令，NVMe 驱动利用此接口实现高效 Doorbell 更新。

### 5.9 完成路径

#### 5.9.1 完成通知：IPI 与 Softirq

当硬件完成命令时，中断处理程序调用 `blk_mq_complete_request()`：

```
中断处理程序
  └─ blk_mq_complete_request(rq)
      │
      └─ blk_mq_complete_request_remote(rq)
          │
          ├─ 本地完成（hctx 只有一个 ctx 且当前 CPU 就是提交 CPU）
          │   → return false → 直接调用 q->mq_ops->complete(rq)
          │
          ├─ 需要 IPI（跨 CPU 完成）
          │   → blk_mq_complete_send_ipi(rq)
          │     llist_add(&rq->ipi_list, &per_cpu(blk_cpu_done, cpu))
          │     smp_call_function_single_async(cpu, &per_cpu(blk_cpu_csd, cpu))
          │     → 目标 CPU 执行 blk_complete_reqs() → complete(rq)
          │
          └─ 单队列或本 CPU
              → blk_mq_raise_softirq(rq)
                llist_add(&rq->ipi_list, this_cpu_ptr(&blk_cpu_done))
                raise_softirq(BLOCK_SOFTIRQ)
                → BLOCK_SOFTIRQ 处理 → blk_complete_reqs() → complete(rq)
```

**设计原则**：请求尽量在提交它的 CPU 上完成，保持缓存局部性。`blk_cpu_done` 是 per-CPU 的 llist（lockless linked list），实现无锁完成通知。

#### 5.9.2 请求终结：`blk_mq_end_request`

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L1177)）驱动完成命令后，调用此函数终结请求：

```c
void blk_mq_end_request(struct request *rq, blk_status_t error)
{
    if (blk_update_request(rq, error, blk_rq_bytes(rq)))
        BUG();                          // 更新已传输字节数
    __blk_mq_end_request(rq, error);    // 释放 tag，通知上层
}
```

**批量完成**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L1198)）：`blk_mq_end_request_batch()` 批量处理完成请求，最多收集 32 个 tag 后一次性释放，大幅减少锁操作。

### 5.10 硬件队列管理

#### 5.10.1 队列冻结与静默

```
blk_mq_freeze_queue(q)                  // 冻结队列：阻止新请求进入
  └─ blk_freeze_queue_start(q)
      └─ blk_mq_freeze_queue_wait(q)    // 等待所有进行中的请求完成

blk_mq_unfreeze_queue(q)                // 解冻

blk_mq_quiesce_queue(q)                 // 静默队列：暂停硬件队列派发
  └─ blk_mq_quiesce_queue_nowait(q)     // 不等完成

blk_mq_unquiesce_queue(q)               // 恢复

blk_mq_wait_quiesce_done(tag_set)       // 等待静默完成
```

**Freeze vs Quiesce**：
- **Freeze**：使用 `mq_freeze_depth` 和 percpu refcount，阻止新请求，用于驱动更新、设备移除
- **Quiesce**：使用 `BLK_MQ_S_STOPPED` 标志，暂停硬件队列派发但保留请求，用于快速暂停/恢复

#### 5.10.2 队列状态标志

```c
// hctx->state 标志
BLK_MQ_S_STOPPED         // 硬件队列已停止（不派发）
BLK_MQ_S_TAG_ACTIVE      // tag 池活跃
BLK_MQ_S_SCHED_RESTART   // 调度器需要重启

// hctx->flags 标志
BLK_MQ_F_SHOULD_MERGE    // 应尝试合并
BLK_MQ_F_TAG_QUEUE_SHARED // tag 队列共享
BLK_MQ_F_BLOCKING        // 阻塞型硬件队列
BLK_MQ_F_NO_SCHED        // 无调度器
BLK_MQ_F_STACKING        // 堆叠设备（如 DM）
BLK_MQ_F_TAG_HCTX_SHARED // 跨 hctx 共享 tag
BLK_MQ_F_BLOCKING        // 驱动 queue_rq 可能阻塞
BLK_MQ_F_NO_SCHED_BY_DEFAULT  // 默认不使用调度器
```

### 5.11 多队列调度框架（blk-mq-sched）

（[blk-mq-sched.c](file:///home/louis/code/linux/block/blk-mq-sched.c)）在 blk-mq 和 I/O 调度器之间建立桥梁：

#### 5.11.1 调度器初始化

```c
int blk_mq_init_sched(struct request_queue *q, struct elevator_type *e)
{
    // 1. 分配 sched_tags（调度器 tag 池）
    // 2. 调用 elevator->init_sched(hctx) 初始化各 hctx 的调度器数据
    // 3. 设置 q->elevator = e
}

void blk_mq_sched_free_rqs(struct request_queue *q)
{
    // 释放所有调度器管理的请求
}
```

#### 5.11.2 调度器请求插入

```c
void blk_mq_sched_insert_request(struct request *rq, bool at_head,
                                  bool run_queue, bool async)
{
    // 1. 如果 rq 有 tag 且不在调度器中 → 直接派发
    // 2. 否则 → elevator->insert_requests(hctx, &list, flags)
}
```

#### 5.11.3 调度器重启

当请求完成释放 tag 后，调度器可能需要重启以处理之前因资源不足而排队的请求：

```c
void blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx)
{
    if (test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state))
        __blk_mq_sched_restart(hctx);
}

void __blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx)
{
    clear_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state);
    smp_mb();  // 内存屏障，与 blk_mq_dispatch_rq_list 中的检查配对
    blk_mq_run_hw_queue(hctx, true);
}
```

### 5.12 辅助模块总览

| 文件 | 行数 | 功能 |
|------|------|------|
| blk-mq.c | 5,365 | 多队列核心框架：请求分配、插入、派发、完成 |
| blk-mq-tag.c | 651 | Tag 分配与管理，基于 sbitmap 的无锁位图分配器 |
| blk-mq-sched.c | 706 | 调度框架：桥接 blk-mq 和 I/O 调度器 |
| blk-mq-cpumap.c | 438 | CPU 到硬件队列的映射表生成 |
| blk-mq-dma.c | 438 | 多队列 DMA 映射辅助（SG 列表构建） |
| blk-mq-sysfs.c | 298 | sysfs 属性导出（/sys/block/xxx/mq/） |
| blk-mq-debugfs.c | 816 | debugfs 调试接口（/sys/kernel/debug/block/xxx/） |
| blk-mq.h | 159 | 内部头文件（ctx、hctx 定义，辅助函数） |

### 5.13 完整 I/O 生命周期（blk-mq 视角）

```
用户态 write(fd, buf, count)
  → vfs_write()
  → ext4_file_write_iter()
  → submit_bio(bio)                          // 文件系统生成 bio
    → submit_bio_noacct(bio)
      → blk_mq_submit_bio(bio)              // [5.6] blk-mq 入口
        ├─ blk_mq_peek_cached_request()     // [5.5.2] 复用 Plug 缓存请求
        ├─ blk_mq_attempt_bio_merge()       // 尝试合并到 plug 中的请求
        ├─ blk_mq_get_new_requests()        // [5.5.1] 分配新 request
        │   └─ __blk_mq_alloc_requests()
        │       └─ blk_mq_get_tag()         // [5.4] 获取 tag
        ├─ blk_mq_bio_to_request()          // bio → request 转换
        ├─ blk_add_rq_to_plug()             // 添加到 plug 列表
        │
        └─ [plug 刷新时]
          blk_mq_flush_plug_list()
            ├─ blk_mq_insert_request()       // [5.7.1] 插入请求
            │   ├─ pass-through → hctx->dispatch
            │   ├─ flush → hctx->dispatch (头部)
            │   ├─ 有调度器 → elevator->insert_requests()
            │   └─ 无调度器 → ctx->rq_lists[hctx->type]
            │
            └─ blk_mq_run_hw_queue()        // [5.7.2] 触发派发
              └─ blk_mq_sched_dispatch_requests()  // [5.7.3]
                └─ blk_mq_dispatch_rq_list()       // [5.8] 派发
                  └─ q->mq_ops->queue_rq(hctx, &bd) // 驱动提交命令
                    └─ nvme_queue_rq()             // NVMe 写入 SQ

  [... 硬件处理 ...]

  中断 → nvme_irq()
    → nvme_process_cq()
      → blk_mq_complete_request(rq)         // [5.9.1] 完成通知
        ├─ blk_mq_complete_request_remote()
        │   ├─ IPI → 目标 CPU → q->mq_ops->complete(rq)
        │   └─ softirq → BLOCK_SOFTIRQ → complete(rq)
        │
        └─ q->mq_ops->complete(rq)
          → nvme_complete_rq()
            → blk_mq_end_request(rq, error)  // [5.9.2] 请求终结
              ├─ blk_update_request()        // 更新传输字节数
              ├─ blk_mq_put_tag()            // [5.4.2] 释放 tag
              └─ blk_mq_finish_request()     // 通知上层（bio->bi_end_io）
```

---

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

**默认策略**（[elevator.c](file:///home/louis/code/linux/block/elevator.c#L727)）：

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

**切换流程**（[elevator.c](file:///home/louis/code/linux/block/elevator.c#L570)）：

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

**插入路径**（直接入队，无调度器参与）（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L2625)）：

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

**派发路径**（无调度器时直接从 ctx 队列取请求）（[blk-mq-sched.c](file:///home/louis/code/linux/block/blk-mq-sched.c#L268)）：

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

**核心数据结构**（[blkdev.h](file:///home/louis/code/linux/include/linux/blkdev.h#L1172)）：

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

**blk_start_plug_nr_ios**（[blk-core.c](file:///home/louis/code/linux/block/blk-core.c#L1221)）：
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

**blk_add_rq_to_plug**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L1409)）：
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

**blk_mq_flush_plug_list**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L2970)）— 核心派发逻辑：

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

在 `blk_mq_submit_bio` 中，bio 提交后会先尝试合并到 plug 列表中的已有请求（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L1085)）：

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

**文件系统 DIO 路径**（[fops.c](file:///home/louis/code/linux/block/fops.c#L209)）— 典型用法：

```c
blk_start_plug(&plug);

for (;;) {
    // 构造 bio ...
    submit_bio(bio);           // bio → request → blk_add_rq_to_plug()
    // 分配下一个 bio ...
}

blk_finish_plug(&plug);       // 批量提交所有请求
```

**直接 I/O 提交**（[blk-execute_rq_nowait](file:///home/louis/code/linux/block/blk-mq.c#L1453)）— 直通请求也使用 plug：

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

**bio 可合并**（[blk-mq-sched.h](file:///home/louis/code/linux/block/blk-mq-sched.h#L75)）：
```c
static inline bool bio_mergeable(struct bio *bio)
{
    return !(bio->bi_opf & REQ_NOMERGE_FLAGS);  // 检查 REQ_NOMERGE 标志
}
```

**request 可合并**（[blk.h](file:///home/louis/code/linux/block/blk.h#L161)）：
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

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L898)）检查以下条件：

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

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L924)）判断 bio 与 request 的合并方向：

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

最常见的合并场景（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L944)）：

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

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L975)）与后向对称，但 bio 插入到 request 头部：

```c
bio->bi_next = req->bio;          // bio 指向原头部
req->bio = bio;                   // 头部更新为 bio
req->__sector = bio->bi_iter.bi_sector;  // 起始扇区前移
req->__data_len += bio->bi_iter.bi_size;
```

### 7.5 Request 合并流程：`attempt_merge`

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L777)）在调度器中将两个已存在的 request 合并：

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
| 1. Plug 合并 | `blk_attempt_plug_merge` | blk-merge.c:1085 | 合并到 plug 列表中已有的 request |
| 2. 调度器合并 | `blk_mq_sched_try_merge` | blk-merge.c:1141 | 通过 `elv_merge()` 查找调度器中的可合并 request |
| 3. 调度器 bio 合并 | `blk_mq_sched_bio_merge` | blk-mq-sched.c | 以 bio 为单位尝试合并到调度器中的 request |
| 4. bio 列表合并 | `blk_bio_list_merge` | blk-merge.c:1116 | 在 bio 列表（倒序最多 8 个）中查找合并 |
| 5. Request 后向合并 | `attempt_back_merge` | blk-merge.c:865 | 在调度器中与后一个 request 合并 |
| 6. Request 前向合并 | `attempt_front_merge` | blk-merge.c:876 | 在调度器中与前一个 request 合并 |

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

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L489)）重新计算一个 request 的物理段数：

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

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L284)）判断一个 bio_vec 是否需要被拆分为多个段：

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

（[blk-mq-dma.c](file:///home/louis/code/linux/block/blk-mq-dma.c#L287)）将 request 的 bio 链转换为 scatter-gather 列表：

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

（[blkdev.h](file:///home/louis/code/linux/include/linux/blkdev.h#L370)）影响合并与分段的关键限制：

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

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L51)）某些设备（如 SATA AHCI）要求 SG 列表中的段之间不能有太大的物理间隙。如果前一个 bio 的最后一个 bvec 的物理地址与后一个 bio 的第一个 bvec 之间的偏移跨越了 `virt_boundary_mask` 边界，则不能合并。

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

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L481)）将 bio 按队列限制拆分：

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

（[blk-merge.c](file:///home/louis/code/linux/block/blk-merge.c#L231)）根据操作类型和队列限制计算最大扇区数：

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

内核中 NVMe 驱动将块层 FUA 标志映射到 NVMe 命令（[core.c](file:///home/louis/code/linux/drivers/nvme/host/core.c#L997)）：

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

（[blk.h](file:///home/louis/code/linux/block/blk.h#L34)）每个硬件队列（`blk_mq_hw_ctx`）拥有一个刷新队列：

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

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c#L74)）一个请求的刷新序列是 PREFLUSH → DATA → POSTFLUSH 的子集：

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

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c#L308)）这是刷新状态机的入口。根据请求的标志和设备能力，决定需要执行哪些阶段：

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

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c#L218)）当刷新状态变化时，检查是否需要下发新的刷新命令：

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

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c#L175)）刷新命令完成时的回调：

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

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c#L129)）每个阶段完成后，推进到下一个阶段：

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

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c#L268)）当 DATA 阶段完成时触发：

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

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L2642)）在 `blk_mq_insert_request` 中，FLUSH 请求被特殊处理：

```c
if (req_op(rq) == REQ_OP_FLUSH) {
    // 直接插入 hctx->dispatch 队列头部
    // 好处：在 NCQ 设备上，FLUSH 是非 NCQ 命令，插入头部可以减少延迟
    blk_mq_request_bypass_insert(rq, BLK_MQ_INSERT_AT_HEAD);
}
```

#### 8.11.2 刷新请求的 requeue 路径

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c#L1574)）`blk_mq_requeue_work` 分别处理 `requeue_list` 和 `flush_list`：

```
blk_mq_requeue_work(work)
  ├─ 从 requeue_list 取出请求 → blk_mq_insert_request()
  └─ 从 flush_list 取出请求 → blk_mq_insert_request()
  └─ blk_mq_run_hw_queues(q, false)
```

### 8.12 NVMe 驱动中的 FUA 支持

（[core.c](file:///home/louis/code/linux/drivers/nvme/host/core.c#L2393)）NVMe 设备通过 `Identify Controller` 的 VWC（Volatile Write Cache）字段声明支持：

```c
if ((ns->ctrl->vwc & NVME_CTRL_VWC_PRESENT) && !info->no_vwc)
    lim.features |= BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA;
else
    lim.features &= ~(BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA);
```

（[core.c](file:///home/louis/code/linux/drivers/nvme/host/core.c#L997)）在构造 NVMe 命令时，FUA 标志直接映射到 NVMe 协议：

```c
if (req->cmd_flags & REQ_FUA)
    control |= NVME_RW_FUA;  // 设置 NVMe 命令的 FUA 位
```

NVMe 的 FUA 位指示控制器将数据直接写入非易失性介质，无需额外的 FLUSH 命令。

### 8.13 用户态接口

（[blk-flush.c](file:///home/louis/code/linux/block/blk-flush.c#L348)）`blkdev_issue_flush()` 让内核其他子系统（如 fsync、journal）发出刷新：

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

### 9.1 blk-rq-qos.c — QoS 框架（374 行）

文件：`block/blk-rq-qos.c`

提供通用的请求 QoS 框架，以链表形式组织多个 QoS 策略：

- `rq_qos` 结构通过 `next` 指针链式连接多个策略。
- 钩子函数：`throttle`、`track`、`issue`、`done`、`requeue`、`cleanup`。
- `rq_wait_inc_below()` — 原子增加 inflight 计数，用于限制并发请求数。

### 9.2 blk-throttle.c — 带宽节流（1,849 行）

文件：`block/blk-throttle.c`

实现基于 cgroup 的块 I/O 带宽限制：
- 限制 IOPS（每秒 I/O 操作数）和 BPS（每秒字节数）。
- 通过令牌桶（Token Bucket）算法实现平滑限流。
- 支持读写分离的带宽限制。

### 9.3 blk-iolatency.c — 延迟控制（1,068 行）

文件：`block/blk-iolatency.c`

基于 cgroup 的 I/O 延迟目标控制：
- 为每个 cgroup 设置 I/O 延迟目标。
- 当延迟超过目标时，自动限制该 cgroup 的 I/O 并发量。
- 使用比例缩放算法动态调整并发限制。

### 9.4 blk-iocost.c — IO 成本模型（3,551 行）

文件：`block/blk-iocost.c`

基于 IO 成本模型的控制器（块层第三大文件），核心思想：

- **成本模型**：将 I/O 成本量化为设备时间。默认采用线性模型，将 I/O 分为顺序/随机两类，各赋予不同成本。
- **虚拟时间（vtime）**：作为主要控制指标，实现按权重比例分配设备时间。
- **控制策略**：
  - Vtime 分配：根据 cgroup 权重计算分层的份额。
  - 带有自适应调节能力，可通过 `tools/cgroup/iocost_coef_gen.py` 生成设备特定参数。

### 9.5 blk-wbt.c — 写回节流（1,025 行）

文件：`block/blk-wbt.c`

Writeback Throttling（写回节流），通过监控写请求的延迟来限制缓冲写（buffered write）的速率，防止写请求堆积导致高延迟。

### 9.6 blk-ioprio.c — I/O 优先级

文件：`block/blk-ioprio.c`

支持基于 cgroup 的 I/O 优先级管理，允许将 I/O 优先级类（RT、BE、IDLE）传递给底层调度器和驱动。

---

## 10. Cgroup 集成

### 10.1 blk-cgroup.c（2,250 行）

文件：`block/blk-cgroup.c`

通用块 IO cgroup 接口，是整个块层 cgroup 的基础设施：

- **策略注册**：`blkcg_policy[]` 数组支持最多 `BLKCG_MAX_POLS` 个策略，由 `blkcg_pol_mutex` 保护。
- **rstat 刷新**：`__blkcg_rstat_flush()` 刷新 cgroup 统计信息。
- **根 cgroup**：`blkcg_root` 全局根 cgroup。
- 支持 `blkcg_debug_stats` 调试统计。

### 10.2 辅助 cgroup 文件

| 文件 | 行数 | 功能 |
|------|------|------|
| blk-cgroup-rwstat.c | 385 | 读写统计的 cgroup 接口 |
| blk-cgroup-fc-appid.c | 316 | FC（Fibre Channel）应用 ID |
| blk-cgroup.h | 503 | cgroup 内部头文件 |
| blk-cgroup-rwstat.h | — | 读写统计头文件 |

---

## 11. 分区管理

### 11.1 分区核心（block/partitions/core.c, 732 行）

文件：`block/partitions/core.c`

负责分区表的检测和解析，通过 `check_part[]` 函数指针数组按优先级顺序尝试各种分区格式：

- 优先检测磁盘地址 0 处的分区表格式（有 ADFS 引导块）
- 然后检测磁盘地址 0xDC0 处的分区信息
- 内核命令行分区（`cmdline_partition`）
- 设备树分区（`of_partition`）
- EFI/GPT 分区

### 11.2 支持的分区格式

| 文件 | 行数 | 分区格式 |
|------|------|----------|
| msdos.c | 717 | DOS/MBR 分区表 |
| efi.c | 756 | EFI/GPT 分区表 |
| ldm.c | 1,487 | Windows 动态磁盘（LDM） |
| acorn.c | 550 | Acorn 分区 |
| cmdline.c | 385 | 内核命令行分区 |
| ibm.c | 414 | IBM S/390 分区 |
| aix.c | 282 | AIX 分区 |
| mac.c | — | Apple Macintosh 分区 |
| sun.c | — | Sun Solaris 分区 |
| sgi.c | — | SGI 分区 |
| atari.c | — | Atari 分区 |
| amiga.c | — | Amiga 分区 |
| osf.c | — | OSF/Unix 分区 |
| karma.c | — | Karma 分区 |
| sysv68.c | — | SysV 68 分区 |
| ultrix.c | — | Ultrix 分区 |
| of.c | — | Open Firmware 分区 |

---

## 12. 数据完整性与加密

### 12.1 数据完整性（DIF/DIX）

| 文件 | 行数 | 功能 |
|------|------|------|
| blk-integrity.c | 342 | 块层数据完整性扩展，支持 DIF/DIX（Data Integrity Field/Extension） |
| bio-integrity.c | 488 | Bio 级别的完整性元数据处理 |
| bio-integrity-auto.c | — | 自动完整性元数据生成 |
| t10-pi.c | 473 | T10 保护信息（Protection Information）处理 |

### 12.2 块层加密

| 文件 | 行数 | 功能 |
|------|------|------|
| blk-crypto.c | 582 | 块层内联加密核心 |
| blk-crypto-profile.c | 662 | 加密配置文件管理 |
| blk-crypto-fallback.c | 679 | 软件加密回退实现 |
| blk-crypto-sysfs.c | — | 加密 sysfs 接口 |
| blk-crypto-internal.h | — | 加密内部头文件 |

### 12.3 SED/Opal 加密

| 文件 | 行数 | 功能 |
|------|------|------|
| sed-opal.c | 3,351 | TCG Opal 自加密驱动器（SED）支持 |
| opal_proto.h | 485 | Opal 协议定义 |

---

## 13. Zoned 块设备

### 13.1 blk-zoned.c（2,363 行）

文件：`block/blk-zoned.c`

实现 Zoned Block Device（ZBD）支持，包括：

- **区域状态管理**：定义了 9 种区域状态（`zone_cond_name[]`）：
  - `NOT_WP`（非写指针）、`EMPTY`、`IMP_OPEN`（隐式打开）、`EXP_OPEN`（显式打开）、`CLOSED`、`READONLY`、`FULL`、`OFFLINE`、`ACTIVE`
- **Zone Write Plug**：每个 zone 一个写插件，通过哈希表管理，支持 BIO 插件化处理。
- Zone Append 操作支持。
- Zone Reset 和 Zone Finish 管理。

---

## 14. Sysfs 与调试接口

### 14.1 blk-sysfs.c（1,030 行）

文件：`block/blk-sysfs.c`

通过 sysfs 导出块层队列属性，每个属性由 `struct queue_sysfs_entry` 描述，包含 `show` 和 `store` 函数。导出的属性包括：

- `nr_requests` — 队列最大请求数
- `read_ahead_kb` — 预读大小
- `max_sectors_kb` — 最大传输大小
- `scheduler` — 当前 I/O 调度器
- `iostats` — I/O 统计开关
- `rq_affinity` — 请求 CPU 亲和性

### 14.2 其他接口

| 文件 | 行数 | 功能 |
|------|------|------|
| ioctl.c | 975 | 块设备 ioctl 系统调用处理 |
| fops.c | 978 | 块设备文件操作（open, release, read_iter, write_iter） |
| bsg.c | 277 | 块 SCSI 通用（BSG）驱动接口 |
| bsg-lib.c | 412 | BSG 库函数 |
| disk-events.c | 489 | 磁盘事件通知（介质变更等） |
| holder.c | — | 块设备持有者（holder）管理 |
| early-lookup.c | 316 | 早期启动时的块设备查找 |

---

## 15. 其他辅助模块

### 15.1 超时与电源管理

| 文件 | 行数 | 功能 |
|------|------|------|
| blk-timeout.c | — | 请求超时处理 |
| blk-pm.c | — | 块层电源管理（runtime PM） |
| blk-pm.h | — | 电源管理头文件 |

### 15.2 统计与跟踪

| 文件 | 行数 | 功能 |
|------|------|------|
| blk-stat.c | — | 块层统计基础设施 |
| blk-stat.h | — | 统计头文件 |

### 15.3 其他

| 文件 | 行数 | 功能 |
|------|------|------|
| blk-ioc.c | 442 | I/O 上下文（io_context）管理 |
| ioprio.c | 249 | I/O 优先级系统调用接口 |
| blk-ia-ranges.c | 314 | 独立访问范围（Independent Access Ranges） |
| badblocks.c | 1,550 | 坏块记录与管理 |

---

## 16. 总结

### 16.1 架构层次

Linux 7.0 块层呈现清晰的层次结构：

```
用户空间 (dd, fio, etc.)
        │
        ▼
┌──────────────────────────────────────┐
│  VFS / 文件系统层                      │
│  (submit_bio)                        │
├──────────────────────────────────────┤
│  Bio 层 (bio.c, blk-merge.c)         │  ← I/O 描述单元
├──────────────────────────────────────┤
│  请求队列 (blk-core.c, blk-mq.c)     │  ← 核心调度
├──────────────────────────────────────┤
│  QoS 层 (rq-qos 链表)                 │  ← 策略链
│  ├─ blk-throttle  ├─ blk-iolatency   │
│  ├─ blk-iocost    └─ blk-wbt         │
├──────────────────────────────────────┤
│  I/O 调度器 (elevator.c)              │  ← 排序/合并
│  ├─ BFQ  ├─ MQ-Deadline  └─ Kyber    │
├──────────────────────────────────────┤
│  blk-mq 多队列 (blk-mq.c)            │  ← 多队列派发
├──────────────────────────────────────┤
│  块设备驱动 (NVMe, SCSI, virtio, etc.)│
└──────────────────────────────────────┘
```

### 16.2 代码量分布

| 子系统 | 代码量 | 占比 |
|--------|--------|------|
| BFQ 调度器 | ~12,000 行 | 16.8% |
| blk-mq 多队列 | ~8,000 行 | 11.2% |
| QoS 控制 | ~7,500 行 | 10.5% |
| Cgroup 集成 | ~3,500 行 | 4.9% |
| 分区管理 | ~5,000 行 | 7.0% |
| 加密/完整性 | ~6,000 行 | 8.4% |
| Bio 核心 | ~3,000 行 | 4.2% |
| 其他辅助 | ~26,400 行 | 37.0% |

### 16.3 关键设计特点

1. **多队列架构**：blk-mq 是块层的核心，通过 per-CPU 软件队列和硬件队列映射实现高并发、低锁竞争。
2. **QoS 策略链**：通过 `rq_qos` 链表实现灵活的策略组合，支持节流、延迟控制、成本模型和写回节流。
3. **丰富的调度器**：BFQ（侧重公平性和低延迟）、MQ-Deadline（侧重简单和低开销）、Kyber（侧重延迟控制）。
4. **完整的分区支持**：支持 18 种分区格式，覆盖主流和历史操作系统。
5. **硬件安全**：内联加密（blk-crypto）和 TCG Opal SED 支持，提供端到端数据保护。
6. **Zoned 设备**：完整的 ZNS/SMR 支持，包含 zone write plug 机制。

---

## 17. NVMe 驱动：块设备注册与移除流程

NVMe（Non-Volatile Memory Express）是当前最主流的 SSD 接口协议。Linux NVMe 驱动位于 `drivers/nvme/` 目录，分为 `host/`（主机端）、`target/`（目标端）、`common/`（公共代码）三个子目录。本章以 PCIe NVMe 驱动为主线，分析块设备注册与移除的完整流程。

### 17.1 关键数据结构

#### 17.1.1 nvme_ctrl — NVMe 控制器

文件：`drivers/nvme/host/nvme.h` L334-L460

```c
struct nvme_ctrl {
    bool comp_seen;
    bool identified;                         // 是否已完成 Identify 初始化
    enum nvme_ctrl_state state;              // 控制器状态: LIVE/RESETTING/CONNECTING/DELETING/DEAD
    spinlock_t lock;
    struct mutex scan_lock;                  // 保护扫描过程的互斥锁
    const struct nvme_ctrl_ops *ops;         // 传输层操作函数集（PCI/TCP/RDMA/FC）
    struct request_queue *admin_q;           // Admin 命令队列
    struct request_queue *connect_q;         // 连接命令队列（Fabrics）
    struct request_queue *fabrics_q;         // Fabrics 命令队列
    struct device *dev;                      // 指向 PCI 设备的 device
    int instance;                            // 控制器实例编号（如 nvme0）
    int numa_node;                           // NUMA 节点
    struct blk_mq_tag_set *tagset;           // IO 队列的 tag set
    struct blk_mq_tag_set *admin_tagset;     // Admin 队列的 tag set
    struct list_head namespaces;             // 该控制器上的所有 namespace 链表
    struct mutex namespaces_lock;            // namespaces 链表保护锁
    struct srcu_struct srcu;                 // 保护 namespaces 遍历的 SRCU
    struct device ctrl_device;               // 控制器 device（/sys/class/nvme/）
    struct device *device;                   // 字符设备 device（/dev/nvme0）
    struct cdev cdev;                        // 字符设备结构体
    struct work_struct reset_work;           // 复位工作项
    struct work_struct delete_work;          // 删除工作项
    struct nvme_subsystem *subsys;           // 所属子系统
    struct opal_dev *opal_dev;               // TCG Opal 自加密设备
    u16 cntlid;                              // 控制器 ID
    u32 ctrl_config;                         // CC 寄存器配置
    u32 queue_count;                         // 已创建的队列数
    u64 cap;                                 // CAP 寄存器
    u32 max_hw_sectors;                      // 最大硬件扇区数
    u32 max_segments;                        // 最大段数
    u32 max_namespaces;                      // 最大 namespace 数
    u8 vwc;                                  // 易失性写缓存
    u32 vs;                                  // 版本号
    struct work_struct scan_work;            // 扫描 namespace 的工作项
    struct work_struct async_event_work;     // 异步事件处理
    struct delayed_work ka_work;             // Keep-Alive 定时器
    unsigned long events;                    // AER 事件位图
    struct nvme_fault_inject fault_inject;   // 故障注入
};
```

**控制器状态机**（`enum nvme_ctrl_state`）：

```
NVME_CTRL_NEW ──► NVME_CTRL_CONNECTING ──► NVME_CTRL_LIVE
                       │       ▲                    │
                       ▼       │                    ▼
                 NVME_CTRL_RESETTING          NVME_CTRL_DELETING
                                                  │
                                                  ▼
                                          NVME_CTRL_DELETING_NOIO
                                                  │
                                                  ▼
                                           NVME_CTRL_DEAD
```

#### 17.1.2 nvme_ns_head — Namespace 头部

文件：`drivers/nvme/host/nvme.h` L526-L575

```c
struct nvme_ns_head {
    struct list_head list;                   // 链接到 subsystem 的 nsheads 链表
    struct srcu_struct srcu;                 // SRCU 保护
    struct nvme_subsystem *subsys;           // 所属子系统
    struct nvme_ns_ids ids;                  // 唯一标识符（eui64/nguid/uuid/csi）
    u8 lba_shift;                            // LBA 偏移（扇区大小 = 2^lba_shift）
    u16 ms;                                  // 元数据大小
    u16 pi_size;                             // 保护信息大小
    u8 pi_type;                              // 保护信息类型
    u8 guard_type;                           // 保护类型
    struct list_head entry;                  // 链接到 subsystem 的 nsheads
    struct kref ref;                         // 引用计数
    bool shared;                             // 是否共享 namespace
    u64 nuse;                                // 已使用的 LBA 数
    unsigned ns_id;                          // Namespace ID
    int instance;                            // 实例编号
    struct gendisk *disk;                    // 多路径聚合盘（仅多路径模式）
    unsigned long features;                  // 特性位图
    struct cdev cdev;                        // 字符设备（/dev/ngXnY）
    struct device cdev_device;               // 字符设备 device
};
```

#### 17.1.3 nvme_ns — Namespace 实例

文件：`drivers/nvme/host/nvme.h` L585-L610

```c
struct nvme_ns {
    struct list_head list;                   // 链接到 ctrl->namespaces 链表
    struct nvme_ctrl *ctrl;                  // 所属控制器
    struct request_queue *queue;             // 块层请求队列
    struct gendisk *disk;                    // 通用磁盘（gendisk）结构体
    struct list_head siblings;               // 链接到 ns_head->list（同一 ns 的多个路径）
    struct kref kref;                        // 引用计数
    struct nvme_ns_head *head;              // 指向 namespace 头部
    unsigned long flags;                     // 标志位：
#define NVME_NS_REMOVING           0         //   正在移除
#define NVME_NS_ANA_PENDING        2         //   ANA 状态待更新
#define NVME_NS_FORCE_RO           3         //   强制只读
#define NVME_NS_READY              4         //   namespace 就绪
    struct cdev cdev;                        // 字符设备
    struct device cdev_device;               // 字符设备 device
    struct nvme_fault_inject fault_inject;   // 故障注入
};
```

#### 17.1.4 nvme_dev — PCIe 设备（含控制器）

文件：`drivers/nvme/host/pci.c` L294-L368

```c
struct nvme_dev {
    struct nvme_queue *queues;               // 队列数组（queues[0]=admin queue）
    struct blk_mq_tag_set tagset;            // IO 队列的 tag set
    struct blk_mq_tag_set admin_tagset;      // Admin 队列的 tag set
    u32 __iomem *dbs;                        // Doorbell 寄存器基址
    struct device *dev;                      // 指向 PCI 设备
    unsigned online_queues;                  // 已上线的队列数
    unsigned max_qid;                        // 最大队列 ID
    unsigned io_queues[HCTX_MAX_TYPES];      // 各类型 IO 队列数
    unsigned int num_vecs;                   // 中断向量数
    u32 q_depth;                             // IO 队列深度
    int io_sqes;                             // IO SQ Entry 大小
    u32 db_stride;                           // Doorbell 步长
    void __iomem *bar;                       // MMIO BAR 地址
    struct mutex shutdown_lock;              // 关断锁
    struct nvme_ctrl ctrl;                   // 嵌入的控制器结构体（通过 container_of 获取）
    u32 last_ps;                             // 上一次电源状态
    mempool_t *dmavec_mempool;               // DMA vec 内存池
    // ... 门铃缓冲区、主机内存缓冲区等
};
```

#### 17.1.5 nvme_queue — 硬件队列

文件：`drivers/nvme/host/pci.c` L365-L414

```c
struct nvme_queue {
    struct nvme_dev *dev;                    // 所属设备
    spinlock_t sq_lock;                      // 提交队列自旋锁
    void *sq_cmds;                           // 提交队列命令缓冲区
    spinlock_t cq_poll_lock;                 // 完成队列轮询锁
    struct nvme_completion *cqes;            // 完成队列条目数组（DMA 一致性内存）
    dma_addr_t sq_dma_addr;                  // 提交队列 DMA 地址
    dma_addr_t cq_dma_addr;                  // 完成队列 DMA 地址
    u32 __iomem *q_db;                       // 队列门铃寄存器地址
    u32 q_depth;                             // 队列深度
    u16 cq_vector;                           // 完成队列中断向量
    u16 sq_tail;                             // 提交队列尾指针
    u16 cq_head;                             // 完成队列头指针
    u16 qid;                                 // 队列 ID
    u8 cq_phase;                             // 完成队列阶段位（Phase Tag）
    u8 sqes;                                 // 提交队列条目大小（2^sqes 字节）
    unsigned long flags;                     // 标志位：
#define NVMEQ_ENABLED              0         //   队列已启用
#define NVMEQ_SQ_CMB               1         //   提交队列在 CMB 中
#define NVMEQ_POLLED               3         //   轮询队列
};
```

#### 17.1.6 nvme_ns_info — Namespace 扫描信息

文件：`drivers/nvme/host/core.c` L40-L52

```c
struct nvme_ns_info {
    struct nvme_ns_ids ids;                  // 唯一标识符
    u32 nsid;                                // Namespace ID
    __le32 anagrpid;                         // ANA 组 ID
    u8 pi_offset;                            // 保护信息偏移
    u16 endgid;                              // 耐久性组 ID
    u64 runs;                                // 可恢复单元数
    bool is_shared;                          // 是否共享
    bool is_readonly;                        // 是否只读
    bool is_ready;                           // 是否就绪
    bool is_removed;                         // 是否已移除
    bool is_rotational;                      // 是否为旋转介质
    bool no_vwc;                             // 无易失性写缓存
};
```

---

### 17.2 注册流程（Probe）

当 NVMe PCIe 设备被 PCI 子系统枚举到时，触发 `nvme_probe()`。整个注册流程分为 **控制器初始化** 和 **Namespace 扫描** 两大阶段。

#### 17.2.1 注册流程总览

```
nvme_probe()                                      [pci.c]
  │
  ├─ 1. nvme_pci_alloc_dev()                      分配 nvme_dev + 初始化 controller
  │
  ├─ 2. nvme_add_ctrl()                           注册字符设备 /dev/nvmeX
  │     └─ cdev_init(&ctrl->cdev, &nvme_dev_fops) 设置字符设备 fops
  │     └─ cdev_device_add()                      添加 cdev + device 到内核
  │
  ├─ 3. nvme_dev_map()                            MMIO BAR 地址映射
  │
  ├─ 4. nvme_pci_alloc_iod_mempool()             DMA 向量内存池分配
  │
  ├─ 5. nvme_pci_enable()                         PCI 设备使能（pci_enable_device + 中断）
  │
  ├─ 6. nvme_alloc_admin_tag_set()               分配 Admin Tag Set
  │     └─ blk_mq_alloc_tag_set()                分配 blk-mq tag set（bitmap 管理）
  │
  ├─ 7. nvme_change_ctrl_state(CONNECTING)      状态 → CONNECTING
  │
  ├─ 8. nvme_init_ctrl_finish()                  控制器初始化完成
  │     ├─ 读取 VS 寄存器（版本号）
  │     ├─ 设置 sqsize（MQES）
  │     ├─ 判断 subsystem 模式（NSSRC）
  │     ├─ nvme_init_identify()                  发送 Identify Controller 命令
  │     ├─ nvme_configure_apst()                 配置自动电源状态转换
  │     ├─ nvme_configure_timestamp()            配置时间戳
  │     ├─ nvme_configure_host_options()         配置主机选项
  │     ├─ nvme_configure_opal()                 配置 Opal 自加密
  │     └─ nvme_start_keep_alive()              启动 Keep-Alive 定时器
  │
  ├─ 9. nvme_setup_io_queues()                   创建 IO 队列
  │     ├─ 计算 nr_io_queues（CPU 数 + write/poll 队列）
  │     ├─ nvme_set_queue_count()                Set Features (Number of Queues)
  │     ├─ nvme_create_io_queues()              循环创建 IO 队列
  │     │     └─ nvme_alloc_queue()             为每个队列：
  │     │           ├─ 分配 CQ DMA 内存（cqes）
  │     │           ├─ 分配 SQ 命令内存（sq_cmds）
  │     │           ├─ 初始化锁（sq_lock, cq_poll_lock）
  │     │           ├─ 设置 doorbell 地址
  │     │           └─ ctrl->queue_count++
  │     └─ queue_request_irq()                   为每个队列注册中断
  │
  ├─ 10. nvme_alloc_io_tag_set()                分配 IO Tag Set
  │      └─ blk_mq_alloc_tag_set()              分配 IO tag set
  │
  ├─ 11. nvme_change_ctrl_state(LIVE)           状态 → LIVE
  │
  ├─ 12. nvme_start_ctrl()                      启动控制器
  │      ├─ nvme_enable_aen()                   启用异步事件通知
  │      ├─ nvme_queue_scan()                   调度 namespace 扫描
  │      │     └─ queue_work(nvme_wq, &ctrl->scan_work)
  │      └─ nvme_unquiesce_io_queues()          解冻 IO 队列
  │
  └─ 13. flush_work(&ctrl->scan_work)           等待扫描完成
```

#### 17.2.2 Namespace 扫描详细流程

`nvme_start_ctrl()` 触发 `nvme_scan_work()` 工作项，该函数是 namespace 发现的核心。

```
nvme_scan_work()                                  [core.c]
  │
  ├─ 检查条件：state == LIVE && ctrl->tagset != NULL
  │
  ├─ nvme_init_non_mdts_limits()                 读取非 MDTS 限制
  │
  ├─ 处理 AER NS_CHANGED 事件
  │     └─ nvme_clear_changed_ns_log()           清除变更日志
  │
  ├─ 扫描 namespace（两种方式）：
  │   │
  │   ├─ 方式 A：nvme_scan_ns_list()             使用 Identify NS List
  │   │     └─ 发送 Identify 命令获取 NSID 列表
  │   │     └─ 对每个 NSID 调用 nvme_scan_ns()
  │   │
  │   └─ 方式 B：nvme_scan_ns_sequential()       顺序扫描（兜底）
  │         ├─ nvme_identify_ctrl()              获取 nn（最大 namespace 数）
  │         └─ for (i = 1; i <= nn; i++)        循环扫描
  │               └─ nvme_scan_ns(ctrl, i)
  │
  └─ 检查是否需要重新扫描（防止遗漏 AEN）
```

**单个 Namespace 扫描**（`nvme_scan_ns()`）：

```
nvme_scan_ns(ctrl, nsid)                          [core.c]
  │
  ├─ nvme_identify_ns_descs()                    获取 NS 描述符（UUID/EUI64/NGUID）
  │
  ├─ 获取 NS 基本信息（尝试两种方式）：
  │   ├─ nvme_ns_info_from_id_cs_indep()         Command Set Independent Identify
  │   │     └─ 发送 Identify (CNS=NS_CS_INDEP) 命令
  │   │     └─ 提取：anagrpid, is_shared, is_readonly, is_ready,
  │   │             is_rotational, no_vwc, endgid
  │   │
  │   └─ nvme_ns_info_from_identify()           传统 Identify Namespace
  │         └─ nvme_identify_ns()               发送 Identify 命令
  │         └─ 提取：anagrpid, is_shared, is_readonly, ncap
  │         └─ 如果 ncap == 0 → is_removed = true
  │
  ├─ 如果 is_removed → nvme_ns_remove_by_nsid() 移除已删除的 ns
  │
  ├─ 如果 !is_ready → return（等待 AEN 通知）
  │
  ├─ nvme_find_get_ns(ctrl, nsid)               查找是否已存在
  │   │
  │   ├─ 已存在 → nvme_validate_ns(ns, &info)   验证并更新
  │   │     ├─ 检查 ID 是否变化
  │   │     └─ nvme_update_ns_info()            更新 NS 信息
  │   │
  │   └─ 不存在 → nvme_alloc_ns(ctrl, &info)    创建新的 namespace
  │
  └─ nvme_put_ns(ns)                            释放引用
```

**创建新 Namespace**（`nvme_alloc_ns()`）：

```
nvme_alloc_ns(ctrl, info)                           [core.c]
  │
  ├─ 1. kzalloc_node(sizeof(*ns))                 分配 nvme_ns 结构体
  │
  ├─ 2. blk_mq_alloc_disk(ctrl->tagset, &lim, ns) 分配 gendisk + request_queue
  │     └─ 块层接口：创建磁盘和请求队列
  │     └─ disk->fops = &nvme_bdev_ops            设置块设备操作函数
  │     └─ disk->private_data = ns                反向指针
  │
  ├─ 3. 初始化 ns 字段：disk, queue, ctrl, kref
  │
  ├─ 4. nvme_init_ns_head(ns, info)               初始化 namespace head
  │     └─ 创建或查找 ns_head（共享 namespace 复用）
  │     └─ 将 ns 加入 ns_head->list（siblings）
  │
  ├─ 5. 设置磁盘名称（根据多路径模式）：
  │     ├─ 多路径 nvme_ns_head_multipath: "nvme%dc%dn%d"
  │     │     └─ disk->flags |= GENHD_FL_HIDDEN  隐藏底层盘
  │     ├─ 多路径非聚合: "nvme%dn%d"
  │     └─ 单路径: "nvme%dn%d"
  │
  ├─ 6. nvme_update_ns_info(ns, info)             更新 namespace 块层信息
  │     │
  │     └─ nvme_update_ns_info_block(ns, info)   块设备信息更新
  │           ├─ nvme_identify_ns()               获取 IDENTIFY NS 数据
  │           ├─ 检查 ncap == 0?
  │           ├─ 计算 lbaf（LBA 格式）
  │           ├─ 获取 ZNS 信息（如果是 Zoned 设备）
  │           ├─ queue_limits_start_update()      开始更新队列限制
  │           ├─ blk_mq_freeze_queue()            冻结队列
  │           ├─ 设置：lba_shift, nuse, capacity
  │           ├─ nvme_set_ctrl_limits()           设置控制器限制
  │           ├─ nvme_configure_metadata()        配置元数据
  │           ├─ nvme_config_discard()            配置 DISCARD 支持
  │           ├─ 设置写缓存/FUA 特性
  │           ├─ nvme_init_integrity()            初始化完整性保护
  │           ├─ 设置写流（write streams）
  │           ├─ queue_limits_commit_update()     提交队列限制
  │           ├─ set_capacity_and_notify()        设置容量 + 发送 uevent
  │           ├─ set_disk_ro()                    设置只读状态
  │           ├─ set_bit(NVME_NS_READY)           标记就绪
  │           └─ blk_mq_unfreeze_queue()          解冻队列
  │
  ├─ 7. nvme_ns_add_to_ctrl_list(ns)              将 ns 加入 ctrl->namespaces
  │
  ├─ 8. device_add_disk(ctrl->device, ns->disk)  向内核注册块设备
  │     └─ 块层接口：创建 /dev/nvmeXnY、sysfs 属性等
  │
  ├─ 9. nvme_add_ns_cdev(ns)                      创建字符设备 /dev/ngXnY
  │
  └─ 10. nvme_mpath_add_disk(ns, anagrpid)        多路径磁盘添加
```

---

### 17.3 移除流程（Remove）

当 NVMe 设备被拔出或驱动卸载时，触发 `nvme_remove()`。移除流程与注册流程基本对称逆向。

#### 17.3.1 移除流程总览

```
nvme_remove(pdev)                                    [pci.c]
  │
  ├─ 1. nvme_change_ctrl_state(DELETING)         状态 → DELETING
  │
  ├─ 2. 检查设备是否仍在位
  │     └─ 如果设备已拔出 → nvme_change_ctrl_state(DEAD)
  │     └─ nvme_dev_disable(dev, true)           硬件关闭
  │
  ├─ 3. flush_work(&ctrl->reset_work)            等待正在进行的复位完成
  │
  ├─ 4. nvme_stop_ctrl(&dev->ctrl)               停止控制器
  │     ├─ nvme_mpath_stop(ctrl)                 停止多路径
  │     ├─ nvme_auth_stop(ctrl)                  停止认证
  │     ├─ nvme_stop_failfast_work(ctrl)         停止快速失败
  │     ├─ flush_work(&ctrl->async_event_work)   等待异步事件处理完成
  │     └─ ctrl->ops->stop_ctrl(ctrl)            传输层停止
  │
  ├─ 5. nvme_remove_namespaces(&dev->ctrl)       移除所有 namespace
  │     │
  │     ├─ nvme_mpath_clear_ctrl_paths(ctrl)     清除多路径
  │     ├─ nvme_unquiesce_io_queues(ctrl)        解冻 IO 队列（防止挂起 IO）
  │     ├─ flush_work(&ctrl->scan_work)          等待扫描完成
  │     ├─ 如果 DEAD → nvme_mark_namespaces_dead() 标记所有盘为 dead
  │     ├─ nvme_change_ctrl_state(DELETING_NOIO) 状态 → DELETING_NOIO
  │     ├─ list_splice_init_rcu()                将 namespaces 链表移出
  │     └─ 对每个 ns 调用 nvme_ns_remove(ns)
  │
  ├─ 6. nvme_dev_disable(dev, true)              硬件禁用
  │     ├─ nvme_quiesce_io_queues()              静默 IO 队列
  │     ├─ nvme_delete_io_queues()               删除 IO 队列（发送 Delete I/O SQ/CQ 命令）
  │     ├─ nvme_disable_ctrl()                   设置 CC.EN=0 禁用控制器
  │     ├─ nvme_suspend_io_queues()              挂起所有 IO 队列
  │     ├─ nvme_suspend_queue(dev, 0)            挂起 Admin 队列
  │     ├─ pci_free_irq_vectors()                释放中断向量
  │     ├─ nvme_reap_pending_cqes()             回收待处理完成队列条目
  │     ├─ nvme_cancel_tagset()                  取消 IO tagset 中所有请求
  │     └─ nvme_cancel_admin_tagset()            取消 Admin tagset 中所有请求
  │
  ├─ 7. nvme_free_host_mem(dev)                  释放主机内存缓冲区
  │
  ├─ 8. nvme_dev_remove_admin(dev)               移除 Admin Tag Set
  │     └─ nvme_remove_admin_tag_set()           释放 Admin tag set
  │
  ├─ 9. nvme_dbbuf_dma_free(dev)                 释放门铃缓冲区 DMA
  │
  ├─ 10. nvme_free_queues(dev, 0)                释放所有队列内存
  │      └─ 从高到低：nvme_free_queue(&dev->queues[i])
  │
  ├─ 11. mempool_destroy(dev->dmavec_mempool)    销毁 DMA 内存池
  │
  ├─ 12. nvme_dev_unmap(dev)                     取消 MMIO 映射
  │
  └─ 13. nvme_uninit_ctrl(&dev->ctrl)            控制器反初始化
       ├─ nvme_stop_keep_alive(ctrl)             停止 Keep-Alive
       ├─ nvme_hwmon_exit(ctrl)                  退出硬件监控
       ├─ cdev_device_del(&ctrl->cdev, ctrl->device) 删除字符设备 /dev/nvmeX
       └─ nvme_put_ctrl(ctrl)                    释放控制器引用
```

#### 17.3.2 单个 Namespace 移除详细流程

```
nvme_ns_remove(ns)                                   [core.c]
  │
  ├─ 1. test_and_set_bit(NVME_NS_REMOVING)        原子标记移除中（防止重复）
  │
  ├─ 2. clear_bit(NVME_NS_READY)                  清除就绪标志
  │
  ├─ 3. set_capacity(ns->disk, 0)                 容量设置为 0
  │
  ├─ 4. nvme_fault_inject_fini()                  清理故障注入
  │
  ├─ 5. synchronize_srcu(&ns->head->srcu)         等待所有读者退出
  │
  ├─ 6. nvme_mpath_clear_current_path(ns)         清除多路径当前路径
  │
  ├─ 7. 从 ns_head->list 中删除（siblings）
  │     └─ 如果是最后一个路径 → 标记 last_path = true
  │
  ├─ 8. nvme_cdev_del(&ns->cdev)                  删除字符设备 /dev/ngXnY
  │
  ├─ 9. nvme_mpath_remove_sysfs_link(ns)          删除多路径 sysfs 链接
  │
  ├─ 10. del_gendisk(ns->disk)                    向块层注销块设备
  │      └─ 块层接口：删除 /dev/nvmeXnY、sysfs 属性
  │
  ├─ 11. 从 ctrl->namespaces 中删除
  │
  ├─ 12. 如果是最后一个路径：
  │      └─ nvme_mpath_remove_disk(ns->head)      移除多路径聚合盘
  │
  └─ 13. nvme_put_ns(ns)                          释放引用（kref_put → 最终释放 ns）
```

---

### 17.4 与块层的交互接口

NVMe 驱动通过以下关键块层 API 与通用块层交互：

| 块层 API | 调用位置 | 作用 |
|----------|----------|------|
| `blk_mq_alloc_tag_set()` | `nvme_alloc_admin_tag_set()` / `nvme_alloc_io_tag_set()` | 分配 tag set，每个 tag 对应一个硬件命令槽位 |
| `blk_mq_alloc_disk()` | `nvme_alloc_ns()` | 分配 gendisk + request_queue，绑定到 tag set |
| `device_add_disk()` | `nvme_alloc_ns()` | 向内核注册块设备（创建 /dev/nvmeXnY） |
| `del_gendisk()` | `nvme_ns_remove()` | 注销块设备 |
| `blk_mq_freeze_queue()` | `nvme_update_ns_info_block()` | 冻结队列（更新限制前） |
| `blk_mq_unfreeze_queue()` | `nvme_update_ns_info_block()` | 解冻队列（更新完成后） |
| `set_capacity()` / `set_capacity_and_notify()` | `nvme_ns_remove()` / `nvme_update_ns_info_block()` | 设置/更新磁盘容量 |
| `set_disk_ro()` | `nvme_update_ns_info_block()` | 设置磁盘只读状态 |
| `queue_limits_start_update()` | `nvme_update_ns_info_block()` | 开始更新队列限制 |
| `queue_limits_commit_update()` | `nvme_update_ns_info_block()` | 提交队列限制更新 |
| `blk_mark_disk_dead()` | `nvme_mark_namespaces_dead()` | 标记磁盘为 dead（突发移除） |
| `blk_queue_dying()` | `nvme_dev_remove_admin()` | 检查队列是否正在销毁 |

### 17.5 关键工作队列

NVMe 驱动使用三个全局工作队列（`core.c`）：

| 工作队列 | 用途 |
|----------|------|
| `nvme_wq` | 扫描、AEN 处理、固件激活、Keep-Alive、周期性重连 |
| `nvme_reset_wq` | 控制器复位（会 flush nvme_wq 上的工作） |
| `nvme_delete_wq` | 控制器删除（会 flush nvme_reset_wq 上的工作） |

### 17.6 涉及的文件清单

| 文件 | 行数 | 作用 |
|------|------|------|
| `drivers/nvme/host/core.c` | ~5,200 | NVMe 核心逻辑：namespace 管理、控制器生命周期、Admin 命令 |
| `drivers/nvme/host/pci.c` | ~3,900 | PCIe 传输层：probe/remove、队列创建/销毁、中断处理 |
| `drivers/nvme/host/nvme.h` | ~700 | 核心数据结构定义（nvme_ctrl/nvme_ns/nvme_ns_head 等） |
| `drivers/nvme/host/multipath.c` | — | 多路径支持（ANA 状态管理） |
| `drivers/nvme/host/sysfs.c` | — | NVMe 特定的 sysfs 属性 |
| `drivers/nvme/host/ioctl.c` | — | NVMe 私有 ioctl 处理 |
| `drivers/nvme/host/pr.c` | — | Persistent Reservation 支持 |
| `drivers/nvme/host/fabrics.c` | — | NVMe-oF Fabrics 通用代码 |
| `drivers/nvme/host/tcp.c` | — | NVMe/TCP 传输层 |
| `drivers/nvme/host/rdma.c` | — | NVMe/RDMA 传输层 |
| `drivers/nvme/host/fc.c` | — | NVMe/FC 传输层 |

---

## 18. NVMe 驱动：块设备读写 I/O 流程

本章分析从用户态程序 `open()` / `read()` / `write()` 一个 NVMe 块设备（如 `/dev/nvme0n1`），到最终数据通过 PCIe 总线传输到硬件 SSD 的完整 I/O 路径，以及从中断/轮询完成到唤醒用户态程序的逆过程。

### 18.1 I/O 流程总览

用户态 `read()`/`write()` 调用经过以下层次：

```
用户态 Application
  │  read(fd, buf, len) / write(fd, buf, len)
  ▼
════════════════════════ VFS 层 ════════════════════════
  │  file->f_op->read_iter() / write_iter()    [block/fops.c]
  │  blkdev_read_iter() / blkdev_write_iter()   O_DIRECT 路径
  │     └─ __blkdev_direct_IO_simple() / __blkdev_direct_IO()
  │           └─ 构建 struct bio
  │              └─ submit_bio(bio)           [block/blk-core.c]
  ▼
════════════════════════ 通用块层 ════════════════════════
  │  submit_bio_noacct()                      [block/blk-core.c]
  │     └─ __submit_bio()
  │           └─ blk_mq_submit_bio(bio)       [block/blk-mq.c]
  │                 ├─ 尝试合并 (bio merge)
  │                 ├─ 分配 request (blk_mq_get_new_requests)
  │                 ├─ 绑定 bio 到 request
  │                 ├─ QoS 限流 (rq_qos_track)
  │                 ├─ 加密 (blk_crypto_rq_get_keyslot)
  │                 ├─ plug 缓存 / 调度器入队
  │                 └─ blk_mq_try_issue_directly(hctx, rq)
  │                       └─ blk_mq_request_issue_directly()
  │                             └─ __blk_mq_issue_directly()
  │                                   └─ q->mq_ops->queue_rq()  ← 进入 NVMe 驱动
  ▼
═══════════════════════ NVMe 驱动层 ═════════════════════════
  │  nvme_queue_rq()                          [drivers/nvme/host/pci.c]
  │     ├─ 检查 NVMEQ_ENABLED
  │     ├─ nvme_check_ready()               控制器状态检查
  │     ├─ nvme_prep_rq(req)                准备请求
  │     │     ├─ nvme_setup_cmd(ns, req)    构造 NVMe 命令 (SLBA, length, opcode...)
  │     │     │     └─ nvme_setup_rw(ns, req, cmd, op)  填充 READ/WRITE 命令
  │     │     ├─ nvme_map_data(req)         DMA 映射 (PRP/SGL)
  │     │     ├─ nvme_map_metadata(req)     元数据 DMA 映射
  │     │     └─ nvme_start_request(req)    设置超时，更新统计
  │     ├─ nvme_sq_copy_cmd(nvmeq, &iod->cmd)  拷贝命令到 SQ
  │     └─ nvme_write_sq_db(nvmeq, bd->last)   写 Doorbell 通知硬件
  ▼
════════════════════════ 硬件层 ═════════════════════════
  │  NVMe SSD 控制器
  │     ├─ 从 SQ 取出命令
  │     ├─ 执行 READ/WRITE (DMA 传输)
  │     └─ 写入 CQ 完成条目 + 发送 MSI-X 中断
  ▼
═══════════════════════ 中断处理 ═════════════════════════
  │  nvme_irq()                               [drivers/nvme/host/pci.c]
  │     └─ nvme_poll_cq(nvmeq, &iob)
  │           └─ while (nvme_cqe_pending(nvmeq)):
  │                 ├─ nvme_handle_cqe(nvmeq, iob, idx)
  │                 │     ├─ nvme_find_rq()         找到对应的 request
  │                 │     └─ nvme_try_complete_req() 尝试完成请求
  │                 │           └─ nvme_pci_complete_rq(req)
  │                 │                 ├─ nvme_pci_unmap_rq(req)  DMA 反映射
  │                 │                 └─ nvme_complete_rq(req)   NVMe 完成处理
  │                 │                       └─ nvme_end_req(req)
  │                 │                             └─ blk_mq_end_request(req, status)
  │                 └─ nvme_update_cq_head(nvmeq)  更新 CQ 头指针
  │           └─ nvme_ring_cq_doorbell(nvmeq)  写 Doorbell 通知硬件已消费
  │
  │  blk_mq_end_request() → bio_endio() → bio->bi_end_io()
  │     └─ blkdev_bio_end_io()                [block/fops.c]
  │           └─ 唤醒等待的 io_complete() 或 唤醒同步等待的进程
  ▼
  用户态  (read()/write() 返回)
```

---

### 18.2 涉及的关键数据结构

#### 18.2.1 nvme_iod — I/O 描述符（Per-Request）

文件：`drivers/nvme/host/pci.c` L431-L444

```c
/*
 * nvme_iod 描述一次 I/O 操作的数据载荷。
 * 每个 blk-mq request 携带一个 nvme_iod（通过 blk_mq_rq_to_pdu 获取）。
 * 在 tag set 分配时预留空间：cmd_size = sizeof(struct nvme_iod)。
 */
struct nvme_iod {
    struct nvme_request req;                     // 通用 NVMe 请求（含 ctrl 指针、状态、重试次数）
    struct nvme_command cmd;                     // 硬件 NVMe 命令（64 字节，直接拷贝到 SQ）
    u8 flags;                                    // 标志位：IOD_ABORTED/SMALL_DESCRIPTOR/SINGLE_SEGMENT/P2P/MMIO
    u8 nr_descriptors;                           // 描述符数量（PRP list 或 SGL 段数）

    size_t total_len;                            // 数据总长度
    struct dma_iova_state dma_state;             // DMA 映射的 IOVA 状态跟踪
    void *descriptors[NVME_MAX_NR_DESCRIPTORS];  // 描述符指针数组（PRP list 页或 SGL 段）
    struct nvme_dma_vec *dma_vecs;               // DMA 向量数组（用于 DMA 重复映射）
    unsigned int nr_dma_vecs;                    // DMA 向量数量

    dma_addr_t meta_dma;                         // 元数据 DMA 地址
    size_t meta_total_len;                       // 元数据总长度
    struct dma_iova_state meta_dma_state;        // 元数据 DMA 状态
    struct nvme_sgl_desc *meta_descriptor;       // 元数据 SGL 描述符
};
```

**iod_flags 标志位**：

| 标志 | 含义 |
|------|------|
| `IOD_ABORTED` | 命令已被超时处理程序中止 |
| `IOD_SMALL_DESCRIPTOR` | 使用小描述符池（小块内存） |
| `IOD_SINGLE_SEGMENT` | 单段 DMA 映射 |
| `IOD_DATA_P2P` | 数据载荷包含 P2P 内存 |
| `IOD_META_P2P` | 元数据包含 P2P 内存 |
| `IOD_DATA_MMIO` | 数据载荷包含 MMIO 内存 |
| `IOD_META_MMIO` | 元数据包含 MMIO 内存 |
| `IOD_SINGLE_META_SEGMENT` | 使用非合并 MPTR 的元数据 |

#### 18.2.2 nvme_dma_vec — DMA 向量

文件：`drivers/nvme/host/pci.c` L421-L424

```c
struct nvme_dma_vec {
    dma_addr_t addr;       // DMA 地址
    unsigned int len;      // 长度
};
```

#### 18.2.3 nvme_command — NVMe 硬件命令

文件：`drivers/nvme/host/nvme.h` L108-L175（部分）

```c
/*
 * NVMe 命令结构体（64 字节），对应硬件 SQ Entry 格式。
 * 支持多种命令类型（RW/Identify/Features/Admin 等），通过 union 复用空间。
 */
struct nvme_command {
    union {
        struct nvme_common_command common;       // 公共字段：opcode, flags, command_id, nsid, cdw2-15
        struct nvme_rw_command rw;               // 读写命令：opcode, nsid, slba, length, control, dsmgmt
        struct nvme_identify identify;           // Identify 命令
        struct nvme_features features;           // Set/Get Features 命令
        struct nvme_dsm_range dsm;               // DataSet Management 命令
    };
};
```

**nvme_rw_command 字段**（读写命令）：

| 字段 | 宽度 | 含义 |
|------|------|------|
| `opcode` | u8 | 操作码：0x02=Read, 0x01=Write |
| `flags` | u8 | 标志位 |
| `command_id` | u16 | 命令 ID（对应 tag） |
| `nsid` | u32 | Namespace ID |
| `slba` | u64 | 起始逻辑块地址 |
| `length` | u16 | 块数 - 1（0-based） |
| `control` | u16 | 控制位：FUA, LR, PRACT, PRCHK 等 |
| `dsmgmt` | u32 | DataSet Management |

#### 18.2.4 nvme_request — 通用 NVMe 请求

文件：`drivers/nvme/host/nvme.h` L259-L286

```c
/*
 * nvme_request 嵌入在 nvme_iod 中，提供 NVMe 请求的通用状态跟踪。
 * 通过 blk_mq_rq_to_pdu(req) → nvme_req(req) 宏获取。
 */
struct nvme_request {
    struct nvme_ctrl *ctrl;      // 所属控制器
    struct nvme_command *cmd;    // 指向 nvme_iod.cmd
    union nvme_result result;    // 完成结果（CQE 中的 DW0-1）
    u16 status;                  // 命令状态码
    u8 retries;                  // 重试次数
    u8 flags;                    // 标志位
    u8 opcode;                   // 操作码
};
```

#### 18.2.5 blkdev_dio — 块设备直接 I/O 描述符

文件：`block/fops.c` L122-L130

```c
/*
 * 块设备直接 I/O 描述符，用于 O_DIRECT 读写。
 * 同步/异步 I/O 通过 flags 中的 DIO_IS_SYNC 区分。
 */
struct blkdev_dio {
    union {
        struct kiocb *iocb;              // 异步 I/O 的 kiocb
        struct task_struct *waiter;      // 同步 I/O 的等待进程
    };
    size_t size;                         // 已完成字节数
    atomic_t ref;                        // 引用计数（多 bio 模式）
    unsigned int flags;                  // DIO_SHOULD_DIRTY | DIO_IS_SYNC
    struct bio bio;                      // 嵌入的 bio 结构体
};
```

#### 18.2.6 nvme_ns_head — Namespace 头（共享信息）

文件：`drivers/nvme/host/nvme.h` L526-L568

```c
/*
 * nvme_ns_head 保存 namespace 的全局共享信息。多路径场景下多个 nvme_ns
 * 共享同一个 nvme_ns_head。读写 I 路径中通过 ns->head->lba_shift 计算
 * LBA 地址，通过 ns->head->ns_id 填充命令字的 NSID 字段。
 */
struct nvme_ns_head {
    struct list_head list;               // 挂入 subsystem 的 ns 链表
    struct nvme_subsystem *subsys;       // 所属子系统
    struct nvme_ns_ids ids;              // namespace 标识符 (EUI64, NGUID, UUID)
    u8 lba_shift;                        // LBA 大小偏移：block_size = 1 << lba_shift
    u16 ms;                              // 元数据大小（每 LBA）
    u16 pi_size;                         // 保护信息大小
    u8 pi_type;                          // 保护信息类型 (Type 1/2/3)
    u8 guard_type;                       // Guard 校验类型 (CRC16/CRC64)
    struct kref ref;                     // 引用计数
    bool shared;                         // 是否多路径共享
    u64 nuse;                            // 已分配 LBA 数量
    unsigned ns_id;                      // Namespace ID (写入 NVMe 命令 NSID 字段)
    int instance;                        // 实例号 (用于生成 /dev/nvmeXnY)
    u64 zsze;                            // Zone Size (zoned 设备)
    struct gendisk *disk;                // 多路径聚合 gendisk
    u16 nr_plids;                        // 放置标识符数量（写流）
    u16 *plids;                          // 放置标识符数组
};
```

#### 18.2.7 nvme_ns — Namespace 实例（Per-Controller）

文件：`drivers/nvme/host/nvme.h` L585-L617

```c
/*
 * nvme_ns 代表一个 NVMe namespace 在特定控制器上的实例。
 * 每个 I 请求通过 req->q->queuedata 指向所属的 nvme_ns。
 * 在 nvme_setup_cmd() 中通过 ns 获取 LBA 偏移、NSID 等。
 */
struct nvme_ns {
    struct list_head list;               // 挂入 controller 的 ns 链表
    struct nvme_ctrl *ctrl;              // 所属控制器
    struct request_queue *queue;         // 关联的请求队列 (req->q)
    struct gendisk *disk;                // 关联的 gendisk
    struct kref kref;                    // 引用计数
    struct nvme_ns_head *head;           // 指向共享的 ns head
    unsigned long flags;                 // NVME_NS_REMOVING | NVME_NS_READY | ...
};
```

#### 18.2.8 nvme_queue — NVMe 硬件队列（SQ/CQ 元数据）

文件：`drivers/nvme/host/pci.c` L365-L394

```c
/*
 * nvme_queue 封装一个 NVMe Submission Queue (SQ) 和 Completion Queue (CQ) 对。
 * 每个 blk-mq 硬件队列 (hctx) 对应一个 nvme_queue（通过 hctx->driver_data 访问）。
 * SQ 位于主机内存，通过 Doorbell 通知硬件读取；CQ 由硬件写入，中断通知主机。
 *
 * SQ 是环形缓冲区，主机写命令 → 硬件读命令。
 * CQ 是环形缓冲区，硬件写完成条目 → 主机读完成条目。
 */
struct nvme_queue {
    struct nvme_dev *dev;                           // 所属 nvme 设备
    spinlock_t sq_lock;                             // SQ 自旋锁（保护 sq_cmds 写入）
    void *sq_cmds;                                  // SQ 命令缓冲区（DMA 一致性内存，映射到硬件）
    spinlock_t cq_poll_lock;                        // CQ 轮询锁（仅 poll 队列）
    struct nvme_completion *cqes;                   // CQ 完成条目数组（DMA 一致性内存）
    dma_addr_t sq_dma_addr;                         // SQ 的 DMA 地址
    dma_addr_t cq_dma_addr;                         // CQ 的 DMA 地址
    u32 __iomem *db;                                // Doorbell 寄存器地址（MMIO）
    u32 q_depth;                                    // 队列深度（SQ 和 CQ 条目数）
    u16 cq_vector;                                  // 中断向量号
    u16 sq_tail;                                    // SQ 尾指针（主机写入位置）
    u16 last_sq_tail;                               // 上次写 Doorbell 时的 SQ 尾（用于批量优化）
    u16 cq_head;                                    // CQ 头指针（主机读取位置）
    u16 qid;                                        // 队列 ID（0 = Admin Queue）
    u8 cq_phase;                                    // CQ Phase Tag（与 CQE 的 Phase 位比较）
    u8 sqes;                                        // SQ Entry Size (1 << sqes = 条目字节数)
    unsigned long flags;                            // NVMEQ_ENABLED | NVMEQ_SQ_CMB | NVMEQ_POLLED
    __le32 *dbbuf_sq_db;                            // Doorbell Buffer：SQ Tail Doorbell
    __le32 *dbbuf_cq_db;                            // Doorbell Buffer：CQ Head Doorbell
    __le32 *dbbuf_sq_ei;                            // Doorbell Buffer：SQ Event Index
    __le32 *dbbuf_cq_ei;                            // Doorbell Buffer：CQ Event Index
    struct completion delete_done;                   // 队列删除完成信号
};
```

#### 18.2.9 blk_mq_queue_data — blk-mq 派发数据

文件：`include/linux/blk-mq.h`

```c
/*
 * blk_mq_queue_data 是 blk-mq 调用 mq_ops->queue_r() 时传递的参数。
 * 包含要派发的请求 (rq) 和是否为最后一个请求的标志 (last)。
 * last 标志用于 NVMe 驱动的批量 Doorbell 优化：
 *   仅当 last=true 时才写 Doorbell，减少 MMIO 写入次数。
 */
struct blk_mq_queue_data {
    struct request *rq;      // 要派发的请求
    bool last;               // 是否为当前批量中的最后一个请求
};
```

---

### 18.3 详细函数调用栈

#### 18.3.1 用户态到块层：open 路径

```
用户态 open("/dev/nvme0n1", O_RDWR)
  └─ syscall
        └─ file_open() → do_dentry_open()
              └─ blkdev_open(inode, filp)                      [block/fops.c]
                    │
                    ├─ bdev = blkdev_get_by_dev(inode->i_rdev, ...)
                    │     └─ bdev = blkdev_get_no_open(dev, ...) 获取或创建 bdev
                    │
                    ├─ disk->fops->open(bdev, filp->f_mode)    调用 nvme_open()
                    │     └─ nvme_ns_open(disk->private_data)   [core.c]
                    │           ├─ nvme_ns_head_multipath() 检查（多路径盘不可直接打开）
                    │           ├─ nvme_get_ns(ns)           增加 namespace 引用计数
                    │           └─ try_module_get()          防止驱动卸载
                    │
                    └─ filp->f_mapping = bdev->bd_inode->i_mapping  设置 address_space
```

#### 18.3.2 用户态到块层：read 路径（O_DIRECT）

```
用户态 read(fd, buf, len)
  └─ syscall
        └─ ksys_read()
              └─ vfs_read()
                    └─ file->f_op->read_iter()  → blkdev_read_iter()  [block/fops.c]
                          │
                          ├─ blkdev_direct_IO()                     [block/fops.c]
                          │     │
                          │     ├─ __blkdev_direct_IO_simple()     单 bio 路径
                          │     │     ├─ bio_init(&bio, bdev, vecs, nr_pages, REQ_OP_READ)
                          │     │     ├─ bio.bi_iter.bi_sector = pos >> SECTOR_SHIFT
                          │     │     ├─ bio.bi_end_io = blkdev_bio_end_io
                          │     │     ├─ blkdev_iov_iter_get_pages()  pin 用户页
                          │     │     └─ submit_bio_wait(&bio)        提交并等待
                          │     │
                          │     └─ __blkdev_direct_IO()           多 bio 路径
                          │           ├─ 构建 blkdev_dio 结构体
                          │           ├─ 循环：
                          │           │     ├─ bio_alloc() + bio_init()
                          │           │     ├─ blkdev_iov_iter_get_pages()
                          │           │     └─ submit_bio(bio)        提交每个 bio
                          │           ├─ 同步：等待 waiter 被唤醒
                          │           └─ 异步：返回 -EIOCBQUEUED
                          │
                          └─ 返回已读字节数
```

#### 18.3.3 块层 submit_bio 到 NVMe queue_rq

```
submit_bio(bio)                                           [block/blk-core.c]
  │
  ├─ bio_set_ioprio(bio)                                设置 I/O 优先级
  ├─ submit_bio_noacct(bio)                             无统计计数提交
  │     └─ __submit_bio(bio)                             [block/blk-core.c]
  │           ├─ blk_crypto_submit_bio(bio)             内联加密处理
  │           └─ blk_mq_submit_bio(bio)                  [block/blk-mq.c]
  │                 │
  │                 ├─ rq = blk_mq_peek_cached_request() 检查 plug 缓存
  │                 ├─ bio_queue_enter(bio)              获取队列引用
  │                 ├─ 检查对齐、poll 支持
  │                 ├─ __bio_split_to_limits()           按队列限制拆分 bio
  │                 ├─ bio_integrity_prep()              完整性校验准备
  │                 ├─ blk_mq_attempt_bio_merge()        尝试与已有请求合并
  │                 │
  │                 ├─ blk_mq_get_new_requests()         分配新 request
  │                 │     └─ blk_mq_rq_ctx_init()       初始化 request 上下文
  │                 │
  │                 ├─ rq_qos_track(q, rq, bio)          QoS 记录
  │                 ├─ blk_mq_bio_to_request(rq, bio)    绑定 bio 到 request
  │                 ├─ blk_crypto_rq_get_keyslot(rq)     加密密钥槽
  │                 │
  │                 ├─ [有 plug] → blk_add_rq_to_plug(plug, rq)
  │                 │              → 稍后批量由 blk_mq_flush_plug_list() 下发
  │                 │
  │                 └─ [无 plug] → blk_mq_try_issue_directly(hctx, rq)
  │                       └─ blk_mq_request_issue_directly(rq, last)
  │                             └─ __blk_mq_issue_directly(hctx, rq, last)
  │                                   └─ q->mq_ops->queue_rq(hctx, &bd)
  │                                         └─ nvme_queue_rq()          [pci.c]
```

#### 18.3.4 NVMe 驱动：请求提交

```
nvme_queue_rq(hctx, bd)                                    [pci.c]
  │
  ├─ 1. nvmeq = hctx->driver_data                        获取硬件队列
  │     dev = nvmeq->dev
  │     req = bd->rq
  │     iod = blk_mq_rq_to_pdu(req)                      获取 I/O 描述符
  │
  ├─ 2. test_bit(NVMEQ_ENABLED, &nvmeq->flags)           队列是否已启用
  │
  ├─ 3. nvme_check_ready(&dev->ctrl, req, true)          控制器状态检查
  │     └─ 检查 state == LIVE && !resetting
  │
  ├─ 4. nvme_prep_rq(req)                                准备请求
  │     │
  │     ├─ 4a. 重置 iod 字段（flags, nr_descriptors, total_len 等）
  │     │
  │     ├─ 4b. nvme_setup_cmd(ns, req)                  构造 NVMe 命令
  │     │     │
  │     │     ├─ nvme_clear_nvme_request(req)            清除 nvme_request 状态
  │     │     │
  │     │     └─ switch (req_op(req)):
  │     │           ├─ REQ_OP_READ  → nvme_setup_rw(ns, req, cmd, nvme_cmd_read)
  │     │           ├─ REQ_OP_WRITE → nvme_setup_rw(ns, req, cmd, nvme_cmd_write)
  │     │           ├─ REQ_OP_FLUSH → nvme_setup_flush(ns, cmd)
  │     │           ├─ REQ_OP_DISCARD → nvme_setup_discard(ns, req, cmd)
  │     │           └─ REQ_OP_WRITE_ZEROES → nvme_setup_write_zeroes(ns, req, cmd)
  │     │
  │     │     nvme_setup_rw(ns, req, cmd, op):           读写命令构造
  │     │       ├─ 设置 FUA / LR 控制位
  │     │       ├─ cmd->rw.opcode = op (0x02=Read, 0x01=Write)
  │     │       ├─ cmd->rw.nsid = cpu_to_le32(ns->head->ns_id)
  │     │       ├─ cmd->rw.slba = cpu_to_le64(nvme_sect_to_lba(blk_rq_pos(req)))
  │     │       ├─ cmd->rw.length = cpu_to_le16((bytes >> lba_shift) - 1)
  │     │       ├─ cmd->rw.control = cpu_to_le16(control)  (FUA, LR, PRACT, PRCHK)
  │     │       └─ cmd->rw.dsmgmt = cpu_to_le32(dsmgmt)   (预取、写流)
  │     │
  │     ├─ 4c. nvme_map_data(req)                       DMA 数据映射
  │     │     │
  │     │     ├─ 单段路径：nvme_pci_setup_data_simple()
  │     │     │     ├─ dma_map_single() 单段 DMA 映射
  │     │     │     └─ 填充 iod->descriptors[0] (PRP1)
  │     │     │
  │     │     ├─ 多段 PRP 路径：nvme_pci_setup_data_prp()
  │     │     │     ├─ blk_rq_dma_map_iter_start() 遍历 bio 的 DMA 段
  │     │     │     ├─ 第一段 → PRP1
  │     │     │     ├─ 第二段 → PRP2（直接）
  │     │     │     └─ 后续段 → PRP list（分配描述符页）
  │     │     │
  │     │     └─ 多段 SGL 路径：nvme_pci_setup_data_sgl()
  │     │           ├─ 分配 SGL 描述符数组
  │     │           └─ 填充 SGL 描述符（Data Block + Last Segment）
  │     │
  │     ├─ 4d. nvme_map_metadata(req)                   元数据 DMA 映射
  │     │     └─ nvme_pci_setup_meta_iter(req)
  │     │          ├─ blk_rq_integrity_dma_map_iter_start() 遍历完整性段
  │     │          ├─ 单段 → 直接使用 meta_dma
  │     │          └─ 多段 → 分配 meta SGL 描述符
  │     │
  │     └─ 4e. nvme_start_request(req)                  设置超时 & 统计
  │           ├─ blk_mq_start_request(req)              记录开始时间戳
  │           └─ nvme_req(req)->timeout = 超时时间
  │
  ├─ 5. spin_lock(&nvmeq->sq_lock)                      获取 SQ 锁
  │
  ├─ 6. nvme_sq_copy_cmd(nvmeq, &iod->cmd)              拷贝命令到 SQ
  │     │
  │     └─ memcpy(nvmeq->sq_cmds + (sq_tail << sqes), cmd, sizeof(*cmd))
  │         // sq_cmds 是 DMA 一致性内存，映射到硬件 SQ
  │         nvmeq->sq_tail++                            推进 SQ 尾指针
  │
  ├─ 7. nvme_write_sq_db(nvmeq, bd->last)              写 Doorbell
  │     │
  │     ├─ 如果 bd->last == false && 未到 last_sq_tail:
  │     │     return                                    延迟写 DB（批量优化）
  │     │
  │     └─ writel(nvmeq->sq_tail, nvmeq->q_db)         写 SQ Tail Doorbell
  │         // 硬件收到 Doorbell 后开始处理 SQ 中的命令
  │
  └─ 8. spin_unlock(&nvmeq->sq_lock)
```

**SQ 命令拷贝与 Doorbell 机制详解**（[pci.c](file:///home/louis/code/linux/drivers/nvme/host/pci.c#L713)）：

```c
/*
 * nvme_sq_copy_cmd — 将 64 字节 NVMe 命令拷贝到 SQ 环形缓冲区。
 * sq_cmds 是 DMA 一致性内存（dma_alloc_coherent），硬件可直接访问。
 * sqes 是 SQ Entry Size 的 log2 值（通常为 6，即 64 字节）。
 */
static inline void nvme_sq_copy_cmd(struct nvme_queue *nvmeq,
                    struct nvme_command *cmd)
{
    memcpy(nvmeq->sq_cmds + (nvmeq->sq_tail << nvmeq->sqes),
        absolute_pointer(cmd), sizeof(*cmd));
    if (++nvmeq->sq_tail == nvmeq->q_depth)
        nvmeq->sq_tail = 0;  // 环形缓冲区回绕
}

/*
 * nvme_write_sq_db — 写 SQ Tail Doorbell 通知硬件有新命令。
 * 批量优化：仅在 bd->last == true 或 SQ 尾指针回绕到 last_sq_tail 时才写 DB。
 * 如果支持 Doorbell Buffer（dbbuf），使用 dbbuf 机制代替 MMIO 写入，
 * 进一步减少 PCIe 事务。
 */
static inline void nvme_write_sq_db(struct nvme_queue *nvmeq, bool write_sq)
{
    if (!write_sq) {
        // 批量模式：延迟写 DB，直到 SQ 尾指针与上次写 DB 的位置一致
        u16 next_tail = nvmeq->sq_tail + 1;
        if (next_tail == nvmeq->q_depth)
            next_tail = 0;
        if (next_tail != nvmeq->last_sq_tail)
            return;  // 尚未追上，继续延迟
    }

    // Doorbell Buffer 优化：仅在 event index 超限时才写 MMIO
    if (nvme_dbbuf_update_and_check_event(nvmeq->sq_tail,
            nvmeq->dbbuf_sq_db, nvmeq->dbbuf_sq_ei))
        writel(nvmeq->sq_tail, nvmeq->q_db);  // 写 MMIO Doorbell 寄存器
    nvmeq->last_sq_tail = nvmeq->sq_tail;
}
```

#### 18.3.5 NVMe 驱动：中断处理与完成

```
════════════════════════ 硬件完成 I/O，发送 MSI-X 中断 ════════════════════════

nvme_irq(irq, data)                                        [pci.c]
  │
  ├─ nvmeq = data                                        获取队列
  ├─ DEFINE_IO_COMP_BATCH(iob)                           定义完成批处理
  │
  └─ nvme_poll_cq(nvmeq, &iob)                           轮询完成队列
        │
        └─ while (nvme_cqe_pending(nvmeq)):              CQE 有效（Phase Tag 匹配）
              │
              ├─ 1. dma_rmb()                            DMA 读屏障
              │
              ├─ 2. nvme_handle_cqe(nvmeq, iob, cq_head) 处理单个 CQE
              │     │
              │     ├─ cqe = &nvmeq->cqes[cq_head]       获取 CQE 条目
              │     ├─ command_id = cqe->command_id      命令 ID = tag
              │     │
              │     ├─ [AEN] → nvme_complete_async_event() 异步事件特殊处理
              │     │
              │     └─ [普通命令]:
              │           ├─ req = nvme_find_rq(tagset, command_id)  通过 tag 找到 request
              │           │
              │           └─ nvme_try_complete_req(req, status, result)
              │                 │
              │                 ├─ 设置 nvme_req(req)->result = result
              │                 ├─ 设置 nvme_req(req)->status = status
              │                 ├─ blk_mq_complete_request_remote(req) 跨 CPU 完成
              │                 │     └─ blk_mq_complete_request(req)
              │                 │           └─ blk_mq_complete_request_remote_cpu()
              │                 │
              │                 └─ [或直接] nvme_pci_complete_rq(req)
              │
              ├─ 3. nvme_update_cq_head(nvmeq)           更新 CQ 头指针
              │     └─ cq_head++; if (cq_head == q_depth) { cq_head=0; cq_phase^=1; }
              │
              └─ 4. 循环回到步骤 1

nvme_ring_cq_doorbell(nvmeq)                               写 CQ Head Doorbell
  └─ writel(nvmeq->cq_head, nvmeq->q_db + db_stride)
      // 通知硬件 CQ 条目已被消费，可复用

nvme_pci_complete_rq(req)                                  完成请求处理
  │
  ├─ nvme_pci_unmap_rq(req)                              DMA 反映射
  │     ├─ blk_rq_nr_phys_segments(req) → nvme_unmap_data(req)
  │     │     ├─ blk_rq_dma_unmap() 释放 DMA 映射
  │     │     └─ 释放 PRP list / SGL 描述符
  │     └─ blk_integrity_rq(req) → nvme_unmap_metadata(req)
  │
  └─ nvme_complete_rq(req)                               [core.c]
        │
        ├─ nvme_cleanup_cmd(req)                         清理命令（释放特殊载荷）
        ├─ nvme_decide_disposition(req):                 决定处理方式
        │     ├─ status == 0 → COMPLETE                  正常完成
        │     ├─ 可重试错误 → RETRY                       重新排队
        │     ├─ 路径错误 → FAILOVER                      切换到备份路径
        │     └─ 认证错误 → AUTHENTICATE                  重新认证
        │
        └─ [COMPLETE] nvme_end_req(req)
              ├─ nvme_end_req_zoned(req)                 Zone Append 特殊处理
              ├─ nvme_trace_bio_complete(req)            追踪
              └─ blk_mq_end_request(req, status)         块层完成
                    └─ blk_update_request()             更新统计
                    └─ __blk_mq_end_request()
                          └─ blk_mq_free_request(rq)     释放 tag
                          └─ bio_endio(bio)              调用 bio->bi_end_io()
                                └─ blkdev_bio_end_io(bio)  [block/fops.c]
                                      ├─ 同步: 唤醒 waiter (wake_up_process)
                                      └─ 异步: dio->iocb->ki_complete(iocb, ret)
```

---

### 18.4 数据拷贝路径

#### 18.4.1 读操作数据流

```
NVMe 设备 DMA → 主机物理内存 → 用户态缓冲区

1. 硬件 DMA 写入主机物理内存
   └─ NVMe SSD 通过 PCIe 将数据 DMA 到主机内存
      └─ 目标地址由 nvme_map_data() 中的 dma_map_single/page() 确定

2. blkdev_iov_iter_get_pages() 预先 pin 用户页
   └─ bio_iov_iter_get_pages() → __bio_iov_iter_get_pages()
      └─ 将用户态虚拟地址对应的物理页 pin 住
      └─ 填充到 bio->bi_io_vec[] 中

3. 硬件 DMA 直接写入用户页
   └─ 零拷贝：硬件 DMA 直接写入用户态缓冲区
   └─ 无需内核中转拷贝

4. 完成时 bio_release_pages()
   └─ 释放 pin 的页
   └─ 如果设置了 DIO_SHOULD_DIRTY → bio_set_pages_dirty()
```

#### 18.4.2 写操作数据流

```
用户态缓冲区 → 主机物理内存 → NVMe 设备 DMA

1. blkdev_iov_iter_get_pages() 预先 pin 用户页
   └─ 同读操作，pin 住用户态缓冲区的物理页

2. nvme_map_data() 建立 DMA 映射
   └─ dma_map_single() / dma_map_page() 建立 DMA 地址映射
   └─ 填充 PRP1/PRP2 或 PRP list 或 SGL

3. 硬件 DMA 从主机内存读取数据
   └─ NVMe SSD 通过 PCIe 从主机内存 DMA 读取数据

4. 完成时 DMA 反映射
   └─ nvme_unmap_data() → dma_unmap_single/page()
   └─ bio_release_pages() 释放 pin
```

---

### 18.5 PRP 与 SGL 数据描述符

NVMe 协议支持两种数据描述符格式，用于告诉硬件数据在主机内存中的位置：

| 特性 | PRP (Physical Region Page) | SGL (Scatter Gather) |
|------|---------------------------|--------------------------|
| **定义** | NVMe 原生格式，由 PR1/PR2 + PRP list 组成 | NVMe 1.2+ 引入的通用格式 |
| **描述** | 每项描述一个物理页（4KB 对齐） | 每项描述任意长度的数据段 |
| **效率** | 小 I 高效（1-2 页直接入命令） | 大 I 更灵活（段数少） |
| **限制** | 每页偏移必须一致 | 每个段可独立指定偏移和长度 |
| **使用条件** | 默认使用 | 启用 SGL 或超过阈值（`sgl_threshold`）时 |

**数据映射决策流程**（`nvme_map_data()`，[pci.c](:///drivers/nvme/host/pci.c#L1216)）：

```
nvme_map_data(req)
  │
  ├─ blk_r_nr_phys_segments(req) == 1 → nvme_pci_setup_data_simple()
  │                                       单段快速路径，原地完成
  │
  ├─ blk_r_dma_map_iter_start() → 遍历 bio 的 DMA 段，建立映射
  │
  └─ 判断使用 PRP 还是 SGL：
        ├─ use_sgl == SGL_FORCED → nvme_pci_setup_data_sgl()
        ├─ use_sgl == SGL_SUPPORTED && 平均段大小 >= sgl_threshold → nvme_pci_setup_data_sgl()
        └─ 否则 → nvme_pci_setup_data_prp()
```

#### 18.5.1 PRP 单段映射（nvme_pci_setup_data_simple）

适用场景：I/O 请求只有一个物理段（常见于 4KB 小块 I/O）。

```c
/*
 * 单段快速路径：跳过 DMA 迭代器，直接处理单个 bio_vec。
 * 如果数据跨页边界但 ≤ 2 页，使用 PRP1+PRP2 直接描述。
 * 如果必须使用 SGL，则填充 SGL 描述符。
 * 返回 BLK_STS_AGAIN 表示需要走多段路径。
 */
static blk_status_t nvme_pci_setup_data_simple(struct request *req,
        enum nvme_use_sgl use_sgl)
{
    struct bio_vec bv = req_bvec(req);
    unsigned int prp1_offset = bv.bv_offset & (NVME_CTRL_PAGE_SIZE - 1);
    bool prp_possible = prp1_offset + bv.bv_len <= NVME_CTRL_PAGE_SIZE * 2;
    dma_addr_t dma_addr;

    // 不能使用 PRP 描述 → 返回 AGAIN 走多段路径
    if (!use_sgl && !prp_possible)
        return BLK_STS_AGAIN;
    if (is_pci_p2pdma_page(bv.bv_page))
        return BLK_STS_AGAIN;

    dma_addr = dma_map_bvec(dev->dev, &bv, rq_dma_dir(req), 0);

    // SGL 路径：填充 SGL 描述符
    if (use_sgl == SGL_FORCED || !prp_possible) {
        iod->cmd.common.flags = NVME_CMD_SGL_METABUF;
        iod->cmd.common.dptr.sgl.addr = cpu_to_le64(dma_addr);
        iod->cmd.common.dptr.sgl.length = cpu_to_le32(bv.bv_len);
        iod->cmd.common.dptr.sgl.type = NVME_SGL_FMT_DATA_DESC << 4;
    }
    // PRP 路径：PRP1 + 可选 PRP2
    else {
        unsigned int first_prp_len = NVME_CTRL_PAGE_SIZE - prp1_offset;
        iod->cmd.common.dptr.prp1 = cpu_to_le64(dma_addr);
        iod->cmd.common.dptr.prp2 = 0;
        if (bv.bv_len > first_prp_len)
            iod->cmd.common.dptr.prp2 =
                cpu_to_le64(dma_addr + first_prp_len);
    }
    iod->flags |= IOD_SINGLE_SEGMENT;
    return BLK_STS_OK;
}
```

#### 18.5.2 PRP 多段映射（nvme_pci_setup_data_prp）

适用场景：多个物理段，不使用 SGL 时的默认路径。

```
nvme_pci_setup_data_prp(req, iter)
  │
  ├─ 第一段 → PR1 (cmd->common.d.prp1)
  │
  ├─ 第二段：直接写入 PR2 (cmd->common.d.prp2)
  │
  ├─ 后续段 (3+)：
  │     ├─ 分配 PRP list 页（从 descriptor_pools 小池或大池分配）
  │     │     └─ dma_pool_alloc() → 获取 DMA 一致性内存页
  │     ├─ PR2 = PRP list 的 DMA 地址
  │     └─ 循环填充 PRP list 条目
  │
  └─ 设置 iod->nr_descriptors, iod->total_len
```

#### 18.5.3 SGL 映射（nvme_pci_setup_data_sgl）

适用场景：启用 SGL，或分段数超过阈值。

```
nvme_pci_setup_data_sgl(req, iter)
  │
  ├─ 计算需要的 SGL 段数 (nr_entries)
  ├─ 分配 SGL 描述符数组（dma_pool_alloc）
  ├─ 设置 cmd->common.d.sgl = SGL 描述符的 DMA 地址
  ├─ cmd->common.flags |= NVME_CMD_SGL_METABUF
  │
  └─ 循环填充 SGL 描述符：
        ├─ nvme_pci_sgl_set_data(&sg_list[i], iter)  Data Block 描述符
        └─ 最后一个段 → Last Segment 描述符
```

**PRP 布局**：

```
单页 (≤ 1 页):   PRP1 = 页地址, PRP2 = 0
两页 (≤ 2 页):   PRP1 = 页1地址, PRP2 = 页2地址
多页 (> 2 页):   PRP1 = 页1地址, PRP2 = PRP List 地址
                  PRP List = [页2地址, 页3地址, 页4地址, ...]
```

**SGL 布局**：

```
SGL Segment 1:  {地址1, 长度1, SGL_DATA_BLOCK}
SGL Segment 2:  {地址2, 长度2, SGL_DATA_BLOCK}
...
SGL Last Segment: {地址N, 长度N, SGL_LAST_SEGMENT}
```

---

### 18.6 完成路径详解

#### 18.6.1 nvme_handle_cqe — CQE 处理

文件：`drivers/nvme/host/pci.c` L1531-L1576

```c
/*
 * nvme_handle_cqe 在中断处理中消费单个 CQE 条目。
 * 核心逻辑：
 *   1. 从 CQE 读取 command_id → 通过 tag 找到对应的 request
 *   2. 尝试同步完成 (nvme_try_complete_re)
 *   3. 否则加入 IO 完成批处理 (blk_m_add_to_batch)
 *   4. 无法加入批处理 → 直接调用 nvme_pci_complete_r
 */
static inline void nvme_handle_cqe(struct nvme_queue *nvme,
            struct io_comp_batch *iob, u16 idx)
{
    struct nvme_completion *cqe = &nvme->cqes[idx];
    __u16 command_id = READ_ONCE(cqe->command_id);
    struct request *req;

    // AEN (Async Event Notification) 特殊处理：没有对应的 request
    if (unlikely(nvme_is_aen_re(nvme->qid, command_id))) {
        nvme_complete_async_event(...);
        return;
    }

    // 通过 tag 找到对应的 request
    req = nvme_find_r(nvme_queue_tagset(nvme), command_id);
    if (unlikely(!req)) {
        dev_warn(..., "invalid id %d completed\n", command_id);
        return;
    }

    // 尝试同步完成，否则加入批处理
    if (!nvme_try_complete_req(req, cqe->status, cqe->result) &&
        !blk_mq_add_to_batch(req, iob,
                   nvme_req(req)->status != NVME_SC_SUCCESS,
                   nvme_pci_complete_batch))
        nvme_pci_complete_rq(req);
}
```

**nvme_find_rq — Tag 匹配与 Genctr 校验**（[nvme.h](file:///home/louis/code/linux/drivers/nvme/host/nvme.h#L662)）：

```c
/*
 * command_id 编码：[genctr(8bit)][tag(16bit)]
 * genctr 用于防止 tag 重用导致的 ABA 问题：
 *   每次分配新 tag 时 genctr 递增，CQE 中携带生成时的 genctr，
 *   匹配时校验 genctr 确保 request 没有被重新分配。
 */
static inline struct request *nvme_find_rq(struct blk_mq_tags *tags,
        u16 command_id)
{
    u8 genctr = nvme_genctr_from_cid(command_id);  // 提取 genctr
    u16 tag = nvme_tag_from_cid(command_id);        // 提取 tag

    rq = blk_mq_tag_to_rq(tags, tag);               // 通过 tag 查找 request
    if (unlikely(!rq))
        return NULL;

    // ABA 防护：校验 genctr 是否匹配
    if (unlikely(nvme_genctr_mask(nvme_req(rq)->genctr) != genctr))
        return NULL;  // genctr 不匹配，说明 request 已被重新分配

    return rq;
}
```

**nvme_try_complete_req — 跨 CPU 完成**（[nvme.h](file:///home/louis/code/linux/drivers/nvme/host/nvme.h#L791)）：

```c
/*
 * 尝试在当前 CPU 上完成请求。如果 request 的 CPU 亲和性与当前 CPU 不匹配，
 * blk_mq_complete_request_remote 会通过 IPI 将完成工作派发到目标 CPU。
 * 返回值：true = 已成功完成，false = 需要在当前上下文继续处理。
 */
static inline bool nvme_try_complete_req(struct request *req, __le16 status,
        union nvme_result result)
{
    rq->genctr++;                             // 递增 genctr（用于 ABA 防护）
    rq->status = le16_to_cpu(status) >> 1;   // 解析状态码（去掉 Phase 位）
    rq->result = result;                      // 保存完成结果
    nvme_should_fail(req);                    // 故障注入检查
    if (unlikely(blk_should_fake_timeout(req->q)))
        return true;                          // 假超时，不真正完成
    return blk_mq_complete_request_remote(req); // 远程完成（如需要）
}
```

#### 18.6.2 nvme_complete_rq — NVMe 完成处理

文件：`drivers/nvme/host/core.c` L457-L496

```
nvme_complete_rq(req)                            [core.c]
  │
  ├─ trace_nvme_complete_rq(req)                 tracepoint
  ├─ nvme_cleanup_cmd(req)                     清理特殊载荷（discard page 等）
  │
  └─ switch (nvme_decide_disposition(req)):
        ├─ COMPLETE:   nvme_end_req(req)          → blk_mq_end_request()
        ├─ RETRY:      nvme_retry_req(req)        重新排队
        ├─ FAILOVER:   nvme_failover_req(req)     切换到备份路径
        └─ AUTHENTICATE: 重新认证 + 重试
```

**nvme_decide_disposition 决策逻辑**（[core.c](file:///home/louis/code/linux/drivers/nvme/host/core.c#L399)）：

```
nvme_decide_disposition(req):
  │
  ├─ status == 0                                    → COMPLETE（正常完成）
  │
  ├─ blk_noretry_request(req)                       → COMPLETE（禁止重试）
  │  status & NVME_STATUS_DNR                       → COMPLETE（Do Not Retry）
  │  retries >= nvme_max_retries                    → COMPLETE（超过重试上限）
  │
  ├─ status & NVME_SCT_SC_MASK == NVME_SC_AUTH_REQUIRED  → AUTHENTICATE
  │
  ├─ req->cmd_flags & REQ_NVME_MPATH (多路径请求):
  │     ├─ nvme_is_path_error(status)               → FAILOVER
  │     └─ blk_queue_dying(req->q)                  → FAILOVER
  │
  ├─ !多路径请求:
  │     └─ blk_queue_dying(req->q)                  → COMPLETE（队列已销毁，放弃）
  │
  └─ 其他所有情况                                    → RETRY
```

**关键点**：
- `NVME_STATUS_DNR`（Do Not Retry）位由硬件设置，指示该错误不可重试
- `nvme_max_retries` 默认值为 5，可通过模块参数 `max_retries` 调整
- 多路径模式下，路径错误会触发 FAILOVER 切换到备用路径；非多路径模式则直接返回错误

#### 18.6.3 nvme_end_req — 最终完成块层请求

```c
void nvme_end_req(struct request *req)
{
    blk_status_t status = nvme_status(nvme_re(req)->status);

    __nvme_end_req(req);       // 错误日志 + Zone Append 处理 + bio trace
    blk_mq_end_request(req, status);  // → blk_update_request() → bio_endio()
}
```

---

### 18.7 NVMe 驱动：open/release 详细流程

#### 18.7.1 open 路径

```
用户态 open("/dev/nvme0n1", O_RDWR)
  └─ syscall
        └─ file_open_name() → do_dentry_open()
              └─ blkdev_open(inode, filp)                        [block/fops.c L674]
                    │
                    ├─ 1. file_to_blk_mode(filp)  将文件标志转为块设备打开模式
                    │     └─ FMODE_READ → BLK_OPEN_READ
                    │     └─ FMODE_WRITE → BLK_OPEN_WRITE
                    │     └─ O_EXCL → BLK_OPEN_EXCL
                    │
                    ├─ 2. bdev_permission(dev, mode, holder)  权限检查
                    │
                    ├─ 3. blkdev_get_no_open(dev, true)  获取/创建 bdev
                    │     └─ 查找或创建 block_device 实例
                    │
                    ├─ 4. bdev_open(bdev, mode, holder, NULL, filp)
                    │     │
                    │     ├─ blkdev_get_whole(bdev, mode, holder)
                    │     │     ├─ 增加 bdev->bd_holders（排他打开检查）
                    │     │     └─ disk->fops->open(disk, mode)  → nvme_open()
                    │     │
                    │     └─ bdev_set_file_inode(filp, bdev)
                    │           └─ filp->f_mapping = bdev->bd_inode->i_mapping
                    │
                    └─ 5. 设置文件能力标志
                          ├─ 支持 atomic write → FMODE_CAN_ATOMIC_WRITE
                          └─ 有 integrity → FMODE_HAS_METADATA

nvme_open(disk, mode)                                          [core.c L1801]
  └─ nvme_ns_open(disk->private_data)                          [core.c L1775]
        ├─ WARN_ON_ONCE(nvme_ns_head_multipath(ns->head))  多路径盘不可直接打开
        ├─ nvme_get_ns(ns)                                  增加 namespace 引用计数
        └─ try_module_get(ns->ctrl->ops->module)             防止驱动模块卸载
```

#### 18.7.2 release 路径

```
用户态 close(fd)
  └─ syscall
        └─ __close_fd() → filp_close()
              └─ blkdev_release(inode, filp)                    [block/fops.c L731]
                    └─ bdev_release(filp)
                          ├─ blkdev_put_whole(bdev_whole(bdev))
                          │     ├─ disk->fops->release(disk)  → nvme_release()
                          │     └─ 减少 bd_holders
                          └─ blkdev_put_no_open(bdev)
                                └─ 减少引用计数，可能释放 bdev

nvme_release(disk)                                             [core.c L1806]
  └─ nvme_ns_release(disk->private_data)                      [core.c L1797]
        ├─ module_put(ns->ctrl->ops->module)                  允许驱动模块卸载
        └─ nvme_put_ns(ns)                                   减少 namespace 引用计数
```

---

### 18.8 批量提交优化

#### 18.8.1 plug 机制

```c
/* 用户态通过 io_uring 或 libaio 批量提交时 */

struct blk_plug *plug = current->plug;
// 如果 plug 存在，多个 request 先缓存到 plug 中
blk_add_rq_to_plug(plug, rq);   // 不会立即下发

// 当 plug 被 flush 时，批量下发
blk_mq_flush_plug_list(plug, ...)
  └─ blk_mq_issue_direct(rqs)    批量下发
        └─ 对每个 request:
              └─ blk_mq_request_issue_directly(rq, last)
                    └─ nvme_queue_rq(hctx, &bd)
                          └─ nvme_write_sq_db(nvmeq, bd->last)
                              // bd->last=true 才写 DB，减少 DB 写入次数
```

#### 18.8.2 nvme_queue_rqs 批量提交

```
nvme_queue_rqs(rqlist)                                    [pci.c]
  │
  ├─ 遍历 rqlist，按队列分组
  │     ├─ 对每个 rq：nvme_prep_rq_batch(nvmeq, req)    准备命令
  │     └─ 收集到 submit_list
  │
  └─ nvme_submit_cmds(nvmeq, &submit_list)              批量提交
        ├─ spin_lock(&nvmeq->sq_lock)
        ├─ while (rq = rq_list_pop(rqlist)):
        │     nvme_sq_copy_cmd(nvmeq, &iod->cmd)         拷贝命令到 SQ
        └─ nvme_write_sq_db(nvmeq, true)                 一次写 DB
        spin_unlock(&nvmeq->sq_lock)
```

#### 18.8.3 完成批处理

```c
// 中断处理中批量消费 CQE
DEFINE_IO_COMP_BATCH(iob);       // 定义完成批处理

nvme_poll_cq(nvmeq, &iob)
  └─ while (nvme_cqe_pending(nvmeq)):
        nvme_handle_cqe(nvmeq, &iob, nvmeq->cq_head)
          └─ nvme_try_complete_req(req, status, result)
                └─ blk_mq_add_to_batch(req, &iob, ...)   加入批处理
                      // 多个 CQE 收集到 iob 中

// 退出循环后统一处理
nvme_pci_complete_batch(&iob)
  └─ nvme_complete_batch(&iob, nvme_pci_unmap_rq)
        └─ 遍历 iob 中的请求，批量完成
```

---

### 18.9 涉及的文件清单

| 文件 | 作用 |
|------|------|
| `block/fops.c` | 块设备文件操作：`blkdev_open()`, `blkdev_read_iter()`, `blkdev_write_iter()` |
| `block/blk-core.c` | `submit_bio()`, `submit_bio_noacct()` |
| `block/blk-mq.c` | `blk_mq_submit_bio()`, `blk_mq_dispatch_rq_list()`, `blk_mq_request_issue_directly()` |
| `drivers/nvme/host/pci.c` | `nvme_queue_rq()`, `nvme_queue_rqs()`, `nvme_prep_rq()`, `nvme_map_data()`, `nvme_irq()`, `nvme_poll_cq()`, `nvme_handle_cqe()` |
| `drivers/nvme/host/core.c` | `nvme_setup_cmd()`, `nvme_setup_rw()`, `nvme_complete_rq()`, `nvme_end_req()`, `nvme_open()`, `nvme_release()` |
| `drivers/nvme/host/nvme.h` | `struct nvme_request`, `struct nvme_command`, `struct nvme_rw_command` |

---

*分析日期：2026-07-15*
*内核版本：Linux 7.0 (ARM64)*
*分析范围：block/ 目录下全部 55 个 .c 文件 + 16 个 .h 文件 + partitions/ 子目录 18 个 .c 文件 + drivers/nvme/host/ 核心文件*