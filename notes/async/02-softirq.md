# Softirq — 软中断

## 1 实现原理

Softirq 是 Linux 内核中最高优先级的底半部（bottom-half）机制，在硬中断返回路径中执行。核心设计如下：

- **编译时静态分配**：`open_softirq()` 在初始化时注册，最多 10 种（`NR_SOFTIRQS`）。
- **每 CPU 挂起位图**：`local_softirq_pending_ref` 记录待处理的 softirq，支持按位操作。
- **并行执行**：同一类型的 softirq 可在多个 CPU 上并行执行，无需全局锁。
- **执行限制**：每次最多处理 2ms 或 10 次循环（`MAX_SOFTIRQ_TIME` 和 `MAX_SOFTIRQ_RESTART`），防止用户态进程饿死。
- **触发路径**：`irq_exit() → invoke_softirq() → __do_softirq()` 或 `ksoftirqd` 内核线程。

## 2 使用场景

| Softirq 类型 | 用途 |
|--|--|
| `HI_SOFTIRQ` | 高优先级 tasklet |
| `TIMER_SOFTIRQ` | 定时器超时处理 |
| `NET_TX_SOFTIRQ` | 网络发包 |
| `NET_RX_SOFTIRQ` | 网络收包 |
| `BLOCK_SOFTIRQ` | 块设备 I/O 完成 |
| `IRQ_POLL_SOFTIRQ` | 中断轮询 |
| `TASKLET_SOFTIRQ` | tasklet 执行 |
| `SCHED_SOFTIRQ` | 调度器负载均衡 |
| `HRTIMER_SOFTIRQ` | 高精度定时器 |
| `RCU_SOFTIRQ` | RCU 回调处理 |

## 3 代码调用栈

```
硬中断处理完成:
irq_exit()
  └→ __irq_exit_rcu()
      └→ invoke_softirq()
          ├─ force_irqthreads()? → wakeup_softirqd()  // 强制线程化
          └─ __do_softirq() → handle_softirqs(false)  // 直接执行

__do_softirq() / handle_softirqs()
  └→ softirq_handle_begin()          // __local_bh_disable_ip
  └→ pending = local_softirq_pending()
  └→ set_softirq_pending(0)          // 清除挂起位图
  └→ local_irq_enable()
  └→ 循环处理每个 pending softirq:
      ├→ h->action()                 // 执行回调
      └→ 检查 preempt_count
  └→ local_irq_disable()
  └→ 检查是否还有新 pending:
      ├─ 有且未超限 → goto restart
      └─ 超限 → wakeup_softirqd()    // 唤醒 ksoftirqd

raise_softirq(nr)
  └→ __raise_softirq_irqoff(nr)      // 设置 pending 位
  └→ 如果不在中断上下文 → wakeup_softirqd()
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    硬中断处理完成                                   │
│                                                                   │
│  irq_exit()                                                       │
│    └→ __irq_exit_rcu()                                           │
│        └→ invoke_softirq()                                       │
│            │                                                      │
│            ├─ force_irqthreads()? ──→ wakeup_softirqd()          │
│            │                           (ksoftirqd 内核线程处理)    │
│            │                                                      │
│            └─ __do_softirq() (直接在当前栈执行)                    │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    handle_softirqs() 处理流程                      │
│                                                                   │
│  1. softirq_handle_begin()  (关底半部)                            │
│  2. 读取 pending 位图，清除所有 pending 位                        │
│  3. 开中断 (local_irq_enable)                                    │
│  4. 循环处理每个 pending:                                         │
│     ┌─────────────────────────────────────────────────────┐       │
│     │ for_each_set_bit(softirq_bit, pending):             │       │
│     │   h = &softirq_vec[vec_nr]                          │       │
│     │   h->action()  ← 执行回调函数                       │       │
│     │   pending >>= softirq_bit                           │       │
│     └─────────────────────────────────────────────────────┘       │
│  5. 关中断 (local_irq_disable)                                    │
│  6. 检查新 pending:                                               │
│     ├─ 有且未超时/未超限 → goto restart                           │
│     └─ 超限 → wakeup_softirqd()  → 下次由 ksoftirqd 处理          │
│  7. softirq_handle_end()  (开底半部)                              │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct softirq_action` — Softirq 动作描述符

```c
// include/linux/interrupt.h
struct softirq_action {
    void (*action)(void);     // 回调函数指针
};

// 全局数组，每个 softirq 一个槽位
static struct softirq_action softirq_vec[NR_SOFTIRQS] __cacheline_aligned_in_smp;
```

### Softirq 编号枚举

```c
// include/linux/interrupt.h
enum {
    HI_SOFTIRQ = 0,          // 高优先级 tasklet
    TIMER_SOFTIRQ,            // 定时器
    NET_TX_SOFTIRQ,           // 网络发送
    NET_RX_SOFTIRQ,           // 网络接收
    BLOCK_SOFTIRQ,            // 块设备
    IRQ_POLL_SOFTIRQ,         // 中断轮询
    TASKLET_SOFTIRQ,          // tasklet
    SCHED_SOFTIRQ,            // 调度器
    HRTIMER_SOFTIRQ,          // 高精度定时器
    RCU_SOFTIRQ,              // RCU (必须为最后一个)
    NR_SOFTIRQS               // 总数
};
```

### 每 CPU 挂起状态

```c
// include/linux/interrupt.h (通过 asm/hardirq.h 中的 irq_cpustat_t)
#define local_softirq_pending()     __this_cpu_read(local_softirq_pending_ref)
#define set_softirq_pending(x)      __this_cpu_write(local_softirq_pending_ref, (x))
#define or_softirq_pending(x)       __this_cpu_or(local_softirq_pending_ref, (x))
```

### 每 CPU ksoftirqd 内核线程

```c
// kernel/softirq.c
DEFINE_PER_CPU(struct task_struct *, ksoftirqd);  // 每 CPU 的 softirqd 线程

// 软中断优先级排序 (同名数组)
const char * const softirq_to_name[NR_SOFTIRQS] = {
    "HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL",
    "TASKLET", "SCHED", "HRTIMER", "RCU"
};
```