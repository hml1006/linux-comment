# setitimer 系统调用分析

## 1. 概述

`setitimer` 系统调用用于设置旧的间隔定时器（interval timer）。这是 POSIX 定时器出现之前的旧式定时器接口，属于 System V / BSD 传统接口。

**原型：**

```c
SYSCALL_DEFINE3(setitimer, int, which, struct __kernel_old_itimerval __user *, value,
                struct __kernel_old_itimerval __user *, ovalue);
```

| 参数 | 类型 | 描述 |
|--|--|--|
| `which` | `int` | 定时器类型（ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF） |
| `value` | `const struct itimerval __user *` | 新的定时器值（可以为 NULL，此时行为是设置定时器为 0） |
| `ovalue` | `struct itimerval __user *` | 输出参数，接收旧的定时器设置（可以为 NULL） |

### 返回值

- 成功时返回 0
- `which` 无效时返回 `-EINVAL`
- 用户空间地址无效时返回 `-EFAULT`

## 2. 完整实现

```c
// kernel/time/itimer.c
SYSCALL_DEFINE3(setitimer, int, which, struct __kernel_old_itimerval __user *, value,
                struct __kernel_old_itimerval __user *, ovalue)
{
    struct itimerspec64 set_buffer, get_buffer;
    int error;

    if (value) {
        // 从用户空间读取定时器值
        error = get_itimerval(&set_buffer, value);
        if (error)
            return error;
    } else {
        // value 为 NULL 时，设置为 0（禁用定时器）
        memset(&set_buffer, 0, sizeof(set_buffer));
        printk_once(KERN_WARNING "%s calls setitimer() with new_value NULL pointer."
                    " Misfeature support will be removed\n",
                    current->comm);
    }

    error = do_setitimer(which, &set_buffer, ovalue ? &get_buffer : NULL);
    if (error || !ovalue)
        return error;
    if (put_itimerval(ovalue, &get_buffer))
        return -EFAULT;
    return 0;
}
```

### do_setitimer 核心实现

```c
static int do_setitimer(int which, struct itimerspec64 *value,
                        struct itimerspec64 *ovalue)
{
    struct task_struct *tsk = current;
    struct hrtimer *timer;
    ktime_t expires;

    switch (which) {
    case ITIMER_REAL:
again:
        spin_lock_irq(&tsk->sighand->siglock);
        timer = &tsk->signal->real_timer;
        
        if (ovalue) {
            // 获取旧定时器的剩余时间
            ovalue->it_value = itimer_get_remtime(timer);
            ovalue->it_interval = ktime_to_timespec64(tsk->signal->it_real_incr);
        }
        
        // 取消当前定时器（可能正在运行，需要处理竞争）
        if (hrtimer_try_to_cancel(timer) < 0) {
            spin_unlock_irq(&tsk->sighand->siglock);
            hrtimer_cancel_wait_running(timer);
            goto again;  // 重试
        }
        
        // 设置新定时器
        expires = timespec64_to_ktime(value->it_value);
        if (expires != 0) {
            tsk->signal->it_real_incr = timespec64_to_ktime(value->it_interval);
            hrtimer_start(timer, expires, HRTIMER_MODE_REL);
        } else {
            tsk->signal->it_real_incr = 0;
        }
        
        trace_itimer_state(ITIMER_REAL, value, 0);
        spin_unlock_irq(&tsk->sighand->siglock);
        break;
        
    case ITIMER_VIRTUAL:
        set_cpu_itimer(tsk, CPUCLOCK_VIRT, value, ovalue);
        break;
        
    case ITIMER_PROF:
        set_cpu_itimer(tsk, CPUCLOCK_PROF, value, ovalue);
        break;
        
    default:
        return -EINVAL;
    }
    return 0;
}
```

### set_cpu_itimer

```c
static void set_cpu_itimer(struct task_struct *tsk, unsigned int clock_id,
                           const struct itimerspec64 *const value,
                           struct itimerspec64 *const ovalue)
{
    u64 oval, nval, ointerval, ninterval;
    struct cpu_itimer *it = &tsk->signal->it[clock_id];

    nval = timespec64_to_ns(&value->it_value);
    ninterval = timespec64_to_ns(&value->it_interval);

    spin_lock_irq(&tsk->sighand->siglock);

    oval = it->expires;      // 旧到期时间
    ointerval = it->incr;    // 旧间隔值
    
    if (oval || nval) {
        if (nval > 0)
            nval += TICK_NSEC;  // 加上 1 tick 避免立即到期
        set_process_cpu_timer(tsk, clock_id, &nval, &oval);
    }
    
    it->expires = nval;      // 新到期时间
    it->incr = ninterval;    // 新间隔值
    
    trace_itimer_state(clock_id == CPUCLOCK_VIRT ?
                       ITIMER_VIRTUAL : ITIMER_PROF, value, nval);

    spin_unlock_irq(&tsk->sighand->siglock);

    if (ovalue) {
        ovalue->it_value = ns_to_timespec64(oval);
        ovalue->it_interval = ns_to_timespec64(ointerval);
    }
}
```

## 3. 函数调用栈

```
setitimer(which, value, ovalue)
  │
  ├─ get_itimerval(&set_buffer, value)  // 从用户空间读取定时器值
  │    ├─ copy_from_user()
  │    ├─ 验证 timeval 合法性
  │    └─ 转换为 itimerspec64
  │
  └─ do_setitimer(which, &set_buffer, ovalue ? &get_buffer : NULL)
       │
       ├─ [ITIMER_REAL]:
       │    ├─ hrtimer_try_to_cancel(&real_timer)  // 取消旧定时器
       │    │    └─ 如果无法取消 → 等待后重试
       │    ├─ 设置 it_real_incr
       │    └─ hrtimer_start(timer, expires, HRTIMER_MODE_REL)
       │
       ├─ [ITIMER_VIRTUAL]:
       │    └─ set_cpu_itimer(tsk, CPUCLOCK_VIRT, ...)
       │         ├─ 设置 it[VIRT].expires 和 .incr
       │         └─ set_process_cpu_timer()
       │
       └─ [ITIMER_PROF]:
            └─ set_cpu_itimer(tsk, CPUCLOCK_PROF, ...)
                 └─ 类似 VIRTUAL
  │
  └─ put_itimerval(ovalue, &get_buffer)  // 将旧值写回用户空间
```

## 4. 关键数据结构

```c
// include/uapi/linux/time.h
struct itimerval {
    struct timeval it_interval;  // 定时器间隔
    struct timeval it_value;     // 当前值
};

// 内核内部使用的数据结构
// include/linux/sched/signal.h
struct signal_struct {
    struct hrtimer real_timer;          // ITIMER_REAL 的高精度定时器
    ktime_t it_real_incr;               // ITIMER_REAL 的间隔
    
    struct cpu_itimer {
        u64 expires;    // 到期绝对时间（ns）
        u64 incr;       // 间隔（ns）
    } it[CPUCLOCK_MAX]; // CPUCLOCK_VIRT = 0, CPUCLOCK_PROF = 1
    // ...
};
```

## 5. 流程图

```
用户态调用 setitimer(which, &value, &old_value)
  │
  ▼
get_itimerval(&set_buffer, value)
  │
  ├─ copy_from_user ──失败──→ -EFAULT
  ├─ timeval_valid 检查 ──非法──→ -EINVAL
  └─ 转换到 itimerspec64
  │
  ▼
do_setitimer(which, &set_buffer, &get_buffer)
  │
  ├─ ITIMER_REAL?
  │    │
  │    ├─ 是:
  │    │    ├─ 保存旧值到 ovalue
  │    │    ├─ 取消 hrtimer（可能重试）
  │    │    ├─ 设置新间隔 it_real_incr
  │    │    └─ hrtimer_start(新定时器)
  │    │
  │    └─ 否 (VIRTUAL/PROF):
  │         │
  │         └─ set_cpu_itimer()
  │              ├─ 保存旧值
  │              ├─ 设置新 expires 和 incr
  │              └─ set_process_cpu_timer()
  │
  ▼
put_itimerval(ovalue, &get_buffer)
  │
  ├─ 转换 itimerspec64 → itimerval
  └─ copy_to_user ──失败──→ -EFAULT
       │
       ▼
  返回 0
```

## 6. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>

volatile int timer_count = 0;

void timer_handler(int sig)
{
    timer_count++;
    printf("Timer tick #%d\n", timer_count);
}

int main(void)
{
    struct itimerval timer, old_timer;
    struct sigaction sa;
    
    // 设置信号处理器
    sa.sa_handler = timer_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    sigaction(SIGVTALRM, &sa, NULL);
    
    // 设置 ITIMER_REAL: 1 秒后开始，之后每 500ms 重复
    timer.it_value.tv_sec = 1;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 500000;
    
    if (setitimer(ITIMER_REAL, &timer, &old_timer) == -1) {
        perror("setitimer");
        exit(1);
    }
    
    printf("Old timer: %ld sec\n", old_timer.it_value.tv_sec);
    
    // 运行 3 秒
    sleep(3);
    
    // 清除定时器
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 0;
    setitimer(ITIMER_REAL, &timer, NULL);
    
    printf("Total ticks: %d\n", timer_count);
    return 0;
}
```

## 7. 注意事项

- 如果 `value` 为 NULL，内核会打印警告并禁用定时器（向后兼容行为）
- 如果 `ovalue` 为 NULL，不返回旧值
- ITIMER_REAL 的取消和设置需要处理 `hrtimer` 的并发问题（使用 `hrtimer_try_to_cancel` + 重试）
- CPU 定时器（VIRTUAL/PROF）在设置时加上 `TICK_NSEC` 以避免立即到期
- 定时器值中的 `timeval` 必须合法（`tv_usec` 在 0-999999 范围内，`tv_sec >= 0`）

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 源码位置：`kernel/time/itimer.c`