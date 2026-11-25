# clock_nanosleep 系统调用分析

## 1. 概述

`clock_nanosleep` 是高精度睡眠接口的 POSIX 版本。与 `nanosleep` 相比，它有两个关键增强：

- **可指定时钟**：`CLOCK_REALTIME`、`CLOCK_MONOTONIC`、`CLOCK_BOOTTIME`、进程/线程 CPU 时钟等；
- **支持绝对时间**：`flags` 传入 `TIMER_ABSTIME` 时，`rqtp` 表示“唤醒时刻”而不是“睡眠时长”，可避免周期性任务的误差累积。

**关键特性：**

- 被信号中断时**不写回剩余时间**（`rmtp` 在 `TIMER_ABSTIME` 模式下被忽略）
- 返回值直接是内核错误码的相反数，**不设置 errno**（与 `nanosleep` 不同）
- 内核实现复用 `hrtimer_nanosleep()`，与 `nanosleep` 共享同一条睡眠路径

**内核源码位置：** `kernel/time/posix-timers.c:1383`

---

## 2. 用户态接口

```c
#include <time.h>

int clock_nanosleep(clockid_t clockid, int flags,
                    const struct timespec *request,
                    struct timespec *remain);
```

| 参数 | 说明 |
|------|------|
| `clockid` | 计时所用的时钟 |
| `flags` | `0` 或 `TIMER_ABSTIME` |
| `request` | 睡眠时长（相对）或目标时刻（绝对） |
| `remain` | 剩余时间；仅相对睡眠且被信号中断时写入 |

### 2.1 支持的时钟

| 时钟 | 备注 |
|------|------|
| `CLOCK_REALTIME` | 受系统时间调整影响；绝对睡眠使用墙上时间 |
| `CLOCK_MONOTONIC` | 推荐用于测量与周期任务 |
| `CLOCK_BOOTTIME` | 包含系统挂起时间 |
| `CLOCK_PROCESS_CPUTIME_ID` | 进程 CPU 时间 |
| `CLOCK_THREAD_CPUTIME_ID` | 线程 CPU 时间 |
| `CLOCK_TAI` | 国际原子时 |

具体支持范围取决于 `k_clock` 是否实现了 `nsleep` 回调；未实现的时钟返回 `EOPNOTSUPP`。

### 2.2 返回值

与大多数系统调用不同，`clock_nanosleep` 成功返回 `0`，失败返回**正数错误码**（如 `EINVAL`、`EINTR`），调用者应直接判断返回值而不是读 `errno`。

---

## 3. 内核实现

### 3.1 系统调用入口

```c
// kernel/time/posix-timers.c:1383
SYSCALL_DEFINE4(clock_nanosleep, const clockid_t, which_clock, int, flags,
        const struct __kernel_timespec __user *, rqtp,
        struct __kernel_timespec __user *, rmtp)
{
    const struct k_clock *kc = clockid_to_kclock(which_clock);
    struct timespec64 t;

    if (!kc)
        return -EINVAL;
    if (!kc->nsleep)
        return -EOPNOTSUPP;

    if (get_timespec64(&t, rqtp))
        return -EFAULT;

    if (!timespec64_valid(&t))
        return -EINVAL;
    if (flags & TIMER_ABSTIME)
        rmtp = NULL;
    current->restart_block.fn = do_no_restart_syscall;
    current->restart_block.nanosleep.type = rmtp ? TT_NATIVE : TT_NONE;
    current->restart_block.nanosleep.rmtp = rmtp;

    return kc->nsleep(which_clock, flags, &t);
}
```

入口先校验时钟与时间值，再根据 `TIMER_ABSTIME` 决定是否保留 `rmtp`，最后交给时钟私有的 `nsleep` 回调。

### 3.2 通用睡眠回调

```c
// kernel/time/posix-timers.c:1355
static int common_nsleep(const clockid_t which_clock, int flags,
                         const struct timespec64 *rqtp)
{
    ktime_t texp = timespec64_to_ktime(*rqtp);

    return hrtimer_nanosleep(texp, flags & TIMER_ABSTIME ?
                             HRTIMER_MODE_ABS : HRTIMER_MODE_REL,
                             which_clock);
}
```

对 `CLOCK_MONOTONIC` 和 `CLOCK_BOOTTIME`，绝对值还要经过时间命名空间转换：

```c
// kernel/time/posix-timers.c:1367
static int common_nsleep_timens(const clockid_t which_clock, int flags,
                                const struct timespec64 *rqtp)
{
    ktime_t texp = timespec64_to_ktime(*rqtp);

    if (flags & TIMER_ABSTIME)
        texp = timens_ktime_to_host(which_clock, texp);

    return hrtimer_nanosleep(texp, flags & TIMER_ABSTIME ?
                             HRTIMER_MODE_ABS : HRTIMER_MODE_REL,
                             which_clock);
}
```

即：容器内进程传入的绝对时刻是“命名空间时间”，进入 hrtimer 前必须换算成宿主时间。

### 3.3 与 nanosleep 的实现差异

| 维度 | nanosleep | clock_nanosleep |
|------|-----------|-----------------|
| 时钟 | 固定 `CLOCK_MONOTONIC` | 由 `clockid` 指定 |
| 时间语义 | 仅相对 | 相对或绝对（`TIMER_ABSTIME`） |
| `restart_block` | 相对睡眠时设置 | 绝对睡眠强制清空 `rmtp` |
| 返回值 | `-1` + errno | 直接返回错误码 |
| 内核函数 | `hrtimer_nanosleep` | `kc->nsleep` → `common_nsleep` → `hrtimer_nanosleep` |

---

## 4. 调用链

```text
用户态 clock_nanosleep(clockid, flags, rqtp, rmtp)
  │
  ▼
sys_clock_nanosleep
  ├─ clockid_to_kclock(clockid)
  │    ├─ 非法 ID → -EINVAL
  │    └─ kc->nsleep == NULL → -EOPNOTSUPP
  ├─ get_timespec64(&t, rqtp)
  ├─ timespec64_valid(&t)
  ├─ TIMER_ABSTIME → rmtp = NULL
  ├─ 设置 restart_block.nanosleep
  └─ kc->nsleep(clockid, flags, &t)
       ├─ CLOCK_MONOTONIC/BOOTTIME → common_nsleep_timens
       │    └─ TIMER_ABSTIME → timens_ktime_to_host()
       └─ hrtimer_nanosleep(expires, mode, clockid)
            ├─ hrtimer_setup_sleeper_on_stack()
            ├─ hrtimer_set_expires_range_ns(expires, timer_slack_ns)
            └─ do_nanosleep()
                 ├─ set_current_state(TASK_INTERRUPTIBLE)
                 ├─ hrtimer_sleeper_start_expires()
                 ├─ schedule()
                 ├─ 到期 → hrtimer_wakeup → try_to_wake_up
                 └─ 被信号中断 → 相对模式写回 rem / 绝对模式直接返回 -EINTR
```

---

## 5. 关键数据结构

```c
// include/uapi/linux/time_types.h
struct __kernel_timespec {
    __kernel_time64_t tv_sec;
    long long         tv_nsec;
};

// include/linux/time64.h
struct timespec64 {
    time64_t tv_sec;
    long     tv_nsec;
};
```

```c
// include/linux/restart_block.h（节选）
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

`struct k_clock` 的 `nsleep` 回调负责把时钟语义翻译为 hrtimer 模式：

```c
// kernel/time/posix-timers.h:22
int (*nsleep)(const clockid_t which_clock, int flags,
              const struct timespec64 *);
```

---

## 6. 错误码

| 返回值 | 触发条件 |
|--------|----------|
| `EINVAL` | `clockid` 非法，或 `tv_nsec` 越界 / `tv_sec` 为负 |
| `EFAULT` | `rqtp` / `rmtp` 指针不可访问 |
| `EOPNOTSUPP` | 该时钟没有实现 `nsleep`（如 CPU 时钟） |
| `EINTR` | 被信号中断 |

再次强调：这些错误码是**函数返回值**，不会写入 `errno`。

---

## 7. 使用示例

### 7.1 相对睡眠（可重试）

```c
#include <stdio.h>
#include <time.h>

int main(void)
{
    struct timespec req = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
    struct timespec rem;
    int ret;

    while ((ret = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &rem)) == EINTR)
        req = rem;                 /* 用剩余时间继续 */

    if (ret) {
        fprintf(stderr, "clock_nanosleep: %d\n", ret);
        return 1;
    }
    return 0;
}
```

### 7.2 绝对时间周期任务（无累积误差）

```c
#include <time.h>

void periodic_task(void)
{
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    for (int i = 0; i < 100; i++) {
        next.tv_nsec += 10 * 1000 * 1000;      /* 10 ms 周期 */
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        /* 绝对睡眠：即使本次任务超时，下一次仍对齐到固定节拍 */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        /* ... 周期任务主体 ... */
    }
}
```

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `kernel/time/posix-timers.c:1383` | `SYSCALL_DEFINE4(clock_nanosleep)` |
| `kernel/time/posix-timers.c:1355` | `common_nsleep()` |
| `kernel/time/posix-timers.c:1367` | `common_nsleep_timens()` |
| `kernel/time/hrtimer.c:2437` | `hrtimer_nanosleep()` |
| `kernel/time/hrtimer.c:2390` | `do_nanosleep()` |
| `kernel/time/posix-timers.h` | `struct k_clock`、`nsleep` 回调 |
| `include/linux/restart_block.h` | `restart_block.nanosleep` |

### 8.1 与其他时间接口的关系

| 操作 | 核心函数 | 到期通知 |
|------|----------|----------|
| `timer_create/settime` | `hrtimer_start` | 信号（`SIGEV_SIGNAL` / `SIGEV_THREAD_ID`） |
| `timerfd_create/settime` | `hrtimer_start` | 文件描述符可读 + epoll |
| `nanosleep` | `hrtimer_nanosleep` | 唤醒当前进程 |
| `clock_nanosleep` | `hrtimer_nanosleep` | 唤醒当前进程（支持绝对时刻） |
| `clock_gettime` | `ktime_get_ts64` / vDSO | 同步返回 |

相关文档：[nanosleep](nanosleep.md)、[clock_gettime](clock_gettime.md)、[timer_create](timer_create.md)、[timerfd_create](timerfd_create.md)。
