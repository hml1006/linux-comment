# sched_getattr 系统调用分析

## 1. 概述

扩展版获取调度属性。与 `sched_getparam` 相比，`sched_getattr` 通过 `struct sched_attr` 结构体返回更丰富的调度属性，包括调度策略、优先级、nice 值、Deadline 参数以及利用率限制（uclamp）等。

**原型：**

```c
SYSCALL_DEFINE4(sched_getattr, pid_t, pid, struct sched_attr __user *, uattr,
                unsigned int, usize, unsigned int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID（0 表示当前进程） |
| `uattr` | `struct sched_attr *` | 用户态属性缓冲区 |
| `usize` | `unsigned int` | 用户态结构体大小（用于前向/后向兼容） |
| `flags` | `unsigned int` | 预留标志位（当前必须为 0） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EINVAL` — 无效参数（`usize` 太小或太大，`flags` 非零）
  - `-ESRCH` — 进程不存在
  - `-EFAULT` — 用户态指针无效
  - `-EPERM` — 权限不足

## 2. 使用场景

- **获取调度属性**: 获取进程的完整调度属性
- **Deadline 任务**: 查询 SCHED_DEADLINE 任务的运行时间、截止时间、周期
- **uclamp 查询**: 查询进程的利用率限制（最小/最大）
- **调度策略诊断**: 检查进程的调度策略和标志

## 3. 函数调用栈

```
sched_getattr(pid, uattr, usize, flags)  (系统调用入口)
└─ kernel/sched/syscalls.c
   ├─ 参数校验：uattr != NULL, pid >= 0, usize <= PAGE_SIZE
   │            usize >= SCHED_ATTR_SIZE_VER0, flags == 0
   │
   ├─ scoped_guard (rcu) {
   │    ├─ find_process_by_pid(pid)        // 查找进程
   │    ├─ security_task_getscheduler(p)   // LSM 安全钩子
   │    │
   │    ├─ kattr.sched_policy = p->policy  // 获取调度策略
   │    ├─ if (p->sched_reset_on_fork)
   │    │    kattr.sched_flags |= SCHED_FLAG_RESET_ON_FORK
   │    │
   │    ├─ get_params(p, &kattr)           // 获取调度参数
   │    │    ├─ 对于 RT: 设置 sched_priority
   │    │    ├─ 对于 CFS: 设置 sched_nice
   │    │    └─ 对于 Deadline: 设置 runtime/deadline/period
   │    │
   │    ├─ kattr.sched_flags &= SCHED_FLAG_ALL
   │    │
   │    └─ #ifdef CONFIG_UCLAMP_TASK
   │        kattr.sched_util_min = p->uclamp_req[UCLAMP_MIN].value
   │        kattr.sched_util_max = p->uclamp_req[UCLAMP_MAX].value
   │        #endif
   │   }
   │
   ├─ kattr.size = min(usize, sizeof(kattr))  // 调整大小
   │
   └─ copy_struct_to_user(uattr, usize, &kattr, sizeof(kattr), NULL)
        └─ 拷贝到用户态
```

## 4. 关键数据结构

```c
// include/uapi/linux/sched/types.h
struct sched_attr {
    __u32 size;                     // 结构体大小（前向/后向兼容）

    __u32 sched_policy;             // 调度策略
    __u64 sched_flags;              // 调度标志

    /* SCHED_NORMAL, SCHED_BATCH */
    __s32 sched_nice;               // nice 值

    /* SCHED_FIFO, SCHED_RR */
    __u32 sched_priority;           // 实时优先级

    /* SCHED_DEADLINE */
    __u64 sched_runtime;            // 运行时间 (ns)
    __u64 sched_deadline;           // 截止时间 (ns)
    __u64 sched_period;             // 周期 (ns)

    /* Utilization hints (CONFIG_UCLAMP_TASK) */
    __u32 sched_util_min;           // 最小利用率 (0~1024)
    __u32 sched_util_max;           // 最大利用率 (0~1024)
};

// 调度标志 (include/uapi/linux/sched.h)
#define SCHED_FLAG_RESET_ON_FORK    0x01  // 子进程重置调度策略为默认
#define SCHED_FLAG_RECLAIM          0x02  // 回收空闲带宽
#define SCHED_FLAG_DL_OVERRUN       0x04  // Deadline 超限处理
#define SCHED_FLAG_KEEP_POLICY      0x08  // 保持策略不变
#define SCHED_FLAG_KEEP_PARAMS      0x10  // 保持参数不变
#define SCHED_FLAG_UTIL_CLAMP_MIN   0x20  // 设置利用率下限
#define SCHED_FLAG_UTIL_CLAMP_MAX   0x40  // 设置利用率上限
#define SCHED_FLAG_ALL              0x7F  // 所有标志掩码
```

## 5. 流程图

```
sched_getattr(123, &attr, sizeof(attr), 0)
  │
  ├─ 参数校验通过
  │
  ├─ 查找 PID=123 的 task_struct
  │
  ├─ 填充 sched_attr:
  │    ├─ sched_policy = SCHED_OTHER (或 SCHED_FIFO 等)
  │    ├─ sched_nice = 0
  │    ├─ sched_priority = 0 (非实时任务)
  │    └─ sched_flags = 0
  │
  └─ copy_struct_to_user(&attr, ...)
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
    struct sched_attr attr = { .size = sizeof(attr) };

    // 获取当前进程的调度属性
    int ret = syscall(SYS_sched_getattr, 0, &attr, sizeof(attr), 0);
    if (ret == -1) {
        perror("sched_getattr");
        return 1;
    }

    printf("Policy: %d\n", attr.sched_policy);
    printf("Nice: %d\n", attr.sched_nice);
    printf("Priority: %u\n", attr.sched_priority);
    printf("Flags: %llu\n", (unsigned long long)attr.sched_flags);

    return 0;
}
```

## 7. 参考

- `kernel/sched/syscalls.c` — sched_getattr 实现
- `include/uapi/linux/sched/types.h` — struct sched_attr 定义
- `include/uapi/linux/sched.h` — 调度策略和标志常量
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)