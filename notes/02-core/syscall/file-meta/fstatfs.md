# fstatfs 系统调用

## 原理与功能

`fstatfs` 通过文件描述符（而非路径）获取文件系统统计信息，与 `statfs` 功能相同但操作方式不同。它不需要路径查找，直接从已打开文件对应的挂载点获取文件系统信息。

### 功能说明

- 通过 fd 获取文件系统统计信息
- 无需路径解析，性能优于 `statfs`
- 可获取已删除但仍在引用的文件的文件系统信息

## 使用场景

- 已打开文件的文件系统查询（如 `df -f` 的底层实现）
- 避免路径解析开销的性能敏感场景
- 文件描述符已打开但文件已删除的场景

## API 及使用案例

### 函数原型

```c
#include <sys/vfs.h>

int fstatfs(int fd, struct statfs *buf);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 已打开的文件描述符 |
| `buf` | `struct statfs*` | 输出缓冲区 |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <sys/vfs.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    struct statfs buf;
    if (fstatfs(fd, &buf) == -1) {
        perror("fstatfs");
        close(fd);
        return 1;
    }

    printf("文件系统类型: 0x%lx\n", buf.f_type);
    printf("块大小: %lu\n", buf.f_bsize);
    printf("总大小: %llu MB\n",
           (unsigned long long)buf.f_blocks * buf.f_bsize / 1024 / 1024);
    printf("可用空间: %llu MB\n",
           (unsigned long long)buf.f_bavail * buf.f_bsize / 1024 / 1024);
    printf("总 inode: %llu\n", buf.f_files);
    printf("可用 inode: %llu\n", buf.f_ffree);

    close(fd);
    return 0;
}
```

## 执行流程

```
用户进程                          内核
    |                               |
    | fstatfs(fd, &buf)             |
    |-----> syscall(#138) --------->|
    |       __arm64_sys_fstatfs()   |
    |                               |
    |    +-----------------------+  |
    |    | fd_statfs()           |  |
    |    | fs/statfs.c           |  |
    |    +-----------+-----------+  |
    |                |              |
    |    +-----------v-----------+  |
    |    | fdget_raw(fd)         |  |
    |    | 获取 struct file      |  |
    |    +-----------+-----------+  |
    |                |              |
    |    +-----------v-----------+  |
    |    | vfs_statfs()          |  |
    |    | 使用 fd->f_path       |  |
    |    +-----------+-----------+  |
    |                |              |
    |    +-----------v-----------+  |
    |    | statfs_by_dentry()    |  |
    |    +-----------+-----------+  |
    |                |              |
    |    +-----------v-----------+  |
    |    | sb->s_op->statfs()    |  |
    |    | 具体文件系统实现      |  |
    |    +-----------+-----------+  |
    |                |              |
    |    +-----------v-----------+  |
    |    | copy_statfs_to_user() |  |
    |    | 拷贝到用户空间        |  |
    |    +-----------+-----------+  |
    |                |              |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
fstatfs(fd, &buf)
  └─ syscall(__NR_fstatfs, fd, &buf)
       └─ __arm64_sys_fstatfs()                    // arch/arm64/kernel/syscall.c
            └─ fd_statfs(fd, pathname, &buf)        // fs/statfs.c
                 ├─ fdget_raw(fd)                   // 获取 struct file
                 └─ vfs_statfs(&f.file->f_path, &buf)
                      └─ statfs_by_dentry(dentry, &buf)
                           └─ sb->s_op->statfs(dentry, &buf)
                                └─ ext4_statfs()     // 以 ext4 为例
                 └─ copy_statfs_to_user(&tmp, &buf)
```

## 关键数据结构

### struct statfs

```c
// include/uapi/asm-generic/statfs.h
struct statfs {
    __u32   f_type;      // 文件系统类型标识符
    __u32   f_bsize;     // 基本块大小
    __u64   f_blocks;    // 文件系统总块数
    __u64   f_bfree;     // 空闲块数
    __u64   f_bavail;    // 非特权用户可用块数
    __u64   f_files;     // 总 inode 数
    __u64   f_ffree;     // 空闲 inode 数
    __u64   f_fsid;      // 文件系统 ID
    __u32   f_namelen;   // 最大文件名长度
    __u32   f_frsize;    // 分片大小
    __u32   f_flags;     // 挂载标志
    __u32   f_spare[4];  // 保留字段
};
```

## 备注

- ARM64 系统调用号为 #138
- 与 `statfs` 的区别：通过 fd 而非路径查询
- `fstatfs` 不需要路径解析，所以更高效
- 与 `statfs` 共享相同的 `struct statfs` 结构