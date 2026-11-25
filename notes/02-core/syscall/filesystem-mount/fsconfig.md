# fsconfig 系统调用

> 所属分类：文件系统挂载与结构 | [ARM64 系统调用表](../arm64-syscall-table.md#5-文件系统挂载与结构)

## 1. 概述

`fsconfig` 在 [`fsopen`](./fsopen.md) 或 [`fspick`](./fspick.md) 返回的文件系统上下文上设置参数，或触发"创建超级块"/"重配置超级块"动作。

两类用途：

- **创建**：`fsopen()` → 若干 `fsconfig(FSCONFIG_SET_*)` → `fsconfig(FSCONFIG_CMD_CREATE)` → [`fsmount`](./fsmount.md)。
- **重配置**：`fspick()` → 若干 `fsconfig(FSCONFIG_SET_*)` → `fsconfig(FSCONFIG_CMD_RECONFIGURE)`，等价于 `mount(MS_REMOUNT)`。

- ARM64 编号 **431**（`include/uapi/asm-generic/unistd.h:771`）。
- 每次调用都在 `fc->uapi_mutex` 下串行执行，参数立即被文件系统解析，错误可即时返回或写入日志。
- 参数类型由 `cmd` 区分，值可以不存在、字符串、二进制块、路径或 fd。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE5(fsconfig,
		int, fd,
		unsigned int, cmd,
		const char __user *, _key,
		const void __user *, _value,
		int, aux)
```

（`fs/fsopen.c:350`）

用户态原型：

```c
#define _GNU_SOURCE
#include <sys/syscall.h>
int fsconfig(int fd, unsigned int cmd, const char *key,
             const void *value, int aux);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `fd` | `int` | `fsopen`/`fspick` 返回的上下文 fd |
| `cmd` | `unsigned int` | `FSCONFIG_*`，见下表 |
| `_key` | `const char *` | 参数名，最长 256 字节；动作命令必须为 `NULL` |
| `_value` | `const void *` | 参数值；按 `cmd` 解释为字符串、二进制块或路径 |
| `aux` | `int` | 二进制长度、路径的 dirfd、或 fd 参数 |

`cmd` 取值（`include/uapi/linux/mount.h:97`）：

| 值 | 名称 | `_key` | `_value` | `aux` |
| --- | --- | --- | --- | --- |
| 0 | `FSCONFIG_SET_FLAG` | 必填 | 必须 `NULL` | 必须 0 |
| 1 | `FSCONFIG_SET_STRING` | 必填 | 非空字符串 | 必须 0 |
| 2 | `FSCONFIG_SET_BINARY` | 必填 | 二进制块 | 长度，1..1 MiB |
| 3 | `FSCONFIG_SET_PATH` | 必填 | 路径 | `AT_FDCWD` 或有效 dirfd |
| 4 | `FSCONFIG_SET_PATH_EMPTY` | 必填 | 路径（可为空串） | 同上 |
| 5 | `FSCONFIG_SET_FD` | 必填 | 必须 `NULL` | 有效 fd |
| 6 | `FSCONFIG_CMD_CREATE` | 必须 `NULL` | 必须 `NULL` | 必须 0 |
| 7 | `FSCONFIG_CMD_RECONFIGURE` | 必须 `NULL` | 必须 `NULL` | 必须 0 |
| 8 | `FSCONFIG_CMD_CREATE_EXCL` | 必须 `NULL` | 必须 `NULL` | 必须 0 |

`FSCONFIG_SET_FLAG` 的参数名可加 `no` 前缀表示取反，具体由文件系统的 `parse_param` 解释。

## 3. 内核实现路径

```
fsconfig(fd, cmd, key, value, aux)           fs/fsopen.c:350
├─ fd < 0 → -EINVAL
├─ 按 cmd 校验 key/value/aux 组合         // 350-398
├─ CLASS(fd, f)(fd)
├─ fd_empty → -EBADF
├─ f_op != &fscontext_fops → -EINVAL
├─ fc = file->private_data
├─ _key → strndup_user(_key, 256)
├─ 构造 struct fs_parameter
│    ├─ STRING → strndup_user(_value, 256)  // 值也限 256 字节
│    ├─ BINARY → memdup_user_nul(_value, aux)
│    ├─ PATH/PATH_EMPTY → getname_flags(_value, LOOKUP_EMPTY?)
│    └─ FD → fget_raw(aux)
├─ mutex_lock_interruptible(&fc->uapi_mutex)
├─ vfs_fsconfig_locked(fc, cmd, &param)    fs/fsopen.c:283
│    ├─ finish_clean_context(fc)           // AWAITING_RECONF 时重新 init
│    ├─ CMD_CREATE → vfs_cmd_create(fc, false)
│    ├─ CMD_CREATE_EXCL → vfs_cmd_create(fc, true)
│    ├─ CMD_RECONFIGURE → vfs_cmd_reconfigure(fc)
│    └─ 参数命令：phase 必须为 CREATE_PARAMS 或 RECONF_PARAMS
│         └─ vfs_parse_fs_param(fc, param) fs/fs_context.c:113
└─ 释放临时 key/string/blob/name/file（可能已被文件系统"偷走"）
```

`vfs_fsconfig_locked()` 的阶段检查：

```c
switch (cmd) {
case FSCONFIG_CMD_CREATE:
	return vfs_cmd_create(fc, false);
case FSCONFIG_CMD_CREATE_EXCL:
	return vfs_cmd_create(fc, true);
case FSCONFIG_CMD_RECONFIGURE:
	return vfs_cmd_reconfigure(fc);
default:
	if (fc->phase != FS_CONTEXT_CREATE_PARAMS &&
	    fc->phase != FS_CONTEXT_RECONF_PARAMS)
		return -EBUSY;

	return vfs_parse_fs_param(fc, param);
}
```

### 3.1 创建超级块

`vfs_cmd_create()`（`fs/fsopen.c:217`）：

1. `fc->phase != FS_CONTEXT_CREATE_PARAMS` → `-EBUSY`（防止重复 CREATE）。
2. `mount_capable(fc)` 检查 `CAP_SYS_ADMIN`；对 `FS_USERNS_MOUNT` 文件系统检查 `fc->user_ns`。
3. `fc->phase = FS_CONTEXT_CREATING`，记录 `exclusive`。
4. `vfs_get_tree(fc)`（`fs/super.c:1689`）调用 `fc->ops->get_tree()`，让文件系统在 `fc->root` 中放入根 dentry。
5. `security_sb_kern_mount(sb)` 做 LSM 检查。
6. `up_write(&sb->s_umount)`，`fc->phase = FS_CONTEXT_AWAITING_MOUNT`。

`exclusive == true` 时文件系统必须创建全新超级块，不能复用已有实例；用于避免共享同一 sb 带来的选项污染。

### 3.2 重配置超级块

`vfs_cmd_reconfigure()`（`fs/fsopen.c:251`）：

1. 仅接受 `FS_CONTEXT_RECONF_PARAMS`。
2. 检查 `ns_capable(sb->s_user_ns, CAP_SYS_ADMIN)`。
3. `down_write(&sb->s_umount)` 后调用 `reconfigure_super(fc)`，把参数交给文件系统的 `->reconfigure`。
4. 成功后 `vfs_clean_context(fc)`，把上下文复位为可再次重配置的状态。

参数命令在 `vfs_parse_fs_param()` 中的顺序是：先尝试公共超级块标志（如 `ro`/`rw`、`sync`），再交给 LSM，再交给 `fc->ops->parse_param`，最后尝试 `source`，都不匹配则返回 `-EINVAL` 并记录 `"Unknown parameter"`。

## 4. 核心数据结构

```c
/* include/linux/fs_context.h:63 */
struct fs_parameter {
	const char		*key;
	enum fs_value_type	type:8;		/* flag/string/blob/filename/file */
	union {
		char		*string;
		void		*blob;
		struct filename	*name;
		struct file	*file;
	};
	size_t	size;
	int	dirfd;
};

/* include/linux/fs_context.h:51 */
enum fs_value_type {
	fs_value_is_undefined,
	fs_value_is_flag,
	fs_value_is_string,
	fs_value_is_blob,
	fs_value_is_filename,
	fs_value_is_file,
};
```

文件系统通过 `struct fs_parameter` 的 `key`/`type` 分派；路径类型还带 `name` 与 `dirfd`，fd 类型带已 `fget_raw()` 的 `file`。`vfs_parse_fs_param()` 允许 LSM 或文件系统"接管"值的所有权（把指针置 `NULL`），因此 `fsconfig` 在调用后逐个判断指针再释放。

## 5. 关键流程

```
fsconfig(fd, FSCONFIG_SET_STRING, "source", "/dev/sda1", 0)
  │
  ├─ 校验 cmd 组合
  ├─ 确认 fd 是 fscontext
  ├─ 复制 key/value 到内核
  ├─ 加 fc->uapi_mutex
  ├─ phase 必须是 CREATE_PARAMS / RECONF_PARAMS
  ├─ vfs_parse_fs_param
  │    ├─ vfs_parse_sb_flag：ro/rw/sync/...
  │    ├─ security_fs_context_parse_param：LSM（如 SELinux context）
  │    ├─ fc->ops->parse_param：文件系统解析
  │    └─ source 兜底或 -EINVAL
  └─ 解锁并释放临时内存

fsconfig(fd, FSCONFIG_CMD_CREATE, NULL, NULL, 0)
  │
  ├─ phase == CREATE_PARAMS ?
  ├─ mount_capable
  ├─ phase = CREATING
  ├─ vfs_get_tree → 文件系统 get_tree（如 ext4_get_tree）
  ├─ security_sb_kern_mount
  └─ phase = AWAITING_MOUNT
```

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `0` | 参数已接受，或创建/重配置成功 |
| `-EINVAL` | `fd < 0`；`cmd` 的 key/value/aux 组合非法；上下文 fd 类型不对；未知参数 |
| `-EBADF` | `fd` 无效；`FSCONFIG_SET_FD` 的 `aux` 无效 |
| `-EOPNOTSUPP` | 未知 `cmd`（default 分支） |
| `-EBUSY` | 阶段不匹配（如重复 CREATE、创建中再次设置参数） |
| `-EPERM` | `mount_capable()` 或 `ns_capable()` 失败；LSM 拒绝 |
| `-ENOMEM` | 复制 key/value/blob、分配超级块失败 |
| `-EFAULT` | 用户指针非法 |
| `-ENODEV` | 文件系统在 `get_tree` 中找不到设备 |
| `-ENOSPC` / `-EIO` / 其他 | 由具体文件系统的 `parse_param` / `get_tree` / `reconfigure` 返回 |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	int fd = syscall(SYS_fsopen, "tmpfs", FSOPEN_CLOEXEC);
	if (fd < 0) { perror("fsopen"); return 1; }

	if (syscall(SYS_fsconfig, fd, FSCONFIG_SET_STRING,
		    "size", "4096k", 0) < 0)
		perror("fsconfig size");

	if (syscall(SYS_fsconfig, fd, FSCONFIG_SET_FLAG,
		    "nosuid", NULL, 0) < 0)
		perror("fsconfig nosuid");

	if (syscall(SYS_fsconfig, fd, FSCONFIG_CMD_CREATE,
		    NULL, NULL, 0) < 0) {
		perror("fsconfig create");
		return 1;
	}

	int mfd = syscall(SYS_fsmount, fd, FSMOUNT_CLOEXEC, 0);
	if (mfd < 0) { perror("fsmount"); return 1; }

	/* move_mount(mfd, "", AT_FDCWD, "/mnt", MOVE_MOUNT_F_EMPTY_PATH) */
	close(mfd);
	close(fd);
	return 0;
}
```

重配置示例（等价 `mount -o remount,ro`）：

```c
int fd = syscall(SYS_fspick, AT_FDCWD, "/mnt", FSPICK_CLOEXEC);
syscall(SYS_fsconfig, fd, FSCONFIG_SET_FLAG, "ro", NULL, 0);
syscall(SYS_fsconfig, fd, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0);
close(fd);
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/fsopen.c` | `SYSCALL_DEFINE5(fsconfig)`（350）、`vfs_cmd_create()`（217）、`vfs_cmd_reconfigure()`（251）、`vfs_fsconfig_locked()`（283） |
| `fs/fs_context.c` | `vfs_parse_fs_param()`（113）、`vfs_parse_fs_param_source()`（80）、`vfs_clean_context()`（535）、`finish_clean_context()`（552） |
| `fs/super.c` | `vfs_get_tree()`（1689）、`mount_capable()`（695）、`reconfigure_super()` |
| `include/linux/fs_context.h` | `struct fs_parameter`（63）、`enum fs_value_type`（51）、阶段枚举 |
| `include/uapi/linux/mount.h` | `enum fsconfig_command`（97）、`FSCONFIG_*` |
| `include/uapi/asm-generic/unistd.h` | `__NR_fsconfig`（771） |
