# pselect6 系统调用分析

> 所属分类：事件通知（I/O 多路复用） | [ARM64 系统调用表](../arm64-syscall-table.md#14-事件通知-epoll)

## 1. 概述

`pselect6` 是 `select` 的 POSIX 版本，在等待 fd 就绪的同时以**原子方式**替换信号掩码，消除"先检查信号、再进入 select"之间的竞态窗口。

与 `select` 的差异：

- 超时为 `struct timespec`（纳秒精度）且是**绝对截止时间**；
- 新增信号掩码参数；
- 多数架构最多 6 个系统调用参数，因此 ARM64 上第 6 个参数是指向 `struct sigset_argpack { sigset_t *p; size_t size; }` 的指针。

ARM64 系统调用号：`__NR_pselect6 = 72`。

## 2. 函数原型与参数

用户空间原型：

```c
#include <sys/select.h>

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            const struct timespec *timeout, const sigset_t *sigmask);
```

内核入口（`fs/select.c:800`）：

```c
SYSCALL_DEFINE6(pselect6, int, n,
                fd_set __user *, inp, fd_set __user *, outp, fd_set __user *, exp,
                struct __kernel_timespec __user *, tsp, void __user *, sig)
```

| 参数 | 说明 |
|------|------|
| `n` | 需检查的 fd 数量，内核截断为 `fdt->max_fds` |
| `inp` / `outp` / `exp` | 读/写/异常事件集合，`NULL` 表示不关心 |
| `tsp` | 绝对超时（`timespec64`），`NULL` 表示无限等待 |
| `sig` | 指向 `struct sigset_argpack` 的用户指针，可为 `NULL` |

第六个参数的结构体（`fs/select.c:780`）：

```c
struct sigset_argpack {
    sigset_t __user *p;
    size_t size;
};
```

由 `get_sigset_argpack()`（`fs/select.c:785`）一次性读入，再把 `x.p`、`x.size` 传给 `do_pselect()`。

## 3. 内核实现路径

```
__arm64_sys_pselect6()
  └─ get_sigset_argpack(&x, sig)                    // 读取 {sigmask, size}
  └─ do_pselect(..., PT_TIMESPEC)                   // fs/select.c:739
       ├─ get_timespec64(&ts, tsp)
       ├─ poll_select_set_timeout(&end_time, ...)   // 转绝对截止时间
       ├─ set_user_sigmask(sigmask, sigsetsize)     // kernel/signal.c:3282
       │    ├─ sigsetsize != sizeof(sigset_t) → -EINVAL
       │    ├─ set_restore_sigmask()
       │    ├─ current->saved_sigmask = current->blocked
       │    └─ set_current_blocked(&kmask)
       ├─ core_sys_select(n, inp, outp, exp, to)    // fs/select.c:621
       │    ├─ n 截断到 max_fds，分配 6 份位图
       │    ├─ get_fd_set() ×3
       │    ├─ do_select(n, &fds, end_time)         // fs/select.c:483
       │    │    ├─ max_select_fd() → -EBADF
       │    │    ├─ poll_initwait(&table)
       │    │    ├─ select_poll_one() → vfs_poll()  // fs/select.c:465
       │    │    ├─ signal_pending() → 退出循环
       │    │    └─ poll_schedule_timeout(..., TASK_INTERRUPTIBLE, to, slack)
       │    ├─ set_fd_set() ×3
       │    └─ 无事件且有信号 → -ERESTARTNOHAND
       └─ poll_select_finish(&end_time, tsp, PT_TIMESPEC, ret)  // fs/select.c:295
            ├─ restore_saved_sigmask_unless(ret == -ERESTARTNOHAND)
            ├─ 剩余时间写回 tsp（Linux 特有）
            └─ ERESTARTNOHAND → EINTR（除非 STICKY_TIMEOUTS）
```

关键点：

1. `set_user_sigmask()` 保存旧掩码，`poll_select_finish()` 恢复；若返回 `-ERESTARTNOHAND` 则不恢复，交给信号返回路径处理。
2. `do_select()` 首次发现就绪事件后把 `wait->_qproc` 置 `NULL`，避免继续注册等待队列项。
3. 超时在进入 `do_select()` 前转换为绝对 `timespec64`，循环唤醒不会导致超时漂移。
4. `pselect6` 不设置 `restart_block`，被信号打断后返回 `EINTR`。

## 4. 核心数据结构

### 4.1 struct poll_wqueues（`include/linux/poll.h:95`）

```c
struct poll_wqueues {
    poll_table pt;
    struct poll_table_page *table;
    struct task_struct *polling_task;
    int triggered;
    int error;
    int inline_index;
    struct poll_table_entry inline_entries[N_INLINE_POLL_ENTRIES];
};
```

`poll_table`（`include/linux/poll.h:37`）包含 `_qproc`（驱动注册等待队列的回调）与 `_key`（关心的位掩码）；`poll_table_entry`（85 行）记录 `filp`、`key`、`wait`、`wait_address`。

### 4.2 fd_set_bits（`fs/select.c:371`）

```c
typedef struct {
    unsigned long *in, *out, *ex;
    unsigned long *res_in, *res_out, *res_ex;
} fd_set_bits;
```

小规模 fd 使用栈上 `SELECT_STACK_ALLOC`（256 字节）；超出时 `kvmalloc(6 * size)`。

### 4.3 事件集合宏（`fs/select.c:459`）

```c
#define POLLIN_SET  (EPOLLRDNORM | EPOLLRDBAND | EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLNVAL)
#define POLLOUT_SET (EPOLLWRBAND | EPOLLWRNORM | EPOLLOUT | EPOLLERR | EPOLLNVAL)
#define POLLEX_SET  (EPOLLPRI | EPOLLNVAL)
```

## 5. 关键流程

```
pselect6(n, &rfds, &wfds, &efds, &ts, &pack)
        │
        ├─ get_sigset_argpack() 读取 sigmask 与 size
        ├─ tsp 拷贝校验 → 绝对截止时间
        ├─ set_user_sigmask()：保存旧掩码 → 安装新掩码
        ├─ core_sys_select()
        │    ├─ n < 0 → EINVAL；n 截断到 max_fds
        │    ├─ do_select()
        │    │    ├─ 有事件 → 填充 res_* 位图并返回事件数
        │    │    ├─ 无事件 → poll_schedule_timeout() 睡眠
        │    │    └─ 信号 → -ERESTARTNOHAND
        │    └─ 结果位图写回用户 fd_set
        └─ poll_select_finish()
             ├─ 恢复信号掩码（EINTR 除外）
             ├─ 写回剩余时间
             └─ -ERESTARTNOHAND → -EINTR
```

## 6. 返回值与错误码

成功返回就绪 fd 的总数（读/写/异常分别计数）；超时返回 0。

| 错误码 | 触发条件 |
|--------|----------|
| `-EBADF` | fd_set 中某位对应的 fd 未打开或超出范围（`max_select_fd()`） |
| `-EINVAL` | `n` 为负；timespec 非法；`sigmask` 非空但 `sigsetsize != sizeof(sigset_t)` |
| `-EFAULT` | 读取 `sigset_argpack`、timespec、fd_set、sigmask 失败，或写回结果失败 |
| `-EINTR` | 等待期间被信号中断 |
| `-ENOMEM` | 位图分配失败 |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <sys/select.h>
#include <unistd.h>

int main(void)
{
    fd_set rfds;
    sigset_t block, orig;
    struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };

    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &orig);

    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);

    /* 等待期间原子地使用 orig（解除 SIGUSR1 阻塞） */
    int ret = pselect(STDIN_FILENO + 1, &rfds, NULL, NULL, &ts, &orig);
    if (ret < 0) {
        perror("pselect");
    } else if (ret == 0) {
        printf("timeout\n");
    } else if (FD_ISSET(STDIN_FILENO, &rfds)) {
        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        printf("read %zd bytes\n", n);
    }

    sigprocmask(SIG_SETMASK, &orig, NULL);
    return 0;
}
```

直接调用 `syscall(__NR_pselect6, ...)` 时需自行构造 `struct sigset_argpack`；glibc 的 `pselect()` 已完成封装。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `fs/select.c` | `SYSCALL_DEFINE6(pselect6)`、`do_pselect()`、`core_sys_select()`、`do_select()` |
| `include/linux/poll.h` | `poll_table`、`poll_wqueues`、`poll_wait()`、`vfs_poll()` |
| `kernel/signal.c` | `set_user_sigmask()`（3282 行） |
| `include/uapi/linux/posix_types.h` | `fd_set` 用户 ABI |
| `arch/arm64/include/generated/uapi/asm/unistd_64.h` | `__NR_pselect6 = 72` |
