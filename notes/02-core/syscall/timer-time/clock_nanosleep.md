### 5.4 clock_nanosleep

与 nanosleep 核心同路径，但支持更多时钟和绝对时间模式：

```
clock_nanosleep(which_clock, flags, rqtp, rmtp)
  └─ kc->nsleep(which_clock, flags, &t, rmtp)
       └─ hrtimer_nanosleep(t, flags & TIMER_ABSTIME ?
                HRTIMER_MODE_ABS : HRTIMER_MODE_REL, which_clock)
```

---

## 6 完整 Mermaid 流程图

```

  === Layer1 - POSIX 定时器 ===
    - timer_create clockid sevp
    - do_timer_create
    - clockid_to_kclock
    - alloc_posix_timer
    - idr 分配 timer ID
    - hrtimer_init
    - timer_settime timerid flags
    - do_timer_settime
    - hrtimer_start -- 红黑树插入
    v
    v

  === Layer2 - 定时器到期 ===
    - hrtimer_interrupt -- 时钟中断
    - __hrtimer_run_queues
    - __run_hrtimer
    ? timer-function
    - posix_timer_fn -- POSIX
    - timerfd_tmrproc -- timerfd
    - send_signal 通知
    - wake_up ctx-wqh
    v
    v
    v

  === Layer3 - timerfd ===
    - timerfd_create clockid flags
    - hrtimer_init
    - anon_inode_getfd
    - timerfd_settime ufd
    - hrtimer_start
    - timerfd_read -- ticks 读取
    - wait_event_interruptible
    v
    v
    v

  === Layer4 - 时间读取 ===
    - clock_gettime clockid tp
    ? vDSO 加速
    - CNTVCT_EL0 直接读
    - 系统调用回退
    - put_timespec64 到用户
    - gettimeofday tv tz
    - ktime_get_real_ts64
    v
    v
    v
    v

  === Layer5 - 睡眠 ===
    - clock_nanosleep clockid flags
    - hrtimer_nanosleep
    - hrtimer_init_sleeper
    - hrtimer_start
    - schedule -- 进程睡眠
    - hrtimer_wakeup -- 到期唤醒
    ? 被信号中断
    - restart_block -- 剩余时间
    - 返回 0
    v
    v
    v
    v
    v
```

---

## 7 完整函数调用链

### 7.1 POSIX 定时器

| 步骤 | 函数 | 文件:行号 | 层 |
|--|--|--|--|
| 1 | `SYSCALL_DEFINE3(timer_create)` | kernel/time/posix-timers.c:566 | Syscall |
| 2 | `do_timer_create(which_clock, event, created_timer_id)` | kernel/time/posix-timers.c:458 | Timer |
| 3 | `clockid_to_kclock(which_clock)` | kernel/time/posix-timers.c | Timer |
| 4 | `alloc_posix_timer()` | kernel/time/posix-timers.c | Timer |
| 5 | `idr_alloc(&posix_timers_id, timer, ...)` | kernel/time/posix-timers.c | Timer |
| 6 | `hrtimer_init(&timer->it.real.timer, clock_id, ...)` | kernel/time/hrtimer.c | hrtimer |
| 7 | `SYSCALL_DEFINE4(timer_settime)` | kernel/time/posix-timers.c:947 | Syscall |
| 8 | `lock_timer(timer_id)` | kernel/time/posix-timers.c | Timer |
| 9 | `arm_timer(timr)` | kernel/time/posix-timers.c | Timer |
| 10 | `hrtimer_start(&timr->it.real.timer, ...)` | kernel/time/hrtimer.c | hrtimer |
| 11 | `__hrtimer_start_range_ns(timer, tim, delta_ns, mode)` | kernel/time/hrtimer.c | hrtimer |
| 12 | `enqueue_hrtimer(timer, ...)` | kernel/time/hrtimer.c | hrtimer |
| 13 | `SYSCALL_DEFINE1(timer_delete)` | kernel/time/posix-timers.c:1052 | Syscall |
| 14 | `hrtimer_cancel(&timr->it.real.timer)` | kernel/time/hrtimer.c | hrtimer |

### 7.2 定时器到期处理

| 步骤 | 函数 | 文件:行号 | 层 |
|--|--|--|--|
| 1 | `hrtimer_interrupt(irq, dev)` | kernel/time/hrtimer.c | hrtimer |
| 2 | `__hrtimer_run_queues(cpu_base, now, flags)` | kernel/time/hrtimer.c | hrtimer |
| 3 | `__run_hrtimer(timer, ...)` | kernel/time/hrtimer.c | hrtimer |
| 4 | `timer->function(timer)` | kernel/time/hrtimer.c | hrtimer |
| 5 | `posix_timer_fn(timer)` | kernel/time/posix-timers.c | Timer |
| 6 | `send_signal(timr->sigq->info.si_signo, ...)` | kernel/signal.c | Signal |

### 7.3 timerfd

| 步骤 | 函数 | 文件:行号 | 层 |
|--|--|--|--|
| 1 | `SYSCALL_DEFINE2(timerfd_create)` | fs/timerfd.c:394 | Syscall |
| 2 | `anon_inode_getfd("[timerfd]", &timerfd_fops, ctx, ...)` | fs/timerfd.c | timerfd |
| 3 | `SYSCALL_DEFINE4(timerfd_settime)` | fs/timerfd.c:548 | Syscall |
| 4 | `do_timerfd_settime(ufd, flags, &new, &old)` | fs/timerfd.c | timerfd |
| 5 | `hrtimer_start(ctx->tmr, ...)` | kernel/time/hrtimer.c | hrtimer |
| 6 | `timerfd_tmrproc(timer)` | fs/timerfd.c | timerfd |
| 7 | `timerfd_read(file, buf, count, ppos)` | fs/timerfd.c | timerfd |

### 7.4 时间读取与睡眠

| 步骤 | 函数 | 文件:行号 | 层 |
|--|--|--|--|
| 1 | `SYSCALL_DEFINE2(clock_gettime)` | kernel/time/posix-timers.c:1127 | Syscall |
| 2 | `kc->clock_get_timespec(which_clock, &kernel_tp)` | kernel/time/posix-timers.c | Timer |
| 3 | `__vdso_clock_gettime` (vDSO) | lib/vdso/gettimeofday.c | vDSO |
| 4 | `CNTVCT_EL0` 直接读取 | arch/arm64/include/asm/arch_timer.h | HW |
| 5 | `SYSCALL_DEFINE2(gettimeofday)` | kernel/time/time.c:140 | Syscall |
| 6 | `ktime_get_real_ts64(&ts)` | kernel/time/timekeeping.c | Timekeeping |
| 7 | `SYSCALL_DEFINE2(nanosleep)` | kernel/time/hrtimer.c:2193 | Syscall |
| 8 | `hrtimer_nanosleep(rqtp, mode, clockid)` | kernel/time/hrtimer.c | hrtimer |
| 9 | `do_nanosleep(t, mode)` | kernel/time/hrtimer.c | hrtimer |
| 10 | `hrtimer_start_expires(&t->timer, mode)` | kernel/time/hrtimer.c | hrtimer |
| 11 | `schedule()` | kernel/sched/core.c | Sched |

---

## 8 关键数据结构

```
struct k_itimer (POSIX 定时器)       struct hrtimer
+-----------------------------+    +---------------------------+
| it_lock (spinlock)          |    | node (rb_node, 红黑树节点)  |
| it_id (timer ID)            |    | _softexpires / expires     |
| it_clock (时钟类型)          |    | state (HRTIMER_STATE_*)   |
| it_signal (通知信号进程)      |    | function (回调函数指针)     |
| it_pid (目标进程)            |    | base (CPU base, 每CPU)    |
| it.real: { timer (hrtimer) }|    +---------------------------+
| it: { it_value, it_interval}|
| it_sigq (sigqueue)          |    struct timerfd_ctx
| it_active / it_requeue      |    +---------------------------+
+-----------------------------+    | wqh (waitqueue)            |
                                   | cancellock / clockid       |
struct k_clock (时钟多态)        | timers (it timerspec64)    |
+-----------------------------+    | ticks (累积 tick 计数)     |
| clock_get_timespec           |    | expired / settime_flags    |
| timer_create / nsleep        |    | tmr (hrtimer 或 alarm)    |
| clock_get / clock_set        |    +---------------------------+
+-----------------------------+
```

---

## 9 总结

定时器与时间系统调用核心路径：

| 操作 | 核心函数 | 定时器类型 | 到期通知方式 |
|--|--|--|--|
| timer_create/settime | `hrtimer_start` | hrtimer | 信号 (SIGEV_SIGNAL/THREAD_ID) |
| timerfd_create/settime | `hrtimer_start` | hrtimer | 文件描述符可读 + epoll |
| nanosleep | `hrtimer_nanosleep` | hrtimer | 唤醒当前进程 |
| clock_gettime | `ktime_get_ts64` / vDSO | 无（时间读取） | 同步返回 |
| clock_nanosleep | `hrtimer_nanosleep` | hrtimer | 唤醒当前进程 |

所有高精度时间操作最终通过 ARM64 `CNTVCT_EL0`（虚拟计数器）或 `CNTPCT_EL0`（物理计数器）寄存器读取硬件时间，hrtimer 通过红黑树组织到期事件，在 `hrtimer_interrupt` 时钟中断中处理到期回调。
