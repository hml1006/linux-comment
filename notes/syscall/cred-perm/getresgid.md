# getresgid 系统调用分析

## 1. 概述

`getresgid` 获取当前进程的三种组 ID：实际 GID（real）、有效 GID（effective）和保存的 set-group-ID（saved）。这三个值共同构成了进程的完整组身份状态，用于组权限管理和 setgid 程序的执行。

**原型：**

```c
SYSCALL_DEFINE3(getresgid, gid_t __user *, rgidp, gid_t __user *, egidp, gid_t __user *, sgidp)
```

**参数：**
- `rgidp`：指向用户空间 `gid_t` 的指针，用于接收实际 GID
- `egidp`：指向用户空间 `gid_t` 的指针，用于接收有效 GID
- `sgidp`：指向用户空间 `gid_t` 的指针，用于接收保存的 GID

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **调试 setgid 程序**：观察三种 GID 的变化
- **安全审计**：完整记录进程的组权限状态
- **权限管理工具**：精确管理组身份状态
- **进程组身份完整查询**：获取完整的组身份三元组

## 3. 函数调用栈

```
getresgid(rgidp, egidp, sgidp)                           // kernel/sys.c
  ├─ cred = current_cred()                               // 获取当前凭证
  ├─ rgid = from_kgid_munged(cred->user_ns, cred->gid)   // 映射实际 GID
  ├─ egid = from_kgid_munged(cred->user_ns, cred->egid)  // 映射有效 GID
  ├─ sgid = from_kgid_munged(cred->user_ns, cred->sgid)  // 映射保存的 GID
  ├─ retval = put_user(rgid, rgidp)                      // 写入用户空间
  │   if (retval) → return retval
  ├─ retval = put_user(egid, egidp)
  │   if (retval) → return retval
  └─ return put_user(sgid, sgidp)                        // 最后一次 put_user 结果
```

## 4. 关键数据结构

### 4.1 struct cred（进程凭证）

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID
    kgid_t gid;          // real GID       → rgidp
    kuid_t suid;         // saved UID
    kgid_t sgid;         // saved GID      → sgidp
    kuid_t euid;         // effective UID
    kgid_t egid;         // effective GID  → egidp
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

### 4.2 三种 GID 的语义

| GID 类型 | 含义 | 作用 |
|----------|------|------|
| `rgid` (real) | 实际组 ID | 进程的原始组身份，由父进程继承 |
| `egid` (effective) | 有效组 ID | 用于组权限检查，setgid 程序会改变此值 |
| `sgid` (saved) | 保存的组 ID | 保存原始 egid，允许非特权进程在 egid 和 sgid 之间切换 |

## 5. 流程图

```
用户态: getresgid(&rgid, &egid, &sgid)
    │
    v
┌─────────────────────────────────────────────────────┐
│ current_cred() → cred                               │
│                                                     │
│ rgid = from_kgid_munged(cred->user_ns, cred->gid)   │
│ egid = from_kgid_munged(cred->user_ns, cred->egid)  │
│ sgid = from_kgid_munged(cred->user_ns, cred->sgid)  │
│                                                     │
│ put_user(rgid, rgidp)  ──── 失败? ──→ 返回错误码     │
│ put_user(egid, egidp)  ──── 失败? ──→ 返回错误码     │
│ put_user(sgid, sgidp)  ──── 返回结果                 │
└─────────────────────────────────────────────────────┘
    │
    v
返回 0 (成功) 或 负错误码 (失败)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EFAULT` | 用户空间指针无效 | `rgidp`、`egidp` 或 `sgidp` 指向不可写地址 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    gid_t rgid, egid, sgid;

    if (getresgid(&rgid, &egid, &sgid) < 0) {
        perror("getresgid");
        return 1;
    }

    printf("Real GID:      %d\n", rgid);
    printf("Effective GID: %d\n", egid);
    printf("Saved GID:     %d\n", sgid);

    return 0;
}
```

### 7.2 检测 setgid 程序状态

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    gid_t rgid, egid, sgid;

    if (getresgid(&rgid, &egid, &sgid) < 0) {
        perror("getresgid");
        return 1;
    }

    printf("RGID=%d EGID=%d SGID=%d\n", rgid, egid, sgid);

    if (rgid != egid) {
        printf("Effective GID differs from Real GID\n");
        printf("This is a setgid binary\n");
    }

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)