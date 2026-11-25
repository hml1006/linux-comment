# fspick 系统调用

> 所属分类：文件系统挂载与结构 | [ARM64 系统调用表](../arm64-syscall-table.md#5-文件系统挂载与结构)

## 1. 概述

`fspick` 选中一个**已挂载的超级块**，为其创建重配置上下文 fd。之后通过 [`fsconfig`](./fsconfig.md) 设置参数并执行 `FSCONFIG_CMD_RECONFIGURE`，等价于 `mount(MS_REMOUNT)`，但参数是逐项传入并可即时校验。

- ARM64 编号 **433**（`include/uapi/asm-generic/unistd.h:775`）。
- 路径必须正好指向某个挂载的根（`target.mnt->mnt_root == target.dentry`），不能是挂载内部的子目录。
- 上下文 fd 与 [`fsopen`](./fsopen.md) 返回的 fd 类型相同，都使用 `fscontext_fops`，但阶段为 `FS_CONTEXT_RECONF_PARAMS`。
- 与 `fsopen` 的区别：`fsopen` 面向"创建新超级块"，`fspick` 面向"修改已有超级块"。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE3(fspick, int, dfd, const char __user *, path, unsigned int, flags)
{
	struct fs_context *fc;
	struct path target;
	unsigned int lookup_flags;
	int ret;

	if (!may_mount())
		return -EPERM;

	if ((flags & ~(FSPICK_CLOEXEC |
		       FSPICK_SYMLINK_NOFOLLOW |
		       FSPICK_NO_AUTOMOUNT |
		       FSPICK_EMPTY_PATH)) != 0)
		return -EINVAL;

	lookup_flags = LOOKUP_FOLLOW | LOOKUP_AUTOMOUNT;
	if (flags & FSPICK_SYMLINK_NOFOLLOW)
		lookup_flags &= ~LOOKUP_FOLLOW;
	if (flags & FSPICK_NO_AUTOMOUNT)
		lookup_flags &= ~LOOKUP_AUTOMOUNT;
	...
	ret = filename_lookup(dfd, filename, lookup_flags, &target, NULL);
	...
	ret = -EINVAL;
	if (target.mnt->mnt_root != target.dentry)
		goto err_path;

	fc = fs_context_for_reconfigure(target.dentry, 0, 0);
	...
	fc->phase = FS_CONTEXT_RECONF_PARAMS;
	...
	return fscontext_create_fd(fc, flags & FSPICK_CLOEXEC ? O_CLOEXEC : 0);
}
```

（`fs/fsopen.c:163`）

用户态原型：

```c
#define _GNU_SOURCE
#include <sys/syscall.h>
int fspick(int dfd, const char *path, unsigned int flags);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `dfd` | `int` | 目录 fd 或 `AT_FDCWD` |
| `path` | `const char *` | 挂载点路径；必须解析到挂载根 |
| `flags` | `unsigned int` | `FSPICK_*` |

`include/uapi/linux/mount.h:89`：

| 标志 | 值 | 含义 |
| --- | --- | --- |
| `FSPICK_CLOEXEC` | 0x01 | 返回 fd 带 `FD_CLOEXEC` |
| `FSPICK_SYMLINK_NOFOLLOW` | 0x02 | 不跟随路径末尾的符号链接 |
| `FSPICK_NO_AUTOMOUNT` | 0x04 | 不触发自动挂载 |
| `FSPICK_EMPTY_PATH` | 0x08 | 允许空路径，直接操作 `dfd` |

## 3. 内核实现路径

```
fspick(dfd, path, flags)                     fs/fsopen.c:163
├─ may_mount() → -EPERM                      fs/namespace.c:2008
├─ flags & ~(FSPICK_*) → -EINVAL
├─ lookup_flags = LOOKUP_FOLLOW | LOOKUP_AUTOMOUNT
│    ├─ FSPICK_SYMLINK_NOFOLLOW → 清除 LOOKUP_FOLLOW
│    └─ FSPICK_NO_AUTOMOUNT     → 清除 LOOKUP_AUTOMOUNT
├─ CLASS(filename_flags, filename)(path,
│        FSPICK_EMPTY_PATH ? LOOKUP_EMPTY : 0)
├─ filename_lookup(dfd, filename, lookup_flags, &target, NULL)
├─ target.mnt->mnt_root != target.dentry → -EINVAL
├─ fs_context_for_reconfigure(target.dentry, 0, 0)
│    └─ alloc_fs_context(..., FS_CONTEXT_FOR_RECONFIGURE)
│         ├─ atomic_inc(&sb->s_active)
│         ├─ fc->user_ns = sb->s_user_ns
│         ├─ fc->root = dget(target.dentry)
│         └─ sb->s_type->init_fs_context(fc)
├─ fc->phase = FS_CONTEXT_RECONF_PARAMS
├─ fscontext_alloc_log(fc)
├─ path_put(&target)
└─ fscontext_create_fd(fc, CLOEXEC?)
```

### 3.1 为什么必须是挂载根

`fs_context_for_reconfigure()`（`fs/fs_context.c:314`）用 `target.dentry` 作为 `fc->root`，并增加超级块的 `s_active`。如果路径只是挂载内的子目录，`fc->root` 会指向子目录而非挂载根，后续 `reconfigure_super()` 与挂载属性修改会产生歧义，因此 `fspick` 提前拒绝：

```c
ret = -EINVAL;
if (target.mnt->mnt_root != target.dentry)
	goto err_path;
```

### 3.2 重配置上下文的生命周期

- `fc->root` 持有 `dentry` 引用与超级块 active 引用。
- `fsconfig(..., FSCONFIG_CMD_RECONFIGURE, ...)` 成功后调用 `vfs_clean_context()`，stage 变为 `FS_CONTEXT_AWAITING_RECONF`，可再次设置参数并重配置。
- `close(fd)` 触发 `fscontext_release()` → `put_fs_context()`，释放 `fc->root` 并 `deactivate_super()`。

## 4. 核心数据结构

```c
/* include/linux/fs_context.h:90（节选） */
struct fs_context {
	const struct fs_context_operations *ops;
	struct mutex		uapi_mutex;
	struct file_system_type	*fs_type;
	void			*fs_private;
	struct dentry		*root;		/* fspick 时指向挂载根 */
	struct user_namespace	*user_ns;	/* 拷贝自 sb->s_user_ns */
	struct net		*net_ns;
	const struct cred	*cred;
	struct p_log		log;
	...
	enum fs_context_purpose	purpose:8;	/* FS_CONTEXT_FOR_RECONFIGURE */
	enum fs_context_phase	phase:8;	/* RECONF_PARAMS / AWAITING_RECONF */
	bool			need_free:1;
	...
};
```

`fs_context_for_reconfigure()` 与 `fs_context_for_mount()` 共用 `alloc_fs_context()`（`fs/fs_context.c:258`），差异在 `purpose`：

| purpose | 初始化行为 |
| --- | --- |
| `FS_CONTEXT_FOR_MOUNT` | `fc->root = NULL`，`user_ns = cred->user_ns` |
| `FS_CONTEXT_FOR_RECONFIGURE` | `atomic_inc(&sb->s_active)`，`fc->root = dget(dentry)`，`user_ns = sb->s_user_ns` |
| `FS_CONTEXT_FOR_SUBMOUNT` | `user_ns = reference->d_sb->s_user_ns`，用于自动挂载 |

## 5. 关键流程

```
fspick(AT_FDCWD, "/mnt", FSPICK_CLOEXEC)
  │
  ├─ may_mount：CAP_SYS_ADMIN
  ├─ 校验 flags
  ├─ 解析路径，得到 target = (vfsmount, dentry)
  ├─ 检查 target 是否为挂载根
  ├─ fs_context_for_reconfigure
  │    ├─ 增加 sb->s_active
  │    ├─ 持有根 dentry
  │    └─ 调用文件系统 init_fs_context
  ├─ phase = RECONF_PARAMS
  ├─ 分配日志
  └─ 返回 fscontext fd
       │
       ├─ fsconfig(fd, FSCONFIG_SET_FLAG, "ro", NULL, 0)
       ├─ fsconfig(fd, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0)
       └─ close(fd)
```

若 `FSCONFIG_CMD_RECONFIGURE` 失败，`fc->phase` 被置为 `FS_CONTEXT_FAILED`，该上下文不能再使用。

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `>= 0` | 重配置上下文 fd |
| `-EPERM` | 缺少 `CAP_SYS_ADMIN` |
| `-EINVAL` | `flags` 含未知位；路径不是挂载根；空路径但未给 `FSPICK_EMPTY_PATH` |
| `-ENOENT` | 路径不存在 |
| `-ENOTDIR` | 路径中间分量不是目录 |
| `-ELOOP` | 符号链接层数过多 |
| `-ENOMEM` | 分配 `fs_context`、日志或 dentry 引用失败 |
| `-EMFILE` / `-ENFILE` | 文件描述符耗尽 |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/mount.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	int fd = syscall(SYS_fspick, AT_FDCWD, "/mnt", FSPICK_CLOEXEC);
	if (fd < 0) { perror("fspick"); return 1; }

	/* 等价 mount -o remount,ro /mnt */
	if (syscall(SYS_fsconfig, fd, FSCONFIG_SET_FLAG,
		    "ro", NULL, 0) < 0)
		perror("fsconfig ro");

	if (syscall(SYS_fsconfig, fd, FSCONFIG_CMD_RECONFIGURE,
		    NULL, NULL, 0) < 0)
		perror("fsconfig reconfigure");
	else
		printf("/mnt remounted read-only\n");

	close(fd);
	return 0;
}
```

通过 fd 选中挂载点（`FSPICK_EMPTY_PATH`）：

```c
int mntfd = open("/mnt", O_PATH | O_DIRECTORY);
int cfd = syscall(SYS_fspick, mntfd, "", FSPICK_CLOEXEC | FSPICK_EMPTY_PATH);
```

注意 `dfd` 指向的 fd 必须解析到挂载根，否则仍返回 `-EINVAL`。

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/fsopen.c` | `SYSCALL_DEFINE3(fspick)`（163）、`fscontext_fops`（84）、`fscontext_create_fd()`（92）、`vfs_cmd_reconfigure()`（251） |
| `fs/fs_context.c` | `alloc_fs_context()`（258）、`fs_context_for_reconfigure()`（314）、`put_fs_context()`（486） |
| `fs/namespace.c` | `may_mount()`（2008）、`reconfigure_super()` |
| `include/linux/fs_context.h` | `struct fs_context`（90）、`enum fs_context_purpose`（29）、阶段枚举（38） |
| `include/uapi/linux/mount.h` | `FSPICK_*`（89-92） |
| `include/uapi/asm-generic/unistd.h` | `__NR_fspick`（775） |
