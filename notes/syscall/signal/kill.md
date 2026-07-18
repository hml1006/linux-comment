# kill 系统调用与信号生命周期分析

## 1. 概述

`kill()` 系统调用用于向指定进程或进程组发送信号。它是 Linux 信号发送机制中最基础的接口。

**原型：**

```c
// kernel/signal.c:3947
SYSCALL_DEFINE2(kill, pid_t, pid, int, sig)
```

**参数：**

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

## 2. 信号生命周期总览

信号从发送到执行完成经历完整的生命周期，可分为五个阶段：

```
┌──────────────────────────────────────────────────────────────────────────┐
│                    信号完整生命周期                                         │
│                                                                          │
│  ┌──────────┐    ┌──────────────┐    ┌──────────┐    ┌───────────────┐   │
│  │ 信号生成  │───►│  信号挂起     │───►│ 信号递送  │───►│  信号处理      │   │
│  │ (Generation)│   │ (Pending)    │   │ (Delivery)│   │ (Disposition) │   │
│  └──────────┘    └──────────────┘    └──────────┘    └───────────────┘   │
│       │               │                   │                │             │
│       │               │                   │          ┌─────┴──────┐      │
│       ▼               ▼                   ▼          ▼            ▼      │
│  kill() 系统调用  sigpending 队列    TIF_SIGPENDING  用户 handler  默认动作 │
│  内核内部发送     blocked 掩码       get_signal()    sigreturn     exit   │
│  硬件异常        信号掩码检查        arch_do_signal  恢复上下文     core   │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

## 3. 第一阶段：信号生成

### 3.1 kill() 系统调用入口

```c
// kernel/signal.c:3947
SYSCALL_DEFINE2(kill, pid_t, pid, int, sig)
{
    struct kernel_siginfo info;

    prepare_kill_siginfo(sig, &info, PIDTYPE_TGID);

    return kill_something_info(sig, &info, pid);
}
```

`prepare_kill_siginfo` 构造内核 siginfo 结构：

```c
// kernel/signal.c:3863
static int prepare_kill_siginfo(int sig, struct kernel_siginfo *info,
                                enum pid_type type)
{
    clear_siginfo(info);
    info->si_signo = sig;
    info->si_errno = 0;
    info->si_code = SI_USER;       // 标记为用户空间发送
    info->si_pid = task_tgid_nr_ns(current, task_active_pid_ns(current));
    info->si_uid = from_kuid_munged(current_user_ns(), current_uid());
    return 0;
}
```

### 3.2 kill_something_info 路由

```c
// kernel/signal.c:1572
static int kill_something_info(int sig, struct kernel_siginfo *info, pid_t pid)
{
    int ret;

    // pid > 0: 发送给单个进程
    if (pid > 0)
        return kill_proc_info(sig, info, pid);

    if (pid == INT_MIN)
        return -ESRCH;

    read_lock(&tasklist_lock);
    if (pid != -1) {
        // pid < -1: 发送给进程组
        // pid == 0: 发送给当前进程组
        ret = __kill_pgrp_info(sig, info,
                pid ? find_vpid(-pid) : task_pgrp(current));
    } else {
        // pid == -1: 发送给所有进程（除 init 和当前进程外）
        int retval = 0, count = 0;
        struct task_struct *p;

        for_each_process(p) {
            if (task_pid_vnr(p) > 1 && !same_thread_group(p, current)) {
                int err = group_send_sig_info(sig, info, p, PIDTYPE_MAX);
                ++count;
                if (err != -EPERM)
                    retval = err;
            }
        }
        ret = count ? retval : -ESRCH;
    }
    read_unlock(&tasklist_lock);

    return ret;
}
```

### 3.3 信号发送路径：kill_proc_info → group_send_sig_info

```c
// kernel/signal.c:1620
int kill_proc_info(int sig, struct kernel_siginfo *info, pid_t pid)
{
    int error;
    struct task_struct *p;

    rcu_read_lock();
    p = find_task_by_vpid(pid);
    if (p)
        error = group_send_sig_info(sig, info, p, PIDTYPE_TGID);
    else
        error = -ESRCH;
    rcu_read_unlock();

    return error;
}
```

`group_send_sig_info` 是信号发送的核心入口：

```c
// kernel/signal.c:1620
int group_send_sig_info(int sig, struct kernel_siginfo *info,
                        struct task_struct *p, enum pid_type type)
{
    int ret;

    // 检查信号有效性
    if (!valid_signal(sig))
        return -EINVAL;

    // 权限检查
    ret = check_kill_permission(sig, info, p);
    if (ret)
        return ret;

    // 发送信号到共享信号队列
    return do_send_sig_info(sig, info, p, type);
}
```

### 3.4 权限检查

```c
// kernel/signal.c:1177
static int check_kill_permission(int sig, struct kernel_siginfo *info,
                                 struct task_struct *t)
{
    struct pid *sid;
    int error;

    // 发送 sig=0 仅用于检查权限（不发送信号）
    if (!valid_signal(sig))
        return -EINVAL;

    if (info != SEND_SIG_NOINFO && (is_si_special(info) || SI_FROMKERNEL(info)))
        return 0;

    // 权能检查：CAP_KILL 或相同的 uid
    error = audit_signal_info(sig, t);
    if (error)
        return error;

    return capable(CAP_KILL) ||
           (uid_eq(task_uid(t), current_uid()) ||
            uid_eq(task_uid(t), current_euid()))
           ? 0 : -EPERM;
}
```

### 3.5 信号的其他生成源

除了 kill() 系统调用，信号还可以通过以下方式生成：

| 生成源 | 例子 | siginfo.si_code |
|--------|------|-----------------|
| 用户态 kill() | `kill(pid, sig)` | SI_USER |
| 用户态 tkill() | `tkill(pid, sig)` | SI_TKILL |
| 用户态 sigqueue() | `sigqueue(pid, sig, val)` | SI_QUEUE |
| 内核内部 | `send_sig_info()` | SI_KERNEL |
| 硬件异常 | 段错误、除零 | SI_KERNEL 或特定码 |
| 定时器 | POSIX 定时器到期 | SI_TIMER |
| 子进程状态 | SIGCHLD | CLD_* |
| 消息队列 | mq_notify | SI_MESGQ |

## 4. 第二阶段：信号挂起（Pending）

### 4.1 __send_signal_locked 核心实现

这是信号入队的核心函数，决定信号是否被忽略、队列还是丢弃：

```c
// kernel/signal.c:1042
static int __send_signal_locked(int sig, struct kernel_siginfo *info,
                                struct task_struct *t, enum pid_type type,
                                bool force)
{
    struct sigpending *pending;
    struct sigqueue *q;
    int override_rlimit;
    int ret = 0, result;

    result = TRACE_SIGNAL_IGNORED;
    // prepare_signal 检查信号是否被忽略
    if (!prepare_signal(sig, t, force))
        goto ret;

    // 选择信号队列：线程私有或进程共享
    pending = (type != PIDTYPE_PID) ? &t->signal->shared_pending : &t->pending;

    // 不可靠信号检查：非实时信号已在队列中则丢弃
    result = TRACE_SIGNAL_ALREADY_PENDING;
    if (legacy_queue(pending, sig))
        goto ret;

    result = TRACE_SIGNAL_DELIVERED;
    // SIGKILL 和内核线程不需要 sigqueue 分配
    if ((sig == SIGKILL) || (t->flags & PF_KTHREAD))
        goto out_set;

    // 分配 sigqueue 条目
    if (sig < SIGRTMIN)
        override_rlimit = (is_si_special(info) || info->si_code >= 0);
    else
        override_rlimit = 0;

    q = sigqueue_alloc(sig, t, GFP_ATOMIC, override_rlimit);

    if (q) {
        list_add_tail(&q->list, &pending->list);
        // 填充信号信息
        switch ((unsigned long) info) {
        case (unsigned long) SEND_SIG_NOINFO:
            // 无信息时填充默认值
            clear_siginfo(&q->info);
            q->info.si_signo = sig;
            q->info.si_errno = 0;
            q->info.si_code = SI_USER;
            q->info.si_pid = task_tgid_nr_ns(current, task_active_pid_ns(t));
            q->info.si_uid = from_kuid_munged(task_cred_xxx(t, user_ns), current_uid());
            break;
        case (unsigned long) SEND_SIG_PRIV:
            // 内核私有信号
            clear_siginfo(&q->info);
            q->info.si_signo = sig;
            q->info.si_errno = 0;
            q->info.si_code = SI_KERNEL;
            q->info.si_pid = 0;
            q->info.si_uid = 0;
            break;
        default:
            copy_siginfo(&q->info, info);
            break;
        }
    } else if (!is_si_special(info) &&
               sig >= SIGRTMIN && info->si_code != SI_USER) {
        // 实时信号队列溢出
        result = TRACE_SIGNAL_OVERFLOW_FAIL;
        ret = -EAGAIN;
        goto ret;
    } else {
        // 信息丢失但仍发送信号
        result = TRACE_SIGNAL_LOSE_INFO;
    }

out_set:
    signalfd_notify(t, sig);
    sigaddset(&pending->signal, sig);  // 位图置位

    // 处理多进程信号（如进程组发送）
    if (type > PIDTYPE_TGID) {
        struct multiprocess_signals *delayed;
        hlist_for_each_entry(delayed, &t->signal->multiprocess, node) {
            sigset_t *signal = &delayed->signal;
            if (sig == SIGCONT)
                sigdelsetmask(signal, SIG_KERNEL_STOP_MASK);
            else if (sig_kernel_stop(sig))
                sigdelset(signal, SIGCONT);
            sigaddset(signal, sig);
        }
    }

    // 唤醒目标进程
    complete_signal(sig, t, type);
ret:
    trace_signal_generate(sig, info, t, type != PIDTYPE_PID, result);
    return ret;
}
```

### 4.2 prepare_signal：预处理检查

在真正入队前，检查信号是否应该被忽略，并处理 STOP/CONT 冲突：

```c
// kernel/signal.c:871
static bool prepare_signal(int sig, struct task_struct *p, bool force)
{
    struct signal_struct *signal = p->signal;
    struct task_struct *t;
    sigset_t flush;

    // 如果进程正在退出，只接受 SIGKILL
    if (signal->flags & SIGNAL_GROUP_EXIT) {
        if (signal->core_state)
            return sig == SIGKILL;
        return false;
    } else if (sig_kernel_stop(sig)) {
        // 停止信号：从所有队列中移除 SIGCONT
        siginitset(&flush, sigmask(SIGCONT));
        flush_sigqueue_mask(p, &flush, &signal->shared_pending);
        for_each_thread(p, t)
            flush_sigqueue_mask(p, &flush, &t->pending);
    } else if (sig == SIGCONT) {
        unsigned int why;
        // SIGCONT：移除所有停止信号，唤醒所有线程
        siginitset(&flush, SIG_KERNEL_STOP_MASK);
        flush_sigqueue_mask(p, &flush, &signal->shared_pending);
        for_each_thread(p, t) {
            flush_sigqueue_mask(p, &flush, &t->pending);
            task_clear_jobctl_pending(t, JOBCTL_STOP_PENDING);
            if (likely(!(t->ptrace & PT_SEIZED))) {
                t->jobctl &= ~JOBCTL_STOPPED;
                wake_up_state(t, __TASK_STOPPED);
            } else
                ptrace_trap_notify(t);
        }
        // 通知父进程继续事件
        why = 0;
        if (signal->flags & SIGNAL_STOP_STOPPED)
            why |= SIGNAL_CLD_CONTINUED;
        else if (signal->group_stop_count)
            why |= SIGNAL_CLD_STOPPED;
        if (why) {
            signal_set_stop_flags(signal, why | SIGNAL_STOP_CONTINUED);
            signal->group_stop_count = 0;
            signal->group_exit_code = 0;
        }
    }

    // 最终检查：信号是否被忽略
    return !sig_ignored(p, sig, force);
}
```

### 4.3 信号队列选择

每个进程有两个信号队列：

```c
// 线程私有队列（per-thread pending）
struct sigpending pending;          // task_struct->pending

// 进程共享队列（shared pending）
struct sigpending shared_pending;   // signal_struct->shared_pending
```

**队列选择规则：**

```c
// type == PIDTYPE_PID  → 线程私有队列
// type != PIDTYPE_PID  → 进程共享队列
pending = (type != PIDTYPE_PID) ? &t->signal->shared_pending : &t->pending;
```

- `kill()` 使用 `PIDTYPE_TGID` → 共享队列
- `tkill()` 使用 `PIDTYPE_PID` → 线程私有队列
- `sigqueue()` 可指定 PIDTYPE

### 4.4 不可靠信号 vs 实时信号

```c
// kernel/signal.c:1037
static inline bool legacy_queue(struct sigpending *signals, int sig)
{
    return (sig < SIGRTMIN) && sigismember(&signals->signal, sig);
}
```

| 特性 | 不可靠信号 (1-31) | 实时信号 (SIGRTMIN-SIGRTMAX) |
|------|-------------------|-------------------------------|
| 队列行为 | 同一种信号最多一个在队列中 | 多个相同信号可以排队 |
| 信息传递 | 重复信号丢失信息 | 每个信号携带完整 siginfo |
| legacy_queue 检查 | 通过则丢弃重复 | 不检查，总是入队 |
| 排序 | 无 | FIFO 顺序 |

### 4.5 关键数据结构

```c
// 信号发送信息结构
struct kernel_siginfo {
    int si_signo;          // 信号编号
    int si_errno;          // 错误号（通常为 0）
    int si_code;           // 信号来源码（SI_USER, SI_KILL, SI_TKILL 等）
    union {
        struct { ... } _kill;    // kill() 发送
        struct { ... } _timer;   // POSIX 定时器
        struct { ... } _rt;      // 实时信号
        struct { ... } _sigchld; // 子进程状态变更
        struct { ... } _sigfault; // 内存访问错误
        struct { ... } _sigpoll; // IO 事件
        struct { ... } _sigsys;  // 系统调用错误
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

## 5. 第三阶段：信号递送决策（complete_signal）

### 5.1 complete_signal 实现

信号入队后，`complete_signal` 决定如何递送信号——选择目标线程并唤醒：

```c
// kernel/signal.c:963
static void complete_signal(int sig, struct task_struct *p, enum pid_type type)
{
    struct signal_struct *signal = p->signal;
    struct task_struct *t;

    // 1. 选择一个能接收信号的线程
    if (wants_signal(sig, p))
        t = p;
    else if ((type == PIDTYPE_PID) || thread_group_empty(p))
        return;  // 单线程不阻塞，且已经在运行
    else {
        // 多线程：轮询选择不阻塞此信号的线程
        t = signal->curr_target;
        while (!wants_signal(sig, t)) {
            t = next_thread(t);
            if (t == signal->curr_target)
                return;  // 所有线程都阻塞此信号
        }
        signal->curr_target = t;
    }

    // 2. 如果是致命信号，立即启动组退出
    if (sig_fatal(p, sig) &&
        (signal->core_state || !(signal->flags & SIGNAL_GROUP_EXIT)) &&
        !sigismember(&t->real_blocked, sig) &&
        (sig == SIGKILL || !p->ptrace)) {
        if (!sig_kernel_coredump(sig)) {
            // 非 coredump 致命信号：立即杀死所有线程
            signal->flags = SIGNAL_GROUP_EXIT;
            signal->group_exit_code = sig;
            signal->group_stop_count = 0;
            __for_each_thread(signal, t) {
                task_clear_jobctl_pending(t, JOBCTL_PENDING_MASK);
                sigaddset(&t->pending.signal, SIGKILL);
                signal_wake_up(t, 1);
            }
            return;
        }
    }

    // 3. 唤醒选中的线程
    signal_wake_up(t, sig == SIGKILL);
}
```

### 5.2 wants_signal 判断

```c
// kernel/signal.c:946
static inline bool wants_signal(int sig, struct task_struct *p)
{
    // 信号被阻塞
    if (sigismember(&p->blocked, sig))
        return false;

    // 进程正在退出
    if (p->flags & PF_EXITING)
        return false;

    // SIGKILL 总是可以递送
    if (sig == SIGKILL)
        return true;

    // 进程已停止或追踪中，不能接收信号
    if (task_is_stopped_or_traced(p))
        return false;

    // 正在运行或没有挂起信号
    return task_curr(p) || !task_sigpending(p);
}
```

### 5.3 signal_wake_up 唤醒目标线程

```c
// kernel/signal.c:721
void signal_wake_up_state(struct task_struct *t, unsigned int state)
{
    lockdep_assert_held(&t->sighand->siglock);

    // 设置 TIF_SIGPENDING 标志，告诉进程"有信号需要处理"
    set_tsk_thread_flag(t, TIF_SIGPENDING);

    // 唤醒进程
    if (!wake_up_state(t, state | TASK_INTERRUPTIBLE))
        kick_process(t);  // 如果进程在另一个 CPU 运行，发送 IPI
}
```

**TIF_SIGPENDING 的作用：**
- 设置此标志后，进程在返回用户空间或进入/退出系统调用时，会检查并处理信号
- `wake_up_state` 将进程从 `TASK_INTERRUPTIBLE`/`TASK_STOPPED` 等状态唤醒
- `kick_process` 发送 IPI 中断，使运行中的进程在中断返回时检查信号

## 6. 第四阶段：信号递送时机

### 6.1 信号递送时机总览

信号不在发送时立即递送，而是"延迟"到以下时机：

```
┌─────────────────────────────────────────────────────────────┐
│                    信号递送时机                                │
│                                                             │
│  1. 系统调用返回用户空间                                       │
│     ┌──────────────────────────────────────────────┐        │
│     │  el0_svc  →  invoke_syscall →  syscall_exit  │        │
│     │  → exit_to_user_mode_loop()                  │        │
│     │  → arch_do_signal_or_restart()               │        │
│     └──────────────────────────────────────────────┘        │
│                                                             │
│  2. 中断/异常返回用户空间                                     │
│     ┌──────────────────────────────────────────────┐        │
│     │  irq_exit → irqentry_exit_to_user_mode()     │        │
│     │  → exit_to_user_mode_loop()                  │        │
│     │  → arch_do_signal_or_restart()               │        │
│     └──────────────────────────────────────────────┘        │
│                                                             │
│  3. 被信号唤醒的进程调度后                                    │
│     ┌──────────────────────────────────────────────┐        │
│     │  wake_up_state → schedule() → ret_to_user    │        │
│     │  → exit_to_user_mode_loop()                  │        │
│     └──────────────────────────────────────────────┘        │
│                                                             │
│  4. 正在运行的进程收到 IPI                                   │
│     ┌──────────────────────────────────────────────┐        │
│     │  kick_process() (IPI) → 中断处理              │        │
│     │  → 中断返回时进入 exit_to_user_mode_loop()    │        │
│     └──────────────────────────────────────────────┘        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 exit_to_user_mode_loop：信号递送入口

```c
// kernel/entry/common.c:41
static __always_inline unsigned long __exit_to_user_mode_loop(
    struct pt_regs *regs, unsigned long ti_work)
{
    while (ti_work & EXIT_TO_USER_MODE_WORK_LOOP) {
        local_irq_enable_exit_to_user(ti_work);

        if (ti_work & (_TIF_NEED_RESCHED | _TIF_NEED_RESCHED_LAZY))
            schedule();

        if (ti_work & _TIF_UPROBE)
            uprobe_notify_resume(regs);

        if (ti_work & _TIF_PATCH_PENDING)
            klp_update_patch_state(current);

        // ★ 信号处理入口
        if (ti_work & (_TIF_SIGPENDING | _TIF_NOTIFY_SIGNAL))
            arch_do_signal_or_restart(regs);

        if (ti_work & _TIF_NOTIFY_RESUME)
            resume_user_mode_work(regs);

        arch_exit_to_user_mode_work(regs, ti_work);

        local_irq_disable_exit_to_user();
        tick_nohz_user_enter_prepare();
        ti_work = read_thread_flags();
    }
    return ti_work;
}
```

### 6.3 ARM64 架构的信号处理入口

```c
// arch/arm64/kernel/signal.c:1594
void arch_do_signal_or_restart(struct pt_regs *regs)
{
    unsigned long continue_addr = 0, restart_addr = 0;
    int retval = 0;
    struct ksignal ksig;
    bool syscall = in_syscall(regs);

    // 1. 处理系统调用重启
    if (syscall) {
        continue_addr = regs->pc;
        restart_addr = continue_addr - (compat_thumb_mode(regs) ? 2 : 4);
        retval = regs->regs[0];
        forget_syscall(regs);

        // 根据返回值决定是否需要重启系统调用
        switch (retval) {
        case -ERESTARTNOHAND:
        case -ERESTARTSYS:
        case -ERESTARTNOINTR:
        case -ERESTART_RESTARTBLOCK:
            regs->regs[0] = regs->orig_x0;
            regs->pc = restart_addr;
            break;
        }
    }

    // 2. 获取需要递送的信号
    if (get_signal(&ksig)) {
        // 有信号需要处理 → 调整系统调用重启
        if (regs->pc == restart_addr &&
            (retval == -ERESTARTNOHAND ||
             retval == -ERESTART_RESTARTBLOCK ||
             (retval == -ERESTARTSYS &&
              !(ksig.ka.sa.sa_flags & SA_RESTART)))) {
            syscall_set_return_value(current, regs, -EINTR, 0);
            regs->pc = continue_addr;
        }

        // 3. 处理信号（设置信号帧或执行默认动作）
        handle_signal(&ksig, regs);
        return;
    }

    // 4. 无信号处理：处理系统调用重启
    if (syscall && regs->pc == restart_addr) {
        // 重启系统调用，除非 SA_RESTART 被设置
        if (retval == -ERESTARTNOHAND ||
            retval == -ERESTART_RESTARTBLOCK ||
            (retval == -ERESTARTSYS &&
             !(ksig.ka.sa.sa_flags & SA_RESTART))) {
            syscall_set_return_value(current, regs, -EINTR, 0);
            regs->pc = continue_addr;
        }
    }
}
```

## 7. 第五阶段：信号处理（get_signal 核心）

### 7.1 get_signal 完整实现

`get_signal` 是信号处理的核心函数，它从队列中取出信号并决定如何处理：

```c
// kernel/signal.c:2799
bool get_signal(struct ksignal *ksig)
{
    struct sighand_struct *sighand = current->sighand;
    struct signal_struct *signal = current->signal;
    int signr;

    clear_notify_signal();
    if (unlikely(task_work_pending(current)))
        task_work_run();

    // 没有挂起信号，直接返回 false
    if (!task_sigpending(current))
        return false;

    try_to_freeze();

relock:
    spin_lock_irq(&sighand->siglock);

    // 处理 SIGCHLD 通知（子进程状态变化）
    if (unlikely(signal->flags & SIGNAL_CLD_MASK)) {
        int why;
        if (signal->flags & SIGNAL_CLD_CONTINUED)
            why = CLD_CONTINUED;
        else
            why = CLD_STOPPED;
        signal->flags &= ~SIGNAL_CLD_MASK;
        spin_unlock_irq(&sighand->siglock);
        do_notify_parent_cldstop(current, false, why);
        goto relock;
    }

    for (;;) {
        // 进程组退出标志
        if ((signal->flags & SIGNAL_GROUP_EXIT) || signal->group_exec_task) {
            signr = SIGKILL;
            sigdelset(&current->pending.signal, SIGKILL);
            trace_signal_deliver(SIGKILL, SEND_SIG_NOINFO,
                                 &sighand->action[SIGKILL-1]);
            recalc_sigpending();
            goto fatal;  // 直接跳到 fatal 处理
        }

        // 作业控制停止
        if (unlikely(current->jobctl & JOBCTL_STOP_PENDING) &&
            do_signal_stop(0))
            goto relock;

        // ptrace trap 处理
        if (unlikely(current->jobctl &
                     (JOBCTL_TRAP_MASK | JOBCTL_TRAP_FREEZE))) {
            if (current->jobctl & JOBCTL_TRAP_MASK) {
                do_jobctl_trap();
                spin_unlock_irq(&sighand->siglock);
            } else if (current->jobctl & JOBCTL_TRAP_FREEZE)
                do_freezer_trap();
            goto relock;
        }

        // 冻结状态处理
        if (unlikely(cgroup_task_frozen(current))) {
            spin_unlock_irq(&sighand->siglock);
            cgroup_leave_frozen(false);
            goto relock;
        }

        // ★ 出队信号
        // 先出队同步信号（硬件异常信号优先）
        type = PIDTYPE_PID;
        signr = dequeue_synchronous_signal(&ksig->info);
        if (!signr)
            signr = dequeue_signal(&current->blocked, &ksig->info, &type);

        if (!signr)
            break;  // 没有信号了

        // ★ ptrace 拦截
        if (unlikely(current->ptrace) && (signr != SIGKILL) &&
            !(sighand->action[signr -1].sa.sa_flags & SA_IMMUTABLE)) {
            signr = ptrace_signal(signr, &ksig->info, type);
            if (!signr)
                continue;  // 追踪器取消信号
        }

        ka = &sighand->action[signr-1];
        trace_signal_deliver(signr, &ksig->info, ka);

        // ★ 处理信号动作
        if (ka->sa.sa_handler == SIG_IGN) {
            // 信号被忽略
            continue;
        }

        if (ka->sa.sa_handler != SIG_DFL) {
            // 用户注册了信号处理函数
            ksig->ka = *ka;
            if (ka->sa.sa_flags & SA_ONESHOT)
                ka->sa.sa_handler = SIG_DFL;
            break;  // 返回信号号，后续 setup_rt_frame
        }

        // ★ 默认动作处理
        if (sig_kernel_ignore(signr))  // SIGCHLD, SIGURG, SIGWINCH
            continue;

        if (unlikely(signal->flags & SIGNAL_UNKILLABLE) &&
                !sig_kernel_only(signr))
            continue;

        if (sig_kernel_stop(signr)) {
            // SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU
            if (signr != SIGSTOP) {
                spin_unlock_irq(&sighand->siglock);
                if (is_current_pgrp_orphaned())
                    goto relock;
                spin_lock_irq(&sighand->siglock);
            }
            if (likely(do_signal_stop(signr)))
                goto relock;
            continue;
        }

    fatal:
        // ★ 致命信号处理
        spin_unlock_irq(&sighand->siglock);

        // 禁止当前产生 coredump 时通知
        if (unlikely(sig_kernel_coredump(signr))) {
            if (coredump_drop_current_signal())
                continue;
            do_coredump(signr);
        }

        // 进程退出
        do_group_exit(signr);
    }

    spin_unlock_irq(&sighand->siglock);
    return false;  // 返回 false 表示没有需要处理的信号
}
```

### 7.2 信号出队

```c
// kernel/signal.c:618
int dequeue_signal(sigset_t *mask, kernel_siginfo_t *info, enum pid_type *type)
{
    struct task_struct *tsk = current;
    struct sigqueue *timer_sigq;
    int signr;

again:
    *type = PIDTYPE_PID;
    timer_sigq = NULL;
    // 先检查线程私有队列
    signr = __dequeue_signal(&tsk->pending, mask, info, &timer_sigq);
    if (!signr) {
        // 再检查进程共享队列
        *type = PIDTYPE_TGID;
        signr = __dequeue_signal(&tsk->signal->shared_pending,
                                 mask, info, &timer_sigq);
        if (unlikely(signr == SIGALRM))
            posixtimer_rearm_itimer(tsk);
    }

    recalc_sigpending();

    if (unlikely(sig_kernel_stop(signr)))
        current->jobctl |= JOBCTL_STOP_DEQUEUED;

    if (IS_ENABLED(CONFIG_POSIX_TIMERS) && unlikely(timer_sigq)) {
        if (!posixtimer_deliver_signal(info, timer_sigq))
            goto again;
    }

    return signr;
}
```

**信号查找优先级：**

```
同步信号（硬件异常） > 线程私有队列 > 进程共享队列
```

### 7.3 信号默认动作分类

```c
// include/linux/signal.h
// 忽略类信号
#define SIG_KERNEL_IGNORE_MASK (\
    sigmask(SIGCONT)   | sigmask(SIGCHLD)  | \
    sigmask(SIGWINCH)  | sigmask(SIGURG)   )

// 停止类信号
#define SIG_KERNEL_STOP_MASK (\
    sigmask(SIGSTOP)   | sigmask(SIGTSTP)  | \
    sigmask(SIGTTIN)   | sigmask(SIGTTOU)  )

// coredump 类信号
#define SIG_KERNEL_COREDUMP_MASK (\
    sigmask(SIGQUIT)   | sigmask(SIGILL)   | \
    sigmask(SIGTRAP)   | sigmask(SIGABRT)  | \
    sigmask(SIGFPE)    | sigmask(SIGSEGV)  | \
    sigmask(SIGBUS)    | sigmask(SIGSYS)   | \
    sigmask(SIGXCPU)   | sigmask(SIGXFSZ)  | \
    sigmask(SIGIOT)    | sigmask(SIGEMT)   )
```

**信号默认动作：**

| 类别 | 信号 | 默认动作 |
|------|------|----------|
| ignore | SIGCONT, SIGCHLD, SIGWINCH, SIGURG | 忽略 |
| stop | SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU | 停止进程 |
| coredump | SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGFPE, SIGSEGV, SIGBUS, SIGSYS, SIGXCPU, SIGXFSZ | 产生 coredump 后退出 |
| terminate | SIGTERM, SIGHUP, SIGINT, SIGKILL, SIGPIPE, SIGALRM, SIGUSR1, SIGUSR2, SIGPROF, SIGVTALRM, SIGSTKFLT, SIGIO, SIGPWR | 直接退出 |
| 特殊 | SIGKILL | 不能被阻塞/忽略/捕获 |
| 特殊 | SIGSTOP | 不能被阻塞/忽略/捕获 |

## 8. 信号处理函数执行（ARM64 架构）

### 8.1 handle_signal：设置信号帧

当 `get_signal` 返回用户注册的信号处理函数时，`handle_signal` 负责在用户栈上构建信号帧：

```c
// arch/arm64/kernel/signal.c:1556
static void handle_signal(struct ksignal *ksig, struct pt_regs *regs)
{
    sigset_t *oldset = sigmask_to_save();
    int usig = ksig->sig;
    int ret;

    rseq_signal_deliver(ksig, regs);

    // 在用户栈上设置信号帧
    if (is_compat_task()) {
        if (ksig->ka.sa.sa_flags & SA_SIGINFO)
            ret = compat_setup_rt_frame(usig, ksig, oldset, regs);
        else
            ret = compat_setup_frame(usig, ksig, oldset, regs);
    } else {
        ret = setup_rt_frame(usig, ksig, oldset, regs);
    }

    ret |= !valid_user_regs(&regs->user_regs, current);

    // 单步追踪支持
    signal_setup_done(ret, ksig, test_thread_flag(TIF_SINGLESTEP));
}
```

### 8.2 setup_rt_frame：构建信号帧

```c
// arch/arm64/kernel/signal.c:1505
static int setup_rt_frame(int usig, struct ksignal *ksig, sigset_t *set,
                          struct pt_regs *regs)
{
    struct rt_sigframe_user_layout user;
    struct rt_sigframe __user *frame;
    struct user_access_state ua_state;
    int err = 0;

    // 保存浮点寄存器状态
    fpsimd_save_and_flush_current_state();

    // 获取信号帧在栈上的位置
    if (get_sigframe(&user, ksig, regs))
        return 1;

    save_reset_user_access_state(&ua_state);
    frame = user.sigframe;

    // 填充 ucontext
    __put_user_error(0, &frame->uc.uc_flags, err);
    __put_user_error(NULL, &frame->uc.uc_link, err);
    err |= __save_altstack(&frame->uc.uc_stack, regs->sp);
    err |= setup_sigframe(&user, regs, set, &ua_state);  // 保存寄存器上下文
    if (ksig->ka.sa.sa_flags & SA_SIGINFO)
        err |= copy_siginfo_to_user(&frame->info, &ksig->info);  // 保存 siginfo

    // 设置返回地址和寄存器
    if (err == 0)
        err = setup_return(regs, ksig, &user, usig);

    if (err == 0)
        set_handler_user_access_state();
    else
        restore_user_access_state(&ua_state);

    return err;
}
```

### 8.3 rt_sigframe 结构

信号帧在用户栈上的布局：

```c
// arch/arm64/kernel/signal.c:45
struct rt_sigframe {
    struct siginfo info;          // 信号信息 (SA_SIGINFO 时有效)
    struct ucontext uc;           // 用户上下文
    // 以下是 uc.uc_mcontext.__reserved 中的扩展数据
    struct fpsimd_context fpsimd;  // 浮点寄存器状态
    struct esr_context esr;       // 异常综合寄存器
    struct sve_context sve;       // SVE 寄存器（可选）
    struct za_context za;         // SME ZA 数组（可选）
    struct zt_context zt;         // SME ZT 数组（可选）
    struct fpmr_context fpmr;     // 浮点模式寄存器
    struct poe_context poe;       // 权限覆盖扩展
    struct gcs_context gcs;       // 影子栈（可选）
    // ...
    struct _aarch64_ctx terminator;  // 终止标记
};
```

### 8.4 setup_return：设置信号处理函数入口

`setup_return` 修改 `pt_regs`，使信号处理函数在返回用户空间时被执行：

```c
// arch/arm64/kernel/signal.c:1443
static int setup_return(struct pt_regs *regs, struct ksignal *ksig,
                        struct rt_sigframe_user_layout *user, int usig)
{
    __sigrestore_t sigtramp;

    // 确定信号返回路径（trampoline）
    if (ksig->ka.sa.sa_flags & SA_RESTORER)
        sigtramp = ksig->ka.sa.sa_restorer;
    else
        sigtramp = VDSO_SYMBOL(current->mm->context.vdso, sigtramp);

    // 设置 GCS（影子栈）入口
    err = gcs_signal_entry(sigtramp, ksig);
    if (err)
        return err;

    // 设置信号处理函数参数
    regs->regs[0] = usig;                          // x0 = 信号编号
    if (ksig->ka.sa.sa_flags & SA_SIGINFO) {
        regs->regs[1] = (unsigned long)&user->sigframe->info;  // x1 = siginfo
        regs->regs[2] = (unsigned long)&user->sigframe->uc;    // x2 = ucontext
    }
    regs->sp = (unsigned long)user->sigframe;      // sp = 信号帧基址
    regs->regs[29] = (unsigned long)&user->next_frame->fp;  // x29(fp) = 帧指针
    regs->regs[30] = (unsigned long)sigtramp;      // x30(lr) = sigtramp 返回地址
    regs->pc = (unsigned long)ksig->ka.sa.sa_handler;  // pc = 信号处理函数

    // 设置 BTI 类型
    if (system_supports_bti()) {
        regs->pstate &= ~PSR_BTYPE_MASK;
        regs->pstate |= PSR_BTYPE_C;  // 设置为 CALL 类型
    }
    regs->pstate &= ~PSR_TCO_BIT;  // 清除标签检查覆盖

    // 禁用 SME 流模式
    if (system_supports_sme()) {
        task_smstop_sm(current);
        current->thread.svcr &= ~SVCR_ZA_MASK;
        write_sysreg_s(0, SYS_TPIDR2_EL0);
    }

    return 0;
}
```

**寄存器修改总结：**

| 寄存器 | 值 | 用途 |
|--------|------|------|
| x0 | usig | 信号编号（传递给信号处理函数第一个参数） |
| x1 | &sigframe->info | siginfo 指针（SA_SIGINFO 时第二个参数） |
| x2 | &sigframe->uc | ucontext 指针（SA_SIGINFO 时第三个参数） |
| sp | sigframe | 栈指针指向信号帧 |
| x29(fp) | &next_frame->fp | 帧指针 |
| x30(lr) | sigtramp | 返回地址 = vdso 中的 sigtramp |
| pc | sa_handler | 信号处理函数入口 |

### 8.5 信号处理函数执行流程

```
用户态执行
    │
    ├── 系统调用/中断/异常
    │
    ▼
内核态 (el0_svc / el0_irq / el0_sync)
    │
    ▼
exit_to_user_mode_loop()
    │
    ▼
arch_do_signal_or_restart()
    │
    ▼
get_signal() → 出队信号
    │
    ├── 用户处理函数 (sa_handler != SIG_DFL)
    │    │
    │    ▼
    │  handle_signal()
    │    │
    │    ├── setup_rt_frame()  ← 在用户栈上构建信号帧
    │    │    ├── get_sigframe()       ← 分配信号帧空间
    │    │    ├── setup_sigframe()     ← 保存当前寄存器
    │    │    └── setup_return()       ← 修改 regs 指向 handler
    │    │
    │    ▼
    │  返回用户空间
    │    │
    │    ┌──────────────────────────────────────┐
    │    │  用户态执行信号处理函数                  │
    │    │  void handler(int sig) {               │
    │    │      // 处理信号                        │
    │    │  }                                      │
    │    │  执行完毕后:                            │
    │    │  BLR x30 → vdso_sigtramp              │
    │    │  → rt_sigreturn 系统调用               │
    │    └──────────────────────────────────────┘
    │    │
    │    ▼
    │  rt_sigreturn()
    │    │
    │    ├── restore_sigframe()  ← 恢复保存的寄存器
    │    ├── restore_altstack()  ← 恢复栈
    │    └── 返回用户态继续执行
    │
    └── 默认动作
         ├── 忽略 → 继续
         ├── 停止 → do_signal_stop() → TASK_STOPPED
         ├── coredump → do_coredump() → do_group_exit()
         └── 退出 → do_group_exit(signr)
```

## 9. 信号恢复（sigreturn）

### 9.1 rt_sigreturn 系统调用

信号处理函数执行完毕后，通过 sigtramp 中的 `rt_sigreturn` 系统调用恢复上下文：

```c
// arch/arm64/kernel/signal.c:1094
SYSCALL_DEFINE0(rt_sigreturn)
{
    struct pt_regs *regs = current_pt_regs();
    struct rt_sigframe __user *frame;

    // 防止挂起的系统调用重启
    current->restart_block.fn = do_no_restart_syscall;

    // 栈对齐检查
    if (regs->sp & 15)
        goto badframe;

    frame = (struct rt_sigframe __user *)regs->sp;

    if (!access_ok(frame, sizeof (*frame)))
        goto badframe;

    // ★ 恢复信号帧中保存的寄存器
    if (restore_sigframe(regs, frame, &ua_state))
        goto badframe;

    if (gcs_restore_signal())
        goto badframe;

    if (restore_altstack(&frame->uc.uc_stack))
        goto badframe;

    restore_user_access_state(&ua_state);

    return regs->regs[0];  // 返回给用户态

badframe:
    arm64_notify_segfault(regs->sp);
    return 0;
}
```

### 9.2 restore_sigframe：恢复寄存器上下文

```c
// arch/arm64/kernel/signal.c:981
static int restore_sigframe(struct pt_regs *regs,
                            struct rt_sigframe __user *sf,
                            struct user_access_state *ua_state)
{
    int err = 0;
    struct user_ctxs user;

    // 恢复通用寄存器
    err |= __copy_from_user(regs, &sf->uc.uc_mcontext, sizeof(struct sigcontext));

    // 恢复信号掩码
    err |= __get_user_error(regs->pc, &sf->uc.uc_mcontext.pc, err);
    err |= __get_user_error(regs->regs[30], &sf->uc.uc_mcontext.sp, err);
    // ... 恢复其他寄存器

    // 恢复浮点寄存器
    if (user.fpsimd) {
        struct fpsimd_state fpsimd;
        err |= __copy_from_user(&fpsimd, user.fpsimd, sizeof(fpsimd));
        fpsimd_update_current_state(&fpsimd);
    }

    // 恢复 SVE/SME 等扩展寄存器
    // ...

    return err;
}
```

## 10. 完整信号生命周期流程图

```
┌──────────────────────────────────────────────────────────────────────┐
│                    kill() 信号完整生命周期                              │
│                                                                      │
│  ┌─────────────┐                                                      │
│  │ 用户程序调用  │                                                      │
│  │ kill(pid,sig)│                                                      │
│  └──────┬──────┘                                                      │
│         │                                                              │
│         ▼                                                              │
│  ┌──────────────────────────────────────────────────┐                  │
│  │ 阶段1: kill() 系统调用内核路径                     │                  │
│  │                                                  │                  │
│  │  SYSCALL_DEFINE2(kill)                          │                  │
│  │    └─ prepare_kill_siginfo() ← 构造 siginfo     │                  │
│  │    └─ kill_something_info()                      │                  │
│  │         ├─ pid>0  → kill_proc_info()             │                  │
│  │         ├─ pid=0  → __kill_pgrp_info()           │                  │
│  │         ├─ pid=-1 → for_each_process()           │                  │
│  │         └─ pid<-1 → kill_pid_info()              │                  │
│  │              └─ group_send_sig_info()             │                  │
│  │                   ├─ check_kill_permission()     │                  │
│  │                   └─ do_send_sig_info()          │                  │
│  │                        └─ __send_signal_locked() │                  │
│  └──────────────────────────────────────────────────┘                  │
│         │                                                              │
│         ▼                                                              │
│  ┌──────────────────────────────────────────────────┐                  │
│  │ 阶段2: 信号入队 __send_signal_locked()            │                  │
│  │                                                  │                  │
│  │  ├─ prepare_signal()                             │                  │
│  │  │   ├─ 检查 SIGNAL_GROUP_EXIT                   │                  │
│  │  │   ├─ STOP信号→清除SIGCONT队列                 │                  │
│  │  │   └─ SIGCONT→清除STOP队列,唤醒STOPPED线程     │                  │
│  │  ├─ 选择队列: shared_pending 或 pending           │                  │
│  │  ├─ legacy_queue() → 不可靠信号去重               │                  │
│  │  ├─ alloc_sigqueue() → 分配队列条目               │                  │
│  │  ├─ 填充 siginfo 信息                            │                  │
│  │  ├─ list_add_tail() → 入队                       │                  │
│  │  ├─ sigaddset() → 位图置位                       │                  │
│  │  └─ complete_signal() → 递送决策                 │                  │
│  └──────────────────────────────────────────────────┘                  │
│         │                                                              │
│         ▼                                                              │
│  ┌──────────────────────────────────────────────────┐                  │
│  │ 阶段3: 递送决策 complete_signal()                 │                  │
│  │                                                  │                  │
│  │  ├─ 选择不阻塞此信号的线程                         │                  │
│  │  ├─ 致命信号? → 设置SIGNAL_GROUP_EXIT            │                  │
│  │  │   └─ 所有线程添加SIGKILL,全部唤醒              │                  │
│  │  └─ 普通信号 → signal_wake_up()                  │                  │
│  │       ├─ set_tsk_thread_flag(TIF_SIGPENDING)     │                  │
│  │       ├─ wake_up_state() → 唤醒睡眠进程           │                  │
│  │       └─ kick_process() → IPI中断运行中进程       │                  │
│  └──────────────────────────────────────────────────┘                  │
│         │                                                              │
│         ▼                                                              │
│  ┌──────────────────────────────────────────────────┐                  │
│  │ 阶段4: 信号递送时机                                │                  │
│  │                                                  │                  │
│  │  exit_to_user_mode_loop()                         │                  │
│  │    └─ arch_do_signal_or_restart()                │                  │
│  │         └─ get_signal()                          │                  │
│  └──────────────────────────────────────────────────┘                  │
│         │                                                              │
│         ▼                                                              │
│  ┌──────────────────────────────────────────────────┐                  │
│  │ 阶段5: 信号处理 get_signal()                       │                  │
│  │                                                  │                  │
│  │  ├─ SIGNAL_GROUP_EXIT → goto fatal               │                  │
│  │  ├─ SIGNAL_CLD_MASK → 通知父进程                  │                  │
│  │  ├─ JOBCTL_STOP_PENDING → do_signal_stop()       │                  │
│  │  ├─ JOBCTL_TRAP_MASK → do_jobctl_trap()          │                  │
│  │  ├─ dequeue_signal() → 出队信号                   │                  │
│  │  ├─ ptrace_signal() → ptrace 拦截                │                  │
│  │  ├─ SIG_IGN → 忽略,继续循环                       │                  │
│  │  ├─ 用户handler → 填充ksig,返回true               │                  │
│  │  ├─ 默认忽略 → 继续循环                            │                  │
│  │  ├─ 默认停止 → do_signal_stop()                   │                  │
│  │  └─ 默认致命 → do_coredump / do_group_exit()      │                  │
│  └──────────────────────────────────────────────────┘                  │
│         │                                                              │
│         ▼                                                              │
│  ┌──────────────────────────────────────────────────┐                  │
│  │ 阶段6: 信号处理函数执行 (ARM64)                    │                  │
│  │                                                  │                  │
│  │  handle_signal()                                 │                  │
│  │    └─ setup_rt_frame()                           │                  │
│  │         ├─ get_sigframe() → 栈上分配信号帧       │                  │
│  │         ├─ setup_sigframe() → 保存寄存器         │                  │
│  │         │   ├─ 通用寄存器 (x0-x30, pc, sp)       │                  │
│  │         │   ├─ 浮点寄存器 (FPSIMD)               │                  │
│  │         │   ├─ SVE/SME寄存器 (可选)              │                  │
│  │         │   ├─ ESR 异常信息                      │                  │
│  │         │   ├─ 信号掩码 (oldset)                  │                  │
│  │         │   └─ 备用栈设置                        │                  │
│  │         └─ setup_return() → 修改 regs            │                  │
│  │              ├─ x0 = 信号编号                    │                  │
│  │              ├─ x1 = siginfo指针                 │                  │
│  │              ├─ x2 = ucontext指针                │                  │
│  │              ├─ sp = 信号帧栈顶                  │                  │
│  │              ├─ lr(x30) = sigtramp              │                  │
│  │              └─ pc = sa_handler                  │                  │
│  │                                                  │                  │
│  │  返回用户空间 → 执行信号处理函数                    │                  │
│  │  → 函数返回时 BLR x30 → sigtramp                 │                  │
│  │  → rt_sigreturn() 系统调用                       │                  │
│  │    └─ restore_sigframe() 恢复所有寄存器            │                  │
│  │    └─ 恢复信号掩码和栈                            │                  │
│  │    └─ 返回用户态继续执行                          │                  │
│  └──────────────────────────────────────────────────┘                  │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

## 11. 信号掩码与阻塞机制

### 11.1 信号阻塞流程

```c
// 信号掩码存储
struct task_struct {
    sigset_t blocked;        // 当前阻塞的信号集
    sigset_t real_blocked;   // 真正的阻塞集（ptrace 时暂存）
};

// sigprocmask 系统调用
SYSCALL_DEFINE3(sigprocmask, int, how, sigset_t __user *, nset,
                sigset_t __user *, oset)
{
    // SIGKILL 和 SIGSTOP 不能被阻塞
    // how: SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK
    // 更新 current->blocked
}
```

**阻塞对信号发送的影响：**
- 信号被阻塞时，仍然可以通过 `__send_signal_locked` 入队（设置 `TIF_SIGPENDING`）
- 但在 `dequeue_signal` 时会检查 `current->blocked` 掩码，被阻塞的信号不会被取出
- `wants_signal` 检查 `sigismember(&p->blocked, sig)`，如果被阻塞则不选择该线程

### 11.2 recalc_sigpending 重新计算挂起状态

```c
// kernel/signal.c:177
void recalc_sigpending(void)
{
    if (!recalc_sigpending_tsk(current) && !freezing(current)) {
        if (unlikely(test_thread_flag(TIF_SIGPENDING)))
            clear_thread_flag(TIF_SIGPENDING);
    }
}

// 检查是否有未阻塞的挂起信号
bool recalc_sigpending_tsk(struct task_struct *t)
{
    if (t->signal->group_stop_count > 0 ||
        t->signal->flags & SIGNAL_GROUP_EXIT)
        return true;

    if (sigisemptyset(&t->pending.signal) &&
        sigisemptyset(&t->signal->shared_pending.signal))
        return false;

    // 检查是否有未被阻塞的信号
    return !sigisemptyset(&t->pending.signal) ||
           !sigisemptyset(&t->signal->shared_pending.signal);
}
```

## 12. 多线程信号处理

### 12.1 信号发送方向

```
kill(pid, SIGTERM)
    │
    └── group_send_sig_info(sig, info, p, PIDTYPE_TGID)
         │
         ├── pending = &p->signal->shared_pending  ← 共享队列
         │
         └── complete_signal(sig, p, PIDTYPE_TGID)
              │
              ├── 选择一个不阻塞此信号的线程
              │   ├── wants_signal(sig, t)
              │   └── 轮询 signal->curr_target
              │
              └── signal_wake_up(t, ...)
                   └── 只唤醒选中的线程
```

**多线程信号递送规则：**
- 进程定向信号（kill）入共享队列，选择一个线程唤醒
- 线程定向信号（tkill）入线程私有队列，唤醒指定线程
- 同步信号（SIGSEGV 等）递送给触发线程
- 致命信号启动组退出，所有线程收到 SIGKILL

### 12.2 信号掩码的继承

```c
// fork 时子进程继承父进程的信号掩码
// clone 时新线程可以指定新的信号掩码

// pthread_create 中：
// 新线程默认继承调用者的信号掩码
// 但通常在创建线程前会设置线程信号掩码
```

## 13. 信号使用场景

| 场景 | 信号 | 说明 |
|------|------|------|
| 进程间通信 | SIGTERM, SIGUSR1/2 | 请求正常退出 |
| 强制终止 | SIGKILL | 不可阻塞/忽略/捕获 |
| 作业控制 | SIGSTOP, SIGCONT, SIGTSTP | 暂停/继续进程组 |
| 硬件异常 | SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGTRAP | 段错误、除零等 |
| 定时器 | SIGALRM, SIGVTALRM, SIGPROF | 定时器超时 |
| 子进程管理 | SIGCHLD | 子进程退出/停止/继续 |
| 调试 | SIGTRAP | 断点、单步执行 |
| 用户态 | SIGINT (Ctrl+C), SIGQUIT (Ctrl+\) | 终端中断 |
| 管道 | SIGPIPE | 写已关闭的管道 |
| 系统通知 | SIGHUP | 终端断开、配置重载 |

## 14. 权限检查

```c
// kernel/signal.c:1177
static int check_kill_permission(int sig, struct kernel_siginfo *info,
                                 struct task_struct *t)
{
    // 空信号检测（仅用于检查进程是否存在）
    if (!valid_signal(sig))
        return -EINVAL;

    // 内核发送的信号总是允许
    if (info != SEND_SIG_NOINFO && (is_si_special(info) || SI_FROMKERNEL(info)))
        return 0;

    // 权限检查：CAP_KILL 或相同用户
    error = audit_signal_info(sig, t);
    if (error)
        return error;

    return capable(CAP_KILL) ||
           (uid_eq(task_uid(t), current_uid()) ||
            uid_eq(task_uid(t), current_euid()))
           ? 0 : -EPERM;
}
```

## 15. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效信号 | sig 不在 1~_NSIG 范围内 |
| EPERM | 权限不足 | 调用者无权向目标进程发送信号 |
| ESRCH | 目标不存在 | 指定的 pid 对应的进程不存在 |
| 0 (sig=0) | 进程存在但有权限 | 使用空信号进行探测 |

## 16. 使用示例

### 16.1 基本信号发送与处理

```c
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// 信号处理函数
static void sig_handler(int sig)
{
    // 注意：信号处理函数中只能调用异步信号安全（async-signal-safe）的函数
    const char *msg = "Caught signal\n";
    write(STDOUT_FILENO, msg, strlen(msg));
}

int main(void)
{
    struct sigaction sa;

    // 注册信号处理函数
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);  // 处理期间不阻塞其他信号
    sa.sa_flags = SA_RESTART;  // 自动重启被中断的系统调用

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    printf("PID: %d, waiting for SIGTERM...\n", getpid());
    printf("Try: kill -TERM %d\n", getpid());

    // pause() 会挂起进程直到收到信号
    pause();

    printf("Signal handler returned\n");
    return 0;
}
```

### 16.2 信号发送者的权限检查

```c
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <pid> <sig>\n", argv[0]);
        return 1;
    }

    pid_t pid = atoi(argv[1]);
    int sig = atoi(argv[2]);

    // 使用 sig=0 检查进程是否存在及权限
    if (kill(pid, 0) == -1) {
        if (errno == ESRCH)
            printf("Process %d does not exist\n", pid);
        else if (errno == EPERM)
            printf("Permission denied to signal process %d\n", pid);
        else
            perror("kill");
        return 1;
    }

    printf("Process %d exists and we have permission\n", pid);

    // 发送实际信号
    if (kill(pid, sig) == -1) {
        perror("kill");
        return 1;
    }

    printf("Signal %d sent to process %d\n", sig, pid);
    return 0;
}
```

### 16.3 进程组信号

```c
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    pid_t pid1, pid2;

    // 创建两个子进程
    if ((pid1 = fork()) == 0) {
        // 第一个子进程
        while (1) {
            printf("Child 1 (PID: %d, PGID: %d) running\n",
                   getpid(), getpgrp());
            sleep(2);
        }
    }

    if ((pid2 = fork()) == 0) {
        // 第二个子进程
        while (1) {
            printf("Child 2 (PID: %d, PGID: %d) running\n",
                   getpid(), getpgrp());
            sleep(2);
        }
    }

    // 父进程等待 3 秒后向整个进程组发送 SIGTERM
    sleep(3);
    printf("Parent sending SIGTERM to process group (PGID: %d)\n",
           getpgrp());

    // pid=0 发送给当前进程组的所有进程
    kill(0, SIGTERM);

    printf("Parent done\n");
    return 0;
}
```

## 17. 关键函数调用栈

### 17.1 信号发送路径

```
kill(pid, sig)
  └─ SYSCALL_DEFINE2(kill, pid, sig)           // kernel/signal.c:3947
       └─ kill_something_info(sig, info, pid)  // kernel/signal.c:1572
            ├─ kill_proc_info(sig, info, pid)  // pid > 0
            │    └─ group_send_sig_info(sig, info, p, PIDTYPE_TGID)
            │         ├─ check_kill_permission(sig, info, p)
            │         └─ do_send_sig_info(sig, info, p, PIDTYPE_TGID)
            │              └─ __send_signal_locked(sig, info, p, type, force)
            │                   ├─ prepare_signal(sig, p, force)     // 预处理
            │                   ├─ legacy_queue(pending, sig)        // 去重检查
            │                   ├─ alloc_sigqueue() → list_add_tail // 入队
            │                   ├─ sigaddset(&pending->signal, sig) // 位图置位
            │                   └─ complete_signal(sig, p, type)    // 递送决策
            │                        └─ signal_wake_up(t, ...)      // 唤醒
            │                             ├─ set_tsk_thread_flag(TIF_SIGPENDING)
            │                             ├─ wake_up_state(t, ...)
            │                             └─ kick_process(t)         // IPI
            ├─ __kill_pgrp_info(sig, info, pgrp)  // pid == 0
            ├─ for_each_process(p)                 // pid == -1
            └─ kill_pid_info(sig, info, pid)       // pid < -1
```

### 17.2 信号递送路径

```
exit_to_user_mode_loop(regs, ti_work)       // kernel/entry/common.c:41
  └─ arch_do_signal_or_restart(regs)        // arch/arm64/kernel/signal.c:1594
       └─ get_signal(&ksig)                 // kernel/signal.c:2799
            ├─ task_sigpending() → 检查 TIF_SIGPENDING
            ├─ dequeue_signal(&blocked, &info, &type)  // 出队信号
            │    ├─ __dequeue_signal(&tsk->pending, ...)       // 线程私有队列
            │    └─ __dequeue_signal(&shared_pending, ...)     // 共享队列
            │         └─ next_signal(pending, mask)            // 查找第一个信号
            │              └─ collect_signal(sig, pending, ...) // 收集信号信息
            ├─ ptrace_signal() → ptrace 拦截
            ├─ SIG_IGN → continue
            ├─ 用户 handler → 返回 true
            ├─ 默认忽略 → continue
            ├─ 默认停止 → do_signal_stop()
            └─ 默认致命 → do_coredump() / do_group_exit()

  // 用户 handler 路径
  └─ handle_signal(&ksig, regs)            // arch/arm64/kernel/signal.c:1556
       └─ setup_rt_frame(usig, ksig, oldset, regs) // arch/arm64/kernel/signal.c:1505
            ├─ get_sigframe(&user, ksig, regs)     // 计算信号帧位置
            ├─ setup_sigframe(&user, regs, set, ...) // 保存寄存器到信号帧
            │    ├─ __put_user(regs->regs, &frame->uc.uc_mcontext.regs)
            │    ├─ fpsimd_signal_preserve_current_state()
            │    └─ __put_user(sigset, &frame->uc.uc_sigmask)  // 保存信号掩码
            └─ setup_return(regs, ksig, &user, usig)  // 修改 regs
                 ├─ regs->regs[0] = usig              // x0 = 信号编号
                 ├─ regs->regs[1] = &info             // x1 = siginfo 指针
                 ├─ regs->regs[2] = &uc               // x2 = ucontext 指针
                 ├─ regs->sp = sigframe               // sp = 信号帧
                 ├─ regs->regs[30] = sigtramp         // lr = sigtramp
                 └─ regs->pc = sa_handler             // pc = 处理函数

  // 信号恢复路径
  └─ rt_sigreturn()                         // arch/arm64/kernel/signal.c:1094
       └─ restore_sigframe(regs, frame, ...) // 从信号帧恢复寄存器
            ├─ __copy_from_user(regs, &sf->uc.uc_mcontext)  // 恢复通用寄存器
            ├─ fpsimd_update_current_state()                // 恢复浮点寄存器
            └─ set_current_blocked()                        // 恢复信号掩码
```

## 18. 参考

- 源码位置：
  - `kernel/signal.c` — 信号核心实现（kill, signal, sigaction, get_signal 等）
  - `arch/arm64/kernel/signal.c` — ARM64 架构信号处理（setup_rt_frame, handle_signal, rt_sigreturn）
  - `kernel/entry/common.c` — 信号递送时机（exit_to_user_mode_loop）
  - `include/linux/signal.h` — 信号常量、数据结构定义
  - `include/uapi/asm-generic/signal.h` — 用户态信号定义
  - `include/linux/irq-entry-common.h` — 退出到用户模式的工作机制
- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)