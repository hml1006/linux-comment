# sync 系统调用分析

## 1. 概述

刷新所有文件系统缓冲区到磁盘。遍历所有已挂载的超级块，将脏页和元数据写入磁盘。

**原型：**

```c
SYSCALL_DEFINE0(sync)
```

**参数：** 无

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EIO` — I/O 错误

## 2. 使用场景

- **系统关机/重启**: 关机前刷新所有缓存
- **`sync` 命令**: 手动触发全局同步
- **紧急数据持久化**: 在关键操作后确保所有数据落盘

## 3. 函数调用栈

```
sync() (系统调用入口)
└─ ksys_sync()                                        // fs/sync.c
   └─ iterate_supers(sync_fs_one, sb)                  // 遍历所有超级块
        └─ sync_fs_one(sb, ...)
             ├─ sync_filesystem(sb)                    // 同步单个文件系统
             │    ├─ sb->s_op->sync_fs(sb, 0)          // 文件系统同步
             │    ├─ writeback_inodes_sb(sb, WB_REASON_SYNC)  // 写回脏 inode
             │    └─ wait_sb_inodes(sb)                // 等待所有写回完成
             └─ sync_blockdev(sb->s_bdev)              // 同步块设备缓存
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
sync()
  │
  v
ksys_sync()
  │
  └─ iterate_supers(sync_fs_one)
       │
       └─ for each super_block:
            │
            ├─ sync_filesystem(sb)
            │    ├─ sync_fs(sb, 0)            // 元数据同步
            │    ├─ writeback_inodes_sb()     // 脏页写回
            │    └─ wait_sb_inodes()          // 等待完成
            │
            └─ sync_blockdev(sb->s_bdev)     // 块设备缓存同步
```

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    // 同步所有文件系统
    sync();
    printf("All file systems synced\n");
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — sync 实现
- `include/linux/fs.h` — super_block 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)