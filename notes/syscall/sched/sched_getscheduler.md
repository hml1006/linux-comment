# sched_getscheduler 系统调用分析

## 1. 概述

获取进程的调度策略。返回当前进程使用的调度策略（`SCHED_FIFO`、`SCHED_RR`、`SCHED_NORMAL` 等），以及 `SCHED_RESET_ON_FORK` 标志。

**原型：**

```c
SYSCALL_DEFINE1(sched_getscheduler, pid_t, pid)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID（0 表示当前进程） |

**返回值：**

- 成功返回调度策略值（正数），可能包含 `SCHED_RESET_ON_FORK` 标志
- 失败返回负值错误码：
  - `-EINVAL` — `pid < 0`
  - `-ESRCH` — 进程不存在
  - `-EPERM` — 权限不足

## 2. 调度策略常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `SCHED_NORMAL` (SCHED_OTHER) | 0 | 普通分时调度（CFS） |
| `SCHED_FIFO` | 1 | 先进先出实时调度 |
| `SCHED_RR` | 2 | 时间片轮转实时调度 |
| `SCHED_BATCH` | 3 | 批处理调度 |
| `SCHED_IDLE` | 5 | 空闲调度 |
| `SCHED_DEADLINE` | 6 | 最晚截止时间优先 |
| `SCHED_EXT` | 7 | 扩展调度（sched_ext） |
| `SCHED_RESET_ON_FORK` | 0x40000000 | 子进程重置调度策略标志 |

## 3. 使用场景

- **进程调度状态查询**: 查看进程使用的是哪种调度策略
- **实时任务检查**: 确认进程是否以实时策略运行
- **调度器调试**: 诊断进程调度行为

## 4. 函数调用栈

```
sched_getscheduler(pid)  (系统调用入口)
└─ kernel/sched/syscalls.c
   ├─ 参数校验：pid >= 0
   │
   ├─ guard(rcu)()
   │
   ├─ find_process_by_pid(pid)        // RCU 查找 task_struct
   │    └─ 未找到 → 返回 -ESRCH
   │
   ├─ security_task_getscheduler(p)   // LSM 安全钩子
   │    └─ 拒绝 → 返回错误码
   │
   └─ retval = p->policy              // 获取调度策略
        └─ if (p->sched_reset_on_fork)
             retval |= SCHED_RESET_ON_FORK  // 附加标志
        └─ 返回 retval
```

## 5. 关键数据结构

```c
// include/uapi/linux/sched.h
#define SCHED_NORMAL    0   // 普通分时调度
#define SCHED_FIFO      1   // 先进先出实时
#define SCHED_RR        2   // 时间片轮转实时
#define SCHED_BATCH     3   // 批处理
#define SCHED_IDLE      5   // 空闲
#define SCHED_DEADLINE  6   // Deadline 调度
#define SCHED_EXT       7   // 扩展调度（sched_ext）

#define SCHED_RESET_ON_FORK  0x40000000  // 子进程重置标志

// 进程的 sched_reset_on_fork 字段
// 如果设置，fork 的子进程将恢复为 SCHED_NORMAL 策略
```

## 6. 流程图

```
sched_getscheduler(0)   // 获取当前进程
  │
  ├─ pid >= 0 → 通过
  │
  ├─ find_process_by_pid(0) → current
  │
  ├─ security_task_getscheduler(current) → 通过
  │
  ├─ current->policy = SCHED_NORMAL (0)
  ├─ current->sched_reset_on_fork = false
  │
  └─ 返回 0 (SCHED_NORMAL)
```

## 7. 使用示例

```c
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int policy;

    // 获取当前进程的调度策略
    policy = sched_getscheduler(0);
    if (policy == -1) {
        perror("sched_getscheduler");
        return 1;
    }

    printf("Scheduling policy: ");
    switch (policy & ~SCHED_RESET_ON_FORK) {
    case SCHED_NORMAL:
        printf("SCHED_NORMAL\n");
        break;
    case SCHED_FIFO:
        printf("SCHED_FIFO\n");
        break;
    case SCHED_RR:
        printf("SCHED_RR\n");
        break;
    case SCHED_BATCH:
        printf("SCHED_BATCH\n");
        break;
    case SCHED_IDLE:
        printf("SCHED_IDLE\n");
        break;
    case SCHED_DEADLINE:
        printf("SCHED_DEADLINE\n");
        break;
    default:
        printf("Unknown (%d)\n", policy & ~SCHED_RESET_ON_FORK);
    }

    if (policy & SCHED_RESET_ON_FORK)
        printf("  (SCHED_RESET_ON_FORK set)\n");

    return 0;
}
```

## 8. 参考

- `kernel/sched/syscalls.c` — sched_getscheduler 实现
- `include/uapi/linux/sched.h` — 调度策略常量定义
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)