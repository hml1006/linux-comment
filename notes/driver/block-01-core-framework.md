# 块层 — 核心框架 (Part I)

> 本文档拆分自 [block_layer_analysis.md](block_layer_analysis.md) Part I，涵盖总体概览、核心数据结构、请求队列管理、Bio层、多队列框架(blk-mq)

# Linux 7.0 块层（Block Layer）代码分析报告

## 目录

### Part I: 核心框架

1. [总体概览](#1-总体概览)
    - [1.1 文件统计](#11-文件统计)
    - [1.2 代码规模排名（Top 15）](#12-代码规模排名top-15)
2. [核心数据结构](#2-核心数据结构)
    - [2.1 关键头文件](#21-关键头文件)
    - [2.2 队列标志位](#22-队列标志位)
3. [请求队列管理（Request Queue）](#3-请求队列管理)
    - [3.1 blk-core.c（1,410 行）](#31-blk-corec1410-行)
    - [3.2 blk-settings.c（1,062 行）](#32-blk-settingsc1062-行)
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
    - [5.1 架构概述：单队列 vs 多队列](#51-架构概述单队列-vs-多队列)
    - [5.2 核心数据结构](#52-核心数据结构)
    - [5.3 CPU 到硬件队列的映射](#53-cpu-到硬件队列的映射)
    - [5.4 Tag 分配与管理](#54-tag-分配与管理)
    - [5.5 请求分配流程](#55-请求分配流程)
    - [5.6 I/O 提交路径：`blk_mq_submit_bio`](#56-io-提交路径blk_mq_submit_bio)
    - [5.7 请求插入与派发](#57-请求插入与派发)
    - [5.8 命令派发：`blk_mq_dispatch_rq_list`](#58-命令派发blk_mq_dispatch_rq_list)
    - [5.9 完成路径](#59-完成路径)
    - [5.10 硬件队列管理](#510-硬件队列管理)
    - [5.11 多队列调度框架（blk-mq-sched）](#511-多队列调度框架blk-mq-sched)
    - [5.12 辅助模块总览](#512-辅助模块总览)
    - [5.13 完整 I/O 生命周期（blk-mq 视角）](#513-完整-io-生命周期blk-mq-视角)
    - [5.14 Bio、Request、Tag、TagSet、软件队列与硬件队列的关系](#514-biorequesttagtagset软件队列与硬件队列的关系)

---

## Part I: 核心框架

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

（[blk_types.h](file:///home/louis/code/linux/include/linux/blk_types.h)）表示一个块 I/O 请求：

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

（[bio.h](file:///home/louis/code/linux/include/linux/bio.h)）用于遍历 bio 中的数据段：

```c
struct bvec_iter {
    sector_t    bi_sector;      /* 当前设备扇区号 */
    unsigned int bi_size;       /* 剩余未处理字节数 */
    unsigned int bi_idx;        /* 当前 bio_vec 索引 */
    unsigned int bi_bvec_done;  /* 当前 bio_vec 中已处理字节数 */
};
```

**遍历宏**（[bio.h](file:///home/louis/code/linux/include/linux/bio.h)）：
```c
// 标准遍历（遵守 bi_iter 偏移）
#define bio_for_each_segment(bvl, bio, iter) ...

// 遍历所有 bio_vec（忽略偏移，驱动不可用）
#define bio_for_each_segment_all(bvl, bio, iter) ...

// 遍历所有 folio（多页 bio_vec 支持）
#define bio_for_each_folio_all(fi, bio) ...
```

#### 4.2.4 BIO 标志（`bi_flags`）

（[blk_types.h](file:///home/louis/code/linux/include/linux/blk_types.h)）

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

（[blk_types.h](file:///home/louis/code/linux/include/linux/blk_types.h)）

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

（[bio.h](file:///home/louis/code/linux/include/linux/bio.h)）管理 bio 和 bio_vec 的内存池：

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

（[bio.h](file:///home/louis/code/linux/include/linux/bio.h)）用于 bio 的批量管理：

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

**分配流程**（[bio.c](file:///home/louis/code/linux/block/bio.c) `bio_alloc_bioset`）：

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

**per-CPU 缓存机制**（[bio.c](file:///home/louis/code/linux/block/bio.c)）：
- `bio_alloc_cache`：每 CPU 一个缓存实例
- `free_list`：进程上下文释放的 bio 链表
- `free_list_irq`：中断上下文释放的 bio 链表（需要关中断访问）
- 阈值 `ALLOC_CACHE_THRESHOLD=16`：当 `free_list_irq` 积累超过 16 个时搬入 `free_list`
- 最大 `ALLOC_CACHE_MAX=256`：超过则直接释放回 mempool

**bio_vec slab 分级**（[bio.c](file:///home/louis/code/linux/block/bio.c)）：

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

**释放流程**（[bio.c](file:///home/louis/code/linux/block/bio.c)）：

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

**新分配**（[bio.c](file:///home/louis/code/linux/block/bio.c)）：
```c
void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,
              unsigned short max_vecs, blk_opf_t opf);
```
- 清零所有字段，设置 `__bi_cnt=1`、`__bi_remaining=1`
- 设置 `bi_bdev`、`bi_opf`、`bi_io_vec`、`bi_max_vecs`

**重置**（[bio.c](file:///home/louis/code/linux/block/bio.c)）：
```c
void bio_reset(struct bio *bio, struct block_device *bdev, blk_opf_t opf);
```
- 保留 `bi_io_vec` 指针，清零 `BIO_RESET_BYTES` 之前的字段
- 用于 bio 池中取出的 bio 重新初始化

**复用**（[bio.c](file:///home/louis/code/linux/block/bio.c)）：
```c
void bio_reuse(struct bio *bio, blk_opf_t opf);
```
- 保留 `bi_io_vec` 数据、`bi_end_io`、`bi_private`
- 重新计算 `bi_iter.bi_size`（遍历所有 bio_vec 求和）
- 典型场景：读取数据后直接写入另一个位置

### 4.5 向 Bio 添加数据

#### 4.5.1 `bio_add_page` — 添加页

（[bio.c](file:///home/louis/code/linux/block/bio.c)）尝试将一页数据添加到 bio：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）用于直接 I/O 路径：

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
submit_bio(bio)                              [bio.c]
  │  task_io_account_read/write()  ← 进程 I/O 统计
  │  bio_set_ioprio()               ← 设置 I/O 优先级
  │
  ▼
submit_bio_noacct(bio)                       [blk-core.c]
  │  检查 REQ_NOWAIT / 加密 / 故障注入
  │  bio_check_ro()                  ← 只读设备检查
  │  bio_check_eod()                 ← 越界检查
  │  blk_partition_remap()           ← 分区 LBA 重映射
  │  Flush 检查：bdev_write_cache()
  │  blk_throtl_bio()                ← 节流控制
  │
  ▼
submit_bio_noacct_nocheck(bio, false)        [blk-core.c]
  │  blk_cgroup_bio_start()          ← cgroup 记账
  │  trace_block_bio_queue()         ← trace 入队
  │
  ├─ current->bio_list 不为空（堆叠驱动递归提交）
  │    → bio_list_add() 加入链表，延迟处理
  │
  └─ 否则 → __submit_bio_noacct_mq(bio)
       │
       ▼
     __submit_bio(bio)                       [blk-core.c]
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

（[blk-core.c](file:///home/louis/code/linux/block/blk-core.c)）

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

（[blk-core.c](file:///home/louis/code/linux/block/blk-core.c)）执行以下检查：

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

（[blk-core.c](file:///home/louis/code/linux/block/blk-core.c)）

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）允许将多个 bio 串联，父 bio 在所有子 bio 完成后才通知上层：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）

```c
struct bio *bio_alloc_clone(struct block_device *bdev, struct bio *bio_src,
                            gfp_t gfp, struct bio_set *bs);
```

- 分配新 bio，共享 `bio_src->bi_io_vec`（不复制数据）
- 设置 `BIO_CLONED` 标志：克隆 bio 不拥有 bio_vec 的所有权
- 克隆 bio 不可调用 `bio_add_page()`（`BIO_CLONED` 检查）

#### 4.9.2 `bio_split` — 分割 bio

（[bio.c](file:///home/louis/code/linux/block/bio.c)）

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）

```c
void bio_trim(struct bio *bio, sector_t offset, sector_t size);
```
- 从 bio 中裁剪出指定范围和长度的子区域
- 通过 `bio_advance()` 前移 + 设置 `bi_size` 实现

### 4.10 Bio 数据拷贝

#### 4.10.1 `bio_copy_data` — 全量拷贝

（[bio.c](file:///home/louis/code/linux/block/bio.c)）

```c
void bio_copy_data(struct bio *dst, struct bio *src);
```

- 拷贝 `min(src->bi_size, dst->bi_size)` 字节
- 逐个 bio_vec 对比，使用 `bvec_kmap_local()` 临时映射

#### 4.10.2 `bio_copy_data_iter` — 迭代器拷贝

（[bio.c](file:///home/louis/code/linux/block/bio.c)）

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

**释放**（[bio.c](file:///home/louis/code/linux/block/bio.c)）：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）处理直接 I/O 读取的脏页问题：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）当需要对数据进行额外处理（如校验和）时使用：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）

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
  ├─ submit_bio(bio)                    [blk-core.c]
  │    ├─ task_io_account_write()       进程统计
  │    └─ submit_bio_noacct(bio)        [blk-core.c]
  │         ├─ bio_check_ro()           只读检查
  │         ├─ bio_check_eod()          越界检查
  │         ├─ blk_partition_remap()    分区重映射
  │         └─ submit_bio_noacct_nocheck()[blk-core.c]
  │              └─ __submit_bio_noacct_mq(bio)
  │                   └─ __submit_bio(bio) [blk-core.c]
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
  │              └─ bio_endio(bio)          [bio.c]
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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）当 bio 的最后几个扇区超出设备容量时，截断 bio 而不是直接返回错误，允许对设备尾部非对齐扇区进行 I/O：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）将 bio 截断到指定大小，对于读操作将超出部分清零：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）绕过 mempool，直接用 kmalloc 分配 bio（含内联 bio_vec）：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）在调用者提供的内存上初始化一个克隆 bio，共享源 bio 的 `bi_io_vec`：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）在直接 I/O 写入前，将 bio 中所有页面标记为脏页：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）提交 bio 并等待其及所有子 bio 完成：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）对块设备执行同步读写，数据位于内核直接映射区：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）从指定迭代器位置开始，将 bio 数据清零：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）遍历 bio 中所有 bio_vec，释放每个页面：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）将 vmalloc 区域的数据添加到 bio：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）每个 CPU 拥有一个缓存实例，加速 bio 分配与释放：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c) `bio_alloc_percpu_cache`）：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c) `bio_put_percpu_cache`）：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）当 CPU 下线时，通过 `CPUHP_BIO_DEAD` 回调清理该 CPU 的缓存：

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

（[bio.c](file:///home/louis/code/linux/block/bio.c)）在 `bioset_exit()` 时调用，注销 CPU 热插拔回调，遍历所有 possible CPU 释放缓存 bio，最后释放 per-CPU 内存。

#### 4.18.6 全局 bio 池初始化

（[bio.c](file:///home/louis/code/linux/block/bio.c)）系统启动时通过 `subsys_initcall(init_bio)` 初始化：

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

（[blk-mq.h](file:///home/louis/code/linux/block/blk-mq.h)）每个 CPU 拥有一个软件队列上下文，用于接收来自该 CPU 的 I/O 提交：

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

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h)）每个硬件队列对应设备的一个提交队列（如 NVMe SQ），是实际向硬件提交命令的实体：

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

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h)）`blk_mq_tag_set` 是设备级的配置，一个设备有一个 `tag_set`，可以跨多个 `request_queue` 共享（如 NVMe 多命名空间）：

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

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h)）每个硬件队列拥有一个 tag 池，tag 是硬件命令标识符，类似于 NVMe 的 Command ID：

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

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h)）驱动通过 `blk_mq_ops` 向块层注册回调，实现硬件交互：

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

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h)）`mq_map` 是一个长度为 `nr_cpu_ids` 的数组，`mq_map[cpu]` 返回该 CPU 应使用的硬件队列编号：

```c
struct blk_mq_queue_map {
    unsigned int *mq_map;       // CPU ID → 硬件队列索引的映射表
    unsigned int nr_queues;     // 此类型硬件队列的数量
    unsigned int queue_offset;  // 硬件队列偏移（如 NVMe 将 poll 队列放在后面）
};
```

#### 5.2.7 队列类型（`hctx_type`）

（[blk-mq.h](file:///home/louis/code/linux/include/linux/blk-mq.h)）硬件队列按功能分为三种类型：

```c
enum hctx_type {
    HCTX_TYPE_DEFAULT,          // 默认队列（处理写请求和未分类的请求）
    HCTX_TYPE_READ,             // 读队列（只有读请求路由到此）
    HCTX_TYPE_POLL,             // 轮询队列（处理 REQ_POLLED 请求）
    HCTX_MAX_TYPES,             // 类型总数 = 3
};
```

**路由规则**（[blk-mq.h](file:///home/louis/code/linux/block/blk-mq.h)）：

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

（[blk-mq-cpumap.c](file:///home/louis/code/linux/block/blk-mq-cpumap.c)）`blk_mq_map_queues()` 建立 CPU 与硬件队列的映射关系：

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

**查找硬件队列**（[blk-mq.h](file:///home/louis/code/linux/block/blk-mq.h)）：

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

（[blk-mq-tag.c](file:///home/louis/code/linux/block/blk-mq-tag.c)）`blk_mq_get_tag()` 从 `sbitmap_queue` 中分配 tag：

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

（[blk-mq-tag.c](file:///home/louis/code/linux/block/blk-mq-tag.c)）`blk_mq_put_tag()` 在请求完成时释放 tag，唤醒等待者：

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

**批量释放优化**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）：`blk_mq_end_request_batch()` 批量收集最多 32 个完成的 tag（`TAG_COMP_BATCH`），一次性调用 `blk_mq_put_tags()` 释放，减少锁操作。

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

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）`blk_mq_get_new_requests()` 在 `blk_mq_submit_bio` 中被调用，为 bio 分配 request：

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

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）`blk_mq_peek_cached_request()` 实现请求缓存复用：

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

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）这是块层 I/O 提交的入口，每个 bio 都经过此函数转换为 request 并投递到队列：

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

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）根据请求类型和队列配置，选择不同的插入路径：

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

**`blk_mq_hctx_mark_pending`**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）：

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

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）触发硬件队列处理待派发请求：

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

（[blk-mq-sched.c](file:///home/louis/code/linux/block/blk-mq-sched.c)）这是真正的派发引擎：

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

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）将请求列表逐个提交给硬件驱动：

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

**`queue_rqs` 批量提交**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）：`blk_mq_dispatch_multiple_queue_requests()` 按 queue 分组请求列表，每组调用 `queue_rqs()` 一次提交多个命令，NVMe 驱动利用此接口实现高效 Doorbell 更新。

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

（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）驱动完成命令后，调用此函数终结请求：

```c
void blk_mq_end_request(struct request *rq, blk_status_t error)
{
    if (blk_update_request(rq, error, blk_rq_bytes(rq)))
        BUG();                          // 更新已传输字节数
    __blk_mq_end_request(rq, error);    // 释放 tag，通知上层
}
```

**批量完成**（[blk-mq.c](file:///home/louis/code/linux/block/blk-mq.c)）：`blk_mq_end_request_batch()` 批量处理完成请求，最多收集 32 个 tag 后一次性释放，大幅减少锁操作。

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

### 5.14 Bio、Request、Tag、TagSet、软件队列与硬件队列的关系

#### 5.14.1 概述

blk-mq 框架中，**bio → request → tag → hw queue** 构成了一条完整的 I/O 提交链。六个核心组件的关系如下：

```
┌─────────────────────────────────────────────────────────────────────┐
│  blk_mq_tag_set (tagset)                                            │
│  ┌─────────────────────────────────────────────────────────────────┐│
│  │  nr_hw_queues = N, queue_depth = D, cmd_size = S               ││
│  │                                                                 ││
│  │  tags[0] ──→ blk_mq_tags (hwq 0) ──→ rqs[0..D-1]               ││
│  │  tags[1] ──→ blk_mq_tags (hwq 1) ──→ rqs[0..D-1]               ││
│  │  tags[2] ──→ blk_mq_tags (hwq 2) ──→ rqs[0..D-1]               ││
│  │  ...                                                             ││
│  │  tags[N-1] ─→ blk_mq_tags (hwq N-1)                             ││
│  └─────────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────────┘
         │                    │
         ▼                    ▼
  ┌──────────────┐   ┌──────────────┐
  │ blk_mq_hw_ctx│   │ blk_mq_hw_ctx│  ← 硬件队列 (每队列一个独立 tags)
  │  .tags = ────┼───┤  .tags = ────┼──→ blk_mq_tags
  │  .ctxs[]     │   │  .ctxs[]     │
  │  .ctx_map    │   │  .ctx_map    │
  │  .dispatch   │   │  .dispatch   │
  └──────┬───────┘   └──────┬───────┘
         │                  │
   ┌─────┴──────┐    ┌──────┴──────┐
   │ blk_mq_ctx │    │ blk_mq_ctx  │  ← 软件队列 (per-CPU)
   │ .rq_lists[]│    │ .rq_lists[] │
   │ .hctxs[]   │    │ .hctxs[]    │
   └─────┬──────┘    └──────┬──────┘
         │                  │
         ▼                  ▼
  ┌──────────────┐   ┌──────────────┐
  │   request    │   │   request    │  ← 请求 (包含 tag + bio)
  │  .tag        │   │  .tag        │
  │  .mq_ctx     │   │  .mq_ctx     │
  │  .mq_hctx    │   │  .mq_hctx    │
  │  .bio        │   │  .bio        │
  └──────┬───────┘   └──────┬───────┘
         │                  │
         ▼                  ▼
  ┌──────────────┐   ┌──────────────┐
  │     bio      │   │     bio      │  ← 块 I/O 单元 (来自文件系统)
  └──────────────┘   └──────────────┘
```

**核心关系总结**：

| 组件 | 数据结构 | 数量关系 | 核心作用 |
|------|---------|---------|---------|
| **bio** | `struct bio` | 一个 request 包含 1~N 个 bio | 文件系统/上层提交的 I/O 单元 |
| **request** | `struct request` | 一个 tag 对应一个 request | 块层内部 I/O 描述符，是驱动处理的单位 |
| **tag** | `int` (索引) | 0 ~ queue_depth-1 | 唯一标识 in-flight request，用于 DMA 完成索引 |
| **blk_mq_tags** | `struct blk_mq_tags` | 每个 hw queue 一个 tags | 管理 tag 位图（sbitmap）和 request 指针数组 |
| **blk_mq_tag_set** | `struct blk_mq_tag_set` | 每个设备一个 tagset | 管理所有 tags 的集合，驱动初始化时配置 |
| **软件队列** | `struct blk_mq_ctx` | per-CPU | 接收请求的入口，无锁合并 |
| **硬件队列** | `struct blk_mq_hw_ctx` | 1~N（对应硬件提交队列） | 实际向硬件提交命令的实体 |

#### 5.14.2 bio → request 转换

**入口**：`blk_mq_submit_bio()` (block/blk-mq.c)

```
blk_mq_submit_bio(bio)
  │
  ├─ [1] 获取当前 CPU 的软件队列和对应的硬件队列
  │     ctx = blk_mq_get_ctx(q)              ← 当前 CPU 的 blk_mq_ctx
  │     hctx = blk_mq_map_queue(cmd_flags, ctx) ← ctx → hctx 映射
  │
  ├─ [2] 尝试与已有 request 合并
  │     blk_mq_attempt_bio_merge(q, bio, nr_segs)
  │     ├─ 与调度器队列中的 request 合并
  │     └─ 与 plug 链表中的 request 合并
  │     → 合并成功则直接返回，无需分配新 request
  │
  ├─ [3] 分配 request + tag
  │     rq = blk_mq_get_new_requests(q, plug, bio)
  │       └─ __blk_mq_alloc_requests(&data)
  │           ├─ data->ctx = blk_mq_get_ctx(q)            ← 确定软件队列
  │           ├─ data->hctx = blk_mq_map_queue(...)   ← 确定硬件队列
  │           ├─ tag = blk_mq_get_tag(data)            ← 从 tags 的 sbitmap 分配 tag
  │           │     └─ sbitmap_queue_get(&tags->bitmap_tags)
  │           │        → 返回 tag 编号 (0 ~ queue_depth-1)
  │           └─ rq = blk_mq_rq_ctx_init(data, tags, tag)
  │               ├─ rq->tag = tag                     ← 设置 tag 索引
  │               ├─ rq->mq_ctx = ctx                  ← 绑定软件队列
  │               └─ rq->mq_hctx = hctx                ← 绑定硬件队列
  │
  ├─ [4] 绑定 bio 到 request
  │     blk_mq_bio_to_request(rq, bio, nr_segs)
  │     ├─ rq->bio = bio                               ← bio 链表头
  │     ├─ rq->biotail = bio                           ← bio 链表尾
  │     ├─ rq->__sector = bio->bi_iter.bi_sector       ← 起始扇区
  │     └─ rq->__data_len = bio->bi_iter.bi_size       ← 数据长度
  │
  ├─ [5] 提交路径选择
  │     ├─ 有 plug → blk_add_rq_to_plug(plug, rq)      ← 批量缓存
  │     ├─ 有调度器 → blk_mq_insert_request(rq, 0)     ← 入软件队列
  │     │              └─ blk_mq_run_hw_queue(hctx, true)
  │     └─ 无调度器 → blk_mq_try_issue_directly(hctx, rq) ← 直接下发
  │
  └─ [6] 最终派发
        blk_mq_dispatch_rq_list(hctx, &list)
          └─ hctx->ops->queue_rq(hctx, &bd)            ← 驱动处理
```

**key insight**：bio 是"请求描述"，request 是"请求容器"。一个 request 可以包含多个 bio（通过合并），但只对应一个 tag（硬件命令槽位）。

#### 5.14.3 Request 内部结构

文件：`include/linux/blk-mq.h` L105-L184

```c
struct request {
    struct request_queue *q;        // 所属请求队列
    struct blk_mq_ctx *mq_ctx;      // 软件队列 (per-CPU)
    struct blk_mq_hw_ctx *mq_hctx;  // 硬件队列

    blk_opf_t cmd_flags;            // 操作码 + 标志位
    req_flags_t rq_flags;           // 内部标志 (RQF_*)

    int tag;                        // 驱动 tag ← 核心！用于硬件索引
    int internal_tag;               // 调度器 tag（有调度器时使用）

    struct bio *bio;                // bio 链表头
    struct bio *biotail;            // bio 链表尾

    union {
        struct list_head queuelist; // 链表节点（软件队列/调度队列/plug 链表）
        struct request *rq_next;    // rq_list 链表
    };

    unsigned short nr_phys_segments; // DMA 段数 (合并后)

    enum mq_rq_state state;          // 请求状态: MQ_RQ_IDLE/IN_FLIGHT/COMPLETE
    atomic_t ref;                    // 引用计数
};
```

**tag 与 internal_tag 的区别**：

```
无 I/O 调度器 (NONE):
  rq->tag = 驱动 tag (从 hctx->tags->bitmap_tags 分配)
  rq->internal_tag = BLK_MQ_NO_TAG

有 I/O 调度器 (BFQ/Kyber):
  rq->tag = BLK_MQ_NO_TAG          ← 分配时暂不取驱动 tag
  rq->internal_tag = 调度器 tag     ← 从 sched_tags 分配
  → 派发到硬件时: __blk_mq_alloc_driver_tag(rq) 补充分配驱动 tag
```

#### 5.14.4 Tag 分配机制

##### blk_mq_tags — Tag 管理器

文件：`include/linux/blk-mq.h` L775-L792

```c
struct blk_mq_tags {
    unsigned int nr_tags;            // 总 tag 数 (queue_depth)
    unsigned int nr_reserved_tags;   // 保留 tag 数 (用于内存回收等紧急场景)
    unsigned int active_queues;      // 活跃队列数 (共享 tags 时使用)

    struct sbitmap_queue bitmap_tags;    // 主 tag 位图
    struct sbitmap_queue breserved_tags; // 保留 tag 位图

    struct request **rqs;            // tag → request 指针数组
    struct request **static_rqs;     // 静态 request 指针数组（初始化时分配）
    struct list_head page_list;      // 分配 request 的页面链表

    spinlock_t lock;
    struct rcu_head rcu_head;
};
```

**sbitmap 位图分配**：

```
tag 位图 (sbitmap_queue):
  ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
  │ 1 │ 0 │ 1 │ 1 │ 0 │ 1 │ 0 │ 0 │ 1 │ 0 │  ← 0=空闲, 1=已用
  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
    0   1   2   3   4   5   6   7   8   9
                       ↑
                 分配 tag=4 (把 1 置为 0)

rqs[] 数组:
  ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┐
  │ rq0 │ rq1 │ rq2 │ rq3 │ rq4 │ rq5 │ ... │  ← tag 作为数组索引
  └─────┴─────┴─────┴─────┴─────┴─────┴─────┘
    0     1     2     3     4     5

static_rqs[] 数组: 预分配的 request 对象池（初始化时一次性分配）
  ┌─────┬─────┬─────┬─────┬─────┬─────┐
  │ rq0 │ rq1 │ rq2 │ rq3 │ rq4 │ rq5 │  ← 永不释放
  └─────┴─────┴─────┴─────┴─────┴─────┘
```

**分配流程** (`blk_mq_get_tag`, block/blk-mq-tag.c)：

```
blk_mq_get_tag(data)
  │
  ├─ tags = blk_mq_tags_from_data(data)  ← 从 hctx 获取 tags
  │
  ├─ bt = &tags->bitmap_tags              ← 主位图
  │
  ├─ tag = __sbitmap_queue_get(bt)        ← CAS 原子操作获取位
  │   ├─ 成功 → tag 编号 (0 ~ nr_tags-1)
  │   └─ 失败 → BLK_MQ_NO_TAG
  │
  ├─ [如果 tag 耗尽]
  │   ├─ blk_mq_run_hw_queue(hctx, false)  ← 触发完成，释放 tag
  │   ├─ tag = __sbitmap_queue_get(bt)      ← 重试
  │   └─ 仍失败 → io_schedule() 睡眠等待
  │
  └─ 返回 tag
```

**tag 的唯一性保证**：

- `blk_mq_unique_tag(rq)` 编码 `(hwq_number << 16) | tag`，在系统范围内唯一
  ```c
  u32 blk_mq_unique_tag(struct request *rq)
  {
      return (rq->mq_hctx->queue_num << BLK_MQ_UNIQUE_TAG_BITS) |
              (rq->tag & BLK_MQ_UNIQUE_TAG_MASK);
  }
  ```
- 驱动通过 CQE 中的 tag 字段直接索引 `tags->rqs[tag]` 找到完成的 request，O(1) 完成查找

#### 5.14.5 TagSet — Tag 集合管理

文件：`include/linux/blk-mq.h` L534-L560

```c
struct blk_mq_tag_set {
    const struct blk_mq_ops *ops;        // blk-mq 操作函数表
    struct blk_mq_queue_map map[HCTX_MAX_TYPES]; // CPU→hw queue 映射表
    unsigned int nr_maps;               // 映射类型数 (1~3)
    unsigned int nr_hw_queues;          // 硬件队列数
    unsigned int queue_depth;           // 每队列深度 (tag 总数)
    unsigned int reserved_tags;         // 保留 tag 数
    unsigned int cmd_size;              // 每个 request 的私有数据大小
    int numa_node;                      // NUMA 节点
    unsigned int timeout;               // 超时时间
    unsigned int flags;                 // BLK_MQ_F_* 标志
    void *driver_data;                  // 驱动私有数据

    struct blk_mq_tags **tags;          // tags[NR_HW_QUEUES] 数组
    struct blk_mq_tags *shared_tags;    // 共享 tags (可选)

    // ... 内部管理字段 ...
};
```

**NVMe 驱动的 TagSet 初始化** (`nvme_alloc_io_tag_set`, drivers/nvme/host/core.c)：

```c
int nvme_alloc_io_tag_set(struct nvme_ctrl *ctrl, struct blk_mq_tag_set *set, ...)
{
    set->ops = ops;                     // nvme_mq_ops
    set->queue_depth = min(ctrl->sqsize, BLK_MQ_MAX_DEPTH - 1);  // 通常是 1023
    set->nr_hw_queues = ctrl->queue_count - 1;  // 减去 admin queue
    set->nr_maps = nr_maps;             // 1 (default) / 2 (read) / 3 (poll)
    set->cmd_size = sizeof(struct nvme_iod);  // 每个 request 额外空间
    set->driver_data = ctrl;

    return blk_mq_alloc_tag_set(set);   // 核心分配函数
}
```

**blk_mq_alloc_tag_set 的核心工作**：

```
blk_mq_alloc_tag_set(set)
  │
  ├─ [1] 为每个 hw queue 分配一个 blk_mq_tags
  │     for each hctx_idx in 0..nr_hw_queues-1:
  │       tags[hctx_idx] = blk_mq_alloc_map_and_rqs(set, hctx_idx, depth)
  │         ├─ blk_mq_init_tags(depth, reserved, flags, node)
  │         │   └─ 初始化 sbitmap_queue (bitmap_tags + breserved_tags)
  │         └─ blk_mq_alloc_rqs(set, tags, hctx_idx, depth)
  │             ├─ 分配 depth 个 struct request 对象
  │             │   (每个 request 尾部跟随 cmd_size 字节的 PDU)
  │             ├─ tags->static_rqs[i] = rq  ← 永久绑定
  │             └─ tags->rqs[i] = rq          ← 运行时绑定
  │
  ├─ [2] 分配 request 的内存布局
  │     ┌──────────────────────────────────────┐
  │     │  struct request (sizeof = ~224 bytes) │  ← 块层通用部分
  │     ├──────────────────────────────────────┤
  │     │  struct nvme_iod (cmd_size bytes)     │  ← 驱动私有 PDU
  │     │    ├─ cmd:     nvme_command           │
  │     │    ├─ meta:    DMA metadata           │
  │     │    └─ dma_vec: DMA scatter list       │
  │     └──────────────────────────────────────┘
  │     blk_mq_rq_to_pdu(rq) = rq + 1  ← 获取 PDU 指针
  │
  └─ [3] 初始化每个 hw queue 的 blk_mq_hw_ctx
        hctx->tags = tags[hctx_idx]      ← 关联 tags
        hctx->queue_num = hctx_idx       ← 队列编号
```

**内存布局示例** (NVMe PCIe, queue_depth=1023, nr_hw_queues=8)：

```
blk_mq_tag_set:
  nr_hw_queues = 8
  queue_depth = 1023
  cmd_size = sizeof(struct nvme_iod) ≈ 128 字节

每个 hw queue 的 tags:
  bitmap_tags: sbitmap_queue, 1023 bit
  rqs[1023]:   struct request *[1023]
  static_rqs[1023]: struct request *[1023]

内存总量 ≈ 8 × 1023 × (sizeof(struct request) + sizeof(struct nvme_iod))
         ≈ 8 × 1023 × (224 + 128) ≈ 2.8 MB
```

#### 5.14.6 软件队列 — 请求入口缓存

**结构** (block/blk-mq.h)：

```c
struct blk_mq_ctx {
    struct {
        spinlock_t lock;
        struct list_head rq_lists[HCTX_MAX_TYPES];  // 三种类型: DEFAULT/READ/POLL
    } ____cacheline_aligned_in_smp;

    unsigned int cpu;
    unsigned short index_hw[HCTX_MAX_TYPES];        // 在 hctx->ctx_map 中的位索引
    struct blk_mq_hw_ctx *hctxs[HCTX_MAX_TYPES];    // 映射到的硬件队列
    struct request_queue *queue;
};
```

**核心作用**：

1. **无锁接收**：每个 CPU 独立拥有自己的 `blk_mq_ctx`，`spin_lock(&ctx->lock)` 仅保护本 CPU 的链表，无跨 CPU 竞争
2. **请求暂存**：当有 I/O 调度器时，`blk_mq_insert_request()` 将 request 插入 `ctx->rq_lists[type]`
3. **批量派发**：硬件队列运行时，`blk_mq_flush_busy_ctxs()` 遍历 `hctx->ctx_map` bitmap，将有 pending 请求的 ctx 排出

**插入流程** (无调度器时)：

```c
// blk_mq_insert_request (block/blk-mq.c)
static void blk_mq_insert_request(struct request *rq, blk_insert_t flags)
{
    struct blk_mq_ctx *ctx = rq->mq_ctx;
    struct blk_mq_hw_ctx *hctx = rq->mq_hctx;

    spin_lock(&ctx->lock);
    if (flags & BLK_MQ_INSERT_AT_HEAD)
        list_add(&rq->queuelist, &ctx->rq_lists[hctx->type]);  // 插到头部
    else
        list_add_tail(&rq->queuelist, &ctx->rq_lists[hctx->type]); // 追加到尾部
    blk_mq_hctx_mark_pending(hctx, ctx);  // 设置 hctx->ctx_map 对应位
    spin_unlock(&ctx->lock);
}
```

**排空流程** (硬件队列运行时)：

```
blk_mq_run_hw_queue(hctx, async)
  └─ __blk_mq_run_hw_queue(hctx)
      └─ blk_mq_flush_busy_ctxs(hctx, &rq_list)   ← 遍历 hctx->ctx_map
          │ 对每个设置了 pending 位的 ctx:
          │   spin_lock(&ctx->lock)
          │   list_splice_tail_init(&ctx->rq_lists[type], &rq_list)
          │   sbitmap_clear_bit(&hctx->ctx_map, ctx->index_hw[type])
          │   spin_unlock(&ctx->lock)
          │
          └─ blk_mq_dispatch_rq_list(hctx, &rq_list)  ← 批量派发
```

**hctx->ctx_map 的作用**：

```
hctx->ctx_map (sbitmap, 宽度 = nr_ctx)
  bit 0: ctx[0] 有 pending 请求
  bit 1: ctx[1] 无 pending 请求
  bit 2: ctx[2] 有 pending 请求
  ...

  blk_mq_flush_busy_ctxs 通过 sbitmap_for_each_set 快速定位有请求的 ctx
  → 避免遍历所有 CPU 的 ctx
```

#### 5.14.7 硬件队列 — 命令提交实体

**结构** (include/linux/blk-mq.h)：

```c
struct blk_mq_hw_ctx {
    struct {
        spinlock_t lock;
        struct list_head dispatch;       // 重试队列（资源不足时暂存）
        unsigned long state;             // BLK_MQ_S_* 状态
    } ____cacheline_aligned_in_smp;

    struct delayed_work run_work;        // 延迟运行
    cpumask_var_t cpumask;              // 可运行此 hctx 的 CPU 集合
    struct request_queue *queue;

    struct sbitmap ctx_map;              // 软件队列 pending 位图
    struct blk_mq_ctx *dispatch_from;    // 轮询选择起点
    unsigned int dispatch_busy;          // 派发繁忙度（EWMA）

    struct blk_mq_tags *tags;            // 驱动 tag 管理器 ← 核心！
    struct blk_mq_tags *sched_tags;      // 调度器 tag 管理器（有调度器时）

    unsigned int queue_num;              // 硬件队列编号
    void *driver_data;                   // 驱动私有数据（如 NVMe 的 nvme_queue）
};
```

**hw queue 与 CPU 的映射关系**：

```
NVMe 8 队列示例:
                      CPU 映射
  hctx[0] (queue 0)  ← CPU 0,1,2,3
  hctx[1] (queue 1)  ← CPU 4,5,6,7
  hctx[2] (queue 2)  ← CPU 8,9,10,11
  ...

blk_mq_map_queue(cmd_flags, ctx):
  // 根据 ctx->cpu 和 cmd_flags 类型，查找 CPU 映射表
  // 返回对应的 hctx 指针
  return ctx->hctxs[blk_mq_get_hctx_type(cmd_flags)]
```

**dispatch 链表的作用**：

```
当 blk_mq_dispatch_rq_list 向驱动提交命令时:
  ├─ 成功 (BLK_STS_OK) → 继续提交下一个
  ├─ 资源不足 (BLK_STS_RESOURCE)
  │   └─ 将未提交的请求移到 hctx->dispatch 链表
  │       → 下次运行 hctx 时优先处理 dispatch 链表
  └─ 设备错误 → 结束请求并返回错误
```

#### 5.14.8 完整 I/O 生命周期（关系视角）

```
时间线 ▼
  │
  │  [写入]  write(fd, buf, 4096)
  │                        │
  │                        ▼
  │  文件系统 (ext4) 构造 bio
  │                        │
  │                        ▼
  │  submit_bio(bio)
  │    └─ blk_mq_submit_bio(bio)
  │                        │
  ├─ ① 分配 request ──────┤
  │    │                   │
  │    │ blk_mq_get_ctx(q) → ctx (当前 CPU 的软件队列)
  │    │ blk_mq_map_queue() → hctx (映射到硬件队列)
  │    │ blk_mq_get_tag()  → tag (从 hctx->tags 的 sbitmap 分配)
  │    │ blk_mq_rq_ctx_init() → rq (绑定 ctx, hctx, tag)
  │    │ blk_mq_bio_to_request() → rq->bio = bio
  │    │
  │    │ 此时:
  │    │   rq->tag = 42              ← 硬件命令槽位
  │    │   rq->mq_ctx = ctx          ← 软件队列
  │    │   rq->mq_hctx = hctx        ← 硬件队列
  │    │   rq->bio = bio             ← 块 I/O 数据
  │    │
  ├─ ② 插入软件队列 ────┤
  │    │    (有调度器或非直接下发)
  │    │    blk_mq_insert_request(rq, 0)
  │    │      → list_add_tail(&rq->queuelist, &ctx->rq_lists[type])
  │    │      → blk_mq_hctx_mark_pending(hctx, ctx)
  │    │         → sbitmap_set_bit(&hctx->ctx_map, ctx->index_hw)
  │    │
  ├─ ③ 派发到硬件 ────┤
  │    │    blk_mq_run_hw_queue(hctx, true)
  │    │      → __blk_mq_run_hw_queue(hctx)
  │    │        → blk_mq_flush_busy_ctxs(hctx, &rq_list)
  │    │          → 遍历 ctx_map, 从有 pending 的 ctx 取出 request
  │    │        → blk_mq_dispatch_rq_list(hctx, &rq_list)
  │    │          → hctx->ops->queue_rq(hctx, &bd)
  │    │             → nvme_queue_rq(hctx, bd)
  │    │
  ├─ ④ 驱动处理 ────┤
  │    │    nvme_queue_rq()
  │    │      → rq->tag 作为 NVMe 命令的 Command ID
  │    │      → memcpy 到 SQ 环
  │    │      → writel(db_value)  ← 门铃寄存器
  │    │
  ├─ ⑤ 完成处理 ────┤
  │    │    NVMe 中断 → nvme_irq()
  │    │      → 读取 CQE, 获取 tag = CQE.command_id
  │    │      → rq = blk_mq_tag_to_rq(hctx->tags, tag)
  │    │         = tags->rqs[tag]  ← O(1) 查找！
  │    │      → blk_mq_complete_request(rq)
  │    │        → blk_mq_end_request(rq, error)
  │    │          → blk_mq_put_tag()  ← 释放 tag 回 sbitmap
  │    │          → bio_endio(rq->bio)  ← 通知文件系统
  │    │
  └─ ⑥ 完成 ────
       tag 回到 sbitmap 空闲池
       等待下一个 bio 复用
```

#### 5.14.9 关键设计要点总结

| 设计点 | 说明 |
|-------|------|
| **tag 即索引** | tag 是 `rqs[]` 数组的索引，完成时 O(1) 查找，无需遍历 |
| **tag 即命令槽** | NVMe 中 tag 直接作为 Command ID，硬件 CQE 携带此 ID 标识完成 |
| **per-CPU 软队列** | 每个 CPU 独立 `blk_mq_ctx`，避免锁竞争，批量派发到 hw queue |
| **ctx_map 位图** | `hctx->ctx_map` 快速定位有 pending 请求的 CPU，避免空遍历 |
| **静态 request 池** | `static_rqs[]` 在初始化时一次性分配，永不释放，避免运行时分配开销 |
| **PUD 紧耦合** | `struct request` 尾部跟随驱动私有数据（如 `nvme_iod`），通过 `blk_mq_rq_to_pdu()` 获取 |
| **双 tag 机制** | 有调度器时 `internal_tag` 用于调度排队，`tag` 在派发时才分配，解耦调度与执行 |
| **sbitmap 无锁分配** | `sbitmap_queue_get()` 使用原子 CAS 操作，无需全局锁，支持高并发 |

---