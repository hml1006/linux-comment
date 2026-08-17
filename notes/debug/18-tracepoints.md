# Tracepoints — 静态跟踪点

## 概述

Tracepoints 是内核中静态定义的跟踪点，使用 `TRACE_EVENT` 宏在编译时埋点。它们是一种零开销（zero-overhead）的调试机制，只有在被启用时才会产生性能开销。Tracepoints 是 Linux 内核追踪基础设施的核心组成部分，被 ftrace、perf、BPF 等多个子系统广泛使用。

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        Tracepoints 架构                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐ │
│  │  声明阶段        │     │  编译阶段        │     │  运行阶段        │ │
│  │  (TRACE_EVENT)   │────▶│  (7 stages)      │────▶│  (tracefs/perf)  │ │
│  └──────────────────┘     └──────────────────┘     └──────────────────┘ │
│         │                        │                        │             │
│         ▼                        ▼                        ▼             │
│  ┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐ │
│  │ 定义事件结构     │     │ 生成事件处理函数 │     │ 启用/禁用事件    │ │
│  │ 定义参数列表     │     │ 生成数据偏移     │     │ 读取事件数据     │ │
│  │ 定义打印格式     │     │ 生成注册逻辑     │     │ 过滤事件         │ │
│  └──────────────────┘     └──────────────────┘     └──────────────────┘ │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

## 核心数据结构

### struct tracepoint

```c
struct tracepoint {
    const char *name;                /* 跟踪点名称 */
    struct static_key_false key;     /* 静态分支键，用于零开销检查 */
    struct static_call_key *static_call_key;
    void *static_call_tramp;
    void *iterator;
    void *probestub;
    struct tracepoint_func __rcu *funcs;  /* 注册的探针函数列表 */
    struct tracepoint_ext *ext;      /* 扩展信息（注册/注销函数） */
};
```

**关键字段说明**：
- `key`：`static_key_false` 类型，通过 `static_branch_unlikely()` 实现零开销检查
- `funcs`：RCU 保护的探针函数数组，支持多探针注册
- `ext`：包含可选的注册和注销回调函数

### struct trace_event_call

```c
struct trace_event_call {
    struct list_head list;
    struct trace_event_class *class;    /* 所属事件类 */
    union {
        const char *name;               /* 事件名称 */
        struct tracepoint *tp;          /* 关联的 tracepoint */
    };
    struct trace_event event;
    char *print_fmt;                    /* 打印格式字符串 */
    union {
        void *module;                   /* 所属模块 */
        atomic_t refcnt;                /* 动态事件引用计数 */
    };
    void *data;
    int flags;                          /* TRACE_EVENT_FL_* 标志 */
#ifdef CONFIG_PERF_EVENTS
    int perf_refcount;
    struct hlist_head __percpu *perf_events;
    struct bpf_prog_array __rcu *prog_array;
    int (*perf_perm)(struct trace_event_call *, struct perf_event *);
#endif
};
```

### struct trace_event_class

```c
struct trace_event_class {
    const char *system;                    /* 事件所属系统（如 sched、irq） */
    void *probe;                           /* 探针函数 */
#ifdef CONFIG_PERF_EVENTS
    void *perf_probe;                      /* perf 探针函数 */
#endif
    int (*reg)(struct trace_event_call *event, enum trace_reg type, void *data);
    struct trace_event_fields *fields_array;  /* 字段定义数组 */
    struct list_head *(*get_fields)(struct trace_event_call *);
    struct list_head fields;               /* 字段列表 */
    int (*raw_init)(struct trace_event_call *);
};
```

### struct trace_event_file

```c
struct trace_event_file {
    struct list_head list;
    struct trace_event_call *event_call;    /* 关联的事件调用 */
    struct event_filter __rcu *filter;      /* 事件过滤器 */
    struct eventfs_inode *ei;
    struct trace_array *tr;
    struct trace_subsystem_dir *system;
    struct list_head triggers;              /* 触发器列表 */
    unsigned long flags;                    /* EVENT_FILE_FL_* 标志 */
    refcount_t ref;                         /* 文件引用计数 */
    atomic_t sm_ref;                        /* 软模式引用计数 */
    atomic_t tm_ref;                        /* 触发模式引用计数 */
};
```

## TRACE_EVENT 宏详解

`TRACE_EVENT` 是定义 tracepoint 的核心宏，其完整语法如下：

```c
TRACE_EVENT(event_name,

    TP_PROTO(prototype),        /* 函数原型，定义参数类型 */

    TP_ARGS(args),              /* 参数列表，传递给探针 */

    TP_STRUCT__entry(           /* 定义事件数据结构 */
        __field(type, name)     /* 固定大小字段 */
        __array(type, name, len)/* 固定长度数组 */
        __dynamic_array(type, name) /* 动态长度数组 */
        __string(name, expr)    /* 字符串字段 */
    ),

    TP_fast_assign(             /* 赋值逻辑 */
        __entry->field = value;
        __assign_str(name);     /* 字符串赋值 */
    ),

    TP_printk("format string", args)  /* 打印格式 */
);
```

### 字段定义宏

| 宏 | 说明 | 示例 |
|----|------|------|
| `__field(type, name)` | 固定大小字段 | `__field(pid_t, pid)` |
| `__array(type, name, len)` | 固定长度数组 | `__array(char, comm, TASK_COMM_LEN)` |
| `__dynamic_array(type, name)` | 动态长度数组 | `__dynamic_array(char, buf)` |
| `__string(name, expr)` | 字符串字段（自动处理） | `__string(comm, t->comm)` |
| `__field_struct(type, name)` | 结构体字段 | `__field_struct(struct task_struct *, task)` |

### DECLARE_EVENT_CLASS / DEFINE_EVENT

当多个事件共享相同的数据结构和处理逻辑时，可以使用模板机制：

```c
/* 定义事件模板 */
DECLARE_EVENT_CLASS(sched_wakeup_template,
    TP_PROTO(struct task_struct *p),
    TP_ARGS(__perf_task(p)),
    TP_STRUCT__entry(
        __array(char, comm, TASK_COMM_LEN)
        __field(pid_t, pid)
        __field(int, prio)
    ),
    TP_fast_assign(
        __assign_str(comm);
        __entry->pid = p->pid;
        __entry->prio = p->prio;
    ),
    TP_printk("comm=%s pid=%d prio=%d", __get_str(comm), __entry->pid, __entry->prio)
);

/* 基于模板定义具体事件 */
DEFINE_EVENT(sched_wakeup_template, sched_wakeup,
    TP_PROTO(struct task_struct *p),
    TP_ARGS(p)
);

DEFINE_EVENT(sched_wakeup_template, sched_wakeup_new,
    TP_PROTO(struct task_struct *p),
    TP_ARGS(p)
);
```

## 编译阶段（7 Stages）

`TRACE_EVENT` 宏在编译时通过 7 个阶段展开，每个阶段生成不同的代码：

### Stage 1 - 定义原始事件结构

生成 `struct trace_event_raw_<name>`，包含事件数据字段：

```c
struct trace_event_raw_sched_wakeup {
    struct trace_entry ent;
    char comm[TASK_COMM_LEN];
    pid_t pid;
    int prio;
    char __data[];
};
```

### Stage 2 - 定义数据偏移结构

生成 `struct trace_event_data_offsets_<name>`，用于记录动态字段的偏移和长度：

```c
struct trace_event_data_offsets_sched_wakeup {
    u32 comm;
    u32 pid;
    u32 prio;
};
```

### Stage 3 - 生成原始输出函数

生成 `trace_raw_output_<name>` 函数，用于将原始数据转换为可读格式：

```c
static notrace enum print_line_t
trace_raw_output_sched_wakeup(struct trace_iterator *iter, int flags,
                              struct trace_event *trace_event)
{
    struct trace_event_raw_sched_wakeup *field;
    // 解析并打印事件数据
}
```

### Stage 4 - 定义事件字段数组

生成 `trace_event_fields_<name>`，描述每个字段的类型、名称、偏移等信息：

```c
static struct trace_event_fields trace_event_fields_sched_wakeup[] = {
    { .type = "char", .name = "comm", .size = 16, ... },
    { .type = "pid_t", .name = "pid", .size = 4, ... },
    { .type = "int", .name = "prio", .size = 4, ... },
    {}
};
```

### Stage 5 - 获取偏移函数

生成 `trace_event_get_offsets_<name>` 函数，计算动态字段的偏移和数据大小：

```c
static inline notrace int trace_event_get_offsets_sched_wakeup(
    struct trace_event_data_offsets_sched_wakeup *__data_offsets,
    struct task_struct *p)
{
    int __data_size = 0;
    // 计算各字段偏移
    return __data_size;
}
```

### Stage 6 - 生成事件回调函数

生成 `trace_event_raw_event_<name>` 函数，这是事件触发时的核心处理逻辑：

```c
static notrace void
trace_event_raw_event_sched_wakeup(void *__data, struct task_struct *p)
{
    guard(preempt_notrace)();
    struct trace_event_file *trace_file = __data;
    struct trace_event_raw_sched_wakeup *entry;
    
    if (trace_trigger_soft_disabled(trace_file))
        return;
    
    entry = trace_event_buffer_reserve(&fbuffer, trace_file, sizeof(*entry));
    if (!entry)
        return;
    
    // 填充事件数据
    __assign_str(comm);
    entry->pid = p->pid;
    
    trace_event_buffer_commit(&fbuffer);
}
```

### Stage 7 - 定义事件类和事件调用

生成 `event_class_<name>` 和 `event_<name>` 结构，并将事件注册到 `_ftrace_events` 段：

```c
static struct trace_event_class __used __refdata event_class_sched_wakeup = {
    .system = "sched",
    .fields_array = trace_event_fields_sched_wakeup,
    .probe = trace_event_raw_event_sched_wakeup,
    .reg = trace_event_reg,
};

static struct trace_event_call __used event_sched_wakeup = {
    .class = &event_class_sched_wakeup,
    { .tp = &__tracepoint_sched_wakeup },
    .event.funcs = &trace_event_type_funcs_sched_wakeup,
    .print_fmt = print_fmt_sched_wakeup,
    .flags = TRACE_EVENT_FL_TRACEPOINT,
};

static struct trace_event_call __used
__section("_ftrace_events") *__event_sched_wakeup = &event_sched_wakeup;
```

## 运行时机制

### 事件注册与启用

内核启动时，`trace_events_init()` 函数遍历 `_ftrace_events` 段中的所有事件，并注册到 `ftrace_events` 链表。

启用事件的流程：

```
echo 1 > /sys/kernel/tracing/events/sched/sched_wakeup/enable
           │
           ▼
    trace_array_set_clr_event()
           │
           ▼
    trace_add_event_call()
           │
           ▼
    tracepoint_probe_register()  ← 将事件回调注册到 tracepoint
           │
           ▼
    static_key_slow_inc()        ← 启用静态分支
```

### 事件触发流程

当代码调用 `trace_sched_wakeup(p)` 时：

```
trace_sched_wakeup(p)
       │
       ▼
static_branch_unlikely(&key)  ← 零开销检查
       │
       ▼ (如果启用)
tracepoint_synchronize_unregister()
       │
       ▼
rcu_read_lock()
       │
       ▼
for_each_func(funcs)           ← 遍历所有注册的探针
    func->func(func->data, args)
       │
       ▼
trace_event_raw_event_xxx()   ← 事件处理函数
       │
       ▼
trace_event_buffer_reserve()   ← 预留环形缓冲区空间
       │
       ▼
填充事件数据
       │
       ▼
trace_event_buffer_commit()    ← 提交事件
```

### 零开销设计

Tracepoints 的零开销设计基于以下机制：

1. **静态分支预测**：`static_key_false` + `static_branch_unlikely()`
   - 当事件未启用时，编译器将检查优化为 `nop`
   - 仅在启用时产生开销

2. **条件编译**：`CONFIG_TRACEPOINTS` 未启用时，所有 tracepoint 调用被移除

3. **RCU 保护**：探针列表使用 RCU 保护，支持无锁遍历

## 事件标志（TRACE_EVENT_FL_*）

| 标志 | 说明 |
|------|------|
| `TRACE_EVENT_FL_CAP_ANY` | 任何用户都可以通过 perf 启用 |
| `TRACE_EVENT_FL_NO_SET_FILTER` | 过滤器设置错误时忽略 |
| `TRACE_EVENT_FL_IGNORE_ENABLE` | 内部事件，不通过 debugfs 启用 |
| `TRACE_EVENT_FL_TRACEPOINT` | 事件是 tracepoint 类型 |
| `TRACE_EVENT_FL_DYNAMIC` | 动态事件（运行时创建） |
| `TRACE_EVENT_FL_KPROBE` | kprobe 事件 |
| `TRACE_EVENT_FL_UPROBE` | uprobe 事件 |
| `TRACE_EVENT_FL_EPROBE` | 事件探针 |
| `TRACE_EVENT_FL_FPROBE` | 函数探针 |
| `TRACE_EVENT_FL_CUSTOM` | 自定义事件 |

## 文件系统接口

### tracefs 接口

```
/sys/kernel/tracing/events/
├── sched/                    # 系统目录
│   ├── sched_wakeup/         # 事件目录
│   │   ├── enable            # 启用/禁用事件
│   │   ├── filter            # 事件过滤器
│   │   ├── format            # 事件格式定义
│   │   └── id                # 事件 ID
│   └── ...
├── irq/
└── ...
```

### format 文件示例

```bash
$ cat /sys/kernel/tracing/events/sched/sched_wakeup/format
name: sched_wakeup
ID: 160
format:
	field:unsigned short common_type;	offset:0;	size:2;	signed:0;
	field:unsigned char common_flags;	offset:2;	size:1;	signed:0;
	field:unsigned char common_preempt_count;	offset:3;	size:1;	signed:0;
	field:int common_pid;	offset:4;	size:4;	signed:1;

	field:char comm[16];	offset:8;	size:16;	signed:0;
	field:pid_t pid;	offset:24;	size:4;	signed:1;
	field:int prio;	offset:28;	size:4;	signed:1;
	field:int success;	offset:32;	size:4;	signed:1;

print fmt: "comm=%s pid=%d prio=%d success=%d target_cpu=%d", __get_str(comm), REC->pid, REC->prio, REC->success, REC->target_cpu
```

## 常用系统的 tracepoint

| 系统 | 主要 tracepoint |
|------|----------------|
| sched | sched_wakeup, sched_switch, sched_process_fork, sched_process_exec |
| irq | irq_handler_entry, irq_handler_exit, softirq_entry, softirq_exit |
| workqueue | workqueue_execute_start, workqueue_execute_end |
| block | block_rq_issue, block_rq_complete, block_bio_queue |
| mm | mm_page_alloc, mm_page_free, mm_page_reclaim |
| syscalls | sys_enter_*, sys_exit_* |

## 使用示例

### 使用 ftrace 跟踪调度事件

```bash
# 进入 tracing 目录
cd /sys/kernel/tracing

# 启用 sched_wakeup 事件
echo 1 > events/sched/sched_wakeup/enable

# 查看跟踪结果
cat trace

# 禁用事件
echo 0 > events/sched/sched_wakeup/enable
```

### 使用 perf 采集 tracepoint 事件

```bash
# 采集调度事件
perf record -e sched:sched_wakeup -a -g -- sleep 10

# 查看结果
perf report

# 实时查看
perf trace -e sched:sched_wakeup
```

### 使用 BPF 程序处理 tracepoint

```c
SEC("tracepoint/sched/sched_wakeup")
int bpf_prog(struct trace_event_raw_sched_wakeup *ctx)
{
    char comm[TASK_COMM_LEN];
    bpf_get_current_comm(&comm, sizeof(comm));
    // 处理事件数据
    return 0;
}
```

## 编译配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_TRACEPOINTS` | 启用 tracepoint 支持（必需） |
| `CONFIG_EVENT_TRACING` | 启用事件追踪 |
| `CONFIG_PERF_EVENTS` | 启用 perf 事件支持 |
| `CONFIG_DYNAMIC_EVENTS` | 启用动态事件（kprobe/uprobe） |
| `CONFIG_TRACER_SNAPSHOT` | 启用追踪快照功能 |

## 性能影响

- **未启用时**：零开销，仅存在一个条件分支预测（被优化为 `nop`）
- **启用时**：取决于事件频率和数据大小，通常在纳秒级别
- **高频事件**：建议配合过滤器使用，减少数据量

## 代码位置

| 文件 | 说明 |
|------|------|
| `include/trace/trace_events.h` | TRACE_EVENT 宏展开定义 |
| `include/trace/define_trace.h` | tracepoint 创建逻辑 |
| `include/linux/trace_event.h` | 核心数据结构定义 |
| `include/linux/tracepoint.h` | tracepoint API |
| `include/linux/tracepoint-defs.h` | tracepoint 基础定义 |
| `kernel/trace/trace_events.c` | 事件注册和管理 |
| `kernel/trace/trace_events_filter.c` | 事件过滤 |
| `include/trace/events/*.h` | 各子系统的 tracepoint 定义 |