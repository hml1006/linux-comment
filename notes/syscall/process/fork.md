# fork 系统调用分析

## 1. 概述

`fork` 是传统的创建子进程系统调用。在 ARM64 架构上，`fork` 没有独立的系统调用编号，而是通过 `clone`（syscall #220）实现。`fork` 实际上是 `clone` 的一个特例调用，传递 `SIGCHLD` 作为退出信号且不共享任何资源。

### 关键特点

- 创建一个与父进程几乎完全相同的子进程
- 子进程获得父进程的代码段、数据段、堆栈的副本（写时复制）
- 子进程和父进程从相同的执行点继续执行
- 返回值：子进程返回 0，父进程返回子进程 PID
- ARM64 上无独立 `fork` 系统调用号，通过 `clone(SIGCHLD, 0, NULL, NULL)` 实现

---

## 2. 函数原型

```c
#include <unistd.h>

pid_t fork(void);
```

### 内核入口

在 ARM64 上，`fork` 的 glibc 封装实际调用：

```c
syscall(__NR_clone, SIGCHLD, 0, NULL, NULL);
```

内核中对应：

```c
// kernel/fork.c:2762
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
                int __user *, parent_tidptr,
                unsigned long, tls,
                int __user *, child_tidptr)
{
    struct kernel_clone_args args = {
        .flags       = (SIGCHLD & ~CSIGNAL),  // fork 的标志: 仅 SIGCHLD
        .pidfd       = parent_tidptr,          // NULL
        .child_tid   = child_tidptr,           // NULL
        .parent_tid  = parent_tidptr,          // NULL
        .exit_signal = SIGCHLD,
        .stack       = 0,                      // 子进程共享父进程栈
        .tls         = 0,                      // 无 TLS
    };
    return kernel_clone(&args);
}
```

---

## 3. 调用链分析

### 完整调用链

```
fork()  (glibc wrapper)
└─ syscall(__NR_clone, SIGCHLD, 0, NULL, NULL)
   └─ __arm64_sys_clone(args)                    // kernel/fork.c
      └─ kernel_clone(&args)                      // kernel/fork.c:2612
         ├─ copy_process(NULL, 0, NUMA_NO_NODE, &args)  // 复制进程
         │  ├─ dup_task_struct(current, node)       // 复制 task_struct
         │  ├─ sched_cgroup_fork(p, args)           // cgroup 初始化
         │  ├─ copy_mm(CLONE_VM=0, p)               // 复制地址空间（COW）
         │  │  └─ dup_mm(tsk, current->mm)
         │  │     └─ dup_mmap(mm, oldmm)            // 复制 VMA（写时复制）
         │  ├─ copy_fs(CLONE_FS=0, p)               // 复制 fs_struct
         │  │  └─ copy_fs_struct(fs)                // 独立副本
         │  ├─ copy_files(CLONE_FILES=0, p)         // 复制 fd 表
         │  │  └─ dup_fd()                          // 独立副本
         │  ├─ copy_sighand(CLONE_SIGHAND=0, p)     // 复制信号处理函数
         │  │  └─ kmemdup(sighand)                  // 独立副本
         │  ├─ copy_signal(CLONE_THREAD=0, p)       // 复制信号结构
         │  │  └─ kmemdup(signal)                   // 独立副本
         │  ├─ copy_namespaces(0, p)                // 复制命名空间
         │  │  └─ get_nsproxy()                     // 增加引用计数
         │  ├─ copy_thread(0, args, p)              // 复制线程上下文
         │  │  └─ arch/arm64/kernel/process.c
         │  │     ├─ 子进程返回 0
         │  │     └─ 父进程上下文复制到子进程
         │  ├─ sched_fork(0, p)                     // 调度器初始化
         │  ├─ alloc_pid(p->nsproxy->pid_ns)        // 分配新 PID
         │  └─ sched_post_fork(p)                   // 调度后处理
         ├─ trace_sched_process_fork(current, p)    // tracepoint
         ├─ nr = pid_vnr(pid)                       // 子进程 PID
         ├─ wake_up_new_task(p)                     // 唤醒新进程
         │  ├─ activate_task(rq, p, 0)              // 加入就绪队列
         │  └─ check_preempt_curr(rq, p)            // 检查抢占
         └─ 返回 nr (子进程 PID 给父进程)
```

---

## 4. 关键数据结构

fork 与 clone 共享完全相同的数据结构。关键区别在于 `kernel_clone_args` 的配置：

```c
// fork 对应的 kernel_clone_args
struct kernel_clone_args fork_args = {
    .flags       = 0,                    // 无 CLONE_VM, CLONE_FILES 等
    .pidfd       = NULL,                 // 不返回 pidfd
    .child_tid   = NULL,                 // 不设置 child tid
    .parent_tid  = NULL,                 // 不设置 parent tid
    .exit_signal = SIGCHLD,              // 子进程退出时发送 SIGCHLD
    .stack       = 0,                    // 使用父进程栈
    .stack_size  = 0,
    .tls         = 0,
};
```

---

## 5. 流程图

```
                     fork()
                        |
                +-------v--------+
                | glibc wrapper   |
                | syscall(__NR_   |
                | clone, SIGCHLD, |
                | 0, NULL, NULL)  |
                +-------+--------+
                        |
                +-------v--------+
                | kernel_clone() |
                | (kernel/fork.c)|
                +-------+--------+
                        |
                +-------v--------+
                | copy_process() |
                | (复制所有资源)  |
                +-------+--------+
                        |
        +---------------+---------------+
        |                               |
+-------v-------+             +---------v--------+
| 复制子系统     |             | 分配 PID          |
| (均独立副本)   |             | alloc_pid()      |
|               |             +---------+--------+
| dup_task_     |                       |
| struct()      |             +---------v--------+
| copy_mm()     |             | wake_up_new_     |
|   (COW)       |             | task(p)          |
| copy_fs()     |             | (加入就绪队列)    |
| copy_files()  |             +---------+--------+
| copy_sighand()|                       |
| copy_signal() |             +---------v--------+
| copy_namespace|             | 父进程: 返回子PID |
| copy_thread() |             | 子进程: 返回 0   |
| sched_fork()  |             +------------------+
+---------------+ 
```

---

## 6. 错误处理

与 clone 相同，fork 通过 `kernel_clone` → `copy_process` 路径返回错误。

| 错误码 | 条件 | 说明 |
|--------|------|------|
| `-EAGAIN` | 超出 `max_threads` 限制 | 进程数过多 |
| `-ENOMEM` | 内存不足 | 无法分配 task_struct 或内核栈 |
| `-ENOMEM` | 复制内存映射时内存不足 | `dup_mmap` 失败 |

---

## 7. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>

int main() {
    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        return 1;
    } else if (pid == 0) {
        // 子进程
        printf("子进程: PID=%d, 父进程 PPID=%d\n", getpid(), getppid());
        return 42;
    } else {
        // 父进程
        printf("父进程: 子进程 PID=%d\n", pid);

        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            printf("子进程退出码: %d\n", WEXITSTATUS(status));
        }
    }

    return 0;
}
```

---

## 8. fork 与 vfork 在 ARM64 上的实现

ARM64 没有独立的 `fork` 和 `vfork` 系统调用号，两者都通过 `clone` 实现：

| 操作 | 实际调用 | 标志 |
|------|----------|------|
| `fork()` | `clone(SIGCHLD, 0, NULL, NULL, NULL)` | 仅 `SIGCHLD` |
| `vfork()` | `clone(CLONE_VFORK \| CLONE_VM \| SIGCHLD, 0, NULL, NULL, NULL)` | `CLONE_VFORK \| CLONE_VM \| SIGCHLD` |
| `clone()` | `clone(flags, stack, ...)` | 用户指定 |

核心区别：

- **fork**: 子进程拥有独立地址空间（写时复制），独立 fd 表，独立信号处理
- **vfork**: 子进程共享父进程地址空间（`CLONE_VM`），父进程阻塞直到子进程 `exec` 或 `exit`（`CLONE_VFORK`）
- **clone**: 通过 `flags` 灵活控制共享粒度

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/fork.c` - 核心实现
- `include/linux/sched/task.h` - kernel_clone_args 定义
- `include/uapi/linux/sched.h` - CLONE_* 标志定义