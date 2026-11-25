# clock_gettime 系统调用分析

## 1. 概述

`clock_gettime` 读取指定时钟的当前时间，精度为纳秒。它是 POSIX 时间接口的核心，`clock_gettime(CLOCK_MONOTONIC)` 也是现代程序测量时间间隔的首选。

**关键特性：**

- 支持多种时钟：实时时钟、单调时钟、进程/线程 CPU 时钟、粗粒度时钟等
- 绝大多数读时钟场景由 **vDSO** 在用户态完成，不真正进入内核
- 写入用户空间的是 `struct timespec`（秒 + 纳秒）
- 单调时钟不受系统时间调整影响，适合测量时间间隔

**内核源码位置：** `kernel/time/posix-timers.c:1142`

---

## 2. 用户态接口

```c
#include <time.h>

int clock_gettime(clockid_t clockid, struct timespec *tp);
```

| 参数 | 说明 |
|------|------|
| `clockid` | 时钟 ID，见下表 |
| `tp` | 输出参数，返回秒和纳秒 |

### 2.1 常用时钟 ID

| 时钟 | 说明 | 受 settimeofday 影响 |
|------|------|------|
| `CLOCK_REALTIME` | 系统实时时间（wall clock） | 是 |
| `CLOCK_MONOTONIC` | 自启动起的单调时间，不含挂起时间 | 否 |
| `CLOCK_BOOTTIME` | 单调时间，包含系统挂起时间 | 否 |
| `CLOCK_MONOTONIC_RAW` | 未经过 NTP 校正的硬件时钟 | 否 |
| `CLOCK_REALTIME_COARSE` | 粗粒度实时时间（低精度、低开销） | 是 |
| `CLOCK_MONOTONIC_COARSE` | 粗粒度单调时间 | 否 |
| `CLOCK_PROCESS_CPUTIME_ID` | 进程消耗的 CPU 时间 | 不适用 |
| `CLOCK_THREAD_CPUTIME_ID` | 线程消耗的 CPU 时间 | 不适用 |
| `CLOCK_TAI` | 国际原子时 | 不适用 |

### 2.2 返回值

| 返回值 | 说明 |
|--------|------|
| `0` | 成功 |
| `-1` | 失败，`errno` 见第 6 节 |

---

## 3. 内核实现

### 3.1 系统调用入口

```c
// kernel/time/posix-timers.c:1142
SYSCALL_DEFINE2(clock_gettime, const clockid_t, which_clock,
        struct __kernel_timespec __user *, tp)
{
    const struct k_clock *kc = clockid_to_kclock(which_clock);
    struct timespec64 kernel_tp;
    int error;

    if (!kc)
        return -EINVAL;

    error = kc->clock_get_timespec(which_clock, &kernel_tp);

    if (!error && put_timespec64(&kernel_tp, tp))
        error = -EFAULT;

    return error;
}
```

入口本身只做三件事：把 `clockid` 解析为 `k_clock` 操作表、调用时钟相关的读取回调、把结果拷贝回用户空间。

### 3.2 时钟分发表

`clockid_to_kclock()` 用 `posix_clocks[]` 把时钟 ID 映射到具体实现：

```c
// kernel/time/posix-timers.c
static const struct k_clock * const posix_clocks[] = {
    [CLOCK_REALTIME]            = &clock_realtime,
    [CLOCK_MONOTONIC]           = &clock_monotonic,
    [CLOCK_PROCESS_CPUTIME_ID]  = &clock_process,
    [CLOCK_THREAD_CPUTIME_ID]   = &clock_thread,
    [CLOCK_MONOTONIC_RAW]       = &clock_monotonic_raw,
    [CLOCK_REALTIME_COARSE]     = &clock_realtime_coarse,
    [CLOCK_MONOTONIC_COARSE]    = &clock_monotonic_coarse,
    [CLOCK_BOOTTIME]            = &clock_boottime,
    [CLOCK_REALTIME_ALARM]      = &alarm_clock,
    [CLOCK_BOOTTIME_ALARM]      = &alarm_clock,
    [CLOCK_TAI]                 = &clock_tai,
};
```

负数 ID 走另外两条路径：`CLOCKFD_MASK` 匹配时是动态 POSIX 时钟（`clock_posix_dynamic`），否则是 CPU 时钟（`clock_posix_cpu`）。

### 3.3 典型读取回调

```c
// kernel/time/posix-timers.c:187
static int posix_get_realtime_timespec(clockid_t which_clock, struct timespec64 *tp)
{
    ktime_get_real_ts64(tp);
    return 0;
}

// kernel/time/posix-timers.c:210
static int posix_get_monotonic_timespec(clockid_t which_clock, struct timespec64 *tp)
{
    ktime_get_ts64(tp);
    timens_add_monotonic(tp);      // 加上时间命名空间偏移
    return 0;
}
```

实时时钟来自 `timekeeper` 的墙钟时间；单调时钟由 `ktime_get_ts64()` 基于 clocksource 计算，并叠加当前时间命名空间的偏移。

---

## 4. 调用链

### 4.1 vDSO 快路径（绝大多数情况）

```text
用户态 clock_gettime(CLOCK_MONOTONIC, &ts)
  │  glibc 解析为 __vdso_clock_gettime
  ▼
__vdso_clock_gettime(clock, ts)              // lib/vdso/gettimeofday.c
  ├─ __arch_get_hw_counter(clock_mode, vd)   // 读 ARM64 虚拟计数器
  │    └─ __arch_counter_get_cntvct()        // mrs cntvct_el0
  ├─ 用 vdso_time_data 中的 mult/shift 换算纳秒
  ├─ 叠加 seq 计数校验，处理与内核更新并发
  └─ 若时钟模式不可用 → 回退 clock_gettime_fallback → svc #0
```

### 4.2 系统调用慢路径

```text
sys_clock_gettime(which_clock, tp)
  └─ clockid_to_kclock(which_clock)
       └─ kc->clock_get_timespec(which_clock, &kernel_tp)
            ├─ CLOCK_REALTIME  → ktime_get_real_ts64
            ├─ CLOCK_MONOTONIC → ktime_get_ts64 + timens_add_monotonic
            ├─ CLOCK_*_COARSE  → 读取 timekeeper 的粗粒度缓存
            └─ CPU 时钟        → 读取 task/thread 的累计运行时间
       └─ put_timespec64(&kernel_tp, tp)     // 拷贝到用户空间
```

ARM64 上 `CNTVCT_EL0` 是虚拟计数器，频率由设备树 `clock-frequency` 或 CNTFRQ_EL0 描述；vDSO 直接读取它，因此一次 `clock_gettime` 通常只有几十个周期。

---

## 5. 关键数据结构

```c
// include/uapi/linux/time_types.h
struct __kernel_timespec {
    __kernel_time64_t tv_sec;      // 秒
    long long         tv_nsec;     // 纳秒（0 ~ 999999999）
};

// include/linux/time64.h
struct timespec64 {
    time64_t tv_sec;
    long     tv_nsec;
};
```

```c
// kernel/time/posix-timers.h:10（节选）
struct k_clock {
    int  (*clock_getres)(const clockid_t which_clock, struct timespec64 *tp);
    int  (*clock_set)(const clockid_t which_clock, const struct timespec64 *tp);
    int  (*clock_get_timespec)(const clockid_t which_clock,
                               struct timespec64 *tp);
    ktime_t (*clock_get_ktime)(const clockid_t which_clock);
    int  (*clock_adj)(const clockid_t which_clock, struct __kernel_timex *tx);
    /* ... 定时器相关回调 ... */
};
```

---

## 6. 错误码

| errno | 触发条件 |
|-------|----------|
| `EINVAL` | `which_clock` 不是合法时钟 ID |
| `EFAULT` | `tp` 指向不可写的用户空间 |
| `ENOSYS` | 旧内核不支持该时钟（vDSO 回退失败） |

---

## 7. 使用示例

```c
#include <stdio.h>
#include <time.h>

int main(void)
{
    struct timespec mono, real;

    if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0) {
        perror("clock_gettime(CLOCK_MONOTONIC)");
        return 1;
    }
    clock_gettime(CLOCK_REALTIME, &real);

    printf("monotonic: %lld.%09ld\n",
           (long long)mono.tv_sec, mono.tv_nsec);
    printf("realtime : %lld.%09ld\n",
           (long long)real.tv_sec, real.tv_nsec);

    /* 测量一段代码的耗时 */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    /* ... 被测代码 ... */
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ns = (end.tv_sec - start.tv_sec) * 1e9 +
                (end.tv_nsec - start.tv_nsec);
    printf("elapsed: %.0f ns\n", ns);

    return 0;
}
```

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `kernel/time/posix-timers.c:1142` | `SYSCALL_DEFINE2(clock_gettime)` |
| `kernel/time/posix-timers.c:187` | `posix_get_realtime_timespec()` |
| `kernel/time/posix-timers.c:210` | `posix_get_monotonic_timespec()` |
| `kernel/time/posix-timers.h:10` | `struct k_clock` 定义 |
| `lib/vdso/gettimeofday.c` | 通用 vDSO 时间读取实现 |
| `arch/arm64/kernel/vdso/vgettimeofday.c` | ARM64 vDSO 入口 |
| `arch/arm64/include/asm/vdso/gettimeofday.h` | `__arch_get_hw_counter()` / `CNTVCT_EL0` |
| `include/uapi/linux/time_types.h` | `struct __kernel_timespec` |

相关文档：[clock_getres](clock_getres.md)、[clock_settime](clock_settime.md)、[gettimeofday](gettimeofday.md)。
