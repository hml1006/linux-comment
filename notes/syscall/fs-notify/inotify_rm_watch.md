# inotify_rm_watch 系统调用分析

## 1. 概述

`inotify_rm_watch` 用于从 inotify 实例中删除一个之前通过 `inotify_add_watch` 添加的监控项（watch）。删除后，该监控项对应的文件系统事件将不再被报告。

**原型：**

```c
#include <sys/inotify.h>

int inotify_rm_watch(int fd, int wd);
```

**内核入口：**

```c
// fs/notify/inotify/inotify_user.c:786
SYSCALL_DEFINE2(inotify_rm_watch, int, fd, __s32, wd)
```

## 2. 使用场景

- **停止监控**：不再需要监控某个文件或目录时删除监控项
- **资源清理**：程序退出前清理监控项，释放系统资源
- **动态监控管理**：根据应用需求动态添加/删除监控项

## 3. 函数调用栈

```
inotify_rm_watch(fd, wd)                                // 系统调用入口
  │
  ├─ 验证 fd 有效性
  │   ├─ CLASS(fd, f)(fd)                               // 获取文件描述符
  │   ├─ fd_empty(f) → 返回 -EBADF                     // 无效 fd
  │   └─ fd_file(f)->f_op != &inotify_fops → 返回 -EINVAL  // 不是 inotify 实例
  │
  ├─ group = fd_file(f)->private_data                   // 获取 inotify 组
  │
  ├─ i_mark = inotify_idr_find(group, wd)               // 通过 wd 查找监控项
  │   └─ idr_find() 在 IDR 中查找
  │   └─ 未找到 → 返回 -EINVAL
  │
  ├─ fsnotify_destroy_mark(&i_mark->fsn_mark, group)     // 销毁标记
  │   ├─ 从对象链表移除
  │   ├─ 从 group 链表移除
  │   ├─ 从 IDR 中移除 wd 映射
  │   └─ 触发标记销毁通知
  │
  └─ fsnotify_put_mark(&i_mark->fsn_mark)               // 释放引用
       └─ 匹配 inotify_idr_find 获取的引用
```

**核心实现源码：**

```c
// fs/notify/inotify/inotify_user.c:786
SYSCALL_DEFINE2(inotify_rm_watch, int, fd, __s32, wd)
{
    struct fsnotify_group *group;
    struct inotify_inode_mark *i_mark;
    CLASS(fd, f)(fd);

    if (fd_empty(f))
        return -EBADF;

    /* verify that this is indeed an inotify instance */
    if (unlikely(fd_file(f)->f_op != &inotify_fops))
        return -EINVAL;

    group = fd_file(f)->private_data;

    i_mark = inotify_idr_find(group, wd);
    if (unlikely(!i_mark))
        return -EINVAL;

    fsnotify_destroy_mark(&i_mark->fsn_mark, group);

    /* match ref taken by inotify_idr_find */
    fsnotify_put_mark(&i_mark->fsn_mark);
    return 0;
}
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

### 4.2 struct fsnotify_mark（fsnotify 标记基础结构）

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

## 5. 流程图

```
用户态调用 inotify_rm_watch(fd, wd)
    │
    ▼
SYSCALL_DEFINE2(inotify_rm_watch)
    │
    ├─ 验证 fd
    │   ├─ fd 无效 → -EBADF
    │   └─ 不是 inotify 实例 → -EINVAL
    │
    ├─ group = fd->private_data
    │
    ├─ inotify_idr_find(group, wd)
    │   ├─ wd 有效 → 返回 i_mark
    │   └─ wd 无效 → 返回 NULL → -EINVAL
    │
    ├─ fsnotify_destroy_mark(&i_mark->fsn_mark, group)
    │   │
    │   ├─ 获取 fsnotify_mark 的 spinlock
    │   │
    │   ├─ 从 inode 的标记链表移除
    │   │   └─ list_del_init(&mark->obj_list)
    │   │
    │   ├─ 从 group 的标记链表移除
    │   │   └─ list_del_init(&mark->g_list)
    │   │
    │   ├─ 从 IDR 移除 wd 映射
    │   │   └─ idr_remove(&group->inotify_data->idr, wd)
    │   │
    │   ├─ 释放 connector 引用
    │   │
    │   └─ 释放 spinlock
    │
    ├─ fsnotify_put_mark(&i_mark->fsn_mark)
    │   └─ 引用计数减 1，为 0 时释放内存
    │
    └─ 返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EBADF` | 无效 fd | `fd` 不是有效的文件描述符 |
| `EINVAL` | 无效参数 | `fd` 不是 inotify 实例；`wd` 不是有效的 watch descriptor |

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
    int fd;
    int wd1, wd2;

    // 创建 inotify 实例
    fd = inotify_init1(0);
    if (fd == -1) {
        perror("inotify_init1");
        exit(EXIT_FAILURE);
    }

    // 添加多个监控项
    wd1 = inotify_add_watch(fd, "/tmp", IN_CREATE | IN_DELETE);
    if (wd1 == -1) {
        perror("inotify_add_watch /tmp");
        exit(EXIT_FAILURE);
    }
    printf("监控 /tmp, wd=%d\n", wd1);

    wd2 = inotify_add_watch(fd, "/var/log", IN_MODIFY | IN_ATTRIB);
    if (wd2 == -1) {
        perror("inotify_add_watch /var/log");
        exit(EXIT_FAILURE);
    }
    printf("监控 /var/log, wd=%d\n", wd2);

    // 删除对 /tmp 的监控
    if (inotify_rm_watch(fd, wd1) == -1) {
        printf("删除监控失败: %s\n", strerror(errno));
    } else {
        printf("已删除 /tmp 的监控 (wd=%d)\n", wd1);
    }

    // 尝试删除已删除的监控项（应该失败）
    if (inotify_rm_watch(fd, wd1) == -1) {
        printf("尝试删除已删除的监控项: %s\n", strerror(errno));
    }

    // 使用无效的 wd
    if (inotify_rm_watch(fd, 999) == -1) {
        printf("使用无效 wd: %s\n", strerror(errno));
    }

    // 删除对 /var/log 的监控
    inotify_rm_watch(fd, wd2);

    close(fd);
    return 0;
}
```

**可能的输出：**

```
监控 /tmp, wd=1
监控 /var/log, wd=2
已删除 /tmp 的监控 (wd=1)
尝试删除已删除的监控项: Invalid argument
使用无效 wd: Invalid argument
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件与目录事件监控)
- 内核源码：`fs/notify/inotify/inotify_user.c`
- 内核头文件：`fs/notify/inotify/inotify.h`
- 内核头文件：`include/linux/fsnotify_backend.h`