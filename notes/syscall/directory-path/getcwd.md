# getcwd 系统调用分析

## 1. 概述

获取当前工作目录的绝对路径。

**原型：**

```c
SYSCALL_DEFINE2(getcwd, char __user *, buf, unsigned long, size)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `buf` | `char *` | 存放路径的缓冲区 |
| `size` | `unsigned long` | 缓冲区大小 |

**返回值：**

- 成功返回路径字符串长度（包括结尾的 `\0`）
- 失败返回负值错误码：
  - `-ERANGE` — 缓冲区太小
  - `-EFAULT` — 用户态指针无效

## 2. 使用场景

- **`pwd` 命令**: 获取当前工作目录
- **路径管理**: 程序需要保存或显示当前目录
- **日志记录**: 记录进程的工作目录

## 3. 函数调用栈

```
getcwd(buf, size) (系统调用入口)
└─ ksys_getcwd(buf, size)                              // fs/d_path.c
   └─ d_path(&current->fs->pwd, buf, size)             // 将路径转换为字符串
        └─ prepend_path()                               // 从 dentry 向上遍历到根
             ├─ 从当前 dentry 开始
             ├─ 逐级获取父 dentry 和分量名
             └─ 拼接到缓冲区前面
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
getcwd(buf, size)
  │
  └─ d_path(&current->fs->pwd, buf, size)
       │
       ├─ 从当前 dentry 开始
       ├─ 逐级向上遍历父目录
       │    ├─ 获取每个分量名
       │    └─ prepend 到缓冲区前面
       └─ 到达根 dentry 后返回
```

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 自动分配缓冲区
    char *cwd = getcwd(NULL, 0);
    if (cwd == NULL) {
        perror("getcwd");
        return 1;
    }
    printf("Current directory: %s\n", cwd);
    free(cwd);
    return 0;
}
```

## 7. 参考

- `fs/d_path.c` — getcwd 实现
- `include/linux/fs_struct.h` — fs_struct 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)