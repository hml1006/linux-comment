# mq_timedsend 系统调用分析

## 1. 概述

`mq_timedsend` 用于向 POSIX 消息队列发送一条消息。每条消息具有一个优先级，优先级高的消息会被优先接收。支持超时等待。

**原型：**

```c
SYSCALL_DEFINE5(mq_timedsend, mqd_t, mqdes, const char __user *, u_msg_ptr,
                size_t, msg_len, unsigned int, msg_prio,
                const struct __kernel_timespec __user *, u_abs_timeout)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `mqdes` | 消息队列描述符 |
| `u_msg_ptr` | 消息数据缓冲区 |
| `msg_len` | 消息长度（必须 <= 队列的 mq_msgsize） |
| `msg_prio` | 消息优先级（0 最低，最大为 `MQ_PRIO_MAX`，通常 32768） |
| `u_abs_timeout` | 绝对超时时间（NULL 表示阻塞等待） |

## 3. 函数调用链

```
mq_timedsend (系统调用入口)
  │
  ├─ [timeout] copy_from_user 并转换 timespec64
  │
  └─ do_mq_timedsend(mqdes, u_msg_ptr, msg_len, msg_prio, timeout)
       │
       └─ do_mq_timedsend(fd, u_msg_ptr, msg_len, msg_prio, timeout)
            │
            ├─ file = fget(fd)                             // 获取 file 结构
            ├─ inode = file_inode(file)
            ├─ info = MQUEUE_I(inode)                      // 获取 mqueue_inode_info
            │
            ├─ security_mq_msg_send(&info->vfs_inode, ...)  // LSM 检查
            │
            ├─ 参数检查:
            │    ├─ msg_len > info->q_msgsize → -EMSGSIZE
            │    └─ msg_prio > MQ_PRIO_MAX → -EINVAL
            │
            ├─ 分配消息体 msg_msg + 数据拷贝
            │    ├─ load_msg(u_msg_ptr, msg_len)
            │    └─ msg->msg_priority = msg_prio
            │
            ├─ mutex_lock(&info->lock)
            │
            ├─ 循环:
            │    ├─ [队列满]:
            │    │    ├─ qsize + msg_len > q_maxsize 或 qcount >= q_maxmsg
            │    │    ├─ [O_NONBLOCK 或 timeout 已过期] → -EAGAIN
            │    │    ├─ [阻塞] wait_for_free(info, timeout)
            │    │    │    └─ prepare_to_wait + schedule_timeout
            │    │    └─ 被唤醒后重试
            │    │
            │    └─ [队列有空闲]:
            │         ├─ msg_insert(new_msg, info)          // 插入红黑树
            │         │    └─ 按优先级插入 msg_tree
            │         │
            │         ├─ [队列从空→非空] 触发通知:
            │         │    └─ mq_notify 注册的通知
            │         │
            │         ├─ wake_up(&info->wait_q)             // 唤醒等待的接收者
            │         │
            │         └─ 跳出循环
            │
            ├─ mutex_unlock(&info->lock)
            └─ fput(file)
```

## 4. 消息排序

```
消息按优先级插入红黑树，相同优先级按时间顺序:

  msg_insert(msg, info):
    ├─ 在 msg_tree 中按优先级查找位置
    ├─ 优先级高 → 右子树
    ├─ 优先级低 → 左子树
    └─ 同优先级 → 在 rb_subtree_last 后添加（FIFO）

接收时从最高优先级（最右端）开始取
```

## 5. 流程图

```
用户态调用 mq_timedsend(mqdes, msg_ptr, len, prio, timeout)
  │
  v
do_mq_timedsend(fd, msg_ptr, len, prio, timeout)
  │
  ├── 获取文件结构 (fget)
  ├── 获取 mqueue_inode_info
  │
  ├── 参数检查 (长度/优先级)
  │
  ├── load_msg() 从用户空间拷贝消息
  │
  ├── mutex_lock(&info->lock)
  │
  ├── 队列满?
  │    ├── 是 → 阻塞? → wait_for_free() → schedule_timeout()
  │    │    ├── 超时 → -EAGAIN
  │    │    └── 被唤醒 → 重试
  │    │
  │    └── 否:
  │         ├── msg_insert() 插入红黑树
  │         ├── [空→非空] 触发通知
  │         ├── wake_up() 唤醒接收者
  │         └── 返回 0
  │
  ├── mutex_unlock(&info->lock)
  └── fput(file)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效描述符 | mqdes 无效或不是写/读写方式打开 |
| `EMSGSIZE` | 消息太长 | msg_len > 队列的 mq_msgsize |
| `EINVAL` | 无效参数 | msg_prio > MQ_PRIO_MAX 或超时时间无效 |
| `EAGAIN` | 队列满 | O_NONBLOCK 或超时且队列满 |
| `ETIMEDOUT` | 超时 | 超时时间内未发送成功 |
| `EINTR` | 信号中断 | 等待时被信号中断 |
| `EFAULT` | 地址错误 | 用户指针不可访问 |
| `ENOMEM` | 内存不足 | 无法分配消息体 |

## 7. 使用示例

```c
#include <mqueue.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    // 发送优先级为 1 的消息
    const char *msg = "Low priority message";
    if (mq_timedsend(mq, msg, strlen(msg) + 1, 1, NULL) == -1) {
        perror("mq_timedsend");
        exit(1);
    }
    printf("Sent: %s (prio=1)\n", msg);

    // 发送优先级为 10 的高优先级消息
    msg = "High priority message";
    if (mq_timedsend(mq, msg, strlen(msg) + 1, 10, NULL) == -1) {
        perror("mq_timedsend");
        exit(1);
    }
    printf("Sent: %s (prio=10)\n", msg);

    // 带超时的发送
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;

    msg = "Timeout message";
    if (mq_timedsend(mq, msg, strlen(msg) + 1, 5, &ts) == -1) {
        perror("mq_timedsend timeout");
    } else {
        printf("Sent: %s (prio=5)\n", msg);
    }

    mq_close(mq);
    mq_unlink("/my_queue");
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#posix-消息队列)
- 源码位置：`ipc/mqueue.c`
- 用户态头文件：`mqueue.h`