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