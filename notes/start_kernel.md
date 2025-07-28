# start_kernel流程

## start kernel流程概览

由head.S跳转到start_kernel函数开始执行内核初始化流程：

```mermaid
flowchart LR
sk[start_kernel]
sk--> set_task_stack_end_magic[set_task_stack_end_magic<br/>在init_task栈顶设置一个magic,用于检测溢出]
sk--> ssp[smp_setup_processor_id<读取并保存core 0的cpuid>]
ssp--> read_cpuid_mpidr[read_cpuid_mpidr<从mpidr寄存器读取cpuid>]
ssp--> set_cpu_logical_map["set_cpu_logical_map<设置__cpu_logical_map[0]=mpidr>"]
sk--> doei[debug_objects_early_init<初始化obj_hash,obj_static_pool,调试用到>]
doei--> raw_spin_lock_init[raw_spin_lock_init<初始化obj_hash表每个bucket spinlock>]
doei--> hlist_add_head["hlist_add_head<obj_static_pool[i].node加入到obj_pool>"]
sk--> init_vmlinux_build_id[init_vmlinux_build_id<从.note section查找build id>]
sk--> cie[cgroup_init_early<cgroups基本初始化>]
cie--> init_cgroup_root[init_cgroup_root<初始化cgroup树root>]
cie--> cgroup_init_subsys[cgroup_init_subsys<初始化subsystem>]
sk--> local_irq_disable[local_irq_disable<关中断>]
sk--> boot_cpu_init[boot_cpu_init<把boot CPU添加到online,present,active,possible map>]
sk--> page_address_init[page_address_init<high memory, 64位cpu为空>]
sk--> early_security_init[early_security_init<初期安全模块初始化>]
sk--> sa[setup_arch<br/>体系结构相关初始化]

```

## setup_arch 流程

```mermaid
flowchart LR
sa[setup_arch<br/>体系结构相关初始化]
sa--> setup_initial_init_mm[setup_initial_init_mm<初始化init 内存管理器>]
sa--> kaslr_init[kaslr_init<br/>Kernel Address Space Layout Random 内核地址随机化初始化]
sa--> fixmap[early_fixmap_init<br/>初始化L0, L1, L2 fixmap区域对应的页表entry]
sa--> early_ioremap_init[early_ioremap_init<br/>初始化 7 个虚地址slot<br/>每个 slot 指向一段 fixmap区域]
early_ioremap_init--> early_ioremap_setup[early_ioremap_setup<br/>循环初始化slot]
sa--> smf[setup_machine_fdt<br/>映射fdt地址]
sa--> jli[jump_label_init<初始化jump table,替换static key指令>]
sa--> parse_early_param[parse_early_param<解析早期启动参数,比如grub传递的quiet>]
parse_early_param--> parse_early_options[parse_early_options]
parse_early_options--> parse_args
sa--> dynamic_scs_init[dynamic_scs_init<Shadow Call Stack, 影子调用栈,栈保护功能,把FP和LR放影子调用栈,防止缓冲区溢出攻击等,需要编译器支持>]
sa--> local_daif_restore[local_daif_restore<mask irq,fiq>]
sa--> cui[cpu_uninstall_idmap<取消idmap ttbr0映射,避免旁路攻击>]
sa--> xen_early_init[xen_early_init<裸机虚拟化>]
sa--> efi_init[efi_init<efi初始化,主要是根据efi的表构造memory map,efi数据在fdt>]
sa--> arm64_memblock_init[arm64_memblock_init<内存块初始化>]
sa--> paging_init[paging_init]
sa--> acpi_table_upgrade[acpi_table_upgrade<br/>acpi部分arm64 服务器支持]
sa--> acpi_boot_table_init[acpi_boot_table_init<br/>一般启用FDT后会disable acpi]
sa--> unflatten_device_tree[unflatten_device_tree<br/>解析设备树,把fdt转换为device_node]
sa--> bootmem_init[bootmem_init<br/>bootmem初始化,内存管理器初始化,把fdt中的memory node转换为memblock,并reserve]
sa--> kasan_init[kasan_init<br/>KASAN初始化,内存检测工具,编译器支持]
sa--> request_standard_resources[request_standard_resources<br/>请求标准资源,比如PCI,USB等]
sa--> early_ioremap_reset[early_ioremap_reset<br/>重置early ioremap的slot]
```

### early_fixmap_init 流程

```mermaid
flowchart LR
fixmap[early_fixmap_init<br/>初始化L0, L1, L2 fixmap区域对应的页表entry]
fixmap--> fixmap_pud[early_fixmap_init_pud<br/>初始化Page Upper Directory]
fixmap_pud--> __p4d_populate[__p4d_populate<br/>启用5级页表则初始化p4d,p4d在pgd和pud之间]
__p4d_populate--> set_p4d[set_p4d<br/>pgtable_l4_enabled = true即启用5级页表,才会执行]
fixmap_pud--> pud_offset_kimg[pud_offset_kimg<获取pud地址>]
pud_offset_kimg--> p4d_to_folded_pud[p4d_to_folded_pud<br/>pgtable_l4_enabled = false, 获取addr在p4d中的entry地址]
fixmap_pud--> efip[early_fixmap_init_pmd<br/>初始化pud entry的值和pmd]
efip--> __pud_populate[__pud_populate填充pud entry-><br/>set_pud设置pud entry-><br/>set_swapper_pgd非5级页表填充pgd entry<br/>5级页表直接填entry]
efip--> early_fixmap_init_pte[early_fixmap_init_pte初始化pte的上级页表-><br/>__pmd_populate填充pmd entry-><br/>set_pmd填充pmd entry]
```

### setup_machine_fdt 流程

```mermaid
flowchart LR
smf[setup_machine_fdt<br/>映射fdt地址]
smf--> frf[fixmap_remap_fdt<br/>映射到pte]
frf--> create_mapping_noalloc[create_mapping_noalloc<br/>映射第一个chunk,以便读取header信息]
create_mapping_noalloc--> __create_pgd_mapping[__create_pgd_mapping-><br/>__create_pgd_mapping_locked-><br/>alloc_init_p4d-><br/>alloc_init_pud-><br/>alloc_init_cont_pmd-><br/>init_pmd-><br/>alloc_init_cont_pte-><br/>init_pte-><br/>__set_pte_nosync填充pte]
frf--> fdt_size[从fdt header获取size]
frf--> create_mapping_noalloc_reset[create_mapping_noalloc<br/>映射剩余data]
smf--> memblock_reserve[memblock_reserve<reserve fdt物理地址>]
memblock_reserve--> memblock_add_range[memblock_add_range<br/>添加fdt range, 添加的内存如果存在重叠,需要处理合并]
memblock_add_range--> memblock_insert_region
smf--> fixmap_remap_fdt[fixmap_remap_fdt<br/>映射完成后页表设置read only<br/>防止fdt被修改]
smf--> of_flat_dt_get_machine_name[of_flat_dt_get_machine_name<从 fdt查找machine信息>]
smf--> dump_stack_set_arch_desc[dump_stack_set_arch_desc<设置machine信息到dump stack desc>]
```

### jump label初始化流程

```mermaid
flowchart LR
jli[jump_label_init<初始化jump table,替换static key指令>]
jli--> jump_label_sort_entries[jump_label_sort_entries<排序jump table的entries>]
jump_label_sort_entries--> sort[sort<堆排序>]
jli--> fjt[for循环遍历jump table]
fjt--> arch_jump_label_transform_static[arch_jump_label_transform_static<type = nop,需要重写nop指令>]
fjt--> init_section_contains[init_section_contains<检查是否在init section>]
fjt--> jump_entry_set_init[jump_entry_set_init<key设置init标志>]
fjt--> static_key_set_entries[static_key_set_entries<初始化static_key字段>]
```

### cpu uninstall idmap流程

```mermaid
flowchart LR
cui[cpu_uninstall_idmap<取消idmap ttbr0映射,避免旁路攻击>]
cui--> cpu_set_reserved_ttbr0[cpu_set_reserved_ttbr0<ttbr0设置空页>]
cui--> local_flush_tlb_all[local_flush_tlb_all<刷tlb>]
cui--> cpu_set_default_tcr_t0sz[cpu_set_default_tcr_t0sz<确保t0sz设置>]
cui--> cpu_switch_mm[cpu_switch_mm]
cpu_switch_mm--> cpu_do_switch_mm[cpu_do_switch_mm<更新ttbr0和ttbr1>]
```

### efi init 流程

```mermaid
flowchart LR
efi_init[efi_init<efi初始化,主要是根据efi的表构造memory map,efi数据在fdt>]
efi_init--> efi_get_fdt_params[efi_get_fdt_params<从fdt中取出efi信息>]
efi_get_fdt_params--> efi_get_fdt_prop[efi_get_fdt_prop]
efi_get_fdt_prop--> fdt_getprop
efi_init--> efi_memmap_init_early[efi_memmap_init_early<映射efi data>]
efi_memmap_init_early--> __efi_memmap_init[__efi_memmap_init<efi memory map>-><br/>early_memremap-><br/>early_memremap_pgprot_adjust<br/>__early_ioremap]
efi_init--> uefi_init[uefi_init]
uefi_init--> early_memremap_ro[early_memremap_ro<设置readonly 权限>-><br/>early_memremap_pgprot_adjust<br/>__early_ioremap]
uefi_init--> efi_systab_check_header[efi_systab_check_header<校验签名>]
uefi_init--> efi_systab_report_header[efi_systab_report_header<打印efi header信息>]
uefi_init--> efi_config_parse_tables[efi_config_parse_tables<解析efi table>]
efi_config_parse_tables--> early_memunmap[early_memunmap<取消fdt中efi数据映射>-><br/>early_iounmap-><br/>__late_clear_fixmap<清页表>-><br/>__set_fixmap-><br/>__pte_clear<br/>flush_tlb_kernel_range]
```

### arm64 memblock init流程

```mermaid
flowchart LR
arm64_memblock_init[arm64_memblock_init<内存块初始化,remove一些no-map区域<br/>reserve一些如kernel,fdt,ramdisk,device等内存空间>]
arm64_memblock_init--> r1[memblock_remove<从memblock中remove超过支持范围的物理地址>]
arm64_memblock_init--> r2[memblock_remove<从memblock中remove linear region外的物理地址>]
arm64_memblock_init--> r3[memblock_remove<从memblock中remove memstart之前的物理地址>]
arm64_memblock_init--> memblock_mem_limit_remove_map[memblock_mem_limit_remove_map<如果存在limit,remove limit之外的region>]
arm64_memblock_init--> memblock_add[memblock_add<如果有limit,把kernel region重新加回来>]
arm64_memblock_init--> memblock_reserve1[memblock_reserve<把kernel加到reserv>]
arm64_memblock_init--> early_init_fdt_scan_reserved_mem["early_init_fdt_scan_reserved_mem<br/>扫描fdt(device tree)中reserved memory,添加到memblock"]
```

### paging_init流程

```mermaid
flowchart LR
paging_init[paging_init<初始化页表>]
paging_init--> map_mem[map_mem]
paging_init--> memblock_allow_resize[memblock_allow_resize<br/>设置允许memblock resize标志]
paging_init--> create_idmap[create_idmap]
paging_init--> declare_kernel_vmas[declare_kernel_vmas<br/>向vmlist添加vm_area]

map_mem--> arm64_kfence_alloc_pool[arm64_kfence_alloc_pool]
map_mem--> memblock_mark_nomap[memblock_mark_nomap<br/>kernel start和end范围设置nomap标志]
map_mem--> for_each_mem_range[for_each_mem_range<br/>遍历memblock中所有region<br/>会skip nomap]
for_each_mem_range--> __map_memblock[__map_memblock<br/>映射到linear region]
map_mem--> __map_memblock1[__map_memblock<br/>给kernel start和end在linear区域做个alias映射<br/>并只保留ro权限]
map_mem--> memblock_clear_nomap[memblock_clear_nomap<br/>清除kernel start和end范围nomap标志]
map_mem--> arm64_kfence_map_pool[arm64_kfence_map_pool]

create_idmap--> __pi_map_range[__pi_map_range<br/>__idmap_text_start和__idmap_text_end区域创建id映射]
create_idmap--> __pi_map_range1[__pi_map_range<br/>__idmap_kpti_flag创建id映射]

declare_kernel_vmas--> declare_vma[declare_vma<br/>添加.text到vmlist]
declare_kernel_vmas--> declare_vma1[declare_vma<br/>添加.rodata到vmlist]
declare_kernel_vmas--> declare_vma2[declare_vma<br/>添加.init.text到vmlist]
declare_kernel_vmas--> declare_vma3[declare_vma<br/>添加.init.data到vmlist]
declare_kernel_vmas--> declare_vma4[declare_vma<br/>添加.data到vmlist]


```
