# execveat 系统调用分析

## 1. 概述

`execveat` 是 `execve` 的扩展版本，属于 `*at` 系列系统调用。它允许通过文件描述符指定目录来解析要执行的文件路径，支持更多标志位控制执行行为。

### 关键特点

- 通过 `dfd` 参数指定目录文件描述符，支持相对路径解析
- 支持 `AT_EMPTY_PATH` 标志（当 `dfd` 指向已打开的可执行文件时）
- 支持 `AT_SYMLINK_NOFOLLOW` 标志（不跟随符号链接）
- 与 `execve` 共享相同的 `do_execveat_common` 核心实现

---

## 2. 函数原型

```c
#define _GNU_SOURCE
#include <unistd.h>

int execveat(int dirfd, const char *pathname,
             char *const argv[], char *const envp[],
             int flags);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `dirfd` | 目录文件描述符（用于相对路径解析） |
| `pathname` | 要执行的文件路径 |
| `argv` | 传递给新程序的参数数组 |
| `envp` | 环境变量数组 |
| `flags` | 标志位：`AT_EMPTY_PATH`、`AT_SYMLINK_NOFOLLOW` |

### 内核入口

```c
// fs/exec.c:1934
SYSCALL_DEFINE5(execveat,
        int, fd, const char __user *, filename,
        const char __user *const __user *, argv,
        const char __user *const __user *, envp,
        int, flags)
{
    CLASS(filename_uflags, name)(filename, flags);
    return do_execveat_common(fd, name,
                              native_arg(argv), native_arg(envp), flags);
}
```

---

## 3. 调用链分析

### 完整调用链

```
execveat(dirfd, pathname, argv, envp, flags)
└─ syscall(__NR_execveat, dirfd, pathname, argv, envp, flags)
   └─ SYSCALL_DEFINE5(execveat)                      // fs/exec.c:1934
      └─ do_execveat_common(fd, filename, argv, envp, flags)  // fs/exec.c:1778
         ├─ [同 execve 的 do_execveat_common 路径]
         │  ├─ bprm_mm_init(bprm)                    // 初始化新地址空间
         │  ├─ do_open_execat(fd, filename, flags)   // 打开文件
         │  │  └─ [AT_EMPTY_PATH] → 直接使用 fd 对应的 file
         │  │  └─ [!AT_EMPTY_PATH] → 按路径打开
         │  ├─ copy_strings(...)                     // 拷贝参数/环境变量
         │  ├─ exec_binprm(bprm, argv, envp)         // 执行二进制
         │  │  └─ search_binary_handler(bprm)
         │  │     └─ load_elf_binary(bprm)           // 加载 ELF
         │  │        ├─ elf_map(...) → do_mmap       // 映射 PT_LOAD 段
         │  │        ├─ de_thread(me)                // 单线程化
         │  │        ├─ exec_mmap(bprm->mm)          // 切换地址空间
         │  │        ├─ exec_fd清理 → close_on_exec  // 关闭 exec 时关闭的 fd
         │  │        └─ start_thread(regs, elf_entry, bprm->p)
         │  └─ audit_bprm(bprm)
         └─ 返回 0（成功，不返回用户态）
```

### do_open_execat 详细流程

```c
// fs/exec.c:920
static struct file *do_open_execat(int fd, struct filename *name, unsigned flags)
{
    struct file *file;
    int err;
    struct open_flags open_exec_flags = {
        .open_flag = O_LARGEFILE | O_RDONLY | __FMODE_EXEC,
        .acc_mode = MAY_EXEC,
        .intent = LOOKUP_OPEN,
        .lookup_flags = LOOKUP_FOLLOW,
    };

    // AT_EMPTY_PATH: 使用已打开的文件描述符
    if ((flags & AT_EMPTY_PATH) && fd == AT_EMPTY_PATH) {
        // 处理 AT_EMPTY_PATH 标志
    }

    // 正常路径：按文件名打开
    file = file_open_name(name, open_exec_flags);
    if (IS_ERR(file))
        return file;

    // 禁止写入（防止执行期间被修改）
    err = deny_write_access(file);
    if (err) {
        fput(file);
        return ERR_PTR(err);
    }

    return file;
}
```

---

## 4. 关键数据结构

```c
// ========== 打开标志 (fs/open.c) ==========

// 执行文件打开标志
// 以 O_RDONLY 打开，设置 __FMODE_EXEC 标记执行模式
// 通过 deny_write_access() 禁止写入
struct open_flags open_exec_flags = {
    .open_flag = O_LARGEFILE | O_RDONLY | __FMODE_EXEC,
    .acc_mode = MAY_EXEC,
    .intent = LOOKUP_OPEN,
    .lookup_flags = LOOKUP_FOLLOW,
};

// ========== execveat 支持的标志 (include/uapi/linux/fcntl.h) ==========

#define AT_EMPTY_PATH       0x1000  // 允许通过空路径名使用 dirfd 指向的文件
#define AT_SYMLINK_NOFOLLOW 0x100   // 不跟随符号链接
```

---

## 5. 流程图

```
                    execveat(dirfd, pathname, argv, envp, flags)
                                      |
                            +---------v----------+
                            | SYSCALL_DEFINE5     |
                            | (fs/exec.c)         |
                            +---------+----------+
                                      |
                            +---------v----------+
                            | do_execveat_common  |
                            | (fd, filename, ...) |
                            +---------+----------+
                                      |
                   +------------------+------------------+
                   |                                     |
            +------v------+                      +------v------+
            | alloc_bprm  |                      | exec_binprm |
            | + bprm_mm_  |                      | (核心执行)  |
            |   init      |                      +------+------+
            +------+------+                             |
                   |                            +------v------+
            +------v------+                     |load_elf_    |
            |do_open_execat|                     |binary       |
            |(fd, filename,|                     |(ELF 加载器) |
            | flags)       |                     +------+------+
            +------+------+                            |
                   |                            +------v------+
            +------v------+                     |start_thread |
            |copy_strings  |                     |(设置 pc, sp)|
            |(参数/环境变量)|                     |(新程序开始)  |
            +------+------+                     +------+------+
                   |                                     |
                   +------------------+------------------+
                                      |
                             +--------v--------+
                             | 成功: 不返回     |
                             | 失败: 返回错误码 |
                             +-----------------+
```

---

## 6. 错误处理

| 错误码 | 条件 | 触发位置 |
|--------|------|----------|
| `-EBADF` | `dfd` 不是有效的文件描述符 | `do_open_execat` |
| `-EINVAL` | flags 包含无效标志 | `do_execveat_common` |
| `-EINVAL` | 设置了 `AT_EMPTY_PATH` 但 `dfd` 不是 `AT_EMPTY_PATH` | `do_open_execat` |
| `-EACCES` | 文件不可执行 | `do_open_execat` |
| `-ETXTBSY` | 文件正在被写入 | `deny_write_access` |
| `-ENOENT` | 文件不存在 | `do_open_execat` |
| `-ELOOP` | 符号链接循环（未设置 `AT_SYMLINK_NOFOLLOW`） | 路径查找 |

---

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

int main() {
    // 使用 AT_EMPTY_PATH 通过 fd 执行文件
    int fd = open("/bin/ls", O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    char *argv[] = {"/bin/ls", "-l", NULL};
    char *envp[] = {"PATH=/usr/bin", NULL};

    printf("通过 fd %d 执行 /bin/ls...\n", fd);

    int ret = execveat(fd, "", argv, envp, AT_EMPTY_PATH);
    // 只有失败时才会执行到这里
    perror("execveat");
    close(fd);
    return 1;
}
```

---

## 8. 与 execve 对比

| 特性 | execve | execveat |
|------|--------|----------|
| **路径解析** | 从当前工作目录开始 | 通过 `dirfd` 指定目录 |
| **fd 参数** | 无 | 支持 `dirfd` |
| **AT_EMPTY_PATH** | 不支持 | 支持（通过 fd 执行已打开文件） |
| **AT_SYMLINK_NOFOLLOW** | 不支持 | 支持 |
| **系统调用号** | 221（ARM64） | 358（ARM64） |
| **内核版本** | 自始存在 | Linux 3.19+ |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `fs/exec.c:1934` - SYSCALL_DEFINE5(execveat)
- `fs/exec.c:1778` - do_execveat_common
- `include/uapi/linux/fcntl.h` - AT_* 标志定义