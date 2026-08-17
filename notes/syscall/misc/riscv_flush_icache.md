# riscv_flush_icache

## 原理与功能

`riscv_flush_icache` 是 RISC-V 架构专用的系统调用，用于刷写指令缓存（Instruction Cache）。尽管 RISC-V 提供了 `fence.i` 指令供用户态直接使用，但该指令仅刷新当前硬件线程（hart）的指令缓存。当 Linux 内核将进程从一个 hart 调度到另一个 hart 时，新 hart 的指令缓存可能包含过时的指令数据，用户态无法自行处理这种情况。

在 ARM64 架构上，此系统调用存在于编号表中，但仅为 RISC-V 架构提供支持，ARM64 上为 stub 实现。

### 功能说明

- 刷新指定地址范围的指令缓存
- 确保指令修改对其他核心可见
- 支持本地刷新和全局刷新两种模式
- RISC-V 架构编号为 259（`__NR_riscv_flush_icache`）

## 函数原型

```c
SYSCALL_DEFINE3(riscv_flush_icache, uintptr_t, start, uintptr_t, end,
                uintptr_t, flags);
```

| 参数 | 类型 | 描述 |
|--|--|--|
| `start` | `uintptr_t` | 要刷新的起始地址 |
| `end` | `uintptr_t` | 要刷新的结束地址 |
| `flags` | `uintptr_t` | 标志位，控制刷新范围 |

### flags 标志位

```c
// arch/riscv/include/asm/cacheflush.h
#define SYS_RISCV_FLUSH_ICACHE_LOCAL 1UL    // 仅刷新当前线程的 icache
#define SYS_RISCV_FLUSH_ICACHE_ALL   (SYS_RISCV_FLUSH_ICACHE_LOCAL)  // 刷新所有

// 注意：当前 LOCAL == ALL，因为内核总是刷新所有 hart 的 icache
```

## 完整实现

```c
// arch/riscv/kernel/sys_riscv.c
SYSCALL_DEFINE3(riscv_flush_icache, uintptr_t, start, uintptr_t, end,
                uintptr_t, flags)
{
    /* Check the reserved flags. */
    if (unlikely(flags & ~SYS_RISCV_FLUSH_ICACHE_ALL))
        return -EINVAL;

    flush_icache_mm(current->mm, flags & SYS_RISCV_FLUSH_ICACHE_LOCAL);

    return 0;
}
```

## 调用链分析

```
riscv_flush_icache(start, end, flags)
  │
  ├─ 1. 检查 flags 是否合法（不能包含未定义的标志位）
  │
  └─ 2. flush_icache_mm(current->mm, local)
       │
       ├─ 如果 local && num_online_cpus() == 1:
       │    └─ local_flush_icache_all()  // 仅本地刷新
       │
       └─ 否则:
            ├─ local_flush_icache_all()      // 刷新当前 hart
            ├─ RISCV_FENCE(w, o)              // 确保数据写操作可见
            └─ (sbi_remote_fence_i() 或 IPI)  // 通知所有其他 hart
                 └─ ipi_remote_fence_i()      // 远程 fence.i 中断
```

### 刷新流程详情

```c
// arch/riscv/mm/cacheflush.c
void flush_icache_all(void)
{
    local_flush_icache_all();  // 执行 fence.i 指令

    if (num_online_cpus() < 2)
        return;

    // 确保数据写入在触发远程 fence.i 之前可见
    RISCV_FENCE(w, o);

    if (riscv_use_sbi_for_rfence())
        sbi_remote_fence_i(NULL);   // 通过 SBI 调用远程 fence.i
    else
        on_each_cpu(ipi_remote_fence_i, NULL, 1);  // 通过 IPI 发送
}
```

## 关键数据结构

### 内核内存上下文

```c
// 每个进程的 mm_struct 在 RISC-V 上保存 icache 刷新状态
struct mm_context_t {
    // ...
    bool force_icache_flush;    // 是否强制刷新 icache
    // ...
};
```

### 相关 prctl 接口

通过 `prctl(PR_RISCV_SET_ICACHE_FLUSH_CTX, ...)` 可以设置用户态 icache 刷新策略：

```c
// 支持的 prctl 值
#define PR_RISCV_CTX_SW_FENCEI_ON    // 允许用户态使用 fence.i
#define PR_RISCV_CTX_SW_FENCEI_OFF   // 禁止用户态使用 fence.i

// 作用范围
#define PR_RISCV_SCOPE_PER_PROCESS   // 影响整个进程
#define PR_RISCV_SCOPE_PER_THREAD    // 仅影响当前线程
```

## 流程图

```
用户态: JIT 编译代码
  │
  ▼
riscv_flush_icache(start, end, flags)
  │
  ▼
检查 flags 合法性 ──非法──→ 返回 -EINVAL
  │
 合法
  │
  ▼
flush_icache_mm(mm, local)
  │
  ├──────────────────────────────────────┐
  │                                      │
  ▼                                      ▼
单核?                             多核?
  │                                      │
  ▼                                      ▼
local_flush_icache_all()          local_flush_icache_all()
  │                                    │
  │                                    │
  │                              RISCV_FENCE(w, o)
  │                                    │
  │                              ┌─────┴─────┐
  │                              │           │
  │                              ▼           ▼
  │                       SBI 远程 fence   IPI 远程 fence
  │                              │           │
  └──────────────────────────────┴───────────┘
                                        │
                                        ▼
                                  返回 0
```

## 使用场景

- **JIT 编译器**：动态生成代码后需要刷新 icache 以确保执行新代码
- **动态代码修改**：使用 `mprotect` 修改代码段后
- **二进制翻译/模拟器**：动态翻译代码块时
- **自修改代码**：程序运行时修改自身指令

## 使用示例

```c
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_RISCV_FLUSH_ICACHE_LOCAL 1

// 在 RISC-V 上，生成新代码后调用此函数
void flush_icache(void *addr, size_t size)
{
    syscall(__NR_riscv_flush_icache,
            (uintptr_t)addr,
            (uintptr_t)addr + size,
            0);  // 0 = 刷新所有 hart
}
```

## 与 ARM64 对比

| 特性 | RISC-V | ARM64 |
|--|--|--|
| 用户态指令 | `fence.i`（仅本地） | 无直接用户态指令 |
| 系统调用 | `riscv_flush_icache` (259) | `cacheflush` (244) |
| 跨核刷新 | 需通过 SBI/IPI | 由硬件维护一致性 |
| 设计原因 | 用户态不知道线程-hart 映射 | 硬件自动处理 |

## 注意事项

- ARM64 上此系统调用仅为 RISC-V 架构兼容性保留
- ARM64 使用 `cacheflush` 系统调用或 `DC`/`IC` 指令管理缓存
- `start` 和 `end` 参数当前主要用于向前兼容，内核不实际使用它们
- 如果标志位包含未定义的值，系统调用返回 `-EINVAL`

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/riscv/kernel/sys_riscv.c](/home/louis/code/linux/arch/riscv/kernel/sys_riscv.c) | riscv_flush_icache 实现 |
| [arch/riscv/mm/cacheflush.c](/home/louis/code/linux/arch/riscv/mm/cacheflush.c) | flush_icache_all 实现 |
| [arch/riscv/include/asm/cacheflush.h](/home/louis/code/linux/arch/riscv/include/asm/cacheflush.h) | 标志位定义 |
| [tools/arch/riscv/include/uapi/asm/unistd.h](/home/louis/code/linux/tools/arch/riscv/include/uapi/asm/unistd.h) | 系统调用编号定义 |