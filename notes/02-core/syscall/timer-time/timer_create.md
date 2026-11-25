# timer_create 系统调用分析

## 1. 概述

`timer_create` 创建一个 POSIX.1b 间隔定时器（interval timer），并返回一个进程内唯一的定时器 ID。定时器到期时可以：

- 向进程/线程发送信号（`SIGEV_SIGNAL` / `SIGEV_THREAD_ID`）
- 不通知（`SIGEV_NONE`，只用于测量）
- 由 glibc 在用户态用线程实现（`SIGEV_THREAD`，内核不感知）

**关键特性：**

- 定时器 ID 属于**进程**（`signal_struct`），同一进程内所有线程可见
- 新创建的定时器处于“未武装”状态，需 `timer_settime` 才启动
- 定时器与 `clockid` 绑定，`CLOCK_REALTIME` / `CLOCK_MONOTONIC` / CPU 时钟等语义不同
- 内核对象是 `struct k_itimer`，底层用 hrtimer（或 CPU 时钟、alarm）实现

**内核源码位置：** `kernel/time/posix-timers.c:574`

---

## 2. 用户态接口

```c
#include <signal.h>
#include <time.h>

int timer_create(clockid_t clockid, struct sigevent *sevp, timer_t *timerid);
```

| 参数 | 说明 |
|------|------|
| `clockid` | 计时时钟 |
| `sevp` | 到期通知方式；为 `NULL` 时等价于 `SIGEV_SIGNAL + SIGALRM` |
| `timerid` | 输出参数，返回定时器 ID |

### 2.1 sigevent

```c
// include/uapi/asm-generic/siginfo.h:336（节选）
typedef struct sigevent {
    sigval_t sigev_value;       // 随信号传递的值
    int      sigev_signo;       // 信号编号
    int      sigev_notify;      // 通知方式
    union {
        int _pad[SIGEV_PAD_SIZE];
        int _tid;               // SIGEV_THREAD_ID 的目标线程
        struct {
            void (*_function)(sigval_t);
            void *_attribute;
        } _sigev_thread;        // SIGEV_THREAD（glibc 实现）
    } _sigev_un;
} sigevent_t;
```

| `sigev_notify` | 行为 |
|----------------|------|
| `SIGEV_SIGNAL` | 到期向进程发送 `sigev_signo` 信号 |
| `SIGEV_THREAD_ID` | 到期向指定线程发送信号（Linux 扩展） |
| `SIGEV_NONE` | 到期不通知，只能通过 `timer_gettime` 观察 |
| `SIGEV_THREAD` | glibc 创建线程执行回调，内核看到的是 `SIGEV_SIGNAL` |

### 2.2 返回值

| 返回值 | 说明 |
|--------|------|
| `0` | 成功，`timerid` 写入定时器 ID |
| `-1` | 失败，`errno` 见第 6 节 |

---

## 3. 内核实现

### 3.1 系统调用入口

```c
// kernel/time/posix-timers.c:574
SYSCALL_DEFINE3(timer_create, const clockid_t, which_clock,
        struct sigevent __user *, timer_event_spec,
        timer_t __user *, created_timer_id)
{
    if (timer_event_spec) {
        sigevent_t event;

        if (copy_from_user(&event, timer_event_spec, sizeof (event)))
            return -EFAULT;
        return do_timer_create(which_clock, &event, created_timer_id);
    }
    return do_timer_create(which_clock, NULL, created_timer_id);
}
```

入口只负责把 `sigevent` 拷入内核，真正的创建在 `do_timer_create()`。

### 3.2 do_timer_create

```c
// kernel/time/posix-timers.c:466（节选）
static int do_timer_create(clockid_t which_clock, struct sigevent *event,
                           timer_t __user *created_timer_id)
{
    const struct k_clock *kc = clockid_to_kclock(which_clock);
    timer_t req_id = TIMER_ANY_ID;
    struct k_itimer *new_timer;
    int error, new_timer_id;

    if (!kc)
        return -EINVAL;
    if (!kc->timer_create)
        return -EOPNOTSUPP;

    /* Special case for CRIU to restore timers with a given timer ID. */
    if (unlikely(current->signal->timer_create_restore_ids)) {
        if (copy_from_user(&req_id, created_timer_id, sizeof(req_id)))
            return -EFAULT;
        if ((unsigned int)req_id > INT_MAX)
            return -EINVAL;
    }

    new_timer = alloc_posix_timer();
    if (unlikely(!new_timer))
        return -EAGAIN;

    spin_lock_init(&new_timer->it_lock);

    new_timer_id = posix_timer_add(new_timer, req_id);
    if (new_timer_id < 0) {
        posixtimer_free_timer(new_timer);
        return new_timer_id;
    }

    new_timer->it_clock = which_clock;
    new_timer->kclock = kc;
    new_timer->it_overrun = -1LL;

    if (event) {
        scoped_guard (rcu)
            new_timer->it_pid = get_pid(good_sigevent(event));
        if (!new_timer->it_pid) {
            error = -EINVAL;
            goto out;
        }
        new_timer->it_sigev_notify     = event->sigev_notify;
        new_timer->sigq.info.si_signo = event->sigev_signo;
        new_timer->sigq.info.si_value = event->sigev_value;
    } else {
        new_timer->it_sigev_notify     = SIGEV_SIGNAL;
        new_timer->sigq.info.si_signo = SIGALRM;
        new_timer->sigq.info.si_value.sival_int = new_timer->it_id;
        new_timer->it_pid = get_pid(task_tgid(current));
    }

    new_timer->sigq.info.si_tid  = new_timer->it_id;
    new_timer->sigq.info.si_code = SI_TIMER;

    if (copy_to_user(created_timer_id, &new_timer_id, sizeof (new_timer_id))) {
        error = -EFAULT;
        goto out;
    }

    error = kc->timer_create(new_timer);
    if (error)
        goto out;

    /* 完成 it_signal 的发布，使定时器对查找路径可见 */
    scoped_guard (spinlock_irq, &new_timer->it_lock) {
        guard(spinlock)(&current->sighand->siglock);
        WRITE_ONCE(new_timer->it_signal, current->signal);
        hlist_add_head_rcu(&new_timer->list, &current->signal->posix_timers);
    }
    return 0;
out:
    posix_timer_unhash_and_free(new_timer);
    return error;
}
```

关键步骤：

1. `alloc_posix_timer()` 从 `posix_timers_cache` 分配 `k_itimer`，并初始化内嵌 `sigqueue` 的引用计数；
2. `posix_timer_add()` 在按 `signal_struct` 哈希的表中分配唯一 ID；
3. `good_sigevent()` 校验通知方式与信号范围，并解析目标进程/线程；
4. `kc->timer_create()` 完成时钟相关初始化（普通 POSIX 定时器是 `common_timer_create()` → `hrtimer_setup()`）；
5. 最后在 `it_lock` + `sighand->siglock` 保护下写入 `it_signal` 并挂入 `sig->posix_timers`，定时器从此刻起才“有效”。

### 3.3 时钟相关初始化

```c
// kernel/time/posix-timers.c:459
static int common_timer_create(struct k_itimer *new_timer)
{
    hrtimer_setup(&new_timer->it.real.timer, posix_timer_fn,
                  new_timer->it_clock, 0);
    return 0;
}
```

CPU 时钟定时器把 `it` 联合体解释为 `struct cpu_timer`，alarm 时钟使用 `struct alarm`，但接口统一由 `k_clock` 回调封装。

---

## 4. 调用链

```text
用户态 timer_create(clockid, sevp, &timerid)
  │
  ▼
sys_timer_create(which_clock, timer_event_spec, created_timer_id)
  ├─ copy_from_user(&event, sevp)              // sevp != NULL
  └─ do_timer_create(which_clock, &event, created_timer_id)
       ├─ clockid_to_kclock(which_clock)
       ├─ alloc_posix_timer()
       │    ├─ kmem_cache_zalloc(posix_timers_cache)
       │    └─ posixtimer_init_sigqueue()
       ├─ posix_timer_add(new_timer, req_id)
       │    └─ hash_bucket() → 分配到 signal_struct 的哈希表
       ├─ good_sigevent(event)
       │    └─ find_vpid() / pid_task()          // SIGEV_THREAD_ID
       ├─ copy_to_user(created_timer_id, &new_timer_id)
       ├─ kc->timer_create(new_timer)
       │    └─ common_timer_create()
       │         └─ hrtimer_setup(posix_timer_fn)
       └─ it_signal 发布
            └─ hlist_add_head_rcu(&timer->list, &sig->posix_timers)
```

创建完成后，定时器尚未武装；`timer_settime` 才会把 hrtimer 插入红黑树并启动。

---

## 5. 关键数据结构

```c
// include/linux/posix-timers.h:186（节选）
struct k_itimer {
    /* 第一缓存行：读多写少 */
    struct hlist_node       t_hash;          // 定时器哈希表节点
    struct hlist_node       list;            // sig->posix_timers 链表
    timer_t                 it_id;           // 定时器 ID
    clockid_t               it_clock;        // 绑定的时钟
    int                     it_sigev_notify; // SIGEV_*
    enum pid_type           it_pid_type;     // PIDTYPE_PID / PIDTYPE_TGID
    struct signal_struct    *it_signal;      // 所属进程
    const struct k_clock    *kclock;         // 时钟操作表

    /* 第二缓存行起：频繁修改 */
    spinlock_t              it_lock;
    int                     it_status;       // DISARMED / ARMED / REQUEUE_PENDING
    s64                     it_overrun;      // 超限次数
    s64                     it_overrun_last;
    ktime_t                 it_interval;     // 周期
    union {
        struct pid          *it_pid;
        struct task_struct  *it_process;
    };
    struct sigqueue         sigq;            // 到期信号
    rcuref_t                rcuref;          // 生命周期引用
    union {
        struct { struct hrtimer timer; } real;
        struct cpu_timer    cpu;
        struct { struct alarm alarmtimer; } alarm;
    } it;
    struct rcu_head         rcu;
} ____cacheline_aligned_in_smp;
```

`____cacheline_aligned_in_smp` 保证每个 `k_itimer` 从缓存行边界开始，减少多核伪共享。

---

## 6. 错误码

| errno | 触发条件 |
|-------|----------|
| `EINVAL` | `clockid` 非法；`sigevent` 通知方式/信号编号非法 |
| `EOPNOTSUPP` | 该时钟不支持创建定时器（如 `CLOCK_MONOTONIC_RAW`） |
| `EAGAIN` | 系统限制（RLIMIT_SIGPENDING）或内存不足，无法分配定时器 |
| `EFAULT` | `sevp` / `timerid` 指针不可访问 |
| `ENOMEM` | 内核内存不足 |

---

## 7. 使用示例

```c
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t expired;

static void handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    expired = 1;
    printf("timer fired, value=%d\n", si->si_value.sival_int);
}

int main(void)
{
    timer_t tid;
    struct sigevent sev;
    struct sigaction sa;
    struct itimerspec its;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN, &sa, NULL);

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_int = 42;

    if (timer_create(CLOCK_MONOTONIC, &sev, &tid) == -1) {
        perror("timer_create");
        return 1;
    }

    its.it_value.tv_sec = 1;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;      // 单次定时器
    timer_settime(tid, 0, &its, NULL);

    while (!expired)
        pause();

    timer_delete(tid);
    return 0;
}
```

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `kernel/time/posix-timers.c:574` | `SYSCALL_DEFINE3(timer_create)` |
| `kernel/time/posix-timers.c:466` | `do_timer_create()` |
| `kernel/time/posix-timers.c:423` | `alloc_posix_timer()` |
| `kernel/time/posix-timers.c:157` | `posix_timer_add()` |
| `kernel/time/posix-timers.c:399` | `good_sigevent()` |
| `kernel/time/posix-timers.c:459` | `common_timer_create()` |
| `include/linux/posix-timers.h:186` | `struct k_itimer` |
| `kernel/time/posix-timers.h:10` | `struct k_clock` |
| `include/uapi/asm-generic/siginfo.h:336` | `struct sigevent` |

相关文档：[timer_settime](timer_settime.md)、[timer_gettime](timer_gettime.md)、[timer_delete](timer_delete.md)、[timerfd_create](timerfd_create.md)。
