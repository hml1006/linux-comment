# futex 系统调用分析

## 1. 概述

futex（Fast Userspace Mutex）是 Linux 内核提供的一种高效同步机制，它将大部分同步操作放在用户空间完成，仅在需要时才进入内核。futex 的核心思想是：当没有竞争时，锁操作完全在用户空间完成，无需系统调用；仅在发生竞争时，才通过系统调用让内核介入处理等待队列。

futex 通过一个用户空间的 32 位整数值（`u32`）作为锁的状态标志，内核维护对应的等待队列哈希桶。用户空间通过原子操作修改该值，内核负责管理等待者的阻塞与唤醒。

**系统调用原型：**

```c
#include <linux/futex.h>
#include <sys/time.h>

long syscall(SYS_futex, u32 *uaddr, int futex_op, u32 val,
             const struct timespec *timeout, u32 *uaddr2, u32 val3);
```

## 2. 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `uaddr` | `u32*` | 用户空间 futex 字地址，指向一个 4 字节对齐的整数值 |
| `futex_op` | `int` | 操作类型，见下方操作列表 |
| `val` | `u32` | 期望值或唤醒数量，含义取决于操作类型 |
| `timeout` | `struct timespec*` | 可选超时时间，仅用于 WAIT 类操作 |
| `uaddr2` | `u32*` | 第二个 futex 地址，用于 requeue 类操作 |
| `val3` | `u32` | 附加参数，通常用于位掩码 |

## 3. 操作类型

```c
// include/uapi/linux/futex.h
#define FUTEX_WAIT            0  // 等待 futex 值变为指定值
#define FUTEX_WAKE            1  // 唤醒指定数量的等待者
#define FUTEX_FD              2  // 已废弃
#define FUTEX_REQUEUE         3  // 将等待者从一个 futex 迁移到另一个
#define FUTEX_CMP_REQUEUE     4  // 带条件比较的 requeue
#define FUTEX_WAKE_OP         5  // 唤醒并执行原子操作
#define FUTEX_LOCK_PI         6  // PI（优先级继承）futex 加锁
#define FUTEX_UNLOCK_PI       7  // PI futex 解锁
#define FUTEX_TRYLOCK_PI      8  // PI futex 尝试加锁
#define FUTEX_WAIT_BITSET     9  // 带位掩码的等待
#define FUTEX_WAKE_BITSET     10 // 带位掩码的唤醒
#define FUTEX_WAIT_REQUEUE_PI 11 // 等待并允许 requeue 到 PI futex
#define FUTEX_CMP_REQUEUE_PI  12 // 带条件比较的 requeue 到 PI futex
#define FUTEX_LOCK_PI2        13 // 更安全的 PI futex 加锁（无 REQUEUE_PI 依赖）

// 标志位
#define FUTEX_PRIVATE_FLAG    128 // 私有 futex（仅同进程内使用）
#define FUTEX_CLOCK_REALTIME  256 // 使用 CLOCK_REALTIME 超时
```

## 4. 核心数据结构

### 4.1 struct futex_q（等待队列项）

```c
// kernel/futex/futex.h
struct futex_q {
    struct plist_node list;          // 优先级排序链表节点，挂入哈希桶
    struct task_struct *task;        // 等待该 futex 的任务
    spinlock_t *lock_ptr;            // 指向哈希桶的自旋锁
    futex_wake_fn *wake;            // 唤醒回调函数
    void *wake_data;                 // 唤醒回调数据
    union futex_key key;             // futex 的哈希键
    struct futex_pi_state *pi_state; // PI 状态（仅 PI futex 使用）
    struct rt_mutex_waiter *rt_waiter; // RT 互斥锁等待者
    union futex_key *requeue_pi_key; // requeue_pi 目标键
    u32 bitset;                      // 位掩码（WAIT_BITSET/WAKE_BITSET）
    atomic_t requeue_state;          // requeue 状态（仅 requeue_pi 使用）
    bool drop_hb_ref;                // 是否释放哈希桶引用
    struct rcuwait requeue_wait;     // requeue 等待（RT 内核）
};
```

### 4.2 struct futex_hash_bucket（哈希桶）

```c
// kernel/futex/futex.h
struct futex_hash_bucket {
    atomic_t waiters;                // 等待者计数（用于 SMP 优化）
    spinlock_t lock;                 // 保护该桶的自旋锁
    struct plist_head chain;         // 优先级排序的等待队列链表
    struct futex_private_hash *priv; // 私有哈希引用
} ____cacheline_aligned_in_smp;     // 缓存行对齐，避免伪共享
```

### 4.3 union futex_key（futex 键）

```c
// kernel/futex/futex.h
union futex_key {
    struct {
        unsigned long pgoff;         // 页内偏移
        struct inode *inode;         // 共享映射的 inode
        int offset;                  // 页内偏移量
    } shared;                        // 共享映射（文件映射）
    struct {
        unsigned long uaddr;         // 用户空间地址（偏移量）
        struct mm_struct *mm;        // 进程地址空间
        int offset;                  // 页内偏移量
    } private;                       // 私有映射（匿名映射）
    struct {
        unsigned long word;          // 64 位地址的高位（用于哈希）
        void *ptr;                   // 指向地址空间的指针
        int offset;                  // 页内偏移量
    } both;
};
```

### 4.4 struct futex_vector（futex_waitv 辅助结构）

```c
// kernel/futex/futex.h
struct futex_vector {
    struct futex_waitv w;            // 用户提供的等待参数
    struct futex_q q;                // 内核端等待队列项
};
```

## 5. 函数调用链分析

### 5.1 系统调用入口

```
futex() 系统调用入口                           // kernel/futex/syscalls.c
  └─ SYSCALL_DEFINE6(futex, uaddr, op, val, utime, uaddr2, val3)
       ├─ futex_cmd_has_timeout(cmd)           // 检查是否需要超时
       ├─ get_timespec64(&ts, utime)           // 从用户空间读取超时时间
       ├─ futex_init_timeout(cmd, op, &ts, &t) // 初始化超时（WAIT 转为绝对时间）
       └─ do_futex(uaddr, op, val, tp, uaddr2, ...) // 分发到具体操作
```

### 5.2 do_futex 分发逻辑

```
do_futex(uaddr, op, val, timeout, uaddr2, val2, val3) // kernel/futex/syscalls.c
  ├─ futex_to_flags(op)                      // 解析 FLAGS_SHARED/FLAGS_CLOCKRT
  ├─ switch(cmd & FUTEX_CMD_MASK):
  │   ├─ FUTEX_WAIT:           → futex_wait(uaddr, flags, val, timeout, MATCH_ANY)
  │   ├─ FUTEX_WAIT_BITSET:    → futex_wait(uaddr, flags, val, timeout, val3)
  │   ├─ FUTEX_WAKE:           → futex_wake(uaddr, flags, val, MATCH_ANY)
  │   ├─ FUTEX_WAKE_BITSET:    → futex_wake(uaddr, flags, val, val3)
  │   ├─ FUTEX_REQUEUE:        → futex_requeue(uaddr, flags, uaddr2, flags, val, val2, NULL, 0)
  │   ├─ FUTEX_CMP_REQUEUE:    → futex_requeue(uaddr, flags, uaddr2, flags, val, val2, &val3, 0)
  │   ├─ FUTEX_WAKE_OP:        → futex_wake_op(uaddr, flags, uaddr2, val, val2, val3)
  │   ├─ FUTEX_LOCK_PI:        → futex_lock_pi(uaddr, flags, timeout, 0)
  │   ├─ FUTEX_LOCK_PI2:       → futex_lock_pi(uaddr, flags, timeout, 0)
  │   ├─ FUTEX_UNLOCK_PI:      → futex_unlock_pi(uaddr, flags)
  │   ├─ FUTEX_TRYLOCK_PI:     → futex_lock_pi(uaddr, flags, NULL, 1)
  │   ├─ FUTEX_WAIT_REQUEUE_PI:  → futex_wait_requeue_pi(...)
  │   └─ FUTEX_CMP_REQUEUE_PI:   → futex_requeue(..., requeue_pi=1)
  └─ default: → return -ENOSYS
```

## 6. 执行流程

```
用户线程 A                        用户线程 B
    |                                |
    | 原子操作递减锁值                |
    | (锁值从 1 → 0)                 |
    | 获取锁成功                      |
    | 执行临界区                      |
    |                                | 原子操作递减锁值
    |                                | (锁值从 0 → -1)
    |                                | 发现锁已被持有
    |                                | 调用 futex(WAIT)
    |                                | 内核检查锁值 == -1
    |                                | 将线程加入等待队列哈希桶
    |                                | 线程进入睡眠
    | 原子操作递增锁值 (释放锁)       |
    | (锁值从 -1 → 0)                |
    | 调用 futex(WAKE, 1)            |
    | 内核计算哈希                   |
    | 获取哈希桶锁                   |
    | 查找匹配的等待者               |
    | 唤醒线程 B                     |
    |                                | 线程 B 被唤醒
    |                                | 获取锁成功
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EAGAIN`/`EWOULDBLOCK` | futex 值已改变 | WAIT 时 `*uaddr != val` |
| `EFAULT` | 用户空间地址错误 | 无法读取 `uaddr`/`uaddr2` |
| `EINTR` | 被信号中断 | 等待期间收到信号 |
| `ETIMEDOUT` | 超时 | 指定时间内未被唤醒 |
| `EINVAL` | 参数无效 | 参数对齐错误、无效操作、无效位掩码 |
| `ENOSYS` | 不支持的操作 | 未启用 PI futex 但使用 PI 操作 |
| `EDEADLK` | 检测到死锁 | PI futex 死锁检测 |
| `ESRCH` | 找不到进程 | 用于 robust_list 相关操作 |

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

static atomic_int futex_word = 1;  // 1 表示未锁定，0 表示锁定

static int futex_wait(atomic_int *uaddr, int expected) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, expected,
                   NULL, NULL, 0);
}

static int futex_wake(atomic_int *uaddr, int nr_wake) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, nr_wake,
                   NULL, NULL, 0);
}

static void lock(atomic_int *lock) {
    // 尝试原子递减（从 1 → 0）
    int expected = 1;
    while (!atomic_compare_exchange_strong(lock, &expected, 0)) {
        // 锁已被持有，使用 futex 等待
        // 先将值设为 -1 表示有等待者
        if (expected != -1) {
            expected = atomic_exchange(lock, -1);
            if (expected == 1)  // 恰好被释放
                continue;
        }
        futex_wait(lock, -1);
        expected = 1;
    }
}

static void unlock(atomic_int *lock) {
    // 原子递增（如果有等待者，值从 -1 → 0）
    if (atomic_fetch_sub(lock, 1) != 1) {
        // 有等待者，将锁值设为 1 并唤醒
        atomic_store(lock, 1);
        futex_wake(lock, 1);
    }
}

// 线程函数
void *worker(void *arg) {
    int id = *(int *)arg;
    lock(&futex_word);
    printf("线程 %d 进入临界区\n", id);
    usleep(100000);  // 模拟工作
    printf("线程 %d 离开临界区\n", id);
    unlock(&futex_word);
    return NULL;
}

int main() {
    pthread_t t1, t2;
    int id1 = 1, id2 = 2;

    pthread_create(&t1, NULL, worker, &id1);
    pthread_create(&t2, NULL, worker, &id2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
```

## 9. 注意要点

1. **futex 字必须 4 字节对齐**，否则返回 `-EINVAL`
2. **私有 vs 共享**：同进程内的线程使用 `FUTEX_PRIVATE_FLAG` 可避免 inode 查找，提升性能
3. **PI futex**：用于解决优先级反转问题，需要 `CONFIG_FUTEX_PI` 内核配置
4. **futex2 系列**：`futex_waitv`/`futex_wake`/`futex_wait`/`futex_requeue` 是 futex2 的新接口，支持同时等待多个 futex，使用 `FUTEX2_*` 标志
5. **robust list**：用于处理线程意外死亡时的 futex 自动清理，避免死锁

## 10. 源码位置

| 文件 | 说明 |
|------|------|
| [kernel/futex/syscalls.c](file:///home/louis/code/linux/kernel/futex/syscalls.c) | futex 系统调用入口及 do_futex 分发 |
| [kernel/futex/core.c](file:///home/louis/code/linux/kernel/futex/core.c) | futex 核心实现（哈希、键管理、futex_cleanup） |
| [kernel/futex/waitwake.c](file:///home/louis/code/linux/kernel/futex/waitwake.c) | futex_wait/futex_wake/futex_wait_multiple 实现 |
| [kernel/futex/requeue.c](file:///home/louis/code/linux/kernel/futex/requeue.c) | futex_requeue/futex_wait_requeue_pi 实现 |
| [kernel/futex/pi.c](file:///home/louis/code/linux/kernel/futex/pi.c) | PI futex 实现 |
| [kernel/futex/futex.h](file:///home/louis/code/linux/kernel/futex/futex.h) | futex 内核数据结构定义 |
| [include/uapi/linux/futex.h](file:///home/louis/code/linux/include/uapi/linux/futex.h) | 用户态 API 定义 |