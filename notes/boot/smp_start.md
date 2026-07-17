# ARM64 SMP 启动流程

## 目录

1. [概述](#概述)
2. [关键数据结构](#关键数据结构)
3. [CPU 枚举阶段（setup_arch）](#cpu-枚举阶段setup_arch)
4. [Boot CPU 准备阶段（smp_prepare_boot_cpu）](#boot-cpu-准备阶段smp_prepare_boot_cpu)
5. [从 CPU 准备阶段（smp_prepare_cpus）](#从-cpu-准备阶段smp_prepare_cpus)
6. [CPU 启动阶段（smp_init）](#cpu-启动阶段smp_init)
7. [从 CPU 启动入口（汇编）](#从-cpu-启动入口汇编)
8. [从 CPU 内核初始化](#从-cpu-内核初始化)
9. [CPU 热插拔流程](#cpu-热插拔流程)
10. [三种启动方式对比](#三种启动方式对比)
11. [关键设计点](#关键设计点)

---

## 概述

ARM64 Linux SMP（Symmetric Multi-Processing）启动流程涉及**主 CPU（boot CPU）**枚举并启动**从 CPU（secondary CPU）**的完整过程。支持三种启动方式：

- **spin-table**：通过特定内存地址传递跳转地址，由主 CPU 写入 release 地址，从 CPU 轮询该地址并跳转
- **PSCI**（Power State Coordination Interface）：通过 `smc`/`hvc` 指令调用固件（如 EL3 Secure Monitor）提供的接口，由固件负责从 CPU 的电源管理和启动
- **ACPI Parking Protocol**：ACPI 系统专用的启动协议，通过 mailbox 机制传递入口地址，由主 CPU 写入 mailbox 后发送 wakeup IPI 唤醒从 CPU

### 整体时序概览

```
start_kernel()                                                      [init/main.c]
│
├─ setup_arch()                                                      [setup.c]
│     ├─ smp_setup_processor_id()       ── 设置 boot CPU 逻辑 ID
│     ├─ init_bootcpu_ops()             ── 初始化 boot CPU 的 cpu_ops
│     ├─ smp_init_cpus()                ── 枚举所有 possible CPU
│     └─ smp_build_mpidr_hash()         ── 预计算 MPIDR hash
│
├─ setup_per_cpu_areas()                                            [percpu.c]
│     └─ smp_prepare_boot_cpu()          ── 初始化 boot CPU per-CPU 区域
│
└─ kernel_init_freeable()                                           [init/main.c]
      │
      ├─ smp_prepare_cpus(setup_max_cpus)  ── 准备从 CPU 启动
      ├─ workqueue_init()                  ── 初始化工作队列子系统
      ├─ init_mm_internals()               ── 初始化 MM 内部统计
      ├─ do_pre_smp_initcalls()            ── 执行 early_initcall 级别 initcall
      ├─ lockup_detector_init()            ── 初始化 lockup 检测器（watchdog）
      ├─ smp_init()                        ── 启动所有从 CPU
      │     │
      │     ├─ idle_threads_init()         ── 为每个 CPU 创建 idle 线程
      │     ├─ cpuhp_threads_init()        ── 初始化 hotplug 状态机线程
      │     ├─ bringup_nonboot_cpus()      ── 逐个/并行启动从 CPU
      │     └─ smp_cpus_done()             ── 启动后收尾工作
      │
      └─ sched_init_smp()                  ── SMP 调度器初始化（sched domain 构建）
```

---

## 关键数据结构

### struct cpu_operations

定义在 [arch/arm64/include/asm/cpu_ops.h](file:///home/louis/code/linux/arch/arm64/include/asm/cpu_ops.h)，是 CPU 启动/热插拔的操作抽象层：

```c
struct cpu_operations {
    const char  *name;          // 名称，匹配 DT 或 ACPI 中的 enable-method
    int     (*cpu_init)(unsigned int);      // 初始化 CPU ops 数据（如读取 release-addr）
    int     (*cpu_prepare)(unsigned int);   // 准备启动（早期一次性准备，如写入 release 地址）
    int     (*cpu_boot)(unsigned int);      // 启动 CPU
    void    (*cpu_postboot)(void);          // 从 CPU 启动后的后处理（在从 CPU 上执行）
#ifdef CONFIG_HOTPLUG_CPU
    bool    (*cpu_can_disable)(unsigned int cpu);
    int     (*cpu_disable)(unsigned int cpu);
    void    (*cpu_die)(unsigned int cpu);
    int     (*cpu_kill)(unsigned int cpu);
#endif
};
```

### struct secondary_data

定义在 [arch/arm64/include/asm/smp.h](file:///home/louis/code/linux/arch/arm64/include/asm/smp.h)，主 CPU 通过此结构向从 CPU 传递启动参数：

```c
struct secondary_data {
    struct task_struct *task;   // 从 CPU 的 idle 进程 task_struct
    long status;                // 启动状态返回值
};
extern struct secondary_data secondary_data;
extern long __early_cpu_boot_status;
```

### CPU 启动状态码

定义在 [arch/arm64/include/asm/smp.h](file:///home/louis/code/linux/arch/arm64/include/asm/smp.h#L10)：

```c
#define CPU_MMU_OFF          (-1)    // 初始状态，MMU 关闭
#define CPU_BOOT_SUCCESS     (0)     // 启动成功
#define CPU_KILL_ME          (1)     // 从 CPU 已调用 cpu_die，需同步确认
#define CPU_STUCK_IN_KERNEL  (2)     // 从 CPU 卡在内核中无法正常退出
#define CPU_PANIC_KERNEL     (3)     // 从 CPU 检测到致命系统错误，触发 panic
```

### 状态更新函数 / 宏

**C 语言版本**（用于 secondary_start_kernel）：

```c
// arch/arm64/include/asm/smp.h
static inline void update_cpu_boot_status(int val)
{
    WRITE_ONCE(secondary_data.status, val);
    dsb(ishst);  // 确保状态对其他 CPU 可见
}
```

**汇编版本**（用于 head.S 早期启动失败路径，MMU 关闭时使用）：

```asm
// arch/arm64/kernel/head.S
.macro update_early_cpu_boot_status status, tmp1, tmp2
    mov \tmp2, #\status
    adr_l \tmp1, __early_cpu_boot_status
    str \tmp2, [\tmp1]
    dmb sy
    dc  ivac, \tmp1         // Invalidate potentially stale cache line
.endm
```

### CPU 状态位图

Linux 使用四个 cpumask 管理 CPU 生命周期：

| 位图 | 含义 | 设置时机 |
|------|------|---------|
| `cpu_possible_mask` | 系统可能存在的 CPU（硬件存在） | `smp_init_cpus()` 中设置 |
| `cpu_present_mask` | 当前已上电的 CPU（hotplug 可插拔） | `smp_prepare_cpus()` 中设置 |
| `cpu_online_mask` | 正在运行的 CPU（可调度任务） | `secondary_start_kernel()` 中设置 |
| `cpu_active_mask` | 可参与负载均衡的 CPU（通常等于 online） | 随 online 同步更新 |

### cpu_ops 注册表

定义在 [arch/arm64/kernel/cpu_ops.c](file:///home/louis/code/linux/arch/arm64/kernel/cpu_ops.c)：

```c
static const struct cpu_operations *cpu_ops[NR_CPUS] __ro_after_init;

// DT 支持的 enable-method
static const struct cpu_operations *const dt_supported_cpu_ops[] __initconst = {
    &smp_spin_table_ops,
    &cpu_psci_ops,
    NULL,
};

// ACPI 支持的 enable-method
static const struct cpu_operations *const acpi_supported_cpu_ops[] __initconst = {
#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
    &acpi_parking_protocol_ops,
#endif
    &cpu_psci_ops,
    NULL,
};
```

---

## CPU 枚举阶段（setup_arch）

在 `setup_arch()` 中完成 CPU 的发现和枚举。

```
setup_arch()                                                      [setup.c:286]
│
├─ smp_setup_processor_id()                                       [setup.c:90]
│     从 MPIDR_EL1 寄存器读取 boot CPU 的硬件 ID
│     设置 cpu_logical_map(0) = mpidr
│
├─ init_bootcpu_ops()                                             [cpu_ops.c]
│     初始化 boot CPU 的 cpu_ops（通过 init_cpu_ops(0)）
│     ├─ cpu_read_enable_method(0) —— 从 DT/ACPI 读取 enable-method
│     │     DT:  读取 cpu 节点 "enable-method" 属性
│     │     ACPI: 读取 MADT 表中指定的 enable-method
│     └─ cpu_get_ops(name) —— 在 dt_supported_cpu_ops / acpi_supported_cpu_ops
│                             中匹配对应的 cpu_operations
│
├─ smp_init_cpus()                                                [smp.c:642]
│     枚举系统中所有可能的 CPU
│     │
│     ├─ [if acpi_disabled] of_parse_and_init_cpus()              [smp.c:617]
│     │     遍历 DT 中所有 cpu 节点
│     │     ├─ 读取每个 CPU 节点的 reg 属性（MPIDR 值）
│     │     ├─ 检查 MPIDR 有效性（hwid & ~MPIDR_HWID_BITMASK）
│     │     ├─ 检查 MPIDR 是否重复（is_mpidr_duplicate）
│     │     ├─ 匹配 boot CPU 的 MPIDR → 标记 bootcpu_valid
│     │     ├─ 非 boot CPU → 设置 cpu_logical_map(cpu_count, hwid)
│     │     └─ early_map_cpu_to_node() —— 建立 CPU 到 NUMA 节点的映射
│     │
│     └─ [else] acpi_parse_and_init_cpus()                        [smp.c:651]
│           解析 ACPI MADT 表中的 GICC（Generic Interrupt Controller）项
│           ├─ acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_INTERRUPT,
│           │                       acpi_parse_gic_cpu_interface, 0)
│           │    回调 acpi_map_gic_cpu_interface() —— 解析每个 GICC 项
│           │    ├─ 检查 flags（ACPI_MADT_ENABLED | ACPI_MADT_GICC_ONLINE_CAPABLE）
│           │    ├─ 检查 MPIDR 有效性和重复性
│           │    ├─ 匹配 boot CPU 的 MPIDR → 标记 bootcpu_valid
│           │    ├─ 非 boot CPU → set_cpu_logical_map(cpu_count, hwid)
│           │    └─ acpi_set_mailbox_entry(cpu_count, processor)
│           │          保存 parking protocol 所需的 mailbox 地址、GIC CPU ID 等信息
│           │          到 cpu_mailbox_entries[cpu_count]（仅在 CONFIG_ARM64_ACPI_PARKING_PROTOCOL 时有效）
│           ├─ acpi_map_cpus_to_nodes() —— 从 SRAT 表映射 CPU 到 NUMA 节点
│           └─ for_each_cpu(i) early_map_cpu_to_node(i, ...)
│
│     └─ for_each_possible_cpu(i=1..nr_cpu_ids)                   [smp.c:493]
│            ├─ if cpu_logical_map(i) != INVALID_HWID
│            └─ smp_cpu_setup(i)                                  [smp.c:487]
│                  ├─ init_cpu_ops(cpu) —— 读取 enable-method 并绑定 cpu_ops
│                  │     ├─ cpu_read_enable_method(cpu)
│                  │     └─ cpu_get_ops(enable_method)
│                  ├─ ops->cpu_init(cpu) —— 调用具体方法的 cpu_init 回调
│                  │     ├─ spin-table: smp_spin_table_cpu_init()
│                  │     │     读取 "cpu-release-addr" 属性，保存到 cpu_release_addr[cpu]
│                  │     └─ PSCI: cpu_psci_cpu_init() —— 空操作（不需要额外初始化）
│                  └─ set_cpu_possible(cpu, true) —— 标记为 possible CPU
│
└─ smp_build_mpidr_hash()                                         [setup.c:109]
       预计算 MPIDR 各 affinity level 的移位值，用于构建线性索引
       通过 XOR 所有 possible CPU 的 MPIDR，找出实际变化的 bit 位
       计算每个 affinity level 需要的 bit 数，生成 hash 移位参数
```

---

## Boot CPU 准备阶段（smp_prepare_boot_cpu）

`smp_prepare_boot_cpu()` 在 `start_kernel()` → `setup_per_cpu_areas()` 中被调用，早于 `kernel_init_freeable()` 中的从 CPU 准备流程。它负责将 boot CPU 从早期临时 per-CPU 区域切换到运行时 per-CPU 区域，并初始化 boot CPU 的 feature 和中断掩码。

```
start_kernel()                                                     [init/main.c]
│
└─ setup_per_cpu_areas()                                           [percpu.c]
     │
     └─ smp_prepare_boot_cpu()                                     [smp.c:447]
           │
           ├─ set_my_cpu_offset(per_cpu_offset(smp_processor_id()))
           │    将 boot CPU 的 per-CPU 区域从临时区域切换到运行时区域
           │    （setup_per_cpu_areas() 分配的运行时 per-CPU 区域）
           │
           ├─ cpuinfo_store_boot_cpu()
           │    保存 boot CPU 的详细信息（CPU ID、频率、feature 寄存器等）
           │
           ├─ setup_boot_cpu_features()
           │    初始化 boot CPU 的 CPU feature 和 errata workaround
           │    ├─ 通过内核链接生成的数据（__cpu_hwcaps、__cpu_errata 等）
           │    └─ 设置 boot CPU 的 capabilities 位图
           │
           ├─ [if system_uses_irq_prio_masking()]
           │  init_gic_priority_masking()
           │    条件性切换到 GIC PMR 进行中断掩码
           │    ├─ 检查 GIC SRE 是否可用
           │    └─ 设置 PMR 寄存器
           │
           ├─ kasan_init_hw_tags()
           │    初始化硬件辅助 KASAN（MTE 标签）
           │
           └─ kasan_init_sw_tags()
                 初始化软件 KASAN 随机标签种子
```

---

## 从 CPU 准备阶段（smp_prepare_cpus）

```
kernel_init_freeable()                                            [init/main.c]
│
├─ smp_prepare_cpus(setup_max_cpus)                               [smp.c:710]
│     │
│     ├─ init_cpu_topology()
│     │     初始化 CPU 拓扑结构（sched domain 的基础）
│     │
│     ├─ store_cpu_topology(this_cpu)
│     │     保存 boot CPU 的拓扑信息
│     │
│     ├─ numa_store_cpu_info(this_cpu)
│     │     保存 boot CPU 的 NUMA 信息
│     │
│     ├─ numa_add_cpu(this_cpu)
│     │     将 boot CPU 加入 NUMA 节点
│     │
│     ├─ [if max_cpus == 0] return
│     │     单核模式（nosmp 或 maxcpus=0），不准备从 CPU
│     │
│     └─ for_each_possible_cpu(cpu)                               [smp.c:730]
│            │ 跳过 boot CPU（cpu == smp_processor_id()）
│            │
│            ├─ ops = get_cpu_ops(cpu) —— 获取该 CPU 的 cpu_operations
│            ├─ [if !ops] continue —— 无 ops 则跳过
│            │
│            ├─ err = ops->cpu_prepare(cpu) —— 调用具体方法的 cpu_prepare 回调
│            │     ├─ spin-table: smp_spin_table_cpu_prepare(cpu)
│            │     │     将 secondary_holding_pen 的物理地址写入
│            │     │     cpu-release-addr 指向的内存位置
│            │     │     ├─ ioremap_cache(cpu_release_addr[cpu])
│            │     │     │   映射 release 地址到内核线性地址空间
│            │     │     ├─ writeq_relaxed(pa_holding_pen, release_addr)
│            │     │     │   写入 secondary_holding_pen 的物理地址
│            │     │     ├─ dcache_clean_inval_poc() —— 刷新 cache 到 PoC
│            │     │     │   确保从 CPU（可能不在 cache 一致性域中）能读到正确值
│            │     │     ├─ sev() —— 发送事件唤醒从 CPU
│            │     │     └─ iounmap(release_addr)
│            │     │
│            │     ├─ PSCI: cpu_psci_cpu_prepare(cpu)             [psci.c:20]
│            │     │     检查 psci_ops.cpu_on 是否可用
│            │     │     若不可用则返回 -ENODEV
│            │     │
│            │     └─ ACPI Parking Protocol:                    [acpi_parking_protocol.c]
│            │            acpi_parking_protocol_cpu_prepare()
│            │            空操作（无需额外准备）
│            │
│            ├─ [if err] continue —— 准备失败则跳过
│            │
│            ├─ set_cpu_present(cpu, true) —— 标记为 present CPU
│            │
│            └─ numa_store_cpu_info(cpu)
│                  保存该 CPU 的 NUMA 信息
│
├─ do_pre_smp_initcalls()
│     执行 early_initcall 级别的 initcall
│     某些子系统（如 GIC 中断控制器）需要在 SMP 启动前初始化
│     （core_initcall 及后续级别在 do_basic_setup() → do_initcalls() 中执行）
│
├─ lockup_detector_init()
│     初始化 hard/soft lockup 检测器（watchdog）
│     需要 SMP 启动前初始化，因为从 CPU 启动后会开始检测
│
└─ smp_init()                                                      [kernel/smp.c:992]
```

---

## CPU 启动阶段（smp_init）

### 总览

```
smp_init()                                                         [kernel/smp.c:992]
│
├─ idle_threads_init()
│     为每个 possible CPU 创建 idle 线程（task_struct）
│     每个 idle 线程有独立的栈空间
│
├─ cpuhp_threads_init()
│     初始化 CPU hotplug 状态机线程
│
├─ bringup_nonboot_cpus(setup_max_cpus)                            [kernel/cpu.c:1870]
│     │
│     ├─ [if max_cpus == 0] return
│     │
│     ├─ [if cpuhp_bringup_cpus_parallel(max_cpus)]                [kernel/cpu.c:1831]
│     │     并行启动优化（CONFIG_HOTPLUG_PARALLEL）
│     │     ├─ arch_cpuhp_init_parallel_bringup() 检查架构支持
│     │     ├─ 如果 SMT aware:
│     │     │   先启动 primary thread → CPUHP_ONLINE
│     │     │   再启动 sibling thread → CPUHP_ONLINE
│     │     └─ 否则直接全部启动 → CPUHP_ONLINE
│     │
│     └─ [serialized] cpuhp_bringup_mask(cpu_present_mask, ...)    [kernel/cpu.c:1764]
│            逐个串联启动每个 present CPU
│            │
│            └─ for_each_cpu(cpu, mask)
│                   cpu_up(cpu, target)                           [kernel/cpu.c]
│                   │
│                   └─ bringup_cpu(cpu)                            [kernel/cpu.c:860]
│                         │
│                         ├─ idle = idle_thread_get(cpu)
│                         │     获取该 CPU 对应的 idle 线程
│                         │
│                         ├─ irq_lock_sparse()
│                         │     锁住稀疏 IRQ 空间，防止并发分配中断
│                         │
│                         ├─ __cpu_up(cpu, idle)                    [smp.c:108]
│                         │     架构相关：启动从 CPU
│                         │
│                         ├─ cpuhp_bp_sync_alive(cpu)
│                         │     同步等待从 CPU 确认存活
│                         │
│                         ├─ bringup_wait_for_ap_online(cpu)
│                         │     等待从 CPU 完成 hotplug 状态机到 ONLINE
│                         │
│                         └─ irq_unlock_sparse()
│
├─ smp_cpus_done(setup_max_cpus)                                    [smp.c:470]
│     │
│     ├─ hyp_mode_check()                                          [smp.c:440]
│     │     检查所有 CPU 是否在一致的异常级别启动
│     │     ├─ 全部 EL2 → "All CPU(s) started at EL2"
│     │     ├─ 不一致 → WARN_TAINT(TAINT_CPU_OUT_OF_SPEC)
│     │     └─ 全部 EL1 → "All CPU(s) started at EL1"
│     │
│     ├─ setup_system_features()                                   [cpufeature.c:3976]
│     │     │  设置系统级 CPU feature
│     │     ├─ setup_system_capabilities()
│     │     │    └─ update_cpu_capabilities(SCOPE_SYSTEM)
│     │     │      对所有 CPU 达成一致的 capabilities 进行全局确认
│     │     ├─ linear_map_maybe_split_to_ptes()
│     │     ├─ kpti_install_ng_mappings()
│     │     │    KPTI 页表隔离设置
│     │     ├─ sve_setup()
│     │     └─ sme_setup()
│     │
│     ├─ setup_user_features()                                     [cpufeature.c:3994]
│     │     │  设置用户空间可见的 CPU feature
│     │     ├─ user_feature_fixup()
│     │     ├─ setup_elf_hwcaps(arm64_elf_hwcaps)
│     │     │    向用户空间暴露 CPU 硬件能力（/proc/cpuinfo）
│     │     ├─ [if 32bit_el0] compat_elf_hwcaps 设置
│     │     └─ minsigstksz_setup()
│     │
│     └─ mark_linear_text_alias_ro()
│
└─ "Brought up %d nodes, %d CPUs"
```

### __cpu_up 详细流程

```
__cpu_up(cpu, idle)                                                [smp.c:108]
│
├─ secondary_data.task = idle
│     设置从 CPU 的 idle 进程 task_struct
│     从 CPU 在 __secondary_switched 中通过这个 task 设置栈指针
│
├─ update_cpu_boot_status(CPU_MMU_OFF)
│     设置启动状态为 MMU_OFF（初始状态）
│     使用 WRITE_ONCE + dsb(ishst) 确保可见性
│
├─ boot_secondary(cpu, idle)                                       [smp.c:100]
│     │
│     └─ ops->cpu_boot(cpu)
│           │
│           ├─ spin-table: smp_spin_table_cpu_boot(cpu)            [smp_spin_table.c:107]
│           │     写入 pen_release = cpu_logical_map(cpu)
│           │     从 CPU 在 secondary_holding_pen 中轮询此值
│           │     ├─ write_pen_release(cpu_logical_map(cpu))
│           │     │    写入要启动的 CPU 的 MPIDR
│           │     │    并 dcache_clean_inval_poc 刷新 cache
│           │     └─ sev() —— 发送事件唤醒从 CPU
│           │
│           ├─ PSCI: cpu_psci_cpu_boot(cpu)                        [psci.c:29]
│           │     ├─ pa_secondary_entry = __pa_symbol(secondary_entry)
│           │     │    获取 secondary_entry 函数标签的物理地址
│           │     │    （从 CPU 此时未开启 MMU，需使用物理地址）
│           │     └─ psci_ops.cpu_on(cpu_logical_map(cpu),
│           │                          pa_secondary_entry)
│           │           通过 smc 指令调用 EL3 固件
│           │           固件将从 CPU 从低功耗状态唤醒
│           │           并跳转到 pa_secondary_entry 执行
│           │
│           └─ ACPI Parking Protocol:                             [acpi_parking_protocol.c:67]
│                  acpi_parking_protocol_cpu_boot(cpu)
│                  ├─ ioremap(cpu_entry->mailbox_addr) —— 映射 mailbox
│                  ├─ readl_relaxed(&mailbox->cpu_id)
│                  │    检查 firmware 是否将 cpu_id 初始化为 ~0U
│                  │    否则说明 mailbox 尚未就绪，返回 -ENXIO
│                  ├─ writeq_relaxed(__pa_symbol(secondary_entry),
│                  │                  &mailbox->entry_point)
│                  │    写入 secondary_entry 的物理地址
│                  ├─ writel_relaxed(cpu_entry->gic_cpu_id,
│                  │                 &mailbox->cpu_id)
│                  │    写入 GIC CPU ID 唤醒目标 CPU
│                  └─ arch_send_wakeup_ipi(cpu)                    [smp.c:1157]
│                        通过 smp_send_reschedule(cpu) 发送调度 IPI
│                        从 CPU 收到 IPI 后检查 mailbox 并跳转到 secondary_entry
│
│
├─ wait_for_completion_timeout(&cpu_running, 5s)
│     等待从 CPU 启动完成
│     从 CPU 在 secondary_start_kernel() 中 complete(&cpu_running)
│
├─ [if cpu_online(cpu)] return 0
│     启动成功
│
└─ [else] 超时处理
       │
       ├─ secondary_data.task = NULL
       ├─ status = READ_ONCE(secondary_data.status)
       │    先读取 secondary_data.status
       ├─ [if status == CPU_MMU_OFF]
       │    status = READ_ONCE(__early_cpu_boot_status)
       │    如果还是 MMU_OFF 状态（MMU 都还没开），
       │    再检查 __early_cpu_boot_status（汇编写入的早期状态）
       │
       └─ switch (status & CPU_BOOT_STATUS_MASK):
              ├─ CPU_KILL_ME → 从 CPU 已调用 cpu_die，尝试同步
              ├─ CPU_STUCK_IN_KERNEL → 从 CPU 卡在内核中
              │    ├─ CPU_STUCK_REASON_52_BIT_VA → 不支持 52-bit VA
              │    └─ CPU_STUCK_REASON_NO_GRAN → 不支持当前页面粒度
              ├─ CPU_PANIC_KERNEL → 从 CPU 触发内核 panic
              └─ default → 未知状态
```

---

## 从 CPU 启动入口（汇编）

### 1. PSCI 方式入口

```
secondary_entry                                                   [head.S:373]
│  PSCI 方式启动的从 CPU 直接跳转到此入口
│  由固件（EL3）通过 smc 指令响应后设置
│
├─ mov x0, xzr
│  bl init_kernel_el
│     初始化从 CPU 的异常级别
│     ├─ 如果当前在 EL2 → 降级到 EL1（或 VHE 模式保持 EL2）
│     └─ 如果当前在 EL1 → 直接返回
│     w0 = cpu_boot_mode（BOOT_CPU_MODE_EL1 或 BOOT_CPU_MODE_EL2）
│
└─ b secondary_startup
```

### 2. Spin-table 方式入口

```
secondary_holding_pen                                             [head.S:355]
│  spin-table 方式启动的从 CPU 在 bootloader 中轮询
│  cpu-release-addr 地址，被主 CPU 的 sev() 唤醒后跳转到此
│
├─ mov x0, xzr
│  bl init_kernel_el
│     初始化异常级别
│
├─ mrs x2, mpidr_el1              —— 读取当前 CPU 的 MPIDR
├─ mov_q x1, MPIDR_HWID_BITMASK
├─ and x2, x2, x1                 —— 取 MPIDR 有效位
├─ adr_l x3, secondary_holding_pen_release
│
├─ pen:                           —— 轮询循环
│  │  ldr x4, [x3]                —— 读取 pen_release 值
│  │  cmp x4, x2                  —— 比较是否匹配当前 CPU 的 MPIDR
│  │  b.eq secondary_startup      —— 匹配则开始启动
│  │  wfe                          —— 不匹配则等待事件
│  │  b pen                       —— 继续轮询
│  │
│  └─ 主 CPU 通过 smp_spin_table_cpu_boot() 调用
│      write_pen_release() 写入 cpu_logical_map(cpu)
│      并 sev() 发送事件唤醒
│
└─ （跳转到 secondary_startup）
```

### 3. 公共启动路径

```
secondary_startup                                                 [head.S:379]
│  PSCI 和 spin-table 方式在此汇合
│
├─ mov x20, x0     —— 保存 boot mode（来自 init_kernel_el 的返回值）
│
├─ [if CONFIG_ARM64_VA_BITS_52]
│  alternative_if ARM64_HAS_VA52
│      bl __cpu_secondary_check52bitva                             [head.S:494]
│      │   检查从 CPU 是否支持 52-bit 虚拟地址
│      │   ├─ 非 LPA2: 检查 ID_AA64MMFR2_EL1.VARange
│      │   ├─ LPA2:    检查 ID_AA64MMFR0_EL1.TGRAN >= LPA2
│      │   ├─ 如果不支持:
│      │   │     update_early_cpu_boot_status
│      │   │         CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_52_BIT_VA
│      │   │     wfe/wfi 死循环
│      │   └─ 如果支持: ret
│
├─ bl __cpu_setup                                                 [proc.S]
│     初始化处理器（同主 CPU 的 __cpu_setup）
│     ├─ tlbi vmalle1 —— 清空本地 TLB
│     ├─ dsb nsh
│     ├─ cpacr_el1 = 0
│     ├─ mair_el1 = MAIR_EL1_SET
│     ├─ tcr_el1 配置（T0SZ, T1SZ, IPS, 等）
│     └─ x0 = INIT_SCTLR_EL1_MMU_ON（返回 SCTLR 值，供 __enable_mmu 使用）
│
├─ adrp x1, swapper_pg_dir        —— TTBR1 页表（内核映射）
│  adrp x2, idmap_pg_dir          —— TTBR0 页表（identity mapping）
│  bl __enable_mmu                                                [head.S]
│     开启 MMU
│     ├─ 检查 ID_AA64MMFR0_EL1.TGRAN 页面粒度支持
│     │  ├─ 小于最小值 → __no_granule_support（wfe/wfi 死循环）
│     │  └─ 大于最大值 → __no_granule_support（wfe/wfi 死循环）
│     ├─ ttbr0_el1 = phys_to_ttbr(idmap_pg_dir)
│     ├─ ttbr1_el1 = load_ttbr1(swapper_pg_dir)
│     └─ set_sctlr_el1(x0) —— 写入 SCTLR，开启 MMU
│
├─ ldr x8, =__secondary_switched
│  br x8
│
└─ __secondary_switched                                           [head.S:402]
      │
      ├─ mov x0, x20
      │  bl set_cpu_boot_mode_flag(x20)
      │    将 CPU boot mode 保存到 __boot_cpu_mode 全局变量
      │
      ├─ mov x0, x20
      │  bl finalise_el2(x20)
      │    如果支持 VHE 且已启用，完成 EL2 虚拟化设置
      │
      ├─ str_l xzr, __early_cpu_boot_status, x3
      │    清除早期启动状态（表示无错误）
      │
      ├─ adr_l x5, vectors
      │  msr vbar_el1, x5
      │  isb
      │    设置从 CPU 的 EL1 中断向量表
      │
      ├─ adr_l x0, secondary_data
      │  ldr x2, [x0, #CPU_BOOT_TASK]
      │  cbz x2, __secondary_too_slow
      │    读取 secondary_data.task
      │    如果为空（主 CPU 超时释放了），进入 wfe/wfi 死循环
      │
      ├─ init_cpu_task x2, x1, x3
      │    初始化从 CPU 的 task 上下文
      │    ├─ sp_el0 = x2（task 地址）
      │    ├─ sp = task 栈顶（task.stack + THREAD_SIZE - PT_REGS_SIZE）
      │    ├─ x29 清空并设置栈帧
      │    ├─ scs_load_current（影子调用栈）
      │    └─ set_this_cpu_offset（per-CPU 变量基址寄存器 tpidr_el1）
      │
      ├─ [if CONFIG_ARM64_PTR_AUTH]
      │  ptrauth_keys_init_cpu x2, x3, x4, x5
      │    初始化指针认证密钥
      │
      └─ bl secondary_start_kernel                                 [smp.c:187]
```

---

## 从 CPU 内核初始化

```
secondary_start_kernel()                                           [smp.c:187]
│  从 CPU 进入内核后的 C 语言入口
│  此时 MMU 已开启，使用 swapper_pg_dir 内核页表
│
├─ mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK
│    读取当前 CPU 的 MPIDR 硬件 ID
│
├─ mmgrab(&init_mm)
│  current->active_mm = &init_mm
│    所有内核线程共享 init_mm 上下文
│    从 CPU 没有自己的地址空间，借用 init_mm
│
├─ cpu_uninstall_idmap()
│    取消从 CPU 的 identity mapping
│    将 TTBR0 指向空页表（reserved_pg_dir）
│    防止 speculative 取指导致意外访问
│
├─ [if system_uses_irq_prio_masking()]
│  init_gic_priority_masking()
│    初始化 GIC 优先级掩码（用于 pseudo-NMI 支持）
│    检查 GIC SRE 是否可用，设置 PMR 寄存器
│
├─ rcutree_report_cpu_starting(cpu)
│    向 RCU 子系统报告该 CPU 已启动
│    从 CPU 现在可以处理 RCU 回调
│
├─ trace_hardirqs_off()
│    跟踪：硬中断关闭状态
│
├─ check_local_cpu_capabilities()                                 [cpufeature.c:3822]
│    检查从 CPU 的 CPU feature 是否与 boot CPU 一致
│    │
│    ├─ check_early_cpu_features()
│    │    ├─ verify_cpu_asid_bits()
│    │    └─ verify_local_cpu_caps(SCOPE_BOOT_CPU)
│    │
│    ├─ [if !system_capabilities_finalized()]
│    │  update_cpu_capabilities(SCOPE_LOCAL_CPU)
│    │    系统 capabilities 尚未最终确定（如 boot CPU 阶段）
│    │    从 CPU 可以更新本地 errata workaround
│    │
│    └─ [else]
│       verify_local_cpu_capabilities()
│       系统 capabilities 已最终确定，从 CPU 必须验证一致性
│       ├─ verify_local_cpu_caps(SCOPE_ALL & ~SCOPE_BOOT_CPU)
│       ├─ verify_local_elf_hwcaps()
│       ├─ [if SVE] verify_sve_features()
│       ├─ [if SME] verify_sme_features()
│       ├─ [if hyp_mode] verify_hyp_capabilities()
│       └─ [if MPAM] verify_mpam_capabilities()
│
│    如果不匹配 → 调用 cpu_die_early() 终止启动
│           ├─ set_cpu_present(cpu, 0) —— 标记 CPU absent
│           ├─ rcutree_report_cpu_dead()
│           ├─ [if HOTPLUG_CPU] update_cpu_boot_status(CPU_KILL_ME)
│           │                   __cpu_try_die(cpu) —— 尝试关闭
│           ├─ update_cpu_boot_status(CPU_STUCK_IN_KERNEL)
│           └─ cpu_park_loop() —— wfe/wfi 死循环
│
├─ ops = get_cpu_ops(cpu)
│  [if ops->cpu_postboot]
│      ops->cpu_postboot()
│      调用 CPU 启动方法的后处理回调
│      ├─ spin-table: 未实现（ops 中无该回调）
│      ├─ PSCI: 未实现（ops 中无该回调）
│      └─ ACPI Parking Protocol: acpi_parking_protocol_cpu_postboot()
│           读取 mailbox->entry_point，WARN_ON 如果固件未清空此字段
│           （协议规定固件应在从 CPU 读取 entry_point 后将其清零，
│            此检查用于验证固件是否遵循协议规范）
│
├─ cpuinfo_store_cpu()
│    保存该 CPU 的详细信息到 cpu_data 数组
│    （包括 CPU ID、频率、feature 寄存器等）
│
├─ store_cpu_topology(cpu)
│    保存 CPU 拓扑信息（cluster、core、thread 层级）
│
├─ notify_cpu_starting(cpu)
│    通知 CPU hotplug 状态机：CPU 正在启动
│    执行 CPUHP_AP_ONLINE 之前的 hotplug 回调
│    ├─ 使能 GIC Redistributor（中断控制器）
│    ├─ 初始化 arch timer（CPU 本地定时器）
│    └─ 执行其他 CPU hotplug 启动阶段的各类回调
│
├─ ipi_setup(cpu)                                                 [smp.c:1046]
│    设置该 CPU 的 IPI（核间中断）
│    ├─ 遍历所有 IPI 类型（NR_IPI = 6）
│    │    ├─ IPI_RESCHEDULE     —— 调度 IPI
│    │    ├─ IPI_CALL_FUNC      —— 函数调用 IPI
│    │    ├─ IPI_CPU_STOP       —— CPU 停止 IPI
│    │    ├─ IPI_CPU_STOP_NMI   —— CPU 停止 NMI（如果支持 NMI）
│    │    ├─ IPI_TIMER          —— 时钟广播 IPI
│    │    └─ IPI_IRQ_WORK       —— IRQ work IPI
│    ├─ [if percpu_descs] 逐个使能 percpu IPI 中断
│    └─ [else] 使能全局 SGI IPI 中断
│           [if NMI] enable_percpu_nmi()
│           [else]   enable_percpu_irq()
│
├─ numa_add_cpu(cpu)
│    将 CPU 加入 NUMA 节点
│
├─ pr_info("CPU%u: Booted secondary processor 0x%010lx [0x%08x]\n")
│    打印启动日志
│
├─ update_cpu_boot_status(CPU_BOOT_SUCCESS)
│    更新启动状态为成功
│    WRITE_ONCE + dsb(ishst) 确保主 CPU 可见
│
├─ set_cpu_online(cpu, true)
│    标记该 CPU 为 online 状态
│    （从此刻起，该 CPU 可被调度使用）
│
├─ complete(&cpu_running)
│    通知主 CPU：从 CPU 启动完成
│    主 CPU 在 __cpu_up() 中等待此 completion
│
├─ local_daif_restore(DAIF_PROCCTX)
│    恢复 DAIF 掩码状态
│    ├─ 取消 Debug 和 SError 的掩码
│    └─ 取消 IRQ 和 FIQ 的掩码
│    （从 CPU 进入时所有异常被屏蔽，现在全部打开）
│
└─ cpu_startup_entry(CPUHP_AP_ONLINE_IDLE)
       进入 idle 循环
       ├─ 循环检查是否需要调度
       ├─ 没有任务可运行时执行 WFI 指令
       └─ CPU 进入低功耗状态，等待中断唤醒
```

---

## CPU 热插拔流程

### CPU 下线流程

```
__cpu_disable()                                                    [smp.c:300]
│  在要被关闭的 CPU 上执行
│
├─ op_cpu_disable(cpu)                                             [smp.c:282]
│     └─ ops->cpu_disable(cpu)
│           PSCI: cpu_psci_cpu_disable(cpu)                        [psci.c:42]
│           ├─ 检查 psci_ops.cpu_off 是否支持
│           ├─ 检查 Trusted OS 是否允许关闭（psci_tos_resident_on）
│           └─ 不支持则返回错误
│
├─ remove_cpu_topology(cpu)
│     移除 CPU 拓扑信息
│
├─ numa_remove_cpu(cpu)
│     从 NUMA 节点移除该 CPU
│
├─ set_cpu_online(cpu, false)
│     标记为离线（此后不再参与调度）
│
├─ ipi_teardown(cpu)                                               [smp.c:1065]
│     清理该 CPU 的 IPI 中断
│     ├─ 遍历所有 IPI 类型
│     ├─ [if NMI] disable_percpu_nmi + teardown_percpu_nmi
│     └─ [else]   disable_percpu_irq
│
└─ irq_migrate_all_off_this_cpu()
       将该 CPU 上的所有中断迁移到其他 CPU
```

```
cpu_die()                                                          [smp.c:359]
│  在要被关闭的 CPU 上执行（idle 线程上下文中）
│
├─ idle_task_exit()
│     idle 线程退出前的清理工作
│
├─ local_daif_mask()
│     屏蔽所有异常（DAIF）
│
├─ cpuhp_ap_report_dead()
│     通知 hotplug 状态机：该 CPU 已死亡
│
└─ ops->cpu_die(cpu)
      PSCI: cpu_psci_cpu_die(cpu)                                  [psci.c:54]
      调用 psci_ops.cpu_off(state) 通过 smc 指令关闭 CPU
      power_state = POWER_DOWN
      此调用不应返回，如果返回则 BUG()
```

### CPU 下线确认

```
arch_cpuhp_cleanup_dead_cpu(cpu)                                   [smp.c:349]
│  在主 CPU 上执行，确认从 CPU 已完全关闭
│
└─ op_cpu_kill(cpu)                                                [smp.c:340]
      PSCI: cpu_psci_cpu_kill(cpu)                                 [psci.c:63]
      通过 psci_ops.affinity_info(cpu_logical_map(cpu), 0) 轮询
      ├─ 如果返回 AFFINITY_LEVEL_OFF → 确认关闭
      ├─ 否则轮询最多 100ms
      └─ 超时则报警告 "may not have shut down cleanly"
```

### 从 CPU 早期启动失败处理

```
cpu_die_early()                                                    [smp.c:405]
│  从 CPU 在早期启动过程中（如 feature 不匹配）调用
│
├─ pr_crit("CPU%d: will not boot\n", cpu)
│
├─ set_cpu_present(cpu, 0)
│     标记该 CPU 为 absent
│
├─ rcutree_report_cpu_dead()
│
├─ [if CONFIG_HOTPLUG_CPU]
│  ├─ update_cpu_boot_status(CPU_KILL_ME)
│  └─ __cpu_try_die(cpu) —— 尝试调用 ops->cpu_die()
│
├─ update_cpu_boot_status(CPU_STUCK_IN_KERNEL)
│
└─ cpu_park_loop()
       wfe / wfi 死循环
```

---

## 三种启动方式对比

| 特性 | spin-table | PSCI | ACPI Parking Protocol |
|------|-----------|------|-----------------------|
| **依赖** | 不需要固件 | 需要 EL3 Secure Monitor 固件 | ACPI 固件 |
| **启动方式** | 主 CPU 写入内存地址，从 CPU 轮询 | 主 CPU 通过 smc/hvc 指令请求固件 | 主 CPU 写入 mailbox 后发送 wakeup IPI |
| **从 CPU 入口** | `secondary_holding_pen` | `secondary_entry` | `secondary_entry` |
| **电源管理** | 不支持（只有启动） | 支持 CPU_ON/CPU_OFF/SUSPEND 等 | 支持 CPU_ON/CPU_OFF |
| **热插拔支持** | 有限 | 完整支持（cpu_disable, cpu_die, cpu_kill） | 完整支持 |
| **设备树/ACPI 属性** | `enable-method = "spin-table"` | `enable-method = "psci"` | ACPI MADT 表中的 parked_address |
| **适用场景** | 简单系统、无固件环境 | 现代 ARM64 标准系统（推荐） | ACPI 系统 |
| **代码文件** | `smp_spin_table.c` | `psci.c` | `acpi_parking_protocol.c` |
| **cpu_init** | 读取 cpu-release-addr | 空操作 | 读取 MADT 中的 mailbox 地址 |
| **cpu_prepare** | 写入 holding_pen 物理地址 + sev() | 检查 cpu_on 是否可用 | 空操作 |
| **cpu_boot** | 写入 pen_release + sev() | 调用 psci_ops.cpu_on() | 写入 entry_point + cpu_id 后发送 wakeup IPI |
| **cpu_postboot** | 未实现 | 未实现 | 检查固件是否清空了 entry_point |
| **cache 维护** | 需要 dcache_clean_inval_poc | 固件负责 | 固件负责 |

---

## 关键设计点

1. **CPU 状态位图**：Linux 使用四个状态位图管理 CPU 生命周期：
   - `cpu_possible_mask`：系统可能存在的 CPU（硬件存在），在 `smp_init_cpus()` 中设置
   - `cpu_present_mask`：当前已上电的 CPU（hotplug 可插拔），在 `smp_prepare_cpus()` 中设置
   - `cpu_online_mask`：正在运行的 CPU（可调度任务），在 `secondary_start_kernel()` 中设置
   - `cpu_active_mask`：可参与负载均衡的 CPU（通常等于 online）

2. **启动时序**：从 CPU 在 `kernel_init_freeable` → `smp_init()` 阶段才被启动，在此之前只有 boot CPU 运行。`do_pre_smp_initcalls()` 在 SMP 启动前执行核心子系统的初始化（如 GIC 中断控制器）

3. **页表使用**：从 CPU 启动时使用 `idmap_pg_dir` 做 identity mapping（TTBR0），使用 `swapper_pg_dir` 做内核映射（TTBR1），启动后通过 `cpu_uninstall_idmap()` 取消 identity mapping，将 TTBR0 指向 `reserved_pg_dir`（空页表）

4. **PSCI 调用方式**：通过 `smc`（Secure Monitor Call）指令进入 EL3，由底层固件完成实际的 CPU 电源管理操作。PSCI 标准定义了 CPU_ON、CPU_OFF、CPU_SUSPEND、AFFINITY_INFO 等接口

5. **CPU feature 一致性**：`check_local_cpu_capabilities()` 确保所有 CPU 的硬件特性一致：
   - 如果系统 capabilities 尚未最终确定，从 CPU 可以更新本地 errata workaround
   - 如果已最终确定，从 CPU 必须验证完全匹配（包括 SVE、SME、hyp、MPAM 等特性）
   - 如果不匹配则通过 `cpu_die_early()` 终止该 CPU 启动

6. **spin-table 的 cache 维护**：主 CPU 写入 release 地址后必须执行 `dcache_clean_inval_poc` 确保从 CPU（可能不在 cache 一致性域中）能读取到正确的值

7. **启动状态双重传递机制**：从 CPU 的启动状态通过两个变量传递：
   - `secondary_data.status`：C 代码中通过 `update_cpu_boot_status()` 写入，使用 `WRITE_ONCE + dsb(ishst)`
   - `__early_cpu_boot_status`：汇编代码中通过 `update_early_cpu_boot_status` 宏写入，使用 `str + dmb sy + dc ivac`（MMU 关闭时使用）
   - `__cpu_up()` 先读取 `secondary_data.status`，如果还是 `CPU_MMU_OFF` 则读取 `__early_cpu_boot_status`

8. **并行启动优化**：`CONFIG_HOTPLUG_PARALLEL` 支持并行启动多个从 CPU，通过 `cpuhp_bringup_cpus_parallel()` 实现。SMT 系统会先启动 primary thread，再启动 sibling thread，以避免 microcode 更新等问题

9. **smp_prepare_boot_cpu**：在 `setup_per_cpu_areas()` 之后调用，将 boot CPU 的 per-CPU 区域从临时区域切换到运行时区域，初始化 boot CPU 的 feature 和 GIC PMR

10. **hotplug 状态机**：CPU hotplug 使用分阶段状态机（`cpuhp_state`），每个阶段有对应的启动和关闭回调。从 CPU 启动时依次经过 `CPUHP_BP_KICK_AP` → `CPUHP_AP_ONLINE` → `CPUHP_ONLINE`，每个阶段都可以回滚