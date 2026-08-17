# mlockall 系统调用分析

## 1. 概述

`mlockall` 系统调用用于锁定调用进程整个地址空间（或未来映射）的内存页，防止它们被换出到交换空间。与 `mlock` 不同，它影响进程的整个地址空间。

**内核源码位置：** `mm/mlock.c`

**原型：**

```c
SYSCALL_DEFINE1(mlockall, int, flags)
```

| 参数 | 描述 |
|------|------|
| `flags` | 控制标志（见下方） |

**flags 值：**

| 标志 | 描述 |
|------|------|
| `MCL_CURRENT` | 锁定当前所有已映射的内存页 |
| `MCL_FUTURE` | 锁定未来所有映射的内存页（通过 mmap、brk 等） |
| `MCL_ONFAULT` | 与 MCL_CURRENT 和/或 MCL_FUTURE 配合使用，仅在缺页时锁定 |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **实时系统初始化**：启动时锁定所有内存，避免运行时缺页中断
- **数据库服务器**：确保整个内存工作集常驻
- **安全敏感应用**：防止任何内存数据被换出到磁盘

## 3. 函数调用链分析

```
mlockall(flags)                                        // 系统调用入口
  ├─ 参数验证（flags 有效性检查）
  ├─ can_do_mlock()                                   // 权限检查
  ├─ rlimit(RLIMIT_MEMLOCK)                           // 获取锁定限制
  ├─ mmap_write_lock_killable(mm)                     // 获取写锁
  ├─ 检查 total_vm 是否超过 lock_limit（除非 CAP_IPC_LOCK）
  └─ apply_mlockall_flags(flags)                      // 核心处理
       ├─ 清除 mm->def_flags 中的 VM_LOCKED_MASK
       ├─ 如果 MCL_FUTURE：
       │    └─ mm->def_flags |= VM_LOCKED
       │    └─ 如果 MCL_ONFAULT:
       │         └─ mm->def_flags |= VM_LOCKONFAULT
       ├─ 如果 MCL_CURRENT：
       │    └─ for_each_vma(vmi, vma)                 // 遍历所有 VMA
       │         └─ mlock_fixup(vma, newflags)         // 设置每个 VMA 的锁定标志
       │              └─ 设置 VM_LOCKED | VM_LOCKONFAULT
       └─ 返回 0
  └─ mmap_write_unlock(mm)
  └─ 如果 MCL_CURRENT 且成功：
       └─ mm_populate(0, TASK_SIZE)                   // 触发缺页填充整个地址空间
```

## 4. 关键数据结构

### `mm->def_flags` 与锁定

```c
struct mm_struct {
    // ...
    unsigned long def_flags;  /* 新建映射的默认标志 */
    unsigned long total_vm;   /* 进程总页面数 */
    unsigned long locked_vm;  /* 已锁定页面数 */
    // ...
};

/* def_flags 中相关标志 */
#define VM_LOCKED      0x00002000  /* 页面被锁定在内存中 */
#define VM_LOCKONFAULT 0x00080000  /* 仅在缺页时锁定 */
```

### `apply_mlockall_flags` 实现

```c
static int apply_mlockall_flags(int flags)
{
    VMA_ITERATOR(vmi, current->mm, 0);
    struct vm_area_struct *vma, *prev = NULL;
    vm_flags_t to_add = 0;

    current->mm->def_flags &= ~VM_LOCKED_MASK;
    if (flags & MCL_FUTURE) {
        current->mm->def_flags |= VM_LOCKED;
        if (flags & MCL_ONFAULT)
            current->mm->def_flags |= VM_LOCKONFAULT;
        if (!(flags & MCL_CURRENT))
            goto out;
    }

    if (flags & MCL_CURRENT) {
        to_add |= VM_LOCKED;
        if (flags & MCL_ONFAULT)
            to_add |= VM_LOCKONFAULT;
    }

    for_each_vma(vmi, vma) {
        // 对每个 VMA，设置锁定标志
        newflags = (vma->vm_flags & ~VM_LOCKED_MASK) | to_add;
        error = mlock_fixup(&vmi, vma, &prev, vma->vm_start,
                          vma->vm_end, newflags);
        // 忽略错误，继续处理下一个 VMA
    }
out:
    return 0;
}
```

## 5. 流程图

```
  用户态调用 mlockall(flags)
         │
         ▼
  ┌──────────────────────────────┐
  │  参数验证                    │
  │  flags & ~(MCL_CURRENT|      │
  │         MCL_FUTURE|MCL_ONFAULT)│
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  can_do_mlock()              │  权限检查
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  RLIMIT_MEMLOCK 检查          │
  │  total_vm <= lock_limit?     │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  apply_mlockall_flags()      │
  │                              │
  │  ┌─────────────────────┐     │
  │  │ 清理 def_flags 锁位  │     │
  │  └──────────┬──────────┘     │
  │             ▼                │
  │  ┌─────────────────────┐     │
  │  │ MCL_FUTURE?         │─────yes──→ def_flags |= VM_LOCKED
  │  └──────────┬──────────┘              (可能 + VM_LOCKONFAULT)
  │             │ no                      │
  │             ▼                         ▼
  │  ┌─────────────────────┐             │
  │  │ MCL_CURRENT?        │─────────────│────→ 遍历所有 VMA
  │  └──────────┬──────────┘             │     └→ mlock_fixup()
  │             │ no                     │
  │             ▼                         ▼
  │         跳过处理                   返回 0
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  MCL_CURRENT 且成功?         │
  │  └─ mm_populate(0, TASK_SIZE)│  触发缺页填充整个地址空间
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | flags 无效（如仅 MCL_ONFAULT 而不带其他标志） |
| `-EPERM` | 没有 CAP_IPC_LOCK 权限且 RLIMIT_MEMLOCK 为 0 |
| `-ENOMEM` | 总页面数超过 RLIMIT_MEMLOCK 限制且无 CAP_IPC_LOCK |
| `-EINTR` | 获取 mmap 写锁时被信号中断 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    /* 锁定当前所有内存，并确保未来映射也被锁定 */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        perror("mlockall");
        return 1;
    }

    /* 此时所有已映射和将来映射的内存都会被锁定 */

    /* ... 执行实时关键操作 ... */

    /* 解锁所有内存 */
    if (munlockall() == -1) {
        perror("munlockall");
        return 1;
    }

    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | mlockall | mlock | munlockall |
|------|----------|-------|------------|
| 作用范围 | 整个地址空间 | 指定地址范围 | 整个地址空间 |
| MCL_FUTURE | 影响未来映射 | 无 | 清除 def_flags |
| 性能影响 | 大（涉及所有页面） | 中（仅指定范围） | 大 |
| 使用场景 | 实时系统初始化 | 特定数据缓冲区 | 恢复默认状态 |

## 9. 关键实现细节

1. **MCL_FUTURE 机制**：通过修改 `mm->def_flags` 实现。后续 `mmap`、`brk` 等操作在创建新 VMA 时会包含 `mm->def_flags` 中的标志，从而自动锁定新分配的内存。

2. **MCL_ONFAULT 优化**：与 `MCL_CURRENT` 或 `MCL_FUTURE` 配合使用，页面仅在首次访问时锁定，而非立即分配。这避免了不必要的物理内存分配，特别适合只在某些条件下访问的大内存区域。

3. **非阻塞行为**：`apply_mlockall_flags()` 在遍历 VMA 设置锁定标志时，对于单个 VMA 的错误会忽略并继续处理下一个 VMA，确保尽可能多的 VMA 被锁定。

4. **mm_populate 范围**：当指定 `MCL_CURRENT` 时，`mm_populate(0, TASK_SIZE)` 会触发整个地址空间的缺页，确保所有页面都驻留在物理内存中。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mlock.c`
- 联机手册：`mlockall(2)`