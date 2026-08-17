# tkill 系统调用分析

## 1. 概述

`tkill()` 系统调用向指定线程发送信号。与 `kill()` 发送到整个线程组不同，`tkill()` 只将信号发送到指定的单个线程（即使该线程属于一个多线程进程）。`tgkill()` 是它的安全版本，额外验证线程组 ID。

**原型：**

```c
SYSCALL_DEFINE2(tkill, pid_t, pid, int, sig)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| pid | pid_t | 目标线程 ID |
| sig | int | 信号编号（0 用于检查权限） |

## 2. 使用场景

- 线程级信号发送：向多线程程序中的特定线程发送信号
- 线程取消：pthread_cancel 内部使用
- 调试器信号：ptrace 调试时向特定线程发送信号
- 线程同步：在特定线程上处理信号

## 3. 函数调用链

```
tkill(pid, sig)                                      // kernel/signal.c:4181
  │
  ├─ pid <= 0 → -EINVAL
  │
  └─ do_tkill(0, pid, sig)                            // kernel/signal.c:4146
       │
       ├─ prepare_kill_siginfo(sig, &info, PIDTYPE_PID)  // si_code = SI_TKILL
       │
       └─ do_send_specific(0, pid, sig, &info)        // kernel/signal.c:4117
            │
            ├─ rcu_read_lock()
            ├─ p = find_task_by_vpid(pid)             // 查找线程
            │
            ├─ check_kill_permission(sig, info, p)     // 权限检查
            │
            ├─ if (!error && sig) {
            │    └─ do_send_sig_info(sig, info, p, PIDTYPE_PID)
            │         └─ __send_signal(sig, info, p, PIDTYPE_PID, false)
            │              ├─ pending = &p->pending    // 线程私有信号队列
            │              ├─ check rlimit 限制
            │              ├─ legacy_queue(pending, sig)  // 不可靠信号丢弃
            │              ├─ sig_addset(&pending->signal, sig)
            │              ├─ alloc_sigqueue() 入队
            │              └─ complete_signal(sig, p, PIDTYPE_PID)
            │                   └─ signal_wake_up(t, 0)  // 唤醒特定线程
            │   }
            └─ rcu_read_unlock()
```

## 4. 关键数据结构

```c
// 关键区别：tkill 使用 PIDTYPE_PID 类型，信号进入线程私有 pending
// 而 kill() 使用 PIDTYPE_TGID，信号进入线程组共享 pending

// 线程私有 pending 结构
struct task_struct {
    struct sigpending pending;           // 线程私有（tkill 目标）
    // ...
};

// 线程组共享 pending
struct signal_struct {
    struct sigpending shared_pending;    // 组共享（kill 目标）
    // ...
};
```

## 5. 流程图

```
用户态: tkill(tid, sig)
    │
    ▼
SYSCALL_DEFINE2(tkill)
    │
    ├─ pid ≤ 0 → -EINVAL
    │
    └─ do_tkill(0, pid, sig)
         │
         ├─ prepare_kill_siginfo()  // si_code = SI_TKILL
         │
         └─ do_send_specific(0, pid, sig, &info)
              │
              ├─ rcu_read_lock()
              │
              ├─ find_task_by_vpid(pid)  // 查找线程
              │   └─ 未找到 → -ESRCH
              │
              ├─ check_kill_permission(sig, info, p)
              │
              ├─ do_send_sig_info(sig, info, p, PIDTYPE_PID)
              │   │
              │   └─ __send_signal()
              │        ├─ pending = &p->pending  // 线程私有
              │        ├─ 信号位图置位
              │        ├─ sigqueue 入队
              │        └─ complete_signal()
              │             └─ signal_wake_up(t, 0)
              │
              └─ rcu_read_unlock()
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效参数 | pid ≤ 0 或 sig 无效 |
| EPERM | 权限不足 | 调用者无权向目标线程发送信号 |
| ESRCH | 目标不存在 | 指定的 tid 不存在 |
| EAGAIN | 资源不足 | 无法分配 sigqueue 结构（实时信号） |

**与 kill() 的区别：**

| 特性 | kill() | tkill() | tgkill() |
|------|--------|---------|----------|
| 目标范围 | 进程组/进程 | 单个线程 | 线程组中的指定线程 |
| PID 类型 | PIDTYPE_TGID | PIDTYPE_PID | PIDTYPE_PID |
| pending 队列 | shared_pending | 线程私有 pending | 线程私有 pending |
| 线程组验证 | 无 | 无 | 有（tgid 检查） |
| 安全性 | 低（PID 重用） | 低 | 高 |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

#ifndef SYS_tkill
#define SYS_tkill 238
#endif

int main(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* 子进程 */
        printf("Child (PID: %d, TID: %ld) waiting...\n",
               getpid(), syscall(SYS_gettid));
        pause();
        exit(0);
    }

    /* 父进程 */
    sleep(1);

    /* 直接向子进程的主线程发送信号 */
    printf("Parent sending SIGTERM to child thread\n");
    if (syscall(SYS_tkill, pid, SIGTERM) < 0) {
        perror("tkill");
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`do_tkill()` - 核心实现
- kernel/signal.c:`do_send_specific()` - 线程特定信号发送