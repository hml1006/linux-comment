# lsm_list_modules 系统调用分析

## 1. 概述

列出当前系统上已加载的 Linux Security Module (LSM) 模块 ID。该调用是 LSM 用户空间 API 的一部分，运行时可查询系统中启用了哪些 LSM 模块。

**原型：**

```c
SYSCALL_DEFINE3(lsm_list_modules,
    u64 __user *, ids,
    u32 __user *, size,
    u32, flags)
```

**参数：**
- `ids`：指向用户空间 `u64` 数组的指针，用于接收 LSM ID 列表（可为 NULL 用于查询大小）
- `size`：指向 `u32` 的指针，输入时表示数组元素个数，输出时表示所需元素个数
- `flags`：预留，必须为 0

**返回值：**
- 成功时返回已加载的 LSM 模块数量（可能为 0）
- 如果 `size` 不足，返回 `-E2BIG` 并将 `size` 更新为所需的最小值
- 失败时返回负的错误码

## 2. 使用场景

- 安全审计工具查询系统启用了哪些 LSM
- 应用程序运行时检测安全上下文环境
- 容器管理工具检查 LSM 兼容性

## 3. 函数调用栈

```
lsm_list_modules(ids, size, flags)                      // security/lsm_syscalls.c
  ├─ flags 检查 → 必须为 0
  ├─ get_user(usize, size)                              // 获取用户传入的 size
  ├─ total_size = lsm_active_cnt * sizeof(*ids)         // 计算所需总大小
  ├─ put_user(total_size, size)                         // 更新 size 为所需值
  ├─ [usize < total_size] → 返回 -E2BIG                // 缓冲区不足
  └─ 循环写入每个 LSM 的 ID:
       for (i = 0; i < lsm_active_cnt; i++)
           put_user(lsm_idlist[i]->id, ids++)
  └─ 返回 lsm_active_cnt
```

## 4. 关键数据结构

### 4.1 LSM ID 列表（内核全局变量）

```c
// security/security.c
const struct lsm_id *lsm_idlist[LSM_CONFIG_COUNT];  // 活跃 LSM 模块 ID 列表
int lsm_active_cnt;                                     // 活跃 LSM 模块数量
```

### 4.2 struct lsm_id（LSM 模块标识）

```c
// include/linux/lsm_hooks.h
struct lsm_id {
    const char *name;    // LSM 名称（如 "selinux", "apparmor"）
    u64 id;              // LSM ID 值（对应 LSM_ID_XXX）
};
```

## 5. 流程图

```
用户态: lsm_list_modules(ids, &size, 0)
    │
    v
┌─────────────────────────────────────┐
│ 检查 flags 是否为 0                 │
│ 否 → 返回 -EINVAL                    │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ get_user(usize, size)               │
│ 从用户空间读取 size 值              │
│ 失败 → 返回 -EFAULT                 │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ total_size = lsm_active_cnt * 8     │
│ put_user(total_size, size)          │
│ 无论是否成功，先更新 size 为所需值  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ usize < total_size?                 │
│ 是 → 返回 -E2BIG                    │
│ 否 → 继续                           │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 循环写入 LSM ID 数组                │
│ for i = 0 to lsm_active_cnt - 1:    │
│   put_user(lsm_idlist[i]->id, ids++)│
│   失败 → 返回 -EFAULT               │
└─────────────────────────────────────┘
    │
    v
返回 lsm_active_cnt (模块数量)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | flags 非零 |
| `-EFAULT` | 内存错误 | ids 或 size 指针不可访问 |
| `-E2BIG` | 缓冲区不足 | ids 数组元素个数小于所需值（size 已更新） |

## 7. 使用示例

```c
#include <linux/lsm.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef lsm_list_modules
static inline int
lsm_list_modules(u64 *ids, u32 *size, u32 flags)
{
    return syscall(__NR_lsm_list_modules, ids, size, flags);
}
#endif

int main(void)
{
    u32 size = 0;
    u64 *ids;
    int count;

    /* 第一次调用：查询模块数量 */
    count = lsm_list_modules(NULL, &size, 0);
    if (count < 0) {
        perror("lsm_list_modules");
        return 1;
    }
    printf("Number of active LSM modules: %d\n", count);

    if (count == 0) {
        printf("No LSM modules loaded\n");
        return 0;
    }

    /* 分配 IDs 数组 */
    ids = malloc(count * sizeof(u64));
    if (!ids) {
        perror("malloc");
        return 1;
    }

    /* 第二次调用：获取实际 ID 列表 */
    count = lsm_list_modules(ids, &size, 0);
    if (count < 0) {
        perror("lsm_list_modules");
        free(ids);
        return 1;
    }

    printf("Loaded LSM modules:\n");
    for (int i = 0; i < count; i++) {
        printf("  [%d] ID = %llu\n", i,
               (unsigned long long)ids[i]);
    }

    free(ids);
    return 0;
}
```

## 8. 参考

- 源码位置：`security/lsm_syscalls.c`
- 头文件：`include/uapi/linux/lsm.h`
- LSM 框架：`security/security.c`
- [ARM64 系统调用表](../arm64-syscall-table.md#权限与安全)