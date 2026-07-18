# mremap 系统调用分析

## 1. 概述

`mremap` 系统调用用于扩展、缩小或移动已有的内存映射。它可以在不复制数据的情况下重新映射虚拟内存区域，是内存管理中的高效操作。

**内核源码位置：** `mm/mremap.c`

**原型：**

```c
SYSCALL_DEFINE5(mremap, unsigned long, addr, unsigned long, old_len,
                unsigned long, new_len, unsigned long, flags,
                unsigned long, new_addr)
```

| 参数 | 描述 |
|------|------|
| `addr` | 原有映射的起始地址 |
| `old_len` | 原有映射的长度 |
| `new_len` | 新映射的长度 |
| `flags` | 标志位（见下方） |
| `new_addr` | MREMAP_FIXED 时的目标地址 |

**flags 值：**

| 标志 | 描述 |
|------|------|
| `MREMAP_MAYMOVE` | 允许移动映射到新地址 |
| `MREMAP_FIXED` | 映射到指定地址（隐含 MAYMOVE） |
| `MREMAP_DONTUNMAP` | 不解除旧映射（仅在 MAYMOVE 时使用） |

**返回值：**
- 成功返回新的映射地址
- 失败返回负数错误码

## 2. 使用场景

- **动态调整映射大小**：扩展或缩小 mmap 分配的内存区域
- **重新定位映射**：将映射移动到新的地址
- **内存压缩**：在地址空间中移动映射以消除碎片

## 3. 函数调用链分析

```
mremap(addr, old_len, new_len, flags, new_addr)          // 系统调用入口
  └─ do_mremap(&vrm)                                     // 核心处理
       ├─ PAGE_ALIGN(old_len/new_len)                    // 页对齐
       ├─ check_mremap_params(vrm)                       // 参数验证
       │    ├─ MREMAP_FIXED 需要 MREMAP_MAYMOVE
       │    ├─ MREMAP_DONTUNMAP 需要 MREMAP_MAYMOVE
       │    ├─ addr 页对齐检查
       │    └─ 大小检查
       ├─ mmap_write_lock_killable(mm)                   // 获取写锁
       │
       ├─ 如果只需要移动（old_len == 0 或 new_len == 0）：
       │    └─ remap_move(vrm)                           // 仅移动映射
       │
       ├─ 否则：
       │    ├─ vma_lookup(mm, addr)                      // 查找 VMA
       │    └─ check_prep_vma(vrm)                       // 准备 VMA 检查
       │         ├─ 检查 VMA 是否可修改
       │         ├─ 检查是否密封（mseal）
       │         └─ 计算 delta = new_len - old_len
       │
       ├─ 如果 MREMAP_FIXED：
       │    └─ mremap_to(vrm)                            // 移到指定地址
       │         ├─ 检查目标地址
       │         └─ move_vma(vrm)                        // 移动 VMA
       │              ├─ 分配新地址
       │              ├─ move_page_tables()               // 拷贝/移动页表
       │              │    └─ 逐 PMD 移动页表条目
       │              └─ 释放旧地址
       │
       └─ 否则（原地扩展或移动）：
            └─ mremap_at(vrm)                            // 原地修改
                 ├─ 如果扩展（new_len > old_len）：
                 │    └─ vma_expand()                    // 扩展 VMA
                 └─ 如果缩小（new_len < old_len）：
                      └─ do_vmi_munmap()                 // 解除多余部分
       └─ mmap_write_unlock(mm)
       └─ 如果扩展且 VM_LOCKED：
            └─ mm_populate()                             // 填充新扩展部分
```

## 4. 关键数据结构

### `struct vma_remap_struct`

```c
struct vma_remap_struct {
    unsigned long addr;              /* 原映射地址 */
    unsigned long old_len;           /* 原长度 */
    unsigned long new_len;           /* 新长度 */
    unsigned long flags;             /* 标志位 */
    unsigned long new_addr;          /* MREMAP_FIXED 目标地址 */
    unsigned long delta;             /* new_len - old_len */

    struct vm_userfaultfd_ctx *uf;   /* userfaultfd 上下文 */
    struct list_head *uf_unmap_early; /* 早期 unmap 列表 */
    struct list_head *uf_unmap;      /* 延迟 unmap 列表 */

    struct vm_area_struct *vma;      /* 查找到的 VMA */
    struct vm_area_struct *vma_prev; /* 前一个 VMA */
    bool mmap_locked;                /* mmap 锁状态 */
    bool populate_expand;           /* 是否需要填充扩展区域 */
    enum mremap_type remap_type;     /* 移动类型 */
    enum mremap_move move;           /* 移动方式 */
};
```

### `move_page_tables` 页表移动

```c
/* 页表移动的核心函数 */
static int move_page_tables(struct vm_area_struct *vma,
    unsigned long old_addr, unsigned long new_addr, unsigned long len,
    bool need_rmap_locks)
{
    // 逐 PMD 遍历页表
    // 对于 PMD 大页，直接移动 PMD 条目
    // 对于普通页，移动 PTE 条目
    // 支持批量移动（move_ptes）
}
```

## 5. 流程图

```
  用户态调用 mremap(old_addr, old_len, new_len, flags, new_addr)
         │
         ▼
  ┌──────────────────────────────────────┐
  │  do_mremap()                         │
  │  ├─ PAGE_ALIGN(old_len, new_len)     │
  │  ├─ check_mremap_params()            │
  │  └─ mmap_write_lock_killable()       │
  └──────────────┬───────────────────────┘
                 ▼
  ┌──────────────────────────────────────┐
  │  判断 remap 类型                     │
  │  ┌──────────────────────────────┐    │
  │  │ old_len==0 或 new_len==0?    │    │
  │  │ 是 → remap_move() (仅移动)   │    │
  │  │ 否 → 根据 flags 判断         │    │
  │  └──────────────┬───────────────┘    │
  │                 ▼                    │
  │        ┌────────┴────────┐           │
  │        │                │           │
  │   MREMAP_FIXED      其他情况         │
  │        │                │           │
  │        ▼                ▼           │
  │  ┌──────────┐    ┌──────────┐        │
  │  │mremap_to │    │mremap_at │        │
  │  │  │       │    │  │       │        │
  │  │  ▼       │    │  ▼       │        │
  │  │move_vma()│    │扩展或缩小│        │
  │  │ │        │    │ │        │        │
  │  │ ├─分配新 │    │ ├─扩展→  │        │
  │  │ │ 地址   │    │ │ vma_   │        │
  │  │ ├─移动页 │    │ │ expand │        │
  │  │ │ 表     │    │ └─缩小→  │        │
  │  │ ├─拷贝   │    │ │ do_vmi_│        │
  │  │ │ VMA    │    │ │ munmap │        │
  │  │ └─释放旧 │    │ └─────── │        │
  │  │   地址   │    │          │        │
  │  └──────────┘    └──────────┘        │
  └──────────────┬───────────────────────┘
                 ▼
  ┌──────────────────────────────────────┐
  │  mmap_write_unlock()                 │
  │  如果扩展且 VM_LOCKED:                │
  │  └─ mm_populate()                    │
  └──────────────┬───────────────────────┘
                 ▼
           返回新地址
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | addr 未页对齐、flags 无效组合、MREMAP_FIXED 无 MAYMOVE |
| `-ENOMEM` | 无法分配新地址空间 |
| `-EFAULT` | 地址范围无效 |
| `-EPERM` | 内存已被 mseal 密封 |
| `-EAGAIN` | 临时资源不足 |
| `-EOVERFLOW` | 计算溢出 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>

int main() {
    /* 分配 4 页内存 */
    size_t old_len = 4 * 4096;
    char *addr = mmap(NULL, old_len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    strcpy(addr, "Hello, mremap!");

    /* 扩展为 8 页（允许移动） */
    size_t new_len = 8 * 4096;
    char *new_addr = mremap(addr, old_len, new_len, MREMAP_MAYMOVE);
    if (new_addr == MAP_FAILED) {
        perror("mremap expand");
        return 1;
    }
    printf("Old: %p, New: %p\n", addr, new_addr);
    printf("Data: %s\n", new_addr);  /* 数据自动拷贝 */

    /* 缩小回 4 页 */
    char *shrunk = mremap(new_addr, new_len, old_len, 0);
    if (shrunk == MAP_FAILED) {
        perror("mremap shrink");
        return 1;
    }

    munmap(shrunk, old_len);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | mremap | munmap + mmap | mremap(MREMAP_FIXED) |
|------|--------|---------------|---------------------|
| 数据拷贝 | 页表移动（高效） | 需手动拷贝 | 页表移动 |
| 原子性 | 是 | 否 | 是 |
| 地址可指定 | 可 | 可 | 可 |
| VMA 合并 | 自动处理 | 需手动 | 自动处理 |
| 性能 | 高 | 低 | 中 |

## 9. 关键实现细节

1. **页表移动而非数据拷贝**：`move_page_tables()` 通过移动页表条目来实现映射的重定位，避免了大量的数据拷贝。对于大页（PMD 级别），直接移动 PMD 条目。

2. **MREMAP_DONTUNMAP**：将映射移动到新地址后，旧地址的映射保留但内容变为零填充（类似于 `mremap` + `mmap(MAP_FIXED, ANONYMOUS)` 的组合效果）。

3. **VMA 扩展**：原地扩展时，**只能扩展 VMA 的末尾**（向高地址增长），不能向低地址扩展。如果扩展方向被相邻 VMA 阻塞，需要 `MREMAP_MAYMOVE` 来移动映射。

4. **mseal 兼容性**：被密封的 VMA 不能被移动、缩小或扩展。`mremap` 在 `check_prep_vma()` 中检查 `VM_SEALED` 标志。

5. **userfaultfd 通知**：`mremap` 过程中会通知 userfaultfd 处理程序，确保移动过程中的一致性。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mremap.c`
- 联机手册：`mremap(2)`