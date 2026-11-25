# syscall

## 原理与功能

`syscall` 是一个间接系统调用机制，允许通过系统调用编号调用任意系统调用。在 ARM64 架构上，`syscall` 没有独立的系统调用编号，而是通过 `svc #0` 指令触发系统调用，系统调用号通过寄存器 `x8` 传递。

此接口主要用于调试和特殊场景，特别是当某些系统调用没有对应的 glibc 封装函数时。

### 功能说明

- 通过系统调用编号直接调用任意系统调用
- 可用于调用没有标准库封装的系统调用
- 通常通过 `syscall()` 库函数（glibc 提供的通用接口）使用

## 用户态调用方式

### 使用 glibc 的 syscall() 函数

```c
#include <sys/syscall.h>
#include <unistd.h>

// 通用原型
long syscall(long number, ...);

// 示例：直接调用 getpid（虽然 glibc 提供了封装）
long pid = syscall(SYS_getpid);

// 调用没有 glibc 封装的系统调用
long ret = syscall(__NR_cacheflush, addr, len, flags);
```

### ARM64 架构的汇编调用

```asm
; ARM64 系统调用约定
; x8 = 系统调用号
; x0-x5 = 参数 1-6
; 返回值在 x0

    mov x8, #__NR_getpid    ; 设置系统调用号
    svc #0                   ; 触发系统调用
    ; 返回后 x0 包含返回值
```

## 系统调用过程

### ARM64 系统调用入口

```
用户态调用 syscall(SYS_getpid)
  │
  ▼
glibc 的 syscall() 包装函数
  │
  ├─ 将参数存入 x0-x5 寄存器
  ├─ 将系统调用号存入 x8 寄存器
  └─ 执行 SVC #0 指令
       │
       ▼
内核态: el0_svc 异常向量入口
  │
  ├─ 保存寄存器状态到 pt_regs
  │
  ├─ 检查系统调用号是否有效
  │    └─ 无效 → 返回 -ENOSYS
  │
  ├─ 通过 sys_call_table 查找处理函数
  │    └─ sys_call_table[nr](regs)
  │
  ├─ 执行系统调用
  │
  └─ 返回用户态
       │
       ▼
glibc 处理返回值
  │
  ├─ 如果返回值在 -4095~-1 范围，设置 errno
  └─ 返回给调用者
```

### ARM64 系统调用表

```c
// arch/arm64/kernel/syscall.c
// 系统调用表定义
void *sys_call_table[__NR_syscalls] = {
    [0 ... __NR_syscalls-1] = __arm64_sys_ni_syscall,
#include <asm/syscall_table_32.h>
};

// 未实现的系统调用返回 -ENOSYS
asmlinkage long __arm64_sys_ni_syscall(const struct pt_regs *__unused)
{
    return -ENOSYS;
}
```

## 关键数据结构

### pt_regs（ARM64）

```c
// arch/arm64/include/asm/ptrace.h
struct pt_regs {
    union {
        struct user_pt_regs user_regs;
        struct {
            u64 regs[31];    // x0-x30 通用寄存器
            u64 sp;          // 堆栈指针
            u64 pc;          // 程序计数器
            u64 pstate;      // 处理器状态
        };
    };
    u64 orig_x0;             // 保存原始的 x0（用于系统调用重启）
    u64 syscallno;           // 系统调用号
    u64 sdei_ttbr1;          // SDEI 相关
    // ...
};
```

### 系统调用表

```c
// 系统调用表类型：函数指针数组
typedef long (*syscall_fn_t)(const struct pt_regs *regs);
extern syscall_fn_t sys_call_table[];
```

## 与 libc 封装的关系

| 系统调用 | glibc 封装 | 说明 |
|--|--|--|
| `getpid` | `getpid()` | 有封装 |
| `write` | `write()` | 有封装 |
| `cacheflush` | 无 | 需使用 `syscall()` |
| `arc_gettls` | 无 | 架构专用 |
| `riscv_hwprobe` | 无 | 架构专用 |

## 使用场景

- **调用没有 glibc 封装的系统调用**：如 `cacheflush`、`riscv_flush_icache` 等
- **进行系统调用基准测试**：直接测量特定系统调用的性能
- **调试和逆向工程**：直接触发系统调用进行调试
- **跨架构兼容代码**：使用统一接口调用不同架构的系统调用
- **内核开发测试**：验证新系统调用的正确性

## 使用示例

```c
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

// 调用没有 glibc 封装的 cacheflush
int my_cacheflush(void *addr, size_t size, int flags)
{
    long ret = syscall(__NR_cacheflush, addr, size, flags);
    if (ret == -1) {
        return -errno;
    }
    return 0;
}

// 直接调用 getpid
pid_t my_getpid(void)
{
    return syscall(SYS_getpid);
}

int main(void)
{
    printf("PID: %d\n", my_getpid());
    return 0;
}
```

## 注意事项

- ARM64 上无独立系统调用号，"syscall" 指的是通用的 `svc` 指令机制
- `syscall()` 是 libc 提供的通用系统调用接口，适用于所有架构
- 直接使用系统调用号会降低可移植性，不同架构的编号可能不同
- 系统调用号定义在 `<asm/unistd.h>` 或 `<sys/syscall.h>` 中
- 返回值处理：负数表示错误（`-errno`），零或正数表示成功

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/arm64/kernel/syscall.c](/home/louis/code/linux/arch/arm64/kernel/syscall.c) | ARM64 系统调用入口 |
| [arch/arm64/include/asm/ptrace.h](/home/louis/code/linux/arch/arm64/include/asm/ptrace.h) | pt_regs 结构定义 |
| [include/uapi/asm-generic/unistd.h](/home/louis/code/linux/include/uapi/asm-generic/unistd.h) | 通用系统调用编号 |
| [include/linux/syscalls.h](/home/louis/code/linux/include/linux/syscalls.h) | 系统调用声明宏 |