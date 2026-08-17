# symlinkat 系统调用

## 1. 原理与功能

**原型：**
```c
int symlinkat(const char *target, int newdirfd, const char *linkpath);
```

**功能：** 创建一个符号链接（软链接），指向 `target` 路径。符号链接是一种特殊的文件，其内容是一个指向另一个文件或目录的路径字符串。

**参数：**
- `target`: 符号链接指向的目标路径（可以是相对或绝对路径，目标可以不存在）
- `newdirfd`: 链接所在目录的 fd，`AT_FDCWD` 表示使用当前工作目录
- `linkpath`: 要创建的符号链接的路径名

**底层系统调用：** `symlinkat` 是 `symlink()` 的底层实现，`symlink()` 等价于 `symlinkat(target, AT_FDCWD, linkpath)`。

**ARM64 系统调用号：** `__NR_symlinkat` (434)

## 2. 执行流程

```
               symlinkat(target, newdirfd, linkpath)
                               |
                     +---------v----------+
                     | CLASS(filename)    |  拷贝 target 和 linkpath
                     | 从用户态拷贝路径   |
                     +---------+----------+
                               |
                     +---------v----------+
                     | filename_symlinkat() |
                     +---------+----------+
                               |
                     +---------v----------+
                     | filename_create()  |  查找 linkpath 的父目录
                     | 创建或获取 dentry   |  -> 创建新 dentry
                     +---------+----------+
                               |
                     +---------v----------+
                     | security_path_      |  LSM 安全钩子检查
                     | symlink()           |
                     +---------+----------+
                               |
                     +---------v----------+
                     | vfs_symlink()       |  VFS 层创建符号链接
                     +---------+----------+
                               |
              +----------------+------------------+
              |                |                  |
     +--------v--------+  +---v--------+   +-----v------+
     | may_create_dentry|  | dir->i_op |   | try_break_ |
     | 检查创建权限     |  | ->symlink |   | deleg()    |
     +--------+--------+  | 具体FS实现 |   | NFS 委托   |
                        | (ext4_symlink|   | 处理       |
                        | 等)         |   +------------+
                        +------+------+
                               |
                     +---------v----------+
                     | fsnotify_create()  |  发送文件系统通知
                     +---------+----------+
                               |
                     +---------v----------+
                     | end_creating_path()|  释放锁
                     +--------------------+
```

## 3. 函数调用栈

```
symlinkat()  [fs/namei.c]
  └── filename_symlinkat()  [fs/namei.c]
        ├── filename_create(newdfd, to, &path, lookup_flags)   // 查找并创建新 dentry
        │     ├── filename_lookup(dfd, name, LOOKUP_PARENT, ...)  // 查找父目录
        │     └── vfs_create(dentry, ...)                        // 如果路径不存在则创建
        ├── security_path_symlink(&path, dentry, target)        // LSM 安全钩子
        ├── vfs_symlink(idmap, dir, dentry, target, &delegated_inode)  // VFS 层
        │     ├── may_create_dentry(idmap, dir, dentry)          // 检查父目录写权限
        │     ├── dir->i_op->symlink(idmap, dir, dentry, target) // 具体文件系统实现
        │     │     ├── ext4_symlink()  // ext4 创建符号链接
        │     │     └── ...             // 其他 FS
        │     └── fsnotify_create(dir, dentry)                   // 发送 inotify 通知
        ├── end_creating_path(&path, dentry)                     // 释放路径锁
        └── break_deleg/delegation 处理                          // 如有需要
```

## 4. 关键数据结构

```c
// 符号链接的 inode 存储方式：
// 1. 快速符号链接（短路径）：直接存储在 inode 的 i_link 字段中
// 2. 慢速符号链接（长路径）：存储在数据块中，通过 page cache 读取
struct inode {
    const char *i_link;       // 快速符号链接内容（内联在 inode 中）
    unsigned int i_linklen;   // 链接路径长度
    // 通过 i_opflags 标记是否使用快速路径
    unsigned int i_opflags;
    // IOP_CACHED_LINK: 表示 i_link 已经缓存了链接内容
    // IOP_DEFAULT_READLINK: 使用默认的 readlink 实现
};

// 符号链接权限：
// 符号链接的权限始终是 0777（rwxrwxrwx），但实际权限由目标文件决定
// 创建时 umask 对符号链接无影响
#define S_IRWXUGO  (S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO  (S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)

// inode 操作表——symlink 方法
struct inode_operations {
    int (*symlink)(struct mnt_idmap *idmap,
                   struct inode *dir, struct dentry *dentry,
                   const char *symname);
    // ... 其他方法
};

// 文件类型
#define S_IFLNK  0120000   // 符号链接文件类型
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

    // === 示例1: 创建符号链接（相对路径）===
    // 创建 /tmp/original.txt -> /tmp/link.txt
    // 注意: 目标文件不存在时也可以创建符号链接
    ret = symlinkat("/tmp/original.txt", AT_FDCWD, "/tmp/link.txt");
    printf("symlinkat: %s\n", ret == 0 ? "OK" : strerror(errno));

    // === 示例2: 在指定目录中创建符号链接 ===
    int dirfd = open("/tmp/mydir", O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) {
        // 在 /tmp/mydir/ 下创建 link -> ../original.txt
        ret = symlinkat("../original.txt", dirfd, "link");
        close(dirfd);
    }

    // === 示例3: 检测符号链接目标（使用 readlinkat）===
    char buf[256];
    ssize_t len = readlinkat(AT_FDCWD, "/tmp/link.txt", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        printf("link target: %s\n", buf);
    }

    // === 示例4: 遍历符号链接（使用 realpath）===
    char *resolved = realpath("/tmp/link.txt", NULL);
    if (resolved) {
        printf("resolved path: %s\n", resolved);
        free(resolved);
    }

    // 清理
    unlink("/tmp/link.txt");

    return 0;
}
```

## 6. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件元数据与属性)
- [readlinkat.md](readlinkat.md)
- [unlinkat.md](unlinkat.md)
- 内核源码: `fs/namei.c` `include/linux/fs.h`