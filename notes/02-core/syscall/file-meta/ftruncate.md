# ftruncate 系统调用

## 原理与功能

`ftruncate` 通过文件描述符截断或扩展文件到指定长度，与 `truncate` 功能相同但操作方式不同。它不需要路径解析，直接对已打开的文件描述符操作。

### 功能说明

- 通过 fd 截断/扩展文件到指定大小
- 需要文件以写模式打开（`O_WRONLY` 或 `O_RDWR`）
- 无需路径解析，操作更高效

## 使用场景

- 打开文件后截断到指定大小
- 下载工具预先分配空间
- 循环日志文件管理
- 共享内存区域大小调整

## API 及使用案例

### 函数原型

```c
#include <unistd.h>
#include <sys/types.h>

int ftruncate(int fd, off_t length);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 已打开的文件描述符（需写权限） |
| `length` | `off_t` | 目标文件大小（字节） |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int main() {
    // 以读写方式打开文件
    int fd = open("example.txt", O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    // 截断为 4096 字节
    if (ftruncate(fd, 4096) == -1) {
        perror("ftruncate");
        close(fd);
        return 1;
    }
    printf("文件已截断/扩展到 4096 字节\n");

    // 验证大小
    struct stat sb;
    if (fstat(fd, &sb) == 0) {
        printf("当前文件大小: %ld 字节\n", sb.st_size);
    }

    close(fd);
    return 0;
}
```

## 执行流程

```
用户进程                          内核
    |                               |
    | ftruncate(fd, length)         |
    |-----> syscall(#46) ---------->|
    |       __arm64_sys_ftruncate() |
    |                               |
    |    +----------------------+   |
    |    | do_sys_ftruncate()   |   |
    |    | fs/open.c            |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | fdget(fd)            |   |
    |    | 获取 struct file     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | 检查写模式:          |   |
    |    | file->f_mode & FMODE_WRITE|
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | do_truncate()        |   |
    |    | 共享截断逻辑         |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | notify_change()      |   |
    |    | VFS 属性修改通知     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | ext4_setattr()       |   |
    |    | 具体文件系统实现     |   |
    |    +---------+------------+   |
    |              |                |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
ftruncate(fd, length)
  └─ syscall(__NR_ftruncate, fd, length)
       └─ __arm64_sys_ftruncate()                 // arch/arm64/kernel/syscall.c
            └─ do_sys_ftruncate(fd, length)        // fs/open.c
                 ├─ fdget(fd)                      // 获取 struct file
                 ├─ 检查 file->f_mode & FMODE_WRITE
                 └─ do_truncate(file->f_path.dentry, length, 0, file->f_path.mnt)
                      └─ notify_change(idmap, dentry, &newattr, NULL)
                           └─ inode->i_op->setattr(idmap, dentry, &attr)
                                └─ ext4_setattr()   // 以 ext4 为例
                                     ├─ ext4_alloc_truncate_blocks()
                                     └─ ext4_truncate(inode)
```

## 关键数据结构

### struct inode（索引节点）

```c
// include/linux/fs.h
struct inode {
    umode_t         i_mode;      // 文件类型和权限
    kuid_t          i_uid;       // 所有者 UID
    kgid_t          i_gid;       // 所属组 GID
    loff_t          i_size;      // 文件大小（truncate 修改此字段）
    struct timespec64 i_atime;   // 最后访问时间
    struct timespec64 i_mtime;   // 最后修改时间
    struct timespec64 i_ctime;   // 状态改变时间
    // ...
};
```

## 备注

- ARM64 系统调用号为 #46
- 与 `truncate` 的区别：通过 fd 操作，不需要路径解析
- 文件必须以写模式打开（`O_WRONLY` 或 `O_RDWR`）
- 文件扩展时额外的空间用零填充