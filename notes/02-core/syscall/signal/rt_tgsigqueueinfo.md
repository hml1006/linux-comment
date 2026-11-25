# rt_tgsigqueueinfo 系统调用分析

> 所属分类：信号处理 | [ARM64 系统调用表](../arm64-syscall-table.md#11-信号处理)

## 1. 概述

`rt_tgsigqueueinfo` 向线程组中的**指定线程**投递实时信号，并携带用户自定义的 `siginfo_t` 数据（`si_value`）。它是 `rt_sigqueueinfo` 的线程定向版本：

- `rt_sigqueueinfo(pid, sig, info)` → 进程定向（`kill` + 信息）；
- `rt_tgsigqueueinfo(tgid, tid, sig, info)` → 线程定向（`tgkill` + 信息）。

典型用途是 POSIX 定时器的 `SIGEV_THREAD_ID` 通知，以及把 `si_value` 精确送到某个线程。内核**不允许用户伪造内核或 `kill`/`tgkill` 产生的信号**：`si_code >= 0` 或 `SI_TKILL` 仅当发送者就是目标线程本身时才被接受。

ARM64 系统调用号：`__NR_rt_tgsigqueueinfo = 240`。

## 2. 函数原型与参数

用户空间原型（glibc 无直接包装，通常用 `syscall()`）：

```c
#include <signal.h>

int rt_tgsigqueueinfo(pid_t tgid, pid_t tid, int sig, siginfo_t *info);
```

内核入口（`kernel/signal.c:4261`）：

```c
SYSCALL_DEFINE4(rt_tgsigqueueinfo, pid_t, tgid, pid_t, pid, int, sig,
                siginfo_t __user *, uinfo)
```

| 参数 | 说明 |
|------|------|
| `tgid` | 目标线程组 ID（进程 ID） |
| `pid` | 目标线程 ID，必须属于 `tgid` 且 `> 0` |
| `sig` | 信号编号；`0` 只做存在性与权限探测，不投递 |
| `uinfo` | 用户 `siginfo_t`；`si_signo` 被内核用 `sig` 覆盖，`si_code` 受严格限制 |

`tgid` 或 `pid` 任一 `<= 0` 返回 `-EINVAL`。

## 3. 内核实现路径

```
__arm64_sys_rt_tgsigqueueinfo(tgid, pid, sig, uinfo)
  └─ SYSCALL_DEFINE4(rt_tgsigqueueinfo)             // kernel/signal.c:4261
       ├─ __copy_siginfo_from_user(sig, &info, uinfo)  // 3543
       │    ├─ copy_from_user(sizeof(struct kernel_siginfo))
       │    ├─ info.si_signo = sig
       │    └─ post_copy_siginfo_from_user()        // 3520
       │         └─ 未知 si_code 布局时扩展区必须全 0，否则 -E2BIG
       └─ do_rt_tgsigqueueinfo(tgid, pid, sig, &info)  // 4245
            ├─ pid <= 0 || tgid <= 0 → -EINVAL
            ├─ (si_code >= 0 || si_code == SI_TKILL)
            │     && task_pid_vnr(current) != pid → -EPERM
            └─ do_send_specific(tgid, pid, sig, info)  // 4129
                 ├─ rcu_read_lock()
                 ├─ p = find_task_by_vpid(pid)
                 ├─ p && task_tgid_vnr(p) == tgid
                 ├─ check_kill_permission(sig, info, p)  // 799
                 │    ├─ !valid_signal(sig) → -EINVAL
                 │    ├─ 权限/安全模块检查 → -EPERM
                 │    └─ sig == 0 时只探测
                 ├─ do_send_sig_info(..., PIDTYPE_PID)   // 1260
                 │    ├─ lock_task_sighand()
                 │    └─ send_signal_locked()            // 1181
                 └─ ESRCH 竞态（目标刚好退出）→ 视为成功 0
```

对比 `do_rt_sigqueueinfo()`（4202）：它通过 `kill_proc_info()`（1483）→ `kill_pid_info()`（1478）发送，按**进程/线程组**展开目标，而非精确到某个线程。

安全规则：

1. `si_code >= 0` 或 `SI_TKILL` 必须"自己发给自己"（`task_pid_vnr(current) == pid`），否则 `-EPERM`。
2. `tgid` 必须与目标实际线程组 ID 匹配，否则 `-ESRCH`，不会误投到同号的其他进程。
3. 未知 `si_code` 布局要求扩展字节全零，防止内核回传未初始化数据。

## 4. 核心数据结构

### 4.1 siginfo_t / kernel_siginfo_t

```c
// include/uapi/asm-generic/siginfo.h（用户 ABI）
typedef struct siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    union {
        struct { int _pad[__SI_PAD_SIZE]; } _pad;
        struct { pid_t si_pid; uid_t si_uid; sigval_t si_sigval; } _rt;
    } _sifields;
} siginfo_t;
```

```c
// include/linux/signal_types.h:12
typedef struct kernel_siginfo {
    __SIGINFO;      /* 与用户 ABI 布局兼容的内核副本 */
} kernel_siginfo_t;
```

### 4.2 struct sigqueue（`include/linux/signal_types.h:22`）

实时信号携带的信息会挂到目标线程的待处理队列：

```c
struct sigqueue {
    struct list_head list;
    int flags;
    kernel_siginfo_t info;   /* 保存用户提供的 si_value 等 */
    struct ucounts *ucounts;
};
```

### 4.3 sigpending

`sigpending` 同时维护普通信号位图与 `sigqueue` 链表；接收者用 `sigwaitinfo()` 或 `SA_SIGINFO` 处理函数即可读到原始 `si_value`。

## 5. 关键流程

```
构造 siginfo_t（si_code = SI_QUEUE，si_value = 42）
        │
        ├─ copy_from_user + 覆盖 si_signo + 校验扩展区
        ├─ tgid/pid > 0 ?
        ├─ si_code 合法性 / 是否自发送 ?
        ├─ find_task_by_vpid(pid)
        │    └─ 不存在或 task_tgid_vnr(p) != tgid → -ESRCH
        ├─ check_kill_permission()
        │    ├─ 非法信号号 → -EINVAL
        │    ├─ 权限不足 → -EPERM
        │    └─ sig == 0 → 返回 0（仅探测）
        └─ do_send_sig_info() → send_signal_locked()
             ├─ 实时信号：分配 sigqueue 挂入 pending
             ├─ 普通信号：置 pending 位图
             └─ 唤醒目标线程
```

## 6. 返回值与错误码

成功返回 0；`sig == 0` 且目标存在、权限足够时同样返回 0，但不投递信号。

| 错误码 | 触发条件 |
|--------|----------|
| `-EINVAL` | `tgid <= 0` 或 `pid <= 0`；`sig` 非法（`check_kill_permission()`） |
| `-EPERM` | 伪造 `si_code >= 0` 或 `SI_TKILL` 且非自发送；无权限向目标发信号 |
| `-ESRCH` | 目标线程不存在，或 `pid` 不属于 `tgid` |
| `-EFAULT` | 读取用户 `siginfo_t` 失败 |
| `-E2BIG` | 未知 `si_code` 布局时扩展字节非零 |
| `-EAGAIN` | 目标待处理实时信号达到 `RLIMIT_SIGPENDING` 上限 |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/syscall.h>

static volatile sig_atomic_t got;

static void handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    got = si->si_value.sival_int;
}

static void *worker(void *arg)
{
    sigset_t set;
    (void)arg;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    while (!got)
        pause();
    return NULL;
}

int main(void)
{
    struct sigaction sa = {0};
    pthread_t th;
    siginfo_t si = {0};

    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN, &sa, NULL);

    pthread_create(&th, NULL, worker, NULL);
    usleep(100000);
    si.si_code = SI_QUEUE;
    si.si_value.sival_int = 42;

    /* 实际应由 worker 用 gettid() 上报 tid；此处 tid 获取从略 */
    pid_t tgid = getpid();
    pid_t tid  = tgid;

    if (syscall(__NR_rt_tgsigqueueinfo, tgid, tid, SIGRTMIN, &si) == -1)
        fprintf(stderr, "rt_tgsigqueueinfo: %s\n", strerror(errno));

    pthread_join(th, NULL);
    return 0;
}
```

示例中 `tid` 用 `tgid` 代替（即发给主线程）；实际使用时应由工作线程通过 `gettid()` 上报真实 TID。若把 `si_code` 设为 `SI_USER`（0），内核会因"伪造信号"返回 `-EPERM`，除非发送者就是 `tid`。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `kernel/signal.c` | `SYSCALL_DEFINE4(rt_tgsigqueueinfo)`（4261）、`do_rt_tgsigqueueinfo()`（4245）、`do_send_specific()`（4129）、`check_kill_permission()`（799）、`do_send_sig_info()`（1260）、`send_signal_locked()`（1181）；`__copy_siginfo_from_user()`（3543）、`post_copy_siginfo_from_user()`（3520） |
| `include/linux/signal_types.h` | `kernel_siginfo_t`、`struct sigqueue` |
| `arch/arm64/include/generated/uapi/asm/unistd_64.h` | `__NR_rt_tgsigqueueinfo = 240` |
