# wait4 系统调用分析

## 1. 概述

`wait4` 用于等待子进程的状态变更（退出、暂停或恢复），并获取子进程的退出状态和资源使用情况。它是 `waitpid()`、`wait()` 和 `wait3()` 等库函数在内核中的底层实现。

与 `waitid` 相比，`wait4` 使用更传统的 PID 参数风格，并支持获取 `struct rusage` 资源使用信息。

**原型：**

```c
// kernel/exit.c:1905
SYSCALL_DEFINE4(wait4, pid_t, upid, int __user *, stat_addr,
                int, options, struct rusage __user *, ru);
```

**参数说明：**
- `upid`：指定要等待的子进程：
  - `<-1`：等待进程组 ID 等于 `-upid` 的任何子进程
  - `-1`：等待任何子进程
  - `0`：等待与调用者同进程组的任何子进程
  - `>0`：等待 PID 等于 `upid` 的特定子进程
- `stat_addr`：指向存储子进程退出状态的内存地址（可为 NULL）
- `options`：等待选项标志（见下文）
- `ru`：指向 `struct rusage` 的指针，用于获取子进程的资源使用情况（可为 NULL）

**options 标志位：**

| 标志 | 值 | 描述 |
|------|-----|------|
| `WNOHANG` | 0x01 | 如果没有子进程已退出，立即返回 0 |
| `WUNTRACED` | 0x02 | 也返回已停止但未报告的子进程 |
| `WEXITED` | 0x04 | 等待已退出的子进程（wait4 自动设置此标志） |
| `WCONTINUED` | 0x08 | 返回已恢复运行的子进程 |
| `__WNOTHREAD` | 0x20000000 | 不等待本线程组中其他线程的子进程 |
| `__WCLONE` | 0x80000000 | 只等待非 SIGCHLD 的子进程（clone 线程） |
| `__WALL` | 0x40000000 | 等待所有子进程，不论类型 |

**返回值：**
- 成功：返回已退出子进程的 PID（正数）
- `WNOHANG` 生效：返回 0
- 错误：返回负的错误码

## 2. 使用场景

- **Shell 作业控制**：Shell 使用 `wait4`（通过 `waitpid` 封装）等待子进程完成，并获取退出状态
- **守护进程管理**：`init` 或服务管理器使用 `wait4` 回收僵尸进程，防止进程表溢出
- **资源监控**：通过 `ru` 参数获取子进程的 CPU 时间、内存使用等资源统计
- **并行任务控制**：父进程创建多个子进程后，使用 `wait4` 等待所有子进程完成
- **进程同步**：父进程在子进程退出前阻塞，确保正确的执行顺序

## 3. 函数调用栈

```
wait4 (系统调用入口)
└── kernel_wait4()                [kernel/exit.c:1846]
    ├── 参数校验（options 标志位合法性、INT_MIN 检查）
    ├── PID 解析（确定 pid_type 和查找 struct pid）
    ├── 初始化 wait_opts 结构体
    └── do_wait()                  [kernel/exit.c:1710]
        ├── trace_sched_process_wait()  // 跟踪点
        ├── init_waitqueue_func_entry() // 初始化等待队列
        ├── add_wait_queue()            // 加入 signal->wait_chldexit
        ├── 循环:
        │   ├── set_current_state(TASK_INTERRUPTIBLE)
        │   ├── __do_wait()             [kernel/exit.c:1663]
        │   │   ├── 检查 PID 是否存在（notask_error = -ECHILD）
        │   │   ├── 读锁 tasklist_lock
        │   │   ├── do_wait_pid() 或 do_wait_thread() + ptrace_do_wait()
        │   │   │   ├── wait_consider_task()  [kernel/exit.c:1453]
        │   │   │   │   ├── eligible_child()  // 检查子进程是否符合条件
        │   │   │   │   ├── wait_task_zombie()  [kernel/exit.c:1173]
        │   │   │   │   │   ├── 处理 WNOWAIT（不收割，只获取状态）
        │   │   │   │   │   ├── cmpxchg exit_state: EXIT_ZOMBIE -> EXIT_DEAD/TRACE
        │   │   │   │   │   ├── 累加子进程资源使用到父进程（CPU 时间、缺页、I/O 等）
        │   │   │   │   │   ├── getrusage() 获取资源使用
        │   │   │   │   │   ├── 设置 wo_stat 退出状态
        │   │   │   │   │   ├── 处理 EXIT_TRACE（ptrace 通知父进程）
        │   │   │   │   │   ├── release_task() 释放任务（EXIT_DEAD）
        │   │   │   │   │   └── 填充 waitid_info（如果 wo_info 非空）
        │   │   │   │   ├── wait_task_stopped()  [kernel/exit.c:1329]
        │   │   │   │   │   ├── 检查 WUNTRACED 标志
        │   │   │   │   │   ├── 获取退出码（CLD_STOPPED/CLD_TRAPPED）
        │   │   │   │   │   └── 填充 wo_stat 和 waitid_info
        │   │   │   │   └── wait_task_continued()  [kernel/exit.c:1401]
        │   │   │   │       ├── 检查 WCONTINUED 标志
        │   │   │   │       ├── 清除 SIGNAL_STOP_CONTINUED 标志
        │   │   │   │       └── 填充 wo_stat 和 waitid_info
        │   │   │   └── 遍历当前线程的所有子进程
        │   │   ├── 释放 tasklist_lock
        │   │   └── 返回 retval（pid 或错误码）
        │   ├── 检查是否被信号中断
        │   └── schedule()  // 睡眠等待子进程唤醒
        ├── __set_current_state(TASK_RUNNING)
        ├── remove_wait_queue()         // 从等待队列移除
        └── 返回 retval
    ├── put_pid() 释放 PID 引用
    └── put_user(wo_stat, stat_addr)  // 将退出状态写入用户空间
```

## 4. 关键数据结构

### struct wait_opts

```c
// kernel/exit.h:12
struct wait_opts {
    enum pid_type       wo_type;      // 等待类型：PIDTYPE_PID/PGID/MAX(任意)
    int                 wo_flags;     // 等待标志（WEXITED|WUNTRACED|WCONTINUED 等）
    struct pid          *wo_pid;      // 目标 PID 的 struct pid 指针
    struct waitid_info  *wo_info;     // waitid 信息（wait4 用 NULL）
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
    int   status;    // 退出状态码
    int   cause;     // 事件原因（CLD_EXITED/CLD_KILLED/CLD_DUMPED等）
};
```

### struct rusage

```c
// include/uapi/linux/resource.h:24
struct rusage {
    struct __kernel_old_timeval ru_utime;   // 用户态 CPU 时间
    struct __kernel_old_timeval ru_stime;   // 内核态 CPU 时间
    __kernel_long_t ru_maxrss;     // 最大驻留集大小（KB）
    __kernel_long_t ru_ixrss;      // 整型共享内存大小
    __kernel_long_t ru_idrss;      // 整型非共享数据大小
    __kernel_long_t ru_isrss;      // 整型非共享栈大小
    __kernel_long_t ru_minflt;     // 缺页次数（小）
    __kernel_long_t ru_majflt;     // 缺页次数（大）
    __kernel_long_t ru_nswap;      // 交换次数
    __kernel_long_t ru_inblock;    // 块输入操作
    __kernel_long_t ru_oublock;    // 块输出操作
    __kernel_long_t ru_msgsnd;     // 发送的消息数
    __kernel_long_t ru_msgrcv;     // 接收的消息数
    __kernel_long_t ru_nsignals;   // 收到的信号数
    __kernel_long_t ru_nvcsw;      // 自愿上下文切换
    __kernel_long_t ru_nivcsw;     // 非自愿上下文切换
};
```

### 子进程状态编码

退出状态按 `status` 参数的编码规则：

| 条件 | `status` 值 | 含义 |
|------|-------------|------|
| 正常退出 | `WIFEXITED(status)` 真 | `WEXITSTATUS(status)` 获取退出码 |
| 信号终止 | `WIFSIGNALED(status)` 真 | `WTERMSIG(status)` 获取信号编号 |
| 程序暂停 | `WIFSTOPPED(status)` 真 | `WSTOPSIG(status)` 获取停止信号 |
| 程序恢复 | `WIFCONTINUED(status)` 真 | 已恢复运行 |

## 5. 流程图

```
用户态调用 wait4(pid, &status, options, &rusage)
                         │
                         ▼
                    ┌─────────────┐
                    │  系统调用入口 │
                    │  SYSCALL_DEFINE4(wait4)
                    └──────┬──────┘
                           │
                           ▼
                    ┌───────────────┐
                    │ kernel_wait4() │
                    │                │
                    │ 1. 校验 options  │
                    │    └ 非法 → -EINVAL
                    │ 2. 校验 upid    │
                    │    └ INT_MIN → -ESRCH
                    │ 3. 解析 PID     │
                    │    upid=-1  → PIDTYPE_MAX
                    │    upid<0   → PIDTYPE_PGID, pid=find_get_pid(-upid)
                    │    upid=0   → PIDTYPE_PGID, pid=current->pgid
                    │    upid>0   → PIDTYPE_PID,  pid=find_get_pid(upid)
                    │ 4. 初始化 wo     │
                    │    wo_type, wo_pid, wo_flags=options|WEXITED
                    │    wo_info=NULL, wo_rusage=ru
                    └──────┬──────┘
                           │
                           ▼
                    ┌───────────────┐
                    │   do_wait()   │
                    │               │
                    │ 1. 注册等待队列 │
                    │    add_wait_queue(&signal->wait_chldexit)
                    │               │
                    │ 循环开始 ◄─────┐
                    │ │             │
                    │ │ set_current_state(TASK_INTERRUPTIBLE)
                    │ │             │
                    │ ▼             │
                    │ __do_wait()   │  (信号未决时重试)
                    │ │             │
                    │ ├─ 检查 PID 有效性
                    │ ├─ 读锁 tasklist_lock
                    │ ├─ 遍历子进程:
                    │ │   ├─ eligible_child()  → 匹配?
                    │ │   │   ├─ 按 PID 类型匹配
                    │ │   │   ├─ 按 __WCLONE 匹配
                    │ │   │   └─ 按 __WALL 匹配
                    │ │   ├─ wait_task_zombie() → 僵尸进程
                    │ │   │   └─ WNOWAIT? → 只读状态
                    │ │   │   └─ 正常 → 收割线程
                    │ │   │       ├─ 累加资源到父进程
                    │ │   │       ├─ 获取 rusage
                    │ │   │       ├─ 设置 wo_stat
                    │ │   │       └─ release_task()
                    │ │   ├─ wait_task_stopped() → 已停止
                    │ │   │   └─ 获取退出码、设置 wo_stat
                    │ │   └─ wait_task_continued() → 已恢复
                    │ │       └─ 清除标志、设置 wo_stat
                    │ ├─ 释放 tasklist_lock
                    │ └─ 返回 pid 或错误码
                    │   ▲
                    │   └─ 返回 -ERESTARTSYS? ──→ schedule()
                    │                              │
                    │             子进程退出时唤醒 ──┘
                    │
                    │  ┌─ retval != -ERESTARTSYS?
                    │  │   └─ 是 → 退出循环
                    │  │   └─ 否 → 检查 signal_pending
                    │  │       └─ 是 → 退出循环
                    │  │       └─ 否 → schedule() 继续等待
                    │  └── (循环继续)
                    │
                    │ set_current_state(TASK_RUNNING)
                    │ remove_wait_queue()
                    └──────┬──────┘
                           │
                           ▼
                    ┌───────────────┐
                    │  返回处理      │
                    │               │
                    │ 1. put_pid()  │
                    │ 2. 如果 ret>0 │
                    │    且 stat_addr 非空
                    │    → put_user(wo_stat, stat_addr)
                    │    → 写入退出状态到用户空间
                    │ 3. 如果 ret>0 │
                    │    且 ru 非空
                    │    → copy_to_user(ru, &r, sizeof(rusage))
                    │ 4. 返回 ret   │
                    └───────────────┘
                           │
                           ▼
                   返回 PID（成功）、0（WNOHANG）或负错误码
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-ECHILD` | 无匹配子进程 | 调用者没有符合条件的子进程 |
| `-EINVAL` | 无效参数 | options 包含非法标志位，或 upid 为 INT_MIN |
| `-EFAULT` | 地址错误 | stat_addr 或 ru 指向不可写的用户空间地址 |
| `-ERESTARTSYS` | 被信号中断 | 等待期间收到信号（内核内部会重启或返回 -EINTR） |

## 7. 使用示例

### 示例 1：基本等待子进程

```c
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程
        printf("Child: PID=%d\n", getpid());
        exit(42);
    }

    // 父进程
    int status;
    struct rusage ru;
    pid_t ret = wait4(pid, &status, 0, &ru);
    if (ret == -1) {
        perror("wait4");
        exit(1);
    }

    printf("Child %d exited\n", ret);
    if (WIFEXITED(status))
        printf("Exit code: %d\n", WEXITSTATUS(status));
    printf("User CPU time: %ld.%06ld sec\n",
           ru.ru_utime.tv_sec, ru.ru_utime.tv_usec);
    return 0;
}
```

### 示例 2：非阻塞等待

```c
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        sleep(2);
        exit(0);
    }

    // 非阻塞等待
    int status;
    pid_t ret;
    while ((ret = wait4(pid, &status, WNOHANG, NULL)) == 0) {
        printf("Child not ready yet, doing other work...\n");
        sleep(1);
    }

    if (ret == pid)
        printf("Child exited!\n");
    return 0;
}
```

### 示例 3：等待任意子进程

```c
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    pid_t p1 = fork();
    if (p1 == 0) { sleep(3); exit(1); }

    pid_t p2 = fork();
    if (p2 == 0) { sleep(1); exit(2); }

    pid_t p3 = fork();
    if (p3 == 0) { sleep(2); exit(3); }

    // 等待任意子进程（pid=-1）
    for (int i = 0; i < 3; i++) {
        int status;
        pid_t ret = wait4(-1, &status, 0, NULL);
        if (ret > 0) {
            printf("Reaped child %d", ret);
            if (WIFEXITED(status))
                printf(" with exit code %d\n", WEXITSTATUS(status));
            else
                printf("\n");
        }
    }
    return 0;
}
```

## 8. 与相关系统调用的比较

| 系统调用 | 差异点 |
|----------|--------|
| `wait4` | 最通用的传统等待接口，支持 PID 语义和 rusage；内部自动设置 `WEXITED` |
| `waitid` | 更现代化的接口，支持 `P_PIDFD`，使用 `idtype/id` 参数对，支持 `WEXITED`/`WSTOPPED`/`WCONTINUED` 显式选择，可返回 `siginfo_t` |
| `waitpid` | 兼容性封装，`pid=-1` 等待任意子进程，内部调用 `kernel_wait4` |
| `wait` | 最简接口，等价于 `waitpid(-1, &status, 0)` |
| `clone3` | 创建子进程（与 wait4 对应，创建-等待配对使用） |

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- [waitid 系统调用分析](./waitid.md)
- [exit 系统调用分析](./exit.md)
- [exit_group 系统调用分析](./exit_group.md)
- 内核源码：`kernel/exit.c`（`wait4`、`kernel_wait4`、`do_wait`、`__do_wait`、`wait_task_zombie`、`wait_task_stopped`、`wait_task_continued`）
- 内核头文件：`kernel/exit.h`（`struct wait_opts`、`struct waitid_info`）
- 用户空间头文件：`include/uapi/linux/wait.h`（等待标志定义）、`include/uapi/linux/resource.h`（`struct rusage`）