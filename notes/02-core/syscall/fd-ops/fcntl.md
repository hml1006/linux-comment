# fcntl 系统调用

> 所属分类：文件描述符操作 | [ARM64 系统调用表](../arm64-syscall-table.md#1-文件描述符操作)

## 1. 概述

`fcntl` 是文件描述符的通用控制接口，按 `cmd` 分派到多个独立子系统：

- 复制 fd 与标志位：`F_DUPFD*`、`F_GETFD` / `F_SETFD`、`F_GETFL` / `F_SETFL`
- 记录锁：`F_GETLK` / `F_SETLK` / `F_SETLKW` / `F_OFD_*`
- 异步通知、租约、目录通知、管道容量、memfd 密封
- 写生命周期提示、委派、新查询命令：`F_*_RW_HINT`、`F_*DELEG`、`F_CREATED_QUERY`

ARM64 编号 **25**（`include/uapi/asm-generic/unistd.h:87` 的 `__NR3264_fcntl`）。32 位 ABI 需 `fcntl64`（`fs/fcntl.c:608`）处理 `struct flock64`；当前 `.config` 中 `CONFIG_COMPAT` 未启用，compat 路径不参与编译。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE3(fcntl, unsigned int, fd, unsigned int, cmd, unsigned long, arg)
{
	CLASS(fd_raw, f)(fd);
	long err;

	if (fd_empty(f))
		return -EBADF;

	if (unlikely(fd_file(f)->f_mode & FMODE_PATH)) {
		if (!check_fcntl_cmd(cmd))
			return -EBADF;
	}

	err = security_file_fcntl(fd_file(f), cmd, arg);
	if (!err)
		err = do_fcntl(fd, cmd, arg, fd_file(f));

	return err;
}
```

（`fs/fcntl.c:587`）

用户态原型：`int fcntl(int fd, int cmd, ... /* arg */);`

| `cmd` | `arg` 类型 | 说明 |
| --- | --- | --- |
| `F_DUPFD` / `F_DUPFD_CLOEXEC` | `int` | 新 fd 下界；后者带 `FD_CLOEXEC` |
| `F_DUPFD_QUERY` | `int` | 判断 fd 是否指向同一个 `struct file` |
| `F_GETFD` / `F_SETFD` | `int` | `FD_CLOEXEC` |
| `F_GETFL` / `F_SETFL` | `int` | `O_*` 打开标志 |
| `F_GETLK` / `F_SETLK` / `F_SETLKW` / `F_OFD_*` | `struct flock *` | 记录锁 |
| `F_GETOWN` / `F_SETOWN` | `int` | 进程/进程组 |
| `F_GETOWN_EX` / `F_SETOWN_EX` | `struct f_owner_ex *` | 带类型属主 |
| `F_GETSIG` / `F_SETSIG` | `int` | 异步通知信号 |
| `F_GETLEASE` / `F_SETLEASE` | `int` | `F_RDLCK`/`F_WRLCK`/`F_UNLCK` |
| `F_NOTIFY` | `int` | `DN_*` |
| `F_GETPIPE_SZ` / `F_SETPIPE_SZ` | `int` | 管道容量 |
| `F_ADD_SEALS` / `F_GET_SEALS` | `unsigned int` | `F_SEAL_*` |
| `F_GET_RW_HINT` / `F_SET_RW_HINT` | `uint64_t *` | `RWH_WRITE_LIFE_*` |
| `F_GETDELEG` / `F_SETDELEG` | `struct delegation *` | 委派控制 |

## 3. 内核实现路径

```
fcntl(fd, cmd, arg)                          fs/fcntl.c:587
├─ fd_raw：fdget_raw()/fdput()，允许 O_PATH fd
├─ check_fcntl_cmd()                         fs/fcntl.c:572
│    // 仅 F_DUPFD*、F_GETFD、F_SETFD、F_GETFL、F_CREATED_QUERY 放行 FMODE_PATH
├─ security_file_fcntl()
└─ do_fcntl()                                fs/fcntl.c:444
   ├─ F_DUPFD[_CLOEXEC] → f_dupfd()          fs/file.c:1580
   ├─ F_DUPFD_QUERY     → f_dupfd_query()    fs/fcntl.c:396
   ├─ F_CREATED_QUERY   → f_created_query()  // FMODE_CREATED
   ├─ F_GETFD / F_SETFD → get/set_close_on_exec()  fs/file.c:1329 / 1321
   ├─ F_GETFL           → filp->f_flags
   ├─ F_SETFL           → setfl()            fs/fcntl.c:39
   ├─ F_GETLK / F_SETLK(W) / F_OFD_* → fcntl_getlk()/fcntl_setlk()
   │                                          fs/locks.c:2437 / 2565
   ├─ F_*OWN* / F_*SIG  → f_getown()/f_setown()/f_setown_ex()/f_owner_sig()
   ├─ F_*LEASE          → fcntl_getlease()/fcntl_setlease()  fs/locks.c
   ├─ F_NOTIFY          → fcntl_dirnotify()  fs/notify/dnotify/dnotify.c:259
   ├─ F_*PIPE_SZ        → pipe_fcntl()       fs/pipe.c:1525
   ├─ F_ADD/GET_SEALS   → memfd_fcntl()      mm/memfd.c:319
   ├─ F_*_RW_HINT       → fcntl_get/set_rw_hint()  fs/fcntl.c:358/369
   ├─ F_*DELEG          → fcntl_get/setdeleg()
   └─ default           → -EINVAL
```

### 3.1 F_DUPFD

```c
int f_dupfd(unsigned int from, struct file *file, unsigned flags)
{
	unsigned long nofile = rlimit(RLIMIT_NOFILE);
	int err;
	if (from >= nofile)
		return -EINVAL;
	err = alloc_fd(from, nofile, flags);
	if (err >= 0) {
		get_file(file);
		fd_install(err, file);
	}
	return err;
}
```

（`fs/file.c:1580`）只给下界，由 `alloc_fd()` 取最小可用编号；与 [`dup3`](./dup3.md) 指定精确目标并先关闭旧文件不同。

### 3.2 F_SETFL

`setfl()`（`fs/fcntl.c:39`）只允许修改：

```c
#define SETFL_MASK (O_APPEND | O_NONBLOCK | O_NDELAY | O_DIRECT | O_NOATIME)
...
spin_lock(&filp->f_lock);
filp->f_flags = (arg & SETFL_MASK) | (filp->f_flags & ~SETFL_MASK);
filp->f_iocb_flags = iocb_flags(filp);
spin_unlock(&filp->f_lock);
```

写回前检查：append-only 文件不允许清 `O_APPEND`；`O_NOATIME` 需属主或 `CAP_FOWNER`；非 FIFO 设 `O_DIRECT` 需 `FMODE_CAN_ODIRECT`；`FASYNC` 变化调用 `f_op->fasync()`；`f_op->check_flags()` 可否决组合。

### 3.3 记录锁

`fcntl_getlk()`（`fs/locks.c:2437`）用 `flock64_to_posix_lock()`（`fs/locks.c:510`）把用户 `struct flock` 转成 `struct file_lock`，再调用 `vfs_test_lock()`（`fs/locks.c:2345`）：有 `f_op->lock` 就用，否则 `posix_test_lock()`。`F_GETLK` 只报告第一个冲突锁。

`fcntl_setlk()`（`fs/locks.c:2565`）处理 `F_SETLK`/`F_SETLKW`；OFD 锁额外置 `FL_OFDLCK` 并把 `flc_owner` 从 `current->files` 换成 `filp`；`F_SETLKW` 置 `FL_SLEEP`。最终经 `do_lock_file_wait()` → `vfs_lock_file()`（`fs/locks.c:2513`）→ `f_op->lock()` 或 `posix_lock_file()`。

## 4. 核心数据结构

```c
/* include/linux/fs.h:1255 */
struct file {
	spinlock_t			f_lock;
	fmode_t				f_mode;		/* FMODE_PATH / FMODE_CREATED */
	const struct file_operations	*f_op;
	struct inode			*f_inode;
	unsigned int			f_flags;	/* F_GETFL/SETFL */
	unsigned int			f_iocb_flags;
	struct fown_struct		*f_owner;
	...
};

/* include/linux/fs.h:1184 */
struct fown_struct {
	struct file *file;
	rwlock_t lock;
	struct pid *pid;
	enum pid_type pid_type;		/* PIDTYPE_PID/TGID/PGID */
	kuid_t uid, euid;
	int signum;			/* F_SETSIG */
};

/* include/linux/filelock.h:107 / 126 */
struct file_lock_core {
	/* 链表、阻塞者与被阻塞请求等字段略 */
	fl_owner_t flc_owner;		/* POSIX: current->files；OFD: struct file * */
	unsigned int flc_flags;		/* FL_POSIX/FL_OFDLCK/FL_SLEEP/FL_ACCESS */
	unsigned char flc_type;		/* F_RDLCK/F_WRLCK/F_UNLCK */
	pid_t flc_pid;
	wait_queue_head_t flc_wait;
	struct file *flc_file;
};

struct file_lock {
	struct file_lock_core c;
	loff_t fl_start;
	loff_t fl_end;
	const struct file_lock_operations *fl_ops;
	const struct lock_manager_operations *fl_lmops;
	union { ... } fl_u;
} __randomize_layout;
```

memfd 的 seal 存在底层 inode 中：`memfd_file_seals_ptr()`（`mm/memfd.c:210`）返回 `SHMEM_I(inode)->seals` 或 `HUGETLBFS_I(inode)->seals`。

## 5. 关键流程

### 5.1 命令分派

| 分类 | 命令 | 处理函数 |
| --- | --- | --- |
| fd 复制 | `F_DUPFD*` | `f_dupfd()`、`f_dupfd_query()` |
| fd 标志 | `F_GETFD`、`F_SETFD` | `get/set_close_on_exec()` |
| 打开标志 | `F_GETFL`、`F_SETFL` | 读 `f_flags`、`setfl()` |
| 记录锁 | `F_GETLK`、`F_SETLK(W)`、`F_OFD_*` | `fcntl_getlk()`、`fcntl_setlk()` |
| 异步通知 | `F_*OWN*`、`F_*SIG` | `f_getown()`、`f_setown_ex()`、`f_owner_sig()` |
| 租约 | `F_*LEASE` | `fcntl_getlease()`、`fcntl_setlease()` |
| 目录通知 | `F_NOTIFY` | `fcntl_dirnotify()` |
| 密封 | `F_ADD_SEALS`、`F_GET_SEALS` | `memfd_fcntl()` |

`include/uapi/linux/fcntl.h:62` 还定义了 `F_GET_FILE_RW_HINT`/`F_SET_FILE_RW_HINT`，但 `do_fcntl()` 无对应分支，走 `default` 返回 `-EINVAL`——UAPI 预留、当前未实现。

### 5.2 加锁流程

```
struct flock
  │ copy_from_user
  ▼
flock64_to_posix_lock()  fs/locks.c:510
  │ 解析 l_whence/l_start/l_len（负 l_len 向低地址扩展）
  │ flc_owner=current->files; flc_flags=FL_POSIX; flc_file=filp
  ▼
fcntl_setlk() / fcntl_getlk()
  │ OFD: flc_owner=filp, flc_flags|=FL_OFDLCK
  │ F_SETLKW: flc_flags|=FL_SLEEP
  ▼
vfs_lock_file() / vfs_test_lock()
  ├─ f_op->lock()
  └─ posix_lock_file() / posix_test_lock()
        └─ inode->i_flctx 的 flc_posix 链表
```

POSIX 锁按 `files_struct` 归属，`close()`/退出会清理；OFD 锁按 `struct file` 归属，`dup` 共享同一锁，`close()` 不释放其他副本的锁。`l_pid == -1` 用于回报 OFD 锁（`locks_translate_pid()`，`fs/locks.c:2374`）。

### 5.3 密封

`memfd_add_seals()`（`mm/memfd.c:230`）：

1. 必须 `FMODE_WRITE`（否则 `-EPERM`），`seals & ~F_ALL_SEALS` → `-EINVAL`。
2. 底层必须是 shmem/hugetlbfs，否则 `-EINVAL`。
3. 已有 `F_SEAL_SEAL` → `-EPERM`。
4. `F_SEAL_EXEC` 且文件已有执行位时，隐式加 `SHRINK|GROW|WRITE|FUTURE_WRITE`，保证 W^X。
5. 首次加 `F_SEAL_WRITE` 时 `mapping_deny_writable()` + `memfd_wait_for_pins()`，失败回滚。

seal 只增不减，作用于整个 inode；详见 [memfd_create](../memfd/memfd_create.md)。

## 6. 返回值与错误码

| 命令 | 成功返回值 |
| --- | --- |
| `F_DUPFD*` | 新 fd |
| `F_DUPFD_QUERY` | 1/0（是否同一 `struct file`） |
| `F_GETFD` | 0 或 `FD_CLOEXEC` |
| `F_GETFL` | `f_flags` |
| `F_GETOWN` | 属主 pid（负值表示进程组，使用 `force_successful_syscall_return()`） |
| 其他 `F_GET*` | 对应值 |
| 设置类 | 0 |

| 错误码 | 触发条件 |
| --- | --- |
| `-EBADF` | fd 无效；`O_PATH` fd 使用非白名单命令；`F_SETLK` 读写类型与 `f_mode` 不符 |
| `-EINVAL` | 未知 cmd；`struct flock` 非法；seal 未知位；OFD 锁 `l_pid != 0`；`from >= RLIMIT_NOFILE` |
| `-EPERM` | `O_NOATIME` 非属主；追加文件清 `O_APPEND`；已有 `F_SEAL_SEAL`；租约权限不足 |
| `-EFAULT` | 用户指针非法 |
| `-EAGAIN` / `-EACCES` | `F_SETLK` 冲突（内核通常返回 `-EAGAIN`） |
| `-ENOLCK` | 锁资源耗尽或文件系统不支持 |
| `-ENOMEM` | 锁/上下文分配失败 |
| `-ESRCH` | `F_SETOWN_EX` 的 pid 不存在 |
| `-EOPNOTSUPP` | 文件系统不支持该操作 |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/etc/hostname", O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }

	/* 复制到 >= 10 的 fd，并带 FD_CLOEXEC */
	int nfd = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	printf("nfd=%d cloexec=%d\n", nfd, fcntl(nfd, F_GETFD) & FD_CLOEXEC);

	/* 读取并修改打开标志 */
	int flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_APPEND);

	/* OFD 写锁：l_pid 必须为 0 */
	struct flock fl = {
		.l_type = F_WRLCK, .l_whence = SEEK_SET,
		.l_start = 0, .l_len = 0, .l_pid = 0,
	};
	if (fcntl(fd, F_OFD_SETLK, &fl) < 0)
		perror("F_OFD_SETLK");

	close(nfd);
	close(fd);
	return 0;
}
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/fcntl.c` | `SYSCALL_DEFINE3(fcntl)`（587）、`do_fcntl()`（444）、`setfl()`（39）、`f_setown_ex()`（216）、`f_owner_sig()`（420） |
| `fs/file.c` | `f_dupfd()`（1580）、`set_close_on_exec()`（1321）、`get_close_on_exec()`（1329）、`fdget_pos()`（1298） |
| `fs/locks.c` | `flock64_to_posix_lock()`（510）、`vfs_test_lock()`（2345）、`fcntl_getlk()`（2437）、`vfs_lock_file()`（2513）、`fcntl_setlk()`（2565）、`fcntl_setlease()`（2186） |
| `mm/memfd.c` | `memfd_file_seals_ptr()`（210）、`memfd_add_seals()`（230）、`memfd_fcntl()`（319） |
| `fs/pipe.c` | `pipe_fcntl()`（1525） |
| `fs/notify/dnotify/dnotify.c` | `fcntl_dirnotify()`（259） |
| `include/linux/fs.h` / `include/linux/filelock.h` | `struct file`、`fown_struct`、`file_lock` |
| `include/uapi/linux/fcntl.h` / `asm-generic/fcntl.h` | `F_*` 命令号、`F_SEAL_*`、`struct flock` |
| `include/uapi/asm-generic/unistd.h` | `__NR3264_fcntl`（87） |
