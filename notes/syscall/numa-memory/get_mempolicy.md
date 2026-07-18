# get_mempolicy 系统调用分析

## 1. 概述

`get_mempolicy` 用于获取调用进程或指定地址的内存策略（mempolicy）。NUMA（Non-Uniform Memory Access）架构中，内存策略决定了进程分配内存时优先选择的 NUMA 节点。

该系统调用可以查询：
- 进程的默认内存策略
- 指定虚拟地址的 VMA 策略
- 指定地址所在的 NUMA 节点
- 进程的 cpuset 允许节点掩码

## 2. 函数原型

```c
#include <numaif.h>
#include <sys/syscall.h>

long ret = syscall(SYS_get_mempolicy,
    int *policy,                // 输出：策略模式
    unsigned long *nmask,       // 输出：节点掩码
    unsigned long maxnode,      // nmask 的最大节点数
    unsigned long addr,         // 查询地址
    unsigned long flags);       // 标志位
```

## 3. 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `policy` | `int*` | 输出参数，接收策略模式（`MPOL_*`） |
| `nmask` | `unsigned long*` | 输出参数，接收节点掩码 |
| `maxnode` | `unsigned long` | `nmask` 的最大节点 ID |
| `addr` | `unsigned long` | 查询地址（与 `MPOL_F_ADDR` 配合使用） |
| `flags` | `unsigned long` | 标志位 |

### 3.1 标志位

```c
// include/uapi/linux/mempolicy.h
#define MPOL_F_NODE         0x01  // 返回 addr 所在 NUMA 节点 ID
#define MPOL_F_ADDR         0x02  // 查询指定地址的策略
#define MPOL_F_MEMS_ALLOWED 0x04  // 返回 cpuset 允许节点掩码
```

### 3.2 策略模式

```c
// include/uapi/linux/mempolicy.h
#define MPOL_DEFAULT        0  // 默认策略（系统级分配）
#define MPOL_PREFERRED      1  // 优先节点
#define MPOL_BIND           2  // 绑定到指定节点集
#define MPOL_INTERLEAVE     3  // 交错分配
#define MPOL_LOCAL          4  // 本地节点分配
#define MPOL_PREFERRED_MANY 5  // 优先多个节点
#define MPOL_WEIGHTED_INTERLEAVE 6  // 加权交错分配
```

## 4. 内核实现

```c
// mm/mempolicy.c
SYSCALL_DEFINE5(get_mempolicy, int __user *, policy,
                unsigned long __user *, nmask, unsigned long, maxnode,
                unsigned long, addr, unsigned long, flags)
{
    return kernel_get_mempolicy(policy, nmask, maxnode, addr, flags);
}

static int kernel_get_mempolicy(int __user *policy,
                                unsigned long __user *nmask,
                                unsigned long maxnode,
                                unsigned long addr,
                                unsigned long flags)
{
    int err;
    int pval;
    nodemask_t nodes;

    if (nmask != NULL && maxnode < nr_node_ids)
        return -EINVAL;

    addr = untagged_addr(addr);

    err = do_get_mempolicy(&pval, &nodes, addr, flags);
    if (err)
        return err;

    if (policy && put_user(pval, policy))
        return -EFAULT;

    if (nmask)
        err = copy_nodes_to_user(nmask, maxnode, &nodes);

    return err;
}
```

## 5. 详细调用链

```
kernel_get_mempolicy(policy, nmask, maxnode, addr, flags)  // mm/mempolicy.c
  └─ do_get_mempolicy(&pval, &nodes, addr, flags)          // 核心实现
       ├─ [flags 无效] → return -EINVAL
       │
       ├─ [MPOL_F_MEMS_ALLOWED]:                           // 查询 cpuset 掩码
       │    ├─ task_lock(current)
       │    ├─ *nmask = cpuset_current_mems_allowed
       │    └─ task_unlock(current)
       │
       ├─ [MPOL_F_ADDR]:                                    // 查询 VMA 策略
       │    ├─ mmap_read_lock(mm)
       │    ├─ vma_lookup(mm, addr)                         // 查找 VMA
       │    │    └─ [未找到] → return -EFAULT
       │    ├─ __get_vma_policy(vma, addr, &ilx)            // 获取 VMA 策略
       │    │    └─ vma->vm_policy 或 task->mempolicy
       │    └─ mmap_read_unlock(mm)
       │
       ├─ [MPOL_F_NODE | MPOL_F_ADDR]:                      // 查询地址所在节点
       │    ├─ lookup_node(mm, addr)                        // 通过 GUP 查页节点
       │    │    └─ get_user_pages_fast() + page_to_nid()
       │    └─ *policy = node_id
       │
       └─ [默认]:                                           // 查询进程默认策略
            ├─ pol = current->mempolicy
            ├─ [pol == NULL] → pol = &default_policy
            ├─ *policy = pol->mode
            └─ get_policy_nodemask(pol, &nodes)             // 获取策略节点掩码
```

## 6. 核心数据结构

### 6.1 struct mempolicy（内存策略）

```c
// include/linux/mempolicy.h
struct mempolicy {
    atomic_t refcnt;                         // 引用计数
    unsigned short mode;                     // 策略模式（MPOL_*）
    unsigned short flags;                    // 策略标志
    nodemask_t nodes;                        // 允许的节点掩码
    union {
        struct {
            unsigned short preferred_node;   // 优先节点（MPOL_PREFERRED）
            unsigned short user_nodemask_len; // 用户节点掩码长度
        } prefs;
        struct {
            spinlock_t lock;
            struct rb_root_cached rb_root;   // 交错分配的红黑树
            int interval;                    // 交错间隔
        } interleave;
    };
    struct rcu_head rcu;                     // RCU 销毁
    unsigned short home_node;                // home 节点（用于 MPOL_BIND/PREFERRED_MANY）
};
```

### 6.2 策略相关标志

```c
// include/uapi/linux/mempolicy.h
/* 模式标志 */
#define MPOL_F_STATIC_NODES     (1 << 15)  // 静态节点掩码（不随 cpuset 变化）
#define MPOL_F_RELATIVE_NODES   (1 << 14)  // 相对节点掩码（相对 cpuset 偏移）
/* 迁移标志 */
#define MPOL_MF_STRICT          (1 << 0)   // 严格模式
#define MPOL_MF_MOVE            (1 << 1)   // 迁移现有页面
#define MPOL_MF_MOVE_ALL        (1 << 2)   // 迁移所有页面（需 CAP_SYS_NICE）
```

## 7. 流程图

```
get_mempolicy 查询策略:
======================

用户态                         内核态
   |                              |
   | syscall(SYS_get_mempolicy,   |
   |   &policy, &nmask, ...)      |
   |----------------------------->|
   |                          kernel_get_mempolicy()
   |                            └─ do_get_mempolicy()
   |                                  │
   |                                  ├─ [MPOL_F_MEMS_ALLOWED]
   |                                  │    └─ 返回 cpuset 掩码
   |                                  │
   |                                  ├─ [MPOL_F_ADDR]
   |                                  │    ├─ 查找 VMA
   |                                  │    ├─ 获取 VMA 策略
   |                                  │    └─ [MPOL_F_NODE] 查页节点
   |                                  │
   |                                  └─ [默认]
   │                                       ├─ 获取 task->mempolicy
   │                                       ├─ 获取策略模式
   │                                       └─ 获取节点掩码
   │                              |
   |        return (policy, mask) |
   |<-----------------------------|
   |                              |
```

## 8. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 参数无效 | `flags` 无效、`maxnode` 小于 `nr_node_ids` |
| `EFAULT` | 地址错误 | `policy` 或 `nmask` 不可写，或 `addr` 无效 |
| `ENOMEM` | 内存不足 | 复制节点掩码时内存不足 |

## 9. 使用示例

```c
#include <numaif.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// 获取当前进程的默认内存策略
int get_mempolicy(int *policy, unsigned long *nmask,
                  unsigned long maxnode, unsigned long addr,
                  unsigned long flags) {
    return syscall(SYS_get_mempolicy, policy, nmask,
                   maxnode, addr, flags);
}

int main() {
    int policy;
    unsigned long nmask = 0;
    int ret;

    // 1. 查询默认策略
    ret = get_mempolicy(&policy, &nmask, sizeof(nmask) * 8,
                        0, 0);
    if (ret == 0) {
        printf("默认策略: ");
        switch (policy) {
        case MPOL_DEFAULT:  printf("MPOL_DEFAULT\n");  break;
        case MPOL_BIND:     printf("MPOL_BIND\n");     break;
        case MPOL_PREFERRED: printf("MPOL_PREFERRED\n"); break;
        case MPOL_INTERLEAVE: printf("MPOL_INTERLEAVE\n"); break;
        case MPOL_LOCAL:    printf("MPOL_LOCAL\n");    break;
        default:            printf("未知 (%d)\n", policy);
        }
        printf("节点掩码: 0x%lx\n", nmask);
    }

    // 2. 查询指定地址的 VMA 策略
    void *ptr = malloc(4096);
    if (ptr) {
        ret = get_mempolicy(&policy, &nmask, sizeof(nmask) * 8,
                            (unsigned long)ptr, MPOL_F_ADDR);
        if (ret == 0) {
            printf("地址 %p 的策略: %d, 掩码: 0x%lx\n",
                   ptr, policy, nmask);
        }
        free(ptr);
    }

    // 3. 查询地址所在 NUMA 节点
    int stack_var = 0;
    int node;
    ret = get_mempolicy(&node, NULL, 0,
                        (unsigned long)&stack_var,
                        MPOL_F_NODE | MPOL_F_ADDR);
    if (ret == 0) {
        printf("栈变量所在 NUMA 节点: %d\n", node);
    }

    // 4. 查询 cpuset 允许节点
    ret = get_mempolicy(&policy, &nmask, sizeof(nmask) * 8,
                        0, MPOL_F_MEMS_ALLOWED);
    if (ret == 0) {
        printf("cpuset 允许节点掩码: 0x%lx\n", nmask);
    }

    return 0;
}
```

## 10. 关键要点

1. **策略优先级**：VMA 策略 > 进程策略 > 系统默认策略
2. **MPOL_F_ADDR**：查询 VMA 策略时，如果 VMA 没有显式设置策略（`vma->vm_policy == NULL`），则返回 `MPOL_DEFAULT`，不会回退到进程策略
3. **MPOL_F_NODE**：当与 `MPOL_F_ADDR` 一起使用时，返回该地址所在页面的物理 NUMA 节点 ID
4. **MPOL_F_MEMS_ALLOWED**：返回 cpuset 限制的节点掩码，此时 `policy` 输出值无效
5. **兼容性**：`maxnode` 参数用于指定 `nmask` 的位宽，需要 `>= nr_node_ids`

## 11. 源码位置

| 文件 | 说明 |
|------|------|
| [mm/mempolicy.c](file:///home/louis/code/linux/mm/mempolicy.c) | `do_get_mempolicy`、`kernel_get_mempolicy` 实现 |
| [include/linux/mempolicy.h](file:///home/louis/code/linux/include/linux/mempolicy.h) | `struct mempolicy`、策略操作函数声明 |
| [include/uapi/linux/mempolicy.h](file:///home/louis/code/linux/include/uapi/linux/mempolicy.h) | `MPOL_*` 宏定义、用户态标志 |