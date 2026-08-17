# map_shadow_stack 系统调用分析

## 1. 概述

`map_shadow_stack` 系统调用用于分配影子栈（Shadow Stack）内存，是硬件辅助的控制流完整性（CFI）机制的一部分。影子栈用于存储返回地址，防止 ROP（Return-Oriented Programming）攻击。

**内核源码位置：**
- x86: `arch/x86/kernel/shstk.c`
- ARM64: `arch/arm64/mm/gcs.c`
- RISC-V: `arch/riscv/kernel/usercfi.c`

**原型：**

```c
SYSCALL_DEFINE3(map_shadow_stack, unsigned long, addr, unsigned long, size, unsigned int, flags)
```

| 参数 | 描述 |
|------|------|
| `addr` | 期望的映射地址（0 表示由内核选择） |
| `size` | 映射大小（字节，必须 8 字节对齐且不为 8） |
| `flags` | 标志位（见下方） |

**返回值：**
- 成功返回映射的起始地址
- 失败返回负数错误码

**flags 值：**

| 标志 | 描述 |
|------|------|
| `SHADOW_STACK_SET_TOKEN` | 在栈顶写入恢复令牌（restore token） |
| `SHADOW_STACK_SET_MARKER` | 在栈顶预留空帧作为栈顶标记（仅 ARM64 GCS） |

## 2. 使用场景

- **硬件安全机制初始化**：为线程分配影子栈，用于 CET（Control-flow Enforcement Technology）/ GCS（Guarded Control Stack）
- **动态影子栈分配**：当线程需要额外影子栈空间时使用
- **sigreturn 处理**：信号处理需要切换影子栈

## 3. 函数调用链分析

### x86 架构

```
map_shadow_stack(addr, size, flags)                   // 系统调用入口
  ├─ cpu_feature_enabled(X86_FEATURE_USER_SHSTK)     // 检查 CPU 特性
  ├─ 参数验证
  │    ├─ flags 检查（仅支持 SHADOW_STACK_SET_TOKEN）
  │    ├─ 如果 set_tok，size 必须 >= 8
  │    └─ 如果 addr 非零，必须 >= 4G（MAP_ABOVE4G）
  └─ alloc_shstk(addr, aligned_size, size, set_tok)
       ├─ do_mmap(NULL, addr, size, PROT_READ,
       │     MAP_ANONYMOUS|MAP_PRIVATE|MAP_ABOVE4G,
       │     VM_SHADOW_STACK|VM_WRITE, ...)           // 创建影子栈映射
       └─ create_rstor_token(addr + token_offset)     // 写入恢复令牌
```

### ARM64 架构（GCS）

```
map_shadow_stack(addr, size, flags)                   // 系统调用入口
  ├─ system_supports_gcs()                           // 检查 GCS 支持
  ├─ 参数验证
  │    ├─ flags 检查（SHADOW_STACK_SET_TOKEN | SHADOW_STACK_SET_MARKER）
  │    ├─ addr 必须页对齐
  │    └─ size 必须 8 字节对齐且不为 8
  └─ alloc_gcs(addr, alloc_size)                     // 分配 GCS 内存
       └─ do_mmap(NULL, addr, size, PROT_READ,
             MAP_ANONYMOUS|MAP_PRIVATE,
             VM_SHADOW_STACK|VM_WRITE, ...)
       └─ 写入 GCS_CAP 令牌（如果 SHADOW_STACK_SET_TOKEN）
            └─ gcsb_dsync()                          // 确保顺序一致性
```

## 4. 关键数据结构

### x86 影子栈相关

```c
/* 影子栈 VMA 标志 */
#define VM_SHADOW_STACK  VM_HIGH_ARCH_5  /* 影子栈 VMA 标志位 */

/* x86 线程影子栈结构 */
struct thread_shstk {
    u64 base;           /* 影子栈基地址 */
    u64 size;           /* 影子栈大小 */
};

/* MSR 寄存器 */
#define MSR_IA32_PL3_SSP  0x000006a7  /* 权限级别 3 的影子栈指针 */
#define MSR_IA32_U_CET    0x000006a8  /* 用户态 CET 配置 */
```

### ARM64 GCS 相关

```c
/* GCS 能力令牌结构 */
#define GCS_CAP(v)  ((v) | 0x1)        /* 标记 GCS 能力令牌 */

/* 线程 GCS 相关字段 */
struct thread_struct {
    unsigned long gcs_base;    /* GCS 基地址 */
    unsigned long gcs_size;    /* GCS 大小 */
    unsigned long gcspr_el0;   /* GCS 指针寄存器值 */
};
```

## 5. 流程图

```
  用户态调用 map_shadow_stack(addr, size, flags)
         │
         ▼
  ┌──────────────────────────────┐
  │  架构特定检查                │
  │  x86:  X86_FEATURE_USER_SHSTK│
  │  ARM64: system_supports_gcs()│
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  参数验证                    │
  │  ├─ flags 检查              │
  │  ├─ addr 对齐检查           │
  │  └─ size 大小检查           │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  分配影子栈内存              │
  │  do_mmap(NULL, addr, size,  │
  │    PROT_READ,               │
  │    MAP_ANONYMOUS|MAP_PRIVATE│
  │    |MAP_ABOVE4G,            │
  │    VM_SHADOW_STACK|VM_WRITE)│
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  是否设置令牌?               │
  │  flags & SHADOW_STACK_SET_  │
  │        TOKEN ?               │
  │  ├─ yes → 写入恢复令牌      │
  │  │  x86: create_rstor_token()│
  │  │  ARM64: put_user_gcs()   │
  │  └─ no  → 跳过              │
  └─────────────┬────────────────┘
                ▼
         返回映射地址
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EOPNOTSUPP` | CPU 不支持影子栈特性 |
| `-EINVAL` | flags 无效、addr 未对齐、size 无效 |
| `-ENOSPC` | 设置令牌但 size < 8 |
| `-ERANGE` | x86 上 addr 非零且 < 4G |
| `-EOVERFLOW` | size 向上页对齐后溢出 |
| `-EFAULT` | 写入令牌时发生缺页错误 |
| `-ENOMEM` | 内存不足，无法分配 VMA 或页表 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <asm/mman.h>  /* 需要 SHADOW_STACK_SET_TOKEN 等宏 */

int main() {
    unsigned long addr;
    unsigned long size = 4096;  /* 一页 */

    /* 分配影子栈，不设置令牌 */
    addr = syscall(__NR_map_shadow_stack, 0, size, 0);
    if ((long)addr < 0) {
        perror("map_shadow_stack");
        return 1;
    }
    printf("Shadow stack allocated at: 0x%lx\n", addr);

    /* 分配另一个带恢复令牌的影子栈 */
    addr = syscall(__NR_map_shadow_stack, 0, size, SHADOW_STACK_SET_TOKEN);
    if ((long)addr < 0) {
        perror("map_shadow_stack (with token)");
        return 1;
    }
    printf("Shadow stack with token at: 0x%lx\n", addr);

    return 0;
}
```

## 8. 架构差异

| 特性 | x86 (CET) | ARM64 (GCS) | RISC-V (Zicfiss) |
|------|-----------|-------------|------------------|
| VMA 标志 | `VM_SHADOW_STACK` | `VM_SHADOW_STACK` | `VM_SHADOW_STACK` |
| 地址限制 | >= 4G | 无 | 无 |
| 令牌类型 | 恢复令牌 (RSTOR) | GCS 能力令牌 (GCS_CAP) | 架构相关 |
| 额外标志 | 无 | `SHADOW_STACK_SET_MARKER` | 架构相关 |
| 内核实现 | `alloc_shstk()` | `alloc_gcs()` | 架构相关 |

## 9. 关键实现细节

1. **VM_SHADOW_STACK 标志**：影子栈 VMA 使用特殊的 `VM_SHADOW_STACK` 标志，内核在页表权限处理中识别此标志，确保影子栈页具有特殊的内存属性（不可执行、不可直接写入用户代码）。

2. **MAP_ABOVE4G**：x86 上影子栈必须分配在 4G 以上地址空间，这是为了与 32 位兼容模式隔离。

3. **恢复令牌**：影子栈末尾的令牌是一个特殊值，包含指向自身的指针，用于在函数返回时验证影子栈的完整性。

4. **do_mmap 直接调用**：`map_shadow_stack` 直接调用 `do_mmap()` 而非 `mmap` 系统调用，绕过了用户态接口的权限检查，但内核会设置特殊的 VMA 标志。

5. **与线程创建的关系**：新线程创建时，内核会自动为其分配影子栈（通过 `gcs_alloc_thread_stack()` 或 `shstk_setup()`），无需用户手动调用 `map_shadow_stack`。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`arch/x86/kernel/shstk.c`
- 内核源码：`arch/arm64/mm/gcs.c`
- 内核源码：`arch/riscv/kernel/usercfi.c`
- Intel CET 技术规范
- ARM GCS (Guarded Control Stack) 架构扩展