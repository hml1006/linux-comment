# 17. Guard 作用域管理

## 17.1 概述

Guard 是内核引入的基于作用域的锁管理机制，利用 C 语言的变量作用域特性，在进入作用域时自动获取锁，在退出作用域时自动释放锁。类似于 C++ 的 RAII (Resource Acquisition Is Initialization) 模式。

**核心优势：**
- 自动释放：无论正常返回还是异常退出，锁都会被释放
- 减少错误：避免忘记解锁或错误路径未解锁
- 简化代码：减少显式的 lock/unlock 调用

## 17.2 核心 API

定义在 [include/linux/guard.h](file:///home/louis/code/linux/include/linux/guard.h)：

### 17.2.1 基本 Guard

```c
// include/linux/guard.h

// 通用 guard 宏
// lock 是锁对象, 在作用域结束时自动释放
#define guard(mutex)(lock)

// 使用模式:
{
    guard(mutex)(&my_mutex);    // 进入作用域时获取锁
    // ... 临界区代码 ...
    // 离开作用域时自动释放锁
}
```

### 17.2.2 带标签的 Guard

```c
// 支持命名标签, 允许在作用域内提前释放
#define guard(mutex)(lock, label)

{
    guard(mutex)(&my_mutex, label);
    // ... 临界区 ...
    // 使用 goto label 可提前退出
    // 也会自动释放锁
}
```

### 17.2.3 Scoped Guard

```c
// scoped_guard — 带条件检查的 guard
// 条件不满足时, 不会获取锁
#define scoped_guard(mutex, lock) \
    for (int __i = 0; __i < 1; __i++) \
        for (auto __g = guard(mutex)(lock); __i < 1; __i++)

// 使用模式:
{
    scoped_guard(mutex, &my_mutex) {
        // 成功获取锁后执行
        // 临界区代码
    }
    // 离开作用域时自动释放
}
```

### 17.2.4 带条件表达式的 Scoped Guard

```c
// 带条件检查, 条件为真时才获取锁
#define scoped_guard_cond(mutex, lock, cond)

{
    scoped_guard_cond(mutex, &my_mutex, ready) {
        // 只有 ready 为真才获取锁并执行
    }
}
```

## 17.3 支持的锁类型

```c
// include/linux/guard.h — 已定义的 guard 类型

// 自旋锁
guard(spinlock)(&lock);          // spin_lock/spin_unlock
guard(spinlock_irq)(&lock);      // spin_lock_irq/spin_unlock_irq
guard(spinlock_irqsave)(&lock);  // spin_lock_irqsave/spin_unlock_irqrestore

// 互斥锁
guard(mutex)(&lock);             // mutex_lock/mutex_unlock

// 读写信号量
guard(rwsem_read)(&sem);         // down_read/up_read
guard(rwsem_write)(&sem);        // down_write/up_write

// RCU
guard(rcu)();                    // rcu_read_lock/rcu_read_unlock

// 本地锁
guard(local_lock)(&lock);        // local_lock/local_unlock
```

## 17.4 实现原理

### 17.4.1 宏实现

```c
// include/linux/guard.h

// guard 宏的核心实现
#define __guard(locktype, lock, label)                    \
    class_guard_##locktype __UNIQUE_ID(guard)             \
        __attribute__((__cleanup__(guard_exit_##locktype))) \
        = guard_enter_##locktype(lock)

// 属性 __cleanup__ 在变量超出作用域时自动调用清理函数
// 这就是 guard 自动释放锁的关键机制
```

### 17.4.2 清理函数

```c
// 每种锁类型定义 enter/exit 函数

// mutex 的 guard 实现
static inline void guard_enter_mutex(struct mutex *lock)
{
    mutex_lock(lock);
}

static inline void guard_exit_mutex(struct mutex *lock)
{
    mutex_unlock(lock);
}

// spinlock 的 guard 实现
static inline void guard_enter_spinlock(spinlock_t *lock)
{
    spin_lock(lock);
}

static inline void guard_exit_spinlock(spinlock_t *lock)
{
    spin_unlock(lock);
}
```

## 17.5 使用示例

### 17.5.1 传统 vs Guard 对比

```c
// 传统方式: 容易忘记解锁
void traditional_function(struct my_struct *s)
{
    mutex_lock(&s->lock);
    if (s->state == ERROR) {
        // 忘记解锁! 死锁!
        return;
    }
    s->data = 42;
    mutex_unlock(&s->lock);
}

// Guard 方式: 自动解锁
void guard_function(struct my_struct *s)
{
    guard(mutex)(&s->lock);          // 自动获取锁

    if (s->state == ERROR) {
        return;                      // 自动释放锁!
    }
    s->data = 42;
    // 离开作用域时自动释放锁
}
```

### 17.5.2 复杂场景

```c
// 多个锁的保护
void multi_lock_example(struct my_struct *s1, struct my_struct *s2)
{
    // 按顺序获取锁, 避免死锁
    guard(mutex)(&s1->lock);       // 先获取 s1 的锁
    guard(mutex)(&s2->lock);       // 再获取 s2 的锁

    // 操作两个结构体
    s1->data = s2->data;

    // 离开作用域时自动释放 s2 的锁, 再释放 s1 的锁
}

// 条件守卫
void conditional_guard(struct my_struct *s, bool need_lock)
{
    if (need_lock) {
        guard(mutex)(&s->lock);
        // 需要锁的分支
        s->data = 42;
        // 自动释放
    }

    // 不需要锁的分支
    s->other = 0;
}
```

## 17.6 使用场景

| 场景 | 适合的 Guard 类型 | 说明 |
|------|-------------------|------|
| 简单临界区 | `guard(mutex)` | 自动获取/释放 mutex |
| 中断保护 | `guard(spinlock_irq)` | 自动关中断 |
| 读锁保护 | `guard(rwsem_read)` | 自动获取/释放读锁 |
| 写锁保护 | `guard(rwsem_write)` | 自动获取/释放写锁 |
| RCU 读端 | `guard(rcu)` | 自动进入/退出 RCU 读端 |
| Per-CPU 保护 | `guard(local_lock)` | 自动获取/释放本地锁 |

## 17.7 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/guard.h](file:///home/louis/code/linux/include/linux/guard.h) | Guard 宏定义 |