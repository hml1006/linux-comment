# ioprio_set 系统调用分析

> 所属分类：进程调度（I/O 优先级） | [ARM64 系统调用表](../arm64-syscall-table.md#9-进程调度)

## 1. 概述

`ioprio_set` 设置进程、进程组或某个用户全部进程的 I/O 调度优先级。它不改变 CPU 调度优先级，而是影响块层 I/O 调度器（如 BFQ、mq-deadline）的请求排序与配额。I/O 优先级由"类别（class）+ 等级（level）+ 可选提示（hint）"编码进一个 `int`。

7.2 内核的布局：bit 0-2 为 level，bit 3-12 为 hint，bit 13-15 为 class。class 包括 `NONE`、`RT`、`BE`、`IDLE`，其中 `RT` 需要特权。

ARM64 系统调用号：`__NR_ioprio_set = 30`。

## 2. 函数原型与参数

```c
#include <linux/ioprio.h>

int ioprio_set(int which, int who, int ioprio);
```

内核入口（`block/ioprio.c:65`）：

```c
SYSCALL_DEFINE3(ioprio_set, int, which, int, who, int, ioprio)
```

| 参数 | 说明 |
|------|------|
| `which` | `IOPRIO_WHO_PROCESS`(1)、`IOPRIO_WHO_PGRP`(2)、`IOPRIO_WHO_USER`(3) |
| `who` | 进程/进程组/用户 ID；0 表示当前进程、当前进程组、当前用户 |
| `ioprio` | 编码后的 I/O 优先级 |

编码宏（`include/uapi/linux/ioprio.h`）：

```c
#define IOPRIO_CLASS_SHIFT  13
#define IOPRIO_CLASS_MASK   (IOPRIO_NR_CLASSES - 1)   /* 8 个类别 */
#define IOPRIO_PRIO_CLASS(ioprio) (((ioprio) >> 13) & 7)
#define IOPRIO_PRIO_DATA(ioprio)  ((ioprio) & ((1UL << 13) - 1))

#define IOPRIO_LEVEL_NR_BITS 3
#define IOPRIO_LEVEL_MASK    7
#define IOPRIO_PRIO_LEVEL(ioprio) ((ioprio) & 7)

#define IOPRIO_HINT_SHIFT    3
#define IOPRIO_HINT_MASK     ((1 << 10) - 1)
#define IOPRIO_PRIO_HINT(ioprio) \
    (((ioprio) >> IOPRIO_HINT_SHIFT) & IOPRIO_HINT_MASK)
```

推荐用 `IOPRIO_PRIO_VALUE(class, level)` 构造；该内联函数在参数越界时返回 `IOPRIO_CLASS_INVALID << 13`。

## 3. 内核实现路径

```
__arm64_sys_ioprio_set(which, who, ioprio)
  └─ SYSCALL_DEFINE3(ioprio_set)                  // block/ioprio.c:65
       ├─ ioprio_check_cap(ioprio)                // block/ioprio.c:33
       │    ├─ RT: 需要 CAP_SYS_ADMIN 或 CAP_SYS_NICE，否则 -EPERM
       │    ├─ BE / IDLE: 允许
       │    ├─ NONE: level 必须为 0，否则 -EINVAL
       │    └─ INVALID / 其他: -EINVAL
       ├─ ret = -ESRCH
       ├─ rcu_read_lock()
       └─ switch (which)
            ├─ PROCESS: find_task_by_vpid(who) 或 current
            │    └─ set_task_ioprio(p, ioprio)   // block/blk-ioc.c:244
            ├─ PGRP: task_pgrp(current)/find_vpid
            │    read_lock(&tasklist_lock)
            │    do_each_pid_thread()：任一失败立即返回
            ├─ USER: make_kuid + find_user
            │    for_each_process_thread()：uid 匹配且可见
            └─ default: -EINVAL
       └─ rcu_read_unlock()
```

`set_task_ioprio()`（`block/blk-ioc.c:244`）核心逻辑：

```c
int set_task_ioprio(struct task_struct *task, int ioprio)
{
    /* UID/EUID 不匹配且无 CAP_SYS_NICE → -EPERM */
    err = security_task_setioprio(task, ioprio);   // LSM 钩子
    if (err)
        return err;

    task_lock(task);
    if (unlikely(!task->io_context)) {
        /* 延迟分配 io_context；失败返回 -ENOMEM */
    }
    task->io_context->ioprio = ioprio;
    task_unlock(task);
    return 0;
}
```

## 4. 核心数据结构

### 4.1 I/O 优先级类别与 which（`include/uapi/linux/ioprio.h`）

```c
enum {
    IOPRIO_CLASS_NONE    = 0,   /* 未显式设置，由 nice 值推导 */
    IOPRIO_CLASS_RT      = 1,   /* 实时，需 CAP_SYS_ADMIN/CAP_SYS_NICE */
    IOPRIO_CLASS_BE      = 2,   /* best-effort，默认 */
    IOPRIO_CLASS_IDLE    = 3,   /* 仅空闲时处理 */
    IOPRIO_CLASS_INVALID = 7,
};

enum { IOPRIO_WHO_PROCESS = 1, IOPRIO_WHO_PGRP, IOPRIO_WHO_USER };
```

RT 与 BE 支持 8 个 level（`0` 最高），IDLE 忽略 level。

### 4.2 struct io_context（`include/linux/iocontext.h`）

`io_context` 保存用户设置的原始优先级字段 `unsigned short ioprio`，配合 `refcount`/`active_ref` 管理生命周期；`task->io_context` 可能为 `NULL`，此时按需分配，`CLONE_IO` 创建的线程共享同一个实例。

### 4.3 未设置时的推导（`include/linux/ioprio.h`）

```c
#define IOPRIO_DEFAULT  IOPRIO_PRIO_VALUE(IOPRIO_CLASS_NONE, 0)

static inline int task_nice_ioprio(struct task_struct *task)
{
    return (task_nice(task) + 20) / 5;      /* nice [-20,19] → level [0,7] */
}

static inline int task_nice_ioclass(struct task_struct *task)
{
    if (task->policy == SCHED_IDLE)
        return IOPRIO_CLASS_IDLE;
    else if (rt_or_dl_task_policy(task))
        return IOPRIO_CLASS_RT;
    else
        return IOPRIO_CLASS_BE;
}
```

## 5. 关键流程

```
ioprio_set(IOPRIO_WHO_PROCESS, tid, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 4))
        │
        ├─ ioprio_check_cap()
        │    ├─ RT 无权限 → EPERM
        │    ├─ NONE 且 level != 0 → EINVAL
        │    └─ class 非法 → EINVAL
        ├─ 按 which 选定目标任务集合
        ├─ set_task_ioprio()
        │    ├─ UID/EUID 或 CAP_SYS_NICE
        │    ├─ security_task_setioprio()
        │    ├─ 必要时分配 io_context
        │    └─ io_context->ioprio = ioprio
        └─ 返回 0
```

设置不会立即触发调度；下一次 I/O 提交时块层读取 `get_current_ioprio()` 决定请求的 `ioprio`。

## 6. 返回值与错误码

成功返回 0。

| 错误码 | 触发条件 |
|--------|----------|
| `-EINVAL` | `which` 非法；class 为 `INVALID`/未知；class 为 `NONE` 但 level 非 0 |
| `-EPERM` | 设置 RT 但无 `CAP_SYS_ADMIN`/`CAP_SYS_NICE`；目标 UID 不匹配且无 `CAP_SYS_NICE` |
| `-ESRCH` | 目标进程/进程组/用户不存在 |
| `-ENOMEM` | 分配 `io_context` 失败 |
| 其他负值 | LSM `security_task_setioprio()` 返回值 |

`IOPRIO_WHO_PGRP` 路径一旦某个任务失败立即返回，不再处理其余任务。

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/ioprio.h>

int main(void)
{
    int prio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 2);

    if (syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, 0, prio) == -1) {
        fprintf(stderr, "ioprio_set: %s\n", strerror(errno));
        return 1;
    }
    printf("class=%d level=%d\n",
           IOPRIO_PRIO_CLASS(prio), IOPRIO_PRIO_LEVEL(prio));

    /* IDLE：仅在磁盘空闲时服务 */
    prio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0);
    if (syscall(__NR_ioprio_set, IOPRIO_WHO_PGRP, 0, prio) == -1)
        perror("ioprio_set(PGRP)");

    return 0;
}
```

`include/uapi/linux/ioprio.h` 可直接被用户态包含；其 `linux/stddef.h` 已为 `__always_inline` 提供定义。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `block/ioprio.c` | `SYSCALL_DEFINE3(ioprio_set)`（65）、`ioprio_check_cap()`（33） |
| `block/blk-ioc.c` | `set_task_ioprio()`（244）、`alloc_io_context()` |
| `include/uapi/linux/ioprio.h` | 类别/等级/hint 编码、`IOPRIO_WHO_*` |
| `include/linux/ioprio.h` | `__get_task_ioprio()`、`task_nice_ioprio()`、`IOPRIO_DEFAULT` |
| `arch/arm64/include/generated/uapi/asm/unistd_64.h` | `__NR_ioprio_set = 30` |
