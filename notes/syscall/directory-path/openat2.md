# openat2 系统调用分析

## 1. 概述

扩展的打开文件接口。通过 `struct open_how` 结构提供更精细的路径解析控制，支持安全路径解析（如禁止符号链接、禁止跨设备等）。

**原型：**

```c
SYSCALL_DEFINE4(openat2, int, dfd, const char __user *, filename,
        struct open_how __user *, how, size_t, usize)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd |
| `filename` | `const char *` | 文件路径 |
| `how` | `struct open_how *` | 打开参数（flags + mode + resolve） |
| `usize` | `size_t` | how 结构体大小 |

**resolve 标志：**

| 标志 | 说明 |
|------|------|
| `RESOLVE_NO_XDEV` | 禁止跨设备 |
| `RESOLVE_NO_MAGICLINKS` | 禁止 magic 符号链接（如 `/proc/self/fd/`） |
| `RESOLVE_NO_SYMLINKS` | 禁止符号链接 |
| `RESOLVE_BENEATH` | 限制在 dfd 目录下 |
| `RESOLVE_IN_ROOT` | 以根目录为锚点 |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码

## 2. 使用场景

- **安全文件访问**: 防止路径遍历攻击
- **沙箱**: 限制文件访问范围
- **容器**: 确保文件访问不逃逸出容器根目录

## 3. 函数调用栈

```
openat2(dfd, filename, how, usize) (系统调用入口)
└─ ksys_openat2(dfd, filename, how, usize)             // fs/open.c
   ├─ copy_from_user(&tmp, how, usize)                  // 拷贝用户参数
   ├─ build_open_how(&tmp, &how)                        // 校验并构建 open_how
   │
   └─ do_sys_openat2(dfd, filename, &how)              // 核心入口
        └─ do_filp_open(dfd, tmp, &op)                 // 打开文件
             └─ path_openat(nd, file, op)              // 路径解析+打开
                  ├─ path_init(nd, dfd, name)           // 从 dfd 开始
                  ├─ [resolve 标志检查]
                  │  ├─ RESOLVE_NO_SYMLINKS → 禁止符号链接
                  │  ├─ RESOLVE_BENEATH → 检查路径在 dfd 下
                  │  └─ RESOLVE_NO_XDEV → 禁止跨设备
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
openat2(dirfd, "test.txt", {O_RDWR, 0, RESOLVE_BENEATH}, sizeof(open_how))
  │
  ├─ copy_from_user → 获取 open_how 参数
  │
  └─ do_sys_openat2()
       └─ path_openat()
            ├─ path_init(nd, dirfd, "test.txt")
            │
            ├─ [RESOLVE_BENEATH] → 检查路径不逃逸 dirfd
            │
            ├─ link_path_walk → 解析 "test.txt"
            │    └─ [RESOLVE_NO_SYMLINKS] → 禁止符号链接
            │
            └─ do_last → lookup_open + vfs_open + fd_install
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    struct open_how how = {
        .flags = O_RDWR | O_CREAT,
        .mode = 0644,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS,
    };
    int fd;
    // 使用 syscall 直接调用（glibc 可能未封装）
    fd = syscall(SYS_openat2, AT_FDCWD, "/tmp/test.txt", &how, sizeof(how));
    if (fd < 0) { perror("openat2"); return 1; }

    write(fd, "hello", 5);
    close(fd);
    printf("File opened with openat2\n");
    return 0;
}
```

## 7. 参考

- `fs/open.c` — openat2 实现
- `include/uapi/linux/openat2.h` — open_how 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)