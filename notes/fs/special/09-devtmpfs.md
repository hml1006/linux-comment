# devtmpfs — 设备节点文件系统

## 1. 概述与实现机制

devtmpfs 是内核自动管理 `/dev` 目录下设备节点文件系统的机制。当内核检测到新设备时，自动在 devtmpfs 中创建设备文件，无需等待用户空间的 udev/mdev 响应。基于 **ramfs** 构建。

### 核心特性

- **自动创建**：设备注册时内核自动创建 `/dev/xxx` 节点
- **最小权限**：初始设备节点权限为 root:root, 0600
- **udev 配合**：devtmpfs 负责创建，udev 负责权限/命名
- **早期可用**：系统启动早期即可使用，无需用户空间

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  udevd (systemd-udevd) 监听 uevent 事件                      │
│  收到 uevent → 修改 devtmpfs 中的设备属性                    │
└────────────────────────┬─────────────────────────────────────┘
                         │ NETLINK uevent
                         ▼
┌──────────────────────────────────────────────────────────────┐
│               devtmpfs (drivers/base/devtmpfs.c)             │
│  devtmpfsd 内核线程 → 监听设备事件                          │
│  devtmpfs_create_node() → 创建设备节点                      │
│  devtmpfs_delete_node() → 删除设备节点                      │
│  devtmpfs_work() → 工作队列处理                             │
└──────────────────────┬───────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│               设备核心层 (drivers/base/core.c)               │
│  device_add() → devtmpfs_create_node()                      │
│  device_del() → devtmpfs_delete_node()                      │
│  kobject_uevent() → 发送 NETLINK 事件到用户空间             │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 devtmpfs 请求结构

```c
// 文件: drivers/base/devtmpfs.c
struct req {
    const char *name;           // 设备节点名称
    umode_t mode;               // 文件模式 (S_IFBLK/S_IFCHR + 权限)
    kuid_t uid;                 // 用户 ID
    kgid_t gid;                 // 组 ID
    struct device *dev;         // 关联的设备
};
```

### 2.2 devtmpfsd 内核线程状态

```c
// 文件: drivers/base/devtmpfs.c
static struct task_struct *thread;  // devtmpfsd 内核线程
static struct super_block *mnt_sb;  // devtmpfs 超级块
static int (*devtmpfs_mount)(void); // 挂载函数
```

---

## 3. API 与使用方法

### 3.1 核心 API

```c
#include <linux/device.h>

// 在 devtmpfs 中创建设备节点
int devtmpfs_create_node(struct device *dev);

// 从 devtmpfs 中删除设备节点
int devtmpfs_delete_node(struct device *dev);

// 初始化 devtmpfs
int __init init_devtmpfs(void);  // 系统启动时调用
```

### 3.2 使用示例

devtmpfs 对用户空间透明，无需手动调用。内核自动在设备注册时创建节点。

```bash
# 查看 devtmpfs 挂载
mount | grep devtmpfs
# devtmpfs on /dev type devtmpfs (rw,relatime,size=...)

# 设备节点自动创建 (无需手动 mknod)
ls -l /dev/sda
# brw-rw---- 1 root disk 8, 0 Jul 17 10:00 /dev/sda

# systemd-udevd 监听并设置权限
udevadm monitor  # 查看 uevent 和 udev 处理
```

```c
// 内核驱动注册时自动触发 devtmpfs 创建
// drivers/base/core.c
int device_add(struct device *dev)
{
    // ... 设备注册核心逻辑 ...
    
    // 创建 devtmpfs 节点
    if (devtmpfs_create_node(dev))
        dev_warn(dev, "devtmpfs_create_node failed\n");
    
    // 发送 uevent 到用户空间
    kobject_uevent(&dev->kobj, KOBJ_ADD);
    
    // ...
}
```

---

## 4. 函数调用栈

### 4.1 devtmpfs 初始化

```
start_kernel()
  → rest_init() → kernel_init()
    → kernel_init_freeable()
      → do_basic_setup()
        → driver_init()
          → devtmpfs_init()                     // drivers/base/devtmpfs.c
            → init_devtmpfs()                   // 初始化 devtmpfs
              → kern_mount(&devtmpfs_fs)        // 内核挂载 devtmpfs
                → devtmpfs_fs.type.init_fs_context()
                → devtmpfs_fill_super()         // 填充超级块
                  → ramfs_fill_super(sb, fc)    // 基于 ramfs
                → mnt_sb = sb                   // 保存超级块
              → thread = kthread_run(devtmpfsd, ...) // 启动 devtmpfsd 内核线程
                → 处理设备创建/删除请求
```

### 4.2 设备节点创建

```
device_add(dev)
  → devtmpfs_create_node(dev)                   // 创建 devtmpfs 节点
    → device_get_devnode(dev, &mode, &uid, &gid, &tmp) // 获取设备节点信息
      → dev->class->devnode(dev, &mode)         // 类提供的 devnode 回调
    → req.mode = mode | S_IFBLK/S_IFCHR         // 设置文件类型
    → req.name = device_get_devnode(...)         // 获取设备节点名
    → req.dev = dev                             // 关联设备
    → devtmpfs_submit_req(&req, tmp)             // 提交请求
      → complete(&req.done)                     // 等待完成
      → devtmpfs_work()                         // 工作队列处理
        → devtmpfsd()                           // 内核线程处理
          → handle(req)                          // 实际处理
            → device_create_file(mnt_sb, req)    // 创建文件节点
              → kernfs_create_file()             // 创建 kernfs 文件
              → kernfs_create_link()             // 创建符号链接
```

### 4.3 设备节点删除

```
device_del(dev)
  → devtmpfs_delete_node(dev)                   // 删除 devtmpfs 节点
    → devtmpfs_submit_req(&req, tmp)             // 提交删除请求
      → devtmpfs_work()
        → devtmpfsd()
          → handle(req)                          // 实际处理
            → kernfs_remove_by_name()            // 删除 kernfs 节点
```

---

## 5. 流程图

### 5.1 devtmpfs 与 udev 协作

```
内核驱动注册新设备
    │
    ▼
device_add()
    │
    ├── devtmpfs_create_node()              # 内核立即创建设备节点
    │     │
    │     ├── 获取设备信息 (name, mode, uid, gid)
    │     ├── 提交请求到 devtmpfsd 内核线程
    │     └── 创建 /dev/xxx 设备文件
    │           │
    │           ▼
    │     /dev/sda 创建完成 (权限 0600, root:root)
    │
    └── kobject_uevent(KOBJ_ADD)            # 发送 NETLINK uevent
          │
          ▼
    udevd (systemd-udevd) 收到 uevent
          │
          ├── 解析设备信息 (子系统、设备类型、属性)
          ├── 匹配 udev 规则
          │     ├── 设置文件权限 (如 disk:disk, 0660)
          │     ├── 创建符号链接 (/dev/disk/by-id/...)
          │     └── 执行自定义规则
          │
          └── 修改 devtmpfs 中已存在的文件属性
                │
                ▼
          /dev/sda 权限更新完成 (0660, root:disk)
```

### 5.2 启动时序

```
系统启动
    │
    ▼
start_kernel()
    │
    ▼
kernel_init() → do_basic_setup() → driver_init()
    │
    ├── init_devtmpfs()                    # 挂载 devtmpfs 到 /dev
    │     ├── kern_mount(devtmpfs_fs)      # 创建空 /dev 目录
    │     ├── 创建初始设备节点
    │     │     ├── /dev/console
    │     │     ├── /dev/null
    │     │     ├── /dev/zero
    │     │     └── /dev/kmsg
    │     └── 启动 devtmpfsd 内核线程
    │
    ├── 设备驱动初始化
    │     └── device_add() 触发 devtmpfs_create_node()
    │
    └── 启动 init 进程 (systemd)
          └── systemd-udevd 启动
                └── 接收 uevent → 设置权限/符号链接
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **系统启动早期** | 内核挂载 devtmpfs 到 /dev 提供基本设备节点 | `/dev/console`, `/dev/null` |
| **热插拔设备** | USB 设备插入时自动创建 /dev/sdb 等节点 | `ls /dev/sdb*` 即时可见 |
| **块设备发现** | 内核检测到磁盘时自动创建 /dev/sda 节点 | `lsblk` 可见所有设备 |
| **字符设备** | 输入设备、串口等字符设备节点管理 | `/dev/input/`, `/dev/tty` |
| **容器场景** | 容器内 /dev 由 devtmpfs 管理 | 容器内可访问设备节点 |
| **udev 配合** | devtmpfs + udev 完成完整设备管理 | 权限、命名、符号链接 |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `drivers/base/devtmpfs.c` | devtmpfs 核心实现（创建、删除、初始化） |
| `drivers/base/core.c` | 设备核心（device_add/device_del 调用 devtmpfs） |
| `drivers/base/base.h` | 内部结构定义 |
| `include/linux/device.h` | 设备结构体定义 |