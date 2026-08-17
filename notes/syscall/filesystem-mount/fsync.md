# fsync 系统调用分析

## 1. 概述

将文件所有数据和元数据同步到磁盘（完整同步）。与 `fdatasync` 的区别在于，`fsync` 会同步所有元数据（包括 atime、mtime 等），确保文件完全持久化。

**原型：**

```c
SYSCALL_DEFINE1(fsync, unsigned int, fd)
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

- **数据库事务提交**: 确保事务日志和数据文件完全持久化
- **关键文件写入**: 配置文件、密码文件等写入后立即同步
- **邮件系统**: 确保邮件投递后文件已写入磁盘

## 3. 函数调用栈

```
fsync(fd) (系统调用入口)
└─ do_fsync(fd, 0)                                    // fs/sync.c (datasync=0)
   ├─ fdget(fd)                                        // 获取 fd 对应的 file 结构
   ├─ file->f_op->fsync(file, start, end, 0)           // datasync=0
   │    └─ ext4_sync_file(file, 0)                     // ext4 实现
   │         ├─ filemap_write_and_wait_range()          // 等待脏页写回
   │         ├─ ext4_sync_inode(file, 0)               // 同步完整 inode 元数据
   │         │    └─ datasync=0 → 同步所有元数据
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
fsync(fd)
  │
  v
do_fsync(fd, 0)  // datasync=0
  │
  ├─ fdget(fd) → 获取 file 对象
  │
  └─ ext4_sync_file(file, 0)
       │
       ├─ filemap_write_and_wait_range()
       │    ├─ 遍历脏页链表
       │    └─ 提交写回请求
       │
       ├─ ext4_sync_inode(file, 0)
       │    └─ 同步完整元数据（包括 atime/mtime）
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

    const char *data = "critical data";
    write(fd, data, strlen(data));

    // 完整同步（数据和所有元数据）
    if (fsync(fd) == -1) {
        perror("fsync");
        return 1;
    }

    printf("Data and metadata synced\n");
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — fsync/fdatasync 实现
- `fs/ext4/fsync.c` — ext4 同步实现
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)