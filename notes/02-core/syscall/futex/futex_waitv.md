# futex_waitv 系统调用分析

## 1. 概述

`futex_waitv` 是 futex2 系列引入的新系统调用（Linux 5.16+），允许线程同时等待多个 futex。当任何一个 futex 被唤醒、值发生变化、超时或收到信号时，系统调用返回。这是传统 `futex(FUTEX_WAIT)` 的增强版本，后者只能等待单个 futex。

`futex_waitv` 解决了多 futex 等待的场景，替代了旧有的 `FUTEX_WAIT_MULTIPLE` 操作（该操作由于设计问题被移除）。它使用 `futex_waitv` 结构体数组来描述多个等待条件。

## 2. 函数原型

```c
#include <linux/futex.h>
#include <sys/syscall.h>

long ret = syscall(SYS_futex_waitv,
    struct futex_waitv *waiters,     // 等待者数组
    unsigned int nr_futexes,         // 数组长度（最大 FUTEX_WAITV_MAX）
    unsigned int flags,              // 保留，必须为 0
    struct __kernel_timespec *timeout,  // 可选超时（绝对时间）
    clockid_t clockid);              // 时钟类型（CLOCK_MONOTONIC 或 CLOCK_REALTIME）
```

## 3. 参数说明

### 3.1 struct futex_waitv

```c
// include/uapi/linux/futex.h
struct futex_waitv {
    __u64 uaddr;                         // futex 用户空间地址（64 位）
    __u64 val;                           // 期望值
    __u64 flags;                         // 标志（FUTEX2_*）
    __u64 __reserved;                    // 保留，必须为 0
};
```

### 3.2 FUTEX2 标志

```c
// include/uapi/linux/futex.h
#define FUTEX2_SIZE_U8      0x00    // 8 位 futex（暂不支持）
#define FUTEX2_SIZE_U16     0x01    // 16 位 futex（暂不支持）
#define FUTEX2_SIZE_U32     0x02    // 32 位 futex
#define FUTEX2_SIZE_U64     0x04    // 64 位 futex（暂不支持）

#define FUTEX2_PRIVATE      FUTEX_SIZE_U32  // 私有 futex 标志
#define FUTEX2_VALID_MASK   0x07    // 有效标志掩码
```

## 4. 详细调用链

```
sys_futex_waitv(waiters, nr_futexes, flags, timeout, clockid)  // kernel/futex/syscalls.c
  ├─ [flags != 0] → return -EINVAL                     // 暂不支持 flags
  ├─ [nr_futexes == 0 || nr_futexes > FUTEX_WAITV_MAX] → return -EINVAL
  ├─ [timeout] futex2_setup_timeout(timeout, clockid, &to)  // 设置超时
  ├─ futexv = kzalloc_objs(*futexv, nr_futexes)       // 分配内核空间数组
  ├─ futex_parse_waitv(futexv, waiters, nr_futexes,    // 解析用户态参数
  │                     futex_wake_mark, NULL)
  │    └─ 循环解析每个 futex_waitv:
  │         ├─ copy_from_user(&aux, &uwaitv[i], sizeof(aux))
  │         ├─ [flags 无效或 __reserved != 0] → return -EINVAL
  │         ├─ futex2_to_flags(aux.flags)              // 转换标志
  │         ├─ futex_flags_valid(flags)                // 验证标志
  │         ├─ futex_validate_input(flags, aux.val)    // 验证值
  │         └─ 填充 futexv[i] 的 w 和 q 字段
  │
  └─ futex_wait_multiple(futexv, nr_futexes, timeout ? &to : NULL)  // 核心等待
       └─ futex_wait_multiple(vs, count, to)           // kernel/futex/waitwake.c
            ├─ [to] hrtimer_sleeper_start_expires(to, HRTIMER_MODE_ABS)
            │
            └─ while (1) {  // 重试循环
                 ├─ futex_wait_multiple_setup(vs, count, &hint)
                 │    ├─ lock all hbs (按地址排序)
                 │    ├─ 循环读取每个 futex 的值:
                 │    │    └─ [uval != val] → 已唤醒，返回索引
                 │    ├─ futex_queue 所有等待者入队
                 │    ├─ unlock all hbs
                 │    └─ return 0
                 │
                 ├─ futex_sleep_multiple(vs, count, to)  // 睡眠
                 │    └─ schedule()
                 │
                 ├─ __set_current_state(TASK_RUNNING)
                 ├─ futex_unqueue_multiple(vs, count)    // 检查唤醒源
                 │    └─ [已出队] → 返回被唤醒的索引
                 ├─ [超时] → return -ETIMEDOUT
                 ├─ [信号] → return -ERESTARTSYS
                 └─ [虚假唤醒] → continue
              }
```

## 5. 核心函数详解

### 5.1 futex_wait_multiple_setup

```c
// kernel/futex/waitwake.c
/*
 * futex_wait_multiple_setup() - 准备等待多个 futex
 * 按顺序锁所有哈希桶，读取每个 futex 的值
 * 如果有任何 futex 的值已改变，立即返回其索引
 */
int futex_wait_multiple_setup(struct futex_vector *vs, int count, int *woken)
{
    // 1. 获取所有 futex 的键
    // 2. 按地址排序哈希桶
    // 3. 依次锁桶
    for (i = 0; i < count; i++) {
        // 读取用户空间值
        // 如果 != 期望值，标记为已唤醒
        if (uval != vs[i].w.val) {
            *woken = i;
            ret = 1;
            goto out;
        }
    }
    // 4. 所有值匹配，入队
    for (i = 0; i < count; i++)
        futex_queue(&vs[i].q, hbs[i], current);
    // 5. 解锁所有桶
out:
    // 释放桶
    return ret;
}
```

### 5.2 futex_sleep_multiple

```c
// kernel/futex/waitwake.c
/*
 * futex_sleep_multiple() - 仅在没有任何 futex 被唤醒时睡眠
 * 检查每个 futex_q 的 lock_ptr，如果任一为 NULL 表示已被唤醒
 */
static void futex_sleep_multiple(struct futex_vector *vs, unsigned int count,
                                 struct hrtimer_sleeper *to)
{
    if (to && !to->task)
        return;  // 超时已到期

    for (; count; count--, vs++) {
        if (!READ_ONCE(vs->q.lock_ptr))
            return;  // 已被唤醒
    }
    schedule();  // 所有 futex 都未被唤醒，睡眠
}
```

## 6. 流程图

```
futex_waitv 调用流程:
=============

用户态                          内核态
   |                              |
   | struct futex_waitv           |
   | waiters[0] = {uaddr,val,flg}|
   | waiters[1] = {uaddr,val,flg}|
   | ...                          |
   | syscall(SYS_futex_waitv,     |
   |   waiters, nr, 0, timeout,  |
   |   clockid)                   |
   |----------------------------->|
   |                              |
   |                          futex_parse_waitv():
   |                            ├─ 验证每个 futex_waitv
   |                            ├─ 转换标志
   |                            └─ 填充 futex_vector 数组
   |                              |
   |                          futex_wait_multiple():
   |                            ├─ futex_wait_multiple_setup:
   |                            │    ├─ 获取所有键
   |                            │    ├─ 锁所有哈希桶
   |                            │    ├─ 读每个 futex 值
   |                            │    ├─ [值已变] → 返回索引
   |                            │    ├─ 入队所有等待者
   |                            │    └─ 解锁所有桶
   |                            │
   |                            ├─ futex_sleep_multiple():
   |                            │    ├─ 检查是否有 futex 已唤醒
   |                            │    └─ schedule()
   |                            │
   |                            ├─ futex_unqueue_multiple():
   |                            │    └─ 返回被唤醒的索引
   |                            │
   |                            └─ [虚假唤醒] → 重试
   |                              |
   |        return 被唤醒的索引   |
   |<-----------------------------|
   |                              |
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 参数无效 | `nr_futexes == 0`、`nr_futexes > FUTEX_WAITV_MAX`、`flags != 0`、`waiters == NULL`、`__reserved != 0`、标志无效 |
| `EFAULT` | 用户空间地址错误 | 无法读取 `waiters` 数组 |
| `ENOMEM` | 内存不足 | 无法分配内核 `futex_vector` 数组 |
| `ETIMEDOUT` | 超时 | 指定时间内未被唤醒 |
| `ERESTARTSYS` | 被信号中断 | 等待期间收到信号 |

## 8. 使用示例

```c
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define FUTEX_WAITV_MAX 128

static int futex_waitv(struct futex_waitv *waiters, unsigned int nr,
                       struct timespec *timeout, clockid_t clockid) {
    return syscall(SYS_futex_waitv, waiters, nr, 0,
                   timeout, clockid);
}

// 同时等待多个条件
int wait_for_any(atomic_int *futexes, unsigned long *expected,
                 unsigned int nr, int timeout_ms) {
    struct futex_waitv *waiters;
    struct timespec ts = {
        .tv_sec = timeout_ms / 1000,
        .tv_nsec = (timeout_ms % 1000) * 1000000,
    };
    int ret;

    waiters = malloc(nr * sizeof(struct futex_waitv));
    if (!waiters) return -1;

    for (unsigned int i = 0; i < nr; i++) {
        waiters[i].uaddr = (__u64)(unsigned long)&futexes[i];
        waiters[i].val = expected[i];
        waiters[i].flags = FUTEX2_SIZE_U32 | FUTEX2_PRIVATE;
        waiters[i].__reserved = 0;
    }

    ret = futex_waitv(waiters, nr,
                      timeout_ms >= 0 ? &ts : NULL,
                      CLOCK_MONOTONIC);

    free(waiters);
    return ret;
}

// 线程函数：等待多个 futex
void *waiter_thread(void *arg) {
    atomic_int *futexes = (atomic_int *)arg;
    unsigned long expected[] = {0, 0, 0};

    printf("等待任意一个 futex 被唤醒...\n");
    int idx = wait_for_any(futexes, expected, 3, 5000);
    if (idx >= 0) {
        printf("futex[%d] 被唤醒！\n", idx);
    } else if (idx == -ETIMEDOUT) {
        printf("等待超时\n");
    } else {
        printf("错误: %d\n", idx);
    }
    return NULL;
}

int main() {
    atomic_int futexes[3] = {0, 0, 0};
    pthread_t thread;

    pthread_create(&thread, NULL, waiter_thread, futexes);
    sleep(1);

    // 唤醒 futex[1]
    futexes[1] = 1;
    syscall(SYS_futex, &futexes[1], FUTEX_WAKE, 1, NULL, NULL, 0);

    pthread_join(thread, NULL);
    return 0;
}
```

## 9. 关键要点

1. **最大等待数**：`FUTEX_WAITV_MAX` 定义了最大可以同时等待的 futex 数量，通常为 128
2. **参数解析**：`futex_parse_waitv` 从用户空间复制并验证每个 `futex_waitv` 结构体
3. **多桶锁**：`futex_wait_multiple_setup` 需要锁多个哈希桶，按地址排序锁桶避免死锁
4. **唤醒检测**：`futex_sleep_multiple` 在睡眠前检查每个 futex 的 `lock_ptr`，如果任一为 NULL（已被唤醒），则不睡眠
5. **返回值**：返回被唤醒的 futex 的数组索引（0-based），不保证是第一个被唤醒的
6. **futex2 系列**：`futex_waitv` 是 futex2 系列的一部分，与 `futex_wake`、`futex_wait` 和 `futex_requeue` 共享 `FUTEX2_*` 标志体系

## 10. 源码位置

| 文件 | 说明 |
|------|------|
| [kernel/futex/syscalls.c](file:///home/louis/code/linux/kernel/futex/syscalls.c) | `sys_futex_waitv` 入口、`futex_parse_waitv` |
| [kernel/futex/waitwake.c](file:///home/louis/code/linux/kernel/futex/waitwake.c) | `futex_wait_multiple`、`futex_wait_multiple_setup`、`futex_sleep_multiple`、`futex_unqueue_multiple` |
| [kernel/futex/futex.h](file:///home/louis/code/linux/kernel/futex/futex.h) | `struct futex_vector` 定义 |
| [include/uapi/linux/futex.h](file:///home/louis/code/linux/include/uapi/linux/futex.h) | `struct futex_waitv`、`FUTEX2_*` 标志定义 |