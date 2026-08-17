# rt_sigpending 系统调用分析

## 1. 概述

`rt_sigpending()` 用于检查当前进程有哪些信号处于待处理（pending）状态，即已经被发送但尚未递送给进程的信号。这些信号包括线程私有 pending 和线程组共享 pending 中当前被阻塞的信号。

**原型：**

```c
SYSCALL_DEFINE2(rt_sigpending, sigset_t __user *, uset, size_t, sigsetsize)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| uset | sigset_t* | 输出参数，返回待处理信号集 |
| sigsetsize | size_t | sigset_t 的大小（用于兼容性检查） |

## 2. 使用场景

- 查询当前被阻塞的待处理信号
- 多线程编程中检查线程特有信号
- 实现信号感知的循环等待（如 sigwaitinfo 的自定义实现）
- 调试信号状态

## 3. 函数调用链

```
rt_sigpending(uset, sigsetsize)                    // kernel/signal.c:3388
  │
  ├─ sigsetsize > sizeof(*uset) → -EINVAL
  │
  └─ do_sigpending(&set)
       │
       ├─ spin_lock_irq(&current->sighand->siglock)
       │
       ├─ sigorsets(&set,                       // 合并 pending 信号
       │     &current->pending.signal,          // 线程私有 pending
       │     &current->signal->shared_pending.signal)  // 共享 pending
       │
       └─ spin_unlock_irq(&current->sighand->siglock)
       │
       └─ sigandsets(&set,                     // 只返回被阻塞的 pending 信号
             &current->blocked, &set)
       │
       └─ copy_to_user(uset, &set, sigsetsize)  // 结果返回用户空间
```

## 4. 关键数据结构

```c
// 信号集类型（bitmap）
typedef struct {
    unsigned long sig[_NSIG_WORDS];  // 每个 bit 对应一个信号
} sigset_t;

// 信号待处理结构
struct sigpending {
    struct list_head list;        // sigqueue 链表
    sigset_t signal;              // 待处理信号位图
};

// 每个 task_struct 包含两个 pending 队列：
struct task_struct {
    struct sigpending pending;               // 线程私有 pending
    struct signal_struct *signal;            // 指向共享信号结构
    // ...
};

struct signal_struct {
    struct sigpending shared_pending;        // 线程组共享 pending
    // ...
};
```

## 5. 流程图

```
用户态: rt_sigpending(&set, sizeof(sigset_t))
    │
    ▼
内核: SYSCALL_DEFINE2(rt_sigpending)
    │
    ├─ 检查 sigsetsize 有效性
    │
    └─ do_sigpending(&set)
         │
         ├─ spin_lock_irq(&sighand->siglock)
         │
         ├─ 线程私有 pending:  ─┐
         │   current->pending   │
         │                      ├─ sigorsets() → 合并位图
         ├─ 共享 pending: ──────┘
         │   signal->shared_pending
         │
         └─ spin_unlock_irq(&sighand->siglock)
         │
         ├─ sigandsets() 过滤：只保留被 blocked 的信号
         │   (即已经到达但被阻塞的才算 pending)
         │
         └─ copy_to_user() 返回给用户
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效参数 | sigsetsize 大于 sigset_t 的实际大小 |
| EFAULT | 内存错误 | uset 指针指向不可访问的用户空间地址 |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

int main(void)
{
    sigset_t pending_set;
    sigset_t block_set;

    /* 阻塞 SIGINT */
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGINT);
    sigprocmask(SIG_BLOCK, &block_set, NULL);

    /* 给自己发送 SIGINT（此时被阻塞） */
    raise(SIGINT);

    /* 查询待处理信号 */
    if (sigpending(&pending_set) == 0) {
        if (sigismember(&pending_set, SIGINT))
            printf("SIGINT is pending (blocked)\n");
        else
            printf("SIGINT is not pending\n");
    }

    /* 解除阻塞，信号会被递送 */
    sigprocmask(SIG_UNBLOCK, &block_set, NULL);

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`do_sigpending()` - 待处理信号查询核心函数
- kernel/signal.c - 信号处理相关实现