# getpgid 系统调用分析

## 1. 概述

`getpgid` 获取指定进程的进程组 ID（process group ID）。进程组是相关进程的集合，用于信号分发和作业控制。当 `pid` 为 0 时，返回当前进程的进程组 ID。

**原型：**

```c
SYSCALL_DEFINE1(getpgid, pid_t, pid)
```

**参数：**
- `pid`：目标进程的 PID。若为 0，则返回当前进程的进程组 ID

**返回值：**
- 成功：目标进程的进程组 ID（`pid_t`）
- 失败：返回负的错误码

## 2. 使用场景

- **作业控制**：shell 将进程放入前台/后台进程组
- **信号分发**：`killpg()` 向整个进程组发送信号
- **进程监控**：查询进程的组归属关系
- **调试工具**：分析进程的层次结构

## 3. 函数调用栈

```
getpgid(pid)                                             // kernel/sys.c
  └─ do_getpgid(pid)
       ├─ rcu_read_lock()
       ├─ [pid == 0] → grp = task_pgrp(current)
       │  [pid != 0] → p = find_task_by_vpid(pid)
       │                if (!p) → -ESRCH
       │                grp = task_pgrp(p)
       │                if (!grp) → -ESRCH
       │                retval = security_task_getpgid(p)  // LSM 检查
       │                if (retval) → goto out
       ├─ retval = pid_vnr(grp)                           // 获取进程组号
       └─ rcu_read_unlock()
```

## 4. 关键数据结构

### 4.1 进程组与 task_struct

```c
// include/linux/sched.h
struct task_struct {
    // ...
    struct pid *thread_pid;         // 线程 PID
    struct task_struct *group_leader; // 线程组 leader
    // ...
};

// include/linux/pid.h
struct pid {
    // ...
    int level;                      // 命名空间层级
    struct hlist_node tasks[PIDTYPE_MAX];  // 按 PID 类型组织的链表
    // ...
};

// PID 类型枚举
enum pid_type {
    PIDTYPE_PID,     // 进程 ID
    PIDTYPE_TGID,    // 线程组 ID
    PIDTYPE_PGID,    // 进程组 ID  ← getpgid 查询此类型
    PIDTYPE_SID,     // 会话 ID
    PIDTYPE_MAX
};
```

### 4.2 辅助函数

```c
// 获取进程的进程组 PID
// kernel/pid.c
struct pid *task_pgrp(struct task_struct *task)
{
    return task->group_leader->pids[PIDTYPE_PGID].pid;
}

// 将内核 PID 转换为当前命名空间可见的 PID 号
pid_t pid_vnr(struct pid *pid)
{
    // 根据 pid->level 和当前命名空间层级计算
}
```

## 5. 流程图

```
用户态: getpgid(pid)
    │
    v
┌───────────────────────────────────────────────┐
│ do_getpgid(pid)                                │
│                                                │
│  rcu_read_lock()                               │
│       │                                        │
│       ├── pid == 0?                            │
│       │    ├─ 是 → grp = task_pgrp(current)    │
│       │    └─ 否 → find_task_by_vpid(pid)      │
│       │           ├─ 未找到 → -ESRCH           │
│       │           └─ 找到 → grp = task_pgrp(p) │
│       │                    ├─ grp 为空 → -ESRCH│
│       │                    └─ LSM 检查 → 拒绝  │
│       │                                        │
│       └── retval = pid_vnr(grp)                │
│                                                │
│  rcu_read_unlock()                             │
└───────────────────────────────────────────────┘
    │
    v
返回进程组 ID 或错误码
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `ESRCH` | 进程不存在 | 指定的 `pid` 没有对应的进程 |
| `ESRCH` | 目标无进程组 | 目标进程尚未关联到任何进程组 |
| `EPERM` | 权限不足 | LSM（如 SELinux）阻止了本次查询 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    pid_t pgid = getpgid(0);
    printf("My process group ID: %d\n", pgid);

    /* 获取指定进程的组 ID */
    pgid = getpgid(getpid());
    printf("Process group of PID %d: %d\n", getpid(), pgid);

    /* 获取父进程的组 ID */
    pgid = getpgid(getppid());
    printf("Parent's process group: %d\n", pgid);

    return 0;
}
```

### 7.2 作业控制场景

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <stdlib.h>

int main(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* 子进程：创建新进程组 */
        setpgid(0, 0);
        printf("Child:  PID=%d, PGID=%d\n", getpid(), getpgid(0));
        pause();
        exit(0);
    }

    /* 父进程 */
    printf("Parent: PID=%d, PGID=%d\n", getpid(), getpgid(0));
    printf("Child's PGID: %d\n", getpgid(pid));

    wait(NULL);
    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`do_getpgid` 实现）
- PID 类型定义：`include/linux/pid.h`
- 进程描述符：`include/linux/sched.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)