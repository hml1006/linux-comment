# Linux 内核 fork+exec 进程创建与执行完整流程分析

> 基于内核源码版本：Linux 6.x (以 ARM64 架构为主)
> 源码根目录：`/home/louis/code/linux/`

---

## 第1章 概述

### 1.1 fork+exec 模型的由来和设计哲学

Unix 操作系统自诞生之初就采用了一种独特而优雅的进程创建模型：**fork+exec 两步走**。这一设计由 Ken Thompson 和 Dennis Ritchie 在贝尔实验室开发 Unix 时确立，并延续至今。

其设计哲学可以概括为：

1. **分离"创建"与"执行"**：`fork()` 负责复制当前进程，`execve()` 负责用新程序替换当前进程。这种分离使得进程创建前后的自定义操作成为可能（如重定向文件描述符、设置资源限制等）。
2. **最小权限原则**：子进程继承父进程的权限集，然后通过 `execve()` 中的 setuid/setgid 机制按需提升权限。
3. **统一性**：所有进程（包括守护进程、shell、用户程序）都通过相同的机制创建。

### 1.2 fork+exec 在 Unix/Linux 中的历史地位

- **1970年代**：Unix V1 首次实现 `fork()` 系统调用
- **1980年代**：SVR4 引入 `vfork()` 优化，解决内存紧张时的 fork 性能问题
- **1990年代**：Linux 0.01 实现基本 fork，Linux 1.0 完善 COW 机制
- **2000年代至今**：Linux 内核持续优化 fork 性能，引入 `clone()` 系统调用族，支持线程创建

### 1.3 进程生命周期概览图

```
    ┌─────────────────────────────────────────────────────┐
    │                    进程生命周期                       │
    └─────────────────────────────────────────────────────┘

    用户态执行
        │
        ▼
    ┌─────────────┐     ┌──────────────┐
    │  fork()     │────▶│ 子进程诞生    │
    │  (创建副本)  │     │ (TASK_RUNNING)│
    └─────────────┘     └──────┬───────┘
        │                      │
        ▼                      ▼
   ┌───────────┐         ┌──────────────┐
   │ 父进程继续  │         │  execve()    │
   │ 执行原程序  │         │  (替换映像)   │
   └───────────┘         └──────┬───────┘
                                │
                          ┌─────▼──────┐
                          │ 新程序开始    │
                          │ 执行新映像    │
                          └─────┬──────┘
                                │
                          ┌─────▼──────┐
                          │  exit()    │
                          │  (进程终止)  │
                          └─────┬──────┘
                                │
                          ┌─────▼──────┐
                          │ 僵尸状态     │
                          │ 等待父进程    │
                          │ wait4()     │
                          └────────────┘
```

---

## 第2章 fork() 系统调用 — 完整流程

### 2.1 系统调用入口

#### 2.1.1 ARM64 架构下的系统调用入口路径

在 ARM64 架构上，用户态程序通过 `SVC` 指令陷入内核。系统调用入口路径如下：

```
用户态: fork() glibc 封装
    │
    ▼
SVC #0  (触发同步异常，ESR_ELx_EC = 0x15, SVC64)
    │
    ▼
el0t_64_sync_handler          [file:///home/louis/code/linux/arch/arm64/kernel/entry-common.c:803]
    │
    ├── ESR_ELx_EC(esr) == ESR_ELx_EC_SVC64
    │
    ▼
el0_svc(regs)                 [file:///home/louis/code/linux/arch/arm64/kernel/entry-common.c:710]
    │
    ├── arm64_enter_from_user_mode(regs)  // 保存用户态上下文
    ├── do_el0_svc(regs)                   // 分发系统调用
    │
    ▼
do_el0_svc(regs)  →  invoke_syscall(regs, sc_nr, scno, sys_call_table)
    │
    ▼
__arm64_sys_clone(regs)       [file:///home/louis/code/linux/arch/arm64/include/asm/syscall_wrapper.h:46]
    │
    ├── 从 regs 中提取参数: clone_flags, newsp, parent_tidptr, tls, child_tidptr
    │
    ▼
SYSCALL_DEFINE5(clone)         [file:///home/louis/code/linux/kernel/fork.c:2762]
    │
    ├── struct kernel_clone_args args = {
    │       .flags      = (lower_32_bits(clone_flags) & ~CSIGNAL),
    │       .pidfd      = parent_tidptr,
    │       .child_tid  = child_tidptr,
    │       .parent_tid = parent_tidptr,
    │       .exit_signal = (lower_32_bits(clone_flags) & CSIGNAL),
    │       .stack      = newsp,
    │       .tls        = tls,
    │   };
    │
    ▼
kernel_clone(&args)            [file:///home/louis/code/linux/kernel/fork.c:2612]
```

> **注意**：ARM64 架构没有独立的 `fork` 系统调用号。glibc 的 `fork()` 封装实际上调用 `clone(SIGCHLD, 0, NULL, NULL)`，即系统调用号 220。如果定义了 `__ARCH_WANT_SYS_FORK`，则也可以使用 `SYSCALL_DEFINE0(fork)`，见 [file:///home/louis/code/linux/kernel/fork.c:2731]。

#### 2.1.2 SYSCALL_DEFINE0(fork) 实现

```c
// file:///home/louis/code/linux/kernel/fork.c:2731
#ifdef __ARCH_WANT_SYS_FORK
SYSCALL_DEFINE0(fork)
{
#ifdef CONFIG_MMU
    struct kernel_clone_args args = {
        .exit_signal = SIGCHLD,     // 子进程退出时向父进程发送 SIGCHLD
    };
    return kernel_clone(&args);
#else
    return -EINVAL;                 // NOMMU 不支持
#endif
}
#endif
```

#### 2.1.3 kernel_clone() — 统一进程创建入口

`kernel_clone()` 是所有进程创建系统调用（fork/vfork/clone/clone3）的统一核心入口。

```c
// file:///home/louis/code/linux/kernel/fork.c:2612
pid_t kernel_clone(struct kernel_clone_args *args)
{
    u64 clone_flags = args->flags;
    struct completion vfork;
    struct pid *pid;
    struct task_struct *p;
    int trace = 0;
    pid_t nr;

    // 1. 参数合法性检查
    if ((clone_flags & CLONE_PIDFD) &&
        (clone_flags & CLONE_PARENT_SETTID) &&
        (args->pidfd == args->parent_tid))
        return -EINVAL;

    // 2. 确定 ptrace 事件类型
    if (!(clone_flags & CLONE_UNTRACED)) {
        if (clone_flags & CLONE_VFORK)
            trace = PTRACE_EVENT_VFORK;
        else if (args->exit_signal != SIGCHLD)
            trace = PTRACE_EVENT_CLONE;
        else
            trace = PTRACE_EVENT_FORK;
        if (likely(!ptrace_event_enabled(current, trace)))
            trace = 0;
    }

    // 3. 核心复制过程
    p = copy_process(NULL, trace, NUMA_NO_NODE, args);
    if (IS_ERR(p))
        return PTR_ERR(p);

    // 4. tracepoint: 进程 fork 事件
    trace_sched_process_fork(current, p);

    // 5. 获取新 PID
    pid = get_task_pid(p, PIDTYPE_PID);
    nr = pid_vnr(pid);

    // 6. 处理 CLONE_PARENT_SETTID
    if (clone_flags & CLONE_PARENT_SETTID)
        put_user(nr, args->parent_tid);

    // 7. 处理 vfork 同步
    if (clone_flags & CLONE_VFORK) {
        p->vfork_done = &vfork;
        init_completion(&vfork);
        get_task_struct(p);
    }

    // 8. LRU 生成相关
    if (IS_ENABLED(CONFIG_LRU_GEN_WALKS_MMU) && !(clone_flags & CLONE_VM)) {
        task_lock(p);
        lru_gen_add_mm(p->mm);
        task_unlock(p);
    }

    // 9. 唤醒新进程
    wake_up_new_task(p);

    // 10. ptrace 事件通知
    if (unlikely(trace))
        ptrace_event_pid(trace, pid);

    // 11. vfork 等待
    if (clone_flags & CLONE_VFORK) {
        if (!wait_for_vfork_done(p, &vfork))
            ptrace_event_pid(PTRACE_EVENT_VFORK_DONE, pid);
    }

    put_pid(pid);
    return nr;  // 返回子进程 PID
}
```

### 2.2 copy_process() — 进程复制核心

`copy_process()` 是 fork 实现中最核心的函数，约 600 行代码。它负责将父进程的几乎所有资源复制给子进程。

```c
// file:///home/louis/code/linux/kernel/fork.c:1964
__latent_entropy struct task_struct *copy_process(
    struct pid *pid, int trace, int node, struct kernel_clone_args *args)
```

**参数说明**：

| 参数 | 含义 |
|------|------|
| `pid` | 预分配的 PID（通常为 NULL，由本函数分配） |
| `trace` | ptrace 事件类型 |
| `node` | NUMA 节点（NUMA_NO_NODE 表示自动选择） |
| `args` | clone 参数（标志位、栈指针、TLS 等） |

**返回值**：成功返回 `task_struct *` 指针，失败返回 `ERR_PTR(-errno)`。

#### 2.2.1 标志位合法性检查

```c
// file:///home/louis/code/linux/kernel/fork.c:1977-2029
// 1. CLONE_NEWNS 和 CLONE_FS 互斥
if ((clone_flags & (CLONE_NEWNS|CLONE_FS)) == (CLONE_NEWNS|CLONE_FS))
    return ERR_PTR(-EINVAL);

// 2. CLONE_NEWUSER 和 CLONE_FS 互斥
if ((clone_flags & (CLONE_NEWUSER|CLONE_FS)) == (CLONE_NEWUSER|CLONE_FS))
    return ERR_PTR(-EINVAL);

// 3. CLONE_THREAD 必须同时包含 CLONE_SIGHAND
if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
    return ERR_PTR(-EINVAL);

// 4. CLONE_SIGHAND 必须同时包含 CLONE_VM
if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
    return ERR_PTR(-EINVAL);

// 5. 全局 init 不能创建兄弟进程
if ((clone_flags & CLONE_PARENT) &&
    current->signal->flags & SIGNAL_UNKILLABLE)
    return ERR_PTR(-EINVAL);

// 6. CLONE_THREAD 不能跨 PID/User 命名空间
if (clone_flags & CLONE_THREAD) {
    if ((clone_flags & (CLONE_NEWUSER | CLONE_NEWPID)) ||
        (task_active_pid_ns(current) != nsp->pid_ns_for_children))
        return ERR_PTR(-EINVAL);
}
```

#### 2.2.2 dup_task_struct() — 复制 task_struct

```c
// file:///home/louis/code/linux/kernel/fork.c:909
static struct task_struct *dup_task_struct(struct task_struct *orig, int node)
{
    struct task_struct *tsk;
    int err;

    // 选择 NUMA 节点
    if (node == NUMA_NO_NODE)
        node = tsk_fork_get_node(orig);

    // 从 slab 分配新 task_struct
    tsk = alloc_task_struct_node(node);
    if (!tsk)
        return NULL;

    // 架构特定复制（ARM64 上为 memcpy）
    err = arch_dup_task_struct(tsk, orig);
    if (err)
        goto free_tsk;

    // 分配内核栈
    err = alloc_thread_stack_node(tsk, node);
    if (err)
        goto free_tsk;

    // 初始化引用计数
    refcount_set(&tsk->rcu_users, 2);  // 用户态可见 + 调度器
    refcount_set(&tsk->usage, 1);      // RCU 引用

    // 栈溢出检测标记
    set_task_stack_end_magic(tsk);

    // 栈 canary 随机化（防栈溢出攻击）
#ifdef CONFIG_STACKPROTECTOR
    tsk->stack_canary = get_random_canary();
#endif

    // 复制 CPU 亲和性掩码
    if (orig->cpus_ptr == &orig->cpus_mask)
        tsk->cpus_ptr = &tsk->cpus_mask;
    dup_user_cpus_ptr(tsk, orig, node);

    // 设置 thread_info
    setup_thread_stack(tsk, orig);

    return tsk;
}
```

**ARM64 的 arch_dup_task_struct()**：

```c
// file:///home/louis/code/linux/arch/arm64/kernel/process.c:348
int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src)
{
    // 保存当前 FPSIMD 状态
    fpsimd_save_and_flush_current_state();
    fpsimd_sync_from_effective_state(src);

    *dst = *src;  // 整体 memcpy 复制

    // 清除 SVE/SME 状态（子进程需要重新初始化）
    dst->thread.fp_type = FP_STATE_FPSIMD;
    dst->thread.sve_state = NULL;
    dst->thread.sme_state = NULL;
    clear_tsk_thread_flag(dst, TIF_SVE);
    clear_tsk_thread_flag(dst, TIF_SME);

    // 清除 MTE 异步故障标记
    clear_tsk_thread_flag(dst, TIF_MTE_ASYNC_FAULT);

    return 0;
}
```

#### 2.2.3 copy_creds() — 复制凭证

```c
// file:///home/louis/code/linux/kernel/fork.c:2084
retval = copy_creds(p, clone_flags);
if (retval < 0)
    goto bad_fork_free;

// RLIMIT_NPROC 检查（防 fork bomb）
retval = -EAGAIN;
if (is_rlimit_overlimit(task_ucounts(p), UCOUNT_RLIMIT_NPROC, rlimit(RLIMIT_NPROC))) {
    if (p->real_cred->user != INIT_USER &&
        !capable(CAP_SYS_RESOURCE) && !capable(CAP_SYS_ADMIN))
        goto bad_fork_cleanup_count;
}
current->flags &= ~PF_NPROC_EXCEEDED;

// 全局线程数上限检查
retval = -EAGAIN;
if (data_race(nr_threads >= max_threads))
    goto bad_fork_cleanup_count;
```

`copy_creds()` 位于 [file:///home/louis/code/linux/kernel/cred.c]，负责：
- 复制 `struct cred`（uid, gid, capabilities 等）
- 检查 `RLIMIT_NPROC` 资源限制
- 检查全局 `nr_threads >= max_threads`（防止 fork bomb）

#### 2.2.4 调度器初始化 — sched_fork()

```c
// file:///home/louis/code/linux/kernel/fork.c:2192
retval = sched_fork(clone_flags, p);
if (retval)
    goto bad_fork_cleanup_policy;
```

`__sched_fork()` 初始化调度相关字段：

```c
// file:///home/louis/code/linux/kernel/sched/core.c
static void __sched_fork(struct task_struct *p)
{
    p->__state = TASK_NEW;              // 防止被意外唤醒
    p->prio = p->normal_prio = p->static_prio = MAX_PRIO - 20;  // 默认优先级
    INIT_LIST_HEAD(&p->se.group_node);  // 调度实体链表初始化
    // ... 更多调度字段初始化
}
```

`sched_fork()` 还会：
- 根据父进程的调度类（DL/RT/CFS/EXT）选择子进程的调度类
- 调用 `init_entity_runnable_average()` 初始化 PELT（Per-Entity Load Tracking）统计
- 设置 `p->__state = TASK_NEW`

#### 2.2.5 copy_files() — 复制文件描述符表

```c
// file:///home/louis/code/linux/kernel/fork.c:2211
retval = copy_files(clone_flags, p, args->no_files);
```

复制策略：
- 如果 `CLONE_FILES`：共享 fd 表（`atomic_inc(&files->count)`）
- 否则：调用 `dup_fd()` 深度复制整个 fd 表（每个文件描述符的 `struct file` 引用计数+1）

#### 2.2.6 copy_fs() — 复制 fs 上下文

```c
// file:///home/louis/code/linux/kernel/fork.c:2214
retval = copy_fs(clone_flags, p);
```

复制策略：
- 如果 `CLONE_FS`：共享 `fs_struct`（引用计数+1）
- 否则：`copy_fs_struct()` 复制当前工作目录（pwd）、根目录（root）、umask 等

#### 2.2.7 copy_sighand() + copy_signal() — 复制信号处理

```c
// file:///home/louis/code/linux/kernel/fork.c:2217
retval = copy_sighand(clone_flags, p);  // 复制信号处理函数表

// file:///home/louis/code/linux/kernel/fork.c:2220
retval = copy_signal(clone_flags, p);   // 复制信号统计信息
```

- `copy_sighand()`：如果 `CLONE_SIGHAND` 则共享，否则复制 `struct sighand_struct`（含 `action[]` 信号处理函数数组）
- `copy_signal()`：始终复制 `struct signal_struct`（含信号统计信息、资源限制等）

#### 2.2.8 copy_mm() — 复制地址空间（核心！）

```c
// file:///home/louis/code/linux/kernel/fork.c:1556
static int copy_mm(u64 clone_flags, struct task_struct *tsk)
{
    struct mm_struct *mm, *oldmm;

    tsk->min_flt = tsk->maj_flt = 0;   // 清空缺页统计
    tsk->nvcsw = tsk->nivcsw = 0;      // 清空上下文切换统计
    tsk->mm = NULL;
    tsk->active_mm = NULL;

    oldmm = current->mm;
    if (!oldmm)
        return 0;                       // 内核线程没有 mm

    if (clone_flags & CLONE_VM) {
        // 共享地址空间（线程）→ 引用计数+1
        mmget(oldmm);
        mm = oldmm;
    } else {
        // 复制地址空间（进程）→ dup_mm()
        mm = dup_mm(tsk, current->mm);
        if (!mm)
            return -ENOMEM;
    }

    tsk->mm = mm;
    tsk->active_mm = mm;
    return 0;
}
```

**dup_mm() 和 dup_mmap() 的详细流程**：

```
dup_mm(tsk, oldmm)                    [file:///home/louis/code/linux/kernel/fork.c:1514]
    │
    ├── mm_alloc() → 分配新 mm_struct
    │
    ├── memcpy(mm, oldmm, sizeof(*mm))  // 复制 mm_struct 基本字段
    │
    ├── dup_mmap(mm, oldmm)             // 复制 VMA 链表
    │   │
    │   ├── mmap_write_lock_killable(oldmm)  // 加锁防止并发修改
    │   │
    │   ├── flush_cache_dup_mm(oldmm)   // 架构特定缓存刷新
    │   │
    │   ├── __mt_dup(&oldmm->mm_mt, &mm->mm_mt, GFP_KERNEL)
    │   │   └── 使用 maple tree 快速复制 VMA 树
    │   │
    │   ├── for_each_vma(vmi, mpnt) {   // 遍历每个 VMA
    │   │   │
    │   │   ├── vm_area_dup(mpnt)       // 复制 VMA 结构
    │   │   │
    │   │   ├── vma_set_file(tmp, mpnt->vm_file)  // 文件引用计数+1
    │   │   │
    │   │   ├── if (file) → 插入 i_mmap 树
    │   │   │
    │   │   └── if (!(tmp->vm_flags & VM_WIPEONFORK))
    │   │       └── copy_page_range(tmp, mpnt)     // 设置 COW → 页表复制
    │   │           ├── 遍历页表，设置所有匿名页为只读
    │   │           ├── 设置父进程页表项为只读（COW 标记）
    │   │           └── 子进程页表指向相同物理页（只读）
    │   │
    │   ├── arch_dup_mmap(oldmm, mm)    // 架构相关操作
    │   │
    │   └── ksm_fork(mm, oldmm)         // KSM 相关
    │       khugepaged_fork(mm, oldmm)  // 透明大页相关
    │
    └── return mm;
```

**COW（Copy-On-Write）数据流图**：

```
    fork() 前:                    fork() 后 (COW):
                                        
    父进程                         父进程                子进程
    ┌──────────┐                ┌──────────┐         ┌──────────┐
    │ 页表     │                │ 页表     │         │ 页表     │
    │ PTE: RW  │                │ PTE: RO  │         │ PTE: RO  │
    └────┬─────┘                └────┬─────┘         └────┬─────┘
         │                           │                    │
         ▼                           ▼                    ▼
    ┌──────────┐                ┌───────────────────────────────┐
    │ 物理页   │                │         物理页 (共享)          │
    │ 0xABC    │                │         0xABC                  │
    └──────────┘                └───────────────────────────────┘
                                        
    写操作时 (COW 触发):
                                        
    父进程/子进程写入 → 缺页异常 → 分配新物理页 → 复制内容 → 更新页表为 RW
```

#### 2.2.9 copy_namespaces() — 复制命名空间

```c
// file:///home/louis/code/linux/kernel/fork.c:2226
retval = copy_namespaces(clone_flags, p);
```

每种命名空间的复制策略：

| 命名空间 | CLONE_NEW* 标志 | 复制策略 |
|---------|----------------|---------|
| PID | CLONE_NEWPID | 创建新 PID 命名空间 |
| Network | CLONE_NEWNET | 创建新网络栈 |
| IPC | CLONE_NEWIPC | 创建新 IPC 资源 |
| UTS | CLONE_NEWUTS | 创建新 hostname/domainname |
| Mount | CLONE_NEWNS | 创建新挂载树 |
| User | CLONE_NEWUSER | 创建新用户映射 |
| Cgroup | CLONE_NEWCGROUP | 创建新 cgroup 层次 |

#### 2.2.10 copy_thread() — 架构特定线程初始化（ARM64）

```c
// file:///home/louis/code/linux/arch/arm64/kernel/process.c:411
int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
    u64 clone_flags = args->flags;
    unsigned long stack_start = args->stack;
    unsigned long tls = args->tls;
    struct pt_regs *childregs = task_pt_regs(p);

    // 清空 CPU 上下文
    memset(&p->thread.cpu_context, 0, sizeof(struct cpu_context));

    // 清空 FPSIMD 状态（防止与旧进程混淆）
    fpsimd_flush_task_state(p);

    if (likely(!args->fn)) {  // 用户态线程 (fork)
        // 复制父进程的寄存器状态
        *childregs = *current_pt_regs();

        // 子进程 fork 返回 0（关键！）
        childregs->regs[0] = 0;

        // 读取当前 TLS 指针
        *task_user_tls(p) = read_sysreg(tpidr_el0);

        // 如果指定了新栈指针
        if (stack_start) {
            if (is_compat_thread(task_thread_info(p)))
                childregs->compat_sp = stack_start;
            else
                childregs->sp = stack_start;
        }

        // 处理 SME 状态
        if (system_supports_sme()) {
            if (!(clone_flags & CLONE_VM)) {
                p->thread.tpidr2_el0 = read_sysreg_s(SYS_TPIDR2_EL0);
                ret = copy_thread_za(p, current);
            } else {
                p->thread.tpidr2_el0 = 0;
            }
        }

        // 设置 TLS（CLONE_SETTLS）
        if (clone_flags & CLONE_SETTLS)
            p->thread.uw.tp_value = tls;

    } else {  // 内核线程 (kthread)
        memset(childregs, 0, sizeof(struct pt_regs));
        childregs->pstate = PSR_MODE_EL1h | PSR_IL_BIT;
        p->thread.cpu_context.x19 = (unsigned long)args->fn;
        p->thread.cpu_context.x20 = (unsigned long)args->fn_arg;
    }

    // 设置返回地址为 ret_from_fork
    p->thread.cpu_context.pc = (unsigned long)ret_from_fork;
    // 设置内核栈指针
    p->thread.cpu_context.sp = (unsigned long)childregs;
    // 设置栈帧（便于 unwind）
    p->thread.cpu_context.fp = (unsigned long)&childregs->stackframe;

    return 0;
}
```

#### 2.2.11 PID 分配

```c
// file:///home/louis/code/linux/kernel/fork.c:2238
if (pid != &init_struct_pid) {
    pid = alloc_pid(p->nsproxy->pid_ns_for_children, args->set_tid,
                    args->set_tid_size);
    if (IS_ERR(pid)) {
        retval = PTR_ERR(pid);
        goto bad_fork_cleanup_thread;
    }
}
```

`alloc_pid()` 位于 [file:///home/louis/code/linux/kernel/pid.c]，负责：
- 在 PID 命名空间层次结构中分配唯一 PID
- 在每个命名空间级别分配可见的虚拟 PID
- 处理 `set_tid` 指定 PID 的情况

#### 2.2.12 设置进程关系

```c
// file:///home/louis/code/linux/kernel/fork.c:2369
write_lock_irq(&tasklist_lock);  // 获取全局进程链表锁

// 设置父子关系
if (clone_flags & (CLONE_PARENT|CLONE_THREAD)) {
    p->real_parent = current->real_parent;
    p->parent_exec_id = current->parent_exec_id;
    if (clone_flags & CLONE_THREAD)
        p->exit_signal = -1;  // 线程不发送退出信号
    else
        p->exit_signal = current->group_leader->exit_signal;
} else {
    p->real_parent = current;  // 父进程为当前进程
    p->parent_exec_id = current->self_exec_id;
    p->exit_signal = args->exit_signal;  // 默认为 SIGCHLD
}
```

#### 2.2.13 插入全局链表

```c
// file:///home/louis/code/linux/kernel/fork.c:2410
// 设置 PID 和 TGID
if (clone_flags & CLONE_THREAD) {
    p->group_leader = current->group_leader;
    p->tgid = current->tgid;
} else {
    p->group_leader = p;
    p->tgid = p->pid;
}

// 插入到 PID 哈希表和全局任务链表
// ... 通过 write_unlock_irq 释放 tasklist_lock 完成
```

#### 2.2.14 最后步骤

```c
// file:///home/louis/code/linux/kernel/fork.c:2480
// 统计
total_forks++;

// 解锁
write_unlock_irq(&tasklist_lock);

// 后处理
proc_fork_connector(p);    // 进程事件连接器通知
sched_post_fork(p);        // 调度器后处理
cgroup_post_fork(p, args); // cgroup 后处理
perf_event_fork(p);        // perf 事件跟踪
```

### 2.3 kernel_clone() 的后续处理

`copy_process()` 返回后，`kernel_clone()` 继续执行：

1. **`trace_sched_process_fork()`**：tracepoint 通知
2. **获取 PID**：`get_task_pid(p, PIDTYPE_PID)` 和 `pid_vnr(pid)`
3. **处理 `CLONE_PARENT_SETTID`**：将子进程 PID 写回用户空间
4. **处理 `CLONE_VFORK`**：初始化 completion，供子进程在 exec/exit 时通知父进程
5. **`wake_up_new_task(p)`**：将新进程加入调度队列
6. **ptrace 事件通知**：`ptrace_event_pid(trace, pid)`
7. **vfork 等待**：父进程调用 `wait_for_vfork_done()` 阻塞直到子进程 exec/exit

### 2.4 wake_up_new_task() 详细分析

```c
// file:///home/louis/code/linux/kernel/sched/core.c
void wake_up_new_task(struct task_struct *p)
{
    struct rq *rq;
    struct rq_flags rf;

    raw_spin_lock_irqsave(&p->pi_lock, rf.flags);
    p->__state = TASK_RUNNING;  // 设置为运行态

    // 选择目标 CPU（fork 负载均衡）
    select_task_rq(p, p->wake_cpu, WF_FORK);

    rq = __task_rq_lock(p, &rf);

    // 将任务加入运行队列
    activate_task(rq, p, ENQUEUE_NOCLOCK);
    p->on_rq = TASK_ON_RQ_QUEUED;

    // 检查是否可抢占当前任务
    if (p->prio < rq->curr->prio)
        resched_curr(rq);

    __task_rq_unlock(rq, &rf);
    raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);

    // 回调通知
    task_woken(p, rq);
}
```

**关键步骤**：

1. **设置 `p->__state = TASK_RUNNING`**：使进程可被调度
2. **`select_task_rq()`**：选择最合适的 CPU，考虑缓存亲和性、负载均衡
3. **`activate_task()`**：将任务加入对应 CPU 的运行队列
4. **`wakeup_preempt()`**：如果新进程优先级更高，触发抢占

### 2.5 ret_from_fork 返回路径

当子进程首次被调度时，从 `ret_from_fork` 开始执行：

```asm
// file:///home/louis/code/linux/arch/arm64/kernel/entry.S:937
SYM_CODE_START(ret_from_fork)
    bl  schedule_tail          // 调度器收尾工作
    cbz x19, 1f               // x19==0 表示用户态线程
    mov x0, x20               // 内核线程：x20=fn_arg
    blr x19                   // 内核线程：x19=fn
1:  get_current_task tsk
    mov x0, sp
    bl  asm_exit_to_user_mode // 返回用户态
    b   ret_to_user           // 恢复用户态寄存器
SYM_CODE_END(ret_from_fork)
```

**执行流程**：

1. `schedule_tail()`：完成调度器切换后的收尾工作
2. 如果是内核线程：调用 `fn(fn_arg)` 执行线程函数
3. 如果是用户态线程：调用 `asm_exit_to_user_mode()` 准备返回用户态
4. `ret_to_user`：恢复用户态寄存器，`ERET` 返回用户空间
5. 子进程返回用户态时，`x0 = 0`（`childregs->regs[0] = 0`），即 fork 返回值 0

---

## 第3章 execve() 系统调用 — 完整流程

### 3.1 系统调用入口

```c
// file:///home/louis/code/linux/fs/exec.c:1924
SYSCALL_DEFINE3(execve,
    const char __user *, filename,        // 可执行文件路径
    const char __user *const __user *, argv,  // 参数数组
    const char __user *const __user *, envp)  // 环境变量数组
{
    CLASS(filename, name)(filename);
    return do_execveat_common(AT_FDCWD, name,
                              native_arg(argv), native_arg(envp), 0);
}
```

**ARM64 系统调用入口**：

```
用户态: execve(pathname, argv, envp)
    │
    ▼
SVC #0  →  el0t_64_sync_handler  →  el0_svc(regs)
    │
    ▼
do_el0_svc(regs)  →  invoke_syscall(regs, sc_nr, scno, sys_call_table)
    │
    ▼
__arm64_sys_execve(regs)        [file:///home/louis/code/linux/arch/arm64/include/asm/syscall_wrapper.h:46]
    │
    ├── 从 regs 提取参数: filename, argv, envp
    │
    ▼
SYSCALL_DEFINE3(execve)          [file:///home/louis/code/linux/fs/exec.c:1924]
    │
    ▼
do_execveat_common(AT_FDCWD, filename, argv, envp, 0)  [file:///home/louis/code/linux/fs/exec.c:1778]
```

### 3.2 准备阶段

#### 3.2.1 do_execveat_common() — 核心入口

```c
// file:///home/louis/code/linux/fs/exec.c:1778
static int do_execveat_common(int fd, struct filename *filename,
                              struct user_arg_ptr argv,
                              struct user_arg_ptr envp,
                              int flags)
{
    int retval;

    // 1. RLIMIT_NPROC 检查
    if ((current->flags & PF_NPROC_EXCEEDED) &&
        is_rlimit_overlimit(current_ucounts(), UCOUNT_RLIMIT_NPROC,
                           rlimit(RLIMIT_NPROC)))
        return -EAGAIN;
    current->flags &= ~PF_NPROC_EXCEEDED;

    // 2. 分配并初始化 linux_binprm
    CLASS(bprm, bprm)(fd, filename, flags);
    if (IS_ERR(bprm))
        return PTR_ERR(bprm);

    // 3. 统计参数和环境变量数量
    retval = count(argv, MAX_ARG_STRINGS);  // 最多 MAX_ARG_STRINGS 个
    if (retval < 0) return retval;
    bprm->argc = retval;

    retval = count(envp, MAX_ARG_STRINGS);
    if (retval < 0) return retval;
    bprm->envc = retval;

    // 4. 检查栈空间限制
    retval = bprm_stack_limits(bprm);
    if (retval < 0) return retval;

    // 5. 复制文件名到栈
    retval = copy_string_kernel(bprm->filename, bprm);
    if (retval < 0) return retval;
    bprm->exec = bprm->p;

    // 6. 复制环境变量和参数到栈
    retval = copy_strings(bprm->envc, envp, bprm);  // 先复制环境变量
    if (retval < 0) return retval;

    retval = copy_strings(bprm->argc, argv, bprm);  // 再复制参数
    if (retval < 0) return retval;

    // 7. 空 argv 处理（添加空串作为 argv[0]）
    if (bprm->argc == 0) {
        retval = copy_string_kernel("", bprm);
        if (retval < 0) return retval;
        bprm->argc = 1;
    }

    // 8. 执行核心
    return bprm_execve(bprm);
}
```

#### 3.2.2 alloc_bprm() — 分配 linux_binprm

```c
// file:///home/louis/code/linux/fs/exec.c:1395
static struct linux_binprm *alloc_bprm(int fd, struct filename *filename, int flags)
{
    struct linux_binprm *bprm;
    struct file *file;
    int retval = -ENOMEM;

    // 1. 打开可执行文件
    file = do_open_execat(fd, filename, flags);
    if (IS_ERR(file))
        return ERR_CAST(file);

    // 2. 分配 bprm 结构（kzalloc）
    bprm = kzalloc_obj(*bprm);
    if (!bprm) {
        do_close_execat(file);
        return ERR_PTR(-ENOMEM);
    }

    bprm->file = file;  // 关联可执行文件

    // 3. 设置文件名
    if (fd == AT_FDCWD || filename->name[0] == '/') {
        bprm->filename = filename->name;
    } else {
        // execveat 相对路径处理
        bprm->fdpath = kasprintf(GFP_KERNEL, "/dev/fd/%d", fd);
        bprm->filename = bprm->fdpath;
    }
    bprm->interp = bprm->filename;  // 解释器名称初始化为文件名

    // 4. 初始化新 mm
    retval = bprm_mm_init(bprm);
    if (!retval)
        return bprm;

    out_free:
        free_bprm(bprm);
        return ERR_PTR(retval);
}
```

**do_open_execat() 的执行流程**：

```c
// file:///home/louis/code/linux/fs/exec.c:756
static struct file *do_open_execat(int fd, struct filename *name, int flags)
{
    struct open_flags open_exec_flags = {
        .open_flag = O_LARGEFILE | O_RDONLY | __FMODE_EXEC,  // 只读+执行标记
        .acc_mode = MAY_EXEC,                                  // 执行权限检查
        .intent = LOOKUP_OPEN,
        .lookup_flags = LOOKUP_FOLLOW,
    };

    // 1. 解析路径并打开文件
    file = do_file_open(fd, name, &open_exec_flags);
    if (IS_ERR(file))
        return file;

    // 2. 检查文件系统是否禁止执行 (noexec 挂载)
    if (path_noexec(&file->f_path))
        return ERR_PTR(-EACCES);

    // 3. 必须是普通文件
    if (WARN_ON_ONCE(!S_ISREG(file_inode(file)->i_mode)))
        return ERR_PTR(-EACCES);

    // 4. 禁止写入（执行期间文件不可写）
    err = exe_file_deny_write_access(file);
    if (err)
        return ERR_PTR(err);

    return file;
}
```

**bprm_mm_init() — 创建新地址空间**：

```c
// file:///home/louis/code/linux/fs/exec.c:238
static int bprm_mm_init(struct linux_binprm *bprm)
{
    // 1. 分配新 mm_struct
    bprm->mm = mm = mm_alloc();
    if (!mm)
        return -ENOMEM;

    // 2. 保存当前 RLIMIT_STACK
    task_lock(current->group_leader);
    bprm->rlim_stack = current->signal->rlim[RLIMIT_STACK];
    task_unlock(current->group_leader);

    // 3. 创建初始栈 VMA
    err = create_init_stack_vma(bprm->mm, &bprm->vma, &bprm->p);
    if (err)
        goto err;

    return 0;
}
```

#### 3.2.3 复制参数和环境变量

```c
// file:///home/louis/code/linux/fs/exec.c:442
static int copy_strings(int argc, struct user_arg_ptr argv, struct linux_binprm *bprm)
```

`copy_strings()` 将用户空间的 argv/envp 字符串复制到新进程的栈空间：

```
    用户空间                          内核栈                         新进程栈
    ┌──────────┐                ┌──────────────────┐          ┌──────────────────┐
    │ argv[0]  │──── ptr ──────▶│  kernel stack    │          │ bprm->p          │
    │ "ls"     │                │                  │          │ 指向栈顶         │
    ├──────────┤                │    copy_strings  │          ├──────────────────┤
    │ argv[1]  │                │    (逐个复制)     │          │  "ls\0"          │
    │ "-l"     │                │                  │          │  "-l\0"          │
    ├──────────┤                │                  │          │  "/bin/ls\0"     │
    │ argv[2]  │                │                  │          │  envp[0]...      │
    │ NULL     │                │                  │          │  envp[1]...      │
    └──────────┘                └──────────────────┘          │  NULL            │
                                                              │  argv[2] ptr     │
                                                              │  argv[1] ptr     │
                                                              │  argv[0] ptr     │
                                                              │  argc            │
                                                              └──────────────────┘
                                                                  (栈底)
```

### 3.3 bprm_execve() — 执行核心

```c
// file:///home/louis/code/linux/fs/exec.c:1724
static int bprm_execve(struct linux_binprm *bprm)
{
    int retval;

    // 1. 准备凭证
    retval = prepare_bprm_creds(bprm);
    if (retval) return retval;

    // 2. 检查不安全的执行状态
    check_unsafe_exec(bprm);
    current->in_execve = 1;
    sched_mm_cid_before_execve(current);

    // 3. 调度器通知（准备执行）
    sched_exec();

    // 4. LSM 安全钩子（设置凭证基础）
    retval = security_bprm_creds_for_exec(bprm);
    if (retval || bprm->is_check) goto out;

    // 5. 执行二进制格式处理
    retval = exec_binprm(bprm);
    if (retval < 0) goto out;

    // 6. 执行成功后的清理
    sched_mm_cid_after_execve(current);
    rseq_execve(current);
    current->in_execve = 0;
    user_events_execve(current);
    acct_update_integrals(current);
    task_numa_free(current, false);
    return retval;

out:
    // 如果已过"不可返回点"，发送 SIGSEGV
    if (bprm->point_of_no_return && !fatal_signal_pending(current))
        force_fatal_sig(SIGSEGV);
    sched_mm_cid_after_execve(current);
    rseq_force_update();
    current->in_execve = 0;
    return retval;
}
```

#### 3.3.1 安全检查

```c
// file:///home/louis/code/linux/fs/exec.c:1486
static void check_unsafe_exec(struct linux_binprm *bprm)
{
    struct task_struct *p = current, *t;
    unsigned n_fs;

    if (p->ptrace)
        bprm->unsafe |= LSM_UNSAFE_PTRACE;  // 被 ptrace 跟踪

    if (task_no_new_privs(current))
        bprm->unsafe |= LSM_UNSAFE_NO_NEW_PRIVS;  // 禁止提权

    // 检查是否有其他线程共享 fs_struct
    n_fs = 1;
    for_other_threads(p, t) {
        if (t->fs == p->fs)
            n_fs++;
    }
    if (p->fs->users > n_fs)
        bprm->unsafe |= LSM_UNSAFE_SHARE;  // 不安全共享
    else
        p->fs->in_exec = 1;  // 标记 exec 进行中
}
```

#### 3.3.2 exec_binprm() — 二进制格式处理

```c
// file:///home/louis/code/linux/fs/exec.c:1679
static int exec_binprm(struct linux_binprm *bprm)
{
    pid_t old_pid, old_vpid;
    int ret, depth;

    old_pid = current->pid;
    old_vpid = task_pid_nr_ns(current, task_active_pid_ns(current->parent));

    // 支持最多 5 层解释器嵌套
    for (depth = 0;; depth++) {
        if (depth > 5)
            return -ELOOP;

        // 搜索匹配的二进制格式处理程序
        ret = search_binary_handler(bprm);
        if (ret < 0)
            return ret;
        if (!bprm->interpreter)
            break;  // 没有解释器，直接执行

        // 有解释器（如 #! 脚本），替换为解释器
        exec = bprm->file;
        bprm->file = bprm->interpreter;
        bprm->interpreter = NULL;
        exe_file_allow_write_access(exec);
        fput(exec);
        // 继续循环，用解释器重新搜索
    }

    // 执行成功后的通知
    audit_bprm(bprm);
    trace_sched_process_exec(current, old_pid, bprm);
    ptrace_event(PTRACE_EVENT_EXEC, old_vpid);
    proc_exec_connector(current);
    return 0;
}
```

**search_binary_handler()**：

```c
// file:///home/louis/code/linux/fs/exec.c:1645
static int search_binary_handler(struct linux_binprm *bprm)
{
    struct linux_binfmt *fmt;
    int retval;

    // 读取文件前 256 字节（BINPRM_BUF_SIZE）到 bprm->buf
    retval = prepare_binprm(bprm);

    // LSM 安全检查
    retval = security_bprm_check(bprm);

    // 遍历注册的二进制格式处理程序
    read_lock(&binfmt_lock);
    list_for_each_entry(fmt, &formats, lh) {
        if (!try_module_get(fmt->module))
            continue;
        read_unlock(&binfmt_lock);

        // 调用处理程序的 load_binary 方法
        retval = fmt->load_binary(bprm);

        read_lock(&binfmt_lock);
        put_binfmt(fmt);
        if (bprm->point_of_no_return || (retval != -ENOEXEC)) {
            read_unlock(&binfmt_lock);
            return retval;  // 成功或其他错误
        }
        // retval == -ENOEXEC 继续尝试下一个
    }
    read_unlock(&binfmt_lock);

    return -ENOEXEC;  // 没有匹配的格式
}
```

### 3.4 begin_new_exec() — 不可返回点

```c
// file:///home/louis/code/linux/fs/exec.c:1091
int begin_new_exec(struct linux_binprm *bprm)
```

这是 exec 的"点 no return"——一旦进入此函数，任何错误都将导致进程被终止，无法返回用户空间。

#### 3.4.1 de_thread() — 清除线程组中其他线程

```c
// file:///home/louis/code/linux/fs/exec.c:1115
retval = de_thread(me);
```

**de_thread() 流程**：

```
de_thread(me)
    │
    ├── 单线程？→ 直接返回
    │
    ├── 多线程：
    │   ├── zap_other_threads(me)  // 向其他线程发送 SIGKILL
    │   ├── 等待所有其他线程退出
    │   │
    │   └── 如果不是线程组 leader：
    │       ├── 交换 PID/TGID（子线程变成 leader）
    │       └── 释放旧的线程组 leader
    │
    └── 返回
```

#### 3.4.2 unshare_files() — 取消共享文件描述符

```c
// file:///home/louis/code/linux/fs/exec.c:1126
retval = unshare_files();
```

确保当前进程的文件描述符表不被其他线程共享，否则后续的 `close_on_exec` 操作会互相影响。

#### 3.4.3 exec_mmap() — 切换地址空间

```c
// file:///home/louis/code/linux/fs/exec.c:1148
retval = exec_mmap(bprm->mm);
bprm->mm = NULL;  // 所有权已转移给进程
```

**exec_mmap() 流程**：

```
exec_mmap(mm)                        [file:///home/louis/code/linux/fs/exec.c]
    │
    ├── mm = bprm->mm  (新地址空间)
    ├── old_mm = current->mm  (旧地址空间)
    │
    ├── activate_mm(mm, old_mm)  // 切换页表基址
    │   └── ARM64: cpu_replace_ttbr0(swapper_pg_dir)  →  TLB 刷新
    │
    ├── current->mm = mm;       // 设置新 mm
    │   current->active_mm = mm;
    │
    ├── task_unlock(current);
    │
    └── mmput(old_mm)           // 释放旧 mm（引用计数-1）
```

#### 3.4.4 flush_thread() — 刷新线程状态

```c
// file:///home/louis/code/linux/fs/exec.c:1175
flush_thread();
```

**ARM64 的 flush_thread()**：

```c
// file:///home/louis/code/linux/arch/arm64/kernel/process.c
void flush_thread(void)
{
    // 清空 FPSIMD/SVE 状态
    fpsimd_flush_thread();
    // 清空 TLS
    tls_thread_flush();
    // 清空硬件断点
    flush_ptrace_hw_breakpoint(current);
    // 清空 MTE（内存标记扩展）设置
    mte_thread_flush();
    // 清空 POE（权限覆盖扩展）
    if (system_supports_poe())
        current->thread.por_el0 = POR_EL0_INIT;
    // 清空 GCS（保护调用栈）
    gcs_thread_flush();
}
```

#### 3.4.5 凭证切换

```c
// file:///home/louis/code/linux/fs/exec.c:1097
retval = bprm_creds_from_file(bprm);  // 处理 setuid/setgid

// file:///home/louis/code/linux/fs/exec.c:1253
security_bprm_committing_creds(bprm);  // LSM 通知（凭证即将提交）
commit_creds(bprm->cred);               // 提交新凭证
bprm->cred = NULL;
security_bprm_committed_creds(bprm);    // LSM 通知（凭证已提交）
```

**bprm_fill_uid() — setuid/setgid 处理**：

```c
// file:///home/louis/code/linux/fs/exec.c:1528
static void bprm_fill_uid(struct linux_binprm *bprm, struct file *file)
{
    // 检查文件是否有 setuid/setgid 位
    if (mode & S_ISUID) {
        bprm->per_clear |= PER_CLEAR_ON_SETID;
        bprm->cred->euid = vfsuid_into_kuid(vfsuid);  // 提升 euid
    }
    if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
        bprm->per_clear |= PER_CLEAR_ON_SETID;
        bprm->cred->egid = vfsgid_into_kgid(vfsgid);  // 提升 egid
    }
}
```

#### 3.4.6 其他设置

```c
// file:///home/louis/code/linux/fs/exec.c:1173
// 清除继承的标志位
me->flags &= ~(PF_RANDOMIZE | PF_FORKNOEXEC | PF_NOFREEZE | PF_NO_SETAFFINITY);

// 更新进程名
__set_task_comm(me, kbasename(bprm->filename), true);

// 递增 self_exec_id（用于 exec 检测）
WRITE_ONCE(me->self_exec_id, me->self_exec_id + 1);

// 重置信号处理函数（默认 SIG_DFL）
flush_signal_handlers(me, 0);

// 关闭设置了 FD_CLOEXEC 的文件描述符
do_close_on_exec(me->files);
```

### 3.5 load_elf_binary() — ELF 加载（核心）

```c
// file:///home/louis/code/linux/fs/binfmt_elf.c:833
static int load_elf_binary(struct linux_binprm *bprm)
```

这是 ELF 格式可执行文件加载的核心函数，约 500 行。

#### 3.5.1 ELF 头检查

```c
// file:///home/louis/code/linux/fs/binfmt_elf.c:833
// bprm->buf 中已经包含文件前 256 字节
struct elfhdr *elf_ex = (struct elfhdr *)bprm->buf;

// 1. ELF magic 检查: \x7fELF
if (memcmp(elf_ex->e_ident, ELFMAG, SELFMAG) != 0)
    goto out;  // → -ENOEXEC

// 2. 必须是 ET_EXEC（可执行文件）或 ET_DYN（共享对象/PIE）
if (elf_ex->e_type != ET_EXEC && elf_ex->e_type != ET_DYN)
    goto out;

// 3. 架构检查
if (!elf_check_arch(elf_ex))
    goto out;
```

#### 3.5.2 程序头解析

```c
// 加载程序头表
elf_phdata = load_elf_phdrs(elf_ex, bprm->file);

// 遍历程序头
for (i = 0; i < elf_ex->e_phnum; i++, elf_ppnt++) {
    switch (elf_ppnt->p_type) {
    case PT_INTERP:
        // 找到解释器（动态链接器）路径
        elf_interpreter = kzalloc(elf_ppnt->p_filesz);
        elf_read(bprm->file, elf_interpreter, elf_ppnt->p_filesz, elf_ppnt->p_offset);
        interpreter = open_exec(elf_interpreter);  // 打开动态链接器
        break;

    case PT_LOAD:
        // 可加载段（需要映射到内存）
        break;

    case PT_GNU_STACK:
        // 栈执行权限
        executable_stack = EXSTACK_DISABLE_X;  // 默认不可执行
        break;

    case PT_GNU_RELRO:
        // 只读重定位
        break;
    }
}
```

#### 3.5.3 解释器加载

如果可执行文件有 `PT_INTERP` 段（动态链接的可执行文件），需要加载解释器：

```c
// 加载动态链接器（如 /lib/ld-linux-aarch64.so.1）
if (elf_interpreter) {
    interp_elf_ex = kmalloc(sizeof(*interp_elf_ex), GFP_KERNEL);
    // 读取解释器的 ELF 头
    retval = elf_read(interpreter, interp_elf_ex, sizeof(*interp_elf_ex), 0);

    // 加载解释器的程序头
    interp_elf_phdata = load_elf_phdrs(interp_elf_ex, interpreter);

    // 映射解释器到进程地址空间
    interp_load_addr = load_elf_interp(interp_elf_ex, interpreter,
                                       &interp_map_addr, interp_elf_phdata, ...);
}
```

#### 3.5.4 ELF 段映射

```c
// 遍历所有 PT_LOAD 段，映射到进程地址空间
for (i = 0, elf_ppnt = elf_phdata; i < elf_ex->e_phnum; i++, elf_ppnt++) {
    if (elf_ppnt->p_type != PT_LOAD)
        continue;

    // 计算映射地址（考虑 ASLR）
    if (elf_ex->e_type == ET_DYN) {
        // PIE 可执行文件：随机选择基址
        load_bias = elf_load(bprm->file, 0, elf_ppnt, ...);
        // 调整对齐
        load_bias = ELF_PAGESTART(load_bias - vaddr);
    }

    // 执行映射
    error = elf_load(bprm->file, load_bias + vaddr, elf_ppnt,
                     elf_prot, elf_flags, total_size);
    // elf_load 内部调用 do_mmap() 完成实际映射
}
```

**ELF 段映射示意图**：

```
    进程地址空间布局 (exec 后)
    ┌──────────────────────────────┐
    │ 0x0000_0000_0000_0000        │
    ├──────────────────────────────┤
    │ 代码段 (PT_LOAD, RX)         │←─ elf_load(文件偏移, 内存地址)
    ├──────────────────────────────┤
    │ 数据段 (PT_LOAD, RW)         │←─ elf_load(文件偏移, 内存地址)
    ├──────────────────────────────┤
    │ BSS (全零填充)               │
    ├──────────────────────────────┤
    │ 堆 (heap)                    │←─ brk
    ├──────────────────────────────┤
    │ 动态链接器映射               │←─ load_elf_interp()
    ├──────────────────────────────┤
    │ 共享库映射                   │
    ├──────────────────────────────┤
    │ vDSO                         │
    ├──────────────────────────────┤
    │ 栈 (stack)                   │←─ setup_arg_pages()
    │ argv, envp, auxv             │
    └──────────────────────────────┘
    │ 0xFFFF_0000_... (内核空间)   │
    └──────────────────────────────┘
```

#### 3.5.5 栈和辅助向量设置

```c
// 1. 设置参数栈（调整栈 VMA 大小和权限）
retval = setup_arg_pages(bprm, randomize_stack_top(STACK_TOP), executable_stack);

// 2. 构建 ELF 辅助向量 (auxv)
retval = create_elf_tables(bprm, elf_ex, interp_load_addr, e_entry, phdr_addr);
```

**ELF 辅助向量 (auxv) 内容**：

```
AT_PHDR      → 程序头表地址
AT_PHENT     → 程序头表项大小 (sizeof(Elf64_Phdr))
AT_PHNUM     → 程序头表项数量
AT_PAGESZ    → 页大小 (4096 或 65536)
AT_BASE      → 解释器基址（动态链接器加载地址）
AT_ENTRY     → 程序入口点
AT_RANDOM    → 16 字节随机数（用于栈 canary 等）
AT_SECURE    → setuid 标记（0 或 1）
AT_HWCAP     → 硬件能力标志
AT_HWCAP2    → 硬件能力标志 2
AT_PLATFORM  → 平台字符串 ("aarch64")
AT_EXECFN    → 可执行文件路径
AT_UID, AT_EUID, AT_GID, AT_EGID
```

#### 3.5.6 启动新线程

```c
// 设置新进程的入口点和栈指针
// ARM64: start_thread(regs, elf_entry, bprm->p)
// 等价于:
regs->pc = elf_entry;     // 新程序入口（或解释器入口）
regs->sp = bprm->p;       // 栈顶指针
regs->regs[0] = bprm->p;  // x0 = 栈指针（指向辅助向量等）
```

**exec 完成后返回用户态**：

```
load_elf_binary() 返回
    │
    ▼
search_binary_handler() 返回
    │
    ▼
exec_binprm() → trace_sched_process_exec() → ptrace_event(PTRACE_EVENT_EXEC)
    │
    ▼
bprm_execve() 返回
    │
    ▼
do_execveat_common() 返回
    │
    ▼
SYSCALL_DEFINE3(execve) 返回
    │
    ▼
__arm64_sys_execve 返回  →  syscall_return 路径
    │
    ▼
ret_to_user  →  kernel_exit 0  →  ERET
    │
    ▼
新程序开始执行:
    PC = _start (或动态链接器入口)
    SP = 指向栈顶 (argv, envp, auxv)
    x0 = 栈指针
```

---

#### 3.5.7 动态链接执行过程（用户态）

##### 3.5.7.1 动态链接器入口

在 exec 完成后，内核通过 `ERET` 返回用户态，新进程的入口点 `PC` 被设置为**动态链接器**（即 `PT_INTERP` 指定的解释器，如 `/lib/ld-linux-aarch64.so.1`）的入口地址，而非应用程序本身的 `_start`。此时：

```
用户态入口点:
    PC  = 动态链接器的入口地址 (ld-linux.so 的 _start)
    SP  = 指向栈顶 (argv, envp, auxv, 参数串)
    x0  = SP (栈指针，传统上用于传递参数)

栈布局 (exec 后初始状态):
    高地址
    ┌──────────────────────────────┐
    │ 参数字符串 (argv strings)    │
    ├──────────────────────────────┤
    │ 环境变量字符串 (envp strings)│
    ├──────────────────────────────┤
    │ 辅助向量 (auxv)             │
    │ AT_NULL                      │
    │ ...                          │
    │ AT_ENTRY, AT_BASE, ...       │
    ├──────────────────────────────┤
    │ envp[n] = NULL               │
    │ ...                          │
    │ envp[0]                      │
    ├──────────────────────────────┤
    │ argv[argc] = NULL            │
    │ ...                          │
    │ argv[0]                      │
    ├──────────────────────────────┤
    │ argc                         │
    └──────────────────────────────┘
    低地址 (SP →)
```

##### 3.5.7.2 动态链接器的核心任务

动态链接器（glibc 中的 `ld-linux-*.so`）负责以下核心任务：

1. **自举（Bootstrap）**：解析自身的重定位条目，使自己能够正常运行
2. **加载共享库**：解析 `DT_NEEDED`，递归加载所有依赖的共享库
3. **重定位（Relocation）**：修正 GOT/PLT 中的符号地址
4. **初始化**：调用所有共享库的 `.init` 段和 `__attribute__((constructor))` 函数
5. **移交控制权**：跳转到应用程序的 `_start` 入口点

##### 3.5.7.3 自举过程

动态链接器本身也是一个 ELF 共享对象（`ET_DYN`），在加载时其自身的重定位尚未完成。因此，动态链接器需要先**自举**：

```
ld-linux _start 入口:
    │
    ├── 1. 读取 auxv 获取 AT_BASE (自身加载基址)
    │
    ├── 2. 找到自身的 .dynamic 段 (通过 DT_PHDR + AT_PHENT)
    │
    ├── 3. 解析 DT_RELA/DT_REL 条目，完成自举重定位
    │      └── 修正 GOT 中的全局符号和函数地址
    │
    ├── 4. 初始化 TLS (线程本地存储) 段
    │
    └── 5. 准备调用 _dl_relocate_object() 加载共享库
```

**自举的关键**：动态链接器必须使用与位置无关的代码（PIC）编写，且在自举完成前不能调用任何外部函数（包括 `memcpy`、`strlen` 等 libc 函数）。

##### 3.5.7.4 共享库加载与依赖解析

动态链接器解析可执行文件的 `.dynamic` 段，处理 `DT_NEEDED` 条目：

```
.dynamic 段 (位于可执行文件内存中):
    ┌──────────────────────────────┐
    │ DT_NEEDED: "libc.so.6"      │───→ 加载 libc.so.6
    ├──────────────────────────────┤
    │ DT_NEEDED: "libpthread.so.0" │───→ 加载 libpthread
    ├──────────────────────────────┤
    │ DT_NEEDED: "libm.so.6"       │───→ 加载 libm
    ├──────────────────────────────┤
    │ DT_SYMTAB: 符号表地址         │
    │ DT_STRTAB: 字符串表地址        │
    │ DT_GNU_HASH: GNU hash 表      │
    │ DT_PLTGOT: GOT 基址           │
    │ DT_INIT: .init 函数地址       │
    │ DT_FINI: .fini 函数地址       │
    │ DT_DEBUG: 调试信息            │
    └──────────────────────────────┘
```

**加载流程：**

```
_dl_relocate_object() 或等效函数:
    │
    ├── 1. 读取 DT_NEEDED 条目
    │
    ├── 2. 对每个 DT_NEEDED:
    │      ├── 搜索共享库 (LD_LIBRARY_PATH, /etc/ld.so.cache, /lib/, /usr/lib/)
    │      ├── mmap() 映射共享库到进程地址空间
    │      ├── 解析该共享库的 DT_NEEDED (递归)
    │      └── 记录符号表到全局符号表
    │
    ├── 3. 广度优先遍历所有依赖，避免重复加载
    │
    └── 4. 构建全局符号表，用于后续符号解析
```

**共享库搜索顺序：**

1. `LD_LIBRARY_PATH` 环境变量（如已设置且非 setuid 程序）
2. `/etc/ld.so.cache`（由 `ldconfig` 预生成）
3. 默认路径: `/lib/`, `/usr/lib/`, `/usr/local/lib/`

##### 3.5.7.5 重定位（Relocation）与 GOT/PLT

**GOT（Global Offset Table，全局偏移表）**：

GOT 是一个位于数据段中的表，存储全局变量和函数的实际地址。共享库中的代码通过 GOT 间接访问全局数据。

```
GOT 布局:
    ┌──────────────────────────────┐
    │ GOT[0] = .dynamic 段地址     │
    │ GOT[1] = link_map 结构体地址 │
    │ GOT[2] = _dl_runtime_resolve │
    ├──────────────────────────────┤
    │ GOT[3] = &printf             │  (已解析)
    │ GOT[4] = &malloc             │  (未解析 → PLT 桩)
    │ GOT[5] = &errno              │
    └──────────────────────────────┘
```

**PLT（Procedure Linkage Table，过程链接表）**：

PLT 是代码段中的桩代码，实现延迟绑定（Lazy Binding）：

```
PLT 桩 (调用 printf 时):
    ┌──────────────────────────────┐
    │ printf@plt:                   │
    │   b _GLOBAL_OFFSET_TABLE_+8  │  → 跳转到 GOT[3]
    │   (首次: GOT[3] 指向 PLT 下一行) │
    │   push n (reloc 索引)        │  → 准备重定位参数
    │   b _dl_runtime_resolve      │  → 调用动态链接器解析
    └──────────────────────────────┘
```

**延迟绑定（Lazy Binding）流程：**

```
首次调用 printf():
    │
    ├── call printf@plt
    │
    ├── PLT 跳转到 GOT[3]
    │      └── GOT[3] 初始指向 PLT 的下一条指令 (push n)
    │
    ├── push reloc_index
    ├── b _dl_runtime_resolve
    │      │
    │      ├── 1. 查找符号表: "printf"
    │      ├── 2. 在已加载的共享库中搜索
    │      │      ├── 搜索 libc.so.6 的符号表
    │      │      └── 找到 printf 的地址
    │      ├── 3. 更新 GOT[3] = printf 的实际地址
    │      └── 4. 跳转到 printf 执行
    │
    └── 后续调用 printf():
        call printf@plt
          └── GOT[3] 已指向 printf → 直接跳转
```

**立即绑定（Eager Binding，`LD_BIND_NOW=1`）**：

当设置 `LD_BIND_NOW` 环境变量时，动态链接器在启动时一次性解析所有符号，而非延迟到首次调用：

```
LD_BIND_NOW=1:
    │
    └── _dl_relocate_object():
        ├── 遍历所有 DT_RELA/DT_REL 条目
        ├── 对每个重定位条目:
        │   ├── R_AARCH64_GLOB_DAT → 查找符号, 写入 GOT
        │   ├── R_AARCH64_JUMP_SLOT → 查找符号, 写入 GOT
        │   └── R_AARCH64_RELATIVE → 基址修正
        └── 所有 GOT 条目在启动时全部解析完成
```

**ARM64 常见重定位类型：**

| 重定位类型 | 说明 | 应用于 |
|-----------|------|--------|
| `R_AARCH64_RELATIVE` | 绝对地址修正 (`base + addend`) | 数据段中的指针 |
| `R_AARCH64_GLOB_DAT` | 全局符号地址修正 | 全局变量 GOT 条目 |
| `R_AARCH64_JUMP_SLOT` | 函数跳转槽修正 | 函数 GOT 条目 (PLT) |
| `R_AARCH64_ABS64` | 64 位绝对地址修正 | 静态重定位 |
| `R_AARCH64_COPY` | 数据段复制修正 | 已定义数据符号 |

##### 3.5.7.6 init/fini 函数执行

所有共享库加载和重定位完成后，动态链接器执行初始化回调：

```
初始化顺序:
    │
    ├── 1. 按依赖顺序（拓扑排序）调用每个共享库的 DT_INIT
    │      ├── DT_INIT: 指向 .init 段的地址
    │      └── 执行 _init() 函数
    │
    ├── 2. 调用共享库中 __attribute__((constructor)) 函数
    │      └── 通过 .init_array 段中的函数指针数组
    │
    ├── 3. 调用可执行文件自身的 .init 和 .init_array
    │
    └── 4. 注册 .fini / .fini_array 函数（通过 atexit）
```

##### 3.5.7.7 移交控制权到应用程序

初始化完成后，动态链接器跳转到应用程序的入口点：

```
动态链接器最后一步:
    │
    ├── 从 auxv 中读取 AT_ENTRY (应用程序入口点)
    │
    ├── 传递参数给应用程序:
    │      x0 = argc
    │      x1 = argv
    │      x2 = envp
    │      (ARM64 调用约定: x0-x2 传递前三个参数)
    │
    └── 跳转到应用程序的 _start 入口:
            _start:
                mov x0, sp          // x0 = 栈指针
                bl _start_c         // 调用 libc _start_c
                    │
                    └── _start_c() → __libc_start_main()
                        │
                        ├── 设置 stack canary (从 auxv AT_RANDOM 读取)
                        ├── 注册 atexit() 处理函数
                        │   ├── 注册 _dl_fini() (共享库清理)
                        │   └── 注册 __libc_csu_fini() (libc 清理)
                        │
                        ├── 调用 __libc_csu_init()
                        │   └── 执行可执行文件的 .init_array
                        │
                        └── main(argc, argv, envp)  ← 应用程序正式开始
```

**最终进程地址空间布局（动态链接后）：**

```
    进程地址空间 (完整)
    ┌──────────────────────────────┐
    │ 0x0000_0000_0000_0000        │
    ├──────────────────────────────┤
    │ 代码段 (RX) - 可执行文件     │
    │ 数据段 (RW) - 可执行文件     │
    │ BSS (RW) - 可执行文件        │
    ├──────────────────────────────┤
    │ 堆 (heap)                    │←─ brk/sbrk
    ├──────────────────────────────┤
    │ libc.so.6 代码段 (RX)        │
    │ libc.so.6 数据段 (RW)        │
    ├──────────────────────────────┤
    │ libm.so.6 代码段 (RX)        │
    │ libm.so.6 数据段 (RW)        │
    ├──────────────────────────────┤
    │ ld-linux 代码段 (RX)         │←─ AT_BASE
    │ ld-linux 数据段 (RW)         │
    ├──────────────────────────────┤
    │ vDSO (内核映射)              │
    ├──────────────────────────────┤
    │ 栈 (stack)                   │←─ SP
    │ argv, envp, auxv             │
    └──────────────────────────────┘
    │ 0xFFFF_0000_... (内核空间)   │
    └──────────────────────────────┘
```

---

## 第4章 fork+exec 的协同工作

### 4.1 经典 fork+exec 模式

```c
// Shell 典型场景
pid_t pid = fork();
if (pid == 0) {
    // 子进程
    close(fd);             // 关闭不需要的文件描述符
    dup2(new_fd, 0);       // 重定向标准输入
    execve("/bin/ls", argv, envp);  // 执行新程序
    _exit(127);            // exec 失败
} else if (pid > 0) {
    // 父进程
    waitpid(pid, &status, 0);  // 等待子进程结束
}
```

**为什么需要两步？**

1. **中间状态可定制**：在 fork 和 exec 之间，子进程可以修改自己的环境（重定向 fd、设置资源限制、更改命名空间等）
2. **权限分离**：fork 继承父进程权限，exec 触发 setuid 提权
3. **管道实现**：shell 管道 `ls | grep` 通过 fork+exec+pipe 实现

### 4.2 COW 在 fork+exec 中的优化

```
fork():  COW 创建
    │
    ├── 父进程和子进程共享所有物理页（只读）
    ├── 任何写操作触发缺页异常 → COW 分配新页
    │
    ▼
exec():  释放几乎所有页面
    │
    ├── exec_mmap() 释放旧 mm
    ├── 新程序重新映射所有段
    └── 旧 COW 页全部释放（由 mmput 触发）
```

**COW 的价值**：fork 后立即 exec 的场景中，COW 避免了大量不必要的内存复制。fork 时只需复制页表（设置只读），exec 时直接丢弃这些页表，分配新页表。

### 4.3 vfork + exec 特殊优化

```c
// vfork 语义
pid_t pid = vfork();
if (pid == 0) {
    // 子进程"借用"父进程地址空间
    execve("/bin/ls", argv, envp);  // 必须立即 exec 或 _exit
    _exit(127);
}
// 父进程阻塞在 vfork 上，直到子进程 exec/exit
```

**vfork 的特点**：

- 子进程共享父进程的地址空间（不复制页表）
- 父进程阻塞直到子进程 exec/exit
- 子进程不能修改父进程地址空间（会直接影响父进程）
- 子进程必须立即 exec 或 _exit

**为什么 vfork 存在但很少被使用**？因为 COW 已经使 fork 的开销很小，vfork 的使用场景非常有限，且容易出错（子进程对父进程地址空间的意外修改）。

### 4.4 进程状态转换图

```
    父进程                             子进程
    ┌──────────────┐                  ┌──────────────────┐
    │ TASK_RUNNING │                  │     (不存在)      │
    │   (执行中)    │                  │                   │
    └──────┬───────┘                  └──────────────────┘
           │
           │ fork()
           ▼
    ┌──────────────┐                  ┌──────────────────┐
    │ TASK_RUNNING │                  │   TASK_NEW       │
    │   (继续执行)  │                  │ (copy_process中)  │
    └──────┬───────┘                  └────────┬─────────┘
           │                                   │
           │                                   │ wake_up_new_task()
           │                                   ▼
           │                          ┌──────────────────┐
           │                          │  TASK_RUNNING    │
           │                          │  (就绪队列中)    │
           │                          └────────┬─────────┘
           │                                   │ 调度器选中
           │                                   ▼
           │                          ┌──────────────────┐
           │                          │  TASK_RUNNING    │
           │                          │  (开始执行)      │
           │                          └────────┬─────────┘
           │                                   │
           │                                   │ execve()
           │                                   ▼
           │                          ┌──────────────────┐
           │                          │  TASK_RUNNING    │
           │                          │  (地址空间替换)  │
           │                          │  (新程序执行)    │
           │                          └──────────────────┘
```

---

## 第5章 关键数据结构

### 5.1 struct task_struct 关键字段

定义在 [file:///home/louis/code/linux/include/linux/sched.h:809]

| 字段 | 类型 | fork/exec 相关说明 |
|------|------|-------------------|
| `__state` | unsigned int | 进程状态（TASK_RUNNING, TASK_NEW, TASK_INTERRUPTIBLE 等） |
| `stack` | void* | 内核栈指针 |
| `usage` | refcount_t | RCU 引用计数 |
| `flags` | unsigned int | 进程标志位（PF_FORKNOEXEC 等） |
| `prio` | int | 动态优先级 |
| `static_prio` | int | 静态优先级 |
| `normal_prio` | int | 普通优先级 |
| `se` | struct sched_entity | CFS 调度实体 |
| `rt` | struct sched_rt_entity | RT 调度实体 |
| `dl` | struct sched_dl_entity | Deadline 调度实体 |
| `mm` | struct mm_struct* | 地址空间（exec 时替换） |
| `active_mm` | struct mm_struct* | 活跃地址空间 |
| `real_parent` | struct task_struct* | 真实父进程 |
| `parent` | struct task_struct* | 接收 SIGCHLD 的父进程 |
| `children` | struct list_head | 子进程链表 |
| `sibling` | struct list_head | 兄弟进程链表 |
| `group_leader` | struct task_struct* | 线程组 leader |
| `pid` | pid_t | 进程 ID |
| `tgid` | pid_t | 线程组 ID |
| `thread_pid` | struct pid* | 内核 PID 结构 |
| `signal` | struct signal_struct* | 信号处理信息 |
| `sighand` | struct sighand_struct* | 信号处理函数 |
| `files` | struct files_struct* | 文件描述符表 |
| `fs` | struct fs_struct* | 文件系统上下文 |
| `nsproxy` | struct nsproxy* | 命名空间代理 |
| `self_exec_id` | u64 | 进程执行计数（exec 递增） |
| `parent_exec_id` | u64 | 父进程执行计数 |
| `stack_canary` | unsigned long | 栈保护 canary 值 |
| `thread` | struct thread_struct | 架构特定线程状态 |

### 5.2 struct linux_binprm

定义在 [file:///home/louis/code/linux/include/linux/binfmts.h:18]

| 字段 | 类型 | 说明 |
|------|------|------|
| `vma` | struct vm_area_struct* | 初始栈 VMA（MMU 模式） |
| `vma_pages` | unsigned long | 初始栈页数 |
| `argmin` | unsigned long | 参数栈空间下限标记 |
| `mm` | struct mm_struct* | 新进程的地址空间 |
| `p` | unsigned long | 栈顶指针（当前内存顶部） |
| `have_execfd` | unsigned int:1 | 是否传递 execfd 给用户空间 |
| `execfd_creds` | unsigned int:1 | 是否使用脚本的凭证 |
| `secureexec` | unsigned int:1 | 是否特权提升 exec |
| `point_of_no_return` | unsigned int:1 | 是否已过不可返回点 |
| `comm_from_dentry` | unsigned int:1 | 进程名是否来自 dentry |
| `is_check` | unsigned int:1 | 是否仅检查可执行性 |
| `executable` | struct file* | 传递给解释器的可执行文件 |
| `interpreter` | struct file* | 解释器文件 |
| `file` | struct file* | 当前正在处理的文件 |
| `cred` | struct cred* | 新凭证 |
| `unsafe` | int | 执行安全性掩码 |
| `per_clear` | unsigned int | 需要清除的 personality 位 |
| `argc` | int | 参数个数 |
| `envc` | int | 环境变量个数 |
| `filename` | const char* | 二进制文件名（procps 所见） |
| `interp` | const char* | 实际执行的名称 |
| `fdpath` | const char* | execveat 生成的路径 |
| `interp_flags` | unsigned int | 解释器标志 |
| `execfd` | int | 执行文件描述符 |
| `exec` | unsigned long | 执行点 |
| `rlim_stack` | struct rlimit | 保存的 RLIMIT_STACK |
| `buf` | char[256] | 文件前 256 字节（ELF 头等） |

### 5.3 struct kernel_clone_args

定义在 [file:///home/louis/code/linux/include/linux/sched/task.h:23]

| 字段 | 类型 | 说明 |
|------|------|------|
| `flags` | u64 | CLONE_* 标志位 |
| `pidfd` | int __user* | pidfd 返回地址 |
| `child_tid` | int __user* | 子进程 TID 地址 |
| `parent_tid` | int __user* | 父进程 TID 地址 |
| `name` | const char* | 进程名（kthread 使用） |
| `exit_signal` | int | 退出信号（fork 为 SIGCHLD） |
| `kthread` | u32:1 | 是否为内核线程 |
| `io_thread` | u32:1 | 是否为 IO 线程 |
| `user_worker` | u32:1 | 是否为用户工作线程 |
| `no_files` | u32:1 | 是否不复制文件描述符 |
| `stack` | unsigned long | 子进程栈指针 |
| `stack_size` | unsigned long | 栈大小 |
| `tls` | unsigned long | TLS 描述符 |
| `set_tid` | pid_t* | 指定 PID 数组 |
| `set_tid_size` | size_t | set_tid 数组大小 |
| `cgroup` | int | cgroup 描述符 |
| `idle` | int | 是否为 idle 进程 |
| `fn` | int (*)(void*) | 线程函数（kthread） |
| `fn_arg` | void* | 线程函数参数 |
| `cgrp` | struct cgroup* | cgroup 指针 |
| `cset` | struct css_set* | cgroup 子系统集 |
| `kill_seq` | unsigned int | 杀死序列号 |

### 5.4 struct mm_struct 的 fork/exec 相关字段

定义在 [file:///home/louis/code/linux/include/linux/mm_types.h]

| 字段 | 类型 | 说明 |
|------|------|------|
| `mmap` | struct vm_area_struct* | VMA 链表头 |
| `mm_rb` | struct rb_root | VMA 红黑树根 |
| `mm_mt` | struct maple_tree | Maple Tree 根 |
| `mmap_lock` | struct rw_semaphore | 地址空间读写锁 |
| `pgd` | pgd_t* | 页全局目录 |
| `mm_users` | atomic_t | 用户空间引用计数 |
| `mm_count` | atomic_t | 内核空间引用计数 |
| `total_vm` | unsigned long | 总虚拟页面数 |
| `start_code, end_code` | unsigned long | 代码段范围 |
| `start_data, end_data` | unsigned long | 数据段范围 |
| `start_brk, brk` | unsigned long | 堆范围 |
| `start_stack` | unsigned long | 栈起始地址 |
| `task_size` | unsigned long | 任务地址空间大小 |
| `arg_start, arg_end` | unsigned long | 参数区范围 |
| `env_start, env_end` | unsigned long | 环境变量区范围 |
| `binfmt` | struct linux_binfmt* | 二进制格式 |
| `flags` | unsigned long | MMF_* 标志 |
| `context` | struct mm_context_t | 架构特定上下文 |

---

## 第6章 完整调用链速查表

### 6.1 fork() 调用链

```
fork()  (glibc 封装 → SVC #0)
  │
  ├── el0t_64_sync_handler                [arch/arm64/kernel/entry-common.c:803]
  │   └── el0_svc(regs)                    [arch/arm64/kernel/entry-common.c:710]
  │
  ├── do_el0_svc(regs)                     [arch/arm64/kernel/syscall.c]
  │   └── invoke_syscall(regs, sc_nr, scno, sys_call_table)
  │
  ├── __arm64_sys_clone(regs)             [arch/arm64/include/asm/syscall_wrapper.h:46]
  │
  ├── SYSCALL_DEFINE5(clone)               [kernel/fork.c:2762]
  │   └── struct kernel_clone_args args = { .exit_signal = SIGCHLD }
  │
  ├── kernel_clone(&args)                  [kernel/fork.c:2612]
  │   │
  │   ├── copy_process(NULL, trace, node, args)  [kernel/fork.c:1964]
  │   │   │
  │   │   ├── 标志位合法性检查              [kernel/fork.c:1977-2029]
  │   │   │
  │   │   ├── dup_task_struct(current, node)  [kernel/fork.c:909]
  │   │   │   ├── alloc_task_struct_node(node)    // 分配 task_struct
  │   │   │   ├── arch_dup_task_struct(tsk, orig)  // ARM64 memcpy
  │   │   │   ├── alloc_thread_stack_node(tsk, node)  // 分配内核栈
  │   │   │   ├── setup_thread_stack(tsk, orig)    // 设置 thread_info
  │   │   │   └── refcount_set(&tsk->rcu_users, 2)  // 引用计数初始化
  │   │   │
  │   │   ├── copy_creds(p, clone_flags)   // 复制凭证 [kernel/cred.c]
  │   │   │   ├── prepare_creds()
  │   │   │   └── RLIMIT_NPROC 检查
  │   │   │
  │   │   ├── sched_fork(clone_flags, p)   [kernel/fork.c:2192]
  │   │   │   ├── __sched_fork() → p->__state = TASK_NEW
  │   │   │   ├── init_entity_runnable_average()
  │   │   │   └── 选择调度类
  │   │   │
  │   │   ├── copy_files(clone_flags, p, no_files)  [kernel/fork.c:2211]
  │   │   │   └── CLONE_FILES ? 共享 : dup_fd()
  │   │   │
  │   │   ├── copy_fs(clone_flags, p)      [kernel/fork.c:2214]
  │   │   │   └── CLONE_FS ? 共享 : copy_fs_struct()
  │   │   │
  │   │   ├── copy_sighand(clone_flags, p) [kernel/fork.c:2217]
  │   │   │   └── CLONE_SIGHAND ? 共享 : 复制信号处理函数
  │   │   │
  │   │   ├── copy_signal(clone_flags, p)  [kernel/fork.c:2220]
  │   │   │
  │   │   ├── copy_mm(clone_flags, p)      [kernel/fork.c:1556]
  │   │   │   └── CLONE_VM ? 共享 : dup_mm() → dup_mmap()
  │   │   │       ├── __mt_dup()  // Maple Tree 复制
  │   │   │       ├── vm_area_dup()  // VMA 复制
  │   │   │       └── copy_page_range()  // COW 页表
  │   │   │
  │   │   ├── copy_namespaces(clone_flags, p)  [kernel/fork.c:2226]
  │   │   │
  │   │   ├── copy_io(clone_flags, p)    [kernel/fork.c:2229]
  │   │   │
  │   │   ├── copy_thread(p, args)       [arch/arm64/kernel/process.c:411]
  │   │   │   ├── *childregs = *current_pt_regs()
  │   │   │   ├── childregs->regs[0] = 0  // fork 返回 0
  │   │   │   ├── p->thread.cpu_context.pc = ret_from_fork
  │   │   │   └── p->thread.cpu_context.sp = childregs
  │   │   │
  │   │   ├── alloc_pid()                [kernel/pid.c]
  │   │   │
  │   │   ├── 设置进程关系 (real_parent, parent_exec_id)
  │   │   │
  │   │   ├── write_lock_irq(&tasklist_lock)
  │   │   ├── __pidhash 插入 → __tasklist 链表插入
  │   │   ├── write_unlock_irq(&tasklist_lock)
  │   │   │
  │   │   └── 后处理:
  │   │       ├── proc_fork_connector(p)
  │   │       ├── sched_post_fork(p)
  │   │       └── cgroup_post_fork(p, args)
  │   │
  │   ├── trace_sched_process_fork(current, p)  // tracepoint
  │   │
  │   ├── wake_up_new_task(p)            [kernel/sched/core.c]
  │   │   ├── p->__state = TASK_RUNNING
  │   │   ├── select_task_rq(p, wake_cpu, WF_FORK)  // 负载均衡
  │   │   ├── activate_task(rq, p, ENQUEUE_NOCLOCK)  // 加入运行队列
  │   │   └── check_preempt_curr(rq, p, WF_FORK)     // 抢占检查
  │   │
  │   ├── ptrace_event_pid(trace, pid)    // ptrace 通知
  │   │
  │   └── if (CLONE_VFORK) → wait_for_vfork_done()
  │
  └── 返回 nr (子进程 PID)
```

### 6.2 execve() 调用链

```
execve(filename, argv, envp)  (glibc 封装 → SVC #0)
  │
  ├── el0t_64_sync_handler → el0_svc(regs)  [arch/arm64/kernel/entry-common.c]
  │
  ├── do_el0_svc(regs) → invoke_syscall()
  │
  ├── __arm64_sys_execve(regs)            [arch/arm64/include/asm/syscall_wrapper.h]
  │
  ├── SYSCALL_DEFINE3(execve)             [fs/exec.c:1924]
  │
  ├── do_execveat_common(AT_FDCWD, ...)   [fs/exec.c:1778]
  │   │
  │   ├── CLASS(bprm, bprm)(fd, filename, flags)
  │   │   └── alloc_bprm(fd, filename, flags)  [fs/exec.c:1395]
  │   │       ├── do_open_execat(fd, filename, flags)  [fs/exec.c:756]
  │   │       │   ├── do_file_open()        // 打开文件
  │   │       │   ├── path_noexec() 检查
  │   │       │   ├── S_ISREG() 检查
  │   │       │   └── deny_write_access()   // 禁止写入
  │   │       │
  │   │       ├── kzalloc_obj(*bprm)        // 分配 bprm
  │   │       │
  │   │       └── bprm_mm_init(bprm)        [fs/exec.c:238]
  │   │           ├── mm_alloc()            // 分配新 mm_struct
  │   │           └── create_init_stack_vma()  // 创建初始栈 VMA
  │   │
  │   ├── count(argv, MAX_ARG_STRINGS)      // 统计参数
  │   ├── count(envp, MAX_ARG_STRINGS)      // 统计环境变量
  │   ├── bprm_stack_limits(bprm)           // 栈空间限制检查
  │   ├── copy_string_kernel(filename, bprm) // 复制文件名
  │   ├── copy_strings(envp, bprm)          // 复制环境变量到栈
  │   ├── copy_strings(argv, bprm)          // 复制参数到栈
  │   │
  │   └── bprm_execve(bprm)                 [fs/exec.c:1724]
  │       │
  │       ├── prepare_bprm_creds(bprm)      [fs/exec.c:1344]
  │       │   └── prepare_exec_creds()
  │       │
  │       ├── check_unsafe_exec(bprm)       [fs/exec.c:1486]
  │       │
  │       ├── sched_exec()                  // 调度器通知
  │       │
  │       ├── security_bprm_creds_for_exec(bprm)  // LSM 钩子
  │       │
  │       └── exec_binprm(bprm)             [fs/exec.c:1679]
  │           │
  │           ├── search_binary_handler(bprm)  [fs/exec.c:1645]
  │           │   │
  │           │   ├── prepare_binprm(bprm)  // 读取文件头
  │           │   │
  │           │   ├── security_bprm_check(bprm)  // LSM 检查
  │           │   │
  │           │   └── list_for_each_entry(fmt, &formats, lh)
  │           │       └── fmt->load_binary(bprm)
  │           │           │
  │           │           └── load_elf_binary(bprm)  [fs/binfmt_elf.c:833]
  │           │               │
  │           │               ├── ELF 头检查            [fs/binfmt_elf.c:833]
  │           │               │   ├── memcmp(elf_ex->e_ident, ELFMAG, SELFMAG)
  │           │               │   ├── elf_ex->e_type ∈ {ET_EXEC, ET_DYN}
  │           │               │   └── elf_check_arch(elf_ex)
  │           │               │
  │           │               ├── load_elf_phdrs()       [fs/binfmt_elf.c:521]
  │           │               │
  │           │               ├── 遍历程序头:
  │           │               │   ├── PT_INTERP → 打开动态链接器
  │           │               │   ├── PT_LOAD → 记录可加载段
  │           │               │   ├── PT_GNU_STACK → 栈执行权限
  │           │               │   └── PT_GNU_RELRO → 只读重定位
  │           │               │
  │           │               ├── begin_new_exec(bprm)   [fs/exec.c:1091]
  │           │               │   │  (点 no return!)
  │           │               │   │
  │           │               │   ├── bprm_creds_from_file(bprm)  [fs/exec.c:1583]
  │           │               │   │   └── bprm_fill_uid(bprm, file)  // setuid/setgid
  │           │               │   │
  │           │               │   ├── bprm->point_of_no_return = true
  │           │               │   │
  │           │               │   ├── de_thread(me)       // 清除其他线程
  │           │               │   │   └── zap_other_threads() + 等待退出
  │           │               │   │
  │           │               │   ├── io_uring_task_cancel()
  │           │               │   │
  │           │               │   ├── unshare_files()     // 取消共享 fd 表
  │           │               │   │
  │           │               │   ├── set_mm_exe_file(bprm->mm, bprm->file)
  │           │               │   │
  │           │               │   ├── exec_mmap(bprm->mm) // 切换地址空间
  │           │               │   │   ├── activate_mm(mm, old_mm)  // 切换页表
  │           │               │   │   └── mmput(old_mm)   // 释放旧 mm
  │           │               │   │
  │           │               │   ├── exec_task_namespaces()
  │           │               │   │
  │           │               │   ├── flush_thread()      // 清空架构状态
  │           │               │   │   └── fpsimd_flush_thread(), tls_thread_flush(), ...
  │           │               │   │
  │           │               │   ├── do_close_on_exec(me->files)  // 关闭 FD_CLOEXEC
  │           │               │   │
  │           │               │   ├── commit_creds(bprm->cred)     // 提交新凭证
  │           │               │   │
  │           │               │   ├── __set_task_comm()   // 更新进程名
  │           │               │   ├── self_exec_id++      // exec 计数递增
  │           │               │   └── flush_signal_handlers(me, 0)  // 重置信号处理
  │           │               │
  │           │               ├── 加载解释器 (如有 PT_INTERP):
  │           │               │   ├── load_elf_interp()   [fs/binfmt_elf.c:646]
  │           │               │   └── elf_load() → do_mmap()  // 映射解释器
  │           │               │
  │           │               ├── 映射 PT_LOAD 段:
  │           │               │   └── elf_load() → do_mmap()  // 映射每个段
  │           │               │
  │           │               ├── setup_arg_pages()       [fs/exec.c]
  │           │               │   └── 调整栈 VMA 大小和权限
  │           │               │
  │           │               ├── create_elf_tables()     [fs/binfmt_elf.c]
  │           │               │   └── 构建 argv, envp, auxv
  │           │               │
  │           │               ├── SET_PERSONALITY(elf_ex)
  │           │               │
  │           │               └── START_THREAD(regs, elf_entry, bprm->p)
  │           │                   └── regs->pc = elf_entry; regs->sp = bprm->p
  │           │
  │           ├── audit_bprm(bprm)           // 审计日志
  │           ├── trace_sched_process_exec() // tracepoint
  │           ├── ptrace_event(PTRACE_EVENT_EXEC)  // ptrace 通知
  │           └── proc_exec_connector()      // 进程事件连接器
  │
  └── 返回 0 (成功) / -1 (失败)
```

---

## 第7章 参考

### 7.1 源码文件索引

| 文件 | 内容 | 关键行号 |
|------|------|---------|
| [file:///home/louis/code/linux/kernel/fork.c] | fork 核心实现 | `SYSCALL_DEFINE0(fork)` L2731, `kernel_clone()` L2612, `copy_process()` L1964, `dup_task_struct()` L909, `copy_mm()` L1556 |
| [file:///home/louis/code/linux/fs/exec.c] | exec 核心实现 | `SYSCALL_DEFINE3(execve)` L1924, `do_execveat_common()` L1778, `bprm_execve()` L1724, `exec_binprm()` L1679, `search_binary_handler()` L1645, `begin_new_exec()` L1091, `alloc_bprm()` L1395, `bprm_mm_init()` L238, `do_open_execat()` L756 |
| [file:///home/louis/code/linux/fs/binfmt_elf.c] | ELF 二进制加载 | `load_elf_binary()` L833, `load_elf_phdrs()` L521, `load_elf_interp()` L646 |
| [file:///home/louis/code/linux/arch/arm64/kernel/process.c] | ARM64 进程管理 | `copy_thread()` L411, `arch_dup_task_struct()` L348, `flush_thread()` L≈ |
| [file:///home/louis/code/linux/arch/arm64/kernel/entry.S] | ARM64 汇编入口 | `ret_from_fork` L937, `cpu_switch_to` L899 |
| [file:///home/louis/code/linux/arch/arm64/kernel/entry-common.c] | ARM64 异常处理 | `el0t_64_sync_handler` L803, `el0_svc()` L710 |
| [file:///home/louis/code/linux/arch/arm64/include/asm/syscall_wrapper.h] | ARM64 系统调用包装 | `__SYSCALL_DEFINEx` L46 |
| [file:///home/louis/code/linux/include/linux/sched.h] | 进程调度核心定义 | `struct task_struct` L809 |
| [file:///home/louis/code/linux/include/linux/sched/task.h] | 任务生命周期接口 | `struct kernel_clone_args` L23, `kernel_clone()` L98, `copy_process()` L99, `sched_fork()` L66 |
| [file:///home/louis/code/linux/include/linux/binfmts.h] | 二进制格式定义 | `struct linux_binprm` L18, `struct linux_binfmt` L89, `begin_new_exec()` L125 |
| [file:///home/louis/code/linux/include/linux/mm_types.h] | 内存管理类型 | `struct mm_struct` |
| [file:///home/louis/code/linux/kernel/pid.c] | PID 分配 | `alloc_pid()` |
| [file:///home/louis/code/linux/kernel/cred.c] | 凭证管理 | `copy_creds()`, `commit_creds()` |
| [file:///home/louis/code/linux/kernel/sched/core.c] | 调度器核心 | `wake_up_new_task()`, `sched_fork()`, `activate_task()` |
| [file:///home/louis/code/linux/mm/mmap.c] | 内存映射 | `dup_mmap()` L1731 |
| [file:///home/louis/code/linux/mm/memory.c] | 内存管理 | `copy_page_range()` |
| [file:///home/louis/code/linux/include/linux/syscalls.h] | 系统调用宏 | `SYSCALL_DEFINE0` L212, `SYSCALL_DEFINEx` L245 |

### 7.2 内核文档链接

- [file:///home/louis/code/linux/Documentation/process/] - 内核开发流程文档
- [file:///home/louis/code/linux/Documentation/scheduler/] - 调度器文档
- [file:///home/louis/code/linux/Documentation/filesystems/] - 文件系统文档

### 7.3 相关 notes 文档

- [file:///home/louis/code/linux/notes/syscall/process/fork.md] - fork 系统调用分析
- [file:///home/louis/code/linux/notes/syscall/process/clone.md] - clone 系统调用分析
- [file:///home/louis/code/linux/notes/syscall/process/execve.md] - execve 系统调用分析
- [file:///home/louis/code/linux/notes/syscall/process/execveat.md] - execveat 系统调用分析
- [file:///home/louis/code/linux/notes/syscall/process/exit.md] - exit 系统调用分析

---

## 第8章 进程退出（exit）完整流程

### 8.1 概述

进程退出是进程生命周期的终结阶段，涉及到内核资源的系统清理、父进程通知、僵尸状态转换等一系列操作。Linux 内核提供以下退出相关的系统调用：

| 系统调用 | 功能 | 入口函数 |
|---------|------|---------|
| `exit(int code)` | 终止当前进程 | `do_exit()` |
| `exit_group(int code)` | 终止线程组中所有进程 | `do_group_exit()` |
| `wait4(pid, status, opts, rusage)` | 等待子进程退出并回收 | `kernel_wait4()` |

### 8.2 退出路径总览

```
用户态 exit() / exit_group()
    │
    ▼
内核入口
    │
    ├── exit(int code)                              [file:///home/louis/code/linux/kernel/exit.c:1083]
    │   └── do_exit((code & 0xff) << 8)
    │
    └── exit_group(int code)                        [file:///home/louis/code/linux/kernel/exit.c:1127]
        └── do_group_exit((code & 0xff) << 8)
            ├── zap_other_threads()      // 杀死组内其他线程
            └── do_exit(code)
                │
                ▼
            do_exit(code)                           [file:///home/louis/code/linux/kernel/exit.c:896]
                │
                ├── 1. 同步退出组
                ├── 2. ptrace 事件通知
                ├── 3. 清理 io_uring
                ├── 4. 退出信号处理 (设置 PF_EXITING)
                ├── 5. 递减 signal->live
                ├── 6. 清理 perf_event
                ├── 7. 释放地址空间 (exit_mm)
                ├── 8. 释放各种子系统资源
                ├── 9. exit_notify() → 通知父进程
                └── 10. do_task_dead() → 调度器移除
```

### 8.3 do_exit() — 退出核心函数

```c
// file:///home/louis/code/linux/kernel/exit.c:896
void __noreturn do_exit(long code)
{
    struct task_struct *tsk = current;
    int group_dead;

    // 1. 同步退出组——确保组内只有一个线程执行 do_exit
    synchronize_group_exit(tsk, code);

    // 2. ptrace 事件通知 (PTRACE_EVENT_EXIT)
    ptrace_event(PTRACE_EVENT_EXIT, code);

    // 3. 取消 io_uring 操作
    io_uring_files_cancel();

    // 4. 退出信号处理：设置 PF_EXITING 标志
    //    PF_EXITING 防止信号继续递送
    exit_signals(tsk);  /* sets PF_EXITING */

    // 5. 递减 signal->live（线程组活跃线程计数）
    //    group_dead 表示是否为线程组最后一个退出的线程
    group_dead = atomic_dec_and_test(&tsk->signal->live);

    // 6. 如果是最后一个线程，取消定时器、清理 itimers
    if (group_dead) {
        if (unlikely(is_global_init(tsk)))
            panic("Attempted to kill init!");
        hrtimer_cancel(&tsk->signal->real_timer);
        exit_itimers(tsk);
    }

    // 7. 记录退出码并发送 tracepoint
    tsk->exit_code = code;
    trace_sched_process_exit(tsk, group_dead);

    // 8. perf_event 清理（将继承的计数器刷新到父进程）
    perf_event_exit_task(tsk);

    // 9. 释放地址空间
    exit_mm();

    // 10. 释放各种子系统资源
    exit_sem(tsk);      // System V 信号量
    exit_shm(tsk);      // 共享内存
    exit_files(tsk);    // 文件描述符表 (put_files_struct)
    exit_fs(tsk);       // 文件系统上下文 (fs_struct)
    exit_nsproxy_namespaces(tsk);  // 命名空间
    exit_task_work(tsk);
    exit_thread(tsk);

    // 11. 通知父进程（转为僵尸状态或立即回收）
    exit_notify(tsk, group_dead);

    // 12. 最终清理
    exit_rcu();
    exit_tasks_rcu_finish();

    // 13. 调度器移除——此函数不会返回
    do_task_dead();
}
```

#### 8.3.1 各清理步骤详解

**exit_mm() — 释放地址空间：**

```
exit_mm():
    │
    ├── mm_release()              // 释放用户态 mm 相关资源
    │   ├── 清除 CPU 上的 mm_cid
    │   ├── 唤醒 clear_child_tid 等待者
    │   └── 释放 futex 状态
    │
    ├── mmap_read_lock(mm)        // 获取 mmap 读锁
    │
    ├── mmgrab(mm)                // 增加引用计数防止释放
    │
    ├── task_lock(tsk);
    │   tsk->mm = NULL;           // 断开与地址空间的连接
    │   task_unlock(tsk);
    │
    ├── enter_lazy_tlb(mm, tsk)   // 切换到惰性 TLB 模式
    │
    ├── mmap_read_unlock(mm)
    │
    └── mmput(mm)                 // 释放 mm_struct
        └── 如果引用计数归零:
            ├── exit_mmap(mm)     // 卸载所有 VMA, 释放页表
            └── mmdrop(mm)        // 释放 mm_struct 本身
```

**exit_files() — 释放文件描述符表：**

```c
// file:///home/louis/code/linux/fs/file.c:526
void exit_files(struct task_struct *tsk)
{
    struct files_struct *files = tsk->files;
    if (files) {
        task_lock(tsk);
        tsk->files = NULL;          // 断开连接
        task_unlock(tsk);
        put_files_struct(files);    // 递减引用计数, 归零时关闭所有 fd
    }
}
```

**exit_fs() — 释放文件系统上下文：**

```c
// file:///home/louis/code/linux/fs/fs_struct.c:90
void exit_fs(struct task_struct *tsk)
{
    struct fs_struct *fs = tsk->fs;
    if (fs) {
        int kill;
        task_lock(tsk);
        tsk->fs = NULL;             // 断开连接
        kill = !--fs->users;        // 递减引用计数
        task_unlock(tsk);
        if (kill)
            free_fs_struct(fs);     // 释放 pwd, root 等
    }
}
```

### 8.4 exit_notify() — 父进程通知与僵尸状态转换

```c
// file:///home/louis/code/linux/kernel/exit.c:736
static void exit_notify(struct task_struct *tsk, int group_dead)
{
    bool autoreap;
    struct task_struct *p, *n;
    LIST_HEAD(dead);

    write_lock_irq(&tasklist_lock);

    // 1. 将子进程重新挂载到其祖父进程或 init 进程
    forget_original_parent(tsk, &dead);

    if (group_dead)
        kill_orphaned_pgrp(tsk->group_leader, NULL);

    // 2. 设置退出状态为 EXIT_ZOMBIE
    tsk->exit_state = EXIT_ZOMBIE;

    // 3. 判断是否需要自动回收（autoreap）
    if (unlikely(tsk->ptrace)) {
        // 被 ptrace 追踪的进程：通知追踪器
        int sig = thread_group_leader(tsk) &&
                thread_group_empty(tsk) &&
                !ptrace_reparented(tsk) ?
            tsk->exit_signal : SIGCHLD;
        autoreap = do_notify_parent(tsk, sig);
    } else if (thread_group_leader(tsk)) {
        // 线程组 leader：通知父进程
        autoreap = thread_group_empty(tsk) &&
            do_notify_parent(tsk, tsk->exit_signal);
    } else {
        // 非 leader 子线程：自动回收（无需通知父进程）
        autoreap = true;
        do_notify_pidfd(tsk);
    }

    // 4. 如果自动回收，直接转为 EXIT_DEAD
    if (autoreap) {
        tsk->exit_state = EXIT_DEAD;
        list_add(&tsk->ptrace_entry, &dead);
    }

    write_unlock_irq(&tasklist_lock);

    // 5. 回收自动回收的进程
    list_for_each_entry_safe(p, n, &dead, ptrace_entry) {
        list_del_init(&p->ptrace_entry);
        release_task(p);
    }
}
```

#### 8.4.1 僵尸状态与自动回收

```
exit_notify() 决策树:
    │
    ├── 是线程组 leader 且被 ptrace 追踪
    │   ├── 通知追踪器 (ptracer)
    │   └── 如果追踪器已设置 WNOHANG 且不关心 → autoreap
    │
    ├── 是线程组 leader 且未被 ptrace
    │   ├── 通知原始父进程 (通过 SIGCHLD 信号)
    │   ├── 父进程 wait4() 后回收
    │   └── 如果父进程已死 → 挂载到 init → autoreap
    │
    ├── 非 leader 子线程 (CLONE_THREAD)
    │   └── 自动回收 (autoreap = true) → 直接 EXIT_DEAD
    │
    └── 父进程设置了 SA_NOCLDWAIT 或 signal->flags & SIGNAL_NOCHILDSTOP
        └── 立即回收 (autoreap = true)
```

**僵尸进程（EXIT_ZOMBIE）**：

```
    僵尸进程状态:
    ┌──────────────────────────────────────┐
    │  task_struct (保留)                    │
    │  ├── exit_state = EXIT_ZOMBIE         │
    │  ├── exit_code = 退出码               │
    │  ├── 信号处理状态 (供 wait4 读取)      │
    │  ├── 资源使用统计 (rusage)            │
    │  └── PID 被保留                        │
    │                                        │
    │  已释放的资源:                          │
    │  ├── 地址空间 (mm)                     │
    │  ├── 文件描述符 (files)                │
    │  ├── 文件系统上下文 (fs)               │
    │  ├── 信号处理 (sighand)                │
    │  └── 命名空间 (nsproxy)                │
    └──────────────────────────────────────┘
```

**状态定义：**

```c
// file:///home/louis/code/linux/include/linux/sched.h:113
#define EXIT_DEAD    0x00000010   // 已回收（task_struct 将被释放）
#define EXIT_ZOMBIE  0x00000020   // 僵尸（等待父进程 wait4 回收）
#define EXIT_TRACE   (EXIT_ZOMBIE | EXIT_DEAD)  // ptrace 追踪中退出
```

### 8.5 release_task() — 最终资源回收

```c
// file:///home/louis/code/linux/kernel/exit.c:324
void release_task(struct task_struct *p)
{
    // 1. 释放 PID
    exit_pid(p);
    // 2. 释放信号处理结构
    __exit_signal(p);
    // 3. 释放 task_struct 本身
    free_task(p);
}
```

**release_task() 释放的资源：**

```
release_task() 完整流程:
    │
    ├── exit_pid(p)                          // 释放 PID
    │   └── pid = remove_pid(p) → free_pid(pid)
    │
    ├── __exit_signal(p)                     // 释放信号结构
    │   ├── task_cputime() → 累加 CPU 时间
    │   ├── sig->nr_threads--
    │   ├── __unhash_process() → 从全局哈希表移除
    │   └── put_signal_struct(sig)           // 释放 signal_struct
    │
    ├── __exit_sighand(p)                    // 释放 sighand_struct
    │   └── put_sighand(p)                   // 递减引用计数
    │
    ├── __cleanup_sighand()                  // 释放信号处理函数表
    │
    ├── sched_core_free(p)
    │
    ├── put_task_struct_rcu_user(p)          // 通过 RCU 延迟释放
    │   └── free_task(p)
    │       ├── free_task_struct(p)          // 释放 task_struct slab
    │       ├── free_thread_stack(p)         // 释放内核栈
    │       └── put_cred(p->cred)
    │
    └── 最终: task_struct 和内核栈彻底释放
```

### 8.6 do_wait() / wait4() — 父进程回收

父进程通过 `wait4()` 系统调用等待子进程退出并回收其资源。

```c
// file:///home/louis/code/linux/kernel/exit.c:1905
SYSCALL_DEFINE4(wait4, pid_t, upid, int __user *, stat_addr,
                int, options, struct rusage __user *, ru)
{
    struct rusage r;
    long err = kernel_wait4(upid, stat_addr, options, ru ? &r : NULL);
    ...
    return err;
}
```

**wait4 核心流程：**

```
kernel_wait4(pid, stat_addr, options, ru):
    │
    └── do_wait(&wo)
        │
        └── while (1) {
                set_current_state(TASK_INTERRUPTIBLE);
                │
                retval = __do_wait(wo);
                │
                ├── if (retval) → 返回 (找到子进程)
                │
                └── else → schedule()  // 睡眠等待子进程
                        │
                        └── 子进程 exit_notify() 发送 SIGCHLD
                            → 唤醒父进程 → 重新检查
            }
```

**__do_wait 遍历子进程：**

```c
// file:///home/louis/code/linux/kernel/exit.c:1663
long __do_wait(struct wait_opts *wo)
{
    read_lock(&tasklist_lock);

    if (wo->wo_type == PIDTYPE_PID) {
        retval = do_wait_pid(wo);              // 按 PID 查找
    } else {
        struct task_struct *tsk = current;
        do {
            retval = do_wait_thread(wo, tsk);  // 遍历子进程
            if (retval)
                return retval;
            retval = ptrace_do_wait(wo, tsk);  // 遍历 ptrace 子进程
            if (retval)
                return retval;
        } while_each_thread(current, tsk);
    }
    ...
}
```

**wait_task_zombie() — 读取僵尸进程状态：**

```c
// file:///home/louis/code/linux/kernel/exit.c:1083
static int wait_task_zombie(struct wait_opts *wo, struct task_struct *p)
{
    // 1. 读取退出码
    exit_code = p->exit_code;

    // 2. 读取资源使用统计
    //    task_cputime(), getrusage() 等

    // 3. 释放进程
    release_task(p);

    // 4. 返回状态信息
    wo->wo_stat = (exit_code << 8) | 0x7f;  // WIFEXITED, WEXITSTATUS
    ...
}
```

### 8.7 do_task_dead() — 调度器最终移除

`do_exit()` 的最后一个步骤：

```c
// file:///home/louis/code/linux/kernel/sched/core.c:4860
void __noreturn do_task_dead(void)
{
    /* 设置特殊状态，防止被调度器选中 */
    WRITE_ONCE(current->__state, TASK_DEAD);

    /* 主动调度，此后再也不会被调度回来 */
    schedule();

    /* 永远不会到达这里 */
    BUG();
}
```

**TASK_DEAD 状态的特殊处理：**

```
do_task_dead() 后:
    │
    schedule() 选择下一个任务:
    │
    ├── context_switch() 中的 finish_task_switch()
    │   └── put_task_struct_rcu_user(prev)
    │       └── 最终释放 task_struct 和内核栈
    │
    └── CPU 切换到新任务执行
        退出进程的 task_struct 被 RCU 异步释放
```

### 8.8 完整退出流程调用链

```
exit_group(code)
    │
    └── do_group_exit(code)           [file:///home/louis/code/linux/kernel/exit.c:1092]
        │
        ├── zap_other_threads()       // 发送 SIGKILL 给组内其他线程
        │
        └── do_exit(code)             [file:///home/louis/code/linux/kernel/exit.c:896]
            │
            ├── synchronize_group_exit(tsk, code)
            ├── ptrace_event(PTRACE_EVENT_EXIT, code)
            ├── io_uring_files_cancel()
            ├── exit_signals(tsk)     // PF_EXITING
            ├── atomic_dec_and_test(&signal->live)  // group_dead
            ├── hrtimer_cancel() / exit_itimers()
            ├── trace_sched_process_exit()
            ├── perf_event_exit_task()
            ├── exit_mm()             // 释放地址空间
            │   ├── mm_release()
            │   ├── task_lock / tsk->mm = NULL
            │   └── mmput(mm) → exit_mmap() → mmdrop()
            │
            ├── exit_sem(tsk)         // SysV 信号量
            ├── exit_shm(tsk)         // 共享内存
            ├── exit_files(tsk)       // 文件描述符表
            ├── exit_fs(tsk)          // fs_struct
            ├── exit_nsproxy_namespaces()  // 命名空间
            ├── exit_task_work()
            ├── exit_thread()
            │
            ├── exit_notify(tsk, group_dead)  // 通知父进程
            │   ├── forget_original_parent()  // 重新挂载子进程
            │   ├── tsk->exit_state = EXIT_ZOMBIE
            │   ├── do_notify_parent(tsk, sig) → SIGCHLD
            │   │   └── 唤醒父进程的 wait4()
            │   ├── 如果 autoreap: EXIT_DEAD → release_task()
            │   └── 否则: 保留 EXIT_ZOMBIE 等待父进程
            │
            └── do_task_dead()        // 调度器移除
                ├── __state = TASK_DEAD
                └── schedule()        // 不会再返回
```

### 8.9 进程退出状态转换图

```
    进程退出状态转换:
                                    (父进程 wait4)
    ┌──────────┐    exit_notify()    ┌───────────┐    release_task()    ┌──────────┐
    │ TASK_    │──────────────────▶  │ EXIT_     │────────────────────▶ │ 已释放    │
    │ RUNNING  │  (或 autoreap)      │ ZOMBIE    │                     │ (无)     │
    └──────────┘                      └───────────┘                     └──────────┘
         │                                 │
         │ autoreap = true                    │
         │ (非 leader 子线程 /                 │ EXIT_DEAD (autoreap)
         │  父进程不关心)                      │
         ▼                                 ▼
    ┌──────────┐                      ┌───────────┐
    │ EXIT_    │                      │ EXIT_     │
    │ DEAD     │                      │ ZOMBIE    │
    └──────────┘                      └───────────┘
         │                                 │
         └── release_task() ───────────────┘
                     │
                     ▼
               task_struct 释放
               (RCU 延迟回收)
```

### 8.10 参考

- [file:///home/louis/code/linux/kernel/exit.c] — 进程退出核心实现
- [file:///home/louis/code/linux/kernel/sched/core.c] — `do_task_dead()` L4860
- [file:///home/louis/code/linux/fs/file.c] — `exit_files()` L526
- [file:///home/louis/code/linux/fs/fs_struct.c] — `exit_fs()` L90
- [file:///home/louis/code/linux/include/linux/sched.h] — `EXIT_DEAD/ZOMBIE/TRACE` L113
- [file:///home/louis/code/linux/notes/syscall/process/exit.md] — exit 系统调用分析