### 5.2 gettimeofday - kernel/time/time.c:140

```c
SYSCALL_DEFINE2(gettimeofday, struct __kernel_old_timeval __user *, tv,
        struct timezone __user *, tz)
{
    if (tv) {
        struct timespec64 ts;
        ktime_get_real_ts64(&ts);                         // CLOCK_REALTIME
        put_user(ts.tv_sec, &tv->tv_sec);
        put_user(ts.tv_nsec / 1000, &tv->tv_usec);        // 微秒转换
    }
    if (tz)
        copy_to_user(tz, &sys_tz, sizeof(sys_tz));        // 时区信息
    return 0;
}
```
