# getrusage 系统调用分析

## 1. 概述

`getrusage` 系统调用用于获取进程或其子进程的资源使用情况统计信息。它返回一个 `struct rusage` 结构体，包含用户态和内核态 CPU 时间、内存使用、I/O 操作、上下文切换等多种资源统计指标。该调用是 POSIX 标准的一部分，源自 BSD 4.3 Reno。

**原型：**

```c
SYSCALL_DEFINE2(getrusage, int, who, struct rusage __user *, ru);
```

- `who`: 指定要获取哪个进程/线程的资源使用数据：
  - `RUSAGE_SELF` (0): 获取当前进程的资源使用（包括其所有线程的累计数据）
  - `RUSAGE_CHILDREN` (-1): 获取当前进程所有已终止且被 wait 的子进程的累计资源使用
  - `RUSAGE_THREAD` (1): 仅获取当前调用线程的资源使用（Linux 特有）
- `ru`: 指向用户空间 `struct rusage` 结构体的指针，用于接收结果。

**返回值：** 成功时返回 0；失败时返回负的错误码。

## 2. 使用场景

- **性能分析工具**：`time` 命令和 `times` 命令底层使用 `getrusage` 获取进程 CPU 时间
- **资源监控**：监控进程的缺页次数、上下文切换等指标
- **容量规划**：收集进程的 `ru_maxrss` 数据，了解内存使用峰值
- **应用优化**：评估 I/O 密集型 vs CPU 密集型应用的资源消耗特征
- **计费系统**：统计用户进程的 CPU 时间用于资源计费
- **调试分析**：`getrusage(RUSAGE_THREAD, ...)` 用于分析多线程程序中每个线程的资源消耗

## 3. 函数调用栈

```
getrusage (系统调用入口)
└── getrusage(current, who, &r)
    ├── 检查 who 参数合法性 (RUSAGE_SELF/CHILDREN/THREAD)
    ├── [RUSAGE_THREAD 分支]:
    │   ├── task_cputime_adjusted(current, &utime, &stime)  // 获取当前线程 CPU 时间
    │   ├── accumulate_thread_rusage(p, &r)                 // 获取线程的缺页/上下文切换数据
    │   └── 获取 maxrss 并跳转到 out_thread
    │
    ├── [RUSAGE_CHILDREN 分支]:
    │   ├── 读取 signal->c* 字段 (cutime, cstime, cmin_flt, cmaj_flt, 等)
    │   │   └── 这些是已终止子进程的累计数据，由 exit.c 在回收时更新
    │   └── 若为 RUSAGE_CHILDREN → 跳转到 out_children
    │
    ├── [RUSAGE_SELF 分支]:
    │   ├── 读取 signal->* 字段 (nvcsw, nivcsw, min_flt, maj_flt, inblock, oublock)
    │   ├── 遍历所有线程 (__for_each_thread) 累加线程级数据
    │   └── thread_group_cputime_adjusted(p, &tgutime, &tgstime)  // 获取线程组 CPU 时间
    │
    ├── out_thread:
    │   ├── get_task_mm(p) 获取 mm_struct
    │   ├── setmax_mm_hiwater_rss(&maxrss, mm)  // 获取或更新 RSS 峰值
    │   └── mmput(mm)
    │
    ├── out_children:
    │   ├── ru_maxrss = maxrss * (PAGE_SIZE / 1024)  // 页数 → KB
    │   ├── ru_utime = ns_to_kernel_old_timeval(utime)
    │   └── ru_stime = ns_to_kernel_old_timeval(stime)
    │
    └── copy_to_user(ru, &r, sizeof(r))  // 将结果拷贝回用户空间
```

## 4. 关键数据结构

### struct rusage（UAPI 定义）

```c
struct rusage {
    struct __kernel_old_timeval ru_utime;   /* 用户态 CPU 时间 */
    struct __kernel_old_timeval ru_stime;   /* 内核态 CPU 时间 */
    __kernel_long_t ru_maxrss;              /* 最大常驻内存集大小 (KB) */
    __kernel_long_t ru_ixrss;               /* 积分的共享内存大小 (未使用) */
    __kernel_long_t ru_idrss;               /* 积分的非共享数据大小 (未使用) */
    __kernel_long_t ru_isrss;               /* 积分的非共享栈大小 (未使用) */
    __kernel_long_t ru_minflt;              /* 缺页中断 (无需I/O) */
    __kernel_long_t ru_majflt;              /* 缺页中断 (需I/O) */
    __kernel_long_t ru_nswap;               /* 交换次数 (未使用) */
    __kernel_long_t ru_inblock;             /* 块输入操作次数 */
    __kernel_long_t ru_oublock;             /* 块输出操作次数 */
    __kernel_long_t ru_msgsnd;              /* 发送的消息数 (未使用) */
    __kernel_long_t ru_msgrcv;              /* 接收的消息数 (未使用) */
    __kernel_long_t ru_nsignals;            /* 接收的信号数 (未使用) */
    __kernel_long_t ru_nvcsw;               /* 自愿上下文切换次数 */
    __kernel_long_t ru_nivcsw;              /* 非自愿上下文切换次数 */
};
```

### who 参数常量

```c
#define RUSAGE_SELF     0   /* 当前进程的所有线程累计 */
#define RUSAGE_CHILDREN (-1) /* 已终止子进程的累计数据 */
#define RUSAGE_BOTH     (-2) /* 内部使用 (sys_wait4) */
#define RUSAGE_THREAD   1   /* 仅当前调用线程 (Linux 特有) */
```

### 信号量结构体中的相关字段

```c
struct signal_struct {
    /* ... */
    
    /* 累加的资源使用数据 (RUSAGE_SELF 使用) */
    u64 utime, stime;          /* 用户/内核态 CPU 时间 (ns) */
    unsigned long nvcsw, nivcsw;    /* 自愿/非自愿上下文切换次数 */
    unsigned long min_flt, maj_flt; /* 缺页中断次数 */
    unsigned long inblock, oublock; /* 块 I/O 操作次数 */
    unsigned long maxrss;           /* 最大 RSS (页数) */
    
    /* 子进程累计数据 (RUSAGE_CHILDREN 使用) */
    u64 cutime, cstime;         /* 子进程用户/内核态 CPU 时间 */
    unsigned long cnvcsw, cnivcsw;  /* 子进程上下文切换 */
    unsigned long cmin_flt, cmaj_flt; /* 子进程缺页中断 */
    unsigned long cinblock, coublock; /* 子进程块 I/O */
    unsigned long cmaxrss;           /* 子进程最大 RSS */
    
    /* ... */
};
```

### 线程级累计函数

```c
static void accumulate_thread_rusage(struct task_struct *t, struct rusage *r)
{
    r->ru_nvcsw += t->nvcsw;
    r->ru_nivcsw += t->nivcsw;
    r->ru_minflt += t->min_flt;
    r->ru_majflt += t->maj_flt;
    r->ru_inblock += task_io_get_inblock(t);
    r->ru_oublock += task_io_get_oublock(t);
}
```

## 5. 流程图

```
用户态: getrusage(who, &ru)
                              │
                              ▼
                    ┌─────────────────────────┐
                    │  检查 who 参数合法性      │
                    │  RUSAGE_SELF/CHILDREN/   │
                    │  THREAD? → 否则 -EINVAL  │
                    └───────────┬─────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │  getrusage(current, who, r) │
                    │                           │
                    │  ┌─────────────────────┐  │
                    │  │ who == RUSAGE_THREAD? │  │
                    │  └──────┬──────┬───────┘  │
                    │   是     │      │ 否       │
                    │         ▼      ▼          │
                    │  ┌────────┐ ┌──────────┐  │
                    │  │线程级  │ │ stats_lock│  │
                    │  │CPU时间 │ │ 序列锁     │  │
                    │  │线程资源│ │ 保护读取   │  │
                    │  └───┬────┘ └─────┬─────┘  │
                    │      │            │         │
                    │      ▼            ▼         │
                    │  ┌─────────────────────┐   │
                    │  │ 累加线程组数据:      │   │
                    │  │ - signal->min_flt   │   │
                    │  │ - signal->maj_flt   │   │
                    │  │ - signal->nvcsw     │   │
                    │  │ - signal->inblock   │   │
                    │  │ - __for_each_thread │   │
                    │  │  累加各线程         │   │
                    │  │ - thread_group_     │   │
                    │  │   cputime_adjusted  │   │
                    │  └─────────┬───────────┘   │
                    │            │                │
                    │            ▼                │
                    │  ┌─────────────────────┐   │
                    │  │ 获取 maxrss (mm)     │   │
                    │  │ 转换: 页数 → KB      │   │
                    │  │ 转换: ns → timeval   │   │
                    │  └─────────┬───────────┘   │
                    └────────────┬───────────────┘
                                 │
                                 ▼
                    ┌─────────────────────────┐
                    │  copy_to_user(ru, &r)    │
                    │  → 返回 0 或 -EFAULT     │
                    └─────────────────────────┘
```

## 6. 错误处理

| 错误码 | 触发条件 |
|--------|---------|
| `-EINVAL` | `who` 参数不是 `RUSAGE_SELF`、`RUSAGE_CHILDREN` 或 `RUSAGE_THREAD` |
| `-EFAULT` | `ru` 指向的用户空间地址不可写 |

**注意：** `getrusage` 的错误处理非常简单，因为核心数据全部来自内核内部的数据结构，不涉及外部资源获取。

## 7. 使用示例

### 示例 1: 获取进程 CPU 时间

```c
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct rusage usage;

    /* 做一些计算工作 */
    volatile unsigned long sum = 0;
    for (int i = 0; i < 100000000; i++)
        sum += i;

    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        printf("User CPU time:  %ld.%06ld sec\n",
               usage.ru_utime.tv_sec, usage.ru_utime.tv_usec);
        printf("Sys CPU time:  %ld.%06ld sec\n",
               usage.ru_stime.tv_sec, usage.ru_stime.tv_usec);
        printf("Max RSS:       %ld KB\n", usage.ru_maxrss);
        printf("Page faults:   %ld (minor) / %ld (major)\n",
               usage.ru_minflt, usage.ru_majflt);
        printf("Context switches: %ld (voluntary) / %ld (involuntary)\n",
               usage.ru_nvcsw, usage.ru_nivcsw);
        printf("I/O:           %ld reads / %ld writes\n",
               usage.ru_inblock, usage.ru_oublock);
    } else {
        perror("getrusage");
        exit(1);
    }
    return 0;
}
```

### 示例 2: 监控线程资源使用（多线程）

```c
#include <sys/resource.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

void *worker(void *arg)
{
    struct rusage usage;
    volatile unsigned long sum = 0;

    /* 线程执行计算任务 */
    for (int i = 0; i < 50000000; i++)
        sum += i;

    /* 获取当前线程的资源使用 */
    getrusage(RUSAGE_THREAD, &usage);
    printf("Thread %ld: user=%ld.%06lds, sys=%ld.%06lds, "
           "page faults=%ld+%ld\n",
           (long)arg,
           usage.ru_utime.tv_sec, usage.ru_utime.tv_usec,
           usage.ru_stime.tv_sec, usage.ru_stime.tv_usec,
           usage.ru_minflt, usage.ru_majflt);
    return NULL;
}

int main(void)
{
    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker, (void *)1);
    pthread_create(&t2, NULL, worker, (void *)2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    /* 获取整个进程的累计资源使用 */
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    printf("Process total: user=%ld.%06lds, sys=%ld.%06lds\n",
           usage.ru_utime.tv_sec, usage.ru_utime.tv_usec,
           usage.ru_stime.tv_sec, usage.ru_stime.tv_usec);
    return 0;
}
```

### 示例 3: 替代 time 命令

```c
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command>\n", argv[0]);
        exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程执行命令 */
        execvp(argv[1], &argv[1]);
        perror("execvp");
        exit(1);
    }

    /* 父进程等待子进程 */
    int status;
    struct rusage usage;
    wait4(pid, &status, 0, &usage);

    printf("User time:     %ld.%06lds\n",
           usage.ru_utime.tv_sec, usage.ru_utime.tv_usec);
    printf("System time:   %ld.%06lds\n",
           usage.ru_stime.tv_sec, usage.ru_stime.tv_usec);
    printf("Max RSS:       %ld KB\n", usage.ru_maxrss);
    printf("Minor faults:  %ld\n", usage.ru_minflt);
    printf("Major faults:  %ld\n", usage.ru_majflt);
    printf("Context switches: %ld vol + %ld invol\n",
           usage.ru_nvcsw, usage.ru_nivcsw);
    printf("I/O: %ld in + %ld out blocks\n",
           usage.ru_inblock, usage.ru_oublock);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
```

## 8. 注意事项

- **RUSAGE_CHILDREN 的限制**：仅统计已终止且被父进程 `wait()` 回收的子进程。未回收的子进程数据不包含在内。
- **RUSAGE_THREAD 是 Linux 扩展**：并非 POSIX 标准，在其他 Unix 系统上可能不可用。
- **ru_ixrss/ru_idrss/ru_isrss**：这些字段在现代 Linux 内核中始终为 0，内核不再维护这些积分值。
- **ru_nswap/ru_msgsnd/ru_msgrcv/ru_nsignals**：同样不为现代 Linux 所维护，始终为 0。
- **ru_maxrss 的单位**：在 `struct rusage` 中单位为 KB，但内核内部以页为单位存储，通过 `maxrss * (PAGE_SIZE / 1024)` 转换。
- **并发安全**：内核使用 `stats_lock` 序列锁保护 `signal_struct` 中的资源统计字段，确保多线程并发读取的一致性。
- **子进程数据更新**：子进程终止时，`exit.c` 中的 `wait_task_zombie()` 会在回收子进程时将子进程的资源使用数据累加到父进程的 `signal->c*` 字段中。

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 内核源码: `kernel/sys.c` (getrusage 实现)
- 内核源码: `include/uapi/linux/resource.h` (struct rusage 定义)
- 内核源码: `include/linux/sched/signal.h` (struct signal_struct)
- `man getrusage(2)` / `man wait4(2)`