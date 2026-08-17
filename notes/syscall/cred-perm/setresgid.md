# setresgid 系统调用分析

## 1. 概述

`setresgid` 设置当前进程的实际 GID、有效 GID 和保存的 GID（real, effective, saved）。传递 -1 表示保留对应值不变。这是最灵活的组 ID 设置接口，允许精确控制进程的所有三种组身份。

**原型：**

```c
SYSCALL_DEFINE3(setresgid, gid_t, rgid, gid_t, egid, gid_t, sgid)
```

**参数：**
- `rgid`：新的实际 GID。若为 -1，则保持不变
- `egid`：新的有效 GID。若为 -1，则保持不变
- `sgid`：新的保存 GID。若为 -1，则保持不变

**返回值：**
- 成功：0
- 失败：返回负的错误码

## 2. 使用场景

- **完全控制所有三种 GID**：特权进程精确管理组权限状态
- **特权程序精确管理组权限状态**：在特权和非特权模式间切换
- **安全敏感应用**：需要精确控制组身份的程序

## 3. 函数调用栈

```
setresgid(rgid, egid, sgid)                              // kernel/sys.c
  └─ __sys_setresgid(rgid, egid, sgid)
       ├─ make_kgid() 转换内核 kgid_t → 无效则 -EINVAL
       ├─ old = current_cred()
       ├─ 检查 no-op: 若所有值都无变化 → 直接返回 0
       ├─ 权限检查: 若 rgid/egid/sgid 中有新值且无 CAP_SETGID → -EPERM
       │   (新值定义为不等于 old->{gid,egid,sgid} 中的任何一个)
       ├─ prepare_creds() → 分配失败则 -ENOMEM
       ├─ 设置 rgid/egid/sgid (若非 -1)
       ├─ new->fsgid = new->egid
       ├─ security_task_fix_setgid(new, old, LSM_SETID_RES) → LSM 检查
       └─ commit_creds(new)
```

## 4. 关键数据结构

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID
    kgid_t gid;          // real GID       ← 可修改
    kuid_t suid;         // saved UID
    kgid_t sgid;         // saved GID      ← 可修改
    kuid_t euid;         // effective UID
    kgid_t egid;         // effective GID  ← 可修改
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations ← 自动同步为 egid
    // ...
};
```

## 5. 流程图

```
用户态: setresgid(rgid, egid, sgid)
    │
    v
┌─────────────────────────────────────────────────────┐
│ make_kgid() 转换 → 无效 GID → -EINVAL               │
│ no-op 检查 → 全无变化 → 直接返回 0                   │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ 权限检查:                                           │
│ rgid_new = rgid ≠ -1 且 rgid ≠ {gid,egid,sgid}?    │
│ egid_new = egid ≠ -1 且 egid ≠ {gid,egid,sgid}?    │
│ sgid_new = sgid ≠ -1 且 sgid ≠ {gid,egid,sgid}?    │
│                                                     │
│ 若 (rgid_new || egid_new || sgid_new) 且            │
│    !CAP_SETGID → -EPERM                             │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ prepare_creds() → 分配新凭证                        │
│ 若失败 → -ENOMEM                                    │
│                                                     │
│ 设置 rgid/egid/sgid (若非 -1)                      │
│ new->fsgid = new->egid                              │
│ security_task_fix_setgid() → LSM 检查                │
│ commit_creds(new) → 返回 0                          │
└─────────────────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效 GID | 某个 GID 参数在当前用户命名空间中无效 |
| `EPERM` | 权限不足 | 非特权进程尝试设置新值（非当前 GID 的值） |
| `ENOMEM` | 内存不足 | 无法分配新的凭证结构体 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    gid_t rgid, egid, sgid;
    getresgid(&rgid, &egid, &sgid);

    printf("Before: RGID=%d, EGID=%d, SGID=%d\n", rgid, egid, sgid);

    /* 将 EUID 设为 RUID（需要 CAP_SETGID 或 EUID 已经是 RUID） */
    if (setresgid(-1, rgid, -1) < 0) {
        perror("setresgid");
        return 1;
    }

    getresgid(&rgid, &egid, &sgid);
    printf("After:  RGID=%d, EGID=%d, SGID=%d\n", rgid, egid, sgid);

    return 0;
}
```

### 7.2 完全重置组身份

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    /* 需要 CAP_SETGID */
    /* 将所有三个 GID 都设为 1000 */
    if (setresgid(1000, 1000, 1000) < 0) {
        perror("setresgid");
        printf("Note: requires CAP_SETGID unless all values are current\n");
        return 1;
    }

    printf("All GIDs set to 1000: RGID=%d, EGID=%d, SGID=%d\n",
           getgid(), getegid(), getresgid(NULL, NULL, NULL) ? 0 : 0);
    /* 注意：getresgid 需要三个有效指针 */
    gid_t r, e, s;
    getresgid(&r, &e, &s);
    printf("RGID=%d, EGID=%d, SGID=%d\n", r, e, s);

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setresgid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)