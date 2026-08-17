# setpgid 系统调用分析

## 1. 概述

`setpgid` 设置指定进程的进程组 ID，用于创建新的进程组或将进程移动到现有进程组。主要用于 shell 的作业控制（前台/后台进程组切换）。调用者可以是进程自身（设置自己的进程组），也可以是父进程（设置子进程的进程组）。

**原型：**

```c
SYSCALL_DEFINE2(setpgid, pid_t, pid, pid_t, pgid)
```

**参数：**
- `pid`：目标进程 PID。若为 0，则使用当前进程
- `pgid`：目标进程组 ID。若为 0，则创建以 `pid` 为组 ID 的新进程组

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **shell 作业控制**：`bash` 在启动后台作业时创建新进程组
- **守护进程创建新进程组**：脱离父进程的进程组
- **管道命令**：shell 将管道中的进程放入同一进程组

## 3. 函数调用栈

```
setpgid(pid, pgid)                                       // kernel/sys.c
  ├─ [pid == 0] → pid = task_pid_vnr(current)
  ├─ [pgid == 0] → pgid = pid
  ├─ [pgid < 0] → -EINVAL
  ├─ rcu_read_lock()
  ├─ write_lock_irq(&tasklist_lock)
  ├─ find_task_by_vpid(pid) → 查找目标进程，若不存在 → -ESRCH
  ├─ 检查: 目标必须是线程组 leader (thread_group_leader)
  │   └─ 否 → -EINVAL
  ├─ 检查: 目标是当前进程的子进程?
  │   ├─ 是 → 必须在同一会话中 → 否则 -EPERM
  │   │       必须尚未执行 exec (PF_FORKNOEXEC) → 否则 -EACCES
  │   └─ 否 → 必须是当前进程自身 → 否则 -ESRCH
  ├─ 检查: 目标不能是会话 leader (signal->leader) → -EPERM
  ├─ 检查: 若 pgid != pid → 目标进程组必须存在且在同一会话中
  │   ├─ find_vpid(pgid) → 查找进程组
  │   └─ pid_task(pgrp, PIDTYPE_PGID) → 检查组是否存在
  ├─ security_task_setpgid(p, pgid) → LSM 检查
  ├─ change_pid() → 更新进程的 PIDTYPE_PGID
  └─ write_unlock_irq(&tasklist_lock)
```

## 4. 关键数据结构

### 4.1 进程组相关

```c
// include/linux/sched.h
struct task_struct {
    struct task_struct *group_leader;  // 线程组 leader
    struct task_struct *real_parent;   // 实际父进程
    unsigned int flags;                // 包含 PF_FORKNOEXEC
    struct signal_struct *signal;      // 信号相关（含 leader 标志）
    // ...
};

// include/linux/signal.h
struct signal_struct {
    unsigned int leader;  // 1 = 此进程是会话 leader
    // ...
};
```

### 4.2 PID 类型

```c
// include/linux/pid.h
enum pid_type {
    PIDTYPE_PID,     // 进程 ID
    PIDTYPE_TGID,    // 线程组 ID
    PIDTYPE_PGID,    // 进程组 ID  ← setpgid 修改此类型
    PIDTYPE_SID,     // 会话 ID
    PIDTYPE_MAX
};
```

## 5. 流程图

```
用户态: setpgid(pid, pgid)
    │
    v
┌─────────────────────────────────────────────────────┐
│ 参数归一化: pid==0→current, pgid==0→pid             │
│ pgid < 0 → -EINVAL                                  │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ 加锁: write_lock_irq(&tasklist_lock)                │
│ find_task_by_vpid(pid) → 未找到 → -ESRCH            │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────────┐
│ 多重检查:                                                │
│ ├─ 是线程组 leader? 否 → -EINVAL                         │
│ ├─ 是当前进程或子进程?                                   │
│ │   ├─ 子进程: 同会话? 否 → -EPERM                      │
│ │   │          未 exec? 否 → -EACCES                    │
│ │   └─ 非子进程: 是当前进程? 否 → -ESRCH                │
│ ├─ 是会话 leader? 是 → -EPERM                           │
│ └─ 加入已有组? pgid 组存在且同会话? 否 → -EPERM         │
└─────────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ security_task_setpgid() → LSM 检查                  │
│ change_pid() → 更新 PIDTYPE_PGID                    │
│ 写解锁 → 返回 0                                     │
└─────────────────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | `pgid < 0`，或目标不是线程组 leader |
| `ESRCH` | 进程不存在 | 找不到指定的 `pid`，或目标非当前进程且非子进程 |
| `EPERM` | 操作不允许 | 目标是会话 leader，或子进程不在同一会话 |
| `EACCES` | 子进程已执行 exec | 子进程已调用 `execve()`，`PF_FORKNOEXEC` 标志已清除 |
| `EPERM` | 进程组不存在 | 加入的进程组不存在或不在同一会话 |

## 7. 使用示例

### 7.1 创建新进程组

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    pid_t pid = getpid();

    /* 创建新进程组，当前进程成为组长 */
    if (setpgid(pid, pid) < 0) {
        perror("setpgid");
        return 1;
    }

    printf("Created new process group: %d\n", pid);
    printf("My PGID: %d\n", getpgid(0));

    return 0;
}
```

### 7.2 shell 作业控制

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

int main(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* 子进程：创建新进程组（后台作业） */
        setpgid(0, 0);
        printf("Child: in new PGID=%d\n", getpgid(0));
        sleep(10);
        exit(0);
    }

    /* 父进程：将子进程放入新进程组 */
    setpgid(pid, pid);

    printf("Parent: set child PID=%d to new PGID=%d\n",
           pid, pid);

    wait(NULL);
    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`
- PID 类型定义：`include/linux/pid.h`
- 进程描述符：`include/linux/sched.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)