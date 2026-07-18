# fanotify_mark 系统调用分析

## 1. 概述

`fanotify_mark` 用于添加、删除或修改 fanotify 通知组对文件系统对象的监控标记（mark）。它支持对文件系统挂载点、文件系统、目录和文件进行监控，并指定要监控的事件类型。

**原型：**

```c
#include <sys/fanotify.h>

int fanotify_mark(int fd, unsigned int flags, uint64_t mask,
                  int dirfd, const char *pathname);
```

**内核入口：**

```c
// fs/notify/fanotify/fanotify_user.c:2155
SYSCALL_DEFINE5(fanotify_mark, int, fanotify_fd, unsigned int, flags,
                __u64, mask, int, dfd, const char  __user *, pathname)
```

## 2. 使用场景

- **挂载点监控**：监控整个文件系统挂载点上的所有事件
- **目录监控**：监控特定目录及其子目录的事件
- **文件监控**：监控特定文件的事件
- **访问决策**：结合 `FAN_OPEN_PERM` / `FAN_ACCESS_PERM` 实现权限控制

## 3. 函数调用栈

```
fanotify_mark(fd, flags, mask, dfd, pathname)            // 系统调用入口
  │
  └─ do_fanotify_mark(fanotify_fd, flags, mask, dfd, pathname)  // 核心实现
       │
       ├─ 参数验证
       │   ├─ upper_32_bits(mask) → 返回 -EINVAL（仅使用低 32 位）
       │   ├─ flags & ~FANOTIFY_MARK_FLAGS → 返回 -EINVAL
       │   └─ 检查 mark_type 和 mark_cmd
       │
       ├─ fsnotify_find_group(fd)                       // 通过 fd 查找 group
       │   └─ 验证 fd 是 fanotify 实例
       │
       ├─ fanotify_find_path(dfd, pathname, &path, flags)  // 解析路径
       │   ├─ [FAN_MARK_MOUNT] → 解析为挂载点
       │   ├─ [FAN_MARK_FILESYSTEM] → 解析为文件系统
       │   └─ [FAN_MARK_INODE] → 解析为 inode
       │
       ├─ 操作分发
       │   ├─ [FAN_MARK_ADD]
       │   │   └─ fanotify_add_mark(group, path, mask, ...)
       │   │        └─ fsnotify_add_mark_locked(group, inode, ...)
       │   │
       │   ├─ [FAN_MARK_REMOVE]
       │   │   └─ fanotify_remove_mark(group, path, ...)
       │   │
       │   └─ [FAN_MARK_FLUSH]
       │       └─ fsnotify_clear_marks_by_group(group, ...)
       │
       └─ 返回 0
```

## 4. 关键数据结构

### 4.1 struct fsnotify_mark（fsnotify 标记）

```c
// include/linux/fsnotify_backend.h
struct fsnotify_mark {
    struct fsnotify_mark_connector *connector; // 连接器（指向 inode/mount）
    struct fsnotify_group *group;              // 所属 group
    spinlock_t lock;                           // 保护锁
    __u32 mask;                                // 事件掩码
    struct list_head obj_list;                 // 对象链表
    struct list_head g_list;                   // group 链表
    struct rcu_head rcu;                       // RCU 销毁
    unsigned int flags;                        // 标记标志
};
```

### 4.2 fanotify 标记类型

```c
// include/uapi/linux/fanotify.h
#define FAN_MARK_ADD        0x00000001  // 添加标记
#define FAN_MARK_REMOVE     0x00000002  // 删除标记
#define FAN_MARK_FLUSH      0x00000080  // 清空所有标记

#define FAN_MARK_INODE      0x00000000  // 监控 inode（文件/目录）
#define FAN_MARK_MOUNT      0x00000010  // 监控挂载点
#define FAN_MARK_FILESYSTEM 0x00000100  // 监控整个文件系统

#define FAN_MARK_IGNORED_MASK  0x00000020  // 忽略掩码
#define FAN_MARK_IGNORED_SURV_MODIFY 0x00000040  // 忽略掩码在修改后保留
```

## 5. 流程图

```
用户态调用 fanotify_mark(fd, flags, mask, dfd, pathname)
    │
    ▼
do_fanotify_mark(fanotify_fd, flags, mask, dfd, pathname)
    │
    ├─ 验证 mask 和 flags
    │
    ├─ fsnotify_find_group(fd)
    │   └─ 验证 fd 是 fanotify 实例
    │
    ├─ 确定标记类型
    │   ├─ FAN_MARK_INODE → FSNOTIFY_OBJ_TYPE_INODE
    │   ├─ FAN_MARK_MOUNT → FSNOTIFY_OBJ_TYPE_VFSMOUNT
    │   └─ FAN_MARK_FILESYSTEM → FSNOTIFY_OBJ_TYPE_SB
    │
    ├─ 解析路径
    │   ├─ pathname == NULL → 使用 dfd 指向的文件
    │   ├─ pathname != NULL → 解析路径
    │   └─ FAN_MARK_MOUNT → 获取挂载点 mnt
    │
    ├─ 操作分发
    │   │
    │   ├─ FAN_MARK_ADD ──────────────────────────────────
    │   │   ├─ 检查现有标记是否已存在
    │   │   ├─ 检查标记数量限制
    │   │   ├─ 分配 fsnotify_mark
    │   │   ├─ 初始化 mask 和 flags
    │   │   └─ 添加到对象的标记链表
    │   │
    │   ├─ FAN_MARK_REMOVE ───────────────────────────────
    │   │   ├─ 查找已存在的标记
    │   │   ├─ 从对象链表移除
    │   │   └─ 释放标记
    │   │
    │   └─ FAN_MARK_FLUSH ────────────────────────────────
    │       └─ 清空 group 的所有标记
    │
    └─ 返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EBADF` | 无效 fd | `fanotify_fd` 不是有效的文件描述符 |
| `EINVAL` | 无效参数 | `flags` 或 `mask` 包含无效值；`dfd` 和 `pathname` 组合无效 |
| `ENOENT` | 不存在 | 路径不存在 |
| `ENOMEM` | 内存不足 | 无法分配 `fsnotify_mark` 结构 |
| `ENOSPC` | 空间不足 | 标记数量超过限制 |
| `EEXIST` | 已存在 | `FAN_MARK_ADD` 且标记已存在 |
| `ENOENT` | 不存在 | `FAN_MARK_REMOVE` 且标记不存在 |
| `EPERM` | 权限不足 | 非特权用户尝试设置 mount/filesystem 标记 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/fanotify.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

int main(void)
{
    int fd;

    // 创建 fanotify 通知组
    fd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC, O_RDONLY | O_LARGEFILE);
    if (fd == -1) {
        perror("fanotify_init");
        exit(EXIT_FAILURE);
    }

    // 监控 /tmp 目录的打开和关闭事件
    if (fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_MOUNT,
                      FAN_OPEN | FAN_CLOSE,
                      AT_FDCWD, "/tmp") == -1) {
        perror("fanotify_mark");
        exit(EXIT_FAILURE);
    }
    printf("已添加标记: 监控 /tmp 的打开和关闭事件\n");

    // 添加忽略掩码：忽略对 /tmp 中特定文件的访问
    // （实际应用中用于减少事件噪声）

    // 读事件循环
    ssize_t len;
    char buf[4096];
    struct fanotify_event_metadata *metadata;

    while (1) {
        len = read(fd, buf, sizeof(buf));
        if (len == -1 && errno != EAGAIN) {
            perror("read");
            break;
        }

        if (len <= 0)
            break;

        metadata = (struct fanotify_event_metadata *)buf;
        while (FAN_EVENT_OK(metadata, len)) {
            printf("事件: mask=0x%llx, fd=%d, pid=%d\n",
                   metadata->mask, metadata->fd, metadata->pid);

            if (metadata->fd >= 0) {
                close(metadata->fd);  // 关闭事件传递的 fd
            }
            metadata = FAN_EVENT_NEXT(metadata, len);
        }
    }

    // 删除标记
    fanotify_mark(fd, FAN_MARK_REMOVE | FAN_MARK_MOUNT,
                  FAN_OPEN | FAN_CLOSE, AT_FDCWD, "/tmp");

    close(fd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件与目录事件监控)
- 内核源码：`fs/notify/fanotify/fanotify_user.c`
- 内核头文件：`include/linux/fsnotify_backend.h`
- 用户空间 API：`include/uapi/linux/fanotify.h`