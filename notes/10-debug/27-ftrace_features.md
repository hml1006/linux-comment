# ftrace 子功能详解

## 1. 函数调用图追踪 (Function Graph Tracer)

### 1.1 功能概述

函数调用图追踪器记录函数的进入和退出，生成完整的函数调用关系图，展示函数调用栈和执行时间。

### 1.2 编译配置

```kconfig
config FUNCTION_GRAPH_TRACER
    bool "Function Graph Tracer"
    depends on FUNCTION_TRACER
    help
      This tracer is used to show a function call graph including
      the duration of each function.
```

### 1.3 工作原理

函数调用图追踪器在每个函数入口记录时间戳，在函数返回前再次记录时间戳，计算函数执行耗时。

### 1.4 使用方法

**启用函数调用图追踪：**
```bash
echo function_graph > /sys/kernel/tracing/current_tracer
```

**查看调用图：**
```bash
cat /sys/kernel/tracing/trace
```

**输出示例：**
```
0)   2.590 us    |  } /* __wake_up */
0)   0.681 us    |  preempt_disable();
0)   0.537 us    |  _raw_spin_lock_irqsave();
0)   0.523 us    |  _raw_spin_unlock_irqrestore();
0)   0.474 us    |  preempt_enable();
0) + 6.403 us    |} /* scheduler_tick */
```

### 1.5 代码位置

- `kernel/trace/trace_functions_graph.c`
- `kernel/trace/fgraph.c`

---

## 2. 调度追踪 (Scheduler Tracer)

### 2.1 功能概述

调度追踪器记录任务唤醒和调度延迟，帮助分析调度器行为和任务延迟问题。

### 2.2 编译配置

```kconfig
config SCHED_TRACER
    bool "Scheduler latency tracer"
    depends on FUNCTION_TRACER
    help
      This tracer is used to record the latency of task wakeup
      and scheduling.
```

### 2.3 工作原理

记录从任务被唤醒到实际获得 CPU 执行的时间间隔，找出调度延迟的瓶颈。

### 2.4 使用方法

**启用调度追踪：**
```bash
echo wakeup > /sys/kernel/tracing/current_tracer
```

**查看调度延迟：**
```bash
cat /sys/kernel/tracing/trace
```

### 2.5 代码位置

- `kernel/trace/trace_sched_wakeup.c`

---

## 3. 中断关闭追踪 (IRQ Off Tracer)

### 3.1 功能概述

中断关闭追踪器记录中断被关闭的时间段，找出导致中断关闭时间过长的代码路径。

### 3.2 编译配置

```kconfig
config IRQSOFF_TRACER
    bool "Interrupts-off Latency Tracer"
    depends on FUNCTION_TRACER
    help
      This tracer records the maximum time for which interrupts
      are disabled.
```

### 3.3 工作原理

在 `local_irq_disable()` 和 `local_irq_enable()` 处埋点，记录中断关闭期间的函数调用轨迹和耗时。

### 3.4 核心数据结构

```c
static struct trace_array *irqsoff_trace __read_mostly;
static int tracer_enabled __read_mostly;

enum {
    TRACER_IRQS_OFF      = (1 << 1),
    TRACER_PREEMPT_OFF   = (1 << 2),
};

static int trace_type __read_mostly;
```

### 3.5 使用方法

**启用中断关闭追踪：**
```bash
echo irqsoff > /sys/kernel/tracing/current_tracer
```

**查看最大延迟：**
```bash
cat /sys/kernel/tracing/trace_max_latency
```

**查看延迟记录：**
```bash
cat /sys/kernel/tracing/trace
```

### 3.6 代码位置

- `kernel/trace/trace_irqsoff.c`

---

## 4. 抢占延迟追踪 (Preempt Tracer)

### 4.1 功能概述

抢占延迟追踪器记录内核抢占被关闭的时间段，找出导致抢占延迟的代码路径。

### 4.2 编译配置

```kconfig
config PREEMPT_TRACER
    bool "Preemption-off Latency Tracer"
    depends on FUNCTION_TRACER && PREEMPTION
    help
      This tracer records the maximum time for which preemption
      is disabled.
```

### 4.3 工作原理

在 `preempt_disable()` 和 `preempt_enable()` 处埋点，记录抢占关闭期间的函数调用轨迹和耗时。

### 4.4 使用方法

**启用抢占延迟追踪：**
```bash
echo preemptoff > /sys/kernel/tracing/current_tracer
```

**查看最大延迟：**
```bash
cat /sys/kernel/tracing/trace_max_latency
```

### 4.5 代码位置

- `kernel/trace/trace_irqsoff.c`

---

## 5. 硬件延迟追踪 (Hardware Latency Tracer)

### 5.1 功能概述

硬件延迟追踪器检测由硬件或固件引起的系统延迟，如 SMI (System Management Interrupt)。

### 5.2 编译配置

```kconfig
config HWLAT_TRACER
    bool "Hardware Latency Detector"
    help
      Detects large system latencies induced by the behavior of
      certain underlying system hardware or firmware.
```

### 5.3 工作原理

通过独占 CPU，采样 CPU 内置定时器，寻找不连续的读数来检测硬件引起的延迟。

### 5.4 核心数据结构

```c
struct hwlat_kthread_data {
    struct task_struct   *kthread;
    u64                  nmi_ts_start;
    u64                  nmi_total_ts;
    int                  nmi_count;
    int                  nmi_cpu;
};

struct hwlat_sample {
    u64                  seqnum;          /* unique sequence */
    u64                  duration;        /* delta */
    u64                  outer_duration;  /* delta (outer loop) */
    u64                  nmi_total_ts;    /* Total time spent in NMIs */
    struct timespec64    timestamp;       /* wall time */
    int                  nmi_count;       /* # NMIs during this sample */
    int                  count;           /* # of iterations over thresh */
};
```

### 5.5 使用方法

**启用硬件延迟追踪：**
```bash
echo hwlat > /sys/kernel/tracing/current_tracer
```

**配置参数：**
```bash
# 设置采样窗口（微秒）
echo 1000000 > /sys/kernel/tracing/hwlat_detector/sample_window

# 设置采样宽度（微秒）
echo 500000 > /sys/kernel/tracing/hwlat_detector/sample_width

# 设置延迟阈值（微秒）
echo 10 > /sys/kernel/tracing/tracing_thresh
```

### 5.6 注意事项

该追踪器会主动占用 CPU，可能引入额外延迟，不适合在生产环境中使用。

### 5.7 代码位置

- `kernel/trace/trace_hwlat.c`

---

## 6. 操作系统噪声追踪 (OS Noise Tracer)

### 6.1 功能概述

OS Noise 追踪器计算运行线程遭受的操作系统噪声，包括中断、调度等引起的延迟。

### 6.2 编译配置

```kconfig
config OSNOISE_TRACER
    bool "OS noise tracer"
    depends on TRACE_IRQFLAGS_SUPPORT && FUNCTION_TRACER
    help
      Traces the OS noise that affects a thread.
```

### 6.3 工作原理

运行一个高优先级线程，测量其在执行过程中被中断、调度等事件打断的时间总和。

### 6.4 使用方法

**启用 OS Noise 追踪：**
```bash
echo osnoise > /sys/kernel/tracing/current_tracer
```

**查看噪声统计：**
```bash
cat /sys/kernel/tracing/osnoise/cpu0/stats
```

### 6.5 代码位置

- `kernel/trace/trace_osnoise.c`

---

## 7. 事件追踪

### 7.1 功能概述

事件追踪通过 tracepoints 记录内核事件，如系统调用、调度、内存分配等。

### 7.2 使用方法

**启用事件追踪：**
```bash
# 启用特定事件
echo 1 > /sys/kernel/tracing/events/sched/sched_switch/enable

# 启用所有事件
echo 1 > /sys/kernel/tracing/events/enable

# 查看追踪结果
cat /sys/kernel/tracing/trace
```

**输出示例：**
```
          <idle>-0       [000] d.h.  8359.772692: sched_switch: prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==> next_comm=bash next_pid=1234 next_prio=120
```

### 7.3 常用事件类别

| 类别 | 说明 |
|------|------|
| `sched` | 调度相关事件 |
| `irq` | 中断相关事件 |
| `kmem` | 内存分配相关事件 |
| `syscalls` | 系统调用相关事件 |
| `block` | 块设备相关事件 |
| `net` | 网络相关事件 |

---

## 8. 追踪器选择与配置

### 8.1 可用追踪器列表

```bash
cat /sys/kernel/tracing/available_tracers
```

常见追踪器：
- `function` - 函数调用追踪
- `function_graph` - 函数调用图追踪
- `irqsoff` - 中断关闭延迟追踪
- `preemptoff` - 抢占延迟追踪
- `preemptirqsoff` - 中断和抢占延迟追踪
- `wakeup` - 调度唤醒延迟追踪
- `wakeup_rt` - RT 任务唤醒延迟追踪
- `hwlat` - 硬件延迟检测
- `osnoise` - OS 噪声追踪

### 8.2 配置选项

| 文件 | 说明 |
|------|------|
| `current_tracer` | 当前启用的追踪器 |
| `available_tracers` | 可用的追踪器列表 |
| `trace` | 追踪缓冲区内容 |
| `trace_pipe` | 追踪缓冲区（消费模式） |
| `tracing_on` | 开启/关闭追踪 |
| `tracing_thresh` | 追踪阈值（微秒） |
| `buffer_size_kb` | 缓冲区大小（KB） |
| `max_graph_depth` | 函数调用图最大深度 |