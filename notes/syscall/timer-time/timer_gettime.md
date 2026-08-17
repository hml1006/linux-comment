# timer_gettime 系统调用分析

## 1. 概述

`timer_gettime` 用于获取 POSIX 间隔定时器（interval timer）的**当前设置**，包括：
- `it_value`：定时器到期前的剩余时间（如果定时器已到期则为 0）
- `it_interval`：定时器的间隔时间（重载值）

**关键特性：**
- 返回的是 `struct itimerspec` 结构，包含剩余时间和间隔时间
- 剩余时间受定时器精度影响，可能被截断
- 如果定时器已到期且不是间隔定时器，`it_value` 返回 0

**内核源码位置：** `kernel/time/posix-timers.c:744`

---

## 2. 函数原型

```c
// kernel/time/posix-timers.c:744
SYSCALL_DEFINE2(timer_gettime, timer_t, timer_id,
                struct __kernel_itimerspec __user *, setting)
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `timer_id` | `timer_t` | 由 `timer_create` 返回的定时器 ID |
| `setting` | `struct __kernel_itimerspec __user *` | 输出参数，指向用户空间的 `itimerspec` 结构 |

### 返回值

| 返回值 | 说明 |
|--------|------|
| `0` | 成功 |
| `-EFAULT` | `setting` 指针无效（无法写入用户空间） |
| `-EINVAL` | `timer_id` 无效 |

---

## 3. 使用场景

### 3.1 查询定时器剩余时间

在需要知道定时器何时到期时，例如：
- 动态调整定时器参数前查询当前设置
- 监控定时器状态，判断是否仍在运行

### 3.2 典型用法

```c
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    timer_t timer_id;
    struct sigevent sev;
    struct itimerspec its, old;

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
    sev.sigev_value.sival_ptr = &timer_id;
    timer_create(CLOCK_MONOTONIC, &sev, &timer_id);

    /* 设置定时器：1秒后到期，之后每500ms到期一次 */
    its.it_value.tv_sec = 1;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 500000000;
    timer_settime(timer_id, 0, &its, NULL);

    /* 查询剩余时间 */
    sleep(1);  // 等待 1 秒
    timer_gettime(timer_id, &old);
    printf("Remaining time: %ld sec %ld nsec\n",
           old.it_value.tv_sec, old.it_value.tv_nsec);
    printf("Interval: %ld sec %ld nsec\n",
           old.it_interval.tv_sec, old.it_interval.tv_nsec);

    timer_delete(timer_id);
    return 0;
}
```

---

## 4. 函数调用链

```
timer_gettime(timer_id, setting)                    // kernel/time/posix-timers.c:744
  └─ do_timer_gettime(timer_id, &cur_setting)       // 获取定时器当前设置
       ├─ memset(setting, 0, sizeof(*setting))      // 清零输出缓冲区
       ├─ scoped_timer_get_or_fail(timer_id)        // 通过 scoped guard 获取锁
       │    ├─ lock_timer(timer_id)                 // 上锁并查找定时器
       │    │    ├─ posix_timer_by_id(timer_id)     // 哈希查找 k_itimer
       │    │    │    └─ hash_bucket(sig, id)       // 计算哈希桶
       │    │    │    └─ hlist_for_each_entry_rcu   // 遍历哈希链表
       │    │    │         └─ 匹配 timer->it_id == id
       │    │    └─ spin_lock_irq(&timer->it_lock)  // 获取定时器自旋锁
       │    │    └─ [失败] → 返回 NULL → 返回 -EINVAL
       │    └─ scoped_timer->kclock->timer_get()    // 调用时钟特定获取函数
       │         └─ common_hrtimer_arm 类的反向操作
       └─ return 0                                  // 成功
  └─ put_itimerspec64(&cur_setting, setting)         // 拷贝到用户空间
       └─ [失败] → ret = -EFAULT
```

### 内核源码

```c
// kernel/time/posix-timers.c:735
static int do_timer_gettime(timer_t timer_id, struct itimerspec64 *setting)
{
    memset(setting, 0, sizeof(*setting));
    scoped_timer_get_or_fail(timer_id)
        scoped_timer->kclock->timer_get(scoped_timer, setting);
    return 0;
}

// kernel/time/posix-timers.c:744
SYSCALL_DEFINE2(timer_gettime, timer_t, timer_id,
                struct __kernel_itimerspec __user *, setting)
{
    struct itimerspec64 cur_setting;

    int ret = do_timer_gettime(timer_id, &cur_setting);
    if (!ret) {
        if (put_itimerspec64(&cur_setting, setting))
            ret = -EFAULT;
    }
    return ret;
}
```

---

## 5. 关键数据结构

### 5.1 用户空间时间结构

```c
// include/uapi/linux/time.h
struct timespec {
    __kernel_time_t tv_sec;       // 秒（time_t）
    long            tv_nsec;      // 纳秒
};

// include/uapi/linux/time.h
struct itimerspec {
    struct timespec it_interval;  // 定时器间隔（重载时间）
    struct timespec it_value;     // 首次到期时间（剩余时间）
};
```

### 5.2 内核空间时间结构

```c
// include/linux/time64.h
struct timespec64 {
    time64_t tv_sec;              // 秒（64 位）
    long      tv_nsec;             // 纳秒
};

struct itimerspec64 {
    struct timespec64 it_interval; // 定时器间隔
    struct timespec64 it_value;    // 首次到期时间
};
```

### 5.3 时钟操作函数表

```c
// include/linux/posix-timers.h
struct k_clock {
    int  (*clock_getres)(const clockid_t which_clock, struct timespec64 *tp);
    int  (*clock_set)(const clockid_t which_clock, struct timespec64 *tp);
    int  (*clock_get_timespec)(const clockid_t which_clock, struct timespec64 *tp);
    int  (*clock_adj)(const clockid_t which_clock, struct timex *tx);
    int  (*timer_create)(struct k_itimer *timer);
    int  (*timer_set)(struct k_itimer *timer, int flags,
                      struct itimerspec64 *new_setting,
                      struct itimerspec64 *old_setting);
    void (*timer_get)(struct k_itimer *timer, struct itimerspec64 *setting);
    // ...
};
```

`timer_get` 回调由具体的时钟实现注册：
- `CLOCK_MONOTONIC` / `CLOCK_REALTIME` → `common_hrtimer_arm` 相关
- `CLOCK_PROCESS_CPUTIME_ID` → CPU 定时器函数

---

## 6. 完整流程图

```
用户态                              内核态
  |                                   |
  |  timer_gettime(timer_id, &its)    |
  |  ─────────────────────────────>   |
  |                                   |
  |                                   |  1. do_timer_gettime()
  |                                   |     ├─ memset(cur_setting, 0, ...)
  |                                   |     └─ scoped_timer_get_or_fail()
  |                                   |          ├─ posix_timer_by_id() 哈希查找
  |                                   |          ├─ spin_lock_irq()
  |                                   |          └─ kclock->timer_get(timr, setting)
  |                                   |               ├─ 读取 it_interval (间隔)
  |                                   |               ├─ 读取 it_value (剩余时间)
  |                                   |               │   └─ 如果定时器激活：
  |                                   |               │        = hrtimer_get_remaining()
  |                                   |               │        = 0 (已到期)
  |                                   |               └─ [scoped guard 自动释放锁]
  |                                   |
  |                                   |  2. put_itimerspec64(&cur, setting)
  |                                   |     └─ copy_to_user() 到用户空间
  |                                   |
  |  ← 返回 0 (或 -EFAULT/-EINVAL)    |
  |                                   |
```

---

## 7. 定时器状态表

| 定时器状态 | `it_value` | `it_interval` | 说明 |
|-----------|-----------|---------------|------|
| 未激活（未设置） | 0, 0 | 0, 0 | 刚创建未调用 `timer_settime` |
| 单次定时器（等待到期） | > 0 | 0, 0 | 一次性定时器等待触发 |
| 单次定时器（已到期） | 0, 0 | 0, 0 | 到期后不会再触发 |
| 间隔定时器（等待到期） | > 0 | > 0 | 首次到期前 |
| 间隔定时器（运行中） | 0, 0 | > 0 | 首次到期后，间隔定时器已激活 |
| 已禁用/已删除 | 0, 0 | 0, 0 | `timer_settime` 设置 0 或 `timer_delete` |

---

## 8. 错误处理

| 错误码 | 条件 | 说明 |
|--------|------|------|
| `-EINVAL` | `timer_id` 无效 | 定时器 ID 在哈希表中未找到，或已被删除 |
| `-EFAULT` | `setting` 指针无效 | 无法将 `itimerspec` 写入用户空间地址 |

---

## 9. 使用注意事项

### 9.1 剩余时间精度

`timer_gettime` 返回的剩余时间基于定时器的底层时钟精度：
- `CLOCK_MONOTONIC` / `CLOCK_REALTIME`：基于高精度定时器（HRT），纳秒级精度
- `CLOCK_PROCESS_CPUTIME_ID`：基于 CPU 定时器，精度受调度影响

### 9.2 与 `timerfd_gettime` 的区别

| 特性 | `timer_gettime` | `timerfd_gettime` |
|------|----------------|-------------------|
| 操作对象 | POSIX 定时器 ID（`timer_t`） | 文件描述符（fd） |
| 通知方式 | 信号（`SIGEV_SIGNAL`）或线程（`SIGEV_THREAD`） | `epoll`/`select`/`read` |
| 返回结构 | `struct itimerspec` | `struct itimerspec` |
| 内核入口 | `SYSCALL_DEFINE2(timer_gettime)` | `SYSCALL_DEFINE2(timerfd_gettime)` |

### 9.3 与 `timer_settime` 配合

`timer_gettime` 和 `timer_settime` 配合使用可安全修改定时器：
```c
struct itimerspec old;

/* 先查询当前设置 */
timer_gettime(timer_id, &old);

/* 基于旧值调整新值 */
old.it_value.tv_sec += 5;  // 延长 5 秒

/* 应用新设置 */
timer_settime(timer_id, 0, &old, NULL);
```

---

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#定时器与时间)
- 内核源码：`kernel/time/posix-timers.c`
- 内核头文件：`include/linux/posix-timers.h`
- 用户态头文件：`<time.h>`
- man 手册：`timer_gettime(2)`