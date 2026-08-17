# setrlimit 系统调用分析

## 1. 概述

`setrlimit` 系统调用用于设置进程的资源限制（resource limit）。它允许修改当前进程的各种资源限制，如 CPU 时间、文件大小、打开文件数等。

**原型：**

```c
SYSCALL_DEFINE2(setrlimit, unsigned int, resource, struct rlimit __user *, rlim);
```

| 参数 | 类型 | 描述 |
|--|--|--|
| `resource` | `unsigned int` | 资源类型标识符 |
| `rlim` | `const struct rlimit __user *` | 新的资源限制值 |

### 返回值

- 成功时返回 0
- `resource` 无效时返回 `-EINVAL`
- 权限不足时返回 `-EPERM`
- 用户空间地址无效时返回 `-EFAULT`

## 2. 完整实现

```c
// kernel/sys.c
SYSCALL_DEFINE2(setrlimit, unsigned int, resource, struct rlimit __user *, rlim)
{
    struct rlimit new_rlim;

    if (copy_from_user(&new_rlim, rlim, sizeof(*rlim)))
        return -EFAULT;
    return do_prlimit(current, resource, &new_rlim, NULL);
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
        // 验证：软限制不能超过硬限制
        if (new_rlim->rlim_cur > new_rlim->rlim_max)
            return -EINVAL;
        
        // 特殊：RLIMIT_NOFILE 的硬限制不能超过系统限制
        if (resource == RLIMIT_NOFILE &&
                new_rlim->rlim_max > sysctl_nr_open)
            return -EPERM;
    }

    rlim = tsk->signal->rlim + resource;
    task_lock(tsk->group_leader);
    
    if (new_rlim) {
        // 权限检查：提高硬限制需要 CAP_SYS_RESOURCE
        if (new_rlim->rlim_max > rlim->rlim_max &&
                !capable(CAP_SYS_RESOURCE))
            retval = -EPERM;
        
        // LSM（Linux Security Module）检查
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

    // RLIMIT_CPU 特殊处理：更新 CPU 定时器
    if (!retval && new_rlim && resource == RLIMIT_CPU &&
        new_rlim->rlim_cur != RLIM_INFINITY &&
        IS_ENABLED(CONFIG_POSIX_TIMERS)) {
        // 更新 CPU 时间限制的定时器
        // ...
    }

    return retval;
}
```

## 3. 函数调用栈

```
setrlimit(resource, rlim)
  │
  ├─ copy_from_user(&new_rlim, rlim, sizeof(*rlim))
  │    └─ 失败 → 返回 -EFAULT
  │
  └─ do_prlimit(current, resource, &new_rlim, NULL)
       │
       ├─ 检查 resource 范围
       │    └─ 无效 → 返回 -EINVAL
       │
       ├─ array_index_nospec()  // Spectre 防护
       │
       ├─ 验证新值：
       │    ├─ rlim_cur <= rlim_max?  ──否──→ 返回 -EINVAL
       │    └─ RLIMIT_NOFILE + rlim_max > sysctl_nr_open?  ──是──→ 返回 -EPERM
       │
       ├─ task_lock(current->group_leader)
       │
       ├─ 权限检查：
       │    ├─ 提高硬限制 && !CAP_SYS_RESOURCE?  ──是──→ 返回 -EPERM
       │    └─ security_task_setrlimit()  ──拒绝──→ 返回 -EPERM
       │
       ├─ 设置新值：*rlim = *new_rlim
       │
       ├─ task_unlock(current->group_leader)
       │
       └─ RLIMIT_CPU 特殊处理
            └─ 更新 CPU 定时器
```

## 4. 关键数据结构

```c
// include/uapi/linux/resource.h
struct rlimit {
    __kernel_ulong_t rlim_cur;  // 软限制 (soft limit)
    __kernel_ulong_t rlim_max;  // 硬限制 (hard limit)
};

#define RLIM_INFINITY  (~0UL)  // 无限制
```

### 资源限制规则

| 规则 | 说明 |
|--|--|
| 软限制 ≤ 硬限制 | `rlim_cur` 不能超过 `rlim_max` |
| 提高硬限制 | 需要 `CAP_SYS_RESOURCE` 能力 |
| 降低硬限制 | 任何进程都可以降低自己的硬限制（不可逆） |
| 修改软限制 | 只要不超过硬限制，任何进程都可以修改 |
| RLIMIT_NOFILE | 硬限制不能超过 `sysctl_nr_open`（通常 1048576） |

## 5. 流程图

```
用户态调用 setrlimit(resource, &rlim)
  │
  ▼
copy_from_user(&new_rlim)  ──失败──→ 返回 -EFAULT
  │
 成功
  │
  ▼
do_prlimit(current, resource, &new_rlim, NULL)
  │
  ├─ resource 范围检查 ──无效──→ 返回 -EINVAL
  │
  ├─ 新值验证:
  │    ├─ rlim_cur > rlim_max? ──是──→ 返回 -EINVAL
  │    └─ RLIMIT_NOFILE && rlim_max > sysctl_nr_open? ──是──→ -EPERM
  │
  ├─ task_lock()
  │
  ├─ 权限检查:
  │    └─ (rlim_max 增加 && !CAP_SYS_RESOURCE)? ──是──→ 返回 -EPERM
  │
  ├─ LSM 检查 (SELinux/AppArmor) ──拒绝──→ 返回 -EPERM
  │
  ├─ 写入新值: signal->rlim[resource] = new_rlim
  │
  ├─ task_unlock()
  │
  └─ RLIMIT_CPU 特殊处理
       │
       ▼
  返回 0
```

## 6. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <string.h>
#include <errno.h>

int main(void)
{
    struct rlimit old, new;
    
    // 获取当前打开文件数限制
    if (getrlimit(RLIMIT_NOFILE, &old) == 0) {
        printf("Current limit: soft=%lu, hard=%lu\n",
               old.rlim_cur, old.rlim_max);
    }
    
    // 将软限制提高到 4096（不能超过硬限制）
    new.rlim_cur = 4096;
    new.rlim_max = old.rlim_max;  // 保持硬限制不变
    
    if (setrlimit(RLIMIT_NOFILE, &new) == 0) {
        printf("New soft limit set to 4096\n");
    } else {
        printf("Failed to set limit: %s\n", strerror(errno));
    }
    
    // 尝试提高硬限制（需要 CAP_SYS_RESOURCE）
    new.rlim_cur = 65536;
    new.rlim_max = 65536;
    
    if (setrlimit(RLIMIT_NOFILE, &new) == -1) {
        // 普通用户会得到 EPERM
        printf("Cannot raise hard limit: %s\n", strerror(errno));
    }
    
    return 0;
}
```

## 7. 注意事项

- `setrlimit` 是旧式接口，新版 POSIX 推荐使用 `prlimit64`（可同时设置/获取，且支持其他进程）
- 修改资源限制会影响当前进程及所有子进程（子进程继承限制）
- 降低硬限制是不可逆的（除非有 `CAP_SYS_RESOURCE`）
- 有些资源限制的修改需要满足特定条件，例如：
  - `RLIMIT_STACK` 的降低可能影响已分配的栈空间
  - `RLIMIT_AS` 的降低可能影响已映射的内存
- 通过 `prlimit(1)` 命令行工具可以查看和修改进程资源限制

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 源码位置：`kernel/sys.c`