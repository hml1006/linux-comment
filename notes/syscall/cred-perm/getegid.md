# getegid 系统调用分析

## 1. 概述

`getegid` 获取当前进程的有效组 ID（effective GID）。有效 GID 用于决定进程在访问文件和其他资源时的组权限。对于设置了 setgid 位的可执行文件，有效 GID 可能与实际 GID 不同。

**原型：**

```c
SYSCALL_DEFINE0(getegid)
```

**参数：** 无

**返回值：**
- 成功：当前进程的有效组 ID（`gid_t`），已映射到当前用户命名空间
- 失败：不返回错误（始终成功）

## 2. 使用场景

- **权限检查**：确定进程的实际组访问权限
- **审计跟踪**：记录执行操作的有效组身份
- **调试 setgid 程序**：检测有效组是否与实际组不同
- **安全感知应用**：确认当前生效的组权限

## 3. 函数调用栈

```
getegid()                                                // kernel/sys.c
  └─ from_kgid_munged(current_user_ns(), current_egid())
       ├─ current_egid() → current->cred->egid          // 读取凭证中的有效 GID
       └─ from_kgid_munged() → 将内核 kgid_t 映射到用户命名空间
                               (若 GID 在命名空间外 → 返回溢出 GID 65534)
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
    kgid_t egid;         // effective GID  ← getegid 读取此字段
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

### 4.2 内核 GID 类型

```c
// include/linux/uidgid.h
typedef struct {
    gid_t val;           // 内核命名空间内的 GID 值
} kgid_t;

// from_kgid_munged() 将 kgid_t 映射到用户命名空间
// 若 GID 在命名空间中不可见，返回 (gid_t)65534 (overflowuid)
```

## 5. 流程图

```
用户态: getegid()
    │
    v
┌──────────────────────────────────────┐
│ current_egid()                       │
│ → current->cred->egid (内核 kgid_t)  │
└──────────────────────────────────────┘
    │
    v
┌──────────────────────────────────────┐
│ from_kgid_munged(user_ns, egid)      │
│ 将 kgid_t 映射到当前用户命名空间     │
│ 若 GID 在命名空间外 → 返回 65534     │
└──────────────────────────────────────┘
    │
    v
返回 gid_t 值给用户空间
```

## 6. 错误处理

`getegid` 是只读操作，始终成功返回，不产生错误码。

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    gid_t egid = getegid();
    printf("Effective GID: %d\n", egid);
    return 0;
}
```

### 7.2 检测 setgid 程序

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    gid_t gid = getgid();
    gid_t egid = getegid();

    if (gid != egid)
        printf("This program has setgid bit set!\n");
    printf("Real GID: %d, Effective GID: %d\n", gid, egid);

    return 0;
}
```

### 7.3 命名空间感知

```c
/* 在容器或用户命名空间中，getegid() 返回映射后的值 */
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    printf("Effective GID in current namespace: %d\n", getegid());
    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`
- 凭证定义：`include/linux/cred.h`
- GID 类型定义：`include/linux/uidgid.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)