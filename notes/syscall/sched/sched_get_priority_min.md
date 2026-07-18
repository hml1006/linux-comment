# sched_get_priority_min 系统调用分析

## 1. 概述

获取指定调度策略的最低优先级。对于实时策略（`SCHED_FIFO`、`SCHED_RR`），返回 1；对于非实时策略，返回 0。

**原型：**

```c
SYSCALL_DEFINE1(sched_get_priority_min, int, policy)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `policy` | `int` | 调度策略（`SCHED_FIFO`、`SCHED_RR`、`SCHED_NORMAL` 等） |

**返回值：**

- 成功返回策略对应的最低优先级值
- 失败返回 `-EINVAL`（无效的策略值）

## 2. 策略优先级对应表

| 策略 | 最低优先级 | 说明 |
|------|-----------|------|
| `SCHED_FIFO` | 1 | 先进先出实时调度 |
| `SCHED_RR` | 1 | 时间片轮转实时调度 |
| `SCHED_DEADLINE` | 0 | 最晚截止时间优先 |
| `SCHED_NORMAL` | 0 | 普通分时调度（CFS） |
| `SCHED_BATCH` | 0 | 批处理调度 |
| `SCHED_IDLE` | 0 | 空闲调度 |
| `SCHED_EXT` | 0 | 扩展调度（sched_ext） |

## 3. 使用场景

- **实时任务配置**: 查询实时策略可用的最低优先级范围
- **调度器通用工具**: 验证给定策略的优先级下限
- **优先级分配**: 在设置实时优先级前检查合法范围

## 4. 内核实现

```c
// kernel/sched/syscalls.c
SYSCALL_DEFINE1(sched_get_priority_min, int, policy)
{
    int ret = -EINVAL;

    switch (policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        ret = 1;          // 实时策略最低优先级为 1
        break;
    case SCHED_DEADLINE:
    case SCHED_NORMAL:
    case SCHED_BATCH:
    case SCHED_IDLE:
    case SCHED_EXT:
        ret = 0;          // 非实时策略最低优先级为 0
        break;
    }
    return ret;
}
```

## 5. 关键数据结构

```c
// include/linux/sched/prio.h
#define MAX_USER_RT_PRIO    100     // 用户空间 RT 优先级数量
#define MAX_RT_PRIO         MAX_USER_RT_PRIO  // 100
#define MAX_PRIO            (MAX_RT_PRIO + NICE_WIDTH)  // 140

// 实时优先级范围：1 ~ 99
// 0 表示非实时任务
// 值越大，优先级越高
```

## 6. 流程图

```
sched_get_priority_min(SCHED_RR)
  │
  └─ switch(policy)
       ├─ case SCHED_FIFO:  → 返回 1
       ├─ case SCHED_RR:    → 返回 1
       ├─ case SCHED_NORMAL: → 返回 0
       ├─ case SCHED_BATCH:  → 返回 0
       ├─ case SCHED_IDLE:   → 返回 0
       └─ default: → 返回 -EINVAL
```

## 7. 使用示例

```c
#include <sched.h>
#include <stdio.h>

int main(void)
{
    int min;

    min = sched_get_priority_min(SCHED_FIFO);
    printf("SCHED_FIFO min priority: %d\n", min);  // 1

    min = sched_get_priority_min(SCHED_RR);
    printf("SCHED_RR min priority: %d\n", min);    // 1

    min = sched_get_priority_min(SCHED_NORMAL);
    printf("SCHED_NORMAL min priority: %d\n", min); // 0

    // 非法策略
    min = sched_get_priority_min(999);
    if (min == -1)
        printf("Invalid policy: EINVAL\n");

    return 0;
}
```

## 8. 参考

- `kernel/sched/syscalls.c` — sched_get_priority_min 实现
- `include/uapi/linux/sched.h` — 调度策略常量定义
- `include/linux/sched/prio.h` — 优先级常量定义
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)