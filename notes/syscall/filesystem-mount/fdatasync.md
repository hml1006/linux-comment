# fdatasync 系统调用分析

## 1. 概述

将文件数据同步到磁盘（仅同步必要元数据）。与 `fsync` 的区别在于，`fdatasync` 不强制同步不需要用于后续读取的元数据（如 `st_atime`），因此性能更好。

**原型：**

```c
SYSCALL_DEFINE1(fdatasync, unsigned int, fd)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的文件描述符
  - `-EIO` — I/O 错误
  - `-EINVAL` — 无效参数

## 2. 使用场景

- **数据库事务提交**: 确保 WAL 日志写入磁盘（如 SQLite 的 `PRAGMA synchronous = FULL`）
- **关键数据持久化**: 配置文件写入后确保不丢失
- **日志系统**: 保证日志记录已持久化

## 3. 函数调用栈

```
fdatasync(fd) (系统调用入口)
└─ do_fsync(fd, 1)                                    // fs/sync.c (datasync=1)
   ├─ fdget(fd)                                        // 获取 fd 对应的 file 结构
   ├─ file->f_op->fsync(file, start, end, 1)           // datasync=1
   │    └─ ext4_sync_file(file, 1)                     // ext4 实现
   │         ├─ filemap_write_and_wait_range()          // 等待脏页写回
   │         ├─ ext4_sync_inode(file, 1)               // 同步必要元数据
   │         │    └─ datasync=1 → 跳过 atime 等元数据
   │         └─ jbd2 事务提交
   └─ fdput(fd)                                        // 释放 fd 引用
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
fdatasync(fd)
  │
  v
do_fsync(fd, 1)  // datasync=1
  │
  ├─ fdget(fd) → 获取 file 对象
  │
  └─ ext4_sync_file(file, 1)
       │
       ├─ filemap_write_and_wait_range()
       │    ├─ 遍历脏页链表
       │    └─ 提交写回请求
       │
       ├─ ext4_sync_inode(file, 1)
       │    └─ 仅同步必要元数据（跳过 atime）
       │
       └─ jbd2 事务提交
            └─ 等待日志刷写到磁盘
```

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    int fd = open("/tmp/test.dat", O_WRONLY | O_CREAT, 0644);
    if (fd == -1) { perror("open"); return 1; }

    const char *data = "important data";
    write(fd, data, strlen(data));

    // 只同步数据和必要元数据（比 fsync 快）
    if (fdatasync(fd) == -1) {
        perror("fdatasync");
        return 1;
    }

    printf("Data synced\n");
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — fdatasync/fsync 实现
- `fs/ext4/fsync.c` — ext4 同步实现
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)