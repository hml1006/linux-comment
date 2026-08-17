# pivot_root 系统调用分析

## 1. 概述

将当前进程的根文件系统移动到指定目录，并设置新的根文件系统。用于容器技术中切换进程的根文件系统。

**原型：**

```c
SYSCALL_DEFINE2(pivot_root, const char __user *, new_root, const char __user *, put_old)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `new_root` | `const char *` | 新的根文件系统路径 |
| `put_old` | `const char *` | 旧根文件系统移动到的目录 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-EBUSY` — 挂载点正忙
  - `-EINVAL` — 无效参数（如 new_root 不在当前根下）

## 2. 使用场景

- **容器运行时**: Docker/containerd 切换容器根文件系统
- **initramfs 过渡**: 从 initramfs 切换到真实根文件系统
- **系统安装**: 安装过程中切换根文件系统

## 3. 函数调用栈

```
pivot_root(new_root, put_old) (系统调用入口)
└─ ksys_pivot_root(new_root, put_old)                 // fs/namespace.c
   ├─ user_path_at(AT_FDCWD, new_root, ...)            // 解析新根路径
   ├─ user_path_at(AT_FDCWD, put_old, ...)             // 解析旧根存放路径
   │
   ├─ [权限检查]
   │  ├─ capable(CAP_SYS_ADMIN)                        // 需要 CAP_SYS_ADMIN
   │  └─ 检查 new_root 是挂载点
   │
   ├─ attach_mnt_tree(root_mnt, old_mnt)               // 移动旧根到 put_old
   │
   ├─ set_fs_root(current->fs, &new_root_path)         // 更新进程根目录
   │
   └─ chroot_fs_refs()                                 // 更新命名空间引用
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
pivot_root(new_root, put_old)
  │
  ├─ 解析 new_root 和 put_old 路径
  │
  ├─ 权限检查 (CAP_SYS_ADMIN)
  │
  ├─ 将当前根移动到 put_old
  │
  ├─ 将 new_root 设置为新的根
  │
  └─ 更新进程 fs_struct 和挂载命名空间
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 准备新根目录（需要 root 权限）
    // mkdir -p /newroot /put_old

    // 切换根文件系统
    if (syscall(SYS_pivot_root, "/newroot", "/put_old") == -1) {
        perror("pivot_root");
        return 1;
    }

    // 确保 /put_old 不在新根下
    // mount --bind /put_old /put_old
    // pivot_root /newroot /put_old

    printf("Root filesystem changed\n");
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — pivot_root 实现
- `include/linux/mount.h` — 挂载结构定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)