# mq_getsetattr 系统调用分析

## 1. 概述

`mq_getsetattr` 用于获取和/或设置 POSIX 消息队列的属性。可以同时获取旧属性并设置新属性，或仅获取当前属性。

**原型：**

```c
SYSCALL_DEFINE3(mq_getsetattr, mqd_t, mqdes,
                const struct mq_attr __user *, u_mqstat,
                struct mq_attr __user *, u_omqstat)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `mqdes` | 消息队列描述符（由 `mq_open` 返回） |
| `u_mqstat` | 新属性（可为 NULL，仅获取时不设置） |
| `u_omqstat` | 输出旧属性（可为 NULL，不获取旧属性） |

**struct mq_attr：**

```c
struct mq_attr {
    long mq_flags;       /* 标志：0 或 O_NONBLOCK */
    long mq_maxmsg;      /* 最大消息数（只读） */
    long mq_msgsize;     /* 最大消息大小（只读） */
    long mq_curmsgs;     /* 当前消息数（只读） */
};
```

注意：`mq_getsetattr` 只能修改 `mq_flags` 字段（仅 `O_NONBLOCK` 标志），`mq_maxmsg`、`mq_msgsize`、`mq_curmsgs` 为只读。

## 3. 函数调用链

```
mq_getsetattr (系统调用入口)
  │
  ├─ copy_from_user(&mqstat, u_mqstat, sizeof(mq_attr))  // 拷贝新属性（如果有）
  │
  └─ do_mq_getsetattr(mqdes, new, old)
       │
       ├─ 验证新属性:
       │    └─ new->mq_flags 只能包含 O_NONBLOCK（其他位非法）
       │
       ├─ CLASS(fd, f)(mqdes)                             // 获取 fd 文件
       │    └─ fdget(mqdes) → struct fd
       │
       ├─ 检查文件操作是否为 mqueue_file_operations
       │
       ├─ inode = file_inode(fd_file(f))
       ├─ info = MQUEUE_I(inode)                          // 获取 mqueue_inode_info
       │
       ├─ spin_lock(&info->lock)
       │
       ├─ 如果 old 不为 NULL:
       │    ├─ *old = info->attr                          // 拷贝当前属性
       │    └─ old->mq_flags = file->f_flags & O_NONBLOCK // 实际标志位
       │
       ├─ 如果 new 不为 NULL:
       │    ├─ audit_mq_getsetattr(mqdes, new)
       │    ├─ 更新 file->f_flags 中的 O_NONBLOCK
       │    └─ 更新 inode 时间戳
       │
       └─ spin_unlock(&info->lock)
```

## 4. 关键数据结构

### 4.1 struct mq_attr（消息队列属性）

```c
// include/uapi/linux/mqueue.h
struct mq_attr {
    long mq_flags;       /* 消息队列标志 [只可设置 O_NONBLOCK] */
    long mq_maxmsg;      /* 队列中最大消息数 [创建时指定，只读] */
    long mq_msgsize;     /* 单条消息最大字节数 [创建时指定，只读] */
    long mq_curmsgs;     /* 当前在队列中的消息数 [只读] */
};
```

### 4.2 struct mqueue_inode_info

```c
// ipc/mqueue.c
struct mqueue_inode_info {
    spinlock_t lock;
    struct inode vfs_inode;
    struct rb_root_cached msg_tree;    /* 按优先级排序的消息红黑树 */
    struct list_head msg_list;         /* 消息链表 */
    unsigned long qsize;               /* 当前队列总字节数 */
    unsigned long qcount;              /* 当前消息数 */
    unsigned long q_maxsize;           /* 最大字节数 */
    unsigned long q_maxmsg;            /* 最大消息数 */
    unsigned long q_msgsize;           /* 单条消息最大大小 */
    int q_flags;
    struct mq_attr attr;               /* 队列属性 */
    struct sigevent notify;            /* 通知配置 */
    struct pid *notify_owner;          /* 通知所有者 */
    struct user_struct *user;
    struct sock *notify_sock;
    struct file *notify_file;
    wait_queue_head_t wait_q;          /* 等待队列 */
};
```

## 5. 流程图

```
用户态调用 mq_getsetattr(mqdes, newattr, oldattr)
  │
  ├── newattr != NULL? → copy_from_user 拷贝到内核
  │
  v
do_mq_getsetattr(mqdes, new, old)
  │
  ├── [new] 验证 mq_flags 只含 O_NONBLOCK
  │
  ├── fdget() 获取文件结构
  ├── 验证是 mqueue 文件
  │
  ├── spin_lock(&info->lock)
  │
  ├── [old] 拷贝当前属性到 old
  ├── [new] 更新文件 f_flags 的 O_NONBLOCK 位
  │
  └── spin_unlock(&info->lock)
       │
       └── [old] copy_to_user(u_omqstat, old) 返回给用户
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效描述符 | mqdes 不是有效的消息队列描述符 |
| `EINVAL` | 无效参数 | new->mq_flags 包含 O_NONBLOCK 以外的标志 |
| `EFAULT` | 地址错误 | u_mqstat 或 u_omqstat 指针不可访问 |

## 7. 使用示例

```c
#include <mqueue.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    struct mq_attr attr = {
        .mq_flags   = 0,
        .mq_maxmsg  = 10,
        .mq_msgsize = 1024,
        .mq_curmsgs = 0,
    };

    mqd_t mq = mq_open("/test_queue", O_CREAT | O_RDWR, 0644, &attr);
    if (mq == (mqd_t)-1) {
        perror("mq_open");
        exit(1);
    }

    struct mq_attr old_attr;

    // 仅获取当前属性
    if (mq_getsetattr(mq, NULL, &old_attr) == -1) {
        perror("mq_getsetattr");
        exit(1);
    }
    printf("Flags: %ld, MaxMsg: %ld, MsgSize: %ld, CurMsgs: %ld\n",
           old_attr.mq_flags, old_attr.mq_maxmsg,
           old_attr.mq_msgsize, old_attr.mq_curmsgs);

    // 设置为非阻塞模式
    struct mq_attr new_attr;
    new_attr.mq_flags = O_NONBLOCK;
    if (mq_getsetattr(mq, &new_attr, &old_attr) == -1) {
        perror("mq_getsetattr set nonblock");
        exit(1);
    }
    printf("Old flags: %ld (before setting NONBLOCK)\n", old_attr.mq_flags);

    mq_close(mq);
    mq_unlink("/test_queue");
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#posix-消息队列)
- 源码位置：`ipc/mqueue.c`
- 用户态头文件：`mqueue.h`