# getcpu 系统调用分析

## 1. 概述

`getcpu` 获取当前线程正在运行的 CPU 编号和 NUMA 节点编号。用于实现用户态调度优化和 NUMA 感知编程。

**原型：**

```c
SYSCALL_DEFINE3(getcpu, unsigned __user *, cpup,
                unsigned __user *, nodep, void __user *, unused)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `cpup` | `unsigned __user *` | 输出参数，接收当前 CPU 编号（可为 NULL） |
| `nodep` | `unsigned __user *` | 输出参数，接收当前 NUMA 节点编号（可为 NULL） |
| `unused` | `void __user *` | 保留参数，当前未使用（必须为 NULL） |

**返回值：**
- 成功返回 0
- 失败返回 `-EFAULT`

## 2. 使用场景

- `sched_getcpu()` 库函数实现
- 用户态 per-CPU 数据结构优化
- NUMA 感知内存分配策略
- 调度器性能分析和调试
- 自旋锁和免锁算法的 CPU 标识

## 3. 函数调用栈

```
SYSCALL_DEFINE3(getcpu, cpup, nodep, unused)            // kernel/sys.c
  ├─ int cpu = raw_smp_processor_id()                    // 获取当前 CPU ID
  ├─ [cpup != NULL] put_user(cpu, cpup)                  // 写入 CPU 编号到用户空间
  │    拷贝失败 → err |= -EFAULT
  ├─ [nodep != NULL] put_user(cpu_to_node(cpu), nodep)   // CPU 对应的 NUMA 节点
  │    拷贝失败 → err |= -EFAULT
  └─ return err ? -EFAULT : 0
```

### 3.1 关键辅助函数

```c
// include/linux/smp.h
#define raw_smp_processor_id()   // 读取当前 CPU 的 ID（通过 per-CPU 变量或特殊寄存器）

// include/linux/topology.h
int cpu_to_node(int cpu);        // 返回 CPU 所属的 NUMA 节点编号
```

## 4. 关键数据结构

### 4.1 CPU 编号

CPU 编号由内核调度器管理，通过 `raw_smp_processor_id()` 获取。在 ARM64 上通常通过 `mrs` 指令读取 `tpidr_el1` 或 `mpidr_el1` 寄存器，在 x86 上通过 `current_task` per-CPU 变量获取。

### 4.2 NUMA 节点映射

```c
// include/linux/topology.h
// CPU 到 NUMA 节点的映射关系存储在内核的 cpu_to_node 数组中
DECLARE_EARLY_PER_CPU(int, x86_cpu_to_node_map);
// 或
extern const struct cpumask *cpu_to_node_mask(int node);
```

NUMA 节点信息在系统启动时通过 ACPI 或 Device Tree 解析，建立 CPU 到内存节点的映射关系。

## 5. 流程图

```
用户态调用 getcpu(cpup, nodep, unused)
    │
    ▼
┌─────────────────────────────────────┐
│  raw_smp_processor_id()             │
│  → 获取当前 CPU 编号                │
│  （如 x86 上通过 per-CPU 变量，     │
│    ARM64 上通过 mpidr_el1 获取）    │
└─────────────────────────────────────┘
    │
    ├── cpup != NULL?
    │    ├── 是 → put_user(cpu, cpup)
    │    │         失败 → err |= -EFAULT
    │    └── 否 → 跳过
    │
    ├── nodep != NULL?
    │    ├── 是 → cpu_to_node(cpu)
    │    │       put_user(node, nodep)
    │    │         失败 → err |= -EFAULT
    │    └── 否 → 跳过
    │
    └── return err ? -EFAULT : 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EFAULT` | 地址错误 | `cpup` 或 `nodep` 指向的用户空间地址不可写 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    unsigned cpu, node;

    // 获取当前 CPU 和 NUMA 节点
    if (syscall(SYS_getcpu, &cpu, &node, NULL) == 0) {
        printf("Current CPU:  %u\n", cpu);
        printf("NUMA node:    %u\n", node);
    } else {
        perror("getcpu");
        return 1;
    }

    // 也可以只获取 CPU 编号
    unsigned cpu_only;
    if (syscall(SYS_getcpu, &cpu_only, NULL, NULL) == 0) {
        printf("CPU only:     %u\n", cpu_only);
    }

    return 0;
}
```

### 库函数封装

glibc 提供了 `sched_getcpu()` 封装，内部使用 `getcpu` 系统调用：

```c
#include <sched.h>
int cpu = sched_getcpu();  // 通过 getcpu syscall 实现
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#系统标识与信息)
- 源码: `kernel/sys.c`（`SYSCALL_DEFINE3(getcpu)`）
- 相关头文件: `include/linux/smp.h`, `include/linux/topology.h`
- 库函数封装: glibc 的 `sched_getcpu()`
- 相关系统调用: `sched_setaffinity()`, `sched_getaffinity()`, `mbind()`