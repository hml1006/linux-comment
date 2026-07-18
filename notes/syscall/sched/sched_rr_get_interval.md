# sched_rr_get_interval 系统调用分析

## 1. 概述

获取 `SCHED_RR`（时间片轮转）调度策略的时间片长度。对于非 RR 策略的进程，返回的时间片为 0（表示无限）。

**原型：**

```c
SYSCALL_DEFINE2(sched_rr_get_interval, pid_t, pid,
                struct __kernel_timespec __user *, interval)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID（0 表示当前进程） |
| `interval` | `struct timespec *` | 用户态时间片缓冲区 |

**返回值：**

- 成功返回 `0`，`interval` 被填充为时间片长度
- 失败返回负值错误码：
  - `-EINVAL` — `pid < 0`
  - `-ESRCH` — 进程不存在
  - `-EFAULT` — 用户态指针无效
  - `-EPERM` — 权限不足

## 2. 使用场景

- **RR 时间片查询**: 查看 SCHED_RR 任务的时间片长度
- **调度器调优**: 了解系统默认的 RR 时间片配置
- **实时任务分析**: 评估实时任务的调度时间粒度

## 3. 函数调用栈

```
sched_rr_get_interval(pid, interval)  (系统调用入口)
└─ kernel/sched/syscalls.c
   ├─ 参数校验：pid >= 0
   │
   └─ sched_rr_get_interval(pid, &t)           // 获取时间片
        ├─ scoped_guard (rcu) {
        │    ├─ find_process_by_pid(pid)        // 查找进程
        │    ├─ security_task_getscheduler(p)   // LSM 安全钩子
        │    │
        │    └─ scoped_guard (task_rq_lock, p) {
        │         ├─ rq = scope.rq
        │         └─ if (p->sched_class->get_rr_interval)
        │              time_slice = p->sched_class->get_rr_interval(rq, p)
        │         // 对于非 RR 任务，time_slice 保持为 0
        │    }
        │   }
        │
        └─ jiffies_to_timespec64(time_slice, t)  // jiffies 转 timespec
   │
   └─ put_timespec64(&t, interval)  // 拷贝到用户态
```

## 4. 关键数据结构

```c
// include/linux/sched.h
// 调度类定义
struct sched_class {
    ...
    unsigned int (*get_rr_interval)(struct rq *rq, struct task_struct *task);
    ...
};

// kernel/sched/rt.c
// SCHED_RR 的默认时间片（由内核配置决定）
// 通常为 100ms (HZ=1000 时) 或系统可配置
static unsigned int get_rr_interval_rt(struct rq *rq, struct task_struct *task)
{
    return rr_interval;  // 以 jiffies 为单位
}

// 对于非 RT 调度类（CFS、IDLE 等），get_rr_interval 返回 0
```

## 5. 流程图

```
sched_rr_get_interval(123, &ts)
  │
  ├─ pid >= 0 → 通过
  │
  ├─ find_process_by_pid(123) → 找到 task_struct
  │
  ├─ 进程是 SCHED_RR 策略
  │    └─ p->sched_class->get_rr_interval()
  │         → 返回 10 个 jiffies (HZ=1000 时 = 10ms)
  │
  ├─ jiffies_to_timespec64(10, &t)
  │    → t.tv_sec = 0, t.tv_nsec = 10000000 (10ms)
  │
  └─ put_timespec64(&t, interval) → 返回 0
```

## 6. 使用示例

```c
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    struct timespec ts;
    int ret;

    // 获取当前进程的 RR 时间片
    // 如果当前进程不是 SCHED_RR，返回的时间片为 0
    ret = sched_rr_get_interval(0, &ts);
    if (ret == -1) {
        perror("sched_rr_get_interval");
        return 1;
    }

    if (ts.tv_sec == 0 && ts.tv_nsec == 0) {
        printf("Current process is not SCHED_RR (time slice = 0)\n");
    } else {
        printf("SCHED_RR time slice: %ld sec %ld nsec\n",
               (long)ts.tv_sec, (long)ts.tv_nsec);
    }

    return 0;
}
```

## 7. 参考

- `kernel/sched/syscalls.c` — sched_rr_get_interval 实现
- `kernel/sched/rt.c` — get_rr_interval_rt 实现
- `include/linux/sched.h` — sched_class 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)