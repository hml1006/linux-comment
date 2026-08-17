# renameat2 系统调用

## 1. 原理与功能

**原型：**
```c
int renameat2(int olddirfd, const char *oldpath,
              int newdirfd, const char *newpath,
              unsigned int flags);
```

**功能：** 重命名（移动）文件或目录，支持 `renameat` 和 `rename` 的所有功能，并额外支持三种标志位控制重命名行为。

**flags 参数：**
| 标志 | 值 | 含义 |
|------|-----|------|
| `RENAME_NOREPLACE` | `(1 << 0)` | 如果目标存在则返回 `-EEXIST`，不覆盖 |
| `RENAME_EXCHANGE` | `(1 << 1)` | 原子交换源和目标，两者必须存在且在同一文件系统 |
| `RENAME_WHITEOUT` | `(1 << 2)` | 重命名时在源位置创建 whiteout 条目（用于 overlay 文件系统） |

**规则：**
- `RENAME_NOREPLACE` 和 `RENAME_EXCHANGE` 互斥，不能同时使用
- `RENAME_WHITEOUT` 需要 `CAP_MKNOD` 权限
- 源和目标必须在同一文件系统（不同文件系统返回 `-EXDEV`）
- `olddirfd`/`newdirfd` 为 `AT_FDCWD`（-100）时使用当前工作目录

## 2. 执行流程

```
                  renameat2(olddirfd, oldpath, newdirfd, newpath, flags)
                               |
                     +---------v----------+
                     | CLASS(filename)    |  用户态拷贝路径名
                     | old = getname()    |
                     | new = getname()    |
                     +---------+----------+
                               |
                     +---------v----------+
                     | filename_renameat2() |
                     +---------+----------+
                               |
              +----------------+------------------+
              |                |                  |
     +--------v--------+  +---v--------+   +-----v------+
     | filename_parentat|  |filename_parentat|  | 参数校验    |
     | 解析 oldpath    |  | 解析 newpath    |  | flags 合法性|
     | 获取 old_path   |  | 获取 new_path   |  | RENAME_*   |
     | dentry/父目录   |  | dentry/父目录   |  | 互斥性检查  |
     +--------+--------+  +---+------------+   +-----+------+
              |                |                      |
              +--------+-------+                      |
                       |                              |
              +--------v--------+                     |
              | 必须同一文件系统 |                     |
              | (old_path.mnt   |                     |
              |  == new_path.mnt)                     |
              +--------+--------+                     |
                       |                              |
              +--------v--------+                     |
              | mnt_want_write()|                     |
              | 获取写权限      |                     |
              +--------+--------+                     |
                       |                              |
              +--------v--------+                     |
              | __start_renaming|                     |
              | 查找 old/new     |                     |
              | dentry          |                     |
              +--------+--------+                     |
                       |                              |
              +--------v--------+                     |
              | vfs_rename()    |<--------------------+
              | (实际重命名)    |
              +--------+--------+
                       |
          +------------+-------------+
          |            |             |
   +------v----+  +---v------+  +---v------+
   | may_delete |  | may_create|  | dir->i_op|
   | 检查源权限 |  | 检查目标  |  | ->rename |
   +------+----+  | 权限      |  | 具体FS   |
          |       +-----+-----+  | 实现     |
          |             |        +----------+
          +------+------+
                 |
          +------v------+
          | 原子操作:    |
          | 1. 更新 dentry|
          | 2. 更新 inode |
          | 3. 发送通知   |
          +------+-------+
                 |
          +------v-------+
          | end_renaming() |
          | 释放锁       |
          +------+--------+
                 |
          +------v--------+
          | mnt_drop_write|
          +---------------+
```

## 3. 函数调用栈

```
renameat2()  [fs/namei.c]
  └── filename_renameat2()  [fs/namei.c]
        ├── filename_parentat(olddfd, from, ...)   // 解析 oldpath 的父目录
        ├── filename_parentat(newdfd, to, ...)     // 解析 newpath 的父目录
        ├── mnt_want_write(old_path.mnt)           // 获取挂载点写权限
        ├── __start_renaming(&rd, ...)             // 查找 old/new dentry 并加锁
        │     ├── lock_rename(old_parent, new_parent)
        │     ├── filename_lookup_ex(..., &rd.old_dentry)
        │     └── filename_lookup_ex(..., &rd.new_dentry)
        ├── security_path_rename(...)              // LSM 安全钩子
        ├── vfs_rename(&rd)                        // VFS 层重命名
        │     ├── may_delete_dentry(...)           // 检查源是否可删除
        │     ├── may_create_dentry(...)            // 检查目标是否可创建
        │     ├── inode_permission(...)             // 检查权限
        │     ├── try_break_deleg(...)             // 处理 NFS delegation
        │     └── dir->i_op->rename(...)           // 具体文件系统（如 ext4_rename）
        ├── end_renaming(&rd)                      // 释放重命名锁
        └── mnt_drop_write(old_path.mnt)           // 释放写权限
```

## 4. 关键数据结构

```c
// 重命名操作参数封装
struct renamedata {
    struct dentry *old_parent;        // 源文件父目录 dentry
    struct dentry *new_parent;        // 目标文件父目录 dentry
    struct mnt_idmap *mnt_idmap;      // 挂载 idmap
    struct dentry *old_dentry;        // 源文件 dentry
    struct dentry *new_dentry;        // 目标文件 dentry
    struct delegated_inode *delegated_inode;  // NFS delegation 处理
    unsigned int flags;               // RENAME_* 标志位
};

// inode 操作表——重命名方法
struct inode_operations {
    int (*rename)(struct mnt_idmap *idmap,
                  struct inode *old_dir, struct dentry *old_dentry,
                  struct inode *new_dir, struct dentry *new_dentry,
                  unsigned int flags);
    // ... 其他方法
};

// 路径查找结果
struct path {
    struct vfsmount *mnt;    // 挂载点
    struct dentry *dentry;   // 目录项
};
```

## 5. 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    int ret;

    // === 示例1: 普通重命名（等价于 rename()）===
    ret = renameat2(AT_FDCWD, "/tmp/old.txt",
                    AT_FDCWD, "/tmp/new.txt", 0);
    printf("rename(0): %s\n", ret == 0 ? "OK" : strerror(errno));

    // === 示例2: RENAME_NOREPLACE 安全重命名 ===
    ret = renameat2(AT_FDCWD, "/tmp/source.txt",
                    AT_FDCWD, "/tmp/target.txt",
                    RENAME_NOREPLACE);
    // 如果 target.txt 存在，返回 -EEXIST，不会覆盖
    printf("rename(RENAME_NOREPLACE): %s\n",
           ret == 0 ? "OK" : strerror(errno));

    // === 示例3: RENAME_EXCHANGE 原子交换 ===
    ret = renameat2(AT_FDCWD, "/tmp/a.txt",
                    AT_FDCWD, "/tmp/b.txt",
                    RENAME_EXCHANGE);
    // a.txt 和 b.txt 内容互换，原子操作
    printf("rename(RENAME_EXCHANGE): %s\n",
           ret == 0 ? "OK" : strerror(errno));

    // === 示例4: 使用目录 fd ===
    int dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    ret = renameat2(dirfd, "old.txt", dirfd, "new.txt", 0);
    close(dirfd);

    return 0;
}
```

## 6. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件元数据与属性)
- [renameat.md](renameat.md)
- [unlinkat.md](unlinkat.md)
- 内核源码: `fs/namei.c` `include/uapi/linux/fs.h`