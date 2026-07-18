# settimeofday 系统调用分析

## 1. 概述

`settimeofday` 是一个传统的系统调用，用于设置系统时间（实时时钟）和时区信息。它源自 BSD 系统，在现代 Linux 系统中已被 `clock_settime` 取代，但仍保留向前兼容性。该调用可以单独设置时间或时区，或者同时设置两者。

**注意**：`settimeofday` 仅影响 `CLOCK_REALTIME`，不影响其他时钟（如 `CLOCK_MONOTONIC`）。

**原型：**

```c
#include <sys/time.h>

int settimeofday(const struct timeval *tv, const struct timezone *tz);
```

**内核入口：**

```c
// kernel/time/time.c:199
SYSCALL_DEFINE2(settimeofday, struct __kernel_old_timeval __user *, tv,
                struct timezone __user *, tz)
```

## 2. 使用场景

- **设置系统时间**：系统启动时初始化实时时钟
- **时区配置**：设置系统时区（通过 `tz` 参数，功能有限）
- **NTP 时间同步**：传统 NTP 实现使用此接口（现代实现使用 `clock_adjtime`）
- **向后兼容**：为旧版应用程序提供兼容性支持

## 3. 函数调用栈

```
settimeofday(tv, tz)                                // 系统调用入口
  │
  ├─ 处理 tv 参数（时间值）
  │   ├─ tv != NULL:
  │   │   ├─ get_user(new_ts.tv_sec, &tv->tv_sec)   // 读取秒
  │   │   ├─ get_user(new_ts.tv_nsec, &tv->tv_usec)  // 读取微秒
  │   │   ├─ 验证: new_ts.tv_nsec > USEC_PER_SEC → 返回 -EINVAL
  │   │   └─ new_ts.tv_nsec *= NSEC_PER_USEC        // 微秒→纳秒转换
  │   │
  │   └─ tv == NULL: 保持 new_ts 未初始化（不设置时间）
  │
  ├─ 处理 tz 参数（时区）
  │   ├─ tz != NULL:
  │   │   └─ copy_from_user(&new_tz, tz, sizeof(*tz))  // 读取时区
  │   │
  │   └─ tz == NULL: 不设置时区
  │
  └─ do_sys_settimeofday64(tv ? &new_ts : NULL, tz ? &new_tz : NULL)
       │
       ├─ [tv != NULL]
       │   ├─ timespec64_valid_settod(tv)              // 验证时间范围
       │   ├─ security_settime64(tv, tz)               // LSM 安全审计
       │   └─ do_settimeofday64(tv)                    // 设置时间
       │
       └─ [tz != NULL]
           ├─ 验证: tz_minuteswest 范围 ±15*60 分钟
           ├─ sys_tz = *tz                             // 更新系统时区
           └─ update_vsyscall_tz()                     // 更新 vDSO 时区
```

## 4. 关键数据结构

### 4.1 struct __kernel_old_timeval（用户空间时间值）

```c
// include/uapi/linux/time.h
struct __kernel_old_timeval {
    __kernel_long_t tv_sec;      /* 秒 */
    __kernel_long_t tv_usec;     /* 微秒 */
};
```

### 4.2 struct timezone（时区结构）

```c
// include/uapi/linux/time.h
struct timezone {
    int tz_minuteswest;   /* 格林威治以西的分钟数 */
    int tz_dsttime;       /* DST（夏令时）校正类型，当前未使用 */
};
```

### 4.3 do_sys_settimeofday64 实现

```c
// kernel/time/time.c:169
int do_sys_settimeofday64(const struct timespec64 *tv, const struct timezone *tz)
{
    static int firsttime = 1;
    int error = 0;

    if (tv && !timespec64_valid_settod(tv))
        return -EINVAL;

    error = security_settime64(tv, tz);
    if (error)
        return error;

    if (tz) {
        /* Verify we're within the +-15 hrs range */
        if (tz->tz_minuteswest > 15*60 || tz->tz_minuteswest < -15*60)
            return -EINVAL;

        sys_tz = *tz;
        update_vsyscall_tz();
        if (firsttime) {
            firsttime = 0;
            if (!tv)
                timekeeping_warp_clock();
        }
    }
    if (tv)
        return do_settimeofday64(tv);
    return 0;
}
```

## 5. 流程图

```
用户态调用 settimeofday(tv, tz)
    │
    ▼
syscall 入口 (SYSCALL_DEFINE2)
    │
    ├─ tv 参数处理
    │   ├─ tv == NULL → 不修改时间
    │   └─ tv != NULL → 读取并验证 timeval
    │       ├─ tv_usec 范围检查 [0, USEC_PER_SEC)
    │       └─ 转换为纳秒 (tv_nsec = tv_usec * 1000)
    │
    ├─ tz 参数处理
    │   ├─ tz == NULL → 不修改时区
    │   └─ tz != NULL → 读取 timezone
    │
    ▼
do_sys_settimeofday64(tv, tz)
    │
    ├─ [设置时间分支]
    │   ├─ timespec64_valid_settod()  ── 无效 → 返回 -EINVAL
    │   ├─ security_settime64()       ── 拒绝 → 返回 -EPERM
    │   └─ do_settimeofday64()
    │       └─ timekeeping 子系统更新时间
    │
    ├─ [设置时区分支]
    │   ├─ tz_minuteswest 范围检查     ── 越界 → 返回 -EINVAL
    │   ├─ sys_tz = *tz
    │   ├─ update_vsyscall_tz()
    │   └─ 首次设置时区且未设置时间 → timekeeping_warp_clock()
    │
    └─ 返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EINVAL` | 无效参数 | `tv_usec` 超出范围 [0, 999999]；`tz_minuteswest` 超出 ±15 小时；时间值无效 |
| `EFAULT` | 地址错误 | `tv` 或 `tz` 指向无效的用户空间地址 |
| `EPERM` | 权限不足 | 调用者没有 `CAP_SYS_TIME` 权限 |
| `EACCES` | 访问被拒绝 | LSM（如 SELinux）安全策略阻止操作 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    struct timeval tv;
    struct timezone tz;

    // 仅设置时区（不修改时间）
    tz.tz_minuteswest = 8 * 60;  // UTC+8（中国标准时间）
    tz.tz_dsttime = 0;           // 不使用夏令时

    if (settimeofday(NULL, &tz) == -1) {
        perror("settimeofday (set tz)");
        exit(EXIT_FAILURE);
    }
    printf("时区已设置为 UTC+8\n");

    // 同时设置时间和时区（需要 CAP_SYS_TIME 权限）
    tv.tv_sec = 1700000000;
    tv.tv_usec = 0;

    if (settimeofday(&tv, NULL) == -1) {
        printf("设置时间失败: %s (需要 CAP_SYS_TIME 权限)\n",
               strerror(errno));
        // 不退出，这通常需要 root 权限
    }

    // 获取当前时间验证
    struct timeval now;
    gettimeofday(&now, NULL);
    printf("当前时间: %ld.%06ld\n", now.tv_sec, now.tv_usec);

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#定时器与时间)
- 内核源码：`kernel/time/time.c`
- 内核源码：`kernel/time/timekeeping.c`
- 时间结构：`include/uapi/linux/time.h`