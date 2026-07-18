# stat 系统调用

## 原理与功能

`stat` 系统调用用于获取文件或目录的状态信息，包括文件类型、权限、大小、时间戳、设备号、inode 号等。它是 Linux 文件系统中最基本的元数据查询接口。

在 ARM64 架构上，`stat` 没有独立的系统调用号，通过 `newfstatat`（syscall #79）实现。glibc 封装层将 `stat()` 调用转换为 `newfstatat(AT_FDCWD, path, buf, 0)`。

### 功能说明

- 获取文件元数据（大小、权限、时间戳等）
- 跟随符号链接（获取目标文件而非链接本身的信息）
- 返回 `struct stat` 结构体

## 使用场景

- 检查文件是否存在（如 `ls -l` 的内部实现）
- 获取文件大小（如 `du` 命令）
- 检查文件类型（普通文件、目录、设备等）
- 获取文件时间戳（如 `stat` 命令）
- 权限检查（如 `access` 命令的替代方案）

## API 及使用案例

### 函数原型

```c
#include <sys/stat.h>
#include <unistd.h>

int stat(const char *pathname, struct stat *statbuf);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `pathname` | `const char*` | 文件路径 |
| `statbuf` | `struct stat*` | 输出缓冲区，存储文件状态信息 |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`（如 `ENOENT` 文件不存在、`EACCES` 无权限）

### 使用示例

```c
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

int main() {
    struct stat sb;

    if (stat("/etc/passwd", &sb) == -1) {
        perror("stat");
        return 1;
    }

    printf("文件大小: %ld 字节\n", sb.st_size);
    printf("块数: %ld\n", sb.st_blocks);
    printf("权限: %o\n", sb.st_mode & 07777);
    printf("inode: %lu\n", sb.st_ino);
    printf("硬链接数: %lu\n", sb.st_nlink);
    printf("最后访问: %s", ctime(&sb.st_atime));
    printf("最后修改: %s", ctime(&sb.st_mtime));
    printf("状态改变: %s", ctime(&sb.st_ctime));

    if (S_ISREG(sb.st_mode))
        printf("文件类型: 普通文件\n");
    else if (S_ISDIR(sb.st_mode))
        printf("文件类型: 目录\n");

    return 0;
}
```

## 执行流程

```
用户进程                         内核
    |                              |
    | stat("/etc/passwd", &buf)    |
    |-----> syscall(#79) --------->|
    |       newfstatat(AT_FDCWD,   |
    |         "/etc/passwd",       |
    |         &stat, 0)            |
    |                              |
    |    +------------------+      |
    |    |  do_statx()      |      |
    |    |  fs/stat.c       |      |
    |    +--------+---------+      |
    |             |                |
    |    +--------v---------+      |
    |    |  vfs_statx()     |      |
    |    |  VFS 层入口      |      |
    |    +--------+---------+      |
    |             |                |
    |    +--------v---------+      |
    |    | filename_lookup()|      |
    |    | 路径解析与查找   |      |
    |    +--------+---------+      |
    |             |                |
    |    +--------v---------+      |
    |    | vfs_getattr()    |      |
    |    | VFS 获取属性     |      |
    |    +--------+---------+      |
    |             |                |
    |    +--------v---------+      |
    |    | ext4_getattr()   |      |
    |    | 具体文件系统实现  |      |
    |    +--------+---------+      |
    |             |                |
    |    +--------v---------+      |
    |    | generic_fillattr()|      |
    |    | 填充 kstat 结构   |      |
    |    +--------+---------+      |
    |             |                |
    |    +--------v---------+      |
    |    | cp_newstat()     |      |
    |    | 拷贝到用户空间   |      |
    |    +--------+---------+      |
    |             |                |
    |<---- 返回 0 ----------------+|
    |                              |
```

## 函数调用栈

```
stat()  (glibc wrapper)
  └─ syscall(__NR_newfstatat, AT_FDCWD, pathname, &stat, 0)
       └─ __arm64_sys_newfstatat()              // arch/arm64/kernel/syscall.c
            └─ do_statx(dfd, pathname, flags, 0, &stat)
                 └─ vfs_statx(dfd, pathname, flags, &stat, request_mask)
                      └─ vfs_getattr_nosec(&path, &stat, request_mask, flags)
                           └─ vfs_getattr(&path, &stat, request_mask, flags)
                                └─ inode->i_op->getattr(&path, &stat, request_mask, flags)
                                     └─ ext4_getattr()  // 以 ext4 为例
                                          └─ generic_fillattr(idmap, request_mask, inode, &stat)
                 └─ cp_newstat(&stat, statbuf)  // 内核→用户空间拷贝
```

## 关键数据结构

### struct kstat（内核内部文件状态）

```c
// include/linux/stat.h
struct kstat {
    u32             mask;          // 有效字段掩码（哪些字段已填充）
    u64             ino;           // inode 号
    dev_t           dev;           // 文件所在设备号
    dev_t           rdev;          // 特殊文件设备号
    umode_t         mode;          // 文件类型和权限位
    unsigned int    nlink;         // 硬链接计数
    uid_t           uid;           // 所有者 UID
    gid_t           gid;           // 所属组 GID
    loff_t          size;          // 文件大小（字节）
    struct timespec64 atime;       // 最后访问时间
    struct timespec64 mtime;       // 最后修改时间
    struct timespec64 ctime;       // 状态改变时间
    u64             blocks;        // 文件占用的块数（512 字节为单位）
    u64             blksize;       // 首选 I/O 块大小
};
```

### struct stat（用户空间可见）

```c
// include/uapi/asm-generic/stat.h
struct stat {
    unsigned long   st_dev;        // 设备号
    unsigned long   st_ino;        // inode 号
    unsigned int    st_mode;       // 文件类型和权限
    unsigned int    st_nlink;      // 硬链接数
    unsigned int    st_uid;        // 所有者 UID
    unsigned int    st_gid;        // 所属组 GID
    unsigned long   st_rdev;       // 特殊设备号
    unsigned long   __pad1;
    long            st_size;       // 文件大小
    int             st_blksize;    // 块大小
    int             __pad2;
    long            st_blocks;     // 占用的 512 字节块数
    struct timespec st_atim;       // 最后访问时间
    struct timespec st_mtim;       // 最后修改时间
    struct timespec st_ctim;       // 状态改变时间
    int             __glibc_reserved[2];
};
```

## 备注

- ARM64 上无独立 `stat` 系统调用号，通过 `newfstatat(AT_FDCWD, ...)` 实现
- 与 `fstat` 的区别：`stat` 通过路径查找，`fstat` 通过文件描述符
- 与 `lstat` 的区别：`stat` 跟随符号链接，`lstat` 不跟随（通过 `AT_SYMLINK_NOFOLLOW` 标志）
- 存在 TOCTOU（Time-of-Check-Time-of-Use）竞态条件问题