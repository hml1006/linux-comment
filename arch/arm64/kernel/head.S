/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level CPU initialisation
 * Based on arch/arm/kernel/head.S
 *
 * Copyright (C) 1994-2002 Russell King
 * Copyright (C) 2003-2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/pgtable.h>

#include <asm/asm_pointer_auth.h>
#include <asm/assembler.h>
#include <asm/boot.h>
#include <asm/bug.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/cache.h>
#include <asm/cputype.h>
#include <asm/el2_setup.h>
#include <asm/elf.h>
#include <asm/image.h>
#include <asm/kernel-pgtable.h>
#include <asm/kvm_arm.h>
#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>
#include <asm/page.h>
#include <asm/scs.h>
#include <asm/smp.h>
#include <asm/sysreg.h>
#include <asm/stacktrace/frame.h>
#include <asm/thread_info.h>
#include <asm/virt.h>

#include "efi-header.S"

#if (PAGE_OFFSET & 0x1fffff) != 0
#error PAGE_OFFSET must be at least 2MB aligned
#endif

/*
 * Kernel startup entry point.
 * ---------------------------
 *
 * The requirements are:
 *   MMU = off, D-cache = off, I-cache = on or off,
 *   x0 = physical address to the FDT blob. x0 指向设备树地址
 *
 * Note that the callee-saved registers are used for storing variables
 * that are useful before the MMU is enabled. The allocations are described
 * in the entry routines.
 */
 /**
 为什么跳转到内核时指令高速缓存可以打开，数据高速缓存必须关闭？
 首先L1 icache是无法关闭的，其次dcache依赖mmu，mmu关闭，dcache无法使用。mmu关闭，所有内存默认non-cacheable
 **/
	__HEAD	// .section	".head.text","ax" 指定section
	/*
	 * DO NOT MODIFY. Image header expected by Linux boot-loaders.
	 */
	 /**
	 efi_signature_nop 其作用是在 EFI 签名列表中添加一个空占位符项
	 **/
	efi_signature_nop			// special NOP to identity as PE/COFF executable
	// 内核启动初始入口
	// b 指令使用label，是相对地址，使用寄存器是绝对地址跳转
	b	primary_entry			// branch to kernel start, magic
	.quad	0				// Image load offset from start of RAM, little-endian
	le64sym	_kernel_size_le			// Effective size of kernel image, little-endian
	le64sym	_kernel_flags_le		// Informative flags, little-endian
	.quad	0				// reserved
	.quad	0				// reserved
	.quad	0				// reserved
	.ascii	ARM64_IMAGE_MAGIC		// Magic number
	.long	.Lpe_header_offset		// Offset to the PE header.

	__EFI_PE_HEADER

	.section ".idmap.text","a"

	/*
	 * The following callee saved general purpose registers are used on the
	 * primary lowlevel boot path:
	 *
	 *  Register   Scope                      Purpose
	 *  x19        primary_entry() .. start_kernel()        whether we entered with the MMU on
	 *  x20        primary_entry() .. __primary_switch()    CPU boot mode
	 *  x21        primary_entry() .. start_kernel()        FDT pointer passed at boot in x0
	 */
SYM_CODE_START(primary_entry)
	bl	record_mmu_state	// 从系统控制寄存器提取mmu开启关闭状态放入x19
	bl	preserve_boot_args	// 把BootLoader传递的x0-x3寄存器放入boot_args数组

	adrp	x1, early_init_stack	// 设置内核启动时临时使用的stack基址
	mov	sp, x1
	mov	x29, xzr
	adrp	x0, __pi_init_idmap_pg_dir	// 设置一一映射页表的地址
	mov	x1, xzr
	bl	__pi_create_init_idmap		// 创建虚地址到物理地址的一一映射，开启MMU要用到

	/*
	 * If the page tables have been populated with non-cacheable
	 * accesses (MMU disabled), invalidate those tables again to
	 * remove any speculatively loaded cache lines.
	 */
	cbnz	x19, 0f
	dmb     sy
	mov	x1, x0				// end of used region
	adrp    x0, __pi_init_idmap_pg_dir
	adr_l	x2, dcache_inval_poc
	blr	x2					// invalidate cache
	b	1f

	/*
	 * If we entered with the MMU and caches on, clean the ID mapped part
	 * of the primary boot code to the PoC so we can safely execute it with
	 * the MMU off.
	 */
0:	adrp	x0, __idmap_text_start
	adr_l	x1, __idmap_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

1:	mov	x0, x19	// mmu 状态放 x0
	bl	init_kernel_el			// w0=cpu_boot_mode
	mov	x20, x0					// 把x0 中的CPU boot mode备份到x20

	/*
	 * The following calls CPU setup code, see arch/arm64/mm/proc.S for
	 * details.
	 * On return, the CPU will be ready for the MMU to be turned on and
	 * the TCR will have been set.
	 */
	bl	__cpu_setup			// initialise processor
	b	__primary_switch
SYM_CODE_END(primary_entry)

	__INIT
SYM_CODE_START_LOCAL(record_mmu_state)
	mrs	x19, CurrentEL	// 获取当前运行级别
	cmp	x19, #CurrentEL_EL2	// 和EL2级别比较
	mrs	x19, sctlr_el1	// 获取 el1 系统控制寄存器内容
	b.ne	0f		// 如果当前不是EL2，跳转到label 0
	mrs	x19, sctlr_el2	// 当前是 el2, 获取el2 系统控制寄存器内容
0:
CPU_LE( tbnz	x19, #SCTLR_ELx_EE_SHIFT, 1f	) // 该bit为0是小端，为1是大端，所以用了两个test bit指令分别判断0和1，cpu实际大小端和宏定义的不一致，需要校正
CPU_BE( tbz	x19, #SCTLR_ELx_EE_SHIFT, 1f	)
	tst	x19, #SCTLR_ELx_C		// Z := (C == 0), C为i cache和d cache开关，1 - enable， 0 - disable
	and	x19, x19, #SCTLR_ELx_M		// isolate M bit，提取mmu状态
	csel	x19, xzr, x19, eq		// clear x19 if Z
	ret

	/*
	 * Set the correct endianness early so all memory accesses issued
	 * before init_kernel_el() occur in the correct byte order. Note that
	 * this means the MMU must be disabled, or the active ID map will end
	 * up getting interpreted with the wrong byte order.
	 */
	 // 代码定义的大小端和cpu寄存器读出来的大小端不一致，需要校正
1:	eor	x19, x19, #SCTLR_ELx_EE	// x19中大小端bit翻转
	bic	x19, x19, #SCTLR_ELx_M	// x19中mmu比特位关闭， 大小端对应bit发生翻转
	b.ne	2f	// 大端跳转label 2
	pre_disable_mmu_workaround	// 普通cpu为空
	msr	sctlr_el2, x19
	b	3f
2:	pre_disable_mmu_workaround
	msr	sctlr_el1, x19	// 改成小端，mmu关闭
3:	isb
	mov	x19, xzr
	ret
SYM_CODE_END(record_mmu_state)

/*
 * Preserve the arguments passed by the bootloader in x0 .. x3
 保存BootLoader传递的参数
 */
SYM_CODE_START_LOCAL(preserve_boot_args)
	mov	x21, x0				// x21=FDT

	// 获取boot_args地址，这个变量数组在setup.c中，用于保存x0 .. x3
	adr_l	x0, boot_args			// record the contents of
	stp	x21, x1, [x0]			// x0 .. x3 at kernel entry
	stp	x2, x3, [x0, #16]

	cbnz	x19, 0f				// skip cache invalidation if MMU is on
	dmb	sy				// needed before dc ivac with
						// MMU off

	add	x1, x0, #0x20			// 4 x 8 bytes
	// clean & invalidate boot_args数据
	b	dcache_inval_poc		// tail call
0:	str_l   x19, mmu_enabled_at_boot, x0	// mmu状态存储到这个变量，后续会用到
	ret
SYM_CODE_END(preserve_boot_args)

	/*
	 * Initialize CPU registers with task-specific and cpu-specific context.
	 *
	 * Create a final frame record at task_pt_regs(current)->stackframe, so
	 * that the unwinder can identify the final frame record of any task by
	 * its location in the task stack. We reserve the entire pt_regs space
	 * for consistency with user tasks and kthreads.
	 */
	.macro	init_cpu_task tsk, tmp1, tmp2
	msr	sp_el0, \tsk	// task地址备份到sp_el0

	// 初始化sp寄存器, 栈从高地址向低地址增长
	ldr	\tmp1, [\tsk, #TSK_STACK]
	add	sp, \tmp1, #THREAD_SIZE	// sp指向栈底
	sub	sp, sp, #PT_REGS_SIZE	// stack保留备份寄存器空间

	stp	xzr, xzr, [sp, #S_STACKFRAME]	// 清空 stackframe字段
	mov	\tmp1, #FRAME_META_TYPE_FINAL
	str	\tmp1, [sp, #S_STACKFRAME_TYPE]
	add	x29, sp, #S_STACKFRAME			// 设置x29  FP指向stackframe

	// 设置影子调用栈,备份返回地址, 返回时做检查
	scs_load_current

	adr_l	\tmp1, __per_cpu_offset
	ldr	w\tmp2, [\tsk, #TSK_TI_CPU]
	ldr	\tmp1, [\tmp1, \tmp2, lsl #3]
	set_this_cpu_offset \tmp1	// tpidr_el1, per-cpu实现寄存器
	.endm

/*
 * The following fragment of code is executed with the MMU enabled.
 *
 *   x0 = __pa(KERNEL_START)
 */
SYM_FUNC_START_LOCAL(__primary_switched)
	adr_l	x4, init_task		// struct task_struct init_task
	init_cpu_task x4, x5, x6	// 初始化task结构体

	// 设置el1 中断向量寄存器, 中断向量位于entry.S
	adr_l	x8, vectors			// load VBAR_EL1 with virtual
	msr	vbar_el1, x8			// vector table address
	isb

	stp	x29, x30, [sp, #-16]!
	mov	x29, sp

	// 设备树地址放x21
	str_l	x21, __fdt_pointer, x5		// Save FDT pointer

	// 内核镜像虚地址放x4, _text
	adrp	x4, _text			// Save the offset between
	sub	x4, x4, x0			// the kernel virtual and
	str_l	x4, kimage_voffset, x5		// physical mappings

	mov	x0, x20
	bl	set_cpu_boot_mode_flag	// cpu boot mode保存到全局变量

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	bl	kasan_early_init
#endif
	mov	x0, x20
	bl	finalise_el2			// Prefer VHE if possible
	ldp	x29, x30, [sp], #16
	bl	start_kernel			// 正式进入内核代码
	ASM_BUG()
SYM_FUNC_END(__primary_switched)

/*
 * end early head section, begin head code that is also used for
 * hotplug and needs to have the same protections as the text region
 */
	.section ".idmap.text","a"

/*
 * Starting from EL2 or EL1, configure the CPU to execute at the highest
 * reachable EL supported by the kernel in a chosen default state. If dropping
 * from EL2 to EL1, configure EL2 before configuring EL1.
 *
 * Since we cannot always rely on ERET synchronizing writes to sysregs (e.g. if
 * SCTLR_ELx.EOS is clear), we place an ISB prior to ERET.
 *
 * Returns either BOOT_CPU_MODE_EL1 or BOOT_CPU_MODE_EL2 in x0 if
 * booted in EL1 or EL2 respectively, with the top 32 bits containing
 * potential context flags. These flags are *not* stored in __boot_cpu_mode.
 *
 * x0: whether we are being called from the primary boot path with the MMU on
 */
SYM_FUNC_START(init_kernel_el)
	mrs	x1, CurrentEL
	cmp	x1, #CurrentEL_EL2
	b.eq	init_el2	// 当前是EL2, 跳转到el2初始化代码

SYM_INNER_LABEL(init_el1, SYM_L_LOCAL)
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF	// 设置mmu off条件下寄存器初始值
	pre_disable_mmu_workaround
	msr	sctlr_el1, x0					// 把上面的值写入系统控制寄存器
	isb									// 指令屏障，重新取指
	mov_q	x0, INIT_PSTATE_EL1			// 把el1初始pstate值放入x0
	msr	spsr_el1, x0					// 设置el1 pstate寄存器
	msr	elr_el1, lr						// 设置el1 异常返回地址为 lr
	mov	w0, #BOOT_CPU_MODE_EL1			// 设置boot CPU mode
	eret

SYM_INNER_LABEL(init_el2, SYM_L_LOCAL)
	msr	elr_el2, lr						// 备份lr寄存器

	// clean all HYP code to the PoC if we booted at EL2 with the MMU on
	cbz	x0, 0f
	adrp	x0, __hyp_idmap_text_start
	adr_l	x1, __hyp_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

	mov_q	x0, INIT_SCTLR_EL2_MMU_OFF
	pre_disable_mmu_workaround
	msr	sctlr_el2, x0
	isb
0:

	init_el2_hcr	HCR_HOST_NVHE_FLAGS | HCR_ATA		// 初始化虚拟化控制寄存器
	init_el2_state							// 初始化el2 寄存器

	/* Hypervisor stub */
	adr_l	x0, __hyp_stub_vectors
	msr	vbar_el2, x0					// 设置虚拟机管理程序异常向量表
	isb

	mov_q	x1, INIT_SCTLR_EL1_MMU_OFF

	mrs	x0, hcr_el2
	and	x0, x0, #HCR_E2H
	cbz	x0, 2f							// 检查是否设置了E2H, 设置1表示el2运行了一个操作系统, 设置0表示el2运行了虚拟机管理程序

	/* Set a sane SCTLR_EL1, the VHE way */
	msr_s	SYS_SCTLR_EL12, x1			// 设置mmu off下 vhe 模式系统控制寄存器各种功能和权限
	mov	x2, #BOOT_CPU_FLAG_E2H			// 设置boot cpu flag, vhe模式是虚拟机管理程序和操作系统都运行在el2
	b	3f								// vhe模式跳过 el1 系统控制寄存器设置, 因为用不到

2:
	msr	sctlr_el1, x1					// 设置 el1 系统控制寄存器
	mov	x2, xzr
3:
	mov	x0, #INIT_PSTATE_EL1
	msr	spsr_el2, x0

	mov	w0, #BOOT_CPU_MODE_EL2
	orr	x0, x0, x2						// 设置当前 boot CPU mode
	eret
SYM_FUNC_END(init_kernel_el)

	/*
	 * This provides a "holding pen" for platforms to hold all secondary
	 * cores are held until we're ready for them to initialise.
	 */
SYM_FUNC_START(secondary_holding_pen)
	mov	x0, xzr
	bl	init_kernel_el			// w0=cpu_boot_mode
	mrs	x2, mpidr_el1
	mov_q	x1, MPIDR_HWID_BITMASK
	and	x2, x2, x1
	adr_l	x3, secondary_holding_pen_release
pen:	ldr	x4, [x3]
	cmp	x4, x2				// 比较待启动CPU id，core0初始化完系统后需要启动其他core
	b.eq	secondary_startup
	wfe
	b	pen
SYM_FUNC_END(secondary_holding_pen)

	/*
	 * Secondary entry point that jumps straight into the kernel. Only to
	 * be used where CPUs are brought online dynamically by the kernel.
	 */
SYM_FUNC_START(secondary_entry)	// Power State Coordination Interface,功耗节能热插拔模块调用此函数启动cpu
	mov	x0, xzr
	bl	init_kernel_el			// w0=cpu_boot_mode
	b	secondary_startup
SYM_FUNC_END(secondary_entry)

SYM_FUNC_START_LOCAL(secondary_startup)
	/*
	 * Common entry point for secondary CPUs.
	 */
	mov	x20, x0				// preserve boot mode

#ifdef CONFIG_ARM64_VA_BITS_52
alternative_if ARM64_HAS_VA52
	bl	__cpu_secondary_check52bitva	// 检查是否支持52位虚地址
alternative_else_nop_endif
#endif

	bl	__cpu_setup			// initialise processor
	adrp	x1, swapper_pg_dir
	adrp	x2, idmap_pg_dir
	bl	__enable_mmu
	ldr	x8, =__secondary_switched
	br	x8
SYM_FUNC_END(secondary_startup)

	.text
SYM_FUNC_START_LOCAL(__secondary_switched)
	mov	x0, x20
	bl	set_cpu_boot_mode_flag			// 设置cpu boot flag

	mov	x0, x20
	bl	finalise_el2

	str_l	xzr, __early_cpu_boot_status, x3
	adr_l	x5, vectors				// 设置中断向量表
	msr	vbar_el1, x5
	isb

	adr_l	x0, secondary_data
	ldr	x2, [x0, #CPU_BOOT_TASK]
	cbz	x2, __secondary_too_slow

	init_cpu_task x2, x1, x3

#ifdef CONFIG_ARM64_PTR_AUTH
	ptrauth_keys_init_cpu x2, x3, x4, x5
#endif

	bl	secondary_start_kernel
	ASM_BUG()
SYM_FUNC_END(__secondary_switched)

SYM_FUNC_START_LOCAL(__secondary_too_slow)
	wfe
	wfi
	b	__secondary_too_slow
SYM_FUNC_END(__secondary_too_slow)

/*
 * Sets the __boot_cpu_mode flag depending on the CPU boot mode passed
 * in w0. See arch/arm64/include/asm/virt.h for more info.
 */
SYM_FUNC_START_LOCAL(set_cpu_boot_mode_flag)
	adr_l	x1, __boot_cpu_mode
	cmp	w0, #BOOT_CPU_MODE_EL2
	b.ne	1f
	add	x1, x1, #4
1:	str	w0, [x1]			// Save CPU boot mode
	ret
SYM_FUNC_END(set_cpu_boot_mode_flag)

/*
 * The booting CPU updates the failed status @__early_cpu_boot_status,
 * with MMU turned off.
 *
 * update_early_cpu_boot_status tmp, status
 *  - Corrupts tmp1, tmp2
 *  - Writes 'status' to __early_cpu_boot_status and makes sure
 *    it is committed to memory.
 */
// cpu boot失败，记录失败原因到变量
	.macro	update_early_cpu_boot_status status, tmp1, tmp2
	mov	\tmp2, #\status
	adr_l	\tmp1, __early_cpu_boot_status
	str	\tmp2, [\tmp1]
	dmb	sy
	dc	ivac, \tmp1			// Invalidate potentially stale cache line
	.endm

/*
 * Enable the MMU.
 *
 *  x0  = SCTLR_EL1 value for turning on the MMU.
 *  x1  = TTBR1_EL1 value
 *  x2  = ID map root table address
 *
 * Returns to the caller via x30/lr. This requires the caller to be covered
 * by the .idmap.text section.
 *
 * Checks if the selected granule size is supported by the CPU.
 * If it isn't, park the CPU
 */
	.section ".idmap.text","a"
SYM_FUNC_START(__enable_mmu)
	mrs	x3, ID_AA64MMFR0_EL1
	ubfx	x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4	// 提取最小页粒度
	cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
	b.lt    __no_granule_support						// CPU支持的页面粒度小于代码支持的最小页面粒度, boot failed
	cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX	// 超过CPU支持的最大粒度, failed
	b.gt    __no_granule_support
	phys_to_ttbr x2, x2
	msr	ttbr0_el1, x2			// load TTBR0
	load_ttbr1 x1, x1, x3

	set_sctlr_el1	x0			// x0寄存器保存了写系统控制寄存器的内容, mmu on已经打开

	ret
SYM_FUNC_END(__enable_mmu)

#ifdef CONFIG_ARM64_VA_BITS_52
SYM_FUNC_START(__cpu_secondary_check52bitva)	// 对比cpu0初始化时的VA地址是否支持
#ifndef CONFIG_ARM64_LPA2
	mrs_s	x0, SYS_ID_AA64MMFR2_EL1
	and	x0, x0, ID_AA64MMFR2_EL1_VARange_MASK
	cbnz	x0, 2f
#else
	mrs	x0, id_aa64mmfr0_el1
	sbfx	x0, x0, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
	cmp	x0, #ID_AA64MMFR0_EL1_TGRAN_LPA2
	b.ge	2f
#endif

	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_52_BIT_VA, x0, x1
1:	wfe
	wfi
	b	1b

2:	ret
SYM_FUNC_END(__cpu_secondary_check52bitva)
#endif

SYM_FUNC_START_LOCAL(__no_granule_support)
	/* Indicate that this CPU can't boot and is stuck in the kernel */
	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_NO_GRAN, x1, x2
1:
	wfe
	wfi
	b	1b
SYM_FUNC_END(__no_granule_support)

SYM_FUNC_START_LOCAL(__primary_switch)
	adrp	x1, reserved_pg_dir			// 保留页表, 用于内核访问user space, SW PAN功能，内核把ttbr0设置为这个页表
	adrp	x2, __pi_init_idmap_pg_dir		// 全局页表 identity mapping
	bl	__enable_mmu					// 设置页表寄存器, 把x0写入系统控制寄存器, mmu on已打开

	adrp	x1, early_init_stack
	mov	sp, x1
	mov	x29, xzr
	mov	x0, x20				// pass the full boot status
	mov	x1, x21				// pass the FDT
	bl	__pi_early_map_kernel		// Map and relocate the kernel

	ldr	x8, =__primary_switched
	adrp	x0, KERNEL_START		// __pa(KERNEL_START)
	br	x8
SYM_FUNC_END(__primary_switch)
