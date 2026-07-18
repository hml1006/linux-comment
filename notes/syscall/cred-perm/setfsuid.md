# setfsuid 系统调用分析

## 1. 概述

`setfsuid` 设置当前进程的文件系统用户 ID（filesystem UID）。fsuid 专门用于文件系统访问权限检查，通常与有效 UID 一致。当通过 `setfsuid` 显式设置后，fsuid 可以独立于 euid 变化，这在 NFS 服务器等需要临时改变文件访问身份的场景中非常有用。

**原型：**

```c
SYSCALL_DEFINE1(setfsuid, uid_t, fsuid)
```

**参数：**
- `fsuid`：新的文件系统 UID

**返回值：**
- 成功：返回之前的 fsuid（始终返回旧值，即便设置失败）
- 失败：返回之前的 fsuid（不返回负值，调用者需通过比较新旧值判断是否成功）

## 2. 使用场景

- **NFS 服务器**：模拟客户端身份进行文件访问
- **文件系统权限切换**：临时以不同身份执行文件操作
- **用户空间文件系统（FUSE）**：以特定用户身份执行 I/O 操作

## 3. 函数调用栈

```
setfsuid(fsuid)                                          // kernel/sys.c
  └─ __sys_setfsuid(fsuid)
       ├─ old = current_cred()
       ├─ old_fsuid = from_kuid_munged(old->user_ns, old->fsuid)
       ├─ kuid = make_kuid(old->user_ns, fsuid)
       │   └─ 若 !uid_valid(kuid) → 返回 old_fsuid
       ├─ prepare_creds() → new
       │   └─ 若 NULL → 返回 old_fsuid
       ├─ 权限检查: kuid 必须是 ruid/euid/suid/fsuid 之一或有 CAP_SETUID
       │   如果通过且 fsuid 有变化:
       │   ├─ new->fsuid = kuid
       │   ├─ security_task_fix_setuid(new, old, LSM_SETID_FS)
       │   │   └─ 若通过 → commit_creds(new) → 返回 old_fsuid
       │   └─ 若 LSM 拒绝 → abort_creds(new) → 返回 old_fsuid
       └─ 权限检查未通过 → abort_creds(new) → 返回 old_fsuid
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
    kuid_t fsuid;        // UID for VFS operations  ← setfsuid 修改此字段
    kgid_t fsgid;        // GID for VFS operations
    // ...
};
```

## 5. 流程图

```
用户态: setfsuid(fsuid)
    │
    v
┌─────────────────────────────────────────────────────┐
│ make_kuid(ns, fsuid)                                │
│ 若 UID 无效 → 返回 old_fsuid (不报错)               │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ prepare_creds()                                     │
│ 若内存不足 → 返回 old_fsuid (不报错)                │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ 权限检查:                                           │
│ kuid ∈ {ruid, euid, suid, fsuid} 或有 CAP_SETUID?  │
│ ├─ 否 → abort_creds, 返回 old_fsuid                 │
│ └─ 是 → 继续                                       │
│       │                                             │
│       ├─ kuid == old->fsuid? → 无变化               │
│       │   abort_creds, 返回 old_fsuid               │
│       └─ 有变化 → new->fsuid = kuid                 │
│             │                                       │
│             └─ security_task_fix_setuid() → LSM 检查│
│                  ├─ 通过 → commit_creds(new)        │
│                  └─ 拒绝 → abort_creds(new)          │
└─────────────────────────────────────────────────────┘
    │
    v
返回 old_fsuid (始终返回旧值)
```

## 6. 错误处理

`setfsuid` 的返回值设计特殊——始终返回旧 fsuid，不通过负值表示错误。调用者需要自行判断设置是否生效。

| 情况 | 返回值 | 说明 |
|------|--------|------|
| 成功修改 | 旧 fsuid | 新值已生效，`geteuid()` 可能不变但 `getfsuid()` 为新值 |
| UID 无效 | 旧 fsuid | `make_kuid()` 返回无效值 |
| 内存不足 | 旧 fsuid | `prepare_creds()` 分配失败 |
| 权限不足 | 旧 fsuid | 新 UID 不在 {ruid, euid, suid, fsuid} 中且无 CAP_SETUID |
| LSM 拒绝 | 旧 fsuid | SELinux 等安全模块阻止 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    uid_t old_fsuid = setfsuid((uid_t)-1);
    printf("Original FSUID: %d\n", old_fsuid);

    /* 设置 fsuid 为当前有效 UID */
    uid_t new_fsuid = setfsuid(geteuid());
    if (new_fsuid == old_fsuid && geteuid() != old_fsuid) {
        /* 返回值相同可能表示未生效，需进一步检查 */
        printf("FSUID unchanged\n");
    } else {
        printf("FSUID change attempted, old was: %d\n", new_fsuid);
    }

    return 0;
}
```

### 7.2 NFS 服务器模拟身份

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

/* 模拟 NFS 服务器在处理客户端请求时的身份切换 */
void handle_nfs_request(uid_t client_uid)
{
    uid_t old_fsuid;

    /* 切换到客户端 UID 进行文件访问 */
    old_fsuid = setfsuid(client_uid);

    /* 在此执行文件系统操作 */
    /* ... */

    /* 恢复原始 fsuid */
    setfsuid(old_fsuid);
}

int main(void)
{
    printf("NFS server FSUID: %d\n", setfsuid((uid_t)-1));
    handle_nfs_request(1000);
    printf("After request, FSUID: %d\n", setfsuid((uid_t)-1));
    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setfsuid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)