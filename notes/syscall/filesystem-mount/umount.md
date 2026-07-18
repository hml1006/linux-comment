# umount 系统调用分析

## 1. 概述

卸载文件系统。这是传统接口，在 ARM64 上无独立系统调用编号，通过 `umount2`（syscall #39）实现。在新代码中应使用 `umount2`。

**原型：**

```c
SYSCALL_DEFINE1(umount, const char __user *, name)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `name` | `const char *` | 挂载点路径 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-EBUSY` — 文件系统正忙
  - `-EINVAL` — 无效路径

## 2. 使用场景

- **系统管理**: 卸载不需要的文件系统
- **外部存储**: 安全移除 USB 设备前卸载
- **兼容性**: 旧代码中使用的传统接口

## 3. 函数调用栈

```
umount(name) (系统调用入口)
└─ ksys_umount(name, 0)                                // fs/namespace.c
   └─ path_umount(path, flags)                         // flags=0
        ├─ user_path_mountpoint_at(AT_FDCWD, name, ...) // 查找挂载点
        ├─ may_mount()                                  // 权限检查
        └─ do_umount(mnt, 0)                            // 执行卸载
             ├─ may_umount(mnt)                         // 检查使用情况
             ├─ sb->s_op->sync_fs(sb, 1)                // 同步超级块
             └─ do_umount_root(mnt)                     // 从挂载树移除
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
umount("/mnt")
  │
  ├─ 查找挂载点
  ├─ 权限检查 (CAP_SYS_ADMIN)
  └─ do_umount()
       ├─ 检查是否有进程在使用
       ├─ 同步文件系统
       └─ 从挂载树移除
```

## 6. 使用示例

```c
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 卸载文件系统
    if (umount("/mnt/data") == -1) {
        perror("umount");
        return 1;
    }
    printf("/mnt/data unmounted\n");
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — umount/umount2 实现
- `include/linux/mount.h` — 挂载结构定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)