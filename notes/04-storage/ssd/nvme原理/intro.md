# NVMe 简介

## nvme是什么
NVMe是一种Host与SSD之间通讯的协议, 它在协议栈中隶属高层, 类比tcp/ip协议.

## nvme和pcie总线关系
nvme over pcie, nvme协议使用pcie bar0和bar1两个寄存器表示一个64位地址,指示nvme协议寄存器地址.
ioremap之后, nvme协议的管理控制类命令执行过程通过主机内存访问方式, admin和io队列中的命令通过dma方式传递给nvme controller, nvme设备获取数据通过命令中的dma地址执行dma操作.

## nvme over fabric
nvme协议还有RDMA/TCP等载体.

## 为什么需要 NVMe

要理解 NVMe，先要理解它取代的是什么。SATA/AHCI 诞生于机械硬盘时代，其设计假设是“一次只能有一个请求在飞、寻道时间占主导”。AHCI 只提供 **1 个命令队列、32 个命令槽**，中断也只有一条（或少量 MSI 向量）。对机械盘来说这足够了，因为磁头本身就是串行的。

但 SSD 内部是高度并行的：多个 channel、多个 die、每个 die 多个 plane，可以同时服务几十甚至上百个请求。AHCI 的单队列很快成为瓶颈：主机侧的锁竞争、单一中断、队列深度不足，让 SSD 的内部并行性无法发挥。

| 维度 | AHCI / SATA | NVMe / PCIe |
|------|-------------|-------------|
| 队列数量 | 1 个命令队列 | 最多 64K 个 I/O 队列 + 1 个 Admin 队列 |
| 队列深度 | 32 | 每队列最多 64K |
| 中断 | 1 条（或少量 MSI） | MSI-X，最多 2048 个向量 |
| 命令格式 | ATA 命令 + PRDT | 64B SQE + 16B CQE，PRP/SGL 寻址 |
| 数据路径 | ATA 寄存器、PIO/DMA | MMIO 寄存器 + 主机内存队列 + DMA |
| 软件栈 | 文件系统 → SCSI/ATA 翻译 → AHCI | 文件系统 → blk-mq → NVMe，无 SCSI 翻译 |
| 典型链路 | SATA 6 Gb/s | PCIe Gen3/4/5 x4，带宽高一个数量级 |
| 典型 4K 随机读延迟 | 50–100 µs | 10–20 µs，优化后可更低 |

NVMe 的收益不只是“带宽更大”，更重要的是**把队列放到主机内存里，用 doorbell 通知控制器**：

- 命令提交不需要写设备寄存器里的命令块，只需要写主机内存 + 一次 doorbell MMIO；
- 完成也不需要读设备寄存器，控制器把 CQE 写回主机内存并发中断；
- 每个 CPU 可以有独立的队列，避免全局锁；
- 队列深度足够大，可以同时把大量请求压给 SSD，让内部并行性跑满。

可以用一个粗略的模型理解队列深度的价值：

```text
IOPS ≈ 队列深度 QD / 单次 I/O 延迟
```

当延迟固定时，QD 越大，能同时“在飞”的请求越多，IOPS 越高；但 QD 超过设备内部并行能力后，收益会饱和，延迟反而上升。

## NVMe over PCIe 传输层

“NVMe 是协议，PCIe 是传输”是理解两者的关键。NVMe 规范定义了命令、完成、队列、namespace 等语义；PCIe 传输层定义了这些队列和寄存器如何映射到 PCIe 设备的 MMIO 空间与 DMA。

```text
+--------------------------------------------------+
| 应用 / 文件系统                                   |
+--------------------------------------------------+
| 块层 blk-mq                                      |
+--------------------------------------------------+
| NVMe 核心：命令、完成、namespace、错误处理        |
+--------------------------------------------------+
| NVMe over PCIe：BAR、doorbell、MSI-X、PRP/SGL     |
+--------------------------------------------------+
| PCIe：TLP、DMA、配置空间                          |
+--------------------------------------------------+
| NVMe SSD 控制器 + NAND                            |
+--------------------------------------------------+
```

### BAR 与寄存器

NVMe 控制器通过 PCIe 的 BAR（Base Address Register）暴露一组 MMIO 寄存器。规范使用 BAR0/BAR1 组成 64 位地址：BAR0 是低 32 位，BAR1 是高 32 位；现代设备也可能只用一个 64 位 BAR。内核 `pci_iomap()` 之后，`dev->bar` 指向这段映射，`dev->dbs = dev->bar + 4096` 指向 doorbell 区。

| 偏移 | 寄存器 | 作用 |
|------|--------|------|
| `0x0000` | CAP | 控制器能力：MQES、CQR、DSTRD、MPSMIN/MAX 等 |
| `0x0008` | VS | 支持的 NVMe 版本 |
| `0x000C` | INTMS | 中断掩码置位 |
| `0x0010` | INTMC | 中断掩码清除 |
| `0x0014` | CC | 控制器配置：EN、CSS、MPS、IOSQES、IOCQES |
| `0x001C` | CSTS | 控制器状态：RDY、CFS、SHST |
| `0x0024` | AQA | Admin 队列属性：ASQS、ACQS |
| `0x0028` | ASQ | Admin SQ 的物理基址 |
| `0x0030` | ACQ | Admin CQ 的物理基址 |
| `0x1000+` | Doorbell | 每个队列的 SQ Tail / CQ Head |

注意这里没有 ATA 那样的命令寄存器、状态寄存器、数据端口。NVMe 把“命令”和“数据”都放进主机内存，设备通过 DMA 去取，这是它与 AHCI 最根本的区别。

### 启动顺序

控制器上电后并不是立刻可用，需要主机按规范初始化：

```text
1. 读 CAP，得到 MQES、DSTRD、MPSMIN/MAX、CSS 等能力
2. 写 CC.EN = 0，等待 CSTS.RDY = 0（复位控制器）
3. 配置 AQA（Admin 队列深度）
4. 写 ASQ / ACQ（Admin 队列物理地址）
5. 配置 CC：CSS=NVM、MPS=4K、IOSQES=6、IOCQES=4
6. 写 CC.EN = 1，等待 CSTS.RDY = 1
7. 通过 Admin 命令 Identify、Set Features、Create CQ/SQ
8. 建立 I/O 队列，注册 namespace，暴露块设备
```

这套流程在内核中对应 `nvme_pci_enable()` → `nvme_pci_configure_admin_queue()` → `nvme_setup_io_queues()`，详见 [NVMe 设备基本结构](arch.md)。

### NVMe over Fabrics

PCIe 只是其中一种传输。NVMe 还可以跑在 RDMA、TCP、Fibre Channel 上，统称 NVMe-oF。区别在于：

| 传输 | 队列/命令如何传递 | 典型场景 |
|------|-------------------|----------|
| PCIe | 主机内存 SQ/CQ + BAR doorbell + MSI-X | 本地 SSD |
| RDMA | 队列映射为 RDMA QP，命令封装为 capsule | 数据中心低延迟网络存储 |
| TCP | 命令封装为 capsule，走 TCP 连接 | 通用网络、云环境 |
| FC | 命令封装为 FC 帧 | 传统 SAN |

Fabrics 传输多了一层“连接（Connect）”命令与 keep-alive 机制，但上层的命令集、完成语义与 namespace 概念保持一致。内核中 `drivers/nvme/host/tcp.c`、`rdma.c`、`fc.c` 分别实现这三种传输。

## 规范的组织方式

NVMe 规范不是一份文档，而是一组文档。2.x 把规范组织为 Base、Command Set、Transport 三族：Base 定义控制器/队列/命令完成等通用语义，Command Set 定义具体操作码，Transport 定义队列与命令如何承载。

| 文档 | 内容 | 与内核的对应 |
|------|------|--------------|
| Base Specification | 控制器、队列、命令/完成、寄存器、错误、复位 | `include/linux/nvme.h`、`drivers/nvme/host/core.c` |
| NVM Command Set | Read/Write/Flush/DSM/Write Zeroes/Compare 等 | `nvme_setup_cmd()`、`nvme_cmd_*` |
| Zoned Namespace Command Set | Zone Append、Zone Management | `drivers/nvme/host/zns.c` |
| PCIe Transport | BAR、doorbell、MSI-X、PRP/SGL | `drivers/nvme/host/pci.c` |
| RDMA/TCP/FC Transport | capsule、连接、队列映射 | `drivers/nvme/host/{rdma,tcp,fc}.c` |
| NVMe-MI | 带外管理 | 用户态工具/管理控制器 |
| Boot / KV 等 | 专用命令集 | 按需实现 |

阅读规范时建议按这个顺序：

1. **Base 的寄存器与队列章节**：理解 CC/CSTS/AQA/ASQ/ACQ 与 SQ/CQ/doorbell；
2. **Base 的命令与完成章节**：64B SQE、16B CQE、phase bit、状态码；
3. **NVM Command Set**：Read/Write/Flush 的字段；
4. **PCIe Transport**：PRP/SGL 与 MSI-X；
5. **Identify / Features / 日志页**：调试与带内管理。

内核头文件 `include/linux/nvme.h` 基本是规范的“C 语言翻译”，把结构体与规范字段对照着看效率最高。

## Namespace：主机看到的逻辑磁盘

Namespace（命名空间）是 NVMe 对“一块可寻址的 LBA 空间”的抽象。主机读写的是 namespace 内的 LBA，而不是 NAND 的物理页。

```text
NVMe Controller
├─ Namespace 1（NSID=1）：/dev/nvme0n1，LBA 0 .. N-1
├─ Namespace 2（NSID=2）：/dev/nvme0n2
└─ ...（可选）
```

关键概念：

| 概念 | 说明 |
|------|------|
| NSID | Namespace Identifier，命令中的 `nsid` 字段 |
| LBA | 逻辑块地址，从 0 开始 |
| LBA Format | 逻辑块大小、元数据大小、相对性能，由 Identify Namespace 返回 |
| FLBAS | 当前使用的 LBA Format 索引 |
| NSZE / NCAP / NUSE | 总块数 / 可用块数 / 已用块数 |
| Attach / Detach | 把 namespace 挂到/摘离控制器 |
| Shared Namespace | 多个控制器共享同一 namespace，用于双端口/多路径 |

一个 namespace 可以有多种 LBA Format，例如 512B、4KB、带 8B 元数据的 4KB 等。`flbas & 0xf` 选择当前格式，`lbaf[].ds` 是 `2^ds` 字节。内核在 `nvme_ns` 中保存这些信息，并在 `nvme_alloc_ns()` 时创建 `gendisk`，最终出现 `/dev/nvme0n1`。

Namespace 与分区表的区别：

- **namespace 由控制器/固件管理**，是设备级的划分；创建/删除需要 Namespace Management 命令（可选特性）；
- **分区表在 LBA 空间内部**，由操作系统管理，设备并不感知；
- namespace 可以被多个控制器共享（多路径），分区表不能。

ZNS（Zoned Namespace）是另一种 namespace 类型：它把 LBA 空间划分为 zone，要求顺序写、显式 reset，以降低 FTL 的 GC 开销。内核在 `drivers/nvme/host/zns.c` 中支持。

## 队列模型概览

NVMe 的队列模型可以概括为“**一对 SQ/CQ + 一个 doorbell**”：

```text
主机内存                                  控制器
┌─────────────────────────┐
│ SQ（Submission Queue）  │  ← 控制器 DMA 读取命令
│ 64B × depth             │
└─────────────────────────┘
┌─────────────────────────┐
│ CQ（Completion Queue）  │  → 控制器 DMA 写完成项
│ 16B × depth             │
└─────────────────────────┘
        ▲            │
        │            ▼
   CQ Head DB    SQ Tail DB（BAR 中的 doorbell）
```

| 组件 | 位置 | 谁写 | 谁读 |
|------|------|------|------|
| SQ | 主机内存 | 主机写 SQE，更新 tail | 控制器 DMA 读 |
| CQ | 主机内存 | 控制器写 CQE，更新 tail | 主机读 |
| SQ Tail Doorbell | 控制器 BAR | 主机 | 控制器 |
| CQ Head Doorbell | 控制器 BAR | 主机 | 控制器 |
| SQ Head | 控制器内部 | 控制器 | 通过 CQE 的 `sq_head` 回传 |
| CQ Tail | 控制器内部 | 控制器 | 主机通过 phase bit 推断 |

队列数量与用途：

| 队列 | qid | 数量 | 用途 |
|------|-----|------|------|
| Admin SQ/CQ | 0 | 1 对 | Identify、Create/Delete 队列、日志、固件升级 |
| I/O SQ/CQ | 1..N | 最多 64K 对 | Read/Write/Flush/DSM 等 |

为什么要多队列？

- **无锁**：每个 CPU 或每个 blk-mq hw queue 用自己的 SQ，避免全局锁；
- **缓存局部性**：SQE/CQE 在本 CPU 的缓存里，减少跨核同步；
- **中断亲和**：每个 CQ 绑定一个 MSI-X 向量，可以定向到提交命令的 CPU；
- **匹配 SSD 并行性**：多个队列可以同时压给控制器的不同内部单元。

在 Linux 中，blk-mq 的 hw queue 与 NVMe 的 I/O 队列一一对应，`nvme_queue_rq()` 在对应的队列上提交命令。具体队列深度、doorbell 与完成处理见 [NVMe 设备基本结构](arch.md)，命令格式见 [NVMe 命令](cmd.md)。

## 内核中的 NVMe 驱动

Linux 的 NVMe 支持分为 host 与 target 两部分：

```text
drivers/nvme/
├─ host/                主机侧：把 NVMe 设备变成块设备
│  ├─ core.c            命令构造、namespace、字符设备、ioctl、错误恢复
│  ├─ pci.c             PCIe 传输：BAR、队列、doorbell、MSI-X
│  ├─ nvme.h            nvme_ctrl / nvme_ns / nvme_queue 等
│  ├─ fabrics.c         NVMe-oF 公共逻辑
│  ├─ tcp.c / rdma.c / fc.c   NVMe-oF 各传输
│  ├─ multipath.c       多路径
│  ├─ zns.c             Zoned Namespace
│  └─ sysfs.c / ioctl.c / hwmon.c
└─ target/              目标侧：把本机存储导出为 NVMe
   ├─ core.c            nvmet 核心
   ├─ loop.c            本地 loop 传输（调试用）
   ├─ tcp.c / rdma.c / fc.c
   ├─ pci-epf.c         PCIe Endpoint Function
   └─ configfs.c        configfs 配置接口
```

### 探测与初始化

以 PCIe SSD 为例，`nvme_probe()` 的主线是：

1. 使能 PCI 设备、映射 BAR；
2. `nvme_pci_enable()`：读 CAP，计算 `q_depth = min(CAP.MQES + 1, io_queue_depth)`，其中 `io_queue_depth` 默认 1024，可模块参数调整；
3. `nvme_pci_configure_admin_queue()`：复位控制器、配置 AQA/ASQ/ACQ、置 CC.EN，等待 CSTS.RDY；
4. `nvme_init_ctrl()` / Identify：读取控制器与 namespace 信息；
5. `nvme_setup_io_queues()`：申请 MSI-X 向量，通过 Admin 命令 Create CQ/SQ；
6. 注册 blk-mq tagset，`nvme_alloc_ns()` 创建 gendisk。

### 数据路径

| 阶段 | 函数 | 说明 |
|------|------|------|
| 块层下发 | `nvme_queue_rq()` | blk-mq 回调 |
| 构造命令 | `nvme_setup_cmd()` | 填充 64B SQE |
| 映射数据 | `nvme_map_data()` | PRP/SGL + DMA map |
| 写 SQ | `nvme_sq_copy_cmd()` | 拷贝 SQE 到主机内存队列 |
| 敲门铃 | `nvme_write_sq_db()` | 写 SQ Tail Doorbell |
| 完成中断 | `nvme_irq()` | MSI-X 处理 |
| 处理 CQE | `nvme_poll_cq()` / `nvme_handle_cqe()` | 检查 phase、完成请求 |
| 回收 | `nvme_ring_cq_doorbell()` | 写 CQ Head Doorbell |

### 配置与工具

| 配置项 | 作用 |
|--------|------|
| `CONFIG_BLK_DEV_NVME` | PCIe NVMe 主机驱动 |
| `CONFIG_NVME_CORE` | NVMe 核心 |
| `CONFIG_NVME_MULTIPATH` | 多路径 |
| `CONFIG_NVME_TARGET` | 目标侧（导出） |
| `CONFIG_NVME_TARGET_TCP` / `_RDMA` / `_FC` | 各 Fabrics 目标 |
| `CONFIG_NVME_VERBOSE_ERRORS` | 可读错误字符串 |

用户态常用 `nvme-cli`：

```bash
nvme list                 # 列出控制器与 namespace
nvme id-ctrl /dev/nvme0   # Identify Controller
nvme id-ns /dev/nvme0n1   # Identify Namespace
nvme smart-log /dev/nvme0 # SMART/健康日志
nvme get-feature /dev/nvme0 -f 0x07   # Number of Queues
```

### 与 RT 内核的关系

在 `CONFIG_PREEMPT_RT=y` 的内核上，NVMe 的 MSI-X 中断默认会线程化，中断处理不再是硬中断上下文；这对降低尾延迟有帮助，但也意味着完成路径多了一次线程唤醒。对延迟极敏感的场景可以使用 poll queue（`poll_queues` 模块参数）或 io_uring 的 I/O polling，绕过中断等待。

队列深度 `io_queue_depth`、写队列数 `write_queues`、poll 队列数 `poll_queues` 都是 `pci.c` 的模块参数，可按负载调整。

## 学习路径

1. 先读本篇，建立“协议 / 传输 / namespace / 队列”的整体框架；
2. 读 [NVMe 设备基本结构](arch.md)，理解控制器、SQ/CQ、doorbell、PRP/SGL 与命令执行全流程；
3. 读 [NVMe 数据结构定义](define.md)，对照 `include/linux/nvme.h` 与 `drivers/nvme/host/pci.c`；
4. 读 [NVMe 命令](cmd.md)，掌握 Admin 与 NVM I/O 命令的操作码与字段；
5. 读 [NVMe PCIe 主机驱动分析](../../nvme/nvme-pcie-host-analysis.md)，沿着 `nvme_probe()` → `nvme_queue_rq()` → `nvme_irq()` 走一遍代码。

## 小结

- NVMe 是为闪存设计的**命令协议**，PCIe 只是它的一种传输；
- 多队列、大深度、MSI-X、doorbell + 主机内存队列，是它取代 AHCI 的核心原因；
- namespace 是主机看到的 LBA 空间，与 NAND 的物理结构解耦；
- Linux 主机侧在 `drivers/nvme/host/`，目标侧在 `drivers/nvme/target/`；
- 理解 NVMe 的关键是把“队列对 + doorbell + 命令/完成”这条链路走通。

相关文档：[NVMe 设备基本结构](arch.md)、[NVMe 数据结构定义](define.md)、[NVMe 命令](cmd.md)、[NVMe PCIe 主机驱动分析](../../nvme/nvme-pcie-host-analysis.md)。
