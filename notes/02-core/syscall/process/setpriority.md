# setpriority 系统调用分析

> 所属分类：进程调度 | [ARM64 系统调用表](../arm64-syscall-table.md#9-进程调度)

## 1. 概述

`setpriority` 用于设置进程、进程组或某个用户的全部进程的 nice 值（静态优先级）。它比 `nice(2)` 更通用：`nice` 只能修改调用者自身且只接受增量，而 `setpriority` 可以按 `PRIO_PROCESS`、`PRIO_PGRP`、`PRIO_USER` 三种范围批量修改。

nice 值范围是 `-20`（最高优先级）到 `19`（最低优先级），默认 0。降低 nice 值（提高优先级）需要特权或满足 `RLIMIT_NICE`；提高 nice 值（降低优先级）总是允许的。

ARM64 系统调用号：`__NR_setpriority = 140`。

## 2. 函数原型与参数

用户空间原型：

```c
#include <sys/resource.h>

int setpriority(int which, id_t who, int prio);
```

内核入口（`kernel/sys.c:259`）：

```c
SYSCALL_DEFINE3(setpriority, int, which, int, who, int, niceval)
```

| 参数 | 说明 |
|------|------|
| `which` | `PRIO_PROCESS`(0)、`PRIO_PGRP`(1)、`PRIO_USER`(2)，其余值返回 `-EINVAL` |
| `who` | 进程 ID、进程组 ID 或用户 ID；传 0 表示当前进程/进程组/有效 UID |
| `niceval` | 目标 nice 值，内核会截断到 `[MIN_NICE, MAX_NICE]` 即 `[-20, 19]` |

`which` 常量定义于 `include/uapi/linux/resource.h:58` 起，取值为 0、1、2。注意参数是"目标值"而非增量；`nice(2)` 的增量语义由 `SYSCALL_DEFINE1(nice)` 在 `kernel/sched/syscalls.c:132` 中换算后再复用 `set_user_nice()`。

## 3. 内核实现路径

```
__arm64_sys_setpriority(which, who, niceval)
  └─ SYSCALL_DEFINE3(setpriority)                     // kernel/sys.c:259
       ├─ which > PRIO_USER || which < PRIO_PROCESS → -EINVAL
       ├─ niceval 截断到 [MIN_NICE, MAX_NICE]
       ├─ rcu_read_lock()
       └─ switch (which)
            ├─ PRIO_PROCESS:
            │    who ? find_task_by_vpid(who) : current
            │    └─ set_one_prio(p, niceval, error)   // kernel/sys.c:235
            ├─ PRIO_PGRP:
            │    who ? find_vpid(who) : task_pgrp(current)
            │    read_lock(&tasklist_lock)
            │    do_each_pid_thread(pgrp, PIDTYPE_PGID, p)
            │         set_one_prio(...)
            │    while_each_pid_thread(...)
            ├─ PRIO_USER:
            │    make_kuid(cred->user_ns, who) / cred->uid
            │    for_each_process_thread(g, p)
            │         若 task_uid(p) == uid 且 task_pid_vnr(p) != 0
            │             set_one_prio(...)
            └─ 返回最后一个错误码
       └─ rcu_read_unlock()

set_one_prio(p, niceval, error)                       // kernel/sys.c:235
  ├─ set_one_prio_perm(p)?                            // kernel/sys.c:219
  │    pcred->uid == cred->euid || pcred->euid == cred->euid
  │    或 ns_capable(pcred->user_ns, CAP_SYS_NICE)
  │    否 → -EPERM
  ├─ niceval < task_nice(p) && !can_nice(p, niceval) → -EACCES
  ├─ security_task_setnice(p, niceval)                // LSM 钩子
  ├─ error == -ESRCH → error = 0
  └─ set_user_nice(p, niceval)                        // kernel/sched/syscalls.c:65
```

`set_user_nice()`（注意 7.2 中位于 `kernel/sched/syscalls.c:65`，不是 `kernel/sched/core.c`）：

```c
void set_user_nice(struct task_struct *p, long nice)
{
    if (task_nice(p) == nice || nice < MIN_NICE || nice > MAX_NICE)
        return;
    guard(task_rq_lock)(p);                 // 锁定任务所在运行队列
    if (task_has_dl_policy(p) || task_has_rt_policy(p)) {
        p->static_prio = NICE_TO_PRIO(nice); // RT/DL 仅记录 nice，不影响调度
        return;
    }
    scoped_guard (sched_change, p, DEQUEUE_SAVE) {
        p->static_prio = NICE_TO_PRIO(nice);
        set_load_weight(p, true);           // 重新计算 CFS 权重
        old_prio = p->prio;
        p->prio = effective_prio(p);        // 更新动态优先级
    }
}
```

## 4. 核心数据结构

### 4.1 `struct task_struct` 中的优先级字段

```c
// include/linux/sched.h
struct task_struct {
    int              static_prio;   // nice 值映射后的静态优先级，范围 100..139
    int              normal_prio;   // 常规优先级（不含 PI/RT 提升）
    int              prio;          // 调度器实际使用的动态优先级
    unsigned int     policy;        // SCHED_NORMAL / SCHED_BATCH / SCHED_IDLE / RT / DL
    struct sched_entity se;         // CFS 调度实体，权重由 static_prio 决定
    struct sched_class *sched_class;
};
```

### 4.2 优先级换算（`include/linux/sched/prio.h`）

```c
#define MAX_NICE        19
#define MIN_NICE        -20
#define NICE_WIDTH      (MAX_NICE - MIN_NICE + 1)   // 40
#define MAX_RT_PRIO     100
#define MAX_PRIO        (MAX_RT_PRIO + NICE_WIDTH)  // 140
#define DEFAULT_PRIO    (MAX_RT_PRIO + NICE_WIDTH / 2) // 120

#define NICE_TO_PRIO(nice)  ((nice) + DEFAULT_PRIO)
#define PRIO_TO_NICE(prio)  ((prio) - DEFAULT_PRIO)
```

`task_nice(p)` 即 `PRIO_TO_NICE(p->static_prio)`。

### 4.3 权限判定

`can_nice()`（`kernel/sched/syscalls.c:118`）判断 `is_nice_reduction()`（`nice_to_rlimit(nice) <= task_rlimit(p, RLIMIT_NICE)`）或 `capable(CAP_SYS_NICE)`；`nice_to_rlimit()` 把 nice 映射为 rlimit 风格的正数。

## 5. 关键流程

```
setpriority(PRIO_PROCESS, pid, -5)
        │
        ▼
which 合法性检查（0..2）
        │
        ▼
niceval 截断到 [-20, 19]
        │
        ▼
按 which 找到目标 task（可能多个）
        │
        ▼
对每个 task 执行 set_one_prio()
  ├─ 权限：同 UID/EUID 或目标 userns 内 CAP_SYS_NICE？
  │     否 → -EPERM
  ├─ 若想降低 nice（提高优先级）：
  │     can_nice() 检查 RLIMIT_NICE / CAP_SYS_NICE
  │     否 → -EACCES
  ├─ LSM security_task_setnice()
  └─ set_user_nice()
        ├─ RT/DL 任务只更新 static_prio
        └─ CFS 任务：更新 static_prio、load_weight、prio
        │
        ▼
返回 0（或最后一个失败任务的错误码）
```

批量范围（`PRIO_PGRP`/`PRIO_USER`）采用"尽力而为"语义：只要有一个任务成功，整体就可能返回 0；错误码是循环中最后一次赋值，因此可能被后续成功覆盖。这与 `getpriority` 的"取最值"不同。

## 6. 返回值与错误码

成功返回 0。

| 错误码 | 触发条件 |
|--------|----------|
| `-EINVAL` | `which` 不在 `PRIO_PROCESS..PRIO_USER` 范围 |
| `-EPERM` | 调用者与目标任务 UID/EUID 不匹配，且没有对目标用户命名空间的 `CAP_SYS_NICE` |
| `-EACCES` | 试图降低 nice 值（提高优先级），但超出 `RLIMIT_NICE` 且无 `CAP_SYS_NICE` |
| `-ESRCH` | 指定的进程/进程组/用户不存在（或 `find_user()` 失败） |
| 其他负值 | LSM 的 `security_task_setnice()` 返回值 |

## 7. 使用示例

```c
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

int main(void)
{
    /* 将当前进程 nice 值设为 10（降低优先级，无需特权） */
    if (setpriority(PRIO_PROCESS, 0, 10) == -1) {
        fprintf(stderr, "setpriority: %s\n", strerror(errno));
        return 1;
    }
    printf("nice = %d\n", getpriority(PRIO_PROCESS, 0));

    /* 提高优先级需要 CAP_SYS_NICE 或足够的 RLIMIT_NICE */
    if (setpriority(PRIO_PROCESS, 0, -5) == -1)
        fprintf(stderr, "raise priority failed: %s\n", strerror(errno));

    /* 调整整个进程组 */
    if (setpriority(PRIO_PGRP, 0, 5) == -1)
        perror("setpriority(PRIO_PGRP)");

    return 0;
}
```

若在容器内运行，`CAP_SYS_NICE` 往往被丢弃，且 `RLIMIT_NICE` 默认可能为 0，此时只有"增大 nice 值"会成功。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `kernel/sys.c` | `SYSCALL_DEFINE3(setpriority)`、`set_one_prio()`、`set_one_prio_perm()` |
| `kernel/sched/syscalls.c` | `set_user_nice()`、`can_nice()`、`is_nice_reduction()`、`SYSCALL_DEFINE1(nice)` |
| `include/linux/sched/prio.h` | `MIN_NICE`/`MAX_NICE`、`NICE_TO_PRIO()`、`nice_to_rlimit()` |
| `include/uapi/linux/resource.h` | `PRIO_PROCESS`/`PRIO_PGRP`/`PRIO_USER` |
| `include/uapi/asm-generic/resource.h` | `RLIMIT_NICE` |
| `include/linux/sched.h` | `struct task_struct` 优先级字段、`task_nice()` |
