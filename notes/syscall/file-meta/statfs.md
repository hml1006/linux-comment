# statfs 系统调用

## 原理与功能

`statfs` 系统调用获取文件系统的统计信息，包括总块数、可用块数、文件节点数、文件系统类型等。它是 `df` 命令的核心实现基础。

### 功能说明

- 获取文件系统总体信息（总大小、可用空间等）
- 获取文件系统类型和挂载标志
- 获取文件系统块大小（用于 I/O 优化）
- 获取文件系统最大文件名长度

## 使用场景

- `df` 命令显示磁盘空间使用情况
- 应用程序检查磁盘可用空间（如备份工具）
- 文件系统类型检测（如是否支持某些特性）
- 优化 I/O 操作（根据块大小调整缓冲区）

## API 及使用案例

### 函数原型

```c
#include <sys/vfs.h>  // 或 <sys/statfs.h>

int statfs(const char *path, struct statfs *buf);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `path` | `const char*` | 文件系统上任意文件或目录的路径 |
| `buf` | `struct statfs*` | 输出缓冲区 |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <sys/vfs.h>

int main() {
    struct statfs buf;

    if (statfs("/", &buf) == -1) {
        perror("statfs");
        return 1;
    }

    unsigned long long total = buf.f_blocks * buf.f_bsize;
    unsigned long long free = buf.f_bfree * buf.f_bsize;
    unsigned long long avail = buf.f_bavail * buf.f_bsize;

    printf("文件系统类型: 0x%lx\n", buf.f_type);
    printf("块大小: %lu\n", buf.f_bsize);
    printf("总大小: %llu 字节 (%.2f GB)\n", total, total / (1024.0*1024*1024));
    printf("可用空间: %llu 字节 (%.2f GB)\n", avail, avail / (1024.0*1024*1024));
    printf("已用空间: %llu 字节 (%.2f GB)\n",
           total - free, (total - free) / (1024.0*1024*1024));
    printf("总 inode: %llu\n", buf.f_files);
    printf("可用 inode: %llu\n", buf.f_ffree);
    printf("最大文件名长度: %lu\n", buf.f_namelen);

    return 0;
}
```

## 执行流程

```
用户进程                          内核
    |                               |
    | statfs("/", &buf)             |
    |-----> syscall(#137) --------->|
    |       __arm64_sys_statfs()    |
    |                               |
    |    +---------------------+    |
    |    | fd_statfs()         |    |
    |    | fs/statfs.c         |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | user_path()         |    |
    |    | 路径解析            |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | vfs_statfs()        |    |
    |    | VFS 层入口          |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | statfs_by_dentry()  |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | sb->s_op->statfs()  |    |
    |    | 具体文件系统实现    |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | copy_statfs_to_user()|    |
    |    | 拷贝到用户空间      |    |
    |    +---------+-----------+    |
    |              |                |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
statfs(pathname, &buf)
  └─ syscall(__NR_statfs, pathname, &buf)
       └─ __arm64_sys_statfs()                     // arch/arm64/kernel/syscall.c
            └─ fd_statfs(dfd, pathname, &buf)       // fs/statfs.c
                 ├─ user_path(pathname, &path)      // 路径解析
                 └─ vfs_statfs(&path, &buf)          // VFS 层
                      └─ statfs_by_dentry(path->dentry, &buf)
                           └─ sb->s_op->statfs(dentry, &buf)
                                └─ ext4_statfs()     // 以 ext4 为例
                 └─ copy_statfs_to_user(&tmp, &buf)  // 拷贝到用户空间
```

## 关键数据结构

### struct statfs（用户空间）

```c
// include/uapi/asm-generic/statfs.h
struct statfs {
    __u32   f_type;      // 文件系统类型标识符（如 EXT4_SUPER_MAGIC=0xEF53）
    __u32   f_bsize;     // 基本块大小
    __u64   f_blocks;    // 文件系统总块数
    __u64   f_bfree;     // 空闲块数
    __u64   f_bavail;    // 非特权用户可用的块数
    __u64   f_files;     // 总 inode 数
    __u64   f_ffree;     // 空闲 inode 数
    __u64   f_fsid;      // 文件系统 ID
    __u32   f_namelen;   // 最大文件名长度
    __u32   f_frsize;    // 分片大小（块大小的子单位）
    __u32   f_flags;     // 挂载标志（如 MS_RDONLY）
    __u32   f_spare[4];  // 保留字段
};
```

### 常见文件系统类型值

```c
#define EXT4_SUPER_MAGIC    0xEF53  // ext4
#define XFS_SUPER_MAGIC     0x58465342  // XFS
#define BTRFS_SUPER_MAGIC   0x9123683E  // Btrfs
#define TMPFS_MAGIC         0x01021994  // tmpfs
#define PROC_SUPER_MAGIC    0x9FA0   // procfs
#define SYSFS_MAGIC         0x62656572  // sysfs
#define NFS_SUPER_MAGIC     0x6969   // NFS
```

## 备注

- ARM64 系统调用号为 #137
- 64 位版本 `statfs64` 也支持，用于大文件系统
- `f_bavail` 通常小于 `f_bfree`，因为有预留块（默认 5%）
- 有对应的 `fstatfs`（通过 fd 获取）和 `statfs64`（支持大文件系统）