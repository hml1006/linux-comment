# cgroup v2（统一层级）核心与控制器

> 源码基线 Linux 7.2。cgroup 核心在 `kernel/cgroup/`，控制器分散在 `kernel/sched/core.c`（cpu）、`mm/memcontrol.c`（memory）、`block/blk-cgroup.c`（io）、`kernel/cgroup/*.c`（pids/cpuset/freezer/hugetlb 等），数据结构在 `include/linux/cgroup-defs.h`。
>
> **配置前提**：本仓库 `.config` 已启用 `CONFIG_CGROUPS=y`、`CONFIG_MEMCG=y`、`CONFIG_BLK_CGROUP=y`、`CONFIG_CGROUP_SCHED=y`、`FAIR_GROUP_SCHED=y`、`CONFIG_CGROUP_PIDS=y`、`CONFIG_CGROUP_FREEZER=y`、`CONFIG_CGROUP_HUGETLB=y`、`CONFIG_CPUSETS=y`、`CONFIG_CGROUP_CPUACCT=y`、`CONFIG_CGROUP_PERF=y`、`CONFIG_CGROUP_BPF=y`、`CONFIG_SCHED_AUTOGROUP=y`。但同时：`CONFIG_PSI` **未启用**（没有 `*.pressure` 文件，`/proc/pressure` 不存在）；`CONFIG_MEMCG_V1`、`CONFIG_CPUSETS_V1` **未启用**（对应 v1 接口不编译）；`CONFIG_BLK_DEV_THROTTLING`、`CONFIG_BLK_CGROUP_IOCOST`、`CONFIG_BLK_CGROUP_IOLATENCY` **未启用**（因此没有 `io.max`、`io.cost.*`、`io.latency`，只有 `io.stat`）；`CONFIG_SWAP` 未启用（没有 `memory.swap.*`）；`CONFIG_CGROUP_RDMA`/`CGROUP_MISC`/`CGROUP_DMEM`/`CGROUP_DEBUG` 均关闭。下文会区分“源码存在”与“当前配置是否存在”。

## 1. 文件与规模

| 文件 | 行数 | 作用 |
|------|------|------|
| `kernel/cgroup/cgroup.c` | 7547 | 核心：层级、css_set、创建/销毁、迁移、v2 基础文件 |
| `kernel/cgroup/cgroup-v1.c` | 1372 | v1 兼容层：legacy 挂载、`tasks`、`release_agent`、`cgroup_no_v1=` |
| `include/linux/cgroup-defs.h` | 982 | `cgroup`/`css_set`/`cgroup_subsys`/`cftype` 定义 |
| `kernel/cgroup/rstat.c` | 771 | 递归统计（per-cpu 无锁更新 + 延迟 flush） |
| `kernel/cgroup/cpuset.c` / `pids.c` / `freezer.c` | 4402 / 460 / 326 | cpuset、pids、freezer 控制器 |
| `kernel/cgroup/namespace.c` | 144 | cgroup namespace |
| `mm/memcontrol.c` | 6068 | memory 控制器 |
| `block/blk-cgroup.c` | 2244 | io 控制器框架（blkg、策略） |
| `kernel/sched/psi.c` | 1725 | PSI（本配置未编译） |

## 2. v1 与 v2 的差异

| 维度 | v1（legacy） | v2（unified / 默认层级） |
|------|--------------|--------------------------|
| 层级 | 可任意多个层级，每个控制器可挂不同层级 | **单一统一层级**（`cgroup2` 文件系统），所有控制器共用一棵树 |
| 挂载 | `mount -t cgroup -o cpu,cpuacct none /sys/fs/cgroup/cpu` | `mount -t cgroup2 none /sys/fs/cgroup` |
| 控制器启用 | 挂载时指定即启用 | 显式写 `cgroup.subtree_control`，父→子逐级启用 |
| 任务列表 | 每层 `tasks`（线程）与 `cgroup.procs`（进程） | 只有 `cgroup.procs`/`cgroup.threads`，按“域/线程”粒度 |
| 内部进程约束/线程粒度 | 无约束；所有控制器可按线程 | **no-internal-process**；只有声明 `.threaded = true` 的控制器支持 threaded 子树 |
| 通知/继承 | `notify_on_release` + `release_agent`；`cgroup.clone_children`（仅 cpuset） | `cgroup.events`（populated/frozen）+ `cgroup.kill`；不自动继承 `subtree_control` |
| 文件命名 | `ss->legacy_name` 前缀（如 `blkio.throttle.*`） | `ss->name` 前缀（如 `io.max`） |
| 委托/内核开关 | 受限；`cgroup_no_v1=all\|named\|<ss>` | chown + `cgroup.subtree_control` + 可选 `nsdelegate` 委派子树；默认层级始终存在 |

`cgroup_file_name()`（`cgroup.c:1568`）决定文件前缀：

```c
snprintf(buf, CGROUP_FILE_NAME_MAX, "%s%s.%s",
	 dbg, cgroup_on_dfl(cgrp) ? ss->name : ss->legacy_name, cft->name);
```

所以同一份 `cftype { .name = "max" }` 在 v2 显示为 `io.max`/`memory.max`/`pids.max`，在 v1 显示为 `blkio.throttle.*`/`memory.limit_in_bytes` 等（由 legacy cftypes 决定）。

## 3. 核心数据结构

### 3.1 `cgroup_subsys_state`（css）

```c
struct cgroup_subsys_state {
	struct cgroup *cgroup;              /* 所属 cgroup（PI） */
	struct cgroup_subsys *ss;           /* 所属子系统（PI） */
	struct percpu_ref refcnt;           /* 引用计数 */
	struct css_rstat_cpu __percpu *rstat_cpu;
	struct list_head sibling, children; /* css 树 */
	int id;                             /* 子系统内唯一，root=1 */
	unsigned int flags;
	unsigned int serial_nr;             /* 全局单调序号 */
	u64 tags;                           /* iter 用标签 */
};
```

css 是**控制器视角下的 cgroup**：每个 (cgroup, 控制器) 对应一个 css，控制器把自己的状态嵌在 css 后面（`container_of`）。

### 3.2 `css_set`：任务侧的组合

```c
struct css_set {
	struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT]; /* 不可变数组 */
	refcount_t refcount;
	struct css_set *dom_cset;
	struct cgroup *dfl_cgrp;
	int nr_tasks;
	struct list_head tasks, mg_tasks, dying_tasks;
	struct list_head task_iters;
	struct list_head e_csets[CGROUP_SUBSYS_COUNT];
	...
};
```

关键设计：**进程不直接指向 `struct cgroup`，而指向 `css_set`**。`css_set` 是“每个控制器各取一个 css”的不可变组合，因此 fork 时只需增加引用计数，无需逐个控制器复制状态；迁移时创建新的 css_set 并把任务挂过去。

css_set 全局哈希表：

```c
static DEFINE_HASHTABLE(css_set_table, CSS_SET_HASH_BITS);
static unsigned long css_set_hash(struct cgroup_subsys_state **css);
```

`find_existing_css_set(old_cset, cgrp, template)`（`cgroup.c:1078`）按目标 cgroup 构造 css 模板并查表；未命中则 `css_set_hash()` + `link_css_set()` 新建，`hash_add(css_set_table, ...)`。

### 3.3 `struct cgroup`

```c
struct cgroup {
	struct cgroup_subsys_state self;    /* ss == NULL，指向自身 */
	unsigned long flags;                /* CGRP_* 位 */
	int level, max_depth;
	int nr_descendants, nr_dying_descendants, max_descendants;
	int nr_populated_domain_children, nr_populated_threaded_children;
	struct kernfs_node *kn;
	struct cgroup_file procs_file, events_file;
	struct cgroup_file psi_files[NR_PSI_RESOURCES];
	u32 subtree_control, subtree_ss_mask;
	u32 old_subtree_control, old_subtree_ss_mask;
	struct cgroup_subsys_state __rcu *subsys[CGROUP_SUBSYS_COUNT];
	struct cgroup_root *root;
	struct list_head cset_links;
	struct list_head e_csets[CGROUP_SUBSYS_COUNT];
	struct cgroup *dom_cgrp, *old_dom_cgrp;
	struct cgroup_rstat_base_cpu __percpu *rstat_base_cpu;
	struct cgroup_base_stat last_bstat, bstat;
	struct list_head pidlists;
	wait_queue_head_t offline_waitq;
	struct psi_group *psi;               /* CONFIG_PSI */
	struct cgroup_bpf bpf;
	struct cgroup_freezer_state freezer;
	DECLARE_FLEX_ARRAY(struct cgroup *, ancestors);
};
```

要点：

- `subsys[ssid]` 是控制器 css；root cgroup 的 `self` 也参与统计；
- `dom_cgrp` 用于 threaded 子树：线程化 cgroup 的资源计入最近的 domain 祖先；
- `cset_links` 串起“哪些 css_set 里有任务落在此 cgroup”；
- `e_csets[ssid]` 用于“某控制器在祖先启用、子 cgroup 未启用”时，把 css_set 的该子系统指针指向祖先 css；
- `ancestors[]` 是到 root 的柔性数组，便于 O(1) 判断祖先关系、避免遍历。

### 3.4 `cgroup_root` 与 `cgroup_subsys`

```c
struct cgroup_root {
	struct kernfs_root *kf_root;
	unsigned int subsys_mask;
	int hierarchy_id;
	struct list_head root_list;
	atomic_t nr_cgrps;
	unsigned int flags;          /* CGRP_ROOT_NS_DELEGATE / FAVOR_DYNMODS / CPUSET_V2_MODE ... */
	char release_agent_path[PATH_MAX];   /* v1 */
	char name[MAX_CGROUP_ROOT_NAMELEN];
	struct cgroup cgrp;          /* 根 cgroup，必须最后 */
};
```

v2 只有一个默认 root（`cgrp_dfl_root`），v1 每个挂载点一个 root。root flags 通过挂载选项设置：`nsdelegate`（`CGRP_ROOT_NS_DELEGATE`）、`memory_localevents`、`memory_recursiveprot`、`memory_hugetlb_accounting`、`pids_localevents`、`favordynmods`（`CGRP_ROOT_FAVOR_DYNMODS`，降低动态 cgroup 迁移开销）等。

`struct cgroup_subsys`（`cgroup-defs.h:781`）是控制器的注册结构：

```c
struct cgroup_subsys {
	struct cgroup_subsys_state *(*css_alloc)(struct cgroup_subsys_state *parent_css);
	int (*css_online)(struct cgroup_subsys_state *css);
	void (*css_offline)(struct cgroup_subsys_state *css);
	void (*css_released)(...);
	void (*css_free)(...);
	void (*css_reset)(...);
	void (*css_killed)(...);
	void (*css_rstat_flush)(struct cgroup_subsys_state *css, int cpu);
	int (*can_attach)(struct cgroup_taskset *tset);
	void (*cancel_attach)(struct cgroup_taskset *tset);
	void (*attach)(struct cgroup_taskset *tset);
	int (*can_fork)(struct task_struct *task, struct css_set *cset);
	void (*cancel_fork)(...);
	void (*fork)(struct task_struct *task);
	void (*exit)(struct task_struct *task);
	void (*release)(struct task_struct *task);
	void (*bind)(struct cgroup_subsys_state *root_css);
	bool early_init:1;
	bool implicit_on_dfl:1;
	bool threaded:1;
	int id;
	const char *name;
	const char *legacy_name;
	struct cgroup_root *root;
	struct idr css_idr;
	struct list_head cfts;
	struct cftype *dfl_cftypes, *legacy_cftypes;
	unsigned int depends_on;
};
```

`depends_on` 表达控制器依赖，例如 `io_cgrp_subsys` 在 `CONFIG_MEMCG` 下 `.depends_on = 1 << memory_cgrp_id`（`block/blk-cgroup.c:1523`），以便回写页时能找到 memcg。`implicit_on_dfl` 的控制器（如 `cpuset` 的某些辅助、`debug`）在 v2 隐式启用且不受 no-internal-process 限制。

### 3.5 `cftype`：控制文件

```c
struct cftype {
	char name[MAX_CFTYPE_NAME];
	char *legacy_name;
	u64 private;
	unsigned int flags;      /* CFTYPE_NOT_ON_ROOT / NS_DELEGATABLE / WORLD_WRITABLE ... */
	...
	int (*seq_show)(struct seq_file *sf, void *v);
	ssize_t (*write)(struct kernfs_open_file *of, char *buf, size_t nbytes, loff_t off);
	...
};
```

cgroup 控制文件就是 kernfs 节点，`cftype` 是回调集合；`file_offset` 可把 `struct cgroup_file` 挂进 `cgroup.procs`/`cgroup.events`/`*.pressure` 以便 poll/通知。

## 4. 层级与任务归属

### 4.1 初始化

```
start_kernel
 └─ cgroup_init_early()        cgroup.c:6406
      ├─ 静态初始化 cgrp_dfl_root（默认层级）
      ├─ 初始化 init_css_set
      └─ 对 early_init 的子系统调用 cgroup_init_subsys()
 └─ cgroup_init()              cgroup.c:6447
      ├─ 逐个子系统 cgroup_init_subsys()
      ├─ css_set 哈希初始化、/proc/cgroups
      └─ 注册 cgroup / cgroup2 文件系统
```

`cgroup2_fs_type`（`cgroup.c:2397`）名称为 `"cgroup2"`，`FS_USERNS_MOUNT`，允许在 user namespace 内挂载；v1 的 `cgroup_fs_type` 仍保留，但受 `cgroup_no_v1=` 限制。

### 4.2 创建 cgroup

`cgroup_create(parent, name, mode)`（`cgroup.c:5890`）：

1. `kzalloc_flex(*cgrp, _low_ancestors, level)` 分配并初始化 `percpu_ref self.refcnt`；
2. `kernfs_create_dir_ns()` 创建目录节点 `cgrp->kn`；
3. `init_cgroup_housekeeping()`、`css_rstat_init(&cgrp->self)`、`psi_cgroup_alloc(cgrp)`（PSI 关闭时为空函数）；
4. 填充 `ancestors[]`，继承父的 freeze 状态/`CGRP_NOTIFY_ON_RELEASE`/`CGRP_CPUSET_CLONE_CHILDREN`；
5. `blocking_notifier_call_chain_robust(&cgroup_lifetime_notifier, ...)`；
6. 挂到父的 `self.children`，更新 `nr_descendants`；
7. 非默认层级继承 `subtree_control`；v2 上子 cgroup 的 `subtree_control` 初始为 0，由用户显式开启；
8. `cgroup_apply_control_enable()` 按 `subtree_ss_mask` 逐个 `css_create()`；
9. `kernfs_activate(cgrp->kn)` 使文件对用户可见。

`css_create()`（`cgroup.c:5837`）调用 `ss->css_alloc(parent_css)`，失败则回滚；成功但父级未 online 时挂到 `css->parent->children` 等待。online 时机由 `css_online()` 完成。

### 4.3 销毁

`rmdir` → `cgroup_rmdir()` → `cgroup_destroy_locked(cgrp)`（`cgroup.c:6242`）：

1. 要求无任务（`css_task_iter` 取不到 task）且无 online 子 cgroup，否则 `-EBUSY`；
2. `cgrp->self.flags &= ~CSS_ONLINE`，并遍历 `cset_links` 把相关 css_set 标记 `dead`，阻止新的迁移与子 cgroup 创建；
3. 对每个 css 调 `kill_css_sync()` → `css_offline()`（控制器释放资源、拒绝新 attach）；
4. `css_clear_dir()` + `kernfs_remove(cgrp->kn)` 移除控制文件；更新祖先的 `nr_descendants`/`nr_dying_descendants`；
5. `percpu_ref_kill(&cgrp->self.refcnt)`，待最后一个引用释放后 `css_release()` → `css_free()`，最后 `cgroup_put(parent)`。

v1 额外触发 `notify_on_release` + `release_agent`（`cgroup-v1.c`），v2 由用户态监听 `cgroup.events` 的 `populated 0` 自行处理。

## 5. 控制器启用：`cgroup.subtree_control`

v2 的控制器**逐级启用**：父写 `+cpu +memory` 后，子 cgroup 才出现对应控制文件。

`cgroup_subtree_control_write()`（`cgroup.c:3540`）流程：

1. 解析 `" +cpu -memory "` 形式的 token，映射到 `enable`/`disable` 位图（带 `cgrp_dfl_inhibit_ss_mask` 过滤）；
2. `cgroup_kn_lock_live()` 取活 cgroup；
3. 对每个 enable：要求父的 `cgroup_control(cgrp)` 已包含该控制器，否则 `-ENOENT`；
4. 对每个 disable：要求没有存活的子 cgroup 仍启用它，否则 `-EBUSY`；
5. `cgroup_vet_subtree_control_enable()` 做控制器自定义校验（如 cpuset 的 `cpuset_v2_mode`）；
6. `cgroup_save_control()` 备份旧掩码 → 更新 `subtree_control` → `cgroup_apply_control()`（内部 `cgroup_apply_control_enable()` 创建缺失 css，`cgroup_apply_control_disable()` 移除）→ `cgroup_finalize_control()`；
7. `kernfs_activate()` 刷新生效。

`cgroup.controllers` 只读列出“本 cgroup 可用（父已启用）的控制器”；`cgroup.subtree_control` 列出/设置“将向子 cgroup 下发的控制器”。

## 6. 任务迁移

写 `cgroup.procs`（或 `cgroup.threads`）触发迁移：

```
__cgroup_procs_write()
 └─ cgroup_attach_permissions(src_cgrp, dst_cgrp, sb, threadgroup)   cgroup.c:5383
      └─ cgroup_procs_write_permission()                             cgroup.c:5352
           ├─ 检查对目标 cgroup.procs 的写权限（含 nsdelegate 规则）
           └─ cgroup_is_valid_domain()/no-internal-process 校验
 └─ cgroup_migrate_vet_dst() / cgroup_migrate_add_src()               cgroup.c:2872
 └─ cgroup_migrate_prepare_dst()                                      cgroup.c:2906
 └─ cgroup_migrate()  → cgroup_migrate_execute()                      cgroup.c:2689
```

`cgroup_migrate_execute()` 的结构值得记住：

1. 先对 `mgctx->ss_mask` 内所有控制器调用 `ss->can_attach(tset)`；任一失败则 `cancel_attach()` 回滚，**此阶段不改变任何任务状态**；
2. 提交点：持 `css_set_lock`，对 `tset->src_csets` 里的每个任务执行 `css_set_move_task(task, from_cset, to_cset, true)`，`to_cset->nr_tasks++`、`from_cset->nr_tasks--`，并调用 `cgroup_freezer_migrate_task()` 处理冻结状态；
3. 之后调 `ss->attach(tset)` 通知控制器迁移完成；
4. `cgroup_migrate_finish()` 释放临时 css_set 引用。

`struct cgroup_mgctx`（`cgroup-internal.h:137`）持有预加载的 src/dst css_set 列表与 `cgroup_taskset`，保证迁移的“全成功或全失败”。

`cgroup.threads` 走线程粒度（需 `threaded` 控制器或 threaded 子树），`cgroup.procs` 走进程粒度（迁移整个线程组）。

## 7. fork/exit 与 css_set 生命周期

```c
void cgroup_fork(struct task_struct *child)
{
	RCU_INIT_POINTER(child->cgroups, &init_css_set);
	INIT_LIST_HEAD(&child->cg_list);
}
```

- fork 早期先把子任务挂到 `init_css_set`（安全占位）；
- `cgroup_css_set_fork()`（在 `copy_process()` 的 `cgroup_can_fork` 阶段）根据父任务与目标 cgroup 计算子任务的 css_set，调用控制器 `can_fork()`；
- `cgroup_post_fork(child, kargs)`（`cgroup.c:6984`）在 `css_set_lock` 下把子任务链入目标 css_set（`css_set_move_task`），处理 `CGRP_FREEZE`（置 `JOBCTL_TRAP_FREEZE`）与 `kill_seq`（若 cgroup 已被 `cgroup.kill`，立即终止），然后调用 `ss->fork()`；
- `cgroup_exit()` 把任务转入 `dying_tasks`，最终 `put_css_set()`，可能触发 css_set 释放。

`cgroup.kill`（`cgroup.c:5535`）写 `1` 后对整棵子树 `__cgroup_kill()`，把每个任务标记 `SIGKILL`，并用 `kill_seq` 防止 fork 竞态产生漏网任务。

## 8. 控制器

### 8.1 CPU（`kernel/sched/core.c`）

```c
struct cgroup_subsys cpu_cgrp_subsys = {
	.css_alloc = cpu_cgroup_css_alloc,
	.css_online = cpu_cgroup_css_online,
	.css_offline = cpu_cgroup_css_offline,
	.css_released = cpu_cgroup_css_released,
	.css_free = cpu_cgroup_css_free,
	.can_attach = cpu_cgroup_can_attach,
	.attach = cpu_cgroup_attach,
	.legacy_cftypes = cpu_legacy_files,
	.dfl_cftypes = cpu_files,
};
```

v2 接口：

- `cpu.max`：`"$MAX $PERIOD"`（us），`max` 表示不限额；对应 `cpu_quota_write()`/`cpu_max_show`；
- `cpu.weight`：CFS 权重（1..10000），映射到 `se->load.weight`；
- `cpu.stat`：`usage_usec`、`user_usec`、`system_usec`、`nr_periods`、`nr_throttled`、`throttled_usec`；
- `cpu.pressure`：PSI（本配置关闭 PSI，因此不存在）。

实现基础是 `task_group`/`sched_entity` 的层级：`cpu_cgroup_css_alloc()` 建 `task_group`，`cpu_cgroup_attach()` 把任务 `sched_move_task()` 到新 tg。

### 8.2 Memory（`mm/memcontrol.c`）

```c
struct cgroup_subsys memory_cgrp_subsys = {
	.css_alloc = mem_cgroup_css_alloc,
	.css_online = mem_cgroup_css_online,
	.css_offline = mem_cgroup_css_offline,
	.css_released = mem_cgroup_css_released,
	.css_free = mem_cgroup_css_free,
	.css_reset = mem_cgroup_css_reset,
	.css_rstat_flush = mem_cgroup_css_rstat_flush,
	.attach = mem_cgroup_attach,
	.fork = mem_cgroup_fork,
	.exit = mem_cgroup_exit,
	.dfl_cftypes = memory_files,
#ifdef CONFIG_MEMCG_V1
	.legacy_cftypes = mem_cgroup_legacy_files,
#endif
};
```

v2 文件（`memory_files[]`，`memcontrol.c:5002`）以 `memory.` 为前缀：`current`、`peak`、`min`、`low`、`high`、`max`、`events`、`events.local`、`stat`、`numa_stat`、`oom.group`、`reclaim`；`swap.*` 依赖 `CONFIG_SWAP`/swap 记账（本配置 `CONFIG_SWAP=n`，因此不存在）。

关键语义：

- `memory.current`：`page_counter_read(&memcg->memory)` 加上 swap（若启用）；
- `memory.max`：硬限额，`memory_max_write()` 设置 `page_counter`，超限时 `try_charge()` 触发 reclaim，仍失败则 OOM（`mem_cgroup_oom()`）；
- `memory.high`：软限额，超出后强制节流回收，不直接 OOM；
- `memory.low`/`memory.min`：保护性限额，回收时优先保护；
- `memory.events`：`low/high/max/oom/oom_kill/oom_group_kill` 计数器；
- `memory.reclaim`：主动回收（写 `"1G"` 等）；
- `memory.oom.group`：整组 OOM kill。

记账路径：`try_charge()`（`mem_cgroup_try_charge()`）→ `page_counter_try_charge()` → `memcg_check_events()`；统计汇总由 rstat（`mem_cgroup_css_rstat_flush`）完成。

### 8.3 IO（`block/blk-cgroup.c`）

```c
struct cgroup_subsys io_cgrp_subsys = {
	.css_alloc = blkcg_css_alloc,
	.css_online = blkcg_css_online,
	.css_offline = blkcg_css_offline,
	.css_free = blkcg_css_free,
	.css_rstat_flush = blkcg_rstat_flush,
	.dfl_cftypes = blkcg_files,
	.legacy_cftypes = blkcg_legacy_files,
	.legacy_name = "blkio",
	.exit = blkcg_exit,
#ifdef CONFIG_MEMCG
	.depends_on = 1 << memory_cgrp_id,
#endif
};
```

- 核心对象是 `struct blkcg`（对应 css）与 `struct blkg`（每个 `(blkcg, request_queue)` 一个），`blkg_lookup()`/`blkg_create()` 维护；
- v2 基础文件 `blkcg_files[]` 只有 `stat`、`reset_stats`（显示为 `io.stat`、`io.reset_stats`）；
- 具体策略通过 `blkcg_policy` 注册额外 cftype：`blk-throttle` 提供 `max`（`io.max`，`throtl_files`，`blk-throttle.c:1622`），`blk-iocost` 提供 `weight`/`cost.qos`/`cost.model`（`io.cost.*`），`blk-iolatency` 提供 `latency`（`io.latency`）；
- 本配置 `BLK_DEV_THROTTLING`/`BLK_CGROUP_IOCOST`/`BLK_CGROUP_IOLATENCY` 均未启用，所以实际只有 `io.stat`/`io.reset_stats`；
- `blkg_conf_prep()`（`blk-cgroup.c:828`）解析 `MAJ:MIN value` 形式的策略参数，`blkcg_print_stat()`（1181）输出 `io.stat`。

### 8.4 PIDs（`kernel/cgroup/pids.c`）

```c
struct cgroup_subsys pids_cgrp_subsys = {
	.css_alloc = pids_css_alloc,
	.css_free = pids_css_free,
	.can_attach = pids_can_attach,
	.cancel_attach = pids_cancel_attach,
	.can_fork = pids_can_fork,
	.cancel_fork = pids_cancel_fork,
	.release = pids_release,
	.legacy_cftypes = pids_files_legacy,
	.dfl_cftypes = pids_files,
	.threaded = true,
};
```

文件：`pids.max`（`pids_max_write`，可写 `max`）、`pids.current`、`pids.peak`、`pids.events`（`max` 次数）、`pids.events.local`。限制在 `pids_can_fork()` 中通过 `pids_try_charge()` 原子扣减 `pids->counter` 实现，扣减失败则 fork 返回 `-EAGAIN`；`cancel_fork()` 在 fork 后续失败时归还配额。`threaded = true` 允许在线程粒度使用。

### 8.5 其它控制器

| 控制器 | subsys 定义 | v2 文件 | 本配置 |
|--------|-------------|---------|--------|
| cpuset | `kernel/cgroup/cpuset.c:3665` | `cpuset.cpus`、`cpuset.mems`、`cpuset.cpus.effective`、`cpuset.mems.effective`、`cpuset.cpus.partition` | 启用（`CPUSETS=y`，v1 接口关闭） |
| freezer | `kernel/cgroup/freezer.c` | `cgroup.freeze`、`cgroup.events`（frozen） | 启用 |
| hugetlb | `mm/hugetlb_cgroup.c` | `hugetlb.<size>.max/current/events` | `CGROUP_HUGETLB=y` |
| cpuacct | `kernel/sched/core.c` | v1 `cpuacct.*`；v2 由 `cpu.stat` 覆盖 | 启用 |
| perf_event | `kernel/events/core.c` | `perf_event` | 启用 |
| bpf | `kernel/bpf/cgroup.c` | `cgroup_bpf` 附属（`BPF_PROG_TYPE_CGROUP_*`） | 启用 |
| rdma | `kernel/cgroup/rdma.c:783` | `rdma.max/current` | 关闭 |
| misc | `kernel/cgroup/misc.c:473` | `misc.*` | 关闭 |
| dmem | `kernel/cgroup/dmem.c:880` | 设备内存配额 | 关闭 |

## 9. rstat：递归统计

cgroup 统计（CPU 时间、内存、IO）不能每次读都遍历整棵树。`kernel/cgroup/rstat.c` 的方案：

- 每个 css 有 per-cpu `rstat_cpu`，更新时只把 css 挂到当前 CPU 的 `rstat_backlog_list`（`llist`，无锁），并沿 `parent` 冒泡；
- `__css_rstat_updated(css, cpu)`（`rstat.c:71`）记录“有脏数据”；
- 读取（如 `cpu.stat`、`memory.stat`）时 `css_rstat_flush()` 先把 backlog 收集到 per-cpu 结构，再自底向上汇总到目标 css，控制器可通过 `css_rstat_flush` 回调补充自定义统计；
- `cgroup_base_stat`（`bstat`/`last_bstat`）保存 `cpu.stat` 的聚合值，`cgroup_base_stat_cputime_show()`（`rstat.c:722`）输出。

## 10. PSI（源码存在，本配置未启用）

PSI（Pressure Stall Information）度量“任务因资源不足而停顿”的时间比例，是 v2 的核心可观测性能力。

- `struct psi_group`（`include/linux/psi_types.h:160`）：`avg_total[]`、`avg[][3]`（10s/60s/300s 指数平均）、`total[][]`、`avg_triggers`、`rtpoll_task`/`rtpoll_timer`；采样周期 `PSI_FREQ = 2*HZ+1`（`psi.c:161`）；
- 每个 cgroup 在 `cgroup_create()` 里 `psi_cgroup_alloc(cgrp)`，`cgroup->psi` 指向 `psi_group`；文件 `cpu.pressure`/`memory.pressure`/`io.pressure`/`irq.pressure`（`cgroup->psi_files[NR_PSI_RESOURCES]`）输出 `some avg10=... total=...` / `full ...`；
- 用户态触发器 `psi_trigger_create()`（`psi.c:1335`）/`psi_trigger_destroy()` 支持 `poll()` 等待阈值，`psi_show()`（`psi.c:1251`）负责格式化；`/proc/pressure/{cpu,memory,io}` 为系统级视图。

**当前 `.config` 中 `CONFIG_PSI` 未设置**，因此 `psi_group` 为空结构、`psi_cgroup_alloc()` 为空、`*.pressure` 文件与 `/proc/pressure` 都不存在。这不是 cgroup v2 缺少该文件类型，而是 PSI 功能未编译。

## 11. 委托（delegation）与 cgroup namespace

v2 的委托模型（把子树交给非 root 用户/容器管理器）：

1. 父级把子目录 `chown` 给目标用户；
2. 该用户可写子目录的 `cgroup.procs`/`cgroup.threads`（`CFTYPE_NS_DELEGATABLE`）与 `cgroup.subtree_control`；
3. 若挂载时指定 `nsdelegate`（`CGRP_ROOT_NS_DELEGATE`），则权限检查按 **cgroup namespace 根** 的视角进行：`cgroup_procs_write_permission()`（`cgroup.c:5352`）用 `cgroup_ns` 判断调用者是否“拥有”该 cgroup，防止容器内进程把任务移出 ns 根；
4. 委托目录中只能启用父级已启用的控制器（`cgroup_control()` 检查），且必须遵守 no-internal-process（除非启用 threaded 子树）。

`kernel/cgroup/namespace.c` 实现 `cgroup_ns`：`struct cgroup_namespace` 记录 `root_cgrp` 与 `ns.inum`，随 `nsproxy` 切换；容器内看到的 `/proc/self/cgroup` 路径相对于 ns 根（`cgroup_path_ns()`，`cgroup.c:2486`）。创建方式为 `unshare(CLONE_NEWCGROUP)` 或挂载 `cgroup2` 时隐式创建。

## 12. v1 兼容层

`kernel/cgroup/cgroup-v1.c` 保留：

- `cgroup1_fs_context_ops`：`mount -t cgroup -o <controllers>` 解析 `none,name=systemd` 等；`release_agent`/`notify_on_release`（`cgroup-v1.c:554`/`599`）在 cgroup 变空时执行用户态程序；`tasks` 与 `cgroup.clone_children` 保留；
- 启动参数 `cgroup_no_v1=all|named|<ss>`（`cgroup-v1.c:1335`）可完全禁用 v1；`cgroup_disable=<ss>`（`cgroup.c:7223`）禁用指定控制器；`/proc/cgroups` 列出每个控制器的 `hierarchy`、`num_cgroups`、`enabled`。

本配置 `MEMCG_V1`/`CPUSETS_V1` 关闭，但 `cgroup-v1.c` 仍会编译（`CONFIG_CGROUPS` 下），只是部分控制器没有 legacy cftypes。

## 13. 当前配置下的 v2 能力清单

| 能力 | 状态 | 说明 |
|------|------|------|
| `cgroup2` 统一层级 / `cgroup.procs`/`cgroup.threads`/`cgroup.subtree_control` | 可用 | `CONFIG_CGROUPS=y`，核心文件齐备 |
| cpu 控制器 | 可用 | `cpu.max`、`cpu.weight`、`cpu.stat`；无 `cpu.pressure` |
| memory 控制器 | 可用 | `memory.{current,peak,min,low,high,max,events,stat,numa_stat,oom.group,reclaim}`；无 `memory.swap.*`（`CONFIG_SWAP=n`） |
| io 控制器 | 部分 | 仅 `io.stat`/`io.reset_stats`；`io.max`/`io.cost.*`/`io.latency` 因对应配置关闭而不存在 |
| pids 控制器 | 可用 | `pids.{max,current,peak,events}` |
| cpuset / freezer / hugetlb | 可用 | |
| PSI | **不可用** | `CONFIG_PSI` 未启用 |
| cgroup namespace | 可用 | `namespace.c`，随 `CONFIG_NAMESPACES=y` |
| rdma/misc/dmem 控制器 | 不可用 | 配置关闭 |
| v1 legacy 接口 | 部分 | `MEMCG_V1`/`CPUSETS_V1` 关闭 |

## 关键文件清单

| 路径 | 说明 |
|------|------|
| `include/linux/cgroup-defs.h` | `cgroup_subsys_state`、`css_set`、`cgroup`、`cgroup_root`、`cgroup_subsys`、`cftype`、CGRP_* 标志 |
| `include/linux/cgroup.h` | 对外 API（`css_get/css_put`、`cgroup_get_e_css`、`cgroup_path_ns`、rstat 接口） |
| `kernel/cgroup/cgroup.c` | 核心：`cgroup_init_early/init`、`cgroup_create`、`cgroup_destroy_locked`、`cgroup_subtree_control_write`、`cgroup_migrate_execute`、`cgroup_post_fork`、`cgroup.kill`、v2 基础文件 |
| `kernel/cgroup/cgroup-v1.c` | v1 挂载、`release_agent`、`cgroup_no_v1=`、legacy 基础文件 |
| `kernel/cgroup/cgroup-internal.h` | `cgroup_mgctx`、内部锁与声明 |
| `kernel/cgroup/rstat.c` | `__css_rstat_updated`、`css_rstat_flush`、`cgroup_base_stat` |
| `kernel/cgroup/pids.c` | pids 控制器与 `pids_can_fork` |
| `kernel/cgroup/cpuset.c` / `cpuset-v1.c` | cpuset v2/v1 |
| `kernel/cgroup/freezer.c` / `legacy_freezer.c` | freezer v2/v1 |
| `kernel/cgroup/namespace.c` | cgroup namespace |
| `kernel/cgroup/rdma.c` `misc.c` `dmem.c` `debug.c` | 可选控制器 |
| `include/linux/psi_types.h` `kernel/sched/psi.c` | PSI 数据结构与采样/触发器 |
| `mm/memcontrol.c` | `memory_cgrp_subsys`、`try_charge`、`memory.max/high/events/reclaim` |
| `block/blk-cgroup.c` | `io_cgrp_subsys`、`blkg`、`blkg_conf_prep`、`blkcg_print_stat` |
| `block/blk-throttle.c` `blk-iocost.c` `blk-iolatency.c` | `io.max`、`io.cost.*`、`io.latency` 策略 |
| `kernel/sched/core.c` | `cpu_cgrp_subsys`、`cpu.max`/`cpu.weight` |
| `mm/hugetlb_cgroup.c` | hugetlb 控制器 |
