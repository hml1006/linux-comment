# sched_getaffinity 系统调用分析

## 1. 概述

获取进程的 CPU 亲和性（affinity）掩码，即进程可以运行在哪些 CPU 上。

**原型：**

```c
SYSCALL_DEFINE3(sched_getaffinity, pid_t, pid, unsigned int, len,
                unsigned long __user *, user_mask_ptr)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID（0 表示当前进程） |
| `len` | `unsigned int` | 用户态掩码缓冲区大小（字节） |
| `user_mask_ptr` | `unsigned long *` | 用户态 CPU 掩码缓冲区指针 |

**返回值：**

- 成功返回实际拷贝到用户态的掩码大小（字节数）
- 失败返回负值错误码：
  - `-EINVAL` — `len` 太小或未对齐
  - `-ESRCH` — 进程不存在
  - `-EFAULT` — 用户态指针无效
  - `-EPERM` — 权限不足

## 2. 使用场景

- **CPU 绑定查询**: 查看进程绑定到了哪些 CPU
- **负载均衡诊断**: 确认进程的 CPU 亲和性设置
- **容器管理**: 检查容器内进程的 CPU 限制

## 3. 函数调用栈

```
sched_getaffinity(pid, len, user_mask_ptr)  (系统调用入口)
└─ kernel/sched/syscalls.c
   ├─ 参数校验：len * 8 >= nr_cpu_ids, len 按 unsigned long 对齐
   ├─ zalloc_cpumask_var(&mask, GFP_KERNEL)  // 分配内核 cpumask
   │
   └─ sched_getaffinity(pid, mask)           // 获取亲和性
        ├─ find_process_by_pid(pid)          // RCU 查找 task_struct
        ├─ security_task_getscheduler(p)     // LSM 安全检查
        ├─ cpumask_and(mask, &p->cpus_mask, cpu_active_mask)
        │    └─ 取进程允许的 CPU 和在线 CPU 的交集
        └─ 返回 0
   │
   ├─ copy_to_user(user_mask_ptr, mask, retlen)  // 拷贝到用户态
   └─ free_cpumask_var(mask)
```

## 4. 关键数据结构

```c
// include/linux/sched.h
struct task_struct {
    ...
    const struct cpumask *cpus_ptr;       // 指向 cpus_mask 或 cpuset
    cpumask_t cpus_mask;                  // 实际的 CPU 亲和性掩码
    ...
};

// include/linux/cpumask.h
// cpumask 是一个位图，每个 bit 对应一个 CPU
// nr_cpu_ids 表示系统中实际存在的 CPU 数量
typedef struct cpumask { DECLARE_BITMAP(bits, NR_CPUS); } cpumask_t;

// 用户态接口 (include/uapi/linux/sched.h)
// CPU_SET() / CPU_ISSET() / CPU_ZERO() 等宏操作 cpu_set_t 类型
// cpu_set_t 的大小由内核配置决定
```

## 5. 流程图

```
sched_getaffinity(0, 128, &mask)
  │
  ├─ 检查 len=128, nr_cpu_ids=8
  │    └─ 128*8=1024 >= 8, len 按 8 对齐 → 通过
  │
  ├─ 分配 cpumask_var_t mask
  │
  ├─ sched_getaffinity(0, mask)
  │    ├─ find_process_by_pid(0) → current
  │    ├─ security_task_getscheduler(current) → OK
  │    └─ cpumask_and(mask, &current->cpus_mask, cpu_active_mask)
  │         → 例如 mask = {0, 1, 2, 3}  (4 个活跃 CPU)
  │
  ├─ copy_to_user(user_mask_ptr, mask, 8)
  │    └─ 返回 8
  │
  └─ free_cpumask_var(mask)
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
    int i, ret;

    CPU_ZERO(&mask);

    // 获取当前进程的 CPU 亲和性
    ret = sched_getaffinity(0, sizeof(mask), &mask);
    if (ret == -1) {
        perror("sched_getaffinity");
        return 1;
    }

    printf("Process %d can run on CPUs: ", getpid());
    for (i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &mask))
            printf("%d ", i);
    }
    printf("\n");

    return 0;
}
```

## 7. 注意事项

- 掩码长度 `len` 必须足够大以容纳所有 CPU（`len * 8 >= nr_cpu_ids`）
- 返回值是实际拷贝到用户态的字节数，而非 0
- 内核会取 `cpus_mask` 和 `cpu_active_mask` 的交集，所以返回的 CPU 均在线的
- 对于 PID 0 表示当前进程

## 8. 参考

- `kernel/sched/syscalls.c` — sched_getaffinity 实现
- `include/linux/cpumask.h` — cpumask 操作宏
- `include/uapi/linux/sched.h` — CPU_SET 等宏定义
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)