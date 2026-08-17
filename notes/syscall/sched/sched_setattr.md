# sched_setattr 系统调用分析

## 1. 概述

扩展版设置调度属性。通过 `struct sched_attr` 结构体设置进程的调度策略、优先级、nice 值、Deadline 参数以及利用率限制（uclamp）等。这是 `sched_setscheduler` 的增强版。

**原型：**

```c
SYSCALL_DEFINE3(sched_setattr, pid_t, pid, struct sched_attr __user *, uattr,
                unsigned int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID（0 表示当前进程） |
| `uattr` | `struct sched_attr *` | 用户态调度属性结构体 |
| `flags` | `unsigned int` | 预留标志位（当前必须为 0） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EINVAL` — 无效参数
  - `-ESRCH` — 进程不存在
  - `-EFAULT` — 用户态指针无效
  - `-EPERM` — 权限不足
  - `-EBUSY` — Deadline 参数冲突

## 2. 使用场景

- **设置实时策略**: 设置 SCHED_FIFO/SCHED_RR 策略及优先级
- **设置 Deadline 调度**: 配置 SCHED_DEADLINE 任务的运行时间、截止时间、周期
- **设置利用率限制**: 通过 uclamp 限制任务的最小/最大利用率
- **修改 nice 值**: 通过 sched_attr 的 sched_nice 字段设置

## 3. 函数调用栈

```
sched_setattr(pid, uattr, flags)  (系统调用入口)
└─ kernel/sched/syscalls.c
   ├─ 参数校验：uattr != NULL, pid >= 0, flags == 0
   │
   ├─ sched_copy_attr(uattr, &attr)       // 从用户态拷贝 sched_attr
   │    └─ copy_struct_from_user()        // 安全拷贝并校验 size
   │
   ├─ [SCHED_FLAG_KEEP_POLICY] 检查
   │    └─ 如果设置，保留当前策略
   │
   ├─ [SCHED_FLAG_KEEP_PARAMS] 检查
   │    └─ 如果设置，保留当前参数
   │
   └─ __sched_setscheduler(pid, &attr)    // 核心设置逻辑
        ├─ find_process_by_pid(pid)       // 查找进程
        ├─ security_task_setscheduler(p)  // LSM 安全钩子
        │
        ├─ [策略转换检查]
        │  ├─ 非特权进程不能切换到 RT 策略
        │  ├─ 切换到 Deadline 需要 CAP_SYS_NICE
        │  └─ 非特权进程不能提高优先级
        │
        ├─ [参数校验]
        │  ├─ RT 优先级: 1~99
        │  ├─ Deadline 参数: runtime <= deadline <= period
        │  └─ nice 值: -20~19
        │
        ├─ [cgroup 检查]
        │  └─ cgroup_can_fork() 等
        │
        └─ sched_change_attr(p, &attr)   // 实际修改调度属性
             ├─ p->policy = attr.sched_policy
             ├─ p->rt_priority = attr.sched_priority
             ├─ p->static_prio = NICE_TO_PRIO(attr.sched_nice)
             ├─ 更新 Deadline 参数 (dl_se)
             ├─ 更新 uclamp 值
             ├─ 将任务移入正确的运行队列
             └─ 重新调度检查
```

## 4. 关键数据结构

```c
// include/uapi/linux/sched/types.h
struct sched_attr {
    __u32 size;                     // 结构体大小
    __u32 sched_policy;             // 调度策略
    __u64 sched_flags;              // 调度标志
    __s32 sched_nice;               // nice 值
    __u32 sched_priority;           // 实时优先级
    __u64 sched_runtime;            // Deadline 运行时间 (ns)
    __u64 sched_deadline;           // Deadline 截止时间 (ns)
    __u64 sched_period;             // Deadline 周期 (ns)
    __u32 sched_util_min;           // 最小利用率 (0~1024)
    __u32 sched_util_max;           // 最大利用率 (0~1024)
};

// 调度标志 (include/uapi/linux/sched.h)
#define SCHED_FLAG_RESET_ON_FORK    0x01
#define SCHED_FLAG_RECLAIM          0x02
#define SCHED_FLAG_DL_OVERRUN       0x04
#define SCHED_FLAG_KEEP_POLICY      0x08  // 设置时保持当前策略不变
#define SCHED_FLAG_KEEP_PARAMS      0x10  // 设置时保持当前参数不变
#define SCHED_FLAG_UTIL_CLAMP_MIN   0x20
#define SCHED_FLAG_UTIL_CLAMP_MAX   0x40
```

## 5. 流程图

```
sched_setattr(0, &attr, 0)
  │
  ├─ attr.sched_policy = SCHED_FIFO
  ├─ attr.sched_priority = 50
  │
  ├─ __sched_setscheduler(0, &attr)
  │    ├─ find_process_by_pid(0) → current
  │    ├─ security_task_setscheduler(current) → OK
  │    │
  │    ├─ 权限检查：设置 RT 需要 CAP_SYS_NICE
  │    │    └─ 有权限 → 继续
  │    │
  │    ├─ 参数校验：priority=50 在 1~99 范围内 → 通过
  │    │
  │    └─ sched_change_attr(current, &attr)
  │         ├─ current->policy = SCHED_FIFO
  │         ├─ current->rt_priority = 50
  │         ├─ 将任务移入 RT 运行队列
  │         └─ resched_curr(rq)  // 触发重新调度
  │
  └─ 返回 0
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/sched.h>
#include <linux/sched/types.h>

int main(void)
{
    struct sched_attr attr = {
        .size = sizeof(attr),
        .sched_policy = SCHED_FIFO,
        .sched_priority = 50,
    };

    // 设置当前进程为 SCHED_FIFO，优先级 50
    int ret = syscall(SYS_sched_setattr, 0, &attr, 0);
    if (ret == -1) {
        perror("sched_setattr");
        return 1;
    }

    printf("Set to SCHED_FIFO priority 50\n");
    return 0;
}
```

## 7. 参考

- `kernel/sched/syscalls.c` — sched_setattr 实现
- `kernel/sched/core.c` — __sched_setscheduler 实现
- `include/uapi/linux/sched/types.h` — struct sched_attr 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)