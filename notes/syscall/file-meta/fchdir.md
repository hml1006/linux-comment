# fchdir 系统调用

## 原理与功能

`fchdir` 通过文件描述符切换进程当前工作目录，与 `chdir` 功能相同但操作方式不同。它不需要路径解析，直接使用已打开的目录 fd。

### 功能说明

- 通过目录 fd 切换工作目录
- 无需路径解析，性能更优
- fd 必须指向目录（非目录返回 `-ENOTDIR`）
- 需要 fd 具有执行（`MAY_EXEC`）和进入目录（`MAY_CHDIR`）权限

### 内核实现

```c
// fs/open.c:574
SYSCALL_DEFINE1(fchdir, unsigned int, fd)
{
    CLASS(fd_raw, f)(fd);
    int error;

    // 检查 fd 是否有效
    if (fd_empty(f))
        return -EBADF;

    // 检查是否是目录
    if (!d_can_lookup(fd_file(f)->f_path.dentry))
        return -ENOTDIR;

    // 检查权限（需要执行和进入目录权限）
    error = file_permission(fd_file(f), MAY_EXEC | MAY_CHDIR);
    if (!error)
        // 更新进程的当前工作目录
        set_fs_pwd(current->fs, &fd_file(f)->f_path);
    return error;
}
```

### 使用场景

- 保存并恢复工作目录（先 `open(".", O_RDONLY)` 再 `fchdir(fd)`）
- 避免路径解析的开销
- 无法通过路径访问的目录（如已删除但仍被 fd 引用的目录）

## API 及使用案例

### 函数原型

```c
#include <unistd.h>

int fchdir(int fd);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 已打开的目录文件描述符 |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main() {
    // 保存当前工作目录
    int cwd_fd = open(".", O_RDONLY);
    if (cwd_fd == -1) {
        perror("open cwd");
        return 1;
    }

    // 切换到 /tmp
    if (chdir("/tmp") == -1) {
        perror("chdir");
        close(cwd_fd);
        return 1;
    }
    printf("已切换到 /tmp\n");

    // 恢复原始工作目录
    if (fchdir(cwd_fd) == -1) {
        perror("fchdir");
        close(cwd_fd);
        return 1;
    }
    printf("已恢复原始目录\n");

    close(cwd_fd);
    return 0;
}
```

## 执行流程

```
fchdir(fd)
  └─ syscall(__NR_fchdir, fd)                    // 系统调用入口
       └─ __arm64_sys_fchdir()                   // arch/arm64/kernel/syscall.c
            └─ SYSCALL_DEFINE1(fchdir)           // fs/open.c:574
                 ├─ CLASS(fd_raw, f)(fd)         // 通过 fd 获取 struct fd
                 │    └─ fdget_raw(fd)           // 从进程 fd 表获取
                 ├─ fd_empty(f)? → -EBADF       // 检查 fd 有效性
                 ├─ d_can_lookup(dentry)?        // 检查是否是目录
                 │    └─ 否 → -ENOTDIR
                 ├─ file_permission(file, MAY_EXEC | MAY_CHDIR)
                 │    └─ 权限检查（LSM 钩子等）
                 └─ set_fs_pwd(current->fs, &file->f_path)
                      └─ fs->pwd = *path        // 更新进程 CWD
```

## 函数调用栈

```
fchdir(fd)
  └─ syscall(__NR_fchdir, fd)
       └─ __arm64_sys_fchdir()                  // arch/arm64/kernel/syscall.c
            └─ SYSCALL_DEFINE1(fchdir, fd)      // fs/open.c:574
                 ├─ fdget_raw(fd)               // 获取 struct fd
                 ├─ d_can_lookup(dentry)         // 检查目录类型
                 ├─ file_permission(file, MAY_EXEC | MAY_CHDIR)
                 └─ set_fs_pwd(current->fs, &file->f_path)
```

## 关键数据结构

### struct fs_struct（进程文件系统上下文）

```c
// include/linux/fs_struct.h
struct fs_struct {
    struct path       pwd;          // 当前工作目录（fchdir 修改此字段）
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

### set_fs_pwd 实现

```c
// fs/fs_struct.c
void set_fs_pwd(struct fs_struct *fs, const struct path *path)
{
    struct path old_pwd;

    // 获取写锁
    write_lock(&fs->lock);
    // 替换 pwd 指针
    old_pwd = fs->pwd;
    fs->pwd = *path;
    // 增加新路径引用计数
    path_get(path);
    write_unlock(&fs->lock);

    // 释放旧路径
    path_put(&old_pwd);
}
```

## 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EBADF` | 无效 fd | `fd` 不是有效的打开文件描述符 |
| `ENOTDIR` | 不是目录 | `fd` 指向的不是目录 |
| `EACCES` | 权限不足 | 对目录无执行权限 |
| `EFAULT` | 地址错误 | 指针参数无效（罕见） |

## 备注

- ARM64 系统调用号为 #50
- 与 `chdir` 的区别：通过 fd 操作，无需路径解析
- 常用于保存/恢复工作目录模式（`open + fchdir` 组合）
- 进程内所有线程共享 `fs_struct`，因此 `fchdir` 影响所有线程
- 对应的 `getcwd()` 读取 `current->fs->pwd` 获取当前目录

## 参考

- 内核源码: `fs/open.c` (`SYSCALL_DEFINE1(fchdir)`)
- `include/linux/fs_struct.h` — `struct fs_struct` 定义
- `include/linux/path.h` — `struct path` 定义
- [chdir.md](chdir.md) — 通过路径切换工作目录