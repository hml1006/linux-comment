# fchmodat 系统调用

## 原理与功能

`fchmodat` 是 `chmod` 的 "at" 系列变体，支持相对于目录文件描述符修改文件权限，以及 `AT_SYMLINK_NOFOLLOW` 标志。

在 ARM64 架构上，`chmod` 没有独立的系统调用号，通过 `fchmodat`（syscall #53）实现。glibc 封装层将 `chmod(path, mode)` 转换为 `fchmodat(AT_FDCWD, path, mode, 0)`。

### 功能说明

- 相对于目录 fd 修改文件权限
- 支持 `AT_SYMLINK_NOFOLLOW` 不跟随符号链接
- 支持 `AT_EMPTY_PATH` 通过 fd 操作
- 支持 `AT_FDCWD` 使用当前工作目录
- 在 ARM64 上，`chmod` 也通过此调用实现

### 注意

`fchmodat` 的 `flags` 参数在旧版本内核中被忽略。如果需要确保 `AT_SYMLINK_NOFOLLOW` 生效，建议使用 `fchmodat2`（Linux 6.6+）。

## 使用场景

- 相对于指定目录修改权限（避免 TOCTOU）
- 不跟随符号链接修改权限
- `chmod` 命令的底层实现

## API 及使用案例

### 函数原型

```c
#include <fcntl.h>
#include <sys/stat.h>

int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `dirfd` | `int` | 目录 fd，`AT_FDCWD` 表示当前工作目录 |
| `pathname` | `const char*` | 文件路径 |
| `mode` | `mode_t` | 新权限模式 |
| `flags` | `int` | 标志位（`AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`） |

### 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>

int main() {
    // 相对于当前工作目录
    if (fchmodat(AT_FDCWD, "example.txt", 0644, 0) == -1) {
        perror("fchmodat");
        return 1;
    }
    printf("权限已设置\n");

    // 不跟随符号链接
    if (fchmodat(AT_FDCWD, "symlink", 0600, AT_SYMLINK_NOFOLLOW) == -1) {
        perror("fchmodat nofollow");
        return 1;
    }

    // 通过目录 fd
    int dirfd = open("/etc", O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) {
        if (fchmodat(dirfd, "passwd", 0644, 0) == 0)
            printf("已修改 /etc/passwd 权限\n");
        close(dirfd);
    }

    return 0;
}
```

## 执行流程

```
fchmodat(dirfd, pathname, mode, flags)
  └─ syscall(__NR_fchmodat, dirfd, pathname, mode, flags)
       └─ __arm64_sys_fchmodat()
            └─ do_fchmodat(dfd, filename, mode, 0)      // fs/open.c:670
                 │ 注意: flags 参数被忽略，始终为 0
                 ├─ 验证 flags 合法性
                 │    (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)
                 ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 │    ├─ LOOKUP_FOLLOW (默认跟随符号链接)
                 │    └─ 如果 flags & AT_SYMLINK_NOFOLLOW → 不跟随
                 └─ chmod_common(&path, mode)            // fs/open.c:621
                      ├─ mnt_want_write(path.mnt)
                      ├─ inode_lock(inode)
                      ├─ security_path_chmod(path, mode)
                      ├─ notify_change(idmap, dentry, &newattrs, NULL)
                      │    └─ inode->i_op->setattr()
                      ├─ inode_unlock(inode)
                      └─ mnt_drop_write(path.mnt)
```

## 函数调用栈

```
fchmodat(dirfd, pathname, mode, flags)
  └─ syscall(__NR_fchmodat, dirfd, pathname, mode, flags)
       └─ __arm64_sys_fchmodat()
            └─ do_fchmodat(dfd, filename, mode, flags)  // fs/open.c:670
                 ├─ 验证 flags (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)
                 ├─ 选择 lookup_flags:
                 │    └─ flags & AT_SYMLINK_NOFOLLOW ? 0 : LOOKUP_FOLLOW
                 ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 └─ chmod_common(&path, mode)            // fs/open.c:621
                      ├─ mnt_want_write(path.mnt)
                      ├─ inode_lock_killable(inode)
                      ├─ security_path_chmod(path, mode)
                      ├─ 构造 newattrs (ia_mode, ia_valid=ATTR_MODE|ATTR_CTIME)
                      ├─ notify_change(mnt_idmap(path.mnt), dentry, &newattrs, NULL)
                      ├─ inode_unlock(inode)
                      └─ mnt_drop_write(path.mnt)
```

## 关键数据结构

```c
// include/linux/fs.h
struct iattr {
    unsigned int    ia_valid;    // 有效属性标志 (ATTR_MODE | ATTR_CTIME)
    umode_t         ia_mode;     // 新权限模式
    // ...
};

// chmod_common 中 ia_mode 的计算:
// 保留文件类型位，仅修改权限位
// ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO)
// S_IALLUGO = S_ISUID|S_ISGID|S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO
```

## 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EACCES` | 权限不足 | 搜索路径中的某个目录无执行权限 |
| `EBADF` | 无效 fd | `dirfd` 不是有效的目录 fd（非 AT_FDCWD） |
| `EFAULT` | 地址错误 | `pathname` 指向非法地址 |
| `EINVAL` | 参数无效 | `flags` 包含未知标志 |
| `ELOOP` | 符号链接循环 | 路径解析遇到过多符号链接 |
| `ENOENT` | 文件不存在 | 路径中的某个分量不存在 |
| `ENOTDIR` | 非目录 | 路径中的某个分量不是目录 |
| `EPERM` | 操作不允许 | 不是文件所有者且无 `CAP_FOWNER` |
| `EROFS` | 只读文件系统 | 文件所在文件系统以只读方式挂载 |

## 备注

- ARM64 系统调用号为 #53
- `AT_SYMLINK_NOFOLLOW` 标志控制是否跟随符号链接
- `chmod()` 通过 `fchmodat(AT_FDCWD, path, mode, 0)` 实现
- `fchmodat` 的 flags 参数在旧版本内核中可能被忽略（详见 `fchmodat2`）
- 通过 `chmod_common` 与 `chmod`、`fchmod` 共享实现逻辑

## 参考

- 内核源码: `fs/open.c` (`do_fchmodat`, `chmod_common`, `SYSCALL_DEFINE3(fchmodat)`)
- `include/linux/fs.h` — `struct iattr`
- `include/uapi/linux/fcntl.h` — `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`