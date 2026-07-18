# setsid 系统调用分析

## 1. 概述

`setsid` 创建新会话。调用进程成为新会话的会话 leader，同时成为新进程组的进程组 leader。创建新会话后，进程将脱离控制终端。这是守护进程创建的标准步骤之一。

**原型：**

```c
SYSCALL_DEFINE0(setsid)
```

**参数：** 无

**返回值：**
- 成功：返回新会话的会话 ID（`pid_t`）
- 失败：返回负的错误码

## 2. 使用场景

- **守护进程创建**：脱离控制终端，避免被终端信号影响
- **创建独立进程组**：使进程组独立于父进程的会话
- **shell 作业控制**：创建新会话以管理作业

## 3. 函数调用栈

```
setsid()                                                 // kernel/sys.c
  └─ ksys_setsid()
       ├─ write_lock_irq(&tasklist_lock)
       ├─ 检查: 调用进程不能已经是会话 leader
       │   └─ group_leader->signal->leader == 1 → -EPERM
       ├─ 检查: 目标 session ID 不能已被进程组使用
       │   └─ pid_task(sid, PIDTYPE_PGID) 存在 → -EPERM
       ├─ group_leader->signal->leader = 1     // 标记为会话 leader
       ├─ set_special_pids(pids, sid)          // 更新 PID 链表
       │    ├─ PIDTYPE_SID = task_pid(group_leader)
       │    └─ PIDTYPE_PGID = task_pid(group_leader)
       ├─ proc_clear_tty(group_leader)         // 断开控制终端
       ├─ err = session (pid 值)
       ├─ write_unlock_irq(&tasklist_lock)
       ├─ if (err > 0) {
       │      proc_sid_connector(group_leader)  // 通知连接器
       │      sched_autogroup_create_attach(group_leader)
       │   }
       └─ return err
```

## 4. 关键数据结构

### 4.1 会话与进程组关系

```c
// include/linux/sched.h
struct task_struct {
    struct task_struct *group_leader;
    // ...
};

// include/linux/signal.h
struct signal_struct {
    unsigned int leader;    // 1 = 此进程是会话 leader
    // ...
};

// PID 类型关系
enum pid_type {
    PIDTYPE_PID,     // 进程 ID
    PIDTYPE_TGID,    // 线程组 ID
    PIDTYPE_PGID,    // 进程组 ID  ← setsid 设置当前进程为此类型的 leader
    PIDTYPE_SID,     // 会话 ID   ← setsid 设置当前进程为此类型的 leader
    PIDTYPE_MAX
};
```

### 4.2 会话层次结构

```
                 ┌──────────────┐
                 │   会话 SID    │  ← setsid 创建
                 │ (会话 leader) │
                 └──────┬───────┘
                        │
               ┌────────┴────────┐
               │  进程组 PGID-1   │
               │  (前台进程组)    │
               └────────┬────────┘
                        │
              ┌─────────┼─────────┐
              │ PID-1   │ PID-2   │
              │ (组长)  │         │
              └─────────┴─────────┘
```

## 5. 流程图

```
用户态: setsid()
    │
    v
┌─────────────────────────────────────────────────────┐
│ 加锁: write_lock_irq(&tasklist_lock)                │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ 检查: 已经是会话 leader? → -EPERM                   │
│ 检查: SID 已被进程组使用? → -EPERM                  │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ signal->leader = 1                                  │
│ set_special_pids():                                 │
│   PIDTYPE_SID = current PID                         │
│   PIDTYPE_PGID = current PID                        │
│ proc_clear_tty(): 断开控制终端                      │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ 解锁: write_unlock_irq(&tasklist_lock)              │
│ 通知连接器                                          │
│ 创建自动调度组                                       │
│ 返回新会话 ID (PID)                                 │
└─────────────────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EPERM` | 已经是会话 leader | 调用进程的 `signal->leader` 已为 1 |
| `EPERM` | PID 已被进程组使用 | 当前 PID 已经被注册为某个进程组的 ID |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    pid_t sid;

    /* 创建新会话 */
    sid = setsid();
    if (sid < 0) {
        perror("setsid");
        return 1;
    }

    printf("New session ID: %d\n", sid);
    printf("Process group ID: %d\n", getpgid(0));

    /* 此时进程已脱离控制终端 */
    /* 通常用于守护进程创建 */

    return 0;
}
```

### 7.2 守护进程创建

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

void daemonize(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    /* 父进程退出，子进程被 init 收养 */
    if (pid > 0)
        exit(0);

    /* 子进程：创建新会话，脱离控制终端 */
    if (setsid() < 0) {
        perror("setsid");
        exit(1);
    }

    /* 再次 fork，确保不会重新获取控制终端 */
    pid = fork();
    if (pid < 0)
        exit(1);
    if (pid > 0)
        exit(0);

    /* 设置文件权限掩码 */
    umask(0);

    /* 切换工作目录到根目录 */
    chdir("/");

    /* 关闭所有文件描述符 */
    for (int i = 0; i < sysconf(_SC_OPEN_MAX); i++)
        close(i);

    /* 重定向标准 I/O 到 /dev/null */
    open("/dev/null", O_RDWR);  /* stdin */
    dup(0);                      /* stdout */
    dup(0);                      /* stderr */
}

int main(void)
{
    daemonize();

    /* 守护进程主循环 */
    printf("Daemon running: PID=%d, SID=%d\n",
           getpid(), getsid(0));

    while (1) {
        /* 执行守护任务 */
        sleep(10);
    }

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`ksys_setsid` 实现）
- PID 类型定义：`include/linux/pid.h`
- 信号结构体：`include/linux/signal.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)