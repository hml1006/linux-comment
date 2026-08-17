# clock_settime 系统调用分析

## 1. 概述

`clock_settime` 用于设置指定时钟的时间值。它允许具有适当权限的进程修改系统时间（对于 `CLOCK_REALTIME`）或特定时钟的时间值。该接口是 POSIX 时钟操作的核心部分，取代了旧的 `settimeofday` 系统调用。

**原型：**

```c
#include <time.h>

int clock_settime(clockid_t clockid, const struct timespec *tp);
```

**内核入口：**

```c
// kernel/time/posix-timers.c:1108
SYSCALL_DEFINE2(clock_settime, const clockid_t, which_clock,
                const struct __kernel_timespec __user *, tp)
```

## 2. 使用场景

- **设置系统时间**：使用 `CLOCK_REALTIME` 设置系统实时时钟（需 `CAP_SYS_TIME` 或 `CAP_SYS_ADMIN`）
- **时间同步服务**：NTP 和 PTP 守护进程使用此接口校准系统时间
- **容器/命名空间**：时间命名空间内的时钟设置
- **测试与模拟**：在测试环境中控制时间流逝

## 3. 函数调用栈

```
clock_settime(which_clock, tp)                    // 系统调用入口
  └─ clockid_to_kclock(which_clock)              // 将 clockid 转换为 k_clock 指针
  │    └─ 无效 clockid → 返回 -EINVAL
  │    └─ 无 clock_set 回调 → 返回 -EINVAL
  └─ get_timespec64(&new_tp, tp)                 // 从用户空间复制时间值
  │    └─ 复制失败 → 返回 -EFAULT
  └─ kc->clock_set(which_clock, &new_tp)         // 调用时钟特定的 setter
       │
       ├─ [CLOCK_REALTIME] → do_sys_settimeofday64(&new_tp, NULL)
       │    ├─ security_settime64()               // LSM 安全审计
       │    ├─ timespec64_valid_settod()           // 验证时间值有效性
       │    └─ do_settimeofday64()                 // 真正设置系统时间
       │         └─ timekeeping_set_time() / timekeeping_inject_offset()
       │
       ├─ [CLOCK_TAI] → (仅可读，不可设置)
       ├─ [CLOCK_MONOTONIC] → (仅可读，不可设置)
       └─ [CLOCK_REALTIME_ALARM] → (仅可读，不可设置)
```

**Stub 实现（当 CONFIG_POSIX_TIMERS=n 时）：**

```c
// kernel/time/posix-stubs.c:26
SYSCALL_DEFINE2(clock_settime, const clockid_t, which_clock,
                const struct __kernel_timespec __user *, tp)
{
    struct timespec64 new_tp;

    if (which_clock != CLOCK_REALTIME)
        return -EINVAL;
    if (get_timespec64(&new_tp, tp))
        return -EFAULT;

    return do_sys_settimeofday64(&new_tp, NULL);
}
```

## 4. 关键数据结构

### 4.1 struct k_clock（时钟操作函数集）

```c
// kernel/time/posix-timers.h:10
struct k_clock {
    // ...
    int (*clock_set)(const clockid_t which_clock,
                     const struct timespec64 *tp);
    // ...
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

## 5. 流程图

```
用户态调用 clock_settime(clockid, &tp)
    │
    ▼
syscall 入口 (SYSCALL_DEFINE2)
    │
    ├─ clockid_to_kclock(which_clock)
    │   ├─ 无效 clockid → 返回 -EINVAL
    │   └─ 有效 clockid → 返回 k_clock 指针
    │
    ├─ kc->clock_set 为 NULL → 返回 -EINVAL
    │
    ├─ get_timespec64(&new_tp, tp)
    │   ├─ 复制成功 → 继续
    │   └─ 复制失败 → 返回 -EFAULT
    │
    ▼
kc->clock_set(which_clock, &new_tp)
    │
    ├─ 仅 CLOCK_REALTIME 可设置
    │   │
    │   ├─ do_sys_settimeofday64(&new_tp, NULL)
    │   │   │
    │   │   ├─ timespec64_valid_settod(tv)  // 验证时间范围
    │   │   │   └─ 无效 → 返回 -EINVAL
    │   │   │
    │   │   ├─ security_settime64(tv, tz)   // LSM 安全审计
    │   │   │   └─ 拒绝 → 返回 -EPERM
    │   │   │
    │   │   └─ do_settimeofday64(tv)        // 设置时间
    │   │       └─ timekeeping_set_time()    // 更新 timekeeping 子系统
    │   │
    │   └─ 返回 0
    │
    └─ 其他时钟 (CLOCK_MONOTONIC, CLOCK_TAI 等)
        └─ 返回 -EINVAL（只读时钟）
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EINVAL` | 无效参数 | `clockid` 无效，或该时钟不支持设置操作，或 `tp` 中的时间值无效 |
| `EFAULT` | 地址错误 | `tp` 指向无效的用户空间地址 |
| `EPERM` | 权限不足 | 调用者没有 `CAP_SYS_TIME` 或 `CAP_SYS_ADMIN` 权限 |
| `EACCES` | 访问被拒绝 | LSM（如 SELinux）安全策略阻止操作 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    struct timespec ts;

    // 获取当前时间
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    printf("当前时间: %ld.%09ld\n", ts.tv_sec, ts.tv_nsec);

    // 将时间向前调整 1 小时
    ts.tv_sec += 3600;

    if (clock_settime(CLOCK_REALTIME, &ts) == -1) {
        printf("设置时间失败: %s (需要 CAP_SYS_TIME 权限)\n",
               strerror(errno));
        exit(EXIT_FAILURE);
    }

    printf("时间已调整 (+1 小时): %ld.%09ld\n", ts.tv_sec, ts.tv_nsec);
    return 0;
}
```

**使用 clock_settime 实现时间同步的简化 NTP 客户端模式：**

```c
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/*
 * 从 NTP 服务器获取的时间偏移（秒）
 * 实际场景中通过 NTP 协议获取
 */
int sync_system_time(time_t ntp_sec, long ntp_nsec)
{
    struct timespec ts;

    // 获取当前时间
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
        return -1;

    // 计算偏移并调整
    ts.tv_sec = ntp_sec;
    ts.tv_nsec = ntp_nsec;

    if (clock_settime(CLOCK_REALTIME, &ts) == -1)
        return -1;

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#定时器与时间)
- 内核源码：`kernel/time/posix-timers.c`
- 内核源码：`kernel/time/posix-stubs.c`
- 内核源码：`kernel/time/time.c`（`do_sys_settimeofday64` 实现）
- 时间结构：`include/uapi/linux/time_types.h`