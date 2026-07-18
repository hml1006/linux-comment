# quotactl 系统调用分析

## 1. 概述

磁盘配额管理，控制用户/组对磁盘空间的使用。通过指定设备路径和命令来管理配额。

**原型：**

```c
SYSCALL_DEFINE4(quotactl, unsigned int, cmd, const char __user *, special,
        qid_t, id, void __user *, addr)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `cmd` | `unsigned int` | 配额命令（Q_QUOTAON/Q_QUOTAOFF 等） |
| `special` | `const char *` | 块设备路径（如 `/dev/sda1`） |
| `id` | `qid_t` | 用户/组 ID |
| `addr` | `void *` | 配额数据缓冲区地址 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 权限不足
  - `-EINVAL` — 无效命令
  - `-ENOENT` — 设备不存在

## 2. 使用场景

- **多用户服务器**: 限制用户磁盘使用量
- **共享主机**: 防止单个用户耗尽磁盘空间
- **配额管理工具**: `quota`、`edquota`、`repquota` 命令

## 3. 函数调用栈

```
quotactl(cmd, special, id, addr) (系统调用入口)
└─ ksys_quotactl(cmd, special, id, addr)              // fs/quota/quota.c
   ├─ sb = quotactl_block(special)                     // 通过设备路径找到超级块
   │
   └─ do_quotactl(sb, cmd, id, addr)                  // 执行配额操作
        ├─ [cmd 分发]
        │  ├─ Q_QUOTAON  → quota_on(sb, type, ...)     // 启用配额
        │  ├─ Q_QUOTAOFF → quota_off(sb, type)         // 停用配额
        │  ├─ Q_GETQUOTA → dqget(sb, id, type) → 读取
        │  ├─ Q_SETQUOTA → dqget + dqcommit → 写入
        │  └─ Q_SYNC     → sync_filesystem(sb)         // 同步
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
quotactl(Q_GETQUOTA, "/dev/sda1", 1000, &dqblk)
  │
  ├─ 通过设备路径找到超级块
  │
  └─ do_quotactl(sb, cmd, id, addr)
       │
       ├─ Q_QUOTAON  → 读取配额文件 (aquota.user/aquota.group)
       ├─ Q_QUOTAOFF → 关闭配额跟踪
       ├─ Q_GETQUOTA → 返回用户/组的配额限制和使用量
       ├─ Q_SETQUOTA → 设置用户/组的配额限制
       └─ Q_SYNC     → 同步配额文件到磁盘
```

## 6. 使用示例

```c
#include <sys/quota.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dqblk quota;
    // 获取用户 1000 在 /dev/sda1 上的配额
    if (quotactl(Q_GETQUOTA, "/dev/sda1", 1000, (void *)&quota) == -1) {
        perror("quotactl");
        return 1;
    }
    printf("User 1000 quota on /dev/sda1:\n");
    printf("  Current space: %lld\n", quota.dqb_curspace);
    printf("  Soft limit: %lld\n", quota.dqb_bsoftlimit);
    printf("  Hard limit: %lld\n", quota.dqb_bhardlimit);
    return 0;
}
```

## 7. 参考

- `fs/quota/quota.c` — quotactl 实现
- `include/linux/quota.h` — 配额数据结构
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)