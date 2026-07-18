# mlock 系统调用分析

## 1. 概述

`mlock` 系统调用用于将指定地址范围内的内存页锁定在物理内存中，防止它们被换出到交换空间。这对于需要保证响应时间的实时应用、处理敏感数据（如密码）的应用，或需要避免缺页中断的性能关键型应用非常重要。

**内核源码位置：** `mm/mlock.c`

**原型：**

```c
SYSCALL_DEFINE2(mlock, unsigned long, start, size_t, len)
```

| 参数 | 描述 |
|------|------|
| `start` | 起始地址 |
| `len` | 区域长度（字节） |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

**相关系统调用：**
- `mlock2(start, len, flags)` — 扩展版，支持 `MLOCK_ONFAULT` 标志（仅在缺页时锁定）

## 2. 使用场景

- **实时应用**：防止因页面换出导致延迟抖动
- **加密/敏感数据处理**：防止敏感数据被换出到磁盘
- **数据库系统**：确保关键数据结构和缓冲池常驻内存
- **高性能计算**：避免缺页中断对性能的影响

## 3. 函数调用链分析

```
mlock(start, len)                                      // 系统调用入口
  └─ do_mlock(start, len, VM_LOCKED)                   // 核心处理
       ├─ can_do_mlock()                               // 检查权限（CAP_IPC_LOCK 或 RLIMIT_MEMLOCK）
       ├─ PAGE_ALIGN / start &= PAGE_MASK              // 页对齐处理
       ├─ rlimit(RLIMIT_MEMLOCK)                       // 获取锁定限制
       ├─ mmap_write_lock_killable(mm)                 // 获取写锁
       ├─ 检查 locked_vm 是否超过 RLIMIT_MEMLOCK
       │    └─ count_mm_mlocked_page_nr()              // 统计已锁定页面数
       ├─ apply_vma_lock_flags(start, len, flags)       // 应用 VMA 锁定标志
       │    └─ for_each_vma_range()                     // 遍历受影响 VMA
       │         └─ mlock_fixup()                       // 对单个 VMA 修正
       │              ├─ 设置 VM_LOCKED 标志
       │              ├─ 如果新锁定，填充页表
       │              └─ 如果解锁，执行解锁操作
       ├─ mmap_write_unlock(mm)                        // 释放写锁
       └─ __mm_populate(start, len, 0)                 // 触发缺页，确保页面驻留
            └─ populate_vma_page_range()                // 实际填充页表
                 └─ __get_user_pages()                  // 获取/创建页面
```

## 4. 关键数据结构

### VMA 锁标志

```c
/* VMA 标志位中与锁相关的位 */
#define VM_LOCKED      0x00002000  /* 页面被锁定在内存中 */
#define VM_LOCKONFAULT 0x00080000  /* 仅在缺页时锁定（mlock2 MLOCK_ONFAULT） */
#define VM_LOCKED_MASK (VM_LOCKED | VM_LOCKONFAULT)  /* 锁掩码 */
```

### `apply_vma_lock_flags` 核心处理

```c
static int apply_vma_lock_flags(unsigned long start, size_t len,
                                vm_flags_t flags)
{
    unsigned long nstart, end, tmp;
    struct vm_area_struct *vma, *prev;
    VMA_ITERATOR(vmi, current->mm, start);

    VM_BUG_ON(offset_in_page(start));
    VM_BUG_ON(len != PAGE_ALIGN(len));
    end = start + len;
    if (end < start)
        return -EINVAL;
    if (end == start)
        return 0;
    vma = vma_iter_load(&vmi);
    if (!vma)
        return -ENOMEM;
    // ... 遍历 VMA 并调用 mlock_fixup
}
```

## 5. 流程图

```
  用户态调用 mlock(start, len)
         │
         ▼
  ┌──────────────────────────────┐
  │  can_do_mlock()              │
  │  └─ RLIMIT_MEMLOCK 检查       │
  │  └─ CAP_IPC_LOCK 检查         │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  页对齐处理                   │
  │  len = PAGE_ALIGN(...)       │
  │  start &= PAGE_MASK          │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  锁定限制检查                 │
  │  locked <= RLIMIT_MEMLOCK?   │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  apply_vma_lock_flags()      │
  │  ┌──────────────────────┐    │
  │  │  for_each VMA:       │    │
  │  │  ┌────────────────┐  │    │
  │  │  │ mlock_fixup()  │  │    │
  │  │  │ ├─ 设置 VM_    │  │    │ 设置 VM_LOCKED 标志
  │  │  │ │   LOCKED     │  │    │
  │  │  │ └─ 更新页表    │  │    │
  │  │  └────────────────┘  │    │
  │  │  next VMA            │    │
  │  └──────────────────────┘    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  __mm_populate(start, len, 0)│  触发缺页，确保页面驻留
  │  └─ populate_vma_page_range()│
  │       └─ __get_user_pages()  │
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EPERM` | 没有 CAP_IPC_LOCK 权限且 RLIMIT_MEMLOCK 为 0 |
| `-EINTR` | 获取 mmap 写锁时被信号中断 |
| `-ENOMEM` | 地址范围包含未映射区域 |
| `-EAGAIN` | 锁定页面数超过 RLIMIT_MEMLOCK 限制 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

int main() {
    size_t len = 1024 * 1024;  /* 1MB */
    char *buf = malloc(len);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    /* 写入数据，触发缺页分配物理页 */
    memset(buf, 'A', len);

    /* 锁定内存，防止被换出 */
    if (mlock(buf, len) == -1) {
        perror("mlock");
        free(buf);
        return 1;
    }

    printf("Memory locked: %zu bytes\n", len);

    /* 使用锁定内存处理敏感数据 */
    // ...

    /* 解锁内存 */
    if (munlock(buf, len) == -1) {
        perror("munlock");
    }

    free(buf);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | mlock | mlock2 | mlockall | mlock_onfault |
|------|-------|--------|----------|---------------|
| 处理范围 | 指定地址范围 | 指定地址范围 | 整个地址空间 | 指定地址范围 |
| 锁定时机 | 立即锁定 | 立即锁定 | 立即锁定 | 仅在缺页时锁定 |
| 标志位 | 无 | `MLOCK_ONFAULT` | `MCL_CURRENT/MCL_FUTURE` | `MLOCK_ONFAULT` |

## 9. 关键实现细节

1. **权限与资源限制**：`can_do_mlock()` 检查当前进程是否有 `CAP_IPC_LOCK` 权限，或 `RLIMIT_MEMLOCK` 软限制是否非零。非特权进程只能锁定有限量的内存。

2. **两阶段过程**：mlock 先通过 `apply_vma_lock_flags()` 设置 VMA 的 `VM_LOCKED` 标志，然后通过 `__mm_populate()` 触发缺页，确保物理页面被分配并驻留。

3. **mlock2(MLOCK_ONFAULT)**：当使用 `MLOCK_ONFAULT` 标志时，`__mm_populate()` 不会被调用，页面仅在首次访问（缺页）时被锁定，避免了立即分配不需要的页面。

4. **锁定计数**：`count_mm_mlocked_page_nr()` 统计已锁定的页面数量，用于检查是否超过 `RLIMIT_MEMLOCK` 限制。如果新区域与已有锁定区域重叠，重叠部分不计入新增锁定计数。

5. **mlock_fixup**：负责修改 VMA 的锁定标志，并在必要时通过 `__mm_populate()` 或解锁操作处理页表。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mlock.c`
- 联机手册：`mlock(2)`, `mlock2(2)`