# name_to_handle_at 系统调用分析

## 1. 概述

获取文件句柄（file handle），用于 NFS 风格的持久化文件引用。与 `open_by_handle_at` 配对使用，可在重启后通过句柄重新打开文件。

**原型：**

```c
SYSCALL_DEFINE5(name_to_handle_at, int, dfd, const char __user *, name,
        struct file_handle __user *, handle, int __user *, mnt_id, int, flag)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd |
| `name` | `const char *` | 文件路径 |
| `handle` | `struct file_handle *` | 输出：文件句柄 |
| `mnt_id` | `int *` | 输出：挂载点 ID |
| `flag` | `int` | 路径解析标志（`AT_EMPTY_PATH` 等） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EFAULT` — 用户态指针无效
  - `-EINVAL` — 无效参数
  - `-ENOENT` — 文件不存在

## 2. 使用场景

- **NFS 文件句柄**: 获取 NFS 导出的文件句柄
- **文件系统备份**: 备份时保存文件句柄
- **文件系统检查**: 通过 inode 号引用文件

## 3. 函数调用栈

```
name_to_handle_at(dfd, name, handle, mnt_id, flag) (系统调用入口)
└─ ksys_name_to_handle_at(dfd, name, handle, mnt_id, flag)  // fs/fhandle.c
   ├─ getname(name)                                      // 拷贝文件名
   ├─ path = user_path_at(dfd, name, flag, &path)        // 路径解析
   │
   ├─ [获取句柄]
   │  └─ exportfs_encode_fh(path.dentry, fh, &handle_size, ...)  // 编码句柄
   │       └─ dentry->d_sb->s_export_op->encode_fh()            // 文件系统实现
   │            └─ [ext4] → ext4_encode_fh()
   │                 └─ 编码 inode 号 + 生成数
   │
   └─ copy_to_user(handle, &fh, ...)                     // 拷贝到用户态
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
name_to_handle_at(AT_FDCWD, "/tmp/test", handle, &mnt_id, 0)
  │
  ├─ 解析路径 → dentry
  │
  └─ exportfs_encode_fh(dentry)
       └─ ext4_encode_fh(dentry)
            ├─ 从 dentry 获取 inode 号
            ├─ 获取文件系统 UUID
            └─ 编码到 file_handle 结构
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
    struct file_handle *handle;
    handle = malloc(sizeof(struct file_handle) + 64);
    handle->handle_bytes = 64;
    int mount_id;

    if (name_to_handle_at(AT_FDCWD, "/tmp/test",
                          handle, &mount_id, 0) == -1) {
        perror("name_to_handle_at");
        return 1;
    }
    printf("File handle obtained, mount_id=%d\n", mount_id);
    free(handle);
    return 0;
}
```

## 7. 参考

- `fs/fhandle.c` — name_to_handle_at 实现
- `include/uapi/linux/fs.h` — file_handle 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)