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

```plantuml
@startsalt
{
{T
    + primary_entry
    ++ record_mmu_state     | 从系统控制寄存器提取mmu开启关闭状态放入x19
    ++ preserve_boot_args   | 把BootLoader传递的x0-x3寄存器放入boot_args数组
    ++ 把sp设置为early_init_stack
    ++ __pi_create_init_idmap| map_range.c:create_init_idmap,创建虚地址到物理地址的一一映射,开启MMU要用到,VA等于PA
    +++ map_range           | init text section映射,初始化完成后释放内存
    +++ map_range           | init data section映射,初始化完成后释放内存
    ++ dcache_inval_poc     | 如果MMU disable,invalidate __pi_init_idmap_pg_dir
    ++ init_kernel_el       | 初始化CPU boot mode,EL1还是EL2
    ++ __cpu_setup          | enable FP/SIMD,debug pmu访问权限,mair寄存器内存属性设置,页表和内存调试功能Feature设置,虚地址物理地址bit长度设置
    +++ 清TLB
    +++ 禁止FPU和SIMD
    +++ 禁止debug功能
    +++ 禁止从EL0访问PMU,AMU
    +++ 配置MAIR寄存器内存属性
    +++ 计算设置地址宽度
    +++ 准备SCTLR寄存器内容放x0
    ++ __primary_switch
    +++ __enable_mmu         | 使能mmu,ttbr0_el1设置为init_idmap_pg_dir
    +++ 把sp设置为early_init_stack
    +++ __pi_early_map_kernel| 传入FDT地址,从FDT读取seed并生成一个offset
    ++++ map_fdt             | 把FDT映射到idmap
    ++++ clear_bss           | 清bss段
    ++++ kaslr_early_init    | 从FDT读取seed并计算一个kaslr seed
    ++++ 计算va_base地址
    ++++ map_kernel          | 创建kernel映射,VA和PA不相等
    +++++ map_segment         | .text section
    +++++ map_segment         | .rodata section
    +++++ map_segment         | .init.text section
    +++++ map_segment         | .init.data section
    +++++ map_segment         | .data section
    +++++ idmap_cpu_replace_ttbr1| 把init_pg_dir设置到ttbr1
    +++++ relocate_kernel     | 重定位内核kaslr feature,R_AARCH64_RELATIVE重定位类型
    +++++ 把text section取消write权限重新map
    +++++ 把init_pg_dir拷贝到swapper_pg_dir
    +++++ idmap_cpu_replace_ttbr1| 把swapper_pg_dir设置到ttbr1
    +++ __primary_switched   | 初始化task struct,设置中断向量表,设置CPU boot mode,初始化kasan功能,设置VHE虚拟化扩展,启动内核
    ++++ init_cpu_task       | 初始化一个task struct,用来做栈回溯
    ++++ 设置中断向量表,取__fdt_pointer和内核镜像地址
    ++++ set_cpu_boot_mode_flag | 把CPU boot mode保存到全局变量
    ++++ kasan_early_init    | kasan功能初步初始化,arm64 MTE Feature可硬件支持kasan
    ++++ finalise_el2VHE 虚拟化扩展设置
    ++++ start_kernel        | 启动内核
}
}
@endsalt
```
