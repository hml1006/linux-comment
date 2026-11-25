# lseek 系统调用

> 所属分类：文件 I/O | [ARM64 系统调用表](../arm64-syscall-table.md#2-文件-io)

## 1. 概述

`lseek` 调整打开文件的读写位置 `file->f_pos`，并返回新位置。它本身不读写数据，只把 `whence` 与 `offset` 组合后交给文件系统的 `->llseek` 回调处理。

- ARM64 编号 **62**（`include/uapi/asm-generic/unistd.h:170`，`__NR3264_lseek`，64 位映射到 `sys_lseek`）。
- 位置存放在 `struct file` 的 `f_pos` 中，属于打开文件描述，由 `dup`/`fork` 共享。
- `SEEK_DATA` / `SEEK_HOLE` 让文件系统报告数据区与空洞，并不真正改变"文件大小"语义。
- 不支持定位的对象（管道、socket）由 `FMODE_LSEEK` 拦截并返回 `-ESPIPE`。
- 相关：指定偏移读写的 [`preadv2`](./preadv2.md)、[`pwritev2`](./pwritev2.md) 不改变 `f_pos`。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE3(lseek, unsigned int, fd, off_t, offset, unsigned int, whence)
{
	return ksys_lseek(fd, offset, whence);
}
```

（`fs/read_write.c:412`）

用户态原型：

```c
#include <unistd.h>
off_t lseek(int fd, off_t offset, int whence);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `fd` | `unsigned int` | 已打开文件描述符 |
| `offset` | `off_t` | 相对 `whence` 的偏移，可为负 |
| `whence` | `unsigned int` | `SEEK_SET`(0)、`SEEK_CUR`(1)、`SEEK_END`(2)、`SEEK_DATA`(3)、`SEEK_HOLE`(4) |

`SEEK_MAX` 定义为 `SEEK_HOLE`（`include/uapi/linux/fs.h:57`）。

## 3. 内核实现路径

```
lseek(fd, offset, whence)                    fs/read_write.c:412
└─ ksys_lseek()                             fs/read_write.c:395
   ├─ CLASS(fd_pos, f)(fd)                   // fdget_pos()：必要时锁 f_pos_lock
   ├─ fd_empty() → -EBADF
   ├─ whence > SEEK_MAX → -EINVAL
   └─ vfs_llseek(file, offset, whence)       fs/read_write.c:387
      ├─ !(f_mode & FMODE_LSEEK) → -ESPIPE
      └─ file->f_op->llseek(file, offset, whence)
         ├─ generic_file_llseek()            fs/read_write.c:246
         │   └─ generic_file_llseek_size()   fs/read_write.c:160
         ├─ ext4_llseek()                    fs/ext4/file.c:1112
         ├─ ext4_dir_llseek()                fs/ext4/dir.c:397
         ├─ default_llseek()                 fs/read_write.c:332
         ├─ noop_llseek()                    fs/read_write.c:326
         └─ fixed_size_llseek() / no_seek_end_llseek()
```

核心代码（`fs/read_write.c:387`）：

```c
loff_t vfs_llseek(struct file *file, loff_t offset, int whence)
{
	if (!(file->f_mode & FMODE_LSEEK))
		return -ESPIPE;
	return file->f_op->llseek(file, offset, whence);
}
```

`ksys_lseek()` 还做了一次窄化检查：

```c
retval = -EINVAL;
if (whence <= SEEK_MAX) {
	loff_t res = vfs_llseek(fd_file(f), offset, whence);
	retval = res;
	if (res != (loff_t)retval)
		retval = -EOVERFLOW;	/* 仅 32 位平台可能发生 */
}
```

在 64 位 ARM64 上 `off_t` 与 `loff_t` 同为 64 位，该分支实际上不会触发；32 位 ABI 才有意义。

### 3.1 f_pos 的并发保护

`ksys_lseek()` 使用 `CLASS(fd_pos)`，它调用 `fdget_pos()`（`fs/file.c:1298`）。只有当 `file_needs_f_pos_lock()` 为真时才获取 `file->f_pos_lock`：

- 文件带 `FMODE_ATOMIC_POS`，且引用计数大于 1（`dup`/`fork` 共享），或
- 文件实现 `iterate_shared`（目录）。

单线程独占的普通文件走无锁快路径。

### 3.2 文件系统的 llseek 回调

**通用实现** `generic_file_llseek_size()`（`fs/read_write.c:160`）先由 `must_set_pos()`（`fs/read_write.c:104`）把 `whence` 折算成绝对偏移：

- `SEEK_END`：加上 `eof`
- `SEEK_CUR`：`offset == 0` 时直接返回当前 `f_pos`，不写回；否则加上 `f_pos`
- `SEEK_DATA`：`offset >= eof` 返回 `-ENXIO`，否则该偏移即数据
- `SEEK_HOLE`：`offset >= eof` 返回 `-ENXIO`，否则返回 `eof`

随后调用 `vfs_setpos()` → `vfs_setpos_cookie()`（`fs/read_write.c:55`）：拒绝负偏移（除非 `FOP_UNSIGNED_OFFSET`）和超过 `maxsize` 的偏移，并在位置变化时重置目录 cookie。

**ext4 常规文件** `ext4_llseek()`（`fs/ext4/file.c:1112`）对普通 whence 走 `generic_file_llseek_size()`，对 `SEEK_HOLE`/`SEEK_DATA` 则在共享 inode 锁下调用 `iomap_seek_hole()` / `iomap_seek_data()`，最后 `vfs_setpos()`。

**ext4 目录** `ext4_dir_llseek()`（`fs/ext4/dir.c:397`）对 htree 目录把偏移解释为文件名哈希，用 `generic_file_llseek_size(file, offset, whence, htree_max, htree_max)`，并重置 `dir_private_info->cookie`。

**其他回调**：`noop_llseek()` 直接返回 `f_pos`；`default_llseek()` 在 inode 锁下自行处理 whence；`fixed_size_llseek()` 限制固定大小设备；`no_seek_end_llseek()` 禁止 `SEEK_END`。

## 4. 核心数据结构

```c
/* include/linux/fs.h:1255 */
struct file {
	...
	union {
		struct mutex	f_pos_lock;	/* 常规文件/目录的 f_pos 保护 */
		u64		f_pipe;		/* 管道复用该字段 */
	};
	loff_t			f_pos;		/* 当前读写位置 */
	...
};

/* include/linux/fs.h:1193 —— 定位会影响预读状态时使用 */
struct file_ra_state {
	pgoff_t start;
	unsigned int size;
	unsigned int async_size;
	unsigned int ra_pages;
	unsigned int mmap_miss;
	loff_t prev_pos;
};
```

`file->f_op->llseek` 的签名：

```c
loff_t (*llseek)(struct file *, loff_t, int);
```

`FMODE_LSEEK` 在 `do_dentry_open()` 中默认置位（`fs/open.c:944`），若文件没有 `->llseek` 则被清除（`fs/open.c:959`）。

## 5. 关键流程

```
lseek(fd, off, SEEK_SET)
  │
  ├─ fdget_pos：共享文件时加 f_pos_lock
  ├─ 校验 whence <= SEEK_MAX
  ├─ vfs_llseek：校验 FMODE_LSEEK
  └─ f_op->llseek
       ├─ SEEK_SET：offset 即新位置
       ├─ SEEK_CUR：offset==0 直接返回 f_pos，否则 f_pos + offset
       ├─ SEEK_END：i_size_read(inode) + offset
       ├─ SEEK_DATA：定位到下一段数据，越界 -ENXIO
       └─ SEEK_HOLE：定位到下一段空洞，越界 -ENXIO
            │
            └─ vfs_setpos：拒绝负值/超上限，写回 file->f_pos
```

### 5.1 SEEK_CUR 的两种锁

`generic_file_llseek_size()` 对 `SEEK_CUR` 额外判断 `file_seek_cur_needs_f_lock()`（`fs/file.c:1284`）：如果文件没有 `FMODE_ATOMIC_POS` 且没有 `iterate_shared`，就可以不加 `f_lock`；否则在 `file->f_lock` 下完成 `f_pos + offset`，避免与同文件的并发 `SEEK_CUR` 竞争。

### 5.2 定位与预读

普通 `lseek` 不主动丢弃页缓存，也不重置 `file_ra_state`。随机访问模式需要 [`fadvise64`](./fadvise64.md) 的 `POSIX_FADV_RANDOM`，顺序访问可配合 `POSIX_FADV_SEQUENTIAL` 调整 `ra_pages`。

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `>= 0` | 新的文件偏移 |
| `-EBADF` | `fd` 无效 |
| `-EINVAL` | `whence` 非法；或结果为负且文件不支持无符号偏移；或超过文件系统上限 |
| `-ESPIPE` | 文件不支持定位（管道、socket、FIFO 等） |
| `-ENXIO` | `SEEK_DATA`/`SEEK_HOLE` 的起始偏移已到达或超过文件末尾 |
| `-EOVERFLOW` | 64 位结果无法放入 32 位 `off_t`（仅 32 位 ABI） |

注意 `lseek` 允许定位到文件末尾之外（形成空洞），因此 `offset > i_size` 本身不是错误。

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/tmp/lseek-demo", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { perror("open"); return 1; }

	write(fd, "0123456789", 10);

	off_t cur = lseek(fd, 0, SEEK_CUR);	/* 10，不改位置 */
	off_t end = lseek(fd, 0, SEEK_END);	/* 10 */
	off_t pos = lseek(fd, 4, SEEK_SET);	/* 4 */

	printf("cur=%lld end=%lld pos=%lld\n",
	       (long long)cur, (long long)end, (long long)pos);

	/* 探测数据/空洞；普通文件整段都是数据 */
	off_t data = lseek(fd, 0, SEEK_DATA);
	off_t hole = lseek(fd, 0, SEEK_HOLE);
	printf("data=%lld hole=%lld\n", (long long)data, (long long)hole);

	close(fd);
	return 0;
}
```

`SEEK_DATA`/`SEEK_HOLE` 在稀疏文件上更有意义，例如：

```c
off_t off = 0;
while (off < file_size) {
	off_t d = lseek(fd, off, SEEK_DATA);
	if (d < 0)
		break;			/* ENXIO：后面全是空洞 */
	off_t h = lseek(fd, d, SEEK_HOLE);
	printf("data [%lld, %lld)\n", (long long)d, (long long)h);
	off = h;
}
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/read_write.c` | `ksys_lseek()`（395）、`vfs_llseek()`（387）、`vfs_setpos_cookie()`（55）、`must_set_pos()`（104）、`generic_file_llseek_size()`（160）、`generic_file_llseek()`（246）、`default_llseek()`（332） |
| `fs/file.c` | `fdget_pos()`（1298）、`file_seek_cur_needs_f_lock()`（1284）、`file_needs_f_pos_lock()`（1273） |
| `fs/open.c` | `do_dentry_open()` 中 `FMODE_LSEEK` 的设置与清除（944 / 959） |
| `fs/ext4/file.c` | `ext4_llseek()`（1112） |
| `fs/ext4/dir.c` | `ext4_dir_llseek()`（397） |
| `include/linux/fs.h` | `struct file` 的 `f_pos`/`f_pos_lock`、`FMODE_LSEEK` |
| `include/uapi/linux/fs.h` | `SEEK_DATA`、`SEEK_HOLE`、`SEEK_MAX`（55-57） |
| `include/uapi/asm-generic/unistd.h` | `__NR3264_lseek`（170） |
