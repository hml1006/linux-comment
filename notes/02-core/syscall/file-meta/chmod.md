# chmod 系统调用

## 原理与功能

`chmod` 系统调用改变指定文件的权限位（mode）。它修改文件的模式（mode），包括所有者/组/其他的读、写、执行权限，以及 setuid/setgid/sticky 等特殊位。

在 ARM64 架构上，`chmod` 没有独立的系统调用号，通过 `fchmodat`（syscall #53）实现。glibc 封装层将 `chmod(path, mode)` 转换为 `fchmodat(AT_FDCWD, path, mode, 0)`。

### 功能说明

- 修改文件权限（读/写/执行）
- 修改特殊权限位（setuid/setgid/sticky）
- 只有文件所有者或特权进程（CAP_FOWNER）才能修改权限
- 非所有者修改文件权限时会清除 setuid/setgid 位（安全机制）

## 使用场景

- `chmod` 命令的实现
- 修改文件权限以限制或允许访问
- 设置 setuid/setgid 位（如 `chmod u+s /bin/prog`）
- 设置 sticky 位（如 `/tmp` 目录）

## API 及使用案例

### 函数原型

```c
#include <sys/stat.h>

int chmod(const char *pathname, mode_t mode);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `pathname` | `const char*` | 文件路径 |
| `mode` | `mode_t` | 新权限模式，由权限位常量按位或组合 |

### mode 权限位常量

| 常量 | 八进制值 | 说明 |
|------|---------|------|
| `S_ISUID` | 4000 | set-user-ID 位 |
| `S_ISGID` | 2000 | set-group-ID 位 |
| `S_ISVTX` | 1000 | sticky 位 |
| `S_IRWXU` | 0700 | 所有者读、写、执行 |
| `S_IRUSR` | 0400 | 所有者读 |
| `S_IWUSR` | 0200 | 所有者写 |
| `S_IXUSR` | 0100 | 所有者执行 |
| `S_IRWXG` | 0070 | 组读、写、执行 |
| `S_IRGRP` | 0040 | 组读 |
| `S_IWGRP` | 0020 | 组写 |
| `S_IXGRP` | 0010 | 组执行 |
| `S_IRWXO` | 0007 | 其他读、写、执行 |
| `S_IROTH` | 0004 | 其他读 |
| `S_IWOTH` | 0002 | 其他写 |
| `S_IXOTH` | 0001 | 其他执行 |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>

int main() {
    // 设置文件为所有者可读写，其他只读
    if (chmod("example.txt", 0644) == -1) {
        perror("chmod");
        return 1;
    }
    printf("权限已设置为 0644\n");

    // 设置可执行权限
    if (chmod("script.sh", 0755) == -1) {
        perror("chmod 0755");
        return 1;
    }
    printf("权限已设置为 0755\n");

    // 设置 setuid 位
    if (chmod("prog", 4755) == -1) {
        perror("chmod setuid");
        return 1;
    }
    printf("已设置 setuid 位\n");

    return 0;
}
```

## 执行流程

```
chmod(path, mode)                   内核
    |                               |
    |-----> syscall(#53) ---------->|
    |  fchmodat(AT_FDCWD, path,     |
    |           mode, 0)            |
    |                               |
    |    +----------------------+   |
    |    | do_fchmodat()        |   |
    |    | fs/open.c:670        |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | filename_lookup()    |   |
    |    | 路径解析:             |   |
    |    | LOOKUP_FOLLOW 跟随   |   |
    |    | 符号链接              |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | chmod_common()       |   |
    |    | fs/open.c:621        |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | mnt_want_write()     |   |
    |    | 检查文件系统可写     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | inode_lock(inode)    |   |
    |    | 锁住 inode           |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | security_path_chmod()|   |
    |    | LSM 安全钩子检查     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | notify_change()      |   |
    |    | VFS 属性修改通知     |   |
    |    | ia_valid = ATTR_MODE |   |
    |    |          | ATTR_CTIME|   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | inode->i_op->setattr()|  |
    |    | 文件系统特定实现     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | ext4_setattr()       |   |
    |    | 更新 i_mode 字段     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | inode_unlock(inode)  |   |
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
chmod(pathname, mode)
  └─ syscall(__NR_fchmodat, AT_FDCWD, pathname, mode, 0)
       └─ __arm64_sys_fchmodat()
            └─ do_fchmodat(AT_FDCWD, pathname, mode, 0)  // fs/open.c:670
                 ├─ filename_lookup(AT_FDCWD, name, LOOKUP_FOLLOW, &path, NULL)
                 └─ chmod_common(&path, mode)              // fs/open.c:621
                      ├─ mnt_want_write(path->mnt)         // 获取挂载写权限
                      ├─ inode_lock_killable(inode)        // 锁住 inode
                      ├─ security_path_chmod(path, mode)   // LSM 安全钩子
                      ├─ 构造 newattrs:
                      │    newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO)
                      │    newattrs.ia_valid = ATTR_MODE | ATTR_CTIME
                      ├─ notify_change(idmap, dentry, &newattrs, NULL)  // VFS 通知
                      │    └─ inode->i_op->setattr(idmap, dentry, &attr)
                      │         └─ ext4_setattr()           // 以 ext4 为例
                      │              └─ ext4_do_update_inode()  // 更新磁盘 inode
                      ├─ inode_unlock(inode)
                      └─ mnt_drop_write(path->mnt)
```

## 关键数据结构

### struct iattr（inode 属性修改描述）

```c
// include/linux/fs.h
struct iattr {
    unsigned int    ia_valid;    // 需要修改哪些属性（ATTR_* 标志）
    umode_t         ia_mode;     // 新权限模式（chmod 设置此字段）
    uid_t           ia_uid;      // 新所有者 UID
    gid_t           ia_gid;      // 新所属组 GID
    loff_t          ia_size;     // 新文件大小
    struct timespec64 ia_atime;  // 新访问时间
    struct timespec64 ia_mtime;  // 新修改时间
    struct timespec64 ia_ctime;  // 新状态改变时间
};
```

### struct inode（VFS 索引节点）

```c
// include/linux/fs.h
struct inode {
    umode_t         i_mode;      // 文件权限和类型（chmod 修改此字段）
    kuid_t          i_uid;       // 文件所有者 UID
    kgid_t          i_gid;       // 文件所属组 GID
    loff_t          i_size;      // 文件大小
    struct timespec64 i_atime;   // 最后访问时间
    struct timespec64 i_mtime;   // 最后修改时间
    struct timespec64 i_ctime;   // 状态改变时间（chmod 更新此字段）
    // ...
};
```

### 属性有效标志

```c
#define ATTR_MODE       0x0001  // 修改权限（chmod 使用）
#define ATTR_UID        0x0002  // 修改所有者
#define ATTR_GID        0x0004  // 修改所属组
#define ATTR_SIZE       0x0008  // 修改文件大小
#define ATTR_ATIME      0x0010  // 修改访问时间
#define ATTR_MTIME      0x0020  // 修改修改时间
#define ATTR_CTIME      0x0040  // 修改状态改变时间（chmod 同时更新）
```

## 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EACCES` | 权限不足 | 搜索路径中的某个目录无执行权限 |
| `EFAULT` | 地址错误 | `pathname` 指向用户空间外的地址 |
| `EIO` | I/O 错误 | 读取文件系统时发生 I/O 错误 |
| `ELOOP` | 符号链接循环 | 路径解析时遇到过多的符号链接 |
| `ENAMETOOLONG` | 路径名过长 | `pathname` 超出 `PATH_MAX` |
| `ENOENT` | 文件不存在 | 路径中的某个分量不存在 |
| `ENOMEM` | 内存不足 | 内核内存分配失败 |
| `ENOTDIR` | 非目录 | 路径中的某个分量不是目录 |
| `EPERM` | 操作不允许 | 不是文件所有者且无 `CAP_FOWNER` 能力 |
| `EROFS` | 只读文件系统 | 文件所在文件系统以只读方式挂载 |

## 备注

- ARM64 上无独立系统调用号，通过 `fchmodat` 实现
- 只有文件所有者或特权进程（CAP_FOWNER）才能修改权限
- 非所有者修改文件权限时会清除 setuid/setgid 位（安全机制）
- `chmod_common` 中 `ia_mode` 的计算：`(mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO)`，保留文件类型位
- 权限修改后自动更新 `ctime`（状态改变时间）

## 参考

- 内核源码: `fs/open.c` (`do_fchmodat`, `chmod_common`)
- `include/linux/fs.h` — `struct iattr`, `struct inode`, `ATTR_*` 常量
- `include/uapi/linux/stat.h` — `S_IRWXU` 等权限常量