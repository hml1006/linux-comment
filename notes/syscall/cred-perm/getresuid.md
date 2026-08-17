# getresuid 系统调用分析

## 1. 概述

`getresuid` 获取当前进程的三种用户 ID：实际 UID（real）、有效 UID（effective）和保存的 set-user-ID（saved）。这三个值共同构成了进程的完整用户身份状态，用于权限管理和 setuid 程序的执行。

**原型：**

```c
SYSCALL_DEFINE3(getresuid, uid_t __user *, ruidp, uid_t __user *, euidp, uid_t __user *, suidp)
```

**参数：**
- `ruidp`：指向用户空间 `uid_t` 的指针，用于接收实际 UID
- `euidp`：指向用户空间 `uid_t` 的指针，用于接收有效 UID
- `suidp`：指向用户空间 `uid_t` 的指针，用于接收保存的 UID

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **调试 setuid 程序**：观察三种 UID 的变化
- **安全审计**：完整记录进程的权限状态
- **权限管理工具**：如 `sudo`、`login` 等程序需要精确管理 UID 状态
- **进程身份完整查询**：获取完整的用户身份三元组

## 3. 函数调用栈

```
getresuid(ruidp, euidp, suidp)                           // kernel/sys.c
  ├─ cred = current_cred()                               // 获取当前凭证
  ├─ ruid = from_kuid_munged(cred->user_ns, cred->uid)   // 映射实际 UID
  ├─ euid = from_kuid_munged(cred->user_ns, cred->euid)  // 映射有效 UID
  ├─ suid = from_kuid_munged(cred->user_ns, cred->suid)  // 映射保存的 UID
  ├─ retval = put_user(ruid, ruidp)                      // 写入用户空间
  │   if (retval) → return retval
  ├─ retval = put_user(euid, euidp)
  │   if (retval) → return retval
  └─ return put_user(suid, suidp)                        // 最后一次 put_user 结果
```

## 4. 关键数据结构

### 4.1 struct cred（进程凭证）

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID       → ruidp
    kgid_t gid;          // real GID
    kuid_t suid;         // saved UID      → suidp
    kgid_t sgid;         // saved GID
    kuid_t euid;         // effective UID  → euidp
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

### 4.2 三种 UID 的语义

| UID 类型 | 含义 | 作用 |
|----------|------|------|
| `ruid` (real) | 实际用户 ID | 进程的原始身份，由父进程继承 |
| `euid` (effective) | 有效用户 ID | 用于权限检查，setuid 程序会改变此值 |
| `suid` (saved) | 保存的用户 ID | 保存原始 euid，允许非特权进程在 euid 和 suid 之间切换 |

## 5. 流程图

```
用户态: getresuid(&ruid, &euid, &suid)
    │
    v
┌─────────────────────────────────────────────────────┐
│ current_cred() → cred                               │
│                                                     │
│ ruid = from_kuid_munged(cred->user_ns, cred->uid)   │
│ euid = from_kuid_munged(cred->user_ns, cred->euid)  │
│ suid = from_kuid_munged(cred->user_ns, cred->suid)  │
│                                                     │
│ put_user(ruid, ruidp)  ──── 失败? ──→ 返回错误码     │
│ put_user(euid, euidp)  ──── 失败? ──→ 返回错误码     │
│ put_user(suid, suidp)  ──── 返回结果                 │
└─────────────────────────────────────────────────────┘
    │
    v
返回 0 (成功) 或 负错误码 (失败)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EFAULT` | 用户空间指针无效 | `ruidp`、`euidp` 或 `suidp` 指向不可写地址 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    uid_t ruid, euid, suid;

    if (getresuid(&ruid, &euid, &suid) < 0) {
        perror("getresuid");
        return 1;
    }

    printf("Real UID:      %d\n", ruid);
    printf("Effective UID: %d\n", euid);
    printf("Saved UID:     %d\n", suid);

    return 0;
}
```

### 7.2 检测 setuid 程序状态

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    uid_t ruid, euid, suid;

    if (getresuid(&ruid, &euid, &suid) < 0) {
        perror("getresuid");
        return 1;
    }

    printf("RUID=%d EUID=%d SUID=%d\n", ruid, euid, suid);

    if (ruid != euid) {
        printf("Effective UID differs from Real UID\n");
        printf("This is a setuid binary or UID was changed\n");
    }

    if (suid == 0 && euid != 0)
        printf("Saved root UID - can restore privileges via seteuid(0)\n");

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)