# sched_setparam 系统调用分析

## 1. 概述

设置进程的调度参数（实时优先级）。与 `sched_setscheduler` 的区别在于，`sched_setparam` 只修改 `sched_priority`，不改变调度策略。但有一个特殊行为：如果当前策略是 `SCHED_NORMAL`（非实时），调用 `sched_setparam` 可以将其切换到 `SCHED_FIFO`（当 `sched_priority > 0` 时）。

**原型：**

```c
SYSCALL_DEFINE2(sched_setparam, pid_t, pid, struct sched_param __user *, param)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID（0 表示当前进程） |
| `param` | `struct sched_param *` | 用户态调度参数（包含 sched_priority） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EINVAL` — 无效参数（`param` 为 NULL 或 `pid < 0`）
  - `-ESRCH` — 进程不存在
  - `-EFAULT` — 用户态指针无效
  - `-EPERM` — 权限不足
  - `-ENOMEM` — 内存不足

## 2. 使用场景

- **修改实时优先级**: 在不改变调度策略的情况下调整实时优先级
- **实时任务调整**: 动态调整实时任务的优先级
- **非实时转实时**: 将非实时任务提升为实时任务（需要权限）

## 3. 函数调用栈

```
sched_setparam(pid, param)  (系统调用入口)
└─ kernel/sched/syscalls.c
   └─ do_sched_setscheduler(pid, SETPARAM_POLICY, param)
        │
        ├─ param 中的 sched_priority 决定最终行为：
        │    ├─ 如果当前策略是 SCHED_NORMAL 且 sched_priority > 0
        │    │    → 自动切换为 SCHED_FIFO
        │    └─ 否则，保持当前策略不变
        │
        ├─ find_process_by_pid(pid)        // 查找进程
        ├─ security_task_setscheduler(p)   // LSM 安全钩子
        │
        ├─ [权限检查]
        │  ├─ 提高优先级 → 需要 CAP_SYS_NICE
        │  └─ 降低优先级 → 非特权进程也可操作
        │
        ├─ [参数校验]
        │  └─ 如果目标策略是 RT: 优先级在 1~99 范围内
        │
        └─ sched_change_attr(p, &attr)     // 实际修改
             ├─ p->rt_priority = param->sched_priority
             ├─ 如果策略变化，更新 p->policy
             ├─ 将任务移入正确的运行队列
             └─ resched_curr(rq)
```

## 4. 关键数据结构

```c
// include/uapi/linux/sched.h
struct sched_param {
    int sched_priority;   // 实时优先级 (1~99, 0 表示非实时)
};

// kernel/sched/core.c
// SETPARAM_POLICY 是一个特殊值，表示"保持当前策略"
#define SETPARAM_POLICY  -1

// 当 do_sched_setscheduler 收到 SETPARAM_POLICY 时：
// 1. 如果 param->sched_priority == 0 → 保持当前策略
// 2. 如果 param->sched_priority > 0 且当前是 SCHED_NORMAL
//    → 自动升级为 SCHED_FIFO
// 3. 否则保持当前策略不变
```

## 5. 流程图

```
sched_setparam(0, &param)    // param.sched_priority = 50
  │
  └─ do_sched_setscheduler(0, SETPARAM_POLICY, &param)
       │
       ├─ 当前策略 = SCHED_NORMAL, sched_priority > 0
       │    → 自动切换为 SCHED_FIFO
       │
       ├─ find_process_by_pid(0) → current
       ├─ 权限检查：设置 RT 优先级 → 需要 CAP_SYS_NICE
       │
       ├─ sched_change_attr(current, &attr)
       │    ├─ current->policy = SCHED_FIFO
       │    ├─ current->rt_priority = 50
       │    └─ 将任务移入 RT 运行队列
       │
       └─ 返回 0
```

## 6. 使用示例

```c
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct sched_param param;
    int ret;

    // 设置实时优先级为 50
    param.sched_priority = 50;

    ret = sched_setparam(0, &param);
    if (ret == -1) {
        perror("sched_setparam");
        return 1;
    }

    printf("RT priority set to 50\n");
    return 0;
}
```

## 7. 注意事项

- `sched_setparam` 通过 `do_sched_setscheduler` 实现，使用 `SETPARAM_POLICY` 特殊值
- 如果当前是非实时策略且 `sched_priority > 0`，会自动切换为 `SCHED_FIFO`
- 如果 `sched_priority == 0`，当前非实时策略保持不变

## 8. 参考

- `kernel/sched/syscalls.c` — sched_setparam 实现
- `kernel/sched/core.c` — do_sched_setscheduler 实现
- `include/uapi/linux/sched.h` — struct sched_param 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)