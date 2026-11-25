# Markdown文档阅读方法

> 本节同时充当 **11.6 SSD 专题** 的导读。原来的 mdbook 使用说明保留在文末。

## 1. 专题定位：同一块盘的两个视角

SSD 相关的知识可以沿"主机侧"和"介质侧"两条线切开，本专题两者的内容都有：

```
主机侧（内核代码）                          介质侧（固件/硬件）
─────────────────────────                  ─────────────────────────
应用 write()/read()                        NAND 阵列 + 控制器
  │                                          │
文件系统 → 块层 (bio / blk-mq)              FTL：映射、GC、WL、POR
  │                                          │
NVMe 驱动 → PCIe 队列                     ECC / 读重试 / 坏块管理
  │                                          │
NVMe 命令 (SQE/CQE)  ───────────────────→  主控固件
                                            │
                                          NAND 命令 (00h-30h 等)
```

- **主机侧**关注"Linux 如何把 I/O 变成 NVMe 命令"：块层、I/O 调度、`nvme` 驱动、队列与 doorbell。对应章节 [11.1 块层](../block/block-01-core-framework.md) 与 [11.2 NVMe](../nvme/nvme-pcie-host-analysis.md)。
- **介质侧**关注"命令落到 NAND 上会发生什么"：SLC/MLC/TLC 的阈值电压、编程/擦除机理、读干扰、ECC、FTL 的映射与垃圾回收。本专题的 NAND 五篇主要讲这条线。

只读主机侧，会看不懂"为什么顺序写比随机写快""为什么 TRIM 之后写性能恢复"；只读介质侧，则无法把 FTL 行为与内核的 `REQ_OP_DISCARD`、zone 写指针对应起来。建议两条线交叉阅读。

## 2. 专题结构

| 篇目 | 主题 | 侧重点 |
|--|--|--|
| [NAND Flash 解密](nand-flash.md) | 单篇速览 | 用一篇文章串起 cell 结构、操作、数据完整性、FTL、ECC |
| [nand原理/简介](nand原理/README.md) | 总览与术语 | 存储层次、三种操作、两条内核路径 |
| [nand原理/物理特性](nand原理/physical.md) | 器件物理 | 浮栅、阈值电压、SLC/MLC/TLC/QLC、寿命衰减 |
| [nand原理/内部结构](nand原理/arch.md) | 芯片组织 | page/block/plane/die/LUN、OOB、坏块表 BBT、读干扰 |
| [nand原理/命令](nand原理/cmd.md) | 总线协议 | 命令集、地址周期、时序、ONFI/Toggle、Cache Read |
| [nand原理/FTL](nand原理/ftl.md) | 地址转换层 | 映射、GC、写放大、磨损均衡、掉电恢复 |
| [nvme原理/简介](nvme原理/intro.md) | 协议定位 | 为什么需要 NVMe、与 PCIe 的关系、Namespace |
| [nvme原理/设备原理](nvme原理/arch.md) | 队列模型 | SQ/CQ、doorbell、PRP/SGL、中断与轮询 |
| [nvme原理/数据结构](nvme原理/define.md) | 命令格式 | `nvme_command`、CQE、Identify、驱动侧结构 |
| [nvme原理/命令](nvme原理/cmd.md) | 命令集 | Admin/NVM 操作码、读写流程、数据指针 |

依赖关系：

```
        nand-flash.md（速览，可独立阅读）
              │
   ┌──────────┴───────────┐
   ▼                      ▼
nand原理/README ──→ physical ──→ arch ──→ cmd
                                   │
                                   ▼
                                  ftl
                                   │
                                   ▼
                          （与块层/文件系统衔接）

nvme原理/intro ──→ arch ──→ define ──→ cmd ──→ 11.2 NVMe 驱动
```

## 3. 两条推荐阅读路径

**路径 A：介质侧优先（想搞懂"SSD 为什么这样"）**

1. [nand原理/简介](nand原理/README.md)：建立 page/block/plane/die 的层次感；
2. [nand原理/物理特性](nand原理/physical.md)：理解"电压即数据"与 SLC/MLC/TLC 的可靠性差异；
3. [nand原理/内部结构](nand原理/arch.md)：OOB、BBT、读干扰、GC 与写放大；
4. [nand原理/命令](nand原理/cmd.md)：看真实命令与时序，理解"为什么擦除慢、编程以页为单位"；
5. [nand原理/FTL](nand原理/ftl.md)：把上面所有约束收束成映射表、GC、WL、POR；
6. 回到 [11.1 块层](../block/block-01-core-framework.md)，看这些约束如何表现为 `discard`、写放大与延迟抖动。

**路径 B：主机侧优先（想搞懂"内核怎么发命令"）**

1. [11.1 块层核心框架](../block/block-01-core-framework.md)：`bio` → `request` → `blk-mq`；
2. [11.1 I/O 调度](../block/block-02-io-scheduling.md)：`mq-deadline` 如何影响随机/顺序表现；
3. [nvme原理/简介](nvme原理/intro.md) 与 [nvme原理/设备原理](nvme原理/arch.md)：队列对、doorbell、PRP；
4. [nvme原理/数据结构](nvme原理/define.md) 与 [nvme原理/命令](nvme原理/cmd.md)：命令格式与读写流程；
5. [11.2 NVMe PCIe Host 驱动](../nvme/nvme-pcie-host-analysis.md)：`nvme_probe()`、`nvme_queue_rq()` 的实际代码；
6. 最后补 [nand原理/FTL](nand原理/ftl.md)：理解设备端为什么"看起来像块设备，实际不是"。

## 4. 与内核存储栈的衔接点

| 设备侧概念 | 内核里的对应 | 代码位置 |
|--|--|--|
| 逻辑地址 LBA | `sector_t` / `bio->bi_iter.bi_sector` | `include/linux/blk_types.h` |
| TRIM / deallocate | `REQ_OP_DISCARD`、`blkdev_issue_discard()` | `block/blk-lib.c` |
| 写零 | `REQ_OP_WRITE_ZEROES` | `include/linux/blk_types.h` |
| 顺序写 zone（ZNS） | `REQ_OP_ZONE_APPEND`、`blk_zone` | `include/uapi/linux/blkzoned.h` |
| 设备并行性 | `blk-mq` 的 hctx/软件队列 | `block/blk-mq.c` |
| NVMe 队列 | `struct nvme_queue`、SQ/CQ | `drivers/nvme/host/pci.c` |
| 裸 NAND | MTD 子系统、`nand_device` | `drivers/mtd/nand/core.c` |

本树 `.config` 中 `CONFIG_BLK_DEV_NVME=y`、`CONFIG_NVME_CORE=y`、`CONFIG_MQ_IOSCHED_DEADLINE=y`，因此主机侧的 NVMe 与 mq-deadline 路径是"真实可运行"的；`CONFIG_MTD` 与 `CONFIG_BLK_DEV_ZONED` 未启用，涉及裸 NAND 与 ZNS 的部分属于"源码存在、当前配置未启用"。

## 5. 术语约定

本专题各篇用词不完全统一，阅读时按以下约定理解：

| 术语 | 含义 | 注意 |
|--|--|--|
| cell | 单个浮栅存储单元 | 存 1/2/3/4 bit 决定 SLC/MLC/TLC/QLC |
| page | 编程/读取的最小单位 | 同一 wordline 上的 cell 集合，容量通常 4K~16K |
| block | 擦除的最小单位 | 含数十到数百个 page |
| plane / die / LUN | 并行单元层级 | 多 plane 可并行编程，提升带宽 |
| LBA / PBA | 逻辑块地址 / 物理块地址 | FTL 维护二者映射 |
| OP | Over-Provisioning，预留空间 | 影响 GC 效率与写放大 |
| WA | Write Amplification，写放大 | 主机写 1 字节，介质可能写多字节 |
| BER | Bit Error Rate，原始误码率 | 随 P/E 次数与保持时间上升 |
| ECC | 纠错码 | 强度必须覆盖最坏 BER |
| GC | Garbage Collection | 回收无效页所在 block |
| WL | Wear Leveling | 均衡各 block 的 P/E 次数 |
| POR | Power Off Recovery | 掉电后恢复映射表 |

## 6. 常见误区

读这套笔记时，容易把主机侧与介质侧的概念混为一谈，下面几组需要区分清楚：

| 易混点 | 正确理解 |
|---|---|
| "SSD 有 FTL，所以内核也有 FTL" | FTL 在 SSD 固件内部，对主机完全透明；内核侧只有通用块层和文件系统，不存在 SSD FTL 代码 |
| "NVMe 是一种闪存" | NVMe 是**主机与控制器之间的协议/接口**，其下可以承载 NAND、ZNS 甚至非易失内存介质 |
| "ZNS 设备不需要 GC" | ZNS 把 zone 管理责任上移到主机，主机仍需按 zone 顺序写并主动 reset，本质是把 GC 变成显式操作 |
| "NAND 的 page 可以覆盖写" | NAND 只能"编程一次、整块擦除"，覆盖写必须写入新 page 并让旧页失效，这正是 GC 与 WA 的根源 |
| "OP 越大越浪费" | OP 提供 GC 周转空间，直接决定稳态写放大与寿命；企业盘通常预留 7%~28% |
| "掉电只会丢失缓存数据" | 映射表若未持久化，掉电可能导致**整盘映射错乱**；因此 FTL 需要 checkpoint + journal 两级保护 |

对内核读者而言，只需牢记一条链路：文件系统 → 块层（bio/blk-mq）→ NVMe 驱动 → 控制器 → 介质。介质内部如何搬运数据，由固件负责，笔记只在解释性能与寿命现象时才下探到 FTL 层。

## 7. 阅读方式

普通markdown编辑器即可打开编辑, 使用mdbook支持浏览器阅读,  
mdbook是rust编写的类似gitbook的开源免费工具.  
[mdbook英文介绍](https://rust-lang.github.io/mdBook/)
[mdbook中文介绍](https://llever.com/mdBook-zh/print.html)

