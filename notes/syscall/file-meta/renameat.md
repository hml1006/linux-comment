# renameat 系统调用分析

## 1. 原理与功能

**renameat** 用于重命名文件或目录，是 `rename(2)` 的 `at` 系列扩展版本。它允许通过目录文件描述符指定相对路径。

**ARM64 系统调用号：** 38 (__NR_renameat)

**原型：**

```c
int renameat(int olddirfd, const char *oldpath,
             int newdirfd, const char *newpath);
```

**参数说明：**
- `olddirfd`：源文件路径的目录 fd
- `oldpath`：源文件路径
- `newdirfd`：目标文件路径的目录 fd
- `newpath`：目标文件路径

**注意：** `renameat` 是旧版接口，在内核中实际调用 `renameat2` 并将 flags 设为 0。推荐使用 `renameat2` 以支持更多标志（如 `RENAME_NOREPLACE`、`RENAME_EXCHANGE`）。

### 内核实现

```c
// fs/namei.c:6538
SYSCALL_DEFINE4(renameat, int, olddfd, const char __user *, oldname,
                int, newdfd, const char __user *, newname)
{
    CLASS(filename, old)(oldname);
    CLASS(filename, new)(newname);
    return filename_renameat2(olddfd, old, newdfd, new, 0);
    // flags = 0，直接调用 renameat2 实现
}
```

### 核心行为

- 源文件和目标文件必须在同一文件系统（跨文件系统返回 `-EXDEV`）
- 目标路径不能是源路径的子目录（目录重命名时检查）
- 需要源和目标父目录的写权限
- 如果目标已存在，会被覆盖（原子替换操作）
- 目录重命名时，源目录不能是当前目录的祖先目录

## 2. 执行流程

```
            renameat(olddirfd, oldpath, newdirfd, newpath)
                               |
                     +---------v----------+
                     |  filename_renameat2 |  fs/namei.c
                     |  (flags = 0)        |
                     +---------+----------+
                               |
                     +---------v----------+
                     |  do_renameat2()     |  核心实现
                     |  filename_parentat()|  查找源路径父目录
                     |  filename_parentat()|  查找目标路径父目录
                     |  mnt_want_write()   |  检查文件系统可写
                     |  __start_renaming() |  查找 old/new dentry 并加锁
                     |  security_inode_    |  LSM 安全检查
                     |  rename()            |
                     |  vfs_rename()       |  VFS 层重命名
                     |  |  may_delete()    |  检查源是否可删除
                     |  |  may_create()    |  检查目标是否可创建
                     |  |  i_op->rename()  |  文件系统实现
                     |  |  (ext4_rename2)  |
                     |  end_renaming()     |  释放锁
                     |  mnt_drop_write()   |
                     +---------+----------+
                               |
                     +---------v----------+
                     |  返回 0 成功/错误码  |
                     +--------------------+
```

## 3. 函数调用栈

```
renameat (用户态)
  └─ syscall(__NR_renameat, olddirfd, oldpath, newdirfd, newpath)
       └─ __arm64_sys_renameat()
            └─ filename_renameat2(olddfd, old, newdfd, new, 0)  // fs/namei.c
                 └─ do_renameat2(olddfd, old, newdfd, new, flags)
                      ├─ filename_parentat(olddfd, old, &old_path, &old_type)
                      │    └─ 查找 oldpath 的父目录 dentry
                      ├─ filename_parentat(newdfd, new, &new_path, &new_type)
                      │    └─ 查找 newpath 的父目录 dentry
                      ├─ 检查 old_path.mnt == new_path.mnt  // 同一文件系统
                      ├─ mnt_want_write(old_path.mnt)       // 获取写权限
                      ├─ __start_renaming(&rd, ...)          // 加锁并查找 dentry
                      │    ├─ lock_rename(old_parent, new_parent)
                      │    ├─ filename_lookup_ex(..., &rd.old_dentry)
                      │    └─ filename_lookup_ex(..., &rd.new_dentry)
                      ├─ security_path_rename(...)           // LSM 钩子
                      ├─ vfs_rename(&rd)                     // VFS 层
                      │    ├─ may_delete_dentry(...)         // 检查源可删除
                      │    ├─ may_create_dentry(...)          // 检查目标可创建
                      │    └─ dir->i_op->rename(...)         // 具体 FS 实现
                      │         └─ ext4_rename2()            // ext4 示例
                      │              └─ ext4_rename()
                      │                   ├─ ext4_add_entry()    // 添加新目录项
                      │                   └─ ext4_delete_entry() // 删除旧目录项
                      ├─ end_renaming(&rd)                    // 释放锁
                      └─ mnt_drop_write(old_path.mnt)         // 释放写权限
```

## 4. 关键数据结构

```c
// rename 操作的关键约束
// 1. 源和目标必须在同一文件系统
// 2. 目标路径不能是源路径的子目录
// 3. 需要源和目标父目录的写权限
// 4. 目录重命名需要特殊处理 (不能重命名非空目录的父目录)

// rename 操作涉及的 inode 操作 (include/linux/fs.h)
struct inode_operations {
    int (*rename)(struct mnt_idmap *idmap,
                  struct inode *old_dir, struct dentry *old_dentry,
                  struct inode *new_dir, struct dentry *new_dentry,
                  unsigned int flags);
    // ...
};

// 重命名操作参数封装 (fs/namei.c)
struct renamedata {
    struct dentry *old_parent;        // 源文件父目录 dentry
    struct dentry *new_parent;        // 目标文件父目录 dentry
    struct mnt_idmap *mnt_idmap;      // 挂载 idmap
    struct dentry *old_dentry;        // 源文件 dentry
    struct dentry *new_dentry;        // 目标文件 dentry
    struct delegated_inode *delegated_inode;  // NFS delegation 处理
    unsigned int flags;               // RENAME_* 标志位
};

// rename 操作流程
// 1. 锁定源和目标目录的 inode（lock_rename 防止死锁）
// 2. 检查 dentry 是否可移动
// 3. 调用文件系统的 rename 实现
// 4. 处理目录项缓存 (d_move 更新 dentry 缓存)
// 5. 更新父目录的 i_nlink (如果目标存在且是目录)
```

## 5. 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| `EACCES` | 权限不足 | 对源或目标父目录无写/执行权限 |
| `EBUSY` | 设备忙 | 尝试重命名挂载点或正在使用的目录 |
| `EFAULT` | 地址错误 | 路径名指向不可访问地址 |
| `EINVAL` | 无效参数 | 源路径包含 `..` 或新路径无效 |
| `EISDIR` | 是目录 | 目标存在且是目录，但源不是目录 |
| `ELOOP` | 符号链接循环 | 路径解析时遇到过多符号链接 |
| `EMLINK` | 链接数超限 | 目标父目录的链接数超过上限 |
| `ENAMETOOLONG` | 路径名过长 | 路径名超过 `PATH_MAX` |
| `ENOENT` | 不存在 | 源路径中某个组件不存在 |
| `ENOMEM` | 内存不足 | 内核内存分配失败 |
| `ENOTDIR` | 不是目录 | 路径中某个组件不是目录 |
| `ENOTEMPTY` | 目录非空 | 尝试用非空目录覆盖非空目录 |
| `EEXIST` | 已存在 | 目标已存在且使用 `RENAME_NOREPLACE` |
| `EXDEV` | 跨设备 | 源和目标不在同一文件系统 |
| `EPERM` | 权限不足 | 目标文件有不可变/仅追加属性 |

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 示例1: 基本重命名
    if (renameat(AT_FDCWD, "/tmp/oldname.txt",
                 AT_FDCWD, "/tmp/newname.txt") == 0) {
        printf("成功重命名文件\n");
    } else {
        perror("renameat");
    }

    // 示例2: 移动到不同目录
    if (renameat(AT_FDCWD, "/tmp/source/file.txt",
                 AT_FDCWD, "/tmp/dest/file.txt") == 0) {
        printf("成功将文件移动到不同目录\n");
    }

    // 示例3: 通过目录 fd 操作
    int src_dir = open("/tmp/source", O_RDONLY | O_DIRECTORY);
    int dst_dir = open("/tmp/dest", O_RDONLY | O_DIRECTORY);
    if (src_dir >= 0 && dst_dir >= 0) {
        if (renameat(src_dir, "doc.txt", dst_dir, "doc.txt") == 0) {
            printf("通过目录 fd 成功重命名文件\n");
        }
        close(src_dir);
        close(dst_dir);
    }

    // 示例4: 重命名目录
    if (renameat(AT_FDCWD, "/tmp/olddir",
                 AT_FDCWD, "/tmp/newdir") == 0) {
        printf("成功重命名目录\n");
    }

    // 示例5: 目标文件已存在时会被覆盖
    // 如果 newname.txt 已存在，会被 oldname.txt 的内容覆盖
    // 这是原子操作：要么全部完成，要么全部不做
    if (renameat(AT_FDCWD, "/tmp/source.txt",
                 AT_FDCWD, "/tmp/existing.txt") == 0) {
        printf("文件已覆盖目标\n");
    }

    exit(EXIT_SUCCESS);
}
```

## 7. 备注

- `renameat` 是历史接口，内核中直接调用 `renameat2` 并将 flags 设为 0
- 推荐在新代码中使用 `renameat2` 以支持 `RENAME_NOREPLACE`、`RENAME_EXCHANGE` 等标志
- `renameat` 是原子操作：要么成功完成，要么文件系统状态不变
- 重命名目录时，如果目标存在且为空目录，目标会被替换
- 重命名操作自动更新 dentry 缓存（`d_move`），无需手动刷新

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件元数据与属性)
- [renameat2.md](renameat2.md) — 支持更多标志的增强版
- 内核源码: `fs/namei.c` (`SYSCALL_DEFINE4(renameat)`, `filename_renameat2`, `do_renameat2`, `vfs_rename`)
- `include/uapi/linux/fs.h` — `RENAME_NOREPLACE`, `RENAME_EXCHANGE` 标志定义