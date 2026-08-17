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

### Part II: I/O 调度与策略控制

6. [I/O 调度器](#6-io-调度器)
    - [6.1 elevator.c — 调度器框架（895 行）](#61-elevatorc-调度器框架895-行)
    - [6.2 BFQ 调度器（Budget Fair Queueing）](#62-bfq-调度器budget-fair-queueing)
    - [6.3 MQ-Deadline 调度器（1,029 行）](#63-mq-deadline-调度器1029-行)
    - [6.4 Kyber 调度器（1,033 行）](#64-kyber-调度器1033-行)
    - [6.5 none 调度器 — 无调度器模式](#65-none-调度器-无调度器模式)
    - [6.6 Plug 机制 — 批量提交优化](#66-plug-机制-批量提交优化)
7. [I/O 合并与分段](#7-io-合并与分段)
    - [7.1 概述](#71-概述)
    - [7.2 合并前置条件](#72-合并前置条件)
    - [7.3 合并方向判断：`blk_try_merge`](#73-合并方向判断blk_try_merge)
    - [7.4 Bio 合并流程](#74-bio-合并流程)
    - [7.5 Request 合并流程：`attempt_merge`](#75-request-合并流程attempt_merge)
    - [7.6 合并的 6 条路径](#76-合并的-6-条路径)
    - [7.7 段管理与 SG 映射](#77-段管理与-sg-映射)
    - [7.8 虚拟边界间隙检查：`bio_will_gap`](#78-虚拟边界间隙检查bio_will_gap)
    - [7.9 Bio 分段流程](#79-bio-分段流程)
    - [7.10 完整合并与分段调用链](#710-完整合并与分段调用链)
8. [刷新与屏障（Flush/FUA）](#8-刷新与屏障flushfua)
    - [8.1 概述](#81-概述)
    - [8.2 屏障（Barrier）与 FUA 深度解析](#82-屏障barrier与-fua-深度解析)
    - [8.3 关键数据结构](#83-关键数据结构)
    - [8.4 刷新序列阶段](#84-刷新序列阶段)
    - [8.5 策略转换：`blk_insert_flush`](#85-策略转换blk_insert_flush)
    - [8.6 三阶段刷新序列](#86-三阶段刷新序列)
    - [8.7 刷新触发机制：`blk_kick_flush`](#87-刷新触发机制blk_kick_flush)
    - [8.8 刷新完成处理：`flush_end_io`](#88-刷新完成处理flush_end_io)
    - [8.9 `blk_flush_complete_seq` — 阶段推进](#89-blk_flush_complete_seq-阶段推进)
    - [8.10 数据阶段完成：`mq_flush_data_end_io`](#810-数据阶段完成mq_flush_data_end_io)
    - [8.11 硬件队列中的刷新路由](#811-硬件队列中的刷新路由)
    - [8.12 NVMe 驱动中的 FUA 支持](#812-nvme-驱动中的-fua-支持)
    - [8.13 用户态接口](#813-用户态接口)
    - [8.14 完整刷新序列调用链](#814-完整刷新序列调用链)
9. [QoS 与资源控制](#9-qos-与资源控制)
    - [9.1 概述：rq_qos 框架架构](#91-概述rq_qos-框架架构)
    - [9.2 blk-throttle.c — 带宽节流（1,849 行）](#92-blk-throttlec-带宽节流1849-行)
    - [9.3 blk-iolatency.c — 延迟控制（1,068 行）](#93-blk-iolatencyc-延迟控制1068-行)
    - [9.4 blk-iocost.c — IO 成本模型（3,551 行）](#94-blk-iocostc-io-成本模型3551-行)
    - [9.5 blk-wbt.c — 写回节流（1,025 行）](#95-blk-wbtc-写回节流1025-行)
    - [9.6 blk-ioprio.c — I/O 优先级（179 行）](#96-blk-ioprioc-io-优先级179-行)
    - [9.7 QoS 策略对比总结](#97-qos-策略对比总结)
10. [Cgroup 集成](#10-cgroup-集成)
    - [10.1 概述](#101-概述)
    - [10.2 核心数据结构](#102-核心数据结构)
    - [10.3 Bio 与 Cgroup 的关联流程](#103-bio-与-cgroup-的关联流程)
    - [10.4 blkg 生命周期管理](#104-blkg-生命周期管理)
    - [10.5 I/O 统计收集与聚合](#105-io-统计收集与聚合)
    - [10.6 策略注册与激活](#106-策略注册与激活)
    - [10.7 诱导延迟机制](#107-诱导延迟机制)
    - [10.8 完整 Bio I/O 路径中的 Cgroup 集成](#108-完整-bio-io-路径中的-cgroup-集成)
    - [10.9 Cgroup 写回集成（CONFIG_CGROUP_WRITEBACK）](#109-cgroup-写回集成config_cgroup_writeback)
    - [10.10 辅助 Cgroup 文件](#1010-辅助-cgroup-文件)
    - [10.11 Cgroup v2 接口汇总](#1011-cgroup-v2-接口汇总)
    - [10.12 `io_cgrp_subsys` — cgroup 子系统定义](#1012-io_cgrp_subsys-cgroup-子系统定义)
    - [10.13 关键设计要点总结](#1013-关键设计要点总结)

### Part III: 设备管理与调试

11. [分区管理](#11-分区管理)
    - [11.1 分区核心（block/partitions/core.c, 732 行）](#111-分区核心blockpartitionscorec-732-行)
    - [11.2 支持的分区格式](#112-支持的分区格式)
    - [11.3 DOS/MBR 分区表详解](#113-dosmbr-分区表详解)
    - [11.4 EFI/GPT 分区表详解](#114-efigpt-分区表详解)
    - [11.5 MBR vs GPT 对比总结](#115-mbr-vs-gpt-对比总结)
12. [数据完整性与加密](#12-数据完整性与加密)
    - [12.1 数据完整性（DIF/DIX）](#121-数据完整性difdix)
    - [12.2 块层内联加密（blk-crypto）](#122-块层内联加密blk-crypto)
    - [12.3 SED/Opal 自加密驱动器](#123-sedopal-自加密驱动器)
    - [12.4 数据完整性与加密对比总结](#124-数据完整性与加密对比总结)
13. [Zoned 块设备](#13-zoned-块设备)
    - [13.1 blk-zoned.c（2,363 行）](#131-blk-zonedc2363-行)
14. [Sysfs 与调试接口](#14-sysfs-与调试接口)
    - [14.1 blk-sysfs.c（1,030 行）](#141-blk-sysfsc1030-行)
    - [14.2 blk-mq-debugfs.c / blk-mq-debugfs.h](#142-blk-mq-debugfsc-blk-mq-debugfsh)
    - [14.3 blktrace — 块层跟踪](#143-blktrace-块层跟踪)
    - [14.4 ioctl.c（975 行）](#144-ioctlc975-行)
    - [14.5 fops.c（978 行）](#145-fopsc978-行)
    - [14.6 bsg.c / bsg-lib.c](#146-bsgc-bsg-libc)
    - [14.7 disk-events.c（489 行）](#147-disk-eventsc489-行)
    - [14.8 holder.c](#148-holderc)
    - [14.9 early-lookup.c（316 行）](#149-early-lookupc316-行)
15. [超时与电源管理](#15-超时与电源管理)
    - [15.1 blk-timeout.c](#151-blk-timeoutc)
    - [15.2 blk-pm.c / blk-pm.h](#152-blk-pmc-blk-pmh)
16. [统计与跟踪](#16-统计与跟踪)
    - [16.1 blk-stat.c / blk-stat.h](#161-blk-statc-blk-stath)
    - [16.2 块层 Tracepoints](#162-块层-tracepoints)
    - [16.3 blk-ioc.c（442 行）](#163-blk-iocc442-行)
    - [16.4 ioprio.c（249 行）](#164-ioprioc249-行)
    - [16.5 blk-ia-ranges.c（314 行）](#165-blk-ia-rangesc314-行)
    - [16.6 badblocks.c（1,550 行）](#166-badblocksc1550-行)

### Part IV: 文件系统交互

17. [Page Cache 与 buffer_head 在块设备读写中的作用](#17-page-cache-与-buffer_head-在块设备读写中的作用)
    - [17.1 概述](#171-概述)
    - [17.2 核心数据结构](#172-核心数据结构)
    - [17.3 Page Cache 读路径](#173-page-cache-读路径)
    - [17.4 Page Cache 写路径](#174-page-cache-写路径)
    - [17.5 buffer_head 到 bio 的转换](#175-buffer_head-到-bio-的转换)
    - [17.6 块设备自身的 Page Cache](#176-块设备自身的-page-cache)
    - [17.7 关键数据流对比](#177-关键数据流对比)
    - [17.8 总结](#178-总结)
18. [Writeback 回写机制 — Flush 线程下刷数据流程](#18-writeback-回写机制--flush-线程下刷数据流程)
    - [18.1 概述](#181-概述)
    - [18.2 核心数据结构](#182-核心数据结构)
    - [18.3 Flush 线程的创建与初始化](#183-flush-线程的创建与初始化)
    - [18.4 Work 入队流程](#184-work-入队流程)
    - [18.5 Flush 线程主循环](#185-flush-线程主循环)
    - [18.6 Inode 回写路径](#186-inode-回写路径)
    - [18.7 文件系统回写实现](#187-文件系统回写实现)
    - [18.8 完整函数调用栈](#188-完整函数调用栈)
    - [18.9 触发条件对比总结](#189-触发条件对比总结)
    - [18.10 关键设计要点总结](#1810-关键设计要点总结)

### Part V: 驱动实例分析

20. [NVMe 驱动：块设备注册与移除流程](#20-nvme-驱动块设备注册与移除流程)
    - [20.1 关键数据结构](#201-关键数据结构)
    - [20.2 注册流程（Probe）](#202-注册流程probe)
    - [20.3 移除流程（Remove）](#203-移除流程remove)
    - [20.4 与块层的交互接口](#204-与块层的交互接口)
    - [20.5 关键工作队列](#205-关键工作队列)
    - [20.6 涉及的文件清单](#206-涉及的文件清单)
21. [NVMe 驱动：块设备读写 I/O 流程](#21-nvme-驱动块设备读写-io-流程)
    - [21.1 I/O 流程总览](#211-io-流程总览)
    - [21.2 涉及的关键数据结构](#212-涉及的关键数据结构)
    - [21.3 详细函数调用栈](#213-详细函数调用栈)
    - [21.4 数据拷贝路径](#214-数据拷贝路径)
    - [21.5 PRP 与 SGL 数据描述符](#215-prp-与-sgl-数据描述符)
    - [21.6 完成路径详解](#216-完成路径详解)
    - [21.7 open/release 详细流程](#217-nvme-驱动openrelease-详细流程)
    - [21.8 批量提交优化](#218-批量提交优化)
    - [21.9 涉及的文件清单](#219-涉及的文件清单)

### Part VI: 附录

22. [总结](#22-总结)
    - [22.1 架构层次](#221-架构层次)
    - [22.2 代码量分布](#222-代码量分布)
    - [22.3 关键设计特点](#223-关键设计特点)

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
  │           ├─ data->ctx = blk_mq_get_ctx(q)        ← 确定软件队列
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
  │    └─ pol->cpd_alloc_fn(GFP_KERNEL)     // per-blkcg per-policy 数据
  │         └─ blkcg->cpd[i] = cpd
  │
  ├─ 初始化字段：
  │    ├─ spin_lock_init(&blkcg->lock)
  │    ├─ refcount_set(&blkcg->online_pin, 1)  // 初始 pin = 1
  │    ├─ INIT_RADIX_TREE(&blkcg->blkg_tree, GFP_NOWAIT)
  │    ├─ INIT_HLIST_HEAD(&blkcg->blkg_list)
  │    └─ list_add_tail(&blkcg->all_blkcgs_node, &all_blkcgs)
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

## Part III: 设备管理与调试

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

### 11.3 DOS/MBR 分区表详解

文件：`block/partitions/msdos.c`（717 行）

MBR（Master Boot Record）是 IBM PC 兼容机最早使用的分区表格式，位于磁盘的 0 号扇区（LBA 0）。Linux 内核通过 `msdos_partition()` 函数解析 MBR 分区表。

#### 11.3.1 MBR 扇区布局

MBR 扇区（512 字节）的完整布局：

```
偏移量    大小    内容
─────────────────────────────────────────────────
0x000     440     引导代码（boot code）
0x1B8       4     磁盘签名（disk signature, unique MBR signature）
0x1BC       2     未知（通常为 0x0000）
0x1BE      64     分区表（4 个主分区表项，每个 16 字节）
0x1FE       2     签名（0x55 0xAA）
```

- **磁盘签名**（`unique_mbr_signature`）：位于偏移 0x1B8 的 4 字节值，用于唯一标识磁盘，通过 `disksig` 传递给 `set_info()` 生成分区 UUID。
- **分区表**：紧接在偏移 0x1BE 处，共 64 字节，包含 4 个主分区表项。
- **签名**：最后两个字节固定为 `0x55AA`，用于标识这是一个有效的 MBR。

#### 11.3.2 核心数据结构

**MBR 分区表项**（[msdos_partition.h](file:///home/louis/code/linux/include/linux/msdos_partition.h)）：

```c
struct msdos_partition {
    u8     boot_ind;      // 引导标志（0x80=可引导, 0x00=不可引导）
    u8     head;          // 起始磁头（CHS 寻址）
    u8     sector;        // 起始扇区（低 6 位, 高 2 位在 cyl 中）
    u8     cyl;           // 起始柱面（高位与 sector 共用）
    u8     sys_ind;       // 分区类型标识（见枚举 msdos_sys_ind）
    u8     end_head;      // 结束磁头
    u8     end_sector;    // 结束扇区
    u8     end_cyl;       // 结束柱面
    __le32 start_sect;    // 起始 LBA（4 字节, 相对于磁盘起始）
    __le32 nr_sects;      // 分区大小（扇区数, 4 字节）
} __packed;  // 共 16 字节
```

**分区类型标识**（`sys_ind` 枚举值）：

```c
enum msdos_sys_ind {
    DOS_EXTENDED_PARTITION      = 5,     // DOS 扩展分区
    WIN98_EXTENDED_PARTITION    = 0x0f,  // Windows 98 扩展分区（LBA）
    LINUX_EXTENDED_PARTITION    = 0x85,  // Linux 扩展分区
    LINUX_DATA_PARTITION        = 0x83,  // Linux 数据分区（ext2/3/4, XFS 等）
    LINUX_SWAP_PARTITION        = 0x82,  // Linux swap 分区（也用于 Solaris）
    LINUX_LVM_PARTITION         = 0x8e,  // Linux LVM 分区
    LINUX_RAID_PARTITION        = 0xfd,  // Linux RAID 自动检测分区
    FREEBSD_PARTITION           = 0xa5,  // FreeBSD 分区
    OPENBSD_PARTITION           = 0xa6,  // OpenBSD 分区
    NETBSD_PARTITION            = 0xa9,  // NetBSD 分区
    UNIXWARE_PARTITION          = 0x63,  // Unixware / SCO Unix / GNU HURD
    DM6_PARTITION               = 0x54,  // Disk Manager 6（有 DDO）
    EZD_PARTITION               = 0x55,  // EZ-DRIVE
};
```

**扩展分区判断**：

```c
static inline int is_extended_partition(struct msdos_partition *p)
{
    return (p->sys_ind == DOS_EXTENDED_PARTITION ||
            p->sys_ind == WIN98_EXTENDED_PARTITION ||
            p->sys_ind == LINUX_EXTENDED_PARTITION);
}
```

三种扩展分区类型（5, 0x0f, 0x85）行为完全相同，区别在于：
- `DOS_EXTENDED_PARTITION (5)`：传统 CHS 扩展分区
- `WIN98_EXTENDED_PARTITION (0x0f)`：支持 LBA 寻址的扩展分区（推荐）
- `LINUX_EXTENDED_PARTITION (0x85)`：Linux 专用扩展分区

#### 11.3.3 MBR 解析流程

`msdos_partition()` 的完整调用流程：

```text
msdos_partition(state)
  │  # 入口: 读取 LBA 0 扇区
  │
  ├─ 1. 读取 MBR 扇区
  │   read_part_sector(state, 0, &sect)
  │
  ├─ 2. AIX 魔数检测
  │   aix_magic_present(state, data)
  │   └─ 检查 0x1BE 处 partition 是否有 Linux 类型
  │       └─ 若无, 检查 LBA 7 的 "_LVM" 签名 → 若匹配则 return aix_partition()
  │
  ├─ 3. 签名检查
  │   msdos_magic_present(data + 510)  →  检查 0x55AA 签名
  │   └─ 不匹配则 return 0
  │
  ├─ 4. Boot indicator 有效性检查
  │   └─ 遍历 4 个分区表项, 检查 boot_ind 是否为 0 或 0x80
  │       └─ 若非法且不是 FAT 文件系统, 则 return 0
  │
  ├─ 5. GPT 保护性 MBR 检查（CONFIG_EFI_PARTITION）
  │   └─ 遍历 4 个分区表项, 若 sys_ind == 0xEE (EFI_PMBR_OSTYPE_EFI_GPT)
  │       └─ return 0（让 GPT 解析器处理）
  │
  ├─ 6. 读取磁盘签名
  │   disksig = le32_to_cpup(data + 0x1B8)
  │
  ├─ 7. 第一遍: 主分区与扩展分区
  │   state->next = 5  # 逻辑分区从 5 开始编号
  │   └─ for slot = 1..4:
  │       ├─ 若 size == 0: continue
  │       ├─ 若 is_extended_partition(p):
  │       │   ├─ put_partition(slot, start, n)  # 扩展分区占位
  │       │   └─ parse_extended(state, start, size, disksig)  # 解析逻辑分区
  │       └─ 否则:
  │           └─ put_partition(state, slot, start, size)
  │               set_info(state, slot, disksig)  # 设置分区 UUID
  │
  └─ 8. 第二遍: 子分区解析
      └─ for slot = 1..4:
          └─ subtypes[n].parse(state, start, size, slot)
              ├─ parse_freebsd → parse_bsd()  # BSD disklabel
              ├─ parse_netbsd  → parse_bsd()
              ├─ parse_openbsd → parse_bsd()
              ├─ parse_minix                    # Minix 子分区
              ├─ parse_unixware                 # Unixware 子分区
              └─ parse_solaris_x86              # Solaris VTOC
```

**MBR 解析中的关键辅助函数**：

```c
// 读取扇区（通过页缓存）
static inline sector_t nr_sects(struct msdos_partition *p)
{
    return (sector_t)get_unaligned_le32(&p->nr_sects);
}

static inline sector_t start_sect(struct msdos_partition *p)
{
    return (sector_t)get_unaligned_le32(&p->start_sect);
}

// 设置分区元信息（UUID）
static void set_info(struct parsed_partitions *state, int slot, u32 disksig)
{
    struct partition_meta_info *info = &state->parts[slot].info;
    snprintf(info->uuid, sizeof(info->uuid), "%08x-%02x", disksig, slot);
    info->volname[0] = 0;
    state->parts[slot].has_info = true;
}
```

#### 11.3.4 扩展分区与逻辑分区

MBR 只能记录 4 个主分区，若要支持更多分区，需要将其中一个主分区标记为扩展分区。扩展分区通过链式结构组织逻辑分区：

**扩展分区布局**：

```
磁盘布局:
┌──────────┬──────────┬──────────┬──────────┬──────────┐
│ 主分区1  │ 主分区2  │ 扩展分区  │ 主分区4  │          │
│ (/dev/sda1)│(/dev/sda2)│(container)│(/dev/sda4)│          │
└──────────┴──────────┴──────────┴──────────┴──────────┘
                          │
                          ▼
          扩展分区内部（链式 EBR）:
          ┌─────────────────┬─────────────────┐
          │ EBR 1           │ 逻辑分区 1       │
          │ [sda5] [→EBR 2] │ (/dev/sda5)      │
          ├─────────────────┼─────────────────┤
          │ EBR 2           │ 逻辑分区 2       │
          │ [sda6] [→EBR 3] │ (/dev/sda6)      │
          ├─────────────────┼─────────────────┤
          │ EBR 3           │ 逻辑分区 3       │
          │ [sda7] [→NULL]  │ (/dev/sda7)      │
          └─────────────────┴─────────────────┘
```

每个 EBR（Extended Boot Record）的结构与 MBR 完全相同：
- 偏移 0x1BE：第一个分区表项 → 指向当前逻辑分区的数据区域
- 偏移 0x1CE：第二个分区表项 → 指向下一个 EBR 的位置

**`parse_extended()` 解析逻辑**：

```c
static void parse_extended(struct parsed_partitions *state,
                           sector_t first_sector, sector_t first_size,
                           u32 disksig)
{
    int loopct = 0;   // 循环防护计数器（上限 100）
    this_sector = first_sector;  // 起始于扩展分区的第一个扇区

    while (1) {
        if (++loopct > 100) return;  // 防无限循环
        if (state->next == state->limit) return;  // 分区数上限

        data = read_part_sector(state, this_sector, &sect);
        if (!msdos_magic_present(data + 510)) goto done;

        p = (struct msdos_partition *)(data + 0x1be);

        // 第一遍: 处理数据分区（逻辑分区）
        for (i = 0; i < 4; i++, p++) {
            if (!nr_sects(p) || is_extended_partition(p)) continue;
            offs = start_sect(p) * sector_size;
            size = nr_sects(p) * sector_size;
            next = this_sector + offs;   // 逻辑分区的绝对起始地址
            put_partition(state, state->next, next, size);
            // ...
        }

        // 第二遍: 查找下一个扩展分区链接
        p -= 4;
        for (i = 0; i < 4; i++, p++)
            if (nr_sects(p) && is_extended_partition(p)) break;
        if (i == 4) goto done;  // 没有更多扩展分区链接

        // 移动到下一个 EBR
        this_sector = first_sector + start_sect(p) * sector_size;
    }
}
```

**关键设计要点**：
- 每个 EBR 读取后检查 `0x55AA` 签名
- `loopct` 上限 100 防止损坏的扩展分区表导致无限循环
- 逻辑分区编号从 5 开始（`state->next = 5`）
- 第 3、4 个 EBR 分区表项检查边界（不能超出扩展分区范围）

#### 11.3.5 MBR 的局限性

| 限制 | 说明 |
|------|------|
| 最大磁盘容量 | 2 TiB（因为 `start_sect` 和 `nr_sects` 均为 32 位） |
| 最大分区数 | 4 个主分区，或 3 个主分区 + 最多约 60 个逻辑分区 |
| 数据冗余 | 无备份分区表，MBR 损坏则整个磁盘不可访问 |
| UUID 支持 | 仅 4 字节磁盘签名，不支持真正的 GUID |
| 分区属性 | 仅有 1 字节类型标识，无属性标志位 |

### 11.4 EFI/GPT 分区表详解

文件：`block/partitions/efi.c`（756 行），头文件：`block/partitions/efi.h`

GPT（GUID Partition Table）是 UEFI 规范定义的新一代分区表格式，克服了 MBR 的 2TiB 限制和 4 个主分区限制。

#### 11.4.1 GPT 磁盘布局

```
LBA 0      ┌─────────────────────────────────────────┐
           │  保护性 MBR（PMBR）                      │
           │  - 分区类型 0xEE, 覆盖整个磁盘或 2TiB    │
           └─────────────────────────────────────────┘
LBA 1      ┌─────────────────────────────────────────┐
           │  GPT 头（Primary GPT Header）            │
           │  - 签名: "EFI PART"                      │
           │  - 分区表位置: LBA 2                     │
           │  - 备份 GPT 头位置: 磁盘最后一个 LBA     │
           └─────────────────────────────────────────┘
LBA 2      ┌─────────────────────────────────────────┐
           │  GPT 分区表项数组（Primary Partition Entries）│
           │  - 默认 128 个条目 × 128 字节 = 16KB     │
           │  - 通常占用 LBA 2~33（共 32 个扇区）      │
           └─────────────────────────────────────────┘
           │                                         │
           │    可用分区区域                           │
           │  (first_usable_lba ~ last_usable_lba)    │
           │                                         │
LBA N-33   ┌─────────────────────────────────────────┐
           │  GPT 分区表项数组（备份）                  │
           │  (Alternate Partition Entries)           │
           └─────────────────────────────────────────┘
LBA N-1    ┌─────────────────────────────────────────┐
           │  备份 GPT 头（Alternate GPT Header）      │
           │  - 随磁盘大小变化位置                      │
           └─────────────────────────────────────────┘
LBA N      （磁盘末尾）
```

#### 11.4.2 核心数据结构

**GPT 头**（[efi.h](file:///home/louis/code/linux/block/partitions/efi.h)）：

```c
typedef struct _gpt_header {
    __le64 signature;                // 签名: "EFI PART" (0x5452415020494645ULL)
    __le32 revision;                 // 修订版本 (0x00010000 = v1.0)
    __le32 header_size;              // 头大小 (通常 92 字节)
    __le32 header_crc32;             // 头 CRC32 校验（计算时此字段置 0）
    __le32 reserved1;                // 保留
    __le64 my_lba;                   // 本 GPT 头所在的 LBA
    __le64 alternate_lba;            // 备份 GPT 头所在的 LBA
    __le64 first_usable_lba;         // 第一个可用数据区的 LBA
    __le64 last_usable_lba;          // 最后一个可用数据区的 LBA
    efi_guid_t disk_guid;            // 磁盘唯一 GUID（16 字节）
    __le64 partition_entry_lba;      // 分区表项数组的起始 LBA
    __le32 num_partition_entries;    // 分区表项数量（默认 128）
    __le32 sizeof_partition_entry;   // 每个分区表项大小（默认 128 字节）
    __le32 partition_entry_array_crc32;  // 分区表项数组 CRC32
    // 剩余空间为零填充（BlockSize - 92 字节）
} __packed gpt_header;  // 共 92 字节
```

**GPT 分区表项**：

```c
typedef struct _gpt_entry {
    efi_guid_t partition_type_guid;    // 分区类型 GUID（16 字节）
    efi_guid_t unique_partition_guid;  // 分区唯一 GUID（16 字节）
    __le64 starting_lba;               // 起始 LBA（8 字节）
    __le64 ending_lba;                 // 结束 LBA（8 字节）
    gpt_entry_attributes attributes;   // 属性标志（8 字节）
    __le16 partition_name[36];         // 分区名称（36 个 UTF-16LE 字符, 72 字节）
} __packed gpt_entry;  // 共 128 字节
```

**GPT 分区属性标志**：

```c
typedef struct _gpt_entry_attributes {
    u64 required_to_function:1;  // 位 0: 分区必须存在才能正常工作
    u64 reserved:47;             // 位 1-47: 保留
    u64 type_guid_specific:16;   // 位 48-63: 类型 GUID 专用
} __packed gpt_entry_attributes;
```

**保护性 MBR 结构**（`legacy_mbr`）：

```c
typedef struct _legacy_mbr {
    u8 boot_code[440];            // 引导代码
    __le32 unique_mbr_signature;  // MBR 签名
    __le16 unknown;               // 未知
    gpt_mbr_record partition_record[4];  // 4 个 MBR 分区记录
    __le16 signature;             // 0xAA55 签名
} __packed legacy_mbr;  // 共 512 字节

typedef struct _gpt_mbr_record {
    u8 boot_indicator;    // 引导标志
    u8 start_head;        // 起始磁头（CHS, GPT 不使用）
    u8 start_sector;      // 起始扇区
    u8 start_track;       // 起始磁道
    u8 os_type;           // OS 类型（GPT: 0xEE, 混合 MBR: 其他类型）
    u8 end_head;          // 结束磁头
    u8 end_sector;        // 结束扇区
    u8 end_track;         // 结束磁道
    __le32 starting_lba;  // 起始 LBA（GPT 使用）
    __le32 size_in_lba;   // 大小（扇区数, GPT 使用）
} __packed gpt_mbr_record;
```

**常用分区类型 GUID**：

```c
#define PARTITION_SYSTEM_GUID           // {C12A7328-F81F-11D2-BA4B-00A0C93EC93B}  EFI 系统分区
#define LEGACY_MBR_PARTITION_GUID       // {024DEE41-33E7-11D3-9D69-0008C781F39F}  MBR 兼容分区
#define PARTITION_MSFT_RESERVED_GUID    // {E3C9E316-0B5C-4DB8-817D-F92DF00215AE}  Microsoft 保留分区
#define PARTITION_BASIC_DATA_GUID       // {EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}  基本数据分区
#define PARTITION_LINUX_RAID_GUID       // {A19D880F-05FC-4D3B-A006-743F0F84911E}  Linux RAID
#define PARTITION_LINUX_SWAP_GUID       // {0657FD6D-A4AB-43C4-84E5-0933C84B4F4F}  Linux Swap
#define PARTITION_LINUX_LVM_GUID        // {E6D6D379-F507-44C2-A23C-238F2A3DF928}  Linux LVM
```

#### 11.4.3 GPT 解析流程

`efi_partition()` 的完整调用流程：

```text
efi_partition(state)
  │  # 入口: 调用 find_valid_gpt() 验证并获取有效的 GPT
  │
  ├─ 1. find_valid_gpt(state, &gpt, &ptes)
  │   │
  │   ├─ 1.1 读取 LBA 0（保护性 MBR）
  │   │   read_lba(state, 0, (u8 *)legacymbr, sizeof(*legacymbr))
  │   │
  │   ├─ 1.2 is_pmbr_valid(legacymbr, total_sectors)
  │   │   │  # 检查 PMBR 有效性
  │   │   │
  │   │   └─ 检查条件:
  │   │       ├─ 签名: le16_to_cpu(mbr->signature) == MSDOS_MBR_SIGNATURE (0xAA55)
  │   │       ├─ 分区类型: os_type == 0xEE (EFI_PMBR_OSTYPE_EFI_GPT)
  │   │       └─ starting_lba == 1 (GPT_PRIMARY_PARTITION_TABLE_LBA)
  │   │
  │   │   └─ 返回 GPT_MBR_PROTECTIVE 或 GPT_MBR_HYBRID
  │   │
  │   ├─ 1.3 is_gpt_valid(state, GPT_PRIMARY_PARTITION_TABLE_LBA, &pgpt, &pptes)
  │   │   │  # 验证主 GPT 头
  │   │   │
  │   │   └─ alloc_read_gpt_header(state, lba)
  │   │       │  # 分配并读取 GPT 头
  │   │       │
  │   │       └─ 验证步骤:
  │   │           ├─ signature == "EFI PART" (0x5452415020494645ULL)
  │   │           ├─ header_size <= 逻辑块大小
  │   │           ├─ header_size >= sizeof(gpt_header)
  │   │           ├─ header_crc32 校验通过（efi_crc32 重新计算）
  │   │           ├─ my_lba == lba 参数
  │   │           ├─ first_usable_lba <= lastlba
  │   │           ├─ last_usable_lba <= lastlba
  │   │           ├─ sizeof_partition_entry == sizeof(gpt_entry)
  │   │           ├─ 分区表大小不超出 KMALLOC_MAX_SIZE
  │   │           └─ partition_entry_array_crc32 校验通过
  │   │
  │   ├─ 1.4 is_gpt_valid(state, pgpt->alternate_lba, &agpt, &aptes)
  │   │   │  # 验证备份 GPT 头
  │   │   │  # 若主 GPT 有效, 才验证备份 GPT
  │   │   │  # 若 force_gpt 则强制从磁盘末尾读取备份 GPT
  │   │   │
  │   ├─ 1.5 compare_gpts(pgpt, agpt, lastlba)
  │   │   │  # 比对主/备 GPT 一致性
  │   │   │
  │   │   └─ 比对项目:
  │   │       ├─ pgpt->my_lba == agpt->alternate_lba
  │   │       ├─ pgpt->alternate_lba == agpt->my_lba
  │   │       ├─ first_usable_lba 一致
  │   │       ├─ last_usable_lba 一致
  │   │       ├─ disk_guid 一致
  │   │       ├─ num_partition_entries 一致
  │   │       ├─ sizeof_partition_entry 一致
  │   │       ├─ partition_entry_array_crc32 一致
  │   │       └─ alternate_lba == lastlba（备份 GPT 应在磁盘末尾）
  │   │
  │   └─ 1.6 选择有效的 GPT
  │       ├─ 若主 GPT 有效: 使用主 GPT（打印警告若备份 GPT 无效）
  │       └─ 若主无效但备份有效: 使用备份 GPT（打印警告）
  │
  └─ 2. 枚举分区表项
      └─ for i = 0..num_partition_entries, limit = state->limit-1:
          ├─ is_pte_valid(&ptes[i], lastlba):
          │   ├─ partition_type_guid != NULL_GUID
          │   ├─ starting_lba <= lastlba
          │   └─ ending_lba <= lastlba
          │
          ├─ put_partition(state, i+1, start * ssz, size * ssz)
          │
          ├─ 若 partition_type_guid == PARTITION_LINUX_RAID_GUID
          │   → 设置 ADDPART_FLAG_RAID 标志
          │
          ├─ efi_guid_to_str(&ptes[i].unique_partition_guid, info->uuid)
          │   → 将分区 GUID 转为 UUID 字符串
          │
          └─ utf16_le_to_7bit(ptes[i].partition_name, label_max, info->volname)
              → 将 UTF-16LE 分区名称转为 ASCII 字符串
```

**关键验证函数 `is_gpt_valid()` 的 CRC 校验逻辑**：

```c
static int is_gpt_valid(struct parsed_partitions *state, u64 lba,
                        gpt_header **gpt, gpt_entry **ptes)
{
    // 1. 读取 GPT 头
    *gpt = alloc_read_gpt_header(state, lba);

    // 2. 验证签名
    if (le64_to_cpu((*gpt)->signature) != GPT_HEADER_SIGNATURE)
        goto fail;

    // 3. 验证头大小范围
    if (le32_to_cpu((*gpt)->header_size) > queue_logical_block_size(...))
        goto fail;
    if (le32_to_cpu((*gpt)->header_size) < sizeof(gpt_header))
        goto fail;

    // 4. CRC 校验: 头 CRC 的计算将 header_crc32 字段置 0 后重新计算
    origcrc = le32_to_cpu((*gpt)->header_crc32);
    (*gpt)->header_crc32 = 0;
    crc = efi_crc32((const unsigned char *)(*gpt),
                    le32_to_cpu((*gpt)->header_size));
    if (crc != origcrc)
        goto fail;

    // 5. 验证 my_lba 一致性
    if (le64_to_cpu((*gpt)->my_lba) != lba)
        goto fail;

    // 6. 验证 first/last_usable_lba 范围
    // 7. 验证 sizeof_partition_entry
    // 8. 读取分区表项并验证 array CRC
    *ptes = alloc_read_gpt_entries(state, *gpt);
    crc = efi_crc32((const unsigned char *)(*ptes), pt_size);
    if (crc != le32_to_cpu((*gpt)->partition_entry_array_crc32))
        goto fail_ptes;

    return 1;  // 验证通过
}
```

#### 11.4.4 保护性 MBR（PMBR）

GPT 规范要求在 LBA 0 处放置一个保护性 MBR（Protective MBR），其目的是：

1. **兼容性**：让不支持 GPT 的旧操作系统/工具识别磁盘为"已分区"，避免误格式化
2. **保护**：防止旧工具误认为磁盘未分区而覆盖 GPT 数据

**PMBR 特征**：
- `signature` = 0xAA55（标准 MBR 签名）
- `partition_record[0].os_type` = 0xEE（EFI_PMBR_OSTYPE_EFI_GPT）
- `partition_record[0].starting_lba` = 1（指向 GPT 头）
- `partition_record[0].size_in_lba` = 整个磁盘大小或 0xFFFFFFFF（2TiB）

**混合 MBR（Hybrid MBR）**：
- 同时包含 GPT 分区和最多 3 个传统 MBR 分区表项（第 4 个保留为 0xEE）
- 用于支持双系统引导（如 macOS Boot Camp）
- 通过 `is_pmbr_valid()` 检测：若存在非 0xEE 且非 0x00 的分区类型，则判定为混合 MBR

```c
static int is_pmbr_valid(legacy_mbr *mbr, sector_t total_sectors)
{
    // 检查签名
    if (le16_to_cpu(mbr->signature) != MSDOS_MBR_SIGNATURE)
        return 0;

    // 查找 0xEE 类型分区
    for (i = 0; i < 4; i++) {
        ret = pmbr_part_valid(&mbr->partition_record[i]);
        if (ret == GPT_MBR_PROTECTIVE) goto check_hybrid;
    }
    return 0;

check_hybrid:
    // 检查是否存在非 GPT 分区 → 混合 MBR
    for (i = 0; i < 4; i++)
        if (mbr->partition_record[i].os_type != 0xEE &&
            mbr->partition_record[i].os_type != 0x00)
            ret = GPT_MBR_HYBRID;
    return ret;
}
```

#### 11.4.5 主/备 GPT 冗余机制

GPT 在磁盘末尾保存一份备份（Alternate）GPT，提供数据冗余：

**冗余策略**：

```
正常情况:
  主 GPT (LBA 1)  ←──────────────→  备份 GPT (LBA N-1)
  主分区表 (LBA 2~33)  ←────────→  备份分区表 (LBA N-33~N-2)
  两者通过 alternate_lba 互相引用

主 GPT 损坏:
  find_valid_gpt() 发现主 GPT CRC 校验失败
  → 尝试加载备份 GPT
  → 如果备份有效, 使用备份 GPT（打印警告）
  → 用户可通过 kernel 参数 'gpt' 强制使用备份

备份 GPT 损坏:
  find_valid_gpt() 发现备份 GPT CRC 校验失败
  → 使用主 GPT（打印警告）
  → 用户空间工具（如 gdisk）可修复备份 GPT

主/备不一致:
  compare_gpts() 检测以下差异:
  - my_lba ↔ alternate_lba 交叉引用不匹配
  - first/last_usable_lba 不一致
  - disk_guid 不一致
  - 分区表数量/大小/CRC 不一致
  → 打印警告, 建议使用 GNU Parted 修复
```

**备份 GPT 定位策略**：

```c
// 正常情况: 从主 GPT 的 alternate_lba 字段获取
good_agpt = is_gpt_valid(state, le64_to_cpu(pgpt->alternate_lba), &agpt, &aptes);

// 强制模式: 从磁盘末尾读取
if (!good_agpt && force_gpt)
    good_agpt = is_gpt_valid(state, lastlba, &agpt, &aptes);

// 驱动特殊处理: 某些设备（如 Apple 磁盘）的备份 GPT 在特殊位置
if (!good_agpt && force_gpt && fops->alternative_gpt_sector)
    fops->alternative_gpt_sector(disk, &agpt_sector);
```

#### 11.4.6 GPT 功能特点

| 特性 | 说明 |
|------|------|
| 最大磁盘容量 | 无限制（64 位 LBA 寻址） |
| 最大分区数 | 默认 128 个（可扩展, 由 `num_partition_entries` 决定） |
| 分区表冗余 | 主/备双份 GPT, CRC 校验保护 |
| 分区标识 | 128 位 GUID, 全局唯一 |
| 分区名称 | 36 字符 UTF-16LE 名称 |
| 分区属性 | 8 字节属性标志位 |
| 兼容性 | 保护性 MBR 确保传统工具兼容 |
| 校验保护 | GPT 头 CRC32 + 分区表阵列 CRC32 |

### 11.5 MBR vs GPT 对比总结

| 对比维度 | DOS/MBR | EFI/GPT |
|----------|---------|---------|
| **规范起源** | IBM PC/AT, 1983 | UEFI 规范, 1999 |
| **实现文件** | `block/partitions/msdos.c` | `block/partitions/efi.c` |
| **代码行数** | 717 行 | 756 行 |
| **核心数据结构** | `struct msdos_partition` (16B) | `struct gpt_header` (92B) + `struct gpt_entry` (128B) |
| **分区表位置** | LBA 0, 偏移 0x1BE | LBA 1 (主), LBA N-1 (备份) |
| **分区表项大小** | 16 字节 | 128 字节 |
| **最大主分区数** | 4 个 | 默认 128 个 |
| **逻辑分区** | 通过扩展分区链式支持 | 不需要（直接支持 128+ 分区） |
| **最大磁盘容量** | 2 TiB (32 位 LBA) | 无限制 (64 位 LBA) |
| **分区标识** | 1 字节 `sys_ind` | 128 位 GUID |
| **唯一磁盘 ID** | 4 字节签名 | 16 字节 GUID |
| **分区名称** | 不支持 | 36 字符 UTF-16LE |
| **数据冗余** | 无 | 主/备 GPT 双份 + CRC32 |
| **校验保护** | 无 | 头 CRC32 + 分区表阵列 CRC32 |
| **兼容性** | 所有 x86 系统 | 需要保护性 MBR |
| **内核检测顺序** | 在 GPT 之后（`check_part[]` 中 GPT 先于 MBR） | 在 MBR 之前 |
| **检测入口** | `msdos_partition()` | `efi_partition()` |
| **检测逻辑** | 检查 0x55AA 签名 + boot_ind 有效性 | 验证 PMBR → 验证 GPT 头 CRC → 验证分区表 CRC |
| **扩展分区** | `parse_extended()` 链式遍历 | 无此概念 |
| **子分区** | BSD disklabel, Solaris VTOC, Minix 等 | 无 |

**内核检测优先级**：

```text
check_part[] 数组定义在 block/partitions/core.c:
──────────────────────────────────────────────────
1. ADFS (Acorn) 分区         # 优先检测（有 ADFS 引导块）
2. CMDLINE 分区               # 内核命令行指定
3. OF (设备树) 分区            # 设备树指定
4. EFI/GPT 分区               # ★ GPT 优先于 MBR ★
5. SGI 分区
6. LDM (Windows 动态磁盘)
7. MSDOS/MBR 分区             # ★ MBR 在 GPT 之后 ★
8. OSF/Unix, Sun, Amiga, Atari, Mac, ...
──────────────────────────────────────────────────
```

**为什么 GPT 检测在 MBR 之前**：
- GPT 磁盘的 LBA 0 包含保护性 MBR（0xEE 类型）
- 若先运行 MBR 检测，`msdos_partition()` 会看到 PMBR 并尝试解析为无效的 MBR
- 内核解决方案：在 `check_part[]` 数组中，`efi_partition` 排在 `msdos_partition` 之前
- `msdos_partition()` 内部也有保护逻辑：若发现 `sys_ind == 0xEE`，立即 `return 0` 让 GPT 处理

**检测流程决策树**：

```text
read_part_sector(LBA 0)
  │
  ├─ 0x55AA 签名存在?
  │   ├─ 否 → 非 MBR 磁盘, 尝试其他分区格式
  │   └─ 是 → 继续
  │
  ├─ sys_ind == 0xEE (GPT PMBR)?
  │   ├─ 是 → 跳过 MBR 解析, 交由 efi_partition() 处理
  │   │        └─ efi_partition():
  │   │            ├─ PMBR 验证通过?
  │   │            ├─ 主 GPT 头 CRC 验证?
  │   │            ├─ 备份 GPT 头 CRC 验证?
  │   │            └─ 分区表项 CRC 验证?
  │   │
  │   └─ 否 → 继续 MBR 解析
  │
  ├─ 扩展分区?
  │   └─ 是 → parse_extended() 链式解析逻辑分区
  │
  └─ 子分区类型?
      └─ BSD/Solaris/Unixware/Minix → 调用对应的子分区解析器
```

---

## 12. 数据完整性与加密

### 12.1 数据完整性（DIF/DIX）

T10 保护信息（T10 Protection Information, PI）是 SCSI 和 NVMe 设备支持的数据完整性方案，也称为 DIF（Data Integrity Field）或 DIX（Data Integrity Extension）。其核心思想是在每个数据扇区后附加一个 PI 元组，用于校验数据的完整性和正确性。

#### 12.1.1 体系架构

```text
                        文件系统 / 应用层
                              │
                              ▼
                    ┌──────────────────────┐
                    │  bio_integrity_prep() │  ← bio-integrity-auto.c
                    │  (自动生成/验证 PI)    │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  bio 层完整性管理      │
                    │  bio_integrity_alloc() │  ← bio-integrity.c
                    │  bio_integrity_free()  │
                    │  bio_integrity_add_page│
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  blk 层完整性管理      │
                    │  blk_integrity_generate│  ← t10-pi.c
                    │  blk_integrity_verify  │
                    │  blk_integrity_prepare │
                    │  blk_integrity_complete│
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  设备驱动层            │
                    │  (NVMe/SCSI 驱动)      │
                    │  硬件处理 PI 或传递    │
                    └──────────────────────┘
```

**三种完整性保护模式**：

| 模式 | 说明 | 数据流 |
|------|------|--------|
| **DIF Type 1** | 数据块 + 保护信息一起传输，ref_tag 为起始 LBA | 主机生成/验证 PI |
| **DIF Type 2** | 同 Type 1，但 ref_tag 间接引用 LBA1 | 主机生成/验证 PI |
| **DIF Type 3** | 数据块 + 保护信息一起传输，ref_tag 未定义 | 主机生成/验证 PI |
| **DIX** | 保护信息与数据分离传输，通过独立 DMA 通道 | 主机生成，设备验证 |

#### 12.1.2 核心数据结构

**设备级完整性描述**（[blkdev.h](file:///home/louis/code/linux/include/linux/blkdev.h)）：

```c
struct blk_integrity {
    unsigned char                   flags;         // 标志位:
                                                    //   BLK_INTEGRITY_NOVERIFY     = 1 << 0  (禁用读校验)
                                                    //   BLK_INTEGRITY_NOGENERATE   = 1 << 1  (禁用写生成)
                                                    //   BLK_INTEGRITY_DEVICE_CAPABLE = 1 << 2 (设备支持)
                                                    //   BLK_INTEGRITY_REF_TAG      = 1 << 3  (启用 ref_tag)
                                                    //   BLK_INTEGRITY_STACKED      = 1 << 4  (已堆叠)
    enum blk_integrity_checksum     csum_type;     // 校验和类型:
                                                    //   BLK_INTEGRITY_CSUM_NONE  = 0
                                                    //   BLK_INTEGRITY_CSUM_IP    = 1 (IP 校验和)
                                                    //   BLK_INTEGRITY_CSUM_CRC   = 2 (CRC16-T10DIF)
                                                    //   BLK_INTEGRITY_CSUM_CRC64 = 3 (CRC64-NVMe)
    unsigned char                   metadata_size; // 每个扇区的元数据总大小 (字节)
    unsigned char                   pi_offset;     // PI 元组在元数据中的偏移
    unsigned char                   interval_exp;  // 数据间隔指数 (2^interval_exp 字节/扇区)
    unsigned char                   tag_size;      // 应用标签大小
    unsigned char                   pi_tuple_size; // PI 元组大小
} __packed;  // 共 7 字节, 嵌入在 struct queue_limits 中
```

**Bio 级完整性负载**（[bio-integrity.h](file:///home/louis/code/linux/include/linux/bio-integrity.h)）：

```c
struct bio_integrity_payload {
    struct bvec_iter    bip_iter;       // 完整性数据迭代器
    unsigned short      bip_vcnt;       // 完整性 bio_vec 数量
    unsigned short      bip_max_vcnt;   // 分配的 bio_vec 槽位数
    unsigned short      bip_flags;      // 控制标志:
                                        //   BIP_BLOCK_INTEGRITY  = 1 << 0  (块层拥有)
                                        //   BIP_MAPPED_INTEGRITY = 1 << 1  (ref_tag 已重映射)
                                        //   BIP_COPY_USER        = 1 << 4  (内核 bounce buffer)
                                        //   BIP_CHECK_GUARD      = 1 << 5  (校验 guard)
                                        //   BIP_CHECK_REFTAG     = 1 << 6  (校验 ref_tag)
                                        //   BIP_CHECK_APPTAG     = 1 << 7  (校验 app_tag)
    u16                 app_tag;        // 应用标签值
    struct bio_vec      *bip_vec;       // 完整性数据页数组
};
```

**T10 PI 元组**（[t10-pi.h](file:///home/louis/code/linux/include/linux/t10-pi.h)）：

```c
struct t10_pi_tuple {
    __be16 guard_tag;   // 校验和 (2 字节): CRC16-T10DIF 或 IP 校验和
    __be16 app_tag;     // 应用标签 (2 字节): 上层应用可用
    __be32 ref_tag;     // 引用标签 (4 字节): 通常为起始 LBA 的低 32 位
} __packed;  // 共 8 字节

// 特殊转义值:
#define T10_PI_APP_ESCAPE  cpu_to_be16(0xffff)     // 应用标签逃逸
#define T10_PI_REF_ESCAPE  cpu_to_be32(0xffffffff)  // 引用标签逃逸
```

**T10 PI 类型定义**：

```c
enum t10_dif_type {
    T10_PI_TYPE0_PROTECTION = 0x0,  // 无保护
    T10_PI_TYPE1_PROTECTION = 0x1,  // ref_tag = LBA (标准)
    T10_PI_TYPE2_PROTECTION = 0x2,  // ref_tag 间接引用
    T10_PI_TYPE3_PROTECTION = 0x3,  // 无 ref_tag
};
```

**CRC64 PI 元组**（NVMe 扩展）：

```c
struct crc64_pi_tuple {
    __be64 guard_tag;   // CRC64 校验和 (8 字节)
    __be16 app_tag;     // 应用标签 (2 字节)
    __be16 ref_tag;     // 引用标签 (2 字节)
} __packed;  // 共 12 字节
```

#### 12.1.3 完整性校验和算法

```c
enum blk_integrity_checksum {
    BLK_INTEGRITY_CSUM_NONE  = 0,  // 无校验和
    BLK_INTEGRITY_CSUM_IP    = 1,  // IP 校验和 (16 位, 快速)
    BLK_INTEGRITY_CSUM_CRC   = 2,  // CRC16-T10DIF (16 位, 标准)
    BLK_INTEGRITY_CSUM_CRC64 = 3,  // CRC64-NVMe (64 位, NVMe 扩展)
};
```

- **IP 校验和**：`ip_compute_csum()`，基于 16 位补码加法，速度快但碰撞概率高于 CRC
- **CRC16-T10DIF**：`crc_t10dif()`，使用 SCSI 规范定义的 CRC16 多项式 `0x18BB7`
- **CRC64-NVMe**：NVMe 扩展的 64 位 CRC，提供更强的校验能力

#### 12.1.4 完整性 I/O 流程

**写路径**（生成 PI 元组）：

```text
bio_integrity_prep(bio)                     # bio-integrity-auto.c
  │
  ├─ 检查设备是否支持完整性 (blk_get_integrity)
  ├─ 检查 bio 是否已有完整性负载
  │
  ├─ 分配 bio_integrity_data (mempool)
  ├─ 调用 bio_integrity_init() 设置 bip
  ├─ 调用 bio_integrity_alloc_buf() 分配元数据缓冲区
  │   └─ kmalloc 或 mempool 备用
  ├─ 设置 seed (bi_sector)
  ├─ 设置 BIP_CHECK 标志 (根据 csum_type)
  │
  └─ 若为 WRITE 且需要校验:
      └─ blk_integrity_generate(bio)        # t10-pi.c
          │
          └─ 遍历 bio 的每个 data segment:
              ├─ t10_pi_generate(iter, bi)  # CRC16 / IP 校验和
              │   └─ 对每个 interval:
              │       ├─ 计算 guard_tag = t10_pi_csum(data, interval)
              │       ├─ app_tag = 0
              │       └─ ref_tag = lower_32_bits(seed)
              │
              └─ ext_pi_crc64_generate(iter, bi)  # CRC64 扩展
                  └─ 对每个 interval:
                      ├─ 计算 guard_tag = crc64(data, interval)
                      ├─ app_tag = 0
                      └─ ref_tag = lower_32_bits(seed)

blk_integrity_prepare(rq)                   # t10-pi.c
  └─ 若 BLK_INTEGRITY_REF_TAG:
      ├─ t10_pi_type1_prepare(rq)  # 为每个 bio 重映射 ref_tag
      └─ ext_pi_type1_prepare(rq)  # CRC64 版本
          └─ 遍历 request 中的每个 bio:
              └─ 用 rq->__sector 替换 bip 中的 seed
```

**读路径**（验证 PI 元组）：

```text
bio_integrity_prep(bio)                     # bio-integrity-auto.c
  │
  └─ 若为 READ:
      └─ 保存 bi_iter 到 saved_bio_iter (用于后续验证)

bio_integrity_endio(bio)                    # bio-integrity.c / blk.h
  │
  ├─ 检查 bip->bip_flags & BIP_BLOCK_INTEGRITY
  │
  └─ __bio_integrity_endio(bio)             # bio-integrity-auto.c
      │
      ├─ 若 READ 成功且需要校验:
      │   └─ queue_work(kintegrityd_wq, &bid->work)
      │       └─ bio_integrity_verify_fn()  # 工作队列处理
      │           └─ blk_integrity_verify_iter(bio, saved_iter)
      │               │
      │               └─ 遍历 bio 的每个 data segment:
      │                   ├─ t10_pi_verify(iter, bi)  # CRC16 / IP
      │                   │   └─ 对每个 interval:
      │                   │       ├─ 检查 app_tag 逃逸
      │                   │       ├─ 检查 ref_tag 匹配
      │                   │       └─ 检查 guard_tag 匹配
      │                   │
      │                   └─ ext_pi_crc64_verify(iter, bi)  # CRC64
      │                       └─ 类似地检查 guard/app/ref tag
      │
      └─ bio_integrity_finish(bid)
          └─ bio_endio(bio)  # 继续 I/O 完成

blk_integrity_complete(rq, nr_bytes)        # t10-pi.c
  └─ 若 BLK_INTEGRITY_REF_TAG:
      ├─ t10_pi_type1_complete(rq, nr_bytes)  # 恢复 ref_tag
      └─ ext_pi_type1_complete(rq, nr_bytes)  # CRC64 版本
```

#### 12.1.5 完整性校验步骤详解

**写入时生成**（`t10_pi_generate()`）：

```c
static void t10_pi_generate(struct blk_integrity_iter *iter,
                            struct blk_integrity *bi)
{
    for (i = 0; i < iter->data_size; i += iter->interval) {
        struct t10_pi_tuple *pi = iter->prot_buf + bi->pi_offset;

        // 1. 计算 guard_tag (数据块校验和)
        pi->guard_tag = t10_pi_csum(0, iter->data_buf, iter->interval,
                                    bi->csum_type);
        // 若 pi_offset > 0, 还需包含元数据前缀
        if (bi->pi_offset)
            pi->guard_tag = t10_pi_csum(pi->guard_tag, iter->prot_buf,
                                        bi->pi_offset, bi->csum_type);

        // 2. app_tag 清零
        pi->app_tag = 0;

        // 3. 设置 ref_tag (起始 LBA 低 32 位)
        if (bi->flags & BLK_INTEGRITY_REF_TAG)
            pi->ref_tag = cpu_to_be32(lower_32_bits(iter->seed));
        else
            pi->ref_tag = 0;

        iter->data_buf += iter->interval;
        iter->prot_buf += bi->metadata_size;
        iter->seed++;
    }
}
```

**读取时验证**（`t10_pi_verify()`）：

```c
static blk_status_t t10_pi_verify(struct blk_integrity_iter *iter,
                                  struct blk_integrity *bi)
{
    for (i = 0; i < iter->data_size; i += iter->interval) {
        struct t10_pi_tuple *pi = iter->prot_buf + bi->pi_offset;

        // Type 1: 检查 ref_tag
        if (bi->flags & BLK_INTEGRITY_REF_TAG) {
            if (pi->app_tag == T10_PI_APP_ESCAPE)
                goto next;  // 逃逸, 跳过校验
            if (be32_to_cpu(pi->ref_tag) != lower_32_bits(iter->seed))
                return BLK_STS_PROTECTION;  // ref_tag 错误!
        }

        // 重新计算 guard_tag 并与存储值比较
        csum = t10_pi_csum(0, iter->data_buf, iter->interval, bi->csum_type);
        if (bi->pi_offset)
            csum = t10_pi_csum(csum, iter->prot_buf, bi->pi_offset, bi->csum_type);

        if (pi->guard_tag != csum)
            return BLK_STS_PROTECTION;  // guard_tag 错误!
    }
    return BLK_STS_OK;
}
```

#### 12.1.6 完整性配置文件管理

**完整性配置验证**（[blk-settings.c](file:///home/louis/code/linux/block/blk-settings.c)）：

```c
static int blk_validate_integrity_limits(struct queue_limits *lim)
{
    struct blk_integrity *bi = &lim->integrity;

    // 1. 无元数据 → 禁用完整性, 设置 NOGENERATE + NOVERIFY
    if (!bi->metadata_size) {
        bi->flags |= BLK_INTEGRITY_NOGENERATE | BLK_INTEGRITY_NOVERIFY;
        return 0;
    }

    // 2. 检查 csum_type 和 REF_TAG 一致性
    if (bi->csum_type == BLK_INTEGRITY_CSUM_NONE &&
        (bi->flags & BLK_INTEGRITY_REF_TAG))
        return -EINVAL;

    // 3. 检查 pi_offset + pi_tuple_size 不超过 metadata_size
    if (bi->pi_offset + bi->pi_tuple_size > bi->metadata_size)
        return -EINVAL;

    // 4. 校验每种 csum_type 的合法性
    switch (bi->csum_type) {
    case BLK_INTEGRITY_CSUM_NONE:
        if (bi->pi_tuple_size) return -EINVAL;
        break;
    case BLK_INTEGRITY_CSUM_CRC:
    case BLK_INTEGRITY_CSUM_IP:
        if (bi->pi_tuple_size != sizeof(struct t10_pi_tuple))
            return -EINVAL;
        break;
    case BLK_INTEGRITY_CSUM_CRC64:
        if (bi->pi_tuple_size != sizeof(struct crc64_pi_tuple))
            return -EINVAL;
        break;
    }
    return 0;
}
```

**完整性配置堆叠**（`queue_limits_stack_integrity()`）：

```c
bool queue_limits_stack_integrity(struct queue_limits *t,
                                  struct queue_limits *b)
{
    struct blk_integrity *ti = &t->integrity;
    struct blk_integrity *bi = &b->integrity;

    if (ti->flags & BLK_INTEGRITY_STACKED) {
        // 已堆叠: 检查与下层一致性
        if (ti->metadata_size != bi->metadata_size) goto incompatible;
        if (ti->interval_exp != bi->interval_exp) goto incompatible;
        if (ti->csum_type != bi->csum_type) goto incompatible;
        if (ti->pi_tuple_size != bi->pi_tuple_size) goto incompatible;
    } else {
        // 首次堆叠: 复制下层配置
        ti->flags = BLK_INTEGRITY_STACKED | ...;
        ti->csum_type = bi->csum_type;
        ti->metadata_size = bi->metadata_size;
        ti->interval_exp = bi->interval_exp;
        ti->tag_size = bi->tag_size;
        ti->pi_tuple_size = bi->pi_tuple_size;
        ti->pi_offset = bi->pi_offset;
    }
    return true;

incompatible:
    memset(ti, 0, sizeof(*ti));
    return false;
}
```

#### 12.1.7 完整性 sysfs 接口

通过 `blk_integrity_attr_group` 导出到 `/sys/block/<disk>/integrity/`：

| 属性 | 读/写 | 说明 |
|------|-------|------|
| `format` | RO | 完整性格式名称 (如 "T10-DIF-TYPE1-CRC") |
| `tag_size` | RO | 应用标签大小 (字节) |
| `protection_interval_bytes` | RO | 保护间隔大小 (字节) |
| `read_verify` | RW | 读取时验证 PI (0=启用, 1=禁用) |
| `write_generate` | RW | 写入时生成 PI (0=启用, 1=禁用) |
| `device_is_integrity_capable` | RO | 设备是否支持完整性 |

**完整性格式名称**（`blk_integrity_profile_name()`）：

```c
const char *blk_integrity_profile_name(struct blk_integrity *bi)
{
    switch (bi->csum_type) {
    case BLK_INTEGRITY_CSUM_IP:
        return bi->flags & BLK_INTEGRITY_REF_TAG ?
               "T10-DIF-TYPE1-IP" : "T10-DIF-TYPE3-IP";
    case BLK_INTEGRITY_CSUM_CRC:
        return bi->flags & BLK_INTEGRITY_REF_TAG ?
               "T10-DIF-TYPE1-CRC" : "T10-DIF-TYPE3-CRC";
    case BLK_INTEGRITY_CSUM_CRC64:
        return bi->flags & BLK_INTEGRITY_REF_TAG ?
               "EXT-DIF-TYPE1-CRC64" : "EXT-DIF-TYPE3-CRC64";
    default:
        return "nop";
    }
}
```

#### 12.1.8 完整性 I/O 合并控制

`blk-integrity.c` 提供两个合并控制函数，确保只有兼容的完整性生物/请求才能合并：

```c
// 检查两个请求的完整性是否可合并
bool blk_integrity_merge_rq(struct request_queue *q,
                            struct request *req, struct request *next)
{
    // 两者都有/都没有完整性负载
    // bip_flags 一致
    // 若检查 app_tag, 值必须一致
    // 合并后 integrity segments 不超限
    // 无 gap
}

// 检查 bio 是否能合并到已有请求
bool blk_integrity_merge_bio(struct request_queue *q,
                             struct request *req, struct bio *bio)
{
    // 类似检查, 用于 bio 合并到 request
}
```

### 12.2 块层内联加密（blk-crypto）

块层内联加密（Inline Encryption）允许存储设备硬件直接对数据进行加密/解密，避免数据在主机内存和磁盘之间传输时的明文暴露，同时提高性能。

#### 12.2.1 体系架构

```text
                        文件系统 / 应用层
                              │
                              ▼
                    ┌──────────────────────────┐
                    │  bio_crypt_set_ctx()      │
                    │  (设置加密上下文)          │
                    └──────────┬───────────────┘
                               │
                    ┌──────────▼───────────────┐
                    │  __blk_crypto_submit_bio()│  ← blk-crypto.c
                    │  (提交路径: 分配 keyslot)  │
                    └──────────┬───────────────┘
                               │
              ┌────────────────┼────────────────┐
              ▼                ▼                ▼
    ┌─────────────────┐ ┌───────────┐ ┌──────────────┐
    │ 硬件内联加密      │ │ 软件回退   │ │ 无加密        │
    │ (设备原生支持)    │ │ (fallback) │ │ (直接提交)    │
    │ blk-crypto-     │ │ blk-crypto│ │              │
    │ profile.c       │ │ -fallback │ │              │
    └─────────────────┘ └───────────┘ └──────────────┘
           │                  │
           ▼                  ▼
    ┌──────────────────────────────────────┐
    │  设备驱动层 (NVMe/UFS/emmc 等)        │
    │  硬件加密引擎或软件加密后提交          │
    └──────────────────────────────────────┘
```

#### 12.2.2 核心数据结构

**加密模式定义**（[blk-crypto.c](file:///home/louis/code/linux/block/blk-crypto.c)）：

```c
const struct blk_crypto_mode blk_crypto_modes[] = {
    [BLK_ENCRYPTION_MODE_AES_256_XTS] = {
        .name = "AES-256-XTS",
        .cipher_str = "xts(aes)",       // Linux Crypto API 名称
        .keysize = 64,                   // 256 位 XTS = 2 × 128 位密钥
        .security_strength = 32,         // 安全强度 (256 位)
        .ivsize = 16,                    // IV 大小 (128 位)
    },
    [BLK_ENCRYPTION_MODE_AES_128_CBC_ESSIV] = {
        .name = "AES-128-CBC-ESSIV",
        .cipher_str = "essiv(cbc(aes),sha256)",
        .keysize = 16,                   // 128 位
        .security_strength = 16,
        .ivsize = 16,
    },
    [BLK_ENCRYPTION_MODE_ADIANTUM] = {
        .name = "Adiantum",
        .cipher_str = "adiantum(xchacha12,aes)",
        .keysize = 32,                   // 256 位
        .security_strength = 32,
        .ivsize = 32,                    // 256 位 IV
    },
    [BLK_ENCRYPTION_MODE_SM4_XTS] = {
        .name = "SM4-XTS",
        .cipher_str = "xts(sm4)",
        .keysize = 32,                   // 256 位 SM4-XTS
        .security_strength = 16,
        .ivsize = 16,
    },
};
```

**加密密钥**（[blk-crypto.h](file:///home/louis/code/linux/include/linux/blk-crypto.h)）：

```c
struct blk_crypto_config {
    enum blk_crypto_mode_num crypto_mode;  // 加密算法
    unsigned int data_unit_size;           // 数据单元大小 (2^n 字节)
    unsigned int dun_bytes;                // DUN 字节数 (1~IV 大小)
    enum blk_crypto_key_type key_type;     // 密钥类型: RAW / HW_WRAPPED
};

struct blk_crypto_key {
    struct blk_crypto_config crypto_cfg;  // 加密配置
    unsigned int data_unit_size_bits;     // 数据单元大小的对数
    unsigned int size;                    // 密钥大小 (字节)
    u8 bytes[BLK_CRYPTO_MAX_RAW_KEY_SIZE]; // 密钥数据 (最大 64 字节)
};
```

**Bio 加密上下文**（[blk-crypto.h](file:///home/louis/code/linux/include/linux/blk-crypto.h)）：

```c
struct bio_crypt_ctx {
    const struct blk_crypto_key *bc_key;   // 加密密钥指针
    u64 bc_dun[BLK_CRYPTO_DUN_ARRAY_SIZE]; // 数据单元编号 (DUN)
};
```

- DUN（Data Unit Number）是每个数据单元的编号，类似于 IV
- 每个数据单元使用 `DUN` 作为 IV 进行加密，保证相同明文在不同位置产生不同密文
- `BLK_CRYPTO_DUN_ARRAY_SIZE = 2`，支持 128 位 DUN

**加密配置文件（Crypto Profile）**（[blk-crypto-profile.h](file:///home/louis/code/linux/include/linux/blk-crypto-profile.h)）：

```c
struct blk_crypto_ll_ops {
    // 编程密钥：将密钥写入硬件 keyslot
    int (*keyslot_program)(struct blk_crypto_profile *profile,
                          const struct blk_crypto_key *key,
                          unsigned int slot);
    // 擦除密钥：从硬件 keyslot 中删除密钥
    int (*keyslot_evict)(struct blk_crypto_profile *profile,
                        const struct blk_crypto_key *key,
                        unsigned int slot);
    // 生成硬件包装密钥
    int (*generate_key)(struct blk_crypto_profile *profile,
                       u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
    // 准备硬件包装密钥 (长期包装 → 短期包装)
    int (*prepare_key)(struct blk_crypto_profile *profile,
                      const u8 *lt_key, size_t lt_key_size,
                      u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
};

struct blk_crypto_profile {
    struct blk_crypto_ll_ops ll_ops;    // 驱动层操作函数
    unsigned int max_dun_bytes_supported; // 最大 DUN 字节数
    unsigned int key_types_supported;    // 支持密钥类型 (RAW/HW_WRAPPED)
    unsigned int modes_supported[BLK_ENCRYPTION_MODE_MAX]; // 位图: 支持的算法×数据单元大小
    struct device *dev;                  // 运行时电源管理设备

    // 以下字段由 blk_crypto_profile_init() 管理:
    struct blk_crypto_keyslot *slots;    // keyslot 数组
    unsigned int num_slots;              // keyslot 数量
    struct list_head idle_slots;         // 空闲 keyslot 链表
    struct hlist_head *slot_hashtable;   // keyslot 哈希表 (按密钥指针)
    struct rw_semaphore lock;            // 保护 keyslot 管理的读写锁
};
```

**Keyslot 结构**（[blk-crypto-profile.c](file:///home/louis/code/linux/block/blk-crypto-profile.c)）：

```c
struct blk_crypto_keyslot {
    atomic_t slot_refs;                    // 引用计数 (当前使用该 slot 的 I/O 请求数)
    struct list_head idle_slot_node;       // 空闲链表节点
    struct hlist_node hash_node;           // 哈希表节点
    const struct blk_crypto_key *key;      // 当前编程的密钥指针
    struct blk_crypto_profile *profile;    // 所属 profile
};
```

#### 12.2.3 加密 I/O 提交路径

```text
submit_bio(bio)
  │
  ├─ 若 bio->bi_crypt_context 存在:
  │   └─ __blk_crypto_submit_bio(bio)              # blk-crypto.c
  │       │
  │       ├─ 1. 检查 bio 是否有数据
  │       │
  │       ├─ 2. 检查设备是否原生支持该加密配置:
  │       │   ├─ 是 → 直接返回 true (继续提交到驱动)
  │       │   │
  │       │   └─ 否 → 检查 fallback 是否启用:
  │       │       ├─ 是 → blk_crypto_fallback_bio_prep(bio)
  │       │       │   ├─ WRITE: 加密后提交
  │       │       │   │   └─ blk_crypto_fallback_encrypt_bio(bio)
  │       │       │   │       ├─ 分配 bounce page
  │       │       │   │       ├─ 使用 crypto API 进行加密
  │       │       │   │       └─ 提交加密后的 bio
  │       │       │   │
  │       │       │   └─ READ: 标记为解密后完成
  │       │       │       └─ 替换 bi_end_io 为解密回调
  │       │       │           └─ blk_crypto_fallback_decrypt_endio
  │       │       │               └─ 工作队列: blk_crypto_fallback_decrypt_bio
  │       │       │
  │       │       └─ 否 → 返回错误 (BLK_STS_NOTSUPP)
  │       │
  │       └─ 3. 返回 true (bio 继续提交)
  │
  └─ blk_mq_submit_bio(rq)
      │
      ├─ __blk_crypto_rq_bio_prep(rq, bio, gfp)     # 复制加密上下文到 request
      │
      └─ blk_crypto_rq_get_keyslot(rq)               # 分配硬件 keyslot
          │
          └─ blk_crypto_get_keyslot(profile, key, &slot_ptr)  # blk-crypto-profile.c
              │
              ├─ 1. 哈希查找: 密钥是否已编程到某个 slot?
              │   ├─ 是 → 增加引用计数, 返回 slot
              │   └─ 否 → 继续
              │
              ├─ 2. 从空闲链表获取一个 slot
              │   ├─ 有 → 使用
              │   └─ 无 → 等待 (wait_event)
              │
              └─ 3. 调用 ll_ops.keyslot_program(profile, key, slot)
                  └─ 驱动将密钥编程到硬件
```

#### 12.2.4 软件加密回退（fallback）机制

当设备不支持硬件加密时，`blk-crypto-fallback.c` 提供软件加密回退：

```c
// fallback 使用 Linux Crypto API 进行软件加密
// 预分配资源:
//   - 100 个 keyslot (可通过参数调整)
//   - 每个 keyslot 包含每个加密模式一个 tfm
//   - 128 个预分配 fallback 上下文
//   - bounce page 内存池

bool blk_crypto_fallback_bio_prep(struct bio *bio)
{
    if (bio_data_dir(bio) == WRITE) {
        // 写入: 加密后提交
        // 1. 分配 fallback 上下文
        // 2. 分配 bounce page
        // 3. 使用 crypto API 加密数据
        // 4. 提交加密后的 bio (无加密上下文)
        // 5. 原始 bio 在加密完成后结束
        blk_crypto_fallback_encrypt_bio(bio);
        return false;  // bio 已被消费
    } else {
        // 读取: 标记为解密后完成
        // 1. 替换 bi_end_io 为 blk_crypto_fallback_decrypt_endio
        // 2. 提交原始 bio (无加密上下文)
        // 3. 完成时: 工作队列中解密数据
        // 4. 恢复原始 bi_end_io 并调用
        return true;   // bio 继续提交
    }
}
```

**fallback 加密流程**：

```
WRITE:  bio → [加密] → 加密后的 bio → 设备
                        ↓
                   bounce pages
                   (明文 → 密文)

READ:   bio → 设备 → [解密] → 解密后的 bio
                      ↓
                工作队列处理
                (密文 → 明文)
```

#### 12.2.5 密钥管理

**密钥初始化**（`blk_crypto_init_key()`）：

```c
int blk_crypto_init_key(struct blk_crypto_key *blk_key,
                        const u8 *key_bytes, size_t key_size,
                        enum blk_crypto_key_type key_type,
                        enum blk_crypto_mode_num crypto_mode,
                        unsigned int dun_bytes,
                        unsigned int data_unit_size)
{
    // 1. 验证加密模式有效性
    // 2. 验证密钥大小 (RAW 模式必须匹配 keysize)
    // 3. 验证 HW_WRAPPED 密钥大小范围
    // 4. 验证 dun_bytes 范围 (1 ~ ivsize)
    // 5. 验证 data_unit_size 为 2 的幂
    // 6. 填充 blk_crypto_key
}
```

**Bio 加密上下文设置**（`bio_crypt_set_ctx()`）：

```c
void bio_crypt_set_ctx(struct bio *bio, const struct blk_crypto_key *key,
                       const u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
                       gfp_t gfp_mask)
{
    struct bio_crypt_ctx *bc = mempool_alloc(bio_crypt_ctx_pool, gfp_mask);
    bc->bc_key = key;
    memcpy(bc->bc_dun, dun, sizeof(bc->bc_dun));
    bio->bi_crypt_context = bc;
}
```

**密钥擦除**（`__blk_crypto_evict_key()`）：

```c
int __blk_crypto_evict_key(struct blk_crypto_profile *profile,
                           const struct blk_crypto_key *key)
{
    // 1. 哈希查找 key 所在的 slot
    // 2. 等待 slot 引用计数降为 0
    // 3. 调用 ll_ops.keyslot_evict() 从硬件擦除密钥
    // 4. 从哈希表移除
    // 5. 将 slot 移回空闲链表
}
```

#### 12.2.6 DUN 处理

DUN（Data Unit Number）确保加密后的数据在不同位置具有唯一性：

```c
// DUN 递增 (多字节大整数加法)
void bio_crypt_dun_increment(u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
                             unsigned int inc)
{
    for (int i = 0; inc && i < BLK_CRYPTO_DUN_ARRAY_SIZE; i++) {
        dun[i] += inc;
        if (dun[i] < inc)  // 溢出 → 进位
            inc = 1;
        else
            inc = 0;
    }
}

// DUN 连续检查 (用于 bio 合并判断)
bool bio_crypt_dun_is_contiguous(const struct bio_crypt_ctx *bc,
                                 unsigned int bytes,
                                 const u64 next_dun[BLK_CRYPTO_DUN_ARRAY_SIZE])
{
    // 计算 bc->bc_dun + bytes 是否等于 next_dun
}

// 加密上下文合并检查
bool bio_crypt_ctx_mergeable(struct bio_crypt_ctx *bc1,
                             unsigned int bc1_bytes,
                             struct bio_crypt_ctx *bc2)
{
    // 1. 检查密钥是否相同 (同一把密钥)
    // 2. 检查 DUN 是否连续
}
```

#### 12.2.7 加密配置文件注册

```c
// 设备驱动调用 blk_crypto_register() 注册加密能力
// 定义在 include/linux/blkdev.h
#ifdef CONFIG_BLK_INLINE_ENCRYPTION
bool blk_crypto_register(struct blk_crypto_profile *profile,
                         struct request_queue *q);
#endif

// 驱动实现步骤:
// 1. blk_crypto_profile_init(&profile, num_slots)
// 2. 填充 profile.ll_ops (keyslot_program, keyslot_evict, ...)
// 3. 填充 profile.modes_supported (支持哪些算法和数据单元大小)
// 4. 调用 blk_crypto_register(&profile, q)
```

#### 12.2.8 加密与完整性互斥

```c
// bio-integrity.c 中禁止完整性 + 加密同时使用
if (WARN_ON_ONCE(bio_has_crypt_ctx(bio)))
    return ERR_PTR(-EOPNOTSUPP);

// 这是设计限制: 硬件加密会修改数据, 使 PI 校验和失效
```

### 12.3 SED/Opal 自加密驱动器

TCG Opal 是自加密驱动器（Self-Encrypting Drive, SED）的安全标准。Linux 内核通过 `sed-opal.c` 提供 Opal 驱动管理功能。

#### 12.3.1 架构概述

```text
                    用户空间 (sedutil / libata)
                          │
                          ▼
                    ┌──────────────────────┐
                    │  sed-opal ioctl       │
                    │  (SED_OPAL_*)         │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  sed-opal.c          │
                    │  (3,351 行)          │
                    │  TCG Opal 协议实现    │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  opal_proto.h        │
                    │  (485 行)            │
                    │  Opal 协议命令行定义  │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  块设备驱动           │
                    │  (ATA/NVMe 命令)      │
                    └──────────────────────┘
```

#### 12.3.2 核心数据结构

```c
struct opal_dev {
    u32             flags;            // 标志位
    void            *data;            // 驱动私有数据
    sec_send_recv   *send_recv;       // 发送/接收安全命令的函数指针
    struct mutex    dev_lock;         // 设备互斥锁
    u16             comid;            // 通信 ID
    u32             hsn;              // 主机会话号
    u32             tsn;              // 目标会话号
    u64             align;            // 对齐粒度
    u64             lowest_lba;       // 最低 LBA
    u32             logical_block_size; // 逻辑块大小
    u8              align_required;   // 是否需要对齐
    size_t          pos;              // 命令缓冲区当前位置
    u8              *cmd;             // 命令缓冲区
    u8              *resp;            // 响应缓冲区
};

// Opal 命令步骤 (分步执行)
struct opal_step {
    int (*fn)(struct opal_dev *dev, void *data);
    void *data;
};
```

#### 12.3.3 支持的 opal 操作

通过 `sed-opal.c` 的 ioctl 接口，用户空间可执行以下操作：

| 操作 | 功能 |
|------|------|
| `SED_OPAL_LOCK_UNLOCK` | 锁定/解锁 Opal 范围 |
| `SED_OPAL_ADD_LOCKING_RANGE` | 添加锁定范围 |
| `SED_OPAL_ERASE_LOCKING_RANGE` | 擦除锁定范围数据 |
| `SED_OPAL_ACTIVATE_LSP` | 激活锁 SP |
| `SED_OPAL_SETUP_LSP` | 设置锁 SP 密码 |
| `SED_OPAL_MBR_DONE` | 完成 MBR 阴影 |
| `SED_OPAL_WRITE_MBR` | 写入 MBR 阴影数据 |
| `SED_OPAL_TPR` | 管理权限 |
| `SED_OPAL_PW_LIFECYCLE` | 管理密码生命周期 |
| `SED_OPAL_SECURE_ERASE` | 安全擦除 |

#### 12.3.4 Opal 协议层（[opal_proto.h](file:///home/louis/code/linux/block/opal_proto.h)）

Opal 协议基于 TCG 存储安全标准，使用 Tiny Atom 编码：

```c
// Opal 原子编码格式
enum opal_atom_width {
    OPAL_WIDTH_TINY,    // 1 字节 (6 位数据 + 2 位控制)
    OPAL_WIDTH_SHORT,   // 2 字节
    OPAL_WIDTH_MEDIUM,  // 3 字节
    OPAL_WIDTH_LONG,    // 6 字节
    OPAL_WIDTH_TOKEN,   // 可变长度 token
};

// Opal 响应 token 解析
struct opal_resp_tok {
    const u8 *pos;                    // token 在缓冲区中的位置
    size_t len;                       // token 长度
    enum opal_response_token type;    // token 类型
    enum opal_atom_width width;       // 编码宽度
    union {
        u64 u;                        // 无符号值
        s64 s;                        // 有符号值
    } stored;
};
```

### 12.4 数据完整性与加密对比总结

| 特性 | 数据完整性 (DIF/DIX) | 块层内联加密 (blk-crypto) | SED/Opal |
|------|----------------------|--------------------------|----------|
| **核心文件** | `blk-integrity.c`, `bio-integrity.c`, `t10-pi.c` | `blk-crypto.c`, `blk-crypto-profile.c`, `blk-crypto-fallback.c` | `sed-opal.c`, `opal_proto.h` |
| **总代码行数** | ~1,645 行 | ~1,923 行 | ~3,836 行 |
| **目的** | 检测数据损坏/篡改 | 防止数据泄露 | 防止数据泄露 (磁盘级) |
| **实现位置** | 块层 (bio/request 层) | 块层 (bio/request 层) | 用户空间 ioctl 触发 |
| **硬件要求** | 可选 (可软件生成/验证) | 可选 (有 fallback) | 必需 (硬件加密引擎) |
| **标准** | T10 SCSI / NVMe PI | 无统一标准 | TCG Opal 2.0 |
| **数据单元** | 扇区 (512B/4KB) | 可配置 (512B~64KB) | 整个磁盘或 LBA 范围 |
| **密钥管理** | 无 | 内核管理 (keyslot) | 驱动器内部管理 |
| **性能影响** | 低 (硬件校验) | 低 (硬件加密) / 中 (fallback) | 无 (硬件加密) |
| **覆盖范围** | 每个扇区的校验和 | 每个数据单元的加密 | 整个磁盘加密 |
| **与加密互斥** | — | 不兼容完整性 | 独立于块层 |
| **配置方式** | sysfs 属性 | ioctl + crypto profile | ioctl |

**I/O 路径中的调用点**：

```text
提交路径 (submit)                     完成路径 (complete)
──────────────────────────────────    ──────────────────────────────────
↓ bio_integrity_prep()                ↓ bio_integrity_endio()
  → 分配/生成 PI (写)                    → 验证 PI (读, 工作队列)
  → 保存 iter (读)                      → 释放 PI 缓冲区
                                       ↓ blk_integrity_complete()
↓ __blk_crypto_submit_bio()              → 恢复 ref_tag
  → 分配 keyslot
  → 硬件加密或 fallback
                                       ↓ blk_crypto_fallback 完成
↓ blk_mq_submit_bio()                    → 解密数据 (读)
  → __blk_crypto_rq_bio_prep()           → 恢复原始 bi_end_io
  → blk_crypto_rq_get_keyslot()
```

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

通过 sysfs 导出块层队列属性，用户空间可通过 `cat /sys/block/<dev>/queue/<attr>` 查看和修改队列参数。

#### 14.1.1 属性描述结构

每个属性由 `struct queue_sysfs_entry` 描述，支持 show/store 以及 limit 版本的 show_limit/store_limit：

```c
struct queue_sysfs_entry {
    struct attribute attr;
    ssize_t (*show)(struct gendisk *disk, char *page);
    ssize_t (*show_limit)(struct gendisk *disk, char *page);
    ssize_t (*store)(struct gendisk *disk, const char *page, size_t count);
    int (*store_limit)(struct gendisk *disk, const char *page,
            size_t count, struct queue_limits *lim);
};
```

区分两种 show/store 函数：
- **普通版**（show/store）：直接操作 `request_queue` 字段，如 `nr_requests`、`read_ahead_kb`
- **Limit 版**（show_limit/store_limit）：通过 `queue_limits` 机制，支持原子提交和回滚。写操作调用 `queue_limits_start_update()` → `store_limit` → `queue_limits_commit_update_frozen()`，确保一致性。

#### 14.1.2 属性分组

属性分为两组，分别以 `struct attribute_group` 管理：

**`queue_attrs`** — 通用属性（bio-based 和 request-based 队列均可见）：

| 属性 | 类型 | 说明 |
|------|------|------|
| `max_hw_sectors_kb` | RO | 硬件最大扇区数（KB） |
| `max_sectors_kb` | RW | 用户可调最大扇区数（KB） |
| `max_segments` | RO | 最大段数 |
| `max_discard_segments` | RO | 最大丢弃段数 |
| `max_integrity_segments` | RO | 最大完整性段数 |
| `max_segment_size` | RO | 最大段大小 |
| `max_write_streams` | RO | 最大写流数 |
| `write_stream_granularity` | RO | 写流粒度 |
| `logical_block_size` | RO | 逻辑块大小 |
| `physical_block_size` | RO | 物理块大小 |
| `chunk_sectors` | RO | 块对齐扇区数 |
| `minimum_io_size` | RO | 最小 I/O 大小 |
| `optimal_io_size` | RO | 最优 I/O 大小 |
| `discard_granularity` | RO | 丢弃粒度 |
| `discard_max_bytes` | RW | 用户可调最大丢弃字节数 |
| `discard_max_hw_bytes` | RO | 硬件最大丢弃字节数 |
| `write_zeroes_max_bytes` | RO | 最大写零字节数 |
| `zone_append_max_bytes` | RO | Zone Append 最大字节数 |
| `zone_write_granularity` | RO | Zone 写粒度 |
| `zoned` | RO | Zoned 设备类型 |
| `max_open_zones` | RO | 最大打开 zone 数 |
| `max_active_zones` | RO | 最大活跃 zone 数 |
| `rotational` | RW | 是否为旋转设备 |
| `iostats` | RW | I/O 统计开关 |
| `add_random` | RW | 是否贡献熵池 |
| `stable_writes` | RW | 稳定写开关 |
| `write_cache` | RW | 写缓存策略（write back/write through） |
| `fua` | RO | FUA 支持 |
| `dax` | RO | DAX 支持 |
| `virt_boundary_mask` | RO | 虚拟边界掩码 |
| `dma_alignment` | RO | DMA 对齐要求 |
| `read_ahead_kb` | RW | 预读大小（KB） |
| `hw_sector_size` | RO | 硬件扇区大小（logical_block_size 别名） |
| `atomic_write_max_bytes` | RO | 原子写最大字节数 |
| `atomic_write_unit_max_bytes` | RO | 原子写单元最大字节数 |
| `atomic_write_unit_min_bytes` | RO | 原子写单元最小字节数 |

**`blk_mq_queue_attrs`** — 仅 request-based（blk-mq）队列可见：

| 属性 | 类型 | 说明 |
|------|------|------|
| `scheduler` | RW | 当前 I/O 调度器（切换/查看） |
| `nr_requests` | RW | 队列最大请求数 |
| `async_depth` | RW | 异步请求深度 |
| `wbt_lat_usec` | RW | WBT 延迟目标（微秒） |
| `rq_affinity` | RW | 请求 CPU 亲和性 |
| `io_timeout` | RW | I/O 超时时间（毫秒） |

#### 14.1.3 属性可见性控制

`queue_attr_visible()` 和 `blk_mq_queue_attr_visible()` 控制属性可见性：

- `max_open_zones` / `max_active_zones`：仅对 zoned 设备可见
- blk-mq 属性组：仅对 request-based 队列可见
- `io_timeout`：仅当驱动实现了 `timeout` 回调时可见

#### 14.1.4 队列注册与注销

```text
# 注册流程
blk_register_queue(disk)
  ├─ kobject_add(&disk->queue_kobj, ...)       # 创建 queue 目录
  ├─ blk_mq_sysfs_register(disk)               # 注册 blk-mq 特定属性
  ├─ debugfs_create_dir(disk->disk_name, ...)   # 创建 debugfs 目录
  ├─ blk_mq_debugfs_register(q)                # 注册 debugfs 文件
  ├─ disk_register_independent_access_ranges()  # 注册独立访问范围
  ├─ blk_crypto_sysfs_register(disk)           # 注册加密属性
  ├─ elevator_set_default(q)                    # 设置默认调度器
  └─ kobject_uevent(&disk->queue_kobj, KOBJ_ADD) # 发送 uevent

# 注销流程
blk_unregister_queue(disk)
  ├─ blk_trace_shutdown(q)                     # 关闭 blktrace
  ├─ debugfs_remove_recursive(q->debugfs_dir)   # 移除 debugfs 目录
  ├─ blk_mq_sysfs_unregister(disk)             # 注销 blk-mq 属性
  └─ kobject_del(&disk->queue_kobj)            # 删除 queue 目录
```

### 14.2 blk-mq-debugfs.c / blk-mq-debugfs.h

文件：`block/blk-mq-debugfs.c` / `block/blk-mq-debugfs.h`

提供 debugfs 调试接口，挂载点：`/sys/kernel/debug/block/<disk>/`。

#### 14.2.1 队列级 debugfs 文件

| 文件 | 权限 | 功能 |
|------|------|------|
| `poll_stat` | 0400 | 轮询统计（当前为空） |
| `requeue_list` | 0400 | 显示被重新入队的请求列表 |
| `pm_only` | 0600 | 显示 PM-only 计数器值 |
| `state` | 0600 | 显示/修改队列状态标志 |
| `zone_wplugs` | 0400 | 显示 zone write plug 状态 |

**`state` 文件**：可写入 `run`、`start`、`kick` 操作队列：
```c
static ssize_t queue_state_write(void *data, const char __user *buf,
                 size_t count, loff_t *ppos)
{
    // "run"   → blk_mq_run_hw_queues(q, true)
    // "start" → blk_mq_start_stopped_hw_queues(q, true)
    // "kick"  → blk_mq_kick_requeue_list(q)
}
```

**队列状态标志**：通过 `blk_flags_show()` 以符号名显示所有 `QUEUE_FLAG_*` 位：

| 标志 | 含义 |
|------|------|
| `QUEUE_FLAG_DYING` | 队列正在销毁 |
| `QUEUE_FLAG_NOMERGES` | 禁止合并 |
| `QUEUE_FLAG_SAME_COMP` | 同 CPU 完成 |
| `QUEUE_FLAG_FAIL_IO` | 模拟 I/O 失败 |
| `QUEUE_FLAG_STATS` | 统计已启用 |
| `QUEUE_FLAG_REGISTERED` | 已注册 sysfs |
| `QUEUE_FLAG_QUIESCED` | 已静默 |
| `QUEUE_FLAG_QOS_ENABLED` | QoS 已启用 |
| `QUEUE_FLAG_BIO_ISSUE_TIME` | 记录 BIO 发起时间 |

#### 14.2.2 硬件队列级 debugfs 文件

| 文件 | 功能 |
|------|------|
| `state` | 硬件队列状态（STOPPED/TAG_ACTIVE/SCHED_RESTART/INACTIVE） |
| `flags` | 硬件队列标志（TAG_QUEUE_SHARED/STACKING/BLOCKING 等） |
| `dispatch` | 派发队列中的请求列表 |
| `busy` | 显示所有正在处理的请求 |
| `tags` | 标签分配器信息（nr_tags, active_queues, bitmap） |
| `tags_bitmap` | 标签位图 |
| `sched_tags` | 调度器标签分配器信息 |
| `sched_tags_bitmap` | 调度器标签位图 |
| `active` | 当前活跃请求数 |
| `dispatch_busy` | 派发忙计数 |
| `type` | 硬件队列类型（default/read/poll） |
| `ctx_map` | CPU 上下文映射位图 |

**请求显示格式**：`__blk_mq_debugfs_rq_show()` 输出每个请求的详细信息：
```
{.op=READ, .cmd_flags=REQ_SYNC|REQ_META, .rq_flags=RQF_STARTED|RQF_STATS,
 .state=in_flight, .tag=42, .internal_tag=-1}
```

#### 14.2.3 软件队列（ctx）级 debugfs 文件

| 文件 | 功能 |
|------|------|
| `read_rq_list` | 读请求列表 |
| `write_rq_list` | 写请求列表 |
| `poll_rq_list` | 轮询请求列表 |

#### 14.2.4 注册与注销

```c
void blk_mq_debugfs_register(struct request_queue *q);       // 注册队列级文件
void blk_mq_debugfs_register_hctxs(struct request_queue *q); // 注册所有 hctx
void blk_mq_debugfs_register_sched(struct request_queue *q); // 注册调度器文件
void blk_mq_debugfs_register_rq_qos(struct request_queue *q); // 注册 QoS 文件
```

### 14.3 blktrace — 块层跟踪

块层通过 `include/trace/events/block.h` 定义了一套完整的 tracepoint 系统，可在运行时通过 ftrace / perf 捕获。

#### 14.3.1 请求级 Tracepoints

| Tracepoint | 触发时机 | 关键参数 |
|-----------|---------|---------|
| `block_rq_insert` | 请求插入队列 | dev, sector, nr_sector, rwbs, ioprio, comm |
| `block_rq_issue` | 请求下发到驱动 | dev, sector, nr_sector, rwbs, ioprio, comm |
| `block_rq_complete` | 请求完成 | dev, sector, nr_sector, error, ioprio, rwbs |
| `block_rq_error` | 请求出错 | dev, sector, nr_sector, error, ioprio, rwbs |
| `block_rq_requeue` | 请求重新入队 | dev, sector, nr_sector, rwbs, ioprio |
| `block_rq_merge` | 请求合并 | dev, sector, nr_sector, rwbs, bytes |

#### 14.3.2 Bio 级 Tracepoints

| Tracepoint | 触发时机 | 关键参数 |
|-----------|---------|---------|
| `block_bio_complete` | bio 完成 | dev, sector, nr_sector, error, rwbs |
| `block_bio_queue` | bio 入队 | dev, sector, nr_sector, rwbs, comm |
| `block_bio_backmerge` | bio 向后合并 | dev, sector, nr_sector, rwbs |
| `block_bio_frontmerge` | bio 向前合并 | dev, sector, nr_sector, rwbs |
| `block_bio_remap` | bio 重映射 | dev, sector, nr_sector, old_dev, old_sector |
| `block_split` | bio 拆分 | dev, sector, new_sector, rwbs |
| `block_getrq` | 分配请求 | dev, sector, nr_sector, rwbs |

#### 14.3.3 Buffer Head 级 Tracepoints

| Tracepoint | 触发时机 |
|-----------|---------|
| `block_touch_buffer` | 访问 buffer_head |
| `block_dirty_buffer` | 标记 buffer_head 脏 |

#### 14.3.4 其他 Tracepoints

| Tracepoint | 触发时机 |
|-----------|---------|
| `block_plug` | 队列插上 |
| `block_unplug` | 队列拔插（含请求数） |
| `block_rq_remap` | 请求重映射 |
| `blk_zone_append_update_request_bio` | Zone Append 完成更新 sector |
| `blkdev_zone_mgmt` | Zone 管理操作 |
| `disk_zone_wplug_add_bio` | 向 zone write plug 添加 bio |
| `blk_zone_wplug_bio` | Zone write plug 处理 bio |

#### 14.3.5 输出格式说明

`rwbs` 字段是一个 5 字符的 I/O 操作描述串，由 `blk_fill_rwbs()` 生成：

| 字符位置 | 含义 |
|---------|------|
| 1 | R=读, W=写, D=丢弃, T=flush, A=zone append |
| 2 | R=读取, W=写入, B=屏障（废弃） |
| 3 | S=同步, F=force_unit_access |
| 4 | A=预读, M=元数据 |
| 5 | M=meta, S=同步（复用） |

**典型输出示例**：
```
  <...>-12345 [000] ...1 123.456: block_rq_issue: 8,0 W 4096 () 12345 + 8 [dd]
  <...>-12345 [000] d... 123.789: block_rq_complete: 8,0 W () 12345 + 8 [0]
```

### 14.4 ioctl.c（975 行）

文件：`block/ioctl.c`

处理块设备 ioctl 系统调用，主要功能分组：

**分区管理**：
- `BLKPG_ADD_PARTITION` — 添加分区（`blkpg_do_ioctl()` → `bdev_add_partition()`）
- `BLKPG_DEL_PARTITION` — 删除分区（`bdev_del_partition()`）
- `BLKPG_RESIZE_PARTITION` — 调整分区大小

**设备信息查询**：
- `BLKGETSIZE` / `BLKGETSIZE64` — 获取设备大小
- `BLKSSZGET` — 获取逻辑块大小
- `BLKPBSZGET` — 获取物理块大小
- `BLKALIGNOFF` — 获取对齐偏移
- `BLKROGET` / `BLKROSET` — 获取/设置只读状态
- `BLKRRPART` — 重新读取分区表

**I/O 参数控制**：
- `BLKFLSBUF` — 刷新缓冲区
- `BLKROTATIONAL` — 获取/设置旋转标志
- `BLKRASET` / `BLKRAGET` — 设置/获取预读大小
- `BLKFRASET` / `BLKFRAGET` — 设置/获取文件预读大小
- `BLKSECTGET` — 获取最大扇区数
- `BLKIOMIN` / `BLKIOOPT` — 获取最小/最优 I/O 大小
- `BLKDISCARD` / `BLKSECDISCARD` — 丢弃/安全丢弃扇区
- `BLKZEROOUT` — 写零
- `BLKWSAME` — 写相同数据

**多队列管理**：
- `BLKTRACESETUP` / `BLKTRACESTART` / `BLKTRACESTOP` / `BLKTRACETEARDOWN` — blktrace 控制
- `BLKSTONRA` — 设置 NVMe 流数量

### 14.5 fops.c（978 行）

文件：`block/fops.c`

实现块设备文件操作集 `def_blk_fops`：

```c
const struct file_operations def_blk_fops = {
    .open           = blkdev_open,       // 打开块设备
    .release        = blkdev_release,    // 关闭块设备
    .read_iter      = blkdev_read_iter,  // 读（使用 iov_iter）
    .write_iter     = blkdev_write_iter, // 写（使用 iov_iter）
    .mmap           = blkdev_mmap,       // 内存映射
    .fsync          = blkdev_fsync,      // 同步刷盘
    .unlocked_ioctl = blkdev_ioctl,      // ioctl 处理
    .compat_ioctl   = compat_blkdev_ioctl, // 32 位兼容 ioctl
    .splice_read    = blkdev_splice_read,  // splice 读
    .splice_write   = blkdev_splice_write, // splice 写
    .iopoll         = blkdev_iopoll,       // 轮询 I/O 完成
};
```

**读路径**：`blkdev_read_iter()` → `blkdev_read_folio()` / `__blkdev_direct_IO_simple()` / `__blkdev_direct_IO()`：
- 小块 I/O 使用 `blkdev_read_folio()`（通过页缓存）
- 大块 I/O 使用直接 I/O：`__blkdev_direct_IO_simple()`（单 bio）或 `__blkdev_direct_IO()`（多 bio、异步）

**写路径**：`blkdev_write_iter()` → `__blkdev_direct_IO_simple()` / `__blkdev_direct_IO()`：
- 始终使用直接 I/O，块设备不支持写回缓存
- 支持 `RWF_DSYNC` / `RWF_SYNC` 标志（通过 `REQ_FUA`）

**原子写**：当 `iocb->ki_flags & IOCB_ATOMIC` 时，bio 添加 `REQ_ATOMIC` 标志。

### 14.6 bsg.c / bsg-lib.c

文件：`block/bsg.c`（277 行）/ `block/bsg-lib.c`（412 行）

实现块层 SCSI 通用（BSG）接口，提供从用户空间直接发送 SCSI 命令到块设备的通道：

- **bsg.c**：字符设备接口层，管理 `bsg_device` 结构，处理 `SG_IO` 和 `SCSI_IOCTL` 命令
- **bsg-lib.c**：BSG 库函数，提供 `bsg_setup_queue()` 和 `bsg_remove_queue()` 供驱动注册 BSG 设备

### 14.7 disk-events.c（489 行）

文件：`block/disk-events.c`

监控磁盘事件（介质变更、弹出请求等），支持两种检测方式：

- **轮询模式**：定时轮询检测介质状态变化（`disk_events_poll_jiffies()`）
- **事件通知**：通过 sysfs 暴露 `events`、`events_async`、`events_poll_msecs` 属性

**核心数据结构**：
```c
struct disk_events {
    struct list_head    node;           // 全局 disk_events 链表
    struct gendisk      *disk;          // 关联的 gendisk
    struct mutex        block_mutex;    // 保护阻塞计数
    unsigned int        pending;        // 已发出的事件
    unsigned int        clearing;       // 正在清除的事件
    long                poll_msecs;     // 轮询间隔
    struct delayed_work dwork;          // 轮询工作项
};
```

**支持的事件类型**：
- `DISK_EVENT_MEDIA_CHANGE` — 介质变更
- `DISK_EVENT_EJECT_REQUEST` — 弹出请求

### 14.8 holder.c

文件：`block/holder.c`

管理块设备持有者关系，通过 sysfs 创建 `holders/` 和 `slaves/` 符号链接：

```c
// 例如：dm-0 映射到 sda
// /sys/block/dm-0/slaves/sda → /sys/block/sda
// /sys/block/sda/holders/dm-0 → /sys/block/dm-0

bd_link_disk_holder(bdev, disk)   // 创建持有者链接
bd_unlink_disk_holder(bdev, disk) // 移除持有者链接
```

### 14.9 early-lookup.c（316 行）

文件：`block/early-lookup.c`

在内核启动早期阶段，当根文件系统尚未挂载时，提供块设备查找功能：

- **early_lookup_bdev()**：通过设备名（如 `PARTUUID=xxx` 或 `/dev/nvme0n1p2`）查找块设备
- 支持 `PARTUUID`、`PARTLABEL`、`UUID`、`LABEL` 等多种标识符

---

## 15. 超时与电源管理

### 15.1 blk-timeout.c

文件：`block/blk-timeout.c`

实现请求超时处理机制，核心是每请求定时器和超时扫描。

#### 15.1.1 超时定时器管理

每个请求队列有一个全局定时器 `q->timeout`，管理队列中所有请求的超时：

```c
void blk_add_timer(struct request *req)
{
    // 1. 设置请求超时时间
    if (!req->timeout)
        req->timeout = q->rq_timeout;  // 默认队列超时时间
    req->rq_flags &= ~RQF_TIMED_OUT;

    // 2. 计算 deadline
    expiry = jiffies + req->timeout;
    WRITE_ONCE(req->deadline, expiry);

    // 3. 调整队列定时器
    expiry = blk_rq_timeout(blk_round_jiffies(expiry));  // 四舍五入到秒
    if (!timer_pending(&q->timeout) ||
        time_before(expiry, q->timeout.expires))
        mod_timer(&q->timeout, expiry);
}
```

**时间粒度**：`blk_round_jiffies()` 将超时时间四舍五入到最近的秒边界，用于合并定时器以减少 CPU 唤醒次数：
```c
static inline unsigned long blk_round_jiffies(unsigned long j)
{
    return (j + blk_timeout_mask) + 1;  // blk_timeout_mask = roundup_pow_of_two(HZ) - 1
}
```

#### 15.1.2 超时处理流程

```text
# 超时处理调用栈
timer_expiry → q->timeout
  │
  └─ blk_rq_timed_out_timer()
      │  # 遍历队列中所有请求，检查 deadline 是否已过期
      │
      └─ [for each request in flight]:
          │
          ├─ if time_after(jiffies, req->deadline):
          │   │  # 请求超时
          │   │
          │   └─ req->q->mq_ops->timeout(req, reserved)
          │       │  # 调用驱动层的 timeout 回调
          │       │  # 返回值决定下一步操作：
          │       │
          │       ├─ BLK_EH_DONE      → 请求已处理，无需额外操作
          │       ├─ BLK_EH_RESET_TIMER → 重置定时器，等待更长
          │       └─ BLK_EH_MULTI      → 需要多步恢复
          │
          └─ [继续检查下一个请求]
```

#### 15.1.3 主动终止请求

```c
void blk_abort_request(struct request *req)
{
    // 立即将 deadline 设为当前时间，触发超时扫描
    WRITE_ONCE(req->deadline, jiffies);
    kblockd_schedule_work(&req->q->timeout_work);
}
```

驱动可在错误恢复时调用，强制立即触发超时处理。

#### 15.1.4 I/O 失败注入

编译时通过 `CONFIG_FAIL_IO_TIMEOUT` 启用，通过 `fail_io_timeout=` 内核参数配置：

```c
static DECLARE_FAULT_ATTR(fail_io_timeout);  // 故障注入属性

bool __blk_should_fake_timeout(struct request_queue *q)
{
    return should_fail(&fail_io_timeout, 1);  // 概率性模拟超时
}
```

通过 sysfs 属性 `part_timeout_show`/`part_timeout_store` 控制 `QUEUE_FLAG_FAIL_IO` 标志，启用/禁用 I/O 失败模拟。

### 15.2 blk-pm.c / blk-pm.h

文件：`block/blk-pm.c` / `block/blk-pm.h`

实现基于请求的块设备运行时电源管理（Runtime PM）。

#### 15.2.1 初始化

```c
void blk_pm_runtime_init(struct request_queue *q, struct device *dev)
{
    q->dev = dev;
    q->rpm_status = RPM_ACTIVE;
    pm_runtime_set_autosuspend_delay(q->dev, -1);  // 初始禁止自动挂起
    pm_runtime_use_autosuspend(q->dev);             // 启用自动挂起模式
}
```

#### 15.2.2 运行时状态机

```
  RPM_ACTIVE ──blk_pre_runtime_suspend()──→ RPM_SUSPENDING ──blk_post_runtime_suspend()──→ RPM_SUSPENDED
       ↑                                                                                        │
       └───────blk_post_runtime_resume()─── RPM_RESUMING ──blk_pre_runtime_resume()──────────────┘
```

**挂起流程**（`blk_pre_runtime_suspend()` → `blk_post_runtime_suspend()`）：

```c
int blk_pre_runtime_suspend(struct request_queue *q)
{
    // 1. 设置状态为 SUSPENDING
    q->rpm_status = RPM_SUSPENDING;

    // 2. 增加 pm_only 计数器，阻止新 I/O 进入
    blk_set_pm_only(q);

    // 3. 冻结队列，等待所有进行中的 I/O 完成
    blk_freeze_queue_start(q);
    percpu_ref_switch_to_atomic_sync(&q->q_usage_counter);

    // 4. 检查是否有活跃 I/O
    if (percpu_ref_is_zero(&q->q_usage_counter))
        ret = 0;  // 可以挂起
    else
        ret = -EBUSY;  // 仍有 I/O，不能挂起

    // 5. 恢复队列（冻结只是检查）
    blk_mq_unfreeze_queue_nomemrestore(q);

    // 6. 如果检查失败，恢复状态
    if (ret < 0) {
        q->rpm_status = RPM_ACTIVE;
        pm_runtime_mark_last_busy(q->dev);
        blk_clear_pm_only(q);
    }
    return ret;
}
```

**恢复流程**（`blk_pre_runtime_resume()` → `blk_post_runtime_resume()`）：

```c
void blk_post_runtime_resume(struct request_queue *q)
{
    q->rpm_status = RPM_ACTIVE;
    pm_runtime_mark_last_busy(q->dev);
    pm_request_autosuspend(q->dev);  // 重新调度自动挂起
    blk_clear_pm_only(q);            // 清除 pm_only，允许新 I/O 进入
}
```

#### 15.2.3 请求路径中的 PM 交互

**`blk_pm.h`** 提供的内联函数：

```c
// 在请求下发时检查是否需要恢复设备
static inline int blk_pm_resume_queue(const bool pm, struct request_queue *q)
{
    if (!q->dev || !blk_queue_pm_only(q))
        return 1;  // 无需恢复
    if (pm && q->rpm_status != RPM_SUSPENDED)
        return 1;  // 请求允许（PM 请求或未挂起）
    pm_request_resume(q->dev);  // 请求恢复设备
    return 0;
}

// 在 I/O 完成后标记最后活跃时间
static inline void blk_pm_mark_last_busy(struct request *rq)
{
    if (rq->q->dev && !(rq->rq_flags & RQF_PM))
        pm_runtime_mark_last_busy(rq->q->dev);
}
```

**PM 请求标记**：通过 `RQF_PM` 标志区分：
- 普通请求：`RQF_PM` 未设置，因此在设备挂起时会被阻塞
- PM 请求：`RQF_PM` 已设置，即使在挂起状态也能下发（用于设备唤醒等关键操作）

---

## 16. 统计与跟踪

### 16.1 blk-stat.c / blk-stat.h

文件：`block/blk-stat.c` / `block/blk-stat.h`

块层统计基础设施，基于回调机制收集请求完成延迟数据。

#### 16.1.1 核心数据结构

```c
struct blk_rq_stat {
    u64 min;           // 最小值
    u64 max;           // 最大值
    u64 batch;         // 批次总和（用于计算均值）
    u64 nr_samples;    // 样本数
};

struct blk_stat_callback {
    struct list_head    list;           // 所有回调链表（RCU 保护）
    struct timer_list   timer;          // 统计周期定时器
    struct blk_rq_stat __percpu *cpu_stat; // Per-CPU 统计桶
    int (*bucket_fn)(const struct request *); // 请求分桶函数
    unsigned int        buckets;        // 桶数
    struct blk_rq_stat *stat;           // 聚合后的统计结果
    void (*timer_fn)(struct blk_stat_callback *); // 定时器回调
    void *data;                         // 私有数据
    struct rcu_head     rcu;
};

struct blk_queue_stats {
    struct list_head callbacks;  // 回调链表
    spinlock_t lock;
    int accounting;              // 基础统计计数
};
```

#### 16.1.2 统计收集流程

```text
# 请求完成时
blk_stat_add(rq, now)
  │  # 计算延迟: value = now - rq->io_start_time_ns
  │
  └─ rcu_read_lock
      └─ [遍历 q->stats->callbacks 的所有回调]:
          │
          ├─ bucket = cb->bucket_fn(rq)  # 请求分桶
          └─ stat = per_cpu_ptr(cb->cpu_stat, cpu)[bucket]
              └─ blk_rq_stat_add(stat, value)  # 更新 per-CPU 统计

# 定时器触发时
blk_stat_timer_fn(cb)
  │
  └─ [遍历所有在线 CPU]:
      │  # 聚合 per-CPU 统计到 cb->stat
      blk_rq_stat_sum(&cb->stat[bucket], &cpu_stat[bucket])
      blk_rq_stat_init(&cpu_stat[bucket])  # 重置 per-CPU 统计
      │
      └─ cb->timer_fn(cb)  # 调用用户回调

# 统计值计算
blk_rq_stat_add(stat, value):
    stat->min = min(stat->min, value)
    stat->max = max(stat->max, value)
    stat->batch += value
    stat->nr_samples++

blk_rq_stat_sum(dst, src):
    dst->min = min(dst->min, src->min)
    dst->max = max(dst->max, src->max)
    dst->mean = div_u64(src->batch + dst->mean * dst->nr_samples,
                        dst->nr_samples + src->nr_samples)
    dst->nr_samples += src->nr_samples
```

#### 16.1.3 回调生命周期管理

```c
// 分配回调
struct blk_stat_callback *
blk_stat_alloc_callback(timer_fn, bucket_fn, buckets, data);

// 添加到队列
blk_stat_add_callback(q, cb);    // 设置 QUEUE_FLAG_STATS

// 移除并释放
blk_stat_remove_callback(q, cb); // 清除 QUEUE_FLAG_STATS（若链表为空）
blk_stat_free_callback(cb);      // 通过 RCU 释放

// 激活统计窗口
blk_stat_activate_msecs(cb, msecs);  // 启动定时器
blk_stat_activate_nsecs(cb, nsecs);  // 启动定时器

// 基础统计（不注册回调，仅记录时间/大小）
blk_stat_enable_accounting(q);
blk_stat_disable_accounting(q);
```

### 16.2 块层 Tracepoints

文件：`include/trace/events/block.h`（约 684 行）

使用 ftrace 框架定义，覆盖整个块 I/O 生命周期。详见 [14.3 blktrace](#143-blktrace--块层跟踪)。

**使能方式**：
```bash
# 使用 trace-cmd
trace-cmd record -e block_rq_issue -e block_rq_complete

# 使用 perf
perf record -e block:block_rq_issue -e block:block_rq_complete

# 使用 ftrace
echo block_rq_issue > /sys/kernel/debug/tracing/set_event
```

### 16.3 blk-ioc.c（442 行）

文件：`block/blk-ioc.c`

管理 I/O 上下文（`struct io_context`），关联进程与 I/O 调度器。

#### 16.3.1 核心数据结构

```c
struct io_context {
    atomic_long_t refcount;          // 引用计数
    atomic_t active_ref;             // 活跃引用计数（进程数）
    struct hlist_head icq_list;      // I/O 上下文与队列关联（io_cq）链表
    spinlock_t lock;                 // 保护 icq_list 和 icq_tree
    int ioprio;                      // 当前 I/O 优先级
    struct radix_tree_root icq_tree; // 按队列 ID 索引的 icq 树
    struct io_cq __rcu *icq_hint;   // 最近使用的 icq 缓存（RCU）
    struct work_struct release_work; // 异步释放工作项
};

struct io_cq {
    struct request_queue *q;         // 关联的请求队列
    struct io_context *ioc;          // 关联的 io_context
    struct list_head q_node;         // 队列的 icq 链表节点
    struct hlist_node ioc_node;      // ioc 的 icq 链表节点
    unsigned int flags;              // ICQ_* 标志
};
```

#### 16.3.2 生命周期管理

```text
# icq 查找与创建
ioc_find_get_icq(q)
  │
  ├─ [ioc 不存在] alloc_io_context() → 分配新 io_context
  │
  ├─ [ioc 存在] ioc_lookup_icq(q)
  │   │  # 先查 icq_hint 缓存，再查 radix tree
  │   │
  │   └─ [icq 不存在] ioc_create_icq(q)
  │       │  # 创建 io_cq，插入 ioc 的 radix tree 和 q 的链表
  │       │  # 调用 elevator 的 init_icq 回调
  │       │
  │       └─ radix_tree_insert(&ioc->icq_tree, q->id, icq)
  │           hlist_add_head(&icq->ioc_node, &ioc->icq_list)
  │           list_add(&icq->q_node, &q->icq_list)
  │
  └─ 返回 icq

# 释放
put_io_context(ioc)
  │
  └─ [引用计数归零] ioc_delay_free(ioc)
      │
      └─ [icq_list 非空] ioc_release_fn()  → 异步释放
          [icq_list 为空] kmem_cache_free() → 直接释放
```

#### 16.3.3 进程关联

```c
// 创建 io_context（进程首次执行 I/O 时懒分配）
current->io_context = alloc_io_context(GFP_ATOMIC, node);

// 进程退出时清理
exit_io_context(task) → ioc_exit_icqs(ioc) → put_io_context(ioc)

// 子进程共享（CLONE_IO）
__copy_io(CLONE_IO, tsk)  →  tsk->io_context = current->io_context
                            atomic_inc(&ioc->active_ref)
```

### 16.4 ioprio.c（249 行）

文件：`block/ioprio.c`

实现 `ioprio_get()` 和 `ioprio_set()` 系统调用，管理 I/O 优先级。

**优先级等级**：
| 等级 | 值 | 说明 |
|------|-----|------|
| `IOPRIO_CLASS_NONE` | 0 | 未设置（继承默认） |
| `IOPRIO_CLASS_RT` | 1 | 实时（需要 CAP_SYS_ADMIN 或 CAP_SYS_NICE） |
| `IOPRIO_CLASS_BE` | 2 | 尽力而为（默认） |
| `IOPRIO_CLASS_IDLE` | 3 | 空闲（仅在磁盘空闲时运行） |

**优先级编码**：`class << IOPRIO_CLASS_SHIFT | level`（level 0-7，0 最高）

**权限检查**（`ioprio_check_cap()`）：
- `IOPRIO_CLASS_RT`：需要 `CAP_SYS_ADMIN` 或 `CAP_SYS_NICE`
- `IOPRIO_CLASS_IDLE`：不需要特殊权限
- `IOPRIO_CLASS_NONE`：level 必须为 0

### 16.5 blk-ia-ranges.c（314 行）

文件：`block/blk-ia-ranges.c`

管理独立访问范围（Independent Access Ranges），通过 sysfs 导出每个范围的信息。

**sysfs 路径**：`/sys/block/<disk>/independent_access_ranges/`

每个范围目录包含：

| 属性 | 说明 |
|------|------|
| `sector` | 起始扇区 |
| `nr_sectors` | 扇区数 |

**核心数据结构**：
```c
struct blk_independent_access_range {
    struct kobject kobj;       // sysfs kobject
    sector_t sector;           // 起始扇区
    sector_t nr_sectors;       // 扇区数
};

struct blk_independent_access_ranges {
    struct kobject kobj;                     // sysfs kobject（父目录）
    unsigned int nr_ia_ranges;                // 范围数
    struct blk_independent_access_range ia_range[]; // 柔性数组
};
```

### 16.6 badblocks.c（1,550 行）

文件：`block/badblocks.c`

管理坏块记录，支持设置/清除坏块范围。

**记录格式**：每个坏块记录为 `(sector, count, acked)` 三元组，以有序数组存储在 `struct badblocks` 中：

```c
struct badblocks {
    seqlock_t lock;       // 顺序锁（允许读-写并发）
    int count;            // 记录数
    int shift;            // 粒度移位（2^shift 扇区为单位）
    u64 *page;            // 记录数组（每个 64 位）
    int length;           // 数组长度（页对齐）
    int changed;          // 是否已更改（sysfs 通知用）
};
```

**核心操作**：
- `badblocks_set(bb, sector, count, acked)` — 设置坏块范围
- `badblocks_clear(bb, sector, count)` — 清除坏块范围
- `badblocks_check(bb, sector, count, ...)` — 检查扇区范围是否包含坏块

**合并策略**：处理 6 种重叠情况（不相邻、S 包含 E、E 包含 S、S 在 E 前、S 在 E 后、完全覆盖），支持 acked/unacked 状态转换。

---

## Part IV: 文件系统交互

## 17. Page Cache 与 buffer_head 在块设备读写中的作用

### 17.1 概述

Page Cache 和 buffer_head 是 Linux 块设备 I/O 栈中承上启下的两个核心机制：

- **Page Cache（页缓存）**：以 `struct folio`（页面）为单位的缓存层，位于 VFS/文件系统与块层之间。所有文件数据的读写都经过页缓存，实现了"缓存命中则零拷贝返回，未命中则触发块设备 I/O"。
- **buffer_head**：以块（block）为单位的 I/O 描述符，将页缓存中的页面与磁盘块号建立映射，并通过 `submit_bh()` 将 I/O 请求转化为 bio 提交给块层。

**在 I/O 栈中的位置**：

```
用户空间    read() / write() / mmap()
   │
   ▼
VFS        generic_file_read_iter() / generic_file_write_iter()
   │
   ▼
Page Cache  filemap_read() → 查找页缓存 (address_space.i_pages)
   │         ├─ 缓存命中 → copy_to_user() (零磁盘 I/O)
   │         └─ 缓存未命中 → 缺页 → read_folio() → 块设备 I/O
   │
   ▼
buffer_head  ext4_read_folio() → block_read_full_folio()
   │         ├─ 块号映射: get_block() 将逻辑块号转为物理块号
   │         └─ submit_bh() → bio_alloc() + submit_bio()
   │
   ▼
Bio 层      submit_bio() → blk_mq_submit_bio() → 驱动
```

### 17.2 核心数据结构

#### 17.2.1 `struct address_space` — 页缓存管理器

（[fs.h](file:///home/louis/code/linux/include/linux/fs.h)）每个文件/块设备的 inode 关联一个 `address_space`，管理该文件的所有缓存页面：

```c
struct address_space {
    struct inode         *host;           /* 所属 inode（文件或块设备） */
    struct xarray         i_pages;        /* 页面索引树（key=页偏移, value=folio） */
    struct rw_semaphore   invalidate_lock;/* 缓存失效保护锁 */
    gfp_t                 gfp_mask;       /* 页面分配标志 */
    unsigned long         nrpages;        /* 缓存页面总数 */
    pgoff_t               writeback_index;/* 回写起始索引 */
    const struct address_space_operations *a_ops; /* 操作函数表（关键！） */
    unsigned long         flags;           /* AS_* 标志位 */
    errseq_t              wb_err;         /* 写回错误序列号 */
    struct list_head       i_private_list;  /* 私有链表（buffer_head 关联） */
    void                 *i_private_data; /* 私有数据 */
};
```

**`i_pages`（xarray）**：高效的基数树结构，以页面索引（folio->index）为 key，存储 `struct folio*`。支持三种 XArray 标签（mark），用于高效查找特定状态的页面：

| 标签 | 含义 |
|------|------|
| `PAGECACHE_TAG_DIRTY` | 脏页（需要回写） |
| `PAGECACHE_TAG_WRITEBACK` | 正在回写中 |
| `PAGECACHE_TAG_TOWRITE` | 标记为待写入 |

#### 17.2.2 `struct address_space_operations` — 文件系统操作接口

（[fs.h](file:///home/louis/code/linux/include/linux/fs.h)）每个文件系统实现自己的 `a_ops`，将通用的页缓存操作与文件系统特定的逻辑解耦：

```c
struct address_space_operations {
    int (*read_folio)(struct file *, struct folio *);       /* 读一页 */
    int (*writepages)(struct address_space *, struct wbc *); /* 批量回写 */
    bool (*dirty_folio)(struct address_space *, struct folio *); /* 标记脏页 */
    void (*readahead)(struct readahead_control *);           /* 预读 */
    int (*write_begin)(...);                                 /* 写开始（准备页面） */
    int (*write_end)(...);                                   /* 写结束（提交数据） */
    ssize_t (*direct_IO)(struct kiocb *, struct iov_iter *); /* 直接 I/O */
    // ...
};
```

**ext4 的实现**（[ext4/inode.c](file:///home/louis/code/linux/fs/ext4/inode.c)）：
- `read_folio` → `ext4_read_folio()` → `ext4_mpage_readpages()`
- `readahead` → `ext4_readahead()` → `ext4_mpage_readpages()`
- `writepages` → `ext4_writepages()` → `ext4_do_writepages()` → `mpage_add_bh_to_extent()` → `submit_bh()`

#### 17.2.3 `struct buffer_head` — 块 I/O 桥梁

（[buffer_head.h](file:///home/louis/code/linux/include/linux/buffer_head.h)）将一个页面内的每个块（block）与磁盘块号建立映射：

```c
struct buffer_head {
    unsigned long        b_state;          /* BH_* 状态标志位 */
    struct buffer_head  *b_this_page;      /* 同一页面内的循环链表 */
    union {
        struct page     *b_page;           /* 所属页面（已废弃） */
        struct folio    *b_folio;          /* 所属 folio（推荐使用） */
    };
    sector_t             b_blocknr;        /* 磁盘块号（物理） */
    size_t               b_size;           /* 块大小（通常 4KB） */
    char                *b_data;           /* 指向页面内数据 */
    struct block_device *b_bdev;           /* 目标块设备 */
    bh_end_io_t         *b_end_io;         /* I/O 完成回调 */
    void                *b_private;        /* 回调私有数据 */
    struct list_head      b_assoc_buffers;  /* 关联链表 */
    struct address_space *b_assoc_map;      /* 关联的 address_space */
    atomic_t              b_count;          /* 引用计数 */
    spinlock_t             b_uptodate_lock;  /* 页面内多 bh 的完成同步锁 */
};
```

**buffer_head 状态标志（`b_state`）**：

| 标志 | 含义 |
|------|------|
| `BH_Uptodate` | 缓冲区包含有效数据 |
| `BH_Dirty` | 缓冲区数据已修改，需要回写 |
| `BH_Lock` | 缓冲区已锁定（I/O 进行中） |
| `BH_Req` | 已提交 I/O 请求 |
| `BH_Mapped` | 已建立磁盘块号映射 |
| `BH_New` | 映射是新创建的（get_block 刚分配） |
| `BH_Async_Read` | 异步读取中 |
| `BH_Async_Write` | 异步写入中 |
| `BH_Delay` | 延迟分配（尚未分配磁盘块） |
| `BH_Meta` | 包含元数据 |
| `BH_Boundary` | 块后存在不连续区域 |

**一个页面内的 buffer_head 布局**：

```
folio (4KB, blocksize=1KB)
┌──────────────────────────────────────┐
│ ┌──────────┬──────────┬──────────┬──────────┐ │
│ │ bh[0]    │ bh[1]    │ bh[2]    │ bh[3]    │ │
│ │ block=100│ block=101│ block=102│ block=103│ │
│ │ offset=0 │ offset=1K│ offset=2K│ offset=3K│ │
│ │ len=1KB  │ len=1KB  │ len=1KB  │ len=1KB  │ │
│ └──────────┴──────────┴──────────┴──────────┘ │
└──────────────────────────────────────┘
  bh[0]→b_this_page→bh[1]→b_this_page→bh[2]→b_this_page→bh[3]→b_this_page→bh[0]
  (循环链表，head = folio_buffers(folio))
```

### 17.3 Page Cache 读路径

#### 17.3.1 完整调用栈

```text
# 文件读取时的 Page Cache 和 buffer_head 交互流程
#
# 数据流: 用户空间 ← copy_to_user ← 页缓存 folio ← buffer_head ← bio ← 块设备
# 关键: 缓存命中直接返回; 缓存未命中通过 buffer_head 触发块设备 I/O

read(fd, buf, count)
  │  # 用户空间系统调用
  │
  └─ ksys_read() → vfs_read()
      │  # VFS 层
      │
      └─ file->f_op->read_iter()
          │
          └─ generic_file_read_iter(iocb, iter)
              │  # mm/filemap.c
              │  # 通用文件读取迭代器
              │
              └─ filemap_read(iocb, iter, 0)
                  │  # mm/filemap.c
                  │  # 核心: 页缓存读取循环
                  │
                  └─ [do while more data]:
                      │
                      ├─ filemap_get_pages(iocb, count, &fbatch, false)
                      │   │  # mm/filemap.c
                      │   │  # 批量获取页缓存中的 folio
                      │   │
                      │   ├─ filemap_get_read_batch(mapping, index, last_index, fbatch)
                      │   │   │  # 通过 xarray 查找页缓存中已存在的 folio
                      │   │   │  # 遍历 i_pages 树, 收集 uptodate 的 folio
                      │   │   │  # 放入 fbatch 数组, 一次性最多 15 个 folio
                      │   │   │
                      │   │   ├─ [缓存命中] fbatch 中有 folio
                      │   │   │   # 页缓存已包含所需数据, 无需磁盘 I/O
                      │   │   │   # 直接跳到 copy_to_user 步骤
                      │   │   │
                      │   │   └─ [缓存未命中] fbatch 为空
                      │   │       │
                      │   │       ├─ page_cache_sync_ra(&ractl, count)
                      │   │       │   # mm/readahead.c
                      │   │       │   # 同步预读: 提前加载后续页面
                      │   │       │   # 调用 a_ops->readahead() → ext4_readahead()
                      │   │       │
                      │   │       └─ filemap_create_folio(iocb, fbatch)
                      │   │           │  # mm/filemap.c
                      │   │           │  # 分配新 folio 并加入页缓存
                      │   │           │
                      │   │           └─ filemap_read_folio(file, mapping->a_ops->read_folio)
                      │   │               │  # ★ 关键: 调用文件系统的 read_folio ★
                      │   │               │  # 触发实际的块设备 I/O
                      │   │               │
                      │   │               └─ a_ops->read_folio(file, folio)
                      │   │                   │  # 对于 ext4: ext4_read_folio()
                      │   │                   │  # fs/ext4/readpage.c
                      │   │                   │
                      │   │                   └─ ext4_mpage_readpages(inode, NULL, NULL, folio)
                      │   │                       │  # fs/ext4/readpage.c
                      │   │                       │  # 核心: 将逻辑块号映射到物理块号
                      │   │                       │  # 构造 bio, 提交给块层
                      │   │                       │  # 注意: ext4 直接构造 bio, 不经过 buffer_head!
                      │   │                       │  # (ext4 使用 mpage 机制, 绕过 buffer_head 直接提交 bio)
                      │   │                       │
                      │   │                       └─ [for each logical block in folio]:
                      │   │                           │  # 遍历 folio 范围内的每个逻辑块
                      │   │                           │
                      │   │                           ├─ ext4_map_blocks(inode, iblock, &map)
                      │   │                           │   # 逻辑块号 → 物理块号映射
                      │   │                           │   # 查询 extent tree 或 indirect block
                      │   │                           │   # 返回: map.m_pblk = 物理块号, map.m_len = 连续块数
                      │   │                           │
                      │   │                           └─ bio_add_folio_nofail(bio, folio, len, offset)
                      │   │                               # 将 folio 页面添加到 bio
                      │   │                               # 设置 bio->bi_iter.bi_sector = 物理块号 << (blkbits - 9)
                      │   │
                      │   └─ [等待 I/O 完成]
                      │       # folio_lock() 等待 folio 被标记为 uptodate
                      │       # bio 完成后: end_io → folio_mark_uptodate → folio_unlock
                      │
                      └─ copy_folio_to_iter(folio, offset, bytes, iter)
                          │  # 将 folio 中的数据拷贝到用户空间 iov_iter
                          │  # 内部调用 copy_to_user()
                          │  # ★ 数据从页缓存到用户空间, 涉及 1 次 CPU 拷贝 ★
                          │
                          └─ folio_mark_accessed(folio)
                              # 标记页面被访问, 用于 LRU 回收决策
```

#### 17.3.2 非 ext4 文件系统的 buffer_head 读路径

对于依赖 buffer_head 的传统文件系统（如 ext2、FAT、`block_read_full_folio` 路径）：

```text
# buffer_head 读路径（传统文件系统）
# 调用链: filemap_read → read_folio → block_read_full_folio → submit_bh

read_folio(file, folio)
  │
  └─ block_read_full_folio(folio, get_block)
      │  # fs/buffer.c
      │  # 为 folio 创建 buffer_head 链表, 逐个提交 I/O
      │
      ├─ folio_create_buffers(folio, inode, 0)
      │   │  # 将 folio 按 blocksize 切分为多个 buffer_head
      │   │  # 例如: 4KB folio, 1KB blocksize → 4 个 buffer_head
      │   │  # 每个 buffer_head 通过 b_this_page 形成循环链表
      │   │
      │   └─ 返回 head (第一个 buffer_head)
      │
      └─ [for each buffer_head in folio]:
          │  # 遍历 folio 的 buffer_head 循环链表
          │
          ├─ if (buffer_uptodate(bh)) → continue
          │   # 该块已包含有效数据, 跳过
          │
          ├─ if (!buffer_mapped(bh)):
          │   │  # 该块尚未建立磁盘映射
          │   │
          │   └─ get_block(inode, iblock, bh, 0)
          │       │  # ★ 文件系统回调: 逻辑块号 → 物理块号 ★
          │       │  # 文件系统负责查找块映射表
          │       │  # 成功: bh->b_blocknr = 物理块号, set_buffer_mapped(bh)
          │       │  # 失败: 超出文件末尾 → folio_zero_range() 填充零
          │       │
          │       └─ 若映射成功: set_buffer_mapped(bh)
          │
          ├─ lock_buffer(bh)
          │   # 锁定 buffer_head, 确保 I/O 互斥
          │
          ├─ mark_buffer_async_read(bh)
          │   # 设置 BH_Async_Read, 设置 bh->b_end_io = end_buffer_async_read
          │
          └─ submit_bh(REQ_OP_READ, bh)
              │  # fs/buffer.c
              │  # 将 buffer_head 转换为 bio 并提交
              │
              └─ submit_bh_wbc(REQ_OP_READ, bh, hint, NULL)
                  │  # fs/buffer.c
                  │
                  ├─ 分配 bio: bio_alloc(bh->b_bdev, 1, REQ_OP_READ, GFP_NOIO)
                  │
                  ├─ 设置扇区号: bio->bi_iter.bi_sector = bh->b_blocknr * (bh->b_size >> 9)
                  │   # 将块号转换为 512 字节扇区号
                  │   # 例如: b_blocknr=100, b_size=4096 → sector=800
                  │
                  ├─ 绑定页面: bio_add_folio_nofail(bio, bh->b_folio, bh->b_size, bh_offset(bh))
                  │   # bio->bi_io_vec->bv_page 指向 bh->b_folio 的页面
                  │   # bv_offset = bh->b_data - page_address(page)
                  │
                  ├─ 设置完成回调: bio->bi_end_io = end_bio_bh_io_sync
                  │   # I/O 完成后回调 bh->b_end_io (即 end_buffer_async_read)
                  │
                  └─ blk_crypto_submit_bio(bio)
                      # 加密处理（如需要）后提交 bio 到块层

# I/O 完成回调
end_buffer_async_read(bh, uptodate)
  │  # 若 uptodate: set_buffer_uptodate(bh)
  │  # 若 !uptodate: clear_buffer_uptodate(bh)
  │
  ├─ unlock_buffer(bh)
  │   # 释放 BH_Lock 锁, 唤醒等待者
  │
  ├─ 检查同一 folio 内所有 bh 是否都完成
  │   # 使用 head->b_uptodate_lock 自旋锁保护
  │   # 遍历 b_this_page 链表检查所有 bh 的 BH_Lock 状态
  │
  └─ if (所有 bh 都已完成):
      │
      └─ folio_end_read(folio, success)
          # 标记 folio 为 uptodate, 解锁 folio
          # 唤醒 filemap_get_pages 中等待的进程
```

### 17.4 Page Cache 写路径

#### 17.4.1 完整调用栈

```text
# 文件写入时的 Page Cache 和 buffer_head 交互流程
#
# 数据流: 用户空间 → copy_from_user → 页缓存 folio → buffer_head 标记脏 → writeback 回写
# 关键: 写入先修改页缓存(标记脏); 回写由内核线程异步完成

write(fd, buf, count)
  │  # 用户空间系统调用
  │
  └─ ksys_write() → vfs_write()
      │
      └─ file->f_op->write_iter()
          │
          └─ generic_file_write_iter(iocb, iter)
              │  # mm/filemap.c
              │
              └─ __generic_file_write_iter(iocb, iter)
                  │
                  ├─ [Direct I/O 路径]
                  │   └─ generic_file_direct_write()
                  │       # 绕过页缓存, 直接构造 bio 提交
                  │
                  └─ [Buffered I/O 路径]
                      └─ generic_perform_write(iocb, iter)
                          │  # mm/filemap.c
                          │
                          └─ [for each folio in write range]:
                              │
                              ├─ a_ops->write_begin(file, mapping, pos, len, &folio, &fsdata)
                              │   │  # 文件系统回调, 准备要写入的 folio
                              │   │  # ext4: ext4_write_begin()
                              │   │
                              │   ├─ grab_cache_folio_write_begin(mapping, index, flags)
                              │   │   # 从页缓存获取或创建 folio
                              │   │   # 如果在页缓存中已存在: 直接返回
                              │   │   # 如果不存在: 分配新 folio, 加入 i_pages xarray
                              │   │
                              │   └─ 若 folio 非 uptodate:
                              │       └─ ext4_read_folio() → ext4_mpage_readpages()
                              │           # 从磁盘读取 folio 内容（部分页写入场景）
                              │
                              ├─ copy_folio_from_iter_atomic(folio, offset, bytes, iter)
                              │   │  # ★ 数据拷贝发生在这里 ★
                              │   │  # 从用户空间 iov_iter 拷贝数据到 folio 页面
                              │   │  # 内部调用 copy_from_user()
                              │   │  # 涉及 1 次 CPU 拷贝
                              │   │
                              │   └─ folio 内容被修改, 但尚未标记为脏
                              │
                              ├─ folio_flush_mapping()  (如果必要)
                              │   # 清除 folio 的私有数据（如旧的 buffer_head）
                              │
                              └─ a_ops->write_end(file, mapping, pos, len, copied, folio, fsdata)
                                  │  # 文件系统回调, 完成写入
                                  │  # ext4: ext4_write_end()
                                  │
                                  ├─ block_write_end(file, mapping, pos, len, copied, folio, fsdata)
                                  │   │  # fs/buffer.c
                                  │   │  # 更新 buffer_head 状态
                                  │   │
                                  │   ├─ __block_commit_write(inode, folio, from, to)
                                  │   │   │  # 标记修改范围内的 buffer_head 为脏
                                  │   │   │
                                  │   │   └─ [for each buffer_head in range]:
                                  │   │       │  # 遍历被修改的 buffer_head
                                  │   │       │
                                  │   │       ├─ set_buffer_uptodate(bh)
                                  │   │       │   # 缓冲区现在包含有效数据
                                  │   │       │
                                  │   │       └─ mark_buffer_dirty(bh)
                                  │   │           │  # ★ 标记 buffer_head 为脏 ★
                                  │   │           │  # 设置 BH_Dirty 标志
                                  │   │           │
                                  │   │           └─ __set_page_dirty(folio_page(folio, 0), mapping, 0)
                                  │   │               │  # 标记 folio 为脏
                                  │   │               │  # 将 folio 加入 mapping 的脏页链表
                                  │   │               │  # 设置 PAGECACHE_TAG_DIRTY 标签
                                  │   │               │
                                  │   │               └─ 将 inode 加入 bdi_writeback 的脏页列表
                                  │   │                   # 定期回写 (writeback) 将回写脏页
                                  │   │
                                  │   └─ folio_mark_uptodate(folio)
                                  │
                                  └─ folio_unlock(folio)
                                      # 解锁 folio, 写路径完成

# ═══════════════════════════════════════════════════════════════
# 异步回写路径 (writeback)
# ═══════════════════════════════════════════════════════════════

[内核回写线程 kworker/writeback]
  │  # 周期性唤醒, 或由 balance_dirty_pages() 触发
  │
  └─ wb_workfn() → wb_do_writeback()
      │
      └─ wb_writeback() → writeback_sb_inodes()
          │
          └─ __writeback_single_inode(inode, wbc)
              │
              └─ do_writepages(mapping, wbc)
                  │
                  └─ a_ops->writepages(mapping, wbc)
                      │  # ext4: ext4_writepages()
                      │  # fs/ext4/inode.c
                      │
                      └─ mpage_prepare_extent_to_map() → mpage_submit_folio()
                          │  # ext4 同样使用 mpage 机制, 直接构造 bio
                          │  # 不经过 buffer_head 的 submit_bh()
                          │
                          └─ bio_add_folio_nofail() → blk_crypto_submit_bio()

# ═══════════════════════════════════════════════════════════════
# buffer_head 写路径（传统文件系统: __block_write_full_folio）
# ═══════════════════════════════════════════════════════════════

__block_write_full_folio(inode, folio, get_block, wbc)
  │  # fs/buffer.c
  │  # 写回一个 folio 的所有脏 buffer_head
  │
  ├─ folio_create_buffers(folio, inode, BH_Dirty|BH_Uptodate)
  │   # 确保 folio 有 buffer_head 链表
  │
  ├─ [Phase 1: 映射所有脏 buffer_head]
  │   └─ [for each buffer_head]:
  │       │
  │       ├─ if (超出文件大小) → clear_buffer_dirty(bh)
  │       │
  │       └─ if (!buffer_mapped(bh) && buffer_dirty(bh)):
  │           └─ get_block(inode, block, bh, 1)
  │               # 分配磁盘块（如果需要）并返回物理块号
  │               # 例如: ext2_get_block() → ext2_alloc_branch()
  │
  ├─ [Phase 2: 锁定并标记异步写入]
  │   └─ [for each buffer_head]:
  │       │
  │       ├─ lock_buffer(bh)
  │       │   # 锁定 buffer_head, 防止并发修改
  │       │
  │       └─ if (test_clear_buffer_dirty(bh)):
  │           └─ mark_buffer_async_write_endio(bh, end_buffer_async_write)
  │               # 设置 BH_Async_Write, bh->b_end_io = end_buffer_async_write
  │
  ├─ folio_start_writeback(folio)
  │   # 标记 folio 为 writeback 状态
  │
  └─ [Phase 3: 提交所有 buffer_head]
      └─ [for each buffer_head]:
          └─ if (buffer_async_write(bh)):
              └─ submit_bh_wbc(REQ_OP_WRITE | write_flags, bh, hint, wbc)
                  │  # 每个 buffer_head 转为一个 bio 提交
                  │  # 注意: 这是一个 O(n) 的 bio 提交, 效率较低
                  │  # 这也是 ext4 转向 mpage 机制的原因之一
                  │
                  └─ ... (bio 提交, 与读路径相同)
```

### 17.5 buffer_head 到 bio 的转换

```text
# submit_bh_wbc: buffer_head → bio 的完整转换流程
# 位置: fs/buffer.c

submit_bh_wbc(REQ_OP_READ, bh, hint, wbc)
  │
  ├─ 状态检查
  │   # BUG_ON(!buffer_locked(bh))      ← 必须已锁定
  │   # BUG_ON(!buffer_mapped(bh))      ← 必须有磁盘映射
  │   # BUG_ON(!bh->b_end_io)           ← 必须有完成回调
  │   # BUG_ON(buffer_delay(bh))        ← 不能是延迟分配
  │
  ├─ 设置操作标志
  │   # if (buffer_meta(bh)) → opf |= REQ_META    ← 元数据 I/O 标记
  │   # if (buffer_prio(bh)) → opf |= REQ_PRIO    ← 优先级 I/O 标记
  │
  ├─ bio = bio_alloc(bh->b_bdev, 1, opf, GFP_NOIO)
  │   # 分配 bio, 最多 1 个 bio_vec
  │   # 一个 buffer_head 对应一个 bio
  │   # 注意: 每个 bh 提交一个 bio, 效率较低
  │   #  ext4 的 mpage 机制将多个连续块合并到一个 bio
  │
  ├─ 设置扇区号
  │   # bio->bi_iter.bi_sector = bh->b_blocknr * (bh->b_size >> 9)
  │   # 例如: b_blocknr=500, b_size=4096
  │   #   → bi_sector = 500 * 8 = 4000
  │   # 块号乘以 (块大小/512) 转换为 512 字节扇区号
  │
  ├─ 绑定页面到 bio
  │   # bio_add_folio_nofail(bio, bh->b_folio, bh->b_size, bh_offset(bh))
  │   # 设置 bio->bi_io_vec[0]:
  │   #   bv_page   = folio_page(bh->b_folio, 0)  ← 页面指针
  │   #   bv_offset = bh_offset(bh)               ← 页内偏移
  │   #   bv_len    = bh->b_size                  ← 块大小
  │
  ├─ 设置完成回调
  │   # bio->bi_end_io = end_bio_bh_io_sync
  │   # bio->bi_private = bh
  │   # I/O 完成后: end_bio_bh_io_sync(bio) → bh->b_end_io(bh, uptodate)
  │
  ├─ guard_bio_eod(bio)
  │   # 检查 bio 是否超出设备边界, 如有需要则截断
  │
  ├─ [如果 wbc 非 NULL]
  │   # wbc_init_bio(wbc, bio)           ← 关联 cgroup 写回控制
  │   # wbc_account_cgroup_owner(wbc, ...) ← 统计 cgroup 写回量
  │
  └─ blk_crypto_submit_bio(bio)
      # 内联加密处理（如需要）后提交到块层
      # 最终调用 submit_bio_noacct() → blk_mq_submit_bio()
```

### 17.6 块设备自身的 Page Cache（bdev mapping）

块设备也有自己的 `address_space`（`bdev->bd_mapping`），用于缓存块设备级别的元数据（如 superblock、bitmap）以及通过 `dd` 等直接访问时的数据：

```text
# 块设备 address_space 的初始化
# block/bdev.c

bdev_alloc_inode(sb)
  │
  ├─ inode = alloc_inode_sb(sb, bdev_inode_cachep, GFP_KERNEL)
  │
  └─ inode->i_data.a_ops = &def_blk_aops
      # 设置块设备的 a_ops
      # def_blk_aops 定义在 block/fops.c 中
      # 包含 read_folio, writepages 等操作
```

**块设备与文件系统 address_space 的区别**：

| 类型 | 所属 | 用途 | a_ops |
|------|------|------|-------|
| 文件 address_space | `inode->i_mapping` | 缓存文件数据 | ext4_aops / ext2_aops 等 |
| 块设备 address_space | `bdev->bd_mapping` | 缓存块设备 raw 数据 | def_blk_aops |

**`__find_get_block_slow()`**（[buffer.c](file:///home/louis/code/linux/fs/buffer.c)）在块设备 address_space 中查找特定块号的 buffer_head：

```c
// 通过 bdev->bd_mapping->i_pages 查找块号对应的 folio
// 然后在 folio 的 buffer_head 链表中查找匹配的 bh
// 如果找到 → 增加引用计数返回
// 如果未找到 → 返回 NULL
```

### 17.7 关键数据流对比

```text
# ═══════════════════════════════════════════════════════════════
# 路径 A: ext4 mpage 读（绕过 buffer_head, 直接 bio）
# ═══════════════════════════════════════════════════════════════

filemap_read()
  └─ filemap_get_pages() → 页缓存未命中
      └─ filemap_read_folio() → ext4_read_folio()
          └─ ext4_mpage_readpages()
              ├─ ext4_map_blocks()  ← 逻辑块→物理块映射
              └─ bio_add_folio_nofail() + submit_bio()
                  # ★ 一个 bio 可包含多个连续块, 高效 ★

# ═══════════════════════════════════════════════════════════════
# 路径 B: buffer_head 读（传统文件系统, 每个 bh 一个 bio）
# ═══════════════════════════════════════════════════════════════

filemap_read()
  └─ filemap_get_pages() → 页缓存未命中
      └─ filemap_read_folio() → block_read_full_folio()
          ├─ folio_create_buffers()  ← 切分 folio 为 buffer_head 链表
          ├─ get_block() × N         ← 每个 bh 获取块映射
          └─ submit_bh() × N         ← 每个 bh 提交一个 bio
              # ★ 每个 bh 一个 bio, 碎片化, 效率较低 ★

# ═══════════════════════════════════════════════════════════════
# 路径 C: 直接 I/O（绕过 Page Cache）
# ═══════════════════════════════════════════════════════════════

generic_file_direct_write()
  └─ a_ops->direct_IO()
      └─ 直接构造 bio, 数据从用户页面直接到块设备
          # ★ 完全不经过页缓存和 buffer_head ★
```

### 17.8 总结

**Page Cache 的核心作用**：
1. **缓存层**：通过 `address_space.i_pages`（xarray）管理文件的所有缓存页面，命中时避免磁盘 I/O
2. **统一接口**：通过 `address_space_operations` 将文件系统与页缓存解耦，不同的文件系统只需实现自己的 `a_ops`
3. **回写管理**：通过 `PAGECACHE_TAG_DIRTY` 标签追踪脏页，由 writeback 机制异步回写
4. **预读优化**：通过 `readahead` 机制提前加载后续页面，减少 I/O 等待

**buffer_head 的核心作用**：
1. **块映射**：将页面内的每个块（block）与磁盘物理块号建立映射（`b_blocknr`）
2. **状态追踪**：通过 `BH_Uptodate`、`BH_Dirty`、`BH_Lock` 等标志管理每个块的状态
3. **I/O 提交**：通过 `submit_bh()` 将 buffer_head 转换为 bio 提交给块层
4. **向后兼容**：为传统文件系统（ext2、FAT 等）提供块 I/O 接口

**ext4 的优化**：ext4 使用 `mpage` 机制，在 `ext4_mpage_readpages()` 中直接构造 bio，将多个连续块合并到一个 bio 中，绕过了 buffer_head 的 `submit_bh()` 路径，显著提高了 I/O 效率。

---

## 18. Writeback 回写机制 — Flush 线程下刷数据流程

### 18.1 概述

Buffer Write 场景下，用户态 `write()` 系统调用仅将数据从用户空间拷贝到 Page Cache 并标记脏页，**实际的磁盘 I/O 由 Flush 线程（Writeback Flusher Thread）异步完成**。Flush 线程是内核中负责将脏页写回磁盘的核心机制，本节分析其完整的工作流程。

**Flush 线程在 I/O 栈中的位置**：

```
用户空间 write(fd, buf, count)
  │
  ▼
VFS / Page Cache 层
  │  └─ 数据写入页缓存，标记脏页
  │  └─ balance_dirty_pages() 限流控制
  │
  ▼
Flush 线程 (wb_workfn)            ← 本节分析的核心
  │  └─ wb_do_writeback()
  │      └─ wb_writeback()
  │          └─ writeback_sb_inodes()
  │              └─ __writeback_single_inode()
  │                  └─ do_writepages()
  │                      └─ a_ops->writepages()  ← 文件系统回调
  │
  ▼
Buffer Head 层 / Bio 层
  │  └─ submit_bh() / mpage_writepages()
  │      └─ submit_bio()
  │
  ▼
块层 (blk-mq)
  └─ blk_mq_submit_bio() → ... → 驱动
```

**涉及的核心文件**：

| 文件 | 行数 | 功能 |
|------|------|------|
| `fs/fs-writeback.c` | 2,994 | Flush 线程主实现：work 管理、inode 回写 |
| `mm/page-writeback.c` | 2,500+ | 脏页比率控制、`do_writepages()`、`balance_dirty_pages()` |
| `fs/buffer.c` | 3,500+ | `submit_bh()` — buffer_head 转 bio |
| `fs/mpage.c` | 500+ | `mpage_writepages()` — 批量回写 |
| `include/linux/writeback.h` | 378 | `struct writeback_control`、`struct wb_domain` |
| `include/linux/backing-dev-defs.h` | 280+ | `struct bdi_writeback`、`struct backing_dev_info` |

### 18.2 核心数据结构

#### 18.2.1 `struct bdi_writeback` — 回写设备管理器

（[backing-dev-defs.h](file:///home/louis/code/linux/include/linux/backing-dev-defs.h)）每个块设备（`backing_dev_info`）关联一个 `bdi_writeback`，管理该设备的所有回写操作：

```c
struct bdi_writeback {
    struct backing_dev_info *bdi;       /* 所属 backing device */

    unsigned long state;                 /* WB_* 状态位（原子位操作） */
    unsigned long last_old_flush;        /* 上次老旧数据刷新时间 */

    /* Inode 链表 — 按脏页状态分类 */
    struct list_head b_dirty;            /* 有脏页的 inode（待回写） */
    struct list_head b_io;               /* 正在回写的 inode */
    struct list_head b_more_io;          /* 回写未完成的 inode（需要更多 I/O） */
    struct list_head b_dirty_time;       /* 时间戳脏的 inode */
    spinlock_t list_lock;                /* 保护 b_* 链表 */

    atomic_t writeback_inodes;           /* 正在回写的 inode 数 */
    struct percpu_counter stat[NR_WB_STAT_ITEMS]; /* 统计：WB_RECLAIMABLE/WB_WRITEBACK 等 */

    /* 带宽估算 */
    unsigned long bw_time_stamp;         /* 上次带宽更新的时间戳 */
    unsigned long dirtied_stamp;
    unsigned long written_stamp;
    unsigned long write_bandwidth;       /* 估算的写带宽 */
    unsigned long avg_write_bandwidth;   /* 平滑后的写带宽 */
    unsigned long dirty_ratelimit;       /* 脏页速率限制 */
    unsigned long balanced_dirty_ratelimit; /* 平衡脏页速率 */

    struct fprop_local_percpu completions; /* 写完成比例统计 */
    int dirty_exceeded;                  /* 是否超过脏页阈值 */
    enum wb_reason start_all_reason;     /* 全量回写的触发原因 */

    /* Work 管理 */
    spinlock_t work_lock;                /* 保护 work_list 和 dwork 调度 */
    struct list_head work_list;          /* 待处理的回写工作链表 */
    struct delayed_work dwork;           /* 延迟工作项（Flush 线程入口） */
    struct delayed_work bw_dwork;        /* 带宽估算更新工作项 */
};
```

**`b_dirty` / `b_io` / `b_more_io` 三链表的作用**：

```
   b_dirty                     b_io                      b_more_io
  ┌──────────┐             ┌──────────┐              ┌──────────┐
  │ inode_A  │  queue_io   │ inode_A  │  writeback   │ inode_A  │
  │ inode_B  │  ───────→   │ inode_B  │  ───────→    │ inode_B  │ ← 部分回写完成
  │ inode_C  │             │ inode_C  │              │          │   但仍有脏页
  └──────────┘             └──────────┘              └──────────┘
    脏页 inode               正在/即将回写               需要更多 I/O
```

- **`b_dirty`**：所有脏 inode 的候诊队列，等待被调度
- **`b_io`**：当前批次正在回写的 inode，`queue_io()` 从 `b_dirty` 搬入
- **`b_more_io`**：某 inode 回写完一批后仍有脏页，搬入此链表继续下一轮

**`wb->state` 状态位**：

| 标志 | 含义 |
|------|------|
| `WB_registered` | `bdi_register()` 已完成 |
| `WB_writeback_running` | 正在执行回写 |
| `WB_has_dirty_io` | `b_{dirty\|io\|more_io}` 上有脏 inode |
| `WB_start_all` | 全量回写请求已发出（防止重复入队） |

#### 18.2.2 `struct wb_writeback_work` — 回写工作项

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）表示一个具体的回写任务，由 Flush 线程从 `wb->work_list` 取出执行：

```c
struct wb_writeback_work {
    long nr_pages;                       /* 需要回写的页数 */
    struct super_block *sb;              /* 目标超级块（NULL=所有） */
    enum writeback_sync_modes sync_mode; /* WB_SYNC_NONE / WB_SYNC_ALL */
    unsigned int tagged_writepages:1;    /* 使用 tag-and-write 避免活锁 */
    unsigned int for_kupdate:1;          /* 周期性老旧数据回写 */
    unsigned int range_cyclic:1;         /* 循环扫描范围 */
    unsigned int for_background:1;       /* 后台回写（超过 bg_thresh） */
    unsigned int for_sync:1;             /* sync(2) WB_SYNC_ALL 回写 */
    unsigned int auto_free:1;            /* 完成后自动 kfree */
    enum wb_reason reason;               /* 触发原因 */

    struct list_head list;               /* 链接到 wb->work_list */
    struct wb_completion *done;          /* 调用者等待完成 */
};
```

**`sync_mode` 的区别**：

| 模式 | 含义 | 行为 |
|------|------|------|
| `WB_SYNC_NONE` | 异步回写 | 不等待 I/O 完成，尽可能多写页面 |
| `WB_SYNC_ALL` | 同步回写 | 等待每次 I/O 完成，`sync()` 系统调用使用 |

**`reason` 枚举**（触发原因）：

| 原因 | 触发场景 |
|------|---------|
| `WB_REASON_BACKGROUND` | `balance_dirty_pages()` 检测到超过后台阈值 |
| `WB_REASON_VMSCAN` | 内存回收需要释放脏页 |
| `WB_REASON_SYNC` | `sync()` / `fsync()` 系统调用 |
| `WB_REASON_PERIODIC` | 定时器到期（`dirty_writeback_interval`） |
| `WB_REASON_FS_FREE_SPACE` | 文件系统剩余空间不足 |
| `WB_REASON_FOREIGN_FLUSH` | Cgroup 写回中检测到外来源 inode |

#### 18.2.3 `struct writeback_control` — 回写控制参数

（[writeback.h](file:///home/louis/code/linux/include/linux/writeback.h)）传递给文件系统 `->writepages()` 回调的控制参数，通常分配在栈上：

```c
struct writeback_control {
    long nr_to_write;                    /* 还需写多少页 */
    long pages_skipped;                  /* 跳过的页数 */
    loff_t range_start;                  /* 写回范围起始偏移 */
    loff_t range_end;                    /* 写回范围结束偏移（包含） */
    enum writeback_sync_modes sync_mode; /* WB_SYNC_NONE / WB_SYNC_ALL */

    unsigned int for_kupdate:1;          /* 周期性回写 */
    unsigned int for_background:1;       /* 后台回写 */
    unsigned int tagged_writepages:1;    /* tag-and-write 模式 */
    unsigned int range_cyclic:1;         /* 循环扫描 */
    unsigned int for_sync:1;             /* sync(2) 同步回写 */
    unsigned int unpinned_netfs_wb:1;    /* 已清除 I_PINNING_NETFS_WB */

    /* 内部字段 */
    struct folio_batch fbatch;           /* folio 批量处理 */
    pgoff_t index;                       /* 当前扫描索引 */
    int saved_err;                       /* 保存的错误码 */

#ifdef CONFIG_CGROUP_WRITEBACK
    struct bdi_writeback *wb;            /* 当前回写所属的 wb */
    struct inode *inode;                 /* 正在回写的 inode */
    /* 外来源 inode 检测 */
    int wb_id;                           /* 当前 wb id */
    int wb_lcand_id;                     /* 上一个候选 wb id */
    int wb_tcand_id;                     /* 当前候选 wb id */
    size_t wb_bytes;                     /* 当前 wb 写入字节数 */
    size_t wb_lcand_bytes;
    size_t wb_tcand_bytes;
#endif
};
```

**`wbc_to_write_flags()`** 将 `sync_mode` 映射为 bio 的 `REQ_*` 标志：

```c
static inline blk_opf_t wbc_to_write_flags(struct writeback_control *wbc)
{
    blk_opf_t flags = 0;
    if (wbc->sync_mode == WB_SYNC_ALL)
        flags |= REQ_SYNC;                  /* 同步写：高优先级 */
    else if (wbc->for_kupdate || wbc->for_background)
        flags |= REQ_BACKGROUND;            /* 后台写：低优先级 */
    return flags;
}
```

#### 18.2.4 `struct backing_dev_info` — 后备设备信息

（[backing-dev-defs.h](file:///home/louis/code/linux/include/linux/backing-dev-defs.h)）每个块设备对应一个 `backing_dev_info`，描述其回写能力：

```c
struct backing_dev_info {
    u64 id;                                /* 唯一标识符 */
    struct rb_node rb_node;                /* 按 id 排序的红黑树节点 */
    struct list_head bdi_list;             /* 全局 bdi 链表 */
    unsigned long ra_pages;                /* 最大预读页数 */
    unsigned long io_pages;                /* 最大 I/O 页数 */
    struct kref refcnt;
    unsigned int capabilities;             /* BDI_CAP_* 能力标志 */
    unsigned int min_ratio;                /* 最小脏页比率 */
    unsigned int max_ratio;                /* 最大脏页比率 */
    atomic_long_t tot_write_bandwidth;     /* 所有 wb 的写带宽总和 */
    unsigned long last_bdp_sleep;          /* 上次限流睡眠时间 */
    struct bdi_writeback wb;              /* 根 wb（内嵌） */
    struct list_head wb_list;              /* 所有 wb 链表（含 cgroup wb） */
    wait_queue_head_t wb_waitq;            /* 等待队列 */
};
```

**BDI_CAP 能力标志**：

| 标志 | 含义 |
|------|------|
| `BDI_CAP_WRITEBACK` | 支持回写（大多数块设备） |
| `BDI_CAP_WRITEBACK_ACCT` | 支持回写统计 |
| `BDI_CAP_STRICTLIMIT` | 严格限制（不借用其他 BDI 的额度） |

### 18.3 Flush 线程的创建与初始化

#### 18.3.1 创建时机

Flush 线程在设备注册时创建，通过 `bdi_alloc()` 初始化 `bdi_writeback`，然后通过 `bdi_register()` 将 `wb->dwork` 绑定到全局 workqueue `bdi_wq`。

**调用链**：

```
设备驱动注册块设备
  └─ device_add_disk()
      └─ blk_register_queue()
          └─ bdi_register()                   ← 注册 backing device
              └─ bdi_register_va()
                  └─ wb_init(wb, bdi, GFP_KERNEL)  ← 初始化 bdi_writeback
                      │
                      ├─ spin_lock_init(&wb->work_lock)
                      ├─ INIT_LIST_HEAD(&wb->work_list)
                      ├─ INIT_DELAYED_WORK(&wb->dwork, wb_workfn)  ← 绑定工作函数
                      ├─ INIT_DELAYED_WORK(&wb->bw_dwork, wb_update_bandwidth_workfn)
                      ├─ percpu_counter_init()  ← 初始化统计计数器
                      │
                      └─ fprop_local_init_percpu()  ← 初始化比例统计
```

**关键点**：Flush 线程不是一个独立的 `kthread`，而是**通过工作队列（workqueue）** 实现的延迟工作项 `wb->dwork`。每次有回写需求时，通过 `mod_delayed_work(bdi_wq, &wb->dwork, delay)` 调度执行。

#### 18.3.2 全局 workqueue `bdi_wq`

`bdi_wq` 在系统初始化时创建，所有块设备的 Flush 线程共享此工作队列：

```c
// mm/backing-dev.c
static int __init default_bdi_init(void)
{
    bdi_wq = alloc_workqueue("writeback", WQ_MEM_RECLAIM | WQ_FREEZABLE | WQ_SYSFS,
                             WQ_UNBOUND_MAX_ACTIVE);
    // WQ_MEM_RECLAIM: 内存回收路径可用（避免死锁）
    // WQ_FREEZABLE: 系统休眠时冻结
    // WQ_UNBOUND: 不限 CPU，可迁移
    // WQ_UNBOUND_MAX_ACTIVE: 最大活跃工作数 = CPU 数
    ...
}
```

### 18.4 Work 入队流程

#### 18.4.1 `wb_queue_work()` — 入队回写工作

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）

```c
static void wb_queue_work(struct bdi_writeback *wb,
                          struct wb_writeback_work *work)
{
    trace_writeback_queue(wb, work);

    if (work->done)
        atomic_inc(&work->done->cnt);       /* 增加完成计数器 */

    spin_lock_irq(&wb->work_lock);

    if (test_bit(WB_registered, &wb->state)) {
        list_add_tail(&work->list, &wb->work_list);  /* 加入工作链表尾部 */
        mod_delayed_work(bdi_wq, &wb->dwork, 0);      /* 立即调度 Flush 线程 */
    } else
        finish_writeback_work(work);                   /* 设备已注销，直接完成 */

    spin_unlock_irq(&wb->work_lock);
}
```

**流程**：
1. 将 `work` 加入 `wb->work_list` 链表尾部（FIFO 顺序）
2. 调用 `mod_delayed_work(bdi_wq, &wb->dwork, 0)` 立即唤醒 Flush 线程
3. 如果设备已注销，直接调用 `finish_writeback_work()` 完成工作

#### 18.4.2 触发回写的 5 种路径

**① 后台回写（Background Writeback）** — 通过 `balance_dirty_pages()` 触发：

```
write() → generic_perform_write()
  → 写入页缓存，标记脏页
  → balance_dirty_pages_ratelimited(mapping)  ← 速率限制的脏页检查
    → balance_dirty_pages(bdi, dirty_thresh)
      → 如果 wb_dirty > wb_thresh（超过脏页阈值）
        → wb_start_background_writeback(wb)  ← 唤醒 Flush 线程
          → wb_wakeup(wb)
            → mod_delayed_work(bdi_wq, &wb->dwork, 0)
```

**② 周期性回写（Periodic / Kupdate）** — 定时器触发：

```c
// 默认 dirty_writeback_interval = 500 厘秒 = 5 秒
// wb_workfn() 执行完毕后：
if (!list_empty(&wb->work_list))
    wb_wakeup(wb);                              /* 还有工作，立即唤醒 */
else if (wb_has_dirty_io(wb) && dirty_writeback_interval)
    wb_wakeup_delayed(wb);                      /* 5 秒后再次检查 */
    // → queue_delayed_work(bdi_wq, &wb->dwork, timeout)
```

**③ 同步回写（Sync / Fsync）** — 直接入队并等待完成：

```c
sync_inodes_sb(sb)
  → 遍历所有 BDI，对每个 wb：
    DEFINE_WB_COMPLETION(done, bdi);           /* 定义完成标记 */
    struct wb_writeback_work work = {
        .sb       = sb,
        .sync_mode = WB_SYNC_ALL,
        .nr_pages  = LONG_MAX,
        .for_sync  = 1,
        .done      = &done,                    /* 设置完成通知 */
    };
    wb_queue_work(wb, &work);                  /* 入队 */
    wb_wait_for_completion(&done);             /* 等待回写完成 */
```

**④ 内存回收触发（Vmscan）** — 页面回收需要先写回脏页：

```
shrink_folio_list() 或 shrink_inactive_list()
  → pageout(folio, mapping)                   /* 尝试换出脏页 */
    → mapping->a_ops->writepages(mapping, wbc) /* 直接触发回写 */
    // 或
  → wakeup_flusher_threads(WB_REASON_VMSCAN)  /* 异步唤醒 Flush 线程 */
    → wb_start_writeback(wb, reason)
      → wb_wakeup(wb)
```

**⑤ 文件系统主动触发** — 空间不足等情况：

```c
writeback_inodes_sb(sb, WB_REASON_FS_FREE_SPACE);
writeback_inodes_sb_nr(sb, nr, WB_REASON_FS_FREE_SPACE);
```

### 18.5 Flush 线程主循环

#### 18.5.1 `wb_workfn()` — Flush 线程入口

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）这是 Flush 线程的核心工作函数，每次 `wb->dwork` 被调度时执行：

```c
void wb_workfn(struct work_struct *work)
{
    struct bdi_writeback *wb = container_of(to_delayed_work(work),
                                            struct bdi_writeback, dwork);
    long pages_written;

    set_worker_desc("flush-%s", bdi_dev_name(wb->bdi));

    if (likely(!current_is_workqueue_rescuer() ||
               !test_bit(WB_registered, &wb->state))) {
        /* 正常路径：持续回写直到 work_list 为空 */
        do {
            pages_written = wb_do_writeback(wb);
            trace_writeback_pages_written(pages_written);
        } while (!list_empty(&wb->work_list));
    } else {
        /* 紧急救援路径：workqueue 工作线程不足，不能独占总资源 */
        pages_written = writeback_inodes_wb(wb, 1024,
                                            WB_REASON_FORKER_THREAD);
        trace_writeback_pages_written(pages_written);
    }

    /* 结束后检查是否需要再次调度 */
    if (!list_empty(&wb->work_list))
        wb_wakeup(wb);                                   /* 还有 work，立即唤醒 */
    else if (wb_has_dirty_io(wb) && dirty_writeback_interval)
        wb_wakeup_delayed(wb);                           /* 有脏页，延迟唤醒 */
}
```

**关键逻辑**：
1. 循环调用 `wb_do_writeback()` 处理 `work_list` 中所有 work 项
2. 如果是在 rescue 模式（workqueue 线程不足），只写 1024 页就退出，避免饿死其他 work 项
3. 结束后根据是否有残留 work 或脏页，决定是否重新调度

#### 18.5.2 `wb_do_writeback()` — 分发回写任务

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）

```c
static long wb_do_writeback(struct bdi_writeback *wb)
{
    struct wb_writeback_work *work;
    long wrote = 0;

    set_bit(WB_writeback_running, &wb->state);           /* 标记回写进行中 */

    /* 1. 处理 work_list 中的显式 work 项 */
    while ((work = get_next_work_item(wb)) != NULL) {
        trace_writeback_exec(wb, work);
        wrote += wb_writeback(wb, work);                 /* 执行回写 */
        finish_writeback_work(work);                     /* 完成通知 */
    }

    /* 2. 检查全量回写请求 */
    wrote += wb_check_start_all(wb);

    /* 3. 检查周期性老旧数据回写 */
    wrote += wb_check_old_data_flush(wb);

    /* 4. 检查后台回写（超过 bg_thresh） */
    wrote += wb_check_background_flush(wb);

    clear_bit(WB_writeback_running, &wb->state);
    return wrote;
}
```

**执行顺序**：
1. **显式 work**（通过 `wb_queue_work()` 入队的）优先级最高
2. **全量回写**（`wb_start_writeback()` 标记 `WB_start_all` 的）
3. **周期性回写**（`wb_check_old_data_flush()` — 检查老旧数据）
4. **后台回写**（`wb_check_background_flush()` — 检查是否超过后台阈值）

#### 18.5.3 `wb_writeback()` — 核心回写循环

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）

```c
static long wb_writeback(struct bdi_writeback *wb,
                         struct wb_writeback_work *work)
{
    long nr_pages = work->nr_pages;
    unsigned long dirtied_before = jiffies;
    struct blk_plug plug;
    bool queued = false;

    blk_start_plug(&plug);                               /* 开启批量提交优化 */

    for (;;) {
        if (work->nr_pages <= 0)                         /* 页数已写够 */
            break;

        /* 后台/周期性回写遇到其他 work，让出 CPU */
        if ((work->for_background || work->for_kupdate) &&
            !list_empty(&wb->work_list))
            break;

        /* 后台回写：低于阈值即停止 */
        if (work->for_background && !wb_over_bg_thresh(wb))
            break;

        spin_lock(&wb->list_lock);
        trace_writeback_start(wb, work);

        /* 如果 b_io 为空，从 b_dirty 搬入 */
        if (list_empty(&wb->b_io)) {
            if (work->for_kupdate)
                dirtied_before = jiffies -
                    msecs_to_jiffies(dirty_expire_interval * 10);
            else if (work->for_background)
                dirtied_before = jiffies;
            queue_io(wb, work, dirtied_before);          /* b_dirty → b_io */
            queued = true;
        }

        /* 回写 b_io 中的 inode */
        if (work->sb)
            progress = writeback_sb_inodes(work->sb, wb, work);
        else
            progress = __writeback_inodes_wb(wb, work);

        trace_writeback_written(wb, work);

        if (progress || !queued) {
            spin_unlock(&wb->list_lock);
            continue;
        }

        if (list_empty(&wb->b_more_io)) {                /* 没有更多 I/O 了 */
            spin_unlock(&wb->list_lock);
            break;
        }

        /* 写了 0 进度，等待 inode 完成 */
        trace_writeback_wait(wb, work);
        inode = wb_inode(wb->b_more_io.prev);
        spin_lock(&inode->i_lock);
        spin_unlock(&wb->list_lock);
        inode_sleep_on_writeback(inode);                 /* 睡眠等待 I/O 完成 */
    }

    blk_finish_plug(&plug);                              /* 刷新 plug，批量提交 */
    return nr_pages - work->nr_pages;
}
```

**核心循环逻辑**：

```
                  ┌──────────────────────────────┐
                  │  开始回写循环                   │
                  └──────────────┬───────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  work->nr_pages <= 0?   │──→ 退出（已写完目标页数）
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  有其他 work 等待?       │──→ 退出（让出 CPU）
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  后台回写低于阈值?        │──→ 退出（已完成任务）
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  b_io 为空?             │──→ queue_io() 从 b_dirty 搬入
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  writeback_sb_inodes()   │  ← 实际回写 inode 的脏页
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  有进度 or 首次入队?      │──→ 继续循环
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  b_more_io 为空?         │──→ 退出
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  inode_sleep_on_writeback│  ← 等待 I/O 完成
                   └────────────┬────────────┘
                                │
                                └──→ 继续循环
```

### 18.6 Inode 回写路径

#### 18.6.1 `writeback_sb_inodes()` — 遍历 inode 链表

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）遍历 `wb->b_io` 链表，对每个 inode 调用 `__writeback_single_inode()`：

```c
static long writeback_sb_inodes(struct super_block *sb,
                                struct bdi_writeback *wb,
                                struct wb_writeback_work *work)
{
    while (!list_empty(&wb->b_io)) {
        struct inode *inode = wb_inode(wb->b_io.prev);
        struct super_block *inode_sb = inode->i_sb;

        if (inode_sb != sb)              /* 跳过不属于目标 super_block 的 inode */
            redirty_tail(inode, wb);
            continue;

        if (!sb_is_blkdev_sb(inode_sb)) {
            if (!super_trylock_shared(inode_sb)) {  /* 尝试获取 s_umount 读锁 */
                redirty_tail(inode, wb);            /* 获取失败，重试 */
                continue;
            }
        }

        /* 提取 inode 并调用 __writeback_single_inode */
        __writeback_single_inode(inode, &wbc);

        /* 进度检查：每 100ms 或写够后退出循环 */
        if (wrote) {
            if (time_is_before_jiffies(start_time + HZ / 10UL))
                break;
            if (work->nr_pages <= 0)
                break;
        }
    }
    return wrote;
}
```

**关键点**：
- 使用 `super_trylock_shared()` 获取 `s_umount` 读锁（非阻塞），避免与 `umount` 竞争
- 每 100ms 或写够目标页数后退出，让出 CPU
- 如果 `inode_sb != work->sb`，将 inode 重新放回 `b_dirty` 尾部

#### 18.6.2 `__writeback_single_inode()` — 单个 inode 回写

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）对单个 inode 的脏页和元数据执行回写：

```c
static int __writeback_single_inode(struct inode *inode,
                                    struct writeback_control *wbc)
{
    struct address_space *mapping = inode->i_mapping;
    long nr_to_write;
    int ret;

    spin_lock(&inode->i_lock);

    if (!(inode_state_read(inode) & I_DIRTY_ALL))    /* 没有脏数据 */
        goto out_unlock_inode;

    if (inode_state_read(inode) & I_SYNC) {           /* 已有回写在进行 */
        if (wbc->sync_mode != WB_SYNC_ALL)
            goto out_unlock_inode;                    /* 异步模式：跳过 */
        inode_wait_for_writeback(inode);              /* 同步模式：等待 */
    }

    inode->i_state |= I_SYNC;                         /* 标记 I_SYNC 防止并发 */
    spin_unlock(&inode->i_lock);

    write_chunk = writeback_chunk_size(inode->i_sb, wb, work);
    wbc->nr_to_write = write_chunk;

    /*
     * 1. 回写脏页（数据）
     */
    ret = do_writepages(mapping, wbc);                /* ★ 核心：写回脏页 ★ */

    /*
     * 2. 回写 inode 元数据（时间戳、大小等）
     */
    if (need_resched()) {
        blk_flush_plug(current->plug, false);
        cond_resched();
    }
    write_inode(inode, wbc);                          /* 调用 s_op->write_inode() */

    /*
     * 3. 更新 inode 状态
     */
    spin_lock(&inode->i_lock);
    inode->i_state &= ~I_SYNC;                        /* 清除 I_SYNC */

    if (!(inode_state_read(inode) & I_DIRTY_ALL)) {
        /* 完全干净：从回写链表移除 */
        list_del_init(&inode->i_io_list);
        inode->i_state &= ~I_DIRTY_ALL;
        ...
    } else {
        /* 仍有脏页：搬入 b_more_io 等待下一轮 */
        requeue_inode(inode, wb, wbc, dirtied_before);
    }
    ...
}
```

**回写顺序**：
1. **先写数据**（`do_writepages` → 文件系统 `->writepages`）
2. **再写元数据**（`write_inode` → `s_op->write_inode`）
3. **更新状态**（清除 `I_SYNC`，如果仍有脏页则搬入 `b_more_io`）

#### 18.6.3 `do_writepages()` — 写回脏页到块层

（[mm/page-writeback.c](file:///home/louis/code/linux/mm/page-writeback.c)）调用文件系统注册的 `->writepages` 回调，将脏页批量写入块层：

```c
int do_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
    int ret;
    struct bdi_writeback *wb;

    if (wbc->nr_to_write <= 0)
        return 0;

    wb = inode_to_wb_wbc(mapping->host, wbc);
    wb_bandwidth_estimate_start(wb);

    while (1) {
        if (mapping->a_ops->writepages)
            ret = mapping->a_ops->writepages(mapping, wbc);  /* 文件系统回调 */
        else
            ret = 0;                                         /* 无回调，跳过 */

        if (ret != -ENOMEM || wbc->sync_mode != WB_SYNC_ALL)
            break;

        reclaim_throttle(NODE_DATA(numa_node_id()), VMSCAN_THROTTLE_WRITEBACK);
    }

    if (time_is_before_jiffies(READ_ONCE(wb->bw_time_stamp) + BANDWIDTH_INTERVAL))
        wb_update_bandwidth(wb);                              /* 更新带宽估算 */

    return ret;
}
```

**关键点**：
- `mapping->a_ops->writepages` 是文件系统注册的回写函数
- 如果 `ENOMEM` 且是 `WB_SYNC_ALL` 模式，会节流重试
- 写完后更新带宽估算值（用于 `balance_dirty_pages` 限流）

### 18.7 文件系统回写实现

#### 18.7.1 ext4 的 `writepages` 路径

（[fs/ext4/inode.c](file:///home/louis/code/linux/fs/ext4/inode.c)）ext4 使用 `ext4_writepages()`，通过 `mpage` 机制将脏页转换为 bio 提交：

```
ext4_writepages(mapping, wbc)
  │
  ├─ ext4_do_writepages(wbc, mpd)              ← 核心回写函数
  │   │
  │   ├─ 循环遍历脏页：
  │   │   │
  │   │   ├─ mpage_add_bh_to_extent(mpd, ...)  ← 将 buffer_head 添加到 extent
  │   │   │   │ 如果当前 extent 已满或不连续：
  │   │   │   │   → mpage_submit_extent(mpd)   ← 提交当前 extent
  │   │   │   │     → ext4_mpage_write_folio() 或 submit_bh()
  │   │   │   │
  │   │   │   └─ 否则：合并到当前 extent
  │   │   │
  │   │   └─ writeback_iter(mapping, wbc, ...)  ← 遍历脏页
  │   │
  │   └─ mpage_submit_extent(mpd)              ← 提交最后一个 extent
  │
  └─ ext4_io_submit(&mpd.io_submit)            ← 提交所有 bio
```

**ext4 的两种回写路径**：

```
ext4 回写
  │
  ├─ 路径 A：mpage 批量提交（推荐）
  │   ext4_mpage_write_folio()
  │     → 将连续块合并到一个 bio
  │     → 直接调用 submit_bio()
  │
  └─ 路径 B：buffer_head 逐个提交（回退）
      mpage_add_bh_to_extent() 失败时
        → submit_bh(REQ_OP_WRITE, bh)
          → submit_bh_wbc()  ← 每个 bh 一个 bio
            → bio_alloc() + submit_bio()
```

#### 18.7.2 ext2 的 `writepages` 路径（Buffer Head 方式）

（[fs/ext2/inode.c](file:///home/louis/code/linux/fs/ext2/inode.c)）ext2 使用 `mpage_writepages()`，通过 `__mpage_writepages()` 批量提交：

```c
// ext2 的 a_ops
const struct address_space_operations ext2_aops = {
    .writepages     = ext2_writepages,          // → mpage_writepages()
    .dirty_folio    = block_dirty_folio,        // 标记 bh 为脏
    ...
};

// mpage_writepages 内部
int __mpage_writepages(struct address_space *mapping,
                       struct writeback_control *wbc,
                       get_block_t get_block,
                       int (*write_folio)(struct folio *, struct writeback_control *))
{
    struct mpage_data mpd = { .get_block = get_block };
    struct folio *folio = NULL;
    struct blk_plug plug;
    int error;

    blk_start_plug(&plug);                              /* 批量提交优化 */

    /* 遍历所有脏页 */
    while ((folio = writeback_iter(mapping, wbc, folio, &error))) {
        if (write_folio) {
            error = write_folio(folio, wbc);            /* 逐个 folio 回写 */
            if (error <= 0)
                continue;
        }
        error = mpage_write_folio(wbc, folio, &mpd);    /* 将 folio 添加到 bio */
    }

    if (mpd.bio)
        mpage_bio_submit_write(mpd.bio);                /* 提交最后一个 bio */

    blk_finish_plug(&plug);                             /* 刷新 plug */
    return error;
}
```

**`mpage_write_folio()` 的关键逻辑**：
1. 遍历 folio 的 buffer_head 链表
2. 对每个脏的 `buffer_head`，调用 `get_block()` 获取物理块号
3. 如果物理块号连续，合并到当前 bio；否则提交当前 bio 并创建新 bio
4. 最终通过 `mpage_bio_submit_write()` 提交 bio

#### 18.7.3 `submit_bh()` — Buffer Head 到 Bio 的转换

（[fs/buffer.c](file:///home/louis/code/linux/fs/buffer.c)）将 buffer_head 转换为 bio 并提交给块层：

```c
void submit_bh(blk_opf_t opf, struct buffer_head *bh)
{
    submit_bh_wbc(opf, bh, WRITE_LIFE_NOT_SET, NULL);
}

static void submit_bh_wbc(blk_opf_t opf, struct buffer_head *bh,
                          enum rw_hint write_hint, struct writeback_control *wbc)
{
    const enum req_op op = opf & REQ_OP_MASK;
    struct bio *bio;

    BUG_ON(!buffer_locked(bh));
    BUG_ON(!buffer_mapped(bh));
    BUG_ON(!bh->b_end_io);
    BUG_ON(buffer_delay(bh));
    BUG_ON(buffer_unwritten(bh));

    /* 检查写错误标志 */
    if (test_set_buffer_req(bh) && (op == REQ_OP_WRITE))
        clear_buffer_write_io_error(bh);

    /* 设置 bio 标志 */
    if (buffer_meta(bh))
        opf |= REQ_META;                    /* 元数据 I/O */
    if (buffer_prio(bh))
        opf |= REQ_PRIO;                    /* 高优先级 I/O */

    /* 分配 bio */
    bio = bio_alloc(bh->b_bdev, 1, opf, GFP_NOIO);
    fscrypt_set_bio_crypt_ctx_bh(bio, bh, GFP_NOIO);

    /* 设置扇区号：块号 → 512 字节扇区号 */
    bio->bi_iter.bi_sector = bh->b_blocknr * (bh->b_size >> 9);

    bio->bi_write_hint = write_hint;

    /* 绑定页面数据 */
    bio_add_folio_nofail(bio, bh->b_folio, bh->b_size, bh_offset(bh));

    /* 设置完成回调 */
    bio->bi_end_io = end_bio_bh_io_sync;
    bio->bi_private = bh;

    /* cgroup 写回关联 */
    if (wbc) {
        wbc_init_bio(wbc, bio);
        wbc_account_cgroup_owner(wbc, bh->b_folio, bh->b_size);
    }

    /* 提交到块层 */
    submit_bio(bio);
}
```

**`submit_bh()` 的完整流程**：

```
submit_bh(REQ_OP_WRITE, bh)
  │
  ├─ [1] 参数检查
  │     ├─ buffer_locked(bh)       ← 必须已锁定
  │     ├─ buffer_mapped(bh)       ← 必须有物理块映射
  │     └─ !buffer_delay(bh)       ← 不能是延迟分配
  │
  ├─ [2] 设置 REQ 标志
  │     ├─ buffer_meta(bh) → REQ_META     ← 元数据
  │     └─ buffer_prio(bh) → REQ_PRIO     ← 高优先级
  │
  ├─ [3] bio_alloc(bh->b_bdev, 1, REQ_OP_WRITE, GFP_NOIO)
  │     └─ 从 fs_bio_set 分配一个 bio，容量 1 个 bio_vec
  │
  ├─ [4] 设置扇区号
  │     bi_sector = bh->b_blocknr * (bh->b_size >> 9)
  │     └─ 例：b_blocknr=1000, b_size=4096 → bi_sector=8000
  │
  ├─ [5] bio_add_folio_nofail(bio, bh->b_folio, bh->b_size, bh_offset(bh))
  │     └─ 将 folio 中的数据段绑定到 bio
  │
  ├─ [6] 设置完成回调
  │     bio->bi_end_io = end_bio_bh_io_sync
  │     bio->bi_private = bh
  │     └─ I/O 完成后：end_bio_bh_io_sync() → bh->b_end_io() → folio 解锁
  │
  └─ [7] submit_bio(bio)
        └─ submit_bio_noacct() → blk_mq_submit_bio() → 块层
```

**I/O 完成回调链**：

```
硬件完成中断
  → 块层完成 (bio_endio)
    → end_bio_bh_io_sync(bio)               ← submit_bh 设置的回调
      ├─ bio->bi_private = bh（取出 buffer_head）
      ├─ if (bio->bi_status) → clear_buffer_uptodate(bh)
      ├─ else → set_buffer_uptodate(bh)
      ├─ unlock_buffer(bh)                  ← 释放 BH_Lock 锁
      │     └─ 唤醒等待该 bh 的进程
      └─ bh->b_end_io(bh, uptodate)         ← 文件系统设置的回调
            └─ 如 end_buffer_async_write()
                ├─ folio 内所有 bh 完成？
                │   ├─ 是 → folio_end_writeback(folio)  ← 通知页缓存
                │   └─ 否 → 等待其他 bh 完成
                └─ bio_put(bio)
```

### 18.8 完整函数调用栈

#### 18.8.1 后台回写路径（Buffer Write 典型场景）

```
用户态 write(fd, buf, count)
  │
  ├─ sys_write() → vfs_write()
  │   └─ generic_perform_write(iocb, iter)           [mm/filemap.c]
  │       └─ 循环写入页缓存：
  │           ├─ a_ops->write_begin() → ext4_write_begin()
  │           │   └─ grab_cache_folio_write_begin()  ← 获取/创建 folio
  │           ├─ copy_folio_from_iter_atomic()       ← 用户数据拷贝到 folio
  │           └─ a_ops->write_end() → ext4_write_end()
  │               └─ block_dirty_folio()             ← 标记脏页
  │                   └─ __set_page_dirty()           ← 设置 PAGECACHE_TAG_DIRTY
  │                       └─ __mark_inode_dirty()     ← 将 inode 加入 wb->b_dirty
  │
  └─ balance_dirty_pages_ratelimited(mapping)         [mm/page-writeback.c]
      └─ balance_dirty_pages(bdi, dirty_thresh)      限流检查
          └─ 如果 wb_dirty > wb_thresh:
              └─ wb_start_background_writeback(wb)   唤醒 Flush 线程
                  └─ wb_wakeup(wb)
                      └─ mod_delayed_work(bdi_wq, &wb->dwork, 0)

┌─────────────────────────────────────────────────────────────────────┐
│  Flush 线程被调度执行                                                 │
└─────────────────────────────────────────────────────────────────────┘

wb_workfn(work)                                      [fs/fs-writeback.c]
  │
  └─ wb_do_writeback(wb)
      │
      ├─ wb_check_background_flush(wb)               ← 检查后台阈值
      │   └─ wb_writeback(wb, &work)                 ← 执行后台回写
      │       │
      │       ├─ queue_io(wb, work, dirtied_before)  ← b_dirty → b_io
      │       │
      │       └─ __writeback_inodes_wb(wb, work)     ← 遍历 b_io
      │           └─ writeback_sb_inodes(sb, wb, work)
      │               └─ __writeback_single_inode(inode, &wbc)
      │                   │
      │                   ├─ do_writepages(mapping, wbc)  ← 写回脏页
      │                   │   │                          [mm/page-writeback.c]
      │                   │   │
      │                   │   └─ mapping->a_ops->writepages(mapping, wbc)
      │                   │       │
      │                   │       ├─ [ext4] ext4_writepages()
      │                   │       │   └─ ext4_do_writepages()
      │                   │   │       ├─ 循环脏页：
      │                   │   │       │   └─ mpage_add_bh_to_extent()
      │                   │   │       │       └─ mpage_submit_extent()
      │                   │   │       │           └─ submit_bh() 或 submit_bio()
      │                   │   │       └─ ext4_io_submit()
      │                   │   │
      │                   │   └─ [ext2] mpage_writepages()
      │                   │       └─ __mpage_writepages()
      │                   │           ├─ mpage_write_folio()  ← 添加 folio 到 bio
      │                   │           │   └─ 遍历 buffer_head 链表
      │                   │           │       ├─ get_block()  ← 块号映射
      │                   │           │       └─ 合并或提交 bio
      │                   │           └─ mpage_bio_submit_write()  ← 提交 bio
      │                   │
      │                   ├─ write_inode(inode, wbc)    ← 写回元数据
      │                   │   └─ s_op->write_inode()
      │                   │
      │                   └─ requeue_inode() 或清除 I_DIRTY
      │
      └─ wb_check_old_data_flush(wb)                  ← 周期性老旧数据回写
          └─ wb_writeback(wb, &work)                  ← 同上
```

#### 18.8.2 同步回写路径（`sync()` / `fsync()`）

```
sync() / fsync(fd)
  │
  └─ sync_inodes_sb(sb)                              [fs/fs-writeback.c]
      │
      └─ 对每个 BDI：
          DEFINE_WB_COMPLETION(done, bdi)             ← 定义完成标记
          struct wb_writeback_work work = {
              .sb       = sb,
              .sync_mode = WB_SYNC_ALL,
              .nr_pages  = LONG_MAX,
              .for_sync  = 1,
              .done      = &done,
          };
          wb_queue_work(wb, &work);                   ← 入队
          wb_wait_for_completion(&done);              ← 等待完成
              │
              ▼
          wb_workfn()
            └─ wb_do_writeback()
                └─ get_next_work_item() → wb_writeback(wb, &work)
                    └─ writeback_sb_inodes(sb, wb, work)
                        └─ __writeback_single_inode(inode, &wbc)
                            ├─ do_writepages()       ← WB_SYNC_ALL: 等待 I/O
                            └─ write_inode()         ← 等待元数据写入
                                │
                                ▼
              finish_writeback_work(work)              ← 通知完成
                └─ atomic_dec_and_test(&done->cnt)
                    → wake_up_all(done->waitq)
```

### 18.9 触发条件对比总结

| 触发类型 | 触发条件 | `sync_mode` | `nr_pages` | 是否等待 |
|---------|---------|-------------|-----------|---------|
| **后台回写** | `wb_dirty > wb_thresh`（超过后台阈值） | `WB_SYNC_NONE` | `LONG_MAX` | 否 |
| **周期性回写** | `dirty_writeback_interval` 定时器到期（默认 5s） | `WB_SYNC_NONE` | `LONG_MAX` | 否 |
| **老旧数据回写** | 脏页驻留超过 `dirty_expire_interval`（默认 30s） | `WB_SYNC_NONE` | `LONG_MAX` | 否 |
| **内存回收** | vmscan 需要回收脏页 | `WB_SYNC_NONE` | 1024 | 否 |
| **sync()** | 用户态调用 `sync()` | `WB_SYNC_ALL` | `LONG_MAX` | 是 |
| **fsync()** | 用户态调用 `fsync(fd)` | `WB_SYNC_ALL` | `LONG_MAX` | 是 |
| **文件系统主动** | 空间不足等 | `WB_SYNC_NONE` | 指定数量 | 否 |

### 18.10 关键设计要点总结

| 设计点 | 说明 |
|-------|------|
| **异步回写** | `write()` 仅写入页缓存，Flush 线程异步回写，写操作不阻塞 |
| **三链表模型** | `b_dirty` → `b_io` → `b_more_io` 实现脏 inode 的调度与重试 |
| **Workqueue 实现** | Flush 线程不依赖独立内核线程，通过 `delayed_work` 在 `bdi_wq` 上调度 |
| **Plug 批量提交** | `wb_writeback()` 使用 `blk_start_plug/blk_finish_plug` 批量提交 I/O 请求 |
| **带宽估算** | 通过 `wb_update_bandwidth()` 动态估算写带宽，用于 `balance_dirty_pages()` 限流 |
| **Cgroup 写回** | `wbc_init_bio()` 将 bio 关联到 cgroup，实现 per-cgroup 的 I/O 隔离 |
| **I_SYNC 互斥** | 同一 inode 的并发回写通过 `I_SYNC` 标志互斥，防止数据竞争 |
| **优先级区分** | `REQ_SYNC`（同步写）vs `REQ_BACKGROUND`（后台写），块层据此调度 |

---

## Part V: 驱动实例分析

## 20. NVMe 驱动：块设备注册与移除流程

NVMe（Non-Volatile Memory Express）是当前最主流的 SSD 接口协议。Linux NVMe 驱动位于 `drivers/nvme/` 目录，分为 `host/`（主机端）、`target/`（目标端）、`common/`（公共代码）三个子目录。本章以 PCIe NVMe 驱动为主线，分析块设备注册与移除的完整流程。

### 20.1 关键数据结构

#### 20.1.1 nvme_ctrl — NVMe 控制器

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

#### 18.1.2 nvme_ns_head — Namespace 头部

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

#### 18.1.3 nvme_ns — Namespace 实例

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

#### 20.1.4 nvme_dev — PCIe 设备（含控制器）

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

#### 20.1.5 nvme_queue — 硬件队列

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

#### 18.1.6 nvme_ns_info — Namespace 扫描信息

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

### 20.2 注册流程（Probe）

当 NVMe PCIe 设备被 PCI 子系统枚举到时，触发 `nvme_probe()`。整个注册流程分为 **控制器初始化** 和 **Namespace 扫描** 两大阶段。

#### 20.2.1 注册流程总览

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

#### 18.2.2 Namespace 扫描详细流程

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

### 20.3 移除流程（Remove）

当 NVMe 设备被拔出或驱动卸载时，触发 `nvme_remove()`。移除流程与注册流程基本对称逆向。

#### 20.3.1 移除流程总览

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

#### 20.3.2 单个 Namespace 移除详细流程

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

### 18.4 与块层的交互接口

#### 18.4.1 概述

NVMe 驱动与通用块层的交互贯穿整个 **块设备注册** 和 **移除** 流程。从驱动视角看，核心交互点包括：

```
NVMe 驱动层                         通用块层
─────────────────                   ─────────────────
nvme_probe()
  ├─ nvme_alloc_admin_tag_set()  →  blk_mq_alloc_tag_set()    # 分配 Admin TagSet
  ├─ nvme_setup_io_queues()      →  (PCI 队列创建)
  ├─ nvme_alloc_io_tag_set()     →  blk_mq_alloc_tag_set()    # 分配 IO TagSet
  └─ nvme_start_ctrl()
       └─ nvme_queue_scan()
            └─ nvme_scan_work()
                 └─ nvme_alloc_ns()
                      ├─ blk_mq_alloc_disk()     →  blk_mq_alloc_queue() + __alloc_disk_node()
                      ├─ nvme_update_ns_info()
                      │    └─ queue_limits_start_update()
                      │    └─ blk_mq_freeze_queue()
                      │    └─ queue_limits_commit_update()
                      │    └─ set_capacity_and_notify()
                      │    └─ blk_mq_unfreeze_queue()
                      └─ device_add_disk()        →  add_disk_fwnode() → __add_disk() + add_disk_final()

nvme_remove()
  ├─ nvme_remove_namespaces()
  │    └─ nvme_ns_remove()
  │         ├─ set_capacity(disk, 0)
  │         └─ del_gendisk(ns->disk)    →  __del_gendisk()
  ├─ nvme_dev_disable()
  │    └─ nvme_cancel_tagset() / cancel_admin_tagset()
  └─ nvme_dev_remove_admin()
       └─ nvme_remove_admin_tag_set()
            └─ blk_mq_destroy_queue() + blk_mq_free_tag_set()
```

#### 20.4.2 块设备注册流程（Driver → Block Layer 视角）

##### 20.4.2.1 Tag Set 分配：`blk_mq_alloc_tag_set()`

**调用位置**：`nvme_alloc_admin_tag_set()` / `nvme_alloc_io_tag_set()`

**函数原型**：`block/blk-mq.c`

```c
int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set)
```

**调用栈**：

```
nvme_alloc_admin_tag_set(ctrl, &dev->admin_tagset, &nvme_mq_admin_ops, cmd_size)
  └─ blk_mq_alloc_tag_set(set)
       ├─ blk_mq_update_queue_map(set)        # 建立 CPU→HW Queue 映射
       ├─ blk_mq_alloc_tag_set_tags(set)      # 分配 tags[] 数组（nr_hw_queues 个）
       │    └─ for_each hw_queue:
       │         blk_mq_alloc_rq_maps(set)    # 分配 request 内存池
       │           └─ blk_mq_alloc_map_and_rqs()
       │                ├─ __sbitmap_queue_init_node()   # 初始化 sbitmap 位图
       │                └─ blk_mq_alloc_rqs(set, tags, ...)  # 分配 struct request 数组
       │                    # 对每个 tag:
       │                    #   rqs[i] = kmalloc_node(sizeof(struct request) + cmd_size)
       │                    #   rqs[i]->tag = i  （预设 tag 索引）
       │                    #   rqs[i]->mq_ctx = NULL （后续分配队列时绑定）
       │
       └─ set->tagset_set_flag(BLK_MQ_TAGSET_ALLOCED)  # 标记已分配
```

**关键数据结构**：`struct blk_mq_tag_set`（详见 5.14.5 节）

**NVMe 驱动调用示例**（PCI 驱动）：

```c
// Admin TagSet: 1 个硬件队列，深度 NVME_AQ_MQ_TAG_DEPTH（通常 31）
nvme_alloc_admin_tag_set(&dev->ctrl, &dev->admin_tagset,
    &nvme_mq_admin_ops, sizeof(struct nvme_iod));
// → set->nr_hw_queues = 1
// → set->queue_depth = 31
// → set->cmd_size = sizeof(struct nvme_iod)  (request 尾部紧邻驱动私有数据)
// → set->ops = &nvme_mq_admin_ops             (queue_rq = nvme_queue_rq)

// IO TagSet: 多个硬件队列（通常等于 CPU 数），深度 ctrl->sqsize
nvme_alloc_io_tag_set(&dev->ctrl, &dev->tagset, &nvme_mq_ops,
    nvme_pci_nr_maps(dev), sizeof(struct nvme_iod));
// → set->nr_hw_queues = ctrl->queue_count - 1 (减去 admin queue)
// → set->queue_depth = min(ctrl->sqsize, BLK_MQ_MAX_DEPTH - 1)
// → set->nr_maps = 3  (HCTX_TYPE_DEFAULT / HCTX_TYPE_READ / HCTX_TYPE_POLL)
// → set->ops = &nvme_mq_ops
```

**内核分配的内存布局**（每个 tag）：

```
┌──────────────────────────────────────────────┐
│ struct request  (kmalloc 分配)               │
│  ├─ .tag = i                                │  ← tag 索引 (0 ~ queue_depth-1)
│  ├─ .mq_ctx = NULL / ptr                    │  ← 软件队列指针
│  ├─ .mq_hctx = NULL / ptr                   │  ← 硬件队列指针
│  ├─ .bio = NULL                             │  ← bio 链表头
│  ├─ .q = NULL / ptr                         │  ← 请求队列指针
│  └─ ...                                     │
├──────────────────────────────────────────────┤
│ struct nvme_iod  (cmd_size 字节, 紧邻 request) │  ← 驱动私有数据
│  ├─ .cmd = nvme_command                     │  ← NVMe 命令（SQE）
│  ├─ .n_meta = ...                           │
│  └─ ...                                     │
└──────────────────────────────────────────────┘
```

##### 20.4.2.2 gendisk + request_queue 创建：`blk_mq_alloc_disk()`

**调用位置**：`nvme_alloc_ns()` 第 2 步

**函数原型**：`block/blk-mq.c`

```c
struct gendisk *__blk_mq_alloc_disk(struct blk_mq_tag_set *set,
        struct queue_limits *lim, void *queuedata,
        struct lock_class_key *lkclass)
```

**宏定义**：
```c
#define blk_mq_alloc_disk(set, lim, queuedata)  \
    __blk_mq_alloc_disk(set, lim, queuedata, NULL)
```

**调用栈**：

```
nvme_alloc_ns(ctrl, info)
  └─ disk = blk_mq_alloc_disk(ctrl->tagset, &lim, ns)
       └─ __blk_mq_alloc_disk(set, &lim, ns, NULL)
            ├─ q = blk_mq_alloc_queue(set, lim, queuedata)
            │    ├─ blk_alloc_queue(lim, node)           # 分配 request_queue 结构体
            │    │    └─ kzalloc_node(sizeof(*q))         # 分配队列内存
            │    │    └─ blk_queue_limits_set(q, lim)    # 设置初始队列限制
            │    │    └─ percpu_ref_init(&q->q_usage_counter, ...)  # 初始化引用计数
            │    │    └─ q->disk = NULL                   # 尚未绑定 gendisk
            │    │
            │    └─ blk_mq_init_allocated_queue(set, q)   # 初始化多队列
            │         ├─ q->mq_ops = set->ops             # 绑定 blk-mq 操作函数
            │         ├─ q->queue_ctx = alloc_percpu()    # 分配 per-CPU 软件队列
            │         ├─ q->nr_hw_queues = set->nr_hw_queues
            │         ├─ q->queue_hw_ctx = kcalloc(nr_hw_queues)  # 分配硬件队列数组
            │         ├─ for_each hw_queue:
            │         │    blk_mq_init_hctx(q, set, hctx_idx)
            │         │      └─ hctx->tags = set->tags[hctx_idx]  # 绑定 tag set
            │         │      └─ hctx->queue = q
            │         │      └─ hctx->ctxs = 分配 CPU→ctx 映射
            │         │      └─ hctx->flags = ...
            │         │
            │         └─ blk_mq_add_queue_tag_set(set, q)  # 将队列注册到 tag set
            │              └─ list_add_tail(&q->tag_set_list, &set->tag_list)
            │
            └─ disk = __alloc_disk_node(q, node, lkclass)  # 分配 gendisk
                 ├─ kzalloc_node(sizeof(*disk))             # 分配 gendisk 内存
                 ├─ disk->queue = q                         # 绑定 request_queue
                 ├─ q->disk = disk                          # 反向绑定
                 ├─ xa_init(&disk->part_tbl)                # 初始化分区表
                 ├─ bdev_alloc(disk, 0)                     # 分配 part0（块设备）
                 └─ set_bit(GD_OWNS_QUEUE, &disk->state)   # 标记磁盘拥有队列
```

**关键绑定关系**（创建后）：

```
nvme_ns                     gendisk                   request_queue
┌──────────────┐        ┌──────────────────┐        ┌──────────────────────┐
│ .disk ───────┼───────►│ .private_data ───┼───────►│ .mq_ops = &nvme_mq_ops│
│ .queue ──────┼───────►│ .queue ──────────┼───────►│ .queuedata = ns      │
│ .ctrl        │        │ .fops = &nvme_   │        │ .tag_set = ctrl->    │
│ .head        │        │   bdev_ops       │        │   tagset             │
│ .flags       │        │ .part0 = bdev    │        │ .queue_hw_ctx[]      │
└──────────────┘        └──────────────────┘        │ .queue_ctx (per-CPU) │
                                                     └──────────────────────┘
```

##### 20.4.2.3 队列限制更新：`queue_limits_start_update()` / `queue_limits_commit_update()`

**调用位置**：`nvme_update_ns_info_block()` 中，在冻结队列后、解冻队列前

**调用栈**：

```
nvme_update_ns_info_block(ns, info)
  ├─ lim = queue_limits_start_update(ns->disk->queue)
  │    └─ mutex_lock(&q->limits_lock)
  │    └─ return q->limits  (当前限制快照)
  │
  ├─ 修改 lim 的各个字段：
  │    ├─ nvme_set_ctrl_limits(ns->ctrl, &lim, false)
  │    │    └─ lim.max_hw_sectors = ctrl->max_hw_sectors
  │    │    └─ lim.max_segments = ctrl->max_segments
  │    │    └─ lim.max_integrity_segments = ctrl->max_integrity_segments
  │    │    └─ lim.dma_alignment = 3
  │    │    └─ lim.features |= BLK_FEAT_READ_ONLY (if applicable)
  │    │
  │    ├─ nvme_configure_metadata(ns->ctrl, ns->head, id, nvm, info)
  │    │    └─ 设置 lim.logical_block_size = 1 << ns->head->lba_shift
  │    │    └─ 设置 lim.physical_block_size = 1 << ns->head->lba_shift
  │    │    └─ 设置 lim.io_min = 1 << ns->head->lba_shift
  │    │
  │    ├─ nvme_set_chunk_sectors(ns, id, &lim)
  │    │    └─ lim.chunk_sectors = iob  (IO 块条带大小)
  │    │
  │    ├─ nvme_config_discard(ns, &lim)
  │    │    └─ lim.max_hw_discard_sectors = ...
  │    │    └─ lim.discard_granularity = ...
  │    │
  │    ├─ lim.features |= BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA  (if vwc present)
  │    ├─ lim.features |= BLK_FEAT_ROTATIONAL  (if is_rotational)
  │    └─ nvme_init_integrity(ns->head, &lim, info)
  │         └─ 设置 lim.integrity (保护信息类型、元数据大小等)
  │
  ├─ ret = queue_limits_commit_update(ns->disk->queue, &lim)
  │    └─ blk_queue_limits_set(q, lim)  # 原子性提交所有限制
  │    └─ mutex_unlock(&q->limits_lock)
  │    └─ return 0
  │
  └─ 后续设置：
       ├─ set_capacity_and_notify(ns->disk, capacity)
       ├─ set_disk_ro(ns->disk, readonly)
       └─ set_bit(NVME_NS_READY, &ns->flags)
```

##### 18.4.2.4 冻结/解冻队列：`blk_mq_freeze_queue()` / `blk_mq_unfreeze_queue()`

**作用**：冻结队列阻止新 I/O 提交，确保所有 in-flight I/O 完成，用于安全更新队列参数。

**调用栈**：

```
nvme_update_ns_info_block(ns, info)
  │
  ├─ memflags = blk_mq_freeze_queue(ns->disk->queue)
  │    └─ blk_freeze_queue_start(q)
  │    │    └─ percpu_ref_kill(&q->q_usage_counter)
  │    │    └─ blk_queue_flag_set(QUEUE_FLAG_FREEZING, q)
  │    │    └─ synchronize_rcu()                     # 等待所有 percpu_ref 引用者退出
  │    │    └─ blk_queue_flag_clear(QUEUE_FLAG_FREEZING, q)
  │    │
  │    └─ blk_freeze_queue_wait(q)
  │         └─ wait_event(q->mq_freeze_wq, percpu_ref_is_zero(&q->q_usage_counter))
  │         # 等待所有 I/O 完成（包括排队中尚未提交的请求）
  │
  │  ┌─ 此时队列已冻结：所有新 bio 在 blk_mq_submit_bio() 的
  │  │  bio_queue_enter() 处等待，不会进入 I/O 路径
  │  │
  │  ├─ 修改队列参数（见 18.4.2.3）
  │  │
  │  └─ blk_mq_unfreeze_queue(ns->disk->queue, memflags)
  │       └─ percpu_ref_resurrect(&q->q_usage_counter)  # 恢复引用计数
  │       └─ wake_up_all(&q->mq_freeze_wq)              # 唤醒等待的 I/O 提交者
  │       └─ memalloc_nofs_restore(memflags)             # 恢复内存分配标志
```

##### 18.4.2.5 容量设置：`set_capacity_and_notify()`

**调用位置**：`nvme_update_ns_info_block()` 末尾

**函数原型**：`block/genhd.c`

```c
void set_capacity_and_notify(struct gendisk *disk, sector_t capacity)
```

**内部逻辑**：
```
set_capacity_and_notify(disk, capacity)
  ├─ old_capacity = get_capacity(disk)       # 保存旧容量
  ├─ disk->part0->bd_nr_sectors = capacity   # 设置新容量
  │
  ├─ if (old_capacity != capacity)
  │    └─ kobject_uevent_env(&disk_to_dev(disk)->kobj, KOBJ_CHANGE, envp)
  │    # 发送 RESIZE=1 的 uevent 到用户空间（如 udev）
  │
  └─ # 如果是首次注册（容量从 0 变为非 0），
     # __add_disk() 中的 add_disk_final() 会扫描分区表
```

##### 20.4.2.6 块设备注册：`device_add_disk()`

**调用位置**：`nvme_alloc_ns()` 第 8 步

**调用栈**：

```
device_add_disk(ctrl->device, ns->disk, nvme_ns_attr_groups)
  └─ add_disk_fwnode(parent, disk, groups, NULL)
       ├─ 获取 tag_set->update_nr_hwq_lock（读锁）
       │
       ├─ __add_disk(parent, disk, groups, NULL)
       │    ├─ 设备号分配
       │    │    ├─ if (disk->major == 0)  # NVMe 驱动不设置 major
       │    │    │    ret = blk_alloc_ext_minor()
       │    │    │    disk->major = BLOCK_EXT_MAJOR
       │    │    │    disk->first_minor = ret
       │    │    └─ ddev->devt = MKDEV(disk->major, disk->first_minor)
       │    │
       │    ├─ ddev->parent = parent
       │    ├─ dev_set_name(ddev, "%s", disk->disk_name)  # 如 "nvme0n1"
       │    │
       │    ├─ ret = device_add(ddev)
       │    │    # 创建 /sys/block/nvme0n1 设备节点
       │    │    # 注册 sysfs 属性 (nvme_ns_attr_groups)
       │    │
       │    ├─ disk_alloc_events(disk)           # 分配事件处理
       │    │
       │    ├─ sysfs_create_link(block_depr, ...)  # 创建 /sys/block/ 链接
       │    │
       │    ├─ bdi_register(disk->bdi, "nvme0n1")  # 注册 BDI (backing device info)
       │    │
       │    ├─ blk_register_queue(disk)           # 注册 /sys/block/nvme0n1/queue/
       │    │    ├─ 创建 queue 相关 sysfs 文件
       │    │    ├─ 初始化调度器 (elevator_init)
       │    │    └─ 注册 mq sysfs 属性
       │    │
       │    ├─ 创建分区相关目录
       │    │    └─ device_add_disk 时还不够分区
       │    │
       │    └─ 释放 uevent suppress
       │         └─ dev_set_uevent_suppress(ddev, 0)
       │
       ├─ 释放 update_nr_hwq_lock 读锁
       │
       └─ add_disk_final(disk)
            ├─ register_disk(disk)              # 创建 /dev/nvme0n1 设备节点
            └─ blk_add_disk_partitions(disk)    # 扫描并添加分区
                 └─ bdev_disk_changed(disk, true)
                      └─ blk_scan_partitions(disk)
                           └─ 读取分区表 → 为每个分区添加 partN
                           └─ 创建 /dev/nvme0n1p1, /dev/nvme0n1p2, ...
```

**关键数据结构：`struct block_device_operations nvme_bdev_ops`**

```c
const struct block_device_operations nvme_bdev_ops = {
    .owner          = THIS_MODULE,
    .ioctl          = nvme_ioctl,              // 块设备 ioctl 处理
    .compat_ioctl   = blkdev_compat_ptr_ioctl, // 32-bit 兼容 ioctl
    .open           = nvme_open,               // open /dev/nvmeXnY
    .release        = nvme_release,            // close /dev/nvmeXnY
    .getgeo         = nvme_getgeo,             // 获取磁盘几何信息
    .get_unique_id  = nvme_get_unique_id,      // 获取唯一标识符
    .report_zones   = nvme_report_zones,       // 报告 ZNS 分区信息
    .pr_ops         = &nvme_pr_ops,            // 持久化保留操作
};
```

**注册完成后的内核可见状态**：

```
/sys/block/nvme0n1/                  ← 块设备 sysfs 目录
  ├── queue/                          ← 请求队列参数
  ├── device -> ../../../nvme0/       ← 指向父设备（控制器）
  ├── capability
  ├── ext_range
  ├── ...
/dev/nvme0n1                         ← 块设备节点
/dev/nvme0n1p1                       ← 分区节点（如果存在分区表）
```

#### 20.4.3 块设备移除流程（Driver → Block Layer 视角）

##### 20.4.3.1 标记磁盘死亡：`blk_mark_disk_dead()`

**调用位置**：`nvme_mark_namespaces_dead()`，在控制器突然断开（如热拔出）时调用

**调用栈**：

```
nvme_mark_namespaces_dead(ctrl)                    # 遍历所有 ns
  └─ for_each ns:
       blk_mark_disk_dead(ns->disk)
         └─ __blk_mark_disk_dead(disk)
              ├─ test_and_set_bit(GD_DEAD, &disk->state)  # 设置 GD_DEAD 标志
              │
              ├─ if (GD_OWNS_QUEUE)
              │    blk_queue_flag_set(QUEUE_FLAG_DYING, q)  # 设置 DYING 标志
              │
              ├─ set_capacity(disk, 0)                      # 容量清零
              │
              └─ blk_queue_start_drain(disk->queue)          # 启动 drain 模式
                   └─ percpu_ref_kill(&q->q_usage_counter)   # 拒绝新 I/O
                   └─ if (q->mq_ops)
                        blk_mq_wake_awaiters(q)              # 唤醒等待者
         └─ blk_report_disk_dead(disk, surprise=true)
              └─ xa_for_each(&disk->part_tbl, idx, bdev)
                   bdev_mark_dead(bdev, surprise)            # 通知文件系统
```

**状态变化**：

```
GD_DEAD=0 → GD_DEAD=1   # 磁盘标记死亡
QUEUE_FLAG_DYING=0 → QUEUE_FLAG_DYING=1  # 队列标记销毁中
# 此后所有新 bio 提交返回 BLK_STS_IOERR
# 所有排队中的请求取消或失败
```

##### 18.4.3.2 注销块设备：`del_gendisk()`

**调用位置**：`nvme_ns_remove()` 第 10 步

**调用栈**：

```
nvme_ns_remove(ns)
  ├─ ... 前置步骤 ...
  │
  └─ del_gendisk(ns->disk)
       └─ if (queue_is_mq(q))
            ├─ disable_elv_switch(q)           # 禁止调度器切换
            │
            └─ __del_gendisk(disk)
                 ├─ disk_del_events(disk)       # 删除事件处理
                 │
                 ├─ 阻止新 opener
                 │    └─ xa_for_each(&disk->part_tbl, idx, part)
                 │         bdev_unhash(part)    # 从 bdev 缓存中摘除
                 │
                 ├─ blk_report_disk_dead(disk, false)  # 通知文件系统
                 │    └─ xa_for_each(&disk->part_tbl, idx, bdev)
                 │         bdev_mark_dead(bdev, false)  # 优雅关闭
                 │
                 ├─ __blk_mark_disk_dead(disk)  # 标记死亡（如果尚未标记）
                 │    └─ blk_freeze_acquire_lock(q)  # 获取冻结锁
                 │
                 ├─ 删除所有分区
                 │    └─ xa_for_each_start(&disk->part_tbl, idx, part, 1)
                 │         drop_partition(part)  # 删除每个分区
                 │
                 ├─ 卸载 BDI 和 sysfs
                 │    ├─ sysfs_remove_link(..., "bdi")
                 │    ├─ bdi_unregister(disk->bdi)
                 │    └─ blk_unregister_queue(disk)
                 │         ├─ 删除 /sys/block/nvme0n1/queue/
                 │         └─ elevator_exit()
                 │
                 ├─ 清理磁盘设备
                 │    ├─ kobject_put(disk->part0->bd_holder_dir)
                 │    ├─ part_stat_set_all(disk->part0, 0)
                 │    ├─ device_del(disk_to_dev(disk))  # 删除 /sys/block/nvme0n1
                 │    └─ pm_runtime_set_memalloc_noio(..., false)
                 │
                 ├─ blk_mq_freeze_queue_wait(q)  # 等待所有 I/O 完全停止
                 │
                 ├─ blk_throtl_cancel_bios(disk)  # 取消限流队列
                 ├─ blk_sync_queue(q)             # 同步队列
                 ├─ blk_flush_integrity()          # 清理完整性检查
                 ├─ blk_mq_cancel_work_sync(q)    # 取消所有工作队列
                 ├─ rq_qos_exit(q)               # 退出 QoS
                 │
                 └─ if (GD_OWNS_QUEUE)
                      blk_mq_exit_queue(q)        # 退出多队列
                      # 内部调用：
                      #   blk_mq_destroy_queue_ctxs()
                      #   blk_mq_free_queue_rqs(q)
                      #   blk_mq_free_queue_rcu()
                      #   percpu_ref_exit()
```

##### 18.4.3.3 队列销毁：`blk_mq_destroy_queue()`

**调用位置**：`nvme_remove_admin_tag_set()` 中

**调用栈**：

```
nvme_remove_admin_tag_set(ctrl)
  └─ blk_mq_destroy_queue(ctrl->admin_q)
       └─ blk_queue_flag_set(QUEUE_FLAG_DYING, q)  # 标记 DYING
       └─ blk_queue_start_drain(q)                  # 启动 drain
       └─ blk_mq_exit_queue(q)                      # 退出多队列
            ├─ blk_mq_destroy_queue_ctxs(q)         # 释放软件队列
            ├─ blk_mq_free_queue_rqs(q)             # 释放 request 内存
            └─ blk_mq_free_queue_rcu(q)             # RCU 延迟释放
```

##### 20.4.3.4 Tag Set 释放：`blk_mq_free_tag_set()`

**调用位置**：`nvme_remove_admin_tag_set()` / `nvme_remove_io_tag_set()`

**调用栈**：

```
nvme_remove_io_tag_set(ctrl)
  └─ blk_mq_free_tag_set(ctrl->tagset)
       ├─ list_del(&set->tag_set_list)              # 从全局 tag_set_list 摘除
       ├─ for_each hw_queue:
       │    blk_mq_free_map_and_rqs(set, tags, hctx_idx)
       │      ├─ sbitmap_queue_free(&tags->bitmap_tags)     # 释放 sbitmap
       │      └─ blk_mq_free_rqs(set, tags, hctx_idx)
       │           └─ for_each tag:
       │                kfree(rqs[i])               # 释放 struct request + cmd_size
       │                # 注意：此时 nvme_iod 作为 request 的私有数据一同释放
       │
       └─ kfree(set->tags)                          # 释放 tags[] 指针数组
```

**移除流程中各步骤的时序关系**：

```
nvme_remove(pdev)
  │
  ├─ nvme_stop_ctrl()           # 停止 I/O 活动
  │
  ├─ nvme_remove_namespaces()
  │    └─ nvme_ns_remove(ns)
  │         ├─ set_capacity(disk, 0)          # 1. 容量清零
  │         └─ del_gendisk(ns->disk)          # 2. 注销块设备
  │              ├─ GD_DEAD=1                 #   a. 标记死亡
  │              ├─ 删除分区表                #   b. 删除分区
  │              ├─ 删除 sysfs                #   c. 删除 sysfs 节点
  │              └─ blk_mq_exit_queue(q)      #   d. 释放 request 队列
  │
  ├─ nvme_dev_disable()         # 硬件禁用
  │    ├─ 删除 IO 队列
  │    └─ 取消所有 tagset 请求
  │
  ├─ nvme_dev_remove_admin()    # 释放 Admin 资源
  │    └─ nvme_remove_admin_tag_set()
  │         ├─ blk_mq_destroy_queue(admin_q) # 销毁 Admin 队列
  │         └─ blk_mq_free_tag_set(admin_tagset)  # 释放 Admin TagSet
  │
  ├─ nvme_free_queues()         # 释放硬件队列内存
  │
  └─ nvme_uninit_ctrl()         # 控制器反初始化
       └─ cdev_device_del()     # 删除字符设备 /dev/nvmeX
```

#### 20.4.4 关键块层数据结构（与 NVMe 驱动交互相关）

##### 18.4.4.1 `struct gendisk` — 通用磁盘表示

文件：`include/linux/blkdev.h` L144

```c
struct gendisk {
    int major;                          // 主设备号（NVMe 使用 BLOCK_EXT_MAJOR）
    int first_minor;                    // 次设备号起始
    int minors;                         // 次设备号数量（分区数 + 1）

    char disk_name[DISK_NAME_LEN];      // 磁盘名称，如 "nvme0n1"

    struct xarray part_tbl;             // 分区表（xarray 索引：0=whole disk, 1+ = partitions）
    struct block_device *part0;         // 整个磁盘的块设备（bdev）

    const struct block_device_operations *fops;  // 块设备操作（nvme_bdev_ops）
    struct request_queue *queue;        // 请求队列
    void *private_data;                 // 驱动私有数据（指向 nvme_ns）

    struct bio_set bio_split;           // 用于 bio 拆分的 mempool

    int flags;                          // 通用标志
    unsigned long state;                // 状态位图
#define GD_NEED_PART_SCAN       0       // 需要分区扫描
#define GD_READ_ONLY            1       // 只读
#define GD_DEAD                 2       // 已死亡（移除流程中设置）
#define GD_NATIVE_CAPACITY      3       // 原生容量
#define GD_ADDED                4       // 已通过 device_add_disk() 注册
#define GD_SUPPRESS_PART_SCAN   5       // 抑制分区扫描
#define GD_OWNS_QUEUE           6       // 拥有队列（由 blk_mq_alloc_disk 创建）

    struct mutex open_mutex;            // open/close 互斥锁
    unsigned open_partitions;           // 已打开分区数

    struct backing_dev_info *bdi;       // 回写设备信息
    struct kobject *slave_dir;          // 从设备目录
    struct disk_events *ev;             // 事件处理
    int node_id;                        // NUMA 节点
    u64 diskseq;                        // 全局唯一序列号
    blk_mode_t open_mode;               // 当前打开模式
};
```

##### 20.4.4.2 `struct request_queue` — 请求队列

文件：`include/linux/blkdev.h` L478

```c
struct request_queue {
    void *queuedata;                    // 驱动私有数据（指向 nvme_ns）

    struct elevator_queue *elevator;    // I/O 调度器（如 mq-deadline）
    const struct blk_mq_ops *mq_ops;    // blk-mq 操作函数表（nvme_mq_ops）

    struct blk_mq_ctx __percpu *queue_ctx;  // per-CPU 软件队列

    unsigned long queue_flags;          // QUEUE_FLAG_* 标志
    unsigned int rq_timeout;            // 请求超时时间

    unsigned int queue_depth;           // 队列深度（= tag set 的 queue_depth）
    refcount_t refs;                    // 引用计数

    unsigned int nr_hw_queues;          // 硬件队列数
    struct blk_mq_hw_ctx * __rcu *queue_hw_ctx;  // 硬件队列指针数组

    struct percpu_ref q_usage_counter;  // I/O 使用计数（冻结/解冻控制）
    struct gendisk *disk;               // 反向指向 gendisk
    struct queue_limits limits;         // 队列限制（NVMe 在 nvme_update_ns_info_block 中设置）

    struct kobject *mq_kobj;            // 多队列 sysfs kobject
    unsigned int nr_requests;           // 最大请求数
    unsigned int async_depth;           // 异步请求深度
};
```

##### 18.4.4.3 `struct queue_limits` — 队列限制

文件：`include/linux/blkdev.h` L370

```c
struct queue_limits {
    blk_features_t features;            // 特性位图（BLK_FEAT_*）
    blk_flags_t flags;                  // 内部标志
    unsigned long seg_boundary_mask;    // DMA 段边界掩码
    unsigned long virt_boundary_mask;   // 虚拟边界掩码

    unsigned int max_hw_sectors;        // 最大硬件扇区数（NVMe: ctrl->max_hw_sectors）
    unsigned int max_dev_sectors;       // 设备最大扇区数
    unsigned int chunk_sectors;         // 条带块大小（NVMe: noiob/nvme_set_chunk_sectors）
    unsigned int max_sectors;           // 最大 I/O 扇区数（min of hw/dev/user）
    unsigned int max_segment_size;      // 最大段字节数
    unsigned int physical_block_size;   // 物理块大小（NVMe: 1 << lba_shift, 通常 512/4096）
    unsigned int logical_block_size;    // 逻辑块大小（同上）
    unsigned int alignment_offset;      // 对齐偏移
    unsigned int io_min;               // 最小 I/O 大小
    unsigned int io_opt;               // 最佳 I/O 大小

    unsigned int max_discard_sectors;    // 最大 DISCARD 扇区数
    unsigned int max_write_zeroes_sectors;  // 最大 WRITE ZEROES 扇区数
    unsigned int discard_granularity;   // DISCARD 粒度
    unsigned int discard_alignment;     // DISCARD 对齐

    unsigned short max_segments;        // 最大 DMA 段数（NVMe: NVMe_MAX_SEGS）
    unsigned short max_integrity_segments;  // 最大完整性段数
    unsigned short max_discard_segments;    // 最大 DISCARD 段数
    unsigned short max_write_streams;   // 最大写流数

    unsigned int dma_alignment;         // DMA 对齐要求（NVMe: 3, 即 8 字节对齐）
    unsigned int dma_pad_mask;          // DMA 填充掩码

    struct blk_integrity integrity;     // 完整性/元数据配置
};
```

##### 18.4.4.4 `struct block_device_operations` — 块设备操作函数表

文件：`include/linux/blkdev.h` L1648

```c
struct block_device_operations {
    void (*submit_bio)(struct bio *bio);  // 非 blk-mq 驱动使用（NVMe 不设置）
    int (*poll_bio)(struct bio *bio, ...); // 轮询 bio（NVMe 不设置）
    int (*open)(struct gendisk *disk, blk_mode_t mode);  // 打开设备
    void (*release)(struct gendisk *disk);  // 关闭设备
    int (*ioctl)(struct block_device *bdev, blk_mode_t mode,
                 unsigned cmd, unsigned long arg);  // ioctl 处理
    int (*compat_ioctl)(...);             // 32-bit 兼容 ioctl
    int (*getgeo)(struct gendisk *, struct hd_geometry *);  // 磁盘几何信息
    int (*set_read_only)(struct block_device *bdev, bool ro);  // 设置只读
    void (*free_disk)(struct gendisk *disk);  // 释放磁盘回调
    int (*report_zones)(...);             // ZNS 分区报告
    int (*get_unique_id)(...);            // 获取唯一标识符
    struct module *owner;                 // 所属模块
    const struct pr_ops *pr_ops;          // 持久化保留操作
};
```

#### 20.4.5 块层交互 API 汇总表

| 块层 API | 调用位置 | 阶段 | 功能描述 |
|----------|----------|------|----------|
| `blk_mq_alloc_tag_set()` | `nvme_alloc_admin_tag_set()` / `nvme_alloc_io_tag_set()` | 注册 | 分配 TagSet，初始化 sbitmap 位图和 request 数组 |
| `blk_mq_alloc_disk()` | `nvme_alloc_ns()` | 注册 | 创建 gendisk + request_queue，绑定到 TagSet |
| `queue_limits_start_update()` | `nvme_update_ns_info_block()` | 注册 | 获取队列限制锁，准备更新 |
| `blk_mq_freeze_queue()` | `nvme_update_ns_info_block()` | 注册 | 冻结队列，等待所有 I/O 完成 |
| `queue_limits_commit_update()` | `nvme_update_ns_info_block()` | 注册 | 提交队列限制更新（原子性） |
| `set_capacity_and_notify()` | `nvme_update_ns_info_block()` | 注册 | 设置磁盘容量，发送 RESIZE uevent |
| `set_disk_ro()` | `nvme_update_ns_info_block()` | 注册 | 设置磁盘只读状态 |
| `blk_mq_unfreeze_queue()` | `nvme_update_ns_info_block()` | 注册 | 解冻队列，恢复 I/O 提交 |
| `device_add_disk()` | `nvme_alloc_ns()` | 注册 | 注册块设备：创建 sysfs、设备节点、扫描分区 |
| `blk_mark_disk_dead()` | `nvme_mark_namespaces_dead()` | 移除 | 标记磁盘死亡（GD_DEAD），拒绝新 I/O |
| `set_capacity()` | `nvme_ns_remove()` | 移除 | 容量清零 |
| `del_gendisk()` | `nvme_ns_remove()` | 移除 | 注销块设备：删除分区、sysfs、设备节点 |
| `blk_mq_destroy_queue()` | `nvme_remove_admin_tag_set()` | 移除 | 销毁请求队列（标记 DYING，释放资源） |
| `blk_mq_free_tag_set()` | `nvme_remove_admin_tag_set()` / `nvme_remove_io_tag_set()` | 移除 | 释放 TagSet：释放 sbitmap 和所有 request 内存 |
| `blk_queue_dying()` | `nvme_dev_remove_admin()` | 移除 | 检查队列是否 DYING（决定是否跳过销毁） |

#### 20.4.6 完整生命周期时序图

```
nvme_probe()                         块层                                    用户空间
    │                                  │                                        │
    ├─ blk_mq_alloc_tag_set()  ───────►│ 分配 sbitmap + request[]              │
    │                                  │                                        │
    ├─ nvme_scan_work()                │                                        │
    │   └─ nvme_alloc_ns()             │                                        │
    │      ├─ blk_mq_alloc_disk() ────►│ 创建 gendisk + request_queue           │
    │      │                           │ 绑定 nvme_mq_ops                       │
    │      │                           │ 初始化 hw_ctx / sw_ctx                 │
    │      │                           │                                        │
    │      ├─ blk_mq_freeze_queue() ──►│ percpu_ref_kill() → 等待 I/O 完成      │
    │      ├─ queue_limits_commit() ──►│ 设置 max_sectors, block_size, 等       │
    │      ├─ set_capacity_and_notify()│ 设置 bd_nr_sectors                     │
    │      ├─ blk_mq_unfreeze_queue()►│ percpu_ref_resurrect() → 恢复 I/O       │
    │      │                           │                                        │
    │      └─ device_add_disk() ─────►│ device_add(ddev)          ────────────►│ /sys/block/nvme0n1
    │                                 │ blk_register_queue()     ────────────►│ /sys/block/nvme0n1/queue/
    │                                 │ add_disk_final()          ────────────►│ /dev/nvme0n1
    │                                 │   └─ 分区扫描            ────────────►│ /dev/nvme0n1p1
    │                                  │                                        │
    │    ▲ 设备正常工作 ▲              │                                        │
    │    ▼ 设备移除     ▼              │                                        │
    │                                  │                                        │
nvme_remove()                          │                                        │
    │                                  │                                        │
    ├─ nvme_ns_remove()                │                                        │
    │   ├─ set_capacity(disk, 0) ─────►│ bd_nr_sectors = 0                     │
    │   │                              │                                        │
    │   └─ del_gendisk(disk) ────────►│ disk_del_events()                      │
    │                                 │ blk_report_disk_dead()                  │
    │                                 │   └─ bdev_mark_dead()  ───────────────►│ 通知文件系统
    │                                 │ drop_partition(part)                   │
    │                                 │ blk_unregister_queue() ───────────────►│ 删除 /sys/block/nvme0n1/queue/
    │                                 │ device_del(ddev)       ───────────────►│ 删除 /sys/block/nvme0n1
    │                                 │ blk_mq_exit_queue()                    │
    │                                 │   └─ 释放 hw_ctx/sw_ctx               │
    │                                 │   └─ 释放 request 内存                 │
    │                                 │                                        │
    └─ nvme_remove_admin_tag_set()     │                                        │
         └─ blk_mq_free_tag_set() ───►│ 释放 sbitmap + tags[]                  │
                                      │                                        │
                                      │   最终：设备节点和 sysfs 全部删除       │
```

### 20.5 关键工作队列

NVMe 驱动使用三个全局工作队列（`core.c`）：

| 工作队列 | 用途 |
|----------|------|
| `nvme_wq` | 扫描、AEN 处理、固件激活、Keep-Alive、周期性重连 |
| `nvme_reset_wq` | 控制器复位（会 flush nvme_wq 上的工作） |
| `nvme_delete_wq` | 控制器删除（会 flush nvme_reset_wq 上的工作） |

### 20.6 涉及的文件清单

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

## 21. NVMe 驱动：块设备读写 I/O 流程

本章分析从用户态程序 `open()` / `read()` / `write()` 一个 NVMe 块设备（如 `/dev/nvme0n1`），到最终数据通过 PCIe 总线传输到硬件 SSD 的完整 I/O 路径，以及从中断/轮询完成到唤醒用户态程序的逆过程。

### 21.1 I/O 流程总览

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

### 21.2 涉及的关键数据结构

#### 21.2.1 nvme_iod — I/O 描述符（Per-Request）

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

#### 21.2.2 nvme_dma_vec — DMA 向量

文件：`drivers/nvme/host/pci.c` L421-L424

```c
struct nvme_dma_vec {
    dma_addr_t addr;       // DMA 地址
    unsigned int len;      // 长度
};
```

#### 21.2.3 nvme_command — NVMe 硬件命令

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

#### 21.2.4 nvme_request — 通用 NVMe 请求

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

#### 21.2.5 blkdev_dio — 块设备直接 I/O 描述符

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

#### 21.2.6 nvme_ns_head — Namespace 头（共享信息）

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

#### 21.2.7 nvme_ns — Namespace 实例（Per-Controller）

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

#### 21.2.8 nvme_queue — NVMe 硬件队列（SQ/CQ 元数据）

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

#### 21.2.9 blk_mq_queue_data — blk-mq 派发数据

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

### 21.3 详细函数调用栈

#### 21.3.1 用户态到块层：open 路径

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

#### 21.3.2 用户态到块层：read 路径（O_DIRECT）

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

#### 21.3.3 块层 submit_bio 到 NVMe queue_rq

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

#### 21.3.4 NVMe 驱动：请求提交

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

**SQ 命令拷贝与 Doorbell 机制详解**（[pci.c](file:///home/louis/code/linux/drivers/nvme/host/pci.c)）：

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

#### 21.3.5 NVMe 驱动：中断处理与完成

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

### 21.4 数据拷贝路径

#### 21.4.1 读操作数据流

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

#### 21.4.2 写操作数据流

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

### 21.5 PRP 与 SGL 数据描述符

NVMe 协议支持两种数据描述符格式，用于告诉硬件数据在主机内存中的位置：

| 特性 | PRP (Physical Region Page) | SGL (Scatter Gather) |
|------|---------------------------|--------------------------|
| **定义** | NVMe 原生格式，由 PR1/PR2 + PRP list 组成 | NVMe 1.2+ 引入的通用格式 |
| **描述** | 每项描述一个物理页（4KB 对齐） | 每项描述任意长度的数据段 |
| **效率** | 小 I 高效（1-2 页直接入命令） | 大 I 更灵活（段数少） |
| **限制** | 每页偏移必须一致 | 每个段可独立指定偏移和长度 |
| **使用条件** | 默认使用 | 启用 SGL 或超过阈值（`sgl_threshold`）时 |

**数据映射决策流程**（`nvme_map_data()`，[pci.c](:///drivers/nvme/host/pci.c)）：

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

#### 21.5.1 PRP 单段映射（nvme_pci_setup_data_simple）

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

#### 21.5.2 PRP 多段映射（nvme_pci_setup_data_prp）

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

#### 21.5.3 SGL 映射（nvme_pci_setup_data_sgl）

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

### 21.6 完成路径详解

#### 21.6.1 nvme_handle_cqe — CQE 处理

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

**nvme_find_rq — Tag 匹配与 Genctr 校验**（[nvme.h](file:///home/louis/code/linux/drivers/nvme/host/nvme.h)）：

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

**nvme_try_complete_req — 跨 CPU 完成**（[nvme.h](file:///home/louis/code/linux/drivers/nvme/host/nvme.h)）：

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

#### 21.6.2 nvme_complete_rq — NVMe 完成处理

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

**nvme_decide_disposition 决策逻辑**（[core.c](file:///home/louis/code/linux/drivers/nvme/host/core.c)）：

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

#### 21.6.3 nvme_end_req — 最终完成块层请求

```c
void nvme_end_req(struct request *req)
{
    blk_status_t status = nvme_status(nvme_re(req)->status);

    __nvme_end_req(req);       // 错误日志 + Zone Append 处理 + bio trace
    blk_mq_end_request(req, status);  // → blk_update_request() → bio_endio()
}
```

---

### 21.7 NVMe 驱动：open/release 详细流程

#### 21.7.1 open 路径

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

#### 21.7.2 release 路径

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

### 21.8 批量提交优化

#### 21.8.1 plug 机制

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

#### 21.8.2 nvme_queue_rqs 批量提交

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

#### 21.8.3 完成批处理

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

### 21.9 涉及的文件清单

| 文件 | 作用 |
|------|------|
| `block/fops.c` | 块设备文件操作：`blkdev_open()`, `blkdev_read_iter()`, `blkdev_write_iter()` |
| `block/blk-core.c` | `submit_bio()`, `submit_bio_noacct()` |
| `block/blk-mq.c` | `blk_mq_submit_bio()`, `blk_mq_dispatch_rq_list()`, `blk_mq_request_issue_directly()` |
| `drivers/nvme/host/pci.c` | `nvme_queue_rq()`, `nvme_queue_rqs()`, `nvme_prep_rq()`, `nvme_map_data()`, `nvme_irq()`, `nvme_poll_cq()`, `nvme_handle_cqe()` |
| `drivers/nvme/host/core.c` | `nvme_setup_cmd()`, `nvme_setup_rw()`, `nvme_complete_rq()`, `nvme_end_req()`, `nvme_open()`, `nvme_release()` |
| `drivers/nvme/host/nvme.h` | `struct nvme_request`, `struct nvme_command`, `struct nvme_rw_command` |

---

## Part VI: 附录

## 22. 总结

### 22.1 架构层次

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

### 22.2 代码量分布

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

### 22.3 关键设计特点

1. **多队列架构**：blk-mq 是块层的核心，通过 per-CPU 软件队列和硬件队列映射实现高并发、低锁竞争。
2. **QoS 策略链**：通过 `rq_qos` 链表实现灵活的策略组合，支持节流、延迟控制、成本模型和写回节流。
3. **丰富的调度器**：BFQ（侧重公平性和低延迟）、MQ-Deadline（侧重简单和低开销）、Kyber（侧重延迟控制）。
4. **完整的分区支持**：支持 18 种分区格式，覆盖主流和历史操作系统。
5. **硬件安全**：内联加密（blk-crypto）和 TCG Opal SED 支持，提供端到端数据保护。
6. **Zoned 设备**：完整的 ZNS/SMR 支持，包含 zone write plug 机制。

---

*分析日期：2026-07-15*
*内核版本：Linux 7.0 (ARM64)*
*分析范围：block/ 目录下全部 55 个 .c 文件 + 16 个 .h 文件 + partitions/ 子目录 18 个 .c 文件 + drivers/nvme/host/ 核心文件*