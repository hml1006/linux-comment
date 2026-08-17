# ptrace 系统调用分析

## 1. 概述

`ptrace` 提供了一种进程追踪机制，允许一个进程（追踪器）观察和控制另一个进程（被追踪者）的执行。它是调试器（如 GDB）和系统调用追踪工具（如 strace）的核心基础设施。

**原型：**

```c
// kernel/ptrace.c:1387
SYSCALL_DEFINE4(ptrace, long, request, long, pid, unsigned long, addr,
    unsigned long, data)
```

**参数：**
- `request`：要执行的 ptrace 操作
- `pid`：目标进程的 PID
- `addr`：操作相关的地址
- `data`：操作相关的数据

## 2. 主要 request 操作

| 操作 | 值 | 功能 |
|------|-----|------|
| `PTRACE_TRACEME` | 0 | 当前进程声明被其父进程追踪 |
| `PTRACE_PEEKTEXT` | 1 | 读取代码段内存 |
| `PTRACE_PEEKDATA` | 2 | 读取数据段内存 |
| `PTRACE_PEEKUSR` | 3 | 读取用户态寄存器 |
| `PTRACE_POKETEXT` | 4 | 写入代码段内存 |
| `PTRACE_POKEDATA` | 5 | 写入数据段内存 |
| `PTRACE_POKEUSR` | 6 | 写入用户态寄存器 |
| `PTRACE_CONT` | 7 | 继续执行 |
| `PTRACE_KILL` | 8 | 发送 SIGKILL |
| `PTRACE_SINGLESTEP` | 9 | 单步执行 |
| `PTRACE_ATTACH` | 16 | 附加到指定进程 |
| `PTRACE_DETACH` | 17 | 分离追踪 |
| `PTRACE_SYSCALL` | 24 | 追踪系统调用入口和出口 |
| `PTRACE_SETOPTIONS` | 0x4200 | 设置追踪选项 |
| `PTRACE_GETEVENTMSG` | 0x4201 | 获取事件消息 |
| `PTRACE_GETSIGINFO` | 0x4202 | 获取信号信息 |
| `PTRACE_SETSIGINFO` | 0x4203 | 设置信号信息 |
| `PTRACE_GETREGSET` | 0x4204 | 获取寄存器集（通过 regset 接口） |
| `PTRACE_SETREGSET` | 0x4205 | 设置寄存器集 |
| `PTRACE_SEIZE` | 0x4206 | 附加到进程（不停止进程） |
| `PTRACE_INTERRUPT` | 0x4207 | 中断被追踪进程 |
| `PTRACE_LISTEN` | 0x4208 | 监听被追踪进程 |
| `PTRACE_PEEKSIGINFO` | 0x4209 | 查看挂起信号 |
| `PTRACE_GETSIGMASK` | 0x420a | 获取信号掩码 |
| `PTRACE_SETSIGMASK` | 0x420b | 设置信号掩码 |
| `PTRACE_GET_SYSCALL_INFO` | 0x420e | 获取系统调用信息 |
| `PTRACE_SET_SYSCALL_INFO` | 0x4212 | 设置系统调用信息 |
| `PTRACE_GET_RSEQ_CONFIGURATION` | 0x420f | 获取可重启序列配置 |

## 3. 关键数据结构

### 3.1 struct task_struct 的 ptrace 相关字段

```c
// include/linux/sched.h
struct task_struct {
    unsigned int ptrace;            // ptrace 标志位
    unsigned int ptrace_event;      // ptrace 事件
    struct list_head ptrace_entry;  // ptrace 链表节点（链接到追踪器的 ptraced 列表）
    struct task_struct *parent;     // 追踪器（ptracer），ptrace 后指向追踪器
    struct task_struct *real_parent; // 真正的父进程（ptrace 期间保持不变）
    const struct cred *ptracer_cred; // 追踪器凭证
    struct list_head ptraced;       // 本进程追踪的子进程列表头
    int exit_code;                  // 退出码/停止信号（ptrace 停止时传递）
    kernel_siginfo_t *last_siginfo; // ptrace 停止时最后收到的 siginfo
    unsigned long ptrace_message;   // ptrace 事件消息
};
```

### 3.2 ptrace 标志位

```c
// include/linux/ptrace.h
#define PT_PTRACED      0x00000001  // 被 ptrace 追踪
#define PT_SEIZED       0x00010000  // 通过 SEIZE 附加，启用新行为

// 事件标志位（通过 PT_OPT_FLAG_SHIFT 偏移）
#define PT_OPT_FLAG_SHIFT    3
#define PT_EVENT_FLAG(event) (1 << (PT_OPT_FLAG_SHIFT + (event)))
#define PT_TRACESYSGOOD      PT_EVENT_FLAG(0)  // 系统调用追踪时设置，SIGTRAP|0x80

// 跟踪事件标志
#define PT_TRACE_FORK        PT_EVENT_FLAG(PTRACE_EVENT_FORK)
#define PT_TRACE_VFORK       PT_EVENT_FLAG(PTRACE_EVENT_VFORK)
#define PT_TRACE_CLONE       PT_EVENT_FLAG(PTRACE_EVENT_CLONE)
#define PT_TRACE_EXEC        PT_EVENT_FLAG(PTRACE_EVENT_EXEC)
#define PT_TRACE_VFORK_DONE  PT_EVENT_FLAG(PTRACE_EVENT_VFORK_DONE)
#define PT_TRACE_EXIT        PT_EVENT_FLAG(PTRACE_EVENT_EXIT)
#define PT_TRACE_SECCOMP     PT_EVENT_FLAG(PTRACE_EVENT_SECCOMP)

#define PT_EXITKILL          (PTRACE_O_EXITKILL << PT_OPT_FLAG_SHIFT)
#define PT_SUSPEND_SECCOMP   (PTRACE_O_SUSPEND_SECCOMP << PT_OPT_FLAG_SHIFT)
```

### 3.3 JOBCTL 标志位

```c
// include/linux/sched/jobctl.h
#define JOBCTL_TRACED           0x00000800  // 进程处于 TASK_TRACED 状态
#define JOBCTL_TRAP_STOP        0x00001000  // 需要执行 STOP trap
#define JOBCTL_TRAP_NOTIFY      0x00002000  // 需要执行 NOTIFY trap
#define JOBCTL_TRAPPING         0x00004000  // 正在转换到 TASK_TRACED 状态
#define JOBCTL_LISTENING        0x00008000  // PTRACE_LISTEN 模式
#define JOBCTL_TRAP_MASK        0x0000f000  // 所有 trap 位的掩码
#define JOBCTL_PTRACE_FROZEN    0x00010000  // ptrace 操作中冻结
#define JOBCTL_STOP_PENDING     0x00020000  // 有停止信号挂起
#define JOBCTL_STOP_DEQUEUED    0x00040000  // 停止信号已出队
```

### 3.4 TIF 标志位

```c
// arch/arm64/include/asm/thread_info.h
#define TIF_SYSCALL_TRACE       0   // 系统调用追踪（ptrace）
#define TIF_SYSCALL_EMU         1   // 系统调用模拟
#define TIF_SINGLESTEP          4   // 单步执行
#define TIF_SYSCALL_WORK        (BIT(TIF_SYSCALL_TRACE) | BIT(TIF_SYSCALL_EMU))
```

### 3.5 ptrace 事件

```c
// include/uapi/linux/ptrace.h
#define PTRACE_EVENT_FORK       1   // fork 事件
#define PTRACE_EVENT_VFORK      2   // vfork 事件
#define PTRACE_EVENT_CLONE      3   // clone 事件
#define PTRACE_EVENT_EXEC       4   // exec 事件
#define PTRACE_EVENT_VFORK_DONE 5   // vfork 完成
#define PTRACE_EVENT_EXIT       6   // exit 事件
#define PTRACE_EVENT_SECCOMP    7   // seccomp 事件
#define PTRACE_EVENT_STOP       128 // 停止事件（非选项控制的事件）

// 系统调用停止消息
#define PTRACE_EVENTMSG_SYSCALL_ENTRY  1
#define PTRACE_EVENTMSG_SYSCALL_EXIT   2
```

## 4. GDB 调试流程 — 整体架构

GDB 通过 ptrace 实现调试的核心流程分为以下阶段：

```
┌─────────────────────────────────────────────────────────────────┐
│                  GDB 调试总体流程                                 │
│                                                                 │
│  1. 启动/附加阶段                                                 │
│     ├── GDB 创建/附加到被调试进程                                   │
│     ├── 建立 ptrace 追踪关系                                      │
│     └── 被调试进程停止在入口或断点处                                │
│                                                                 │
│  2. 调试循环（GDB 主循环）                                        │
│     ├── 等待被调试进程停止（wait4）                                │
│     ├── 检查停止原因（信号/断点/单步/系统调用）                     │
│     ├── 与用户交互（显示源代码、变量等）                            │
│     ├── 执行用户命令（设置断点、读写内存、读写寄存器等）             │
│     └── 恢复被调试进程执行（ptrace_resume）                        │
│                                                                 │
│  3. 分离阶段                                                     │
│     └── 解除追踪关系，恢复被调试进程正常执行                        │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 5. GDB 调试流程 — 详细交互分析

### 5.1 启动调试：GDB 直接启动被调试进程

#### 5.1.1 流程总览

```
GDB (追踪器)                        被调试进程 (被追踪者)              内核
    │                                      │                         │
    │ fork()                                │                         │
    ├─────────────────────────────────────► │                         │
    │                                      │                         │
    │          ptrace(PTRACE_TRACEME)       │                         │
    │ ◄─────────────────────────────────────┤                         │
    │                                      │ 设置 current->ptrace    │
    │                                      │ = PT_PTRACED            │
    │                                      │ child->parent =         │
    │                                      │ current->real_parent    │
    │                                      │ (即 GDB)                │
    │                                      │                         │
    │          execve(被调试程序)           │                         │
    │ ◄─────────────────────────────────────┤                         │
    │                                      │  exec 前发送 SIGTRAP    │
    │                                      │ ──► ptrace_stop()       │
    │                                      │ ──► 进入 TASK_TRACED    │
    │                                      │                          │
    │ wait4(SIGTRAP)                       │                          │
    │ ◄────────────────────────────────────┤  wait_task_stopped()     │
    │                                      │ 读取 p->exit_code       │
    │                                      │ 返回停止信息            │
    │                                      │                          │
    │ 设置断点、读取信息...                │                          │
    │                                      │                          │
    │ ptrace(PTRACE_CONT, ...)              │                          │
    ├─────────────────────────────────────► │  ptrace_resume()        │
    │                                      │  child->exit_code = 0   │
    │                                      │  wake_up_state()        │
    │                                      │ ──► 继续执行            │
```

#### 5.1.2 核心代码分析

**GDB fork 子进程并调用 TRACEME：**

```c
// 用户态 GDB 伪代码
pid_t pid = fork();
if (pid == 0) {
    // 子进程：被调试进程
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    // 当前进程标记为 PT_PTRACED，parent 指向 GDB
    execve("./a.out", argv, envp);
    // exec 触发内核发送 SIGTRAP
}
```

**内核 ptrace_traceme 实现：**

```c
// kernel/ptrace.c:487
static int ptrace_traceme(void)
{
    int ret = -EPERM;

    write_lock_irq(&tasklist_lock);
    /* 是否已经被追踪？ */
    if (!current->ptrace) {
        ret = security_ptrace_traceme(current->parent);
        /*
         * 检查 PF_EXITING 确保 real_parent 没有已经调用 exit_ptrace()
         */
        if (!ret && !(current->real_parent->flags & PF_EXITING)) {
            current->ptrace = PT_PTRACED;
            ptrace_link(current, current->real_parent);
            // 将 current 加入父进程的 ptraced 链表
            // current->parent = current->real_parent（即 GDB）
        }
    }
    write_unlock_irq(&tasklist_lock);

    return ret;
}
```

**exec 时发送 SIGTRAP：**

```c
// kernel/ptrace.h:148
static inline void ptrace_event(int event, unsigned long message)
{
    if (unlikely(ptrace_event_enabled(current, event))) {
        ptrace_notify((event << 8) | SIGTRAP, message);
    } else if (event == PTRACE_EVENT_EXEC) {
        /* 传统 EXEC 报告通过 SIGTRAP */
        if ((current->ptrace & (PT_PTRACED|PT_SEIZED)) == PT_PTRACED)
            send_sig(SIGTRAP, current, 0);
    }
}
```

### 5.2 动态附加：GDB 附加到运行中的进程

#### 5.2.1 流程总览

```
GDB (追踪器)                        被调试进程 (被追踪者)              内核
    │                                      │                         │
    │ ptrace(PTRACE_ATTACH, pid)            │                         │
    ├───────────────────────────────────────┤────── ptrace_attach() ──►│
    │                                      │                         │
    │                                      │ 1. find_get_task_by_vpid│
    │                                      │ 2. __ptrace_may_access  │
    │                                      │ 3. ptrace_link          │
    │                                      │    child->parent = GDB  │
    │                                      │    child->ptrace |=     │
    │                                      │      PT_PTRACED         │
    │                                      │ 4. ptrace_set_stopped   │
    │                                      │    发送 SIGSTOP         │
    │                                      │                         │
    │                                      │ 收到 SIGSTOP            │
    │                                      │ get_signal()            │
    │                                      │ ──► ptrace_stop()       │
    │                                      │ ──► TASK_TRACED         │
    │                                      │                          │
    │ wait4(pid, &status, 0)              │                          │
    │ ◄────────────────────────────────────┤  ptrace_do_wait()       │
    │ status = SIGSTOP                     │  wait_task_stopped()    │
    │                                      │                          │
    │ 读取寄存器、内存、设置断点...        │                          │
    │                                      │                          │
    │ ptrace(PTRACE_CONT, ...)              │                          │
    ├─────────────────────────────────────► │  ptrace_resume()        │
    │                                      │  child->exit_code = 0   │
    │                                      │  wake_up_state()        │
    │                                      │ ──► 继续执行            │
```

#### 5.2.2 核心代码分析

**ptrace_attach 实现：**

```c
// kernel/ptrace.c:409
static int ptrace_attach(struct task_struct *task, long request,
                         unsigned long addr, unsigned long flags)
{
    bool seize = (request == PTRACE_SEIZE);
    int retval;

    if (seize) {
        // PTRACE_SEIZE 验证 flags 参数
        if (addr != 0)
            return -EIO;
        if (flags & ~(unsigned long)PTRACE_O_MASK)
            return -EIO;
        retval = check_ptrace_options(flags);
        if (retval)
            return retval;
        flags = PT_PTRACED | PT_SEIZED | (flags << PT_OPT_FLAG_SHIFT);
    } else {
        flags = PT_PTRACED;
    }

    audit_ptrace(task);

    if (unlikely(task->flags & PF_KTHREAD))
        return -EPERM;
    if (same_thread_group(task, current))
        return -EPERM;

    // 权限检查
    scoped_guard (task_lock, task) {
        retval = __ptrace_may_access(task, PTRACE_MODE_ATTACH_REALCREDS);
        if (retval)
            return retval;
    }

    // 建立追踪关系
    scoped_guard (write_lock_irq, &tasklist_lock) {
        if (unlikely(task->exit_state))
            return -EPERM;
        if (task->ptrace)
            return -EPERM;  // 已被其他追踪器附加

        task->ptrace = flags;
        ptrace_link(task, current);  // task->parent = current
        ptrace_set_stopped(task, seize);  // ATTACH 发送 SIGSTOP
    }

    // 等待 JOBCTL_TRAPPING 清除，确保进程进入 TASK_TRACED
    wait_on_bit(&task->jobctl, JOBCTL_TRAPPING_BIT, TASK_KILLABLE);
    proc_ptrace_connector(task, PTRACE_ATTACH);

    return 0;
}
```

**ptrace_link 建立追踪关系：**

```c
// kernel/ptrace.c:69
void __ptrace_link(struct task_struct *child, struct task_struct *new_parent,
                   const struct cred *ptracer_cred)
{
    BUG_ON(!list_empty(&child->ptrace_entry));
    list_add(&child->ptrace_entry, &new_parent->ptraced);
    child->parent = new_parent;  // 将 parent 指向追踪器
    child->ptracer_cred = get_cred(ptracer_cred);
}
```

**PTRACE_SEIZE 与 PTRACE_ATTACH 的区别：**

`PTRACE_SEIZE` 是 `PTRACE_ATTACH` 的现代化替代方案：
- `ATTACH` 会发送 SIGSTOP 停止目标进程，产生标准的停止信号
- `SEIZE` 不会发送 SIGSTOP，而是设置 `JOBCTL_TRAP_STOP`，采用"软停止"方式
- `SEIZE` 支持 `PTRACE_INTERRUPT` 和 `PTRACE_LISTEN` 操作
- `SEIZE` 会设置 `PT_SEIZED` 标志，启用新行为

### 5.3 断点设置

#### 5.3.1 软件断点（Software Breakpoint）

GDB 通过替换指令来设置软件断点。在 ARM64 上，断点指令是 `BRK #0`（编码为 0xd4200000）。

**流程：**

```
GDB                                    被调试进程                    内核
  │                                        │                         │
  │ 读取目标地址指令                                          │
  │ ptrace(PTRACE_PEEKDATA, addr)          │                         │
  ├───────────────────────────────────────►│  ptrace_access_vm()     │
  │ ◄── 返回原始指令                       │                         │
  │                                        │                         │
  │ 保存原始指令到 GDB 内部                │                         │
  │                                        │                         │
  │ 写入断点指令 (BRK #0)                                         │
  │ ptrace(PTRACE_POKEDATA, addr, BRK)     │                         │
  ├───────────────────────────────────────►│  ptrace_access_vm()     │
  │                                        │  FOLL_FORCE|FOLL_WRITE  │
  │                                        │  写入断点指令           │
```

**内核实现：**

```c
// kernel/ptrace.c:1436
int generic_ptrace_pokedata(struct task_struct *tsk, unsigned long addr,
                            unsigned long data)
{
    int copied;

    copied = ptrace_access_vm(tsk, addr, &data, sizeof(data),
            FOLL_FORCE | FOLL_WRITE);
    return (copied == sizeof(data)) ? 0 : -EIO;
}

// kernel/ptrace.c:44
int ptrace_access_vm(struct task_struct *tsk, unsigned long addr,
                     void *buf, int len, unsigned int gup_flags)
{
    struct mm_struct *mm;
    int ret;

    mm = get_task_mm(tsk);
    if (!mm)
        return 0;

    if (!tsk->ptrace ||
        (current != tsk->parent) ||
        ((get_dumpable(mm) != SUID_DUMP_USER) &&
         !ptracer_capable(tsk, mm->user_ns))) {
        mmput(mm);
        return 0;
    }

    ret = access_remote_vm(mm, addr, buf, len, gup_flags);
    mmput(mm);
    return ret;
}
```

#### 5.3.2 硬件断点（Hardware Breakpoint）

ARM64 架构通过调试寄存器实现硬件断点。GDB 使用 `PTRACE_SETREGSET` 操作设置硬件断点。

**ARM64 硬件断点相关代码：**

```c
// arch/arm64/kernel/ptrace.c:285
static struct perf_event *ptrace_hbp_create(unsigned int note_type,
                                            struct task_struct *tsk,
                                            unsigned long idx)
{
    struct perf_event *bp;
    struct perf_event_attr attr;
    int err, type;

    switch (note_type) {
    case NT_ARM_HW_BREAK:
        type = HW_BREAKPOINT_X;  // 执行断点
        break;
    case NT_ARM_HW_WATCH:
        type = HW_BREAKPOINT_RW;  // 读写监视点
        break;
    default:
        return ERR_PTR(-EINVAL);
    }

    ptrace_breakpoint_init(&attr);
    attr.bp_addr  = 0;
    attr.bp_len   = HW_BREAKPOINT_LEN_4;
    attr.bp_type  = type;
    attr.disabled = 1;

    bp = register_user_hw_breakpoint(&attr, ptrace_hbptriggered, NULL, tsk);
    // ... 注册硬件断点，触发时调用 ptrace_hbptriggered
}
```

ARM64 的硬件断点限制：
- 最多 6 个指令断点（BRP，Breakpoint Register Pairs）
- 最多 4 个数据监视点（WRP，Watchpoint Register Pairs）
- 断点命中时触发 `ptrace_hbptriggered`，发送 `SIGTRAP` 信号

#### 5.3.3 软件断点详细实现

##### 5.3.3.1 完整执行流程 — 从设置到命中到恢复

软件断点的完整生命周期包括以下步骤：

```
┌─────────────────────────────────────────────────────────────────────┐
│              软件断点完整生命周期                                       │
│                                                                     │
│  1. 保存原始指令                                                      │
│     GDB 通过 PTRACE_PEEKDATA 读取目标地址的原始指令                      │
│     保存到 GDB 内部的断点链表中                                        │
│                                                                     │
│  2. 写入断点指令                                                      │
│     GDB 通过 PTRACE_POKEDATA 写入 BRK #0 (0xd4200000)                 │
│     └── 内核 generic_ptrace_pokedata() → ptrace_access_vm()           │
│         → access_remote_vm() 使用 FOLL_FORCE|FOLL_WRITE               │
│                                                                     │
│  3. 恢复被调试进程执行                                                  │
│     GDB 调用 PTRACE_CONT 或 PTRACE_SINGLESTEP                         │
│                                                                     │
│  4. 执行命中断点                                                      │
│     CPU 执行到 BRK #0 指令                                            │
│     └── 触发 BRK 异常 → 进入 EL1 异常向量                               │
│                                                                     │
│  5. 内核异常处理                                                      │
│     ESR_ELx_EC_BRK64(0x3C) 异常类                                    │
│     └── el0_brk64() → do_el0_brk64() → send_user_sigtrap(TRAP_BRKPT) │
│                                                                     │
│  6. 信号传递                                                          │
│     arm64_force_sig_fault(SIGTRAP, TRAP_BRKPT, pc)                    │
│     └── 信号挂起到被调试进程的 signal_pending()                         │
│                                                                     │
│  7. ptrace_stop                                                      │
│     get_signal() → ptrace_signal() → ptrace_stop()                    │
│     └── 进程进入 TASK_TRACED 状态                                     │
│     └── 发送 SIGCHLD 通知 GDB                                         │
│                                                                     │
│  8. GDB 处理                                                          │
│     wait4() 收到 SIGCHLD → 读取 exit_code = SIGTRAP                   │
│     └── 检查 si_code = TRAP_BRKPT 确认是断点命中                        │
│     └── 读取 PC 寄存器确认断点位置                                      │
│                                                                     │
│  9. 恢复原始指令并单步跳过                                              │
│     GDB 通过 PTRACE_POKEDATA 恢复原始指令                              │
│     └── 回退 PC 到断点地址（ARM64 不需要回退，见 5.3.3.4）               │
│     └── 使用 PTRACE_SINGLESTEP 单步执行原始指令                         │
│                                                                     │
│ 10. 重新插入断点                                                      │
│     单步完成后，GDB 再次通过 PTRACE_POKEDATA 写入 BRK #0               │
│     然后恢复执行（PTRACE_CONT）                                        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**GDB 断点命中处理伪代码（简化）：**

```c
// 用户态 GDB 断点命中处理逻辑（简化伪代码）
void handle_breakpoint_hit(pid_t pid, unsigned long bp_addr,
                           unsigned long orig_instr) {
    // 1. 恢复原始指令
    ptrace(PTRACE_POKEDATA, pid, bp_addr, orig_instr);

    // 2. 回退 PC（x86 需要，ARM64 不需要）
    // ARM64: BRK 异常时 PC 停在 BRK 指令上，不需要回退
    // x86: INT3 后 RIP 指向下一条指令，需要 PC -= 1

    // 3. 单步执行原始指令
    ptrace(PTRACE_SINGLESTEP, pid, 0, 0);
    waitpid(pid, &status, 0);

    // 4. 重新插入断点
    ptrace(PTRACE_POKEDATA, pid, bp_addr, ARM64_BRK_INSTR);

    // 5. 恢复执行
    ptrace(PTRACE_CONT, pid, 0, 0);
}
```

##### 5.3.3.2 ARM64 BRK 指令编码细节

**BRK 指令编码格式：**

```
31  30  29 28  27  26  25  24  23  22  21  20        5  4   0
┌────┬───┬───┬───┬────┬───┬───┬───┬───┬───┬──────────┬──────┐
│ 1  1  0  1  0  1  0  0  0  0  1     imm16         0  0  0  0 0
└────┴───┴───┴───┴────┴───┴───┴───┴───┴───┴──────────┴──────┘
  └─ 固定位 ─┘  └─ BRK ─┘    └─ 立即数 ─┘    └─ 固定位 ─┘
```

- **固定操作码**：`0xd4200000`（`AARCH64_BREAK_MON`，定义于 `arch/arm64/include/asm/insn-def.h:15`）
- **立即数字段（imm16）**：bits[20:5]，共 16 位
- `BRK #0` 指令编码：`0xd4200000`（imm16 = 0）

**内核中 BRK 立即数的分配：**

```c
// arch/arm64/include/asm/brk-imm.h
#define KPROBES_BRK_IMM         0x004   // kprobes 断点
#define UPROBES_BRK_IMM         0x005   // uprobes 断点
#define KPROBES_BRK_SS_IMM      0x006   // kprobes 单步
#define KRETPROBES_BRK_IMM      0x007   // kretprobes 断点
#define FAULT_BRK_IMM           0x100   // 故意触发故障
#define KGDB_DYN_DBG_BRK_IMM    0x400   // kgdb 动态调试
#define KGDB_COMPILED_DBG_BRK_IMM 0x401 // kgdb 编译调试
#define BUG_BRK_IMM             0x800   // BUG/WARN 宏
#define KASAN_BRK_IMM           0x900   // KASAN
#define UBSAN_BRK_IMM           0x5500  // UBSAN
```

**BRK 指令的异常向量处理路径：**

用户态执行 `BRK #0` 时，CPU 触发同步异常，异常类为 `ESR_ELx_EC_BRK64`（`0x3C`），完整处理路径：

```
用户态执行 BRK #0
    │
    ▼  CPU 异常捕获
    └── ESR_EL1.EC = 0x3C (BRK64)
    │
    ▼  el0t_64_sync_handler          // arch/arm64/kernel/entry-common.c:737
    └── case ESR_ELx_EC_BRK64:
        └── el0_brk64(regs, esr)     // arch/arm64/kernel/entry-common.c:710
            ├── arm64_enter_from_user_mode(regs)
            ├── local_daif_restore(DAIF_PROCCTX)
            └── do_el0_brk64(esr, regs)  // arch/arm64/kernel/debug-monitors.c:254
                │
                ├── [CONFIG_UPROBES] 检查 UPROBES_BRK_IMM
                │   └── uprobe_brk_handler() 处理 uprobe 断点
                │
                └── send_user_sigtrap(TRAP_BRKPT)
                    // arch/arm64/kernel/debug-monitors.c:163
                    └── arm64_force_sig_fault(SIGTRAP, TRAP_BRKPT,
                                              instruction_pointer(regs))
```

**内核态 BRK 处理路径**（`el1_brk64`）：

```c
// arch/arm64/kernel/debug-monitors.c:264
void do_el1_brk64(unsigned long esr, struct pt_regs *regs)
{
    if (call_el1_break_hook(regs, esr) == DBG_HOOK_HANDLED)
        return;
    die("Oops - BRK", regs, esr);
}
```

`call_el1_break_hook` 按优先级检查各 BRK 立即数，依次尝试 BUG、CFI、KASAN、KGDB、kprobes、kretprobes 等处理函数。如果所有 hook 都未处理，则调用 `die()` 触发内核 panic。

##### 5.3.3.3 x86 INT3 对比

| 特性 | ARM64 BRK | x86 INT3 |
|------|-----------|----------|
| **指令编码** | `0xd4200000`（4 字节） | `0xCC`（1 字节） |
| **指令长度** | 固定 4 字节 | 固定 1 字节 |
| **异常向量** | `ESR_ELx_EC_BRK64` (0x3C) | `#BP` (Int3, 向量 3) |
| **异常处理** | `do_el0_brk64()` → `send_user_sigtrap()` | `do_int3()` → `force_sig_fault(SIGTRAP)` |
| **信号** | `SIGTRAP` + `TRAP_BRKPT` | `SIGTRAP` + `TRAP_BRKPT` |

**指令长度差异对并发安全的影响：**

- **x86 INT3（1 字节）**：替换单字节指令具有天然的原子性。在多线程环境中，如果一个线程正在取指执行而另一个线程正在写入 INT3，由于 INT3 只有 1 字节，与目标指令的第一个字节重叠的概率极高，但即便发生部分写入，INT3 仍能被正确解码执行。
- **ARM64 BRK（4 字节）**：ARM64 指令固定为 4 字节，且 ARM64 架构要求指令访问必须是 4 字节对齐的。在 64KB 页对齐的地址上，BRK 替换是原子的。但在多核系统中，如果一个核心正在执行 4 字节指令，另一个核心正在写入 BRK，可能出现部分更新的情况。ARM64 的 Break-Before-Make（BBM）机制要求：修改指令时必须先写入一个断点指令（如 BRK），ISB 同步，再写入新指令，以确保多核观察一致性。

**指令长度对 PC 回退的影响：**

- **x86**：`INT3` 执行后，RIP 指向 INT3 指令的**下一条指令**。因此 GDB 需要将 RIP 回退 1 字节，以重新执行被替换的原始指令。
- **ARM64**：`BRK #0` 执行后，异常处理时 `instruction_pointer(regs)` 返回的是 BRK 指令本身的地址。ARM64 架构中，BRK 异常属于精确异常，`regs->pc` 在异常入口处保存为 BRK 指令地址。因此 GDB 不需要回退 PC，可以直接在断点地址处恢复原始指令并单步执行。

##### 5.3.3.4 断点命中后的 PC 处理

**ARM64 BRK 异常的 PC 行为：**

ARM64 架构中，BRK 指令执行时：

1. CPU 将 BRK 指令的地址（`PC`）保存到 `ELR_EL1`（Exception Link Register）
2. 进入异常向量后，`struct pt_regs` 中的 `pc` 字段设置为 `ELR_EL1` 的值
3. 因此 `regs->pc` 指向 BRK 指令本身，**不是 BRK 的下一条指令**

```c
// arch/arm64/kernel/debug-monitors.c:163
static void send_user_sigtrap(int si_code)
{
    struct pt_regs *regs = current_pt_regs();
    // ...
    arm64_force_sig_fault(SIGTRAP, si_code, instruction_pointer(regs),
                          NULL);
    // instruction_pointer(regs) 返回 regs->pc
    // 对于 BRK 异常，regs->pc == BRK 指令地址（不需要回退）
}
```

**GDB 的 PC 恢复策略：**

GDB 在断点命中后执行以下操作恢复执行：

```
1. 断点命中，PC 停在 BRK 指令处
2. GDB 恢复原始指令到断点地址
3. 设置 PC 为断点地址（ARM64 不需要，因为 PC 已经指向断点地址）
4. 单步执行一条指令
5. 重新插入 BRK 断点
6. 恢复执行
```

**x86 与 ARM64 对比：**

| 架构 | 异常后 PC 位置 | 需要回退？ | 回退量 |
|------|---------------|-----------|--------|
| ARM64 | 停在 BRK 指令 | 否 | 0 |
| x86 | 指向 INT3 之后 | 是 | 1 字节 |

##### 5.3.3.5 多线程断点处理

**所有线程停止机制：**

当多线程程序中的某个线程命中断点时，GDB 需要暂停所有线程以保持调试状态的一致性：

1. **断点命中**：线程 A 命中断点，进入 `ptrace_stop()`，状态变为 `TASK_TRACED`
2. **GDB 收到通知**：`wait4()` 返回线程 A 的停止信息
3. **GDB 停止其他线程**：GDB 遍历进程组中的所有线程，对每个线程调用 `ptrace(PTRACE_INTERRUPT, tid)` 或 `ptrace(PTRACE_ATTACH, tid)`
4. **内核处理 INTERRUPT**：`ptrace_interrupt()` 设置 `JOBCTL_TRAP_NOTIFY`，发送 `SIGTRAP` 信号
5. **其他线程进入 ptrace_stop**：每个线程在 `get_signal()` 中检测到 `JOBCTL_TRAP_NOTIFY`，进入 `ptrace_stop()`
6. **所有线程停止**：GDB 确认所有线程都已进入 `TASK_TRACED` 状态

**断点插入/删除时的线程同步：**

在多线程环境中插入或删除软件断点时，GDB 需要确保线程安全：

1. 暂停所有线程（通过 `PTRACE_INTERRUPT` 或 `SIGSTOP`）
2. 在目标地址写入或恢复指令
3. 恢复所有线程执行

这样做是为了避免在某个线程正在执行目标地址处的指令时，另一个线程修改了该地址的指令，导致不可预测的行为。

**内核中的线程遍历：**

```c
// kernel/ptrace.c:130
int ptrace_check_attach(struct task_struct *child, bool ignore_state)
{
    // 检查被追踪进程是否处于 TASK_TRACED 状态
    // 冻结进程以确保操作安全
    if (!ignore_state && !ptrace_freeze_traced(child))
        return -ESRCH;
    return 0;
}
```

GDB 使用 `PTRACE_GETEVENTMSG` 配合 `PTRACE_EVENT_STOP` 来确认线程停止的原因。

### 5.3.4 监视点（Watchpoint）

##### 5.3.4.1 监视点概念

**监视点 vs 代码断点：**

| 特性 | 代码断点（Breakpoint） | 监视点（Watchpoint） |
|------|----------------------|---------------------|
| **触发条件** | 执行到指定地址的指令 | 访问（读/写）指定内存地址 |
| **监控对象** | 指令执行流 | 数据访问 |
| **实现方式** | 软件（指令替换）或硬件 | 通常为硬件（调试寄存器） |
| **典型用途** | 停在某行代码 | 监控变量值的变化 |

**监视点类型：**

- **读监视点（Read Watchpoint）**：当指定内存地址被读取时触发
- **写监视点（Write Watchpoint）**：当指定内存地址被写入时触发
- **读写监视点（Read/Write Watchpoint）**：读取或写入均触发

在 ARM64 中，这些类型通过 `DBGWCR` 寄存器的 LSC（Load/Store/Compute）字段控制：

```c
// arch/arm64/include/asm/hw_breakpoint.h
#define ARM_BREAKPOINT_LOAD    1   // 读监视点
#define ARM_BREAKPOINT_STORE   2   // 写监视点
// ARM_BREAKPOINT_LOAD | ARM_BREAKPOINT_STORE = 3 读写监视点
```

**硬件监视点与软件模拟监视点的区别：**

- **硬件监视点**：利用 CPU 调试寄存器，无性能开销（仅在命中时触发异常），但有数量限制
- **软件模拟监视点**：通过单步执行 + 指令模拟实现，每次访问都会触发单步异常，性能开销极大

##### 5.3.4.2 ARM64 硬件监视点实现

**ARM64 调试寄存器体系：**

ARM64 架构提供两组调试寄存器用于监视点：

| 寄存器 | 全称 | 功能 |
|--------|------|------|
| `DBGWVRn_EL1` | Watchpoint Value Register n | 存储监视地址 |
| `DBGWCRn_EL1` | Watchpoint Control Register n | 控制监视行为 |

**DBGWCR 控制寄存器编码：**

```
DBGWCRn_EL1 寄存器位域：
┌───────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐
│ 31:29 │ 28:24  │ 23:21  │ 20:19  │ 18:16  │ 14:13  │ 12:5   │  4:3   │ 2:1  │ 0 │
├───────┼────────┼────────┼────────┼────────┼────────┼────────┼────────┼──────┤─────┤
│  Mask  │  RES   │  MSC   │  RES   │  BAS   │  RES   │  LSC   │  RES   │ PAC │ E  │
└───────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┴──────┴─────┘
  └─ 掩码 ┘          └─ 监控 ┘        └─ 字节选 ┘   └─ 访问类型 ┘        └─ 特权 ┘└─ 启用
```

- **E（bit 0）**：Enable，监视点启用
- **PAC（bits 2:1）**：Privilege Access Control，权限控制（EL0/EL1）
- **LSC（bits 14:13）**：Load/Store/Compute，访问类型
  - `00` — 保留
  - `01` — 读（Load）
  - `10` — 写（Store）
  - `11` — 读写（Load/Store）
- **BAS（bits 18:16）**：Byte Address Select，字节选择（标记哪些字节被监视）
- **MSC（bits 23:21）**：Linked Breakpoint Number，链接的断点号
- **Mask（bits 31:29）**：地址掩码，用于监视地址范围

**内核中硬件监视点控制结构体：**

```c
// arch/arm64/include/asm/hw_breakpoint.h:13
struct arch_hw_breakpoint_ctrl {
    u32 __reserved  : 19,
    len             : 8,    // 长度/字节选择 (对应 BAS)
    type            : 2,    // 类型 (LSC: Load/Store)
    privilege       : 2,    // 特权级 (PAC)
    enabled         : 1;    // 启用 (E)
};

// arch/arm64/include/asm/hw_breakpoint.h:21
struct arch_hw_breakpoint {
    u64 address;                    // 监视地址 (DBGWVR)
    u64 trigger;                    // 触发时的实际访问地址
    struct arch_hw_breakpoint_ctrl ctrl;  // 控制 (DBGWCR)
};
```

**编码/解码函数：**

```c
// arch/arm64/include/asm/hw_breakpoint.h:33
static inline u32 encode_ctrl_reg(struct arch_hw_breakpoint_ctrl ctrl)
{
    u32 val = (ctrl.len << 5) | (ctrl.type << 3) |
              (ctrl.privilege << 1) | ctrl.enabled;
    if (is_kernel_in_hyp_mode() && ctrl.privilege == AARCH64_BREAKPOINT_EL1)
        val |= DBG_HMC_HYP;
    return val;
}

// arch/arm64/include/asm/hw_breakpoint.h:44
static inline void decode_ctrl_reg(u32 reg,
                                   struct arch_hw_breakpoint_ctrl *ctrl)
{
    ctrl->enabled   = reg & 0x1;
    reg >>= 1;
    ctrl->privilege = reg & 0x3;
    reg >>= 2;
    ctrl->type      = reg & 0x3;
    reg >>= 2;
    ctrl->len       = reg & 0xff;
}
```

**架构限制：**

- 最多监视点数量：通过 `ID_AA64DFR0_EL1` 寄存器的 `WRPs` 字段读取
- 实际可用数量：`get_num_wrps()` 返回 `1 + ID_AA64DFR0_EL1.WRPs`
- 常见实现：Cortex-A 系列通常提供 4 个监视点寄存器

```c
// arch/arm64/include/asm/hw_breakpoint.h:139
static inline int get_num_wrps(void)
{
    u64 dfr0 = read_sanitised_ftr_reg(SYS_ID_AA64DFR0_EL1);
    return 1 + cpuid_feature_extract_unsigned_field(dfr0,
                    ID_AA64DFR0_EL1_WRPs_SHIFT);
}
```

**对齐要求：**

监视地址必须与监视长度对齐。例如，如果监视 4 字节，地址必须 4 字节对齐。ARM64 硬件不支持不对齐的监视点。内核中通过 `arch_bp_generic_fields()` 进行转换：

```c
// arch/arm64/kernel/hw_breakpoint.c:353
int arch_bp_generic_fields(struct arch_hw_breakpoint_ctrl ctrl,
                           int *gen_len, int *gen_type, int *offset)
{
    // Type 转换
    switch (ctrl.type) {
    case ARM_BREAKPOINT_EXECUTE: *gen_type = HW_BREAKPOINT_X; break;
    case ARM_BREAKPOINT_LOAD:    *gen_type = HW_BREAKPOINT_R; break;
    case ARM_BREAKPOINT_STORE:   *gen_type = HW_BREAKPOINT_W; break;
    case ARM_BREAKPOINT_LOAD | ARM_BREAKPOINT_STORE:
                                 *gen_type = HW_BREAKPOINT_RW; break;
    default: return -EINVAL;
    }
    // Len 转换（从 BAS 编码到通用长度编码）
    *offset = __ffs(ctrl.len);
    // ...
}
```

##### 5.3.4.3 通过 PTRACE_SETREGSET 设置监视点

GDB 通过 `PTRACE_SETREGSET` 操作，配合 `NT_ARM_HW_WATCH` note 类型来设置硬件监视点。

**完整流程：**

```
GDB                                         内核
  │                                           │
  │ struct user_hwdebug_state {                │
  │     struct note_hdr {                      │
  │         .n_type = NT_ARM_HW_WATCH          │
  │     };                                     │
  │     struct dbg_reg {                       │
  │         .addr = 目标地址      // DBGWVR    │
  │         .ctrl = 控制值        // DBGWCR    │
  │     };                                     │
  │ }                                          │
  │                                           │
  │ ptrace(PTRACE_SETREGSET, pid,              │
  │        NT_ARM_HW_WATCH, &iov)              │
  ├──────────────────────────────────────────► │
  │                                           │
  │ ptrace_regset()                            │
  │  └── copy_regset_from_user()              │
  │      └── hw_break_set()                   │
  │          └── ptrace_hbp_setup()           │
  │              ├── ptrace_hbp_get_initialised_bp()
  │              │   └── ptrace_hbp_create()  │
  │              │       ├── ptrace_breakpoint_init(&attr)
  │              │       ├── attr.bp_addr = 0 │
  │              │       ├── attr.bp_len = HW_BREAKPOINT_LEN_4
  │              │       ├── attr.bp_type = HW_BREAKPOINT_RW
  │              │       ├── attr.disabled = 1│
  │              │       └── register_user_hw_breakpoint(&attr,
  │              │               ptrace_hbptriggered, NULL, tsk)
  │              │               └── perf_event_create_kernel_counter()
  │              │                   └── 创建 perf_event 并注册到框架
  │              │
  │              └── ptrace_hbp_fill_attr_ctrl()
  │                  └── arch_bp_generic_fields()  // 转换编码
  │                  └── modify_user_hw_breakpoint()
  │                      └── hw_breakpoint_arch_parse()
  │                          └── arch_build_bp_info()  // 验证并设置地址
  │                              ├── 检查地址对齐
  │                              ├── 检查长度有效性
  │                              └── 设置 hw.address, hw.ctrl
  │                                           │
  │ 返回 0                                     │
  │ ◄──────────────────────────────────────────┤
```

**ptrace_hbp_create 核心代码：**

```c
// arch/arm64/kernel/ptrace.c:285
static struct perf_event *ptrace_hbp_create(unsigned int note_type,
                                            struct task_struct *tsk,
                                            unsigned long idx)
{
    struct perf_event *bp;
    struct perf_event_attr attr;
    int err, type;

    switch (note_type) {
    case NT_ARM_HW_BREAK:
        type = HW_BREAKPOINT_X;       // 执行断点
        break;
    case NT_ARM_HW_WATCH:
        type = HW_BREAKPOINT_RW;      // 读写监视点
        break;
    default:
        return ERR_PTR(-EINVAL);
    }

    ptrace_breakpoint_init(&attr);
    attr.bp_addr  = 0;
    attr.bp_len   = HW_BREAKPOINT_LEN_4;
    attr.bp_type  = type;
    attr.disabled = 1;

    // 注册用户态硬件断点，触发时调用 ptrace_hbptriggered
    bp = register_user_hw_breakpoint(&attr, ptrace_hbptriggered, NULL, tsk);
    // ...
}
```

**register_user_hw_breakpoint 注册过程：**

```c
// kernel/events/hw_breakpoint.c:741
struct perf_event *
register_user_hw_breakpoint(struct perf_event_attr *attr,
                            perf_overflow_handler_t triggered,
                            void *context,
                            struct task_struct *tsk)
{
    // 通过 perf_event 框架创建内核计数器
    // attr 中的 bp_addr, bp_len, bp_type 将被传递给架构特定代码
    // 最终由 arch_install_hw_breakpoint() 写入调试寄存器
    return perf_event_create_kernel_counter(attr, -1, tsk,
                                            triggered, context);
}
```

**perf_event 框架的介入：**

硬件断点和监视点通过 perf_event 子系统管理。`register_user_hw_breakpoint()` 创建了一个 perf_event，该 event 的 `overflow_handler` 被设置为 `ptrace_hbptriggered`。当监视点命中时：

1. CPU 触发 Watchpoint 异常
2. `do_watchpoint()` 处理异常
3. 调用 `perf_bp_event(wp, regs)` 触发 perf event
4. perf event 调用 `ptrace_hbptriggered()` 回调
5. `ptrace_hbptriggered()` 发送 `SIGTRAP` 信号

**线程数据结构中的监视点存储：**

```c
// arch/arm64/include/asm/processor.h:108
struct debug_info {
    int             suspended_step;     // 是否挂起单步
    int             bps_disabled;       // 断点是否禁用
    int             wps_disabled;       // 监视点是否禁用
    struct perf_event *hbp_break[ARM_MAX_BRP];  // 硬件断点指针数组
    struct perf_event *hbp_watch[ARM_MAX_WRP];  // 硬件监视点指针数组
};

// arch/arm64/include/asm/processor.h:173
struct thread_struct {
    // ...
    struct debug_info    debug;    // 调试信息（含断点和监视点）
    // ...
};
```

##### 5.3.4.4 监视点命中处理流程

**完整流程图：**

```
被调试进程访问被监视的内存地址              GDB (追踪器)              内核
    │                                           │                  │
    │ CPU 执行 Load/Store 指令                                        │
    │ 硬件比较地址与 DBGWVR 匹配                                      │
    │ 且访问类型匹配 LSC 字段                                         │
    │ ──► 触发 Watchpoint 异常                        │                  │
    │      ESR_ELx_EC = WATCHPT_LOW (0x34)           │                  │
    │      FAR_EL1 = 触发访问的地址                   │                  │
    │                                           │                  │
    │ el0_watchpt() 异常处理                        │                  │
    │  └── do_watchpoint(far, esr, regs)            │                  │
    │      │                                        │                  │
    │      ├── 遍历所有监视点寄存器 (wp_on_reg)     │                  │
    │      ├── 读取 DBGWVR, DBGWCR 比较             │                  │
    │      ├── get_distance_from_watchpoint()       │                  │
    │      │   计算地址距离（处理近似匹配）          │                  │
    │      │                                        │                  │
    │      ├── watchpoint_report(wp, addr, regs)    │                  │
    │      │    └── perf_bp_event(wp, regs)         │                  │
    │      │        └── ptrace_hbptriggered()       │                  │
    │      │            └── arm64_force_sig_fault(  │                  │
    │      │                 SIGTRAP, TRAP_HWBKPT,  │                  │
    │      │                 bkpt->trigger)         │                  │
    │      │                                        │                  │
    │      ├── 禁用 EL0 监视点                       │                  │
    │      │   toggle_bp_registers(WCR, EL0, 0)     │                  │
    │      │                                        │                  │
    │      └── 启用单步                              │                  │
    │          user_enable_single_step(current)     │                  │
    │          (跳过触发监视点的指令)                │                  │
    │                                           │                  │
    │ 信号处理                                      │                  │
    │ get_signal()                                 │                  │
    │  └── ptrace_signal(SIGTRAP, info, type)      │                  │
    │      └── ptrace_stop(SIGTRAP, CLD_TRAPPED)   │                  │
    │          ├── TASK_TRACED                     │                  │
    │          ├── exit_code = SIGTRAP             │                  │
    │          ├── last_siginfo->si_code =         │                  │
    │          │   TRAP_HWBKPT                     │                  │
    │          │   si_addr = 触发地址              │                  │
    │          ├── do_notify_parent_cldstop()      │                  │
    │          │   └── 发送 SIGCHLD                │                  │
    │          └── schedule()                      │                  │
    │                                           │                  │
    │ wait4() SIGCHLD 到达                        │                  │
    │ ◄───────────────────────────────────────────┤                  │
    │                                           │                  │
    │ GDB 分析：                                    │                  │
    │  ├── WSTOPSIG = SIGTRAP                      │                  │
    │  ├── PTRACE_GETSIGINFO → si_code =           │                  │
    │  │   TRAP_HWBKPT（确认是监视点命中）          │                  │
    │  └── si_addr = 触发地址                      │                  │
    │                                           │                  │
    │ GDB 显示变量变化信息                          │                  │
    │                                           │                  │
    │ ptrace(PTRACE_CONT, ...)                     │                  │
    ├───────────────────────────────────────────► │ ptrace_resume()  │
    │                                           │ 单步完成后恢复     │
    │                                           │ 监视点自动重新启用  │
```

**内核 do_watchpoint 代码路径：**

```c
// arch/arm64/kernel/hw_breakpoint.c:753
void do_watchpoint(unsigned long addr, unsigned long esr, struct pt_regs *regs)
{
    int i, step = 0, *kernel_step, access, closest_match = 0;
    u64 min_dist = -1, dist;
    u32 ctrl_reg;
    u64 val;
    struct perf_event *wp, **slots;
    struct debug_info *debug_info;
    struct arch_hw_breakpoint_ctrl ctrl;

    slots = this_cpu_ptr(wp_on_reg);
    debug_info = &current->thread.debug;

    // 1. 遍历所有监视点寄存器，查找匹配项
    rcu_read_lock();
    for (i = 0; i < core_num_wrps; ++i) {
        wp = slots[i];
        if (wp == NULL)
            continue;

        // 2. 检查访问类型匹配（读/写）
        access = (esr & ESR_ELx_WNR) ? HW_BREAKPOINT_W : HW_BREAKPOINT_R;
        if (!(access & hw_breakpoint_type(wp)))
            continue;

        // 3. 读取 DBGWVR 和 DBGWCR 进行比较
        val = read_wb_reg(AARCH64_DBG_REG_WVR, i);
        ctrl_reg = read_wb_reg(AARCH64_DBG_REG_WCR, i);
        decode_ctrl_reg(ctrl_reg, &ctrl);
        dist = get_distance_from_watchpoint(addr, val, &ctrl);

        if (dist < min_dist) {
            min_dist = dist;
            closest_match = i;
        }
        // 精确匹配
        if (dist != 0)
            continue;
        step = watchpoint_report(wp, addr, regs);
    }

    // 4. 没有精确匹配时使用最近匹配
    if (min_dist > 0 && min_dist != -1)
        step = watchpoint_report(slots[closest_match], addr, regs);
    rcu_read_unlock();

    if (!step)
        return;

    // 5. 禁用 EL0 监视点，启用单步跳过触发指令
    toggle_bp_registers(AARCH64_DBG_REG_WCR, DBG_ACTIVE_EL0, 0);
    if (user_mode(regs)) {
        debug_info->wps_disabled = 1;
        if (debug_info->bps_disabled)
            return;
        if (test_thread_flag(TIF_SINGLESTEP))
            debug_info->suspended_step = 1;
        else
            user_enable_single_step(current);
    }
}
```

**watchpoint_report 回调：**

```c
// arch/arm64/kernel/hw_breakpoint.c:732
static int watchpoint_report(struct perf_event *wp, unsigned long addr,
                             struct pt_regs *regs)
{
    int step = is_default_overflow_handler(wp);
    struct arch_hw_breakpoint *info = counter_arch_bp(wp);

    info->trigger = addr;  // 记录触发地址

    // 如果是用户态监视点从内核态（uaccess）触发，内核自行处理单步
    if (!user_mode(regs) && info->ctrl.privilege == AARCH64_BREAKPOINT_EL0)
        step = 1;
    else
        perf_bp_event(wp, regs);  // 触发 perf event → ptrace_hbptriggered

    return step;
}
```

**ptrace_hbptriggered 发送信号：**

```c
// arch/arm64/kernel/ptrace.c:171
static void ptrace_hbptriggered(struct perf_event *bp,
                                struct perf_sample_data *data,
                                struct pt_regs *regs)
{
    struct arch_hw_breakpoint *bkpt = counter_arch_bp(bp);
    const char *desc = "Hardware breakpoint trap (ptrace)";

    if (is_compat_task()) {
        // AArch32 兼容模式：使用 si_errno 编码断点/监视点索引
        // ...
        arm64_force_sig_ptrace_errno_trap(si_errno, bkpt->trigger, desc);
        return;
    }

    // AArch64 模式：发送 SIGTRAP + TRAP_HWBKPT
    // si_addr = 触发访问的地址
    arm64_force_sig_fault(SIGTRAP, TRAP_HWBKPT, bkpt->trigger, desc);
}
```

**信号信息中的 TRAP_HWBKPT：**

```c
// include/uapi/asm-generic/siginfo.h:261
#define TRAP_BRKPT      1   // 软件断点命中
#define TRAP_TRACE      2   // 单步追踪
#define TRAP_BRANCH     3   // 分支追踪
#define TRAP_HWBKPT     4   // 硬件断点/监视点命中
```

##### 5.3.4.5 x86 调试寄存器对比

**x86 调试寄存器体系：**

x86 架构提供 8 个调试寄存器（DR0-DR7），其中 DR0-DR3 用于断点/监视点地址，DR6 为状态寄存器，DR7 为控制寄存器。

| 寄存器 | 功能 | ARM64 对应 |
|--------|------|-----------|
| **DR0-DR3** | 断点/监视点地址（最多 4 个） | `DBGWVRn_EL1` / `DBGBVRn_EL1` |
| **DR6** | 断点状态寄存器（指示哪个断点触发） | `ESR_EL1`（通过异常类区分） |
| **DR7** | 断点控制寄存器（启用、类型、长度） | `DBGWCRn_EL1` / `DBGBCRn_EL1` |

**x86 DR7 控制寄存器编码：**

```c
// arch/x86/include/uapi/asm/debugreg.h
#define DR_TRAP0        (0x1)         // DR6 bit 0: DR0 命中
#define DR_TRAP1        (0x2)         // DR6 bit 1: DR1 命中
#define DR_TRAP2        (0x4)         // DR6 bit 2: DR2 命中
#define DR_TRAP3        (0x8)         // DR6 bit 3: DR3 命中
#define DR_TRAP_BITS    (DR_TRAP0|DR_TRAP1|DR_TRAP2|DR_TRAP3)
#define DR_STEP         (0x4000)      // DR6 bit 14: 单步完成
#define DR_SWITCH       (0x8000)      // DR6 bit 15: 任务切换

#define DR_RW_EXECUTE   (0x0)         // 执行断点
#define DR_RW_WRITE     (0x1)         // 写监视点
#define DR_RW_READ      (0x3)         // 读/写监视点

#define DR_LEN_1        (0x0)         // 1 字节长度
#define DR_LEN_2        (0x4)         // 2 字节长度
#define DR_LEN_4        (0xC)         // 4 字节长度
#define DR_LEN_8        (0x8)         // 8 字节长度

#define DR_LOCAL_ENABLE  (0x1)        // 本地启用（每个寄存器）
#define DR_GLOBAL_ENABLE (0x2)        // 全局启用（每个寄存器）
#define DR_LOCAL_ENABLE_MASK  (0x55)  // 所有本地启用位
#define DR_GLOBAL_ENABLE_MASK (0xAA) // 所有全局启用位
```

**x86 与 ARM64 监视点机制对比：**

| 特性 | x86 | ARM64 |
|------|-----|-------|
| **地址寄存器** | DR0-DR3（4 个） | `DBGWVR0`-`DBGWVRn`（最多 16 个，常见 4 个） |
| **控制寄存器** | DR7 | `DBGWCR0`-`DBGWCRn` |
| **状态寄存器** | DR6（指示哪个断点触发） | `ESR_EL1` + FAR_EL1（地址和类型） |
| **读监视点** | `DR_RW_READ` (0x3) | `ARM_BREAKPOINT_LOAD` (1) |
| **写监视点** | `DR_RW_WRITE` (0x1) | `ARM_BREAKPOINT_STORE` (2) |
| **读写监视点** | 不支持（DR_RW_READ 实际是读写） | `ARM_BREAKPOINT_LOAD \| ARM_BREAKPOINT_STORE` (3) |
| **执行监视点** | `DR_RW_EXECUTE` (0x0) | 通过硬件断点（BRP）实现 |
| **字节选择** | 通过 LEN 字段（1/2/4/8 字节） | 通过 BAS 字段（按位选择字节） |
| **地址掩码** | 不支持 | 支持（MSC 字段，用于地址范围监视） |
| **数量限制** | 最多 4 个 | 架构支持最多 16 个，实际取决于实现 |
| **异常处理** | `#DB` (向量 1) → `do_debug()` | `WATCHPT_LOW` (EC 0x34) → `do_watchpoint()` |
| **信号** | `SIGTRAP` + `TRAP_HWBKPT` | `SIGTRAP` + `TRAP_HWBKPT` |

##### 5.3.4.6 软件监视点（模拟监视点）

**实现原理：**

当硬件监视点资源不足时，调试器可以退而使用软件模拟监视点。其基本思路是：

1. 在被监视的地址处设置软件断点（如 ARM64 的 `BRK #0` 或 x86 的 `INT3`）
2. 断点命中后，检查当前指令是否为加载/存储指令
3. 如果是且访问地址匹配，则报告监视点命中
4. 否则恢复执行

**通过单步执行实现的监视点：**

另一种更广泛的软件监视点实现方式是利用单步执行：

```
1. 暂停所有线程
2. 清除被监视内存页的访问权限或设置脏页追踪
3. 单步执行每条指令
4. 每条指令执行后检查是否访问了被监视的地址
5. 如果是，报告监视点命中
6. 否则继续执行下一条指令
```

**性能开销分析：**

| 实现方式 | 性能开销 | 适用场景 |
|---------|---------|---------|
| **硬件监视点** | 零开销（仅命中时触发异常） | 少量监视点、性能敏感场景 |
| **软件断点模拟** | 命中时开销大（需反汇编分析） | 只监视写入、少量变量 |
| **单步模拟** | 极大（每指令一次异常） | 无硬件支持时的兜底方案 |

**eBPF 的监视点支持：**

eBPF 通过 `bpf_probe_read()` 等方式实现类似监视点的功能，但并非真正的硬件监视点：

- **kprobe/uprobe**：可以在函数入口/出口处附加 BPF 程序，读取函数参数和返回值
- **tracepoint**：在内核特定事件点附加 BPF 程序
- **perf_event**：可以通过 perf_event_open() 创建硬件断点/监视点事件，并使用 BPF 程序处理

##### 5.3.4.7 监视点限制与最佳实践

**硬件资源限制：**

```c
// arch/arm64/include/asm/hw_breakpoint.h:82
#define ARM_MAX_BRP      16    // 硬件断点最大数量（架构限制）
#define ARM_MAX_WRP      16    // 硬件监视点最大数量（架构限制）

// 实际可用数量取决于 CPU 实现
// 可通过以下方式查询：
// hw_breakpoint_slots(TYPE_DATA) 返回实际监视点数量
```

- 常见 ARM64 CPU 实现 4 个监视点（WRP）
- 如果超出硬件限制，`register_user_hw_breakpoint()` 返回 `-ENOSPC`

**地址对齐限制：**

- 监视地址必须与监视长度对齐（如 4 字节监视需要 4 字节对齐）
- 不对齐的监视点请求会被内核拒绝
- GDB 在设置监视点时会自动处理对齐问题

**多线程监视点注意事项：**

1. **线程私有监视点**：每个线程可以设置独立的监视点（存储在 `thread_struct.debug.hbp_watch` 中）
2. **上下文切换**：内核在 `hw_breakpoint_thread_switch()` 中保存/恢复监视点寄存器
3. **资源共享**：不同线程的监视点相互独立，但需要注意硬件资源总量限制
4. **fork 行为**：子进程通过 `ptrace_hw_copy_thread()` 继承父进程的监视点设置（初始化为空）

```c
// arch/arm64/kernel/ptrace.c:227
void ptrace_hw_copy_thread(struct task_struct *tsk)
{
    memset(&tsk->thread.debug, 0, sizeof(struct debug_info));
}
```

**最佳实践：**

- 优先使用硬件监视点（性能最优）
- 在设置监视点前查询可用硬件资源
- 使用 `PTRACE_GETREGSET` + `NT_ARM_HW_WATCH` 获取资源信息
- 对于大范围监视，考虑使用 eBPF 或软件模拟方案
- 在多线程环境中，监视点设置在特定线程上，影响仅限该线程

### 5.4 断点命中

#### 5.4.1 流程总览

```
被调试进程执行到断点指令                    GDB (追踪器)               内核
    │                                           │                    │
    │ CPU 执行 BRK #0                                               │
    │ ──► 进入异常向量 el1_sync                    │                    │
    │ ──► do_el1_undef_instr()                     │                    │
    │ ──► brk_handler()                            │                    │
    │ ──► force_sig_fault(SIGTRAP, ...)             │                    │
    │                                           │                    │
    │ 信号处理                                      │                    │
    │ get_signal()                              │                    │
    │ ──► ptrace_signal()                           │                    │
    │ ──► ptrace_stop(SIGTRAP, CLD_TRAPPED)        │                    │
    │      │                                   │                    │
    │      ├──── set_special_state(TASK_TRACED)                     │
    │      ├──── current->exit_code = SIGTRAP                      │
    │      ├──── current->last_siginfo = &info                     │
    │      ├──── do_notify_parent_cldstop()                        │
    │      │       └── 发送 SIGCHLD 给 GDB                         │
    │      ├──── schedule() ──► 睡眠                               │
    │      │                                   │                    │
    │ wait4() SIGCHLD 到达                       │                    │
    │ ◄──────────────────────────────────────────┤                    │
    │ ptrace_do_wait()                          │                    │
    │ wait_task_stopped()                       │                    │
    │ 读取 p->exit_code = SIGTRAP               │                    │
    │ 返回停止信息给 GDB                         │                    │
    │                                           │                    │
    │ GDB 分析停止原因：SIGTRAP                   │                    │
    │ 检查 PC 指向 BRK 指令                     │                    │
    │ 恢复原始指令，PC 回退                     │                    │
    │                                           │                    │
    │ ptrace(PTRACE_CONT, ...)                  │                    │
    ├───────────────────────────────────────────►│ ptrace_resume()    │
    │                                           │ 恢复执行           │
```

#### 5.4.2 ARM64 断点异常处理

```c
// arch/arm64/kernel/debug-monitors.c
// 内核中 brk_handler 处理 BRK 异常
// 对于用户态断点，发送 SIGTRAP 信号

// include/linux/ptrace.h:407
static inline int ptrace_report_syscall(unsigned long message)
{
    int ptrace = current->ptrace;
    int signr;

    if (!(ptrace & PT_PTRACED))
        return 0;

    signr = ptrace_notify(SIGTRAP | ((ptrace & PT_TRACESYSGOOD) ? 0x80 : 0),
                          message);

    if (signr)
        send_sig(signr, current, 1);

    return fatal_signal_pending(current);
}
```

#### 5.4.3 ptrace_stop 核心实现

`ptrace_stop` 是 ptrace 机制的核心，用于将被追踪进程置入 `TASK_TRACED` 状态并通知追踪器：

```c
// kernel/signal.c:2349
static int ptrace_stop(int exit_code, int why, unsigned long message,
                       kernel_siginfo_t *info)
    __releases(&current->sighand->siglock)
    __acquires(&current->sighand->siglock)
{
    bool gstop_done = false;

    if (arch_ptrace_stop_needed()) {
        // 架构特定的预处理（如 ia64 需要回写寄存器存储）
        spin_unlock_irq(&current->sighand->siglock);
        arch_ptrace_stop();
        spin_lock_irq(&current->sighand->siglock);
    }

    // 检查 ptrace 是否已解除或收到致命信号
    if (!current->ptrace || __fatal_signal_pending(current))
        return exit_code;

    // 关键步骤：将进程状态设为 TASK_TRACED
    set_special_state(TASK_TRACED);
    current->jobctl |= JOBCTL_TRACED;

    // 内存屏障：确保 TASK_TRACED 在 TRAPPING 清除前可见
    smp_wmb();

    // 保存停止信息供追踪器读取
    current->ptrace_message = message;  // 事件消息
    current->last_siginfo = info;       // 信号信息
    current->exit_code = exit_code;     // 停止码

    // 处理组停止相关
    if (why == CLD_STOPPED && (current->jobctl & JOBCTL_STOP_PENDING))
        gstop_done = task_participate_group_stop(current);

    // 清除等待中的 trap 标志
    task_clear_jobctl_pending(current, JOBCTL_TRAP_STOP);
    if (info && info->si_code >> 8 == PTRACE_EVENT_STOP)
        task_clear_jobctl_pending(current, JOBCTL_TRAP_NOTIFY);

    // 清除 TRAPPING，通知 ptrace_attach 等待者
    task_clear_jobctl_trapping(current);

    spin_unlock_irq(&current->sighand->siglock);
    read_lock(&tasklist_lock);

    // 通知追踪器：发送 SIGCHLD 信号
    if (current->ptrace)
        do_notify_parent_cldstop(current, true, why);

    // 通知真正的父进程（如果组停止完成）
    if (gstop_done && (!current->ptrace || ptrace_reparented(current)))
        do_notify_parent_cldstop(current, false, why);

    read_unlock(&tasklist_lock);
    cgroup_enter_frozen();

    // 进入睡眠，等待追踪器用 ptrace_resume 唤醒
    schedule();

    cgroup_leave_frozen(true);

    // 被唤醒后恢复执行
    spin_lock_irq(&current->sighand->siglock);
    exit_code = current->exit_code;  // 追踪器设置的退出码
    current->last_siginfo = NULL;
    current->ptrace_message = 0;
    current->exit_code = 0;

    // 清除 LISTENING 和 FROZEN 标志
    current->jobctl &= ~(JOBCTL_LISTENING | JOBCTL_PTRACE_FROZEN);

    // 重新计算挂起信号
    recalc_sigpending_tsk(current);
    return exit_code;
}
```

### 5.5 追踪器等待处理（wait4 系统调用）

GDB 通过 `wait4()` 或 `waitpid()` 等待被调试进程停止。内核通过 `ptrace_do_wait()` 和 `wait_task_stopped()` 处理。

**wait4 系统调用入口：**

```c
// kernel/exit.c:1905
SYSCALL_DEFINE4(wait4, pid_t, upid, int __user *, stat_addr,
                int, options, struct rusage __user *, ru)
{
    // ... 解析参数
    ret = do_wait(&wo);
    // ...
}
```

**do_wait 遍历子进程和追踪子进程：**

```c
// kernel/exit.c:1663
long __do_wait(struct wait_opts *wo)
{
    read_lock(&tasklist_lock);

    if (wo->wo_type == PIDTYPE_PID) {
        retval = do_wait_pid(wo);
    } else {
        struct task_struct *tsk = current;
        do {
            retval = do_wait_thread(wo, tsk);  // 遍历子进程
            if (retval)
                return retval;

            retval = ptrace_do_wait(wo, tsk);   // 遍历 ptrace 子进程
            if (retval)
                return retval;
        } while_each_thread(current, tsk);
    }
    // ...
}
```

**ptrace_do_wait 遍历被追踪进程：**

```c
// kernel/exit.c:1579
static int ptrace_do_wait(struct wait_opts *wo, struct task_struct *tsk)
{
    struct task_struct *p;

    list_for_each_entry(p, &tsk->ptraced, ptrace_entry) {
        int ret = wait_consider_task(wo, 1, p);  // ptrace = 1
        if (ret)
            return ret;
    }
    return 0;
}
```

**wait_task_stopped 读取停止状态：**

```c
// kernel/exit.c:1329
static int wait_task_stopped(struct wait_opts *wo,
                            int ptrace, struct task_struct *p)
{
    int exit_code, *p_code, why;
    pid_t pid;

    // ptrace 停止总是可见（不需要 WUNTRACED）
    if (!ptrace && !(wo->wo_flags & WUNTRACED))
        return 0;

    p_code = task_stopped_code(p, ptrace);  // ptrace=true 时返回 &p->exit_code
    if (!p_code)
        return 0;

    spin_lock_irq(&p->sighand->siglock);
    p_code = task_stopped_code(p, ptrace);
    exit_code = *p_code;
    if (!exit_code)
        goto unlock_sig;

    if (!unlikely(wo->wo_flags & WNOWAIT))
        *p_code = 0;  // 清零 exit_code，表示已消费

    // 填充返回状态
    spin_unlock_irq(&p->sighand->siglock);

    why = ptrace ? CLD_TRAPPED : CLD_STOPPED;
    wo->wo_stat = (exit_code << 8) | 0x7f;  // 状态编码：低7位为信号号

    return pid;
}
```

### 5.6 继续执行（ptrace_resume）

#### 5.6.1 流程总览

GDB 使用 `PTRACE_CONT`、`PTRACE_SYSCALL`、`PTRACE_SINGLESTEP` 恢复被调试进程执行。

**ptrace_resume 实现：**

```c
// kernel/ptrace.c:823
static int ptrace_resume(struct task_struct *child, long request,
                         unsigned long data)
{
    if (!valid_signal(data))
        return -EIO;

    // 设置/清除 SYSCALL_TRACE 工作标志
    if (request == PTRACE_SYSCALL)
        set_task_syscall_work(child, SYSCALL_TRACE);
    else
        clear_task_syscall_work(child, SYSCALL_TRACE);

    // 设置/清除 SYSCALL_EMU 工作标志
    if (request == PTRACE_SYSEMU || request == PTRACE_SYSEMU_SINGLESTEP)
        set_task_syscall_work(child, SYSCALL_EMU);
    else
        clear_task_syscall_work(child, SYSCALL_EMU);

    // 单步/块步设置
    if (is_singleblock(request)) {
        user_enable_block_step(child);
    } else if (is_singlestep(request) || is_sysemu_singlestep(request)) {
        user_enable_single_step(child);
    } else {
        user_disable_single_step(child);
    }

    // 设置退出码和唤醒进程
    spin_lock_irq(&child->sighand->siglock);
    child->exit_code = data;  // 传递的信号（0 表示无信号）
    child->jobctl &= ~JOBCTL_TRACED;  // 清除 TRACED 状态
    wake_up_state(child, __TASK_TRACED);  // 唤醒被追踪进程
    spin_unlock_irq(&child->sighand->siglock);

    return 0;
}
```

**被追踪进程被唤醒后的执行路径：**

```
ptrace_resume() 唤醒被追踪进程
    │
    v
被追踪进程从 schedule() 返回
    │
    ├── exit_code = ptrace_stop 返回的码
    ├── 继续回到 get_signal() 或 ret_to_user
    │
    ├── data=0: 正常继续执行
    ├── data=signal: 传递信号给被调试进程
    │
    └── 单步模式：CPU 执行一条指令后触发单步异常
```

### 5.7 单步执行

#### 5.7.1 流程总览

```
GDB                                        被调试进程                内核
  │                                           │                    │
  │ ptrace(PTRACE_SINGLESTEP, ...)             │                    │
  ├───────────────────────────────────────────►│ ptrace_resume()    │
  │                                           │ user_enable_       │
  │                                           │ single_step()      │
  │                                           │ 设置 PSTATE.SS=1   │
  │                                           │ 或硬件调试寄存器    │
  │                                           │                    │
  │                                           │ 执行一条指令       │
  │                                           │ SS 异常触发        │
  │                                           │ ──► SIGTRAP        │
  │                                           │ ──► ptrace_stop()  │
  │                                           │                    │
  │ wait4() 收到 SIGTRAP                      │                    │
  │ ◄─────────────────────────────────────────┤                    │
  │                                           │                    │
  │ GDB 分析 PC 寄存器，显示当前指令           │                    │
```

#### 5.7.2 ARM64 单步实现

ARM64 使用 PSTATE 寄存器中的 SS（Software Step）位来实现单步：

```c
// arch/arm64/kernel/debug-monitors.c
// 单步通过 PSTATE.SS 位控制
// 当 SS=1 时，执行一条指令后触发 Single Step 异常

// 内核中 user_enable_single_step 设置 PSTATE.SS 位
// 每条指令执行后 CPU 自动清除 SS 位并触发异常
```

**关键点：**
- ARM64 PSTATE 寄存器中的 SS 位（bit 21）控制单步
- 设置 SS=1 后，CPU 执行一条指令就触发 `Single Step` 异常
- 异常处理程序发送 `SIGTRAP` 信号
- 被追踪进程进入 `ptrace_stop`，等待追踪器处理

### 5.8 系统调用追踪

#### 5.8.1 流程总览

```
被调试进程                            GDB (追踪器)                  内核
    │                                    │                       │
    │ 执行 SVC 指令                                                │
    │ ──► el0_svc                                        │
    │                                    │                       │
    │ el0_svc_common()                                        │
    │ has_syscall_work(flags) 检查                                │
    │ (TIF_SYSCALL_TRACE 已设置)                              │
    │                                    │                       │
    │ syscall_trace_enter()                                    │
    │ ──► report_syscall_entry()                                  │
    │      ├──── ptrace_save_reg() 保存 x7                        │
    │      │      └── x7 = PTRACE_SYSCALL_ENTER                   │
    │      ├──── ptrace_report_syscall_entry()                    │
    │      │      └── ptrace_notify(SIGTRAP|0x80)                 │
    │      │      └── ptrace_stop()                               │
    │      │           │  TASK_TRACED, exit_code=SIGTRAP|0x80     │
    │      │           │  ptrace_message=ENTRY                    │
    │      │           │  schedule()                              │
    │      │                                    │                 │
    │ wait4() SIGTRAP│0x80                   │                    │
    │ ◄───────────────────────────────────────┤                    │
    │ GDB 读取系统调用号和参数               │                    │
    │ ptrace(PTRACE_GETREGSET)               │                    │
    │ ptrace(PTRACE_GET_SYSCALL_INFO)        │                    │
    │                                    │                       │
    │ GDB 可修改系统调用号/参数               │                    │
    │ ptrace(PTRACE_SETREGSET)               │                    │
    │ ptrace(PTRACE_SET_SYSCALL_INFO)        │                    │
    │                                    │                       │
    │ ptrace(PTRACE_SYSCALL, ...)            │                    │
    ├───────────────────────────────────────►│ ptrace_resume()    │
    │                                    │ set SYSCALL_TRACE    │
    │                                    │ wake_up_state()      │
    │                                    │                       │
    │ 从 ptrace_stop 返回                    │                    │
    │ x7 恢复为原始值                     │                    │
    │                                    │                       │
    │ 执行系统调用                             │                    │
    │ invoke_syscall()                    │                    │
    │                                    │                       │
    │ syscall_trace_exit()                                     │
    │ ──► report_syscall_exit()                                   │
    │      ├──── ptrace_save_reg() 保存 x7                        │
    │      │      └── x7 = PTRACE_SYSCALL_EXIT                    │
    │      ├──── ptrace_report_syscall_exit()                     │
    │      │      └── ptrace_notify(SIGTRAP|0x80)                 │
    │      │      └── ptrace_stop()                               │
    │      │           │  TASK_TRACED                             │
    │      │           │  schedule()                              │
    │      │                                    │                 │
    │ wait4() 再次收到通知                    │                    │
    │ ◄───────────────────────────────────────┤                    │
    │ GDB 读取系统调用返回值                 │                    │
    │                                    │                       │
    │ ptrace(PTRACE_SYSCALL, ...)            │                    │
    ├───────────────────────────────────────►│ ptrace_resume()    │
    │                                    │ 继续执行              │
```

#### 5.8.2 ARM64 系统调用追踪实现

**ARM64 系统调用入口：**

```c
// arch/arm64/kernel/syscall.c:73
static void el0_svc_common(struct pt_regs *regs, int scno, int sc_nr,
                           const syscall_fn_t syscall_table[])
{
    unsigned long flags = read_thread_flags();

    regs->orig_x0 = regs->regs[0];
    regs->syscallno = scno;

    // 检查是否有系统调用追踪工作
    if (has_syscall_work(flags)) {
        if (scno == NO_SYSCALL)
            syscall_set_return_value(current, regs, -ENOSYS, 0);
        scno = syscall_trace_enter(regs);  // 进入追踪
        if (scno == NO_SYSCALL)
            goto trace_exit;  // 追踪器要求跳过系统调用
    }

    invoke_syscall(regs, scno, sc_nr, syscall_table);  // 执行系统调用

    // 检查是否需要追踪出口
    if (!has_syscall_work(flags) && !IS_ENABLED(CONFIG_DEBUG_RSEQ)) {
        flags = read_thread_flags();
        if (!has_syscall_work(flags) && !(flags & _TIF_SINGLESTEP))
            return;
    }

trace_exit:
    syscall_trace_exit(regs);  // 退出追踪
}
```

**ARM64 系统调用入口追踪：**

```c
// arch/arm64/kernel/ptrace.c:2411
int syscall_trace_enter(struct pt_regs *regs)
{
    unsigned long flags = read_thread_flags();
    int ret;

    if (flags & (_TIF_SYSCALL_EMU | _TIF_SYSCALL_TRACE)) {
        ret = report_syscall_entry(regs);
        // 如果追踪器返回非零或者 SYSCALL_EMU 模式，跳过系统调用
        if (ret || (flags & _TIF_SYSCALL_EMU))
            return NO_SYSCALL;
    }

    // seccomp 检查
    if (secure_computing() == -1)
        return NO_SYSCALL;

    // tracepoint 追踪
    if (test_thread_flag(TIF_SYSCALL_TRACEPOINT))
        trace_sys_enter(regs, regs->syscallno);

    audit_syscall_entry(regs->syscallno, regs->orig_x0, regs->regs[1],
                        regs->regs[2], regs->regs[3]);

    return regs->syscallno;
}
```

**ARM64 系统调用出口追踪：**

```c
// arch/arm64/kernel/ptrace.c:2435
void syscall_trace_exit(struct pt_regs *regs)
{
    unsigned long flags = read_thread_flags();

    audit_syscall_exit(regs);

    if (flags & _TIF_SYSCALL_TRACEPOINT)
        trace_sys_exit(regs, syscall_get_return_value(current, regs));

    if (flags & (_TIF_SYSCALL_TRACE | _TIF_SINGLESTEP))
        report_syscall_exit(regs);

    rseq_syscall(regs);
}
```

**ARM64 寄存器保存机制（用于区分入口/出口）：**

```c
// arch/arm64/kernel/ptrace.c:2347
static __always_inline unsigned long ptrace_save_reg(struct pt_regs *regs,
                                                     enum ptrace_syscall_dir dir,
                                                     int *regno)
{
    /*
     * AArch64 使用 x7 寄存器来区分系统调用入口/出口停止：
     * - 入口停止：x7 = 0 (PTRACE_SYSCALL_ENTER)
     * - 出口停止：x7 = 1 (PTRACE_SYSCALL_EXIT)
     * AArch32 使用 r12 寄存器
     */
    *regno = (is_compat_task() ? 12 : 7);
    saved_reg = regs->regs[*regno];
    regs->regs[*regno] = dir;
    return saved_reg;
}
```

### 5.9 信号处理

#### 5.9.1 信号拦截与传递

当被调试进程收到信号时，内核通过 `ptrace_signal()` 拦截信号并将信号传递给追踪器处理：

```c
// kernel/signal.c:2731
static int ptrace_signal(int signr, kernel_siginfo_t *info, enum pid_type type)
{
    // 标记停止信号已出队
    current->jobctl |= JOBCTL_STOP_DEQUEUED;

    // 进入 ptrace_stop，通知追踪器有信号到达
    signr = ptrace_stop(signr, CLD_TRAPPED, 0, info);

    // 从 ptrace_stop 返回
    // 如果 signr == 0，表示追踪器取消了信号
    if (signr == 0)
        return signr;

    // 如果追踪器修改了信号，更新 siginfo
    if (signr != info->si_signo) {
        clear_siginfo(info);
        info->si_signo = signr;
        info->si_errno = 0;
        info->si_code = SI_USER;
        info->si_pid = task_pid_vnr(current->parent);
        info->si_uid = from_kuid_munged(current_user_ns(),
                                        task_uid(current->parent));
    }

    // 如果信号被阻塞，重新入队
    if (sigismember(&current->blocked, signr) ||
        fatal_signal_pending(current)) {
        send_signal_locked(signr, info, current, type);
        signr = 0;
    }

    return signr;
}
```

**信号处理流程：**

```
被调试进程收到信号
    │
    v
get_signal() 循环
    │
    ├── 检查 JOBCTL_STOP_PENDING ──► do_signal_stop()
    ├── 检查 JOBCTL_TRAP_MASK ──► do_jobctl_trap()
    │
    ├── 非 SIGKILL 信号：
    │    └── ptrace_signal(signr, info, type)
    │         └── ptrace_stop(signr, CLD_TRAPPED, 0, info)
    │              ├── TASK_TRACED
    │              ├── exit_code = signr
    │              ├── 通知追踪器
    │              └── schedule()
    │
    ├── 追踪器通过 ptrace(PTRACE_CONT, pid, 0, sig) 恢复
    │    └── ptrace_stop 返回 signr
    │
    └── 信号传递到被调试进程或取消
```

**GDB 通过 `PTRACE_GETSIGINFO` 和 `PTRACE_SETSIGINFO` 查看/修改信号：**

```c
// kernel/ptrace.c:677
static int ptrace_getsiginfo(struct task_struct *child, kernel_siginfo_t *info)
{
    unsigned long flags;
    int error = -ESRCH;

    if (lock_task_sighand(child, &flags)) {
        error = -EINVAL;
        if (likely(child->last_siginfo != NULL)) {
            copy_siginfo(info, child->last_siginfo);
            error = 0;
        }
        unlock_task_sighand(child, &flags);
    }
    return error;
}
```

### 5.10 寄存器读写

#### 5.10.1 regset 机制

ARM64 使用 regset 机制统一管理寄存器读写，支持多种寄存器类型：

```c
// arch/arm64/kernel/ptrace.c:1577
enum aarch64_regset {
    REGSET_GPR,           // 通用寄存器
    REGSET_FPR,           // 浮点寄存器
    REGSET_TLS,           // TLS 寄存器
    REGSET_HW_BREAK,      // 硬件断点
    REGSET_HW_WATCH,      // 硬件监视点
    REGSET_FPMR,          // 浮点模式寄存器
    REGSET_SYSTEM_CALL,   // 系统调用号
    REGSET_SVE,           // SVE（可伸缩向量扩展）
    REGSET_SSVE,          // 流模式 SVE
    REGSET_ZA,            // SME ZA 数组
    REGSET_ZT,            // SME ZT 数组
    REGSET_PAC_MASK,      // PAC 掩码
    REGSET_PAC_ENABLED_KEYS,  // PAC 启用密钥
    REGSET_PACA_KEYS,     // PAC 地址密钥
    REGSET_PACG_KEYS,     // PAC 通用密钥
    REGSET_TAGGED_ADDR_CTRL,  // 标记地址控制
    REGSET_POE,           // 权限覆盖扩展
    REGSET_GCS,           // 影子栈（GCS）
};
```

**通用寄存器读写：**

```c
// arch/arm64/kernel/ptrace.c:555
static int gpr_get(struct task_struct *target,
                   const struct user_regset *regset,
                   struct membuf to)
{
    struct user_pt_regs *uregs = &task_pt_regs(target)->user_regs;
    return membuf_write(&to, uregs, sizeof(*uregs));
}

static int gpr_set(struct task_struct *target, const struct user_regset *regset,
                   unsigned int pos, unsigned int count,
                   const void *kbuf, const void __user *ubuf)
{
    int ret;
    struct user_pt_regs newregs = task_pt_regs(target)->user_regs;

    ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &newregs, 0, -1);
    if (ret)
        return ret;

    if (!valid_user_regs(&newregs, target))
        return -EINVAL;

    task_pt_regs(target)->user_regs = newregs;
    return 0;
}
```

**通过 `PTRACE_GETREGSET/SETREGSET` 操作寄存器：**

```c
// kernel/ptrace.c:888
static int ptrace_regset(struct task_struct *task, int req, unsigned int type,
                         struct iovec *kiov)
{
    const struct user_regset_view *view = task_user_regset_view(task);
    const struct user_regset *regset = find_regset(view, type);
    int regset_no;

    if (!regset || (kiov->iov_len % regset->size) != 0)
        return -EINVAL;

    regset_no = regset - view->regsets;
    kiov->iov_len = min(kiov->iov_len,
                        (__kernel_size_t) (regset->n * regset->size));

    if (req == PTRACE_GETREGSET)
        return copy_regset_to_user(task, view, regset_no, 0,
                                   kiov->iov_len, kiov->iov_base);
    else
        return copy_regset_from_user(task, view, regset_no, 0,
                                     kiov->iov_len, kiov->iov_base);
}
```

### 5.11 获取/设置系统调用信息

Linux 5.3+ 提供了 `PTRACE_GET_SYSCALL_INFO` 和 `PTRACE_SET_SYSCALL_INFO` 操作，用于获取/设置系统调用详细信息：

```c
// kernel/ptrace.c:967
static int ptrace_get_syscall_info_op(struct task_struct *child)
{
    // 根据 last_siginfo 的 si_code 判断停止类型
    switch (child->last_siginfo ? child->last_siginfo->si_code : 0) {
    case SIGTRAP | 0x80:
        // 系统调用追踪停止
        switch (child->ptrace_message) {
        case PTRACE_EVENTMSG_SYSCALL_ENTRY:
            return PTRACE_SYSCALL_INFO_ENTRY;  // 入口
        case PTRACE_EVENTMSG_SYSCALL_EXIT:
            return PTRACE_SYSCALL_INFO_EXIT;   // 出口
        }
    case SIGTRAP | (PTRACE_EVENT_SECCOMP << 8):
        return PTRACE_SYSCALL_INFO_SECCOMP;    // seccomp
    }
    return PTRACE_SYSCALL_INFO_NONE;
}
```

**用户态数据结构：**

```c
// include/uapi/linux/ptrace.h:83
struct ptrace_syscall_info {
    __u8 op;        // PTRACE_SYSCALL_INFO_ENTRY/EXIT/SECCOMP/NONE
    __u8 reserved;
    __u16 flags;
    __u32 arch;     // 架构标识（如 AUDIT_ARCH_AARCH64）
    __u64 instruction_pointer;
    __u64 stack_pointer;
    union {
        struct {
            __u64 nr;         // 系统调用号
            __u64 args[6];    // 系统调用参数
        } entry;
        struct {
            __s64 rval;       // 返回值
            __u8 is_error;    // 是否错误
        } exit;
        struct {
            __u64 nr;
            __u64 args[6];
            __u32 ret_data;
            __u32 reserved2;
        } seccomp;
    };
};
```

### 5.12 分离操作

#### 5.12.1 流程总览

```
GDB                                    被调试进程                    内核
  │                                        │                         │
  │ ptrace(PTRACE_DETACH, pid, 0, sig)     │                         │
  ├───────────────────────────────────────►│ ptrace_detach()         │
  │                                        │ ptrace_disable()        │
  │                                        │ user_disable_single_    │
  │                                        │   step()                │
  │                                        │                         │
  │                                        │ __ptrace_detach()       │
  │                                        │ ├── __ptrace_unlink()   │
  │                                        │ │   ├── child->parent = │
  │                                        │ │   │   real_parent     │
  │                                        │ │   ├── child->ptrace=0 │
  │                                        │ │   ├── 清除 JOBCTL_TRAP│
  │                                        │ │   └── 唤醒进程        │
  │                                        │ └── 处理僵尸状态        │
  │                                        │                         │
  │                                        │ child->exit_code = sig  │
  │                                        │ ──► 恢复执行           │
```

#### 5.12.2 核心代码分析

```c
// kernel/ptrace.c:563
static int ptrace_detach(struct task_struct *child, unsigned int data)
{
    if (!valid_signal(data))
        return -EIO;

    // 架构特定的清理（禁用单步等）
    ptrace_disable(child);

    write_lock_irq(&tasklist_lock);

    WARN_ON(!child->ptrace || child->exit_state);
    // 设置退出码（要传递的信号）
    child->exit_code = data;
    __ptrace_detach(current, child);  // 解除追踪关系

    write_unlock_irq(&tasklist_lock);

    proc_ptrace_connector(child, PTRACE_DETACH);
    return 0;
}
```

**__ptrace_unlink 解除追踪关系：**

```c
// kernel/ptrace.c:117
void __ptrace_unlink(struct task_struct *child)
{
    const struct cred *old_cred;

    // 清除系统调用追踪标志
    clear_task_syscall_work(child, SYSCALL_TRACE);
    clear_task_syscall_work(child, SYSCALL_EMU);

    // 恢复 parent 为真正的父进程
    child->parent = child->real_parent;
    list_del_init(&child->ptrace_entry);
    old_cred = child->ptracer_cred;
    child->ptracer_cred = NULL;
    put_cred(old_cred);

    spin_lock(&child->sighand->siglock);
    child->ptrace = 0;  // 清除所有 ptrace 标志

    // 清除所有挂起的 trap 和 TRAPPING
    task_clear_jobctl_pending(child, JOBCTL_TRAP_MASK);
    task_clear_jobctl_trapping(child);

    // 恢复 JOBCTL_STOP_PENDING（如果组停止进行中）
    if (!(child->flags & PF_EXITING) &&
        (child->signal->flags & SIGNAL_STOP_STOPPED ||
         child->signal->group_stop_count))
        child->jobctl |= JOBCTL_STOP_PENDING;

    // 如果进程在 TRACED 状态，唤醒它
    if (child->jobctl & JOBCTL_STOP_PENDING || task_is_traced(child))
        ptrace_signal_wake_up(child, true);

    spin_unlock(&child->sighand->siglock);
}
```

### 5.13 追踪器退出处理

当追踪器（GDB）异常退出时，内核自动清理所有被追踪的进程：

```c
// kernel/ptrace.c:594
void exit_ptrace(struct task_struct *tracer, struct list_head *dead)
{
    struct task_struct *p, *n;

    list_for_each_entry_safe(p, n, &tracer->ptraced, ptrace_entry) {
        // 如果设置了 PT_EXITKILL，发送 SIGKILL
        if (unlikely(p->ptrace & PT_EXITKILL))
            send_sig_info(SIGKILL, SEND_SIG_PRIV, p);

        // 解除追踪关系
        if (__ptrace_detach(tracer, p))
            list_add(&p->ptrace_entry, dead);
    }
}
```

## 6. 完整调试场景分析

### 6.1 GDB 调试循环详细流程

```
┌──────────────────────────────────────────────────────────────────┐
│                    GDB 主调试循环                                  │
│                                                                  │
│  while (1) {                                                      │
│      // 1. 等待被调试进程停止                                     │
│      status = waitpid(pid, &wstat, WUNTRACED|WEXITED);           │
│                                                                  │
│      // 2. 分析停止原因                                           │
│      if (WIFEXITED(wstat)) {                                     │
│          // 进程正常退出                                          │
│          break;                                                   │
│      }                                                            │
│      if (WIFSIGNALED(wstat)) {                                    │
│          // 进程被信号杀死                                        │
│          break;                                                   │
│      }                                                            │
│      if (WIFSTOPPED(wstat)) {                                     │
│          sig = WSTOPSIG(wstat);  // 获取停止信号                   │
│          if (sig == SIGTRAP) {                                     │
│              // 断点/单步/系统调用停止                             │
│              if (WSTOPSIG(wstat) == (SIGTRAP | 0x80)) {            │
│                  // 系统调用追踪停止                               │
│                  handle_syscall_stop();                            │
│              } else {                                              │
│                  // 断点或单步停止                                 │
│                  handle_breakpoint_stop();                         │
│              }                                                     │
│          } else if (sig == SIGSTOP) {                              │
│              // 初始附加停止                                       │
│              handle_attach_stop();                                 │
│          } else {                                                  │
│              // 其他信号                                           │
│              handle_signal_stop(sig);                              │
│          }                                                         │
│      }                                                             │
│                                                                  │
│      // 3. 处理用户命令（读取/写入）                               │
│      while (cmd = get_user_command()) {                            │
│          switch (cmd) {                                            │
│          case "continue":                                          │
│              ptrace(PTRACE_CONT, ...);                             │
│              break;                                                │
│          case "next":                                              │
│              // 设置下一个断点                                     │
│              ptrace(PTRACE_CONT, ...);                             │
│              break;                                                │
│          case "step":                                              │
│              ptrace(PTRACE_SINGLESTEP, ...);                       │
│              break;                                                │
│          case "break":                                             │
│              ptrace(PTRACE_POKEDATA, addr, BRK_INSTR);             │
│              break;                                                │
│          case "print":                                             │
│              ptrace(PTRACE_PEEKDATA, addr, ...);                   │
│              break;                                                │
│          case "finish":                                            │
│              // 在当前函数返回地址设断点                            │
│              ptrace(PTRACE_CONT, ...);                             │
│              break;                                                │
│          }                                                         │
│      }                                                             │
│  }                                                                 │
└──────────────────────────────────────────────────────────────────┘
```

### 6.2 GDB 启动调试完整场景

**场景：** `gdb ./a.out` → `run`

```
1. GDB 进程 fork()
2. 子进程调用 ptrace(PTRACE_TRACEME)
3. 子进程调用 execve("./a.out")
   └── 内核在 exec 完成后发送 SIGTRAP
   └── 子进程进入 ptrace_stop(SIGTRAP, CLD_TRAPPED)
   └── 子进程进入 TASK_TRACED 状态
4. GDB 调用 wait4()
   └── ptrace_do_wait() → wait_task_stopped()
   └── 读取 exit_code = SIGTRAP
   └── 返回停止信息
5. GDB 读取系统类型、入口点等信息
   └── ptrace(PTRACE_GETREGSET, REGSET_GPR)
   └── ptrace(PTRACE_PEEKDATA, ...)
6. GDB 设置断点
   └── ptrace(PTRACE_POKEDATA, addr, BRK_INSTR)
7. GDB 恢复执行
   └── ptrace(PTRACE_CONT, pid, 0, 0)
   └── 内核 ptrace_resume()
   └── 被调试进程从 schedule() 返回
   └── 继续执行用户代码
8. 进程执行到断点指令 BRK #0
   └── CPU 触发 BRK 异常
   └── 内核发送 SIGTRAP
   └── ptrace_stop(SIGTRAP, CLD_TRAPPED)
9. GDB 再次 wait4() 收到 SIGTRAP
   └── 分析停止原因
   └── 显示当前源代码位置
10. 用户输入命令，GDB 处理
    └── 循环重复步骤 5-10
```

### 6.3 GDB 附加到运行中进程完整场景

**场景：** `gdb -p PID` 或 `attach PID`

```
1. GDB 调用 ptrace(PTRACE_ATTACH, pid)
   └── 内核 ptrace_attach():
       ├── find_get_task_by_vpid(pid)  // 查找目标进程
       ├── __ptrace_may_access()       // 权限检查
       ├── ptrace_link()               // 建立追踪关系
       │   └── child->parent = current
       │   └── child->ptrace = PT_PTRACED
       └── ptrace_set_stopped()        // 发送 SIGSTOP

2. 目标进程收到 SIGSTOP
   └── get_signal() → 检查 JOBCTL_STOP_PENDING
   └── ptrace_stop(SIGSTOP, CLD_STOPPED)
   └── TASK_TRACED, exit_code = SIGSTOP

3. GDB 调用 wait4(pid, &status, 0)
   └── ptrace_do_wait() → wait_task_stopped()
   └── 读取 exit_code = SIGSTOP
   └── 返回停止信息

4. GDB 读取寄存器、内存信息
   └── ptrace(PTRACE_GETREGSET, REGSET_GPR)
   └── ... 读取各种调试信息

5. 设置断点、恢复执行等操作同启动场景
```

### 6.4 单步执行完整场景

**场景：** GDB 中执行 `stepi` 命令

```
1. GDB 调用 ptrace(PTRACE_SINGLESTEP, pid, 0, 0)
   └── 内核 ptrace_resume():
       ├── clear_task_syscall_work(child, SYSCALL_TRACE)
       ├── user_enable_single_step(child)
       │   └── ARM64: 设置 PSTATE.SS = 1
       ├── child->exit_code = 0
       ├── child->jobctl &= ~JOBCTL_TRACED
       └── wake_up_state(child, __TASK_TRACED)

2. 被调试进程从 schedule() 返回
   └── do_notify_resume() 检查 TIF_SINGLESTEP
   └── 返回用户态执行一条指令

3. CPU 执行一条指令后触发 Single Step 异常
   └── PSTATE.SS 被 CPU 自动清除
   └── 进入 el1_sync 异常处理
   └── 内核发送 SIGTRAP
   └── ptrace_stop(SIGTRAP, CLD_TRAPPED)

4. GDB 调用 wait4() 收到 SIGTRAP
   └── 读取 PC 寄存器，确认当前指令位置
   └── 显示当前指令

5. 用户可继续 stepi 或执行其他命令
```

### 6.5 系统调用追踪完整场景

**场景：** strace 追踪系统调用

```
1. strace 调用 ptrace(PTRACE_TRACEME) 或 PTRACE_ATTACH
2. strace 设置 PTRACE_SETOPTIONS(PTRACE_O_TRACESYSGOOD)
3. strace 调用 ptrace(PTRACE_SYSCALL, pid, 0, 0)
   └── 内核 ptrace_resume():
       ├── set_task_syscall_work(child, SYSCALL_TRACE)
       │   └── 设置 TIF_SYSCALL_TRACE 标志
       └── 唤醒进程

4. 被调试进程执行 SVC 指令进入系统调用
   └── el0_svc_common():
       └── has_syscall_work(flags) = true
       └── syscall_trace_enter(regs):
           ├── report_syscall_entry(regs)
           │   ├── ptrace_save_reg() 设置 x7 = 0 (ENTRY)
           │   └── ptrace_notify(SIGTRAP|0x80, ENTRY)
           │       └── ptrace_stop(SIGTRAP|0x80, CLD_TRAPPED, ENTRY)
           │           └── TASK_TRACED, schedule()
           │
           ├── strace 通过 wait4() 收到 SIGTRAP|0x80
           │   └── 读取系统调用号和参数
           │   └── ptrace(PTRACE_SYSCALL, ...) 恢复
           │
           └── 从 ptrace_stop 返回，继续执行

5. 内核执行系统调用
   └── invoke_syscall(regs, scno, ...)

6. 系统调用完成后
   └── syscall_trace_exit(regs):
       ├── report_syscall_exit(regs)
       │   ├── ptrace_save_reg() 设置 x7 = 1 (EXIT)
       │   └── ptrace_notify(SIGTRAP|0x80, EXIT)
       │       └── ptrace_stop(SIGTRAP|0x80, CLD_TRAPPED, EXIT)
       │
       ├── strace 通过 wait4() 收到 SIGTRAP|0x80
       │   └── 读取返回值
       │   └── ptrace(PTRACE_SYSCALL, ...) 继续
       │
       └── 循环步骤 4-6 追踪每个系统调用
```

## 7. 关键函数调用栈

### 7.1 系统调用入口追踪

```
SVC 指令
  └── el0_svc() 或 el0_ia32_svc()              // arch/arm64/kernel/entry.S
      └── el0_svc_common(regs, scno, ...)       // arch/arm64/kernel/syscall.c:73
          └── syscall_trace_enter(regs)          // arch/arm64/kernel/ptrace.c:2411
              └── report_syscall_entry(regs)     // arch/arm64/kernel/ptrace.c:2376
                  ├── ptrace_save_reg(regs, ENTER, &regno)
                  └── ptrace_report_syscall_entry(regs)  // include/linux/ptrace.h:449
                      └── ptrace_report_syscall(ENTRY)   // include/linux/ptrace.h:407
                          └── ptrace_notify(SIGTRAP|0x80, ENTRY)  // kernel/signal.c:2500
                              └── ptrace_stop(SIGTRAP|0x80, CLD_TRAPPED, ENTRY, info)
                                  ├── set_special_state(TASK_TRACED)
                                  ├── do_notify_parent_cldstop(current, true, CLD_TRAPPED)
                                  │   └── 发送 SIGCHLD 给追踪器
                                  └── schedule()
```

### 7.2 断点命中

```
用户态执行 BRK #0
  └── el1_sync 异常向量
      └── do_el1_undef_instr() 或 brk_handler()
          └── force_sig_fault(SIGTRAP, TRAP_BRKPT, ...)
              └── send_signal(SIGTRAP, ...)
                  └── 信号递送
                      └── get_signal()                     // kernel/signal.c:2799
                          └── ptrace_signal(signr, info, type)  // kernel/signal.c:2731
                              └── ptrace_stop(signr, CLD_TRAPPED, 0, info)
                                  ├── set_special_state(TASK_TRACED)
                                  ├── do_notify_parent_cldstop(current, true, CLD_TRAPPED)
                                  └── schedule()
```

### 7.3 追踪器等待

```
wait4(pid, &status, options)
  └── SYSCALL_DEFINE4(wait4)                    // kernel/exit.c:1905
      └── do_wait(&wo)                          // kernel/exit.c:1710
          └── __do_wait(&wo)                    // kernel/exit.c:1663
              ├── do_wait_thread(wo, tsk)       // 遍历子进程
              └── ptrace_do_wait(wo, tsk)       // 遍历 ptrace 子进程
                  └── wait_consider_task(wo, 1, p)  // kernel/exit.c:1453
                      └── wait_task_stopped(wo, 1, p)  // kernel/exit.c:1329
                          ├── task_stopped_code(p, true)  // 返回 &p->exit_code
                          ├── 读取 exit_code
                          ├── 清零 exit_code
                          └── 返回 pid 和状态信息
```

### 7.4 继续执行

```
ptrace(PTRACE_CONT, pid, 0, sig)
  └── SYSCALL_DEFINE4(ptrace)                   // kernel/ptrace.c:1387
      └── ptrace_check_attach(child, false)     // 确认进程在 TASK_TRACED
          └── ptrace_freeze_traced(child)       // 冻结进程
      └── arch_ptrace(child, CONT, 0, sig)      // arm64: 调用 ptrace_request
          └── ptrace_request(child, CONT, 0, sig)  // kernel/ptrace.c:1136
              └── ptrace_resume(child, CONT, sig)  // kernel/ptrace.c:823
                  ├── clear_task_syscall_work(child, SYSCALL_TRACE)
                  ├── user_disable_single_step(child)
                  ├── child->exit_code = sig
                  ├── child->jobctl &= ~JOBCTL_TRACED
                  └── wake_up_state(child, __TASK_TRACED)
                      └── try_to_wake_up()
                          └── 被追踪进程从 schedule() 返回
                              └── ptrace_stop 返回 exit_code
                                  └── 继续执行用户代码
```

## 8. 用户态 ptrace 选项

通过 `PTRACE_SETOPTIONS` 设置追踪选项，控制哪些事件需要通知追踪器：

```c
// include/uapi/linux/ptrace.h
#define PTRACE_O_TRACESYSGOOD   1          // 系统调用追踪时使用 SIGTRAP|0x80
#define PTRACE_O_TRACEFORK      (1 << 1)   // 追踪 fork 事件
#define PTRACE_O_TRACEVFORK     (1 << 2)   // 追踪 vfork 事件
#define PTRACE_O_TRACECLONE     (1 << 3)   // 追踪 clone 事件
#define PTRACE_O_TRACEEXEC      (1 << 4)   // 追踪 exec 事件
#define PTRACE_O_TRACEVFORKDONE (1 << 5)   // 追踪 vfork 完成
#define PTRACE_O_TRACEEXIT      (1 << 6)   // 追踪 exit 事件
#define PTRACE_O_TRACESECCOMP   (1 << 7)   // 追踪 seccomp 事件
#define PTRACE_O_EXITKILL       (1 << 20)  // 追踪器退出时杀死被追踪进程
#define PTRACE_O_SUSPEND_SECCOMP (1 << 21) // 暂停 seccomp 过滤
```

**PTRACE_O_TRACESYSGOOD 的作用：**
- 设置此选项后，系统调用追踪停止时发送的信号为 `SIGTRAP | 0x80`
- 这使追踪器可以区分系统调用停止（`SIGTRAP | 0x80`）和普通信号停止（`SIGTRAP`）

## 9. 冻结机制（ptrace_freeze_traced）

在 ptrace 操作期间，内核使用冻结机制确保被追踪进程不会意外改变状态：

```c
// kernel/ptrace.c:184
static bool ptrace_freeze_traced(struct task_struct *task)
{
    bool ret = false;

    /* Lockless, nobody but us can set this flag */
    if (task->jobctl & JOBCTL_LISTENING)
        return ret;

    spin_lock_irq(&task->sighand->siglock);
    if (task_is_traced(task) && !looks_like_a_spurious_pid(task) &&
        !__fatal_signal_pending(task)) {
        task->jobctl |= JOBCTL_PTRACE_FROZEN;
        ret = true;
    }
    spin_unlock_irq(&task->sighand->siglock);

    return ret;
}
```

**冻结的作用：**
- 防止在 ptrace 操作期间被追踪进程被意外唤醒
- 确保 ptrace_check_attach 返回后，进程处于安全状态
- 操作完成后通过 ptrace_unfreeze_traced 解冻

## 10. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-ESRCH` | 进程不存在 | 指定的 PID 无效，或进程已退出 |
| `-EPERM` | 权限不足 | 无 CAP_SYS_PTRACE 或非同一用户，或附加到内核线程 |
| `-EINVAL` | 无效参数 | request 无效 / 地址无效 / 信号无效 |
| `-EIO` | IO 错误 | 内存访问无效 / 寄存器访问失败 / SEIZE 时 addr 非零 |
| `-EBUSY` | 资源忙 | 进程已被其他追踪器附加 |
| `-EFAULT` | 内存错误 | 用户空间地址不可访问 |
| `-ERESTARTNOINTR` | 信号中断 | 等待 cred_guard_mutex 时被信号中断 |

## 11. 使用示例

### 11.1 基本调试器框架

```c
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

// 断点指令（ARM64 BRK #0）
#define ARM64_BRK_INSTR 0xd4200000

int main(int argc, char *argv[])
{
    pid_t pid;
    int status;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <program>\n", argv[0]);
        return 1;
    }

    pid = fork();
    if (pid == 0) {
        /* 子进程：被调试进程 */
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);  // 等待父进程准备好
        execvp(argv[1], &argv[1]);
        perror("execvp");
        _exit(1);
    }

    /* 父进程：调试器 */
    waitpid(pid, &status, 0);

    /* 设置追踪系统调用选项 */
    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);

    /* 读取寄存器 */
    struct user_regs_struct regs;
    ptrace(PTRACE_GETREGS, pid, 0, &regs);

    printf("Child PID: %d, initial PC: 0x%lx\n", pid, regs.pc);

    /* 在入口点设置断点（简化示例，实际 GDB 需要分析 ELF） */
    /* 假设我们要在地址 0x4004a0 设断点 */
    unsigned long bp_addr = 0x4004a0;
    unsigned long orig_instr = ptrace(PTRACE_PEEKTEXT, pid, bp_addr, 0);
    ptrace(PTRACE_POKEDATA, pid, bp_addr, ARM64_BRK_INSTR);

    /* 恢复执行 */
    ptrace(PTRACE_CONT, pid, 0, 0);

    /* 调试循环 */
    while (1) {
        pid_t w = waitpid(pid, &status, 0);
        if (w == -1) {
            perror("waitpid");
            break;
        }

        if (WIFEXITED(status)) {
            printf("Child exited with status %d\n", WEXITSTATUS(status));
            break;
        }

        if (WIFSIGNALED(status)) {
            printf("Child killed by signal %d\n", WTERMSIG(status));
            break;
        }

        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);

            if (sig == (SIGTRAP | 0x80)) {
                /* 系统调用追踪停止 */
                printf("System call stop\n");
                /* 读取系统调用信息 */
                ptrace(PTRACE_GETREGS, pid, 0, &regs);
                printf("  x8 (syscall nr): %ld\n", regs.regs[8]);
                ptrace(PTRACE_SYSCALL, pid, 0, 0);
            } else if (sig == SIGTRAP) {
                /* 断点或单步停止 */
                printf("Breakpoint hit\n");

                /* 恢复原始指令 */
                ptrace(PTRACE_POKEDATA, pid, bp_addr, orig_instr);

                /* 读取寄存器 */
                ptrace(PTRACE_GETREGS, pid, 0, &regs);
                printf("  PC: 0x%lx\n", regs.pc);

                /* 继续执行 */
                ptrace(PTRACE_CONT, pid, 0, 0);
            } else if (sig == SIGSTOP) {
                printf("Initial stop\n");
                ptrace(PTRACE_CONT, pid, 0, 0);
            } else {
                /* 其他信号 */
                printf("Signal %d received\n", sig);
                ptrace(PTRACE_CONT, pid, 0, sig);
            }
        }
    }

    return 0;
}
```

### 11.2 ARM64 系统调用追踪示例

```c
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        /* 子进程 */
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        /* 执行要追踪的程序 */
        execl("/bin/ls", "ls", "-l", NULL);
        _exit(1);
    }

    /* 父进程 */
    waitpid(pid, &status, 0);
    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);

    /* 追踪系统调用 */
    ptrace(PTRACE_SYSCALL, pid, 0, 0);

    while (1) {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;

        if (WSTOPSIG(status) == (SIGTRAP | 0x80)) {
            /* 系统调用入口/出口停止 */
            struct user_regs_struct regs;
            ptrace(PTRACE_GETREGS, pid, 0, &regs);

            /* ARM64 约定：
             * x7 == 0: 系统调用入口
             * x7 == 1: 系统调用出口
             * x8: 系统调用号
             * x0-x5: 参数（入口）/ 返回值（出口）
             */
            if (regs.regs[7] == 0) {
                /* 入口 */
                printf("syscall entry: nr=%ld, args=(%ld, %ld, %ld)\n",
                       regs.regs[8],
                       regs.regs[0], regs.regs[1], regs.regs[2]);
            } else {
                /* 出口 */
                printf("syscall exit: nr=%ld, ret=%ld\n",
                       regs.regs[8], regs.regs[0]);
            }

            ptrace(PTRACE_SYSCALL, pid, 0, 0);
        }
    }

    return 0;
}
```

## 12. 参考

- 源码位置：
  - `kernel/ptrace.c` — ptrace 系统调用核心实现
  - `arch/arm64/kernel/ptrace.c` — ARM64 架构特定实现
  - `arch/arm64/kernel/syscall.c` — ARM64 系统调用入口/出口
  - `kernel/signal.c` — ptrace_stop、ptrace_signal 等信号处理
  - `kernel/exit.c` — wait4 系统调用、ptrace_do_wait
  - `include/linux/ptrace.h` — 内核 ptrace 接口和标志定义
  - `include/uapi/linux/ptrace.h` — 用户态 ptrace 接口定义
- 相关文档：[ARM64 系统调用表](../arm64-syscall-table.md#BPF 与追踪)