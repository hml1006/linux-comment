# 15. Lockdep 锁验证

## 15.1 概述

Lockdep (Lock Dependency Validator) 是 Linux 内核的运行时锁依赖验证系统，用于在系统运行时检测和预防死锁。它是内核中最重要的调试基础设施之一。

**核心思想：**
- 记录锁的获取顺序（依赖关系）
- 构建锁依赖图
- 检测依赖图中的环（死锁）
- 在死锁实际发生前触发警告

## 15.2 关键数据结构

### 15.2.1 锁类

定义在 [include/linux/lockdep_types.h](file:///home/louis/code/linux/include/linux/lockdep_types.h)：

```c
// include/linux/lockdep_types.h

// 每个锁对象包含一个 lockdep_map
struct lockdep_map {
    struct lock_class        *class_cache;    // 缓存锁类指针
    const char               *name;           // 锁名称
    int                       cpu;            // 所属 CPU
    unsigned long             ip;             // 初始化 IP
};

// 锁类 (每个唯一的锁类型)
struct lock_class {
    struct list_head          hash_entry;     // 哈希链表
    struct list_head          lock_entry;     // 全局锁链表

    /*
     * 锁依赖位图
     * 记录该锁之后可以获取哪些锁
     * 用于检测环形依赖
     */
    struct list_head          locks_after;    // 锁后关系
    struct list_head          locks_before;   // 锁前关系

    const char                *name;          // 锁名称
    int                       name_version;   // 名称版本

    u32                       wait_context_generation; // 等待上下文生成号
    unsigned long             contention_point[4];     // 竞争点

    // 校验状态
    unsigned long             ops;            // 操作计数
    unsigned long              usage_mask;     // 使用模式掩码
    struct stack_trace           usage_traces[XXX_LOCK_USAGE_STATES];

    // 子类
    struct lock_class_subclass *subclass;     // 子类
};
```

### 15.2.2 锁依赖链

```c
// kernel/locking/lockdep.c

// 锁依赖关系 (lock A → lock B 表示获取 A 后又获取 B)
struct lock_list {
    struct list_head          entry;          // 链表节点
    struct lock_class         *class;         // 目标锁类
    struct lock_class         *links_to;      // 依赖的目标
    int                       distance;       // 距离
    struct lock_trace         trace;          // 调用栈
    u16                       dep_type;       // 依赖类型
    /*
     * dep_type 编码:
     *  位 0: 依赖类型 (读/写)
     *  位 1: 是否递归
     */
};
```

## 15.3 工作原理

### 15.3.1 锁依赖图

```
Lockdep 维护一个锁依赖图:

获取顺序: A → B → C

图:
  A ──→ B ──→ C

  (A 之后可以获取 B, B 之后可以获取 C)

检测到: A → B → C → A (环!)

  A ──→ B
  ↑     │
  │     ▼
  └──── C

  死锁! 依赖图中有环!
```

### 15.3.2 检测流程

```
lock_acquire() 调用:
  │
  ├── 1. 检查当前锁类是否已记录
  │     ├── 是 → 使用缓存
  │     └── 否 → 分配新锁类
  │
  ├── 2. 记录当前锁的获取顺序
  │     └── 加入当前任务的锁链表中
  │
  ├── 3. 检查与前一个获取的锁的依赖关系
  │     └── 记录 new_lock → prev_lock 的依赖
  │
  ├── 4. 检查新依赖是否引入环
  │     └── check_noncircular():
  │           ├── BFS (广度优先搜索) 遍历依赖图
  │           └── 如果找到从 new_lock 回到 prev_lock 的路径
  │                 └── 报告死锁!
  │
  └── 5. 其他检查:
        ├── check_irq_usage()      → IRQ 安全检测
        ├── check_deadlock()       → 递归死锁检测
        └── check_usage_forwards() → 使用模式检查
```

## 15.4 死锁检测类型

### 15.4.1 递归死锁

```
相同锁类递归获取:
  spin_lock(&lock);
  spin_lock(&lock);    // DEADLOCK!

  Lockdep 报告: "possible recursive locking detected"
```

### 15.4.2 环形依赖

```
锁顺序反转:
  Task A: spin_lock(A); spin_lock(B);
  Task B: spin_lock(B); spin_lock(A);

  Lockdep 报告: "possible circular locking dependency detected"
```

### 15.4.3 IRQ 安全冲突

```
中断上下文锁冲突:
  进程上下文: spin_lock_irq(&lock);    // 关中断获取
  中断处理:   spin_lock(&lock);        // 不关中断

  Lockdep 报告: "possible irq lock inversion dependency detected"
  (如果在中断中获取锁, 进程上下文必须关中断)
```

### 15.4.4 锁持有时间错误

```
在持有锁时调用可能导致睡眠的函数:
  spin_lock(&lock);
  mutex_lock(&another);    // 错误! spin_lock 下不可睡眠

  Lockdep 报告: "scheduling while atomic"
```

## 15.5 使用方式

```c
// 1. 开启 Lockdep
// 内核配置: CONFIG_DEBUG_LOCKDEP=y
//           CONFIG_LOCKDEP=y
//           CONFIG_PROVE_LOCKING=y

// 2. 锁 API 自动集成 Lockdep
// 所有 spin_lock/mutex_lock 等 API 自动调用 lock_acquire()

// 3. 手动标注
// 在锁获取和释放时, Lockdep 自动插入

// 4. 查看 Lockdep 输出
// dmesg | grep "lockdep"
// /proc/lockdep      → 锁类信息
// /proc/lockdep_chains → 锁依赖链
// /proc/lock_stat    → 锁统计

// 5. 检查当前进程持有锁
// /proc/self/stack   → 查看调用栈
// /proc/lockdep      → 查看系统锁状态
```

## 15.6 使用场景

| 场景 | 说明 |
|------|------|
| 驱动开发 | 验证锁使用是否正确 |
| 内核模块 | 检测模块引入的死锁 |
| 新功能开发 | 确保锁顺序正确 |
| 性能调优 | 分析锁竞争 |

## 15.7 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/lockdep.h](file:///home/louis/code/linux/include/linux/lockdep.h) | Lockdep API |
| [include/linux/lockdep_types.h](file:///home/louis/code/linux/include/linux/lockdep_types.h) | Lockdep 数据结构 |
| [kernel/locking/lockdep.c](file:///home/louis/code/linux/kernel/locking/lockdep.c) | Lockdep 实现 |
| [kernel/locking/lockdep_proc.c](file:///home/louis/code/linux/kernel/locking/lockdep_proc.c) | Lockdep proc 文件系统 |