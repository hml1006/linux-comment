### 5.3 nanosleep - kernel/time/hrtimer.c:2193

```c
SYSCALL_DEFINE2(nanosleep, struct __kernel_timespec __user *, rqtp,
        struct __kernel_timespec __user *, rmtp)
{
    return hrtimer_nanosleep(timespec64_to_ktime(tu), HRTIMER_MODE_REL,
                 CLOCK_MONOTONIC);
}
```

```
hrtimer_nanosleep(rqtp, mode, clockid)
  └─ hrtimer_init_sleeper_on_stack(&t, clockid, mode)    // 栈上初始化睡眠定时器
  │    └─ hrtimer_init(&t->timer, clock_id, mode)
  │    └─ t->task = current
  │    └─ t->timer.function = hrtimer_wakeup              // 到期唤醒回调
  ├─ hrtimer_start_expires(&t->timer, mode)               // 启动定时器
  └─ schedule()                                            // 调度/睡眠
       ├─ 定时器到期 → hrtimer_wakeup → try_to_wake_up    // 唤醒
       └─ 若被信号中断 → 恢复剩余时间到 rmtp              // restart_block
```
