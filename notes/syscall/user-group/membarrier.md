# membarrier 系统调用分析

## 1. 概述

`membarrier` 提供内存屏障（memory barrier）功能，用于在多线程程序中同步内存访问。允许一个线程在全局范围内或对特定 CPU 发出内存屏障指令，确保内存操作的顺序一致性。

**原型：**

```c
SYSCALL_DEFINE3(membarrier, int, cmd, unsigned int, flags, int, cpu_id)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `cmd` | `int` | 命令（`MEMBARRIER_CMD_*` 枚举值） |
| `flags` | `unsigned int` | 标志位（`MEMBARRIER_CMD_FLAG_CPU`） |
| `cpu_id` | `int` | CPU ID（当 `flags` 包含 `MEMBARRIER_CMD_FLAG_CPU` 时指定目标 CPU） |

**返回值：**
- `MEMBARRIER_CMD_QUERY`：返回支持的命令位掩码
- 其他命令：成功返回 0，失败返回负的错误码

## 2. 使用场景

- 用户态 RCU（Read-Copy-Update）实现
- 多线程无锁数据结构
- 实时系统中的内存排序保证
- 进程间共享内存同步

## 3. 函数调用栈

```
SYSCALL_DEFINE3(membarrier, cmd, flags, cpu_id)          // kernel/sched/membarrier.c
  ├─ 参数校验: cmd 相关的 flags 检查
  │    MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ: flags 只能为 0 或 MEMBARRIER_CMD_FLAG_CPU
  │    其他命令: flags 必须为 0
  ├─ [!(flags & MEMBARRIER_CMD_FLAG_CPU)] → cpu_id = -1
  │
  ├─ switch (cmd) {
  │    case MEMBARRIER_CMD_QUERY:
  │    │    return MEMBARRIER_CMD_BITMASK;  // 返回支持的命令位掩码
  │    │
  │    case MEMBARRIER_CMD_GLOBAL:
  │    │    synchronize_rcu();               // 全局同步
  │    │    return 0;
  │    │
  │    case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
  │    │    return membarrier_global_expedited();  // 全局快速屏障
  │    │
  │    case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
  │    │    return membarrier_register_global_expedited();  // 注册全局快速屏障
  │    │
  │    case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
  │    │    return membarrier_private_expedited(0, cpu_id);  // 私有快速屏障
  │    │
  │    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
  │    │    return membarrier_register_private_expedited(0);  // 注册私有快速屏障
  │    │
  │    case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
  │    │    return membarrier_private_expedited(MEMBARRIER_FLAG_SYNC_CORE, cpu_id);
  │    │
  │    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
  │    │    return membarrier_register_private_expedited(MEMBARRIER_FLAG_SYNC_CORE);
  │    │
  │    case MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ:
  │    │    return membarrier_private_expedited(MEMBARRIER_FLAG_RSEQ, cpu_id);
  │    │
  │    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ:
  │    │    return membarrier_register_private_expedited(MEMBARRIER_FLAG_RSEQ);
  │    │
  │    case MEMBARRIER_CMD_GET_REGISTRATIONS:
  │    │    return membarrier_get_registrations();  // 获取注册状态
  │    │
  │    default:
  │    │    return -EINVAL;
  │  }
```

### 3.1 membarrier_global_expedited 调用栈

```
membarrier_global_expedited()                            // kernel/sched/membarrier.c
  └─ smp_call_function_many(cpu_online_mask, ipi_membarrier, NULL, 1)
       └─ 对所有在线 CPU 发送 IPI（处理器间中断）
            └─ ipi_membarrier()  // IPI 处理函数，执行内存屏障指令
```

### 3.2 membarrier_private_expedited 调用栈

```
membarrier_private_expedited(flags, cpu_id)              // kernel/sched/membarrier.c
  ├─ 检查是否已注册（通过 task_struct->membarrier_state）
  ├─ [cpu_id >= 0] → 仅向指定 CPU 发送 IPI
  └─ [cpu_id < 0]  → 向所有在线 CPU 发送 IPI
       └─ smp_call_function_many() / smp_call_function_single()
```

## 4. 关键数据结构

### 4.1 membarrier 命令枚举

```c
// include/uapi/linux/membarrier.h
#define MEMBARRIER_CMD_QUERY                   0
#define MEMBARRIER_CMD_GLOBAL                  1
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED        2
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED   4
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED       8
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED  16
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE   32
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE  64
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ  128
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ  256
#define MEMBARRIER_CMD_GET_REGISTRATIONS       512
```

### 4.2 membarrier_state（进程注册状态）

```c
// include/linux/sched.h (task_struct 中)
struct task_struct {
    // ...
    unsigned long membarrier_state;  // 位掩码，记录进程注册的 membarrier 模式
    // ...
};
```

### 4.3 MEMBARRIER_CMD_FLAG_CPU

```c
// include/uapi/linux/membarrier.h
#define MEMBARRIER_CMD_FLAG_CPU  (1 << 0)  // 指示 cpu_id 参数有效
```

## 5. 流程图

```
用户态调用 membarrier(cmd, flags, cpu_id)
    │
    ▼
┌─────────────────────────────────────────┐
│  QUERY?                                  │
│  ├─ 是 → 返回 MEMBARRIER_CMD_BITMASK    │
│  └─ 否 → 继续                           │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  需要注册的 cmd?                         │
│  ├─ REGISTER_GLOBAL_EXPEDITED           │
│  │    → 设置 membarrier_state 中的位     │
│  ├─ REGISTER_PRIVATE_EXPEDITED          │
│  │    → 设置 membarrier_state 中的位     │
│  ├─ REGISTER_PRIVATE_EXPEDITED_SYNC_CORE│
│  │    → 设置相应位                      │
│  └─ REGISTER_PRIVATE_EXPEDITED_RSEQ     │
│       → 设置相应位                      │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  执行屏障的 cmd?                         │
│  ├─ GLOBAL → synchronize_rcu()          │
│  ├─ GLOBAL_EXPEDITED →                  │
│  │    smp_call_function_many(all)       │
│  ├─ PRIVATE_EXPEDITED →                 │
│  │    smp_call_function_many(registered)│
│  └─ PRIVATE_EXPEDITED_SYNC_CORE/RSEQ → │
│       smp_call_function_many(registered)│
└─────────────────────────────────────────┘
    │
    ▼
  IPI 处理函数 ipi_membarrier()
  → 执行 smp_mb() 内存屏障指令
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | `cmd` 无效，或 `flags` 与 `cmd` 不兼容 |
| `-EPERM` | 未注册 | 执行 `PRIVATE_EXPEDITED` 前未调用 `REGISTER_PRIVATE_EXPEDITED` |
| `-ENOSYS` | 不支持 | 内核未配置 `CONFIG_MEMBARRIER` |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/membarrier.h>

int main(void)
{
    // 查询支持的命令
    int mask = syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0, 0);
    if (mask < 0) {
        perror("membarrier query");
        return 1;
    }

    printf("Supported commands: 0x%x\n", mask);

    // 注册私有快速屏障
    if (mask & MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED) {
        if (syscall(SYS_membarrier,
                    MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) < 0) {
            perror("membarrier register");
            return 1;
        }
        printf("Registered private expedited\n");
    }

    // 执行私有快速屏障
    if (mask & MEMBARRIER_CMD_PRIVATE_EXPEDITED) {
        if (syscall(SYS_membarrier,
                    MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) < 0) {
            perror("membarrier private expedited");
            return 1;
        }
        printf("Memory barrier executed\n");
    }

    // 获取注册状态
    if (mask & MEMBARRIER_CMD_GET_REGISTRATIONS) {
        int regs = syscall(SYS_membarrier,
                           MEMBARRIER_CMD_GET_REGISTRATIONS, 0, 0);
        if (regs >= 0) {
            printf("Current registrations: 0x%x\n", regs);
        }
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#用户与组关系)
- 源码: `kernel/sched/membarrier.c`
- 头文件: `include/uapi/linux/membarrier.h`
- 配置选项: `CONFIG_MEMBARRIER`
- 相关系统调用: `rseq()`