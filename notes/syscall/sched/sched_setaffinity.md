# sched_setaffinity 系统调用分析

## 1. 概述

设置进程的 CPU 亲和性（affinity）掩码，将进程绑定到指定的 CPU 或 CPU 集合上运行。

**原型：**

```c
SYSCALL_DEFINE3(sched_setaffinity, pid_t, pid, unsigned int, len,
                unsigned long __user *, user_mask_ptr)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID（0 表示当前进程） |
| `len` | `unsigned int` | 用户态掩码缓冲区大小（字节） |
| `user_mask_ptr` | `unsigned long *` | 用户态 CPU 掩码缓冲区指针 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EINVAL` — 无效的掩码（空掩码或超出 CPU 范围）
  - `-ESRCH` — 进程不存在
  - `-EFAULT` — 用户态指针无效
  - `-EPERM` — 权限不足
  - `-ENOMEM` — 内存不足

## 2. 使用场景

- **CPU 绑定**: 将计算密集型任务绑定到特定 CPU
- **性能优化**: 避免 CPU 缓存失效（cache ping-pong）
- **隔离控制**: 将关键任务隔离到专用 CPU
- **容器管理**: 限制容器内进程使用的 CPU

## 3. 函数调用栈

```
sched_setaffinity(pid, len, user_mask_ptr)  (系统调用入口)
└─ kernel/sched/syscalls.c
   ├─ alloc_cpumask_var(&new_mask, GFP_KERNEL)  // 分配内核 cpumask
   │
   ├─ get_user_cpu_mask(user_mask_ptr, len, new_mask)  // 从用户态拷贝掩码
   │
   └─ sched_setaffinity(pid, new_mask)          // 核心实现
        ├─ find_process_by_pid(pid)             // RCU 查找 task_struct
        ├─ cpus_allowed = new_mask & cpu_possible_mask  // 取有效 CPU
        ├─ security_task_setscheduler(p)        // LSM 安全钩子
        │
        ├─ [权限检查]
        │  ├─ 如果新掩码是当前掩码的子集 → 允许
        │  └─ 否则需要 CAP_SYS_NICE
        │
        ├─ cpuset_cpus_allowed(p) 检查          // cpuset 约束
        ├─ dl_task_check_affinity(p, new_mask)  // Deadline 任务检查
        │
        └─ __set_cpus_allowed_ptr(p, new_mask)  // 实际设置亲和性
             ├─ 更新 p->cpus_ptr 和 p->cpus_mask
             ├─ 如果进程正在运行，触发 CPU 迁移
             └─ stop_one_cpu() 推送任务到目标 CPU
```

## 4. 关键数据结构

```c
// include/linux/sched.h
struct task_struct {
    ...
    const struct cpumask *cpus_ptr;       // 指向 cpus_mask 或 cpuset 的掩码
    cpumask_t cpus_mask;                  // 实际的 CPU 亲和性掩码
    unsigned int nr_cpus_allowed;         // 允许的 CPU 数量
    ...
};

// include/linux/cpumask.h
// cpumask 位图操作
// cpu_possible_mask   - 系统中可能存在的所有 CPU
// cpu_online_mask     - 当前在线的 CPU
// cpu_present_mask    - 实际存在的 CPU
// cpu_active_mask     - 可调度的活跃 CPU

// 用户态接口 (cpu_set_t 操作宏)
// CPU_ZERO(&mask)     - 清除所有位
// CPU_SET(cpu, &mask) - 设置指定 CPU
// CPU_CLR(cpu, &mask) - 清除指定 CPU
```

## 5. 流程图

```
sched_setaffinity(0, 128, &mask)   // 绑定到 CPU 0-3
  │
  ├─ 用户态设置 mask = {0, 1, 2, 3}
  │
  ├─ get_user_cpu_mask() → new_mask = {0, 1, 2, 3}
  │
  ├─ find_process_by_pid(0) → current
  │
  ├─ 权限检查：新掩码是当前掩码的子集 → 允许
  │
  ├─ __set_cpus_allowed_ptr(current, new_mask)
  │    ├─ current->cpus_mask = {0, 1, 2, 3}
  │    ├─ 如果当前进程正在其他 CPU 上运行
  │    │    └─ stop_one_cpu() → 迁移到 {0, 1, 2, 3} 中之一
  │    └─ 更新 nr_cpus_allowed = 4
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

int main(void)
{
    cpu_set_t mask;
    int ret;

    // 限制当前进程到 CPU 0 和 CPU 1
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);
    CPU_SET(1, &mask);

    ret = sched_setaffinity(0, sizeof(mask), &mask);
    if (ret == -1) {
        perror("sched_setaffinity");
        return 1;
    }

    printf("Process %d bound to CPU 0 and 1\n", getpid());
    return 0;
}
```

## 7. 注意事项

- 掩码必须非空（至少设置一个 CPU），否则返回 `-EINVAL`
- 非特权进程只能将任务绑定到当前掩码的子集
- 对于 `SCHED_DEADLINE` 任务，需要额外检查带宽约束
- 设置亲和性可能触发进程跨 CPU 迁移，有性能开销

## 8. 参考

- `kernel/sched/syscalls.c` — sched_setaffinity 实现
- `kernel/sched/core.c` — __set_cpus_allowed_ptr 实现
- `include/linux/cpumask.h` — cpumask 操作宏
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)