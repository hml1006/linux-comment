# umask 系统调用

## 原理与功能

`umask` 系统调用设置进程的文件创建掩码。当进程创建新文件或目录时，指定的权限位会与 umask 进行与运算，umask 中置 1 的位会被清除。这是系统安全机制的一部分，防止进程意外创建权限过宽的文件。

### 功能说明

- 设置进程级文件创建掩码
- 影响 `open`、`creat`、`mkdir` 等系统调用创建的权限
- 对新文件的权限过滤：`实际权限 = 请求权限 & ~umask`

## 使用场景

- 确保新文件不包含意外权限（如 shell 默认 umask 022）
- 提高临时文件的保密性（umask 077）
- 守护进程设置严格 umask 确保安全

## API 及使用案例

### 函数原型

```c
#include <sys/types.h>
#include <sys/stat.h>

mode_t umask(mode_t mask);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `mask` | `mode_t` | 新的文件创建掩码 |

### 返回值

- 返回之前的 umask 值（以便恢复）

### 使用示例

```c
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    // 保存当前 umask，并设置为 077（仅所有者可访问）
    mode_t old_mask = umask(077);
    printf("原 umask: %04o, 新 umask: 0077\n", old_mask);

    // 创建文件（请求 0666，但实际受 umask 影响）
    // 实际权限 = 0666 & ~077 = 0600
    int fd = open("secure.txt", O_WRONLY | O_CREAT, 0666);
    if (fd >= 0) {
        struct stat sb;
        fstat(fd, &sb);
        printf("文件实际权限: %04o\n", sb.st_mode & 07777);
        close(fd);
    }

    // 恢复原始 umask
    umask(old_mask);
    printf("已恢复 umask 为 %04o\n", old_mask);

    return 0;
}
```

## 执行流程

```
umask(mask)                        内核
    |                               |
    |-----> syscall(#60) ---------->|
    |       __arm64_sys_umask()     |
    |                               |
    |    +----------------------+   |
    |    | umask(mask)          |   |
    |    | fs/namei.c           |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | current->fs->umask   |   |
    |    | = mask & S_IRWXUGO   |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | 返回旧的 umask 值    |   |
    |    +---------+------------+   |
    |              |                |
    |<---- 返回 old_mask ----------+|
    |                               |
```

## 函数调用栈

```
umask(mask)
  └─ syscall(__NR_umask, mask)
       └─ __arm64_sys_umask()                    // arch/arm64/kernel/syscall.c
            └─ umask(mask)                        // fs/namei.c
                 ├─ old = current->fs->umask
                 ├─ current->fs->umask = mask & S_IRWXUGO
                 └─ return old
```

## 关键数据结构

### struct fs_struct（进程文件系统上下文）

```c
// include/linux/fs_struct.h
struct fs_struct {
    struct path       pwd;          // 当前工作目录
    struct path       root;         // 根目录
    int               users;        // 引用计数
    rwlock_t          lock;         // 保护锁
    int               in_exec;      // 是否正在执行 exec
    umode_t           umask;        // 文件创建掩码
};
```

## 备注

- ARM64 系统调用号为 #60
- umask 影响 `open`、`creat`、`mkdir`、`mknod` 等调用
- 常见 umask 值：022（默认）、077（严格）、002（宽松）
- `umask(0)` 表示不屏蔽任何权限位
- umask 只影响当前进程，fork 后子进程继承