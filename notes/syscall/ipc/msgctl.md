# msgctl 系统调用分析

## 1. 概述

`msgctl` 是 System V 消息队列的控制操作，用于对已创建的消息队列执行各种控制命令，包括获取状态、修改属性、删除队列等。

**原型：**

```c
SYSCALL_DEFINE3(msgctl, int, msqid, int, cmd, struct msqid_ds __user *, buf)
// 实际调用:
return ksys_msgctl(msqid, cmd, buf, IPC_64);
```

## 2. 支持的 cmd 命令

| 命令 | 说明 |
|------|------|
| `IPC_STAT` | 获取消息队列的 `msqid_ds` 状态信息 |
| `IPC_SET` | 设置消息队列的 `msg_perm.uid`、`msg_perm.gid`、`msg_perm.mode` 和 `msg_qbytes` |
| `IPC_RMID` | 删除消息队列及其上所有消息 |
| `IPC_INFO` | 获取系统级消息队列限制信息（`msginfo` 结构） |
| `MSG_INFO` | 获取系统级消息队列消耗信息（类似 IPC_INFO，但返回已用资源） |
| `MSG_STAT` | 获取指定索引的消息队列状态（遍历用，msqid 为索引而非队列 ID） |
| `MSG_STAT_ANY` | 类似 MSG_STAT，但无需拥有权限 |
| `MSG_COPY` | （与 msgrcv 配合）拷贝消息但不移除 |

## 3. 函数调用链

```
msgctl (系统调用入口)
  └─ ksys_msgctl(msqid, cmd, buf, IPC_64)
       ├─ case IPC_INFO / MSG_INFO:
       │    └─ msgctl_info(ns, msqid, cmd, &msginfo)  // 获取系统限制信息
       │         ├─ 获取 msg_ctlmax / msg_ctlmnb / msg_ctlmni 等限制
       │         └─ copy_to_user(buf, &msginfo, sizeof(msginfo))
       ├─ case IPC_STAT / MSG_STAT / MSG_STAT_ANY:
       │    ├─ msgctl_stat(ns, msqid, cmd, &msqid64)   // 获取队列状态
       │    │    ├─ msq = msq_obtain_object_check(ns, msqid)  // 查找队列
       │    │    ├─ ipc_lock_object(&msq->q_perm)       // 加锁
       │    │    ├─ kernel_to_ipc64_perm(...)            // 转换权限结构
       │    │    ├─ 填充 msqid64_ds 各字段
       │    │    └─ ipc_unlock_object(&msq->q_perm)
       │    └─ copy_msqid_to_user(buf, &msqid64, version)  // 拷贝到用户空间
       ├─ case IPC_SET:
       │    ├─ copy_msqid_from_user(&msqid64, buf, version) // 从用户空间拷贝新属性
       │    └─ msgctl_down(ns, msqid, cmd, &msqid64.msg_perm, msqid64.msg_qbytes)
       │         ├─ msq = msq_obtain_object_check(ns, msqid)
       │         ├─ ipcctl_pre_down(&msq->q_perm, ...)  // 权限检查
       │         ├─ 更新 msg_perm.uid/gid/mode
       │         ├─ 更新 msg_qbytes
       │         └─ ipc_update_pid(&msq->q_lspid, ...)
       ├─ case IPC_RMID:
       │    └─ msgctl_down(ns, msqid, cmd, NULL, 0)     // 删除队列
       │         ├─ ipc_lock_object(&msq->q_perm)
       │         ├─ freeque(msq, ns)                     // 释放队列
       │         │    ├─ 遍历 q_messages 链表，释放所有消息
       │         │    ├─ 唤醒所有等待的发送者/接收者
       │         │    └─ ipc_rmid(&msg_ids(ns), &msq->q_perm)  // 从 IDR 中移除
       │         └─ ipc_unlock_object(&msq->q_perm)
       └─ default:
            return -EINVAL
```

## 4. 关键数据结构

### 4.1 struct msqid64_ds（内核态消息队列状态）

```c
// include/uapi/linux/msg.h
struct msqid64_ds {
    struct ipc64_perm msg_perm;    /* 所有权和权限 */
    __kernel_time_t msg_stime;     /* 最后 msgsnd 时间 */
    __kernel_time_t msg_rtime;     /* 最后 msgrcv 时间 */
    __kernel_time_t msg_ctime;     /* 最后修改时间 */
    unsigned long  msg_cbytes;     /* 队列中当前字节数 */
    unsigned long  msg_qnum;       /* 队列中消息数 */
    unsigned long  msg_qbytes;     /* 队列最大字节数 */
    __kernel_pid_t msg_lspid;      /* 最后 msgsnd 的 PID */
    __kernel_pid_t msg_lrpid;      /* 最后 msgrcv 的 PID */
    unsigned long  __unused4;
    unsigned long  __unused5;
};
```

### 4.2 struct msginfo（系统级限制信息）

```c
// include/uapi/linux/msg.h
struct msginfo {
    int msgpool;    /* 缓冲区池大小，未使用 */
    int msgmap;     /* 消息映射条目数，未使用 */
    int msgmax;     /* 单条消息最大字节数 */
    int msgmnb;     /* 队列最大总字节数 */
    int msgmni;     /* 最大消息队列数 */
    int msgssz;     /* 消息段大小，未使用 */
    int msgtql;     /* 系统级最大消息数，未使用 */
    unsigned short msgseg; /* 最大段数，未使用 */
};
```

### 4.3 struct msg_queue（内核消息队列结构）

```c
// include/linux/msg.h
struct msg_queue {
    struct kern_ipc_perm q_perm;       /* IPC 权限结构 */
    time64_t q_stime;                  /* 最后 msgsnd 时间 */
    time64_t q_rtime;                  /* 最后 msgrcv 时间 */
    time64_t q_ctime;                  /* 最后修改时间 */
    unsigned long q_cbytes;            /* 队列中当前字节数 */
    unsigned long q_qnum;              /* 队列中消息数 */
    unsigned long q_qbytes;            /* 队列最大字节数 */
    struct pid *q_lspid;               /* 最后 msgsnd 的进程 PID */
    struct pid *q_lrpid;               /* 最后 msgrcv 的进程 PID */
    struct list_head q_messages;       /* 消息链表头 */
    struct list_head q_receivers;      /* 接收者等待队列 */
    struct list_head q_senders;        /* 发送者等待队列 */
};
```

## 5. 流程图

```
用户态调用 msgctl(msqid, cmd, buf)
  │
  v
syscall 入口 (SYSCALL_DEFINE3)
  │
  v
ksys_msgctl(msqid, cmd, buf, IPC_64)
  │
  ├── cmd 分类
  │
  ├── IPC_INFO ──── msgctl_info() ──── copy_to_user(msginfo)
  │
  ├── MSG_INFO ──── msgctl_info() ──── copy_to_user(msginfo)
  │
  ├── IPC_STAT ──── msgctl_stat() ──── copy_msqid_to_user()
  ├── MSG_STAT ───── msgctl_stat() ──── copy_msqid_to_user()
  ├── MSG_STAT_ANY ── msgctl_stat() ──── copy_msqid_to_user()
  │
  ├── IPC_SET ────── copy_msqid_from_user() ──── msgctl_down()
  │
  ├── IPC_RMID ───── msgctl_down() ──── freeque()
  │                      │
  │                      ├── 遍历 q_messages 释放所有消息
  │                      ├── 唤醒所有等待进程
  │                      └── ipc_rmid() 从 IDR 移除
  │
  └── default ────── return -EINVAL
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | msqid < 0 或 cmd < 0 或未知 cmd |
| `EFAULT` | 用户空间拷贝失败 | buf 指针不可访问 |
| `EACCES` | 权限不足 | IPC_STAT 时无权访问 |
| `EIDRM` | 队列已被删除 | 操作时队列已被标记为删除 |
| `EPERM` | 操作不允许 | IPC_SET/IPC_RMID 时权限不足 |
| `ENOMEM` | 内存不足 | 内核分配内存失败 |
| `EOVERFLOW` | 值溢出 | 32/64 位兼容转换时值溢出 |

## 7. 使用示例

```c
#include <sys/msg.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int main() {
    key_t key = ftok("/tmp", 'M');
    int msqid = msgget(key, IPC_CREAT | 0666);
    if (msqid == -1) {
        perror("msgget");
        exit(1);
    }

    struct msqid_ds buf;

    // 获取消息队列状态
    if (msgctl(msqid, IPC_STAT, &buf) == -1) {
        perror("msgctl IPC_STAT");
        exit(1);
    }
    printf("Queue: msqid=%d, msg_qnum=%lu, msg_qbytes=%lu\n",
           msqid, buf.msg_qnum, buf.msg_qbytes);
    printf("Owner: uid=%d, gid=%d\n",
           buf.msg_perm.uid, buf.msg_perm.gid);

    // 设置最大字节数
    buf.msg_qbytes = 16384;
    if (msgctl(msqid, IPC_SET, &buf) == -1) {
        perror("msgctl IPC_SET");
    }

    // 删除消息队列
    if (msgctl(msqid, IPC_RMID, NULL) == -1) {
        perror("msgctl IPC_RMID");
        exit(1);
    }
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/msg.c`
- 用户态头文件：`include/uapi/linux/msg.h`