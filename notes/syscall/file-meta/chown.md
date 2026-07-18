# chown 系统调用

## 原理与功能

`chown` 系统调用改变指定文件的所有者和/或所属组。只有特权进程（`CAP_CHOWN`）可以更改文件所有者，文件所有者可以将文件组更改为其所属的任何组。

在 ARM64 架构上，`chown` 没有独立的系统调用号，通过 `fchownat`（syscall #55）实现。glibc 封装层将 `chown(path, owner, group)` 转换为 `fchownat(AT_FDCWD, path, owner, group, 0)`。

### 功能说明

- 更改文件所有者（需要 CAP_CHOWN）
- 更改文件所属组（所有者可改到其所属的任何组）
- 传递 `-1` 表示不修改该字段
- 对应的 `lchown` 通过 `fchownat(AT_FDCWD, path, owner, group, AT_SYMLINK_NOFOLLOW)` 实现

## 使用场景

- `chown` 命令的实现
- 文件所有权转移（如 `chown user:group file`）
- 系统管理操作
- 容器和用户命名空间中的文件所有权映射

## API 及使用案例

### 函数原型

```c
#include <unistd.h>
#include <sys/types.h>

int chown(const char *pathname, uid_t owner, gid_t group);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `pathname` | `const char*` | 文件路径 |
| `owner` | `uid_t` | 新所有者 ID（-1 表示不修改） |
| `group` | `gid_t` | 新组 ID（-1 表示不修改） |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>

int main() {
    // 更改文件所有者为 UID 1000
    if (chown("example.txt", 1000, -1) == -1) {
        perror("chown");
        return 1;
    }
    printf("所有者已更改\n");

    // 更改文件所属组为 GID 100
    if (chown("example.txt", -1, 100) == -1) {
        perror("chown group");
        return 1;
    }
    printf("所属组已更改\n");

    // 同时更改所有者和组
    if (chown("example.txt", 1000, 100) == -1) {
        perror("chown both");
        return 1;
    }
    printf("所有者和组已更改\n");

    return 0;
}
```

## 执行流程

```
chown(path, owner, group)           内核
    |                               |
    |-----> syscall(#55) ---------->|
    |  fchownat(AT_FDCWD, path,     |
    |    owner, group, 0)           |
    |                               |
    |    +----------------------+   |
    |    | do_fchownat()        |   |
    |    | fs/open.c:790        |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | 验证 flags:          |   |
    |    | AT_SYMLINK_NOFOLLOW  |   |
    |    | | AT_EMPTY_PATH      |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | filename_lookup()    |   |
    |    | 路径解析              |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | mnt_want_write()     |   |
    |    | 检查文件系统可写     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | chown_common()       |   |
    |    | 共享的 chown 逻辑    |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | 权限检查:             |   |
    |    | inode_owner_or_      |   |
    |    | capable()            |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | security_path_chown()|   |
    |    | LSM 安全钩子         |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | notify_change()      |   |
    |    | VFS 属性修改通知     |   |
    |    | ia_valid = ATTR_UID  |   |
    |    |          | ATTR_GID  |   |
    |    |          | ATTR_CTIME|   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | inode->i_op->setattr()|  |
    |    | 文件系统实现         |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | mnt_drop_write()     |   |
    |    +---------+------------+   |
    |              |                |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
chown(pathname, owner, group)
  └─ syscall(__NR_fchownat, AT_FDCWD, pathname, owner, group, 0)
       └─ __arm64_sys_fchownat()
            └─ do_fchownat(AT_FDCWD, filename, user, group, 0)  // fs/open.c:790
                 ├─ 验证 flag 参数合法性
                 ├─ filename_lookup(AT_FDCWD, name, LOOKUP_FOLLOW, &path, NULL)
                 ├─ mnt_want_write(path.mnt)
                 └─ chown_common(&path, user, group)              // fs/open.c
                      ├─ inode_owner_or_capable(idmap, inode)     // 权限检查
                      ├─ security_path_chown(path, uid, gid)      // LSM 钩子
                      ├─ 构造 iattr:
                      │    ia_valid = ATTR_UID | ATTR_GID | ATTR_CTIME
                      │    ia_uid = user (或 -1 时不设置)
                      │    ia_gid = group (或 -1 时不设置)
                      ├─ notify_change(idmap, dentry, &newattrs, NULL)
                      │    └─ inode->i_op->setattr()
                      │         └─ ext4_setattr()  // 更新 i_uid / i_gid
                      └─ mnt_drop_write(path.mnt)
```

## 关键数据结构

### struct inode（索引节点中的所有权字段）

```c
// include/linux/fs.h
struct inode {
    kuid_t      i_uid;      // 文件所有者（chown 修改此字段）
    kgid_t      i_gid;      // 文件所属组（chown 修改此字段）
    umode_t     i_mode;     // 文件类型和权限
    loff_t      i_size;     // 文件大小
    struct timespec64 i_ctime; // 状态改变时间（chown 更新此字段）
    // ...
};
```

### struct iattr（属性修改描述）

```c
// include/linux/fs.h
struct iattr {
    unsigned int    ia_valid;    // 有效属性标志
    umode_t         ia_mode;     // 权限模式
    uid_t           ia_uid;      // 新的用户 ID
    gid_t           ia_gid;      // 新的组 ID
    // ...
};

// chown 使用的 ia_valid 标志
#define ATTR_UID        0x0002  // 修改所有者
#define ATTR_GID        0x0004  // 修改所属组
#define ATTR_CTIME      0x0040  // 同时更新 ctime
```

## 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EACCES` | 权限不足 | 搜索路径中的某个目录无执行权限 |
| `EFAULT` | 地址错误 | `pathname` 指向非法地址 |
| `ELOOP` | 符号链接循环 | 路径解析遇到过多符号链接 |
| `ENAMETOOLONG` | 路径名过长 | `pathname` 超出 `PATH_MAX` |
| `ENOENT` | 文件不存在 | 路径中的某个分量不存在 |
| `ENOMEM` | 内存不足 | 内核内存分配失败 |
| `ENOTDIR` | 非目录 | 路径中的某个分量不是目录 |
| `EPERM` | 操作不允许 | 无 `CAP_CHOWN` 或非所有者 |
| `EROFS` | 只读文件系统 | 文件所在文件系统以只读方式挂载 |

## 备注

- ARM64 上无独立系统调用号，通过 `fchownat` 实现
- 传递 `-1` 给 owner 或 group 表示不修改该字段
- 只有特权进程（CAP_CHOWN）可以更改所有者
- 非特权进程可以将组改到其所属的任何组
- 更改所有者会清除 setuid/setgid 位（安全机制）
- 存在对应的 `lchown` 变体：`fchownat(AT_FDCWD, path, owner, group, AT_SYMLINK_NOFOLLOW)`

## 参考

- 内核源码: `fs/open.c` (`do_fchownat`, `chown_common`, `vfs_fchown`)
- `include/linux/fs.h` — `struct iattr`, `struct inode`
- `include/uapi/linux/fcntl.h` — `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`