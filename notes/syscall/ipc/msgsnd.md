# msgsnd 系统调用分析

## 1. 概述

`msgsnd` 用于向 System V 消息队列发送一条消息。每条消息包含一个正整数的类型字段和正文数据。

**原型：**

```c
SYSCALL_DEFINE4(msgsnd, int, msqid, struct msgbuf __user *, msgp,
                size_t, msgsz, int, msgflg)
// 实际调用:
return ksys_msgsnd(msqid, msgp, msgsz, msgflg);
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `msqid` | 消息队列 ID |
| `msgp` | 指向 `struct msgbuf` 的指针，包含消息类型和正文 |
| `msgsz` | 消息正文长度（不含消息类型字段） |
| `msgflg` | 标志位 |

**msgflg 标志：**

| 标志 | 说明 |
|------|------|
| `IPC_NOWAIT` | 队列满时立即返回 EAGAIN，不阻塞 |
| `0` | 队列满时阻塞直到有空间可用 |

**struct msgbuf 格式：**

```c
struct msgbuf {
    long mtype;       /* 消息类型，必须 > 0 */
    char mtext[1];    /* 消息正文（长度由 msgsz 指定） */
};
```

## 3. 函数调用链

```
msgsnd (系统调用入口)
  └─ ksys_msgsnd(msqid, msgp, msgsz, msgflg)
       └─ do_msgsnd(msqid, mtype, mtext, msgsz, msgflg)
            ├─ 参数检查:
            │    ├─ msgsz > msg_ctlmax → -EINVAL
            │    ├─ msgsz < 0 → -EINVAL
            │    ├─ msqid < 0 → -EINVAL
            │    └─ mtype < 1 → -EINVAL
            │
            ├─ msg = load_msg(mtext, msgsz)               // 从用户空间拷贝消息正文
            │    └─ 分配 msg_msg + 可能的 msg_msgseg 分段
            │
            ├─ msg->m_type = mtype;                       // 设置消息类型
            ├─ msg->m_ts = msgsz;                         // 设置消息长度
            │
            ├─ rcu_read_lock()
            ├─ msq = msq_obtain_object_check(ns, msqid)   // 查找队列
            │
            ├─ for (;;) {
            │    ├─ ipcperms(ns, &msq->q_perm, S_IWUGO)    // 写权限检查
            │    ├─ ipc_lock_object(&msq->q_perm)
            │    ├─ security_msg_queue_msgsnd(...)          // LSM 安全检查
            │    │
            │    ├─ msg_fits_inqueue(msq, msgsz)            // 检查队列空间
            │    │    ├─ q_cbytes + msgsz <= q_qbytes?      // 总字节数限制
            │    │    └─ q_qnum < msg_ctlmni?               // 消息数限制
            │    │
            │    ├─ 空间足够? → break (继续执行发送)
            │    │
            │    ├─ 空间不足:
            │    │    ├─ [IPC_NOWAIT] → return -EAGAIN
            │    │    └─ [阻塞] 加入 q_senders 等待队列
            │    │         ├─ ss_add(msq, &s, msgsz)
            │    │         ├─ schedule() 阻塞
            │    │         ├─ 被唤醒后:
            │    │         │    ├─ 检查 EIDRM
            │    │         │    ├─ ss_del(&s)
            │    │         │    └─ 检查信号 → ERESTARTNOHAND
            │    │         └─ 重试
            │    }
            │
            ├─ ipc_update_pid(&msq->q_lspid, task_tgid(current))
            ├─ msq->q_stime = ktime_get_real_seconds()
            │
            ├─ pipelined_send(msq, msg, &wake_q)           // 尝试直接传递给等待的接收者
            │    ├─ 有接收者在等待？
            │    │    ├─ 是 → 直接传递给接收者，跳过队列
            │    │    └─ 否 → 将消息入队
            │    │         ├─ list_add_tail(&msg->m_list, &msq->q_messages)
            │    │         ├─ msq->q_cbytes += msgsz
            │    │         ├─ msq->q_qnum++
            │    │         └─ 更新 per-cpu 计数器
            │    │
            │    └─ ipc_unlock_object(&msq->q_perm)
            │
            ├─ rcu_read_unlock()
            ├─ wake_up_q(&wake_q)                          // 唤醒接收者
            └─ return 0
```

## 4. 关键数据结构

### 4.1 struct msg_msgseg（消息分段）

```c
// include/linux/msg.h
struct msg_msgseg {
    struct msg_msgseg *next;           /* 下一个分段 */
    /* 数据紧随其后 */
};
```

大消息采用分段存储，msg_msg 主体包含第一个分段，后续通过 `msg_msgseg->next` 链表链接。

### 4.2 struct msg_sender（发送者等待队列项）

```c
// ipc/msg.c (内部结构)
struct msg_sender {
    struct list_head list;             /* 链表节点，链接到 q_senders */
    struct task_struct *task;          /* 等待的进程 */
    size_t msgsz;                      /* 待发送的消息大小 */
};
```

## 5. 流程图

```
用户态调用 msgsnd(msqid, msgp, msgsz, msgflg)
  │
  v
do_msgsnd(msqid, mtype, mtext, msgsz, msgflg)
  │
  ├── 参数验证
  │
  ├── load_msg() 从用户空间拷贝消息
  │
  ├── 查找消息队列 (msq_obtain_object_check)
  │
  ├── 循环：
  │    ├── 权限检查 (ipcperms)
  │    ├── 安全检查 (LSM)
  │    │
  │    ├── msg_fits_inqueue() 检查空间
  │    │    ├── 足够 → 跳出循环
  │    │    │
  │    │    └── 不足:
  │    │         ├── IPC_NOWAIT → -EAGAIN
  │    │         └── 加入 q_senders
  │    │              └── schedule() 阻塞等待
  │    │
  │    └── 被唤醒 → 重试
  │
  ├── 更新队列统计 (q_lspid, q_stime)
  │
  ├── pipelined_send() 尝试直接传递
  │    ├── 有接收者等待? → 直接传递给接收者
  │    └── 无接收者 → 消息入队
  │
  ├── wake_up_q() 唤醒接收者
  └── 返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | msqid < 0、msgsz > msg_ctlmax、msgsz < 0、mtype < 1 |
| `EACCES` | 权限不足 | 无写权限 |
| `EIDRM` | 队列已删除 | 等待期间队列被删除 |
| `EAGAIN` | 队列满 | IPC_NOWAIT 且队列无空间 |
| `EFAULT` | 地址错误 | msgp 指针不可访问 |
| `ENOMEM` | 内存不足 | 无法分配消息体 |
| `ERESTARTNOHAND` | 信号中断 | 等待时收到信号 |

## 7. 使用示例

```c
#include <sys/msg.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSG_SIZE 256

struct msgbuf {
    long mtype;
    char mtext[MSG_SIZE];
};

int main() {
    key_t key = ftok("/tmp", 'M');
    int msqid = msgget(key, IPC_CREAT | 0666);
    if (msqid == -1) {
        perror("msgget");
        exit(1);
    }

    struct msgbuf buf;
    buf.mtype = 1;  // 消息类型（必须 > 0）
    strcpy(buf.mtext, "Hello, System V Message Queue!");

    // 发送消息（阻塞模式）
    if (msgsnd(msqid, &buf, strlen(buf.mtext) + 1, 0) == -1) {
        perror("msgsnd");
        exit(1);
    }
    printf("Message sent (type=1): %s\n", buf.mtext);

    // 发送高优先级消息
    buf.mtype = 2;
    strcpy(buf.mtext, "High priority message");
    if (msgsnd(msqid, &buf, strlen(buf.mtext) + 1, IPC_NOWAIT) == -1) {
        perror("msgsnd (non-blocking)");
        exit(1);
    }
    printf("Message sent (type=2): %s\n", buf.mtext);

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/msg.c`
- 用户态头文件：`sys/msg.h`