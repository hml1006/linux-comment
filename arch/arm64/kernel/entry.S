/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level exception handling code
 *
 * Copyright (C) 2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/linkage.h>

#include <asm/alternative.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/asm_pointer_auth.h>
#include <asm/bug.h>
#include <asm/cpufeature.h>
#include <asm/errno.h>
#include <asm/esr.h>
#include <asm/irq.h>
#include <asm/memory.h>
#include <asm/mmu.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/scs.h>
#include <asm/stacktrace/frame.h>
#include <asm/thread_info.h>
#include <asm/asm-uaccess.h>
#include <asm/unistd.h>

/**
当异常发生时，CPU会自动做如下一些事情。

将处理器状态寄存器PSTATE保存到对应目标异常等级的SPSR_ELx中
将返回地址保存在对应目标异常等级的ELRELx中
把PSTATE寄存器里的DAIF域都设置为1，这相当于把调试异常、系统错误 （SError）、IRQ以及FIQ都关闭了。具体异常原因需要查看ESR_ELx
设置栈指针，指向对应目标异常等级下的栈，自动切换SP到SP_ELx
CPU处理器会从异常发生现场的异常等级切换到对应目标异常等级，然后跳转到异常向量表并执行
上述是ARMv8处理器检测到异常发生后自动做的事情。操作系统需要做的事情是从中断向量表开始，根据异常发生的类型，跳转到合适的异常向量表。异常向量表中的每项会保存一个异常处理的跳转函数，然后跳转到恰当的异常处理函数并处理异常。

当操作系统的异常处理完成后，执行一条eret指令即可从异常返回。这条指令会自动完成如下工作

从ELR_ELx中恢复PC指针
从SPSR_ELx恢复处理器的状态
**/
	/*
	* 宏，把所有通用寄存器清0
	*/
	.macro	clear_gp_regs
	/*
	* irq伪指令，会生成32个mov指令清零所有寄存器
	*/
	.irp	n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
	mov	x\n, xzr
	.endr
	.endm

	/*
	* 定义中断向量entry，一个entry可以达到128字节，
	* el 异常级别，regsize 寄存器大小 label 跳转入口地址
	*/
	.macro kernel_ventry, el:req, ht:req, regsize:req, label:req
	/* 2^7 对齐，align伪指令在x86和arm不一样 */
	.align 7
.Lventry_start\@:
	.if	\el == 0	// 中断来自user space
	/*
	 * This must be the first instruction of the EL0 vector entries. It is
	 * skipped by the trampoline vectors, to trigger the cleanup.
	 */
	b	.Lskip_tramp_vectors_cleanup\@	// 从tramp_vector跳转过来时, 这条指令会被跳过, x30 LR寄存器地址被设置为了下一条指令
	.if	\regsize == 64
	/*
	* tramp_entry中会把x30备份到tpidrro_el0, 此处恢复x30, x30是LR寄存器
	*/
	mrs	x30, tpidrro_el0
	msr	tpidrro_el0, xzr
	.else
	mov	x30, xzr
	.endif
.Lskip_tramp_vectors_cleanup\@:
	.endif

	/*分配需要保存的现场寄存器栈空间，栈必须对齐 THREAD_SHIFT*/
	sub	sp, sp, #PT_REGS_SIZE
	/*
	 * Test whether the SP has overflowed, without corrupting a GPR.
	 * Task and IRQ stacks are aligned so that SP & (1 << THREAD_SHIFT)
	 * should always be zero.
	 */
	// vmap stack 分配栈，在栈的起始会分配一个保护页，stack是32K对齐，目的是 SP & (1 << THREAD_SHIFT) 这种简单方法检查溢出
	// 保护页不映射物理内存，但是占用虚地址空间，如果踩到保护页会产生内存错误，进入本中断向量进行检查处理
	add	sp, sp, x0			// sp' = sp + x0
	sub	x0, sp, x0			// x0' = sp' - x0 = (sp + x0) - x0 = sp
	tbnz	x0, #THREAD_SHIFT, 0f	// ==THREAD_SHIFT==14, 当sp溢出时,THREAD_SHIFT bit为1,如果没有溢出,这个bit为0
	sub	x0, sp, x0			// x0'' = sp' - x0' = (sp + x0) - sp = x0
	sub	sp, sp, x0			// sp'' = sp' - x0 = (sp + x0) - x0 = sp

	b	el\el\ht\()_\regsize\()_\label	// entry_handler

0:
	/*
	 * Either we've just detected an overflow, or we've taken an exception
	 * while on the overflow stack. Either way, we won't return to
	 * userspace, and can clobber EL0 registers to free up GPRs.
	 */

	/* Stash the original SP (minus PT_REGS_SIZE) in tpidr_el0. */
	msr	tpidr_el0, x0			// x0备份到tpidr_el0, 此时x0的值等于sp初始值

	/* Recover the original x0 value and stash it in tpidrro_el0 */
	sub	x0, sp, x0				// 恢复x0的原始值
	msr	tpidrro_el0, x0			// x0备份到tpidrro_el0

	/* Switch to the overflow stack */
	adr_this_cpu sp, overflow_stack + OVERFLOW_STACK_SIZE, x0	// 取per-cpu over_flow_stack栈地址,设置到sp，主栈满了，切到备用栈

	/*
	 * Check whether we were already on the overflow stack. This may happen
	 * after panic() re-enables interrupts.
	 */
	mrs	x0, tpidr_el0			// sp of interrupted context
	sub	x0, sp, x0			// delta with top of overflow stack, 计算主栈和overflow stack的地址偏移
	tst	x0, #~(OVERFLOW_STACK_SIZE - 1)	// within range?  判断interrupted sp和overflow_stack sp的偏移是否在overflow stack size内
										// 为0则说明异常发生时的sp在overflow stack地址范围，正在处理panic时发生中断，否则说明某些代码触发了stack Overflow，
										// 接下来处理panic
	b.ne	__bad_stack			// no? -> bad stack pointer 跳转处理stack Overflow流程
									// yes? -> continue

	/* We were already on the overflow stack. Restore sp/x0 and carry on. */
	sub	sp, sp, x0
	mrs	x0, tpidrro_el0
	b	el\el\ht\()_\regsize\()_\label
.org .Lventry_start\@ + 128	// Did we overflow the ventry slot?
	.endm

	.macro	tramp_alias, dst, sym
	.set	.Lalias\@, TRAMP_VALIAS + \sym - .entry.tramp.text
	movz	\dst, :abs_g2_s:.Lalias\@
	movk	\dst, :abs_g1_nc:.Lalias\@
	movk	\dst, :abs_g0_nc:.Lalias\@
	.endm

	/*
	 * This macro corrupts x0-x3. It is the caller's duty  to save/restore
	 * them if required.
	 */
	.macro	apply_ssbd, state, tmp1, tmp2
alternative_cb	ARM64_ALWAYS_SYSTEM, spectre_v4_patch_fw_mitigation_enable
	b	.L__asm_ssbd_skip\@		// Patched to NOP
alternative_cb_end
	ldr_this_cpu	\tmp2, arm64_ssbd_callback_required, \tmp1
	cbz	\tmp2,	.L__asm_ssbd_skip\@
	ldr	\tmp2, [tsk, #TSK_TI_FLAGS]
	tbnz	\tmp2, #TIF_SSBD, .L__asm_ssbd_skip\@
	mov	w0, #ARM_SMCCC_ARCH_WORKAROUND_2
	mov	w1, #\state
alternative_cb	ARM64_ALWAYS_SYSTEM, smccc_patch_fw_mitigation_conduit
	nop					// Patched to SMC/HVC #0
alternative_cb_end
.L__asm_ssbd_skip\@:
	.endm

	/* Check for MTE asynchronous tag check faults */
	.macro check_mte_async_tcf, tmp, ti_flags, thread_sctlr
#ifdef CONFIG_ARM64_MTE
	.arch_extension lse
alternative_if_not ARM64_MTE
	b	1f
alternative_else_nop_endif
	/*
	 * Asynchronous tag check faults are only possible in ASYNC (2) or
	 * ASYM (3) modes. In each of these modes bit 1 of SCTLR_EL1.TCF0 is
	 * set, so skip the check if it is unset.
	 */
	tbz	\thread_sctlr, #(SCTLR_EL1_TCF0_SHIFT + 1), 1f
	mrs_s	\tmp, SYS_TFSRE0_EL1
	tbz	\tmp, #SYS_TFSR_EL1_TF0_SHIFT, 1f
	/* Asynchronous TCF occurred for TTBR0 access, set the TI flag */
	mov	\tmp, #_TIF_MTE_ASYNC_FAULT
	add	\ti_flags, tsk, #TSK_TI_FLAGS
	stset	\tmp, [\ti_flags]
1:
#endif
	.endm

	/* Clear the MTE asynchronous tag check faults */
	.macro clear_mte_async_tcf thread_sctlr
#ifdef CONFIG_ARM64_MTE
alternative_if ARM64_MTE
	/* See comment in check_mte_async_tcf above. */
	tbz	\thread_sctlr, #(SCTLR_EL1_TCF0_SHIFT + 1), 1f
	dsb	ish
	msr_s	SYS_TFSRE0_EL1, xzr
1:
alternative_else_nop_endif
#endif
	.endm

	.macro mte_set_gcr, mte_ctrl, tmp
#ifdef CONFIG_ARM64_MTE
	ubfx	\tmp, \mte_ctrl, #MTE_CTRL_GCR_USER_EXCL_SHIFT, #16
	orr	\tmp, \tmp, #SYS_GCR_EL1_RRND
	msr_s	SYS_GCR_EL1, \tmp
#endif
	.endm

	.macro mte_set_kernel_gcr, tmp, tmp2
#ifdef CONFIG_KASAN_HW_TAGS
alternative_cb	ARM64_ALWAYS_SYSTEM, kasan_hw_tags_enable
	b	1f
alternative_cb_end
	mov	\tmp, KERNEL_GCR_EL1
	msr_s	SYS_GCR_EL1, \tmp
1:
#endif
	.endm

	.macro mte_set_user_gcr, tsk, tmp, tmp2
#ifdef CONFIG_KASAN_HW_TAGS
alternative_cb	ARM64_ALWAYS_SYSTEM, kasan_hw_tags_enable
	b	1f
alternative_cb_end
	ldr	\tmp, [\tsk, #THREAD_MTE_CTRL]

	mte_set_gcr \tmp, \tmp2
1:
#endif
	.endm

	.macro	kernel_entry, el, regsize = 64
	.if	\el == 0
	alternative_insn nop, SET_PSTATE_DIT(1), ARM64_HAS_DIT
	.endif
	.if	\regsize == 32
	mov	w0, w0				// zero upper 32 bits of x0
	.endif
	// 保存通用寄存器
	stp	x0, x1, [sp, #16 * 0]
	stp	x2, x3, [sp, #16 * 1]
	stp	x4, x5, [sp, #16 * 2]
	stp	x6, x7, [sp, #16 * 3]
	stp	x8, x9, [sp, #16 * 4]
	stp	x10, x11, [sp, #16 * 5]
	stp	x12, x13, [sp, #16 * 6]
	stp	x14, x15, [sp, #16 * 7]
	stp	x16, x17, [sp, #16 * 8]
	stp	x18, x19, [sp, #16 * 9]
	stp	x20, x21, [sp, #16 * 10]
	stp	x22, x23, [sp, #16 * 11]
	stp	x24, x25, [sp, #16 * 12]
	stp	x26, x27, [sp, #16 * 13]
	stp	x28, x29, [sp, #16 * 14]

	.if	\el == 0
	// 清空通用寄存器
	clear_gp_regs
	// el0 sp寄存器
	mrs	x21, sp_el0
	/**
	arm64 从 EL0 切换进 EL1 的时候保存完上下文之后 sp_el0 就用不到了，所以可以把当前进程的指针保存在 sp_el0 里面快速访问，
	然后在返回用户态的时候将 sp_el0 保存进 percpu 存储区的 __entry_task，而 percpu 存储区的偏移存在 tpidr_el1 里面，
	那么下次进入 EL1 的时候就可以用 __entry_task + tpidr_el1 又找到当前 CPU 上的 current，然后存进 sp_el0 里访问
	**/
	// 每次任务切换时，将next task放 __entry_task，切换完成后，__entry_task放的就是current task
	// 用户态到内核态时，把current task放sp el0
	// tsk .req x28, 所以tsk是x28寄存器，放current thread info
	ldr_this_cpu	tsk, __entry_task, x20	// 从 __entry_task 获取当前task指针
	msr	sp_el0, tsk	//sp_el0执行current task

	/*
	 * Ensure MDSCR_EL1.SS is clear, since we can unmask debug exceptions
	 * when scheduling.
	 */
	ldr	x19, [tsk, #TSK_TI_FLAGS]	// task_struct 中thread_info.flags
	disable_step_tsk x19, x20		// 关闭单步调试

	/* Check for asynchronous tag check faults in user space */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	check_mte_async_tcf x22, x23, x0	// memory tagged extension功能检查

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * Enable IA for in-kernel PAC if the task had it disabled. Although
	 * this could be implemented with an unconditional MRS which would avoid
	 * a load, this was measured to be slower on Cortex-A75 and Cortex-A76.
	 *
	 * Install the kernel IA key only if IA was enabled in the task. If IA
	 * was disabled on kernel exit then we would have left the kernel IA
	 * installed so there is no need to install it again.
	 */
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f	// 检查指针认证功能,内核在每个进程execv时分配一个key，这个key随task一起切换
	__ptrauth_keys_install_kernel_nosync tsk, x20, x22, x23	// 给task开启 PA功能
	b	2f
1:
	mrs	x0, sctlr_el1
	orr	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0			// 开启PA
2:
alternative_else_nop_endif
#endif

	/**
	apply_ssbd用于启用 Speculative Store Bypass Disable（SSBD）特性。
	该函数在 Linux 内核中实现，主要用于 Spectre 漏洞的缓解。
	SSBD 特性是 ARMv8.5-A 架构中引入的一种硬件特性，用于防止 Spectre 漏洞的攻击。
	当启用 SSBD 特性时，处理器会禁用对 Store Buffer 的 speculative access，
	从而防止 Spectre 攻击利用 Store Buffer 的 speculative access 进行信息泄露。
	在 Linux 系统中，启用 SSBD 特性可以通过设置相应的控制寄存器实现。
	**/
	apply_ssbd 1, x22, x23

	mte_set_kernel_gcr x22, x23	// 设置Global Control Register启用mte

	/*
	 * Any non-self-synchronizing system register updates required for
	 * kernel entry should be placed before this point.
	 */
alternative_if ARM64_MTE
	isb
	b	1f
alternative_else_nop_endif
alternative_if ARM64_HAS_ADDRESS_AUTH
	isb
alternative_else_nop_endif
1:
	// shadow call stack功能，防ROP攻击
	scs_load_current_base
	.else
	add	x21, sp, #PT_REGS_SIZE
	get_current_task tsk	// 取current， sp_el0
	.endif /* \el == 0 */
	mrs	x22, elr_el1
	mrs	x23, spsr_el1
	stp	lr, x21, [sp, #S_LR]

	/*
	 * Create a metadata frame record. The unwinder will use this to
	 * identify and unwind exception boundaries.
	 */
	stp	xzr, xzr, [sp, #S_STACKFRAME]
	.if \el == 0
	mov	x0, #FRAME_META_TYPE_FINAL
	.else
	mov	x0, #FRAME_META_TYPE_PT_REGS
	.endif
	str	x0, [sp, #S_STACKFRAME_TYPE]
	add	x29, sp, #S_STACKFRAME

// PAN 功能，禁止内核访问用户空间数据，必须使用copy_from_user, copy_to_user才能访问
// 实现原理是ttbr0寄存器置零, 这样内核就不能访问页表了, 这个是软件模拟实现，不是armv8 硬件PAN功能
#ifdef CONFIG_ARM64_SW_TTBR0_PAN
alternative_if_not ARM64_HAS_PAN
	bl	__swpan_entry_el\el
alternative_else_nop_endif
#endif

	stp	x22, x23, [sp, #S_PC]

	/* Not in a syscall by default (el0_svc overwrites for real syscall) */
	.if	\el == 0
	mov	w21, #NO_SYSCALL
	str	w21, [sp, #S_SYSCALLNO]
	.endif

#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_save\@
alternative_else_nop_endif

	mrs_s	x20, SYS_ICC_PMR_EL1
	str	w20, [sp, #S_PMR]
	mov	x20, #GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET
	msr_s	SYS_ICC_PMR_EL1, x20

.Lskip_pmr_save\@:
#endif

	/*
	 * Registers that may be useful after this macro is invoked:
	 *
	 * x20 - ICC_PMR_EL1
	 * x21 - aborted SP
	 * x22 - aborted PC
	 * x23 - aborted PSTATE
	*/
	.endm

	.macro	kernel_exit, el
	.if	\el != 0
	disable_daif	// 退出到el1需禁用 DAIF
	.endif

#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_restore\@
alternative_else_nop_endif

	ldr	w20, [sp, #S_PMR]
	msr_s	SYS_ICC_PMR_EL1, x20

	/* Ensure priority change is seen by redistributor */
alternative_if_not ARM64_HAS_GIC_PRIO_RELAXED_SYNC
	dsb	sy
alternative_else_nop_endif

.Lskip_pmr_restore\@:
#endif

	ldp	x21, x22, [sp, #S_PC]		// load ELR, SPSR

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
alternative_if_not ARM64_HAS_PAN
	bl	__swpan_exit_el\el		// 恢复ttbr0_el1
alternative_else_nop_endif
#endif

	.if	\el == 0
	ldr	x23, [sp, #S_SP]		// load return stack pointer
	msr	sp_el0, x23
	tst	x22, #PSR_MODE32_BIT		// native task?
	b.eq	3f

#ifdef CONFIG_ARM64_ERRATUM_845719
alternative_if ARM64_WORKAROUND_845719
#ifdef CONFIG_PID_IN_CONTEXTIDR
	mrs	x29, contextidr_el1
	msr	contextidr_el1, x29
#else
	msr contextidr_el1, xzr
#endif
alternative_else_nop_endif
#endif
3:
	/* Ignore asynchronous tag check faults in the uaccess routines */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	clear_mte_async_tcf x0

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * IA was enabled for in-kernel PAC. Disable it now if needed, or
	 * alternatively install the user's IA. All other per-task keys and
	 * SCTLR bits were updated on task switch.
	 *
	 * No kernel C function calls after this.
	 */
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	__ptrauth_keys_install_user tsk, x0, x1, x2
	b	2f
1:
	mrs	x0, sctlr_el1
	bic	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

	mte_set_user_gcr tsk, x0, x1

	apply_ssbd 0, x0, x1
	.endif

	msr	elr_el1, x21			// set up the return data
	msr	spsr_el1, x22
	ldp	x0, x1, [sp, #16 * 0]
	ldp	x2, x3, [sp, #16 * 1]
	ldp	x4, x5, [sp, #16 * 2]
	ldp	x6, x7, [sp, #16 * 3]
	ldp	x8, x9, [sp, #16 * 4]
	ldp	x10, x11, [sp, #16 * 5]
	ldp	x12, x13, [sp, #16 * 6]
	ldp	x14, x15, [sp, #16 * 7]
	ldp	x16, x17, [sp, #16 * 8]
	ldp	x18, x19, [sp, #16 * 9]
	ldp	x20, x21, [sp, #16 * 10]
	ldp	x22, x23, [sp, #16 * 11]
	ldp	x24, x25, [sp, #16 * 12]
	ldp	x26, x27, [sp, #16 * 13]
	ldp	x28, x29, [sp, #16 * 14]

	.if	\el == 0
#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	alternative_insn "b .L_skip_tramp_exit_\@", nop, ARM64_UNMAP_KERNEL_AT_EL0

	msr	far_el1, x29

	ldr_this_cpu	x30, this_cpu_vector, x29
	tramp_alias	x29, tramp_exit
	msr		vbar_el1, x30		// install vector table
	ldr		lr, [sp, #S_LR]		// restore x30
	add		sp, sp, #PT_REGS_SIZE	// restore sp
	br		x29

.L_skip_tramp_exit_\@:
#endif
	.endif

	ldr	lr, [sp, #S_LR]
	add	sp, sp, #PT_REGS_SIZE		// restore sp

	.if \el == 0
	/* This must be after the last explicit memory access */
alternative_if ARM64_WORKAROUND_SPECULATIVE_UNPRIV_LOAD
	tlbi	vale1, xzr
	dsb	nsh
alternative_else_nop_endif
	.else
	/* Ensure any device/NC reads complete */
	alternative_insn nop, "dmb sy", ARM64_WORKAROUND_1508412
	.endif

	eret
	sb
	.endm

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * Set the TTBR0 PAN bit in SPSR. When the exception is taken from
	 * EL0, there is no need to check the state of TTBR0_EL1 since
	 * accesses are always enabled.
	 * Note that the meaning of this bit differs from the ARMv8.1 PAN
	 * feature as all TTBR0_EL1 accesses are disabled, not just those to
	 * user mappings.
	 */
	 // 进入内核disable用户空间页表寄存器
SYM_CODE_START_LOCAL(__swpan_entry_el1)
	mrs	x21, ttbr0_el1
	tst	x21, #TTBRx_EL1_ASID_MASK	// Check for the reserved ASID
	orr	x23, x23, #PSR_PAN_BIT		// Set the emulated PAN in the saved SPSR
	b.eq	1f				// TTBR0 access already disabled
	and	x23, x23, #~PSR_PAN_BIT		// Clear the emulated PAN in the saved SPSR
SYM_INNER_LABEL(__swpan_entry_el0, SYM_L_LOCAL)
	__uaccess_ttbr0_disable x21
1:	ret
SYM_CODE_END(__swpan_entry_el1)

	/*
	 * Restore access to TTBR0_EL1. If returning to EL0, no need for SPSR
	 * PAN bit checking.
	 */
	 // 软件模拟PAN是通过修改用户态页表阻止内核访问用户空间数据，退出内核态时需要恢复用户空间页表寄存器
SYM_CODE_START_LOCAL(__swpan_exit_el1)
	tbnz	x22, #22, 1f			// Skip re-enabling TTBR0 access if the PSR_PAN_BIT is set
	__uaccess_ttbr0_enable x0, x1
1:	and	x22, x22, #~PSR_PAN_BIT		// ARMv8.0 CPUs do not understand this bit
	ret
SYM_CODE_END(__swpan_exit_el1)

SYM_CODE_START_LOCAL(__swpan_exit_el0)
	__uaccess_ttbr0_enable x0, x1
	/*
	 * Enable errata workarounds only if returning to user. The only
	 * workaround currently required for TTBR0_EL1 changes are for the
	 * Cavium erratum 27456 (broadcast TLBI instructions may cause I-cache
	 * corruption).
	 */
	b	post_ttbr_update_workaround
SYM_CODE_END(__swpan_exit_el0)
#endif

/* GPRs used by entry code */
tsk	.req	x28		// current thread_info

	.text

/*
 * Exception vectors.
 */
	.pushsection ".entry.text", "ax"
/**
如果发生异常后并没有exception level切换，并且发生异常之前使用的栈指针是SP_EL0，那么使用第一组异常向量表。不知道有什么场景，linux没处理
如果发生异常后并没有exception level切换，并且发生异常之前使用的栈指针是SP_EL1/2/3，那么使用第二组异常向量表。异常发生在内核态
如果发生异常导致了exception level切换，并且发生异常之前的exception level运行在AARCH64模式，那么使用第三组异常向量表。系统调用
如果发生异常导致了exception level切换，并且发生异常之前的exception level运行在AARCH32模式，那么使用第四组异常向量表。用户态跑的aarch32程序，执行了系统调用
**/
/**
el1 sync：当前处于内核态时，发生了指令执行异常、缺页中断（跳转地址或者取地址）。
el1 irq：当前处于内核态时，发生硬件中断。
el0 sync：当前处于用户态时，发生了指令执行异常、缺页中断（跳转地址或者取地址）、系统调用。
el0 irq：当前处于用户态时，发生了硬件中断。
**/
/**
fiq 快速中断在secure world中使用, 即el3
**/
// h - handler, t - thread
	.align	11
SYM_CODE_START(vectors)
	kernel_ventry	1, t, 64, sync		// Synchronous EL1t
	kernel_ventry	1, t, 64, irq		// IRQ EL1t
	kernel_ventry	1, t, 64, fiq		// FIQ EL1t
	kernel_ventry	1, t, 64, error		// Error EL1t

	kernel_ventry	1, h, 64, sync		// Synchronous EL1h
	kernel_ventry	1, h, 64, irq		// IRQ EL1h
	kernel_ventry	1, h, 64, fiq		// FIQ EL1h
	kernel_ventry	1, h, 64, error		// Error EL1h

	kernel_ventry	0, t, 64, sync		// Synchronous 64-bit EL0
	kernel_ventry	0, t, 64, irq		// IRQ 64-bit EL0
	kernel_ventry	0, t, 64, fiq		// FIQ 64-bit EL0
	kernel_ventry	0, t, 64, error		// Error 64-bit EL0

	kernel_ventry	0, t, 32, sync		// Synchronous 32-bit EL0
	kernel_ventry	0, t, 32, irq		// IRQ 32-bit EL0
	kernel_ventry	0, t, 32, fiq		// FIQ 32-bit EL0
	kernel_ventry	0, t, 32, error		// Error 32-bit EL0
SYM_CODE_END(vectors)

SYM_CODE_START_LOCAL(__bad_stack)
	/*
	 * We detected an overflow in kernel_ventry, which switched to the
	 * overflow stack. Stash the exception regs, and head to our overflow
	 * handler.
	 */

	/* Restore the original x0 value */
	mrs	x0, tpidrro_el0

	/*
	 * Store the original GPRs to the new stack. The orginal SP (minus
	 * PT_REGS_SIZE) was stashed in tpidr_el0 by kernel_ventry.
	 */
	sub	sp, sp, #PT_REGS_SIZE
	kernel_entry 1	// 保存GPRs
	mrs	x0, tpidr_el0
	add	x0, x0, #PT_REGS_SIZE
	str	x0, [sp, #S_SP]	// 保存Overflow sp

	/* Stash the regs for handle_bad_stack */
	mov	x0, sp

	/* Time to die */
	bl	handle_bad_stack	// 处理bad stack， 栈回溯
	ASM_BUG()
SYM_CODE_END(__bad_stack)


	.macro entry_handler el:req, ht:req, regsize:req, label:req
SYM_CODE_START_LOCAL(el\el\ht\()_\regsize\()_\label)		// kernel_ventry会跳转到此处执行
	kernel_entry \el, \regsize								// 备份通用寄存器, 部分初始化操作
	mov	x0, sp
	bl	el\el\ht\()_\regsize\()_\label\()_handler			// 跳转到handler执行
	.if \el == 0	// 表示系统调用或者用户态触发的异常
	b	ret_to_user
	.else
	b	ret_to_kernel
	.endif
SYM_CODE_END(el\el\ht\()_\regsize\()_\label)
	.endm

/*
 * Early exception handlers
 */
 /**
 这些 macro 会展开为很多异常入口函数, kernel_ventry会跳转到此处
 **/
	entry_handler	1, t, 64, sync
	entry_handler	1, t, 64, irq
	entry_handler	1, t, 64, fiq
	entry_handler	1, t, 64, error

	entry_handler	1, h, 64, sync
	entry_handler	1, h, 64, irq
	entry_handler	1, h, 64, fiq
	entry_handler	1, h, 64, error

	entry_handler	0, t, 64, sync
	entry_handler	0, t, 64, irq
	entry_handler	0, t, 64, fiq
	entry_handler	0, t, 64, error

	entry_handler	0, t, 32, sync
	entry_handler	0, t, 32, irq
	entry_handler	0, t, 32, fiq
	entry_handler	0, t, 32, error

SYM_CODE_START_LOCAL(ret_to_kernel)
	kernel_exit 1
SYM_CODE_END(ret_to_kernel)

SYM_CODE_START_LOCAL(ret_to_user)
	ldr	x19, [tsk, #TSK_TI_FLAGS]	// re-check for single-step
	enable_step_tsk x19, x2			// 如果用户态单步调试，需要恢复
#ifdef CONFIG_KSTACK_ERASE
	bl	stackleak_erase_on_task_stack
#endif
	kernel_exit 0
SYM_CODE_END(ret_to_user)

	.popsection				// .entry.text

	// Move from tramp_pg_dir to swapper_pg_dir
	.macro tramp_map_kernel, tmp
	mrs	\tmp, ttbr1_el1						// 取出当前内核页表寄存器
	add	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET	// 偏移到swapper_pg_dir
	bic	\tmp, \tmp, #USER_ASID_FLAG			// 清掉user asid flag
	msr	ttbr1_el1, \tmp						// 重置到swapper_pg_dir
#ifdef CONFIG_QCOM_FALKOR_ERRATUM_1003
alternative_if ARM64_WORKAROUND_QCOM_FALKOR_E1003
	/* ASID already in \tmp[63:48] */
	movk	\tmp, #:abs_g2_nc:(TRAMP_VALIAS >> 12)
	movk	\tmp, #:abs_g1_nc:(TRAMP_VALIAS >> 12)
	/* 2MB boundary containing the vectors, so we nobble the walk cache */
	movk	\tmp, #:abs_g0_nc:((TRAMP_VALIAS & ~(SZ_2M - 1)) >> 12)
	isb
	tlbi	vae1, \tmp
	dsb	nsh
alternative_else_nop_endif
#endif /* CONFIG_QCOM_FALKOR_ERRATUM_1003 */
	.endm

	// Move from swapper_pg_dir to tramp_pg_dir
	.macro tramp_unmap_kernel, tmp
	mrs	\tmp, ttbr1_el1
	sub	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET
	orr	\tmp, \tmp, #USER_ASID_FLAG
	msr	ttbr1_el1, \tmp
	/*
	 * We avoid running the post_ttbr_update_workaround here because
	 * it's only needed by Cavium ThunderX, which requires KPTI to be
	 * disabled.
	 */
	.endm

	.macro		tramp_data_read_var	dst, var
#ifdef CONFIG_RELOCATABLE
	ldr		\dst, .L__tramp_data_\var			// 这个label保存了vectors中断向量表的地址
	.ifndef		.L__tramp_data_\var
	.pushsection	".entry.tramp.rodata", "a", %progbits
	.align		3
.L__tramp_data_\var:
	.quad		\var							// 声明一个64位变量, 值为vectors
	.popsection
	.endif
#else
	/*
	 * As !RELOCATABLE implies !RANDOMIZE_BASE the address is always a
	 * compile time constant (and hence not secret and not worth hiding).
	 *
	 * As statically allocated kernel code and data always live in the top
	 * 47 bits of the address space we can sign-extend bit 47 and avoid an
	 * instruction to load the upper 16 bits (which must be 0xFFFF).
	 */
	movz		\dst, :abs_g2_s:\var
	movk		\dst, :abs_g1_nc:\var
	movk		\dst, :abs_g0_nc:\var
#endif
	.endm

// Branch Target Buffer Hardening, 分支预测漏洞会导致缓存穿透
#define BHB_MITIGATION_NONE	0
#define BHB_MITIGATION_LOOP	1	// 循环中包含间接分支时,使用循环缓冲器缓解BHB漏洞
#define BHB_MITIGATION_FW	2	// firmware+hardware修复
#define BHB_MITIGATION_INSN	3	// 内核指令级修复

	.macro tramp_ventry, vector_start, regsize, kpti, bhb
	.align	7
1:
	.if	\regsize == 64
	msr	tpidrro_el0, x30	// Restored in kernel_ventry
	.endif

	.if	\bhb == BHB_MITIGATION_LOOP
	/*
	 * This sequence must appear before the first indirect branch. i.e. the
	 * ret out of tramp_ventry. It appears here because x30 is free.
	 */
	__mitigate_spectre_bhb_loop	x30
	.endif // \bhb == BHB_MITIGATION_LOOP

	.if	\bhb == BHB_MITIGATION_INSN
	clearbhb
	isb
	.endif // \bhb == BHB_MITIGATION_INSN

	.if	\kpti == 1
	/*
	 * Defend against branch aliasing attacks by pushing a dummy
	 * entry onto the return stack and using a RET instruction to
	 * enter the full-fat kernel vectors.
	 */
	bl	2f
	b	.							// 因为x30返回地址会被修改, 所以此处不会执行
2:
	tramp_map_kernel	x30			// 从tramp_pg_dir切换到swapper_pg_dir,cpu处于el0时内核页表是tramp_pg_dir
alternative_insn isb, nop, ARM64_WORKAROUND_QCOM_FALKOR_E1003
	tramp_data_read_var	x30, vectors	// 取中断向量表地址放入x30
alternative_if_not ARM64_WORKAROUND_CAVIUM_TX2_219_PRFM
	prfm	plil1strm, [x30, #(1b - \vector_start)]
alternative_else_nop_endif

	msr	vbar_el1, x30				// 把vectors地址放入中断向量表寄存器
	isb
	.else
	adr_l	x30, vectors
	.endif // \kpti == 1

	.if	\bhb == BHB_MITIGATION_FW
	/*
	 * The firmware sequence must appear before the first indirect branch.
	 * i.e. the ret out of tramp_ventry. But it also needs the stack to be
	 * mapped to save/restore the registers the SMC clobbers.
	 */
	__mitigate_spectre_bhb_fw
	.endif // \bhb == BHB_MITIGATION_FW

	add	x30, x30, #(1b - \vector_start + 4)		// x30存放当前entry第二条指令相对于generate_tramp_vector起始地址的偏移
	ret											// 返回到vectors中断向量表, x30地址, 并且会跳过第一条指令, 因为上面加4了
.org 1b + 128	// Did we overflow the ventry slot?
	.endm

	.macro	generate_tramp_vector,	kpti, bhb
.Lvector_start\@:
	.space	0x400					// 空出8个entry, 这段属于el1t, el1h, 只有el0t, el0t 32才需要,也就是el0切el1才需要tramp_vector

	.rept	4						// 此处是el0t的4个entry, repeat 4次
	tramp_ventry	.Lvector_start\@, 64, \kpti, \bhb
	.endr
	.rept	4						// 此处是el0t_32的4个entry
	tramp_ventry	.Lvector_start\@, 32, \kpti, \bhb
	.endr
	.endm

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * Exception vectors trampoline.
 * The order must match __bp_harden_el1_vectors and the
 * arm64_bp_harden_el1_vectors enum.
 */
	.pushsection ".entry.tramp.text", "ax"
	.align	11
SYM_CODE_START_LOCAL_NOALIGN(tramp_vectors)
#ifdef CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_LOOP
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_FW
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_INSN
#endif /* CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY */
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_NONE
SYM_CODE_END(tramp_vectors)

SYM_CODE_START_LOCAL(tramp_exit)
	tramp_unmap_kernel	x29
	mrs		x29, far_el1		// restore x29
	eret
	sb
SYM_CODE_END(tramp_exit)
	.popsection				// .entry.tramp.text
#endif /* CONFIG_UNMAP_KERNEL_AT_EL0 */

/*
 * Exception vectors for spectre mitigations on entry from EL1 when
 * kpti is not in use.
 */
 // 未开启内核页表隔离时的缓解幽灵漏洞方案, 对el0系统调用和异常做tramp
	.macro generate_el1_vector, bhb
.Lvector_start\@:
	kernel_ventry	1, t, 64, sync		// Synchronous EL1t
	kernel_ventry	1, t, 64, irq		// IRQ EL1t
	kernel_ventry	1, t, 64, fiq		// FIQ EL1h
	kernel_ventry	1, t, 64, error		// Error EL1t

	kernel_ventry	1, h, 64, sync		// Synchronous EL1h
	kernel_ventry	1, h, 64, irq		// IRQ EL1h
	kernel_ventry	1, h, 64, fiq		// FIQ EL1h
	kernel_ventry	1, h, 64, error		// Error EL1h

	.rept	4
	tramp_ventry	.Lvector_start\@, 64, 0, \bhb
	.endr
	.rept 4
	tramp_ventry	.Lvector_start\@, 32, 0, \bhb
	.endr
	.endm

/* The order must match tramp_vecs and the arm64_bp_harden_el1_vectors enum. */
	.pushsection ".entry.text", "ax"
	.align	11
SYM_CODE_START(__bp_harden_el1_vectors)	// 三张中断向量表
#ifdef CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY
	generate_el1_vector	bhb=BHB_MITIGATION_LOOP
	generate_el1_vector	bhb=BHB_MITIGATION_FW
	generate_el1_vector	bhb=BHB_MITIGATION_INSN
#endif /* CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY */
SYM_CODE_END(__bp_harden_el1_vectors)
	.popsection


/*
 * Register switch for AArch64. The callee-saved registers need to be saved
 * and restored. On entry:
 *   x0 = previous task_struct (must be preserved across the switch)
 *   x1 = next task_struct
 * Previous and next are guaranteed not to be the same.
 *
 */
SYM_FUNC_START(cpu_switch_to)
	save_and_disable_daif x11
	mov	x10, #THREAD_CPU_CONTEXT
	add	x8, x0, x10
	mov	x9, sp
	stp	x19, x20, [x8], #16		// store callee-saved registers
	stp	x21, x22, [x8], #16
	stp	x23, x24, [x8], #16
	stp	x25, x26, [x8], #16
	stp	x27, x28, [x8], #16
	stp	x29, x9, [x8], #16
	str	lr, [x8]
	add	x8, x1, x10
	ldp	x19, x20, [x8], #16		// restore callee-saved registers
	ldp	x21, x22, [x8], #16
	ldp	x23, x24, [x8], #16
	ldp	x25, x26, [x8], #16
	ldp	x27, x28, [x8], #16
	ldp	x29, x9, [x8], #16
	ldr	lr, [x8]
	mov	sp, x9
	msr	sp_el0, x1
	ptrauth_keys_install_kernel x1, x8, x9, x10
	scs_save x0
	scs_load_current
	restore_irq x11
	ret
SYM_FUNC_END(cpu_switch_to)
NOKPROBE(cpu_switch_to)

/*
 * This is how we return from a fork.
 */
SYM_CODE_START(ret_from_fork)
	bl	schedule_tail
	cbz	x19, 1f				// not a kernel thread
	mov	x0, x20
	blr	x19
1:	get_current_task tsk
	mov	x0, sp
	bl	asm_exit_to_user_mode
	b	ret_to_user
SYM_CODE_END(ret_from_fork)
NOKPROBE(ret_from_fork)

/*
 * void call_on_irq_stack(struct pt_regs *regs,
 * 		          void (*func)(struct pt_regs *));
 *
 * Calls func(regs) using this CPU's irq stack and shadow irq stack.
 */
SYM_FUNC_START(call_on_irq_stack)
	save_and_disable_daif x9
#ifdef CONFIG_SHADOW_CALL_STACK
	get_current_task x16
	scs_save x16
	ldr_this_cpu scs_sp, irq_shadow_call_stack_ptr, x17
#endif

	/* Create a frame record to save our LR and SP (implicit in FP) */
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp

	ldr_this_cpu x16, irq_stack_ptr, x17

	/* Move to the new stack and call the function there */
	add	sp, x16, #IRQ_STACK_SIZE
	restore_irq x9
	blr	x1

	save_and_disable_daif x9
	/*
	 * Restore the SP from the FP, and restore the FP and LR from the frame
	 * record.
	 */
	mov	sp, x29
	ldp	x29, x30, [sp], #16
	scs_load_current
	restore_irq x9
	ret
SYM_FUNC_END(call_on_irq_stack)
NOKPROBE(call_on_irq_stack)

#ifdef CONFIG_ARM_SDE_INTERFACE

#include <asm/sdei.h>
#include <uapi/linux/arm_sdei.h>

.macro sdei_handler_exit exit_mode
	/* On success, this call never returns... */
	cmp	\exit_mode, #SDEI_EXIT_SMC
	b.ne	99f
	smc	#0
	b	.
99:	hvc	#0
	b	.
.endm

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * The regular SDEI entry point may have been unmapped along with the rest of
 * the kernel. This trampoline restores the kernel mapping to make the x1 memory
 * argument accessible.
 *
 * This clobbers x4, __sdei_handler() will restore this from firmware's
 * copy.
 */
.pushsection ".entry.tramp.text", "ax"
SYM_CODE_START(__sdei_asm_entry_trampoline)
	mrs	x4, ttbr1_el1
	tbz	x4, #USER_ASID_BIT, 1f

	tramp_map_kernel tmp=x4
	isb
	mov	x4, xzr

	/*
	 * Remember whether to unmap the kernel on exit.
	 */
1:	str	x4, [x1, #(SDEI_EVENT_INTREGS + S_SDEI_TTBR1)]
	tramp_data_read_var     x4, __sdei_asm_handler
	br	x4
SYM_CODE_END(__sdei_asm_entry_trampoline)
NOKPROBE(__sdei_asm_entry_trampoline)

/*
 * Make the exit call and restore the original ttbr1_el1
 *
 * x0 & x1: setup for the exit API call
 * x2: exit_mode
 * x4: struct sdei_registered_event argument from registration time.
 */
SYM_CODE_START(__sdei_asm_exit_trampoline)
	ldr	x4, [x4, #(SDEI_EVENT_INTREGS + S_SDEI_TTBR1)]
	cbnz	x4, 1f

	tramp_unmap_kernel	tmp=x4

1:	sdei_handler_exit exit_mode=x2
SYM_CODE_END(__sdei_asm_exit_trampoline)
NOKPROBE(__sdei_asm_exit_trampoline)
.popsection		// .entry.tramp.text
#endif /* CONFIG_UNMAP_KERNEL_AT_EL0 */

/*
 * Software Delegated Exception entry point.
 *
 * x0: Event number
 * x1: struct sdei_registered_event argument from registration time.
 * x2: interrupted PC
 * x3: interrupted PSTATE
 * x4: maybe clobbered by the trampoline
 *
 * Firmware has preserved x0->x17 for us, we must save/restore the rest to
 * follow SMC-CC. We save (or retrieve) all the registers as the handler may
 * want them.
 */
SYM_CODE_START(__sdei_asm_handler)
	stp     x2, x3, [x1, #SDEI_EVENT_INTREGS + S_PC]
	stp     x4, x5, [x1, #SDEI_EVENT_INTREGS + 16 * 2]
	stp     x6, x7, [x1, #SDEI_EVENT_INTREGS + 16 * 3]
	stp     x8, x9, [x1, #SDEI_EVENT_INTREGS + 16 * 4]
	stp     x10, x11, [x1, #SDEI_EVENT_INTREGS + 16 * 5]
	stp     x12, x13, [x1, #SDEI_EVENT_INTREGS + 16 * 6]
	stp     x14, x15, [x1, #SDEI_EVENT_INTREGS + 16 * 7]
	stp     x16, x17, [x1, #SDEI_EVENT_INTREGS + 16 * 8]
	stp     x18, x19, [x1, #SDEI_EVENT_INTREGS + 16 * 9]
	stp     x20, x21, [x1, #SDEI_EVENT_INTREGS + 16 * 10]
	stp     x22, x23, [x1, #SDEI_EVENT_INTREGS + 16 * 11]
	stp     x24, x25, [x1, #SDEI_EVENT_INTREGS + 16 * 12]
	stp     x26, x27, [x1, #SDEI_EVENT_INTREGS + 16 * 13]
	stp     x28, x29, [x1, #SDEI_EVENT_INTREGS + 16 * 14]
	mov	x4, sp
	stp     lr, x4, [x1, #SDEI_EVENT_INTREGS + S_LR]

	mov	x19, x1

	/* Store the registered-event for crash_smp_send_stop() */
	ldrb	w4, [x19, #SDEI_EVENT_PRIORITY]
	cbnz	w4, 1f
	adr_this_cpu dst=x5, sym=sdei_active_normal_event, tmp=x6
	b	2f
1:	adr_this_cpu dst=x5, sym=sdei_active_critical_event, tmp=x6
2:	str	x19, [x5]

	/*
	 * entry.S may have been using sp as a scratch register, find whether
	 * this is a normal or critical event and switch to the appropriate
	 * stack for this CPU.
	 */
	cbnz	w4, 1f
	ldr_this_cpu dst=x5, sym=sdei_stack_normal_ptr, tmp=x6
	b	2f
1:	ldr_this_cpu dst=x5, sym=sdei_stack_critical_ptr, tmp=x6
2:	mov	x6, #SDEI_STACK_SIZE
	add	x5, x5, x6
	mov	sp, x5

#ifdef CONFIG_SHADOW_CALL_STACK
	/* Use a separate shadow call stack for normal and critical events */
	cbnz	w4, 3f
	ldr_this_cpu dst=scs_sp, sym=sdei_shadow_call_stack_normal_ptr, tmp=x6
	b	4f
3:	ldr_this_cpu dst=scs_sp, sym=sdei_shadow_call_stack_critical_ptr, tmp=x6
4:
#endif

	/*
	 * We may have interrupted userspace, or a guest, or exit-from or
	 * return-to either of these. We can't trust sp_el0, restore it.
	 */
	mrs	x28, sp_el0
	ldr_this_cpu	dst=x0, sym=__entry_task, tmp=x1
	msr	sp_el0, x0

	/* If we interrupted the kernel point to the previous stack/frame. */
	and     x0, x3, #0xc
	mrs     x1, CurrentEL
	cmp     x0, x1
	csel	x29, x29, xzr, eq	// fp, or zero
	csel	x4, x2, xzr, eq		// elr, or zero

	stp	x29, x4, [sp, #-16]!
	mov	x29, sp

	add	x0, x19, #SDEI_EVENT_INTREGS
	mov	x1, x19
	bl	__sdei_handler

	msr	sp_el0, x28
	/* restore regs >x17 that we clobbered */
	mov	x4, x19         // keep x4 for __sdei_asm_exit_trampoline
	ldp	x28, x29, [x4, #SDEI_EVENT_INTREGS + 16 * 14]
	ldp	x18, x19, [x4, #SDEI_EVENT_INTREGS + 16 * 9]
	ldp	lr, x1, [x4, #SDEI_EVENT_INTREGS + S_LR]
	mov	sp, x1

	mov	x1, x0			// address to complete_and_resume
	/* x0 = (x0 <= SDEI_EV_FAILED) ?
	 * EVENT_COMPLETE:EVENT_COMPLETE_AND_RESUME
	 */
	cmp	x0, #SDEI_EV_FAILED
	mov_q	x2, SDEI_1_0_FN_SDEI_EVENT_COMPLETE
	mov_q	x3, SDEI_1_0_FN_SDEI_EVENT_COMPLETE_AND_RESUME
	csel	x0, x2, x3, ls

	ldr_l	x2, sdei_exit_mode

	/* Clear the registered-event seen by crash_smp_send_stop() */
	ldrb	w3, [x4, #SDEI_EVENT_PRIORITY]
	cbnz	w3, 1f
	adr_this_cpu dst=x5, sym=sdei_active_normal_event, tmp=x6
	b	2f
1:	adr_this_cpu dst=x5, sym=sdei_active_critical_event, tmp=x6
2:	str	xzr, [x5]

alternative_if_not ARM64_UNMAP_KERNEL_AT_EL0
	sdei_handler_exit exit_mode=x2
alternative_else_nop_endif

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	tramp_alias	dst=x5, sym=__sdei_asm_exit_trampoline
	br	x5
#endif
SYM_CODE_END(__sdei_asm_handler)
NOKPROBE(__sdei_asm_handler)

SYM_CODE_START(__sdei_handler_abort)
	mov_q	x0, SDEI_1_0_FN_SDEI_EVENT_COMPLETE_AND_RESUME
	adr	x1, 1f
	ldr_l	x2, sdei_exit_mode
	sdei_handler_exit exit_mode=x2
	// exit the handler and jump to the next instruction.
	// Exit will stomp x0-x17, PSTATE, ELR_ELx, and SPSR_ELx.
1:	ret
SYM_CODE_END(__sdei_handler_abort)
NOKPROBE(__sdei_handler_abort)
#endif /* CONFIG_ARM_SDE_INTERFACE */
