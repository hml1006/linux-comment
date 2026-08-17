# prctl 系统调用分析

## 1. 概述

`prctl` 是 Linux 的进程控制接口，提供多种进程级别的操作，包括设置进程名、信号处理、安全特性、性能监控等。它是一个多功能系统调用，通过 `option` 参数选择具体操作。

### 关键特点

- 多功能系统调用，通过 `option` 参数区分超过 50 种操作
- 首先调用 `security_task_prctl` 让 LSM（SELinux/AppArmor）优先处理
- 处理完成后直接返回，不执行后续的 switch 分支
- 如果 LSM 返回 `-ENOSYS`（未处理），则内核自行处理

---

## 2. 函数原型

```c
#include <sys/prctl.h>

int prctl(int option, unsigned long arg2, unsigned long arg3,
          unsigned long arg4, unsigned long arg5);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `option` | 操作类型（PR_* 宏） |
| `arg2`-`arg5` | 选项相关参数，含义取决于 `option` |

### 内核入口

```c
// kernel/sys.c:2534
SYSCALL_DEFINE5(prctl, int, option, unsigned long, arg2, unsigned long, arg3,
                unsigned long, arg4, unsigned long, arg5)
{
    struct task_struct *me = current;
    unsigned char comm[sizeof(me->comm)];
    long error;

    // 先让 LSM 处理（SELinux/AppArmor 等）
    error = security_task_prctl(option, arg2, arg3, arg4, arg5);
    if (error != -ENOSYS)
        return error;

    error = 0;
    switch (option) {
        // ... 大量 case 分支
    }
    return error;
}
```

---

## 3. 调用链分析

### 完整调用链

```
prctl(option, arg2, arg3, arg4, arg5)
└─ syscall(__NR_prctl, option, arg2, arg3, arg4, arg5)
   └─ SYSCALL_DEFINE5(prctl)                      // kernel/sys.c:2534
      ├─ security_task_prctl(option, ...)          // 优先 LSM 处理
      │  └─ [SELinux] → selinux_task_prctl()
      │  └─ [AppArmor] → apparmor_task_prctl()
      │  └─ [未处理] → 返回 -ENOSYS
      ├─ [LSM 返回 -ENOSYS] → switch(option)
      │
      │  // ===== 进程名操作 =====
      │  ├─ case PR_SET_NAME:                       // 设置进程名
      │  │  ├─ strncpy_from_user(comm, arg2, ...)
      │  │  └─ set_task_comm(me, comm)
      │  ├─ case PR_GET_NAME:                       // 获取进程名
      │  │  ├─ get_task_comm(comm, me)
      │  │  └─ copy_to_user(arg2, comm, ...)
      │
      │  // ===== 信号操作 =====
      │  ├─ case PR_SET_PDEATHSIG:                  // 设置父进程死亡信号
      │  │  ├─ valid_signal(arg2)检查
      │  │  └─ me->pdeath_signal = arg2
      │  ├─ case PR_GET_PDEATHSIG:                  // 获取父进程死亡信号
      │  │  └─ put_user(me->pdeath_signal, arg2)
      │
      │  // ===== 安全/转储操作 =====
      │  ├─ case PR_SET_DUMPABLE:                   // 设置可转储标志
      │  │  └─ set_dumpable(me->mm, arg2)
      │  ├─ case PR_GET_DUMPABLE:                   // 获取可转储标志
      │  │  └─ error = get_dumpable(me->mm)
      │  ├─ case PR_SET_NO_NEW_PRIVS:               // 禁止获取新特权
      │  │  └─ task_set_no_new_privs(current)
      │  ├─ case PR_GET_NO_NEW_PRIVS:               // 查询新特权状态
      │  │  └─ return task_no_new_privs(current) ? 1 : 0
      │  ├─ case PR_SET_SECCOMP:                    // 设置 seccomp
      │  │  └─ prctl_set_seccomp(arg2, arg3)
      │  ├─ case PR_GET_SECCOMP:                    // 获取 seccomp 状态
      │  │  └─ prctl_get_seccomp()
      │
      │  // ===== 内存管理 =====
      │  ├─ case PR_SET_MM:                         // 设置内存映射属性
      │  │  └─ prctl_set_mm(arg2, arg3, arg4, arg5)
      │  ├─ case PR_SET_THP_DISABLE:                // 禁用透明大页
      │  │  └─ prctl_set_thp_disable(arg2, ...)
      │  ├─ case PR_GET_THP_DISABLE:                // 查询透明大页状态
      │  │  └─ prctl_get_thp_disable(arg2, ...)
      │
      │  // ===== 定时器/调度 =====
      │  ├─ case PR_SET_TIMERSLACK:                 // 设置定时器松弛度
      │  │  ├─ [RT/DL 任务] → break
      │  │  └─ current->timer_slack_ns = arg2
      │  ├─ case PR_GET_TIMERSLACK:                 // 获取定时器松弛度
      │  │  └─ error = current->timer_slack_ns
      │
      │  // ===== 子进程管理 =====
      │  ├─ case PR_SET_CHILD_SUBREAPER:            // 设置子进程 reaper
      │  │  ├─ me->signal->is_child_subreaper = !!arg2
      │  │  └─ [arg2] → walk_process_tree(me, propagate_has_child_subreaper)
      │  ├─ case PR_GET_CHILD_SUBREAPER:            // 查询子进程 reaper 状态
      │  │  └─ put_user(me->signal->is_child_subreaper, arg2)
      │
      │  // ===== 架构特定 =====
      │  ├─ case PR_SET_FP_MODE:                    // 设置浮点模式
      │  │  └─ SET_FP_MODE(me, arg2)
      │  ├─ case PR_GET_FP_MODE:                    // 获取浮点模式
      │  │  └─ GET_FP_MODE(me)
      │  ├─ case PR_SVE_SET_VL:                     // 设置 SVE 向量长度
      │  │  └─ SVE_SET_VL(arg2)
      │  ├─ case PR_SVE_GET_VL:                     // 获取 SVE 向量长度
      │  │  └─ SVE_GET_VL()
      │  ├─ case PR_SME_SET_VL:                     // 设置 SME 向量长度
      │  │  └─ SME_SET_VL(arg2)
      │  ├─ case PR_SME_GET_VL:                     // 获取 SME 向量长度
      │  │  └─ SME_GET_VL()
      │
      │  // ===== 性能事件 =====
      │  ├─ case PR_TASK_PERF_EVENTS_DISABLE:       // 禁用 perf 事件
      │  │  └─ perf_event_task_disable()
      │  ├─ case PR_TASK_PERF_EVENTS_ENABLE:        // 启用 perf 事件
      │  │  └─ perf_event_task_enable()
      │
      │  // ===== 推测执行控制 =====
      │  ├─ case PR_GET_SPECULATION_CTRL:           // 获取推测执行控制
      │  │  └─ arch_prctl_spec_ctrl_get(arg2, ...)
      │  ├─ case PR_SET_SPECULATION_CTRL:           // 设置推测执行控制
      │  │  └─ arch_prctl_spec_ctrl_set(arg2, arg3, ...)
      │
      │  // ===== 其他 =====
      │  ├─ case PR_GET_TID_ADDRESS:                // 获取 clear_child_tid 地址
      │  │  └─ prctl_get_tid_address(me, arg2)
      │  ├─ case PR_MCE_KILL:                       // MCE 错误处理策略
      │  │  └─ 设置/清除 PF_MCE_PROCESS / PF_MCE_EARLY
      │  └─ default: error = -EINVAL
      │
      └─ return error
```

---

## 4. 关键数据结构

```c
// ========== prctl 选项 (include/uapi/linux/prctl.h) ==========

// 进程名操作
#define PR_SET_NAME      15  // 设置进程名（comm，最多 16 字节）
#define PR_GET_NAME      16  // 获取进程名

// 信号操作
#define PR_SET_PDEATHSIG  1  // 设置父进程死亡时发送的信号
#define PR_GET_PDEATHSIG  2  // 获取父进程死亡信号

// 安全/转储
#define PR_SET_DUMPABLE   3  // 设置 core dump 权限
#define PR_GET_DUMPABLE   4  // 获取 core dump 权限
#define PR_SET_NO_NEW_PRIVS  38  // 禁止获取新特权
#define PR_GET_NO_NEW_PRIVS  39  // 查询新特权状态
#define PR_SET_SECCOMP   22  // 设置 seccomp 过滤器
#define PR_GET_SECCOMP   21  // 获取 seccomp 状态

// 内存管理
#define PR_SET_MM        36  // 设置内存映射参数
#define PR_SET_THP_DISABLE  41  // 禁用透明大页
#define PR_GET_THP_DISABLE  42  // 查询透明大页状态

// 定时器
#define PR_SET_TIMERSLACK  29  // 设置定时器松弛度
#define PR_GET_TIMERSLACK  30  // 获取定时器松弛度

// 子进程管理
#define PR_SET_CHILD_SUBREAPER  36  // 设置为子进程 reaper
#define PR_GET_CHILD_SUBREAPER  37  // 查询子进程 reaper 状态

// 性能事件
#define PR_TASK_PERF_EVENTS_DISABLE  31  // 禁用 perf 事件
#define PR_TASK_PERF_EVENTS_ENABLE   32  // 启用 perf 事件

// 推测执行控制
#define PR_GET_SPECULATION_CTRL  52  // 获取推测执行控制
#define PR_SET_SPECULATION_CTRL  53  // 设置推测执行控制

// 架构特定
#define PR_SET_FP_MODE  45  // 设置浮点模式
#define PR_GET_FP_MODE  46  // 获取浮点模式
#define PR_SVE_SET_VL   50  // 设置 SVE 向量长度
#define PR_SVE_GET_VL   51  // 获取 SVE 向量长度
#define PR_SME_SET_VL   55  // 设置 SME 向量长度
#define PR_SME_GET_VL   56  // 获取 SME 向量长度

// ========== task_struct 中的相关字段 (include/linux/sched.h) ==========

struct task_struct {
    char comm[TASK_COMM_LEN];           // 进程名（PR_SET/GET_NAME 操作的目标）
    unsigned int flags;                 // 进程标志（PF_MCE_PROCESS 等）
    // ...
    int pdeath_signal;                  // 父进程死亡信号（PR_SET/GET_PDEATHSIG）
    unsigned long timer_slack_ns;       // 定时器松弛度（PR_SET/GET_TIMERSLACK）
    unsigned long default_timer_slack_ns;  // 默认定时器松弛度
    // ...
    struct mm_struct *mm;               // 地址空间（dumpable 标志在此）
    // ...
};

// ========== mm_struct 中的 dumpable 字段 ==========

struct mm_struct {
    // ...
    atomic_t mm_users;                  // 用户空间引用计数
    atomic_t mm_count;                  // 内核引用计数
    // ...
    unsigned long flags;                // MMF_* 标志
    // ...
};

// dumpable 值
#define SUID_DUMP_DISABLE  0  // 不可转储
#define SUID_DUMP_USER     1  // 可转储（默认）
```

---

## 5. 流程图

```
                     prctl(option, arg2, arg3, arg4, arg5)
                                      |
                            +---------v----------+
                            | SYSCALL_DEFINE5     |
                            | (kernel/sys.c)      |
                            +---------+----------+
                                      |
                            +---------v----------+
                            | security_task_      |
                            | prctl()             |
                            | (LSM 优先处理)       |
                            +---------+----------+
                                      |
                      +---------------+---------------+
                      |                               |
                 +----v----+                   +------v------+
                 | LSM 处理 |                   | 返回 -ENOSYS|
                 | 成功     |                   | (LSM 未处理)|
                 +----+----+                   +------+------+
                      |                               |
                      |                        +------v------+
                      |                        | switch(     |
                      |                        |  option)    |
                      |                        +------+------+
                      |                               |
                      |            +------------------+------------------+
                      |            |                  |                   |
                      |    +-------v-------+  +------v-------+  +-------v-------+
                      |    | PR_SET_NAME   |  | PR_SET_MM    |  | PR_SET_SECCOMP|
                      |    | PR_GET_NAME   |  | PR_GET_*     |  | ...           |
                      |    | PR_SET_PDEATH |  | PR_SET_CHILD |  |               |
                      |    | PR_GET_*      |  | _SUBREAPER   |  |               |
                      |    | ...           |  | ...          |  |               |
                      |    +-------+-------+  +------+-------+  +-------v-------+
                      |            |                  |                   |
                      +------------+------------------+------------------+
                                   |
                            +------v------+
                            | 返回 error  |
                            +-------------+
```

---

## 6. 错误处理

| 错误码 | 条件 | 触发位置 |
|--------|------|----------|
| `-EINVAL` | 无效的 `option` 值 | switch default |
| `-EINVAL` | 无效的信号值（PR_SET_PDEATHSIG） | `PR_SET_PDEATHSIG` |
| `-EINVAL` | 无效的 dumpable 值 | `PR_SET_DUMPABLE` |
| `-EINVAL` | PR_SET_NO_NEW_PRIVS 的 arg2 != 1 | `PR_SET_NO_NEW_PRIVS` |
| `-EFAULT` | 用户空间地址无效（PR_SET_NAME/GET_NAME） | `copy_to/from_user` |
| `-ENOSYS` | 架构不支持的操作 | 架构相关函数 |

---

## 7. 使用示例

```c
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

int main() {
    char name[16];

    // 设置进程名
    prctl(PR_SET_NAME, "my-program");

    // 获取进程名
    prctl(PR_GET_NAME, name);
    printf("进程名: %s\n", name);

    // 设置父进程死亡时收到 SIGHUP
    prctl(PR_SET_PDEATHSIG, SIGHUP);
    printf("父进程死亡信号: %d\n", prctl(PR_GET_PDEATHSIG));

    // 禁止获取新特权
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    printf("NO_NEW_PRIVS: %d\n", prctl(PR_GET_NO_NEW_PRIVS));

    // 检查可转储性
    printf("DUMPABLE: %d\n", prctl(PR_GET_DUMPABLE));

    return 0;
}
```

---

## 8. 常见操作总结

| 操作 | 功能 | 参数 | 内核版本 |
|------|------|------|----------|
| `PR_SET_NAME` | 设置进程名 | arg2=name(用户空间字符串) | 2.6.9+ |
| `PR_GET_NAME` | 获取进程名 | arg2=name(输出缓冲区) | 2.6.9+ |
| `PR_SET_PDEATHSIG` | 设置父进程死亡信号 | arg2=信号值 | 2.1.57+ |
| `PR_GET_PDEATHSIG` | 获取父进程死亡信号 | arg2=输出指针 | 2.1.57+ |
| `PR_SET_DUMPABLE` | 设置 core dump 权限 | arg2=0/1 | 2.3.20+ |
| `PR_GET_DUMPABLE` | 获取 core dump 权限 | 无 | 2.3.20+ |
| `PR_SET_NO_NEW_PRIVS` | 禁止获取新特权 | arg2=1 | 3.5+ |
| `PR_SET_SECCOMP` | 设置 seccomp 过滤器 | arg2=模式, arg3=过滤器 | 2.6.23+ |
| `PR_SET_MM` | 设置内存映射参数 | arg2=子选项, arg3=值 | 3.3+ |
| `PR_SET_CHILD_SUBREAPER` | 设置子进程 reaper | arg2=0/1 | 3.4+ |
| `PR_SET_TIMERSLACK` | 设置定时器松弛度 | arg2=松弛度(ns) | 2.6.28+ |
| `PR_SVE_SET_VL` | 设置 SVE 向量长度 | arg2=向量长度 | 4.15+ |
| `PR_SET_SPECULATION_CTRL` | 设置推测执行控制 | arg2=类型, arg3=策略 | 4.17+ |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/sys.c:2534` - SYSCALL_DEFINE5(prctl)
- `include/uapi/linux/prctl.h` - 所有 PR_* 选项定义
- `security/security.c` - security_task_prctl 实现