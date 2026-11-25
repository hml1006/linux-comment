# utimensat 系统调用

> 所属分类：文件元数据与属性 | [ARM64 系统调用表](../arm64-syscall-table.md#3-文件元数据与属性)

## 1. 概述

`utimensat` 修改文件的访问时间（atime）和修改时间（mtime），支持纳秒精度、`UTIME_NOW`（取当前时间）、`UTIME_OMIT`（保持不变）两种特殊值，并可通过 `AT_SYMLINK_NOFOLLOW` 直接操作符号链接本身。

- ARM64 编号 **88**（`include/uapi/asm-generic/unistd.h:246`）。
- 同时支持"路径 + dirfd"和"fd 本身"两种目标形式。
- 是 `utime`、`utimes`、`futimesat` 的现代替代；旧接口在新架构上通常由 libc 包装成 `utimensat`。
- 真正落盘由文件系统的 `inode->i_op->setattr` 完成，ext4 对应 `ext4_setattr()`（`fs/ext4/inode.c:6223`）。
- 读取侧对应 `stat`/`statx` 的 `generic_fillattr()` 与 `vfs_getattr()`（`fs/stat.c:82` / `254`）。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE4(utimensat, int, dfd, const char __user *, filename,
		struct __kernel_timespec __user *, utimes, int, flags)
{
	struct timespec64 tstimes[2];

	if (utimes) {
		if ((get_timespec64(&tstimes[0], &utimes[0]) ||
			get_timespec64(&tstimes[1], &utimes[1])))
			return -EFAULT;

		/* Nothing to do, we must not even check the path.  */
		if (tstimes[0].tv_nsec == UTIME_OMIT &&
		    tstimes[1].tv_nsec == UTIME_OMIT)
			return 0;
	}

	return do_utimes(dfd, filename, utimes ? tstimes : NULL, flags);
}
```

（`fs/utimes.c:142`）

用户态原型：

```c
#include <fcntl.h>
#include <sys/stat.h>
int utimensat(int dirfd, const char *pathname,
              const struct timespec times[2], int flags);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `dfd` | `int` | 目录 fd 或 `AT_FDCWD`；`filename == NULL` 且 `dfd` 为有效 fd 时操作该 fd |
| `filename` | `const char *` | 路径；配合 `AT_EMPTY_PATH` 可为空串 |
| `utimes` | `struct timespec[2]` | `[0]`=atime，`[1]`=mtime；`NULL` 表示两者都设为当前时间 |
| `flags` | `int` | `AT_SYMLINK_NOFOLLOW`、`AT_EMPTY_PATH` |

特殊 `tv_nsec`（`include/linux/stat.h:15`）：

```c
#define UTIME_NOW	((1l << 30) - 1l)	/* 取当前时间 */
#define UTIME_OMIT	((1l << 30) - 2l)	/* 保持原值 */
```

其他情况下 `tv_nsec` 必须在 `[0, 999999999]` 内。

## 3. 内核实现路径

```
utimensat(dfd, filename, utimes, flags)      fs/utimes.c:142
├─ get_timespec64() × 2                      // 拷贝并校验用户 timespec
├─ 两者都是 UTIME_OMIT → 直接返回 0（不查找路径）
└─ do_utimes(dfd, filename, times, flags)    fs/utimes.c:134
   ├─ filename == NULL && dfd != AT_FDCWD
   │    └─ do_utimes_fd(fd, times, flags)    fs/utimes.c:108
   │         ├─ flags != 0 → -EINVAL
   │         └─ vfs_utimes(&file->f_path, times)
   └─ 否则 do_utimes_path(dfd, filename, times, flags)  fs/utimes.c:82
        ├─ flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH) → -EINVAL
        ├─ 未设 AT_SYMLINK_NOFOLLOW 时加 LOOKUP_FOLLOW
        ├─ filename_lookup()
        ├─ vfs_utimes(&path, times)
        └─ retry_estale() 时加 LOOKUP_REVAL 重试
```

`vfs_utimes()`（`fs/utimes.c:21`）把 `times` 翻译成 `struct iattr`：

```c
error = mnt_want_write(path->mnt);
...
newattrs.ia_valid = ATTR_CTIME | ATTR_MTIME | ATTR_ATIME;
if (times) {
	if (times[0].tv_nsec == UTIME_OMIT)
		newattrs.ia_valid &= ~ATTR_ATIME;
	else if (times[0].tv_nsec != UTIME_NOW) {
		newattrs.ia_atime = times[0];
		newattrs.ia_valid |= ATTR_ATIME_SET;
	}
	/* mtime 同理 */
	newattrs.ia_valid |= ATTR_TIMES_SET;
} else {
	newattrs.ia_valid |= ATTR_TOUCH;
}
...
inode_lock(inode);
error = notify_change(mnt_idmap(path->mnt), path->dentry, &newattrs,
		      &delegated_inode);
inode_unlock(inode);
```

- `mnt_want_write()` 拒绝只读挂载。
- `ATTR_TIMES_SET` 表示"显式时间更新"，即使只更新一项也要求属主权限。
- `times == NULL` 或两者都是 `UTIME_NOW` 时设置 `ATTR_TOUCH`，允许"属主或具有写权限"的调用者。

`notify_change()`（`fs/attr.c:427`）依次执行：

1. `may_setattr()`（`fs/attr.c:372`）：immutable/append-only 文件拒绝修改；`ATTR_TOUCH` 下非属主需要 `MAY_WRITE`。
2. 用 `current_time(inode)` 填充未显式指定的时间，`ATTR_*_SET` 的时间经 `timestamp_truncate()` 截断到文件系统精度。
3. `setattr_prepare()`（`fs/attr.c:161`）做权限与大小检查。
4. 调用 `inode->i_op->setattr()`，例如 ext4 的 `ext4_setattr()`。

### 3.1 与 fs/stat.c 的关系

`utimensat` 只写时间；观察时间在 `fs/stat.c`：

- `generic_fillattr()`（82）把 `inode_get_atime()` / `inode_get_mtime()` 填入 `struct kstat`。
- `vfs_getattr_nosec()` / `vfs_getattr()`（181 / 254）允许文件系统覆盖属性，`SB_NOATIME` 时提供填充 atime。
- `fill_mg_cmtime()`（45）用于只请求 mtime/ctime 的 `statx` 场景。

两者共用 inode 中的时间戳与 `i_version` 语义，因此 `utimensat` 更新后下一次 `stat` 能立即看到新值（是否落盘取决于文件系统）。

## 4. 核心数据结构

```c
/* include/linux/fs.h:257 */
struct iattr {
	unsigned int	ia_valid;	/* ATTR_ATIME / ATTR_MTIME / ATTR_*_SET / ATTR_TOUCH */
	umode_t		ia_mode;
	union { kuid_t ia_uid; vfsuid_t ia_vfsuid; };
	union { kgid_t ia_gid; vfsgid_t ia_vfsgid; };
	loff_t		ia_size;
	struct timespec64 ia_atime;
	struct timespec64 ia_mtime;
	struct timespec64 ia_ctime;
	struct file	*ia_file;
};

/* include/linux/fs.h:227 附近的 ia_valid 位 */
#define ATTR_CTIME	(1 << 6)
#define ATTR_ATIME_SET	(1 << 7)
#define ATTR_MTIME_SET	(1 << 8)
#define ATTR_CTIME_SET	(1 << 10)
#define ATTR_TIMES_SET	(1 << 16)
#define ATTR_TOUCH	(1 << 17)
```

`struct timespec64` 由 `get_timespec64()` 从用户态 `__kernel_timespec` 拷贝，避免 32 位 `time_t` 的截断问题。当前 `.config` 中 `CONFIG_COMPAT` 未启用，`utimensat_time32` 路径不编译，但源码保留在 `fs/utimes.c:246`。

## 5. 关键流程

```
utimensat(AT_FDCWD, "/tmp/f", { {0, UTIME_NOW}, {0, UTIME_OMIT} }, 0)
  │
  ├─ 拷贝两个 timespec，校验 nsec
  ├─ 不是"双 OMIT"
  ├─ do_utimes_path
  │    ├─ 校验 flags
  │    ├─ filename_lookup：得到 path（默认跟随符号链接）
  │    └─ vfs_utimes
  │         ├─ mnt_want_write：只读挂载 → EROFS
  │         ├─ ia_valid = ATTR_CTIME|ATTR_MTIME|ATTR_ATIME
  │         ├─ atime = UTIME_NOW → 不加 ATTR_ATIME_SET
  │         ├─ mtime = UTIME_OMIT → 清除 ATTR_MTIME
  │         ├─ ia_valid |= ATTR_TIMES_SET
  │         ├─ inode_lock
  │         ├─ notify_change
  │         │    ├─ may_setattr：immutable/append-only 检查
  │         │    ├─ current_time + timestamp_truncate
  │         │    ├─ setattr_prepare
  │         │    └─ i_op->setattr（ext4_setattr）
  │         └─ mnt_drop_write
  └─ 返回 0
```

若文件系统在 `setattr` 中检测到 NFS 委派冲突，会返回 `-EAGAIN` 类错误并通过 `delegated_inode` 触发 `break_deleg_wait()`，随后 `goto retry_deleg` 重试。

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `0` | 成功；两个时间都为 `UTIME_OMIT` 时完全不查找路径直接成功 |
| `-EFAULT` | `utimes` 指针不可读 |
| `-EINVAL` | `tv_nsec` 非法；`flags` 含未知位；fd 形式下 `flags != 0`；`filename == NULL` 且 `dfd == AT_FDCWD` |
| `-EBADF` | fd 形式下 `dfd` 无效 |
| `-EPERM` | 非属主且无权限；immutable/append-only 文件；`ATTR_TIMES_SET` 下权限不足 |
| `-EACCES` | 路径中目录不可搜索，或 `ATTR_TOUCH` 下缺少写权限 |
| `-EROFS` | 挂载只读（`mnt_want_write()`） |
| `-ENOENT` / `-ENOTDIR` / `-ELOOP` | 路径查找失败 |
| `-EOPNOTSUPP` | 文件系统不支持修改时间（如部分只读伪文件系统） |
| `-ESTALE` | NFS 文件句柄失效（内部会 `LOOKUP_REVAL` 重试） |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
	const char *path = "/tmp/utimensat-demo";
	int fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) { perror("open"); return 1; }
	write(fd, "x", 1);
	close(fd);

	/* 只把 mtime 设为当前时间，atime 保持不变 */
	struct timespec ts[2] = {
		{ .tv_sec = 0, .tv_nsec = UTIME_OMIT },
		{ .tv_sec = 0, .tv_nsec = UTIME_NOW  },
	};
	if (utimensat(AT_FDCWD, path, ts, 0) < 0) {
		perror("utimensat");
		return 1;
	}

	/* 显式设置两个时间（2024-01-01 00:00:00 UTC） */
	struct timespec set[2] = {
		{ .tv_sec = 1704067200, .tv_nsec = 123456789 },
		{ .tv_sec = 1704067200, .tv_nsec = 987654321 },
	};
	if (utimensat(AT_FDCWD, path, set, 0) < 0)
		perror("utimensat set");

	/* 通过 fd 操作，filename 传 NULL */
	if (utimensat(fd, NULL, set, 0) < 0)
		perror("utimensat fd");
	return 0;
}
```

符号链接本身的时间戳：

```c
struct timespec now[2] = { {0, UTIME_NOW}, {0, UTIME_NOW} };
utimensat(AT_FDCWD, "/tmp/symlink", now, AT_SYMLINK_NOFOLLOW);
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/utimes.c` | `nsec_valid()`（13）、`vfs_utimes()`（21）、`do_utimes_path()`（82）、`do_utimes_fd()`（108）、`do_utimes()`（134）、`SYSCALL_DEFINE4(utimensat)`（142） |
| `fs/attr.c` | `setattr_prepare()`（161）、`may_setattr()`（372）、`notify_change()`（427） |
| `fs/stat.c` | `fill_mg_cmtime()`（45）、`generic_fillattr()`（82）、`vfs_getattr()`（254） |
| `include/linux/fs.h` | `struct iattr`（257）、`ATTR_*` 位 |
| `include/linux/stat.h` | `UTIME_NOW` / `UTIME_OMIT`（15-16） |
| `fs/ext4/inode.c` | `ext4_setattr()`（6223） |
| `include/uapi/asm-generic/unistd.h` | `__NR_utimensat`（246） |
