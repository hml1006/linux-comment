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

## 14. 源码实况订正：本树没有 `cgroup-v2.c`

前文（第 1 节）的表格是按“cgroup v2 语义分层”列的，但落到本树的实际文件布局时有一处必须更正：**Linux 7.2 的中文注释树里不存在 `kernel/cgroup/cgroup-v2.c`**，v2 的统一层级（默认层级）逻辑全部在 `kernel/cgroup/cgroup.c` 内（`cgroup2_fs_type` 定义在 `cgroup.c:2397`，`cgroup_fs_type` 在 `cgroup.c:2389`，二者同在 `cgroup_init()` 中注册，见 `cgroup.c:6538`/`cgroup.c:6539`）。v2 与 v1 的差异靠 `cgroup_on_dfl()`（`cgroup.c:350`）在运行时分支，而不是靠文件名分支。

实际目录：

```text
kernel/cgroup/
├── cgroup.c         7547 行  核心 + v2 统一层级 + 基础文件（cgroup.*）
├── cgroup-v1.c      1372 行  仅 v1 兼容层
├── cgroup-internal.h 296 行  cgroup_mgctx、内部声明
├── rstat.c           771 行  递归统计
├── pids.c            460 行  pids 控制器
├── freezer.c         326 行  v2 freezer（legacy_freezer.c 是 v1）
├── cpuset.c 4402 / cpuset-v1.c 875
├── namespace.c       144 行  cgroup namespace
└── rdma.c misc.c dmem.c debug.c  （本配置全关）
```

因此下文凡是“v2 核心在 `cgroup-v2.c`”的表述，都应按 `kernel/cgroup/cgroup.c` 理解。

本树 v2 基础文件由 `cgroup_base_files[]`（`cgroup.c` 中 `static struct cftype cgroup_base_files[]`）定义，逐个核对如下（这比笼统说“cgroup.procs/cgroup.subtree_control”更准确）：

| 文件 | 定义处 | 关键回调 | 备注 |
|------|--------|----------|------|
| `cgroup.type` | `cgroup_base_files[]` | `cgroup_type_show`/`cgroup_type_write` | `CFTYPE_NOT_ON_ROOT`，写 `threaded`/`domain` |
| `cgroup.procs` | 同上 | `cgroup_procs_start/next/show`、`cgroup_procs_write` | `CFTYPE_NS_DELEGATABLE`，`file_offset = procs_file` |
| `cgroup.threads` | 同上 | 同上 + `cgroup_threads_write` | 线程粒度，`CFTYPE_NS_DELEGATABLE` |
| `cgroup.controllers` | 同上 | `cgroup_controllers_show` | 只读，显示 `cgroup_control()` |
| `cgroup.subtree_control` | 同上 | `cgroup_subtree_control_show/write` | `CFTYPE_NS_DELEGATABLE` |
| `cgroup.events` | 同上 | `cgroup_events_show` | `file_offset = events_file`，可 poll |
| `cgroup.max.descendants` / `cgroup.max.depth` | 同上 | `cgroup_max_descendants_*` / `cgroup_max_depth_*` | 限制子树规模 |
| `cgroup.stat` / `cgroup.stat.local` | 同上 | `cgroup_stat_show` / `cgroup_core_local_stat_show` | `nr_descendants`、`nr_dying_descendants` |
| `cgroup.freeze` | 同上 | `cgroup_freeze_show/write` | 调 `cgroup_freeze()` |
| `cgroup.kill` | 同上 | `cgroup_kill_write` | 只接受写 `1` |
| `cpu.stat` / `cpu.stat.local` | 同上（**核心文件，不属 cpu 控制器**） | `cpu_stat_show` / `cpu_local_stat_show` | 因此即使没有 cpu 控制器也有 `cpu.stat` |

`cpu.stat` 由核心提供这一点很实用：判断“cpu 控制器到底有没有启用”不能看 `cpu.stat` 是否存在，而要看 `cpu.weight`/`cpu.max` 是否存在（见第 24 节）。

## 15. `css_set` 全字段与迁移专用字段

前文只给了 `css_set` 的节选。完整定义在 `include/linux/cgroup-defs.h`，其中与迁移/线程化相关的字段值得单独记：

```c
struct css_set {
	struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT]; /* 创建后不可变 */
	refcount_t refcount;
	struct css_set *dom_cset;          /* 线程化 cset → 最近 domain cset */
	struct cgroup *dfl_cgrp;
	int nr_tasks;
	struct list_head tasks, mg_tasks, dying_tasks;
	struct list_head task_iters;
	struct list_head e_cset_node[CGROUP_SUBSYS_COUNT];
	struct list_head threaded_csets, threaded_csets_node;
	struct hlist_node hlist;           /* css_set_table 桶链 */
	struct list_head cgrp_links;
	struct list_head mg_src_preload_node, mg_dst_preload_node, mg_node;
	struct cgroup *mg_src_cgrp, *mg_dst_cgrp;
	struct css_set *mg_dst_cset;
	bool dead;
	struct rcu_head rcu_head;
};
```

四个要点：

1. `subsys[]` **创建后不可变**（只有启动期 `init_css_set` 例外），所以“改 cgroup 归属”必然是换一个 `css_set`，而不是改数组；
2. 迁移的临时状态全挂在 cset 上：`mg_src_cgrp`/`mg_dst_cgrp`/`mg_dst_cset` 标明“这次迁移从哪到哪”，`mg_tasks` 是“正在迁移中的任务”链表，任务本身从 `tasks` 移到 `mg_tasks`（`cgroup_migrate_add_task()`，`cgroup.c:2588`），提交成功后再落到目标 cset 的 `tasks`；
3. `dead` 由 `cgroup_destroy_locked()` 在删除 cgroup 时置位（`cgroup.c:6275` 附近），迁移路径会忽略 `dead` 的 cset；
4. `threaded_csets` 挂在 `dom_cset` 上，线程化子树的 cset 通过 `dom_cset` 把资源算到最近的 domain 祖先。

`css_set_hash()`（`cgroup.c:939`）对 `subsys[]` 做哈希，`find_existing_css_set()`（`cgroup.c:1078`）先查表复用，`find_css_set()`（`cgroup.c:1205`）在未命中时 `link_css_set()`（`cgroup.c:1172`）新建并 `hash_add()`。这正是“fork 几乎零成本”的原因：子进程默认复用父的 cset，只有 `CLONE_INTO_CGROUP` 才走 `find_css_set()` 新建。

## 16. 文件访问协议：`cgroup_kn_lock_live()` 与 kernfs 活跃保护

cgroup 的每个控制文件都是 kernfs 节点，但**回调不能直接持 `cgroup_mutex` 进去**，否则会与 kernfs 的 `active_ref` 保护形成锁序反转（尤其 rmdir 自己）。标准入口是：

```text
kernfs 回调（cgroup_file_write / cgroup_seqfile_show ...）
 └─ cgroup_kn_lock_live(kn, drain_offline)        cgroup.c:1695
      ├─ kn->priv（目录）或 kn_priv(kn)（文件所属 cgroup）
      ├─ cgroup_tryget(cgrp)：拿活引用，失败返回 NULL
      ├─ kernfs_break_active_protection(kn)：主动放弃 kernfs 活跃保护
      ├─ drain_offline ? cgroup_lock_and_drain_offline(cgrp) : cgroup_lock()
      └─ cgroup_is_dead(cgrp) ? cgroup_kn_unlock() : 返回 cgrp
 └─ 业务逻辑
 └─ cgroup_kn_unlock(kn)                          cgroup.c:1663
      ├─ cgroup_unlock()
      ├─ kernfs_unbreak_active_protection(kn)
      └─ cgroup_put(cgrp)
```

“break active protection → 加锁 → 校验存活”的顺序使得 **`cgroup_mutex` 嵌套在 kernfs 活跃引用之外**，从而允许写文件的过程中删除自己所在的 cgroup。`drain_offline=true` 用于 `cgroup.subtree_control` 这类需要等待 offline css 排空的写操作（`cgroup.c:3578`）。

由此可得一条排障经验：如果看到 cgroup 回调里报 `-ENODEV`，说明进入时 cgroup 已 `dead`；如果卡在 `cgroup_lock_and_drain_offline()`，说明有 css 的 `css_offline()` 迟迟不返回（典型是 memcg 的离线 drain）。

## 17. 控制文件的读写实现：`cgroup_add_file()` / `cgroup_seqfile_show()`

`cgroup_add_file()`（`cgroup.c:4437`）是“cftype → kernfs 节点”的唯一入口：

```c
kn = __kernfs_create_file(cgrp->kn, cgroup_file_name(cgrp, cft, name),
			  cgroup_file_mode(cft),
			  current_fsuid(), current_fsgid(),
			  0, cft->kf_ops, cft, NULL, key);
if (cft->file_offset) {
	struct cgroup_file *cfile = (void *)css + cft->file_offset;
	timer_setup(&cfile->notify_timer, cgroup_file_notify_timer, 0);
	spin_lock_init(&cfile->lock);
	cfile->kn = kn;
}
```

- 文件名由 `cgroup_file_name()` 拼（`ss->name` 或 `ss->legacy_name` + `.` + `cft->name`）；
- 权限由 `cgroup_file_mode()`（`cgroup.c:1592`）算：`CFTYPE_WORLD_WRITABLE`/`CFTYPE_NS_DELEGATABLE` 会改变位；
- `file_offset` 非零时，把 `struct cgroup_file`（`kn` + `notify_timer` + `lock`）嵌在对应 css 里，这样 `cgroup_file_notify()` 能按需唤醒 poll 者（`pids.events`、`cgroup.events`、`*.pressure` 都靠它）。

读路径统一走 seq_file：

```text
read(2)
 → cgroup_seqfile_start/next/stop（cgroup.c:4377/4382/4387）
 → cgroup_seqfile_show(seq, arg)                      cgroup.c:4393
     ├─ cft->seq_show 存在 → 直接调用（多数控制器）
     ├─ 否则 cft->read_u64 → seq_printf("%llu\n")
     └─ 否则 cft->read_s64 → seq_printf("%lld\n")
```

写路径统一走 `cgroup_file_write()`（`cgroup.c:4314`），它先 `cgroup_kn_lock_live()` 再按 `cft->write`/`write_u64`/`write_s64` 分派；`poll` 走 `cgroup_file_poll()`（`cgroup.c:4367`）。

还有一个容易忽略的细节：`cgroup_file_open()`（`cgroup.c:4277`）会把**打开文件那一刻的 `cgroup_ns` 存进 `cgroup_file_ctx`**（`get_cgroup_ns()`），`cgroup_file_release()` 再 `put_cgroup_ns()`。委托权限检查因此用的是“打开文件时的 ns”，而不是“写入时的 ns”——这就是前文所说“防止继承 fd 攻击”的实现基础。

## 18. `cgroup.procs` 写入全链路（含锁序）

把前文第 6 节的骨架补全到函数级：

```text
write(cgroup.procs, "<pid>")
 → cgroup_procs_write()                          (cgroup_base_files 的 .write)
   → __cgroup_procs_write(of, buf, threadgroup=true)        cgroup.c:5404
      ├─ dst_cgrp = cgroup_kn_lock_live(of->kn, false)
      ├─ task = cgroup_procs_write_start(buf, true, &lock_mode)   cgroup.c:3046
      │    ├─ kstrtoint → pid（0 表示 current）
      │    ├─ find_task_by_vpid()（RCU 下）→ threadgroup 时取 group_leader
      │    ├─ 拒绝 no_cgroup_migration / PF_NO_SETAFFINITY（kthread 陷阱）
      │    ├─ 选 lock_mode：
      │    │    pid!=0 或 threadgroup → CGRP_ATTACH_LOCK_GLOBAL（percpu_down_write）
      │    │    或 CGRP_ATTACH_LOCK_PER_THREADGROUP（tsk->signal->cgroup_threadgroup_rwsem）
      │    │    仅当前线程自身 → CGRP_ATTACH_LOCK_NONE
      │    ├─ cgroup_attach_lock(lock_mode, tsk)      cgroup.c:2533
      │    │    └─ cpus_read_lock() 之后才 percpu_down_write/写线程组 rwsem
      │    └─ threadgroup 且 !thread_group_leader → 丢掉重试（与 de_thread 竞争）
      ├─ src_cgrp = task_cgroup_from_root(task, &cgrp_dfl_root)
      ├─ cgroup_attach_permissions(src, dst, sb, threadgroup, ns)   cgroup.c:5383
      │    ├─ cgroup_procs_write_permission()      cgroup.c:5352
      │    │    ├─ 求 src/dst 的公共祖先 com_cgrp，要求对 com_cgrp 的 cgroup.procs 有写权限
      │    │    └─ nsdelegate 时：src 与 dst 都必须是 ns->root_cset->dfl_cgrp 的后代，否则 -ENOENT
      │    ├─ cgroup_migrate_vet_dst(dst)          cgroup.c:2796
      │    └─ 非 threadgroup 且 dom_cgrp 不同 → -EOPNOTSUPP
      ├─ cgroup_attach_task(dst, task, threadgroup) cgroup.c:3016
      │    ├─ cgroup_migrate_add_src()（每个 src cset）   cgroup.c:2872
      │    ├─ cgroup_migrate_prepare_dst()               cgroup.c:2919
      │    ├─ cgroup_migrate()                           cgroup.c:2986
      │    │    └─ cgroup_migrate_execute()              cgroup.c:2689
      │    └─ cgroup_migrate_finish()                    cgroup.c:2827
      └─ cgroup_procs_write_finish(task, lock_mode)      cgroup.c:3123
           └─ cgroup_attach_unlock() → put_task_struct()
```

三个设计点：

1. **先 `cpus_read_lock()` 再写 `cgroup_threadgroup_rwsem`**：源码注释（`cgroup.c:2521` 附近）明确说明这样 `->attach()` 可以假设 CPU 热插拔已被禁止，避免 sched 相关回调与 CPU 下线竞争；
2. **`favordynmods`**：挂载选项 `favordynmods` 打开后走每线程组 rwsem（`CGRP_ATTACH_LOCK_PER_THREADGROUP`），把全局 `percpu_down_write()` 的代价降到只锁目标线程组，适合频繁动态创建/迁移 cgroup 的场景；
3. **公共祖先权限模型**：只要对“本源与目标 cgroup 的公共祖先”的 `cgroup.procs` 有写权限即可迁移，这是 v2 允许“把进程向下移入自己拥有的子树”的关键；`nsdelegate` 再叠加“源和目标都必须在自己 ns 根之内”。

## 19. 迁移提交点：`cgroup_migrate_execute()` 的精确语义

前文描述了“can_attach → 提交 → attach”的三段式，这里补齐源码级细节（`cgroup.c:2689`）：

```c
/* 1. 预检：任一失败即回滚，任务状态未变 */
do_each_subsys_mask(ss, ssid, mgctx->ss_mask) {
	if (ss->can_attach) { tset->ssid = ssid; ret = ss->can_attach(tset); ... }
} while_each_subsys_mask();

/* 2. 提交点：持 css_set_lock，切换 css_set 指针 */
spin_lock_irq(&css_set_lock);
list_for_each_entry(cset, &tset->src_csets, mg_node) {
	list_for_each_entry_safe(task, tmp_task, &cset->mg_tasks, cg_list) {
		struct css_set *from_cset = task_css_set(task);
		struct css_set *to_cset = cset->mg_dst_cset;
		get_css_set(to_cset);
		to_cset->nr_tasks++;
		css_set_move_task(task, from_cset, to_cset, true);
		from_cset->nr_tasks--;
		cgroup_freezer_migrate_task(task, from_cset->dfl_cgrp, to_cset->dfl_cgrp);
		put_css_set_locked(from_cset);
	}
}
spin_unlock_irq(&css_set_lock);

/* 3. 通知控制器（此时 tset->csets 已切到 dst_csets） */
tset->csets = &tset->dst_csets;
do_each_subsys_mask(ss, ssid, mgctx->ss_mask)
	if (ss->attach) ss->attach(tset);
```

要点补正：

- 提交点持的是 `css_set_lock` 并 `spin_lock_irq()` 关中断，因为 `task_css_set()` 在中断/调度路径也会被读（如 `task_dfl_cgroup()`）；
- `cgroup_freezer_migrate_task()` 在锁内调用，负责“迁入冻结 cgroup 时置 `JOBCTL_TRAP_FREEZE`、迁出冻结 cgroup 时清标记”；
- `put_css_set_locked(from_cset)` 可能直接触发旧 cset 的释放，所以必须在锁内用 `_locked` 版本；
- **`can_attach` 并非一律只读**：pids 控制器就在 `can_attach` 里就把 pid 计数按“目标 +1、源 -1”调整好（`pids.c:200`），失败由 `cancel_attach`（`pids.c:225`）反向回滚。所以更准确的说法是“提交点之前不改任务的 css_set 归属，但控制器可以预扣资源，必须由 cancel_attach 撤销”。

## 20. `cgroup.type` 与 threaded 子树

`cgroup.type` 的四个取值来自 `cgroup_type_show()`（`cgroup.c:3691`），**判断顺序即优先级**：

| 值 | 判定条件（按源码 if/else 顺序） | 含义 |
|----|------------------------------|------|
| `threaded` | `cgroup_is_threaded(cgrp)` | 只能容纳线程，资源记到 `dom_cgrp` |
| `domain invalid` | `!cgroup_is_valid_domain(cgrp)` | 有内部任务/控制器导致不能作域，需先清理 |
| `domain threaded` | `cgroup_is_thread_root(cgrp)` | 线程化子树的根，同时仍是资源域 |
| `domain` | 以上都不满足 | 普通域 cgroup，可启用任意控制器 |

写 `"threaded"` 走 `cgroup_type_write()`（`cgroup.c:3707`）：只接受 `threaded` 这一个字符串（`strcmp` 不等于即 `-EINVAL`），并且 `cgroup_kn_lock_live(of->kn, true)` 会先 drain 掉 dying css。随后 `cgroup_enable_threaded()`（`cgroup.c:3644`）：

1. `cgroup_is_threaded(cgrp)` 直接返回 0（幂等）；
2. `cgroup_is_populated(cgrp) || (cgrp->subtree_control & ~cgrp_dfl_threaded_ss_mask)` → `-EOPNOTSUPP`；
3. 校验父的域：`cgroup_is_valid_domain(dom_cgrp)` 且 `cgroup_can_be_thread_root(dom_cgrp)`，否则 `-EOPNOTSUPP`；
4. `cgroup_save_control()` → 对 `cgroup_for_each_live_descendant_pre()` 中“自己或已 threaded”的 cgroup 设 `dsct->dom_cgrp = dom_cgrp` → `cgroup_apply_control()`；成功则 `parent->nr_threaded_children++`。

注意源码注释强调“这一步不应引起真正的任务迁移”，所以它比 `cgroup_update_dfl_csses()`（`cgroup.c:3175`，用于 `subtree_control` 变化时重建 css 关联）轻得多。

相关判定函数值得记成一族：

- `cgroup_is_threaded()`（`cgroup.c:386`）：`cgrp->dom_cgrp != cgrp`；
- `cgroup_is_mixable()`（`cgroup.c:392`）：能否同时容纳域级与线程级（`cgroup.type == domain` 且无控制器）；
- `cgroup_can_be_thread_root()`（`cgroup.c:403`）：无内部任务、无启用控制器、父是域；
- `cgroup_is_valid_domain()`（`cgroup.c:447`）：检查 `cgroup_control()` 与“内部进程”约束。

## 21. `cgroup.kill` 与 `kill_seq`：fork 竞态的封堵

前文说“`kill_seq` 防止 fork 竞态产生漏网任务”，具体机制如下：

```text
write(cgroup.kill, "1")
 → cgroup_kill_write()                     cgroup.c:4244
   ├─ 只接受 1，否则 -ERANGE
   ├─ cgroup_is_threaded(cgrp) → -EOPNOTSUPP（kill 是进程级操作）
   └─ cgroup_kill(cgrp)                    cgroup.c:4233
        └─ cgroup_for_each_live_descendant_pre(dsct, css, cgrp)
             └─ __cgroup_kill(dsct)        cgroup.c:4207
                  ├─ spin_lock_irq(&css_set_lock); cgrp->kill_seq++; spin_unlock_irq()
                  ├─ css_task_iter_start(..., CSS_TASK_ITER_PROCS | CSS_TASK_ITER_THREADED)
                  └─ 跳过 PF_KTHREAD / 已 __fatal_signal_pending，其余 send_sig(SIGKILL)
```

`kill_seq` 的两端：

- fork 侧在 `cgroup_css_set_fork()`（`cgroup.c:6788`）持 `css_set_lock` 记录 `kargs->kill_seq = cgrp->kill_seq`（或 `cset->dfl_cgrp->kill_seq`）；
- 收尾在 `cgroup_post_fork()`（`cgroup.c:6984`）再次读取当前 `cgrp_kill_seq`，`kill = kargs->kill_seq != cgrp_kill_seq`，命中则 `do_send_sig_info(SIGKILL, ..., PIDTYPE_TGID)`。

于是“kill 写完之后才完成 fork 的任务”必然看到序号变化而被补杀，不会漏。注意 `cgroup.kill` 只做 `send_sig(SIGKILL)` 而**不等待**，需要确认全部退出要自行 poll `cgroup.events` 的 `populated 0`。

## 22. 销毁路径的引用链与 dying cgroup

`cgroup_destroy_locked()`（`cgroup.c:6242`）在前文 5 步基础上，有几处“顺序不可换”的细节：

```c
/* 1. 有任务就 EBUSY */
css_task_iter_start(&cgrp->self, 0, &it);
task = css_task_iter_next(&it); css_task_iter_end(&it);
if (task) return -EBUSY;

/* 2. 有 online 子 cgroup 也 EBUSY（不能只看 children 是否为空） */
if (css_has_online_children(&cgrp->self)) return -EBUSY;

/* 3. 先摘 CSS_ONLINE，再把所有相关 cset 标 dead */
cgrp->self.flags &= ~CSS_ONLINE;
spin_lock_irq(&css_set_lock);
list_for_each_entry(link, &cgrp->cset_links, cset_link) link->cset->dead = true;
spin_unlock_irq(&css_set_lock);

/* 4. 逐个 kill_css_sync()，然后清目录、摘 kn */
for_each_css(css, ssid, cgrp) kill_css_sync(css);
css_clear_dir(&cgrp->self);
kernfs_remove(cgrp->kn);

/* 5. 更新祖先计数：nr_descendants-- / nr_dying_descendants++ */
/* 6. 通知 CGROUP_LIFETIME_OFFLINE */
/* 7. 基准引用 percpu_ref_kill()，未被 populate 的 css 直接 kill_css_finish() */
```

为什么 `nr_dying_descendants` 要单独计数：`rmdir` 返回后 cgroup 目录已不可见，但只要还有任务持有旧 cset 的引用（比如某个线程正处于 `exec` 中途），`struct cgroup` 就不能释放。这类“已删未死”的 cgroup 会一直挂在祖先的 `nr_dying_descendants` 上，`cgroup.stat` 里能直接看到。排障时若该值持续增长，说明有任务长期占着旧 cset。

`css_create()`（`cgroup.c:5837`）的失败路径也值得注意：分配成功后先 `list_add_tail_rcu(&css->sibling, &parent_css->children)` 再 `online_css()`，失败则 `list_del_rcu()` 并 `queue_rcu_work(cgroup_free_wq, &css->destroy_rwork)`——**css 的释放一律走 RCU work**，因为 `css_task_iter`/`task_css()` 可能在 RCU 读侧持有指针。

## 23. `freezer`：`cgroup.freeze` 的计数与 seqcount

前文只说了 `cgroup.freeze` 文件。实现要点（`kernel/cgroup/freezer.c`）：

```c
/* 是否真的冻结：CGRP_FREEZE 置位 且 冻结任务数 == 任务总数 */
frozen = test_bit(CGRP_FREEZE, &cgrp->flags) &&
	 cgrp->freezer.nr_frozen_tasks == __cgroup_task_count(cgrp);   /* freezer.c:75 */

/* 自底向上传播：所有后代都冻结时，祖先也变 frozen */
static void cgroup_propagate_frozen(struct cgroup *cgrp, bool frozen) /* freezer.c:36 */
```

- `cgroup_enter_frozen()`（`freezer.c:104`）/`cgroup_leave_frozen()`（`freezer.c:128`）都在 `css_set_lock` 下改 `current->frozen` 与 `nr_frozen_tasks`，并调 `cgroup_update_frozen()`（`freezer.c:66`）；
- `cgroup_leave_frozen(false)` 在“正在冻结”时**不**减计数，而是补上 `JOBCTL_TRAP_FREEZE` 并 `set_thread_flag(TIF_SIGPENDING)`，避免状态抖动（源码注释写得很清楚）；
- `cgroup_create()` 里对新 cgroup 做 `seqcount_spinlock_init(&cgrp->freezer.freeze_seq, &css_set_lock)`（`cgroup.c:5944`），并继承父的 `freezer.e_freeze`；父在冻结时新子 cgroup 直接置 `CGRP_FREEZE|CGRP_FROZEN`，因为“空 cgroup 立刻算冻结”。

`cgroup.events` 的 `frozen` 字段就来自这里，配合 `cgroup.events` 的 `file_offset` 与 `cgroup_file_notify()` 可被 `poll()`。

## 24. CPU 控制器：本配置实际暴露的文件

前文列了 `cpu.max`，但本树 `.config` 中 **`CONFIG_CFS_BANDWIDTH is not set`**，因此 `cpu_files[]`（`kernel/sched/core.c:10504`）里受 `CONFIG_GROUP_SCHED_BANDWIDTH` 保护的 `max`/`max.burst` **不编译**。按 `CONFIG_GROUP_SCHED_WEIGHT=y`、`CONFIG_UCLAMP_TASK_GROUP` 未启用来核对，本配置实际是：

| 文件 | 依赖 | 读/写回调 | 本配置 |
|------|------|-----------|--------|
| `cpu.weight` | `CONFIG_GROUP_SCHED_WEIGHT` | `cpu_weight_read_u64` / `cpu_weight_write_u64` | 有 |
| `cpu.weight.nice` | 同上 | `cpu_weight_nice_read_s64` / `cpu_weight_nice_write_s64` | 有 |
| `cpu.idle` | 同上 | `cpu_idle_read_s64` / `cpu_idle_write_s64` | 有 |
| `cpu.max`、`cpu.max.burst` | `CONFIG_GROUP_SCHED_BANDWIDTH`（由 `CFS_BANDWIDTH` 选中） | `cpu_max_show` / `cpu_max_write`、`cpu_burst_*` | **无** |
| `cpu.uclamp.min` / `cpu.uclamp.max` | `CONFIG_UCLAMP_TASK_GROUP` | `cpu_uclamp_*_show/write` | **无** |
| `cpu.stat` / `cpu.stat.local` | cgroup 核心 | `cpu_stat_show` / `cpu_local_stat_show` | 有（`cgroup_base_files[]`） |

`cpu_cgrp_subsys`（`kernel/sched/core.c:10556`）另有三个字段值得记：

```c
.css_extra_stat_show = cpu_extra_stat_show,   /* 控制器追加统计，进 cpu.stat */
.css_local_stat_show = cpu_local_stat_show,   /* 进 cpu.stat.local */
.early_init	= true,                  /* 启动早期就要可用（调度依赖） */
.threaded	= true,                  /* 允许线程粒度 */
```

`cpu.weight.nice` 提供 nice 语义的接口（内部换算成 `sched_weight`），比直接写 1..10000 的 `cpu.weight` 更贴近传统调优习惯。真正把任务挂到 target `task_group` 的是 `sched_move_task()`（`kernel/sched/core.c:9478`），由 `cpu_cgroup_attach()` 调用。

## 25. pids 控制器：层次计费与事件语义

`struct pids_cgroup`（`kernel/cgroup/pids.c:49`）的关键字段：

```c
struct pids_cgroup {
	struct cgroup_subsys_state css;
	atomic64_t counter;    /* 当前计数（含所有后代） */
	atomic64_t limit;      /* PIDS_MAX = PID_MAX_LIMIT + 1 表示 max */
	int64_t    watermark;  /* pids.peak */
	struct cgroup_file events_file, events_local_file;
	atomic64_t events[NR_PIDCG_EVENTS];
	atomic64_t events_local[NR_PIDCG_EVENTS];
};
```

计费规则（`pids_charge()` `pids.c:145` / `pids_uncharge()` `pids.c:128` / `pids_try_charge()` `pids.c:166`）：

- 沿 `parent_pids()` 链**逐层累加**，因此 `parent/pids.current` 是子树总和，且“最严格的层级限制生效”；
- `pids_try_charge()` 先 `atomic64_add_return()` 再加回滚（`revert:` 标签），失败返回 `-EAGAIN`——这就是 fork 失败给用户的 `-EAGAIN` 来源；
- `pids_charge()` 明确注释“不跟随限制、不可能失败”，因为它只用于迁移回滚这种“必须成功”的场合；
- 事件有两套：`PIDCG_MAX`（祖先限制被撞）与 `PIDCG_FORKFAIL`（本 cgroup 的 fork 被拒），`events` 向上传播、`events.local` 只记本层；`CGRP_ROOT_PIDS_LOCAL_EVENTS` 可把语义切成“只记本地”。

`pids_can_fork()`（`pids.c:273`）的注释点出一个隐含依赖：它用 `cset->subsys[pids_cgrp_id]` 而**不检查任务存活**，因为 `copy_process()` 全程持有 `cgroup_threadgroup_change_begin()` 的读侧，隐式保证 css 不会消失。

## 26. memcg 的 `try_charge_memcg()`：一个完整的重试状态机

`try_charge_memcg()`（`mm/memcontrol.c:2595`）比前文一句“try_charge → page_counter_try_charge”复杂得多，实际流程：

```text
retry:
  consume_stock(memcg, nr_pages)                  /* 先吃 per-cpu 缓存 */
  batch = max(MEMCG_CHARGE_BATCH, nr_pages)       /* 或 gfpflags_allow_spinning() 判定的批量 */
  if (do_memsw_account() 关) → memsw 也扣
  page_counter_try_charge(&memcg->memory, batch, &counter)
     ├─ 成功 → done_restock（回填 stock）
     └─ 失败 → mem_over_limit = mem_cgroup_from_counter(counter, memory)
  if (batch > nr_pages) { batch = nr_pages; goto retry; }   /* 缩批重试 */

  if (current->flags & PF_MEMALLOC) goto force;    /* 回收路径自身分配，放行 */
  if (task_in_memcg_oom(current))    goto nomem;
  if (!gfpflags_allow_blocking(gfp_mask)) goto nomem;

  __memcg_memory_event(mem_over_limit, MEMCG_MAX, ...);   /* memory.events: max++ */
  psi_memstall_enter(&pflags);
  try_to_free_mem_cgroup_pages(...)                /* 回收 */
  psi_memstall_leave(&pflags);
  if (margin >= nr_pages) goto retry;
  drain_all_stock(mem_over_limit); goto retry;     /* 先把别人的 stock 倒回来 */
  ...
  if (mem_cgroup_oom(...)) { passed_oom = true; nr_retries = MAX_RECLAIM_RETRIES; goto retry; }
nomem:
  if (!(gfp_mask & (__GFP_NOFAIL | __GFP_HIGH))) return -ENOMEM;
force:  /* 强制超额，仍然记 MEMCG_MAX */
```

由此可以解释几个常见现象：

- `memory.events` 的 `max` 计数会在**回收之前**就加一（`__memcg_memory_event(... MEMCG_MAX ...)`），所以“`max` 有值但没 OOM”是正常的；
- `gfpflags_allow_spinning()` 为假（原子上下文）时批量被压到 `nr_pages`，避免在不可睡眠上下文里攒 stock；
- `drain_all_stock()` 是关键一步：某个 CPU 上缓存的 charge 会让“看起来没超”的 memcg 实际已超，先 drain 再判定；
- `memory.high` 的节流在 `memory_high_write()`（`memcontrol.c:4768`）设置后由 `memory_high` 分支处理，与 `max` 的硬 OOM 路径分开。

`memory_files[]` 与 `memory_cgrp_subsys` 分别在 `memcontrol.c` 的 `memory_files` 数组与 `memory_cgrp_subsys`（`memcontrol.c:5076`），本配置 `CONFIG_SWAP=n`、`CONFIG_MEMCG_V1` 未启用，因此 `memory.swap.*` 与 v1 的 `memory.limit_in_bytes` 都不存在。

## 27. io 控制器：`blkcg` 与 `blkg` 的对象关系

`blkcg_css_alloc()`（`block/blk-cgroup.c:1365`）要点：

```c
if (!parent_css) blkcg = &blkcg_root;     /* 根 cgroup 用静态对象 */
else blkcg = kzalloc_obj(*blkcg);
if (init_blkcg_llists(blkcg)) ...
for (i = 0; i < BLKCG_MAX_POLS; i++) {     /* 每个策略一份 per-blkcg 数据 */
	struct blkcg_policy *pol = blkcg_policy[i];
	if (!pol || !pol->cpd_alloc_fn) continue;
	blkcg->cpd[i] = pol->cpd_alloc_fn(GFP_KERNEL);
	...
}
INIT_RADIX_TREE(&blkcg->blkg_tree, GFP_NOWAIT);
refcount_set(&blkcg->online_pin, 1);       /* 延迟 offline，等 cgwb 收尾 */
list_add_tail(&blkcg->all_blkcgs_node, &all_blkcgs);
```

对象关系：

| 对象 | 粒度 | 作用 |
|------|------|------|
| `struct blkcg` | 每个 cgroup | 策略私有数据 `cpd[BLKCG_MAX_POLS]`、`blkg_tree`（radix tree，按 disk 索引）、`online_pin` |
| `struct blkcg_gq`（blkg） | 每个 (blkcg, gendisk) | 实际 IO 统计/限流状态，`blkg_create()`（`blk-cgroup.c:379`）创建 |
| `struct blkcg_policy` | 全局 | 注册额外 cftype 与 `pd_alloc_fn`/`cpd_alloc_fn` |

统计路径按 per-cpu 缓存 + 按需 flush：

- `blkcg_rstat_flush()`（`blk-cgroup.c:1070`）→ `__blkcg_rstat_flush()`（`blk-cgroup.c:994`），把每个 blkg 的 per-cpu 计数收集到 `iostat` 再输出；
- `blkcg_files[]`（`blk-cgroup.c:1198`）只有 `stat`、`reset_stats`（即 `io.stat`/`io.reset_stats`）；
- 参数解析 `blkg_conf_prep()`（`blk-cgroup.c:828`）负责 `MAJ:MIN value` 形式，只有 `BLK_DEV_THROTTLING` 等策略才用到它；
- `io_cgrp_subsys`（`blk-cgroup.c:1508`）在 `CONFIG_MEMCG` 下 `.depends_on = 1 << memory_cgrp_id`（`blk-cgroup.c:1524`），保证默认层级上 memcg 与 io 同时可用——注释说明这是为了“从回写页反查 owner cgroup”。

本配置 `BLK_DEV_THROTTLING`/`BLK_CGROUP_IOCOST`/`BLK_CGROUP_IOLATENCY` 全关，所以用户态只能看到 `io.stat`/`io.reset_stats`，`io.max`/`io.cost.*`/`io.latency` 不存在；但 `blkcg`/`blkg` 对象、`online_pin`、per-cpu 统计仍然完整存在。

## 28. rstat 在 7.2 的重构（与正文第 9 节的差异）

第 9 节描述的是“每个 css 的 per-cpu `rstat_cpu` + 全局 `rstat_backlog_list`”。本树的实现已经改成 **per-subsystem × per-cpu 的 lockless list**，读源码时要注意：

```c
static spinlock_t *ss_rstat_lock(struct cgroup_subsys *ss)     /* rstat.c:41 */
{
	if (ss) return &ss->rstat_ss_lock;
	return &rstat_base_lock;
}

static inline struct llist_head *ss_lhead_cpu(struct cgroup_subsys *ss, int cpu) /* rstat.c:49 */
{
	if (ss) return per_cpu_ptr(ss->lhead, cpu);
	return per_cpu_ptr(&rstat_backlog_list, cpu);
}
```

即：**每个子系统有自己的 `llist` 数组**（`ss->lhead`）与自己的锁（`ss->rstat_ss_lock`），只有 css 自身的统计（`ss == NULL`）才用全局的 `rstat_backlog_list`/`rstat_base_lock`。

`__css_rstat_updated()`（`rstat.c:71`）的新实现要点：

1. `lockdep_assert_preemption_disabled()`——调用者必须已关抢占（在 irqsave 临界区或 per-cpu 段内）；
2. 无 NMI-safe `cmpxchg` 的架构在 NMI 中直接返回（`CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG`）；
3. 用 `try_cmpxchg(&rstatc->lnode.next, &self, NULL)` 让“第一个插入者”胜出，输家直接返回——这样同一 per-cpu 节点被 irq/NMI 重入时不会重复 `llist_add()`；
4. 只有胜者执行 `llist_add(&rstatc->lnode, lhead)`。

flush 侧：

- `css_process_update_tree()`（`rstat.c:166`）用 `llist_del_first_init()` 逐个取出，再 `__css_process_update_tree()`（`rstat.c:137`）把 css 及其祖先挂成 `updated_children` 树；
- `css_rstat_flush()`（`rstat.c:409`）是导出给 BPF 的 kfunc（`KF_SLEEPABLE`，见 `rstat.c:755` 附近的 `BTF_ID_FLAGS`），可以睡眠，因此 `cgroup_base_stat_cputime_show()`（`rstat.c:722`）里直接 `css_rstat_flush(&cgrp->self)` 后输出；
- `cgroup_base_stat_flush()`（`rstat.c:572`）负责把 `cgroup_base_stat` 的增量累加进 `bstat`，`last_bstat` 做差分。

对容器运行时的影响：这意味着 `cpu.stat`/`memory.stat` 的读取代价从“遍历整棵 backlog”降为“只处理该子系统自己的链表”，`CONFIG_CGROUP_BPF=y` 时还能在 BPF 程序里直接调 `css_rstat_updated()`/`css_rstat_flush()` 做低开销聚合。

## 29. PREEMPT_RT 下的 cgroup（本树 `CONFIG_PREEMPT_RT=y`）

本仓库 `.config` 打开了 `CONFIG_PREEMPT_RT=y`，阅读 cgroup 代码时要注意下列与实时内核相关的约束：

| 机制 | 代码位置 | RT 下的含义 |
|------|----------|-------------|
| `css_set_lock` | 全文件大量 `spin_lock_irq(&css_set_lock)` | RT 下 `spinlock_t` 由 rtmutex 实现，`spin_lock_irq()` 会关中断并可能睡眠；代码因此严格保证“锁内不做可能睡眠的事”（`can_attach`/`attach` 都在锁外调用） |
| `cgroup_threadgroup_rwsem` | `percpu_down_write()`（`cgroup.c:2542`） | percpu-rwsem 的写侧在 RT 下仍要求“不能有读者在 CPU 上”，所以先 `cpus_read_lock()` 再取，避免与 CPU 热插拔互锁 |
| 每线程组 rwsem | `down_write(&tsk->signal->cgroup_threadgroup_rwsem)`（`cgroup.c:2545`） | `favordynmods` 路径，粒度更细，RT 下优先使用它可减少全局停顿 |
| rstat | `lockdep_assert_preemption_disabled()`（`rstat.c:81`） | per-cpu 更新必须在关抢占区；RT 下“关抢占”不等于“关中断”，因此 NMI 分支另做检查 |
| freezer | `spin_lock(&child->sighand->siglock)`（`cgroup.c:7025`） | RT 下 siglock 是 raw spinlock 语义的转换点，`cgroup_post_fork()` 里尽量缩到最小 |

实践含义：实时负载上频繁写 `cgroup.procs` 会因全局 `percpu_down_write()` 造成可感知抖动；挂载时加 `favordynmods` 可显著缓解。

## 30. 调试与配置速查

| 手段 | 说明 |
|------|------|
| `/proc/cgroups` | 每个控制器的 `hierarchy`/`num_cgroups`/`enabled`，由 `cgroup_init()` 注册 |
| `/proc/<pid>/cgroup` | 进程在默认层级与各 v1 层级中的路径（受 cgroup namespace 影响） |
| `cgroup.stat` | `nr_descendants`、`nr_dying_descendants`，判断“删不掉/删了没释放” |
| `cgroup.events` | `populated`、`frozen`，可 `poll()` |
| `cgroup.max.depth`/`cgroup.max.descendants` | 限制子树深度/节点数，防御性配置 |
| 启动参数 `cgroup_disable=<ss>` | `cgroup.c:7192` 解析、`cgroup.c:7223` 注册，禁单个控制器 |
| 启动参数 `cgroup_no_v1=all\|named\|<ss>` | `cgroup-v1.c`，`cgroup_no_v1_mask`（`cgroup-v1.c:31`）与 `cgroup_no_v1_named`（`cgroup-v1.c:34`） |
| 启动参数 `cgroup_debug` | `cgroup.c:7233`，打开 `CFTYPE_DEBUG` 文件 |
| 启动参数 `cgroup_favordynmods=` | `cgroup.c:7239`，全局默认开启 per-threadgroup rwsem |
| tracepoint | `TRACE_CGROUP_PATH(attach_task, ...)`、`rmdir`、`mkdir` 等，`/sys/kernel/tracing/events/cgroup/` |
| `CONFIG_DEBUG_LOCK_ALLOC` | `cgroup_add_file()` 里把 `cft->lockdep_key` 传给 kernfs，可按文件区分锁类 |

注意 `CONFIG_CGROUP_DEBUG` 未启用，`debug.c` 不编译；`cgroup_debug` 启动参数只影响带 `CFTYPE_DEBUG` 的核心文件。

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
