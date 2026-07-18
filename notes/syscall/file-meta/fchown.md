# fchown 系统调用

## 原理与功能

`fchown` 通过文件描述符改变文件所有者和/或所属组，与 `chown` 功能相同但操作方式不同。它不需要路径解析，直接操作已打开的文件。

### 功能说明

- 通过 fd 更改文件所有者/所属组
- 无需路径解析，性能更优
- 需要 `CAP_CHOWN` 能力（更改所有者）或文件所有者权限（更改组）
- 传递 `-1` 表示不修改该字段

## 使用场景

- 打开文件后修改所有权
- 避免路径解析的开销
- 无法通过路径访问的文件（如已删除但仍被进程持有的文件）

## API 及使用案例

### 函数原型

```c
#include <unistd.h>
#include <sys/types.h>

int fchown(int fd, uid_t owner, gid_t group);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 已打开的文件描述符 |
| `owner` | `uid_t` | 新所有者 ID（-1 不修改） |
| `group` | `gid_t` | 新组 ID（-1 不修改） |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main() {
    int fd = open("example.txt", O_RDWR);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    // 更改所有者和组
    if (fchown(fd, 1000, 100) == -1) {
        perror("fchown");
        close(fd);
        return 1;
    }
    printf("所有权已更改\n");

    close(fd);
    return 0;
}
```

## 执行流程

```
fchown(fd, owner, group)            内核
    |                               |
    |-----> syscall(#54) ---------->|
    |       __arm64_sys_fchown()    |
    |                               |
    |    +----------------------+   |
    |    | ksys_fchown()        |   |
    |    | fs/open.c:849        |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | fdget(fd)            |   |
    |    | 获取 struct file     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | vfs_fchown()         |   |
    |    | fs/open.c:836        |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | mnt_want_write_file()|   |
    |    | 检查文件系统可写     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | chown_common()       |   |
    |    | 共享的 chown 逻辑    |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | notify_change()      |   |
    |    | VFS 属性修改通知     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | inode->i_op->setattr()|  |
    |    | 文件系统实现         |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | mnt_drop_write_file()|   |
    |    +---------+------------+   |
    |              |                |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
fchown(fd, owner, group)
  └─ syscall(__NR_fchown, fd, owner, group)
       └─ __arm64_sys_fchown()
            └─ ksys_fchown(fd, user, group)             // fs/open.c:849
                 ├─ fdget(fd)                           // 获取 struct file
                 └─ vfs_fchown(fd_file(f), user, group)  // fs/open.c:836
                      ├─ mnt_want_write_file(f)         // 获取挂载写权限
                      ├─ audit_file(f)
                      └─ chown_common(&file->f_path, user, group)
                           ├─ inode_owner_or_capable()  // 权限检查
                           ├─ security_path_chown()     // LSM 钩子
                           ├─ notify_change()           // VFS 通知
                           │    └─ inode->i_op->setattr()
                           └─ mnt_drop_write_file(f)
```

## 关键数据结构

```c
// include/linux/fs.h
struct file {
    struct path             f_path;       // 文件的 dentry 和 mount 点
    struct inode            *f_inode;     // 缓存 inode 指针
    // ...
};

// vfs_fchown 实现 (fs/open.c:836)
int vfs_fchown(struct file *file, uid_t user, gid_t group)
{
    int error;
    error = mnt_want_write_file(file);
    if (error)
        return error;
    audit_file(file);
    error = chown_common(&file->f_path, user, group);
    mnt_drop_write_file(file);
    return error;
}
```

## 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EBADF` | 无效 fd | `fd` 不是有效的文件描述符 |
| `EIO` | I/O 错误 | 读取文件系统时发生 I/O 错误 |
| `EPERM` | 操作不允许 | 无 `CAP_CHOWN` 或非所有者 |
| `EROFS` | 只读文件系统 | 文件所在文件系统以只读方式挂载 |
| `ENOMEM` | 内存不足 | 内核内存分配失败 |

## 备注

- ARM64 系统调用号为 #54
- 与 `chown` 的区别：通过 fd 操作，无需路径解析
- 不需要文件以写模式打开

## 参考

- 内核源码: `fs/open.c` (`ksys_fchown`, `vfs_fchown`, `chown_common`, `SYSCALL_DEFINE3(fchown)`)
- `include/linux/fs.h` — `struct file`