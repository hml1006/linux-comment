# open_tree 系统调用分析

## 1. 概述

获取一个挂载点的文件描述符。这是 Linux 5.2+ 新挂载 API 的一部分，返回的 fd 可用于后续的 `move_mount()` 等操作。

**原型：**

```c
SYSCALL_DEFINE3(open_tree, int, dfd, const char __user *, filename, unsigned, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd |
| `filename` | `const char *` | 挂载点路径 |
| `flags` | `unsigned int` | 打开标志（`OPEN_TREE_CLONE` 等） |

**flags 标志：**

| 标志 | 说明 |
|------|------|
| `OPEN_TREE_CLONE` | 克隆挂载树（创建新挂载实例，不从原挂载树分离） |
| `AT_EMPTY_PATH` | 允许 dfd 指向的路径为空 |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码

## 2. 使用场景

- **挂载树操作**: 获取挂载点 fd 用于 `move_mount()`
- **挂载克隆**: 克隆挂载树用于创建新命名空间
- **容器管理**: 管理容器挂载命名空间

## 3. 函数调用栈

```
open_tree(dfd, filename, flags) (系统调用入口)
└─ ksys_open_tree(dfd, filename, flags)                // fs/namespace.c
   └─ vfs_open_tree(dfd, filename, flags)              // 打开挂载树
        ├─ path = user_path_at(dfd, filename, ...)      // 解析路径
        │
        ├─ [flags & OPEN_TREE_CLONE]
        │    └─ clone_mnt(old_mnt, path.dentry, flag)  // 克隆挂载
        │         ├─ alloc_vfsmnt(old_mnt)              // 分配新挂载
        │         └─ copy_mnt_ns()                      // 复制挂载命名空间
        │
        └─ [默认] → 获取挂载点 fd
             └─ anon_inode_getfd("[mount]", &mount_fops, mnt, ...)
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
open_tree(AT_FDCWD, "/mnt", OPEN_TREE_CLONE)
  │
  ├─ 解析路径 → 找到挂载点
  │
  └─ clone_mnt(old_mnt)
       ├─ alloc_vfsmnt() → 分配新的 mount 结构
       ├─ 复制挂载标志和属性
       └─ 返回指向新挂载的 fd
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
    // 克隆 /mnt 挂载点
    int fd = syscall(SYS_open_tree, AT_FDCWD,
                     "/mnt", OPEN_TREE_CLONE);
    if (fd < 0) { perror("open_tree"); return 1; }

    printf("Open tree fd=%d\n", fd);
    // 可用于 move_mount() 等操作
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — open_tree 实现
- `include/uapi/linux/mount.h` — OPEN_TREE_* 标志定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)