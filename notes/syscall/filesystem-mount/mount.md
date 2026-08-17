# mount 系统调用分析

## 1. 概述

挂载文件系统。将设备上的文件系统关联到目录树的某个挂载点，使文件和目录可访问。

**原型：**

```c
SYSCALL_DEFINE5(mount, const char __user *, dev_name, const char __user *, dir_name,
        const char __user *, type, unsigned long, flags, void __user *, data)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dev_name` | `const char *` | 设备名（如 `/dev/sda1`）或伪文件系统名（如 `proc`） |
| `dir_name` | `const char *` | 挂载点路径 |
| `type` | `const char *` | 文件系统类型（如 `ext4`、`xfs`、`tmpfs`） |
| `flags` | `unsigned long` | 挂载标志（`MS_RDONLY`、`MS_NOEXEC` 等） |
| `data` | `void *` | 文件系统特定选项 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-ENOENT` — 设备或挂载点不存在
  - `-EBUSY` — 设备或挂载点正忙
  - `-EINVAL` — 无效参数

## 2. 使用场景

- **系统启动**: 挂载根文件系统
- **外部存储**: 挂载 USB 设备、SD 卡
- **虚拟文件系统**: 挂载 procfs、sysfs、tmpfs、devtmpfs
- **容器**: 挂载容器文件系统

## 3. 函数调用栈

```
mount(dev_name, dir_name, type, flags, data) (系统调用入口)
└─ ksys_mount(dev_name, dir_name, type, flags, data)  // fs/namespace.c
   └─ do_mount(dev_name, dir_name, type, flags, data)  // 核心挂载逻辑
        ├─ user_path_at(AT_FDCWD, dir_name, LOOKUP_FOLLOW, &path)
        │                                                 // 解析挂载点路径
        ├─ [权限检查]
        │  ├─ capable(CAP_SYS_ADMIN)                     // 需要 CAP_SYS_ADMIN
        │  └─ inode_permission(inode, MAY_WRITE)         // 挂载点需要可写
        │
        ├─ [根据 flags 分发]
        │  ├─ MS_REMOUNT    → do_remount()               // 重新挂载
        │  ├─ MS_BIND       → do_loopback()              // 绑定挂载
        │  ├─ MS_MOVE       → do_move_mount()            // 移动挂载
        │  └─ [默认]        → do_new_mount()             // 全新挂载
        │
        └─ do_new_mount(&path, type, flags, options, dev_name)
             ├─ type = get_fs_type(type)                 // 查找文件系统类型
             ├─ sb = sget_fc(fc, ...)                     // 获取或创建超级块
             └─ do_add_mount(sb, path, mnt_flags)         // 添加到挂载树
```

## 4. 关键数据结构

```c
// ===== struct mount (挂载实例, include/linux/mount.h) =====
struct mount {
    struct hlist_node mnt_hash;           // 挂载哈希链表节点
    struct mount *mnt_parent;             // 父挂载
    struct dentry *mnt_mountpoint;        // 挂载点的 dentry
    struct vfsmount mnt;                  // 通用 VFS 挂载结构
    struct list_head mnt_mounts;          // 子挂载链表
    struct list_head mnt_child;           // 兄弟挂载链表
    struct list_head mnt_instance;        // 超级块关联链表
    const char *mnt_devname;              // 设备名（如 /dev/sda1）
    struct list_head mnt_list;            // 挂载命名空间链表
    int mnt_id;                           // 挂载 ID
    int mnt_group_id;                     // 挂载组 ID
    struct mnt_pcp __percpu *mnt_pcp;     // 每 CPU 引用计数
};

// ===== struct vfsmount (VFS 挂载, include/linux/mount.h) =====
struct vfsmount {
    struct dentry *mnt_root;              // 挂载的根 dentry
    struct super_block *mnt_sb;           // 指向超级块
    int mnt_flags;                        // 挂载标志
    struct user_namespace *mnt_userns;    // 挂载的用户命名空间
};

// ===== struct super_block (超级块, include/linux/fs.h) =====
struct super_block {
    struct list_head s_list;              // 全局超级块链表
    dev_t s_dev;                          // 设备号
    unsigned long s_blocksize;            // 块大小（字节）
    struct file_system_type *s_type;      // 文件系统类型
    const struct super_operations *s_op;  // 超级块操作函数集
    struct dentry *s_root;                // 根 dentry
    struct list_head s_inodes;            // 所有 inode 链表
    struct list_head s_mounts;            // 关联的挂载链表
    void *s_fs_info;                      // 文件系统私有数据
};

// ===== struct mnt_id_req (挂载 ID 查询请求, include/uapi/linux/mount.h) =====
struct mnt_id_req {
    __u32 size;           // 结构体大小
    __u64 mnt_id;         // 挂载 ID
    __u64 last_mnt_id;    // 上次查询的最后一个 ID（listmount 分页用）
    __u64 mnt_group_id;   // 挂载组 ID
    __u32 param;          // 预留参数
};

// ===== struct statmount (挂载点统计信息, include/uapi/linux/statmount.h) =====
struct statmount {
    __u32 size;             // 结构体大小
    __u64 mnt_id;           // 挂载 ID
    __u64 mnt_parent_id;    // 父挂载 ID
    __u64 mnt_id_old;       // 旧挂载 ID
    __u64 mnt_parent_id_old; // 旧父挂载 ID
    __u64 mnt_attr;         // 挂载属性
    __u64 mnt_attr_userns;  // 用户命名空间属性
    __u64 mnt_root;         // 挂载根
    __u64 mount_options;    // 挂载选项
};
```

## 5. 流程图

```
mount("/dev/sda1", "/mnt", "ext4", 0, NULL)
  │
  v
do_mount()
  │
  ├─ 解析挂载点路径 → dentry + mnt
  │
  ├─ 权限检查 (CAP_SYS_ADMIN)
  │
  └─ do_new_mount()
       │
       ├─ get_fs_type("ext4")  → 查找 ext4 文件系统驱动
       │
       ├─ vfs_get_tree(fc)     → 读取超级块
       │    └─ ext4_fill_super()  → 读取 ext4 超级块
       │
       └─ do_add_mount()       → 挂载到命名空间
            ├─ graft_tree()    → 挂载树嫁接
            └─ mnt_id 分配
```

## 6. 使用示例

```c
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 挂载 ext4 分区
    if (mount("/dev/sda1", "/mnt/data", "ext4", 0, NULL) == -1) {
        perror("mount");
        return 1;
    }
    printf("/dev/sda1 mounted to /mnt/data\n");
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — mount 核心实现
- `include/linux/mount.h` — 挂载结构定义
- `include/uapi/linux/mount.h` — 用户态挂载标志
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)