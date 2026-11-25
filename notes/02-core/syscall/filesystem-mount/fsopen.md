# fsopen 系统调用

> 所属分类：文件系统挂载与结构 | [ARM64 系统调用表](../arm64-syscall-table.md#5-文件系统挂载与结构)

## 1. 概述

`fsopen` 按文件系统类型名创建一个**文件系统上下文**（`struct fs_context`），并返回一个匿名 inode 文件描述符。该 fd 不是挂载点，而是后续 [`fsconfig`](./fsconfig.md)、[`fsmount`](./fsmount.md) 操作的句柄：

```
fsopen() → fsconfig()... → fsconfig(FSCONFIG_CMD_CREATE) → fsmount() → move_mount()
```

- ARM64 编号 **430**（`include/uapi/asm-generic/unistd.h:769`）。
- 取代 `mount(2)` 的"一次性传入全部选项"模型，允许逐项校验参数，并把错误信息通过 `read()` 读回。
- 权限：需要当前挂载命名空间所属 user namespace 的 `CAP_SYS_ADMIN`（`may_mount()`，`fs/namespace.c:2008`）。
- 关闭 fd 会释放整个上下文；若超级块已创建但未挂载，也会一并释放。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE2(fsopen, const char __user *, _fs_name, unsigned int, flags)
{
	struct file_system_type *fs_type;
	struct fs_context *fc;
	const char *fs_name;
	int ret;

	if (!may_mount())
		return -EPERM;

	if (flags & ~FSOPEN_CLOEXEC)
		return -EINVAL;

	fs_name = strndup_user(_fs_name, PAGE_SIZE);
	if (IS_ERR(fs_name))
		return PTR_ERR(fs_name);

	fs_type = get_fs_type(fs_name);
	kfree(fs_name);
	if (!fs_type)
		return -ENODEV;

	fc = fs_context_for_mount(fs_type, 0);
	put_filesystem(fs_type);
	if (IS_ERR(fc))
		return PTR_ERR(fc);

	fc->phase = FS_CONTEXT_CREATE_PARAMS;

	ret = fscontext_alloc_log(fc);
	if (ret < 0)
		goto err_fc;

	return fscontext_create_fd(fc, flags & FSOPEN_CLOEXEC ? O_CLOEXEC : 0);

err_fc:
	put_fs_context(fc);
	return ret;
}
```

（`fs/fsopen.c:120`）

用户态原型：

```c
#define _GNU_SOURCE
#include <sys/syscall.h>
int fsopen(const char *fs_name, unsigned int flags);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `fs_name` | `const char *` | 已注册的文件系统类型名，如 `"ext4"`、`"tmpfs"`、`"xfs"` |
| `flags` | `unsigned int` | 仅允许 `FSOPEN_CLOEXEC`（0x01） |

## 3. 内核实现路径

```
fsopen(fs_name, flags)                       fs/fsopen.c:120
├─ may_mount()                               fs/namespace.c:2008
├─ flags & ~FSOPEN_CLOEXEC → -EINVAL
├─ strndup_user(fs_name, PAGE_SIZE)          // 最长一页
├─ get_fs_type(fs_name) → -ENODEV
├─ fs_context_for_mount(fs_type, 0)          fs/fs_context.c:306
│    └─ alloc_fs_context(..., FS_CONTEXT_FOR_MOUNT)
│         ├─ kzalloc struct fs_context
│         ├─ get_filesystem / get_current_cred / get_net
│         ├─ fc->user_ns = cred->user_ns
│         └─ fs_type->init_fs_context(fc)
├─ fc->phase = FS_CONTEXT_CREATE_PARAMS
├─ fscontext_alloc_log(fc)                   fs/fsopen.c:103
│    └─ 分配 fc_log，容量 8 条消息
└─ fscontext_create_fd(fc, o_flags)          fs/fsopen.c:92
     └─ anon_inode_getfd("[fscontext]", &fscontext_fops, fc, O_RDWR | o_flags)
```

`fscontext_create_fd()` 失败时会调用 `put_fs_context(fc)`，因此错误路径不会泄漏上下文。

### 3.1 返回的 fd 能做什么

`fscontext_fops` 只实现两个方法（`fs/fsopen.c:84`）：

```c
const struct file_operations fscontext_fops = {
	.read		= fscontext_read,
	.release	= fscontext_release,
};
```

- `read()`：从上下文日志环形缓冲区取一条消息。每次 `read()` 返回一条，缓冲区空时返回 `-ENODATA`，用户缓冲区小于消息长度返回 `-EMSGSIZE`。消息由 `logfc()` 以 `"<level> <prefix>: <text>\n"` 形式写入。
- `release()`：`put_fs_context(fc)` 释放上下文。
- 其他操作（`write`、`ioctl` 等）没有实现，返回 `-EINVAL`。
- `fsconfig` / `fsmount` 会检查 `file->f_op == &fscontext_fops`，因此不能把普通文件 fd 冒充上下文。

### 3.2 与 fs_context_for_mount 的关系

`fs_context_for_mount()` 最终调用 `alloc_fs_context()`（`fs/fs_context.c:258`），为 `FS_CONTEXT_FOR_MOUNT` 设置：

- `fc->fs_type = get_filesystem(fs_type)`
- `fc->cred = get_current_cred()`
- `fc->net_ns = get_net(current->nsproxy->net_ns)`
- `fc->user_ns = get_user_ns(fc->cred->user_ns)`
- `fc->log.prefix = fs_type->name`
- `mutex_init(&fc->uapi_mutex)`

随后调用文件系统的 `init_fs_context(fc)`，由它填充 `fc->ops` 与 `fc->fs_private`。

## 4. 核心数据结构

```c
/* include/linux/fs_context.h:90 */
struct fs_context {
	const struct fs_context_operations *ops;
	struct mutex		uapi_mutex;	/* 串行化 fsconfig 调用 */
	struct file_system_type	*fs_type;
	void			*fs_private;	/* 文件系统私有上下文 */
	void			*sget_key;
	struct dentry		*root;		/* get_tree 后的根 dentry */
	struct user_namespace	*user_ns;
	struct net		*net_ns;
	const struct cred	*cred;
	struct p_log		log;
	const char		*source;
	void			*security;	/* LSM 选项 */
	void			*s_fs_info;
	unsigned int		sb_flags;
	unsigned int		sb_flags_mask;
	unsigned int		s_iflags;
	enum fs_context_purpose	purpose:8;
	enum fs_context_phase	phase:8;
	bool			need_free:1;
	bool			global:1;
	bool			oldapi:1;
	bool			exclusive:1;
};

/* include/linux/fs_context.h:115 */
struct fs_context_operations {
	void (*free)(struct fs_context *fc);
	int  (*dup)(struct fs_context *fc, struct fs_context *src_fc);
	int  (*parse_param)(struct fs_context *fc, struct fs_parameter *param);
	int  (*parse_monolithic)(struct fs_context *fc, void *data);
	int  (*get_tree)(struct fs_context *fc);
	int  (*reconfigure)(struct fs_context *fc);
};
```

阶段枚举（`include/linux/fs_context.h:38`）：

| 阶段 | 含义 |
| --- | --- |
| `FS_CONTEXT_CREATE_PARAMS` | `fsopen` 后，可设置创建参数 |
| `FS_CONTEXT_CREATING` | 正在 `vfs_get_tree()` |
| `FS_CONTEXT_AWAITING_MOUNT` | 超级块已建好，等待 `fsmount()` |
| `FS_CONTEXT_AWAITING_RECONF` | 挂载/重配置完成，等待下一次重配置初始化 |
| `FS_CONTEXT_RECONF_PARAMS` | `fspick` 后，可设置重配置参数 |
| `FS_CONTEXT_RECONFIGURING` | 正在 `reconfigure_super()` |
| `FS_CONTEXT_FAILED` | 迁移失败，上下文不可继续使用 |

## 5. 关键流程

```
fsopen("tmpfs", FSOPEN_CLOEXEC)
  │
  ├─ 权限检查 may_mount
  ├─ 按名字查找 file_system_type
  ├─ 分配并初始化 fs_context
  │    └─ tmpfs 的 init_fs_context 注册 parse_param/get_tree
  ├─ phase = CREATE_PARAMS
  ├─ 分配日志缓冲区
  └─ anon_inode_getfd 返回 fd
       │
       ├─ read(fd, buf, n) 读回错误/警告（可选）
       ├─ fsconfig(fd, ...) 逐项设置参数
       ├─ fsconfig(fd, FSCONFIG_CMD_CREATE, NULL, NULL, 0)
       └─ fsmount(fd, ...) 生成挂载 fd
```

若 `fsconfig` 阶段某参数非法，错误文本会进入 `fc->log`，但没有设置 `O_NONBLOCK` 的 `read()` 在无消息时返回 `-ENODATA`，程序应据此判断而不是阻塞等待。

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `>= 0` | 上下文文件描述符 |
| `-EPERM` | 缺少 `CAP_SYS_ADMIN` |
| `-EINVAL` | `flags` 含未知位 |
| `-ENODEV` | 文件系统类型未注册，或对应模块未加载 |
| `-ENOMEM` | `strndup_user()`、`kzalloc_obj()`、`fc_log` 分配失败 |
| `-EFAULT` | `fs_name` 指向非法用户地址 |
| `-EMFILE` / `-ENFILE` | 进程或系统文件描述符耗尽（`anon_inode_getfd()`） |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <errno.h>
#include <linux/mount.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	int fd = syscall(SYS_fsopen, "tmpfs", FSOPEN_CLOEXEC);
	if (fd < 0) { perror("fsopen"); return 1; }

	char log[256];
	ssize_t n = read(fd, log, sizeof(log));
	if (n > 0)
		printf("fs context log: %.*s", (int)n, log);
	else if (errno != ENODATA)
		perror("read log");

	/* 后续：fsconfig 设置 size，再 FSCONFIG_CMD_CREATE，最后 fsmount */
	if (syscall(SYS_fsconfig, fd, FSCONFIG_SET_STRING, "size", "1m", 0) < 0)
		perror("fsconfig");

	close(fd);	/* 未创建超级块时直接释放上下文 */
	return 0;
}
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/fsopen.c` | `fscontext_read()`（47）、`fscontext_release()`（73）、`fscontext_fops`（84）、`fscontext_create_fd()`（92）、`fscontext_alloc_log()`（103）、`SYSCALL_DEFINE2(fsopen)`（120） |
| `fs/fs_context.c` | `alloc_fs_context()`（258）、`fs_context_for_mount()`（306）、`put_fs_context()`（486）、`logfc()` |
| `include/linux/fs_context.h` | `struct fs_context`（90）、`struct fs_context_operations`（115）、阶段枚举（38） |
| `include/uapi/linux/mount.h` | `FSOPEN_CLOEXEC`（84） |
| `fs/namespace.c` | `may_mount()`（2008） |
| `include/uapi/asm-generic/unistd.h` | `__NR_fsopen`（769） |
