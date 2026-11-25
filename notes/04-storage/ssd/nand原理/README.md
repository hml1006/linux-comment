# NAND Flash 简介

    介绍nand flash基本物理原理, 面向软件开发者的芯片特性.

## 从存储层次看 NAND

CPU 能直接寻址的只有 DRAM 与少量 SRAM，它们掉电即失；磁盘、SSD、eMMC、UFS、SD 卡则属于非易失存储。NAND Flash 是其中使用最广的一类介质：它把电荷囚禁在浮栅或电荷陷阱中，用阈值电压的高低表示比特，不需要电力即可保持数据。

NAND 与 DRAM、NOR、机械硬盘的差别不只是“快慢”，更在于**访问语义**：

| 介质 | 寻址方式 | 读粒度 | 写粒度 | 擦除 | 典型用途 |
|------|----------|--------|--------|------|----------|
| SRAM | 字节/缓存行 | 字节 | 字节 | 不需要 | CPU 缓存 |
| DRAM | 字节/缓存行 | 字节 | 字节 | 不需要（需刷新） | 主存 |
| NOR Flash | 字节/随机 | 字节 | 字节（1→0） | 块 | 代码就地执行 |
| NAND Flash | 页/块 | 页（4–16 KB） | 页（4–16 KB） | 块（数 MB） | SSD、eMMC、UFS、SD |
| 机械硬盘 | 扇区 | 扇区（512 B/4 KB） | 扇区 | 不需要 | 大容量冷数据 |

对软件开发者最重要的三条结论是：

1. **读以页为单位，写（编程）以页为单位，擦除以块为单位**；
2. **编程只能把比特从 1 变成 0，擦除才能把 0 变回 1**，所以覆盖写必须先擦除；
3. **擦除的粒度远大于读写**，导致“异地更新 + 垃圾回收”成为所有 NAND 系统的共同设计。

这三点在 [闪存内部原理](physical.md) 中有器件级的解释，在 [闪存内部结构原理](arch.md) 中会展开到 page/block/plane 的组织方式。

## 单元类型：SLC / MLC / TLC / QLC

一个存储单元（cell）能区分多少个阈值电压区间，就决定了它能存多少比特。SLC 用两个区间表示 0/1；MLC 用四个区间表示 2 bit；TLC 用八个区间表示 3 bit；QLC 用十六个区间表示 4 bit。区间越多，单个晶体管的容量越大、每 GB 成本越低，但写入越慢、寿命越短、对 ECC 的要求越高。

| 类型 | bit/cell | 阈值区间数 | 典型 P/E 次数 | 页读延迟（量级） | 页编程时间（量级） | 相对成本 | 典型场景 |
|------|----------|------------|---------------|------------------|--------------------|----------|----------|
| SLC | 1 | 2 | 5 万–10 万 | 25–50 µs | 0.2–0.4 ms | 最高 | 企业级缓存盘、工业/车规 |
| MLC | 2 | 4 | 3 千–1 万 | 40–60 µs | 0.4–0.8 ms | 高 | 企业级、高端消费 |
| TLC | 3 | 8 | 1 千–3 千 | 50–80 µs | 0.5–1.0 ms | 低 | 主流消费级、读密集型数据中心 |
| QLC | 4 | 16 | 100–1 000 | 80–120 µs | 1–2 ms | 最低 | 大容量温数据、读多写少 |

> 表中数值是 3D NAND 时代的量级，不同厂商、不同堆叠层数、不同接口速率差异很大，不能当作选型时的精确参数。

为什么 bit/cell 越多越慢？可以分三个层面理解：

- **写**：TLC/QLC 需要把阈值电压精确地“注”到某个窄区间，通常要多次 program-verify 迭代；同一 wordline 还要分 lower/middle/upper 多 pass 写入。
- **读**：一个 cell 的状态不能一次比较得出，需要多次改变参考电压做 sensing，才能分辨出多位信息。
- **可靠性**：区间越窄，留白越小，电荷泄漏、读干扰、单元间耦合造成的阈值漂移更容易越过判决边界，因此需要更强的 ECC（BCH/LDPC）和更多的 OOB 冗余。

QLC 之后还有 PLC（5 bit/cell）的研究与少量产品，但可靠性和写入性能的代价更大，尚未成为主流。

## 存储层次：channel → chip/die → plane → block → page

NAND 的容量与并行性由一套层次化结构决定。不同厂商、不同文档的命名略有差异（Device/Target/LUN 等），但从“主机如何访问”的角度可以统一为：

```text
Host / NAND Controller
 └─ Channel 0..N            共享一组 DQ/DQS/RE/WE/CLE/ALE/CE 信号
     └─ Chip/Package 0..M   一个封装，可含 1..16 个 Die
         └─ Die/LUN 0..K    独立执行命令、独立上报 Ready/Busy
             └─ Plane 0..P  拥有独立 Page Register，可并行操作
                 └─ Block   擦除的最小单位，通常 128–512 个 Page
                     └─ Page  读/编程的最小单位，通常 4/8/16 KB + OOB
                         └─ Cell  1–4 bit
```

| 层次 | 典型规模 | 关键属性 | 对性能的意义 |
|------|----------|----------|--------------|
| Channel | 1–8 条/控制器 | 共享数据总线与命令锁存 | 通道间可并行，通道内争用总线 |
| Chip/Package | 1–16 Die/封装 | 独立片选 CE# | 片选间可并行，但共享通道 |
| Die/LUN | 1–4 Plane/Die | 独立 Ready/Busy、独立命令队列 | 最基本的并行执行单元 |
| Plane | 1K–4K Block | 独立 Page Register，支持 multi-plane | 同 Die 内可并行读/写两个 Plane |
| Block | 128–512 Page | 擦除单位，共享衬底 | 擦除代价高，决定 GC 粒度 |
| Page | 4–16 KB + OOB | 读/编程单位，对应一根 wordline | 顺序写、整页写 |
| Cell | 1–4 bit | 浮栅/电荷陷阱晶体管 | 决定寿命、速度、ECC 强度 |

并行性不是免费的：同一通道上的多个 Die 共享 DQ 总线，命令、地址、数据都要排队；同一 Die 上的多个 Plane 虽然可以同时执行，但共享内部电荷泵与 sensing 电路，时序上仍有限制。SSD 主控的调度器本质上就是在这些约束下做“把哪些请求交给哪个 Die/Plane”的排布。

## 三种基本操作

NAND 的命令集围绕三个动作展开，具体时序见 [命令](cmd.md)。

### 读（Read）

主机给出页地址，芯片把该页的阈值状态 sensing 出来，经过内部 ECC 或直接送到 Page Register，再由主机通过数据总线读走。读操作不改变存储内容，但会在未选中的 wordline 上施加 pass 电压，长期反复读同一区域会累积 read disturb。

### 编程（Program）

主机把整页数据写入 Cache Register，芯片再把它“编程”进目标 wordline。编程只能把 cell 从擦除态（逻辑 1）推向更低的阈值（逻辑 0），不能反向。因此：

- 目标页必须是已擦除的；
- 一页通常要一次写满，从列地址 0 开始；
- 同一页不能被重复编程（除非是 SLC 缓存或特殊模式）。

### 擦除（Erase）

擦除以 block 为单位，把所有 wordline 上的 cell 一起推回高阈值（逻辑 1）。由于一个 block 内所有 cell 共享衬底，施加高电压时无法只影响某一页，这是“擦除粒度大”的物理根源。

| 操作 | 粒度 | 方向 | 典型耗时 | 是否可原地重复 |
|------|------|------|----------|----------------|
| 读 | Page（4–16 KB） | 不改状态 | 25–120 µs | 可以 |
| 编程 | Page（4–16 KB） | 1 → 0 | 0.2–2 ms | 不可以，需先擦除 |
| 擦除 | Block（数 MB） | 0 → 1 | 3–10 ms | 可以（擦除后重新写） |

这张表解释了很多上层现象：

- **随机写比顺序写慢**：随机写会频繁触发“读-改-写”和 GC；
- **小写放大**：只改 4 KB，也可能要搬移整个 block 的有效页；
- **TRIM 很重要**：告诉 SSD 哪些 LBA 已无效，GC 才能少搬数据；
- **写入放大（WA）** 是 SSD 寿命与性能的核心指标。

## 从裸 NAND 到 SSD：FTL 的位置

裸 NAND 芯片只提供“读页/写页/擦块”三个动作，以及一堆坏块和比特错误。要把这种介质变成主机可以随意覆盖写的块设备，中间必须有一层 **FTL（Flash Translation Layer）**：

```text
应用
 └─ 文件系统（ext4/xfs/btrfs…）
     └─ 块层（blk-mq）
         └─ NVMe 驱动
             └─ NVMe SSD 固件
                 ├─ FTL：LBA → PPA 映射、GC、磨损均衡、掉电恢复
                 └─ NAND 控制器：ECC、坏块管理、时序
                     └─ NAND 颗粒
```

主机看到的只是连续的 LBA 空间；**坏块、ECC、GC、磨损均衡全部由 SSD 固件对主机透明**。这也是为什么 NVMe SSD 可以直接跑通用文件系统，而裸 NAND 通常需要 UBI/UBIFS 这类专门的闪存文件系统。

FTL 的详细职责见 [FTL](ftl.md)，GC 与磨损均衡的机理见 [闪存内部结构原理](arch.md)。

## ECC 与坏块管理

NAND 的错误不是“整页全错”，而是页内少量比特翻转。常见来源包括：

| 错误来源 | 机理 | 缓解手段 |
|----------|------|----------|
| 擦写磨损 | 氧化层变薄，电荷保持能力下降 | 磨损均衡、降低 P/E |
| 数据保持（retention） | 电荷随时间/高温泄漏 | 数据刷新、ECC 余量 |
| 读干扰（read disturb） | 读时 pass 电压轻微编程了未选中页 | 读计数、read reclaim |
| 编程干扰（program disturb） | 编程时相邻 wordline 受高压影响 | 时序优化、ECC |
| 单元间耦合 | 相邻 cell 电荷变化影响阈值 | 写序、预补偿、ECC |
| 写错误 | 多 pass 写入时 lower page 已出错 | 1-pass 写、校验 |

ECC 的基本做法是：把一页切成若干 sector（如 1 KB/2 KB），每个 sector 计算校验位，存放在该页的 OOB（spare area）中。读时先纠错再交给上层；纠错失败则报 uncorrectable error。BCH 适合硬判决、实现简单；LDPC 提供软判决增益，是现代 TLC/QLC 的主流选择，但需要多次 read retry 与更复杂的译码器。

坏块分为两类：

- **出厂坏块（factory bad block）**：厂商在出厂测试时标记，通常在第一个 page 的 OOB 特定字节写入非 0xFF 值；
- **增长坏块（grown bad block）**：使用过程中编程/擦除失败或 ECC 无法纠正，被驱动或 FTL 动态标记。

管理方式通常有两种：预留块替换，或坏块表（BBT）跳过。详见 [闪存内部结构原理](arch.md) 中的 OOB 与 BBT 小节。

## 磨损均衡与 FTL 的作用

每个 block 的 P/E 次数有限。如果总是写同一批“热”块，它们会先耗尽，整盘提前报废。磨损均衡（Wear Leveling）因此成为 FTL 的必备功能：

- **动态磨损均衡**：新写入的数据优先分配到 P/E 较低的块，天然分散热数据；
- **静态磨损均衡**：定期把长期不动的“冷数据”从低 P/E 块搬到高 P/E 块，让冷块也参与轮换；
- **与 GC 协同**：GC 选择 victim block 时，除了有效页比例，也会考虑块的磨损程度。

磨损均衡的收益可以用寿命公式粗略衡量：

```text
可写入总量 TBW ≈ 容量 × P/E 次数 / 写入放大 WA
```

例如 1 TB TLC、P/E=3000、WA=2，则理论 TBW 约为 1.5 PB。降低 WA（更大的 OP、更好的 GC、TRIM）和降低 P/E 消耗同样重要。

## 与 Linux 内核的衔接

Linux 对 NAND 有两条完全不同的支持路径，理解它们的区别是阅读本目录后续笔记的前提。

### 路径一：裸 NAND（MTD 子系统）

裸 NAND 芯片通过并行/SPI 接口直接挂在 SoC 上，内核把它抽象为 MTD（Memory Technology Device）设备，而不是块设备：

| 层次 | 代码位置 | 职责 |
|------|----------|------|
| NAND 核心 | `drivers/mtd/nand/raw/nand_base.c` | 通用命令序列：读页、写页、擦块、读状态、复位、读 ID |
| 芯片 ID 表 | `drivers/mtd/nand/raw/nand_ids.c` | 厂商/器件 ID、页大小、OOB 布局、ECC 能力 |
| 控制器驱动 | `drivers/mtd/nand/raw/*` | 具体 SoC 的时序、DMA、ECC 引擎 |
| SPI-NAND | `drivers/mtd/nand/spi/` | SPI 接口 NAND，命令集与 ONFI 并行 NAND 不同 |
| MTD 核心 | `drivers/mtd/mtdcore.c`、`mtdchar.c`、`mtdblock.c` | 字符设备 `/dev/mtdN`、简单块封装 |
| UBI | `drivers/mtd/ubi/` | 坏块管理、磨损均衡、逻辑卷、后台线程 |
| UBIFS | `fs/ubifs/` | 面向闪存的日志结构文件系统 |

关键点：**MTD/UBI 把 NAND 当作“有坏块、需要擦除、需要 ECC 的原始介质”来管理**。`mtdblock` 只是最简单的块封装，不做磨损均衡，也不适合直接放通用文件系统；生产环境通常使用 UBI + UBIFS，或只读的 squashfs + UBI。

### 路径二：SSD（NVMe/块层）

NVMe SSD 内部已经包含 FTL，主机侧内核只看到标准的块设备：

| 层次 | 代码位置 | 职责 |
|------|----------|------|
| NVMe 核心 | `drivers/nvme/host/core.c` | 命令构造、命名空间、字符设备、ioctl |
| PCIe 传输 | `drivers/nvme/host/pci.c` | BAR 映射、Admin/IO 队列、doorbell、MSI-X |
| 块层 | `block/`、`blk-mq` | 请求排队、调度、合并 |
| 文件系统 | `fs/` | ext4/xfs/btrfs 等，不感知 NAND |

在这条路径上，**内核不需要也不应该知道坏块、ECC、GC**；这些都在 SSD 固件里完成。NVMe 的队列、命令与数据结构见 [NVMe 简介](../nvme原理/intro.md)、[NVMe 设备基本结构](../nvme原理/arch.md)、[NVMe 数据结构定义](../nvme原理/define.md)、[NVMe 命令](../nvme原理/cmd.md)。

### 两条路径的对比

| 维度 | 裸 NAND + MTD/UBI | NVMe SSD |
|------|-------------------|----------|
| 主机看到 | MTD 字符设备/UBI 卷 | 块设备 `/dev/nvme0n1` |
| 坏块管理 | 内核/UBI 负责 | SSD 固件负责，主机不可见 |
| ECC | 控制器或软件 ECC | 固件/主控负责 |
| 磨损均衡 | UBI 负责 | FTL 负责 |
| 掉电恢复 | UBI/UBIFS journal | FTL + 电容/日志 |
| 典型文件系统 | UBIFS、JFFS2、squashfs | ext4、xfs、btrfs |
| 适用场景 | 嵌入式、路由器、车机 | 通用服务器/桌面 |

> 在 `CONFIG_PREEMPT_RT=y` 的内核上，NAND 控制器中断通常会被线程化，UBI 的后台线程（`ubi_bgt`）也可以按 RT 优先级调度。这改变的是等待与抢占行为，不改变 NAND 的页/块语义；长耗时的擦除/编程等待仍应交给中断或工作队列，避免在原子上下文里自旋。

## 术语速查

| 术语 | 含义 |
|------|------|
| Cell | 最小存储单元，1–4 bit |
| Page | 读/编程单位，对应一根 wordline |
| Block | 擦除单位，共享衬底的一组 page |
| Plane | Die 内可并行的子单元，拥有独立 Page Register |
| Die/LUN | 可独立执行命令、独立上报 Ready/Busy 的芯片单元 |
| Channel | 一组共享数据/控制信号的通道 |
| OOB/Spare | 页内数据区之外的冗余区，存放 ECC、坏块标记、元数据 |
| ECC | 纠错码，BCH/LDPC 等 |
| FTL | 闪存转换层，负责映射、GC、磨损均衡、掉电恢复 |
| GC | 垃圾回收，搬移有效页并擦除 victim block |
| WA | 写放大，实际写入介质的数据量 / 主机写入量 |
| P/E | Program/Erase，一次编程+擦除循环 |
| Read disturb | 读操作对未选中页的轻微编程效应 |
| Retention | 电荷随时间/温度泄漏导致的数据保持能力下降 |

## 延伸阅读

- [闪存物理特性](physical.md)：浮栅/电荷陷阱、wordline/string、SLC/MLC/TLC 的物理区别；
- [闪存内部结构原理](arch.md)：page/OOB 布局、block 组织、plane 并行、disturb、GC 与磨损均衡；
- [命令](cmd.md)：ONFI 命令集、地址周期、读/写/擦除时序、状态寄存器；
- [FTL](ftl.md)：地址映射、异地更新、GC、磨损均衡、掉电恢复；
- [NVMe 简介](../nvme原理/intro.md)：SSD 主机侧协议与内核驱动。