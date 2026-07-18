# setreuid 系统调用分析

## 1. 概述

`setreuid` 设置当前进程的实际和有效用户 ID（real UID 和 effective UID）。BSD 风格的 `setreuid` 允许分别设置 ruid 和 euid，传递 -1 表示保留当前值不变。这是 `seteuid` 的底层实现基础。

**原型：**

```c
SYSCALL_DEFINE2(setreuid, uid_t, ruid, uid_t, euid)
```

**参数：**
- `ruid`：新的实际 UID。若为 -1，则保持不变
- `euid`：新的有效 UID。若为 -1，则保持不变

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **临时切换用户身份**：特权程序在特权和非特权模式间切换
- **精细控制用户 ID**：分别控制实际和有效 UID
- **BSD 兼容性**：为旧程序提供兼容接口
- **seteuid 的基础实现**：`seteuid(euid)` 实际调用 `setreuid(-1, euid)`

## 3. 函数调用栈

```
setreuid(ruid, euid)                                     // kernel/sys.c
  └─ __sys_setreuid(ruid, euid)
       ├─ make_kuid(ns, ruid/euid) → 转换内核 kuid_t
       │   └─ 若无效 → -EINVAL
       ├─ prepare_creds() → 分配新凭证
       │   └─ 若 NULL → -ENOMEM
       ├─ [ruid != -1] 权限检查:
       │    ├─ 设置 new->uid = kruid
       │    └─ 若没有 CAP_SETUID:
       │         if (!uid_eq(old->uid, kruid) && !uid_eq(old->euid, kruid))
       │             → goto error (-EPERM)
       ├─ [euid != -1] 权限检查:
       │    ├─ 设置 new->euid = keuid
       │    └─ 若没有 CAP_SETUID:
       │         if (!uid_eq(old->uid, keuid) && !uid_eq(old->euid, keuid) &&
       │             !uid_eq(old->suid, keuid))
       │             → goto error (-EPERM)
       ├─ 若 ruid 改变 → set_user(new) 更新用户计数
       ├─ 更新 suid: 如果 ruid 被设置或 euid 被改为非旧 uid 的值
       │    new->suid = new->euid
       ├─ new->fsuid = new->euid
       ├─ security_task_fix_setuid(new, old, LSM_SETID_RE) → LSM 检查
       ├─ set_cred_ucounts() → 更新用户计数
       ├─ flag_nproc_exceeded() → 检查 NPROC 限制
       └─ commit_creds(new) → 返回 0
```

## 4. 关键数据结构

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID       ← setreuid 可修改此字段
    kgid_t gid;          // real GID
    kuid_t suid;         // saved UID      ← 可能被自动更新
    kgid_t sgid;         // saved GID
    kuid_t euid;         // effective UID  ← setreuid 可修改此字段
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations ← 自动同步为 euid
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

## 5. 流程图

```
用户态: setreuid(ruid, euid)
    │
    v
┌─────────────────────────────────────────────────┐
│ make_kuid() 转换参数 → 无效则 -EINVAL           │
│ prepare_creds() → 分配凭证 → 失败则 -ENOMEM      │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────────┐
│ ruid != -1?                                              │
│ ├─ 是 → 有 CAP_SETUID?                                   │
│ │   ├─ 是 → 设置 new->uid = kruid                        │
│ │   └─ 否 → kruid 是 old->uid 或 old->euid?              │
│ │         ├─ 是 → 设置 new->uid = kruid                  │
│ │         └─ 否 → -EPERM                                 │
│ └─ 否 → 跳过                                             │
│                                                          │
│ euid != -1?                                              │
│ ├─ 是 → 有 CAP_SETUID?                                   │
│ │   ├─ 是 → 设置 new->euid = keuid                       │
│ │   └─ 否 → keuid 是 ruid/euid/suid 之一?                │
│ │         ├─ 是 → 设置 new->euid = keuid                  │
│ │         └─ 否 → -EPERM                                 │
│ └─ 否 → 跳过                                             │
└─────────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ 若 ruid 改变 → set_user(new)                     │
│ suid 更新: ruid 被设置 或 euid ≠ 旧 uid?        │
│ ├─ 是 → new->suid = new->euid                   │
│ └─ 否 → suid 不变                               │
│ new->fsuid = new->euid                          │
│ security_task_fix_setuid() → LSM 检查            │
│ set_cred_ucounts()                              │
│ flag_nproc_exceeded()                           │
│ commit_creds(new) → 返回 0                       │
└─────────────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效 UID | `ruid` 或 `euid` 在当前用户命名空间中无效 |
| `EPERM` | 权限不足 | 非特权进程尝试设置不允许的值 |
| `ENOMEM` | 内存不足 | 无法分配新的凭证结构体 |
| `EAGAIN` | 资源限制 | `set_user()` 失败（如 `RLIMIT_NPROC` 限制） |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    uid_t ruid = getuid();
    uid_t euid = geteuid();

    printf("Before: RUID=%d, EUID=%d\n", ruid, euid);

    /* 交换实际 UID 和有效 UID */
    if (setreuid(euid, ruid) < 0) {
        perror("setreuid");
        return 1;
    }

    printf("After:  RUID=%d, EUID=%d\n", getuid(), geteuid());

    return 0;
}
```

### 7.2 仅修改有效 UID

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    /* 将 EUID 设为当前 RUID（保留 RUID 不变） */
    if (setreuid(-1, getuid()) < 0) {
        perror("setreuid");
        return 1;
    }

    printf("After setreuid(-1, ruid): RUID=%d, EUID=%d\n",
           getuid(), geteuid());

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setreuid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)