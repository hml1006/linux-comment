# 强制访问控制（MAC）模块：SELinux / AppArmor / Smack / TOMOYO

> 源码基线 Linux 7.2。四个模块都是 LSM 框架上的具体实现（见 `01-lsm.md`），通过 `DEFINE_LSM()` 注册、`security_add_hooks()` 挂接 hook。
>
> **配置前提**：当前 `.config` 中 `CONFIG_SECURITY` 未启用，因此 `CONFIG_SECURITY_SELINUX`、`CONFIG_SECURITY_APPARMOR`、`CONFIG_SECURITY_SMACK`、`CONFIG_SECURITY_TOMOYO` 都无从开启，四个模块在当前构建中均不参与编译。下文描述的是源码中存在的完整实现，运行时可用性取决于是否打开 `CONFIG_SECURITY` 与对应子项。

## 1. 模型对比

| 维度 | SELinux | AppArmor | Smack | TOMOYO |
|------|---------|----------|-------|--------|
| 判定依据 | 类型（type）+ 角色 + MLS 级别，策略编译成 avtab | 路径 + 名称，DFA 状态机 | 单标签（label），主体/客体各一个标签 | 路径名 + 进程域（domain） |
| 主体标识 | SID（sidtab） | `aa_label`（可为 profile 栈） | `smack_known` 字符串标签 | `tomoyo_domain_info` |
| 客体标签存放 | `security.selinux` xattr | 由路径实时匹配，不写 xattr | `security.SMACK64` 等 xattr | 不写 xattr，路径匹配 |
| 策略语言 | CIL / 二进制策略包 | profile 文本，编译为二进制 | `/sys/fs/smackfs/load2` 文本规则 | `/sys/kernel/security/tomoyo/` 文本 |
| 默认模式 | permissive / enforcing | complain / enforce / kill | enforce（无 permissive 全局开关，可用 `smackfs` 规则放宽） | disabled / learning / permissive / enforcing |
| 挂接 hook 数（`LSM_HOOK_INIT` 计） | 221 | 80 | 122 | 30 |
| 主要源文件规模 | `hooks.c` 7999 行、`ss/services.c` 3962、`ss/policydb.c` 4115 | `lsm.c` 2588、`domain.c` 1622、`label.c` 2135 | `smack_lsm.c` 5380、`smackfs.c` 3044 | `common.c` 3025、`file.c` 1050、`domain.c` 950 |

共同点：都在 LSM 框架里实现 `inode_permission`、`file_permission`、`bprm_creds_for_exec`、`task_*`、`sb_*`、`socket_*` 等 hook；失败时返回 `-EACCES`，由 LSM 框架短路并触发审计。

---

## 2. SELinux

### 2.1 架构

```
syscall/VFS  ──> security_inode_permission()
                     │
                     ▼
             selinux_inode_permission()            security/selinux/hooks.c
                     │  1. task_avdcache_search()  每任务 AVD 缓存（7.2 代码中存在）
                     │  2. avc_has_perm_noaudit()   AVC 全局缓存
                     │        └─ security_compute_av()  → avtab 查表 + conditional
                     │  3. avc_audit_required() + audit_inode_permission()
                     ▼
                  允许/拒绝
```

SELinux 是“类型强制 + 角色 + MLS”模型。策略由用户态 `libselinux`/`checkpolicy`/`sefcontext` 编译后写入 `/sys/fs/selinux/load`，内核侧编译结果存放在 `struct selinux_policy`（`policydb`、`sidtab`、`avtab`、`cond_av_list` 等）。

### 2.2 关键数据结构

每个 LSM blob 里只放 SID 等元数据，真正的策略在全局：

```c
// security/selinux/include/objsec.h
struct task_security_struct {
	struct {
		u32 sid;                 /* 当前 SID */
		u32 seqno;               /* AVC 序列号，策略变更后失效 */
		unsigned int dir_spot;
		struct avdc_entry dir[TSEC_AVDC_DIR_SIZE];  /* 目录类快捷缓存 */
		bool permissive_neveraudit;
	} avdcache;
} __randomize_layout;

struct inode_security_struct {
	struct inode *inode;
	struct list_head list;
	u32 task_sid;   /* 创建者 SID */
	u32 sid;        /* 本对象 SID */
	u16 sclass;     /* 安全类：file/dir/socket/... */
	unsigned char initialized;
	spinlock_t lock;
};

struct file_security_struct {
	u32 sid, fown_sid, isid, pseqno;
};

struct superblock_security_struct {
	u32 sid, def_sid, mntpoint_sid, creator_sid;
	unsigned short behavior;  /* SECURITY_FS_USE_* */
	unsigned short flags;
	struct mutex lock;
	struct list_head isec_head;
	spinlock_t isec_lock;
};
```

访问方式统一为 blob + 偏移，例如 `selinux_task(task) = task->security + selinux_blob_sizes.lbs_task`。

SID 表 `struct sidtab`（`security/selinux/ss/sidtab.h`）是分层哈希表：`sidtab_entry { sid, hash, context, list }`，支持策略重载时“旧表 → 新表”的增量转换（`sidtab_convert_params`）。`struct avc_node`（`security/selinux/avc.c`）缓存 `(ssid, tsid, tclass) → av_decision`，`avc_cache` 为固定槽位哈希 + per-slot 自旋锁 + LRU 提示。

### 2.3 权限判定流程

`selinux_inode_permission()`（`hooks.c`）是理解 SELinux 的最佳入口：

1. `mask = requested & (MAY_READ|MAY_WRITE|MAY_EXEC|MAY_APPEND)`，纯存在性检查（`F_OK`）直接放行；
2. `task_avdcache_permnoaudit()`：若任务处于 permissive 且 `neveraudit`，同 SID 且 `seqno == avc_policy_seqno()` 时直接放行；
3. `inode_security_rcu()` 取 `isec`（`MAY_NOT_BLOCK` 走 RCU 版本，否则可能睡眠初始化）；
4. `file_mask_to_av(inode->i_mode, mask)` 把 `MAY_*` 转成 `FILE__READ/WRITE/EXECUTE/APPEND` 权限位；
5. `task_avdcache_search()` 命中则直接用 `avdc->avd`，否则 `avc_has_perm_noaudit(sid, isec->sid, isec->sclass, perms, 0, avdp)` 并回填；
6. `denied = perms & ~avdp->allowed`；enforcing 且非 `AVD_FLAGS_PERMISSIVE` 时返回 `-EACCES`；
7. `avc_audit_required()` 计算是否需要审计，需要则 `audit_inode_permission()` 产生 `AVC` 审计记录。

`avc_has_perm_noaudit()` 未命中缓存时调用 `security_compute_av()`：查 `avtab` 得允许集合，再跑 `constraint`（TE 约束）、`conditional`（布尔表达式）和 MLS 级别检查，合并成 `av_decision`。策略变更（`avc_ss_reset()`）会递增 seqno，使以上缓存全部失效。

与 open 路径的配合：`selinux_file_open()`（`hooks.c:4254`）在打开时保存 inode 标签与策略序列号，供后续 `selinux_file_permission` 判断是否需要重新验证：

```c
fsec = selinux_file(file);
isec = inode_security(file_inode(file));
fsec->isid = isec->sid;
fsec->pseqno = avc_policy_seqno();
/*
 * Since the inode label or policy seqno may have changed
 * between the selinux_inode_permission check and the saving
 * of state above, recheck that access is still permitted.
 * ...
 * This check is not redundant - do not remove.
 */
return file_path_has_perm(file->f_cred, file, open_file_to_av(file));
```

注释特意强调这次复查“不是冗余的，不要删除”：`inode_permission` 检查与保存状态之间标签/策略可能已变化，若不复查，已打开的 fd 可能永远不再对新标签/新策略重新验证。这正是 [01-lsm.md](01-lsm.md) §11.1 中 `security_file_open` 职责的 SELinux 实现。

`selinux_inode_permission()`（`hooks.c:3243`）中两级缓存的分支代码：

```c
tsec = selinux_task(current);
if (task_avdcache_permnoaudit(tsec, sid))
	return 0;

isec = inode_security_rcu(inode, requested & MAY_NOT_BLOCK);
if (IS_ERR(isec))
	return PTR_ERR(isec);
perms = file_mask_to_av(inode->i_mode, mask);

rc = task_avdcache_search(tsec, isec, &avdc);
if (likely(!rc)) {
	/* Cache hit. */
	avdp = &avdc->avd;
	denied = perms & ~avdp->allowed;
	if (unlikely(denied) && enforcing_enabled() &&
		!(avdp->flags & AVD_FLAGS_PERMISSIVE))
		rc = -EACCES;
} else {
	/* Cache miss. */
	rc = avc_has_perm_noaudit(sid, isec->sid, isec->sclass,
				  perms, 0, avdp);
	task_avdcache_update(tsec, isec, avdp);
}
```

任务级 avdcache 命中时完全绕过全局 AVC（连锁都不取）；未命中才走 `avc_has_perm_noaudit()` 并把结果回填任务缓存。`inode_security_rcu()` 按 `MAY_NOT_BLOCK` 决定走 RCU 快路径还是可睡眠的初始化路径，对应 VFS 路径遍历的 rcu-walk/ref-walk 两种模式。

### 2.4 策略加载与审计

- 策略加载：`security_load_policy(void *data, size_t len, ...)`（`ss/services.c:2309`），先 `policydb_read()` 解析，再 `sidtab` 转换、`avtab` 替换，最后广播 `avc_ss_reset()`；`security/selinux/selinuxfs.c` 提供 `/sys/fs/selinux/` 接口（`load`、`enforce`、`context`、`class`、`bool`、`avc/cache_stats` 等）。
- 上下文转换：`security_context_to_sid()` / `security_sid_to_context()`（`ss/services.c`），前者把 `u:r:t:s0:c0` 形式字符串映射到 SID，后者反向，`_force`/`_default` 变体用于无法失败的内核路径。
- 审计点：`avc_audit_required()` → `avc_audit()` / `audit_inode_permission()`；permissive 域中的拒绝以 `granted` 形式记录；`selinux_inode_permission` 里 `audited` 是否为零决定是否进 audit 子系统。
- 初始标签：`security_inode_init_security()` 对应 `selinux_inode_init_security()`，根据父目录类型/`fs_use_xattr`/`defcontext` 计算子对象标签并写入 `security.selinux`；超级块行为由 `superblock_security_struct.behavior`（`SECURITY_FS_USE_XATTR`/`GENFS`/`TASK`/`MNTPOINT`）决定。
- 与网络：`netif.c`/`netnode.c`/`netport.c`/`xfrm.c` 实现 `socket_*`、`xfrm_policy` 等 hook，`nlmsgtab.c` 映射 Netlink 消息类型。

### 2.5 初始化

`DEFINE_LSM(selinux)` 带 `LSM_FLAG_LEGACY_MAJOR | LSM_FLAG_EXCLUSIVE` 与 `enabled = &selinux_enabled_boot`；`selinux_init()`（`hooks.c:7845`）调用 `security_add_hooks(selinux_hooks, ...)`，`enforcing_set(selinux_enforcing_boot)` 决定初始 enforcing；`initcall_device = selinux_initcall` 在设备初始化阶段完成策略相关的收尾。没有加载策略前 `selinux_initialized()` 为假，大多数 hook 直接放行（避免启动早期死锁）。

### 2.6 AVC 缓存内部结构

`security/selinux/avc.c:48-78` 的三层结构：

```c
struct avc_entry {
	u32			ssid;
	u32			tsid;
	u16			tclass;
	struct av_decision	avd;
	struct avc_xperms_node	*xp_node;
};

struct avc_node {
	struct avc_entry	ae;
	struct hlist_node	list; /* anchored in avc_cache->slots[i] */
	struct rcu_head		rhead;
};

struct avc_cache {
	struct hlist_head	slots[AVC_CACHE_SLOTS];
	spinlock_t		slots_lock[AVC_CACHE_SLOTS];
	atomic_t		lru_hint;
	atomic_t		active_nodes;
	u32			latest_notif;
};
```

- 槽位数 `AVC_CACHE_SLOTS = 1 << CONFIG_SECURITY_SELINUX_AVC_HASH_BITS`（`avc.c:38`），每槽独立自旋锁；读侧走 RCU（`hlist_for_each_entry_rcu`），写侧 `spin_lock_irqsave`；
- 哈希函数 `avc_hash()`（`avc.c:126`）对 `(ssid, tsid, tclass)` 做 `av_hash`；
- 节点从专用 slab `avc_node_cachep` 分配（`avc_init()`，`avc.c:136`，`SLAB_PANIC`），分配用 `GFP_NOWAIT`——判定路径可能处于原子上下文；
- 容量控制：`avc_alloc_node()`（`avc.c:494`）发现 `active_nodes` 超过 `avc_cache_threshold`（默认等于槽位数，`AVC_DEF_CACHE_THRESHOLD`，可经 selinuxfs 调整）就调用 `avc_reclaim_node()`（`avc.c:459`）：从 `lru_hint` 原子递增选槽，`spin_trylock` 避免阻塞，每轮最多回收 `AVC_CACHE_RECLAIM=16` 个节点；
- 扩展权限（ioctl 等）单独挂在 `avc_xperms_node` 上，其 decision 链表通常只有 1-5 项，直接线性查找（`avc.c:174-190` 注释）。

`avc_has_perm_noaudit()`（`avc.c:1145`）的快路径：

```c
rcu_read_lock();
node = avc_lookup(ssid, tsid, tclass);
if (unlikely(!node)) {
	rcu_read_unlock();
	return avc_perm_nonode(ssid, tsid, tclass, requested, flags, avd);
}
denied = requested & ~node->ae.avd.allowed;
memcpy(avd, &node->ae.avd, sizeof(*avd));
rcu_read_unlock();
if (unlikely(denied))
	return avc_denied(ssid, tsid, tclass, requested, 0, 0, 0, flags, avd);
return 0;
```

未命中时走 `avc_perm_nonode()`（`avc.c:1110`，特意 `noinline` 以节省快路径栈空间）：`avc_compute_av()` 调安全服务器计算并回填缓存。`avc_insert()`（`avc.c:606`）先检查 `avc_latest_notif_update()`（策略已变更则放弃插入），再在槽锁内查重——已有同键节点则 `avc_node_replace()` 原子替换，否则 `hlist_add_head_rcu()`。`avc_has_perm()`（`avc.c:1189`）= noaudit 判定 + `avc_audit()`；把判定与审计分开，是为了让调用者能在持锁时判定、放锁后审计。

### 2.7 策略加载的完整流程

`security_load_policy()`（`ss/services.c:2309`）按以下顺序构建新策略：

1. `kzalloc` 新 `selinux_policy` 与新 `sidtab`；
2. `policydb_read()` 解析二进制策略 blob；
3. `selinux_set_mapping()` 建立内核安全类/权限位与策略符号的映射；
4. `policydb_load_isids()` 加载初始 SID（内核对象使用的固定上下文）；
5. **首次加载**（`!selinux_initialized()`）直接返回，不需要保留旧状态；
6. 重载时：`security_preserve_bools()` 保留旧策略中已激活的布尔值；`sidtab_convert()`（`ss/sidtab.c:426`）把旧 SID 表中的上下文逐个转换到新策略的表示（类型/角色/级别可能重编号）；
7. 成功后由调用者（selinuxfs `sel_write_load`）在 `policy_mutex` 下 RCU 替换 `state->policy`，并 `avc_ss_reset()`（`avc.c:954`）递增 seqno 使全部缓存失效。

错误路径逐级回滚（`err_free_convert_data` → `err_free_isids` → `err_mapping` → `err_policydb` → `err_sidtab` → `err_policy`），任何一步失败都不会影响正在使用的旧策略——这是 SELinux 策略重载可以在线进行的原因。

### 2.8 selinuxfs 接口一览

`security/selinux/selinuxfs.c:1831` 起的 `selinux_files[]` 定义了 `/sys/fs/selinux/` 下的文件（摘录）：

| 文件 | 权限 | 用途 |
|------|------|------|
| `load` | 0600 | 写入二进制策略（`sel_write_load`，538 行） |
| `enforce` | 0644 | 读/写 enforcing 状态（`sel_write_enforce`，132 行） |
| `context`/`access`/`create`/`relabel`/`member` | 0666 | 用户态事务式查询：上下文转换、权限判定、创建标签计算等 |
| `policyvers` | 0444 | 内核支持的最高策略版本 |
| `commit_pending_bools` | 0200 | 提交布尔值修改并使 AVC 失效 |
| `checkreqprot` | 0644 | mmap/mprotect 是否按请求 prot 检查 |
| `reject_unknown`/`deny_unknown` | 0444 | 未知类/权限的处理策略 |
| `policy` | 0444 | 读回当前二进制策略 |
| `validatetrans` | 0222 | 校验上下文转换合法性 |

另有 `avc/` 子目录（`selinuxfs.c:1886`）提供缓存统计与阈值调整，`booleans/`（1863 行）、`class/`（1916 行）、`initial_contexts/`（1906 行）、`policy_capabilities/`（1923 行）等目录。用户态 `libselinux` 的 `security_load_policy`、`security_getenforce` 等 API 最终都落到这些文件。

---

## 3. AppArmor

### 3.1 架构

AppArmor 是**路径名 + 名称**模型：不给文件写标签，而是在每次访问时把绝对路径（或 socket 地址、信号等）送进 profile 里编译好的 **DFA（确定性有限自动机）**，得到许可位图。

```
VFS 路径 ──> aa_path_perm()             security/apparmor/file.c
                 │  fn_for_each_confined(label, profile, ...)
                 │      profile_path_perm()
                 │          aa_dfa_match(dfa, start, pathname)  match.c
                 ▼
             struct aa_perms { allow, audit, quiet, deny, ... }
```

### 3.2 关键数据结构

```c
// security/apparmor/include/policy.h
struct aa_profile {
	struct aa_policy base;        /* 名字、子 profile 链表 */
	struct aa_profile __rcu *parent;
	struct aa_ns *ns;
	enum audit_mode audit;       /* AUDIT_NORMAL / AUDIT_QUIET / AUDIT_ALL */
	long mode;                   /* APPARMOR_ENFORCE / APPARMOR_COMPLAIN / APPARMOR_KILL */
	u32 path_flags;
	struct aa_attachment attach; /* exec 时匹配的名字/xattr 条件 */
	struct aa_loaddata *rawdata;
	int n_rules;
	/* 变长 rules[] 紧随其后 */
};

struct aa_ruleset {
	int size;
	struct aa_policydb *policy;   /* 主 DFA */
	struct aa_policydb *file;     /* 文件 DFA */
	struct aa_caps caps;
	struct aa_rlimit rlimits;
	struct aa_secmark *secmark;
};

// security/apparmor/include/label.h
struct aa_label {
	struct aa_common_ref count;
	struct rb_node node;
	struct aa_proxy *proxy;
	__counted char *hname;
	long flags;
	u32 secid;
	u64 mediates;
	union {
		struct {
			struct aa_profile *profile[2];
			DECLARE_FLEX_ARRAY(struct aa_ruleset *, rules);
		};
		DECLARE_FLEX_ARRAY(struct aa_profile *, vec);  /* profile 栈 */
	};
};
```

`aa_label` 是“profile 集合”：`size == 1` 时就是单个 profile，大于 1 表示 profile 栈（stacking），`fn_for_each_confined()` 会对栈中每个模块分别判定，取最严结果。`struct aa_ns`（`security/apparmor/include/policy_ns.h`）提供命名空间隔离，含 `labelset`（红黑树索引的 label 集合）与 `unconfined` 模板。

### 3.3 权限判定与路径匹配

- `apparmor_file_permission()` → `common_file_perm(OP_FPERM, file, mask)` → 取 `cred_label()` 与 file 的 label，调 `aa_file_perm()`。
- 路径类操作走 `aa_path_perm(op, subj_cred, label, path, flags, request, cond)`：先 `aa_get_buffer()` 取 per-cpu 路径缓冲，再 `profile_path_perm()` 用 `d_path()`/`aa_path_name()` 生成规范化路径，交给 `aa_dfa_match()` 在 `file` DFA 上运行。
- DFA 由用户态 `apparmor_parser` 编译，二进制 blob 经 `aa_dfa_unpack()`（`match.c:315`）校验 `table_header`、重定位 `data16→data32`、`verify_table_headers()` 后挂载。`aa_dfa_match` 从 `DFA_START` 逐字符跳转，返回终止状态，状态对应的 `aa_perms` 表给出允许/拒绝/审计位。
- 权限位定义在 `security/apparmor/include/perms.h`（`AA_MAY_READ`、`AA_MAY_WRITE`、`AA_MAY_EXEC`、`AA_MAY_LINK`、`AA_MAY_LOCK`、`AA_EXEC_MMAP` 等），`AA_MAY_CHANGE_PROFILE`/`AA_MAY_ONEXEC` 控制域切换。
- 其它 hook：`apparmor_inode_permission`、`apparmor_socket_*`（`af_unix.c`/`net.c`）、`apparmor_signal_*`、`apparmor_ptrace_*`、`apparmor_capable`（profile 的 `caps` 集合），以及 `apparmor_bprm_creds_for_exec`（exec 时匹配 `attach` 决定新 label）。
- `common_perm_cond`/`path_mediated_fs()` 用来跳过不需要路径中介的文件系统（`path_flags` 中的 `PATH_MEDIATE_DELETED` 等）。

### 3.4 域切换、策略加载与审计

- **域切换**：`aa_change_profile(const char *fqname, int flags)`（`domain.c:1424`）与 `aa_change_hat()`。`flags` 可含 `AA_CHANGE_NOFLAGS`/`AA_CHANGE_TEST`/`AA_CHANGE_STACK`；onexec 通过 `aa_set_current_onexec()` 预置下一次 exec 的目标 label。切换前必须通过 `AA_MAY_CHANGE_PROFILE` 权限检查。
- **策略加载**：`aa_replace_profiles()`（`policy_unpack.c`）解析用户态 blob（`aa_unpack_*` 系列），创建/替换 `aa_profile`，经 `aa_prepare_ns()` 定位命名空间，再 `__replace_profile()` 原子替换并 bump `aa_g_profile_mode` 相关的 revision；接口在 `/sys/kernel/security/apparmor/.replace`、`.load`、`.remove`。
- **审计**：`aa_audit(type, profile, gfp, fmt, ...)`（`audit.c`）统一走 `audit_log_start()`，记录 `apparmor="DENIED"/"ALLOWED"/"AUDIT"`、`profile=`、`name=`、`requested_mask=` 等字段；`AUDIT_MODE(profile)` 结合全局 `aa_g_audit`（`AUDIT_NORMAL`/`AUDIT_QUIET`/`AUDIT_ALL`）决定是否记录。complain 模式下拒绝只记录不阻断，并累积到 `learning` 缓存供 `aa-logprof` 生成规则。
- **初始化**：`DEFINE_LSM(apparmor)` 为 `LSM_FLAG_LEGACY_MAJOR | LSM_FLAG_EXCLUSIVE`，`init = apparmor_init`、`initcall_fs = aa_create_aafs`（建立 `securityfs` 下的 apparmor 目录），可选 `initcall_late = init_profile_hash`。
- **挂接范围**：80 个 hook，覆盖文件、挂载、网络、信号、ptrace、capability、`usr::`、`change_profile` 等。

### 3.5 DFA 匹配与策略解包细节

`aa_dfa_match()`（`match.c:515`）是压缩 DFA 的逐字符驱动：

```c
aa_state_t aa_dfa_match(struct aa_dfa *dfa, aa_state_t start, const char *str)
{
	u32 *def = DEFAULT_TABLE(dfa);
	u32 *base = BASE_TABLE(dfa);
	u32 *next = NEXT_TABLE(dfa);
	u32 *check = CHECK_TABLE(dfa);
	aa_state_t state = start;

	if (state == DFA_NOMATCH)
		return DFA_NOMATCH;
	if (dfa->tables[YYTD_ID_EC]) {
		u8 *equiv = EQUIV_TABLE(dfa);
		while (*str) {
			u8 c = equiv[(u8) *str];
			match_char(state, def, base, next, check, c);
			str++;
		}
	} else {
		while (*str) {
			match_char(state, def, base, next, check, (u8) *str);
			str++;
		}
	}
	return state;
}
```

四张表（default/base/next/check）是双数组压缩 DFA 的变体：`base[state] + c` 给出候选位置，`check` 验证归属，失配时走 `def[state]` 默认转移。若存在等价类表（`YYTD_ID_EC`），先把输入字节映射到等价类再转移，大幅压缩表规模。返回的终止状态用于索引 `aa_perms` 表得到允许/拒绝/审计位。

解包侧：`unpack_table()`（`match.c:34`）校验 `table_header` 魔数/版本/大小，`remap_data16_to_data32()`（`match.c:277`）把用户态 16 位数据重定位为内核 32 位，`verify_table_headers()`（`match.c:104`）交叉校验各表一致性。profile 级解包在 `policy_unpack.c`：`aa_unpack_array()`（348 行）、`aa_unpack_str()`（389 行）、`aa_unpack_strdup()`（413 行）等原语逐个读取带名字段，`unpack_profile()`（`policy_unpack.c:1114`）组装出 `aa_profile`。替换入口 `aa_replace_profiles()` 实际位于 `policy.c:1169`（由 `apparmorfs.c:505` 的写入路径调用；`.load`/`.replace`/`.remove` 接口在 `apparmorfs.c:2097-2111` 创建，权限 0640），负责命名空间定位、新旧 profile 原子替换与引用计数交接。

### 3.6 label 合并与 profile 栈

`aa_label_merge()`（`label.c:1211`）把两个 label 合并成一个（用于 stack 切换）：

```c
struct aa_label *aa_label_merge(struct aa_label *a, struct aa_label *b, gfp_t gfp)
{
	if (a == b)
		return aa_get_newest_label(a);
	...
	a = aa_get_newest_label_condref(a, &a_needput);
	b = aa_get_newest_label_condref(b, &b_needput);
	new = aa_label_alloc(a->size + b->size, NULL, gfp);
	if (!new)
		goto out;
	label = label_merge_insert(new, a, b);
	label_free_or_put_new(label, new);
	...
}
```

要点：先取两侧最新版本（profile 可能被替换，旧 label 会 stale）；按 `a->size + b->size` 预分配；`label_merge_insert()` 归并去重后插入全局 label 索引，若已存在等价 label 则复用并释放新分配。遍历时 `fn_for_each_confined()`（`include/label.h:232`）通过 `aa_label_next_confined()`（`label.c:487`）跳过 unconfined profile，只对栈中真正受限的 profile 逐一判定，任一拒绝即整体拒绝。

---

## 4. Smack

### 4.1 架构

Smack（Simplified Mandatory Access Control Kernel）是**单标签**模型：每个主体（任务）和客体（inode、socket、msg 等）恰好一个 `struct smack_known` 标签，规则由二元组 `(subject, object) → access` 描述，不存在类型/角色/域层次。

```c
// security/smack/smack.h
struct smack_known {
	struct list_head list;
	char *smk_known;              /* 标签字符串 */
	u32 smk_secid;                /* 网络/审计用 secid */
	struct smack_rule *smk_rules;
	struct mutex smk_rules_lock;
	struct list_head smk_rules_list;
	struct smk_netlbladdr *smk_netlbladdr;
	u32 smk_flags;                /* SMK_FLAG_INTERNETLABELS 等 */
};
```

blob 布局（`smack_blob_sizes`）：`lbs_cred = sizeof(struct task_smack)`、`lbs_inode = sizeof(struct inode_smack)`、`lbs_sock = sizeof(struct socket_smack)`、`lbs_superblock = sizeof(struct superblock_smack)`、其余对象只存 `struct smack_known *`。

### 4.2 访问判定 `smk_access()`

`smk_access(subject, object, request, audit)`（`smack_access.c:117`）的顺序是 Smack 语义的全部精华：

1. **硬编码规则**：`*` 主体不能访问任何对象；`^`（web/internet）标签双向全通；`*` 客体任何人可访问；主体与客体标签**相同**则全通；
2. `hat` 主体可读/锁定任意客体，`floor` 客体可被任意主体读/锁定（读写类请求才适用）；
3. 其余情况查 `subject->smk_rules` 中 `(subject, object)` 的规则：`smk_access_entry()` 返回允许位掩码，`(request & may) != request` 则 `-EACCES`；
4. 结尾 `smk_audit()` 记录 `SMACK` 审计（`subject=`、`object=`、`request=`）。

包装层：`smk_curacc()` 取当前任务标签后调用；`smk_tskacc(tsp, obj_known, mode, a)` 从 `task_smack` 取 `smk_task`；exec 时用 `smk_task`/`smk_forked`/`smk_transmuted` 决定新旧标签。`smk_of_task()` 等 inline 从 cred blob 取。

### 4.3 标签来源与网络

- **inode 标签**：`security.smack64` xattr（`SMACK64`）；读不到时用 `smack_mnt_opts` 指定的 `smk_root`，或目录的默认标签。`security.smack64.exec`（`SMACK64EXEC`）、`.mmap`（`SMACK64MMAP`）、`.transmute`（`SMACK64TRANSMUTE`）、`.ip`（`SMACK64IPIN/IPOUT`）是变体。
- **挂载选项**：`smack_sb_eat_lsm_opts()`（`smack_lsm.c:724`）解析 `smackfsdef=`、`smackfsroot=`、`smackfshat=`、`smackfsfloor=`、`smackfstransmute=`，存入 `superblock_smack`。
- **网络**：`socket_smack { smk_in, smk_out, smk_packet }`；发送时 `smack_netlabel_send()` 调 `netlbl_req_setattr()` 把 `smk_out->smk_netlabel` 写到 IP 选项（CIPSO，DOI 默认 3，见 `smack.h` 中 `SMACK_CIPSO_DOI_DEFAULT`），接收时 `smack_socket_sock_rcv_skb()` 解析并对 `smk_in` 做检查。`smk_netlbladdr` 维护“目的地址段 → 标签/`-CIPSO`”映射。
- **smackfs**：`/sys/fs/smackfs/` 由 `smackfs.c` 实现，`load2`（`smk_write_load2()`）加载 `subject object access` 规则，另有 `load-self2`、`change-rule2`、`access2`、`netlabel`、`cipso`、`onlycap`、`ambient`、`relabel-self`、`ptrace`、`unconfined` 等接口；`smk_write_load()` 是兼容的旧格式（整数权限）。
- **特权/豁免**：`onlycap` 指定受 SMACK 约束的标签集合；`CAP_MAC_ADMIN` 用于修改策略/标签；`SMACK_NOTRANSMUTE` 等标志控制 exec 语义。

### 4.4 初始化

`DEFINE_LSM(smack)`：`LSM_FLAG_LEGACY_MAJOR | LSM_FLAG_EXCLUSIVE`，`init = smack_init`、`initcall_device = smack_initcall`。`smack_init()` 初始化 `smack_known_list`、`smack_rule_cache`，并把当前任务标签设为 `smack_known_floor`（内核任务通常为 `^`），同时创建 `smackfs`。122 个 hook 覆盖 inode/file/sb/task/socket/ipc/xattr 全路径。

### 4.5 `smk_access()` 源码逐段

`smack_access.c:117` 的判定顺序与 §4.2 描述一一对应，关键片段：

```c
if (subject == &smack_known_star) {
	rc = -EACCES;
	goto out_audit;
}
if (object == &smack_known_web || subject == &smack_known_web)
	goto out_audit;
if (object == &smack_known_star)
	goto out_audit;
if (subject->smk_known == object->smk_known)
	goto out_audit;
if ((request & MAY_ANYREAD) == request ||
    (request & MAY_LOCK) == request) {
	if (object == &smack_known_floor)
		goto out_audit;
	if (subject == &smack_known_hat)
		goto out_audit;
}
rcu_read_lock();
may = smk_access_entry(subject->smk_known, object->smk_known,
		       &subject->smk_rules);
rcu_read_unlock();
if (may <= 0 || (request & may) != request) {
	rc = -EACCES;
	goto out_audit;
}
```

实现细节：

1. 同标签比较用**指针相等**（`subject->smk_known == object->smk_known`）而非字符串比较——标签字符串全局驻留在 `smack_known_list`，同一标签只有一个 `smack_known` 实例，这使快路径极快；
2. hat/floor 豁免只对“纯读类”（`(request & MAY_ANYREAD) == request`）或 `MAY_LOCK` 请求生效，写请求不享受；
3. 规则查找在 RCU 读侧进行，`smk_access_entry()`（`smack_access.c:83`）遍历 `subject->smk_rules`；规则写入（smackfs `load2`）走互斥锁 + RCU 发布；
4. `CONFIG_SECURITY_SMACK_BRINGUP` 下带 `MAY_BRINGUP` 的规则返回正值 `SMACK_BRINGUP_ALLOW`，让 hook 层识别“因 bringup 规则而放行”的访问并额外审计，用于策略调试；
5. 所有分支统一汇到 `out_audit`：`CONFIG_AUDIT` 下经 `smack_log()` 输出 subject/object/request/rc，保证放行与拒绝都有审计出口。

---

## 5. TOMOYO

### 5.1 架构

TOMOYO 是**路径名 + 域**模型，设计目标是“用审计日志学习出策略”，因此有独有的 **learning** 模式。每个进程属于一个 domain（`tomoyo_domain_info`），domain 上挂 ACL 链表；访问文件时用规范化绝对路径匹配 `path`/`path2`/`path_number`/`mkdev` 等 ACL，并可用 `condition` 做细粒度条件判断。

```c
// security/tomoyo/common.h
struct tomoyo_domain_info {
	struct list_head list;
	struct list_head acl_info_list;         /* 本域 ACL */
	const struct tomoyo_path_info *domainname;
	struct tomoyo_policy_namespace *ns;
	unsigned long group[TOMOYO_MAX_ACL_GROUPS / BITS_PER_LONG];
	u8 profile;                             /* profile 号 */
	bool is_deleted;
	bool flags[TOMOYO_MAX_DOMAIN_INFO_FLAGS];
	atomic_t users;
};

struct tomoyo_path_acl {
	struct tomoyo_acl_info head;   /* type = TOMOYO_TYPE_PATH_ACL */
	u16 perm;                     /* TOMOYO_TYPE_READ/WRITE/... 位图 */
	struct tomoyo_name_union name;
};
```

域的根是 `tomoyo_kernel_domain`（内核线程）；用户域由 exec 时的 domain transition 产生。

### 5.2 权限判定

以 `tomoyo_path_perm(operation, path, target)`（`file.c:808`）为例：

1. `tomoyo_init_request_info(&r, NULL, tomoyo_p2mac[operation])` 按 `profile` 取出该操作的执行模式（`TOMOYO_CONFIG_DISABLED/LEARNING/PERMISSIVE/ENFORCING`），`DISABLED` 直接返回 0；
2. `tomoyo_read_lock()`（SRCU 读锁）后 `tomoyo_get_realpath(&buf, path)` 得到规范化绝对路径；
3. `rmdir`/`chroot` 追加尾斜杠；`symlink` 额外构造 `symlink_target` 并放入 `obj`；
4. `tomoyo_path_permission()` → `tomoyo_check_acl(&r, tomoyo_check_path_acl)`；
5. 若 `r.granted == false` 且模式为 enforcing → `-EACCES`；learning 模式则把该访问追加到策略文件（`tomoyo_add_entry()`），permissive 只记录；
6. 无论结果都经 `tomoyo_save_domain_acl()`/审计写入 `/sys/kernel/security/tomoyo/` 的统计与 `audit` 接口。

`tomoyo_check_acl()`（`domain.c:161`）遍历 `domain->acl_info_list`，跳过 `is_deleted` 与类型不匹配项，调用具体 `check_entry`（路径匹配）与 `tomoyo_condition()`（`condition.c`，支持 `uid`、`gid`、`mode` 位、`path1`/`path2` 等条件）；未命中则继续遍历该域所属 `acl_group`（`domain->ns->acl_group[i]`），全部失败置 `granted = false`。

相关 hook：`tomoyo_path_perm`（read/write/exec/append/unlink/rmdir/truncate/symlink/chroot/unmount）、`tomoyo_path_number_perm`（`create`/`mkdir`/`mknod`/`mkfifo`/`socket` 等带数字参数的操作）、`tomoyo_path2_perm`（`link`/`rename`）、`tomoyo_mkdev_perm`、`tomoyo_network_perm`（`network.c`）、`tomoyo_mount_perm`（`mount.c`）、`tomoyo_env_perm`（`environ.c`）、`tomoyo_ptrace_perm`。

### 5.3 域切换

`tomoyo_find_next_domain(bprm)`（`domain.c:702`）在 `bprm_check_security` 时被调用：取当前域，依次匹配 `tomoyo_transition_control`（`file_pattern`+`program` 规则）、`tomoyo_task_acl`（`manual_domain_transition`）与 `aggregator` 规则，找到目标 domain 名后 `tomoyo_get_domainname()` 查表；找不到则新建 `tomoyo_domain_info` 并加入 `tomoyo_domain_list`。`tomoyo_write_self()`（`securityfs_if.c`）允许进程通过 `self_domain` 手动切换。

### 5.4 策略接口

`/sys/kernel/security/tomoyo/` 由 `securityfs_if.c` 创建（`tomoyo_interface_init`，`initcall_fs`），文件包括 `profile`、`exception_policy`、`domain_policy`、`manager`、`query`、`stat`、`audit`、`self_domain`、`version`。文本策略由 `load_policy.c` 解析，例如：

```
<kernel> /usr/sbin/sshd
file read /etc/ssh/*
file execute /bin/bash
network inet stream 0-65535 127.0.0.1 22
```

`common.c` 负责语法解析、profile 管理（`/profile` 里的 `TOMOYO_CONFIG_*`）、组（`group`）、ACL 存储与 `/sys/kernel/security/tomoyo/stat` 统计。TOMOYO 只有 30 个 hook，是四者中最少的：它把大多数判定收敛到少量路径类 hook。

### 5.5 `tomoyo_check_acl()` 源码与组回退

`domain.c:161` 的完整逻辑：

```c
void tomoyo_check_acl(struct tomoyo_request_info *r,
		      bool (*check_entry)(struct tomoyo_request_info *,
					  const struct tomoyo_acl_info *))
{
	const struct tomoyo_domain_info *domain = r->domain;
	struct tomoyo_acl_info *ptr;
	const struct list_head *list = &domain->acl_info_list;
	u16 i = 0;

retry:
	list_for_each_entry_rcu(ptr, list, list,
				srcu_read_lock_held(&tomoyo_ss)) {
		if (ptr->is_deleted || ptr->type != r->param_type)
			continue;
		if (!check_entry(r, ptr))
			continue;
		if (!tomoyo_condition(r, ptr->cond))
			continue;
		r->matched_acl = ptr;
		r->granted = true;
		return;
	}
	for (; i < TOMOYO_MAX_ACL_GROUPS; i++) {
		if (!test_bit(i, domain->group))
			continue;
		list = &domain->ns->acl_group[i++];
		goto retry;
	}
	r->granted = false;
}
```

三层过滤：`is_deleted`/类型不匹配跳过 → `check_entry` 做路径/参数匹配 → `tomoyo_condition()` 求值附加条件。域自身 ACL 全部未命中后，按 `domain->group` 位图依次回退到命名空间的共享 `acl_group[i]`（`goto retry` 复用同一段遍历代码），全部失败才置 `granted = false`。遍历在 SRCU（`tomoyo_ss`）保护下进行，策略更新侧等待 SRCU 宽限期后再释放旧 ACL，这是 TOMOYO 能在线学习/修改策略而不阻塞访问判定的基础。命中时记录 `r->matched_acl`，供审计与 learning 模式引用。

---

## 6. 当前配置下的实际状态

- `CONFIG_SECURITY=n`：`security/selinux/`、`security/apparmor/`、`security/smack/`、`security/tomoyo/` 都不会被编译；`lsm_list_modules` 也无从返回这些 ID（对应 `LSM_ID_SELINUX/APPARMOR/SMACK/TOMOYO` 定义在 `include/uapi/linux/lsm.h`，但 `MAX_LSM_COUNT` 为 0）。
- 传统 DAC 与 capability 检查仍然生效；`CONFIG_SECURITY_SELINUX_BOOTPARAM`、`selinux=0`、`apparmor=0` 等参数在未启用 `CONFIG_SECURITY` 时不存在。
- 若需在本机构建启用，最小集合为 `CONFIG_SECURITY=y` + `CONFIG_SECURITY_SELINUX=y`（或其他模块）+ 对应的 `CONFIG_LSM`/`lsm=` 顺序；SELinux/AppArmor/Smack 之间因 `LSM_FLAG_EXCLUSIVE` 与 legacy major 语义而互斥，TOMOYO 可与其它模块堆叠但仍受 `CONFIG_LSM` 顺序约束。
- 四个模块的策略路径分别是 `/sys/fs/selinux/`、`/sys/kernel/security/apparmor/`、`/sys/fs/smackfs/`、`/sys/kernel/security/tomoyo/`；当前构建下这些目录均不存在。

## 7. 横向对比：判定路径与 exec 域切换

### 7.1 一次文件访问的判定路径对比

| 模块 | 入口 hook | 主体标识来源 | 客体标识来源 | 判定核心 | 缓存 |
|------|-----------|--------------|--------------|----------|------|
| SELinux | `selinux_inode_permission`（`hooks.c:3243`） | `current_sid()`（task blob） | `isec->sid`（inode blob，来自 xattr） | `avc_has_perm_noaudit` → `security_compute_av` | 任务级 avdcache + 全局 AVC |
| AppArmor | `apparmor_inode_permission`/`aa_path_perm` | `cred_label()`（profile 栈） | 实时 `d_path` 规范化路径 | `aa_dfa_match` 在 file DFA 上运行 | per-cpu 路径缓冲，无判定缓存 |
| Smack | `smack_inode_permission` → `smk_curacc` | `task_smack.smk_task` | `inode_smack`（xattr/挂载默认） | `smk_access` 硬编码规则 + 规则表 | 无（规则表本身小） |
| TOMOYO | `tomoyo_path_perm`（`file.c:808`） | `tomoyo_domain_info` | `tomoyo_get_realpath` 规范化路径 | `tomoyo_check_acl` + condition | 无（ACL 链表遍历） |

差异的本质：SELinux/Smack 是**标签模型**，客体标签持久化在 xattr，判定时只做标签对查表，开销与路径长度无关；AppArmor/TOMOYO 是**路径模型**，每次判定都要生成并匹配路径，开销与路径深度相关，但无需维护磁盘标签。SELinux 的两级缓存（任务 avdcache + 全局 AVC）正是为抵消 avtab 查表 + 约束求值的成本；路径模型则靠 DFA/前缀匹配把成本压到线性。四者的 hook 都经 [01-lsm.md](01-lsm.md) 的 static call 分发，短路语义意味着任一模块拒绝即整体拒绝。

### 7.2 exec 域切换对比

| 模块 | 切换依据 | 关键路径 | 无匹配时行为 |
|------|----------|----------|--------------|
| SELinux | 策略 `type_transition` + `dyntransition` | `selinux_bprm_creds_for_exec`（`hooks.c:2336`）→ `security_transition_sid` | 保持原域；`bprm_check_security` 最终裁决 |
| AppArmor | profile `attach` 匹配（路径/xattr）+ `change_profile`/`onexec` | `apparmor_bprm_creds_for_exec` → `aa_change_profile` 路径 | unconfined 或继承当前 label |
| Smack | `security.SMACK64EXEC` xattr / transmute | `smack_bprm_creds_for_exec` | 继承任务标签 |
| TOMOYO | `transition_control`/`aggregator`/手动域规则 | `tomoyo_find_next_domain`（`domain.c:702`） | 按策略新建域 |

四者都挂在 [01-lsm.md](01-lsm.md) §11.3 描述的 `bprm_*` hook 序列上；IMA 的 `BPRM_CHECK` 测量/评估（见 [03-ima-evm.md](03-ima-evm.md)）作为 `LSM_ORDER_LAST` 模块在域切换裁决之后执行。

### 7.3 策略更新与失效机制对比

- SELinux：整体策略替换 + seqno 递增，AVC/avdcache 按 seqno 惰性失效（§2.6/2.7）；
- AppArmor：profile 级原子替换，label 通过 `aa_get_newest_label` 追踪最新版本，旧 profile 引用计数归零后释放（§3.6）；
- Smack：规则逐条增删（`load2`/`change-rule2`），RCU 发布，无全局失效概念（§4.5）；
- TOMOYO：ACL 逐条追加/删除 + SRCU 宽限期，learning 模式直接把新访问写入策略（§5.5）。

## 关键文件清单

| 路径 | 说明 |
|------|------|
| `security/selinux/hooks.c` | SELinux 221 个 hook 实现、`DEFINE_LSM(selinux)`、`selinux_inode_permission` |
| `security/selinux/avc.c` | AVC 缓存（`avc_node`/`avc_cache`）、`avc_has_perm`、`avc_ss_reset` |
| `security/selinux/ss/services.c` | `security_compute_av`、`security_load_policy`、SID/上下文转换 |
| `security/selinux/ss/policydb.c` | 二进制策略解析与符号表 |
| `security/selinux/ss/sidtab.c` `sidtab.h` | SID → 安全上下文映射 |
| `security/selinux/ss/avtab.c` `conditional.c` `constraint.h` `mls.c` | 类型/布尔/约束/MLS 判定 |
| `security/selinux/include/objsec.h` | `task/inode/file/superblock_security_struct`、blob 取用 inline |
| `security/selinux/include/security.h` | `struct av_decision`、`avc_audit_required()` 声明 |
| `security/selinux/selinuxfs.c` | `/sys/fs/selinux/` 用户态接口 |
| `security/selinux/net*.c` `xfrm.c` `nlmsgtab.c` | 网络与 XFRM hook |
| `security/apparmor/lsm.c` | AppArmor hook 注册、`DEFINE_LSM(apparmor)`、`common_file_perm` |
| `security/apparmor/domain.c` | `aa_change_profile`、`aa_change_hat`、exec 域切换 |
| `security/apparmor/label.c` | `aa_label` 分配/合并/子集判断、label 集合 |
| `security/apparmor/match.c` | DFA 解包（`aa_dfa_unpack`）与匹配（`aa_dfa_match`） |
| `security/apparmor/policy.c` `policy_unpack.c` | profile 替换与二进制策略解析 |
| `security/apparmor/file.c` | `aa_path_perm`、`aa_file_perm`、`path_mediated_fs` |
| `security/apparmor/include/policy.h` `label.h` `policy_ns.h` | `aa_profile`/`aa_ruleset`/`aa_label`/`aa_ns` |
| `security/smack/smack_lsm.c` | Smack 122 个 hook、`smack_inode_permission`、挂载选项解析 |
| `security/smack/smack_access.c` | `smk_access`、`smk_access_entry`、`smk_curacc`、`smk_tskacc` |
| `security/smack/smackfs.c` | `/sys/fs/smackfs/`（`load2`、`change-rule2`、`netlabel`、`cipso`） |
| `security/smack/smack.h` | `smack_known`、`task_smack`、`inode_smack`、`socket_smack`、CIPSO 常量 |
| `security/smack/smack_netfilter.c` `smack_lsm.c` 网络段 | CIPSO 收发与 socket 标签 |
| `security/tomoyo/file.c` | `tomoyo_path_perm`、`tomoyo_path_number_perm`、`tomoyo_path2_perm` |
| `security/tomoyo/domain.c` | `tomoyo_check_acl`、`tomoyo_find_next_domain` |
| `security/tomoyo/common.c` | 策略文本解析、profile/group/ACL 存储 |
| `security/tomoyo/securityfs_if.c` | `/sys/kernel/security/tomoyo/` 接口与 `self_domain` |
| `security/tomoyo/condition.c` | ACL `condition` 表达式求值 |
| `security/tomoyo/load_policy.c` | 策略加载入口 |
| `include/uapi/linux/lsm.h` | `LSM_ID_SELINUX/APPARMOR/SMACK/TOMOYO` |
| `security/apparmor/apparmorfs.c` | `.load`/`.replace`/`.remove` 接口（2097-2111 行）、写入路径调用 `aa_replace_profiles`（505 行） |
| `security/apparmor/policy.c` | `aa_replace_profiles()`（1169 行）：profile 替换主逻辑 |
| `security/selinux/ss/sidtab.c` | `sidtab_convert()`（426 行）：策略重载时的 SID 表转换 |
