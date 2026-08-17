# munlockall 系统调用分析

## 1. 概述

`munlockall` 系统调用用于解锁调用进程整个地址空间中所有被锁定的内存页，并且清除 `mm->def_flags` 中的 `VM_LOCKED` 标志，使得未来映射也不会被自动锁定。

**内核源码位置：** `mm/mlock.c`

**原型：**

```c
SYSCALL_DEFINE0(munlockall)
```

**参数：** 无

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **撤销 mlockall 效果**：在完成实时操作后恢复默认内存管理行为
- **资源释放**：让内核可以自由换出不再需要的页面

## 3. 函数调用链分析

```
munlockall()                                             // 系统调用入口
  ├─ mmap_write_lock_killable(mm)                        // 获取写锁
  └─ apply_mlockall_flags(0)                             // 清除所有锁定标志
       ├─ mm->def_flags &= ~VM_LOCKED_MASK              // 清除默认锁定标志
       └─ 遍历所有 VMA：
            └─ for_each_vma(vmi, vma)
                 └─ mlock_fixup(vma, newflags=清除锁定标志)
                      ├─ 清除 VM_LOCKED / VM_LOCKONFAULT
                      └─ 更新页表
  └─ mmap_write_unlock(mm)                               // 释放写锁
```

## 4. 关键数据结构

```c
/* 同 mlockall */
struct mm_struct {
    unsigned long def_flags;  /* 包含 VM_LOCKED 等默认标志 */
};
```

## 5. 流程图

```
  用户态调用 munlockall()
         │
         ▼
  ┌──────────────────────────────┐
  │  mmap_write_lock_killable()  │  获取写锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  apply_mlockall_flags(0)     │
  │  ┌──────────────────────┐    │
  │  │ 清除 def_flags 中的   │    │
  │  │ VM_LOCKED_MASK       │    │
  │  └──────────┬───────────┘    │
  │             ▼                │
  │  ┌──────────────────────┐    │
  │  │ 遍历所有 VMA:        │    │
  │  │ ┌────────────────┐   │    │
  │  │ │ mlock_fixup()  │   │    │
  │  │ │ 清除 VM_       │   │    │
  │  │ │ LOCKED 标志    │   │    │
  │  │ └────────────────┘   │    │
  │  │ next VMA             │    │
  │  └──────────────────────┘    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_unlock()         │  释放写锁
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINTR` | 获取 mmap 写锁时被信号中断 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>

int main() {
    /* 锁定所有内存 */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        perror("mlockall");
        return 1;
    }

    printf("All memory locked\n");

    /* ... 执行实时操作 ... */

    /* 解锁所有内存 */
    if (munlockall() == -1) {
        perror("munlockall");
        return 1;
    }

    printf("All memory unlocked\n");
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | munlockall | munlock | mlockall |
|------|------------|---------|----------|
| 作用范围 | 整个地址空间 | 指定范围 | 整个地址空间 |
| 清除 def_flags | 是 | 否 | 设置 def_flags |
| 调用权限 | 无 | 无 | CAP_IPC_LOCK |
| 使用场景 | 撤销 mlockall | 释放特定区域 | 锁定所有内存 |

## 9. 关键实现细节

1. `munlockall` 调用 `apply_mlockall_flags(0)`，参数 0 表示清除所有标志，与 `mlockall` 使用相同的底层函数。

2. 与 `mlockall` 不同，`munlockall` 不需要 `mm_populate()` 调用，因为它是解锁操作。

3. 任何进程都可以调用 `munlockall`，无需特殊权限。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mlock.c`
- 联机手册：`munlockall(2)`