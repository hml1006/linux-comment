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

## 6 抢占计数与底半部开关

softirq 的"关/开"复用抢占计数 `preempt_count` 的位段（`include/linux/preempt.h`）：

```c
// include/linux/preempt.h（位段布局）
//   PREEMPT_MASK:  0x000000ff
//   SOFTIRQ_MASK:  0x0000ff00
//   HARDIRQ_MASK:  0x000f0000
#define SOFTIRQ_OFFSET         (1UL << SOFTIRQ_SHIFT)
#define SOFTIRQ_DISABLE_OFFSET (2 * SOFTIRQ_OFFSET)
```

`local_bh_disable()` 加 `SOFTIRQ_DISABLE_OFFSET`（2 个单位），`__do_softirq()` 执行时只加 `SOFTIRQ_OFFSET`（1 个单位）。这样 `softirq_count()` 能区分"正在执行 softirq"与" merely 关了底半部"。`in_softirq()`、`in_serving_softirq()` 都据此判断。

**本树 `CONFIG_PREEMPT_RT=y`，走的是 RT 分支**：BH 禁用计数不再放在 `preempt_count`，而是 per-CPU 的 `softirq_ctrl.cnt` 加任务级 `current->softirq_disable_cnt`，目的是允许处于 BH 禁用区的任务被抢占：

```c
// kernel/softirq.c（CONFIG_PREEMPT_RT 分支）
struct softirq_ctrl {
    local_lock_t lock;
    int          cnt;
};
static DEFINE_PER_CPU(struct softirq_ctrl, softirq_ctrl) = {
    .lock = INIT_LOCAL_LOCK(softirq_ctrl.lock),
};
```

`__local_bh_disable_ip()` 首次进入时，若可抢占则 `migrate_disable()`（本树 `CONFIG_PREEMPT_RT_NEEDS_BH_LOCK` 未启用，故不用 local_lock）并 `rcu_read_lock()`，以满足 RCU 底半部要求；计数同时记到 per-CPU 与任务两处。非 RT 分支（`__preempt_count_add(cnt)` 那套）源码存在，当前配置未启用。

## 7 执行限额与 handle_softirqs 细节

`handle_softirqs()` 的两条限额定义在 `kernel/softirq.c`：

```c
#define MAX_SOFTIRQ_TIME    msecs_to_jiffies(2)
#define MAX_SOFTIRQ_RESTART 10
```

注释说明：既要尽快处理 softirq，又不能锁死系统；`stop_machine()` 等场景 jiffies 可能停走，所以除 2ms 时间上限外还需 `MAX_SOFTIRQ_RESTART` 次数上限兜底。主循环用 `ffs(pending)` 逐位取最低置位，`h += softirq_bit - 1` 定位 `softirq_vec` 槽位，执行前后用 `trace_softirq_entry/exit` 包裹，并校验 `preempt_count` 是否被回调破坏：

```c
// kernel/softirq.c: handle_softirqs() 片段
prev_count = preempt_count();
kstat_incr_softirqs_this_cpu(vec_nr);   // 计入 /proc/softirqs
h->action();
if (unlikely(prev_count != preempt_count())) {
    pr_err("huh, entered softirq %u %s ... exited with %08x?\n", ...);
    preempt_count_set(prev_count);
}
```

循环结束关中断重读 pending，若仍有且 `time_before(jiffies, end) && !need_resched() && --max_restart` 则 `goto restart`，否则 `wakeup_softirqd()`。进入时还会 `current->flags &= ~PF_MEMALLOC`，退出时恢复，避免借用任务上下文污染内存分配语义。

## 8 触发路径与本树 RT 形态

`__irq_exit_rcu()` 先 `preempt_count_sub(HARDIRQ_OFFSET)`，再在 `!in_interrupt() && local_softirq_pending()` 时调 `invoke_softirq()`。RT 与非 RT 的 `invoke_softirq()` 截然不同：

```c
// kernel/softirq.c（RT 分支）
static inline void invoke_softirq(void)
{
    if (should_wake_ksoftirqd())   // softirq_ctrl.cnt == 0
        wakeup_softirqd();
}
```

即 RT 下硬中断返回**不直接执行 softirq**，只唤醒 `ksoftirqd`；非 RT 分支才会 `__do_softirq()`/`do_softirq_own_stack()`（源码存在，当前配置未启用）。另一条执行入口是 `__local_bh_enable_ip()`：开底半部时若有 pending 且当前可抢占，就地 `__do_softirq()`；不可抢占则唤醒 `ksoftirqd`。

`raise_softirq_irqoff()` 置位后，若 `!in_interrupt() && should_wake_ksoftirqd()` 也唤醒 `ksoftirqd`，保证进程上下文 raise 的 softirq 尽快被调度。

## 9 初始化与 ksoftirqd/ktimerd

`softirq_init()` 初始化每 CPU tasklet 链表尾指针，并 `open_softirq(TASKLET_SOFTIRQ, tasklet_action)`、`open_softirq(HI_SOFTIRQ, tasklet_hi_action)`。`open_softirq()` 本身只是 `softirq_vec[nr].action = action`。

`spawn_ksoftirqd()` 是 `early_initcall`：注册 `CPUHP_SOFTIRQ_DEAD` 热插拔回调 `takeover_tasklets()`（CPU 下线时把遗留 tasklet 迁到当前 CPU），并通过 `smpboot_register_percpu_thread(&softirq_threads)` 创建每 CPU `ksoftirqd/%u`。`run_ksoftirqd()` 调 `handle_softirqs(true)`，`ksirqd=true` 时非 RT 会额外 `rcu_softirq_qs()` 报告静止态。

本树 `CONFIG_IRQ_FORCED_THREADING=y` 且 RT 下 `force_irqthreads()` 恒真，故还注册 `ktimers/%u` 线程（`timer_thread`）：`__irq_exit_rcu()` 末尾若 `local_timers_pending_force_th()` 有定时器 softirq 待处理，则 `wake_timersd()`，由 `run_ktimerd()` 把 `pending_timer_softirq` 合并进 pending 后 `__do_softirq()`，把定时器处理也线程化。

## 10 配置与调试

- `/proc/softirqs`：各 CPU 各向量累计次数，来自 `kstat_incr_softirqs_this_cpu()`。
- `trace_softirq_raise/entry/exit`（`trace/events/irq.h`，本文件 `CREATE_TRACE_POINTS`）：`CONFIG_FTRACE` 当前配置未启用，源码存在、运行时不可用。
- `preempt_count` 不一致会打 `pr_err`；`CONFIG_DEBUG_PREEMPT` 相关校验当前未启用。
- 本树 `CONFIG_HZ=250`、`CONFIG_NO_HZ_IDLE=y`；`MAX_SOFTIRQ_TIME` 的 2ms 在 HZ=250 下约半个 jiffy。
- RT 下 `local_bh_blocked()` 供 idle 任务判断是否有任务持 BH 锁阻塞，避免误报 softirq pending 警告。

## 11 关键文件清单

| 文件 | 作用 |
|--|--|
| `kernel/softirq.c` | softirq 核心、tasklet、ksoftirqd/ktimerd |
| `include/linux/interrupt.h` | 向量枚举、`softirq_action`、raise/open API |
| `include/linux/preempt.h` | `preempt_count` 位段与 SOFTIRQ_OFFSET |
| `arch/arm64/include/asm/softirq_stack.h` | `do_softirq_own_stack`（非 RT） |
| `kernel/irq_work.c` | 比 softirq 更早的 irq_work 层 |

## 12 与其他异步机制的对比

| 机制 | 执行上下文 | 触发时机 | 可否睡眠 | 串行性 |
|--|--|--|--|--|
| irq_work | 硬中断/NMI 安全入队 | 自 IPI 或 tick | 否 | 每 CPU 链表 |
| softirq | 软中断（RT 下 ksoftirqd） | irq_exit / local_bh_enable | 否 | 同向量可多 CPU 并行 |
| tasklet | 软中断（TASKLET/HI） | raise_softirq | 否 | 同一 tasklet 严格串行 |
| BH workqueue | 软中断 | raise_softirq | 否 | 每 CPU 串行 |
| 普通 workqueue | 进程（kworker） | kick_pool 唤醒 | 是 | 受 max_active 约束 |

softirq 是上述底半部机制的公共底座：tasklet 与 BH workqueue 都复用 `TASKLET_SOFTIRQ`/`HI_SOFTIRQ` 向量，RCU 在非 RT 下复用 `RCU_SOFTIRQ`。

相关笔记：[irq_work](01-irq_work.md)、[BH 工作队列](03-bh-workqueue.md)、[tasklet](04-tasklet.md)、[异步执行概要](async-execution-summary.md)。