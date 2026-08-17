# 10. Per-CPU 变量

## 10.1 概述

Per-CPU 变量是一种"避免共享"的同步策略：每个 CPU 拥有独立的数据副本，从而完全消除并发访问冲突。

**核心思想：**
- 数据分片：每个 CPU 一份副本，无需互斥
- 无锁访问：读写在本地副本上操作，无竞争
- 局部性优：始终在本地 CPU 缓存中操作，性能极高

## 10.2 关键数据结构

### 10.2.1 静态 Per-CPU 变量

```c
// include/linux/percpu-defs.h

// 声明 per-CPU 变量
DECLARE_PER_CPU(type, name);

// 定义 per-CPU 变量
DEFINE_PER_CPU(type, name);

// 定义 per-CPU 数组
DEFINE_PER_CPU(type, name[size]);

// 定义 read_mostly 类型
DEFINE_PER_CPU_READ_MOSTLY(type, name);

// 定义带对齐的 per-CPU 变量
DEFINE_PER_CPU_ALIGNED(type, name);
```

### 10.2.2 动态 Per-CPU 变量

```c
// include/linux/percpu.h

// 分配
void __percpu *alloc_percpu(type);           // 按类型分配
void __percpu *alloc_percpu_gfp(type, gfp_t); // 带 GFP 标志
void __percpu *__alloc_percpu(size_t size, size_t align);

// 释放
void free_percpu(void __percpu *ptr);
```

## 10.3 核心 API

### 10.3.1 访问操作

```c
// include/linux/percpu.h

// 1. 获取本地 CPU 副本 (禁用抢占)
get_cpu_var(var);       // 返回 var 的本地副本 (lvalue)
put_cpu_var(var);       // 完成访问

// 使用模式:
get_cpu_var(stats).counter++;
put_cpu_var(stats);

// 2. 获取指针 (禁用抢占)
preempt_disable();
p = this_cpu_ptr(&var);
// 使用 *p ...
preempt_enable();

// 3. per-CPU 操作 (带原子性)
// 编译成单个 per-CPU 原子指令 (如果架构支持)
this_cpu_read(var);          // 读
this_cpu_write(var, val);    // 写
this_cpu_add(var, val);      // 加
this_cpu_inc(var);           // 自增
this_cpu_dec(var);           // 自减
this_cpu_and(var, val);      // 与
this_cpu_or(var, val);       // 或
this_cpu_xchg(var, nval);    // 交换
this_cpu_cmpxchg(var, oval, nval);  // CAS

// 4. 指定 CPU 访问
per_cpu(var, cpu);           // 读取指定 CPU 的副本
```

### 10.3.2 批量操作

```c
// 遍历所有 CPU 的副本
int cpu;
for_each_possible_cpu(cpu) {
    val = per_cpu(var, cpu);
    total += val;
}
```

## 10.4 内存布局

```
Per-CPU 变量的内存布局:

CPU 0 区域:
  ┌────────────────┐
  │ var_0          │ ← __per_cpu_offset[0] + 偏移
  ├────────────────┤
  │ ...
  └────────────────┘

CPU 1 区域:
  ┌────────────────┐
  │ var_1          │ ← __per_cpu_offset[1] + 偏移
  ├────────────────┤
  │ ...
  └────────────────┘

CPU N 区域:
  ┌────────────────┐
  │ var_N          │ ← __per_cpu_offset[N] + 偏移
  ├────────────────┤
  │ ...
  └────────────────┘

每个 CPU 区域大小 = PERCPU_ENOUGH_ROOM
(通常为 64KB, 可通过内核配置调整)
```

```
地址转换:
  __per_cpu_offset[cpu] 数组:
  ┌─────────────────────────────────────┐
  │  __per_cpu_offset[0] = 0xFFFF8000  │ → CPU0 数据区域起始
  │  __per_cpu_offset[1] = 0xFFFFC000  │ → CPU1 数据区域起始
  │  __per_cpu_offset[2] = 0xFFFF0000  │ → CPU2 数据区域起始
  │  ...                                │
  └─────────────────────────────────────┘

  访问 per_cpu(var, cpu):
    addr = (unsigned long)&var + __per_cpu_offset[cpu]
```

## 10.5 PREEMPT_RT 下的变化

在 PREEMPT_RT 下，`get_cpu_var()` 和 `this_cpu_ops()` 的实现有所变化：

```c
// 非 RT: 禁用抢占即可保证 per-CPU 安全
#define get_cpu_var(var)                     \
({                                           \
    preempt_disable();                       \
    this_cpu_ptr(&var);                      \
})

// PREEMPT_RT: 需要禁用迁移 (抢占仍然允许)
#define get_cpu_var(var)                     \
({                                           \
    migrate_disable();                       \
    this_cpu_ptr(&var);                      \
})

#define put_cpu_var(var)                     \
    migrate_enable();

// 原因: RT 下 spin_lock 不再禁用抢占,
// 但 per-CPU 变量访问期间不能迁移到其他 CPU
```

## 10.6 使用场景

| 场景 | 说明 |
|------|------|
| 统计计数器 (网络/CPU) | 每个 CPU 统计, 汇总时读取所有副本 |
| RCU 数据 | RCU 的 per-CPU 回调队列 |
| 调度器统计 | 每个 CPU 的运行队列统计 |
| 页面分配器 | 每个 CPU 的页面缓存 |
| 内核栈 | 每个任务+每个 CPU 的栈 |

## 10.7 使用示例

```c
// 示例: per-CPU 网络统计
DEFINE_PER_CPU(struct net_stats, cpu_stats);

// 更新统计 (本地 CPU, 无锁)
void update_rx_stats(size_t len)
{
    struct net_stats *stats;

    preempt_disable();
    stats = this_cpu_ptr(&cpu_stats);
    stats->rx_packets++;
    stats->rx_bytes += len;
    preempt_enable();
}

// 汇总统计 (遍历所有 CPU)
struct net_stats get_total_stats(void)
{
    struct net_stats total = { 0 };
    int cpu;

    for_each_possible_cpu(cpu) {
        struct net_stats *stats = per_cpu_ptr(&cpu_stats, cpu);
        total.rx_packets += stats->rx_packets;
        total.rx_bytes += stats->rx_bytes;
    }
    return total;
}
```

## 10.8 使用注意事项

```c
// 1. 访问期间必须禁止迁移/抢占
// 否则可能被调度到其他 CPU, 操作错误的副本
preempt_disable();
p = this_cpu_ptr(&var);
// 如果不禁止, 此处可能被调度到 CPU 1
// 但 p 指向 CPU 0 的副本 → 数据不一致!
*p = 42;
preempt_enable();

// 2. 非原子操作不保证安全 (被中断打断)
preempt_disable();
this_cpu_ptr(&var)->counter++;  // 可能被中断打断
preempt_enable();

// 如果中断也访问同一变量, 需要 local_irq_save()
unsigned long flags;
local_irq_save(flags);
this_cpu_ptr(&var)->counter++;
local_irq_restore(flags);

// 3. 汇总时可能看到不一致视图
// 遍历所有 CPU 读取时, 各 CPU 的值可能不同步

// 4. 动态 per-CPU 变量需要 __percpu 注释
// 辅助 Sparse 工具检查
void __percpu *ptr;
```

## 10.9 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/percpu.h](file:///home/louis/code/linux/include/linux/percpu.h) | Per-CPU 变量 API |
| [include/linux/percpu-defs.h](file:///home/louis/code/linux/include/linux/percpu-defs.h) | Per-CPU 宏定义 |
| [mm/percpu.c](file:///home/louis/code/linux/mm/percpu.c) | Per-CPU 分配器实现 |
| [arch/arm64/include/asm/percpu.h](file:///home/louis/code/linux/arch/arm64/include/asm/percpu.h) | ARM64 per-CPU 实现 |