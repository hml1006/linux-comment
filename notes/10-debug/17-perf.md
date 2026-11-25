# perf

## 概述

perf 是 Linux 内核提供的性能分析工具，支持多种性能分析方式：

- **硬件性能计数器**：基于 CPU PMU（Performance Monitoring Unit）
- **软件事件**：内核追踪点、内核计数器
- **动态追踪**：kprobe、uprobe、tracepoint
- **采样分析**：CPU 采样、调用栈分析

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                         perf Architecture                          │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                      用户空间工具                            │   │
│  │  perf record / perf stat / perf top / perf report           │   │
│  │  tools/perf/                                               │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼ ioctl / syscall                       │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                      内核事件子系统                          │   │
│  │  kernel/events/core.c                                       │   │
│  │                                                             │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │   │
│  │  │ perf_event   │  │ perf_event   │  │ perf_event   │      │   │
│  │  │  context     │  │  context     │  │  context     │      │   │
│  │  │  (CPU/Task)  │  │  (CPU/Task)  │  │  (CPU/Task)  │      │   │
│  │  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │   │
│  │         │                 │                 │                │   │
│  │         ▼                 ▼                 ▼                │   │
│  │  ┌──────────────────────────────────────────────────┐        │   │
│  │  │              PMU 子系统 (perf_pmu)               │        │   │
│  │  │  - 硬件 PMU (Intel/AMD ARM)                      │        │   │
│  │  │  - 软件 PMU (tracepoint, kprobe, uprobe)         │        │   │
│  │  │  - 虚拟化 PMU (KVM guest)                        │        │   │
│  │  └──────────────────────────────────────────────────┘        │   │
│  │         │                                                     │   │
│  │         ▼                                                     │   │
│  │  ┌──────────────────────────────────────────────────┐        │   │
│  │  │              Ring Buffer                         │        │   │
│  │  │  - 每 CPU 环形缓冲区                            │        │   │
│  │  │  - AUX buffer (用于 Intel PT 等)                │        │   │
│  │  └──────────────────────────────────────────────────┘        │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 核心数据结构

### perf_event

```c
struct perf_event {
    struct list_head      active_list;
    struct list_head      owner_entry;
    struct perf_event_context *ctx;
    struct pmu           *pmu;
    struct hw_perf_event hw;
    struct perf_sample_data sample_data;
    u64                  event_id;
    enum perf_event_type type;
    __u64                config;
    __u64                config1;
    __u64                config2;
    int                  cpu;
    struct task_struct   *target;
    struct ring_buffer   *rb;
    struct perf_event   *group_leader;
    struct list_head      group_entry;
    struct file          *filp;
    unsigned long        flags;
    ...
};
```

### perf_event_context

```c
struct perf_event_context {
    struct list_head      event_list;
    struct list_head      active_ctx;
    struct list_head      pinned_ctx;
    raw_spinlock_t        lock;
    struct task_struct   *task;
    int                   nr_events;
    int                   nr_active;
    int                   nr_pinned;
    unsigned long         is_active;
    enum event_type_t     event_type;
    ...
};
```

### pmu

```c
struct pmu {
    struct list_head      entry;
    const char           *name;
    struct module        *module;
    int                  (*event_init)(struct perf_event *event);
    void                 (*event_exit)(struct perf_event *event);
    void                 (*start)(struct perf_event *event, int flags);
    void                 (*stop)(struct perf_event *event, int flags);
    void                 (*read)(struct perf_event *event);
    int                  (*add)(struct perf_event *event, int flags);
    void                 (*del)(struct perf_event *event, int flags);
    int                  (*event_idx)(struct perf_event *event);
    int                  max_events;
    bool                 exclusive;
    bool                 pmu_disable;
    ...
};
```

### ring_buffer

```c
struct ring_buffer {
    struct perf_event *event;
    unsigned long      data_size;
    unsigned long      nr_pages;
    struct page      **pages;
    void             **data_pages;
    unsigned long      head;
    unsigned long      tail;
    unsigned long      mask;
    struct perf_buffer *buffer;
    ...
};
```

## 事件类型

| 类型 | 描述 | 示例 |
|------|------|------|
| **硬件事件** | CPU PMU 计数器 | cycles, instructions, cache-misses |
| **软件事件** | 内核计数器 | cpu-clock, task-clock, page-faults |
| **追踪事件** | tracepoint 事件 | sched_switch, block_rq_issue |
| **kprobe** | 内核动态探针 | 任意内核函数入口/出口 |
| **uprobe** | 用户态动态探针 | 任意用户函数入口/出口 |
| **硬件断点** | 内存访问断点 | 读写/执行断点 |
| **硬件追踪** | CPU 追踪技术 | Intel PT, ARM SPE |

## Ring Buffer 机制

### 结构

```
┌─────────────────────────────────────────────────────────────────────┐
│                      Ring Buffer Structure                         │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Page 0: [data...]  ← head/tail pointer                    │   │
│  ├─────────────────────────────────────────────────────────────┤   │
│  │  Page 1: [data...]                                         │   │
│  ├─────────────────────────────────────────────────────────────┤   │
│  │  Page 2: [data...]                                         │   │
│  ├─────────────────────────────────────────────────────────────┤   │
│  │  ...                                                       │   │
│  ├─────────────────────────────────────────────────────────────┤   │
│  │  Page N: [data...]                                         │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  head: 生产者写入位置 (内核)                                       │
│  tail: 消费者读取位置 (用户空间)                                    │
└─────────────────────────────────────────────────────────────────────┘
```

### 工作流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                   Ring Buffer Workflow                            │
│                                                                     │
│  内核端 (生产者)                                                   │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  perf_event_output()                                        │   │
│  │  ├─ 获取当前 CPU 的 ring buffer                            │   │
│  │  ├─ 检查是否有足够空间                                      │   │
│  │  ├─ 如果空间不足，唤醒用户空间读线程                        │   │
│  │  └─ 写入数据到 ring buffer                                │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  用户空间 (消费者)                                                 │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  perf_mmap_read()                                          │   │
│  │  ├─ 读取 head 指针                                         │   │
│  │  ├─ 读取 head 和 tail 之间的数据                          │   │
│  │  └─ 更新 tail 指针                                         │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## PMU 子系统

### PMU 注册

```c
int perf_pmu_register(struct pmu *pmu, const char *name, int type)
{
    pmu->name = name;
    pmu->type = type;
    list_add(&pmu->entry, &pmus);
    ...
}
```

### 内置 PMU

| PMU | 描述 |
|-----|------|
| **cpu** | CPU 硬件性能计数器 |
| **tracepoint** | tracepoint 事件 |
| **kprobe** | 内核动态探针 |
| **uprobe** | 用户态动态探针 |
| **hw_breakpoint** | 硬件断点 |
| **software** | 软件事件 |

## 采样机制

### 采样流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Sampling Workflow                            │
│                                                                     │
│  1. 设置采样周期                                                    │
│     perf_event.attr.sample_period = N (每 N 个事件采样一次)          │
│                                                                     │
│  2. PMU 触发中断                                                    │
│     当事件计数达到采样周期时，触发 PMU 中断                          │
│                                                                     │
│  3. 采样处理                                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  perf_event_interrupt()                                     │   │
│  │  ├─ 禁用该事件                                              │   │
│  │  ├─ 获取采样数据 (IP, 时间戳, CPU)                          │   │
│  │  ├─ 获取调用栈 (callchain)                                  │   │
│  │  ├─ 获取寄存器状态                                          │   │
│  │  └─ 输出到 ring buffer                                      │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  4. 用户空间读取                                                    │
│     perf record 从 ring buffer 读取采样数据并写入文件                │
└─────────────────────────────────────────────────────────────────────┘
```

### 调用栈采样

```c
void perf_callchain_kernel(struct perf_callchain_entry_ctx *entry,
                           struct pt_regs *regs)
{
    unsigned int size = entry->max_stack;
    
    if (!entry->nr || size == entry->nr)
        return;
    
    entry->nr += unwind_stack(entry->stack + entry->nr,
                              &size - entry->nr,
                              regs, NULL);
}
```

## tracepoint 集成

### tracepoint 事件注册

```c
void perf_trace_event_register(struct trace_event *event)
{
    struct pmu *pmu = &event->perf_pmu;
    
    pmu->name = event->name;
    pmu->event_init = trace_event_perf_init;
    pmu->add = trace_event_perf_add;
    pmu->del = trace_event_perf_del;
    ...
    perf_pmu_register(pmu, event->name, PERF_TYPE_TRACEPOINT);
}
```

### tracepoint 事件触发

```c
static void trace_event_raw_event(struct trace_event_call *call,
                                  struct trace_event_file *file,
                                  struct trace_event_buffer *fbuffer,
                                  void *rec, unsigned long flags)
{
    struct perf_event *event;
    
    rcu_read_lock();
    list_for_each_entry_rcu(event, &call->perf_events, owner_entry) {
        if (!event->state)
            continue;
        perf_event_output(event, fbuffer->ctx, fbuffer->cpu,
                         &fbuffer->data, fbuffer->size);
    }
    rcu_read_unlock();
}
```

## 动态探针

### kprobe

```c
struct perf_kprobe {
    struct perf_event  *event;
    struct kprobe      *kp;
    struct kretprobe   *rp;
    unsigned long       addr;
    char               *symbol;
    unsigned int        offset;
    bool                retprobe;
};
```

### uprobe

```c
struct perf_uprobe {
    struct perf_event  *event;
    struct uprobe      *up;
    struct uprobe      *uretprobe;
    struct inode       *inode;
    loff_t              offset;
    char               *path;
};
```

## AUX Buffer (硬件追踪)

### Intel PT

```c
struct intel_pt {
    struct perf_event   *event;
    void                *buf;
    unsigned long        buf_size;
    unsigned long        head;
    unsigned long        tail;
    struct page        **pages;
    ...
};
```

AUX buffer 用于存储硬件追踪数据（如 Intel PT、ARM SPE），独立于主 ring buffer。

## 常用命令

### perf stat

```bash
# 统计程序性能
perf stat ./my_program

# 指定事件
perf stat -e cycles,instructions,cache-misses ./my_program

# 持续监控
perf stat -a sleep 10
```

### perf record

```bash
# 采样程序
perf record ./my_program

# 指定采样频率
perf record -F 99 ./my_program

# 监控所有进程
perf record -a sleep 10

# 包含调用栈
perf record -g ./my_program
```

### perf report

```bash
# 分析采样数据
perf report

# 显示调用图
perf report --call-graph

# 按函数名过滤
perf report -f my_function
```

### perf top

```bash
# 实时性能监控
perf top

# 指定事件
perf top -e cache-misses

# 显示调用栈
perf top -g
```

### perf trace

```bash
# 追踪系统调用
perf trace ./my_program

# 过滤系统调用
perf trace -e open,read,write ./my_program
```

### perf probe

```bash
# 添加 kprobe
perf probe --add 'my_function'

# 添加返回探针
perf probe --add 'my_function%return'

# 添加带参数的探针
perf probe --add 'my_function arg1 arg2'

# 删除探针
perf probe --del 'my_function'
```

## 编译配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_PERF_EVENTS` | 启用 perf 事件子系统 |
| `CONFIG_PERF_COUNTERS` | 启用性能计数器 |
| `CONFIG_HW_PERF_EVENTS` | 启用硬件性能计数器 |
| `CONFIG_PERF_USE_VMALLOC` | 使用 vmalloc 分配 ring buffer |
| `CONFIG_KPROBE_EVENTS` | 启用 kprobe 事件 |
| `CONFIG_UPROBE_EVENTS` | 启用 uprobe 事件 |
| `CONFIG_HW_BREAKPOINT` | 启用硬件断点 |
| `CONFIG_TRACEPOINTS` | 启用 tracepoint |

## 性能影响

| 方面 | 影响 |
|------|------|
| **采样频率** | 频率越高，开销越大 |
| **调用栈深度** | 越深，采样开销越大 |
| **事件数量** | 越多，开销越大 |
| **Ring buffer 大小** | 越大，内存开销越大 |

## 使用场景

1. **性能分析**：找出性能瓶颈
2. **热点分析**：找出 CPU 热点函数
3. **内存分析**：分析内存访问模式
4. **锁分析**：分析锁竞争
5. **系统调用追踪**：追踪系统调用

## 代码位置

| 文件 | 说明 |
|------|------|
| `kernel/events/core.c` | perf 事件核心实现 |
| `kernel/events/ring_buffer.c` | ring buffer 实现 |
| `kernel/events/callchain.c` | 调用栈采样 |
| `kernel/events/hw_breakpoint.c` | 硬件断点 |
| `kernel/events/uprobes.c` | uprobe 支持 |
| `tools/perf/` | 用户空间 perf 工具 |