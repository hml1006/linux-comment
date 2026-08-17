# printk — 内核日志系统与动态调试

## 概述

printk 是 Linux 内核中最基础的日志输出函数，用于将调试信息、警告和错误消息输出到内核日志缓冲区。动态调试（Dynamic Debug）则是基于 printk 的扩展机制，允许在运行时动态开启/关闭特定文件、函数或行的 `pr_debug()`/`dev_dbg()` 打印，而无需重新编译内核。

## printk 架构设计

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         printk 架构                                    │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐ │
│  │   pr_* 宏        │     │   vprintk_emit   │     │   Ring Buffer    │ │
│  │  (pr_info等)     │────▶│   (格式化处理)   │────▶│   (日志缓冲区)   │ │
│  └──────────────────┘     └──────────────────┘     └──────────────────┘ │
│                                                          │              │
│                                                          ▼              │
│  ┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐ │
│  │   dmesg 命令     │◀────│   kmsg 接口      │◀────│   Console 驱动   │ │
│  │   (用户空间)     │     │   (/dev/kmsg)    │     │   (tty/serial)   │ │
│  └──────────────────┘     └──────────────────┘     └──────────────────┘ │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

## printk 核心数据结构

### console_printk 数组

```c
int console_printk[4] = {
    CONSOLE_LOGLEVEL_DEFAULT,   /* console_loglevel - 控制台输出级别 */
    MESSAGE_LOGLEVEL_DEFAULT,   /* default_message_loglevel - 默认消息级别 */
    CONSOLE_LOGLEVEL_MIN,       /* minimum_console_loglevel - 最小控制台级别 */
    CONSOLE_LOGLEVEL_DEFAULT,   /* default_console_loglevel - 默认控制台级别 */
};
```

### 日志级别

| 级别 | 宏 | 数值 | 说明 |
|------|-----|------|------|
| KERN_EMERG | `<0>` | 0 | 紧急情况，系统可能崩溃 |
| KERN_ALERT | `<1>` | 1 | 需要立即处理 |
| KERN_CRIT | `<2>` | 2 | 严重错误 |
| KERN_ERR | `<3>` | 3 | 错误 |
| KERN_WARNING | `<4>` | 4 | 警告 |
| KERN_NOTICE | `<5>` | 5 | 注意事项 |
| KERN_INFO | `<6>` | 6 | 一般信息 |
| KERN_DEBUG | `<7>` | 7 | 调试信息 |
| KERN_CONT | `<c>` | - | 续行消息 |

### struct console

```c
struct console {
    char            name[8];         /* 控制台名称 */
    void            (*write)(struct console *, const char *, unsigned);
    int             (*read)(struct console *, char *, unsigned);
    struct console  *(*device)(struct console *, int *);
    void            (*unblank)(void);
    int             (*setup)(struct console *, char *);
    int             (*exit)(struct console *);
    int             flags;           /* CON_* 标志 */
    int             index;           /* 控制台索引 */
    int             cflag;           /* tty 控制标志 */
    struct console  *next;           /* 下一个控制台 */
    struct module   *owner;          /* 所属模块 */
    struct console_cmdline *cmdline; /* 命令行配置 */
    char            *options;        /* 选项字符串 */
    void            *data;           /* 私有数据 */
    struct hlist_node node;          /* 哈希链表节点 */
    struct srcu_struct *srcu;        /* SRCU 保护 */
};
```

## printk 工作流程

### 核心函数调用链

```
pr_info("message")
       │
       ▼
printk(KERN_INFO "message")
       │
       ▼
vprintk_emit(facility, level, dev_info, fmt, args)
       │
       ├── 解析格式字符串
       ├── 添加时间戳和进程信息
       ├── 写入 ring buffer
       └── 唤醒控制台输出
               │
               ▼
console_unlock()
       │
       └── 遍历 console_list
               │
               ▼
       console->write()
               │
               ├── tty 控制台
               ├── serial 控制台
               ├── framebuffer 控制台
               └── ...
```

### vprintk_emit 函数

```c
asmlinkage __printf(4, 0)
int vprintk_emit(int facility, int level,
                 const struct dev_printk_info *dev_info,
                 const char *fmt, va_list args)
{
    /* 1. 获取当前上下文信息 */
    unsigned int caller_id = printk_get_caller_id();
    
    /* 2. 格式化消息 */
    va_copy(args_copy, args);
    len = vscnprintf(text_buf, sizeof(text_buf), fmt, args_copy);
    va_end(args_copy);
    
    /* 3. 添加前缀（时间戳、进程信息等） */
    prepend_prefix(text_buf, &level, &facility, caller_id);
    
    /* 4. 写入 ring buffer */
    trace_console(text_buf, len);
    ringbuffer_write(text_buf, len);
    
    /* 5. 触发控制台输出 */
    console_trylock();
    console_unlock();
    
    return len;
}
```

## pr_* 宏系列

### 基础宏

```c
#define pr_emerg(fmt, ...)  printk(KERN_EMERG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...)  printk(KERN_ALERT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...)   printk(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)    printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)   printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)   printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_cont(fmt, ...)   printk(KERN_CONT fmt, ##__VA_ARGS__)
```

### 条件宏

```c
/* 仅在 DEBUG 定义时输出 */
#ifdef DEBUG
#define pr_devel(fmt, ...)  printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#else
#define pr_devel(fmt, ...)  no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#endif

/* 动态调试（运行时可配置） */
#if defined(CONFIG_DYNAMIC_DEBUG) || \
    (defined(CONFIG_DYNAMIC_DEBUG_CORE) && defined(DYNAMIC_DEBUG_MODULE))
#define pr_debug(fmt, ...)  dynamic_pr_debug(fmt, ##__VA_ARGS__)
#elif defined(DEBUG)
#define pr_debug(fmt, ...)  printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...)  no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#endif
```

### 一次性和限速宏

```c
/* 一次性消息 */
#define pr_info_once(fmt, ...)      printk_once(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn_once(fmt, ...)      printk_once(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)

/* 限速消息 */
#define pr_info_ratelimited(fmt, ...)  printk_ratelimited(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn_ratelimited(fmt, ...)  printk_ratelimited(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
```

## 动态调试（Dynamic Debug）

### 概述

动态调试允许在运行时通过 `debugfs` 接口开启或关闭特定的 `pr_debug()`/`dev_dbg()` 调用，而无需重新编译内核。这对于调试特定模块或代码路径非常有用。

### 核心数据结构

#### struct _ddebug

```c
struct _ddebug {
    const char *modname;          /* 模块名称 */
    const char *function;        /* 函数名称 */
    const char *filename;        /* 文件名称 */
    const char *format;          /* 格式字符串 */
    unsigned int lineno:18;      /* 行号 */
    unsigned int class_id:CLS_BITS; /* 类别 ID */
    unsigned int flags:8;        /* 调试标志 */
#ifdef CONFIG_JUMP_LABEL
    union {
        struct static_key_true dd_key_true;
        struct static_key_false dd_key_false;
    } key;                       /* 静态分支键 */
#endif
} __attribute__((aligned(8)));
```

#### 调试标志

| 标志 | 位 | 说明 | 选项字符 |
|------|----|------|---------|
| `_DPRINTK_FLAGS_PRINT` | 0 | 启用打印 | 'p' |
| `_DPRINTK_FLAGS_INCL_MODNAME` | 1 | 包含模块名 | 'm' |
| `_DPRINTK_FLAGS_INCL_FUNCNAME` | 2 | 包含函数名 | 'f' |
| `_DPRINTK_FLAGS_INCL_LINENO` | 3 | 包含行号 | 'l' |
| `_DPRINTK_FLAGS_INCL_TID` | 4 | 包含线程 ID | 't' |
| `_DPRINTK_FLAGS_INCL_SOURCENAME` | 5 | 包含源文件名 | 's' |
| `_DPRINTK_FLAGS_INCL_STACK` | 6 | 包含堆栈跟踪 | 'd' |

### 动态调试工作原理

#### 编译时处理

当启用 `CONFIG_DYNAMIC_DEBUG` 时，每个 `pr_debug()` 调用会在 `__dyndbg` ELF 段中创建一个 `struct _ddebug` 实例：

```c
#define dynamic_pr_debug(fmt, ...) \
    _dynamic_func_call(fmt, __dynamic_pr_debug, pr_fmt(fmt), ##__VA_ARGS__)

#define _dynamic_func_call(fmt, func, ...) \
    __dynamic_func_call(__UNIQUE_ID(ddebug), fmt, func, ##__VA_ARGS__)

#define __dynamic_func_call(id, fmt, func, ...) \
    do { \
        DEFINE_DYNAMIC_DEBUG_METADATA(id, fmt); \
        if (DYNAMIC_DEBUG_BRANCH(id)) { \
            func(&id, ##__VA_ARGS__); \
            __dynamic_dump_stack(id); \
        } \
    } while (0)
```

#### 运行时检查

```c
#ifdef CONFIG_JUMP_LABEL
#define DYNAMIC_DEBUG_BRANCH(descriptor) \
    static_branch_unlikely(&descriptor.key.dd_key_false)
#else
#define DYNAMIC_DEBUG_BRANCH(descriptor) \
    unlikely(descriptor.flags & _DPRINTK_FLAGS_PRINT)
#endif
```

使用 `static_branch_unlikely()` 实现零开销检查，当调试未启用时，分支预测会跳过整个调试代码块。

### debugfs 接口

动态调试通过 `/sys/kernel/debug/dynamic_debug/` 目录提供用户接口：

```
/sys/kernel/debug/dynamic_debug/
├── control          # 配置接口（读写）
└── descriptors      # 列出所有调试描述符（只读）
```

### control 文件操作

#### 查询当前状态

```bash
cat /sys/kernel/debug/dynamic_debug/control
# 输出示例：
# init/main.c:pr_debug:24 [main] _ "early initcall %s\n"
# init/main.c:pr_debug:45 [main] _ "initcall %s\n"
```

#### 启用调试消息

```bash
# 启用特定文件的所有调试消息
echo -n 'file init/main.c +p' > /sys/kernel/debug/dynamic_debug/control

# 启用特定函数的调试消息
echo -n 'func do_initcall +p' > /sys/kernel/debug/dynamic_debug/control

# 启用特定模块的调试消息
echo -n 'module main +p' > /sys/kernel/debug/dynamic_debug/control

# 启用特定行范围的调试消息
echo -n 'line 100-200 +p' > /sys/kernel/debug/dynamic_debug/control

# 启用所有调试消息
echo -n '+p' > /sys/kernel/debug/dynamic_debug/control
```

#### 禁用调试消息

```bash
# 禁用特定文件的调试消息
echo -n 'file init/main.c -p' > /sys/kernel/debug/dynamic_debug/control

# 禁用所有调试消息
echo -n '-p' > /sys/kernel/debug/dynamic_debug/control
```

#### 添加额外信息

```bash
# 添加模块名和函数名
echo -n 'file drivers/pci/probe.c +pmf' > /sys/kernel/debug/dynamic_debug/control

# 添加行号和线程 ID
echo -n 'func pci_probe_device +plt' > /sys/kernel/debug/dynamic_debug/control

# 添加堆栈跟踪
echo -n 'module pci +pd' > /sys/kernel/debug/dynamic_debug/control
```

### 命令语法

```
[file <filename>] [func <function>] [module <modname>] [format <fmt>] [line <line-range>] [class <class>] [+/-flags]
```

| 参数 | 说明 |
|------|------|
| `file <filename>` | 匹配文件名（支持通配符） |
| `func <function>` | 匹配函数名（支持通配符） |
| `module <modname>` | 匹配模块名（支持通配符） |
| `format <fmt>` | 匹配格式字符串（支持通配符） |
| `line <line-range>` | 匹配行号范围 |
| `class <class>` | 匹配类别 |
| `+flags` | 添加标志 |
| `-flags` | 移除标志 |

### 调试类（Debug Classes）

动态调试支持按类别分组管理调试消息：

```c
DECLARE_DYNDBG_CLASSMAP(drm_dbg_classes, DD_CLASS_TYPE_DISJOINT_NAMES, 0,
    "DRM_DEBUG_CORE",
    "DRM_DEBUG_DRIVER",
    "DRM_DEBUG_KMS",
    "DRM_DEBUG_PRIME",
);
```

使用示例：

```bash
# 启用 DRM KMS 类的调试消息
echo -n 'class DRM_DEBUG_KMS +p' > /sys/kernel/debug/dynamic_debug/control

# 通过模块参数设置
modprobe drm dyndbg=class:DRM_DEBUG_KMS+pmf
```

## 设备调试宏

### dev_dbg 系列

```c
#ifdef CONFIG_DYNAMIC_DEBUG
#define dev_dbg(dev, fmt, ...)      dynamic_dev_dbg(dev, fmt, ##__VA_ARGS__)
#define dev_dbg_ratelimited(dev, fmt, ...) ...
#else
#define dev_dbg(dev, fmt, ...)      dev_no_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__)
#endif
```

### netdev_dbg 系列

```c
#ifdef CONFIG_DYNAMIC_DEBUG
#define netdev_dbg(dev, fmt, ...)   dynamic_netdev_dbg(dev, fmt, ##__VA_ARGS__)
#else
#define netdev_dbg(dev, fmt, ...)   netdev_no_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__)
#endif
```

## 内核启动参数

### printk 相关参数

```bash
# 设置控制台日志级别
console_loglevel=<level>

# 控制 /dev/kmsg 输出
printk.devkmsg=on|off|ratelimit

# 禁用所有 printk 输出
quiet

# 启用详细输出
loglevel=<level>

# 设置默认消息级别
message_loglevel=<level>
```

### 动态调试参数

```bash
# 通过内核启动参数启用调试
dyndbg="file init/main.c +p"

# 多个条件
dyndbg="file *.c +p; module drm +pmf"

# 模块加载时启用
modprobe mymodule dyndbg=+p
modprobe mymodule dyndbg="func myfunc +pmf"
```

## 编译配置

### printk 配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_PRINTK` | 启用 printk 支持 |
| `CONFIG_PRINTK_INDEX` | 启用 printk 索引 |
| `CONFIG_EARLY_PRINTK` | 启用早期 printk（启动阶段） |
| `CONFIG_EARLYCON` | 启用早期控制台 |
| `CONFIG_MESSAGE_LOGLEVEL_DEFAULT` | 默认消息级别 |
| `CONFIG_CONSOLE_LOGLEVEL_DEFAULT` | 默认控制台级别 |

### 动态调试配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_DYNAMIC_DEBUG` | 启用动态调试（内建） |
| `CONFIG_DYNAMIC_DEBUG_CORE` | 启用动态调试核心（模块支持） |
| `CONFIG_JUMP_LABEL` | 启用静态分支优化（推荐） |

## 性能影响

### printk 性能

- **未启用时**：`no_printk()` 仅进行编译时格式检查，运行时零开销
- **启用时**：取决于消息频率和输出设备速度
- **控制台输出**：最慢的部分，建议在生产环境限制日志级别

### 动态调试性能

- **未启用时**：通过 `static_branch_unlikely()` 实现，几乎零开销
- **启用时**：与普通 printk 相同
- **Jump Label 优化**：启用 `CONFIG_JUMP_LABEL` 后，分支预测几乎完美，未启用的调试代码不会被执行

## 使用示例

### 基础用法

```c
#include <linux/printk.h>

/* 一般信息 */
pr_info("Device %s initialized\n", dev_name(&my_dev));

/* 警告 */
pr_warn("Low memory, reducing buffer size\n");

/* 错误 */
pr_err("Failed to allocate memory: %d\n", ret);

/* 调试（动态可配置） */
pr_debug("Processing request from %s\n", current->comm);
```

### 设备驱动调试

```c
#include <linux/device.h>

struct my_device {
    struct device dev;
    int status;
};

void my_device_process(struct my_device *my_dev)
{
    /* 设备特定调试 */
    dev_dbg(&my_dev->dev, "Processing device %s, status=%d\n",
            dev_name(&my_dev->dev), my_dev->status);
    
    /* 带线程 ID 的调试 */
    if (my_dev->status < 0)
        dev_dbg(&my_dev->dev, "Status error, tid=%d\n", current->pid);
}
```

### 动态调试控制

```bash
# 启用 scsi 模块的所有调试消息
echo -n 'module scsi +p' > /sys/kernel/debug/dynamic_debug/control

# 启用特定文件的调试并添加函数名和行号
echo -n 'file drivers/scsi/scsi_scan.c +pf' > /sys/kernel/debug/dynamic_debug/control

# 禁用所有调试消息
echo -n '-p' > /sys/kernel/debug/dynamic_debug/control

# 查询特定文件的调试状态
grep 'scsi_scan.c' /sys/kernel/debug/dynamic_debug/control
```

## 代码位置

| 文件 | 说明 |
|------|------|
| `kernel/printk/printk.c` | printk 核心实现 |
| `kernel/printk/printk_ringbuffer.c` | ring buffer 实现 |
| `kernel/printk/console_cmdline.c` | 控制台命令行处理 |
| `include/linux/printk.h` | printk 头文件 |
| `include/linux/kern_levels.h` | 日志级别定义 |
| `lib/dynamic_debug.c` | 动态调试核心实现 |
| `include/linux/dynamic_debug.h` | 动态调试头文件 |