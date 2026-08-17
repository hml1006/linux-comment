# sysinfo 系统调用分析

## 1. 概述

`sysinfo` 系统调用用于获取系统总体信息，包括运行时间、平均负载、内存使用情况、交换空间、进程数等。它返回一个 `struct sysinfo` 结构体，包含系统运行的关键统计信息。

### 关键特点

- 提供系统级别的运行状态快照
- 通过 `do_sysinfo` 收集所有信息，然后 `copy_to_user` 返回到用户空间
- 内存值以字节为单位（通过 `mem_unit` 字段指定单位）
- 在 32 位兼容模式下，会处理大内存值的缩放问题

---

## 2. 函数原型

```c
#include <sys/sysinfo.h>

int sysinfo(struct sysinfo *info);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `info` | 指向 `struct sysinfo` 的用户空间缓冲区 |

### 内核入口

```c
// kernel/sys.c:2997
SYSCALL_DEFINE1(sysinfo, struct sysinfo __user *, info)
{
    struct sysinfo val;

    do_sysinfo(&val);

    if (copy_to_user(info, &val, sizeof(struct sysinfo)))
        return -EFAULT;

    return 0;
}
```

---

## 3. 调用链分析

### 完整调用链

```
sysinfo(info)
└─ syscall(__NR_sysinfo, info)
   └─ SYSCALL_DEFINE1(sysinfo)                    // kernel/sys.c:2997
      └─ do_sysinfo(&val)                          // kernel/sys.c:2934
         ├─ ktime_get_boottime_ts64(&tp)            // 获取系统启动时间
         ├─ timens_add_boottime(&tp)                 // 时间命名空间调整
         ├─ info->uptime = tp.tv_sec + (tp.tv_nsec ? 1 : 0)  // 运行秒数
         ├─ get_avenrun(info->loads, ...)            // 获取平均负载
         │  └─ avenrun[] 数组的固定值（1/5/15 分钟）
         ├─ info->procs = nr_threads                // 当前进程数
         ├─ si_meminfo(info)                        // 获取内存信息
         │  ├─ info->totalram = totalram_pages * PAGE_SIZE
         │  ├─ info->freeram = nr_free_pages() * PAGE_SIZE
         │  ├─ info->sharedram = nr_shared_pages() * PAGE_SIZE
         │  └─ info->bufferram = nr_buffer_pages() * PAGE_SIZE
         ├─ si_swapinfo(info)                       // 获取交换空间信息
         │  ├─ info->totalswap = nr_swap_pages * PAGE_SIZE
         │  └─ info->freeswap = nr_free_swap_pages * PAGE_SIZE
         ├─ [高内存] → info->totalhigh / freehigh
         ├─ info->mem_unit = 1 (或缩放后的值)
         │  └─ 如果内存值不会溢出 32 位，将 mem_unit 保持为 1
         │  └─ 否则缩放 mem_unit 并调整内存值（兼容旧版）
         └─ copy_to_user(info, &val, sizeof(struct sysinfo))

```

### do_sysinfo 详细流程

```c
// kernel/sys.c:2934
static int do_sysinfo(struct sysinfo *info)
{
    unsigned long mem_total, sav_total;
    unsigned int mem_unit, bitcount;
    struct timespec64 tp;

    memset(info, 0, sizeof(struct sysinfo));

    // 1. 运行时间
    ktime_get_boottime_ts64(&tp);
    timens_add_boottime(&tp);
    info->uptime = tp.tv_sec + (tp.tv_nsec ? 1 : 0);

    // 2. 平均负载
    get_avenrun(info->loads, 0, SI_LOAD_SHIFT - FSHIFT);

    // 3. 进程数
    info->procs = nr_threads;

    // 4. 内存信息
    si_meminfo(info);
    si_swapinfo(info);

    // 5. 兼容性：确保内存值不溢出 32 位
    mem_total = info->totalram + info->totalswap;
    if (mem_total < info->totalram || mem_total < info->totalswap)
        goto out;  // 溢出，保持现有值

    // 尝试缩放 mem_unit 以兼容 32 位程序
    bitcount = 0;
    mem_unit = info->mem_unit;
    while (mem_unit > 1) {
        bitcount++;
        mem_unit >>= 1;
        sav_total = mem_total;
        mem_total <<= 1;
        if (mem_total < sav_total)
            goto out;  // 溢出，停止缩放
    }

    // 应用缩放
    info->mem_unit = 1;
    info->totalram <<= bitcount;
    info->freeram <<= bitcount;
    info->sharedram <<= bitcount;
    info->bufferram <<= bitcount;
    info->totalswap <<= bitcount;
    info->freeswap <<= bitcount;
    info->totalhigh <<= bitcount;
    info->freehigh <<= bitcount;

out:
    return 0;
}
```

---

## 4. 关键数据结构

```c
// ========== sysinfo 结构体 (include/uapi/linux/sysinfo.h) ==========

struct sysinfo {
    __kernel_long_t uptime;          // 系统启动以来的秒数
    __kernel_ulong_t loads[3];       // 1、5、15 分钟平均负载
    __kernel_ulong_t totalram;       // 总可用物理内存
    __kernel_ulong_t freeram;        // 空闲内存
    __kernel_ulong_t sharedram;      // 共享内存
    __kernel_ulong_t bufferram;      // 缓冲内存
    __kernel_ulong_t totalswap;      // 总交换空间
    __kernel_ulong_t freeswap;       // 空闲交换空间
    __u16 procs;                     // 当前进程数
    __u16 pad;                       // 填充（m68k 对齐）
    __kernel_ulong_t totalhigh;      // 总高端内存
    __kernel_ulong_t freehigh;       // 空闲高端内存
    __u32 mem_unit;                  // 内存单位（字节）
    char _f[20-2*sizeof(__kernel_ulong_t)-sizeof(__u32)];  // 填充
};

// ========== 内核内部的内存统计函数 ==========

// mm/page_alloc.c
void si_meminfo(struct sysinfo *val)
{
    val->totalram = totalram_pages() << PAGE_SHIFT;
    val->freeram = nr_free_pages() << PAGE_SHIFT;
    val->sharedram = nr_shared_pages() << PAGE_SHIFT;
    val->bufferram = nr_buffer_pages() << PAGE_SHIFT;
    val->totalhigh = nr_free_highpages() << PAGE_SHIFT;
    val->freehigh = 0;  // 大多数架构为 0
    val->mem_unit = PAGE_SIZE;
}

// mm/swap_state.c
void si_swapinfo(struct sysinfo *val)
{
    val->totalswap = nr_swap_pages << PAGE_SHIFT;
    val->freeswap = nr_free_swap_pages << PAGE_SHIFT;
}
```

---

## 5. 流程图

```
                     sysinfo(struct sysinfo *info)
                                      |
                            +---------v----------+
                            | SYSCALL_DEFINE1     |
                            | (kernel/sys.c)      |
                            +---------+----------+
                                      |
                            +---------v----------+
                            | do_sysinfo(&val)    |
                            | (填充 struct sys-  |
                            |  info 结构体)       |
                            +---------+----------+
                                      |
               +----------------------+----------------------+
               |                      |                      |
        +------v------+       +------v------+       +------v------+
        | ktime_get_  |       | get_avenrun |       | si_meminfo  |
        | boottime_   |       | (平均负载)  |       | (内存信息)  |
        | ts64        |       +------+------+       +------+------+
        | (运行时间)  |              |                      |
        +------+------+       +------v------+       +------v------+
               |              | loads[0] =  |       | totalram    |
        +------v------+       |   1min avg  |       | freeram     |
        | info->uptime|       | loads[1] =  |       | sharedram   |
        | = tv_sec    |       |   5min avg  |       | bufferram   |
        +------+------+       | loads[2] =  |       | totalhigh   |
               |              |  15min avg  |       +------+------+
        +------v------+       +------+------+              |
        | info->procs |              |               +------v------+
        | = nr_threads|              |               | si_swapinfo |
        +------+------+              |               | (交换信息)  |
               |                     |               +------+------+
               +---------------------+----------------------+
                                     |
                            +--------v--------+
                            | 内存值兼容性缩放  |
                            | (32 位兼容)      |
                            +--------+--------+
                                     |
                            +--------v--------+
                            | copy_to_user(   |
                            | info, &val, ...)|
                            +--------+--------+
                                     |
                            +--------v--------+
                            | 返回 0（成功）   |
                            +-----------------+
```

---

## 6. 错误处理

| 错误码 | 条件 | 触发位置 |
|--------|------|----------|
| `-EFAULT` | `info` 指针指向无效用户空间地址 | `copy_to_user` |

---

## 7. 使用示例

```c
#include <sys/sysinfo.h>
#include <stdio.h>

int main() {
    struct sysinfo info;

    if (sysinfo(&info) == -1) {
        perror("sysinfo");
        return 1;
    }

    printf("运行时间: %ld 秒\n", info.uptime);
    printf("  约 %ld 天 %ld 小时\n", info.uptime / 86400,
           (info.uptime % 86400) / 3600);

    printf("平均负载: %ld %ld %ld\n",
           info.loads[0], info.loads[1], info.loads[2]);

    printf("总内存: %ld MB\n", info.totalram / (1024 * 1024));
    printf("空闲内存: %ld MB\n", info.freeram / (1024 * 1024));
    printf("共享内存: %ld MB\n", info.sharedram / (1024 * 1024));
    printf("缓冲内存: %ld MB\n", info.bufferram / (1024 * 1024));

    printf("总交换: %ld MB\n", info.totalswap / (1024 * 1024));
    printf("空闲交换: %ld MB\n", info.freeswap / (1024 * 1024));

    printf("当前进程数: %d\n", info.procs);
    printf("内存单位: %u 字节\n", info.mem_unit);

    return 0;
}
```

---

## 8. 与 /proc/meminfo 对比

| 特性 | sysinfo | /proc/meminfo |
|------|---------|---------------|
| **访问方式** | 系统调用 | 文件读取 |
| **性能** | 快（一次系统调用） | 较慢（文件系统操作） |
| **信息量** | 有限（只包含基本信息） | 丰富（详细的内存统计） |
| **内存值** | 以 `mem_unit` 为单位 | 以 kB 为单位 |
| **兼容性** | 所有 Unix 类似系统 | Linux 特有 |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/sys.c:2934` - do_sysinfo 实现
- `kernel/sys.c:2997` - SYSCALL_DEFINE1(sysinfo)
- `include/uapi/linux/sysinfo.h` - struct sysinfo 定义