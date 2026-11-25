# restart_syscall 系统调用分析

## 1. 概述

`restart_syscall` 是一个特殊的系统调用，用于在执行可中断等待（interruptible sleep）的系统调用被信号中断后，通过 `ERESTART_RESTARTBLOCK` 机制重新启动该系统调用。它充当了一个"重新调用"的入口点，使得被中断的系统调用可以通过一个不同的系统调用号重新启动。

**原型：**

```c
SYSCALL_DEFINE0(restart_syscall)
{
    struct restart_block *restart = &current->restart_block;
    return restart->fn(restart);
}
```

### 系统调用编号

- 通用编号：128（`__NR_restart_syscall`）
- 定义在 `include/uapi/asm-generic/unistd.h`

## 2. 原理

### ERESTART 机制

当系统调用进入可中断等待（如 `wait_event_interruptible`）并被信号中断时，内核不直接返回 `-EINTR`，而是返回一个内部使用的错误码：

| 错误码 | 含义 | 信号处理后的行为 |
|--|--|--|
| `-ERESTARTNOHAND` | 无处理器的系统调用重启 | 如果有信号处理器，返回 `-EINTR`；否则重启 |
| `-ERESTARTSYS` | 可重启的系统调用 | 如果信号处理器设置了 SA_RESTART，则重启；否则返回 `-EINTR` |
| `-ERESTARTNOINTR` | 不可被信号中断的重启 | 总是重启系统调用 |
| `-ERESTART_RESTARTBLOCK` | 通过 restart_syscall 重启 | 使用 `restart_block.fn` 重新启动 |

### restart_block 结构

```c
// include/linux/restart_block.h
struct restart_block {
    long (*fn)(struct restart_block *);
    union {
        // futex 相关
        struct {
            u32 __user *uaddr;
            u32 val;
            u32 flags;
            u32 bitset;
            u64 time;
            struct timespec64 __user *rmtp;
        } futex;
        // nanosleep 相关
        struct {
            struct timespec64 __user *rmtp;
            struct timespec64 expires;
        } nanosleep;
        // poll 相关
        struct {
            struct pollfd __user *ufds;
            int nfds;
            int has_timeout;
            unsigned long tv_sec;
            unsigned long tv_nsec;
        } poll;
        // 其他...
    };
};
```

## 3. 完整实现

```c
// kernel/signal.c
SYSCALL_DEFINE0(restart_syscall)
{
    struct restart_block *restart = &current->restart_block;
    return restart->fn(restart);
}

// 默认的 fn，返回 -EINTR
long do_no_restart_syscall(struct restart_block *param)
{
    return -EINTR;
}
```

## 4. 调用链分析

### 信号处理时的重启决策

```
信号到达时，正在执行系统调用 A
  │
  ▼
arch_do_signal_or_restart(regs)           // 架构相关的信号处理
  │
  ├─ 有信号处理器:
  │    │
  │    └─ 根据错误码决定:
  │         ├─ ERESTARTNOHAND → 返回 -EINTR (不重启)
  │         ├─ ERESTARTSYS + 无 SA_RESTART → 返回 -EINTR
  │         ├─ ERESTARTSYS + 有 SA_RESTART → 重启系统调用 A
  │         ├─ ERESTARTNOINTR → 重启系统调用 A
  │         └─ ERESTART_RESTARTBLOCK → 设置 restart_syscall
  │
  └─ 无信号处理器:
       │
       ├─ ERESTARTNOHAND / ERESTARTSYS / ERESTARTNOINTR:
       │    └─ 重启系统调用 A
       │
       └─ ERESTART_RESTARTBLOCK:
            └─ 设置 regs->ax = __NR_restart_syscall
                 └─ 下次返回用户态后执行 restart_syscall
```

### restart_syscall 执行流程

```
restart_syscall()
  │
  └─ current->restart_block.fn(restart_block)
       │
       ├─ futex_wait_restart()     // futex 等待
       ├─ nanosleep_restart()      // 时钟休眠
       ├─ poll_restart()           // 轮询
       └─ 其他...
```

## 5. 流程图

```
系统调用 A 进入内核
  │
  ▼
内核执行系统调用 A
  │
  ▼
进入可中断等待 (如 schedule())
  │
  ▼
信号到达 → 唤醒进程
  │
  ▼
返回 -ERESTART_RESTARTBLOCK
  │
  ▼
用户态返回前，信号处理代码检查
  │
  ├─ 有信号处理器 → 处理信号
  │    └─ 设置 regs->ax = __NR_restart_syscall
  │
  └─ 无信号处理器 → 同样设置 restart_syscall
       │
       ▼
  返回用户态 → 执行 restart_syscall
       │
       ▼
  restart_syscall()
       │
       ▼
  restart_block->fn(restart)
       │
       ├─ 重新执行被中断的操作
       │    (如 futex_wait、nanosleep 等)
       │
       └─ 返回最终结果
```

## 6. 架构相关实现

### x86_64

```c
// arch/x86/kernel/signal.c
void arch_do_signal_or_restart(struct pt_regs *regs)
{
    // ...
    if (syscall_get_nr(current, regs) != -1) {
        switch (syscall_get_error(current, regs)) {
        case -ERESTART_RESTARTBLOCK:
            regs->ax = get_nr_restart_syscall(regs);
            regs->ip -= 2;  // 重新执行 syscall 指令
            break;
        // ...
        }
    }
}
```

### ARM64

```c
// arch/arm64/kernel/signal.c
// 在信号处理过程中
if (syscall && regs->pc == restart_addr) {
    if (retval == -ERESTART_RESTARTBLOCK)
        setup_restart_syscall(regs);
    // ...
}
```

### RISC-V / CSKY

```c
// arch/csky/kernel/signal.c
case -ERESTART_RESTARTBLOCK:
    regs->a0 = regs->orig_a0;
    regs_syscallid(regs) = __NR_restart_syscall;
    regs->pc -= TRAP0_SIZE;
    break;
```

## 7. 使用场景

- **futex 等待**：`futex(FUTEX_WAIT)` 被信号中断后，通过 `restart_syscall` 重新等待
- **nanosleep**：高精度休眠被信号中断后的重新休眠
- **poll/select**：多路复用 I/O 等待被信号中断后的重新等待
- **pause**：等待信号的系统调用

### 典型示例：futex_wait

```c
// kernel/futex/waitwake.c
static long futex_wait_restart(struct restart_block *restart)
{
    u32 __user *uaddr = restart->futex.uaddr;
    ktime_t *tp = NULL;

    if (restart->futex.flags & FLAGS_HAS_TIMEOUT)
        tp = &restart->futex.time;

    restart->fn = do_no_restart_syscall;

    return (long)futex_wait(uaddr, restart->futex.flags,
                            restart->futex.val, tp, restart->futex.bitset);
}
```

## 8. 关键数据结构

```c
// include/linux/restart_block.h
struct restart_block {
    long (*fn)(struct restart_block *);
    union {
        // Futex 重启上下文
        struct {
            u32 __user *uaddr;
            u32 val;
            u32 flags;
            u32 bitset;
            u64 time;
            struct timespec64 __user *rmtp;
        } futex;

        // Nanosleep 重启上下文
        struct {
            struct timespec64 __user *rmtp;
            struct timespec64 expires;
        } nanosleep;

        // Poll 重启上下文
        struct {
            struct pollfd __user *ufds;
            int nfds;
            int has_timeout;
            unsigned long tv_sec;
            unsigned long tv_nsec;
        } poll;
    };
};
```

## 9. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>

void handler(int sig) {
    write(STDOUT_FILENO, "Signal received\n", 16);
}

int main(void)
{
    struct timespec ts = {.tv_sec = 5, .tv_nsec = 0};
    
    signal(SIGALRM, handler);
    
    alarm(1);  // 1 秒后发送 SIGALRM
    
    // nanosleep 被 SIGALRM 中断后，内核自动通过
    // restart_syscall 重新启动休眠
    // 但如果我们设置了 SA_RESTART... 实际上 nanosleep
    // 总是返回 -ERESTART_RESTARTBLOCK 并被重启
    
    struct timespec rem;
    int ret = nanosleep(&ts, &rem);
    if (ret == -1) {
        perror("nanosleep");
    }
    
    return 0;
}
```

## 10. 源码位置

| 文件 | 说明 |
|--|--|
| [kernel/signal.c](/home/louis/code/linux/kernel/signal.c) | restart_syscall 实现 |
| [include/linux/restart_block.h](/home/louis/code/linux/include/linux/restart_block.h) | restart_block 结构定义 |
| [include/uapi/asm-generic/unistd.h](/home/louis/code/linux/include/uapi/asm-generic/unistd.h) | 系统调用编号定义 |
| [arch/x86/kernel/signal.c](/home/louis/code/linux/arch/x86/kernel/signal.c) | x86 信号处理重启逻辑 |
| [arch/arm64/kernel/signal.c](/home/louis/code/linux/arch/arm64/kernel/signal.c) | ARM64 信号处理重启逻辑 |
| [arch/csky/kernel/signal.c](/home/louis/code/linux/arch/csky/kernel/signal.c) | CSKY 信号处理重启逻辑 |
| [kernel/futex/waitwake.c](/home/louis/code/linux/kernel/futex/waitwake.c) | futex_wait_restart 示例 |