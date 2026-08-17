# ioctl 系统调用分析

## 1. 概述

设备 I/O 控制。向设备驱动程序发送控制命令，执行标准的 read/write 无法完成的操作。

**原型：**

```c
SYSCALL_DEFINE3(ioctl, unsigned int, fd, unsigned int, cmd, unsigned long, arg)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符 |
| `cmd` | `unsigned int` | 设备特定命令码 |
| `arg` | `unsigned long` | 命令参数（通常为指针） |

**返回值：**

- 成功返回 `0`（部分命令可能返回正数）
- 失败返回负值错误码：
  - `-EBADF` — 无效的 fd
  - `-ENOTTY` — fd 不支持 ioctl
  - `-EINVAL` — 无效命令

## 2. 使用场景

- **终端控制**: `TIOCGWINSZ` 获取终端大小
- **网络设备**: `SIOCGIFADDR` 获取网络接口地址
- **块设备**: `BLKGETSIZE64` 获取设备大小
- **驱动控制**: 自定义设备驱动命令

## 3. 函数调用栈

```
ioctl(fd, cmd, arg) (系统调用入口)
└─ ksys_ioctl(fd, cmd, arg)                            // fs/ioctl.c
   └─ do_vfs_ioctl(file, cmd, arg)                     // 分发 ioctl 请求
        ├─ [通用 cmd 处理]
        │  ├─ FIONCLEX → 清除 close-on-exec 标志
        │  ├─ FIOCLEX  → 设置 close-on-exec 标志
        │  ├─ FIONBIO  → 设置非阻塞 I/O
        │  ├─ FIOASYNC → 设置异步 I/O
        │  └─ FIONREAD → 获取可读字节数
        │
        └─ vfs_ioctl(file, cmd, arg)                   // 设备特定 ioctl
             └─ file->f_op->unlocked_ioctl(file, cmd, arg) // 驱动实现
                  └─ 设备驱动 switch(cmd) 处理
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
ioctl(fd, cmd, arg)
  │
  v
do_vfs_ioctl(file, cmd, arg)
  │
  ├─ 是通用 cmd?
  │    ├─ FIONCLEX/FIOCLEX → 设置 close-on-exec
  │    ├─ FIONBIO → 设置非阻塞
  │    └─ FIONREAD → 返回可读字节数
  │
  └─ 否 → vfs_ioctl()
       └─ file->f_op->unlocked_ioctl(file, cmd, arg)
            └─ 设备驱动 switch cmd 处理
```

## 6. 使用示例

```c
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/fs.h>

int main(void)
{
    int fd = open("/dev/nvme0n1", O_RDONLY);
    if (fd == -1) { perror("open"); return 1; }

    unsigned long long size;
    // 获取块设备大小
    if (ioctl(fd, BLKGETSIZE64, &size) == -1) {
        perror("ioctl");
        return 1;
    }
    printf("Device size: %llu bytes\n", size);
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/ioctl.c` — ioctl 实现
- `include/uapi/linux/fs.h` — 通用 ioctl 命令定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)