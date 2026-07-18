# getrlimit 系统调用分析

## 1. 概述

`getrlimit` 系统调用用于获取进程的资源限制（resource limit）。它允许查询当前进程的各种资源限制，如 CPU 时间、文件大小、打开文件数等。

**原型：**

```c
SYSCALL_DEFINE2(getrlimit, unsigned int, resource, struct rlimit __user *, rlim);
```

| 参数 | 类型 | 描述 |
|--|--|--|
| `resource` | `unsigned int` | 资源类型标识符 |
| `rlim` | `struct rlimit __user *` | 输出参数，接收资源限制值 |

### 返回值

- 成功时返回 0
- `resource` 无效时返回 `-EINVAL`
- 用户空间地址无效时返回 `-EFAULT`

## 2. 完整实现

```c
// kernel/sys.c
SYSCALL_DEFINE2(getrlimit, unsigned int, resource, struct rlimit __user *, rlim)
{
    struct rlimit value;
    int ret;

    ret = do_prlimit(current, resource, NULL, &value);
    if (!ret)
        ret = copy_to_user(rlim, &value, sizeof(*rlim)) ? -EFAULT : 0;

    return ret;
}
```

### do_prlimit 核心实现

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
        // 验证新值
        if (new_rlim->rlim_cur > new_rlim->rlim_max)
            return -EINVAL;
        if (resource == RLIMIT_NOFILE &&
                new_rlim->rlim_max > sysctl_nr_open)
            return -EPERM;
    }

    rlim = tsk->signal->rlim + resource;
    task_lock(tsk->group_leader);
    
    if (new_rlim) {
        // 权限检查：不能超过硬限制
        if (new_rlim->rlim_max > rlim->rlim_max &&
                !capable(CAP_SYS_RESOURCE))
            retval = -EPERM;
        if (!retval)
            retval = security_task_setrlimit(tsk, resource, new_rlim);
    }
    
    if (!retval) {
        if (old_rlim)
            *old_rlim = *rlim;  // 读取旧值
        if (new_rlim)
            *rlim = *new_rlim;  // 设置新值
    }
    task_unlock(tsk->group_leader);

    // RLIMIT_CPU 特殊处理
    if (!retval && new_rlim && resource == RLIMIT_CPU &&
        new_rlim->rlim_cur != RLIM_INFINITY &&
        IS_ENABLED(CONFIG_POSIX_TIMERS)) {
        // 更新 CPU 定时器
        // ...
    }

    return retval;
}
```

## 3. 函数调用栈

```
getrlimit(resource, rlim)
  │
  └─ do_prlimit(current, resource, NULL, &value)
       │
       ├─ 检查 resource 范围 (0 ~ RLIM_NLIMITS-1)
       │    └─ 无效 → 返回 -EINVAL
       │
       ├─ resource = array_index_nospec()  // 防止 Spectre 攻击
       │
       ├─ task_lock(current->group_leader)
       │
       ├─ value = *rlim  // 读取当前限制值
       │
       └─ task_unlock(current->group_leader)
  │
  └─ copy_to_user(rlim, &value, sizeof(*rlim))  // 复制到用户空间
       └─ 失败 → 返回 -EFAULT
```

## 4. 关键数据结构

```c
// include/uapi/linux/resource.h
struct rlimit {
    __kernel_ulong_t rlim_cur;  // 软限制 (soft limit)
    __kernel_ulong_t rlim_max;  // 硬限制 (hard limit)
};

#define RLIM_INFINITY  (~0UL)  // 无限制

// 进程信号结构中的资源限制数组
// include/linux/sched/signal.h
struct signal_struct {
    struct rlimit rlim[RLIM_NLIMITS];  // 所有资源限制
    // ...
};
```

### 资源类型定义

```c
// include/uapi/asm-generic/resource.h
#define RLIMIT_CPU        0   // CPU 时间（秒）
#define RLIMIT_FSIZE      1   // 文件大小（字节）
#define RLIMIT_DATA       2   // 数据段大小（字节）
#define RLIMIT_STACK      3   // 栈大小（字节）
#define RLIMIT_CORE       4   // core 文件大小（字节）
#define RLIMIT_RSS        5   // 驻留集大小（页数）
#define RLIMIT_NPROC      6   // 用户最大进程数
#define RLIMIT_NOFILE     7   // 打开文件数
#define RLIMIT_MEMLOCK    8   // 锁定内存大小（字节）
#define RLIMIT_AS         9   // 地址空间大小（字节）
#define RLIMIT_LOCKS     10   // 文件锁数量
#define RLIMIT_SIGPENDING 11  // 待处理信号数
#define RLIMIT_MSGQUEUE  12   // POSIX 消息队列字节数
#define RLIMIT_NICE      13   // nice 优先级
#define RLIMIT_RTPRIO    14   // 实时优先级
#define RLIMIT_RTTIME    15   // 实时 CPU 时间（微秒）
#define RLIM_NLIMITS     16   // 限制数量
```

## 5. 流程图

```
用户态调用 getrlimit(resource, &rlim)
  │
  ▼
SYSCALL_DEFINE2(getrlimit, resource, rlim)
  │
  ▼
do_prlimit(current, resource, NULL, &value)
  │
  ├─ resource >= RLIM_NLIMITS? ──是──→ 返回 -EINVAL
  │
  ├─ array_index_nospec(resource, RLIM_NLIMITS)  // Spectre 防护
  │
  ├─ task_lock(current->group_leader)
  │
  ├─ value = current->signal->rlim[resource]
  │    │
  │    └─ 读取 rlim_cur 和 rlim_max
  │
  └─ task_unlock(current->group_leader)
  │
  ▼
copy_to_user(rlim, &value, sizeof(rlim))  ──失败──→ -EFAULT
  │
  ▼
返回 0
```

## 6. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    struct rlimit rlim;
    const char *names[] = {
        "RLIMIT_CPU", "RLIMIT_FSIZE", "RLIMIT_DATA", "RLIMIT_STACK",
        "RLIMIT_CORE", "RLIMIT_RSS", "RLIMIT_NPROC", "RLIMIT_NOFILE",
        "RLIMIT_MEMLOCK", "RLIMIT_AS", "RLIMIT_LOCKS", "RLIMIT_SIGPENDING",
        "RLIMIT_MSGQUEUE", "RLIMIT_NICE", "RLIMIT_RTPRIO", "RLIMIT_RTTIME"
    };
    
    printf("Process resource limits:\n");
    printf("%-20s %-20s %-20s\n", "Resource", "Soft Limit", "Hard Limit");
    printf("--------------------------------------------------------\n");
    
    for (int i = 0; i < 16; i++) {
        if (getrlimit(i, &rlim) == 0) {
            printf("%-20s ", names[i]);
            if (rlim.rlim_cur == RLIM_INFINITY)
                printf("%-20s", "unlimited");
            else
                printf("%-20lu", rlim.rlim_cur);
            
            if (rlim.rlim_max == RLIM_INFINITY)
                printf("%-20s\n", "unlimited");
            else
                printf("%-20lu\n", rlim.rlim_max);
        }
    }
    
    return 0;
}
```

## 7. 注意事项

- `getrlimit` 是旧式接口，新代码推荐使用 `prlimit64` 或 `getrlimit` 的兼容版本
- 内核中的 `do_prlimit` 是 `getrlimit`、`setrlimit` 和 `prlimit64` 的共同核心
- 获取自己的限制时不需要任何权限
- `rlim_cur` <= `rlim_max`，软限制不能超过硬限制
- `RLIM_INFINITY`（`~0UL`）表示无限制
- 使用 `array_index_nospec` 防止 Spectre v1 边信道攻击

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 源码位置：`kernel/sys.c`