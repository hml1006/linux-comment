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

## 8. 信号栈帧处理过程（ARM64 架构深度分析）

### 8.1 概述：信号帧的作用

当进程收到需要用户自定义处理的信号时（`sa_handler != SIG_DFL` 且 `!= SIG_IGN`），内核需要在用户栈上构建一个**信号帧（signal frame）**，其作用为：

1. **保存当前上下文**：将 CPU 通用寄存器、SP、PC、PSTATE、FPSIMD/SVE/SME 等状态保存到栈上
2. **建立虚假调用帧**：构造一个"函数调用"环境，让信号处理函数犹如被正常调用一样执行
3. **提供返回路径**：设置返回地址为 sigtramp，使信号处理函数执行完毕后能通过 `rt_sigreturn` 系统调用恢复上下文
4. **记录信号信息**：保存 `siginfo_t` 供信号处理函数查阅

### 8.2 信号帧整体栈布局

```
用户栈布局（从高地址到低地址）:

  ┌──────────────────────────────────────┐  ← 原 sp (中断/异常发生时的栈指针)
  │                                      │
  │     执行 sigsp() 后的栈顶调整          │
  │     SA_ONSTACK 时切换到备用信号栈      │
  │                                      │
  ├──────────────────────────────────────┤  ← sp_top
  │   struct frame_record (fp, lr)       │  16 字节, 用于栈回溯的帧记录
  │   * 此项由 setup_sigframe() 填充为     │
  │     fp = 原 regs[29], lr = 原 regs[30]│
  ├──────────────────────────────────────┤
  │                                      │
  │   struct rt_sigframe                 │
  │                                      │
  │   ├── siginfo.info                   │  SA_SIGINFO 时有效
  │   │   (128 字节)                     │  copy_siginfo_to_user()
  │   │                                   │
  │   ├── ucontext                       │
  │   │   ├── uc_flags                   │  = 0
  │   │   ├── uc_link                    │  = NULL
  │   │   ├── uc_stack                   │  保存 altstack 信息 (sp, size, flags)
  │   │   ├── uc_sigmask                │  保存 oldset（恢复时恢复信号掩码）
  │   │   └── sigcontext                │
  │   │       ├── fault_address          │  current->thread.fault_address
  │   │       ├── regs[0..30]           │  通用寄存器 x0-x30
  │   │       ├── sp                     │  栈指针
  │   │       ├── pc                     │  程序计数器
  │   │       ├── pstate                 │  处理器状态
  │   │       └── __reserved[4096]       │  4KB 预留空间, 包含扩展上下文:
  │   │            ├── fpsimd_context    │  FPSR, FPCR, V0-V31 (128-bit × 32)
  │   │            ├── esr_context       │  异常综合寄存器 (仅同步信号)
  │   │            ├── gcs_context       │  GCS 影子栈状态 (可选)
  │   │            ├── sve_context       │  SVE 寄存器 (可选)
  │   │            ├── tpidr2_context    │  TPIDR2 寄存器 (可选)
  │   │            ├── fpmr_context      │  浮点模式寄存器 (可选)
  │   │            ├── poe_context       │  POE 权限覆盖 (可选)
  │   │            ├── za_context        │  SME ZA 数组 (可选)
  │   │            ├── zt_context        │  SME ZT 数组 (可选)
  │   │            ├── extra_context     │  扩展记录 (可选, 超出 4KB 时)
  │   │            └── terminator        │  {magic=0, size=0}
  │   └──────────────────────────────────┘
  └──────────────────────────────────────┘  ← sp (setup_return 将 sp 指向 sigframe)
```

### 8.3 关键数据结构定义

```c
// ===== 信号帧顶层结构 =====
// arch/arm64/kernel/signal.c:45
struct rt_sigframe {
    struct siginfo info;          // 128 字节, SA_SIGINFO 时有效
    struct ucontext uc;           // 用户上下文（含 sigcontext + __reserved[4096]）
};

// ===== ucontext 结构 =====
// arch/arm64/include/uapi/asm/ucontext.h:22
struct ucontext {
    unsigned long    uc_flags;    // 标志位（当前为 0）
    struct ucontext *uc_link;    // 上下文链（当前为 NULL）
    stack_t          uc_stack;   // 备用栈信息
    sigset_t         uc_sigmask; // 信号掩码
    __u8             __unused[1024/8 - sizeof(sigset_t)];  // 填充至 1024-bit
    struct sigcontext uc_mcontext; // 真正的寄存器上下文
};

// ===== sigcontext 结构 =====
// arch/arm64/include/uapi/asm/sigcontext.h:28
struct sigcontext {
    __u64 fault_address;          // 故障地址（缺页/对齐错误等）
    __u64 regs[31];               // 通用寄存器 x0-x30
    __u64 sp;                     // 栈指针
    __u64 pc;                     // 程序计数器
    __u64 pstate;                 // 处理器状态 (PSTATE/SPSR_EL1)
    __u8 __reserved[4096]         // 16 字节对齐, 4KB 预留
        __attribute__((__aligned__(16)));  // fpsimd_context + 扩展记录
};

// ===== 帧记录（用于栈回溯） =====
// arch/arm64/include/asm/stacktrace/frame.h:32
struct frame_record {
    u64 fp;                       // 父帧指针 (x29)
    u64 lr;                       // 返回地址 (x30)
};

// ===== 扩展上下文头 =====
// arch/arm64/include/uapi/asm/sigcontext.h:70
struct _aarch64_ctx {
    __u32 magic;                  // 魔数标识类型
    __u32 size;                   // 结构总大小（含 head）
};

// ===== FPSIMD 上下文 =====
// arch/arm64/include/uapi/asm/sigcontext.h:77
struct fpsimd_context {
    struct _aarch64_ctx head;     // {FPSIMD_MAGIC, sizeof(fpsimd_context)}
    __u32 fpsr;                   // 浮点状态寄存器
    __u32 fpcr;                   // 浮点控制寄存器
    __uint128_t vregs[32];        // V0-V31 (NEON/FP 寄存器)
};

// ===== ESR 上下文 =====
// arch/arm64/include/uapi/asm/sigcontext.h:96
struct esr_context {
    struct _aarch64_ctx head;     // {ESR_MAGIC, sizeof(esr_context)}
    __u64 esr;                    // 异常综合寄存器 (ESR_EL1)
};

// ===== rt_sigframe_user_layout（布局跟踪结构） =====
// arch/arm64/kernel/signal.c:50
struct rt_sigframe_user_layout {
    struct rt_sigframe __user *sigframe;   // 信号帧基址
    struct frame_record __user *next_frame; // frame_record 位置
    unsigned long size;                      // 已分配大小
    unsigned long limit;                     // 允许的最大大小
    // 各扩展记录的偏移量（0 表示不存在）
    unsigned long fpsimd_offset;
    unsigned long esr_offset;
    unsigned long gcs_offset;
    unsigned long sve_offset;
    unsigned long tpidr2_offset;
    unsigned long za_offset;
    unsigned long zt_offset;
    unsigned long fpmr_offset;
    unsigned long poe_offset;
    unsigned long extra_offset;
    unsigned long end_offset;              // terminator 偏移
};
```

### 8.4 信号帧分配过程：get_sigframe

```c
// arch/arm64/kernel/signal.c:1372
static int get_sigframe(struct rt_sigframe_user_layout *user,
                         struct ksignal *ksig, struct pt_regs *regs)
{
    unsigned long sp, sp_top;
    int err;

    // 1. 初始化布局，计算扩展记录大小
    init_user_layout(user);                     // size = offsetof(rt_sigframe, __reserved)
    err = setup_sigframe_layout(user, false);   // 根据当前硬件特性分配扩展记录偏移
    if (err)
        return err;

    // 2. 确定栈指针位置
    //    sigsp() 判断: SA_ONSTACK 且未在备用栈上时, 切换到 sas_ss_sp
    //    否则使用当前 sp
    sp = sp_top = sigsp(regs->sp, ksig);

    // 3. 分配 frame_record（栈回溯帧记录, 16 字节）
    sp = round_down(sp - sizeof(struct frame_record), 16);
    user->next_frame = (struct frame_record __user *)sp;

    // 4. 分配 rt_sigframe（向下生长, 16 字节对齐）
    sp = round_down(sp, 16) - sigframe_size(user);
    user->sigframe = (struct rt_sigframe __user *)sp;

    // 5. 验证可写性
    if (!access_ok(user->sigframe, sp_top - sp))
        return -EFAULT;

    return 0;
}
```

**栈分配示意图：**

```
高地址
     ┌─────────────────────┐  ← regs->sp (原始栈顶, 调用 sigsp 的输入)
     │                     │
     │  可能切换到备用栈    │  ← sigsp() 判断 SA_ONSTACK
     │  (SA_ONSTACK 时)    │
     ├─────────────────────┤  ← sp_top
     │   frame_record      │  16 字节, 向下增长
     │   (fp, lr)          │
     ├─────────────────────┤
     │                     │
     │   rt_sigframe       │  sigframe_size = round_up(max(user->size,
     │   (不定大小)         │    sizeof(struct rt_sigframe)), 16)
     │                     │
     └─────────────────────┘  ← sp (user->sigframe)
低地址
```

### 8.5 扩展记录布局算法：setup_sigframe_layout

`setup_sigframe_layout` 按固定顺序分配扩展记录在 `__reserved[4096]` 中的位置，最大不超过 4096 字节。超出部分使用 `extra_context` 记录到额外空间：

```c
// arch/arm64/kernel/signal.c:1140
static int setup_sigframe_layout(struct rt_sigframe_user_layout *user, bool add_all)
{
    // 分配顺序（每个成功则 user->size 增加）:
    //
    // 1. fpsimd_offset  ← sizeof(struct fpsimd_context)  // 始终存在
    // 2. esr_offset     ← sizeof(struct esr_context)      // 仅同步信号（fault_code 非零）
    // 3. gcs_offset     ← sizeof(struct gcs_context)      // 仅 CONFIG_ARM64_GCS 启用时
    // 4. sve_offset     ← SVE_SIG_CONTEXT_SIZE(vq)        // SVE/SME 启用且当前为 SVE 模式
    // 5. tpidr2_offset  ← sizeof(struct tpidr2_context)   // TPIDR2 启用
    // 6. za_offset      ← ZA_SIG_CONTEXT_SIZE(vq)         // SME 且 ZA 启用
    // 7. zt_offset      ← ZT_SIG_CONTEXT_SIZE(1)          // SME2 且 ZA 启用
    // 8. fpmr_offset    ← sizeof(struct fpmr_context)     // FPMR 启用
    // 9. poe_offset     ← sizeof(struct poe_context)      // POE 启用
    // 10. end_offset    ← 0 (terminator)                   // 总是
    //
    // 若 size > sizeof(__reserved) - 预留空间, 自动启用 extra_context
    return sigframe_alloc_end(user);
}
```

**扩展记录魔数（magic）表：**

| 魔数 | 宏定义 | 对应结构 |
|------|--------|----------|
| `0x46508001` | `FPSIMD_MAGIC` | fpsimd_context |
| `0x45535201` | `ESR_MAGIC` | esr_context |
| `0x53564501` | `SVE_MAGIC` | sve_context |
| `0x47105301` | `GCS_MAGIC` | gcs_context |
| `0x54505202` | `TPIDR2_MAGIC` | tpidr2_context |
| `0x5a410001` | `ZA_MAGIC` | za_context |
| `0x5a540001` | `ZT_MAGIC` | zt_context |
| `0x46505201` | `FPMR_MAGIC` | fpmr_context |
| `0x504f4530` | `POE_MAGIC` | poe_context |
| `0x45585401` | `EXTRA_MAGIC` | extra_context |
| `0` | — | terminator |

### 8.6 寄存器保存过程：setup_sigframe

`setup_sigframe` 将当前进程的完整执行状态写入用户栈上的信号帧：

```c
// arch/arm64/kernel/signal.c:1239
static int setup_sigframe(struct rt_sigframe_user_layout *user,
                          struct pt_regs *regs, sigset_t *set,
                          const struct user_access_state *ua_state)
{
    struct rt_sigframe __user *sf = user->sigframe;

    // === 第一步: frame_record 栈回溯 ===
    __put_user(regs->regs[29], &user->next_frame->fp);  // fp = 原 x29
    __put_user(regs->regs[30], &user->next_frame->lr);  // lr = 原 x30

    // === 第二步: 通用寄存器 + SP + PC + PSTATE ===
    for (i = 0; i < 31; i++)
        __put_user(regs->regs[i], &sf->uc.uc_mcontext.regs[i]);  // x0-x30
    __put_user(regs->sp,       &sf->uc.uc_mcontext.sp);
    __put_user(regs->pc,       &sf->uc.uc_mcontext.pc);
    __put_user(regs->pstate,   &sf->uc.uc_mcontext.pstate);
    __put_user(current->thread.fault_address, &sf->uc.uc_mcontext.fault_address);

    // === 第三步: 信号掩码 ===
    __copy_to_user(&sf->uc.uc_sigmask, set, sizeof(*set));

    // === 第四步: FPSIMD (始终保存) ===
    preserve_fpsimd_context(fpsimd_ctx);   // 保存 FPSR, FPCR, V0-V31

    // === 第五步: ESR 上下文 (仅同步信号) ===
    preserve_esr_context(esr_ctx);         // current->thread.fault_code

    // === 第六步: 可选扩展 (根据硬件能力) ===
    if (system_supports_gcs())
        preserve_gcs_context(gcs_ctx);     // GCS 影子栈
    if (system_supports_sve() || system_supports_sme())
        preserve_sve_context(sve_ctx);     // SVE/SME 向量寄存器
    if (system_supports_tpidr2())
        preserve_tpidr2_context(tpidr2_ctx);  // TPIDR2_EL0
    if (system_supports_fpmr())
        preserve_fpmr_context(fpmr_ctx);   // 浮点模式寄存器
    if (system_supports_poe())
        preserve_poe_context(poe_ctx, ua_state);  // POR_EL0
    if (system_supports_sme())
        preserve_za_context(za_ctx);       // SME ZA 数组
    if (system_supports_sme2())
        preserve_zt_context(zt_ctx);       // SME ZT 数组

    // === 第七步: extra_context + terminator ===
    // 若 __reserved 空间不足 (size > 4096), 写入 extra_context 记录
    // 最后写入 terminator {magic=0, size=0}

    return err;
}
```

### 8.7 修改 pt_regs 进入信号处理函数：setup_return

```c
// arch/arm64/kernel/signal.c:1443
static int setup_return(struct pt_regs *regs, struct ksignal *ksig,
                        struct rt_sigframe_user_layout *user, int usig)
{
    __sigrestore_t sigtramp;

    // === 第一步: 确定 sigtramp 位置 ===
    if (ksig->ka.sa.sa_flags & SA_RESTORER)
        sigtramp = ksig->ka.sa.sa_restorer;    // 用户提供返回函数
    else
        sigtramp = VDSO_SYMBOL(current->mm->context.vdso, sigtramp);
        // vdso 中的 __kernel_rt_sigreturn

    // === 第二步: GCS (影子栈) 入口设置 ===
    err = gcs_signal_entry(sigtramp, ksig);
    // 在 GCS 上 push cap 和 sigtramp 地址
    // 使信号返回时能从 GCS 正确恢复

    // ──── 此后不得失败 ────

    // === 第三步: 设置信号处理函数参数 (AArch64 ABI) ===
    regs->regs[0] = usig;                              // x0 = 信号编号
    if (ksig->ka.sa.sa_flags & SA_SIGINFO) {
        regs->regs[1] = (unsigned long)&user->sigframe->info;  // x1 = siginfo*
        regs->regs[2] = (unsigned long)&user->sigframe->uc;    // x2 = ucontext*
    }

    // === 第四步: 设置栈和帧指针 ===
    regs->sp = (unsigned long)user->sigframe;          // SP 指向 rt_sigframe
    regs->regs[29] = (unsigned long)&user->next_frame->fp; // FP (x29)

    // === 第五步: 设置返回地址 ===
    regs->regs[30] = (unsigned long)sigtramp;          // LR (x30) = sigtramp
    regs->pc = (unsigned long)ksig->ka.sa.sa_handler;  // PC = 信号处理函数

    // === 第六步: 修改 PSTATE ===
    if (system_supports_bti()) {
        regs->pstate &= ~PSR_BTYPE_MASK;
        regs->pstate |= PSR_BTYPE_C;     // BTI 类型 = CALL (如同 BLR 调用)
    }
    regs->pstate &= ~PSR_TCO_BIT;        // 清除 Tag Check Override

    // === 第七步: 清除 SME 状态 ===
    if (system_supports_sme()) {
        task_smstop_sm(current);         // 退出流模式
        current->thread.svcr &= ~SVCR_ZA_MASK;  // 禁用 ZA
        write_sysreg_s(0, SYS_TPIDR2_EL0);      // 清零 TPIDR2_EL0
    }

    return 0;
}
```

### 8.8 sigtramp VDSO 实现

`sigtramp` 是信号处理函数的返回路径。它位于 vdso 中，是一个极简的汇编函数：

```asm
// arch/arm64/kernel/vdso/sigreturn.S:71
SYM_CODE_START(__kernel_rt_sigreturn)
//  PLEASE DO NOT MODIFY
    mov x8, #__NR_rt_sigreturn     // 将 rt_sigreturn 系统调用号装入 x8
//  PLEASE DO NOT MODIFY
    svc #0                          // 触发系统调用
//  PLEASE DO NOT MODIFY
SYM_CODE_END(__kernel_rt_sigreturn)
```

**设计要点：**

1. **为什么使用 vdso 而不是内联代码？** 每个架构的 sigtramp 代码必须放置在所有进程都能访问的已知地址，vdso 在每个进程的地址空间中固定映射，满足此需求。

2. **神秘的 NOP**：在 `__kernel_rt_sigreturn` 之前有一个 `nop` 指令，这是因为某些 unwinder（如 libc++）会无条件地从 `_Unwind_GetIP()` 的结果中减 1 来识别调用函数。这个 NOP 使减 1 后仍落在可识别的代码区域内。

3. **为什么不使用 BTI 入口**：`SYM_CODE_START` 而非 `SYM_FUNC_START`，不发出 BTI C 指令，因为某些 unwinder 无法识别 BTI 入口点。回归器保证此函数仅从 `RET` 指令到达，因此不需要 landing pad。

4. **如何链接到 vdso**：`VDSO_SYMBOL(current->mm->context.vdso, sigtramp)` 从进程的 vdso 映射中查找 `__kernel_rt_sigreturn` 符号。

### 8.9 寄存器修改总结

执行 `setup_return` 后，`pt_regs` 的内容被修改，当 `eret` 返回用户空间时，CPU 将从信号处理函数开始执行：

```
                 ┌──────────────────────────────────┐
                 │       内核态 pt_regs 修改          │
                 ├──────────────────────────────────┤
                 │  x0 = usig (信号编号)             │
                 │  x1 = &sigframe->info (siginfo)  │
                 │  x2 = &sigframe->uc (ucontext)   │
                 │  sp  = sigframe                  │
                 │  x29 = &next_frame->fp           │
                 │  x30 = __kernel_rt_sigreturn      │
                 │  pc  = sa_handler                 │
                 │  PSTATE.BTYPE = CALL              │
                 │  PSTATE.TCO = 0                   │
                 └──────────────────────────────────┘
                              │
                              │ eret 返回用户态
                              ▼
                 ┌──────────────────────────────────┐
                 │    用户态执行信号处理函数            │
                 │                                  │
                 │  void handler(int sig) {          │
                 │      // ...                      │
                 │  }                                │
                 │                                  │
                 │  信号处理函数通过 RET 返回           │
                 │  → 跳转到 x30 (__kernel_rt_sigreturn) │
                 └──────────────────────────────────┘
                              │
                              ▼
                 ┌──────────────────────────────────┐
                 │  __kernel_rt_sigreturn:           │
                 │    mov x8, #__NR_rt_sigreturn     │
                 │    svc #0    ← 再次进入内核       │
                 └──────────────────────────────────┘
                              │
                              ▼
                 ┌──────────────────────────────────┐
                 │  rt_sigreturn 系统调用             │
                 │  → restore_sigframe() 恢复全部状态  │
                 │  → eret 返回中断点继续执行           │
                 └──────────────────────────────────┘
```

**关键设计决策：**

- **为什么不直接从内核返回中断点？** 信号处理函数的执行在用户态，内核无法直接调用用户态函数。通过修改 `pt_regs`，内核巧妙地让 CPU 在 `eret` 时"错误地"跳转到信号处理函数，而非原先的中断点。
- **为什么使用 sigtramp 而不是让内核直接恢复？** 信号处理函数执行完毕后需要恢复上下文，这必须通过系统调用完成（因为恢复操作需要内核权限）。sigtramp 提供了一个标准化的入口来触发 `rt_sigreturn`。

## 9. 信号恢复（rt_sigreturn 及返回用户态过程）

### 9.1 rt_sigreturn 系统调用

信号处理函数执行完毕后，通过 `RET` 指令跳转到 `x30`（即 `__kernel_rt_sigreturn`），后者执行 `SVC #0` 进入 `rt_sigreturn` 系统调用：

```c
// arch/arm64/kernel/signal.c:1094
SYSCALL_DEFINE0(rt_sigreturn)
{
    struct pt_regs *regs = current_pt_regs();
    struct rt_sigframe __user *frame;
    struct user_access_state ua_state;

    // 1. 防止挂起的系统调用重启
    //    若此前有系统调用被信号中断，不应该在此恢复后重启
    current->restart_block.fn = do_no_restart_syscall;

    // 2. 栈对齐校验 (AAPCS64 要求 SP 16 字节对齐)
    if (regs->sp & 15)
        goto badframe;

    // 3. 信号帧指针 = 当前 SP
    //    因为 setup_return 设置 regs->sp = sigframe
    frame = (struct rt_sigframe __user *)regs->sp;

    if (!access_ok(frame, sizeof(*frame)))
        goto badframe;

    // 4. 保存并重置用户访问状态 (POE)
    save_reset_user_access_state(&ua_state);

    // 5. ★ 恢复寄存器上下文
    if (restore_sigframe(regs, frame, &ua_state))
        goto badframe;

    // 6. GCS 影子栈恢复
    if (gcs_restore_signal())
        goto badframe;

    // 7. 恢复备用栈信息
    if (restore_altstack(&frame->uc.uc_stack))
        goto badframe;

    restore_user_access_state(&ua_state);

    return regs->regs[0];  // 返回 x0 值（系统调用返回值）

badframe:
    arm64_notify_segfault(regs->sp);  // SIGSEGV
    return 0;
}
```

### 9.2 restore_sigframe：从信号帧恢复全部寄存器

```c
// arch/arm64/kernel/signal.c:981
static int restore_sigframe(struct pt_regs *regs,
                            struct rt_sigframe __user *sf,
                            struct user_access_state *ua_state)
{
    sigset_t set;
    int i, err;
    struct user_ctxs user;

    // === 1. 恢复信号掩码 ===
    err = __copy_from_user(&set, &sf->uc.uc_sigmask, sizeof(set));
    if (err == 0)
        set_current_blocked(&set);             // 信号掩码恢复

    // === 2. 恢复通用寄存器 (x0-x30, sp, pc, pstate) ===
    for (i = 0; i < 31; i++)
        __get_user(regs->regs[i], &sf->uc.uc_mcontext.regs[i]);
    __get_user(regs->sp, &sf->uc.uc_mcontext.sp);
    __get_user(regs->pc, &sf->uc.uc_mcontext.pc);
    __get_user(regs->pstate, &sf->uc.uc_mcontext.pstate);

    // === 3. 防止 sys_rt_sigreturn 自身被重启 ===
    forget_syscall(regs);                      // 清空 syscall 信息

    // === 4. 验证恢复后的寄存器合法性 ===
    fpsimd_save_and_flush_current_state();
    err |= !valid_user_regs(&regs->user_regs, current);

    // === 5. 解析扩展记录并恢复 ===
    if (err == 0)
        err = parse_user_sigframe(&user, sf);   // 扫描 __reserved 找到各记录

    if (err == 0 && system_supports_fpsimd()) {
        if (!user.fpsimd)
            return -EINVAL;                      // fpsimd 必须存在
        if (user.sve)
            err = restore_sve_fpsimd_context(&user);  // SVE 格式恢复
        else
            err = restore_fpsimd_context(&user);       // 普通 FPSIMD 恢复
    }

    if (err == 0 && system_supports_gcs() && user.gcs)
        err = restore_gcs_context(&user);        // GCS 影子栈恢复

    if (err == 0 && system_supports_tpidr2() && user.tpidr2)
        err = restore_tpidr2_context(&user);     // TPIDR2_EL0

    if (err == 0 && system_supports_fpmr() && user.fpmr)
        err = restore_fpmr_context(&user);       // 浮点模式寄存器

    if (err == 0 && system_supports_sme() && user.za)
        err = restore_za_context(&user);         // SME ZA 数组

    if (err == 0 && system_supports_sme2() && user.zt)
        err = restore_zt_context(&user);         // SME ZT 数组

    if (err == 0 && system_supports_poe() && user.poe)
        err = restore_poe_context(&user, ua_state);  // POR_EL0

    return err;
}
```

### 9.3 GCS 恢复过程

当启用 GCS（Guarded Control Stack, 影子栈）时，信号返回需要验证并恢复影子栈：

```c
// arch/arm64/kernel/signal.c:1042
static int gcs_restore_signal(void)
{
    u64 gcspr_el0, cap;

    gcspr_el0 = read_sysreg_s(SYS_GCSPR_EL0);

    // 确保之前 GCS 操作的可见性
    gcsb_dsync();

    // GCSPR_EL0 应指向一个 capped GCS, 读取 cap
    copy_from_user(&cap, (unsigned long __user *)gcspr_el0, sizeof(cap));

    // 验证 cap 是否正确
    if (cap != GCS_SIGNAL_CAP(gcspr_el0))
        return -EINVAL;

    // 使 token 失效以防止重用
    put_user_gcs(0, (unsigned long __user *)gcspr_el0, &ret);

    // 恢复 GCSPR_EL0: 跳过 cap 和 sigtramp 条目
    write_sysreg_s(gcspr_el0 + 8, SYS_GCSPR_EL0);

    return 0;
}
```

**GCS 栈操作示意：**

```
信号发送时 (gcs_signal_entry):
  ┌──────────────┐  ← 原 GCSPR_EL0 (A)
  │  cap 条目     │  GCS_SIGNAL_CAP(A-8)
  ├──────────────┤
  │  sigtramp 地址│  __kernel_rt_sigreturn
  └──────────────┘  ← 新 GCSPR_EL0 = A - 16

信号恢复时 (gcs_restore_signal):
  1. 读取当前 GCSPR_EL0 处的 cap, 验证
  2. 将 cap 位置零 (失效)
  3. GCSPR_EL0 += 8 → 跳过 cap 和 sigtramp, 回到 A
```

### 9.4 信号返回后恢复执行的完整流程

```
用户态进入中断点 (如 *p = 42)
    ↓
[数据访问异常 → el0_da → do_page_fault → handle_mm_fault]
    ↑                                        ↓
    │                        do_anonymous_page() 分配页
    │                                        ↓
    │                        返回用户空间 exit_to_user_mode_loop()
    │                                        ↓
    │                        TIF_SIGPENDING 置位 → arch_do_signal_or_restart()
    │                                        ↓
    │                        get_signal() → 取出 SIGSEGV 信号
    │                                        ↓
    │                        handle_signal()  →  保存上下文到信号帧
    │                             setup_sigframe(): 保存 x0-x30, sp, pc, pstate, fpsimd...
    │                             setup_return(): regs->pc = sa_handler
    │                                        ↓
    │                        eret 返回用户态执行信号处理函数
    │                                        ↓
    │  ┌──────────────── 信号处理函数执行 ──────────────┐
    │  │  void handler(int sig) { /* 用户代码 */ }    │
    │  │  RET 指令 → x30(lr) → __kernel_rt_sigreturn  │
    │  └──────────────────────────────────────────────┘
    │                                        ↓
    │                        mov x8, #__NR_rt_sigreturn
    │                        svc #0  → 再次进入内核
    │                                        ↓
    │                        rt_sigreturn():
    │                          restore_sigframe() 恢复 x0-x30, sp, pc, pstate, fpsimd
    │                          set_current_blocked() 恢复信号掩码
    │                          restore_altstack() 恢复备用栈
    │                                        ↓
    └──────── eret 返回原始中断点 ──────────┘
                       ↓
                  *p = 42 指令重新执行
                       ↓
                  页表已建立, 访问成功
```

### 9.5 关键安全与正确性保证

| 机制 | 说明 |
|------|------|
| `valid_user_regs()` 验证 | `restore_sigframe` 后和 `setup_rt_frame` 后都验证 `pt_regs` 的合法性，防止通过伪造信号帧提权 |
| `forget_syscall(regs)` | 防止 `rt_sigreturn` 自身被当做被中断的系统调用来重启 |
| `do_no_restart_syscall` | 清除 `restart_block`, 防止信号处理前的系统调用在信号处理后错误重启 |
| Stack alignment (SP & 15) | AAPCS64 要求 SP 16 字节对齐，信号帧分配确保对齐 |
| `access_ok()` 检查 | 信号帧在用户栈上，写入前检查地址范围有效性 |
| POE 状态管理 | 保存/恢复 POR_EL0，确保信号处理期间不因权限覆盖(Overlay)故障导致 uaccess 失败 |
| GCS cap 验证 | 影子栈上放置特殊 cap 标记，恢复时验证防篡改 |
| `gcsb_dsync()` | 确保 GCS 操作顺序可见性，防止存储缓冲导致的数据不一致 |

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