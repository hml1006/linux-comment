# open_tree_attr 系统调用分析

## 1. 概述

打开挂载点并获取其属性 fd，通过 `mount_attr` 结构查询或修改挂载属性。这是 Linux 5.2+ 新挂载 API 的一部分。

**原型：**

```c
SYSCALL_DEFINE5(open_tree_attr, int, dfd, const char __user *, filename,
        unsigned, flags, struct mount_attr __user *, uattr,
        size_t, usize)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd |
| `filename` | `const char *` | 路径名 |
| `flags` | `unsigned int` | 打开标志（`OPEN_TREE_CLONE` 等） |
| `uattr` | `struct mount_attr *` | 挂载属性（查询/设置） |
| `usize` | `size_t` | 属性结构体大小 |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码

## 2. 使用场景

- **挂载属性查询**: 查询挂载点的传播类型、挂载标志等
- **挂载属性修改**: 设置挂载为只读、递归从属等
- **容器管理**: 管理容器命名空间的挂载属性

## 3. 函数调用栈

```
open_tree_attr(dfd, filename, flags, uattr, usize) (系统调用入口)
└─ ksys_open_tree_attr(dfd, filename, flags, uattr, usize)  // fs/namespace.c
   ├─ vfs_open_tree(dfd, filename, flags)                // 打开挂载树
   │    └─ 获取 mount 实例
   │
   └─ set_mount_attributes(mnt, uattr, ...)             // 设置/查询属性
        ├─ copy_from_user(&attr, uattr, size)            // 拷贝用户参数
        ├─ [查询] → 读取 mount 属性
        └─ [设置] → 修改 mount 属性
             ├─ mount->mnt.mnt_flags = attr.mnt_attr
             └─ 更新传播关系
```

## 4. 关键数据结构

```c
// ===== struct file_handle (文件句柄, include/uapi/linux/fs.h) =====
struct file_handle {
    unsigned int handle_bytes;   // 句柄数据大小
    int handle_type;             // 句柄类型
    unsigned char f_handle[];    // 句柄数据（可变长度）
};

// ===== struct file_lock (文件锁, include/linux/fs.h) =====
struct file_lock {
    struct file_lock *fl_next;       // 同一 inode 上的下一个锁
    struct list_head fl_list;        // 锁链表
    struct hlist_node fl_link;       // 哈希链表
    fl_owner_t fl_owner;             // 锁所有者
    unsigned int fl_flags;           // 锁标志
    unsigned char fl_type;           // 锁类型 (F_RDLCK/F_WRLCK/F_UNLCK)
    unsigned int fl_pid;             // 持有锁的进程 PID
    struct pid *fl_nspid;            // 命名空间 PID
    wait_queue_head_t fl_wait;       // 等待队列
    struct file *fl_file;            // 关联的文件
};

// ===== struct open_how (openat2 参数, include/uapi/linux/openat2.h) =====
struct open_how {
    __u64 flags;     // O_* 打开标志
    __u64 mode;      // 创建模式（O_CREAT 时有效）
    __u64 resolve;   // 路径解析控制标志
};
// resolve 标志位:
// RESOLVE_NO_XDEV       - 禁止跨设备
// RESOLVE_NO_MAGICLINKS - 禁止 magic 符号链接
// RESOLVE_NO_SYMLINKS   - 禁止符号链接
// RESOLVE_BENEATH       - 限制在 dfd 下
// RESOLVE_IN_ROOT       - 以根目录为锚点
```

## 5. 流程图

```
open_tree_attr(dfd, path, flags, uattr, usize)
  │
  ├─ vfs_open_tree() → 获取 mount 实例
  │
  └─ set_mount_attributes()
       ├─ copy_from_user(uattr)  // 获取属性请求
       ├─ [查询] → 读取 mnt_flags 并返回
       └─ [设置] → 修改 mnt_flags
            ├─ 设置只读/读写
            ├─ 设置传播类型
            └─ 返回新 fd
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(void)
{
    struct mount_attr attr = {
        .attr_set = MOUNT_ATTR_RDONLY,  // 设置为只读
        .attr_clr = 0,
    };

    // 打开 /mnt 挂载点并设置属性
    int fd = syscall(SYS_open_tree_attr, AT_FDCWD,
                     "/mnt", 0, &attr, sizeof(attr));
    if (fd < 0) { perror("open_tree_attr"); return 1; }

    printf("Mount attribute set, fd=%d\n", fd);
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — open_tree/open_tree_attr 实现
- `include/uapi/linux/mount.h` — mount_attr 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)