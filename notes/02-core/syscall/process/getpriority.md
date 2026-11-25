# getpriority 系统调用分析

> 所属分类：进程调度 | [ARM64 系统调用表](../arm64-syscall-table.md#9-进程调度)

## 1. 概述

`getpriority` 查询进程、进程组或某个用户全部进程的 nice 值。它的返回值有一个历史遗留的"偏移"设计：为了避免返回负数与错误码混淆，内核返回的是 `20 - nice`，即 nice 值 `-20..19` 映射为 `40..1`。glibc 包装函数再减去 20 还原为 nice 值。

当查询范围包含多个进程时，POSIX 规定返回其中**数值最大**（也就是优先级最低）的 nice 值；内核正是这样实现的：用 `nice_to_rlimit()` 转换后取最大值。

ARM64 系统调用号：`__NR_getpriority = 141`。

## 2. 函数原型与参数

用户空间原型：

```c
#include <sys/resource.h>

int getpriority(int which, id_t who);
```

内核入口（`kernel/sys.c:329`）：

```c
SYSCALL_DEFINE2(getpriority, int, which, int, who)
```

| 参数 | 说明 |
|------|------|
| `which` | `PRIO_PROCESS`(0)、`PRIO_PGRP`(1)、`PRIO_USER`(2) |
| `who` | 进程 ID / 进程组 ID / 用户 ID；0 表示当前进程、当前进程组或调用者有效 UID |

原始系统调用返回值范围是 `1..40`（`nice_to_rlimit()` 的结果），不是 `-20..19`。`include/uapi/linux/resource.h` 定义：

```c
#define PRIO_PROCESS    0
#define PRIO_PGRP       1
#define PRIO_USER       2
```

## 3. 内核实现路径

```
__arm64_sys_getpriority(which, who)
  └─ SYSCALL_DEFINE2(getpriority)                    // kernel/sys.c:329
       ├─ which > PRIO_USER || which < PRIO_PROCESS → -EINVAL
       ├─ retval = -ESRCH
       ├─ rcu_read_lock()
       └─ switch (which)
            ├─ PRIO_PROCESS:
            │    who ? find_task_by_vpid(who) : current
            │    niceval = nice_to_rlimit(task_nice(p))
            │    retval = max(retval, niceval)
            ├─ PRIO_PGRP:
            │    who ? find_vpid(who) : task_pgrp(current)
            │    read_lock(&tasklist_lock)
            │    do_each_pid_thread(pgrp, PIDTYPE_PGID, p)
            │         retval = max(retval, nice_to_rlimit(task_nice(p)))
            │    while_each_pid_thread(...)
            ├─ PRIO_USER:
            │    uid = make_kuid(cred->user_ns, who)
            │    user = who ? find_user(uid) : cred->user
            │    for_each_process_thread(g, p)
            │         若 task_uid(p) == uid 且 task_pid_vnr(p) != 0
            │             retval = max(retval, nice_to_rlimit(task_nice(p)))
            └─ 返回 retval
       └─ rcu_read_unlock()

nice_to_rlimit(nice)                                 // include/linux/sched/prio.h
  = MAX_NICE - nice + 1                              // 19 - nice + 1
```

要点：

1. `retval` 初值为 `-ESRCH`，但随后一定会被 `nice_to_rlimit()` 的正值覆盖（找到任务时），所以"成功返回值恒为正数"。
2. 对 `PRIO_PGRP` 需要 `tasklist_lock` 读锁保护遍历；`PRIO_USER` 在 RCU 保护下用 `for_each_process_thread()` 扫描全进程表。
3. `PRIO_USER` 路径会 `find_user()` 增加 user 引用计数，结束后 `free_uid()` 释放。

## 4. 核心数据结构

### 4.1 任务优先级字段

```c
// include/linux/sched.h
struct task_struct {
    int static_prio;    // nice 值映射后的静态优先级
    int normal_prio;
    int prio;
    unsigned int policy;
};
```

`task_nice(p)` 定义为：

```c
#define task_nice(p)    PRIO_TO_NICE((p)->static_prio)
```

### 4.2 nice 与 rlimit 风格值的换算（`include/linux/sched/prio.h`）

```c
#define MAX_NICE    19
#define MIN_NICE    -20
#define DEFAULT_PRIO (MAX_RT_PRIO + NICE_WIDTH / 2)   // 120

#define NICE_TO_PRIO(nice)  ((nice) + DEFAULT_PRIO)
#define PRIO_TO_NICE(prio)  ((prio) - DEFAULT_PRIO)

static inline long nice_to_rlimit(long nice)
{
    return (MAX_NICE - nice + 1);
}

static inline long rlimit_to_nice(long prio)
{
    return (MAX_NICE - prio + 1);
}
```

映射示例：

| nice | `nice_to_rlimit()` | glibc `getpriority()` 返回 |
|------|--------------------|---------------------------|
| -20  | 40 | -20 |
| 0    | 20 | 0 |

### 4.3 PID 查找

- `find_task_by_vpid(who)`：按当前 PID 命名空间的虚拟 PID 查找任务，需要 RCU 读锁。
- `find_vpid(who)`：返回 `struct pid *`，用于进程组遍历。

## 5. 关键流程

```
getpriority(PRIO_PROCESS, pid)
        │
        ▼
which 合法性检查
        │
        ▼
retval = -ESRCH
        │
        ▼
按 which 选择目标集合
  ├─ 单个 task：find_task_by_vpid / current
  ├─ 进程组：tasklist_lock + do_each_pid_thread
  └─ 用户：for_each_process_thread + uid 过滤
        │
        ▼
对每个目标：
  nice = PRIO_TO_NICE(task->static_prio)
  retval = max(retval, 19 - nice + 1)
        │
        ▼
返回 retval（1..40，或 -ESRCH/-EINVAL）
        │
        ▼
glibc 包装：ret == -1 && errno == 0 ? 返回 -1 : 返回 ret - 20
```

注意最大值的语义：`nice_to_rlimit()` 与 nice 值反向，因此"取最大 rlimit 值"等价于"取最小 nice 值"，也就是"最低优先级"。这正是 POSIX 对多进程查询的要求。

## 6. 返回值与错误码

| 返回值 | 含义 |
|--------|------|
| `1..40` | 原始系统调用返回 `20 - nice`；glibc 包装减去 20 后得到 `-20..19` |
| `-EINVAL` | `which` 非法 |
| `-ESRCH` | 目标进程/进程组/用户不存在 |

glibc 的 `getpriority()` 存在经典的 `-1` 歧义：合法 nice 值 -1 与错误返回 -1 无法仅凭返回值区分。glibc 的做法是调用前把 `errno` 清零，返回 -1 时检查 `errno` 是否仍为 0。

## 7. 使用示例

```c
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

int main(void)
{
    errno = 0;
    int prio = getpriority(PRIO_PROCESS, 0);
    if (prio == -1 && errno != 0) {
        fprintf(stderr, "getpriority: %s\n", strerror(errno));
        return 1;
    }
    printf("nice = %d\n", prio);

    /* 查询整个进程组：返回组内最低优先级（最大 nice） */
    errno = 0;
    int pgrp_prio = getpriority(PRIO_PGRP, 0);
    if (pgrp_prio == -1 && errno != 0)
        perror("getpriority(PRIO_PGRP)");
    else
        printf("process group nice = %d\n", pgrp_prio);

    /* 直接走原始系统调用可观察 1..40 的编码 */
    long raw = syscall(__NR_getpriority, PRIO_PROCESS, 0);
    printf("raw syscall = %ld (nice = %ld)\n", raw, raw - 20);

    return 0;
}
```

原始系统调用返回 `1..40`，因此不能用 `-1` 判断错误；应检查返回值是否落在 `1..40` 之外。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `kernel/sys.c` | `SYSCALL_DEFINE2(getpriority)`（329 行） |
| `include/linux/sched/prio.h` | `nice_to_rlimit()`、`rlimit_to_nice()`、优先级常量 |
| `include/linux/sched.h` | `struct task_struct`、`task_nice()` |
| `include/uapi/linux/resource.h` | `PRIO_PROCESS`/`PRIO_PGRP`/`PRIO_USER` |
| `kernel/pid.c` | `find_task_by_vpid()`、`find_vpid()` |
| `arch/arm64/include/generated/uapi/asm/unistd_64.h` | `__NR_getpriority = 141` |
