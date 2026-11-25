# debugfs — 调试文件系统

## 1. 概述与实现机制

debugfs 是一个简单的内存文件系统，为内核开发者提供快速创建调试接口的途径。API 简单，适合临时/调试用途，**不适合作为稳定的用户空间 ABI**。

### 核心特性

- **简单 API**：一行代码即可创建调试文件
- **动态创建**：运行时按需创建/删除调试接口
- **类型支持**：直接支持 u8/u16/u32/u64/bool/x32 等原子类型
- **条件编译**：依赖 `CONFIG_DEBUG_FS` 内核配置选项

### 实现架构

```
┌─────────────────────────────────────────────────────┐
│                    用户空间                          │
│  cat /sys/kernel/debug/xxx   |   echo val > ...    │
└────────────────────────┬────────────────────────────┘
                         │ VFS 系统调用
                         ▼
┌─────────────────────────────────────────────────────┐
│               debugfs 层 (fs/debugfs/)              │
│  debugfs_create_file() / debugfs_create_dir()      │
│  debugfs_create_u32() / debugfs_create_bool()      │
│  debugfs_attr_read() / debugfs_attr_write()         │
└────────────────────────┬────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│               底层实现 (simple_fs 风格)              │
│  debugfs_get_inode() → 分配 inode                   │
│  d_make_persistent() → 关联 dentry 和 inode         │
│  file_operations → debugfs_full_proxy_ops           │
└─────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 debugfs_fsdata — debugfs 文件私有数据

```c
// 文件: fs/debugfs/internal.h
struct debugfs_fsdata {
    const struct file_operations *real_fops;  // 实际文件操作函数
    refcount_t                    active_users; // 活跃用户计数
    struct completion             active_users_drained; // 完成量
};
```

### 2.2 debugfs_blob_wrapper — 二进制 blob 包装器

```c
// 文件: include/linux/debugfs.h
struct debugfs_blob_wrapper {
    void *data;              // 指向二进制数据
    unsigned long size;      // 数据大小
};
```

### 2.3 debugfs_regset32 — 32 位寄存器集合

```c
// 文件: include/linux/debugfs.h
struct debugfs_regset32 {
    const struct debugfs_reg32 *regs;  // 寄存器定义数组
    int nregs;                        // 寄存器数量
    void __iomem *base;               // 寄存器基地址
};
```

---

## 3. API 与使用方法

### 3.1 核心 API

```c
#include <linux/debugfs.h>

// 创建/获取 debugfs 根目录
struct dentry *debugfs_create_dir(const char *name, struct dentry *parent);

// 创建自定义文件 (需提供 file_operations)
struct dentry *debugfs_create_file(const char *name, umode_t mode,
                                   struct dentry *parent, void *data,
                                   const struct file_operations *fops);

// 创建原子类型文件 (自动处理读写)
struct dentry *debugfs_create_u8(const char *name, umode_t mode,
                                 struct dentry *parent, u8 *value);
struct dentry *debugfs_create_u16(const char *name, umode_t mode,
                                  struct dentry *parent, u16 *value);
struct dentry *debugfs_create_u32(const char *name, umode_t mode,
                                  struct dentry *parent, u32 *value);
struct dentry *debugfs_create_u64(const char *name, umode_t mode,
                                  struct dentry *parent, u64 *value);
struct dentry *debugfs_create_bool(const char *name, umode_t mode,
                                   struct dentry *parent, bool *value);
struct dentry *debugfs_create_x8(const char *name, umode_t mode,
                                 struct dentry *parent, u8 *value);
struct dentry *debugfs_create_x16(const char *name, umode_t mode,
                                  struct dentry *parent, u16 *value);
struct dentry *debugfs_create_x32(const char *name, umode_t mode,
                                  struct dentry *parent, u32 *value);
struct dentry *debugfs_create_x64(const char *name, umode_t mode,
                                  struct dentry *parent, u64 *value);
struct dentry *debugfs_create_size_t(const char *name, umode_t mode,
                                     struct dentry *parent, size_t *value);

// 创建二进制 blob 文件
struct dentry *debugfs_create_blob(const char *name, umode_t mode,
                                   struct dentry *parent,
                                   struct debugfs_blob_wrapper *blob);

// 创建寄存器转储文件
struct dentry *debugfs_create_regset32(const char *name, umode_t mode,
                                       struct dentry *parent,
                                       struct debugfs_regset32 *regset);

// 创建设备树文件
struct dentry *debugfs_create_devm_seqfile(struct device *dev,
                                           const char *name,
                                           struct dentry *parent,
                                           int (*show)(struct seq_file *, void *));

// 创建符号链接
struct dentry *debugfs_create_symlink(const char *name,
                                      struct dentry *parent,
                                      const char *target);

// 删除文件/目录
void debugfs_remove(struct dentry *dentry);
void debugfs_remove_recursive(struct dentry *dentry);
```

### 3.2 使用示例

```c
// 示例1: 简单原子类型文件
static struct dentry *debug_dir;
static u32 my_counter;
static bool my_enable;

static int __init my_init(void)
{
    debug_dir = debugfs_create_dir("my_driver", NULL);

    debugfs_create_u32("counter", 0644, debug_dir, &my_counter);
    debugfs_create_bool("enable", 0644, debug_dir, &my_enable);
    debugfs_create_x32("reg_status", 0444, debug_dir, &hw_status);

    return 0;
}

static void __exit my_exit(void)
{
    debugfs_remove_recursive(debug_dir);
}
```

```c
// 示例2: 自定义 file_operations 文件
static int my_show(struct seq_file *m, void *v)
{
    seq_printf(m, "Driver state:\n");
    seq_printf(m, "  counter: %u\n", my_counter);
    seq_printf(m, "  enabled: %s\n", my_enable ? "yes" : "no");
    seq_printf(m, "  temperature: %d\n", read_hw_temperature());
    return 0;
}

static int my_open(struct inode *inode, struct file *file)
{
    return single_open(file, my_show, inode->i_private);
}

static const struct file_operations my_fops = {
    .owner   = THIS_MODULE,
    .open    = my_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

static int __init my_init(void)
{
    debugfs_create_file("status", 0444, debug_dir, NULL, &my_fops);
    return 0;
}
```

```c
// 示例3: 二进制 blob 导出
static struct debugfs_blob_wrapper my_blob = {
    .data = sensor_data_buffer,
    .size = sizeof(sensor_data_buffer),
};

debugfs_create_blob("sensor_data", 0444, debug_dir, &my_blob);
```

```c
// 示例4: 寄存器转储
static struct debugfs_reg32 my_regs[] = {
    { .name = "CTRL",  .offset = 0x00 },
    { .name = "STATUS", .offset = 0x04 },
    { .name = "DATA",   .offset = 0x08 },
    { .name = "IRQ",    .offset = 0x0C },
};

static struct debugfs_regset32 my_regset = {
    .regs  = my_regs,
    .nregs = ARRAY_SIZE(my_regs),
    .base  = ioremap(0x12340000, 0x1000),
};

debugfs_create_regset32("registers", 0444, debug_dir, &my_regset);
```

---

## 4. 函数调用栈

### 4.1 debugfs 创建目录

```
debugfs_create_dir("my_dir", parent)
  → debugfs_start_creating(name, parent)      // 准备创建
    → debugfs_automount(?)                     // 检查自动挂载
    → simple_pin_fs(&debug_fs_type, ...)       // 固定文件系统
    → lookup_one_len(name, parent)             // 查找 dentry
  → debugfs_get_inode(dentry->d_sb)           // 创建 inode
    → new_inode(sb)
    → inode->i_mode = S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO
    → inode->i_op = &debugfs_dir_inode_operations
    → inode->i_fop = &simple_dir_operations
  → d_make_persistent(dentry, inode)           // 关联 dentry 和 inode
  → debugfs_end_creating(dentry)               // 结束创建
    → d_instantiate(dentry, inode)             // 实例化 dentry
```

### 4.2 debugfs 创建原子类型文件

```
debugfs_create_u32("counter", 0644, parent, &value)
  → debugfs_create_mode_unsafe(name, mode, parent, value,
                               &fops_u32_ro, &fops_u32_wo, &fops_u32_rw)
    → debugfs_create_file(name, mode, parent, value, &fops_u32_rw)
      → debugfs_start_creating(name, parent)
      → debugfs_get_inode(dentry->d_sb)       // 创建 inode
        → inode->i_mode = mode | S_IFREG
        → inode->i_fop = &debugfs_full_proxy_ops  // 代理操作
        → inode->i_private = &fsdata          // 存储实际 fops
      → d_make_persistent(dentry, inode)
      → debugfs_end_creating(dentry)

// 读取 u32 时:
// read → debugfs_full_proxy_ops → debugfs_u32_get()
//   → debugfs_u32_get() 从 value 指针读取
//   → snprintf(buf, PAGE_SIZE, "%u\n", *val)
```

---

## 5. 流程图

### 5.1 debugfs 读取流程

```
用户读文件                           debugfs 内核
    │                                   │
    │  read(fd, buf, count)              │
    │──────────────────────────────────►│
    │                                   │
    │  VFS → file->f_op->read_iter()    │
    │       (debugfs_full_proxy_ops)     │
    │                                   │
    │  debugfs_full_proxy_read()        │
    │    → 检查文件权限                  │
    │    → 调用 real_fops->read()        │
    │      │                            │
    │      ├── 原子类型: debugfs_u32_get │
    │      │     → snprintf(buf, "%u")  │
    │      │                            │
    │      ├── seq_file: seq_read()     │
    │      │     → show() 回调生成内容  │
    │      │                            │
    │      └── blob: debugfs_read_blob()│
    │            → simple_read_from_buffer│
    │                                   │
    │  ◄── 返回数据到用户空间           │
    │                                   │
```

### 5.2 debugfs 使用模式

```
┌─────────────────────────────────────────────────────────────┐
│                  内核模块/驱动                               │
│                                                             │
│  init:                                                      │
│    debugfs_create_dir("my_drv", NULL)                       │
│      └── /sys/kernel/debug/my_drv/                          │
│            ├── counter      (u32, 可读写)                   │
│            ├── enable       (bool, 可读写)                  │
│            ├── reg_status   (x32, 只读)                     │
│            ├── status       (seq_file, 只读, 多行状态)      │
│            ├── sensor_data  (blob, 只读, 二进制数据)        │
│            └── registers    (regset32, 只读, 寄存器转储)    │
│                                                             │
│  exit:                                                      │
│    debugfs_remove_recursive(my_drv_dir)                     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **驱动调试** | 暴露驱动内部寄存器、状态、计数器 | 寄存器转储、温度传感器值 |
| **性能调优** | 运行时调整驱动参数，无需重新编译 | 调整缓冲区大小、中断合并阈值 |
| **内核状态检查** | 查看内核内部数据结构状态 | slab 分配器状态、内存节点信息 |
| **跟踪/日志** | 输出调试跟踪信息 | 函数调用计数、事件序列记录 |
| **硬件测试** | 直接控制硬件引脚/寄存器 | GPIO 电平控制、PWM 占空比调整 |
| **开发阶段** | 快速原型验证，后期可移除 | 新功能临时调试接口 |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `fs/debugfs/inode.c` | debugfs 核心实现（目录/文件创建） |
| `fs/debugfs/file.c` | debugfs 原子类型文件创建 API |
| `fs/debugfs/internal.h` | 内部数据结构定义 |
| `include/linux/debugfs.h` | 对外 API 头文件 |