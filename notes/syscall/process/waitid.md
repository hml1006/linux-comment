# waitid 系统调用分析

## 1. 概述

`waitid` 是 Linux 提供的现代化进程等待系统调用，与 `wait4` 相比，它使用 `idtype`/`id` 参数对来标识等待目标，支持 `P_PIDFD` 类型，能显式选择等待事件类型（退出、停止、继续），并返回详细的 `siginfo_t` 结构。它还支持可选的 `struct rusage` 资源使用信息。

**原型：**

```c
// kernel/exit.c:1813
SYSCALL_DEFINE5(waitid, int, which, pid_t, upid, struct siginfo __user *,
                infop, int, options, struct rusage __user *, ru);
```

**参数说明：**
- `which`：等待类型标识符，指定如何解释 `upid` 参数：
  - `P_ALL`（0）：等待任何子进程，忽略 `upid`
  - `P_PID`（1）：等待特定 PID 的子进程，`upid` 必须大于 0
  - `P_PGID`（2）：等待特定进程组的子进程，`upid` 为进程组 ID（0 表示调用者的进程组）
  - `P_PIDFD`（3）：等待由 pidfd 文件描述符指向的进程，`upid` 为 pidfd
- `upid`：根据 `which` 类型解释的进程标识符
- `infop`：指向 `struct siginfo` 的指针，用于接收子进程状态变更的详细信息（可为 NULL）
- `options`：等待选项标志（见下文）
- `ru`：指向 `struct rusage` 的指针，用于获取子进程的资源使用情况（可为 NULL）

**options 标志位：**

| 标志 | 值 | 描述 |
|------|-----|------|
| `WNOHANG` | 0x01 | 如果没有子进程已退出，立即返回 0 |
| `WUNTRACED` | 0x02 | 也返回已停止但未报告的子进程 |
| `WEXITED` | 0x04 | 等待已退出的子进程（waitid **必须**显式设置此标志） |
| `WSTOPPED` | 0x02 | 同 WUNTRACED，等待已停止的子进程 |
| `WCONTINUED` | 0x08 | 返回已恢复运行的子进程 |
| `WNOWAIT` | 0x01000000 | 不收割子进程，只获取状态（子进程保持僵尸状态） |
| `__WNOTHREAD` | 0x20000000 | 不等待本线程组中其他线程的子进程 |
| `__WALL` | 0x40000000 | 等待所有子进程，不论类型 |
| `__WCLONE` | 0x80000000 | 只等待非 SIGCHLD 的子进程 |

**重要区别：** 与 `wait4` 不同，`waitid` **必须**显式设置 `WEXITED`、`WSTOPPED` 或 `WCONTINUED` 中的一个或多个，否则返回 `-EINVAL`。

**返回值：**
- 成功且子进程状态可用：返回 0（`infop` 中填充详细信息）
- `WNOHANG` 生效：返回 0（`infop->si_signo` 为 0）
- 错误：返回负的错误码

## 2. 使用场景

- **细粒度事件选择**：需要精确控制等待哪些事件（只等待退出，或只等待停止/继续）
- **pidfd 等待**：使用 pidfd 文件描述符而非 PID 来等待进程，避免 PID 重用问题
- **详细状态信息**：需要获取子进程状态变更的详细原因编码（`si_code`）和信号编号
- **非破坏性检查**：使用 `WNOWAIT` 检查子进程状态而不收割（不回收僵尸状态）
- **高级资源监控**：结合 `ru` 参数获取子进程的 CPU 使用和内存统计

## 3. 函数调用栈

```
waitid (系统调用入口)
├── kernel_waitid()                  [kernel/exit.c:1795]
│   ├── kernel_waitid_prepare()      [kernel/exit.c:1735]
│   │   ├── 校验 options 合法性
│   │   │   └── 必须包含 WEXITED|WSTOPPED|WCONTINUED 之一
│   │   ├── 根据 which 类型解析 PID:
│   │   │   ├── P_ALL    → PIDTYPE_MAX, pid=NULL
│   │   │   ├── P_PID    → PIDTYPE_PID,  pid=find_get_pid(upid), upid>0
│   │   │   ├── P_PGID   → PIDTYPE_PGID, pid=find_get_pid(upid) 或 current->pgid
│   │   │   └── P_PIDFD  → PIDTYPE_PID,  pid=pidfd_get_pid(upid, &f_flags)
│   │   └── 初始化 wait_opts 结构体
│   └── do_wait()                    [kernel/exit.c:1710]
│       ├── (同 wait4 的 do_wait 流程)
│       ├── __do_wait() → do_wait_pid/do_wait_thread → wait_consider_task
│       │   ├── wait_task_zombie() — 处理退出
│       │   ├── wait_task_stopped() — 处理停止
│       │   └── wait_task_continued() — 处理继续
│       └── 返回 pid 或错误码
├── 处理返回值:
│   ├── err > 0:
│   │   ├── signo = SIGCHLD
│   │   ├── err = 0
│   │   └── copy_to_user(ru, &r, sizeof(rusage))  // 复制资源信息
│   └── infop 非空 → 写入 siginfo 到用户空间:
│       ├── si_signo = SIGCHLD
│       ├── si_errno = 0
│       ├── si_code = info.cause     // CLD_EXITED/KILLED/DUMPED/TRAPPED/STOPPED/CONTINUED
│       ├── si_pid = info.pid
│       ├── si_uid = info.uid
│       └── si_status = info.status
└── 返回 err
```

## 4. 关键数据结构

### struct wait_opts

```c
// kernel/exit.h:12
struct wait_opts {
    enum pid_type       wo_type;      // 等待类型：PIDTYPE_PID/PGID/MAX(任意)
    int                 wo_flags;     // 等待标志
    struct pid          *wo_pid;      // 目标 PID 的 struct pid 指针
    struct waitid_info  *wo_info;     // waitid 信息指针（用于填充 siginfo）
    int                 wo_stat;      // 子进程退出状态
    struct rusage       *wo_rusage;   // 资源使用信息指针
    wait_queue_entry_t  child_wait;   // 等待队列项
    int                 notask_error; // 无匹配子进程时的错误码
};
```

### struct waitid_info

```c
// kernel/exit.h:5
struct waitid_info {
    pid_t pid;       // 子进程 PID
    uid_t uid;       // 子进程用户 UID
    int   status;    // 退出状态码或信号编号
    int   cause;     // 事件原因（CLD_* 常量）
};
```

**cause 字段取值：**

| 常量 | 值 | 含义 |
|------|-----|------|
| `CLD_EXITED` | 1 | 子进程正常退出 |
| `CLD_KILLED` | 2 | 子进程被信号杀死 |
| `CLD_DUMPED` | 3 | 子进程异常终止（产生 core dump） |
| `CLD_TRAPPED` | 4 | 被跟踪的子进程遇到陷阱 |
| `CLD_STOPPED` | 5 | 子进程已停止 |
| `CLD_CONTINUED` | 6 | 已停止的子进程继续运行 |

### struct siginfo（等待相关字段）

```c
// include/uapi/asm-generic/siginfo.h
// 从 waitid 返回时，填充以下字段：
struct siginfo {
    int      si_signo;   // 信号编号（SIGCHLD）
    int      si_errno;   // 错误号（0）
    int      si_code;    // 原因码（CLD_* 常量）
    pid_t    si_pid;     // 引起状态变化的子进程 PID
    uid_t    si_uid;     // 子进程实际用户 ID
    int      si_status;  // 退出状态或信号编号
    // ... 其他字段
};
```

### waitid 的 idtype 类型

```c
// include/uapi/linux/wait.h:17
#define P_ALL    0    // 等待任何子进程
#define P_PID    1    // 等待特定 PID 的子进程
#define P_PGID   2    // 等待特定进程组的子进程
#define P_PIDFD  3    // 等待 pidfd 指向的进程
```

## 5. 流程图

```
用户态调用 waitid(P_PID, pid, &info, WEXITED|WCONTINUED, &ru)
                            │
                            ▼
                     ┌──────────────┐
                     │  系统调用入口  │
                     │ SYSCALL_DEFINE5(waitid)
                     └──────┬───────┘
                            │
                            ▼
                     ┌───────────────┐
                     │ kernel_waitid() │
                     └───────┬───────┘
                             │
                             ▼
                     ┌──────────────────────┐
                     │ kernel_waitid_prepare() │
                     │                      │
                     │ 1. 校验 options      │
                     │    └ 非法标志 → -EINVAL
                     │    └ 缺少 WEXITED|WSTOPPED|WCONTINUED → -EINVAL
                     │ 2. 按 which 解析 PID  │
                     │    ┌─────┬──────────┐
                     │    │P_ALL│PIDTYPE_MAX│
                     │    ├─────┼──────────┤
                     │    │P_PID│PIDTYPE_PID│
                     │    ├─────┼──────────┤
                     │    │P_PGID│PIDTYPE_PGID│
                     │    ├─────┼──────────┤
                     │    │P_PIDFD│PIDTYPE_PID│
                     │    └─────┴──────────┘
                     │ 3. 初始化 wo 结构体   │
                     │    wo_info = &info    │
                     └──────┬───────────────┘
                            │
                            ▼
                     ┌───────────────┐
                     │   do_wait()   │
                     │               │
                     │  (同 wait4 的  │
                     │   do_wait 流程)│
                     │               │
                     │ 子进程状态变更: │
                     │  ├─ 退出 → wait_task_zombie()
                     │  │   └─ 填充 info.cause = CLD_EXITED/KILLED/DUMPED
                     │  │      info.status = 退出码/信号编号
                     │  │
                     │  ├─ 停止 → wait_task_stopped()
                     │  │   └─ 填充 info.cause = CLD_STOPPED/TRAPPED
                     │  │      info.status = 停止信号编号
                     │  │
                     │  └─ 继续 → wait_task_continued()
                     │      └─ 填充 info.cause = CLD_CONTINUED
                     │         info.status = SIGCONT
                     │
                     │  info.pid = 子进程 PID
                     │  info.uid = 子进程 UID
                     └──────┬───────┘
                            │
                            ▼
                     ┌──────────────────┐
                     │  返回值处理       │
                     │                  │
                     │ err > 0 (有子进程  │
                     │ 状态变更):        │
                     │  ├─ signo = SIGCHLD│
                     │  ├─ err = 0       │
                     │  └─ 复制 rusage  │
                     │                   │
                     │ infop 非空:        │
                     │  ├─ si_signo = SIGCHLD│
                     │  ├─ si_errno = 0  │
                     │  ├─ si_code = cause │
                     │  ├─ si_pid = pid  │
                     │  ├─ si_uid = uid  │
                     │  └─ si_status = status│
                     │                   │
                     │ 使用 unsafe_put_user │
                     │ 写入用户空间       │
                     └──────┬────────────┘
                            │
                            ▼
                    返回 0（成功）、0（WNOHANG）或负错误码
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-ECHILD` | 无匹配子进程 | 调用者没有符合条件的子进程 |
| `-EINVAL` | 无效参数 | options 包含非法标志，或未设置 `WEXITED|WSTOPPED|WCONTINUED`，或 `P_PID` 模式下 `upid <= 0`，或 `P_PGID` 模式下 `upid < 0`，或 `which` 为无效值 |
| `-EFAULT` | 地址错误 | `infop` 或 `ru` 指向不可写的用户空间地址 |
| `-EAGAIN` | 需重试 | 非 `WNOHANG` 模式下，子进程状态在返回前被清除（如另一个线程收割了该子进程） |
| `-EINTR` | 被信号中断 | 等待期间收到信号（内核内部会重启或返回 -EINTR） |
| `-EBADF` | 无效文件描述符 | `P_PIDFD` 模式下，`upid` 不是有效的 pidfd |

## 7. 使用示例

### 示例 1：使用 P_PID 等待特定子进程退出

```c
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

int main(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        printf("Child PID=%d running\n", getpid());
        sleep(1);
        exit(42);
    }

    siginfo_t info;
    struct rusage ru;
    int ret = waitid(P_PID, pid, &info, WEXITED | WNOWAIT, &ru);
    if (ret == -1) {
        perror("waitid");
        exit(1);
    }

    printf("Child PID=%d changed state\n", info.si_pid);
    printf("si_code=%d (CLD_EXITED=%d)\n", info.si_code, CLD_EXITED);
    if (info.si_code == CLD_EXITED)
        printf("Exit status: %d\n", info.si_status);
    printf("User CPU time: %ld.%06ld sec\n",
           ru.ru_utime.tv_sec, ru.ru_utime.tv_usec);
    return 0;
}
```

### 示例 2：使用 P_PIDFD 等待 pidfd 指向的进程

```c
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

int main(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        sleep(2);
        exit(0);
    }

    // 获取 pidfd（Linux 5.3+）
    int pidfd = syscall(SYS_pidfd_open, pid, 0);
    if (pidfd == -1) {
        perror("pidfd_open");
        exit(1);
    }

    // 使用 P_PIDFD 等待
    siginfo_t info;
    int ret = waitid(P_PIDFD, pidfd, &info, WEXITED, NULL);
    if (ret == -1) {
        perror("waitid");
        exit(1);
    }

    printf("Process %d exited with code %d\n",
           info.si_pid, info.si_status);
    close(pidfd);
    return 0;
}
```

### 示例 3：等待所有子进程的任意事件

```c
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

int main(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：暂停自身
        printf("Child PID=%d, stopping itself...\n", getpid());
        raise(SIGSTOP);
        printf("Child resumed, exiting\n");
        exit(0);
    }

    sleep(1);  // 等待子进程暂停

    // 等待停止事件
    siginfo_t info;
    int ret = waitid(P_ALL, 0, &info, WSTOPPED, NULL);
    if (ret == 0) {
        printf("Child PID=%d stopped (si_code=%d)\n",
               info.si_pid, info.si_code);
    }

    // 让子进程继续运行
    kill(pid, SIGCONT);

    // 等待继续事件
    ret = waitid(P_ALL, 0, &info, WCONTINUED, NULL);
    if (ret == 0) {
        printf("Child PID=%d continued (si_code=%d)\n",
               info.si_pid, info.si_code);
    }

    // 等待退出事件
    ret = waitid(P_ALL, 0, &info, WEXITED, NULL);
    if (ret == 0) {
        printf("Child PID=%d exited with status %d\n",
               info.si_pid, info.si_status);
    }

    return 0;
}
```

### 示例 4：非阻塞检查（WNOHANG）

```c
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        sleep(3);
        exit(0);
    }

    siginfo_t info;
    int ret;

    // 非阻塞检查
    ret = waitid(P_PID, pid, &info, WEXITED | WNOHANG, NULL);
    if (ret == -1) {
        perror("waitid");
        exit(1);
    }
    if (info.si_signo == 0)
        printf("Child still running (WNOHANG returned 0)\n");

    // 阻塞等待
    ret = waitid(P_PID, pid, &info, WEXITED, NULL);
    if (ret == 0)
        printf("Child PID=%d exited\n", info.si_pid);

    return 0;
}
```

## 8. 与相关系统调用的比较

| 系统调用 | 差异点 |
|----------|--------|
| `waitid` | 最现代化的等待接口，支持 `P_PIDFD`、`P_ALL`，详细 `siginfo_t` 返回，必须显式选择事件类型 |
| `wait4` | 传统接口，使用 PID 直接语义，自动设置 `WEXITED`，支持 `pid=-1` 等待任意子进程，不支持 `P_PIDFD` |
| `waitpid` | 兼容性封装，等价于 `wait4` 的简化版本，内部调用 `kernel_wait4` |
| `wait` | 最简接口，等价于 `waitpid(-1, &status, 0)` |

**waitid 与 wait4 的关键区别：**

| 特性 | waitid | wait4 |
|------|--------|-------|
| 事件类型选择 | 必须显式指定 | 自动包含 WEXITED |
| P_PIDFD 支持 | 是 | 否 |
| 返回信息 | siginfo_t（详细原因码） | 退出状态码（int） |
| 资源使用 | 可选 rusage 参数 | 可选 rusage 参数 |
| 非破坏性等待 | 支持 WNOWAIT | 支持 WNOWAIT |
| 等待任意进程 | P_ALL | pid=-1 |

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- [wait4 系统调用分析](./wait4.md)
- [exit 系统调用分析](./exit.md)
- [exit_group 系统调用分析](./exit_group.md)
- 内核源码：`kernel/exit.c`（`waitid`、`kernel_waitid`、`kernel_waitid_prepare`、`do_wait`、`wait_task_zombie`、`wait_task_stopped`、`wait_task_continued`）
- 内核头文件：`kernel/exit.h`（`struct wait_opts`、`struct waitid_info`）
- 用户空间头文件：`include/uapi/linux/wait.h`（idtype 和等待标志定义）、`include/uapi/asm-generic/siginfo.h`（`CLD_*` 常量定义）、`include/uapi/linux/resource.h`（`struct rusage`）