# pkey_free 系统调用分析

## 1. 概述

`pkey_free` 系统调用用于释放先前通过 `pkey_alloc` 分配的内存保护密钥，使其可供后续分配使用。

**内核源码位置：** `mm/mprotect.c`（需要 `CONFIG_ARCH_HAS_PKEYS`）

**原型：**

```c
SYSCALL_DEFINE1(pkey_free, int, pkey)
```

| 参数 | 描述 |
|------|------|
| `pkey` | 要释放的 pkey 编号 |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **资源回收**：不再需要 pkey 保护时释放，避免浪费 pkey 资源
- **与 pkey_alloc 配对**：每个 pkey_alloc 应配对 pkey_free

## 3. 函数调用链分析

```
pkey_free(pkey)                                          // 系统调用入口
  ├─ mmap_write_lock(current->mm)                        // 获取写锁
  ├─ mm_pkey_free(current->mm, pkey)                     // 释放 pkey
  │    └─ 在 mm->pkey_bitmap 中清除对应位
  └─ mmap_write_unlock(current->mm)                      // 释放写锁
```

## 4. 关键数据结构

```c
struct mm_struct {
    unsigned long pkey_bitmap;  /* 已分配的 pkey 位图 */
};

/* 内部函数 */
static inline int mm_pkey_free(struct mm_struct *mm, int pkey)
{
    if (!mm_pkey_is_allocated(mm, pkey))
        return -EINVAL;
    __clear_bit(pkey, &mm->pkey_bitmap);  /* 清除位图中的对应位 */
    return 0;
}
```

## 5. 流程图

```
  用户态调用 pkey_free(pkey)
         │
         ▼
  ┌──────────────────────────────┐
  │  mmap_write_lock()           │  获取写锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mm_pkey_free()              │
  │  ├─ pkey 是否已分配?         │
  │  │  └─ 否 → -EINVAL          │
  │  └─ 清除位图对应位           │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_unlock()         │
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | pkey 未分配或无效 |
| `-EINTR` | 获取 mmap 写锁时被信号中断 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>

int main() {
    int pkey = pkey_alloc(0, 0);
    if (pkey == -1) {
        perror("pkey_alloc");
        return 1;
    }

    printf("Allocated pkey: %d\n", pkey);

    /* 使用 pkey ... */

    /* 释放 pkey */
    if (pkey_free(pkey) == -1) {
        perror("pkey_free");
        return 1;
    }

    printf("pkey %d freed\n", pkey);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | pkey_free | pkey_alloc | munmap |
|------|-----------|------------|--------|
| 功能 | 释放密钥 | 分配密钥 | 释放内存映射 |
| 影响范围 | 全局 | 全局 | 局部 |
| 是否需要 mmap 写锁 | 是 | 是 | 是 |

## 9. 关键实现细节

1. **简单实现**：`pkey_free` 的实现非常简单——仅清除 `mm->pkey_bitmap` 中的对应位。它不检查是否有 VMA 仍在使用该 pkey，但内核注释提到这是一个可改进的地方。

2. **无硬件操作**：与 `pkey_alloc` 不同，`pkey_free` 不调用任何架构特定的硬件操作。释放 pkey 只会让该 pkey 号可被重新分配。

3. **VMA 残留**：pkey 释放后，如果某些 VMA 仍引用该 pkey，这些 VMA 的 pkey 设置不会自动清除。但重新分配 pkey 后，这些 VMA 会使用新的 pkey 权限。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mprotect.c`
- 联机手册：`pkey_free(2)`