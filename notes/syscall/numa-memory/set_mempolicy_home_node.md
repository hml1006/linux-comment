# set_mempolicy_home_node 系统调用分析

## 1. 概述

`set_mempolicy_home_node` 是 Linux 5.17+ 引入的系统调用，用于为指定虚拟地址范围设置"home 节点"。home 节点是 NUMA 策略的扩展概念，用于指定内存页面的"归属"节点。

当策略为 `MPOL_BIND` 或 `MPOL_PREFERRED_MANY` 时，home 节点提供以下语义：
- 标记页面的首选物理位置
- 影响 NUMA 平衡（NUMA Balancing）的决策
- 页面迁移时作为参考节点

这个系统调用通常与 cgroup 级别的内存策略配合使用，允许在容器/虚拟化场景中精确控制内存放置。

## 2. 函数原型

```c
#include <numaif.h>
#include <sys/syscall.h>

long ret = syscall(SYS_set_mempolicy_home_node,
    unsigned long start,        // 起始地址
    unsigned long len,          // 长度
    unsigned long home_node,    // home 节点 ID
    unsigned long flags);       // 保留，必须为 0
```

## 3. 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `start` | `unsigned long` | 起始虚拟地址（页对齐） |
| `len` | `unsigned long` | 内存区域长度 |
| `home_node` | `unsigned long` | home 节点 ID，必须在线 |
| `flags` | `unsigned long` | 保留，必须为 0 |

## 4. 内核实现

```c
// mm/mempolicy.c
SYSCALL_DEFINE4(set_mempolicy_home_node, unsigned long, start, unsigned long, len,
                unsigned long, home_node, unsigned long, flags)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma, *prev;
    struct mempolicy *new, *old;
    unsigned long end;
    int err = -ENOENT;
    VMA_ITERATOR(vmi, mm, start);

    start = untagged_addr(start);
    if (start & ~PAGE_MASK)
        return -EINVAL;

    // flags 保留，必须为 0
    if (flags != 0)
        return -EINVAL;

    // 检查 home_node 在线
    if (home_node >= MAX_NUMNODES || !node_online(home_node))
        return -EINVAL;

    len = PAGE_ALIGN(len);
    end = start + len;
    if (end < start)
        return -EINVAL;
    if (end == start)
        return 0;

    mmap_write_lock(mm);
    prev = vma_prev(&vmi);
    for_each_vma_range(vmi, vma, end) {
        old = vma_policy(vma);

        // 跳过没有显式策略的 VMA
        if (!old) {
            prev = vma;
            continue;
        }

        // 仅支持 MPOL_BIND 和 MPOL_PREFERRED_MANY
        if (old->mode != MPOL_BIND &&
            old->mode != MPOL_PREFERRED_MANY) {
            err = -EOPNOTSUPP;
            break;
        }

        // 复制旧策略并设置 home_node
        new = mpol_dup(old);
        if (IS_ERR(new)) {
            err = PTR_ERR(new);
            break;
        }

        vma_start_write(vma);
        new->home_node = home_node;
        err = mbind_range(&vmi, vma, &prev, start, end, new);
        mpol_put(new);
        if (err)
            break;
    }
    mmap_write_unlock(mm);
    return err;
}
```

## 5. 详细调用链

```
sys_set_mempolicy_home_node(start, len, home_node, flags)  // mm/mempolicy.c
  ├─ [start 未页对齐] → return -EINVAL
  ├─ [flags != 0] → return -EINVAL
  ├─ [home_node 不在线或超出范围] → return -EINVAL
  ├─ [end < start || end == start] → return -EINVAL / 0
  │
  ├─ mmap_write_lock(mm)                                 // 写锁地址空间
  │
  ├─ VMA_ITERATOR(vmi, mm, start)                        // 初始化 VMA 迭代器
  │
  └─ for_each_vma_range(vmi, vma, end):                  // 遍历范围内的 VMA
       ├─ vma_policy(vma)                                 // 获取当前策略
       ├─ [无策略] → continue                             // 跳过
       │
       ├─ [策略不是 MPOL_BIND 和 MPOL_PREFERRED_MANY]
       │    └─ return -EOPNOTSUPP
       │
       ├─ mpol_dup(old)                                   // 复制策略
       ├─ new->home_node = home_node                      // 设置 home 节点
       └─ mbind_range(&vmi, vma, &prev, start, end, new)  // 应用新策略
            └─ vma_set_policy(vma, new)                   // 替换 VMA 策略
  │
  └─ mmap_write_unlock(mm)                               // 解锁
```

## 6. 核心数据结构

```c
// include/linux/mempolicy.h
struct mempolicy {
    atomic_t refcnt;                         // 引用计数
    unsigned short mode;                     // 策略模式
    unsigned short flags;                    // 策略标志
    nodemask_t nodes;                        // 节点掩码
    union {
        struct {                             // MPOL_PREFERRED
            unsigned short preferred_node;
            unsigned short user_nodemask_len;
        } prefs;
        struct {                             // MPOL_INTERLEAVE
            spinlock_t lock;
            struct rb_root_cached rb_root;
            int interval;
        } interleave;
    };
    struct rcu_head rcu;                     // RCU 销毁
    unsigned short home_node;                // home 节点（NUMA 平衡参考节点）
};
```

## 7. 流程图

```
set_mempolicy_home_node 调用流程:
===============================

用户态                         内核态
   |                              |
   | syscall(                     |
   |   SYS_set_mempolicy_home_node,|
   |   start, len, node, 0)      |
   |----------------------------->|
   |                          SYSCALL_DEFINE4
   |                            ├─ 参数验证
   |                            ├─ mmap_write_lock(mm)
   |                            ├─ 遍历 VMA:
   |                            │    ├─ vma_policy()
   |                            │    ├─ [无策略] → continue
   |                            │    ├─ [非 BIND/PREFERRED_MANY]
   |                            │    │    └─ -EOPNOTSUPP
   |                            │    ├─ mpol_dup()
   |                            │    ├─ 设置 home_node
   |                            │    └─ mbind_range()
   |                            └─ mmap_write_unlock(mm)
   |                              |
   |        return 0             |
   |<-----------------------------|
   |                              |
```

## 8. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 参数无效 | `start` 未页对齐、`flags != 0`、`home_node` 不在线或越界 |
| `EOPNOTSUPP` | 不支持的策略 | VMA 策略不是 `MPOL_BIND` 或 `MPOL_PREFERRED_MANY` |
| `ENOENT` | 未找到 | 地址范围内没有设置显式策略的 VMA |
| `ENOMEM` | 内存不足 | `mpol_dup` 分配失败 |

## 9. 使用示例

```c
#include <numaif.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int set_mempolicy_home_node(unsigned long start,
                                   unsigned long len,
                                   unsigned long home_node,
                                   unsigned long flags) {
    return syscall(SYS_set_mempolicy_home_node, start, len,
                   home_node, flags);
}

#define PAGE_SIZE 4096

int main() {
    unsigned long nmask;
    void *buf;
    int ret;

    // 1. 分配内存
    buf = malloc(1024 * PAGE_SIZE);
    if (!buf) {
        perror("malloc");
        return 1;
    }
    memset(buf, 0, 1024 * PAGE_SIZE);

    // 2. 先绑定到特定节点（home_node 需要 BIND 或 PREFERRED_MANY 策略）
    nmask = 1 << 0;
    ret = syscall(SYS_mbind, (unsigned long)buf, 1024 * PAGE_SIZE,
                  MPOL_BIND, &nmask, 1, MPOL_MF_MOVE);
    if (ret) {
        printf("mbind 失败: %s\n", strerror(errno));
        free(buf);
        return 1;
    }

    // 3. 设置 home 节点为节点 0
    ret = set_mempolicy_home_node((unsigned long)buf,
                                  1024 * PAGE_SIZE,
                                  0,  // home_node = 0
                                  0); // flags = 0
    if (ret == 0) {
        printf("成功设置 home 节点为节点 0\n");
    } else if (ret == -EOPNOTSUPP) {
        printf("当前策略不支持设置 home 节点\n");
    } else if (ret == -EINVAL) {
        printf("节点 0 不在线或参数无效\n");
    }

    // 4. 尝试设置到不支持的策略上（MPOL_INTERLEAVE）
    nmask = 0x0F;
    ret = syscall(SYS_mbind, (unsigned long)buf, 1024 * PAGE_SIZE,
                  MPOL_INTERLEAVE, &nmask, 4, MPOL_MF_MOVE);
    if (ret == 0) {
        ret = set_mempolicy_home_node((unsigned long)buf,
                                      1024 * PAGE_SIZE,
                                      0, 0);
        if (ret == -EOPNOTSUPP) {
            printf("MPOL_INTERLEAVE 不支持设置 home 节点\n");
        }
    }

    free(buf);
    return 0;
}
```

## 10. 关键要点

1. **策略限制**：home 节点仅适用于 `MPOL_BIND` 和 `MPOL_PREFERRED_MANY` 策略，其他策略返回 `-EOPNOTSUPP`
2. **节点在线性**：`home_node` 必须在 `node_online()` 中，即该节点必须在系统中物理存在
3. **VMA 级别**：home 节点是 VMA 级别的属性，每个 VMA 可以有独立的 home 节点
4. **NUMA 平衡**：home 节点影响内核的 NUMA 平衡算法，页面倾向于迁移到其 home 节点
5. **cgroup 集成**：与 cgroup 的 `cpuset.mems` 配合使用，用于容器环境的内存放置策略
6. **flags 保留**：`flags` 参数当前必须为 0，为未来扩展预留

## 11. 源码位置

| 文件 | 说明 |
|------|------|
| [mm/mempolicy.c](file:///home/louis/code/linux/mm/mempolicy.c) | `sys_set_mempolicy_home_node` 实现 |
| [include/linux/mempolicy.h](file:///home/louis/code/linux/include/linux/mempolicy.h) | `struct mempolicy` 中的 `home_node` 字段 |
| [include/uapi/linux/mempolicy.h](file:///home/louis/code/linux/include/uapi/linux/mempolicy.h) | `MPOL_*` 宏定义 |