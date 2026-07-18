# times 系统调用分析

## 1. 概述

`times` 获取当前进程及其子进程的用户态 CPU 时间和内核态 CPU 时间。返回自系统启动以来的时钟滴答数。

**原型：**

```c
SYSCALL_DEFINE1(times, struct tms __user *, tbuf)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `tbuf` | `struct tms __user *` | 指向用户空间 `tms` 结构体的指针，用于接收进程时间信息（可为 NULL） |

**返回值：**
- 成功返回自系统启动以来的时钟滴答数（`clock_t` 类型，强制成功返回）
- 失败返回负的错误码

## 2. 使用场景

- `time` 命令和 shell 内置的 `time` 统计命令执行时间
- 进程性能分析工具统计 CPU 时间分配
- `/bin/time` 等工具测量程序执行时间
- 进程资源监控和 profiler

## 3. 函数调用栈

```
SYSCALL_DEFINE1(times, tbuf)                           // kernel/sys.c
  ├─ [tbuf != NULL]
  │    ├─ do_sys_times(&tmp)                            // 填充 tms 结构体
  │    │    ├─ thread_group_cputime_adjusted(current, &tgutime, &tgstime)
  │    │    │    └─ 获取当前进程组的用户态和内核态 CPU 累计时间（纳秒级）
  │    │    ├─ cutime = current->signal->cutime         // 子进程用户态时间
  │    │    ├─ cstime = current->signal->cstime         // 子进程内核态时间
  │    │    ├─ tmp.tms_utime  = nsec_to_clock_t(tgutime)  // 纳秒转时钟滴答
  │    │    ├─ tmp.tms_stime  = nsec_to_clock_t(tgstime)
  │    │    ├─ tmp.tms_cutime = nsec_to_clock_t(cutime)
  │    │    └─ tmp.tms_cstime = nsec_to_clock_t(cstime)
  │    └─ copy_to_user(tbuf, &tmp, sizeof(struct tms))
  │         拷贝失败 → 返回 -EFAULT
  ├─ force_successful_syscall_return()                  // 强制成功返回（即使 tbuf 为 NULL）
  └─ return (long)jiffies_64_to_clock_t(get_jiffies_64()) // 返回系统启动后的时钟滴答数
```

### 兼容层（32位时间）

```c
COMPAT_SYSCALL_DEFINE1(times, struct compat_tms __user *, tbuf)
{
    // 对 32 位 compat 模式进行时间转换
    do_sys_times(&tms);
    // 将 clock_t 转换为 compat_clock_t
    tmp.tms_utime = clock_t_to_compat_clock_t(tms.tms_utime);
    // ... 其余字段同理
    return compat_jiffies_to_clock_t(jiffies);
}
```

## 4. 关键数据结构

### 4.1 struct tms（进程时间结构体）

```c
// include/uapi/linux/times.h
struct tms {
    __kernel_clock_t tms_utime;   // 用户态 CPU 时间（时钟滴答数）
    __kernel_clock_t tms_stime;   // 内核态 CPU 时间（时钟滴答数）
    __kernel_clock_t tms_cutime;  // 已终止子进程的用户态 CPU 时间
    __kernel_clock_t tms_cstime;  // 已终止子进程的内核态 CPU 时间
};
```

| 字段 | 描述 |
|------|------|
| `tms_utime` | 当前进程在用户态执行所消耗的 CPU 时间 |
| `tms_stime` | 当前进程在内核态（系统调用）执行所消耗的 CPU 时间 |
| `tms_cutime` | 当前进程的已终止子进程的用户态 CPU 时间累计 |
| `tms_cstime` | 当前进程的已终止子进程的内核态 CPU 时间累计 |

### 4.2 子进程时间累计

子进程时间 (`cutime`/`cstime`) 存储在 `signal_struct` 中：

```c
// include/linux/sched/signal.h
struct signal_struct {
    // ...
    u64 cutime;    // 子进程用户态时间累计（纳秒）
    u64 cstime;    // 子进程内核态时间累计（纳秒）
    // ...
};
```

- 子进程退出时（`wait()` 或 `SIGCHLD`），其 CPU 时间通过 `wait_task_zombie()` 累加到父进程的 `signal->cutime`/`cstime`
- 当前进程的 CPU 时间通过 `thread_group_cputime_adjusted()` 实时获取

### 4.3 时间单位转换

```c
// 纳秒 (nsec) → 时钟滴答 (clock_t)
nsec_to_clock_t(nsec);  // 使用 USER_HZ（通常为 100）进行转换

// jiffies → clock_t
jiffies_64_to_clock_t(jiffies);  // 返回系统启动后的总时钟滴答数
```

## 5. 流程图

```
用户态调用 times(tbuf)
    │
    ▼
┌─────────────────────────────────────┐
│  SYSCALL_DEFINE1(times, tbuf)      │
│  kernel/sys.c                       │
└─────────────────────────────────────┘
    │
    ├── tbuf == NULL? ─→ 跳过拷贝，直接返回时钟滴答数
    │
    ▼ (tbuf != NULL)
┌─────────────────────────────────────┐
│  do_sys_times(&tmp)                │
│  ├─ thread_group_cputime_adjusted()│  ← 获取当前进程 CPU 时间（纳秒）
│  ├─ signal->cutime / cstime        │  ← 读取子进程累计时间
│  └─ nsec_to_clock_t()              │  ← 纳秒 → 时钟滴答
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  copy_to_user(tbuf, &tmp, sizeof)  │
│  失败 → 返回 -EFAULT                │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  force_successful_syscall_return() │  ← 强制成功返回
│  return jiffies_64_to_clock_t()    │  ← 返回系统启动以来时钟滴答数
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EFAULT` | 地址错误 | `tbuf` 非 NULL 且 `copy_to_user()` 拷贝失败 |

注意：`times()` 使用 `force_successful_syscall_return()`，即使 `tbuf` 为 NULL，也返回系统启动后的时钟滴答数，不会返回错误。

## 7. 使用示例

```c
#include <sys/times.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct tms t;
    clock_t start, end;

    // 获取起始时间
    start = times(&t);
    printf("Start time: %ld ticks\n", start);
    printf("  User time:   %ld ticks\n", t.tms_utime);
    printf("  System time: %ld ticks\n", t.tms_stime);

    // 做一些工作
    for (volatile long i = 0; i < 100000000; i++)
        ;

    // 获取结束时间
    end = times(&t);
    printf("End time: %ld ticks\n", end);
    printf("  User time:   %ld ticks\n", t.tms_utime);
    printf("  System time: %ld ticks\n", t.tms_stime);
    printf("  Child user:   %ld ticks\n", t.tms_cutime);
    printf("  Child system: %ld ticks\n", t.tms_cstime);
    printf("Elapsed: %ld ticks (%.2f seconds)\n",
           end - start, (double)(end - start) / sysconf(_SC_CLK_TCK));

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#系统标识与信息)
- 源码: `kernel/sys.c`（`SYSCALL_DEFINE1(times)` 和 `do_sys_times()`）
- 头文件: `include/uapi/linux/times.h`
- 时间单位转换: `include/linux/jiffies.h` 中的 `nsec_to_clock_t()` 和 `jiffies_64_to_clock_t()`
- 子进程时间累计: `kernel/exit.c` 中的 `wait_task_zombie()`
- 相关系统调用: `getrusage()`, `clock_gettime()`, `time()`