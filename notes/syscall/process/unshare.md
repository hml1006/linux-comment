# unshare 系统调用分析

## 1. 概述

`unshare` 系统调用允许进程将原本共享的某些资源（如地址空间、文件描述符表、文件系统信息、命名空间等）分离出来，创建独立的副本。与 `clone` 创建新进程时控制共享不同，`unshare` 在当前进程上操作，不创建新进程。

### 关键特点

- 在当前进程上操作，不创建新进程
- 通过 `CLONE_*` 标志指定要分离的资源
- 与 `clone` 共享相同的 `copy_*` 函数族
- 用于实现容器命名空间隔离、内存隔离等
- 某些标志有隐含依赖关系（如 `CLONE_NEWUSER` 隐含 `CLONE_THREAD | CLONE_FS`）

---

## 2. 函数原型

```c
#define _GNU_SOURCE
#include <sched.h>

int unshare(int flags);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `flags` | `CLONE_*` 标志位，指定要分离的资源 |

### 内核入口

```c
// kernel/fork.c:3242
SYSCALL_DEFINE1(unshare, unsigned long, unshare_flags)
{
    return ksys_unshare(unshare_flags);
}
```

---

## 3. 调用链分析

### 完整调用链

```
unshare(flags)
└─ syscall(__NR_unshare, flags)
   └─ SYSCALL_DEFINE1(unshare)                   // kernel/fork.c:3242
      └─ ksys_unshare(unshare_flags)              // kernel/fork.c:3123
         ├─ [CLONE_NEWUSER] → flags |= CLONE_THREAD | CLONE_FS
         ├─ [CLONE_VM] → flags |= CLONE_SIGHAND
         ├─ [CLONE_SIGHAND] → flags |= CLONE_THREAD
         ├─ [CLONE_NEWNS] → flags |= CLONE_FS
         ├─ check_unshare_flags(unshare_flags)     // 检查标志合法性
         ├─ [CLONE_NEWIPC|CLONE_SYSVSEM] → do_sysvsem = 1
         ├─ unshare_fs(unshare_flags, &new_fs)     // 分离 fs_struct
         │  └─ [CLONE_FS] → copy_fs_struct(fs)     // 创建独立副本
         ├─ unshare_fd(unshare_flags, &new_fd)     // 分离 fd 表
         │  └─ [CLONE_FILES] → dup_fd()            // 创建独立副本
         ├─ unshare_userns(unshare_flags, &new_cred)  // 分离用户命名空间
         │  └─ [CLONE_NEWUSER] → create_user_ns()
         ├─ unshare_nsproxy_namespaces(unshare_flags, &new_nsproxy, ...)
         │  └─ [CLONE_NEW*] → create_new_namespaces()  // 创建新命名空间
         ├─ unshare_vm(unshare_flags, &new_nsproxy)  // 分离地址空间
         │  └─ [CLONE_VM] → 无操作（unshare 不支持 CLONE_VM）
         ├─ 提交更改到当前进程
         │  ├─ [new_fs] → task->fs = new_fs
         │  ├─ [new_fd] → task->files = new_fd
         │  ├─ [new_cred] → commit_creds(new_cred)
         │  └─ [new_nsproxy] → task->nsproxy = new_nsproxy
         └─ 清理临时备份
```

### ksys_unshare 详细流程

```c
// kernel/fork.c:3123
int ksys_unshare(unsigned long unshare_flags)
{
    struct fs_struct *fs, *new_fs = NULL;
    struct files_struct *new_fd = NULL;
    struct cred *new_cred = NULL;
    struct nsproxy *new_nsproxy = NULL;
    int do_sysvsem = 0;
    int err;

    // 处理隐含依赖
    if (unshare_flags & CLONE_NEWUSER)
        unshare_flags |= CLONE_THREAD | CLONE_FS;
    if (unshare_flags & CLONE_VM)
        unshare_flags |= CLONE_SIGHAND;
    if (unshare_flags & CLONE_SIGHAND)
        unshare_flags |= CLONE_THREAD;
    if (unshare_flags & CLONE_NEWNS)
        unshare_flags |= CLONE_FS;

    // 检查标志合法性
    err = check_unshare_flags(unshare_flags);
    if (err)
        goto bad_unshare_out;

    // 分离各子系统
    err = unshare_fs(unshare_flags, &new_fs);
    if (err)
        goto bad_unshare_out;
    err = unshare_fd(unshare_flags, &new_fd);
    if (err)
        goto bad_unshare_cleanup_fs;
    err = unshare_userns(unshare_flags, &new_cred);
    if (err)
        goto bad_unshare_cleanup_fd;
    err = unshare_nsproxy_namespaces(unshare_flags, &new_nsproxy,
                                     new_cred, new_fs);
    if (err)
        goto bad_unshare_cleanup_cred;

    // 提交更改到当前进程
    if (new_fs) {
        fs = current->fs;
        current->fs = new_fs;
        free_fs_struct(fs);
    }
    if (new_fd) {
        struct files_struct *old_fd;
        task_lock(current);
        old_fd = current->files;
        current->files = new_fd;
        task_unlock(current);
        put_files_struct(old_fd);
    }
    if (new_nsproxy) {
        switch_task_namespaces(current, new_nsproxy);
    }
    if (new_cred)
        commit_creds(new_cred);

    return 0;

    // 错误处理：清理已分配的资源
bad_unshare_cleanup_cred:
    put_cred(new_cred);
bad_unshare_cleanup_fd:
    if (new_fd)
        put_files_struct(new_fd);
bad_unshare_cleanup_fs:
    if (new_fs)
        free_fs_struct(new_fs);
bad_unshare_out:
    return err;
}
```

---

## 4. 关键数据结构

```c
// ========== unshare 支持的主要标志 (include/uapi/linux/sched.h) ==========

#define CLONE_VM        0x00000100  // 分离地址空间（mm_struct）
#define CLONE_FS        0x00000200  // 分离文件系统信息（fs_struct）
#define CLONE_FILES     0x00000400  // 分离文件描述符表（files_struct）
#define CLONE_SIGHAND   0x00000800  // 分离信号处理函数（sighand_struct）
#define CLONE_THREAD    0x00010000  // 分离线程组（创建独立线程）
#define CLONE_NEWNS     0x00020000  // 分离挂载命名空间
#define CLONE_SYSVSEM   0x00040000  // 分离 SysV 信号量
#define CLONE_NEWCGROUP 0x02000000  // 分离 cgroup 命名空间
#define CLONE_NEWUTS    0x04000000  // 分离 UTS 命名空间
#define CLONE_NEWIPC    0x08000000  // 分离 IPC 命名空间
#define CLONE_NEWUSER   0x10000000  // 分离用户命名空间
#define CLONE_NEWPID    0x20000000  // 分离 PID 命名空间
#define CLONE_NEWNET    0x40000000  // 分离网络命名空间
#define CLONE_NEWTIME   0x00000080  // 分离时间命名空间

// ========== 隐含依赖关系 ==========

// CLONE_NEWUSER → CLONE_THREAD | CLONE_FS
// CLONE_VM → CLONE_SIGHAND
// CLONE_SIGHAND → CLONE_THREAD
// CLONE_NEWNS → CLONE_FS
```

---

## 5. 流程图

```
                     unshare(flags)
                        |
                +-------v--------+
                | SYSCALL_DEFINE1 |
                | (kernel/fork.c) |
                +-------+--------+
                        |
                +-------v--------+
                | ksys_unshare() |
                +-------+--------+
                        |
        +---------------+---------------+
        |                               |
        | 处理隐含依赖                   |
        | (补充必要的 CLONE_* 标志)      |
        +-------------------------------+
                        |
        +---------------+---------------+
        |                               |
        | 创建各子系统独立副本            |
        |                               |
        | unshare_fs()                  |
        |   → [CLONE_FS] → 新 fs_struct|
        |                               |
        | unshare_fd()                  |
        |   → [CLONE_FILES] → 新 fd 表  |
        |                               |
        | unshare_userns()              |
        |   → [CLONE_NEWUSER] → 新 cred |
        |                               |
        | unshare_nsproxy_namespaces()  |
        |   → [CLONE_NEW*] → 新命名空间  |
        +-------------------------------+
                        |
        +---------------+---------------+
        |                               |
        | 提交更改到当前进程             |
        |  - current->fs = new_fs       |
        |  - current->files = new_fd    |
        |  - current->nsproxy = new_ns  |
        |  - commit_creds(new_cred)     |
        |                               |
        | 清理旧资源                    |
        |  - free_fs_struct(old_fs)     |
        |  - put_files_struct(old_fd)   |
        |  - switch_task_namespaces()   |
        +-------------------------------+
                        |
                +-------v--------+
                | 返回 0 (成功)  |
                +----------------+
```

---

## 6. 错误处理

| 错误码 | 条件 | 触发位置 |
|--------|------|----------|
| `-EINVAL` | 设置了无效的 `CLONE_*` 标志 | `check_unshare_flags` |
| `-EINVAL` | 设置了 `CLONE_THREAD` 但未设置 `CLONE_SIGHAND`（线程上下文） | `check_unshare_flags` |
| `-EINVAL` | 设置了 `CLONE_NEWPID` 但未设置 `CLONE_THREAD` | `check_unshare_flags` |
| `-ENOMEM` | 内存不足，无法分配新结构 | `unshare_fs`/`unshare_fd` 等 |
| `-EUSERS` | 用户命名空间限制（`CLONE_NEWUSER`） | `unshare_userns` |
| `-EPERM` | 权限不足（`CLONE_NEW*` 需要 `CAP_SYS_ADMIN`） | `unshare_nsproxy_namespaces` |

---

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

int main() {
    // 示例1: 分离文件描述符表
    // 之后当前进程的 fd 操作不会影响其他线程
    printf("=== 分离文件描述符表 ===\n");
    int fd = open("/tmp/test.txt", O_CREAT | O_RDWR, 0644);
    printf("打开 fd=%d\n", fd);

    if (unshare(CLONE_FILES) == -1) {
        perror("unshare(CLONE_FILES)");
    } else {
        printf("文件描述符表已分离\n");
        // 现在 close(fd) 不会影响其他线程的 fd 表
    }

    // 示例2: 创建新的挂载命名空间（需要 CAP_SYS_ADMIN）
    // if (unshare(CLONE_NEWNS) == -1)
    //     perror("unshare(CLONE_NEWNS)");

    // 示例3: 创建新用户命名空间（需要 CAP_SYS_ADMIN）
    // if (unshare(CLONE_NEWUSER) == -1)
    //     perror("unshare(CLONE_NEWUSER)");

    close(fd);
    return 0;
}
```

---

## 8. unshare 与 clone 对比

| 特性 | unshare | clone |
|------|---------|-------|
| **是否创建新进程** | 否 | 是 |
| **操作对象** | 当前进程 | 创建新进程 |
| **CLONE_VM** | 不支持（返回 -EINVAL） | 支持（创建线程） |
| **CLONE_THREAD** | 支持（但有限制） | 支持 |
| **命名空间** | 创建新命名空间（容器） | 创建新命名空间 + 新进程 |
| **使用场景** | 容器命名空间隔离 | 创建进程/线程 |
| **内核实现** | `ksys_unshare` | `kernel_clone` |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/fork.c:3123` - ksys_unshare 实现
- `kernel/fork.c:3242` - SYSCALL_DEFINE1(unshare)
- `include/uapi/linux/sched.h` - CLONE_* 标志定义