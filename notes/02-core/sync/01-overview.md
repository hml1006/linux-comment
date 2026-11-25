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

### 1.2.1 每层的落地文件与核心类型

上图每一层都能在源码里找到唯一的"主文件"，查代码时可直接跳到这些位置：

| 层次 | 主要文件 | 核心类型/函数 |
|--|--|--|
| 自旋锁 | `kernel/locking/qspinlock.c`、`kernel/locking/qrwlock.c` | `arch_spinlock_t`、`queued_spin_lock_slowpath()` |
| MCS/OSQ | `kernel/locking/mcs_spinlock.h`、`kernel/locking/osq_lock.c` | `struct mcs_spinlock`、`osq_lock()` |
| 互斥锁 | `kernel/locking/mutex.c` | `struct mutex`、`__mutex_lock_common()` |
| RT 互斥 | `kernel/locking/rtmutex.c`、`rtmutex_api.c` | `struct rt_mutex_base`、`__rt_mutex_slowlock()` |
| 读写信号量 | `kernel/locking/rwsem.c` | `struct rw_semaphore`、`rwsem_down_write_slowpath()` |
| RT 读写信号量 | `kernel/locking/rwbase_rt.c` | `struct rwbase_rt`、`rwbase_read_lock()` |
| 信号量 | `kernel/locking/semaphore.c` | `struct semaphore`、`___down_common()` |
| percpu-rwsem | `kernel/locking/percpu-rwsem.c` | `struct percpu_rw_semaphore` |
| local_lock | `include/linux/local_lock_internal.h` | `local_lock_t`、`__local_lock()` |
| lockdep | `kernel/locking/lockdep.c`、`lockdep_proc.c` | `struct lockdep_map`、`lock_acquire()` |
| 锁事件统计 | `kernel/locking/lock_events.c` | `LOCKEVENT_*`、`debugfs/lock_event_counts/` |
| 锁压力测试 | `kernel/locking/locktorture.c` | `lock_torture_ops` |

`kernel/locking/Makefile` 会根据配置把其中若干文件编进 `built-in.a`；本树 `kernel/locking/` 目录下同时存在 `qspinlock.o`、`rwsem.o`、`rtmutex_api.o`、`percpu-rwsem.o` 等产物，说明这些机制都实际参与了构建。

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

### 1.3.1 机制对比总表

| 机制 | 类型 | 失败/等待方式 | 可睡眠 | 中断上下文可用 | 公平性 | 典型临界区 |
|--|--|--|--|--|--|--|
| `raw_spinlock_t` | 忙等 | 自旋（可关中断） | 否 | 是 | 无 | 几条指令 |
| `spinlock_t` | 忙等 | 自旋 | 非 RT 否 / RT 是 | 是 | 无 | 几十条指令 |
| `rwlock_t` | 读写忙等 | 自旋 | 同上 | 是 | 无 | 读多写少短临界区 |
| `mutex` | 阻塞 | 睡 + 可选乐观自旋 | 是 | 否 | 排队 | 系统调用级 |
| `rw_semaphore` | 读写阻塞 | 睡 + 乐观自旋 | 是 | 否 | 写者优先 | 读多写少长临界区 |
| `rt_mutex` | 阻塞 | 睡 + PI | 是 | 否 | 按优先级 | RT 互斥 |
| `semaphore` | 计数阻塞 | 睡 | 是 | `down_trylock`/`up` 可以 | FIFO | 资源池 |
| `percpu_rw_semaphore` | 读写阻塞 | 快路径无原子操作 | 是 | 否 | 写者等全部读者 | 写极少的全局状态 |
| `local_lock` | 迁移/抢占控制 | 不忙等；RT 下退化为 per-CPU rtmutex | RT 下可以，非 RT 否 | `_irq`/`_bh` 变体 | 无 | per-CPU 数据 |
| `RCU` | 无锁读 | 读者不阻塞 | 读端否 | 读端可以 | 无 | 指针替换 |
| `seqlock` | 无锁读 | 读者重试 | 读端否 | 读端可以 | 写者不等读者 | 一致性快照 |
| `atomic_t`/`bitops` | 无锁 | 不阻塞 | 否 | 是 | — | 计数器/标志位 |
| `completion` | 事件 | 睡 | 是 | `complete` 可以 | 无 | 一次性事件 |

### 1.3.2 PREEMPT_RT 改变了什么

本树 `CONFIG_PREEMPT_RT=y`，这一项会批量改写上表中的"类型"，但不是所有机制都被改写：

| 非 RT 定义 | RT 定义 | 依据 |
|--|--|--|
| `spinlock_t` = `raw_spinlock` 联合体 | `spinlock_t` = `struct rt_mutex_base lock` | `include/linux/spinlock_types.h` 的 `#else` 分支 |
| `rwlock_t` = `arch_rwlock_t` | `rwlock_t` = `rwbase_rt` + `atomic_t readers` | `include/linux/rwlock_types.h` |
| `mutex` = `atomic_long_t owner` + `wait_list` | `mutex` = `rt_mutex_base` | `include/linux/mutex.h` |
| `rw_semaphore` = `count`/`owner`/`osq` | `rw_semaphore` = `rwbase_rt` | `include/linux/rwsem.h` |
| `local_lock_t` = 空结构（仅 lockdep 时是 `dep_map`+`owner`） | `local_lock_t` = `spinlock_t`（即 rtmutex） | `include/linux/local_lock_internal.h` |
| `semaphore` 不变 | `semaphore` 不变（仍用 `raw_spinlock_t`） | `include/linux/semaphore.h` |
| `raw_spinlock_t` 不变 | `raw_spinlock_t` 不变 | 同上 |

RT 的核心承诺是"除 `raw_spinlock` 外的自旋锁都可抢占"，因此由 `spinlock_t` 保护的临界区在 RT 下可能被更高优先级任务打断，而 `raw_spinlock_t` 保护的临界区依旧不可打断。信号量之所以"幸免"，是因为它本来就可睡眠，不需要被改写；代价是它失去了优先级继承。

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

### 1.4.1 上表的依据

这些 "✓/✗" 不是约定，而是由实现强制出来的：

| 机制 | 强制手段 | 代码位置 |
|--|--|--|
| `mutex`/`rwsem`/`semaphore` | `might_sleep()` + `__schedule()` | `kernel/locking/mutex.c`、`rwsem.c`、`semaphore.c` |
| `spinlock_t`（非 RT） | `preempt_disable()` | `include/linux/spinlock.h` |
| `spinlock_t`（RT） | 底层 rtmutex，可睡眠，因此**不能**在硬中断上下文使用 | `include/linux/spinlock_types.h` |
| `raw_spinlock_t` | 真正的自旋，RT 下也不变 | `include/linux/spinlock_types_raw.h` |
| `RCU` 读端 | `rcu_read_lock()` 只关抢占/开屏障，不睡眠 | `include/linux/rcupdate.h` |
| `local_lock` | 非 RT 为 `preempt_disable()`，RT 为 `migrate_disable()+spin_lock()` | `include/linux/local_lock_internal.h` |

严格来说，上表"硬中断里可以用 spinlock"只在**非 RT** 成立；本树开启 `CONFIG_PREEMPT_RT`，硬中断里必须用 `raw_spinlock_t`。若在硬中断里拿了 `spinlock_t`，开启 `CONFIG_DEBUG_ATOMIC_SLEEP` 的内核会打印 "sleeping function called from invalid context"；本树该选项未启用，同一错误往往只表现为难以复现的死锁。这也是 RT 补丁必须把中断路径上的 `spin_lock()` 逐一改成 `raw_spin_lock()` 的原因。

### 1.4.2 判断"能否在此处睡眠"的经验法则

```
当前上下文
  ├─ NMI / 硬中断 / softirq / tasklet  → 绝不能睡眠
  │    ├─ 需要互斥？ → raw_spinlock_t（RT 下也只能 raw_*）
  │    └─ 需要 per-CPU 保护？ → local_lock_irqsave() 或 this_cpu_* 操作
  │
  ├─ 持有 spinlock / rcu_read_lock / 关抢占  → 不能睡眠
  │    └─ 需要"等待"？ → 说明设计有问题，改用完成量或把工作推到 workqueue
  │
  └─ 普通进程上下文 → 可以睡眠
       ├─ 临界区短 → mutex
       └─ 临界区长、读多写少 → rwsem / percpu_rwsem
```

最容易被忽略的是第二类：**持有自旋锁时调用了可能睡眠的函数**。`might_sleep()` 只会在开启 `CONFIG_DEBUG_ATOMIC_SLEEP` 时报警，而本树该选项未启用，因此这类 bug 在运行期往往表现为"偶发卡死"而非明确报错。审查代码时应反向确认：任何 `kmalloc(GFP_KERNEL)`、`copy_to_user()`、`mutex_lock()`、`schedule()` 都不应出现在 `spin_lock()` 与 `spin_unlock()` 之间。

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

### 1.5.1 本树配置下哪些能力真的存在

同步机制的"可用性"高度依赖配置。本树的关键选项实际取值：

| 配置项 | 本树取值 | 影响 |
|--|--|--|
| `CONFIG_SMP` | `y` | 存在真正的跨 CPU 竞争，`qspinlock` 而非 UP 简化版 |
| `CONFIG_PREEMPT_RT` | `y` | `spinlock_t`/`mutex`/`rw_semaphore` 改为 rtmutex 变体 |
| `HZ` | 250 | `down_timeout()` 等超时的粒度是 4 ms |
| `CONFIG_RWSEM_SPIN_ON_OWNER` | `y` | 仅非 RT 生效；RT 下 `rw_semaphore` 走 `rwbase_rt`，此选项无意义 |
| `CONFIG_DEBUG_LOCK_ALLOC` | 未启用 | 所有 `struct lockdep_map` 成员不编译 |
| `CONFIG_PROVE_LOCKING` | 未启用 | 没有死锁检测、锁环报告 |
| `CONFIG_DEBUG_SPINLOCK` | 未启用 | 没有 `magic`/`owner_cpu` 字段，无法检测重复解锁 |
| `CONFIG_DEBUG_RWSEMS` | 未启用 | 没有 `magic` 字段，读/写不平衡检测关闭 |
| `CONFIG_LOCK_EVENT_COUNTS` | 未启用 | 没有 `debugfs/lock_event_counts/` |
| `CONFIG_LOCK_TORTURE_TEST` | 未启用 | 无锁压力测试模块 |
| `CONFIG_DEBUG_ATOMIC_SLEEP` | 未启用 | `might_sleep()` 不报"原子上下文睡眠" |
| `CONFIG_DEBUG_PREEMPT` | 未启用 | 抢占计数不平衡不报警 |

换句话说：本树是"RT + 精简调试"的配置。分析并发问题时不能依赖 lockdep 报告，只能靠代码审查、`trace_contention_*`（也需手动开启 tracing）与自建复现。

### 1.5.2 选择时的常见错误

| 错误 | 现象 | 根因 |
|--|--|--|
| 用 `mutex` 保护中断也会访问的数据 | 中断里报 `scheduling while atomic`，或只能改成 `spin_lock_irqsave` | mutex 不可在中断上下文获取 |
| 在 RT 内核用 `spin_lock` 保护"必须 CPU 本地"的数据 | 数据被别的 CPU 改写 | RT 下 spinlock 不再隐式关抢占，需 `local_lock` 或 `raw_spinlock` |
| 用 `rmb()` 代替 `smp_rmb()` | 在 ARM64 上多一条昂贵屏障 | 区分"编译器屏障""SMP 屏障""I/O 屏障" |
| 读多写少却用 mutex | 读侧并发度差 | 应评估 rwsem/percpu_rwsem/RCU |
| 用 RCU 保护"读者需要稳定快照"的结构 | 读到中间状态 | RCU 只保证"不释放"，不保证一致性，需 seqlock |
| 用 `local_lock` 保护可能被别的 CPU 访问的数据 | 静默数据损坏 | local_lock 不是跨 CPU 互斥 |

关于最后一条，`local_lock` 的名字容易误导：它只阻止**本 CPU** 上的并发（抢占/中断/migration），不提供任何跨 CPU 互斥。用它保护 per-CPU 变量是正确用法，用它保护全局链表则是错误用法。

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