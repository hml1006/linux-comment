# 5. RT 互斥锁与优先级继承

## 5.1 概述

RT mutex 是支持优先级继承 (Priority Inheritance) 的互斥锁，专门解决实时系统中的**优先级反转 (Priority Inversion)** 问题。

**核心问题——优先级反转：**

```
优先级反转场景:
  高优先级任务 H 等待低优先级任务 L 持有的锁,
  但 L 被中优先级任务 M 抢占,
  导致 H 被 M 间接阻塞 — 优先级反转!

  正常: H > M > L (优先级)
  实际: H 等待 L → L 被 M 抢占 → H 等待 M 完成 → 优先级反转!
```

**解决方案——优先级继承：**

```
优先级继承:
  当 L 持有 H 等待的锁时,
  L 临时提升到 H 的优先级 (继承),
  阻止 M 抢占 L,
  L 快速完成临界区后释放锁,
  恢复原始优先级.
```

## 5.2 关键数据结构

### 5.2.1 struct rt_mutex_base

定义在 [include/linux/rtmutex.h](file:///home/louis/code/linux/include/linux/rtmutex.h)：

```c
// include/linux/rtmutex.h
struct rt_mutex_base {
    raw_spinlock_t          wait_lock;       // 保护 wait_list 的自旋锁
    struct rb_root_cached   waiters;         // 红黑树等待队列 (按优先级排序)
    struct task_struct      *owner;          // 持有者 (含低 2 位标志)
};
```

### 5.2.2 struct rt_mutex

```c
// include/linux/rtmutex.h
struct rt_mutex {
    struct rt_mutex_base    rtmutex;         // 基础结构
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map      dep_map;
#endif
};
```

### 5.2.3 owner 字段编码

```c
struct task_struct *owner;  // 低 2 位用作标志位

#define RTMUTEX_HAS_WAITERS     1UL  // 位 0: 有等待者
#define RTMUTEX_TASK_TO_OWNER(t) ((unsigned long)(t) & ~RTMUTEX_HAS_WAITERS)
#define RTMUTEX_OWNER_TO_TASK(o) ((struct task_struct *)((unsigned long)(o) & ~RTMUTEX_HAS_WAITERS))
```

### 5.2.4 等待者节点

```c
// kernel/locking/rtmutex.c
struct rt_mutex_waiter {
    struct rb_node          tree_entry;      // 红黑树节点 (按优先级排序)
    struct rb_node          pi_tree_entry;   // PI 传播树节点
    struct task_struct      *task;           // 等待任务
    struct rt_mutex_base    *lock;           // 等待的锁
    int                     prio;            // 等待优先级
    bool                    deadlock;        // 死锁检测标记
};
```

## 5.3 优先级继承机制

### 5.3.1 核心原理

```
优先级继承传播链:

  锁等待关系:
  Task H (prio=0) → 等待 lock_A (被 Task L 持有)
  Task L (prio=10) → 持有 lock_A, 等待 lock_B (被 Task M 持有)
  Task M (prio=20) → 持有 lock_B

  优先级继承传播:
  Step 1: H 等待 lock_A → L 继承 H 的优先级 (prio 0→10→0)
  Step 2: L 等待 lock_B → M 继承 L 的优先级 (prio 20→10→0)
  Step 3: M 释放 lock_B → M 恢复原始优先级 20
  Step 4: L 获取 lock_B → L 释放 lock_A → L 恢复原始优先级 10
  Step 5: H 获取 lock_A → 继续执行
```

### 5.3.2 实现

实现于 [kernel/locking/rtmutex.c](file:///home/louis/code/linux/kernel/locking/rtmutex.c)：

```c
// 核心锁获取函数
static int __sched
rt_mutex_slowlock(struct rt_mutex_base *lock, int state,
                  struct hrtimer_sleeper *timeout,
                  enum rtmutex_chainwalk chwalk)
{
    struct rt_mutex_waiter waiter;

    // 1. 初始化等待者节点
    rt_mutex_init_waiter(&waiter, false);

    // 2. 加入等待队列 (红黑树, 按优先级排序)
    raw_spin_lock(&lock->wait_lock);

    // 3. 尝试获取锁
    if (try_to_take_rt_mutex(lock, current, NULL))
        goto unlock;

    // 4. 设置等待者优先级
    waiter.task = current;
    waiter.prio = current->prio;

    // 5. 将等待者加入锁的等待队列
    rt_mutex_enqueue(lock, &waiter);

    // 6. 触发优先级继承
    //    如果锁被持有, 提升持有者的优先级
    rt_mutex_adjust_prio_chain(current, waiter.prio, lock, &waiter, chwalk);

    // 7. 睡眠等待
    __set_current_state(state);
    raw_spin_unlock(&lock->wait_lock);

    // 8. 调度出去
    schedule();

    // 9. 被唤醒后, 从等待队列移除
    raw_spin_lock(&lock->wait_lock);
    rt_mutex_dequeue(lock, &waiter);

unlock:
    raw_spin_unlock(&lock->wait_lock);

    // 10. 恢复优先级
    rt_mutex_adjust_prio_chain(current, current->prio, NULL, NULL, RT_MUTEX_FULL_CHAINWALK);

    return 0;
}
```

### 5.3.3 优先级继承传播

```c
// kernel/locking/rtmutex.c
static int __sched
rt_mutex_adjust_prio_chain(struct task_struct *task,
                           u8 prio,                    // 需要继承的优先级
                           struct rt_mutex_base *lock, // 当前锁
                           struct rt_mutex_waiter *waiter,
                           enum rtmutex_chainwalk chwalk)
{
    // 沿锁等待链递归传播优先级
    while (true) {
        // 1. 更新 task 的优先级
        rt_mutex_update_prio(task, prio);

        // 2. 如果 task 没有等待其他锁, 停止传播
        if (!rt_mutex_has_waiters(task->pi_blocked_on))
            break;

        // 3. 获取 task 正在等待的锁
        next_lock = task->pi_blocked_on->lock;

        // 4. 获取锁的持有者
        next_owner = rt_mutex_owner(next_lock);

        // 5. 递归到下一个持有者
        //    传播路径: H → L → M → ...
        task = next_owner;
        waiter = task->pi_blocked_on;
        prio = waiter->prio;
    }
}
```

## 5.4 rt_mutex_lock 调用链

### 5.4.1 快速路径 (无竞争)

```
rt_mutex_lock(lock)                              [kernel/locking/rtmutex.c]
  │
  └── __rt_mutex_lock(lock, TASK_UNINTERRUPTIBLE)
        │
        ├── try_to_take_rt_mutex(lock, current, NULL)  → 尝试获取
        │     │
        │     └── cmpxchg_acquire(&lock->owner, NULL, current)
        │           │
        │           ├── 成功 → 直接返回
        │           │
        │           └── 失败 → rt_mutex_slowlock()
        │
        └── rt_mutex_slowlock(lock, state, NULL, RT_MUTEX_MIN_CHAINWALK)
              │
              └── [见 5.4.2 慢速路径]
```

### 5.4.2 慢速路径 (有竞争)

```
rt_mutex_slowlock():
  │
  ├── 1. 初始化等待者
  │
  ├── 2. rt_mutex_enqueue(lock, &waiter)
  │     └── 按优先级插入红黑树
  │
  ├── 3. rt_mutex_adjust_prio_chain()
  │     └── 触发优先级继承
  │           └── 提升持有者及其依赖链的优先级
  │
  ├── 4. schedule()
  │     └── 睡眠等待
  │
  └── 5. 被唤醒后
        ├── rt_mutex_dequeue(lock, &waiter)
        └── try_to_take_rt_mutex() → 获取锁
```

### 5.4.3 rt_mutex_unlock 调用链

```
rt_mutex_unlock(lock)                            [kernel/locking/rtmutex.c]
  │
  └── __rt_mutex_unlock(lock)
        │
        ├── 1. 标记 owner 为 RTMUTEX_HAS_WAITERS
        │
        ├── 2. 检查是否有等待者
        │     └── 无等待者 → 清除 owner, 直接返回
        │
        └── 3. 有等待者 → rt_mutex_slowunlock(lock)
              │
              ├── raw_spin_lock(&lock->wait_lock)
              │
              ├── rt_mutex_dequeue_top_waiter(lock)
              │     └── 取出红黑树中优先级最高的等待者
              │
              ├── rt_mutex_setprio(waiter->task, waiter->prio)
              │     └── 恢复等待者的优先级
              │
              ├── rt_mutex_adjust_prio_chain(owner, ...)
              │     └── 恢复持有者的优先级 (继承结束)
              │
              ├── wake_up_process(waiter->task)
              │     └── 唤醒最高优先级的等待者
              │
              └── raw_spin_unlock(&lock->wait_lock)
```

## 5.5 优先级继承传播流程

```
场景: H(prio=0) 等待 L(prio=10) 持有的锁, L 等待 M(prio=20) 持有的锁

H 调用 rt_mutex_lock(lock_A):
  │
  ├── lock_A 被 L 持有
  │
  ├── H 加入 lock_A 的等待队列 (红黑树)
  │
  └── rt_mutex_adjust_prio_chain(H, prio=0, lock_A, waiter_H):
        │
        ├── H 的优先级不变 (prio=0)
        │
        ├── L 持有 lock_A → 检查 L 的优先级
        │     └── L.prio=10 > 0 → 提升 L.prio=0 (继承 H 的优先级)
        │
        ├── L 的优先级提升后, L 也被更早调度
        │
        ├── L 在等待 lock_B → 继续传播
        │     └── rt_mutex_adjust_prio_chain(L, prio=0, lock_B, waiter_L):
        │           │
        │           ├── M 持有 lock_B → 检查 M 的优先级
        │           │     └── M.prio=20 > 0 → 提升 M.prio=0
        │           │
        │           └── M 没有等待其他锁 → 停止传播
        │
        └── 传播结束

M 释放 lock_B:
  │
  ├── M 恢复原始优先级 (prio=20)
  │
  ├── L 获取 lock_B
  │
  └── L 释放 lock_A:
        ├── L 恢复原始优先级 (prio=10)
        └── H 获取 lock_A → 继续执行
```

## 5.6 PREEMPT_RT 中的核心作用

在 PREEMPT_RT 内核中，rt_mutex 是锁体系的基础，spinlock_t 和 mutex 都基于 rt_mutex：

```
PREEMPT_RT 锁体系:
  ┌─────────────────────────────────────┐
  │  raw_spinlock_t                     │ ← 唯一不变的自旋锁
  │  (仍为 qspinlock, 不可睡眠)         │
  ├─────────────────────────────────────┤
  │  spinlock_t → 基于 rt_mutex_base    │ ← 可睡眠
  │  rwlock_t    → 基于 rwbase_rt       │ ← 可睡眠
  │  mutex       → 基于 rt_mutex_base   │ ← 可睡眠 (含 PI)
  │  rwsem       → 基于 rwbase_rt       │ ← 可睡眠
  │  local_lock  → 基于 migrate_disable │ ← 禁止迁移
  └─────────────────────────────────────┘
```

## 5.7 使用场景

| 场景 | 说明 |
|------|------|
| 实时任务互斥 | 防止优先级反转 |
| PREEMPT_RT spinlock 实现 | spinlock_t 的底层实现 |
| PREEMPT_RT mutex 实现 | mutex 的底层实现 |
| FUTEX 实时锁 | 用户态实时锁 |
| 内核 PI 睡眠锁 | 内核中需要 PI 的锁 |

## 5.8 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/rtmutex.h](file:///home/louis/code/linux/include/linux/rtmutex.h) | rt_mutex API |
| [kernel/locking/rtmutex.c](file:///home/louis/code/linux/kernel/locking/rtmutex.c) | rt_mutex 实现 |
| [kernel/locking/rtmutex_api.c](file:///home/louis/code/linux/kernel/locking/rtmutex_api.c) | rt_mutex API 导出 |
| [kernel/locking/spinlock_rt.c](file:///home/louis/code/linux/kernel/locking/spinlock_rt.c) | PREEMPT_RT 自旋锁 |