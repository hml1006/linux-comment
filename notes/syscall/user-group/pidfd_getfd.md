# pidfd_getfd 系统调用分析

## 1. 概述

`pidfd_getfd` 通过目标进程的 pidfd，获取该进程中指定文件描述符的副本，并将其安装到当前进程中。返回的 fd 带有 `FD_CLOEXEC` 标志。

**原型：**

```c
SYSCALL_DEFINE3(pidfd_getfd, int, pidfd, int, fd, unsigned int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `pidfd` | `int` | 目标进程的 pidfd（通过 `pidfd_open()` 获取） |
| `fd` | `int` | 目标进程中要复制的文件描述符编号 |
| `flags` | `unsigned int` | 保留标志位，当前必须为 0 |

**返回值：**
- 成功返回新的文件描述符（带有 `FD_CLOEXEC`）
- 失败返回负的错误码

## 2. 使用场景

- 调试器（如 gdb）访问目标进程的文件描述符
- 容器管理工具检查或复制容器中的 fd
- 进程间文件传递（无需 `UNIX` 域套接字）
- `checkpoint/restore` 场景

## 3. 函数调用栈

```
SYSCALL_DEFINE3(pidfd_getfd, pidfd, fd, flags)            // kernel/pid.c
  ├─ flags != 0 → 返回 -EINVAL
  ├─ CLASS(fd, f)(pidfd)                                   // 通过 pidfd 获取 struct fd
  ├─ fd_empty(f) → 返回 -EBADF
  ├─ pid = pidfd_pid(fd_file(f))                           // 从 pidfd 文件获取 struct pid
  │    IS_ERR(pid) → 返回 PTR_ERR(pid)
  └─ pidfd_getfd(pid, fd)                                  // 核心实现
       ├─ task = get_pid_task(pid, PIDTYPE_PID)            // 从 pid 获取 task_struct
       │    找不到 → 返回 -ESRCH
       ├─ file = __pidfd_fget(task, fd)                    // 从目标进程获取 fd 对应的 file
       │    无效 fd → 返回 -EBADF
       ├─ put_task_struct(task)                            // 释放 task 引用
       ├─ ret = receive_fd(file, NULL, O_CLOEXEC)          // 接收 fd 到当前进程
       │    └─ 创建一个新的 fd 指向同一 file 结构体
       └─ fput(file) / return ret
```

### 3.1 权限检查

`pidfd_getfd` 隐式地要求调用者具有对目标进程的 ptrace 权限。`__pidfd_fget()` 内部会检查 `ptrace_may_access()`：

```c
// kernel/pid.c
static struct file *__pidfd_fget(struct task_struct *task, int fd)
{
    struct file *file;
    // 检查 ptrace 权限
    if (!ptrace_may_access(task, PTRACE_MODE_ATTACH_REALCREDS))
        return ERR_PTR(-EPERM);
    file = fget_task(task, fd);
    if (!file)
        return ERR_PTR(-EBADF);
    return file;
}
```

## 4. 关键数据结构

### 4.1 pidfd 文件

pidfd 是一个指向进程的匿名文件描述符，通过 `pidfd_open()` 创建。其底层文件操作对应于 `pidfd_fops`，文件私有数据包含指向 `struct pid` 的引用。

### 4.2 receive_fd 机制

`receive_fd()` 是内核的通用 fd 接收机制，用于将 `struct file` 安装到当前进程的文件描述符表中。它分配一个新的 fd 编号，在 `files_struct` 中建立 fd → file 的映射。

## 5. 流程图

```
用户态调用 pidfd_getfd(pidfd, fd, flags)
    │
    ▼
┌─────────────────────────────────────────┐
│  flags != 0 → 返回 -EINVAL              │
│  pidfd 无效 → 返回 -EBADF               │
│  pidfd 不是 pidfd 文件 → 返回 -EBADF    │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  pidfd_pid() 获取 struct pid            │
│  → 失败返回错误码                        │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  get_pid_task(pid, PIDTYPE_PID)         │
│  → 获取目标进程的 task_struct           │
│  → 找不到 → 返回 -ESRCH                 │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  __pidfd_fget(task, fd)                 │
│  ├─ ptrace_may_access() 权限检查        │
│  │    无权限 → 返回 -EPERM              │
│  ├─ fget_task(task, fd) 获取 file       │
│  │    fd 无效 → 返回 -EBADF             │
│  └─ 返回 file 指针                      │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  receive_fd(file, NULL, O_CLOEXEC)      │
│  → 在当前进程安装 fd 副本               │
│  → 返回新 fd 编号                       │
└─────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | `flags` 非零 |
| `-EBADF` | 无效 fd | `pidfd` 无效，或目标进程的 `fd` 无效 |
| `-ESRCH` | 进程不存在 | pidfd 对应的进程已退出 |
| `-EPERM` | 权限不足 | 调用者没有 ptrace 目标进程的权限 |
| `-EMFILE` | 文件描述符表满 | 当前进程的文件描述符表已满 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>

int main(void)
{
    int pipefd[2];
    pipe(pipefd);

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程：关闭读端，写入数据 */
        close(pipefd[0]);
        write(pipefd[1], "hello", 5);
        pause();
        exit(0);
    }

    /* 父进程 */
    close(pipefd[1]);  // 关闭写端

    // 通过 pidfd_open 获取子进程的 pidfd
    int pidfd = syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        perror("pidfd_open");
        kill(pid, SIGKILL);
        wait(NULL);
        return 1;
    }

    // 通过 pidfd_getfd 获取子进程的写端 fd 副本
    int child_fd = syscall(SYS_pidfd_getfd, pidfd, pipefd[1], 0);
    if (child_fd < 0) {
        perror("pidfd_getfd");
        close(pidfd);
        kill(pid, SIGKILL);
        wait(NULL);
        return 1;
    }

    printf("Got child's fd: %d\n", child_fd);

    // 现在可以通过 child_fd 从管道的写端写数据
    write(child_fd, "world", 5);

    // 通过原始读端读取
    char buf[16] = {0};
    read(pipefd[0], buf, sizeof(buf));
    printf("Read: %s\n", buf);  // 输出 "helloworld"

    kill(pid, SIGKILL);
    wait(NULL);
    close(pidfd);
    close(child_fd);
    close(pipefd[0]);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#用户与组关系)
- 源码: `kernel/pid.c`（`SYSCALL_DEFINE3(pidfd_getfd)` 和 `pidfd_getfd()`）
- 相关系统调用: `pidfd_open()`, `pidfd_send_signal()`, `kcmp()`
- 测试用例: `tools/testing/selftests/pidfd/pidfd_getfd_test.c`