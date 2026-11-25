# mbind 系统调用分析

## 1. 概述

`mbind` 用于设置指定虚拟内存区域（VMA）的内存策略。与 `set_mempolicy`（设置进程默认策略）不同，`mbind` 可以精确控制特定地址范围的 NUMA 内存分配行为，并可以选择性地迁移该区域内已有的页面。

`mbind` 是 NUMA 内存管理中最精细化的策略控制接口，常用于数据库、中间件等对内存访问延迟敏感的应用。

## 2. 函数原型

```c
#include <numaif.h>
#include <sys/syscall.h>

long ret = syscall(SYS_mbind,
    unsigned long start,        // 起始地址
    unsigned long len,          // 长度
    int mode,                   // 策略模式及标志
    unsigned long *nmask,       // 节点掩码
    unsigned long maxnode,      // 最大节点数
    unsigned int flags);        // 迁移标志
```

## 3. 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `start` | `unsigned long` | 起始虚拟地址（页对齐） |
| `len` | `unsigned long` | 内存区域长度（向上页对齐） |
| `mode` | `int` | 策略模式（`MPOL_*`）和模式标志（`MPOL_F_*`） |
| `nmask` | `unsigned long*` | 节点掩码 |
| `maxnode` | `unsigned long` | 最大节点 ID |
| `flags` | `unsigned int` | 迁移标志 |

### 3.1 迁移标志

```c
// include/uapi/linux/mempolicy.h
#define MPOL_MF_STRICT      0x01  // 严格模式：出错时报告
#define MPOL_MF_MOVE        0x02  // 迁移现有可移动页面
#define MPOL_MF_MOVE_ALL    0x04  // 迁移所有页面（需 CAP_SYS_NICE）
```

## 4. 内核实现

```c
// mm/mempolicy.c
SYSCALL_DEFINE6(mbind, unsigned long, start, unsigned long, len,
                unsigned long, mode, const unsigned long __user *, nmask,
                unsigned long, maxnode, unsigned int, flags)
{
    return kernel_mbind(start, len, mode, nmask, maxnode, flags);
}

static long kernel_mbind(unsigned long start, unsigned long len,
                         unsigned long mode, const unsigned long __user *nmask,
                         unsigned long maxnode, unsigned int flags)
{
    unsigned short mode_flags;
    nodemask_t nodes;
    int lmode = mode;
    int err;

    start = untagged_addr(start);
    err = sanitize_mpol_flags(&lmode, &mode_flags);
    if (err)
        return err;

    err = get_nodes(&nodes, nmask, maxnode);
    if (err)
        return err;

    return do_mbind(start, len, lmode, mode_flags, &nodes, flags);
}
```

## 5. 详细调用链

```
kernel_mbind(start, len, mode, nmask, maxnode, flags)      // mm/mempolicy.c
  └─ do_mbind(start, len, mode, mode_flags, &nodes, flags)  // 核心实现
       ├─ [flags 无效] → return -EINVAL
       ├─ [MPOL_MF_MOVE_ALL && !CAP_SYS_NICE] → return -EPERM
       ├─ [start 未页对齐] → return -EINVAL
       │
       ├─ mpol_new(mode, mode_flags, nmask)                 // 创建新策略
       │    └─ [MPOL_DEFAULT] → new = NULL
       │
       ├─ [MPOL_MF_MOVE | MPOL_MF_MOVE_ALL]
       │    └─ lru_cache_disable()                          // 禁用 LRU 缓存
       │
       ├─ mpol_set_nodemask(new, nmask, scratch)            // 设置节点掩码
       │
       ├─ queue_pages_range(mm, start, end, nmask,          // 收集需要迁移的页面
       │                    flags | MPOL_MF_INVERT | MPOL_MF_WRLOCK,
       │                    &pagelist)
       │
       ├─ mbind_range(&vmi, vma, &prev, start, end, new)    // 设置 VMA 策略
       │    └─ vma_set_policy(vma, new)                     // 应用到 VMA
       │         └─ vma->vm_policy = new
       │
       ├─ [有页面需要迁移]
       │    └─ migrate_pages(&pagelist, alloc_migration_target, ...)
       │         └─ unmap_and_move()                         // 逐页解除映射并迁移
       │
       ├─ [MPOL_MF_MOVE | MPOL_MF_MOVE_ALL]
       │    └─ lru_cache_enable()
       │
       └─ mpol_put(new) 或 mpol_put(old)                    // 释放引用
```

## 6. 核心函数详解

### 6.1 do_mbind

```c
// mm/mempolicy.c
static long do_mbind(unsigned long start, unsigned long len,
                     unsigned short mode, unsigned short mode_flags,
                     nodemask_t *nmask, unsigned long flags)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma, *prev;
    struct vma_iterator vmi;
    struct migration_mpol mmpol;
    struct mempolicy *new;
    unsigned long end;
    long err;
    long nr_failed;
    LIST_HEAD(pagelist);

    // 参数验证
    if (flags & ~(unsigned long)MPOL_MF_VALID)
        return -EINVAL;
    if ((flags & MPOL_MF_MOVE_ALL) && !capable(CAP_SYS_NICE))
        return -EPERM;
    if (start & ~PAGE_MASK)
        return -EINVAL;

    // 创建策略
    new = mpol_new(mode, mode_flags, nmask);
    if (IS_ERR(new))
        return PTR_ERR(new);

    // 1. 收集需要迁移的页面
    nr_failed = queue_pages_range(mm, start, end, nmask,
            flags | MPOL_MF_INVERT | MPOL_MF_WRLOCK, &pagelist);

    // 2. 遍历所有 VMA，设置策略
    vma_iter_init(&vmi, mm, start);
    prev = vma_prev(&vmi);
    for_each_vma_range(vmi, vma, end) {
        err = mbind_range(&vmi, vma, &prev, start, end, new);
        if (err)
            break;
    }

    // 3. 迁移页面到目标节点
    if (!err && !list_empty(&pagelist)) {
        // ... 迁移逻辑 ...
        err = migrate_pages(&pagelist, alloc_migration_target,
                NULL, (unsigned long)&mtc, MIGRATE_SYNC,
                MR_SYSCALL, NULL);
    }

    return err;
}
```

### 6.2 queue_pages_range

```c
// mm/mempolicy.c
/*
 * queue_pages_range() - 扫描指定地址范围内的页面
 * 将需要迁移的页面加入 pagelist
 *
 * 遍历 VMA 树，对每个页面检查是否在目标节点上
 * 如果不在目标节点，将其加入迁移列表
 */
static long queue_pages_range(struct mm_struct *mm,
                              unsigned long start, unsigned long end,
                              nodemask_t *nodes, unsigned long flags,
                              struct list_head *pagelist)
{
    // 使用 pagewalk 框架遍历页面
    // 对每个页面调用 queue_pages_pte_range() 检查
    // 条件符合的页面加入 pagelist
}
```

## 7. 流程图

```
mbind 调用流程:
=============

用户态                         内核态
   |                              |
   | syscall(SYS_mbind,           |
   |   start, len, MPOL_BIND,     |
   |   nmask, maxnode,            |
   |   MPOL_MF_MOVE)              |
   |----------------------------->|
   |                          kernel_mbind()
   |                            ├─ sanitize_mpol_flags()
   |                            ├─ get_nodes()  // 读掩码
   |                            └─ do_mbind()
   |                                 ├─ mpol_new()  // 创建策略
   |                                 ├─ mpol_set_nodemask()
   |                                 ├─ queue_pages_range()
   |                                 │    └─ 收集需迁移的页面
   |                                 ├─ mbind_range()
   |                                 │    └─ 设置 VMA 策略
   |                                 ├─ migrate_pages()
   |                                 │    └─ 迁移页面到目标节点
   |                                 └─ 清理
   |                              |
   |        return 0             |
   |<-----------------------------|
   |                              |
```

## 8. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 参数无效 | `start` 未页对齐、`flags` 无效、`mode` 无效 |
| `EPERM` | 权限不足 | 使用 `MPOL_MF_MOVE_ALL` 但无 `CAP_SYS_NICE` |
| `ENOMEM` | 内存不足 | 无法分配策略或迁移页面 |
| `EFAULT` | 地址错误 | `start` 到 `start+len` 范围无效 |
| `EIO` | I/O 错误 | 迁移页面时发生 I/O 错误 |
| `EAGAIN` | 部分失败 | `MPOL_MF_STRICT` 时部分页面迁移失败 |

## 9. 使用示例

```c
#include <numaif.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int mbind(unsigned long start, unsigned long len, int mode,
                 unsigned long *nmask, unsigned long maxnode,
                 unsigned int flags) {
    return syscall(SYS_mbind, start, len, mode, nmask,
                   maxnode, flags);
}

#define PAGE_SIZE 4096

int main() {
    unsigned long nmask;
    void *buf;
    int ret;

    // 1. 分配内存
    buf = malloc(1024 * PAGE_SIZE);  // 4MB
    if (!buf) {
        perror("malloc");
        return 1;
    }

    // 确保页面已分配（触发性缺页）
    memset(buf, 0, 1024 * PAGE_SIZE);

    // 2. 绑定到节点 0（不迁移已有页面）
    nmask = 1 << 0;
    ret = mbind((unsigned long)buf, 1024 * PAGE_SIZE,
                MPOL_BIND, &nmask, 1, 0);
    if (ret == 0) {
        printf("已绑定内存区域到节点 0（后续分配）\n");
    }

    // 3. 绑定到节点 1 并迁移现有页面
    nmask = 1 << 1;
    ret = mbind((unsigned long)buf, 1024 * PAGE_SIZE,
                MPOL_BIND, &nmask, 1,
                MPOL_MF_MOVE | MPOL_MF_STRICT);
    if (ret == 0) {
        printf("已迁移内存到节点 1\n");
    } else if (ret == -EIO) {
        printf("部分页面迁移失败\n");
    }

    // 4. 在节点 0 和 1 间交错分配
    nmask = (1 << 0) | (1 << 1);
    ret = mbind((unsigned long)buf, 1024 * PAGE_SIZE,
                MPOL_INTERLEAVE, &nmask, 2, 0);
    if (ret == 0) {
        printf("已设置交错分配（节点 0 和 1）\n");
    }

    free(buf);
    return 0;
}
```

## 10. 关键要点

1. **VMA 策略 vs 进程策略**：`mbind` 设置的 VMA 策略优先级高于 `set_mempolicy` 设置的进程默认策略
2. **页面迁移**：`MPOL_MF_MOVE` 迁移可移动页面，`MPOL_MF_MOVE_ALL` 迁移所有页面（包括可能不可移动的页面）
3. **LRU 缓存管理**：迁移页面时禁用 LRU 缓存，避免迁移过程中页面被 LRU 回收
4. **pagewalk 框架**：`queue_pages_range` 使用内核 pagewalk 框架遍历页面表，检查每个页面的节点
5. **原子性**：策略设置和页面迁移不是原子的——如果 `mbind_range` 失败，已迁移的页面不会回滚
6. **MPOL_DEFAULT**：将 VMA 策略恢复为默认，等同于删除显式策略

## 11. 源码位置

| 文件 | 说明 |
|------|------|
| [mm/mempolicy.c](file:///home/louis/code/linux/mm/mempolicy.c) | `do_mbind`、`kernel_mbind`、`mbind_range`、`queue_pages_range` 实现 |
| [mm/migrate.c](file:///home/louis/code/linux/mm/migrate.c) | `migrate_pages`、`unmap_and_move` 页面迁移实现 |
| [include/linux/mempolicy.h](file:///home/louis/code/linux/include/linux/mempolicy.h) | `struct mempolicy`、`mpol_new`、`mpol_set_nodemask` 声明 |
| [include/uapi/linux/mempolicy.h](file:///home/louis/code/linux/include/uapi/linux/mempolicy.h) | `MPOL_*`、`MPOL_MF_*` 宏定义 |