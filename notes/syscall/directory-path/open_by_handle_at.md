# open_by_handle_at 系统调用分析

## 1. 概述

通过文件句柄（file handle）打开文件。与 `name_to_handle_at` 配对使用，用于实现 NFS 风格的持久化文件引用。

**原型：**

```c
SYSCALL_DEFINE3(open_by_handle_at, int, mountdirfd,
        struct file_handle __user *, handle, int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `mountdirfd` | `int` | 文件系统挂载点的 fd（由 `name_to_handle_at` 的 `mnt_id` 获得） |
| `handle` | `struct file_handle *` | 文件句柄 |
| `flags` | `int` | `O_*` 打开标志 |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码：
  - `-EACCES` — 权限不足
  - `-ESTALE` — 文件句柄已过期（文件被删除或移动）
  - `-EINVAL` — 无效的句柄

## 2. 使用场景

- **NFS 文件句柄**: 通过 NFS 导出的文件句柄重新打开文件
- **文件系统备份**: 备份时保存文件句柄，恢复时用句柄打开
- **文件系统检查**: `fsck` 工具通过 inode 号打开文件

## 3. 函数调用栈

```
open_by_handle_at(mountdirfd, handle, flags) (系统调用入口)
└─ do_handle_open(mountdirfd, handle, flags)           // fs/fhandle.c
   ├─ getname(handle->f_handle, handle->handle_bytes)   // 获取文件系统句柄
   │
   ├─ exportfs_decode_fh(sb, fh, flags, ...)           // 解码句柄为 dentry
   │    └─ sb->s_export_op->fh_to_dentry(sb, fh, ...)  // 文件系统特定实现
   │         └─ [ext4] → ext4_fh_to_dentry()
   │              └─ ext4_get_inode_by_inum()           // 通过 inode 号查找
   │
   └─ vfs_open(path, file)                              // 打开文件
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
open_by_handle_at(mountdirfd, handle, flags)
  │
  ├─ 通过 mountdirfd 找到文件系统超级块
  │
  ├─ exportfs_decode_fh(sb, fh)
  │    └─ fh_to_dentry(sb, fh)  // 解码句柄
  │         └─ ext4_fh_to_dentry()
  │              └─ ext4_get_inode_by_inum(inode_num)
  │                   └─ 读取 inode 表
  │
  └─ vfs_open(path, file)  // 打开文件并返回 fd
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/fs.h>

int main(void)
{
    // 1. 先获取文件句柄
    struct file_handle *handle;
    handle = malloc(sizeof(struct file_handle) + 64);
    handle->handle_bytes = 64;
    int mount_id;
    if (name_to_handle_at(AT_FDCWD, "/tmp/test",
                          handle, &mount_id, 0) == -1) {
        perror("name_to_handle_at");
        return 1;
    }

    // 2. 打开挂载点
    int mnt_fd = open("/", O_RDONLY);
    if (mnt_fd == -1) { perror("open mount"); return 1; }

    // 3. 通过句柄打开文件
    int fd = open_by_handle_at(mnt_fd, handle, O_RDWR);
    if (fd == -1) { perror("open_by_handle_at"); return 1; }

    printf("Opened by handle: fd=%d\n", fd);
    close(fd);
    close(mnt_fd);
    free(handle);
    return 0;
}
```

## 7. 参考

- `fs/fhandle.c` — name_to_handle_at/open_by_handle_at 实现
- `include/uapi/linux/fs.h` — file_handle 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)