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

## 2.6 vermagic 校验

`include/linux/vermagic.h` 的 `VERMAGIC_STRING` = `UTS_RELEASE` + SMP + preempt 模式 + `MODULE_UNLOAD` + `MODVERSIONS` + `MODULE_ARCH_VERMAGIC` + `MODULE_RANDSTRUCT`。

`check_modinfo()`（第 2633 行）在 `MODULE_INIT_IGNORE_VERMAGIC` 时把 `modmagic` 置 NULL；缺失则 `try_to_force_load(mod, "bad vermagic")`，不匹配则打印期望值并返回 `-ENOEXEC`。强制加载会打上 `TAINT_FORCED_MODULE`。

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
