# truncate 系统调用

## 原理与功能

`truncate` 系统调用将指定文件截断或扩展到指定长度。如果文件原来大于 `length`，超出的数据将被丢弃；如果文件原来小于 `length`，文件将用空字节（`\0`）填充扩展。

### 功能说明

- 截断文件到指定大小（丢弃超出部分）
- 扩展文件到指定大小（用零填充）
- 修改文件大小属性（`i_size`），不修改文件内容偏移量
- 需要写权限和对文件路径的访问权限

## 使用场景

- 清空文件内容（`truncate(path, 0)`）
- 为文件预分配空间（创建稀疏文件或预分配）
- 日志文件截断
- 下载工具预先分配空间（避免碎片）

## API 及使用案例

### 函数原型

```c
#include <unistd.h>
#include <sys/types.h>

int truncate(const char *path, off_t length);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `path` | `const char*` | 文件路径 |
| `length` | `off_t` | 目标文件大小（字节） |

### 返回值

- 成功返回 0
- 失败返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

int main() {
    // 将文件截断为 1024 字节
    if (truncate("example.txt", 1024) == -1) {
        perror("truncate");
        return 1;
    }
    printf("文件已截断/扩展到 1024 字节\n");

    // 清空文件（截断为 0）
    if (truncate("example.txt", 0) == -1) {
        perror("truncate to 0");
        return 1;
    }
    printf("文件已清空\n");

    // 验证文件大小
    struct stat sb;
    if (stat("example.txt", &sb) == 0) {
        printf("当前文件大小: %ld 字节\n", sb.st_size);
    }

    return 0;
}
```

## 执行流程

```
用户进程                          内核
    |                               |
    | truncate(path, length)        |
    |-----> syscall(#45) ---------->|
    |  ARM64 上通过 do_truncate()   |
    |                               |
    |    +----------------------+   |
    |    | do_sys_truncate()    |   |
    |    | fs/open.c            |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | user_path()          |   |
    |    | 路径解析和权限检查   |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | do_truncate()        |   |
    |    | (共享的截断逻辑)     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | get_write_access()   |   |
    |    | 检查写权限           |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | notify_change()      |   |
    |    | VFS 属性修改通知     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | inode->i_op->setattr()|   |
    |    | 文件系统特定实现     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | ext4_setattr()       |   |
    |    | 更新 i_size          |   |
    |    | 回收/分配数据块      |   |
    |    +---------+------------+   |
    |              |                |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
truncate(pathname, length)
  └─ syscall(__NR_truncate, pathname, length)
       └─ __arm64_sys_truncate()                  // arch/arm64/kernel/syscall.c
            └─ do_sys_truncate(pathname, length)   // fs/open.c
                 ├─ user_path(pathname, &path)     // 路径解析
                 └─ do_truncate(path.dentry, length, 0, path.mnt)
                      ├─ get_write_access(inode)    // 检查写权限
                      ├─ notify_change(idmap, dentry, &newattr, NULL)  // VFS 通知
                      │    └─ inode->i_op->setattr(idmap, dentry, &attr)
                      │         └─ ext4_setattr()   // 以 ext4 为例
                      │              ├─ ext4_alloc_truncate_blocks()  // 截断数据块
                      │              └─ ext4_truncate(inode)  // 更新 inode
                      └─ put_write_access(inode)
```

## 关键数据结构

### struct iattr（inode 属性修改描述）

```c
// include/linux/fs.h
struct iattr {
    unsigned int    ia_valid;    // 需要修改哪些属性（ATTR_* 标志）
    umode_t         ia_mode;     // 新权限模式
    uid_t           ia_uid;      // 新所有者 UID
    gid_t           ia_gid;      // 新所属组 GID
    loff_t          ia_size;     // 新文件大小（截断/扩展时使用）
    struct timespec64 ia_atime;  // 新访问时间
    struct timespec64 ia_mtime;  // 新修改时间
    struct timespec64 ia_ctime;  // 新状态改变时间
    // ...
};
```

### 属性有效标志

```c
#define ATTR_MODE       0x0001  // 修改权限
#define ATTR_UID        0x0002  // 修改所有者
#define ATTR_GID        0x0004  // 修改所属组
#define ATTR_SIZE       0x0008  // 修改文件大小（truncate 使用）
#define ATTR_ATIME      0x0010  // 修改访问时间
#define ATTR_MTIME      0x0020  // 修改修改时间
#define ATTR_CTIME      0x0040  // 修改状态改变时间
```

## 备注

- ARM64 系统调用号为 #45
- `truncate` 通过路径操作，`ftruncate` 通过 fd 操作
- 文件扩展时写入零字节，但不一定要分配物理块（稀疏文件）
- 只有文件所有者或特权进程才能截断文件
- 截断操作会更新文件的 `ctime` 和 `mtime`