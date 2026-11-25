# unlinkat 系统调用分析

## 1. 原理与功能

**unlinkat** 用于删除目录项（文件或目录），是 `unlink(2)` 和 `rmdir(2)` 的 `at` 系列扩展版本。它统一实现了删除文件和删除目录的功能。

**ARM64 系统调用号：** 35 (__NR_unlinkat)

**原型：**

```c
int unlinkat(int dirfd, const char *pathname, int flags);
```

**参数说明：**
- `dirfd`：目录文件描述符，用于相对路径解析
- `pathname`：要删除的文件或目录路径
- `flags`：控制标志位
  - `AT_REMOVEDIR`（0x200）：删除目录（相当于 rmdir）
  - 0：删除文件（相当于 unlink）

**核心行为：**
- `flags = 0`：删除文件，减少 inode 的硬链接计数
- `flags = AT_REMOVEDIR`：删除空目录
- 删除操作会减少目标文件的硬链接计数（`i_nlink`），当 `i_nlink` 降为 0 时，文件数据会被最终回收

## 2. 执行流程

```
                unlinkat(dirfd, pathname, flags)
                               |
                     +---------v----------+
                     |  验证 flags          |
                     |  & ~AT_REMOVEDIR    |
                     +---------+----------+
                               |
              +----------------+----------------+
              |                                 |
     +--------v--------+              +--------v--------+
     | flags=0:        |              | AT_REMOVEDIR:   |
     | unlink 操作      |              | rmdir 操作       |
     | filename_unlinkat|              | filename_rmdir  |
     +--------+--------+              +--------+--------+
              |                                 |
              +----------+----------------------+
                         |
              +----------v-----------+
              |  vfs_unlink/rmdir     |  VFS 层
              |  mnt_want_write()     |  检查可写
              |  security_inode_      |  LSM 检查
              |  unlink/rmdir()       |
              |  i_op->unlink/rmdir() |  文件系统实现
              |  减少 i_nlink         |
              |  mnt_drop_write()     |
              +----------+-----------+
                         |
              +----------v-----------+
              |  返回 0 成功/错误码    |
              +----------------------+
```

## 3. 函数调用栈

```
unlinkat (用户态)
  └─ syscall(__NR_unlinkat, dirfd, pathname, flags)
       └─ __arm64_sys_unlinkat()
            └─ if (flags & AT_REMOVEDIR)
                    return filename_rmdir(dfd, name)
                 else
                    return filename_unlinkat(dfd, name)

    // 删除文件路径
    filename_unlinkat(dfd, name)
         └─ do_unlinkat(dfd, name)
              ├─ filename_lookup(dfd, name, LOOKUP_PARENT, &path, &error)
              ├─ mnt_want_write(path.mnt)
              ├─ security_inode_unlink(dir, dentry)
              ├─ vfs_unlink(idmap, dir, dentry, NULL)
              │    └─ i_op->unlink(dir, dentry)
              │         └─ ext4_unlink()  // 具体文件系统实现
              │              ├─ ext4_delete_entry()  // 删除目录项
              │              └─ inode_dec_link_count()  // 减少 i_nlink
              ├─ mnt_drop_write(path.mnt)
              └─ path_put(&path)

    // 删除目录路径
    filename_rmdir(dfd, name)
         └─ do_rmdir(dfd, name)
              ├─ filename_lookup(dfd, name, LOOKUP_PARENT, &path, &error)
              ├─ mnt_want_write(path.mnt)
              ├─ security_inode_rmdir(dir, dentry)
              ├─ vfs_rmdir(idmap, dir, dentry)
              │    └─ i_op->rmdir(dir, dentry)
              │         └─ ext4_rmdir()  // 具体文件系统实现
              ├─ mnt_drop_write(path.mnt)
              └─ path_put(&path)
```

## 4. 关键数据结构

```c
// AT_REMOVEDIR 标志 (include/uapi/linux/fcntl.h)
#define AT_REMOVEDIR       0x200   // 删除目录 (rmdir 语义)

// inode 链接计数 (include/linux/fs.h)
struct inode {
    nlink_t         i_nlink;     // 硬链接数
    // 当 i_nlink == 0 时，文件系统会回收 inode 和数据块
    // ...
};

// dentry 状态标志 (include/linux/dcache.h)
struct dentry {
    unsigned int d_flags;        // 目录项标志
    // DCACHE_DISCONNECTED: 目录项未连接
    // ...
};

// 目录项操作 (include/linux/fs.h)
struct inode_operations {
    int (*unlink)(struct inode *dir, struct dentry *dentry);
    int (*rmdir)(struct inode *dir, struct dentry *dentry);
    // ...
};
```

## 5. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 示例1: 删除文件 (同 unlink)
    if (unlinkat(AT_FDCWD, "/tmp/test.txt", 0) == 0) {
        printf("成功删除文件\n");
    } else {
        perror("unlinkat (unlink)");
    }

    // 示例2: 删除空目录 (同 rmdir)
    if (unlinkat(AT_FDCWD, "/tmp/emptydir", AT_REMOVEDIR) == 0) {
        printf("成功删除空目录\n");
    } else {
        perror("unlinkat (rmdir)");
    }

    // 示例3: 通过文件描述符删除相对路径
    int dir_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        if (unlinkat(dir_fd, "test_in_tmp.txt", 0) == 0) {
            printf("通过 dir_fd 成功删除 /tmp/test_in_tmp.txt\n");
        }
        close(dir_fd);
    }

    // 示例4: 同时删除多个文件
    const char *files[] = {"file1.txt", "file2.txt", "file3.txt"};
    int dir_fd2 = open("/tmp/batch", O_RDONLY | O_DIRECTORY);
    if (dir_fd2 >= 0) {
        for (int i = 0; i < 3; i++) {
            if (unlinkat(dir_fd2, files[i], 0) == 0) {
                printf("成功删除 %s\n", files[i]);
            }
        }
        close(dir_fd2);
    }

    exit(EXIT_SUCCESS);
}
```