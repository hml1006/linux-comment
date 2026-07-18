# sched_getparam 系统调用分析

## 1. 概述

获取进程的调度参数（实时优先级）。对于实时任务（`SCHED_FIFO`/`SCHED_RR`），返回 `sched_priority`；对于非实时任务，返回 `sched_priority = 0`。

**原型：**

```c
SYSCALL_DEFINE2(sched_getparam, pid_t, pid, struct sched_param __user *, param)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID（0 表示当前进程） |
| `param` | `struct sched_param *` | 用户态调度参数缓冲区 |

**返回值：**

- 成功返回 `0`，`param->sched_priority` 被填充
- 失败返回负值错误码：
  - `-EINVAL` — 无效参数（`param` 为 NULL 或 `pid < 0`）
  - `-ESRCH` — 进程不存在
  - `-EFAULT` — 用户态指针无效
  - `-EPERM` — 权限不足

## 2. 使用场景

- **查询实时优先级**: 获取进程的实时优先级值
- **调度器状态检查**: 诊断进程的调度优先级
- **任务分类**: 判断进程是否为实时任务

## 3. 函数调用栈

```
sched_getparam(pid, param)  (系统调用入口)
└─ kernel/sched/syscalls.c
   ├─ 参数校验：param != NULL, pid >= 0
   │
   ├─ scoped_guard (rcu) {
   │    ├─ find_process_by_pid(pid)        // RCU 查找 task_struct
   │    ├─ security_task_getscheduler(p)   // LSM 安全钩子
   │    │
   │    └─ if (task_has_rt_policy(p))
   │         lp.sched_priority = p->rt_priority  // 获取实时优先级
   │         // 非实时任务 lp.sched_priority 保持为 0
   │   }
   │
   └─ copy_to_user(param, &lp, sizeof(*param))
        ├─ 成功 → 返回 0
        └─ 失败 → 返回 -EFAULT
```

## 4. 关键数据结构

```c
// include/uapi/linux/sched.h
struct sched_param {
    int sched_priority;   // 实时优先级 (1~99, 0 表示非实时)
};

// include/linux/sched.h
// task_struct 中的调度相关字段
struct task_struct {
    unsigned int policy;      // 调度策略 (SCHED_*)
    int rt_priority;          // 实时优先级
    int static_prio;          // 静态优先级
    int normal_prio;          // 常规优先级
    int prio;                 // 动态优先级
};

// 辅助宏
#define task_has_rt_policy(p)   \
    ((p)->policy == SCHED_FIFO || (p)->policy == SCHED_RR)
```

## 5. 流程图

```
sched_getparam(123, &param)
  │
  ├─ param != NULL, pid >= 0 → 通过
  │
  ├─ find_process_by_pid(123) → 找到 task_struct
  │
  ├─ security_task_getscheduler(p) → 权限检查通过
  │
  ├─ task_has_rt_policy(p)? → 是 (SCHED_FIFO)
  │    └─ lp.sched_priority = p->rt_priority = 50
  │
  └─ copy_to_user(param, &lp, 4) → 成功，返回 0
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

    // 获取当前进程的调度参数
    ret = sched_getparam(0, &param);
    if (ret == -1) {
        perror("sched_getparam");
        return 1;
    }

    printf("Current process RT priority: %d\n", param.sched_priority);

    // 获取其他进程的调度参数
    pid_t pid = 123;  // 示例 PID
    ret = sched_getparam(pid, &param);
    if (ret == 0) {
        printf("Process %d RT priority: %d\n", pid, param.sched_priority);
    } else {
        perror("sched_getparam");
    }

    return 0;
}
```

## 7. 参考

- `kernel/sched/syscalls.c` — sched_getparam 实现
- `include/uapi/linux/sched.h` — struct sched_param 定义
- `include/linux/sched.h` — task_struct 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)