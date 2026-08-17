# perf_event_open 系统调用分析

## 1. 概述

`perf_event_open` 用于创建性能监控事件。这些事件可以是硬件事件（如 CPU 周期数、缓存未命中）、软件事件（如页面错误、上下文切换）或 tracepoint 事件。该调用是 Linux `perf` 工具的核心系统调用。

**原型：**

```c
SYSCALL_DEFINE5(perf_event_open,
    struct perf_event_attr __user *, attr_uptr,
    pid_t, pid, int, cpu, int, group_fd, unsigned long, flags)
```

**参数：**
- `attr_uptr`：指向 `perf_event_attr` 结构体的指针，描述事件类型和配置
- `pid`：目标进程 PID（-1 表示所有进程，0 表示当前进程）
- `cpu`：目标 CPU 编号（-1 表示所有 CPU）
- `group_fd`：事件组 leader 的 fd（-1 表示独立事件）
- `flags`：标志位（`PERF_FLAG_FD_CLOEXEC`, `PERF_FLAG_FD_OUTPUT`, `PERF_FLAG_FD_NO_GROUP` 等）

## 2. 使用场景

- 性能分析工具（`perf`, `perf top`, `perf record`）
- 硬件性能计数器监控
- 软件事件统计（上下文切换、迁移、页面错误）
- tracepoint 和 kprobe/uprobe 采样
- 基于采样的性能分析

## 3. 函数调用栈

```
perf_event_open(attr_uptr, pid, cpu, group_fd, flags)  // kernel/events/core.c
  ├─ flags 检查 → 只允许 PERF_FLAG_ALL 中的位
  ├─ perf_copy_attr(attr_uptr, &attr)                   // 从用户空间拷贝属性
  ├─ security_perf_event_open() → LSM 安全检查
  ├─ 根据 pid/cpu 确定目标:
  │    ├─ [pid == -1 && cpu == -1] → 系统范围
  │    ├─ [pid == -1 && cpu >= 0] → 指定 CPU
  │    ├─ [pid >= 0 && cpu == -1] → 指定进程
  │    └─ [pid >= 0 && cpu >= 0] → 指定进程的指定 CPU
  ├─ perf_event_alloc(&attr, cpu, ...)                  // 分配 perf_event 结构
  │    ├─ 根据 attr->type 确定 PMU
  │    ├─ 分配事件 ID
  │    └─ 初始化 hw_perf_event
  ├─ anon_inode_getfd("[perf_event]", &perf_fops, ...)  // 创建事件 fd
  └─ perf_install_in_context(ctx, event, cpu)            // 安装到 CPU 上下文
       └─ add_event_to_ctx(event, ctx)                   // 添加到事件上下文
```

## 4. 关键数据结构

### 4.1 struct perf_event_attr（事件属性，用户空间传入）

```c
// include/uapi/linux/perf_event.h
struct perf_event_attr {
    __u32 type;                 // 事件类型 (PERF_TYPE_HARDWARE 等)
    __u32 size;                 // 结构体大小（向前兼容）
    __u64 config;               // 事件配置（取决于类型）
    union {
        __u64 sample_period;    // 采样周期
        __u64 sample_freq;      // 采样频率
    };
    __u64 sample_type;          // 采样数据类型
    __u64 read_format;          // 读取格式
    __u64 disabled       : 1;   // 初始禁用
    __u64 inherit        : 1;   // 子进程继承
    __u64 pinned         : 1;   // 必须使用 PMU
    __u64 exclusive      : 1;   // 独占 PMU
    __u64 exclude_user   : 1;   // 排除用户空间
    __u64 exclude_kernel : 1;   // 排除内核空间
    __u64 exclude_hv     : 1;   // 排除 Hypervisor
    __u64 exclude_idle   : 1;   // 排除空闲
    __u64 mmap           : 1;   // 记录 mmap
    __u64 comm           : 1;   // 记录进程名
    __u64 freq           : 1;   // 使用频率而非周期
    __u64 inherit_stat   : 1;   // 继承统计
    __u64 enable_on_exec : 1;   // exec 时启用
    __u64 task           : 1;   // 记录 fork/exit
    __u64 watermark      : 1;   // 水印通知
    // ... 更多标志位和字段
    __u64 bp_addr;              // 断点地址
    __u32 bp_len;               // 断点长度
    __u32 bp_type;              // 断点类型
};
```

### 4.2 struct perf_event（内核事件结构，部分字段）

```c
// include/linux/perf_event.h
struct perf_event {
    struct list_head event_entry;        // 事件链表
    struct list_head sibling_list;       // 兄弟事件链表
    struct hlist_node hlist_entry;       // 哈希链表
    int nr_siblings;                     // 兄弟事件数
    struct perf_event *group_leader;     // 组领导者
    struct pmu *pmu;                     // PMU 实例
    enum perf_event_state state;         // 事件状态
    local64_t count;                     // 事件计数
    struct perf_event_attr attr;         // 事件属性
    struct hw_perf_event hw;            // 硬件 PMU 状态
    struct perf_event_context *ctx;      // 事件上下文
    struct file *filp;                   // 关联文件
};
```

## 5. 流程图

```
用户态: perf_event_open(&attr, pid, cpu, group_fd, flags)
    │
    v
┌─────────────────────────────────────┐
│ 验证 flags 和 attr 参数             │
│ perf_copy_attr() 拷贝到内核         │
│ security_perf_event_open() 安全检查 │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 确定目标 (pid, cpu):               │
│ - 系统范围监控                      │
│ - 每个 CPU 监控                     │
│ - 每个进程监控                      │
│ 查找或创建 perf_event_context      │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ perf_event_alloc()                  │
│ 1. 根据 type 查找 PMU              │
│ 2. 分配 perf_event 结构            │
│ 3. 初始化事件属性                   │
│ 4. 分配事件 ID                     │
│ 5. 如果是硬件事件，保留 PMU 资源   │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 创建匿名 fd                        │
│ 关联 perf_fops 操作集              │
│ (read, ioctl, mmap, poll, ...)     │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ perf_install_in_context()           │
│ 将事件安装到目标 CPU 上下文        │
│ 启用事件计数或采样                 │
└─────────────────────────────────────┘
    │
    v
返回事件 fd
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | attr 无效 / flags 无效 / 类型不兼容 |
| `-EPERM` | 权限不足 | 无权监控目标 / 需要 CAP_SYS_ADMIN |
| `-ENOMEM` | 内存不足 | 事件结构分配失败 |
| `-EFAULT` | 内存错误 | attr_uptr 不可访问 |
| `-EBUSY` | 资源繁忙 | PMU 资源已被独占 |
| `-EACCES` | 拒绝访问 | 安全策略禁止 |
| `-EMFILE` | 文件表满 | 进程文件描述符表耗尽 |

## 7. 使用示例

```c
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu,
                   group_fd, flags);
}

int main(void)
{
    struct perf_event_attr pe;
    long long count;
    int fd;

    memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(pe);
    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;

    fd = perf_event_open(&pe, 0, -1, -1, 0);
    if (fd == -1) {
        perror("perf_event_open");
        return 1;
    }

    /* 启用事件 */
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    /* 执行一些工作 */
    volatile int sum = 0;
    for (int i = 0; i < 1000000; i++)
        sum += i;

    /* 禁用事件 */
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

    /* 读取计数 */
    read(fd, &count, sizeof(count));
    printf("CPU cycles: %lld\n", count);

    close(fd);
    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/events/core.c`
- 头文件：`include/uapi/linux/perf_event.h`, `include/linux/perf_event.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#BPF 与追踪)