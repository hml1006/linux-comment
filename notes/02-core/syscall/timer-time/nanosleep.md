# nanosleep 系统调用分析

## 1. 概述

`nanosleep` 让当前进程休眠指定的一段时间，精度为纳秒。它是 POSIX 的高精度睡眠接口，也是用户态实现 `usleep`/`sleep` 的底层基础。

**关键特性：**

- 使用相对时间（不是绝对时间），以 `CLOCK_MONOTONIC` 计时
- 休眠期间进程处于 `TASK_INTERRUPTIBLE`，可被信号唤醒
- 被信号中断时返回 `-1` 并置 `errno = EINTR`，同时把剩余时间写回 `rem`
- 内核使用 hrtimer 实现，不依赖低精度 timer wheel
- 输入时间会按 `timer_slack_ns` 做范围化，允许一定的合并唤醒

**内核源码位置：** `kernel/time/hrtimer.c:2466`，核心实现 `hrtimer_nanosleep()` 在 `kernel/time/hrtimer.c:2437`

---

## 2. 用户态接口

```c
#include <time.h>

int nanosleep(const struct timespec *req, struct timespec *rem);
```

| 参数 | 说明 |
|------|------|
| `req` | 请求休眠的时长（秒 + 纳秒），必须合法 |
| `rem` | 输出参数，被信号中断时写入剩余时间，可为 `NULL` |

### 返回值

| 返回值 | errno | 说明 |
|--------|-------|------|
| `0` | — | 休眠完整结束 |
| `-1` | `EINTR` | 被信号中断，剩余时间写入 `rem` |
| `-1` | `EINVAL` | `tv_nsec` 不在 `[0, 999999999]`，或秒为负 |
| `-1` | `EFAULT` | 指针不可读写 |

---

## 3. 内核实现

### 3.1 系统调用入口

```c
// kernel/time/hrtimer.c:2466
SYSCALL_DEFINE2(nanosleep, struct __kernel_timespec __user *, rqtp,
        struct __kernel_timespec __user *, rmtp)
{
    struct timespec64 tu;

    if (get_timespec64(&tu, rqtp))
        return -EFAULT;

    if (!timespec64_valid(&tu))
        return -EINVAL;

    current->restart_block.fn = do_no_restart_syscall;
    current->restart_block.nanosleep.type = rmtp ? TT_NATIVE : TT_NONE;
    current->restart_block.nanosleep.rmtp = rmtp;
    return hrtimer_nanosleep(timespec64_to_ktime(tu), HRTIMER_MODE_REL, CLOCK_MONOTONIC);
}
```

入口完成三件事：校验用户时间、把 `rem` 指针登记到 `restart_block`（供信号处理和自动重启使用）、以相对模式调用 `hrtimer_nanosleep()`。

### 3.2 hrtimer_nanosleep

```c
// kernel/time/hrtimer.c:2437
long hrtimer_nanosleep(ktime_t rqtp, const enum hrtimer_mode mode, const clockid_t clockid)
{
    struct restart_block *restart;
    struct hrtimer_sleeper t;
    int ret;

    hrtimer_setup_sleeper_on_stack(&t, clockid, mode);
    hrtimer_set_expires_range_ns(&t.timer, rqtp, current->timer_slack_ns);
    ret = do_nanosleep(&t, mode);
    if (ret != -ERESTART_RESTARTBLOCK)
        goto out;

    /* Absolute timers do not update the rmtp value and restart: */
    if (mode == HRTIMER_MODE_ABS) {
        ret = -ERESTARTNOHAND;
        goto out;
    }

    restart = &current->restart_block;
    restart->nanosleep.clockid = t.timer.base->clockid;
    restart->nanosleep.expires = hrtimer_get_expires(&t.timer);
    set_restart_fn(restart, hrtimer_nanosleep_restart);
out:
    destroy_hrtimer_on_stack(&t.timer);
    return ret;
}
```

要点：

- `hrtimer_set_expires_range_ns()` 把到期时间放在 `[rqtp, rqtp + slack]` 范围内，允许内核把临近的唤醒合并，降低功耗；
- 定时器与 `hrtimer_sleeper` 位于内核栈上，无需动态分配；
- 只有**相对时间**睡眠会设置 `restart_block`。如果是绝对时间（`clock_nanosleep` 的 `TIMER_ABSTIME`），被信号中断后直接返回，不自动重启。

### 3.3 do_nanosleep

```c
// kernel/time/hrtimer.c:2390
static int __sched do_nanosleep(struct hrtimer_sleeper *t, enum hrtimer_mode mode)
{
    struct restart_block *restart;

    do {
        set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
        hrtimer_sleeper_start_expires(t, mode);

        if (likely(t->task))
            schedule();

        hrtimer_cancel(&t->timer);
        mode = HRTIMER_MODE_ABS;

    } while (t->task && !signal_pending(current));

    __set_current_state(TASK_RUNNING);

    if (!t->task)
        return 0;

    restart = &current->restart_block;
    if (restart->nanosleep.type != TT_NONE) {
        ktime_t rem = hrtimer_expires_remaining(&t->timer);
        struct timespec64 rmt;

        if (rem <= 0)
            return 0;
        rmt = ktime_to_timespec64(rem);

        return nanosleep_copyout(restart, &rmt);
    }
    return -ERESTART_RESTARTBLOCK;
}
```

循环处理“虚假唤醒”：只要还有信号待处理就退出循环；定时器未到期时把剩余时间写回用户空间，返回 `-EINTR`。若用户没传 `rem`，则返回 `-ERESTART_RESTARTBLOCK`，由信号返回路径决定是否自动重启。

---

## 4. 调用链

```text
用户态 nanosleep(req, rem)
  │
  ▼
sys_nanosleep(rqtp, rmtp)
  ├─ get_timespec64(&tu, rqtp)
  ├─ timespec64_valid(&tu)                  // 0 <= nsec < 1e9
  ├─ restart_block.nanosleep = {rmtp}
  └─ hrtimer_nanosleep(ktime, REL, CLOCK_MONOTONIC)
       ├─ hrtimer_setup_sleeper_on_stack()
       ├─ hrtimer_set_expires_range_ns(rqtp, timer_slack_ns)
       └─ do_nanosleep()
            ├─ set_current_state(TASK_INTERRUPTIBLE)
            ├─ hrtimer_sleeper_start_expires()
            │    └─ hrtimer_start_range_ns() → 加入 hrtimer 红黑树
            ├─ schedule()                   // 让出 CPU
            │    └─ 到期：hrtimer_wakeup → try_to_wake_up
            ├─ hrtimer_cancel()
            └─ 若被信号中断 → nanosleep_copyout(rem) → -EINTR
```

---

## 5. 关键数据结构

```c
// include/linux/hrtimer.h（节选）
struct hrtimer_sleeper {
    struct hrtimer timer;
    struct task_struct *task;      // 到期后唤醒的任务
};

struct restart_block {
    long (*fn)(struct restart_block *);
    union {
        struct {
            clockid_t clockid;
            enum timespec_type type;
            union {
                struct __kernel_timespec __user *rmtp;
                struct old_timespec32 __user *compat_rmtp;
            };
            u64 expires;
        } nanosleep;
        /* ... */
    };
};
```

```c
// include/uapi/linux/time_types.h
struct __kernel_timespec {
    __kernel_time64_t tv_sec;
    long long         tv_nsec;
};
```

---

## 6. 错误码

| errno | 触发条件 |
|-------|----------|
| `EINVAL` | `tv_nsec` 越界或 `tv_sec` 为负 |
| `EFAULT` | `req` / `rem` 指针不可访问 |
| `EINTR` | 被信号中断（不是错误，调用者可重试） |

---

## 7. 使用示例

```c
#include <errno.h>
#include <stdio.h>
#include <time.h>

int main(void)
{
    struct timespec req = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 };
    struct timespec rem;

    while (nanosleep(&req, &rem) == -1) {
        if (errno != EINTR) {
            perror("nanosleep");
            return 1;
        }
        /* 被信号打断：用剩余时间继续睡 */
        req = rem;
        printf("interrupted, remaining %ld.%09ld\n",
               (long)req.tv_sec, req.tv_nsec);
    }
    puts("done");
    return 0;
}
```

### 7.1 子毫秒睡眠的注意事项

- `nanosleep` 的实际睡眠时间通常比请求值长，取决于调度延迟与 hrtimer 精度；
- 需要严格周期时，应使用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` 按绝对时间唤醒，避免误差累积。

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `kernel/time/hrtimer.c:2466` | `SYSCALL_DEFINE2(nanosleep)` |
| `kernel/time/hrtimer.c:2437` | `hrtimer_nanosleep()` |
| `kernel/time/hrtimer.c:2390` | `do_nanosleep()` |
| `kernel/time/hrtimer.c:2425` | `hrtimer_nanosleep_restart()` |
| `include/linux/hrtimer.h` | `struct hrtimer_sleeper` |
| `include/linux/restart_block.h` | `struct restart_block` |

相关文档：[clock_nanosleep](clock_nanosleep.md)、[clock_gettime](clock_gettime.md)、[timer_create](timer_create.md)。
