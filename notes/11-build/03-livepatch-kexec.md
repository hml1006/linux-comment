# 3. Livepatch 与 Kexec：运行时代码替换与内核重启

## 3.1 概述

本篇覆盖两个经常被并列讨论、但机制完全不同的能力：

| 能力 | 目标 | 核心源码 |
| --- | --- | --- |
| Livepatch | 不重启内核，把某个函数的调用重定向到新实现 | `kernel/livepatch/` |
| Kexec | 从当前内核直接引导另一个内核 | `kernel/kexec.c`、`kernel/kexec_file.c`、`kernel/kexec_core.c` |
| Kdump（Kexec 的用法） | 崩溃时用 kexec 引导捕获内核，导出 vmcore | `kernel/crash_core.c`、`CONFIG_CRASH_DUMP` |

Livepatch 是“函数级替换”，依赖 ftrace；Kexec 是“整机内核替换”，依赖架构的 `machine_kexec`。

## 3.2 Livepatch 的一致性模型

Livepatch 最难的不是替换指令，而是**保证任意任务在任意时刻看到一致的函数集合**。内核采用“全局目标状态 + 每任务状态 + 每函数过渡标志”的模型。

### 3.2.1 三种状态

`include/linux/livepatch.h:21`：

```c
#define KLP_TRANSITION_IDLE		-1
#define KLP_TRANSITION_UNPATCHED	 0
#define KLP_TRANSITION_PATCHED		 1
```

- 全局 `klp_target_state`（`transition.c:26`）：所有任务最终要切换到的状态
- 每任务 `task->patch_state`：该任务当前应使用的函数版本
- 每函数 `func->transition`：该函数正在过渡中

### 3.2.2 每函数状态组合

`struct klp_func` 的注释（`include/linux/livepatch.h:42-55`）给出四种组合：

| patched | transition | 含义 |
| --- | --- | --- |
| 0 | 0 | 未打补丁 |
| 0 | 1 | 未打补丁，临时起始态 |
| 1 | 1 | 已打补丁，但可能只对部分任务可见 |
| 1 | 0 | 已打补丁，对所有任务可见 |

卸载时按相反顺序回退。

### 3.2.3 为什么需要“每任务”状态

如果只是把函数入口改成跳转，那么一个任务可能刚进入旧函数、在其内部又调用同一个被替换函数，从而出现“旧函数调用新函数”的混合栈。Livepatch 的规则是：**一个任务在一次过渡中只能看到一个一致版本**。因此：

- `klp_ftrace_handler()` 在 `func->transition` 为真时读取 `current->patch_state`
- 若任务还是 `UNPATCHED`，则沿 `func_stack` 找到旧版本继续执行
- 只有当任务被安全切换到目标状态后，才让它看到新函数

## 3.3 核心数据结构

### 3.3.1 klp_func

```c
struct klp_func {
	const char *old_name;       /* 要替换的函数名 */
	void *new_func;             /* 新实现 */
	unsigned long old_sympos;   /* 同名符号消歧，0 表示唯一 */
	void *old_func;             /* 解析后的旧地址 */
	struct list_head stack_node;/* klp_ops->func_stack 节点 */
	unsigned long old_size, new_size;
	bool nop, patched, transition;
};
```

`old_sympos` 用于处理 kallsyms 中同名符号（例如被多次编译的静态函数）。

### 3.3.2 klp_object

```c
struct klp_object {
	const char *name;           /* 模块名；NULL 表示 vmlinux */
	struct klp_func *funcs;
	struct klp_callbacks callbacks;
	struct module *mod;
	bool dynamic, patched;
};
```

`objs` 数组以“`funcs` 与 `name` 同时为空”作为结束标记，见 `klp_for_each_object_static()`。

### 3.3.3 klp_patch

```c
struct klp_patch {
	struct module *mod;
	struct klp_object *objs;
	struct klp_state *states;
	bool replace;
	struct list_head list;
	struct list_head obj_list;
	bool enabled, forced;
	struct work_struct free_work;
	struct completion finish;
};
```

- `replace = true` 表示替换当前所有活跃补丁（累积补丁链）
- `states` 用于跨补丁共享系统状态（`klp_state`：`id`/`version`/`data`），由 `kernel/livepatch/state.c` 管理

### 3.3.4 klp_callbacks

`include/linux/livepatch.h` 中的回调结构提供 `pre_patch`/`post_patch`/`pre_unpatch`/`post_unpatch`，`kernel/livepatch/core.h` 的内联函数负责调用并维护 `post_unpatch_enabled` 标志。

## 3.4 ftrace 重定向机制

### 3.4.1 klp_ops 与函数栈

每个被替换的旧函数地址对应一个 `struct klp_ops`，其中有一个 `func_stack`。多个补丁可以叠加在同一函数上，栈顶是当前最新版本。

### 3.4.2 klp_ftrace_handler

`kernel/livepatch/patch.c:40`：

```c
static void notrace klp_ftrace_handler(unsigned long ip, unsigned long parent_ip,
				       struct ftrace_ops *fops, struct ftrace_regs *fregs)
{
	ops = container_of(fops, struct klp_ops, fops);
	bit = ftrace_test_recursion_trylock(ip, parent_ip);
	func = list_first_or_null_rcu(&ops->func_stack, struct klp_func, stack_node);
	smp_rmb();
	if (unlikely(func->transition)) {
		smp_rmb();
		patch_state = current->patch_state;
		if (patch_state == KLP_TRANSITION_UNPATCHED) {
			func = list_entry_rcu(func->stack_node.next, ...);
			if (&func->stack_node == &ops->func_stack)
				goto unlock;      /* 没有旧补丁，执行原函数 */
		}
	}
	if (func->nop)
		goto unlock;                  /* NOP 占位，不修改 PC */
	ftrace_regs_set_instruction_pointer(fregs, (unsigned long)func->new_func);
unlock:
	ftrace_test_recursion_unlock(bit);
}
```

关键点：`notrace` 防止递归；`ftrace_test_recursion_trylock()` 禁用抢占并防止 ftrace 递归，是 `klp_synchronize_transition()` 使用特殊 RCU 同步的前提；`nop` 用原代码替换已有补丁且不修改 PC，避免自跳转死循环。

### 3.4.3 ftrace_ops 标志

```c
ops->fops.flags = FTRACE_OPS_FL_DYNAMIC |
#ifndef CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS
		  FTRACE_OPS_FL_SAVE_REGS |
#endif
		  FTRACE_OPS_FL_IPMODIFY |
		  FTRACE_OPS_FL_PERMANENT;
```

`IPMODIFY` 允许修改指令指针，是重定向的必要条件；`PERMANENT` 表示补丁生命周期内不注销；`SAVE_REGS` 用于缺少 `DYNAMIC_FTRACE_WITH_ARGS` 的架构；`DYNAMIC` 支持每个函数独立 ops。注册时先 `ftrace_location()` 找地址，再 `ftrace_set_filter_ip()` 过滤。

## 3.5 使能与 transition 流程

### 3.5.1 klp_enable_patch

`kernel/livepatch/core.c:1108` 的 `klp_enable_patch()` 做三件事：

1. 校验补丁对象/函数，解析 `old_func` 地址，建立 sysfs
2. `klp_init_patch_early()` / `klp_init_patch()` 建立内部链表
3. 对每个对象调用 `__klp_enable_patch()`

### 3.5.2 __klp_enable_patch

`core.c:1040` 的顺序：

```c
klp_init_transition(patch, KLP_TRANSITION_PATCHED);
klp_for_each_object(patch, obj)
	if (klp_is_object_loaded(obj))
		klp_patch_object(obj);      /* 把新 func 压入 func_stack */
klp_start_transition();             /* 给任务置 TIF_PATCH_PENDING */
klp_try_complete_transition();
```

`klp_init_transition()`（`transition.c:552`）：

1. 断言全局状态为 `IDLE`
2. 设置 `klp_transition_patch` 与 `klp_target_state`
3. 遍历所有进程/线程和每 CPU idle 任务，把 `task->patch_state` 设为初始状态（打补丁时为 `UNPATCHED`）
4. `smp_wmb()` 保证任务状态初始化先于 `func->transition = true`
5. 对所有 func 置 `transition = true`

### 3.5.3 任务切换

`klp_try_switch_task()`（`transition.c:305`）：已处于目标状态则成功；架构没有可靠栈回溯（`!klp_have_reliable_stack()`）时只能依赖任务在 `__schedule()` 中主动切换；否则检查任务栈中是否有待打/待撤函数，或任务正在运行（`-EBUSY`）、栈不可靠（`-EINVAL`）、睡在目标函数上（`-EADDRINUSE`）。

`__klp_sched_try_switch()`（第 357 行）在调度点调用：`klp_patch_pending(current)`（读 `TIF_PATCH_PENDING`）为真时执行 `smp_rmb()` 后调用 `klp_try_switch_task(current)`。无法自行切换的任务由 `klp_send_signals()`（第 387 行）发假信号强制经过调度点。

### 3.5.4 完成过渡

`klp_complete_transition()`（`transition.c:81`）：撤补丁时先删除栈顶函数、清理 nop；把 `klp_target_state` 与每个任务的 `patch_state` 置为 `IDLE`；更新 sysfs、唤醒等待者。完成后新函数对所有任务一致可见。

### 3.5.5 一个最小 livepatch 模块

```c
static int livepatch_probe(void) { return 0; }

static struct klp_func funcs[] = {
	{ .old_name = "cmdline_proc_show", .new_func = livepatch_probe }, {}
};
static struct klp_object objs[] = {
	{ .funcs = funcs },   /* name == NULL：vmlinux */
	{}
};
static struct klp_patch patch = { .mod = THIS_MODULE, .objs = objs };

static int __init lp_init(void) { return klp_enable_patch(&patch); }
static void __exit lp_exit(void) { }
module_init(lp_init);
module_exit(lp_exit);
MODULE_LICENSE("GPL");
```

实际补丁中 `new_func` 必须与原函数签名完全一致，并且通常通过 `klp_get_state()` 共享状态。

## 3.6 Kexec：两套系统调用

| 接口 | 系统调用 | 输入 | 适用 |
| --- | --- | --- | --- |
| 传统段式 | `kexec_load` | 用户态解析好的 `kexec_segment` 数组 | 用户态工具自行处理 ELF/zImage |
| 文件式 | `kexec_file_load` | 内核镜像 fd、initrd fd、cmdline | 内核负责解析、校验、布局 |

### 3.6.1 kexec_load

`kernel/kexec.c:242` 的 `SYSCALL_DEFINE4(kexec_load, entry, nr_segments, segments, flags)` 先经 `kexec_load_check()`（第 202 行）校验：`kexec_load_permitted()`（需 `CAP_SYS_BOOT`）、`security_kernel_load_data(LOADING_KEXEC_IMAGE, false)`、flags 只能落在 `KEXEC_FLAGS | KEXEC_ARCH_MASK`、`nr_segments <= KEXEC_SEGMENT_MAX`、架构字段为 `KEXEC_ARCH_DEFAULT` 或 `KEXEC_ARCH`。

`do_kexec_load()`（第 91 行）：`KEXEC_ON_CRASH` 设 `KEXEC_TYPE_CRASH` 并指向 `kexec_crash_image`，否则指向 `kexec_image`；逐段 `copy_from_user`，`machine_kexec_prepare()`，`kimage_load_segment()` 复制到目标物理页，`machine_kexec_cleanup()`、`kimage_terminate()`，最后 `xchg(dest_image, image)` 原子替换。

### 3.6.2 kexec_file_load

`kernel/kexec_file.c:364` 的 `SYSCALL_DEFINE5(kexec_file_load, kernel_fd, initrd_fd, cmdline_len, cmdline_ptr, flags)` 支持 `KEXEC_FILE_UNLOAD`、`KEXEC_FILE_ON_CRASH`、`KEXEC_FILE_NO_INITRAMFS`、`KEXEC_FILE_DEBUG`。流程：`machine_kexec_prepare()`、`kimage_crash_copy_vmcoreinfo()`、`kexec_calculate_store_digests()`、逐段 `kimage_load_segment()`、`kimage_terminate()`、`kexec_post_load()`、`kimage_file_post_load_cleanup()`、`xchg(dest_image, image)`。

文件式加载通过 `struct kexec_file_ops` 分派：`kexec_image_probe_default()` 遍历 `kexec_file_loaders[]`，由各架构提供 ELF/bzImage 解析器，最终都调用 `kexec_add_buffer()`（第 747 行）把缓冲区放进 `kimage` 段表。

## 3.7 struct kimage

`include/linux/kexec.h:339`：

```c
struct kimage {
	kimage_entry_t head;
	kimage_entry_t *entry;
	kimage_entry_t *last_entry;

	unsigned long start;
	struct page *control_code_page;
	struct page *swap_page;
	void *vmcoreinfo_data_copy;

	unsigned long nr_segments;
	struct kexec_segment segment[KEXEC_SEGMENT_MAX];
	struct page *segment_cma[KEXEC_SEGMENT_MAX];

	struct list_head control_pages;
	struct list_head dest_pages;
	struct list_head unusable_pages;

	unsigned int type : 1;          /* KEXEC_TYPE_DEFAULT / KEXEC_TYPE_CRASH */
	unsigned int preserve_context : 1;
	unsigned int file_mode : 1;
	/* ... */

#ifdef CONFIG_KEXEC_FILE
	void *kernel_buf;  unsigned long kernel_buf_len;
	void *initrd_buf;  unsigned long initrd_buf_len;
	char *cmdline_buf; unsigned long cmdline_buf_len;
	const struct kexec_file_ops *fops;
	void *image_loader_data;
	struct purgatory_info purgatory_info;
#endif
};
```

镜像由**控制页链**（`head`/`entry`）描述：每个控制页含“下一块目标页”的条目，最终由 `machine_kexec()` 按架构约定跳转。`purgatory` 是一小段在旧内核已退出、新内核尚未启动之间运行的代码，负责校验、关中断、跳转。

## 3.8 从加载到重启

`kernel/kexec_core.c:1138` 的 `kernel_kexec()`：

1. 取 `kexec_image`
2. `machine_kexec_prepare()` 等架构准备
3. 停其他 CPU、关闭中断、`kernel_restart_prepare()`
4. `machine_kexec(image)` 执行跳转

用户态一般在 `kexec -e` 时调用 `reboot(LINUX_REBOOT_CMD_KEXEC)`，由内核走 `kernel_kexec()`。

## 3.9 Kdump 关系

Kdump 不是独立机制，而是 **kexec 的 crash 模式**：

- 预留 crashkernel 内存（`crashk_res`，由 `crashkernel=` 参数或设备树确定）
- 用户态 `kexec -p vmlinux --initrd=...` 调用 `kexec_load`/`kexec_file_load`，flags 带 `KEXEC_ON_CRASH`/`KEXEC_FILE_ON_CRASH`
- 镜像存入 `kexec_crash_image`（`kexec_core.c:1034`）
- 崩溃时 `panic()` → `crash_kexec()`（`kernel/crash_core.c`）→ `machine_crash_shutdown()` → `machine_kexec(kexec_crash_image)`
- 捕获内核通过 `/proc/vmcore` 导出旧内核内存

`kexec_crash_loaded()`（`crash_core.c:108`）就是 `!!kexec_crash_image`，对应 sysfs 的 `crash_loaded`。

## 3.10 当前 .config 下的可见性

| 配置项 | 当前值 | 影响 |
| --- | --- | --- |
| `CONFIG_KEXEC_CORE` | `y` | `kernel/kexec.c`、`kernel/kexec_core.c` 参与编译 |
| `CONFIG_KEXEC_FILE` | `y` | `kernel/kexec_file.c` 参与编译，`kexec_file_load` 可用 |
| `CONFIG_KEXEC_SIG` | 未设置 | 文件式加载不做内核镜像签名强制校验 |
| `CONFIG_KEXEC_HANDOVER` | 未设置 | 不支持 kexec handover |
| `CONFIG_CRASH_DUMP` | `y` | `/proc/vmcore`、crash 镜像支持开启 |
| `CONFIG_HAVE_LIVEPATCH` | `y` | 架构能力存在 |
| `CONFIG_LIVEPATCH` | 未设置 | `kernel/livepatch/Makefile` 的 `obj-$(CONFIG_LIVEPATCH)` 为空，整个 livepatch 不编译 |
| `CONFIG_MODULES` | 未设置 | 即使打开 `LIVEPATCH` 也无法加载补丁模块（Kconfig 依赖 `MODULES`） |

因此当前内核中：

- **Kexec 可用**：`kexec_load` 与 `kexec_file_load` 均存在，`CONFIG_CRASH_DUMP=y` 支持 kdump 捕获。
- **Livepatch 不可用**：源码完整，但 `CONFIG_LIVEPATCH` 未启用；并且 livepatch 的 Kconfig 还依赖 `DYNAMIC_FTRACE_WITH_REGS || DYNAMIC_FTRACE_WITH_ARGS`、`MODULES`、`SYSFS`、`KALLSYMS_ALL`、`!TRIM_UNUSED_KSYMS`。当前 `CONFIG_MODULES` 不满足，打开了 `LIVEPATCH` 也无法真正使用。

## 3.11 关键文件清单

- `/home/louis/code/linux/include/linux/livepatch.h`：`klp_patch`/`klp_object`/`klp_func`/`klp_state`
- `/home/louis/code/linux/kernel/livepatch/core.c`：补丁注册、对象/函数初始化、`klp_enable_patch`
- `/home/louis/code/linux/kernel/livepatch/patch.c`：ftrace 重定向、`klp_ftrace_handler`、`func_stack`
- `/home/louis/code/linux/kernel/livepatch/transition.c`：一致性模型、任务切换、完成过渡
- `/home/louis/code/linux/kernel/livepatch/state.c`：跨补丁状态
- `/home/louis/code/linux/kernel/livepatch/shadow.c`：shadow 变量
- `/home/louis/code/linux/kernel/livepatch/Kconfig`：`LIVEPATCH` 依赖
- `/home/louis/code/linux/kernel/kexec.c`：`kexec_load` 与 `do_kexec_load`
- `/home/louis/code/linux/kernel/kexec_file.c`：`kexec_file_load`、镜像探测与 `kexec_add_buffer`
- `/home/louis/code/linux/kernel/kexec_core.c`：`struct kimage` 管理、`kernel_kexec`
- `/home/louis/code/linux/kernel/kexec_elf.c`：ELF 镜像解析
- `/home/louis/code/linux/kernel/crash_core.c`：`crash_kexec`、`kexec_crash_loaded`
- `/home/louis/code/linux/include/linux/kexec.h`：`struct kimage`、kexec 接口
- `/home/louis/code/linux/Documentation/admin-guide/kdump/kdump.rst`：kdump 用户态配置
