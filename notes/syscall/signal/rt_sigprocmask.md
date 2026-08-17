# rt_sigprocmask 系统调用分析

## 1. 概述

`rt_sigprocmask()` 用于检查和修改当前线程的信号掩码（blocked signal mask）。信号掩码定义了哪些信号当前被阻塞，不会递送给进程。它是 POSIX 标准的 `sigprocmask()` 在 Linux 上的实现。

**原型：**

```c
SYSCALL_DEFINE4(rt_sigprocmask, int, how,
                sigset_t __user *, nset,
                sigset_t __user *, oset,
                size_t, sigsetsize)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| how | int | 操作方式：SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK |
| nset | sigset_t* | 新的信号掩码（可为 NULL，只查询） |
| oset | sigset_t* | 输出旧的信号掩码（可为 NULL） |
| sigsetsize | size_t | sigset_t 的大小（用于兼容性检查） |

**how 参数取值：**

| 值 | 含义 | 效果 |
|----|------|------|
| SIG_BLOCK | 添加阻塞 | 当前掩码 = 当前掩码 ∪ nset |
| SIG_UNBLOCK | 移除阻塞 | 当前掩码 = 当前掩码 - nset |
| SIG_SETMASK | 完全替换 | 当前掩码 = nset |

## 2. 使用场景

- 临界区保护：在执行关键代码段时阻塞特定信号
- 信号同步：配合 sigwait/sigsuspend 使用
- 多线程信号处理：控制不同线程的信号掩码
- 避免信号重入：在信号处理函数中阻塞同种信号（SA_NODEFER 的替代）

## 3. 函数调用链

```
rt_sigprocmask(how, nset, oset, sigsetsize)       // kernel/signal.c:3316
  │
  ├─ sigsetsize != sizeof(sigset_t) → -EINVAL
  │
  ├─ old_set = current->blocked    // 保存当前掩码
  │
  ├─ 若 nset 非空：
  │   ├─ copy_from_user(&new_set, nset, sizeof(sigset_t))
  │   ├─ sigdelsetmask(&new_set, sigmask(SIGKILL)|sigmask(SIGSTOP))  // 强制不可屏蔽
  │   └─ sigprocmask(how, &new_set, NULL)
  │        └─ 实际调用了 set_current_blocked()
  │
  └─ 若 oset 非空：
       └─ copy_to_user(oset, &old_set, sizeof(sigset_t))

sigprocmask(how, new_set, NULL)                    // kernel/signal.c
  ├─ how == SIG_BLOCK:
  │    └─ current->blocked |= *new_set
  ├─ how == SIG_UNBLOCK:
  │    └─ current->blocked &= ~*new_set
  └─ how == SIG_SETMASK:
       └─ current->blocked = *new_set
  │
  ├─ recalc_sigpending(current)    // 重新计算待处理信号
  └─ 若阻塞解除后有信号可递送：
       └─ do_signal() 在返回用户态时处理
```

## 4. 关键数据结构

```c
// 信号掩码类型（bitmap）
typedef struct {
    unsigned long sig[_NSIG_WORDS];
} sigset_t;

// task_struct 中的信号相关字段
struct task_struct {
    sigset_t blocked;              // 当前阻塞的信号集
    sigset_t saved_sigmask;        // 保存的掩码（用于 sigsuspend/ppoll 等）
    struct sigpending pending;     // 线程私有 pending 信号
    struct sighand_struct *sighand; // 信号处理函数表
};
```

## 5. 流程图

```
用户态: sigprocmask(how, &nset, &oset)
    │
    ▼
rt_sigprocmask(how, nset, oset, sigsetsize)
    │
    ├─ 检查 sigsetsize == sizeof(sigset_t)
    │
    ├─ old_set = current->blocked    // 保存旧掩码
    │
    ├─ 若 nset != NULL:
    │   ├─ 从用户空间拷贝 nset
    │   ├─ 强制清除 SIGKILL 和 SIGSTOP 位
    │   │
    │   └─ switch(how):
    │        ├─ SIG_BLOCK:     blocked |= nset
    │        ├─ SIG_UNBLOCK:   blocked &= ~nset
    │        └─ SIG_SETMASK:   blocked = nset
    │
    ├─ recalc_sigpending()     // 重新计算 pending 状态
    │
    └─ 若 oset != NULL:
         └─ 将 old_set 拷贝到用户空间
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效参数 | sigsetsize 不匹配，或 how 不是有效值 |
| EFAULT | 内存错误 | nset/oset 指针指向不可访问的内存 |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

int main(void)
{
    sigset_t new_mask, old_mask;

    /* 初始化新掩码 */
    sigemptyset(&new_mask);
    sigaddset(&new_mask, SIGINT);
    sigaddset(&new_mask, SIGQUIT);

    /* 阻塞 SIGINT 和 SIGQUIT，同时保存旧掩码 */
    if (sigprocmask(SIG_BLOCK, &new_mask, &old_mask) == -1) {
        perror("sigprocmask");
        return 1;
    }

    printf("Signals blocked. SIGINT and SIGQUIT will not be delivered.\n");
    printf("Sleeping for 3 seconds...\n");
    sleep(3);

    /* 恢复旧掩码（解除阻塞） */
    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) == -1) {
        perror("sigprocmask restore");
        return 1;
    }

    printf("Original signal mask restored.\n");
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`sigprocmask()` - 信号掩码修改核心函数
- kernel/signal.c:`recalc_sigpending()` - pending 信号重新计算