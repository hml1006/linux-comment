# fchownat 系统调用分析

## 1. 原理与功能

**fchownat** 是 `at` 系列系统调用之一，用于通过目录文件描述符相对路径更改文件所有者和所属组。它统一实现了 `chown(2)`、`lchown(2)`、`fchown(2)` 的功能。

**ARM64 系统调用号：** 54 (__NR_fchownat)

**原型：**

```c
int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags);
```

**参数说明：**
- `dirfd`：目录文件描述符，用于相对路径解析。设为 `AT_FDCWD` 时使用当前工作目录
- `pathname`：文件路径，可为空字符串（配合 `AT_EMPTY_PATH`）
- `owner`：新的所有者 UID，设为 -1 表示不改变
- `group`：新的所属组 GID，设为 -1 表示不改变
- `flags`：控制标志位
  - `AT_SYMLINK_NOFOLLOW`（0x100）：不跟随符号链接
  - `AT_EMPTY_PATH`（0x1000）：允许通过 dirfd 操作空路径

**权限要求：**
- 需要拥有 `CAP_CHOWN` 能力，或者文件所有者与调用者相同
- 非特权用户只能将文件组改为自己的属组之一

## 2. 执行流程

```
                    fchownat(dirfd, pathname, owner, group, flags)
                               |
                     +---------v----------+
                     |  do_fchownat()      |  fs/open.c
                     |  验证 flags 有效性   |
                     |  (AT_SYMLINK_NOFOLLOW|
                     |   /AT_EMPTY_PATH)    |
                     +---------+----------+
                               |
                     +---------v----------+
                     |  filename_lookup()  |  路径查找
                     |  根据 dfd 解析路径   |
                     |  AT_SYMLINK_NOFOLLOW|
                     |  决定是否跟随链接    |
                     +---------+----------+
                               |
                     +---------v----------+
                     |  mnt_want_write()   |  检查文件系统可写
                     +---------+----------+
                               |
                     +---------v----------+
                     |  chown_common()     |  核心权限检查
                     |  inode_owner_or_    |  和 inode 更新
                     |  capable() 检查     |
                     |  security_path_     |
                     |  chown() LSM 检查   |
                     |  notify_change()    |
                     |  更新 i_uid/i_gid   |
                     +---------+----------+
                               |
                     +---------v----------+
                     |  mnt_drop_write()   |  释放写权限
                     +---------+----------+
                               |
                     +---------v----------+
                     |  path_put()         |  释放路径引用
                     +---------+----------+
                               |
                     +---------v----------+
                     |  返回 0 成功/错误码  |
                     +--------------------+
```

## 3. 函数调用栈

```
fchownat (用户态)
  └─ syscall(__NR_fchownat, dirfd, pathname, owner, group, flags)
       └─ __arm64_sys_fchownat()
            └─ do_fchownat(dfd, filename, user, group, flag)
                 ├─ 验证 flags (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)
                 ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 │    └─ path_lookupat() → 具体的文件系统路径解析
                 ├─ mnt_want_write(path.mnt)  // 检查文件系统可写
                 ├─ chown_common(&path, user, group)
                 │    ├─ inode_owner_or_capable(idmap, inode)  // 权限检查
                 │    ├─ security_path_chown(path, uid, gid)   // LSM 钩子
                 │    ├─ mnt_idmap_permission()                // idmap 权限
                 │    └─ notify_change(idmap, dentry, &newattrs, NULL)
                 │         └─ setattr_prepare() → i_op->setattr()
                 ├─ mnt_drop_write(path.mnt)
                 └─ path_put(&path)
```

## 4. 关键数据结构

```c
// AT_* 标志常量定义 (include/uapi/linux/fcntl.h)
#define AT_FDCWD            -100    // 表示使用当前工作目录
#define AT_SYMLINK_NOFOLLOW 0x100   // 不跟随符号链接
#define AT_EMPTY_PATH       0x1000  // 允许通过 fd 操作空路径

// chown_common 使用的 iattr 结构 (include/linux/fs.h)
struct iattr {
    unsigned int    ia_valid;    // 有效属性标志 (ATTR_UID | ATTR_GID)
    umode_t         ia_mode;     // 权限模式
    uid_t           ia_uid;      // 新的用户 ID
    gid_t           ia_gid;      // 新的组 ID
    loff_t          ia_size;     // 文件大小
    struct timespec64 ia_atime;  // 访问时间
    struct timespec64 ia_mtime;  // 修改时间
    struct timespec64 ia_ctime;  // 状态改变时间
};

// inode 中的所有者信息 (include/linux/fs.h)
struct inode {
    umode_t         i_mode;      // 文件类型和权限
    kuid_t          i_uid;       // 文件所有者 UID
    kgid_t          i_gid;       // 文件所属组 GID
    // ... 其他字段
};

// 路径查找结果 (include/linux/path.h)
struct path {
    struct vfsmount *mnt;        // 文件系统挂载点
    struct dentry   *dentry;     // 目录项
};
```

## 5. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 示例1: 更改文件所有者为 root (需要 CAP_CHOWN)
    if (fchownat(AT_FDCWD, "/tmp/test.txt", 0, -1, 0) == 0) {
        printf("成功更改文件所有者为 root\n");
    } else {
        perror("fchownat");
    }

    // 示例2: 只更改组 (不改变所有者)
    if (fchownat(AT_FDCWD, "/tmp/test.txt", -1, 1000, 0) == 0) {
        printf("成功更改文件所属组\n");
    }

    // 示例3: 通过文件描述符操作 (不跟随符号链接)
    int fd = open("/tmp/test.txt", O_RDONLY);
    if (fd >= 0) {
        if (fchownat(fd, "", 1000, 1000, AT_EMPTY_PATH) == 0) {
            printf("通过 fd 成功更改文件所有者与组\n");
        }
        close(fd);
    }

    // 示例4: 不跟随符号链接
    if (fchownat(AT_FDCWD, "/tmp/symlink", 1000, 1000,
                 AT_SYMLINK_NOFOLLOW) == 0) {
        printf("成功更改符号链接自身的所有者\n");
    }

    exit(EXIT_SUCCESS);
}
```