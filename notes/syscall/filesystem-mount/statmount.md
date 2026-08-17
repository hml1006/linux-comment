# statmount 系统调用分析

## 1. 概述

获取指定挂载点的详细信息，包括挂载 ID、父挂载 ID、挂载属性、选项等。这是 Linux 5.2+ 新挂载 API 的一部分。

**原型：**

```c
SYSCALL_DEFINE4(statmount, const struct mnt_id_req __user *, req,
        struct statmount __user *, buf, size_t, bufsize, unsigned int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | `const struct mnt_id_req *` | 查询请求（指定要查询的 mnt_id） |
| `buf` | `struct statmount *` | 接收挂载信息的缓冲区 |
| `bufsize` | `size_t` | 缓冲区大小 |
| `flags` | `unsigned int` | 标志位 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码

## 2. 使用场景

- **`statmount` 命令**: 获取挂载点详细信息
- **容器管理**: 查询容器命名空间的挂载属性
- **系统工具**: `df`、`mount` 等命令获取挂载信息

## 3. 函数调用栈

```
statmount(req, buf, bufsize, flags) (系统调用入口)
└─ ksys_statmount(req, buf, bufsize, flags)          // fs/namespace.c
   ├─ copy_from_user(&kreq, req, size)                 // 拷贝请求参数
   ├─ mnt_id = kreq.mnt_id                             // 获取目标挂载 ID
   ├─ mount = __lookup_mnt_id(ns, mnt_id)              // 通过 ID 查找挂载
   │
   └─ fill_statmount(ks, mount, flags)                 // 填充 statmount 结构
        ├─ ks->mnt_id = mount->mnt_id                  // 挂载 ID
        ├─ ks->mnt_parent_id = parent->mnt_id          // 父挂载 ID
        ├─ ks->mnt_attr = mount->mnt.mnt_flags         // 挂载属性
        ├─ ks->mnt_root = mount->mnt.mnt_root          // 挂载根
        └─ [更多字段...]
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
statmount()
  │
  ├─ 拷贝用户参数 (mnt_id_req)
  │
  ├─ 通过 mnt_id 查找挂载实例
  │
  └─ fill_statmount()
       ├─ 填充 mnt_id / mnt_parent_id
       ├─ 填充 mnt_attr / mnt_attr_userns
       ├─ 填充 mnt_root / mount_options
       └─ copy_to_user(buf, ks, bufsize)
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <linux/statmount.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    struct mnt_id_req req = {
        .size = sizeof(struct mnt_id_req),
        .mnt_id = 1,  // 查询 mnt_id=1 的挂载点
    };
    struct statmount buf;
    long ret = syscall(SYS_statmount, &req, &buf, sizeof(buf), 0);
    if (ret < 0) { perror("statmount"); return 1; }

    printf("mnt_id: %lu\n", buf.mnt_id);
    printf("parent_id: %lu\n", buf.mnt_parent_id);
    printf("mnt_attr: %lu\n", buf.mnt_attr);
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — statmount 实现
- `include/uapi/linux/statmount.h` — statmount 结构定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)