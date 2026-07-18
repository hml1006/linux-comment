# futex_wait 系统调用分析

## 1. 概述

`futex_wait` 是 futex 的等待操作。当用户空间线程发现锁已被其他线程持有时，调用 `futex_wait` 将自身阻塞。内核会检查用户空间 futex 字的值是否仍等于期望值，如果相等则将当前线程加入等待队列并调度出去；如果不相等则立即返回 `-EWOULDBLOCK`，说明条件已改变，调用者应重试。

futex_wait 有两种实现路径：
- **传统接口**：通过 `futex(FUTEX_WAIT, ...)` 系统调用
- **futex2 接口**：通过 `sys_futex_wait()` 系统调用（`FUTEX2_*` 标志）

## 2. 函数原型

### 传统接口

```c
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>

long futex_wait_result = syscall(SYS_futex,
    u32 *uaddr,          // futex 用户空间地址
    FUTEX_WAIT,          // 操作码
    u32 val,             // 期望值
    const struct timespec *timeout,  // 可选超时（绝对时间）
    NULL,                // uaddr2（未使用）
    0);                  // val3（未使用）
```

### futex2 接口

```c
#include <linux/futex.h>
#include <sys/syscall.h>

long futex_wait_result = syscall(SYS_futex_wait,
    void *uaddr,                   // futex 用户空间地址
    unsigned long val,             // 期望值
    unsigned long mask,            // 位掩码（FUTEX_BITSET_MATCH_ANY 表示匹配所有位）
    unsigned int flags,            // FUTEX2 标志
    struct __kernel_timespec *timeout,  // 可选超时（绝对时间）
    clockid_t clockid);            // 时钟类型（CLOCK_MONOTONIC 或 CLOCK_REALTIME）
```

## 3. 详细调用链

```
sys_futex_wait(uaddr, val, mask, flags, timeout, clockid)  // kernel/futex/syscalls.c
  ├─ 检查 flags 有效性
  ├─ futex2_to_flags(flags)                                 // 转换 FUTEX2 标志为内核标志
  ├─ futex_validate_input(flags, val)                       // 验证 val 值合法性
  ├─ futex2_setup_timeout(timeout, clockid, &to)            // 设置超时定时器
  └─ __futex_wait(uaddr, flags, val, timeout ? &to : NULL, mask)
       └─ futex_wait_setup(uaddr, val, flags, &q, NULL, current)  // 等待设置
            ├─ get_futex_key(uaddr, flags, &q->key, FUTEX_READ)   // 获取 futex 键
            ├─ futex_get_value_locked(&uval, uaddr)               // 读取用户空间值
            ├─ [uval != val] → return -EWOULDBLOCK                // 值已改变
            ├─ futex_queue(&q, hb, task)                          // 入队
            │    └─ __futex_queue(q, hb, task)                    // 加入哈希桶的 plist
            │         └─ plist_add(&q->list, &hb->chain)          // 按优先级加入
            └─ return 0
       └─ futex_do_wait(&q, to)                                   // 等待
            ├─ hrtimer_sleeper_start_expires(to, HRTIMER_MODE_ABS) // 启动定时器
            ├─ [未出队] schedule()                                 // 让出 CPU
            └─ __set_current_state(TASK_RUNNING)                  // 恢复运行
       ├─ futex_unqueue(&q)                                       // 尝试出队
       │    └─ [已出队] → return 0 表示被唤醒
       ├─ [超时] → return -ETIMEDOUT
       ├─ [信号] → return -ERESTARTSYS（可重启）
       └─ [虚假唤醒] → goto retry
```

## 4. 核心函数详解

### 4.1 futex_wait_setup

```c
// kernel/futex/core.c
/*
 * futex_wait_setup() - 准备等待 futex
 * 在持有哈希桶锁的情况下，原子性地：
 * 1. 获取 futex 键
 * 2. 读取用户空间值
 * 3. 验证值与期望值一致
 * 4. 将等待者入队
 */
int futex_wait_setup(u32 __user *uaddr, u32 val, unsigned int flags,
                     struct futex_q *q, union futex_key *key2,
                     struct task_struct *task)
{
    // ...
    ret = get_futex_key(uaddr, flags, &q->key, FUTEX_READ);
    // ...
    ret = futex_get_value_locked(&uval, uaddr);
    // ...
    if (uval != val)
        return -EWOULDBLOCK;
    // ...
    futex_queue(q, hb, task);
    return 0;
}
```

### 4.2 futex_do_wait

```c
// kernel/futex/waitwake.c
/*
 * futex_do_wait() - 等待被唤醒、超时或信号
 * 使用 hrtimer 实现超时，调用 schedule() 让出 CPU
 */
void futex_do_wait(struct futex_q *q, struct hrtimer_sleeper *timeout)
{
    if (timeout)
        hrtimer_sleeper_start_expires(timeout, HRTIMER_MODE_ABS);
    if (likely(!plist_node_empty(&q->list))) {
        if (!timeout || timeout->task)
            schedule();
    }
    __set_current_state(TASK_RUNNING);
}
```

### 4.3 重试机制

```c
// kernel/futex/waitwake.c
/*
 * __futex_wait() 使用 retry 标签实现重试循环
 * 处理虚假唤醒：被唤醒但未出队且无信号等待时重新等待
 */
int __futex_wait(u32 __user *uaddr, unsigned int flags, u32 val,
                 struct hrtimer_sleeper *to, u32 bitset)
{
    struct futex_q q = futex_q_init;
    int ret;

    if (!bitset)
        return -EINVAL;
    q.bitset = bitset;

retry:
    ret = futex_wait_setup(uaddr, val, flags, &q, NULL, current);
    if (ret)
        return ret;

    futex_do_wait(&q, to);

    if (!futex_unqueue(&q))
        return 0;  // 被唤醒（已出队）

    if (to && !to->task)
        return -ETIMEDOUT;

    if (!signal_pending(current))
        goto retry;  // 虚假唤醒，重试

    return -ERESTARTSYS;
}
```

## 5. 流程图

```
futex_wait 调用流程:
=============

用户态                          内核态
   |                              |
   | syscall(SYS_futex_wait, ...) |
   |----------------------------->|
   |                              |
   |                          futex_wait_setup:
   |                            ├─ get_futex_key()    // 获取键
   |                            ├─ locked_read(uaddr)  // 读用户值
   |                            ├─ if (uval != val)    // 值已变？
   |                            │    └─ return -EWOULDBLOCK
   |                            ├─ futex_queue()       // 入队
   |                            └─ spin_unlock()       // 释放锁
   |                              |
   |                          futex_do_wait:
   |                            ├─ hrtimer_start()     // 启动定时器
   |                            ├─ schedule()          // 睡眠
   |                              |
   |     <--- 被唤醒/超时/信号 ---|
   |                              |
   |                          futex_unqueue():
   |                            ├─ [已出队] → 被唤醒
   |                            ├─ [超时]   → -ETIMEDOUT
   |                            └─ [信号]   → -ERESTARTSYS
   |                              |
   |        return (0 / 错误码)   |
   |<-----------------------------|
   |                              |
```

## 6. 等待队列操作

```
futex 哈希桶结构:
=================

   futex_hash_bucket[]          plist (优先级排序)
   ┌──────────────┐
   │  chain  │────┼──→ ┌──────────┐  ┌──────────┐  ┌──────────┐
   │  lock    │    │    │ futex_q  │  │ futex_q  │  │ futex_q  │
   │  waiters │    │    │ (最高优)  │→│ (次高优)  │→│ (最低优)  │
   └──────────────┘    └──────────┘  └──────────┘  └──────────┘
                           │              │             │
                          task          task           task

哈希键匹配规则:
   futex_q 的 key 必须与目标 futex 的 key 完全匹配
   共享 futex：inode + pgoff + offset
   私有 futex：mm + uaddr + offset
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EWOULDBLOCK` | futex 值已改变 | `*uaddr != val`，无需等待 |
| `EFAULT` | 用户空间地址错误 | 无法读取 `uaddr` |
| `EINTR` | 被信号中断 | 等待期间收到信号（非可重启） |
| `ERESTARTSYS` | 被信号中断（可重启） | 等待期间收到信号，可重启系统调用 |
| `ETIMEDOUT` | 超时 | 指定时间内未被唤醒 |
| `EINVAL` | 参数无效 | 位掩码为 0、flags 无效、对齐错误 |

## 8. 使用示例

```c
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static int futex_wait_timeout(atomic_int *uaddr, int expected,
                              const struct timespec *timeout) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, expected,
                   timeout, NULL, 0);
}

static int futex_wait_bitset(atomic_int *uaddr, int expected, u32 bitset) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT_BITSET, expected,
                   NULL, NULL, bitset);
}

int main() {
    atomic_int futex_word = 0;
    struct timespec ts = {
        .tv_sec = 2,     // 2 秒超时
        .tv_nsec = 0,
    };

    // 等待 futex 值变为 1，2 秒超时
    int ret = futex_wait_timeout(&futex_word, 1, &ts);
    if (ret == -ETIMEDOUT) {
        printf("等待超时\n");
    } else if (ret == -EWOULDBLOCK) {
        printf("futex 值已改变（当前值 != 期望值）\n");
    } else if (ret == 0) {
        printf("被唤醒\n");
    } else {
        printf("错误: %d\n", ret);
    }

    // 带位掩码的等待 - 只匹配特定标志位
    ret = futex_wait_bitset(&futex_word, 0, 0xFFFFFFFF);
    if (ret == -EWOULDBLOCK) {
        printf("futex 值不是 0\n");
    }

    return 0;
}
```

## 9. 关键要点

1. **竞态条件处理**：`futex_wait_setup` 在持有哈希桶锁的情况下读取用户空间值，确保与入队操作原子性，避免 Waker 在 Waiter 入队前完成 Wake 的经典竞态
2. **内存屏障**：`futex_wait` 使用 `smp_mb()` 确保等待者计数的递增在读值之前完成，与 Waker 侧的 `smp_mb()` 配对
3. **虚假唤醒**：`__futex_wait` 使用 `retry` 循环处理虚假唤醒，只有被信号中断或超时才退出
4. **超时处理**：`FUTEX_WAIT` 使用相对超时（内核自动转换为绝对时间），`FUTEX_WAIT_BITSET` 使用绝对超时
5. **可重启系统调用**：`ERESTARTSYS` 会被信号处理框架自动重启，用户态无需手动重试

## 10. 源码位置

| 文件 | 说明 |
|------|------|
| [kernel/futex/syscalls.c](file:///home/louis/code/linux/kernel/futex/syscalls.c) | `sys_futex_wait` 入口和 `do_futex` 分发 |
| [kernel/futex/waitwake.c](file:///home/louis/code/linux/kernel/futex/waitwake.c) | `__futex_wait`、`futex_wait`、`futex_do_wait` 实现 |
| [kernel/futex/core.c](file:///home/louis/code/linux/kernel/futex/core.c) | `futex_wait_setup`、`get_futex_key`、`futex_queue` |
| [kernel/futex/futex.h](file:///home/louis/code/linux/kernel/futex/futex.h) | `futex_q`、`futex_hash_bucket` 等数据结构 |