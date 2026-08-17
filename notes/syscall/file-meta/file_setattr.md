# file_setattr 系统调用

## 1. 原理与功能

**原型：**
```c
int file_setattr(int dirfd, const char *pathname,
                 struct file_attr *ufattr, size_t usize,
                 unsigned int at_flags);
```

**功能：** 设置文件扩展属性，是 `file_getattr` 的对应写操作。统一了传统的 `FS_IOC_SETFLAGS` 和 `FS_IOC_FSSETXATTR` ioctl 接口。

**参数：**
- `dirfd`: 目录 fd，`AT_FDCWD` 表示使用当前工作目录；`AT_EMPTY_PATH` 且 `pathname` 为空时通过 fd 操作
- `pathname`: 文件路径名（可为空，配合 `AT_EMPTY_PATH` 使用 fd）
- `ufattr`: 用户态 `struct file_attr` 缓冲区指针，包含要设置的属性
- `usize`: 用户缓冲区大小
- `at_flags`: 标志位，支持 `AT_SYMLINK_NOFOLLOW` 和 `AT_EMPTY_PATH`

**可以设置的属性：**
- `fa_xflags`: 文件扩展标志，如 `FS_XFLAG_IMMUTABLE`（不可变）、`FS_XFLAG_APPEND`（仅追加）
- `fa_extsize`: 建议的 extent 分配大小
- `fa_projid`: 项目 ID（配额管理）
- `fa_cowextsize`: CoW extent 大小

**权限要求：**
- 设置 `FS_XFLAG_IMMUTABLE` 或 `FS_XFLAG_APPEND` 需要 `CAP_LINUX_IMMUTABLE` 权限
- 修改其他属性需要文件所有者或 `CAP_FOWNER` 权限
- 设置 `fa_projid` 需要 `CAP_SYS_ADMIN` 或配额管理权限

**ARM64 系统调用号：** `__NR_file_setattr` (461)

## 2. 执行流程

```
         file_setattr(dirfd, pathname, ufattr, usize, at_flags)
                               |
                     +---------v----------+
                     | 参数校验:           |
                     | at_flags 合法性     |
                     | usize 范围检查     |
                     | (>= 24, <= 4096)   |
                     +---------+----------+
                               |
                     +---------v----------+
                     | copy_struct_from_  |  从用户空间拷贝
                     | user(&fattr, ...)  |  file_attr 结构体
                     +---------+----------+
                               |
                     +---------v----------+
                     | file_attr_to_      |  用户态 -> 内核态
                     | fileattr(&fattr,   |  格式转换
                     |   &fa)             |
                     +---------+----------+
                               |
                     +---------v----------+
                     | 选择路径解析方式:  |
                     +---------+----------+
                               |
               +---------------+---------------+
               |                               |
     +---------v----------+          +---------v----------+
     | pathname 为空      |          | pathname 不为空     |
     | 且 AT_EMPTY_PATH  |          | filename_lookup()  |
     | fd_file(f)->f_path |          | 查找路径           |
     +---------+----------+          +---------+----------+
               |                               |
               +---------------+---------------+
                               |
                     +---------v----------+
                     | vfs_fileattr_set()  |  VFS 层设置文件属性
                     +---------+----------+
                               |
                +--------------+--------------+
                |              |              |
       +--------v-------+  +--v--------+  +--v--------+
       | ext4:          |  | xfs:      |  | btrfs:    |
       | ext4_fileattr_ |  | xfs_ioc_  |  | btrfs_    |
       | set()          |  | setxflags |  | fileattr_ |
       | 写入 inode     |  |           |  | set()     |
       | flags 和 xattr |  |           |  |           |
       +--------+-------+  +-----------+  +-----------+
                |
       +--------v-----------+
       | 通过 inode->i_op->  |  写入成功
       | setattr /          |  返回 0
       | setflags 等        |
       +--------------------+
```

## 3. 函数调用栈

```
file_setattr()  [fs/file_attr.c]
  ├── BUILD_BUG_ON(sizeof(struct file_attr) 检查)  // 编译时结构体大小检查
  ├── 参数校验: at_flags, usize 范围
  ├── copy_struct_from_user(&fattr, sizeof(fattr), ufattr, usize)  // 从用户态拷贝
  ├── file_attr_to_fileattr(&fattr, &fa)  // 转换并校验
  │     ├── 检查 fa_xflags 中的未知位
  │     └── 填充 file_kattr 结构体
  ├── 路径解析:
  │     ├── [AT_EMPTY_PATH + null pathname]
  │     │     └── fd_file(f)->f_path  // 通过 fd 获取文件路径
  │     └── [普通路径]
  │           └── filename_lookup(dfd, name, lookup_flags, &filepath, NULL)
  ├── mnt_want_write(filepath.mnt)  // 获取写权限
  ├── vfs_fileattr_set(filepath.dentry, &fa)  // VFS 层
  │     ├── inode_permission(...)  // 检查文件所有者/CAP_FOWNER
  │     └── inode->i_op->fileattr_set(dentry, fa)  // 具体 FS 实现
  │           ├── ext4_fileattr_set()  // ext4 实现
  │           │     ├── ext4_ioctl_setflags()  // 写入 inode->i_flags
  │           │     └── ext4_ioctl_setxattr()  // 写入扩展属性
  │           ├── xfs_fileattr_set()   // xfs 实现
  │           └── btrfs_fileattr_set() // btrfs 实现
  └── mnt_drop_write(filepath.mnt)  // 释放写权限
```

## 4. 关键数据结构

```c
// 用户态文件属性结构体（见 file_getattr 的详细注释）
struct file_attr {
    __u64 fa_xflags;      // 扩展标志位（set），见 FS_XFLAG_* 定义
    __u32 fa_extsize;     // 建议的 extent 大小（set）
    __u32 fa_nextents;    // 预留（get only，set 时忽略）
    __u32 fa_projid;      // 项目 ID（set）
    __u32 fa_cowextsize;  // CoW extent 大小（set）
};

// 可设置的 xflags 标志位
#define FS_XFLAG_IMMUTABLE    0x00000008  // 文件不可修改（需要 CAP_LINUX_IMMUTABLE）
#define FS_XFLAG_APPEND       0x00000010  // 仅追加写（需要 CAP_LINUX_IMMUTABLE）
#define FS_XFLAG_SYNC         0x00000020  // 所有写入同步
#define FS_XFLAG_NOATIME      0x00000040  // 不更新访问时间
#define FS_XFLAG_NODUMP       0x00000080  // 不包含在备份中
#define FS_XFLAG_PROJINHERIT  0x00000200  // 子文件继承项目 ID
#define FS_XFLAG_NOSYMLINKS   0x00000400  // 禁止创建符号链接
#define FS_XFLAG_EXTSIZE      0x00000800  // extent 分配大小提示
#define FS_XFLAG_DAX          0x00008000  // 使用 DAX 模式

// 内核内部文件属性结构体
struct file_kattr {
    u32 flags;           // 传统 flags
    u32 fsx_xflags;      // 扩展 xflags（来自 fa_xflags）
    u32 fsx_extsize;     // 建议的 extent 大小
    u32 fsx_nextents;    // (忽略)
    u32 fsx_projid;      // 项目 ID
    u32 fsx_cowextsize;  // CoW extent 大小
    // 选择器
    bool flags_valid:1;  // 是否设置了 flags
    bool fsx_valid:1;    // 是否设置了 fsx 字段
};

// 转换函数（用户态 -> 内核态）
int file_attr_to_fileattr(const struct file_attr *fattr,
                          struct file_kattr *fa)
{
    // 检查 fa_xflags 中是否有未知位
    if (fattr->fa_xflags & ~FS_XFLAG_ALL)
        return -EINVAL;

    // 如果 fa_xflags 非零，标记 fsx 字段有效
    if (fattr->fa_xflags) {
        fa->fsx_valid = true;
        fa->fsx_xflags = fattr->fa_xflags;
    }

    // 如果 fa_extsize 或 fa_projid 或 fa_cowextsize 非零，标记 fsx 有效
    // 如果 fa_flags（传统标志）非零，标记 flags 有效
    // ...
    return 0;
}
```

## 5. 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <errno.h>
#include <string.h>

#ifndef __NR_file_setattr
#define __NR_file_setattr 461  // ARM64
#endif

static inline int file_setattr(int dirfd, const char *pathname,
                               const struct file_attr *fattr, size_t usize,
                               unsigned int flags)
{
    return syscall(__NR_file_setattr, dirfd, pathname, fattr, usize, flags);
}

int main(void)
{
    struct file_attr fattr = {};
    int ret;

    // === 示例1: 设置文件不可变标志（需要 root）===
    fattr.fa_xflags = FS_XFLAG_IMMUTABLE;
    ret = file_setattr(AT_FDCWD, "/tmp/test.txt",
                       &fattr, sizeof(fattr), 0);
    if (ret == 0) {
        printf("已设置不可变标志\n");
        // 现在文件不可修改、删除、重命名
    } else {
        printf("设置不可变失败: %s\n", strerror(errno));
        // 预期: EPERM (非 root) 或 EOPNOTSUPP (FS 不支持)
    }

    // === 示例2: 设置仅追加标志 ===
    memset(&fattr, 0, sizeof(fattr));
    fattr.fa_xflags = FS_XFLAG_APPEND;
    ret = file_setattr(AT_FDCWD, "/tmp/test.log",
                       &fattr, sizeof(fattr), 0);
    if (ret == 0) {
        printf("已设置仅追加标志\n");
        // 日志文件只能追加写，不能覆盖或删除
    }

    // === 示例3: 清除不可变标志（同样需要 root）===
    memset(&fattr, 0, sizeof(fattr));
    fattr.fa_xflags = 0;  // 清除所有 xflags
    // 注意: 需要先清除 FS_XFLAG_IMMUTABLE 才能修改文件
    ret = file_setattr(AT_FDCWD, "/tmp/test.txt",
                       &fattr, sizeof(fattr), 0);
    if (ret == 0) {
        printf("已清除不可变标志\n");
    }

    // === 示例4: 设置项目 ID（配额管理）===
    memset(&fattr, 0, sizeof(fattr));
    fattr.fa_projid = 100;  // 设置项目 ID 为 100
    ret = file_setattr(AT_FDCWD, "/tmp/project_file.txt",
                       &fattr, sizeof(fattr), 0);
    if (ret == 0) {
        printf("已设置项目 ID 为 100\n");
    } else {
        printf("设置项目 ID 失败: %s\n", strerror(errno));
        // 可能需要 FS 支持（如 XFS）和权限
    }

    // === 示例5: 通过 fd 设置 ===
    int fd = open("/tmp/test.txt", O_RDONLY);
    if (fd >= 0) {
        memset(&fattr, 0, sizeof(fattr));
        fattr.fa_xflags = FS_XFLAG_NODUMP;  // 设置不备份标志
        ret = file_setattr(fd, NULL, &fattr, sizeof(fattr), AT_EMPTY_PATH);
        close(fd);
    }

    return 0;
}
```

## 6. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件元数据与属性)
- [file_getattr.md](file_getattr.md)
- [chattr(1) 手册](https://man7.org/linux/man-pages/man1/chattr.1.html)
- 内核源码: `fs/file_attr.c` `include/uapi/linux/fs.h` `include/linux/fileattr.h`