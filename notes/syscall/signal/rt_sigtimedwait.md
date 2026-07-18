# rt_sigtimedwait 系统调用分析

## 1. 概述

`rt_sigtimedwait()` 同步等待指定信号集中的信号到达，并可选择设置超时时间。与 `sigsuspend()` 不同，它不执行信号处理函数，而是直接返回信号信息，并支持超时机制。该函数是 POSIX `sigtimedwait()` 的 Linux 实现。

**原型：**

```c
SYSCALL_DEFINE4(rt_sigtimedwait, const sigset_t __user *, uthese,
                siginfo_t __user *, uinfo,
                const struct __kernel_timespec __user *, uts,
                size_t, sigsetsize)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| uthese | sigset_t* | 要等待的信号集 |
| uinfo | siginfo_t* | 输出接收到的信号信息（可为 NULL） |
| uts | timespec* | 超时时间（可为 NULL，表示无限等待） |
| sigsetsize | size_t | sigset_t 的大小（用于兼容性检查） |

## 2. 使用场景

- 同步等待信号：不注册信号处理函数，直接等待信号
- 实时信号处理：获取实时信号附带的数据（si_value）
- 超时控制：在指定时间内等待信号，超时则返回
- 替代 signalfd + epoll 的轻量级方案

## 3. 函数调用链

```
rt_sigtimedwait(uthese, uinfo, uts, sigsetsize)   // kernel/signal.c:3802
  │
  ├─ sigsetsize != sizeof(sigset_t) → -EINVAL
  ├─ copy_from_user(&these, uthese, sizeof(these))
  ├─ 若 uts 非空：get_timespec64(&ts, uts)
  │
  └─ do_sigtimedwait(&these, &info, &ts)            // kernel/signal.c:3743
       │
       ├─ 签名：do_sigtimedwait(which, info, ts)
       │
       ├─ mask = *which
       ├─ 移除 SIGKILL 和 SIGSTOP（不可阻塞）
       ├─ signotset(&mask)  // 取反：需要等待的信号集
       │
       ├─ spin_lock_irq(&tsk->sighand->siglock)
       ├─ sig = dequeue_signal(&mask, info, &type)  // 尝试取出信号
       │
       ├─ if (!sig && timeout) {
       │   ├─ tsk->real_blocked = tsk->blocked      // 保存当前掩码
       │   ├─ current->blocked &= mask               // 临时解除等待信号的阻塞
       │   ├─ recalc_sigpending()
       │   ├─ spin_unlock_irq
       │   │
       │   ├─ __set_current_state(TASK_INTERRUPTIBLE | TASK_FREEZABLE)
       │   ├─ schedule_hrtimeout_range(to, slack, HRTIMER_MODE_REL)  // 睡眠
       │   │
       │   ├─ spin_lock_irq
       │   ├─ __set_task_blocked(tsk, &tsk->real_blocked)  // 恢复掩码
       │   ├─ sig = dequeue_signal(&mask, info, &type)     // 再次尝试
       │   }
       │
       └─ spin_unlock_irq(&tsk->sighand->siglock)
       │
       ├─ if (sig) return sig
       └─ return ret ? -EINTR : -EAGAIN
```

## 4. 关键数据结构

```c
// 超时时间使用 hrtimer 高精度定时器
struct timespec64 {
    time64_t tv_sec;          // 秒
    long tv_nsec;             // 纳秒（0~999999999）
};

// 信号出队函数 dequeue_signal():
// 从 pending 队列中取出第一个匹配 mask 的信号
// 如果是实时信号（sig >= SIGRTMIN），从链表取最早入队的
// 如果是标准信号（sig < SIGRTMIN），从位图取最小编号
```

## 5. 流程图

```
用户态: sigtimedwait(&set, &info, &timeout)
    │
    ▼
rt_sigtimedwait(uthese, uinfo, uts, sigsetsize)
    │
    ├─ 从用户空间拷贝信号集和超时时间
    │
    └─ do_sigtimedwait(&these, &info, &ts)
         │
         ├─ 取反信号集：等待的信号 = 全集 - 这些信号
         │              (SIGKILL/SIGSTOP 强制排除)
         │
         ├─ spin_lock_irq
         │
         ├─ dequeue_signal() ─── 有信号？──→ 返回信号编号
         │
         │  无信号且超时 > 0
         │
         ├─ 临时解除等待信号的阻塞
         │
         ├─ spin_unlock_irq
         │
         ├─ 进程睡眠（schedule_hrtimeout_range）
         │   │
         │   ├─ 信号到达 → 唤醒
         │   ├─ 超时 → 唤醒
         │   └─ 其他 → 继续
         │
         ├─ spin_lock_irq
         ├─ 恢复原始阻塞掩码
         │
         ├─ dequeue_signal() ── 有信号？──→ 返回信号编号
         │
         │  无信号
         │
         └─ 超时返回 -EAGAIN，被信号中断返回 -EINTR
```

## 6. 错误处理

| 返回值 | 含义 | 触发条件 |
|--------|------|----------|
| 正数 | 信号编号 | 成功接收到信号，返回信号编号 |
| EAGAIN | 超时 | 超时时间内没有信号到达 |
| EINTR | 被中断 | 被其他信号中断（非等待集中的信号） |
| EINVAL | 无效参数 | sigsetsize 不匹配或超时值无效 |
| EFAULT | 内存错误 | 用户空间指针无效 |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void)
{
    sigset_t set;
    siginfo_t info;
    struct timespec timeout;

    /* 阻塞 SIGINT 和 SIGRTMIN */
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &set, NULL);

    printf("Waiting for SIGINT or SIGRTMIN (timeout: 5s)...\n");
    printf("Try: kill -INT %d or kill -RTMIN %d\n", getpid(), getpid());

    /* 设置 5 秒超时 */
    timeout.tv_sec = 5;
    timeout.tv_nsec = 0;

    int sig = sigtimedwait(&set, &info, &timeout);

    if (sig > 0) {
        printf("Received signal %d", sig);
        if (sig == SIGRTMIN)
            printf(", si_value.sival_int = %d", info.si_value.sival_int);
        printf("\n");
    } else if (sig == -1 && errno == EAGAIN) {
        printf("Timeout: no signal received within 5s\n");
    } else {
        perror("sigtimedwait");
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`do_sigtimedwait()` - 核心实现
- kernel/signal.c:`dequeue_signal()` - 信号出队函数