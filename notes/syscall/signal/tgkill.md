# tgkill 系统调用分析

## 1. 概述

`tgkill()` 系统调用向指定线程组中的特定线程发送信号。与 `kill()` 不同，`tgkill()` 要求同时提供线程组 ID（tgid）和线程 ID（tid），从而避免 PID 重用导致的误投递问题。它比 `tkill()` 多一层线程组验证。

**原型：**

```c
SYSCALL_DEFINE3(tgkill, pid_t, tgid, pid_t, pid, int, sig)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| tgid | pid_t | 目标线程组 ID |
| pid | pid_t | 目标线程 ID |
| sig | int | 信号编号（0 用于检查权限） |

## 2. 使用场景

- 精确线程信号发送：向多线程程序中的特定线程发送信号
- 线程管理：向特定线程发送 SIGCANCEL 等线程管理信号
- 调试器：ptrace 调试时向特定线程发送信号
- 确保信号送达期望的线程（避免 PID 重用问题）

## 3. 函数调用链

```
tgkill(tgid, pid, sig)                               // kernel/signal.c:4165
  │
  ├─ pid <= 0 || tgid <= 0 → -EINVAL
  │
  └─ do_tkill(tgid, pid, sig)                        // kernel/signal.c:4146
       │
       ├─ prepare_kill_siginfo(sig, &info, PIDTYPE_PID)  // 构造 siginfo
       │
       └─ do_send_specific(tgid, pid, sig, &info)    // kernel/signal.c:4117
            │
            ├─ rcu_read_lock()
            ├─ p = find_task_by_vpid(pid)             // 通过 tid 查找线程
            ├─ 检查 tgid == task_tgid_vnr(p)           // 验证线程组归属
            │
            ├─ check_kill_permission(sig, info, p)     // 权限检查
            │
            ├─ if (!error && sig) {
            │    └─ do_send_sig_info(sig, info, p, PIDTYPE_PID)  // 发送到线程私有 pending
            │         └─ __send_signal(sig, info, p, PIDTYPE_PID, false)
            │              ├─ pending = &p->pending    // 线程私有信号队列
            │              ├─ sig_addset(&pending->signal, sig)
            │              ├─ alloc_sigqueue() 入队
            │              └─ complete_signal(sig, p, PIDTYPE_PID)
            │                   └─ signal_wake_up(t, 0)  // 唤醒特定线程
            │   }
            └─ rcu_read_unlock()
```

## 4. 关键数据结构

```c
// 线程私有信号队列（每个线程独立）
struct task_struct {
    struct sigpending pending;           // 线程私有 pending（tgkill 目标）
    pid_t pid;                           // 线程 ID (tid)
    pid_t tgid;                          // 线程组 ID (pid/tgid)
    struct signal_struct *signal;        // 共享信号结构
};

// 线程组共享信号队列
struct signal_struct {
    struct sigpending shared_pending;    // 共享 pending（kill 目标）
    pid_t leader;                        // 线程组 leader 的 pid
};

// 信号类型枚举
enum pid_type {
    PIDTYPE_PID,         // 线程私有（tkill/tgkill 用）
    PIDTYPE_TGID,        // 线程组（kill 用）
    PIDTYPE_PGID,        // 进程组
    PIDTYPE_SID,         // 会话
};
```

## 5. 流程图

```
用户态: tgkill(tgid, tid, sig)
    │
    ▼
SYSCALL_DEFINE3(tgkill)
    │
    ├─ 参数检查 (pid ≤ 0 || tgid ≤ 0) → -EINVAL
    │
    └─ do_tkill(tgid, pid, sig)
         │
         ├─ prepare_kill_siginfo()  // 构造 siginfo，si_code = SI_TKILL
         │
         └─ do_send_specific(tgid, pid, sig, &info)
              │
              ├─ find_task_by_vpid(pid)  // 查找线程
              │
              ├─ 验证 task_tgid_vnr(p) == tgid  // 确保属于同一线程组
              │   └─ 不匹配 → -ESRCH
              │
              ├─ check_kill_permission()  // 权限检查
              │
              └─ do_send_sig_info(sig, info, p, PIDTYPE_PID)
                   └─ __send_signal(sig, info, p, PIDTYPE_PID, false)
                        │
                        ├─ pending = &p->pending  // 线程私有 pending
                        │
                        ├─ legacy_queue() 检查不可靠信号
                        │
                        ├─ sig_addset() 位图置位
                        │
                        ├─ alloc_sigqueue() 入队
                        │
                        └─ complete_signal()
                             └─ signal_wake_up(t, 0)
                                  └─ t->state = TASK_RUNNING
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效参数 | pid ≤ 0 或 tgid ≤ 0 或 sig 无效 |
| EPERM | 权限不足 | 调用者无权向目标线程发送信号 |
| ESRCH | 目标不存在 | 指定的 tid 不存在，或 tgid 不匹配 |
| EAGAIN | 资源不足 | 无法分配 sigqueue 结构（实时信号） |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

void *thread_func(void *arg)
{
    printf("Thread (TID: %ld) running...\n", syscall(SYS_gettid));
    pause();
    printf("Thread continuing after signal\n");
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, thread_func, NULL);

    sleep(1);  // 等待线程启动

    pthread_t tid = pthread_self();  // 获取线程标识
    /* 注意：实际使用中需要获取线程的 tid，这里仅为演示 */

    printf("Main thread sending SIGUSR1\n");

    /* 使用 tgkill 发送信号
     * syscall(SYS_tgkill, getpid(), target_tid, SIGUSR1);
     */

    pthread_join(thread, NULL);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`do_tkill()` - 核心实现
- kernel/signal.c:`do_send_specific()` - 线程特定信号发送