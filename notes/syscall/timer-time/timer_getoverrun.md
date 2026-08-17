# timer_getoverrun 系统调用分析

## 1. 概述

`timer_getoverrun` 用于获取 POSIX 间隔定时器（interval timer）的**溢出计数**（overrun count）。当定时器到期频率高于信号递送（signal delivery）频率时，多个定时器到期事件会被合并，溢出计数记录被合并的到期次数。

**关键特性：**
- 返回值是相对于最近一次递送的信号的溢出计数
- 仅在信号处理函数中调用才有意义
- 溢出计数上限为 `INT_MAX`
- 信号递送时，`posixtimer_deliver_signal` 会计算并缓存溢出计数

**内核源码位置：** `kernel/time/posix-timers.c:792`

---

## 2. 函数原型

```c
// kernel/time/posix-timers.c:792
SYSCALL_DEFINE1(timer_getoverrun, timer_t, timer_id)
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `timer_id` | `timer_t` | 由 `timer_create` 返回的定时器 ID |

### 返回值

| 返回值 | 说明 |
|--------|------|
| `1..INT_MAX` | 与最近一次递送信号相关的溢出计数 |
| `-EINVAL` | `timer_id` 无效 |

---

## 3. 使用场景

### 3.1 定时器溢出

当 POSIX 间隔定时器使用信号通知（`SIGEV_SIGNAL`）时，如果定时器到期频率很高，而信号处理函数执行较慢，多次定时器到期事件可能被合并为一次信号递送。溢出计数表示：
- 如果定时器每 1ms 到期一次，但信号处理函数耗时 5ms，则每次信号递送时溢出计数为 4（表示有 4 次到期被合并）

### 3.2 典型用法

```c
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static timer_t timer_id;

static void handler(int sig, siginfo_t *si, void *uc)
{
    int overrun = timer_getoverrun(timer_id);
    printf("Timer fired, overrun count: %d\n", overrun);
}

int main(void)
{
    struct sigaction sa;
    struct sigevent sev;
    struct itimerspec its;

    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
    sev.sigev_value.sival_ptr = &timer_id;
    timer_create(CLOCK_MONOTONIC, &sev, &timer_id);

    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 1000000;  // 1ms 间隔
    its.it_value.tv_sec = 1;
    its.it_value.tv_nsec = 0;
    timer_settime(timer_id, 0, &its, NULL);

    pause();
    return 0;
}
```

---

## 4. 函数调用链

```
timer_getoverrun(timer_id)                          // kernel/time/posix-timers.c:792
  └─ scoped_timer_get_or_fail(timer_id)             // 通过 scoped guard 获取定时器锁
       └─ lock_timer(timer_id)                      // 上锁并查找定时器
            ├─ posix_timer_by_id(timer_id)          // 哈希查找 k_itimer
            │    └─ hash_bucket(sig, id)            // 计算哈希桶（jhash2）
            │    └─ hlist_for_each_entry_rcu(...)   // 遍历哈希链表
            │         └─ 匹配 timer->it_id == id
            ├─ spin_lock_irq(&timer->it_lock)       // 获取定时器自旋锁
            └─ [失败] → 返回 NULL → 系统调用返回 -EINVAL
       └─ timer_overrun_to_int(scoped_timer)        // 安全转换溢出计数
            └─ timr->it_overrun_last > INT_MAX ?
                 → INT_MAX : (int)timr->it_overrun_last
```

### 内核源码

```c
// kernel/time/posix-timers.c:792
SYSCALL_DEFINE1(timer_getoverrun, timer_t, timer_id)
{
    scoped_timer_get_or_fail(timer_id)
        return timer_overrun_to_int(scoped_timer);
}

// kernel/time/posix-timers.c:283
static inline int timer_overrun_to_int(struct k_itimer *timr)
{
    if (timr->it_overrun_last > (s64)INT_MAX)
        return INT_MAX;
    return (int)timr->it_overrun_last;
}
```

---

## 5. 关键数据结构

### 5.1 内核定时器结构体

```c
// kernel/time/posix-timers.h
struct k_itimer {
    struct hlist_node      t_hash;             // 哈希链表节点（按 signal_struct 哈希）
    struct list_head       t_entry;            // 进程定时器链表节点
    struct signal_struct   *it_signal;         // 所属进程信号结构
    clockid_t              it_clock;           // 时钟 ID（CLOCK_MONOTONIC 等）
    timer_t                it_id;              // 定时器 ID（用户空间标识）
    int                    it_active;          // 定时器是否激活
    s64                    it_overrun;         // 累计溢出计数（未递送信号的部分）
    s64                    it_overrun_last;    // 最近一次递送信号时的溢出计数
    int                    it_requeue_pending; // 重排队等待标志
    int                    it_signal_seq;      // 信号序列号（用于检测竞争）
    union {
        struct {
            struct hrtimer timer;              // 高精度定时器
            ktime_t        interval;           // 间隔时间（HRT 模式）
        } real;
        struct alarm        alarm;             // 闹钟定时器（CLOCK_REALTIME_ALARM）
        struct {
            struct task_struct *task;          // 绑定 CPU 的定时器任务
            struct hrtimer  timer;             // CPU 定时器
        } cpu;
    } it;
    struct sigqueue        sigq;               // 信号队列项（信号递送用）
    struct rcu_head        rcu;                // RCU 回调
};
```

### 5.2 溢出计数相关字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `it_overrun` | `s64` | 累计溢出计数，`common_hrtimer_rearm` 中通过 `hrtimer_forward_now` 递增 |
| `it_overrun_last` | `s64` | 上次信号递送时的溢出计数快照，`posixtimer_deliver_signal` 中设置 |

### 5.3 溢出计数生命周期

```
timer_settime()  → it_overrun = 0, it_overrun_last = 0
定时器到期       → common_hrtimer_rearm → it_overrun += hrtimer_forward_now()
信号递送         → posixtimer_deliver_signal → it_overrun_last = it_overrun, it_overrun = 0
timer_getoverrun → it_overrun_last 返回给用户
```

---

## 6. 溢出计数计算流程

### 6.1 定时器重排（到期时）

```c
// kernel/time/posix-timers.c:291
static void common_hrtimer_rearm(struct k_itimer *timr)
{
    struct hrtimer *timer = &timr->it.real.timer;

    timr->it_overrun += hrtimer_forward_now(timer, timr->it_interval);
    hrtimer_restart(timer);
}
```

`hrtimer_forward_now` 计算自上次到期以来应该经过的间隔数，累加到 `it_overrun`。

### 6.2 信号递送时

```c
// kernel/time/posix-timers.c:329
bool posixtimer_deliver_signal(struct kernel_siginfo *info,
                               struct sigqueue *timer_sigq)
{
    struct k_itimer *timr = container_of(timer_sigq, struct k_itimer, sigq);
    // ...
    info->si_overrun = timer_overrun_to_int(timr);
    return true;
}
```

---

## 7. 完整流程图

```
用户态                              内核态
  |                                   |
  |  timer_getoverrun(timer_id)       |
  |  ─────────────────────────────>   |
  |                                   |
  |                                   |  1. scoped_timer_get_or_fail()
  |                                   |     ├─ posix_timer_by_id() 哈希查找
  |                                   |     └─ spin_lock_irq(&timer->it_lock)
  |                                   |
  |                                   |  2. timer_overrun_to_int(timr)
  |                                   |     └─ 读取 timr->it_overrun_last
  |                                   |        (上限 INT_MAX)
  |                                   |
  |                                   |  3. [scoped guard 自动释放锁]
  |                                   |
  |  ← 返回 overrun_count (int)       |
  |  (或 -EINVAL)                     |
  |                                   |
```

---

## 8. 错误处理

| 错误码 | 条件 | 说明 |
|--------|------|------|
| `-EINVAL` | `timer_id` 无效 | 定时器 ID 在哈希表中未找到，或已被删除 |

---

## 9. 注意事项

1. **溢出计数的语义：** 返回值是相对于最近一次递送的信号的溢出计数，不是当前累计值。在信号处理函数之外调用，返回值可能没有意义。

2. **上限 INT_MAX：** `timer_overrun_to_int` 将溢出计数截断到 `INT_MAX`，防止 64 位到 32 位的转换溢出。

3. **与 `timer_gettime` 的区别：**
   - `timer_gettime`：返回定时器到期前的剩余时间
   - `timer_getoverrun`：返回定时器到期间被合并的次数

4. **仅限间隔定时器：** 一次性定时器（`it_interval` 为 0）的溢出计数始终为 0。

5. **线程安全：** `scoped_timer_get_or_fail` 通过 `spin_lock_irq` 保护对 `k_itimer` 的访问，同时使用 RCU 保护哈希查找。

---

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#定时器与时间)
- 内核源码：`kernel/time/posix-timers.c`
- 内核头文件：`kernel/time/posix-timers.h`
- man 手册：`timer_getoverrun(2)`