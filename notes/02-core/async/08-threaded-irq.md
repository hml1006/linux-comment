# Threaded IRQ — 中断线程化

## 1 实现原理

Threaded IRQ 将中断处理的下半部从 hardirq 上下文迁移到内核线程中执行，允许在中断处理中使用可睡眠的锁。核心设计如下：

- **双 handler 模型**：`request_threaded_irq(irq, handler, thread_fn, ...)` 注册两个处理函数：
  - `handler`：在 hardirq 上下文执行，快速完成，返回 `IRQ_WAKE_THREAD` 唤醒线程
  - `thread_fn`：在内核线程中执行，可睡眠
- **irqaction 绑定**：每个 `irqaction` 结构包含 `thread` 字段指向对应的内核线程。
- **强制线程化**：`force_irqthreads()` 内核参数可强制所有中断处理线程化（PREEMPT_RT 默认行为）。
- **优先级继承**：线程以 `SCHED_FIFO` 策略运行，确保实时性。

## 2 使用场景

- **减少关中断时间**：将耗时操作移到线程中，降低中断延迟。
- **RT 内核必须**：PREEMPT_RT 下几乎所有中断都线程化处理。
- **需要可睡眠锁的驱动**：如 I2C、SPI 等总线通信。
- **设备热插拔、电源管理**。

## 3 代码调用栈

```
注册:
request_threaded_irq(irq, handler, thread_fn, flags, name, dev)
  └→ __setup_irq()
      └→ 分配 irqaction
      └→ 如果 thread_fn 不为 NULL → 创建内核线程
          └→ kthread_create(irq_thread, action, "irq/%d-%s", irq, name)
      └→ 注册 handler

中断触发:
  └→ 硬件中断 → 通用中断处理
      └→ handler  (hardirq 上下文)
          └→ 返回 IRQ_WAKE_THREAD
              └→ __irq_wake_thread()
                  └→ wake_up_process(action->thread)

线程处理:
irq_thread()
  └→ irq_wait_for_interrupt()  (等待中断唤醒)
  └→ handler_fn(desc, action)  (调用 thread_fn)
      └→ action->thread_fn(irq, action->dev_id)
      └→ 返回 IRQ_HANDLED 或 IRQ_WAKE_THREAD
  └→ 循环等待下一次中断
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    Threaded IRQ 处理流程                          │
│                                                                   │
│  request_threaded_irq(irq, handler, thread_fn, ...)              │
│    └→ 注册 handler 为 hardirq 处理函数                            │
│    └→ 创建内核线程 "irq/NNN-name"                                 │
│        └→ 线程以 SCHED_FIFO 策略运行                              │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    中断触发流程                                    │
│                                                                   │
│  硬件中断到达                                                      │
│    │                                                               │
│    ▼                                                               │
│  do_IRQ() → generic_handle_irq()                                  │
│    │                                                               │
│    ▼                                                               │
│  handler(irq, dev_id)  ← 在 hardirq 上下文 (关中断)              │
│    │  - 快速完成 (ACK 设备、读取状态)                               │
│    │  - 返回 IRQ_WAKE_THREAD                                      │
│    │                                                               │
│    ▼                                                               │
│  __irq_wake_thread()                                              │
│    └→ wake_up_process(action->thread)                             │
│    └→ 开中断 (继续处理其他中断)                                    │
│                                                                   │
│    ▼                                                               │
│  irq_thread() 内核线程                                            │
│    └→ thread_fn(irq, dev_id)  ← 在进程上下文 (可睡眠)            │
│        - 完成剩余的 I/O 处理                                       │
│        - 使用可睡眠锁 (mutex, wait_event)                          │
│    └→ 等待下一次中断唤醒                                           │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct irqaction` — 中断动作描述符

```c
// include/linux/interrupt.h
struct irqaction {
    irq_handler_t handler;          // hardirq 处理函数 (快速)
    union {
        void *dev_id;               // 设备 ID
        void __percpu *percpu_dev_id;  // per-CPU 设备 ID
    };
    const struct cpumask *affinity; // CPU 亲和性
    struct irqaction *next;         // 同一个中断号的下一个 action (共享中断)
    irq_handler_t thread_fn;        // 线程化处理函数 (可睡眠)
    struct task_struct *thread;     // 对应的内核线程
    struct irqaction *secondary;    // 次 handler (强制线程化)
    unsigned int irq;               // 中断号
    unsigned int flags;             // 标志位 (IRQF_* 系列)
    unsigned long thread_flags;     // 线程标志
    unsigned long thread_mask;      // 线程掩码
    const char *name;               // 中断名称
    struct proc_dir_entry *dir;     // /proc/irq/ 条目
} ____cacheline_internodealigned_in_smp;
```

### 关键 API

```c
// 注册线程化中断 (handler 和 thread_fn 均可为 NULL)
// - handler==NULL AND thread_fn!=NULL: 使用默认 handler (irq_default_primary_handler)
// - handler!=NULL AND thread_fn!=NULL: handler 在 hardirq 执行，返回 IRQ_WAKE_THREAD
// - handler!=NULL AND thread_fn==NULL: 只在 hardirq 执行，不进线程
int request_threaded_irq(unsigned int irq, irq_handler_t handler,
                         irq_handler_t thread_fn, unsigned long flags,
                         const char *name, void *dev);

// 简化版 (自动管理生命周期)
int devm_request_threaded_irq(struct device *dev, unsigned int irq,
                              irq_handler_t handler, irq_handler_t thread_fn,
                              unsigned long irqflags, const char *devname,
                              void *dev_id);

// 强制线程化全局开关
extern bool force_irqthreads;
```

### 线程化中断的返回值

```c
// include/linux/irqreturn.h
enum irqreturn {
    IRQ_NONE        = (0 << 0),  // 未处理
    IRQ_HANDLED     = (1 << 0),  // 已处理
    IRQ_WAKE_THREAD = (1 << 1),  // 需要唤醒线程处理
};
```

## 6 注册路径与线程创建

`request_threaded_irq()`（`kernel/irq/manage.c`）先做合法性校验（共享中断必须传 `dev_id` 等），若 `handler==NULL` 则补上默认 primary handler，再分配 `irqaction` 并调 `__setup_irq()`：

```c
// kernel/irq/manage.c: request_threaded_irq()
if (!handler) {
    if (!thread_fn)
        return -EINVAL;
    handler = irq_default_primary_handler;   // 仅返回 IRQ_WAKE_THREAD
}
action->handler = handler;
action->thread_fn = thread_fn;
retval = __setup_irq(irq, desc, action);
```

`irq_default_primary_handler()` 本体就一行 `return IRQ_WAKE_THREAD;`，专为 oneshot 中断设计。`__setup_irq()` 里若需要线程，调 `setup_irq_thread()` 用 `kthread_create()` 创建：

```c
// kernel/irq/manage.c: setup_irq_thread()
t = kthread_create(irq_thread, new, "irq/%d-%s", irq, new->name);
// secondary 线程名为 "irq/%d-s-%s"
new->thread = get_task_struct(t);   // 持引用，线程死了也不释放 task
```

线程创建后并不立即跑，由 `__setup_irq()` 末尾 `wake_up_and_wait_for_irq_thread_ready()` 唤醒并等 `IRQTF_READY` 置位。

## 7 irq_thread 主循环

`irq_thread()`（`kernel/irq/manage.c`）是线程主体：

```c
// kernel/irq/manage.c: irq_thread() 骨架
irq_thread_set_ready(desc, action);
sched_set_fifo(current);                 // SCHED_FIFO 优先级 50
if (force_irqthreads() && test_bit(IRQTF_FORCED_THREAD, ...))
    handler_fn = irq_forced_thread_fn;   // 强制线程化
else
    handler_fn = irq_thread_fn;          // 显式线程化
init_task_work(&on_exit_work, irq_thread_dtor);
task_work_add(current, &on_exit_work, TWA_NONE);
while (!irq_wait_for_interrupt(desc, action)) {
    action_ret = handler_fn(desc, action);
    if (action_ret == IRQ_WAKE_THREAD)
        irq_wake_secondary(desc, action);
    wake_threads_waitq(desc);
}
task_work_cancel_func(current, irq_thread_dtor);
```

`irq_wait_for_interrupt()` 把任务设为 `TASK_INTERRUPTIBLE`，`test_and_clear_bit(IRQTF_RUNTHREAD)` 判断是否有中断要处理；`kthread_should_stop()` 时若仍有 RUNTHREAD 位会再跑最后一次。这里用 [task_work](11-task_work.md) 注册 `irq_thread_dtor` 作为异常退出兜底：若线程被杀，`PF_EXITING` 路径会递减 `threads_active` 并 `irq_finalize_oneshot()`，避免 oneshot 掩码卡死。

## 8 primary/thread 分工与唤醒

hardirq 侧 handler 返回 `IRQ_WAKE_THREAD` 后，通用中断处理调 `__irq_wake_thread()`（`kernel/irq/handle.c`）：

```c
// kernel/irq/handle.c: __irq_wake_thread()
if (action->thread->flags & PF_EXITING)
    return;
if (test_and_set_bit(IRQTF_RUNTHREAD, &action->thread_flags))
    return;                              // 已置位，无需重复唤醒
desc->threads_oneshot |= action->thread_mask;
atomic_inc(&desc->threads_active);
wake_up_state(action->thread, TASK_INTERRUPTIBLE);
```

`threads_active` 计数供 `synchronize_irq()` 等待所有线程处理完；`wake_threads_waitq()` 在计数归零时唤醒等待者。显式线程化走 `irq_thread_fn()`（直接调 `thread_fn`，可睡眠）；强制线程化走 `irq_forced_thread_fn()`，额外 `local_bh_disable()`（非 RT 还 `local_irq_disable()`）包裹，以模拟 hardirq 的关底半部语义、避免死锁。

## 9 IRQF_ONESHOT 与掩码管理

`IRQF_ONESHOT` 表示"线程处理期间保持中断线屏蔽"，适合电平触发、设备中断需在线程里才能清除的场景。`__setup_irq()` 为每个 oneshot action 分配一个 `thread_mask` 位（`1UL << ffz(thread_mask)`），或入 `desc->threads_oneshot`。线程跑完 `irq_thread_fn()` 调 `irq_finalize_oneshot()`：

```c
// kernel/irq/manage.c: irq_finalize_oneshot() 要点
if (unlikely(irqd_irq_inprogress(&desc->irq_data))) {
    ... cpu_relax(); goto again;     // 等 hardirq 侧完成，防永久屏蔽
}
if (test_bit(IRQTF_RUNTHREAD, &action->thread_flags))
    goto out_unlock;                 // 又要跑，不清掩码
desc->threads_oneshot &= ~action->thread_mask;
if (!desc->threads_oneshot && !irqd_irq_disabled(...) && irqd_irq_masked(...))
    unmask_threaded_irq(desc);       // 所有线程完成才 unmask
```

共享中断线上多个 oneshot action 各占一位，全部完成才 unmask。`request_threaded_irq()` 注释明确：`IRQF_ONESHOT` 必须配 `thread_fn`，否则 `WARN_ON_ONCE`。

## 10 强制线程化与本树 RT 形态

`force_irqthreads()` 定义在 `include/linux/interrupt.h`：

```c
#ifdef CONFIG_IRQ_FORCED_THREADING
# ifdef CONFIG_PREEMPT_RT
#  define force_irqthreads()  (true)        // 本树恒真
# else
DECLARE_STATIC_KEY_FALSE(force_irqthreads_key);
#  define force_irqthreads()  (static_branch_unlikely(&force_irqthreads_key))
# endif
#else
#define force_irqthreads()  (false)
#endif
```

本树 `CONFIG_PREEMPT_RT=y`、`CONFIG_IRQ_FORCED_THREADING=y`，故 `force_irqthreads()` 恒真（非 RT 下由 `threadirqs` 启动参数启用 static key，源码存在，当前配置未启用该分支）。`irq_setup_forced_threading()` 对未显式线程化的中断自动加 `IRQF_ONESHOT`，若同时有 primary 和 thread_fn 则创建 secondary action；带 `IRQF_NO_THREAD`/`IRQF_PERCPU`/`IRQF_ONESHOT` 的跳过。配合 [软中断](02-softirq.md) 的 `ktimers/%u`，RT 下定时器与多数中断都被线程化。

## 11 配置、调试与关键文件

- `/proc/interrupts` 看中断计数，`ps -e | grep irq/` 看 `irq/N-name` 线程；线程为 `SCHED_FIFO` 优先级 50（`sched_set_fifo`）。
- `CONFIG_DEBUG_SHIRQ_FIXME` 共享中断注册时假触发校验：当前配置未启用（源码存在）。
- `irq_thread_check_affinity()` 在 `IRQTF_AFFINITY` 置位时把线程迁到中断 effective affinity。

| 文件 | 作用 |
|--|--|
| `kernel/irq/manage.c` | `request_threaded_irq`/`__setup_irq`/`irq_thread`/oneshot/强制线程化 |
| `kernel/irq/handle.c` | `__irq_wake_thread`/通用中断分发 |
| `include/linux/interrupt.h` | `irqaction`、`force_irqthreads`、IRQF 标志 |
| `include/linux/irqreturn.h` | `IRQ_NONE/HANDLED/WAKE_THREAD` |

相关笔记：[软中断](02-softirq.md)、[内核线程](09-kthread.md)、[task_work](11-task_work.md)、[异步执行概要](async-execution-summary.md)。