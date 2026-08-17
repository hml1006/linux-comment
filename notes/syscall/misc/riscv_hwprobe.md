# riscv_hwprobe

## 原理与功能

`riscv_hwprobe` 是 RISC-V 架构专用的系统调用，用于探测硬件能力（Hardware Probing）。此系统调用允许用户态查询 CPU 的硬件特性，包括支持哪些指令集扩展、处理器厂商 ID、架构 ID 等。

在 ARM64 架构上，此系统调用存在于编号表中，但仅为 RISC-V 架构提供支持，ARM64 上为 stub 实现。

### 功能说明

- 探测 CPU 硬件特性
- 查询支持的指令集扩展
- 支持按 CPU 集合查询
- RISC-V 架构编号为 258（`__NR_riscv_hwprobe`）

## 函数原型

```c
SYSCALL_DEFINE5(riscv_hwprobe, struct riscv_hwprobe __user *, pairs,
                size_t, pair_count, size_t, cpusetsize,
                unsigned long __user *, cpus, unsigned int, flags);
```

| 参数 | 类型 | 描述 |
|--|--|--|
| `pairs` | `struct riscv_hwprobe __user *` | 键值对数组，用户态预填 key，内核填充 value |
| `pair_count` | `size_t` | pairs 数组元素个数 |
| `cpusetsize` | `size_t` | cpus CPU 位图的大小（字节） |
| `cpus` | `unsigned long __user *` | CPU 集合位图，NULL 表示所有在线 CPU |
| `flags` | `unsigned int` | 标志位 |

### 关键数据结构

```c
// arch/riscv/include/uapi/asm/hwprobe.h
struct riscv_hwprobe {
    __s64 key;      // 查询键（用户态预填）
    __u64 value;    // 返回值（内核填充）
};

// 支持的 key 值
#define RISCV_HWPROBE_KEY_MVENDORID     0   // 厂商 ID
#define RISCV_HWPROBE_KEY_MARCHID       1   // 架构 ID
#define RISCV_HWPROBE_KEY_MIMPID        2   // 实现 ID
#define RISCV_HWPROBE_KEY_BASE_BEHAVIOR 3   // 基础行为
#define     RISCV_HWPROBE_BASE_BEHAVIOR_IMA  (1 << 0)  // 支持 IMA
#define RISCV_HWPROBE_KEY_IMA_EXT_0     4   // IMA 扩展 0
#define RISCV_HWPROBE_KEY_IMA_EXT_1     5   // IMA 扩展 1
#define RISCV_HWPROBE_KEY_CPUPERF_0     6   // CPU 性能 0
// ... 更多 key 值

// flags 标志位
#define RISCV_HWPROBE_WHICH_CPUS  (1 << 0)  // 反转行为：给定值，过滤 CPU 集合
```

## 完整实现

```c
// kernel/sys_riscv.c (sys_hwprobe.c)
SYSCALL_DEFINE5(riscv_hwprobe, struct riscv_hwprobe __user *, pairs,
        size_t, pair_count, size_t, cpusetsize, unsigned long __user *,
        cpus, unsigned int, flags)
{
    return do_riscv_hwprobe(pairs, pair_count, cpusetsize,
                            cpus, flags);
}
```

### 核心实现流程

```c
static int do_riscv_hwprobe(struct riscv_hwprobe __user *pairs,
                            size_t pair_count, size_t cpusetsize,
                            unsigned long __user *cpus_user,
                            unsigned int flags)
{
    // 确保 vDSO 数据已初始化（首次调用时会等待异步探测完成）
    DO_ONCE_SLEEPABLE(complete_hwprobe_vdso_data);

    if (flags & RISCV_HWPROBE_WHICH_CPUS)
        return hwprobe_get_cpus(pairs, pair_count, cpusetsize,
                                cpus_user, flags);

    // 默认行为：查询 key 对应的值
    return hwprobe_get_values(pairs, pair_count, cpusetsize,
                              cpus_user, flags);
}
```

## 调用链分析

### 获取值模式（默认）

```
riscv_hwprobe(pairs, pair_count, cpusetsize, cpus, flags=0)
  │
  └─ do_riscv_hwprobe()
       ├─ complete_hwprobe_vdso_data()  // 初始化 vDSO 数据
       │
       └─ hwprobe_get_values()
            └─ 对每个 pair:
                 ├─ get_user(pair.key, &pairs->key)  // 读取用户态的 key
                 ├─ hwprobe_one_pair(&pair, &cpus)    // 查询硬件
                 │    ├─ 根据 key 值分发表
                 │    ├─ 读取 cpufeature 数据
                 │    └─ 填充 pair.value
                 └─ put_user(pair.key, &pairs->key)   // 写回结果
                 └─ put_user(pair.value, &pairs->value)
```

### 获取 CPU 模式（`flags = RISCV_HWPROBE_WHICH_CPUS`）

```
riscv_hwprobe(pairs, pair_count, cpusetsize, cpus, flags=WHICH_CPUS)
  │
  └─ do_riscv_hwprobe()
       └─ hwprobe_get_cpus()
            ├─ copy_from_user(&cpus, cpus_user, cpusetsize)  // 读取 CPU 位图
            ├─ cpumask_and(&cpus, &cpus, cpu_online_mask)     // 只保留在线 CPU
            │
            └─ 对每个 pair:
                 ├─ 对 CPU 集合中的每个 CPU:
                 │    ├─ hwprobe_one_pair(&tmp, &one_cpu)
                 │    └─ 比较 tmp 与 pair 的值
                 │         └─ 不匹配 → 从 CPU 集合中移除该 CPU
                 │
                 └─ copy_to_user(cpus_user, &cpus, cpusetsize)  // 写回过滤后的 CPU 位图
```

## 关键数据结构

### vDSO 加速数据

```c
// arch/riscv/kernel/sys_hwprobe.c
// 内核初始化时填充 vDSO 数据，使常见查询无需系统调用
struct vdso_arch_data {
    u64 all_cpu_hwprobe_values[RISCV_HWPROBE_MAX_KEY + 1];
    bool homogeneous_cpus;  // 所有 CPU 是否同质
    bool ready;             // 数据是否已就绪
};
```

### 支持的 Key 值

| Key | 值 | 描述 | 返回类型 |
|--|--|--|--|
| `RISCV_HWPROBE_KEY_MVENDORID` | 0 | 厂商 ID | 值 |
| `RISCV_HWPROBE_KEY_MARCHID` | 1 | 架构 ID | 值 |
| `RISCV_HWPROBE_KEY_MIMPID` | 2 | 实现 ID | 值 |
| `RISCV_HWPROBE_KEY_BASE_BEHAVIOR` | 3 | 基础行为 | 位掩码 |
| `RISCV_HWPROBE_KEY_IMA_EXT_0` | 4 | IMA 扩展 0 | 位掩码 |
| `RISCV_HWPROBE_KEY_IMA_EXT_1` | 5 | IMA 扩展 1 | 位掩码 |
| `RISCV_HWPROBE_KEY_CPUPERF_0` | 6 | CPU 性能提示 | 位掩码 |
| ... | ... | ... | ... |

## 流程图

```
用户态调用 riscv_hwprobe(pairs, count, cpusetsize, cpus, flags)
  │
  ▼
do_riscv_hwprobe()
  │
  ├─ (首次) 等待异步探测完成
  │
  ├─ flags & WHICH_CPUS?
  │    │
  │    ├─ 是 → hwprobe_get_cpus()
  │    │    │
  │    │    ├─ 读取用户态 CPU 位图
  │    │    ├─ 只保留在线 CPU
  │    │    ├─ 对每个 pair:
  │    │    │    └─ 遍历每个 CPU，比较值是否匹配
  │    │    │         └─ 不匹配 → 从位图中移除 CPU
  │    │    └─ 写回过滤后的 CPU 位图
  │    │
  │    └─ 否 → hwprobe_get_values()
  │         │
  │         ├─ 对每个 pair:
  │         │    ├─ 读取 key
  │         │    ├─ hwprobe_one_pair() 查询硬件
  │         │    └─ 写回 key + value
  │         │
  │         └─ 返回 0
  │
  ▼
返回用户态
```

## 使用场景

- **运行时特性检测**：查询 CPU 是否支持特定指令集扩展（如向量扩展 V、压缩指令集 C 等）
- **性能优化**：根据 CPU 性能特性选择最优算法路径
- **跨平台兼容**：同一二进制文件在不同 RISC-V 实现上运行时检测硬件能力
- **虚拟化场景**：在虚拟机中探测宿主机硬件能力

## 使用示例

```c
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>

#define __NR_riscv_hwprobe 258
#define RISCV_HWPROBE_KEY_MVENDORID 0
#define RISCV_HWPROBE_KEY_MARCHID   1

int main(void)
{
    struct riscv_hwprobe pairs[2];
    
    pairs[0].key = RISCV_HWPROBE_KEY_MVENDORID;
    pairs[1].key = RISCV_HWPROBE_KEY_MARCHID;
    
    // 查询所有在线 CPU
    syscall(__NR_riscv_hwprobe, pairs, 2, 0, NULL, 0);
    
    printf("Vendor ID: 0x%llx\n", pairs[0].value);
    printf("Arch ID: 0x%llx\n", pairs[1].value);
    
    return 0;
}
```

## 与 ARM64 对比

| 特性 | RISC-V | ARM64 |
|--|--|--|
| 硬件探测方式 | 系统调用 `riscv_hwprobe` | `MRS` 指令读取 ID 寄存器 |
| 查询粒度 | 按 CPU 集合 | 单 CPU |
| 灵活度 | 可扩展的 key-value 机制 | 固定寄存器 |
| vDSO 加速 | 支持（常见查询无需系统调用） | 不适用 |

## 注意事项

- ARM64 上此系统调用仅为 RISC-V 架构兼容性保留
- ARM64 通过 `MRS` 指令读取 ID 寄存器获知硬件能力
- 如果 key 值未知，内核将 key 设为 -1，value 设为 0
- 对于值类型 key（如 MVENDORID），如果指定 CPU 集合中各 CPU 值不同，返回 -1
- 对于位掩码类型 key，返回值为指定 CPU 集合中各 CPU 值的逻辑与

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/riscv/kernel/sys_hwprobe.c](/home/louis/code/linux/arch/riscv/kernel/sys_hwprobe.c) | riscv_hwprobe 完整实现 |
| [arch/riscv/include/uapi/asm/hwprobe.h](/home/louis/code/linux/arch/riscv/include/uapi/asm/hwprobe.h) | key 值定义和数据结构 |
| [arch/riscv/kernel/vdso/hwprobe.c](/home/louis/code/linux/arch/riscv/kernel/vdso/hwprobe.c) | vDSO 加速实现 |
| [Documentation/arch/riscv/hwprobe.rst](/home/louis/code/linux/Documentation/arch/riscv/hwprobe.rst) | 官方文档 |