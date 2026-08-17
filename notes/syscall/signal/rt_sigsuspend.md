# rt_sigsuspend 系统调用分析

## 1. 概述

`rt_sigsuspend()` 用于临时替换进程的信号掩码，然后挂起进程等待信号。当信号到达且信号处理函数执行完毕后，系统调用返回（实际上是 `-EINTR` 并触发 `ERESTARTNOHAND` 重启机制）。该调用是 `sigsuspend()` 的 POSIX 实时信号扩展版本。

**原型：**

```c
SYSCALL_DEFINE2(rt_sigsuspend, sigset_t __user *, unewset, size_t, sigsetsize)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| unewset | sigset_t* | 临时替换的信号掩码 |
| sigsetsize | size_t | sigset_t 的大小（用于兼容性检查） |

## 2. 使用场景

- 等待特定信号：临时解除某些信号的阻塞，等待它们到达
- 实现关键区保护 + 等待：典型模式是阻塞信号 → 执行关键区 → sigsuspend 原子性等待
- 替代 `pause()` 的精细控制版本

## 3. 函数调用链

```
rt_sigsuspend(unewset, sigsetsize)                // kernel/signal.c:4850
  │
  ├─ sigsetsize != sizeof(sigset_t) → -EINVAL
  │
  ├─ copy_from_user(&newset, unewset, sizeof(newset))
  │
  └─ sigsuspend(&newset)                           // kernel/signal.c:4831
       │
       ├─ current->saved_sigmask = current->blocked  // 保存当前掩码
       │
       ├─ set_current_blocked(set)                  // 设置新的掩码
       │
       ├─ while (!signal_pending(current)) {
       │    ├─ __set_current_state(TASK_INTERRUPTIBLE)  // 设置可中断睡眠
       │    └─ schedule()                              // 调度，放弃 CPU
       │   }
       │
       ├─ set_restore_sigmask()                      // 标记需要恢复掩码
       │
       └─ return -ERESTARTNOHAND                     // 返回用户态时重启
```

## 4. 关键数据结构

```c
// task_struct 中与本调用相关的字段
struct task_struct {
    sigset_t blocked;              // 当前阻塞信号集
    sigset_t saved_sigmask;        // 保存的信号掩码（sigsuspend 使用）
    // ...
};

// TIF_RESTORE_SIGMASK 标志
// 当 set_restore_sigmask() 设置该标志后，
// 内核在返回用户态时会自动恢复 saved_sigmask 到 blocked
```

## 5. 流程图

```
用户态: sigsuspend(&mask)
    │
    ▼
rt_sigsuspend(unewset, sigsetsize)
    │
    ├─ 从用户空间拷贝新掩码
    │
    └─ sigsuspend(&newset)
         │
         ├─ saved_sigmask = current->blocked   // 保存旧掩码
         │
         ├─ current->blocked = newset           // 临时替换掩码
         │
         ├─ recalc_sigpending()                 // 重新计算 pending
         │
         ├─ loop:
         │   │
         │   ├─ signal_pending(current)?
         │   │   ├─ 是 → 跳出循环
         │   │   └─ 否 → 继续
         │   │
         │   ├─ __set_current_state(TASK_INTERRUPTIBLE)
         │   │
         │   └─ schedule()
         │       │
         │       └─ (进程睡眠，等待信号唤醒)
         │
         ├─ set_restore_sigmask()  // 标记需要恢复掩码
         │
         └─ return -ERESTARTNOHAND
              │
              └─ 返回用户态时，内核自动恢复 saved_sigmask
```

## 6. 错误处理

| 返回值 | 含义 | 说明 |
|--------|------|------|
| -EINTR | 被信号中断 | 信号处理函数执行后返回（实际是 ERESTARTNOHAND 触发） |
| -EINVAL | 无效参数 | sigsetsize 不匹配 |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

volatile int sig_received = 0;

void handler(int sig)
{
    sig_received = 1;
}

int main(void)
{
    struct sigaction sa;
    sigset_t new_mask, old_mask;

    /* 设置信号处理函数 */
    sa.sa_handler = handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    /* 阻塞 SIGUSR1 */
    sigemptyset(&new_mask);
    sigaddset(&new_mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &new_mask, &old_mask);

    /* 执行关键区操作... */
    printf("In critical section, PID=%d\n", getpid());
    printf("Send SIGUSR1 to continue...\n");

    /* 原子性地解除阻塞并等待信号 */
    sigset_t wait_mask;
    sigemptyset(&wait_mask);  // 不阻塞任何信号
    sigsuspend(&wait_mask);   // 相当于 pause() 但更安全

    printf("Woke up from sigsuspend\n");

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`sigsuspend()` - 核心实现
- kernel/signal.c:`set_restore_sigmask()` - 掩码恢复机制