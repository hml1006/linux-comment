# exit 系统调用分析

## 1. 概述

`exit` 系统调用用于终止当前进程。它执行一系列清理操作，释放进程占用的所有资源，并通知父进程子进程已退出。`exit` 只终止调用线程，而 `exit_group` 终止整个线程组。

### 关键特点

- `exit` 只终止当前调用线程，`exit_group` 终止整个线程组
- 通过 `do_exit` 实现核心退出逻辑
- 释放 mm（地址空间）、files（fd 表）、fs（文件系统信息）、namespace（命名空间）等
- 通过 `exit_notify` 向父进程发送 SIGCHLD 信号
- 最后调用 `schedule()` 切换到其他进程，不再返回

---

## 2. 函数原型

```c
#include <unistd.h>

void _exit(int status);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `status` | 进程退出状态码（低 8 位有效） |

### 内核入口

```c
// kernel/exit.c:1083
SYSCALL_DEFINE1(exit, int, error_code)
{
    do_exit((error_code & 0xff) << 8);
}
```

---

## 3. 调用链分析

### 完整调用链

```
exit(status)
└─ syscall(__NR_exit, status)
   └─ SYSCALL_DEFINE1(exit)                           // kernel/exit.c:1083
      └─ do_exit((error_code & 0xff) << 8)            // kernel/exit.c:896
         ├─ kthread_do_exit()                          // 如果是内核线程
         ├─ kcov_task_exit(tsk)                        // kcov 清理
         ├─ kmsan_task_exit(tsk)                       // KMSAN 清理
         ├─ synchronize_group_exit(tsk, code)          // 线程组同步
         ├─ ptrace_event(PTRACE_EVENT_EXIT, code)      // ptrace 通知
         ├─ exit_signals(tsk)                          // 设置 PF_EXITING
         ├─ seccomp_filter_release(tsk)                // seccomp 清理
         ├─ acct_update_integrals(tsk)                 // 记账更新
         ├─ group_dead = atomic_dec_and_test(&tsk->signal->live)  // 是否最后一个线程
         ├─ [group_dead && is_global_init] → panic()   // init 进程退出 → 内核 panic
         ├─ [group_dead] → hrtimer_cancel, exit_itimers  // 定时器清理
         ├─ [group_dead] → tty_audit_exit()           // TTY 审计
         ├─ audit_free(tsk)                            // 审计清理
         ├─ tsk->exit_code = code                      // 记录退出码
         ├─ taskstats_exit(tsk, group_dead)            // taskstats 通知
         ├─ trace_sched_process_exit(tsk, group_dead)  // tracepoint
         ├─ perf_event_exit_task(tsk)                  // perf 事件清理
         ├─ exit_mm()                                  // 释放地址空间
         │  ├─ mm_release(tsk, tsk->mm)                 // 释放 futex 等
         │  └─ mmput(mm)                               // 释放 mm_struct
         │     └─ exit_mmap(mm)                        // 释放所有 VMA
         ├─ [group_dead] → acct_process()              // 进程记账
         ├─ exit_sem(tsk)                              // SysV 信号量清理
         ├─ exit_shm(tsk)                              // 共享内存清理
         ├─ exit_files(tsk)                            // 释放 fd 表
         │  └─ put_files_struct(files)                  // 引用计数减 1
         ├─ exit_fs(tsk)                               // 释放 fs_struct
         │  └─ free_fs_struct(fs)
         ├─ [group_dead] → disassociate_ctty(1)        // 与控制终端断开
         ├─ exit_nsproxy_namespaces(tsk)               // 释放命名空间
         ├─ exit_task_work(tsk)                        // task_work 清理
         ├─ exit_thread(tsk)                           // 架构特定线程清理
         ├─ cgroup_task_exit(tsk)                      // cgroup 清理
         ├─ exit_notify(tsk, group_dead)               // 通知父进程
         │  ├─ forget_original_parent(tsk, &dead_reaper)  // 移交孤儿进程
         │  │  └─ find_new_reaper(tsk, &dead_reaper)      // 寻找新父进程（init/子 reaper）
         │  ├─ kill_orphaned_pgrp(tsk, ...)            // 处理孤儿进程组
         │  └─ [group_dead] → do_notify_parent(tsk, tsk->exit_code)  // 发送 SIGCHLD
         │     └─ __wake_up_parent(tsk, tsk->parent)  // 唤醒父进程
         ├─ proc_exit_connector(tsk)                   // 进程事件连接器
         ├─ exit_tasks_rcu_start()                     // RCU 退出开始
         ├─ schedule()                                 // 切换进程（不再返回）
         └─ BUG()  // 不应到达这里
```

### do_exit 详细流程

```c
// kernel/exit.c:896
void __noreturn do_exit(long code)
{
    struct task_struct *tsk = current;
    int group_dead;

    // 检查是否在中断上下文中
    WARN_ON(irqs_disabled());

    // 同步线程组退出
    synchronize_group_exit(tsk, code);

    // 退出信号处理（设置 PF_EXITING 标志）
    exit_signals(tsk);

    // 检查是否为线程组最后一个线程
    group_dead = atomic_dec_and_test(&tsk->signal->live);

    // 释放地址空间
    exit_mm();

    // 释放文件描述符表
    exit_files(tsk);

    // 释放文件系统信息
    exit_fs(tsk);

    // 释放命名空间
    exit_nsproxy_namespaces(tsk);

    // 通知父进程
    exit_notify(tsk, group_dead);

    // 切换到其他进程
    schedule();
    // 不再执行到这里
    BUG();
}
```

---

## 4. 关键数据结构

```c
// ========== 进程状态标志 (include/linux/sched.h) ==========

#define PF_EXITING      0x00000004  // 正在退出，在 exit_signals 中设置
#define PF_DEAD         0x00000080  // 进程已死亡（在 __exit_signal 中设置）

// ========== 进程退出状态 (include/linux/sched.h) ==========

// task_struct 中与退出相关的字段
struct task_struct {
    // ...
    int exit_code;          // 进程退出码
    int exit_signal;        // 退出时发送给父进程的信号
    int pdeath_signal;      // 父进程死亡时发送给本进程的信号
    unsigned long jobctl;   // 作业控制
    // ...

    // 信号相关
    struct signal_struct *signal;   // 线程组信号结构
    struct sighand_struct *sighand; // 信号处理函数
    struct sigpending pending;      // 待处理信号
    // ...
};

// ========== 信号结构 (include/linux/sched.h) ==========

struct signal_struct {
    atomic_t live;          // 线程组中存活线程数
    // ...
    int group_exit_code;    // 线程组退出码
    int group_stop_count;   // 组停止计数
    unsigned int flags;     // 标志位（SIGNAL_*）
    // ...
    wait_queue_head_t wait_chldexit;  // 父进程等待子进程退出的等待队列
    struct task_struct *group_exit_task;  // 执行组退出的线程
    // ...
};

// ========== exit_mm 中释放 mm_struct 的关键操作 ==========

// mm_release: 释放 futex 状态、udelay 等
// 在 exit_mm() 中调用
void mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
    // 释放 futex 状态
    futex_exit_release(tsk);
    // 释放用户空间锁
    uprobe_exit_mm(tsk);

    // 如果设置了 clear_child_tid，通知子进程
    if (tsk->clear_child_tid) {
        put_user(0, tsk->clear_child_tid);
        // 如果有 futex 在等待，唤醒它
        futex_wake(tsk->clear_child_tid, 1);
    }
}
```

---

## 5. 流程图

```
                     exit(status)
                         |
                 +-------v--------+
                 | SYSCALL_DEFINE1 |
                 | (kernel/exit.c) |
                 +-------+--------+
                         |
                 +-------v--------+
                 | do_exit(code)  |
                 | (核心退出逻辑)  |
                 +-------+--------+
                         |
        +----------------+-----------------+
        |                                   |
        | 阶段1: 退出前准备                  |
        |  - synchronize_group_exit()       |
        |  - ptrace_event(EXIT)             |
        |  - exit_signals() → PF_EXITING    |
        |  - seccomp_filter_release()       |
        +-----------------------------------+
                         |
        +----------------+-----------------+
        |                                   |
        | 阶段2: 释放资源                   |
        |  - exit_mm()     → 释放地址空间   |
        |  - exit_sem()    → 释放信号量     |
        |  - exit_shm()    → 释放共享内存   |
        |  - exit_files()  → 释放 fd 表     |
        |  - exit_fs()     → 释放 fs 结构   |
        |  - exit_nsproxy()→ 释放命名空间   |
        |  - exit_task_work()               |
        |  - exit_thread() → 架构清理       |
        +-----------------------------------+
                         |
        +----------------+-----------------+
        |                                   |
        | 阶段3: 通知和调度                |
        |  - exit_notify()                  |
        |     ├─ forget_original_parent()   |
        |     │  └─ find_new_reaper()       |
        |     └─ do_notify_parent()         |
        |        └─ __wake_up_parent()      |
        |  - schedule()                     |
        |  (不再返回)                       |
        +-----------------------------------+
```

---

## 6. 错误处理

| 错误码/条件 | 说明 | 触发位置 |
|-------------|------|----------|
| `panic()` | 全局 init 进程退出 | `do_exit` |
| `WARN_ON(irqs_disabled())` | 中断禁用时退出（警告） | `do_exit` |
| `WARN_ON(tsk->plug)` | 存在未刷新的 plug 列表（警告） | `do_exit` |

`exit` 系统调用没有返回值，调用成功后进程不再存在。

---

## 7. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

int main() {
    pid_t pid = fork();

    if (pid == 0) {
        // 子进程
        printf("子进程: PID=%d, 即将退出\n", getpid());
        _exit(42);  // 退出码 42
        // 不会执行到这里
    } else if (pid > 0) {
        // 父进程
        int status;
        wait(&status);

        if (WIFEXITED(status)) {
            printf("子进程退出码: %d\n", WEXITSTATUS(status));
        }
    }

    return 0;
}
```

---

## 8. 与 exit_group 对比

| 特性 | exit | exit_group |
|------|------|------------|
| **作用范围** | 仅终止调用线程 | 终止整个线程组 |
| **实现** | 直接调用 `do_exit` | 调用 `do_group_exit` → `do_exit` |
| **内核函数** | `do_exit(long code)` | `do_group_exit(int exit_code)` |
| **使用场景** | 线程退出 | 进程退出（所有线程退出） |
| **glibc 封装** | `_exit()` | `exit_group()`（glibc 内部使用） |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/exit.c:896` - do_exit 实现
- `kernel/exit.c:1083` - SYSCALL_DEFINE1(exit)
- `kernel/signal.c` - exit_signals 实现