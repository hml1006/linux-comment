# fsmount 系统调用

> 所属分类：文件系统挂载与结构 | [ARM64 系统调用表](../arm64-syscall-table.md#5-文件系统挂载与结构)

## 1. 概述

`fsmount` 把 [`fsconfig`](./fsconfig.md) 已创建好的超级块转换成一个**挂载实例**，并以 `O_PATH` 文件描述符的形式返回。此时挂载尚未接入任何可见目录树，需要再用 [`move_mount`](./move_mount.md) 安装到目标位置。

- ARM64 编号 **432**（`include/uapi/asm-generic/unistd.h:773`）。
- 输入 fd 必须处于 `FS_CONTEXT_AWAITING_MOUNT` 阶段，即已成功执行 `FSCONFIG_CMD_CREATE`。
- 可通过 `attr_flags` 一次性设置只读、nosuid、nodev、noexec、atime 策略、nosymfollow 等挂载属性。
- `FSMOUNT_NAMESPACE` 会新建一个只含该挂载的挂载命名空间，返回命名空间 fd（而不是普通挂载 fd）。
- 返回的挂载 fd 带 `FMODE_NEED_UNMOUNT`，`close()` 时若仍未挂载会自动销毁。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE3(fsmount, int, fs_fd, unsigned int, flags,
		unsigned int, attr_flags)
{
	struct path new_path __free(path_put) = {};
	struct mnt_namespace *ns;
	struct fs_context *fc;
	struct vfsmount *new_mnt;
	struct mount *mnt;
	unsigned int mnt_flags = 0;
	long ret;

	if ((flags & ~(FSMOUNT_CLOEXEC | FSMOUNT_NAMESPACE)) != 0)
		return -EINVAL;

	if ((flags & FSMOUNT_NAMESPACE) &&
	    !ns_capable(current_user_ns(), CAP_SYS_ADMIN))
		return -EPERM;

	if (!(flags & FSMOUNT_NAMESPACE) && !may_mount())
		return -EPERM;

	if (attr_flags & ~FSMOUNT_VALID_FLAGS)
		return -EINVAL;
	...
}
```

（`fs/namespace.c:4443`）

用户态原型：

```c
#define _GNU_SOURCE
#include <sys/syscall.h>
int fsmount(int fs_fd, unsigned int flags, unsigned int attr_flags);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `fs_fd` | `int` | `fsopen` + `fsconfig(CMD_CREATE)` 后的上下文 fd |
| `flags` | `unsigned int` | `FSMOUNT_CLOEXEC`、`FSMOUNT_NAMESPACE` |
| `attr_flags` | `unsigned int` | `MOUNT_ATTR_*` 子集 |

`flags`（`include/uapi/linux/mount.h:112`）：

| 标志 | 值 | 含义 |
| --- | --- | --- |
| `FSMOUNT_CLOEXEC` | 0x01 | 返回 fd 带 `FD_CLOEXEC` |
| `FSMOUNT_NAMESPACE` | 0x02 | 新建挂载命名空间并返回其 fd |

`attr_flags` 只接受 `FSMOUNT_VALID_FLAGS`（`fs/namespace.c:4409`）：

```c
#define FSMOUNT_VALID_FLAGS                                                    \
	(MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV |            \
	 MOUNT_ATTR_NOEXEC | MOUNT_ATTR__ATIME | MOUNT_ATTR_NODIRATIME |       \
	 MOUNT_ATTR_NOSYMFOLLOW)
```

其中 `MOUNT_ATTR__ATIME` 是枚举掩码，`attr_flags & MOUNT_ATTR__ATIME` 只能取 `MOUNT_ATTR_RELATIME`、`MOUNT_ATTR_NOATIME`、`MOUNT_ATTR_STRICTATIME` 之一。

## 3. 内核实现路径

```
fsmount(fs_fd, flags, attr_flags)            fs/namespace.c:4443
├─ flags 校验；NAMESPACE 需 CAP_SYS_ADMIN，普通模式需 may_mount()
├─ attr_flags & ~FSMOUNT_VALID_FLAGS → -EINVAL
├─ attr_flags_to_mnt_flags()                fs/namespace.c:4419
├─ atime → MNT_NOATIME / MNT_RELATIME
├─ CLASS(fd, f)(fs_fd)
│    ├─ fd_empty → -EBADF
│    └─ f_op != &fscontext_fops → -EINVAL
├─ fc = file->private_data
├─ 加 fc->uapi_mutex
├─ !fc->root → -EINVAL
├─ mount_too_revealing(sb, &mnt_flags) → -EPERM
├─ fc->phase != FS_CONTEXT_AWAITING_MOUNT → -EBUSY
├─ vfs_create_mount(fc)                     // 生成 struct vfsmount
├─ sb->s_flags & SB_NOUSER → -EINVAL
├─ new_mnt->mnt_flags = mnt_flags
├─ vfs_clean_context(fc)                    // 上下文转为可重配置状态
├─ FSMOUNT_NAMESPACE ?
│    └─ open_new_namespace(&new_path, MOUNT_COPY_NEW)
└─ 否则：
     ├─ alloc_mnt_ns(user_ns, true)
     ├─ ns->root = mnt; ns->nr_mounts = 1; mnt_add_to_ns(ns, mnt)
     ├─ dentry_open(&new_path, O_PATH, fc->cred)
     └─ f_mode |= FMODE_NEED_UNMOUNT
```

关键代码（`fs/namespace.c:4496`）：

```c
/* There must be a valid superblock or we can't mount it */
ret = -EINVAL;
if (!fc->root)
	return ret;

ret = -EPERM;
if (mount_too_revealing(fc->root->d_sb, &mnt_flags)) {
	errorfcp(fc, "VFS", "Mount too revealing");
	return ret;
}

ret = -EBUSY;
if (fc->phase != FS_CONTEXT_AWAITING_MOUNT)
	return ret;
...
new_mnt = vfs_create_mount(fc);
if (IS_ERR(new_mnt))
	return PTR_ERR(new_mnt);
if (new_mnt->mnt_sb->s_flags & SB_NOUSER) {
	mntput(new_mnt);
	return -EINVAL;
}
new_mnt->mnt_flags = mnt_flags;
```

### 3.1 mount_too_revealing 的作用

当调用者处于非初始 user namespace 时，挂载必须"不过度暴露"宿主文件系统。该函数检查 `SB_I_NODEV`、`SB_I_NOEXEC` 等 inode 标志与 `mnt_flags` 的组合，必要时要求 `MNT_NODEV`/`MNT_NOEXEC`，否则拒绝挂载。它是容器场景下的安全边界。

### 3.2 返回值 fd 的两种形态

- **普通模式**：匿名挂载命名空间中的单挂载，`dentry_open(O_PATH)` 得到 `struct file`，`f_path` 指向 `fc->root` + 新 `vfsmount`。该 fd 是 [`move_mount`](./move_mount.md) 的典型输入。
- **`FSMOUNT_NAMESPACE`**：`open_new_namespace()` 返回的是 **mount namespace fd**（nsfs 文件），可用于 `setns()` 进入新命名空间，而不是传给 `move_mount`。

`vfs_clean_context(fc)`（`fs/fs_context.c:535`）释放文件系统私有上下文、`sb_flags`、安全选项和 `source`，把 `purpose` 设为 `FS_CONTEXT_FOR_RECONFIGURE`、`phase` 设为 `FS_CONTEXT_AWAITING_RECONF`。之后仍可用同一 fd 做重配置，但不能再 `fsmount` 一次。

## 4. 核心数据结构

```c
/* include/linux/mount.h:58 */
struct vfsmount {
	struct dentry *mnt_root;	/* 挂载树根 */
	struct super_block *mnt_sb;
	int mnt_flags;			/* MNT_*，由 attr_flags 转换 */
	struct mnt_idmap *mnt_idmap;
};

/* fs/mount.h:46 */
struct mount {
	struct hlist_node mnt_hash;
	struct mount *mnt_parent;
	struct dentry *mnt_mountpoint;
	struct vfsmount mnt;
	...
	struct mnt_namespace *mnt_ns;
	struct mountpoint *mnt_mp;
	...
};
```

`attr_flags_to_mnt_flags()` 把 `MOUNT_ATTR_RDONLY`→`MNT_READONLY`、`MOUNT_ATTR_NOSUID`→`MNT_NOSUID`、`MOUNT_ATTR_NODEV`→`MNT_NODEV`、`MOUNT_ATTR_NOEXEC`→`MNT_NOEXEC`、`MOUNT_ATTR_NODIRATIME`→`MNT_NODIRATIME`、`MOUNT_ATTR_NOSYMFOLLOW`→`MNT_NOSYMFOLLOW`。`MOUNT_ATTR_IDMAP` 不在 `FSMOUNT_VALID_FLAGS` 中，idmapped mount 需在挂载后用 [`mount_setattr`](./mount_setattr.md) 设置。

## 5. 关键流程

```
fsmount(fsfd, FSMOUNT_CLOEXEC, MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOEXEC)
  │
  ├─ 校验 flags / attr_flags
  ├─ 转换 MNT_READONLY | MNT_NOEXEC
  ├─ 确认 fsfd 是 fscontext 且 fc->root 已存在
  ├─ mount_too_revealing：必要时要求 nodev/noexec
  ├─ phase == AWAITING_MOUNT
  ├─ vfs_create_mount：分配 vfsmount，引用 sb 与根 dentry
  ├─ 设置 mnt_flags
  ├─ vfs_clean_context
  └─ dentry_open(O_PATH) → 返回 fd
       └─ 该 fd 可传给 move_mount 安装
```

安装示例：

```c
int mfd = fsmount(fsfd, FSMOUNT_CLOEXEC, 0);
move_mount(mfd, "", AT_FDCWD, "/mnt", MOVE_MOUNT_F_EMPTY_PATH);
```

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `>= 0` | 挂载 fd（普通模式）或命名空间 fd（`FSMOUNT_NAMESPACE`） |
| `-EINVAL` | `flags`/`attr_flags` 非法；fd 不是 fscontext；`fc->root` 为空；`SB_NOUSER`；atime 组合非法 |
| `-EBADF` | `fs_fd` 无效 |
| `-EPERM` | 缺少 `CAP_SYS_ADMIN`；`mount_too_revealing()` 拒绝 |
| `-EBUSY` | 上下文阶段不是 `FS_CONTEXT_AWAITING_MOUNT`（例如尚未 CREATE） |
| `-ENOMEM` | `vfs_create_mount()`、`alloc_mnt_ns()`、`dentry_open()` 失败 |
| `-EMFILE` / `-ENFILE` | 文件描述符耗尽 |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	int fsfd = syscall(SYS_fsopen, "tmpfs", FSOPEN_CLOEXEC);
	if (fsfd < 0) { perror("fsopen"); return 1; }

	if (syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING,
		    "size", "1m", 0) < 0)
		perror("fsconfig size");
	if (syscall(SYS_fsconfig, fsfd, FSCONFIG_CMD_CREATE,
		    NULL, NULL, 0) < 0) {
		perror("fsconfig create");
		return 1;
	}

	/* 只读、不可执行地创建挂载实例 */
	int mfd = syscall(SYS_fsmount, fsfd, FSMOUNT_CLOEXEC,
			  MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOEXEC);
	if (mfd < 0) { perror("fsmount"); return 1; }

	if (syscall(SYS_move_mount, mfd, "", AT_FDCWD, "/tmp/demo",
		    MOVE_MOUNT_F_EMPTY_PATH) < 0)
		perror("move_mount");

	close(mfd);
	close(fsfd);
	return 0;
}
```

创建独立挂载命名空间：

```c
int nsfd = syscall(SYS_fsmount, fsfd, FSMOUNT_CLOEXEC | FSMOUNT_NAMESPACE, 0);
if (nsfd >= 0)
	setns(nsfd, CLONE_NEWNS);
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/namespace.c` | `FSMOUNT_VALID_FLAGS`（4409）、`attr_flags_to_mnt_flags()`（4419）、`SYSCALL_DEFINE3(fsmount)`（4443）、`mount_too_revealing()`（6423）、`open_new_namespace()`（3184） |
| `fs/fs_context.c` | `vfs_clean_context()`（535）、`put_fs_context()`（486） |
| `fs/fsopen.c` | `fscontext_fops`（84）、`fscontext_create_fd()`（92） |
| `fs/pnode.c` | 超级块挂载树 / 传播辅助逻辑 |
| `include/linux/mount.h` | `struct vfsmount`（58） |
| `include/uapi/linux/mount.h` | `FSMOUNT_*`（112）、`MOUNT_ATTR_*`（118） |
| `include/uapi/asm-generic/unistd.h` | `__NR_fsmount`（773） |
