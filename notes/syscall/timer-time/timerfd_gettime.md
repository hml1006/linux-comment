# timerfd_gettime 系统调用分析

## 1. 概述

`timerfd_gettime` 用于获取通过 `timerfd_create` 创建的定时器文件描述符的当前剩余时间和间隔设置。它返回定时器下一次到期前的剩余时间，以及定时器的间隔值（如果定时器是周期性的）。

**原型：**

```c
#include <sys/timerfd.h>

int timerfd_gettime(int fd, struct itimerspec *curr_value);
```

**内核入口：**

```c
// fs/timerfd.c:566
SYSCALL_DEFINE2(timerfd_gettime, int, ufd,
                struct __kernel_itimerspec __user *, otmr)
```

## 2. 使用场景

- **查询定时器状态**：检查 timerfd 定时器是否正在运行以及剩余时间
- **监控定时器进度**：在长时间运行的操作中定期检查定时器剩余时间
- **调试和诊断**：验证定时器设置是否正确
- **避免竞态条件**：在读取 timerfd 之前检查是否有待处理的到期事件

## 3. 函数调用栈

```
timerfd_gettime(ufd, otmr)                          // 系统调用入口
  └─ do_timerfd_gettime(ufd, &kotmr)                // 核心实现
  │    ├─ CLASS(fd, f)(ufd)                         // 获取文件描述符
  │    ├─ fd_empty(f) → 返回 -EBADF                 // 无效 fd
  │    ├─ fd_file(f)->f_op != &timerfd_fops → 返回 -EINVAL  // 不是 timerfd
  │    ├─ ctx = fd_file(f)->private_data             // 获取 timerfd_ctx
  │    ├─ spin_lock_irq(&ctx->wqh.lock)              // 加锁保护
  │    │
  │    ├─ [如果定时器已到期且有间隔]
  │    │   ├─ ctx->expired = 0                       // 清除到期标志
  │    │   ├─ [alarm 定时器] alarm_forward_now()     // 推进闹钟
  │    │   ├─ [hrtimer 定时器] hrtimer_forward_now() // 推进高精度定时器
  │    │   ├─ alarm_restart / hrtimer_restart        // 重新启动定时器
  │    │   └─ ctx->ticks 更新                         // 更新 tick 计数
  │    │
  │    ├─ t->it_value = timerfd_get_remaining(ctx)    // 获取剩余时间
  │    ├─ t->it_interval = ctx->tintv                 // 获取间隔时间
  │    └─ spin_unlock_irq(&ctx->wqh.lock)             // 解锁
  │
  └─ put_itimerspec64(&kotmr, otmr)                  // 复制到用户空间
       └─ 失败 → 返回 -EFAULT
```

**核心实现源码：**

```c
// fs/timerfd.c:515
static int do_timerfd_gettime(int ufd, struct itimerspec64 *t)
{
    struct timerfd_ctx *ctx;
    CLASS(fd, f)(ufd);

    if (fd_empty(f))
        return -EBADF;
    if (fd_file(f)->f_op != &timerfd_fops)
        return -EINVAL;
    ctx = fd_file(f)->private_data;

    spin_lock_irq(&ctx->wqh.lock);
    if (ctx->expired && ctx->tintv) {
        ctx->expired = 0;
        if (isalarm(ctx)) {
            ctx->ticks += alarm_forward_now(&ctx->t.alarm, ctx->tintv) - 1;
            alarm_restart(&ctx->t.alarm);
        } else {
            ctx->ticks += hrtimer_forward_now(&ctx->t.tmr, ctx->tintv) - 1;
            hrtimer_restart(&ctx->t.tmr);
        }
    }
    t->it_value = ktime_to_timespec64(timerfd_get_remaining(ctx));
    t->it_interval = ktime_to_timespec64(ctx->tintv);
    spin_unlock_irq(&ctx->wqh.lock);
    return 0;
}
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
    ktime_t tintv;                // 定时器间隔（0 表示非周期性）
    ktime_t moffs;                // 单调时钟偏移（用于时钟设置检测）
    wait_queue_head_t wqh;        // 等待队列头（在 read/poll/epoll 中使用）
    u64 ticks;                    // 到期事件计数
    int clockid;                  // 时钟 ID
    short unsigned expired;       // 到期标志
    short unsigned settime_flags; // 设置标志（用于 fdinfo 显示）
    struct rcu_head rcu;          // RCU 销毁回调
    struct list_head clist;       // 取消链表
    spinlock_t cancel_lock;       // 取消锁
    bool might_cancel;            // 是否可能被取消
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

### 4.3 struct itimerspec64（内核空间定时器时间结构）

```c
// include/linux/time64.h:18
struct itimerspec64 {
    struct timespec64 it_interval;
    struct timespec64 it_value;
};
```

## 5. 流程图

```
用户态调用 timerfd_gettime(fd, &curr_value)
    │
    ▼
SYSCALL_DEFINE2(timerfd_gettime)
    │
    └─ do_timerfd_gettime(ufd, &kotmr)
        │
        ├─ 获取文件描述符 f
        │   ├─ fd 无效 → 返回 -EBADF
        │   └─ fd 不是 timerfd → 返回 -EINVAL
        │
        ├─ 获取 ctx = fd->private_data
        │
        ├─ spin_lock_irq(&ctx->wqh.lock)
        │
        ├─ 检查 ctx->expired && ctx->tintv
        │   ├─ true: 定时器已到期且有间隔
        │   │   ├─ 清除 expired 标志
        │   │   ├─ alarm_forward_now / hrtimer_forward_now
        │   │   │   └─ 推进定时器到下一个到期点
        │   │   ├─ 更新 ticks 计数
        │   │   └─ 重新启动定时器
        │   │
        │   └─ false: 未到期或非周期性
        │
        ├─ it_value = timerfd_get_remaining(ctx)
        │   ├─ 定时器正在运行 → 返回到期前的剩余时间
        │   └─ 定时器未运行 → 返回 0
        │
        ├─ it_interval = ctx->tintv
        │
        └─ spin_unlock_irq(&ctx->wqh.lock)
        │
        ▼
    put_itimerspec64(&kotmr, otmr)
        ├─ 成功 → 返回 0
        └─ 失败 → 返回 -EFAULT
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EBADF` | 无效文件描述符 | `fd` 不是有效的文件描述符 |
| `EINVAL` | 无效参数 | `fd` 不是 timerfd 文件描述符（`f_op != &timerfd_fops`） |
| `EFAULT` | 地址错误 | `curr_value` 指向无效的用户空间地址 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    struct itimerspec spec;
    struct itimerspec curr;
    int fd;

    // 创建一个 2 秒后到期、每 1 秒重复的定时器
    spec.it_value.tv_sec = 2;
    spec.it_value.tv_nsec = 0;
    spec.it_interval.tv_sec = 1;
    spec.it_interval.tv_nsec = 0;

    fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd == -1) {
        perror("timerfd_create");
        exit(EXIT_FAILURE);
    }

    // 设置定时器
    if (timerfd_settime(fd, 0, &spec, NULL) == -1) {
        perror("timerfd_settime");
        exit(EXIT_FAILURE);
    }

    // 立即查询剩余时间
    if (timerfd_gettime(fd, &curr) == -1) {
        perror("timerfd_gettime");
        exit(EXIT_FAILURE);
    }

    printf("定时器状态:\n");
    printf("  it_value:     %ld sec, %ld nsec (到期前剩余时间)\n",
           curr.it_value.tv_sec, curr.it_value.tv_nsec);
    printf("  it_interval:  %ld sec, %ld nsec (周期)\n",
           curr.it_interval.tv_sec, curr.it_interval.tv_nsec);

    // 等待 1 秒后再次查询
    sleep(1);

    if (timerfd_gettime(fd, &curr) == -1) {
        perror("timerfd_gettime");
        exit(EXIT_FAILURE);
    }

    printf("1 秒后查询:\n");
    printf("  it_value:     %ld sec, %ld nsec (剩余时间减少)\n",
           curr.it_value.tv_sec, curr.it_value.tv_nsec);

    close(fd);
    return 0;
}
```

**可能的输出：**

```
定时器状态:
  it_value:     2 sec, 0 nsec (到期前剩余时间)
  it_interval:  1 sec, 0 nsec (周期)
1 秒后查询:
  it_value:     1 sec, 0 nsec (剩余时间减少)
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#定时器与时间)
- 内核源码：`fs/timerfd.c`
- 内核源码：`include/linux/time64.h`（`struct itimerspec64` 定义）
- 用户空间 API：`include/uapi/linux/time_types.h`（`struct __kernel_itimerspec` 定义）