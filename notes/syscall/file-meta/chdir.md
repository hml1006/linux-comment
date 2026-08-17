# chdir 系统调用

## 原理与功能

`chdir` 系统调用将进程的当前工作目录（Current Working Directory, CWD）切换到指定路径。CWD 是进程解析相对路径的基准目录，每个进程在其 `struct fs_struct` 中维护 CWD。

### 功能说明

- 切换进程当前工作目录
- 影响所有后续相对路径操作
- 需要有对目标目录的执行（x）权限
- 线程共享 CWD（同一进程组内所有线程共享）

## 使用场景

- shell 中 `cd` 命令的实现
- 守护进程切换到特定工作目录（如 `/var/lib/xxx`）
- 程序启动后切换到数据目录

## API 及使用案例

### 函数原型

```c
#include <unistd.h>

int chdir(const char *path);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `path` | `const char*` | 目标目录路径 |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`（如 `EACCES` 无权限、`ENOTDIR` 不是目录）

### 使用示例

```c
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main() {
    char buf[1024];

    // 获取当前工作目录
    if (getcwd(buf, sizeof(buf)) != NULL)
        printf("当前目录: %s\n", buf);

    // 切换到 /tmp
    if (chdir("/tmp") == -1) {
        perror("chdir");
        return 1;
    }
    printf("已切换到 /tmp\n");

    // 验证切换成功
    if (getcwd(buf, sizeof(buf)) != NULL)
        printf("当前目录: %s\n", buf);

    // 现在可以使用相对路径打开 /tmp/test.txt
    int fd = open("test.txt", O_RDONLY | O_CREAT, 0644);
    if (fd >= 0) {
        printf("成功创建/打开 /tmp/test.txt\n");
        close(fd);
    }

    return 0;
}
```

## 执行流程

```
用户进程                          内核
    |                               |
    | chdir("/tmp")                 |
    |-----> syscall(#49) ---------->|
    |       __arm64_sys_chdir()     |
    |                               |
    |    +----------------------+   |
    |    | do_chdir()           |   |
    |    | fs/open.c            |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | user_path()          |   |
    |    | 路径解析              |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | 检查是否为目录:      |   |
    |    | S_ISDIR(path.dentry) |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | inode_permission()   |   |
    |    | 检查目录执行权限      |   |
    |    | (X_OK)               |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | set_fs_pwd()         |   |
    |    | 更新进程 fs_struct   |   |
    |    | 中的 pwd 指针        |   |
    |    +---------+------------+   |
    |              |                |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
chdir(pathname)
  └─ syscall(__NR_chdir, pathname)
       └─ __arm64_sys_chdir()                    // arch/arm64/kernel/syscall.c
            └─ do_chdir(pathname)                 // fs/open.c
                 ├─ user_path(pathname, &path)    // 路径解析
                 ├─ 检查 S_ISDIR(path.dentry->d_inode->i_mode)
                 ├─ inode_permission(idmap, inode, MAY_EXEC)  // 检查执行权限
                 └─ set_fs_pwd(current->fs, &path)  // 更新进程 CWD
                      └─ fs->pwd = *path           // 替换 pwd 指针
```

## 关键数据结构

### struct fs_struct（进程文件系统上下文）

```c
// include/linux/fs_struct.h
struct fs_struct {
    struct path       pwd;          // 当前工作目录
    struct path       root;         // 根目录
    int               users;        // 引用计数
    rwlock_t          lock;         // 保护锁
    int               in_exec;      // 是否正在执行 exec
};
```

### struct path（路径描述）

```c
// include/linux/path.h
struct path {
    struct vfsmount *mnt;    // 挂载点
    struct dentry   *dentry; // 目录项
};
```

## 备注

- ARM64 系统调用号为 #49
- 需要目标目录的执行（`x`）权限
- 变更对同一进程的所有线程可见（因为共享 `fs_struct`）
- 对应的 `fchdir` 通过 fd 切换，无需路径解析
- `getcwd()` 读取 `current->fs->pwd` 获取当前目录