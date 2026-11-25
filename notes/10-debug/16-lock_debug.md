# 锁调试功能

## 概述

Linux 内核提供了多种锁调试功能，用于检测锁使用错误和收集锁相关统计信息：

- **spinlock_debug**: 调试 spinlock/rwlock 的使用
- **mutex-debug**: 调试 mutex 的使用
- **irqflag-debug**: 调试中断标志的使用
- **lock_events**: 收集锁事件统计信息

## spinlock_debug

### 概述

spinlock_debug 用于检测 spinlock 和 rwlock 的使用错误，如递归加锁、错误的解锁者、magic 校验失败等。

### 核心数据结构

```c
typedef struct raw_spinlock {
    arch_spinlock_t raw_lock;
    struct lockdep_map dep_map;
#ifdef CONFIG_DEBUG_SPINLOCK
    unsigned int magic;
    struct task_struct *owner;
    int owner_cpu;
#endif
} raw_spinlock_t;

#define SPINLOCK_MAGIC 0xdead4ead
#define SPINLOCK_OWNER_INIT ((struct task_struct *)-1L)
```

调试字段：
- `magic`: 魔数，用于检测内存损坏
- `owner`: 当前持有锁的任务
- `owner_cpu`: 当前持有锁的 CPU

### 初始化

```c
void __raw_spin_lock_init(raw_spinlock_t *lock, const char *name,
                          struct lock_class_key *key, short inner)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    debug_check_no_locks_freed((void *)lock, sizeof(*lock));
    lockdep_init_map_wait(&lock->dep_map, name, key, 0, inner);
#endif
    lock->raw_lock = (arch_spinlock_t)__ARCH_SPIN_LOCK_UNLOCKED;
    lock->magic = SPINLOCK_MAGIC;
    lock->owner = SPINLOCK_OWNER_INIT;
    lock->owner_cpu = -1;
}
```

### 加锁检查

```c
static inline void debug_spin_lock_before(raw_spinlock_t *lock)
{
    SPIN_BUG_ON(READ_ONCE(lock->magic) != SPINLOCK_MAGIC, lock, "bad magic");
    SPIN_BUG_ON(READ_ONCE(lock->owner) == current, lock, "recursion");
    SPIN_BUG_ON(READ_ONCE(lock->owner_cpu) == raw_smp_processor_id(),
                                    lock, "cpu recursion");
}

static inline void debug_spin_lock_after(raw_spinlock_t *lock)
{
    WRITE_ONCE(lock->owner_cpu, raw_smp_processor_id());
    WRITE_ONCE(lock->owner, current);
}
```

检查项：
- **bad magic**: lock 结构被破坏
- **recursion**: 当前任务递归加锁
- **cpu recursion**: 同一 CPU 递归加锁

### 解锁检查

```c
static inline void debug_spin_unlock(raw_spinlock_t *lock)
{
    SPIN_BUG_ON(lock->magic != SPINLOCK_MAGIC, lock, "bad magic");
    SPIN_BUG_ON(!raw_spin_is_locked(lock), lock, "already unlocked");
    SPIN_BUG_ON(lock->owner != current, lock, "wrong owner");
    SPIN_BUG_ON(lock->owner_cpu != raw_smp_processor_id(),
                                    lock, "wrong CPU");
    WRITE_ONCE(lock->owner, SPINLOCK_OWNER_INIT);
    WRITE_ONCE(lock->owner_cpu, -1);
}
```

检查项：
- **bad magic**: lock 结构被破坏
- **already unlocked**: 锁未被持有
- **wrong owner**: 错误的解锁者
- **wrong CPU**: 错误的 CPU 解锁

### rwlock 调试

rwlock 调试与 spinlock 类似，使用 `RWLOCK_MAGIC` 魔数：

```c
#define RWLOCK_MAGIC 0xdeaf1eed
```

读写锁的写锁路径同样检查 magic、递归、owner 和 owner_cpu。

## mutex-debug

### 概述

mutex-debug 用于检测 mutex 的使用错误，如 waiter 结构损坏、任务阻塞状态不一致等。

### 调试检查

```c
void debug_mutex_lock_common(struct mutex *lock, struct mutex_waiter *waiter)
{
    memset(waiter, MUTEX_DEBUG_INIT, sizeof(*waiter));
    waiter->magic = waiter;
    INIT_LIST_HEAD(&waiter->list);
    waiter->ww_ctx = MUTEX_POISON_WW_CTX;
}

void debug_mutex_wake_waiter(struct mutex *lock, struct mutex_waiter *waiter)
{
    lockdep_assert_held(&lock->wait_lock);
    DEBUG_LOCKS_WARN_ON(list_empty(&lock->wait_list));
    DEBUG_LOCKS_WARN_ON(waiter->magic != waiter);
    DEBUG_LOCKS_WARN_ON(list_empty(&waiter->list));
}

void debug_mutex_free_waiter(struct mutex_waiter *waiter)
{
    DEBUG_LOCKS_WARN_ON(!list_empty(&waiter->list));
    memset(waiter, MUTEX_DEBUG_FREE, sizeof(*waiter));
}

void debug_mutex_remove_waiter(struct mutex *lock, struct mutex_waiter *waiter,
                               struct task_struct *task)
{
    struct mutex *blocked_on = __get_task_blocked_on(task);

    DEBUG_LOCKS_WARN_ON(list_empty(&waiter->list));
    DEBUG_LOCKS_WARN_ON(waiter->task != task);
    DEBUG_LOCKS_WARN_ON(blocked_on && blocked_on != lock);

    INIT_LIST_HEAD(&waiter->list);
    waiter->task = NULL;
}
```

### mutex 销毁

```c
void mutex_destroy(struct mutex *lock)
{
    DEBUG_LOCKS_WARN_ON(mutex_is_locked(lock));
    lock->magic = NULL;
}
```

确保销毁时 mutex 未被持有。

## irqflag-debug

### 概述

irqflag-debug 用于检测中断标志的错误使用，特别是 `raw_local_irq_restore()` 被调用时中断已经启用的情况。

### 核心函数

```c
noinstr void warn_bogus_irq_restore(void)
{
    instrumentation_begin();
    WARN_ONCE(1, "raw_local_irq_restore() called with IRQs enabled\n");
    instrumentation_end();
}
```

当检测到 `raw_local_irq_restore()` 被调用时中断已经启用，会发出警告。

## lock_events

### 概述

lock_events 用于收集锁相关的统计信息，包括 qspinlock、rwsem、rtmutex、lockdep 等的事件计数。

### 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                      lock_events Architecture                      │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                  锁操作路径                                  │   │
│  │  qspinlock / rwsem / rtmutex / lockdep                     │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │              lockevent_inc() / lockevent_add()              │   │
│  │  - 每 CPU 计数器，使用 raw_cpu_inc() 减少开销               │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │              per_cpu(lockevents[event])                     │   │
│  │  - 每 CPU 数组存储各事件计数                                 │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │              debugfs 接口                                   │   │
│  │  /sys/kernel/debug/lock_event_counts/                      │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### 事件类型

| 类别 | 事件 | 描述 |
|------|------|------|
| **PV qspinlock** | `pv_kick_unlock` | 解锁时发出的 vCPU kick 次数 |
| | `pv_kick_wake` | 用于 pv_latency_wake 的 vCPU kick 次数 |
| | `pv_lock_stealing` | 锁窃取操作次数 |
| | `pv_spurious_wakeup` | 非头部 vCPU 的虚假唤醒次数 |
| **qspinlock** | `lock_pending` | 通过 pending 代码的加锁操作次数 |
| | `lock_slowpath` | 通过 MCS 锁队列的加锁操作次数 |
| | `lock_use_node2/3/4` | 使用第 2/3/4 个 per CPU 节点的次数 |
| | `lock_no_node` | 未使用 per CPU 节点的加锁次数 |
| **rwsem** | `rwsem_sleep_reader` | 读者睡眠次数 |
| | `rwsem_sleep_writer` | 写者睡眠次数 |
| | `rwsem_opt_lock` | 优化获取的写锁次数 |
| | `rwsem_opt_fail` | 优化自旋失败次数 |
| | `rwsem_rlock` | 获取的读锁次数 |
| | `rwsem_wlock` | 获取的写锁次数 |
| **rtlock** | `rtlock_slowlock` | rtlock_slowlock() 调用次数 |
| | `rtlock_slow_sleep` | 睡眠次数 |
| | `rtlock_slow_wake` | 唤醒次数 |
| **rtmutex** | `rtmutex_slowlock` | rt_mutex_slowlock() 调用次数 |
| | `rtmutex_deadlock` | 死锁处理次数 |
| **lockdep** | `lockdep_acquire` | lockdep 获取次数 |
| | `lockdep_lock` | lockdep 加锁次数 |

### debugfs 接口

```bash
# 目录位置
/sys/kernel/debug/lock_event_counts/

# 可用文件
pv_kick_unlock        - PV kick 解锁计数
lock_slowpath         - qspinlock 慢速路径计数
rwsem_sleep_reader    - rwsem 读者睡眠计数
rwsem_sleep_writer    - rwsem 写者睡眠计数
rtmutex_deadlock      - rtmutex 死锁计数
...
.reset_counts         - 重置所有计数 (写入)
```

### 使用示例

```bash
# 查看 qspinlock 慢速路径计数
cat /sys/kernel/debug/lock_event_counts/lock_slowpath

# 查看 rwsem 读者睡眠计数
cat /sys/kernel/debug/lock_event_counts/rwsem_sleep_reader

# 重置所有计数
echo > /sys/kernel/debug/lock_event_counts/.reset_counts
```

### 实现机制

```c
DECLARE_PER_CPU(unsigned long, lockevents[lockevent_num]);

static inline void __lockevent_inc(enum lock_events event, bool cond)
{
    if (cond)
        raw_cpu_inc(lockevents[event]);
}

#define lockevent_inc(ev)       __lockevent_inc(LOCKEVENT_ ##ev, true)
#define lockevent_cond_inc(ev, c) __lockevent_inc(LOCKEVENT_ ##ev, c)
```

使用每 CPU 变量和 `raw_cpu_inc()` 减少开销，适合生产环境使用。

## 编译配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_DEBUG_SPINLOCK` | 启用 spinlock 调试 |
| `CONFIG_DEBUG_MUTEXES` | 启用 mutex 调试 |
| `CONFIG_DEBUG_IRQFLAGS` | 启用中断标志调试 |
| `CONFIG_LOCK_EVENT_COUNTS` | 启用锁事件计数 |

## 性能影响

| 功能 | 内存开销 | CPU 开销 |
|------|----------|----------|
| **spinlock_debug** | 每个锁增加 ~24 字节 | 每次加锁/解锁增加少量检查 |
| **mutex-debug** | 每个 waiter 增加少量字段 | 每次操作增加少量检查 |
| **irqflag-debug** | 无额外开销 | 仅在检测到错误时发出警告 |
| **lock_events** | 每 CPU ~2KB | 使用 raw_cpu_inc()，开销极低 |

## 使用场景

1. **驱动开发**：在开发过程中启用锁调试，检测锁使用错误
2. **性能分析**：使用 lock_events 收集锁相关统计信息
3. **问题排查**：当系统出现死锁或锁竞争时，启用锁调试定位问题

## 代码位置

| 文件 | 说明 |
|------|------|
| `kernel/locking/spinlock_debug.c` | spinlock/rwlock 调试实现 |
| `kernel/locking/mutex-debug.c` | mutex 调试实现 |
| `kernel/locking/irqflag-debug.c` | 中断标志调试实现 |
| `kernel/locking/lock_events.c` | 锁事件计数实现 |
| `kernel/locking/lock_events.h` | 锁事件头文件 |
| `kernel/locking/lock_events_list.h` | 锁事件列表定义 |