# getuid 系统调用分析

## 1. 概述

获取当前进程的实际用户 ID（real UID）。此系统调用是获取进程身份标识的最基本操作之一，返回的 UID 值经过用户命名空间映射。

**原型：**

```c
SYSCALL_DEFINE0(getuid)
```

**参数：** 无

**返回值：** 当前进程的实际用户 ID（`uid_t`）

## 2. 使用场景

- 进程身份识别
- 用户权限检查
- 日志记录（谁在执行什么操作）

## 3. 函数调用栈

```
getuid()                                                 // kernel/sys.c
  └─ from_kuid_munged(current_user_ns(), current_uid())
       ├─ current_uid() → current->cred->uid            // 读取凭证中的实际 UID
       └─ from_kuid_munged() → 将内核 UID 映射到用户命名空间
```

## 4. 关键数据结构

### 4.1 struct cred（进程凭证）

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID of the task
    kuid_t gid;          // real GID of the task
    kuid_t suid;         // saved UID (setuid 保留)
    kgid_t sgid;         // saved GID
    kuid_t euid;         // effective UID
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ... (capabilities, keys, LSM, etc.)
};
```

## 5. 流程图

```
用户态: getuid()
    │
    v
┌─────────────────────────────────────┐
│ current_uid()                       │
│ → current->cred->uid (内核 kuid_t)  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ from_kuid_munged(user_ns, uid)      │
│ 将 kuid_t 映射到当前用户命名空间    │
│ 若 UID 在命名空间外 → 返回 overflow │
│ uid (65534)                         │
└─────────────────────────────────────┘
    │
    v
返回 uid_t 值
```

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    uid_t uid = getuid();
    uid_t euid = geteuid();

    printf("Real UID:      %d\n", uid);
    printf("Effective UID: %d\n", euid);

    if (uid == 0)
        printf("Running as root\n");
    else
        printf("Running as user %d\n", uid);

    return 0;
}
```

## 7. 参考

- 源码位置：`kernel/sys.c`
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)