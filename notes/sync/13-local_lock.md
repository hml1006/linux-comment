# 13. 本地锁 (local_lock)

## 13.1 概述

Local lock 是 PREEMPT_RT 内核引入的机制，用于替代在非 RT 内核中通过"禁用抢占 + per-CPU 访问"实现的隐式锁定模式。

**核心问题：**
- 非 RT 内核中，`preempt_disable()` 可保护 per-CPU 数据
- PREEMPT_RT 下 spin_lock 不再禁用抢占，需要显式机制
- local_lock 提供显式的 per-CPU 锁语义

## 13.2 关键数据结构

定义在 [include/linux/local_lock.h](file:///home/louis/code/linux/include/linux/local_lock.h)：

```c
// include/linux/local_lock.h
typedef struct {
    /*
     * 非 RT: 空结构体 (仅编译时检查)
     * RT:    包含 spinlock_t 实现
     */
#ifdef CONFIG_PREEMPT_RT
    spinlock_t          lock;       // RT 下实际的锁
#endif
    struct lockdep_map  dep_map;    // Lockdep 依赖图
} local_lock_t;

// 静态初始化
#define LOCAL_LOCK_INIT(name) {                        \
    .dep_map = { .name = #name },                      \
}

#define DEFINE_LOCAL_LOCK(name)                        \
    local_lock_t name = LOCAL_LOCK_INIT(name)

// 动态初始化
#define local_lock_init(lock)                          \
    do {                                               \
        static struct lock_class_key _key;             \
        lockdep_init_map(&(lock)->dep_map, #lock, &_key, 0); \
    } while (0)
```

## 13.3 核心 API

```c
// include/linux/local_lock.h

// 获取本地锁
void local_lock(local_lock_t *lock);
// 非 RT: preempt_disable()
// RT:    spin_lock(&lock->lock) + migrate_disable()

// 释放本地锁
void local_unlock(local_lock_t *lock);
// 非 RT: preempt_enable()
// RT:    spin_unlock(&lock->lock) + migrate_enable()

// 带 irq 保护的变体
void local_lock_irq(local_lock_t *lock);
void local_unlock_irq(local_lock_t *lock);
void local_lock_irqsave(local_lock_t *lock, unsigned long flags);
void local_unlock_irqrestore(local_lock_t *lock, unsigned long flags);

// 带 softirq 保护的变体
void local_lock_bh(local_lock_t *lock);
void local_unlock_bh(local_lock_t *lock);
```

## 13.4 实现原理

### 13.4.1 非 RT 实现

```c
// 非 RT: local_lock 就是 preempt_disable/enable
#define local_lock(lock)                          \
    do {                                          \
        migrate_disable();                        \
        lock_acquire(&(lock)->dep_map, 0, 0, 0, 1, NULL, _THIS_IP_); \
    } while (0)

#define local_unlock(lock)                        \
    do {                                          \
        lock_release(&(lock)->dep_map, _THIS_IP_); \
        migrate_enable();                         \
    } while (0)
```

### 13.4.2 PREEMPT_RT 实现

```c
// PREEMPT_RT: local_lock 是真实的 spinlock
// 但这是 per-CPU spinlock, 不会跨 CPU 竞争
// 仅用于防止同 CPU 上的抢占/中断/softirq 竞争

#define local_lock(lock)                          \
    do {                                          \
        spin_lock(&(lock)->lock);                 \
        lock_acquire(&(lock)->dep_map, 0, 0, 0, 1, NULL, _THIS_IP_); \
    } while (0)

#define local_unlock(lock)                        \
    do {                                          \
        lock_release(&(lock)->dep_map, _THIS_IP_); \
        spin_unlock(&(lock)->lock);               \
    } while (0)
```

## 13.5 使用场景

| 场景 | 说明 |
|------|------|
| Per-CPU 统计 | 保护 per-CPU 统计计数器 |
| 本地资源池 | 保护 per-CPU 内存池 |
| 网络数据包处理 | 保护 per-CPU 网络队列 |
| 调度器本地数据 | 保护 per-CPU 调度数据 |

## 13.6 使用示例

```c
// 示例: per-CPU 统计计数器
DEFINE_LOCAL_LOCK(lock);
DEFINE_PER_CPU(struct stats, cpu_stats);

void update_stats(size_t bytes)
{
    struct stats *stats;

    local_lock(&lock);
    stats = this_cpu_ptr(&cpu_stats);
    stats->packets++;
    stats->bytes += bytes;
    local_unlock(&lock);
}

void read_stats(void)
{
    unsigned long total_packets = 0;
    unsigned long total_bytes = 0;
    int cpu;

    for_each_possible_cpu(cpu) {
        struct stats *stats = per_cpu_ptr(&cpu_stats, cpu);
        total_packets += stats->packets;
        total_bytes += stats->bytes;
    }
    printk("total: %lu packets, %lu bytes\n", total_packets, total_bytes);
}
```

## 13.7 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/local_lock.h](file:///home/louis/code/linux/include/linux/local_lock.h) | local_lock API |