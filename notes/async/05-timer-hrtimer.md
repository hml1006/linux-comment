# Timer / hrtimer — 定时器

## 1 实现原理

Linux 内核提供两套定时器机制：基于 jiffies 的 `timer_list`（timer wheel）和高精度 `hrtimer`。

### timer_list（timer wheel）

- **分桶定时器轮**：基于 jiffies 的定时器轮算法，将超时时间按精度分组到不同桶中。
- **每 CPU 处理**：每个 CPU 有自己的 `timer_base` 结构，包含 `vectors[]` 数组和 `pending_map` 位图。
- **TIMER_SOFTIRQ**：定时器超时在 `TIMER_SOFTIRQ` softirq 中处理，由 `run_timer_softirq()` 驱动。

### hrtimer（高精度定时器）

- **红黑树组织**：使用 `timerqueue`（基于红黑树）按超时时间排序，支持纳秒级精度。
- **双模式执行**：`HRTIMER_MODE_HARD` 在硬中断上下文执行，`HRTIMER_MODE_SOFT` 在 `HRTIMER_SOFTIRQ` 中执行。
- **clock_base 分桶**：按时钟类型（CLOCK_MONOTONIC、CLOCK_REALTIME 等）分桶，每 CPU 独立。

## 2 使用场景

| 定时器类型 | 精度 | 上下文 | 典型用途 |
|--|--|--|--|
| timer_list | jiffies (1-10ms) | softirq | 超时处理、周期性任务、延迟工作 |
| hrtimer (HARD) | 纳秒 | 硬中断 | 高精度定时、调度器时间片 |
| hrtimer (SOFT) | 纳秒 | softirq | 用户态定时器、clock_nanosleep |

## 3 代码调用栈

### timer_list

```
timer_setup(&timer, callback, 0);
add_timer(&timer); 或 mod_timer(&timer, jiffies + delay);

时钟中断:
tick_periodic() 或 tick_sched_handle()
  └→ update_process_times()
      └→ run_local_timers()
          └→ raise_softirq(TIMER_SOFTIRQ)

softirq 处理:
__do_softirq() → TIMER_SOFTIRQ
  └→ run_timer_softirq()
      └→ __run_timers()
          └→ 遍历 timer_base->vectors[]
              └→ detach_expired_timers()
              └→ 遍历每个到期 timer:
                  └→ callback(&timer)
```

### hrtimer

```
hrtimer_setup(&timer, callback, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
hrtimer_start(&timer, ktime_set(0, 500000), HRTIMER_MODE_REL);

时钟中断 (非必须):
  └→ hrtimer_interrupt()
      └→ __hrtimer_run_queues()
          └→ 遍历红黑树取到期定时器
              └→ hrtimer_cb_get_time()
              └→ hrtimer_forward()  (如果 periodic)
              └→ timer->function(timer)  (回调)

或者:
  └→ hrtimer_run_queues()  (由 HRTIMER_SOFTIRQ 触发)
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    timer_list (Timer Wheel)                       │
│                                                                   │
│  添加定时器:                                                      │
│  mod_timer(&timer, expires)                                      │
│    └→ 计算桶索引 (基于 expires)                                   │
│    └→ 将 timer 加入对应桶的 hlist                                 │
│    └→ 更新 timer_base->next_expiry                                │
│                                                                   │
│  触发:                                                            │
│  tick → run_local_timers() → raise TIMER_SOFTIRQ                 │
│    └→ run_timer_softirq()                                        │
│        └→ __run_timers()                                         │
│            └→ 遍历到期桶:                                         │
│                └→ 取出所有到期 timer -> callback(&timer)          │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                    hrtimer (红黑树)                                │
│                                                                   │
│  添加定时器:                                                      │
│  hrtimer_start(&timer, tim, mode)                                │
│    └→ 根据 clock_id 选择 clock_base                               │
│    └→ 将 timer 插入 timerqueue (红黑树)                           │
│    └→ 更新 clock_base->next_timer                                 │
│    └→ 如果需要 → hrtimer_reprogram() (操作硬件)                   │
│                                                                   │
│  触发 (HARD 模式):                                                │
│  hrtimer_interrupt() (硬件时钟中断)                               │
│    └→ __hrtimer_run_queues()                                     │
│        └→ timerqueue_getnext() 取最早到期定时器                   │
│        └→ 循环处理所有到期定时器:                                 │
│            └→ timer->function(timer)                              │
│            └→ 如果返回 HRTIMER_RESTART → 重新插入                 │
│                                                                   │
│  触发 (SOFT 模式):                                                │
│  hrtimer_run_queues() (HRTIMER_SOFTIRQ)                          │
│    └→ 同上，但在 softirq 上下文执行                               │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct timer_list` — 低精度定时器

```c
// include/linux/timer_types.h
struct timer_list {
    struct hlist_node entry;        // 哈希链表节点 (挂入 timer wheel 桶)
    unsigned long expires;          // 超时 jiffies 值
    void (*function)(struct timer_list *);  // 超时回调函数
    u32 flags;                      // 标志位 (TIMER_* 系列)
#ifdef CONFIG_LOCKDEP
    struct lockdep_map lockdep_map;
#endif
};
```

### `struct timer_base` — 每 CPU 定时器轮

```c
// kernel/time/timer.c
struct timer_base {
    raw_spinlock_t lock;            // 保护定时器链表
    struct timer_list *running_timer;  // 当前正在执行的定时器
    unsigned long clk;              // 当前 jiffies 值
    unsigned long next_expiry;      // 下次到期时间
    unsigned int cpu;               // 所属 CPU
    bool next_expiry_recalc;        // 需要重新计算 next_expiry
    bool is_idle;                   // CPU 是否空闲
    bool timers_pending;            // 是否有挂起的定时器
    DECLARE_BITMAP(pending_map, WHEEL_SIZE);  // 桶挂起位图
    struct hlist_head vectors[WHEEL_SIZE];     // 定时器轮桶数组
} ____cacheline_aligned;
```

### `struct hrtimer` — 高精度定时器

```c
// include/linux/hrtimer_types.h
struct hrtimer {
    struct timerqueue_node node;    // 红黑树节点 (包含 expires 和 rb_node)
    enum hrtimer_restart (*function)(struct hrtimer *);  // 回调函数
    struct hrtimer_clock_base *base;  // 指向所属 clock_base
    u8 state;                        // 状态: HRTIMER_STATE_* 系列
    u8 is_soft;                       // 是否在 softirq 上下文执行
    u8 is_hard;                       // 是否在硬中断上下文执行 (PREEMPT_RT)
};
```

### `struct hrtimer_clock_base` — 每 CPU 每时钟的定时器基

```c
// include/linux/hrtimer_types.h
struct hrtimer_clock_base {
    struct hrtimer_cpu_base *cpu_base;  // 所属 CPU 基
    unsigned int index;                 // 时钟类型索引
    clockid_t clockid;                  // 时钟 ID
    seqcount_raw_spinlock_t seq;        // 顺序锁保护
    struct timerqueue_head active;      // 活跃定时器队列 (红黑树)
    ktime_t (*get_time)(void);          // 获取当前时间函数
    ktime_t offset;                     // 时间偏移
};
```

### `struct hrtimer_cpu_base` — 每 CPU 高精度定时器基础

```c
// include/linux/hrtimer_types.h
struct hrtimer_cpu_base {
    raw_spinlock_t lock;            // 保护锁
    unsigned int cpu;               // CPU 编号
    unsigned int active_bases;      // 活跃的 clock_base 位图
    unsigned int softirq_active;    // softirq 中活跃的 clock_base 位图
    ktime_t expires_next;           // 下次到期时间
    struct hrtimer_clock_base clock_base[HRTIMER_MAX_CLOCK_BASES];  // 各时钟基
};
```