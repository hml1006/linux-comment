# 6. 读写信号量 (rw_semaphore)

## 6.1 概述

读写信号量 (rwsem) 是一种允许多个读者并发访问、写者独占访问的同步机制。适用于读操作远多于写操作的场景。

**核心特性：**
- 读并发：多个读者可同时获得锁
- 写互斥：写者必须等待所有读者释放
- 写者优先：新读者在写者等待时不能获取锁 (防止写者饥饿)
- 可睡眠：读者和写者等待时都会睡眠
- 乐观自旋：等待者可在持有者运行时短暂自旋

## 6.2 关键数据结构

### 6.2.1 struct rw_semaphore

定义在 [include/linux/rwsem.h](file:///home/louis/code/linux/include/linux/rwsem.h)：

```c
// include/linux/rwsem.h
struct rw_semaphore {
    atomic_long_t           count;          // 读写计数器
    /*
     * count 编码:
     *  位 0          : 写锁定标志 (1=写者持有)
     *  位 1          : 等待者标志 (1=有等待者)
     *  位 [63:2]     : 读者计数 (每读 +1)
     *  count = 0     : 未锁定
     *  count = 1     : 写者持有, 无等待者
     *  count = RWSEM_WRITER_LOCKED | RWSEM_FLAG_WAITERS = 0x03
     *                : 写者持有, 有等待者
     *  count = N     : N 个读者持有
     */
    raw_spinlock_t          wait_lock;      // 保护 wait_list
    struct list_head        wait_list;      // 等待者链表 (FIFO)
#ifdef CONFIG_DEBUG_RWSEMS
    void                    *magic;
    struct task_struct      *owner;         // 写者持有者
    raw_spinlock_t          osq;            // 乐观自旋锁
#endif
    /*
     * 在 PREEMPT_RT 下:
     * rwsem 基于 rwbase_rt, 需要额外的计数器
     */
#ifdef CONFIG_PREEMPT_RT
    struct rwbase_rt        rt;             // RT 读写基础
#endif
};
```

### 6.2.2 count 字段编码

```
atomic_long_t count (64-bit):
┌──────────────────────────────────────────────┬──────┬──────┐
│ 读者计数 (62-bit)                            │  W   │  R   │
│                                              │ AIT  │ EAD  │
│                                              │ ERS  │ LOCK │
│                                              │ 位 1 │ 位 0 │
├──────────────────────────────────────────────┼──────┼──────┤
│ 0                                            │  0   │  0   │  → 未锁定
│ 0                                            │  0   │  1   │  → 写者锁定
│ 0                                            │  1   │  0   │  → 有等待者
│ N                                            │  ?   │  0   │  → N 个读者
└──────────────────────────────────────────────┴──────┴──────┘

#define RWSEM_WRITER_LOCKED    (1UL << 0)   // 写锁定标志
#define RWSEM_FLAG_WAITERS     (1UL << 1)   // 等待者标志
#define RWSEM_READER_FLAG_MASK RWSEM_WRITER_LOCKED  // 读者计数对齐
#define RWSEM_READER_BIAS      RWSEM_WRITER_LOCKED  // 每个读者 +1
```

## 6.3 核心 API

定义在 [include/linux/rwsem.h](file:///home/louis/code/linux/include/linux/rwsem.h)：

```c
// 静态初始化
DECLARE_RWSEM(name);

// 动态初始化
init_rwsem(sem);

// 读者操作
void down_read(struct rw_semaphore *sem);                // 获取读锁
int down_read_trylock(struct rw_semaphore *sem);          // 尝试获取读锁
void up_read(struct rw_semaphore *sem);                   // 释放读锁

// 可中断
int down_read_interruptible(struct rw_semaphore *sem);    // 可被信号中断
int down_read_killable(struct rw_semaphore *sem);         // 可被 SIGKILL 中断

// 写者操作
void down_write(struct rw_semaphore *sem);                // 获取写锁
int down_write_trylock(struct rw_semaphore *sem);         // 尝试获取写锁
void up_write(struct rw_semaphore *sem);                  // 释放写锁

// 可中断
int down_write_killable(struct rw_semaphore *sem);       // 可被 SIGKILL 中断

// 降级
void downgrade_write(struct rw_semaphore *sem);          // 写锁降级为读锁
```

## 6.4 down_read 调用链

```
down_read(sem)                                   [kernel/locking/rwsem.c]
  │
  ├── 1. 快速路径:
  │     └── rwsem_down_read_slowpath(sem, state)
  │           │
  │           └── atomic_long_add(RWSEM_READER_BIAS, &sem->count)
  │                 │
  │                 ├── 之前未被写锁定且无等待者 → 获取成功, 直接返回
  │                 │     (条件: count & ~RWSEM_READER_BIAS 无写锁且无等待者)
  │                 │
  │                 └── 有写锁或等待者 → 进入慢速路径
  │
  └── 2. 慢速路径:
        └── rwsem_down_read_slowpath(sem, TASK_UNINTERRUPTIBLE)
              │
              ├── raw_spin_lock(&sem->wait_lock)
              │
              ├── 加入 wait_list (FIFO)
              │
              ├── raw_spin_unlock(&sem->wait_lock)
              │
              ├── 如果写者正在等待, 读者排队
              │     (写者优先策略, 防止写者饥饿)
              │
              └── schedule() → 睡眠等待
                    │
                    └── 被唤醒 → 获取读锁
```

## 6.5 down_write 调用链

```
down_write(sem)                                  [kernel/locking/rwsem.c]
  │
  ├── 1. 快速路径:
  │     └── atomic_long_cmpxchg_acquire(&sem->count, 0, RWSEM_WRITER_LOCKED)
  │           │
  │           ├── 成功 → 获取写锁, 直接返回
  │           │
  │           └── 失败 → 进入慢速路径
  │
  └── 2. 慢速路径:
        └── rwsem_down_write_slowpath(sem, TASK_UNINTERRUPTIBLE)
              │
              ├── raw_spin_lock(&sem->wait_lock)
              │
              ├── 加入 wait_list (FIFO)
              │
              ├── 设置 RWSEM_FLAG_WAITERS 标志
              │
              ├── raw_spin_unlock(&sem->wait_lock)
              │
              ├── 乐观自旋 (如果持有者正在运行)
              │     └── rwsem_optimistic_spin(sem)
              │           ├── osq_lock(&sem->osq)  → 加入 MCS 队列
              │           └── while (owner_on_cpu(owner)):
              │                 ├── cpu_relax()
              │                 └── 尝试 atomic_try_cmpxchg() 获取
              │
              └── rwsem_sleep_writer(sem)
                    └── schedule() → 睡眠等待
```

## 6.6 乐观自旋优化

### 6.6.1 原理

类似 mutex 的自适应自旋，rwsem 的写者也在持有者运行时短暂自旋：

```c
// kernel/locking/rwsem.c
static bool rwsem_optimistic_spin(struct rw_semaphore *sem)
{
    struct task_struct *owner;

    // 1. 加入乐观自旋队列 (MCS 风格)
    if (!osq_lock(&sem->osq))
        return false;

    while (true) {
        owner = rwsem_get_owner(sem);

        // 2. 如果锁已被释放, 尝试获取
        if (!owner) {
            if (rwsem_try_write_lock(sem)) {
                osq_unlock(&sem->osq);
                return true;
            }
            break;
        }

        // 3. 持有者不在 CPU 上 → 退出自旋
        if (!owner_on_cpu(owner)) {
            break;
        }

        // 4. 其他等待者已获取锁 → 退出自旋
        if (rwsem_is_contended(sem)) {
            break;
        }

        cpu_relax();
    }

    osq_unlock(&sem->osq);
    return false;
}
```

## 6.7 写者优先策略

rwsem 实现写者优先 (writer-biased) 以防止写者饥饿：

```
场景: 读者持续获取锁, 写者无法获取

写者优先策略:
  1. 写者调用 down_write()
  2. 写者加入 wait_list, 设置 RWSEM_FLAG_WAITERS
  3. 新读者调用 down_read():
     a. 检查 count 中的 RWSEM_FLAG_WAITERS
     b. 如果设置了等待者标志 → 新读者加入 wait_list 排队
     c. 不直接获取读锁
  4. 当前读者释放后, 写者获取锁
  5. 写者释放后, 唤醒所有等待的读者
```

## 6.8 PREEMPT_RT 下的 rwsem

在 PREEMPT_RT 下，rwsem 基于 `rwbase_rt` 实现，变为可睡眠的读写锁：

```c
// include/linux/rwbase_rt.h
struct rwbase_rt {
    atomic_t            readers;          // 读者计数 (无符号)
    /*
     * readers 编码:
     *  0xFFEEEEEE : 写者持有
     *  0x00000000 : 未锁定
     *  0x00000001 : 1 个读者
     *  N          : N 个读者
     */
    struct rt_mutex_base *rtmutex;         // 底层 rt_mutex
};
```

**RT vs 非 RT 对比：**

| 特性 | 非 RT | PREEMPT_RT |
|------|-------|------------|
| 实现 | atomic_long_t 计数 + FIFO 队列 | rwbase_rt + rt_mutex |
| 等待行为 | 可睡眠 | 可睡眠 |
| 读者并发 | 多个读者可同时持有 | 多个读者可同时持有 |
| 写者优先 | 是 | 是 |
| 乐观自旋 | 是 | 否 |
| 中断上下文 | 不可用 | 不可用 |

## 6.9 使用场景

| 场景 | 说明 |
|------|------|
| 内存管理 (mmap_lock) | 保护进程地址空间, 读多写少 |
| 文件系统 (i_rwsem) | 保护 inode 操作 |
| 信号量处理 | 保护信号量数据结构 |
| 内核事件通知 | 保护事件监听的注册/注销 |

## 6.10 使用注意事项

```c
// 1. 不可在中断上下文中使用
// down_read()/down_write() 可睡眠

// 2. 读锁升级为写锁 — 必须降级后重新获取
down_read(&sem);
// 读操作...
up_read(&sem);
down_write(&sem);   // 正确: 释放读锁再获取写锁

// 错误: 直接升级会导致死锁
down_read(&sem);
// 尝试 down_write(&sem);  // 死锁! (读者不能升级为写者)

// 3. 写锁降级为读锁
down_write(&sem);
// 写操作...
downgrade_write(&sem);  // 写锁降级为读锁
// 只能读操作...
up_read(&sem);
```

## 6.11 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/rwsem.h](file:///home/louis/code/linux/include/linux/rwsem.h) | rwsem API |
| [kernel/locking/rwsem.c](file:///home/louis/code/linux/kernel/locking/rwsem.c) | rwsem 实现 |
| [include/linux/rwbase_rt.h](file:///home/louis/code/linux/include/linux/rwbase_rt.h) | PREEMPT_RT rwbase 头文件 |
| [kernel/locking/rwbase_rt.c](file:///home/louis/code/linux/kernel/locking/rwbase_rt.c) | PREEMPT_RT rwbase 实现 |