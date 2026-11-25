# ARM64 通用定时器硬件配置过程分析

## 1. 硬件体系概述

ARMv8/Arm64 架构定义了一套 **Generic Timer**（通用定时器）硬件规范，包括以下三种硬件变体：

| 类型 | 访问方式 | 硬件位置 | 用途 |
|------|----------|----------|------|
| **CP15 Sysreg Timer** | `mrs`/`msr` 系统寄存器指令 | 每个 CPU 核心内部 | 标准 per-CPU 定时器，clocksource + clockevent |
| **Memory-Mapped Timer** | MMIO 读写（`ldr`/`str`） | SoC 内存空间 | 不支持 CP15 访问的系统，或需要额外定时器框架 |
| **EL2 Hypervisor Timer** | 系统寄存器（EL2 专属） | 每个 CPU 核心内部 | 虚拟化场景，Hypervisor 使用 |

### 1.1 硬件架构图

```
                        ┌──────────────────────────────┐
                        │      System Counter          │
                        │   (56~64-bit, free-running)  │
                        │    频率由 CNTFRQ_EL0 报告       │
                        └──────────┬───────────────────┘
                                   │
              ┌────────────────────┼──────────────────────┐
              │ cntpct_el0         │ cntvct_el0           │
              ▼                    ▼                      │
     ┌───────────────────────────────────────┐            │
     │  Per-CPU Timer (CP15 sysreg)          │            │
     │  ┌─────────────────────────────────┐  │            │
     │  │  Physical Timer                 │  │            │
     │  │  CNTP_CTL_EL0, CNTP_CVAL_EL0,  │  │            │
     │  │  CNTP_TVAL_EL0                  │  │            │
     │  │  → PPI 14 (non-secure)          │  │            │
     │  │  → PPI 13 (secure)              │  │            │
     │  ├─────────────────────────────────┤  │            │
     │  │  Virtual Timer                  │  │            │
     │  │  CNTV_CTL_EL0, CNTV_CVAL_EL0,  │  │            │
     │  │  CNTV_TVAL_EL0                  │  │            │
     │  │  → PPI 11                       │  │            │
     │  ├─────────────────────────────────┤  │            │
     │  │  EL2 Physical Timer             │  │            │
     │  │  CNTHP_CTL_EL2, CNTHP_CVAL_EL2 │  │            │
     │  │  → PPI 10                       │  │            │
     │  └─────────────────────────────────┘  │            │
     └───────────────────────────────────────┘            │
                                                          │
     ┌───────────────────────────────────────┐            │
     │  Memory-Mapped Timer (MMIO Frame)     │            │
     │                                        │            │
     │  CNTCTLBase (控制寄存器基址)            │            │
     │  ├─ CNTTIDR       → Frame 能力标识     │            │
     │  ├─ CNTACR(n)     → Frame n 访问控制  │            │
     │  └─ Frame n:                         │            │
     │       ├─ CNTPCT_LO/HI  (物理计数器)   │            │
     │       ├─ CNTVCT_LO/HI  (虚拟计数器)   │            │
     │       ├─ CNTFRQ        (频率)         │            │
     │       ├─ CNTP_CVAL_LO/HI (物理比较值) │            │
     │       ├─ CNTP_CTL       (物理控制)    │            │
     │       ├─ CNTV_CVAL_LO/HI (虚拟比较值) │            │
     │       └─ CNTV_CTL       (虚拟控制)    │            │
     └───────────────────────────────────────┘            │
                                                          │
                                   ┌──────────────────────┘
                                   ▼
                        ┌──────────────────────┐
                        │  GIC (中断控制器)     │
                        │  PPI 10/11/13/14     │
                        │  SPI (MMIO Frame)    │
                        └──────────────────────┘
```

### 1.2 关键硬件概念

**System Counter（系统计数器）**：
- 由整个 SoC 唯一的自由运行计数器驱动
- 宽度至少 56 位（通常 64 位）
- 频率由 `CNTFRQ_EL0` 寄存器报告（典型值：1MHz~100MHz）
- 最小滚转保证时间：40 年

**CNTVOFF_EL2（虚拟偏移）**：
- `CNTVCT = CNTPCT - CNTVOFF_EL2`
- 由 Hypervisor 设置，为每个 VM 提供独立的虚拟时间视图
- 在 EL2 启动代码中清零（`cntvoff_el2, xzr`）

**ECV（Enhanced Counter Virtualization）**：
- ARMv8.6 引入，提供 `CNTPCTSS_EL0` / `CNTVCTSS_EL0` 自同步读取
- 消除传统 `ISB + MRS` 序列的开销
- 使能位在 `CNTHCTL_EL2.ECV`（bit 12）

---

## 2. 不同 Timer 硬件的配置路径

ARM64 通用定时器有三种配置入口，由固件/设备树/ACPI 决定走哪条路径：

```
  Kernel 启动
      │
      ├─ [EL2 Boot] __init_el2_timers         ← 所有路径都经过
      │
      ├─ [DT 路径] timer_probe()
      │    ├─ TIMER_OF_DECLARE(armv8_arch_timer)
      │    │    └─ arch_timer_of_init()        ← CP15 sysreg 定时器
      │    │
      │    └─ platform_driver(arch_timer_mmio)
      │         └─ arch_timer_mmio_probe()     ← MMIO 内存映射定时器
      │
      └─ [ACPI 路径] acpi_table_parse(GTDT)
           └─ arch_timer_acpi_init()            ← CP15 sysreg (ACPI 板)
```

### 2.1 路径比较

| 特性 | DT CP15 路径 | ACPI CP15 路径 | MMIO 路径 |
|------|-------------|---------------|-----------|
| 触发方式 | `TIMER_OF_DECLARE` | `TIMER_ACPI_DECLARE` | `platform_driver.probe` |
| 中断类型 | PPI（per-CPU） | PPI（per-CPU） | SPI（全局） |
| 频率来源 | `CNTFRQ_EL0` 或 DTS `clock-frequency` | 仅 `CNTFRQ_EL0` | 寄存器 `CNTFRQ` 或 DTS |
| `c3stop` 判断 | DTS `always-on` 属性 | `GTDT` 表 `C3STOP` 标志 | 始终 `false`（MMIO 帧独立于 CPU） |
| Errata 匹配 | `ate_match_dt` | `ate_match_acpi_oem_info` | 不适用 |
| 每个 CPU 配置 | CPU hotplug 回调 | CPU hotplug 回调 | 全局配置 |

---

## 3. 阶段 1：EL2 启动代码 — 最底层的硬件配置

### 3.1 源码位置

[arch/arm64/include/asm/el2_setup.h:112](file:///home/louis/code/linux/arch/arm64/include/asm/el2_setup.h#L112)

### 3.2 配置代码

```asm
.macro __init_el2_timers
    mov     x0, #3                          // bit[0]=EL1PCTEN, bit[1]=EL1PCEN
    __check_hvhe .LnVHE_\@, x1
    lsl     x0, x0, #10                     // VHE 模式: 移到 bit[10:11]
.LnVHE_\@:
    msr     cnthctl_el2, x0                 // 写入 CNTHCTL_EL2
    msr     cntvoff_el2, xzr                // 清零虚拟偏移
.endm
```

### 3.3 硬件行为

**非 VHE 模式（nVHE）**：`CNTHCTL_EL2 = 0x3`

| 位域 | 值 | 含义 |
|------|----|------|
| [0] EL1PCTEN | 1 | 允许 EL1 访问物理计数器 `CNTPCT_EL0` |
| [1] EL1PCEN | 1 | 允许 EL1 访问物理定时器 `CNTP_CTL/CVAL_EL0` |
| [2] EVNTEN | 0 | 事件流禁用 |
| [13] EL1TVT | 0 | 禁止 EL1 访问虚拟定时器（nVHE 下由 Host 管理） |
| [14] EL1TVCT | 0 | 禁止 EL1 访问虚拟计数器（nVHE 下由 Host 管理） |

**VHE 模式**：`CNTHCTL_EL2 = 0xC00`

- 当 `HCR_EL2.E2H = 1` 时，`CNTHCTL_EL2` 的位布局与 `CNTKCTL_EL1` 相同
- bit[10] = EL0PCTEN, bit[11] = EL0PCEN
- 此时 `CNTKCTL_EL1` 访问指令被重定向到 `CNTHCTL_EL2`

**CNTVOFF_EL2**：清零，确保 `CNTVCT = CNTPCT`（Host 模式下虚拟计数器等于物理计数器）

### 3.4 寄存器变化

| 寄存器 | 配置前 | 配置后 |
|--------|--------|--------|
| `CNTHCTL_EL2` | 0（Reset） | 0x3 (nVHE) 或 0xC00 (VHE) |
| `CNTVOFF_EL2` | 未定义 | 0x0 |

### 3.5 调用上下文

```
  → 入口: head.S
    → __primary_switched
      → __cpu_setup
        → __init_el2_timers (在 EL2 未退出时调用)
```

---

## 4. 阶段 2：DT/ACPI 驱动探测

### 4.1 DT 路径：`arch_timer_of_init()`

**源码位置**: [drivers/clocksource/arm_arch_timer.c:1130](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L1130)

#### 4.1.1 完整调用栈

```
start_kernel()
  └─ time_init()                                    ← init/main.c
       └─ timer_probe()                             ← drivers/clocksource/timer_probe.c
            └─ TIMER_OF_DECLARE(armv8_arch_timer)
                 └─ arch_timer_of_init(np)          ← drivers/clocksource/arm_arch_timer.c:1130
                      │
                      ├─ [Step 1] 解析中断
                      │    ├─ of_irq_get_byname(np, "sec-phys")  → PPI 13
                      │    ├─ of_irq_get_byname(np, "phys")      → PPI 14
                      │    ├─ of_irq_get_byname(np, "virt")      → PPI 11
                      │    └─ of_irq_get_byname(np, "hyp-phys")  → PPI 10
                      │    └─ arch_timer_populate_kvm_info()     ← 设置 KVM 信息
                      │
                      ├─ [Step 2] 读取频率
                      │    └─ arch_timer_get_cntfrq()            ← read_sysreg(cntfrq_el0)
                      │    └─ arch_timer_of_configure_rate()     ← 可选 DTS clock-frequency 覆盖
                      │
                      ├─ [Step 3] 解析 DTS 属性
                      │    ├─ "always-on" → arch_timer_c3stop = false
                      │    └─ "arm,no-tick-in-suspend" → arch_counter_suspend_stop = true
                      │
                      ├─ [Step 4] 检查 Errata
                      │    └─ arch_timer_check_ool_workaround(ate_match_dt, np)
                      │
                      ├─ [Step 5] 选择 PPI
                      │    └─ arch_timer_select_ppi()
                      │         ├─ is_kernel_in_hyp_mode()       → ARCH_TIMER_HYP_PPI
                      │         ├─ !is_hyp_mode_available()      → ARCH_TIMER_VIRT_PPI
                      │         └─ CONFIG_ARM64                 → ARCH_TIMER_PHYS_NONSECURE_PPI
                      │
                      ├─ [Step 6] arch_timer_register()          ← 注册中断和 CPU hotplug
                      │    ├─ alloc_percpu(clock_event_device)
                      │    ├─ request_percpu_irq(ppi, handler, "arch_timer")
                      │    ├─ arch_timer_cpu_pm_init()
                      │    └─ cpuhp_setup_state(CPUHP_AP_ARM_ARCH_TIMER_STARTING,
                      │         arch_timer_starting_cpu, arch_timer_dying_cpu)
                      │
                      └─ [Step 7] arch_timer_common_init()
                           ├─ arch_timer_banner()                ← 打印 log
                           ├─ arch_counter_register()            ← 注册 clocksource
                           │    ├─ clocksource_register_hz(&clocksource_counter, rate)
                           │    ├─ timecounter_init()             ← KVM timecounter
                           │    └─ sched_clock_register()         ← 调度时钟
                           └─ arch_timer_arch_init()             ← arm64 空函数
```

#### 4.1.2 硬件行为

**中断解析**：读取 DTS 中 `interrupts` 属性，将 PPI 号存入全局 `arch_timer_ppi[]` 数组。

**频率确定**：
1. 默认读取 `CNTFRQ_EL0` 系统寄存器（硬件复位值由 SoC 固件设置）
2. 如果 DTS 中有 `clock-frequency` 属性，则覆盖硬件值
3. 频率必须 ≥ 1MHz，否则 `WARN_ON`

**PPI 选择逻辑**（`arch_timer_select_ppi`）：

```
PPI 选择流程图:
═══════════════════════════════════════════════════════════════════
是 ┌──────────────────┐ 否
◄──┤ is_kernel_in_hyp │
   │     _mode()?     │
   └──────────────────┘
         │
         ▼
  ┌──────────────┐
  │ HYP_PPI      │  ← PPI 10, 使用 CNTHP_*_EL2
  │ (VHE 模式)    │     内核在 EL2 运行，物理定时器重定向到管理程序定时器
  └──────────────┘

         │ 否
         ▼
   ┌──────────────────────┐ 是
   │is_hyp_mode_available?│
   └──────────────────────┘
         │ 否               │
         ▼                  ▼
   ┌──────────────┐   ┌──────────────┐
   │ VIRT_PPI     │   │ 继续判断     │
   │ (PPI 11)     │   └──────────────┘
   └──────────────┘         │
                            ▼
                    ┌──────────────────┐
                    │ CONFIG_ARM64 ?   │
                    └──────────────────┘
                    是 │         │ 否
                      ▼         ▼
              ┌────────────┐  ┌──────────────┐
              │PHYS_NONSEC │  │PHYS_SECURE   │
              │(PPI 14)    │  │(PPI 13)      │
              └────────────┘  └──────────────┘
═══════════════════════════════════════════════════════════════════
```

**PPI 选择总结**：

| 场景 | 选择的 PPI | 使用的寄存器 | 说明 |
|------|-----------|-------------|------|
| VHE (EL2) | `HYP_PPI` (10) | `CNTHP_*_EL2` | 内核在 EL2 运行，物理定时器被重定向 |
| nVHE + virt PPI 可用 | `VIRT_PPI` (11) | `CNTV_*_EL0` | 标准路径，虚拟定时器留给 Guest |
| 无 HYP + arm64 | `PHYS_NONSECURE` (14) | `CNTP_*_EL0` | 非安全物理定时器 |
| 无 HYP + arm | `PHYS_SECURE` (13) | `CNTP_*_EL0` | 安全物理定时器 |

#### 4.1.3 寄存器变化

| 寄存器 | 阶段 | 值 | 说明 |
|--------|------|----|------|
| `CNTFRQ_EL0` | 探测前 | SoC 复位值 | 硬件写入，只读 |
| `arch_timer_rate` | 探测后 | 如 19200000 | 全局变量，频率值 |
| `arch_timer_ppi[]` | 探测后 | 如 [13, 14, 11, 10] | 全局数组，PPI 号 |
| `arch_timer_c3stop` | 探测后 | true/false | 由 `always-on` 属性决定 |

### 4.2 ACPI 路径：`arch_timer_acpi_init()`

**源码位置**: [drivers/clocksource/arm_arch_timer.c:1206](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L1206)

#### 4.2.1 完整调用栈

```
start_kernel()
  └─ time_init()
       └─ acpi_table_parse(ACPI_SIG_GTDT, ...)
            └─ TIMER_ACPI_DECLARE(arch_timer, ACPI_SIG_GTDT)
                 └─ arch_timer_acpi_init(table)     ← drivers/clocksource/arm_arch_timer.c:1206
                      │
                      ├─ [Step 1] acpi_gtdt_init(table)     ← 解析 GTDT 表头
                      │
                      ├─ [Step 2] 获取中断
                      │    ├─ acpi_gtdt_map_ppi(PHYS_NONSECURE) → PPI 14
                      │    ├─ acpi_gtdt_map_ppi(VIRT)          → PPI 11
                      │    └─ acpi_gtdt_map_ppi(HYP)           → PPI 10
                      │
                      ├─ [Step 3] 读取频率（仅 CNTFRQ_EL0，无 DTS 覆盖）
                      │    └─ arch_timer_rate = arch_timer_get_cntfrq()
                      │
                      ├─ [Step 4] 选择 PPI
                      │    └─ arch_timer_select_ppi()          ← 同 DT 路径
                      │
                      ├─ [Step 5] GTDT 特定属性
                      │    └─ acpi_gtdt_c3stop()               ← 读取 C3STOP 标志
                      │
                      ├─ [Step 6] 检查 Errata
                      │    └─ arch_timer_check_ool_workaround(ate_match_acpi_oem_info, table)
                      │
                      ├─ [Step 7] arch_timer_register()        ← 同 DT 路径
                      │
                      └─ [Step 8] arch_timer_common_init()     ← 同 DT 路径
```

#### 4.2.2 ACPI 与 DT 路径的关键差异

| 差异点 | DT 路径 | ACPI 路径 |
|--------|---------|-----------|
| 中断命名 | 通过 `interrupt-names` 匹配 | 通过 GTDT 表固定位置 |
| 频率覆盖 | 支持 `clock-frequency` 属性覆盖 | 强制使用 `CNTFRQ_EL0` |
| `c3stop` | DTS `always-on` 属性 | GTDT 表的 `C3STOP` 标志 |
| Errata 匹配 | 匹配 DTS 属性（如 `fsl,erratum-a008585`） | 匹配 ACPI OEM ID（如 `HISI`） |
| 安全 PPI | arm64 下不使用 `PHYS_SECURE_PPI` | 同左 |

### 4.3 MMIO 路径：`arch_timer_mmio_probe()`

**源码位置**: [drivers/clocksource/arm_arch_timer_mmio.c:380](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer_mmio.c#L380)

#### 4.3.1 完整调用栈

```
start_kernel()
  └─ time_init()
       └─ timer_probe()                              ← 遍历所有 timer 兼容节点
            └─ TIMER_OF_DECLARE(armv7_arch_timer_mem)
                 → platform_driver(arch_timer_mmio_drv)
                      └─ arch_timer_mmio_probe(pdev)  ← drivers/clocksource/arm_arch_timer_mmio.c:380
                           │
                           ├─ [Step 1] of_populate_gt_block(pdev, at)
                           │    ├─ 解析 CNTCTLBase (res 0)
                           │    └─ 遍历子节点 frame
                           │         ├─ 读取 frame-number
                           │         ├─ 解析 frame cntbase (res 0)
                           │         ├─ of_irq_parse_and_map(frame, 0) → phys_irq
                           │         └─ of_irq_parse_and_map(frame, 1) → virt_irq
                           │
                           ├─ [Step 2] find_best_frame(pdev)
                           │    ├─ ioremap(CNTCTLBase)
                           │    ├─ 读取 CNTTIDR 获取 frame 能力
                           │    ├─ 遍历 frame:
                           │    │    ├─ 写 CNTACR(n) 尝试使能所有访问
                           │    │    ├─ 读回 CNTACR(n) 确认哪些位生效
                           │    │    ├─ 优先选 virtual 且 virt_irq 存在的 frame
                           │    │    └─ 回退到 physical 且 phys_irq 存在的 frame
                           │    └─ iounmap()
                           │
                           └─ [Step 3] arch_timer_mmio_frame_register(pdev, frame)
                                ├─ devm_request_mem_region(frame->cntbase)
                                ├─ devm_ioremap(frame->cntbase)           ← 映射 frame 寄存器
                                ├─ readl(CNTFRQ) → at->rate              ← 读取频率
                                ├─ devm_request_irq(irq, handler)         ← 注册 SPI 中断
                                └─ arch_timer_mmio_setup(at, irq)
                                     ├─ clockevents_config_and_register() ← clockevent (rating=400)
                                     └─ clocksource_register_hz()          ← clocksource (rating=300)
```

#### 4.3.2 DTS 绑定示例

```dts
timer@2a810000 {
    compatible = "arm,armv7-timer-mem";
    reg = <0 0x2a810000 0 0x10000>;        /* CNTCTLBase */

    frame@2a830000 {
        frame-number = <0>;
        interrupts = <0 26 4>;              /* phys_irq (SPI) */
        reg = <0 0x2a830000 0 0x10000>;     /* frame cntbase */
    };
};
```

#### 4.3.3 MMIO 寄存器布局

MMIO Frame 内部寄存器偏移（相对于 frame 的 cntbase）：

| 偏移 | 寄存器 | 宽度 | 描述 |
|------|--------|------|------|
| 0x00 | `CNTPCT_LO` | 32 | 物理计数器低 32 位 |
| 0x04 | `CNTPCT_HI` | 32 | 物理计数器高 32 位 |
| 0x08 | `CNTVCT_LO` | 32 | 虚拟计数器低 32 位 |
| 0x0C | `CNTVCT_HI` | 32 | 虚拟计数器高 32 位 |
| 0x10 | `CNTFRQ` | 32 | 计数器频率 |
| 0x20 | `CNTP_CVAL_LO` | 32 | 物理比较值低 32 位 |
| 0x24 | `CNTP_CVAL_HI` | 32 | 物理比较值高 32 位 |
| 0x2C | `CNTP_CTL` | 32 | 物理定时器控制 |
| 0x30 | `CNTV_CVAL_LO` | 32 | 虚拟比较值低 32 位 |
| 0x34 | `CNTV_CVAL_HI` | 32 | 虚拟比较值高 32 位 |
| 0x3C | `CNTV_CTL` | 32 | 虚拟定时器控制 |

CNTCTLBase 内部寄存器（全局控制）：

| 偏移 | 寄存器 | 描述 |
|------|--------|------|
| 0x08 | `CNTTIDR` | 标识各 frame 的能力（bit[4n+1] = frame n 支持 virtual） |
| 0x40 + n*4 | `CNTACR(n)` | Frame n 的访问控制寄存器 |

#### 4.3.4 Frame 选择算法

```
find_best_frame() 算法:
═══════════════════════════════════════════════════════════════════
1. ioremap(CNTCTLBase)
2. 读 CNTTIDR → 获取各 frame 能力
3. 对每个 frame n:
   a. 写 CNTACR(n) = 0x3F (使能所有访问)
   b. 读回 CNTACR(n) → 确认哪些访问位实际生效
   c. 检查条件:
      - 优先: CNTTIDR 显示支持 virtual
            + CNTACR.RWVT 和 CNTACR.RVCT 已生效
            + 有 virt_irq
            → 选择 VIRT_ACCESS
      - 回退: CNTACR.RWPT 和 CNTACR.RPCT 已生效
            + 有 phys_irq
            → 选择 PHYS_ACCESS
4. iounmap(CNTCTLBase)
5. 返回最佳 frame
═══════════════════════════════════════════════════════════════════
```

---

## 5. 阶段 3：中断和 Per-CPU 配置

### 5.1 `arch_timer_register()`

**源码位置**: [drivers/clocksource/arm_arch_timer.c:1006](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L1006)

#### 5.1.1 硬件行为

**中断注册**：

根据已选择的 PPI 类型，注册对应的中断处理函数：

| PPI 选择 | 中断号 | 处理函数 | 备注 |
|----------|--------|----------|------|
| `VIRT_PPI` | PPI 11 | `arch_timer_handler_virt` | 使用 `CNTV_*_EL0` |
| `PHYS_NONSECURE_PPI` | PPI 14 | `arch_timer_handler_phys` | 使用 `CNTP_*_EL0`，额外注册 PPI 14 |
| `PHYS_SECURE_PPI` | PPI 13 | `arch_timer_handler_phys` | 使用 `CNTP_*_EL0`，如果 PPI 14 可用也注册 |
| `HYP_PPI` | PPI 10 | `arch_timer_handler_phys` | 使用 `CNTHP_*_EL2`（VHE 重定向） |

**CPU hotplug 回调注册**：

```c
cpuhp_setup_state(CPUHP_AP_ARM_ARCH_TIMER_STARTING,
                  "clockevents/arm/arch_timer:starting",
                  arch_timer_starting_cpu,   // 每个 CPU 上线时调用
                  arch_timer_dying_cpu);     // 每个 CPU 下线时调用
```

### 5.2 `arch_timer_starting_cpu()` — Per-CPU 配置

**源码位置**: [drivers/clocksource/arm_arch_timer.c:825](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L825)

#### 5.2.1 完整调用栈

```
boot CPU 启动
  │
  └─ cpuhp_thread_fun()                          ← CPU hotplug 框架
       └─ cpuhp_invoke_callback(CPUHP_AP_ARM_ARCH_TIMER_STARTING)
            └─ arch_timer_starting_cpu(cpu)      ← drivers/clocksource/arm_arch_timer.c:825
                 │
                 ├─ [1] __arch_timer_setup(clk)  ← 配置 clock_event_device
                 │    ├─ clk->features = ONESHOT
                 │    ├─ clk->name = "arch_sys_timer"
                 │    ├─ clk->rating = 450
                 │    ├─ clk->cpumask = cpumask_of(cpu)
                 │    ├─ clk->irq = arch_timer_ppi[arch_timer_uses_ppi]
                 │    ├─ clk->set_state_shutdown = arch_timer_shutdown_virt/phys
                 │    ├─ clk->set_next_event = set_next_event_virt/phys
                 │    ├─ clk->set_state_shutdown(clk)     ← 关闭定时器 (ENABLE=0)
                 │    ├─ clockevents_config_and_register() ← 注册到 clockevents 框架
                 │    └─ 检查 __arch_timer_check_delta()   ← 最大 delta 检测
                 │
                 ├─ [2] 使能 PPI 中断
                 │    ├─ check_ppi_trigger(irq)            ← 验证触发类型
                 │    └─ enable_percpu_irq(irq, flags)     ← 使能 GIC 中断
                 │
                 ├─ [3] 非安全 PPI 额外使能（如果适用）
                 │    └─ enable_percpu_irq(PHYS_NONSECURE_PPI)
                 │
                 └─ [4] arch_counter_set_user_access()     ← 配置 CNTKCTL_EL1
                      └─ 设置用户态访问权限
```

#### 5.2.2 硬件行为 — 关闭定时器

`__arch_timer_setup()` 中调用 `set_state_shutdown()`：

```c
// arch_timer_shutdown_virt()
ctrl = read_sysreg(cntv_ctl_el0);     // 读取当前控制寄存器
ctrl &= ~ARCH_TIMER_CTRL_ENABLE;      // 清除 ENABLE 位
write_sysreg(ctrl, cntv_ctl_el0);     // 写回，关闭定时器
isb();
```

**CNTV_CTL_EL0 寄存器变化**：

| 位域 | 名称 | 配置前 | 配置后 |
|------|------|--------|--------|
| [0] | ENABLE | 取决于固件 | 0（关闭） |
| [1] | IMASK | 取决于固件 | 保持不变 |
| [2] | ISTATUS | 只读 | 不变 |

#### 5.2.3 硬件行为 — 配置用户态访问

`arch_counter_set_user_access()` 配置 `CNTKCTL_EL1`：

**源码位置**: [drivers/clocksource/arm_arch_timer.c:790](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L790)

```c
// 先清除所有用户态访问位
cntkctl &= ~(ARCH_TIMER_USR_PT_ACCESS_EN    // bit[0]: 物理定时器
           | ARCH_TIMER_USR_VT_ACCESS_EN     // bit[8]: 虚拟定时器
           | ARCH_TIMER_USR_VCT_ACCESS_EN    // bit[1]: 虚拟计数器
           | ARCH_TIMER_VIRT_EVT_EN          // bit[2]: 事件流
           | ARCH_TIMER_USR_PCT_ACCESS_EN);  // bit[9]?? 物理计数器

// 如果无 errata，使能用户态读取虚拟计数器（VDSO 需要）
if (!arch_timer_this_cpu_has_cntvct_wa())
    cntkctl |= ARCH_TIMER_USR_VCT_ACCESS_EN; // 允许用户态读 CNTVCT_EL0
```

**CNTKCTL_EL1 寄存器布局**：

| 位域 | 名称 | 描述 | 典型值 |
|------|------|------|--------|
| [0] | EL0PCTEN | 用户态访问物理计数器 | 0 |
| [1] | EL0PVTEN | 用户态访问物理定时器 | 0 |
| [8] | EL0VTEN | 用户态访问虚拟定时器 | 0 |
| [9] | EL0VCTEN | 用户态访问虚拟计数器 | 1（VDSO 需要） |
| [2] | EVNTEN | 事件流使能 | 0（后续配置） |
| [3] | EVNTDIR | 事件流方向 | 0 |
| [7:4] | EVNTI | 事件流 divider | 0 |

#### 5.2.4 寄存器变化总结（Per-CPU 配置阶段）

| 寄存器 | 配置前 | 配置后 | 说明 |
|--------|--------|--------|------|
| `CNTV_CTL_EL0` (或 `CNTP_CTL_EL0`) | 未定义/固件值 | ENABLE=0, IMASK 不变 | 关闭定时器 |
| `CNTKCTL_EL1` | 0（Reset） | `EL0VCTEN=1` (通常) | 允许用户态读虚拟计数器 |
| GIC PPI 中断 | 掩码（masked） | 使能（unmasked） | 允许接收定时器中断 |

---

## 6. 阶段 4：Clocksource 和 Clockevent 注册

### 6.1 `arch_counter_register()` — Clocksource 注册

**源码位置**: [drivers/clocksource/arm_arch_timer.c:899](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L899)

#### 6.1.1 硬件行为

**计数器选择**：

```
计数器选择逻辑:
═══════════════════════════════════════════════════════════════════
条件: CONFIG_ARM64 && !is_hyp_mode_available()
      || arch_timer_uses_ppi == ARCH_TIMER_VIRT_PPI
                          │
                    是    ▼    否
                  ┌──────────────────┐
                  │ 使用 CNTVCT       │    使用 CNTPCT
                  │ (虚拟计数器)      │    (物理计数器)
                  └──────────────────┘
                          │
                    是    ▼    否
            有 errata? ──────→ arch_counter_get_cntvct()
                          │         └─ mrs %0, cntvct_el0
                          ▼
             arch_counter_get_cntvct_stable()
               └─ 通过 erratum_handler 重定向
═══════════════════════════════════════════════════════════════════
```

**Clocksource 参数**：

```c
static struct clocksource clocksource_counter = {
    .name   = "arch_sys_counter",
    .id     = CSID_ARM_ARCH_COUNTER,        // 唯一 ID，用于 VDSO 识别
    .rating = 400,                           // 极高评级（仅次于 500 的完美时钟源）
    .read   = arch_counter_read,             // 读取函数 → arch_timer_read_counter()
    .flags  = CLOCK_SOURCE_IS_CONTINUOUS,    // 永不停止
    .mask   = CLOCKSOURCE_MASK(56~64),       // 由 arch_counter_get_width() 确定
    .vdso_clock_mode = VDSO_CLOCKMODE_ARCHTIMER, // VDSO 快速路径
};
```

**sched_clock 注册**：

```c
sched_clock_register(scr, width, arch_timer_rate);
// scr = 计数器读取函数（同上）
// width = 计数器宽度（56~64）
// arch_timer_rate = 频率（如 19.2MHz）
```

### 6.2 `__arch_timer_setup()` — Clockevent 注册

**源码位置**: [drivers/clocksource/arm_arch_timer.c:674](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L674)

#### 6.2.1 Clockevent 参数

```c
clk->name = "arch_sys_timer";
clk->rating = 450;           // 高于 clocksource 的 400
clk->features = CLOCK_EVT_FEAT_ONESHOT;
if (arch_timer_c3stop)
    clk->features |= CLOCK_EVT_FEAT_C3STOP;
```

**`clockevents_config_and_register()` 配置**：

```c
clockevents_config_and_register(clk, arch_timer_rate, 0xf, max_delta);
// arch_timer_rate = 频率（如 19.2MHz）
// 0xf = min_delta (15 ticks)
// max_delta = 由 __arch_timer_check_delta() 确定
//   - 正常: CLOCKSOURCE_MASK(56~64)
//   - XGene-1 有坑: CLOCKSOURCE_MASK(31) (CVAL 实现为 TVAL，只有 32 位)
```

#### 6.2.2 `set_next_event` 硬件行为

```c
set_next_event_virt(evt, clk):
1. ctrl = read_sysreg(cntv_ctl_el0)    // 读取当前控制寄存器
2. ctrl |= ARCH_TIMER_CTRL_ENABLE       // 设置 ENABLE=1
3. ctrl &= ~ARCH_TIMER_CTRL_IT_MASK     // 清除 IMASK（允许中断）
4. cnt = __arch_counter_get_cntvct()    // 读取当前虚拟计数器值
5. write_sysreg(cnt + evt, cntv_cval_el0) // 设置比较值 = 当前值 + 超时值
6. write_sysreg(ctrl, cntv_ctl_el0)     // 写回控制寄存器，使能定时器
```

**CNTV_CVAL_EL0 和 CNTV_CTL_EL0 寄存器状态**：

```
编程前:
  CNTV_CVAL_EL0 = 0x0 (或上次值)
  CNTV_CTL_EL0  = {ENABLE=0, IMASK=1, ISTATUS=?}

编程后:
  CNTV_CVAL_EL0 = current_cnt + evt
  CNTV_CTL_EL0  = {ENABLE=1, IMASK=0, ISTATUS=0}
  → 当 System Counter 递增到 CNTV_CVAL_EL0 时触发中断
```

---

## 7. 阶段 5：Event Stream 配置

### 7.1 硬件行为

**源码位置**: [drivers/clocksource/arm_arch_timer.c:742](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L742)

Event Stream 是 ARM Generic Timer 的一个可选功能，以固定频率产生广播事件，用于 **WFE/WFI 指令的唤醒**。

```c
core_initcall(arch_timer_evtstrm_register);
  └─ cpuhp_setup_state(CPUHP_AP_ARM_ARCH_TIMER_EVTSTRM_STARTING)
       └─ arch_timer_evtstrm_starting_cpu(cpu)
            └─ arch_timer_configure_evtstream()
                 └─ arch_timer_evtstrm_enable(divider)
```

**配置计算**：

```c
// 目标: 100us 间隔的事件流
// EVT_STREAM_FREQ = 1s / 100us = 10000 Hz
// divider = (arch_timer_rate / 2) / 10000
// 取最接近的 2 的幂

// 例如: 19.2MHz 计数器
// evt_stream_div = 19200000 / 2 / 10000 = 960
// lsb = fls(960) - 1 = 9 (512)
// 如果 bit[8] 置位 (960 & 256 = 1) → lsb = 10
// divider = 10
```

**CNTKCTL_EL1 配置变化**：

```c
cntkctl |= (divider << ARCH_TIMER_EVT_TRIGGER_SHIFT)  // bit[7:4] = divider
        |  ARCH_TIMER_VIRT_EVT_EN;                      // bit[2] = 1 (事件流使能)
```

**ECV 特殊处理**（ARMv8.6+）：

```c
// 如果 divider > 15，使用 EVNTIS 标志扩展
if (cpus_have_final_cap(ARM64_HAS_ECV) && divider > 15) {
    cntkctl |= ARCH_TIMER_EVT_INTERVAL_SCALE;  // bit[17] = 1
    divider -= 8;  // 减去 8 的基数
}
```

### 7.2 寄存器变化

| 寄存器 | 配置前 | 配置后 |
|--------|--------|--------|
| `CNTKCTL_EL1.EVNTEN` | 0 | 1 |
| `CNTKCTL_EL1.EVNTI` | 0 | divider 值 |
| `CNTKCTL_EL1.EVNTDIR` | 0 | 0（递增计数触发） |
| `CNTKCTL_EL1.EVNTIS` (ECV) | 0 | 1（如果 divider > 15） |

---

## 8. 中断处理硬件行为

### 8.1 中断触发条件

```
定时器中断触发条件:
═══════════════════════════════════════════════════════════════════
System Counter 递增 ...
                │
                当 CNTPCT_EL0 >= CNTP_CVAL_EL0 (物理定时器)
                或 CNTVCT_EL0 >= CNTV_CVAL_EL0 (虚拟定时器)
                │
                ▼
        CNTP_CTL_EL0.ISTATUS = 1  (或 CNTV_CTL_EL0.ISTATUS = 1)
                │
                如果 CNTP_CTL_EL0.IMASK = 0 且 ENABLE = 1
                │
                ▼
        GIC 产生 PPI 中断
                │
                ▼
        CPU 异常向量表 → 跳转到 IRQ 处理
═══════════════════════════════════════════════════════════════════
```

### 8.2 中断处理流程

```
[硬件] System Counter >= CVAL  →  ISTATUS=1  →  PPI 中断
     │
     ▼
[GIC] 中断分发到目标 CPU
     │
     ▼
[CPU] 异常入口 → el1_irq()
     │
     ▼
[Kernel] handle_arch_irq() → gic_handle_irq()
     │
     ▼
[Kernel] __handle_domain_irq() → generic_handle_irq()
     │
     ▼
[Kernel] arch_timer_handler_virt(irq, dev_id)
     │
     ├─ ctrl = read_sysreg(cntv_ctl_el0)       // 读取状态
     ├─ if (ctrl & ARCH_TIMER_CTRL_IT_STAT) {   // 检查 ISTATUS
     │    ctrl |= ARCH_TIMER_CTRL_IT_MASK       // 设置 IMASK (防止重入)
     │    write_sysreg(ctrl, cntv_ctl_el0)      // 写回
     │    evt->event_handler(evt)               // 调用 clockevent 回调
     │    return IRQ_HANDLED;
     │ }
     └─ return IRQ_NONE;
```

### 8.3 中断处理后的寄存器状态

| 寄存器 | 中断前 | 中断处理中 | 中断处理后 |
|--------|--------|-----------|-----------|
| `CNTV_CTL_EL0.ISTATUS` | 0 | 1 | 1（直到写入新 CVAL） |
| `CNTV_CTL_EL0.IMASK` | 0 | 1（设置屏蔽） | 1（直到下次 set_next_event 清除） |
| `CNTV_CTL_EL0.ENABLE` | 1 | 1 | 1 |
| GIC PPI 状态 | 未断言 | 断言 | 断言（直到 EOI） |

---

## 9. 关键数据结构

### 9.1 `struct clocksource` — 时钟源

**源码位置**: [include/linux/clocksource.h](file:///home/louis/code/linux/include/linux/clocksource.h)

```c
struct clocksource {
    u64 (*read)(struct clocksource *cs);     // 读取硬件计数器
    u64 mask;                                 // 计数器掩码 (56~64位宽)
    u32 mult;                                 // 周期→纳秒 转换因子
    u32 shift;                                // 右移位数
    u64 max_idle_ns;                          // 最大空闲时间（NOHZ 使用）
    enum vdso_clock_mode vdso_clock_mode;     // VDSO 时钟模式
    const char *name;                         // "arch_sys_counter"
    int rating;                               // 400
    enum clocksource_ids id;                  // CSID_ARM_ARCH_COUNTER
    unsigned long flags;                      // CLOCK_SOURCE_IS_CONTINUOUS
    // ...
};
```

**ARM64 Arch Timer 实例**（[drivers/clocksource/arm_arch_timer.c:145](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L145)）：

```c
static struct clocksource clocksource_counter = {
    .name   = "arch_sys_counter",
    .id     = CSID_ARM_ARCH_COUNTER,
    .rating = 400,
    .read   = arch_counter_read,          // → arch_timer_read_counter()
    .flags  = CLOCK_SOURCE_IS_CONTINUOUS,
};
```

### 9.2 `struct clock_event_device` — 时钟事件设备

**源码位置**: [include/linux/clockchips.h](file:///home/louis/code/linux/include/linux/clockchips.h)

```c
struct clock_event_device {
    void (*event_handler)(struct clock_event_device *);  // 中断回调
    int (*set_next_event)(unsigned long evt, struct clock_event_device *);
    int (*set_state_shutdown)(struct clock_event_device *);
    int (*set_state_oneshot_stopped)(struct clock_event_device *);
    u64 max_delta_ticks;
    u64 min_delta_ticks;
    u32 mult;
    u32 shift;
    const char *name;                     // "arch_sys_timer"
    int rating;                           // 450
    int irq;                              // PPI 号
    const struct cpumask *cpumask;        // 绑定到单个 CPU
    unsigned long features;               // CLOCK_EVT_FEAT_ONESHOT
    // ...
};
```

### 9.3 `struct arch_timer_erratum_workaround` — Errata 处理

**源码位置**: [arch/arm64/include/asm/arch_timer.h:46](file:///home/louis/code/linux/arch/arm64/include/asm/arch_timer.h#L46)

```c
struct arch_timer_erratum_workaround {
    enum arch_timer_erratum_match_type match_type;  // DT/ACPI/CAP 匹配方式
    const void *id;                                   // 匹配 ID
    const char *desc;                                 // 描述
    u64 (*read_cntpct_el0)(void);                     // 替换的物理计数器读取
    u64 (*read_cntvct_el0)(void);                     // 替换的虚拟计数器读取
    int (*set_next_event_phys)(unsigned long, struct clock_event_device *);
    int (*set_next_event_virt)(unsigned long, struct clock_event_device *);
    bool disable_compat_vdso;                         // 禁用兼容 VDSO
};
```

### 9.4 `struct arch_timer_mem` / `struct arch_timer_mem_frame` — MMIO 定时器

**源码位置**: [include/clocksource/arm_arch_timer.h:64](file:///home/louis/code/linux/include/clocksource/arm_arch_timer.h#L64)

```c
struct arch_timer_mem_frame {
    bool valid;                   // 该 frame 是否有效
    phys_addr_t cntbase;          // Frame 寄存器基址
    size_t size;                  // Frame 大小
    int phys_irq;                 // 物理定时器 SPI
    int virt_irq;                 // 虚拟定时器 SPI
};

struct arch_timer_mem {
    phys_addr_t cntctlbase;       // CNTCTLBase 地址
    size_t size;                  // 控制块大小
    struct arch_timer_mem_frame frame[ARCH_TIMER_MEM_MAX_FRAMES];  // 最多 8 个 frame
};
```

### 9.5 `struct arch_timer` — MMIO 定时器驱动程序内部结构

**源码位置**: [drivers/clocksource/arm_arch_timer_mmio.c:46](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer_mmio.c#L46)

```c
struct arch_timer {
    struct clock_event_device evt;    // clockevent 设备
    struct clocksource cs;            // clocksource 设备
    struct arch_timer_mem *gt_block;  // GT 块描述
    void __iomem *base;               // Frame 寄存器映射
    enum arch_timer_access access;    // PHYS_ACCESS 或 VIRT_ACCESS
    u32 rate;                         // 计数器频率
};
```

### 9.6 全局变量

| 变量 | 类型 | 描述 |
|------|------|------|
| `arch_timer_rate` | `u32` | 计数器频率（Hz） |
| `arch_timer_ppi[]` | `int[5]` | 各 PPI 中断号 |
| `arch_timer_uses_ppi` | `enum arch_timer_ppi_nr` | 选中的 PPI 类型 |
| `arch_timer_evt` | `struct clock_event_device __percpu *` | 每 CPU clockevent 设备 |
| `arch_timer_c3stop` | `bool` | 空闲时定时器是否停止 |
| `arch_timer_read_counter` | `u64 (*)(void)` | 函数指针，当前使用的计数器读取函数 |
| `arch_counter_suspend_stop` | `bool` | 挂起时计数器是否停止 |

---

## 10. 完整配置流程总图

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                          ARM64 Generic Timer 完整配置流程                              │
└─────────────────────────────────────────────────────────────────────────────────────┘

=== Phase 0: 硬件复位 ===
  寄存器状态: CNTHCTL_EL2=0, CNTVOFF_EL2=未定义, CNTKCTL_EL1=0
  定时器状态: 关闭, 中断掩码

         │
         ▼
=== Phase 1: EL2 Boot Code (head.S → __init_el2_timers) ===
  CNTHCTL_EL2 = 0x3 (nVHE) 或 0xC00 (VHE)
  └─ EL1PCTEN=1, EL1PCEN=1  → EL1 可访问物理定时器和计数器
  CNTVOFF_EL2 = 0x0          → CNTVCT = CNTPCT

         │
         ▼
=== Phase 2: 驱动探测 (time_init) ===
  ┌── DT: arch_timer_of_init() ──┐   ┌── ACPI: arch_timer_acpi_init() ──┐
  │  ├─ arch_timer_ppi[] = PPI  │   │  ├─ arch_timer_ppi[] = PPI      │
  │  ├─ arch_timer_rate = freq  │   │  ├─ arch_timer_rate = CNTFRQ    │
  │  └─ arch_timer_uses_ppi     │   │  └─ arch_timer_uses_ppi         │
  └─────────────────────────────┘   └─────────────────────────────────┘
  ┌── MMIO: arch_timer_mmio_probe() ─────────────────────────────┐
  │  ├─ 解析 CNTCTLBase + Frame                                  │
  │  ├─ find_best_frame() → 选择 VIRT/PHYS frame                 │
  │  └─ ioremap + 注册 clocksource + clockevent                  │
  └──────────────────────────────────────────────────────────────┘

         │
         ▼
=== Phase 3: 中断注册 (arch_timer_register) ===
  request_percpu_irq(PPI, handler, "arch_timer")
  └─ handler = arch_timer_handler_virt/phys

         │
         ▼
=== Phase 4: Per-CPU 配置 (CPU hotplug) ===
  arch_timer_starting_cpu(cpu):
  ├─ __arch_timer_setup()
  │   ├─ 关闭定时器: CNTV_CTL_EL0.ENABLE = 0
  │   └─ clockevents_config_and_register()
  ├─ enable_percpu_irq(PPI)        ← 使能 GIC 中断
  └─ arch_counter_set_user_access()
      └─ CNTKCTL_EL1.EL0VCTEN = 1  ← VDSO 需要

         │
         ▼
=== Phase 5: Clocksource 注册 (arch_timer_common_init) ===
  arch_counter_register():
  ├─ arch_timer_read_counter = cntvct/cntpct
  ├─ clocksource_register_hz(&clocksource_counter, rate)
  │   └─ rating=400, CSID_ARM_ARCH_COUNTER
  ├─ timecounter_init()  ← KVM
  └─ sched_clock_register()  ← 调度时钟

         │
         ▼
=== Phase 6: Event Stream 配置 (core_initcall) ===
  arch_timer_evtstrm_register():
  └─ CNTKCTL_EL1.EVNTEN=1, EVNTI=divider
     └─ 100us 间隔的事件流

         │
         ▼
=== 运行时状态 ===
  Clocksource:   arch_timer_read_counter() → cntvct_el0
  Clockevent:    set_next_event → CNTV_CVAL_EL0 = current + delta
                     │
                     ▼
  中断处理:      arch_timer_handler_virt()
                 → 读 CNTV_CTL_EL0, 检查 ISTATUS
                 → 设置 IMASK, 调用 event_handler()
```

---

## 11. 文件清单

| 文件 | 说明 |
|------|------|
| [drivers/clocksource/arm_arch_timer.c](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c) | CP15 sysreg 定时器驱动（主驱动） |
| [drivers/clocksource/arm_arch_timer_mmio.c](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer_mmio.c) | MMIO 内存映射定时器驱动 |
| [arch/arm64/include/asm/arch_timer.h](file:///home/louis/code/linux/arch/arm64/include/asm/arch_timer.h) | 架构相关的定时器访问函数 |
| [arch/arm64/include/asm/sysreg.h](file:///home/louis/code/linux/arch/arm64/include/asm/sysreg.h) | 系统寄存器编码定义 |
| [arch/arm64/include/asm/el2_setup.h](file:///home/louis/code/linux/arch/arm64/include/asm/el2_setup.h) | EL2 启动代码中的定时器配置 |
| [include/clocksource/arm_arch_timer.h](file:///home/louis/code/linux/include/clocksource/arm_arch_timer.h) | 定时器驱动公共头文件 |
| [include/linux/clocksource.h](file:///home/louis/code/linux/include/linux/clocksource.h) | Clocksource 核心数据结构 |
| [include/linux/clockchips.h](file:///home/louis/code/linux/include/linux/clockchips.h) | Clockevent 核心数据结构 |
| [arch/arm64/include/asm/timex.h](file:///home/louis/code/linux/arch/arm64/include/asm/timex.h) | ARM64 timex 接口（`get_cycles`） |