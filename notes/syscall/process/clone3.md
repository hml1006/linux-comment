# clone3 系统调用分析

## 1. 概述

`clone3` 是 `clone` 的扩展版本，使用结构体参数替代寄存器参数，解决了传统 `clone` 因寄存器数量限制而难以扩展的问题。它支持通过 `clone_args` 结构体版本号机制向后兼容，是创建新进程的推荐方式。

### 关键特点

- 使用 `struct clone_args` 结构体传递参数，`size` 参数标识结构体版本
- 支持 `CLONE_PIDFD` 原生返回 pidfd
- 支持通过 `set_tid`/`set_tid_size` 指定新进程的 PID
- 支持通过 `cgroup` 字段将新进程直接迁移到指定 cgroup
- 与 `clone` 共享相同的 `kernel_clone` → `copy_process` 核心路径

---

## 2. 函数原型

```c
#define _GNU_SOURCE
#include <sched.h>

int clone3(struct clone_args *args, size_t size);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `args` | 指向 `struct clone_args` 的指针 |
| `size` | `args` 结构体的大小，用于版本兼容 |

### 内核入口

```c
// kernel/fork.c:2934
SYSCALL_DEFINE2(clone3, struct clone_args __user *, uargs, size_t, size)
{
    int err;
    struct kernel_clone_args kargs;
    pid_t set_tid[MAX_PID_NS_LEVEL];

    kargs.set_tid = set_tid;

    err = copy_clone_args_from_user(&kargs, uargs, size);
    if (err)
        return err;

    if (!clone3_args_valid(&kargs))
        return -EINVAL;

    return kernel_clone(&kargs);
}
```

---

## 3. 调用链分析

### 完整调用链

```
clone3(uargs, size)
└─ syscall(__NR_clone3, uargs, size)
   └─ SYSCALL_DEFINE2(clone3)                       // kernel/fork.c:2934
      ├─ copy_clone_args_from_user(&kargs, uargs, size)  // 从用户态拷贝参数
      │  ├─ [size == CLONE_ARGS_SIZE_VER0]  → 64字节版本
      │  ├─ [size == CLONE_ARGS_SIZE_VER1]  → 80字节版本（+ set_tid, set_tid_size）
      │  ├─ [size == CLONE_ARGS_SIZE_VER2]  → 88字节版本（+ cgroup）
      │  └─ copy_struct_from_user(kargs, uargs, size)
      ├─ clone3_args_valid(&kargs)                 // 验证参数合法性
      │  └─ 检查 flags 中 CSIGNAL 位是否清零
      │  └─ 检查 exit_signal 是否合法
      │  └─ 检查 set_tid_size 是否合法
      └─ kernel_clone(&kargs)                      // 统一创建路径
         └─ [同 clone 的 kernel_clone 路径]
            ├─ copy_process(NULL, trace, node, args)
            ├─ wake_up_new_task(p)
            └─ [CLONE_VFORK] → wait_for_vfork_done()
```

### copy_clone_args_from_user 详细流程

```c
// kernel/fork.c:2798
static noinline int copy_clone_args_from_user(
    struct kernel_clone_args *kargs,
    struct clone_args __user *uargs, size_t size)
{
    struct clone_args args;

    // 从用户空间拷贝结构体
    if (copy_struct_from_user(&args, sizeof(args), uargs, size))
        return -EFAULT;

    // 转换字段
    kargs->flags        = args.flags;
    kargs->pidfd        = u64_to_user_ptr(args.pidfd);
    kargs->child_tid    = u64_to_user_ptr(args.child_tid);
    kargs->parent_tid   = u64_to_user_ptr(args.parent_tid);
    kargs->exit_signal  = args.exit_signal;
    kargs->stack        = args.stack;
    kargs->stack_size   = args.stack_size;
    kargs->tls          = args.tls;

    // 从 set_tid 数组拷贝（如果 size >= VER1）
    if (size > CLONE_ARGS_SIZE_VER0)
        copy_from_user(kargs->set_tid, u64_to_user_ptr(args.set_tid),
                       args.set_tid_size * sizeof(pid_t));

    // cgroup 字段（如果 size >= VER2）
    if (size > CLONE_ARGS_SIZE_VER1)
        kargs->cgroup = args.cgroup;

    return 0;
}
```

---

## 4. 关键数据结构

```c
// ========== 用户态 clone_args (include/uapi/linux/sched.h) ==========

/**
 * struct clone_args - arguments for the clone3 syscall
 * @flags:        Flags for the new process as listed above.
 * @pidfd:        If CLONE_PIDFD is set, a pidfd will be returned here.
 * @child_tid:    If CLONE_CHILD_SETTID is set, child TID returned here.
 * @parent_tid:   If CLONE_PARENT_SETTID is set, child TID returned here.
 * @exit_signal:  The exit_signal sent to parent when child exits.
 * @stack:        Stack pointer for the child process (lowest address).
 * @stack_size:   Size of the stack for the child process.
 * @tls:          TLS descriptor if CLONE_SETTLS is set.
 * @set_tid:      Pointer to array of PIDs for each PID namespace level.
 * @set_tid_size: Number of elements in set_tid array.
 * @cgroup:       File descriptor for cgroup to migrate to.
 */
struct clone_args {
    __aligned_u64 flags;            // 标志位
    __aligned_u64 pidfd;            // pidfd 输出
    __aligned_u64 child_tid;        // 子进程 TID 输出地址
    __aligned_u64 parent_tid;       // 父进程 TID 输出地址
    __aligned_u64 exit_signal;      // 退出信号
    __aligned_u64 stack;            // 栈地址
    __aligned_u64 stack_size;       // 栈大小
    __aligned_u64 tls;              // TLS 描述符
    __aligned_u64 set_tid;          // 指定 PID 数组指针
    __aligned_u64 set_tid_size;     // 指定 PID 数组大小
    __aligned_u64 cgroup;           // cgroup fd
};

// 结构体版本大小
#define CLONE_ARGS_SIZE_VER0 64    // 初始版本
#define CLONE_ARGS_SIZE_VER1 80    // 添加 set_tid/set_tid_size
#define CLONE_ARGS_SIZE_VER2 88    // 添加 cgroup
```

---

## 5. 流程图

```
                     clone3(struct clone_args *args, size_t size)
                                      |
                            +---------v----------+
                            | SYSCALL_DEFINE2     |
                            | (kernel/fork.c)     |
                            +---------+----------+
                                      |
                            +---------v----------+
                            | copy_clone_args_    |
                            | from_user()         |
                            | 从用户空间拷贝并    |
                            | 转换参数结构体      |
                            +---------+----------+
                                      |
                            +---------v----------+
                            | clone3_args_valid() |
                            | 验证参数合法性      |
                            +---------+----------+
                                      |
                            +---------v----------+
                            | kernel_clone(&kargs)|
                            | (共用创建路径)      |
                            +---------+----------+
                                      |
                   +------------------+------------------+
                   |                                     |
            +------v------+                      +------v------+
            | copy_process |                      | wake_up_new_ |
            | (复制进程)   |                      | task(p)      |
            +------+------+                      +------+-------+
                   |                                     |
      +------------+-------------+              +--------v--------+
      |            |             |              | 返回子进程 PID   |
      |    复制子系统    分配 PID     |              | 或 pidfd         |
      |            |             |              +-----------------+
 +----v---+  +----v---+  +----v----+
 |dup_task|  |copy_mm |  |alloc_pid|
 |struct   |  |copy_fs |  |(支持   |
 |(同clone) |  |copy_files |  set_tid)|
 |         |  |copy_sighand|  +---------+
 |         |  |copy_signal|
 |         |  |copy_namesp|
 |         |  |copy_thread|
 |         |  |sched_fork |
 +---------+  +-----------+
```

---

## 6. 错误处理

| 错误码 | 条件 | 触发位置 |
|--------|------|----------|
| `-EFAULT` | 从用户空间拷贝 `clone_args` 失败 | `copy_clone_args_from_user` |
| `-EINVAL` | flags 中包含 CSIGNAL 位 | `clone3_args_valid` |
| `-EINVAL` | exit_signal 不是合法信号值 | `clone3_args_valid` |
| `-EINVAL` | set_tid_size 超过 MAX_PID_NS_LEVEL | `clone3_args_valid` |
| `-EINVAL` | 设置了 CLONE_NEWPID 但未设置 CLONE_THREAD 或 CLONE_PARENT | `copy_process` |
| `-E2BIG` | size 参数大于内核支持的 `clone_args` 大小 | `copy_struct_from_user` |
| `-ENOSYS` | 架构未定义 `__ARCH_BROKEN_SYS_CLONE3` | `SYSCALL_DEFINE2(clone3)` |

---

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#define STACK_SIZE (1024 * 1024)

int child_func(void *arg) {
    printf("子进程: PID=%d\n", getpid());
    return 0;
}

int main() {
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        exit(1);
    }

    struct clone_args args = {
        .flags       = CLONE_VM | SIGCHLD,
        .stack       = (unsigned long)stack + STACK_SIZE,
        .stack_size  = STACK_SIZE,
        .exit_signal = SIGCHLD,
    };

    pid_t pid = clone3(&args, sizeof(args));
    if (pid == -1) {
        perror("clone3");
        free(stack);
        exit(1);
    }

    printf("父进程: 子进程 PID=%d\n", pid);
    wait(NULL);
    free(stack);
    return 0;
}
```

---

## 8. 与 clone 对比

| 特性 | clone | clone3 |
|------|-------|--------|
| **参数传递** | 通过寄存器（最多 5 个） | 通过结构体（可扩展） |
| **版本兼容** | 无 | 结构体大小版本号 |
| **pidfd 支持** | 通过 parent_tid 返回（有限） | 原生支持 |
| **指定 PID** | 不支持 | 支持（set_tid） |
| **cgroup 迁移** | 不支持 | 支持 |
| **栈大小指定** | 不支持 | 支持（stack_size） |
| **内核版本** | 自始存在 | Linux 5.3+ |
| **系统调用号** | 220（ARM64） | 435（ARM64） |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/fork.c:2934` - SYSCALL_DEFINE2(clone3)
- `include/uapi/linux/sched.h` - struct clone_args 定义
- `kernel/fork.c:2798` - copy_clone_args_from_user