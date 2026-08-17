# munmap 系统调用分析

## 1. 概述

`munmap` 系统调用用于解除进程地址空间中的内存映射，释放相关的虚拟内存区域（VMA）和页表。

**内核源码位置：** `mm/mmap.c`（入口），`mm/vma.c`（核心实现）

**原型：**

```c
SYSCALL_DEFINE2(munmap, unsigned long, addr, size_t, len)
```

| 参数 | 描述 |
|------|------|
| `addr` | 起始地址 |
| `len` | 区域长度（字节） |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **释放 mmap 分配的内存**：与 `mmap` 配对使用
- **解除文件映射**：关闭文件映射时释放资源
- **地址空间管理**：在 MAP_FIXED 映射前解除已有映射
- **进程退出时**：内核自动调用 munmap 释放所有映射

## 3. 函数调用链分析

```
munmap(addr, len)                                        // 系统调用入口
  └─ __vm_munmap(addr, len, unlock=true)                 // 核心处理
       ├─ mmap_write_lock_killable(mm)                   // 获取写锁
       └─ do_vmi_munmap(&vmi, mm, start, len, &uf, unlock) // 执行解除
            ├─ 参数验证与对齐
            ├─ arch_unmap(mm, addr, len)                 // 架构特定操作
            ├─ 检查 VMA 是否可解除（mseal 检查）
            ├─ detach_vmas_to_be_unmapped(vmi, mm, ...)  // 分离要解除的 VMA
            │    └─ 从 mm->mmap 链表和红黑树中移除 VMA
            ├─ unmap_region(mm, &tlb, vmas, ...)         // 释放页表
            │    ├─ tlb_gather_mmu()                     // 初始化 TLB 收集器
            │    ├─ unmap_vmas(&tlb, vma, start, end)    // 遍历 VMA 清除 PTE
            │    │    └─ zap_page_range()                // 清除页表条目
            │    │         └─ tlb_flush_mmu()            // 刷新 TLB
            │    └─ tlb_finish_mmu()                     // 完成 TLB 刷新
            ├─ 处理 userfaultfd 通知
            └─ remove_vma_list(vmas)                     // 释放 VMA 结构
                 └─ remove_vma(vma)                      // 释放单个 VMA
                      ├─ 文件映射 → fput(vma->vm_file)
                      └─ kmem_cache_free(vma)            // 释放 VMA 缓存
       └─ mmap_write_unlock(mm)                          // 释放写锁（如果 unlock）
```

## 4. 关键数据结构

### VMA 解除流程

```c
/* 要解除的 VMA 列表 */
struct vma_munmap_struct {
    struct vma_iterator *vmi;            /* VMA 迭代器 */
    struct vm_area_struct *vma;          /* 找到的起始 VMA */
    struct vm_area_struct *prev;         /* 前一个 VMA */
    struct vm_area_struct *next;         /* 下一个 VMA */
    struct list_head *uf;                /* userfaultfd 列表 */
    unsigned long start;                 /* 解除起始地址 */
    unsigned long end;                   /* 解除结束地址 */
    int vma_count;                       /* 解除的 VMA 数量 */
    bool mmap_locked;                    /* mmap 锁状态 */
    bool vma_locked;                     /* VMA 锁状态 */
    // ...
};
```

## 5. 流程图

```
  用户态调用 munmap(addr, len)
         │
         ▼
  ┌──────────────────────────────┐
  │  __vm_munmap()               │
  │  mmap_write_lock_killable()  │  获取写锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  do_vmi_munmap()             │
  │  ├─ arch_unmap()             │  架构相关操作
  │  ├─ mseal 检查               │  密封区域不可解除
  │  └─ detach_vmas_to_be_       │  从红黑树/链表分离 VMA
  │      unmapped()              │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  unmap_region()              │  释放页表
  │  ┌──────────────────────┐    │
  │  │ tlb_gather_mmu()    │    │  初始化 TLB 收集
  │  │ unmap_vmas()        │    │  逐个 VMA 清除 PTE
  │  │ │  └─ zap_page_     │    │
  │  │ │     range()       │    │  清除页表条目
  │  │ └─ tlb_flush_mmu()  │    │  刷新 TLB
  │  │ tlb_finish_mmu()    │    │  完成 TLB 刷新
  │  └──────────────────────┘    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  remove_vma_list()           │  释放 VMA 结构
  │  ┌──────────────────────┐    │
  │  │ for each VMA:        │    │
  │  │ ├─ 文件映射: fput()  │    │
  │  │ ├─ 释放 anon_vma     │    │
  │  │ └─ kmem_cache_free() │    │
  │  └──────────────────────┘    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_unlock(mm)       │  释放写锁
  │  userfaultfd_unmap_complete()│
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | len=0、addr 未对齐 |
| `-ENOMEM` | 地址范围无效 |
| `-EPERM` | 地址范围包含被 mseal 密封的 VMA |
| `-EINTR` | 获取 mmap 写锁时被信号中断 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

int main() {
    /* 分配 4 页内存 */
    size_t len = 4 * 4096;
    char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    strcpy(addr, "Hello, munmap!");

    /* 解除前 2 页的映射 */
    if (munmap(addr, 2 * 4096) == -1) {
        perror("munmap partial");
        return 1;
    }

    /* 解除剩余 2 页的映射 */
    if (munmap(addr + 2 * 4096, 2 * 4096) == -1) {
        perror("munmap rest");
        return 1;
    }

    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | munmap | mmap(MAP_FIXED) | brk | mremap |
|------|--------|-----------------|-----|--------|
| 功能 | 解除映射 | 创建映射覆盖 | 收缩堆 | 移动/调整映射 |
| VMA 处理 | 删除 | 替换 | 删除 | 修改/移动 |
| 页表处理 | 清除 | 替换 | 清除 | 移动 |

## 9. 关键实现细节

1. **VMA 分离（detach）**：`detach_vmas_to_be_unmapped()` 将受影响的 VMA 从 `mm->mmap` 链表和 `mm->mm_rb` 红黑树中分离，但此时不释放内存，确保操作原子性。

2. **两阶段释放**：先清除页表（`unmap_region`），再释放 VMA 结构（`remove_vma_list`）。如果中间发生错误，VMA 已从树中移除但未释放，内存管理仍然安全。

3. **mseal 保护**：`munmap` 会检查 VMA 是否被密封（`VM_SEALED`），如果目标 VMA 被密封，操作返回 `-EPERM`。

4. **TLB 刷新**：`unmap_region` 使用 `tlb_gather_mmu`/`tlb_finish_mmu` 机制批量收集和刷新 TLB，而不是每次 PTE 修改都刷新，提高性能。

5. **userfaultfd 通知**：`unmap` 操作会通知 userfaultfd 处理程序，以便跟踪页面状态变化。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mmap.c`（入口）
- 内核源码：`mm/vma.c`（核心实现：do_vmi_munmap, unmap_region）
- 联机手册：`munmap(2)`