# mq_notify 系统调用分析

## 1. 概述

`mq_notify` 用于在 POSIX 消息队列上注册异步通知。当消息队列从空变为非空时，内核会向注册的进程发送通知。通知是单次触发的，每次收到通知后必须重新注册。

**原型：**

```c
SYSCALL_DEFINE2(mq_notify, mqd_t, mqdes,
                const struct sigevent __user *, u_notification)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `mqdes` | 消息队列描述符 |
| `u_notification` | 通知配置（`struct sigevent`），NULL 表示注销通知 |

**通知方式：**

| sigev_notify 值 | 说明 |
|-----------------|------|
| `SIGEV_NONE` | 不发送通知（占位用） |
| `SIGEV_SIGNAL` | 发送信号（如 `SIGUSR1`），通过 `sigev_signo` 指定信号号 |
| `SIGEV_THREAD` | 通过 netlink 套接字通知（内核内部创建线程） |
| `NULL` | 注销当前注册的通知 |

## 3. 函数调用链

```
mq_notify (系统调用入口)
  │
  ├─ copy_from_user(&n, u_notification, sizeof(sigevent))  // 拷贝通知配置
  │
  └─ do_mq_notify(mqdes, notification)
       │
       ├─ 参数检查:
       │    ├─ notification != NULL:
       │    │    ├─ sigev_notify ∈ {SIGEV_NONE, SIGEV_SIGNAL, SIGEV_THREAD}
       │    │    ├─ sigev_notify == SIGEV_SIGNAL → valid_signal(sigev_signo)
       │    │    └─ sigev_notify == SIGEV_THREAD:
       │    │         ├─ alloc_skb(NOTIFY_COOKIE_LEN)         // 分配通知 skb
       │    │         ├─ copy_from_user(skb->data, sigev_value.sival_ptr, ...)
       │    │         └─ netlink_getsockbyfd(sigev_signo)     // 获取 netlink 套接字
       │    │
       │    └─ notification == NULL → 注销通知
       │
       ├─ CLASS(fd, f)(mqdes)                               // 获取 fd 文件
       ├─ inode = file_inode(fd_file(f))
       ├─ info = MQUEUE_I(inode)
       │
       ├─ spin_lock(&info->lock)
       │
       ├─ 如果 info->notify_owner 存在（已有通知注册）:
       │    └─ 如果当前进程不是通知所有者 → -EBUSY
       │
       ├─ 如果 notification == NULL:
       │    ├─ 清除通知（info->notify_owner = NULL 等）
       │    └─ 返回
       │
       ├─ 如果 notification != NULL:
       │    ├─ 保存通知配置到 info->notify
       │    ├─ info->notify_owner = current
       │    │
       │    ├─ 如果队列当前非空:
       │    │    └─ 立即触发通知（signal / netlink）
       │    │
       │    └─ 如果队列为空:
       │         └─ 等待队列变为非空时触发（由 mq_timedsend 触发）
       │
       └─ spin_unlock(&info->lock)
```

## 4. 通知触发时机

```
通知触发条件:
  队列从空 → 非空

  触发流程:
  mq_timedsend() 发送消息
    └─ msg_insert() 插入消息到空队列
         └─ 检查 info->notify_owner
              ├─ 有通知注册?
              │    ├─ SIGEV_SIGNAL → send_sig(info->notify.sigev_signo, ...)
              │    ├─ SIGEV_THREAD → netlink 发送 skb
              │    └─ SIGEV_NONE → 不操作
              │
              └─ 清除通知（单次触发，需重新注册）
```

## 5. 关键数据结构

### 5.1 struct sigevent（信号事件）

```c
// include/uapi/asm-generic/siginfo.h
struct sigevent {
    int sigev_notify;                    /* 通知类型 */
    int sigev_signo;                     /* 信号编号 */
    union sigval sigev_value;            /* 传递给信号处理函数的参数 */
    union {
        int _pad[SIGEV_PAD_SIZE];
        int _tid;
        struct {
            void (*_function)(sigval_t); /* SIGEV_THREAD 的线程函数 */
            void *_attribute;            /* 线程属性 */
        } _sigev_thread;
    } _sigev_un;
};
```

### 5.2 struct mqueue_inode_info（部分字段）

```c
// ipc/mqueue.c
struct mqueue_inode_info {
    /* ... */
    struct sigevent notify;              /* 通知配置 */
    struct pid *notify_owner;            /* 通知所有者 PID */
    struct sock *notify_sock;            /* netlink 套接字（SIGEV_THREAD 时使用） */
    struct file *notify_file;            /* 通知文件 */
    /* ... */
};
```

## 6. 流程图

```
用户态调用 mq_notify(mqdes, notification)
  │
  ├── notification == NULL?
  │    └── 是 → 注销通知
  │
  └── notification != NULL?
       ├── 验证 sigev_notify 类型
       │
       ├── 获取文件描述符
       │
       ├── spin_lock(&info->lock)
       │
       ├── 已有通知?
       │    ├── 是且不是当前进程 → -EBUSY
       │    └── 否 → 注册新通知
       │
       ├── 队列当前非空?
       │    └── 是 → 立即触发通知
       │
       ├── 队列空?
       │    └── 等待下次 mq_timedsend() 触发
       │
       └── spin_unlock(&info->lock)
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效描述符 | mqdes 无效 |
| `EBUSY` | 通知已注册 | 另一个进程已在此队列上注册通知 |
| `EINVAL` | 无效参数 | sigev_notify 类型无效或信号号无效 |
| `ENOMEM` | 内存不足 | 无法分配 skb 或 netlink 套接字 |
| `EFAULT` | 地址错误 | notification 指针不可访问 |

## 8. 使用示例

```c
#include <mqueue.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

volatile sig_atomic_t got_notification = 0;

void handler(int sig) {
    got_notification = 1;
}

int main() {
    struct mq_attr attr = {
        .mq_maxmsg  = 10,
        .mq_msgsize = 256,
    };

    mqd_t mq = mq_open("/notify_queue", O_CREAT | O_RDWR, 0644, &attr);
    if (mq == (mqd_t)-1) {
        perror("mq_open");
        exit(1);
    }

    // 设置信号处理函数
    signal(SIGUSR1, handler);

    // 注册通知（SIGEV_SIGNAL 方式）
    struct sigevent sev;
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    sev.sigev_value.sival_ptr = &mq;

    if (mq_notify(mq, &sev) == -1) {
        perror("mq_notify");
        exit(1);
    }

    printf("Waiting for notification...\n");

    // 等待通知（另一个进程发送消息后触发）
    while (!got_notification) {
        pause();
    }

    printf("Got notification! Receiving message...\n");

    char buf[256];
    unsigned int prio;
    ssize_t n = mq_receive(mq, buf, sizeof(buf), &prio);
    if (n > 0) {
        buf[n] = '\0';
        printf("Received: %s (prio=%u)\n", buf, prio);
    }

    // 通知是单次的，需要重新注册
    got_notification = 0;
    mq_notify(mq, &sev);

    mq_close(mq);
    mq_unlink("/notify_queue");
    return 0;
}
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#posix-消息队列)
- 源码位置：`ipc/mqueue.c`
- 用户态头文件：`mqueue.h`、`signal.h`