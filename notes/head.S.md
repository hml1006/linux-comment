# 页表

**idmap_pg_dir**是identity mapping使用的页表， 从CPU启动会用到， 部分其他功能也会用到。
**swapper_pg_dir**是kernel image mapping最终使用的页表。pgd地址默认初始化为这个值。

**init_idmap_pg_dir**是**Root Level**的identity mapping全局页表，会加载到页表寄存器。

**init_pg_dir**高地址全局页表，临时的内核页表，加载到ttbr1_el1，会被swapper_pg_dir替换。

请注意，这里的内存是一段连续内存。也就是说页表（PGD/PUD/PMD）都是连在一起的，地址相差PAGE_SIZE（4k）。
identity mapping主要是打开MMU的过度阶段，因此对于identity mapping不需要映射整个kernel，只需要映射操作MMU代码相关的部分。这段代码是利用linux中常用手段自定义代码段，自定义的代码段的名称是".idmap.text"。除此之外，肯定还需要在链接脚本中声明两个标量，用来标记代码段的开始和结束。可以从vmlinux.lds.S中找到答案。

```ld
    . = ALIGN(SZ_4K);  
    __idmap_text_start = .;  
    *(.idmap.text)  
    __idmap_text_end = .;
```

# head.S启动过程

```mermaid
flowchart LR
primary_entry[primary_entry]
primary_entry--> rms[record_mmu_state<br/>从系统控制寄存器提取mmu开启关闭状态放入x19]
primary_entry--> pbr[preserve_boot_args<br/>把BootLoader传递的x0-x3寄存器放入boot_args数组]
primary_entry--> cim[__pi_create_init_idmap<br/>map_range.c:create_init_idmap<br/>创建虚地址到物理地址的一一映射,开启MMU要用到<br/>VA等于PA]
cim--> mrt[map_range<br/> init text section映射<br/>初始化完成后释放内存]
cim--> mrd[map_range<br/> init data section映射<br/>初始化完成后释放内存]
primary_entry-->dc[dcache_clean_poc<br/> 刷cache]
primary_entry--> ik[init_kernel_el<br/>初始化CPU boot mode,EL1还是EL2,<br/>]
primary_entry--> cs[__cpu_setup<br/>enable FP/SIMD,debug pmu访问权限<br/>mair寄存器内存属性设置,<br/>页表和内存调试功能Feature设置<br/>虚地址物理地址bit长度设置]
cs--> 清TLB
cs--> 禁止FPU和SIMD
cs--> 禁止debug功能
cs--> 禁止从EL0访问PMU,AMU
cs--> 配置MAIR寄存器内存属性
cs--> 计算设置地址宽度
cs--> 准备SCTLR寄存器内容放x0
primary_entry--> ps[__primary_switch]

```

# head.S启动子流程

## __primary_switch 流程

```mermaid
flowchart LR
ps[__primary_switch] 
ps--> em[__enable_mmu 使能mmu>ttbr0_el1设置为init_idmap_pg_dir]
ps--> emk[__pi_early_map_kernel<br/>传入FDT地址,从FDT读取seed并生成一个offset]
ps--> psd[__primary_switched]
```

### early map kernel

```mermaid
flowchart LR
emk[__pi_early_map_kernel<br/>传入FDT地址,从FDT读取seed并生成一个offset]
emk--> mf[map_fdt<br/>把FDT映射到idmap]
emk--> clr_bss[clear_bss<br/>清bss段]
emk--> ki[kaslr_early_init<br/>从FDT读取seed并<br/>计算一个kaslr seed]
emk--> va_base[根据kaslr seed计算va_base地址]
emk--> mp[map_kernel<br/>创建kernel映射,VA和PA不相等]
mp--> mst[map_segment<br/>.text section]
mp--> msr[map_segment<br/>.rodata section]
mp--> msit[map_segment<br/>.init.text section]
mp--> msid[map_segment<br/>.init.data section]
mp--> msd[map_segment<br/>.data section]
mp--> idmap_cpu_replace_ttbr1[idmap_cpu_replace_ttbr1<br/>把init_pg_dir设置到ttbr1]
mp--> mk[relocate_kernel<br/>重定位内核kaslr feature<br/>R_AARCH64_RELATIVE重定位类型]
mp--> remap[把text section取消write权限重新map]
mp--> cp[把init_pg_dir拷贝到swapper_pg_dir并更新ttbr1_el1]
```

### _primary_switched流程

```mermaid
flowchart LR
psd[__primary_switched]
psd--> ict[init_cpu_task<br/>初始化一个task struct,用来做栈回溯]
psd--> ifdt[设置中断向量表,取__fdt_pointer和内核镜像地址]
psd--> scbm[set_cpu_boot_mode_flag<br/>把CPU boot mode保存到全局变量]
psd--> kei[kasan_early_init<br/>kasan功能初步初始化,arm64 MTE Feature可硬件支持kasan]
psd--> vhe[finalise_el2VHE虚拟化扩展设置]
psd--> sk[start_kernel]
```