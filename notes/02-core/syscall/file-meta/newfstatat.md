# newfstatat 系统调用

## 原理与功能

`newfstatat` 是 ARM64 架构上 `stat` 系列系统调用的核心实现，通过一个系统调用统一实现了 `stat`、`fstat`、`lstat` 三个 POSIX 函数的功能。它根据 `dirfd` 和 `flags` 参数的不同组合，实现不同的行为。

### 功能说明

- 获取相对于目录 fd 的文件状态信息
- 支持 `AT_EMPTY_PATH`：通过 fd 获取状态（替代 `fstat`）
- 支持 `AT_SYMLINK_NOFOLLOW`：不跟随符号链接（替代 `lstat`）
- 使用 `AT_FDCWD` 作为 dirfd 时，路径相对于当前工作目录（替代 `stat`）

## 使用场景

- 实现 `stat()`、`fstat()`、`lstat()` 三个 POSIX 函数的底层调用
- 需要避免 TOCTOU 竞态条件的精确路径操作
- 相对于指定目录的文件状态查询

## API 及使用案例

### 函数原型

```c
#include <sys/stat.h>
#include <fcntl.h>

int newfstatat(int dirfd, const char *pathname,
               struct stat *statbuf, int flags);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `dirfd` | `int` | 目录 fd，`AT_FDCWD` 表示当前工作目录 |
| `pathname` | `const char*` | 文件路径，`AT_EMPTY_PATH` 时可为空字符串 |
| `statbuf` | `struct stat*` | 输出缓冲区 |
| `flags` | `int` | `AT_EMPTY_PATH`、`AT_SYMLINK_NOFOLLOW` |

### 使用示例

```c
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    struct stat sb;

    // stat() 等价：newfstatat(AT_FDCWD, "file", &sb, 0)
    if (newfstatat(AT_FDCWD, "/etc/passwd", &sb, 0) == -1) {
        perror("newfstatat");
        return 1;
    }
    printf("stat 方式 - 大小: %ld\n", sb.st_size);

    // lstat() 等价：newfstatat(AT_FDCWD, "symlink", &sb, AT_SYMLINK_NOFOLLOW)
    if (newfstatat(AT_FDCWD, "/etc/passwd", &sb, AT_SYMLINK_NOFOLLOW) == -1) {
        perror("newfstatat (nofollow)");
        return 1;
    }
    printf("lstat 方式 - 大小: %ld\n", sb.st_size);

    // fstat() 等价：newfstatat(fd, "", &sb, AT_EMPTY_PATH)
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd >= 0) {
        if (newfstatat(fd, "", &sb, AT_EMPTY_PATH) == 0) {
            printf("fstat 方式 - 大小: %ld\n", sb.st_size);
        }
        close(fd);
    }

    return 0;
}
```

## 执行流程

```
用户态调用                          内核
    |                                 |
    | newfstatat(dirfd, path,         |
    |   statbuf, flags)               |
    |-----> syscall(#79) ------------>|
    |                                 |
    |    +----------------------+     |
    |    | __arm64_sys_newfstatat|     |
    |    +----------+-----------+     |
    |               |                 |
    |    +----------v-----------+     |
    |    | do_statx()           |     |
    |    | fs/stat.c            |     |
    |    +----------+-----------+     |
    |               |                 |
    |    +----------v-----------+     |
    |    | vfs_statx()          |     |
    |    | VFS 层入口           |     |
    |    +----------+-----------+     |
    |               |                 |
    |    +----------v-----------+     |
    |    | 路径解析:             |     |
    |    | 如果 flags 包含       |     |
    |    | AT_EMPTY_PATH:        |     |
    |    |   直接使用 dirfd      |     |
    |    |   对应的 file 对象    |     |
    |    | 否则:                 |     |
    |    |   filename_lookup()   |     |
    |    +----------+-----------+     |
    |               |                 |
    |    +----------v-----------+     |
    |    | vfs_getattr()        |     |
    |    +----------+-----------+     |
    |               |                 |
    |    +----------v-----------+     |
    |    | ext4_getattr()       |     |
    |    | (文件系统相关)        |     |
    |    +----------+-----------+     |
    |               |                 |
    |    +----------v-----------+     |
    |    | generic_fillattr()   |     |
    |    | 填充 kstat 结构       |     |
    |    +----------+-----------+     |
    |               |                 |
    |    +----------v-----------+     |
    |    | cp_newstat()         |     |
    |    | 拷贝到用户空间       |     |
    |    +----------+-----------+     |
    |               |                 |
    |<---- 返回 0 -------------------+|
    |                                 |
```

## 函数调用栈

```
newfstatat(dirfd, pathname, statbuf, flags)
  └─ syscall(__NR_newfstatat, dirfd, pathname, statbuf, flags)
       └─ __arm64_sys_newfstatat()              // arch/arm64/kernel/syscall.c
            └─ do_statx(dfd, pathname, flags, 0, &stat)  // fs/stat.c
                 └─ vfs_statx(dfd, pathname, flags, &stat, 0)
                      ├─ 若 AT_EMPTY_PATH:
                      │    └─ fdget_raw(dfd) → vfs_getattr(&f.file->f_path, ...)
                      └─ 否则:
                           └─ user_path_at(dfd, pathname, lookup_flags, &path)
                                └─ filename_lookup()  // fs/namei.c
                      └─ vfs_getattr(&path, &stat, request_mask, flags)
                           └─ inode->i_op->getattr(&path, &stat, request_mask, flags)
                                └─ ext4_getattr()  // 以 ext4 为例
                                     └─ generic_fillattr(idmap, request_mask, inode, &stat)
                 └─ cp_newstat(&stat, statbuf)  // 拷贝结果到用户空间
```

## 关键数据结构

### struct kstat（内核内部文件状态）

```c
// include/linux/stat.h
struct kstat {
    u32             mask;          // 有效字段掩码
    u64             ino;           // inode 号
    dev_t           dev;           // 设备号
    dev_t           rdev;          // 特殊设备号
    umode_t         mode;          // 文件类型和权限
    unsigned int    nlink;         // 硬链接数
    uid_t           uid;           // 所有者 UID
    gid_t           gid;           // 所属组 GID
    loff_t          size;          // 文件大小
    struct timespec64 atime;       // 最后访问时间
    struct timespec64 mtime;       // 最后修改时间
    struct timespec64 ctime;       // 状态改变时间
    u64             blocks;        // 占用的 512 字节块数
    u64             blksize;       // 首选 I/O 块大小
};
```

### 标志位定义

```c
// include/uapi/linux/fcntl.h
#define AT_FDCWD            -100    // 表示当前工作目录
#define AT_SYMLINK_NOFOLLOW 0x100   // 不跟随符号链接
#define AT_EMPTY_PATH       0x1000  // 允许通过 fd 操作空路径
#define AT_STATX_SYNC_AS_STAT 0x0000 // 与 stat 同步方式相同
#define AT_STATX_FORCE_SYNC 0x2000  // 强制同步
#define AT_STATX_DONT_SYNC  0x4000  // 不要求同步
```

## 备注

- ARM64 系统调用号为 #79
- 这是 ARM64 上 `stat` 系列的唯一系统调用入口
- glibc 的 `stat()`、`fstat()`、`lstat()` 都使用此调用
- 使用 `AT_EMPTY_PATH` 时，`pathname` 应为空字符串，且 `dirfd` 必须指向已打开的文件