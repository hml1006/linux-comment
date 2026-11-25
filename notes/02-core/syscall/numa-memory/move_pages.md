# move_pages 系统调用分析

## 1. 概述

`move_pages` 用于将指定进程的一组特定页面迁移到指定的 NUMA 节点。与 `migrate_pages`（按节点掩码批量迁移所有页面）不同，`move_pages` 允许精确控制每个页面的目标节点，每个页面可以迁移到不同的节点。

`move_pages` 有两种模式：
- **迁移模式**：提供 `nodes` 数组，将页面迁移到指定节点
- **统计模式**：`nodes` 为 NULL，仅查询页面所在节点

## 2. 函数原型

```c
#include <numaif.h>
#include <sys/syscall.h>

long ret = syscall(SYS_move_pages,
    pid_t pid,                 // 目标进程 PID（0 表示当前进程）
    unsigned long nr_pages,    // 页面数量
    const void **pages,        // 页面地址数组
    const int *nodes,          // 目标节点数组（NULL 表示仅查询）
    int *status,               // 输出：每个页面的状态
    int flags);                // 标志
```

## 3. 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID，0 表示当前进程 |
| `nr_pages` | `unsigned long` | 要处理的页面数量 |
| `pages` | `const void**` | 用户空间页面地址数组 |
| `nodes` | `const int*` | 目标节点数组（每个元素为目标节点 ID，-1 表示不迁移） |
| `status` | `int*` | 输出数组，每个页面的状态码 |
| `flags` | `int` | 标志（`MPOL_MF_MOVE` 或 `MPOL_MF_MOVE_ALL`） |

### 3.1 status 输出值

| 状态值 | 含义 |
|--------|------|
| 0 到 `MAX_NUMNODES-1` | 页面所在/迁移到的 NUMA 节点 |
| `-EACCES` | 页面不可迁移 |
| `-EBUSY` | 页面正忙，无法迁移 |
| `-EFAULT` | 地址无效 |
| `-EIO` | I/O 错误 |
| `-EINVAL` | 参数无效 |
| `-ENOENT` | 页面不存在 |
| `-ENOMEM` | 内存不足 |
| `-EPERM` | 权限不足 |

## 4. 内核实现

```c
// mm/migrate.c
SYSCALL_DEFINE6(move_pages, pid_t, pid, unsigned long, nr_pages,
                const void __user * __user *, pages,
                const int __user *, nodes,
                int __user *, status, int, flags)
{
    return kernel_move_pages(pid, nr_pages, pages, nodes, status, flags);
}

static int kernel_move_pages(pid_t pid, unsigned long nr_pages,
                             const void __user * __user *pages,
                             const int __user *nodes,
                             int __user *status, int flags)
{
    struct mm_struct *mm;
    int err;
    nodemask_t task_nodes;

    // 检查标志
    if (flags & ~(MPOL_MF_MOVE|MPOL_MF_MOVE_ALL))
        return -EINVAL;
    if ((flags & MPOL_MF_MOVE_ALL) && !capable(CAP_SYS_NICE))
        return -EPERM;

    // 查找目标进程的 mm_struct
    mm = find_mm_struct(pid, &task_nodes);
    if (IS_ERR(mm))
        return PTR_ERR(mm);

    // 迁移模式 或 统计模式
    if (nodes)
        err = do_pages_move(mm, task_nodes, nr_pages, pages,
                            nodes, status, flags);
    else
        err = do_pages_stat(mm, nr_pages, pages, status);

    mmput(mm);
    return err;
}
```

## 5. 详细调用链

### 5.1 迁移模式

```
kernel_move_pages(pid, nr_pages, pages, nodes, status, flags) // mm/migrate.c
  └─ do_pages_move(mm, task_nodes, nr_pages, pages, nodes, status, flags)
       ├─ lru_cache_disable()                               // 禁用 LRU
       │
       ├─ 循环处理每个页面:
       │    ├─ get_user(p, pages[i])                        // 读页面地址
       │    ├─ get_user(node, nodes[i])                     // 读目标节点
       │    ├─ [node < 0 || node >= MAX_NUMNODES] → -ENODEV
       │    ├─ [node 不在 task_nodes 内] → -EACCES
       │    └─ add_folio_for_migration(mm, p, node, ...)     // 隔离页面
       │         ├─ get_user_pages_fast()                   // 获取页面
       │         └─ isolate_lru_folio()                     // 从 LRU 隔离
       │
       ├─ 同节点页面合并批量迁移:
       │    └─ move_pages_and_store_status(node, ...)        // 迁移到同节点
       │         └─ migrate_pages(&pagelist, ...)            // 批量迁移
       │
       └─ lru_cache_enable()                                // 恢复 LRU
```

### 5.2 统计模式

```
do_pages_stat(mm, nr_pages, pages, status)                 // mm/migrate.c
  ├─ 循环处理每个页面:
  │    ├─ get_user(p, pages[i])                            // 读页面地址
  │    ├─ get_user_pages_fast(p, 1, 0, &page)              // 获取页面
  │    └─ page_to_nid(page)                                 // 查询节点
  │         └─ status[i] = node_id                          // 写入状态
  └─ return 0
```

## 6. 核心函数详解

### 6.1 add_folio_for_migration

```c
// mm/migrate.c
/*
 * add_folio_for_migration() - 隔离一个页面准备迁移
 * 通过 GUP 获取页面引用，然后从 LRU 列表中隔离
 * 页面被加入 pagelist，等待批量迁移
 */
static int add_folio_for_migration(struct mm_struct *mm,
                                   const void __user *p,
                                   int target_node,
                                   struct list_head *pagelist,
                                   int flags)
{
    struct folio *folio;
    int err;

    // 通过 GUP 获取页面
    err = get_user_pages_fast(addr, 1, 0, &folio_page(folio, 0));
    if (err <= 0)
        return -EFAULT;

    // 从 LRU 隔离
    if (!isolate_lru_folio(folio)) {
        put_page(folio_page(folio, 0));
        return -EBUSY;
    }

    // 加入迁移列表
    list_add_tail(&folio->lru, pagelist);
    return 0;
}
```

### 6.2 do_pages_move

```c
// mm/migrate.c
/*
 * do_pages_move() - 批量迁移页面到指定节点
 * 将目标节点相同的页面分组，批量迁移以提高效率
 *
 * @mm: 目标进程的 mm_struct
 * @task_nodes: 目标进程的 cpuset 节点掩码
 * @nr_pages: 页面数量
 * @pages: 页面地址数组
 * @nodes: 目标节点数组
 * @status: 状态输出数组
 * @flags: 迁移标志
 */
static int do_pages_move(struct mm_struct *mm, nodemask_t task_nodes,
                         unsigned long nr_pages,
                         const void __user * __user *pages,
                         const int __user *nodes,
                         int __user *status, int flags)
{
    // 将同一目标节点的页面聚合成批
    // 每批调用 move_pages_and_store_status() 进行迁移
    // 通过 migrate_pages() 批量迁移
}
```

## 7. 流程图

```
move_pages 调用流程:
====================

用户态                         内核态
   |                              |
   | syscall(SYS_move_pages,      |
   |   pid, nr_pages, pages,      |
   |   nodes, status, flags)      |
   |----------------------------->|
   |                          kernel_move_pages()
   |                            ├─ find_mm_struct()
   |                            │
   |                            ├─ [nodes != NULL] 迁移模式:
   |                            │    └─ do_pages_move()
   |                            │         ├─ lru_cache_disable()
   |                            │         ├─ 循环隔离页面
   |                            │         │    └─ add_folio_for_migration()
   |                            │         ├─ move_pages_and_store_status()
   |                            │         │    └─ migrate_pages()
   |                            │         └─ lru_cache_enable()
   |                            │
   |                            └─ [nodes == NULL] 统计模式:
   │                                 └─ do_pages_stat()
   │                                      └─ page_to_nid() 每个页面
   |                              |
   |        return 0, status[]   |
   |<-----------------------------|
   |                              |
```

## 8. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 参数无效 | `flags` 无效、`nr_pages` 无效 |
| `EPERM` | 权限不足 | 使用 `MPOL_MF_MOVE_ALL` 无 `CAP_SYS_NICE` |
| `ESRCH` | 进程不存在 | 找不到指定 PID 的进程 |
| `ENODEV` | 节点无效 | 目标节点号超出范围或节点无内存 |
| `EACCES` | 节点不允许 | 目标节点不在进程的 cpuset 内 |
| `EFAULT` | 地址错误 | 页面地址数组或状态数组不可访问 |

## 9. 使用示例

```c
#include <numaif.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int move_pages(pid_t pid, unsigned long nr_pages,
                      void **pages, int *nodes,
                      int *status, int flags) {
    return syscall(SYS_move_pages, pid, nr_pages,
                   pages, nodes, status, flags);
}

#define PAGE_SIZE 4096

int main() {
    void *pages[4];
    int nodes[4];
    int status[4];
    int ret;
    int i;

    // 分配 4 页内存
    for (i = 0; i < 4; i++) {
        pages[i] = malloc(PAGE_SIZE);
        if (pages[i]) {
            // 触发缺页分配
            *(volatile char *)pages[i] = 0;
        }
    }

    // 1. 统计模式：查询每页所在节点
    ret = move_pages(0, 4, pages, NULL, status, 0);
    if (ret == 0) {
        printf("页面节点分布:\n");
        for (i = 0; i < 4; i++) {
            printf("  pages[%d] (%p) → 节点 %d\n",
                   i, pages[i], status[i]);
        }
    }

    // 2. 迁移模式：将页面迁移到指定节点
    nodes[0] = 0;   // 第 0 页 → 节点 0
    nodes[1] = 1;   // 第 1 页 → 节点 1
    nodes[2] = 0;   // 第 2 页 → 节点 0
    nodes[3] = 1;   // 第 3 页 → 节点 1

    ret = move_pages(0, 4, pages, nodes, status,
                     MPOL_MF_MOVE);
    if (ret == 0) {
        printf("迁移结果:\n");
        for (i = 0; i < 4; i++) {
            printf("  pages[%d] → 节点 %d (状态: %d)\n",
                   i, nodes[i], status[i]);
        }
    } else {
        printf("迁移失败: %s\n", strerror(-ret));
    }

    // 3. 验证迁移结果
    ret = move_pages(0, 4, pages, NULL, status, 0);
    if (ret == 0) {
        printf("验证迁移后节点分布:\n");
        for (i = 0; i < 4; i++) {
            printf("  pages[%d] → 节点 %d\n",
                   i, status[i]);
        }
    }

    for (i = 0; i < 4; i++) {
        free(pages[i]);
    }
    return 0;
}
```

## 10. 关键要点

1. **逐页控制**：每个页面可以指定不同的目标节点，比 `migrate_pages` 更精细
2. **批量迁移**：同一目标节点的页面会合并批量迁移，提高效率
3. **统计模式**：`nodes == NULL` 时不迁移，仅查询页面位置
4. **LRU 管理**：迁移期间禁用 LRU 缓存，防止页面被回收
5. **GUP 引用**：通过 `get_user_pages_fast` 获取页面引用，确保页面在迁移期间不被释放
6. **返回值**：返回值为 0 表示成功，每个页面的具体迁移结果通过 `status` 数组返回

## 11. 源码位置

| 文件 | 说明 |
|------|------|
| [mm/migrate.c](file:///home/louis/code/linux/mm/migrate.c) | `kernel_move_pages`、`do_pages_move`、`do_pages_stat`、`add_folio_for_migration` 实现 |
| [mm/mempolicy.c](file:///home/louis/code/linux/mm/mempolicy.c) | `find_mm_struct` 辅助函数 |
| [include/linux/migrate.h](file:///home/louis/code/linux/include/linux/migrate.h) | 迁移相关函数声明 |