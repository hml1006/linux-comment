# ioprio_get 系统调用分析

> 所属分类：进程调度（I/O 优先级） | [ARM64 系统调用表](../arm64-syscall-table.md#9-进程调度)

## 1. 概述

`ioprio_get` 查询进程、进程组或某个用户全部进程的 I/O 调度优先级。返回值为编码后的 `ioprio`：高 3 位是 class，中间 10 位是 hint，低 3 位是 level。

它有两个容易混淆的语义分支：

- `IOPRIO_WHO_PROCESS` 返回**用户显式设置的原始值**（`task->io_context->ioprio`；未设置时返回 `IOPRIO_CLASS_NONE`，即 0），保留"未设置"与"显式设为默认"的区别；
- `IOPRIO_WHO_PGRP` / `IOPRIO_WHO_USER` 返回**归一化后的实际优先级**：未显式设置时按 CPU 调度策略与 nice 值推导；多任务时取数值最小（优先级最高）者。

ARM64 系统调用号：`__NR_ioprio_get = 31`。

## 2. 函数原型与参数

```c
#include <linux/ioprio.h>

int ioprio_get(int which, int who);
```

内核入口（`block/ioprio.c:180`）：

```c
SYSCALL_DEFINE2(ioprio_get, int, which, int, who)
```

| 参数 | 说明 |
|------|------|
| `which` | `IOPRIO_WHO_PROCESS`(1)、`IOPRIO_WHO_PGRP`(2)、`IOPRIO_WHO_USER`(3) |
| `who` | 进程 ID / 进程组 ID / 用户 ID；0 表示当前进程、当前进程组、当前用户 |

返回值用以下宏解码（`include/uapi/linux/ioprio.h`）：

```c
#define IOPRIO_PRIO_CLASS(ioprio) (((ioprio) >> 13) & 7)
#define IOPRIO_PRIO_LEVEL(ioprio) ((ioprio) & 7)
#define IOPRIO_PRIO_HINT(ioprio)  (((ioprio) >> 3) & 0x3ff)
```

## 3. 内核实现路径

```
__arm64_sys_ioprio_get(which, who)
  └─ SYSCALL_DEFINE2(ioprio_get)                    // block/ioprio.c:180
       ├─ ret = -ESRCH
       ├─ rcu_read_lock()
       └─ switch (which)
            ├─ PROCESS: find_task_by_vpid(who) / current
            │    └─ get_task_raw_ioprio(p)          // 158
            │         ├─ security_task_getioprio(p)
            │         ├─ task_lock(p)
            │         ├─ p->io_context ? ioc->ioprio : IOPRIO_DEFAULT
            │         └─ task_unlock(p)
            ├─ PGRP: task_pgrp(current) / find_vpid
            │    read_lock(&tasklist_lock)
            │    do_each_pid_thread():
            │         tmpio = get_task_ioprio(p)     // 138
            │         ret = (ret == -ESRCH) ? tmpio : ioprio_best(ret, tmpio)
            ├─ USER: make_kuid + find_user
            │    for_each_process_thread()：uid 匹配且 task_pid_vnr(p) != 0
            │         → get_task_ioprio() + ioprio_best()
            └─ default: -EINVAL
       └─ rcu_read_unlock()
```

`get_task_ioprio()`（138）在 `task_lock()` 下调用 `__get_task_ioprio()`（`include/linux/ioprio.h`）：

```c
static inline int __get_task_ioprio(struct task_struct *p)
{
    struct io_context *ioc = p->io_context;
    int prio;

    if (!ioc)
        return IOPRIO_PRIO_VALUE(task_nice_ioclass(p), task_nice_ioprio(p));

    prio = ioc->ioprio;
    if (IOPRIO_PRIO_CLASS(prio) == IOPRIO_CLASS_NONE)
        prio = IOPRIO_PRIO_VALUE(task_nice_ioclass(p), task_nice_ioprio(p));
    return prio;
}
```

`ioprio_best()`（175）就是取较小值：

```c
static int ioprio_best(unsigned short aprio, unsigned short bprio)
{
    return min(aprio, bprio);
}
```

由于 class 位于高位，`RT(1) < BE(2) < IDLE(3)`，且同类别内 level 越小优先级越高，`min()` 选出的正是**最高 I/O 优先级**。

## 4. 核心数据结构

### 4.1 struct io_context（`include/linux/iocontext.h`）

```c
struct io_context {
    atomic_long_t refcount;
    atomic_t active_ref;
    unsigned short ioprio;   /* 用户设置的原始 ioprio */
    /* ... */
};
```

该指针可以为 `NULL`（任务从未设置 I/O 优先级且未参与块 I/O），查询路径必须处理。

### 4.2 编码布局

| 位域 | 位置 | 说明 |
|------|------|------|
| level | bit 0-2 | 0 最高，7 最低 |
| hint | bit 3-12 | 10 位 I/O 提示（如设备命令时长限制） |
| class | bit 13-15 | 0=NONE, 1=RT, 2=BE, 3=IDLE, 7=INVALID |

```c
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_HINT_SHIFT  3
#define IOPRIO_NR_CLASSES  8
#define IOPRIO_NR_HINTS    (1 << 10)
#define IOPRIO_NR_LEVELS   (1 << 3)
```

### 4.3 默认值推导（`include/linux/ioprio.h`）

`task_nice_ioprio()` 把 nice 映射为 level（`(nice + 20) / 5`）；`task_nice_ioclass()` 对 `SCHED_IDLE` 返回 `IDLE`，对 RT/DL 返回 `RT`，其余返回 `BE`。nice 到 level 的映射：`-20→0`、`0→4`、`19→7`。

### 4.4 查询语义对比

| which | 使用的函数 | 未设置时返回 | 多任务归并 |
|-------|------------|--------------|------------|
| `PROCESS` | `get_task_raw_ioprio()` | `IOPRIO_DEFAULT`（0） | 不适用 |
| `PGRP` | `get_task_ioprio()` | 由 nice/policy 推导 | `min()` |
| `USER` | `get_task_ioprio()` | 由 nice/policy 推导 | `min()` |

## 5. 关键流程

```
ioprio_get(PROCESS, tid)
        ├─ find_task_by_vpid(tid) / current
        ├─ get_task_raw_ioprio()
        │    ├─ task_lock()
        │    ├─ io_context ? io_context->ioprio : 0
        │    └─ task_unlock()
        └─ 返回原始值（0 表示未设置）

ioprio_get(PGRP/USER, id)
        ├─ 遍历进程组 / 全进程表中同 uid 的可见任务
        ├─ get_task_ioprio() → __get_task_ioprio()
        │    └─ class == NONE 时按 nice + policy 推导
        ├─ ret = min(ret, tmpio)
        └─ 返回最高 I/O 优先级
```

## 6. 返回值与错误码

成功时返回非负的 `ioprio` 编码值（0 也是合法值，表示 `IOPRIO_CLASS_NONE`，需结合 `which` 解释）。

| 错误码 | 触发条件 |
|--------|----------|
| `-ESRCH` | 目标进程/进程组不存在，或用户没有任何可见进程 |
| `-EINVAL` | `which` 非法 |
| 其他负值 | LSM `security_task_getioprio()` 返回值 |

对 `PRIO_USER` 查询，`find_user()` 失败或用户无可见进程时保持 `-ESRCH`。

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/ioprio.h>

static const char *class_name(int c)
{
    switch (c) {
    case IOPRIO_CLASS_NONE: return "none";
    case IOPRIO_CLASS_RT:   return "rt";
    case IOPRIO_CLASS_BE:   return "be";
    case IOPRIO_CLASS_IDLE: return "idle";
    default:                return "invalid";
    }
}

int main(void)
{
    long prio = syscall(__NR_ioprio_get, IOPRIO_WHO_PROCESS, 0);
    if (prio < 0) {
        fprintf(stderr, "ioprio_get: %s\n", strerror(errno));
        return 1;
    }
    printf("raw=%ld class=%s level=%d hint=%d\n",
           prio, class_name(IOPRIO_PRIO_CLASS(prio)),
           IOPRIO_PRIO_LEVEL(prio), IOPRIO_PRIO_HINT(prio));

    return 0;
}
```

若 `PROCESS` 查询返回 0，说明该任务从未调用 `ioprio_set`，实际 I/O 优先级由 nice 与调度策略动态推导；用 `PGRP` 查询可看到归一化后的值。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `block/ioprio.c` | `SYSCALL_DEFINE2(ioprio_get)`（180）、`get_task_ioprio()`（138）、`get_task_raw_ioprio()`（158）、`ioprio_best()`（175） |
| `include/linux/ioprio.h` | `__get_task_ioprio()`、`task_nice_ioprio()`、`task_nice_ioclass()` |
| `include/uapi/linux/ioprio.h` | `IOPRIO_PRIO_CLASS/LEVEL/HINT` 解码、类别枚举 |
| `include/linux/iocontext.h` | `struct io_context` |
| `arch/arm64/include/generated/uapi/asm/unistd_64.h` | `__NR_ioprio_get = 31` |
