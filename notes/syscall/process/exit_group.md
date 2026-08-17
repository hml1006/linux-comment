# exit_group 系统调用分析

## 1. 概述

`exit_group` 系统调用用于终止当前进程所在线程组的所有线程。与 `exit` 只终止调用线程不同，`exit_group` 会向线程组中所有线程发送退出信号，确保整个进程完全终止。

### 关键特点

- 终止整个线程组（所有线程），而非仅当前线程
- 通过 `do_group_exit` → `do_exit` 实现
- 设置 `group_exit_code` 后，其他线程在返回用户态时会检测到并自动退出
- 是 glibc 中 `exit()` 函数的底层实现

---

## 2. 函数原型

```c
#include <unistd.h>

void _exit(int status);
// 或
#include <stdlib.h>
void exit(int status);  // 实际调用 exit_group
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `status` | 进程退出状态码（低 8 位有效） |

### 内核入口

```c
// kernel/exit.c:1127
SYSCALL_DEFINE1(exit_group, int, error_code)
{
    do_group_exit((error_code & 0xff) << 8);
    /* NOTREACHED */
    return 0;
}
```

---

## 3. 调用链分析

### 完整调用链

```
exit_group(error_code)
└─ syscall(__NR_exit_group, error_code)
   └─ SYSCALL_DEFINE1(exit_group)                    // kernel/exit.c:1127
      └─ do_group_exit((error_code & 0xff) << 8)     // kernel/exit.c:1092
         ├─ spin_lock_irq(&tsk->sighand->siglock)
         ├─ [signal->flags & SIGNAL_GROUP_EXIT]      // 已设置组退出
         │  └─ exit_code = signal->group_exit_code    // 使用已有退出码
         ├─ [!SIGNAL_GROUP_EXIT]                      // 首次设置组退出
         │  ├─ signal->group_exit_code = exit_code
         │  ├─ signal->flags = SIGNAL_GROUP_EXIT
         │  └─ spin_unlock_irq(&tsk->sighand->siglock)
         ├─ spin_unlock_irq(&tsk->sighand->siglock)
         └─ do_exit(exit_code)                        // kernel/exit.c:896
            └─ [同 exit 的 do_exit 路径]
               ├─ exit_signals(tsk)                   // 设置 PF_EXITING
               ├─ group_dead = atomic_dec_and_test(&tsk->signal->live)
               ├─ exit_mm()                           // 释放地址空间
               ├─ exit_files(tsk)                     // 释放 fd 表
               ├─ exit_fs(tsk)                        // 释放 fs 结构
               ├─ exit_nsproxy_namespaces(tsk)        // 释放命名空间
               ├─ exit_notify(tsk, group_dead)        // 通知父进程
               └─ schedule()                          // 切换进程
```

### do_group_exit 详细流程

```c
// kernel/exit.c:1092
void __noreturn do_group_exit(int exit_code)
{
    struct signal_struct *sig = current->signal;

    spin_lock_irq(&sig->siglock);

    // 检查是否已经设置了组退出
    if (sig->flags & SIGNAL_GROUP_EXIT) {
        // 已有其他线程设置了组退出，使用其退出码
        exit_code = sig->group_exit_code;
    } else {
        // 首次设置组退出
        sig->group_exit_code = exit_code;
        sig->flags |= SIGNAL_GROUP_EXIT;
    }
    spin_unlock_irq(&sig->siglock);

    // 执行线程退出
    do_exit(exit_code);
    // 不返回
}
```

### 其他线程如何检测组退出

当调用 `do_group_exit` 的线程设置了 `SIGNAL_GROUP_EXIT` 标志后，线程组中的其他线程在以下时机检测到并退出：

1. **返回用户态时**：在 `exit_to_user_mode_loop` 中检查 `task_sigpending(current)` 和 `signal_group_exit()`
2. **系统调用返回时**：在 `syscall_exit_work` 中处理
3. **信号递送时**：在 `get_signal()` 中检测到 `SIGNAL_GROUP_EXIT`

```c
// kernel/signal.c
void exit_signals(struct task_struct *tsk)
{
    int group_stop = 0;
    sigset_t unblocked;

    // 设置 PF_EXITING
    tsk->flags |= PF_EXITING;

    // 如果当前线程是最后一个线程，需要重新计算信号
    if (thread_group_empty(tsk))
        goto out;

    // 通知其他线程
    spin_lock_irq(&tsk->sighand->siglock);
    // 清空信号队列
    flush_sigqueue(&tsk->pending);
    tsk->jobctl &= ~JOBCTL_STOP_DEQUEUED;
    spin_unlock_irq(&tsk->sighand->siglock);
out:
    // 重新计算信号
    recalc_sigpending();
}
```

---

## 4. 关键数据结构

```c
// ========== 信号结构体中的组退出相关字段 (include/linux/sched.h) ==========

struct signal_struct {
    atomic_t live;                      // 线程组中存活线程数
    // ...
    int group_exit_code;                // 线程组退出码（do_group_exit 设置）
    unsigned int flags;                 // 标志位
    // ...
    struct task_struct *group_exit_task;  // 执行组退出的线程
    // ...
};

// 信号标志
#define SIGNAL_GROUP_EXIT   0x00000002  // 线程组正在退出（do_group_exit 设置）
#define SIGNAL_STOP_STOPPED 0x00000001  // 线程组已停止
#define SIGNAL_STOP_CONTINUED 0x00000004  // 线程组已继续
#define SIGNAL_STOP_DEQUEUED 0x00000008  // 停止信号已出队
```

---

## 5. 流程图

```
                     exit_group(error_code)
                              |
                     +--------v--------+
                     | SYSCALL_DEFINE1 |
                     | (kernel/exit.c) |
                     +--------+--------+
                              |
                     +--------v--------+
                     | do_group_exit() |
                     | (kernel/exit.c) |
                     +--------+--------+
                              |
              +---------------+---------------+
              |                               |
    +---------v---------+           +---------v---------+
    | SIGNAL_GROUP_EXIT |           | 首次设置组退出     |
    | 已设置?            |           |                    |
    | 是: 使用已有退出码  |           | 设置:              |
    | 否: 设置新退出码   |           | group_exit_code    |
    +---------+---------+           | flags |=           |
              |                     | SIGNAL_GROUP_EXIT  |
              |                     +---------+----------+
              |                               |
              +---------------+---------------+
                              |
                     +--------v--------+
                     | do_exit(code)   |
                     | (同 exit 路径)   |
                     +--------+--------+
                              |
              +---------------+---------------+
              | 释放资源      | 通知父进程      |
              | exit_mm()     | exit_notify()  |
              | exit_files()  | → SIGCHLD      |
              | exit_fs()     |                |
              | exit_nsproxy()|                |
              +-------+-------+----------------+
                      |
              +-------v-------+
              | schedule()    |
              | (不再返回)    |
              +---------------+
```

---

## 6. 错误处理

`exit_group` 系统调用没有返回值，调用成功后线程不再存在。

| 条件 | 行为 | 说明 |
|------|------|------|
| 多次调用 `do_group_exit` | 使用首次设置的退出码 | 后续调用忽略 `exit_code` 参数 |
| `SIGNAL_GROUP_EXIT` 已设置 | 不重复设置，仅复用退出码 | 由 `siglock` 保护 |
| 全局 init 进程退出 | `panic()` | 内核保护 |

---

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

void *thread_func(void *arg) {
    sleep(1);  // 模拟工作
    printf("线程: 仍在运行, PID=%d, TID=%d\n", getpid(), gettid());
    return NULL;
}

int main() {
    pthread_t thread;
    pthread_create(&thread, NULL, thread_func, NULL);

    sleep(2);  // 等待线程开始运行

    printf("主进程: 调用 exit_group, 所有线程将终止\n");
    exit_group(42);  // 立即终止所有线程
    // 不会执行到这里
    return 0;
}
```

---

## 8. 与 exit 对比

| 特性 | exit | exit_group |
|------|------|------------|
| **作用范围** | 仅终止调用线程 | 终止整个线程组 |
| **内核实现** | `do_exit(long code)` | `do_group_exit(int)` → `do_exit` |
| **SIGNAL_GROUP_EXIT** | 不设置 | 设置该标志 |
| **其他线程行为** | 继续运行 | 检测到 SIGNAL_GROUP_EXIT 后退出 |
| **glibc exit()** | 不使用 | 使用 |
| **glibc _exit()** | 使用 | 也可能使用 |
| **系统调用号** | 93（ARM64） | 94（ARM64） |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/exit.c:1092` - do_group_exit 实现
- `kernel/exit.c:1127` - SYSCALL_DEFINE1(exit_group)
- `kernel/exit.c:896` - do_exit 实现