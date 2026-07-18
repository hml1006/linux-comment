# quotactl_fd 系统调用分析

## 1. 概述

通过文件描述符进行磁盘配额操作，与 `quotactl` 功能相同但通过 fd 而非路径指定设备。

**原型：**

```c
SYSCALL_DEFINE4(quotactl_fd, unsigned int, fd, unsigned int, cmd,
        qid_t, id, void __user *, addr)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符（所在文件系统） |
| `cmd` | `unsigned int` | 配额命令（Q_QUOTAON/Q_QUOTAOFF/Q_GETQUOTA/Q_SETQUOTA 等） |
| `id` | `qid_t` | 用户/组 ID |
| `addr` | `void *` | 配额数据缓冲区地址 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的 fd
  - `-EPERM` — 权限不足
  - `-EINVAL` — 无效命令

## 2. 使用场景

- 同 `quotactl`，但通过 fd 指定文件系统（避免路径竞争条件）
- 适用于已打开的文件描述符场景

## 3. 函数调用栈

```
quotactl_fd(fd, cmd, id, addr) (系统调用入口)
└─ ksys_quotactl_fd(fd, cmd, id, addr)               // fs/quota/quota.c
   ├─ fdget(fd)                                        // 获取 file 对象
   ├─ sb = file->f_inode->i_sb                         // 获取超级块
   │
   └─ do_quotactl(sb, cmd, id, addr)                  // 执行配额操作
        ├─ [cmd 分发]
        │  ├─ Q_QUOTAON  → sb->s_op->quota_on(sb, type, ...)  // 启用
        │  ├─ Q_QUOTAOFF → sb->s_op->quota_off(sb, type)      // 停用
        │  ├─ Q_GETQUOTA → dqget(sb, id, type) → 读取配额信息
        │  ├─ Q_SETQUOTA → dqget + dqcommit → 写入配额信息
        │  └─ Q_SYNC     → sb->s_op->sync_fs(sb, ...)         // 同步
        └─ ...
```

## 4. 关键数据结构

```c
// ===== 配额命令宏 (include/uapi/linux/quota.h) =====
#define Q_QUOTAON  0x010001  // 启用配额
#define Q_QUOTAOFF 0x010002  // 停用配额
#define Q_GETQUOTA 0x030007  // 获取配额
#define Q_SETQUOTA 0x030008  // 设置配额
#define Q_SYNC     0x060001  // 同步配额文件

// ===== struct mem_dqblk (内存配额限制, include/linux/quota.h) =====
struct mem_dqblk {
    qsize_t dqb_bhardlimit;   // 块硬限制
    qsize_t dqb_bsoftlimit;   // 块软限制
    qsize_t dqb_curspace;     // 当前使用的空间
    qsize_t dqb_ihardlimit;   // inode 硬限制
    qsize_t qsize_t dqb_isoftlimit; // inode 软限制
    qsize_t dqb_curinodes;    // 当前 inode 数
    time64_t dqb_btime;       // 块限制宽限期
    time64_t dqb_itime;       // inode 限制宽限期
};

// ===== struct dquot (磁盘配额节点, include/linux/quota.h) =====
struct dquot {
    struct hlist_node dq_hash;     // 哈希链表
    struct list_head dq_inuse;     // 使用中链表
    struct list_head dq_free;      // 空闲链表
    struct list_head dq_dirty;     // 脏链表
    struct mutex dq_lock;          // 互斥锁
    spinlock_t dq_dqb_lock;       // 数据锁
    struct kqid dq_id;             // 配额 ID（用户/组）
    unsigned int dq_flags;         // 标志
    struct mem_dqblk dq_dqb;      // 配额限制数据
    struct super_block *dq_sb;     // 所属超级块
};
```

## 5. 流程图

```
quotactl_fd(fd, cmd, id, addr)
  │
  ├─ fdget(fd) → sb = file->f_inode->i_sb
  │
  └─ do_quotactl(sb, cmd, id, addr)
       │
       ├─ Q_QUOTAON  → 启用配额文件
       ├─ Q_QUOTAOFF → 停用配额
       ├─ Q_GETQUOTA → 获取用户/组的配额限制
       ├─ Q_SETQUOTA → 设置用户/组的配额限制
       └─ Q_SYNC     → 同步配额文件
```

## 6. 使用示例

```c
#include <sys/quota.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/mnt/data", O_RDONLY);
    if (fd == -1) { perror("open"); return 1; }

    struct dqblk quota;
    // 获取用户 1000 的磁盘配额
    if (quotactl_fd(fd, Q_GETQUOTA, 1000, (void *)&quota) == -1) {
        perror("quotactl_fd");
        return 1;
    }
    printf("User 1000 quota: cur=%lld, soft=%lld, hard=%lld\n",
           quota.dqb_curspace, quota.dqb_bsoftlimit, quota.dqb_bhardlimit);
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/quota/quota.c` — quotactl_fd 实现
- `include/linux/quota.h` — 配额数据结构
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)