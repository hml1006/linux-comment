# setresuid 系统调用分析

## 1. 概述

`setresuid` 设置当前进程的实际 UID、有效 UID 和保存的 UID（real, effective, saved）。传递 -1 表示保留对应值不变。这是最灵活的用户 ID 设置接口，允许精确控制进程的所有三种用户身份。`sudo`、`login` 等安全敏感程序使用此调用来精确管理权限状态。

**原型：**

```c
SYSCALL_DEFINE3(setresuid, uid_t, ruid, uid_t, euid, uid_t, suid)
```

**参数：**
- `ruid`：新的实际 UID。若为 -1，则保持不变
- `euid`：新的有效 UID。若为 -1，则保持不变
- `suid`：新的保存 UID。若为 -1，则保持不变

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **完全控制所有三种 UID**：特权进程精确管理用户权限状态
- **sudo/login 等安全程序**：身份认证后精确设置所有 UID
- **容器运行时**：在用户命名空间内设置 UID 映射

## 3. 函数调用栈

```
setresuid(ruid, euid, suid)                              // kernel/sys.c
  └─ __sys_setresuid(ruid, euid, suid)
       ├─ make_kuid() 转换内核 kuid_t → 无效则 -EINVAL
       ├─ old = current_cred()
       ├─ 检查 no-op: 若所有值都无变化 → 直接返回 0
       ├─ 权限检查: 若 ruid/euid/suid 中有新值且无 CAP_SETUID → -EPERM
       │   (新值: 不等于 old->{uid, euid, suid} 中的任何一个)
       ├─ prepare_creds() → 分配失败则 -ENOMEM
       ├─ 设置 ruid: 若 ruid 改变 → set_user(new) 更新用户计数
       ├─ 设置 euid (若非 -1)
       ├─ 设置 suid (若非 -1)
       ├─ new->fsuid = new->euid
       ├─ security_task_fix_setuid(new, old, LSM_SETID_RES) → LSM 检查
       ├─ set_cred_ucounts() → 更新用户计数
       ├─ flag_nproc_exceeded() → 检查 NPROC 限制
       └─ commit_creds(new)
```

## 4. 关键数据结构

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID       ← 可修改
    kgid_t gid;          // real GID
    kuid_t suid;         // saved UID      ← 可修改
    kgid_t sgid;         // saved GID
    kuid_t euid;         // effective UID  ← 可修改
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations ← 自动同步为 euid
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

## 5. 流程图

```
用户态: setresuid(ruid, euid, suid)
    │
    v
┌─────────────────────────────────────────────────────┐
│ make_kuid() 转换 → 无效 UID → -EINVAL               │
│ no-op 检查 → 全无变化 → 直接返回 0                   │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ 权限检查:                                           │
│ ruid_new = ruid ≠ -1 且 ruid ≠ {uid,euid,suid}?    │
│ euid_new = euid ≠ -1 且 euid ≠ {uid,euid,suid}?    │
│ suid_new = suid ≠ -1 且 suid ≠ {uid,euid,suid}?    │
│                                                     │
│ 若 (ruid_new || euid_new || suid_new) 且            │
│    !CAP_SETUID → -EPERM                             │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ prepare_creds() → 分配新凭证                        │
│ 若失败 → -ENOMEM                                    │
│                                                     │
│ 设置 ruid (若非 -1)                                 │
│   └─ 若 ruid 改变 → set_user(new)                   │
│ 设置 euid (若非 -1)                                 │
│ 设置 suid (若非 -1)                                 │
│ new->fsuid = new->euid                              │
│ security_task_fix_setuid() → LSM 检查                │
│ set_cred_ucounts()                                  │
│ flag_nproc_exceeded()                               │
│ commit_creds(new) → 返回 0                          │
└─────────────────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效 UID | 某个 UID 参数在当前用户命名空间中无效 |
| `EPERM` | 权限不足 | 非特权进程尝试设置新值（非当前 UID 的值） |
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
    uid_t ruid, euid, suid;
    getresuid(&ruid, &euid, &suid);

    printf("Before: RUID=%d, EUID=%d, SUID=%d\n", ruid, euid, suid);

    /* 将 EUID 设为 RUID（需要 CAP_SETUID 或 EUID 已经是 RUID） */
    if (setresuid(-1, ruid, -1) < 0) {
        perror("setresuid");
        return 1;
    }

    getresuid(&ruid, &euid, &suid);
    printf("After:  RUID=%d, EUID=%d, SUID=%d\n", ruid, euid, suid);

    return 0;
}
```

### 7.2 完全重置用户身份

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    /* 需要 CAP_SETUID */
    /* 将所有三个 UID 都设为 1000，完全放弃高权限 */
    if (setresuid(1000, 1000, 1000) < 0) {
        perror("setresuid");
        printf("Note: requires CAP_SETUID unless all values are current\n");
        return 1;
    }

    uid_t r, e, s;
    getresuid(&r, &e, &s);
    printf("After setresuid(1000,1000,1000):\n");
    printf("RUID=%d, EUID=%d, SUID=%d\n", r, e, s);

    /* 注意：SUID 也被设为 1000，无法再恢复 root 权限 */
    if (seteuid(0) < 0)
        printf("Cannot regain root privileges - SUID is no longer 0\n");

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setresuid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)