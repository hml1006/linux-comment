# fstatat 系统调用

## 原理与功能

`fstatat` 是一个 POSIX 标准系统调用，用于获取相对于目录文件描述符的文件状态信息。在 ARM64 架构上，`fstatat` 没有独立的系统调用编号，而是通过 `newfstatat`（syscall #79）实现。

### 功能说明

- 获取文件或目录的状态信息（权限、大小、时间戳、inode 号等）
- 支持 `AT_EMPTY_PATH`、`AT_SYMLINK_NOFOLLOW` 等标志位
- 与 `stat` 的区别在于可以指定目录文件描述符，避免 TOCTOU 竞争条件
- 内部调用 `vfs_fstatat` → `vfs_statx` 实现，统一使用 `struct kstat` 内核态数据结构

### 使用场景

- 需要相对于某个目录而非当前工作目录获取文件状态
- 在 `O_PATH` 文件描述符上使用 `AT_EMPTY_PATH` 获取状态
- 避免符号链接跟随的场景
- libc 中 `fstatat()` 和 `stat()` 的底层实现

## API 及使用案例

### 函数原型

```c
#include <fcntl.h>
#include <sys/stat.h>

int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `dirfd` | `int` | 目录文件描述符，`AT_FDCWD` 表示当前工作目录 |
| `pathname` | `const char*` | 文件路径，可为空（配合 `AT_EMPTY_PATH`） |
| `statbuf` | `struct stat*` | 输出缓冲区，存储文件状态信息 |
| `flags` | `int` | 标志位，如 `AT_SYMLINK_NOFOLLOW`(0x100)、`AT_EMPTY_PATH`(0x1000) |

### 返回值

- 成功返回 0，`statbuf` 被填充
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    struct stat sb;

    // 示例1: 基本用法（等价于 stat()）
    if (fstatat(AT_FDCWD, "/etc/passwd", &sb, 0) == 0) {
        printf("文件大小: %ld 字节\n", sb.st_size);
        printf("inode: %lu\n", sb.st_ino);
        printf("权限: %o\n", sb.st_mode & 07777);
    }

    // 示例2: 不跟随符号链接
    if (fstatat(AT_FDCWD, "/tmp/symlink", &sb, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(sb.st_mode))
            printf("这是一个符号链接本身的信息\n");
    }

    // 示例3: 通过目录 fd 获取相对路径状态
    int dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) {
        if (fstatat(dirfd, "test.txt", &sb, 0) == 0) {
            printf("/tmp/test.txt 大小: %ld\n", sb.st_size);
        }
        close(dirfd);
    }

    return 0;
}
```

## 执行流程

```
fstatat(dirfd, pathname, statbuf, flags)
  └─ syscall(__NR_newfstatat, dirfd, pathname, statbuf, flags)
       └─ __arm64_sys_newfstatat()                     // arch/arm64/kernel/syscall.c
            └─ vfs_fstatat(dfd, filename, &stat, flags)  // fs/stat.c:365
                 ├─ filename 为空且 dfd >= 0?
                 │    ├─ 是 → vfs_fstat(dfd, &stat)     // 直接通过 fd 获取
                 │    └─ 否 → vfs_statx(dfd, name, flags | AT_NO_AUTOMOUNT,
                 │                        &stat, STATX_BASIC_STATS)  // fs/stat.c:293
                 │             ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 │             │    └─ 路径解析（根据 dirfd 和 flags 查找路径）
                 │             └─ vfs_statx_path(&path, flags, &stat, request_mask)
                 │                  └─ vfs_getattr(&path, &stat, request_mask, flags)
                 │                       └─ inode->i_op->getattr(&path, &stat, ...)
                 │                            └─ ext4_getattr()  // EXT4 示例
                 │                                 ├─ generic_fillattr()  // 通用字段
                 │                                 └─ ext4_fillattr()     // FS 特定字段
                 │
                 └─ cp_new_stat(&stat, statbuf)       // 内核 kstat → 用户态 stat
                      └─ copy_to_user(statbuf, &tmp, sizeof(tmp))  // 拷贝到用户空间
```

## 函数调用栈

```
fstatat()  (glibc wrapper)
  └─ syscall(__NR_newfstatat, dirfd, pathname, statbuf, flags)
       └─ __arm64_sys_newfstatat()                    // arch/arm64/kernel/syscall.c
            └─ vfs_fstatat(dfd, filename, &stat, flags)  // fs/stat.c:365
                 ├─ vfs_statx(dfd, name, flags, &stat, STATX_BASIC_STATS)  // fs/stat.c:293
                 │    ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 │    └─ vfs_getattr(&path, &stat, request_mask, flags)
                 │         └─ inode->i_op->getattr()
                 │              └─ ext4_getattr()  // fs/ext4/inode.c
                 │                   ├─ generic_fillattr(idmap, request_mask, inode, stat)
                 │                   └─ ext4_fillattr(inode, stat)
                 └─ cp_new_stat(&stat, statbuf)        // fs/stat.c
                      └─ copy_to_user(statbuf, &tmp, sizeof(tmp))
```

## 关键数据结构

### struct kstat（内核文件状态）

```c
// include/linux/stat.h
struct kstat {
    u32             mask;          // 有效字段掩码
    u64             ino;           // inode 号
    dev_t           dev;           // 文件所在设备号
    dev_t           rdev;          // 特殊文件设备号（如设备节点）
    umode_t         mode;          // 文件类型和权限
    unsigned int    nlink;         // 硬链接数
    uid_t           uid;           // 所有者的 UID
    gid_t           gid;           // 所有者的 GID
    loff_t          size;          // 文件大小
    struct timespec64 atime;       // 最后访问时间
    struct timespec64 mtime;       // 最后修改时间
    struct timespec64 ctime;       // 最后状态变更时间
    u64             blksize;       // 首选 I/O 块大小
    u64             blocks;        // 分配的 512 字节块数
    u64             result_mask;   // 实际返回的字段掩码
};
```

### struct stat（用户空间文件状态）

```c
// include/uapi/asm-generic/stat.h
struct stat {
    unsigned long   st_dev;        // 设备号
    unsigned long   st_ino;        // inode 号
    unsigned int    st_mode;       // 文件类型和权限
    unsigned int    st_nlink;      // 硬链接数
    unsigned int    st_uid;        // 所有者 UID
    unsigned int    st_gid;        // 所有者 GID
    unsigned long   st_rdev;       // 特殊设备号
    unsigned long   st_size;       // 文件大小
    unsigned long   st_blksize;    // I/O 块大小
    unsigned long   st_blocks;     // 块数
    struct timespec st_atim;       // 访问时间
    struct timespec st_mtim;       // 修改时间
    struct timespec st_ctim;       // 状态变更时间
};
```

### vfs_fstatat 实现分析

```c
// fs/stat.c:365
int vfs_fstatat(int dfd, const char __user *filename,
                struct kstat *stat, int flags)
{
    CLASS(filename_maybe_null, name)(filename, flags);

    // 处理 AT_EMPTY_PATH：filename 为空时直接通过 fd 获取
    if (!name && dfd >= 0)
        return vfs_fstat(dfd, stat);

    // 否则走标准路径查找流程
    return vfs_statx(dfd, name, flags | AT_NO_AUTOMOUNT,
                     stat, STATX_BASIC_STATS);
}
```

## 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EACCES` | 权限不足 | 对路径中某个目录无搜索权限 |
| `EBADF` | 无效 fd | `dirfd` 不是有效文件描述符 |
| `EFAULT` | 地址错误 | `pathname` 或 `statbuf` 指向不可访问地址 |
| `ELOOP` | 符号链接循环 | 路径解析时遇到过多符号链接 |
| `ENAMETOOLONG` | 路径名过长 | `pathname` 超过 `PATH_MAX` |
| `ENOENT` | 文件不存在 | 路径中某个组件不存在 |
| `ENOMEM` | 内存不足 | 内核内存分配失败 |
| `ENOTDIR` | 不是目录 | 路径中某个组件不是目录 |
| `EOVERFLOW` | 溢出 | 32 位兼容模式下的字段溢出 |

## 备注

- ARM64 上无独立的 `fstatat` 系统调用号，使用 `newfstatat`（#79）替代
- `fstatat` 是 `stat()` 和 `lstat()` 的 at 系列扩展版本
- 对于新代码，推荐使用 `statx()` 系统调用，其功能更丰富且接口更稳定
- `AT_EMPTY_PATH` 标志允许通过 `O_PATH` 类型的 fd 获取文件状态
- 内部调用链：`fstatat` → `newfstatat` → `vfs_fstatat` → `vfs_statx` → `vfs_getattr` → 文件系统实现

## 参考

- 内核源码: `fs/stat.c` (`SYSCALL_DEFINE4(newfstatat)`, `vfs_fstatat`, `vfs_statx`)
- `include/linux/stat.h` — `struct kstat` 定义
- `include/uapi/asm-generic/stat.h` — `struct stat` 定义
- [statx.md](statx.md) — 推荐使用的增强版 stat 系统调用
- [newfstatat.md](newfstatat.md) — ARM64 上的实际实现