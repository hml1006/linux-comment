# sync_file_range 系统调用分析

## 1. 概述

同步文件指定范围内的数据到磁盘，提供比 `fsync`/`fdatasync` 更细粒度的同步控制。

**原型：**

```c
SYSCALL_DEFINE4(sync_file_range, int, fd, loff_t, offset, loff_t, nbytes,
        unsigned int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符 |
| `offset` | `loff_t` | 同步起始偏移 |
| `nbytes` | `loff_t` | 同步字节数（0 表示从 offset 到文件末尾） |
| `flags` | `unsigned int` | 控制标志（见下表） |

**flags 标志：**

| 标志 | 值 | 说明 |
|------|-----|------|
| `SYNC_FILE_RANGE_WAIT_BEFORE` | 1 | 写入前等待页面的脏数据回写完成 |
| `SYNC_FILE_RANGE_WRITE` | 2 | 发起脏页的写回请求 |
| `SYNC_FILE_RANGE_WAIT_AFTER` | 4 | 写入后等待所有写回完成 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的 fd
  - `-EINVAL` — 无效的 flags
  - `-EIO` — I/O 错误

## 2. 使用场景

- **数据库预写日志**: 仅同步 WAL 文件的特定区域
- **大文件增量同步**: 分批同步大文件的已修改区域
- **零拷贝数据传输**: 在 `splice` 等操作后同步特定区域

## 3. 函数调用栈

```
sync_file_range(fd, offset, nbytes, flags) (系统调用入口)
└─ ksys_sync_file_range(fd, offset, nbytes, flags)   // fs/sync.c
   ├─ fdget(fd)                                        // 获取 file 对象
   ├─ file_write_and_wait_range(file, offset, endbyte)  // 先等待正在写回的页
   │    └─ __filemap_fdatawait_range(mapping, offset, endbyte)
   │
   ├─ filemap_fdatawrite_range(mapping, offset, endbyte) // 发起写回
   │    └─ do_writepages(mapping, &wbc)                 // 页面写回
   │
   └─ filemap_fdatawait_range(mapping, offset, endbyte)  // 等待写回完成
        └─ wait_on_page_writeback_range()
```

## 4. 关键数据结构

```c
// ===== struct file (文件对象, include/linux/fs.h) =====
struct file {
    struct path f_path;                    // 文件路径
    struct inode *f_inode;                 // 指向 inode
    const struct file_operations *f_op;    // 文件操作函数集
    atomic_long_t f_count;                 // 引用计数
    unsigned int f_flags;                  // 打开标志
    loff_t f_pos;                          // 文件偏移
    struct address_space *f_mapping;       // 地址空间映射
};

// ===== struct address_space (地址空间, include/linux/fs.h) =====
struct address_space {
    struct inode *host;                    // 所属 inode
    struct xarray i_pages;                 // 页缓存基数树
    unsigned long nrpages;                 // 页缓存页数
    const struct address_space_operations *a_ops;  // 地址空间操作
};

// ===== struct super_block 超级块 (见 mount 相关文档) =====
```

## 5. 流程图

```
sync_file_range(fd, offset, nbytes, flags)
  │
  ├─ [WAIT_BEFORE]  → 等待 offset~offset+nbytes 的脏页写回完成
  │
  ├─ [WRITE]        → 发起 offset~offset+nbytes 的脏页写回
  │
  └─ [WAIT_AFTER]   → 等待写回完成
```

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    int fd = open("/tmp/largefile.dat", O_WRONLY | O_CREAT, 0644);
    if (fd == -1) { perror("open"); return 1; }

    char buf[4096] = {0};
    for (int i = 0; i < 1000; i++) {
        write(fd, buf, 4096);
    }

    // 分步同步：
    // 1. 等待前 1MB 的写回完成
    sync_file_range(fd, 0, 1024*1024, SYNC_FILE_RANGE_WAIT_BEFORE);
    // 2. 发起前 1MB 的写回
    sync_file_range(fd, 0, 1024*1024, SYNC_FILE_RANGE_WRITE);
    // 3. 等待写回完成
    sync_file_range(fd, 0, 1024*1024, SYNC_FILE_RANGE_WAIT_AFTER);

    printf("First 1MB synced\n");
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — sync_file_range 实现
- `mm/page-writeback.c` — 页写回核心逻辑
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)