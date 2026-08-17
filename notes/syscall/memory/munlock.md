# munlock 系统调用分析

## 1. 概述

`munlock` 系统调用用于解锁指定地址范围内之前通过 `mlock` 锁定的内存页，允许这些页面被内核换出到交换空间。

**内核源码位置：** `mm/mlock.c`

**原型：**

```c
SYSCALL_DEFINE2(munlock, unsigned long, start, size_t, len)
```

| 参数 | 描述 |
|------|------|
| `start` | 起始地址 |
| `len` | 区域长度（字节） |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **释放锁定内存**：在不再需要保证内存驻留时，允许页面被换出
- **资源管理**：临时锁定内存完成操作后解锁
- **与 mlock 配对**：每个 `mlock` 应配对 `munlock`

## 3. 函数调用链分析

```
munlock(start, len)                                      // 系统调用入口
  ├─ untagged_addr(start)
  ├─ 页对齐处理
  ├─ mmap_write_lock_killable(mm)                        // 获取写锁
  ├─ apply_vma_lock_flags(start, len, 0)                 // 清除锁定标志
  │    └─ for_each_vma_range(vmi, vma, end)
  │         └─ mlock_fixup(vma, newflags=清除VM_LOCKED)
  │              ├─ 清除 VM_LOCKED / VM_LOCKONFAULT 标志
  │              └─ 更新页表状态
  └─ mmap_write_unlock(mm)                               // 释放写锁
```

## 4. 关键数据结构

```c
/* 与 mlock 共享相同的 VMA 标志位 */
#define VM_LOCKED      0x00002000
#define VM_LOCKONFAULT 0x00080000
```

## 5. 流程图

```
  用户态调用 munlock(start, len)
         │
         ▼
  ┌──────────────────────────────┐
  │  页对齐处理                   │
  │  len = PAGE_ALIGN(...)       │
  │  start &= PAGE_MASK          │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_lock_killable()  │  获取写锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  apply_vma_lock_flags(flags=0)│  清除锁定标志
  │  ┌──────────────────────┐    │
  │  │ for_each VMA:        │    │
  │  │ ┌────────────────┐   │    │
  │  │ │ mlock_fixup()  │   │    │
  │  │ │ ├─ 清除 VM_    │   │    │
  │  │ │ │   LOCKED     │   │    │
  │  │ │ └─ 更新页表    │   │    │
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
| `-ENOMEM` | 地址范围包含未映射区域 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

int main() {
    size_t len = 1024 * 1024;
    char *buf = malloc(len);
    if (!buf) { perror("malloc"); return 1; }

    memset(buf, 'A', len);

    /* 锁定内存 */
    if (mlock(buf, len) == -1) {
        perror("mlock");
        free(buf);
        return 1;
    }

    printf("Memory locked\n");

    /* 使用完毕后解锁 */
    if (munlock(buf, len) == -1) {
        perror("munlock");
    }

    printf("Memory unlocked\n");
    free(buf);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | munlock | munlockall | mlock |
|------|---------|------------|-------|
| 作用范围 | 指定地址范围 | 整个地址空间 | 锁定操作 |
| 清除 def_flags | 否 | 是 | 不适用 |
| 权限要求 | 无 | 无 | CAP_IPC_LOCK |

## 9. 关键实现细节

1. **与 mlock 共享实现**：`munlock` 和 `mlock` 使用相同的 `apply_vma_lock_flags()` 函数，只是传入的 `flags` 参数为 0（清除所有锁定标志）。

2. **无需权限检查**：`munlock` 不需要 `CAP_IPC_LOCK` 权限，任何进程都可以解锁自己的内存。

3. **无 `__mm_populate` 调用**：与 `mlock` 不同，`munlock` 不需要触发缺页，因为它只是清除锁定标志。

4. **mlock_fixup 中的解锁操作**：当 `mlock_fixup` 检测到从锁定变为非锁定状态时，会更新页表并可能触发页面回收。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mlock.c`
- 联机手册：`munlock(2)`