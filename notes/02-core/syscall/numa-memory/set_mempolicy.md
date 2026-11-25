# set_mempolicy 系统调用分析

## 1. 概述

`set_mempolicy` 用于设置调用进程的默认内存策略（mempolicy）。该策略影响进程后续所有匿名内存映射的分配行为，决定了页面分配时优先选择的 NUMA 节点。

常用的策略包括：
- **MPOL_DEFAULT**：系统默认分配（本地节点优先）
- **MPOL_BIND**：绑定到指定节点集
- **MPOL_PREFERRED**：优先选择指定节点
- **MPOL_INTERLEAVE**：在指定节点集间交错分配
- **MPOL_LOCAL**：仅在分配线程所在的本地节点分配
- **MPOL_PREFERRED_MANY**：优先多个节点
- **MPOL_WEIGHTED_INTERLEAVE**：加权交错分配

## 2. 函数原型

```c
#include <numaif.h>
#include <sys/syscall.h>

long ret = syscall(SYS_set_mempolicy,
    int mode,                 // 策略模式（MPOL_*）
    unsigned long *nmask,     // 节点掩码
    unsigned long maxnode);   // nmask 的最大节点数
```

## 3. 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `mode` | `int` | 策略模式，可包含 `MPOL_F_*` 标志 |
| `nmask` | `unsigned long*` | 节点掩码指针（`MPOL_DEFAULT` 和 `MPOL_LOCAL` 可为 NULL） |
| `maxnode` | `unsigned long` | `nmask` 的最大节点 ID |

## 4. 内核实现

```c
// mm/mempolicy.c
SYSCALL_DEFINE3(set_mempolicy, int, mode, const unsigned long __user *, nmask,
                unsigned long, maxnode)
{
    return kernel_set_mempolicy(mode, nmask, maxnode);
}

static long kernel_set_mempolicy(int mode, const unsigned long __user *nmask,
                                 unsigned long maxnode)
{
    unsigned short mode_flags;
    nodemask_t nodes;
    int lmode = mode;
    int err;

    err = sanitize_mpol_flags(&lmode, &mode_flags);  // 分离模式与标志
    if (err)
        return err;

    err = get_nodes(&nodes, nmask, maxnode);          // 从用户空间获取节点掩码
    if (err)
        return err;

    return do_set_mempolicy(lmode, mode_flags, &nodes); // 设置策略
}

static long do_set_mempolicy(unsigned short mode, unsigned short flags,
                             nodemask_t *nodes)
{
    struct mempolicy *new, *old;
    NODEMASK_SCRATCH(scratch);
    int ret;

    if (!scratch)
        return -ENOMEM;

    new = mpol_new(mode, flags, nodes);               // 创建新策略对象
    if (IS_ERR(new))
        return PTR_ERR(new);

    task_lock(current);
    ret = mpol_set_nodemask(new, nodes, scratch);     // 设置节点掩码并验证
    if (ret) {
        task_unlock(current);
        mpol_put(new);
        return ret;
    }

    old = current->mempolicy;                         // 保存旧策略
    current->mempolicy = new;                         // 更新为新策略
    if (new && (new->mode == MPOL_INTERLEAVE ||
                new->mode == MPOL_WEIGHTED_INTERLEAVE)) {
        current->il_prev = MAX_NUMNODES - 1;          // 重置交错索引
        current->il_weight = 0;
    }
    task_unlock(current);
    mpol_put(old);                                    // 释放旧策略引用
    return 0;
}
```

## 5. 详细调用链

```
kernel_set_mempolicy(mode, nmask, maxnode)              // mm/mempolicy.c
  ├─ sanitize_mpol_flags(&lmode, &mode_flags)
  │    ├─ 从 mode 中分离 MPOL_F_STATIC_NODES/RELATIVE_NODES 标志
  │    └─ 验证模式合法性
  │
  ├─ get_nodes(&nodes, nmask, maxnode)                  // 解析节点掩码
  │    ├─ [MPOL_DEFAULT || MPOL_LOCAL] → nodes_clear
  │    ├─ [MPOL_PREFERRED && nmask == NULL] → 单节点 0
  │    └─ [其他] → copy_from_user 读取节点掩码
  │
  └─ do_set_mempolicy(mode, flags, &nodes)
       ├─ mpol_new(mode, flags, nodes)                  // 创建 mempolicy 对象
       │    ├─ kmem_cache_alloc(policy_cache)           // 从 slab 缓存分配
       │    ├─ 初始化 mode、flags、refcnt=1
       │    └─ 根据模式初始化特定字段
       │
       ├─ task_lock(current)                            // 保护 task_struct
       ├─ mpol_set_nodemask(new, nodes, scratch)        // 设置并验证节点掩码
       │    ├─ 验证节点在线且在 cpuset 内
       │    ├─ 复制节点掩码到 new->nodes
       │    └─ 设置 preferred_node 等
       │
       ├─ current->mempolicy = new                      // 替换策略
       ├─ task_unlock(current)
       └─ mpol_put(old)                                 // 释放旧策略
```

## 6. 核心数据结构

### 6.1 struct mempolicy

```c
// include/linux/mempolicy.h
struct mempolicy {
    atomic_t refcnt;                         // 引用计数
    unsigned short mode;                     // 策略模式
    unsigned short flags;                    // MPOL_F_STATIC_NODES 等
    nodemask_t nodes;                        // 策略节点掩码
    union {
        struct {                             // MPOL_PREFERRED / PREFERRED_MANY
            unsigned short preferred_node;
            unsigned short user_nodemask_len;
        } prefs;
        struct {                             // MPOL_INTERLEAVE / WEIGHTED_INTERLEAVE
            spinlock_t lock;
            struct rb_root_cached rb_root;
            int interval;
        } interleave;
    };
    struct rcu_head rcu;                     // RCU 回调
    unsigned short home_node;                // home 节点
};
```

### 6.2 mm_struct 中的策略相关字段

```c
// include/linux/mm_types.h
struct mm_struct {
    // ...
    struct mempolicy *mempolicy;             // 进程级内存策略
    unsigned long il_prev;                   // 前一次交错分配时的地址偏移
    unsigned long nid_prev;                  // 前一次分配的节点
    // ...
};
```

## 7. 流程图

```
set_mempolicy 调用流程:
======================

用户态                         内核态
   |                              |
   | syscall(SYS_set_mempolicy,   |
   |   MPOL_BIND, nmask, maxnode) |
   |----------------------------->|
   |                          kernel_set_mempolicy()
   |                            ├─ sanitize_mpol_flags()
   |                            ├─ get_nodes()  // 读用户掩码
   |                            └─ do_set_mempolicy()
   |                                 ├─ mpol_new()     // 分配策略对象
   |                                 │    └─ kmem_cache_alloc()
   |                                 ├─ task_lock()
   |                                 ├─ mpol_set_nodemask()
   |                                 ├─ current->mempolicy = new
   |                                 ├─ task_unlock()
   |                                 └─ mpol_put(old)  // 释放旧策略
   |                              |
   |        return 0             |
   |<-----------------------------|
   |                              |
```

## 8. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 参数无效 | `mode` 无效、`maxnode` 无效、`nmask` 为空节点 |
| `ENOMEM` | 内存不足 | 无法分配 `mempolicy` 结构体或 `NODEMASK_SCRATCH` |
| `EPERM` | 权限不足 | 请求的节点不在进程 cpuset 中且无 `CAP_SYS_NICE` |

## 9. 使用示例

```c
#include <numaif.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int set_mempolicy(int mode, unsigned long *nmask,
                         unsigned long maxnode) {
    return syscall(SYS_set_mempolicy, mode, nmask, maxnode);
}

static void print_err(const char *msg) {
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
}

int main() {
    unsigned long nmask;
    int ret;

    // 1. 设置默认策略（恢复系统默认行为）
    ret = set_mempolicy(MPOL_DEFAULT, NULL, 0);
    if (ret) { print_err("MPOL_DEFAULT"); return 1; }
    printf("已设置默认策略\n");

    // 2. 绑定到 NUMA 节点 0 和 1
    nmask = (1 << 0) | (1 << 1);  // 节点 0 和 1
    ret = set_mempolicy(MPOL_BIND, &nmask, 2);
    if (ret == 0) {
        printf("已绑定到节点 0 和 1\n");
    } else if (errno == EINVAL) {
        printf("节点 0 或 1 不可用\n");
    }

    // 3. 设置交错分配（在节点 0-3 间）
    nmask = 0x0F;  // 节点 0,1,2,3
    ret = set_mempolicy(MPOL_INTERLEAVE, &nmask, 4);
    if (ret == 0) {
        printf("已设置交错分配，节点 0-3\n");
    }

    // 4. 优先节点 0
    nmask = 1 << 0;
    ret = set_mempolicy(MPOL_PREFERRED, &nmask, 1);
    if (ret == 0) {
        printf("已设置优先节点 0\n");
    }

    // 5. 本地节点分配
    ret = set_mempolicy(MPOL_LOCAL, NULL, 0);
    if (ret == 0) {
        printf("已设置本地节点分配\n");
    }

    return 0;
}
```

## 10. 关键要点

1. **进程级策略**：`set_mempolicy` 设置的是进程默认策略，影响所有后续匿名分配。VMA 策略（通过 `mbind` 设置）具有更高优先级
2. **线程继承**：子进程通过 `fork()` 继承父进程的内存策略
3. **cpuset 限制**：策略节点掩码必须是 cpuset 允许掩码的子集，否则返回 `-EINVAL`
4. **交错分配**：`MPOL_INTERLEAVE` 在每次页面分配时轮换节点，`il_prev` 跟踪上次分配的节点
5. **引用计数**：`mempolicy` 使用引用计数管理生命周期，`mpol_put` 在引用计数归零时释放

## 11. 源码位置

| 文件 | 说明 |
|------|------|
| [mm/mempolicy.c](file:///home/louis/code/linux/mm/mempolicy.c) | `do_set_mempolicy`、`kernel_set_mempolicy`、`mpol_new`、`mpol_set_nodemask` 实现 |
| [include/linux/mempolicy.h](file:///home/louis/code/linux/include/linux/mempolicy.h) | `struct mempolicy` 定义、策略操作函数 |
| [include/uapi/linux/mempolicy.h](file:///home/louis/code/linux/include/uapi/linux/mempolicy.h) | `MPOL_*` 宏定义 |