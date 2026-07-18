# clock_getres 系统调用分析

## 1. 概述

`clock_getres` 用于获取指定时钟的分辨率（精度）。它返回时钟的精度值，即该时钟定时器的最小时间增量。该信息对于需要了解时钟精确度的应用程序非常有用，例如需要根据系统时钟精度调整超时时间的场景。

**原型：**

```c
#include <time.h>

int clock_getres(clockid_t clockid, struct timespec *res);
```

**内核入口：**

```c
// kernel/time/posix-timers.c:1247
SYSCALL_DEFINE2(clock_getres, const clockid_t, which_clock,
                struct __kernel_timespec __user *, tp)
```

## 2. 使用场景

- **查询时钟精度**：确定特定时钟能够提供的最小时间精度
- **调整超时参数**：根据时钟精度设置合理的超时时间，避免因精度不足导致超时不准确
- **跨平台兼容**：检查系统是否支持特定的时钟类型
- **性能优化**：了解硬件时钟精度，优化时间相关操作的轮询间隔

## 3. 函数调用栈

```
clock_getres(which_clock, tp)                    // 系统调用入口
  └─ clockid_to_kclock(which_clock)              // 将 clockid 转换为 k_clock 指针
  └─ kc->clock_getres(which_clock, &rtn_tp)      // 调用时钟特定的 getres 回调
  │    ├─ [CLOCK_REALTIME] → posix_getres_realtime()  // 返回 hrtimer_resolution
  │    ├─ [CLOCK_MONOTONIC] → posix_getres_monotonic() // 返回 hrtimer_resolution
  │    ├─ [CLOCK_BOOTTIME] → posix_getres_boottime()   // 返回 hrtimer_resolution
  │    ├─ [CLOCK_TAI] → posix_getres_tai()              // 返回 hrtimer_resolution
  │    └─ [CLOCK_REALTIME_ALARM/CLOCK_BOOTTIME_ALARM] → alarm_clock_getres()
  └─ put_timespec64(&rtn_tp, tp)                 // 将结果复制到用户空间
```

**Stub 实现（当 CONFIG_POSIX_TIMERS=n 时）：**

```c
// kernel/time/posix-stubs.c:75
SYSCALL_DEFINE2(clock_getres, const clockid_t, which_clock,
                struct __kernel_timespec __user *, tp)
{
    struct timespec64 rtn_tp = {
        .tv_sec = 0,
        .tv_nsec = hrtimer_resolution,
    };

    switch (which_clock) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_BOOTTIME:
        if (put_timespec64(&rtn_tp, tp))
            return -EFAULT;
        return 0;
    default:
        return -EINVAL;
    }
}
```

## 4. 关键数据结构

### 4.1 struct k_clock（时钟操作函数集）

```c
// kernel/time/posix-timers.h:10
struct k_clock {
    int (*clock_getres)(const clockid_t which_clock,
                        struct timespec64 *tp);
    int (*clock_set)(const clockid_t which_clock,
                     const struct timespec64 *tp);
    int (*clock_get_timespec)(const clockid_t which_clock,
                              struct timespec64 *tp);
    ktime_t (*clock_get_ktime)(const clockid_t which_clock);
    int (*clock_adj)(const clockid_t which_clock, struct __kernel_timex *tx);
    int (*timer_create)(struct k_itimer *timer);
    int (*nsleep)(const clockid_t which_clock, int flags,
                  const struct timespec64 *);
    int (*timer_set)(struct k_itimer *timr, int flags,
                     struct itimerspec64 *new_setting,
                     struct itimerspec64 *old_setting);
    int (*timer_del)(struct k_itimer *timr);
    void (*timer_get)(struct k_itimer *timr,
                      struct itimerspec64 *cur_setting);
    void (*timer_rearm)(struct k_itimer *timr);
};
```

### 4.2 struct __kernel_timespec（用户空间时间结构）

```c
// include/uapi/linux/time_types.h:7
struct __kernel_timespec {
    __kernel_time64_t tv_sec;   /* 秒 */
    long long tv_nsec;          /* 纳秒 */
};
```

### 4.3 struct timespec64（内核空间时间结构）

```c
// include/linux/time64.h
struct timespec64 {
    time64_t    tv_sec;         /* 秒 */
    long        tv_nsec;        /* 纳秒 */
};
```

### 4.4 时钟 ID 常量

```c
// include/uapi/linux/time.h
#define CLOCK_REALTIME              0   // 系统实时时钟
#define CLOCK_MONOTONIC             1   // 单调递增时钟（不受系统时间调整影响）
#define CLOCK_PROCESS_CPUTIME_ID    2   // 进程 CPU 时间
#define CLOCK_THREAD_CPUTIME_ID     3   // 线程 CPU 时间
#define CLOCK_MONOTONIC_RAW         4   // 原始单调时钟（不受 NTP 调整影响）
#define CLOCK_REALTIME_COARSE       5   // 粗粒度实时时钟（快速读取）
#define CLOCK_MONOTONIC_COARSE      6   // 粗粒度单调时钟（快速读取）
#define CLOCK_BOOTTIME              7   // 启动时间（包括休眠时间）
#define CLOCK_REALTIME_ALARM        8   // 支持告警的实时时钟（需 CAP_WAKE_ALARM）
#define CLOCK_BOOTTIME_ALARM        9   // 支持告警的启动时钟（需 CAP_WAKE_ALARM）
#define CLOCK_TAI                   11  // TAI（国际原子时）时钟
```

## 5. 流程图

```
用户态调用 clock_getres(clockid, &res)
    │
    ▼
syscall 入口 (SYSCALL_DEFINE2)
    │
    ├─ clockid_to_kclock(which_clock)
    │   │
    │   ├─ 有效 clockid → 返回对应 k_clock 指针
    │   └─ 无效 clockid → 返回 NULL → 返回 -EINVAL
    │
    ▼
kc->clock_getres(which_clock, &rtn_tp)
    │
    ├─ 标准时钟 (REALTIME/MONOTONIC/BOOTTIME/TAI)
    │   └─ rtn_tp.tv_sec = 0
    │   └─ rtn_tp.tv_nsec = hrtimer_resolution (通常为 1ns)
    │
    ├─ 粗粒度时钟 (REALTIME_COARSE/MONOTONIC_COARSE)
    │   └─ rtn_tp.tv_sec = 0
    │   └─ rtn_tp.tv_nsec = TICK_NSEC (通常为 1ms 或 4ms)
    │
    ├─ 告警时钟 (REALTIME_ALARM/BOOTTIME_ALARM)
    │   └─ rtn_tp.tv_sec = 0
    │   └─ rtn_tp.tv_nsec = hrtimer_resolution
    │
    ├─ CPU 时间时钟 (PROCESS_CPUTIME_ID/THREAD_CPUTIME_ID)
    │   └─ 取决于硬件 PMU 精度
    │
    └─ 动态时钟 (通过 fd 派生的时钟)
        └─ 取决于具体实现
    │
    ▼
put_timespec64(&rtn_tp, tp)     // 复制到用户空间
    │
    ├─ 成功 → 返回 0
    └─ 失败 → 返回 -EFAULT
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EINVAL` | 无效参数 | `which_clock` 不是有效的时钟 ID |
| `EFAULT` | 地址错误 | `tp` 指向无效的用户空间地址 |
| `ENOSYS` | 不支持 | 时钟 ID 对应的时钟未实现 `clock_getres` |

## 7. 使用示例

```c
#include <stdio.h>
#include <time.h>
#include <string.h>

int main(void)
{
    struct timespec res;
    clockid_t clocks[] = {
        CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME,
        CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE,
        CLOCK_MONOTONIC_RAW, CLOCK_TAI
    };
    const char *names[] = {
        "CLOCK_REALTIME", "CLOCK_MONOTONIC", "CLOCK_BOOTTIME",
        "CLOCK_REALTIME_COARSE", "CLOCK_MONOTONIC_COARSE",
        "CLOCK_MONOTONIC_RAW", "CLOCK_TAI"
    };
    int n = sizeof(clocks) / sizeof(clocks[0]);

    for (int i = 0; i < n; i++) {
        if (clock_getres(clocks[i], &res) == 0) {
            printf("%-25s: %ld sec, %ld nsec (%.2f nsec)\n",
                   names[i], res.tv_sec, res.tv_nsec,
                   res.tv_sec * 1e9 + res.tv_nsec);
        } else {
            printf("%-25s: %s\n", names[i], strerror(errno));
        }
    }
    return 0;
}
```

**可能的输出：**

```
CLOCK_REALTIME            : 0 sec, 1 nsec (1.00 nsec)
CLOCK_MONOTONIC           : 0 sec, 1 nsec (1.00 nsec)
CLOCK_BOOTTIME            : 0 sec, 1 nsec (1.00 nsec)
CLOCK_REALTIME_COARSE     : 0 sec, 1000000 nsec (1000000.00 nsec)
CLOCK_MONOTONIC_COARSE    : 0 sec, 1000000 nsec (1000000.00 nsec)
CLOCK_MONOTONIC_RAW       : 0 sec, 1 nsec (1.00 nsec)
CLOCK_TAI                 : 0 sec, 1 nsec (1.00 nsec)
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#定时器与时间)
- 内核源码：`kernel/time/posix-timers.c`
- 内核源码：`kernel/time/posix-stubs.c`
- 时钟定义：`include/uapi/linux/time.h`
- 时间结构：`include/uapi/linux/time_types.h`