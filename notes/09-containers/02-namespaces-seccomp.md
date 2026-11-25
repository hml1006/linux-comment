# Namespace 与 seccomp

> 源码基线 Linux 7.2。namespace 核心在 `kernel/nsproxy.c`、`include/linux/nsproxy.h`、`include/linux/ns_common.h` 及各自的 `*_namespace.c`/`net_namespace.c`；nsfs 在 `fs/nsfs.c`；seccomp 在 `kernel/seccomp.c`、`include/linux/seccomp*.h`、`include/uapi/linux/seccomp.h`。
>
> **配置前提**：本仓库 `.config` 已启用 `CONFIG_NAMESPACES=y`、`CONFIG_UTS_NS=y`、`CONFIG_IPC_NS=y`、`CONFIG_USER_NS=y`、`CONFIG_PID_NS=y`、`CONFIG_NET_NS=y`、`CONFIG_TIME_NS=y`、`CONFIG_CGROUPS=y`，以及 `CONFIG_SECCOMP=y`、`CONFIG_SECCOMP_FILTER=y`。`CONFIG_SECCOMP_CACHE_DEBUG` 未启用（无 `seccomp_cache` debugfs/proc 接口）；`CONFIG_CHECKPOINT_RESTORE` 未启用（`seccomp_get_filter`/`seccomp_get_metadata` 不可用）。这些都是配置裁剪，不是内核缺少对应代码。

## 1. Namespace 类型总览

| 常量 | 类型 | nsproxy 字段 | 主要源文件 | 隔离内容 |
|------|------|--------------|------------|----------|
| `CLONE_NEWNS` | mount | `mnt_ns` | `fs/namespace.c`、`fs/pnode.c` | 挂载树、root/pwd |
| `CLONE_NEWUTS` | UTS | `uts_ns` | `kernel/utsname.c`、`include/linux/uts_namespace.h` | hostname/domainname/nodename |
| `CLONE_NEWIPC` | IPC | `ipc_ns` | `ipc/namespace.c` | SysV sem/msg/shm、POSIX mqueue |
| `CLONE_NEWPID` | PID | `pid_ns_for_children` | `kernel/pid_namespace.c`、`kernel/pid.c` | PID 编号空间、`init` 进程 |
| `CLONE_NEWNET` | network | `net_ns` | `net/core/net_namespace.c` | 网络设备、协议栈、路由、netfilter |
| `CLONE_NEWUSER` | user | （在 `cred->user_ns`） | `kernel/user_namespace.c` | UID/GID 映射、capability 域 |
| `CLONE_NEWCGROUP` | cgroup | `cgroup_ns` | `kernel/cgroup/namespace.c` | cgroup 根视图 |
| `CLONE_NEWTIME` | time | `time_ns_for_children` | `kernel/time/namespace.c` | `CLOCK_MONOTONIC`/`BOOTTIME` 偏移 |

PID namespace 是特殊情况：`nsproxy` 中保存的是 **`pid_ns_for_children`**（子进程将使用的 pid ns），当前任务实际所属的 pid ns 由 `task_active_pid_ns(task)` 从 `task->thread_pid` 推导。这样 `setns(CLONE_NEWPID)` 才符合“只影响之后 fork 的子进程”的语义。

## 2. `nsproxy` 与 `ns_common`

### 2.1 `struct nsproxy`

```c
// include/linux/nsproxy.h
struct nsproxy {
	refcount_t count;
	struct uts_namespace *uts_ns;
	struct ipc_namespace *ipc_ns;
	struct mnt_namespace *mnt_ns;
	struct pid_namespace *pid_ns_for_children;
	struct net	     *net_ns;
	struct time_namespace *time_ns;
	struct time_namespace *time_ns_for_children;
	struct cgroup_namespace *cgroup_ns;
};
extern struct nsproxy init_nsproxy;
```

- 一个 `nsproxy` 被“共享全部 namespace 的任务”共用；一旦某个 namespace 被 clone/unshare，就复制出新的 `nsproxy`；
- `count` 是引用计数，`copy_namespaces()` 在无需新建时只 `get_nsproxy(old_ns)`；
- 访问规则：只有 `current` 可修改自己的 `nsproxy`（需持 `task_lock`）；访问他人 ns 需 `task_lock(task)` + 引用。

### 2.2 `struct ns_common`（7.x 重构）

7.2 把 namespace 生命周期重构为**两级引用 + 命名空间树**（`include/linux/ns/ns_common_types.h`，字段节选）：

```c
struct ns_common {
	u32 ns_type;                  /* 64 位 CLONE_NEW* 类型编码 */
	u32 ns_id;                    /* 全局唯一 ID */
	unsigned int inum;            /* nsfs inode number */
	const struct proc_ns_operations *ops;
	struct dentry *stashed;
	refcount_t __ns_ref;          /* 内存生命周期 */
	atomic_t __ns_ref_active;     /* 活跃用户（任务/fd/bind mount） */
	struct ns_node ns_unified_node;
	struct ns_node ns_tree_node;
	struct ns_node ns_owner_node;
	struct list_head ns_owner_root;
	...
};
```

- `__ns_ref`：普通 refcount，决定结构体何时释放，同时把 ns 钉在命名空间树上；
- `__ns_ref_active`：活跃引用。任何任务（经 nsproxy/cred）、打开的 nsfs fd、bind mount 都持有一个 active 引用；归零后 ns 变为 **inactive**，从树中对用户态不可见，但仍可能被内部引用保活（例如 `file->f_cred`）。`ns_ref_get()`/`ns_ref_put()`、`ns_ref_active_get()`/`ns_ref_active_put()` 成对使用；
- 初始化 `ns_common_init()` 会给非 init ns 填 `ns_id`/`inum`；init ns 用 `is_ns_init_id()`/`is_ns_init_inum()` 特判（`MNT_NS_INIT_INO`...`IPC_NS_INIT_INO` 范围）；
- 命名空间树由 `kernel/nstree.c` 维护，并新增 `listns(2)`（`kernel/nstree.c:763` `SYSCALL_DEFINE4(listns, ...)`）与 `statns` 语义（见 `include/uapi/linux/nsfs.h`）。

`struct proc_ns_operations`（`include/linux/proc_ns.h:17`）是每种 ns 的操作表：

```c
struct proc_ns_operations {
	const char *name;
	const char *real_ns_name;
	struct ns_common *(*get)(struct task_struct *task);
	void (*put)(struct ns_common *ns);
	int (*install)(struct nsset *nsset, struct ns_common *ns);
	struct user_namespace *(*owner)(struct ns_common *ns);
	struct ns_common *(*get_parent)(struct ns_common *ns);
};
```

`netns_operations`、`utsns_operations`、`ipcns_operations`、`pidns_operations`、`pidns_for_children_operations`、`userns_operations`、`mntns_operations`、`cgroupns_operations`、`timens_operations`、`timens_for_children_operations` 各自实现，被 `/proc/<pid>/ns/*` 与 `setns(2)` 共用。

## 3. clone / unshare / setns

### 3.1 `clone(CLONE_NEW*)`

`copy_process()` 中调用 `copy_namespaces(flags, tsk)`（`kernel/nsproxy.c:169`）：

1. 若 `flags` 不含任何 namespace 位（`CLONE_NS_ALL & ~CLONE_NEWUSER`），且要么 `CLONE_VM`（线程），要么 `time_ns_for_children == time_ns`，则直接 `get_nsproxy(old_ns)` 返回；
2. 否则要求 `ns_capable(user_ns, CAP_SYS_ADMIN)`（注意：`CLONE_NEWUSER` 本身不需要，其他 ns 在**新** userns 中需要 CAP_SYS_ADMIN）；
3. 禁止 `CLONE_NEWIPC | CLONE_SYSVSEM` 同时使用（否则旧 ipc ns 的 undolist 不可达）；
4. `create_new_namespaces(flags, tsk, user_ns, tsk->fs)` 依次调用 `copy_mnt_ns`、`copy_utsname`、`copy_ipcs`、`copy_pid_ns`、`copy_cgroup_ns`、`copy_net_ns`、`copy_time_ns`，未请求的 ns 只增加引用；
5. 非 `CLONE_VM` 时 `timens_on_fork()`；
6. `nsproxy_ns_active_get(new_ns)` 后把 `tsk->nsproxy` 换成新 proxy。

### 3.2 `unshare(2)`

`SYSCALL_DEFINE1(unshare)` → `ksys_unshare()`（`kernel/fork.c:3217`）：

- 依赖修正：`CLONE_NEWUSER` 隐含 `CLONE_THREAD | CLONE_FS`；`CLONE_NEWNS` 隐含 `CLONE_FS`；`CLONE_VM` 隐含 `CLONE_SIGHAND`，`CLONE_SIGHAND` 隐含 `CLONE_THREAD`；
- `UNSHARE_EMPTY_MNTNS`（32 位别名 `CLONE_PARENT_SETTID`）会被转换成 64 位 `CLONE_EMPTY_MNTNS`，用于创建“只有根挂载”的空 mount namespace；
- 依次 `unshare_fs()`、`unshare_fd()`、`unshare_userns()`、`unshare_nsproxy_namespaces()`；
- `unshare_nsproxy_namespaces()`（`nsproxy.c:211`）同样检查 CAP_SYS_ADMIN 并调用 `create_new_namespaces()`；
- 最后 `switch_task_namespaces(current, new_nsproxy)`、`commit_creds(new_cred)`、`set_fs_root/pwd` 等提交。

### 3.3 `setns(2)`

`SYSCALL_DEFINE2(setns, int fd, int flags)`（`nsproxy.c:569`）：

1. fd 可以是 `/proc/<pid>/ns/<type>` 的 nsfs 文件，也可以是 pidfd；
2. nsfs 文件：`get_proc_ns(inode)` 取 `ns_common`，若 `flags` 非零必须等于 `ns->ns_type`；
3. `prepare_nsset(flags, &nsset)` 先 `create_new_namespaces(0, ...)` 复制一份 nsproxy，必要时 `prepare_creds()`/`copy_fs_struct()`；
4. `validate_ns(&nsset, ns)` 即 `ns->ops->install(&nsset, ns)`，各 namespace 在这里做权限与合法性检查；
5. pidfd 路径走 `validate_nsset()`：RCU 下取目标任务 `nsproxy`，要求 `ptrace_may_access(PTRACE_MODE_READ_REALCREDS)`，再对每个请求的 ns 调 `install`；
6. `commit_nsset()`：提交 creds、`set_fs_root/pwd`、`exit_sem()`（NEWIPC）、`timens_commit()`（NEWTIME）、`switch_task_namespaces()`。

`install` 的典型权限要求：`mntns_install()`（`fs/namespace.c:6487`）要求目标 mnt ns 的 userns 与当前 cred userns 中都有 `CAP_SYS_ADMIN`，且 `CAP_SYS_CHROOT`，还要求 `fs->users == 1`（无共享 fs_struct），匿名 ns 拒绝；`pidns_install()`（`pid_namespace.c:392`）要求两个 userns 的 CAP_SYS_ADMIN，且新 ns 必须是当前 active pid ns 的祖先或自身（只能“进入更深”，不能横向跳转）。

## 4. PID namespace

`struct pid_namespace`（`include/linux/pid_namespace.h:26`）：

```c
struct pid_namespace {
	struct idr idr;                /* pid 分配 */
	unsigned int pid_allocated;
	struct task_struct *child_reaper;  /* 该 ns 的 init */
	struct kmem_cache *pid_cachep;
	unsigned int level;            /* init_pid_ns = 0 */
	int pid_max;
	struct pid_namespace *parent;
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	int reboot;
	struct ns_common ns;
	struct work_struct work;
};
```

创建 `create_pid_namespace()`（`kernel/pid_namespace.c:76`）：

- `level = parent->level + 1`，超过 `MAX_PID_NS_LEVEL` 返回 `-ENOSPC`；
- `inc_pid_namespaces(user_ns)` 占 ucount；
- `create_pid_cachep(level)`：内核为每个层级预建 `struct pid` 的 slab cache（`struct pid` 内嵌 `numbers[level]` 柔性数组），因此 `struct pid` 大小随层级固定，不能在 ns 创建后改变；
- `pid_max = PID_MAX_LIMIT`，注册 per-ns sysctl（`kernel.pid_max` 等）；
- `ns_tree_add(ns)` 加入命名空间树。

PID 编号解析：

- `alloc_pid(ns, set_tid, ...)`（`kernel/pid.c:159`）在**从目标 ns 到 root 的整条链**上为每一层分配一个 `struct upid`，所以同一进程在不同 pid ns 里有不同 PID；
- `pid_nr_ns(pid, ns)`（`kernel/pid.c:542`）把 `struct pid` 转换为指定 ns 中的数字，若 pid 不属于该 ns 的祖先链则返回 0；
- `__task_pid_nr_ns()` 用于 `/proc`、`getppid`、`kill` 等按调用者 ns 视角输出；
- `zap_pid_ns_processes()`（`pid_namespace.c:192`）在 ns 的 init 退出时杀死其余进程，`reboot` 字段保存退出码；
- `MAX_PID_NS_LEVEL` 限制嵌套深度（通常 32）。

## 5. Mount namespace

`struct mnt_namespace`（`fs/mount.h:11`）：

```c
struct mnt_namespace {
	struct ns_common ns;
	struct mount *root;
	struct { struct rb_root mounts; struct rb_node *mnt_last_node, *mnt_first_node; };
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	wait_queue_head_t poll;
	u64 seq_origin;      /* 源 ns 的序号 */
	u64 event;           /* mount/umount 事件序号 */
	struct hlist_head mnt_visible_mounts;
	unsigned int nr_mounts, pending_mounts;
	refcount_t passive;
	bool is_anon;
};
```

- 每个 mount 挂在 `ns->mounts` 红黑树里（按 `mnt_id` 排序），`mnt_add_to_ns()` 维护；
- `copy_mnt_ns(flags, ns, user_ns, new_fs)`（`fs/namespace.c:4241`）：无 `CLONE_NEWNS` 时 `get_mnt_ns()` 复用；否则 `alloc_mnt_ns()` + `copy_tree(old, old->mnt.mnt_root, copy_flags)`，`copy_flags` 含 `CL_COPY_UNBINDABLE | CL_EXPIRE`，跨 userns 时加 `CL_SLAVE`；`CLONE_EMPTY_MNTNS` 时只 `clone_mnt()` 根挂载；
- 复制时并行遍历新旧树，把新树的 mount 标记为属于新 ns，并替换 `new_fs->root`/`pwd` 指向的 mount；
- `namespace_sem`（7.2 中通过 `guard(namespace_excl)()` 获取的独占锁）保护挂载树修改；
- shared subtree（peer group/slave）语义在 `fs/pnode.c`，跨 ns 传播 mount/umount 事件；
- `pivot_root(2)`（`fs/namespace.c:4775`）改变 ns 的根；`mount_setattr(2)` 支持递归属性修改（`IDMAP`、`NOEXEC` 等）；
- 序列号 `seq_origin`/`event` 用于 `statmount(2)`/`listmount(2)` 的增量枚举与 `NS_GET_MNTNS_ID`。

## 6. Network namespace

`struct net`（`include/net/net_namespace.h:62`）是协议栈的根：

```c
struct net {
	refcount_t passive;
	spinlock_t rules_mod_lock;
	unsigned int dev_base_seq;
	u32 ifindex;
	struct list_head list, exit_list;
	struct llist_node defer_free_list, cleanup_list;
	struct list_head ptype_all, ptype_specific;
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	struct idr netns_ids;
	struct ns_common ns;
	struct ref_tracker_dir refcnt_tracker, notrefcnt_tracker;
	struct list_head dev_base_head;
	struct proc_dir_entry *proc_net, *proc_net_stat;
	struct ctl_table_set sysctls;
	...
	struct net_generic __rcu *gen;    /* 各子系统的私有数据 */
};
```

- `net_generic(net, id)` 是“按子系统 ID 存取私有结构”的机制，ID 由 `register_pernet_subsys()`/`register_pernet_device()` 分配；
- `copy_net_ns(flags, user_ns, old_net)`（`net/core/net_namespace.c:549`）：无 `CLONE_NEWNET` 时 `get_net(old)`；否则 `net_alloc()` + `setup_net(net, user_ns)`；
- `setup_net()`（`net_namespace.c:436`）按注册顺序执行所有 `pernet_operations->init`（设备类 `init` 还可能延迟到 `register_pernet_device` 完成）；失败则逆序 `exit`；
- 每个 socket 通过 `sock_net(sk)` 关联 netns，`sk->sk_net_refcnt` 决定是否持 active 引用；
- 销毁由 `cleanup_net()` workqueue 异步完成：先 `ops_pre_exit`/`exit`，再把 net 放入 `cleanup_list`，最后 `net_free()`；
- `/proc/sys/net` 是 per-netns 的（`net->sysctls`），`/proc/net` 也是（`proc_net`）；
- `netns_ids` 支持 `NETNSA_NSID`，配合 `RTM_GETNSID`。

## 7. User namespace

`struct user_namespace`（`include/linux/user_namespace.h:76`）：

```c
struct user_namespace {
	struct uid_gid_map uid_map, gid_map, projid_map;
	struct user_namespace *parent;
	int level;
	kuid_t owner;
	kgid_t group;
	struct ns_common ns;
	unsigned long flags;
	bool parent_could_setfcap;
	struct list_head keyring_name_list;   /* CONFIG_KEYS */
	struct key *user_keyring_register;
	struct rw_semaphore keyring_sem;
	struct ucounts *ucounts;
	long ucount_max[UCOUNT_COUNTS];
	long rlimit_max[UCOUNT_RLIMIT_COUNTS];
};
```

要点：

- `create_user_ns(new_cred)`（`kernel/user_namespace.c:83`）在 `CLONE_NEWUSER` 时调用：新 ns 的 `parent` 是当前 cred 的 userns，`level = parent->level + 1`，新 ns 内创建者获得全部 capability（但只在**该 ns 及其子 ns** 有效）；
- **ID 映射**：`uid_map`/`gid_map` 由写 `/proc/<pid>/uid_map` 建立，`map_write()` 解析 `inside outside count` 行；`make_kuid(ns, uid)`/`from_kuid(ns, kuid)` 通过 `map_id_range_down()`/`map_id_up()` 做二分查找；未映射的 ID 用 `overflowuid`/`overflowgid`（65534）；
- 映射限制：非特权用户只能映射自己的 uid/gid，且每个映射区间至少一行；写入一次后不可再写（除非 `CAP_SETUID` 在父 ns）；`/proc/sys/user/max_user_namespaces` 限制嵌套数量；
- `ns_capable(ns, cap)` 沿 `ns->parent` 链查找：只有目标 ns 及其祖先中的 capability 才有效，这就是“容器 root 不能影响宿主”的基础；
- `ucounts` 统计该 ns 及其子 ns 的进程数、inotify、key 等资源，对应 `RLIMIT_*` 的 per-userns 上限；
- 7.2 在 `create_user_ns()` 中调用 `security_create_user_ns(new)`，允许 LSM 拒绝创建（例如安全策略禁止非特权 userns）。

## 8. UTS / IPC / time / cgroup namespace

### 8.1 UTS

```c
struct uts_namespace {
	struct new_utsname name;       /* sysname/nodename/release/version/machine/domainname */
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	struct ns_common ns;
};
```

`sethostname(2)`/`setdomainname(2)` 修改 `utsname()->nodename`/`domainname`，受 `CAP_SYS_ADMIN`（在 `uts_ns->user_ns` 中）与 `uts_sem` 保护；`uname(2)` 读 `current->nsproxy->uts_ns->name`。`copy_utsname()` 在 `CLONE_NEWUTS` 时复制。

### 8.2 IPC

```c
struct ipc_namespace {
	struct ipc_ids ids[3];      /* SEM/MSG/SHM */
	int sem_ctls[4];
	unsigned int msg_ctlmax, msg_ctlmnb, msg_ctlmni;
	size_t shm_ctlmax, shm_ctlall;
	unsigned long shm_tot;
	int shm_ctlmni, shm_rmid_forced;
	struct vfsmount *mq_mnt;    /* POSIX mqueue 的私有 mqueuefs */
	unsigned int mq_queues_count;
	struct ns_common ns;
	...
};
```

`copy_ipcs()`（`ipc/namespace.c:111`）→ `create_ipc_ns()`：初始化 3 组 `ipc_ids`、每个 ns 独立的 `mq_mnt`（`mqueuefs` 实例），因此消息队列/信号量/共享内存完全隔离；`shm_tot` 是该 ns 的共享内存总量，受 per-ns sysctl 限制。`unshare(CLONE_NEWIPC)` 时会 `exit_sem()`/`exit_shm()` 丢弃旧 ns 的 undolist。

### 8.3 time

```c
struct timens_offsets { struct timespec64 monotonic, boottime; };

struct time_namespace {
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	struct ns_common ns;
	struct timens_offsets offsets;
	struct page *vvar_page;     /* CONFIG_TIME_NS_VDSO */
	bool frozen_offsets;
};
```

与其它 ns 不同，`CLONE_NEWTIME` 只设置 **`time_ns_for_children`**，当前进程仍在旧 ns；`exec_task_namespaces()`（`nsproxy.c`）与 `timens_on_fork()` 在 exec/fork 时把子任务切入新 ns（`kernel/time/namespace.c:213`）。偏移通过 `/proc/<pid>/timens_offsets` 设置（`timens_install()` 校验 `CAP_SYS_TIME` 与新 ns 的 userns），首次有任务加入后 `frozen_offsets` 置位、不可再改。`copy_time_ns()` 在克隆时复制 offsets。

### 8.4 cgroup

`struct cgroup_namespace`（`kernel/cgroup/namespace.c`）只保存 `root_cgrp` 与 `ns`，让 `/proc/self/cgroup` 与 cgroupfs 的视图以 ns 根为起点。`CLONE_NEWCGROUP` 由 `copy_cgroup_ns()` 创建；在 `cgroup2` 上配合 `nsdelegate` 挂载选项实现容器内安全委托（详见 `01-cgroup-v2.md`）。

## 9. nsfs 与 `/proc/<pid>/ns`

- `/proc/<pid>/ns/{mnt,uts,ipc,pid,net,user,cgroup,time,pid_for_children,time_for_children}` 是符号链接，指向 nsfs 中的 inode；`ns_entries[]`（`fs/proc/namespaces.c:15`）列出所有类型，`proc_ns_get_link()` 调 `ops->get(task)` 取 `ns_common`；
- `fs/nsfs.c` 提供 nsfs 文件系统与 inode 操作：`ns_file_operations.unlocked_ioctl = ns_ioctl`；
- ioctl（`include/uapi/linux/nsfs.h`）：`NS_GET_USERNS`、`NS_GET_PARENT`、`NS_GET_NSTYPE`、`NS_GET_OWNER_UID`、`NS_GET_MNTNS_ID`、`NS_GET_ID`、`NS_GET_PID_FROM_PIDNS`、`NS_GET_TGID_FROM_PIDNS`、`NS_GET_PID_IN_PIDNS`、`NS_GET_TGID_IN_PIDNS`；
- 把 nsfs fd 传给 `setns(2)` 即可加入；fd 本身持 active 引用，因此即使原进程退出，ns 仍可通过 fd 存活（但可能转为 inactive，不能再被 `/proc` 打开）；
- 7.2 新增 `listns(2)`（`kernel/nstree.c:763`）用于枚举命名空间树，配合 `nsfs.h` 中的 `struct ns_id_req` 与 `struct ns_id_info`。

## 10. seccomp 模式与安装

### 10.1 模式

```c
#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT   1
#define SECCOMP_MODE_FILTER   2
```

- **strict**：`__secure_computing_strict()` 只允许 `read`/`write`/`_exit`/`sigreturn`（架构相关），其余 `SIGKILL`；由 `prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT)` 启用；
- **filter**：`prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)` 或 `seccomp(2)` 安装 BPF 程序；
- `struct seccomp`（`include/linux/seccomp_types.h`）：`mode`、`filter_count`、`filter`；filter 是链表头，入口在 `task_struct` 里。

### 10.2 安装路径

`SYSCALL_DEFINE3(seccomp, op, flags, args)`（`kernel/seccomp.c:2126`）：

- `SECCOMP_SET_MODE_STRICT` → `seccomp_set_mode_strict()`
- `SECCOMP_SET_MODE_FILTER` → `seccomp_set_mode_filter(flags, args)`：
  1. `seccomp_prepare_filter()` 把用户态 `sock_fprog` 用 `bpf_prog_create_from_user()` 编译成 `struct bpf_prog`，并校验指令（禁止越界跳转、限制长度）；
  2. `seccomp_attach_filter(flags, filter)`（`seccomp.c`）：
     - 累计 `total_insns`，超过 `MAX_INSNS_PER_PATH`（`(1<<18)/sizeof(struct sock_filter)`，约 32K 条）返回 `-ENOMEM`；
     - `SECCOMP_FILTER_FLAG_TSYNC`：先 `seccomp_can_sync_threads()`，成功后 `seccomp_sync_threads()` 把过滤器同步到同线程组所有线程；
     - `filter->prev = current->seccomp.filter`，形成链表；`seccomp_cache_prepare(filter)` 建立快速放行位图；`atomic_inc(&filter_count)`；
  3. `SECCOMP_FILTER_FLAG_NEW_LISTENER`：创建匿名 inode fd 供 `SECCOMP_RET_USER_NOTIF` 使用（`anon_inode_getfd`），一个 filter 只能有一个 listener；
  4. `SECCOMP_FILTER_FLAG_LOG` 把 `filter->log = true`，使非 ALLOW 动作都记审计；`WAIT_KILLABLE_RECV` 让等待通知的进程可被信号打断。

`prctl_set_seccomp()` 是 prctl 路径的等价入口；`prctl_get_seccomp()` 返回当前 mode。`seccomp_assign_mode()` 设置 `task->seccomp.mode` 并置 `SYSCALL_WORK_SECCOMP`，同时（除非 `SPEC_ALLOW`）调用 `arch_seccomp_spec_mitigate()` 打开 Spectre 缓解。

## 11. 过滤执行与动作

系统调用入口 `secure_computing()` → `__secure_computing()`（`seccomp.c`）→ `__seccomp_filter()`：

```c
filter_ret = seccomp_run_filters(&sd, &match);
data = filter_ret & SECCOMP_RET_DATA;
action = filter_ret & SECCOMP_RET_ACTION_FULL;
```

`seccomp_run_filters()`（`seccomp.c:404`）：

- 取 `current->seccomp.filter`；`seccomp_cache_check_allow(f, sd)` 命中则直接返回 `SECCOMP_RET_ALLOW`；
- 否则从新到旧遍历 `f->prev`，对每个 `f->prog` 执行 `bpf_prog_run_pin_on_cpu(f->prog, sd)`；
- **返回值小的优先级高**，所以 `KILL_PROCESS` 最优先、`ALLOW` 最低：

| 动作 | 值 | 行为 |
|------|-----|------|
| `SECCOMP_RET_KILL_PROCESS` | 0x80000000 | 杀整个进程（SIGSYS + core，`SECCOMP_MODE_DEAD`） |
| `SECCOMP_RET_KILL_THREAD` | 0x00000000 | 杀当前线程 |
| `SECCOMP_RET_TRAP` | 0x00030000 | 回滚 syscall，`force_sig_seccomp()` 投递 SIGSYS |
| `SECCOMP_RET_ERRNO` | 0x00050000 | 返回 `-data`（上限 `MAX_ERRNO`） |
| `SECCOMP_RET_USER_NOTIF` | 0x7fc00000 | 交给 listener，`seccomp_do_user_notification()` 阻塞等待 |
| `SECCOMP_RET_TRACE` | 0x7ff00000 | 通知 ptracer（`PTRACE_EVENT_SECCOMP`）；无 tracer 则 `-ENOSYS` |
| `SECCOMP_RET_LOG` | 0x7ffc0000 | 记录审计后放行 |
| `SECCOMP_RET_ALLOW` | 0x7fff0000 | 放行 |

`populate_seccomp_data()` 构造 `struct seccomp_data { int nr; __u32 arch; __u64 instruction_pointer; __u64 args[6]; }`，BPF 程序据此判定。`seccomp_log()` 负责审计（`audit_seccomp()`），记录 syscall 号、action、filter 的 `log` 标志。

`SECCOMP_RET_TRACE` 会重新执行一次过滤（`recheck_after_trace = true`），因为 tracer 可能改写寄存器；`SECCOMP_RET_USER_NOTIF` 的 `SECCOMP_USER_NOTIF_FLAG_CONTINUE` 会让 syscall 继续，但存在 TOCTOU，头文件注释明确警告它不能用于实现安全策略。

### 11.1 action cache

开启 `SECCOMP_ARCH_NATIVE` 时，`struct action_cache`（`seccomp.c` 约 160 行）为每个 filter 保存位图：

```c
struct action_cache {
	DECLARE_BITMAP(allow_native, SECCOMP_ARCH_NATIVE_NR);
#ifdef SECCOMP_ARCH_COMPAT
	DECLARE_BITMAP(allow_compat, SECCOMP_ARCH_COMPAT_NR);
#endif
};
```

`seccomp_cache_prepare()`/`seccomp_cache_prepare_bitmap()` 静态分析 BPF 指令：若某 syscall 的所有路径都返回 ALLOW，则置位；运行时 `seccomp_cache_check_allow()` 直接放行，避免执行 BPF。`CONFIG_SECCOMP_CACHE_DEBUG` 打开时可通过 `/proc/<pid>/seccomp_cache` 查看（本配置未启用）。

## 12. User notification 与 fd 注入

`SECCOMP_RET_USER_NOTIF` 依赖 `SECCOMP_FILTER_FLAG_NEW_LISTENER` 创建的 fd：

- 内核侧 `seccomp_do_user_notification()`（`seccomp.c:1163`）分配 `struct seccomp_knotif`，加入 `filter->notif->notifications`，在 `wait_event_interruptible` 上等待；
- 用户态 `ioctl(SECCOMP_IOCTL_NOTIF_RECV)` 取 `struct seccomp_notif`（含 `id`、`pid`、`data`），`SECCOMP_IOCTL_NOTIF_SEND` 回 `struct seccomp_notif_resp { id, val, error, flags }`；
- `SECCOMP_IOCTL_NOTIF_ADDFD` 注入 fd（`struct seccomp_notif_addfd`，支持 `ADDFD_FLAG_SETFD`/`SEND`），`SECCOMP_IOCTL_NOTIF_ID_VALID` 校验 id 仍有效，防止对已退出进程的 syscall 做出响应；
- `struct notification`（`seccomp.c` 约 150 行）保存 `requests` 原子计数、`next_id`、`notifications` 链表；`filter->notify_lock` 保护并发访问；
- `SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP` 改变唤醒语义（同步唤醒监听者）。

## 13. 当前配置下的能力对照

| 能力 | 状态 | 依据 |
|------|------|------|
| 8 类 namespace（mnt/uts/ipc/pid/net/user/cgroup/time） | 全部可用 | `CONFIG_NAMESPACES` 及 `*_NS=y`、`CGROUPS=y` |
| `unshare`/`setns`/`clone(CLONE_NEW*)` | 可用 | 核心代码无条件编译 |
| nsfs ioctl、`/proc/pid/ns` | 可用 | `fs/nsfs.c`、`fs/proc/namespaces.c` |
| `listns(2)`（7.2 新增） | 可用 | `kernel/nstree.c`，依赖 `CONFIG_NAMESPACES` |
| seccomp strict/filter | 可用 | `CONFIG_SECCOMP=y`、`CONFIG_SECCOMP_FILTER=y` |
| `SECCOMP_RET_USER_NOTIF` | 可用 | filter 路径编译 |
| `seccomp_cache` 调试接口 | **不可用** | `CONFIG_SECCOMP_CACHE_DEBUG` 未启用 |
| `SECCOMP_GET_FILTER`/`GET_METADATA`（ptrace 取 filter） | **不可用** | `CONFIG_CHECKPOINT_RESTORE` 未启用 |
| `PT_SUSPEND_SECCOMP`（ptrace 暂停 seccomp） | **不可用** | 依赖 `CONFIG_CHECKPOINT_RESTORE` |
| PID ns 嵌套上限 | `MAX_PID_NS_LEVEL` | 每个 pid 需在每层分配编号，`struct pid` 大小按层预分配 |
| 非特权 userns | 由 `security_create_user_ns` + `max_user_namespaces` 控制 | 本配置 `USER_NS=y`，未启用 LSM 时无额外限制 |

## 14. 源码实况订正：`ns_common`/`ns_tree` 的真实布局

前文 2.2 节按“设计意图”列出 `ns_id`、`ns_unified_node`、`ns_tree_node`、`ns_owner_node`、`ns_owner_root` 都在 `struct ns_common` 里。本树的实际代码把这些字段**下沉到了 `struct ns_tree`**，`ns_common` 只保留引用计数与 inode 信息（`include/linux/ns/ns_common_types.h`）：

```c
struct ns_common {
	struct {
		refcount_t __ns_ref;      /* 主引用，do not use directly */
	} ____cacheline_aligned_in_smp;
	u32 ns_type;                      /* CLONE_NEW* 类型 */
	struct dentry *stashed;           /* VFS 缓存的 dentry */
	const struct proc_ns_operations *ops;
	unsigned int inum;                /* nsfs inode 号 */
	union {
		struct ns_tree;           /* 匿名结构，活动 ns 的树节点 */
		struct rcu_head ns_rcu;   /* 释放期使用的 RCU 头 */
	};
};
```

而 `include/linux/ns/nstree_types.h` 中：

```c
struct ns_tree {
	u64 ns_id;
	atomic_t __ns_ref_active;
	struct ns_tree_node ns_unified_node;
	struct ns_tree_node ns_tree_node;
	struct ns_tree_node ns_owner_node;
	struct ns_tree_root ns_owner_root;
};
```

由此得到三点：

1. `ns_common` 与 `ns_tree` **共用一个 union 存储**：活跃时是树节点（含 `ns_id` 与活跃引用计数），释放时复用为 `rcu_head`。所以“两级引用”里 `__ns_ref` 在 `ns_common`，`__ns_ref_active` 在 `ns_tree`；
2. `ns_id` 是 64 位（`u64`），与 `listns(2)` 返回的 id 类型一致；`inum`（nsfs inode 号）是另一套编号，两者不可混用；
3. 初始化命名空间（`init_net`/`init_pid_ns`…）被 `__ns_ref_dec_and_lock()` 特判（`include/linux/ns_common.h:110` 的 `is_ns_init_id(ns)`）：永远不会被释放，且断言 `__ns_ref == 1 && __ns_ref_active == 1`。

各类型的 init 值由宏按类型分派，不靠手工填：

```c
#define ns_init_inum(__ns)  _Generic((__ns), struct net *: NET_NS_INIT_INO, ...)
#define ns_init_id(__ns)    _Generic((__ns), struct net *: NET_NS_INIT_ID, ...)
#define to_ns_common(__ns)  _Generic((__ns), struct net *: &(__ns)->ns, ...)
```

`FOR_EACH_NS_TYPE` 是 7.2 引入的“单一事实源”：新增一种 namespace 只需在此加一行，`ns_common_type()`、`CLONE_NS_ALL` 等自动派生。这对读代码很有用——判断“某 flags 位属于哪种 ns”时不用再去翻 UAPI 头。

引用辅助宏（`include/linux/ns_common.h`）也值得记：

| 宏 | 语义 |
|----|------|
| `ns_ref_inc()` / `ns_ref_put()` | 主引用加减，归零触发释放（init ns 例外） |
| `ns_ref_get()` | 唯一化（`refcount_inc_not_zero`）并返回 bool，失败表示已销毁 |
| `ns_ref_active_get()` / `ns_ref_active_put()` | 活跃引用加减，归零使 ns 从树中“对用户态不可见” |
| `ns_get_unless_inactive()` | 活跃引用检查 + 主引用唯一化，nsfs/nstree 的查表入口用它（`fs/nsfs.c:571`、`kernel/nstree.c:541`）防“复活已死 ns” |

## 15. `nsproxy` 的生命周期与活跃引用

`init_nsproxy`（`kernel/nsproxy.c:34`）是启动时的静态对象，且**`mnt_ns` 显式为 NULL**（首个 mount namespace 由 `init_mount_tree()` 之后才装进去）：

```c
struct nsproxy init_nsproxy = {
	.count = REFCOUNT_INIT(1),
	.uts_ns = &init_uts_ns,
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
	.ipc_ns = &init_ipc_ns,
#endif
	.mnt_ns = NULL,                  /* init_mount_tree() 里才赋值 &init_mnt_ns */
	.pid_ns_for_children = &init_pid_ns,
	.net_ns = &init_net,             /* CONFIG_NET */
	.cgroup_ns = &init_cgroup_ns,    /* CONFIG_CGROUPS */
	.time_ns = &init_time_ns,        /* CONFIG_TIME_NS */
	.time_ns_for_children = &init_time_ns,
};
```

创建/释放（`nsproxy.c:54`/`64`/`77`）：

```text
create_nsproxy()  → kmem_cache_alloc(nsproxy_cachep)，refcount_set(1)
nsproxy_free(ns)  → put_mnt_ns / put_uts_ns / put_ipc_ns / put_pid_ns /
                    put_time_ns(time_ns, time_ns_for_children) /
                    put_cgroup_ns / put_net，最后 kmem_cache_free
deactivate_nsproxy(ns) → nsproxy_ns_active_put(ns) + nsproxy_free(ns)
```

`nsproxy_cachep` 在 `nsproxy_cache_init()`（`nsproxy.c:609`）用 `KMEM_CACHE(nsproxy, SLAB_PANIC|SLAB_ACCOUNT)` 建，**带 `SLAB_ACCOUNT`**：nsproxy 的分配计入 memcg，避免“一个容器反复 fork 创建 nsproxy”绕过内存计费。

活跃引用有两个入口：

- `switch_task_namespaces(p, new)`（`nsproxy.c:245`）在 `task_lock` 外先 `nsproxy_ns_active_get(new)`，换指针后再 `put_nsproxy(ns)`；
- `exit_nsproxy_namespaces(p)`（`nsproxy.c:263`）等价于 `switch_task_namespaces(p, NULL)`。

user namespace 的活跃引用走 cred 而不是 nsproxy：

```c
switch_cred_namespaces(old, new)  /* ns_ref_active_get(new->user_ns); ns_ref_active_put(old->user_ns) */
get_cred_namespaces(tsk)         /* ns_ref_active_get(tsk->real_cred->user_ns) */
exit_cred_namespaces(tsk)        /* ns_ref_active_put(...) */
```

这解释了文档里的一个细节：**user ns 与 pid ns 在任务退出后仍可访问，直到任务被 reap**——因为活跃引用挂在 cred 上，而 cred 在僵尸阶段还没释放。

## 16. `create_new_namespaces()` 的创建顺序与回滚链

`create_new_namespaces()`（`nsproxy.c:88`）按固定顺序创建，失败时用 `goto` 逐级回滚：

```text
create_nsproxy()
 ├─ copy_mnt_ns(flags, old_mnt_ns, user_ns, new_fs)        失败 → out_ns
 ├─ copy_utsname(flags, user_ns, old_uts_ns)                失败 → out_uts（put_mnt_ns）
 ├─ copy_ipcs(flags, user_ns, old_ipc_ns)                   失败 → out_ipc（put_uts_ns）
 ├─ copy_pid_ns(flags, user_ns, old_pid_ns_for_children)    失败 → out_pid（put_ipc_ns）
 ├─ copy_cgroup_ns(flags, user_ns, old_cgroup_ns)           失败 → out_cgroup（put_pid_ns）
 ├─ copy_net_ns(flags, user_ns, old_net)                    失败 → out_net（put_cgroup_ns）
 └─ copy_time_ns(flags, user_ns, old_time_ns_for_children)   失败 → out_time（put_net）
     └─ new_nsp->time_ns = get_time_ns(tsk->nsproxy->time_ns)
```

顺序本身有意义：

- **mnt 最先**：`copy_mnt_ns()` 需要 `new_fs`（unshare 时可能已 `copy_fs_struct()` 出的新 fs），且其内部要取 `namespace_excl`；
- **pid 在 net/cgroup 之前**：`copy_pid_ns()` 要校验 `task_active_pid_ns(current) == old_ns`（`pid_namespace.c:180`），越早做越不容易被后续失败路径干扰；
- **time 最后且分两段**：`time_ns_for_children` 按 flags 复制，而 `time_ns`（当前任务的）永远只 `get_time_ns()`，不复制。这是 `CLONE_NEWTIME` 语义“只影响子进程”在数据结构上的体现。

## 17. fork / unshare / setns / exec 四路对比

| 入口 | 主函数 | 锁 | nsproxy 处理 | 典型用途 |
|------|--------|----|--------------|----------|
| `clone(CLONE_NEW*)` | `copy_namespaces()`（`nsproxy.c:169`） | `cgroup_threadgroup_rwsem`（由 cgroup 侧持） | 直接 `tsk->nsproxy = new_ns` | 创建新进程并同时进新 ns |
| `unshare(2)` | `unshare_nsproxy_namespaces()`（`nsproxy.c:211`） | 无全局锁，靠 `current` 本身 | 返回新 nsproxy，由 `ksys_unshare()` 最后 `switch_task_namespaces()` | 当前进程脱离共享 |
| `setns(2)`（nsfs fd） | `prepare_nsset()`+`validate_ns()`+`commit_nsset()` | `cgroup_mutex`（仅 NEWCGROUP 相关路径间接） | 在 nsset 上“预装”，`commit_nsset()` 一次性切换 | 加入别人的 ns |
| `setns(2)`（pidfd） | `validate_nsset()`（`nsproxy.c:398`） | RCU + `task_lock` 取目标 nsproxy | 同上 | 以“目标进程的全部 ns”为准批量加入 |
| `execve(2)` | `exec_task_namespaces()`（`nsproxy.c:284`） | 无 | 若 `time_ns_for_children != time_ns` 则 `create_new_namespaces(0, ...)` 后切换 | 让 `CLONE_NEWTIME` 生效 |

`exec_task_namespaces()` 只有一种情况会做事：`time_ns_for_children == time_ns` 时直接返回；否则 `create_new_namespaces(0, tsk, current_user_ns(), tsk->fs)` + `timens_on_fork()` + `switch_task_namespaces()`。所以 `unshare(CLONE_NEWTIME)` 之后必须先 `exec` 或 `fork`，否则 `CLOCK_MONOTONIC` 的偏移看不到——这正是“time ns 只能影响子进程”的实现方式。

## 18. `unshare(2)`：依赖修正与 `CLONE_EMPTY_MNTNS`

`ksys_unshare()`（`kernel/fork.c`）在调 `unshare_nsproxy_namespaces()` 之前先做 flags 修正（前文 3.2 节已列），其中 7.2 新增的一条在 `nsproxy.c:225` 的注释里写明：

```c
/*
 * Convert the 32-bit UNSHARE_EMPTY_MNTNS (which aliases
 * CLONE_PARENT_SETTID) to the unique 64-bit CLONE_EMPTY_MNTNS.
 */
if (flags & UNSHARE_EMPTY_MNTNS) {
	flags &= ~(u64)UNSHARE_EMPTY_MNTNS;
	flags |= CLONE_EMPTY_MNTNS;
}
```

原因：`CLONE_PARENT_SETTID` 是 32 位 clone flags 里的老位，`unshare(2)` 的 int flags 无法再加新位；内核借这个别名表达“空的 mount namespace”，再在 64 位内部空间转成不冲突的 `CLONE_EMPTY_MNTNS`。

`unshare_nsproxy_namespaces()` 的两个前置条件：

1. `flags & (CLONE_NS_ALL & ~CLONE_NEWUSER)` 非空，否则直接 `return 0`（没请求 ns 就不建 proxy）；
2. `ns_capable(user_ns, CAP_SYS_ADMIN)`，其中 `user_ns` 取 `new_cred ? new_cred->user_ns : current_user_ns()`——若同一调用里还 `unshare(CLONE_NEWUSER)`，用的是**新** userns 的权限，这就是“非特权用户先 unshare userns 再 unshare 其它 ns”能成立的原因。

## 19. `setns(2)`：三阶段与两个路径

`SYSCALL_DEFINE2(setns, ...)`（`nsproxy.c:569`）结构非常紧凑：

```text
if (proc_ns_file(f)):  ns = get_proc_ns(inode); flags 非零必须等于 ns->ns_type；flags = ns->ns_type
elif (pidfd):          check_setns_flags(flags)
prepare_nsset(flags, &nsset)
validate_ns(&nsset, ns) 或 validate_nsset(&nsset, pidfd_pid(f))
commit_nsset(&nsset); perf_event_namespaces(current)
put_nsset(&nsset)
```

三阶段职责：

| 阶段 | 函数 | 做的事 |
|------|------|--------|
| 预备 | `prepare_nsset()`（`nsproxy.c:354`） | `create_new_namespaces(0, me, current_user_ns(), me->fs)` 造一份“和当前一样”的 proxy；`CLONE_NEWUSER` 时 `prepare_creds()`；`fs` 只在需要时复制 |
| 校验 | `validate_ns()` / `validate_nsset()` | 调 `ns->ops->install(&nsset, ns)`，各 ns 自己检查权限 |
| 提交 | `commit_nsset()`（`nsproxy.c:535`） | `commit_creds`（NEWUSER）、`set_fs_root/pwd`（NEWNS 且还附带了别的）、`exit_sem`（NEWIPC）、`timens_commit`（NEWTIME）、`switch_task_namespaces` |

`prepare_nsset()` 里 `fs_struct` 的复制策略很讲究（`nsproxy.c:370`）：

```c
if (flags == CLONE_NEWNS) {
	nsset->fs = me->fs;              /* 只进 mount ns：复用，commit 时自然也生效 */
} else if (flags & CLONE_NEWNS) {
	nsset->fs = copy_fs_struct(me->fs);  /* 混合场景：必须复制，commit 时才 set_fs_root */
}
```

对应 `commit_nsset()` 的条件 `(flags & CLONE_NEWNS) && (flags & ~CLONE_NEWNS)`：只有“混合场景”才需要把临时 fs 的 root/pwd 搬回 `me->fs`。

pidfd 路径（`validate_nsset()`，`nsproxy.c:398`）额外做两件事：

1. `ptrace_may_access(tsk, PTRACE_MODE_READ_REALCREDS)`——只有能 ptrace 目标，才允许套用它的 ns；
2. `CLONE_NEWPID` 时取的是 `task_active_pid_ns(tsk)`（目标**当前**的 pid ns），而不是 `nsproxy->pid_ns_for_children`。这与前一节“nsproxy 存的是给子进程用的”相呼应：`setns` 要加入的是对方“实际所在”的 pid ns。

## 20. PID namespace：创建、销毁与 `zap_pid_ns_processes()`

`create_pid_namespace()`（`kernel/pid_namespace.c:76`）的失败路径与顺序：

```c
level = parent_pid_ns->level + 1;
if (!in_userns(parent_pid_ns->user_ns, user_ns)) return ERR_PTR(-EINVAL);  /* 必须嵌套在父 userns 内 */
if (level > MAX_PID_NS_LEVEL)                      return ERR_PTR(-ENOSPC); /* MAX_PID_NS_LEVEL = 32 */
ucounts = inc_pid_namespaces(user_ns);                                     /* UCOUNT_PID_NAMESPACES */
ns = kmem_cache_zalloc(pid_ns_cachep, GFP_KERNEL);
idr_init(&ns->idr);
ns->pid_cachep = create_pid_cachep(level);          /* 每层一个 struct pid 的 slab cache */
ns_common_init(ns);
ns->pid_max = PID_MAX_LIMIT;
register_pidns_sysctls(ns);                         /* /proc/sys/kernel/ns_last_pid 等 */
ns->level = level;
ns->parent = get_pid_ns(parent_pid_ns);
ns->user_ns = get_user_ns(user_ns);
ns->ucounts = ucounts;
ns->pid_allocated = PIDNS_ADDING;                   /* 1U << 31，创建期的特殊计数 */
INIT_WORK(&ns->work, destroy_pid_namespace_work);
ns_tree_add(ns);
```

两个细节值得注意：

- `level > MAX_PID_NS_LEVEL` 判断的是“新层级”，因此根 ns（`init_pid_ns`，level=0）之外最多 32 层；`struct pid` 的 `numbers[]` 长度按层级在 `create_pid_cachep()` 时确定，所以**层级深度与 slab cache 一一对应**，不能在 ns 创建后改；
- `ns->pid_allocated = PIDNS_ADDING` 是个“正在创建”的哨兵值（`include/linux/pid_namespace.h`），`put_pid_ns()` 只有在 `ns_ref_put()` 归零时才 `schedule_work(&ns->work)`。

销毁是“沿父链批量回收”：

```c
static void destroy_pid_namespace_work(struct work_struct *work)
{
	struct pid_namespace *ns = container_of(work, struct pid_namespace, work);
	do {
		struct pid_namespace *parent = ns->parent;
		destroy_pid_namespace(ns);            /* ns_tree_remove + unregister sysctl + ns_common_free + idr_destroy */
		ns = parent;
	} while (ns != &init_pid_ns && ns_ref_put(ns));
}
```

即：子 ns 的 work 会顺手把“已经没有引用的父 ns”一起回收，避免多层嵌套时需要每层各自排队；`destroy_pid_namespace()`（`pid_namespace.c:150`）最后 `call_rcu(&ns->rcu, delayed_free_pidns)`，`delayed_free_pidns()` 才 `dec_pid_namespaces()`/`put_user_ns()`/`kmem_cache_free()`。

`zap_pid_ns_processes()`（`pid_namespace.c:192`）是该 ns 的 init 退出时的收尾：

```text
disable_pid_allocation(pid_ns)                         /* 禁止再分配 pid */
me->sighand->action[SIGCHLD-1].sa_handler = SIG_IGN     /* 让子进程自动 reap，加速关闭 */
idr_for_each_entry_continue(&pid_ns->idr, pid, nr)      /* nr 从 2 开始，跳过保留位 */
	→ 对存活任务 group_send_sig_info(SIGKILL, SEND_SIG_PRIV, task, PIDTYPE_MAX)
do { clear TIF_SIGPENDING/TIF_NOTIFY_SIGNAL; rc = kernel_wait4(-1, NULL, __WALL, NULL); } while (rc != -ECHILD);
for (;;) { 若 pid_ns->pid_allocated == init_pids 则 break; schedule(); }
if (pid_ns->reboot) current->signal->group_exit_code = pid_ns->reboot;
acct_exit_ns(pid_ns);
```

- `init_pids = thread_group_leader(me) ? 1 : 2`（`pid_namespace.c:197`）：init 自己占 1 个 pid，若它还有其他线程则占 2 个；
- 那个 `for (;;) schedule()` 等的是“所有子 ns 进程彻底消失”，因为 `kernel_wait4()` 只能等到自己可见的僵尸，`setns()+fork()` 出来的、父进程在 ns 外的僵尸需要靠 `pid_allocated` 归零；`reboot` 字段则把“容器 reboot”的退出码映射到 init 的退出状态。

## 21. PID ns 的 `install` 约束：只能“进入更深”

`pidns_install()`（`pid_namespace.c:392`）的核心是 `pidns_is_ancestor(new, active)`：

```c
if (!ns_capable(new->user_ns, CAP_SYS_ADMIN) ||
    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN)) return -EPERM;
if (!pidns_is_ancestor(new, active)) return -EINVAL;
put_pid_ns(nsproxy->pid_ns_for_children);
nsproxy->pid_ns_for_children = get_pid_ns(new);
```

`pidns_is_ancestor()`（`pid_namespace.c:379`）是“从 child 沿 parent 往上走到与 ancestor 同层再比较”：`child->level < ancestor->level` 直接 false，否则循环上溯后判断 `ns == ancestor`。

所以 `setns` 到一个“兄弟 pid ns”会 `-EINVAL`，只能进入当前 active ns 的后代（或自身）。这条约束保证 `fork()` 返回值在调用者 ns 里始终有意义，也保证进程无法横向逃出当前 pid ns。

`pidns_get_parent()`（`pid_namespace.c:418`）配合 `NS_GET_PARENT`：只有当父 ns 在**调用者的 active ns 链**上（即调用者能“看到”它）时才返回，否则 `-EPERM`。`pidns_for_children_operations`（`pid_namespace.c:450`）与 `pidns_operations`（`pid_namespace.c:441`）共用同一 `install`，区别只在 `get` 取的是 `pid_ns_for_children` 还是 `task_active_pid_ns()`。

## 22. Mount namespace：`alloc_mnt_ns()` 与跨 userns 的 `CL_SLAVE`

`alloc_mnt_ns()`（`fs/namespace.c:4204`）有两个容易被忽略的分支：

```c
ucounts = inc_mnt_namespaces(user_ns);            /* UCOUNT_MNT_NAMESPACES */
new_ns = kzalloc_obj(struct mnt_namespace, GFP_KERNEL_ACCOUNT);
if (anon)
	ret = ns_common_init_inum(new_ns, MNT_NS_ANON_INO);   /* 匿名 ns：固定 inum，不可 setns */
else
	ret = ns_common_init(new_ns);                           /* 正常 ns：分配 inum/ns_id */
ns_tree_gen_id(new_ns);
new_ns->is_anon = anon;
refcount_set(&new_ns->passive, 1);
new_ns->mounts = RB_ROOT;
init_waitqueue_head(&new_ns->poll);
```

- **匿名 mount ns**（`is_anon`）用 `MNT_NS_ANON_INO`，`mntns_install()` 直接 `-EINVAL` 拒绝（`fs/namespace.c:6501`），因此“匿名 ns 不可 setns”；它主要用于内部挂载（如 `fsopen`/`fsmount` 的临时上下文）；
- `passive` 引用是“不钉住 mounts 的引用”，与 `ns_common.__ns_ref` 分工不同，`put_mnt_ns()` 里两者分别递减。

`copy_mnt_ns()`（`fs/namespace.c:4241`）的复制策略：

```c
if (likely(!(flags & CLONE_NEWNS))) { get_mnt_ns(ns); return ns; }
guard(namespace_excl)();
if (flags & CLONE_EMPTY_MNTNS) copy_flags = 0;
else                          copy_flags = CL_COPY_UNBINDABLE | CL_EXPIRE;
if (user_ns != ns->user_ns)   copy_flags |= CL_SLAVE;
new = (flags & CLONE_EMPTY_MNTNS) ? clone_mnt(old, old->mnt.mnt_root, copy_flags)
                                 : copy_tree(old, old->mnt.mnt_root, copy_flags);
if (user_ns != ns->user_ns) { guard(mount_writer)(); lock_mnt_tree(new); }
```

三种复制语义：

| 场景 | copy_flags | 效果 |
|------|-----------|------|
| 普通 `CLONE_NEWNS` | `CL_COPY_UNBINDABLE \| CL_EXPIRE` | 完整复制挂载树；unbindable 挂载也复制（但在新 ns 中仍不可 bind）；过期挂载直接丢弃 |
| `CLONE_EMPTY_MNTNS` | 0 | 只 `clone_mnt()` 根挂载，得到“只有 / 的空 ns” |
| 跨 userns（`user_ns != ns->user_ns`） | 追加 `CL_SLAVE` | 新树整体成为旧树的 slave，mount 事件单向传播，防止容器反向影响宿主 |

`mntns_install()`（`fs/namespace.c:6487`）的检查串起来是一条“必须完全在掌控内”的清单：

```c
if (!ns_capable(mnt_ns->user_ns, CAP_SYS_ADMIN) ||
    !ns_capable(user_ns, CAP_SYS_CHROOT) ||
    !ns_capable(user_ns, CAP_SYS_ADMIN)) return -EPERM;
if (is_anon_ns(mnt_ns)) return -EINVAL;
if (fs->users != 1)     return -EINVAL;        /* 不能有共享 fs_struct（线程/CLONE_FS） */
...
err = vfs_path_lookup(mnt_ns->root->mnt.mnt_root, &mnt_ns->root->mnt, "/", LOOKUP_DOWN, &root);
set_fs_pwd(fs, &root); set_fs_root(fs, &root);
```

`fs->users != 1` 这一条是实践中最常见的 `setns` 失败原因：多线程程序（或 `clone(CLONE_FS)` 出来的进程）共享 `fs_struct`，必须先 `unshare(CLONE_FS)` 或确保单线程。

## 23. Network namespace：`pernet_ops_rwsem` 与异步销毁

`setup_net()`（`net/core/net_namespace.c:436`）必须持 `pernet_ops_rwsem` 读侧：

```text
net_cookie = ns_tree_gen_id(net)                 /* nstree 的 64 位 id */
list_for_each_entry(ops, &pernet_list, list) { error = ops_init(ops, net); if (error) goto out_undo; }
down_write(&net_rwsem); list_add_tail_rcu(&net->list, &net_namespace_list); up_write(&net_rwsem);
ns_tree_add_raw(net)                             /* 加入 ns 树，但对用户态不可见 */
out_undo: ops_undo_list(&pernet_list, ops, ..., false); rcu_barrier();
```

`copy_net_ns()`（`net_namespace.c:549`）在 `CLONE_NEWNET` 时的完整代价是：`inc_net_namespaces()` → `net_alloc()`（含 `net_alloc_generic()`，`CONFIG_KEYS` 时建 `key_domain`）→ `preinit_net()`（`ptype_all`/`ptype_specific` 链表、`preinit_net_sysctl`）→ `down_read_killable(&pernet_ops_rwsem)` → `setup_net()` → `up_read()`。

销毁完全异步（`net_namespace.c:662`）：

```text
put_net() 归零 → net_passive_dec()/____put_net() → llist_add(cleanup_list) → queue_work(netns_wq)
cleanup_net():
  llist_del_all(&cleanup_list)                        /* 批量快照 */
  down_read(&pernet_ops_rwsem)
  down_write(&net_rwsem): ns_tree_remove + list_del_rcu + net->is_dying = true
  unhash_nsid(last)                                   /* 清 NETNSA_NSID 映射并 RTM_DELNSID */
  ops_undo_list(&pernet_list, NULL, &net_exit_list, true)   /* 逆序 exit */
  up_read(&pernet_ops_rwsem); rcu_barrier(); net_complete_free()
  最后：ns_common_free + dec_net_namespaces + key_remove_domain + put_user_ns + net_passive_dec
```

关键点：

- `cleanup_net()` 记录 `cleanup_net_task`，`net_ns_barrier()` 用“拿一次 `pernet_ops_rwsem` 写锁”等它结束；模块卸载必须调，否则 `pernet_operations` 代码可能被卸载后仍被调用；
- `net_passive_dec()`（`net_namespace.c:530`）先释放 `net->gen`、`ref_tracker_dir_exit()`，再挂到 `defer_free_list`，由 `net_complete_free()` 在 `rcu_barrier()` 后统一 `kmem_cache_free`——多等一个 grace period 是为了 `rtnl`/路由缓存里可能残留的指针。

## 24. User namespace：`create_user_ns()` 的六道门槛

`create_user_ns()`（`kernel/user_namespace.c:83`）在真正分配前要做一系列检查，顺序即“拒绝优先级”：

```c
/* 前置检查（顺序即拒绝优先级） */
if (parent_ns->level > 32) goto fail;                     /* 嵌套深度上限 32 */
ucounts = inc_user_namespaces(parent_ns, owner);          /* UCOUNT_USER_NAMESPACES */
if (!ucounts) goto fail;
if (current_chrooted()) goto fail_dec;                    /* chroot 后禁止建 userns */
if (!kuid_has_mapping(parent_ns, owner) ||
    !kgid_has_mapping(parent_ns, group)) goto fail_dec;    /* 创建者必须在父 ns 有映射 */
ret = security_create_user_ns(new);                       /* LSM 可否决 */
if (ret < 0) goto fail_dec;

/* 分配与初始化 */
ns->parent_could_setfcap = cap_raised(new->cap_effective, CAP_SETFCAP);
ns_common_init(ns);
ns->parent = parent_ns; ns->level = parent_ns->level + 1;
ns->owner = owner; ns->group = group;
INIT_WORK(&ns->work, free_user_ns);
for (i = 0; i < UCOUNT_COUNTS; i++) ns->ucount_max[i] = INT_MAX;
set_userns_rlimit_max(ns, UCOUNT_RLIMIT_NPROC, enforced_nproc_rlimit());
set_userns_rlimit_max(ns, UCOUNT_RLIMIT_MSGQUEUE, rlimit(RLIMIT_MSGQUEUE));
set_userns_rlimit_max(ns, UCOUNT_RLIMIT_SIGPENDING, rlimit(RLIMIT_SIGPENDING));
set_userns_rlimit_max(ns, UCOUNT_RLIMIT_MEMLOCK, rlimit(RLIMIT_MEMLOCK));
ns->flags = parent_ns->flags;                 /* 继承 USERNS_SETGROUPS_ALLOWED */
setup_userns_sysctls(ns);
set_cred_user_ns(new, ns);                    /* 关键：新 cred 指向新 userns */
ns_tree_add(ns);
```

`set_cred_user_ns(new, ns)` 是“容器 root 诞生”的那一刻：`new`（`prepare_creds()` 出来的副本）的 `user_ns` 换成新 ns，调用方随后 `commit_creds()`，于是该进程在新 ns 内拥有全部 capability。

三个配套机制：

- `ns_capable(ns, cap)`（`kernel/capability.c:361`）：沿 `ns->parent` 链检查“目标 ns 及其祖先”，这是“容器 root 不能影响宿主”的根；
- `userns_may_setgroups()`（`kernel/user_namespace.c:1292`）：只有 `USERNS_SETGROUPS_ALLOWED` 且已写 gid_map 才允许 `setgroups`，防止非特权用户借 setgroups 提权；
- `map_write()`（`kernel/user_namespace.c:932`）：
  - 只允许“文件开头、单次、小于一页”的写（`*ppos != 0 || count >= PAGE_SIZE` → `-EINVAL`）；
  - `userns_state_mutex` 串行化所有 map 写，`map->nr_extents != 0` 时拒绝二次写；
  - 非特权写入只能映射自己的 uid/gid（`cap_setid` 参数由 `proc_uid_map_write`/`proc_gid_map_write` 传 `CAP_SETUID`/`CAP_SETGID`）；
  - 查表用 `map_id_range_down()`（`user_namespace.c:318`）/`map_id_up()` 的二分，未映射返回 `overflowuid`/`overflowgid`。

## 25. UTS / IPC / time：三个“小而关键”的差异

| ns | 创建函数 | 关键点 | 本配置 |
|----|---------|--------|--------|
| UTS | `clone_uts_ns()`（`kernel/utsname.c:36`）/`copy_utsname()`（`:79`） | `down_read(&uts_sem)` 复制 `struct new_utsname`；`sethostname` 用 `down_write(&uts_sem)` | `CONFIG_UTS_NS=y` |
| IPC | `create_ipc_ns()`（`ipc/namespace.c:39`）/`copy_ipcs()`（`:111`） | `ids[3]` 各自 `ipc_ids_init`；`mq_mnt` 是新 `mqueuefs` 实例（`mntput()` 在销毁时）；`shm_tot` 独立计账 | `CONFIG_IPC_NS=y`、`CONFIG_SYSVIPC=y`、`CONFIG_POSIX_MQUEUE=y` |
| time | `copy_time_ns()`（`kernel/time/namespace.c:134`）/`timens_install()`（`:191`）/`timens_on_fork()`（`:213`） | `time_ns_for_children` 与实际 `time_ns` 分离；`frozen_offsets` 置位后偏移不可改（`:310`） | `CONFIG_TIME_NS=y`、`CONFIG_TIME_NS_VDSO=y` |

`copy_ipcs()`（`ipc/namespace.c:111`）非常直接，**没有** pid ns 那种“必须是当前所在”的校验：无 `CLONE_NEWIPC` 时 `get_ipc_ns(ns)`，否则 `create_ipc_ns(user_ns, ns)`。只要 `ns_capable(user_ns, CAP_SYS_ADMIN)` 在 `copy_namespaces()`/`unshare_nsproxy_namespaces()` 侧通过，就能从任意 ipc ns 派生。真正限制“不可横向跳”的只有 pid ns（`pidns_is_ancestor`）与 mount ns（`fs->users == 1`）。

`timens_install()`（`kernel/time/namespace.c:191`）除了 `ns_capable(nsset->cred->user_ns, CAP_SYS_TIME)`，还要校验目标 userns 的权限；`timens_on_fork()`（`:213`）在 `copy_process()` 里被调用，把 `offsets` 搬进子任务；`timens_commit()` 在 `setns` 提交阶段把 `time_ns` 真正换上。

## 26. nsfs 与 `listns(2)`：树 + 缓存 dentry

`fs/nsfs.c` 的三个关键点：

1. `nsfs_evict()`（`fs/nsfs.c:58`）：

```c
static void nsfs_evict(struct inode *inode)
{
	struct ns_common *ns = inode->i_private;
	__ns_ref_active_put(ns);     /* inode 本身持一个 active 引用 */
	clear_inode(inode);
	ns->ops->put(ns);            /* 再放掉 ops->get() 拿到的引用 */
}
```

这解释了“nsfs fd 能保活 ns，但不阻止它变 inactive”：inode 持有的是 active 引用，inode 被回收时自然 `__ns_ref_active_put()`。

2. **stashed dentry**：`ns_get_path_cb()`（`fs/nsfs.c:67`）→ `path_from_stashed(&ns->stashed, nsfs_mnt, ns, path)`，同一个 ns 反复打开 `/proc/<pid>/ns/*` 时复用 dentry，避免 inode 爆炸；`ns_dentry_operations.d_prune = stashed_dentry_prune`（`fs/nsfs.c:53`）负责回收。
3. `open_namespace_file()`（`fs/nsfs.c:102`）是内核内部“打开某 ns 的 fd”的接口（如 `SIOCGSKNS`），它会 **consume 一个引用**。

`listns(2)`（`SYSCALL_DEFINE4(listns, ...)`，`kernel/nstree.c:763`）先校验 `flags == 0` 与 `nr_ns_ids <= 1000000`（否则 `-EOVERFLOW`），再 `copy_ns_id_req()`、`prepare_klistns()`，最后按 `req.user_ns_id` 走 `do_listns_userns()` 或 `do_listns()`。

`do_listns()`（`nstree.c:695`）的枚举逻辑：

- `hweight32(kls->ns_type) == 1` 时用 `ns_tree_from_type()` 选**类型树**，否则遍历 `ns_unified_root`（全部类型）；
- `last_ns_id` 作游标：`lookup_ns_id_at(last_ns_id + 1, ns_type)` 支持分页；
- 每个候选都要 `legitimize_ns(kls, ns)`（检查调用者 userns 是否有权看到），再 `put_user(valid->ns_id, ...)`；
- 全程 `rcu_read_lock()`，但 `put_user()` 前临时 `rcu_read_unlock()` 并列住 `prev` 引用，避免在 RCU 读侧做可能 fault 的用户拷贝。

与传统 `/proc` 枚举相比，这是“按 ns 树 + 64 位 id 游标”的稳定枚举，不受 pid 复用影响。

## 27. seccomp 数据结构：`refs` 与 `users` 双计数

`struct seccomp_filter`（`kernel/seccomp.c:224`）：

```c
struct seccomp_filter {
	refcount_t refs;              /* 内存生命周期 */
	refcount_t users;             /* 还有多少“直接/间接使用者” */
	bool log;
	bool wait_killable_recv;
	struct action_cache cache;    /* SECCOMP_ARCH_NATIVE 使能时的放行位图 */
	struct seccomp_filter *prev;  /* 形成“树”而非链：fork 共享父节点 */
	struct bpf_prog *prog;
	struct notification *notif;   /* 仅 NEW_LISTENER 时非 NULL */
	struct mutex notify_lock;
	wait_queue_head_t wqh;
};
```

两个引用计数的区别（源码注释写得很细，值得照抄理解）：

- `refs`：每个直接挂在该 filter 上的任务 +1、依赖它的子 filter（`filter->prev`）+1、user notifier 生命周期 +1；归零才释放；
- `users`：每个直接挂载任务 +1、依赖它的父 filter +1；归零表示“不可能再有新任务关联到它”，但**不一定能释放**（`users <= refs`）。

`prev` 使得 filter 结构在内存中是**单向树**（fork 出的多个任务共享父 filter），而不是每个任务一份拷贝——这就是 `SECCOMP_FILTER_FLAG_TSYNC` 能低成本同步的基础。

`action_cache` 只有 `SECCOMP_ARCH_NATIVE` 定义时才非空（`seccomp.c:168`），否则是空结构（`seccomp.c:175` 的 `struct action_cache { };`），因此无该特性的架构上 `seccomp_cache_check_allow()` 的编译结果直接落空。

## 28. 安装路径：指令重写、长度上限与 listener

`seccomp_check_filter()`（`seccomp.c:278`）不是简单校验，而是**改写**用户指令：

```c
case BPF_LD | BPF_W | BPF_ABS:
	ftest->code = BPF_LDX | BPF_W | BPF_ABS;          /* 改成用 X 寄存器基址 */
	if (k >= sizeof(struct seccomp_data) || k & 3)    /* 越界或未 4 字节对齐 */
		return -EINVAL;
	continue;
case BPF_LD | BPF_W | BPF_LEN:
	ftest->code = BPF_LD | BPF_IMM;
	ftest->k = sizeof(struct seccomp_data);           /* LEN 变成常量 */
	continue;
```

即：把 classic BPF 里“按 skb 绝对偏移取数”的语义重定向为“按 `struct seccomp_data` 偏移取数”，并把 `BPF_LEN` 折叠成 `sizeof(struct seccomp_data)`。所以 seccomp 只能看 `nr/arch/args/IP`，超界偏移一律 `-EINVAL`。

`seccomp_attach_filter()`（`seccomp.c:921`）：

```c
total_insns = filter->prog->len;
for (walker = current->seccomp.filter; walker; walker = walker->prev)
	total_insns += walker->prog->len + 4;      /* 每个上游 filter 加 4 条惩罚 */
if (total_insns > MAX_INSNS_PER_PATH) return -ENOMEM;   /* (1<<18)/sizeof(sock_filter) = 32768 */

if (flags & SECCOMP_FILTER_FLAG_TSYNC) { ret = seccomp_can_sync_threads(); ... }
if (flags & SECCOMP_FILTER_FLAG_LOG) filter->log = true;
if (flags & SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV) filter->wait_killable_recv = true;
filter->prev = current->seccomp.filter;
seccomp_cache_prepare(filter);                 /* 建 ALLOW 位图 */
current->seccomp.filter = filter;
atomic_inc(&current->seccomp.filter_count);
if (flags & SECCOMP_FILTER_FLAG_TSYNC) seccomp_sync_threads(flags);
```

`MAX_INSNS_PER_PATH` 是**整条 `prev` 链**的累计上限（256KB 指令 / 8 字节），所以“不断叠加小 filter”也会被挡住。

`seccomp_set_mode_filter()`（`seccomp.c:1956`）的关键顺序：flags 校验（`WAIT_KILLABLE_RECV` 必须同时带 `NEW_LISTENER`）→ `seccomp_prepare_user_filter()`（`copy_from_user` + `bpf_prog_create_from_user`）→ 需要 listener 时 `get_unused_fd_flags(O_CLOEXEC)` 并 `init_listener()` → TSYNC 时 `mutex_lock_killable(&current->signal->cred_guard_mutex)`（防与 exec 竞争）→ `spin_lock_irq(&current->sighand->siglock)` → `seccomp_may_assign_mode()` → `has_duplicate_listener()`（`-EBUSY`）→ `seccomp_attach_filter()` → `seccomp_assign_mode()` → 解锁后 `fd_install()`（失败则回滚 fd 与 `seccomp_notify_detach()`）。

`seccomp_assign_mode()`（`seccomp.c:447`）里有一条易漏的顺序保证：

```c
task->seccomp.mode = seccomp_mode;
smp_mb__before_atomic();                 /* 保证 mode/filter 先于 syscall work 位可见 */
if ((flags & SECCOMP_FILTER_FLAG_SPEC_ALLOW) == 0) arch_seccomp_spec_mitigate(task);
set_task_syscall_work(task, SECCOMP);
```

## 29. 执行路径：`__seccomp_filter()` 逐分支

`__secure_computing()`（`seccomp.c:1388`）先处理一个 prctl 特例，再按 mode 分派：

```c
if (IS_ENABLED(CONFIG_CHECKPOINT_RESTORE) &&
    unlikely(current->ptrace & PT_SUSPEND_SECCOMP))
	return 0;                        /* 本树 CHECKPOINT_RESTORE=n，这段不编译 */
this_syscall = syscall_get_nr(current, current_pt_regs());
switch (mode) {
case SECCOMP_MODE_STRICT: __secure_computing_strict(this_syscall); return 0;
case SECCOMP_MODE_FILTER: return __seccomp_filter(this_syscall, false);
case SECCOMP_MODE_DEAD:   WARN_ON_ONCE(1); do_exit(SIGKILL); return -1;
}
```

`__seccomp_filter()`（`seccomp.c:1259`）的分支细节：

| 分支 | 行为 | 关键代码 |
|------|------|----------|
| `SECCOMP_RET_ERRNO` | `data > MAX_ERRNO` 时截断为 `MAX_ERRNO`，`syscall_set_return_value(-data)` | `seccomp.c:1279` |
| `SECCOMP_RET_TRAP` | `syscall_rollback()` 回滚寄存器，`force_sig_seccomp(this_syscall, data, false)` | `:1287` |
| `SECCOMP_RET_TRACE` | 已由 tracer 复查过（`recheck_after_trace`）直接 `return 0`；无 tracer → `-ENOSYS`；否则 `ptrace_event(PTRACE_EVENT_SECCOMP, data)`，若 `fatal_signal_pending()` 跳过，随后用**新 syscall 号**递归 `__seccomp_filter(..., true)` | `:1294` |
| `SECCOMP_RET_USER_NOTIF` | `seccomp_do_user_notification()`，返回非 0 表示要 `skip` | `:1337` |
| `SECCOMP_RET_LOG` | `seccomp_log(...)` 后 `return 0` | `:1343` |
| `SECCOMP_RET_ALLOW` | 直接 `return 0`，且 `match` 必为 NULL | `:1347` |
| `KILL_THREAD`/`KILL_PROCESS`/default | `mode = SECCOMP_MODE_DEAD`；只有“KILL_PROCESS”或“最后一个线程”才 `syscall_rollback + force_sig_seccomp(..., true)` 打 core，否则 `do_exit(SIGSYS)` | `:1355` |

`SECCOMP_RET_TRACE` 的“重新检查”很关键：tracer 可以改寄存器中的 syscall 号，所以内核用 `syscall_get_nr()` 重新取号并**递归**跑一遍过滤器；注释明确“这里有意用 NULL 的 `struct seccomp_data` 强制重载所有寄存器”。`seccomp_run_filters()`（`seccomp.c:404`）的优先级规则是 `ACTION_ONLY(cur_ret) < ACTION_ONLY(ret)`，其中 `ACTION_ONLY` 为 `(s32)(ret & SECCOMP_RET_ACTION_FULL)`——**数值小者优先**，因此 `KILL_PROCESS(0x80000000)` 作为有符号数最小、优先级最高。

`seccomp_log()`（`seccomp.c:1008`）受 `seccomp_actions_logged`（`seccomp.c:1000`）位掩码控制，默认记 KILL/TRAP/ERRNO/NOTIF/TRACE/LOG 但不记 ALLOW；每位对应 `SECCOMP_LOG_*`（`seccomp.c:991`）。

## 30. User notification：三态机与 addfd 批处理

`struct seccomp_knotif`（`seccomp.c:61`）的状态机（`enum notify_state`，`seccomp.c:55`）：

```text
INIT ──(listener read 到)──► SENT ──(reply)──► REPLIED
  ▲                             │
  └──────(收到信号/被中断)───────┘
```

`seccomp_do_user_notification()`（`seccomp.c:1163`）的流程：

```c
mutex_lock(&match->notify_lock);
if (!match->notif) { err = -ENOSYS; goto out; }     /* 没有 listener 时退化为 ENOSYS */
n.task = current; n.state = SECCOMP_NOTIFY_INIT; n.data = sd;
n.id = seccomp_next_notify_id(match); init_completion(&n.ready);
list_add_tail(&n.list, &match->notif->notifications); INIT_LIST_HEAD(&n.addfd);
atomic_inc(&match->notif->requests);              /* FD_SYNC_WAKE_UP 时用 wake_up_poll_on_current_cpu() */
do {
	wait_killable = should_sleep_killable(match, &n);
	mutex_unlock(&match->notify_lock);
	err = wait_killable ? wait_for_completion_killable(&n.ready)
	                    : wait_for_completion_interruptible(&n.ready);
	mutex_lock(&match->notify_lock);
	if (err) { 可切换到 killable 则继续等，否则 goto interrupted; }
	addfd = list_first_entry_or_null(&n.addfd, ...);
	if (addfd) seccomp_handle_addfd(addfd, &n);     /* 处理 ADDFD 请求 */
} while (n.state != SECCOMP_NOTIFY_REPLIED);
```

两个设计点：

- **`wait_killable` 是动态的**：`SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV` 只在“通知已发出（`state >= SENT`）”后才让任务进入 killable 等待，避免监听者尚未收到就错过唤醒；
- **addfd 与应答并行**：`SECCOMP_IOCTL_NOTIF_ADDFD` 把 `struct seccomp_kaddfd` 挂到 `n.addfd`，被通知线程在等待循环里处理并 `complete(&addfd->completion)`，因此“注入 fd”不需要额外唤醒。

返回语义：

```c
if (flags & SECCOMP_USER_NOTIF_FLAG_CONTINUE) return 0;   /* 让 syscall 继续（有 TOCTOU 风险） */
syscall_set_return_value(current, current_pt_regs(), err, ret);
return -1;                                                /* 跳过 syscall */
```

中断路径里会把所有未处理的 addfd 标成 `-ESRCH` 并 `complete()`，避免 listener 侧永久等待。

## 31. 与 ptrace / clone / audit 的衔接

| 交互点 | 代码 | 说明 |
|--------|------|------|
| `PTRACE_EVENT_SECCOMP` | `seccomp.c:1308` 的 `ptrace_event(PTRACE_EVENT_SECCOMP, data)` | `data` 低 16 位透传给 tracer，tracer 可在停在事件点时改寄存器 |
| `PT_SUSPEND_SECCOMP` | `seccomp.c:1393` | 依赖 `CONFIG_CHECKPOINT_RESTORE`，本树**未启用**，无法通过 ptrace 暂停 seccomp |
| `SECCOMP_GET_FILTER`/`GET_METADATA` | 同属 `CONFIG_CHECKPOINT_RESTORE` | 本树不可用，C/R 工具无法导出 filter |
| fork 继承 | `get_seccomp_filter()`（`seccomp.c:979`） | `refs++` 与 `users++` 各一次；`copy_process()` 里调用 |
| 线程同步 / 退出释放 | `seccomp_sync_threads()`（`seccomp.c:598`）、`seccomp_filter_release()`（`:570`） | TSYNC 把新 filter 挂到所有线程；退出时递减 `users`/`refs` 并处理 notifier |
| audit | `seccomp_log()` → `audit_seccomp()` | 需要 `CONFIG_AUDIT`/`CONFIG_AUDITSYSCALL`，本树均 `=y` |

TSYNC 与 `exec` 的竞态由 `cred_guard_mutex` 封堵（`seccomp.c:2013`）：`exec` 持该 mutex 写侧，因此 TSYNC 的 `mutex_lock_killable()` 会等待，避免“exec 后 filter 不一致”。

## 32. 相关笔记与调用链速查

本笔记相关的系统调用细节见：

- `unshare(2)`：[../02-core/syscall/process/unshare.md](../02-core/syscall/process/unshare.md)
- `setns(2)`：[../02-core/syscall/user-group/setns.md](../02-core/syscall/user-group/setns.md)
- `seccomp(2)`：[../02-core/syscall/security/seccomp.md](../02-core/syscall/security/seccomp.md)
- `clone(2)`/`clone3(2)`：[../02-core/syscall/process/clone.md](../02-core/syscall/process/clone.md)、[../02-core/syscall/process/clone3.md](../02-core/syscall/process/clone3.md)
- `listns(2)`：[../02-core/syscall/system-info/listns.md](../02-core/syscall/system-info/listns.md)
- cgroup 侧：[01-cgroup-v2.md](01-cgroup-v2.md)

三条最常用的调用链：

```text
# 1. clone 带新 ns
clone(2)/clone3(2) → kernel_clone() → copy_process() → copy_namespaces(flags, p)   nsproxy.c:169
  → create_new_namespaces() → copy_mnt_ns/copy_utsname/copy_ipcs/copy_pid_ns/
      copy_cgroup_ns/copy_net_ns/copy_time_ns → (非 CLONE_VM) timens_on_fork()

# 2. 写 cgroup.procs 进容器（namespace 与 cgroup 的交叉点）
write(cgroup.procs) → cgroup_procs_write_permission()
  → 若 CGRP_ROOT_NS_DELEGATE，用 current->nsproxy->cgroup_ns->root_cset 做边界判定

# 3. 一次系统调用的 seccomp 判定
syscall entry → syscall_work 含 SECCOMP → secure_computing() → __secure_computing()
  → __seccomp_filter() → seccomp_run_filters()（cache → BPF 链，取最小 ACTION）
  → ERRNO/TRAP/TRACE/USER_NOTIF/LOG/KILL_* 分派 → seccomp_log()
```

## 关键文件清单

| 路径 | 说明 |
|------|------|
| `include/linux/nsproxy.h` | `struct nsproxy`、`struct nsset`、访问规则 |
| `include/linux/ns_common.h` / `include/linux/ns/ns_common_types.h` | `ns_common` 两级引用、`ns_ref_*` 宏、`NS_COMMON_INIT` |
| `include/linux/proc_ns.h` | `struct proc_ns_operations`、各 `*ns_operations` 声明 |
| `kernel/nsproxy.c` | `copy_namespaces`、`create_new_namespaces`、`unshare_nsproxy_namespaces`、`switch_task_namespaces`、`setns` |
| `kernel/fork.c` | `ksys_unshare`（3217）、`copy_process` 中的 namespace 处理 |
| `kernel/nsproxy.c` / `kernel/nstree.c` | 命名空间树、`listns(2)`（763） |
| `kernel/pid_namespace.c` | `create_pid_namespace`(76)、`copy_pid_ns`(175)、`zap_pid_ns_processes`(192)、`pidns_install`(392) |
| `kernel/pid.c` | `alloc_pid`(159)、`pid_nr_ns`(542)、`__task_pid_nr_ns` |
| `fs/namespace.c` / `fs/mount.h` / `fs/pnode.c` | `mnt_namespace`、`copy_mnt_ns`(4241)、`pivot_root`(4775)、`mntns_install`(6487)、shared subtree |
| `net/core/net_namespace.c` / `include/net/net_namespace.h` | `struct net`、`copy_net_ns`(549)、`setup_net`(436)、`cleanup_net`、pernet_operations |
| `kernel/user_namespace.c` / `include/linux/user_namespace.h` | `create_user_ns`(83)、`map_id_range_down`、`make_kuid`/`from_kuid`、`ucounts` |
| `kernel/utsname.c` / `include/linux/uts_namespace.h` | `uts_namespace`、`copy_utsname`、`uts_sem` |
| `ipc/namespace.c` / `include/linux/ipc_namespace.h` | `ipc_namespace`、`create_ipc_ns`(39)、`copy_ipcs`(111)、`mq_mnt` |
| `kernel/time/namespace.c` / `include/linux/time_namespace.h` | `time_namespace`、`timens_offsets`、`copy_time_ns`(134)、`timens_install`(191)、`timens_on_fork`(213) |
| `kernel/cgroup/namespace.c` | `cgroup_namespace`、`copy_cgroup_ns` |
| `fs/nsfs.c` / `include/uapi/linux/nsfs.h` | nsfs inode、`ns_ioctl`、`NS_GET_*` |
| `fs/proc/namespaces.c` | `/proc/<pid>/ns/*` 符号链接与 `ns_entries[]` |
| `kernel/seccomp.c` | `struct seccomp_filter`(224)、`action_cache`、`seccomp_run_filters`(404)、`__seccomp_filter`(1259)、`seccomp_attach_filter`、`seccomp_do_user_notification`(1163)、`SYSCALL_DEFINE3(seccomp)`(2126) |
| `include/linux/seccomp_types.h` | `struct seccomp { mode, filter_count, filter }` |
| `include/linux/seccomp.h` | `secure_computing()`、滤镜 flag 掩码、`prctl_get/set_seccomp` |
| `include/uapi/linux/seccomp.h` | `SECCOMP_MODE_*`、`SECCOMP_RET_*`、`SECCOMP_FILTER_FLAG_*`、`struct seccomp_notif*` |
| `kernel/seccomp.c` 中 `seccomp_log()` | seccomp 审计输出（配合 `audit`） |
