# preadv2 系统调用

> 所属分类：文件 I/O | [ARM64 系统调用表](../arm64-syscall-table.md#2-文件-io)

## 1. 概述

`preadv2` 是 [`preadv`](./preadv.md) 的扩展：一次性从文件读入多个用户缓冲区（scatter read），并可指定文件偏移和每次 I/O 的行为标志（`RWF_*`）。

- ARM64 编号 **286**（`include/uapi/asm-generic/unistd.h:689`）。
- 当 `pos == -1` 时退化为 [`readv`](./readv.md) 语义，从当前 `f_pos` 读取并推进位置；否则从给定位移读取，**不改变** `f_pos`。
- `flags` 用于表达 `RWF_HIPRI`、`RWF_DSYNC`、`RWF_SYNC`、`RWF_NOWAIT`、`RWF_APPEND`、`RWF_NOAPPEND`、`RWF_ATOMIC`、`RWF_DONTCACHE`、`RWF_NOSIGNAL` 等 per-I/O 语义。
- 底层走 `iov_iter` + `kiocb`，最终调用文件系统的 `read_iter`（回退到 `read`）。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE6(preadv2, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h,
		rwf_t, flags)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);

	if (pos == -1)
		return do_readv(fd, vec, vlen, flags);

	return do_preadv(fd, vec, vlen, pos, flags);
}
```

（`fs/read_write.c:1462`）

用户态原型：

```c
#define _GNU_SOURCE
#include <sys/uio.h>
ssize_t preadv2(int fd, const struct iovec *iov, int iovcnt,
                off_t offset, int flags);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `fd` | `unsigned long` | 文件描述符 |
| `vec` | `const struct iovec *` | 用户缓冲区数组 |
| `vlen` | `unsigned long` | 数组元素个数，上限 `UIO_MAXIOV`=1024 |
| `pos_l` / `pos_h` | `unsigned long` | 64 位偏移的低/高 32 位；组合为 `-1` 表示使用当前 `f_pos` |
| `flags` | `rwf_t`（`int`） | `RWF_*` 位掩码 |

`include/uapi/linux/uio.h` 定义：

```c
struct iovec {
	void __user *iov_base;
	__kernel_size_t iov_len;
};

#define UIO_FASTIOV	8
#define UIO_MAXIOV	1024
```

`include/uapi/linux/fs.h:428` 起定义 `RWF_*`，`RWF_SUPPORTED` 是所有已支持位的并集：

| 标志 | 值 | 语义 |
| --- | --- | --- |
| `RWF_HIPRI` | 0x01 | 高优先级请求，可轮询完成 |
| `RWF_DSYNC` | 0x02 | 本次 I/O 等效 `O_DSYNC` |
| `RWF_SYNC` | 0x04 | 本次 I/O 等效 `O_SYNC` |
| `RWF_NOWAIT` | 0x08 | 不阻塞，无法立即完成时返回 `-EAGAIN` |
| `RWF_APPEND` | 0x10 | 本次写入追加到文件末尾 |
| `RWF_NOAPPEND` | 0x20 | 本次写入忽略 `O_APPEND` |
| `RWF_ATOMIC` | 0x40 | 原子写（仅写、需硬件/文件系统支持） |
| `RWF_DONTCACHE` | 0x80 | 读写后丢弃页缓存 |
| `RWF_NOSIGNAL` | 0x100 | 管道/socket 写入不产生 `SIGPIPE` |

## 3. 内核实现路径

```
preadv2(fd, vec, vlen, pos_l, pos_h, flags)  fs/read_write.c:1462
├─ pos_from_hilo(pos_h, pos_l)               fs/read_write.c:1394
├─ pos == -1 → do_readv(fd, vec, vlen, flags)      // 使用当前 f_pos
└─ do_preadv(fd, vec, vlen, pos, flags)      fs/read_write.c:1400
   ├─ pos < 0 → -EINVAL
   ├─ CLASS(fd, f)(fd) → -EBADF
   ├─ !(f_mode & FMODE_PREAD) → -ESPIPE
   └─ vfs_readv(file, vec, vlen, &pos, flags) fs/read_write.c:1201
      ├─ !(f_mode & FMODE_READ) → -EBADF
      ├─ !(f_mode & FMODE_CAN_READ) → -EINVAL
      ├─ import_iovec(ITER_DEST, ...)         lib/iov_iter.c:1466
      ├─ tot_len == 0 → 返回 0
      ├─ rw_verify_area(READ, file, pos, tot_len)
      ├─ f_op->read_iter ? do_iter_readv_writev()
      │                    : do_loop_readv_writev()
      └─ fsnotify_access(file)
```

`do_iter_readv_writev()`（`fs/read_write.c:1002`）构造 `kiocb`：

```c
init_sync_kiocb(&kiocb, filp);
ret = kiocb_set_rw_flags(&kiocb, flags, type);
if (ret)
	return ret;
kiocb.ki_pos = (ppos ? *ppos : 0);

if (type == READ)
	ret = filp->f_op->read_iter(&kiocb, iter);
else
	ret = filp->f_op->write_iter(&kiocb, iter);
BUG_ON(ret == -EIOCBQUEUED);
if (ppos)
	*ppos = kiocb.ki_pos;
```

对 `preadv2`，`ppos` 指向 `do_preadv()` 的局部 `pos`，因此 `ki_pos` 的推进只写回局部变量，不修改 `file->f_pos`。

### 3.1 标志校验

`kiocb_set_rw_flags()`（`include/linux/fs.h:3438`）在调用文件系统之前统一校验：

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
if (flags & RWF_DONTCACHE) {
	if (!(ki->ki_filp->f_op->fop_flags & FOP_DONTCACHE))
		return -EOPNOTSUPP;
	if (IS_DAX(ki->ki_filp->f_mapping->host))
		return -EOPNOTSUPP;
}
```

`RWF_NOAPPEND` 在文件带 `O_APPEND` 且 inode 为 append-only 时返回 `-EPERM`，否则清除 `IOCB_APPEND`。

### 3.2 向量导入与长度限制

`import_iovec()`（`lib/iov_iter.c:1466`）把用户 `iovec` 数组复制到内核：小规模（≤ `UIO_FASTIOV`=8）用栈上数组，否则 `kmalloc`。单段时走 `__import_iovec_ubuf()` 快路径。总长度被截断到 `MAX_RW_COUNT`，单段 `iov_len` 转为 `ssize_t` 为负或总长溢出时返回 `-EINVAL`，地址不可访问返回 `-EFAULT`。

## 4. 核心数据结构

```c
/* include/linux/uio.h —— 内核态迭代器 */
struct iov_iter {
	u8 iter_type;			/* ITER_IOVEC / ITER_BVEC / ITER_KVEC / ... */
	bool nofault;
	bool data_source;		/* READ 或 WRITE */
	size_t iov_offset;
	size_t count;			/* 剩余字节数 */
	union {
		const struct iovec *__iov;
		const struct bio_vec *bvec;
		const char *kvec;
		...
	};
	unsigned long nr_segs;
	...
};

/* fs/read_write.c:1002 使用的同步 kiocb */
struct kiocb {
	struct file		*ki_filp;
	loff_t			ki_pos;
	void (*ki_complete)(struct kiocb *, long, long);
	void			*private;
	int			ki_flags;	/* IOCB_*，由 RWF_* 转换 */
	u16			ki_ioprio;
	...
};
```

`init_sync_kiocb()` 把完成回调设为 `NULL`，表示同步调用；`do_iter_readv_writev()` 用 `BUG_ON(ret == -EIOCBQUEUED)` 断言文件系统没有把同步请求异步排队。

## 5. 关键流程

```
preadv2(fd, iov, 3, off, 0, RWF_NOWAIT)
  │
  ├─ pos != -1 → do_preadv
  │    ├─ fd 必须有 FMODE_PREAD
  │    └─ vfs_readv
  │         ├─ import_iovec：复制 iovec、校验地址、限制总长
  │         ├─ rw_verify_area：偏移/长度/权限/LSM/fsnotify
  │         └─ do_iter_readv_writev
  │              ├─ kiocb_set_rw_flags：RWF_NOWAIT 需 FMODE_NOWAIT
  │              ├─ kiocb.ki_pos = off
  │              └─ f_op->read_iter
  │                   └─ ext4_file_read_iter → generic_file_read_iter
  │                        └─ filemap_read / iomap / direct I/O
  └─ 返回实际读取字节数，不更新 file->f_pos
```

失败时可能已经读入部分数据：`read_iter` 返回正数表示短读，调用方需要循环处理剩余量。

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `> 0` | 实际读取的字节数（可能小于请求总量） |
| `0` | 到达文件末尾或 `iov` 总长为 0 |
| `-EBADF` | `fd` 无效，或文件未以读方式打开（`FMODE_READ` 缺失） |
| `-EINVAL` | `pos < 0` 且不等于 -1；文件不可读（`FMODE_CAN_READ` 缺失）；`iovcnt` 超限；`rw_verify_area` 判定偏移/长度非法 |
| `-ESPIPE` | 文件不支持 `pread`（缺少 `FMODE_PREAD`，如管道/socket） |
| `-EFAULT` | `iov` 数组或缓冲区不可访问 |
| `-EOPNOTSUPP` | 未知 `RWF_*`；`RWF_NOWAIT` 无 `FMODE_NOWAIT`；`RWF_ATOMIC` 用于读或无原子写能力；`RWF_DONTCACHE` 不受支持 |
| `-EAGAIN` | `RWF_NOWAIT` 下无法立即完成 |
| `-EPERM` | `RWF_NOAPPEND` 与 append-only inode 冲突 |
| `-ENOMEM` | `iovec` 数组分配失败 |
| `-EOVERFLOW` | 偏移加长度溢出（`rw_verify_area` 对无符号偏移文件） |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/etc/hostname", O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }

	char a[16], b[64];
	struct iovec iov[2] = {
		{ .iov_base = a, .iov_len = sizeof(a) },
		{ .iov_base = b, .iov_len = sizeof(b) },
	};

	/* 从偏移 0 读，使用 RWF_NOWAIT：无法立即完成会返回 EAGAIN */
	ssize_t n = preadv2(fd, iov, 2, 0, RWF_NOWAIT);
	if (n < 0) {
		perror("preadv2");
	} else {
		printf("read %zd bytes\n", n);
		/* 确认 f_pos 未变 */
		printf("f_pos = %lld\n", (long long)lseek(fd, 0, SEEK_CUR));
	}

	close(fd);
	return 0;
}
```

`offset = -1` 的用法（等价 `readv`）：

```c
ssize_t n = preadv2(fd, iov, 2, -1, 0);
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/read_write.c` | `pos_from_hilo()`（1394）、`do_preadv()`（1400）、`SYSCALL_DEFINE6(preadv2)`（1462）、`vfs_readv()`（1201）、`do_iter_readv_writev()`（1002） |
| `include/linux/fs.h` | `kiocb_set_rw_flags()`（3438）、`struct kiocb` |
| `include/linux/uio.h` | `struct iov_iter` |
| `include/uapi/linux/fs.h` | `RWF_*` 与 `RWF_SUPPORTED`（428-457） |
| `include/uapi/linux/uio.h` | `struct iovec`、`UIO_FASTIOV`、`UIO_MAXIOV` |
| `lib/iov_iter.c` | `import_iovec()`（1466）、`__import_iovec()`（1389） |
| `include/uapi/asm-generic/unistd.h` | `__NR_preadv2`（689） |
