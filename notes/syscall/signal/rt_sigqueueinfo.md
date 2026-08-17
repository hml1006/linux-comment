# rt_sigqueueinfo 系统调用分析

## 1. 概述

`rt_sigqueueinfo()` 系统调用用于向指定进程发送信号，并附带自定义的 `siginfo_t` 信息。与 `kill()` 不同，它允许发送方指定信号的完整信息（包括 si_code、si_value 等），这使得它可用于发送实时信号和 POSIX 定时器信号。

**原型：**

```c
SYSCALL_DEFINE3(rt_sigqueueinfo, pid_t, pid, int, sig,
                siginfo_t __user *, uinfo)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| pid | pid_t | 目标进程 ID |
| sig | int | 信号编号 |
| uinfo | siginfo_t* | 用户提供的信号信息（包含 si_code、si_value 等） |

## 2. 使用场景

- 发送实时信号（SIGRTMIN ~ SIGRTMAX）附带数据
- 自定义信号信息（si_code、si_value）
- 实现用户态信号通知机制（如 POSIX 定时器通知）
- 提供给 `sigwaitinfo()` 或 `signalfd` 消费

## 3. 函数调用链

```
rt_sigqueueinfo(pid, sig, uinfo)                  // kernel/signal.c:4209
  │
  ├─ __copy_siginfo_from_user(sig, &info, uinfo)  // 从用户空间拷贝 siginfo
  │
  └─ do_rt_sigqueueinfo(pid, sig, &info)          // kernel/signal.c:4190
       │
       ├─ 安全性检查：
       │   si_code >= 0 或 si_code == SI_TKILL → 只能是给自己发送
       │   (防止冒充内核发送信号)
       │
       └─ kill_proc_info(sig, info, pid)           // 发送到进程
            └─ group_send_sig_info(sig, info, p, PIDTYPE_TGID)
                 └─ __send_signal(sig, info, p, PIDTYPE_TGID, false)
```

## 4. 关键数据结构

```c
// 信号信息结构（用户态）
typedef struct {
    int si_signo;     // 信号编号
    int si_errno;     // 错误号
    int si_code;      // 信号来源码
    // 联合体，根据 si_code 决定有效字段
    union {
        // ... 多种信号特定字段
        struct {
            union sigval si_value;  // 实时信号附加值
        } _rt;
    };
} siginfo_t;

// si_code 约束：
// - si_code < 0  → 由内核生成（如 SIGSEGV、SIGFPE）
// - si_code == 0 → SI_USER（由 kill() 发送）
// - si_code > 0  → 由用户态通过 rt_sigqueueinfo 发送
// - SI_TKILL (=-6) 由 tkill/tgkill 发送

// 关键限制：
// 非 root 用户只能发送 si_code = SI_USER (0) 或 SI_TKILL (-6)
// 且 si_code >= 0 的信号只能发送给自己
```

## 5. 流程图

```
用户态: rt_sigqueueinfo(pid, sig, &uinfo)
    │
    ▼
SYSCALL_DEFINE3(rt_sigqueueinfo)
    │
    ├─ __copy_siginfo_from_user(sig, &info, uinfo)
    │   // 从用户空间拷贝并验证 si_signo 一致性
    │
    └─ do_rt_sigqueueinfo(pid, sig, &info)
         │
         ├─ 权限检查：
         │   ├─ si_code >= 0 或 si_code == SI_TKILL
         │   └─ 且 pid != current → -EPERM
         │   (只能自己给自己发送任意信号)
         │
         └─ kill_proc_info(sig, info, pid)
              │
              └─ group_send_sig_info()
                   └─ __send_signal()
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效参数 | sig 无效 |
| EPERM | 权限不足 | 试图冒充内核发送信号（si_code < 0 且目标不是自己） |
| ESRCH | 目标不存在 | 指定的 pid 对应的进程不存在 |
| EFAULT | 内存错误 | uinfo 指针指向不可访问的内存 |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

#ifndef SYS_rt_sigqueueinfo
#define SYS_rt_sigqueueinfo 129
#endif

int main(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* 子进程：使用 sigwaitinfo 等待信号 */
        sigset_t set;
        siginfo_t info;

        sigemptyset(&set);
        sigaddset(&set, SIGRTMIN);
        sigprocmask(SIG_BLOCK, &set, NULL);

        printf("Child waiting for SIGRTMIN...\n");
        sigwaitinfo(&set, &info);
        printf("Child got signal %d, si_value.sival_int = %d\n",
               info.si_signo, info.si_value.sival_int);
        exit(0);
    }

    /* 父进程：发送带数据的实时信号 */
    sleep(1);

    siginfo_t uinfo;
    uinfo.si_signo = SIGRTMIN;
    uinfo.si_code = SI_QUEUE;          // 用户态发送
    uinfo.si_value.sival_int = 42;     // 附带数据

    if (syscall(SYS_rt_sigqueueinfo, pid, SIGRTMIN, &uinfo) < 0) {
        perror("rt_sigqueueinfo");
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`do_rt_sigqueueinfo()` - 核心实现
- include/uapi/asm-generic/siginfo.h - siginfo_t 结构定义