# fanotify_init 系统调用分析

## 1. 概述

`fanotify_init` 用于初始化一个 fanotify 事件组，并返回一个文件描述符用于后续操作。fanotify（filesystem notification）是一种文件系统事件监控机制，相比 inotify，它支持更细粒度的事件控制、访问决策（权限检查）和全局文件系统范围的监控。

**原型：**

```c
#include <sys/fanotify.h>

int fanotify_init(unsigned int flags, unsigned int event_f_flags);
```

**内核入口：**

```c
// fs/notify/fanotify/fanotify_user.c:1607
SYSCALL_DEFINE2(fanotify_init, unsigned int, flags, unsigned int, event_f_flags)
```

## 2. 使用场景

- **文件系统访问监控**：监控文件系统上所有文件/目录的访问、打开、关闭等操作
- **恶意软件扫描**：结合 `FAN_OPEN_PERM` 或 `FAN_ACCESS_PERM` 实现实时访问决策
- **文件完整性监控**：监控关键系统文件的修改
- **备份与同步**：监控文件系统变化以触发增量备份

## 3. 函数调用栈

```
fanotify_init(flags, event_f_flags)                      // 系统调用入口
  │
  └─ fanotify_init() 实现 (fs/notify/fanotify/fanotify_user.c)
       │
       ├─ 权限检查
       │   ├─ [有 CAP_SYS_ADMIN] → 完全权限
       │   │   └─ 允许所有操作
       │   └─ [无 CAP_SYS_ADMIN] → 有限权限（非特权模式）
       │       ├─ 仅允许 FANOTIFY_FID_BITS | FAN_REPORT_MNT
       │       ├─ 设置 FANOTIFY_UNPRIV 内部标志
       │       └─ 限制 mount/filesystem mark 和 pid/fd 报告
       │
       ├─ 标志验证
       │   ├─ 检查 flags 中的有效位
       │   ├─ FAN_REPORT_PIDFD 和 FAN_REPORT_TID 互斥
       │   ├─ FAN_REPORT_MNT 只能与 FAN_CLASS_NOTIF 结合
       │   └─ FAN_REPORT_NAME 需要 FAN_REPORT_DIR_FID
       │
       ├─ 验证 event_f_flags
       │   ├─ 检查 O_ACCMODE 有效（O_RDONLY/O_RDWR/O_WRONLY）
       │   └─ 检查扩展标志有效
       │
       ├─ fanotify 通知组类型选择
       │   ├─ FAN_CLASS_NOTIF: 标准通知（默认）
       │   ├─ FAN_CLASS_CONTENT: 内容访问前通知（允许修改内容）
       │   └─ FAN_CLASS_PRE_CONTENT: 内容访问前通知（更高优先级）
       │
       ├─ fsnotify_alloc_group(&fanotify_fsnotify_ops)  // 分配 fsnotify_group
       │   └─ 分配并初始化 fsnotify 通知组
       │
       ├─ get_unused_fd_flags(flags & FAN_CLOEXEC)       // 获取空闲 fd
       │
       └─ anon_inode_getfd("fanotify", &fanotify_fops, group, ...)  // 创建文件
            └─ 返回 fd
```

## 4. 关键数据结构

### 4.1 struct fsnotify_group（通知组）

```c
// include/linux/fsnotify_backend.h
struct fsnotify_group {
    const struct fsnotify_ops *ops;      // 操作函数集
    struct fsnotify_event_queue event_list;  // 事件队列
    spinlock_t notification_lock;        // 通知锁
    wait_queue_head_t notification_waitq; // 等待队列（read/poll 阻塞）
    struct list_head listener_list;      // 监听器链表
    atomic_t user_waits;                 // 用户等待计数
    unsigned int max_events;             // 最大事件数
    struct mem_cgroup *memcg;            // 内存 cgroup
    struct fwnode_handle *fanotify_data; // fanotify 私有数据
    struct inotify_group_private_data *inotify_data; // inotify 私有数据
};
```

### 4.2 struct fanotify_event_metadata（fanotify 事件元数据）

```c
// include/uapi/linux/fanotify.h
struct fanotify_event_metadata {
    __u32 event_len;      // 事件长度
    __u8 vers;            // 版本号
    __u8 reserved;        // 保留
    __u16 metadata_len;   // 元数据长度
    __align_u64 mask;     // 事件掩码（FAN_OPEN, FAN_ACCESS, FAN_CLOSE 等）
    __s32 fd;             // 事件关联的文件描述符
    __s32 pid;            // 触发事件的进程 PID
};
```

## 5. 流程图

```
用户态调用 fanotify_init(flags, event_f_flags)
    │
    ▼
syscall 入口
    │
    ├─ 权限检查
    │   ├─ 有 CAP_SYS_ADMIN → 无限制
    │   └─ 无 CAP_SYS_ADMIN → 有限功能（非特权模式）
    │
    ├─ 标志验证
    │   ├─ 检查 FAN_CLASS_BITS 通知类
    │   │   ├─ FAN_CLASS_NOTIF      (0x00) — 标准通知
    │   │   ├─ FAN_CLASS_CONTENT    (0x04) — 内容访问前通知
    │   │   └─ FAN_CLASS_PRE_CONTENT (0x08) — 预内容通知
    │   │
    │   ├─ 检查 FANOTIFY_FID_BITS 报告模式
    │   │   ├─ FAN_REPORT_FID       — 报告文件标识符
    │   │   ├─ FAN_REPORT_DIR_FID   — 报告目录标识符
    │   │   ├─ FAN_REPORT_NAME      — 报告文件名（需 DIR_FID）
    │   │   └─ FAN_REPORT_TARGET_FID — 报告目标文件标识符
    │   │
    │   ├─ 检查 FAN_REPORT_PIDFD / FAN_REPORT_TID 互斥
    │   └─ 检查 FAN_REPORT_MNT 限制
    │
    ├─ fsnotify_alloc_group()
    │   ├─ kzalloc(sizeof(struct fsnotify_group))
    │   ├─ 初始化 spinlock
    │   ├─ 初始化等待队列
    │   ├─ 初始化事件队列
    │   └─ 设置 ops = &fanotify_fsnotify_ops
    │
    ├─ anon_inode_getfd("fanotify", &fanotify_fops, group, ...)
    │   ├─ 创建匿名 inode 和 file 结构
    │   ├─ 关联 fanotify_fops 操作函数集
    │   └─ 设置 file->private_data = group
    │
    └─ 返回 fd
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--|--|--|
| `EINVAL` | 无效参数 | `flags` 或 `event_f_flags` 包含无效标志组合 |
| `EPERM` | 权限不足 | 非特权用户尝试设置管理员标志（如 `FAN_CLASS_CONTENT`） |
| `EMFILE` | 文件描述符过多 | 已达到进程级文件描述符限制 |
| `ENOMEM` | 内存不足 | 无法分配 `fsnotify_group` 结构 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/fanotify.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    int fd;

    // 创建 fanotify 通知组（标准通知模式，非阻塞）
    fd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK,
                       O_RDONLY | O_LARGEFILE);
    if (fd == -1) {
        perror("fanotify_init");
        exit(EXIT_FAILURE);
    }

    printf("fanotify 初始化成功，fd=%d\n", fd);

    // 创建 fanotify 通知组（权限决策模式）
    int fd_perm = fanotify_init(FAN_CLASS_CONTENT | FAN_CLOEXEC,
                                O_RDONLY | O_LARGEFILE);
    if (fd_perm == -1) {
        printf("注意: 权限决策模式需要 CAP_SYS_ADMIN: %s\n",
               strerror(errno));
    } else {
        printf("fanotify 权限决策模式初始化成功，fd=%d\n", fd_perm);
        close(fd_perm);
    }

    close(fd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件与目录事件监控)
- 内核源码：`fs/notify/fanotify/fanotify_user.c`
- 内核头文件：`include/linux/fsnotify_backend.h`
- 用户空间 API：`include/uapi/linux/fanotify.h`