# setregid 系统调用分析

## 1. 概述

`setregid` 设置当前进程的实际和有效组 ID（real GID 和 effective GID）。BSD 风格的 `setregid` 允许分别设置 rgid 和 egid，传递 -1 表示保留当前值不变。这是 `setegid` 的底层实现基础。

**原型：**

```c
SYSCALL_DEFINE2(setregid, gid_t, rgid, gid_t, egid)
```

**参数：**
- `rgid`：新的实际 GID。若为 -1，则保持不变
- `egid`：新的有效 GID。若为 -1，则保持不变

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **临时切换组身份**：特权程序在特权和非特权模式间切换
- **精细控制组 ID**：分别控制实际和有效 GID
- **BSD 兼容性**：为旧程序提供兼容接口

## 3. 函数调用栈

```
setregid(rgid, egid)                                     // kernel/sys.c
  └─ __sys_setregid(rgid, egid)
       ├─ make_kgid(ns, rgid/egid) → 转换内核 kgid_t
       │   └─ 若无效 → -EINVAL
       ├─ prepare_creds() → 分配新凭证
       │   └─ 若 NULL → -ENOMEM
       ├─ [rgid != -1] 权限检查:
       │    ├─ [有 CAP_SETGID] → 直接设置 new->gid = krgid
       │    └─ [无 CAP_SETGID] → 只能设置为 rgid 或 egid:
       │         if (gid_eq(old->gid, krgid) || gid_eq(old->egid, krgid))
       │             new->gid = krgid
       │         else → goto error (-EPERM)
       ├─ [egid != -1] 权限检查:
       │    ├─ [有 CAP_SETGID] → 直接设置 new->egid = kegid
       │    └─ [无 CAP_SETGID] → 只能设置为 rgid/egid/sgid:
       │         if (gid_eq(old->gid, kegid) || gid_eq(old->egid, kegid) ||
       │             gid_eq(old->sgid, kegid))
       │             new->egid = kegid
       │         else → goto error (-EPERM)
       ├─ 更新 sgid: 如果 rgid 被设置或 egid 被改为非旧 rgid 的值
       │    new->sgid = new->egid
       ├─ new->fsgid = new->egid
       ├─ security_task_fix_setgid(new, old, LSM_SETID_RE) → LSM 检查
       └─ commit_creds(new) → 返回 0
```

## 4. 关键数据结构

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID
    kgid_t gid;          // real GID       ← setregid 可修改此字段
    kuid_t suid;         // saved UID
    kgid_t sgid;         // saved GID      ← 可能被自动更新
    kuid_t euid;         // effective UID
    kgid_t egid;         // effective GID  ← setregid 可修改此字段
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations ← 自动同步为 egid
    // ...
};
```

## 5. 流程图

```
用户态: setregid(rgid, egid)
    │
    v
┌─────────────────────────────────────────────────┐
│ make_kgid() 转换参数 → 无效则 -EINVAL           │
│ prepare_creds() → 分配凭证 → 失败则 -ENOMEM      │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────────┐
│ rgid != -1?                                              │
│ ├─ 是 → 有 CAP_SETGID?                                   │
│ │   ├─ 是 → 设置 new->gid = krgid                        │
│ │   └─ 否 → krgid 是 old->gid 或 old->egid?              │
│ │         ├─ 是 → 设置 new->gid = krgid                  │
│ │         └─ 否 → -EPERM                                 │
│ └─ 否 → 跳过                                             │
│                                                          │
│ egid != -1?                                              │
│ ├─ 是 → 有 CAP_SETGID?                                   │
│ │   ├─ 是 → 设置 new->egid = kegid                       │
│ │   └─ 否 → kegid 是 rgid/egid/sgid 之一?                │
│ │         ├─ 是 → 设置 new->egid = kegid                  │
│ │         └─ 否 → -EPERM                                 │
│ └─ 否 → 跳过                                             │
└─────────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ sgid 更新: rgid 被设置 或 egid ≠ 旧 rgid?       │
│ ├─ 是 → new->sgid = new->egid                   │
│ └─ 否 → sgid 不变                               │
│ new->fsgid = new->egid                          │
│ security_task_fix_setgid() → LSM 检查            │
│ commit_creds(new) → 返回 0                       │
└─────────────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效 GID | `rgid` 或 `egid` 在当前用户命名空间中无效 |
| `EPERM` | 权限不足 | 非特权进程尝试设置不允许的值 |
| `ENOMEM` | 内存不足 | 无法分配新的凭证结构体 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    gid_t rgid = getgid();
    gid_t egid = getegid();

    printf("Before: RGID=%d, EGID=%d\n", rgid, egid);

    /* 交换实际 GID 和有效 GID */
    if (setregid(egid, rgid) < 0) {
        perror("setregid");
        return 1;
    }

    printf("After:  RGID=%d, EGID=%d\n", getgid(), getegid());

    return 0;
}
```

### 7.2 仅修改有效 GID

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    /* 将 EGID 设为当前 RGID（保留 RGID 不变） */
    if (setregid(-1, getgid()) < 0) {
        perror("setregid");
        return 1;
    }

    printf("After setregid(-1, rgid): RGID=%d, EGID=%d\n",
           getgid(), getegid());

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setregid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)