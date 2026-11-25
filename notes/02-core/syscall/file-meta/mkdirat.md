# mkdirat 系统调用分析

## 1. 原理与功能

**mkdirat** 用于创建目录，是 `mkdir(2)` 的 `at` 系列扩展版本。它允许通过目录文件描述符指定相对路径。

**ARM64 系统调用号：** 34 (__NR_mkdirat)

**原型：**

```c
int mkdirat(int dirfd, const char *pathname, mode_t mode);
```

**参数说明：**
- `dirfd`：目录文件描述符，`AT_FDCWD` 表示当前工作目录
- `pathname`：要创建的目录路径
- `mode`：目录权限模式（受 umask 影响）

**核心行为：**
- 创建名为 `pathname` 的新目录
- 新目录的 inode 类型为 `S_IFDIR`
- 实际权限 = `mode & ~umask`
- 父目录的 `i_nlink` 会增加（因为新目录包含 `.` 条目）
- 需要父目录的写权限和搜索权限

## 2. 执行流程

```
                mkdirat(dirfd, pathname, mode)
                               |
                     +---------v----------+
                     |  filename_mkdirat() |  fs/namei.c
                     |  CLASS(name)        |  用户态文件名拷贝
                     +---------+----------+
                               |
                     +---------v----------+
                     |  vfs_mkdir()        |  VFS 层
                     |  mnt_want_write()   |  检查文件系统可写
                     |  security_inode_    |  LSM 安全检查
                     |  mkdir()            |
                     |  i_op->mkdir()      |  文件系统实现
                     |  (如 ext4_mkdir)     |
                     |  创建新 inode       |
                     |  添加 . 和 .. 条目  |
                     |  更新父目录 i_nlink  |
                     |  mnt_drop_write()   |
                     +---------+----------+
                               |
                     +---------v----------+
                     |  返回 0 成功/错误码  |
                     +--------------------+
```

## 3. 函数调用栈

```
mkdirat (用户态)
  └─ syscall(__NR_mkdirat, dirfd, pathname, mode)
       └─ __arm64_sys_mkdirat()
            └─ filename_mkdirat(dfd, name, mode)
                 ├─ filename_create(dfd, name, LOOKUP_DIRECTORY, &path, &error)
                 │    └─ path_openat() 路径解析
                 ├─ mnt_want_write(path.mnt)
                 ├─ security_inode_mkdir(dir, dentry, mode)
                 ├─ vfs_mkdir(idmap, dir, dentry, mode)
                 │    └─ i_op->mkdir(idmap, dir, dentry, mode)
                 │         └─ ext4_mkdir()  // ext4 实现
                 │              ├─ ext4_new_inode_start()  // 分配新 inode
                 │              ├─ ext4_mark_inode_dirty()
                 │              ├─ ext4_add_entry(dentry, inode)  // 添加 . 条目
                 │              ├─ ext4_add_entry(dentry, parent)  // 添加 .. 条目
                 │              ├─ inode_inc_link_count(inode)     // . 增加链接
                 │              ├─ inode_inc_link_count(dir)       // 父目录 .. 增加
                 │              └─ d_instantiate_new(dentry, inode) // 关联 dentry
                 ├─ mnt_drop_write(path.mnt)
                 └─ path_put(&path)
```

## 4. 关键数据结构

```c
// 目录 inode 操作 (include/linux/fs.h)
struct inode_operations {
    int (*mkdir)(struct mnt_idmap *idmap, struct inode *dir,
                 struct dentry *dentry, umode_t mode);
    // ...
};

// 目录项定义 (include/linux/dcache.h)
struct dentry {
    struct qstr d_name;          // 目录项名称
    struct inode *d_inode;       // 指向的 inode
    struct dentry *d_parent;     // 父目录项
    // ...
};

// 新目录的默认权限掩码
// 典型 mode = 0755 (rwxr-xr-x)
// 实际权限 = mode & ~umask  (如 umask=022, 实际=0755)
#define S_IRWXU 00700  // 所有者 rwx
#define S_IRUSR 00400  // 所有者 r
#define S_IWUSR 00200  // 所有者 w
#define S_IXUSR 00100  // 所有者 x
#define S_IRWXG 00070  // 组 rwx
#define S_IRWXO 00007  // 其他 rwx

// 文件类型掩码
#define S_IFDIR 0040000  // 目录文件类型
```

## 5. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 示例1: 在 /tmp 下创建目录
    if (mkdirat(AT_FDCWD, "/tmp/newdir", 0755) == 0) {
        printf("成功创建目录 /tmp/newdir (0755)\n");
    } else {
        perror("mkdirat");
    }

    // 示例2: 通过目录 fd 创建相对路径的目录
    int dir_fd = open("/tmp/mydir", O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        // 创建 /tmp/mydir/subdir
        if (mkdirat(dir_fd, "subdir", 0700) == 0) {
            printf("通过 dir_fd 成功创建子目录 subdir\n");
        }
        close(dir_fd);
    }

    // 示例3: 创建多级目录 (需要逐级创建)
    const char *path = "/tmp/a/b/c";
    // 注意: mkdirat 不会自动创建中间目录
    // 需要先创建 /tmp/a, 再 /tmp/a/b, 最后 /tmp/a/b/c
    if (mkdirat(AT_FDCWD, "/tmp/a", 0755) == 0 &&
        mkdirat(AT_FDCWD, "/tmp/a/b", 0755) == 0 &&
        mkdirat(AT_FDCWD, "/tmp/a/b/c", 0755) == 0) {
        printf("成功创建多级目录\n");
    }

    // 示例4: 使用不同权限模式
    // 私密目录 (仅所有者可访问)
    if (mkdirat(AT_FDCWD, "/tmp/private", 0700) == 0) {
        printf("成功创建私密目录\n");
    }
    // 共享目录 (所有人可读写)
    if (mkdirat(AT_FDCWD, "/tmp/shared", 0777) == 0) {
        printf("成功创建共享目录\n");
    }

    exit(EXIT_SUCCESS);
}
```