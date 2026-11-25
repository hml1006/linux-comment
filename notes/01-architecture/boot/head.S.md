# 页表

## 页表概述

ARM64 内核启动过程中使用 4 组页表，它们在链接脚本中分配在连续内存区域内（地址相差 PAGE_SIZE=4K）：

| 页表 | 用途 | 生命周期 |
|------|------|----------|
| **init_idmap_pg_dir** | 早期 identity mapping 页表（Root Level），VA=PA | 仅用于开启 MMU 过渡阶段，之后不再使用 |
| **init_pg_dir** | 临时内核页表，加载到 ttbr1_el1 | 早期映射完成后被 swapper_pg_dir 替换 |
| **swapper_pg_dir** | 最终内核页表，pgd 默认值 | 整个内核生命周期 |
| **idmap_pg_dir** | identity mapping 页表（部分功能也会用到） | 整个内核生命周期 |

### identity mapping 说明

identity mapping 是开启 MMU 的过渡阶段所用，VA 等于 PA。不需要映射整个内核，只需要映射操作 MMU 代码相关的部分（`.idmap.text` 段）。

链接脚本中自定义段声明（`arch/arm64/kernel/vmlinux.lds.S`）：

```ld
    . = ALIGN(SZ_4K);
    __idmap_text_start = .;
    *(.idmap.text)
    __idmap_text_end = .;
```

### 页表切换流程

```
primary_entry 阶段（MMU 关闭）:
  TTBR0_EL1 = init_idmap_pg_dir  (identity mapping, VA=PA)
  TTBR1_EL1 = 未设置

__primary_switch → __enable_mmu（MMU 开启）:
  TTBR0_EL1 = init_idmap_pg_dir  (仍为 identity mapping)
  TTBR1_EL1 = reserved_pg_dir    (保留页表，防止 speculative page table walk)

__pi_early_map_kernel → map_kernel（创建内核映射）:
  --- 第一步: map_segment 完成所有段映射到 init_pg_dir ---
  TTBR1_EL1 = init_pg_dir        (临时内核页表，通过 idmap_cpu_replace_ttbr1 切换)
  --- 第二步: 重定位 + 重新映射 text 段 ---
  --- 第三步: 拷贝 init_pg_dir → swapper_pg_dir ---
  TTBR1_EL1 = swapper_pg_dir     (最终内核页表，通过 idmap_cpu_replace_ttbr1 切换)
```

# head.S 启动过程

## 寄存器使用约定

| 寄存器 | 作用域 | 用途 |
|--------|--------|------|
| x19 | primary_entry() ~ start_kernel() | 记录启动时 MMU 开启/关闭状态 |
| x20 | primary_entry() ~ __primary_switch() | CPU boot mode（EL1/EL2） |
| x21 | primary_entry() ~ start_kernel() | FDT 设备树指针（来自 x0） |

## 启动流程总览

```
primary_entry                                                  [head.S]
│
├─ record_mmu_state                                            [head.S]
│     从 sctlr_el1/sctlr_el2 提取 M bit（MMU 开关状态），存入 x19
│     同时校正大小端（代码定义大小端与 CPU 寄存器不一致时翻转）
│     x19 = 0  → MMU 关闭（进入时 dcache 必须关闭）
│     x19 ≠ 0  → MMU 开启（EL2 透传场景）
│
├─ preserve_boot_args                                          [head.S]
│     将 BootLoader 传递的 x0~x3 保存到 boot_args[4] 数组
│     x21 = x0（保存 FDT 地址）
│     如果 MMU 关闭，dcache_inval_poc 使 boot_args 缓存失效
│     如果 MMU 开启，记录 mmu_enabled_at_boot = x19
│
├─ sp = early_init_stack（设置临时栈指针）
│
├─ x0 = __pi_init_idmap_pg_dir, x1 = 0
│  __pi_create_init_idmap                                      [map_range.c]
│     创建 identity mapping 页表，VA=PA
│     返回下一个可用页表地址（x0），用于后续 cache 操作
│     │
│     ├─ map_range(_stext, __initdata_begin, PAGE_KERNEL_ROX)  [map_range.c]
│     │    映射 .text 段为只读可执行（init text section）
│     │
│     └─ map_range(__initdata_begin, _end, PAGE_KERNEL)        [map_range.c]
│          映射 .init.data 段为读写（init data section）
│
├─ [条件分支: x19 判断]                                        [head.S]
│  │
│  ├─ x19 == 0（MMU 关闭时进入）:
│  │     x1 = x0 (create_init_idmap 返回的 end)
│  │     x0 = __pi_init_idmap_pg_dir
│  │     dcache_inval_poc(x0, x1)         —— 使页表缓存行失效
│  │
│  └─ x19 != 0（MMU 开启时进入）:
│        x0 = __idmap_text_start
│        x1 = __idmap_text_end
│        dcache_clean_poc(x0, x1)          —— 清洗 idmap 代码段到 PoC
│
├─ x0 = x19 (MMU 状态)
│  init_kernel_el                                                [head.S]
│     初始化 CPU 运行级别（EL1 或 EL2）
│     │
│     ├─ 当前 EL1 → init_el1:
│     │     sctlr_el1 = INIT_SCTLR_EL1_MMU_OFF（MMU 关闭）
│     │     spsr_el1 = INIT_PSTATE_EL1, elr_el1 = lr
│     │     w0 = BOOT_CPU_MODE_EL1
│     │     eret 返回
│     │
│     └─ 当前 EL2 → init_el2:
│           如果 MMU 开启，先 clean hyp 代码到 PoC
│           sctlr_el2 = INIT_SCTLR_EL2_MMU_OFF
│           init_el2_hcr —— 初始化虚拟化控制寄存器
│           init_el2_state —— 初始化 EL2 系统寄存器
│           vbar_el2 = __hyp_stub_vectors
│           └─ 检查 HCR_EL2.E2H:
│              ├─ E2H=1（VHE 模式）→ SCTLR_EL12 设置, BOOT_CPU_FLAG_E2H
│              └─ E2H=0（非 VHE）→ sctlr_el1 设置
│           spsr_el2 = INIT_PSTATE_EL1
│           w0 = BOOT_CPU_MODE_EL2 | flags
│           eret 返回
│     x20 = x0 (保存 boot mode)
│
├─ __cpu_setup                                                   [proc.S]
│     初始化处理器，为开启 MMU 做准备
│     │
│     ├─ tlbi vmalle1 —— 清空本地 TLB
│     ├─ dsb nsh —— 数据同步屏障
│     ├─ cpacr_el1 = 0 —— 禁止 FPU/SIMD
│     ├─ mdscr_el1 = MDSCR_EL1_TDCC —— 禁止从 EL0 访问 DCC（debug 功能）
│     ├─ reset_pmuserenr_el0 —— 禁止从 EL0 访问 PMU
│     ├─ reset_amuserenr_el0 —— 禁止从 EL0 访问 AMU
│     ├─ mair_el1 = MAIR_EL1_SET —— 配置内存属性（Memory Attribute Indirection Register）
│     ├─ tcr_el1 配置:
│     │   - T0SZ = IDMAP_VA_BITS（identity mapping 地址宽度）
│     │   - T1SZ = VA_BITS_MIN（内核地址宽度）
│     │   - TCR_CACHE_FLAGS, TCR_SHARED, TCR_TG_FLAGS
│     │   - TCR_KASLR_FLAGS, TCR_EL1_AS, TCR_EL1_TBI0
│     │   - tcr_clear_errata_bits —— 修正 errata 有问题的 bit
│     │   - 如果 VA_BITS==52: tcr_set_t1sz, LPA2 时设置 TCR_EL1_DS
│     │   - tcr_compute_pa_size —— 设置 IPS（物理地址空间大小）
│     │   - 如果 HW_AFDBM: 设置 TCR_EL1_HA（硬件 Access Flag 更新）
│     │     - 如果支持 HAFT: 设置 TCR2_EL1_HAFT
│     ├─ 如果支持 S1PIE: 配置 PIE 权限隔离扩展寄存器
│     ├─ 如果支持 TCRX: 配置 TCR2_EL1
│     └─ x0 = INIT_SCTLR_EL1_MMU_ON（返回 SCTLR 值，供 __enable_mmu 使用）
│
└─ __primary_switch                                              [head.S]
     │
     ├─ x1 = reserved_pg_dir
     │  x2 = __pi_init_idmap_pg_dir
     │  __enable_mmu(x0=SCTLR值, x1=reserved_pg_dir, x2=init_idmap_pg_dir)
     │  │                                                         [head.S]
     │  ├─ 检查 ID_AA64MMFR0_EL1.TGRAN 是否支持当前页面粒度
     │  │  ├─ 小于最小值 → __no_granule_support（死循环）
     │  │  └─ 大于最大值 → __no_granule_support（死循环）
     │  ├─ ttbr0_el1 = phys_to_ttbr(x2)   —— identity mapping 页表
     │  ├─ ttbr1_el1 = load_ttbr1(x1)     —— reserved_pg_dir（空页表）
     │  └─ set_sctlr_el1(x0)              —— 写入 SCTLR, 开启 MMU
     │
     ├─ sp = early_init_stack（重新设置临时栈，因为 MMU 开启后虚拟地址变了）
     │  x29 = 0
     │
     ├─ x0 = x20 (boot status)
     │  x1 = x21 (FDT 地址)
     │  __pi_early_map_kernel                                     [map_kernel.c]
     │  │  ≡ early_map_kernel(boot_status, fdt)
     │  │
     │  ├─ map_fdt(fdt)                                           [map_kernel.c]
     │  │     map_range(fdt, fdt+MAX_FDT_SIZE, ..., PAGE_KERNEL)
     │  │     把 FDT 映射到 init_idmap_pg_dir 的 identity mapping 中
     │  │     返回映射后的 FDT 指针
     │  │
     │  ├─ clear_bss
     │  │     memset(__bss_start, 0, init_pg_end - __bss_start)
     │  │     清空 BSS 段和初始页表占用的内存
     │  │
     │  ├─ init_feature_override(boot_status, fdt, chosen)
     │  │     从设备树 /chosen 节点解析 CPU feature 覆盖参数
     │  │
     │  ├─ kaslr_early_init(fdt, chosen)                          [kaslr_early.c]
     │  │     从设备树 /chosen 节点读取 "kaslr-seed" 属性
     │  │     如果不存在，尝试使用 CPU 的 RNDR 指令生成随机数
     │  │     计算 KASLR 偏移量: range/2 + (range * seed) >> 64
     │  │     返回 kaslr_seed（高位移到 MIN_KIMG_ALIGN 对齐之外）
     │  │
     │  ├─ 计算 va_base = KIMAGE_VADDR + kaslr_offset
     │  │
     │  ├─ [如果 LPA2 且 VA_BITS > VA_BITS_MIN]:
     │  │  remap_idmap_for_lpa2()                                 [map_kernel.c]
     │  │     ├─ 创建临时 ID map（清除 PTE_SHARED bit）
     │  │     ├─ set_ttbr0_for_lpa2() 切换到临时页表
     │  │     ├─ 重新创建 init_idmap_pg_dir（LPA2 兼容格式）
     │  │     └─ set_ttbr0_for_lpa2() 切换回 init_idmap_pg_dir
     │  │
     │  ├─ map_kernel(kaslr_offset, va_base-pa_base, root_level)  [map_kernel.c]
     │  │  │
     │  │  ├─ [第一遍: 所有段映射到 init_pg_dir]
     │  │  │  │
     │  │  │  ├─ map_segment(_text, _stext, data_prot)      —— 非可执行代码段
     │  │  │  ├─ map_segment(_stext, _etext, prot)          —— .text 段
     │  │  │  │     prot = twopass ? data_prot : text_prot
     │  │  │  │     twopass 条件: RELOCATABLE || SCS
     │  │  │  ├─ map_segment(__start_rodata, __inittext_begin, data_prot)
     │  │  │  │                                              —— .rodata 段
     │  │  │  ├─ map_segment(__inittext_begin, __inittext_end, prot)
     │  │  │  │                                              —— .init.text 段
     │  │  │  ├─ map_segment(__initdata_begin, __initdata_end, data_prot)
     │  │  │  │                                              —— .init.data 段
     │  │  │  └─ map_segment(_data, _end, data_prot)         —— .data 段
     │  │  │
     │  │  ├─ idmap_cpu_replace_ttbr1(init_pg_dir)             [proc.S]
     │  │  │     └─ 切换 TTBR1_EL1 = init_pg_dir（临时内核页表生效）
     │  │  │
     │  │  ├─ [第二遍: 如果 twopass]
     │  │  │  │
     │  │  │  ├─ relocate_kernel(kaslr_offset)                 [relocate.c]
     │  │  │  │     处理 R_AARCH64_RELATIVE 类型重定位
     │  │  │  │     处理 RELR 压缩格式重定位
     │  │  │  │
     │  │  │  ├─ [如果 SCS 动态开启]: scs_patch + ic ialluis
     │  │  │  │
     │  │  │  ├─ unmap_segment(_stext, _etext) —— 取消 .text 映射
     │  │  │  ├─ __tlbi(vmalle1) —— 清 TLB
     │  │  │  │
     │  │  │  └─ map_segment(_stext, _etext, text_prot) —— 重新映射为只读可执行
     │  │  │     map_segment(__inittext_begin, __inittext_end, text_prot)
     │  │  │
     │  │  └─ [最终切换]
     │  │        ├─ memcpy(swapper_pg_dir, init_pg_dir, PAGE_SIZE)
     │  │        │    拷贝根页表到最终位置
     │  │        └─ idmap_cpu_replace_ttbr1(swapper_pg_dir)
     │  │             切换 TTBR1_EL1 = swapper_pg_dir（最终内核页表生效）
     │  │
     │  └─ 返回 __primary_switch
     │
     ├─ x8 = __primary_switched
     │  x0 = __pa(KERNEL_START)
     │  br x8（跳转到主内核初始化）
     │
     └─ __primary_switched                                       [head.S]
        │
        ├─ init_cpu_task(init_task, x5, x6)
        │     ├─ sp_el0 = init_task（task 地址）
        │     ├─ sp = init_task.stack + THREAD_SIZE - PT_REGS_SIZE
        │     ├─ stackframe 清空并设置 FRAME_META_TYPE_FINAL
        │     ├─ x29 = sp + S_STACKFRAME（设置 FP 栈帧指针）
        │     ├─ scs_load_current（影子调用栈）
        │     └─ set_this_cpu_offset（per-CPU 变量基址寄存器 tpidr_el1）
        │
        ├─ vbar_el1 = vectors（设置 EL1 中断向量表，位于 entry.S）
        │
        ├─ __fdt_pointer = x21（保存 FDT 指针到全局变量）
        │
        ├─ kimage_voffset = _text - x0（保存内核虚拟地址到物理地址偏移）
        │
        ├─ set_cpu_boot_mode_flag(x20)
        │    把 CPU boot mode 保存到 __boot_cpu_mode 全局变量
        │
        ├─ [如果 KASAN 开启]: kasan_early_init()
        │    KASAN 功能初步初始化（ARM64 MTE 可硬件支持 KASAN）
        │
        ├─ finalise_el2(x20)
        │    VHE 虚拟化扩展设置，如果支持 VHE 则启用
        │
        └─ start_kernel()
              正式启动内核（init/main.c）
```

## 关键函数详解

### record_mmu_state

```asm
SYM_CODE_START_LOCAL(record_mmu_state)
    mrs     x19, CurrentEL
    cmp     x19, #CurrentEL_EL2
    mrs     x19, sctlr_el1        // 获取 EL1 系统控制寄存器
    b.ne    0f                    // 不是 EL2 则跳过
    mrs     x19, sctlr_el2        // 是 EL2 则获取 EL2 系统控制寄存器
0:
    tbnz    x19, #SCTLR_ELx_EE_SHIFT, 1f  // 检查大小端
    tst     x19, #SCTLR_ELx_C            // 检查 cache 状态
    and     x19, x19, #SCTLR_ELx_M       // 提取 M bit（MMU 开关）
    csel    x19, xzr, x19, eq            // 如果 C=0（cache 关闭），x19=0
    ret
1:  // 大小端校正
    eor     x19, x19, #SCTLR_ELx_EE     // 翻转大小端 bit
    bic     x19, x19, #SCTLR_ELx_M      // 关闭 MMU
    ...                                  // 写入 sctlr 并 isb
    mov     x19, xzr                     // x19=0 表示 MMU 关闭
    ret
```

关键点:
- `x19 = 0` → 启动时 MMU 关闭（最常见情况）
- `x19 ≠ 0` → 启动时 MMU 开启（EL2 透传，如 EFI 启动）
- 大小端校正是为了处理代码定义大小端与 CPU 实际大小端不一致的情况

### __pi_create_init_idmap

```c
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
{
    phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE;
    pgprot_t text_prot = PAGE_KERNEL_ROX;  // 只读可执行
    pgprot_t data_prot = PAGE_KERNEL;      // 读写

    pgprot_val(text_prot) &= ~clrmask;
    pgprot_val(data_prot) &= ~clrmask;

    // 映射 .text 段: [ _stext, __initdata_begin ) → PAGE_KERNEL_ROX
    map_range(&ptep, (u64)_stext, (u64)__initdata_begin,
              (phys_addr_t)_stext, text_prot, IDMAP_ROOT_LEVEL,
              (pte_t *)pg_dir, false, 0);

    // 映射 .init.data 段: [ __initdata_begin, _end ) → PAGE_KERNEL
    map_range(&ptep, (u64)__initdata_begin, (u64)_end,
              (phys_addr_t)__initdata_begin, data_prot, IDMAP_ROOT_LEVEL,
              (pte_t *)pg_dir, false, 0);

    return ptep;  // 返回下一个可用页表地址
}
```

关键点:
- 使用 `init_idmap_pg_dir` 作为根页表
- `clrmask` 参数用于 LPA2 时清除特定 bit，普通启动时为 0
- 只映射 `.text` 和 `.init.data` 两个段，最小化 identity mapping 范围
- MMU 关闭时执行，指针可直接当作物理地址使用

### __enable_mmu

```asm
// x0 = SCTLR_EL1 value (MMU ON)
// x1 = TTBR1_EL1 value (reserved_pg_dir)
// x2 = ID map root table address (init_idmap_pg_dir)
SYM_FUNC_START(__enable_mmu)
    mrs     x3, ID_AA64MMFR0_EL1
    ubfx    x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
    cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
    b.lt    __no_granule_support       // 粒度太小
    cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
    b.gt    __no_granule_support       // 粒度太大
    phys_to_ttbr x2, x2
    msr     ttbr0_el1, x2              // identity mapping 页表
    load_ttbr1 x1, x1, x3              // reserved_pg_dir
    set_sctlr_el1  x0                  // 写入 SCTLR, 开启 MMU
    ret
```

关键点:
- 开启 MMU 前检查 CPU 是否支持当前配置的页面粒度
- `ttbr0_el1` = init_idmap_pg_dir（identity mapping）
- `ttbr1_el1` = reserved_pg_dir（空页表，防止 speculative walk 访问到错误地址）
- `set_sctlr_el1` 写入 SCTLR 寄存器，其中 M bit=1 开启 MMU

### early_map_kernel (__pi_early_map_kernel)

```c
asmlinkage void __init early_map_kernel(u64 boot_status, phys_addr_t fdt)
{
    // 1. map_fdt(fdt) —— 映射 FDT 到 identity mapping 中
    // 2. memset(__bss_start, 0, ...) —— 清 BSS
    // 3. init_feature_override() —— 解析设备树 CPU feature 覆盖
    // 4. kaslr_early_init(fdt, chosen) —— 读取 KASLR seed
    // 5. 计算 va_base = KIMAGE_VADDR + kaslr_offset
    // 6. [LPA2] remap_idmap_for_lpa2()
    // 7. map_kernel(kaslr_offset, va_base - pa_base, root_level)
    //     ├─ 第一遍: 映射所有段到 init_pg_dir
    //     ├─ idmap_cpu_replace_ttbr1(init_pg_dir)
    //     ├─ [第二遍如果 twopass]:
    //     │   ├─ relocate_kernel(kaslr_offset)
    //     │   ├─ unmap + 重新映射 text 为只读
    //     └─ memcpy → swapper_pg_dir
    //        idmap_cpu_replace_ttbr1(swapper_pg_dir)
}
```

### map_kernel 内部映射的段

| 段 | 虚拟地址范围 | 保护属性 | 说明 |
|----|-------------|----------|------|
| 非代码段 | [_text, _stext) | data_prot (RW) | 启动后不执行的代码，含非可执行数据 |
| .text | [_stext, _etext) | text_prot (ROX) / data_prot | 主代码段 |
| .rodata | [__start_rodata, __inittext_begin) | data_prot (RW) | 只读数据 |
| .init.text | [__inittext_begin, __inittext_end) | text_prot / data_prot | 初始化代码 |
| .init.data | [__initdata_begin, __initdata_end) | data_prot (RW) | 初始化数据 |
| .data | [_data, _end) | data_prot (RW) | 数据段 |

`twopass` 条件:
- `CONFIG_RELOCATABLE=y`（KASLR 需要重定位）
- 或 `CONFIG_UNWIND_PATCH_PAC_INTO_SCS=y`（需要动态 patch 影子调用栈）

第一遍所有段映射为 RW，第二遍 relocate_kernel 后重新映射 text 段为 ROX。

### init_cpu_task

```asm
.macro init_cpu_task tsk, tmp1, tmp2
    msr     sp_el0, \tsk             // task 地址备份到 sp_el0
    ldr     \tmp1, [\tsk, #TSK_STACK]
    add     sp, \tmp1, #THREAD_SIZE  // sp 指向栈底
    sub     sp, sp, #PT_REGS_SIZE    // 保留 pt_regs 空间
    stp     xzr, xzr, [sp, #S_STACKFRAME]  // 清空 stackframe
    mov     \tmp1, #FRAME_META_TYPE_FINAL
    str     \tmp1, [sp, #S_STACKFRAME_TYPE]
    add     x29, sp, #S_STACKFRAME   // FP 指向 stackframe
    scs_load_current                  // 影子调用栈
    ...                               // per-CPU offset
.endm
```

## 启动流程要点总结

1. **入口点**: `primary_entry` 是内核启动入口，`b primary_entry` 位于 `.head.text` 段
2. **MMU 关闭/开启检测**: 通过 `record_mmu_state` 区分两种启动场景，影响后续 cache 操作
3. **Identity mapping**: `create_init_idmap` 只映射 `.text` 和 `.init.data` 段，最小化映射范围
4. **EL 初始化**: `init_kernel_el` 处理 EL1/EL2 两种启动模式，EL2 模式下还处理 VHE 虚拟化
5. **CPU 配置**: `__cpu_setup` 配置 MAIR、TCR、SCTLR 等关键系统寄存器，为 MMU 开启做准备
6. **MMU 开启**: `__enable_mmu` 设置 TTBR0/TTBR1，写入 SCTLR 开启 MMU（同时检查页面粒度支持）
7. **内核映射**: `early_map_kernel` 完成 FDT 映射、BSS 清零、KASLR 初始化、完整内核映射
8. **两遍映射**: 可重定位内核需要两遍映射——第一遍 RW 映射做重定位，第二遍改为 ROX
9. **页表切换**: `init_pg_dir` → `swapper_pg_dir` 的切换通过 `idmap_cpu_replace_ttbr1` 完成
10. **最终跳转**: `__primary_switched` 初始化 task struct、中断向量表、KASAN、VHE，然后调用 `start_kernel`