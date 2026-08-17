# process_madvise 系统调用分析

## 1. 概述

`process_madvise` 系统调用用于向另一个进程的内存地址空间提供使用模式建议（类似 `madvise`，但作用于其他进程）。它允许一个进程向另一个进程的内存管理提供建议，适用于系统管理工具和服务进程。

**内核源码位置：** `mm/madvise.c`

**原型：**

```c
SYSCALL_DEFINE5(process_madvise, int, pidfd, const struct iovec __user *, vec,
                size_t, vlen, int, behavior, unsigned int, flags)
```

| 参数 | 描述 |
|------|------|
| `pidfd` | 目标进程的文件描述符（通过 `pidfd_open` 获得） |
| `vec` | iovec 数组，描述地址范围 |
| `vlen` | iovec 数组长度 |
| `behavior` | 建议行为（仅支持非破坏性行为） |
| `flags` | 保留标志（当前必须为 0） |

**返回值：**
- 成功返回处理的总字节数（可能部分完成）
- 失败返回负数错误码

## 2. 支持的 behavior

`process_madvise` 仅支持以下非破坏性行为（由 `process_madvise_remote_valid()` 检查）：

| behavior | 描述 |
|----------|------|
| `MADV_COLD` | 建议页面为冷页 |
| `MADV_PAGEOUT` | 建议页面换出 |
| `MADV_WILLNEED` | 建议预读页面 |
| `MADV_POPULATE_READ` | 预填充读缺页 |
| `MADV_POPULATE_WRITE` | 预填充写缺页 |
| `MADV_COLLAPSE` | 建议合并为 THP |

## 3. 使用场景

- **内存压力管理**：系统管理服务可以建议其他进程换出冷页面
- **性能优化**：预读其他进程的页面
- **内存回收**：在系统内存不足时，管理进程可建议其他进程释放不活跃页面

## 4. 函数调用链分析

```
process_madvise(pidfd, vec, vlen, behavior, flags)        // 系统调用入口
  ├─ flags 检查（必须为 0）
  ├─ import_iovec(ITER_DEST, vec, vlen, ...)              // 导入 iovec 数组
  ├─ pidfd_get_task(pidfd, &f_flags)                      // 获取目标 task_struct
  ├─ mm_access(task, PTRACE_MODE_READ_FSCREDS)            // 获取目标 mm_struct
  ├─ 检查是否远程进程：
  │    ├─ 如果是远程进程，检查 behavior 是否有效
  │    │    └─ process_madvise_remote_valid(behavior)
  │    └─ 如果是远程进程，需要 CAP_SYS_NICE
  └─ vector_madvise(mm, &iter, behavior)                  // 批量处理
       ├─ madvise_lock()                                  // 获取锁
       └─ 循环处理每个 iovec：
            ├─ madvise_do_behavior()                      // 处理单个范围
            │    └─ madvise_walk_vmas()                   // 遍历 VMA
            │         └─ madvise_vma_behavior()           // 应用行为
            └─ 处理 -ERESTARTNOINTR 重试
       └─ madvise_unlock()
```

## 5. 关键数据结构

### iovec 结构

```c
struct iovec {
    void __user *iov_base;  /* 起始地址 */
    __kernel_size_t iov_len; /* 长度 */
};
```

## 6. 流程图

```
  用户态调用 process_madvise(pidfd, vec, vlen, behavior, flags)
         │
         ▼
  ┌──────────────────────────────┐
  │  flags 检查                  │
  │  import_iovec()              │  导入用户态 iovec
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  pidfd_get_task(pidfd)       │  获取目标进程
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mm_access(task, READ)       │  获取目标进程 mm
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  远程进程?                   │
  │  ├─ 是 → behavior 白名单检查 │
  │  │  → CAP_SYS_NICE 检查      │
  │  └─ 否 → 直接继续           │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  vector_madvise()            │  批量处理所有 iovec
  │  ┌──────────────────────┐    │
  │  │ for each iovec:      │    │
  │  │ ┌────────────────┐   │    │
  │  │ │ madvise_do_    │   │    │
  │  │ │ behavior()    │   │    │
  │  │ │ └─ 遍历 VMA   │   │    │
  │  │ │    应用行为    │   │    │
  │  │ └────────────────┘   │    │
  │  │ next iovec           │    │
  │  └──────────────────────┘    │
  └─────────────┬────────────────┘
                ▼
        返回总处理字节数
```

## 7. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | flags 非零、behavior 无效 |
| `-EBADF` | pidfd 无效 |
| `-EPERM` | 远程进程无 CAP_SYS_NICE |
| `-ENOMEM` | 地址范围无效 |
| `-ESRCH` | 目标进程不存在 |

## 8. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main() {
    /* 获取目标进程的 pidfd */
    pid_t target_pid = 1234;  /* 示例 PID */
    int pidfd = syscall(__NR_pidfd_open, target_pid, 0);
    if (pidfd == -1) {
        perror("pidfd_open");
        return 1;
    }

    /* 建议目标进程换出某些页面 */
    struct iovec iov = {
        .iov_base = (void*)0x7f0000000000,  /* 目标地址 */
        .iov_len  = 4096 * 256,              /* 1MB */
    };

    ssize_t ret = syscall(__NR_process_madvise, pidfd,
                          &iov, 1, MADV_COLD, 0);
    if (ret < 0) {
        perror("process_madvise");
    } else {
        printf("Successfully processed %zd bytes\n", ret);
    }

    close(pidfd);
    return 0;
}
```

## 9. 与相关系统调用的比较

| 特性 | process_madvise | madvise | process_madvise(自身) |
|------|----------------|---------|----------------------|
| 作用进程 | 其他进程 | 自身 | 自身（通过 pidfd） |
| 支持行为 | 非破坏性子集 | 所有行为 | 所有行为 |
| 权限要求 | CAP_SYS_NICE | 无 | 无 |
| 使用方式 | 批量 iovec | 单个范围 | 批量 iovec |

## 10. 关键实现细节

1. **白名单限制**：`process_madvise_remote_valid()` 限制远程进程只能使用非破坏性行为（MADV_COLD、MADV_PAGEOUT 等），防止恶意进程对目标进程执行破坏性操作。

2. **权限要求**：远程操作需要 `CAP_SYS_NICE` 权限，这是为了防止普通用户干扰其他进程的内存管理。

3. **批量处理**：`vector_madvise()` 支持通过 `iovec` 数组批量处理多个不连续的地址范围，在一次系统调用中完成多个操作，提高效率。

4. **部分完成**：返回值是处理的总字节数，可能小于请求的总长度。调用者需要检查返回值并重新处理未完成的部分。

5. **信号安全**：`vector_madvise()` 处理 `-ERESTARTNOINTR` 重试，防止在信号干扰下丢失工作进度。

## 11. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/madvise.c`
- 联机手册：`process_madvise(2)`