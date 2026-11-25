# ARM64 中断子系统分析

## 目录

1. [概述](#1-概述)
2. [ARM64 异常处理基础](#2-arm64-异常处理基础)
3. [GICv3 硬件架构](#3-gicv3-硬件架构)
4. [GICv3 驱动分析](#4-gicv3-驱动分析)
5. [GICv3 ITS 与 MSI 中断](#5-gicv3-its-与-msi-中断)
6. [NVMe MSI-X 中断分析](#6-nvme-msi-x-中断分析)
7. [核心 IRQ 子系统](#7-核心-irq-子系统)
8. [完整中断处理流程](#8-完整中断处理流程)
9. [关键数据结构汇总](#9-关键数据结构汇总)
10. [文件清单](#10-文件清单)

---

## 1. 概述

ARM64 中断子系统由以下层次构成：

```
硬件层: 外设 → GIC Distributor → GIC Redistributor/CPU Interface → CPU Core
驱动层: 外设驱动 → IRQ Domain → GIC驱动 → irq_chip → 异常向量表
核心层: request_irq → irq_desc → handle_irq_desc → generic_handle_domain_irq
```

以 NVMe 设备的 MSI-X 中断为例，完整路径为：

```
NVMe设备 → PCIe MSI-X → GIC ITS → GIC Redistributor → CPU Core
    → 异常向量表 → gic_handle_irq → generic_handle_domain_irq → nvme_irq
```

---

## 2. ARM64 异常处理基础

### 2.1 异常向量表

ARM64 架构定义了 4 组异常向量表，每组包含 4 个入口（同步、IRQ、FIQ、Error），共 16 个入口，每个入口 128 字节（2^7 对齐）。

- **第一组 (EL1t)**：异常发生在 EL1，使用 SP_EL0 栈指针
- **第二组 (EL1h)**：异常发生在 EL1，使用 SP_EL1 栈指针（内核态中断使用此组）
- **第三组 (EL0t_64)**：异常从 EL0 的 AArch64 状态发生（用户态中断）
- **第四组 (EL0t_32)**：异常从 EL0 的 AArch32 状态发生

#### 向量表定义 (`arch/arm64/kernel/entry.S`)

```asm
.align 11
SYM_CODE_START(vectors)
    kernel_ventry   1, t, 64, sync       // Synchronous EL1t
    kernel_ventry   1, t, 64, irq        // IRQ EL1t
    kernel_ventry   1, t, 64, fiq        // FIQ EL1t
    kernel_ventry   1, t, 64, error      // Error EL1t

    kernel_ventry   1, h, 64, sync       // Synchronous EL1h
    kernel_ventry   1, h, 64, irq        // IRQ EL1h    ← 内核态IRQ入口
    kernel_ventry   1, h, 64, fiq        // FIQ EL1h
    kernel_ventry   1, h, 64, error      // Error EL1h

    kernel_ventry   0, t, 64, sync       // Synchronous 64-bit EL0
    kernel_ventry   0, t, 64, irq        // IRQ 64-bit EL0  ← 用户态IRQ入口
    kernel_ventry   0, t, 64, fiq        // FIQ 64-bit EL0
    kernel_ventry   0, t, 64, error      // Error 64-bit EL0

    kernel_ventry   0, t, 32, sync       // Synchronous 32-bit EL0
    kernel_ventry   0, t, 32, irq        // IRQ 32-bit EL0
    kernel_ventry   0, t, 32, fiq        // FIQ 32-bit EL0
    kernel_ventry   0, t, 32, error      // Error 32-bit EL0
SYM_CODE_END(vectors)
```

#### `kernel_ventry` 宏工作流程

1. 检查栈溢出（SP 与 THREAD_SHIFT 对齐检查）
2. 分配 `PT_REGS_SIZE` 栈空间保存现场
3. 跳转到 `el{el}{ht}_{regsize}_{label}_handler`

### 2.2 异常发生时 CPU 自动完成的操作

1. 将 PSTATE 保存到 SPSR_ELx
2. 将返回地址保存到 ELR_ELx
3. 设置 PSTATE.DAIF = 1（屏蔽调试、SError、IRQ、FIQ）
4. 切换 SP 到 SP_ELx
5. 根据异常类型跳转到向量表对应入口

### 2.3 异常返回（eret 指令）

1. 从 ELR_ELx 恢复 PC
2. 从 SPSR_ELx 恢复 PSTATE

### 2.4 IRQ 处理入口函数调用链

```
el1h_64_irq_handler (entry.S 中的宏展开)
  → el1_interrupt(regs, handle_arch_irq)  (entry-common.c)
    → __el1_irq(regs, handler)
      → enter_from_kernel_mode(regs)   // irqentry_enter
      → do_interrupt_handler(regs, handler)
        → call_on_irq_stack(regs, handler)  // 切到IRQ栈
          → handler(regs)  // = handle_arch_irq = gic_handle_irq
      → exit_to_kernel_mode(regs, state)  // irqentry_exit
```

用户态中断路径类似：
```
el0t_64_irq_handler
  → el0_interrupt(regs, handle_arch_irq)
    → __el0_irq_handler_common
      → arm64_enter_from_user_mode(regs)  // context_tracking
      → do_interrupt_handler(regs, handle_arch_irq)
      → arm64_exit_to_user_mode(regs)
```

### 2.5 `set_handle_irq` 注册根中断处理器

```c
// arch/arm64/kernel/irq.c
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init = default_handle_irq;

int __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
    if (handle_arch_irq != default_handle_irq)
        return -EBUSY;
    handle_arch_irq = handle_irq;
    pr_info("Root IRQ handler: %ps\n", handle_irq);
    return 0;
}
```

由 GIC 驱动在初始化时调用 `set_handle_irq(gic_handle_irq)` 注册。

---

## 3. GICv3 硬件架构

### 3.1 GICv3 组成

GICv3 (Generic Interrupt Controller v3) 由以下组件构成：

| 组件 | 功能 | 寄存器基址 |
|------|------|-----------|
| **Distributor (GICD)** | 全局中断分配、SPI 管理 | `GICD_*` 寄存器 |
| **Redistributor (GICR)** | 每 CPU 一个，SGI/PPI/LPI 管理 | `GICR_*` 寄存器 |
| **CPU Interface** | CPU 接口，通过系统寄存器访问 | `ICC_*_EL1` 系统寄存器 |
| **ITS (可选)** | MSI 中断翻译 | `GITS_*` 寄存器 |

### 3.2 中断类型

| 类型 | ID 范围 | 说明 |
|------|---------|------|
| **SGI** (Software Generated Interrupt) | 0-15 | 软件触发，用于 IPI |
| **PPI** (Private Peripheral Interrupt) | 16-31 | 每 CPU 私有外设中断 |
| **SPI** (Shared Peripheral Interrupt) | 32-1019 | 共享外设中断，可路由到任意 CPU |
| **ESPI** (Extended SPI) | 4096-5119 | 扩展 SPI |
| **EPPI** (Extended PPI) | 1056-1119 | 扩展 PPI |
| **LPI** (Locality-specific Peripheral Interrupt) | 8192-16777215 | 基于消息的中断，通过 ITS 配置 |

### 3.3 GICv3 关键寄存器

#### Distributor 寄存器 (MMIO)

| 寄存器 | 偏移 | 描述 |
|--------|------|------|
| `GICD_CTLR` | 0x0000 | 控制器配置（ARE, Enable, DS） |
| `GICD_TYPER` | 0x0004 | 类型寄存器（SPI 数量、LPI 支持等） |
| `GICD_IIDR` | 0x0008 | 实现者 ID |
| `GICD_IGROUPRn` | 0x0080 | 中断组寄存器（Group 0/1） |
| `GICD_ISENABLERn` | 0x0100 | 中断设置使能 |
| `GICD_ICENABLERn` | 0x0180 | 中断清除使能 |
| `GICD_ISPENDRn` | 0x0200 | 中断设置挂起 |
| `GICD_ICPENDRn` | 0x0280 | 中断清除挂起 |
| `GICD_ISACTIVERn` | 0x0300 | 中断设置 Active |
| `GICD_ICACTIVERn` | 0x0380 | 中断清除 Active |
| `GICD_IPRIORITYRn` | 0x0400 | 中断优先级 |
| `GICD_ITARGETSRn` | 0x0800 | 中断目标 CPU（GICv2 兼容） |
| `GICD_ICFGRn` | 0x0C00 | 中断配置（电平/边沿触发） |
| `GICD_IROUTERn` | 0x6000 | 中断路由（GICv3 特有） |

#### Redistributor 寄存器 (MMIO，每 CPU 一套)

| 寄存器 | 偏移 | 描述 |
|--------|------|------|
| `GICR_CTLR` | 0x0000 | Redistributor 控制 |
| `GICR_IIDR` | 0x0004 | 实现者 ID |
| `GICR_TYPER` | 0x0008 | 类型（关联的 CPU 等信息） |
| `GICR_WAKER` | 0x0014 | 唤醒控制 |
| `GICR_IGROUPR0` | 0x0080 | SGI/PPI 组寄存器 |
| `GICR_ISENABLER0` | 0x0100 | SGI/PPI 设置使能 |
| `GICR_ICENABLER0` | 0x0180 | SGI/PPI 清除使能 |
| `GICR_ISPENDR0` | 0x0200 | SGI/PPI 设置挂起 |
| `GICR_ICPENDR0` | 0x0280 | SGI/PPI 清除挂起 |
| `GICR_ISACTIVER0` | 0x0300 | SGI/PPI 设置 Active |
| `GICR_ICACTIVER0` | 0x0380 | SGI/PPI 清除 Active |
| `GICR_IPRIORITYR0` | 0x0400 | SGI/PPI 优先级 |
| `GICR_ICFGR0` | 0x0C00 | SGI/PPI 配置 |
| `GICR_IGRPMODR0` | 0x0D00 | SGI/PPI 组模式 |

#### CPU Interface 系统寄存器 (通过 `mrs`/`msr` 访问)

| 寄存器 | 描述 |
|--------|------|
| `ICC_IAR_EL1` | 中断应答寄存器（读取获取中断 ID） |
| `ICC_EOIR_EL1` | 中断结束寄存器（写回表示处理完成） |
| `ICC_PMR_EL1` | 优先级掩码寄存器 |
| `ICC_BPR_EL1` | 二进制点寄存器（优先级分组） |
| `ICC_RPR_EL1` | 当前运行优先级寄存器 |
| `ICC_CTLR_EL1` | CPU 接口控制寄存器 |
| `ICC_SRE_EL1` | 系统寄存器使能 |
| `ICC_IGRPEN1_EL1` | Group 1 中断使能 |
| `ICC_SGI1R_EL1` | SGI 触发寄存器 |

### 3.4 GICv3 中断路由

SPI 的路由通过 `GICD_IROUTERn` 寄存器配置，支持：
- 路由到指定 CPU
- 路由到 CPU 组（任意 CPU 组中的 CPU）
- 路由到任何 CPU

PPI/SGI 始终路由到本地 CPU。

### 3.5 GICv3 中断优先级

GICv3 支持 8-32 级优先级（由 `ICC_CTLR_EL1.PRI_BITS` 决定），Linux 通常只使用两级：
- `GICV3_PRIO_IRQ` (0xa0)：普通 IRQ 优先级
- `GICV3_PRIO_NMI` (0x80)：伪 NMI 优先级

PMR (Priority Mask Register) 的作用：
- 写 PMR 可以屏蔽低于该优先级的所有中断
- Linux 在 IRQ 处理期间将 PMR 设为 `DEFAULT_PMR_VALUE` (0xf0)

---

## 4. GICv3 驱动分析

### 4.1 关键数据结构

#### `gic_chip_data` - GICv3 控制器实例

```c
// drivers/irqchip/irq-gic-v3.c
struct gic_chip_data {
    struct fwnode_handle    *fwnode;        // 固件节点句柄
    phys_addr_t             dist_phys_base; // Distributor 物理地址
    void __iomem            *dist_base;     // Distributor 虚拟地址
    struct redist_region    *redist_regions; // Redistributor 区域
    struct rdists           rdists;         // Redistributor 信息
    struct irq_domain       *domain;        // IRQ 域
    u64                     redist_stride;  // Redistributor 步长
    u32                     nr_redist_regions;
    u64                     flags;          // 工作区标志
    bool                    has_rss;
    unsigned int            ppi_nr;         // PPI 数量
    struct partition_affinity *parts;
    unsigned int            nr_parts;
};
```

#### `gic_chip` - IRQ Chip 定义

```c
static struct irq_chip gic_chip = {
    .name               = "GICv3",
    .irq_mask           = gic_mask_irq,
    .irq_unmask         = gic_unmask_irq,
    .irq_eoi            = gic_eoi_irq,
    .irq_set_type       = gic_set_type,
    .irq_set_affinity   = gic_set_affinity,
    .irq_retrigger      = gic_retrigger,
    .irq_get_irqchip_state = gic_irq_get_irqchip_state,
    .irq_set_irqchip_state = gic_irq_set_irqchip_state,
    .irq_nmi_setup      = gic_irq_nmi_setup,
    .irq_nmi_teardown   = gic_irq_nmi_teardown,
    .ipi_send_mask      = gic_ipi_send_mask,
    .flags              = IRQCHIP_SET_TYPE_MASKED |
                          IRQCHIP_SKIP_SET_WAKE |
                          IRQCHIP_MASK_ON_SUSPEND,
};
```

`gic_eoimode1_chip` 使用 `gic_eoimode1_eoi_irq`（拆分的 EOI/Deactivate），当支持 EOImode=1 时使用。

### 4.2 初始化流程

#### 4.2.1 设备树初始化路径

```
start_kernel (init/main.c)
  → init_IRQ() (arch/arm64/kernel/irq.c)
    → irqchip_init() (drivers/irqchip/irqchip.c)
      → of_irq_init(__irqchip_of_table)
        → gic_of_init() (drivers/irqchip/irq-gic-v3.c)
```

#### 4.2.2 `gic_of_init` 详细流程

```c
static int __init gic_of_init(struct device_node *node, struct device_node *parent)
{
    // 1. 解析 Distributor 寄存器地址
    // 2. 解析 Redistributor 区域
    // 3. 收集 Redistributor 基址

    // 4. 调用 gic_init_bases
    err = gic_init_bases(dist_phys_base, dist_base,
                         rdist_regions, nr_redist_regions,
                         redist_stride, handle);
    // 5. KVM 信息设置
    gic_of_setup_kvm_info(node);

    return err;
}
```

#### 4.2.3 `gic_init_bases` 详细流程

```c
static int __init gic_init_bases(phys_addr_t dist_phys_base,
                                 void __iomem *dist_base,
                                 struct redist_region *rdist_regs,
                                 u32 nr_redist_regions,
                                 u64 redist_stride,
                                 struct fwnode_handle *handle)
{
    // 1. 保存基址信息到 gic_data
    gic_data.fwnode = handle;
    gic_data.dist_phys_base = dist_phys_base;
    gic_data.dist_base = dist_base;
    gic_data.redist_regions = rdist_regs;
    gic_data.nr_redist_regions = nr_redist_regions;
    gic_data.redist_stride = redist_stride;

    // 2. 读取 GICD_TYPER 获取硬件信息
    typer = readl_relaxed(gic_data.dist_base + GICD_TYPER);

    // 3. 应用勘误工作区
    gic_enable_quirks(..., gic_quirks, &gic_data);

    // 4. 创建 IRQ Domain
    gic_data.domain = irq_domain_create_tree(handle, &gic_irq_domain_ops, &gic_data);
    irq_domain_update_bus_token(gic_data.domain, DOMAIN_BUS_WIRED);

    // 5. 初始化 Distributor
    gic_dist_init();

    // 6. 初始化 Redistributor（当前 CPU）
    err = gic_populate_rdist();
    gic_set_redist_base(rdist_regs[0].redist_base, 0);

    // 7. 注册根中断处理器
    set_handle_irq(gic_handle_irq);

    // 8. 初始化 ITS
    its_init(handle, &gic_data.rdists, gic_data.domain, dist_prio_irq);
    its_cpu_init();

    // 9. 每 CPU 初始化
    gic_cpu_init();

    // 10. SMP 初始化（SGI 用于 IPI）
    gic_smp_init();

    return 0;
}
```

#### 4.2.4 `gic_dist_init` - Distributor 初始化

```c
static void __init gic_dist_init(void)
{
    void __iomem *base = gic_data.dist_base;

    // 1. 禁用 Distributor
    writel_relaxed(0, base + GICD_CTLR);
    gic_dist_wait_for_rwp();

    // 2. 配置 SPI 为非安全 Group-1
    for (i = 32; i < GIC_LINE_NR; i += 32)
        writel_relaxed(~0, base + GICD_IGROUPR + i / 8);

    // 3. 配置 ESPI 范围
    for (i = 0; i < GIC_ESPI_NR; i += 32) {
        writel_relaxed(~0U, base + GICD_ICENABLERnE + i / 8);
        writel_relaxed(~0U, base + GICD_ICACTIVERnE + i / 8);
    }

    // 4. 设置优先级
    for (i = 0; i < GIC_ESPI_NR; i += 4)
        writel_relaxed(REPEAT_BYTE_U32(dist_prio_irq),
                       base + GICD_IPRIORITYRnE + i);

    // 5. 配置 SPI 中断
    gic_dist_config(base, GIC_LINE_NR, dist_prio_irq);

    // 6. 启用 Distributor（ARE, Group1）
    val = GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1A | GICD_CTLR_ENABLE_G1;
    writel_relaxed(val, base + GICD_CTLR);
    gic_dist_wait_for_rwp();
}
```

#### 4.2.5 `gic_cpu_init` - 每 CPU 初始化

```c
static void gic_cpu_init(void)
{
    // 1. 获取当前 CPU 的 Redistributor 基址
    gic_populate_rdist();
    gic_enable_redist(true);

    // 2. 配置 SGI/PPI 为非安全 Group-1
    rbase = gic_data_rdist_sgi_base();
    for (i = 0; i < gic_data.ppi_nr + SGI_NR; i += 32)
        writel_relaxed(~0, rbase + GICR_IGROUPR0 + i / 8);

    // 3. 配置 SGI/PPI 优先级
    gic_cpu_config(rbase, gic_data.ppi_nr + SGI_NR, dist_prio_irq);
    gic_redist_wait_for_rwp();

    // 4. 初始化 CPU 接口系统寄存器
    gic_cpu_sys_reg_init();
}
```

### 4.3 GIC 配置函数调用栈

#### 4.3.1 启动阶段完整调用栈

```
start_kernel()
  ├── setup_arch()
  │     └── paging_init()          ← 页表初始化完成，可 ioremap
  │
  └── init_IRQ()                   ← arch/arm64/kernel/irq.c
        └── irqchip_init()         ← drivers/irqchip/irqchip.c
              ├── of_irq_init(__irqchip_of_table)    ← DT 路径
              │     └── gic_of_init()                ← IRQCHIP_DECLARE(gic_v3, "arm,gic-v3")
              │           ├── gic_of_iomap(node, 0, "GICD", &res)   ← 映射 Distributor 寄存器
              │           ├── gic_validate_dist_version(dist_base)  ← 读取 GICD_PIDR2 验证 GICv3
              │           ├── gic_of_iomap(node, 1+i, "GICR", &res) ← 映射 Redistributor 区域
              │           ├── gic_enable_of_quirks()                ← 应用设备树勘误
              │           ├── gic_init_bases(dist_phys_base, dist_base, ...)  ← 核心初始化
              │           │     ├── 保存基址到 gic_data
              │           │     ├── readl_relaxed(base + GICD_TYPER)         ← 读取硬件能力
              │           │     ├── gic_enable_quirks(GICD_IIDR, ...)        ← 应用 IIDR 勘误
              │           │     ├── irq_domain_create_tree(handle, &gic_irq_domain_ops, &gic_data)
              │           │     │     └── __irq_domain_create()              ← 创建 IRQ 映射域
              │           │     ├── irq_domain_update_bus_token(domain, DOMAIN_BUS_WIRED)
              │           │     ├── mbi_init(handle, domain)                 ← 可选: MBI 初始化
              │           │     ├── set_handle_irq(gic_handle_irq)           ← 注册根中断处理器
              │           │     ├── gic_update_rdist_properties()            ← 遍历 Redistributor 收集特性
              │           │     │     └── gic_iterate_rdists(__gic_update_rdist_properties)
              │           │     │           ├── 读取 GICR_TYPER → VLPIS/RVPEID/DirectLPI
              │           │     │           └── 读取 GICR_CTLR.IR
              │           │     ├── gic_cpu_sys_reg_enable()                ← 启用系统寄存器访问
              │           │     │     └── gic_enable_sre()                  ← 设置 ICC_SRE_EL1.SRE=1
              │           │     ├── gic_prio_init()                         ← 初始化优先级掩码
              │           │     ├── gic_dist_init()                         ← Distributor 初始化
              │           │     │     ├── writel_relaxed(0, base + GICD_CTLR)     ← 禁用 Distributor
              │           │     │     ├── gic_dist_wait_for_rwp()           ← 等待同步
              │           │     │     ├── 配置 SPI/ESPI 为 Group-1 (非安全)
              │           │     │     ├── 禁用/去激活所有 ESPI
              │           │     │     ├── 设置 ESPI 优先级
              │           │     │     ├── gic_dist_config(base, GIC_LINE_NR, prio)  ← 公共配置
              │           │     │     │     ├── 设置 SPI 触发模式 (ICFGR: 电平触发低电平有效)
              │           │             │     ├── 设置 SPI 优先级 (IPRIORITYR)
              │           │             │     └── 去激活并禁用所有 SPI (ICACTIVER/ICENABLER)
              │           │             ├── writel_relaxed(ARE_NS|ENABLE_G1A|ENABLE_G1, GICD_CTLR) ← 启用 Distributor
              │           │             ├── gic_dist_wait_for_rwp()
              │           │             └── 设置所有 SPI/ESPI 路由到当前 CPU (GICD_IROUTER)
              │           │     ├── gic_cpu_init()                          ← 当前 CPU Redistributor 初始化
              │           │     │     ├── gic_populate_rdist()              ← 查找当前 CPU 的 Redistributor
              │           │     │     │     └── gic_iterate_rdists(__gic_populate_rdist)
              │           │     │     │           ├── 读取 GICR_TYPER.Last 和 GICR_TYPER.ProcessorNumber
              │           │     │     │           └── 匹配当前 CPU mpidr → 设置 gic_data_rdist_rd_base()
              │           │     │     ├── gic_enable_redist(true)           ← 唤醒 Redistributor
              │           │     │     │     ├── 清除 GICR_WAKER.ProcessorSleep
              │           │     │     │     └── 轮询等待 GICR_WAKER.ChildrenAsleep 清除
              │           │     │     ├── 配置 SGI/PPI 为 Group-1 (GICR_IGROUPR0)
              │           │     │     ├── gic_cpu_config(rbase, nr, prio)   ← 公共 CPU 配置
              │           │     │     │     ├── 禁用所有 SGI/PPI (ICENABLER)
              │           │     │     │     ├── 去激活所有 SGI/PPI (ICACTIVER)
              │           │     │     │     └── 设置 SGI/PPI 优先级 (IPRIORITYR)
              │           │     │     ├── gic_redist_wait_for_rwp()
              │           │     │     └── gic_cpu_sys_reg_init()           ← CPU 接口系统寄存器初始化
              │           │     │           ├── write_gicreg(DEFAULT_PMR_VALUE, ICC_PMR_EL1)  ← 设置优先级掩码
              │           │     │           ├── gic_write_bpr1(0)           ← 设置二进制点寄存器
              │           │     │           ├── gic_write_ctlr(EOImode)     ← 设置 EOI 模式 (mode 0/1)
              │           │     │           ├── 清除优先级组寄存器 (ICC_AP0Rn/ICC_AP1Rn)
              │           │     │           └── write_gicreg(1, ICC_IGRPEN1_EL1)  ← 启用 Group-1 中断
              │           │     ├── gic_enable_nmi_support()               ← Pseudo-NMI 支持
              │           │     ├── gic_smp_init()                          ← SGI/IPI 初始化
              │           │     │     ├── cpuhp_setup_state(CPUHP_BP_PREPARE_DYN, gic_check_rdist)
              │           │     │     ├── cpuhp_setup_state(CPUHP_AP_IRQ_GIC_STARTING, gic_starting_cpu)
              │           │     │     └── irq_domain_alloc_irqs(domain, 8, &sgi_fwspec)  ← 分配 8 个 SGI
              │           │     ├── gic_cpu_pm_init()                      ← CPU 电源管理通知
              │           │     │     └── cpu_pm_register_notifier(&gic_cpu_pm_notifier_block)
              │           │     └── its_init(handle, &rdists, domain, prio)  ← ITS 子系统初始化
              │           │           ├── [ITS 初始化细节见 5.2 节]
              │           │           └── its_cpu_init()                   ← 当前 CPU ITS 初始化
              │           └── gic_of_setup_kvm_info(node)                  ← KVM 支持
              │
              └── acpi_irq_init()                             ← ACPI 路径
                    └── gic_acpi_init()
                          ├── acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_DISTRIBUTOR, ...)
                          ├── gic_acpi_collect_gicr_base()                ← 收集 GICR 基址
                          │     ├── acpi_table_parse_madt(GICR, gic_acpi_parse_madt_redist)
                          │     └── acpi_table_parse_madt(GICC, gic_acpi_parse_madt_gicc)
                          └── gic_init_bases(dist_phys_base, dist_base, ...)  ← 与 DT 路径合并
```

#### 4.3.2 CPU 热插拔配置调用栈

```
CPU hotplug state machine
  └── CPUHP_AP_IRQ_GIC_STARTING
        └── gic_starting_cpu(cpu)
              ├── gic_cpu_sys_reg_enable()         ← 启用系统寄存器 (ICC_SRE_EL1)
              ├── gic_cpu_init()                    ← 每 CPU Redistributor 初始化
              │     ├── gic_populate_rdist()
              │     ├── gic_enable_redist(true)
              │     ├── 配置 SGI/PPI Group-1
              │     ├── gic_cpu_config()
              │     └── gic_cpu_sys_reg_init()
              └── its_cpu_init()                    ← 每 CPU ITS 初始化
                    ├── redist_disable_lpis()       ← 禁用旧 LPIs
                    ├── its_cpu_init_lpis()         ← 初始化 LPI 配置
                    │     ├── 设置 GICR_PROPBASER (LPI 属性表基址)
                    │     ├── 设置 GICR_PENDBASER (LPI 挂起表基址)
                    │     └── 设置 GICR_CTLR.EnableLPI=1
                    └── its_cpu_init_collections()  ← 初始化 ITS 集合
                          └── its_cpu_init_collection(its)
                                ├── 计算 Collection ID = smp_processor_id()
                                └── its_send_its_cmd(its, GITS_CMD_MAPC)  ← 发送 MAPC 命令
```

#### 4.3.3 中断配置函数调用栈

当驱动通过 `irq_set_type()` / `irq_set_affinity()` / `irq_mask()` 配置中断时，调用链如下：

```
irq_set_type(irq, type)                     ← include/linux/irq.h
  └── __irq_set_trigger(desc, flags)
        └── chip->irq_set_type(d, type)
              └── gic_set_type(d, type)      ← drivers/irqchip/irq-gic-v3.c:701
                    ├── get_intid_range(d)    ← 确定中断范围 (SGI/PPI/SPI/ESPI/LPI)
                    ├── gic_irq_in_rdist(d)   ← PPI/EPPI → Redistributor 基址
                    │     └── base = gic_data_rdist_sgi_base()  ← GICR 基址
                    │── base = gic_dist_base_alias(d)           ← SPI/ESPI → Distributor 基址
                    ├── convert_offset_index(d, GICD_ICFGR, &index)  ← 计算寄存器偏移
                    └── gic_configure_irq(index, type, base + offset)  ← 写 ICFGR 寄存器

irq_set_affinity(irq, cpumask)              ← include/linux/irq.h
  └── __irq_do_set_affinity(d, mask, force)
        └── chip->irq_set_affinity(d, mask, force)
              └── gic_set_affinity(d, mask_val, force)  ← drivers/irqchip/irq-gic-v3.c:1428
                    ├── cpumask_any_and(mask, cpu_online_mask)  ← 选择目标 CPU
                    ├── gic_peek_irq(d, GICD_ISENABLER)  ← 检查中断是否已启用
                    ├── gic_mask_irq(d)                   ← 如果已启用，先屏蔽
                    ├── convert_offset_index(d, GICD_IROUTER, &index)
                    ├── gic_write_irouter(val, reg)       ← 写 GICD_IROUTER 设置路由
                    ├── gic_unmask_irq(d)                 ← 重新启用
                    └── irq_data_update_effective_affinity(d, cpumask_of(cpu))

irq_mask(irq) / irq_unmask(irq)
  ├── chip->irq_mask(d) → gic_mask_irq(d)
  │     ├── gic_poke_irq(d, GICD_ICENABLER)  ← 写 ICENABLER 寄存器禁能
  │     └── gic_redist_wait_for_rwp() / gic_dist_wait_for_rwp()
  └── chip->irq_unmask(d) → gic_unmask_irq(d)
        └── gic_poke_irq(d, GICD_ISENABLER)  ← 写 ISENABLER 寄存器使能
```

#### 4.3.4 中断分配函数调用栈 (IRQ Domain Map)

当系统需要为设备分配中断号时 (如 `irq_domain_alloc_irqs`)，调用链如下：

```
irq_domain_alloc_irqs(domain, nr_irqs, node, fwspec)    ← kernel/irq/irqdomain.c
  └── __irq_domain_alloc_irqs(domain, nr_irqs, node, fwspec)
        ├── irq_domain_alloc_descs(virq, nr_irqs, node)  ← 分配 irq_desc
        └── __irq_domain_alloc_irqs(domain, virq, nr_irqs, fwspec)
              └── domain->ops->alloc(domain, virq, nr_irqs, arg)
                    └── gic_irq_domain_alloc(domain, virq, nr_irqs, arg)  ← gic-v3.c:1653
                          ├── gic_irq_domain_translate(domain, fwspec, &hwirq, &type)  ← 解析 fwspec
                          └── for (i = 0; i < nr_irqs; i++)
                                └── gic_irq_domain_map(domain, virq + i, hwirq + i)  ← gic-v3.c:1547
                                      ├── 选择 irq_chip (gic_chip 或 gic_eoimode1_chip)
                                      ├── 根据中断范围选择处理函数:
                                      │     ├── SGI/PPI/EPPI → handle_percpu_devid_irq
                                      │     ├── SPI/ESPI     → handle_fasteoi_irq
                                      │     └── LPI          → handle_fasteoi_irq
                                      └── irq_domain_set_info(d, irq, hw, chip, ...)
                                            └── irq_set_chip_and_handler(irq, chip, handler)
```

#### 4.3.5 CPU 电源管理路径

当 CPU 从电源管理状态恢复时，通过 `gic_cpu_pm_notifier` 重新配置 GIC：

```
CPU_PM_EXIT / CPU_PM_ENTER_FAILED
  └── gic_cpu_pm_notifier(cmd, v)              ← gic-v3.c:1482
        ├── gic_enable_redist(true)              ← 唤醒 Redistributor (如安全禁用)
        ├── gic_cpu_sys_reg_enable()             ← 重新启用系统寄存器
        └── gic_cpu_sys_reg_init()               ← 重新初始化 CPU 接口寄存器

CPU_PM_ENTER (且安全禁用)
  └── gic_cpu_pm_notifier(cmd, v)
        ├── gic_write_grpen1(0)                  ← 禁用 Group-1 中断
        └── gic_enable_redist(false)             ← 使 Redistributor 进入睡眠
```

### 4.4 中断处理函数

#### 4.4.1 `gic_handle_irq` - 根中断处理器

```c
static void __exception_irq_entry gic_handle_irq(struct pt_regs *regs)
{
    if (unlikely(gic_supports_nmi() && !interrupts_enabled(regs)))
        __gic_handle_irq_from_irqsoff(regs);  // NMI 路径（IRQ 已禁用）
    else
        __gic_handle_irq_from_irqson(regs);   // 普通 IRQ 路径
}
```

#### 4.4.2 `__gic_handle_irq_from_irqson` - 普通 IRQ 路径

```c
static void __gic_handle_irq_from_irqson(struct pt_regs *regs)
{
    u32 irqnr;

    // 1. 读取 IAR 获取中断号（同时 deactivate 中断）
    irqnr = gic_read_iar();

    // 2. 检查是否为伪 NMI
    is_nmi = gic_rpr_is_nmi_prio();

    if (is_nmi) {
        nmi_enter();
        __gic_handle_nmi(irqnr, regs);
        nmi_exit();
    }

    // 3. 设置 PMR 屏蔽普通 IRQ
    if (gic_prio_masking_enabled()) {
        gic_pmr_mask_irqs();
        gic_arch_enable_irqs();  // 清除 DAIF.IF
    }

    // 4. 处理普通 IRQ
    if (!is_nmi)
        __gic_handle_irq(irqnr, regs);
}
```

#### 4.4.3 `__gic_handle_irq` - 核心分发逻辑

```c
static void __gic_handle_irq(u32 irqnr, struct pt_regs *regs)
{
    // 1. 检查特殊中断号（1020-1023：空闲、EOI 等）
    if (gic_irqnr_is_special(irqnr))
        return;

    // 2. 完成 ACK（写 EOIR）
    gic_complete_ack(irqnr);

    // 3. 通过 IRQ Domain 分发到具体 handler
    if (generic_handle_domain_irq(gic_data.domain, irqnr)) {
        WARN_ONCE(true, "Unexpected interrupt (irqnr %u)\n", irqnr);
        gic_deactivate_unhandled(irqnr);
    }
}
```

### 4.5 IRQ Domain 映射

#### `gic_irq_domain_map` - 中断类型到 handler 的映射

```c
static int gic_irq_domain_map(struct irq_domain *d, unsigned int irq,
                              irq_hw_number_t hw)
{
    struct irq_chip *chip = &gic_chip;
    if (static_branch_likely(&supports_deactivate_key))
        chip = &gic_eoimode1_chip;

    switch (__get_intid_range(hw)) {
    case SGI_RANGE:
    case PPI_RANGE:
    case EPPI_RANGE:
        // 每 CPU 中断
        irq_set_percpu_devid(irq);
        irq_domain_set_info(d, irq, hw, chip, d->host_data,
                            handle_percpu_devid_irq, NULL, NULL);
        break;

    case SPI_RANGE:
    case ESPI_RANGE:
        // 共享外设中断，使用 fasteoi 处理模型
        irq_domain_set_info(d, irq, hw, chip, d->host_data,
                            handle_fasteoi_irq, NULL, NULL);
        irq_set_probe(irq);
        irqd_set_single_target(irqd);
        break;

    case LPI_RANGE:
        // LPI 中断，通过 ITS
        if (!gic_dist_supports_lpis())
            return -EPERM;
        irq_domain_set_info(d, irq, hw, chip, d->host_data,
                            handle_fasteoi_irq, NULL, NULL);
        break;
    }
    return 0;
}
```

---

## 5. GICv3 ITS 与 MSI 中断

### 5.1 ITS 硬件架构

ITS (Interrupt Translation Service) 是 GICv3 的可选组件，负责将 MSI 消息翻译为 LPI 中断。

**ITS 工作原理：**
1. 设备发送 MSI 消息（写地址 + 数据）
2. ITS 拦截 MSI 写入，根据 DeviceID 和 EventID 查找翻译表
3. 翻译为 LPI 中断号，路由到目标 CPU

**ITS 关键寄存器：**

| 寄存器 | 描述 |
|--------|------|
| `GITS_CTLR` | ITS 控制 |
| `GITS_TYPER` | 类型信息 |
| `GITS_CBASER` | 命令队列基址 |
| `GITS_CWRITER` | 命令队列写指针 |
| `GITS_CREADR` | 命令队列读指针 |
| `GITS_TRANSLATER` | 翻译寄存器（MSI 写入地址） |

**ITS 命令：**

| 命令 | 描述 |
|------|------|
| `GITS_CMD_MAPD` | 映射设备（DeviceID → ITT） |
| `GITS_CMD_MAPTI` | 映射中断（EventID → LPI + CPU） |
| `GITS_CMD_INV` | 无效化缓存 |
| `GITS_CMD_INT` | 断言中断（软件触发） |
| `GITS_CMD_CLEAR` | 清除中断 |
| `GITS_CMD_DISCARD` | 丢弃中断映射 |
| `GITS_CMD_INVALL` | 全部无效化 |
| `GITS_CMD_MOVI` | 移动中断到其他 CPU |
| `GITS_CMD_SYNC` | 同步命令 |

### 5.2 ITS 驱动

#### 关键数据结构

```c
// drivers/irqchip/irq-gic-v3-its.c
struct its_node {
    fwnode_handle           *fwnode_handle;  // 固件节点
    void __iomem            *base;            // ITS 寄存器基址
    struct list_head        entry;            // 链表节点
    struct irq_domain       *domain;          // ITS IRQ Domain
    struct its_collection   *collections;     // CPU 集合
    struct its_cmd_block    *cmd_base;        // 命令队列基址
    struct its_cmd_block    *cmd_write;       // 命令写指针
    u64                     flags;            // 标志
    u32                     device_id_bits;   // DeviceID 位宽
    u32                     event_id_bits;    // EventID 位宽
};

struct its_device {
    struct its_node         *its;             // 所属 ITS
    struct its_event_map    event_map;        // LPI 位图
    void                    *itt;             // 中断翻译表
    u32                     nr_ites;          // ITE 数量
    u32                     device_id;        // DeviceID
};
```

#### ITS 初始化流程

```
gic_init_bases()
  └── its_init(handle, &rdists, domain, prio)        ← drivers/irqchip/irq-gic-v3-its.c:5816
        ├── gen_pool_create(ITS_ITT_ALIGN)            ← 创建 ITT 内存池
        ├── gic_rdists = rdists                        ← 保存全局 rdists 引用
        ├── its_parent = parent_domain                 ← GIC domain 作为 ITS parent
        │
        ├── [DT 路径] its_of_probe(of_node)            ← 遍历设备树 ITS 节点
        │     ├── its_reset_one(&res)                  ← 对所有 ITS 执行硬件复位
        │     └── for_each ITS node:
        │           ├── its_node_init(&res, fwnode, nid)  ← 分配并初始化 its_node
        │           │     ├── ioremap(phys_base, SZ_64K)  ← 映射 ITS 寄存器
        │           │     ├── gits_read_typer(base)        ← 读取 GITS_TYPER
        │           │     └── 初始化 its->device_id_bits, event_id_bits
        │           └── its_probe_one(its)               ← 注册单个 ITS
        │                 ├── its_enable_quirks(its)      ← 应用 ITS 勘误
        │                 ├── its_alloc_pages(ITS_CMD_QUEUE_SZ)  ← 分配命令队列内存
        │                 ├── its_alloc_tables(its)       ← 分配 ITS 内部表
        │                 │     ├── its_alloc_table(its, GITS_BASER0)  ← 设备表 (DeviceID → ITT)
        │                 │     ├── its_alloc_table(its, GITS_BASER1)  ← 集合表 (CollectionID → CPU)
        │                 │     └── [v4] its_alloc_table(its, GITS_BASER2/3)  ← vPE/VMOVP 表
        │                 ├── its_alloc_collections(its)  ← 分配集合数组
        │                 ├── 设置 GITS_CBASER            ← 命令队列基址 (含共享性/缓存属性)
        │                 ├── gits_write_cwriter(0)        ← 写指针复位
        │                 ├── GITS_CTLR |= ENABLE         ← 启用 ITS
        │                 │    (v4: 同时设置 ImDe)
        │                 ├── its_init_domain(its)        ← 创建 ITS IRQ Domain
        │                 │     ├── msi_domain_info = kzalloc
        │                 │     │     info->ops = &its_msi_domain_ops
        │                 │     │     info->data = its
        │                 │     └── msi_create_parent_irq_domain(&dom_info,
        │                 │               &gic_v3_its_msi_parent_ops)
        │                 │           ├── irq_domain_create_hierarchy(its_parent, ...)
        │                 │           │     └── ops = &its_domain_ops
        │                 │           └── msi_parent_domain_init(domain, &gic_v3_its_msi_parent_ops)
        │                 └── list_add(&its->entry, &its_nodes)  ← 加入全局链表
        │
        ├── [ACPI 路径] its_acpi_probe()
        │     ├── acpi_table_parse_srat_its()          ← 解析 SRAT 获取 NUMA 信息
        │     └── for_each ITS in MADT:
        │           ├── its_node_init(&res, ...)
        │           └── its_probe_one(its)
        │
        ├── allocate_lpi_tables()                      ← 分配全局 LPI 属性表
        │     └── lpi_init_lpi_tables()
        │           ├── lpi_config_table = kzalloc(...)    ← LPI 配置表 (属性+优先级)
        │           └── lpi_get_config_base()              ← 获取共享内存基址
        │
        ├── [GICv4] its_init_vpe_domain()              ← 初始化 VPE 域
        ├── [GICv4] its_init_v4(parent_domain, ...)    ← 初始化 GICv4 支持
        │
        └── register_syscore(&its_syscore)             ← 系统暂停/恢复回调
              ├── its_syscore_suspend: 保存 GITS_CTLR, CBASER, CWRITER
              └── its_syscore_resume: 恢复寄存器，重新同步集合

  └── its_cpu_init()                                  ← 当前 CPU ITS 初始化
        ├── redist_disable_lpis()                      ← 禁用旧 LPI 配置
        │     └── 清除 GICR_CTLR.EnableLPI
        ├── its_cpu_init_lpis()                        ← 初始化 LPI 配置
        │     ├── gicr_write_propbaser(lpi_prop_va, GICR_PROPBASER)  ← LPI 属性表
        │     ├── gicr_write_pendbaser(lpi_pend_va, GICR_PENDBASER)  ← LPI 挂起表
        │     └── writel_relaxed(GICR_CTLR.EnableLPI, GICR_CTLR)
        └── its_cpu_init_collections()                 ← 初始化 ITS 集合到当前 CPU
              └── for_each ITS node:
                    └── its_cpu_init_collection(its)
                          ├── col_id = smp_processor_id()
                          ├── its->collections[col_id].target_addr = cpu_logical_map(cpu)
                          └── its_send_its_cmd(its, cmd)  ← GITS_CMD_MAPC
```

```c
// 代码简写对照
int __init its_init(struct fwnode_handle *handle, struct rdists *rdists,
                    struct irq_domain *parent_domain, u8 irq_prio)
{
    // ... [实现同上方调用栈描述]
}
```

#### ITS Domain 创建

```c
static int its_init_domain(struct its_node *its)
{
    struct irq_domain_info dom_info = {
        .fwnode         = its->fwnode_handle,
        .ops            = &its_domain_ops,
        .domain_flags   = its->msi_domain_flags,
        .parent         = its_parent,  // GIC parent domain
    };
    struct msi_domain_info *info;

    info = kzalloc_obj(*info);
    info->ops = &its_msi_domain_ops;
    info->data = its;
    dom_info.host_data = info;

    // 创建 MSI parent IRQ domain
    msi_create_parent_irq_domain(&dom_info, &gic_v3_its_msi_parent_ops);
}
```

#### ITS IRQ Domain 操作

```c
static const struct irq_domain_ops its_domain_ops = {
    .select         = msi_lib_irq_domain_select,
    .alloc          = its_irq_domain_alloc,
    .free           = its_irq_domain_free,
    .activate       = its_irq_domain_activate,
    .deactivate     = its_irq_domain_deactivate,
};
```

#### ITS 中断分配 (`its_irq_domain_alloc`)

```c
static int its_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
                                unsigned int nr_irqs, void *arg)
{
    msi_alloc_info_t *info = arg;
    struct its_device *its_dev;
    irq_hw_number_t hwirq;
    int err, i;

    // 1. 获取或创建 ITS Device 对象
    dev_id = info->scratchpad[0].ul;  // PCIe RID
    its_dev = its_create_device(its, dev_id, nr_irqs, true);

    // 2. 分配 LPI/EventID
    err = its_alloc_device_irq(its_dev, nr_irqs, &hwirq);

    // 3. 对每个 IRQ 设置映射
    for (i = 0; i < nr_irqs; i++) {
        irq_domain_set_info(domain, virq + i, hwirq + i,
                            &its_irq_chip, its_dev,
                            handle_fasteoi_irq, NULL, NULL);
        irqd_set_single_target(irqd);
        irqd_set_affinity_on_activate(irqd);
    }
    return 0;
}
```

### 5.3 MSI Parent 域

```c
// drivers/irqchip/irq-gic-its-msi-parent.c
const struct msi_parent_ops gic_v3_its_msi_parent_ops = {
    .supported_flags    = ITS_MSI_FLAGS_SUPPORTED,
    .required_flags     = ITS_MSI_FLAGS_REQUIRED,
    .chip_flags         = MSI_CHIP_FLAG_SET_EOI,
    .bus_select_token   = DOMAIN_BUS_NEXUS,
    .bus_select_mask    = MATCH_PCI_MSI | MATCH_PLATFORM_MSI,
    .prefix             = "ITS-",
    .init_dev_msi_info  = its_init_dev_msi_info,
};
```

`its_init_dev_msi_info` 根据总线类型设置 MSI prepare 回调：
- `DOMAIN_BUS_PCI_DEVICE_MSI/MSIX` → `its_pci_msi_prepare`
- `DOMAIN_BUS_DEVICE_MSI` → `its_pmsi_prepare`

#### `its_pci_msi_prepare` - PCI MSI 准备

```c
static int its_pci_msi_prepare(struct irq_domain *domain, struct device *dev,
                               int nvec, msi_alloc_info_t *info)
{
    struct pci_dev *pdev = to_pci_dev(dev);

    // 1. 获取 PCIe 的 Requester ID (RID = Bus:Device:Function)
    info->scratchpad[0].ul = pci_msi_domain_get_msi_rid(domain->parent, pdev);

    // 2. 向上取整到 2 的幂次
    // 3. 调用父域（ITS domain）的 msi_prepare
    msi_info = msi_get_domain_info(domain->parent);
    return msi_info->ops->msi_prepare(domain->parent, dev, nvec, info);
}
```

### 5.4 中断域层次结构

```
PCIe 设备中断域层次（从上到下）：

PCI_MSI/MSIX Domain           ← PCIe 设备驱动直接操作
    ↓ parent
ITS Domain                    ← ITS 中断翻译
    ↓ parent
GIC Domain                    ← GIC Distributor 管理
    ↓
CPU Interface (ICC_*_EL1)     ← 硬件 CPU 接口
```

---

## 6. NVMe MSI-X 中断分析

### 6.1 NVMe 设备关键数据结构

```c
// drivers/nvme/host/pci.c
struct nvme_dev {
    struct nvme_queue *queues;         // 队列数组（queue 0 为 admin queue）
    struct blk_mq_tag_set tagset;      // IO 队列 tagset
    struct blk_mq_tag_set admin_tagset; // Admin 队列 tagset
    unsigned online_queues;            // 已上线队列数
    unsigned max_qid;                  // 最大队列 ID
    unsigned io_queues[HCTX_MAX_TYPES]; // IO 队列类型分布
    unsigned int num_vecs;             // 已分配的 MSI-X 向量数
    u32 q_depth;                       // 队列深度
    void __iomem *bar;                 // PCIe BAR 映射
    struct nvme_ctrl ctrl;             // NVMe 控制层
    unsigned int nr_allocated_queues;  // 已分配的队列数
    unsigned int nr_write_queues;      // 写队列数
    unsigned int nr_poll_queues;       // Poll 队列数
};

struct nvme_queue {
    struct nvme_dev *dev;
    struct nvme_completion *cqes;     // 完成队列内存
    dma_addr_t cq_dma_addr;           // CQ DMA 地址
    dma_addr_t sq_dma_addr;           // SQ DMA 地址
    u32 __iomem *q_db;                // 门铃寄存器
    u32 q_depth;
    u16 cq_vector;                    // 关联的 MSI-X 向量号
    u16 qid;                          // 队列 ID (0=admin)
    u8 cq_phase;                      // CQ Phase 位
    unsigned long flags;              // NVMEQ_ENABLED, NVMEQ_POLLED 等
};
```

### 6.2 NVMe MSI-X 中断设置流程

```
nvme_probe
  → nvme_reset_work
    → nvme_setup_io_queues          // IO 队列设置入口
      → nvme_setup_irqs             // 分配 MSI-X 向量
      → nvme_create_io_queues       // 创建 IO 队列
        → nvme_create_queue
          → adapter_alloc_cq        // 发送 Admin Create CQ 命令
          → adapter_alloc_sq        // 发送 Admin Create SQ 命令
          → nvme_init_queue         // 初始化队列
          → queue_request_irq       // 注册中断处理函数
```

### 6.3 `nvme_setup_irqs` - 分配 MSI-X 向量

```c
static int nvme_setup_irqs(struct nvme_dev *dev, unsigned int nr_io_queues)
{
    struct pci_dev *pdev = to_pci_dev(dev->dev);
    struct irq_affinity affd = {
        .pre_vectors    = 1,           // 第 0 个向量给 admin queue
        .calc_sets      = nvme_calc_irq_sets,
        .priv           = dev,
    };
    unsigned int irq_queues, poll_queues;
    unsigned int flags = PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY;

    poll_queues = min(dev->nr_poll_queues, nr_io_queues - 1);
    dev->io_queues[HCTX_TYPE_POLL] = poll_queues;

    dev->io_queues[HCTX_TYPE_DEFAULT] = 1;
    dev->io_queues[HCTX_TYPE_READ] = 0;

    irq_queues = 1;  // admin queue
    if (!(dev->ctrl.quirks & NVME_QUIRK_SINGLE_VECTOR))
        irq_queues += (nr_io_queues - poll_queues);

    // 调用 PCI 核心分配 MSI-X 向量
    return pci_alloc_irq_vectors_affinity(pdev, 1, irq_queues, flags, &affd);
}
```

### 6.4 `nvme_create_queue` - 创建队列并注册中断

```c
static int nvme_create_queue(struct nvme_queue *nvmeq, int qid, bool polled)
{
    struct nvme_dev *dev = nvmeq->dev;
    u16 vector = 0;

    // 1. 确定 MSI-X 向量号
    if (!polled)
        vector = dev->num_vecs == 1 ? 0 : qid;  // 队列 ID 对应向量号
    else
        set_bit(NVMEQ_POLLED, &nvmeq->flags);

    // 2. 分配 CQ（通知 NVMe 控制器）
    result = adapter_alloc_cq(dev, qid, nvmeq, vector);
    // 3. 分配 SQ
    result = adapter_alloc_sq(dev, qid, nvmeq);

    nvmeq->cq_vector = vector;  // 保存向量号

    // 4. 初始化队列
    nvme_init_queue(nvmeq, qid);

    // 5. 注册中断处理函数
    if (!polled) {
        result = queue_request_irq(nvmeq);
    }

    set_bit(NVMEQ_ENABLED, &nvmeq->flags);
    return result;
}
```

### 6.5 `queue_request_irq` - 注册中断处理函数

```c
static int queue_request_irq(struct nvme_queue *nvmeq)
{
    struct pci_dev *pdev = to_pci_dev(nvmeq->dev->dev);
    int nr = nvmeq->dev->ctrl.instance;

    if (use_threaded_interrupts) {
        // 线程化中断：request_threaded_irq
        return pci_request_irq(pdev, nvmeq->cq_vector,
                nvme_irq_check,    // 硬中断 handler（检查是否有完成）
                nvme_irq,          // 线程化 handler
                nvmeq, "nvme%dq%d", nr, nvmeq->qid);
    } else {
        return pci_request_irq(pdev, nvmeq->cq_vector,
                nvme_irq,          // 硬中断 handler
                NULL,              // 无线程化
                nvmeq, "nvme%dq%d", nr, nvmeq->qid);
    }
}
```

### 6.6 `nvme_irq` - 中断处理函数

```c
static irqreturn_t nvme_irq(int irq, void *data)
{
    struct nvme_queue *nvmeq = data;
    DEFINE_IO_COMP_BATCH(iob);

    // 轮询 CQ 获取完成项
    if (nvme_poll_cq(nvmeq, &iob)) {
        // 批量完成请求
        if (!rq_list_empty(&iob.req_list))
            nvme_pci_complete_batch(&iob);
        return IRQ_HANDLED;
    }
    return IRQ_NONE;
}
```

### 6.7 `pci_alloc_irq_vectors_affinity` 调用链

```
nvme_setup_irqs()
  └── pci_alloc_irq_vectors_affinity(pdev, 1, irq_queues, flags, &affd)
        ← drivers/pci/msi/api.c
        │
        └── pci_msi_alloc_irq_vectors()
              ← drivers/pci/msi/irqdomain.c
              │
              ├── msi_create_device_irq_domain(dev, ...)     ← 创建设备级 IRQ Domain
              │     └── irq_domain_create_hierarchy(its_parent, 0, nvecs, ...)
              │           └── ops = &pci_msi_domain_ops
              │
              ├── msi_domain_alloc_irqs(domain, dev, nvecs) ← 核心分配入口
              │     ← kernel/irq/msi.c
              │     │
              │     └── __msi_domain_alloc_irqs(domain, dev, nvecs)
              │           │
              │           ├── msi_domain_ops->msi_prepare(...)  ← ITS 准备阶段
              │           │     └── its_pci_msi_prepare(domain, dev, nvecs, &info)
              │           │           ← drivers/irqchip/irq-gic-its-msi-parent.c
              │           │           ├── pci_msi_domain_get_msi_rid(domain, pdev)
              │           │           │     ← 获取 PCIe Requester ID (BDF)
              │           │           │     └── RID = (Bus << 8) | (Device << 3) | Function
              │           │           ├── info->scratchpad[0].ul = rid  ← 注入 DeviceID
              │           │           └── msi_get_domain_info(domain->parent)->ops->msi_prepare(...)
              │           │
              │           ├── irq_domain_alloc_irqs(domain, nvecs, node, &info)
              │           │     ← kernel/irq/irqdomain.c
              │           │     │
              │           │     └── __irq_domain_alloc_irqs(domain, virq, nvecs, &info)
              │           │           │
              │           │           ├── irq_domain_alloc_descs(virq, nvecs, node)
              │           │           │     ← 分配 irq_desc 数组和虚拟 IRQ 号
              │           │           │
              │           │           ├── [ITS Domain] its_domain_ops->alloc(...)
              │           │           │     ← its_irq_domain_alloc()  irq-gic-v3-its.c
              │           │           │     │
              │           │           │     ├── its_create_device(its, dev_id, nvecs, true)
              │           │           │     │     ├── kzalloc(struct its_device)
              │           │           │     │     ├── its_alloc_itt(its_dev, nvecs)  ← 分配 ITT 表
              │           │           │     │     │     └── gen_pool_dma_alloc(itt_pool, size)
              │           │           │     │     ├── its_send_mapd(its_dev, true)  ← GITS_CMD_MAPD
              │           │           │     │     │     └── ITS 命令: 设备表条目 [DeviceID → ITT 基址]
              │           │           │     │     └── list_add(&its_dev->entry, &its->its_device_list)
              │           │           │     │
              │           │           │     ├── its_alloc_device_irq(its_dev, nvecs, &hwirq)
              │           │           │     │     └── 从 LPI 位图分配连续的 EventID/LPI 号
              │           │           │     │
              │           │           │     └── for (i = 0; i < nvecs; i++)
              │           │           │           └── irq_domain_set_info(domain, virq+i, hwirq+i,
              │           │           │                              &its_irq_chip, its_dev,
              │           │           │                              handle_fasteoi_irq, ...)
              │           │           │
              │           │           ├── [GIC Domain] gic_irq_domain_ops->alloc(...)
              │           │           │     ← gic_irq_domain_alloc()  irq-gic-v3.c
              │           │           │     └── gic_irq_domain_map(...)
              │           │           │           └── irq_domain_set_info(d, irq, hw, chip,
              │           │           │                              handle_fasteoi_irq, ...)
              │           │           │
              │           │           └── irq_domain_activate_irq(irq_data)
              │           │                 └── [ITS Domain] its_domain_ops->activate(...)
              │           │                       ← its_irq_domain_activate()  irq-gic-v3-its.c
              │           │                       │
              │           │                       ├── its_map_irq(its_dev, d, irq_data)
              │           │                       │     ├── its_send_mapti(its_dev, event_id, lpi_nr)
              │           │                       │     │     └── GITS_CMD_MAPTI: [EventID → LPI, CPU]
              │           │                       │     └── its_send_inv(its_dev, event_id)
              │           │                       │           └── GITS_CMD_INV: 无效化 ITS 缓存
              │           │                       │
              │           │                       └── irq_chip_compose_parent(d, ...)
              │           │                             └── [GIC Domain] gic_irq_domain_activate(...)
              │           │                                   ← GIC domain 的激活操作
              │           │
              │           ├── msi_domain_ops->msi_postprep(...)  ← 后处理
              │           │
              │           └── dev->msi.data->num_vectors = nvecs  ← 保存向量数
              │
              └── pci_dev->msix_enabled = 1  ← 标记 MSI-X 已启用
```

### 6.8 NVMe MSI-X 中断完成路径

```
NVMe 控制器完成 IO 请求
  → 写 CQ 条目到主机内存
  → 触发 MSI-X 中断（写 GITS_TRANSLATER 地址）
    → ITS 翻译为 LPI
    → GIC Redistributor 接收 LPI
    → CPU Interface 触发 IRQ
    → 异常向量表 → gic_handle_irq
      → generic_handle_domain_irq
        → irq_find_mapping → irq_desc
        → handle_fasteoi_irq
          → nvme_irq (action->handler)
            → nvme_poll_cq
            → nvme_pci_complete_batch
              → blk_mq_end_request_batch
```

---

## 7. 核心 IRQ 子系统

### 7.1 `irq_desc` - 中断描述符

```c
// include/linux/irq.h
struct irq_desc {
    struct irq_common_data  irq_common_data;  // 通用数据
    struct irq_data         irq_data;         // IRQ 数据（hwirq, chip, domain）
    unsigned int __percpu   *kstat_irqs;      // 统计信息
    irq_flow_handler_t      handle_irq;       // 中断流处理函数（handle_fasteoi_irq 等）
    struct irqaction        *action;          // 中断动作链表（request_irq 注册的 handler）
    unsigned int            status_use_accessors;
    struct irqaction        *last_unhandled;  // 上次未处理的 action
    struct raw_spinlock     lock;
    struct cpumask          *percpu_enabled;  // 每 CPU 使能
    const char              *name;            // 中断名称
};
```

### 7.2 `irq_domain` - IRQ 映射域

```c
// include/linux/irqdomain.h
struct irq_domain {
    struct list_head        link;             // 全局链表
    const char              *name;            // 域名
    const struct irq_domain_ops *ops;         // 域操作函数
    struct fwnode_handle    *fwnode;          // 固件节点
    struct irq_domain       *parent;          // 父域
    struct irq_domain_hierarchy   *hierarchies; // 层次结构
    enum irq_domain_bus_token bus_token;      // 总线类型
    unsigned int            flags;            // 标志
    struct device           *pm_dev;          // PM 设备
    void                    *host_data;        // 私有数据（GIC: gic_chip_data, ITS: msi_domain_info）
};
```

### 7.3 `irq_chip` - 中断控制器硬件操作

```c
// include/linux/irq.h
struct irq_chip {
    const char      *name;
    void            (*irq_mask)(struct irq_data *data);       // 屏蔽中断
    void            (*irq_unmask)(struct irq_data *data);     // 取消屏蔽
    void            (*irq_eoi)(struct irq_data *data);        // EOI
    int             (*irq_set_affinity)(struct irq_data *data, const struct cpumask *dest, bool force);
    int             (*irq_set_type)(struct irq_data *data, unsigned int flow_type);
    int             (*irq_set_irqchip_state)(struct irq_data *data, enum irqchip_irq_state which, bool val);
    int             (*irq_get_irqchip_state)(struct irq_data *data, enum irqchip_irq_state which, bool *val);
    void            (*irq_ack)(struct irq_data *data);
    void            (*irq_nmi_setup)(struct irq_data *data);
    void            (*irq_nmi_teardown)(struct irq_data *data);
    void            (*ipi_send_mask)(struct irq_data *data, const struct cpumask *dest);
    unsigned long   flags;
};
```

### 7.4 `generic_handle_domain_irq` - 通用中断分发

```c
// kernel/irq/irqdesc.c
int generic_handle_domain_irq(struct irq_domain *domain, irq_hw_number_t hwirq)
{
    return handle_irq_desc(irq_resolve_mapping(domain, hwirq));
}
```

`irq_resolve_mapping` 查找/创建虚拟 IRQ 号到 `irq_desc` 的映射，然后 `handle_irq_desc` 调用 `desc->handle_irq`（即 `handle_fasteoi_irq` 或 `handle_percpu_devid_irq`）。

### 7.5 `handle_fasteoi_irq` - EOI 模式中断流处理

```c
// kernel/irq/chip.c
void handle_fasteoi_irq(struct irq_desc *desc)
{
    struct irq_chip *chip = irq_desc_get_chip(desc);

    raw_spin_lock(&desc->lock);

    // 1. 处理正在挂起的中断（重试机制）
    // 2. 如果中断未使能，直接 EOI 返回
    if (!irq_may_run(desc))
        goto out;

    // 3. 调用 action 链表中的 handler
    action = desc->action;
    if (action) {
        if (irq_may_run(desc)) {
            // 调用 handler
            action_ret = handle_irq_event(desc);
        }
    }

out:
    // 4. EOI
    chip->irq_eoi(&desc->irq_data);
    raw_spin_unlock(&desc->lock);
}
```

### 7.6 `request_irq` 调用链

```
request_irq(irq, handler, flags, name, dev)
  → request_threaded_irq(irq, handler, NULL, flags, name, dev)
    → __setup_irq(irq, desc, action)
      → 初始化 irqaction
      → irq_startup(desc, IRQ_RESEND)
      → __irq_startup
        → irq_domain_activate_irq
        → chip->irq_unmask(desc->irq_data)  // GIC: gic_unmask_irq
      → 将 action 挂入 desc->action 链表
```

### 7.7 `irq_find_mapping` - 查找 IRQ 映射

```c
unsigned int irq_find_mapping(struct irq_domain *domain, irq_hw_number_t hwirq)
{
    struct irq_data *data;

    // 1. 查找域内的映射
    data = radix_tree_lookup(&domain->revmap_tree, hwirq);
    if (data)
        return data->irq;

    // 2. 线性域查找
    if (domain->revmap_size && hwirq < domain->revmap_size)
        return domain->linear_revmap[hwirq];

    return 0;
}
```

---

## 8. 完整中断处理流程

### 8.1 NVMe MSI-X 中断完整路径

```
[硬件层]
NVMe 控制器完成 IO
  → 写完成队列条目到主机内存（DMA）
  → 写 PCIe MSI-X 门铃（GITS_TRANSLATER 地址 + EventID 数据）

[ITS 翻译层]
ITS 硬件拦截 MSI 写入
  → 根据 DeviceID + EventID 查找翻译表
  → 翻译为 LPI 中断号 + 目标 CPU
  → 转发到对应 Redistributor

[GIC 分发层]
Redistributor 接收 LPI
  → 检查优先级
  → 设置 Pending 位
  → 发送到 CPU Interface

[CPU 异常入口]
CPU Interface 触发 IRQ 异常
  → CPU 自动完成：
    - PSTATE → SPSR_EL1
    - PC → ELR_EL1
    - PSTATE.DAIF.IRQ = 1 (屏蔽IRQ)
    - SP → SP_EL1
    - 跳转到 vectors[el1h_irq]  (向量表偏移 0x400)

[中断向量处理]
kernel_ventry 1, h, 64, irq
  → 分配 PT_REGS_SIZE 栈空间
  → 检查栈溢出
  → 跳转到 el1h_64_irq_handler

[入口函数处理]
el1h_64_irq_handler(regs)  (entry.S 展开)
  → el1_interrupt(regs, handle_arch_irq)  (entry-common.c)
    → __el1_irq(regs, handler)
      → enter_from_kernel_mode(regs)
        → irqentry_enter(regs)  // lockdep, RCU, context tracking
      → do_interrupt_handler(regs, handler)
        → call_on_irq_stack(regs, gic_handle_irq)  // 切到 IRQ 栈

[GIC 驱动处理]
gic_handle_irq(regs)  (irq-gic-v3.c)
  → __gic_handle_irq_from_irqson(regs)  // IRQ 使能路径
    → irqnr = gic_read_iar()  // 读 ICC_IAR_EL1 获取中断号
    → gic_complete_ack(irqnr)  // 写 ICC_EOIR_EL1
    → generic_handle_domain_irq(gic_data.domain, irqnr)

[IRQ 核心分发]
generic_handle_domain_irq(domain, hwirq)  (irqdesc.c)
  → irq_resolve_mapping(domain, hwirq)  // 查找 irq_desc
  → handle_irq_desc(desc)  // 调用 desc->handle_irq
    → handle_fasteoi_irq(desc)  // 标准 fasteoi 处理流程
      → handle_irq_event(desc)  // 处理 action 链表
        → __handle_irq_event_percpu(desc, *flags)
          → action->handler(irq, action->dev_id)  // 调用 nvme_irq

[NVMe 驱动处理]
nvme_irq(irq, nvmeq)  (nvme/pci.c)
  → nvme_poll_cq(nvmeq, &iob)  // 轮询完成队列
    → 读取 CQ 条目
    → 匹配请求
    → 添加到 IO 完成批处理
  → nvme_pci_complete_batch(&iob)  // 批量完成
    → blk_mq_end_request_batch  // 通知块层

[中断返回]
exit_to_kernel_mode(regs, state)
  → irqentry_exit(regs, state)  // RCU, lockdep
  → eret  // 从 ELR_EL1 恢复 PC，从 SPSR_EL1 恢复 PSTATE
```

### 8.2 总调用链图

```
start_kernel
  → init_IRQ
    → irqchip_init
      → gic_of_init
        → gic_init_bases
          → gic_dist_init
          → set_handle_irq(gic_handle_irq)
          → its_init
          → gic_cpu_init
          → gic_smp_init

nvme_probe
  → nvme_reset_work
    → nvme_setup_io_queues
      → nvme_setup_irqs
        → pci_alloc_irq_vectors_affinity
          → msi_domain_alloc_irqs
            → its_pci_msi_prepare
            → its_irq_domain_alloc
              → its_create_device
              → its_alloc_device_irq
      → nvme_create_io_queues
        → nvme_create_queue
          → adapter_alloc_cq
          → adapter_alloc_sq
          → queue_request_irq
            → pci_request_irq
              → request_threaded_irq(nvme_irq, ...)

[中断到来]
vectors[el1h_irq]
  → el1h_64_irq_handler
    → el1_interrupt(regs, handle_arch_irq)
      → __el1_irq(regs, gic_handle_irq)
        → do_interrupt_handler(regs, gic_handle_irq)
          → gic_handle_irq(regs)
            → __gic_handle_irq_from_irqson(regs)
              → gic_read_iar()
              → gic_complete_ack()
              → generic_handle_domain_irq(gic_data.domain, irqnr)
                → irq_resolve_mapping(domain, hwirq)
                → handle_irq_desc(desc)
                  → handle_fasteoi_irq(desc)
                    → handle_irq_event(desc)
                      → __handle_irq_event_percpu(desc)
                        → nvme_irq(irq, nvmeq)
                          → nvme_poll_cq(nvmeq, &iob)
                          → blk_mq_end_request_batch
```

---

## 9. 关键数据结构汇总

| 数据结构 | 定义位置 | 描述 |
|---------|---------|------|
| `irq_desc` | `include/linux/irq.h` | 中断描述符，每个中断源一个 |
| `irq_data` | `include/linux/irq.h` | 中断数据（hwirq, chip, domain 等） |
| `irq_chip` | `include/linux/irq.h` | 中断控制器硬件操作接口 |
| `irq_domain` | `include/linux/irqdomain.h` | IRQ 映射域，hwirq → virq 映射 |
| `irq_domain_ops` | `include/linux/irqdomain.h` | 域操作（alloc, free, activate, map 等） |
| `irqaction` | `include/linux/interrupt.h` | 中断动作（handler 等） |
| `irq_fwspec` | `include/linux/irqdomain.h` | 固件中断描述 |
| `msi_msg` | `include/linux/msi.h` | MSI 消息（address_lo, address_hi, data） |
| `msi_alloc_info_t` | `include/linux/msi.h` | MSI 分配信息 |
| `msi_parent_ops` | `include/linux/msi.h` | MSI 父域操作 |
| `gic_chip_data` | `drivers/irqchip/irq-gic-v3.c` | GICv3 控制器实例 |
| `redist_region` | `drivers/irqchip/irq-gic-v3.c` | Redistributor 区域描述 |
| `its_node` | `drivers/irqchip/irq-gic-v3-its.c` | ITS 节点 |
| `its_device` | `drivers/irqchip/irq-gic-v3-its.c` | ITS 设备（LPI 映射） |
| `nvme_dev` | `drivers/nvme/host/pci.c` | NVMe PCIe 设备实例 |
| `nvme_queue` | `drivers/nvme/host/pci.c` | NVMe 队列（Admin/IO） |

---

## 10. 文件清单

| 文件 | 描述 |
|------|------|
| `arch/arm64/kernel/entry.S` | ARM64 异常向量表 + 汇编入口 |
| `arch/arm64/kernel/entry-common.c` | IRQ 入口 C 函数（el1_irq, el0_irq） |
| `arch/arm64/kernel/irq.c` | set_handle_irq, init_IRQ |
| `arch/arm64/include/asm/irq.h` | handle_arch_irq 声明 |
| `arch/arm64/include/asm/exception.h` | 异常处理函数声明 |
| `drivers/irqchip/irq-gic-v3.c` | GICv3 驱动（主驱动） |
| `drivers/irqchip/irq-gic-v3-its.c` | GICv3 ITS 驱动 |
| `drivers/irqchip/irq-gic-its-msi-parent.c` | ITS MSI 父域操作 |
| `drivers/irqchip/irq-gic-common.c` | GIC 通用工具函数 |
| `drivers/irqchip/irq-gic.c` | GICv2 驱动 |
| `include/linux/irq.h` | 核心 IRQ 数据结构 |
| `include/linux/irqdomain.h` | IRQ Domain 定义 |
| `include/linux/msi.h` | MSI 相关定义 |
| `include/linux/interrupt.h` | request_irq 等 API |
| `include/linux/irqchip/arm-gic-v3.h` | GICv3 寄存器定义 |
| `kernel/irq/irqdesc.c` | irq_desc 管理 + generic_handle_domain_irq |
| `kernel/irq/handle.c` | handle_irq_event, handle_bad_irq |
| `kernel/irq/chip.c` | handle_fasteoi_irq 等流处理函数 |
| `kernel/irq/irqdomain.c` | irq_domain 管理 |
| `kernel/irq/manage.c` | request_irq, setup_irq |
| `drivers/nvme/host/pci.c` | NVMe PCIe 驱动（MSI-X 设置） |
| `drivers/pci/msi/api.c` | pci_alloc_irq_vectors_affinity |