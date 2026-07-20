# debugfs — 调试文件系统

## 概述

debugfs 是 Linux 内核提供的一个简单的调试文件系统，用于内核开发人员向用户空间暴露调试信息和控制接口。debugfs 提供了一种轻量级的方式来创建虚拟文件和目录，内核组件可以通过这些接口提供运行时状态信息、配置参数和调试控制。

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                      debugfs Architecture                         │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     用户空间 (User Space)                    │   │
│  │                                                             │   │
│  │  /sys/kernel/debug/                                         │   │
│  │  ├── kmemleak         → kmemleak 控制接口                   │   │
│  │  ├── kcsan/           → KCSAN 统计和配置                    │   │
│  │  ├── tracing/         → ftrace 追踪接口                     │   │
│  │  ├── lockdep          → lockdep 锁依赖信息                  │   │
│  │  ├── sched/           → 调度器调试信息                      │   │
│  │  ├── gpio/            → GPIO 调试信息                       │   │
│  │  └── ...              → 其他子系统接口                      │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                   VFS Layer                                 │   │
│  │                                                             │   │
│  │  • dentry 缓存                                              │   │
│  │  • inode 管理                                               │   │
│  │  • 文件操作转发                                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                   debugfs Core                              │   │
│  │                                                             │   │
│  │  ┌─────────────────────────────────────────────────────┐   │   │
│  │  │  inode.c - inode 和目录管理                           │   │   │
│  │  │  • debugfs_get_inode()                               │   │   │
│  │  │  • debugfs_create_dir()                              │   │   │
│  │  │  • debugfs_create_symlink()                          │   │   │
│  │  │  • debugfs_remove()                                  │   │   │
│  │  └─────────────────────────────────────────────────────┘   │   │
│  │                                                             │   │
│  │  ┌─────────────────────────────────────────────────────┐   │   │
│  │  │  file.c - 文件创建和操作                              │   │   │
│  │  │  • debugfs_create_file()                             │   │   │
│  │  │  • debugfs_create_u32()                              │   │   │
│  │  │  • debugfs_create_x8()                               │   │   │
│  │  │  • debugfs_create_blob()                             │   │   │
│  │  │  • debugfs_attr_read/write()                         │   │   │
│  │  └─────────────────────────────────────────────────────┘   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                   内核子系统 (Kernel Subsystems)             │   │
│  │                                                             │   │
│  │  • kmemleak_debugfs_init()                                 │   │
│  │  • kcsan_debugfs_init()                                   │   │
│  │  • ftrace_init_tracefs()                                   │   │
│  │  • lockdep_init()                                         │   │
│  │  • 各驱动模块的 debugfs_init()                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 核心数据结构

### debugfs_blob_wrapper

```
struct debugfs_blob_wrapper {
    void            *data;         /* 数据指针 */
    unsigned long   size;          /* 数据大小 */
};
```

### debugfs_reg32 / debugfs_regset32

```
struct debugfs_reg32 {
    char            *name;         /* 寄存器名称 */
    unsigned long   offset;        /* 寄存器偏移 */
};

struct debugfs_regset32 {
    const struct debugfs_reg32 *regs;  /* 寄存器数组 */
    int             nregs;         /* 寄存器数量 */
    void __iomem    *base;         /* 寄存器基地址 */
    struct device   *dev;          /* 设备指针（可选） */
};
```

### debugfs_short_fops

```
struct debugfs_short_fops {
    ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
    ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
    loff_t (*llseek)(struct file *, loff_t, int);
};
```

## 文件创建接口

### 创建普通文件

```c
struct dentry *debugfs_create_file(
    const char *name,              /* 文件名 */
    umode_t mode,                  /* 文件权限 */
    struct dentry *parent,         /* 父目录 */
    void *data,                    /* 私有数据 */
    const struct file_operations *fops  /* 文件操作 */
);
```

### 创建目录

```c
struct dentry *debugfs_create_dir(
    const char *name,              /* 目录名 */
    struct dentry *parent          /* 父目录 */
);
```

### 创建符号链接

```c
struct dentry *debugfs_create_symlink(
    const char *name,              /* 链接名 */
    struct dentry *parent,         /* 父目录 */
    const char *target             /* 目标路径 */
);
```

### 创建简单属性文件

```c
struct dentry *debugfs_create_u32(
    const char *name,              /* 文件名 */
    umode_t mode,                  /* 文件权限 */
    struct dentry *parent,         /* 父目录 */
    u32 *value                    /* 值指针 */
);

struct dentry *debugfs_create_x8(
    const char *name,              /* 文件名 */
    umode_t mode,                  /* 文件权限 */
    struct dentry *parent,         /* 父目录 */
    u8 *value                     /* 值指针 */
);

struct dentry *debugfs_create_blob(
    const char *name,              /* 文件名 */
    umode_t mode,                  /* 文件权限 */
    struct dentry *parent,         /* 父目录 */
    struct debugfs_blob_wrapper *blob  /* 数据包装器 */
);
```

### 创建属性文件（自定义 get/set）

```c
#define DEFINE_DEBUGFS_ATTRIBUTE(__fops, __get, __set, __fmt)
```

示例：

```c
static int my_var_get(void *data, u64 *val)
{
    *val = *(int *)data;
    return 0;
}

static int my_var_set(void *data, u64 val)
{
    *(int *)data = val;
    return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(my_var_fops, my_var_get, my_var_set, "%llu\n");

debugfs_create_file("my_var", 0644, parent, &my_var, &my_var_fops);
```

## 文件操作实现

### seq_file 方式

```c
static int my_debug_show(struct seq_file *s, void *v)
{
    seq_printf(s, "Value: %d\n", my_value);
    seq_printf(s, "Status: %s\n", my_status ? "enabled" : "disabled");
    return 0;
}

static int my_debug_open(struct inode *inode, struct file *file)
{
    return single_open(file, my_debug_show, NULL);
}

static const struct file_operations my_debug_fops = {
    .open = my_debug_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

debugfs_create_file("my_debug", 0444, parent, NULL, &my_debug_fops);
```

### 简单读写方式

```c
static ssize_t my_file_read(struct file *file, char __user *buf,
                            size_t count, loff_t *ppos)
{
    char tmp[64];
    int len;
    
    len = snprintf(tmp, sizeof(tmp), "%d\n", my_data);
    return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static ssize_t my_file_write(struct file *file, const char __user *buf,
                             size_t count, loff_t *ppos)
{
    char tmp[64];
    int val;
    
    if (count >= sizeof(tmp))
        return -EINVAL;
    
    if (copy_from_user(tmp, buf, count))
        return -EFAULT;
    
    tmp[count] = '\0';
    if (kstrtoint(tmp, 10, &val))
        return -EINVAL;
    
    my_data = val;
    return count;
}

static const struct file_operations my_file_fops = {
    .read = my_file_read,
    .write = my_file_write,
    .open = simple_open,
    .llseek = noop_llseek,
};

debugfs_create_file("my_file", 0644, parent, NULL, &my_file_fops);
```

## 目录结构

```
/sys/kernel/debug/
├── kmemleak                    # kmemleak 主接口
├── kmemleak_enabled            # kmemleak 启用状态
├── kmemleak_max_age            # 对象最小年龄
├── kmemleak_scan_sleep         # 扫描间隔
├── kcsan/                      # KCSAN 目录
│   ├── enabled                 # 启用状态
│   ├── counters                # 统计信息
│   ├── suppressions            # 抑制规则
│   └── weak_memory             # 弱内存模型
├── tracing/                    # ftrace 目录
│   ├── trace                   # 追踪输出
│   ├── trace_pipe              # 管道输出
│   ├── events/                 # 事件目录
│   ├── kprobe_events           # kprobe 事件
│   ├── uprobe_events           # uprobe 事件
│   ├── set_ftrace_filter       # 函数过滤
│   ├── set_graph_function      # 调用图函数
│   └── ...                     # 其他接口
├── lockdep                     # lockdep 依赖图
├── lockstat                    # 锁统计信息
├── sched/                      # 调度器调试
│   ├── debug                   # 调度器调试信息
│   └── latency_stats           # 延迟统计
├── gpio/                       # GPIO 调试
├── pwm/                        # PWM 调试
├── clk/                        # 时钟调试
├── regulator/                  # 电源管理调试
├── i2c/                        # I2C 调试
└── ...                         # 其他子系统
```

## 使用示例

### 查看 kmemleak 报告

```bash
cat /sys/kernel/debug/kmemleak
```

### 触发 kmemleak 扫描

```bash
echo scan > /sys/kernel/debug/kmemleak
```

### 查看 ftrace 输出

```bash
cat /sys/kernel/debug/tracing/trace
```

### 设置 ftrace 过滤

```bash
echo my_function > /sys/kernel/debug/tracing/set_ftrace_filter
echo function > /sys/kernel/debug/tracing/current_tracer
```

### 查看 KCSAN 统计

```bash
cat /sys/kernel/debug/kcsan/counters
```

## 编译配置

| 配置项 | 说明 |
|--------|------|
| CONFIG_DEBUG_FS | 启用 debugfs |
| CONFIG_DEBUG_FS_ALLOW_ALL | 默认允许所有访问 |

## 挂载方式

```bash
# 自动挂载（推荐）
mount -t debugfs none /sys/kernel/debug

# 或在 fstab 中添加
none /sys/kernel/debug debugfs defaults 0 0
```

## 与其他文件系统的对比

| 特性 | /proc | /sys | debugfs |
|------|-------|------|---------|
| 用途 | 进程信息 | 设备/驱动 | 调试信息 |
| 生命周期 | 进程相关 | 设备相关 | 运行时 |
| 稳定性 | 稳定 | 稳定 | 不稳定 |
| API | 固定 | 严格 | 灵活 |
| 性能 | 中等 | 中等 | 高效 |

## 最佳实践

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Best Practices                              │
│                                                                     │
│  1. 使用 debugfs 而非 /proc 或 /sys:                                │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • 调试信息属于 debugfs 的范畴                               │   │
│  │  • /proc 用于进程信息，/sys 用于设备属性                     │   │
│  │  • debugfs 提供更灵活的接口                                  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  2. 权限控制:                                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • 只读文件使用 0444 或 0644                                │   │
│  │  • 可写文件使用 0644 或 0600                                │   │
│  │  • 敏感信息使用 0600 或更高权限                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  3. 资源清理:                                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • 模块卸载时调用 debugfs_remove_recursive()               │   │
│  │  • 使用 debugfs_remove() 删除单个文件/目录                  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  4. 性能考虑:                                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • 避免在 read 回调中进行复杂计算                           │   │
│  │  • 使用 seq_file 处理大量数据                              │   │
│  │  • 考虑使用 debugfs_blob_wrapper 处理静态数据               │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 总结

debugfs 是内核调试的重要基础设施：

1. **轻量级**：简单的 API，易于使用
2. **灵活**：支持各种类型的文件和目录
3. **标准化**：统一的接口规范
4. **隔离性**：调试信息与系统核心功能分离
5. **动态**：支持运行时创建和删除

debugfs 为内核开发人员提供了一个方便的方式来暴露调试信息，是内核开发和调试的必备工具。