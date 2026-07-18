# sigaltstack 系统调用分析

## 1. 概述

`sigaltstack()` 用于设置或查询进程的替代信号栈（alternate signal stack）。当信号处理函数设置了 `SA_ONSTACK` 标志时，内核将在替代栈而非默认栈上执行信号处理函数。这在默认栈空间不足时非常有用（如 SIGSEGV 处理）。

**原型：**

```c
SYSCALL_DEFINE2(sigaltstack, const stack_t __user *, uss, stack_t __user *, uoss)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| uss | stack_t* | 新的替代栈配置（可为 NULL，只查询） |
| uoss | stack_t* | 输出旧的替代栈配置（可为 NULL） |

## 2. 使用场景

- 处理栈溢出信号：当进程栈空间不足时，SIGSEGV 无法在默认栈上处理
- 协程/用户态线程：为信号处理提供独立的栈空间
- 安全关键代码：确保信号处理函数在可控的栈上执行
- 深度嵌套信号处理：避免在已近溢出的栈上执行信号处理

## 3. 函数调用链

```
sigaltstack(uss, uoss)                             // kernel/signal.c:4444
  │
  ├─ 若 uss 非空：copy_from_user(&new, uss, sizeof(stack_t))
  │
  └─ do_sigaltstack(&new, &old, sp, MINSIGSTKSZ)   // kernel/signal.c:4387
       │
       ├─ 若 oss 非空：
       │   ├─ oss->ss_sp = t->sas_ss_sp
       │   ├─ oss->ss_size = t->sas_ss_size
       │   └─ oss->ss_flags = sas_ss_flags(sp) | SS_FLAG_BITS
       │
       └─ 若 ss 非空：
            ├─ on_sig_stack(sp) → -EPERM  // 不能在替代栈上设置替代栈
            │
            ├─ 验证 ss_flags 合法性：
            │   ├─ SS_DISABLE → 禁用替代栈
            │   ├─ SS_ONSTACK → 保留
            │   └─ 0 → 正常设置
            │
            ├─ 若 ss_mode == SS_DISABLE：
            │   ├─ ss_size = 0
            │   └─ ss_sp = NULL
            │
            ├─ 若 ss_mode == 0：
            │   ├─ ss_size < MINSIGSTKSZ → -ENOMEM
            │   └─ sigaltstack_size_valid() 检查
            │
            └─ 更新 task_struct 字段：
                 ├─ t->sas_ss_sp = (unsigned long) ss_sp
                 ├─ t->sas_ss_size = ss_size
                 └─ t->sas_ss_flags = ss_flags
```

## 4. 关键数据结构

```c
// 用户态的栈描述结构
typedef struct {
    void  *ss_sp;        // 栈基址（起始地址）
    int    ss_flags;     // 标志：SS_DISABLE / SS_ONSTACK / 0
    size_t ss_size;      // 栈大小
} stack_t;

// task_struct 中的替代栈字段
struct task_struct {
    unsigned long sas_ss_sp;     // 替代栈地址（无符号长整型）
    size_t        sas_ss_size;   // 替代栈大小
    unsigned int  sas_ss_flags;  // 替代栈标志
};

// 栈标志
#define SS_DISABLE  2   // 禁用替代栈
#define SS_ONSTACK  1   // 当前正在使用替代栈（内核设置，用户只读）
#define SS_AUTODISARM (1U << 31)  // 自动禁用（信号处理后恢复）

// 最小栈大小
#define MINSIGSTKSZ  2048     // 最小替代栈大小（x86: 2048，ARM64: 4096）
#define SIGSTKSZ     8192     // 推荐的典型替代栈大小
```

## 5. 流程图

```
用户态: sigaltstack(&new_stack, &old_stack)
    │
    ▼
SYSCALL_DEFINE2(sigaltstack)
    │
    ├─ 从用户空间拷贝 new_stack（若提供）
    │
    └─ do_sigaltstack(ss, oss, sp, MINSIGSTKSZ)
         │
         ├─ [查询旧栈] 若 oss != NULL:
         │   ├─ ss_sp  = current->sas_ss_sp
         │   ├─ ss_size = current->sas_ss_size
         │   └─ ss_flags = sas_ss_flags(sp) | sas_ss_flags
         │
         └─ [设置新栈] 若 ss != NULL:
              │
              ├─ 检查是否在替代栈上 → -EPERM
              │
              ├─ 验证 ss_flags 模式
              │
              ├─ SS_DISABLE:
              │   └─ 清除替代栈 (sp=NULL, size=0)
              │
              └─ 正常模式 (0):
                   ├─ 检查 ss_size >= MINSIGSTKSZ → -ENOMEM
                   ├─ 检查栈大小有效性
                   └─ 更新 task_struct 字段
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EPERM | 操作不允许 | 当前正在替代栈上执行（on_sig_stack） |
| EINVAL | 无效参数 | ss_flags 包含无效值 |
| ENOMEM | 栈太小 | ss_size < MINSIGSTKSZ 或 sigaltstack_size_valid 失败 |
| EFAULT | 内存错误 | uss/uoss 指针指向不可访问的内存 |

## 7. 使用示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define ALTERNATE_STACK_SIZE 65536

static char alt_stack[ALTERNATE_STACK_SIZE];

void segv_handler(int sig, siginfo_t *info, void *context)
{
    /* 在替代栈上处理 SIGSEGV */
    write(STDOUT_FILENO, "Caught SIGSEGV on alternate stack!\n", 35);
    _exit(1);
}

int main(void)
{
    stack_t ss;
    struct sigaction sa;

    /* 设置替代信号栈 */
    ss.ss_sp = alt_stack;
    ss.ss_size = ALTERNATE_STACK_SIZE;
    ss.ss_flags = 0;

    if (sigaltstack(&ss, NULL) == -1) {
        perror("sigaltstack");
        return 1;
    }

    /* 设置信号处理函数，使用 SA_ONSTACK */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);

    /* 触发 SIGSEGV */
    printf("Triggering SIGSEGV...\n");
    *(volatile int *)0 = 0;

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#信号处理)
- kernel/signal.c:`do_sigaltstack()` - 核心实现
- kernel/signal.c:`sas_ss_flags()` - 替代栈标志检查
- include/uapi/linux/signal.h - stack_t 及标志定义