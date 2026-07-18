# inotify_add_watch 系统调用分析

## 1. 概述

`inotify_add_watch` 用于向 inotify 实例添加一个新的监控项（watch），或修改一个已存在的监控项的事件掩码。每个监控项关联一个 inode，并指定要监控的事件类型（如文件打开、修改、关闭等）。

**原型：**

```c
#include <sys/inotify.h>

int inotify_add_watch(int fd, const char *pathname, uint32_t mask);
```

**内核入口：**

```c
// fs/notify/inotify/inotify_user.c:729
SYSCALL_DEFINE3(inotify_add_watch, int, fd, const char __user *, pathname,
                u32, mask)
```

## 2. 使用场景

- **文件变更监控**：监控特定文件或目录的变更事件
- **目录监控**：监控目录下的文件创建、删除、移动等操作
- **配置热加载**：监控配置文件修改，自动重新加载
- **文件管理器**：实时更新文件列表显示

## 3. 函数调用栈

```
inotify_add_watch(fd, pathname, mask)                    // 系统调用入口
  │
  ├─ 验证 mask 合法性
  │   ├─ mask & ~ALL_INOTIFY_BITS → 返回 -EINVAL
  │   └─ !(mask & ALL_INOTIFY_BITS) → 返回 -EINVAL
  │
  ├─ 验证 fd 有效性
  │   ├─ fd_empty(f) → 返回 -EBADF
  │   └─ fd_file(f)->f_op != &inotify_fops → 返回 -EINVAL
  │
  ├─ 检查 IN_MASK_ADD 和 IN_MASK_CREATE 互斥
  │   └─ 同时设置 → 返回 -EINVAL
  │
  ├─ inotify_find_inode(pathname, &path, flags, mask)   // 解析路径
  │   └─ 路径不存在 → 返回 -ENOENT
  │
  ├─ inode = path.dentry->d_inode
  ├─ group = fd_file(f)->private_data
  │
  └─ inotify_update_watch(group, inode, mask)            // 核心操作
       │
       ├─ inotify_new_watch(group, inode, mask)           // 创建新监控
       │   ├─ inotify_add_to_idr(group, inode, &wd)      // 分配 wd（watch descriptor）
       │   │   └─ idr_alloc_cyclic() 分配唯一 wd
       │   ├─ fsnotify_add_inode_mark()                  // 添加 inode mark
       │   │   └─ fsnotify_attach_connector_and_object()
       │   │        └─ inode->i_fsnotify_marks 添加 mark
       │   └─ 返回 wd
       │
       └─ inotify_update_existing_watch()                // 更新已存在的监控
            └─ 更新事件掩码
```

## 4. 关键数据结构

### 4.1 struct inotify_inode_mark（inotify inode 标记）

```c
// fs/notify/inotify/inotify.h:15
struct inotify_inode_mark {
    struct fsnotify_mark fsn_mark;   // 基础 fsnotify 标记
    int wd;                          // watch descriptor（监控描述符）
};
```

### 4.2 struct inotify_event（inotify 事件结构）

```c
// include/uapi/linux/inotify.h
struct inotify_event {
    __s32   wd;         // 监控描述符（watch descriptor）
    __u32   mask;       // 事件掩码（IN_ACCESS, IN_MODIFY, IN_CREATE 等）
    __u32   cookie;     // 关联 cookie（IN_MOVED_FROM/IN_MOVED_TO 成对使用）
    __u32   len;        // name 字段长度
    char    name[];     // 事件文件名（变长，0 表示事件发生在被监控对象本身）
};
```

### 4.3 常用事件掩码

```c
// include/uapi/linux/inotify.h
#define IN_ACCESS       0x00000001  // 文件被访问
#define IN_MODIFY       0x00000002  // 文件被修改
#define IN_ATTRIB       0x00000004  // 文件属性变化
#define IN_CLOSE_WRITE  0x00000008  // 文件关闭（写模式打开）
#define IN_CLOSE_NOWRITE 0x00000010 // 文件关闭（非写模式打开）
#define IN_OPEN         0x00000020  // 文件被打开
#define IN_MOVED_FROM   0x00000040  // 文件从目录移出
#define IN_MOVED_TO     0x00000080  // 文件移入目录
#define IN_CREATE       0x00000100  // 文件/目录创建
#define IN_DELETE       0x00000200  // 文件/目录删除
#define IN_DELETE_SELF  0x00000400  // 被监控对象本身被删除
#define IN_MOVE_SELF    0x00000800  // 被监控对象本身被移动
```

## 5. 流程图

```
用户态调用 inotify_add_watch(fd, pathname, mask)
    │
    ▼
SYSCALL_DEFINE3(inotify_add_watch)
    │
    ├─ 验证 mask
    │   ├─ mask 包含无效位 → -EINVAL
    │   └─ mask 没有有效位 → -EINVAL
    │
    ├─ 验证 fd
    │   ├─ fd 无效 → -EBADF
    │   └─ 不是 inotify 实例 → -EINVAL
    │
    ├─ 检查 IN_MASK_ADD && IN_MASK_CREATE → -EINVAL
    │
    ├─ inotify_find_inode(pathname, ...)
    │   └─ 解析路径为 inode
    │
    └─ inotify_update_watch(group, inode, mask)
        │
        ├─ 查找是否已有该 inode 的监控
        │   │
        │   ├─ [已存在]
        │   │   └─ inotify_update_existing_watch()
        │   │       ├─ [IN_MASK_CREATE] → 返回 -EEXIST
        │   │       ├─ [IN_MASK_ADD] → mask |= old_mask
        │   │       └─ [默认] → mask = new_mask
        │   │
        │   └─ [不存在]
        │       └─ inotify_new_watch()
        │           ├─ inotify_add_to_idr()
        │           │   └─ idr_alloc_cyclic() 分配 wd
        │           │
        │           ├─ fsnotify_add_inode_mark()
        │           │   ├─ 分配 fsnotify_mark
        │           │   ├─ 初始化 mask 和 wd
        │           │   └─ 添加到 inode->i_fsnotify_marks
        │           │
        │           └─ 返回 wd
        │
        └─ 返回 wd
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EBADF` | 无效 fd | `fd` 不是有效的文件描述符 |
| `EINVAL` | 无效参数 | `mask` 包含无效事件位；`fd` 不是 inotify 实例；`IN_MASK_ADD` 和 `IN_MASK_CREATE` 同时设置 |
| `ENOENT` | 路径不存在 | `pathname` 指向的路径不存在 |
| `ENOMEM` | 内存不足 | 无法分配 `inotify_inode_mark` 结构 |
| `ENOSPC` | 空间不足 | 监控项数量超过 `/proc/sys/fs/inotify/max_user_watches` 限制 |
| `EEXIST` | 已存在 | `IN_MASK_CREATE` 标志设置但监控项已存在 |
| `EACCES` | 权限不足 | 对路径中的某个组件没有搜索权限 |
| `ENOTDIR` | 不是目录 | `IN_ONLYDIR` 标志设置但路径不是目录 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    int fd, wd;

    // 创建 inotify 实例
    fd = inotify_init1(IN_NONBLOCK);
    if (fd == -1) {
        perror("inotify_init1");
        exit(EXIT_FAILURE);
    }

    // 监控 /tmp 目录的创建和删除事件
    wd = inotify_add_watch(fd, "/tmp", IN_CREATE | IN_DELETE);
    if (wd == -1) {
        perror("inotify_add_watch");
        exit(EXIT_FAILURE);
    }
    printf("监控 /tmp 目录，wd=%d\n", wd);

    // 监控 /etc/passwd 的修改事件
    int wd2 = inotify_add_watch(fd, "/etc/passwd", IN_MODIFY);
    if (wd2 == -1) {
        perror("inotify_add_watch /etc/passwd");
        // 非关键错误，继续执行
    } else {
        printf("监控 /etc/passwd 修改，wd=%d\n", wd2);
    }

    // 使用 IN_MASK_CREATE 确保只创建新监控（不修改已存在的）
    int wd3 = inotify_add_watch(fd, "/tmp", IN_CREATE | IN_MASK_CREATE);
    if (wd3 == -1 && errno == EEXIST) {
        printf("/tmp 已有监控项\n");
    }

    // 使用 IN_MASK_ADD 添加额外事件到现有监控
    // 这会向 /tmp 的监控添加 IN_ATTRIB 事件
    inotify_add_watch(fd, "/tmp", IN_ATTRIB | IN_MASK_ADD);

    close(fd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件与目录事件监控)
- 内核源码：`fs/notify/inotify/inotify_user.c`
- 内核头文件：`fs/notify/inotify/inotify.h`
- 用户空间 API：`include/uapi/linux/inotify.h`