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

### 2.4 策略加载与审计

- 策略加载：`security_load_policy(void *data, size_t len, ...)`（`ss/services.c:2309`），先 `policydb_read()` 解析，再 `sidtab` 转换、`avtab` 替换，最后广播 `avc_ss_reset()`；`security/selinux/selinuxfs.c` 提供 `/sys/fs/selinux/` 接口（`load`、`enforce`、`context`、`class`、`bool`、`avc/cache_stats` 等）。
- 上下文转换：`security_context_to_sid()` / `security_sid_to_context()`（`ss/services.c`），前者把 `u:r:t:s0:c0` 形式字符串映射到 SID，后者反向，`_force`/`_default` 变体用于无法失败的内核路径。
- 审计点：`avc_audit_required()` → `avc_audit()` / `audit_inode_permission()`；permissive 域中的拒绝以 `granted` 形式记录；`selinux_inode_permission` 里 `audited` 是否为零决定是否进 audit 子系统。
- 初始标签：`security_inode_init_security()` 对应 `selinux_inode_init_security()`，根据父目录类型/`fs_use_xattr`/`defcontext` 计算子对象标签并写入 `security.selinux`；超级块行为由 `superblock_security_struct.behavior`（`SECURITY_FS_USE_XATTR`/`GENFS`/`TASK`/`MNTPOINT`）决定。
- 与网络：`netif.c`/`netnode.c`/`netport.c`/`xfrm.c` 实现 `socket_*`、`xfrm_policy` 等 hook，`nlmsgtab.c` 映射 Netlink 消息类型。

### 2.5 初始化

`DEFINE_LSM(selinux)` 带 `LSM_FLAG_LEGACY_MAJOR | LSM_FLAG_EXCLUSIVE` 与 `enabled = &selinux_enabled_boot`；`selinux_init()`（`hooks.c:7845`）调用 `security_add_hooks(selinux_hooks, ...)`，`enforcing_set(selinux_enforcing_boot)` 决定初始 enforcing；`initcall_device = selinux_initcall` 在设备初始化阶段完成策略相关的收尾。没有加载策略前 `selinux_initialized()` 为假，大多数 hook 直接放行（避免启动早期死锁）。

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

---

## 6. 当前配置下的实际状态

- `CONFIG_SECURITY=n`：`security/selinux/`、`security/apparmor/`、`security/smack/`、`security/tomoyo/` 都不会被编译；`lsm_list_modules` 也无从返回这些 ID（对应 `LSM_ID_SELINUX/APPARMOR/SMACK/TOMOYO` 定义在 `include/uapi/linux/lsm.h`，但 `MAX_LSM_COUNT` 为 0）。
- 传统 DAC 与 capability 检查仍然生效；`CONFIG_SECURITY_SELINUX_BOOTPARAM`、`selinux=0`、`apparmor=0` 等参数在未启用 `CONFIG_SECURITY` 时不存在。
- 若需在本机构建启用，最小集合为 `CONFIG_SECURITY=y` + `CONFIG_SECURITY_SELINUX=y`（或其他模块）+ 对应的 `CONFIG_LSM`/`lsm=` 顺序；SELinux/AppArmor/Smack 之间因 `LSM_FLAG_EXCLUSIVE` 与 legacy major 语义而互斥，TOMOYO 可与其它模块堆叠但仍受 `CONFIG_LSM` 顺序约束。
- 四个模块的策略路径分别是 `/sys/fs/selinux/`、`/sys/kernel/security/apparmor/`、`/sys/fs/smackfs/`、`/sys/kernel/security/tomoyo/`；当前构建下这些目录均不存在。

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
