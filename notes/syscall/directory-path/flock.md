# flock 系统调用分析

## 1. 概述

在打开的文件上施加或释放建议性文件锁（advisory lock）。与 POSIX 记录锁（fcntl）不同，flock 锁整个文件且不可与 fcntl 锁混用。

**原型：**

```c
SYSCALL_DEFINE2(flock, unsigned int, fd, unsigned int, cmd)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符 |
| `cmd` | `unsigned int` | 操作命令 |

**cmd 命令：**

| 命令 | 说明 |
|------|------|
| `LOCK_SH` | 共享锁（多个进程可同时持有） |
| `LOCK_EX` | 排他锁（仅一个进程可持有） |
| `LOCK_UN` | 释放锁 |
| `LOCK_NB` | 非阻塞模式（与 LOCK_SH 或 LOCK_EX 组合使用） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的文件描述符
  - `-EINVAL` — 无效命令
  - `-EWOULDBLOCK` — 非阻塞模式下无法获取锁

## 2. 使用场景

- **进程同步**: 多个进程协调对同一文件的访问
- **配置文件**: 防止并发修改配置文件
- **守护进程**: 确保单实例运行（如 PID 文件加锁）

## 3. 函数调用栈

```
flock(fd, cmd) (系统调用入口)
└─ ksys_flock(fd, cmd)                                 // fs/locks.c
   └─ flock_make_lock(file, &fl, type)                  // 创建文件锁结构
        └─ locks_lock_file_wait(file, &fl)              // 获取锁（可能阻塞）
             └─ locks_lock_inode_wait(BF_INODE(file), &fl)
                  └─ __locks_lock_inode(inode, &fl)
                       └─ posix_lock_inode(inode, &fl)  // 实际锁操作
                            ├─ locks_find_conflict()    // 查找冲突锁
                            └─ locks_insert_lock()      // 插入锁记录
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
flock(fd, LOCK_EX)
  │
  ├─ 创建 file_lock 结构 (LOCK_EX 类型)
  │
  └─ locks_lock_file_wait()
       │
       ├─ locks_find_conflict() → 检查冲突锁
       │    ├─ 无冲突 → 插入锁记录
       │    └─ 有冲突 → 等待 (LOCK_NB 则返回 EWOULDBLOCK)
       │
       └─ 锁释放后唤醒等待进程
```

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>

int main(void)
{
    int fd = open("/tmp/lockfile", O_RDWR | O_CREAT, 0644);
    if (fd == -1) { perror("open"); return 1; }

    // 获取排他锁（阻塞模式）
    if (flock(fd, LOCK_EX) == -1) {
        perror("flock");
        return 1;
    }
    printf("Lock acquired\n");

    // 临界区操作...
    sleep(5);

    // 释放锁
    flock(fd, LOCK_UN);
    printf("Lock released\n");
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/locks.c` — flock 实现
- `include/linux/fs.h` — file_lock 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)