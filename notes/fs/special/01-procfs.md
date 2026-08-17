# procfs — 进程文件系统

## 1. 概述与实现机制

procfs 是一个虚拟文件系统（pseudo filesystem），以文件形式暴露内核和进程信息。内容在读取时动态生成，不占用磁盘空间。基于 **kernfs** 框架构建。

### 核心特性

- **动态生成**：读取时调用内核回调函数生成内容，而非从磁盘读取
- **进程信息**：每个进程对应 `/proc/[pid]/` 目录
- **内核参数**：通过 `/proc/sys/` 暴露，可通过 sysctl 修改
- **硬件信息**：通过 `/proc/cpuinfo`, `/proc/meminfo` 等暴露

### 实现架构

```
┌─────────────────────────────────────────────────────┐
│                  用户空间                           │
│  cat /proc/meminfo  |  ls /proc/  |  sysctl 调用   │
└──────────────────────────┬──────────────────────────┘
                           │ VFS 系统调用
                           ▼
┌─────────────────────────────────────────────────────┐
│                  VFS 层                              │
│  proc_reg_read() → proc_reg_open() → proc_reg_ioctl()│
└──────────────────────────┬──────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────┐
│               procfs 核心层                          │
│  proc_fill_super() → proc_get_inode()               │
│  proc_create_single() / proc_create_data()          │
│  /proc/[pid] 通过 proc_pid_readdir() 动态枚举       │
└──────────────────────────┬──────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────┐
│               kernfs 框架层                          │
│  kernfs_create_root() → kernfs_create_dir()         │
│  kernfs_create_file() → kernfs_get_inode()          │
│  提供 sysfs_ops → show()/store() 回调机制           │
└─────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 proc_dir_entry — proc 目录项

```c
// 文件: fs/proc/internal.h
struct proc_dir_entry {
    struct hlist_node  sibling;        // 兄弟节点链表
    struct hlist_node  subdir_node;    // 子目录节点
    struct hlist_head  subdir;         // 子目录哈希表
    struct rb_root     subdir_rb;      // 子目录红黑树 (快速查找)
    struct proc_dir_entry *parent;     // 父目录项

    union {
        const struct proc_ops *proc_ops;  // proc 操作函数表
        const struct file_operations *fops; // 文件操作 (旧版)
    };

    struct inode_operations *ops;      // inode 操作 (目录/文件)
    struct kobject          *kobj;     // 关联的 kobject

    nlink_t nlink;                     // 硬链接计数
    umode_t mode;                      // 文件类型和权限
    loff_t size;                       // 文件大小 (部分文件使用)
    struct proc_dir_entry *data;       // 私有数据指针
    refcount_t refcnt;                 // 引用计数
    u8 namelen;                        // 名称长度
    char name[];                       // 文件名 (灵活数组)
};
```

### 2.2 proc_inode — proc inode 扩展

```c
// 文件: fs/proc/internal.h
struct proc_inode {
    struct pid            *pid;        // 关联的进程 PID 结构
    struct proc_dir_entry *pde;        // 对应的 proc 目录项
    struct ctl_table_header *sysctl;   // sysctl 表头 (用于 /proc/sys)
    struct ctl_table      *sysctl_entry; // sysctl 表项
    struct inode           vfs_inode;  // 嵌入的 VFS inode
};
```

### 2.3 proc_fs_info — proc 文件系统实例信息

```c
// 文件: fs/proc/internal.h
struct proc_fs_info {
    struct pid_namespace *pid_ns;       // 关联的 PID 命名空间
    struct proc_dir_entry *proc_self;   // /proc/self 目录项
    struct proc_dir_entry *proc_thread_self; // /proc/thread-self 目录项
    struct dentry *proc_mnt;            // 挂载根 dentry
    struct list_head mount_list;        // 挂载列表
};
```

### 2.4 proc_ops — proc 操作函数表

```c
// 文件: include/linux/proc_fs.h
struct proc_ops {
    unsigned int    proc_flags;         // 标志位
    int     (*proc_open)(struct inode *, struct file *);  // 打开文件
    ssize_t (*proc_read)(struct file *, char __user *, size_t, loff_t *); // 读
    ssize_t (*proc_write)(struct file *, const char __user *, size_t, loff_t *); // 写
    loff_t  (*proc_lseek)(struct file *, loff_t, int);   // 定位
    int     (*proc_release)(struct inode *, struct file *); // 释放
    __poll_t (*proc_poll)(struct file *, struct poll_table_struct *); // 轮询
    int     (*proc_ioctl)(struct inode *, struct file *, unsigned int, unsigned long); // IOCTL
    int     (*proc_mmap)(struct file *, struct vm_area_struct *); // 内存映射
};
```

---

## 3. API 与使用方法

### 3.1 创建 proc 条目

```c
#include <linux/proc_fs.h>

// 创建目录
struct proc_dir_entry *proc_mkdir(const char *name, struct proc_dir_entry *parent);

// 创建 proc 文件 (使用 proc_ops)
struct proc_dir_entry *proc_create(const char *name, umode_t mode,
                                   struct proc_dir_entry *parent,
                                   const struct proc_ops *proc_ops);

// 创建单输出文件 (open → single_open → show 回调)
struct proc_dir_entry *proc_create_single(const char *name, umode_t mode,
                                          struct proc_dir_entry *parent,
                                          int (*show)(struct seq_file *, void *));

// 创建带数据的单输出文件
struct proc_dir_entry *proc_create_single_data(const char *name, umode_t mode,
                                               struct proc_dir_entry *parent,
                                               int (*show)(struct seq_file *, void *),
                                               void *data);

// 创建 seq_file 接口文件
struct proc_dir_entry *proc_create_seq(const char *name, umode_t mode,
                                       struct proc_dir_entry *parent,
                                       const struct seq_operations *ops);

// 创建带数据的 seq_file 接口文件
struct proc_dir_entry *proc_create_seq_data(const char *name, umode_t mode,
                                            struct proc_dir_entry *parent,
                                            const struct seq_operations *ops,
                                            void *data);

// 创建统计文件 (seq_file + 统计接口)
struct proc_dir_entry *proc_create_stats(const char *name, umode_t mode,
                                         struct proc_dir_entry *parent,
                                         const struct seq_operations *ops,
                                         void *data);

// 移除 proc 条目
void remove_proc_entry(const char *name, struct proc_dir_entry *parent);
void remove_proc_subtree(const char *name, struct proc_dir_entry *parent);
```

### 3.2 使用示例

```c
// 示例1: 创建简单的 proc 文件 (single_open)
static int my_proc_show(struct seq_file *m, void *v)
{
    seq_printf(m, "My counter: %d\n", my_counter);
    seq_printf(m, "My status: %s\n", my_status ? "active" : "inactive");
    return 0;
}

static int my_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, my_proc_show, NULL);
}

static const struct proc_ops my_proc_fops = {
    .proc_open    = my_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static int __init my_init(void)
{
    proc_create("my_driver_info", 0444, NULL, &my_proc_fops);
    return 0;
}

static void __exit my_exit(void)
{
    remove_proc_entry("my_driver_info", NULL);
}
```

```c
// 示例2: 创建可写的 proc 文件
static ssize_t my_proc_write(struct file *file, const char __user *buffer,
                             size_t count, loff_t *pos)
{
    char buf[32];
    if (count > sizeof(buf) - 1)
        return -EINVAL;
    if (copy_from_user(buf, buffer, count))
        return -EFAULT;
    buf[count] = '\0';

    if (strncmp(buf, "enable", 6) == 0)
        my_enabled = true;
    else if (strncmp(buf, "disable", 7) == 0)
        my_enabled = false;
    return count;
}
```

---

## 4. 函数调用栈

### 4.1 挂载流程

```
mount -t proc none /proc
  ↓ sys_mount() → do_mount() → path_mount() → do_new_mount()
    ↓
    vfs_get_tree(&proc_fs_type)
      → proc_init_fs_context()          // 初始化文件系统上下文
          → proc_fs_context = kzalloc()
          → fc->fs_private = proc_fs_context
          → fc->ops = &proc_context_ops
      → proc_fill_super(sb, fc)         // 填充超级块
          → kzalloc_obj(proc_fs_info)   // 分配 fs_info
          → get_pid_ns(ctx->pid_ns)     // 获取 PID 命名空间
          → sb->s_op = &proc_sops       // 设置超级块操作
          → proc_get_inode(sb, &proc_root)  // 创建根 inode
          → d_make_root(root_inode)     // 创建根 dentry
          → proc_setup_self(sb)         // 创建 /proc/self
          → proc_setup_thread_self(sb)  // 创建 /proc/thread-self
    ↓
    do_add_mount() → 将挂载加入挂载树
```

### 4.2 读取流程 (以 /proc/meminfo 为例)

```
cat /proc/meminfo
  ↓ sys_read() → vfs_read() → file->f_op->read()
    ↓
    proc_reg_read()                      // proc 通用读取入口
      → proc_reg_open()                  // 打开文件（第一次读取时）
        → proc_meminfo_open()            // proc_meminfo 的 open 回调
          → single_open(file, proc_meminfo_show, NULL)  // 初始化 seq_file
            → seq_open(file, &single_read_op)           // 设置 seq_file
    ↓
    → seq_read()                         // seq_file 读取
      → single_start() → single_next() → single_show()
        → proc_meminfo_show()            // 生成 meminfo 内容
          → si_meminfo(&val)             // 获取内存统计
          → si_swapinfo(&val)            // 获取交换统计
          → seq_printf(m, "MemTotal: ...")  // 格式化输出
          → seq_printf(m, "MemFree: ...")
          → seq_printf(m, "Buffers: ...")
          → ...
    ↓
    → copy_to_user(buf, seq_buf, count)  // 拷贝到用户空间
```

### 4.3 进程目录枚举

```
ls /proc/
  ↓ sys_getdents64() → iterate_dir() → file->f_op->iterate()
    ↓
    proc_root_readdir()                  // 枚举 /proc 根目录
      → proc_pid_readdir()               // 枚举所有进程 PID
        → for_each_process(task)         // 遍历所有进程
          → proc_pid_make_inode()        // 为每个进程创建 inode
          → proc_fill_cache()            // 填充 dentry 缓存
            → proc_fill_common()         // 填充 proc_dir_entry
      → proc_root_readdir_sub()          // 枚举非进程条目
        → proc_pde_readdir()             // 遍历 proc_dir_entry 子树
```

---

## 5. 流程图

### 5.1 procfs 架构总览

```
┌─────────────────────────────────────────────────────────────────────┐
│                        /proc 文件系统                              │
│                                                                     │
│  ┌─────────────────────┐         ┌──────────────────────────────┐  │
│  │   进程信息目录       │         │   系统信息文件               │  │
│  │                     │         │                              │  │
│  │  /proc/1/           │         │  /proc/cpuinfo               │  │
│  │  /proc/1234/        │         │  /proc/meminfo               │  │
│  │  /proc/self/        │         │  /proc/uptime                │  │
│  │  /proc/thread-self/ │         │  /proc/version               │  │
│  └─────────┬───────────┘         │  /proc/diskstats             │  │
│            │                     │  /proc/stat                  │  │
│            ▼                     │  /proc/loadavg               │  │
│  ┌─────────────────────┐         │  /proc/mounts                │  │
│  │  proc_pid_readdir() │         │  /proc/partitions            │  │
│  │  → 枚举所有进程     │         │  /proc/filesystems           │  │
│  │  → 创建 PID 目录    │         └──────────┬───────────────────┘  │
│  └─────────────────────┘                    │                      │
│                                             ▼                      │
│  ┌─────────────────────┐         ┌──────────────────────────────┐  │
│  │  每个 /proc/[pid]/  │         │  /proc/sys/ (sysctl)        │  │
│  │  cmdline, cwd, fd   │         │  sysctl 系统参数            │  │
│  │  maps, stat, ...    │         │  /proc/sys/net/...          │  │
│  └─────────────────────┘         │  /proc/sys/kernel/...       │  │
│                                  │  /proc/sys/vm/...           │  │
│  ┌─────────────────────┐         └──────────────────────────────┘  │
│  │  /proc/net/         │                                           │
│  │  网络协议栈统计信息 │                                           │
│  └─────────────────────┘                                           │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.2 读取流程时序

```
用户空间                  VFS                  procfs                 内核
   │                      │                     │                      │
   │  read(fd, buf, cnt)  │                     │                      │
   │─────────────────────►│                     │                      │
   │                      │  proc_reg_read()    │                      │
   │                      │────────────────────►│                      │
   │                      │                     │  首次打开？          │
   │                      │                     │  ──► proc_reg_open() │
   │                      │                     │      │               │
   │                      │                     │      ▼               │
   │                      │                     │  single_open()       │
   │                      │                     │  (设置 seq_file)     │
   │                      │                     │                      │
   │                      │                     │  seq_read()          │
   │                      │                     │  → single_show()     │
   │                      │                     │    → proc_xxx_show() │
   │                      │                     │                      │
   │                      │                     │    si_meminfo() ─────│──►
   │                      │                     │    si_swapinfo() ────│──►
   │                      │                     │    seq_printf() ◄───│───
   │                      │                     │                      │
   │  ◄───────────────────│─────────────────────│                      │
   │  (返回内存信息)      │                     │                      │
   │                      │                     │                      │
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **系统监控** | 获取 CPU/内存/磁盘/网络使用情况 | `cat /proc/meminfo`, `cat /proc/stat` |
| **进程管理** | 查看进程状态、内存映射、打开文件 | `ls /proc/1234/fd/`, `cat /proc/1234/status` |
| **内核调优** | 通过 sysctl 接口修改内核参数 | `echo 1 > /proc/sys/net/ipv4/ip_forward` |
| **调试诊断** | 内核开发者临时暴露调试信息 | `/proc/modules`, `/proc/interrupts` |
| **容器隔离** | 每个 PID 命名空间有独立的 /proc | 容器内 `/proc` 只显示容器内进程 |
| **驱动开发** | 驱动模块暴露控制/状态接口 | `/proc/my_driver_info` |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `fs/proc/root.c` | procfs 文件系统类型、挂载、初始化 |
| `fs/proc/generic.c` | proc 条目创建/删除通用逻辑 |
| `fs/proc/inode.c` | proc inode 分配和操作 |
| `fs/proc/proc_net.c` | /proc/net 相关实现 |
| `fs/proc/self.c` | /proc/self 和 /proc/thread-self |
| `fs/proc/meminfo.c` | /proc/meminfo 实现 |
| `fs/proc/stat.c` | /proc/stat 实现 |
| `fs/proc/base.c` | /proc/[pid]/ 目录下的条目实现 |
| `fs/proc/internal.h` | 内部数据结构定义 |
| `include/linux/proc_fs.h` | 对外 API 头文件 |