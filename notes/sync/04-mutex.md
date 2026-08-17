# 4. 互斥锁 (mutex)

## 4.1 概述

Mutex 是可睡眠的互斥锁，适用于临界区较长的场景。当锁被其他任务持有时，等待者会睡眠并调度出去，而不是自旋忙等。

**核心特性：**
- 可睡眠：等待时调度出去，不占 CPU
- 严格语义：只能由持有者解锁，不可递归
- 自适应自旋：在持有者正在运行时，短暂自旋避免上下文切换
- 优先级继承 (PREEMPT_RT)：解决优先级反转问题

## 4.2 关键数据结构

### 4.2.1 struct mutex (非 PREEMPT_RT)

定义在 [include/linux/mutex_types.h](file:///home/louis/code/linux/include/linux/mutex_types.h)：

```c
#ifndef CONFIG_PREEMPT_RT

context_lock_struct(mutex) {
    atomic_long_t           owner;          // 持有者 (含标志位)
    raw_spinlock_t          wait_lock;      // 保护 wait_list 的自旋锁
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
    struct optimistic_spin_queue osq;       // 乐观自旋 MCS 队列
#endif
    struct list_head        wait_list;      // 等待者链表 (FIFO)
#ifdef CONFIG_DEBUG_MUTEXES
    void                    *magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map      dep_map;
#endif
};
```

### 4.2.2 owner 字段编码

```
atomic_long_t owner (64-bit):
┌─────────────────────────────────────────────────────────────┐
│ 位 [63:3]           │  位 [2]  │  位 [1]  │  位 [0]        │
├─────────────────────┼──────────┼──────────┼────────────────┤
│  task_struct 指针    │  HAS_DOWN│  HANDOFF │  WAITERS       │
│  (指向持有者)       │  (读锁定) │  (移交)  │  (有等待者)    │
└─────────────────────┴──────────┴──────────┴────────────────┘

#define MUTEX_FLAGS                 (0x07)  // 低 3 位标志掩码
#define MUTEX_WAITERS               (0x01)  // 有等待者在 wait_list 中
#define MUTEX_HANDOFF               (0x02)  // 解锁时直接移交给等待者
#define MUTEX_HAS_DOWN              (0x04)  // 读锁定 (用于 ww_mutex)
```

### 4.2.3 struct mutex (PREEMPT_RT)

```c
#ifdef CONFIG_PREEMPT_RT

struct mutex {
    struct rt_mutex_base    lock;           // 基于 rt_mutex
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map      dep_map;
#endif
};
```

## 4.3 核心 API

定义在 [include/linux/mutex.h](file:///home/louis/code/linux/include/linux/mutex.h)：

```c
// 静态初始化
DEFINE_MUTEX(mutexname);

// 动态初始化
mutex_init(mutex);

// 基本操作
void mutex_lock(struct mutex *lock);                // 获取锁 (可睡眠)
int mutex_lock_interruptible(struct mutex *lock);    // 可被信号中断
int mutex_lock_killable(struct mutex *lock);         // 可被 SIGKILL 中断
int mutex_trylock(struct mutex *lock);               // 尝试获取, 不睡眠

void mutex_unlock(struct mutex *lock);              // 释放锁

// 调试
int mutex_is_locked(struct mutex *lock);
int mutex_owner(struct mutex *lock);                // 返回持有者 task_struct
```

## 4.4 mutex_lock 完整调用链

### 4.4.1 快速路径 (无竞争)

```
mutex_lock(lock)                           [kernel/locking/mutex.c]
  │
  └── __mutex_fastpath_lock(lock)
        │
        └── atomic_long_try_cmpxchg_acquire(&lock->owner, 0UL, task)
              │
              ├── 成功 → owner = task, 直接返回
              │         (无竞争, 微秒级完成)
              │
              └── 失败 → __mutex_lock_slowpath(lock)
```

### 4.4.2 慢速路径 (有竞争)

```
__mutex_lock_slowpath(lock)                [kernel/locking/mutex.c:770]
  │
  ├── 1. 乐观自旋阶段
  │     └── mutex_optimistic_spin(lock, ww_ctx, &flags)
  │           │
  │           ├── osq_lock(&lock->osq)     → 加入乐观自旋 MCS 队列
  │           │
  │           └── while (true):
  │                 ├── 检查持有者是否仍在 CPU 上运行
  │                 │     └── mutex_spin_on_owner()
  │                 │           ├── owner_on_cpu(owner) ? 继续自旋 : 退出
  │                 │           └── __mutex_owner(lock) == owner ? 继续 : 退出
  │                 │
  │                 └── 尝试获取锁
  │                       └── atomic_long_try_cmpxchg_acquire() → 成功则返回
  │
  ├── 2. 睡眠等待阶段
  │     └── __mutex_lock_common()
  │           │
  │           ├── set_current_state(TASK_UNINTERRUPTIBLE)
  │           │
  │           ├── raw_spin_lock(&lock->wait_lock)    → 获取等待队列锁
  │           │
  │           ├── list_add_tail(&waiter.list, &lock->wait_list) → 加入等待队列
  │           │
  │           ├── raw_spin_unlock(&lock->wait_lock)  → 释放等待队列锁
  │           │
  │           └── schedule()                         → 调度出去
  │                 │
  │                 └── 被唤醒后:
  │                       ├── 尝试获取锁
  │                       └── 获取成功 → 从 wait_list 移除
  │
  └── 3. 获取成功, 返回
```

### 4.4.3 mutex_unlock 调用链

```
mutex_unlock(lock)                         [kernel/locking/mutex.c]
  │
  ├── __mutex_fastpath_unlock(lock)
  │     │
  │     └── atomic_long_cmpxchg_release(&lock->owner, task, 0UL)
  │           │
  │           ├── 成功 → 直接返回 (无等待者)
  │           │
  │           └── 失败 → __mutex_unlock_slowpath(lock)
  │
  └── __mutex_unlock_slowpath(lock)
        │
        ├── raw_spin_lock(&lock->wait_lock)
        │
        ├── 从 wait_list 中取出第一个等待者
        │
        ├── 设置 MUTEX_HANDOFF 标志 → 移交锁
        │
        ├── wake_up_process(waiter->task) → 唤醒等待者
        │
        └── raw_spin_unlock(&lock->wait_lock)
```

## 4.5 自适应自旋

### 4.5.1 原理

Mutex 的自适应自旋 (adaptive spinning) 是一种优化：当持有者正在 CPU 上运行时，等待者短暂自旋等待，避免上下文切换的开销。

```c
// kernel/locking/mutex.c
static bool mutex_optimistic_spin(struct mutex *lock,
                                  struct ww_acquire_ctx *ww_ctx,
                                  struct mutex_waiter *waiter)
{
    // 仅在持有者正在运行时才自旋
    // 如果持有者被调度出去, 立即停止自旋, 进入睡眠
    while (true) {
        struct task_struct *owner;

        owner = __mutex_owner(lock);
        if (!owner)     // 锁已释放, 尝试获取
            break;

        if (!owner_on_cpu(owner))  // 持有者不在 CPU 上
            return false;           // 退出自旋, 进入睡眠

        // 检查是否需要刷新 CPU bug 的 TLB
        if (need_resched())
            return false;

        cpu_relax();                // 自旋等待
    }

    // 尝试获取锁
    return atomic_long_try_cmpxchg_acquire(&lock->owner, 0UL, task);
}
```

### 4.5.2 决策流程

```
mutex_lock() 进入:
  │
  ├── 持有者正在 CPU 上运行?
  │     ├── 是 → 自旋等待 (节省切换时间)
  │     └── 否 → 进入睡眠 (避免浪费 CPU)
  │
  ├── 自旋中持有者被调度出去?
  │     └── 立即退出自旋, 进入睡眠
  │
  └── 自旋中锁被释放?
        └── 尝试获取, 成功则返回
```

## 4.6 使用场景

| 场景 | 推荐 API | 说明 |
|------|---------|------|
| 文件系统操作 | `mutex_lock()` | 保护 inode, dentry |
| 内存分配 | `mutex_lock()` | 保护内存管理数据结构 |
| 设备驱动 I/O | `mutex_lock_interruptible()` | 允许被信号中断 |
| 可能被杀死 | `mutex_lock_killable()` | 被 SIGKILL 中断 |
| 尝试获取 | `mutex_trylock()` | 非阻塞检查 |

## 4.7 使用注意事项

```c
// 1. 不可在中断上下文中使用
// mutex_lock() 可睡眠, 中断上下文不可睡眠
void irq_handler(void)
{
    mutex_lock(&lock);    // 错误! 可能导致系统崩溃
    // ...
    mutex_unlock(&lock);
}

// 2. 不可递归
mutex_lock(&lock);
mutex_lock(&lock);    // 死锁! (mutex 不可递归)

// 3. 必须由持有者解锁
mutex_lock(&lock);
// 临界区...
mutex_unlock(&lock);  // 必须是同一 task

// 4. 临界区内不能使用可能导致睡眠的函数
// 正确的使用模式:
mutex_lock(&lock);
// 快速操作共享数据...
mutex_unlock(&lock);

// 较长的操作 (如 copy_to_user) 可能触发调度
// 应在解锁后执行
```

## 4.8 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/mutex.h](file:///home/louis/code/linux/include/linux/mutex.h) | mutex API |
| [include/linux/mutex_types.h](file:///home/louis/code/linux/include/linux/mutex_types.h) | mutex 数据结构 |
| [kernel/locking/mutex.c](file:///home/louis/code/linux/kernel/locking/mutex.c) | mutex 实现 |
| [kernel/locking/osq_lock.c](file:///home/louis/code/linux/kernel/locking/osq_lock.c) | OSQ 乐观自旋队列 |
| [include/linux/ww_mutex.h](file:///home/louis/code/linux/include/linux/ww_mutex.h) | Wound-Wait mutex |