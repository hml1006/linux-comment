# getuid 系统调用分析

## 1. 概述

获取当前进程的实际用户 ID（real UID）。此系统调用是获取进程身份标识的最基本操作之一，返回的 UID 值经过用户命名空间映射。

**原型：**

```c
SYSCALL_DEFINE0(getuid)
```

**参数：** 无

**返回值：** 当前进程的实际用户 ID（`uid_t`）

### 1.1 源码全貌

在 `kernel/sys.c` 里它只有三行，且没有加任何锁：

```c
SYSCALL_DEFINE0(getuid)
{
	/* Only we change this so SMP safe */
	return from_kuid_munged(current_user_ns(), current_uid());
}
```

注释 `Only we change this so SMP safe` 是关键：`cred` 一旦发布就是不可变的（`commit_creds()` 会换上一个新对象），所以读自己的凭证不需要加锁，只需保证指针本身是稳定的。`current_cred()` 的实现正是如此：

```c
#define current_cred() \
	rcu_dereference_protected(current->cred, 1)
```

`rcu_dereference_protected(..., 1)` 表示"当前任务只能由自己替换自己的 cred"，因此可以跳过 `rcu_read_lock()`。这是 `getuid()` 比 `getppid()` 少一次 RCU 临界区的原因——后者要用 `task_tgid_vnr(rcu_dereference(current->real_parent))` 并显式 `rcu_read_lock()`。

## 2. 使用场景

- 进程身份识别
- 用户权限检查
- 日志记录（谁在执行什么操作）

## 3. 函数调用栈

```
getuid()                                                 // kernel/sys.c
  └─ from_kuid_munged(current_user_ns(), current_uid())
       ├─ current_uid() → current->cred->uid            // 读取凭证中的实际 UID
       └─ from_kuid_munged() → 将内核 UID 映射到用户命名空间
```

### 3.1 映射的实际实现

`kuid_t` 是"内核全局 UID"，`uid_t` 是"某个用户命名空间里的 UID"，两者之间是一次查表。本树开启 `CONFIG_USER_NS=y`，走的是 `kernel/user_namespace.c` 的真实实现：

```c
uid_t from_kuid_munged(struct user_namespace *targ, kuid_t kuid)
{
	uid_t uid;
	uid = from_kuid(targ, kuid);

	if (uid == (uid_t) -1)
		uid = overflowuid;
	return uid;
}
```

`from_kuid()` 只是 `map_id_up(&targ->uid_map, __kuid_val(kuid))`，而 `map_id_up()` 转发给 `map_id_range_up(map, id, 1)`：

```c
u32 map_id_range_up(struct uid_gid_map *map, u32 id, u32 count)
{
	struct uid_gid_extent *extent;
	unsigned extents = map->nr_extents;
	smp_rmb();

	if (extents <= UID_GID_MAP_MAX_BASE_EXTENTS)
		extent = map_id_range_up_base(extents, map, id, count);
	else
		extent = map_id_range_up_max(extents, map, id, count);

	if (extent)
		id = (id - extent->lower_first) + extent->first;
	else
		id = (u32) -1;

	return id;
}
```

细节：

- `smp_rmb()` 与写侧的屏障配对，保证"先看到 `nr_extents` 再看到 extent 数组内容"；`uid_map` 支持在挂载后一次性写入（`uid_map` 只能写一次），但仍是 RCU 风格的发布。
- 映射条目少（`<= UID_GID_MAP_MAX_BASE_EXTENTS`，即 5 条）时线性扫描 `extent[]`（每个条目形如 `inside_id  outside_id  count`，都用 `lower_first/first/count` 表示）；条目多时改用已排序的 `map->reverse` 数组做 `bsearch()`。
- 映射公式 `(id - lower_first) + first` 就是把"命名空间内 ID"平移到"父命名空间 ID"。
- 找不到映射返回 `(u32)-1`，`from_kuid_munged()` 把它换成 `overflowuid`。

### 3.2 四个 get*id 的对称性

`kernel/sys.c` 中相邻的四个实现完全同构，只有字段和 GID 版本不同：

```c
SYSCALL_DEFINE0(getuid)  { return from_kuid_munged(current_user_ns(), current_uid()); }
SYSCALL_DEFINE0(geteuid) { return from_kuid_munged(current_user_ns(), current_euid()); }
SYSCALL_DEFINE0(getgid)  { return from_kgid_munged(current_user_ns(), current_gid()); }
SYSCALL_DEFINE0(getegid) { return from_kgid_munged(current_user_ns(), current_egid()); }
```

`current_uid()` 等宏统统展开为 `current_cred()->field`，因此这四个调用的代价都是"读一个指针 + 一次数组/二分查表"。注意本树 `.config` 中 `CONFIG_SECURITY` 未启用，`struct cred` 里没有 `void *security`（LSM blob）成员，`CONFIG_KEYS` 未启用也没有四个 keyring 指针——这些字段在源码中存在，但当前配置不参与编译。另外 `getuid()` 不经过任何 LSM 钩子（`security/security.c` 里只有 `security_task_getpgid()`/`security_task_getsid()` 这类"能返回进程组/会话信息"的钩子），它纯粹是"读自己的 cred"。

## 4. 关键数据结构

### 4.1 struct cred（进程凭证）

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID of the task
    kuid_t gid;          // real GID of the task
    kuid_t suid;         // saved UID (setuid 保留)
    kgid_t sgid;         // saved GID
    kuid_t euid;         // effective UID
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ... (capabilities, keys, LSM, etc.)
};
```

### 4.2 八个身份字段的语义

`struct cred` 前 8 个字段是四个 `kuid_t` 与四个 `kgid_t` 交错排列，`getuid()` 只碰第一个：

| 字段 | 名称 | 语义 | 谁使用 |
|--|--|--|--|
| `uid` | real UID | 进程的真实属主，`fork()` 后保持不变 | `getuid()`、`/proc/pid/status` 的 `Uid:` 首列、审计日志 |
| `euid` | effective UID | 决定"当前这一步"是否拥有特权 | `geteuid()`、`access()` 类判定、`setuid()` 的权限检查 |
| `suid` | saved UID | 供 setuid 程序在"临时降权后恢复" | `setresuid()`、`execve()` 的 setuid 处理 |
| `fsuid` | fs UID | 专门用于 VFS 权限判定，可被 `setfsuid()` 单独修改 | `inode_permission()`、文件创建属主 |
| `gid`/`egid`/`sgid`/`fsgid` | 对应 GID | 同上，另加 `getgroups()` 的补充组 | 组权限判定 |

`fsuid` 与 `euid` 分离是一个历史设计：NFS 服务器可以在不为每个请求 `seteuid()` 的情况下，只切换文件系统视角的身份。所以"看起来是 root（euid=0）但文件访问按 nobody 判定"是可能的。

### 4.3 cred 的不可变性与引用计数

```c
struct cred {
	atomic_long_t	usage;
	...
	/* RCU deletion */
	union {
		int non_rcu;			/* Can we skip RCU deletion? */
		struct rcu_head		rcu;		/* RCU deletion hook */
	};
} __randomize_layout;
```

三个设计要点：

1. **发布后不可变**：`prepare_creds()` 复制旧 cred，`commit_creds()` 用 `rcu_assign_pointer(task->real_cred, new)` 与 `rcu_assign_pointer(task->cred, new)` 换上（并 `get_cred(new)` 为 `real_cred` 与 `cred` 各持一份引用，最后 `put_cred_many(old, 2)` 释放旧的）。因此 `getuid()` 读到的单一字段天然一致。
2. **释放延迟**：`put_cred()` 减到 0 后走 `put_cred_rcu()` → `__put_cred()`，前者是 `call_rcu()` 回调，保证还在无锁读 `cred` 的路径（如 `/proc`、audit）不会看到已释放内存。
3. `__randomize_layout` 是 `CONFIG_RANDSTRUCT` 的结构体布局随机化，纯粹是防御性措施；本树 `.config` 未启用 RANDSTRUCT。

`commit_creds()` 里还藏着一个与 `getuid()` 相关的安全细节：当 euid/egid/fsuid/fsgid 或 capability 集合发生变化时，会先调整 `task_exec_state_set_dumpable(suid_dumpable)` 与 `pdeath_signal`，并保证"不可 dump 的可见性"早于"凭证变化"可见，防止 `ptrace_may_access()` 竞态。

## 5. 流程图

```
用户态: getuid()
    │
    v
┌─────────────────────────────────────┐
│ current_uid()                       │
│ → current->cred->uid (内核 kuid_t)  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ from_kuid_munged(user_ns, uid)      │
│ 将 kuid_t 映射到当前用户命名空间    │
│ 若 UID 在命名空间外 → 返回 overflow │
│ uid (65534)                         │
└─────────────────────────────────────┘
    │
    v
返回 uid_t 值
```

### 5.1 映射失败时的兜底值

`from_kuid()` 在"该 kuid 在目标命名空间里没有映射"时返回 `(uid_t)-1`，而 `from_kuid_munged()` 把 `-1` 换成全局 `overflowuid`：

```c
int overflowuid = DEFAULT_OVERFLOWUID;     /* include/linux/highuid.h: 65534 */
int overflowgid = DEFAULT_OVERFLOWGID;

sysctl: kernel.overflowuid  (proc_dointvec_minmax, 0..SYSCTL_MAXOLDUID)
```

这解释了一个常见疑问："为什么容器里看不到宿主机的某个 UID，`getuid()` 却显示 65534（nobody）而不是报错？"因为 `getuid(2)` 的 ABI 承诺"总是返回一个合法的 `uid_t`"，错误无法通过返回值表达。与之相对，`from_kuid()` 保留 `-1`，供 `stat()` 的 `st_uid`、`/proc` 的 mapping 检查等"可以明确失败"的场景使用。

注意还有一个 `fs_overflowuid`（`DEFAULT_FS_OVERFLOWUID`，同样 65534），专供文件系统元数据格式化使用，和 `overflowuid` 是两个独立 sysctl。

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    uid_t uid = getuid();
    uid_t euid = geteuid();

    printf("Real UID:      %d\n", uid);
    printf("Effective UID: %d\n", euid);

    if (uid == 0)
        printf("Running as root\n");
    else
        printf("Running as user %d\n", uid);

    return 0;
}
```

### 6.1 错误码与 ABI 细节

| 项目 | 说明 |
|--|--|
| 错误码 | **无**。`getuid()` 不会失败，`glibc` 甚至不设置 `errno` |
| 返回值 | 32 位 `uid_t`，经当前用户命名空间映射，越界时得 `overflowuid` |
| 线程语义 | 凭证是 per-thread 的（`task->cred`），但 `setuid()` 由 POSIX 要求"同进程所有线程一起改"；`getuid()` 读的是本线程的 cred，正常情况下两者一致 |
| 原生编号（arm64） | `getuid` = 174（`sys_getuid`） |
| compat 编号（arm64 32 位） | `getuid32` = 199（`sys_getuid`）、`getuid` = 24（`sys_getuid16`） |
| 16 位截断 | `kernel/uid16.c` 的 `getuid16()` = `high2lowuid(from_kuid_munged(...))`，把 65534 映射为 `0xFFFE` |

`high2lowuid()` 的定义是 `((uid) & ~0xFFFF ? (old_uid_t)overflowuid : (old_uid_t)(uid))`：16 位放不下的值统一压成 `overflowuid`（65534，即 `0xFFFE`），保证老的 16 位 ABI 也有确定的兜底值。该特性依赖 `CONFIG_UID16`（`HAVE_UID16 && MULTIUSER`，默认 y）；本树 `include/generated/autoconf.h` 中未定义 `CONFIG_UID16`，因此 `kernel/uid16.o` 不参与本次编译，`getuid16` 只作为源码里的 compat 兼容路径存在。

### 6.2 相关系统调用

| 系统调用 | 关系 | 笔记 |
|--|--|--|
| `geteuid` | 同一个 cred 的 `euid` 字段 | [geteuid.md](geteuid.md) |
| `getgid` / `getegid` | GID 版本，走 `from_kgid_munged` | [getgid.md](getgid.md)、[getegid.md](getegid.md) |
| `getresuid` | 一次取回 real/effective/saved 三个 UID | [getresuid.md](getresuid.md) |
| `setuid` / `setreuid` / `setresuid` | 修改这些字段的唯一途径 | [setuid.md](setuid.md)、[setreuid.md](setreuid.md)、[setresuid.md](setresuid.md) |
| `setfsuid` | 单独修改 fsuid，不影响 euid | [setfsuid.md](setfsuid.md) |
| `capget` | 与 UID 一起构成"进程特权"的完整视图 | [capget.md](capget.md) |

## 7. 参考

- 源码位置：`kernel/sys.c`
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#10-进程凭证与权限)