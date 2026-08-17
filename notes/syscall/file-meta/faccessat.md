# faccessat 系统调用

## 原理与功能

`faccessat` 是 `access` 的 "at" 系列变体，允许相对于目录文件描述符检查文件权限。它比 `access` 更灵活，支持 `AT_FDCWD` 和 `AT_SYMLINK_NOFOLLOW` 等标志。

在 ARM64 架构上，`access` 没有独立的系统调用号，通过 `faccessat`（syscall #48）实现。glibc 封装层将 `access(path, mode)` 转换为 `faccessat(AT_FDCWD, path, mode, 0)`。

### 功能说明

- 相对于目录 fd 检查文件权限
- 支持 `AT_FDCWD` 使用当前工作目录
- 支持 `AT_SYMLINK_NOFOLLOW` 不跟随符号链接
- 使用实际用户/组 ID 检查权限（通过 `access_override_creds` 切换凭证）
- 在 ARM64 上，`access` 和 `faccessat` 都映射到此调用

### 与 access 的关键区别

| 特性 | `access` | `faccessat` |
|------|---------|------------|
| 目录 fd | 无（始终当前工作目录） | 支持 `AT_FDCWD` 或指定 fd |
| 标志位 | 无 | 支持 `AT_SYMLINK_NOFOLLOW` |
| 空路径 | 不支持 | 支持 `AT_EMPTY_PATH` |
| 内核实现 | 通过 `faccessat` 封装 | 直接调用 `do_faccessat` |

## 使用场景

- 相对于指定目录检查文件权限
- 避免 TOCTOU 竞态条件的精确权限检查
- 实现 `access()` 的底层调用
- `setuid` 程序以实际用户身份检查权限

## API 及使用案例

### 函数原型

```c
#include <fcntl.h>
#include <unistd.h>

int faccessat(int dirfd, const char *pathname, int mode, int flags);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `dirfd` | `int` | 目录 fd，`AT_FDCWD` 表示当前工作目录 |
| `pathname` | `const char*` | 文件路径 |
| `mode` | `int` | 检查模式（`F_OK`, `R_OK`, `W_OK`, `X_OK`） |
| `flags` | `int` | 标志位（历史原因，此参数被内核忽略，始终为 0） |

### 注意

`faccessat` 的 `flags` 参数在 Linux 内核实现中被忽略（始终传入 0 到 `do_faccessat`）。如果需要使用 `AT_EACCESS` 或 `AT_SYMLINK_NOFOLLOW` 标志，请使用 `faccessat2`（Linux 5.8+）。

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    // 相对于当前工作目录检查
    if (faccessat(AT_FDCWD, "/etc/passwd", R_OK, 0) == 0)
        printf("文件可读\n");

    // 打开目录后相对于该目录检查
    int dirfd = open("/etc", O_RDONLY);
    if (dirfd >= 0) {
        if (faccessat(dirfd, "passwd", R_OK, 0) == 0)
            printf("/etc/passwd 可读\n");
        close(dirfd);
    }

    return 0;
}
```

## 执行流程

```
用户进程                          内核
    |                               |
    | faccessat(dirfd, path,        |
    |   mode, flags)                |
    |-----> syscall(#48) ---------->|
    |       __arm64_sys_faccessat() |
    |                               |
    |    +----------------------+   |
    |    | do_faccessat()       |   |
    |    | fs/open.c:466        |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | 参数校验:             |   |
    |    | mode & ~S_IRWXO      |   |
    |    | flags & ~(AT_EACCESS |   |
    |    |  | AT_SYMLINK_NOFOLLOW|   |
    |    |  | AT_EMPTY_PATH)    |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | access_need_override |   |
    |    | _creds(flags)        |   |
    |    | 如果无需 AT_EACCESS:  |   |
    |    | 切换 cred→real_cred  |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | filename_lookup()    |   |
    |    | 路径解析              |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | MAY_EXEC && noexec   |   |
    |    | 检查文件系统 noexec   |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | inode_permission()   |   |
    |    | 使用 MAY_ACCESS 标志  |   |
    |    | 检查 inode 权限      |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | 只读文件系统检查:     |   |
    |    | 如果 W_OK 且 FS 只读  |   |
    |    | 返回 -EROFS          |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | 恢复原始 cred        |   |
    |    +---------+------------+   |
    |              |                |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
faccessat(dirfd, pathname, mode, 0)
  └─ syscall(__NR_faccessat, dirfd, pathname, mode, 0)
       └─ __arm64_sys_faccessat()
            └─ do_faccessat(dfd, filename, mode, 0)      // fs/open.c:466
                 ├─ 参数校验: mode & ~S_IRWXO → -EINVAL
                 │            flags & 非法位 → -EINVAL
                 ├─ access_need_override_creds(flags)
                 │    └─ 如果 flags & AT_EACCESS: 不切换（使用有效 ID）
                 │    └─ 否则: 切换 cred 为 real_cred（使用实际 ID）
                 ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 ├─ 如果 (mode & MAY_EXEC) && S_ISREG:
                 │    └─ path_noexec(&path) → -EACCES
                 ├─ inode_permission(idmap, inode, mode | MAY_ACCESS)
                 └─ 如果 (mode & S_IWOTH) && !special_file:
                      └─ __mnt_is_readonly(path.mnt) → -EROFS
```

## 关键数据结构

### struct cred（进程凭证）

```c
// include/linux/cred.h
struct cred {
    kuid_t      uid;          // 实际用户 ID（access 默认使用这个）
    kgid_t      gid;          // 实际组 ID（access 默认使用这个）
    kuid_t      suid;         // 保存的用户 ID
    kgid_t      sgid;         // 保存的组 ID
    kuid_t      euid;         // 有效用户 ID（AT_EACCESS 时使用）
    kgid_t      egid;         // 有效组 ID（AT_EACCESS 时使用）
    kuid_t      fsuid;        // 文件系统用户 ID
    kgid_t      fsgid;        // 文件系统组 ID
    // ...
};
```

### access_need_override_creds 逻辑

```c
// fs/open.c:390
static bool access_need_override_creds(int flags)
{
    const struct cred *cred;

    // AT_EACCESS 标志表示使用有效 ID，无需切换
    if (flags & AT_EACCESS)
        return false;

    // 如果 fsuid/fsgid 与 uid/gid 不同，需要切换
    cred = current_cred();
    if (!uid_eq(cred->fsuid, cred->uid) ||
        !gid_eq(cred->fsgid, cred->gid))
        return true;

    // 如果当前进程没有安全能力集，需要切换
    if (!issecure(SECURE_NO_SETUID_FIXUP))
        return true;

    return false;
}
```

## 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EACCES` | 权限不足 | 请求的访问模式被拒绝，或在 noexec 挂载上检查 X_OK |
| `EFAULT` | 地址错误 | `pathname` 指向非法地址 |
| `EINVAL` | 参数无效 | `mode` 包含非法位，或 `flags` 包含未知标志 |
| `EIO` | I/O 错误 | 读取文件系统时发生 I/O 错误 |
| `ELOOP` | 符号链接循环 | 路径解析遇到过多符号链接 |
| `ENAMETOOLONG` | 路径名过长 | `pathname` 超出 `PATH_MAX` |
| `ENOENT` | 文件不存在 | 路径中的某个分量不存在 |
| `ENOMEM` | 内存不足 | 内核内存分配失败 |
| `ENOTDIR` | 非目录 | 路径中的某个分量不是目录 |
| `EROFS` | 只读文件系统 | 请求 W_OK 但文件系统以只读方式挂载 |
| `ENOTSUP` | 不支持 | `AT_EMPTY_PATH` 用于不支持该标志的文件系统 |

## 备注

- ARM64 系统调用号为 #48
- `faccessat` 的 flags 参数在内核中被忽略（始终为 0），需要使用 `faccessat2`
- 默认使用实际用户/组 ID（与 `access` 相同），这与大多数其他文件操作（使用有效 ID）不同
- 存在 TOCTOU 竞态条件：检查和使用之间文件可能变化
- 内核实现中会检查文件系统 noexec 标志（执行权限检查）
- 会检查只读文件系统（写权限检查）

## 参考

- 内核源码: `fs/open.c` (`do_faccessat`, `access_need_override_creds`, `access_override_creds`)
- `include/linux/cred.h` — `struct cred`
- `include/uapi/linux/fcntl.h` — `AT_FDCWD`, `AT_EACCESS`, `AT_SYMLINK_NOFOLLOW`