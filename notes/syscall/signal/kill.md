# kill 系统调用分析

## 1. 概述

`kill()` 系统调用用于向指定进程或进程组发送信号。它是 Linux 信号发送机制中最基础的接口，支持发送到单个进程、进程组或所有进程（除 init 外）。

**原型：**

```c
SYSCALL_DEFINE2(kill, pid_t, pid, int, sig)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| pid | pid_t | 目标进程 ID（取值含义见下表） |
| sig | int | 信号编号（0 用于检查权限而不发送信号） |

**pid 参数取值规则：**

| pid 值 | 目标 |
|--------|------|
| pid > 0 | 发送给 PID 为 pid 的进程 |
| pid == 0 | 发送给当前进程所属进程组的所有进程 |
| pid == -1 | 发送给所有进程（除 init 和当前进程外） |
| pid < -1 | 发送给进程组 ID 为 -pid 的所有进程 |

## 2. 使用场景

- 进程间通信：向其他进程发送 SIGTERM、SIGKILL 等终止信号
- 进程管理：`kill -9 <pid>` 强制终止进程
- 权限检查：`kill(pid, 0)` 检查目标进程是否存在以及是否具有发送信号权限
- 作业控制：向进程组发送 SIGSTOP、SIGCONT 等信号
- 父子进程管理：子进程退出时向父进程发送 SIGCHLD

## 3. 函数调用链

```
kill(pid, sig) → kernel_signal.c
  ├─ pid > 0 → kill_proc_info(sig, info, pid)
  │    └─ group_send_sig_info(sig, info, p, PIDTYPE_TGID)
  │         └─ __send_signal(sig, info, p, PIDTYPE_TGID, false)
  │              ├─ pending = &p->signal->shared_pending    // 共享信号队列
  │              ├─ legacy_queue(pending, sig)               // 不可靠信号检查
  │              ├─ sig_addset(&pending->signal, sig)        // 位图置位
  │              ├─ 若 SIGQUEUE 类型 → alloc_sigqueue 入队   // 携带信息的信号入队
  │              └─ complete_signal(sig, p, PIDTYPE_TGID)    // 递送决策
  │                   ├─ signal_wake_up(t, resume)           // 唤醒目标线程
  │                   │    └─ t->state = TASK_RUNNING
  │                   │    └─ kick_process / ttwu_queue
  │                   └─ 若信号被阻塞 → 标记 pending（延迟递送）
  ├─ pid == 0 → __kill_pgrp_info(sig, info, task_pgrp(current))  // 进程组
  ├─ pid == -1 → for_each_process(p) group_send_sig_info(...)    // 全局（除 init）
  └─ pid < -1 → kill_pid_info(sig, info, find_vpid(-pid))
```

## 4. 关键数据结构

```c
// 信号发送信息结构
struct kernel_siginfo {
    int si_signo;          // 信号编号
    int si_errno;          // 错误号（通常为 0）
    int si_code;           // 信号来源码（SI_USER, SI_KILL, SI_TKILL 等）
    union {
        // 不同 si_code 对应不同的联合体成员
        struct { ... } _kill;    // kill() 发送
        struct { ... } _timer;   // POSIX 定时器
        struct { ... } _rt;      // 实时信号
        struct { ... } _sigchld; // 子进程状态变更
        struct { ... } _sigfault; // 内存访问错误
    };
};

// 信号待处理队列（每个进程两个：线程私有 + 共享）
struct sigpending {
    struct list_head list;        // sigqueue 链表
    sigset_t signal;              // 待处理信号位图
};

// sigqueue（信号队列条目）
struct sigqueue {
    struct list_head list;        // 链表指针
    int flags;                    // 标志位
    kernel_siginfo_t info;        // 信号信息
    struct user_struct *user;     // 用户资源跟踪
};
```

## 5. 流程图

```
用户态调用 kill(pid, sig)
    │
    ▼
SYSCALL_DEFINE2(kill, pid, sig)
    │
    ├─ prepare_kill_siginfo(sig, &info)  // 构造内核 siginfo
    │
    └─ kill_something_info(sig, &info, pid)
         │
         ├── pid > 0 ──────────────────┐
         │    └─ kill_proc_info()       │
         │                              │
         ├── pid == 0 ─────────────────┤
         │    └─ __kill_pgrp_info()     │
         │         └─ 当前进程组         │
         │                              ├──→ group_send_sig_info()
         ├── pid == -1 ────────────────┤         │
         │    └─ for_each_process()     │    do_send_sig_info()
         │                              │         │
         └── pid < -1 ─────────────────┘    __send_signal()
              └─ kill_pid_info()                    │
                   └─ 进程组 ID = -pid       ├─ 信号位图置位
                                              ├─ sigqueue 入队
                                              └─ complete_signal()
                                                     │
                                              ┌──────┴──────┐
                                              ▼              ▼
                                        signal_wake_up()   阻塞 → 标记 pending
                                              │
                                        ┌─────┴──────┐
                                        ▼             ▼
                                   TASK_RUNNING    kick_process()
                                   (直接唤醒)      (IPI 远程唤醒)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效信号 | sig 不在 1~_NSIG 范围内 |
| EPERM | 权限不足 | 调用者无权向目标进程发送信号 |
| ESRCH | 目标不存在 | 指定的 pid 对应的进程不存在 |
| 0 (sig=0) | 进程存在但有权限 | 使用空信号进行探测 |

权限检查由 `check_kill_permission()` 实现：
- 调用者必须有 CAP_KILL 能力，或者
- 调用者的 real/euid 等于目标进程的 real/suid（即拥有相同用户）

## 7. 使用示例

```c
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* 子进程：等待信号 */
        printf("Child (PID: %d) waiting for signal...\n", getpid());
        pause();
        printf("Child received signal, exiting.\n");
        exit(0);
    }

    /* 父进程：发送信号 */
    sleep(1);
    printf("Parent sending SIGTERM to child (PID: %d)\n", pid);
    kill(pid, SIGTERM);

    /* 检查进程是否存在 */
    if (kill(pid, 0) == -1) {
        perror("Child no longer exists");
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`kill_something_info()` - 信号发送主路径
- kernel/signal.c:`__send_signal()` - 信号入队核心函数
- kernel/signal.c:`complete_signal()` - 信号递送决策