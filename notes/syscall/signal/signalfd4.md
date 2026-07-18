# signalfd4 系统调用分析

## 1. 概述

`signalfd4()` 创建一个文件描述符，用于接收发往本进程的指定信号。通过该文件描述符，进程可以使用 `read()`、`poll()`、`epoll` 等标准 I/O 操作来异步接收信号，而不是使用传统的信号处理函数机制。这为基于事件循环的编程模型（如 epoll 框架）提供了信号集成能力。

**原型：**

```c
SYSCALL_DEFINE4(signalfd4, int, ufd,
                sigset_t __user *, user_mask,
                size_t, sizemask, int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| ufd | int | 如果是 -1 则创建新 fd，否则更新已有 signalfd 的掩码 |
| user_mask | sigset_t* | 要接收的信号集 |
| sizemask | size_t | sigset_t 的大小 |
| flags | int | 标志位（SFD_CLOEXEC / SFD_NONBLOCK） |

## 2. 使用场景

- 事件驱动编程：与 epoll 结合，统一处理 I/O 事件和信号
- 线程化信号处理：指定线程接收特定信号，避免传统信号处理的异步问题
- 避免信号处理函数的可重入问题
- 使用 `read()` 获取信号附带信息（siginfo_t）

## 3. 函数调用链

```
signalfd4(ufd, user_mask, sizemask, flags)         // fs/signalfd.c:299
  │
  ├─ sizemask != sizeof(sigset_t) → -EINVAL
  ├─ copy_from_user(&mask, user_mask, sizeof(mask))
  │
  └─ do_signalfd4(ufd, &mask, flags)
       │
       ├─ 若 ufd == -1：创建新的 signalfd
       │   └─ signalfd_file_create(current->sighand, &mask, flags)
       │        ├─ ctx = kzalloc(sizeof(*ctx))
       │        ├─ ctx->sigmask = mask
       │        ├─ ctx->sighand = sighand
       │        ├─ anon_inode_getfile("[signalfd]", &signalfd_fops, ctx, ...)
       │        └─ fd_install / fd_publish
       │
       └─ 若 ufd != -1：更新已有的 signalfd
            └─ signalfd_file_overwrite(ufd, &mask)
                 ├─ 获取 fd_file
                 ├─ 验证是 signalfd（f_op == &signalfd_fops）
                 ├─ 更新 ctx->sigmask = mask
                 └─ 清理已为此 signalfd 入队的信号

信号递送时的通知：
  signalfd 通过 signalfd_notify() 集成到信号递送路径：
  get_signal(&ksig)
    └─ signalfd_notify(current, sig)
         └─ 若 sig 在 signalfd 的 mask 中
              └─ 信号被 signalfd 消费，不进入传统处理流程

read 操作：
  signalfd_read(file, buf, count, ppos)
    └─ signalfd_dequeue(ctx, &info, nonblock)
         ├─ 从 pending 队列取出匹配的信号
         └─ 组装 struct signalfd_siginfo 返回给用户

poll 操作：
  signalfd_poll(file, pt)
    └─ 若有待处理信号 → POLLIN
```

## 4. 关键数据结构

```c
// signalfd 上下文（每个 signalfd 实例一个）
struct signalfd_ctx {
    sigset_t sigmask;                 // 要接收的信号掩码
    struct sighand_struct *sighand;   // 指向进程的信号处理表
};

// 用户态读取的信号信息结构
struct signalfd_siginfo {
    __u32 ssi_signo;      // 信号编号
    __s32 ssi_errno;      // 错误号
    __s32 ssi_code;       // 信号来源码
    __u32 ssi_pid;        // 发送进程 PID
    __u32 ssi_uid;        // 发送进程 UID
    __s32 ssi_fd;         // 文件描述符（SIGPOLL）
    __u32 ssi_tid;        // 线程 ID
    __u32 ssi_band;       // 事件（SIGPOLL）
    __u32 ssi_overrun;    // 定时器溢出计数
    __u32 ssi_trapno;     // 陷阱号
    __s32 ssi_status;     // 退出状态/信号
    __s32 ssi_int;        // 实时信号整数值（si_value.sival_int）
    __u64 ssi_ptr;        // 实时信号指针值（si_value.sival_ptr）
    __u64 ssi_utime;      // 用户态时间
    __u64 ssi_stime;      // 内核态时间
    __u64 ssi_addr;       // 错误地址
    __u16 ssi_addr_lsb;   // 地址 LSB（SIGBUS/SIGSEGV）
    __u16 __pad2;         // 填充
    __s32 ssi_syscall;    // 系统调用号（SIGSYS）
    __u64 ssi_call_addr;  // 系统调用地址（SIGSYS）
};

// file_operations
static const struct file_operations signalfd_fops = {
    .read = signalfd_read,
    .poll = signalfd_poll,
    .release = signalfd_release,
};
```

## 5. 流程图

```
创建 signalfd:
  signalfd4(-1, &mask, 0)
    │
    └─ signalfd_file_create()
         ├─ 分配 signalfd_ctx
         ├─ 设置信号掩码
         └─ 创建匿名文件 → 返回 fd

信号递送通知:
  信号到达进程
    │
    └─ get_signal()
         │
         └─ signalfd_notify(current, sig)
              │
              ├─ 遍历 current->sighand 中的 signalfd
              ├─ 检查 sig 是否在 signalfd 的 mask 中
              └─ 若是 → 将信号入队到 signalfd 的等待队列
                   并标记为已处理（不进入传统信号流程）

读取信号:
  read(fd, &siginfo, sizeof(siginfo))
    │
    └─ signalfd_read()
         └─ signalfd_dequeue()
              ├─ 锁定 sighand->siglock
              ├─ 从 pending 队列取出匹配信号
              ├─ 组装 signalfd_siginfo
              └─ 返回给用户

与 epoll 集成:
  epoll_wait(epfd, events, maxevents, timeout)
    │
    └─ signalfd_poll()
         └─ 若有待处理信号 → 返回 POLLIN
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效参数 | sizemask 不等于 sizeof(sigset_t) |
| EMFILE | 文件描述符耗尽 | 进程已打开过多文件 |
| ENOMEM | 内存不足 | 无法分配 signalfd_ctx |
| EBADF | 无效 fd | ufd 不是有效的文件描述符 |
| EFAULT | 内存错误 | user_mask 指针指向不可访问地址 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

int main(void)
{
    sigset_t mask;
    int sfd;
    struct signalfd_siginfo fdsi;

    /* 阻塞 SIGINT 和 SIGQUIT */
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    /* 创建 signalfd */
    sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (sfd == -1) {
        perror("signalfd");
        return 1;
    }

    printf("Waiting for signals (send SIGINT/Ctrl+C or SIGQUIT/Ctrl+\\)...\n");

    while (1) {
        ssize_t s = read(sfd, &fdsi, sizeof(fdsi));
        if (s != sizeof(fdsi)) {
            perror("read");
            break;
        }

        printf("Got signal %d", fdsi.ssi_signo);
        if (fdsi.ssi_signo == SIGINT) {
            printf(" (SIGINT), breaking\n");
            break;
        } else if (fdsi.ssi_signo == SIGQUIT) {
            printf(" (SIGQUIT)\n");
        }
    }

    close(sfd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- fs/signalfd.c - signalfd 完整实现
- include/uapi/linux/signalfd.h - signalfd_siginfo 定义