# msgrcv 系统调用分析

## 1. 概述

`msgrcv` 用于从 System V 消息队列中接收消息。支持按消息类型选择接收，并可指定是否阻塞等待。

**原型：**

```c
SYSCALL_DEFINE5(msgrcv, int, msqid, struct msgbuf __user *, msgp,
                size_t, msgsz, long, msgtyp, int, msgflg)
// 实际调用:
return ksys_msgrcv(msqid, msgp, msgsz, msgtyp, msgflg);
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `msqid` | 消息队列 ID |
| `msgp` | 接收消息缓冲区（`struct msgbuf`） |
| `msgsz` | 消息数据部分最大长度 |
| `msgtyp` | 消息类型选择（见下表） |
| `msgflg` | 标志位 |

**msgtyp 选择规则：**

| msgtyp 值 | 接收行为 |
|-----------|----------|
| `0` | 接收队列中第一条消息 |
| `> 0` | 接收类型等于 msgtyp 的第一条消息 |
| `< 0` | 接收类型小于等于 `abs(msgtyp)` 的最小类型值的第一条消息 |

**msgflg 标志：**

| 标志 | 说明 |
|------|------|
| `IPC_NOWAIT` | 无消息时立即返回 ENOMSG，不阻塞 |
| `MSG_NOERROR` | 消息长度超过 msgsz 时截断而非返回 E2BIG |
| `MSG_EXCEPT` | 配合 msgtyp > 0，接收类型不等于 msgtyp 的消息 |
| `MSG_COPY` | 拷贝消息但不从队列中移除（需配合 IPC_NOWAIT，Linux 特有） |

## 3. 函数调用链

```
msgrcv (系统调用入口)
  └─ ksys_msgrcv(msqid, msgp, msgsz, msgtyp, msgflg)
       └─ do_msgrcv(msqid, msgp, msgsz, msgtyp, msgflg, do_msg_fill)
            ├─ [MSG_COPY] prepare_copy(buf, min(bufsz, ns->msg_ctlmax))
            ├─ convert_mode(&msgtyp, msgflg)               // 转换查找模式
            │
            ├─ rcu_read_lock()
            ├─ msq = msq_obtain_object_check(ns, msqid)    // 查找队列
            │
            ├─ for (;;) {
            │    ├─ ipcperms(ns, &msq->q_perm, S_IRUGO)     // 权限检查
            │    ├─ ipc_lock_object(&msq->q_perm)
            │    │
            │    ├─ find_msg(msq, &msgtyp, mode)            // 按类型查找消息
            │    │    └─ 遍历 q_messages 链表
            │    │         ├─ msgtyp==0 → 取第一条
            │    │         ├─ msgtyp>0 → 匹配类型
            │    │         └─ msgtyp<0 → 取最小类型（<= |msgtyp|）
            │    │
            │    ├─ 找到消息?
            │    │    ├─ 是:
            │    │    │    ├─ 长度检查（bufsz < msg->m_ts → E2BIG 或截断）
            │    │    │    ├─ [MSG_COPY] copy_msg(msg, copy) → 不删除
            │    │    │    ├─ [正常] list_del 从队列移除
            │    │    │    ├─ 更新 q_qnum/q_cbytes/q_rtime/q_lrpid
            │    │    │    ├─ ss_wakeup(msq, &wake_q, false)  // 唤醒等待的发送者
            │    │    │    └─ goto out_unlock0
            │    │    │
            │    │    └─ 否:
            │    │         ├─ [IPC_NOWAIT] → return -ENOMSG
            │    │         └─ [阻塞] 加入 q_receivers 等待队列
            │    │              ├─ set_current_state(TASK_INTERRUPTIBLE)
            │    │              ├─ ipc_unlock_object → rcu_read_unlock
            │    │              ├─ schedule()                 // 阻塞等待
            │    │              └─ 被唤醒后重新检查
            │    }
            │
            ├─ [找到消息后] do_msg_fill(buf, msg, bufsz)
            │    └─ copy_msg_to_user(buf, msg, bufsz)       // 拷贝到用户空间
            ├─ free_msg(msg)                                 // 释放消息体
            └─ wake_up_q(&wake_q)                            // 唤醒等待的发送者
```

## 4. 关键数据结构

### 4.1 struct msg_msg（消息体）

```c
// include/linux/msg.h
struct msg_msg {
    struct list_head m_list;           /* 链表节点，链接到 q_messages */
    long m_type;                       /* 消息类型，必须 > 0 */
    size_t m_ts;                       /* 消息正文长度 */
    struct msg_msgseg *next;           /* 分段链表（大消息时使用） */
    void *security;                    /* 安全字段 */
    /* 消息正文紧随其后 */
};
```

### 4.2 struct msg_receiver（接收者等待队列项）

```c
// ipc/msg.c (内部结构)
struct msg_receiver {
    struct list_head r_list;           /* 链表节点，链接到 q_receivers */
    struct task_struct *r_tsk;         /* 等待的进程 */
    struct msg_msg *r_msg;             /* 接收到的消息指针 */
    long r_msgtype;                    /* 期望的消息类型 */
    long r_mode;                       /* 查找模式 */
    size_t r_maxsize;                  /* 最大接收大小 */
};
```

## 5. 流程图

```
用户态调用 msgrcv(msqid, msgp, msgsz, msgtyp, msgflg)
  │
  v
do_msgrcv(msqid, msgp, msgsz, msgtyp, msgflg, do_msg_fill)
  │
  ├── 查找消息队列 (msq_obtain_object_check)
  │
  ├── 循环：
  │    ├── 权限检查
  │    ├── 加锁
  │    │
  │    ├── find_msg() 在 q_messages 链表中查找
  │    │    │
  │    │    ├── 找到?
  │    │    │    ├── 是 → 检查长度 → 从链表移除 → 更新统计 → 跳出
  │    │    │    └── 否 → 进入等待
  │    │    │
  │    │    └── IPC_NOWAIT? → 返回 -ENOMSG
  │    │
  │    ├── 加入 q_receivers 等待队列
  │    ├── schedule() 阻塞
  │    └── 被唤醒后重试
  │
  ├── do_msg_fill() 拷贝消息到用户空间
  ├── free_msg() 释放内核消息
  └── 唤醒等待的发送者
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | msqid < 0 或 bufsz < 0 或 MSG_COPY 使用不当 |
| `EACCES` | 权限不足 | 无读权限 |
| `EIDRM` | 队列已删除 | 等待期间队列被删除 |
| `ENOMSG` | 无消息 | IPC_NOWAIT 且队列中无匹配消息 |
| `E2BIG` | 消息太长 | msgsz < 消息长度且未设 MSG_NOERROR |
| `EFAULT` | 地址错误 | msgp 指针不可访问 |
| `ERESTARTNOHAND` | 信号中断 | 等待时收到信号 |
| `ENOMEM` | 内存不足 | MSG_COPY 时无法分配内存 |

## 7. 使用示例

```c
#include <sys/msg.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct msgbuf {
    long mtype;
    char mtext[256];
};

int main() {
    key_t key = ftok("/tmp", 'M');
    int msqid = msgget(key, IPC_CREAT | 0666);
    if (msqid == -1) {
        perror("msgget");
        exit(1);
    }

    struct msgbuf buf;
    ssize_t n;

    // 接收类型为 1 的消息（阻塞等待）
    n = msgrcv(msqid, &buf, sizeof(buf.mtext), 1, 0);
    if (n == -1) {
        perror("msgrcv");
        exit(1);
    }
    printf("Received (type=1, size=%zd): %s\n", n, buf.mtext);

    // 非阻塞接收类型为 2 的消息
    n = msgrcv(msqid, &buf, sizeof(buf.mtext), 2, IPC_NOWAIT);
    if (n == -1) {
        perror("msgrcv (non-blocking)");
    } else {
        printf("Received (type=2, size=%zd): %s\n", n, buf.mtext);
    }

    // 接收优先级最低的消息（msgtyp < 0）
    n = msgrcv(msqid, &buf, sizeof(buf.mtext), -100, IPC_NOWAIT);
    if (n == -1) {
        perror("msgrcv (any type)");
    } else {
        printf("Received (any type<=100, size=%zd): %s\n", n, buf.mtext);
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/msg.c`
- 用户态头文件：`sys/msg.h`