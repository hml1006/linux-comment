# rt_sigaction 系统调用分析

## 1. 概述

`rt_sigaction()` 用于检查和修改指定信号的处理动作（信号处理函数）。它是 POSIX 标准的 `sigaction()` 在 Linux 上的实现，支持所有标准信号，包括实时信号（SIGRTMIN ~ SIGRTMAX）。

**原型：**

```c
SYSCALL_DEFINE4(rt_sigaction, int, sig,
                const struct sigaction __user *, act,
                struct sigaction __user *, oact,
                size_t, sigsetsize)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| sig | int | 信号编号（除 SIGKILL 和 SIGSTOP 外） |
| act | const sigaction* | 新的信号处理动作（可为 NULL，只查询） |
| oact | sigaction* | 输出旧的信号处理动作（可为 NULL） |
| sigsetsize | size_t | sigset_t 的大小（用于兼容性检查） |

## 2. 使用场景

- 注册信号处理函数：为特定信号设置用户态处理函数
- 查询当前信号处理：传入 act=NULL 获取当前信号处理配置
- 恢复默认行为：设置 act->sa_handler = SIG_DFL
- 忽略信号：设置 act->sa_handler = SIG_IGN
- 配置信号处理标志：SA_SIGINFO、SA_RESTART、SA_ONSTACK 等

## 3. 函数调用链

```
rt_sigaction(sig, act, oact, sigsetsize)        // kernel/signal.c:4627
  │
  ├─ sigsetsize != sizeof(sigset_t) → -EINVAL
  │
  ├─ 若 act 非空：copy_from_user(&new_act, act, sizeof(new_act))
  │
  └─ do_sigaction(sig, act ? &new_act : NULL, oact ? &old_act : NULL)
       │
       ├─ valid_signal(sig) → sig >= 1 && sig < _NSIG
       │
       ├─ k = &current->sighand->action[sig-1]    // 信号处理函数表
       │
       ├─ spin_lock_irq(&p->sighand->siglock)
       │
       ├─ 若 SA_IMMUTABLE 标志已设置 → -EINVAL
       │
       ├─ if (oact) *oact = *k                     // 保存旧 action
       │
       ├─ if (act) *k = *act                       // 设置新 action
       │    ├─ 若 SA_SIGINFO → 三参数 handler
       │    ├─ 若 SA_RESTORER → sa_restorer 函数
       │    ├─ 若 SA_ONSTACK → 使用 sigaltstack
       │    ├─ 若 SA_RESETHAND → 处理一次后恢复默认
       │    ├─ 若 SA_NODEFER → 处理期间不阻塞自身
       │    └─ 若 SA_NOCLDSTOP → 子进程停止时不通知
       │
       └─ spin_unlock_irq(&p->sighand->siglock)
```

## 4. 关键数据结构

```c
// 用户态信号处理动作结构
struct sigaction {
    union {
        __sighandler_t sa_handler;    // SIG_DFL / SIG_IGN / 用户函数地址
        void (*sa_sigaction)(int, siginfo_t *, void *);  // SA_SIGINFO 模式
    };
    sigset_t sa_mask;                 // 处理期间阻塞的信号集
    int sa_flags;                     // 标志位
    void (*sa_restorer)(void);        // 恢复函数（已废弃，SA_RESTORER）
};

// 内核态信号处理动作结构
struct k_sigaction {
    struct sigaction sa;              // 用户可见的 sigaction
    unsigned int flags;               // SA_IMMUTABLE 等内核内部标志
};

// 每个进程的信号处理表
struct sighand_struct {
    struct k_sigaction action[_NSIG]; // 信号处理函数表（索引 0~64）
    spinlock_t siglock;               // 信号锁
    atomic_t count;                   // 引用计数（线程共享）
};

// sa_flags 标志位
#define SA_NOCLDSTOP  0x00000001  // 子进程停止时不产生 SIGCHLD
#define SA_NOCLDWAIT  0x00000002  // 子进程退出时不产生僵尸进程
#define SA_SIGINFO    0x00000004  // 使用三参数信号处理函数
#define SA_ONSTACK    0x08000000  // 在 sigaltstack 上执行处理函数
#define SA_RESTART    0x10000000  // 自动重启被信号中断的系统调用
#define SA_NODEFER    0x40000000  // 处理信号时不阻塞同种信号
#define SA_RESETHAND  0x80000000  // 处理一次后恢复默认行为
#define SA_RESTORER   0x04000000  // 指定 sa_restorer 函数
```

## 5. 流程图

```
用户态调用 sigaction(sig, act, oact)
    │
    ▼
rt_sigaction(sig, act, oact, sigsetsize)
    │
    ├─ 从用户空间拷贝 act（若非空）
    │
    └─ do_sigaction(sig, act, oact)
         │
         ├─ 检查 sig 有效性（1 ≤ sig < _NSIG）
         │
         ├─ 获取 action 表项：k = &sighand->action[sig-1]
         │
         ├─ 加锁 spin_lock_irq(&sighand->siglock)
         │
         ├─ 检查 SA_IMMUTABLE（SIGKILL/SIGSTOP 保护）→ -EINVAL
         │
         ├─ 若 oact 非空 → 将当前 *k 拷贝到 oact
         │
         ├─ 若 act 非空：
         │   ├─ *k = *act                    // 写入新动作
         │   ├─ 若 act->sa_handler == SIG_DFL
         │   │   └─ 清除 SA_SIGINFO 标志
         │   └─ 若 sig == SIGKILL/SIGSTOP
         │       └─ 强制设置 SA_IMMUTABLE
         │
         └─ 解锁 spin_unlock_irq(&sighand->siglock)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效参数 | sig 无效、sigsetsize 不匹配 |
| EFAULT | 内存错误 | act/oact 指针指向不可访问的内存 |
| ENOMEM | 内存不足 | 旧版 sigaction 无法分配内核缓冲 |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

void handler(int sig)
{
    printf("Caught signal %d (%s)\n", sig, strsignal(sig));
}

int main(void)
{
    struct sigaction sa;

    /* 设置信号处理函数 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;           // 自动重启被中断的系统调用
    sigemptyset(&sa.sa_mask);           // 处理期间不额外阻塞信号

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    /* 查询当前信号处理 */
    struct sigaction old;
    if (sigaction(SIGINT, NULL, &old) == -1) {
        perror("sigaction query");
        return 1;
    }

    printf("Current handler for SIGINT: %p\n", old.sa_handler);

    /* 等待信号 */
    printf("Press Ctrl+C to trigger signal...\n");
    pause();

    /* 恢复默认行为 */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGINT, &sa, NULL);

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`do_sigaction()` - 信号处理动作设置核心函数
- include/linux/signal.h - sigaction 相关结构定义