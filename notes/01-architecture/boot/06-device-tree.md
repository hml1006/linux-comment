# Device Tree 启动路径

## 1. 概述

ARM64 引导协议约定：bootloader 通过 **x0 传递 DTB（Flattened Device Tree）
物理地址**，x1~x3 保留。`setup_arch()` 调用 `setup_machine_fdt(__fdt_pointer)`
完成早期解析；ACPI 未启用时，`unflatten_device_tree()` 把 FDT 展开成
`struct device_node` 树，最后由 `of_platform_populate()` 转换成
`platform_device`。

当前 `.config`：

| 配置 | 值 | 说明 |
| --- | --- | --- |
| `CONFIG_OF` / `OF_FLATTREE` | y | 设备树与 FDT 展开 |
| `CONFIG_OF_EARLY_FLATTREE` | y | 早期扁平树扫描 |
| `CONFIG_OF_ADDRESS` / `OF_IRQ` | y | reg/ranges 转换、中断映射 |
| `CONFIG_OF_KOBJ` / `OF_RESERVED_MEM` | y | sysfs 导出、reserved-memory |
| `CONFIG_OF_OVERLAY` | n | overlay/resolver/dynamic 不编译 |
| `CONFIG_ACPI` | n | `acpi_disabled` 恒为 1，走 DT 路径 |

`CONFIG_ACPI=n` 时 `include/linux/acpi.h` 提供 `#define acpi_disabled 1`、
`static inline void acpi_boot_table_init(void) {}`、
`static inline void acpi_table_upgrade(void) {}` 等桩，`setup_arch()` 的
调用形态不变但不执行 ACPI。

## 2. DTB 物理布局

FDT 头部（`scripts/dtc/libfdt/fdt.h`）：

```c
struct fdt_header {
	fdt32_t magic;		 /* 0xd00dfeed */
	fdt32_t totalsize;
	fdt32_t off_dt_struct;	 /* 结构块偏移 */
	fdt32_t off_dt_strings;	 /* 字符串块偏移 */
	fdt32_t off_mem_rsvmap;	 /* 保留内存表偏移 */
	fdt32_t version;
	fdt32_t last_comp_version;
	fdt32_t boot_cpuid_phys;
	fdt32_t size_dt_strings;
	fdt32_t size_dt_struct;
};
```

结构块由 `FDT_BEGIN_NODE`、`FDT_END_NODE`、`FDT_PROP`、`FDT_NOP`、
`FDT_END` 等 token 组成；`fdt_nop_property()` 用 `FDT_NOP` 擦除敏感属性。

ARM64 约束（`arch/arm64/include/asm/boot.h`）：`MIN_FDT_ALIGN = 8`、
`MAX_FDT_SIZE = SZ_2M`。不满足时 `setup_machine_fdt()` 打印错误并原地
`cpu_relax()`——此时异常处理和控制台都没就绪，连 `BUG()` 都不能用。

## 3. 早期 FDT 解析

### 3.1 fixmap 映射

`arch/arm64/mm/fixmap.c:138`：

```c
void *__init fixmap_remap_fdt(phys_addr_t dt_phys, int *size, pgprot_t prot)
{
	const u64 dt_virt_base = __fix_to_virt(FIX_FDT);
	...
	if (!dt_phys || dt_phys % MIN_FDT_ALIGN)
		return NULL;
	dt_phys_base = round_down(dt_phys, PAGE_SIZE);
	offset = dt_phys % PAGE_SIZE;
	dt_virt = (void *)dt_virt_base + offset;

	create_mapping_noalloc(dt_phys_base, dt_virt_base, PAGE_SIZE, prot);
	if (fdt_magic(dt_virt) != FDT_MAGIC)
		return NULL;
	*size = fdt_totalsize(dt_virt);
	if (*size > MAX_FDT_SIZE)
		return NULL;
	if (offset + *size > PAGE_SIZE)
		create_mapping_noalloc(dt_phys_base, dt_virt_base,
				       offset + *size, prot);
	return dt_virt;
}
```

要点：

1. 映射到固定虚拟地址 `FIX_FDT`，保留页内偏移，避免 libfdt 指针运算跨页；
2. 先映射一页读头拿 `totalsize`，再按需映射整块；
3. 返回 fixmap 地址，**不能**用 `__pa()`，所以 `setup_machine_fdt()` 单独
   把 `dt_phys` 传给 `early_init_dt_scan()`；
4. 扫描完成后再次映射为 `PAGE_KERNEL_RO`，防止后续篡改。

### 3.2 setup_machine_fdt

```
setup_machine_fdt(dt_phys)                           [arch/arm64/kernel/setup.c:172]
├─ fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL)
├─ memblock_reserve(dt_phys, size)
├─ early_init_dt_scan(dt_virt, dt_phys)              [drivers/of/fdt.c:1246]
│  ├─ early_init_dt_verify()
│  │  ├─ fdt_check_header()
│  │  ├─ initial_boot_params = dt_virt
│  │  ├─ initial_boot_params_pa = dt_phys
│  │  ├─ of_fdt_crc32 = crc32_be(...)
│  │  └─ early_init_dt_scan_root()          读根 #address-cells/#size-cells
│  └─ early_init_dt_scan_nodes()
│     ├─ early_init_dt_scan_chosen()
│     │  ├─ initrd / elfcorehdr / dmcryptkeys
│     │  ├─ rng-seed → add_bootloader_randomness() 后擦除并重算 crc32
│     │  └─ bootargs → boot_command_line
│     ├─ early_init_dt_scan_memory()
│     │  └─ device_type=="memory" → early_init_dt_add_memory_arch()
│     ├─ early_init_dt_check_for_usable_mem_range()
│     └─ early_init_dt_check_kho()
├─ fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO)
└─ of_flat_dt_get_machine_name() → "Machine model: ..."
```

`early_init_dt_scan_root()` 把 `#address-cells` / `#size-cells` 存到
`dt_root_addr_cells` / `dt_root_size_cells`（缺省 2/1），缺失会 `WARN`，
影响后续所有 `reg` 解析。

`early_init_dt_scan_memory()` 只认 `device_type = "memory"`，优先
`linux,usable-memory`，否则 `reg`；逐项加入 memblock，带 `hotpluggable`
的区域额外 `memblock_mark_hotplug()`。

`/chosen/rng-seed` 是“消费后擦除”的典型：

```c
rng_seed = of_get_flat_dt_prop(node, "rng-seed", &l);
if (rng_seed && l > 0) {
	add_bootloader_randomness(rng_seed, l);
	fdt_nop_property(initial_boot_params, node, "rng-seed");
	of_fdt_crc32 = crc32_be(~0, initial_boot_params,
				fdt_totalsize(initial_boot_params));
}
```

## 4. unflatten_device_tree：FDT → device_node

`arch/arm64/kernel/setup.c:358`：

```c
acpi_boot_table_init();
if (acpi_disabled)
	unflatten_device_tree();
```

当前配置必然展开。`drivers/of/fdt.c:1282`：

```
unflatten_device_tree()
├─ fdt_scan_reserved_mem_late()
├─ initial_boot_params == NULL → 用 __dtb_empty_root_begin 空根节点
│  并 copy_device_tree()
├─ __unflatten_device_tree(fdt, NULL, &of_root,
│                          early_init_dt_alloc_memory_arch, false)
│  ├─ fdt_check_header()
│  ├─ 第一遍 unflatten_dt_nodes(blob, NULL, ...)     dryrun，只算内存
│  ├─ dt_alloc(size + 4, __alignof__(struct device_node))
│  ├─ *(mem + size) = cpu_to_be32(0xdeadbeef)         越界哨兵
│  ├─ 第二遍 unflatten_dt_nodes(blob, mem, dad, mynodes)
│  │  ├─ populate_node()
│  │  │  ├─ unflatten_dt_alloc(sizeof(*np) + strlen(name) + 1)
│  │  │  ├─ of_node_init(np)，full_name 紧随结构体
│  │  │  └─ populate_properties()，phandle 写入 np->phandle
│  │  └─ reverse_nodes(root)                          反转 child 链表
│  └─ 检查 0xdeadbeef 是否被覆盖
└─ of_alias_scan()                                     解析 /aliases
```

关键点：

- **两遍扫描**：`dryrun` 先算大小，第二遍写入；整棵树一次分配，节点、
  属性名、属性值同处一块内存；
- 深度上限 `FDT_MAX_DEPTH = 64`，超过 `WARN_ON_ONCE` 并跳过；
- 展开时 child 链表头插，最后 `reverse_nodes()` 递归反转，恢复 dts 顺序；
- `of_root`/`of_chosen`/`of_aliases` 是 `drivers/of/base.c` 的全局指针；
  bootloader 不给 DTB 时退化为内建空根节点。

### device_node

`struct device_node`（`include/linux/of.h:48`）字段：`name`、`phandle`、
`full_name`、`fwnode`、`properties`/`deadprops`、`parent`/`child`/`sibling`、
`kobj`（`CONFIG_OF_KOBJ`）、`_flags`、`data`。

常用标志（`include/linux/of.h:150`）：`OF_DYNAMIC=1`、`OF_POPULATED=3`、
`OF_POPULATED_BUS=4`、`OF_OVERLAY=5`。`OF_POPULATED(_BUS)` 是幂等性的
核心，保证同一节点不会重复实例化。

## 5. of_match 与属性解析

### 5.1 of_match

`of_match_node()`（`drivers/of/base.c:1176`）对 `of_device_id` 表逐项打分取
最高。`of_device_id` 含 `name[32]`、`type[32]`、`compatible[128]`、
`data`。

`__of_device_is_compatible()`（346 行）：

1. `compatible` 命中：`score = INT_MAX/2 - (index << 2)`，越靠前分越高；
2. 再匹配 `type` 加 2 分；
3. 再匹配 `name` 加 1 分。

因此是“最具体 compatible 优先”，而非返回第一个命中项。驱动通常用
`of_device_get_match_data()` 取 `.data`。

### 5.2 status

`of_device_is_available()`（584 行）在 `devtree_lock` 下判断：

- `status` 缺失 → available；
- `"okay"` / `"ok"` → available；
- 其他（`"disabled"`、`"fail"`、`"fail-sss"`、`"reserved"`）→ 不可用。

`__of_device_is_status()` 支持以 `-` 结尾的前缀匹配，故 `"fail-"` 匹配
所有 `fail-xxx`。

### 5.3 reg 与 ranges

设备树地址是总线局部地址，须逐级通过父节点 `ranges` 转 CPU 物理地址：

```
of_address_to_resource(dev, index, r)                 [drivers/of/address.c:1101]
└─ __of_address_to_resource()
   ├─ __of_get_address()                              按 #address-cells/#size-cells 解析 reg
   │  └─ of_match_bus()                               选择 default/pci/isa
   ├─ of_translate_address() → __of_translate_address()
   │  └─ 逐级向上 of_translate_one()
   │     ├─ ranges 缺失 → 不可翻译
   │     ├─ ranges 为空 → 1:1 直通
   │     └─ 遍历 <child addr, parent addr, size> 三元组
   ├─ of_mmio_is_nonposted() → IORESOURCE_MEM_NONPOSTED
   ├─ r->flags、r->name（reg-names[index] 或 full_name）
   └─ __of_address_resource_bounds()
```

`ranges` 的 cell 数由当前/父节点 `#address-cells` 与 `#size-cells` 决定。
`ranges` 缺失时 `of_translate_one()` 判失败，只有 PowerPC Apple 机器把
“缺失”当“空 ranges（1:1）”。

### 5.4 interrupts

```
of_irq_parse_one(device, index, out_irq)             [drivers/of/irq.c:428]
├─ 优先 interrupts-extended（含 phandle）
├─ 否则 of_irq_find_parent() 沿 interrupt-parent 向上
├─ 读父节点 #interrupt-cells 作为 args_count
├─ 取 device 的 interrupts 第 index 组 specifier
└─ of_irq_parse_raw() 处理 interrupt-map 递归翻译

of_irq_get() → irq_create_of_mapping()
└─ irq_create_fwspec_mapping()                       [kernel/irq/irqdomain.c:913]
   ├─ fwspec_to_domain()
   ├─ irq_domain_translate()
   ├─ irq_find_mapping() 命中则复用
   └─ 层次 domain → irq_domain_alloc_irqs_locked()
      非层次 → irq_create_mapping_affinity_locked()
```

GICv3（`drivers/irqchip/irq-gic-v3.c:1589`）的 `#interrupt-cells = 3`：
type（0=SPI，1=PPI，2=ESPI，3=EPPI，4=LPI）、号、触发标志；SPI 号 +32、
PPI 号 +16 得 hwirq。`gic_irq_domain_map()` 再按 hwirq 区间选
`handle_fasteoi_irq`（SPI/LPI）或 `handle_percpu_devid_irq`（SGI/PPI）。

## 6. device_node → platform_device

### 6.1 of_device_alloc

`drivers/of/platform.c:97`：`platform_device_alloc("", PLATFORM_DEVID_NONE)`
后按 `of_address_count()` 分配资源数组，逐个 `of_address_to_resource()`；
再 `device_set_node()` 绑定 fwnode，`bus_id` 为空时
`of_device_make_bus_id()` 生成唯一名字。**中断不在此时变成资源**，由驱动
`platform_get_irq()` → `of_irq_get()` 按需解析。

### 6.2 of_platform_populate

```
of_platform_default_populate_init()             arch_initcall_sync [platform.c:501]
├─ /reserved-memory 下匹配 reserved_mem_matches[] 的节点创建设备
├─ /firmware → of_platform_default_populate()
├─ /chosen 的 simple-framebuffer → 创建设备并 sysfb_disable()
└─ of_platform_default_populate(NULL, NULL, NULL)
   └─ of_platform_populate()                    [platform.c:443]
      └─ for_each_child_of_node(root, child)
         └─ of_platform_bus_create(child, matches, lookup, parent, strict=true)
            ├─ strict 且无 compatible → 跳过
            ├─ of_skipped_node_table 命中 → 跳过
            ├─ OF_POPULATED_BUS 已置位 → 跳过
            ├─ of_dev_lookup() → auxdata
            ├─ compatible="arm,primecell" → of_amba_device_create()
            ├─ of_platform_device_create_pdata()
            │  ├─ !of_device_is_available() || OF_POPULATED → NULL
            │  ├─ of_device_alloc()
            │  ├─ of_msi_configure()
            │  ├─ of_device_add() → device_add()
            │  └─ 失败则 of_node_clear_flag(OF_POPULATED)
            └─ bus 匹配 matches 时递归 child
```

默认 match 表只有 `simple-bus`、`simple-mfd`、`isa`、`arm,amba-bus`。
即顶层节点无论是否匹配都会创建，但只有“透明总线”的子节点递归实例化；
PCI/I2C/SPI 等子设备由各自总线驱动枚举。

`struct of_dev_auxdata`（`include/linux/of_platform.h:40`）按 compatible +
物理地址给设备指定名字与 `platform_data`，是 ARM32 板级代码迁移的过渡
机制，ARM64 新平台基本不用。

## 7. Overlay（当前配置未启用）

`drivers/of/Makefile`：

```make
obj-$(CONFIG_OF_DYNAMIC) += dynamic.o
obj-$(CONFIG_OF_RESOLVE) += resolver.o
obj-$(CONFIG_OF_OVERLAY) += overlay.o
```

当前 `CONFIG_OF_OVERLAY=n`（它会 `select OF_DYNAMIC`、`OF_RESOLVE`），
因此 `overlay.o`、`resolver.o`、`dynamic.o` 都**不编译**，
`of_overlay_fdt_apply()` 不存在，运行期给内核打 DT overlay 不可用。

源码流程：复制 FDT → `of_fdt_unflatten_tree()` → `of_resolve_phandles()`
（调整 phandle、修正 local fixups）→ `build_changeset()` →
`of_overlay_apply()`（PRE_APPLY 通知、`of_changeset_apply()`、POST_APPLY
通知）→ 记录 `ovcs_id` 供 `of_overlay_remove()` 回滚。

`dynamic.c` 提供 `of_attach_node()`/`of_detach_node()`/`of_reconfig_notify()`；
没有 `CONFIG_OF_DYNAMIC` 时节点树视为只读。

## 8. 当前配置与调试入口

`CONFIG_ACPI=n` 时必然 `unflatten_device_tree()`；OF_FLATTREE/EARLY_FLATTREE/
ADDRESS/IRQ 齐全，DTB 解析、地址转换与中断映射可用；OF_KOBJ 导出
`/sys/firmware/devicetree/base/`；OF_OVERLAY=n 无 overlay；OF_RESERVED_MEM
解析 `/reserved-memory`。调试看 `/proc/device-tree/`、`/sys/firmware/fdt`
（可 `dtc -I dtb -O dts` 反编译）和 `drivers/of/*` 动态调试。

## 9. 关键文件清单

| 文件 | 作用 |
| --- | --- |
| `arch/arm64/kernel/setup.c` | `setup_machine_fdt()`、`setup_arch()` 的 DT/ACPI 分支 |
| `arch/arm64/mm/fixmap.c` | `fixmap_remap_fdt()`，DTB 映射与只读化 |
| `arch/arm64/include/asm/boot.h` | `MIN_FDT_ALIGN`、`MAX_FDT_SIZE` |
| `drivers/of/fdt.c` | 早期扫描、`unflatten_device_tree()`、空根节点回退 |
| `drivers/of/base.c` | `of_match_node()`、`of_device_is_available()`、节点查找 |
| `drivers/of/address.c` | `of_address_to_resource()`、`of_translate_address()`、ranges |
| `drivers/of/irq.c` | `of_irq_parse_one()`、`of_irq_get()`、interrupt-map |
| `drivers/of/platform.c` | `of_device_alloc()`、`of_platform_populate()` |
| `include/linux/of.h` | `device_node`、`property`、`of_device_id`、OF_* 标志 |
| `include/linux/of_fdt.h` / `of_platform.h` / `libfdt.h` | 早期扫描、auxdata、libfdt 接口 |
