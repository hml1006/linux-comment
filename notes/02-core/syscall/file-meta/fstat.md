# fstat 系统调用

## 原理与功能

`fstat` 系统调用通过文件描述符获取文件状态信息，与 `stat` 的区别在于不需要路径查找，直接从已打开的文件描述符中获取元数据。由于内核已经持有 `struct file` 指针，路径可以绕过权限检查（只要有 fd 就有权访问）。

在 ARM64 架构上，`fstat` 没有独立的系统调用号，通过 `newfstatat`（syscall #79）实现。glibc 封装层将 `fstat(fd, buf)` 转换为 `newfstatat(fd, "", buf, AT_EMPTY_PATH)`。

### 功能说明

- 通过 fd 获取文件元数据（大小、权限、时间戳等）
- 无需路径解析，性能优于 `stat`
- 可获取已删除但仍在引用的文件信息（因为 inode 仍存在）
- 绕过路径权限检查

## 使用场景

- 已打开文件的元数据查询（如 `fstat` 命令）
- 获取已删除临时文件的大小（文件仍被进程持有）
- 网络 socket 的信息查询（部分字段有效）
- 性能敏感场景（避免路径解析开销）

## API 及使用案例

### 函数原型

```c
#include <sys/stat.h>
#include <unistd.h>

int fstat(int fd, struct stat *statbuf);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 已打开的文件描述符 |
| `statbuf` | `struct stat*` | 输出缓冲区 |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`（如 `EBADF` 无效 fd）

### 使用示例

```c
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

int main() {
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        perror("fstat");
        close(fd);
        return 1;
    }

    printf("文件大小: %ld 字节\n", sb.st_size);
    printf("权限: %o\n", sb.st_mode & 07777);
    printf("inode: %lu\n", sb.st_ino);
    printf("块数: %ld\n", sb.st_blocks);
    printf("最后修改: %s", ctime(&sb.st_mtime));

    close(fd);
    return 0;
}
```

## 执行流程

```
用户进程                        内核
    |                             |
    | fstat(fd, &buf)             |
    |-----> syscall(#79) -------->|
    |  newfstatat(fd, "",         |
    |    &buf, AT_EMPTY_PATH)     |
    |                             |
    |    +------------------+     |
    |    | do_statx()       |     |
    |    | fs/stat.c        |     |
    |    +--------+---------+     |
    |             |               |
    |    +--------v---------+     |
    |    | vfs_statx()      |     |
    |    +--------+---------+     |
    |             |               |
    |    +--------v---------+     |
    |    | fdget_raw(fd)    |     |
    |    | 获取 struct file  |     |
    |    +--------+---------+     |
    |             |               |
    |    +--------v---------+     |
    |    | vfs_getattr()    |     |
    |    | 使用 file->f_path|     |
    |    +--------+---------+     |
    |             |               |
    |    +--------v---------+     |
    |    | ext4_getattr()   |     |
    |    | 文件系统实现      |     |
    |    +--------+---------+     |
    |             |               |
    |    +--------v---------+     |
    |    | generic_fillattr()|     |
    |    | 填充 kstat 结构   |     |
    |    +--------+---------+     |
    |             |               |
    |    +--------v---------+     |
    |    | cp_newstat()     |     |
    |    | 拷贝到用户空间   |     |
    |    +--------+---------+     |
    |             |               |
    |<---- 返回 0 ---------------+|
    |                             |
```

## 函数调用栈

```
fstat(fd, &statbuf)
  └─ syscall(__NR_newfstatat, fd, "", &statbuf, AT_EMPTY_PATH)
       └─ __arm64_sys_newfstatat()              // arch/arm64/kernel/syscall.c
            └─ do_statx(dfd, "", flags, 0, &stat)
                 └─ vfs_statx(dfd, "", flags | AT_EMPTY_PATH, &stat, 0)
                      ├─ fdget_raw(fd)           // 获取 struct file
                      └─ vfs_getattr(&f.file->f_path, &stat, request_mask, flags)
                           └─ inode->i_op->getattr()
                                └─ ext4_getattr()
                                     └─ generic_fillattr(idmap, request_mask, inode, &stat)
```

## 关键数据结构

### struct file（已打开文件描述）

```c
// include/linux/fs.h
struct file {
    struct path             f_path;       // 文件的 dentry 和 mount 点
    struct inode            *f_inode;     // 缓存 inode 指针
    const struct file_operations *f_op;   // 文件操作函数表
    fmode_t                 f_mode;       // 文件打开模式（O_RDONLY等）
    loff_t                  f_pos;        // 文件偏移量
    struct fown_struct      f_owner;      // 文件所有者（用于信号驱动IO）
    struct user_struct      *f_cred;      // 打开时的安全上下文
    // ...
};
```

### struct path（文件路径描述）

```c
// include/linux/path.h
struct path {
    struct vfsmount *mnt;    // 挂载点
    struct dentry   *dentry; // 目录项（包含 inode 指针）
};
```

## 备注

- ARM64 上无独立 `fstat` 系统调用号，通过 `newfstatat(fd, "", buf, AT_EMPTY_PATH)` 实现
- 与 `stat` 的区别：`fstat` 不需要路径解析，直接使用已打开的 fd
- 可以获取已删除但仍被进程引用的文件信息（`stat` 无法做到）
- 适用于 socket 和 pipe 等特殊文件描述符（部分 stat 字段有定义）