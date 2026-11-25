# fadvise64 系统调用

> 所属分类：文件 I/O | [ARM64 系统调用表](../arm64-syscall-table.md#2-文件-io)

## 1. 概述

`fadvise64` 向内核提供文件访问模式的**建议**（advice）。它不是语义性操作：不保证数据一定被读入或丢弃，只是调整预读窗口、随机/顺序标志，或主动触发预读、回写与页缓存回收。

- ARM64 编号 **223**。`include/uapi/asm-generic/unistd.h:572` 把 `__NR3264_fadvise64` 用 `__SC_COMP` 映射到 `sys_fadvise64_64`，因此该架构实际编译的是 `fadvise64_64`；`fadvise64` 本体只出现在定义 `__ARCH_WANT_SYS_FADVISE64` 的架构（`mm/fadvise.c:205`）。
- 当前 `.config` 中 `CONFIG_ADVISE_SYSCALLS=y`，系统调用已启用。
- 建议作用于 `struct file` 与它背后的 `address_space`/`backing_dev_info`，不改变文件内容。
- 相关：`lseek` 改变位置，`fadvise` 改变内核对该位置的 I/O 预期。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE4(fadvise64_64, int, fd, loff_t, offset, loff_t, len, int, advice)
{
	return ksys_fadvise64_64(fd, offset, len, advice);
}

#ifdef __ARCH_WANT_SYS_FADVISE64
SYSCALL_DEFINE4(fadvise64, int, fd, loff_t, offset, size_t, len, int, advice)
{
	return ksys_fadvise64_64(fd, offset, len, advice);
}
#endif
```

（`mm/fadvise.c:200`、`mm/fadvise.c:207`）

用户态原型：

```c
#define _GNU_SOURCE
#include <fcntl.h>
int posix_fadvise(int fd, off_t offset, off_t len, int advice);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `fd` | `int` | 已打开文件描述符 |
| `offset` | `loff_t` | 建议区间起始偏移，必须非负 |
| `len` | `loff_t` | 区间长度；`0` 表示"到文件末尾/尽可能多" |
| `advice` | `int` | `POSIX_FADV_*` |

`include/uapi/linux/fadvise.h` 的取值：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `POSIX_FADV_NORMAL` | 恢复默认预读窗口，清除随机/一次性标志 |
| 1 | `POSIX_FADV_RANDOM` | 随机访问，关闭预读 |
| 2 | `POSIX_FADV_SEQUENTIAL` | 顺序访问，预读窗口加倍 |
| 3 | `POSIX_FADV_WILLNEED` | 提前把区间读入页缓存 |
| 4 | `POSIX_FADV_DONTNEED` | 尝试丢弃区间内的干净页缓存 |
| 5 | `POSIX_FADV_NOREUSE` | 数据只访问一次（标记 `FMODE_NOREUSE`） |

（s390x 的 `DONTNEED`/`NOREUSE` 为 6/7，见同一头文件的条件编译。）

## 3. 内核实现路径

```
fadvise64_64(fd, offset, len, advice)        mm/fadvise.c:200
└─ ksys_fadvise64_64()                      mm/fadvise.c:190
   ├─ CLASS(fd, f)(fd)
   ├─ fd_empty() → -EBADF
   └─ vfs_fadvise(file, offset, len, advice) mm/fadvise.c:179
      ├─ file->f_op->fadvise()               // 文件系统自定义
      └─ generic_fadvise()                   mm/fadvise.c:31
         ├─ S_ISFIFO → -ESPIPE
         ├─ !mapping || len < 0 || offset < 0 → -EINVAL
         ├─ DAX 或 noop_backing_dev_info → 忽略建议，返回 0
         ├─ NORMAL/RANDOM/SEQUENTIAL/NOREUSE → 调整 f_ra / f_mode
         ├─ WILLNEED → force_page_cache_readahead()
         └─ DONTNEED → filemap_flush_range() + 失效页缓存
            ├─ lru_add_drain()
            ├─ mapping_try_invalidate()
            └─ 失败时 lru_add_drain_all() + invalidate_mapping_pages()
```

核心代码（`mm/fadvise.c:179`）：

```c
int vfs_fadvise(struct file *file, loff_t offset, loff_t len, int advice)
{
	if (file->f_op->fadvise)
		return file->f_op->fadvise(file, offset, len, advice);

	return generic_fadvise(file, offset, len, advice);
}
```

### 3.1 区间端点计算

`generic_fadvise()` 先把 `[offset, offset+len)` 转成闭区间 `endbyte`，并用无符号运算避免有符号溢出（`mm/fadvise.c:72`）：

```c
endbyte = (u64)offset + (u64)len;
if (!len || endbyte < len)
	endbyte = LLONG_MAX;
else
	endbyte--;		/* inclusive */
```

`len == 0` 或加法溢出都表示"到尽可能远的末尾"。

### 3.2 各建议的实际动作

- `NORMAL`：`file->f_ra.ra_pages = bdi->ra_pages`，清除 `FMODE_RANDOM | FMODE_NOREUSE`。
- `RANDOM`：置 `FMODE_RANDOM`，预读器据此减少或关闭预读。
- `SEQUENTIAL`：`ra_pages = bdi->ra_pages * 2`，并清除 `FMODE_RANDOM`。
- `WILLNEED`：按页对齐计算页数，调用 `force_page_cache_readahead()` 主动预读。
- `NOREUSE`：置 `FMODE_NOREUSE`，提示后续回收优先。
- `DONTNEED`：先 `filemap_flush_range()` 回写脏页；只处理**完整页**（部分页保留），再尝试 `mapping_try_invalidate()`。若页还挂在远端 CPU 的 LRU 缓存中导致失败，则 `lru_add_drain_all()` 后重试 `invalidate_mapping_pages()`。

DAX 文件或没有真实后备设备的映射直接忽略建议（返回 0），对未知 `advice` 返回 `-EINVAL`。

## 4. 核心数据结构

```c
/* include/linux/fs.h:1193 */
struct file_ra_state {
	pgoff_t start;		/* 最近一次预读起点 */
	unsigned int size;	/* 预读页数 */
	unsigned int async_size;
	unsigned int ra_pages;	/* 预读窗口，fadvise 直接修改 */
	unsigned int mmap_miss;
	loff_t prev_pos;
};

/* include/linux/fs.h:1255 */
struct file {
	...
	struct address_space	*f_mapping;	/* 页缓存映射 */
	struct file_ra_state	f_ra;		/* 与 f_task_work/f_llist 共用联合体 */
	...
};

/* include/linux/backing-dev.h —— 预读窗口默认值来源 */
struct backing_dev_info {
	...
	unsigned int ra_pages;
	...
};
```

`address_space` 提供 `mapping_try_invalidate()` / `invalidate_mapping_pages()` 所需的页缓存索引；`inode_to_bdi(mapping->host)` 取得后备设备信息。

## 5. 关键流程

```
posix_fadvise(fd, off, len, POSIX_FADV_DONTNEED)
  │
  ├─ ksys_fadvise64_64：fd → struct file
  ├─ vfs_fadvise：优先 f_op->fadvise
  └─ generic_fadvise
       ├─ 计算 [off, endbyte]
       ├─ filemap_flush_range：把脏页回写
       ├─ 起点向上取整到完整页，终点向下调整
       ├─ lru_add_drain：把本地 per-CPU 页刷入 LRU
       ├─ mapping_try_invalidate：丢弃干净页
       └─ 若有失败：lru_add_drain_all + invalidate_mapping_pages
```

`WILLNEED` 的流程相反：计算页范围后直接调用 `force_page_cache_readahead()` 发起同步/异步预读，不等待 I/O 完成。

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `0` | 建议已被接受（不代表一定执行） |
| `-EBADF` | `fd` 无效 |
| `-EINVAL` | `offset < 0`、`len < 0`、未知 `advice`、`mapping` 为空 |
| `-ESPIPE` | 对象是 FIFO |
| `-ENOMEM` | 预读或内部数据结构分配失败 |

注意：glibc 的 `posix_fadvise()` 返回错误码本身（而非 `-1` 并设置 `errno`），直接使用 `syscall(SYS_fadvise64, ...)` 时才看到负返回值。

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/var/tmp/bigfile", O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }

	/* 告诉内核：接下来顺序读，提前读入 1 MiB */
	if (posix_fadvise(fd, 0, 1 << 20, POSIX_FADV_SEQUENTIAL) != 0)
		perror("SEQUENTIAL");
	if (posix_fadvise(fd, 0, 1 << 20, POSIX_FADV_WILLNEED) != 0)
		perror("WILLNEED");

	/* 用完这段后丢弃页缓存，降低内存压力 */
	if (posix_fadvise(fd, 0, 1 << 20, POSIX_FADV_DONTNEED) != 0)
		perror("DONTNEED");

	/* 随机访问模式 */
	posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);

	close(fd);
	return 0;
}
```

直接使用系统调用（ARM64 上 223 号）：

```c
#include <sys/syscall.h>
#include <unistd.h>

long ret = syscall(SYS_fadvise64, fd, 0, len, POSIX_FADV_NORMAL);
if (ret < 0)
	perror("fadvise64");
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `mm/fadvise.c` | `generic_fadvise()`（31）、`vfs_fadvise()`（179）、`ksys_fadvise64_64()`（190）、`SYSCALL_DEFINE4(fadvise64_64)`（200）、`fadvise64`（207） |
| `include/uapi/linux/fadvise.h` | `POSIX_FADV_*` 取值 |
| `include/uapi/asm-generic/unistd.h` | `__NR3264_fadvise64`（572）及 `__SC_COMP` 映射 |
| `include/linux/fs.h` | `struct file`、`struct file_ra_state` |
| `include/linux/backing-dev.h` | `struct backing_dev_info::ra_pages` |
| `mm/readahead.c` | `force_page_cache_readahead()` |
| `mm/truncate.c` | `invalidate_mapping_pages()`、`mapping_try_invalidate()` |
| `mm/swap.c` | `lru_add_drain()`、`lru_add_drain_all()` |
