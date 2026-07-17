# 3. 自旋锁

## 3.1 概述

自旋锁是最基本的锁机制：当锁被其他 CPU 持有时，等待者会"自旋"（忙等），直到锁被释放。适用于临界区极小（微秒级）的场景。

**核心特性：**
- 忙等：等待时不调度，不睡眠，占着 CPU 自旋
- 低延迟：临界区极短时，自旋比上下文切换更高效
- 上下文敏感：可在中断上下文中使用（raw_spinlock）

## 3.2 关键数据结构

### 3.2.1 raw_spinlock_t

定义在 [include/linux/spinlock_types_raw.h](file:///home/louis/code/linux/include/linux/spinlock_types_raw.h)：

```c
// include/linux/spinlock_types_raw.h
context_lock_struct(raw_spinlock) {
    arch_spinlock_t raw_lock;           // 架构相关实现 (qspinlock)
#ifdef CONFIG_DEBUG_SPINLOCK
    unsigned int magic;                 // 魔数: SPINLOCK_MAGIC = 0xdead4ead
    unsigned int owner_cpu;             // 持有者 CPU 编号
    void *owner;                         // 持有者 task_struct 指针
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map dep_map;         // Lockdep 依赖图
#endif
};
typedef struct raw_spinlock raw_spinlock_t;
```

### 3.2.2 spinlock_t (非 PREEMPT_RT)

定义在 [include/linux/spinlock_types.h](file:///home/louis/code/linux/include/linux/spinlock_types.h)：

```c
// 非 PREEMPT_RT: spinlock_t 包装 raw_spinlock_t
#ifndef CONFIG_PREEMPT_RT

context_lock_struct(spinlock) {
    union {
        struct raw_spinlock rlock;       // 实际就是 raw_spinlock
#ifdef CONFIG_DEBUG_LOCK_ALLOC
        struct {
            u8 __padding[LOCK_PADSIZE];  // 填充到 raw_spinlock.dep_map 偏移
            struct lockdep_map dep_map;
        };
#endif
    };
};
typedef struct spinlock spinlock_t;
```

### 3.2.3 arch_spinlock_t (qspinlock)

在 ARM64 上，`arch_spinlock_t` 是排队自旋锁 (qspinlock)：

```c
// arch/arm64/include/asm/spinlock_types.h
typedef struct {
    union {
        u32 lock;                       // 32-bit 编码
        struct {
            u16 locked;                 // 锁持有者计数
            u16 pending;                // 挂起等待者
        };
    };
} arch_spinlock_t;
```

**qspinlock 编码格式：**

```
arch_spinlock_t (32-bit):
┌──────┬──────────────────────────────────────┐
│ lock │               tail                   │
│ 1bit │    31 bits (CPU# + nesting)         │
├──────┼──────────────────────────────────────┤
│  0=未锁 │  尾部等待者的 CPU 编号 + 嵌套层   │
│  1=已锁 │                                   │
└──────┴──────────────────────────────────────┘
```

## 3.3 核心 API

### 3.3.1 raw_spinlock API

```c
// include/linux/spinlock.h

// 初始化
raw_spin_lock_init(lock);

// 基本操作
raw_spin_lock(lock);                        // 获取锁 (自旋等待)
raw_spin_unlock(lock);                      // 释放锁
raw_spin_trylock(lock);                     // 尝试获取, 成功返回 1, 失败返回 0

// 关中断变体
raw_spin_lock_irq(lock);                    // 获取锁 + 关闭本地 CPU 中断
raw_spin_unlock_irq(lock);                  // 释放锁 + 开启本地 CPU 中断
raw_spin_lock_irqsave(lock, flags);         // 获取锁 + 保存并关闭中断
raw_spin_unlock_irqrestore(lock, flags);    // 释放锁 + 恢复中断

// 关软中断变体
raw_spin_lock_bh(lock);                     // 获取锁 + 关闭 softirq
raw_spin_unlock_bh(lock);                   // 释放锁 + 开启 softirq
```

### 3.3.2 spinlock API

```c
// 非 PREEMPT_RT 下, spin_lock() 直接映射到 raw_spin_lock()
static __always_inline void spin_lock(spinlock_t *lock)
{
    raw_spin_lock(&lock->rlock);
}

static __always_inline void spin_unlock(spinlock_t *lock)
{
    raw_spin_unlock(&lock->rlock);
}

static __always_inline void spin_lock_irqsave(spinlock_t *lock, unsigned long flags)
{
    raw_spin_lock_irqsave(&lock->rlock, flags);
}
```

## 3.4 qspinlock 排队自旋锁

### 3.4.1 原理

传统自旋锁的问题：所有等待者在同一个共享变量上自旋，导致严重的缓存行乒乓。

qspinlock 基于 MCS 锁的思想，让每个等待者在自己的本地变量上自旋：

```
传统自旋锁 (缓存行乒乓):
  CPU 0: 持有锁
  CPU 1: 自旋在 lock→ 缓存行在 CPU 0/1/2 间跳动
  CPU 2: 自旋在 lock→

qspinlock (排队自旋):
  CPU 0: 持有锁
  CPU 1: 自旋在 node1→locked (本地缓存)
  CPU 2: 自旋在 node2→locked (本地缓存)
  └── 解锁时仅唤醒下一个等待者, 缓存行只在相邻 CPU 间传递
```

### 3.4.2 实现

实现于 [kernel/locking/qspinlock.c](file:///home/louis/code/linux/kernel/locking/qspinlock.c)：

```c
// qspinlock 快速路径
void queued_spin_lock(struct qspinlock *lock)
{
    // 1. 快速路径: 尝试直接获取锁
    if (likely(atomic_try_cmpxchg_acquire(&lock->val, 0, _Q_LOCKED_VAL)))
        return;

    // 2. 慢速路径: 排队等待
    queued_spin_lock_slowpath(lock, atomic_read(&lock->val));
}
```

**qspinlock 慢速路径流程：**

```
queued_spin_lock_slowpath():
  │
  ├── pending 位处理 (第 1 位等待者)
  │     ├── 尝试设置 pending 位
  │     ├── 成功 → 自旋等待锁释放, 然后获取锁
  │     └── 失败 → 进入排队
  │
  ├── queue 排队 (MCS 风格)
  │     ├── 分配 per-CPU mcs_spinlock 节点
  │     ├── 通过 xchg() 将节点加入队列尾部
  │     └── 在本地节点的 locked 标志上自旋
  │
  └── unlock 传播:
        └── 解锁时, 如果队列不空, 设置下一个节点的 locked 标志
              └── 下一个等待者从本地自旋中退出, 获取锁
```

### 3.4.3 MCS 锁节点

```c
// kernel/locking/mcs_spinlock.h
struct mcs_spinlock {
    struct mcs_spinlock *next;     // 指向队列中的下一个节点
    int locked;                    // 1 = 锁已获取 (本地自旋变量)
    int count;                     // 嵌套计数
};
```

## 3.5 qrwlock 排队读写锁

### 3.5.1 数据结构

```c
// include/asm-generic/qrwlock_types.h
typedef struct qrwlock {
    union {
        atomic_t cnts;              // 32-bit 计数器
        struct {
#ifdef __LITTLE_ENDIAN
            u8 wlocked;             // 写锁定标志 (位 0-7)
            u8 __lstate[3];         // 保留
#else
            u8 __lstate[3];
            u8 wlocked;
#endif
        };
    };
    arch_spinlock_t wait_lock;      // 等待队列保护锁
} arch_rwlock_t;
```

**cnts 字段编码：**

```
qrwlock.cnts (32-bit):
┌──────────┬──────────┬──────────┬──────────┐
│ byte 3   │ byte 2   │ byte 1   │ byte 0   │
│ __lstate │ __lstate │ __lstate │ wlocked  │
├──────────┴──────────┴──────────┼──────────┤
│  读者计数 (24-bit)             │ 写锁标志 │
│  每读一次 + _QR_BIAS (0x100)  │ 0x01=写锁│
└────────────────────────────────┴──────────┘
```

### 3.5.2 读写锁工作原理

实现于 [kernel/locking/qrwlock.c](file:///home/louis/code/linux/kernel/locking/qrwlock.c)：

```
读锁获取:
  queued_read_lock():
    1. atomic_add(_QR_BIAS, &cnts)  → 读者计数 +1
    2. if (!(cnts & _QW_LOCKED))    → 无写锁, 获取成功
    3. 有写锁 → queued_read_lock_slowpath()
       a. atomic_sub(_QR_BIAS, &cnts)  → 撤销计数
       b. arch_spin_lock(&wait_lock)    → 排队等待
       c. 等待 !(cnts & _QW_LOCKED)    → 等待写锁释放
       d. arch_spin_unlock(&wait_lock)  → 出队

写锁获取:
  queued_write_lock():
    1. atomic_cmpxchg(&cnts, 0, _QW_LOCKED)  → 尝试获取
    2. 成功 → 返回
    3. 失败 → queued_write_lock_slowpath()
       a. atomic_or(_QW_LOCKED, &cnts)  → 设置写锁标志
       b. 等待 cnts == _QW_LOCKED       → 等待所有读者释放
       c. arch_spin_lock(&wait_lock)    → 排队 (公平性)
       d. arch_spin_unlock(&wait_lock)  → 出队
```

## 3.6 PREEMPT_RT 下的自旋锁

### 3.6.1 数据结构

```c
// CONFIG_PREEMPT_RT 下 spinlock_t 基于 rt_mutex
context_lock_struct(spinlock) {
    struct rt_mutex_base    lock;       // 可睡眠的 rt_mutex
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map      dep_map;
#endif
};
```

### 3.6.2 实现

实现于 [kernel/locking/spinlock_rt.c](file:///home/louis/code/linux/kernel/locking/spinlock_rt.c)：

```c
// RT 下 spin_lock 的实现
static __always_inline void __rt_spin_lock(spinlock_t *lock)
{
    rtlock_might_resched();              // 检查是否需要调度
    rtlock_lock(&lock->lock);            // 获取 rt_mutex (可睡眠)
    rcu_read_lock();                     // 模拟 spin_lock 的 RCU 保护
    migrate_disable();                   // 禁止迁移到其他 CPU
}

void __sched rt_spin_lock(spinlock_t *lock)
{
    spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
    __rt_spin_lock(lock);
}
```

### 3.6.3 非 RT vs RT 对比

| 特性 | 非 RT | PREEMPT_RT |
|------|-------|------------|
| 实现 | qspinlock (arch_spinlock_t) | rt_mutex |
| 等待行为 | 自旋 (占 CPU) | 阻塞 (可睡眠) |
| 抢占 | 禁用抢占 | 不禁用 |
| 迁移 | 隐式禁止 (通过禁抢占) | 显式 `migrate_disable()` |
| RCU | 隐式保护 (禁抢占即 RCU) | 显式 `rcu_read_lock()` |
| 可用上下文 | 任意 (含中断) | 仅进程上下文 |
| 延迟 | 极低 (ns 级) | 较高 (μs 级, 可调度) |
| **raw_spinlock** | 不变 | 不变 (仍为自旋锁) |

## 3.7 调用栈流程

### 3.7.1 spin_lock 调用链 (非 RT)

```
spin_lock(lock)
  │
  └── raw_spin_lock(&lock->rlock)        [include/linux/spinlock.h]
        │
        └── _raw_spin_lock(lock)          [kernel/locking/spinlock.c]
              │
              └── __raw_spin_lock(lock)   [include/linux/spinlock_api_smp.h]
                    │
                    ├── preempt_disable()           → 禁止抢占
                    │
                    ├── LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock)
                    │     │
                    │     └── do_raw_spin_lock(lock)  [include/linux/spinlock.h]
                    │           │
                    │           └── arch_spin_lock(&lock->raw_lock)  [arch/arm64/include/asm/spinlock.h]
                    │                 │
                    │                 └── queued_spin_lock(&lock->val)  [kernel/locking/qspinlock.c]
                    │                       │
                    │                       ├── atomic_try_cmpxchg_acquire() → 快速路径
                    │                       └── queued_spin_lock_slowpath()  → 排队自旋
                    │
                    └── mmiowb_spin_lock()          → MMIO 写屏障
```

### 3.7.2 spin_unlock 调用链 (非 RT)

```
spin_unlock(lock)
  │
  └── raw_spin_unlock(&lock->rlock)      [include/linux/spinlock.h]
        │
        └── _raw_spin_unlock(lock)        [kernel/locking/spinlock.c]
              │
              └── __raw_spin_unlock(lock) [include/linux/spinlock_api_smp.h]
                    │
                    ├── do_raw_spin_unlock(lock)    [include/linux/spinlock.h]
                    │     └── arch_spin_unlock(&lock->raw_lock)
                    │           └── queued_spin_unlock(&lock->val)
                    │                 └── smp_store_release(&lock->locked, 0)
                    │                       └── 如果队列不空, 唤醒下一个等待者
                    │
                    └── preempt_enable()            → 重新允许抢占
```

## 3.8 使用场景

| 场景 | 推荐锁类型 | 变体 |
|------|-----------|------|
| 保护进程上下文共享数据 | `spinlock_t` | `spin_lock()` |
| 中断上下文中保护数据 | `raw_spinlock_t` | `raw_spin_lock_irqsave()` |
| softirq/tasklet 中保护数据 | `raw_spinlock_t` | `raw_spin_lock_bh()` |
| 调度器内部 | `raw_spinlock_t` | `raw_spin_lock()` |
| 读多写少, 短临界区 | `rwlock_t` | `read_lock()/write_lock()` |
| PREEMPT_RT 下的中断上下文 | `raw_spinlock_t` (唯一选择) | `raw_spin_lock()` |

## 3.9 使用注意事项

```c
// 1. 临界区必须极短
// 自旋锁持有者禁用抢占, 如果临界区过长, 会影响实时性

// 2. 不可递归
// 自旋锁不可递归获取, 否则死锁
spin_lock(&lock);
spin_lock(&lock);  // 死锁!

// 3. 关中断 vs 不关中断
// 如果中断处理程序也获取同一锁, 必须使用 _irqsave 变体
spin_lock_irqsave(&lock, flags);   // 关闭中断, 防止死锁
// ... 临界区 ...
spin_unlock_irqrestore(&lock, flags);

// 4. PREEMPT_RT 下 spin_lock 不可在 raw_spin_lock 临界区内使用
raw_spin_lock(&raw_lock);
spin_lock(&spin_lock);  // RT 下可能死锁! (spin_lock 可睡眠)
raw_spin_unlock(&raw_lock);
```

## 3.10 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/spinlock.h](file:///home/louis/code/linux/include/linux/spinlock.h) | 自旋锁 API |
| [include/linux/spinlock_types.h](file:///home/louis/code/linux/include/linux/spinlock_types.h) | spinlock_t 定义 |
| [include/linux/spinlock_types_raw.h](file:///home/louis/code/linux/include/linux/spinlock_types_raw.h) | raw_spinlock_t 定义 |
| [include/linux/spinlock_rt.h](file:///home/louis/code/linux/include/linux/spinlock_rt.h) | PREEMPT_RT 自旋锁 API |
| [kernel/locking/spinlock.c](file:///home/louis/code/linux/kernel/locking/spinlock.c) | 自旋锁实现 |
| [kernel/locking/spinlock_rt.c](file:///home/louis/code/linux/kernel/locking/spinlock_rt.c) | PREEMPT_RT 自旋锁实现 |
| [kernel/locking/qspinlock.c](file:///home/louis/code/linux/kernel/locking/qspinlock.c) | 排队自旋锁实现 |
| [kernel/locking/qrwlock.c](file:///home/louis/code/linux/kernel/locking/qrwlock.c) | 排队读写锁实现 |
| [kernel/locking/mcs_spinlock.h](file:///home/louis/code/linux/kernel/locking/mcs_spinlock.h) | MCS 锁头文件 |