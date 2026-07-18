# setns 系统调用分析

## 1. 概述

`setns` 将当前进程关联到指定文件描述符所代表的命名空间，从而加入该命名空间。支持通过命名空间文件（`/proc/<pid>/ns/` 中的文件）或 pidfd 来切换命名空间。

**原型：**

```c
SYSCALL_DEFINE2(setns, int, fd, int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `fd` | `int` | 命名空间文件描述符（如 `/proc/PID/ns/net`）或 pidfd |
| `flags` | `int` | 命名空间类型（如 `CLONE_NEWNET`、`CLONE_NEWNS` 等），0 表示自动检测 |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- 容器管理工具（如 `nsenter`）加入指定容器的命名空间
- 调试工具检查进程的命名空间隔离
- 网络命名空间管理（如将进程移入特定网络命名空间）
- 容器运行时配置

## 3. 函数调用栈

```
SYSCALL_DEFINE2(setns, fd, flags)                        // kernel/nsproxy.c
  ├─ CLASS(fd, f)(fd)                                      // 通过 fd 获取 struct fd
  ├─ fd_empty(f) → 返回 -EBADF
  ├─ [proc_ns_file(fd_file(f))]                            // 情况1: 命名空间文件
  │    ├─ ns = get_proc_ns(file_inode(fd_file(f)))         // 获取命名空间信息
  │    ├─ [flags && ns->ns_type != flags] → 返回 -EINVAL   // 类型不匹配
  │    └─ flags = ns->ns_type                              // 使用命名空间类型
  ├─ [else if pidfd_pid(fd_file(f))]                       // 情况2: pidfd
  │    └─ check_setns_flags(flags)                         // 检查 flags 是否有效
  ├─ else → 返回 -EINVAL                                   // 既不是 ns 文件也不是 pidfd
  ├─ prepare_nsset(flags, &nsset)                          // 准备命名空间集合
  ├─ [proc_ns_file → validate_ns(&nsset, ns)]              // 验证单个命名空间
  │   否则 → validate_nsset(&nsset, pidfd_pid(...))        // 验证多个命名空间
  ├─ commit_nsset(&nsset)                                  // 提交命名空间切换
  │    ├─ switch_task_namespaces(current, new_ns)          // 切换 nsproxy
  │    └─ 更新其他相关字段（如 fs、pid 等）
  ├─ perf_event_namespaces(current)                        // 记录命名空间事件
  ├─ put_nsset(&nsset)
  └─ return 0
```

### 3.1 命名空间文件结构

`/proc/<pid>/ns/` 目录下包含以下命名空间文件：

| 文件 | 命名空间类型 | 说明 |
|------|-------------|------|
| `cgroup` | `CLONE_NEWCGROUP` | cgroup 命名空间 |
| `ipc` | `CLONE_NEWIPC` | IPC 命名空间 |
| `net` | `CLONE_NEWNET` | 网络命名空间 |
| `mnt` | `CLONE_NEWNS` | 挂载命名空间 |
| `pid` | `CLONE_NEWPID` | PID 命名空间 |
| `pid_for_children` | `CLONE_NEWPID` | 子进程 PID 命名空间 |
| `time` | `CLONE_NEWTIME` | 时间命名空间 |
| `user` | `CLONE_NEWUSER` | 用户命名空间 |
| `uts` | `CLONE_NEWUTS` | UTS 命名空间 |

## 4. 关键数据结构

### 4.1 struct nsproxy（命名空间代理）

```c
// include/linux/nsproxy.h
struct nsproxy {
    atomic_t count;                      // 引用计数
    struct uts_namespace *uts_ns;        // UTS 命名空间（主机名、域名）
    struct ipc_namespace *ipc_ns;        // IPC 命名空间
    struct mnt_namespace *mnt_ns;        // 挂载命名空间
    struct pid_namespace *pid_ns_for_children; // 子进程 PID 命名空间
    struct net *net_ns;                  // 网络命名空间
    struct time_namespace *time_ns;      // 时间命名空间
    struct time_namespace *time_ns_for_children; // 子进程时间命名空间
    struct cgroup_namespace *cgroup_ns;  // cgroup 命名空间
};
```

### 4.2 struct ns_common（命名空间公共结构）

```c
// include/linux/ns_common.h
struct ns_common {
    atomic_t stashed;              // 内部状态
    struct proc_ns_operations *ops; // 命名空间操作函数表
    unsigned int inum;             // 命名空间 inode 编号（唯一标识符）
};
```

## 5. 流程图

```
用户态调用 setns(fd, flags)
    │
    ▼
┌─────────────────────────────────────────┐
│  通过 fd 获取文件                       │
│  fd_empty → 返回 -EBADF                 │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  判断 fd 类型                            │
│  ├─ 命名空间文件 (proc_ns_file)         │
│  │    → 获取命名空间类型和对象           │
│  ├─ pidfd (pidfd_pid)                   │
│  │    → 检查 flags 参数                 │
│  └─ 其他 → 返回 -EINVAL                 │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  prepare_nsset(flags, &nsset)           │
│  → 创建新的 nsproxy 副本                │
│  → 根据 flags 替换指定的命名空间         │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  验证 (validate_ns / validate_nsset)    │
│  → 检查权限（CAP_SYS_ADMIN）            │
│  → 验证命名空间兼容性                    │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  commit_nsset(&nsset)                   │
│  → switch_task_namespaces()             │
│  → 更新 current->nsproxy               │
│  → 记录 perf 事件                       │
└─────────────────────────────────────────┘
    │
    ▼
  返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EBADF` | 无效 fd | `fd` 不是有效的文件描述符 |
| `-EINVAL` | 无效参数 | 文件不是有效的命名空间文件或 pidfd，或 `flags` 与命名空间类型不匹配 |
| `-EPERM` | 权限不足 | 调用者没有 `CAP_SYS_ADMIN` 能力 |
| `-EACCES` | 访问被拒绝 | 命名空间访问控制拒绝 |
| `-ENOMEM` | 内存不足 | 内核分配内存失败 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sched.h>

int main(void)
{
    // 打开目标进程的网络命名空间文件
    int fd = open("/proc/1/ns/net", O_RDONLY);
    if (fd < 0) {
        perror("open ns file");
        return 1;
    }

    printf("Joining network namespace of PID 1...\n");

    // 切换到该网络命名空间
    if (syscall(SYS_setns, fd, CLONE_NEWNET) == -1) {
        perror("setns");
        close(fd);
        return 1;
    }

    printf("Successfully joined the network namespace\n");

    close(fd);
    return 0;
}
```

### nsenter 命令等价实现

```c
// 使用 pidfd 的方式（需要较新内核）
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <target_pid>\n", argv[0]);
        return 1;
    }

    pid_t target_pid = atoi(argv[1]);

    // 通过 pidfd_open 获取目标进程的 pidfd
    int pidfd = syscall(SYS_pidfd_open, target_pid, 0);
    if (pidfd < 0) {
        perror("pidfd_open");
        return 1;
    }

    // 通过 pidfd 加入目标进程的所有命名空间
    // flags = 0 表示自动检测并加入所有命名空间
    if (syscall(SYS_setns, pidfd, 0) == -1) {
        perror("setns via pidfd");
        close(pidfd);
        return 1;
    }

    printf("Joined all namespaces of PID %d\n", target_pid);
    close(pidfd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#用户与组关系)
- 源码: `kernel/nsproxy.c`（`SYSCALL_DEFINE2(setns)`）
- 头文件: `include/linux/nsproxy.h`, `include/linux/proc_ns.h`
- 相关系统调用: `unshare()`, `clone()`, `pidfd_open()`, `listns()`
- 用户态命令: `nsenter`（util-linux 包）