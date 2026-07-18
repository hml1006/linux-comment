# mincore 系统调用分析

## 1. 概述

`mincore` 系统调用用于检测指定地址范围内的内存页是否驻留在物理内存中（即是否在 RAM 中，而非被换出或未被分配）。

**内核源码位置：** `mm/mincore.c`

**原型：**

```c
SYSCALL_DEFINE3(mincore, unsigned long, start, size_t, len,
                unsigned char __user *, vec)
```

| 参数 | 描述 |
|------|------|
| `start` | 起始地址（必须页对齐） |
| `len` | 待检查区域长度（字节） |
| `vec` | 输出缓冲区，每个字节对应一页的状态 |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

**vec 输出：**
- 每个字节的最低有效位为 1 表示该页驻留在内存中
- 为 0 表示该页不在内存中

## 2. 使用场景

- **内存使用监控**：检查哪些页面在物理内存中，辅助内存分析工具
- **数据库管理**：检查缓冲池页面是否在内存中
- **调试与性能分析**：了解应用程序的内存驻留情况
- **大页感知**：检查透明大页（THP）的驻留状态

## 3. 函数调用链分析

```
mincore(start, len, vec)                              // 系统调用入口
  ├─ untagged_addr(start)                             // 去除地址标签
  ├─ 参数验证
  │    ├─ start 页对齐检查
  │    ├─ access_ok(start, len)                       // 用户地址范围检查
  │    └─ access_ok(vec, pages)                       // 输出缓冲区检查
  ├─ __get_free_page(GFP_USER)                        // 分配临时内核缓冲区
  └─ 循环处理（每次最多 PAGE_SIZE 个条目）
       └─ do_mincore(start, pages, tmp)                // 核心处理
            ├─ vma_lookup(mm, addr)                    // 查找 VMA
            ├─ can_do_mincore(vma)                     // 权限检查
            └─ walk_page_range(vma->vm_mm, addr, end,
                 &mincore_walk_ops, vec)               // 页表遍历
                 ├─ mincore_pte_range()                // PTE 处理（非 huge 页）
                 │    ├─ pmd_trans_huge_lock()         // THP 检查
                 │    ├─ pte_offset_map_lock()         // 获取 PTE 锁
                 │    └─ 遍历 PTE：
                 │         ├─ pte_none / pte_is_marker → 未映射
                 │         │    └─ __mincore_unmapped_range()
                 │         │         ├─ 文件映射 → mincore_page()
                 │         │         │    └─ filemap_get_entry() 检查页缓存
                 │         │         └─ 匿名映射 → 返回 0
                 │         ├─ pte_present → 驻留内存
                 │         └─ 其他 → swap 条目
                 │              └─ mincore_swap() 检查交换缓存
                 └─ mincore_hugetlb()                  // 大页处理
       └─ copy_to_user(vec, tmp, retval)              // 结果拷贝回用户空间
```

## 4. 关键数据结构

### 页表遍历操作集

```c
static const struct mm_walk_ops mincore_walk_ops = {
    .pmd_entry      = mincore_pte_range,     /* PMD/PTE 级别处理 */
    .pte_hole       = mincore_unmapped_range, /* PTE 空洞处理 */
    .hugetlb_entry  = mincore_hugetlb,        /* HugeTLB 处理 */
    .walk_lock      = PGWALK_RDLOCK,          /* 持有读锁 */
};
```

### 核心处理函数 `do_mincore`

```c
static long do_mincore(unsigned long addr, unsigned long pages, unsigned char *vec)
{
    struct vm_area_struct *vma;
    unsigned long end;
    int err;

    vma = vma_lookup(current->mm, addr);
    if (!vma)
        return -ENOMEM;
    end = min(vma->vm_end, addr + (pages << PAGE_SHIFT));

    /* 如果进程没有权限，假设所有页面都在内存中 */
    if (!can_do_mincore(vma)) {
        unsigned long pages = DIV_ROUND_UP(end - addr, PAGE_SIZE);
        memset(vec, 1, pages);
        return pages;
    }

    err = walk_page_range(vma->vm_mm, addr, end, &mincore_walk_ops, vec);
    if (err < 0)
        return err;
    return (end - addr) >> PAGE_SHIFT;
}
```

## 5. 流程图

```
  用户态调用 mincore(start, len, vec)
         │
         ▼
  ┌──────────────────────────────────┐
  │  参数验证                        │
  │  ├─ start 页对齐                 │
  │  ├─ access_ok(start, len)        │
  │  └─ access_ok(vec, pages)        │
  └──────────────┬───────────────────┘
                 ▼
  ┌──────────────────────────────────┐
  │  分配临时内核缓冲区 tmp          │
  │  __get_free_page(GFP_USER)       │
  └──────────────┬───────────────────┘
                 ▼
          ┌──────┴──────┐
          │ pages > 0?  │────no──→ 返回 0
          └──────┬──────┘
                 │ yes
                 ▼
          ┌────────────────┐
          │ do_mincore()   │  每次处理最多 PAGE_SIZE 个条目
          └────────┬───────┘
                   ▼
          ┌────────────────┐
          │ vma_lookup()   │  查找 VMA
          └────────┬───────┘
                   ▼
          ┌──────────────────────┐
          │ can_do_mincore(vma)  │  权限检查
          └──────────┬───────────┘
                     │
            ┌────────┴────────┐
            │ 无权限          │ 有权限
            │ 返回全 1        │
            └────────┬────────┘
                     ▼
          ┌──────────────────────┐
          │ walk_page_range()    │  遍历页表
          │ ├─ mincore_pte_range │  → PTE 检查
          │ ├─ mincore_hugetlb   │  → 大页检查
          │ └─ mincore_unmapped  │  → 空洞检查
          └──────────┬───────────┘
                     ▼
          ┌──────────────────────┐
          │ copy_to_user(vec,    │
          │   tmp, retval)       │  拷贝结果到用户空间
          └──────────┬───────────┘
                     ▼
          ┌──────────┴──────────┐
          │ 更新 pages, vec,    │
          │ start, 继续循环?    │
          └────────────────────┘
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | start 未页对齐 |
| `-ENOMEM` | 地址范围无效或未映射 |
| `-EFAULT` | vec 指向非法地址 |
| `-EAGAIN` | 无法分配临时内核缓冲区 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

int main() {
    size_t len = 4096 * 4;  /* 4 页 */
    unsigned char vec[4];
    char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* 刚分配（未访问）时，检查驻留状态 */
    if (mincore(addr, len, vec) == 0) {
        for (int i = 0; i < 4; i++) {
            printf("Page %d: %s\n", i,
                   (vec[i] & 1) ? "in memory" : "not in memory");
        }
    }

    /* 写入数据触发缺页 */
    memset(addr, 'A', len);

    /* 再次检查驻留状态 */
    if (mincore(addr, len, vec) == 0) {
        for (int i = 0; i < 4; i++) {
            printf("After access - Page %d: %s\n", i,
                   (vec[i] & 1) ? "in memory" : "not in memory");
        }
    }

    munmap(addr, len);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | mincore | mlock | madvise |
|------|---------|-------|---------|
| 功能 | 查询页面驻留状态 | 锁定页面到内存 | 提供使用建议 |
| 副作用 | 无 | 防止页面被换出 | 取决于 behavior |
| 权限 | 需要文件写权限 | 需要 CAP_IPC_LOCK | 无特殊权限 |
| 输出 | 用户缓冲区 | 无 | 无 |

## 9. 关键实现细节

1. **权限检查**：`can_do_mincore()` 防止信息泄露。对于匿名映射，总是允许；对于文件映射，只有当前进程有文件写权限时才允许检查，否则假设所有页面都在内存中。

2. **临时缓冲区**：内核使用一个单独页面的临时缓冲区，每次处理最多 `PAGE_SIZE` 个条目（4096 个页面），以避免在持有锁时进行大量拷贝。

3. **交换条目处理**：`mincore_swap()` 检查交换缓存，如果页面在交换缓存中且数据是最新的（uptodate），则视为驻留。

4. **页表遍历**：使用 `walk_page_range()` 框架，通过 `mincore_pte_range()` 回调逐 PTE 检查。对于 THP 直接标记为驻留，对于普通 PTE 逐一判断。

5. **不保证原子性**：mincore 检查的结果是瞬时的，页面状态可能在检查后立即改变。只有被 mlock 锁定的页面才能保证驻留。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mincore.c`
- 内核源码：`include/linux/pagewalk.h`（页表遍历框架）