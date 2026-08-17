# getitimer 系统调用分析

## 1. 概述

`getitimer` 系统调用用于获取旧的间隔定时器（interval timer）的值。这是 POSIX 定时器出现之前的旧式定时器接口，属于 System V / BSD 传统接口。

**原型：**

```c
SYSCALL_DEFINE2(getitimer, int, which, struct __kernel_old_itimerval __user *, value);
```

| 参数 | 类型 | 描述 |
|--|--|--|
| `which` | `int` | 定时器类型（ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF） |
| `value` | `struct __kernel_old_itimerval __user *` | 输出参数，接收当前定时器设置 |

### 返回值

- 成功时返回 0
- `which` 无效时返回 `-EINVAL`
- 用户空间地址无效时返回 `-EFAULT`

## 2. 定时器类型

```c
// include/uapi/linux/time.h
#define ITIMER_REAL    0    // 实时定时器，到期发送 SIGALRM
#define ITIMER_VIRTUAL 1    // 虚拟定时器（进程用户态 CPU 时间），到期发送 SIGVTALRM
#define ITIMER_PROF    2    // 分析定时器（进程用户+内核态 CPU 时间），到期发送 SIGPROF
```

### 定时器差异

| 定时器 | 计时方式 | 到期信号 | 使用场景 |
|--|--|--|--|
| `ITIMER_REAL` | 墙上时钟（实时） | `SIGALRM` | 超时控制、定时任务 |
| `ITIMER_VIRTUAL` | 仅用户态 CPU 时间 | `SIGVTALRM` | 用户态代码执行时间 profiling |
| `ITIMER_PROF` | 用户态+内核态 CPU 时间 | `SIGPROF` | 代码性能分析（profiling） |

## 3. 完整实现

```c
// kernel/time/itimer.c
SYSCALL_DEFINE2(getitimer, int, which, struct __kernel_old_itimerval __user *, value)
{
    struct itimerspec64 get_buffer;
    int error = do_getitimer(which, &get_buffer);

    if (!error && put_itimerval(value, &get_buffer))
        error = -EFAULT;
    return error;
}
```

### do_getitimer 核心实现

```c
static int do_getitimer(int which, struct itimerspec64 *value)
{
    struct task_struct *tsk = current;

    switch (which) {
    case ITIMER_REAL:
        // 获取实时定时器剩余时间
        spin_lock_irq(&tsk->sighand->siglock);
        value->it_value = itimer_get_remtime(&tsk->signal->real_timer);
        value->it_interval = ktime_to_timespec64(tsk->signal->it_real_incr);
        spin_unlock_irq(&tsk->sighand->siglock);
        break;
        
    case ITIMER_VIRTUAL:
        // 获取虚拟定时器剩余时间
        get_cpu_itimer(tsk, CPUCLOCK_VIRT, value);
        break;
        
    case ITIMER_PROF:
        // 获取分析定时器剩余时间
        get_cpu_itimer(tsk, CPUCLOCK_PROF, value);
        break;
        
    default:
        return -EINVAL;
    }
    return 0;
}
```

### 获取 CPU 定时器

```c
static void get_cpu_itimer(struct task_struct *tsk, unsigned int clock_id,
                           struct itimerspec64 *const value)
{
    u64 val, interval;
    struct cpu_itimer *it = &tsk->signal->it[clock_id];

    spin_lock_irq(&tsk->sighand->siglock);

    val = it->expires;       // 到期时间
    interval = it->incr;     // 间隔值
    if (val) {
        u64 t, samples[CPUCLOCK_MAX];

        thread_group_sample_cputime(tsk, samples);
        t = samples[clock_id];  // 当前已使用的 CPU 时间

        if (val < t)
            // 已过期，返回 1 tick
            val = TICK_NSEC;
        else
            val -= t;  // 剩余时间 = 到期时间 - 已用时间
    }

    spin_unlock_irq(&tsk->sighand->siglock);

    value->it_value = ns_to_timespec64(val);
    value->it_interval = ns_to_timespec64(interval);
}
```

## 4. 函数调用栈

```
getitimer(which, value)
  │
  └─ do_getitimer(which, &get_buffer)
       │
       ├─ [ITIMER_REAL]:
       │    ├─ spin_lock_irq(&siglock)
       │    ├─ itimer_get_remtime(&real_timer)  // hrtimer 剩余时间
       │    ├─ ktime_to_timespec64(it_real_incr) // 间隔值
       │    └─ spin_unlock_irq(&siglock)
       │
       ├─ [ITIMER_VIRTUAL]:
       │    └─ get_cpu_itimer(tsk, CPUCLOCK_VIRT, value)
       │         ├─ 读取 it[CPUCLOCK_VIRT].expires
       │         ├─ thread_group_sample_cputime()  // 获取线程组 CPU 时间
       │         └─ 计算剩余时间
       │
       └─ [ITIMER_PROF]:
            └─ get_cpu_itimer(tsk, CPUCLOCK_PROF, value)
                 └─ 类似 ITIMER_VIRTUAL，但使用 CPUCLOCK_PROF
  │
  └─ put_itimerval(value, &get_buffer)  // 将内核格式转换为用户态格式
       └─ copy_to_user()  ──失败──→ 返回 -EFAULT
```

## 5. 关键数据结构

```c
// include/uapi/linux/time.h
struct itimerval {
    struct timeval it_interval;  // 定时器间隔
    struct timeval it_value;     // 当前值（剩余时间）
};

struct timeval {
    __kernel_time_t tv_sec;      // 秒
    __kernel_suseconds_t tv_usec;// 微秒
};

// 内核内部使用的 64 位版本
struct itimerspec64 {
    struct timespec64 it_interval;
    struct timespec64 it_value;
};

// 进程信号相关数据结构
// include/linux/sched/signal.h
struct signal_struct {
    struct hrtimer real_timer;          // ITIMER_REAL 的 hrtimer
    ktime_t it_real_incr;               // ITIMER_REAL 的间隔值
    
    struct cpu_itimer it[CPUCLOCK_MAX]; // ITIMER_VIRTUAL 和 ITIMER_PROF
    // ...
};

struct cpu_itimer {
    u64 expires;    // 到期时间（ns）
    u64 incr;       // 间隔值（ns）
};
```

## 6. 流程图

```
用户态调用 getitimer(which, &value)
  │
  ▼
SYSCALL_DEFINE2(getitimer, which, value)
  │
  ▼
do_getitimer(which, &get_buffer)
  │
  ├─ 检查 which
  │    │
  │    ├─ 无效 ──→ 返回 -EINVAL
  │    │
  │    ├─ ITIMER_REAL
  │    │    │
  │    │    └─ 读取 signal->real_timer 剩余时间
  │    │         └─ 读取 signal->it_real_incr
  │    │
  │    ├─ ITIMER_VIRTUAL
  │    │    │
  │    │    └─ get_cpu_itimer(CPUCLOCK_VIRT)
  │    │         ├─ 读取 it[VIRT].expires
  │    │         ├─ 读取线程组虚拟 CPU 时间
  │    │         └─ 计算剩余
  │    │
  │    └─ ITIMER_PROF
  │         │
  │         └─ get_cpu_itimer(CPUCLOCK_PROF)
  │              └─ 类似虚拟定时器
  │
  ▼
put_itimerval(value, &get_buffer)
  │
  ├─ ns_to_timespec64 → timeval 转换
  │
  └─ copy_to_user()  ──失败──→ 返回 -EFAULT
       │
       ▼
  返回 0 (成功)
```

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <signal.h>

void timer_handler(int sig)
{
    printf("Timer expired! (SIGALRM)\n");
}

int main(void)
{
    struct itimerval timer, old_timer;
    struct sigaction sa;
    
    // 设置信号处理器
    sa.sa_handler = timer_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    
    // 设置定时器：1 秒后到期，之后每 2 秒重复
    timer.it_value.tv_sec = 1;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 2;
    timer.it_interval.tv_usec = 0;
    
    setitimer(ITIMER_REAL, &timer, NULL);
    
    // 获取当前定时器状态
    getitimer(ITIMER_REAL, &old_timer);
    printf("Timer: %ld sec %ld usec (interval: %ld sec %ld usec)\n",
           old_timer.it_value.tv_sec, old_timer.it_value.tv_usec,
           old_timer.it_interval.tv_sec, old_timer.it_interval.tv_usec);
    
    // 等待信号
    pause();
    
    return 0;
}
```

## 8. 注意事项

- `getitimer` 是旧式接口，新代码应优先使用 POSIX 定时器（`timer_create`、`timer_gettime` 等）
- 每个进程每种类型的定时器只有一个（`ITIMER_REAL`、`ITIMER_VIRTUAL`、`ITIMER_PROF`）
- ITIMER_REAL 使用高精度定时器（hrtimer），精度为纳秒级
- ITIMER_VIRTUAL 和 ITIMER_PROF 基于进程的 CPU 时间计数，精度受 `TICK_NSEC` 影响
- 定时器值转换为 `timeval`（微秒精度）时可能存在精度损失

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 源码位置：`kernel/time/itimer.c`