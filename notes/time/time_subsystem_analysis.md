# Linux 7.0 时间子系统（Time Subsystem）代码分析报告

## 目录

1. [总体概览](#1-总体概览)
2. [核心数据结构](#2-核心数据结构)
3. [Clocksource — 时钟源抽象层](#3-clocksource--时钟源抽象层)
4. [ARM64 架构定时器硬件与驱动分析](#35-arm64-架构定时器arch-timer硬件与驱动分析)
5. [Clockevent — 时钟事件设备](#4-clockevent--时钟事件设备)
6. [Timekeeping — 核心时间管理](#5-timekeeping--核心时间管理)
7. [Timer Wheel — 低精度定时器](#6-timer-wheel--低精度定时器)
8. [HRTimer — 高精度定时器](#7-hrtimer--高精度定时器)
9. [Tick 子系统 — 周期性调度滴答](#8-tick-子系统--周期性调度滴答)
10. [NTP 协议层](#9-ntp-协议层)
11. [Jiffies — 基准时钟源](#10-jiffies--基准时钟源)
12. [sched_clock — 调度时钟](#11-sched_clock--调度时钟)
13. [Alarmtimer — 闹钟定时器](#12-alarmtimer--闹钟定时器)
14. [POSIX 定时器](#13-posix-定时器)
15. [VDSO — 快速用户态时间读取](#14-vdso--快速用户态时间读取)
16. [时间命名空间（Time Namespace）](#15-时间命名空间time-namespace)
17. [完整调用链总结](#16-完整调用链总结)
18. [涉及的文件清单](#17-涉及的文件清单)

---

## 1. 总体概览

### 1.1 文件统计

时间子系统代码位于 `kernel/time/` 目录下，包含 **41 个 .c 源文件** 和 **5 个 .h 头文件**，以及 `include/linux/` 下的多个核心头文件。

### 1.2 代码规模排名（Top 15）

| 排名 | 文件 | 行数 | 功能 |
|------|------|------|------|
| 1 | `timekeeping.c` | ~3,080 | 核心时间管理，NMI 安全时钟读取 |
| 2 | `hrtimer.c` | ~2,390 | 高精度定时器实现 |
| 3 | `timer.c` | ~2,300 | 低精度定时器（timer wheel） |
| 4 | `tick-sched.c` | ~1,600 | NOHZ 调度 tick 管理 |
| 5 | `posix-cpu-timers.c` | ~1,500 | POSIX CPU 定时器 |
| 6 | `clocksource.c` | ~1,100 | 时钟源注册/选择/看门狗 |
| 7 | `posix-timers.c` | ~1,070 | POSIX 定时器框架 |
| 8 | `ntp.c` | ~1,000 | NTP 状态机与频率调整 |
| 9 | `alarmtimer.c` | ~900 | RTC 闹钟定时器 |
| 10 | `timer_migration.c` | ~800 | 定时器迁移管理 |
| 11 | `time.c` | ~700 | 系统调用接口（time/gettimeofday/adjtimex） |
| 12 | `clockevents.c` | ~600 | 时钟事件设备管理 |
| 13 | `sched_clock.c` | ~500 | 调度时钟（sched_clock） |
| 14 | `tick-broadcast.c` | ~450 | Tick 广播模式 |
| 15 | `tick-common.c` | ~300 | Tick 公共管理 |

### 1.3 子系统架构总览

```
┌─────────────────────────────────────────────────────────────────────┐
│                       用户空间 (User Space)                          │
│  gettimeofday() / clock_gettime() / timer_create() / nanosleep()    │
└───────────────────────────────┬─────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────┐
│  VDSO (Virtual Dynamic Shared Object)  ── 快速用户态时间读取         │
│  vsyscall.c                                                         │
└──────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────┐
│  系统调用层 (sys_time.c / sys_clock_gettime / sys_nanosleep)        │
│  kernel/time/time.c, posix-timers.c, posix-cpu-timers.c             │
└──────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────┐
│  时间管理核心 (Timekeeping Core)                                     │
│  ┌─────────────────────┐  ┌──────────────────────┐                  │
│  │  struct timekeeper   │  │  struct tk_fast      │  NMI 安全读取   │
│  │  tkr_mono / tkr_raw  │  │  seqcount_latch      │                  │
│  │  offs_* 偏移量       │  │  base[0]/base[1]     │                  │
│  └──────────┬──────────┘  └──────────┬───────────┘                  │
│             │                        │                              │
│             ▼                        ▼                              │
│  ┌──────────────────────────────────────────────────────┐           │
│  │  struct clocksource  ── 硬件时钟抽象层                │           │
│  │  read() / mask / mult / shift / rating               │           │
│  └──────────────────────────────────────────────────────┘           │
└──────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────┐
│  定时器系统 (Timer Subsystem)                                        │
│  ┌─────────────────────┐  ┌──────────────────────┐                  │
│  │ Timer Wheel          │  │ HRTimer              │                  │
│  │ (低精度, O(1))       │  │ (高精度, 红黑树)     │                  │
│  │ 9级 + 64桶/级        │  │ 8 clock bases / CPU  │                  │
│  │ 粒度: 1ms~12d        │  │ 分辨率: 1ns          │                  │
│  └─────────────────────┘  └──────────────────────┘                  │
└──────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Tick 子系统 (Tick Management)                                      │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────────┐            │
│  │  Periodic    │ │  Oneshot     │ │  Broadcast       │            │
│  │  tick-common │ │  tick-oneshot│ │  tick-broadcast  │            │
│  └──────────────┘ └──────────────┘ └──────────────────┘            │
│                                                                      │
│  ┌──────────────┐ ┌──────────────────┐                              │
│  │  NOHZ / dyntick│ │  Timer Migration  │                            │
│  │  tick-sched   │ │  timer_migration  │                            │
│  └──────────────┘ └──────────────────┘                              │
└──────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────┐
│  硬件抽象层 (Hardware Abstraction)                                   │
│  ┌─────────────────────┐  ┌──────────────────────┐                  │
│  │  clocksource         │  │  clock_event_device   │                  │
│  │  (自由运行计数器)    │  │  (可编程事件设备)     │                  │
│  │  e.g., TSC/arch_timer│  │  e.g., PIT/APIC timer │                  │
│  └─────────────────────┘  └──────────────────────┘                  │
└──────────────────────────────────────────────────────────────────────┘
```

**五大核心子系统**：

| 子系统 | 核心文件 | 功能 |
|--------|----------|------|
| **Clocksource** | `clocksource.c` | 硬件时钟计数器抽象，负责注册、选择、看门狗校验 |
| **Clockevent** | `clockevents.c` | 可编程时钟事件设备，支持 periodic/oneshot 模式 |
| **Timekeeping** | `timekeeping.c` | 核心时间管理，维护系统时间、NTP 调整、挂起/恢复 |
| **Timer Wheel** | `timer.c` | 低精度定时器（timer wheel），基于 jiffies |
| **HRTimer** | `hrtimer.c` | 高精度定时器，基于红黑树，支持 8 种 clock base |
| **Tick** | `tick-*.c` | 周期性调度滴答管理，NOHZ 空闲省电 |

---

## 2. 核心数据结构

### 2.1 关键头文件

| 头文件 | 位置 | 功能 |
|--------|------|------|
| `clocksource.h` | `include/linux/` | `struct clocksource` 定义 |
| `clockchips.h` | `include/linux/` | `struct clock_event_device` 定义 |
| `timekeeper_internal.h` | `include/linux/` | `struct timekeeper`、`struct tk_read_base` 定义 |
| `hrtimer_defs.h` | `include/linux/` | `struct hrtimer_cpu_base`、`struct hrtimer_clock_base` 定义 |
| `hrtimer_types.h` | `include/linux/` | `struct hrtimer` 定义 |
| `timer_types.h` | `include/linux/` | `struct timer_list` 定义 |
| `timerqueue_types.h` | `include/linux/` | `struct timerqueue_node`、`struct timerqueue_head` 定义 |
| `tick-internal.h` | `kernel/time/` | Tick 子系统内部接口 |
| `tick-sched.h` | `kernel/time/` | `struct tick_sched` 定义 |
| `timekeeping.h` | `kernel/time/` | Timekeeping 内部接口 |
| `ntp_internal.h` | `kernel/time/` | NTP 内部接口 |

### 2.2 核心数据结构关系图

```
                    ┌──────────────────────────────────┐
                    │     struct clocksource            │
                    │  read() / mask / mult / shift     │
                    │  rating / flags / name            │
                    └────────────┬─────────────────────┘
                                 │
                    ┌────────────▼─────────────────────┐
                    │     struct tk_read_base           │
                    │  clock / mult / shift / mask      │
                    │  cycle_last / xtime_nsec / base   │
                    │  base_real                        │
                    └────────────┬─────────────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              ▼                  ▼                  ▼
    ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
    │  struct timekeeper│ │  struct tk_fast │ │  struct ntp_data│
    │  tkr_mono/raw    │ │  seq / base[2]  │ │  time_offset    │
    │  xtime_sec       │ │  (NMI安全读取)  │ │  time_freq      │
    │  offs_real/boot/ │ │                 │ │  tick_length    │
    │  tai/aux         │ │                 │ │  time_state     │
    └─────────────────┘ └─────────────────┘ └─────────────────┘

                    ┌──────────────────────────────────┐
                    │  struct clock_event_device        │
                    │  event_handler / set_next_event   │
                    │  features / rating / cpumask      │
                    │  state_use_accessors              │
                    └────────────┬─────────────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              ▼                  ▼                  ▼
    ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
    │  struct          │ │  struct          │ │  struct          │
    │  tick_device     │ │  tick_sched      │ │  hrtimer_cpu_base│
    │  evtdev / mode   │ │  sched_timer     │ │  clock_base[8]   │
    │                  │ │  idle_* / flags  │ │  active_bases    │
    └─────────────────┘ └─────────────────┘ └─────────────────┘

    ┌──────────────────────────────────────────────────────────┐
    │  struct timer_base          struct hrtimer               │
    │  clk / next_expiry          node / _softexpires          │
    │  pending_map / vectors[]    function / base / state      │
    │  (timer wheel)              (红黑树节点)                  │
    └──────────────────────────────────────────────────────────┘
```

---

## 3. Clocksource — 时钟源抽象层

### 3.1 概述

Clocksource 是 Linux 内核中对硬件自由运行计数器的抽象层。无论底层是 TSC（x86）、arch_timer（ARM）、还是平台 HPET，均通过 `struct clocksource` 提供统一的接口。Timekeeping 层通过 clocksource 的 `read()` 回调获取原始硬件周期计数，再通过 `mult` / `shift` 因子转换为纳秒。

### 3.2 核心数据结构

（[clocksource.h](file:///home/louis/code/linux/include/linux/clocksource.h#L57)）

```c
struct clocksource {
    u64         (*read)(struct clocksource *cs);  // 读取硬件计数器
    u64         mask;             // 计数器位宽掩码（如 64bit: ~0ULL）
    u32         mult;             // 周期→纳秒 乘数
    u32         shift;            // 周期→纳秒 位移（右移）
    u64         max_idle_ns;      // 允许的最大空闲时间（ns）
    u32         maxadj;           // mult 最大可调范围（~11%）
    u32         uncertainty_margin; // 每半秒的最大不确定性（ns）
    u64         max_cycles;       // 不溢出乘法的最大周期值
    u64         max_raw_delta;    // 负向运动检测的最大安全 delta
    const char  *name;            // 时钟源名称
    int         rating;           // 评级（1-499，越高越好）
    enum clocksource_ids id;      // 时钟源 ID
    unsigned long flags;          // 标志位
    struct clocksource_base *base;// 硬件基础抽象

    int (*enable)(struct clocksource *cs);    // 启用
    void (*disable)(struct clocksource *cs);   // 禁用
    void (*suspend)(struct clocksource *cs);   // 挂起
    void (*resume)(struct clocksource *cs);    // 恢复
};
```

**关键字段说明**：

| 字段 | 含义 |
|------|------|
| `read()` | 回调函数，读取硬件计数器的当前值 |
| `mask` | 当计数器不足 64 位时，做 `val & mask` 处理 |
| `mult`, `shift` | 转换公式：`ns = (cycles * mult) >> shift` |
| `rating` | 评级：1-99 仅调试，100-199 可用，200-299 良好，300-399 期望，400-499 完美 |
| `max_idle_ns` | 最大安全空闲时间，超过此值可能导致计数器溢出 |
| `max_cycles` | 不超过此值，`cycles * mult` 不会溢出 64 位 |

### 3.3 转换公式

```
nanoseconds = (cycles_delta * mult) >> shift
```

其中 `cycles_delta = (now - last) & mask`，`mult` 和 `shift` 通过 `clocks_calc_mult_shift()` 计算：

```c
void clocks_calc_mult_shift(u32 *mult, u32 *shift, u32 from, u32 to, u32 maxsec);
```

- `from`：源频率（如 1GHz = NSEC_PER_SEC）
- `to`：目标频率（如时钟源频率 19.2MHz）
- `maxsec`：保证不溢出的最大转换范围（秒）

### 3.4 标志位

（[clocksource.h](file:///home/louis/code/linux/include/linux/clocksource.h#L157)）

| 标志 | 含义 |
|------|------|
| `CLOCK_SOURCE_IS_CONTINUOUS` | 计数器在挂起期间继续运行 |
| `CLOCK_SOURCE_MUST_VERIFY` | 注册后需验证后才可使用 |
| `CLOCK_SOURCE_WATCHDOG` | 启用了看门狗监控 |
| `CLOCK_SOURCE_VALID_FOR_HRES` | 可用作高精度定时器的时钟源 |
| `CLOCK_SOURCE_UNSTABLE` | 被看门狗标记为不稳定 |
| `CLOCK_SOURCE_SUSPEND_NONSTOP` | 挂起期间计数器不停止 |
| `CLOCK_SOURCE_RESELECT` | 允许重新选择 |
| `CLOCK_SOURCE_VERIFY_PERCPU` | 需要对每个 CPU 进行验证 |

### 3.5 注册与选择流程

```
clocksource_register_khz(cs, khz)
  │
  ├─ __clocksource_register(cs)
  │    ├─ clocksource_update_freq()  // 计算 mult/shift
  │    ├─ clocksource_enqueue(cs)    // 按 rating 插入全局链表
  │    └─ clocksource_select()       // 选择最佳时钟源
  │         └─ __clocksource_select()
  │              ├─ 遍历 clocksource_list，选 rating 最高者
  │              ├─ 若与当前不同，调用 change_clocksource()
  │              └─ 通知 timekeeping: timekeeping_notify()
  │
  └─ 若 CLOCK_SOURCE_MUST_VERIFY
       └─ 启动 watchdog 定时器（clocksource_watchdog()）
```

### 3.6 看门狗机制（Watchdog）

（[clocksource.c](file:///home/louis/code/linux/kernel/time/clocksource.c)）

```
clocksource_watchdog()  ← 定时器回调（每 0.5 秒）
  │
  ├─ 遍历所有带 CLOCK_SOURCE_WATCHDOG 标志的时钟源
  ├─ 读取 watchdog 时钟源（如 HPET）当前值 → wd_now
  ├─ 读取被测时钟源当前值 → cs_now
  │
  ├─ 计算差值：cs_delta = (cs_now - cs_last) & mask
  │          wd_delta = (wd_now - wd_last) & mask
  │
  ├─ 若 cs_delta 与 wd_delta 偏差超过阈值（WATCHDOG_THRESHOLD）
  │   └─ 标记为不稳定：cs->flags |= CLOCK_SOURCE_UNSTABLE
  │       └─ clocksource_mark_unstable(cs)
  │            ├─ cs->mark_unstable(cs)  // 通知驱动
  │            └─ clocksource_select()   // 切换到备用时钟源
  │
  └─ 更新 cs_last / wd_last
```

### 3.7 涉及的关键函数

| 函数 | 位置 | 功能 |
|------|------|------|
| `clocksource_register_hz/khz()` | `clocksource.c` | 注册时钟源 |
| `__clocksource_register_scale()` | `clocksource.c` | 注册并计算 mult/shift |
| `clocksource_select()` | `clocksource.c` | 选择最佳时钟源 |
| `clocksource_change_rating()` | `clocksource.c` | 动态调整评级 |
| `clocksource_unbind()` | `clocksource.c` | 注销时钟源 |
| `clocksource_suspend/resume()` | `clocksource.c` | 挂起/恢复 |
| `clocksource_start_suspend_timing()` | `clocksource.c` | 开始挂起计时 |
| `clocksource_stop_suspend_timing()` | `clocksource.c` | 停止挂起计时 |
| `timekeeping_notify()` | `timekeeping.c` | 通知 timekeeping 切换时钟源 |

---

## 3.5 ARM64 架构定时器（Arch Timer）硬件与驱动分析

### 3.5.1 概述

ARM64 架构定时器（ARM Generic Timer）是 ARMv8 架构中内置的硬件定时器，由两个核心部分组成：

1. **System Counter（系统计数器）**：一个自由运行的 56~64 位递增计数器，为整个系统提供统一的全局时间基准
2. **Per-CPU Timer（每 CPU 定时器）**：每个 CPU 核心本地集成的可编程定时器，基于系统计数器工作

ARM 通用定时器在 Linux 内核中同时扮演 **clocksource**（通过 CNTVCT/CNTPCT 寄存器提供系统计数器）和 **clockevent**（通过 CVAL/TVAL 寄存器提供可编程事件）双重角色。

### 3.5.2 硬件寄存器一览

#### 系统级寄存器（全局）

| 寄存器 | 宽度 | 访问等级 | 描述 |
|--------|------|----------|------|
| `CNTFRQ_EL0` | 32 | EL0 | 系统计数器频率（Hz） |
| `CNTPCT_EL0` | 64 | EL0 | 物理计数器当前值 |
| `CNTVCT_EL0` | 64 | EL0 | 虚拟计数器当前值 |
| `CNTCTLBase` | - | - | 内存映射控制基地址（MMIO 框架） |
| `CNTTIDR` | 32 | - | 内存映射定时器标识寄存器 |

#### 每 CPU 定时器寄存器

| 寄存器 | 宽度 | 访问等级 | 描述 |
|--------|------|----------|------|
| `CNTP_CTL_EL0` | 32 | EL0 | 物理定时器控制寄存器 |
| `CNTP_CVAL_EL0` | 64 | EL0 | 物理定时器比较值（绝对值） |
| `CNTP_TVAL_EL0` | 32 | EL0 | 物理定时器计数值（相对值，有符号） |
| `CNTV_CTL_EL0` | 32 | EL0 | 虚拟定时器控制寄存器 |
| `CNTV_CVAL_EL0` | 64 | EL0 | 虚拟定时器比较值（绝对值） |
| `CNTV_TVAL_EL0` | 32 | EL0 | 虚拟定时器计数值（相对值） |
| `CNTKCTL_EL1` | 32 | EL1 | 定时器内核控制寄存器（用户态访问控制） |

**CNTP_CTL_EL0 控制寄存器格式**：

| 位域 | 名称 | 描述 |
|------|------|------|
| [0] | ENABLE | 定时器使能 |
| [1] | IMASK | 中断屏蔽（1=屏蔽） |
| [2] | ISTATUS | 中断状态（只读，1=待处理） |

**定时器触发条件**：`CNTP_CVAL_EL0 <= 系统计数器当前值` 时，ISTATUS 置 1。

#### 虚拟化相关寄存器

| 寄存器 | 描述 |
|--------|------|
| `CNTHP_CTL_EL2` | 管理程序物理定时器控制 |
| `CNTHP_CVAL_EL2` | 管理程序物理定时器比较值 |
| `CNTHV_CTL_EL2` | 管理程序虚拟定时器控制 |
| `CNTHV_CVAL_EL2` | 管理程序虚拟定时器比较值 |
| `CNTVOFF_EL2` | 虚拟计数器偏移（`CNTVCT = CNTPCT - CNTVOFF`） |

### 3.5.3 硬件架构图

```
                         System Counter (56~64-bit, free-running)
                               │
                               │  cntpct / cntvct
              ┌────────────────┼────────────────────┐
              │                │                     │
         ┌────┴────┐     ┌────┴────┐          ┌─────┴─────┐
         │ CPU 0   │     │ CPU 1   │   ...    │ CPU N     │
         │         │     │         │          │           │
         │ CNTP_CTL│     │ CNTP_CTL│          │ CNTP_CTL  │
         │ CNTP_CVAL│    │ CNTP_CVAL│         │ CNTP_CVAL │
         │ CNTV_CTL│     │ CNTV_CTL│          │ CNTV_CTL  │
         │ CNTV_CVAL│    │ CNTV_CVAL│         │ CNTV_CVAL │
         │         │     │         │          │           │
         │ PPI IRQ │     │ PPI IRQ │          │ PPI IRQ   │
         └────┬────┘     └────┬────┘          └─────┬─────┘
              │              │                     │
              └──────────────┼─────────────────────┘
                             │
                             ▼
                       GIC (Generic Interrupt Controller)
                       PPI 11: virt, PPI 14: phys, PPI 10: hyp, PPI 13: sec-phys
```

**PPI 中断映射**（ARMv8 标准）：

| PPI# | 名称 | 映射的页眉常量 |
|------|------|----------------|
| 10 | 管理程序物理定时器 | `ARCH_TIMER_HYP_PPI` |
| 11 | 虚拟定时器 | `ARCH_TIMER_VIRT_PPI` |
| 13 | 安全物理定时器 | `ARCH_TIMER_PHYS_SECURE_PPI` |
| 14 | 非安全物理定时器 | `ARCH_TIMER_PHYS_NONSECURE_PPI` |

### 3.5.4 初始化流程

#### 完整初始化调用链

```
start_kernel()
  │
  ├─ time_init()                                          ← 架构相关时间初始化
  │    │
  │    ├─ timer_probe()                                   ← DT 方式探测定时器
  │    │    └─ TIMER_OF_DECLARE(armv8_arch_timer, ...)
  │    │         └─ arch_timer_of_init(np)
  │    │              │
  │    │              ├─ [1] 解析 DTS 中断
  │    │              │    ├─ of_irq_get_byname(np, "sec-phys")  → PPI 13
  │    │              │    ├─ of_irq_get_byname(np, "phys")      → PPI 14
  │    │              │    ├─ of_irq_get_byname(np, "virt")      → PPI 11
  │    │              │    └─ of_irq_get_byname(np, "hyp-phys")  → PPI 10
  │    │              │
  │    │              ├─ [2] 读取计数器频率
  │    │              │    └─ arch_timer_get_cntfrq()
  │    │              │         └─ read_sysreg(cntfrq_el0)      ← 硬件寄存器
  │    │              │
  │    │              ├─ [3] 选择 PPI
  │    │              │    └─ arch_timer_select_ppi()
  │    │              │         ├─ is_kernel_in_hyp_mode()       → ARCH_TIMER_HYP_PPI
  │    │              │         ├─ ARCH_TIMER_VIRT_PPI (标准)
  │    │              │         └─ ARCH_TIMER_PHYS_NONSECURE_PPI (备选)
  │    │              │
  │    │              ├─ [4] 解析 DTS 属性
  │    │              │    ├─ "always-on" → !arch_timer_c3stop
  │    │              │    ├─ "arm,no-tick-in-suspend" → arch_counter_suspend_stop
  │    │              │    └─ 检查 errata workaround
  │    │              │
  │    │              ├─ [5] arch_timer_register()
  │    │              │    ├─ alloc_percpu(clock_event_device)
  │    │              │    ├─ request_percpu_irq(ppi, handler, ...)
  │    │              │    │    └─ arch_timer_handler_virt/phys
  │    │              │    ├─ cpuhp_setup_state(CPUHP_AP_ARM_ARCH_TIMER_STARTING,
  │    │              │    │     arch_timer_starting_cpu, ...)
  │    │              │    │    └─ 每个 CPU hotplug 上线时调用
  │    │              │    │         └─ __arch_timer_setup(clk)
  │    │              │    │              ├─ 设置 clock_event_device 回调
  │    │              │    │              ├─ clockevents_config_and_register()
  │    │              │    │              └─ enable_percpu_irq()
  │    │              │    └─ arch_timer_cpu_pm_init()
  │    │              │
  │    │              └─ [6] arch_timer_common_init()
  │    │                   ├─ arch_timer_banner()          ← 打印 "cp15 timer running at XMHz"
  │    │                   ├─ arch_counter_register()      ← 注册 clocksource
  │    │                   │    ├─ arch_timer_read_counter = cntvct/cntpct
  │    │                   │    ├─ clocksource_register_hz(&clocksource_counter, rate)
  │    │                   │    │    └─ clocksource_counter.rating = 400
  │    │                   │    │    └─ clocksource_counter.id = CSID_ARM_ARCH_COUNTER
  │    │                   │    ├─ timecounter_init()      ← KVM timecounter
  │    │                   │    └─ sched_clock_register()  ← 注册调度时钟
  │    │                   └─ arch_timer_arch_init()      ← arch/arm64 空函数
  │    │
  │    └─ (ACPI 路径) arch_timer_acpi_init()              ← 通过 GTDT 表
  │         └─ 流程类似 DT 路径，使用 acpi_gtdt_map_ppi()
  │
  ├─ clocksource_done_booting()                         ← 完成时钟源选择
  │    └─ clocksource_select()
  │         └─ 选择 clocksource_counter (rating=400)
  │
  └─ tick_init()                                        ← Tick 子系统初始化
```

### 3.5.5 DTS 绑定与设备树解析

**标准 DTS 节点**（[arm,arch_timer.yaml](file:///home/louis/code/linux/Documentation/devicetree/bindings/timer/arm,arch_timer.yaml)）：

```dts
timer {
    compatible = "arm,armv8-timer";
    interrupts = <GIC_PPI 13 IRQ_TYPE_LEVEL_LOW>,  /* secure phys */
                 <GIC_PPI 14 IRQ_TYPE_LEVEL_LOW>,  /* non-secure phys */
                 <GIC_PPI 11 IRQ_TYPE_LEVEL_LOW>,  /* virt */
                 <GIC_PPI 10 IRQ_TYPE_LEVEL_LOW>;  /* hyp phys */
    clock-frequency = <19200000>;                   /* 可选，不建议使用 */
    always-on;                                      /* 可选，空闲时不停 */
};
```

**ACPI 路径**：通过 GTDT（Generic Timer Description Table）描述，由 `arch_timer_acpi_init()` 解析。

### 3.5.6 Clocksource 注册

（[arch_timer.c](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L145)）

```c
static struct clocksource clocksource_counter = {
    .name   = "arch_sys_counter",
    .id     = CSID_ARM_ARCH_COUNTER,   // 唯一 ID，用于 VDSO 和跨时钟源同步
    .rating = 400,                     // 极高评级（仅次于完美 500）
    .read   = arch_counter_read,       // 读取函数
    .flags  = CLOCK_SOURCE_IS_CONTINUOUS,  // 永不停止
};
```

**计数器读取选择**（`arch_counter_register()` 中）：

```
无条件使用 CNTVCT（虚拟计数器）：
  ├─ 无 errata:         arch_counter_get_cntvct()
  │    └─ mrs %0, cntvct_el0
  │
  └─ 有 errata:         arch_counter_get_cntvct_stable()
       └─ 通过 erratum_handler 重定向到带 workaround 的读取函数

HYP 模式或无虚拟化条件下使用 CNTPCT（物理计数器）：
  类似上述选择，但使用 cntpct_el0
```

### 3.5.7 Clockevent 注册

（[arch_timer.c](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L674)）

```c
static void __arch_timer_setup(struct clock_event_device *clk)
{
    clk->features = CLOCK_EVT_FEAT_ONESHOT;
    if (arch_timer_c3stop)
        clk->features |= CLOCK_EVT_FEAT_C3STOP;
    clk->name = "arch_sys_timer";
    clk->rating = 450;                               // 高于 clocksource 的 400
    clk->cpumask = cpumask_of(smp_processor_id());
    clk->set_next_event = set_next_event_virt/phys;
    clk->set_state_shutdown = arch_timer_shutdown_virt/phys;
    clockevents_config_and_register(clk, arch_timer_rate, 0xf, max_delta);
}
```

**编程下一个事件**（`set_next_event`）：

```
set_next_event_virt(evt, clk)
  │
  ├─ ctrl = read_sysreg(cntv_ctl_el0)      // 读取控制寄存器
  ├─ ctrl |= ENABLE                         // 使能定时器
  ├─ ctrl &= ~IMASK                         // 取消中断屏蔽
  ├─ cnt = __arch_counter_get_cntvct()      // 读取当前虚拟计数器
  ├─ write_sysreg(cnt + evt, cntv_cval_el0) // 设置比较值 = 当前值 + 延时
  └─ write_sysreg(ctrl, cntv_ctl_el0)      // 写回控制寄存器
```

### 3.5.8 中断处理流程

```
[硬件事件] 系统计数器递增到 CNTV_CVAL == CNTVCT
     │
     ▼
GIC 产生 PPI 11 中断
     │
     ▼
arch_timer_handler_virt(irq, dev_id)
     │
     └─ timer_handler(ARCH_TIMER_VIRT_ACCESS, evt)
          │
          ├─ ctrl = read_sysreg(cntv_ctl_el0)   // 读取控制寄存器
          ├─ 检查 ISTATUS bit 是否置位
          │
          ├─ ctrl |= IMASK                      // 屏蔽中断（防止重入）
          ├─ write_sysreg(ctrl, cntv_ctl_el0)   // 写回
          │
          └─ evt->event_handler(evt)            // 调用 clock_event_device 回调
               │
               ├─ hrtimer_interrupt()           // 高精度模式
               │    └─ __hrtimer_run_queues()
               │
               └─ tick_handle_periodic()        // 周期模式
                    └─ do_timer(1) + update_process_times()
```

### 3.5.9 双模式切换

ARM 通用定时器始终工作在 **oneshot 模式**——即使需要周期性 tick，也是通过 `hrtimer` 在每次到期后重新编程实现的：

```
periodic tick 模拟（高精度模式）:
  tick_sched_timer()  ← hrtimer 回调，每 tick_period 触发一次
    │
    ├─ do_timer(1) / update_process_times()
    │
    └─ hrtimer_forward(timer, now, tick_period)  ← 重新编程
         └─ 返回 HRTIMER_RESTART

NOHZ 空闲（停止 tick）:
  tick_nohz_stop_tick()
    │
    └─ tick_program_event(next_timer, force)
         └─ clockevents_program_event(dev, expires, force)
              └─ dev->set_next_event(delta, dev)  ← 重新编程到下一个到期时间
```

### 3.5.10 Errata Workaround 机制

（[arch_timer.c](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer.c#L160)）

ARM 通用定时器存在多个已知硬件 errata，通过 `arch_timer_erratum_workaround` 结构体管理：

```c
struct arch_timer_erratum_workaround {
    enum arch_timer_erratum_match_type match_type;  // DT/ACPI/CAP 匹配方式
    const void *id;                                   // 匹配 ID 或 midr 范围
    const char *desc;                                 // 描述
    u64 (*read_cntpct_el0)(void);                     // 替换的物理计数器读取函数
    u64 (*read_cntvct_el0)(void);                     // 替换的虚拟计数器读取函数
    int (*set_next_event_phys)(unsigned long, struct clock_event_device *);
    int (*set_next_event_virt)(unsigned long, struct clock_event_device *);
    bool disable_compat_vdso;                         // 是否禁用 VDSO
};
```

**支持的 errata**：

| Errata | 影响 SoC | 问题 | 解决方法 |
|--------|----------|------|----------|
| FSL A-008585 | NXP QorIQ | 计数器读取可能返回错误值 | 连续读取直到两次值相同 |
| HISI 161010101 | Hisilicon | 计数器可能跳变 32 | 特殊读取序列 |
| Cortex-A73 858921 | Cortex-A73 | 计数器可能返回错误值 | 动态检测并替换读取函数 |
| SUN50I UNKNOWN1 | Allwinner A64 | 低位翻转时计数器不稳定 | 循环读取直到稳定 |

### 3.5.11 内存映射定时器（MMIO Timer）

除了 CPU 集成的 CP15 寄存器访问，ARM 通用定时器还有内存映射变体，通过 `arm,armv7-timer-mem` 兼容节点注册：

（[arm_arch_timer_mmio.c](file:///home/louis/code/linux/drivers/clocksource/arm_arch_timer_mmio.c#L248)）

```
arch_timer_mmio_probe(pdev)
  │
  ├─ of_populate_gt_block()       ← 解析 DTS 中的 frame 信息
  │    ├─ 每个 frame 对应一个定时器框架
  │    ├─ 包含 cntbase（计数器基地址）、phys_irq、virt_irq
  │    └─ 最多 8 个 frame
  │
  ├─ find_best_frame()           ← 选择最佳 frame
  │    ├─ 优先选 virtual 模式
  │    ├─ 检查 CNTACR 寄存器确定可访问性
  │    └─ 回退到 physical 模式
  │
  ├─ arch_timer_mmio_frame_register()
  │    ├─ devm_ioremap()         ← 映射寄存器
  │    ├─ 读取 CNTFRQ 获取频率
  │    ├─ devm_request_irq()     ← 注册 SPIs
  │    └─ arch_timer_mmio_setup()
  │         ├─ clockevents_config_and_register()  ← 注册 clockevent (rating=400)
  │         └─ clocksource_register_hz()           ← 注册 clocksource (rating=300)
  │              └─ "arch_mmio_counter"
```

### 3.5.12 完整硬件→驱动→子系统数据流

```
┌──────────────────────────────────────────────────────────────────────────┐
│                             硬件层 (Hardware)                             │
│                                                                          │
│  System Counter (56~64-bit, free-running)                                │
│       │  cntpct_el0 / cntvct_el0                                         │
│       │                                                                  │
│  Per-CPU Timer: CNTP_CTL / CNTP_CVAL / CNTV_CTL / CNTV_CVAL            │
│       │                                                                  │
│  GIC: PPI 11(virt) / PPI 14(phys) / PPI 10(hyp) / PPI 13(sec-phys)     │
└───────────────────────┬──────────────────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                             驱动层 (Driver)                               │
│  drivers/clocksource/arm_arch_timer.c                                    │
│                                                                          │
│  ┌───────────────────────┐      ┌──────────────────────────┐            │
│  │ clocksource_counter    │      │ per-CPU clock_event_dev  │            │
│  │ rating=400, id=ARM_ARCH│      │ rating=450, name=arch_timer          │
│  │ .read = arch_counter_  │      │ .set_next_event = set_   │            │
│  │        read()          │      │    next_event_virt/phys  │            │
│  │ .read 调用 cntvct_el0  │      │ .event_handler = 由 tick │            │
│  └───────────┬───────────┘      │ 子系统或 hrtimer 设置     │            │
│              │                  └──────────────┬───────────┘            │
│              │                                │                          │
│              ▼                                ▼                          │
│  ┌───────────────────────┐      ┌──────────────────────────┐            │
│  │ sched_clock_register() │      │ request_percpu_irq()     │            │
│  │ → sched_clock()        │      │ → arch_timer_handler_virt│            │
│  │ → 调度器时间戳         │      │ → evt->event_handler()  │            │
│  └───────────────────────┘      └──────────────────────────┘            │
└───────────────────────┬──────────────────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                           时间子系统层 (Time Subsystem)                    │
│                                                                          │
│  ┌──────────────┐   ┌──────────────┐   ┌────────────────────────┐      │
│  │ clocksource   │   │ clockevents  │   │ tick / hrtimer         │      │
│  │ 选最佳时钟源  │   │ 管理 event   │   │ 定时器框架            │      │
│  │ → 选到 400    │   │ 设备注册    │   │ → hrtimer_interrupt() │      │
│  └──────┬───────┘   └──────┬───────┘   └───────────┬────────────┘      │
│         │                  │                        │                    │
│         ▼                  ▼                        ▼                    │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │ timekeeping_core                                                 │   │
│  │ 使用 clocksource_counter 计算时间，更新 tk_fast/VDSO             │   │
│  │ update_wall_time() 每个 tick 累加时间                            │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │ VDSO (虚拟动态共享对象)                                            │   │
│  │ 用户态直接读取 cntvct_el0 + vdso_data 中的 mult/shift/base        │   │
│  │ → 零系统调用的 clock_gettime()                                    │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────┘
```

### 3.5.13 关键文件清单

| 文件 | 路径 | 行数 | 功能 |
|------|------|------|------|
| `arm_arch_timer.c` | `drivers/clocksource/` | ~1,276 | 主驱动（CP15 访问） |
| `arm_arch_timer_mmio.c` | `drivers/clocksource/` | ~442 | 内存映射变体驱动 |
| `arch_timer.h` | `arch/arm64/include/asm/` | ~229 | ARM64 架构访问封装 |
| `arm_arch_timer.h` | `include/clocksource/` | ~113 | 公共头文件（寄存器宏、枚举） |
| `arm,arch_timer.yaml` | `Documentation/.../timer/` | ~130 | DT 绑定文档 |
| `arm,arch_timer_mmio.yaml` | `Documentation/.../timer/` | ~120 | MMIO DT 绑定文档 |

### 3.5.14 Kconfig 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `ARM_ARCH_TIMER` | 选中 | 核心支持 |
| `ARM_ARCH_TIMER_EVTSTREAM` | y（若 ARM_ARCH_TIMER） | 事件流生成（WFE 唤醒） |
| `ARM_ARCH_TIMER_OOL_WORKAROUND` | 由 errata 选项选择 | 带外 workaround 框架 |
| `FSL_ERRATUM_A008585` | y | NXP 计数器 errata |
| `HISILICON_ERRATUM_161010101` | y | Hisilicon 计数器 errata |
| `ARM64_ERRATUM_858921` | y | Cortex-A73 计数器 errata |
| `SUN50I_ERRATUM_UNKNOWN1` | y | Allwinner A64 计数器 errata |

---

## 4. Clockevent — 时钟事件设备

### 4.1 概述

Clockevent 是对可编程硬件定时器的抽象，用于在指定时间触发中断。每个 CPU 通常有一个本地时钟事件设备（如 x86 APIC timer、ARM generic timer），支持 periodic（周期性）和 oneshot（单次触发）两种模式。

### 4.2 核心数据结构

（[clockchips.h](file:///home/louis/code/linux/include/linux/clockchips.h#L64)）

```c
struct clock_event_device {
    void (*event_handler)(struct clock_event_device *);  // 中断处理回调
    int  (*set_next_event)(unsigned long evt, struct clock_event_device *);
    int  (*set_next_ktime)(ktime_t expires, struct clock_event_device *);
    ktime_t next_event;                                  // 下一个事件时间
    u64 max_delta_ns;                                    // 最大可编程间隔(ns)
    u64 min_delta_ns;                                    // 最小可编程间隔(ns)
    u32 mult;                                            // ns→cycles 乘数
    u32 shift;                                           // ns→cycles 位移
    enum clock_event_state state_use_accessors;           // 当前状态
    unsigned int features;                                // 特性标志

    int (*set_state_periodic)(struct clock_event_device *);
    int (*set_state_oneshot)(struct clock_event_device *);
    int (*set_state_shutdown)(struct clock_event_device *);
    int (*tick_resume)(struct clock_event_device *);

    void (*broadcast)(const struct cpumask *mask);       // 广播函数
    const char *name;
    int rating;
    int irq;
    const struct cpumask *cpumask;
};
```

### 4.3 设备状态机

```
    ┌──────────┐
    │ DETACHED │  ← 初始状态
    └────┬─────┘
         │ register_device
         ▼
    ┌──────────┐
    │ SHUTDOWN │  ← 关闭状态（设备已断电）
    └────┬─────┘
         │
    ┌────┴────┐
    │         │
    ▼         ▼
┌─────────┐ ┌─────────┐
│PERIODIC │ │ ONESHOT │
└─────────┘ └────┬────┘
                  │
                  ▼
         ┌─────────────────┐
         │ ONESHOT_STOPPED │  ← 临时停止（无待处理事件）
         └─────────────────┘
```

### 4.4 特性标志

（[clockchips.h](file:///home/louis/code/linux/include/linux/clockchips.h#L33)）

| 标志 | 含义 |
|------|------|
| `CLOCK_EVT_FEAT_PERIODIC` | 支持 periodic 模式 |
| `CLOCK_EVT_FEAT_ONESHOT` | 支持 oneshot 模式 |
| `CLOCK_EVT_FEAT_KTIME` | 支持 `set_next_ktime` 接口 |
| `CLOCK_EVT_FEAT_C3STOP` | 在 C3 睡眠状态停止，需广播支持 |
| `CLOCK_EVT_FEAT_DUMMY` | 虚拟设备（仅用于标记） |
| `CLOCK_EVT_FEAT_DYNIRQ` | 广播模式下动态设置中断亲和性 |
| `CLOCK_EVT_FEAT_PERCPU` | 每个 CPU 一个实例 |
| `CLOCK_EVT_FEAT_HRTIMER` | 基于 hrtimer 的虚拟时钟事件设备 |

### 4.5 注册与编程流程

```
clockevents_register_device(dev)
  │
  ├─ 设置初始状态为 DETACHED
  ├─ 将 dev 加入 clockevent_devices 链表
  ├─ 调用 tick_check_new_device(dev)
  │    └─ tick_check_new_device()
  │         ├─ 遍历 online CPU，找最合适的 tick_device
  │         ├─ tick_check_replacement() 检查是否替换现有设备
  │         └─ tick_setup_device() 设置初始模式
  │
  └─  若需要 → 触发广播设备检查

clockevents_program_event(dev, expires, force)
  │
  ├─ 计算 delta = expires - now
  ├─ 检查 delta 是否在 [min_delta_ns, max_delta_ns] 范围内
  ├─ 若 delta < min_delta_ns → 强制设为 min_delta_ns
  │
  ├─ 调用 dev->set_next_event(delta_ticks, dev)
  │    └─ 或 dev->set_next_ktime(expires, dev)
  │
  └─ 更新 dev->next_event = expires
```

### 4.6 涉及的关键函数

| 函数 | 位置 | 功能 |
|------|------|------|
| `clockevents_register_device()` | `clockevents.c` | 注册时钟事件设备 |
| `clockevents_program_event()` | `clockevents.c` | 编程下一个事件 |
| `clockevents_switch_state()` | `clockevents.c` | 切换设备状态 |
| `clockevents_config_and_register()` | `clockevents.c` | 配置并注册 |
| `clockevents_update_freq()` | `clockevents.c` | 更新频率并重新计算 mult/shift |
| `clockevent_delta2ns()` | `clockevents.c` | 将设备 ticks 转为纳秒 |

---

## 5. Timekeeping — 核心时间管理

### 5.1 概述

Timekeeping 是时间子系统的核心，维护系统所有时钟（REALTIME、MONOTONIC、BOOTTIME、TAI、RAW）的当前值，处理 NTP 频率调整，以及挂起/恢复的时间补偿。

### 5.2 核心数据结构

#### 5.2.1 `struct tk_read_base` — 快速读取基础

（[timekeeper_internal.h](file:///home/louis/code/linux/include/linux/timekeeper_internal.h#L33)）

```c
struct tk_read_base {
    struct clocksource *clock;    // 当前使用的时钟源
    u64 mask;                     // 位宽掩码
    u64 cycle_last;               // 上次更新时的 cycle 值
    u32 mult;                     // NTP 调整后的乘数
    u32 shift;                    // 位移
    u64 xtime_nsec;               // 上次更新时累积的纳秒（移位后）
    ktime_t base;                 // 单调时钟基准时间
    u64 base_real;                // 实时时钟基准时间（NMI 安全读取用）
};
```

#### 5.2.2 `struct timekeeper` — 核心时钟管理器

（[timekeeper_internal.h](file:///home/louis/code/linux/include/linux/timekeeper_internal.h#L76)）

```c
struct timekeeper {
    /* Cacheline 0: */
    struct tk_read_base tkr_mono;       // 单调时钟读取基础

    /* Cacheline 1: */
    u64            xtime_sec;           // REALTIME 秒数
    unsigned long  ktime_sec;           // MONOTONIC 秒数
    struct timespec64 wall_to_monotonic; // REALTIME → MONOTONIC 偏移
    ktime_t        offs_real;           // MONOTONIC → REALTIME 偏移
    ktime_t        offs_boot;           // MONOTONIC → BOOTTIME 偏移
    ktime_t        offs_tai;            // MONOTONIC → TAI 偏移
    u32            coarse_nsec;         // 粗略时间纳秒（coarse 接口用）
    enum timekeeper_ids id;

    /* Cacheline 2: */
    struct tk_read_base tkr_raw;        // RAW 时钟读取基础
    u64            raw_sec;             // RAW 秒数

    /* Cacheline 3, 4: */
    u64            cycle_interval;      // 每个 NTP 间隔的 cycle 数
    u64            xtime_interval;      // 每个 NTP 间隔的移位纳秒
    s64            xtime_remainder;     // 舍入余数
    u64            raw_interval;
    ktime_t        next_leap_ktime;     // 下一个闰秒时间
    u64            ntp_tick;            // NTP tick 长度
    s64            ntp_error;           // NTP 累积误差
    s32            tai_offset;          // UTC → TAI 偏移（秒）
};
```

**五个时钟类型的关系**：

```
REALTIME  = MONOTONIC + offs_real
BOOTTIME  = MONOTONIC + offs_boot  (包含挂起时间)
TAI       = MONOTONIC + offs_tai   (TAI = UTC + 闰秒)
MONOTONIC = 基准时钟，从开机开始累计
RAW       = MONOTONIC 但不受 NTP 调频影响
```

#### 5.2.3 `struct tk_fast` — NMI 安全快速读取

（[timekeeping.c](file:///home/louis/code/linux/kernel/time/timekeeping.c#L96)）

```c
struct tk_fast {
    seqcount_latch_t seq;        // 锁存序列计数器
    struct tk_read_base base[2];  // 双缓冲，无锁读取
};
```

**工作原理**：写入时交替更新 `base[0]` 和 `base[1]`，读取时通过 `seq` 的最低 bit 选择当前有效的 base，无需获取锁。

### 5.3 时间读取 API 族

```
                     ┌──────────────────┐
                     │  ktime_get()      │  MONOTONIC (默认)
                     │  ktime_get_ns()   │
                     ├──────────────────┤
                     │  ktime_get_real() │  REALTIME (壁钟)
                     │  ktime_get_real_ns│
                     ├──────────────────┤
                     │  ktime_get_boot   │  BOOTTIME (含挂起)
                     │  ktime_get_boot_ns│
                     ├──────────────────┤
                     │  ktime_get_tai()  │  TAI 时间
                     │  ktime_get_tai_ns │
                     ├──────────────────┤
                     │  ktime_get_raw()  │  RAW 单调（无 NTP 调整）
                     ├──────────────────┤
                     │  ktime_get_coarse │  Coarse 版本（可能低精度）
                     │  *_ts64()         │  timespec64 版本
                     └──────────────────┘
```

**NMI 安全版本**（无锁，使用 seqcount_latch）：

| 函数 | 返回 |
|------|------|
| `ktime_get_mono_fast_ns()` | 快速单调时间（ns） |
| `ktime_get_raw_fast_ns()` | 快速 RAW 时间（ns） |
| `ktime_get_boot_fast_ns()` | 快速 BOOTTIME（ns） |
| `ktime_get_tai_fast_ns()` | 快速 TAI 时间（ns） |
| `ktime_get_real_fast_ns()` | 快速 REALTIME（ns） |

### 5.4 核心更新流程

#### 5.4.1 定时更新：`update_wall_time()`

```
update_wall_time()                    ← 每个 tick 调用（do_timer() →）
  │
  └─ timekeeping_advance(TK_ADV_TICK)
       │
       └─ __timekeeping_advance(tkd, mode)
            │
            ├─ timekeeping_forward_now(tk)  // 读取当前 cycle，累加 nsec
            │    ├─ cycle_now = tk_clock_read(&tk->tkr_mono)
            │    ├─ delta = clocksource_delta(cycle_now, tk->tkr_mono.cycle_last, ...)
            │    │   └─ 若 delta > max_raw_delta → 返回 0（防时间倒退）
            │    ├─ tk->tkr_mono.xtime_nsec += delta * mult
            │    ├─ tk->tkr_raw.xtime_nsec += delta * raw_mult
            │    └─ tk->tkr_mono.cycle_last = cycle_now
            │
            ├─ 计算当前 NTP 间隔内的累积偏移
            │
            ├─ 若累积偏移 >= cycle_interval：
            │    └─ logarithmic_accumulation(tk, offset, ...)
            │         ├─ tk->tkr_mono.xtime_nsec += xtime_interval
            │         ├─ tk->xtime_sec += 1
            │         ├─ tk->raw_sec += 1（raw 时钟）
            │         └─ NTP 调整：timekeeping_adjust(tk, offset)
            │              ├─ 根据 ntp_tick 和 ntp_error 调整 mult
            │              └─ 累积余数到 ntp_error
            │
            └─ 更新 tk_fast 和 VDSO 数据
                 ├─ update_fast_timekeeper(&tk->tkr_mono, &tk_fast_mono)
                 ├─ update_fast_timekeeper(&tk->tkr_raw, &tk_fast_raw)
                 └─ update_vsyscall(tk)  // 更新 VDSO 数据页
```

#### 5.4.2 时间获取：`ktime_get()`

```
ktime_get()
  │
  ├─ 尝试 NMI 快速路径：
  │    └─ ktime_get_mono_fast_ns()
  │         ├─ seq = raw_read_seqcount_latch(&tk_fast_mono.seq)
  │         ├─ base = tk_fast_mono.base[seq & 1]
  │         ├─ now = base.clock->read(base.clock)
  │         ├─ delta = (now - base.cycle_last) & base.mask
  │         └─ return base.base + ((delta * base.mult) >> base.shift)
  │
  └─ 若需精确值（或快速路径失败）：
       └─ ktime_get_slow()  ← 获取 tk_core.seq 读锁
            ├─ timekeeping_forward_now(tk)
            └─ 返回 tk->tkr_mono.base + xtime_nsec >> shift
```

#### 5.4.3 时间设置：`do_settimeofday64()`

```
do_settimeofday64(ts)
  │
  ├─ 获取 tk_core.lock
  ├─ timekeeping_forward_now(tk)      // 先更新到当前时刻
  ├─ tk_set_xtime(tk, ts)              // 设置新时间
  ├─ tk_set_wall_to_mono(tk, wtm)      // 更新 wall_to_monotonic 偏移
  └─ timekeeping_update_from_shadow()  // 发布更新
       ├─ memcpy(&tkd->timekeeper, &tkd->shadow_timekeeper, ...)
       ├─ update_fast_timekeeper()
       ├─ update_vsyscall()
       └─ tk_update_leap_state_all()
```

### 5.5 挂起/恢复流程

```
timekeeping_suspend()
  │
  ├─ read_persistent_clock64()  ← 读取持久时钟（RTC）
  ├─ timekeeping_forward_now(tk)
  ├─ clocksource_start_suspend_timing(clock, cycle_now)
  │    └─ 记录挂起前的 cycle 值
  └─ timekeeping_suspended = 1

timekeeping_resume()
  │
  ├─ read_persistent_clock64()  ← 读取恢复后的持久时钟
  ├─ clockevents_resume()
  ├─ clocksource_resume()
  │
  ├─ cycle_now = tk_clock_read(&tks->tkr_mono)
  ├─ nsec = clocksource_stop_suspend_timing(clock, cycle_now)
  │    └─ 计算挂起期间经过的纳秒
  │
  ├─ 若 nsec > 0（非停止时钟源成功）：
  │    └─ __timekeeping_inject_sleeptime(tk, ts_delta)
  │         ├─ tk_xtime_add(tk, &ts_delta)       // REALTIME 增加挂起时间
  │         └─ tk_update_sleep_time(tk, delta)    // offs_boot 增加挂起时间
  │
  ├─ 若持久时钟可用但无非停止时钟源：
  │    └─ 使用 RTC 差值计算挂起时间
  │
  ├─ timekeeping_suspended = 0
  └─ tick_resume()
```

### 5.6 涉及的关键函数

| 函数 | 功能 |
|------|------|
| `timekeeping_init()` | 系统启动时初始化时间管理 |
| `update_wall_time()` | 每个 tick 调用，累积时间 |
| `do_timer()` | 每个 tick 调用，更新 jiffies 并调用 `update_wall_time()` |
| `ktime_get()` | 获取单调时间 |
| `ktime_get_real_ts64()` | 获取实时时间 |
| `ktime_get_boottime()` | 获取启动时间（含挂起） |
| `ktime_get_raw()` | 获取 RAW 时间（无 NTP 调整） |
| `do_settimeofday64()` | 设置时间 |
| `timekeeping_inject_sleeptime64()` | 挂起恢复时注入睡眠时间 |
| `timekeeping_suspend/resume()` | 挂起/恢复时间管理 |
| `timekeeping_advance()` | 推进时间 |
| `update_fast_timekeeper()` | 更新 NMI 安全快速读取路径 |
| `ktime_get_snapshot()` | 获取系统时间快照 |
| `get_device_system_crosststamp()` | 获取设备与系统时间跨时间戳 |

---

## 6. Timer Wheel — 低精度定时器

### 6.1 概述

Timer Wheel 是内核经典的定时器实现，基于 9 级（HZ>100）或 8 级（HZ≤100）级联时间轮，每级 64 个桶，粒度从 1ms 到约 12 天。每个 CPU 拥有独立的 `timer_base`，支持本地定时器、全局定时器和延迟定时器三种类型。

### 6.2 核心数据结构

（[timer.c](file:///home/louis/code/linux/kernel/time/timer.c#L165)）

```c
struct timer_list {
    struct hlist_node  entry;       // 哈希链表节点
    unsigned long      expires;     // 到期 jiffies
    void (*function)(struct timer_list *);  // 回调函数
    u32                flags;       // 标志位
};

struct timer_base {
    raw_spinlock_t     lock;        // per-CPU 锁
    struct timer_list  *running_timer;  // 当前正在运行的定时器
    unsigned long      clk;         // 当前时钟（base time）
    unsigned long      next_expiry; // 下一个到期时间
    unsigned int       cpu;         // 所属 CPU
    bool               next_expiry_recalc;  // 需要重新计算 next_expiry
    bool               is_idle;     // 是否空闲
    bool               timers_pending; // 是否有待处理定时器
    DECLARE_BITMAP(pending_map, WHEEL_SIZE);  // 桶位图
    struct hlist_head  vectors[WHEEL_SIZE];   // 桶数组
};
```

### 6.3 时间轮结构

```
每个级别: 64 个桶（LVL_SIZE = 2^6 = 64）
深度: 9 级（HZ > 100）或 8 级（HZ ≤ 100）

级别结构:
Level 0: 粒度 = 1/HZ          范围 = 0 ~ 63 * (1/HZ)
Level 1: 粒度 = 2^3 / HZ      范围 = 64 ~ 511 * (1/HZ)
Level 2: 粒度 = 2^6 / HZ      范围 = 512 ~ 4095 * (1/HZ)
...
Level n: 粒度 = 2^(3n) / HZ   范围 = LVL_START(n) ~ LVL_START(n+1)-1

HZ=1000 时的实际粒度:
Level 0:   1 ms         0 ms -       63 ms
Level 1:   8 ms        64 ms -      511 ms
Level 2:  64 ms       512 ms -     4095 ms
Level 3: 512 ms      4096 ms -    32767 ms
Level 4:   4 s        32768 ms -   262143 ms
Level 5:  32 s       262144 ms -  2097151 ms
Level 6:   4 m       2097152 ms - 16777215 ms
Level 7:  34 m      16777216 ms - 134217727 ms
Level 8:   4 h      134217728 ms - 1073741822 ms (~12.4 天)
```

### 6.4 定时器添加流程

```
add_timer(timer)
  │
  └─ __mod_timer(timer, expires, flags)
       │
       ├─ [1] 锁定 timer_base
       ├─ [2] 若定时器已在运行（callback_running）→ 等待完成
       │
       ├─ [3] 计算桶索引:
       │    idx = (expires - base->clk)  // 相对于 base clock 的偏移
       │    level = 找 idx 所在的 level
       │    bucket = (idx >> LVL_SHIFT(level)) & LVL_MASK
       │    vec = LVL_OFFS(level) + bucket
       │
       ├─ [4] 从旧位置移除，插入新位置
       │    hlist_add_head(&timer->entry, &base->vectors[vec])
       │    set_bit(vec, base->pending_map)
       │
       ├─ [5] 更新 base->next_expiry
       │
       └─ [6] 若定时器插入到当前 CPU 且比现有 next_expiry 更早
            → 可能需要触发 IPI 重新编程硬件定时器
```

### 6.5 定时器到期处理

```
timer interrupt (硬件中断)
  │
  └─ __run_timers(base)
       │
       ├─ 锁定 base->lock
       ├─ 处理当前时钟桶的所有到期定时器
       │
       ├─ while (time_after_eq(jiffies, base->clk)):
       │    ├─ 从 level 0 的当前桶中取出所有定时器
       │    ├─ 执行每个定时器的回调函数（释放锁 → 执行 → 重锁）
       │    ├─ 若 level 0 的所有桶处理完，推进到下一个 level
       │    └─ base->clk++
       │
       ├─ 级联（cascading）:
       │    当 base->clk 跨越 level 边界时，将上一级的定时器
       │    重新分发到当前级的具体桶中
       │    注意：现代内核已无需级联（使用分级索引）
       │
       └─ 更新 base->next_expiry = find_next_expiry(base)
```

### 6.6 NOHZ 空闲处理

```
tick_nohz_stop_tick()  ← 进入空闲时停止 tick
  │
  ├─ 计算下一个定时器到期时间（timer_base->next_expiry）
  ├─ 计算 tick_sched->timer_expires
  ├─ 编程时钟事件设备在下一个定时器到期时唤醒
  └─ 停止周期性 tick

tick_nohz_restart_tick()  ← 退出空闲时恢复 tick
  │
  ├─ 重新编程时钟事件设备为周期性 tick
  └─ 恢复 tick_sched->sched_timer
```

### 6.7 涉及的关键函数

| 函数 | 功能 |
|------|------|
| `__init_timer()` | 初始化定时器 |
| `__mod_timer()` | 修改定时器到期时间 |
| `add_timer()` | 添加定时器 |
| `del_timer()` | 删除定时器 |
| `del_timer_sync()` | 同步删除定时器（等待回调完成） |
| `timer_reduce()` | 将定时器到期时间提前 |
| `__run_timers()` | 定时器到期处理（中断上下文） |
| `call_timer_fn()` | 执行定时器回调 |
| `collect_expired_timers()` | 收集已到期定时器 |
| `next_timer_interrupt()` | 查找下一个定时器中断时间 |

---

## 7. HRTimer — 高精度定时器

### 7.1 概述

HRTimer（High-Resolution Timer）提供纳秒级精度的定时器服务，基于红黑树（`timerqueue`）管理，每个 CPU 拥有 8 个 clock base，覆盖 4 种时钟类型（MONOTONIC、REALTIME、BOOTTIME、TAI）的硬中断和软中断版本。

### 7.2 核心数据结构

#### 7.2.1 `struct hrtimer` — 高精度定时器

（[hrtimer_types.h](file:///home/louis/code/linux/include/linux/hrtimer_types.h#L18)）

```c
struct hrtimer {
    struct timerqueue_node      node;          // 红黑树节点（含 expires）
    ktime_t                     _softexpires;  // 最早到期时间
    enum hrtimer_restart        (*function)(struct hrtimer *);  // 回调
    struct hrtimer_clock_base   *base;         // 所属 clock base
    u8                          state;         // 状态
    u8                          is_rel;        // 相对时间
    u8                          is_soft;       // 软中断上下文
    u8                          is_hard;       // 硬中断上下文（RT）
};
```

#### 7.2.2 `struct hrtimer_clock_base` — 时钟基

（[hrtimer_defs.h](file:///home/louis/code/linux/include/linux/hrtimer_defs.h#L17)）

```c
struct hrtimer_clock_base {
    struct hrtimer_cpu_base *cpu_base;    // 所属 CPU base
    unsigned int            index;        // base 类型索引
    clockid_t               clockid;      // 时钟 ID
    seqcount_raw_spinlock_t seq;          // 运行时保护
    struct hrtimer          *running;     // 当前正在运行的定时器
    struct timerqueue_head  active;       // 活跃定时器红黑树
    ktime_t                 offset;       // 到 MONOTONIC 的偏移
};
```

#### 7.2.3 `struct hrtimer_cpu_base` — CPU 级 base

（[hrtimer_defs.h](file:///home/louis/code/linux/include/linux/hrtimer_defs.h#L59)）

```c
struct hrtimer_cpu_base {
    raw_spinlock_t              lock;
    unsigned int                cpu;
    unsigned int                active_bases;    // 有活跃定时器的 base 位图
    unsigned int                clock_was_set_seq;
    unsigned int                hres_active:1;   // 高精度模式激活
    unsigned int                in_hrtirq:1;     // 正在 hrtimer_interrupt 中
    unsigned int                hang_detected:1;
    unsigned int                softirq_activated:1;
    unsigned int                online:1;
    ktime_t                     expires_next;    // 下一个到期时间
    struct hrtimer              *next_timer;     // 第一个到期的定时器
    ktime_t                     softirq_expires_next;
    struct hrtimer              *softirq_next_timer;
    struct hrtimer_clock_base   clock_base[HRTIMER_MAX_CLOCK_BASES]; // 8 个 base
    call_single_data_t          csd;             // IPI 回调数据
};
```

#### 7.2.4 8 个 Clock Base

| 索引 | 名称 | 时钟 ID | 特点 |
|------|------|---------|------|
| 0 | `HRTIMER_BASE_MONOTONIC` | `CLOCK_MONOTONIC` | 单调时钟，硬中断 |
| 1 | `HRTIMER_BASE_REALTIME` | `CLOCK_REALTIME` | 实时时钟，硬中断 |
| 2 | `HRTIMER_BASE_BOOTTIME` | `CLOCK_BOOTTIME` | 启动时间，含挂起，硬中断 |
| 3 | `HRTIMER_BASE_TAI` | `CLOCK_TAI` | TAI 时间，硬中断 |
| 4 | `HRTIMER_BASE_MONOTONIC_SOFT` | `CLOCK_MONOTONIC` | 软中断版本 |
| 5 | `HRTIMER_BASE_REALTIME_SOFT` | `CLOCK_REALTIME` | 软中断版本 |
| 6 | `HRTIMER_BASE_BOOTTIME_SOFT` | `CLOCK_BOOTTIME` | 软中断版本 |
| 7 | `HRTIMER_BASE_TAI_SOFT` | `CLOCK_TAI` | 软中断版本 |

**硬中断 vs 软中断**：硬中断定时器在 `hrtimer_interrupt()`（硬中断上下文）中到期；软中断定时器在 `HRTIMER_SOFTIRQ` 软中断上下文中到期，允许回调函数执行可能睡眠的操作。

### 7.3 定时器添加流程

```
hrtimer_start(timer, tim, mode)
  │
  └─ __hrtimer_start_range_ns(timer, tim, slack_ns, mode)
       │
       ├─ [1] 锁定 timer->base->cpu_base->lock
       │
       ├─ [2] 若定时器已在活跃队列中 → 移除
       │
       ├─ [3] 设置到期时间：
       │    ├─ 若 hres_active：timer->node.expires = tim (绝对时间)
       │    ├─ 否则：timer->node.expires = tim + base->offset
       │    └─ timer->_softexpires = tim (软到期时间，用于 slack)
       │
       ├─ [4] 插入红黑树：
       │    ├─ timerqueue_add(&base->active, &timer->node)
       │    └─ base->cpu_base->active_bases |= (1 << base->index)
       │
       ├─ [5] 若新定时器比当前 next_timer 更早：
       │    ├─ 更新 cpu_base->expires_next
       │    └─ 若 hres_active：
       │         └─ tick_program_event(expires_next, force)
       │              └─ clockevents_program_event(dev, expires, force)
       │
       └─ [6] 释放锁
```

### 7.4 到期处理流程

```
hrtimer_interrupt(dev)  ← 硬件时钟事件中断
  │
  ├─ 获取 cpu_base->lock
  ├─ cpu_base->in_hrtirq = 1
  │
  ├─ 循环处理到期定时器：
  │    └─ __hrtimer_run_queues(cpu_base, now)
  │         ├─ 遍历 active_bases 中每个有活跃定时器的 base
  │         ├─ 从红黑树中取出所有到期节点（expires <= now）
  │         │    └─ timerqueue_getexpired(&base->active)
  │         │
  │         ├─ 对每个到期的定时器：
  │         │    ├─ 从红黑树中移除
  │         │    ├─ base->running = timer
  │         │    ├─ 释放锁，调用 timer->function(timer)
  │         │    ├─ 重锁
  │         │    └─ 若返回 HRTIMER_RESTART → 重新插入
  │         │
  │         └─ 若所有定时器已处理 → 退出循环
  │
  ├─ 检查死锁（hang_detected）：
  │    ├─ 若循环执行时间超过 max_hang_time
  │    ├─ 递增 nr_hangs，更新 max_hang_time
  │    └─ 将 expires_next 推迟到 now + max_hang_time
  │
  ├─ 编程下一个事件：
  │    └─ tick_program_event(cpu_base->expires_next, force)
  │
  └─ 触发软中断 HRTIMER_SOFTIRQ 处理软中断定时器
       └─ run_hrtimer_softirq()
            └─ __hrtimer_run_queues()  // 处理 SOFT base 的定时器
```

### 7.5 涉及的关键函数

| 函数 | 功能 |
|------|------|
| `hrtimer_setup()` | 初始化 hrtimer |
| `hrtimer_start()` | 启动定时器 |
| `hrtimer_cancel()` | 取消定时器（等待回调完成） |
| `hrtimer_try_to_cancel()` | 尝试取消定时器 |
| `hrtimer_forward()` | 将定时器向前推进 |
| `hrtimer_interrupt()` | 定时器到期中断处理 |
| `__hrtimer_run_queues()` | 运行到期的定时器 |
| `hrtimer_nanosleep()` | 高精度睡眠系统调用 |
| `clock_nanosleep_restart()` | 时钟睡眠重启 |

---

## 8. Tick 子系统 — 周期性调度滴答

### 8.1 概述

Tick 子系统管理内核的周期性调度滴答。每个 CPU 的时钟事件设备被编程为周期性地产生中断，用于进程调度、时间更新、定时器到期等。Tick 子系统支持三种模式：periodic（周期性）、oneshot（单次，用于 NOHZ）、以及 broadcast（广播，用于 C3 睡眠）。

### 8.2 核心数据结构

#### 8.2.1 `struct tick_device` — 每 CPU tick 设备

（[tick-sched.h](file:///home/louis/code/linux/kernel/time/tick-sched.h#L7)）

```c
struct tick_device {
    struct clock_event_device *evtdev;  // 时钟事件设备
    enum tick_device_mode mode;         // 当前模式
};

enum tick_device_mode {
    TICKDEV_MODE_PERIODIC,    // 周期性模式
    TICKDEV_MODE_ONESHOT,     // 单次触发模式
};
```

#### 8.2.2 `struct tick_sched` — 每 CPU tick 调度状态

（[tick-sched.h](file:///home/louis/code/linux/kernel/time/tick-sched.h#L24)）

```c
struct tick_sched {
    unsigned long   flags;           // TS_FLAG_* 状态标志
    unsigned int    stalled_jiffies;
    unsigned long   last_tick_jiffies;
    struct hrtimer  sched_timer;     // 高精度模式下的调度定时器
    ktime_t         last_tick;       // 上次 tick 时间
    ktime_t         next_tick;       // 下次 tick 时间
    unsigned long   idle_jiffies;
    ktime_t         idle_entrytime;  // 进入空闲时间
    ktime_t         idle_exittime;   // 退出空闲时间
    u64             timer_expires_base;
    u64             timer_expires;   // 下一个定时器到期时间
    ktime_t         idle_expires;    // 空闲时到期时间
    unsigned long   idle_calls;      // 进入空闲次数
    unsigned long   idle_sleeps;     // 停止 tick 的空闲次数
    ktime_t         idle_sleeptime;  // 空闲睡眠总时间
    ktime_t         iowait_sleeptime; // IO 等待睡眠时间
};
```

**TS_FLAG 状态标志**：

| 标志 | 含义 |
|------|------|
| `TS_FLAG_INIDLE` | CPU 处于空闲状态 |
| `TS_FLAG_STOPPED` | 空闲 tick 已停止 |
| `TS_FLAG_IDLE_ACTIVE` | 活跃空闲模式 |
| `TS_FLAG_DO_TIMER_LAST` | 上次是 tick_do_timer_cpu |
| `TS_FLAG_NOHZ` | NOHZ 已启用 |
| `TS_FLAG_HIGHRES` | 高精度模式 |

### 8.3 全局变量

| 变量 | 类型 | 含义 |
|------|------|------|
| `tick_cpu_device` | `DEFINE_PER_CPU(struct tick_device)` | 每 CPU tick 设备 |
| `tick_cpu_sched` | `DEFINE_PER_CPU(struct tick_sched)` | 每 CPU tick 调度状态 |
| `tick_next_period` | `ktime_t` | 下一个 tick 周期时间 |
| `tick_do_timer_cpu` | `int` | 负责调用 do_timer() 的 CPU |

### 8.4 初始化流程

```
tick_init()  ← 系统启动初始化
  │
  └─ tick_broadcast_init()

tick_setup_device(dev, cpu, newdev)
  │
  ├─ 设置 per-CPU tick_device
  ├─ 若 tick_do_timer_cpu == TICK_DO_TIMER_BOOT
  │    └─ 设置当前 CPU 为 tick_do_timer_cpu
  │
  ├─ 若高精度模式已启用：
  │    └─ tick_setup_oneshot(dev, handler, next_event)
  │         └─ dev->set_state_oneshot()
  │            dev->event_handler = hrtimer_interrupt
  │            clockevents_program_event(dev, next_event, true)
  │
  ├─ 否则若 periodic 模式：
  │    └─ tick_setup_periodic(dev, broadcast)
  │         └─ dev->set_state_periodic()
  │            dev->event_handler = tick_handle_periodic
  │
  └─ 若需广播：
       └─ tick_device_uses_broadcast(dev, cpu)
```

### 8.5 Periodic 模式流程

```
tick_handle_periodic(dev)  ← 时钟事件设备中断
  │
  ├─ 若当前 CPU 是 tick_do_timer_cpu：
  │    └─ do_timer(1)  ← 更新 jiffies 和墙上时间
  │         ├─ write_seqlock(&jiffies_seq)
  │         ├─ jiffies_64 += 1
  │         ├─ update_wall_time()  ← 调用 timekeeping 推进
  │         ├─ update_process_times(user_mode)
  │         │    ├─ account_process_tick()  ← 进程时间统计
  │         │    └─ run_local_timers()  ← 触发软中断 TIMER_SOFTIRQ
  │         └─ write_sequnlock(&jiffies_seq)
  │
  ├─ 否则：
  │    └─ 仅 tick_nohz_full_update_tick()
  │
  └─ 若需广播：
       └─ tick_do_periodic_broadcast()
```

### 8.6 NOHZ 空闲流程

```
tick_nohz_idle_go_to_sleep()  ← CPU 进入空闲
  │
  ├─ tick_nohz_stop_tick(cpu)
  │    ├─ 计算下一个定时器到期时间
  │    │    ├─ 检查 timer_base->next_expiry
  │    │    ├─ 检查 hrtimer_cpu_base->expires_next
  │    │    └─ 取最小值
  │    │
  │    ├─ 若下一个事件远在未来（> 1 tick）：
  │    │    ├─ cpu_base->expires_next = next_timer
  │    │    ├─ tick_program_event(next_timer, force)
  │    │    ├─ set TS_FLAG_STOPPED
  │    │    └─ tick_sched->idle_sleeps++
  │    │
  │    └─ 否则：保持周期性 tick
  │
  │  [CPU 进入空闲，等待下一个事件唤醒]
  │
  └─ tick_nohz_idle_exit()  ← 被事件唤醒
       ├─ tick_nohz_restart_tick()
       │    ├─ 清除 TS_FLAG_STOPPED
       │    ├─ 重新编程 hrtimer 为周期性 tick
       │    └─ 更新 tick_sched->idle_sleeptime
       │
       └─ tick_do_update_jiffies64(now)  ← 更新错过的 jiffies
```

### 8.7 高精度 Tick 模式

```
tick_setup_sched_timer()  ← 从 periodic 切换到 hres 模式
  │
  ├─ hrtimer_setup(&ts->sched_timer, tick_sched_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_HARD)
  ├─ ts->sched_timer.is_hard = 1
  ├─ ts->next_tick = ktime_get() + tick_period
  └─ hrtimer_start(&ts->sched_timer, ts->next_tick, HRTIMER_MODE_ABS_PINNED_HARD)

tick_sched_timer(timer)  ← hrtimer 回调
  │
  ├─ now = ktime_get()
  │
  ├─ 若当前 CPU 是 tick_do_timer_cpu：
  │    └─ tick_do_update_jiffies64(now)  ← 更新 jiffies
  │
  ├─ update_process_times(user_mode)  ← 用户/内核时间统计
  │
  ├─ 计算下一次 tick 时间：
  │    └─ ts->next_tick = now + tick_period
  │
  └─ 重新启动 hrtimer：hrtimer_forward(timer, now, tick_period)
       └─ 返回 HRTIMER_RESTART
```

### 8.8 广播模式（Broadcast）

当 CPU 进入深睡眠（C3 及以上）时，本地时钟事件设备停止运行。此时需要另一个设备（广播设备）代为产生中断来唤醒所有睡眠 CPU。

```
tick_broadcast_oneshot_control(enter)
  │
  ├─ 若 CPU 进入 C3 睡眠：
  │    ├─ 将本地时钟事件设备标记为停止
  │    ├─ 将本地 CPU 加入 broadcast mask
  │    └─ 编程广播设备在下一个到期时间产生中断
  │
  └─ 若 CPU 退出 C3 睡眠：
       ├─ 从 broadcast mask 中移除
       └─ 恢复本地时钟事件设备

tick_handle_oneshot_broadcast(dev)  ← 广播设备中断
  │
  ├─ 遍历 broadcast mask 中的所有 CPU
  ├─ 对每个 CPU 检查其下一个到期时间
  ├─ 若到期时间已到：发送 IPI（CLOCK_EVT_NOTIFY_BROADCAST）
  └─ 编程广播设备在下一个最早到期时间
```

### 8.9 涉及的关键文件

| 文件 | 功能 |
|------|------|
| `tick-common.c` | Tick 公共管理，初始化、周期事件处理 |
| `tick-oneshot.c` | Oneshot 模式管理 |
| `tick-sched.c` | NOHZ 调度 tick 管理（空闲+全动态） |
| `tick-broadcast.c` | 广播模式处理 |
| `tick-broadcast-hrtimer.c` | 基于 hrtimer 的广播设备 |

---

## 9. NTP 协议层

### 9.1 概述

NTP（Network Time Protocol）层处理系统时间的频率调整和偏移校正。它维护 PLL（锁相环）状态，通过 `adjtimex()` 系统调用与用户态 NTP 守护进程交互，并提供 PPS（Pulse Per Second）支持。

### 9.2 核心数据结构

（[ntp.c](file:///home/louis/code/linux/kernel/time/ntp.c#L30)）

```c
struct ntp_data {
    unsigned long   tick_usec;          // USER_HZ 周期（微秒）
    u64             tick_length;        // 调整后的 tick 长度
    u64             tick_length_base;   // tick_length 基准值
    int             time_state;         // 时钟同步状态
    int             time_status;        // 状态位
    s64             time_offset;        // 时间偏移调整（ns）
    long            time_constant;      // PLL 时间常数
    long            time_maxerror;      // 最大误差（us）
    long            time_esterror;      // 估计误差（us）
    s64             time_freq;          // 频率偏移（ns/s 缩放）
    time64_t        time_reftime;       // 上次调整时间
    long            time_adjust;        // 一次性调整值
    s64             ntp_tick_adj;       // 启动参数可配的 tick 调整
    time64_t        ntp_next_leap_sec;  // 下一个闰秒时间
};
```

### 9.3 NTP 状态机

```
TIME_OK      ← 正常同步状态
TIME_INS     ← 插入正闰秒（等待 23:59:60）
TIME_DEL     ← 删除负闰秒
TIME_OOP     ← 闰秒进行中
TIME_WAIT    ← 闰秒后等待
TIME_ERROR   ← 同步丢失
```

### 9.4 频率调整流程

```
adjtimex(txc)  ← 系统调用入口
  │
  └─ do_adjtimex(txc)
       │
       ├─ 验证参数（time_constant、time_offset 等）
       │
       ├─ 若设置新值：
       │    ├─ 更新 time_offset、time_freq、time_constant
       │    ├─ timekeeping_advance(TK_ADV_FREQ)  ← 立即应用新频率
       │    └─ 更新 tk->ntp_tick 和调整个 tick 长度
       │
       ├─ 若 PPS 信号有效：
       │    └─ hardpps()  ← 处理 PPS 信号
       │
       └─ 返回当前 NTP 状态

timekeeping_adjust(tk, offset)  ← 每个 tick 调用
  │
  ├─ 计算 tick_length = ntp_tick_length()
  │    └─ 根据 time_freq 调整 tick_length 长度
  │
  ├─ 计算累积误差 ntp_error += (xtime_interval - ntp_tick)
  │
  ├─ 若 ntp_error 达到阈值：
  │    ├─ 调整 tkr_mono.mult（最大调整范围 maxadj）
  │    └─ 减小 ntp_error 累积
  │
  └─ 处理闰秒：
       └─ 若 next_leap_ktime 到达 → 调整 xtime_sec ± 1
```

### 9.5 涉及的关键函数

| 函数 | 功能 |
|------|------|
| `do_adjtimex()` | 处理 adjtimex 系统调用 |
| `ntp_tick_length()` | 计算当前 NTP tick 长度 |
| `ntp_update_frequency()` | 更新频率调整 |
| `hardpps()` | 处理 PPS 信号 |
| `second_overflow()` | 每秒调用的 NTP 溢出处理 |

---

## 10. Jiffies — 基准时钟源

### 10.1 概述

Jiffies 是内核最基础的时钟概念，表示自系统启动以来的 tick 数。`jiffies_64` 是 64 位计数器，`jiffies` 是 `jiffies_64` 的低 32 位（兼容旧代码）。Jiffies 也注册为一个 `clocksource`，但评级最低（rating=1），仅在无更好时钟源时使用。

### 10.2 核心数据结构

（[jiffies.c](file:///home/louis/code/linux/kernel/time/jiffies.c)）

```c
__visible u64 jiffies_64 __cacheline_aligned_in_smp = INITIAL_JIFFIES;

static struct clocksource clocksource_jiffies = {
    .name         = "jiffies",
    .rating       = 1,              // 最低有效评级
    .read         = jiffies_read,   // 返回 (u64) jiffies
    .mask         = CLOCKSOURCE_MASK(32),
    .mult         = TICK_NSEC << JIFFIES_SHIFT,
    .shift        = JIFFIES_SHIFT,
    .max_cycles   = 10,
};

__cacheline_aligned_in_smp DEFINE_RAW_SPINLOCK(jiffies_lock);
__cacheline_aligned_in_smp seqcount_raw_spinlock_t jiffies_seq =
    SEQCNT_RAW_SPINLOCK_ZERO(jiffies_seq, &jiffies_lock);
```

### 10.3 关键常量

| 常量 | 含义 |
|------|------|
| `HZ` | 内核每秒 tick 数（通常 1000 或 100） |
| `TICK_NSEC` | 每个 tick 的纳秒数 |
| `JIFFIES_SHIFT` | Jiffies 时钟源转换移位 |
| `INITIAL_JIFFIES` | 初始 jiffies 值（避免 0 值问题） |

### 10.4 涉及的关键函数

| 函数 | 功能 |
|------|------|
| `jiffies_read()` | 读取 jiffies 值 |
| `get_jiffies_64()` | 64 位安全读取（32 位平台需 seqcount 保护） |
| `register_refined_jiffies()` | 注册精炼版 jiffies 时钟源（基于实际频率） |
| `init_jiffies_clocksource()` | 初始化 jiffies 时钟源 |

---

## 11. sched_clock — 调度时钟

### 11.1 概述

`sched_clock()` 提供高精度、低开销的 CPU 本地时间戳，常用于调度器时间计算、跟踪（trace）以及内核时间测量。支持停用（stop）和恢复（start）操作，并可通过 `sched_clock_register()` 注册硬件时钟源。

### 11.2 核心数据结构

（[sched_clock.c](file:///home/louis/code/linux/kernel/time/sched_clock.c#L22)）

```c
struct clock_data {
    seqcount_latch_t        seq;               // 锁存序列计数器
    struct clock_read_data  read_data[2];      // 双缓冲读取数据
    ktime_t                 wrap_kt;           // 时钟包装时间
    unsigned long           rate;              // 时钟频率
    u64                     (*actual_read_sched_clock)(void);  // 硬件读取函数
};

struct clock_read_data {
    u64 (*read_sched_clock)(void);  // 调度时钟读取函数
    u64 (*mult)                    // 移位后的乘数
    u32 (*shift);                  // 移位
    u64 (*epoch_ns);               // 基准时间（ns）
    u64 (*epoch_cyc);              // 基准周期值
    u64 (*suspended);              // 挂起标志
};
```

### 11.3 读取流程

```
sched_clock()
  │
  └─ cyc_to_ns(read_sched_clock())
       │
       ├─ 读取硬件时钟源当前值
       ├─ 计算 delta = (now - epoch_cyc)
       ├─ 转换为 ns：delta * mult >> shift
       └─ 返回 epoch_ns + ns

sched_clock_read_begin()  ← NMI 安全读取开始
sched_clock_read_retry()  ← NMI 安全读取重试
```

### 11.4 涉及的关键函数

| 函数 | 功能 |
|------|------|
| `sched_clock_register()` | 注册新的调度时钟源 |
| `sched_clock()` | 读取当前调度时钟值 |
| `sched_clock_suspend()` | 挂起调度时钟 |
| `sched_clock_resume()` | 恢复调度时钟 |
| `sched_clock_tick()` | 每周期的调度时钟更新 |
| `sched_clock_idle_wakeup_event()` | 空闲唤醒事件 |

---

## 12. Alarmtimer — 闹钟定时器

### 12.1 概述

Alarmtimer 提供在系统挂起/睡眠期间仍能唤起的定时器。它基于 hrtimer 实现，但又与 RTC 设备关联，以便在系统进入 S3（挂起到内存）时继续工作。

### 12.2 核心数据结构

（[alarmtimer.c](file:///home/louis/code/linux/kernel/time/alarmtimer.c#L30)）

```c
struct alarm_base {
    spinlock_t              lock;              // base 锁
    struct timerqueue_head  timerqueue;        // 定时器红黑树队列
    ktime_t                 (*get_ktime)(void); // 读取当前时间
    clockid_t               base_clockid;      // 所属时钟 ID
};

enum alarmtimer_type {
    ALARM_REALTIME,        // CLOCK_REALTIME 基
    ALARM_BOOTTIME,        // CLOCK_BOOTTIME 基（含挂起）
    ALARM_NUMTYPE,         // 类型数量
};
```

### 12.3 涉及的关键函数

| 函数 | 功能 |
|------|------|
| `alarm_start()` | 启动闹钟定时器 |
| `alarm_cancel()` | 取消闹钟定时器 |
| `alarm_try_to_cancel()` | 尝试取消 |
| `alarm_restart()` | 重启闹钟定时器 |
| `alarmtimer_suspend()` | 挂起时将最近闹钟注册到 RTC |
| `alarmtimer_resume()` | 恢复时重新激活 |

---

## 13. POSIX 定时器

### 13.1 概述

POSIX 定时器提供 `timer_create()`、`timer_settime()`、`timer_gettime()` 等用户态接口，支持 `CLOCK_REALTIME`、`CLOCK_MONOTONIC`、`CLOCK_PROCESS_CPUTIME_ID` 和 `CLOCK_THREAD_CPUTIME_ID` 四种时钟类型。

### 13.2 核心数据结构

```c
struct k_itimer {
    struct hlist_node      list;        // 全局链表节点
    struct signal_struct   *sigq;       // 信号队列
    clockid_t              it_clock;    // 时钟 ID
    int                    it_id;       // 定时器 ID
    struct itimerspec64    it_interval; // 间隔
    struct itimerspec64    it_value;    // 到期值
    union {
        struct hrtimer      timer;      // 基于 hrtimer 的定时器
        struct cpu_timer    cpu;        // 基于 CPU 时钟的定时器
    } it;
};
```

### 13.3 涉及的关键文件

| 文件 | 功能 |
|------|------|
| `posix-timers.c` | POSIX 定时器框架（创建、删除、到期通知） |
| `posix-cpu-timers.c` | CPU 时间定时器（进程/线程级） |
| `posix-clock.c` | POSIX 时钟管理 |
| `itimer.c` | ITIMER 接口（setitimer/getitimer） |

---

## 14. VDSO — 快速用户态时间读取

### 14.1 概述

VDSO（Virtual Dynamic Shared Object）将部分时间读取功能暴露到用户空间，无需系统调用即可获取时间。通过 `update_vsyscall()` 在每次时间更新时将数据写入 `struct vdso_data` 页，用户态直接读取。

### 14.2 核心数据结构

```c
struct vdso_data {
    u64  cycle_last;          // 上次 cycle 值
    u64  mask;                // 时钟源掩码
    u32  mult;                // 乘数
    u32  shift;               // 位移
    u64  base_real;           // REALTIME 基准
    u64  base_mono;           // MONOTONIC 基准
    u64  sec;                 // 秒数
    u64  nsec;                // 纳秒数
    seqcount_t seq;           // 序列计数器
};
```

### 14.3 更新流程

```
update_vsyscall(tk)  ← 每次 timekeeping 更新时调用
  │
  ├─ 写入 vdso_data 到 VVAR 页
  ├─ 设置 cycle_last、mult、shift、base 等
  └─ 增加序列计数器（用户态通过它检测一致性）
```

### 14.4 涉及的关键函数

| 函数 | 文件 | 功能 |
|------|------|------|
| `update_vsyscall()` | 架构相关 | 更新 VDSO 数据页 |
| `update_vsyscall_tz()` | 架构相关 | 更新时区信息 |

---

## 15. 时间命名空间（Time Namespace）

### 15.1 概述

时间命名空间允许容器感知不同的系统时间偏移，使容器内的进程看到不同的 REALTIME 和 MONOTONIC 时间。

### 15.2 涉及的文件

- `time_namespace.c` — 时间命名空间核心逻辑
- `time.c`（`kernel/time/`）— `clock_gettime()` 等系统调用中处理时间命名空间偏移

### 15.3 关键机制

```c
struct time_namespace {
    struct kref           kref;
    struct ns_common      ns;
    struct timens_offsets offsets;  // REALTIME 和 MONOTONIC 偏移
};
```

---

## 16. 完整调用链总结

### 16.1 时间获取路径

```
用户态:
  clock_gettime(CLOCK_MONOTONIC, &ts)
    │
    ├─ [VDSO 快速路径] 直接读取 VVAR 页中的 vdso_data
    │    └─ 无需系统调用，用户态完成时间计算
    │
    └─ [系统调用路径]
         │
         └─ sys_clock_gettime()
              │
              ├─ ktime_get_clocktai_ts64()  // CLOCK_TAI
              ├─ ktime_get_real_ts64()       // CLOCK_REALTIME
              ├─ ktime_get_ts64()            // CLOCK_MONOTONIC
              └─ (其他时钟类型)
                   │
                   └─ timekeeping_get_ns()
                        ├─ tk_read(tkr)  → clocksource->read()
                        └─ 计算 delta * mult >> shift
```

### 16.2 定时器创建路径

```
用户态:
  timer_create(CLOCK_MONOTONIC, &evp, &timerid)
    │
    └─ sys_timer_create()
         └─ do_timer_create()
              ├─ alloc_posix_timer()
              │    └─ kzalloc(sizeof(struct k_itimer))
              │
              ├─ hrtimer_setup(&timer->it.hrtimer, posix_timer_fn, ...)
              │
              └─ 返回 timerid 到用户态

内核态:
  hrtimer_start(timer, expires, mode)
    │
    ├─ __hrtimer_start_range_ns()
    │    ├─ timerqueue_add(&base->active, &timer->node)
    │    └─ 若是最早到期：
    │         └─ tick_program_event(expires_next, force)
    │              └─ clockevents_program_event(dev, expires, force)
    │                   └─ dev->set_next_event(delta, dev)
    │
    └─ 硬件在到期时产生中断
```

### 16.3 定时器到期路径

```
硬中断: hrtimer_interrupt(dev)
  │
  ├─ __hrtimer_run_queues(cpu_base, now)
  │    ├─ timerqueue_getexpired(&base->active)
  │    ├─ 移除到期节点
  │    ├─ timer->function(timer)  ← 执行回调
  │    └─ 若返回 HRTIMER_RESTART → 重新插入
  │
  ├─ tick_program_event(next_expires)
  │
  └─ raise_softirq(HRTIMER_SOFTIRQ)  ← 处理软中断定时器
       └─ run_hrtimer_softirq()
            └─ __hrtimer_run_queues()  (SOFT base)
```

### 16.4 Tick 更新路径

```
周期性 tick 中断:
  tick_handle_periodic(dev)
    │
    ├─ do_timer(1)
    │    ├─ jiffies_64 += 1
    │    ├─ update_wall_time()
    │    │    └─ timekeeping_advance(TK_ADV_TICK)
    │    │         ├─ timekeeping_forward_now(tk)
    │    │         ├─ logarithmic_accumulation()
    │    │         ├─ timekeeping_adjust()  ← NTP 调整
    │    │         └─ update_fast_timekeeper()
    │    │         └─ update_vsyscall()
    │    │
    │    └─ update_process_times(user_mode)
    │         ├─ account_process_tick()  ← 进程时间统计
    │         └─ run_local_timers()  ← 触发 TIMER_SOFTIRQ
    │              └─ __run_timers()  ← 处理 timer wheel 到期
    │
    └─ 若非 tick_do_timer_cpu：
         └─ 仅更新本地统计
```

### 16.5 Timekeeping 设置路径

```
do_settimeofday64(ts)
  │
  ├─ timekeeping_forward_now(tk)  ← 先推进到当前时间
  ├─ tk_set_xtime(tk, ts)          ← 设置新时间
  ├─ tk_set_wall_to_mono(tk, wtm)  ← 更新偏移
  └─ timekeeping_update_from_shadow()
       ├─ 更新 tk_fast 双缓冲（NMI 安全路径）
       ├─ update_vsyscall()         ← 更新 VDSO
       ├─ tick_clock_notify()       ← 通知 hrtimer 时钟已变
       └─ clock_was_set()           ← 通知定时器刷新
```

### 16.6 挂起/恢复路径

```
[挂起]
  timekeeping_suspend()
    ├─ 读取持久时钟（RTC）
    ├─ timekeeping_forward_now()
    ├─ clocksource_start_suspend_timing()
    └─ 设置 timekeeping_suspended = 1

[恢复]
  timekeeping_resume()
    ├─ 读取持久时钟（RTC）
    ├─ 恢复时钟事件设备
    ├─ 恢复时钟源
    ├─ clocksource_stop_suspend_timing()  ← 计算挂起时间
    ├─ 若需要：__timekeeping_inject_sleeptime()
    │    ├─ 增加 REALTIME 秒数
    │    └─ 增加 offs_boot 偏移
    ├─ timekeeping_update_from_shadow()
    └─ tick_resume() + timerfd_resume()
```

---

## 17. 涉及的文件清单

### 17.1 核心头文件

| 文件 | 路径 | 功能 |
|------|------|------|
| `clocksource.h` | `include/linux/` | 时钟源结构体定义 |
| `clockchips.h` | `include/linux/` | 时钟事件设备结构体定义 |
| `timekeeper_internal.h` | `include/linux/` | timekeeper 结构体定义 |
| `hrtimer_defs.h` | `include/linux/` | hrtimer 内部结构体定义 |
| `hrtimer_types.h` | `include/linux/` | hrtimer 类型定义 |
| `timer_types.h` | `include/linux/` | timer_list 类型定义 |
| `timerqueue_types.h` | `include/linux/` | timerqueue 类型定义 |
| `timekeeping.h` | `include/linux/` | 时间管理接口 |
| `ktime.h` | `include/linux/` | ktime_t 类型与操作 |
| `jiffies.h` | `include/linux/` | jiffies 相关宏 |
| `timex.h` | `include/linux/` | NTP 接口 |
| `alarmtimer.h` | `include/linux/` | 闹钟定时器接口 |
| `sched_clock.h` | `include/linux/` | 调度时钟接口 |
| `posix-timers.h` | `include/linux/` | POSIX 定时器接口 |

### 17.2 实现文件

| 文件 | 路径 | 行数 | 功能 |
|------|------|------|------|
| `timekeeping.c` | `kernel/time/` | ~3,080 | 核心时间管理 |
| `hrtimer.c` | `kernel/time/` | ~2,390 | 高精度定时器 |
| `timer.c` | `kernel/time/` | ~2,300 | 低精度定时器 |
| `tick-sched.c` | `kernel/time/` | ~1,600 | NOHZ tick 调度 |
| `posix-cpu-timers.c` | `kernel/time/` | ~1,500 | POSIX CPU 定时器 |
| `clocksource.c` | `kernel/time/` | ~1,100 | 时钟源管理 |
| `posix-timers.c` | `kernel/time/` | ~1,070 | POSIX 定时器 |
| `ntp.c` | `kernel/time/` | ~1,000 | NTP 协议 |
| `alarmtimer.c` | `kernel/time/` | ~900 | 闹钟定时器 |
| `timer_migration.c` | `kernel/time/` | ~800 | 定时器迁移 |
| `time.c` | `kernel/time/` | ~700 | 系统调用接口 |
| `clockevents.c` | `kernel/time/` | ~600 | 时钟事件管理 |
| `sched_clock.c` | `kernel/time/` | ~500 | 调度时钟 |
| `tick-broadcast.c` | `kernel/time/` | ~450 | Tick 广播 |
| `tick-common.c` | `kernel/time/` | ~300 | Tick 公共管理 |
| `tick-oneshot.c` | `kernel/time/` | ~100 | Oneshot tick |
| `jiffies.c` | `kernel/time/` | ~100 | Jiffies 时钟源 |
| `timeconv.c` | `kernel/time/` | ~100 | 时间转换 |
| `timecounter.c` | `kernel/time/` | ~100 | 时间计数器 |
| `tick-broadcast-hrtimer.c` | `kernel/time/` | ~100 | 基于 hrtimer 的广播 |
| `itimer.c` | `kernel/time/` | ~100 | ITIMER 接口 |
| `posix-clock.c` | `kernel/time/` | ~100 | POSIX 时钟 |
| `timer_list.c` | `kernel/time/` | ~100 | /proc/timer_list |
| `vsyscall.c` | `kernel/time/` | ~100 | VDSO 时间数据 |
| `sleep_timeout.c` | `kernel/time/` | ~100 | 超时睡眠 |
| `time_test.c` | `kernel/time/` | ~100 | 时间测试 |
| `timekeeping_debug.c` | `kernel/time/` | ~100 | 调试支持 |
| `time_namespace.c` | `kernel/time/` | ~100 | 时间命名空间 |
| `test_udelay.c` | `kernel/time/` | ~100 | udelay 测试 |

### 17.3 内部头文件

| 文件 | 功能 |
|------|------|
| `kernel/time/timekeeping.h` | Timekeeping 内部接口 |
| `kernel/time/timekeeping_internal.h` | 内部结构体（tk_read_base, timekeeper） |
| `kernel/time/tick-internal.h` | Tick 子系统内部接口 |
| `kernel/time/tick-sched.h` | Tick 调度内部结构体 |
| `kernel/time/posix-timers.h` | POSIX 定时器内部接口 |

---

## 附录：关键数据结构快速参考

### 核心数据流图

```
clocksource_read()
    │
    │  (硬件周期计数)
    ▼
timekeeping_forward_now()
    │
    │  delta = (now - cycle_last) & mask
    │  xtime_nsec += delta * mult
    │  cycle_last = now
    ▼
logarithmic_accumulation()
    │
    │  xtime_nsec += xtime_interval
    │  xtime_sec += 1
    │  timekeeping_adjust()  ← NTP 频率微调
    ▼
update_fast_timekeeper()  ← 更新 NMI 安全路径
update_vsyscall()         ← 更新 VDSO
    │
    ├──→ ktime_get_mono_fast_ns()  (NMI 上下文)
    ├──→ clock_gettime()  (VDSO 用户态)
    └──→ ktime_get()  (系统调用)
```

### 定时器类型对比

| 特性 | Timer Wheel | HRTimer |
|------|-------------|---------|
| 数据结构 | 多级时间轮（64桶×9级） | 红黑树（timerqueue） |
| 精度 | 1/HZ（通常 1ms~10ms） | 纳秒级 |
| 到期检查 | 每次 tick 中断 | 每次硬件时钟中断 |
| 适用范围 | 短周期、大量定时器 | 高精度、少量定时器 |
| 每 CPU 开销 | 级联操作 | 红黑树插入/删除 |
| 支持 NMI 安全 | 否 | 否 |
| 基础时钟源 | jiffies | ktime（基于 clocksource） |

### 时钟类型对比

| 时钟类型 | 时钟 ID | 基准 | 特性 |
|----------|---------|------|------|
| REALTIME | `CLOCK_REALTIME` | UTC | 可被设置/adjtime 调整，受闰秒影响 |
| MONOTONIC | `CLOCK_MONOTONIC` | 开机时间 | 不可设置，不受闰秒影响 |
| BOOTTIME | `CLOCK_BOOTTIME` | 开机时间 | 含挂起时间，不可设置 |
| TAI | `CLOCK_TAI` | TAI | UTC + 闰秒偏移，不可设置 |
| RAW | `CLOCK_MONOTONIC_RAW` | 开机时间 | 不受 NTP 调频影响 |
| REALTIME_COARSE | `CLOCK_REALTIME_COARSE` | 快速近似 | 低频更新，低开销 |
| MONOTONIC_COARSE | `CLOCK_MONOTONIC_COARSE` | 快速近似 | 低频更新，低开销 |
| PROCESS_CPUTIME_ID | `CLOCK_PROCESS_CPUTIME_ID` | 进程 CPU | 进程消耗的 CPU 时间 |
| THREAD_CPUTIME_ID | `CLOCK_THREAD_CPUTIME_ID` | 线程 CPU | 线程消耗的 CPU 时间 |

---

*本文档基于 Linux 7.0 内核源代码分析生成，涵盖 `kernel/time/` 目录下主要时间子系统组件的核心流程、数据结构和函数调用栈。*