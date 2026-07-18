# or1k_atomic

## 原理与功能

`or1k_atomic` 是 OpenRISC 架构专用的系统调用，用于提供用户态原子操作能力。OpenRISC 1000 架构通过此系统调用实现原子交换（XCHG）等操作，因为其用户态没有直接的原子指令。

在 ARM64 架构上，此系统调用存在于编号表中，但仅为 OpenRISC 架构提供支持，ARM64 上为 stub 实现（返回 -ENOSYS）。

### 功能说明

- 提供用户态原子操作（目前仅实现 XCHG 交换）
- OpenRISC 架构编号为 244（`__NR_or1k_atomic`）

## 函数原型

```c
asmlinkage long sys_or1k_atomic(unsigned long type, unsigned long *v1,
                                unsigned long *v2);
```

| 参数 | 类型 | 描述 |
|--|--|--|
| `type` | `unsigned long` | 原子操作类型标识（目前仅支持 1 = XCHG） |
| `v1` | `unsigned long *` | 第一个操作数指针 |
| `v2` | `unsigned long *` | 第二个操作数指针 |

### 返回值

- 成功时返回 0
- 错误时返回负的错误码

## 完整实现（汇编）

```asm
; arch/openrisc/kernel/entry.S
ENTRY(sys_or1k_atomic)
    ; FIXME: This ignores r3 and always does an XCHG
    DISABLE_INTERRUPTS(r17, r19)    ; 关闭中断以保证原子性
    l.lwz   r29, 0(r4)              ; 加载 *v1 到 r29
    l.lwz   r27, 0(r5)              ; 加载 *v2 到 r27
    l.sw    0(r4), r27               ; 将 r27 (原*v2) 写入 *v1
    l.sw    0(r5), r29               ; 将 r29 (原*v1) 写入 *v2
    ENABLE_INTERRUPTS(r17)           ; 重新开启中断
    l.jr    r9                       ; 返回
    l.or    r11, r0, r0              ; 设置返回值为 0
```

## 调用链分析

```
sys_or1k_atomic(type, v1, v2)
  │
  ├─ DISABLE_INTERRUPTS()   // 关闭中断（原子性保证）
  ├─ l.lwz r29, 0(v1)       // tmp1 = *v1
  ├─ l.lwz r27, 0(v2)       // tmp2 = *v2
  ├─ l.sw 0(v1), r27        // *v1 = tmp2 (原*v2)
  ├─ l.sw 0(v2), r29        // *v2 = tmp1 (原*v1)
  ├─ ENABLE_INTERRUPTS()    // 重新开启中断
  └─ return 0
```

## 关键数据结构

```c
// 定义在 arch/openrisc/include/uapi/asm/unistd.h
#define __NR_or1k_atomic (__NR_arch_specific_syscall + 0)
```

## 流程图

```
用户态调用 sys_or1k_atomic(type, v1, v2)
  │
  ▼
OpenRISC 系统调用入口 (trap)
  │
  ▼
DISABLE_INTERRUPTS()     ─── 关闭中断
  │
  ▼
tmp1 = *v1               ─── 读取 v1 处的值
  │
  ▼
tmp2 = *v2               ─── 读取 v2 处的值
  │
  ▼
*v1 = tmp2               ─── 将 v2 的值写入 v1
  │
  ▼
*v2 = tmp1               ─── 将 v1 的原值写入 v2
  │
  ▼
ENABLE_INTERRUPTS()      ─── 重新开启中断
  │
  ▼
返回 0 (成功)
```

## 原子操作机制

OpenRISC 架构使用两种方式保证原子性：

### 1. 系统调用方式（`sys_or1k_atomic`）
- 通过关闭中断来保证原子性
- 适用于单处理器系统
- 用于用户态无法直接访问的原子操作

### 2. 硬件指令方式（内核内部）
```c
// arch/openrisc/include/asm/cmpxchg.h
// 使用 l.lwa / l.swa (Load-Linked/Store-Conditional) 指令对
#define __cmpxchg(ptr, old, new)                    \
({                                                  \
    __asm__ __volatile__(                           \
        "1: l.lwa  %0, 0(%1)  \n"                  \
        "   l.sfeq %0, %2     \n"                  \
        "   l.bnf  2f         \n"                  \
        "   l.swa  0(%1), %3  \n"                  \
        "   l.bnf  1b         \n"                  \
        "2:                   \n"                  \
        : "=&r"(_prev)                              \
        : "r"(ptr), "r"(old), "r"(new)              \
        : "cc", "memory");                          \
    _prev;                                          \
})
```

## 使用场景

- 用户态需要原子交换（XCHG）操作的场景
- 用户态无锁编程
- 跨平台代码中为 OpenRISC 架构提供原子操作支持

## 与 ARM64 对比

| 特性 | OpenRISC | ARM64 |
|--|--|--|
| 原子操作 | 系统调用（关闭中断） | `LDXR`/`STXR` 指令 |
| 性能 | 较高（系统调用开销） | 极低 |
| 多核支持 | 有限 | 原生支持 |
| 系统调用编号 | 244 | stub |

## 注意事项

- ARM64 上此系统调用仅为 OpenRISC 架构兼容性保留
- ARM64 用户态通过 `LDXR`/`STXR` 指令实现原子操作，无需系统调用
- OpenRISC 的实现目前仅支持 XCHG 操作（`type` 参数被忽略）
- 由于通过关闭中断实现原子性，仅适用于单处理器场景

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/openrisc/kernel/entry.S](/home/louis/code/linux/arch/openrisc/kernel/entry.S) | or1k_atomic 汇编实现 |
| [arch/openrisc/include/asm/syscalls.h](/home/louis/code/linux/arch/openrisc/include/asm/syscalls.h) | 系统调用声明 |
| [arch/openrisc/kernel/sys_call_table.c](/home/louis/code/linux/arch/openrisc/kernel/sys_call_table.c) | 系统调用表 |