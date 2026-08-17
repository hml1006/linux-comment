# seteuid 系统调用分析

## 1. 概述

`seteuid` 设置当前进程的有效用户 ID（effective UID）。`seteuid` 在 glibc 中实现为 `setreuid(-1, euid)` 的封装，没有独立的系统调用入口。非特权进程只能将 euid 设置为当前实际 UID、有效 UID 或保存的 UID 之一。这是非特权进程临时提升或降低权限的常用方式。

**原型：**

```c
// 在 glibc 中实现，非独立 syscall
// 实际调用: setreuid(-1, euid)
int seteuid(uid_t euid);
```

**参数：**
- `euid`：新的有效 UID。若为 -1，则保持不变

**返回值：**
- 成功：0
- 失败：-1，并设置 `errno`

## 2. 使用场景

- **临时提升权限执行特权操作**：setuid root 程序在执行特权操作后降权
- **临时降低权限避免误操作**：以普通用户身份执行操作，需要时再提升
- **守护进程在不同操作间切换身份**：在不同用户身份间切换处理请求
- **权限委派**：如 `sudo` 工具在验证后切换身份

## 3. 函数调用栈

```
seteuid(euid)                                            // glibc 封装
  └─ setreuid(-1, euid)                                  // kernel/sys.c
       └─ __sys_setreuid(-1, euid)
            ├─ make_kuid(ns, euid) → 转换内核 kuid_t
            ├─ prepare_creds() → 分配新凭证
            ├─ [euid != -1] 权限检查:
            │    ├─ [有 CAP_SETUID] → 直接设置
            │    └─ [无 CAP_SETUID] → 只能设置为 ruid/euid/suid 之一
            │        否则 → -EPERM
            ├─ if (!uid_eq(new->uid, old->uid)) → set_user(new)
            ├─ new->suid = new->euid  (如果 euid 被改变)
            ├─ new->fsuid = new->euid
            ├─ security_task_fix_setuid() → LSM 检查
            ├─ set_cred_ucounts() → 更新用户计数
            ├─ flag_nproc_exceeded() → 检查 NPROC 限制
            └─ commit_creds(new)
```

## 4. 关键数据结构

### 4.1 struct cred（进程凭证）

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID
    kgid_t gid;          // real GID
    kuid_t suid;         // saved UID (setuid 保留)
    kgid_t sgid;         // saved GID
    kuid_t euid;         // effective UID  ← seteuid 修改此字段
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

## 5. 流程图

```
用户态: seteuid(euid)
    │
    v
┌─────────────────────────────────────────────────┐
│ make_kuid(ns, euid)                             │
│ 将用户空间 UID 转换为内核 kuid_t                │
│ 若无效 → 返回 -EINVAL                           │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ prepare_creds() → 分配新 cred                    │
│ 若内存不足 → 返回 -ENOMEM                        │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ 权限检查:                                        │
│ ├─ 有 CAP_SETUID?  → 允许任意设置               │
│ └─ euid 是否是 ruid/euid/suid 之一?             │
│     ├─ 是 → 允许                                 │
│     └─ 否 → -EPERM                               │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ 更新凭证与后处理:                                │
│ ├─ 若 ruid 改变 → set_user(new)                 │
│ ├─ new->suid = new->euid  (euid 被改变时)       │
│ ├─ new->fsuid = new->euid                       │
│ ├─ security_task_fix_setuid() → LSM 检查        │
│ ├─ set_cred_ucounts()                           │
│ └─ flag_nproc_exceeded()                        │
└─────────────────────────────────────────────────┘
    │
    v
commit_creds(new) → 返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效 UID | `euid` 在当前用户命名空间中无效 |
| `EPERM` | 权限不足 | 非特权进程尝试设置非当前 ruid/euid/suid 的值 |
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
    uid_t original_euid = geteuid();

    printf("Original EUID: %d\n", original_euid);

    /* 尝试临时切换有效 UID 为当前实际 UID */
    if (seteuid(getuid()) < 0) {
        perror("seteuid failed");
        return 1;
    }

    printf("After seteuid: EUID=%d\n", geteuid());

    /* 恢复原始有效 UID */
    if (seteuid(original_euid) < 0) {
        perror("seteuid restore");
        return 1;
    }

    printf("Restored EUID: %d\n", geteuid());
    return 0;
}
```

### 7.2 特权降级模式

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/capability.h>

int main(void)
{
    /* 假设这是 setuid root 程序 */
    uid_t ruid = getuid();   /* 实际用户（普通用户） */
    uid_t euid = geteuid();  /* 有效用户（root） */

    printf("Before: RUID=%d, EUID=%d\n", ruid, euid);

    /* 降级：将 EUID 设为 RUID，暂时放弃 root 权限 */
    if (seteuid(ruid) < 0) {
        perror("seteuid drop privs");
        return 1;
    }

    printf("After drop: EUID=%d (running as user)\n", geteuid());

    /* 执行需要 root 权限的操作时再提升 */
    /* 注意：由于 SUID 仍为 0，可以恢复 */
    if (seteuid(0) < 0) {
        perror("seteuid regain privs");
        return 1;
    }

    printf("After regain: EUID=%d (back to root)\n", geteuid());

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setreuid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)