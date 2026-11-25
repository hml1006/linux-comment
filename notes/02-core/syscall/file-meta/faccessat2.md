# faccessat2 系统调用

## 原理与功能

`faccessat2` 是 Linux 5.8 引入的 `faccessat` 增强版本，修复了原 `faccessat` 在 flags 参数处理上的问题。原 `faccessat` 的 flags 参数被内核忽略（历史原因），而 `faccessat2` 正确实现了 `AT_EACCESS` 和 `AT_SYMLINK_NOFOLLOW` 标志。

### 功能说明

- 正确支持 `AT_EACCESS`：使用有效用户/组 ID（而非实际 ID）检查权限
- 正确支持 `AT_SYMLINK_NOFOLLOW`：不跟随符号链接
- 正确支持 `AT_EMPTY_PATH`：通过空路径和 dirfd 操作
- 与 `faccessat` 相同的 `at` 系列语义
- 底层共享同一个 `do_faccessat` 实现

### 与 faccessat 的区别

| 特性 | `faccessat` | `faccessat2` |
|------|------------|-------------|
| flags 参数 | 被忽略（始终为 0） | 正确实现 |
| `AT_EACCESS` | 无效 | 使用有效用户/组 ID |
| `AT_SYMLINK_NOFOLLOW` | 无效 | 不跟随符号链接 |
| `AT_EMPTY_PATH` | 无效 | 允许通过 fd 操作 |
| 引入内核版本 | 2.6.16 | 5.8 |

## 使用场景

- 需要 `AT_EACCESS` 标志的权限检查（使用有效 ID）
- 新代码中替代 `faccessat` 的更安全选择
- 需要精确控制标志位的权限检查
- setuid 程序需要以有效用户身份检查权限

## API 及使用案例

### 函数原型

```c
#include <fcntl.h>
#include <unistd.h>

int faccessat2(int dirfd, const char *pathname, int mode, int flags);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `dirfd` | `int` | 目录 fd，`AT_FDCWD` 表示当前工作目录 |
| `pathname` | `const char*` | 文件路径 |
| `mode` | `int` | 检查模式（`F_OK`, `R_OK`, `W_OK`, `X_OK`） |
| `flags` | `int` | `AT_EACCESS`、`AT_SYMLINK_NOFOLLOW`、`AT_EMPTY_PATH` |

### flags 标志位

| 标志 | 值 | 说明 |
|------|-----|------|
| `AT_EACCESS` | 0x200 | 使用有效用户/组 ID 检查权限 |
| `AT_SYMLINK_NOFOLLOW` | 0x100 | 不跟随符号链接 |
| `AT_EMPTY_PATH` | 0x1000 | 允许 pathname 为空，通过 dirfd 操作 |

### 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    // 使用有效 ID 检查（不同于 access 使用实际 ID）
    // 在 setuid 程序中，AT_EACCESS 使用提升后的有效用户 ID
    if (faccessat2(AT_FDCWD, "/etc/shadow", R_OK, AT_EACCESS) == 0)
        printf("有效用户可读 /etc/shadow\n");
    else
        perror("不可读");

    // 不跟随符号链接
    if (faccessat2(AT_FDCWD, "/link", F_OK, AT_SYMLINK_NOFOLLOW) == 0)
        printf("符号链接存在\n");

    // 通过 fd 检查（AT_EMPTY_PATH）
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd >= 0) {
        if (faccessat2(fd, "", R_OK, AT_EMPTY_PATH) == 0)
            printf("通过 fd 检查：文件可读\n");
        close(fd);
    }

    return 0;
}
```

## 执行流程

```
faccessat2(dirfd, pathname, mode, flags)
  └─ syscall(__NR_faccessat2, dirfd, pathname, mode, flags)
       └─ __arm64_sys_faccessat2()
            └─ do_faccessat(dfd, filename, mode, flags)   // fs/open.c:466
                 ├─ 参数校验: mode & ~S_IRWXO → -EINVAL
                 │            flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH) → -EINVAL
                 │
                 ├─ access_need_override_creds(flags):
                 │    ├─ 如果 flags & AT_EACCESS → 不切换凭证（使用有效 ID）
                 │    └─ 否则 → 切换 cred 为 real_cred（使用实际 ID）
                 │
                 ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 │    ├─ 默认 lookup_flags = LOOKUP_FOLLOW
                 │    └─ 如果 flags & AT_SYMLINK_NOFOLLOW → 清除 LOOKUP_FOLLOW
                 │
                 ├─ 如果 (mode & MAY_EXEC) && S_ISREG:
                 │    └─ path_noexec(&path) → -EACCES
                 │
                 ├─ inode_permission(idmap, inode, mode | MAY_ACCESS)
                 │
                 └─ 如果 (mode & S_IWOTH) && !special_file:
                      └─ __mnt_is_readonly(path.mnt) → -EROFS
```

## 函数调用栈

```
faccessat2(dirfd, pathname, mode, flags)
  └─ syscall(__NR_faccessat2, dirfd, pathname, mode, flags)
       └─ __arm64_sys_faccessat2()
            └─ do_faccessat(dfd, filename, mode, flags)   // fs/open.c:466
                 ├─ 选择 cred（AT_EACCESS → effective, 否则 → real）
                 ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 ├─ noexec 检查
                 ├─ inode_permission(idmap, inode, mode | MAY_ACCESS)
                 └─ 只读文件系统检查
```

## 关键数据结构

### AT_EACCESS 标志

```c
// include/uapi/linux/fcntl.h
#define AT_EACCESS          0x200  // 使用有效用户/组 ID
#define AT_SYMLINK_NOFOLLOW 0x100  // 不跟随符号链接
#define AT_EMPTY_PATH       0x1000 // 允许空路径
```

### access_need_override_creds

```c
// fs/open.c:390
static bool access_need_override_creds(int flags)
{
    const struct cred *cred;

    // AT_EACCESS 标志表示使用有效 ID，无需切换到实际 ID
    if (flags & AT_EACCESS)
        return false;

    // 否则需要切换到实际用户/组 ID
    cred = current_cred();
    if (!uid_eq(cred->fsuid, cred->uid) ||
        !gid_eq(cred->fsgid, cred->gid))
        return true;

    if (!issecure(SECURE_NO_SETUID_FIXUP))
        return true;

    return false;
}
```

## 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EACCES` | 权限不足 | 请求的访问模式被拒绝 |
| `EFAULT` | 地址错误 | `pathname` 指向非法地址 |
| `EINVAL` | 参数无效 | `mode` 或 `flags` 包含非法位 |
| `ELOOP` | 符号链接循环 | 路径解析遇到过多符号链接 |
| `ENOENT` | 文件不存在 | 路径中的某个分量不存在 |
| `ENOTDIR` | 非目录 | 路径中的某个分量不是目录 |
| `EROFS` | 只读文件系统 | 请求 W_OK 但文件系统只读 |
| `ENOMEM` | 内存不足 | 内核内存分配失败 |

## 备注

- ARM64 系统调用号为 #439
- Linux 5.8 引入
- 修复了 `faccessat` 中 flags 参数被忽略的问题
- 推荐在新代码中使用 `faccessat2` 替代 `faccessat`
- `faccessat2` 和 `faccessat` 共享同一个内核实现函数 `do_faccessat`，区别仅在于 `faccessat` 传入了 `flags=0`

## 参考

- 内核源码: `fs/open.c` (`SYSCALL_DEFINE4(faccessat2)`, `do_faccessat`)
- `include/uapi/linux/fcntl.h` — `AT_EACCESS`, `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`