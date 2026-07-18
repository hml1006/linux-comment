# geteuid 系统调用分析

## 1. 概述

`geteuid` 获取当前进程的有效用户 ID（effective UID）。有效 UID 是 Linux 权限检查的核心依据，用于决定进程在访问文件和其他资源时的权限。对于设置了 setuid 位的可执行文件，有效 UID 可能与实际 UID 不同。

**原型：**

```c
SYSCALL_DEFINE0(geteuid)
```

**参数：** 无

**返回值：**
- 成功：当前进程的有效用户 ID（`uid_t`），已映射到当前用户命名空间
- 失败：不返回错误（始终成功）

## 2. 使用场景

- **权限检查**：确定进程的实际访问权限（内核使用 euid 进行权限判定）
- **审计跟踪**：记录执行操作的有效用户身份
- **调试 setuid 程序**：检测有效 UID 是否与实际 UID 不同
- **安全感知应用**：确认当前生效的权限级别

## 3. 函数调用栈

```
geteuid()                                                // kernel/sys.c
  └─ from_kuid_munged(current_user_ns(), current_euid())
       ├─ current_euid() → current->cred->euid          // 读取凭证中的有效 UID
       └─ from_kuid_munged() → 将内核 kuid_t 映射到用户命名空间
                                (若 UID 在命名空间外 → 返回溢出 UID 65534)
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
    kuid_t euid;         // effective UID  ← geteuid 读取此字段
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

### 4.2 内核 UID 类型

```c
// include/linux/uidgid.h
typedef struct {
    uid_t val;           // 内核命名空间内的 UID 值
} kuid_t;

// from_kuid_munged() 将 kuid_t 映射到用户命名空间
// 若 UID 在命名空间中不可见，返回 (uid_t)65534 (overflowuid)
```

## 5. 流程图

```
用户态: geteuid()
    │
    v
┌──────────────────────────────────────┐
│ current_euid()                       │
│ → current->cred->euid (内核 kuid_t)  │
└──────────────────────────────────────┘
    │
    v
┌──────────────────────────────────────┐
│ from_kuid_munged(user_ns, euid)      │
│ 将 kuid_t 映射到当前用户命名空间     │
│ 若 UID 在命名空间外 → 返回 65534     │
└──────────────────────────────────────┘
    │
    v
返回 uid_t 值给用户空间
```

## 6. 错误处理

`geteuid` 是只读操作，始终成功返回，不产生错误码。

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    uid_t euid = geteuid();
    printf("Effective UID: %d\n", euid);
    return 0;
}
```

### 7.2 检测 setuid 程序

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    uid_t uid = getuid();
    uid_t euid = geteuid();

    printf("Real UID: %d, Effective UID: %d\n", uid, euid);

    if (uid != euid)
        printf("This is a setuid binary (euid differs from uid)!\n");
    else
        printf("Running with normal privileges\n");

    return 0;
}
```

### 7.3 权限级别检查

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    if (geteuid() == 0)
        printf("Running with root privileges (effective UID is 0)\n");
    else
        printf("Running as unprivileged user (effective UID is %d)\n", geteuid());

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`
- 凭证定义：`include/linux/cred.h`
- UID 类型定义：`include/linux/uidgid.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)