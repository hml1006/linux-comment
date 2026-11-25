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
