# 16. 内存屏障

## 16.1 概述

内存屏障 (Memory Barrier) 是硬件层面的同步原语，用于控制 CPU 和编译器对内存访问的重排序。在多处理器环境中，不同 CPU 可能观察到不同的内存访问顺序，内存屏障强制保证顺序一致性。

**为什么需要内存屏障：**
- CPU 乱序执行：现代 CPU 为了性能会重排指令
- 编译器优化：编译器可能重排内存访问
- 缓存一致性：不同 CPU 的缓存可能不同步
- DMA/外设：设备可能观察到不同的内存顺序

## 16.2 屏障类型

### 16.2.1 读写屏障分类

```
┌───────────────┬──────────────────────────────────────────────┐
│    屏障类型    │                    效果                      │
├───────────────┼──────────────────────────────────────────────┤
│ 读屏障 (rmb)  │ 屏障前的所有读操作必须在屏障后的读操作之前完成 │
│ 写屏障 (wmb)  │ 屏障前的所有写操作必须在屏障后的写操作之前完成 │
│ 全屏障 (mb)   │ 屏障前的所有读写操作必须在屏障后的操作之前完成 │
│ 获取屏障      │ 屏障后的读写不能重排到屏障前 (ACQUIRE)        │
│ 释放屏障      │ 屏障前的读写不能重排到屏障后 (RELEASE)        │
│ 依赖屏障      │ 仅确保有数据依赖的读操作顺序                  │
└───────────────┴──────────────────────────────────────────────┘
```

### 16.2.2 屏障效果示意

```
无屏障:
  CPU 0: 写 A, 写 B, 读 C, 读 D
  CPU 0 实际执行: 读 C, 写 B, 读 D, 写 A (任意重排)

有 mb 屏障:
  CPU 0: 写 A; mb(); 写 B; mb(); 读 C; mb(); 读 D
  CPU 0 实际执行: 写 A → 写 B → 读 C → 读 D (强制顺序)

有 rmb 屏障:
  CPU 0: 读 A; rmb(); 读 B → 读 A 必须在读 B 之前完成

有 wmb 屏障:
  CPU 0: 写 A; wmb(); 写 B → 写 A 必须在写 B 之前完成
```

## 16.3 内核屏障 API

### 16.3.1 通用屏障

```c
// include/asm-generic/barrier.h

// 编译器屏障 — 阻止编译器重排, 但不影响 CPU
#define barrier() __asm__ __volatile__("" ::: "memory")

// 读屏障 — 所有读操作不能重排越过屏障
smp_rmb();

// 写屏障 — 所有写操作不能重排越过屏障
smp_wmb();

// 全屏障 — 所有读写操作不能重排越过屏障
smp_mb();

// 数据依赖屏障 — 仅确保数据依赖的顺序
// 防止因 CPU 的依赖预测执行导致的问题
smp_read_barrier_depends();
```

### 16.3.2 ACQUIRE/RELEASE 语义

```c
// ACQUIRE 操作: 屏障后的所有读写不能重排到屏障前
// 典型: 锁获取
smp_acquire__after_ctrl_dep();  // 控制依赖后的 ACQUIRE
smp_mb__after_atomic();         // 原子操作后的全屏障

// RELEASE 操作: 屏障前的所有读写不能重排到屏障后
// 典型: 锁释放
smp_mb__before_atomic();        // 原子操作前的全屏障
smp_mb__after_spinlock();       // spin_lock 后的全屏障
```

### 16.3.3 MMIO 屏障

```c
// 用于 MMIO (内存映射 I/O) 操作
// 确保设备寄存器读写顺序

// 读屏障变体
mmiob_read_barrier();

// 写屏障变体
mmiob_write_barrier();

// spin_unlock 时隐式插入 MMIO 屏障
mmiowb_spin_lock();
mmiowb_spin_unlock();
```

### 16.3.4 DMA 屏障

```c
// 用于 DMA 操作
// 确保 CPU 写入内存后, 设备可见

// DMA 写屏障
dma_wmb();
// 确保 DMA 描述符写入后, 设备才能看到

// DMA 读屏障
dma_rmb();
// 确保设备写入后, CPU 读取时看到最新值
```

## 16.4 ARM64 架构实现

### 16.4.1 ARM64 屏障指令

```asm
// ARM64 屏障指令

// DMB (Data Memory Barrier) — 数据内存屏障
dmb sy;     // 系统域全屏障
dmb ish;    // 内部共享域屏障 (SMP)
dmb ishst;  // 内部共享域存储屏障
dmb nsh;    // 非共享域屏障
dmb osh;    // 外部共享域屏障

// DSB (Data Synchronization Barrier) — 数据同步屏障
dsb sy;     // 系统域同步屏障
dsb ish;    // 内部共享域同步屏障
// DSB 比 DMB 更强, 会等待所有缓存/写缓冲/分支预测完成

// ISB (Instruction Synchronization Barrier) — 指令同步屏障
isb;        // 清空流水线, 用于指令修改
```

### 16.4.2 内核屏障到 ARM64 的映射

```c
// arch/arm64/include/asm/barrier.h

// smp_mb() → dmb ish
#define smp_mb()    asm volatile("dmb ish" : : : "memory")

// smp_rmb() → dmb ishld
#define smp_rmb()   asm volatile("dmb ishld" : : : "memory")

// smp_wmb() → dmb ishst
#define smp_wmb()   asm volatile("dmb ishst" : : : "memory")

// mb() → dsb sy (用于 MMIO, 比 smp_* 更强)
#define mb()        asm volatile("dsb sy" : : : "memory")
#define rmb()       asm volatile("dsb ishld" : : : "memory")
#define wmb()       asm volatile("dsb ishst" : : : "memory")
```

## 16.5 使用场景

### 16.5.1 生产者-消费者

```c
// 经典的生产者-消费者模式
// 必须使用屏障确保数据可见性

// 生产者
void producer(struct data *shared, struct data *new_data)
{
    new_data->value = 42;
    new_data->valid = true;

    // 写屏障: 确保 value 和 valid 写入完成
    // 消费者才能看到正确的数据
    smp_wmb();

    shared->ptr = new_data;  // 发布指针
}

// 消费者
void consumer(struct data *shared)
{
    struct data *data;

    data = READ_ONCE(shared->ptr);

    // 读屏障: 确保获取指针后, 读取的数据是最新的
    smp_rmb();

    if (data->valid) {
        // 使用 data->value (此时 value 一定可见)
    }
}
```

### 16.5.2 自旋锁实现

```c
// 自旋锁中的屏障使用

// 锁获取 (ACQUIRE 语义)
// 确保锁获取后, 临界区内的数据可见
arch_spin_lock(lock) {
    // 获取锁 (原子操作)
    // 隐式 ACQUIRE 屏障: 之后的读写不能重排到锁获取前
}

// 锁释放 (RELEASE 语义)
// 确保临界区内的数据对其他 CPU 可见
arch_spin_unlock(lock) {
    // 释放锁 (原子操作)
    // 隐式 RELEASE 屏障: 之前的读写不能重排到锁释放后
}
```

### 16.5.3 RCU 写者

```c
// RCU 更新中的屏障使用

// 发布新指针
rcu_assign_pointer(ptr, new_val) {
    // 1. 写屏障: 确保新数据写入完成
    smp_wmb();
    // 2. 原子赋值
    WRITE_ONCE(ptr, new_val);
}

// 读取指针
rcu_dereference(ptr) {
    // 1. 读取指针
    typeof(*ptr) *p = READ_ONCE(ptr);
    // 2. 读屏障: 确保通过指针读取的数据是最新的
    // 实际是数据依赖屏障 (read_barrier_depends)
    return p;
}
```

## 16.6 使用注意事项

```c
// 1. 屏障不是魔法 — 需要配对使用
// 生产者的 wmb() 必须与消费者的 rmb() 配对

// 2. 过度使用屏障会降低性能
// 屏障会阻止 CPU 乱序执行的优化

// 3. 优先使用锁机制
// 锁内部已经包含正确的屏障
// 直接使用屏障很容易出错

// 4. 了解硬件模型
// ARM64 是弱序模型, 需要更多屏障
// x86 是强序模型, 大部分情况下不需要屏障

// 5. 使用 READ_ONCE/WRITE_ONCE
// 即使不使用屏障, 也应该使用这些宏
// 防止编译器优化导致的意外行为
```

## 16.7 关键文件

| 文件 | 说明 |
|------|------|
| [include/asm-generic/barrier.h](file:///home/louis/code/linux/include/asm-generic/barrier.h) | 通用屏障定义 |
| [arch/arm64/include/asm/barrier.h](file:///home/louis/code/linux/arch/arm64/include/asm/barrier.h) | ARM64 屏障实现 |
| [Documentation/memory-barriers.txt](file:///home/louis/code/linux/Documentation/memory-barriers.txt) | 内存屏障文档 |