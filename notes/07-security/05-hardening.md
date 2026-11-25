# 内核加固与漏洞缓解（以 ARM64 为主）

> 源码基线 Linux 7.2。相关配置分散在 `arch/arm64/Kconfig`、`arch/Kconfig`、`security/Kconfig.hardening`、`mm/Kconfig`，实现分布在 `arch/arm64/kernel/`（`kaslr.c`、`pi/kaslr_early.c`、`proton-pack.c`、`cpu_errata.c`、`entry.S`）、`arch/arm64/include/asm/`（`pointer_auth.h`、`stackprotector.h`、`scs.h`）、`mm/`（`mm_init.c`、`page_alloc.c`、`slub.c`、`usercopy.c`）、`security/lockdown/lockdown.c`。
>
> **配置前提**：本文所有“当前配置”结论来自本仓库 `.config`。注意该配置是刻意裁剪的调试配置：`CONFIG_SECURITY` 未启用，因此 Lockdown LSM 不存在；`CONFIG_RANDOMIZE_BASE` 显式关闭；`FORTIFY_SOURCE`、`HARDENED_USERCOPY`、堆 freelist 硬化、`INIT_ON_ALLOC/FREE` 均关闭。但 `ARM64_PTR_AUTH`/`ARM64_BTI`（用户态）、`STACKPROTECTOR_STRONG`、`STACKPROTECTOR_PER_TASK`、`UNMAP_KERNEL_AT_EL0`、`ARM64_EPAN`、`STRICT_KERNEL_RWX`、`MITIGATE_SPECTRE_BRANCH_HISTORY` 是打开的。**“配置关闭”不等于“内核不支持”**。

## 1. 当前 `.config` 基线

| 配置 | 当前值 | 作用 |
|------|--------|------|
| `CONFIG_ARM64_PTR_AUTH` | `y` | 指针认证（ARMv8.3），含用户态 |
| `CONFIG_ARM64_PTR_AUTH_KERNEL` | `y` | 内核态使用 PAC |
| `CONFIG_ARM64_BTI` | `y` | 用户态 BTI 支持 |
| `CONFIG_ARM64_BTI_KERNEL` | 未设置 | 内核代码不插入 BTI（`bti c`） |
| `CONFIG_CFI` | 未设置 | 无 kCFI 前向边检查 |
| `CONFIG_RANDSTRUCT_NONE` | `y` | 不做结构体布局随机化 |
| `CONFIG_STACKPROTECTOR` / `_STRONG` / `_PER_TASK` | 均 `y` | 每任务栈 canary |
| `CONFIG_SHADOW_CALL_STACK` | 未设置 | 无影子调用栈 |
| `CONFIG_RANDOMIZE_BASE` | **未设置** | KASLR 关闭 |
| `CONFIG_UNMAP_KERNEL_AT_EL0` | `y` | KPTI（EL0 时卸载内核页表） |
| `CONFIG_ARM64_EPAN` | `y` | Enhanced PAN |
| `CONFIG_STRICT_KERNEL_RWX` | `y` | text/rodata 只读、NX |
| `CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY` | `y` | Spectre-BHB 缓解 |
| `CONFIG_INIT_ON_ALLOC_DEFAULT_ON` / `_FREE_` | 未设置 | 分配/释放不自动清零 |
| `CONFIG_ZERO_CALL_USED_REGS` | 未设置 | 函数返回不清零寄存器 |
| `CONFIG_FORTIFY_SOURCE` | 未设置 | 无 str/mem 边界检查 |
| `CONFIG_HARDENED_USERCOPY` | 未设置 | 无 usercopy 范围校验 |
| `CONFIG_SLAB_FREELIST_RANDOM` / `_HARDENED` | 未设置 | freelist 不随机/不加密 |
| `CONFIG_KMALLOC_PARTITION_CACHES` | 未设置 | kmalloc cache 不分区 |
| `CONFIG_SECURITY` | 未设置 | Lockdown LSM 不可用 |
| `CONFIG_KASAN/KCSAN/KMSAN/UBSAN/KFENCE` | 未设置 | 无动态检测器（仅 `SLUB_DEBUG=y`） |

## 2. 指针认证（PAC）

### 2.1 硬件与配置

PAC（Pointer Authentication，ARMv8.3）用 128 位密钥对指针的高位做签名（`PACIA`/`PACIB`/`AUTIA`/`AUTIB` 等），返回地址、函数指针、数据指针在栈/堆上被篡改后无法通过认证，从而缓解 ROP/JOP。

- `CONFIG_ARM64_PTR_AUTH`：为**用户态**启用（每个进程 exec 时初始化密钥，随上下文切换）；
- `CONFIG_ARM64_PTR_AUTH_KERNEL`：为**内核态**启用，依赖 `ARM64_PTR_AUTH`，且要求链接器支持 `.note.gnu.property`（`LD_IS_LLD || LD_VERSION >= 23301` 等条件见 `arch/arm64/Kconfig:1991`）。

### 2.2 数据结构与实现

`arch/arm64/include/asm/pointer_auth.h`：

```c
struct ptrauth_key { unsigned long lo, hi; };   /* 128 位密钥 */

struct ptrauth_keys_user {
	struct ptrauth_key apia, apib, apda, apdb, apga;
};

struct ptrauth_keys_kernel {
	struct ptrauth_key apia;      /* 内核只用 APIA 签返回地址 */
};
```

- `ptrauth_keys_init_kernel()`：`get_random_bytes(&keys->apia, sizeof(keys->apia))`，仅在 `system_supports_address_auth()` 为真时执行；
- `__ptrauth_key_install_nosync(APIA, v)`：写 `SYS_APIAKEYLO_EL1`/`SYS_APIAKEYHI_EL1`；
- `ptrauth_keys_switch_kernel()`：进程切换时把 `keys_kernel.apia` 装入系统寄存器；
- `ptrauth_keys_init_user()`：exec 时为 APIA/APIB/APDA/APDB（地址认证）和 APGA（通用认证）生成随机密钥，`ptrauth_keys_install_user()` 写入 EL1 寄存器；用户态可经 `prctl(PR_PAC_RESET_KEYS)` 重置（`ptrauth_prctl_reset_keys()`）；
- `ptrauth_enable()`：置 `SCTLR_ELx_ENIA|ENIB|ENDA|ENDB`，之后 `isb`。

`boot_init_stack_canary()`（`arch/arm64/include/asm/stackprotector.h`）在启动时同时调用 `ptrauth_thread_init_kernel(current)`/`ptrauth_thread_switch_kernel(current)`/`ptrauth_enable()`，即内核 PAC 密钥先于任何被签名的返回地址产生。

运行时检测：`system_supports_address_auth()`/`system_supports_generic_auth()`（`cpufeature.c`）。若引导 CPU 支持而某个后上线 CPU 不支持，该 CPU 会被 park（`ARM64_PTR_AUTH` help 中明确说明）。

### 2.3 用户态密钥生命周期与 prctl

`pointer_auth.h` 的注释明确了用户态密钥的语义：**每进程一套密钥，所有线程共享；fork 继承，exec 重新初始化**。相关宏构成完整生命周期：

- `ptrauth_thread_init_user()`：exec 时调 `ptrauth_keys_init_user()` 生成五把随机密钥，并 `ptrauth_set_enabled_keys(current, PR_PAC_ENABLED_KEYS_MASK, PR_PAC_ENABLED_KEYS_MASK)` 默认启用全部地址密钥；
- `ptrauth_thread_switch_user(tsk)`：上下文切换时 `ptrauth_keys_install_user()` 安装 APIB/APDA/APDB（地址认证）与 APGA（通用认证）；APIA 由内核侧单独管理；
- `ptrauth_suspend_exit()`：休眠唤醒后重新安装用户密钥。

用户态经 `prctl` 控制：`PR_PAC_RESET_KEYS`（`ptrauth_prctl_reset_keys()`）重置指定密钥，`PR_PAC_SET_ENABLED_KEYS`/`PR_PAC_GET_ENABLED_KEYS`（`ptrauth_set_enabled_keys()`/`ptrauth_get_enabled_keys()`）按位启停。PAC 占据的位宽由 `ptrauth_user_pac_mask()`（`GENMASK_ULL(54, vabits_actual)`）与 `ptrauth_kernel_pac_mask()`（`GENMASK_ULL(63, vabits_actual)`）描述——依赖 TBI（Top Byte Ignore）开启，否则 bits 63:56 也参与。

### 2.4 编译器开关

内核 PAC 的插入由 `arch/arm64/Makefile:81` 决定：

```make
ifeq ($(CONFIG_ARM64_BTI_KERNEL),y)
  KBUILD_CFLAGS += -mbranch-protection=pac-ret+bti
  KBUILD_RUSTFLAGS += -Zbranch-protection=bti,pac-ret
else ifeq ($(CONFIG_ARM64_PTR_AUTH_KERNEL),y)
  KBUILD_RUSTFLAGS += -Zbranch-protection=pac-ret
  ifeq ($(CONFIG_CC_HAS_BRANCH_PROT_PAC_RET),y)
    KBUILD_CFLAGS += -mbranch-protection=pac-ret
  else
    KBUILD_CFLAGS += -msign-return-address=non-leaf
  endif
else
  KBUILD_CFLAGS += $(call cc-option,-mbranch-protection=none)
endif
```

当前配置走第二分支：`-mbranch-protection=pac-ret`（旧 GCC 退回 `-msign-return-address=non-leaf`），即只对非叶函数的返回地址做 `pacia`/`autia`；Rust 代码同样启用。两个选项都关时显式传 `none`，避免工具链默认值带来意外。

## 3. BTI（分支目标识别）

BTI（ARMv8.5）要求间接跳转目标必须是 `BTI c`/`BTI j`/`BTI jc` 指令，否则触发异常，用于限制 `BR`/`BLR` 的可达位置，和 PAC 互补。

- `CONFIG_ARM64_BTI`（当前 `y`）：允许**用户态**二进制使用 BTI；未编译 BTI 的程序仍可运行，只是得不到保护；
- `CONFIG_ARM64_BTI_KERNEL`（当前未设置）：给内核代码插入 BTI 并在内核态强制；`arch/arm64/Kconfig:2109` 要求 `ARM64_PTR_AUTH_KERNEL`、编译器支持 `-mbranch-protection=pac-ret+bti`，且 GCC 有已知 bug 时禁止（`depends on !CC_IS_GCC`）。

代码中通过 `IS_ENABLED(CONFIG_ARM64_BTI_KERNEL)` 决定汇编入口是否放置 `bti c`（`arch/arm64/include/asm/assembler.h:786` 附近），ftrace 的 trampoline 也需要相应处理（`arch/arm64/kernel/ftrace.c:129/190`）。运行时能力检测在 `cpufeature.c:2918` 附近。

由于本配置 `ARM64_BTI_KERNEL` 未开，内核态没有 BTI 强制；这是配置选择，不是硬件或代码缺失。

## 4. kCFI（内核控制流完整性）

7.2 已把旧的 `CFI_CLANG` 迁移为统一 `CONFIG_CFI`（`arch/Kconfig:937`）：

```
config CFI
	bool "Use Kernel Control Flow Integrity (kCFI)"
	default CFI_CLANG
	depends on ARCH_SUPPORTS_CFI
	depends on $(cc-option,-fsanitize=kcfi)
```

编译期给每个函数类型生成 32 位 type hash，间接调用前比较目标函数的 hash（`-fsanitize=kcfi`）。运行时：

- `kernel/cfi.c` 维护 `__kcfi_traps` 段（`__start___kcfi_traps`/`__stop___kcfi_traps`），当间接调用命中 trap 时报告并终止；
- 模块加载时解析模块的 `__kcfi_traps` 段（`kernel/cfi.c:77` 起 `mod->kcfi_traps`/`kcfi_traps_end`），`is_trap()` 判断地址是否落在 trap 上；
- `CONFIG_CFI_ICALL_NORMALIZE_INTEGERS`（`arch/Kconfig:959`）：把同尺寸同符号性的整数类型归一化到同一 hash，是 Rust 支持的前提；
- `CONFIG_CFI_PERMISSIVE` 可把违规降级为警告。

当前 `.config` 中 `CONFIG_CFI` 未设置，`CONFIG_RANDSTRUCT_NONE=y`，因此既无前向边 CFI，也无结构体布局随机化。

失败处理在 `kernel/cfi.c:14` 的 `report_cfi_failure()`：

```c
if (target)
	pr_err("CFI failure at %pS (target: %pS; expected type: 0x%08x)\n",
	       (void *)addr, (void *)*target, type);
...
if (cfi_warn) {
	__warn(NULL, 0, (void *)addr, 0, regs, NULL);
	return BUG_TRAP_TYPE_WARN;
}
return BUG_TRAP_TYPE_BUG;
```

`cfi_warn` 由 `CONFIG_CFI_PERMISSIVE` 决定（`__ro_after_init`）：permissive 模式只 WARN 不 BUG，便于灰度上线。trap 地址用相对偏移编码（`trap_address(p) = p + *p`），`is_cfi_trap()` 先查内核 `__start___kcfi_traps` 段，再经 `__module_address()` 查模块的 `kcfi_traps`（由 `module_cfi_finalize()` 在模块加载时从 ELF `__kcfi_traps` section 填充）。BPF JIT 也接入 kCFI：`DEFINE_CFI_TYPE(cfi_bpf_hash, __bpf_prog_runX)` 为 `bpf_func_t`/`bpf_callback_t` 生成类型 hash，供 JIT 代码匹配。

## 5. 影子调用栈（SCS）

`CONFIG_SHADOW_CALL_STACK`（`arch/Kconfig:754`）用一块独立影子栈保存返回地址，普通栈被溢出改写也不影响返回地址。ARM64 通过 `x18` 保存影子栈指针：

- `arch/arm64/include/asm/scs.h` 的 `scs_load_current`/`scs_load_current_base` 宏在异常入口/上下文切换时加载 `current->scs_base`；
- `arch/arm64/kernel/pi/patch-scs.c` 在早期对不写 `x18` 的函数打补丁；
- 依赖 `DYNAMIC_FTRACE_WITH_ARGS`/`REGS` 或关闭函数图追踪。

当前未启用。注意 SCS 与 PAC 都保护返回地址，但机制不同：PAC 是密码学认证，SCS 是物理隔离副本。

`asm/scs.h` 的汇编宏展示了 x18 的装载/保存：

```asm
scs_sp	.req	x18

.macro scs_load_current
	get_current_task scs_sp
	ldr	scs_sp, [scs_sp, #TSK_TI_SCS_SP]
.endm

.macro scs_save tsk
	str	scs_sp, [\tsk, #TSK_TI_SCS_SP]
.endm
```

本树还有**动态 SCS**（`CONFIG_UNWIND_PATCH_PAC_INTO_SCS`）：`dynamic_scs_init()` 在早期检测 `__pi_dynamic_scs_is_enabled` 后启用 `dynamic_scs_enabled` 静态键，`__pi_scs_patch()` 解析 `eh_frame` 的 CIE/FDE，把原本 PAC 签名的返回地址路径补丁成 SCS  push/pop——即在不重编译的前提下按 unwind 信息把 PAC 转换为影子栈保护。

## 6. 栈保护（stack protector）

配置：`CONFIG_STACKPROTECTOR=y`、`CONFIG_STACKPROTECTOR_STRONG=y`、`CONFIG_STACKPROTECTOR_PER_TASK=y`（当前全部开启）。

ARM64 的 per-task canary 实现很特别：不用全局 `__stack_chk_guard`，而是把 canary 放在 `task_struct` 中，编译器通过 **sysreg `sp_el0` + 偏移**访问：

```make
# arch/arm64/Makefile:71
ifeq ($(CONFIG_STACKPROTECTOR_PER_TASK),y)
stack_protector_prepare: prepare0
	$(eval KBUILD_CFLAGS += -mstack-protector-guard=sysreg \
				-mstack-protector-guard-reg=sp_el0 \
				-mstack-protector-guard-offset=$(shell \
			awk '{if ($$2 == "TSK_STACK_CANARY") print $$3;}' \
				$(objtree)/include/generated/asm-offsets.h))
endif
```

`sp_el0` 在 ARM64 上保存 `current`（`arch/arm64/include/asm/current.h`），因此 `[sp_el0 + TSK_STACK_CANARY]` 就是当前任务的 canary。

初始化（`arch/arm64/include/asm/stackprotector.h`）：

```c
static __always_inline void boot_init_stack_canary(void)
{
	unsigned long canary = get_random_canary();
	current->stack_canary = canary;
	if (!IS_ENABLED(CONFIG_STACKPROTECTOR_PER_TASK))
		__stack_chk_guard = current->stack_canary;
	ptrauth_thread_init_kernel(current);
	...
}
```

每个任务在 fork 时获得独立 canary（`dup_task_struct`/`arch_dup_task_struct`），崩溃时不会因全局 canary 泄露而全系统失效。

canary 的生成与失败路径：

```c
// include/linux/stackprotector.h:23
static inline unsigned long get_random_canary(void)
{
	return get_random_long() & CANARY_MASK;
}
```

64 位下 `CANARY_MASK = 0xffffffffffffff00`——低字节恒为零，使 canary 被当作字符串时提前截断，增加溢出覆盖难度。校验失败走 `__stack_chk_fail()`（`kernel/panic.c:1199`）：`noinstr` 函数直接 `panic("stack-protector: Kernel stack is corrupted in: %pB", __builtin_return_address(0))`，即栈保护失败是**整机 panic** 而非仅杀进程，防止攻击者继续利用已破坏的栈。

## 7. KASLR

### 7.1 配置与实现

`CONFIG_RANDOMIZE_BASE`（`arch/arm64/Kconfig:2361`）select `RELOCATABLE`，**当前 .config 明确关闭**（`# CONFIG_RANDOMIZE_BASE is not set`），因此：

- `arch/arm64/kernel/kaslr.c` 的 `kaslr_init()` 不会被编译；
- `arch/arm64/include/asm/memory.h:249` 的 `kaslr_offset()` 仍存在，但值为 `(u64)&_text - KIMAGE_VADDR`，即 0；
- `kaslr_enabled()` 在非 KASLR 构建中返回 `false`（`#ifdef CONFIG_RANDOMIZE_BASE` 分支之外）。

启用时的流程：

1. **早期（位置无关代码，`arch/arm64/kernel/pi/kaslr_early.c`）**：从 FDT `/chosen/kaslr-seed` 读 64 位种子并清零属性（`get_kaslr_seed()`）；没有种子时尝试 `RNDR`（`__early_cpu_has_rndr()`/`__arm64_rndr()`）。若都不可用则返回 0（不随机化）。
2. 计算偏移：

```c
range = (VMALLOC_END - KIMAGE_VADDR) / 2;
return range / 2 + (((__uint128_t)range * seed) >> 64);
```

即把内核放在 `VMALLOC` 区中间一半，避开上下各 1/4，避免与其它分配冲突。
3. **后期（`kaslr.c` 的 `kaslr_init()`）**：`kaslr_disabled_cmdline()` 检查 `nokaslr`；若 `kaslr_offset() < MIN_KIMG_ALIGN`（`arch/arm64/include/asm/boot.h:18`，`SZ_2M`）则判定“缺少种子”，打印 `KASLR disabled due to lack of seed`；否则置 `__kaslr_is_enabled = true`。

早期代码的约束值得强调：`pi/kaslr_early.c` 文件头注释写明运行在“非常早期”，**不允许使用全局变量或任何依赖绝对寻址的东西**，因此整个文件只有 62 行、全部位置无关。种子读取与清零一气呵成：

```c
// pi/kaslr_early.c:21 get_kaslr_seed()
prop = fdt_getprop_w(fdt, node, seed_str, &len);
if (!prop || len != sizeof(u64))
	return 0;
ret = fdt64_to_cpu(*prop);
*prop = 0;                    /* 用完即清零 FDT 中的种子 */
return ret;
```

`kaslr_early_init()` 先查 `kaslr_disabled_cmdline()`（`nokaslr`），再取 FDT 种子，失败才尝试 `RNDR` 硬件随机数；两者皆无返回 0，内核不随机化。种子清零防止后续读取 FDT 的代码（或 crash dump）泄露它。

### 7.2 模块区域

`CONFIG_RANDOMIZE_MODULE_REGION_FULL`（`arch/arm64/Kconfig:2379`，依赖 `RANDOMIZE_BASE`）把模块区在覆盖内核的 2GB 窗口内随机化，降低模块地址泄露内核布局的风险。KASLR 关闭时该选项也不可用。

### 7.3 当前影响

KASLR 关闭意味着 `_text` 固定在 `KIMAGE_VADDR`，`kaslr_offset()` 恒为 0，`/proc/vmcore` 的 `KERNELOFFSET=0`（`vmcore_info.c:35`），调试友好但削弱了对地址依赖型漏洞的防护。

## 8. 内存布局保护

- `CONFIG_STRICT_KERNEL_RWX=y`：内核 text/rodata 映射为只读，非 text 区域 NX，`mark_rodata_ro()` 在启动末期收紧权限；配合 `__ro_after_init`（`include/linux/cache.h:60`，`__section(".data..ro_after_init")`）让初始化后不再修改的数据进入只读页。
- `CONFIG_UNMAP_KERNEL_AT_EL0=y`（KPTI）：用户态运行时页表切换到 `tramp_pg_dir`，内核页表被卸载；`arch/arm64/kernel/entry.S` 中 `tramp_vectors`/`tramp_map_kernel` 在异常入口把 `tramp_pg_dir` 换回 `swapper_pg_dir`（`entry.S:697`、`789`、`836`）。这是 Meltdown 类攻击的核心缓解。
- `CONFIG_ARM64_EPAN=y`：Enhanced Privileged Access Never，内核默认不能直接访问用户内存，必须走 `copy_from_user`/`copy_to_user`/`uaccess` 路径；比传统 PAN 更严格地覆盖 `ldtr/sttr` 等指令。
- `CONFIG_ARM64_SW_TTBR0_PAN` 未设置：硬件 PAN 可用时不需要软件模拟。

### 8.1 KPTI 的页表切换细节

KPTI 的核心是把 `tramp_pg_dir` 与 `swapper_pg_dir` 放在固定偏移上，用加减法切换（`entry.S:697`）：

```asm
.macro tramp_map_kernel, tmp
	mrs	\tmp, ttbr1_el1
	add	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET   // tramp → swapper
	bic	\tmp, \tmp, #USER_ASID_FLAG
	msr	ttbr1_el1, \tmp
.endm

.macro tramp_unmap_kernel, tmp
	mrs	\tmp, ttbr1_el1
	sub	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET   // swapper → tramp
	orr	\tmp, \tmp, #USER_ASID_FLAG
	msr	ttbr1_el1, \tmp
.endm
```

EL0 异常入口的 `tramp_ventry`（`entry.S:760`）先做 BHB 缓解（见第 9 节），再 `tramp_map_kernel` 换回完整内核页表，从 `.entry.tramp.rodata` 读出真正的 `vectors` 地址写入 `vbar_el1`，最后 `ret` 跳入完整向量。返回用户态时 `tramp_exit` 执行 `tramp_unmap_kernel`（`entry.S:846`）。trampoline 里还用一个技巧防御分支别名攻击：`bl 2f` 压入假返回地址后再 `ret` 进入内核向量，避免直接 `br` 被 BTB 注入。

`tramp_vectors`（`entry.S:836`）按 `kpti × bhb` 组合生成多份向量（LOOP/FW/INSN/NONE），运行时按 CPU 能力选择——这正是 `MITIGATE_SPECTRE_BRANCH_HISTORY=y` 在入口代码里的落点。

### 8.2 mark_rodata_ro 的实际范围

`arch/arm64/mm/mmu.c:1201` 的 `mark_rodata_ro()` 收紧三段映射：

```c
section_size = (unsigned long)__init_begin - (unsigned long)__start_rodata;
WRITE_ONCE(rodata_is_rw, false);
update_mapping_prot(__pa_symbol(__start_rodata), ..., PAGE_KERNEL_RO);
/* _text 到 _stext 之间也标只读 */
update_mapping_prot(__pa_symbol(_text), ..., PAGE_KERNEL_RO);
/* 线性映射中的 data/bss 同样只读 */
update_mapping_prot(__pa_symbol(__init_end),
		    (unsigned long)lm_alias(__init_end), ..., PAGE_KERNEL_RO);
```

注意第三段：内核 data/bss 在**线性映射**（`lm_alias`）里的别名也被标只读，堵住“经线性映射绕过 rodata 保护”的路径。`rodata_enabled` 还决定 `PAGE_KERNEL_ROX` 的使用（`mmu.c:1340`），即 text 映射为只读可执行而非可写可执行。

## 9. Spectre 类缓解

ARM64 的缓解集中在 `arch/arm64/kernel/proton-pack.c`（约 1100+ 行）与 `cpu_errata.c`，运行时通过 `arm64_cpu_capabilities` 匹配 MIDR，并通过 sysfs `mitigations/` 或命令行 `spectre_v2=`、`spectre_v4=`、`spectre_bhb=` 调整。

| 变体 | 机制 | 关键函数/文件 |
|------|------|---------------|
| v1（边界检查绕过） | `array_index_nospec()` 生成掩码，阻止推测执行越界索引 | `include/linux/nospec.h`、`arch/arm64/include/asm/barrier.h` 的 `array_index_mask_nospec()` |
| v2（BTI/分支目标注入） | 返回地址预测器隔离：`install_bp_hardening_cb()` 安装 SMC/HVC 固件调用或 `qcom_link_stack_sanitisation`，向量在 `entry.S` 的 `.entry.tramp.text` | `proton-pack.c:218` 起 |
| v3a（系统寄存器推测读） | `spectre_v3a_enable_mitigation()` 依据 CPU 能力置位 | `proton-pack.c:333` |
| v4 / SSB（存储绕过） | `spectre_v4_enable_mitigation()` 设置 SSBS/`spec_bar`；`spectre_v4_enable_task_mitigation()` 在任务切换时按 `TIF_SSBD` 更新 PSTATE.SSBS；用户可通过 `prctl(PR_SPEC_STORE_BYPASS)` 控制（`arch_prctl_spec_ctrl_set/get`） | `proton-pack.c:634/657/754/798` |
| BHB（分支历史注入） | `spectre_bhb_enable_mitigation()`，对循环做 `spectre_bhb_patch_loop_mitigation_enable()`，对固件调用做 patch；受 `MITIGATE_SPECTRE_BRANCH_HISTORY` 控制（当前 `y`） | `proton-pack.c:1026/1100/1111` |
| 其它 errata | `cpu_errata.c` 按 MIDR 匹配并 `cpu_enable_*()` | `arch/arm64/kernel/cpu_errata.c` |

`spectre_v2_enable_mitigation()` 在启动时根据 `mitigations=off`/`auto` 等参数与能力决定 `spectre_v2_state`；`update_mitigation_state()`（`proton-pack.c:40`）维护 `MITIGATION_ON/NONE/UNKNOWN` 状态并打印。

### 9.1 v2 决策链与固件调用

`spectre_v2_enable_mitigation()`（`proton-pack.c:301`）的决策顺序：先查硬件缓解（`ID_AA64PFR0_EL1.CSV2` 或 safe list），硬件不行才走固件（`spectre_v2_enable_fw_mitigation()`）。固件路径按 SMCCC conduit 选择回调：

```c
switch (arm_smccc_1_1_get_conduit()) {
case SMCCC_CONDUIT_HVC:
	cb = call_hvc_arch_workaround_1;
	break;
case SMCCC_CONDUIT_SMC:
	cb = call_smc_arch_workaround_1;
	break;
default:
	return SPECTRE_VULNERABLE;
}
cb = spectre_v2_get_sw_mitigation_cb() ?: cb;
install_bp_hardening_cb(cb);
```

Qualcomm Falkor 有专用软件缓解 `qcom_link_stack_sanitisation()`（`proton-pack.c:245`）：保存 `x30` 后执行 16 次 `bl . + 4` 填满 link stack，再恢复 `x30`，使返回预测器不再含用户态注入的条目。`install_bp_hardening_cb()` 把回调写入 per-CPU `bp_hardening_data`，并在 EL2 可用时设置 `HYP_VECTOR_SPECTRE_DIRECT` 槽位供 KVM 使用。

状态更新用 `cmpxchg_relaxed` 循环且**只升不降**（`new <= state` 即退出）；`system_capabilities_finalized()` 之后再更新会 `WARN_ON`——big.LITTLE 系统里各 CPU 能力不同时，最终状态取最弱者。

### 9.2 BHB 缓解选择与 alternatives 补丁

`spectre_bhb_enable_mitigation()`（`proton-pack.c:1026`）按优先级选择缓解手段：

1. `supports_ecbhb()`（`ID_AA64MMFR1_EL1.ECBHB`）→ 硬件自动缓解，`BHB_HW`；
2. `supports_clearbhb()` → 入口插 `clearbhb` 指令，`BHB_INSN`，选 `EL1_VECTOR_BHB_CLEAR_INSN` 向量；
3. `spectre_bhb_loop_affected()` 返回非零 k → 循环缓解，`BHB_LOOP`，选 `EL1_VECTOR_BHB_LOOP` 向量；
4. `has_spectre_bhb_fw_mitigation()`（SMCCC `ARCH_WORKAROUND_3`）→ 固件缓解，`BHB_FW`，并卸载 v2 的 WA1 回调（WA3 覆盖它）。

k 值按 MIDR 分档（`spectre_bhb_loop_affected()`，`proton-pack.c:866`）：Cortex-X3/Neoverse-V2 为 132，A715/A720 为 38，A78/X1/X2/A710/N2/V1 为 32，A76/A77/N1 为 24，Ampere1 为 11，A57/A72 为 8；`max_bhb_k` 取全系统最大值。safe list（A35/A53/A55/A510/A520 等）直接判不受影响。

入口循环的次数不是编译期常量，而是 alternatives 运行时补丁：`spectre_bhb_patch_loop_iter()`（`proton-pack.c:1122`）把 `MOV` 指令的立即数改写为 `max_bhb_k`；`spectre_bhb_patch_loop_mitigation_enable()`/`spectre_bhb_patch_fw_mitigation_enabled()` 在对应缓解启用时把占位指令 patch 成 NOP；`spectre_bhb_patch_wa3()` 把立即数 patch 成 `ARM_SMCCC_ARCH_WORKAROUND_3` 的 ORR 编码。命令行 `nospectre_bhb`/`nospectre_v2`/`ssbd=` 分别关闭对应缓解，`spectre_print_disabled_mitigations()` 统一打印。

## 10. 堆与拷贝硬化

### 10.1 init_on_alloc / init_on_free

`mm/mm_init.c`：

```c
DEFINE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_ALLOC_DEFAULT_ON, init_on_alloc);
DEFINE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_FREE_DEFAULT_ON, init_on_free);
early_param("init_on_alloc", early_init_on_alloc);
early_param("init_on_free", early_init_on_free);
```

`want_init_on_alloc(gfp)`/`want_init_on_free()`（`include/linux/mm.h:4692`）在页分配器（`mm/page_alloc.c:1317`、`1813`）和 SLUB（`mm/slub.c:1933`、`2699`、`4549`）中生效。当前两个默认值都关闭，但可在命令行用 `init_on_alloc=1 init_on_free=1` 打开；`SLAB_POISON` 优先于它们（`slub.c:1936` 的提示）。

判断函数本身展示了静态键的零开销设计（`mm.h:4693`）：

```c
static inline bool want_init_on_alloc(gfp_t flags)
{
	if (static_branch_maybe(CONFIG_INIT_ON_ALLOC_DEFAULT_ON,
				&init_on_alloc))
		return true;
	return flags & __GFP_ZERO;
}
```

`DEFINE_STATIC_KEY_MAYBE`（`mm_init.c:2498`）按编译期默认值决定静态键初始状态：默认关闭时，分配热路径上的检查被 patch 成跳转直达 `__GFP_ZERO` 判断，无额外分支开销；`early_param` 解析的 `_init_on_alloc_enabled_early` 在启动早期翻转静态键。`init_on_free` 同理，但注意它**不**接受 `__GFP_ZERO` 兜底——释放清零只能靠全局开关。

### 10.2 SLUB freelist 硬化

- `CONFIG_SLAB_FREELIST_RANDOM`：`shuffle_freelist()`（`mm/slub.c:3279` 附近）在创建 cache 时打乱 freelist 顺序，增加堆喷稳定性难度；
- `CONFIG_SLAB_FREELIST_HARDENED`：`freelist_ptr_encode()`/`freelist_ptr_decode()`（`mm/slub.c:517/530`）用 per-cache 随机值与对象地址做异或，并在 `set_freepointer()` 中校验，检测 freelist 被改写。

当前均未启用，因此 `kmem_cache`/`kmalloc` 的 freelist 是可预测的（对调试有利）。

编码实现（`mm/slub.c:517`）很短，但设计点清晰：

```c
static inline freeptr_t freelist_ptr_encode(const struct kmem_cache *s,
					    void *ptr, unsigned long ptr_addr)
{
	unsigned long encoded;
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	encoded = (unsigned long)ptr ^ s->random ^ swab(ptr_addr);
#else
	encoded = (unsigned long)ptr;
#endif
	return (freeptr_t){.v = encoded};
}
```

异或项含**存放指针的地址**（字节序翻转后），因此同一 freelist 值在不同对象位置编码不同，攻击者不能简单复制一个合法指针到别处；`set_freepointer()`（`slub.c:554`）还有 `BUG_ON(object == fp)` 朴素检测 double free。未启用时这些全部退化为直存直取。

### 10.3 kmalloc 分区缓存

7.2 用 `CONFIG_KMALLOC_PARTITION_CACHES`（`mm/Kconfig:251`，默认 `RANDOM_KMALLOC_CACHES`）取代旧的随机 kmalloc cache：为 `KMALLOC_NORMAL` 生成 16 份隔离副本，使不同子系统/模块的对象落在不同 cache，降低“同 cache 相邻对象”型漏洞的利用价值。实现见 `mm/slab_common.c:828` 起的 `#ifdef CONFIG_KMALLOC_PARTITION_CACHES` 与 `KMALLOC_PARTITION_NAME` 宏。当前未启用。

实现方式是用 X-macro 为每个尺寸档展开 15 个分区名（`slab_common.c:831`）：

```c
#define KMA_PART_1(sz)  .name[KMALLOC_PARTITION_START + 1] = "kmalloc-part-01-" #sz,
#define KMA_PART_2(sz)  KMA_PART_1(sz) \
                        .name[KMALLOC_PARTITION_START + 2] = "kmalloc-part-02-" #sz,
...
#define KMA_PART_15(sz) KMA_PART_14(sz) \
                        .name[KMALLOC_PARTITION_START + 15] = "kmalloc-part-15-" #sz,
```

这些名字经 `INIT_KMALLOC_INFO()` 汇入 `kmalloc_info[]`（`slab_common.c:872`），与 `kmalloc-rcl-`（reclaim）、`kmalloc-cg-`（memcg）、`kmalloc-dma-` 等并列；尺寸档从 8/16/32 一直到 2M（`kmalloc_index()` 上限 2^21），另有 96/192 两个非 2 的幂档。启用后 `/sys/kernel/slab/` 下可见 `kmalloc-part-NN-<sz>` 系列 cache。

### 10.4 usercopy 与 FORTIFY

- `CONFIG_HARDENED_USERCOPY`（`mm/usercopy.c`）：`__check_object_size()` 在每次 `copy_to_user`/`copy_from_user` 时检查范围——`check_stack_object()` 验证栈对象大小，`check_heap_object()` 通过 `__check_heap_object()` 验证 slab 对象边界，跨越多个分配页或超出对象大小则 `usercopy_abort()`；`hardened_usercopy=` 可运行时开关（`usercopy.c:268`）。当前未启用。
- `CONFIG_FORTIFY_SOURCE`（`include/linux/fortify-string.h`）：对 `strcpy`/`memcpy`/`strscpy` 等提供 `__FORTIFY_INLINE` 包装，编译期已知大小时直接 `__builtin_*_chk`，运行期用 `__builtin_object_size` 校验，越界调用 `__fortify_panic()`/`fortify_warn_once()`。当前未启用。
- `CONFIG_ZERO_CALL_USED_REGS`：编译选项 `-fzero-call-used-regs=used-gpr`，函数返回前清零调用者可见寄存器，减少寄存器残留信息泄露。当前未启用。

`__check_object_size()`（`mm/usercopy.c:223`）的检查顺序与失败语义：

```c
void __check_object_size(const void *ptr, unsigned long n, bool to_user)
{
	if (!n)
		return;
	check_bogus_address((const unsigned long)ptr, n, to_user);
	switch (check_stack_object(ptr, n)) {
	case NOT_STACK:
		break;
	case GOOD_FRAME:
	case GOOD_STACK:
		return;
	default:
		usercopy_abort("process stack", ...);
	}
	check_heap_object(ptr, n, to_user);
	check_kernel_text_object((const unsigned long)ptr, n, to_user);
}
```

即：非法地址（回绕/NULL/内核 text）→ 栈对象越界 → 堆对象越界（slab 经 `__check_heap_object()`，vmalloc 查 `vm_area`，页分配查 compound 边界）→ 内核 text 暴露。任何一步失败都 `usercopy_abort()` 打印细节并 BUG。运行时开关是静态键 `validate_usercopy_range`（`DEFINE_STATIC_KEY_MAYBE_RO(CONFIG_HARDENED_USERCOPY_DEFAULT_ON, ...)`，`usercopy.c:212`），`hardened_usercopy=` 启动参数经 `late_initcall(set_hardened_usercopy)` 生效——关闭时检查点被静态分支消除，零开销。

## 11. Lockdown

`security/lockdown/lockdown.c` 是一个极小的 LSM：

```c
static enum lockdown_reason kernel_locked_down;

static const enum lockdown_reason lockdown_levels[] = {
	LOCKDOWN_NONE, LOCKDOWN_INTEGRITY_MAX, LOCKDOWN_CONFIDENTIALITY_MAX,
};
```

- 级别：`none` < `integrity` < `confidentiality`；`lock_kernel_down()` 只允许单向提升；
- 入口：`early_param("lockdown", lockdown_param)`（`lockdown=integrity|confidentiality`）、`/sys/kernel/security/lockdown`、以及 UEFI Secure Boot 检测（`arch_get_secureboot()`）；
- 执行点：唯一 hook `LSM_HOOK_INIT(locked_down, lockdown_is_locked_down)`，所有 `security_locked_down(what)` 调用都落到这里；拒绝时打印 `Lockdown: %s: %s is restricted`，原因字符串由 `security/security.c` 的 `lockdown_reasons[]` 提供（`LOCKDOWN_MODULE_SIGNATURE`、`LOCKDOWN_DEV_MEM`、`LOCKDOWN_KEXEC`、`LOCKDOWN_BPF_READ_KERNEL`、`LOCKDOWN_PERF`、`LOCKDOWN_TRACEFS` 等）。

`security/lockdown/Kconfig` 提供 `LOCK_DOWN_KERNEL_FORCE_NONE/INTEGRITY/CONFIDENTIALITY` 三选一。**当前 `CONFIG_SECURITY=n`，因此 Lockdown 完全不存在**——`/sys/kernel/security/lockdown` 不会出现，`security_locked_down()` 恒返回 0（inline 空实现）。

源码中的判定逻辑（`lockdown.c:27/59`）：

```c
static int lock_kernel_down(const char *where, enum lockdown_reason level)
{
	if (kernel_locked_down >= level)
		return -EPERM;          /* 只允许单向提升 */
	kernel_locked_down = level;
	pr_notice("Kernel is locked down from %s; ...\n", where);
	return 0;
}

static int lockdown_is_locked_down(enum lockdown_reason what)
{
	if (kernel_locked_down >= what) {
		pr_notice_ratelimited("Lockdown: %s: %s is restricted; ...\n",
				      current->comm, lockdown_reasons[what]);
		return -EPERM;
	}
	return 0;
}
```

每个 `lockdown_reason` 自带所需级别，`kernel_locked_down >= what` 一行完成比较。securityfs 文件读输出 `[integrity]` 形式的当前级别（`lockdown_read()`），写只接受级别名且同样只能提升（`lockdown_write()` → `lock_kernel_down("securityfs", ...)`）。启用 `CONFIG_SECURITY_LOCKDOWN_LSM_EARLY` 时用 `DEFINE_EARLY_LSM(lockdown)` 提前注册，保证早期启动路径也受约束；`lockdown_lsm_init()` 还会按 `LOCK_DOWN_KERNEL_FORCE_*` 在注册时直接锁定。

## 12. 其它编译期加固

- `__randomize_layout`/`__no_randomize_layout`（`include/linux/compiler_types.h:469`）：在 `CONFIG_RANDSTRUCT_FULL/PERFORMANCE` 下由编译器插件重排结构体字段；当前 `RANDSTRUCT_NONE=y`，宏为空。
- `__no_sanitize_address`/`__no_kcsan`/`__no_kmsan_checks`（`compiler_types.h:286-336`）：给不能插桩的代码（早期启动、lockdep 内部等）关闭 sanitizer；本配置无 sanitizer，宏无实际作用。
- `__counted_by(member)`（`compiler_types.h:376`）：配合 `CONFIG_UBSAN_BOUNDS`/`FORTIFY` 把柔性数组长度与计数字段关联，供运行时边界检查；当前未启用相关检测。
- `CONFIG_SLUB_DEBUG=y`：提供 `slub_debug=`、redzone、poison、`/sys/kernel/slab/*/` 统计，属于调试而非攻击缓解，但在无 KASAN 的配置里是发现越界/UAF 的主要手段。
- `CONFIG_DEBUG_LIST`、`CONFIG_BUG_ON_DATA_CORRUPTION`、`CONFIG_PANIC_ON_OOPS` 当前均未启用，链表损坏不会被主动检测。

`__ro_after_init` 的语义值得单独说明（`include/linux/cache.h:54` 注释）：标记的数据在 `mark_rodata_ro()` 之后只读，但 init 期间可写，因此不能放进 `.rodata`（那是 `const` 的位置），而是独立 section：

```c
// include/linux/cache.h:59
#ifndef __ro_after_init
#define __ro_after_init __section(".data..ro_after_init")
#endif
```

启动末期该 section 随 rodata 一起收紧。本系列笔记涉及的实例：

- `ima_blob_sizes`（`ima_main.c:1369`）、`evm_blob_sizes`（`evm_main.c:1225`）：LSM blob 布局；
- `evm_hooks`/`ima_appraise_hooks`：hook 表，控制流劫持的高价值目标；
- `cfi_warn`（`kernel/cfi.c:12`）、`ima_iint_cache` slab 指针（`ima_iint.c:16`）。

这与 `STRICT_KERNEL_RWX` 是同一防线的两层：section 归类 + 页表权限收紧，使运行期改写全局函数指针的路径被堵死。

## 13. 小结与启用建议

按“当前源码存在 / 当前配置关闭”区分：

1. **已生效**：PAC（内核+用户）、用户态 BTI、per-task 栈保护、KPTI、EPAN、STRICT_KERNEL_RWX、Spectre-BHB/v2/v3a/v4 缓解框架。
2. **源码完整但本配置关闭**：KASLR（`RANDOMIZE_BASE`）、内核 BTI（`ARM64_BTI_KERNEL`）、kCFI（`CFI`）、SCS、Lockdown（`SECURITY`）、init_on_alloc/free、freelist 随机/硬化、kmalloc 分区缓存、HARDENED_USERCOPY、FORTIFY_SOURCE、ZERO_CALL_USED_REGS、RANDSTRUCT。
3. 若要接近生产加固配置，建议至少打开：`RANDOMIZE_BASE`、`ARM64_BTI_KERNEL`、`STACKPROTECTOR_STRONG`（已开）、`INIT_ON_ALLOC_DEFAULT_ON`、`INIT_ON_FREE_DEFAULT_ON`、`SLAB_FREELIST_RANDOM/HARDENED`、`HARDENED_USERCOPY`、`FORTIFY_SOURCE`、`SECURITY` + `SECURITY_LOCKDOWN_LSM`；有 Clang 时加 `CFI`；发布内核可加 `RANDSTRUCT_FULL`。
4. 调试配置保留 KASLR 关闭、freelist 不硬化、无 sanitizer，可以换来可复现的地址与更低的运行时开销，但**不能据此判断上游内核缺少这些能力**。

按攻击类型对照各缓解的覆盖关系（源码均存在，生效与否取决于配置）：

| 攻击类型 | 主要缓解 | 本配置状态 |
|----------|----------|------------|
| 栈溢出改写返回地址 | PAC（`pac-ret`）、SCS、stack protector | PAC/canary 生效，SCS 关 |
| 栈溢出改写函数指针/数据 | PAC 数据密钥（用户态）、kCFI | 用户态 PAC 生效，kCFI 关 |
| JOP/间接跳转劫持 | BTI、kCFI | 用户态 BTI 生效，内核 BTI/kCFI 关 |
| 堆 UAF/越界 | MTE、KASAN、init_on_free、freelist 硬化 | MTE 生效（需硬件），其余关 |
| 堆喷布局预测 | freelist 随机、kmalloc 分区、KASLR | 均关 |
| 内核地址泄露 | KASLR、KPTI、E0PD、`__ro_after_init` | KPTI/E0PD 生效，KASLR 关 |
| 推测执行侧信道 | Spectre v1/v2/v4/BHB 缓解、nospec 屏障 | 框架生效 |
| usercopy 越界读写 | HARDENED_USERCOPY、FORTIFY、PAN/EPAN | PAN/EPAN 生效，检查类关 |
| 运行时内核篡改 | Lockdown、STRICT_KERNEL_RWX、模块签名 | RWX 生效，Lockdown 关 |

可以看到本调试配置的取舍：保留**硬件强制类**（PAC/BTI/PAN/KPTI/MTE）与推测执行缓解，关闭**软件检查类**（kCFI/usercopy/FORTIFY）与**随机化类**（KASLR/freelist），前者开销低且不影响地址可复现性，后者正是调试时需要排除的变量。

## 14. uaccess 与 PAN 实现

`copy_from_user`/`copy_to_user` 的“临时允许访问用户内存”由 `arch/arm64/include/asm/uaccess.h` 的 `uaccess_enable_privileged()`/`uaccess_disable_privileged()` 控制，两条互斥路径：

```c
static inline void uaccess_disable_privileged(void)
{
	mte_disable_tco();
	if (uaccess_ttbr0_disable())
		return;
	__uaccess_enable_hw_pan();
}
```

- **硬件 PAN**（本配置走这条）：`__uaccess_enable_hw_pan()`/`__uaccess_disable_hw_pan()` 用 `ALTERNATIVE("nop", SET_PSTATE_PAN(1/0), ARM64_HAS_PAN)` 在运行时 patch 出 `msr pan, #1/#0`——不支持 PAN 的 CPU 上就是 nop；
- **软件 TTBR0 PAN**（`CONFIG_ARM64_SW_TTBR0_PAN`，本配置未设）：`__uaccess_ttbr0_disable()` 把 `ttbr0_el1` 指向 `reserved_pg_dir`（位于 `swapper_pg_dir` 之前的固定偏移），并给 `ttbr1_el1` 换保留 ASID，使用户映射彻底不可达；enable 时从 `current_thread_info()->ttbr0` 恢复。全程 `local_irq_save` 防止 ASID roll-over 竞态。

此外 uaccess 指针会做地址净化：清 bit 55 保证指针落在 TTBR0（用户）范围，任何指向内核地址的“用户指针”都会被折叠到用户空间而访问失败——这是 Spectre v1 缓解在 uaccess 层的体现（`cpu_show_spectre_v1()` 输出的 “__user pointer sanitization” 即指此类机制）。EPAN（`ARM64_EPAN=y`）进一步让 `ldtr/sttr` 等非特权访问指令也受 PAN 约束。

## 15. 验证与调试接口

确认加固是否生效的运行时入口：

| 接口 | 内容 |
|------|------|
| `/sys/devices/system/cpu/vulnerabilities/*` | `spectre_v1/v2/v3a/v4`、`spectre_bhb`、`meltdown` 等，由 `cpu_show_*()` 输出（如 `proton-pack.c:62` 的 v1 固定串） |
| `/proc/cpuinfo` Features | `paca`/`pag`/`bti`/`ssbs`/`sb`/`csv2`/`csv3` 等硬件能力位 |
| dmesg | `KASLR disabled due to lack of seed`、`CPU features: detected: ...`、`spectre-v2 mitigation disabled by command-line option`（`spectre_print_disabled_mitigations()`） |
| `/sys/kernel/security/lockdown` | Lockdown 级别（需 `CONFIG_SECURITY`，本配置不存在） |
| `/sys/kernel/slab/*/` | SLUB 统计与 `slub_debug=` 选项（本配置 `SLUB_DEBUG=y`） |

命令行开关汇总：`nokaslr`、`mitigations=off/auto`、`nospectre_v2`、`nospectre_bhb`、`ssbd=force-on/force-off/kernel`、`init_on_alloc=0/1`、`init_on_free=0/1`、`hardened_usercopy=on/off`、`slub_debug=`、`lockdown=integrity/confidentiality`。用户态 `prctl` 侧：`PR_PAC_RESET_KEYS`/`PR_PAC_SET_ENABLED_KEYS`（PAC）、`PR_SPEC_STORE_BYPASS`（SSBD，经 `arch_prctl_spec_ctrl_set()`，`proton-pack.c:754`）。

一个值得注意的联动：`unpriv_ebpf_notify()`（`proton-pack.c:1182`）在 v2 已缓解但 BHB 未缓解时，若开启非特权 eBPF 会打印 `Unprivileged eBPF is enabled, data leaks possible via Spectre v2 BHB attacks!`——说明缓解状态评估会跨子系统联动。

## 16. MTE 与 E0PD（当前已启用的两项）

除第 1 节表格外，本配置还启用了两项常被忽略的 ARMv8.5 特性：

### 16.1 MTE（Memory Tagging Extension）

`CONFIG_ARM64_MTE=y`（`arch/arm64/Kconfig:2150`，default y，依赖 `ARM64_AS_HAS_MTE && ARM64_TAGGED_ADDR_ABI`）。MTE 给每 16 字节内存 granule 分配 4 位 tag，指针高位携带 lock tag，访问时不匹配即 fault——硬件级的 UAF/越界检测。内核侧要点：

- Kconfig `select ARCH_HAS_SUBPAGE_FAULTS`，因为 uaccess 例程需要按 granule 处理 tag 检查失败；
- `entry.S` 在异常入口/出口维护 tag 状态：`check_mte_async_tcf`（`entry.S:167`）检查异步 tag check fault，`mte_set_kernel_gcr`/`mte_set_user_gcr`（`entry.S:210/221`）按 `GCR_EL1` 切换内核/用户 tag 包含掩码；
- `mte_disable_tco()`/`mte_enable_tco()`（`asm/mte.h`）在 uaccess 期间临时关闭 Tag Check Override，与 PAN 开关联动（见第 14 节代码）；
- 用户态经 `prctl(PR_SET_TAGGED_ADDR_CTRL)` 控制，内核线程默认不启用 tag 检查。

MTE 与 KASAN 定位不同：KASAN 是编译期插桩的软件检测（本配置未开），MTE 是硬件特性，开销更低但需要硬件与用户态配合。

### 16.2 E0PD（EL0 Privileged Access Never 的时序补充）

`CONFIG_ARM64_E0PD=y`（`arch/arm64/Kconfig:2130`，default y）。Kconfig help 原文：E0PD 保证 **EL0 经 TTBR1 的访问恒时 fault**，提供与 KPTI 类似的 KASLR 保护收益，但开销更低，且不干扰 SPE 等对内核内存的合法访问。即使用户态探测内核地址，fault 时延也不泄露该地址是否已映射——这是对 KPTI 的时序侧信道补充，本配置两者同时启用。

## 关键文件清单

| 路径 | 说明 |
|------|------|
| `arch/arm64/Kconfig` | `RANDOMIZE_BASE`(2361)、`RANDOMIZE_MODULE_REGION_FULL`(2379)、`ARM64_PTR_AUTH`(1967)、`ARM64_PTR_AUTH_KERNEL`(1991)、`ARM64_BTI`(2087)、`ARM64_BTI_KERNEL`(2109) |
| `arch/arm64/include/asm/pointer_auth.h` | `ptrauth_key`、`ptrauth_keys_user/kernel`、密钥初始化/安装/切换、`ptrauth_enable()` |
| `arch/arm64/include/asm/stackprotector.h` | `boot_init_stack_canary()`，per-task canary 与 PAC 初始化 |
| `arch/arm64/Makefile` | `-mstack-protector-guard=sysreg/sp_el0`（71-79 行） |
| `arch/arm64/include/asm/memory.h` | `kaslr_offset()`、`kaslr_enabled()` |
| `arch/arm64/include/asm/boot.h` | `MIN_KIMG_ALIGN = SZ_2M` |
| `arch/arm64/kernel/kaslr.c` | `kaslr_init()`、`nokaslr` 处理、种子缺失判定 |
| `arch/arm64/kernel/pi/kaslr_early.c` | `kaslr_early_init()`、`get_kaslr_seed()`、偏移计算 |
| `arch/arm64/kernel/proton-pack.c` | Spectre v2/v3a/v4/BHB 缓解与状态机 |
| `arch/arm64/kernel/cpu_errata.c` | CPU erratum 匹配与 `cpu_enable_*` |
| `arch/arm64/kernel/entry.S` | KPTI `tramp_vectors`/`tramp_map_kernel`、异常入口 |
| `arch/arm64/mm/mmu.c` | `mark_rodata_ro()`（1201）、`PAGE_KERNEL_ROX` 选择 |
| `arch/arm64/include/asm/uaccess.h` | `uaccess_enable/disable_privileged`、SW_TTBR0_PAN、硬件 PAN alternatives |
| `arch/arm64/include/asm/scs.h` | 影子调用栈指针加载宏 |
| `arch/arm64/kernel/pi/patch-scs.c` | 早期 SCS 补丁 |
| `arch/Kconfig` | `CFI`(937)、`CFI_ICALL_NORMALIZE_INTEGERS`(959)、`SHADOW_CALL_STACK`(754)、`STRICT_KERNEL_RWX`(1581) |
| `kernel/cfi.c` | kCFI trap 处理与模块 `__kcfi_traps` 解析 |
| `include/linux/cfi.h` | CFI 声明 |
| `security/Kconfig.hardening` | `INIT_ON_ALLOC_DEFAULT_ON`(159)、`INIT_ON_FREE_DEFAULT_ON`(172)、`ZERO_CALL_USED_REGS`(194)、`FORTIFY_SOURCE`(214)、`HARDENED_USERCOPY`(221)、`RANDSTRUCT_*`(303) |
| `include/linux/compiler_types.h` | `__no_sanitize_*`、`__randomize_layout`、`__counted_by` |
| `include/linux/cache.h` | `__ro_after_init`（60 行） |
| `mm/mm_init.c` | `init_on_alloc`/`init_on_free` 静态键与 `early_param`（2511/2519） |
| `mm/page_alloc.c` | 页分配路径上的 `want_init_on_alloc/free`（1317/1813） |
| `mm/slub.c` | freelist 编码/解码（517/530）、`shuffle_freelist`、init_on 处理 |
| `mm/slab_common.c` | `KMALLOC_PARTITION_CACHES` 实现（828 起） |
| `mm/Kconfig` | `KMALLOC_PARTITION_CACHES`(251)、`SHUFFLE_PAGE_ALLOCATOR`(323) |
| `mm/usercopy.c` | `__check_object_size()`、`check_stack_object()`、`check_heap_object()` |
| `include/linux/fortify-string.h` | FORTIFY 版 str/mem 函数与 `__fortify_panic()` |
| `include/linux/nospec.h` | `array_index_nospec()`、`barrier_nospec()` |
| `security/lockdown/lockdown.c` | lockdown LSM、级别与 `locked_down` hook |
| `kernel/panic.c` | `__stack_chk_fail()`（1199）栈保护失败 panic |
| `include/linux/stackprotector.h` | `get_random_canary()`、`CANARY_MASK` |
| `arch/arm64/include/asm/mte.h` | MTE tag 控制、`mte_*_tco` |
| `security/lockdown/Kconfig` | `LOCK_DOWN_KERNEL_FORCE_*` |
| `security/security.c` | `lockdown_reasons[]`、`security_locked_down()` |
