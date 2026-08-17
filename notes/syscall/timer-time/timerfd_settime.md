# timerfd_settime 系统调用分析

## 1. 概述

`timerfd_settime` 用于启动、停止或重新配置由 `timerfd_create` 创建的定时器文件描述符。它可以设置定时器的首次到期时间和后续的间隔周期。该调用支持 `TFD_TIMER_ABSTIME` 标志，用于指定绝对时间而非相对时间。

**原型：**

```c
#include <sys/timerfd.h>

int timerfd_settime(int fd, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value);
```

**内核入口：**

```c
// fs/timerfd.c:548
SYSCALL_DEFINE4(timerfd_settime, int, ufd, int, flags,
                const struct __kernel_itimerspec __user *, utmr,
                struct __kernel_itimerspec __user *, otmr)
```

## 2. 使用场景

- **创建定时器**：设置一次性或周期性定时器
- **重新配置定时器**：修改正在运行的定时器的到期时间或间隔
- **停止定时器**：将 `it_value` 设置为 0 可停止定时器
- **获取旧值**：通过 `old_value` 参数获取之前定时器的设置

## 3. 函数调用栈

```
timerfd_settime(ufd, flags, utmr, otmr)               // 系统调用入口
  └─ get_itimerspec64(&new, utmr)                      // 从用户空间复制新值
  │    └─ 失败 → 返回 -EFAULT
  └─ do_timerfd_settime(ufd, flags, &new, &old)        // 核心实现
  │    ├─ CLASS(fd, f)(ufd)                             // 获取文件描述符
  │    ├─ fd_empty(f) → 返回 -EBADF
  │    ├─ fd_file(f)->f_op != &timerfd_fops → 返回 -EINVAL
  │    ├─ ctx = fd_file(f)->private_data                // 获取 timerfd_ctx
  │    │
  │    ├─ [停止现有定时器]
  │    │   ├─ spin_lock_irq(&ctx->wqh.lock)
  │    │   ├─ [alarm 定时器] alarm_try_to_cancel()
  │    │   ├─ [hrtimer 定时器] hrtimer_try_to_cancel()
  │    │   └─ 循环等待直到取消成功
  │    │
  │    ├─ [如果定时器已到期且有间隔]
  │    │   ├─ alarm_forward_now / hrtimer_forward_now  // 推进到下一个到期点
  │    │
  │    ├─ old->it_value = timerfd_get_remaining(ctx)    // 获取旧的剩余时间
  │    ├─ old->it_interval = ctx->tintv                 // 获取旧的间隔
  │    │
  │    ├─ timerfd_setup(ctx, flags, new)                // 重新配置定时器
  │    │   ├─ 设置新 it_value 和 it_interval
  │    │   ├─ [it_value 非零] → 启动定时器
  │    │   │   ├─ hrtimer_start(ctx->t.tmr, ...)        // 启动 hrtimer
  │    │   │   └─ ctx->t.tmr->function = timerfd_tmrproc  // 到期回调
  │    │   └─ [it_value 为零] → 停止定时器
  │    │
  │    └─ spin_unlock_irq(&ctx->wqh.lock)
  │
  └─ [otmr != NULL] put_itimerspec64(&old, otmr)        // 复制旧值到用户空间
       └─ 失败 → 返回 -EFAULT
```

## 4. 关键数据结构

### 4.1 struct timerfd_ctx（timerfd 上下文）

```c
// fs/timerfd.c:31
struct timerfd_ctx {
    union {
        struct hrtimer tmr;       // 高精度定时器（标准时钟）
        struct alarm alarm;       // 闹钟（CLOCK_REALTIME_ALARM / CLOCK_BOOTTIME_ALARM）
    } t;
    ktime_t tintv;                // 定时器间隔
    ktime_t moffs;                // 单调时钟偏移
    wait_queue_head_t wqh;        // 等待队列头
    u64 ticks;                    // 到期事件计数
    int clockid;                  // 时钟 ID
    short unsigned expired;       // 到期标志
    short unsigned settime_flags; // 设置标志
    struct rcu_head rcu;
    struct list_head clist;
    spinlock_t cancel_lock;
    bool might_cancel;
};
```

### 4.2 struct __kernel_itimerspec（用户空间定时器时间结构）

```c
// include/uapi/linux/time_types.h:12
struct __kernel_itimerspec {
    struct __kernel_timespec it_interval;    /* 定时器周期 */
    struct __kernel_timespec it_value;       /* 定时器到期时间 */
};
```

## 5. 流程图

```
用户态调用 timerfd_settime(fd, flags, &new_value, &old_value)
    │
    ▼
SYSCALL_DEFINE4(timerfd_settime)
    │
    ├─ get_itimerspec64(&new, utmr)  ── 失败 → -EFAULT
    │
    ▼
do_timerfd_settime(ufd, flags, &new, &old)
    │
    ├─ 验证 fd 有效且为 timerfd
    │   ├─ 无效 fd → -EBADF
    │   └─ 不是 timerfd → -EINVAL
    │
    ├─ 停止现有定时器
    │   ├─ spin_lock_irq
    │   ├─ 循环: hrtimer_try_to_cancel / alarm_try_to_cancel
    │   │   └─ 返回 >= 0 → 成功，跳出循环
    │   │   └─ 返回 < 0 → 等待后重试（hrtimer_cancel_wait_running）
    │   └─ spi n_unlock_irq
    │
    ├─ 处理已到期且有间隔的定时器
    │   └─ hrtimer_forward_now / alarm_forward_now
    │
    ├─ 保存旧值到 old
    │   ├─ old.it_value = timerfd_get_remaining(ctx)
    │   └─ old.it_interval = ctx->tintv
    │
    ├─ timerfd_setup(ctx, flags, &new)
    │   │
    │   ├─ 设置 ctx->tintv = new.it_interval
    │   │
    │   ├─ [new.it_value 非零]
    │   │   ├─ 处理 TFD_TIMER_ABSTIME 标志
    │   │   │   ├─ 已设置: 将到期时间设为绝对时间
    │   │   │   └─ 未设置: 将到期时间设为相对时间 (当前时间 + it_value)
    │   │   ├─ [alarm 定时器] alarm_start(ctx->t.alarm, ...)
    │   │   ├─ [hrtimer 定时器] hrtimer_start(ctx->t.tmr, ...)
    │   │   │   └─ 设置回调函数: timerfd_tmrproc
    │   │   └─ 更新 ctx->moffs
    │   │
    │   └─ [new.it_value 为零]
    │       └─ 定时器已停止（不启动）
    │
    └─ spin_unlock_irq
    │
    ▼
put_itimerspec64(&old, otmr)  ── 失败 → -EFAULT
    │
    ▼
返回 0
```

### 定时器到期回调

```c
// fs/timerfd.c:74
static enum hrtimer_restart timerfd_tmrproc(struct hrtimer *htmr)
{
    struct timerfd_ctx *ctx = container_of(htmr, struct timerfd_ctx, t.tmr);
    timerfd_triggered(ctx);
    return HRTIMER_NORESTART;  // 非重启模式（由 timerfd_setup 处理周期）
}

// fs/timerfd.c:63
static void timerfd_triggered(struct timerfd_ctx *ctx)
{
    unsigned long flags;
    spin_lock_irqsave(&ctx->wqh.lock, flags);
    ctx->expired = 1;              // 设置到期标志
    ctx->ticks++;                  // 递增 tick 计数
    wake_up_locked_poll(&ctx->wqh, EPOLLIN);  // 唤醒等待的进程
    spin_unlock_irqrestore(&ctx->wqh.lock, flags);
}
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EBADF` | 无效文件描述符 | `fd` 不是有效的文件描述符 |
| `EINVAL` | 无效参数 | `fd` 不是 timerfd；`flags` 包含无效标志；`new_value` 中的时间值无效 |
| `EFAULT` | 地址错误 | `new_value` 或 `old_value` 指向无效的用户空间地址 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

int main(void)
{
    struct itimerspec new_val;
    struct itimerspec old_val;
    int fd;

    // 创建 timerfd
    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd == -1) {
        perror("timerfd_create");
        exit(EXIT_FAILURE);
    }

    // 设置定时器：3 秒后首次到期，之后每 1 秒到期一次
    new_val.it_value.tv_sec = 3;
    new_val.it_value.tv_nsec = 0;
    new_val.it_interval.tv_sec = 1;
    new_val.it_interval.tv_nsec = 0;

    if (timerfd_settime(fd, 0, &new_val, &old_val) == -1) {
        perror("timerfd_settime");
        exit(EXIT_FAILURE);
    }

    printf("定时器已设置: 3 秒后首次到期，之后每 1 秒到期\n");

    // 重新配置为 5 秒后到期的单次定时器
    new_val.it_value.tv_sec = 5;
    new_val.it_value.tv_nsec = 0;
    new_val.it_interval.tv_sec = 0;
    new_val.it_interval.tv_nsec = 0;

    if (timerfd_settime(fd, 0, &new_val, &old_val) == -1) {
        perror("timerfd_settime");
        exit(EXIT_FAILURE);
    }

    printf("定时器已重新配置: 5 秒后到期（单次）\n");
    printf("之前的定时器: it_value=%ld.%ld, it_interval=%ld.%ld\n",
           old_val.it_value.tv_sec, old_val.it_value.tv_nsec,
           old_val.it_interval.tv_sec, old_val.it_interval.tv_nsec);

    // 读取到期事件（阻塞模式）
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) > 0) {
        printf("定时器到期次数: %lu\n", expirations);
    }

    close(fd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#定时器与时间)
- 内核源码：`fs/timerfd.c`
- 用户空间 API：`include/uapi/linux/time_types.h`