# mq_timedreceive 系统调用分析

## 1. 概述

`mq_timedreceive` 用于从 POSIX 消息队列中接收一条消息。消息按优先级顺序接收（最高优先级优先，同优先级 FIFO）。支持超时等待。

**原型：**

```c
SYSCALL_DEFINE5(mq_timedreceive, mqd_t, mqdes, char __user *, u_msg_ptr,
                size_t, msg_len, unsigned int __user *, u_msg_prio,
                const struct __kernel_timespec __user *, u_abs_timeout)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `mqdes` | 消息队列描述符 |
| `u_msg_ptr` | 接收消息缓冲区 |
| `msg_len` | 缓冲区大小（必须 >= 队列的 mq_msgsize） |
| `u_msg_prio` | 输出消息优先级 |
| `u_abs_timeout` | 绝对超时时间（NULL 表示阻塞等待） |

## 3. 函数调用链

```
mq_timedreceive (系统调用入口)
  │
  ├─ [timeout] copy_from_user 并转换 timespec64
  │
  └─ do_mq_timedreceive(mqdes, u_msg_ptr, msg_len, u_msg_prio, timeout)
       │
       └─ do_mq_timedreceive(fd, u_msg_ptr, msg_len, u_msg_prio, timeout)
            │
            ├─ fget(fd)                                    // 获取 file 结构
            ├─ inode = file_inode(file)
            ├─ info = MQUEUE_I(inode)                      // 获取 mqueue_inode_info
            │
            ├─ security_mq_msg_receive(&info->vfs_inode, ...)  // LSM 检查
            │
            ├─ mutex_lock(&info->lock)
            │
            ├─ 循环:
            │    ├─ [队列空]:
            │    │    ├─ [O_NONBLOCK 或 timeout 已过期] → -EAGAIN
            │    │    ├─ [阻塞] wait_for_msg(info, timeout)
            │    │    │    └─ prepare_to_wait + schedule_timeout
            │    │    └─ 被唤醒后重试
            │    │
            │    └─ [队列非空]:
            │         ├─ msg = msg_remove(info, &prio_ptr)  // 取出最高优先级消息
            │         │    └─ 从红黑树和链表移除
            │         │
            │         ├─ 检查 msg_len >= msg->m_ts:
            │         │    └─ 否 → 放回消息 → -EMSGSIZE
            │         │
            │         ├─ copy_to_user(u_msg_ptr, msg->data, msg->m_ts)
            │         ├─ [u_msg_prio] put_user(prio_ptr, u_msg_prio)
            │         │
            │         ├─ free_msg(msg)                      // 释放消息
            │         │
            │         └─ 跳出循环
            │
            ├─ mutex_unlock(&info->lock)
            ├─ fput(file)
            └─ 返回接收的字节数
```

## 4. 消息优先级

```
消息按优先级存储在红黑树中:

       [prio=8]
      /        \
  [prio=5]    [prio=10]     ← 最高优先级在右子树最右端
  /     \
[prio=3] [prio=5]

接收顺序: 最高优先级 → 次高优先级 → ... (同优先级 FIFO)
```

## 5. 数据结构

### 5.1 struct msg_msg（POSIX 消息体）

```c
// ipc/mqueue.c
struct msg_msg {
    struct list_head list;             /* 链表节点 */
    struct rb_node node;               /* 红黑树节点 */
    long mtype;                        /* 消息类型 */
    unsigned int msg_priority;         /* 消息优先级 */
    size_t m_ts;                       /* 消息正文长度 */
    struct msg_msgseg *next;           /* 分段链表（大消息） */
    void *security;                    /* 安全字段 */
};
```

## 6. 流程图

```
用户态调用 mq_timedreceive(mqdes, msg_ptr, len, prio, timeout)
  │
  v
do_mq_timedreceive(fd, msg_ptr, len, prio, timeout)
  │
  ├── 获取文件结构 (fget)
  ├── 获取 mqueue_inode_info
  │
  ├── mutex_lock(&info->lock)
  │
  ├── 队列空?
  │    ├── 是 → 阻塞? → wait_for_msg() → schedule_timeout()
  │    │    ├── 超时 → -EAGAIN
  │    │    └── 被唤醒 → 重试
  │    │
  │    └── 否 → msg_remove() 取出最高优先级消息
  │         ├── 检查缓冲区大小
  │         ├── copy_to_user() 拷贝消息
  │         ├── free_msg() 释放
  │         └── 返回字节数
  │
  ├── mutex_unlock(&info->lock)
  └── fput(file)
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效描述符 | mqdes 无效或不是读/读写方式打开 |
| `EMSGSIZE` | 缓冲区太小 | msg_len < 队列的 mq_msgsize |
| `EAGAIN` | 无消息 | O_NONBLOCK 或超时且队列为空 |
| `ETIMEDOUT` | 超时 | 超时时间内未收到消息 |
| `EINTR` | 信号中断 | 等待时被信号中断 |
| `EFAULT` | 地址错误 | 用户指针不可访问 |

## 8. 使用示例

```c
#include <mqueue.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main() {
    struct mq_attr attr = {
        .mq_maxmsg  = 10,
        .mq_msgsize = 256,
    };

    mqd_t mq = mq_open("/my_queue", O_CREAT | O_RDWR, 0644, &attr);
    if (mq == (mqd_t)-1) {
        perror("mq_open");
        exit(1);
    }

    char buf[256];
    unsigned int prio;

    // 阻塞接收
    printf("Waiting for message (blocking)...\n");
    ssize_t n = mq_timedreceive(mq, buf, sizeof(buf), &prio, NULL);
    if (n == -1) {
        perror("mq_timedreceive");
        exit(1);
    }
    buf[n] = '\0';
    printf("Received (prio=%u): %s\n", prio, buf);

    // 带超时的接收（5 秒超时）
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;

    printf("Waiting for message (timeout 5s)...\n");
    n = mq_timedreceive(mq, buf, sizeof(buf), &prio, &ts);
    if (n == -1) {
        perror("mq_timedreceive timeout");
    } else {
        buf[n] = '\0';
        printf("Received (prio=%u): %s\n", prio, buf);
    }

    mq_close(mq);
    mq_unlink("/my_queue");
    return 0;
}
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#posix-消息队列)
- 源码位置：`ipc/mqueue.c`
- 用户态头文件：`mqueue.h`