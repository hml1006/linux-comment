# openat 系统调用分析

## 1. 概述

通过目录 fd 打开文件。与 `open` 的区别在于可以指定路径解析的起始目录，避免 TOCTOU 竞争条件。

**原型：**

```c
SYSCALL_DEFINE4(openat, int, dfd, const char __user *, filename,
        int, flags, umode_t, mode)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd（`AT_FDCWD` 表示当前目录） |
| `filename` | `const char *` | 文件路径 |
| `flags` | `int` | O_* 打开标志 |
| `mode` | `umode_t` | 创建模式（O_CREAT 时有效） |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码

## 2. 使用场景

- **相对路径打开**: 相对于已打开的目录 fd 打开文件
- **避免竞争**: 避免 TOCTOU（time-of-check-time-of-use）竞争
- **容器**: 通过容器根目录 fd 打开文件

## 3. 函数调用栈

```
openat(dfd, filename, flags, mode) (系统调用入口)
└─ ksys_openat(dfd, filename, flags, mode)             // fs/open.c
   └─ do_sys_openat2(dfd, filename, &how)              // 核心入口
        └─ do_filp_open(dfd, tmp, &op)                 // 打开文件
             └─ path_openat(nd, file, op)              // 路径解析+打开
                  ├─ path_init(nd, dfd, name)           // 从 dfd 开始
                  ├─ link_path_walk(name, nd)           // 逐分量解析
                  └─ do_last(nd, file, op)              // 打开最后分量
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
openat(dirfd, "test.txt", O_RDWR, 0)
  │
  ├─ 从 dirfd 指向的目录开始解析路径
  │
  └─ path_openat()
       ├─ path_init(nd, dirfd, "test.txt")
       ├─ link_path_walk → 解析 "test.txt"
       └─ do_last → lookup_open + vfs_open + fd_install
```

## 6. 使用示例

```c
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int dir_fd = open("/tmp", O_RDONLY);
    if (dir_fd == -1) { perror("open dir"); return 1; }

    // 相对于 /tmp 打开文件
    int fd = openat(dir_fd, "test.txt", O_RDWR | O_CREAT, 0644);
    if (fd == -1) { perror("openat"); return 1; }

    write(fd, "hello", 5);
    close(fd);
    close(dir_fd);
    return 0;
}
```

## 7. 参考

- `fs/open.c` — openat 实现
- `include/uapi/asm-generic/fcntl.h` — AT_* 标志定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)