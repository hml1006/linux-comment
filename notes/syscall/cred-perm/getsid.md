# getsid 系统调用分析

## 1. 概述

`getsid` 获取指定进程的会话 ID（session ID）。会话是进程组的集合，通常由 shell 在登录时创建。当 `pid` 为 0 时，返回当前进程的会话 ID。会话用于作业控制和终端管理。

**原型：**

```c
SYSCALL_DEFINE1(getsid, pid_t, pid)
```

**参数：**
- `pid`：目标进程的 PID。若为 0，则返回当前进程的会话 ID

**返回值：**
- 成功：目标进程的会话 ID（`pid_t`）
- 失败：返回负的错误码

## 2. 使用场景

- **作业控制**：确定进程所属的会话
- **进程审计**：追踪进程的会话归属
- **守护进程管理**：确认进程是否已脱离会话
- **终端管理**：管理控制终端与进程组的关系

## 3. 函数调用栈

```
getsid(pid)                                              // kernel/sys.c
  ├─ rcu_read_lock()
  ├─ [pid == 0] → sid = task_session(current)
  │  [pid != 0] → p = find_task_by_vpid(pid)
  │                if (!p) → -ESRCH
  │                sid = task_session(p)
  │                if (!sid) → -ESRCH
  │                retval = security_task_getsid(p)  // LSM 检查
  │                if (retval) → goto out
  ├─ retval = pid_vnr(sid)                           // 映射到当前命名空间
  └─ rcu_read_unlock()
```

## 4. 关键数据结构

### 4.1 会话管理

```c
// include/linux/sched.h
struct task_struct {
    // ...
    struct pid *thread_pid;
    struct task_struct *group_leader;
    // ...
};

// include/linux/signal.h
struct signal_struct {
    // ...
    struct pid *pids[PIDTYPE_MAX];  // 包含 PIDTYPE_SID 的 PID 指针
    unsigned int leader;            // 1 表示此进程是会话 leader
    // ...
};

// 获取会话的辅助函数
// kernel/pid.c
struct pid *task_session(struct task_struct *task)
{
    return task->group_leader->pids[PIDTYPE_SID].pid;
}
```

### 4.2 PID 类型枚举

```c
// include/linux/pid.h
enum pid_type {
    PIDTYPE_PID,     // 进程 ID
    PIDTYPE_TGID,    // 线程组 ID
    PIDTYPE_PGID,    // 进程组 ID
    PIDTYPE_SID,     // 会话 ID  ← getsid 查询此类型
    PIDTYPE_MAX
};
```

## 5. 流程图

```
用户态: getsid(pid)
    │
    v
┌───────────────────────────────────────────────┐
│ rcu_read_lock()                               │
│       │                                        │
│       ├── pid == 0?                            │
│       │    ├─ 是 → sid = task_session(current) │
│       │    └─ 否 → find_task_by_vpid(pid)      │
│       │           ├─ 未找到 → -ESRCH           │
│       │           └─ 找到 → sid = task_session(p)│
│       │                    ├─ sid 为空 → -ESRCH │
│       │                    └─ LSM 检查 → 拒绝  │
│       │                                         │
│       └── retval = pid_vnr(sid)                 │
│                                                 │
│ rcu_read_unlock()                               │
└───────────────────────────────────────────────┘
    │
    v
返回会话 ID 或错误码
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `ESRCH` | 进程不存在 | 指定的 `pid` 没有对应的进程 |
| `ESRCH` | 目标无会话 | 目标进程尚未关联到任何会话 |
| `EPERM` | 权限不足 | LSM（如 SELinux）阻止了本次查询 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    pid_t sid = getsid(0);
    printf("My session ID: %d\n", sid);

    /* 获取父进程的会话 ID */
    sid = getsid(getppid());
    if (sid >= 0)
        printf("Parent session ID: %d\n", sid);

    return 0;
}
```

### 7.2 守护进程与会话

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid > 0)
        exit(0);  /* 父进程退出 */

    /* 子进程创建新会话（脱离控制终端） */
    if (setsid() < 0) {
        perror("setsid");
        exit(1);
    }

    printf("Daemon started: PID=%d, SID=%d, PGID=%d\n",
           getpid(), getsid(0), getpgid(0));

    /* 创建新会话后，进程成为会话 leader */
    /* 且没有控制终端 */

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`
- PID 类型定义：`include/linux/pid.h`
- 信号结构体：`include/linux/signal.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)