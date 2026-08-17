# mknodat 系统调用

## 1. 原理与功能

**原型：**
```c
int mknodat(int dirfd, const char *pathname, mode_t mode, dev_t dev);
```

**功能：** 创建特殊文件（设备节点、命名管道、套接字）或普通文件。

**参数：**
- `dirfd`: 目录 fd，`AT_FDCWD` 表示使用当前工作目录
- `pathname`: 要创建的文件路径
- `mode`: 文件类型和权限，由 `S_IFMT` 掩码和权限位组合
- `dev`: 设备号（仅对块设备 `S_IFBLK` 和字符设备 `S_IFCHR` 有效）

**mode 文件类型：**
| 类型 | 值 | 说明 |
|------|-----|------|
| `S_IFREG` | `0100000` | 普通文件 |
| `S_IFCHR` | `0020000` | 字符设备 |
| `S_IFBLK` | `0060000` | 块设备 |
| `S_IFIFO` | `0010000` | 命名管道（FIFO） |
| `S_IFSOCK` | `0140000` | 套接字 |

**权限要求：**
- 创建字符/块设备需要 `CAP_MKNOD` 权限
- 创建 FIFO/套接字不需要特殊权限（仅需父目录写权限）
- **注意：** `mknodat` 不用于创建目录（使用 `mkdirat`）

**ARM64 系统调用号：** `__NR_mknodat` (133)

## 2. 执行流程

```
                mknodat(dirfd, pathname, mode, dev)
                               |
                     +---------v----------+
                     | CLASS(filename)    |  拷贝 pathname
                     | getname()          |
                     +---------+----------+
                               |
                     +---------v----------+
                     | filename_mknodat() |
                     +---------+----------+
                               |
                     +---------v----------+
                     | may_mknod(mode)    |  检查 mode 合法性
                     | 只允许 S_IFREG/    |  不允许 S_IFDIR
                     | CHR/BLK/FIFO/SOCK  |
                     +---------+----------+
                               |
                     +---------v----------+
                     | filename_create()  |  查找父目录并创建 dentry
                     +---------+----------+
                               |
                     +---------v----------+
                     | security_path_     |  LSM 安全钩子
                     | mknod()            |
                     +---------+----------+
                               |
                     +---------v----------+
                     | 根据 mode 分派:    |
                     +---------+----------+
                               |
          +--------------------+---------------------+
          |                    |                     |
  +-------v--------+  +-------v--------+  +--------v-------+
  | S_IFREG / 0   |  | S_IFCHR /      |  | S_IFIFO /      |
  | vfs_create()  |  | S_IFBLK        |  | S_IFSOCK       |
  | 创建普通文件  |  | vfs_mknod()    |  | vfs_mknod()    |
  +-------+-------+  | 创建设备节点   |  | 创建 FIFO/套接字|
          |          +-------+--------+  +--------+--------+
          |                  |                     |
          +------------------+---------------------+
                             |
                     +-------v--------+
                     | end_creating_  |  释放创建锁
                     | path()         |
                     +----------------+
```

## 3. 函数调用栈

```
mknodat()  [fs/namei.c]
  └── filename_mknodat(dfd, name, mode, dev)  [fs/namei.c]
        ├── may_mknod(mode)                     // 检查 mode 是否合法
        │     └── 只允许 S_IFREG/S_IFCHR/S_IFBLK/S_IFIFO/S_IFSOCK
        ├── filename_create(dfd, name, &path, lookup_flags)  // 查找父目录
        │     ├── filename_lookup(dfd, name, LOOKUP_PARENT, ...)
        │     └── vfs_create(dentry, ...)  // 如果 dentry 不存在则创建
        ├── security_path_mknod(&path, dentry, mode, dev)  // LSM 检查
        ├── switch (mode & S_IFMT) {
        │     case 0:
        │     case S_IFREG:
        │         vfs_create(idmap, dentry, mode, &di)     // 创建普通文件
        │         └── dir->i_op->create(idmap, dir, dentry, mode, ...)
        │         break;
        │     case S_IFCHR:
        │     case S_IFBLK:
        │         vfs_mknod(idmap, dir, dentry, mode, new_decode_dev(dev), &di)
        │         └── may_create_dentry(...)  // 检查父目录写权限
        │         ├── capable(CAP_MKNOD)     // 需要 CAP_MKNOD 权限
        │         ├── devcgroup_inode_mknod()  // device cgroup 检查
        │         ├── security_inode_mknod()   // LSM 检查
        │         └── dir->i_op->mknod(...)    // 具体 FS 创建节点
        │         break;
        │     case S_IFIFO:
        │     case S_IFSOCK:
        │         vfs_mknod(idmap, dir, dentry, mode, 0, &di)  // dev=0
        │         └── dir->i_op->mknod(...)
        │         break;
        │ }
        └── end_creating_path(&path, dentry)  // 释放锁
```

## 4. 关键数据结构

```c
// 设备号编码
// 主设备号 (major): 标识设备驱动程序
// 次设备号 (minor): 标识具体设备实例
typedef unsigned int dev_t;

// 从用户态 dev_t 解码
static inline unsigned new_decode_dev(dev_t dev)
{
    unsigned major = (dev & 0xfff00) >> 8;  // 主设备号
    unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00); // 次设备号
    return MKDEV(major, minor);
}

// inode 操作表——mknod/create 方法
struct inode_operations {
    int (*create)(struct mnt_idmap *, struct inode *, struct dentry *,
                  umode_t, bool);
    int (*mknod)(struct mnt_idmap *, struct inode *, struct dentry *,
                 umode_t, dev_t);
    // ... 其他方法
};

// 文件类型掩码
#define S_IFMT   00170000   // 文件类型位掩码
#define S_IFSOCK 0140000    // 套接字
#define S_IFLNK  0120000    // 符号链接
#define S_IFREG  0100000    // 普通文件
#define S_IFBLK  0060000    // 块设备
#define S_IFDIR  0040000    // 目录
#define S_IFCHR  0020000    // 字符设备
#define S_IFIFO  0010000    // FIFO

// device cgroup 判断
int devcgroup_inode_mknod(mode_t mode, dev_t dev)
{
    // 检查设备的 cgroup 策略，允许/禁止创建设备节点
    // 在容器环境中尤为关键
}
```

## 5. 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    int ret;

    // === 示例1: 创建命名管道 (FIFO) ===
    // 更推荐使用 mkfifo() 或 mkfifoat()，但 mknodat 也可以
    ret = mknodat(AT_FDCWD, "/tmp/myfifo",
                  S_IFIFO | 0666, 0);
    printf("创建 FIFO: %s\n", ret == 0 ? "OK" : strerror(errno));

    // === 示例2: 创建字符设备节点（需要 root 或 CAP_MKNOD）===
    // 创建 /dev/null 的设备节点
    ret = mknodat(AT_FDCWD, "/tmp/null",
                  S_IFCHR | 0666, makedev(1, 3));
    printf("创建字符设备: %s\n", ret == 0 ? "OK" : strerror(errno));

    // === 示例3: 创建块设备节点（需要 root）===
    // 创建 /dev/sda 的设备节点
    ret = mknodat(AT_FDCWD, "/tmp/sda",
                  S_IFBLK | 0644, makedev(8, 0));
    printf("创建块设备: %s\n", ret == 0 ? "OK" : strerror(errno));

    // === 示例4: 使用目录 fd ===
    int dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    ret = mknodat(dirfd, "test_socket",
                  S_IFSOCK | 0644, 0);
    printf("创建套接字: %s\n", ret == 0 ? "OK" : strerror(errno));
    close(dirfd);

    // === 示例5: 权限检查——普通用户创建设备节点会失败 ===
    ret = mknodat(AT_FDCWD, "/tmp/test_dev",
                  S_IFCHR | 0644, makedev(1, 3));
    if (ret < 0) {
        printf("普通用户创建设备节点失败: %s\n", strerror(errno));
        // 预期: EPERM (Operation not permitted)
    }

    // 清理
    unlink("/tmp/myfifo");
    unlink("/tmp/null");
    unlink("/tmp/sda");
    unlink("/tmp/test_socket");

    return 0;
}
```

## 6. 设备节点的作用

### 6.1 本质

`mknodat` 创建的设备节点（device node）本质是一个**通往内核设备驱动的文件系统入口**。设备节点本身**不包含设备数据**，它只是一个"门牌号"——通过 inode 中存储的 `major:minor` 设备号，告诉内核应该把 `open/read/write/ioctl` 等文件操作转发给哪个设备驱动。

### 6.2 提供标准文件操作接口访问硬件

```c
// 用户态程序通过设备节点操作硬件，无需关心驱动细节
int fd = open("/dev/nvme0", O_RDWR);   // 打开 NVMe 控制器
ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd); // 发送 NVMe 管理命令
close(fd);
```

背后路径：
```
open("/dev/nvme0")
  → VFS 通过 inode.i_rdev = (major=240, minor=0)
  → chrdev_open() 查找 chrdevs[240][0] → cdev
  → 调用 nvme_dev_fops.open() → NVMe 驱动实际处理
```

### 6.3 三种典型设备节点

| 设备节点 | 示例 | 用途 |
|---------|------|------|
| **字符设备** | `/dev/null`, `/dev/tty`, `/dev/nvme0` | 流式读写，按字节访问 |
| **块设备** | `/dev/sda`, `/dev/nvme0n1` | 块级读写，支持挂载文件系统 |
| **特殊设备** | `/dev/random`, `/dev/zero` | 提供内核服务（随机数、零数据等） |

### 6.4 没有设备节点会怎样？

没有设备节点，用户态就无法通过文件路径访问设备驱动。但设备本身依然存在（驱动已加载到内核），只是缺少了文件系统的"入口"。

```bash
# 手动删除设备节点后，设备仍在工作，但无法通过路径访问
rm /dev/nvme0n1
# 内核中 NVMe 驱动依然正常运行，数据仍可读写
# 但用户态无法 open("/dev/nvme0n1") 了
# 用 mknod 可以重建：mknod /dev/nvme0n1 b 240 0
```

### 6.5 设备节点与驱动的桥梁作用

```
  ┌─────────────┐     open/read/write/ioctl      ┌──────────────┐
  │  用户态程序  │ ──────────────────────────────→ │  设备节点    │
  │  (进程)      │                                 │  /dev/nvme0  │
  └─────────────┘                                  └──────┬───────┘
                                                            │
                                               inode.i_rdev = (240, 0)
                                                            │
                                                            ▼
                                                     ┌──────────────┐
                                                     │  chrdev_open  │
                                                     │  通过设备号   │
                                                     │  查找 cdev    │
                                                     └──────┬───────┘
                                                            │
                                                            ▼
                                                     ┌──────────────┐
                                                     │  NVMe 驱动   │
                                                     │  nvme_dev_fops│
                                                     │  .open = ...  │
                                                     │  .ioctl = ... │
                                                     └──────────────┘
```

**总结：** `mknodat` 创建的设备节点是一个**文件系统层面的"快捷方式"**，它不涉及任何硬件 I/O，只是把设备号（major:minor）贴到一个文件路径上。真正驱动设备工作的，是内核中已经注册的设备驱动。设备节点是连接**用户态文件操作**和**内核驱动**的桥梁。

## 7. mknodat 与 NVMe 驱动设备创建对比

`mknodat` 创建设备节点与 NVMe 驱动创建设备是两种完全不同的机制，但最终结果都是让用户态可以通过 `/dev` 下的文件访问硬件设备。

### 7.1 总体对比

| 维度 | `mknodat` 系统调用 | NVMe 驱动设备创建 |
|------|-------------------|-------------------|
| 发起者 | 用户态进程（root） | 内核驱动（自动） |
| 触发时机 | 手动调用 | PCIe 设备探测时自动触发 |
| 本质操作 | 文件系统元数据操作 | 内核设备模型注册 |
| 存储位置 | 磁盘 inode（持久化） | 内核内存数据结构 |
| 设备号来源 | 用户指定（`makedev`） | 内核 `alloc_chrdev_region` 分配 |
| 设备节点创建 | `mknodat` 直接创建 | `devtmpfs`/`udev` 自动创建 |
| 数据流路径 | 不涉及硬件 I/O | 涉及 PCIe 配置空间读取、NVMe 命令交互 |
| 文件系统依赖 | 依赖（在 ext4 等 FS 上创建 inode） | 不依赖（`devtmpfs` 是内存文件系统） |

### 7.2 `mknodat` 创建字符设备节点流程

```
用户态: mknodat(AT_FDCWD, "/dev/mydevice", S_IFCHR | 0644, makedev(240, 0))
                                       │
                                       ▼
内核态: mknodat()  [fs/namei.c]
         │
         ▼
    filename_mknodat()
         │
         ├── may_mknod(mode)          // 检查 mode 合法
         ├── filename_create()         // 查找父目录 /dev，创建 dentry
         ├── vfs_mknod()              // 通用 VFS 层
         │    └── dir->i_op->mknod()  // → ext4_mknod()
         │         │
         │         ▼
         │    ext4_mknod()  [fs/ext4/namei.c]
         │         │
         │         ├── ext4_new_inode_start_handle()  // 分配新 inode
         │         ├── init_special_inode(inode, mode, rdev)
         │         │    └── inode->i_rdev = rdev       // ★ 存储设备号
         │         │    └── inode->i_fop = &def_chr_fops // 设置字符设备文件操作
         │         └── ext4_add_nondir()               // 添加目录项
         │              └── ext4_add_entry()            // 写入 ext4_dir_entry_2
         │                   └── 磁盘布局更新:
         │                       ├── inode 表: 新 inode(i_mode=S_IFCHR, i_rdev=240:0)
         │                       └── 目录块: ext4_dir_entry_2(name="mydevice", inode=#12345)
         │
         └── end_creating_path()      // 释放锁
```

**关键点：** `mknodat` 在磁盘上创建一个**持久化**的 inode，其中 `i_rdev` 字段记录设备号。打开设备文件时，VFS 通过 `i_rdev` 查找已注册的设备驱动。

### 7.3 NVMe 驱动设备创建流程

```
┌─────────────────────────────────────────────────────────────────┐
│ 模块初始化阶段: nvme_core_init()  [drivers/nvme/host/core.c]   │
│                                                                 │
│  alloc_chrdev_region(&nvme_ctrl_base_chr_devt, 0, NVME_MINORS,  │
│                      "nvme")          // 分配设备号范围         │
│  class_register(&nvme_class)          // 注册 "nvme" 设备类     │
│  alloc_chrdev_region(&nvme_ns_chr_devt, 0, NVME_MINORS,         │
│                      "nvme-generic")  // 分配 generic 设备号范围 │
│  class_register(&nvme_ns_chr_class)   // 注册 "nvme-generic" 类  │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ PCIe 探测阶段: nvme_probe()  [drivers/nvme/host/pci.c]          │
│                                                                 │
│  nvme_init_ctrl(ctrl, dev, &nvme_ctrl_ops, quirks)             │
│    └── 初始化 NVMe 控制器结构体，设置 admin queue               │
│                                                                 │
│  nvme_init_ctrl_finish(ctrl)                                   │
│    └── 发送 Identify Controller 命令，获取控制器信息            │
│                                                                 │
│  nvme_add_ctrl(ctrl)                     // ★ 创建控制器设备    │
│    └── ctrl->device->devt = MKDEV(MAJOR(nvme_ctrl_base_chr_devt),│
│                                    ctrl->instance)  // 分配设备号│
│    └── ctrl->device->class = &nvme_class                        │
│    └── cdev_init(&ctrl->cdev, &nvme_dev_fops)  // 初始化 cdev   │
│    └── cdev_device_add(&ctrl->cdev, ctrl->device)               │
│         │                                                       │
│         ├── cdev_add()        // 向内核注册字符设备              │
│         │    └── 将 cdev 加入 chrdevs 散列表                     │
│         │    └── 建立 设备号 → cdev 映射                        │
│         └── device_add()      // 添加到设备模型                  │
│              └── devtmpfs_create_node()  // devtmpfs 自动创建设备节点│
│              └── kobject_uevent()        // 通知 udev            │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 命名空间扫描阶段: nvme_scan_work()                              │
│                                                                 │
│  nvme_alloc_ns(ctrl, &info)              // ★ 创建命名空间设备  │
│    │                                                           │
│    ├── disk = blk_mq_alloc_disk(ctrl->tagset, &lim, ns)       │
│    │    └── 分配 gendisk，绑定 request_queue                    │
│    ├── disk->fops = &nvme_bdev_ops                             │
│    ├── sprintf(disk->disk_name, "nvme%dn%d", ...)             │
│    │                        // 如 "nvme0n1"                    │
│    ├── device_add_disk(ctrl->device, ns->disk, ...)            │
│    │    └── register_disk()                                    │
│    │    └── blk_register_region()  // 注册块设备设备号          │
│    │    └── devtmpfs_create_node()  // devtmpfs 创建 /dev/nvme0n1│
│    │                                                           │
│    └── nvme_add_ns_cdev(ns)           // ★ 创建 generic 字符设备│
│         └── nvme_cdev_add(&ns->cdev, &ns->cdev_device,         │
│                            &nvme_ns_chr_fops, THIS_MODULE)      │
│              ├── ida_alloc()  // 分配次设备号                    │
│              ├── device_initialize()                           │
│              ├── cdev_init(&ns->cdev, &nvme_ns_chr_fops)       │
│              └── cdev_device_add()                             │
│                   └── devtmpfs_create_node()  // /dev/ng0n1    │
└─────────────────────────────────────────────────────────────────┘
```

### 7.4 关键代码分析

#### NVMe 控制器字符设备注册

```c
// drivers/nvme/host/core.c
int nvme_add_ctrl(struct nvme_ctrl *ctrl)
{
    int ret;

    ret = ida_alloc(&nvme_instance_ida, GFP_KERNEL);
    if (ret < 0)
        return ret;
    ctrl->instance = ret;

    // 设置设备号：主设备号来自 alloc_chrdev_region，次设备号为 instance
    ctrl->device->devt = MKDEV(MAJOR(nvme_ctrl_base_chr_devt),
                               ctrl->instance);
    ctrl->device->class = &nvme_class;  // 设备类："nvme"

    // 初始化 cdev 并绑定 file_operations
    cdev_init(&ctrl->cdev, &nvme_dev_fops);
    ctrl->cdev.owner = ctrl->ops->module;

    // 注册到内核：cdev_add + device_add
    // devtmpfs 在此之后自动创建 /dev/nvme0
    ret = cdev_device_add(&ctrl->cdev, ctrl->device);
    if (ret) {
        // 失败处理
    }
    return 0;
}
```

#### NVMe 命名空间块设备注册

```c
// drivers/nvme/host/core.c
static void nvme_alloc_ns(struct nvme_ctrl *ctrl, struct nvme_ns_info *info)
{
    struct nvme_ns *ns;
    struct gendisk *disk;

    ns = kzalloc_node(sizeof(*ns), GFP_KERNEL, node);
    disk = blk_mq_alloc_disk(ctrl->tagset, &lim, ns);
    disk->fops = &nvme_bdev_ops;       // 块设备操作
    disk->private_data = ns;
    ns->disk = disk;

    // 设置磁盘名称 "nvme0n1"
    sprintf(disk->disk_name, "nvme%dn%d", ctrl->instance, ns->head->instance);

    // 注册块设备：生成 /dev/nvme0n1
    if (device_add_disk(ctrl->device, ns->disk, nvme_ns_attr_groups))
        goto out_cleanup_ns_from_list;

    // 注册 generic 字符设备：生成 /dev/ng0n1
    if (!nvme_ns_head_multipath(ns->head))
        nvme_add_ns_cdev(ns);
}
```

#### `cdev_device_add` 实现

```c
// drivers/nvme/host/core.c
int nvme_cdev_add(struct cdev *cdev, struct device *cdev_device,
                  const struct file_operations *fops, struct module *owner)
{
    int minor;

    // 动态分配次设备号
    minor = ida_alloc(&nvme_ns_chr_minor_ida, GFP_KERNEL);
    cdev_device->devt = MKDEV(MAJOR(nvme_ns_chr_devt), minor);
    cdev_device->class = &nvme_ns_chr_class;  // 设备类："nvme-generic"

    device_initialize(cdev_device);
    cdev_init(cdev, fops);
    cdev->owner = owner;

    // cdev_add + device_add 组合操作
    // 1. cdev_add: 将 cdev 加入 chrdevs 映射表
    // 2. device_add: 注册到设备模型，触发 devtmpfs 创建节点
    return cdev_device_add(cdev, cdev_device);
}
```

#### ext4 `mknod` 实现

```c
// fs/ext4/namei.c
static int ext4_mknod(struct mnt_idmap *idmap, struct inode *dir,
                      struct dentry *dentry, umode_t mode, dev_t rdev)
{
    handle_t *handle;
    struct inode *inode;
    int err;

    // 1. 分配新 inode（位图操作 + 磁盘 inode 表写入）
    inode = ext4_new_inode_start_handle(idmap, dir, mode, &dentry->d_name,
                                        0, NULL, EXT4_HT_DIR, credits);
    // 2. 初始化特殊 inode
    init_special_inode(inode, inode->i_mode, rdev);
    //    └── inode->i_rdev = rdev          // 存储设备号
    //    └── inode->i_fop = &def_chr_fops  // 字符设备文件操作

    // 3. 设置 inode 操作表（特殊文件无 read/write 操作）
    inode->i_op = &ext4_special_inode_operations;

    // 4. 添加目录项并写回磁盘
    err = ext4_add_nondir(handle, dentry, &inode);
    //    └── ext4_add_entry() → 写入 ext4_dir_entry_2 到目录块
    //    └── ext4_mark_inode_dirty() → 写回 inode 到磁盘 inode 表
}
```

### 7.5 设备号生命周期对比

```
mknodat 方式:
  用户指定设备号 → 写入磁盘 inode.i_rdev → 持久化存储
  （设备号长期保存在文件系统中，即使重启也还在）

NVMe 驱动方式:
  alloc_chrdev_region() 分配设备号 → cdev_add() 注册 → 存入内存
  （设备号由内核动态分配，重启后可能不同）
```

### 7.6 打开设备文件时的路径对比

```
mknodat 创建的文件:
  open("/dev/mydevice", ...)
    → path_openat() → ext4_lookup()  // 找到目录项中的 inode 号
    → ext4_iget()                    // 读取磁盘 inode，加载 i_rdev
    → init_special_inode()           // 设置 i_fop = &def_chr_fops
    → chrdev_open()                  // 通过 i_rdev 查找已注册的 cdev
    → cdev->ops->open()              // 调用设备驱动 open 方法

NVMe 驱动创建的设备文件 (/dev/nvme0n1):
  open("/dev/nvme0n1", ...)
    → path_openat() → devtmpfs 查找  // devtmpfs 是内存文件系统
    → 找到 dentry 中的 inode
    → inode.i_rdev 指向 NVMe 块设备号
    → blkdev_open()                  // 块设备通用 open
    → blkdev_get() → blkdev_get_by_dev()
    → 通过设备号找到 gendisk → nvme_bdev_ops.open()
```

### 7.7 总结

| 机制 | 本质 | 设备号来源 | 持久性 | 适用场景 |
|------|------|-----------|--------|---------|
| `mknodat` | 文件系统元数据操作 | 用户指定 | 磁盘持久化 | 手动创建固定设备节点 |
| NVMe 驱动 | 内核设备模型注册 | 内核动态分配 | 内存中（devtmpfs 自动重建） | 热插拔设备自动管理 |

**核心区别：** `mknodat` 是在文件系统中**创建一个指向设备号的目录项**，而 NVMe 驱动是在内核中**注册一个提供 I/O 能力的设备对象**。前者是"贴标签"，后者是"造工具"。

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件元数据与属性)
- [mkdirat.md](mkdirat.md)
- 内核源码: `fs/namei.c` `include/uapi/linux/stat.h`
- NVMe 驱动: `drivers/nvme/host/core.c` `drivers/nvme/host/nvme.h`