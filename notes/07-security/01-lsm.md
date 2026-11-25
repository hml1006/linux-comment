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

清单中还有少量条件编译项：`CONFIG_SECURITY_PATH` 包裹路径类 hook（`lsm_hook_defs.h:90-110`），`CONFIG_SECURITY_NETWORK` 包裹网络类（330-389 行），另有 `WATCH_QUEUE`、`KEY_NOTIFICATIONS` 相关的小段。用 `grep -c '^LSM_HOOK('` 统计行首声明得 277 条，加上条件块内缩进的 2 条共 279 条，与上表一致。也就是说 hook 总数本身随配置变化，而 `union security_list_options`、静态调用表、默认值常量都按同一份清单展开，任何配置下都天然保持一致。

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

`security.c` 中该表的实例还带两个额外属性（`security/security.c:139`）：

```c
struct lsm_static_calls_table
	static_calls_table __ro_after_init __aligned(sizeof(u64)) = { ... };
```

- `__ro_after_init`：初始化完成后整张表转入只读段，运行期对 hook 指针的任何改写都会触发页保护异常，这本身就是一道加固；
- `__aligned(sizeof(u64))`：源码注释解释，early LSM 阶段某些架构的对齐访问故障处理尚未就绪，表必须按 8 字节对齐，避免早期初始化触发未处理的 fault。

每个槽位的 `key`/`trampoline`/`active` 在编译期由 `INIT_LSM_STATIC_CALL` 宏填好（`security.c:141-154`），`hl` 初始为 NULL，等待 `security_add_hooks()` 认领。`CONFIG_HAVE_STATIC_CALL` 未定义时 `LSM_HOOK_TRAMP` 退化为 NULL（`security.c:107-112`），框架仍可用普通函数指针方式工作。

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

### 2.6 `struct lsm_id` 与 `struct lsm_prop`

每个模块用一个 `struct lsm_id` 标识自己（`include/linux/lsm_hooks.h:81`）：

```c
struct lsm_id {
	const char *name;   /* 模块名，需经 LSM 维护者认可 */
	u64 id;             /* uapi/linux/lsm.h 中的 LSM_ID_* */
};
```

`include/uapi/linux/lsm.h:53` 起定义了稳定的数字 ID：`LSM_ID_CAPABILITY=100`、`SELINUX=101`、`SMACK=102`、`TOMOYO=103`、`APPARMOR=104`、`YAMA=105`、`LOADPIN=106`、`SAFESETID=107`、`LOCKDOWN=108`、`BPF=109`、`LANDLOCK=110`、`IMA=111`、`EVM=112`、`IPE=113`。这些 ID 是 `lsm_list_modules` 的返回值，也是 `lsm_ctx.id` 的取值；多模块堆叠时用户态据此区分每条上下文属于哪个模块。

跨模块传递安全属性时不再使用单一 secid，而是 `struct lsm_prop`（`include/linux/security.h:164`）：

```c
struct lsm_prop {
	struct lsm_prop_selinux selinux;
	struct lsm_prop_smack smack;
	struct lsm_prop_apparmor apparmor;
	struct lsm_prop_bpf bpf;
};
```

`security_inode_getlsmprop()`、`security_cred_getlsmprop()`、`security_current_getlsmprop_subj()` 等入口（`security/security.c:2287/2912/3144`）把各模块的私有标识一次性收集进同一个结构；审计规则匹配（`security_audit_rule_match()`，`security.c:5276`）、网络标签等需要“同时尊重多个模块”的路径以它为参数。这是 LSM 堆叠在 7.x 的关键基础设施：框架不再假设只有一个模块拥有“那个”安全标识。

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

默认值常量的生成也在 `security.c`（447-455 行）：

```c
#define LSM_RET_DEFAULT(NAME) (NAME##_default)
#define DECLARE_LSM_RET_DEFAULT_int(DEFAULT, NAME) \
	static const int __maybe_unused LSM_RET_DEFAULT(NAME) = (DEFAULT);
#define LSM_HOOK(RET, DEFAULT, NAME, ...) \
	DECLARE_LSM_RET_DEFAULT_##RET(DEFAULT, NAME)
#include <linux/lsm_hook_defs.h>
```

`void` 型 hook 对应的 `DECLARE_LSM_RET_DEFAULT_void` 是空宏，因此只有 `int` 型 hook 有 `NAME_default` 常量。`call_void_hook` 的展开（`security.c:466-476`）与 `call_int_hook` 类似，只是去掉返回值比较与短路：

```c
#define __CALL_STATIC_VOID(NUM, HOOK, ...)				     \
do {									     \
	if (static_branch_unlikely(&SECURITY_HOOK_ACTIVE_KEY(HOOK, NUM))) {    \
		static_call(LSM_STATIC_CALL(HOOK, NUM))(__VA_ARGS__);	     \
	}								     \
} while (0);
```

由于 `static_branch_unlikely` 在槽位未启用时被补丁为 nop，未挂接任何模块的 hook 在运行期几乎零开销。这与“`CONFIG_SECURITY=n` 时 inline 空实现”构成两级零开销设计：编译期剔除与运行期 static key 关闭。

### 3.2 静态调用的填充

`security_add_hooks()`（`security/lsm_init.c`）对每个 `security_hook_list` 调用 `lsm_static_call_init()`：从 `static_calls_table.NAME[0]` 起找到第一个 `hl == NULL` 的槽位，执行 `__static_call_update()` 并把 `active` 这个 static key 打开。槽位耗尽会 `panic("exhausted LSM callback slots")`，正常配置下不会发生，因为槽位总数等于 `MAX_LSM_COUNT`。

`MAX_LSM_COUNT`（`include/linux/lsm_count.h`）在编译期由 `CONFIG_SECURITY_*` 组合统计：capability、selinux、smack、apparmor、tomoyo、yama、loadpin、lockdown、safesetid、bpf、landlock、ima、evm、ipe。**当前 `.config` 关闭 `CONFIG_SECURITY`，此宏退化为 0**，整个机制不参与编译。

`lsm_static_call_init()` 的实际代码（`security/lsm_init.c:341`）：

```c
static int __init lsm_static_call_init(struct security_hook_list *hl)
{
	struct lsm_static_call *scall = hl->scalls;
	int i;

	for (i = 0; i < MAX_LSM_COUNT; i++) {
		/* Update the first static call that is not used yet */
		if (!scall->hl) {
			__static_call_update(scall->key, scall->trampoline,
					     hl->hook.lsm_func_addr);
			scall->hl = hl;
			static_branch_enable(scall->active);
			return 0;
		}
		scall++;
	}
	return -ENOSPC;
}
```

注意 `hl->hook.lsm_func_addr`：`union security_list_options` 末尾特意放了一个 `void *lsm_func_addr` 成员（`lsm_hooks.h:42`），让框架可以不关心具体 hook 签名而统一取出函数地址。`security_add_hooks()`（`lsm_init.c:369`）在循环里先给每个 `hooks[i].lsmid` 赋值再填槽，失败即 `panic("exhausted LSM callback slots with LSM %s")`。槽位按模块注册顺序从 0 开始占用，因此 `lsm_order` 的顺序直接决定了同一 hook 上各模块回调的执行顺序。

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

`lsm_order_append()`（`lsm_init.c:153`）的判定顺序值得逐条对照源码：先查重（`lsm_order_exists`），再跳过被显式禁用的模块（`lsm->enabled && !lsm_is_enabled(lsm)`），然后检查 `lsm_active_cnt == MAX_LSM_COUNT` 上限，最后处理 `LSM_FLAG_EXCLUSIVE`——全局只允许一个互斥模块，`lsm_exclusive` 记录第一个入选者，后来者直接 `lsm_enabled_set(lsm, false)`。入选时同步写三处：`lsm_enabled_set(lsm, true)`、`lsm_order[lsm_active_cnt] = lsm`、`lsm_idlist[lsm_active_cnt++] = lsm->id`。

legacy `security=` 的排他语义在 `lsm_order_parse()` 开头（`lsm_init.c:204-219`）：一旦指定了 `security=selinux`，所有名字不匹配的 `LSM_FLAG_LEGACY_MAJOR` 模块都会被预先禁用，且**不会回退**到其它 major——注释明确写了这是为了兼容旧行为。另外 `security_init()` 中若同时存在 `lsm=` 与 `security=`，前者生效、后者被置 NULL（`lsm_init.c:433-438`）。

一个容易忽略的细节：TOMOYO 也带 `LSM_FLAG_LEGACY_MAJOR`（`security/tomoyo/tomoyo.c:614`），但**不带** `LSM_FLAG_EXCLUSIVE`，因此它能与 SELinux/AppArmor/Smack 堆叠；`security=` 参数却会把它当作 legacy major 参与排他选择。`lsm_enabled_set()`（`lsm_init.c:104`）对没有自带 `enabled` 变量的模块使用框架内置的 `lsm_enabled_true/false` 两个 `__initdata` 常量存放状态，对自带变量的模块（如 selinux 的 `selinux_enabled_boot`）则直接写模块自己的变量。

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

`init/main.c` 中的调用位置有严格的前置依赖：`early_security_init()`（1010 行）之前必须先执行 `jump_label_init()` 与 `static_call_init()`（1008-1009 行，源码注释“Static keys and static calls are needed by LSMs”），因为 early LSM 的 hook 分发依赖 static call/jump label 基础设施；`security_init()`（1199 行）则排在 `cred_init()`/`fork_init()`/`key_init()` 之后、`vfs_caches_init()` 之前，确保初始任务的 cred/task blob 与后续 inode cache 都能就位。

`lsm_initcall` 宏本体（`lsm_init.c:42`）：

```c
#define lsm_initcall(level)						\
	({								\
		int _r, _rc = 0;					\
		struct lsm_info **_lp, *_l;				\
		lsm_order_for_each(_lp) {				\
			_l = *_lp;					\
			if (!_l->initcall_##level)			\
				continue;				\
			_r = _l->initcall_##level();			\
			if (_r) {					\
				pr_warn("failed LSM %s %s initcall with errno %d\n", \
					_l->id->name, #level, _r);	\
				if (!_rc)				\
					_rc = _r;			\
			}						\
		}							\
		_rc;							\
	})
```

`core` 阶段比较特殊：`security_initcall_core()`（`lsm_init.c:517`）先调用 `securityfs_init()` 再跑各模块的 `initcall_core`，两者错误码取先失败者；`late` 阶段结束后广播 `call_blocking_lsm_notifier(LSM_STARTED_ALL, NULL)`（`lsm_init.c:564`），供依赖“全部 LSM 就绪”的子系统挂接通知。`security_init()` 末尾初始化各模块时按 `lsm_count_early` 跳过已经初始化的 early LSM（`lsm_init.c:485-491`），避免重复初始化。

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

`cap_capable()`（`security/commoncap.c:124`）本身只是薄封装，核心在 `cap_capable_helper()`（`commoncap.c:68`）：

```c
static inline int cap_capable_helper(const struct cred *cred,
				     struct user_namespace *target_ns,
				     const struct user_namespace *cred_ns,
				     int cap)
{
	struct user_namespace *ns = target_ns;

	for (;;) {
		if (likely(ns == cred_ns))
			return cap_raised(cred->cap_effective, cap) ? 0 : -EPERM;
		if (ns->level <= cred_ns->level)
			return -EPERM;
		if ((ns->parent == cred_ns) && uid_eq(ns->owner, cred->euid))
			return 0;
		ns = ns->parent;
	}
}
```

语义：从资源所在 user namespace 逐级向上走到 cred 的 namespace；途中若 cred 是目标 ns 父 ns 的 owner，则视为拥有全部 capability（子 namespace 的创建者对子 ns 内资源全权）；到达同层后检查 `cap_effective`；若已经低于 cred 所在层级则直接拒绝。`cap_capable()` 外层还加了 `trace_cap_capable()` tracepoint，可用 ftrace 观察 capability 判定过程。

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

UAPI 定义（`include/uapi/linux/lsm.h:35`）：

```c
struct lsm_ctx {
	__u64 id;
	__u64 flags;
	__u64 len;
	__u64 ctx_len;
	__u8 ctx[] __counted_by(ctx_len);
};
```

`len` 是整条记录（含头部）的长度，`ctx_len` 是上下文值长度，柔性数组用 `__counted_by` 标注以便编译器/检测器做边界检查。`lsm_list_modules` 的实现（`security/lsm_syscalls.c:105`）先把 `lsm_active_cnt * sizeof(u64)` 写回 `size`，再逐个 `put_user(lsm_idlist[i]->id)`；缓冲区不足时返回 `-E2BIG` 但已回填所需大小，用户态可据此二次分配。`lsm_set_self_attr` 则先 `mutex_lock_interruptible(&current->signal->cred_guard_mutex)` 再调 `security_setselfattr()`（`lsm_syscalls.c:57-68`），与 exec/ptrace 等修改 cred 的路径互斥。

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

全表在 `security/security.c:43-75`，按 `enum lockdown_reason` 索引，摘录：

```c
const char *const lockdown_reasons[LOCKDOWN_CONFIDENTIALITY_MAX + 1] = {
	[LOCKDOWN_NONE] = "none",
	[LOCKDOWN_MODULE_SIGNATURE] = "unsigned module loading",
	[LOCKDOWN_DEV_MEM] = "/dev/mem,kmem,port",
	[LOCKDOWN_KEXEC] = "kexec of unsigned images",
	[LOCKDOWN_BPF_READ_KERNEL] = "use of bpf to read kernel RAM",
	[LOCKDOWN_PERF] = "unsafe use of perf",
	[LOCKDOWN_TRACEFS] = "use of tracefs",
	[LOCKDOWN_INTEGRITY_MAX] = "integrity",
	[LOCKDOWN_CONFIDENTIALITY_MAX] = "confidentiality",
	/* ... 共 30 项 ... */
};
```

`security_locked_down()`（`security.c:5493`）只是 `call_int_hook(locked_down, what)`。把原因字符串集中在框架层，保证 Lockdown LSM 的审计输出与各调用点语义一致；`LOCKDOWN_INTEGRITY_MAX`/`LOCKDOWN_CONFIDENTIALITY_MAX` 两个“级别哨兵”同时被 `security/lockdown/lockdown.c` 用作级别数组元素（见 [05-hardening.md](05-hardening.md) 第 11 节）。

## 9. 当前配置下的实际状态

- `CONFIG_SECURITY=n`：`security/` 整体不编译；`security_*()` 系列在内核里由 `include/linux/security.h` 的 `static inline` 空实现替代（返回 0 / 默认值），`MAX_LSM_COUNT` 为 0。
- 因此 capability 的 LSM 包装（`cap_capable` 作为 hook）不存在，但传统 capability 检查本身在内核核心代码中仍然有效；`cap_mmap_min_addr` 的 `dac_mmap_min_addr` 逻辑依赖 `security/commoncap.c` 在 `CONFIG_SECURITY` 之外也编译的那部分。
- 若后续要观察 LSM 运行行为，需在 `.config` 打开 `CONFIG_SECURITY` 及具体模块（如 `CONFIG_SECURITY_SELINUX`、`CONFIG_SECURITY_APPARMOR`），并用 `lsm=` 或 `CONFIG_LSM` 控制启用顺序；`lsm.debug` 可打开框架自身的初始化日志（`security/lsm_init.c` 的 `lsm_debug_enable()`）。
- 启用后可用 `lsm_list_modules` 系统调用或 `/sys/kernel/security/lsm` 查看实际启用列表。
- 本树 `.config` 还关闭了 `CONFIG_SECURITYFS`：即使打开 `CONFIG_SECURITY`，`/sys/kernel/security/` 也需要 `CONFIG_SECURITYFS=y` 才会出现。`securityfs_init()`（`security/inode.c:363`）在 core initcall 阶段创建挂载点、注册文件系统并创建只读 `lsm` 文件（`security/inode.c:376`，权限 0444）。
- 本树 `CONFIG_PREEMPT_RT=y`。LSM 框架本身不引入额外锁，hook 在与调用者相同的上下文执行；blob 分配（`kmem_cache_zalloc`/`kzalloc`）的 gfp 约束由调用点决定，RT 下原子上下文中的分配失败路径同样返回 `-ENOMEM` 而非睡眠。

## 10. Security blob 的完整生命周期

### 10.1 分配：两种策略

框架把 blob 分配分成两类。`cred`/`task`/`ipc`/`key`/`msg_msg`/`superblock`/`bdev`/BPF 对象走通用 `lsm_blob_alloc()`（`security/security.c:211`）：

```c
static int lsm_blob_alloc(void **dest, size_t size, gfp_t gfp)
{
	if (size == 0) {
		*dest = NULL;
		return 0;
	}
	*dest = kzalloc(size, gfp);
	if (*dest == NULL)
		return -ENOMEM;
	return 0;
}
```

`size == 0`（没有任何启用模块需要该类 blob）时直接置 NULL，不分配内存。`lsm_cred_alloc()`（233 行）、`lsm_task_alloc()`（268 行）等都是它的一行封装；`lsm_key_alloc()` 额外包在 `#ifdef CONFIG_KEYS` 里（286-299 行），本树 `CONFIG_KEYS=n` 时不存在。

`file`/`backing_file`/`inode` 数量巨大，走专用 slab。以 file 为例（`security.c:164`）：

```c
static int lsm_file_alloc(struct file *file)
{
	if (!lsm_file_cache) {
		file->f_security = NULL;
		return 0;
	}
	file->f_security = kmem_cache_zalloc(lsm_file_cache, GFP_KERNEL);
	if (file->f_security == NULL)
		return -ENOMEM;
	return 0;
}
```

三个 cache 在 `security_init()` 里按需创建（`lsm_init.c:465-477`），都带 `SLAB_PANIC`——blob 尺寸在启动期已确定，创建失败意味着配置/内存出了根本性问题，直接 panic 而不是带病运行。

### 10.2 inode blob 的 RCU 释放

inode 是最复杂的：VFS 路径遍历可能在 RCU 读侧临界区里引用 inode，`security_inode_permission()` 甚至可能在 `security_inode_free()` 之后仍被调用。因此 inode blob 头部预留 `struct rcu_head`（见 §2.5），释放走 `call_rcu`（`security.c:1225-1254`）：

```c
static void inode_free_by_rcu(struct rcu_head *head)
{
	/* The rcu head is at the start of the inode blob */
	call_void_hook(inode_free_security_rcu, head);
	kmem_cache_free(lsm_inode_cache, head);
}

void security_inode_free(struct inode *inode)
{
	call_void_hook(inode_free_security, inode);
	if (!inode->i_security)
		return;
	call_rcu((struct rcu_head *)inode->i_security, inode_free_by_rcu);
}
```

两个 hook 的分工：`inode_free_security` 在释放前立即调用，模块可做即时清理；真正可能被并发路径读到的状态必须留到 `inode_free_security_rcu`（宽限期结束后）再释放。源码注释特别提醒：尽管进入了 `security_inode_free()`，inode 仍可能被 VFS 路径遍历引用，需要保留 inode 状态供 `security_inode_permission()` 使用的模块只能在 `inode_free_security_rcu` 回调里释放该状态。分配侧 `security_inode_alloc()`（`security.c:1213`）先 `lsm_inode_alloc()` 再 `call_int_hook(inode_alloc_security)`，后者失败时回滚调用 `security_inode_free()`。

### 10.3 其余对象的释放

- `security_file_free()`（`security.c:2433`）：先 `call_void_hook(file_free_security)`，再 `kmem_cache_free(lsm_file_cache, blob)`；
- `security_task_free()`（2802 行）：`call_void_hook(task_free)` 后 `kfree(task->security)`；
- `security_cred_free()`（2839 行）：先判 `cred->security == NULL`（注释说明 `prepare_creds()` 的失败路径可能走到这里），再 `call_void_hook(cred_free)` + `kfree`。

这些释放函数与分配函数成对出现在 VFS/fork/cred 代码中，构成“框架管内存、模块管内容”的边界：模块只负责初始化和清理 blob 内自己那一段，永远不直接释放整个 blob。

## 11. 与系统调用路径的交互

### 11.1 open() 的完整检查链

以 `openat2` 为例，LSM 检查点分布在路径解析与文件打开两个阶段：

```
do_sys_openat2 → do_filp_open → path_openat
  → do_open()                          fs/namei.c:4857
      ├─ may_open() → inode_permission()
      │     └─ security_inode_permission()   fs/namei.c:663/700
      ├─ vfs_open() → do_dentry_open()       fs/open.c:885
      │     └─ security_file_open(f)         fs/open.c:925
      └─ security_file_post_open(file, acc_mode)  fs/namei.c:4909
```

`do_dentry_open()` 中的调用点（`fs/open.c:925`）：

```c
error = security_file_open(f);
if (unlikely(error))
	goto cleanup_all;
```

`security_file_open()`（`security.c:2737`）本身只有一行 `call_int_hook(file_open, file)`，但注释点明了它的职责：保存 open 时刻的权限状态供后续 `file_permission` 使用，并在 `inode_permission` 之后情况有变时复查；模块可通过 `file->f_flags & __FMODE_EXEC` 判断文件是否正被用于执行（包括 ELF 间接加载 ld.so 的情形）。

`security_file_post_open()`（`security.c:2753`）是较新的 hook，在文件成功打开后调用，供需要“文件内容已可读”才能决策的模块使用（如 IMA 的测量/评估，见 [03-ima-evm.md](03-ima-evm.md)）。它被 `EXPORT_SYMBOL_GPL` 导出，堆叠文件系统也可调用。注意 `security_inode_permission()` 对 `IS_PRIVATE(inode)` 直接放行（`security.c:1838`），而 `security_file_open()` 没有这层短路——anon inode 的 file 仍会走模块检查。

### 11.2 新建 inode 的原子标签：security_inode_init_security

创建文件时，VFS 在 inode 创建事务内调用 `security_inode_init_security()`（`security.c:1331`），让各模块原子地写入初始安全 xattr：

```c
int security_inode_init_security(struct inode *inode, struct inode *dir,
				 const struct qstr *qstr,
				 const initxattrs initxattrs, void *fs_data)
{
	struct lsm_static_call *scall;
	struct xattr *new_xattrs = NULL;
	int ret = -EOPNOTSUPP, xattr_count = 0;

	if (unlikely(IS_PRIVATE(inode)))
		return 0;
	if (!blob_sizes.lbs_xattr_count)
		return 0;
	if (initxattrs) {
		/* Allocate +1 as terminator. */
		new_xattrs = kcalloc(blob_sizes.lbs_xattr_count + 1,
				     sizeof(*new_xattrs), GFP_NOFS);
		if (!new_xattrs)
			return -ENOMEM;
	}
	lsm_for_each_hook(scall, inode_init_security) {
		ret = scall->hl->hook.inode_init_security(inode, dir, qstr,
							  new_xattrs, &xattr_count);
		if (ret && ret != -EOPNOTSUPP)
			goto out;
	}
	if (!xattr_count)
		goto out;
	ret = initxattrs(inode, new_xattrs, fs_data);
out:
	for (; xattr_count > 0; xattr_count--)
		kfree(new_xattrs[xattr_count - 1].value);
	kfree(new_xattrs);
	return (ret == -EOPNOTSUPP) ? 0 : ret;
}
```

要点：

1. 这是**遍历型**而非短路型入口：`-EOPNOTSUPP` 表示“本模块不提供 xattr”，继续调用后续模块，只有真正的错误才中断；
2. xattr 槽位总数由各模块 `lbs_xattr_count` 累加而来（§2.5），模块用 `lsm_get_xattr_slot()`（`lsm_hooks.h:211`）领取槽位并自增计数；
3. 所有模块填完后由文件系统的 `initxattrs` 回调一次性写入磁盘，最后框架统一释放各 `value`；
4. SELinux 写 `security.selinux`、Smack 写 `security.SMACK64`、IMA/EVM 写 `security.ima`/`security.evm` 都走这条路径（见 [02-mac.md](02-mac.md)、[03-ima-evm.md](03-ima-evm.md)）。

### 11.3 execve() 与凭证切换

exec 路径上的 hook 顺序为 `security_bprm_creds_for_exec`（计算新凭证）→ `security_bprm_check_security`（最终裁决，IMA 在此测量/评估可执行文件）→ `security_bprm_committing_creds` / `security_bprm_committed_creds`（提交前后通知）。各 MAC 模块的域切换（SELinux 的 type transition、AppArmor 的 profile 切换、TOMOYO 的 domain 切换）都发生在这一组 hook 中，细节见 [02-mac.md](02-mac.md)。

## 12. 用户态接口的实现细节

### 12.1 security_getselfattr() 的累积算法

`security_getselfattr()`（`security.c:3728`）的核心循环：

```c
lsm_for_each_hook(scall, getselfattr) {
	if (single && lctx.id != scall->hl->lsmid->id)
		continue;
	entrysize = left;
	if (base)
		uctx = (struct lsm_ctx __user *)(base + total);
	rc = scall->hl->hook.getselfattr(attr, uctx, &entrysize, flags);
	if (rc == -EOPNOTSUPP)
		continue;
	if (rc == -E2BIG) {
		rc = 0;
		left = 0;
		toobig = true;
	} else if (rc < 0)
		return rc;
	else
		left -= entrysize;
	total += entrysize;
	count += rc;
	if (single)
		break;
}
```

语义细节：`-EOPNOTSUPP` 跳过；`-E2BIG` 不立即返回，而是置 `toobig` 并继续统计后续模块所需空间，保证一次调用就能拿到完整的所需尺寸；`LSM_FLAG_SINGLE`（`uapi/linux/lsm.h:91`）时先 `copy_from_user` 读取用户指定的 `lctx.id`，只匹配该模块。最终 `put_user(total, size)` 回填总尺寸，返回条目数或 `-E2BIG`；一个模块都没提供数据时返回 `LSM_RET_DEFAULT(getselfattr)`。

### 12.2 lsm_fill_user_ctx() 的尺寸协商

模块侧用 `lsm_fill_user_ctx()`（`security.c:400`）把私有上下文打包成 `lsm_ctx`：

```c
nctx_len = ALIGN(struct_size(nctx, ctx, val_len), sizeof(void *));
if (nctx_len > *uctx_len) {
	rc = -E2BIG;
	goto out;
}
/* no buffer - return success/0 and set @uctx_len to the req size */
if (!uctx)
	goto out;
nctx = kzalloc(nctx_len, GFP_KERNEL);
...
nctx->id = id;
nctx->flags = flags;
nctx->len = nctx_len;
nctx->ctx_len = val_len;
memcpy(nctx->ctx, val, val_len);
if (copy_to_user(uctx, nctx, nctx_len))
	rc = -EFAULT;
```

`uctx == NULL` 时只计算尺寸不拷贝，这是用户态“先探测大小再分配”两段式调用的内核侧支撑；每条记录按 `sizeof(void *)` 对齐，保证数组中下一条 `lsm_ctx` 的起始地址对齐。无论成功与否，`*uctx_len` 都会被更新为本条记录的实际尺寸。

## 13. 配置与调试

### 13.1 lsm.debug 与初始化日志

`lsm.debug` 命令行参数置位 `lsm_debug`（`lsm_init.c:92`），`lsm_pr_dbg()`（`security/lsm.h:17`）据此输出。`security_init()` 在调试模式下会打印：可用 LSM 列表（early 模块带 `(E)` 标记）、`CONFIG_LSM` 内建顺序、legacy/boot 参数、每个 blob 的最终尺寸（`lsm_init.c:412-463`）。这是排查“某模块为何没启用”的第一手段：被互斥规则、顺序表遗漏或显式禁用的模块都会有 `skip ...` 日志。

### 13.2 early LSM 与 Lockdown 的双形态

`DEFINE_EARLY_LSM` 把模块放进 `.early_lsm_info.init` 段，在 `early_security_init()` 阶段（`start_kernel` 早期）初始化。本树中唯一使用者是 Lockdown：`security/lockdown/lockdown.c:164-172` 用 `#ifdef CONFIG_SECURITY_LOCKDOWN_LSM_EARLY` 在 `DEFINE_EARLY_LSM(lockdown)` 与 `DEFINE_LSM(lockdown)` 之间二选一：

```c
#ifdef CONFIG_SECURITY_LOCKDOWN_LSM_EARLY
DEFINE_EARLY_LSM(lockdown) = {
#else
DEFINE_LSM(lockdown) = {
#endif
	.id = &lockdown_lsmid,
	.init = lockdown_lsm_init,
	.initcall_core = lockdown_secfs_init,
};
```

early 形态的意义是让 lockdown 在尽可能早的阶段生效，覆盖早期启动中的受限操作。注意 `early_security_init()` 里不能用 `lsm_pr_dbg()`——此时 `lsm_debug` 尚未被命令行解析设置（源码注释明确说明）。

### 13.3 各 LSM 的注册位置一览

| 模块 | 注册宏位置 | order/flags 特点 |
|------|-----------|------------------|
| capability | `security/commoncap.c:1517` | `LSM_ORDER_FIRST` |
| selinux | `security/selinux/hooks.c:7916` | legacy major + exclusive |
| smack | `security/smack/smack_lsm.c:5374` | legacy major + exclusive |
| apparmor | `security/apparmor/lsm.c:2575` | legacy major + exclusive |
| tomoyo | `security/tomoyo/tomoyo.c:614` | legacy major（无 exclusive，可堆叠） |
| yama | `security/yama/yama_lsm.c:478` | 无 flags，mutable |
| loadpin | `security/loadpin/loadpin.c:430` | 无 flags，mutable |
| lockdown | `security/lockdown/lockdown.c:165/167` | early 可选 |
| safesetid | `security/safesetid/lsm.c:289` | 无 flags，mutable |
| bpf | `security/bpf/hooks.c:38` | 无 flags，mutable |
| landlock | `security/landlock/setup.c:77` | 无 flags，mutable |
| ima | `security/integrity/ima/ima_main.c:1373` | `LSM_ORDER_LAST` |
| evm | `security/integrity/evm/evm_main.c:1230` | `LSM_ORDER_LAST` |
| ipe | `security/ipe/ipe.c:95` | 无 flags，mutable |

（源码存在，当前配置未启用：上表所有模块在本树 `.config` 下均不编译。）

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
| `security/inode.c` | `securityfs_init()`（363 行）、`lsm` 只读文件（376 行） |
| `security/lockdown/lockdown.c` | early/普通双形态注册（164-172 行）、lockdown 级别与 securityfs 接口 |
| `fs/open.c` | `do_dentry_open()`（885 行）中的 `security_file_open()` 调用点（925 行） |
| `fs/namei.c` | `do_open()`（4857 行）、`security_file_post_open()` 调用点（4909 行）、`inode_permission` 中的 hook（663/700 行） |
