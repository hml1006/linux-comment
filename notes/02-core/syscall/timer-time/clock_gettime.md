### 5.1 clock_gettime - kernel/time/posix-timers.c:1127

```c
SYSCALL_DEFINE2(clock_gettime, const clockid_t, which_clock,
        struct __kernel_timespec __user *, tp)
{
    const struct k_clock *kc = clockid_to_kclock(which_clock);
    struct timespec64 kernel_tp;
    error = kc->clock_get_timespec(which_clock, &kernel_tp);
    return put_timespec64(&kernel_tp, tp);
}
```

vDSO 加速路径（用户态无需系统调用）：
```
// lib/vdso/gettimeofday.c
__vdso_clock_gettime(clock, ts)
  ├─ __arch_get_hw_counter(cs_monotonic_mask, &ts)       // ARM64 CNTVCT_EL0 读
  │    └─ isb; cntvct_el0 = read_sysreg(CNTVCT_EL0)      // 虚拟计数器寄存器
  ├─ clocksource 时间计算
  └─ 若 vDSO 无法处理 → 系统调用回退
```
