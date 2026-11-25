# migrate_pages 系统调用分析

## 1. 概述

`migrate_pages` 用于将指定进程的所有页面从一个节点集迁移到另一个节点集。这是一个进程级别的批量页面迁移操作，常用于 NUMA 系统的动态负载均衡、内存热插拔时的页面迁移，以及系统管理员手动调整页面分布。

与 `move_pages`（迁移指定页面）不同，`migrate_pages` 迁移的是进程的所有可移动页面（包括匿名页、文件页等），按源节点和目标节点集进行批量迁移。

## 2. 函数原型

```c
#include <numaif.h>
#include <sys/syscall.h>

long ret = syscall(SYS_migrate_pages,
    pid_t pid,                 // 目标进程 PID（0 表示当前进程）
    unsigned long maxnode,     // 节点掩码最大节点数
    const unsigned long *old_nodes,  // 源节点掩码（迁移出这些节点）
    const unsigned long *new_nodes); // 目标节点掩码（迁移入这些节点）
```

## 3. 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `pid_t` | 目标进程 PID，0 表示当前进程 |
| `maxnode` | `unsigned long` | 节点掩码的最大节点 ID |
| `old_nodes` | `unsigned long*` | 源节点掩码（从这些节点迁移出去） |
| `new_nodes` | `unsigned long*` | 目标节点掩码（迁移到这些节点） |

## 4. 内核实现

```c
// mm/mempolicy.c
SYSCALL_DEFINE4(migrate_pages, pid_t, pid, unsigned long, maxnode,
                const unsigned long __user *, old_nodes,
                const unsigned long __user *, new_nodes)
{
    return kernel_migrate_pages(pid, maxnode, old_nodes, new_nodes);
}

static int kernel_migrate_pages(pid_t pid, unsigned long maxnode,
                                const unsigned long __user *old_nodes,
                                const unsigned long __user *new_nodes)
{
    struct mm_struct *mm = NULL;
    struct task_struct *task;
    nodemask_t task_nodes;
    int err;
    nodemask_t *old;
    nodemask_t *new;
    NODEMASK_SCRATCH(scratch);

    if (!scratch)
        return -ENOMEM;

    old = &scratch->mask1;
    new = &scratch->mask2;

    // 1. 从用户空间读取节点掩码
    err = get_nodes(old, old_nodes, maxnode);
    if (err) goto out;
    err = get_nodes(new, new_nodes, maxnode);
    if (err) goto out;

    // 2. 查找目标进程
    rcu_read_lock();
    task = pid ? find_task_by_vpid(pid) : current;
    if (!task) {
        rcu_read_unlock();
        err = -ESRCH;
        goto out;
    }
    get_task_struct(task);

    // 3. 权限检查
    if (!ptrace_may_access(task, PTRACE_MODE_READ_REALCREDS)) {
        rcu_read_unlock();
        err = -EPERM;
        goto out_put;
    }
    rcu_read_unlock();

    // 4. 验证目标节点在 cpuset 内
    task_nodes = cpuset_mems_allowed(task);
    if (!nodes_subset(*new, task_nodes) && !capable(CAP_SYS_NICE)) {
        err = -EPERM;
        goto out_put;
    }

    // 5. 安全检查（LSM）
    err = security_task_movememory(task);
    if (err) goto out_put;

    // 6. 获取 mm_struct 并执行迁移
    mm = get_task_mm(task);
    put_task_struct(task);
    if (!mm) {
        err = -EINVAL;
        goto out;
    }

    err = do_migrate_pages(mm, old, new,
            capable(CAP_SYS_NICE) ? MPOL_MF_MOVE_ALL : MPOL_MF_MOVE);

    mmput(mm);
out:
    NODEMASK_SCRATCH_FREE(scratch);
    return err;
out_put:
    put_task_struct(task);
    goto out;
}
```

## 5. 详细调用链

```
kernel_migrate_pages(pid, maxnode, old_nodes, new_nodes)   // mm/mempolicy.c
  ├─ get_nodes(old, old_nodes, maxnode)                    // 读取源节点掩码
  ├─ get_nodes(new, new_nodes, maxnode)                    // 读取目标节点掩码
  ├─ find_task_by_vpid(pid)                                // 查找目标进程
  ├─ ptrace_may_access()                                   // 权限检查
  ├─ security_task_movememory()                            // LSM 检查
  ├─ get_task_mm(task)                                     // 获取 mm_struct
  │
  └─ do_migrate_pages(mm, old, new, flags)                 // 执行迁移
       └─ do_migrate_pages(mm, from, to, flags)            // mm/mempolicy.c
            ├─ [old == new] → return 0                     // 相同，无需迁移
            │
            ├─ 遍历所有 VMA：                                // 通过 pagewalk
            │    └─ isolate_migratepages_range()           // 隔离页面
            │         └─ 检查页面是否在源节点上
            │              └─ 是 → 加入迁移列表
            │
            └─ migrate_pages(&pagelist, alloc_migration_target, ...)  // 批量迁移
                 └─ unmap_and_move()                       // 对每个页面
                      ├─ try_to_unmap()                    // 解除映射
                      ├─ alloc_migration_target()          // 在目标节点分配新页
                      └─ move_to_new_folio()               // 复制数据到新页
                           └─ migrate_folio_move()         // 更新页表
```

## 6. 核心函数详解

### 6.1 do_migrate_pages

```c
// mm/mempolicy.c
/*
 * do_migrate_pages() - 将进程页面从源节点集迁移到目标节点集
 * @mm: 目标进程的 mm_struct
 * @from: 源节点掩码
 * @to: 目标节点掩码
 * @flags: MPOL_MF_MOVE 或 MPOL_MF_MOVE_ALL
 *
 * 返回值：0 成功，负数表示错误
 */
int do_migrate_pages(struct mm_struct *mm, const nodemask_t *from,
                     const nodemask_t *to, int flags)
{
    // 1. 遍历进程的所有 VMA
    // 2. 对每个 VMA，扫描其页面
    // 3. 如果页面在 from 节点集，将其隔离并加入迁移列表
    // 4. 调用 migrate_pages() 批量迁移
    // 5. 返回迁移结果
}
```

### 6.2 migrate_pages

```c
// mm/migrate.c
/*
 * migrate_pages() - 批量迁移页面列表到目标节点
 * @from: 需要迁移的页面列表
 * @get_new_page: 分配新页面的回调函数
 * @put_new_page: 释放新页面的回调函数
 * @private: 回调参数（包含目标节点信息）
 * @mode: 迁移模式（MIGRATE_SYNC 等）
 * @reason: 迁移原因（MR_SYSCALL 等）
 * @ret_succeeded: 成功迁移的页面数
 *
 * 返回值：0 成功，负数表示错误
 */
int migrate_pages(struct list_head *from, new_folio_t get_new_page,
                  free_folio_t put_new_page, unsigned long private,
                  enum migrate_mode mode, int reason,
                  unsigned int *ret_succeeded)
{
    // 对列表中的每个 folio/页面：
    // 1. 在目标节点分配新页面
    // 2. 解除旧页面的映射
    // 3. 复制数据
    // 4. 更新页表指向新页面
    // 5. 释放旧页面
}
```

## 7. 流程图

```
migrate_pages 调用流程:
======================

用户态                         内核态
   |                              |
   | syscall(SYS_migrate_pages,   |
   |   pid, maxnode, old, new)    |
   |----------------------------->|
   |                          kernel_migrate_pages()
   |                            ├─ get_nodes(old)  // 读源掩码
   |                            ├─ get_nodes(new)  // 读目标掩码
   |                            ├─ find_task_by_vpid()
   |                            ├─ ptrace_may_access()
   |                            ├─ get_task_mm()
   |                            └─ do_migrate_pages()
   |                                 ├─ 遍历所有 VMA
   |                                 ├─ isolate 页面
   |                                 ├─ 加入迁移列表
   |                                 └─ migrate_pages()
   |                                      ├─ try_to_unmap()
   |                                      ├─ alloc_migration_target()
   │                                      ├─ move_to_new_folio()
   │                                      └─ 释放旧页面
   |                              |
   |        return 0             |
   |<-----------------------------|
   |                              |
```

## 8. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `ESRCH` | 进程不存在 | 指定的 `pid` 未找到 |
| `EPERM` | 权限不足 | 无权访问目标进程或无 `CAP_SYS_NICE` |
| `ENOMEM` | 内存不足 | 无法分配 `NODEMASK_SCRATCH` |
| `EINVAL` | 参数无效 | 节点掩码无效、`maxnode` 无效 |
| `EFAULT` | 地址错误 | 无法读取 `old_nodes` 或 `new_nodes` |

## 9. 使用示例

```c
#include <numaif.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int migrate_pages(pid_t pid, unsigned long maxnode,
                         unsigned long *old_nodes,
                         unsigned long *new_nodes) {
    return syscall(SYS_migrate_pages, pid, maxnode,
                   old_nodes, new_nodes);
}

int main() {
    unsigned long old_nodes, new_nodes;
    int ret;

    // 1. 将当前进程所有页面从节点 0 迁移到节点 1
    old_nodes = 1 << 0;   // 源：节点 0
    new_nodes = 1 << 1;   // 目标：节点 1
    ret = migrate_pages(0, 2, &old_nodes, &new_nodes);
    if (ret == 0) {
        printf("成功将页面从节点 0 迁移到节点 1\n");
    } else if (ret < 0) {
        printf("迁移失败: %s\n", strerror(-ret));
    } else {
        printf("迁移完成，%d 个页面未能迁移\n", ret);
    }

    // 2. 将进程 PID 1234 的页面从节点 0,1 迁移到节点 2,3
    old_nodes = (1 << 0) | (1 << 1);  // 节点 0,1
    new_nodes = (1 << 2) | (1 << 3);  // 节点 2,3
    ret = migrate_pages(1234, 4, &old_nodes, &new_nodes);
    if (ret == -EPERM) {
        printf("无权限迁移 PID 1234 的页面\n");
    } else if (ret == -ESRCH) {
        printf("PID 1234 不存在\n");
    }

    // 3. 将页面从所有节点集中到节点 0
    old_nodes = ~0UL;  // 所有节点
    new_nodes = 1 << 0;
    ret = migrate_pages(0, sizeof(unsigned long) * 8,
                        &old_nodes, &new_nodes);
    if (ret == 0) {
        printf("成功将所有页面迁移到节点 0\n");
    }

    return 0;
}
```

## 10. 关键要点

1. **进程级别**：`migrate_pages` 操作整个进程的所有页面，而非指定地址范围
2. **权限要求**：迁移其他进程需要 `ptrace` 权限，使用 `MPOL_MF_MOVE_ALL` 需要 `CAP_SYS_NICE`
3. **cpuset 限制**：目标节点必须在目标进程的 cpuset 允许节点内
4. **非阻塞**：`do_migrate_pages` 使用 `MIGRATE_SYNC` 模式，会等待迁移完成
5. **返回值**：返回无法迁移的页面数（非负值）或错误码（负值）
6. **页面状态**：被锁定（mlock）的页面、内核栈页面等不可迁移

## 11. 源码位置

| 文件 | 说明 |
|------|------|
| [mm/mempolicy.c](file:///home/louis/code/linux/mm/mempolicy.c) | `kernel_migrate_pages`、`do_migrate_pages`、`migrate_to_node` 实现 |
| [mm/migrate.c](file:///home/louis/code/linux/mm/migrate.c) | `migrate_pages`、`unmap_and_move`、`migrate_folio_move` 实现 |
| [include/linux/mempolicy.h](file:///home/louis/code/linux/include/linux/mempolicy.h) | `mempolicy` 相关声明 |
| [include/linux/migrate.h](file:///home/louis/code/linux/include/linux/migrate.h) | 迁移相关函数声明 |