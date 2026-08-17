# kcmp 系统调用分析

## 1. 概述

`kcmp` 比较两个进程的内核资源（如文件描述符、内存地址空间、文件系统等）是否指向同一内核对象。用于检查两个进程是否共享某些资源，常用于 `pidfd_getfd` 测试和 `checkpoint/restore` 场景。

**原型：**

```c
SYSCALL_DEFINE5(kcmp, pid_t, pid1, pid_t, pid2, int, type,
                unsigned long, idx1, unsigned long, idx2)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `pid1` | `pid_t` | 第一个进程的 PID |
| `pid2` | `pid_t` | 第二个进程的 PID |
| `type` | `int` | 比较类型（`KCMP_*` 枚举值） |
| `idx1` | `unsigned long` | 第一个进程的索引（取决于 type） |
| `idx2` | `unsigned long` | 第二个进程的索引（取决于 type） |

**返回值：**
- 0：两个对象相同（指向同一内核对象）
- 1：obj1 小于 obj2（用于排序）
- 2：obj1 大于 obj2（用于排序）
- 负的错误码

## 2. 使用场景

- 检查两个进程是否共享同一文件描述符
- 检查两个进程是否在同一个进程中（`KCMP_VM`）
- 容器运行时检查进程关系
- CRIU（Checkpoint/Restore In Userspace）检查进程资源
- 调试和诊断工具
- `pidfd_getfd` 测试用例中验证 fd 复制

## 3. 函数调用栈

```
SYSCALL_DEFINE5(kcmp, pid1, pid2, type, idx1, idx2)    // kernel/kcmp.c
  ├─ rcu_read_lock()
  ├─ find_task_by_vpid(pid1) / find_task_by_vpid(pid2)  // 通过 PID 查找 task_struct
  │    找不到 → 返回 -ESRCH
  ├─ get_task_struct(task1) / get_task_struct(task2)    // 增加引用计数
  ├─ rcu_read_unlock()
  ├─ kcmp_lock(&task1->exec_update_lock, &task2->exec_update_lock) // 获取 exec 锁
  ├─ ptrace_may_access(task1, PTRACE_MODE_READ_REALCREDS) // 权限检查
  │    无权限 → 返回 -EPERM
  ├─ switch (type) {
  │    case KCMP_FILE:     kcmp_ptr(filp1, filp2, KCMP_FILE)      // 文件对象
  │    case KCMP_VM:       kcmp_ptr(mm1, mm2, KCMP_VM)            // 内存地址空间
  │    case KCMP_FILES:    kcmp_ptr(files1, files2, KCMP_FILES)   // 文件描述符表
  │    case KCMP_FS:       kcmp_ptr(fs1, fs2, KCMP_FS)            // 文件系统上下文
  │    case KCMP_SIGHAND:  kcmp_ptr(sighand1, sighand2, ...)      // 信号处理表
  │    case KCMP_IO:       kcmp_ptr(io_context1, io_context2, ...) // IO 上下文
  │    case KCMP_SYSVSEM:  kcmp_ptr(undo_list1, undo_list2, ...)  // SystemV 信号量
  │    case KCMP_EPOLL_TFD: kcmp_epoll_target(...)                // epoll 目标文件
  │    default:            ret = -EINVAL
  │  }
  ├─ kcmp_unlock(...)                                            // 释放 exec 锁
  ├─ put_task_struct(task1) / put_task_struct(task2)              // 释放引用
  └─ return ret
```

### 3.1 kcmp_ptr 内核对象比较函数

```c
// kernel/kcmp.c
static int kcmp_ptr(void *v1, void *v2, enum kcmp_type type)
{
    long ret;
    // 使用 cookies 数组对指针进行混淆（安全原因，不暴露真实内核内存地址）
    // 但保持比较结果的一致性，可用于排序
    ret = (unsigned long)v1 - (unsigned long)v2;
    // 根据 hash 后的指针值返回 0/1/2
    return (ret < 0) | ((ret > 0) << 1);
}
```

### 3.2 kcmp_epoll_target 处理

```c
// kernel/kcmp.c (CONFIG_EPOLL)
static int kcmp_epoll_target(struct task_struct *task1,
                             struct task_struct *task2,
                             unsigned long idx1,
                             struct kcmp_epoll_slot __user *uslot)
{
    // 1. 从 task1 获取 idx1 对应的文件
    // 2. 从 uslot 解析 epoll slot（efd, tfd, toff）
    // 3. 从 task2 的 efd 对应的 epoll 实例中查找 tfd 文件
    // 4. 比较两个文件是否相同
}
```

## 4. 关键数据结构

### 4.1 kcmp_type 枚举

```c
// include/uapi/linux/kcmp.h
enum kcmp_type {
    KCMP_FILE,          // 比较文件对象（struct file）
    KCMP_VM,            // 比较内存地址空间（struct mm_struct）
    KCMP_FILES,         // 比较文件描述符表（struct files_struct）
    KCMP_FS,            // 比较文件系统上下文（struct fs_struct）
    KCMP_SIGHAND,       // 比较信号处理表（struct sighand_struct）
    KCMP_IO,            // 比较 IO 上下文（struct io_context）
    KCMP_SYSVSEM,       // 比较 SystemV 信号量撤销列表
    KCMP_EPOLL_TFD,     // 比较 epoll 中的目标文件描述符

    KCMP_TYPES,         // 类型总数
};
```

### 4.2 struct kcmp_epoll_slot（epoll 比较槽位）

```c
// include/uapi/linux/kcmp.h
struct kcmp_epoll_slot {
    __u32 efd;      // epoll 文件描述符
    __u32 tfd;      // 目标文件编号
    __u32 toff;     // 目标在相同编号序列中的偏移
};
```

### 4.3 指针混淆机制

```c
// kernel/kcmp.c
static unsigned long cookies[KCMP_TYPES][2] __read_mostly;

static __init int kcmp_cookies_init(void)
{
    int i;
    get_random_bytes(cookies, sizeof(cookies));
    for (i = 0; i < KCMP_TYPES; i++)
        cookies[i][1] |= (~(~0UL >> 1) | 1);  // 确保是奇数
    return 0;
}
arch_initcall(kcmp_cookies_init);
```

为了保护内核地址安全，`kcmp` 不会暴露真实的内核指针：
1. 将内核指针与随机值 XOR
2. 乘以一个大奇数（确保唯一性）
3. 比较混淆后的值

## 5. 流程图

```
用户态调用 kcmp(pid1, pid2, type, idx1, idx2)
    │
    ▼
┌─────────────────────────────────────────┐
│  find_task_by_vpid() 查找两个进程       │
│  找不到 → 返回 -ESRCH                   │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  kcmp_lock() 获取 exec 锁               │
│  ptrace_may_access() 权限检查           │
│  无权限 → 返回 -EPERM                   │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  根据 type 选择比较操作                 │
│                                         │
│  KCMP_FILE  → 根据 idx1/idx2 查找 fd   │
│  KCMP_VM    → 比较 mm_struct 指针       │
│  KCMP_FILES → 比较 files_struct 指针    │
│  KCMP_FS    → 比较 fs_struct 指针       │
│  KCMP_SIGHAND → 比较 sighand_struct     │
│  KCMP_IO    → 比较 io_context 指针      │
│  KCMP_SYSVSEM → 比较 sysvsem undo_list  │
│  KCMP_EPOLL_TFD → epoll 目标比较       │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  kcmp_ptr() / kcmp_epoll_target()      │
│  → 返回 0 (相同) / 1 (小于) / 2 (大于) │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  kcmp_unlock() 释放锁                  │
│  put_task_struct() 释放引用            │
│  返回结果                               │
└─────────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-ESRCH` | 进程不存在 | `find_task_by_vpid()` 找不到指定 PID |
| `-EPERM` | 权限不足 | `ptrace_may_access()` 检查失败 |
| `-EBADF` | 文件描述符无效 | `KCMP_FILE` 类型时 `idx1`/`idx2` 对应的 fd 无效 |
| `-EINVAL` | 无效参数 | `type` 超出 `KCMP_TYPES` 范围 |
| `-EFAULT` | 地址错误 | `KCMP_EPOLL_TFD` 时 `uslot` 拷贝失败 |
| `-EOPNOTSUPP` | 不支持 | `KCMP_EPOLL_TFD` 但内核未配置 `CONFIG_EPOLL`；或 `KCMP_SYSVSEM` 但未配置 `CONFIG_SYSVIPC` |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/kcmp.h>
#include <fcntl.h>
#include <sys/wait.h>

int main(void)
{
    int fd = open("/tmp/testfile", O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程 */
        int fd2 = open("/tmp/testfile", O_RDWR);
        // 比较父子进程的 KCMP_VM（应该不同 → 返回值非0）
        int ret = syscall(SYS_kcmp, getpid(), getppid(),
                          KCMP_VM, 0, 0);
        printf("VM compare: %d\n", ret);  // 非0表示不同

        // 比较父子进程的 KCMP_FILES（应该不同 → 返回值非0）
        ret = syscall(SYS_kcmp, getpid(), getppid(),
                      KCMP_FILES, 0, 0);
        printf("FILES compare: %d\n", ret);  // 非0表示不同

        // 比较同一个进程的文件描述符
        ret = syscall(SYS_kcmp, getpid(), getpid(),
                      KCMP_FILE, fd, fd);
        printf("Same file self compare: %d\n", ret);  // 0表示相同

        // 比较父子进程的不同 fd（指向同一文件，应该相同）
        ret = syscall(SYS_kcmp, getpid(), getppid(),
                      KCMP_FILE, fd2, fd);
        printf("Cross-process same file: %d\n", ret);  // 0表示相同

        close(fd2);
        exit(0);
    }

    wait(NULL);
    close(fd);
    unlink("/tmp/testfile");
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#用户与组关系)
- 源码: `kernel/kcmp.c`
- 头文件: `include/uapi/linux/kcmp.h`
- 测试用例: `tools/testing/selftests/kcmp/kcmp_test.c`
- 相关系统调用: `pidfd_getfd()`, `ptrace()`