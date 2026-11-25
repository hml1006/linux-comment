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

## 6 标志位实际取值与状态机核实

第 5 节给出的标志位是示意性写法。本树中这些标志实际定义在 `include/linux/smp_types.h`（而非 `irq_work_types.h`），且取值与示意写法不同，需以源码为准：

```c
// include/linux/smp_types.h
enum {
    IRQ_WORK_PENDING    = 0x01,
    IRQ_WORK_BUSY       = 0x02,
    IRQ_WORK_LAZY       = 0x04, /* No IPI, wait for tick */
    IRQ_WORK_HARD_IRQ   = 0x08, /* IRQ context on PREEMPT_RT */
    IRQ_WORK_CLAIMED    = (IRQ_WORK_PENDING | IRQ_WORK_BUSY),
};
// CSD_TYPE_IRQ_WORK = 0x20，用于跨 CPU call_single 队列区分节点类型
```

即 `PENDING` 是 bit0、`BUSY` 是 bit1，`CLAIMED` 是二者的组合（0x03）。`irq_work_claim()`（`kernel/irq_work.c`）一次性置上 `CLAIMED | CSD_TYPE_IRQ_WORK`：

```c
// kernel/irq_work.c: irq_work_claim()
oflags = atomic_fetch_or(IRQ_WORK_CLAIMED | CSD_TYPE_IRQ_WORK, &work->node.a_flags);
if (oflags & IRQ_WORK_PENDING)
    return false;   // 已 pending，无需重复入队/发 IPI
return true;
```

`struct irq_work` 本身非常精简（`include/linux/irq_work_types.h`），状态全部塞进内嵌 `__call_single_node` 的 `a_flags` 原子字段：

```c
// include/linux/irq_work_types.h
struct irq_work {
    struct __call_single_node node;   // 含 a_flags 与 llist 节点
    void (*func)(struct irq_work *);
    struct rcuwait irqwait;           // irq_work_sync() 睡眠等待用
};
```

执行侧 `irq_work_single()` 先清 `PENDING`（此刻 work 可被重新入队），`smp_mb()` 与 claim 配对，再调回调，最后用 `cmpxchg` 清 `BUSY`——若期间有人重新 claim 则保留其状态：

```c
// kernel/irq_work.c: irq_work_single()
flags = atomic_read(&work->node.a_flags);
flags &= ~IRQ_WORK_PENDING;
atomic_set(&work->node.a_flags, flags);
smp_mb();
work->func(work);
(void)atomic_cmpxchg(&work->node.a_flags, flags, flags & ~IRQ_WORK_BUSY);
```

## 7 入队路径与自 IPI 触发

`irq_work_queue()` 关抢占后走 `__irq_work_queue_local()`，按标志选择链表：带 `IRQ_WORK_LAZY`，或在 `CONFIG_PREEMPT_RT` 下未标 `IRQ_WORK_HARD_IRQ` 的，都进 `lazy_list`，否则进 `raised_list`。`llist_add()` 返回 false 表示链表原本非空（已有待处理项会触发执行），直接返回不再 raise：

```c
// kernel/irq_work.c: __irq_work_queue_local()
if (!llist_add(&work->node.llist, list))
    return;
if (!lazy_work || tick_nohz_tick_stopped())
    irq_work_raise(work);   // lazy 且 tick 未停 → 等下一个 tick
```

`irq_work_raise()` 最终调 `arch_irq_work_raise()`。arm64 实现是给自己发一个 `IPI_IRQ_WORK` 核间中断（`arch/arm64/kernel/smp.c`）：

```c
// arch/arm64/kernel/smp.c
void arch_irq_work_raise(void)
{
    smp_cross_call(cpumask_of(smp_processor_id()), IPI_IRQ_WORK);
}
```

该 IPI 在 `do_handle_IPI()` 的 `case IPI_IRQ_WORK:` 分支里调用 `irq_work_run()`。这就是"自 IPI 触发"的完整闭环：入队 → 给自己发 IPI → 中断返回路径执行回调。弱符号默认实现为空（`kernel/irq_work.c` 的 `__weak arch_irq_work_raise`），无专用 IPI 的架构退化为靠 tick 兜底。

跨 CPU 入队 `irq_work_queue_on()` 走 `__smp_call_single_queue()`；在 RT 下若目标 work 非 HARD，会先挂到目标 CPU 的 `lazy_list`，再用一个 per-CPU 的 `irq_work_wakeup`（HARD）去唤醒目标 CPU 的 `irq_workd` 线程。

## 8 执行路径与 PREEMPT_RT 形态

本树 `.config` 中 `CONFIG_PREEMPT_RT=y`，因此 lazy 工作不在硬中断里跑，而是交给 per-CPU 线程 `irq_work/%u`：

```c
// kernel/irq_work.c
void irq_work_run(void)
{
    irq_work_run_list(this_cpu_ptr(&raised_list));
    if (!IS_ENABLED(CONFIG_PREEMPT_RT))
        irq_work_run_list(this_cpu_ptr(&lazy_list));
    else
        wake_irq_workd();   // RT：唤醒 irq_workd 处理 lazy
}
```

`irq_workd` 通过 `smp_hotplug_thread` 注册（`irqwork_threads`，`thread_comm = "irq_work/%u"`），`irq_workd_setup()` 调 `sched_set_fifo_low()` 设为低优先级 FIFO；`irq_work_init_threads()` 是 `early_initcall`，仅在 RT 下真正注册。`run_irq_workd()` 在 `guard(rcu)` 保护下跑 `lazy_list`。`irq_work_tick()` 则只在架构无专用 IPI 时补跑 `raised_list`，RT 下同样唤醒 `irq_workd`。

`irq_work_sync()` 用于确保 work 不再被使用：RT 非 HARD 或无架构 IPI 时走 `rcuwait_wait_event()` 睡眠等待加 `synchronize_rcu()`；否则 `while (irq_work_is_busy(work)) cpu_relax()` 自旋。

## 9 与 printk 的关系

printk 是 irq_work 的典型用户。`kernel/printk/printk.c` 定义了 per-CPU 的 `wake_up_klogd_work`，用 `IRQ_WORK_INIT_LAZY` 初始化（惰性、等 tick）：

```c
// kernel/printk/printk.c
static DEFINE_PER_CPU(struct irq_work, wake_up_klogd_work) =
    IRQ_WORK_INIT_LAZY(wake_up_klogd_work_func);
```

`defer_console_output()` → `__wake_up_klogd(PRINTK_PENDING_WAKEUP | PRINTK_PENDING_OUTPUT)`，在 `preempt_disable()` 下置 `printk_pending` 位并 `irq_work_queue()`。回调 `wake_up_klogd_work_func()` 取出 pending 位：需要输出时 `console_trylock()/console_unlock()`（或唤醒 legacy printk 线程），需要唤醒时 `wake_up_interruptible(&log_wait)`。这样就把"不能在当前上下文（如 NMI/调度器内部）直接刷 console"的工作推迟到安全的 irq_work 上下文。`kernel/task_work.c` 的 `TWA_NMI_CURRENT` 也借助一个 no-op 的 `irq_work_NMI_resume` 来触发返回用户态路径，见 [task_work](11-task_work.md)。

## 10 配置与调试

- `CONFIG_IRQ_WORK=y`（本树已启用）；`CONFIG_PREEMPT_RT=y` 决定 lazy 走 `irq_workd` 线程。
- `irq_work_raise()` 内有 `trace_call__ipi_send_cpu()`  tracepoint（`trace/events/ipi.h`），但 `CONFIG_FTRACE` 当前配置未启用，源码存在、运行时不可用。
- arm64 的 IPI 名称表含 `"IRQ work interrupts"`（`arch/arm64/kernel/smp.c`），可在 `/proc/interrupts` 观察自 IPI 计数。
- `lockdep_irq_work_enter/exit()` 包裹回调，供锁依赖校验标注上下文。

## 11 关键文件清单

| 文件 | 作用 |
|--|--|
| `kernel/irq_work.c` | 入队/执行/sync/irq_workd 主体 |
| `include/linux/irq_work_types.h` | `struct irq_work` 定义 |
| `include/linux/irq_work.h` | `IRQ_WORK_INIT*` 宏与 pending/busy 判断 |
| `include/linux/smp_types.h` | 标志位枚举、`__call_single_node` |
| `arch/arm64/kernel/smp.c` | `arch_irq_work_raise()` 与 IPI 处理 |
| `kernel/printk/printk.c` | `wake_up_klogd_work`/`defer_console_output` |

相关笔记：[软中断](02-softirq.md)、[task_work](11-task_work.md)、[异步执行概要](async-execution-summary.md)。