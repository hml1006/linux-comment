# clock_adjtime 系统调用分析

## 1. 概述

`clock_adjtime()` 是 `adjtimex()` 的扩展版本，允许指定操作的时钟 ID（clockid）。它支持对特定时钟（如 CLOCK_REALTIME、CLOCK_TAI 或动态时钟）进行 NTP 调整，而不仅仅是对系统主时钟进行操作。

**原型：**

```c
SYSCALL_DEFINE2(clock_adjtime, const clockid_t, which_clock,
                struct __kernel_timex __user *, utx)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| which_clock | clockid_t | 目标时钟 ID |
| utx | timex* | 输入/输出参数，包含时钟调整参数 |

## 2. 使用场景

- NTP 精确时间同步：对特定时钟进行频率和偏移调整
- 硬件时钟调整：支持动态时钟的微调
- 读取时钟状态：获取特定时钟的精度、误差等信息
- POSIX 时钟调整接口

## 3. 函数调用链

```
clock_adjtime(which_clock, utx)                    // kernel/time/posix-timers.c:1157
  │
  ├─ copy_from_user(&ktx, utx, sizeof(ktx))
  │
  ├─ do_clock_adjtime(which_clock, &ktx)            // kernel/time/posix-timers.c:1145
  │    │
  │    ├─ kc = clockid_to_kclock(which_clock)       // 查找 k_clock
  │    ├─ 若 !kc → -EINVAL
  │    ├─ 若 !kc->clock_adj → -EOPNOTSUPP
  │    │
  │    └─ kc->clock_adj(which_clock, ktx)           // 时钟特定 adj 函数
  │         ├─ CLOCK_REALTIME → do_adjtimex()
  │         └─ 动态时钟 → pc_clock_adjtime()        // posix-clock.c
  │
  └─ copy_to_user(utx, &ktx, sizeof(ktx))           // 写回结果
```

## 4. 关键数据结构

```c
// 时钟多态接口（通过 k_clock 结构分发）
struct k_clock {
    int (*clock_getres)(const clockid_t, struct timespec64 *);
    int (*clock_set)(const clockid_t, const struct timespec64 *);
    int (*clock_get_timespec)(const clockid_t, struct timespec64 *);
    int (*clock_adj)(const clockid_t, struct __kernel_timex *);
    int (*timer_create)(struct k_itimer *);
    int (*nsleep)(const clockid_t, int, struct timespec64 *);
    // ...
};

// 支持的时钟
static const struct k_clock clock_realtime = {
    .clock_getres = posix_get_hrtimer_res,
    .clock_set = do_clock_settime,
    .clock_get_timespec = do_clock_gettime,
    .clock_adj = do_adjtimex,           // CLOCK_REALTIME 使用 adjtimex
    // ...
};

static const struct k_clock clock_monotonic = {
    .clock_getres = posix_get_hrtimer_res,
    .clock_get_timespec = do_clock_gettime,
    // clock_adj = NULL  → -EOPNOTSUPP
};
```

## 5. 流程图

```
用户态: clock_adjtime(CLOCK_REALTIME, &txc)
    │
    ▼
SYSCALL_DEFINE2(clock_adjtime)
    │
    ├─ 从用户空间拷贝 txc 结构
    │
    ├─ do_clock_adjtime(which_clock, &ktx)
    │   │
    │   ├─ clockid_to_kclock()  →  kc
    │   │
    │   ├─ kc->clock_adj == NULL ? → -EOPNOTSUPP
    │   │   (CLOCK_MONOTONIC 不可调整)
    │   │
    │   └─ kc->clock_adj(which_clock, ktx)
    │        │
    │        ├─ CLOCK_REALTIME → do_adjtimex()
    │        │   └─ __do_adjtimex() → ntp_adjtimex()
    │        │
    │        └─ 动态时钟 → pc_clock_adjtime()
    │            └─ cd.clk->ops.clock_adjtime()
    │
    └─ 写回 txc 结构到用户空间
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效时钟 | which_clock 不是有效的时钟 ID |
| EOPNOTSUPP | 不支持 | 该时钟不支持调整操作 |
| EPERM | 权限不足 | 非特权用户尝试设置参数 |
| EFAULT | 内存错误 | utx 指针指向不可访问地址 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    struct timex txc = {0};

    /* 读取 CLOCK_REALTIME 的时钟状态 */
    if (clock_adjtime(CLOCK_REALTIME, &txc) == -1) {
        perror("clock_adjtime");
        return 1;
    }

    printf("CLOCK_REALTIME status:\n");
    printf("  offset:    %ld us\n", txc.offset);
    printf("  freq:      %ld\n", txc.freq);
    printf("  maxerror:  %ld us\n", txc.maxerror);
    printf("  status:    %d\n", txc.status);

    /* CLOCK_MONOTONIC 不支持调整 */
    if (clock_adjtime(CLOCK_MONOTONIC, &txc) == -1)
        perror("clock_adjtime on MONOTONIC (expected)");

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#定时器与时间)
- kernel/time/posix-timers.c:`do_clock_adjtime()` - 核心实现
- kernel/time/posix-clock.c:`pc_clock_adjtime()` - 动态时钟调整