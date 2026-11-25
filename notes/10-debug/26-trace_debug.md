# 追踪调试功能

## 1. traceoff_on_warning

### 1.1 功能概述

`traceoff_on_warning` 是一个内核调试功能，当系统触发 `WARN()` 警告时自动关闭追踪功能。这有助于在发生警告时保留追踪缓冲区中的数据，防止后续的追踪记录覆盖关键的调试信息。

### 1.2 工作原理

#### 1.2.1 核心数据结构

```c
static int __disable_trace_on_warning;
```

- `__disable_trace_on_warning`：全局标志，控制是否启用该功能

#### 1.2.2 初始化与配置

```c
static int __init stop_trace_on_warning(char *str)
{
    if ((strcmp(str, "=0") != 0 && strcmp(str, "=off") != 0))
        __disable_trace_on_warning = 1;
    return 1;
}
__setup("traceoff_on_warning", stop_trace_on_warning);
```

- 支持内核启动参数 `traceoff_on_warning`
- 默认不启用，需显式指定启动参数

#### 1.2.3 运行时控制接口

```c
{
    .procname    = "traceoff_on_warning",
    .data        = &__disable_trace_on_warning,
    .maxlen      = sizeof(__disable_trace_on_warning),
    .mode        = 0644,
    .proc_handler = proc_dointvec,
},
```

- `/proc/sys/kernel/traceoff_on_warning`：运行时控制接口
- 值为 1 时启用，0 时禁用

#### 1.2.4 触发机制

```c
void disable_trace_on_warning(void)
{
    if (__disable_trace_on_warning) {
        struct trace_array *tr = READ_ONCE(printk_trace);

        trace_array_printk_buf(global_trace.array_buffer.buffer, _THIS_IP_,
            "Disabling tracing due to warning\n");
        tracing_off();

        if (tr != &global_trace) {
            trace_array_printk_buf(tr->array_buffer.buffer, _THIS_IP_,
                                   "Disabling tracing due to warning\n");
            tracer_tracing_off(tr);
        }
    }
}
```

- 当 `WARN()` 被触发时，调用 `disable_trace_on_warning()`
- 关闭全局追踪缓冲区和 printk 追踪缓冲区
- 在关闭前记录一条日志说明原因

### 1.3 使用方法

**启动时启用：**
```bash
traceoff_on_warning
```

**运行时启用：**
```bash
echo 1 > /proc/sys/kernel/traceoff_on_warning
```

**运行时禁用：**
```bash
echo 0 > /proc/sys/kernel/traceoff_on_warning
```

### 1.4 代码位置

- `kernel/trace/trace.c`

---

## 2. hist_debug

### 2.1 功能概述

`hist_debug` 是直方图触发器的调试功能，当启用 `CONFIG_HIST_TRIGGERS_DEBUG` 后，每个事件目录下会出现 `hist_debug` 文件，用于查看直方图触发器的内部数据结构和配置信息。

### 2.2 编译配置

```kconfig
config HIST_TRIGGERS_DEBUG
    bool "Hist trigger debug support"
    depends on HIST_TRIGGERS
    help
      Add "hist_debug" file for each event, which when read will
      dump out a bunch of internal details about the hist triggers
      defined on that event.
```

### 2.3 核心数据结构与函数

#### 2.3.1 文件操作接口

```c
const struct file_operations event_hist_debug_fops = {
    .open = event_hist_debug_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = tracing_single_release_file_tr,
};
```

#### 2.3.2 调试信息展示

```c
static int hist_debug_show(struct seq_file *m, void *v)
{
    struct event_trigger_data *data;
    struct trace_event_file *event_file;
    int n = 0;

    guard(mutex)(&event_mutex);

    event_file = event_file_file(m->private);
    if (unlikely(!event_file))
        return -ENODEV;

    list_for_each_entry(data, &event_file->triggers, list) {
        if (data->cmd_ops->trigger_type == ETT_EVENT_HIST)
            hist_trigger_debug_show(m, data, n++);
    }
    return 0;
}
```

- 遍历事件的所有触发器
- 对直方图类型的触发器调用 `hist_trigger_debug_show()` 输出详细信息

### 2.4 使用方法

**查看事件的直方图调试信息：**
```bash
cat /sys/kernel/tracing/events/<category>/<event>/hist_debug
```

**示例：**
```bash
cat /sys/kernel/tracing/events/sched/sched_waking/hist_debug
```

### 2.5 输出内容

`hist_debug` 文件输出以下信息：
- 直方图键字段定义
- 值字段定义（如 `hitcount`、自定义聚合函数）
- 触发器配置参数
- 过滤条件
- 排序规则
- 操作链信息

### 2.6 用途

1. **开发验证**：帮助开发者验证直方图触发器实现是否正确
2. **教育参考**：提供直方图触发器内部工作机制的详细信息
3. **问题排查**：当直方图行为不符合预期时，用于分析配置和数据结构

### 2.7 代码位置

- `kernel/trace/trace_events_hist.c`
- `kernel/trace/trace_events.c`