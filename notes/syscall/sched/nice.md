# nice 系统调用分析

## 1. 概述

`nice` 是传统的进程优先级调整系统调用，用于降低或恢复进程的优先级（nice 值）。在 ARM64 架构上，`nice` 没有独立的系统调用编号，而是通过 `setpriority`（syscall #141）模拟实现。

**功能说明：**
- 修改调用进程的 nice 值（-20 到 19）
- 较低的 nice 值表示更高的优先级
- 只有特权进程（`CAP_SYS_NICE`）才能降低 nice 值（提高优先级）

**原型：**

```c
#include <unistd.h>

int nice(int inc);
```

**参数：**

| 参数 | 说明 |
|------|------|
| `inc` | nice 值增量，新 nice = 当前 nice + inc |

**返回值：**

- 成功返回新的 nice 值
- 失败返回 -1 并设置 errno

## 2. 使用场景

- 降低后台任务的优先级，避免干扰前台交互
- 批处理作业中降低非关键任务的 CPU 调度优先级
- 系统管理脚本中调整进程优先级

## 3. 函数调用栈

```
nice(inc)  (glibc wrapper)
  └─ syscall(__NR_setpriority, PRIO_PROCESS, 0, inc)
       └─ __arm64_sys_setpriority(which, who, niceval)  // kernel/sys.c
            └─ set_one_prio(task, which, niceval)
                 ├─ task_nice(task)                     // 获取当前 nice 值
                 ├─ can_nice(task, niceval)             // 权限检查
                 ├─ 新的 nice 值 = inc + task_nice(task)
                 ├─ set_user_nice(task, niceval)        // 设置 nice 值
                 └─ 更新 task->static_prio              // 更新静态优先级
```

## 4. 关键数据结构

### struct task_struct 中的调度字段

```c
// include/linux/sched.h
struct task_struct {
    int                 static_prio;   // 静态优先级 (nice 值映射)
    int                 normal_prio;   // 常规优先级
    int                 prio;          // 动态优先级
    unsigned int        policy;        // 调度策略 (SCHED_NORMAL 等)
    struct sched_entity se;            // 调度实体（CFS 调度类）
    struct sched_class  *sched_class;  // 调度器类指针
};
```

### nice 值与优先级映射

```c
// include/linux/sched/prio.h
#define MAX_NICE        19
#define MIN_NICE        -20
#define NICE_WIDTH      (MAX_NICE - MIN_NICE + 1)

#define MAX_USER_RT_PRIO    100
#define MAX_RT_PRIO         MAX_USER_RT_PRIO  // 100
#define MAX_PRIO            (MAX_RT_PRIO + NICE_WIDTH)  // 140

#define DEFAULT_PRIO        (MAX_RT_PRIO + NICE_WIDTH / 2)  // 120
```

### nice 值与 static_prio 的转换

```c
// kernel/sched/core.c
// nice 值 → static_prio
#define NICE_TO_PRIO(nice)  ((nice) + DEFAULT_PRIO)
// static_prio → nice 值
#define PRIO_TO_NICE(prio)  ((prio) - DEFAULT_PRIO)
// 用户态 nice 值范围校验
#define TASK_NICE(p)        PRIO_TO_NICE((p)->static_prio)
```

## 5. 流程图

```
nice(5)     // 降低优先级（nice 值 +5）
  │
  ├─ glibc 封装 → syscall(__NR_setpriority, PRIO_PROCESS, 0, 5)
  │
  ├─ set_one_prio(current, PRIO_PROCESS, 5)
  │    ├─ can_nice(current, 5) → 检查权限
  │    │    └─ 如果当前 nice=0, 目标 nice=5, 非特权进程可以降低优先级
  │    │    └─ 如果当前 nice=0, 目标 nice=-5, 需要 CAP_SYS_NICE
  │    │
  │    ├─ set_user_nice(current, 0 + 5)
  │    │    ├─ task->static_prio = NICE_TO_PRIO(5) = 125
  │    │    ├─ task->prio = effective_prio(task)
  │    │    └─ 如果 task 在运行队列中，重新排队
  │    │
  │    └─ 返回 0
  │
  └─ 返回新的 nice 值 5
```

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    int ret;

    // 将当前进程的 nice 值增加 5（降低优先级）
    ret = nice(5);
    if (ret == -1) {
        perror("nice");
        return 1;
    }
    printf("New nice value: %d\n", ret);

    // 尝试提高优先级（需要 CAP_SYS_NICE）
    ret = nice(-10);
    if (ret == -1) {
        printf("Cannot increase priority (need CAP_SYS_NICE): %s\n",
               strerror(errno));
    }

    return 0;
}
```

## 7. 注意事项

- ARM64 上无独立 `nice` 系统调用号，通过 `setpriority` 模拟
- nice 值范围：-20（最高优先级）到 19（最低优先级），默认 0
- 只有特权进程（`CAP_SYS_NICE`）才能降低 nice 值（提高优先级）
- 非特权进程只能增加 nice 值（降低优先级），且不能超过上限 19

## 8. 参考

- `kernel/sys.c` — setpriority/nice 实现
- `kernel/sched/core.c` — set_user_nice 实现
- `include/linux/sched.h` — task_struct 定义
- `include/linux/sched/prio.h` — 优先级常量定义
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)