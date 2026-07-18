# getgroups 系统调用分析

## 1. 概述

获取当前进程的附属组（supplementary groups）列表。附属组用于扩展进程的组权限检查。

**原型：**

```c
SYSCALL_DEFINE2(getgroups, int, gidsetsize, gid_t __user *, grouplist)
```

**参数：**
- `gidsetsize`：`grouplist` 数组的大小（元素个数）。若为 0，则仅返回附属组数量
- `grouplist`：指向用户空间 `gid_t` 数组的指针，用于接收组 ID 列表

## 2. 使用场景

- 查询进程所属的所有组
- 安全检查：确认用户是否属于某个组
- `groups` 命令的底层实现

## 3. 函数调用栈

```
getgroups(gidsetsize, grouplist)                         // kernel/groups.c
  ├─ [gidsetsize < 0] → 返回 -EINVAL
  ├─ i = cred->group_info->ngroups                       // 获取附属组数量
  ├─ [gidsetsize != 0]:
  │    ├─ [i > gidsetsize] → 返回 -EINVAL               // 缓冲区太小
  │    └─ groups_to_user(grouplist, cred->group_info)    // 拷贝到用户空间
  └─ 返回 i (附属组数量)
```

## 4. 关键数据结构

```c
// include/linux/cred.h
struct group_info {
    refcount_t usage;   // 引用计数
    int ngroups;        // 附属组数量
    kgid_t gid[];       // 组 ID 数组（柔性数组）
};
```

## 5. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

int main(void)
{
    int ngroups;
    gid_t *groups;

    /* 第一次调用：获取组数量 */
    ngroups = getgroups(0, NULL);
    if (ngroups < 0) {
        perror("getgroups");
        return 1;
    }

    printf("Number of supplementary groups: %d\n", ngroups);

    if (ngroups == 0)
        return 0;

    groups = malloc(ngroups * sizeof(gid_t));
    if (!groups) {
        perror("malloc");
        return 1;
    }

    /* 第二次调用：获取实际组列表 */
    if (getgroups(ngroups, groups) < 0) {
        perror("getgroups");
        free(groups);
        return 1;
    }

    printf("Supplementary groups: ");
    for (int i = 0; i < ngroups; i++)
        printf("%d ", groups[i]);
    printf("\n");

    free(groups);
    return 0;
}
```

## 6. 参考

- 源码位置：`kernel/groups.c`
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)