# clone 系统调用分析

## 1. 概述

`clone` 是 Linux 中创建新进程/线程的传统系统调用。与 `fork` 不同，`clone` 允许通过 `CLONE_*` 标志精细控制父进程与子进程之间共享的资源（内存空间、文件描述符表、信号处理函数等）。

在 ARM64 上，`fork` 和 `vfork` 均由 `clone` 实现，没有独立的系统调用号。

### 关键特点

- `clone` 通过 `flags` 参数控制资源共享粒度
- 在 ARM64 上，`fork` 实际调用 `clone(SIGCHLD, 0, NULL, NULL)`
- `clone` 和 `clone3` 最终都通过 `kernel_clone` → `copy_process` 路径创建新进程
- 写时复制（COW）技术：`copy_mm` 不立即复制物理内存，而是共享页表并标记为只读

---

## 2. 函数原型

```c
#include <sched.h>

long clone(unsigned long flags, void *stack,
           int *parent_tid, unsigned long tls,
           int *child_tid);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `flags` | `CLONE_*` 标志位组合，控制资源共享 |
| `stack` | 子进程栈指针（创建线程时指定，创建进程时可为 NULL） |
| `parent_tid` | 若设置 `CLONE_PARENT_SETTID`，将子进程 TID 写入此地址 |
| `tls` | 若设置 `CLONE_SETTLS`，设置线程本地存储描述符 |
| `child_tid` | 若设置 `CLONE_CHILD_SETTID`/`CLONE_CHILD_CLEARTID`，操作此地址 |

### 内核入口

```c
// kernel/fork.c:2762
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
                int __user *, parent_tidptr,
                unsigned long, tls,
                int __user *, child_tidptr)
{
    struct kernel_clone_args args = {
        .flags      = (lower_32_bits(clone_flags) & ~CSIGNAL),
        .pidfd      = parent_tidptr,
        .child_tid  = child_tidptr,
        .parent_tid = parent_tidptr,
        .exit_signal = (lower_32_bits(clone_flags) & CSIGNAL),
        .stack      = newsp,
        .tls        = tls,
    };

    return kernel_clone(&args);
}
```

---

## 3. 调用链分析

### 完整调用链

```
clone()  (glibc wrapper)
└─ syscall(__NR_clone, flags, stack, parent_tid, child_tid, tls)
   └─ SYSCALL_DEFINE5(clone)                       // kernel/fork.c:2762
      └─ kernel_clone(&args)                        // kernel/fork.c:2612
         ├─ copy_process(NULL, trace, node, args)   // kernel/fork.c:1964
         │  ├─ dup_task_struct(current, node)         // 复制 task_struct
         │  │  └─ alloc_task_struct_node(node)        // 分配 task_struct
         │  │  └─ alloc_thread_stack_node(tsk, node)  // 分配内核栈
         │  │  └─ arch_dup_task_struct(tsk, orig)     // 架构相关复制
         │  ├─ sched_cgroup_fork(p, args)             // cgroup 初始化
         │  ├─ copy_mm(clone_flags, p)                // 复制地址空间
         │  │  └─ [CLONE_VM] → 共享 mm_struct
         │  │  └─ [!CLONE_VM] → dup_mm(tsk, current->mm)
         │  │     └─ dup_mmap(mm, oldmm)              // 复制内存映射（COW）
         │  ├─ copy_fs(clone_flags, p)                // 复制 fs_struct
         │  │  └─ [CLONE_FS] → 共享 fs_struct
         │  │  └─ [!CLONE_FS] → copy_fs_struct(fs)
         │  ├─ copy_files(clone_flags, p)             // 复制 fd 表
         │  │  └─ [CLONE_FILES] → 共享 files_struct
         │  │  └─ [!CLONE_FILES] → dup_fd()
         │  ├─ copy_sighand(clone_flags, p)           // 复制信号处理函数
         │  │  └─ [CLONE_SIGHAND] → 共享 sighand_struct
         │  │  └─ [!CLONE_SIGHAND] → kmemdup
         │  ├─ copy_signal(clone_flags, p)            // 复制信号结构
         │  │  └─ [CLONE_THREAD] → 共享 signal_struct
         │  │  └─ [!CLONE_THREAD] → kmemdup
         │  ├─ copy_namespaces(clone_flags, p)        // 复制命名空间
         │  │  └─ [CLONE_NEW*] → create_new_namespaces()
         │  │  └─ [!CLONE_NEW*] → get_nsproxy()
         │  ├─ copy_thread(clone_flags, args, p)      // 复制线程上下文
         │  │  └─ arch/arm64/kernel/process.c
         │  │     ├─ memset(&p->thread.cpu_context, 0, ...)
         │  │     ├─ p->thread.cpu_context.x19 = stack
         │  │     ├─ p->thread.cpu_context.x20 = flags
         │  │     ├─ p->thread.cpu_context.lr = ret_from_fork
         │  │     └─ p->thread.cpu_context.sp = (unsigned long)child_stack
         │  ├─ sched_fork(clone_flags, p)             // 调度器初始化
         │  │  ├─ p->state = TASK_NEW
         │  │  └─ p->prio = current->prio
         │  ├─ alloc_pid(p->nsproxy->pid_ns)          // 分配新 PID
         │  │  └─ __alloc_pid(pid_ns)                  // 在 PID 命名空间中分配
         │  ├─ total_forks++                           // 统计计数
         │  └─ sched_post_fork(p)                      // 调度后处理
         │     └─ cgroup_post_fork(p, args)            // cgroup 关联
         ├─ trace_sched_process_fork(current, p)       // tracepoint
         ├─ pid = get_task_pid(p, PIDTYPE_PID)
         ├─ nr = pid_vnr(pid)                          // 获取 PID 的虚拟编号
         ├─ [CLONE_PARENT_SETTID] → put_user(nr, args->parent_tid)
         ├─ [CLONE_VFORK] → init_completion(&vfork)   // vfork 同步
         ├─ wake_up_new_task(p)                        // 唤醒新进程
         │  ├─ activate_task(rq, p, 0)                 // 加入就绪队列
         │  └─ check_preempt_curr(rq, p)               // 检查是否抢占比
         └─ [CLONE_VFORK] → wait_for_vfork_done()     // vfork 等待
```

### copy_process 详细流程

```c
// kernel/fork.c:1964
__latent_entropy struct task_struct *copy_process(
                    struct pid *pid, int trace, int node,
                    struct kernel_clone_args *args)
{
    int retval;
    struct task_struct *p;

    // 1. 复制 task_struct 和内核栈
    p = dup_task_struct(current, node);
    if (!p)
        return ERR_PTR(-ENOMEM);

    // 2. 检查 CLONE_* 标志合法性
    if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
        return ERR_PTR(-EINVAL);
    // ... 更多合法性检查

    // 3. 复制各子系统
    retval = copy_mm(clone_flags, p);
    retval = copy_fs(clone_flags, p);
    retval = copy_files(clone_flags, p);
    retval = copy_sighand(clone_flags, p);
    retval = copy_signal(clone_flags, p);
    retval = copy_namespaces(clone_flags, p);
    copy_thread(clone_flags, args, p);

    // 4. 分配 PID
    if (pid)
        retval = -EINVAL;  // 仅 kernel_thread 使用外部 PID
    else
        pid = alloc_pid(p->nsproxy->pid_ns);

    // 5. 初始化各链表
    INIT_LIST_HEAD(&p->children);
    INIT_LIST_HEAD(&p->sibling);
    // ...

    return p;
}
```

---

## 4. 关键数据结构

```c
// ========== 内核 clone 参数 (include/linux/sched/task.h) ==========

struct kernel_clone_args {
    u64 flags;                      // CLONE_* 标志位
    int __user *pidfd;              // pidfd 返回地址（CLONE_PIDFD）
    int __user *child_tid;          // 子进程 TID 返回地址
    int __user *parent_tid;         // 父进程 TID 返回地址
    const char *name;               // 进程名称（kthread 使用）
    int exit_signal;                // 子进程退出时发送的信号
    u32 kthread:1;                  // 是否为内核线程
    u32 io_thread:1;                // 是否为 IO 线程
    u32 user_worker:1;              // 是否为用户工作线程
    u32 no_files:1;                 // 是否不复制文件描述符
    unsigned long stack;            // 子进程栈指针
    unsigned long stack_size;       // 栈大小
    unsigned long tls;              // TLS 描述符
    pid_t *set_tid;                 // 指定 PID 数组
    size_t set_tid_size;            // set_tid 数组大小
    int cgroup;                     // cgroup 描述符
    int idle;                       // 是否为 idle 进程
    int (*fn)(void *);              // 线程函数（kthread 使用）
    void *fn_arg;                   // 线程函数参数
};

// ========== 进程描述符 (include/linux/sched.h) ==========

struct task_struct {
    unsigned int                    __state;        // 进程状态
    struct thread_info              thread_info;    // 架构特定线程信息
    void                            *stack;         // 内核栈
    struct mm_struct                *mm;            // 内存管理信息
    struct mm_struct                *active_mm;     // 活跃内存映射
    pid_t                           pid;            // 进程 ID
    struct task_struct              *real_parent;   // 实际父进程
    struct task_struct              *parent;        // 接收信号的父进程
    struct list_head                children;       // 子进程列表
    struct list_head                sibling;        // 兄弟进程节点
    struct task_struct              *group_leader;  // 线程组组长
    struct files_struct             *files;         // 打开的文件
    struct fs_struct                *fs;            // 文件系统信息
    struct signal_struct            *signal;        // 信号处理
    struct sighand_struct           *sighand;       // 信号处理函数
    struct nsproxy                  *nsproxy;       // 命名空间
    // ... 更多字段
};

// ========== 线程结构 (arch/arm64/include/asm/processor.h) ==========

struct thread_struct {
    struct cpu_context  cpu_context;    // 保存的 CPU 寄存器
    unsigned long       tpidr_el0;      // TLS 基址寄存器
    struct fpsimd_state fpsimd_state;   // SIMD/浮点寄存器状态
    unsigned long       pc;             // 程序计数器（用于 ptrace）
    // ...
};

struct cpu_context {
    unsigned long x19;      // 被调用者保存寄存器
    unsigned long x20;
    unsigned long x21;
    unsigned long x22;
    unsigned long x23;
    unsigned long x24;
    unsigned long x25;
    unsigned long x26;
    unsigned long x27;
    unsigned long x28;
    unsigned long fp;       // x29 - 帧指针
    unsigned long lr;       // x30 - 链接寄存器
    unsigned long sp;       // 栈指针
};
```

---

## 5. 流程图

```
                    clone(flags, stack, parent_tid, tls, child_tid)
                                      |
                            +---------v----------+
                            | glibc wrapper       |
                            | syscall(__NR_clone) |
                            +---------+----------+
                                      |
                            +---------v----------+
                            | SYSCALL_DEFINE5     |
                            | (kernel/fork.c)     |
                            | 构造 kernel_clone_  |
                            | args 结构体         |
                            +---------+----------+
                                      |
                            +---------v----------+
                            | kernel_clone(&args) |
                            | (kernel/fork.c)     |
                            +---------+----------+
                                      |
                   +------------------+------------------+
                   |                                     |
            +------v------+                      +------v------+
            | copy_process |                      | wake_up_new_ |
            | (核心复制)   |                      | task(p)      |
            +------+------+                      +------+-------+
                   |                                     |
      +------------+-------------+              +--------v--------+
      |            |             |              | activate_task() |
      |    复制子系统    分配 PID     |              | (加入就绪队列)  |
      |            |             |              +--------+--------+
 +----v---+  +----v---+  +----v----+                    |
 |dup_task|  |copy_mm |  |alloc_pid|             +------v-------+
 |struct   |  |copy_fs |  +---------+             | 检查是否需要  |
 |(task_struct|copy_files |                        | 抢占当前进程  |
 |+内核栈)  |copy_sighand|                        +------+-------+
 +---------+  |copy_signal|                               |
              |copy_namesp|                        +------v-------+
              |copy_thread|                        | 返回子进程 PID|
              |sched_fork |                        | (父进程上下文)|
              +-----------+                        +--------------+
```

---

## 6. 错误处理

| 错误码 | 条件 | 触发位置 |
|--------|------|----------|
| `-EINVAL` | CLONE_THREAD 未同时设置 CLONE_SIGHAND | `copy_process` |
| `-EINVAL` | CLONE_SIGHAND 未同时设置 CLONE_VM | `copy_process` |
| `-EINVAL` | CLONE_PIDFD 与 CLONE_PARENT_SETTID 指向同一地址 | `kernel_clone` |
| `-EAGAIN` | 超出 `max_threads` 限制 | `copy_process` |
| `-ENOMEM` | 分配 task_struct 或内核栈失败 | `dup_task_struct` |
| `-EINVAL` | 设置了 CLONE_NEWUSER 但未设置 CLONE_THREAD | `copy_process` |
| `-EINVAL` | 设置了 CLONE_NEWPID 但未设置 CLONE_THREAD 或 CLONE_PARENT | `copy_process` |

---

## 7. 使用示例

```c
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#define STACK_SIZE (1024 * 1024)

int child_func(void *arg) {
    printf("子进程: PID=%d, TID=%d\n", getpid(), gettid());
    printf("子进程: arg=%s\n", (char *)arg);
    return 42;
}

int main() {
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        exit(1);
    }

    // 创建子进程（共享地址空间，即线程）
    pid_t pid = clone(child_func,
                      stack + STACK_SIZE,  // 栈顶（栈向下增长）
                      CLONE_VM | CLONE_VFORK | SIGCHLD,
                      (void *)"hello");

    if (pid == -1) {
        perror("clone");
        exit(1);
    }

    printf("父进程: 子进程 PID=%d\n", pid);
    free(stack);
    return 0;
}
```

---

## 8. 与相关系统调用对比

| 特性 | fork | clone | clone3 |
|------|------|-------|--------|
| **参数方式** | 无参数 | 寄存器参数 | 结构体参数（可扩展） |
| **资源共享控制** | 全部隔离 | 通过 flags 精细控制 | 通过 flags 精细控制 |
| **可扩展性** | 固定 | 受寄存器数量限制 | 通过结构体版本号扩展 |
| **pidfd 支持** | 无 | 有限 | 原生支持 |
| **指定 PID** | 无 | 无 | 支持（set_tid） |
| **cgroup 迁移** | 无 | 无 | 支持 |
| **ARM64 系统调用号** | 无（通过 clone） | 220 | 435 |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/fork.c` - 核心实现
- `include/linux/sched/task.h` - kernel_clone_args 定义
- `include/uapi/linux/sched.h` - CLONE_* 标志定义