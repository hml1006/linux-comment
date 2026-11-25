# futex_wake 系统调用分析

## 1. 概述

`futex_wake` 是 futex 的唤醒操作。当用户空间线程释放锁时，调用 `futex_wake` 唤醒等待在该 futex 上的其他线程。内核通过 futex 地址计算哈希，找到对应的哈希桶，遍历等待队列，唤醒最多 `nr_wake` 个匹配的等待者。

futex_wake 有两种实现路径：
- **传统接口**：通过 `futex(FUTEX_WAKE, ...)` 系统调用
- **futex2 接口**：通过 `sys_futex_wake()` 系统调用

## 2. 函数原型

### 传统接口

```c
#include <linux/futex.h>
#include <sys/syscall.h>

long nr_woken = syscall(SYS_futex,
    u32 *uaddr,          // futex 用户空间地址
    FUTEX_WAKE,          // 操作码
    u32 nr_wake,         // 最大唤醒数
    NULL,                // timeout（未使用）
    NULL,                // uaddr2（未使用）
    0);                  // val3（未使用）
```

### futex2 接口

```c
#include <linux/futex.h>
#include <sys/syscall.h>

long nr_woken = syscall(SYS_futex_wake,
    void *uaddr,         // futex 用户空间地址
    unsigned long mask,  // 位掩码
    int nr,              // 最大唤醒数
    unsigned int flags); // FUTEX2 标志
```

## 3. 详细调用链

```
sys_futex_wake(uaddr, mask, nr, flags)                  // kernel/futex/syscalls.c
  ├─ 检查 flags 有效性
  ├─ futex2_to_flags(flags)                             // 转换 FUTEX2 标志
  ├─ futex_flags_valid(flags)                           // 验证标志组合
  ├─ futex_validate_input(flags, mask)                  // 验证 mask 值
  └─ futex_wake(uaddr, FLAGS_STRICT | flags, nr, mask)  // 核心唤醒
       └─ futex_wake(uaddr, flags, nr_wake, bitset)     // kernel/futex/waitwake.c
            ├─ [bitset == 0] → return -EINVAL
            ├─ get_futex_key(uaddr, flags, &key, FUTEX_READ)  // 获取 futex 键
            ├─ [FLAGS_STRICT && nr_wake == 0] → return 0     // 快速路径
            ├─ hb = futex_hash(&key)                         // 获取哈希桶
            ├─ [无等待者] → return 0                         // 优化：无等待者直接返回
            ├─ spin_lock(&hb->lock)                          // 加锁保护链表
            ├─ plist_for_each_entry_safe(this, next, &hb->chain, list)
            │    └─ futex_match(&this->key, &key)            // 匹配 futex 键
            │         ├─ [PI 或 RT 等待者] → return -EINVAL  // 不允许直接唤醒 PI 等待者
            │         ├─ [bitset 不匹配] → continue          // 位掩码过滤
            │         └─ this->wake(&wake_q, this)           // 调用唤醒回调
            │              └─ __futex_wake_mark(this)        // 标记唤醒
            │                   └─ [ret++ >= nr_wake] → break // 达到唤醒数
            ├─ spin_unlock(&hb->lock)                        // 释放锁
            └─ wake_up_q(&wake_q)                            // 批量唤醒任务
                 └─ try_to_wake_up(task, ...)                 // 唤醒进程
```

## 4. 核心函数详解

### 4.1 futex_wake

```c
// kernel/futex/waitwake.c
/*
 * futex_wake() - 唤醒等待在指定 futex 上的任务
 * @uaddr:   用户空间 futex 地址
 * @flags:   FLAGS_SHARED、FLAGS_STRICT 等
 * @nr_wake: 最大唤醒数量
 * @bitset:  位掩码，仅唤醒 bitset 匹配的等待者
 *
 * 返回值：实际唤醒的任务数量，负数表示错误
 */
int futex_wake(u32 __user *uaddr, unsigned int flags, int nr_wake, u32 bitset)
{
    struct futex_q *this, *next;
    union futex_key key = FUTEX_KEY_INIT;
    DEFINE_WAKE_Q(wake_q);
    int ret;

    if (!bitset)
        return -EINVAL;

    ret = get_futex_key(uaddr, flags, &key, FUTEX_READ);
    if (unlikely(ret != 0))
        return ret;

    if ((flags & FLAGS_STRICT) && !nr_wake)
        return 0;

    CLASS(hb, hb)(&key);

    if (!futex_hb_waiters_pending(hb))
        return ret;

    spin_lock(&hb->lock);

    plist_for_each_entry_safe(this, next, &hb->chain, list) {
        if (futex_match(&this->key, &key)) {
            if (this->pi_state || this->rt_waiter) {
                ret = -EINVAL;
                break;
            }
            if (!(this->bitset & bitset))
                continue;

            this->wake(&wake_q, this);
            if (++ret >= nr_wake)
                break;
        }
    }

    spin_unlock(&hb->lock);
    wake_up_q(&wake_q);
    return ret;
}
```

### 4.2 唤醒回调机制

```c
// kernel/futex/futex.h
typedef void (futex_wake_fn)(struct wake_q_head *wake_q, struct futex_q *q);

// kernel/futex/waitwake.c
/*
 * __futex_wake_mark() - 从哈希桶中移除等待者并添加到唤醒队列
 * 将 futex_q 从 plist 中移除，标记 lock_ptr 为 NULL
 * 表示该 q 已被唤醒，等待者不再需要竞争锁
 */
bool __futex_wake_mark(struct futex_q *q)
{
    // 从哈希桶的 plist 中移除
    plist_del(&q->list, &q->lock_ptr->chain);
    // 设置 lock_ptr 为 NULL，表示已出队
    // futex_unqueue 通过检查 lock_ptr 判断是否已出队
    q->lock_ptr = NULL;
    return true;
}

/*
 * futex_wake_mark() - 默认唤醒回调
 * 将等待者任务添加到 wake_q 中，由 wake_up_q() 批量唤醒
 */
void futex_wake_mark(struct wake_q_head *wake_q, struct futex_q *q)
{
    wake_q_add(wake_q, q->task);
    q->task = NULL;
    __futex_wake_mark(q);
}
```

## 5. 流程图

```
futex_wake 调用流程:
=============

用户态                          内核态
   |                              |
   | syscall(SYS_futex_wake, ...) |
   |----------------------------->|
   |                              |
   |                          futex_wake():
   |                            ├─ get_futex_key()         // 获取键
   |                            ├─ futex_hb_waiters_pending() // 检查是否有等待者
   |                            │    └─ [无等待者] → 直接返回 0
   |                            ├─ spin_lock(hb->lock)     // 加锁
   |                            ├─ 遍历 plist 链表:
   |                            │    ├─ futex_match(key)   // 匹配键
   |                            │    ├─ bitset 检查        // 位掩码过滤
   |                            │    └─ wake(q)           // 入唤醒队列
   |                            ├─ spin_unlock(hb->lock)   // 解锁
   |                            └─ wake_up_q(&wake_q)      // 批量唤醒
   |                                 └─ try_to_wake_up()   // 唤醒进程
   |                              |
   |        return 唤醒数量       |
   |<-----------------------------|
   |                              |
```

## 6. 唤醒优化

### 6.1 无等待者优化

```c
// 在获取哈希桶锁之前检查是否有等待者
// 避免在无等待者时获取锁的开销
if (!futex_hb_waiters_pending(hb))
    return ret;
```

### 6.2 批量唤醒

```c
// 使用 wake_q 机制批量唤醒，减少锁竞争
// 将所有要唤醒的任务先加入 wake_q，然后一次性唤醒
DEFINE_WAKE_Q(wake_q);
// ... 遍历添加任务到 wake_q ...
wake_up_q(&wake_q);  // 一次性唤醒所有任务
```

### 6.3 位掩码过滤

```c
// 等待者设置 bitset，唤醒者检查 bitset 交集
// 允许精细化唤醒，只有 bitset 匹配的等待者才会被唤醒
if (!(this->bitset & bitset))
    continue;
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 参数无效 | `bitset == 0`、flags 无效、遇到 PI 等待者 |
| `EFAULT` | 用户空间地址错误 | 无法读取 `uaddr` |
| `ENOSYS` | 不支持的操作 | 未启用 futex 支持 |

## 8. 使用示例

```c
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>

static int futex_wake(atomic_int *uaddr, int nr_wake) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, nr_wake,
                   NULL, NULL, 0);
}

// 带位掩码的唤醒
static int futex_wake_bitset(atomic_int *uaddr, int nr_wake, u32 bitset) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE_BITSET, nr_wake,
                   NULL, NULL, bitset);
}

// 使用 futex2 接口
static int futex_wake2(void *uaddr, unsigned long mask, int nr,
                       unsigned int flags) {
    return syscall(SYS_futex_wake, uaddr, mask, nr, flags);
}

int main() {
    atomic_int futex_word = 0;

    // 唤醒所有等待者
    int nr = futex_wake(&futex_word, INT_MAX);
    printf("唤醒 %d 个等待者\n", nr);

    // 唤醒 1 个等待者
    nr = futex_wake(&futex_word, 1);
    printf("唤醒 1 个等待者，实际唤醒: %d\n", nr);

    // 带位掩码唤醒
    nr = futex_wake_bitset(&futex_word, 1, 0xFFFFFFFF);
    printf("位掩码唤醒: %d\n", nr);

    return 0;
}
```

## 9. 关键要点

1. **唤醒数量限制**：`nr_wake` 参数控制最大唤醒数量，通常为 1（避免惊群效应），也可设为 `INT_MAX` 唤醒所有等待者
2. **优先级排序**：等待队列使用 `plist`（优先级排序链表），`futex_wake` 按优先级从高到低遍历，高优先级任务先被唤醒
3. **位掩码匹配**：`FUTEX_WAKE_BITSET` 使用位掩码进行精细化唤醒，只有 `this->bitset & bitset != 0` 的等待者才会被唤醒
4. **PI futex 限制**：不能直接使用 `futex_wake` 唤醒 PI futex 的等待者，必须通过 PI 逻辑处理
5. **批量唤醒优化**：使用 `wake_q` 延迟唤醒，在释放哈希桶锁后才实际唤醒任务，减少锁持有时间
6. **无等待者优化**：在获取哈希桶锁之前检查 `futex_hb_waiters_pending()`，避免不必要的锁获取

## 10. 源码位置

| 文件 | 说明 |
|------|------|
| [kernel/futex/syscalls.c](file:///home/louis/code/linux/kernel/futex/syscalls.c) | `sys_futex_wake` 入口和 `do_futex` 分发 |
| [kernel/futex/waitwake.c](file:///home/louis/code/linux/kernel/futex/waitwake.c) | `futex_wake`、`__futex_wake_mark`、`futex_wake_mark` 实现 |
| [kernel/futex/core.c](file:///home/louis/code/linux/kernel/futex/core.c) | `get_futex_key`、哈希桶管理 |
| [kernel/futex/futex.h](file:///home/louis/code/linux/kernel/futex/futex.h) | `futex_q`、`futex_hash_bucket`、`futex_wake_fn` 定义 |