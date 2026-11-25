# 1. 概述 — Linux 内核并发与同步机制

## 1.1 内核并发的来源

Linux 内核是一个多任务、多处理器系统，并发访问可能来自以下来源：

```
并发来源:
  ├── SMP (对称多处理): 多个 CPU 同时执行
  │     └── 同时访问共享数据 → 竞争条件
  │
  ├── 抢占: 高优先级任务抢占低优先级任务
  │     └── 被抢占点可能在临界区中间
  │
  ├── 中断上下文:
  │     ├── 硬中断 (IRQ): 异步硬件事件
  │     └── 软中断 (softirq/tasklet): 中断下半部
  │
  └── 进程间同步:
        └── 一个任务等待另一个任务完成
```

## 1.2 同步机制层次结构

```
┌──────────────────────────────────────────────────────────────┐
│                     应用层 / 系统调用                         │
├──────────────────────────────────────────────────────────────┤
│  Guard/Scoped API  │  Completions  │  Wait Queues            │
│  (自动释放锁)      │  (任务同步)    │  (条件等待)             │
├──────────────────────────────────────────────────────────────┤
│  RCU  │  Seqlock  │  Per-CPU  │  Local Lock                │
│ (无锁读)│ (重试读)  │ (无共享)   │ (per-CPU 锁)              │
├───────────────────┬──────────────────────────────────────────┤
│  Mutex  │  RWSEM  │  RT-Mutex  │  Semaphore  │  percpu-     │
│ (可睡眠) │ (读写分) │  (PI继承)   │  (计数)     │  rwsem      │
├───────────────────┴──────────────────────────────────────────┤
│  Spinlock  │  Raw Spinlock  │  RWlock  │  MCS/OSQ Lock      │
│  (qspinlock)│  (qspinlock)  │ (qrwlock)│  (排队自旋)         │
├──────────────────────────────────────────────────────────────┤
│  Atomic Operations (atomic_t, atomic_long_t, bitops)         │
├──────────────────────────────────────────────────────────────┤
│  硬件层面: 内存屏障  │  Cache Coherency  │  DMA 一致性       │
└──────────────────────────────────────────────────────────────┘
```

## 1.3 同步机制分类

### 按等待行为分类

| 类别 | 机制 | 等待行为 |
|------|------|---------|
| 自旋锁类 | spinlock, raw_spinlock, rwlock, qspinlock, MCS | 忙等 (spin) |
| 可睡眠锁 | mutex, rwsem, rt_mutex, semaphore | 阻塞 (sleep) |
| 无锁同步 | atomic_t, RCU, seqlock, per-CPU | 无阻塞 |
| 同步原语 | completion, wait_queue | 事件驱动 |

### 按使用场景分类

```
场景 1: 短临界区 (几行代码, 微秒级)
  └→ spinlock / raw_spinlock

场景 2: 长临界区 (文件操作, I/O)
  └→ mutex / rwsem

场景 3: 读多写少
  ├── RCU      (读者完全无锁)
  ├── rwsem    (读端可并发)
  ├── rwlock   (读端可并发)
  └── seqlock  (读者无锁, 写者互斥)

场景 4: 中断上下文
  └→ raw_spinlock (唯一选择)

场景 5: 单 CPU 本地数据
  ├── per-CPU 变量 (完全无锁)
  └── local_lock (加锁但无迁移)
```

## 1.4 上下文规则

不同上下文下可使用的同步机制：

```
              硬中断  │  softirq  │  进程上下文
                     │  tasklet  │
─────────────────────┼───────────┼──────────────
raw_spinlock    ✓    │    ✓      │     ✓
spinlock        ✓    │    ✓      │     ✓
rwlock          ✓    │    ✓      │     ✓
mutex           ✗    │    ✗      │     ✓
rwsem           ✗    │    ✗      │     ✓
RCU (读端)      ✓    │    ✓      │     ✓
RCU (同步)      ✗    │    ✗      │     ✓
completion      ✗    │    ✗      │     ✓
```

## 1.5 选择决策树

```
需要保护共享数据?
  │
  ├── 数据可分区给每个 CPU?
  │     └── per-CPU 变量 (最佳性能)
  │
  ├── 只有计数器?
  │     └── atomic_t
  │
  ├── 读极多, 写极少?
  │     ├── RCU (读者最快)
  │     └── seqlock (写者优先)
  │
  ├── 临界区很短? (微秒级)
  │     ├── 中断上下文? → raw_spinlock
  │     └── 进程上下文 → spinlock
  │
  └── 临界区可能很长?
        ├── 需要读写分离? → rwsem
        └── 互斥访问 → mutex
```

## 1.6 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/spinlock.h](file:///home/louis/code/linux/include/linux/spinlock.h) | 自旋锁 API |
| [include/linux/mutex.h](file:///home/louis/code/linux/include/linux/mutex.h) | 互斥锁 API |
| [include/linux/rcupdate.h](file:///home/louis/code/linux/include/linux/rcupdate.h) | RCU API |
| [include/linux/atomic.h](file:///home/louis/code/linux/include/linux/atomic.h) | 原子操作 API |
| [include/linux/wait.h](file:///home/louis/code/linux/include/linux/wait.h) | 等待队列 |
| [kernel/locking/](file:///home/louis/code/linux/kernel/locking/) | 锁实现目录 |
| [kernel/rcu/](file:///home/louis/code/linux/kernel/rcu/) | RCU 实现目录 |