# getgid 系统调用分析

## 1. 概述

`getgid` 获取当前进程的实际组 ID（real GID）。实际 GID 标识进程所属的用户组，在进程创建时从父进程继承。实际 GID 不会因 setgid 位而改变，是进程的原始组身份标识。

**原型：**

```c
SYSCALL_DEFINE0(getgid)
```

**参数：** 无

**返回值：**
- 成功：当前进程的实际组 ID（`gid_t`），已映射到当前用户命名空间
- 失败：不返回错误（始终成功）

## 2. 使用场景

- **进程身份识别**：确认进程的原始组归属
- **组权限基线**：与有效 GID 对比，判断是否处于提权状态
- **日志记录**：记录操作者的原始组身份
- **审计跟踪**：确定进程的初始组归属

## 3. 函数调用栈

```
getgid()                                                 // kernel/sys.c
  └─ from_kgid_munged(current_user_ns(), current_gid())
       ├─ current_gid() → current->cred->gid            // 读取凭证中的实际 GID
       └─ from_kgid_munged() → 将内核 kgid_t 映射到用户命名空间
                                (若 GID 在命名空间外 → 返回溢出 GID 65534)
```

## 4. 关键数据结构

### 4.1 struct cred（进程凭证）

```c
// include/linux/cred.h
struct cred {
    kuid_t uid;          // real UID
    kgid_t gid;          // real GID  ← getgid 读取此字段
    kuid_t suid;         // saved UID
    kgid_t sgid;         // saved GID
    kuid_t euid;         // effective UID
    kgid_t egid;         // effective GID
    kuid_t fsuid;        // UID for VFS operations
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

### 4.2 内核 GID 类型与映射

```c
// include/linux/uidgid.h
typedef struct {
    gid_t val;
} kgid_t;

// from_kgid_munged(ns, kgid) 执行命名空间映射：
// 1. 若 kgid 在 ns 的 kuid/kgid 映射表中 → 返回映射后的 gid_t
// 2. 若不在映射表中 → 返回 (gid_t)65534 (overflowgid)
```

## 5. 流程图

```
用户态: getgid()
    │
    v
┌─────────────────────────────────────┐
│ current_gid()                       │
│ → current->cred->gid (内核 kgid_t)  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ from_kgid_munged(user_ns, gid)      │
│ 将 kgid_t 映射到当前用户命名空间    │
│ 若 GID 在命名空间外 → 返回 65534    │
└─────────────────────────────────────┘
    │
    v
返回 gid_t 值给用户空间
```

## 6. 错误处理

`getgid` 是只读操作，始终成功返回，不产生错误码。

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    gid_t gid = getgid();
    gid_t egid = getegid();

    printf("Real GID: %d, Effective GID: %d\n", gid, egid);
    return 0;
}
```

### 7.2 配合 setgid 检查

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    gid_t gid = getgid();
    gid_t egid = getegid();

    if (gid != egid)
        printf("Program is running with setgid privilege\n");
    printf("Real GID: %d, Effective GID: %d\n", gid, egid);

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`
- 凭证定义：`include/linux/cred.h`
- GID 类型定义：`include/linux/uidgid.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)