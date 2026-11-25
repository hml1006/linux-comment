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

## 5. 影子调用栈（SCS）

`CONFIG_SHADOW_CALL_STACK`（`arch/Kconfig:754`）用一块独立影子栈保存返回地址，普通栈被溢出改写也不影响返回地址。ARM64 通过 `x18` 保存影子栈指针：

- `arch/arm64/include/asm/scs.h` 的 `scs_load_current`/`scs_load_current_base` 宏在异常入口/上下文切换时加载 `current->scs_base`；
- `arch/arm64/kernel/pi/patch-scs.c` 在早期对不写 `x18` 的函数打补丁；
- 依赖 `DYNAMIC_FTRACE_WITH_ARGS`/`REGS` 或关闭函数图追踪。

当前未启用。注意 SCS 与 PAC 都保护返回地址，但机制不同：PAC 是密码学认证，SCS 是物理隔离副本。

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

### 7.2 模块区域

`CONFIG_RANDOMIZE_MODULE_REGION_FULL`（`arch/arm64/Kconfig:2379`，依赖 `RANDOMIZE_BASE`）把模块区在覆盖内核的 2GB 窗口内随机化，降低模块地址泄露内核布局的风险。KASLR 关闭时该选项也不可用。

### 7.3 当前影响

KASLR 关闭意味着 `_text` 固定在 `KIMAGE_VADDR`，`kaslr_offset()` 恒为 0，`/proc/vmcore` 的 `KERNELOFFSET=0`（`vmcore_info.c:35`），调试友好但削弱了对地址依赖型漏洞的防护。

## 8. 内存布局保护

- `CONFIG_STRICT_KERNEL_RWX=y`：内核 text/rodata 映射为只读，非 text 区域 NX，`mark_rodata_ro()` 在启动末期收紧权限；配合 `__ro_after_init`（`include/linux/cache.h:60`，`__section(".data..ro_after_init")`）让初始化后不再修改的数据进入只读页。
- `CONFIG_UNMAP_KERNEL_AT_EL0=y`（KPTI）：用户态运行时页表切换到 `tramp_pg_dir`，内核页表被卸载；`arch/arm64/kernel/entry.S` 中 `tramp_vectors`/`tramp_map_kernel` 在异常入口把 `tramp_pg_dir` 换回 `swapper_pg_dir`（`entry.S:697`、`789`、`836`）。这是 Meltdown 类攻击的核心缓解。
- `CONFIG_ARM64_EPAN=y`：Enhanced Privileged Access Never，内核默认不能直接访问用户内存，必须走 `copy_from_user`/`copy_to_user`/`uaccess` 路径；比传统 PAN 更严格地覆盖 `ldtr/sttr` 等指令。
- `CONFIG_ARM64_SW_TTBR0_PAN` 未设置：硬件 PAN 可用时不需要软件模拟。

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

### 10.2 SLUB freelist 硬化

- `CONFIG_SLAB_FREELIST_RANDOM`：`shuffle_freelist()`（`mm/slub.c:3279` 附近）在创建 cache 时打乱 freelist 顺序，增加堆喷稳定性难度；
- `CONFIG_SLAB_FREELIST_HARDENED`：`freelist_ptr_encode()`/`freelist_ptr_decode()`（`mm/slub.c:517/530`）用 per-cache 随机值与对象地址做异或，并在 `set_freepointer()` 中校验，检测 freelist 被改写。

当前均未启用，因此 `kmem_cache`/`kmalloc` 的 freelist 是可预测的（对调试有利）。

### 10.3 kmalloc 分区缓存

7.2 用 `CONFIG_KMALLOC_PARTITION_CACHES`（`mm/Kconfig:251`，默认 `RANDOM_KMALLOC_CACHES`）取代旧的随机 kmalloc cache：为 `KMALLOC_NORMAL` 生成 16 份隔离副本，使不同子系统/模块的对象落在不同 cache，降低“同 cache 相邻对象”型漏洞的利用价值。实现见 `mm/slab_common.c:828` 起的 `#ifdef CONFIG_KMALLOC_PARTITION_CACHES` 与 `KMALLOC_PARTITION_NAME` 宏。当前未启用。

### 10.4 usercopy 与 FORTIFY

- `CONFIG_HARDENED_USERCOPY`（`mm/usercopy.c`）：`__check_object_size()` 在每次 `copy_to_user`/`copy_from_user` 时检查范围——`check_stack_object()` 验证栈对象大小，`check_heap_object()` 通过 `__check_heap_object()` 验证 slab 对象边界，跨越多个分配页或超出对象大小则 `usercopy_abort()`；`hardened_usercopy=` 可运行时开关（`usercopy.c:268`）。当前未启用。
- `CONFIG_FORTIFY_SOURCE`（`include/linux/fortify-string.h`）：对 `strcpy`/`memcpy`/`strscpy` 等提供 `__FORTIFY_INLINE` 包装，编译期已知大小时直接 `__builtin_*_chk`，运行期用 `__builtin_object_size` 校验，越界调用 `__fortify_panic()`/`fortify_warn_once()`。当前未启用。
- `CONFIG_ZERO_CALL_USED_REGS`：编译选项 `-fzero-call-used-regs=used-gpr`，函数返回前清零调用者可见寄存器，减少寄存器残留信息泄露。当前未启用。

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

## 12. 其它编译期加固

- `__randomize_layout`/`__no_randomize_layout`（`include/linux/compiler_types.h:469`）：在 `CONFIG_RANDSTRUCT_FULL/PERFORMANCE` 下由编译器插件重排结构体字段；当前 `RANDSTRUCT_NONE=y`，宏为空。
- `__no_sanitize_address`/`__no_kcsan`/`__no_kmsan_checks`（`compiler_types.h:286-336`）：给不能插桩的代码（早期启动、lockdep 内部等）关闭 sanitizer；本配置无 sanitizer，宏无实际作用。
- `__counted_by(member)`（`compiler_types.h:376`）：配合 `CONFIG_UBSAN_BOUNDS`/`FORTIFY` 把柔性数组长度与计数字段关联，供运行时边界检查；当前未启用相关检测。
- `CONFIG_SLUB_DEBUG=y`：提供 `slub_debug=`、redzone、poison、`/sys/kernel/slab/*/` 统计，属于调试而非攻击缓解，但在无 KASAN 的配置里是发现越界/UAF 的主要手段。
- `CONFIG_DEBUG_LIST`、`CONFIG_BUG_ON_DATA_CORRUPTION`、`CONFIG_PANIC_ON_OOPS` 当前均未启用，链表损坏不会被主动检测。

## 13. 小结与启用建议

按“当前源码存在 / 当前配置关闭”区分：

1. **已生效**：PAC（内核+用户）、用户态 BTI、per-task 栈保护、KPTI、EPAN、STRICT_KERNEL_RWX、Spectre-BHB/v2/v3a/v4 缓解框架。
2. **源码完整但本配置关闭**：KASLR（`RANDOMIZE_BASE`）、内核 BTI（`ARM64_BTI_KERNEL`）、kCFI（`CFI`）、SCS、Lockdown（`SECURITY`）、init_on_alloc/free、freelist 随机/硬化、kmalloc 分区缓存、HARDENED_USERCOPY、FORTIFY_SOURCE、ZERO_CALL_USED_REGS、RANDSTRUCT。
3. 若要接近生产加固配置，建议至少打开：`RANDOMIZE_BASE`、`ARM64_BTI_KERNEL`、`STACKPROTECTOR_STRONG`（已开）、`INIT_ON_ALLOC_DEFAULT_ON`、`INIT_ON_FREE_DEFAULT_ON`、`SLAB_FREELIST_RANDOM/HARDENED`、`HARDENED_USERCOPY`、`FORTIFY_SOURCE`、`SECURITY` + `SECURITY_LOCKDOWN_LSM`；有 Clang 时加 `CFI`；发布内核可加 `RANDSTRUCT_FULL`。
4. 调试配置保留 KASLR 关闭、freelist 不硬化、无 sanitizer，可以换来可复现的地址与更低的运行时开销，但**不能据此判断上游内核缺少这些能力**。

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
| `security/lockdown/Kconfig` | `LOCK_DOWN_KERNEL_FORCE_*` |
| `security/security.c` | `lockdown_reasons[]`、`security_locked_down()` |
