# setfsgid 系统调用分析

## 1. 概述

`setfsgid` 设置当前进程的文件系统组 ID（filesystem GID）。fsgid 专门用于文件系统组权限检查，通常与有效 GID 一致。当通过 `setfsgid` 显式设置后，fsgid 可以独立于 egid 变化，这在 NFS 服务器等需要临时改变文件访问组身份的场景中非常有用。

**原型：**

```c
SYSCALL_DEFINE1(setfsgid, gid_t, fsgid)
```

**参数：**
- `fsgid`：新的文件系统 GID

**返回值：**
- 成功：返回之前的 fsgid（始终返回旧值，即便设置失败）
- 失败：返回之前的 fsgid（不返回负值，调用者需通过比较新旧值判断是否成功）

## 2. 使用场景

- **NFS 服务器**：模拟客户端组身份进行文件访问
- **文件系统权限切换**：临时以不同组身份执行文件操作
- **用户空间文件系统（FUSE）**：以特定组身份执行 I/O 操作

## 3. 函数调用栈

```
setfsgid(fsgid)                                          // kernel/sys.c
  └─ __sys_setfsgid(fsgid)
       ├─ old = current_cred()
       ├─ old_fsgid = from_kgid_munged(old->user_ns, old->fsgid)
       ├─ kgid = make_kgid(old->user_ns, fsgid)
       │   └─ 若 !gid_valid(kgid) → 返回 old_fsgid
       ├─ prepare_creds() → new
       │   └─ 若 NULL → 返回 old_fsgid
       ├─ 权限检查: kgid 必须是 rgid/egid/sgid/fsgid 之一或有 CAP_SETGID
       │   如果通过且 fsgid 有变化:
       │   ├─ new->fsgid = kgid
       │   ├─ security_task_fix_setgid(new, old, LSM_SETID_FS)
       │   │   └─ 若通过 → commit_creds(new) → 返回 old_fsgid
       │   └─ 若 LSM 拒绝 → abort_creds(new) → 返回 old_fsgid
       └─ 权限检查未通过 → abort_creds(new) → 返回 old_fsgid
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
    kgid_t fsgid;        // GID for VFS operations  ← setfsgid 修改此字段
    // ...
};
```

## 5. 流程图

```
用户态: setfsgid(fsgid)
    │
    v
┌─────────────────────────────────────────────────────┐
│ make_kgid(ns, fsgid)                                │
│ 若 GID 无效 → 返回 old_fsgid (不报错)               │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ prepare_creds()                                     │
│ 若内存不足 → 返回 old_fsgid (不报错)                │
└─────────────────────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────────────────────┐
│ 权限检查:                                           │
│ kgid ∈ {rgid, egid, sgid, fsgid} 或有 CAP_SETGID?  │
│ ├─ 否 → abort_creds, 返回 old_fsgid                 │
│ └─ 是 → 继续                                       │
│       │                                             │
│       ├─ kgid == old->fsgid? → 无变化               │
│       │   abort_creds, 返回 old_fsgid               │
│       └─ 有变化 → new->fsgid = kgid                 │
│             │                                       │
│             └─ security_task_fix_setgid() → LSM 检查│
│                  ├─ 通过 → commit_creds(new)        │
│                  └─ 拒绝 → abort_creds(new)          │
└─────────────────────────────────────────────────────┘
    │
    v
返回 old_fsgid (始终返回旧值)
```

## 6. 错误处理

`setfsgid` 的返回值设计特殊——始终返回旧 fsgid，不通过负值表示错误。

| 情况 | 返回值 | 说明 |
|------|--------|------|
| 成功修改 | 旧 fsgid | 新值已生效 |
| GID 无效 | 旧 fsgid | `make_kgid()` 返回无效值 |
| 内存不足 | 旧 fsgid | `prepare_creds()` 分配失败 |
| 权限不足 | 旧 fsgid | 新 GID 不在 {rgid, egid, sgid, fsgid} 中且无 CAP_SETGID |
| LSM 拒绝 | 旧 fsgid | SELinux 等安全模块阻止 |

## 7. 使用示例

### 7.1 基础用法

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

int main(void)
{
    gid_t old_fsgid = setfsgid((gid_t)-1);
    printf("Original FSGID: %d\n", old_fsgid);

    /* 设置 fsgid 为当前有效 GID */
    gid_t ret = setfsgid(getegid());
    printf("setfsgid returned: %d (old FSGID)\n", ret);

    return 0;
}
```

### 7.2 NFS 服务器模拟组身份

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

/* 模拟 NFS 服务器在处理客户端请求时的组身份切换 */
void handle_nfs_request(gid_t client_gid)
{
    gid_t old_fsgid;

    /* 切换到客户端 GID 进行文件访问 */
    old_fsgid = setfsgid(client_gid);

    /* 在此执行文件系统操作 */
    /* ... */

    /* 恢复原始 fsgid */
    setfsgid(old_fsgid);
}

int main(void)
{
    printf("NFS server FSGID: %d\n", setfsgid((gid_t)-1));
    handle_nfs_request(1000);
    printf("After request, FSGID: %d\n", setfsgid((gid_t)-1));
    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/sys.c`（`__sys_setfsgid` 实现）
- 凭证定义：`include/linux/cred.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)