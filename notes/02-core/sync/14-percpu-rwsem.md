# 14. Per-CPU RWSEM (percpu-rwsem)

## 14.1 概述

Per-CPU RWSEM 是一种基于 per-CPU 计数器的读写信号量，针对"读非常频繁、写极少"的场景优化。读者仅操作 per-CPU 计数器（无竞争），写者需要等待所有 CPU 的读者完成。

**核心特性：**
- 读者极快：仅操作 per-CPU 计数器，无需原子操作
- 写者批量等待：写者等待所有读者释放
- 适合读极多写极少的场景

## 14.2 关键数据结构

定义在 [include/linux/percpu-rwsem.h](file:///home/louis/code/linux/include/linux/percpu-rwsem.h)：

```c
// include/linux/percpu-rwsem.h
struct percpu_rw_semaphore {
    /*
     * rcu_sync 用于协调读者和写者之间的 RCU 宽限期,
     * 确保写者看到所有读者已完成
     */
    struct rcu_sync          rss;        // RCU 同步

    /*
     * read_count 是 per-CPU 变量,
     * 每个 CPU 维护一个本地读者计数
     */
    unsigned int __percpu    *read_count; // per-CPU 读者计数

    /*
     * 写者锁, 使用 rt_mutex (PREEMPT_RT) 或 rwsem
     */
    struct rcuwait           writer;     // 写者等待队列
    wait_queue_head_t        waiters;    // 等待者队列
    struct rw_semaphore      rw_sem;     // 底层 rwsem (非 RT)

    /*
     * 写者状态
     * 0 = 无写者, 1 = 有写者或等待
     */
    atomic_t                 block;      // 写者阻塞标志
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map       dep_map;
#endif
};
```

## 14.3 核心 API

```c
// include/linux/percpu-rwsem.h

// 初始化
void percpu_free_rwsem(struct percpu_rw_semaphore *sem);
int percpu_init_rwsem(struct percpu_rw_semaphore *sem);

// 读者操作
void percpu_down_read(struct percpu_rw_semaphore *sem);
void percpu_up_read(struct percpu_rw_semaphore *sem);

// 写者操作
void percpu_down_write(struct percpu_rw_semaphore *sem);
void percpu_up_write(struct percpu_rw_semaphore *sem);
```

## 14.4 工作原理

### 14.4.1 读者路径

```
percpu_down_read(sem):
  │
  ├── rcu_read_lock()
  │     └── 在 RCU 读端临界区内保证 per-CPU 访问安全
  │
  ├── if (atomic_read(&sem->block) == 0):
  │     └── __this_cpu_inc(sem->read_count)  → 本地计数 +1 (无原子操作)
  │
  └── else:
        └── 有写者 → 进入慢速路径
              └── percpu_down_read_slowpath(sem)
                    ├── __this_cpu_dec(sem->read_count)  → 撤销计数
                    └── down_read(&sem->rw_sem)          → 获取 rwsem 读锁

percpu_up_read(sem):
  │
  └── __this_cpu_dec(sem->read_count)  → 本地计数 -1 (无原子操作)
        │
        └── rcu_read_unlock()
```

### 14.4.2 写者路径

```
percpu_down_write(sem):
  │
  ├── down_write(&sem->rw_sem)         → 获取 rwsem 写锁
  │
  ├── atomic_set(&sem->block, 1)       → 设置阻塞标志
  │
  ├── synchronize_rcu()                → 等待 RCU 宽限期
  │     └── 确保所有读者看到 block=1
  │
  └── 遍历所有 CPU:
        └── while (per_cpu(*sem->read_count, cpu) > 0):
              └── 等待该 CPU 上的读者释放

percpu_up_write(sem):
  │
  ├── atomic_set(&sem->block, 0)       → 清除阻塞标志
  │
  └── up_write(&sem->rw_sem)           → 释放 rwsem 写锁
```

## 14.5 使用场景

| 场景 | 说明 |
|------|------|
| 文件系统挂载状态 | 检查文件系统是否挂载 (读极多, 写极少) |
| 设备热插拔 | 检查设备是否在线 |
| 模块引用计数 | 检查模块是否已卸载 |
| 全局配置 | 检查配置是否正在变更 |

## 14.6 使用示例

```c
// 示例: 保护文件系统挂载状态
struct percpu_rw_semaphore sb_lock;  // superblock 操作锁
struct super_block *sb;

// 读者: 检查挂载状态
int is_mounted(struct super_block *sb)
{
    int ret;

    percpu_down_read(&sb_lock);
    ret = sb->s_flags & SB_ACTIVE;  // 读挂载标志
    percpu_up_read(&sb_lock);

    return ret;
}

// 写者: 卸载
void unmount_sb(struct super_block *sb)
{
    percpu_down_write(&sb_lock);
    sb->s_flags &= ~SB_ACTIVE;      // 清除挂载标志
    // 执行卸载操作...
    percpu_up_write(&sb_lock);
}
```

## 14.7 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/percpu-rwsem.h](file:///home/louis/code/linux/include/linux/percpu-rwsem.h) | percpu-rwsem API |
| [kernel/locking/percpu-rwsem.c](file:///home/louis/code/linux/kernel/locking/percpu-rwsem.c) | percpu-rwsem 实现 |