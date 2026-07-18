# setgid 系统调用分析

## 1. 概述

`setgid` 设置当前进程的组 ID。`setgid` 遵循 POSIX 语义，根据调用者是否拥有 `CAP_SETGID` 能力决定行为。特权进程可以设置所有 GID 为任意值；非特权进程只能设置有效 GID 和文件系统 GID 为实际 GID 或保存的 GID。

**原型：**

```c
SYSCALL_DEFINE1(setgid, gid_t, gid)
```

**参数：**
- `gid`：要设置的组 ID

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **特权进程放弃组权限**：root 进程切换为普通组身份
- **组身份切换**：setgid 程序在需要时切换组
- **守护进程降权**：以高权限启动后降至低权限组

## 3. 函数调用栈

```
setgid(gid)                                              // kernel/sys.c
  └─ __sys_setgid(gid)
       ├─ make_kgid(ns, gid) → 转换为内核 kgid_t
       │   └─ 若 !gid_valid(kgid) → 返回 -EINVAL
       ├─ prepare_creds() → 分配新凭证
       │   └─ 若 NULL → 返回 -ENOMEM
       ├─ 权限检查:
       │    ├─ [有 CAP_SETGID] → 设置所有 GID:
       │    │    new->gid = new->egid = new->sgid = new->fsgid = kgid
       │    └─ [无 CAP_SETGID] → 只能设置 egid/fsgid:
       │         if (gid_eq(kgid, old->gid) || gid_eq(kgid, old->sgid))
       │             new->egid = new->fsgid = kgid
       │         else → -EPERM
       ├─ security_task_fix_setgid(new, old, LSM_SETID_ID) → LSM 检查
       └─ commit_creds(new)
```

## 4. 关键数据结构

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID
    kgid_t gid;          // real GID
    kuid_t suid;         // saved UID
    kgid_t sgid;         // saved GID
    kuid_t euid;         // effective UID
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

## 5. 流程图

```
用户态: setgid(gid)
    │
    v
┌─────────────────────────────────────────────────┐
│ make_kgid(ns, gid) → 若无效 GID → -EINVAL      │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ prepare_creds() → 若内存不足 → -ENOMEM           │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ 权限检查:                                        │
│                                                 │
│ 有 CAP_SETGID?                                   │
│ ├─ 是 → 设置全部 GID:                            │
│ │   new->gid = new->egid = kgid                 │
│ │   new->sgid = new->fsgid = kgid               │
│ │                                               │
│ └─ 否 → kgid 是 rgid 或 sgid?                   │
│       ├─ 是 → 设置 egid 和 fsgid                │
│       │   new->egid = new->fsgid = kgid         │
│       └─ 否 → -EPERM                            │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ security_task_fix_setgid() → LSM 检查            │
│ ├─ 通过 → commit_creds(new) → 返回 0            │
│ └─ 拒绝 → abort_creds(new) → 返回错误码         │
└─────────────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效 GID | `gid` 在当前用户命名空间中无效 |
| `EPERM` | 权限不足 | 非特权进程尝试设置非当前 rgid/sgid 的值 |
| `ENOMEM` | 内存不足 | 无法分配新的凭证结构体 |

## 7. 使用示例

### 7.1 特权进程降权

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    gid_t gid = getgid();
    printf("Current GID: %d\n", gid);

    /* 以 root 运行时，切换到组 1000 */
    if (gid == 0) {
        if (setgid(1000) < 0) {
            perror("setgid");
            return 1;
        }
        printf("After setgid(1000): GID=%d, EGID=%d\n",
               getgid(), getegid());
    } else {
        /* 非特权进程只能设置 egid 为 rgid 或 sgid */
        if (setgid(gid) < 0) {
            perror("setgid");
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
    /* 非特权进程：尝试将 GID 改为 1000 */
    if (setgid(1000) < 0) {
        perror("setgid");
        /* 只有 rgid 或 sgid 为 1000 时才会成功 */
        printf("Note: need CAP_SETGID or target GID must be rgid/sgid\n");
    }

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setgid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)