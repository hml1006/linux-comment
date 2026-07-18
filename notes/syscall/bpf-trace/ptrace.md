# ptrace 系统调用分析

## 1. 概述

`ptrace` 提供了一种进程追踪机制，允许一个进程（追踪器）观察和控制另一个进程（被追踪者）的执行。它是调试器（如 GDB）和系统调用追踪工具（如 strace）的核心基础设施。

**原型：**

```c
SYSCALL_DEFINE4(ptrace, long, request, long, pid, unsigned long, addr,
    unsigned long, data)
```

**参数：**
- `request`：要执行的 ptrace 操作
- `pid`：目标进程的 PID
- `addr`：操作相关的地址
- `data`：操作相关的数据

## 2. 主要 request 操作

| 操作 | 功能 |
|------|------|
| `PTRACE_TRACEME` | 当前进程声明被其父进程追踪 |
| `PTRACE_ATTACH` | 附加到指定进程 |
| `PTRACE_SEIZE` | 附加到进程（不停止进程） |
| `PTRACE_DETACH` | 分离追踪 |
| `PTRACE_PEEKDATA` | 读取内存数据 |
| `PTRACE_POKEDATA` | 写入内存数据 |
| `PTRACE_GETREGS` | 获取寄存器 |
| `PTRACE_SETREGS` | 设置寄存器 |
| `PTRACE_GETFPREGS` | 获取浮点寄存器 |
| `PTRACE_CONT` | 继续执行 |
| `PTRACE_SYSCALL` | 追踪系统调用入口和出口 |
| `PTRACE_SINGLESTEP` | 单步执行 |
| `PTRACE_KILL` | 发送信号 |
| `PTRACE_INTERRUPT` | 中断被追踪进程 |
| `PTRACE_LISTEN` | 监听被追踪进程 |
| `PTRACE_GETEVENTMSG` | 获取事件消息 |
| `PTRACE_GETSIGINFO` | 获取信号信息 |
| `PTRACE_SETSIGINFO` | 设置信号信息 |
| `PTRACE_SETOPTIONS` | 设置追踪选项 |
| `PTRACE_GET_RSEQ_CONFIGURATION` | 获取可重启序列配置 |

## 3. 函数调用栈

```
ptrace(request, pid, addr, data)                         // kernel/ptrace.c
  ├─ [PTRACE_TRACEME] → ptrace_traceme()
  │    └─ 设置 current->ptrace 标志，标记为被追踪
  │
  ├─ [PTRACE_ATTACH | PTRACE_SEIZE] → ptrace_attach()
  │    ├─ find_get_task_by_vpid(pid) → 查找目标进程
  │    ├─ __ptrace_may_access() → 权限检查
  │    │    └─ 检查 CAP_SYS_PTRACE 或相同用户/命名空间
  │    ├─ ptrace_link(child, current) → 建立追踪关系
  │    └─ 发送 SIGSTOP 信号（ATTACH）或 wake up（SEIZE）
  │
  ├─ 其他操作:
  │    ├─ ptrace_check_attach() → 确认被追踪进程已停止
  │    └─ arch_ptrace(child, request, addr, data) → 架构特定操作
  │         ├─ PTRACE_PEEKDATA → ptrace_peek_data()
  │         ├─ PTRACE_POKEDATA → ptrace_poke_data()
  │         ├─ PTRACE_GETREGS → ptrace_getregs()
  │         ├─ PTRACE_CONT → ptrace_resume()
  │         ├─ PTRACE_SYSCALL → ptrace_resume(...,TRACE_SYSCALL)
  │         └─ PTRACE_SINGLESTEP → ptrace_resume(...,TRACE_STEP)
  │
  └─ 返回请求结果
```

## 4. 关键数据结构

### 4.1 struct task_struct 的 ptrace 相关字段

```c
// include/linux/sched.h
struct task_struct {
    unsigned int ptrace;            // ptrace 标志位
    unsigned int ptrace_event;      // ptrace 事件
    struct list_head ptrace_entry;  // ptrace 链表节点
    struct task_struct *parent;     // 追踪器（ptracer）
    const struct cred *ptracer_cred; // 追踪器凭证
};
```

### 4.2 ptrace 标志位

```c
// include/linux/ptrace.h
#define PT_PTRACED       0x00000001  // 被 ptrace 追踪
#define PT_TRACESYSGOOD  0x00000002  // 系统调用追踪时设置
#define PT_DTRACE        0x00000004  // 延迟追踪
#define PT_SEIZED        0x00000080  // 通过 SEIZE 附加
#define PT_PTRACE_CAP    0x00000100  // 使用 CAP_SYS_PTRACE 附加
```

## 5. 流程图

```
用户态: ptrace(PTRACE_ATTACH, pid, NULL, 0)
    │
    v
┌─────────────────────────────────────┐
│ find_get_task_by_vpid(pid)          │
│ 查找目标进程                        │
│ 不存在 → 返回 -ESRCH                │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ __ptrace_may_access(child)          │
│ 检查权限:                           │
│ 1. 相同用户?                        │
│ 2. CAP_SYS_PTRACE?                  │
│ 3. LSM 允许?                        │
│ 失败 → 返回 -EPERM                  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ ptrace_link(child, current)         │
│ 建立追踪关系:                       │
│ child->parent = current             │
│ child->ptrace |= PT_PTRACED         │
│ list_add(child->ptrace_entry, ...)  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 发送 SIGSTOP 给目标进程             │
│ 等待目标进程停止                    │
└─────────────────────────────────────┘
    │
    v
返回 0 (成功)

--- 追踪器随后可以执行以下操作 ---
    │
    v
┌─────────────────────────────────────┐
│ ptrace(PTRACE_CONT, pid, 0, 0)     │
│ → ptrace_resume(child, request, 0) │
│    设置 child->exit_code = data    │
│    唤醒被追踪进程                   │
└─────────────────────────────────────┘

┌─────────────────────────────────────┐
│ ptrace(PTRACE_SYSCALL, pid, 0, 0) │
│ → ptrace_resume(child,             │
│      PTRACE_SYSCALL, 0)            │
│ 每次系统调用入口/出口都会停止      │
└─────────────────────────────────────┘

┌─────────────────────────────────────┐
│ ptrace(PTRACE_GETREGS, pid, ...)   │
│ → arch_ptrace → ptrace_getregs()   │
│ 拷贝寄存器到用户空间               │
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-ESRCH` | 进程不存在 | 指定的 PID 无效 |
| `-EPERM` | 权限不足 | 无 CAP_SYS_PTRACE 或非同一用户 |
| `-EINVAL` | 无效参数 | request 无效 / 地址无效 |
| `-EIO` | IO 错误 | 内存访问无效 / 寄存器访问失败 |
| `-EBUSY` | 资源忙 | 进程已被其他追踪器附加 |
| `-EFAULT` | 内存错误 | 用户空间地址不可访问 |

## 7. 使用示例

```c
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* 子进程：被追踪 */
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);  // 等待父进程附加

        printf("Child: running...\n");
        /* 执行要追踪的操作 */
        _exit(0);
    }

    /* 父进程：追踪器 */
    int status;
    waitpid(pid, &status, 0);

    /* 附加到子进程 */
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) {
        perror("PTRACE_ATTACH");
        return 1;
    }

    waitpid(pid, &status, 0);

    /* 设置追踪系统调用 */
    ptrace(PTRACE_SETOPTIONS, pid, 0,
           PTRACE_O_TRACESYSGOOD);

    printf("Tracing syscalls of PID %d...\n", pid);

    /* 追踪几次系统调用 */
    for (int i = 0; i < 5; i++) {
        /* 系统调用入口 */
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
        waitpid(pid, &status, 0);

        /* 系统调用出口 */
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
        waitpid(pid, &status, 0);
    }

    /* 分离 */
    ptrace(PTRACE_DETACH, pid, 0, 0);
    printf("Detached from PID %d\n", pid);

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/ptrace.c`
- 架构特定实现：`arch/*/kernel/ptrace.c`
- 头文件：`include/uapi/linux/ptrace.h`, `include/linux/ptrace.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#BPF 与追踪)