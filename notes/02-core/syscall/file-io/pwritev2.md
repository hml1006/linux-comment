# pwritev2 系统调用

> 所属分类：文件 I/O | [ARM64 系统调用表](../arm64-syscall-table.md#2-文件-io)

## 1. 概述

`pwritev2` 是 [`pwritev`](./pwritev.md) 的扩展：把多个用户缓冲区集中写入文件（gather write），可指定文件偏移，并携带每次 I/O 的行为标志（`RWF_*`）。

- ARM64 编号 **287**（`include/uapi/asm-generic/unistd.h:691`）。
- `pos == -1` 时退化为 [`writev`](./writev.md) 语义，从当前 `f_pos` 写入并推进；否则写到给定位移，**不改变** `f_pos`。
- 相比 `pwritev` 多出的能力主要来自 `flags`：`RWF_DSYNC`、`RWF_SYNC`、`RWF_NOWAIT`、`RWF_APPEND`、`RWF_NOAPPEND`、`RWF_ATOMIC`、`RWF_DONTCACHE`、`RWF_NOSIGNAL`、`RWF_HIPRI`。
- 写入路径比读多一层 `file_start_write()`，用于块设备/文件系统的一致性保护。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE6(pwritev2, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h,
		rwf_t, flags)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);

	if (pos == -1)
		return do_writev(fd, vec, vlen, flags);

	return do_pwritev(fd, vec, vlen, pos, flags);
}
```

（`fs/read_write.c:1482`）

用户态原型：

```c
#define _GNU_SOURCE
#include <sys/uio.h>
ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt,
                 off_t offset, int flags);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `fd` | `unsigned long` | 文件描述符 |
| `vec` | `const struct iovec *` | 待写数据缓冲区数组 |
| `vlen` | `unsigned long` | 段数，上限 `UIO_MAXIOV`=1024 |
| `pos_l` / `pos_h` | `unsigned long` | 偏移低/高 32 位；组合为 `-1` 表示使用当前 `f_pos` |
| `flags` | `rwf_t`（`int`） | `RWF_*` 位掩码 |

`flags` 定义见 `include/uapi/linux/fs.h:428`，完整集合由 `RWF_SUPPORTED` 描述：

```c
#define RWF_SUPPORTED	(RWF_HIPRI | RWF_DSYNC | RWF_SYNC | RWF_NOWAIT |\
			 RWF_APPEND | RWF_NOAPPEND | RWF_ATOMIC |\
			 RWF_DONTCACHE | RWF_NOSIGNAL)
```

其中 `RWF_ATOMIC` 只对写有意义；`RWF_APPEND` 与 `RWF_NOAPPEND` 互斥。

## 3. 内核实现路径

```
pwritev2(fd, vec, vlen, pos_l, pos_h, flags) fs/read_write.c:1482
├─ pos_from_hilo(pos_h, pos_l)               fs/read_write.c:1394
├─ pos == -1 → do_writev(fd, vec, vlen, flags)     // 使用并推进 f_pos
└─ do_pwritev(fd, vec, vlen, pos, flags)     fs/read_write.c:1421
   ├─ pos < 0 → -EINVAL
   ├─ CLASS(fd, f)(fd) → -EBADF
   ├─ !(f_mode & FMODE_PWRITE) → -ESPIPE
   └─ vfs_writev(file, vec, vlen, &pos, flags) fs/read_write.c:1265
      ├─ !(f_mode & FMODE_WRITE) → -EBADF
      ├─ !(f_mode & FMODE_CAN_WRITE) → -EINVAL
      ├─ import_iovec(ITER_SOURCE, ...)       lib/iov_iter.c:1466
      ├─ tot_len == 0 → 返回 0
      ├─ rw_verify_area(WRITE, file, pos, tot_len)
      ├─ file_start_write(file)
      ├─ f_op->write_iter ? do_iter_readv_writev()
      │                    : do_loop_readv_writev()
      ├─ fsnotify_modify(file)
      └─ file_end_write(file)
```

`do_writev()`（`fs/read_write.c:1364`）与 `do_readv()` 对称，区别是使用 `CLASS(fd_pos)` 取得 `f_pos` 并在成功后写回；`do_pwritev()` 使用 `CLASS(fd)`，只操作局部 `pos`。

### 3.1 为什么写路径需要 file_start_write

`vfs_writev()` 在调用文件系统前执行 `file_start_write(file)`，结束后 `file_end_write(file)`。这与 `sb_start_write()`/`sb_end_write()` 的冻结机制配合，防止在文件系统冻结（如快照、remount-ro）期间写入。读路径不需要这一层。

`do_iter_readv_writev()` 本身不负责写保护，它只构造 `kiocb` 并调用 `write_iter`。RWF 标志的转换与校验发生在 `kiocb_set_rw_flags()`（`include/linux/fs.h:3438`）：

```c
if (unlikely(flags & ~RWF_SUPPORTED))
	return -EOPNOTSUPP;
if (unlikely((flags & RWF_APPEND) && (flags & RWF_NOAPPEND)))
	return -EINVAL;
if (flags & RWF_NOWAIT) {
	if (!(ki->ki_filp->f_mode & FMODE_NOWAIT))
		return -EOPNOTSUPP;
}
if (flags & RWF_ATOMIC) {
	if (rw_type != WRITE)
		return -EOPNOTSUPP;
	if (!(ki->ki_filp->f_mode & FMODE_CAN_ATOMIC_WRITE))
		return -EOPNOTSUPP;
}
```

### 3.2 偏移语义

- `pos != -1`：`kiocb.ki_pos = pos`，`write_iter` 在该偏移写入；`do_iter_readv_writev()` 只把推进后的 `ki_pos` 写回局部 `pos`。
- `pos == -1`：走 `do_writev()`，`ppos = file_ppos(file)`（`fs/read_write.c:843`），普通文件返回 `&file->f_pos`，流对象返回 `NULL`；成功后 `file->f_pos = pos`。
- `RWF_APPEND`：即使给了偏移，文件系统也可把写入追加到末尾（由 `IOCB_APPEND` 指示）；`RWF_NOAPPEND` 用于临时压过 `O_APPEND`。

## 4. 核心数据结构

```c
/* include/linux/uio.h */
struct iov_iter {
	u8 iter_type;
	bool nofault;
	bool data_source;		/* WRITE 时数据从用户流向内核 */
	size_t iov_offset;
	size_t count;
	union {
		const struct iovec *__iov;
		const struct bio_vec *bvec;
		const char *kvec;
		...
	};
	unsigned long nr_segs;
	...
};

/* include/linux/fs.h —— 同步写请求 */
struct kiocb {
	struct file		*ki_filp;
	loff_t			ki_pos;		/* 本次写入偏移，不自动写回 f_pos */
	void (*ki_complete)(struct kiocb *, long, long);
	void			*private;
	int			ki_flags;	/* IOCB_APPEND / IOCB_DSYNC / IOCB_NOWAIT ... */
	u16			ki_ioprio;
	...
};

/* include/linux/fs.h:1255 */
struct file {
	...
	fmode_t			f_mode;		/* FMODE_WRITE / FMODE_PWRITE */
	unsigned int		f_flags;	/* O_APPEND 等 */
	loff_t			f_pos;
	...
};
```

## 5. 关键流程

```
pwritev2(fd, iov, 2, off, RWF_DSYNC)
  │
  ├─ pos != -1 → do_pwritev
  │    ├─ 需要 FMODE_PWRITE（普通文件/块设备，非管道）
  │    └─ vfs_writev
  │         ├─ import_iovec：复制 iovec、校验地址、截断到 MAX_RW_COUNT
  │         ├─ rw_verify_area：偏移、长度、MAY_WRITE、LSM、fsnotify
  │         ├─ file_start_write：防冻结竞态
  │         ├─ do_iter_readv_writev
  │         │    ├─ kiocb_set_rw_flags：RWF_DSYNC → IOCB_DSYNC
  │         │    ├─ kiocb.ki_pos = off
  │         │    └─ f_op->write_iter
  │         │         └─ ext4_file_write_iter → generic_file_write_iter / iomap
  │         ├─ fsnotify_modify
  │         └─ file_end_write
  └─ 返回写入字节数，不更新 file->f_pos
```

短写（返回值小于 `tot_len`）是合法的，调用方需自行循环；`O_NONBLOCK` 或 `RWF_NOWAIT` 下可能返回 `-EAGAIN`。

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `> 0` | 实际写入的字节数（可能短写） |
| `0` | `iov` 总长为 0 |
| `-EBADF` | `fd` 无效，或未以写方式打开（`FMODE_WRITE` 缺失） |
| `-EINVAL` | `pos < 0` 且不等于 -1；不可写（`FMODE_CAN_WRITE` 缺失）；`iovcnt` 超限；`rw_verify_area` 判定非法 |
| `-ESPIPE` | 对象不支持 `pwrite`（缺少 `FMODE_PWRITE`，如管道） |
| `-EFAULT` | `iov` 或数据缓冲区不可访问 |
| `-EOPNOTSUPP` | 未知 `RWF_*`；`RWF_NOWAIT` 无 `FMODE_NOWAIT`；`RWF_ATOMIC` 无 `FMODE_CAN_ATOMIC_WRITE`；`RWF_DONTCACHE` 不受支持 |
| `-EAGAIN` | `RWF_NOWAIT`/`O_NONBLOCK` 下无法立即写入 |
| `-EPERM` | `RWF_NOAPPEND` 与 append-only inode 冲突 |
| `-ENOSPC` | 设备空间不足 |
| `-EDQUOT` | 超出配额 |
| `-EFBIG` | 超过文件大小上限或 `RLIMIT_FSIZE` |
| `-ENOMEM` | `iovec` 数组分配失败 |

写入超过 `RLIMIT_FSIZE` 时还可能收到 `SIGXFSZ`。

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/tmp/pwritev2-demo", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { perror("open"); return 1; }

	struct iovec iov[2];
	iov[0].iov_base = "hello ";
	iov[0].iov_len  = 6;
	iov[1].iov_base = "world\n";
	iov[1].iov_len  = 6;

	/* 从偏移 100 写，带 O_DSYNC 语义；f_pos 不变 */
	ssize_t n = pwritev2(fd, iov, 2, 100, RWF_DSYNC);
	if (n < 0) {
		perror("pwritev2");
	} else {
		printf("wrote %zd bytes at 100\n", n);
		printf("f_pos = %lld\n", (long long)lseek(fd, 0, SEEK_CUR));
	}

	close(fd);
	return 0;
}
```

追加语义：

```c
/* O_APPEND 文件上临时忽略追加 */
ssize_t n = pwritev2(fd, iov, 2, 0, RWF_NOAPPEND);
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/read_write.c` | `pos_from_hilo()`（1394）、`do_pwritev()`（1421）、`SYSCALL_DEFINE6(pwritev2)`（1482）、`vfs_writev()`（1265）、`do_iter_readv_writev()`（1002）、`file_ppos()`（843） |
| `include/linux/fs.h` | `kiocb_set_rw_flags()`（3438）、`struct kiocb`、`struct file` |
| `include/linux/uio.h` | `struct iov_iter` |
| `include/uapi/linux/fs.h` | `RWF_*` 与 `RWF_SUPPORTED`（428-457） |
| `include/uapi/linux/uio.h` | `struct iovec`、`UIO_MAXIOV` |
| `lib/iov_iter.c` | `import_iovec()`（1466） |
| `include/uapi/asm-generic/unistd.h` | `__NR_pwritev2`（691） |
