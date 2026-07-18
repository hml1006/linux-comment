# setegid 系统调用分析

## 1. 概述

`setegid` 设置当前进程的有效组 ID（effective GID）。`setegid` 在 glibc 中实现为 `setregid(-1, egid)` 的封装，没有独立的系统调用入口。非特权进程只能将 egid 设置为当前实际 GID、有效 GID 或保存的 GID 之一。

**原型：**

```c
// 在 glibc 中实现，非独立 syscall
// 实际调用: setregid(-1, egid)
int setegid(gid_t egid);
```

**参数：**
- `egid`：新的有效 GID。若为 -1，则保持不变

**返回值：**
- 成功：0
- 失败：-1，并设置 `errno`

## 2. 使用场景

- **临时切换组身份**：在需要特定组权限的操作前后切换
- **文件系统访问权限管理**：临时调整文件访问的组权限
- **特权降级**：setgid 程序临时放弃组特权

## 3. 函数调用栈

```
setegid(egid)                                            // glibc 封装
  └─ setregid(-1, egid)                                  // kernel/sys.c
       └─ __sys_setregid(-1, egid)
            ├─ make_kgid(ns, egid) → 转换内核 kgid_t
            ├─ prepare_creds() → 分配新凭证
            ├─ [egid != -1] 权限检查:
            │    ├─ [有 CAP_SETGID] → 直接设置
            │    └─ [无 CAP_SETGID] → 只能设置为 rgid/egid/sgid 之一
            │        否则 → -EPERM
            ├─ new->sgid = new->egid  (如果 egid 被改变)
            ├─ new->fsgid = new->egid
            ├─ security_task_fix_setgid() → LSM 检查
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
    kuid_t euid;         // effective UID
    kgid_t egid;         // effective GID  ← setegid 修改此字段
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

## 5. 流程图

```
用户态: setegid(egid)
    │
    v
┌─────────────────────────────────────────────────┐
│ make_kgid(ns, egid)                             │
│ 将用户空间 GID 转换为内核 kgid_t                │
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
│ ├─ 有 CAP_SETGID?  → 允许任意设置               │
│ └─ egid 是否是 rgid/egid/sgid 之一?             │
│     ├─ 是 → 允许                                 │
│     └─ 否 → -EPERM                               │
└─────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────┐
│ 更新凭证:                                        │
│ new->egid = kegid                                │
│ new->sgid = new->egid  (egid 被改变时)           │
│ new->fsgid = new->egid                           │
│ security_task_fix_setgid() → LSM 检查            │
└─────────────────────────────────────────────────┘
    │
    v
commit_creds(new) → 返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效 GID | `egid` 在当前用户命名空间中无效 |
| `EPERM` | 权限不足 | 非特权进程尝试设置非当前 rgid/egid/sgid 的值 |
| `ENOMEM` | 内存不足 | 无法分配新的凭证结构体 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    gid_t original_egid = getegid();

    printf("Original EGID: %d\n", original_egid);

    /* 将 EGID 设置为当前 RGID（通常是非特权操作） */
    if (setegid(getgid()) < 0) {
        perror("setegid");
        return 1;
    }

    printf("After setegid: EGID=%d\n", getegid());

    return 0;
}
```

### 7.2 临时切换组身份

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    gid_t saved_egid = getegid();

    printf("Current EGID: %d\n", saved_egid);

    /* 尝试临时切换（需要 CAP_SETGID 或目标 GID 是当前 GID 之一） */
    if (setegid(1000) < 0) {
        perror("setegid failed");
        /* 非特权进程只能切换到 rgid/egid/sgid */
    }

    /* 恢复原始 EGID */
    if (setegid(saved_egid) < 0) {
        perror("setegid restore");
        return 1;
    }

    printf("EGID restored to: %d\n", getegid());
    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setregid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)