# LSM 框架（Linux Security Module）

> 源码基线：Linux 7.2（Makefile `VERSION = 7 / PATCHLEVEL = 2`）。本文只讨论 LSM 框架本身（`security/security.c`、`security/lsm_init.c`、`security/lsm_syscalls.c`、`include/linux/lsm_hook_defs.h` 等），不展开各具体模块的策略实现。
>
> **配置前提**：当前 `.config` 中被刻意裁剪，`CONFIG_SECURITY` 未启用（`CONFIG_CRYPTO`、`CONFIG_KEYS` 同样未启用）。因此本文所描述的全部代码路径、hook 与 LSM 模块在当前构建里**已被编译剔除**，`security/` 目录不参与编译。这不是“内核 7.2 没有 LSM”，而是本地调试配置关闭了它。下文凡涉及运行时效果的地方都会明确标注。

## 1. 概述

LSM 是内核里统一的强制访问控制（MAC）挂接框架。其设计目标是在 VFS、任务管理、网络、IPC、BPF 等关键路径上插入一组固定的“安全检查点”（hook），由各安全模块（SELinux、AppArmor、Smack、Tomoyo、Yama、LoadPin、Lockdown、Integrity/IMA/EVM、BPF LSM、Landlock、IPE 等）实现，权限判定的最终结论取所有模块中最严格的那个。

框架的三层结构：

```
文件系统/系统调用/网络路径
        │  security_inode_permission() / security_file_open() / security_task_*()
        ▼
security/security.c   —— 统一入口 + hook 调用宏 + blob 分配
        │  static call（每 hook 一个 MAX_LSM_COUNT 大小的 static_call 数组）
        ▼
security/selinux、security/apparmor、security/smack、security/tomoyo、
security/yama、security/loadpin、security/lockdown、security/integrity、
security/landlock、security/ipe、security/bpf、security/commoncap
```

关键文件规模（当前源码）：

| 文件 | 行数 | 作用 |
|------|------|------|
| `security/security.c` | 5718 | 所有 `security_*()` 入口、hook 调用宏、blob 分配 |
| `security/lsm_init.c` | 568 | LSM 排序、blob 尺寸汇总、初始化与 initcall 分发 |
| `security/lsm_syscalls.c` | 129 | `lsm_get_self_attr` / `lsm_set_self_attr` / `lsm_list_modules` |
| `include/linux/lsm_hooks.h` | 219 | 框架核心结构体 |
| `include/linux/lsm_hook_defs.h` | — | 279 条 `LSM_HOOK()` 声明，唯一的 hook 清单来源 |
| `include/linux/lsm_count.h` | — | 编译期计算 `MAX_LSM_COUNT` |

注意 `security.c` 的入口函数只是薄封装，例如：

```c
int security_inode_permission(struct inode *inode, int mask)
{
	if (unlikely(IS_PRIVATE(inode)))
		return 0;
	return call_int_hook(inode_permission, inode, mask);
}
```

`IS_PRIVATE()`（`S_PRIVATE`，用于 anon inode 等内部对象）直接放行，这是框架层唯一的通用短路。

## 2. 核心数据结构

### 2.1 hook 声明：`lsm_hook_defs.h`

所有 hook 只有一处声明，形如：

```c
LSM_HOOK(<返回类型>, <默认返回值>, <hook 名>, <参数...>)
```

例如 `LSM_HOOK(int, 0, inode_permission, struct inode *inode, int mask)`。默认返回值表示“该模块不关心此 hook，视为放行”，`int` 型通常为 0，也有 `-ENOPARAM`（`fs_context_parse_param`）这类特例；`void` 型写作 `LSM_RET_VOID`。

这个宏被多处 `#include` 复用，从而保证“声明、静态调用表、结构体成员、默认值常量”天然一致：

- `include/linux/lsm_hooks.h`：`union security_list_options` 成员、`struct lsm_static_calls_table` 数组；
- `security/security.c`：定义 `DEFINE_STATIC_CALL_NULL` 与 static key；
- `security/lsm_init.c`：`lsm_static_call_init()` 填充静态调用。

### 2.2 `struct security_hook_list`

```c
struct security_hook_list {
	struct lsm_static_call *scalls;   /* 该 hook 的 static call 数组起点 */
	union security_list_options hook; /* 实际的回调函数指针 */
	const struct lsm_id *lsmid;       /* 所属模块标识 */
} __randomize_layout;
```

模块用宏批量初始化（`LSM_HOOK_INIT`）：

```c
#define LSM_HOOK_INIT(NAME, HOOK)			\
	{						\
		.scalls = static_calls_table.NAME,	\
		.hook = { .NAME = HOOK }		\
	}
```

`capability_hooks[]`、`selinux_hooks[]` 等都是 `struct security_hook_list` 数组，元素个数即该模块挂接的 hook 数。

### 2.3 `struct lsm_static_call` 与静态调用表

```c
struct lsm_static_call {
	struct static_call_key *key;
	void *trampoline;
	struct security_hook_list *hl;
	struct static_key_false *active;
} __randomize_layout;
```

每个 hook 对应一个长度为 `MAX_LSM_COUNT` 的数组：

```c
struct lsm_static_calls_table {
	#define LSM_HOOK(RET, DEFAULT, NAME, ...) \
		struct lsm_static_call NAME[MAX_LSM_COUNT];
	#include <linux/lsm_hook_defs.h>
	#undef LSM_HOOK
} __packed __randomize_layout;
```

这是 7.x 之后取代旧版 `security_hook_heads` 双向链表的设计：`CONFIG_HAVE_STATIC_CALL` 下每个槽位是一个 static call + static key，禁用时零开销。

### 2.4 `struct lsm_info` 与 `DEFINE_LSM`

```c
struct lsm_info {
	const struct lsm_id *id;
	enum lsm_order order;
	unsigned long flags;
	struct lsm_blob_sizes *blobs;
	int *enabled;
	int (*init)(void);
	int (*initcall_pure)(void);
	int (*initcall_early)(void);
	int (*initcall_core)(void);
	int (*initcall_subsys)(void);
	int (*initcall_fs)(void);
	int (*initcall_device)(void);
	int (*initcall_late)(void);
};
```

模块用 `DEFINE_LSM(x)` 把自己放进 `.lsm_info.init` 段，用 `DEFINE_EARLY_LSM(x)` 放进 `.early_lsm_info.init` 段。链接脚本按段收集，`lsm_init.c` 通过 `__start_lsm_info/__end_lsm_info` 遍历，因此**新增模块不改框架代码**。

`order` 取值：

| 值 | 含义 | 使用者 |
|----|------|--------|
| `LSM_ORDER_FIRST` (-1) | 永远最先 | 仅 `capability` |
| `LSM_ORDER_MUTABLE` (0) | 由 `CONFIG_LSM` / `lsm=` 排序 | 绝大多数 |
| `LSM_ORDER_LAST` (1) | 永远最后 | `ima`、`evm`（`security/integrity/ima/ima_main.c:1376`、`security/integrity/evm/evm_main.c:1233`） |

`flags`：`LSM_FLAG_LEGACY_MAJOR`（支持旧式 `security=` 选择）、`LSM_FLAG_EXCLUSIVE`（同组互斥，只能有一个）。

### 2.5 Security blob 尺寸表

```c
struct lsm_blob_sizes {
	unsigned int lbs_cred, lbs_file, lbs_backing_file, lbs_ib;
	unsigned int lbs_inode, lbs_sock, lbs_superblock, lbs_ipc, lbs_key;
	unsigned int lbs_msg_msg, lbs_perf_event, lbs_task, lbs_xattr_count;
	unsigned int lbs_tun_dev, lbs_bdev, lbs_bpf_map, lbs_bpf_prog, lbs_bpf_token;
};
```

每个模块声明自己需要多少字节，框架在启动时把所有模块的需求**顺序拼接**成一个大 blob，并把每个模块的字段改写成相对偏移（`lsm_prepare()` → `lsm_blob_size_update()`，按 `sizeof(void *)` 对齐）。分配方式分两类：

- `cred`/`task` 直接 `kzalloc` 挂在 `cred->security` / `task->security`；
- `file`/`backing_file`/`inode` 用专用 `kmem_cache`（`lsm_file_cache`、`lsm_backing_file_cache`、`lsm_inode_cache`），因为数量巨大且需 RCU 释放；inode blob 额外预留一个 `struct rcu_head`（`lsm_prepare()` 中 `blob_sizes.lbs_inode = sizeof(struct rcu_head)`）。

模块侧通过 offset 取自己的私有结构，例如 SELinux 的 `selinux_cred(cred)`、`selinux_inode(inode)` 均是 `blob + offset` 的强制转换。

## 3. hook 调用机制

### 3.1 调用宏

`security.c` 用 `UNROLL(MAX_LSM_COUNT, ...)` 在编译期展开成 MAX_LSM_COUNT 段代码：

```c
#define __CALL_STATIC_INT(NUM, R, HOOK, LABEL, ...)			\
do {									\
	if (static_branch_unlikely(&SECURITY_HOOK_ACTIVE_KEY(HOOK, NUM))) { \
		R = static_call(LSM_STATIC_CALL(HOOK, NUM))(__VA_ARGS__); \
		if (R != LSM_RET_DEFAULT(HOOK))				\
			goto LABEL;					\
	}								\
} while (0);

#define call_int_hook(HOOK, ...)					\
({									\
	__label__ OUT;							\
	int RC = LSM_RET_DEFAULT(HOOK);					\
	LSM_LOOP_UNROLL(__CALL_STATIC_INT, RC, HOOK, OUT, __VA_ARGS__);	\
OUT:									\
	RC;								\
})
```

语义要点：

1. **短路**：一旦某个模块返回非默认值（通常是负错误码），立即 `goto OUT` 返回，后续模块不再执行。因此顺序即优先级，排在前面（`capability`）的模块拥有否决权。
2. **默认值**：`LSM_RET_DEFAULT(NAME)` 由 `lsm_hook_defs.h` 里的 `DEFAULT` 生成（`static const int NAME##_default = (DEFAULT);`，见 `security.c` 第 442-452 行附近）。
3. `call_void_hook` 无返回值、遍历全部模块。

### 3.2 静态调用的填充

`security_add_hooks()`（`security/lsm_init.c`）对每个 `security_hook_list` 调用 `lsm_static_call_init()`：从 `static_calls_table.NAME[0]` 起找到第一个 `hl == NULL` 的槽位，执行 `__static_call_update()` 并把 `active` 这个 static key 打开。槽位耗尽会 `panic("exhausted LSM callback slots")`，正常配置下不会发生，因为槽位总数等于 `MAX_LSM_COUNT`。

`MAX_LSM_COUNT`（`include/linux/lsm_count.h`）在编译期由 `CONFIG_SECURITY_*` 组合统计：capability、selinux、smack、apparmor、tomoyo、yama、loadpin、lockdown、safesetid、bpf、landlock、ima、evm、ipe。**当前 `.config` 关闭 `CONFIG_SECURITY`，此宏退化为 0**，整个机制不参与编译。

### 3.3 遍历某个 hook 的所有实现

用户态查询类接口需要遍历而不是短路，用：

```c
#define lsm_for_each_hook(scall, NAME)					\
	for (scall = static_calls_table.NAME;				\
	     scall - static_calls_table.NAME < MAX_LSM_COUNT; scall++)	\
		if (static_key_enabled(&scall->active->key))
```

`security_getselfattr()` 正是这样逐个模块收集 `lsm_ctx`。

## 4. LSM 顺序、堆叠与启用规则

`lsm_init.c` 中有三个排序来源，优先级从高到低：

1. 内核命令行 `lsm=`（`lsm_choose_lsm()`）；
2. 编译期 `CONFIG_LSM`（`lsm_order_builtin`）；
3. 旧式 `security=`（`lsm_choose_security()`，仅对带 `LSM_FLAG_LEGACY_MAJOR` 的模块生效，且显式选中后不会回退到其他 major）。

`CONFIG_LSM` 默认值（`security/Kconfig:268` 起）例如：

```
landlock,lockdown,yama,loadpin,safesetid,selinux,smack,tomoyo,apparmor,ipe,bpf
```

解析流程 `lsm_order_parse()`：

- 先放 `LSM_ORDER_FIRST`（capability）；
- 再按字符串表顺序放 `LSM_ORDER_MUTABLE`；
- 若存在 legacy 参数，插入被选中的 major 模块；
- 最后放 `LSM_ORDER_LAST`（ima、evm）；
- 未出现在顺序表中的模块一律 `lsm_enabled_set(lsm, false)`。

`lsm_order_append()` 的额外约束：

- 去重；
- `lsm_active_cnt == MAX_LSM_COUNT` 时告警并禁用；
- `LSM_FLAG_EXCLUSIVE`：已有互斥模块时，新来的被禁用（SELinux/AppArmor 同属 legacy major，二者互斥）。

最终启用列表记录在全局 `lsm_idlist[]`（`const struct lsm_id *`），供 `lsm_list_modules` 返回。

## 5. 初始化流程与调用链

```
start_kernel()
  └─ early_security_init()            init/main.c:1010
       └─ 遍历 .early_lsm_info.init：lsm_prepare() + lsm_init_single()
  ...
  └─ security_init()                  init/main.c:1199
       ├─ lsm_order_parse()            按 CONFIG_LSM 或 lsm= 决定顺序
       ├─ lsm_order_for_each(lsm) lsm_prepare(*lsm)   汇总 blob 尺寸
       ├─ kmem_cache_create(lsm_file_cache / lsm_backing_file_cache / lsm_inode_cache)
       ├─ lsm_cred_alloc(current->cred) / lsm_task_alloc(current)
       └─ lsm_init_single()           跳过已初始化的 early LSM，其余依次 ->init()
```

之后框架把每个模块的可选初始化阶段映射到标准 initcall（`security_initcall_*`）：

| initcall | 对应 `lsm_info` 字段 |
|----------|----------------------|
| `pure_initcall` | `initcall_pure` |
| `early_initcall` | `initcall_early` |
| `core_initcall` | `initcall_core`（同时 `securityfs_init()`） |
| `subsys_initcall` | `initcall_subsys` |
| `fs_initcall` | `initcall_fs` |
| `device_initcall` | `initcall_device` |
| `late_initcall` | `initcall_late`，完成后 `call_blocking_lsm_notifier(LSM_STARTED_ALL, NULL)` |

`lsm_initcall(level)` 宏对顺序表中的每个模块调用同名回调，单个失败只 `pr_warn`，返回第一个错误码。SELinux 用 `initcall_device = selinux_initcall`（`security/selinux/hooks.c` LSM 定义处）完成策略加载后的收尾，例如 `selinux_complete_init()` 遍历已存在超级块补挂标签。

## 6. capability 与 major LSM

`capability` 是唯一 `LSM_ORDER_FIRST` 模块，编译条件为 `CONFIG_SECURITY`（`security/commoncap.c`）。它把传统 DAC 中的 capability 判定包装成 hook，是“永远存在”的底座：

```c
static struct security_hook_list capability_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(capable, cap_capable),
	LSM_HOOK_INIT(settime, cap_settime),
	LSM_HOOK_INIT(ptrace_access_check, cap_ptrace_access_check),
	LSM_HOOK_INIT(ptrace_traceme, cap_ptrace_traceme),
	LSM_HOOK_INIT(capget, cap_capget),
	LSM_HOOK_INIT(capset, cap_capset),
	LSM_HOOK_INIT(bprm_creds_from_file, cap_bprm_creds_from_file),
	LSM_HOOK_INIT(inode_need_killpriv, cap_inode_need_killpriv),
	LSM_HOOK_INIT(inode_killpriv, cap_inode_killpriv),
	LSM_HOOK_INIT(inode_getsecurity, cap_inode_getsecurity),
	LSM_HOOK_INIT(mmap_addr, cap_mmap_addr),
	LSM_HOOK_INIT(task_fix_setuid, cap_task_fix_setuid),
	LSM_HOOK_INIT(task_prctl, cap_task_prctl),
	LSM_HOOK_INIT(task_setscheduler, cap_task_setscheduler),
	LSM_HOOK_INIT(task_setioprio, cap_task_setioprio),
	LSM_HOOK_INIT(task_setnice, cap_task_setnice),
	LSM_HOOK_INIT(vm_enough_memory, cap_vm_enough_memory),
};
```

`cap_capable()` 的核心是 capability 集合与 user namespace 的层级比较；`cap_mmap_addr()` 负责 `dac_mmap_min_addr` 低地址保护（注意它在 `CONFIG_SECURITY` 之外也单独编译，因为在未启用 LSM 时仍需防 NULL 解引用）。

“major LSM”指旧模型下只能选一个的强制访问控制主体（SELinux / AppArmor / Smack / Tomoyo），它们在 7.x 已改为可堆叠，但仍保留 `LSM_FLAG_LEGACY_MAJOR | LSM_FLAG_EXCLUSIVE` 与 `security=` 兼容语义。SELinux 的定义可作为模板：

```c
DEFINE_LSM(selinux) = {
	.id = &selinux_lsmid,
	.flags = LSM_FLAG_LEGACY_MAJOR | LSM_FLAG_EXCLUSIVE,
	.enabled = &selinux_enabled_boot,
	.blobs = &selinux_blob_sizes,
	.init = selinux_init,
	.initcall_device = selinux_initcall,
};
```

## 7. 用户态接口（`lsm_syscalls.c`）

7.x 提供三个系统调用代替 `/sys/kernel/security/lsm` 与 `/proc/.../attr/*` 的部分功能：

| 系统调用 | 参数 | 说明 |
|----------|------|------|
| `lsm_list_modules` | `u64 __user *ids, u32 __user *size, u32 flags` | 按 `lsm_idlist[]` 顺序返回已启用模块 ID（`LSM_ID_*`，见 `uapi/linux/lsm.h`）；空间不足返回 `-E2BIG` 并回填所需大小 |
| `lsm_get_self_attr` | `attr, struct lsm_ctx __user *ctx, u32 __user *size, flags` | 读取当前任务的 LSM 上下文，可传 `LSM_FLAG_SINGLE` 只取指定模块 |
| `lsm_set_self_attr` | `attr, ctx, size, flags` | 设置当前任务上下文，持 `cred_guard_mutex` 后调用 `security_setselfattr()` |

`attr` 取值由 `lsm_name_to_attr()` 映射：`current`、`exec`、`fscreate`、`keycreate`、`prev`、`sockcreate`。

`security_getselfattr()`（`security.c`）的实现展示了“遍历 + 累积”的典型写法：用 `lsm_for_each_hook(scall, getselfattr)` 逐个模块取数据，`-EOPNOTSUPP` 跳过，`-E2BIG` 记为“空间不足”并继续统计总长度，最终返回条目数或 `-E2BIG`。

`struct lsm_ctx` 是用户/内核共享的变长结构（`uapi/linux/lsm.h`），`lsm_fill_user_ctx()`（`security.c`）负责按 `LSM_CTX_SIZE/ALIGN` 拷贝并处理 `E2BIG`。

## 8. 典型调用点举例

| 路径 | 框架入口 | 典型实现 |
|------|----------|----------|
| `open()` | `security_file_open()` → `file_open` | SELinux `selinux_file_open`、AppArmor `apparmor_file_open` |
| `inode_permission()` | `security_inode_permission()` | SELinux `selinux_inode_permission`、Smack `smack_inode_permission` |
| `execve()` | `security_bprm_creds_for_exec` / `bprm_check_security` / `bprm_committing_creds` | 标签转换与域切换 |
| `kill/ptrace` | `security_ptrace_access_check` / `task_kill` | Yama 的 `ptrace_scope` |
| `mount` | `security_sb_mount`、`sb_eat_lsm_opts`、`sb_set_mnt_opts` | SELinux 挂载上下文（`context=`/`fscontext=`） |
| 创建 inode | `security_inode_init_security()` | 生成 `security.selinux` 等初始 xattr |
| 模块加载 | `security_kernel_read_file`、`security_locked_down` | Lockdown/IMA 校验签名 |

`security_locked_down()` 的原因字符串集中在 `security.c` 顶部 `lockdown_reasons[]`（`LOCKDOWN_MODULE_SIGNATURE`、`LOCKDOWN_DEV_MEM`、`LOCKDOWN_KEXEC`、`LOCKDOWN_BPF_READ_KERNEL`、`LOCKDOWN_PERF` 等），审计日志与文档应引用同一份描述。

## 9. 当前配置下的实际状态

- `CONFIG_SECURITY=n`：`security/` 整体不编译；`security_*()` 系列在内核里由 `include/linux/security.h` 的 `static inline` 空实现替代（返回 0 / 默认值），`MAX_LSM_COUNT` 为 0。
- 因此 capability 的 LSM 包装（`cap_capable` 作为 hook）不存在，但传统 capability 检查本身在内核核心代码中仍然有效；`cap_mmap_min_addr` 的 `dac_mmap_min_addr` 逻辑依赖 `security/commoncap.c` 在 `CONFIG_SECURITY` 之外也编译的那部分。
- 若后续要观察 LSM 运行行为，需在 `.config` 打开 `CONFIG_SECURITY` 及具体模块（如 `CONFIG_SECURITY_SELINUX`、`CONFIG_SECURITY_APPARMOR`），并用 `lsm=` 或 `CONFIG_LSM` 控制启用顺序；`lsm.debug` 可打开框架自身的初始化日志（`security/lsm_init.c` 的 `lsm_debug_enable()`）。
- 启用后可用 `lsm_list_modules` 系统调用或 `/sys/kernel/security/lsm` 查看实际启用列表。

## 关键文件清单

| 路径 | 说明 |
|------|------|
| `include/linux/lsm_hook_defs.h` | 279 条 hook 的唯一声明源（返回类型/默认值/参数） |
| `include/linux/lsm_hooks.h` | `security_hook_list`、`lsm_static_call`、`lsm_static_calls_table`、`lsm_info`、`lsm_blob_sizes`、`DEFINE_LSM` |
| `include/linux/lsm_count.h` | 编译期 `MAX_LSM_COUNT` 统计 |
| `include/linux/security.h` | 各 `security_*()` 原型；`CONFIG_SECURITY=n` 时的 inline 空实现 |
| `include/uapi/linux/lsm.h` | `LSM_ID_*`、`LSM_ATTR_*`、`struct lsm_ctx` |
| `security/security.c` | 所有 `security_*()` 入口、`call_int_hook`/`call_void_hook`、blob 分配、`lockdown_reasons[]` |
| `security/lsm_init.c` | `security_init()`、`early_security_init()`、排序与 blob 汇总、initcall 分发 |
| `security/lsm_syscalls.c` | `lsm_list_modules`/`lsm_get_self_attr`/`lsm_set_self_attr` |
| `security/lsm.h` | 框架内部声明与 `lsm_pr_dbg()` |
| `security/commoncap.c` | capability LSM（`capability_hooks[]`、`cap_capable`、`cap_mmap_addr`） |
| `security/Kconfig` | `CONFIG_LSM`、`CONFIG_SECURITY_*` 配置项 |
| `init/main.c` | `early_security_init()`（1010 行）、`security_init()`（1199 行） |
