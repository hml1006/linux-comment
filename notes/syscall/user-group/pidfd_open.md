# pidfd_open 系统调用分析

## 1. 概述

`pidfd_open` 打开一个进程文件描述符（pidfd），该文件描述符指向指定 PID 的进程。pidfd 提供了一种无竞争的方式引用进程，避免了传统 PID 重用问题。

**原型：**

```c
SYSCALL_DEFINE2(pidfd_open, pid_t, pid, unsigned int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `pid` | `pid_t` | 目标进程的 PID（必须 > 0） |
| `flags` | `unsigned int` | 标志位（`PIDFD_NONBLOCK` 或 `PIDFD_THREAD`） |

**返回值：**
- 成功返回 pidfd 文件描述符
- 失败返回负的错误码

## 2. 使用场景

- `pidfd_getfd()`、`pidfd_send_signal()` 等 pidfd 系列系统调用的前置操作
- 进程管理工具（如 `waitid()` 的 `P_PIDFD` 模式）
- 避免 PID 重用问题的进程监控
- 容器和系统管理

## 3. 函数调用栈

```
SYSCALL_DEFINE2(pidfd_open, pid, flags)                  // kernel/pid.c
  ├─ flags & ~(PIDFD_NONBLOCK | PIDFD_THREAD) → 返回 -EINVAL
  ├─ pid <= 0 → 返回 -EINVAL
  ├─ p = find_get_pid(pid)                                // 通过 PID 查找 struct pid
  │    找不到pid → 返回 -ESRCH
  ├─ fd = pidfd_create(p, flags)                          // 创建 pidfd
  │    ├─ anon_inode_getfile("[pidfd]", &pidfd_fops, ...)  // 创建匿名文件
  │    │    └─ 返回 struct file，操作函数为 pidfd_fops
  │    ├─ fd_install(fd, file)                             // 安装 fd 到当前进程
  │    └─ return fd
  ├─ put_pid(p)                                            // 释放 pid 引用
  └─ return fd
```

### 3.1 pidfd_create 详细流程

```c
// kernel/pid.c
static int pidfd_create(struct pid *pid, unsigned int flags)
{
    int fd;
    struct file *file;
    // 分配新的 fd 编号
    fd = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
    // 创建匿名 inode 文件
    // pidfd_fops 定义了 pidfd 的行为（poll, release, 等）
    file = anon_inode_getfile("[pidfd]", &pidfd_fops, pid,
                              O_RDWR | (flags & PIDFD_NONBLOCK ? O_NONBLOCK : 0));
    // 将 pid 和 file 关联
    pidfd_set_pid(file, pid);
    // 安装 fd
    fd_install(fd, file);
    return fd;
}
```

### 3.2 pidfd 文件操作

```c
// kernel/pid.c
static const struct file_operations pidfd_fops = {
    .release = pidfd_release,    // 释放 pidfd 时减少 pid 引用计数
    .poll = pidfd_poll,          // 支持 poll/select 等待进程状态变化
};
```

## 4. 关键数据结构

### 4.1 pidfd 标志位

```c
// include/uapi/linux/pidfd.h
#define PIDFD_NONBLOCK  O_NONBLOCK  // 非阻塞模式
#define PIDFD_THREAD    (1 << 2)    // 允许对线程组中任意线程操作（而非仅线程组 leader）
```

### 4.2 struct pid（进程 ID 内核结构）

```c
// include/linux/pid.h
struct pid {
    struct hlist_node hash;         // PID 哈希表节点
    struct hlist_head tasks[PIDTYPE_MAX]; // 各类型 PID 对应的 task 链表
    struct rcu_head rcu;            // RCU 回调
    int count;                      // 引用计数
    unsigned int level;             // 命名空间层级
    struct pid_namespace *ns;       // 所属 PID 命名空间
    // ... 更多字段
};
```

## 5. 流程图

```
用户态调用 pidfd_open(pid, flags)
    │
    ▼
┌─────────────────────────────────────┐
│  参数校验                           │
│  flags 无效 → 返回 -EINVAL          │
│  pid <= 0 → 返回 -EINVAL            │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  find_get_pid(pid)                  │
│  → 查找 struct pid                  │
│  → 找不到 → 返回 -ESRCH             │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  pidfd_create(pid, flags)           │
│  ├─ get_unused_fd_flags()           │  ← 分配新 fd
│  ├─ anon_inode_getfile("[pidfd]",  │
│  │    &pidfd_fops, pid, ...)       │  ← 创建匿名文件
│  ├─ fd_install(fd, file)           │  ← 安装 fd
│  └─ return fd                      │
└─────────────────────────────────────┘
    │
    ▼
  put_pid(pid)  // 释放引用
  return fd
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | `flags` 包含未定义的位，或 `pid <= 0` |
| `-ESRCH` | 进程不存在 | `find_get_pid()` 找不到指定 PID 的进程 |
| `-EMFILE` | 文件描述符表满 | 当前进程的 fd 数已达上限 |
| `-ENOMEM` | 内存不足 | 内核分配内存失败 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>

int main(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程 */
        printf("Child PID: %d\n", getpid());
        pause();
        exit(0);
    }

    /* 父进程 */

    // 打开 pidfd
    int pidfd = syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        perror("pidfd_open");
        kill(pid, SIGKILL);
        wait(NULL);
        return 1;
    }

    printf("pidfd: %d\n", pidfd);

    // 通过 pidfd 发送信号
    if (syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, NULL, 0) == 0) {
        printf("Signal sent via pidfd\n");
    }

    // 使用 pidfd 等待（需配合 waitid 的 P_PIDFD 标志）
    siginfo_t info;
    if (waitid(P_PIDFD, pidfd, &info, WEXITED) == 0) {
        printf("Child exited with status %d\n", info.si_status);
    }

    close(pidfd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#用户与组关系)
- 源码: `kernel/pid.c`（`SYSCALL_DEFINE2(pidfd_open)` 和 `pidfd_create()`）
- 头文件: `include/uapi/linux/pidfd.h`
- 相关系统调用: `pidfd_getfd()`, `pidfd_send_signal()`, `waitid()`
- 测试用例: `tools/testing/selftests/pidfd/`