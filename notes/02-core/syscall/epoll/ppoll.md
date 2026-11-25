# ppoll 系统调用分析

> 所属分类：事件通知（I/O 多路复用） | [ARM64 系统调用表](../arm64-syscall-table.md#14-事件通知-epoll)

## 1. 概述

`ppoll` 是 `poll` 的信号安全版本：它在开始等待前以原子方式替换信号掩码，并在返回时恢复原掩码，从而消除"检查信号 → 进入 poll"之间的竞态窗口，同时对超时提供纳秒精度。

与 `poll` 的主要差异：

- 超时参数为 `struct timespec`（相对时间，纳秒精度），`poll` 使用毫秒；
- 增加信号掩码参数；
- 内核不会为 `ppoll` 设置 `restart_block`（不同于 `poll`），因为需要恢复信号掩码；
- `tsp` 在返回时被写回剩余时间（Linux 的实现细节，POSIX 不要求）。

ARM64 系统调用号：`__NR_ppoll = 73`。

## 2. 函数原型与参数

用户空间原型：

```c
#include <poll.h>

int ppoll(struct pollfd *fds, nfds_t nfds,
          const struct timespec *tmo_p, const sigset_t *sigmask);
```

内核入口（`fs/select.c:1092`）：

```c
SYSCALL_DEFINE5(ppoll, struct pollfd __user *, ufds, unsigned int, nfds,
                struct __kernel_timespec __user *, tsp,
                const sigset_t __user *, sigmask, size_t, sigsetsize)
```

| 参数 | 说明 |
|------|------|
| `ufds` | `struct pollfd` 数组，包含 fd、events（输入）、revents（输出） |
| `nfds` | 数组元素个数，上限受 `RLIMIT_NOFILE` 约束 |
| `tsp` | 相对超时；`NULL` 表示无限等待；0 表示只轮询一次 |
| `sigmask` | 等待期间使用的信号掩码，`NULL` 表示不改变 |
| `sigsetsize` | 必须等于 `sizeof(sigset_t)` |

`struct pollfd` 定义于 `<poll.h>`：

```c
struct pollfd {
    int   fd;
    short events;   /* 用户关心的事件 */
    short revents;  /* 内核返回的事件 */
};
```

内核的 `events` 是用户态 16 位 `POLL*` 编码，`do_pollfd()` 通过 `demangle_poll()` 转成 32 位 `__poll_t`（`EPOLL*`）后交给 `vfs_poll()`，返回时再用 `mangle_poll()` 转回。

## 3. 内核实现路径

```
__arm64_sys_ppoll()
  └─ get_timespec64(&ts, tsp)                     // 相对超时
  └─ poll_select_set_timeout(&end_time, ts.tv_sec, ts.tv_nsec)
  │    └─ 转为绝对截止时间（ktime_get_ts64 + add_safe）
  └─ set_user_sigmask(sigmask, sigsetsize)        // kernel/signal.c:3282
  └─ do_sys_poll(ufds, nfds, to)                  // fs/select.c:974
       ├─ nfds > rlimit(RLIMIT_NOFILE) → -EINVAL
       ├─ 把用户 pollfd 分批拷贝到：
       │    ├─ 栈上 stack_pps（POLL_STACK_ALLOC）
       │    └─ 页大小分块 kmalloc_flex()（POLLFD_PER_PAGE）
       ├─ poll_initwait(&table)
       ├─ do_poll(head, &table, end_time)         // fs/select.c:883
       │    ├─ 超时为 0 → wait->_qproc = NULL; timed_out = 1
       │    ├─ 第一轮：对每个 pollfd 调 do_pollfd()
       │    │      ├─ fd < 0 → revents = 0
       │    │      ├─ 无效 fd → EPOLLNVAL
       │    │      └─ vfs_poll() 并 & filter
       │    ├─ 需要等待时 pt->_qproc = NULL，避免重复注册
       │    ├─ signal_pending() → -ERESTARTNOHAND
       │    └─ poll_schedule_timeout(wait, TASK_INTERRUPTIBLE, to, slack)
       ├─ poll_freewait(&table)
       └─ 把每个 pollfd->revents 写回用户空间（unsafe_put_user）
  └─ poll_select_finish(&end_time, tsp, PT_TIMESPEC, ret)
       ├─ restore_saved_sigmask_unless(ret == -ERESTARTNOHAND)
       ├─ 写回剩余时间到 tsp
       └─ -ERESTARTNOHAND → -EINTR
```

细节说明：

1. `poll_schedule_timeout()` 使用 `TASK_INTERRUPTIBLE`，因此可被信号唤醒；`select_estimate_accuracy()` 根据超时长度计算 timer slack，避免高精度定时器带来的唤醒开销。
2. `ppoll` 不设置 `restart_block`，所以被信号中断后返回 `EINTR`；而 `poll` 可以通过 `restart_block->poll` 与 `do_restart_poll()` 自动重启（`fs/select.c:1042`）。
3. `do_pollfd()` 使用 `CLASS(fd, f)` 引用计数保护，避免在 `vfs_poll()` 期间 fd 被关闭。

## 4. 核心数据结构

### 4.1 struct poll_list（`fs/select.c:845`）

```c
struct poll_list {
    struct poll_list *next;
    unsigned int len;
    struct pollfd entries[] __counted_by(len);
};
```

用户数组被分成多个 `poll_list`，栈上放第一块，剩余按页分配，避免为大 `nfds` 一次性 `kmalloc`。

### 4.2 struct poll_wqueues（`include/linux/poll.h:95`）

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

`poll_table`（`include/linux/poll.h:37`）：

```c
typedef struct poll_table_struct {
    poll_queue_proc _qproc;
    __poll_t _key;
} poll_table;
```

每个驱动的 `poll` 回调通过 `poll_wait(file, wait_address, pt)` 把自身等待队列挂到 `pt->_qproc` 上；`poll_wait()` 内部还会执行 `smp_mb()`，与 `wq_has_sleeper()` 配对，防止"先检查后睡眠"被重排。

`mangle_poll()`/`demangle_poll()` 在 16 位 `POLL*` 与 32 位 `EPOLL*` 之间转换；`DEFAULT_POLLMASK` 为 `EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM`，文件没有 `poll` 操作时由 `vfs_poll()` 直接返回它。

## 5. 关键流程

```
ppoll(fds, nfds, &ts, &mask)
  ├─ 校验 nfds ≤ RLIMIT_NOFILE
  ├─ timespec → 绝对截止时间
  ├─ set_user_sigmask()：保存 blocked → 安装新掩码
  ├─ do_sys_poll()
  │    ├─ 分批拷贝 pollfd
  │    └─ do_poll()
  │         ├─ 遍历 pollfd，vfs_poll() 收集 revents
  │         ├─ 有事件 → 返回事件数
  │         ├─ 无事件且未超时：注册等待队列后睡眠
  │         └─ 有信号 → -ERESTARTNOHAND
  ├─ revents 写回用户空间
  └─ poll_select_finish()：恢复掩码、写回剩余时间、EINTR 转换
```

## 6. 返回值与错误码

成功返回 `revents` 非零的 fd 个数；超时返回 0。

| 错误码 | 触发条件 |
|--------|----------|
| `-EINVAL` | `nfds > RLIMIT_NOFILE`；`timespec` 非法（负值或 `tv_nsec` 越界）；`sigmask` 非空且 `sigsetsize != sizeof(sigset_t)` |
| `-EFAULT` | 拷贝 `ufds`、`tsp` 或 `sigmask` 失败，或写回 `revents` 失败 |
| `-EINTR` | 等待期间收到信号（`-ERESTARTNOHAND` 经 `poll_select_finish()` 转换） |
| `-ENOMEM` | 为 `poll_list` 分页分配失败 |

单个 fd 的问题不会让系统调用失败，而是记录在对应元素的 `revents` 中（如 `POLLNVAL`）。

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

int main(void)
{
    struct pollfd fds[1] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
    };
    sigset_t mask, orig;
    struct timespec ts = { .tv_sec = 10, .tv_nsec = 0 };

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, &orig);   /* 先阻塞，避免竞态 */

    /* 等待期间使用 orig（解除 SIGUSR1 阻塞） */
    int ret = ppoll(fds, 1, &ts, &orig);
    if (ret < 0) {
        perror("ppoll");
    } else if (ret == 0) {
        printf("10 秒超时\n");
    } else if (fds[0].revents & POLLIN) {
        printf("stdin 可读\n");
    }

    sigprocmask(SIG_SETMASK, &orig, NULL);
    return 0;
}
```

注意：`ppoll` 会修改 `ts`，若需要在循环中重复使用，应在每次调用前重新赋值；POSIX 语义并不要求修改该参数，这是 Linux 的实现行为。glibc 的 `ppoll()` 包装不会替你隐藏这一点。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `fs/select.c` | `SYSCALL_DEFINE5(ppoll)`、`do_sys_poll()`、`do_poll()`、`do_pollfd()`、`poll_select_finish()` |
| `include/linux/poll.h` | `poll_table`、`poll_wqueues`、`poll_wait()`、`vfs_poll()`、`mangle_poll()` |
| `kernel/signal.c` | `set_user_sigmask()`（3282 行） |
| `include/uapi/asm-generic/poll.h` | `struct pollfd`、`POLL*` 常量 |
| `arch/arm64/include/generated/uapi/asm/unistd_64.h` | `__NR_ppoll = 73` |
