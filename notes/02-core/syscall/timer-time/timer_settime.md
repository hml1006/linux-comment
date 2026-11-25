# timer_settime 系统调用分析

## 1. 概述

`timer_settime` 用于武装（arm）、重新武装或解除（disarm）一个由 `timer_create` 创建的 POSIX 间隔定时器。

- `it_value` 为 0：解除定时器；
- `it_value` 非 0：定时器在指定时刻（相对或绝对）到期；
- `it_interval` 非 0：定时器周期性触发。

**关键特性：**

- 支持 `TIMER_ABSTIME` 绝对时间，避免周期任务的误差累积
- 可通过 `old_value` 取回设置前的旧值
- 对已到期但尚未投递的信号，重新设置会重置 overrun 计数
- 内核在 `it_lock` 下尝试取消旧定时器；若到期回调正在运行则返回 `TIMER_RETRY` 并重试

**内核源码位置：** `kernel/time/posix-timers.c:962`

---

## 2. 用户态接口

```c
#include <time.h>

int timer_settime(timer_t timerid, int flags,
                  const struct itimerspec *new_value,
                  struct itimerspec *old_value);
```

| 参数 | 说明 |
|------|------|
| `timerid` | `timer_create` 返回的定时器 ID |
| `flags` | `0` 或 `TIMER_ABSTIME` |
| `new_value` | 新的到期时间与周期 |
| `old_value` | 输出旧设置，可为 `NULL` |

```c
struct itimerspec {
    struct timespec it_interval;   // 周期；0 表示单次
    struct timespec it_value;      // 首次到期时间；0 表示解除
};
```

### 返回值

| 返回值 | 说明 |
|--------|------|
| `0` | 成功 |
| `-1` | 失败，`errno` 见第 6 节 |

---

## 3. 内核实现

### 3.1 系统调用入口

```c
// kernel/time/posix-timers.c:962
SYSCALL_DEFINE4(timer_settime, timer_t, timer_id, int, flags,
        const struct __kernel_itimerspec __user *, new_setting,
        struct __kernel_itimerspec __user *, old_setting)
{
    struct itimerspec64 new_spec, old_spec, *rtn;
    int error = 0;

    if (!new_setting)
        return -EINVAL;

    if (get_itimerspec64(&new_spec, new_setting))
        return -EFAULT;

    rtn = old_setting ? &old_spec : NULL;
    error = do_timer_settime(timer_id, flags, &new_spec, rtn);
    if (!error && old_setting) {
        if (put_itimerspec64(&old_spec, old_setting))
            error = -EFAULT;
    }
    return error;
}
```

入口负责 32/64 位时间结构转换，并在成功后把旧值写回用户空间。

### 3.2 do_timer_settime

```c
// kernel/time/posix-timers.c:927
static int do_timer_settime(timer_t timer_id, int tmr_flags,
                            struct itimerspec64 *new_spec64,
                            struct itimerspec64 *old_spec64)
{
    if (!timespec64_valid(&new_spec64->it_interval) ||
        !timespec64_valid(&new_spec64->it_value))
        return -EINVAL;

    if (old_spec64)
        memset(old_spec64, 0, sizeof(*old_spec64));

    for (; ; old_spec64 = NULL) {
        struct k_itimer *timr;

        scoped_timer_get_or_fail(timer_id) {
            timr = scoped_timer;

            if (old_spec64)
                old_spec64->it_interval =
                    ktime_to_timespec64(timr->it_interval);

            /* Prevent signal delivery and rearming. */
            timr->it_signal_seq++;

            int ret = timr->kclock->timer_set(timr, tmr_flags,
                                              new_spec64, old_spec64);
            if (ret != TIMER_RETRY)
                return ret;

            /* Protect the timer from being freed when leaving the lock scope */
            rcu_read_lock();
        }
        timer_wait_running(timr);
        rcu_read_unlock();
    }
}
```

`TIMER_RETRY` 是这里的关键：如果定时器到期回调正在另一个 CPU 上执行并持有 `it_lock`，直接自旋会活锁；内核先释放锁、`timer_wait_running()` 等待回调结束，再重试。

### 3.3 common_timer_set

```c
// kernel/time/posix-timers.c:887（节选）
int common_timer_set(struct k_itimer *timr, int flags,
                     struct itimerspec64 *new_setting,
                     struct itimerspec64 *old_setting)
{
    const struct k_clock *kc = timr->kclock;
    bool sigev_none;
    ktime_t expires;

    if (old_setting)
        common_timer_get(timr, old_setting);

    if (kc->timer_try_to_cancel(timr) < 0)
        return TIMER_RETRY;

    timr->it_status = POSIX_TIMER_DISARMED;
    posix_timer_set_common(timr, new_setting);

    /* Keep timer disarmed when it_value is zero */
    if (!new_setting->it_value.tv_sec && !new_setting->it_value.tv_nsec)
        return 0;

    expires = timespec64_to_ktime(new_setting->it_value);
    if (flags & TIMER_ABSTIME)
        expires = timens_ktime_to_host(timr->it_clock, expires);
    sigev_none = timr->it_sigev_notify == SIGEV_NONE;

    if (kc->timer_arm(timr, expires, flags & TIMER_ABSTIME, sigev_none)) {
        if (!sigev_none)
            timr->it_status = POSIX_TIMER_ARMED;
    } else {
        /* Timer was already expired, queue the signal */
        posix_timer_queue_signal(timr);
    }
    return 0;
}
```

语义要点：

- `it_value == 0` 只修改 `it_interval` 并保持解除状态；
- `TIMER_ABSTIME` 下绝对时刻需经 `timens_ktime_to_host()` 从时间命名空间换算到宿主时间；
- `timer_arm()` 返回 `false` 表示目标时刻已过，立即排队一次信号（而不是等到下一个周期）。

### 3.4 周期定时器的重新武装

周期定时器到期后由 `posix_timer_fn()` 重新计算下一次到期时间并再次启动 hrtimer。overrun 计数（`it_overrun`）记录“信号未及时投递时又到期了多少次”；`posix_timer_set_common()` 在每次 `timer_settime` 时把它重置为 `-1` 并清零 `it_overrun_last`。

---

## 4. 调用链

```text
用户态 timer_settime(timerid, flags, new_value, old_value)
  │
  ▼
sys_timer_settime(timer_id, flags, new_setting, old_setting)
  ├─ get_itimerspec64(&new_spec, new_setting)
  └─ do_timer_settime()
       ├─ timespec64_valid(it_interval / it_value)
       ├─ scoped_timer_get_or_fail(timer_id)
       │    ├─ posix_timer_by_id()             // 哈希查找
       │    └─ lock_timer()                    // 取 it_lock
       └─ kc->timer_set(timr, flags, new, old)
            └─ common_timer_set()
                 ├─ common_timer_get()          // 旧值
                 ├─ kc->timer_try_to_cancel()   // 失败 → TIMER_RETRY
                 ├─ posix_timer_set_common()    // 更新 it_interval，清 overrun
                 ├─ it_value == 0 → 解除返回
                 ├─ TIMER_ABSTIME → timens_ktime_to_host()
                 └─ kc->timer_arm()
                      └─ hrtimer_start(&timr->it.real.timer, ...)
                           └─ enqueue_hrtimer() // 插入每 CPU 红黑树
```

---

## 5. 关键数据结构

```c
// include/uapi/linux/time_types.h:12
struct __kernel_itimerspec {
    struct __kernel_timespec it_interval;
    struct __kernel_timespec it_value;
};

// include/linux/time64.h:18
struct itimerspec64 {
    struct timespec64 it_interval;
    struct timespec64 it_value;
};
```

`struct k_itimer` 中与本调用相关的字段：

| 字段 | 作用 |
|------|------|
| `it_interval` | 周期（ktime 表示） |
| `it_status` | `POSIX_TIMER_DISARMED` / `ARMED` / `REQUEUE_PENDING` |
| `it_overrun` / `it_overrun_last` | 超限计数 |
| `it_signal_seq` | 阻止信号投递与重新武装竞争 |
| `it.real.timer` | 底层 hrtimer |

---

## 6. 错误码

| errno | 触发条件 |
|-------|----------|
| `EINVAL` | `new_value == NULL`；`tv_nsec` 越界或秒为负；`timerid` 无效 |
| `EFAULT` | `new_value` / `old_value` 指针不可访问 |

---

## 7. 使用示例

```c
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    timer_t tid;
    struct sigevent sev = { 0 };
    struct itimerspec its, old;
    sigset_t mask;

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    timer_create(CLOCK_MONOTONIC, &sev, &tid);

    /* 1 秒后首次到期，之后每 250 ms 一次 */
    its.it_value.tv_sec = 1;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 250 * 1000 * 1000;
    timer_settime(tid, 0, &its, NULL);

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);       /* 便于同步读取信号 */
    sigwait(&mask, NULL);

    /* 改为绝对时刻：CLOCK_MONOTONIC 当前时间 + 2 秒 */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    its.it_value = now;
    its.it_value.tv_sec += 2;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    timer_settime(tid, TIMER_ABSTIME, &its, &old);

    printf("old interval: %ld.%09ld\n",
           (long)old.it_interval.tv_sec, old.it_interval.tv_nsec);

    /* 解除定时器 */
    memset(&its, 0, sizeof(its));
    timer_settime(tid, 0, &its, NULL);

    timer_delete(tid);
    return 0;
}
```

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `kernel/time/posix-timers.c:962` | `SYSCALL_DEFINE4(timer_settime)` |
| `kernel/time/posix-timers.c:927` | `do_timer_settime()` |
| `kernel/time/posix-timers.c:887` | `common_timer_set()` |
| `kernel/time/posix-timers.c:874` | `posix_timer_set_common()` |
| `kernel/time/posix-timers.h` | `enum posix_timer_state`、`TIMER_RETRY` |
| `include/uapi/linux/time_types.h:12` | `struct __kernel_itimerspec` |

相关文档：[timer_create](timer_create.md)、[timer_gettime](timer_gettime.md)、[timer_delete](timer_delete.md)。
