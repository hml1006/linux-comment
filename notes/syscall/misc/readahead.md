# readahead 系统调用分析

## 1. 概述

`readahead` 系统调用用于预读文件数据到页缓存（page cache）中，以便后续的读操作能够直接从缓存中获取数据，无需等待磁盘 I/O。这是一种异步 I/O 优化技术，通过提前将文件内容加载到内存中，显著提升顺序读取的性能。

**原型：**

```c
SYSCALL_DEFINE3(readahead, int, fd, loff_t, offset, size_t, count);
```

- `fd`: 文件描述符，指向需要预读的文件。
- `offset`: 预读的起始偏移量（字节）。
- `count`: 要预读的字节数。

**返回值：** 成功时返回 0；失败时返回负的错误码。

## 2. 使用场景

- **数据库系统**：在扫描大量数据前，提前将数据页加载到缓存中
- **多媒体播放器**：提前加载后续文件块，避免播放卡顿
- **文件复制工具**：在读取源文件时，提前预读后续数据块
- **科学计算**：在处理大型数据文件时，确保数据已在缓存中
- **备份工具**：顺序读取大数据文件时减少磁盘 I/O 等待时间

## 3. 函数调用栈

```
readahead (系统调用入口)
└── ksys_readahead(fd, offset, count)
    ├── CLASS(fd, f)(fd)                    // 获取 fd 对应的 struct fd
    ├── fd_empty(f) 检查 → -EBADF
    ├── 检查 file->f_mode & FMODE_READ → -EBADF
    ├── 检查 file->f_mapping 和 a_ops → -EINVAL
    ├── 检查 inode 类型 (S_ISREG / S_ISBLK) → -EINVAL
    ├── 检查 IS_ANON_FILE → -EINVAL
    └── vfs_fadvise(file, offset, count, POSIX_FADV_WILLNEED)
        └── generic_fadvise(file, offset, len, POSIX_FADV_WILLNEED)
            └── 解析 offset 和 len，计算起始页和结束页
            └── force_page_cache_readahead(mapping, file, index, nr_to_read)
                └── force_page_cache_ra(&ractl, nr_to_read)
                    ├── page_cache_ra_unbounded(ractl, nr_to_read, 0)
                    │   ├── 分配 folio 并添加到 page cache
                    │   └── 遍历 nr_to_read 个页
                    └── read_pages(ractl)
                        └── mapping->a_ops->readahead(ractl)
                            └── (文件系统具体实现，如 ext4_mpage_readpages)
```

## 4. 关键数据结构

### struct file_ra_state（文件预读状态）

```c
struct file_ra_state {
    pgoff_t start;             /* 当前预读窗口的起始页 */
    unsigned int size;         /* 预读窗口大小（页数） */
    unsigned int async_size;   /* 异步预读区域大小（触发下一次预读的阈值） */
    unsigned int ra_pages;     /* 文件系统最大预读页数 */
    unsigned int mmap_miss;    /* mmap 预读缺失计数 */
    loff_t prev_pos;           /* 上次读取位置（用于检测顺序访问） */
    unsigned int order;        /* 预读 folio 的 order */
};
```

**预读窗口示意图：**

```
        |<----- async_size ---------|
        |------------------- size ------------------->|
        |==================#===========================|
        ^start             ^page marked with PG_readahead
                           （触发异步预读的位置）
```

### ksys_readahead 核心实现

```c
ssize_t ksys_readahead(int fd, loff_t offset, size_t count)
{
    struct file *file;
    const struct inode *inode;

    CLASS(fd, f)(fd);
    if (fd_empty(f))
        return -EBADF;

    file = fd_file(f);
    if (!(file->f_mode & FMODE_READ))
        return -EBADF;

    /* 仅支持常规文件和块设备 */
    if (!file->f_mapping || !file->f_mapping->a_ops)
        return -EINVAL;

    inode = file_inode(file);
    if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
        return -EINVAL;
    if (IS_ANON_FILE(inode))
        return -EINVAL;

    return vfs_fadvise(fd_file(f), offset, count, POSIX_FADV_WILLNEED);
}
```

### 强制预读函数

```c
void force_page_cache_ra(struct readahead_control *ractl,
                         unsigned long nr_to_read)
{
    struct address_space *mapping = ractl->mapping;

    /* 如果文件太大，限制预读范围 */
    if (nr_to_read > mapping->host->i_size >> PAGE_SHIFT)
        nr_to_read = mapping->host->i_size >> PAGE_SHIFT;

    page_cache_ra_unbounded(ractl, nr_to_read, 0);
    /* 0 表示 lookahead_size = 0，全部为同步预读 */
}
```

## 5. 流程图

```
用户态: readahead(fd, offset, count)
                              │
                              ▼
                    ┌───────────────────────┐
                    │  ksys_readahead()      │
                    │                        │
                    │  检查 fd 有效性        │
                    │  检查 FMODE_READ       │
                    │  检查 inode 类型       │
                    │  检查 anon 文件        │
                    └───────────┬───────────┘
                                │
                                ▼
                    ┌───────────────────────┐
                    │  vfs_fadvise(WILLNEED) │
                    └───────────┬───────────┘
                                │
                                ▼
                    ┌───────────────────────┐
                    │  generic_fadvise()     │
                    │                        │
                    │  解析 offset/len       │
                    │  计算 start_index      │
                    │  计算 nr_pages         │
                    └───────────┬───────────┘
                                │
                                ▼
                    ┌───────────────────────┐
                    │  force_page_cache_ra() │
                    └───────────┬───────────┘
                                │
                                ▼
                    ┌───────────────────────┐
                    │  page_cache_ra_unbounded() │
                    │                        │
                    │  循环:                 │
                    │  ┌─────────────────┐   │
                    │  │ 分配 folio       │   │
                    │  │ filemap_add_folio│   │
                    │  │ (添加到 page    │   │
                    │  │  cache)         │   │
                    │  └────────┬────────┘   │
                    │           │            │
                    │           重复直到      │
                    │           nr_to_read    │
                    └───────────┬───────────┘
                                │
                                ▼
                    ┌───────────────────────┐
                    │  read_pages()          │
                    │  → a_ops->readahead()  │
                    │  → 提交 I/O 请求       │
                    └───────────────────────┘
```

## 6. 错误处理

| 错误码 | 触发条件 |
|--------|---------|
| `-EBADF` | 文件描述符无效，或文件未以读模式打开 |
| `-EINVAL` | 文件不支持预读（无 `f_mapping` 或 `a_ops`） |
| `-EINVAL` | 文件不是常规文件或块设备 |
| `-EINVAL` | 文件是匿名文件（`IS_ANON_FILE`） |

**注意：** `readahead` 系统调用本身不返回预读过程中发生的 I/O 错误。这是因为预读是一种"尽力而为"的优化操作——即使部分页面预读失败，后续的 `read()` 调用仍会尝试通过 `->read_folio()` 读取它们。

## 7. 使用示例

### 示例 1: 基本预读

```c
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int fd = open("/path/to/large/file", O_RDONLY);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    /* 预读文件前 1MB 数据 */
    if (readahead(fd, 0, 1024 * 1024) < 0) {
        perror("readahead");
        close(fd);
        exit(1);
    }

    /* 后续的 read() 将从 page cache 中获取数据，速度更快 */
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
        perror("read");
        close(fd);
        exit(1);
    }

    close(fd);
    return 0;
}
```

### 示例 2: 大文件分块预读

```c
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int fd = open("/path/to/huge/file", O_RDONLY);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    off_t file_size = lseek(fd, 0, SEEK_END);
    off_t offset;
    const size_t chunk_size = 4 * 1024 * 1024;  /* 每次预读 4MB */

    /* 分块预读整个文件 */
    for (offset = 0; offset < file_size; offset += chunk_size) {
        size_t count = (file_size - offset < chunk_size) ?
                       file_size - offset : chunk_size;
        if (readahead(fd, offset, count) < 0) {
            perror("readahead");
            break;
        }
        printf("Readahead at offset %ld, size %zu\n", offset, count);
    }

    close(fd);
    return 0;
}
```

### 示例 3: 使用 fadvise 的等效操作

```c
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int fd = open("/path/to/file", O_RDONLY);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    /* readahead(fd, 0, 65536) 等效于: */
    posix_fadvise(fd, 0, 65536, POSIX_FADV_WILLNEED);

    /* 告诉内核我们不再需要这些缓存数据 */
    posix_fadvise(fd, 0, 65536, POSIX_FADV_DONTNEED);

    close(fd);
    return 0;
}
```

## 8. 性能考虑

- **同步 vs 异步**：`readahead()` 调用本身是同步的（它触发 I/O 后立即返回，不等待 I/O 完成），但预读的 I/O 操作是异步提交的
- **缓存影响**：预读的数据会占用 page cache，如果预读过多可能挤占其他进程的缓存
- **内核自动预读**：Linux 内核的 `page_cache_sync_ra()` 和 `page_cache_async_ra()` 会自动进行顺序预读优化，因此大多数应用无需手动调用 `readahead()`
- **文件系统差异**：不同文件系统（ext4、btrfs、xfs）的 `readahead` 实现效率不同
- **块设备支持**：`readahead` 也支持块设备（如 `/dev/sda`），但大多数场景下用于常规文件

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 内核源码: `mm/readahead.c`
- 内核源码: `mm/fadvise.c` (vfs_fadvise 实现)
- 内核源码: `include/linux/fs.h` (struct file_ra_state)
- `man readahead(2)` / `man posix_fadvise(2)`