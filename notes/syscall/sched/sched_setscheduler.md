# sched_setscheduler 系统调用分析

## 1. 概述

设置进程的调度策略和调度参数（实时优先级）。这是设置调度策略的标准接口，支持 `SCHED_FIFO`、`SCHED_RR`、`SCHED_NORMAL`、`SCHED_BATCH`、`SCHED_IDLE` 等策略。

**原型：**

```c
SYSCALL_DEFINE3(sched_setscheduler, pid_t, pid, int, policy,
                struct sched_param __user *, param)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID（0 表示当前进程） |
| `policy` | `int` | 调度策略（`SCHED_FIFO`、`SCHED_RR`、`SCHED_NORMAL` 等） |
| `param` | `struct sched_param *` | 调度参数（包含 `sched_priority`） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EINVAL` — 无效策略（`policy < 0`）或无效参数
  - `-ESRCH` — 进程不存在
  - `-EFAULT` — 用户态指针无效
  - `-EPERM` — 权限不足
  - `-ENOMEM` — 内存不足

## 2. 调度策略

| 策略 | 值 | 说明 | 优先级范围 |
|------|-----|------|-----------|
| `SCHED_NORMAL` | 0 | 普通分时调度（CFS） | 0 |
| `SCHED_FIFO` | 1 | 先进先出实时调度 | 1~99 |
| `SCHED_RR` | 2 | 时间片轮转实时调度 | 1~99 |
| `SCHED_BATCH` | 3 | 批处理调度 | 0 |
| `SCHED_IDLE` | 5 | 空闲调度 | 0 |
| `SCHED_DEADLINE` | 6 | 最晚截止时间优先（需通过 `sched_setattr` 设置） | 0 |
| `SCHED_EXT` | 7 | 扩展调度（sched_ext） | 0 |

## 3. 使用场景

- **设置实时策略**: 将关键任务设置为 SCHED_FIFO 或 SCHED_RR
- **降低优先级**: 将后台任务设置为 SCHED_IDLE 或 SCHED_BATCH
- **恢复默认**: 将实时任务恢复为 SCHED_NORMAL

## 4. 函数调用栈

```
sched_setscheduler(pid, policy, param)  (系统调用入口)
└─ kernel/sched/syscalls.c
   ├─ 参数校验：policy >= 0
   │
   └─ do_sched_setscheduler(pid, policy, param)  // kernel/sched/core.c
        ├─ copy_from_user(&lp, param, sizeof(lp))  // 拷贝用户参数
        │
        ├─ find_process_by_pid(pid)        // RCU 查找 task_struct
        ├─ security_task_setscheduler(p)   // LSM 安全钩子
        │
        ├─ [权限检查]
        │  ├─ 设置 RT 策略 (SCHED_FIFO/SCHED_RR) → 需要 CAP_SYS_NICE
        │  ├─ 提高 RT 优先级 → 需要 CAP_SYS_NICE
        │  └─ 设置为 SCHED_IDLE → 需要 CAP_SYS_NICE
        │
        ├─ [参数校验]
        │  ├─ RT 优先级: 1~99
        │  ├─ 非 RT 策略: 优先级必须为 0
        │  └─ SCHED_IDLE: 优先级必须为 0
        │
        └─ sched_change_attr(p, &attr)     // 实际修改
             ├─ p->policy = policy
             ├─ p->rt_priority = lp.sched_priority
             ├─ 更新 p->static_prio
             ├─ 将任务从原运行队列移除
             ├─ 将任务插入新策略的运行队列
             └─ resched_curr(rq)  // 触发重新调度
```

## 5. 关键数据结构

```c
// include/uapi/linux/sched.h
struct sched_param {
    int sched_priority;   // 实时优先级 (1~99, 0 表示非实时)
};

// 调度策略常量
#define SCHED_NORMAL    0
#define SCHED_FIFO      1
#define SCHED_RR        2
#define SCHED_BATCH     3
#define SCHED_IDLE      5
#define SCHED_DEADLINE  6

// 调度类 (sched_class) 关系
// RT 策略 → &rt_sched_class
// CFS 策略 → &fair_sched_class
// IDLE 策略 → &idle_sched_class
// Deadline → &dl_sched_class
```

## 6. 流程图

```
sched_setscheduler(0, SCHED_FIFO, &param)  // param.sched_priority = 50
  │
  ├─ policy = SCHED_FIFO, policy >= 0 → 通过
  │
  └─ do_sched_setscheduler(0, SCHED_FIFO, &param)
       │
       ├─ copy_from_user: lp.sched_priority = 50
       │
       ├─ find_process_by_pid(0) → current
       ├─ security_task_setscheduler(current) → OK
       │
       ├─ 权限检查：设置 RT → 需要 CAP_SYS_NICE
       │    └─ 有权限 → 继续
       │
       ├─ 参数校验：priority=50, 范围 1~99 → 通过
       │
       └─ sched_change_attr(current, &attr)
            ├─ current->policy = SCHED_FIFO
            ├─ current->rt_priority = 50
            ├─ current->sched_class = &rt_sched_class
            ├─ dequeue_task(rq, current, DEQUEUE_SAVE)    // 出队
            ├─ enqueue_task(rq, current, ENQUEUE_RESTORE) // 入队到 RT 队列
            └─ resched_curr(rq)  // 触发重新调度
```

## 7. 使用示例

```c
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct sched_param param;
    int ret;

    param.sched_priority = 50;

    // 设置为 SCHED_FIFO 实时策略
    ret = sched_setscheduler(0, SCHED_FIFO, &param);
    if (ret == -1) {
        perror("sched_setscheduler");
        return 1;
    }

    printf("Set to SCHED_FIFO (priority 50)\n");

    // 恢复为普通调度
    param.sched_priority = 0;
    ret = sched_setscheduler(0, SCHED_NORMAL, &param);
    if (ret == 0) {
        printf("Restored to SCHED_NORMAL\n");
    }

    return 0;
}
```

## 8. 注意事项

- 设置实时策略（`SCHED_FIFO`/`SCHED_RR`）需要 `CAP_SYS_NICE` 权限
- 非实时策略的优先级必须为 0，实时策略的优先级必须在 1~99 之间
- 切换到 `SCHED_IDLE` 也需要 `CAP_SYS_NICE` 权限
- `SCHED_DEADLINE` 策略不能通过 `sched_setscheduler` 设置，必须使用 `sched_setattr`

## 9. 参考

- `kernel/sched/syscalls.c` — sched_setscheduler 实现
- `kernel/sched/core.c` — do_sched_setscheduler 实现
- `include/uapi/linux/sched.h` — 调度策略常量定义
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)