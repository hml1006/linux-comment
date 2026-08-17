# bpf — BPF 文件系统

## 1. 概述与实现机制

bpf 文件系统（BPF 文件系统）用于持久化 BPF 程序和 map，使得 BPF 资源在创建它们的进程退出后仍然存在，并可供其他进程访问。挂载在 `/sys/fs/bpf/`。

### 核心功能

- **BPF 对象 pin 操作**：将 BPF 程序/map 持久化到文件系统
- **BPF 对象 unpin 操作**：移除持久化的 BPF 对象
- **对象共享**：不同进程通过路径访问持久化的 BPF 对象
- **生命周期管理**：BPF 对象生命周期不再绑定到创建进程

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  bpftool prog pin id 42 /sys/fs/bpf/my_prog                │
│  bpftool map pin id 10 /sys/fs/bpf/my_map                  │
│  bpftool prog show /sys/fs/bpf/my_prog                     │
└────────────────────────┬─────────────────────────────────────┘
                         │ 系统调用
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                  BPF 文件系统 (kernel/bpf/inode.c)           │
│  bpf_fs_type → 文件系统类型注册                             │
│  bpf_obj_pin() → 将对象持久化到文件系统                     │
│  bpf_obj_get() → 从文件系统获取对象                         │
│  bpf_mkdir() / bpf_mkobj() 支持目录和对象创建               │
│  bpf_any_get() / bpf_any_put() → 引用计数管理              │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                   BPF 核心 (kernel/bpf/)                     │
│  bpf_prog_alloc() / bpf_map_create()                       │
│  bpf_prog_put() / bpf_map_put()                             │
│  通过 fd 或 pin 路径访问 BPF 对象                           │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 bpf_fs 文件系统类型

```c
// 文件: kernel/bpf/inode.c
static struct file_system_type bpf_fs_type = {
    .owner          = THIS_MODULE,
    .name           = "bpf",
    .init_fs_context = bpf_init_fs_context,
    .parameters     = bpf_fs_parameters,
    .kill_sb        = bpf_kill_super,
    .fs_flags       = FS_USERNS_MOUNT,
};
```

### 2.2 bpf_inode — bpf 文件系统 inode 扩展

```c
// 文件: kernel/bpf/inode.c
struct bpf_inode {
    enum bpf_type type;          // 对象类型 (BPF_TYPE_PROG/MAP/LINK)
    void *private;               // 指向 bpf_prog / bpf_map / bpf_link
    struct inode vfs_inode;      // 嵌入的 VFS inode
};
```

### 2.3 bpf 对象类型枚举

```c
// 文件: kernel/bpf/inode.c
enum bpf_type {
    BPF_TYPE_UNSPEC,     // 未指定
    BPF_TYPE_PROG,       // BPF 程序
    BPF_TYPE_MAP,        // BPF Map
    BPF_TYPE_LINK,       // BPF 链接
    BPF_TYPE_MAP_OF_MAPS, // Map of Maps
    BPF_TYPE_PROG_ARY,   // 程序数组
};
```

---

## 3. API 与使用方法

### 3.1 核心 API

```c
#include <linux/bpf.h>
#include <linux/bpf-inode.h>

// 用户空间系统调用 (通过 libbpf 或 bpftool)
// 查看 bpf 系统调用手册

// 内核内部 API
int bpf_obj_pin_user(u32 ufd, int path_fd, const char __user *pathname);
int bpf_obj_get_user(int path_fd, const char __user *pathname, int flags);
```

### 3.2 使用示例

```bash
# 使用 bpftool 操作 BPF 文件系统

# 加载 BPF 程序并 pin
bpftool prog load bpf_prog.o /sys/fs/bpf/my_prog
bpftool prog pin id 42 /sys/fs/bpf/my_prog

# 创建 BPF map 并 pin
bpftool map create /sys/fs/bpf/my_map type hash key 4 value 8 entries 1024 name my_map
bpftool map pin id 10 /sys/fs/bpf/my_map

# 查看 pin 的 BPF 对象
bpftool prog show /sys/fs/bpf/my_prog
bpftool map show /sys/fs/bpf/my_map

# 删除 pin 的 BPF 对象
rm /sys/fs/bpf/my_prog
rm /sys/fs/bpf/my_map

# 使用 libbpf 编程
// 用户空间 C 程序
int bpf_obj_pin(int fd, const char *pathname);
int bpf_obj_get(const char *pathname);
```

```c
// 内核内部 pin 实现 (简化)
int bpf_obj_pin_user(u32 ufd, int path_fd, const char __user *pathname)
{
    enum bpf_type type;
    void *raw;

    // 根据 fd 探测对象类型
    raw = bpf_fd_probe_obj(ufd, &type);
    //  → bpf_prog_get(fd) → BPF_TYPE_PROG
    //  → bpf_map_get(fd)  → BPF_TYPE_MAP
    //  → bpf_link_get(fd) → BPF_TYPE_LINK
    if (IS_ERR(raw))
        return PTR_ERR(raw);

    // 执行 pin 操作
    ret = bpf_obj_do_pin(path_fd, pathname, raw, type);
    if (ret != 0)
        bpf_any_put(raw, type);  // pin 失败 → 释放引用

    return ret;
}
```

```c
// 内核内部 get 实现 (简化)
int bpf_obj_get_user(int path_fd, const char __user *pathname, int flags)
{
    // 获取路径对应的 inode
    // 获取 inode 中存储的 bpf 对象
    // 创建新的 fd 指向该对象
    // 增加引用计数
    return bpf_obj_do_get(path_fd, pathname, &bpf_flags, flags);
}
```

---

## 4. 函数调用栈

### 4.1 BPF 对象 pin

```
bpftool prog pin id 42 /sys/fs/bpf/my_prog
  ↓
bpf(BPF_OBJ_PIN, ...) 系统调用
  → bpf_obj_pin_user(ufd, path_fd, pathname)      // kernel/bpf/inode.c
    → bpf_fd_probe_obj(ufd, &type)                  // 根据 fd 获取对象
      → bpf_prog_get(ufd)                           // 获取 BPF 程序
        → 如果是 BPF 程序 fd
          → type = BPF_TYPE_PROG
          → return prog
    → bpf_obj_do_pin(path_fd, pathname, raw, type)  // 执行 pin
      → user_path_create()                          // 创建文件路径
      → bpf_inode = bpf_get_inode(sb, d_inode(dir), mode)  // 创建 inode
      → bpf_inode->type = type                      // 存储类型
      → bpf_inode->private = raw                    // 存储对象指针
      → bpf_any_get(raw, type)                      // 增加引用计数
      → d_instantiate(dentry, inode)                // 关联 dentry
```

### 4.2 BPF 对象 get

```
bpftool prog show /sys/fs/bpf/my_prog
  ↓
bpf(BPF_OBJ_GET, ...) 系统调用
  → bpf_obj_get_user(path_fd, pathname, flags)     // kernel/bpf/inode.c
    → bpf_obj_do_get(path_fd, pathname, &bpf_flags, flags)
      → kern_path()                                  // 查找文件路径
      → inode = d_inode(dentry)                      // 获取 inode
      → bpf_inode = BPF_I(inode)                     // 获取 bpf inode
      → raw = bpf_inode->private                     // 获取对象指针
      → type = bpf_inode->type                       // 获取对象类型
      → fd = bpf_any_get(raw, type)                  // 创建新 fd
        → 根据 type 创建文件描述符
        → 增加引用计数
      → return fd
```

### 4.3 BPF 文件系统初始化

```
start_kernel()
  → vfs_caches_init()
    → mnt_init()
      → bpf_init()                                  // kernel/bpf/inode.c
        → sysfs_create_mount_point()                // 创建挂载点
        → register_filesystem(&bpf_fs_type)          // 注册 BPF 文件系统
```

---

## 5. 流程图

### 5.1 BPF 对象生命周期

```
创建 BPF 对象 (程序/map)
    │
    ▼
┌──────────────────────┐
│  bpf_prog_create()   │  ← 创建 BPF 程序
│  或 bpf_map_create() │  ← 创建 BPF map
└──────────┬───────────┘
           │
           ├── 进程内使用 → 通过 fd 引用
           │
           ├── 进程退出 → 引用计数降为 0 → 销毁
           │
           └── bpf_obj_pin() → 持久化到 BPF 文件系统
                 │
                 ▼
           ┌──────────────────────┐
           │  /sys/fs/bpf/my_prog │  ← 创建文件节点
           │  引用计数 +1          │
           └──────────────────────┘
                 │
                 ├── 其他进程通过 bpf_obj_get() 获取
                 │     → 引用计数 +1
                 │     → 创建新 fd
                 │
                 ├── 创建进程退出 → 引用计数 -1 (但仍 > 0)
                 │
                 └── 删除文件 (rm /sys/fs/bpf/my_prog)
                       → 引用计数 -1
                       → 降为 0 → 销毁 BPF 对象
```

### 5.2 BPF 文件系统目录结构

```
/sys/fs/bpf/                    # BPF 文件系统挂载点
│
├── my_prog                     # BPF 程序 pin 节点
│   (内核存储: bpf_prog 结构)   # 读取时返回程序信息
│
├── my_map                      # BPF map pin 节点
│   (内核存储: bpf_map 结构)    # 读取时返回 map 信息
│
├── my_link                     # BPF 链接 pin 节点
│
├── pinned_progs/               # 用户自定义目录
│   ├── trace_enter_openat
│   └── trace_exit_openat
│
├── pinned_maps/                # 用户自定义目录
│   ├── config_map
│   └── stats_map
│
└── ...                         # 其他 BPF 对象
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **BPF 程序持久化** | 在进程退出后保留 BPF 程序 | 系统监控工具加载后退出 |
| **BPF map 共享** | 多个进程共享 BPF map 数据 | 多个监控工具共享性能数据 |
| **容器化部署** | BPF 程序在容器间传递 | 将 BPF 程序 pin 到宿主机文件系统 |
| **加载器分离** | 加载器加载后退出，程序持续运行 | bpftool 加载后退出 |
| **BPF 链接持久化** | 持久化 BPF 程序与事件的绑定 | 跟踪点/断点绑定 |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `kernel/bpf/inode.c` | BPF 文件系统核心实现 |
| `kernel/bpf/syscall.c` | BPF 系统调用 (BPF_OBJ_PIN/GET) |
| `kernel/bpf/preload/` | BPF 预加载 |
| `include/linux/bpf.h` | BPF 核心 API 和数据结构 |
| `tools/lib/bpf/libbpf.c` | 用户空间 libbpf 库 (bpf_obj_pin/get) |