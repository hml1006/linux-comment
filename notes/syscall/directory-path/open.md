# open 系统调用分析

## 1. 概述

打开或创建文件，返回文件描述符。这是最基础的文件打开接口，在新代码中推荐使用 `openat` 或 `openat2`。

**原型：**

```c
SYSCALL_DEFINE3(open, const char __user *, filename, int, flags, umode_t, mode)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `filename` | `const char *` | 文件路径 |
| `flags` | `int` | O_* 打开标志 |
| `mode` | `umode_t` | 创建模式（O_CREAT 时有效） |

**返回值：**

- 成功返回文件描述符（非负整数）
- 失败返回负值错误码：
  - `-EACCES` — 权限不足
  - `-ENOENT` — 文件不存在
  - `-EEXIST` — 文件已存在（O_CREAT | O_EXCL）
  - `-EINVAL` — 无效标志

## 2. 使用场景

- **文件读写**: 打开文件进行 read/write 操作
- **文件创建**: 使用 O_CREAT 创建新文件
- **设备访问**: 打开设备文件

## 3. 函数调用栈

```
open(filename, flags, mode) (系统调用入口)
└─ ksys_open(filename, flags, mode)                    // fs/open.c
   └─ do_sys_open(AT_FDCWD, filename, flags, mode)     // AT_FDCWD 表示当前目录
        └─ do_sys_openat2(AT_FDCWD, filename, &how)    // 核心入口
             └─ do_filp_open(AT_FDCWD, tmp, &op)       // 打开文件
                  └─ path_openat(nd, file, op)          // 路径解析+打开
                       ├─ path_init(nd, AT_FDCWD, name)  // 初始化路径查找
                       ├─ link_path_walk(name, nd)       // 逐分量解析路径
                       │    └─ walk_component(nd, ...)    // 解析单个路径分量
                       ├─ do_last(nd, file, op)          // 打开最后一个分量
                       │    ├─ lookup_open(nd, file, op)  // 查找/创建 dentry
                       │    └─ vfs_open(nd, file)         // VFS 打开文件
                       └─ fd_install(file, fd)            // 安装 fd
```

## 4. 关键数据结构

```c
// ===== struct file_handle (文件句柄, include/uapi/linux/fs.h) =====
struct file_handle {
    unsigned int handle_bytes;   // 句柄数据大小
    int handle_type;             // 句柄类型
    unsigned char f_handle[];    // 句柄数据（可变长度）
};

// ===== struct file_lock (文件锁, include/linux/fs.h) =====
struct file_lock {
    struct file_lock *fl_next;       // 同一 inode 上的下一个锁
    struct list_head fl_list;        // 锁链表
    struct hlist_node fl_link;       // 哈希链表
    fl_owner_t fl_owner;             // 锁所有者
    unsigned int fl_flags;           // 锁标志
    unsigned char fl_type;           // 锁类型 (F_RDLCK/F_WRLCK/F_UNLCK)
    unsigned int fl_pid;             // 持有锁的进程 PID
    struct pid *fl_nspid;            // 命名空间 PID
    wait_queue_head_t fl_wait;       // 等待队列
    struct file *fl_file;            // 关联的文件
};

// ===== struct open_how (openat2 参数, include/uapi/linux/openat2.h) =====
struct open_how {
    __u64 flags;     // O_* 打开标志
    __u64 mode;      // 创建模式（O_CREAT 时有效）
    __u64 resolve;   // 路径解析控制标志
};
// resolve 标志位:
// RESOLVE_NO_XDEV       - 禁止跨设备
// RESOLVE_NO_MAGICLINKS - 禁止 magic 符号链接
// RESOLVE_NO_SYMLINKS   - 禁止符号链接
// RESOLVE_BENEATH       - 限制在 dfd 下
// RESOLVE_IN_ROOT       - 以根目录为锚点
```

## 5. 流程图

```
open("/tmp/test.txt", O_RDWR | O_CREAT, 0644)
  │
  v
do_sys_openat2(AT_FDCWD, "tmp/test.txt", &how)
  │
  └─ path_openat()
       │
       ├─ path_init() → 初始化路径查找上下文
       │
       ├─ link_path_walk() → 逐分量解析路径
       │    ├─ "tmp" → lookup 找到 dentry
       │    └─ "test.txt" → do_last()
       │
       └─ do_last()
            ├─ lookup_open() → 查找或创建 dentry
            ├─ vfs_open() → 调用文件系统 open
            └─ fd_install() → 分配 fd
```

## 6. 使用示例

```c
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/tmp/test.txt", O_RDWR | O_CREAT, 0644);
    if (fd == -1) { perror("open"); return 1; }

    write(fd, "hello", 5);
    close(fd);
    printf("File written\n");
    return 0;
}
```

## 7. 参考

- `fs/open.c` — open/openat/openat2 实现
- `include/uapi/asm-generic/fcntl.h` — O_* 标志定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)