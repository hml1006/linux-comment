# timer_delete 系统调用分析

## 1. 概述

`timer_delete` 删除一个由 `timer_create` 创建的 POSIX 间隔定时器，并释放其内核对象。

**关键特性：**

- 删除前会自动解除（disarm）定时器，取消尚未到期的 hrtimer
- 若定时器已到期但信号尚未投递，删除会丢弃该信号
- 定时器 ID 在删除后即可被复用
- 删除操作分两步：先使定时器失效并移出链表，再释放引用；并发查找者仍可能短暂看到该定时器，但会因 `it_signal` 的无效标记而失败
- POSIX 规定删除不存在的定时器返回 `EINVAL`

**内核源码位置：** `kernel/time/posix-timers.c:1067`

---

## 2. 用户态接口

```c
#include <time.h>

int timer_delete(timer_t timerid);
```

| 参数 | 说明 |
|------|------|
| `timerid` | 要删除的定时器 ID |

### 返回值

| 返回值 | 说明 |
|--------|------|
| `0` | 成功 |
| `-1` | 失败，`errno = EINVAL`（ID 无效或不属于当前进程） |

---

## 3. 内核实现

### 3.1 系统调用入口

```c
// kernel/time/posix-timers.c:1067
SYSCALL_DEFINE1(timer_delete, timer_t, timer_id)
{
    struct k_itimer *timer;

    scoped_timer_get_or_fail(timer_id) {
        timer = scoped_timer;
        posix_timer_delete(timer);
    }
    /* Remove it from the hash, which frees up the timer ID */
    posix_timer_unhash_and_free(timer);
    return 0;
}
```

`scoped_timer_get_or_fail()` 是内核的 scope guard 宏：在作用域内查找并锁定定时器，失败时直接返回 `-EINVAL`；离开作用域自动释放 `it_lock`。

### 3.2 posix_timer_delete

```c
// kernel/time/posix-timers.c:1029
static void posix_timer_delete(struct k_itimer *timer)
{
    /*
     * 使定时器失效、从链表移除，并清理 ignored 链表中的待处理信号。
     * 失效标记必须在 sighand->siglock 下写入，确保信号代码能看到它。
     */
    timer->it_signal_seq++;

    scoped_guard (spinlock, &current->sighand->siglock) {
        unsigned long sig = (unsigned long)timer->it_signal | 1UL;

        WRITE_ONCE(timer->it_signal, (struct signal_struct *)sig);
        hlist_del_rcu(&timer->list);
        posix_timer_cleanup_ignored(timer);
    }

    while (timer->kclock->timer_del(timer) == TIMER_RETRY) {
        guard(rcu)();
        spin_unlock_irq(&timer->it_lock);
        timer_wait_running(timer);
        spin_lock_irq(&timer->it_lock);
    }
}
```

三个关键动作：

1. **失效标记**：`it_signal` 的最低位被置 1，表示该定时器不可再被查找/投递；`posix_timer_by_id()` 与信号投递路径都会检查这一位。
2. **摘链**：从 `sig->posix_timers` 与可能的 ignored 链表移除，防止后续信号处理再访问。
3. **取消定时器**：调用 `kc->timer_del()`。普通 POSIX 定时器走 `common_timer_del()` → `hrtimer_try_to_cancel()`；若到期回调正在运行则返回 `TIMER_RETRY`，此时释放锁、等待回调结束、重试。

### 3.3 释放对象

```c
// kernel/time/posix-timers.c:450
static void posix_timer_unhash_and_free(struct k_itimer *tmr)
{
    struct timer_hash_bucket *bucket =
        hash_bucket(posix_sig_owner(tmr), tmr->it_id);

    scoped_guard (spinlock, &bucket->lock)
        hlist_del_rcu(&tmr->t_hash);
    posixtimer_putref(tmr);
}
```

从按 `signal_struct` 哈希的 ID 表中删除节点，并递减引用计数。最后一个引用释放时 `posixtimer_free_timer()` 归还 `k_itimer` 到 slab 缓存（`kfree_rcu`），定时器 ID 随之可复用。

### 3.4 与进程退出的关系

进程最后一个线程退出时，`exit_itimers()` 会遍历 `sig->posix_timers`，对每个定时器执行类似的删除流程，保证不会泄漏 hrtimer 与信号引用。

---

## 4. 调用链

```text
用户态 timer_delete(timerid)
  │
  ▼
sys_timer_delete(timer_id)
  ├─ scoped_timer_get_or_fail(timer_id)
  │    ├─ posix_timer_by_id(timer_id)          // 哈希查找
  │    └─ lock_timer()                         // 取 it_lock
  ├─ posix_timer_delete(timer)
  │    ├─ it_signal_seq++
  │    ├─ sighand->siglock 下：
  │    │    ├─ it_signal |= 1（失效标记）
  │    │    ├─ hlist_del_rcu(&timer->list)
  │    │    └─ posix_timer_cleanup_ignored()
  │    └─ kc->timer_del(timer)
  │         └─ common_timer_del()
  │              └─ hrtimer_try_to_cancel()
  │                   └─ 若回调运行中 → TIMER_RETRY → 等锁重试
  └─ posix_timer_unhash_and_free(timer)
       ├─ hlist_del_rcu(&timer->t_hash)
       └─ posixtimer_putref()
            └─ 引用归零 → kfree_rcu() 释放 k_itimer
```

---

## 5. 关键数据结构

```c
// include/linux/posix-timers.h:186（节选）
struct k_itimer {
    struct hlist_node    t_hash;      // ID 哈希表节点
    struct hlist_node    list;        // sig->posix_timers 节点
    timer_t              it_id;
    struct signal_struct *it_signal;  // 最低位为“失效”标记
    spinlock_t           it_lock;
    int                  it_status;
    unsigned int         it_signal_seq;
    struct sigqueue      sigq;
    rcuref_t             rcuref;      // 生命周期引用计数
    union {
        struct { struct hrtimer timer; } real;
        struct cpu_timer cpu;
        struct { struct alarm alarmtimer; } alarm;
    } it;
    struct rcu_head      rcu;
};
```

`it_signal` 的最低位复用为无效标记，是理解删除并发控制的关键：删除者置位后，任何持有旧指针的查找者都会判定定时器无效。

---

## 6. 错误码

| errno | 触发条件 |
|-------|----------|
| `EINVAL` | `timerid` 不存在、已被删除，或不属于当前进程 |

`timer_delete` 没有 `EFAULT` 分支——它不接受用户指针。

---

## 7. 使用示例

```c
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    timer_t tid;
    struct sigevent sev;
    struct itimerspec its;

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_NONE;          /* 本例不需要信号 */

    if (timer_create(CLOCK_MONOTONIC, &sev, &tid) == -1) {
        perror("timer_create");
        return 1;
    }

    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 1;
    its.it_interval.tv_nsec = 0;
    timer_settime(tid, 0, &its, NULL);

    /* ... 使用定时器 ... */

    if (timer_delete(tid) == -1) {          /* 删除后 ID 可复用 */
        perror("timer_delete");
        return 1;
    }
    return 0;
}
```

### 7.1 自动删除

POSIX 定时器不会随线程退出自动删除，而是随**进程**退出清理。长时间运行的服务若反复创建定时器，必须显式 `timer_delete`，否则会持续占用 `RLIMIT_SIGPENDING` 配额。

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `kernel/time/posix-timers.c:1067` | `SYSCALL_DEFINE1(timer_delete)` |
| `kernel/time/posix-timers.c:1029` | `posix_timer_delete()` |
| `kernel/time/posix-timers.c:450` | `posix_timer_unhash_and_free()` |
| `kernel/time/posix-timers.c:1021` | `posix_timer_cleanup_ignored()` |
| `kernel/time/posix-timers.c:1085` | `exit_itimers()` |
| `include/linux/posix-timers.h:186` | `struct k_itimer` |

相关文档：[timer_create](timer_create.md)、[timer_settime](timer_settime.md)、[timer_gettime](timer_gettime.md)。
