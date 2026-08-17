# irq_work — 中断上下文工作队列

## 1 实现原理

irq_work 是一种在 NMI/硬中断上下文中执行回调的机制，提供最低延迟的异步执行能力。其核心设计如下：

- **每 CPU 双链表**：每个 CPU 维护两个 `llist_head` 链表（`raised_list` 和 `lazy_list`），分别存放普通和惰性 irq_work 条目。
- **NMI 安全入队**：入队操作使用 `atomic_fetch_or` 实现无锁 claim，确保 NMI 上下文也可安全使用。
- **标志位状态机**：每个 `irq_work` 条目通过 `IRQ_WORK_PENDING`、`IRQ_WORK_BUSY`、`IRQ_WORK_CLAIMED` 标志位管理状态：
  - `free` → `claimed` → `pending` → `busy` → `free`
- **执行触发**：分两种路径：
  - `raised_list`：通过 `arch_irq_work_raise()` 发送 IPI 立即触发执行
  - `lazy_list`：延迟到下一个 tick 或通过专用内核线程执行（PREEMPT_RT）

## 2 使用场景

- **跨 CPU 中断通知**：如 perf 事件采样、RCU 紧急回调唤醒
- **低延迟通知**：需要比 softirq 更优先执行的场景
- **NMI 上下文中的延迟工作**：在 NMI 中无法执行的操作通过 irq_work 推迟到硬中断上下文

## 3 代码调用栈

```
irq_work_queue(work)
  └→ irq_work_claim(work)              // atomic_fetch_or 设置 CLAIMED|PENDING
  └→ __irq_work_queue_local(work)      // 根据 flags 选择 raised_list 或 lazy_list
      └→ llist_add(&work->node.llist, list)
      └→ irq_work_raise(work)          // 调用 arch_irq_work_raise()

中断返回路径:
irq_exit()
  └→ irq_work_run()                    // 处理 raised_list
      └→ irq_work_run_list(list)
          └→ irq_work_single(work)
              └→ work->func(work)      // 执行回调

Tick 处理:
irq_work_tick()                         // 处理 lazy_list
  └→ irq_work_run_list(lazy_list)
```

## 4 流程图

```
┌──────────────────────────────────────────────────────────────┐
│                     irq_work_queue(work)                       │
│                                                                │
│  1. irq_work_claim(): atomic_fetch_or 设置 CLAIMED|PENDING     │
│     └─ 如果已 PENDING，返回 false (不重复入队)                  │
│                                                                │
│  2. 根据 flags 选择链表:                                       │
│     ┌──────────────────────┬──────────────────────┐            │
│     │  raised_list         │  lazy_list           │            │
│     │  (普通/IRQ_WORK_HARD)│  (IRQ_WORK_LAZY/RT)  │            │
│     └──────────┬───────────┴──────────┬───────────┘            │
│                │                       │                       │
│                ▼                       ▼                       │
│  arch_irq_work_raise()         tick_nohz_tick_stopped()?       │
│  (发送 IPI 立即执行)           ├─ 是: arch_irq_work_raise()    │
│                                └─ 否: 等 tick 触发            │
│                                                                │
└──────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌──────────────────────────────────────────────────────────────┐
│                      执行路径                                   │
│                                                                │
│  raised_list (高优先级):                                       │
│  irq_exit() → irq_work_run() → irq_work_single()              │
│    → 清除 PENDING → smp_mb() → work->func(work)               │
│                                                                │
│  lazy_list (低优先级):                                         │
│  irq_work_tick() → irq_work_run_list(lazy_list)               │
│  PREEMPT_RT: irq_workd 内核线程                                │
│    → irq_workd_should_run() → run_irq_workd()                 │
└──────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct irq_work` — irq_work 条目

```c
// include/linux/irq_work_types.h
struct irq_work {
    struct __call_single_node node;    // 内嵌的跨 CPU 调用节点，包含:
                                       //   - union { u_flags; a_flags; }
                                       //     u_flags 包含 IRQ_WORK_PENDING,
                                       //     IRQ_WORK_BUSY, IRQ_WORK_LAZY,
                                       //     IRQ_WORK_HARD_IRQ 等标志
                                       //   - struct llist_node llist (链表节点)
    void (*func)(struct irq_work *);   // 回调函数指针
    struct rcuwait irqwait;            // 用于同步等待 (irq_work_sync)
};
```

### 状态标志位

```c
// include/linux/irq_work_types.h (通过 irq_work_flags 定义)
#define IRQ_WORK_BUSY       BIT(0)    // 回调正在执行中
#define IRQ_WORK_PENDING    BIT(1)    // 已入队等待执行
#define IRQ_WORK_LAZY       BIT(2)    // 惰性标志，可延迟到 tick 处理
#define IRQ_WORK_HARD_IRQ   BIT(3)    // 在硬中断上下文执行 (PREEMPT_RT)

// 状态组合:
//   free:      flags & (PENDING|BUSY) == 0
//   claimed:   flags & CLAIMED != 0 (但未入队)
//   pending:   flags & PENDING != 0 (已入队)
//   busy:      flags & BUSY != 0 (回调正在执行)
```

### 每 CPU 链表

```c
// kernel/irq_work.c
static DEFINE_PER_CPU(struct llist_head, raised_list);  // 高优先级链表
static DEFINE_PER_CPU(struct llist_head, lazy_list);    // 低优先级链表
static DEFINE_PER_CPU(struct task_struct *, irq_workd); // PREEMPT_RT 专用线程
```

### 初始化宏

```c
#define IRQ_WORK_INIT(_func)           __IRQ_WORK_INIT(_func, 0)
#define IRQ_WORK_INIT_LAZY(_func)      __IRQ_WORK_INIT(_func, IRQ_WORK_LAZY)
#define IRQ_WORK_INIT_HARD(_func)      __IRQ_WORK_INIT(_func, IRQ_WORK_HARD_IRQ)
#define DEFINE_IRQ_WORK(name, _f)      struct irq_work name = IRQ_WORK_INIT(_f)
```