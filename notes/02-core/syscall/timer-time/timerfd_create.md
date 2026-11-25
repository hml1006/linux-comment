# timerfd_create 系统调用分析

## 1. 概述

`timerfd_create` 创建一个定时器文件描述符（timerfd）。定时器到期时，内核让该文件描述符变为**可读**，`read()` 返回一个 `u64`，表示自上次读取以来到期的次数。

**关键特性：**

- 把“定时”转换为“文件可读事件”，可无缝接入 `select` / `poll` / `epoll`
- 支持 `CLOCK_REALTIME`、`CLOCK_MONOTONIC`、`CLOCK_BOOTTIME` 及其 `_ALARM` 变体
- 创建时不启动，需 `timerfd_settime` 武装
- 周期性定时器若多次到期而未被读取，`read()` 返回累计次数（overrun）
- 底层对普通时钟使用 hrtimer，对 `_ALARM` 时钟使用 `struct alarm`

**内核源码位置：** `fs/timerfd.c:424`

---

## 2. 用户态接口

```c
#include <sys/timerfd.h>

int timerfd_create(int clockid, int flags);
```

| 参数 | 说明 |
|------|------|
| `clockid` | `CLOCK_REALTIME` / `CLOCK_MONOTONIC` / `CLOCK_BOOTTIME` / `CLOCK_REALTIME_ALARM` / `CLOCK_BOOTTIME_ALARM` |
| `flags` | `TFD_NONBLOCK`、`TFD_CLOEXEC` |

### 2.1 创建标志

```c
// include/uapi/linux/timerfd.h
#define TFD_CLOEXEC   O_CLOEXEC      // exec 时自动关闭
#define TFD_NONBLOCK  O_NONBLOCK     // 非阻塞读取
```

### 2.2 相关接口

```c
int timerfd_settime(int fd, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);
```

`timerfd_settime` 的 `flags` 额外支持：

| 标志 | 说明 |
|------|------|
| `TFD_TIMER_ABSTIME` | `new_value->it_value` 是绝对时刻 |
| `TFD_TIMER_CANCEL_ON_SET` | 实时时钟被 settimeofday 调整时，等待中的 `read` 返回 `ECANCELED` |

### 2.3 返回值

| 返回值 | 说明 |
|--------|------|
| `>= 0` | 定时器文件描述符 |
| `-1` | 失败，`errno` 见第 6 节 |

---

## 3. 内核实现

### 3.1 系统调用入口

```c
// fs/timerfd.c:424
SYSCALL_DEFINE2(timerfd_create, int, clockid, int, flags)
{
    struct timerfd_ctx *ctx __free(kfree) = NULL;
    int ret;

    /* Check the TFD_* constants for consistency.  */
    BUILD_BUG_ON(TFD_CLOEXEC != O_CLOEXEC);
    BUILD_BUG_ON(TFD_NONBLOCK != O_NONBLOCK);

    if ((flags & ~TFD_CREATE_FLAGS) ||
        (clockid != CLOCK_MONOTONIC &&
         clockid != CLOCK_REALTIME &&
         clockid != CLOCK_REALTIME_ALARM &&
         clockid != CLOCK_BOOTTIME &&
         clockid != CLOCK_BOOTTIME_ALARM))
        return -EINVAL;

    if ((clockid == CLOCK_REALTIME_ALARM ||
         clockid == CLOCK_BOOTTIME_ALARM) &&
        !capable(CAP_WAKE_ALARM))
        return -EPERM;

    ctx = kzalloc_obj(*ctx);
    if (!ctx)
        return -ENOMEM;

    init_waitqueue_head(&ctx->wqh);
    spin_lock_init(&ctx->cancel_lock);
    ctx->clockid = clockid;

    if (isalarm(ctx))
        alarm_init(&ctx->t.alarm,
                   ctx->clockid == CLOCK_REALTIME_ALARM ?
                   ALARM_REALTIME : ALARM_BOOTTIME,
                   timerfd_alarmproc);
    else
        hrtimer_setup(&ctx->t.tmr, timerfd_tmrproc, clockid, HRTIMER_MODE_ABS);

    ctx->moffs = ktime_mono_to_real(0);

    ret = FD_ADD(flags & TFD_SHARED_FCNTL_FLAGS,
                 anon_inode_getfile_fmode("[timerfd]", &timerfd_fops, ctx,
                                          O_RDWR | (flags & TFD_SHARED_FCNTL_FLAGS),
                                          FMODE_NOWAIT));
    if (ret >= 0)
        retain_and_null_ptr(ctx);
    return ret;
}
```

要点：

- 只接受 5 种时钟；`_ALARM` 时钟需要 `CAP_WAKE_ALARM`；
- `timerfd_ctx` 通过 `anon_inode_getfile_fmode()` 挂在匿名 inode 上，不占用真实文件系统；
- `FMODE_NOWAIT` 表示支持 `RWF_NOWAIT` / `IOCB_NOWAIT` 语义；
- `__free(kfree)` 是内核的 scope-based cleanup：`FD_ADD` 失败时自动释放 `ctx`。

### 3.2 到期路径

```c
// fs/timerfd.c（节选自 timerfd_tmrproc）
static enum hrtimer_restart timerfd_tmrproc(struct hrtimer *htmr)
{
    struct timerfd_ctx *ctx = container_of(htmr, struct timerfd_ctx, t.tmr);

    spin_lock_irq(&ctx->wqh.lock);
    ctx->ticks++;
    ctx->expired = 1;
    wake_up_locked_poll(&ctx->wqh, EPOLLIN);
    spin_unlock_irq(&ctx->wqh.lock);

    return HRTIMER_NORESTART;
}
```

每次到期只做三件事：累加 `ticks`、置 `expired`、唤醒等待者。周期性的重新武装放在 `read()` 中完成，避免用户设置极短周期造成中断风暴（DoS）。

### 3.3 read 语义

```c
// fs/timerfd.c:300（节选）
static ssize_t timerfd_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct timerfd_ctx *ctx = file->private_data;
    u64 ticks = 0;

    if (iov_iter_count(to) < sizeof(ticks))
        return -EINVAL;

    spin_lock_irq(&ctx->wqh.lock);
    if (file->f_flags & O_NONBLOCK || iocb->ki_flags & IOCB_NOWAIT)
        res = -EAGAIN;
    else
        res = wait_event_interruptible_locked_irq(ctx->wqh, ctx->ticks);

    if (timerfd_canceled(ctx)) {
        ctx->ticks = 0;
        ctx->expired = 0;
        res = -ECANCELED;
    }

    if (ctx->ticks) {
        unsigned int expired = ctx->expired;

        ticks = ctx->ticks;
        ctx->expired = 0;
        ctx->ticks = 0;

        if (expired && ctx->tintv)
            ticks += timerfd_restart(ctx);
    }
    spin_unlock_irq(&ctx->wqh.lock);

    if (ticks) {
        res = copy_to_iter(&ticks, sizeof(ticks), to);
        if (!res)
            res = -EFAULT;
    }
    return res;
}
```

- 缓冲区必须至少 8 字节，否则 `EINVAL`；
- 非阻塞且无到期事件时返回 `EAGAIN`；
- 返回的 `ticks` 是累计到期次数，读取后计数清零；
- 周期定时器在读取时重新武装，`timerfd_restart()` 会把读取期间又到期的次数补上。

---

## 4. 调用链

```text
用户态 timerfd_create(clockid, flags)
  │
  ▼
sys_timerfd_create(clockid, flags)
  ├─ 校验 flags / clockid
  ├─ _ALARM 时钟 → capable(CAP_WAKE_ALARM)
  ├─ kzalloc_obj(struct timerfd_ctx)
  ├─ init_waitqueue_head(&ctx->wqh)
  ├─ alarm_init() 或 hrtimer_setup()
  └─ anon_inode_getfile_fmode("[timerfd]", &timerfd_fops, ctx, ...)
       └─ 返回 fd

后续：
timerfd_settime(fd, flags, &its, NULL)
  └─ do_timerfd_settime()
       ├─ hrtimer_start_range_ns() / alarm_start()
       └─ 到期 → timerfd_tmrproc()
            ├─ ctx->ticks++
            ├─ wake_up_locked_poll(EPOLLIN)
            └─ HRTIMER_NORESTART

read(fd, &ticks, 8) / epoll_wait()
  └─ timerfd_read_iter()
       ├─ 等待 ctx->wqh
       ├─ 读取并清零 ticks
       └─ 周期定时器 → timerfd_restart()
```

---

## 5. 关键数据结构

```c
// fs/timerfd.c:31
struct timerfd_ctx {
    union {
        struct hrtimer tmr;      // 普通时钟
        struct alarm alarm;      // _ALARM 时钟
    } t;
    ktime_t tintv;               // 周期
    ktime_t moffs;               // MONOTONIC 与 REALTIME 的偏移
    wait_queue_head_t wqh;       // 等待队列
    u64 ticks;                   // 累计到期次数
    int clockid;
    short unsigned expired;      // 是否已到期未读取
    short unsigned settime_flags;
    struct rcu_head rcu;
    struct list_head clist;      // 取消链表（CANCEL_ON_SET）
    spinlock_t cancel_lock;
    bool might_cancel;
};
```

```c
// fs/timerfd.c（节选）
static const struct file_operations timerfd_fops = {
    .release    = timerfd_release,
    .poll       = timerfd_poll,
    .read_iter  = timerfd_read_iter,
    .llseek     = noop_llseek,
    .show_fdinfo = timerfd_show,
    .unlocked_ioctl = timerfd_ioctl,
};
```

`poll` 在 `ctx->ticks != 0` 时返回 `EPOLLIN`，这是 timerfd 能与 epoll 配合的关键。

---

## 6. 错误码

| errno | 触发条件 |
|-------|----------|
| `EINVAL` | `clockid` 不受支持，或 `flags` 含未知位 |
| `EPERM` | 使用 `_ALARM` 时钟但没有 `CAP_WAKE_ALARM` |
| `EMFILE` / `ENFILE` | 文件描述符或系统文件表耗尽 |
| `ENOMEM` | 内存不足 |

---

## 7. 使用示例

```c
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <unistd.h>

int main(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd == -1) {
        perror("timerfd_create");
        return 1;
    }

    struct itimerspec its = {
        .it_value    = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 },
        .it_interval = { .tv_sec = 0, .tv_nsec = 250 * 1000 * 1000 },
    };
    timerfd_settime(fd, 0, &its, NULL);

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    for (int i = 0; i < 3; i++) {
        if (poll(&pfd, 1, 2000) <= 0) {
            perror("poll");
            break;
        }
        uint64_t expirations;
        if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
            perror("read");
            break;
        }
        printf("expired %llu time(s)\n",
               (unsigned long long)expirations);
    }

    close(fd);
    return 0;
}
```

### 7.1 与 POSIX 定时器的取舍

| 维度 | `timer_create` | `timerfd_create` |
|------|----------------|------------------|
| 通知方式 | 信号 | fd 可读（poll/epoll） |
| 多线程 | 需处理信号投递目标 | 天然可分发到事件循环 |
| 适用场景 | 传统信号驱动程序 | 网络/事件驱动服务 |

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `fs/timerfd.c:424` | `SYSCALL_DEFINE2(timerfd_create)` |
| `fs/timerfd.c:567` | `SYSCALL_DEFINE4(timerfd_settime)` |
| `fs/timerfd.c:585` | `SYSCALL_DEFINE2(timerfd_gettime)` |
| `fs/timerfd.c:300` | `timerfd_read_iter()` |
| `fs/timerfd.c:284` | `timerfd_poll()` |
| `fs/timerfd.c:31` | `struct timerfd_ctx` |
| `include/uapi/linux/timerfd.h` | `TFD_*` 标志 |
| `include/linux/timerfd.h` | `TFD_CREATE_FLAGS` |

相关文档：[timer_create](timer_create.md)、[timer_settime](timer_settime.md)、[clock_nanosleep](clock_nanosleep.md)。
