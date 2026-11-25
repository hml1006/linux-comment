# 2. 模块加载器：从 .ko 到运行中的 struct module

## 2.1 概述

可加载模块（Loadable Kernel Module）允许在运行时把一段 ELF 可重定位对象链接进内核。入口是三个系统调用：

| 系统调用 | 源码位置 | 作用 |
| --- | --- | --- |
| `init_module(umod, len, uargs)` | `kernel/module/main.c:3634` | 从用户态缓冲区复制整个模块镜像 |
| `finit_module(fd, uargs, flags)` | `kernel/module/main.c:3799` | 从文件描述符加载，支持压缩模块与幂等加载 |
| `delete_module(name, flags)` | `kernel/module/main.c:804` | 卸载模块，可带 `O_NONBLOCK`/强制标志 |

三者最终都汇聚到 `load_module()`（`kernel/module/main.c:3422`）。

**前置条件：** 当前 `.config` 中 `# CONFIG_MODULES is not set`，而 `kernel/Makefile:61` 是 `obj-$(CONFIG_MODULES) += module/`，所以 `kernel/module/` 整个目录当前不参与编译，上述系统调用在当前内核中不存在。本文描述的是源码完整实现，属于“源码存在、当前配置未启用”。

## 2.2 模块的 ELF 布局

`.ko` 是一个 **ET_REL 可重定位 ELF**，由 `scripts/Makefile.modfinal` 用 `$(LD) -r -T scripts/module.lds` 链接生成。典型节区：

| 节区 | 内容 |
| --- | --- |
| `.text` | 常驻代码 |
| `.init.text` / `.exit.text` | 初始化/退出代码，init 段在加载完成后释放 |
| `.rodata` | 只读数据 |
| `.ro_after_init` | init 后转只读的数据 |
| `.data` / `.bss` | 可写数据 |
| `.init.data` | 初始化期数据 |
| `__ksymtab` / `__ksymtab_strings` | 模块导出的符号表与名字 |
| `__kcrctab` | 符号 CRC（`CONFIG_MODVERSIONS`） |
| `__versions` | 模块依赖的符号 CRC 列表 |
| `__param` | `struct kernel_param` 数组 |
| `.modinfo` | `MODULE_*` 宏写入的键值对，含 `vermagic` |
| `.gnu.linkonce.this_module` | 模块内的 `struct module` 模板 |
| `.note.gnu.build-id` | build ID（`.BTF`/`.BTF.base` 若启用也在此类） |

加载器只搬运带 `SHF_ALLOC` 的节区。`layout_sections()`（`kernel/module/main.c:1752`）与 `__layout_sections()`（第 1680 行）用两组掩码把节区归类：

```c
static const unsigned long masks[][2] = {
	{ SHF_EXECINSTR | SHF_ALLOC, ARCH_SHF_SMALL },   // text
	{ SHF_ALLOC, SHF_WRITE | ARCH_SHF_SMALL },       // rodata
	{ SHF_RO_AFTER_INIT | SHF_ALLOC, ARCH_SHF_SMALL },
	{ SHF_WRITE | SHF_ALLOC, ARCH_SHF_SMALL },       // data
	{ ARCH_SHF_SMALL | SHF_ALLOC, 0 },               // small data
};
```

结果写入每个节头的 `sh_entsize`：高位编码 `enum mod_mem_type`，低位编码该节在对应内存区中的偏移（`module_get_offset_and_type()`，第 1657 行）。`move_module()`（第 2788 行）随后按类型分配内存并 `memcpy` 节内容。

### 2.2.1 sh_entsize 的位域编码

这个“把类型塞进节头”的技巧定义在 `kernel/module/internal.h` 第 21-30 行：

```c
/*
 * Use highest 4 bits of sh_entsize to store the mod_mem_type of
 * this section. This leaves 28 bits for offset on 32-bit systems, which is
 * about 256 MiB (WARN_ON_ONCE if we exceed that).
 */

#define SH_ENTSIZE_TYPE_BITS	4
#define SH_ENTSIZE_TYPE_SHIFT	(BITS_PER_LONG - SH_ENTSIZE_TYPE_BITS)
#define SH_ENTSIZE_TYPE_MASK	((1UL << SH_ENTSIZE_TYPE_BITS) - 1)
#define SH_ENTSIZE_OFFSET_MASK	((1UL << (BITS_PER_LONG - SH_ENTSIZE_TYPE_BITS)) - 1)
```

`module_get_offset_and_type()`（`main.c` 第 1657-1669 行）先按当前区大小做对齐，再返回 `offset | (type << SHIFT)`；如果偏移侵入了类型位，`WARN_ON_ONCE(offset & mask)` 会报警——32 位系统上一个区超过约 256 MiB 就会触发。

`layout_sections()` 先给所有节设哨兵值（第 1756-1757 行）：

```c
	for (i = 0; i < info->hdr->e_shnum; i++)
		info->sechdrs[i].sh_entsize = ~0UL;
```

于是 `__layout_sections()` 里 `s->sh_entsize != ~0UL` 的节会被跳过，保证一个节只归入一个区。两遍扫描（core 与 init）用不同的映射表：

| 掩码序号 | 节标志条件 | core 区 | init 区 |
| --- | --- | --- | --- |
| 0 | `SHF_EXECINSTR \| SHF_ALLOC` | `MOD_TEXT` | `MOD_INIT_TEXT` |
| 1 | `SHF_ALLOC`（且不写） | `MOD_RODATA` | `MOD_INIT_RODATA` |
| 2 | `SHF_RO_AFTER_INIT \| SHF_ALLOC` | `MOD_RO_AFTER_INIT` | `MOD_INVALID`（跳过） |
| 3 | `SHF_WRITE \| SHF_ALLOC` | `MOD_DATA` | `MOD_INIT_DATA` |
| 4 | `ARCH_SHF_SMALL \| SHF_ALLOC` | `MOD_DATA` | `MOD_INIT_DATA` |

两个额外规则容易被忽略：

- `is_init != module_init_layout_section(sname)` 用于把 `.init.text`/`.exit.text` 与常驻节分开。`module_init_layout_section()`（第 1671-1678 行）在 `!CONFIG_MODULE_UNLOAD` 时把 `.exit.*` 也当作 init 节，因为模块不可卸载，退出代码没必要常驻。
- codetag 节不参与普通布局（第 1730-1738 行），它们由 `codetag_alloc_module_section()` 单独分配，因为模块卸载后这些元数据仍可能被使用。

## 2.3 关键数据结构

### 2.3.1 enum module_state

`include/linux/module.h:306`：

```c
enum module_state {
	MODULE_STATE_LIVE,	/* 正常 */
	MODULE_STATE_COMING,	/* 已成形，正在跑 module_init */
	MODULE_STATE_GOING,	/* 正在卸载 */
	MODULE_STATE_UNFORMED,	/* 仍在装载 */
};
```

### 2.3.2 enum mod_mem_type 与 module_memory

`include/linux/module.h:318` 把模块内存分成 7 类：

```c
enum mod_mem_type {
	MOD_TEXT = 0,
	MOD_DATA,
	MOD_RODATA,
	MOD_RO_AFTER_INIT,
	MOD_INIT_TEXT,
	MOD_INIT_DATA,
	MOD_INIT_RODATA,
	MOD_MEM_NUM_TYPES,
};
```

```c
struct module_memory {
	void *base;
	bool is_rox;
	unsigned int size;
#ifdef CONFIG_MODULES_TREE_LOOKUP
	struct mod_tree_node mtn;
#endif
};
```

这种拆分使 text/rodata/data 可以分别设置 NX/RO 权限，并让 init 内存可被整体释放。

### 2.3.3 struct module

`include/linux/module.h:397` 起。与加载流程强相关的字段：

```c
struct module {
	enum module_state state;
	struct list_head list;              /* 全局 modules 链表 */
	char name[MODULE_NAME_LEN];
	struct module_kobject mkobj;        /* /sys/module/<name> */
	const struct kernel_symbol *syms;   /* __ksymtab */
	const u32 *crcs;
	const u8 *flagstab;
	struct kernel_param *kp;            /* __param */
	struct module_memory mem[MOD_MEM_NUM_TYPES];
	/* ... init/exit、refcnt、source_list、taints、kallsyms ... */
};
```

`mod->init` / `mod->exit` 来自 `.gnu.linkonce.this_module` 节里的模板；加载器在 `move_module()` 时把整个 `struct module` 复制到内核分配的内存中，之后所有写入都作用于内核副本。

### 2.3.4 struct load_info

定义在 `kernel/module/internal.h`。加载期间承载 ELF 头、节头表、字符串表、节索引（`index.mod`、`index.sym`、`index.str`、`index.vers`、`index.pcpu` 等）、模块名、镜像长度、签名结果与临时 kallsyms。加载完成后 `free_copy()` 释放用户态副本。

实际结构（第 62-93 行）还有几个容易忽略的成员：

```c
struct load_info {
	const char *name;
	/* pointer to module in temporary copy, freed at end of load_module() */
	struct module *mod;
	Elf_Ehdr *hdr;
	unsigned long len;
	Elf_Shdr *sechdrs;
	char *secstrings, *strtab;
	unsigned long symoffs, stroffs, init_typeoffs, core_typeoffs;
	bool sig_ok;
#ifdef CONFIG_MODULE_DECOMPRESS
	struct page **pages;
	unsigned int max_pages;
	unsigned int used_pages;
#endif
	struct {
		unsigned int sym, str, mod, vers, info, pcpu;
		unsigned int vers_ext_crc, vers_ext_name;
	} index;
};
```

- `symoffs`/`stroffs`/`core_typeoffs`/`init_typeoffs` 是 `layout_symtab()` 计算出的“裁剪版 kallsyms”在 `MOD_DATA`/`MOD_INIT_DATA` 区中的偏移（见 2.15）。
- `sig_ok` 在验签成功后由 `module_sig_check()` 置位，最终复制到 `mod->sig_ok` 并决定是否打 `TAINT_UNSIGNED_MODULE`。
- `CONFIG_MODULE_DECOMPRESS` 下的 `pages`/`max_pages`/`used_pages` 用于“边解压边占用页”，避免把整个解压结果再拷贝一次（见 2.14）。
- `index` 里 `0` 表示“该节不存在”，这是可选节判断的统一约定。

### 2.3.5 struct module 的字段与使用点

`include/linux/module.h` 第 397 行起的 `struct module` 以 `____cacheline_aligned __randomize_layout` 结尾。按“谁写、谁读”整理关键字段：

| 字段 | 写入点 | 主要读者 |
| --- | --- | --- |
| `state` | `add_unformed_module`→`complete_formation`→`do_init_module` | `/proc/modules`、`try_module_get` |
| `list` | `add_unformed_module`（`list_add_rcu`） | `find_module`、kallsyms 遍历 |
| `name` | `layout_and_allocate` 从 `.gnu.linkonce.this_module` 复制 | sysfs、日志、`find_module` |
| `build_id` | `init_build_id()`（`kallsyms.c:225`） | `CONFIG_STACKTRACE_BUILD_ID` 栈回溯 |
| `mkobj` | `mod_sysfs_init()` | `/sys/module/<name>` |
| `syms`/`crcs`/`flagstab` | `find_module_sections()` | `find_symbol()` |
| `kp`/`num_kp` | `find_module_sections()` | `parse_args()`、sysfs `parameters/` |
| `mem[]` | `module_memory_alloc()` | `within_module_*()`、权限设置 |
| `init`/`exit` | 从 `.gnu.linkonce.this_module` 模板复制 | `do_one_initcall()`、`delete_module` |
| `source_list`/`target_list`/`refcnt` | `module_unload_init()`/`add_module_usage()` | 卸载检查、sysfs `holders/` |
| `kallsyms`/`core_kallsyms` | `add_kallsyms()`；init 完成后 `rcu_assign_pointer` 切换 | `module_address_lookup()` |
| `taints` | `module_augment_kernel_taints()` | `/proc/sys/kernel/tainted`、sysfs `taint` |

注意 `source_list`/`target_list`/`exit`/`refcnt` 都包在 `#ifdef CONFIG_MODULE_UNLOAD` 内（第 559-570 行），整个 `delete_module()` 系统调用也在 `#ifdef CONFIG_MODULE_UNLOAD`（`main.c` 第 643-977 行）里。所以“能加载但不能卸载”（`MODULES=y, MODULE_UNLOAD=n`）是一等配置，此时内核结构体本身都会瘦身。

生命周期可以用状态机串起来：

```
UNFORMED --add_unformed_module--> UNFORMED(在 modules 链表中)
   --complete_formation--> COMING
   --do_init_module 成功--> LIVE
   --delete_module/try_stop_module--> GOING
   --free_module--> UNFORMED（保留在链表中防重名，直到 RCU 宽限期后摘链）
```

`free_module()`（第 1417 行）先把状态改回 `UNFORMED`，再等 `synchronize_rcu()` 才 `list_del_rcu()`，正是为了让并发遍历 kallsyms 的读者安全。

### 2.3.6 模块内存来自 execmem

`module_memory_alloc()`（`main.c` 第 1341-1378 行）不再直接调 `module_alloc()`，而是走统一的可执行内存分配器：

```c
	if (mod_mem_type_is_data(type))
		execmem_type = EXECMEM_MODULE_DATA;
	else
		execmem_type = EXECMEM_MODULE_TEXT;

	ptr = execmem_alloc_rw(execmem_type, size);
	...
	mod->mem[type].is_rox = execmem_is_rox(execmem_type);
```

`execmem_alloc_rw()` 分配的初态是可写（RW）映射，方便 `move_module()` 直接 `memcpy`；随后由 `complete_formation()` 调 `module_enable_text_rox()` 等把 text 改成 RO+X、data 改成 NX。`is_rox` 记录架构是否直接给出了 ROX 映射，`module_memory_restore_rox()`（第 1380 行）在错误路径上把临时改写的 text 恢复成 ROX，避免留下可写代码页。

这解释了 `CONFIG_MODULES` Kconfig 里 `select EXECMEM` 的原因（`kernel/module/Kconfig` 第 2-5 行）：模块加载器依赖 execmem 提供 W^X 友好的映射。

## 2.4 从系统调用进入 load_module

### 2.4.1 init_module

```c
SYSCALL_DEFINE3(init_module, void __user *, umod,
		unsigned long, len, const char __user *, uargs)
{
	struct load_info info = { };
	err = may_init_module();          /* CAP_SYS_MODULE + modules_disabled */
	err = copy_module_from_user(umod, len, &info);
	return load_module(&info, uargs, 0);
}
```

### 2.4.2 finit_module

`finit_module` 先校验 flags 只允许 `MODULE_INIT_IGNORE_MODVERSIONS`、`MODULE_INIT_IGNORE_VERMAGIC`、`MODULE_INIT_COMPRESSED_FILE`，然后用 `CLASS(fd, f)(fd)` 管理文件引用，交给 `idempotent_init_module()`。后者通过 `idem_hash`（256 桶）与 `struct idempotent` 保证 **同一文件并发 finit_module 只真正加载一次**，其余调用等待并复用结果。

### 2.4.3 delete_module

`delete_module` 的检查顺序（`kernel/module/main.c:804`）：

1. `capable(CAP_SYS_MODULE)` 且 `!modules_disabled`
2. `find_module(name)`；`mod->source_list` 非空返回 `-EWOULDBLOCK`
3. 状态必须是 `MODULE_STATE_LIVE`；有 `init` 必须有 `exit`，否则只能 `try_force_unload()`
4. `try_stop_module()` 检查引用计数，必要时 `-EBUSY`
5. 调 `mod->exit()`、发 `MODULE_STATE_GOING` 通知、`klp_module_going()`、`ftrace_release_mod()`、`async_synchronize_full()`，最后 `free_module()`

### 2.4.4 卸载为什么要先解锁 module_mutex

`delete_module()` 在 `try_stop_module()` 成功后立刻 `mutex_unlock(&module_mutex)`（第 860 行），再调用 `mod->exit()`：

```c
	mutex_unlock(&module_mutex);
	/* Final destruction now no one is using it. */
	if (mod->exit != NULL)
		mod->exit();
	blocking_notifier_call_chain(&module_notify_list,
				     MODULE_STATE_GOING, mod);
	klp_module_going(mod);
	ftrace_release_mod(mod);

	async_synchronize_full();
```

原因是 `exit()` 与通知链的消费者可能反过来获取 `module_mutex`（例如注销设备、释放其他模块的引用），持锁调用会直接死锁。此时模块状态已是 `GOING`，`strong_try_module_get()` 会拒绝新的引用，`find_symbol()` 也不会再解析它的导出符号，因此“无锁执行 exit”是安全的。

`try_stop_module()`（第 772-785 行）的核心是 `try_release_module_ref()`：把加载时预留的 `MODULE_REF_BASE`（1）减掉，若归零说明没人引用；非零时尝试 `atomic_add_unless` 回补，失败则走 `try_force_unload()`（需要 `CONFIG_MODULE_FORCE_UNLOAD` 且 `O_TRUNC`），强制卸载会打 `TAINT_FORCED_RMMOD`。

### 2.4.5 系统调用表的 ENOSYS 入口

`CONFIG_MODULES` 关闭时，`kernel/module/main.c` 整体不编译，系统调用号并不会“消失”，而是由 `kernel/sys_ni.c` 的 `COND_SYSCALL` 提供兜底：

```c
COND_SYSCALL(kexec_load);
COND_SYSCALL_COMPAT(kexec_load);
COND_SYSCALL(init_module);
COND_SYSCALL(delete_module);
...
COND_SYSCALL(finit_module);
```

`COND_SYSCALL(init_module)` 的定义展开后是 `__weak sys_init_module`，若真实实现不存在，则 `sys_call_table` 指向 `sys_ni_syscall`，返回 `-ENOSYS`。这与 2.10 表格的结论一致，也说明“源码里写了 `SYSCALL_DEFINE3(init_module)`”不等于“当前内核有该系统调用”。

## 2.5 load_module 主流程

`load_module()`（第 3422 行）是一条带完整错误回滚标签的长流水线：

### 2.5.1 签名与 ELF 校验

```c
err = module_sig_check(info, flags);        /* 先验签，避免解析损坏模块 */
err = elf_validity_cache_copy(info, flags); /* ELF 头/节头/字符串表校验 */
err = early_mod_check(info, flags);         /* 名称、vermagic、livepatch 等 */
```

`module_sig_check()` 放在最前是刻意设计：注释说明验签只需要 `info->len`，不需要节信息，能最大限度降低损坏模块在验签前触发解析风险。

### 2.5.2 布局与分配

```c
mod = layout_and_allocate(info, flags);
```

内部顺序（约第 2920-3000 行）：

1. `rewrite_section_headers()`：把 `sh_addr` 改为内核临时副本地址
2. `check_modinfo()`：校验 `vermagic`，处理 livepatch 模块约束
3. `layout_sections()`：计算各 `mod_mem_type` 大小并把偏移写入 `sh_entsize`
4. `move_module()`：分配 text/rodata/data/init 内存并复制节内容

### 2.5.3 登记与初始化

```c
err = add_unformed_module(mod);             /* 占位，防重名 */
module_augment_kernel_taints(mod, info);    /* 专有/强制加载污染内核 */
err = percpu_modalloc(mod, info);
err = module_unload_init(mod);              /* 建立依赖关系数据结构 */
init_param_lock(mod);
err = find_module_sections(mod, info);      /* __param/__ksymtab/__kcrctab/... */
err = check_export_symbol_sections(mod);
err = setup_modinfo(mod, info);             /* version/srcversion/import_ns */
```

`find_module_sections()` 把各特殊节的地址缓存到 `struct module`：`mod->kp`、`mod->syms`、`mod->crcs`、`mod->flagstab`，以及 tracepoint、jump_label、ftrace events、BTF 等可选节。

### 2.5.4 符号解析与重定位

```c
err = simplify_symbols(mod, info);
err = apply_relocations(mod, info);
err = post_relocation(mod, info);
flush_module_icache(mod);
```

- `simplify_symbols()`（第 1530 行）：`SHN_UNDEF` 走 `resolve_symbol_wait()`（弱符号可缺失）；`SHN_LIVEPATCH` 留给 livepatch；`SHN_ABS` 不动；`SHN_COMMON` 报错并建议 `-fno-common`；普通节符号 `st_value += 该节最终 sh_addr`，percpu 符号用 `mod_percpu(mod)` 基址
- `apply_relocations()`（第 1608 行）：`SHF_RELA_LIVEPATCH` → `klp_apply_section_relocs()`；`SHT_REL` → `apply_relocate()`；`SHT_RELA` → `apply_relocate_add()`；架构实现在 `arch/*/kernel/module.c`
- `post_relocation()` 修正 `__jump_table`、`__tracepoints`、`__ftrace_events` 等需要内核地址的结构

### 2.5.5 成形与运行

```c
mod->args = strndup_user(uargs, ~0UL >> 1);
init_build_id(mod, info);
ftrace_module_init(mod);
err = complete_formation(mod, info);        /* 设置 RO/NX 权限 */
err = prepare_coming_module(mod);           /* 状态转 COMING，通知链 */
after_dashes = parse_args(mod->name, mod->args, mod->kp, mod->num_kp,
			  -32768, 32767, mod, unknown_module_param_cb);
err = mod_sysfs_setup(mod, info, mod->kp, mod->num_kp);
if (is_livepatch_module(mod))
	err = copy_module_elf(mod, info);
free_copy(info, flags);
return do_init_module(mod);
```

`parse_args()` 在模块代码可执行后解析 `key=value` 参数，`--` 之后的内容被忽略并告警。`mod_sysfs_setup()` 创建 `/sys/module/<name>/`。

### 2.5.6 do_init_module

`do_init_module()`（第 3077 行）：

1. 记录 init 段地址到 `struct mod_initfree`
2. `do_mod_ctors(mod)` 执行构造函数
3. `do_one_initcall(mod->init)` 调用 `module_init()` 注册的函数
4. 返回负值则回滚；返回正值只告警（应遵循 0/-E 约定）
5. 状态改为 `MODULE_STATE_LIVE`，发通知链、`kobject_uevent(KOBJ_ADD)`
6. `module_put(mod)` 释放初始引用，`trim_init_extable()`
7. `rcu_assign_pointer(mod->kallsyms, &mod->core_kallsyms)`
8. 把 init text/data/rodata 记录到 `init_free_list`，由 `init_free_wq` 工作队列在 `synchronize_rcu()` 后释放

`do_free_init()`（第 3040 行）用 `execmem_free()` 释放 init 内存，避免在 RCU 读者仍可能遍历 kallsyms 时直接释放。

### 2.5.7 错误回滚标签全景

`load_module()` 的尾部（第 3582-3631 行）是一串按“资源获取逆序”排列的标签，读它比读正向流程更容易理解每一步依赖了什么：

```c
 sysfs_cleanup:
	mod_sysfs_teardown(mod);
 coming_cleanup:
	mod->state = MODULE_STATE_GOING;
	module_destroy_params(mod->kp, mod->num_kp);
	blocking_notifier_call_chain(&module_notify_list, MODULE_STATE_GOING, mod);
	klp_module_going(mod);
 bug_cleanup:
	mod->state = MODULE_STATE_GOING;
	mutex_lock(&module_mutex);
	module_bug_cleanup(mod);
	mutex_unlock(&module_mutex);
 ddebug_cleanup:
	ftrace_release_mod(mod);
	synchronize_rcu();
	kfree(mod->args);
 free_arch_cleanup:
	module_arch_cleanup(mod);
 free_modinfo:
	free_modinfo(mod);
 free_unload:
	module_unload_free(mod);
 unlink_mod:
	mutex_lock(&module_mutex);
	list_del_rcu(&mod->list);
	mod_tree_remove(mod);
	wake_up_all(&module_wq);
	synchronize_rcu();
	mutex_unlock(&module_mutex);
 free_module:
	mod_stat_bump_invalid(info, flags);
	module_memory_restore_rox(mod);
	module_deallocate(mod, info);
 free_copy:
	if (!module_allocated) {
		audit_log_kern_module(info->name ? info->name : "?");
		mod_stat_bump_becoming(info, flags);
	}
	free_copy(info, flags);
	return err;
```

几个设计要点：

- **`unlink_mod` 里 `wake_up_all(&module_wq)` 在 `synchronize_rcu()` 之前**：等待同名模块的线程需要尽快被唤醒，然后再安全地摘链。
- **`free_module` 标签与 `free_module()` 函数同名但是两回事**：标签只做 `module_deallocate()`（释放 execmem 与 percpu），真正的 `free_module()` 只在卸载路径调用。
- **`module_allocated` 布尔量**区分“失败发生在分配前还是分配后”，决定是否记 `mod_stat_bump_becoming()`（become 统计）。这是 `CONFIG_MODULE_STATS` 的埋点。
- `audit_log_kern_module()` 在失败路径也会记录模块名（未知时记 `"?"`），保证审计日志不缺条目。

### 2.5.8 complete_formation 的严格权限

`complete_formation()`（第 3307-3346 行）是“成形”的最后一步，顺序不能乱：

```c
	err = verify_exported_symbols(mod);
	...
	module_bug_finalize(info->hdr, info->sechdrs, mod);
	module_cfi_finalize(info->hdr, info->sechdrs, mod);

	err = module_enable_rodata_ro(mod);
	err = module_enable_data_nx(mod);
	err = module_enable_text_rox(mod);
	...
	mod->state = MODULE_STATE_COMING;
```

- `verify_exported_symbols()` 检查导出符号是否与内核或其他模块重名（`CONFIG_MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS` 影响命名空间检查的严格度）。
- `module_enable_rodata_ro()` / `module_enable_data_nx()` / `module_enable_text_rox()` 是 `kernel/module/strict_rwx.c` 的三个入口，均以 `CONFIG_STRICT_MODULE_RWX` 为门控（`rodata` 还额外看 `rodata_enabled`）。前者调 `set_memory_ro()`，`data_nx()` 调 `set_memory_nx()`，`text_rox()` 在 strict 模式下调 `set_memory_rox()`、非 strict 模式退化为 `set_memory_x()`；若 `is_rox` 为真则直接 `execmem_restore_rox()` 恢复原映射。
- 权限收紧必须在 `state = COMING` **之前**完成，否则其他 CPU 可能在只读化过程中通过 `try_module_get()` 看到模块并执行仍在可写映射中的代码。

`module_enable_ro_after_init` 则延后到 init 完成（`module_enable_rodata_ro_after_init()`，`do_init_module` 第 3151 行），因为 `.data..ro_after_init` 在 init 期间必须可写。`strict_rwx.c` 第 111-133 行的 `ro_after_init[]` 数组正好列出三个例外节：

```c
static const char *const ro_after_init[] = {
	".data..ro_after_init",
	"__jump_table",
#ifdef CONFIG_HAVE_STATIC_CALL_INLINE
	".static_call_sites",
#endif
};
```

`module_mark_ro_after_init()`（第 135-151 行）按节名给它们补上 `SHF_RO_AFTER_INIT`，从而在 `layout_sections()` 时进入 `MOD_RO_AFTER_INIT` 区；`__jump_table` 之所以也被纳入，是因为只有指向 `__init` 代码的条目会在加载时被改写，之后即只读。

### 2.5.9 init 返回值约定与 -EEXIST

`do_init_module()` 对 `mod->init()` 的返回值有两条硬性约定（第 3105-3120 行）：

```c
	if (mod->init != NULL)
		ret = do_one_initcall(mod->init);
	if (ret < 0) {
		if (ret == -EEXIST)
			ret = -EBUSY;
		goto fail_free_freeinit;
	}
	if (ret > 0)
		pr_warn("%s: init suspiciously returned %d, it should follow 0/-E convention\n",
			mod->name, ret);
```

- 负数：加载失败，走回滚路径；`-EEXIST` 被改写成 `-EBUSY`，因为 `-EEXIST` 被 `[f]init_module()` 保留给“同名模块已存在”这一用户态可识别的语义。
- 正数：只告警，模块仍被视为加载成功（历史兼容）。
- `do_one_initcall()` 本身会把 initcall 放进 `initcall_debug` 计时、`async` 调度与 `kprobe` 可观测路径中。

## 2.6 vermagic 校验

`include/linux/vermagic.h` 的 `VERMAGIC_STRING` = `UTS_RELEASE` + SMP + preempt 模式 + `MODULE_UNLOAD` + `MODVERSIONS` + `MODULE_ARCH_VERMAGIC` + `MODULE_RANDSTRUCT`。

`check_modinfo()`（第 2633 行）在 `MODULE_INIT_IGNORE_VERMAGIC` 时把 `modmagic` 置 NULL；缺失则 `try_to_force_load(mod, "bad vermagic")`，不匹配则打印期望值并返回 `-ENOEXEC`。强制加载会打上 `TAINT_FORCED_MODULE`。

`same_magic()`（`kernel/module/version.c` 第 97-105 行）还有一个细节：当模块带 CRC（`has_crcs`）时，只比较 `vermagic` 中第一个空格之后的部分，也就是忽略 `UTS_RELEASE`：

```c
int same_magic(const char *amagic, const char *bmagic, bool has_crcs)
{
	if (has_crcs) {
		amagic += strcspn(amagic, " ");
		bmagic += strcspn(bmagic, " ");
	}
	return strcmp(amagic, bmagic) == 0;
}
```

这允许“同一内核版本、不同小版本”的模块在启用 `MODVERSIONS` 时复用，只要符号 CRC 全部匹配——CRC 才是真正的 ABI 契约。

### 2.6.1 污染标志（taint）

`check_modinfo()` 之后没有直接设置 taint，而是由 `module_augment_kernel_taints()`（第 2570-2631 行）集中处理。它读取 `.modinfo` 中的键：

| modinfo 键 | 条件 | taint（字母见 `kernel/panic.c:808` 的 `taint_flags[]`） |
| --- | --- | --- |
| `license` | 非 GPL 兼容 | `TAINT_PROPRIETARY_MODULE`（`P`） |
| `livepatch` | 模块标为 livepatch | `TAINT_LIVEPATCH`（`K`） |
| `test` | 测试模块 | `TAINT_TEST`（`N`） |
| 签名缺失（`CONFIG_MODULE_SIG`） | `!mod->sig_ok` | `TAINT_UNSIGNED_MODULE`（`E`） |
| 强制加载 | `try_to_force_load()` | `TAINT_FORCED_MODULE`（`F`） |
| 非 in-tree 模块 | 加载时由 `add_taint_module()` 外的地方判定 | `TAINT_OOT_MODULE`（`O`） |

特殊名字 `ndiswrapper` 与 `module_license_offenders[]` 里的模块（第 2619-2626 行）会被直接打上专有模块标记，即使它们自身声明 GPL——因为这些模块以包装专有驱动著称。`add_taint_module()` 同时把标记记到 `mod->taints`，sysfs 的 `taint` 属性和 `/proc/modules` 的括号后缀（如 `(POE)` = 专有 + 树外 + 未签名）都从这里来。

## 2.7 符号版本（MODVERSIONS）

`kernel/module/version.c` 的 `check_version()`：导出方无 CRC 时返回 1（内核已被污染）；有扩展节 `__version_ext_crcs`/`__version_ext_names` 时按名字匹配；否则遍历 `__versions` 中的 `struct modversion_info`；完全不匹配时 `try_to_force_load()`。CRC 由 `genksyms` 或 `CONFIG_GENDWARFKSYMS` 生成。

## 2.8 模块签名与强制加载

### 2.8.1 签名格式

签名附加在模块文件末尾，最后是 `struct module_signature`（`include/uapi/linux/module_signature.h:31`）：

```c
struct module_signature {
	__u8  algo;
	__u8  hash;
	__u8  id_type;
	__u8  signer_len;
	__u8  key_id_len;
	__u8  __pad[3];
	__be32 sig_len;
};
```

`mod_check_sig()`（`kernel/module_signature.c`）校验：

- `sig_len` 不越界
- `id_type == MODULE_SIGNATURE_TYPE_PKCS7`
- 其余字段必须为 0（PKCS#7 容器自带算法信息）

### 2.8.2 验签流程

`mod_verify_sig()`（`kernel/module/signing.c:43`）从镜像尾部复制 `module_signature`、调用 `mod_check_sig()`、截断 `modlen -= sig_len + sizeof(ms)`，再调用 `verify_pkcs7_signature(..., VERIFY_USE_SECONDARY_KEYRING, VERIFYING_MODULE_SIGNATURE, ...)`。

`module_sig_check()`（第 70 行）：若尾部有 `MODULE_SIGNATURE_MARKER` 且未被 `MODULE_INIT_IGNORE_*` 破坏，则验签并设 `info->sig_ok = true`；未签名/缺 key/加密不支持时，按 `sig_enforce`（`CONFIG_MODULE_SIG_FORCE` 或 `module.sig_enforce=`）决定返回 `-EKEYREJECTED` 还是走 `security_locked_down(LOCKDOWN_MODULE_SIGNATURE)`；其他错误（内存不足、签名格式错误、验签失败）一律致命。被 `IGNORE_MODVERSIONS`/`IGNORE_VERMAGIC` 处理过的模块不再等于被签名镜像，因此跳过验签。

### 2.8.3 lockdown 与 LSM

`module_sig_check()` 最后一步不是 `return 0`，而是把决定权交给安全层：

```c
	if (is_module_sig_enforced()) {
		pr_notice("Loading of %s is rejected\n", reason);
		return -EKEYREJECTED;
	}

	return security_locked_down(LOCKDOWN_MODULE_SIGNATURE);
```

也就是说，非强制模式下“未签名模块”是否允许加载，取决于内核是否处于 lockdown 状态（`security_locked_down()` 会查询 LSM 钩子）。在 integrity lockdown 下，即使 `CONFIG_MODULE_SIG_FORCE=n`，加载未签名模块也会失败（`-EPERM`）。

此外还有两个前置 LSM 钩子，位于 `copy_module_from_user()` 与 `init_module_from_file()`：

- `security_kernel_load_data(LOADING_MODULE, true)`：在**复制镜像前**检查（可阻止读取）；
- `security_kernel_post_load_data(..., LOADING_MODULE, "init_module")`：复制完成后检查内容（IMA 度量/评估）；
- `security_kernel_read_file()` 在 `finit_module` 路径由 `kernel_read_file()` 内部调用。

`kernel/module/signing.c` 顶部的 `#undef MODULE_PARAM_PREFIX` + `#define MODULE_PARAM_PREFIX "module."` 也很关键：它让 `sig_enforce` 暴露为 `/sys/module/module/parameters/sig_enforce`，而不是挂在某个具体模块下。

### 2.8.4 签名相关配置

`kernel/module/Kconfig` 第 252-338 行定义了一组相互关联的选项：

| 配置 | 作用 |
| --- | --- |
| `MODULE_SIG` | 启用模块签名校验框架 |
| `MODULE_SIG_FORCE` | 无有效签名即拒绝加载 |
| `MODULE_SIG_ALL` | `make modules_install` 时自动签名所有模块 |
| `MODULE_SIG_SHA256/384/512/SHA3_*` | 签名摘要算法 |
| `MODULE_SIG_HASH` | 实际选中的算法名（内部符号） |

`CONFIG_MODULE_SIG` 关闭时，`module_sig_check()` 仍有实现，但 `sig_enforce` 恒为 `false`，`module_signature` 校验不会把未签名模块视为错误；`struct module` 里的 `sig_ok` 字段也只在 `#ifdef CONFIG_MODULE_SIG` 下存在。

## 2.9 模块参数与 sysfs

### 2.9.1 kernel_param

`include/linux/moduleparam.h:82`：

```c
struct kernel_param {
	const char *name;
	struct module *mod;
	const struct kernel_param_ops *ops;
	const u16 perm;
	s8 level;
	u8 flags;
	union {
		void *arg;
		const struct kparam_string *str;
		const struct kparam_array *arr;
	};
};
```

`module_param()` / `module_param_array()` / `module_param_cb()` 把条目放进 `__param` 节；`MODULE_PARM_DESC()` 进入 `.modinfo`。`find_module_sections()` 把 `__param` 映射到 `mod->kp`，`parse_args()` 用 `ops->set` 写入。

### 2.9.2 /sys/module 布局

`mod_sysfs_setup()`（`kernel/module/sysfs.c:364`）创建：

```
/sys/module/<name>/
├── parameters/<param>     # 权限来自 kernel_param.perm
├── sections/<section>     # CONFIG_KALLSYMS 下显示地址
├── holders/<user>         # 依赖本模块的模块
├── notes/                 # .note.* 段（build-id）
├── refcnt
├── coresize / initsize
├── taint
├── version / srcversion
└── ...
```

`struct module_attribute`（`include/linux/module.h:54`）提供 `show/store/setup/test/free` 六个回调，`MODINFO_ATTR(version)`、`MODINFO_ATTR(srcversion)` 在 `main.c` 中由宏批量生成。

`mod_sysfs_setup()`（`sysfs.c` 第 364-417 行）的创建顺序与回滚顺序严格对称：

```
mod_sysfs_init()                 → /sys/module/<name>
 → holders/                      → kobject_create_and_add("holders")
 → module_param_sysfs_setup()    → parameters/*
 → module_add_modinfo_attrs()    → version/srcversion/import_ns 等
 → add_usage_links()             → holders/<user> 符号链接
 → add_sect_attrs()              → sections/*
 → add_notes_attrs()             → notes/*
```

失败路径从 `out_unreg_sect_attrs` 依次回退到 `out_unreg`，每一步只撤销已经成功的部分；`mod_sysfs_teardown()`（第 426 行）则是正常卸载时的逆序。`mod_sysfs_init()` 里先用 `kset_find_obj(module_kset, mod->name)` 检查同名 kobject，是“模块名唯一”的第二道保险（第一道在 `add_unformed_module()`）。

### 2.9.3 参数解析的真实实现

`parse_args()` 并不在 `kernel/module/` 下，而在 `kernel/params.c` 第 161-212 行。核心循环：

```c
	while (*args) {
		int ret;
		int irq_was_disabled;

		args = next_arg(args, &param, &val);
		/* Stop at -- */
		if (!val && strcmp(param, "--") == 0)
			return err ?: args;
		irq_was_disabled = irqs_disabled();
		ret = parse_one(param, val, doing, params, num,
				min_level, max_level, arg, unknown);
		if (irq_was_disabled && !irqs_disabled())
			pr_warn("%s: option '%s' enabled irq's!\n",
				doing, param);
		...
	}
```

几个行为细节：

- `next_arg()` 支持 `key=value` 与裸 `key`（value 为 NULL，表示布尔真或 `key` 形式的位置参数）。
- `--` 之后的内容不解析，原样返回给调用者；`load_module()` 会打印 `parameters '%s' after '--' ignored`。
- `irq_was_disabled` 检查：如果某参数 `set()` 意外打开了中断，打印警告——这是内核参数代码的防御性检查。
- `parse_one()` 先按名字在 `__param` 数组里线性查找，找不到再交给 `unknown` 回调；模块的 `unknown_module_param_cb()` 专门识别 `async_probe=` 与 `dyndbg=`。

参数类型由 `STANDARD_PARAM_DEF` 宏批量生成（第 214-242 行），`param_ops_byte/short/int/uint/ulong/hexint...` 的 `set`/`get` 都是 `kstrto*` 与 `scnprintf` 的薄包装。`param_set_uint_minmax()`（第 244 行）则是 `module_param_range` 之类的底层实现。

## 2.10 当前配置下的可见性

| 项 | 源码 | 当前 `.config` |
| --- | --- | --- |
| `kernel/module/main.c` | 存在，含全部三个 syscall | `CONFIG_MODULES` 未启用，整个目录不编译 |
| `kernel/module/signing.c` | 存在 | 依赖 `CONFIG_MODULE_SIG`，未启用 |
| `kernel/module/version.c` | 存在 | 依赖 `CONFIG_MODVERSIONS`，未启用 |
| `kernel/module/sysfs.c` | 存在 | 依赖 `CONFIG_SYSFS`，但目录本身受 `CONFIG_MODULES` 门控 |

因此在当前内核上执行 `init_module`/`finit_module`/`delete_module` 会得到 `ENOSYS`，而不是源码里的错误码。分析模块行为必须在启用 `CONFIG_MODULES`（以及按需 `CONFIG_MODULE_UNLOAD`、`CONFIG_MODULE_SIG`、`CONFIG_MODVERSIONS`）的配置下进行。

## 2.11 关键文件清单

- `/home/louis/code/linux/kernel/module/main.c`：`load_module`、`do_init_module`、三个 syscall、布局/重定位
- `/home/louis/code/linux/kernel/module/internal.h`：`struct load_info` 与内部接口
- `/home/louis/code/linux/kernel/module/signing.c`：`module_sig_check`、`mod_verify_sig`
- `/home/louis/code/linux/kernel/module_signature.c`：`mod_check_sig`
- `/home/louis/code/linux/kernel/module/version.c`：`check_version`
- `/home/louis/code/linux/kernel/module/sysfs.c`：`mod_sysfs_setup` 与 sections/holders
- `/home/louis/code/linux/kernel/module/kallsyms.c`：模块 kallsyms
- `/home/louis/code/linux/kernel/module/tree_lookup.c`：模块地址区间树
- `/home/louis/code/linux/kernel/module/procfs.c`：`/proc/modules`
- `/home/louis/code/linux/include/linux/module.h`：`struct module`、`module_state`、`module_attribute`
- `/home/louis/code/linux/include/linux/moduleparam.h`：`kernel_param` 与参数宏
- `/home/louis/code/linux/include/linux/vermagic.h`：`VERMAGIC_STRING`
- `/home/louis/code/linux/include/uapi/linux/module_signature.h`：签名结构
- `/home/louis/code/linux/include/uapi/linux/module.h`：`MODULE_INIT_*` 标志
- `/home/louis/code/linux/scripts/Makefile.modfinal`：`.ko` 链接
- `/home/louis/code/linux/scripts/Makefile.modpost`：modpost 阶段

## 2.12 request_module 与 modprobe

内核自己不会解析 `modules.dep`，模块自动加载完全交给用户态。`kernel/module/kmod.c` 的 `__request_module()`（第 132-179 行）是内核侧唯一入口：

```c
int __request_module(bool wait, const char *fmt, ...)
{
	...
	if (!modprobe_path[0])
		return -ENOENT;
	...
	ret = security_kernel_module_request(module_name);
	if (ret)
		return ret;

	ret = down_timeout(&kmod_concurrent_max, MAX_KMOD_ALL_BUSY_TIMEOUT * HZ);
	if (ret) {
		pr_warn_ratelimited("request_module: modprobe %s cannot be processed, kmod busy with %d threads for more than %d seconds now", ...);
		return ret;
	}

	trace_module_request(module_name, wait, _RET_IP_);

	if (kmod_dup_request_exists_wait(module_name, wait, &dup_ret)) {
		ret = dup_ret;
		goto out;
	}

	ret = call_modprobe(module_name, wait ? UMH_WAIT_PROC : UMH_WAIT_EXEC);
out:
	up(&kmod_concurrent_max);
	return ret;
}
```

关键点：

| 机制 | 代码 | 说明 |
| --- | --- | --- |
| `modprobe_path` | 第 64 行，`= CONFIG_MODPROBE_PATH` | 可通过 `/proc/sys/kernel/modprobe` 改写（`main.c` 的 sysctl 表） |
| 并发上限 | `MAX_KMOD_CONCURRENT 50`、`DEFINE_SEMAPHORE(kmod_concurrent_max, 50)` | 防止 modprobe 风暴，超时 5 秒（`MAX_KMOD_ALL_BUSY_TIMEOUT`） |
| LSM 钩子 | `security_kernel_module_request()` | 允许 LSM 拒绝自动加载（如 lockdown） |
| 死锁防护 | `WARN_ON_ONCE(wait && current_is_async())` | 异步 worker 里同步等模块会与 `async_synchronize_full()` 互锁 |
| 参数 | `call_modprobe()` 第 92-96 行 | `argv = { modprobe_path, "-q", "--", module_name, NULL }` |

`request_module()` 只是 `__request_module(true, ...)` 的宏包装；`request_module_nowait()` 对应 `wait=false`。它们最终都调用 `call_usermodehelper_exec()`，与 `init`、`uevent` 共用同一套内核启动用户态程序的机制。

## 2.13 幂等加载与重复请求

模块加载有两条独立的“去重”机制，容易混淆：

| 机制 | 位置 | 作用域 | 目的 |
| --- | --- | --- | --- |
| `idem_hash` | `main.c` 第 3657-3717 行 | 同一 `file_inode` 的并发 `finit_module` | 只真正加载一次，其余等待复用结果 |
| `dup_kmod_reqs` | `dups.c` 第 44-119 行 | 同名模块的并发 `request_module` | 抑制重复 modprobe，减少内存与延迟 |
| `modules` 链表 + `module_wq` | `add_unformed_module()` | 内核全局模块名 | 保证同一时刻只有一个同名模块在装载 |

`finit_module` 的幂等实现细节：

```c
struct idempotent {
	const void *cookie;
	struct hlist_node entry;
	struct completion complete;
	int ret;
};

#define IDEM_HASH_BITS 8
static struct hlist_head idem_hash[1 << IDEM_HASH_BITS];
```

`idempotent(&idem, file_inode(f))` 返回 `true` 表示“已有人在做”，于是走 `idempotent_wait_for_completion()`；第一个调用者在 `init_module_from_file()` 完成后调用 `idempotent_complete()`，遍历同一 hash 桶、把结果分发给所有等待者并 `complete()`。`ret` 初值为 `-EINTR`，保证被信号打断的等待者拿到的返回值仍然合法。

`dups.c` 的 `kmod_dup_request_exists_wait()` 则只对 `request_module()` 生效，且需要 `CONFIG_MODULE_DEBUG_AUTOLOAD_DUPS`；它的主要价值是诊断“用户态 modprobe 并发调用 finit_module 造成的虚拟内存峰值”，见 `kernel/module/Kconfig` 第 63-111 行的帮助文本。

`add_unformed_module()` 是最后一道防线：`module_patient_check_exists()`（第 3242-3279 行）在发现同名模块处于 `COMING`/`UNFORMED` 时，会释放 `module_mutex` 并 `wait_event_interruptible(module_wq, finished_loading(name))`，等对方结束再决定返回 `-EEXIST` 还是 `-EBUSY`。三者叠加后，`insmod` 并发同名模块不会导致两份代码同时驻留。

## 2.14 压缩模块

`finit_module` 的 `MODULE_INIT_COMPRESSED_FILE` 标志允许内核直接读 `.ko.gz/.ko.xz/.ko.zst`，由 `kernel/module/decompress.c` 处理：

```
finit_module(fd, uargs, MODULE_INIT_COMPRESSED_FILE)
 → idempotent_init_module()
 → init_module_from_file()
 → kernel_read_file(..., READING_MODULE_COMPRESSED)
 → module_decompress(&info, buf, len)         /* gzip / xz / zstd */
 → security_kernel_post_read_file(..., READING_MODULE)  /* 解压后再做 IMA */
 → load_module()
```

压缩格式由文件头魔数识别，三种后端分别是 `module_gzip_decompress()`（第 85 行）、`module_xz_decompress()`（第 149 行）、`module_zstd_decompress()`（第 207 行）。它们共用一个“按需扩页”的缓冲管理：

```c
static struct page *module_get_next_page(struct load_info *info)
{
	struct page *page;
	int error;

	if (info->max_pages == info->used_pages) {
		error = module_extend_max_pages(info, info->used_pages);
		if (error)
			return ERR_PTR(error);
	}

	page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM);
	if (!page)
		return ERR_PTR(-ENOMEM);

	info->pages[info->used_pages++] = page;
	return page;
}
```

`module_extend_max_pages()` 按 2 的幂扩容 `info->pages` 数组，`used_pages` 记录真正用到的页。这样解压时不需要先知道解压后大小，也不会为最坏情况预留全部内存。`free_copy()`（第 2523 行）会根据 flags 调 `module_decompress_cleanup()` 而不是 `vfree(info->hdr)`，因为此时镜像是多页拼接而非单一 vmalloc 区。

注意 `security_kernel_post_read_file()` 在解压**之后**才调用，且传入 `READING_MODULE`（而非 `READING_MODULE_COMPRESSED`），所以 IMA 度量的是解压后的 ELF，而不是压缩字节流。

## 2.15 模块 kallsyms：两套符号表

模块加载时有两份符号表，切换时机是 init 完成：

| 表 | 位置 | 生命周期 | 内容 |
| --- | --- | --- | --- |
| `mod->kallsyms`（init 表） | `MOD_INIT_DATA` | 到 init 结束 | 完整 `.symtab`/`.strtab` |
| `mod->core_kallsyms` | `MOD_DATA` | 模块常驻期 | 只保留 core 符号 |

`layout_symtab()`（`kallsyms.c` 第 113-163 行）在 `layout_sections()` 之后、`move_module()` 之前运行，它把两个表所需空间“预订”到对应内存区：

```c
	/* Append room for core symbols at end of core part. */
	info->symoffs = ALIGN(mod_mem_data->size, symsect->sh_addralign ?: 1);
	info->stroffs = mod_mem_data->size = info->symoffs + ndst * sizeof(Elf_Sym);
	mod_mem_data->size += strtab_size;
	info->core_typeoffs = mod_mem_data->size;
	mod_mem_data->size += ndst * sizeof(char);
```

`ndst` 是“core 符号”的数量，由 `is_core_symbol()` 判定；livepatch 模块特殊，保留**全部**符号（`is_livepatch_module(mod)`），因为补丁模块需要按名字解析旧函数。

`add_kallsyms()`（第 170-222 行）在 `post_relocation()` 中调用，负责填充两份表：

```c
	mod->core_kallsyms.symtab = dst = data_base + info->symoffs;
	mod->core_kallsyms.strtab = s = data_base + info->stroffs;
	mod->core_kallsyms.typetab = data_base + info->core_typeoffs;
	...
	/* Set up to point into init section. */
	rcu_assign_pointer(mod->kallsyms, kallsyms);
```

`typetab` 每项一个字符（`elf_type()` 把 `STT_FUNC` 等映射成 `t/T/d/b/r`），供 `%pS`/`%ps` 打印时标注类型。init 结束时 `do_init_module()` 执行：

```c
	rcu_assign_pointer(mod->kallsyms, &mod->core_kallsyms);
```

之后 `module_address_lookup()`、`lookup_module_symbol_name()`、`module_get_kallsym()` 都只看到裁剪版表，`/proc/kallsyms` 中模块符号的数量也随之减少。

## 2.16 依赖关系图与卸载顺序

`CONFIG_MODULE_UNLOAD` 打开时，每个跨模块符号引用都会建立一条 `struct module_use` 双向边（`internal.h` 第 114-118 行）：

```c
struct module_use {
	struct list_head source_list;
	struct list_head target_list;
	struct module *source, *target;
};
```

`add_module_usage(a, b)`（第 690-705 行）把 `use->source_list` 挂到 `b->source_list`、`use->target_list` 挂到 `a->target_list`，于是：

- 从 A 出发沿 `target_list` 可以列出“A 依赖谁”（`module_unload_free()` 用它逐个 `module_put`）；
- 从 B 出发沿 `source_list` 可以列出“谁依赖我”（`delete_module()` 用它判断 `-EWOULDBLOCK`，sysfs `holders/` 也用它生成符号链接）。

`already_uses()`（第 669-680 行）避免同一条边重复添加；`ref_module()`（第 707-725 行）在建立边之前先 `strong_try_module_get(b)`，确保目标模块不会被并发卸载。

卸载顺序因此是**依赖图的拓扑逆序**：

```
rmmod A            # A 依赖 B
 → A->source_list 非空？ 否
 → A->target_list 中的 B 逐个 module_put()
 → module_unload_free(A) 删除所有 module_use
 → 若 B 的 refcnt 归零，rmmod B 才能成功
```

`/proc/modules` 的每一行正是这张图的文本投影（`procfs.c` 第 106-111 行注释）：

```
modulename size refcount deps address
```

其中 `deps` 来自 `mod->target_list`，`refcount` 来自 `module_refcount()`。`/proc/modules` 由 `kernel/module/procfs.c` 末尾的 `module_init(proc_modules_init)` 注册，因此 `CONFIG_MODULES=n` 时该目录不编译，`/proc/modules` 文件本身也不存在——这是判断“运行内核是否支持模块”最快的方法。与之配套的 `/sys/module/` 同理。

## 2.17 当前配置逐项核对

`grep 'CONFIG_MODULE' .config` 在本树只返回两行：

```
730:CONFIG_MODULES_USE_ELF_RELA=y
788:# CONFIG_MODULES is not set
```

- `CONFIG_MODULES_USE_ELF_RELA=y` 不是“模块功能”，而是架构能力：`arch/arm64/Kconfig:240` 无条件 `select MODULES_USE_ELF_RELA`。它决定模块加载器用 `SHT_RELA`（显式加数）还是 `SHT_REL`，arm64 是前者。
- 其余 `MODULE_*` / `MODVERSIONS` / `MODULE_SIG*` 选项在 `CONFIG_MODULES=n` 时根本不会出现在 `.config` 中，因为 `kernel/module/Kconfig` 第 23 行 `if MODULES` 把它们全部包住。

因此对当前内核的准确结论是：

| 能力 | 状态 |
| --- | --- |
| `init_module` / `finit_module` / `delete_module` | 不存在，`sys_ni.c` 返回 `-ENOSYS` |
| `request_module()` | `include/linux/kmod.h` 第 27-28 行的内联 stub，返回 `-ENOSYS` |
| `/sys/module/*`、`/proc/modules` | 不存在（`kernel/module/` 整个目录不编译） |
| 模块签名、MODVERSIONS | 相关代码不参与编译 |
| 内置的“模块”元数据 | 仍存在：`modules.builtin`、`modules.builtin.modinfo` 由 modpost 生成 |

这与 [../02-core/syscall/module-kexec/init_module.md](../02-core/syscall/module-kexec/init_module.md) 中“系统调用入口”的描述需要合起来读：那边讲系统调用号与 ABI，这边讲实现；当前配置下前者只会走到 `sys_ni_syscall`。同一目录下的 [finit_module.md](../02-core/syscall/module-kexec/finit_module.md)、[delete_module.md](../02-core/syscall/module-kexec/delete_module.md) 亦然。

## 2.18 关键文件清单（补充）

- `/home/louis/code/linux/kernel/module/strict_rwx.c`：`module_enable_text_rox` 等权限收紧
- `/home/louis/code/linux/kernel/module/decompress.c`：gzip/xz/zstd 解压与按页缓冲
- `/home/louis/code/linux/kernel/module/dups.c`：`request_module` 重复请求抑制
- `/home/louis/code/linux/kernel/module/kmod.c`：`modprobe_path`、`__request_module`、`call_modprobe`
- `/home/louis/code/linux/kernel/module/stats.c`：`CONFIG_MODULE_STATS` 统计
- `/home/louis/code/linux/kernel/module/tracking.c`：加载失败与 tainted 模块记录
- `/home/louis/code/linux/kernel/module/tree_lookup.c`：模块地址区间树（`mod_tree`）
- `/home/louis/code/linux/kernel/params.c`：`parse_args`、`param_ops_*`、`kernel_param_lock`
- `/home/louis/code/linux/kernel/panic.c`：`taint_flags[]` 字母表
- `/home/louis/code/linux/kernel/sys_ni.c`：`COND_SYSCALL(init_module)` 等 ENOSYS 兜底
- `/home/louis/code/linux/kernel/module/Kconfig`：`MODULES` 及其子选项的依赖
