# msgget 系统调用分析

## 1. 概述

`msgget` 用于获取或创建一个 System V 消息队列标识符。如果 key 对应的队列已存在，则返回其标识符；否则根据 msgflg 创建新队列。

**原型：**

```c
SYSCALL_DEFINE2(msgget, key_t, key, int, msgflg)
// 实际调用:
return ksys_msgget(key, msgflg);
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `key` | IPC 键值（`IPC_PRIVATE` 或通过 `ftok()` 生成） |
| `msgflg` | 标志位，包含权限和创建选项 |

**msgflg 标志：**

| 标志 | 说明 |
|------|------|
| `IPC_CREAT` | 若队列不存在则创建 |
| `IPC_EXCL` | 与 IPC_CREAT 一起使用时，若队列已存在则返回 EEXIST |
| `0xxx` | 低 9 位为权限位（如 0666 表示读写权限） |

## 3. 函数调用链

```
msgget (系统调用入口)
  └─ ksys_msgget(key, msgflg)
       └─ ipcget(ns, &msg_ids(ns), &msg_ops, &msg_params)
            ├─ 如果 key == IPC_PRIVATE:
            │    └─ newque(ns, &msg_params)              // 创建私有队列
            │         ├─ ipc_rcu_alloc(sizeof(*msq))       // 分配 msg_queue
            │         ├─ ipc_addid(&msg_ids(ns), &msq->q_perm, ns->msg_ctlmni)
            │         │    └─ idr_alloc / xa_alloc          // 分配 ID
            │         ├─ 初始化队列各字段（q_stime/rtime/ctime等）
            │         ├─ INIT_LIST_HEAD(&msq->q_messages)
            │         ├─ INIT_LIST_HEAD(&msq->q_receivers)
            │         └─ INIT_LIST_HEAD(&msq->q_senders)
            │
            └─ 如果 key != IPC_PRIVATE:
                 └─ ipc_findkey(&msg_ids(ns), key)        // 查找已有队列
                      ├─ 找到且 IPC_CREAT|IPC_EXCL → return -EEXIST
                      ├─ 找到 → return msq->q_perm.id
                      └─ 未找到且 IPC_CREAT → newque(ns, &msg_params)
```

## 4. 关键数据结构

### 4.1 struct ipc_ops（IPC 操作函数表）

```c
// ipc/msg.c
static const struct ipc_ops msg_ops = {
    .getnew = newque,                    // 创建新消息队列
    .associate = msg_associate,          // 关联检查
    .more_checks = msg_more_checks,      // 额外检查
};
```

### 4.2 struct ipc_params（IPC 参数）

```c
// ipc/util.h
struct ipc_params {
    key_t key;                          // 键值
    int flg;                            // 标志位
    union {
        size_t size;                    // 用于 shmget
        int nsems;                      // 用于 semget
    } u;
};
```

### 4.3 struct kern_ipc_perm（内核 IPC 权限结构）

```c
// include/linux/ipc.h
struct kern_ipc_perm {
    spinlock_t lock;                    // 自旋锁
    bool deleted;                       // 是否已标记删除
    int id;                             // IPC 对象 ID
    key_t key;                          // IPC 键值
    kuid_t uid;                         // 所有者 uid
    kgid_t gid;                         // 所有者 gid
    kuid_t cuid;                        // 创建者 uid
    kgid_t cgid;                        // 创建者 gid
    umode_t mode;                       // 权限位
    unsigned long seq;                  // 序列号（用于生成唯一 ID）
    struct rcu_head rcu;                // RCU 头
};
```

## 5. 流程图

```
用户态调用 msgget(key, msgflg)
  │
  v
ksys_msgget(key, msgflg)
  │
  v
ipcget(ns, &msg_ids(ns), &msg_ops, &msg_params)
  │
  ├── key == IPC_PRIVATE?
  │    └── 是 → newque() ──→ 分配 ID → 返回 msqid
  │
  └── key != IPC_PRIVATE?
       ├── ipc_findkey() 查找
       │    ├── 找到?
       │    │    ├── (IPC_CREAT|IPC_EXCL) → 返回 -EEXIST
       │    │    ├── 权限检查通过 → 返回 msqid
       │    │    └── 权限检查失败 → 返回 -EACCES
       │    │
       │    └── 未找到?
       │         ├── (IPC_CREAT) → newque() → 返回 msqid
       │         └── (无 IPC_CREAT) → 返回 -ENOENT
       │
       └── newque() 内部:
            ├── ipc_rcu_alloc(sizeof(struct msg_queue))
            ├── ipc_addid(&msg_ids(ns), &msq->q_perm, ns->msg_ctlmni)
            ├── 初始化队列字段
            └── 返回 msqid
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EACCES` | 权限不足 | 队列存在但无访问权限 |
| `EEXIST` | 队列已存在 | 指定了 IPC_CREAT|IPC_EXCL 且队列已存在 |
| `ENOENT` | 队列不存在 | 未指定 IPC_CREAT 且队列不存在 |
| `ENOMEM` | 内存不足 | 无法分配 msg_queue 结构 |
| `ENOSPC` | 超出系统限制 | 队列数已达 msg_ctlmni 上限 |

## 7. 使用示例

```c
#include <sys/msg.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main() {
    key_t key;
    int msqid;

    // 使用 ftok 生成键值
    key = ftok("/tmp", 'M');
    if (key == -1) {
        perror("ftok");
        exit(1);
    }

    // 创建消息队列（如已存在则直接获取）
    msqid = msgget(key, IPC_CREAT | 0666);
    if (msqid == -1) {
        perror("msgget");
        exit(1);
    }
    printf("Message queue created: msqid=%d\n", msqid);

    // 尝试独占创建（应失败，因为队列已存在）
    msqid = msgget(key, IPC_CREAT | IPC_EXCL | 0666);
    if (msqid == -1 && errno == EEXIST) {
        printf("Queue already exists (expected)\n");
    }

    // 创建私有队列
    int priv_id = msgget(IPC_PRIVATE, 0666);
    if (priv_id == -1) {
        perror("msgget IPC_PRIVATE");
        exit(1);
    }
    printf("Private queue created: msqid=%d\n", priv_id);

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/msg.c`、`ipc/util.c`
- 用户态头文件：`sys/msg.h`、`sys/ipc.h`