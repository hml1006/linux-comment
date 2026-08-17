# statx 系统调用

## 原理与功能

`statx` 是 Linux 4.11 引入的增强版文件状态获取系统调用，解决了传统 `stat` 系列接口的以下问题：

1. **字段掩码机制**：只请求需要的字段，避免不必要的开销
2. **纳秒时间戳**：直接返回 `struct timespec`（纳秒级精度）
3. **更多元数据**：支持文件生成号、BSD 文件属性、挂载点 ID 等
4. **可扩展性**：通过 `struct statx` 设计，未来可添加新字段而不破坏 ABI
5. **同步模式**：支持 `AT_STATX_SYNC_AS_STAT` 等标志控制同步行为

### 功能说明

- 获取文件扩展状态信息（比 `stat` 更丰富）
- 支持掩码选择字段（`STATX_SIZE`、`STATX_MTIME` 等）
- 支持同步模式标志（强制同步、允许垫片、同步与 stat 相同）
- 不跟随符号链接（默认行为，通过 `AT_SYMLINK_NOFOLLOW` 控制）

## 使用场景

- 需要纳秒级时间戳的应用
- 需要文件生成号（`stx_ino` 生成号）的备份工具
- 需要挂载点信息的文件系统监控工具
- 需要避免不必要的元数据查询开销的性能敏感应用
- 现代 Linux 系统中的 `stat` 命令实现

## API 及使用案例

### 函数原型

```c
#include <sys/stat.h>
#include <fcntl.h>

int statx(int dirfd, const char *pathname, int flags,
          unsigned int mask, struct statx *statxbuf);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `dirfd` | `int` | 目录 fd，`AT_FDCWD` 表示当前工作目录 |
| `pathname` | `const char*` | 文件路径 |
| `flags` | `int` | 标志位：`AT_EMPTY_PATH`、`AT_SYMLINK_NOFOLLOW`、`AT_STATX_SYNC_*` |
| `mask` | `unsigned int` | 请求的字段掩码（`STATX_SIZE`、`STATX_MTIME` 等） |
| `statxbuf` | `struct statx*` | 输出缓冲区 |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

int main() {
    struct statx stx;

    // 请求大小、修改时间、权限
    unsigned int mask = STATX_SIZE | STATX_MTIME | STATX_MODE;

    if (statx(AT_FDCWD, "/etc/passwd", 0, mask, &stx) == -1) {
        perror("statx");
        return 1;
    }

    printf("文件大小: %lld 字节\n", stx.stx_size);
    printf("权限: %o\n", stx.stx_mode & 07777);
    printf("inode: %llu\n", stx.stx_ino);
    printf("设备号: major=%u minor=%u\n",
           major(stx.stx_dev), minor(stx.stx_dev));

    // 纳秒级时间戳
    printf("修改时间: %lld.%09u\n",
           stx.stx_mtime.tv_sec, stx.stx_mtime.tv_nsec);

    // 检查请求的字段是否有效
    if (stx.stx_mask & STATX_SIZE)
        printf("大小字段有效\n");

    // 检查文件类型
    if (S_ISREG(stx.stx_mode))
        printf("文件类型: 普通文件\n");

    return 0;
}
```

## 执行流程

```
用户进程                          内核
    |                               |
    | statx(AT_FDCWD, path, 0,      |
    |       STATX_SIZE|STATX_MODE,  |
    |       &stx)                    |
    |-----> syscall(#291) ---------->|
    |       __arm64_sys_statx()     |
    |                               |
    |    +-------------------+      |
    |    | vfs_statx()       |      |
    |    | fs/stat.c         |      |
    |    +--------+----------+      |
    |             |                 |
    |    +--------v----------+      |
    |    | filename_lookup() |      |
    |    | 路径查找          |      |
    |    +--------+----------+      |
    |             |                 |
    |    +--------v----------+      |
    |    | vfs_getattr()     |      |
    |    | VFS 获取属性      |      |
    |    +--------+----------+      |
    |             |                 |
    |    +--------v----------+      |
    |    | ext4_getattr()    |      |
    |    | ext4 实现         |      |
    |    +--------+----------+      |
    |             |                 |
    |    +--------v----------+      |
    |    | generic_fillattr()|      |
    |    | 填充通用字段      |      |
    |    +--------+----------+      |
    |             |                 |
    |    +--------v----------+      |
    |    | fill_ext4_fields()|      |
    |    | 填充 ext4 特定字段|      |
    |    +--------+----------+      |
    |             |                 |
    |    +--------v----------+      |
    |    | cp_statx()        |      |
    |    | 拷贝到用户空间    |      |
    |    +--------+----------+      |
    |             |                 |
    |<---- 返回 0 -----------------+|
    |                               |
```

## 函数调用栈

```
statx(dirfd, pathname, flags, mask, &statxbuf)
  └─ syscall(__NR_statx, dirfd, pathname, flags, mask, &statxbuf)
       └─ __arm64_sys_statx()                    // arch/arm64/kernel/syscall.c
            └─ vfs_statx(dfd, pathname, flags, &stat, mask)
                 ├─ user_path_at(dfd, pathname, lookup_flags, &path)
                 │    └─ filename_lookup()        // 路径解析
                 └─ vfs_getattr(&path, &stat, request_mask, flags)
                      └─ inode->i_op->getattr(&path, &stat, request_mask, flags)
                           └─ ext4_getattr()      // 以 ext4 为例
                                ├─ generic_fillattr(idmap, request_mask, inode, &stat)
                                └─ ext4_fillattr(inode, &stat)  // ext4 特定字段
```

## 关键数据结构

### struct statx（用户空间）

```c
// include/uapi/linux/stat.h
struct statx {
    __u32   stx_mask;        // 掩码：哪些字段有效（STATX_* 位标志）
    __u32   stx_blksize;     // 首选 I/O 块大小
    __u64   stx_attributes;  // 文件属性标志（STATX_ATTR_*）
    __u32   stx_nlink;       // 硬链接数
    __u32   stx_uid;         // 所有者 UID
    __u32   stx_gid;         // 所属组 GID
    __u16   stx_mode;        // 文件类型和权限
    __u16   __spare0[1];
    __u64   stx_ino;         // inode 号
    __u64   stx_size;        // 文件大小
    __u64   stx_blocks;      // 占用的 512 字节块数
    __u64   stx_attributes_mask; // 支持的属性掩码
    struct statx_timestamp stx_atime;   // 最后访问时间
    struct statx_timestamp stx_btime;   // 文件创建时间（诞生时间）
    struct statx_timestamp stx_ctime;   // 状态改变时间
    struct statx_timestamp stx_mtime;   // 最后修改时间
    __u32   stx_rdev_major;  // 特殊文件设备号 major
    __u32   stx_rdev_minor;  // 特殊文件设备号 minor
    __u32   stx_dev_major;   // 文件所在设备号 major
    __u32   stx_dev_minor;   // 文件所在设备号 minor
    __u64   __spare2[14];    // 填充字段，预留扩展
};
```

### struct statx_timestamp（高精度时间戳）

```c
// include/uapi/linux/stat.h
struct statx_timestamp {
    __s64   tv_sec;     // 秒数（从 1970-01-01 开始）
    __u32   tv_nsec;    // 纳秒数（0-999999999）
    __s32   __reserved; // 保留字段
};
```

### 字段掩码（stx_mask 位标志）

```c
#define STATX_TYPE       0x00000001U  // 文件类型
#define STATX_MODE       0x00000002U  // 文件权限
#define STATX_NLINK      0x00000004U  // 硬链接数
#define STATX_UID        0x00000008U  // 所有者 UID
#define STATX_GID        0x00000010U  // 所属组 GID
#define STATX_ATIME      0x00000020U  // 访问时间
#define STATX_MTIME      0x00000040U  // 修改时间
#define STATX_CTIME      0x00000080U  // 状态改变时间
#define STATX_INO        0x00000100U  // inode 号
#define STATX_SIZE       0x00000200U  // 文件大小
#define STATX_BLOCKS     0x00000400U  // 块数
#define STATX_BTIME      0x00000800U  // 文件创建时间
#define STATX_ALL        0x00000FFFU  // 所有标准字段
```

## 备注

- ARM64 上系统调用号为 #291
- `statx` 不提供 `lstatx` 变体，通过 `AT_SYMLINK_NOFOLLOW` 标志控制
- `stx_attributes` 包含 `STATX_ATTR_COMPRESSED`、`STATX_ATTR_IMMUTABLE` 等扩展属性
- `stx_btime`（文件创建时间）仅在支持的文件系统上有效（如 ext4 的 crtime）