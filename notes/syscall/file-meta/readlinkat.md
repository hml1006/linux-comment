# readlinkat 系统调用

## 1. 原理与功能

**原型：**
```c
ssize_t readlinkat(int dirfd, const char *pathname,
                   char *buf, size_t bufsiz);
```

**功能：** 读取符号链接的目标路径字符串。符号链接的内容是一个路径字符串，`readlinkat` 将其读取到用户缓冲区。

**参数：**
- `dirfd`: 目录 fd，`AT_FDCWD` 表示使用当前工作目录
- `pathname`: 符号链接路径
- `buf`: 输出缓冲区
- `bufsiz`: 缓冲区大小

**注意：**
- `readlinkat` **不追加** null 终止符
- 如果 `bufsiz` 过小，内容会被截断（实际长度可以通过返回值和 `bufsiz` 比较判断）
- 只能读取符号链接，不能读取普通文件（返回 `-EINVAL`）
- `readlink()` 等价于 `readlinkat(AT_FDCWD, path, buf, bufsiz)`

**ARM64 系统调用号：** `__NR_readlinkat` (78)

## 2. 执行流程

```
              readlinkat(dirfd, pathname, buf, bufsiz)
                               |
                     +---------v----------+
                     | 检查 bufsiz <= 0   |
                     | -> 返回 -EINVAL    |
                     +---------+----------+
                               |
                     +---------v----------+
                     | filename_lookup()  |  查找路径
                     | (LOOKUP_EMPTY 允许 |  获取 dentry
                     |  空路径)           |
                     +---------+----------+
                               |
                     +---------v----------+
                     | 检查是否是符号链接   |
                     | d_is_symlink() 或  |
                     | inode->i_op->      |
                     |   readlink 存在    |
                     +---------+----------+
                               |
                     +---------v----------+
                     | security_inode_     |  LSM 安全钩子
                     | readlink()          |
                     +---------+----------+
                               |
                     +---------v----------+
                     | touch_atime()      |  更新访问时间
                     +---------+----------+
                               |
                     +---------v----------+
                     | vfs_readlink()     |  VFS 层读取链接
                     +---------+----------+
                               |
               +---------------+---------------+
               |                               |
     +---------v----------+          +---------v----------+
     | IOP_CACHED_LINK    |          | 普通路径:          |
     | 快速路径:          |          | i_op->get_link()  |
     | readlink_copy()    |          | 获取链接内容       |
     | 直接复制 i_link    |          | 然后 readlink_copy |
     | 到用户空间         |          | 复制到用户空间     |
     +---------+----------+          +---------+----------+
               |                               |
               +---------------+---------------+
                               |
                     +---------v----------+
                     | path_put()         |  释放路径引用
                     +--------------------+
```

## 3. 函数调用栈

```
readlinkat()  [fs/stat.c]
  └── do_readlinkat(dfd, pathname, buf, bufsiz)  [fs/stat.c]
        ├── filename_lookup(dfd, name, LOOKUP_EMPTY, &path, NULL)  // 查找路径（允许空路径）
        │     └── path_lookupat()  ->  walk_component() -> 具体 FS lookup
        ├── d_is_symlink(path.dentry)  // 检查是否是符号链接
        ├── security_inode_readlink(path.dentry)  // LSM 安全钩子
        ├── touch_atime(&path)  // 更新 inode 访问时间
        ├── vfs_readlink(path.dentry, buf, bufsiz)  [fs/namei.c]
        │     ├── [快速路径] readlink_copy(buf, buflen, inode->i_link, inode->i_linklen)
        │     │     └── copy_to_user(buffer, link, copylen)  // 从内核复制到用户空间
        │     └── [慢速路径] inode->i_op->get_link(dentry, ...)  // 从数据块读取
        │           └── readlink_copy(buf, buflen, link, len)
        └── path_put(&path)  // 释放 dentry/mnt 引用
```

## 4. 关键数据结构

```c
// 符号链接 inode 存储
struct inode {
    const char *i_link;         // 内联符号链接内容（短路径时使用）
    unsigned int i_linklen;     // 链接路径长度
    unsigned int i_opflags;     // 操作标志位
    // IOP_CACHED_LINK (1<<0): 链接内容已缓存在 i_link 中
    // IOP_DEFAULT_READLINK (1<<1): 使用默认的 readlink 实现
};

// 路径查找结果
struct path {
    struct vfsmount *mnt;    // 挂载点
    struct dentry *dentry;   // 目录项
};

// dentry 辅助宏
static inline bool d_is_symlink(const struct dentry *dentry)
{
    return d_is_symlink(dentry);
    // 实际判断: d_inode(dentry)->i_mode & S_IFMT == S_IFLNK
}

// 读取链接内容的辅助函数
int readlink_copy(char __user *buffer, int buflen,
                  const char *link, int linklen)
{
    int copylen = linklen;
    // 如果用户缓冲区太小，截断
    if (unlikely(copylen > (unsigned)buflen))
        copylen = buflen;
    // 从内核空间复制到用户空间
    if (copy_to_user(buffer, link, copylen))
        copylen = -EFAULT;
    return copylen;
}
```

## 5. 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
    char buf[256];
    ssize_t len;

    // === 示例1: 基本读取符号链接 ===
    len = readlinkat(AT_FDCWD, "/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        printf("当前进程可执行文件: %s\n", buf);
    }

    // === 示例2: 使用目录 fd ===
    // 创建测试: /tmp/symlink -> /etc/passwd
    symlinkat("/etc/passwd", AT_FDCWD, "/tmp/symlink");

    int dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    len = readlinkat(dirfd, "symlink", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        printf("符号链接目标: %s\n", buf);
    }
    close(dirfd);

    // === 示例3: 缓冲区大小不足的处理 ===
    char small_buf[4];
    len = readlinkat(AT_FDCWD, "/tmp/symlink", small_buf, sizeof(small_buf));
    if (len > 0) {
        printf("读取了 %zd 字节（实际路径更长）\n", len);
        // 注意: 内容被截断，且没有 null 终止符
    }

    // === 示例4: 使用 realpath 解析完整路径 ===
    char *resolved = realpath("/tmp/symlink", NULL);
    if (resolved) {
        printf("解析后的完整路径: %s\n", resolved);
        free(resolved);
    }

    unlink("/tmp/symlink");
    return 0;
}
```

## 6. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件元数据与属性)
- [symlinkat.md](symlinkat.md)
- 内核源码: `fs/stat.c` `fs/namei.c` `include/linux/fs.h`