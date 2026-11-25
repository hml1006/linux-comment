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

### 3.2.4 过渡的完成、取消与反向

`transition.c` 提供了三个“结束过渡”的入口，适用场景不同：

| 函数 | 行号 | 语义 |
| --- | --- | --- |
| `klp_try_complete_transition()` | 430 | 尝试把所有任务切到目标状态；`complete` 为真才真正完成 |
| `klp_cancel_transition()` | 156 | 初始化阶段失败（参数校验/回滚），直接清理，不切换任务 |
| `klp_reverse_transition()` | 627 | 过渡进行到一半要撤销（例如管理员写 `enabled=0`），把目标状态反转 |

`klp_try_complete_transition()` 的循环值得细读：

```c
	read_lock(&tasklist_lock);
	for_each_process_thread(g, task)
		if (!klp_try_switch_task(task))
			complete = false;
	read_unlock(&tasklist_lock);

	cpus_read_lock();
	for_each_possible_cpu(cpu) {
		task = idle_task(cpu);
		if (cpu_online(cpu)) {
			if (!klp_try_switch_task(task)) {
				complete = false;
				/* Make idle task go through the main loop. */
				wake_up_if_idle(cpu);
			}
		} else if (task->patch_state != klp_target_state) {
			clear_tsk_thread_flag(task, TIF_PATCH_PENDING);
			task->patch_state = klp_target_state;
		}
	}
	cpus_read_unlock();
```

- **在线 idle 任务**：无法切栈时必须 `wake_up_if_idle(cpu)`，让它在 idle 循环的切换点自愿更新；
- **离线 CPU 的 idle 任务**：可以直接改 `patch_state`，因为没有代码在跑；
- **未完成时**：每 `SIGNALS_TIMEOUT`（15 次）调用一次 `klp_send_signals()`，对非 kthread 发假信号（`set_notify_signal()`）、对 kthread `wake_up_state(TASK_INTERRUPTIBLE)`，逼迫它们经过调度点；同时 `schedule_delayed_work(&klp_transition_work, round_jiffies_relative(HZ))` 每秒重试。

真正完成后调用 `klp_complete_transition()`（第 81 行），它负责切 sysfs 状态、清 `TIF_PATCH_PENDING` 与 `patch_state`，并在 `!patch->enabled` 时异步释放补丁（`klp_free_patch_async`）。

反向过渡在实现上很巧妙：它并不“回滚”，而是把 `klp_target_state` 取反、清掉所有 `TIF_PATCH_PENDING`、重新走一遍 `klp_start_transition()`。这样“已切到新状态的任务”会被要求再切回旧状态，语义上等价于取消。

极端情况下可以用 sysfs 强制完成：

```c
static ssize_t force_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	...
	patch = container_of(kobj, struct klp_patch, kobj);
	if (patch != klp_transition_patch) {
		mutex_unlock(&klp_mutex);
		return -EINVAL;
	}

	klp_force_transition();
	...
}
```

`klp_force_transition()`（第 706 行）把所有任务的 `patch_state` 直接设为目标值并清 pending 标志——这是“我确定没有任务卡在被替换函数里”的人为担保，代价是失去了安全保证。

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

### 3.4.4 nop 补丁与累积补丁（atomic replace）

`klp_patch.replace = true` 的“累积补丁”模式有一个陷阱：新补丁若不再替换某个旧补丁替换过的函数，直接卸载旧补丁会导致 ftrace 回跳到原函数，但任务栈里可能还有旧补丁的帧。内核的解法是给这些“被遗弃的函数”插入 **nop 补丁**：

```c
static struct klp_func *klp_alloc_func_nop(struct klp_func *old_func,
					   struct klp_object *obj)
{
	...
	klp_init_func_early(obj, func);
	/*
	 * func->new_func is same as func->old_func. These addresses are
	 * set when the object is loaded, see klp_init_object_loaded().
	 */
	func->old_sympos = old_func->old_sympos;
	func->nop = true;
	return func;
}
```

`klp_add_nops()`（`core.c` 第 615 行）遍历所有已安装补丁的对象，为新补丁补上缺失的 `klp_func`，并置 `nop = true`。这样 `func_stack` 的栈顶始终有一个“指向原函数”的条目，`klp_ftrace_handler()` 看到 `func->nop` 时不修改 PC：

```c
	if (func->nop)
		goto unlock;                  /* NOP 占位，不修改 PC */
```

于是“旧补丁的函数栈条目被移除”不会改变实际执行流，等所有任务都完成过渡后，nop 条目也可以安全丢弃（`klp_discard_nops()`，`core.c` 第 1219 行）。

累积补丁的另一个约束在 `state.c` 的 `klp_is_patch_compatible()`：新补丁必须能处理所有旧补丁改过的系统状态，且 `state->version` 不能比旧的低——否则 `klp_enable_patch()` 返回 `-EINVAL`。

### 3.4.5 撤销一个函数的重定向

`klp_unpatch_func()`（`patch.c` 第 127-158 行）区分两种情形：

```c
	if (list_is_singular(&ops->func_stack)) {
		unsigned long ftrace_loc;

		ftrace_loc = ftrace_location((unsigned long)func->old_func);
		if (WARN_ON(!ftrace_loc))
			return;

		WARN_ON(unregister_ftrace_function(&ops->fops));
		WARN_ON(ftrace_set_filter_ip(&ops->fops, ftrace_loc, 1, 0));

		list_del_rcu(&func->stack_node);
		list_del(&ops->node);
		kfree(ops);
	} else {
		list_del_rcu(&func->stack_node);
	}
```

- 栈里只剩这一个函数：注销 `ftrace_ops` 并释放 `klp_ops`，`ftrace_set_filter_ip(..., 1, 0)` 的 `1` 表示“移除过滤项”；
- 栈里还有旧版本：只 `list_del_rcu()`，让 RCU 读写者自然过渡。

注意 `kfree(ops)` 之前没有 `synchronize_rcu()`，因为整个操作在 `klp_mutex` 下，且 `func_stack` 的遍历在 handler 中处于 RCU 读临界区；`PERMANENT` 标志保证 ftrace 不会在补丁存活期内自行回收 ops。

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

### 3.5.6 补丁模块与普通模块加载器的衔接

livepatch 补丁本身是一个 `.ko`，其 `module_init()` 调用 `klp_enable_patch()`。这带来一个顺序问题：若补丁针对某个**尚未加载**的模块，此时 `obj->mod` 为 NULL，不能立即打补丁。内核用两条通知钩子解决：

```c
int klp_module_coming(struct module *mod)
{
	if (WARN_ON(mod->state != MODULE_STATE_COMING))
		return -EINVAL;
	...
	mod->klp_alive = true;

	klp_for_each_patch(patch) {
		klp_for_each_object(patch, obj) {
			if (!klp_is_module(obj) || strcmp(obj->name, mod->name))
				continue;

			obj->mod = mod;
			...
			ret = klp_init_object_loaded(patch, obj);
			...
			ret = klp_patch_object(obj);
			...
			if (patch != klp_transition_patch)
				klp_post_patch_callback(obj);
			break;
		}
	}
	...
}
```

`klp_module_coming()` 由 `prepare_coming_module()` 在模块状态 `COMING` 时调用（见 2.5.5），因此“模块一加载就自动被打上补丁”。反之 `klp_module_going()` 在 `GOING` 时清理：

```c
void klp_module_going(struct module *mod)
{
	if (WARN_ON(mod->state != MODULE_STATE_GOING &&
		    mod->state != MODULE_STATE_COMING))
		return;

	mutex_lock(&klp_mutex);
	mod->klp_alive = false;
	klp_cleanup_module_patches_limited(mod, NULL);
	mutex_unlock(&klp_mutex);
}
```

`mod->klp_alive` 是“这个模块是否已经走过 livepatch 钩子”的标记，`struct module` 中 `#ifdef CONFIG_LIVEPATCH` 下才有。补丁模块自身在 `load_module()` 中被识别（`.modinfo` 里有 `livepatch=` 键，`set_livepatch_module()`），并触发 `TAINT_LIVEPATCH`。

### 3.5.7 系统状态共享与版本兼容

`klp_state` 让多个补丁共享“系统已改到什么程度”的信息：

```c
struct klp_state {
	unsigned long id;
	unsigned int version;
	void *data;
};
```

- `id` 是补丁作者定义的整数标识（例如某个数据结构布局的编号）；
- `version` 用于累积补丁的单调性检查；
- `data` 由补丁自行解释，通常放指针或小结构。

`klp_get_state(patch, id)` 在**当前补丁**里找；`klp_get_prev_state(id)` 只在过渡期间有效（`WARN_ON_ONCE(!klp_transition_patch)`），用于读取“上一个补丁留下的状态”。`klp_is_state_compatible()`（`state.c` 第 87-99 行）规定：

```c
	/* A cumulative livepatch must handle all already modified states. */
	if (!state)
		return !patch->replace;

	return state->version >= old_state->version;
```

即：累积补丁必须覆盖所有旧状态，且版本号不得回退。这条规则防止“新补丁不知道旧补丁改过什么”导致状态与实际代码不一致。

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

### 3.6.3 arm64 上并没有 ELF 加载器

这是本树最容易看错的一点。通用代码里确实有 `kernel/kexec_elf.c`，但它的对象由独立配置符号控制：

```make
# kernel/Makefile
obj-$(CONFIG_KEXEC_FILE) += kexec_file.o
obj-$(CONFIG_KEXEC_ELF)  += kexec_elf.o
```

而 `CONFIG_KEXEC_ELF` 只有 parisc/powerpc/loongarch/riscv 等架构 `select`；本树的 arm64 `Kconfig` 只有：

```kconfig
config ARCH_SUPPORTS_KEXEC_FILE
	def_bool y

config ARCH_SUPPORTS_KEXEC_SIG
	def_bool y
```

没有 `select KEXEC_ELF`，`.config` 里也检索不到 `CONFIG_KEXEC_ELF`。arm64 的文件式加载走的是 `arch/arm64/kernel/kexec_image.c` 与 `machine_kexec_file.c`：

| 文件 | 作用 |
| --- | --- |
| `arch/arm64/kernel/kexec_image.c` | 解析 arm64 `Image` 头（`ARM64_IMAGE_MAGIC`），把内核镜像放进 kimage |
| `arch/arm64/kernel/machine_kexec_file.c` | 生成 ELF core headers（`prepare_elf_headers`）与启动用 DTB（`of_kexec_alloc_and_setup_fdt`） |

`kexec_image.c` 的 `image_probe()` 只做两件事：

```c
	if (!h || (kernel_len < sizeof(*h)))
		return -EINVAL;

	if (memcmp(&h->magic, ARM64_IMAGE_MAGIC, sizeof(h->magic)))
		return -EINVAL;
```

因此走 `kexec_file_load()`（kexec-tools 加 `-s` 使用文件式系统调用时）时，arm64 期望的是带合法 `Image` 头的镜像（`arch/arm64/boot/Image`），而不是任意 ELF；若用 `kexec_load()` 的传统段式接口，则镜像解析完全由用户态工具完成，内核不关心格式。`kernel/kexec_elf.c` 在本树中“源码存在、当前架构不编译”。

arm64 也没有定义 `CONFIG_ARCH_SUPPORTS_KEXEC_PURGATORY`，因此 `kexec_calculate_store_digests()` 直接返回：

```c
	if (!IS_ENABLED(CONFIG_ARCH_SUPPORTS_KEXEC_PURGATORY))
		return 0;
```

以及 `kexec_file.c` 第 884-1261 行整段 purgatory 符号解析/重定位代码都不参与编译。这与 3.7 节“purgatory 负责校验、关中断、跳转”的描述需要区分：purgatory 是**可选架构能力**，不是所有架构都有。

### 3.6.4 加载前的安全闸门

两套系统调用在真正分配内存前都要过安全层。`kexec_load_check()`（`kernel/kexec.c` 第 202-240 行）：

```c
	if (!kexec_load_permitted(image_type))
		return -EPERM;

	result = security_kernel_load_data(LOADING_KEXEC_IMAGE, false);
	if (result < 0)
		return result;

	result = security_locked_down(LOCKDOWN_KEXEC);
	if (result)
		return result;

	if ((flags & KEXEC_FLAGS) != (flags & ~KEXEC_ARCH_MASK))
		return -EINVAL;

	if (nr_segments > KEXEC_SEGMENT_MAX)
		return -EINVAL;
```

`kexec_load_permitted()`（`kexec_core.c` 第 1108 行）还额外维护两个可写 sysctl 限额：

```c
	limit = (kexec_image_type == KEXEC_TYPE_CRASH) ?
		&load_limit_panic : &load_limit_reboot;
	...
	if (limit->limit != -1)
		limit->limit--;
```

即 `/proc/sys/kernel/kexec_load_limit_reboot` 与 `kexec_load_limit_panic`，默认 `-1` 表示不限；设成 N 后只允许再加载 N 次，用于“重启即失效”的加固策略。另有 `kexec_load_disabled` 一次性开关。

文件式加载的签名校验在 `kimage_file_prepare_segments()` 中、读完内核镜像之后立即进行：

```c
#ifdef CONFIG_KEXEC_SIG
	ret = kimage_validate_signature(image);
	if (ret)
		goto out;
#endif
```

`CONFIG_KEXEC_SIG` 未设置时这段不编译，镜像签名完全不检查；本树正是这种情况（`.config` 第 293 行 `# CONFIG_KEXEC_SIG is not set`）。

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

### 3.7.1 控制页链的编码

条目是 `unsigned long`，低 4 位是类型（`include/linux/kexec.h` 第 5-14 行）：

```c
#define IND_DESTINATION   (1 << 0)
#define IND_INDIRECTION   (1 << 1)
#define IND_DONE          (1 << 2)
#define IND_SOURCE        (1 << 3)
#define IND_FLAGS (IND_DESTINATION | IND_INDIRECTION | IND_DONE | IND_SOURCE)
```

对应语义：

| 条目 | 含义 |
| --- | --- |
| `IND_DONE` | 链结束 |
| `IND_INDIRECTION` | 本条目指向另一个控制页（链的嵌套） |
| `IND_DESTINATION` | 后面 `IND_SOURCE` 条目要拷贝到的物理页 |
| `IND_SOURCE` | 源页（来自 `kimage` 分配的目标页） |

`kimage_add_entry()`（`kexec_core.c` 第 483-507 行）在控制页写满时自动分配新页并写 `IND_INDIRECTION`，把链“续”到新页；`kimage_terminate()`（第 536-542 行）最后写 `IND_DONE`。遍历宏 `for_each_kimage_entry()`（第 544-547 行）在 `IND_INDIRECTION` 时跳到间接页，否则指针加一。

`machine_kexec()` 只需按架构约定把 `image->head` 的物理地址与 `image->start` 交给跳转代码；具体“把哪些页拷到哪里”由这段链自解释。`control_code_page`/`swap_page` 是给架构代码临时使用的控制页与交换页（`preserve_context` 场景下用来保存旧内核状态）。

## 3.8 从加载到重启

`kernel/kexec_core.c:1138` 的 `kernel_kexec()`：

1. 取 `kexec_image`
2. `machine_kexec_prepare()` 等架构准备
3. 停其他 CPU、关闭中断、`kernel_restart_prepare()`
4. `machine_kexec(image)` 执行跳转

用户态一般在 `kexec -e` 时调用 `reboot(LINUX_REBOOT_CMD_KEXEC)`，由内核走 `kernel_kexec()`。

### 3.8.1 kernel_kexec 的精确顺序

```c
	if (!kexec_trylock())
		return -EBUSY;
	if (!kexec_image) {
		error = -EINVAL;
		goto Unlock;
	}

	if (!kexec_image->preserve_context) {
		error = liveupdate_reboot();
		if (error)
			goto Unlock;
	}
	...
	{
		kexec_in_progress = true;
		kernel_restart_prepare("kexec reboot");
		migrate_to_reboot_cpu();
		syscore_shutdown();

		cpu_hotplug_enable();
		pr_notice("Starting new kernel\n");
		machine_shutdown();
	}

	kmsg_dump(KMSG_DUMP_SHUTDOWN);
	machine_kexec(kexec_image);
```

顺序上的几个要点：

- `kexec_trylock()` 是一把全局互斥锁，防止 `kexec_file_load` 在“跳转前”替换镜像（3.9 的 `__crash_kexec` 也会拿它）；
- `liveupdate_reboot()` 是 Live Update Orchestrator（`kernel/liveupdate/`，`CONFIG_LIVEUPDATE`）的钩子；当前配置里 `CONFIG_KEXEC_HANDOVER` 未启用、`CONFIG_LIVEUPDATE` 符号也不存在，因此它走 `include/linux/liveupdate.h` 的内联 stub 直接返回 0；
- `migrate_to_reboot_cpu()` 会把当前任务迁到重启 CPU 并禁用 CPU hotplug，所以紧接着必须 `cpu_hotplug_enable()`——因为 kexec 路径仍要使用 CPU hotplug 回调；
- `syscore_shutdown()` 与 `machine_shutdown()` 分别处理系统核心设备与架构相关设备；
- `kmsg_dump(KMSG_DUMP_SHUTDOWN)` 在跳转前把日志刷到 pstore/console；
- `machine_kexec()` 之后**正常不会返回**。

`preserve_context`（`CONFIG_KEXEC_JUMP`）是另一条分支：冻结进程、挂起设备、关闭从 CPU、`syscore_suspend()`，让新内核返回后能恢复现场。当前 `.config` 没有 `CONFIG_KEXEC_JUMP`，该路径不参与编译。

## 3.9 Kdump 关系

Kdump 不是独立机制，而是 **kexec 的 crash 模式**：

- 预留 crashkernel 内存（`crashk_res`，由 `crashkernel=` 参数或设备树确定）
- 用户态 `kexec -p vmlinux --initrd=...` 调用 `kexec_load`/`kexec_file_load`，flags 带 `KEXEC_ON_CRASH`/`KEXEC_FILE_ON_CRASH`
- 镜像存入 `kexec_crash_image`（`kexec_core.c:1034`）
- 崩溃时 `panic()` → `crash_kexec()`（`kernel/crash_core.c`）→ `machine_crash_shutdown()` → `machine_kexec(kexec_crash_image)`
- 捕获内核通过 `/proc/vmcore` 导出旧内核内存

`kexec_crash_loaded()`（`crash_core.c:108`）就是 `!!kexec_crash_image`，对应 sysfs 的 `crash_loaded`。

### 3.9.1 crashkernel 预留的解析

crash 内存不再由 `crash_core.c` 自己解析，而是拆到 `kernel/crash_reserve.c`。两个 resource 描述预留区（第 27-40 行）：

```c
struct resource crashk_res = {
	.name  = "Crash kernel",
	.start = 0,
	.end   = 0,
	.flags = IORESOURCE_BUSY | IORESOURCE_SYSTEM_RAM,
	.desc  = IORES_DESC_CRASH_KERNEL
};
struct resource crashk_low_res = { ... };
```

`crashk_low_res` 用于“低地址预留”需求（某些设备/DMA 只能访问低 4G）。解析器支持的语法（`kernel/crash_reserve.c` 第 49-55 行注释）：

```
crashkernel=ramsize-range:size[,...][@offset]
```

另有 `crashkernel=size`、`crashkernel=size,high`、`crashkernel=size@offset` 等简写。`parse_crashkernel_mem()`（第 56 行）会把系统 RAM 向上取整到 128M 再匹配区间：

```c
	total_mem = roundup(total_mem, SZ_128M);
```

因为固件会预留一部分内存，`total_mem` 往往小于真实物理内存，取整可避免“机器有 128G 却匹配不到 `crashkernel=2G-64G:256M`”这类问题。

预留完成后由 `crash_prepare_elf64_headers()`（`crash_core.c` 第 174 行）生成 ELF core headers，捕获内核启动后 `fs/proc/vmcore.c` 以 `/proc/vmcore` 暴露旧内核内存（本树 `CONFIG_PROC_VMCORE=y`，`fs/proc/Makefile` 第 35 行）。

### 3.9.2 崩溃路径的精确顺序

```c
void __noclone __crash_kexec(struct pt_regs *regs)
{
	if (kexec_trylock()) {
		if (kexec_crash_image) {
			struct pt_regs fixed_regs;

			crash_setup_regs(&fixed_regs, regs);
			crash_save_vmcoreinfo();
			machine_crash_shutdown(&fixed_regs);
			crash_cma_clear_pending_dma();
			machine_kexec(kexec_crash_image);
		}
		kexec_unlock();
	}
}
```

关键点：

- `__noclone` + `STACK_FRAME_NON_STANDARD`：崩溃时栈已经不可信，禁止编译器做尾调用/框架优化，保证回溯可用；
- `kexec_trylock()` 的必要性写在注释里：crash 内核可能被重新加载到**同一块预留内存**，若不与 `kexec_load` 互斥，另一个 CPU 可能在崩溃处理中途覆盖它；
- `crash_setup_regs()` 先把当前寄存器快照到固定结构，`crash_save_vmcoreinfo()` 再把 vmcoreinfo 写入预留区，之后才 `machine_crash_shutdown()`；
- `crash_kexec()`（第 152 行）用 `panic_try_start()` 保证只有一个 CPU 执行，结束后 `panic_reset()` 允许再次 panic；
- `kexec_should_crash()`（第 90 行）决定哪些 `panic()` 应该走 kdump：`in_interrupt()`、`!p->pid`、`is_global_init(p)`、`panic_on_oops`，且 `crash_kexec_post_notifiers` 为真时推迟到 notifier 之后。

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

## 3.12 配置复核：kexec_load 实际并未编入

3.10 的表格有一处需要按源码修正：`CONFIG_KEXEC_CORE=y` **不会**让 `kernel/kexec.c` 参与编译。`kernel/Makefile` 第 81-87 行是三个独立门控：

```make
obj-$(CONFIG_KEXEC_CORE) += kexec_core.o
obj-$(CONFIG_KEXEC)      += kexec.o
obj-$(CONFIG_KEXEC_FILE) += kexec_file.o
obj-$(CONFIG_KEXEC_ELF)  += kexec_elf.o
```

而本树 `.config` 第 286-295 行只有：

```kconfig
CONFIG_CRASH_RESERVE=y
CONFIG_VMCORE_INFO=y
CONFIG_KEXEC_CORE=y
CONFIG_KEXEC_FILE=y
# CONFIG_KEXEC_SIG is not set
CONFIG_CRASH_DUMP=y
```

没有 `CONFIG_KEXEC=y`。为什么会这样？因果链在 Kconfig 里：

```
# CONFIG_PM is not set（.config:585）
 → CONFIG_PM_SLEEP=n
 → CONFIG_PM_SLEEP_SMP=n（kernel/power/Kconfig:145）
 → CONFIG_ARCH_SUPPORTS_KEXEC=n（arch/arm64/Kconfig:1637，def_bool PM_SLEEP_SMP）
 → CONFIG_KEXEC 不可选（kernel/Kconfig.kexec:19，depends on ARCH_SUPPORTS_KEXEC）
```

因此当前内核的准确画像是：

| 系统调用 | 来源 | 当前状态 |
| --- | --- | --- |
| `kexec_load` | `kernel/kexec.c` | **不编译**，`sys_ni.c` 的 `COND_SYSCALL(kexec_load)` 返回 `-ENOSYS` |
| `kexec_file_load` | `kernel/kexec_file.c` | 编译并可用，依赖 `CONFIG_KEXEC_FILE=y` |
| `reboot(LINUX_REBOOT_CMD_KEXEC)` | `kernel/reboot.c` → `kernel_kexec()` | 可用（只对 `kexec_image` 生效） |
| `kexec_crash_image` 路径 | `kernel/crash_core.c` | 可用，`CONFIG_CRASH_DUMP=y` |
| `kexec_load` 的兼容实现 `compat_sys_kexec_load` | `kernel/kexec.c` | 同样不编译 |

`kernel/sys_ni.c` 第 89-90 行正好有：

```c
COND_SYSCALL(kexec_load);
COND_SYSCALL_COMPAT(kexec_load);
```

这与第 2 章模块系统调用是同一套机制：`COND_SYSCALL` 生成 `__weak` 的 `sys_kexec_load`，真实实现缺失时落到 `sys_ni_syscall`。所以“源码里有 `SYSCALL_DEFINE4(kexec_load, ...)`”绝不等于“当前内核能调用它”。

此外 `CONFIG_KEXEC_FILE=y` 还 `select CRYPTO_LIB_SHA256`、`select KEXEC_CORE`（`kernel/Kconfig.kexec` 第 31-40 行），这是 `kexec_calculate_store_digests()` 能编译的前提；`CONFIG_KEXEC_ELF` 在 arm64 上未被 select，见 3.6.3。

## 3.13 Livepatch 在当前配置下为何完全不可用

3.10 的表格只列出了 `CONFIG_LIVEPATCH` 未设置，实际还有一层更基础的缺失。`kernel/livepatch/Kconfig` 第 7-14 行：

```kconfig
config LIVEPATCH
	bool "Kernel Live Patching"
	depends on DYNAMIC_FTRACE_WITH_REGS || DYNAMIC_FTRACE_WITH_ARGS
	depends on MODULES
	depends on SYSFS
	depends on KALLSYMS_ALL
	depends on HAVE_LIVEPATCH
	depends on !TRIM_UNUSED_KSYMS
```

逐项对照本树 `.config`：

| 依赖 | 当前值 | 满足 |
| --- | --- | --- |
| `DYNAMIC_FTRACE_WITH_REGS` / `WITH_ARGS` | 均未设置（`CONFIG_FTRACE` 未设置，见第 3703 行） | 否 |
| `MODULES` | `# CONFIG_MODULES is not set` | 否 |
| `SYSFS` | `y`（第 3266 行） | 是 |
| `KALLSYMS_ALL` | `y`（第 271 行） | 是 |
| `HAVE_LIVEPATCH` | `y`（第 652 行） | 是 |
| `!TRIM_UNUSED_KSYMS` | `TRIM_UNUSED_KSYMS` 未设置 | 是 |

关键在第一条：`CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS=y`（第 3697 行）只是“架构支持”的能力符号，真正启用 ftrace 需要 `CONFIG_FTRACE=y`，而本树明确写了：

```
3703: # CONFIG_FTRACE is not set
```

没有 ftrace，`register_ftrace_function()`、`ftrace_set_filter_ip()`、`FTRACE_OPS_FL_IPMODIFY` 全都无从谈起，livepatch 的 `patch.c` 即使编译也只是死代码。因此“打开 `CONFIG_LIVEPATCH` 就能用”是不成立的：必须先启用 `FTRACE` 与 `FUNCTION_TRACER`/`DYNAMIC_FTRACE`，再打开 `MODULES`，最后才轮得到 `LIVEPATCH`。

`kernel/livepatch/Makefile` 只有一行聚合：

```make
obj-$(CONFIG_LIVEPATCH) += livepatch.o
livepatch-objs := core.o patch.o shadow.o state.o transition.o
```

即五个 `.o` 打包成一个 `livepatch.o` 内置对象（不是模块），补丁本身才是模块。

## 3.14 可观测性与调试

### 3.14.1 Livepatch 的 sysfs 接口

`klp_init()`（`core.c` 第 1378 行）创建根节点：

```c
static int __init klp_init(void)
{
	klp_root_kobj = kobject_create_and_add("livepatch", kernel_kobj);
	if (!klp_root_kobj)
		return -ENOMEM;

	return 0;
}
```

于是每个补丁出现在 `/sys/kernel/livepatch/<patch-name>/`：

| 属性 | 读写 | 含义 |
| --- | --- | --- |
| `enabled` | RW | 写 1 启用；写 0 触发 `__klp_disable_patch()` 或对进行中的过渡调用 `klp_reverse_transition()` |
| `transition` | RO | 当前补丁是否处于过渡（`patch == klp_transition_patch`） |
| `force` | WO | 写 1 调用 `klp_force_transition()`，强制完成 |
| `replace` | RO | 是否为累积补丁 |
| `stack_order` | RO | 在补丁链中的顺序 |
| `patched`（每个对象下） | RO | 该对象是否已打补丁 |

`enabled_store()` 里“不能重新启用已禁用补丁”的规则值得注意：

```c
	if (patch == klp_transition_patch)
		klp_reverse_transition();
	else if (!enabled)
		ret = __klp_disable_patch(patch);
	else
		ret = -EINVAL;
```

处于过渡中时，写任何值都只是“反转方向”，而不是重复启用；这避免了在过渡中途叠加第二次过渡。

### 3.14.2 Kexec 的观测点

| 位置 | 内容 |
| --- | --- |
| `/sys/kernel/kexec/loaded` | `!!kexec_image` |
| `/sys/kernel/kexec/crash_loaded` | `kexec_crash_loaded()` |
| `/sys/kernel/kexec/crash_size` | crash 预留区大小（`crash_size_show/store`，可写以扩缩） |
| `/sys/kernel/kexec_loaded` 等 | 上述属性的兼容符号链接 |
| `/proc/sys/kernel/kexec_load_disabled` | 1 表示永久禁用 kexec_load |
| `/proc/sys/kernel/kexec_load_limit_reboot` / `_panic` | 剩余可加载次数，`-1` 不限 |
| `/proc/vmcore` | 捕获内核中可见的旧内存（`CONFIG_PROC_VMCORE=y`） |

sysfs 根节点在 `kexec_core.c` 第 1352 行创建：

```c
	kexec_kobj = kobject_create_and_add("kexec", kernel_kobj);
```

随后第 1330-1342 行的 `kexec_links[]` 通过 `compat_only_sysfs_link_entry_to_kobj()` 在 `/sys/kernel/` 下建立旧名字的兼容链接：

```c
static struct kexec_link_entry kexec_links[] = {
	{ "loaded", "kexec_loaded" },
#ifdef CONFIG_CRASH_DUMP
	{ "crash_loaded", "kexec_crash_loaded" },
	{ "crash_size", "kexec_crash_size" },
	...
```

因此 `cat /sys/kernel/crash_loaded`（旧路径）与 `cat /sys/kernel/kexec/crash_loaded`（新路径）等价；写文档时建议用新路径。

### 3.14.3 典型调试步骤

```sh
# 1. 确认内核是否支持文件式加载
grep -E 'KEXEC|CRASH' /boot/config-$(uname -r)

# 2. 加载并检查
kexec -s -l /boot/Image --initrd=/boot/initrd.img --reuse-cmdline
cat /sys/kernel/kexec_loaded

# 3. 加载 crash 内核
kexec -p /boot/Image --initrd=/boot/initrd.img --reuse-cmdline
cat /sys/kernel/crash_loaded

# 4. livepatch 状态（需要 CONFIG_LIVEPATCH）
ls /sys/kernel/livepatch/
cat /sys/kernel/livepatch/<patch>/transition
```

arm64 上第 2 步必须用带 `Image` 头的镜像（见 3.6.3）；若用传统 `kexec -l`，则要求 `CONFIG_KEXEC=y`，而本树不满足，系统调用会返回 `-ENOSYS`（errno 显示为 `Function not implemented`）。

## 3.15 klp-build：从源码差异自动生成补丁

`kernel/livepatch/Kconfig` 第 22-31 行还有一个上游较新的能力：

```kconfig
config HAVE_KLP_BUILD
	bool

config KLP_BUILD
	def_bool y
	depends on LIVEPATCH && HAVE_KLP_BUILD
	select OBJTOOL
```

`KLP_BUILD` 依赖 `HAVE_KLP_BUILD`，而本树只有 x86_64 选择它：

```
arch/x86/Kconfig:265:	select HAVE_KLP_BUILD	if X86_64
```

arm64 的 `Kconfig` 中没有这一行，`.config` 里也检索不到 `CONFIG_KLP_BUILD`。因此“用 `klp-build` 对比两个 vmlinux 自动生成 livepatch 模块”这套工具链在本树（arm64）上不可用；它依赖 objtool 的 `--klp-build` 能力与 DWARF 差异分析，本质上是把“手工写 `klp_func` 表”自动化。

## 3.16 关键文件清单（补充）

- `/home/louis/code/linux/kernel/crash_reserve.c`：`crashkernel=` 解析、`crashk_res`/`crashk_low_res`
- `/home/louis/code/linux/kernel/Kconfig.kexec`：`KEXEC`、`KEXEC_FILE`、`KEXEC_ELF` 的依赖
- `/home/louis/code/linux/arch/arm64/kernel/kexec_image.c`：arm64 `Image` 格式加载器
- `/home/louis/code/linux/arch/arm64/kernel/machine_kexec_file.c`：ELF core headers 与 DTB 生成
- `/home/louis/code/linux/arch/arm64/kernel/machine_kexec.c`：`machine_kexec` 的 arm64 实现
- `/home/louis/code/linux/include/linux/liveupdate.h`：`liveupdate_reboot()` 的真实实现与 stub
- `/home/louis/code/linux/kernel/liveupdate/`：Live Update Orchestrator（当前配置未编译）
- `/home/louis/code/linux/kernel/sys_ni.c`：`COND_SYSCALL(kexec_load)` 的 ENOSYS 兜底
- `/home/louis/code/linux/fs/proc/vmcore.c`：`/proc/vmcore` 实现
- `/home/louis/code/linux/kernel/power/Kconfig`：`PM_SLEEP_SMP` → `ARCH_SUPPORTS_KEXEC` 的因果起点

## 3.17 与相邻章节的衔接

- 崩溃转储的用户态配置、`crashkernel=` 实践与 vmcore 分析，见 [../10-debug/21-kdump.md](../10-debug/21-kdump.md)。那篇从运维视角讲 kdump，本篇从源码视角讲 `kexec_file_load`/`__crash_kexec` 的实现；两篇的配置结论需要以本篇 3.12 的复核为准。
- `kexec_load` / `kexec_file_load` 的系统调用 ABI 与参数校验，见 [../02-core/syscall/module-kexec/kexec_load.md](../02-core/syscall/module-kexec/kexec_load.md) 与 [kexec_file_load.md](../02-core/syscall/module-kexec/kexec_file_load.md)。
- livepatch 补丁是模块，模块加载器如何识别 `livepatch=` 键、如何调用 `klp_module_coming()`，见 [02-module-loader.md](02-module-loader.md) 的 2.5.5 与 3.5.6。
- livepatch 依赖 ftrace 的 `IPMODIFY`，ftrace 本身见 [../10-debug/04-ftrace.md](../10-debug/04-ftrace.md)。
