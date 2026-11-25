### 3.1 timer_create - kernel/time/posix-timers.c:566

```c
SYSCALL_DEFINE3(timer_create, const clockid_t, which_clock,
        struct sigevent __user *, timer_event_spec,
        timer_t __user *, created_timer_id)
{
    return do_timer_create(which_clock, event, created_timer_id);
}
```

```
do_timer_create(which_clock, event, created_timer_id)
  ├─ clockid_to_kclock(which_clock)                      // 查找 k_clock
  │    ├─ CLOCK_REALTIME → &clock_realtime
  │    ├─ CLOCK_MONOTONIC → &clock_monotonic
  │    ├─ CLOCK_PROCESS_CPUTIME_ID → &process_cpu
  │    └─ CLOCK_THREAD_CPUTIME_ID → &thread_cpu
  ├─ alloc_posix_timer() → new_timer                      // 分配 k_itimer
  │    └─ kmem_cache_alloc(posix_timers_cache, GFP_KERNEL)
  ├─ 为 new_timer 分配 ID（idr 分配）
  ├─ kc->timer_create(new_timer, event)                   // 时钟特定创建
  │    ├─ 普通时钟: posix_timer_create → hrtimer_init
  │    └─ CPU 时钟: posix_cpu_timer_create
  ├─ 若 event 非空: 复制信号通知配置
  │    ├─ SIGEV_SIGNAL: timer->sigq = alloc_sigqueue
  │    └─ SIGEV_THREAD_ID: timer->it_pid = find_task_by_vpid
  └─ copy_to_user(created_timer_id, &new_timer_id)
```
