# rt_sigreturn 系统调用分析

## 1. 概述

`rt_sigreturn()` 用于从信号处理函数返回。当用户态信号处理函数执行完毕后，通过此系统调用恢复到信号发生前的执行上下文（包括寄存器状态、信号掩码、备用栈等）。该调用永远不会返回，它直接恢复之前的上下文并跳转到被中断的执行点。

**原型：**

```c
SYSCALL_DEFINE0(rt_sigreturn)   // 无参数，栈帧已由 setup_rt_frame 设置
```

## 2. 使用场景

- 信号处理函数返回：在用户态信号处理函数执行完毕后自动调用
- 恢复执行上下文：恢复被信号中断时的寄存器状态和信号掩码
- sigaltstack 恢复：如果使用替代信号栈，返回到原栈

## 3. 函数调用链（ARM64）

```
rt_sigreturn()                                    // arch/arm64/kernel/signal.c:1089
  │
  ├─ current->restart_block.fn = do_no_restart_syscall  // 防止系统调用重启
  │
  ├─ 检查栈对齐（sp & 15 → badframe）
  │
  ├─ frame = (struct rt_sigframe __user *)regs->sp  // 获取信号帧
  │
  ├─ access_ok(frame, sizeof(*frame))              // 检查可访问性
  │
  ├─ restore_sigframe(regs, frame, &ua_state)      // 恢复寄存器上下文
  │    ├─ restore_sigcontext(regs, &frame->uc.uc_mcontext)
  │    │    ├─ 恢复通用寄存器 (x0-x30, sp, pc)
  │    │    ├─ 恢复浮点/SIMD 寄存器 (FPSR, FPCR)
  │    │    └─ 恢复 ESR（异常综合寄存器，用于信号信息）
  │    └─ 恢复用户访问状态
  │
  ├─ restore_altstack(&frame->uc.uc_stack)         // 恢复 sigaltstack 状态
  │
  └─ return regs->regs[0] (返回被中断的执行点)
```

## 4. 关键数据结构

```c
// ARM64 信号帧结构
struct rt_sigframe {
    struct siginfo info;               // 信号信息
    struct ucontext uc;                // 用户上下文
};

// 用户上下文
struct ucontext {
    unsigned long uc_flags;            // 标志
    struct ucontext *uc_link;          // 链接上下文（用于信号栈切换）
    stack_t uc_stack;                  // 信号栈信息
    sigset_t uc_sigmask;               // 信号掩码
    struct sigcontext uc_mcontext;     // 机器上下文（寄存器）
};

// ARM64 信号上下文
struct sigcontext {
    __u64 fault_address;               // 错误地址（用于 SIGSEGV/SIGBUS）
    __u32 header; / __u32 error_code;  // 头部 / 错误码
    struct _aarch64_ctx *regs;         // 寄存器上下文
};

// 信号帧的内存布局（栈上）：
// 高地址
// +---------------------+
// | siginfo_t           |  信号信息
// +---------------------+
// | ucontext:           |
// |   uc_sigmask        |  信号掩码
// |   uc_stack          |  栈信息
// |   uc_mcontext       |  寄存器上下文
// +---------------------+  ← sp
// 低地址
```

## 5. 流程图

```
信号处理函数执行完毕
    │
    │  (通过 sigreturn 汇编指令或 vDSO)
    ▼
内核入口: rt_sigreturn()
    │
    ├─ 重置 restart_block（防止系统调用被重启）
    │
    ├─ 从栈指针获取 rt_sigframe 地址
    │   frame = (struct rt_sigframe *)regs->sp
    │
    ├─ 验证栈帧可访问性
    │
    ├─ restore_sigframe()
    │   │
    │   ├─ 恢复通用寄存器 (x0-x30, sp, pc)
    │   ├─ 恢复浮点/SIMD 寄存器 (FPSR, FPCR)
    │   ├─ 恢复 ESR 寄存器
    │   └─ 恢复用户访问权限状态
    │
    ├─ restore_altstack()
    │   │
    │   └─ 若使用过替代信号栈，恢复原始栈
    │
    └─ return regs->regs[0]  // 返回到被中断的代码处
         │
         (直接跳转到被中断时的 PC，恢复完整上下文)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| SIGSEGV | 段错误 | 栈帧不可访问、栈对齐错误、寄存器恢复失败 |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

/* 信号处理函数 */
void handler(int sig)
{
    /* 信号处理完毕后，会自动执行 sigreturn 系统调用 */
    printf("Signal %d caught\n", sig);
    /* 简单的 write 是异步信号安全的 */
    write(STDOUT_FILENO, "Handler done\n", 13);
}

int main(void)
{
    struct sigaction sa;
    sa.sa_handler = handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);

    printf("Waiting for signal...\n");
    pause();
    printf("Returned from signal handler\n");

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- arch/arm64/kernel/signal.c:`rt_sigreturn()` - ARM64 实现
- arch/arm64/kernel/signal.c:`restore_sigframe()` - 信号帧恢复
- arch/arm64/kernel/signal.c:`setup_rt_frame()` - 信号帧创建（对应操作）