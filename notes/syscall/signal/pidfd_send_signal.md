# pidfd_send_signal 系统调用分析

## 1. 概述

`pidfd_send_signal()` 系统调用通过进程文件描述符（pidfd）发送信号，避免了传统 `kill()` 中 PID 重用导致的 TOCTOU（Time-of-Check-Time-of-Use）安全问题。pidfd 是对进程的一个稳定引用，在整个生命周期内不会因 PID 重用而指向其他进程。

**原型：**

```c
SYSCALL_DEFINE4(pidfd_send_signal, int, pidfd, int, sig,
                siginfo_t __user *, info, unsigned int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| pidfd | int | 进程文件描述符（通过 pidfd_open() 获取）或特殊值 |
| sig | int | 信号编号（0 用于检查权限） |
| info | siginfo_t* | 信号附带信息（可为 NULL） |
| flags | unsigned int | 标志位（当前保留，必须为 0） |

**特殊 pidfd 值：**

| 值 | 含义 |
|----|------|
| PIDFD_SELF_THREAD | 向当前线程自身发送信号（PIDTYPE_PID 范围） |
| PIDFD_SELF_THREAD_GROUP | 向当前进程的线程组发送信号（PIDTYPE_TGID 范围） |

## 2. 使用场景

- 安全信号发送：避免 PID 重用导致的误杀
- 线程级信号：通过 `PIDFD_THREAD` 标志可精确控制信号发送到单个线程还是整个线程组
- 跨命名空间：pidfd 绑定到特定 pid 命名空间，提供命名空间安全的信号发送
- 结合 pidfd_open()：先通过 `pidfd_open()` 获取稳定引用，再发送信号

## 3. 函数调用链

```
pidfd_send_signal(pidfd, sig, info, flags)   // kernel/signal.c:4063
  │
  ├─ flags 检查（必须为 0 或有效标志）
  │
  ├─ 判断 pidfd 类型：
  │   ├─ PIDFD_SELF_THREAD → current, PIDTYPE_PID
  │   ├─ PIDFD_SELF_THREAD_GROUP → current, PIDTYPE_TGID
  │   └─ 普通 pidfd → pidfd_to_pid() 获取 pid
  │
  └─ do_pidfd_send_signal(pid, sig, type, info, flags)  // kernel/signal.c:4008
       │
       ├─ flags 覆盖 type（若 flags 指定了 PIDFD_SIGNAL_THREAD 等）
       │
       ├─ 若 info 非空：
       │   ├─ copy_siginfo_from_user_any(&kinfo, info)  // 从用户空间拷贝
       │   ├─ 检查 sig 与 kinfo.si_signo 一致性
       │   └─ 权限检查：只有自己可以发送任意 si_code
       │
       ├─ 若 info 为 NULL：
       │   └─ prepare_kill_siginfo(sig, &kinfo, type)  // 构造默认 siginfo
       │
       ├─ 若 type == PIDTYPE_PGID：
       │   └─ kill_pgrp_info(sig, &kinfo, pid)
       │
       └─ kill_pid_info_type(sig, &kinfo, pid, type)
            └─ group_send_sig_info(sig, info, p, type)
                 └─ __send_signal(sig, info, p, type, false)
                      ├─ pending = &p->signal->shared_pending
                      ├─ sig_addset(&pending->signal, sig)
                      ├─ alloc_sigqueue() 入队
                      └─ complete_signal(sig, p, type)
```

## 4. 关键数据结构

```c
// 内核信号信息结构
typedef struct kernel_siginfo {
    __SIGINFO;
} kernel_siginfo_t;

// pidfd 相关标志
#define PIDFD_SIGNAL_THREAD         (1 << 0)  // 发送到单个线程
#define PIDFD_SIGNAL_THREAD_GROUP   (1 << 1)  // 发送到线程组
#define PIDFD_SIGNAL_PROCESS_GROUP  (1 << 2)  // 发送到进程组

// 允许的 pidfd_send_signal flags
#define PIDFD_SEND_SIGNAL_FLAGS (PIDFD_SIGNAL_THREAD | \
                                 PIDFD_SIGNAL_THREAD_GROUP | \
                                 PIDFD_SIGNAL_PROCESS_GROUP)
```

## 5. 流程图

```
用户态
  │
  ├─ pidfd = pidfd_open(target_pid, 0);  // 获取稳定 pidfd 引用
  │
  └─ pidfd_send_signal(pidfd, sig, NULL, 0);
       │
       ▼
内核态 pidfd_send_signal()
       │
       ├─ 检查 flags 合法性 ── 无效 → -EINVAL
       │
       ├─ 解析 pidfd：
       │   ├─ CLASS(fd, f)(pidfd)       // 获取 fd 文件结构
       │   ├─ pidfd_to_pid(fd_file(f))  // 从 pidfd 解析出 pid 结构
       │   └─ 检查 pid 命名空间访问权限
       │
       ├─ do_pidfd_send_signal()
       │   │
       │   ├─ 复制 siginfo（若有）
       │   │
       │   ├─ 权限检查（仅自己可发送任意 si_code）
       │   │
       │   └─ kill_pid_info_type()
       │       └─ group_send_sig_info()
       │           └─ __send_signal()
       │               ├─ 不可靠信号丢弃（legacy_queue）
       │               ├─ 信号位图置位
       │               ├─ sigqueue 分配与入队
       │               └─ complete_signal() → signal_wake_up()
       │
       └─ 返回 0 / 错误码
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EBADF | 无效 fd | pidfd 不是有效的文件描述符 |
| EINVAL | 无效参数 | sig 无效、flags 包含未知位、pidfd 命名空间不匹配 |
| EPERM | 权限不足 | 无权向目标发送信号，或试图以非法 si_code 发送 |
| ESRCH | 目标不存在 | pidfd 对应的进程已退出 |
| EFAULT | 用户内存错误 | info 指针指向非法地址 |

## 7. 使用示例

```c
#include <sys/syscall.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif

int main(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* 子进程 */
        printf("Child PID: %d\n", getpid());
        pause();
        printf("Child exiting\n");
        exit(0);
    }

    /* 父进程 */
    sleep(1);

    /* 通过 pidfd_open 获取稳定引用 */
    int pidfd = syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        perror("pidfd_open");
        return 1;
    }

    /* 通过 pidfd 发送信号 */
    if (syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, NULL, 0) < 0) {
        perror("pidfd_send_signal");
    }

    close(pidfd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`do_pidfd_send_signal()` - pidfd 信号发送核心实现
- kernel/pid.c:`pidfd_get_pid()` - 从 pidfd 获取 pid 结构
- include/uapi/linux/pidfd.h - pidfd 相关常量定义