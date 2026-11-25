### 3.2 timer_settime - kernel/time/posix-timers.c:947

```c
SYSCALL_DEFINE4(timer_settime, timer_t, timer_id, int, flags,
        const struct __kernel_itimerspec __user *, new_setting,
        struct __kernel_itimerspec __user *, old_setting)
{
    return do_timer_settime(timer_id, flags, &new_spec, rtn);
}
```

```
do_timer_settime(timer_id, flags, &new_spec, rtn)
  ├─ lock_timer(timer_id) → timr                           // 加锁 + 查找
  ├─ if (o) *o = timr->it                               // 保存旧值
  ├─ timr->it = new_spec                                // 设置新定时器值
  │    ├─ it_value: 初始到期时间
  │    └─ it_interval: 周期重载时间
  ├─ timr->it_requeue_pending = 0
  ├─ timr->it_active = 0
  ├─ arm_timer(timr)                                      // 激活定时器
  │    └─ hrtimer_start(&timr->it.real.timer, ...)         // 启动 hrtimer
  │         └─ __hrtimer_start_range_ns(timer, tim, delta_ns, mode, ...)
  │              ├─ hrtimer_remove(timer)                  // 先从红黑树移除
  │              ├─ timer->state &= ~HRTIMER_STATE_ENQUEUED
  │              ├─ timer->node.expires = tim              // 设置到期时间
  │              ├─ timer->state |= HRTIMER_STATE_ENQUEUED
  │              └─ enqueue_hrtimer(timer, ...)             // 插入红黑树
  │                   └─ timerqueue_add(head, &timer->node)
  │                        └─ rb_insert_augmented           // 红黑树插入
  └─ unlock_timer(timr)
```

### 3.3 定时器到期处理

```
hrtimer 到期
  └─ __hrtimer_run_queues(cpu_base, now, flags)           // kernel/time/hrtimer.c
       └─ for (each base)
            └─ hrtimer_run_queues
                 ├─ get_next_timer(base, now)              // 红黑树取最小节点
                 ├─ __run_hrtimer(timer, &base_clock->was_clock_changed)
                 │    └─ timer->function(timer)            // 执行回调
                 │         ├─ posix_timer_fn(timer)         // POSIX 定时器回调
                 │         │    ├─ timr = container_of
                 │         │    ├─ it_real_arm(timr)        // 若周期性→重新 hrtimer_start
                 │         │    └─ 发送信号:
                 │         │         ├─ SIGEV_SIGNAL → send_signal(timr->sigq)
                 │         │         └─ SIGEV_THREAD_ID → 发送到指定线程
                 │         └─ timerfd_tmrproc(timer)        // timerfd 回调
                 │              └─ timerfd_triggered(ctx)
                 │                   ├─ ctx->ticks++        // 增加 tick 计数
                 │                   └─ wake_up(&ctx->wqh)  // 唤醒 reader
                 └─ hrtimer_forward / enqueue 等
```
