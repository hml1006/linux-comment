# gettimeofday 系统调用分析

## 1. 概述

`gettimeofday` 读取系统实时时间（wall clock）与时区信息。它是最古老的 Unix 时间接口之一，精度为微秒（`tv_usec`）。

**关键特性：**

- 返回自 Unix Epoch（1970-01-01 00:00:00 UTC）以来的秒数和微秒数
- 时区参数 `tz` 是为兼容历史实现保留的，Linux 上几乎不使用
- 与 `clock_gettime(CLOCK_REALTIME)` 读取同一时间源，但精度只有微秒
- 新代码应优先使用 `clock_gettime`，`gettimeofday` 主要保留给旧程序

**内核源码位置：** `kernel/time/time.c:140`

---

## 2. 用户态接口

```c
#include <sys/time.h>

int gettimeofday(struct timeval *tv, struct timezone *tz);
```

| 参数 | 说明 |
|------|------|
| `tv` | 输出参数，返回 `struct timeval`（秒 + 微秒），可为 `NULL` |
| `tz` | 输出参数，返回 `struct timezone`，可为 `NULL`；已废弃 |

```c
struct timeval {
    time_t      tv_sec;     // 秒
    suseconds_t tv_usec;    // 微秒（0 ~ 999999）
};

struct timezone {
    int tz_minuteswest;     // 相对 UTC 的分钟偏移（西为正）
    int tz_dsttime;         // DST 校正类型（Linux 未使用）
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
// kernel/time/time.c:140
SYSCALL_DEFINE2(gettimeofday, struct __kernel_old_timeval __user *, tv,
        struct timezone __user *, tz)
{
    if (likely(tv != NULL)) {
        struct timespec64 ts;

        ktime_get_real_ts64(&ts);
        if (put_user(ts.tv_sec, &tv->tv_sec) ||
            put_user(ts.tv_nsec / 1000, &tv->tv_usec))
            return -EFAULT;
    }
    if (unlikely(tz != NULL)) {
        if (copy_to_user(tz, &sys_tz, sizeof(sys_tz)))
            return -EFAULT;
    }
    return 0;
}
```

实现非常直接：

1. `ktime_get_real_ts64()` 从 timekeeper 读取实时时间（纳秒精度）；
2. `tv_nsec / 1000` 截断为微秒，分别写入 `tv_sec` 与 `tv_usec`；
3. `tz` 非空时拷贝全局 `sys_tz`（通常由旧的 `settimeofday(tz)` 设置）。

### 3.2 时间源与并发

`ktime_get_real_ts64()` 内部通过 timekeeper 的 seqcount 做无锁读取：

```text
ktime_get_real_ts64(ts)
  └─ ktime_get_real_ts64_mg / tk_xtime
       ├─ 读 tk->seq（偶数表示稳定）
       ├─ 取 tk->xtime_sec + tk->wall_to_monotonic
       ├─ 用 clocksource 的 cyc2ns 计算亚秒部分
       └─ 若 seq 变化则重试（seqcount 读侧临界区）
```

因此 `gettimeofday` 不需要持锁，也不会阻塞。

### 3.3 与 vDSO 的关系

glibc 中的 `gettimeofday` 通常是 `__vdso_gettimeofday`，直接读 ARM64 的 `CNTVCT_EL0` 虚拟计数器并用 vDSO 数据页中的参数换算，不执行 `svc #0`：

```text
__vdso_gettimeofday(tv, tz)                  // lib/vdso/gettimeofday.c
  ├─ __arch_get_hw_counter(VDSO_CLOCKMODE_ARCHTIMER, vd)
  │    └─ __arch_counter_get_cntvct()        // mrs cntvct_el0
  ├─ 计算秒/纳秒，转换为微秒
  └─ tz != NULL 时回退到系统调用（vDSO 不维护时区）
```

正因为 `tz` 只在慢路径处理，传 `tz != NULL` 会强制走系统调用。

---

## 4. 调用链

```text
用户态 gettimeofday(tv, tz)
  │
  ├─ tz == NULL ──► __vdso_gettimeofday
  │                   ├─ 读 CNTVCT_EL0
  │                   ├─ 换算为 timespec
  │                   └─ 写回 tv（不进入内核）
  │
  └─ tz != NULL ──► svc #0 → sys_gettimeofday
                        ├─ tv != NULL:
                        │    ├─ ktime_get_real_ts64(&ts)
                        │    └─ put_user(tv_sec / tv_usec)
                        └─ tz != NULL:
                             └─ copy_to_user(&sys_tz)
```

---

## 5. 关键数据结构

```c
// include/uapi/linux/time.h
struct __kernel_old_timeval {
    __kernel_long_t tv_sec;      // 秒
    __kernel_long_t tv_usec;     // 微秒
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
```

半秒以下的精度在拷贝前已被截断：`ts.tv_nsec / 1000` 丢弃了不足 1 微秒的部分。

---

## 6. 错误码

| errno | 触发条件 |
|-------|----------|
| `EFAULT` | `tv` 或 `tz` 指向不可写的用户空间 |
| `ENOSYS` | 极旧架构/内核未实现（现代 ARM64 不会出现） |

注意：`gettimeofday` **不会**因为 `tz` 内容无效而失败——内核只是原样拷贝 `sys_tz`。

---

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

int main(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        return 1;
    }
    printf("sec=%lld usec=%ld\n",
           (long long)tv.tv_sec, (long)tv.tv_usec);

    /* 等价的现代写法：微秒精度需自行换算 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("sec=%lld usec=%ld\n",
           (long long)ts.tv_sec, ts.tv_nsec / 1000);

    return 0;
}
```

### 7.1 测量耗时（不推荐，仅兼容旧代码）

```c
struct timeval start, end;
gettimeofday(&start, NULL);
/* ... */
gettimeofday(&end, NULL);
long us = (end.tv_sec - start.tv_sec) * 1000000L +
          (end.tv_usec - start.tv_usec);
```

测量时间间隔应改用 `clock_gettime(CLOCK_MONOTONIC)`，避免系统时间被调整导致结果失真。

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `kernel/time/time.c:140` | `SYSCALL_DEFINE2(gettimeofday)` |
| `kernel/time/time.c:224` | 32 位兼容入口 `COMPAT_SYSCALL_DEFINE2(gettimeofday)` |
| `kernel/time/timekeeping.c` | `ktime_get_real_ts64()` 与 timekeeper |
| `lib/vdso/gettimeofday.c` | vDSO 快路径 |
| `arch/arm64/include/asm/vdso/gettimeofday.h` | `gettimeofday_fallback()`、计数器读取 |
| `include/uapi/linux/time.h` | `struct __kernel_old_timeval` / `struct timezone` |

相关文档：[clock_gettime](clock_gettime.md)、[settimeofday](settimeofday.md)、[clock_settime](clock_settime.md)。
