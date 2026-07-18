# mmap 系统调用分析

## 1. 概述

`mmap` 系统调用用于在进程的虚拟地址空间中创建内存映射。它可以用于文件映射（将文件内容映射到内存）和匿名映射（分配内存）。这是 Linux 中最核心的内存管理系统调用之一。

**内核源码位置：** `mm/mmap.c`

**原型：**

```c
SYSCALL_DEFINE6(mmap_pgoff, unsigned long, addr, unsigned long, len,
                unsigned long, prot, unsigned long, flags,
                unsigned long, fd, unsigned long, pgoff)
```

各架构包装函数可能不同（如 `mmap`、`mmap2`），但最终都调用 `ksys_mmap_pgoff()`。

| 参数 | 描述 |
|------|------|
| `addr` | 期望的映射起始地址（hint，MAP_FIXED 时为强制） |
| `len` | 映射长度（字节） |
| `prot` | 内存保护标志（PROT_READ/WRITE/EXEC/NONE） |
| `flags` | 映射类型和选项（MAP_SHARED/PRIVATE/FIXED/ANONYMOUS 等） |
| `fd` | 文件描述符（匿名映射时为 -1） |
| `pgoff` | 文件偏移（以页为单位） |

**返回值：**
- 成功返回映射的起始地址
- 失败返回 `MAP_FAILED`（即 `(void*)-1`）

## 2. 使用场景

- **内存分配**：`malloc` 在大内存分配时使用 `mmap(MAP_ANONYMOUS)`
- **文件 I/O**：将文件映射到内存，通过内存访问替代 read/write
- **共享内存**：`MAP_SHARED` 用于进程间共享内存
- **动态库加载**：加载器使用 `mmap` 将共享库映射到进程地址空间
- **内核/驱动接口**：mmap 设备内存用于 DMA 或 MMIO

## 3. 函数调用链分析

```
mmap(addr, len, prot, flags, fd, offset)               // 架构特定入口
  │
  └─ ksys_mmap_pgoff(addr, len, prot, flags, fd, pgoff) // 通用入口
       ├─ 文件映射：
       │    ├─ fget(fd)                                 // 获取文件引用
       │    ├─ 大页检查（文件大页或 MAP_HUGETLB）
       │    └─ 参数调整（大页对齐）
       └─ 匿名映射：
            └─ 如果 MAP_HUGETLB，创建 hugetlb 文件
       └─ vm_mmap_pgoff(file, addr, len, prot, flags, pgoff)
            └─ mmap_write_lock_killable(mm)             // 获取写锁
            └─ do_mmap(file, addr, len, prot, flags, vm_flags, pgoff, ...)
                 ├─ 参数验证：
                 │    ├─ READ_IMPLIES_EXEC 处理
                 │    ├─ len 页对齐与溢出检查
                 │    ├─ map_count 检查（sysctl_max_map_count）
                 │    └─ 计算 vm_flags：
                 │         calc_vm_prot_bits(prot, pkey)
                 │         calc_vm_flag_bits(file, flags)
                 ├─ __get_unmapped_area()               // 查找空闲地址
                 │    ├─ 文件映射→file->f_op->get_unmapped_area()
                 │    └─ 匿名映射→generic_get_unmapped_area()
                 ├─ MAP_FIXED_NOREPLACE 检查
                 ├─ CAN_DO_MLOCK / mlock_future_ok 检查
                 ├─ 文件映射检查（权限、文件类型、seal 等）
                 └─ mmap_region(file, addr, len, vm_flags, pgoff, uf)
                      ├─ 如果是 MAP_FIXED，先处理已存在的映射
                      │    └─ do_vmi_munmap()
                      ├─ 检查是否可合并到已有 VMA
                      │    └─ vma_merge()
                      ├─ 否则分配新的 VMA：
                      │    ├─ vm_area_alloc()            // 分配 VMA 结构
                      │    ├─ 文件映射：
                      │    │    ├─ file->f_op->mmap()    // 文件系统 mmap 回调
                      │    │    │    └─ ext4_file_mmap() // ex: ext4
                      │    │    │         └─ vma->vm_ops = ext4_file_vm_ops
                      │    │    └─ vma_link()            // 链接 VMA 到 mm 结构
                      │    └─ 匿名映射：
                      │         └─ vma_set_anonymous()   // 标记为匿名
                      └─ vma_set_page_prot()             // 设置页表权限
            └─ mmap_write_unlock(mm)
  └─ 如果 VM_LOCKED 或 MAP_POPULATE：
       └─ mm_populate()                                 // 触发缺页
```

## 4. 关键数据结构

### `struct vm_area_struct` (VMA)

```c
struct vm_area_struct {
    unsigned long vm_start;          /* VMA 起始地址 */
    unsigned long vm_end;            /* VMA 结束地址 */
    struct vm_area_struct *vm_next;  /* 链表中的下一个 VMA */
    struct vm_area_struct *vm_prev;  /* 链表中的上一个 VMA */
    struct rb_node vm_rb;            /* 红黑树节点 */
    struct mm_struct *vm_mm;         /* 所属的 mm_struct */
    pgprot_t vm_page_prot;           /* 页表访问权限 */
    unsigned long vm_flags;          /* VMA 标志位 */
    struct file *vm_file;            /* 映射的文件（文件映射时） */
    unsigned long vm_pgoff;          /* 文件偏移（页为单位） */
    const struct vm_operations_struct *vm_ops;  /* VMA 操作回调 */
    struct anon_vma *anon_vma;       /* 匿名映射反向映射 */
};
```

### 保护标志映射

```c
/* prot 到 vm_flags 的转换 */
pgprot_t protection_map[16];  /* 索引由 PROT_READ/WRITE/EXEC/SHARED 组合 */

/* 例：匿名映射的缺页回调 */
static const struct vm_operations_struct anon_vm_ops = {
    .fault = anon_vma_fault,      /* 匿名页缺页处理 */
};
```

## 5. 流程图

```
  用户态调用 mmap(addr, len, prot, flags, fd, offset)
         │
         ▼
  ┌──────────────────────────────────────┐
  │  ksys_mmap_pgoff()                   │
  │  ├─ 文件映射：fget(fd), 大页检查    │
  │  └─ 匿名映射：MAP_HUGETLB 处理      │
  └──────────────┬───────────────────────┘
                 ▼
  ┌──────────────────────────────────────┐
  │  vm_mmap_pgoff()                     │
  │  └─ do_mmap()                        │
  │       ├─ 参数验证与 vm_flags 计算     │
  │       ├─ get_unmapped_area()         │  查找空闲地址空间
  │       └─ mmap_region()               │  核心：创建映射
  │            │                         │
  │            ▼                         │
  │     ┌──────────────────────┐         │
  │     │ MAP_FIXED?           │         │
  │     ├─ yes → do_vmi_munmap│         │  先解除已有映射
  │     └─ no  → 跳过         │         │
  │     └──────────┬───────────┘         │
  │                ▼                     │
  │     ┌──────────────────────┐         │
  │     │ vma_merge() 尝试合并  │         │  与相邻 VMA 合并
  │     └──────────┬───────────┘         │
  │         合并失败│                     │
  │                ▼                     │
  │     ┌──────────────────────┐         │
  │     │ 分配新 VMA           │         │
  │     │ 文件映射:            │         │
  │     │  ├─ file->f_op->mmap│         │  文件系统回调
  │     │  │  └─ ext4_file_   │         │
  │     │  │     mmap()       │         │
  │     │  └─ vma_link()      │         │
  │     │ 匿名映射:            │         │
  │     │  └─ vma_set_anon()  │         │
  │     └──────────────────────┘         │
  └──────────────┬───────────────────────┘
                 ▼
  ┌──────────────────────────────────────┐
  │  是否需要填充页表?                    │
  │  VM_LOCKED 或 MAP_POPULATE?          │
  │  └─ mm_populate()                    │
  └──────────────┬───────────────────────┘
                 ▼
           返回映射地址
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | len=0、flags 无效、prot 无效、offset 未页对齐 |
| `-ENOMEM` | 无法分配地址空间、超出 VMA 数量限制 |
| `-EBADF` | 文件映射且 fd 无效 |
| `-EACCES` | 文件映射权限不匹配（如写保护文件要求 MAP_SHARED\|PROT_WRITE） |
| `-EPERM` | 文件 noexec 挂载要求 PROT_EXEC、MAP_LOCKED 无权限 |
| `-EEXIST` | MAP_FIXED_NOREPLACE 且地址已被占用 |
| `-EOVERFLOW` | 文件偏移溢出 |
| `-EAGAIN` | mlock 锁定限制 |
| `-ENODEV` | 不支持 mmap 的文件系统 |
| `-EOPNOTSUPP` | MAP_SHARED_VALIDATE 包含不支持标志 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main() {
    /* 示例1：匿名映射分配内存 */
    size_t len = 4096;
    char *anon = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) {
        perror("mmap anonymous");
        return 1;
    }
    strcpy(anon, "Hello, mmap!");
    printf("Anonymous: %s\n", anon);
    munmap(anon, len);

    /* 示例2：文件映射 */
    int fd = open("/tmp/test.txt", O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    ftruncate(fd, 4096);

    char *file_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    if (file_map == MAP_FAILED) {
        perror("mmap file");
        close(fd);
        return 1;
    }
    strcpy(file_map, "File-backed mapping!");
    munmap(file_map, 4096);
    close(fd);

    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | mmap | brk | shmat | mmap(MAP_FIXED) |
|------|------|-----|-------|-----------------|
| 用途 | 通用映射 | 堆管理 | System V 共享内存 | 固定地址映射 |
| 灵活性 | 高 | 低 | 中 | 高 |
| 文件映射 | 支持 | 不支持 | 不支持 | 支持 |
| 进程间共享 | MAP_SHARED | 不支持 | 支持 | 支持 |

## 9. 关键实现细节

1. **两阶段架构**：`mmap` 仅创建 VMA（虚拟内存区域），物理页分配推迟到**缺页异常**（page fault）时。文件映射缺页通过 `filemap_fault()` 从磁盘读取数据，匿名映射缺页通过 `do_anonymous_page()` 分配零填充页。

2. **get_unmapped_area**：查找空闲地址区间的算法有两种：`unmapped_area()`（自底向上）和 `unmapped_area_topdown()`（自顶向下），由架构和 `MAP_FIXED` 标志决定。

3. **vma_merge**：新映射尝试与相邻 VMA 合并，以减少 VMA 数量。合并条件包括：权限相同、同一文件、连续地址等。

4. **文件系统 mmap 回调**：文件映射通过 `file->f_op->mmap()` 调用文件系统特定的 mmap 处理，如 `ext4_file_mmap()` 设置 `vm_ops` 为 `ext4_file_vm_ops`，注册 `fault` 和 `map_pages` 回调。

5. **MAP_DROPPABLE**：特殊映射类型，页面内容在内存压力下可被丢弃，适用于缓存等场景。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mmap.c`
- 内核源码：`include/linux/mm.h`（VMA 相关结构定义）
- 内核源码：`include/uapi/linux/mman.h`（用户态常量定义）