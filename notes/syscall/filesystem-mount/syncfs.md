# syncfs 系统调用分析

## 1. 概述

刷新指定文件描述符所在文件系统的所有缓冲区到磁盘，类似 `sync` 但只作用于一个文件系统。

**原型：**

```c
SYSCALL_DEFINE1(syncfs, int, fd)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符（其所在文件系统将被同步） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的文件描述符
  - `-EIO` — I/O 错误

## 2. 使用场景

- **数据库**: 确保特定文件系统的数据持久化
- **外部存储卸载**: 卸载 USB 设备前同步其文件系统
- **文件系统管理**: 只同步指定文件系统，避免全面同步的性能开销

## 3. 函数调用栈

```
syncfs(fd) (系统调用入口)
└─ ksys_syncfs(fd)                                    // fs/sync.c
   ├─ fdget(fd)                                        // 获取 file 对象
   ├─ sb = file->f_path.dentry->d_sb                   // 获取超级块
   │
   └─ sync_filesystem(sb)                              // 同步文件系统
        ├─ filemap_write_and_wait(sb->s_mapping)        // 写回脏页
        ├─ sb->s_op->sync_fs(sb, 0)                    // 文件系统同步
        └─ sync_blockdev(sb->s_bdev)                   // 同步块设备
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
syncfs(fd)
  │
  ├─ fdget(fd) → 获取 file 对象
  │
  ├─ file->f_path.dentry->d_sb → 获取超级块
  │
  └─ sync_filesystem(sb)
       ├─ filemap_write_and_wait()  // 写回所有脏页
       ├─ sync_fs(sb, 0)            // 文件系统元数据同步
       └─ sync_blockdev()           // 同步块设备缓存
```

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

int main(void)
{
    int fd = open("/mnt/usb/test.txt", O_RDONLY);
    if (fd == -1) { perror("open"); return 1; }

    // 同步 /mnt/usb 所在文件系统
    if (syncfs(fd) == -1) {
        perror("syncfs");
        return 1;
    }
    printf("File system synced\n");

    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — syncfs 实现
- `include/linux/fs.h` — super_block 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)