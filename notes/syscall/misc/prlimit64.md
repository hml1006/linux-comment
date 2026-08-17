# prlimit64 系统调用分析

## 1. 概述

`prlimit64` 是 Linux 内核用于读取和修改进程资源限制（resource limits）的扩展系统调用。它统一并扩展了传统的 `getrlimit`/`setrlimit` 的功能，支持对任意进程（而不仅仅是当前进程）的资源限制进行操作，并使用 64 位宽的限制值（`rlimit64` 结构体），避免了 32 位平台上 `rlimit` 结构体的精度限制。

**原型：**

```c
SYSCALL_DEFINE4(prlimit64, pid_t, pid, unsigned int, resource,
                const struct rlimit64 __user *, new_rlim,
                struct rlimit64 __user *, old_rlim);
```

- `pid`: 目标进程的 PID。若为 0，则操作当前进程。
- `resource`: 要操作的资源类型，如 `RLIMIT_CPU`、`RLIMIT_NOFILE` 等。
- `new_rlim`: 用户空间传入的 `rlimit64` 结构体指针，用于设置新限制。若为 `NULL` 则表示只读。
- `old_rlim`: 用户空间传出结构体指针，用于获取修改前的旧限制值。若为 `NULL` 则表示不获取。

## 2. 使用场景

- **容器运行时**：设置容器内进程的 `RLIMIT_NOFILE`、`RLIMIT_NPROC` 等资源上限
- **系统监控工具**：查询任意进程的当前资源限制（如 `prlimit` 命令行工具）
- **防御性编程**：在 `fork()` 子进程前调整 `RLIMIT_STACK` 或 `RLIMIT_AS`，防止资源耗尽攻击
- **实时系统**：设置 `RLIMIT_RTTIME` 限制实时线程的 CPU 使用时间
- **兼容性**：`getrlimit` 和 `setrlimit` 的底层实现最终都调用 `do_prlimit()`，而 `prlimit64` 是统一的入口

## 3. 函数调用栈

```
prlimit64 (系统调用入口)
├── copy_from_user(&new64, new_rlim, sizeof(new64))  // 从用户态拷贝新限制
├── rlim64_to_rlim(&new64, &new)                      // 将 rlimit64 转换为 rlimit
├── rcu_read_lock()
├── find_task_by_vpid(pid)                            // 根据 PID 查找 task_struct
├── check_prlimit_permission(tsk, checkflags)         // 检查操作权限
├── get_task_struct(tsk)                              // 增加引用计数防止释放
├── rcu_read_unlock()
├── [若目标进程非当前线程组] read_lock(&tasklist_lock) // 防止 group exit 竞争
├── do_prlimit(tsk, resource, &new, &old)             // 核心实现
│   ├── 检查 resource < RLIM_NLIMITS
│   ├── array_index_nospec(resource, RLIM_NLIMITS)    // 防止 Spectre 旁路攻击
│   ├── 检查 new_rlim->rlim_cur <= new_rlim->rlim_max
│   ├── 检查 RLIMIT_NOFILE 时 rlim_max <= sysctl_nr_open
│   ├── task_lock(tsk->group_leader)                  // 持有锁保护 signal->rlim
│   ├── 若设置新限制: 检查 capable(CAP_SYS_RESOURCE)
│   ├── 若设置新限制: security_task_setrlimit()       // LSM 钩子
│   ├── 复制 old_rlim / 写入 new_rlim
│   ├── task_unlock(tsk->group_leader)
│   └── [若 RLIMIT_CPU] 设置 POSIX CPU 定时器
├── [若需要] read_unlock(&tasklist_lock)
├── rlim_to_rlim64(&old, &old64)                      // 将 rlimit 转回 rlimit64
├── copy_to_user(old_rlim, &old64, sizeof(old64))     // 拷贝旧值回用户态
└── put_task_struct(tsk)
```

## 4. 关键数据结构

### struct rlimit64（UAPI，64 位扩展）

```c
struct rlimit64 {
    __u64 rlim_cur;   // 软限制 (soft limit)，当前生效的资源上限
    __u64 rlim_max;   // 硬限制 (hard limit)，软限制可调整的最大值
};
```

### struct rlimit（内核内部使用的传统版本）

```c
struct rlimit {
    __kernel_ulong_t rlim_cur;  // 软限制
    __kernel_ulong_t rlim_max;  // 硬限制
};
```

### 资源限制 ID 定义（`<asm-generic/resource.h>`）

```c
#define RLIMIT_CPU          0   /* CPU 时间，单位秒 */
#define RLIMIT_FSIZE        1   /* 最大文件大小 */
#define RLIMIT_DATA         2   /* 数据段最大值 */
#define RLIMIT_STACK        3   /* 栈最大值 */
#define RLIMIT_CORE         4   /* core 文件最大值 */
#define RLIMIT_RSS          5   /* 最大常驻内存集大小 */
#define RLIMIT_NPROC        6   /* 最大进程数 */
#define RLIMIT_NOFILE       7   /* 最大打开文件数 */
#define RLIMIT_MEMLOCK      8   /* 最大锁定内存大小 */
#define RLIMIT_AS           9   /* 地址空间限制 */
#define RLIMIT_LOCKS        10  /* 最大文件锁持有数 */
#define RLIMIT_SIGPENDING   11  /* 最大待处理信号数 */
#define RLIMIT_MSGQUEUE     12  /* POSIX 消息队列最大字节数 */
#define RLIMIT_NICE         13  /* 最大 nice 值 */
#define RLIMIT_RTPRIO       14  /* 最大实时优先级 */
#define RLIMIT_RTTIME       15  /* 实时任务超时 (μs) */
#define RLIM_NLIMITS        16  /* 限制种类总数 */
```

### 辅助类型定义

```c
#define RLIM_INFINITY   (~0UL)     /* 传统 unlimited 表示 */
#define RLIM64_INFINITY (~0ULL)    /* 64 位 unlimited 表示 */
```

### 转换函数

```c
/* 将内核 rlimit 转换为用户态 rlimit64 */
static void rlim_to_rlim64(const struct rlimit *rlim, struct rlimit64 *rlim64)
{
    if (rlim->rlim_cur == RLIM_INFINITY)
        rlim64->rlim_cur = RLIM64_INFINITY;
    else
        rlim64->rlim_cur = rlim->rlim_cur;
    if (rlim->rlim_max == RLIM_INFINITY)
        rlim64->rlim_max = RLIM64_INFINITY;
    else
        rlim64->rlim_max = rlim->rlim_max;
}

/* 将用户态 rlimit64 转换为内核 rlimit */
static void rlim64_to_rlim(const struct rlimit64 *rlim64, struct rlimit *rlim)
{
    if (rlim64_is_infinity(rlim64->rlim_cur))
        rlim->rlim_cur = RLIM_INFINITY;
    else
        rlim->rlim_cur = (unsigned long)rlim64->rlim_cur;
    if (rlim64_is_infinity(rlim64->rlim_max))
        rlim->rlim_max = RLIM_INFINITY;
    else
        rlim->rlim_max = (unsigned long)rlim64->rlim_max;
}
```

### do_prlimit 核心函数

```c
static int do_prlimit(struct task_struct *tsk, unsigned int resource,
                      struct rlimit *new_rlim, struct rlimit *old_rlim)
{
    struct rlimit *rlim;
    int retval = 0;

    if (resource >= RLIM_NLIMITS)
        return -EINVAL;
    resource = array_index_nospec(resource, RLIM_NLIMITS);

    if (new_rlim) {
        if (new_rlim->rlim_cur > new_rlim->rlim_max)
            return -EINVAL;
        if (resource == RLIMIT_NOFILE &&
                new_rlim->rlim_max > sysctl_nr_open)
            return -EPERM;
    }

    rlim = tsk->signal->rlim + resource;
    task_lock(tsk->group_leader);
    if (new_rlim) {
        if (new_rlim->rlim_max > rlim->rlim_max &&
                !capable(CAP_SYS_RESOURCE))
            retval = -EPERM;
        if (!retval)
            retval = security_task_setrlimit(tsk, resource, new_rlim);
    }
    if (!retval) {
        if (old_rlim)
            *old_rlim = *rlim;
        if (new_rlim)
            *rlim = *new_rlim;
    }
    task_unlock(tsk->group_leader);

    /* RLIMIT_CPU 特殊处理：设置 POSIX CPU 定时器 */
    if (!retval && new_rlim && resource == RLIMIT_CPU &&
        new_rlim->rlim_cur != RLIM_INFINITY &&
        IS_ENABLED(CONFIG_POSIX_TIMERS)) {
        /* ... 更新 CPU 定时器 ... */
    }
    return retval;
}
```

### 权限检查函数

```c
/* rcu lock must be held */
static int check_prlimit_permission(struct task_struct *task,
                                    unsigned int flags)
{
    const struct cred *cred = current_cred(), *tcred;
    bool id_match;

    if (current == task)
        return 0;

    tcred = __task_cred(task);
    id_match = (uid_eq(cred->uid, tcred->euid) &&
                uid_eq(cred->uid, tcred->suid) &&
                uid_eq(cred->uid, tcred->uid)  &&
                gid_eq(cred->gid, tcred->egid) &&
                gid_eq(cred->gid, tcred->sgid) &&
                gid_eq(cred->gid, tcred->gid));
    if (!id_match && !ns_capable(tcred->user_ns, CAP_SYS_RESOURCE))
        return -EPERM;

    return security_task_prlimit(cred, tcred, flags);
}
```

## 5. 流程图

```
用户态调用 prlimit64(pid, resource, new_rlim, old_rlim)
                              │
                              ▼
                    ┌─────────────────────┐
                    │  拷贝 new_rlim 到内核 │
                    │  rlim64_to_rlim 转换  │
                    └─────────┬───────────┘
                              │
                              ▼
                    ┌─────────────────────┐
                    │  根据 pid 查找 task   │
                    │  find_task_by_vpid() │
                    └─────────┬───────────┘
                              │
                              ▼
                    ┌─────────────────────┐
                    │  check_prlimit_permission │
                    ├─────────────────────┤
                    │ current == task? → 跳过 │
                    │ UID/GID 匹配? → 允许 │
                    │ CAP_SYS_RESOURCE? → 允许│
                    │ 否则 → -EPERM        │
                    └─────────┬───────────┘
                              │
                              ▼
                    ┌─────────────────────┐
                    │  同线程组?            │
                    │  否 → tasklist_lock   │
                    │  pid_alive() 检查     │
                    └─────────┬───────────┘
                              │
                              ▼
                    ┌─────────────────────┐
                    │    do_prlimit()       │
                    │                      │
                    │  resource 范围检查 ───→ -EINVAL    │
                    │  cur > max? ─────────→ -EINVAL    │
                    │  NOFILE > nr_open? ──→ -EPERM     │
                    │  max > 旧max?         │
                    │   && !CAP_SYS_RESOURCE→ -EPERM    │
                    │  security_task_setrlimit() → LSM  │
                    │  写入新值 / 读取旧值    │
                    │  RLIMIT_CPU → 定时器   │
                    └─────────┬───────────┘
                              │
                              ▼
                    ┌─────────────────────┐
                    │  转换 rlim → rlim64   │
                    │  copy_to_user 回用户态 │
                    └─────────┬───────────┘
                              │
                              ▼
                    返回 0 (成功) 或错误码
```

## 6. 错误处理

| 错误码 | 触发条件 |
|--------|---------|
| `-EFAULT` | `copy_from_user` 或 `copy_to_user` 访问用户空间地址失败 |
| `-ESRCH` | 指定的 `pid` 对应的进程不存在 |
| `-EPERM` | 权限不足（非 root 且 UID/GID 不匹配，且无 `CAP_SYS_RESOURCE`） |
| `-EINVAL` | `resource >= RLIM_NLIMITS`，或 `rlim_cur > rlim_max` |
| `-EPERM` | `RLIMIT_NOFILE` 的 `rlim_max` 超过 `sysctl_nr_open` |
| `-EPERM` | 硬限制提升（`rlim_max` 变大）但无 `CAP_SYS_RESOURCE` 权限 |

## 7. 使用示例

### 示例 1: 查询当前进程的打开文件数限制

```c
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct rlimit64 rlim;

    if (prlimit64(0, RLIMIT_NOFILE, NULL, &rlim) == 0) {
        printf("RLIMIT_NOFILE: soft=%llu, hard=%llu\n",
               (unsigned long long)rlim.rlim_cur,
               (unsigned long long)rlim.rlim_max);
    } else {
        perror("prlimit64");
        exit(1);
    }
    return 0;
}
```

### 示例 2: 设置其他进程的栈大小限制

```c
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    pid_t pid = atoi(argv[1]);
    struct rlimit64 new_rlim, old_rlim;

    /* 将目标进程的栈限制设为 4MB */
    new_rlim.rlim_cur = 4 * 1024 * 1024;
    new_rlim.rlim_max = 8 * 1024 * 1024;

    if (prlimit64(pid, RLIMIT_STACK, &new_rlim, &old_rlim) == 0) {
        printf("Old limits: soft=%llu, hard=%llu\n",
               (unsigned long long)old_rlim.rlim_cur,
               (unsigned long long)old_rlim.rlim_max);
    } else {
        perror("prlimit64");
        exit(1);
    }
    return 0;
}
```

### 示例 3: 使用 prlimit 命令行工具

```bash
# 查看进程 1234 的所有资源限制
prlimit --pid 1234

# 将进程 1234 的 NOFILE 软限制设为 4096
prlimit --pid 1234 --nofile=4096

# 同时设置软硬限制
prlimit --pid 1234 --nofile=4096:8192
```

## 8. 与 getrlimit/setrlimit 的对比

| 特性 | getrlimit/setrlimit | prlimit64 |
|------|-------------------|-----------|
| 操作其他进程 | 不支持 | 支持（通过 `pid` 参数） |
| 限制值宽度 | 32 位（`unsigned long`） | 64 位（`__u64`） |
| 原子读写 | 不支持（需两次调用） | 支持（一次调用同时读写） |
| 内核实现 | 均调用 `do_prlimit()` | 直接调用 `do_prlimit()` |
| 兼容层 | 有 `COMPAT` 版本 | 原生支持 64 位 |

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 内核源码: `kernel/sys.c` (prlimit64 实现)
- 内核源码: `include/uapi/linux/resource.h` (数据结构定义)
- 内核源码: `include/uapi/asm-generic/resource.h` (RLIMIT_* 常量定义)
- `man prlimit(2)` / `man getrlimit(2)`