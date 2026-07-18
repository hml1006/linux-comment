# file_getattr 系统调用

## 1. 原理与功能

**原型：**
```c
int file_getattr(int dirfd, const char *pathname,
                 struct file_attr *ufattr, size_t usize,
                 unsigned int at_flags);
```

**功能：** 获取文件扩展属性（extended attributes），包括 FS_IOC_GETFLAGS 和 FS_IOC_FSGETXATTR 两种接口的联合。提供比 `stat()` 更丰富的文件属性信息，如不可变（immutable）、追加写（append-only）、项目 ID（project ID）等。

**参数：**
- `dirfd`: 目录 fd，`AT_FDCWD` 表示使用当前工作目录；`AT_EMPTY_PATH` 且 `pathname` 为空时通过 fd 操作
- `pathname`: 文件路径名（可为空，配合 `AT_EMPTY_PATH` 使用 fd）
- `ufattr`: 用户态 `struct file_attr` 缓冲区指针
- `usize`: 用户缓冲区大小（支持版本兼容，最小 `FILE_ATTR_SIZE_VER0` = 24 字节）
- `at_flags`: 标志位，支持 `AT_SYMLINK_NOFOLLOW` 和 `AT_EMPTY_PATH`

**支持的属性（通过 `fa_xflags` 字段）：**
| 标志 | 值 | 含义 |
|------|-----|------|
| `FS_XFLAG_REALTIME` | `0x00000001` | 实时卷数据 |
| `FS_XFLAG_PREALLOC` | `0x00000002` | 预分配文件块 |
| `FS_XFLAG_IMMUTABLE` | `0x00000008` | 文件不可修改 |
| `FS_XFLAG_APPEND` | `0x00000010` | 仅追加写 |
| `FS_XFLAG_SYNC` | `0x00000020` | 同步写 |
| `FS_XFLAG_NOATIME` | `0x00000040` | 不更新访问时间 |
| `FS_XFLAG_NODUMP` | `0x00000080` | 不备份 |
| `FS_XFLAG_PROJINHERIT` | `0x00000200` | 继承项目 ID |
| `FS_XFLAG_DAX` | `0x00008000` | 使用 DAX 模式 |

**注意：** `file_getattr` 和 `file_setattr` 是 Linux 5.17 引入的新系统调用，统一了 `FS_IOC_GETFLAGS`/`FS_IOC_SETFLAGS` 和 `FS_IOC_FSGETXATTR`/`FS_IOC_FSSETXATTR` 两种 ioctl 接口。

## 2. 执行流程

```
         file_getattr(dirfd, pathname, ufattr, usize, at_flags)
                               |
                     +---------v----------+
                     | 参数校验:           |
                     | at_flags 合法性     |
                     | usize 范围检查     |
                     | (>= 24, <= 4096)   |
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
                     | vfs_fileattr_get()  |  VFS 层获取文件属性
                     +---------+----------+
                               |
                +--------------+--------------+
                |              |              |
       +--------v-------+  +--v--------+  +--v--------+
       | ext4:          |  | xfs:      |  | btrfs:    |
       | ext4_fileattr_ |  | xfs_ioc_  |  | btrfs_    |
       | get()          |  | getxflags |  | fileattr_ |
       | 读取 inode     |  |           |  | get()     |
       | flags 和 xattr |  |           |  |           |
       +--------+-------+  +-----------+  +-----------+
                |
       +--------v-----------+
       | fileattr_to_file_  |
       | attr(&fa, &fattr)  |  内核 file_kattr -> 用户态 file_attr
       +--------+-----------+
                |
       +--------v-----------+
       | copy_struct_to_    |
       | user(ufattr, usize,|  复制到用户空间
       | &fattr, sizeof)    |
       +--------------------+
```

## 3. 函数调用栈

```
file_getattr()  [fs/file_attr.c]
  ├── BUILD_BUG_ON(sizeof(struct file_attr) 检查)  // 编译时结构体大小检查
  ├── 参数校验: at_flags, usize 范围
  ├── 路径解析:
  │     ├── [AT_EMPTY_PATH + null pathname]
  │     │     └── fd_file(f)->f_path  // 通过 fd 获取文件路径
  │     └── [普通路径]
  │           └── filename_lookup(dfd, name, lookup_flags, &filepath, NULL)
  ├── vfs_fileattr_get(filepath.dentry, &fa)  // VFS 层
  │     └── inode->i_op->fileattr_get(dentry, fa)  // 具体 FS 实现
  │           ├── ext4_fileattr_get()  // ext4 实现
  │           │     ├── ext4_ioctl_getflags()  // 读取 inode->i_flags
  │           │     └── ext4_ioctl_getxattr()  // 读取扩展属性
  │           ├── xfs_fileattr_get()   // xfs 实现
  │           └── btrfs_fileattr_get() // btrfs 实现
  ├── fileattr_to_file_attr(&fa, &fattr)  // 内核格式 -> 用户态格式
  └── copy_struct_to_user(ufattr, usize, &fattr, sizeof(fattr), NULL)
```

## 4. 关键数据结构

```c
// 用户态文件属性结构体（UAPI）
// 统一了 FS_IOC_GETFLAGS 和 FS_IOC_FSGETXATTR 两种接口
struct file_attr {
    __u64 fa_xflags;      // 扩展标志位（get/set），见 FS_XFLAG_* 定义
    __u32 fa_extsize;     // 建议的 extent 大小（get/set）
    __u32 fa_nextents;    // extent 数量（get only）
    __u32 fa_projid;      // 项目 ID（get/set），用于配额管理
    __u32 fa_cowextsize;  // CoW extent 大小（get/set）
};

// 版本兼容
#define FILE_ATTR_SIZE_VER0    24
#define FILE_ATTR_SIZE_LATEST  FILE_ATTR_SIZE_VER0

// 内核内部文件属性结构体
struct file_kattr {
    u32 flags;           // 传统 flags (FS_IOC_GETFLAGS)
    u32 fsx_xflags;      // 扩展 xflags
    u32 fsx_extsize;     // extent 大小
    u32 fsx_nextents;    // extents 数量
    u32 fsx_projid;      // 项目 ID
    u32 fsx_cowextsize;  // CoW extent 大小
    // 选择器（标记哪些字段有效）
    bool flags_valid:1;  // flags 字段有效
    bool fsx_valid:1;    // fsx 字段有效
};

// 转换函数
void fileattr_to_file_attr(const struct file_kattr *fa,
                           struct file_attr *fattr)
{
    fattr->fa_xflags = fa->fsx_xflags;
    fattr->fa_extsize = fa->fsx_extsize;
    fattr->fa_nextents = fa->fsx_nextents;
    fattr->fa_projid = fa->fsx_projid;
    fattr->fa_cowextsize = fa->fsx_cowextsize;
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

// 封装 syscall
#ifndef __NR_file_getattr
#define __NR_file_getattr 460  // ARM64
#endif

static inline int file_getattr(int dirfd, const char *pathname,
                               struct file_attr *fattr, size_t usize,
                               unsigned int flags)
{
    return syscall(__NR_file_getattr, dirfd, pathname, fattr, usize, flags);
}

int main(void)
{
    struct file_attr fattr = {};
    int ret;

    // === 示例1: 获取文件扩展属性 ===
    ret = file_getattr(AT_FDCWD, "/etc/passwd",
                       &fattr, sizeof(fattr), AT_SYMLINK_NOFOLLOW);
    if (ret == 0) {
        printf("xflags: 0x%llx\n", (unsigned long long)fattr.fa_xflags);
        printf("extsize: %u\n", fattr.fa_extsize);
        printf("nextents: %u\n", fattr.fa_nextents);
        printf("projid: %u\n", fattr.fa_projid);

        // 检查是否设置了不可变标志
        if (fattr.fa_xflags & FS_XFLAG_IMMUTABLE)
            printf("文件不可修改 (immutable)\n");
        if (fattr.fa_xflags & FS_XFLAG_APPEND)
            printf("仅追加写 (append-only)\n");
    } else {
        // 文件系统可能不支持
        printf("file_getattr: %s\n", strerror(errno));
    }

    // === 示例2: 通过 fd 获取（AT_EMPTY_PATH）===
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd >= 0) {
        ret = file_getattr(fd, NULL, &fattr, sizeof(fattr),
                           AT_EMPTY_PATH);
        if (ret == 0) {
            printf("通过 fd 获取: projid=%u\n", fattr.fa_projid);
        }
        close(fd);
    }

    // === 示例3: 检查项目 ID 配额 ===
    // 在支持项目配额的文件系统上（如 XFS）
    ret = file_getattr(AT_FDCWD, "/tmp/test.txt",
                       &fattr, sizeof(fattr), 0);
    if (ret == 0 && fattr.fa_projid > 0) {
        printf("文件项目 ID: %u\n", fattr.fa_projid);
    }

    return 0;
}
```

## 6. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件元数据与属性)
- [file_setattr.md](file_setattr.md)
- [stat.md](stat.md)
- 内核源码: `fs/file_attr.c` `include/uapi/linux/fs.h` `include/linux/fileattr.h`