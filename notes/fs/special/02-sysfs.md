# sysfs — 内核对象文件系统

## 1. 概述与实现机制

sysfs 将内核对象模型（kobject）层次结构以文件系统形式暴露在 `/sys` 目录下。每个 **kobject** 对应 `/sys/` 下的一个目录，每个 **attribute**（属性）对应一个文件。基于 **kernfs** 框架构建。

### 核心特性

- **kobject 映射**：内核对象树 → 文件系统目录树
- **属性暴露**：kobject 的属性通过 attribute 文件读写
- **热插拔**：设备插拔时自动更新目录结构
- **统一视图**：设备、驱动、总线、类等统一以文件系统呈现

### 实现架构

```
┌─────────────────────────────────────────────────────────────┐
│                    用户空间                                  │
│  cat /sys/block/nvme0n1/size    |    echo 0 > /sys/...     │
└────────────────────────┬────────────────────────────────────┘
                         │ VFS 系统调用
                         ▼
┌─────────────────────────────────────────────────────────────┐
│               sysfs 层 (fs/sysfs/)                          │
│  sysfs_file_ops → sysfs_kf_seq_show() / sysfs_kf_write()   │
│  sysfs_create_dir() / sysfs_create_file()                   │
│  sysfs_add_file_mode_ns() → 创建 kernfs 节点               │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│               kernfs 框架层 (fs/kernfs/)                    │
│  kernfs_create_root() → kernfs_create_dir()                │
│  kernfs_create_file() → kernfs_get_inode()                 │
│  kernfs_ops → seq_show() / write() 回调                    │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│               kobject 层 (lib/kobject.c)                    │
│  kobject_add() → kobject_add_internal()                    │
│    → create_dir() → sysfs_create_dir()                     │
│  kobject_uevent() → 发送热插拔事件                         │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 kobject — 内核对象

```c
// 文件: include/linux/kobject.h
struct kobject {
    const char          *name;          // 对象名称（目录名）
    struct list_head    entry;          // 在 kset 中的链表节点
    struct kobject      *parent;        // 父 kobject
    struct kset         *kset;          // 所属的 kset
    const struct kobj_type *ktype;      // 对象类型（包含 attribute 信息）
    struct kernfs_node  *sd;            // 对应的 kernfs 节点（sysfs 目录）
    struct kref         kref;           // 引用计数
    unsigned int        state_initialized:1;  // 是否已初始化
    unsigned int        state_in_sysfs:1;     // 是否已在 sysfs 中注册
    unsigned int        state_add_uevent_sent:1; // 是否已发送 ADD uevent
    unsigned int        state_remove_uevent_sent:1; // 是否已发送 REMOVE uevent
    unsigned int        uevent_suppress:1;  // 是否抑制 uevent
};
```

### 2.2 kset — kobject 集合

```c
// 文件: include/linux/kobject.h
struct kset {
    struct list_head    list;           // 包含的 kobject 链表
    spinlock_t          list_lock;      // 链表锁
    struct kobject      kobj;           // 自身的 kobject
    const struct kset_uevent_ops *uevent_ops; // uevent 操作函数
};
```

### 2.3 kobj_type — kobject 类型

```c
// 文件: include/linux/kobject.h
struct kobj_type {
    void (*release)(struct kobject *kobj);  // 释放回调
    const struct sysfs_ops *sysfs_ops;      // sysfs 操作函数
    const struct attribute_group **default_groups; // 默认属性组
    const struct kobj_ns_type_operations *(*child_ns_type)(struct kobject *kobj);
    const void *(*namespace)(struct kobject *kobj);
    void (*child_sysfs_direntry)(struct kobject *kobj, struct kernfs_node *kn);
};
```

### 2.4 attribute — 属性描述

```c
// 文件: include/linux/sysfs.h
struct attribute {
    const char          *name;          // 属性名（文件名）
    umode_t             mode;           // 文件权限
};
```

### 2.5 sysfs_ops — sysfs 操作

```c
// 文件: include/linux/sysfs.h
struct sysfs_ops {
    ssize_t (*show)(struct kobject *, struct attribute *, char *);  // 读回调
    ssize_t (*store)(struct kobject *, struct attribute *, const char *, size_t); // 写回调
};
```

### 2.6 device_attribute — 设备属性

```c
// 文件: include/linux/device.h
struct device_attribute {
    struct attribute    attr;           // 属性描述
    ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
    ssize_t (*store)(struct device *dev, struct device_attribute *attr,
                     const char *buf, size_t count);
};
```

### 2.7 kernfs_node — kernfs 节点

```c
// 文件: include/linux/kernfs.h
struct kernfs_node {
    const char          *name;          // 节点名称
    struct rb_node      rb;             // 红黑树节点（父目录子节点）
    const void          *ns;            // 命名空间指针
    unsigned int        hash;           // 名称哈希
    struct kernfs_node  *parent;        // 父节点
    union {
        struct kernfs_elem_dir  *dir;   // 目录信息
        struct kernfs_elem_symlink *symlink; // 符号链接信息
        struct kernfs_elem_attr *attr;  // 属性文件信息
    };
    void                *priv;          // 私有数据（指向 kobject）
    struct kernfs_root  *root;          // 所属 kernfs 根
    struct kernfs_node  *dep;           // 依赖节点
    struct kobject      *kobj;          // 关联的 kobject
    atomic_t            count;          // 引用计数
    unsigned int        flags;          // 标志位
    umode_t             mode;           // 文件类型和权限
    struct kernfs_iattrs *iattr;        // inode 属性
};
```

---

## 3. API 与使用方法

### 3.1 核心 API

```c
#include <linux/kobject.h>
#include <linux/sysfs.h>

// 创建/初始化 kobject
int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
                         struct kobject *parent, const char *fmt, ...);
void kobject_del(struct kobject *kobj);
void kobject_put(struct kobject *kobj);

// 创建 sysfs 属性文件
int sysfs_create_file(struct kobject *kobj, const struct attribute *attr);
int sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp);
int sysfs_create_groups(struct kobject *kobj, const struct attribute_group **groups);
void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr);
void sysfs_remove_group(struct kobject *kobj, const struct attribute_group *grp);
void sysfs_remove_groups(struct kobject *kobj, const struct attribute_group **groups);

// 创建二进制属性文件
int sysfs_create_bin_file(struct kobject *kobj,
                          const struct bin_attribute *attr);
void sysfs_remove_bin_file(struct kobject *kobj,
                           const struct bin_attribute *attr);

// 创建符号链接
int sysfs_create_link(struct kobject *kobj, struct kobject *target,
                      const char *name);
void sysfs_remove_link(struct kobject *kobj, const char *name);
```

### 3.2 使用示例

```c
// 示例: 驱动创建 sysfs 属性

// 定义 show/store 回调
static ssize_t my_value_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    struct my_drv *drv = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%d\n", drv->value);
}

static ssize_t my_value_store(struct device *dev,
                              struct device_attribute *attr,
                              const char *buf, size_t count)
{
    struct my_drv *drv = dev_get_drvdata(dev);
    int ret = kstrtoint(buf, 0, &drv->value);
    if (ret < 0)
        return ret;
    return count;
}

// 定义属性
static DEVICE_ATTR(my_value, 0644, my_value_show, my_value_store);

// 在驱动 probe 中创建
static int my_drv_probe(struct platform_device *pdev)
{
    struct my_drv *drv;
    // ...
    device_create_file(&pdev->dev, &dev_attr_my_value);
    return 0;
}

// 在驱动 remove 中移除
static int my_drv_remove(struct platform_device *pdev)
{
    device_remove_file(&pdev->dev, &dev_attr_my_value);
    return 0;
}
```

```c
// 示例: 创建自定义 kobject 和属性组

static struct attribute *my_attrs[] = {
    &dev_attr_my_value.attr,
    &dev_attr_my_status.attr,
    NULL,  // 必须以 NULL 结尾
};

static const struct attribute_group my_attr_group = {
    .name = "my_subdir",  // 若为 NULL，属性直接创建在 kobject 目录下
    .attrs = my_attrs,
};

// 注册属性组
sysfs_create_group(&my_kobj, &my_attr_group);
```

---

## 4. 函数调用栈

### 4.1 sysfs 初始化

```
start_kernel()
  → vfs_caches_init() → mnt_init()
    → sysfs_init()                          // fs/sysfs/mount.c
      → kernfs_create_root(NULL, KERNFS_ROOT_EXTRA_OPEN_PERM_CHECK, NULL)
          → 创建 sysfs 根 kernfs 节点
      → sysfs_root_kn = kernfs_root_to_node(sysfs_root)
      → register_filesystem(&sysfs_fs_type)  // 注册 sysfs 文件系统类型
```

### 4.2 挂载流程

```
mount -t sysfs none /sys
  ↓ sys_mount() → do_mount() → do_new_mount()
    ↓
    vfs_get_tree(&sysfs_fs_type)
      → sysfs_init_fs_context(fc)            // 初始化文件系统上下文
          → fc->ops = &sysfs_context_ops
      → sysfs_fill_super(sb, fc)             // 填充超级块
          → sb->s_op = &sysfs_super_ops
          → sb->s_root = kernfs_get_inode(sb, sysfs_root_kn)
              → 创建根 inode
              → d_make_root(inode)           // 创建根 dentry
```

### 4.3 kobject 创建与 sysfs 目录

```
kobject_add(kobj, parent, "name")
  → kobject_add_varg(kobj, parent, fmt, vargs)
    → kobject_add_internal(kobj)             // 核心添加逻辑
      → create_dir(kobj)                     // 创建 sysfs 目录
        → sysfs_create_dir_ns(kobj, ...)     // 创建 sysfs 目录
          → kernfs_create_dir_ns(parent->sd, name, mode, ...)
            → kernfs_add_one(kn)             // 添加到 kernfs 树
              → kernfs_link_sibling(kn)      // 链接到红黑树
      → kobject_uevent(kobj, KOBJ_ADD)       // 发送热插拔事件

// 创建默认属性文件
kobject_add_internal()
  → populate_default_groups(kobj)            // 创建默认属性组
    → sysfs_create_groups(kobj, ktype->default_groups)
      → sysfs_group_add_file()               // 逐个创建属性文件
        → sysfs_add_file_mode_ns(kobj->sd, ...)
          → kernfs_create_file_ns(parent_kn, name, mode, ...)
            → kernfs_add_one(kn)             // 添加到 kernfs 树
```

### 4.4 读取属性文件

```
cat /sys/block/nvme0n1/size
  ↓ sys_read() → vfs_read() → file->f_op->read_iter()
    ↓
    kernfs_file_read_iter()                  // kernfs 通用读取
      → kernfs_seq_start() → kernfs_seq_show()
        → kernfs_ops->seq_show()             // 调用 sysfs_ops->show()
          → sysfs_kf_seq_show()              // sysfs 的 seq_show 回调
            → kobj->ktype->sysfs_ops->show(kobj, attr, buf)
              → device_attr->show(dev, attr, buf)  // 设备属性 show 回调
                → (如 nvme 的 size_show())
                  → return sysfs_emit(buf, "%llu\n", dev_size);
```

---

## 5. 流程图

### 5.1 sysfs 目录结构树

```
/sys/
├── block/                     # 块设备 (通过 kobject 创建设备目录)
│   └── nvme0n1/
│       ├── queue/             # IO 队列参数
│       ├── size               # 设备大小
│       ├── stat               # I/O 统计
│       └── ...
├── bus/                       # 总线
│   ├── pci/                   # PCI 总线
│   │   ├── devices/           # PCI 设备
│   │   │   └── 0000:00:1f.2/ # 具体设备的属性
│   │   └── drivers/           # PCI 驱动
│   │       └── nvme/          # 驱动的属性
│   └── usb/                   # USB 总线
├── class/                     # 设备类
│   ├── net/                   # 网络设备
│   ├── input/                 # 输入设备
│   └── tty/                   # 终端设备
├── dev/                       # 设备号 (主:次 → 设备)
│   ├── block/                 # 块设备号映射
│   └── char/                  # 字符设备号映射
├── devices/                   # 设备树 (物理拓扑)
│   └── platform/              # platform 设备
│   └── pci0000:00/            # PCI 设备树
│       └── 0000:00:1f.2/      # 具体 PCI 设备
├── firmware/                  # 固件信息
│   ├── acpi/                  # ACPI 表
│   └── devicetree/            # DT 设备树
├── fs/                        # 文件系统信息
├── kernel/                    # 内核参数
├── module/                    # 内核模块
└── power/                     # 电源管理
```

### 5.2 kobject 创建与 sysfs 注册流程

```
驱动 probe() 或设备注册
       │
       ▼
  device_initialize(&dev->dev)       // 初始化设备 kobject
       │
       ▼
  device_add(&dev->dev)              // 添加设备
       │
       ├── kobject_add(&dev->dev.kobj, parent, "name")
       │     │
       │     ├── create_dir()        // 创建 sysfs 目录
       │     │     └── sysfs_create_dir_ns()
       │     │           └── kernfs_create_dir_ns()
       │     │                 └── kernfs_add_one() → 链接到红黑树
       │     │
       │     └── populate_default_groups()
       │           └── sysfs_create_groups()
       │                 └── sysfs_add_file_mode_ns()
       │                       └── kernfs_create_file_ns()
       │                             └── kernfs_add_one()
       │
       ├── device_create_file()       // 创建设备额外属性
       │     └── sysfs_create_file()
       │
       ├── device_add_class_symlinks() // 创建 class 符号链接
       │
       └── kobject_uevent(&dev->dev.kobj, KOBJ_ADD)
             └── 发送 NETLINK uevent 到用户空间
```

---

## 6. 使用场景

| 场景 | 描述 | 示例路径 |
|------|------|----------|
| **设备发现** | 查看系统所有设备及其属性 | `/sys/devices/`, `/sys/block/` |
| **驱动参数** | 运行时调整驱动参数 | `/sys/module/nvme/parameters/` |
| **电源管理** | 控制设备电源状态 | `/sys/power/state`, `/sys/devices/.../power/control` |
| **IO 调度** | 调整块设备 IO 调度器 | `/sys/block/sda/queue/scheduler` |
| **网络配置** | 查看/修改网络设备参数 | `/sys/class/net/eth0/` |
| **固件信息** | 查看 ACPI/DT 固件信息 | `/sys/firmware/acpi/`, `/sys/firmware/devicetree/` |
| **LED 控制** | 控制 LED 触发模式 | `/sys/class/leds/.../trigger` |
| **CPU 调频** | 调整 CPU 频率和调度策略 | `/sys/devices/system/cpu/cpu0/cpufreq/` |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `fs/sysfs/mount.c` | sysfs 文件系统类型、挂载、初始化 |
| `fs/sysfs/file.c` | sysfs 文件读写操作实现 |
| `fs/sysfs/dir.c` | sysfs 目录操作实现 |
| `fs/sysfs/symlink.c` | sysfs 符号链接实现 |
| `fs/sysfs/group.c` | sysfs 属性组创建/删除 |
| `lib/kobject.c` | kobject 核心实现 |
| `include/linux/kobject.h` | kobject/kset 数据结构定义 |
| `include/linux/sysfs.h` | sysfs API 和数据结构定义 |
| `include/linux/device.h` | device_attribute 定义 |
| `fs/kernfs/` | kernfs 框架（sysfs 的底层实现） |