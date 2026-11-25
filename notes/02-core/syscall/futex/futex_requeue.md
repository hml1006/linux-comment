# futex_requeue 系统调用分析

## 1. 概述

`futex_requeue` 用于将等待在一个 futex（源）上的线程迁移到另一个 futex（目标）上，同时可以唤醒部分等待者。这是一种高效的同步原语，常用于实现条件变量等高级同步机制。

核心思想：先唤醒 `nr_wake` 个等待者，然后将剩余等待者从源 futex 的等待队列迁移到目标 futex 的等待队列。

futex_requeue 有两种变体：
- **FUTEX_REQUEUE**：无条件 requeue（不检查 futex 值）
- **FUTEX_CMP_REQUEUE**：带条件比较的 requeue（检查 `*uaddr1 == val3`）
- **FUTEX_CMP_REQUEUE_PI**：requeue 到 PI futex

## 2. 函数原型

### 传统接口

```c
#include <linux/futex.h>
#include <sys/syscall.h>

long nr_requeued = syscall(SYS_futex,
    u32 *uaddr1,           // 源 futex 地址
    FUTEX_CMP_REQUEUE,     // 操作码（或 FUTEX_REQUEUE）
    u32 nr_wake,           // 唤醒数量
    u32 nr_requeue,        // requeue 数量
    u32 *uaddr2,           // 目标 futex 地址
    u32 cmpval);           // 比较值（仅 CMP_REQUEUE）
```

### futex2 接口

```c
#include <linux/futex.h>
#include <sys/syscall.h>

long nr_requeued = syscall(SYS_futex_requeue,
    struct futex_waitv *waiters,  // 包含源和目标 futex 信息的数组
    unsigned int flags,           // 保留，必须为 0
    int nr_wake,                  // 唤醒数量（源 futex）
    int nr_requeue);              // requeue 数量（从源到目标）
```

## 3. 详细调用链

```
sys_futex_requeue(waiters, flags, nr_wake, nr_requeue)  // kernel/futex/syscalls.c
  ├─ futex_parse_waitv(futexes, waiters, 2, ...)         // 解析用户态参数
  │    └─ 提取 futexes[0]（源）和 futexes[1]（目标）
  ├─ [flags 不匹配] → return -EINVAL
  ├─ cmpval = futexes[0].w.val                           // 期望比较值
  └─ futex_requeue(uaddr1, flags1, uaddr2, flags2,      // 核心操作
                   nr_wake, nr_requeue, &cmpval, 0)
       └─ futex_requeue(uaddr1, flags1, uaddr2, flags2,  // kernel/futex/requeue.c
                        nr_wake, nr_requeue, cmpval, requeue_pi)
            ├─ [nr_wake < 0 || nr_requeue < 0] → return -EINVAL
            ├─ get_futex_key(uaddr1, flags1, &key1, FUTEX_READ)  // 获取源键
            ├─ get_futex_key(uaddr2, flags2, &key2, FUTEX_WRITE) // 获取目标键
            ├─ [requeue_pi] futex_proxy_trylock_atomic()  // PI 情况尝试代理锁
            ├─ double_lock_hb(hb1, hb2)                   // 按地址顺序锁两个桶
            ├─ [cmpval] futex_get_value_locked(&curval, uaddr1) // 读源值
            │    └─ [curval != *cmpval] → goto out_unlock  // 值不匹配，返回 -EAGAIN
            ├─ plist_for_each_entry_safe(this, next, &hb1->chain, list)
            │    └─ futex_match(&this->key, &key1)         // 匹配源 futex
            │         ├─ [nr_wake > 0]                     // 先唤醒
            │         │    └─ wake_futex(this)             // 唤醒并出队
            │         │         └─ wake_up_state(task, TASK_NORMAL)
            │         │    nr_wake--
            │         ├─ [nr_requeue > 0]                  // 再 requeue
            │         │    └─ requeue_futex(this, hb1, hb2, &key2)
            │         │         ├─ futex_unqueue(this)     // 从源桶出队
            │         │         ├─ this->key = key2        // 更新键
            │         │         └─ plist_add(&this->list, &hb2->chain) // 入目标桶
            │         │    nr_requeue--
            │         └─ [达到配额] → break
            ├─ out_unlock:
            │    └─ double_unlock_hb(hb1, hb2)             // 释放两个桶的锁
            └─ wake_up_q(&wake_q)                          // 执行批量唤醒
```

## 4. 核心函数详解

### 4.1 requeue_futex

```c
// kernel/futex/requeue.c
/*
 * requeue_futex() - 将等待者从源 futex 迁移到目标 futex
 * 在持有两个哈希桶锁的情况下执行迁移
 *
 * 步骤：
 * 1. 从源哈希桶的 plist 中移除
 * 2. 更新 futex_q 的 key 为目标 futex 的 key
 * 3. 将 futex_q 加入目标哈希桶的 plist
 */
static inline void requeue_futex(struct futex_q *q, struct futex_hash_bucket *hb1,
                                 struct futex_hash_bucket *hb2, union futex_key *key2)
{
    // 从源桶出队
    hlist_del_init(&q->list);
    // 更新键
    q->key = *key2;
    // 入目标桶
    plist_add(&q->list, &hb2->chain);
    // 更新锁指针
    q->lock_ptr = &hb2->lock;
}
```

### 4.2 futex_requeue 双锁机制

```c
// kernel/futex/futex.h
/*
 * double_lock_hb() - 按地址顺序锁两个哈希桶
 * 避免死锁：始终先锁地址较小的桶
 */
static inline void
double_lock_hb(struct futex_hash_bucket *hb1, struct futex_hash_bucket *hb2)
{
    if (hb1 > hb2)
        swap(hb1, hb2);
    spin_lock(&hb1->lock);
    if (hb1 != hb2)
        spin_lock_nested(&hb2->lock, SINGLE_DEPTH_NESTING);
}

static inline void
double_unlock_hb(struct futex_hash_bucket *hb1, struct futex_hash_bucket *hb2)
{
    spin_unlock(&hb1->lock);
    if (hb1 != hb2)
        spin_unlock(&hb2->lock);
}
```

## 5. 流程图

```
futex_requeue 调用流程:
=============

用户态                          内核态
   |                              |
   | syscall(SYS_futex, ...,      |
   |   FUTEX_CMP_REQUEUE, ...)    |
   |----------------------------->|
   |                          futex_requeue():
   |                            ├─ get_futex_key(uaddr1)  // 源键
   |                            ├─ get_futex_key(uaddr2)  // 目标键
   |                            ├─ double_lock_hb(hb1, hb2) // 锁两个桶
   |                            ├─ 读用户值比较
   |                            │    └─ [不匹配] → 返回 -EAGAIN
   |                            ├─ 遍历源桶的 plist:
   |                            │    │
   |                            │    ├─ 前 nr_wake 个:
   |                            │    │    └─ wake_futex()   // 唤醒
   |                            │    │
   |                            │    └─ 接下来 nr_requeue 个:
   |                            │         └─ requeue_futex()  // 迁移
   |                            │              ├─ 从 hb1 出队
   |                            │              ├─ 更新 key 为 key2
   |                            │              └─ 入队到 hb2
   |                            │
   |                            ├─ double_unlock_hb(hb1, hb2)
   |                            └─ wake_up_q()  // 唤醒
   |                              |
   |        return 操作数量      |
   |<-----------------------------|
   |                              |
```

## 6. 条件变量实现示例

futex_requeue 常用于实现条件变量：

```
条件变量 wait 操作:
==================
1. 释放互斥锁
2. futex(CMP_REQUEUE, 0, 1, &cond, &mutex, 1)
   - 不唤醒任何等待者（nr_wake=0）
   - 将当前线程从 cond 的等待队列迁移到 mutex 的等待队列
3. futex(WAIT, &mutex, 1)  // 等待互斥锁

条件变量 signal 操作:
====================
1. futex(CMP_REQUEUE, 1, 0, &cond, &mutex, 0)
   - 唤醒 1 个 cond 上的等待者（nr_wake=1）
   - 不迁移（nr_requeue=0）
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EAGAIN` | 源 futex 值不匹配 | `CMP_REQUEUE` 时 `*uaddr1 != cmpval` |
| `EFAULT` | 用户空间地址错误 | 无法读取 `uaddr1`/`uaddr2` |
| `EINVAL` | 参数无效 | `nr_wake < 0`、`nr_requeue < 0`、flags 不一致 |
| `ENOSYS` | 不支持的操作 | 未启用 PI 但请求 `requeue_pi` |
| `EDEADLK` | 死锁检测 | PI requeue 时检测到潜在死锁 |

## 8. 使用示例

```c
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>

// 一个简单的条件变量实现

typedef struct {
    atomic_int mutex;   // 互斥锁：1=未锁定, 0=锁定
    atomic_int cond;    // 条件变量 futex 字
} condvar_t;

static int futex_wait(atomic_int *uaddr, int expected) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, expected,
                   NULL, NULL, 0);
}

static int futex_wake(atomic_int *uaddr, int nr) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, nr,
                   NULL, NULL, 0);
}

static int futex_cmp_requeue(atomic_int *uaddr1, int nr_wake, int nr_requeue,
                             atomic_int *uaddr2, int cmpval) {
    return syscall(SYS_futex, uaddr1, FUTEX_CMP_REQUEUE,
                   nr_wake, nr_requeue, uaddr2, cmpval);
}

// 条件变量 wait
void cond_wait(condvar_t *cv) {
    // 释放互斥锁（将 mutex 设为 1）
    atomic_store(&cv->mutex, 1);
    // 将当前线程从 cond 队列迁移到 mutex 队列
    futex_cmp_requeue(&cv->cond, 0, 1, &cv->mutex, 0);
    // 等待 mutex
    futex_wait(&cv->mutex, 1);
    // 获取 mutex
    while (atomic_exchange(&cv->mutex, 0) != 1)
        futex_wait(&cv->mutex, 0);
}

// 条件变量 signal
void cond_signal(condvar_t *cv) {
    // 唤醒 1 个等待者（使用 requeue 将其迁移到 mutex 队列）
    futex_cmp_requeue(&cv->cond, 1, 0, &cv->mutex, 0);
}

// 条件变量 broadcast
void cond_broadcast(condvar_t *cv) {
    // 唤醒所有等待者
    futex_wake(&cv->cond, INT_MAX);
}

int main() {
    condvar_t cv = { .mutex = 1, .cond = 0 };
    printf("条件变量示例\n");
    return 0;
}
```

## 9. 关键要点

1. **双桶锁定顺序**：`double_lock_hb` 按哈希桶地址升序锁桶，避免死锁
2. **原子性**：`futex_requeue` 在持有两个哈希桶锁的情况下执行，确保唤醒和迁移操作的原子性
3. **条件参数**：`FUTEX_CMP_REQUEUE` 需要额外检查 `*uaddr1 == cmpval`，这是安全的关键——确保用户空间状态未变
4. **惊群效应避免**：通过 requeue 而不是 broadcast 唤醒所有等待者，将大多数等待者迁移到互斥锁队列，避免惊群
5. **PI requeue**：`FUTEX_CMP_REQUEUE_PI` 支持将等待者迁移到 PI futex，需要处理优先级继承和代理锁获取

## 10. 源码位置

| 文件 | 说明 |
|------|------|
| [kernel/futex/syscalls.c](file:///home/louis/code/linux/kernel/futex/syscalls.c) | `sys_futex_requeue` 入口和 `do_futex` 分发 |
| [kernel/futex/requeue.c](file:///home/louis/code/linux/kernel/futex/requeue.c) | `futex_requeue`、`requeue_futex`、`futex_wait_requeue_pi` 实现 |
| [kernel/futex/futex.h](file:///home/louis/code/linux/kernel/futex/futex.h) | `double_lock_hb`、`double_unlock_hb`、`futex_requeue_state` 枚举 |