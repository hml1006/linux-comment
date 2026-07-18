# setuid 系统调用分析

## 1. 概述

`setuid` 设置当前进程的用户 ID。`setuid` 遵循 POSIX 语义，根据调用者的权限决定如何设置 UID。对于特权进程（拥有 `CAP_SETUID`），可以设置所有 UID 为任意值；对于非特权进程，只能设置有效 UID 为实际 UID 或保存的 UID。这是 Linux 权限管理中最核心的系统调用之一。

**原型：**

```c
SYSCALL_DEFINE1(setuid, uid_t, uid)
```

**参数：**
- `uid`：要设置的用户 ID

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **特权进程放弃权限**：root 进程切换为普通用户身份（如 `su`、`login` 守护进程）
- **用户身份切换**：setuid 程序在需要时切换用户
- **守护进程降权**：以 root 启动后降至普通用户权限运行
- **容器运行时**：在用户命名空间内管理 UID

## 3. 函数调用栈

```
setuid(uid)                                              // kernel/sys.c
  └─ __sys_setuid(uid)
       ├─ make_kuid(ns, uid) → 转换为内核 kuid_t
       │   └─ 若 !uid_valid(kuid) → 返回 -EINVAL
       ├─ prepare_creds() → 分配新凭证
       │   └─ 若 NULL → 返回 -ENOMEM
       ├─ 权限检查:
       │    ├─ [有 CAP_SETUID] →
       │    │    new->suid = new->uid = kuid
       │    │    if (!uid_eq(kuid, old->uid)) → set_user(new)
       │    └─ [无 CAP_SETUID] →
       │         if (!uid_eq(kuid, old->uid) && !uid_eq(kuid, new->suid))
       │             → -EPERM
       │         注意: 这里 new->suid 是 prepare_creds 拷贝的旧 suid
       ├─ new->fsuid = new->euid = kuid
       ├─ security_task_fix_setuid(new, old, LSM_SETID_ID) → LSM 检查
       ├─ set_cred_ucounts() → 更新用户计数
       ├─ flag_nproc_exceeded() → 检查 NPROC 限制
       └─ commit_creds(new)
```

## 4. 关键数据结构

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID       ← 特权进程可修改
    kgid_t gid;          // real GID
    kuid_t suid;         // saved UID      ← 特权进程可修改
    kgid_t sgid;         // saved GID
    kuid_t euid;         // effective UID  ← 总是被修改为 kuid
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations ← 总是被同步为 kuid
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

## 5. 流程图

```
用户态: setuid(uid)
    │
    v
┌─────────────────────────────────────────────────┐
│ make_kuid(ns, uid) → 若无效 UID → -EINVAL      │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ prepare_creds() → 若内存不足 → -ENOMEM           │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ 权限检查:                                            │
│                                                     │
│ 有 CAP_SETUID?                                       │
│ ├─ 是 → 全部设置:                                   │
│ │   new->suid = new->uid = kuid                     │
│ │   if (kuid != old->uid) → set_user(new)           │
│ │   new->euid = new->fsuid = kuid                   │
│ │                                                   │
│ └─ 否 → 非特权:                                    │
│       if (kuid == old->uid || kuid == old->suid)    │
│           new->euid = new->fsuid = kuid             │
│       else → -EPERM                                 │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ security_task_fix_setuid() → LSM 检查            │
│ set_cred_ucounts()                              │
│ flag_nproc_exceeded()                           │
│ commit_creds(new) → 返回 0                       │
└─────────────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效 UID | `uid` 在当前用户命名空间中无效 |
| `EPERM` | 权限不足 | 非特权进程尝试设置非当前 uid/suid 的值 |
| `ENOMEM` | 内存不足 | 无法分配新的凭证结构体 |
| `EAGAIN` | 资源限制 | `set_user()` 失败（如 `RLIMIT_NPROC` 限制） |

## 7. 使用示例

### 7.1 特权进程降权

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    uid_t uid = getuid();
    printf("Current UID: %d\n", uid);

    if (uid == 0) {
        /* 以 root 运行时，切换到用户 1000 */
        /* 这将永久放弃 root 权限（所有 UID 都被设置） */
        if (setuid(1000) < 0) {
            perror("setuid");
            return 1;
        }
        printf("After setuid(1000): UID=%d, EUID=%d\n",
               getuid(), geteuid());

        /* 尝试恢复 root 权限 */
        if (setuid(0) < 0) {
            printf("Cannot regain root - all UIDs permanently changed\n");
        }
    } else {
        /* 非特权进程只能设置 euid 为 ruid 或 suid */
        if (setuid(uid) < 0) {
            perror("setuid");
            return 1;
        }
    }

    return 0;
}
```

### 7.2 非特权进程使用

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    /* 非特权进程：尝试将 UID 改为 1000 */
    if (setuid(1000) < 0) {
        perror("setuid");
        /* 只有 ruid 或 suid 为 1000 时才会成功 */
        /* 否则返回 -EPERM */
        printf("Note: need CAP_SETUID or target UID must be ruid/suid\n");
    }

    return 0;
}
```

### 7.3 守护进程典型降权模式

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <pwd.h>

int main(void)
{
    /* 假设以 root 运行 */
    struct passwd *pw = getpwnam("nobody");

    if (!pw) {
        perror("getpwnam");
        return 1;
    }

    printf("Running as root, switching to user 'nobody' (UID=%d)\n",
           pw->pw_uid);

    /* 先初始化需要 root 权限的资源 */
    /* ... */

    /* 永久放弃 root 权限，切换到 nobody 用户 */
    if (setuid(pw->pw_uid) < 0) {
        perror("setuid");
        return 1;
    }

    printf("Now running as UID=%d, EUID=%d\n",
           getuid(), geteuid());

    /* 无法再切换回 root */
    /* 以非特权用户身份继续运行 */

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setuid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)