# setgroups 系统调用分析

## 1. 概述

`setgroups` 设置当前进程的附属组（supplementary groups）列表。附属组用于扩展进程的组权限检查——当进程访问文件时，内核不仅检查有效 GID，还检查所有附属组。此操作需要 `CAP_SETGID` 能力。

**原型：**

```c
SYSCALL_DEFINE2(setgroups, int, gidsetsize, gid_t __user *, grouplist)
```

**参数：**
- `gidsetsize`：`grouplist` 数组的大小（元素个数）
- `grouplist`：指向用户空间 `gid_t` 数组的指针，包含新的附属组列表

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **初始化进程的附属组列表**：`/bin/login` 等程序在认证后设置用户的组列表
- **容器运行时配置命名空间的组映射**：设置容器内的附属组
- **权限管理工具**：`newgrp`、`sg` 等命令切换组时重新设置附属组

## 3. 函数调用栈

```
setgroups(gidsetsize, grouplist)                         // kernel/groups.c
  ├─ may_setgroups() → 检查是否拥有 CAP_SETGID 能力
  │   └─ 若无 → 返回 -EPERM
  ├─ gidsetsize > NGROUPS_MAX (65536) → 返回 -EINVAL
  ├─ groups_alloc(gidsetsize) → 分配 group_info 结构体
  │   └─ 若内存不足 → 返回 -ENOMEM
  ├─ groups_from_user(group_info, grouplist) → 从用户空间拷贝组列表
  │   └─ 若拷贝失败 → put_group_info() → 返回错误码
  ├─ groups_sort(group_info) → 对组列表排序（二分查找优化）
  └─ set_current_groups(group_info) → 设置到当前凭证
       └─ commit_creds() → 应用新凭证
```

## 4. 关键数据结构

### 4.1 struct group_info（附属组信息）

```c
// include/linux/cred.h
struct group_info {
    refcount_t usage;   // 引用计数
    int ngroups;        // 附属组数量
    kgid_t gid[];       // 组 ID 数组（柔性数组，按升序排列）
};
```

### 4.2 辅助函数

```c
// kernel/groups.c
static int may_setgroups(void)
{
    return ns_capable(current_user_ns(), CAP_SETGID);
}

// 最大附属组数量
#define NGROUPS_MAX 65536
```

## 5. 流程图

```
用户态: setgroups(gidsetsize, grouplist)
    │
    v
┌─────────────────────────────────────────────────────┐
│ may_setgroups()                                      │
│ 检查 CAP_SETGID 能力                                 │
│ 若无 → -EPERM                                        │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ gidsetsize > NGROUPS_MAX? → -EINVAL                 │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ groups_alloc(gidsetsize) → 分配 group_info           │
│ 若内存不足 → -ENOMEM                                 │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ groups_from_user(grouplist) → 从用户空间拷贝数据      │
│ 若 EFAULT → 释放已分配内存，返回 -EFAULT             │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ groups_sort(group_info) → 按 GID 升序排序            │
│ (用于后续 in_group_p() 的二分查找)                   │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ set_current_groups(group_info) → 应用新附属组列表    │
│ commit_creds() → 凭证生效                            │
└─────────────────────────────────────────────────────┘
    │
    v
返回 0 (成功)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EPERM` | 权限不足 | 调用进程没有 `CAP_SETGID` 能力 |
| `EINVAL` | 参数无效 | `gidsetsize` 大于 `NGROUPS_MAX`（65536） |
| `ENOMEM` | 内存不足 | 无法分配 `group_info` 结构体 |
| `EFAULT` | 用户空间指针无效 | `grouplist` 指向不可读的地址 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <grp.h>

int main(void)
{
    gid_t groups[] = {1000, 1001, 1002};
    int ngroups = sizeof(groups) / sizeof(groups[0]);

    /* 需要 CAP_SETGID 权限 */
    if (setgroups(ngroups, groups) < 0) {
        perror("setgroups");
        return 1;
    }

    printf("Supplementary groups set successfully\n");
    return 0;
}
```

### 7.2 查询并重置组列表

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <grp.h>

int main(void)
{
    int ngroups;
    gid_t *groups;

    /* 先获取当前附属组数量 */
    ngroups = getgroups(0, NULL);
    if (ngroups < 0) {
        perror("getgroups");
        return 1;
    }

    printf("Current supplementary groups count: %d\n", ngroups);

    if (ngroups > 0) {
        groups = malloc(ngroups * sizeof(gid_t));
        getgroups(ngroups, groups);

        printf("Current groups: ");
        for (int i = 0; i < ngroups; i++)
            printf("%d ", groups[i]);
        printf("\n");
        free(groups);
    }

    /* 设置新的附属组列表（需要 CAP_SETGID） */
    gid_t new_groups[] = {1000, 2000};
    if (setgroups(2, new_groups) < 0) {
        perror("setgroups");
        return 1;
    }

    printf("Groups updated successfully\n");
    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/groups.c`
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)