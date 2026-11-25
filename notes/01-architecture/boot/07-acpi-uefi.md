# ACPI 与 UEFI 启动

## 1. 概述

ARM64 有两条固件描述路径：设备树（DT）和 ACPI，二者互斥。`setup_arch()`
解析完早期 FDT 后调用 `acpi_boot_table_init()`，成功则走 ACPI，否则
`unflatten_device_tree()` 走 DT。UEFI 是“引导方式”，与 ACPI/DT 正交：
EFI stub 在 ExitBootServices 前完成内存分配、initrd 加载和参数传递。

当前 `.config`：

| 配置 | 值 | 说明 |
| --- | --- | --- |
| `CONFIG_ARCH_SUPPORTS_ACPI` | y | 架构支持 |
| `CONFIG_ACPI` | **n** | ACPI 解释器、表解析、设备枚举均不编译 |
| `CONFIG_EFI` | y | UEFI 运行时支持 |
| `CONFIG_EFI_STUB` / `EFI_GENERIC_STUB` | y | PE/COFF 通用 stub |
| `CONFIG_EFI_PARAMS_FROM_FDT` | y | 从 `/chosen` 读 UEFI 系统表与内存图 |
| `CONFIG_EFI_RUNTIME_WRAPPERS` | y | 运行时调用封装 |
| `CONFIG_EFI_EARLYCON` / `EFIVAR_FS` | y | EFI earlycon、efivarfs |
| `CONFIG_EFI_ARMSTUB_DTB_LOADER` | y | stub 可从命令行加载 DTB |

`CONFIG_ACPI=n` 的影响：

- `arch/arm64/kernel/Makefile` 的 `obj-$(CONFIG_ACPI) += acpi.o` 不成立，
  `arch/arm64/kernel/acpi.c` **不编译**；
- `drivers/acpi/` 的 `tables.o`、`acpi.o`、`acpica/` 均不构建；
- `include/linux/acpi.h` 提供 `#define acpi_disabled 1`、
  `acpi_boot_table_init()` / `acpi_table_upgrade()` 空桩，调用点代码形态
  不变但分支恒不成立。

因此 ACPI 是“源码存在、当前配置未启用”；EFI 是“已编译，但只有 UEFI
引导时才真正生效”。

## 2. ARM64 的 ACPI 启动路径

### 2.1 开关与选择

`arch/arm64/kernel/acpi.c`：

```c
int acpi_noirq = 1;		/* skip ACPI IRQ initialization */
int acpi_disabled = 1;
int acpi_pci_disabled = 1;	/* skip ACPI PCI scan and IRQ init */
```

`early_param("acpi", parse_acpi)` 支持 `off`（强制 DT）、`on`（优先 ACPI）、
`force`（表失败也启用）、`nospcr`（不用 SPCR 串口）。选择逻辑在
`acpi_boot_table_init()`（198 行）：

```c
if (param_acpi_off ||
    (!param_acpi_on && !param_acpi_force && !dt_is_stub()))
	goto done;

enable_acpi();

if (acpi_table_init() || acpi_fadt_sanity_check()) {
	pr_err("Failed to init ACPI tables\n");
	if (!param_acpi_force)
		disable_acpi();
}
```

`dt_is_stub()`（72 行）判断 DTB 是否只是占位：若根节点除 `/chosen` 和 Xen
`/hypervisor` 外还有节点，就认为真实 DT 存在，默认不启用 ACPI；空 DTB
才允许 ACPI。这样同一镜像可同时支持 ACPI 服务器和 DT 平台。

`done:` 之后：ACPI 关闭时若 `earlycon_acpi_spcr_enable` 则回退
`early_init_dt_scan_chosen_stdout()`；ACPI 启用时读 FACS 的
`hardware_signature` 供休眠、`acpi_parse_spcr()` 配置串口、BGRT 解析开机 logo。

### 2.2 FADT 检查与 PSCI 标志

`acpi_fadt_sanity_check()`（127 行）：

1. `acpi_get_table(ACPI_SIG_FADT, 0, &table)` 取 FACP，失败返回 `-ENODEV`；
2. 要求 FADT ≥ 5.1（ARM GIC/SMP 启动信息自 5.1 引入）；
3. 必须置位 `ACPI_FADT_HW_REDUCED`，ARM64 不支持传统 ACPI 硬件模型；
4. `acpi_put_table()` 释放映射。

`acpi_psci_present()` / `acpi_psci_use_hvc()` 读 FADT 的 `arm_boot_flags`：
`ACPI_FADT_PSCI_COMPLIANT` 表示用 PSCI，`ACPI_FADT_PSCI_USE_HVC` 表示
用 HVC 而非 SMC 作为 conduit。

### 2.3 表的发现与解析

```
acpi_table_init()                                   [drivers/acpi/tables.c:757]
├─ acpi_locate_initial_tables()
│  ├─ 按 acpi_verify_table_checksum 设置表校验开关
│  └─ acpi_initialize_tables(initial_tables, ACPI_MAX_TABLES, 0)
│     └─ ACPICA：RSDP → RSDT/XSDT → 逐表 checksum
└─ acpi_table_init_complete()
   ├─ acpi_table_initrd_scan()                       initrd 表覆盖
   └─ check_multiple_madt()
```

`acpi_reserve_initial_tables()` 对每张表的物理区间 `memblock_reserve()`。
表头（`include/acpi/actbl.h:68`）含 `signature[4]`、`length`、`revision`、
`checksum`、`oem_id[6]`、`oem_table_id[8]`、`oem_revision`、
`asl_compiler_id[4]`、`asl_compiler_revision`。

ARM64 常用表：

| 签名 | 宏 | 用途 |
| --- | --- | --- |
| `FACP` | `ACPI_SIG_FADT` | 固定硬件、ARM boot flags、HW_REDUCED |
| `APIC` | `ACPI_SIG_MADT` | GIC CPU 接口与中断源 |
| `GTDT` | `ACPI_SIG_GTDT` | 系统计数器、EL1/EL2 定时器中断 |
| `PPTT` | `ACPI_SIG_PPTT` | CPU 拓扑 |
| `MCFG` | `ACPI_SIG_MCFG` | PCIe ECAM |
| `SPCR` | `ACPI_SIG_SPCR` | 串口控制台 |
| `DSDT` | `ACPI_SIG_DSDT` | AML 命名空间根 |

MADT 的 GICC 条目（`include/acpi/actbl2.h:1431`）含
`cpu_interface_number`、`uid`、`flags`（`ACPI_MADT_ENABLED`）、`arm_mpidr`、
`gicr_base_address`；GTDT（`actbl1.h:1586`）含
`non_secure_el1_interrupt`、`virtual_timer_interrupt`、
`platform_timer_count/offset`。

`__acpi_map_table()` 在 `paging_init()` 前调用，只能用 `early_memremap()`；
`acpi_os_ioremap()` 依据 EFI 内存图选择 `PAGE_KERNEL`/`PAGE_KERNEL_RO`/
`PROT_NORMAL_NC`/`PROT_DEVICE_nGnRnE`，并拒绝把内核内存映射为可写。

### 2.4 设备枚举

```
acpi_init()                                       subsys_initcall [bus.c:1578]
├─ acpi_bus_init()                                [bus.c:1472]
│  ├─ acpi_os_initialize1(); acpi_load_tables()
│  ├─ acpi_ec_ecdt_probe()
│  ├─ acpi_enable_subsystem(ACPI_NO_ACPI_ENABLE)
│  ├─ acpi_initialize_objects(ACPI_FULL_INITIALIZATION)
│  ├─ _OSC 协商、table handler、acpi_sysfs_init()
│  ├─ acpi_ec_dsdt_probe(); acpi_sleep_init()
│  ├─ acpi_bus_init_irq()                         取中断模型并评估 \_PIC
│  ├─ acpi_install_notify_handler(ACPI_ROOT_OBJECT, ...)
│  └─ bus_register(&acpi_bus_type)
├─ acpi_init_ffh(); pci_mmcfg_late_init(); acpi_viot_early_init()
├─ acpi_hest_init(); acpi_ghes_init()
├─ acpi_arch_init()                               ARM64 专有
└─ acpi_scan_init()                               [scan.c:2821]
   ├─ 注册 pci root/link、processor、platform、lpss、apd、
   │  cmos_rtc、container、memory_hotplug、watchdog、pnp、
   │  power_resources、lpit 等 handler
   ├─ acpi_scan_add_handler(&generic_device_handler)
   ├─ STAO 处理（可能屏蔽 SPCR UART）
   ├─ acpi_gpe_apply_masked_gpes(); acpi_update_all_gpes()
   └─ acpi_bus_scan(ACPI_ROOT_OBJECT)
      ├─ acpi_bus_check_add()
      ├─ acpi_add_single_object()                 分配 acpi_device
      ├─ acpi_default_enumeration()               非专用设备 → platform_device
      └─ acpi_bus_attach()                        递归 _STA 可用子设备
```

`acpi_add_single_object()`（1861 行）做 `acpi_init_device_object()`、处理
`_DEP` 依赖、读取 `_STA`；`acpi_default_enumeration()`（2247 行）为没有
专用扫描器的设备创建 platform device 或登记 system device 以保留资源。

ARM64 专有（`drivers/acpi/arm64/init.c`）：

```c
void __init acpi_arch_init(void)
{
	if (IS_ENABLED(CONFIG_ACPI_AGDI)) acpi_agdi_init();
	if (IS_ENABLED(CONFIG_ACPI_APMT)) acpi_apmt_init();
	if (IS_ENABLED(CONFIG_ACPI_IORT)) acpi_iort_init();
	if (IS_ENABLED(CONFIG_ARM_AMBA))  acpi_amba_init();
}
```

IORT 负责 I/O 拓扑与 MSI 映射，APMT 描述性能监控，AGDI 是活动监视。

### 2.5 CPU 枚举与 PSCI

```
smp_init_cpus()                                  [arch/arm64/kernel/smp.c]
├─ acpi_disabled ? of_parse_and_init_cpus()
│                : acpi_parse_and_init_cpus()
└─ acpi_parse_and_init_cpus()
   └─ acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_INTERRUPT,
                            acpi_parse_gic_cpu_interface, 0)
      └─ acpi_map_gic_cpu_interface(processor)    [smp.c:571]
         ├─ flags & ACPI_MADT_ENABLED
         ├─ arm_mpidr → set_cpu_possible()/__cpu_logical_map[]
         └─ 记录 parking/performance 中断
```

`setup_arch()` 中 PSCI 同样二选一：

```c
if (acpi_disabled)
	psci_dt_init();
else
	psci_acpi_init();
```

## 3. EFI stub

PE/COFF 入口由 `arch/arm64/kernel/efi-header.S` 指向
`__efistub_efi_pe_entry`，实现是通用 stub 的
`drivers/firmware/efi/libstub/efi-stub-entry.c:35`：

```
efi_pe_entry(handle, systab)
├─ 校验 systab signature，取 LOADED_IMAGE_PROTOCOL
├─ efi_handle_cmdline() → DT bootargs
├─ handle_kernel_image()：校验 image_base/SEGMENT_ALIGN，
│  efi_kaslr_relocate_kernel(..., efi_kaslr_get_phys_seed())
├─ efi_stub_common() → efi_boot_kernel() → ExitBootServices
└─ efi_enter_kernel() → primary_entry
```

`efi_stub_common()`（`efi-stub.c:153`）依次执行：
`check_platform_features()`、`setup_primary_display()`、
`efi_retrieve_eventlog()`、`efi_enable_reset_attack_mitigation()`、
`efi_load_initrd()`、`efi_random_get_seed()`；不支持 `SetVirtualAddressMap`
时强制 `efi_novamap`；再 `install_memreserve_table()`，最后
`efi_boot_kernel()`（`libstub/fdt.c:344`）→
`allocate_new_fdt_and_exit_boot()` → `efi_enter_kernel(kernel_addr, fdt_addr, size)`。

`allocate_new_fdt_and_exit_boot()` 写入 `linux,uefi-system-table`、
`linux,uefi-mmap-start/size/desc-size/desc-ver`、initrd 与命令行，然后
`ExitBootServices()`；`efi_enter_kernel()` 跳到 `primary_entry`
（`primary_entry_offset()` = `primary_entry - _text`）。

## 4. EFI 运行时与内存映射

### 4.1 efi_init

`drivers/firmware/efi/efi-init.c:233`：

```
efi_init()
├─ efi_get_fdt_params(&data)                     [fdtparams.c:84]
│  └─ /chosen 的 linux,uefi-system-table / mmap-start / mmap-size
│     / mmap-desc-size / mmap-desc-ver；Xen 用 /hypervisor/uefi
├─ efi_memmap_init_early(&data)                  early_memremap 映射内存图
├─ WARN(desc_version != 1)
├─ uefi_init(efi_system_table)                   systab/runtime/config tables
├─ reserve_regions()                             用 EFI 内存图重建 memblock
├─ early_init_dt_check_for_usable_mem_range()
├─ efi_find_mirror(); efi_esrt_init(); efi_mokvar_table_init()
├─ memblock_reserve(内存图自身)
└─ init_primary_display()
```

`reserve_regions()` 先丢弃 DT 解析出的 memblock（KHO 场景除外），再遍历
`for_each_efi_memory_desc()`：可行内存 `early_init_dt_add_memory_arch()`；
非 usable 或 `EFI_MEMORY_SP` → `memblock_mark_nomap()` 或跳过；
`EFI_ACPI_RECLAIM_MEMORY` → 额外 `memblock_reserve()`。

### 4.2 内存描述符

`efi_memory_desc_t` 含 `type`、`phys_addr`、`virt_addr`、`num_pages`、
`attribute`（WB/WT/WC/UC/RUNTIME/SP 等）。

`efi_memmap_init_early()`（`memmap.c:75`）在 vmalloc 未就绪时用
`early_memremap()`；`efi_memmap_init_late()`（125 行）改用
`memremap(..., MEMREMAP_WB)` 并置 `EFI_MEMMAP_LATE`。早期映射需在
`efi_init()` 后 `efi_memmap_unmap()`，否则长期占用 fixmap。

常用查询：`efi_mem_attributes(phys)`、`efi_mem_desc_lookup()`、
`for_each_efi_memory_desc(md)`。ARM64 的 `efi_create_mapping()`
（`arch/arm64/kernel/efi.c:86`）按描述符类型建立运行时映射，
`efi_set_mapping_permissions()`（130 行）把代码段设为只读/可执行。

### 4.3 运行时服务

`struct efi`（`include/linux/efi.h:637`）保存 `runtime` 表、版本、
`acpi`/`smbios`/`esrt` 等配置表地址，以及
`get_time/set_time/get_variable/set_variable/reset_system` 等函数指针。

- `EFI_RUNTIME_WRAPPERS=y`：调用经 `arch_efi_call_virt_setup()/teardown()`
  （`arch/arm64/kernel/efi.c:169`），保存/恢复 x18 并处理固件破坏 x18
  （`efi_handle_corrupted_x18()`）；
- `SetVirtualAddressMap()` 由 stub 在 ExitBootServices 后调用，只对
  `EFI_MEMORY_RUNTIME` 区域建虚拟映射（`efi_get_virtmap()`，`efi-stub.c:226`）；
- `efi_novamap` 为真时保持物理地址调用；
- `arm64_efi_rt_init()`（`efi.c:245`，`core_initcall`）安装运行时异常修复。

## 5. ACPI 与 DT 的取舍

| 维度 | DT | ACPI |
| --- | --- | --- |
| 描述来源 | bootloader 传 x0 DTB | UEFI 提供 RSDP/XSDT |
| 选择开关 | `acpi_disabled=1` | `acpi_boot_table_init()` 成功 |
| 设备枚举 | `unflatten_device_tree()` + `of_platform_populate()` | `acpi_bus_scan()` + scan handlers |
| CPU 枚举 | `/cpus/cpu@N` 的 `reg`/`enable-method` | MADT GICC 的 `arm_mpidr`/`flags` |
| PSCI | `psci_dt_init()` | `psci_acpi_init()` |
| 中断 | `of_irq_parse_one()` + irq domain | MADT/GTDT + IORT/GSI |
| 早期控制台 | `/chosen/stdout-path` | SPCR |

选择一旦确定不再切换，`acpi_disabled` 是全局开关，驱动与架构代码大量依赖。

## 6. 当前配置下的实际行为

1. `CONFIG_ACPI=n`：`acpi.o`、`drivers/acpi/*` 不编译；`acpi_disabled` 恒 1；
   `setup_arch()` 必然 `unflatten_device_tree()`、`psci_dt_init()`。
2. `CONFIG_EFI=y`：EFI 代码编译；但若直接 `-kernel` 启动（不经过 UEFI），
   `efi_get_fdt_params()` 在 `/chosen` 找不到 `linux,uefi-system-table`
   即返回 0，`efi_init()` 直接返回，`efi_enabled(EFI_BOOT)` 为假，
   运行时服务不可用。
3. `EFI_STUB=y` 带 PE/COFF 头；`EFI_ZBOOT=n` 不做压缩内核自解压；
   `EFI_CAPSULE_LOADER=n`、`EFI_TEST=n` 无 capsule 更新与测试接口。

调试：`dmesg | grep -i efi`、检查 `/sys/firmware/efi/` 与
`/sys/firmware/efi/memmap`；当前配置下 `acpi=...` 参数无效（未编译）。

## 7. 关键调用链

```
start_kernel → setup_arch
├─ setup_machine_fdt(__fdt_pointer)
├─ efi_init()                                   [efi-init.c:233]
│  ├─ efi_get_fdt_params()
│  ├─ efi_memmap_init_early()
│  ├─ uefi_init()
│  └─ reserve_regions()
├─ acpi_boot_table_init()                       ACPI=n 时为空桩
├─ if (acpi_disabled) unflatten_device_tree()
├─ if (acpi_disabled) psci_dt_init(); else psci_acpi_init()
└─ smp_init_cpus()
   └─ acpi_disabled ? of_parse_and_init_cpus()
                    : acpi_parse_and_init_cpus()

UEFI 引导时：
efi_pe_entry → handle_kernel_image → efi_stub_common
             → efi_boot_kernel → ExitBootServices
             → efi_enter_kernel → primary_entry
```

## 8. 关键文件清单

| 文件 | 作用 |
| --- | --- |
| `arch/arm64/kernel/acpi.c` | `acpi_boot_table_init()`、`parse_acpi()`、FADT 检查、PSCI 标志（当前未编译） |
| `arch/arm64/kernel/efi.c` | ARM64 EFI 映射、运行时封装、x18 修复 |
| `arch/arm64/kernel/setup.c` / `smp.c` | `efi_init()`、ACPI/DT 分支、PSCI 初始化、MADT CPU 枚举 |
| `drivers/acpi/tables.c` | `acpi_table_init()`、RSDP/XSDT 定位、表预留 |
| `drivers/acpi/bus.c` | `acpi_init()`、`acpi_bus_init()` |
| `drivers/acpi/scan.c` | `acpi_scan_init()`、`acpi_bus_scan()`、设备对象与默认枚举 |
| `drivers/acpi/arm64/init.c` | `acpi_arch_init()`：IORT/APMT/AGDI/AMBA |
| `drivers/firmware/efi/efi-init.c` | `efi_init()`、`reserve_regions()` |
| `drivers/firmware/efi/fdtparams.c` | `efi_get_fdt_params()` |
| `drivers/firmware/efi/libstub/efi-stub-entry.c` / `efi-stub.c` | `efi_pe_entry()`、`efi_stub_common()`、virtmap |
| `drivers/firmware/efi/libstub/arm64-stub.c` | `handle_kernel_image()` |
| `drivers/firmware/efi/libstub/fdt.c` | `efi_boot_kernel()`、ExitBootServices |
| `include/acpi/actbl*.h` / `include/linux/efi.h` | ACPI 表结构、`struct efi`、`efi_memory_desc_t` |
