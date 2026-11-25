# Tasklet — 小任务（已弃用）

## 1 实现原理

Tasklet 是一种基于 Softirq（`TASKLET_SOFTIRQ` 和 `HI_SOFTIRQ`）的下半部机制，已被标记为 deprecated，建议迁移到 BH Workqueue。核心设计如下：

- **每 CPU 链表**：每个 CPU 维护 `tasklet_vec`（普通）和 `tasklet_hi_vec`（高优先级）两个链表头。
- **串行保证**：同一类型的 tasklet 在同一 CPU 上串行执行，不同 tasklet 之间可并行。
- **disable 计数**：`tasklet_struct.count` 原子计数，非零时禁止执行，可用于临时禁用。
- **状态标志**：`state` 字段包含 `TASKLET_STATE_SCHED`（已调度）和 `TASKLET_STATE_RUN`（正在执行）。

## 2 使用场景

- **旧式驱动下半部**：网卡、USB 等传统驱动。
- **简单串行化任务**：不需要 sleep 的简短处理。
- **状态**：已被标记为 deprecated，新代码应使用 BH Workqueue。

## 3 代码调用栈

```
调度 tasklet:
tasklet_schedule(t)
  └→ __tasklet_schedule(t)
      └→ t->next = NULL
      └→ 将 t 加入 per-CPU tasklet_vec 链表尾部
      └→ raise_softirq_irqoff(TASKLET_SOFTIRQ)

执行:
softirq 处理 → TASKLET_SOFTIRQ
  └→ tasklet_action()
      └→ tasklet_action_common(&tasklet_vec, TASKLET_SOFTIRQ)
          └→ 取链表头，清空链表
          └→ 遍历每个 tasklet:
              ├→ tasklet_trylock(t)  (确保不并发执行)
              ├→ atomic_read(&t->count) == 0? (未被禁用)
              ├→ tasklet_clear_sched(t)
              ├→ t->callback(t) 或 t->func(t->data)
              └→ tasklet_unlock(t)
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    Tasklet 调度与执行流程                          │
│                                                                   │
│  tasklet_schedule(t)                                              │
│    └→ 加锁 (local_irq_save)                                      │
│    └→ 将 t 加入 per-CPU tasklet_vec 链表尾部                     │
│    └→ raise_softirq_irqoff(TASKLET_SOFTIRQ)                     │
│    └→ 解锁 (local_irq_restore)                                   │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    tasklet_action_common()                        │
│                                                                   │
│  1. 关中断，取链表头，清空链表                                   │
│  2. 开中断                                                       │
│  3. 遍历链表:                                                    │
│     ┌─────────────────────────────────────────────────────┐       │
│     │ for each tasklet in list:                           │       │
│     │   ├─ tasklet_trylock() 成功?                        │       │
│     │   │   ├─ count == 0? (未被禁用)                     │       │
│     │   │   │   ├─ 清除 SCHED 标志                        │       │
│     │   │   │   ├─ 执行 callback 或 func                  │       │
│     │   │   │   └─ tasklet_unlock()                       │       │
│     │   │   └─ count != 0 → 放回链表尾部                  │       │
│     │   └─ tasklet_trylock() 失败 → 放回链表尾部          │       │
│     └─────────────────────────────────────────────────────┘       │
│                                                                   │
│  tasklet_struct 状态机:                                           │
│  ┌─────────┐  tasklet_schedule()  ┌───────────┐                  │
│  │  idle   │ ───────────────────→ │  SCHED    │                  │
│  └─────────┘                      └─────┬─────┘                  │
│       ▲                                │                         │
│       │                             执行开始                      │
│       │                                │                         │
│       │                          ┌─────▼─────┐                  │
│       │                          │ RUN│SCHED │                  │
│       │                          └─────┬─────┘                  │
│       │                             执行结束                      │
│       └──────────────────────────────────┘                       │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct tasklet_struct` — Tasklet 描述符

```c
// include/linux/interrupt.h (或相关头文件)
struct tasklet_struct {
    struct tasklet_struct *next;        // 链表下一个节点
    unsigned int state;                 // 状态: TASKLET_STATE_SCHED, TASKLET_STATE_RUN
    atomic_t count;                     // 禁用计数: 0=可用, >0=禁用
    bool use_callback;                  // true: 使用 callback, false: 使用 func/data
    union {
        void (*func)(unsigned long data);  // 旧式回调 (use_callback=false)
        void (*callback)(struct tasklet_struct *t); // 新式回调 (use_callback=true)
    };
    unsigned long data;                 // 传递给 func 的参数
};
```

### 状态标志位

```c
// include/linux/interrupt.h
enum {
    TASKLET_STATE_SCHED,    // 0: 已调度，等待执行
    TASKLET_STATE_RUN       // 1: 正在执行中
};
```

### 每 CPU 链表

```c
// kernel/softirq.c
struct tasklet_head {
    struct tasklet_struct *head;    // 链表头
    struct tasklet_struct **tail;   // 链表尾指针
};

static DEFINE_PER_CPU(struct tasklet_head, tasklet_vec);     // 普通 tasklet
static DEFINE_PER_CPU(struct tasklet_head, tasklet_hi_vec);  // 高优先级 tasklet
```

### 初始化 API

```c
// 新式 (推荐)
void tasklet_setup(struct tasklet_struct *t,
                   void (*callback)(struct tasklet_struct *));

// 旧式
void tasklet_init(struct tasklet_struct *t,
                  void (*func)(unsigned long), unsigned long data);

// 调度
void tasklet_schedule(struct tasklet_struct *t);
void tasklet_hi_schedule(struct tasklet_struct *t);

// 禁用/启用
void tasklet_disable(struct tasklet_struct *t);
void tasklet_enable(struct tasklet_struct *t);
```

## 6 本树实现形态核实

先读源码确认：本树 tasklet 仍是**基于 softirq 的经典实现**（`kernel/softirq.c` 末尾的 "Tasklets" 段），并未被 BH workqueue 完全取代；`tasklet_action()` 里先跑 `workqueue_softirq_action()` 再跑 `tasklet_action_common()`，二者共存。

`struct tasklet_struct` 实际定义在 `include/linux/interrupt.h`，注意 `state` 是 `unsigned long`（第 5 节示意写的是 `unsigned int`，以源码为准），且 `func`/`callback` 是 union：

```c
// include/linux/interrupt.h
struct tasklet_struct {
    struct tasklet_struct *next;
    unsigned long state;        // TASKLET_STATE_SCHED / TASKLET_STATE_RUN
    atomic_t count;             // 0=可用，>0=禁用
    bool use_callback;
    union {
        void (*func)(unsigned long data);          // 旧式
        void (*callback)(struct tasklet_struct *t); // 新式
    };
    unsigned long data;
};
```

`tasklet_schedule()` 是内联函数，先用 `test_and_set_bit(TASKLET_STATE_SCHED)` 去重，只有首次置位才真正入队：

```c
// include/linux/interrupt.h
static inline void tasklet_schedule(struct tasklet_struct *t)
{
    if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
        __tasklet_schedule(t);
}
```

## 7 调度与执行调用链

`__tasklet_schedule()` → `__tasklet_schedule_common()`，关中断后把 tasklet 挂到每 CPU 链表尾，再 raise softirq：

```c
// kernel/softirq.c: __tasklet_schedule_common()
local_irq_save(flags);
head = this_cpu_ptr(headp);
t->next = NULL;
*head->tail = t;
head->tail = &(t->next);
raise_softirq_irqoff(softirq_nr);   // TASKLET_SOFTIRQ 或 HI_SOFTIRQ
local_irq_restore(flags);
```

执行侧 `tasklet_action_common()` 先关中断摘走整条链表并重置头尾，开中断后逐个处理。`tasklet_trylock()` 即 `!test_and_set_bit(TASKLET_STATE_RUN, &t->state)`，保证同一 tasklet 不并发；`count==0` 才执行，`tasklet_clear_sched()` 用 `test_and_clear_wake_up_bit(TASKLET_STATE_SCHED, ...)` 清调度位（清不掉会 `WARN_ONCE`）。执行完 `tasklet_unlock()` 用 `clear_and_wake_up_bit(TASKLET_STATE_RUN, ...)` 清运行位并唤醒等待者。被禁用或 trylock 失败的 tasklet 重新挂回链表尾并 `__raise_softirq_irqoff()` 下次再试。

`tasklet_setup()`/`tasklet_init()` 分别设 `use_callback=true/false`，初始化 `state=0`、`count=0`。静态初始化用 `DECLARE_TASKLET`（新式）或 `DECLARE_TASKLET_OLD`（旧式），`from_tasklet()` 宏由成员反推宿主结构。

## 8 禁用、杀死与 PREEMPT_RT 同步

- `tasklet_disable()`：`atomic_inc(&t->count)` 后 `tasklet_unlock_wait()`（`wait_on_bit(TASKLET_STATE_RUN)`）再 `smp_mb()`，确保正在跑的回调结束。`tasklet_disable_nosync()` 只加计数不等待。
- `tasklet_kill()`：`wait_on_bit_lock(TASKLET_STATE_SCHED)` 等调度位释放，再 `tasklet_unlock_wait()` + `tasklet_clear_sched()`；在中断上下文调用会 `pr_notice` 警告。
- `tasklet_unlock_spin_wait()`：自旋等 RUN 位清除，注释明确"不要在新代码中使用"。

本树 `CONFIG_PREEMPT_RT=y`，tasklet 回调在 `ksoftirqd` 线程里跑、可被抢占，因此引入 per-CPU 的 `tasklet_sync_callback`（`cb_lock` 自旋锁 + `cb_waiters` 计数）：`tasklet_action_common()` 整个循环包在 `tasklet_lock_callback()/tasklet_unlock_callback()` 里；`tasklet_unlock_spin_wait()` 在 RT 下改调 `tasklet_callback_cancel_wait_running()`，通过短暂释放/重取 `cb_lock` 让正在跑的回调有机会推进，避免当前任务抢占 softirq 处理或阻止 ksoftirqd 运行造成活锁。非 RT 下这些函数是空操作（源码存在，当前配置未启用其 RT 分支之外的形态）。

## 9 初始化、热插拔与 softirq 关系

`softirq_init()`（`kernel/softirq.c`）初始化每 CPU `tasklet_vec`/`tasklet_hi_vec` 的尾指针，并注册 `open_softirq(TASKLET_SOFTIRQ, tasklet_action)`、`open_softirq(HI_SOFTIRQ, tasklet_hi_action)`。CPU 下线时 `takeover_tasklets()` 先 `workqueue_softirq_dead()`，再把死 CPU 的两条 tasklet 链表整体拼到当前 CPU 链表尾并 raise 对应 softirq，保证遗留 tasklet 不丢。

tasklet 与 softirq 的关系：tasklet 本身不是独立调度实体，而是 `TASKLET_SOFTIRQ`/`HI_SOFTIRQ` 这两个 softirq 向量上的"用户"。串行性由 `TASKLET_STATE_RUN` 位保证（同一 tasklet 全局串行），不同 tasklet 之间、不同 CPU 之间可并行。

## 10 配置与调试

- `CONFIG_SMP` 或 `CONFIG_PREEMPT_RT` 决定 `tasklet_trylock/unlock` 是否为真实原子操作；本树两者皆满足。
- `trace_tasklet_entry/exit`（`trace/events/irq.h`）：`CONFIG_FTRACE` 当前配置未启用，源码存在、运行时不可用。
- `tasklet_clear_sched()` 的 `WARN_ONCE` 可捕获 SCHED 状态异常；`tasklet_kill` 在中断上下文调用会打 notice。
- 迁移建议：新代码用 [BH 工作队列](03-bh-workqueue.md)（`system_bh_wq`），获得统一的 `cancel_work_sync`/`flush_work` 与 lockdep 支持。

## 11 关键文件清单

| 文件 | 作用 |
|--|--|
| `kernel/softirq.c` | tasklet 调度/执行/kill/RT 同步、`softirq_init`、`takeover_tasklets` |
| `include/linux/interrupt.h` | `tasklet_struct`、状态枚举、`tasklet_schedule`/`disable` 内联、DECLARE 宏 |
| `kernel/workqueue.c` | `workqueue_softirq_action`（先于 tasklet 执行） |

## 12 状态位组合与机制对比

`state` 的两个位独立变化，组合含义如下：

| SCHED | RUN | 含义 |
|--|--|--|
| 0 | 0 | 空闲，可被调度 |
| 1 | 0 | 已入队，等待执行 |
| 1 | 1 | 正在执行（执行前清 SCHED，故实际多为 0/1） |
| 0 | 1 | 回调执行中，可被重新 `tasklet_schedule` |

执行前 `tasklet_clear_sched()` 清 SCHED、`tasklet_trylock()` 置 RUN，因此回调运行期间 SCHED 已清，允许回调内部再次调度自己（下一轮再跑）。

| 维度 | tasklet | BH workqueue | 普通 workqueue |
|--|--|--|--|
| 上下文 | softirq | softirq | 进程 |
| 可睡眠 | 否 | 否 | 是 |
| 串行粒度 | 单个 tasklet | 每 CPU | 每 work + max_active |
| 取消同步 | `tasklet_kill` | `cancel_work_sync` | `cancel_work_sync` |
| 状态 | 已弃用 | 推荐 | 推荐 |

相关笔记：[软中断](02-softirq.md)、[BH 工作队列](03-bh-workqueue.md)、[工作队列](07-workqueue.md)、[异步执行概要](async-execution-summary.md)。