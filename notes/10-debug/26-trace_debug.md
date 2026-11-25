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

### 1.5 真正的触发点：`__warn()` 与 `__report_bug()`

`disable_trace_on_warning()` 自己不会"被 WARN 自动调用"，它必须由 WARN 的公共路径显式调用。本树有两处：

```c
/* lib/bug.c, __report_bug() */
	/*
	 * Before the once logic so suppressed warnings do not consume
	 * the single-fire budget of WARN_ON_ONCE().
	 */
	if (warning && kunit_is_suppressed_warning(true))
		return BUG_TRAP_TYPE_WARN;

	disable_trace_on_warning();
```

```c
/* kernel/panic.c, __warn() */
void __warn(const char *file, int line, void *caller, unsigned taint,
	    struct pt_regs *regs, struct warn_args *args)
{
	nbcon_cpu_emergency_enter();

	disable_trace_on_warning();

	if (file) {
		pr_warn("WARNING: %s:%d at %pS, CPU#%d: %s/%d\n", ...);
	}
	...
	check_panic_on_warn("kernel");
	...
	dump_stack();
	...
	add_taint(taint, LOCKDEP_STILL_OK);
}
```

由此得到两条确定性结论：

1. **追踪一定在 `dump_stack()`/`panic()` 之前被关掉**。`__warn()` 的第一件事就是 `disable_trace_on_warning()`，所以 ring buffer 冻结的是"警告发生前"的内容，不会被后续 `dump_stack()` 的 trace 覆盖。
2. **关闭动作是幂等的**。`lib/bug.c` 把 `disable_trace_on_warning()` 放在 `once` 判重之前，因此重复触发的 `WARN_ON_ONCE` 仍会再次调用它；由于此时 ring buffer 已经 `off`，重复调用只是多打印一行 "Disabling tracing due to warning"，不会破坏已有记录。`__warn()` 路径则每次真正打印警告时调用一次。

### 1.6 与 panic_on_warn 的交互

`__warn()` 的顺序是：

```
disable_trace_on_warning()
  → pr_warn("WARNING: ...")
  → print_modules() / show_regs()
  → check_panic_on_warn("kernel")   ← 这里可能 panic()
  → dump_stack()
```

因此在 `panic_on_warn=1` 的场景下，顺序是"**先冻结 trace，再 panic**"。这正好满足事后分析的需求：panic 触发的 ramoops/pstore 或 kdump 里，`trace` 缓冲区保存的是警告前的执行历史；如果反过来（先 panic 再关 trace），panic 打印本身会灌满缓冲区，把有价值的记录挤掉。

另外 `check_panic_on_warn()` 对 `warn_limit` 的处理是"达到上限才 panic"：

```c
void check_panic_on_warn(const char *origin)
{
	unsigned int limit;

	if (panic_on_warn)
		panic("%s: panic_on_warn set ...\n", origin);

	limit = READ_ONCE(warn_limit);
	if (atomic_inc_return(&warn_count) >= limit && limit)
		panic("%s: system warned too often (kernel.warn_limit is %d)",
		      origin, limit);
}
```

注意即使不 panic，`warn_count` 也会累加；`/sys/kernel/warn_count` 可读。`traceoff_on_warning` 与 `warn_limit` 是**独立**的两套机制：前者在第 1 次警告就关追踪，后者在第 limit 次警告时 panic。

### 1.7 `tracing_off()` 与 `tracer_tracing_off()`

`disable_trace_on_warning()` 调用了两种关闭函数：

```c
	trace_array_printk_buf(global_trace.array_buffer.buffer, _THIS_IP_,
		"Disabling tracing due to warning\n");
	tracing_off();          /* = tracer_tracing_off(&global_trace) */

	if (tr != &global_trace) {
		trace_array_printk_buf(tr->array_buffer.buffer, _THIS_IP_,
				       "Disabling tracing due to warning\n");
		tracer_tracing_off(tr);
	}
```

| 函数 | 行为 | 是否可嵌套 |
|--|--|--|
| `tracer_tracing_off(tr)` | `ring_buffer_record_off(buffer)` + `tr->buffer_disabled = 1` | 否（一次 `on` 就能恢复） |
| `tracer_tracing_disable(tr)` | `ring_buffer_record_disable(buffer)` | 是（计数器） |
| `tracing_off()` | 对 `global_trace` 调 `tracer_tracing_off()` | 否 |

`tr` 是 `READ_ONCE(printk_trace)`，即"当前负责 `trace_printk()` 输出的实例"。默认 `printk_trace == &global_trace`，只有把某个 instance 设为 printk 目标时才会不同：

```c
struct trace_array *printk_trace = &global_trace;

static void update_printk_trace(struct trace_array *tr)
{
	if (printk_trace == tr)
		return;

	printk_trace->trace_flags &= ~TRACE_ITER(TRACE_PRINTK);
	printk_trace = tr;
	tr->trace_flags |= TRACE_ITER(TRACE_PRINTK);
}
```

所以关闭逻辑要"关两次"：一次关全局实例（常规 ftrace 数据），一次关 `printk_trace`（`trace_printk` 数据）。二者可能是同一个实例，此时只关一次并只打印一行。

### 1.8 配置与代价

| 项 | 说明 |
|--|--|
| 编译开关 | 无独立 Kconfig，随 `CONFIG_TRACING`（`CONFIG_FTRACE`）一起编译 |
| 默认值 | `__disable_trace_on_warning = 0`，必须显式开启 |
| 启动参数 | `traceoff_on_warning`（`=0`/`=off` 表示不启用） |
| 运行时 | `/proc/sys/kernel/traceoff_on_warning`（0644，`proc_dointvec`） |
| sysctl 表位置 | `kernel/trace/trace.c` 的 `trace_sysctl_table[]`，不是 `kernel/sysctl.c` |
| 代价 | 关闭时仅一次分支判断；开启后每次 WARN 多一次 `ring_buffer_record_off` 与两条 `trace_array_printk_buf` |

注意 `trace_array_printk_buf()` 只在"缓冲区已分配"时有效；如果 tracing 从未启动，这两行日志不会出现在 trace 里，但 `tracing_off()` 仍会把 `buffer_disabled` 置 1（`tracer_tracing_off()` 对 `buffer` 为 NULL 的情况有判断）。

本树 `include/generated/autoconf.h` 中未定义 `CONFIG_FTRACE`/`CONFIG_TRACEPOINTS`，因此上述代码虽然存在于 `kernel/trace/trace.c`，在当前配置下**未参与编译**——本节属于"源码存在、当前配置未启用"。

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

### 2.5.1 输出结构是源码里写死的

`hist_debug_show()` 只是遍历触发器，真正的输出格式在 `hist_trigger_debug_show()` 里逐行 `seq_printf`：

```c
static void hist_trigger_debug_show(struct seq_file *m,
				    struct event_trigger_data *data, int n)
{
	struct hist_trigger_data *hist_data;
	int i, ret;

	if (n > 0)
		seq_puts(m, "\n\n");

	seq_puts(m, "# event histogram\n#\n# trigger info: ");
	data->cmd_ops->print(m, data);
	seq_puts(m, "#\n\n");

	hist_data = data->private_data;

	seq_printf(m, "hist_data: %p\n\n", hist_data);
	seq_printf(m, "  n_vals: %u\n", hist_data->n_vals);
	seq_printf(m, "  n_keys: %u\n", hist_data->n_keys);
	seq_printf(m, "  n_fields: %u\n", hist_data->n_fields);

	seq_puts(m, "\n  val fields:\n\n");
	seq_puts(m, "    hist_data->fields[0]:\n");
	ret = hist_field_debug_show(m, hist_data->fields[0],
				    HIST_FIELD_FL_HITCOUNT);
	...
	for (i = hist_data->n_vals; i < hist_data->n_fields; i++)
		hist_field_debug_show(m, hist_data->fields[i], HIST_FIELD_FL_KEY);
	...
	ret = hist_actions_debug_show(m, hist_data);
}
```

因此一个事件的 `hist_debug` 输出固定按以下顺序出现（示意，`%p` 受 `kptr_restrict` 影响可能被哈希）：

```
# event histogram
#
# trigger info: hist:sched_waking:hitcount=... [active]
#

hist_data: 00000000deadbeef

  n_vals: 2
  n_keys: 1
  n_fields: 3

  val fields:

    hist_data->fields[0]:
      flags:
        VAL: HIST_FIELD_FL_HITCOUNT
      type: u64
      size: 8
      is_signed: 0
      function: hist_field_<fn>()

    hist_data->fields[1]:
      flags:
        VAL: normal u64 value
      ftrace_event_field name: prio
      ...

  key fields:

    hist_data->fields[2]:
      flags:
        HIST_FIELD_FL_KEY
      ftrace_event_field name: comm
      ...
```

每个字段的细节由 `hist_field_debug_show()` 打印，包含：

| 输出项 | 来源 | 用途 |
|--|--|--|
| `flags:` | `hist_field_debug_show_flags()` | 区分 key / hitcount / var / var_ref / alias / const |
| `ftrace_event_field name` | `field->field->name` | 原始 tracepoint 字段名，核对是否解析错 |
| `var.name` / `var.idx` | `HIST_FIELD_FL_VAR` | 变量名与它在 `tracing_map_elt.vars[]` 中的下标 |
| `constant` | `HIST_FIELD_FL_CONST` | 常量表达式的值 |
| `var_ref_idx` | `HIST_FIELD_FL_ALIAS/VAR_REF` | 在 `hist_data->var_refs[]` 中的下标 |
| `system` / `event_name` | 跨事件引用 | `onmatch()` 等使用的目标事件 |
| `type` / `size` / `is_signed` | `hist_field` | 与 tracepoint 定义是否一致 |
| `function:` | `field_funcs[field->fn_num]` | 该字段实际调用的 `hist_field_*()` 处理函数，名字来自 `FIELD_FUNCS` 宏表（`nop`/`counter`/`u64`/`string`…） |

`val fields` 与 `key fields` 的分界由 `n_vals` 决定：`fields[0]` 固定是 hitcount，`fields[1..n_vals-1]` 是用户定义的聚合值，`fields[n_vals..n_fields-1]` 才是 key。

### 2.5.2 什么时候该看 hist_debug

| 现象 | `hist_debug` 里应重点看 |
|--|--|
| `hist` 文件里触发器存在，但直方图没有数据 | `trigger info` 是否 `[active]`、`key fields` 是否为空 |
| 聚合值恒为 0 | `val fields` 的 `function:` 是否是你期望的聚合函数 |
| `onchange()`/`onmatch()` 不触发 | `action tracking variables` 与 `match_data.event_system/event` 是否配对 |
| `save()` 的参数取不到 | `save action variables` 中 `field_vars[]`/`save_vars[]` 的下标 |
| 变量值串了 | `var.idx` 是否重复或越界（同一 event 的多个 trigger 共享 `tracing_map` 布局） |

`event_mutex` 在 `hist_debug_show()` 里用 `guard(mutex)(&event_mutex)` 持有，因此读这个文件时不会与"动态创建/删除 trigger"并发。它也要求先有 `CONFIG_HIST_TRIGGERS`，否则 `event_hist_debug_fops` 根本不会被引用。

### 2.5.3 文件是如何挂到事件目录的

`kernel/trace/trace_events.c` 的 `event_attr` 表里有一项专门的 `hist_debug`：

```c
#ifdef CONFIG_HIST_TRIGGERS_DEBUG
		{
			.name		= "hist_debug",
			.callback	= event_callback,
		},
#endif
```

`event_callback()` 在 `strcmp(name, "hist_debug") == 0` 时把 `*fops` 指向 `event_hist_debug_fops`：

```c
const struct file_operations event_hist_debug_fops = {
	.open = event_hist_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = tracing_single_release_file_tr,
};
```

`event_hist_debug_open()` 的关键是 `tracing_open_file_tr()`：它在打开时把当前 `trace_array` 绑定到文件，避免"读的过程中实例被销毁"的竞态；失败时还要 `tracing_release_file_tr()` 回滚。这也解释了为什么不能直接用 `single_open()`——必须先建立 trace_array 的引用。

### 2.5.4 本树配置现状

| 配置项 | 本树 | 后果 |
|--|--|--|
| `CONFIG_FTRACE` / `CONFIG_TRACING` | 未启用 | `kernel/trace/` 基本不参与编译 |
| `CONFIG_HIST_TRIGGERS` | 未启用 | 没有 `hist` 文件 |
| `CONFIG_HIST_TRIGGERS_DEBUG` | 未启用 | 没有 `hist_debug` 文件 |

所以在当前内核上 `cat /sys/kernel/tracing/events/.../hist_debug` 会得到 "No such file or directory"。这些代码的价值在于阅读实现与"按需打开配置"：只要打开 `CONFIG_HIST_TRIGGERS_DEBUG`（依赖 `HIST_TRIGGERS`），无需改代码即可获得完整的内部结构 dump。

### 2.5.5 与其它 debugfs 文件的分工

| 文件 | 面向 | 内容 |
|--|--|--|
| `hist` | 用户/脚本 | 人类可读的直方图结果、触发器增删 |
| `hist_debug` | 开发者 | 内部指针、数组下标、字段 flags |
| `trigger` | 用户 | 通用事件触发器列表 |
| `enable` | 用户 | 事件开关 |
| `filter` | 用户 | 事件过滤表达式 |

判断"是配置问题还是实现问题"的顺序：先看 `hist`（表达式是否被接受）、再看 `filter`（过滤是否把事件全挡掉）、最后看 `hist_debug`（字段是否解析到了预期位置）。

### 2.6 用途

1. **开发验证**：帮助开发者验证直方图触发器实现是否正确
2. **教育参考**：提供直方图触发器内部工作机制的详细信息
3. **问题排查**：当直方图行为不符合预期时，用于分析配置和数据结构

### 2.7 代码位置

- `kernel/trace/trace_events_hist.c`
- `kernel/trace/trace_events.c`