# fchmod 系统调用

## 原理与功能

`fchmod` 通过文件描述符改变文件权限，与 `chmod` 功能相同但操作方式不同。它不需要路径解析，直接操作已打开的文件。

### 功能说明

- 通过 fd 修改文件权限
- 无需路径解析，性能更优
- 需要文件描述符（对文件本身无写权限要求，但需要文件所有者或 CAP_FOWNER）

## 使用场景

- 打开文件后修改权限
- 避免路径解析开销
- 无法通过路径访问的文件（如已删除但仍被进程持有的文件）
- 临时文件创建后设置严格权限

## API 及使用案例

### 函数原型

```c
#include <sys/stat.h>

int fchmod(int fd, mode_t mode);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 已打开的文件描述符 |
| `mode` | `mode_t` | 新权限模式 |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd = open("example.txt", O_RDWR | O_CREAT, 0666);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    // 通过 fd 修改权限为 0600
    if (fchmod(fd, 0600) == -1) {
        perror("fchmod");
        close(fd);
        return 1;
    }
    printf("权限已设置为 0600\n");

    close(fd);
    return 0;
}
```

## 执行流程

```
fchmod(fd, mode)                    内核
    |                               |
    |-----> syscall(#52) ---------->|
    |       __arm64_sys_fchmod()    |
    |                               |
    |    +----------------------+   |
    |    | vfs_fchmod()         |   |
    |    | fs/open.c            |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | fdget(fd)            |   |
    |    | 获取 struct file     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | mnt_want_write_file()|   |
    |    | 检查文件系统可写     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | chmod_common()       |   |
    |    | 共享的 chmod 逻辑    |   |
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
fchmod(fd, mode)
  └─ syscall(__NR_fchmod, fd, mode)
       └─ __arm64_sys_fchmod()
            └─ vfs_fchmod(fd_file(f), mode)           // fs/open.c
                 ├─ mnt_want_write_file(f)            // 获取挂载写权限
                 ├─ chmod_common(&file->f_path, mode)  // fs/open.c:621
                 │    ├─ inode_lock_killable(inode)
                 │    ├─ security_path_chmod(path, mode)
                 │    ├─ 构造 newattrs:
                 │    │    ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO)
                 │    │    ia_valid = ATTR_MODE | ATTR_CTIME
                 │    └─ notify_change(idmap, dentry, &newattrs, NULL)
                 └─ mnt_drop_write_file(f)
```

## 关键数据结构

### struct file（已打开文件描述）

```c
// include/linux/fs.h
struct file {
    struct path             f_path;       // 文件的 dentry 和 mount 点
    struct inode            *f_inode;     // 缓存 inode 指针
    const struct file_operations *f_op;   // 文件操作函数表
    fmode_t                 f_mode;       // 文件打开模式
    struct cred             *f_cred;      // 打开时的安全上下文
    // ...
};
```

### chmod_common 共享逻辑

```c
// fs/open.c:621
int chmod_common(const struct path *path, umode_t mode)
{
    struct inode *inode = path->dentry->d_inode;
    struct iattr newattrs;
    int error;

    error = mnt_want_write(path->mnt);
    if (error)
        return error;
    error = inode_lock_killable(inode);
    if (error)
        goto out_mnt_unlock;
    error = security_path_chmod(path, mode);
    if (error)
        goto out_unlock;
    newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
    newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
    error = notify_change(mnt_idmap(path->mnt), path->dentry,
                          &newattrs, &delegated_inode);
    // ...
}
```

## 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EBADF` | 无效 fd | `fd` 不是有效的文件描述符 |
| `EIO` | I/O 错误 | 读取文件系统时发生 I/O 错误 |
| `EPERM` | 操作不允许 | 不是文件所有者且无 `CAP_FOWNER` 能力 |
| `EROFS` | 只读文件系统 | 文件所在文件系统以只读方式挂载 |
| `ENOMEM` | 内存不足 | 内核内存分配失败 |

## 备注

- ARM64 系统调用号为 #52
- 与 `chmod` 的区别：通过 fd 操作，无需路径解析
- 不需要文件以写模式打开（`fchmod` 修改的是 inode 元数据，不是文件内容）
- 需要文件所有者或 `CAP_FOWNER` 能力
- 非所有者修改权限时会清除 setuid/setgid 位

## 参考

- 内核源码: `fs/open.c` (`vfs_fchmod`, `chmod_common`, `SYSCALL_DEFINE2(fchmod)`)
- `include/linux/fs.h` — `struct file`, `struct iattr`