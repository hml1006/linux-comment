# Linux 内核进程调度子系统分析

## 目录

1. [概述](#1-概述)
2. [核心数据结构](#2-核心数据结构)
   - 2.1 [task_struct 中的调度相关字段](#21-task_struct-中的调度相关字段)
   - 2.2 [struct rq —— 每 CPU 运行队列](#22-struct-rq--每-cpu-运行队列)
   - 2.3 [struct cfs_rq —— CFS 运行队列](#23-struct-cfs_rq--cfs-运行队列)
   - 2.4 [struct rt_rq —— 实时运行队列](#24-struct-rt_rq--实时运行队列)
   - 2.5 [struct dl_rq —— 截止时间运行队列](#25-struct-dl_rq--截止时间运行队列)
   - 2.6 [struct sched_class —— 调度类接口](#26-struct-sched_class--调度类接口)
   - 2.7 [struct sched_entity / sched_rt_entity / sched_dl_entity](#27-struct-sched_entity--sched_rt_entity--sched_dl_entity)
3. [调度类层次结构](#3-调度类层次结构)
   - 3.1 [Stop 调度类](#31-stop-调度类)
   - 3.2 [Deadline 调度类 (SCHED_DEADLINE)](#32-deadline-调度类-sched_deadline)
   - 3.3 [Real-Time 调度类 (SCHED_FIFO/SCHED_RR)](#33-real-time-调度类-sched_fifosched_rr)
   - 3.4 [CFS 调度类 (SCHED_NORMAL/SCHED_BATCH/SCHED_IDLE)](#34-cfs-调度类-sched_normalsched_batchsched_idle)
   - 3.5 [Idle 调度类](#35-idle-调度类)
   - 3.6 [EXT 调度类 (SCHED_EXT)](#36-ext-调度类-sched_ext)
   - 3.7 [调度类优先级与遍历](#37-调度类优先级与遍历)
4. [核心调度流程](#4-核心调度流程)
   - 4.1 [schedule() 入口](#41-schedule-入口)
   - 4.2 [__schedule() 核心调度函数](#42-__schedule-核心调度函数)
   - 4.3 [pick_next_task() 任务选择](#43-pick_next_task-任务选择)
   - 4.4 [context_switch() 上下文切换](#44-context_switch-上下文切换)
   - 4.5 [调度模式 (sched_mode)](#45-调度模式-sched_mode)
   - 4.6 [PREEMPT_RT 调度整合](#46-preempt_rt-调度整合)
5. [CFS/EEVDF 完全公平调度器](#5-cfseevdf-完全公平调度器)
   - 5.1 [EEVDF 算法概述](#51-eedf-算法概述)
   - 5.2 [虚拟运行时间 (vruntime)](#52-虚拟运行时间-vruntime)
   - 5.3 [enqueue_task_fair / dequeue_task_fair](#53-enqueue_task_fair--dequeue_task_fair)
   - 5.4 [pick_next_task_fair 与延迟出队](#54-pick_next_task_fair-与延迟出队)
   - 5.5 [CFS 带宽控制 (cfs_bandwidth)](#55-cfs-带宽控制-cfs_bandwidth)
6. [实时调度 (RT)](#6-实时调度-rt)
   - 6.1 [SCHED_FIFO 与 SCHED_RR](#61-sched_fifo-与-sched_rr)
   - 6.2 [RT 优先级数组](#62-rt-优先级数组)
   - 6.3 [RT 推拉迁移](#63-rt-推拉迁移)
   - 6.4 [RT 带宽控制](#64-rt-带宽控制)
7. [截止时间调度 (SCHED_DEADLINE)](#7-截止时间调度-sched_deadline)
   - 7.1 [Earliest Deadline First (EDF) 算法](#71-earliest-deadline-first-edf-算法)
   - 7.2 [dl_rq 数据结构](#72-dl_rq-数据结构)
   - 7.3 [GRUB 带宽回收](#73-grub-带宽回收)
   - 7.4 [DL 推拉迁移](#74-dl-推拉迁移)
8. [负载均衡](#8-负载均衡)
   - 8.1 [调度域 (sched_domain) 与调度组 (sched_group)](#81-调度域-sched_domain-与调度组-sched_group)
   - 8.2 [负载均衡流程](#82-负载均衡流程)
   - 8.3 [新空闲负载均衡 (newidle_balance)](#83-新空闲负载均衡-newidle_balance)
   - 8.4 [主动均衡 (active_balance)](#84-主动均衡-active_balance)
   - 8.5 [misfit 任务迁移](#85-misfit-任务迁移)
9. [PELT —— Per-Entity Load Tracking](#9-pelt--per-entity-load-tracking)
   - 9.1 [PELT 基本原理](#91-pelt-基本原理)
   - 9.2 [struct sched_avg](#92-struct-sched_avg)
   - 9.3 [PELT 在负载均衡中的应用](#93-pelt-在负载均衡中的应用)
10. [任务组与 CGroup 集成](#10-任务组与-cgroup-集成)
    - 10.1 [struct task_group](#101-struct-task_group)
    - 10.2 [CFS 组调度 (CONFIG_FAIR_GROUP_SCHED)](#102-cfs-组调度-config_fair_group_sched)
    - 10.3 [RT 组调度 (CONFIG_RT_GROUP_SCHED)](#103-rt-组调度-config_rt_group_sched)
11. [Core Scheduling (CONFIG_SCHED_CORE)](#11-core-scheduling-config_sched_core)
    - 11.1 [核心调度原理](#111-核心调度原理)
    - 11.2 [core_cookie 与 SMT 隔离](#112-core_cookie-与-smt-隔离)
    - 11.3 [forceidle 机制](#113-forceidle-机制)
12. [sched_ext —— 可扩展调度 (BPF)](#12-sched_ext--可扩展调度-bpf)
    - 12.1 [架构概览](#121-架构概览)
    - 12.2 [SCX 运行队列与调度流程](#122-scx-运行队列与调度流程)
13. [NUMA 平衡](#13-numa-平衡)
    - 13.1 [NUMA 故障统计](#131-numa-故障统计)
    - 13.2 [NUMA 迁移策略](#132-numa-迁移策略)
14. [调度器特性与可调参数](#14-调度器特性与可调参数)
    - 14.1 [features.h 调度特性](#141-featuresh-调度特性)
    - 14.2 [关键可调参数](#142-关键可调参数)
15. [调度相关系统调用](#15-调度相关系统调用)
    - 15.1 [sched_setattr / sched_getattr](#151-sched_setattr--sched_getattr)
    - 15.2 [sched_yield](#152-sched_yield)
    - 15.3 [set_cpus_allowed / sched_setaffinity](#153-set_cpus_allowed--sched_setaffinity)
16. [调度器初始化](#16-调度器初始化)
    - 16.1 [sched_init()](#161-sched_init)
    - 16.2 [sched_init_smp()](#162-sched_init_smp)
17. [附录](#17-附录)
    - 17.1 [关键文件清单](#171-关键文件清单)
    - 17.2 [数据结构关系图](#172-数据结构关系图)

---

## 1. 概述

进程调度子系统是 Linux 内核的核心组件之一，负责决定哪个进程在何时、在哪个 CPU 上运行。其核心设计目标包括：

- **公平性**：确保所有进程获得合理的 CPU 时间份额
- **性能**：最小化调度开销，最大化系统吞吐量
- **实时性**：为实时任务提供确定性的调度保证
- **可扩展性**：在 SMP/NUMA 系统上高效扩展
- **可定制性**：通过 sched_ext 允许 BPF 程序实现自定义调度策略

Linux 调度器采用 **模块化调度类** 架构，每个调度类实现一种调度策略，通过统一的 `struct sched_class` 接口进行抽象。调度类按优先级排序，调度时从最高优先级调度类开始遍历，选择可运行的任务。

当前内核实现了 6 个调度类（按优先级从高到低）：

| 优先级 | 调度类 | 策略宏 | 调度策略 |
|--------|--------|--------|----------|
| 最高 | `stop_sched_class` | - | 每 CPU 停靠任务 |
| ↓ | `dl_sched_class` | `SCHED_DEADLINE` | 截止时间优先 |
| ↓ | `rt_sched_class` | `SCHED_FIFO` / `SCHED_RR` | 固定优先级实时 |
| ↓ | `fair_sched_class` | `SCHED_NORMAL` / `SCHED_BATCH` / `SCHED_IDLE` | CFS/EEVDF 完全公平 |
| ↓ | `ext_sched_class` | `SCHED_EXT` | BPF 可扩展调度 |
| 最低 | `idle_sched_class` | - | 空闲任务 |

---

## 2. 核心数据结构

### 2.1 task_struct 中的调度相关字段

`struct task_struct` 中与调度相关的核心字段（定义在 `include/linux/sched.h`）：

```c
struct task_struct {
    /* 调度策略与优先级 */
    unsigned int            policy;         // SCHED_NORMAL, SCHED_FIFO, 等
    int                     prio;           // 动态优先级
    int                     static_prio;    // 静态优先级 (nice 值映射)
    int                     normal_prio;    // 常规优先级
    unsigned int            rt_priority;    // 实时优先级 (1-99)

    /* 调度类 */
    const struct sched_class *sched_class;  // 指向当前调度类

    /* 调度实体 (嵌入各调度类的实体) */
    struct sched_entity     se;             // CFS 调度实体
    struct sched_rt_entity  rt;             // RT 调度实体
    struct sched_dl_entity  dl;             // Deadline 调度实体

    /* CPU 与运行队列状态 */
    unsigned int            cpu;            // 当前/上次运行的 CPU
    unsigned int            nr_cpus_allowed; // 允许的 CPU 数量
    cpumask_t               cpus_mask;      // CPU 亲和性掩码

    /* 运行队列状态 */
    int                     on_rq;          // TASK_ON_RQ_QUEUED/MIGRATING
    int                     on_cpu;         // 是否正在 CPU 上运行
    unsigned int            migration_disabled; // 迁移禁用标志
    unsigned int            wakee_flips;    // 唤醒翻转计数
    unsigned long           wakee_flip_decay_ts;

    /* Core Scheduling */
    unsigned long           core_cookie;    // 核心调度 cookie

    /* sched_ext */
    struct scx_task         scx;            // 扩展调度数据

    /* NUMA 平衡 */
    int                     numa_preferred_nid;
    unsigned long           *numa_faults;

    /* 时间统计 */
    u64                     utime, stime;   // 用户/系统时间
    u64                     gtime;          // 访客时间
    struct sched_info       sched_info;     // 调度统计信息
};
```

### 2.2 struct rq —— 每 CPU 运行队列

`struct rq` 是每 CPU 的核心调度数据结构，定义在 `kernel/sched/sched.h`。每个 CPU 有一个全局的 `runqueues` 数组 `DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues)`。

```c
struct rq {
    /* 热缓存行：频繁读取的字段 */
    unsigned int        nr_running;         // 本 CPU 可运行任务数
    unsigned int        ttwu_pending;       // 待处理的唤醒操作
    unsigned long       cpu_capacity;       // CPU 计算容量
    struct task_struct  *curr;              // 当前运行的任务
    struct task_struct  *idle;              // 空闲任务
    struct task_struct  *stop;              // 停靠任务

    /* 锁 */
    raw_spinlock_t      __lock;             // 运行队列锁

    /* 子运行队列 */
    struct cfs_rq       cfs;                // CFS 运行队列
    struct rt_rq        rt;                 // RT 运行队列
    struct dl_rq        dl;                 // Deadline 运行队列
#ifdef CONFIG_SCHED_CLASS_EXT
    struct scx_rq       scx;                // sched_ext 运行队列
#endif

    /* 调度域 */
    struct root_domain  *rd;                // 根域
    struct sched_domain *sd;                // 调度域

    /* 时钟 */
    u64                 clock;              // 运行队列时钟
    u64                 clock_task;         // 任务时钟 (除去中断时间)
    u64                 clock_pelt;         // PELT 时钟

    /* 负载均衡 */
    unsigned long       misfit_task_load;   // misfit 任务负载
    int                 active_balance;     // 主动均衡标志
    int                 push_cpu;           // 推送到目标 CPU
    struct cpu_stop_work active_balance_work;

    /* 统计 */
    s64                 nr_switches;        // 切换次数
    unsigned long       nr_uninterruptible; // 不可中断任务数

    /* Core Scheduling */
    struct rq           *core;              // 指向 core 运行队列
    unsigned int        core_enabled;
    unsigned long       core_cookie;
    struct rb_root      core_tree;

    /* 其他 */
    int                 cpu;                // 本 CPU 编号
    int                 online;             // CPU 在线状态
    struct list_head    cfs_tasks;          // CFS 任务列表（轮询用）
};
```

### 2.3 struct cfs_rq —— CFS 运行队列

```c
struct cfs_rq {
    struct load_weight      load;               // 运行队列总负载
    unsigned int            nr_queued;          // 队列中任务数
    unsigned int            h_nr_queued;        // SCHED_NORMAL/BATCH/IDLE 任务数
    unsigned int            h_nr_runnable;      // 可运行任务数
    unsigned int            h_nr_idle;          // SCHED_IDLE 任务数

    s64                     sum_w_vruntime;     // 加权虚拟运行时间和
    u64                     sum_weight;         // 权重和
    u64                     zero_vruntime;      // 零点虚拟运行时间

    struct rb_root_cached   tasks_timeline;     // 红黑树，按 vruntime 排序

    struct sched_entity     *curr;              // 当前正在运行的实体
    struct sched_entity     *next;              // 下一个候选实体

    struct sched_avg        avg;                // PELT 平均负载

#ifdef CONFIG_FAIR_GROUP_SCHED
    struct rq               *rq;                // 所属 CPU 运行队列
    struct task_group       *tg;                // 所属任务组
    struct list_head        leaf_cfs_rq_list;   // 叶子 cfs_rq 链表

    /* CFS 带宽控制 */
    int                     runtime_enabled;
    s64                     runtime_remaining;
    bool                    throttled;
    u64                     throttled_clock;
#endif
};
```

### 2.4 struct rt_rq —— 实时运行队列

```c
struct rt_rq {
    struct rt_prio_array    active;             // 优先级位图 + 队列数组
    unsigned int            rt_nr_running;      // 运行中的 RT 任务数
    unsigned int            rr_nr_running;      // SCHED_RR 任务数
    struct {
        int curr;                               // 当前最高 RT 优先级
        int next;                               // 次高优先级
    } highest_prio;
    bool                    overloaded;          // 是否过载
    struct plist_head       pushable_tasks;     // 可推送任务列表

    int                     rt_queued;

#ifdef CONFIG_RT_GROUP_SCHED
    int                     rt_throttled;
    u64                     rt_time;
    u64                     rt_runtime;
    struct rq               *rq;
#endif
};
```

### 2.5 struct dl_rq —— 截止时间运行队列

```c
struct dl_rq {
    struct rb_root_cached   root;               // 红黑树，按截止时间排序
    unsigned int            dl_nr_running;      // 运行中的 DL 任务数

    struct {
        u64 curr;                               // 当前任务截止时间
        u64 next;                               // 最早可运行任务截止时间
    } earliest_dl;

    bool                    overloaded;
    struct rb_root_cached   pushable_dl_tasks_root; // 可推送任务树

    u64                     running_bw;         // 当前运行带宽
    u64                     this_bw;            // 已分配带宽
    u64                     extra_bw;           // 额外带宽
    u64                     max_bw;             // 最大可回收带宽
    u64                     bw_ratio;           // 带宽比例 (GRUB)
};
```

### 2.6 struct sched_class —— 调度类接口

`struct sched_class` 定义在 `kernel/sched/sched.h` 的 2500 行，是调度器体系的核心抽象。每个调度类实现一组回调函数：

```c
struct sched_class {
#ifdef CONFIG_UCLAMP_TASK
    int uclamp_enabled;
#endif

    /* 入队/出队 */
    void (*enqueue_task)(struct rq *rq, struct task_struct *p, int flags);
    bool (*dequeue_task)(struct rq *rq, struct task_struct *p, int flags);

    /* 主动让出 CPU */
    void (*yield_task)(struct rq *rq);
    bool (*yield_to_task)(struct rq *rq, struct task_struct *p);

    /* 唤醒抢占检查 */
    void (*wakeup_preempt)(struct rq *rq, struct task_struct *p, int flags);

    /* 负载均衡 */
    int (*balance)(struct rq *rq, struct task_struct *prev, struct rq_flags *rf);

    /* 任务选择 */
    struct task_struct *(*pick_task)(struct rq *rq, struct rq_flags *rf);
    struct task_struct *(*pick_next_task)(struct rq *rq, struct task_struct *prev,
                                          struct rq_flags *rf);

    /* 任务切换 */
    void (*put_prev_task)(struct rq *rq, struct task_struct *p, struct task_struct *next);
    void (*set_next_task)(struct rq *rq, struct task_struct *p, bool first);

    /* CPU 选择 */
    int (*select_task_rq)(struct task_struct *p, int task_cpu, int flags);
    void (*migrate_task_rq)(struct task_struct *p, int new_cpu);

    /* 任务唤醒后处理 */
    void (*task_woken)(struct rq *this_rq, struct task_struct *task);

    /* CPU 亲和性 */
    void (*set_cpus_allowed)(struct task_struct *p, struct affinity_context *ctx);

    /* CPU 热插拔 */
    void (*rq_online)(struct rq *rq);
    void (*rq_offline)(struct rq *rq);

    /* 迁移锁 */
    struct rq *(*find_lock_rq)(struct task_struct *p, struct rq *rq);

    /* 时钟节拍 */
    void (*task_tick)(struct rq *rq, struct task_struct *p, int queued);
    void (*task_fork)(struct task_struct *p);
    void (*task_dead)(struct task_struct *p);

    /* 调度类切换 */
    void (*switching_from)(struct rq *this_rq, struct task_struct *task);
    void (*switched_from)(struct rq *this_rq, struct task_struct *task);
    void (*switching_to)(struct rq *this_rq, struct task_struct *task);
    void (*switched_to)(struct rq *this_rq, struct task_struct *task);
    u64  (*get_prio)(struct rq *this_rq, struct task_struct *task);
    void (*prio_changed)(struct rq *this_rq, struct task_struct *task, u64 oldprio);

    /* 权重重新计算 */
    void (*reweight_task)(struct rq *this_rq, struct task_struct *task,
                          const struct load_weight *lw);

    /* RR 时间片 */
    unsigned int (*get_rr_interval)(struct rq *rq, struct task_struct *task);

    /* 当前任务时间更新 */
    void (*update_curr)(struct rq *rq);

#ifdef CONFIG_FAIR_GROUP_SCHED
    void (*task_change_group)(struct task_struct *p);
#endif

#ifdef CONFIG_SCHED_CORE
    int (*task_is_throttled)(struct task_struct *p, int cpu);
#endif
};
```

每个调度类实例通过 `DEFINE_SCHED_CLASS(name)` 宏定义，该宏将调度类放入特定的链接器段中，链接器在 `__sched_class_highest[]` 和 `__sched_class_lowest[]` 之间按优先级排列：

```c
#define DEFINE_SCHED_CLASS(name) \
const struct sched_class name##_sched_class \
    __aligned(__alignof__(struct sched_class)) \
    __section("__" #name "_sched_class")
```

### 2.7 struct sched_entity / sched_rt_entity / sched_dl_entity

每个任务在 `task_struct` 中嵌入了三个调度实体，分别对应不同的调度类：

```c
struct sched_entity {
    struct load_weight      load;           // 权重
    struct rb_node          run_node;       // 红黑树节点
    struct list_head        group_node;     // 组节点
    unsigned int            on_rq;          // 是否在运行队列上

    u64                     vruntime;       // 虚拟运行时间 (EEVDF 核心)
    u64                     vlag;           // 虚拟滞后
    u64                     deadline;       // 虚拟截止时间
    u64                     slice;          // 时间片长度

    u64                     sum_exec_runtime;   // 总执行时间
    u64                     prev_sum_exec_runtime; // 上次统计
    u64                     nr_migrations;

    struct sched_statistics statistics;      // 调度统计
#ifdef CONFIG_FAIR_GROUP_SCHED
    struct sched_entity     *parent;         // 父实体
    struct cfs_rq           *cfs_rq;         // 所属 cfs_rq
    struct cfs_rq           *my_q;           // 子 cfs_rq (组调度)
    int                     depth;           // 层级深度
#endif
    struct sched_avg        avg;             // PELT 平均负载
};

struct sched_rt_entity {
    struct list_head        run_list;        // 优先级队列链表节点
    unsigned long           timeout;         // 超时时间
    unsigned int            time_slice;      // RR 时间片

    struct sched_rt_entity  *back;           // 链表上一个
    struct sched_rt_entity  *parent;         // 父实体
    struct rt_rq            *rt_rq;          // 所属 rt_rq
    struct rt_rq            *my_q;           // 子 rt_rq (组调度)
};

struct sched_dl_entity {
    struct rb_node          rb_node;         // 红黑树节点

    unsigned long           dl_runtime;      // 最大运行时
    unsigned long           dl_deadline;     // 相对截止时间
    unsigned long           dl_period;       // 周期
    u64                     dl_bw;           // 带宽

    u64                     runtime;         // 剩余运行时
    u64                     deadline;        // 绝对截止时间

    int                     dl_throttled;    // 是否被节流
    int                     dl_boosted;      // 是否被提升
    int                     dl_yielded;      // 是否让出
    int                     dl_non_contending; // 非竞争状态
    int                     dl_overrun;      // 是否超限

    struct hrtimer          dl_timer;        // 截止时间定时器

    struct sched_dl_entity  *pi_se;          // PI 继承实体
};
```

---

## 3. 调度类层次结构

### 3.1 Stop 调度类

**文件**: `kernel/sched/stop_task.c`

优先级最高的调度类，用于每 CPU 的停靠任务 (`migration/N`)。该调度类极其简单，只有一个任务（`rq->stop`），不可抢占，不可让出 CPU。

关键特性：
- 当 `rq->stop` 任务被入队时，它总是被选择运行
- 用于 `stop_machine` 机制，如 CPU 热插拔、迁移
- 没有 `enqueue_task` 的实际实现（任务已经在运行队列中）
- `pick_task_stop`：直接返回 `rq->stop`

### 3.2 Deadline 调度类 (SCHED_DEADLINE)

**文件**: `kernel/sched/deadline.c`

实现 Earliest Deadline First (EDF) 算法，结合 Constant Bandwidth Server (CBS) 和 GRUB (Greedy Reclamation of Unused Bandwidth) 带宽回收机制。

关键特性：
- 任务指定 `runtime`、`deadline`、`period` 参数
- 使用红黑树按绝对截止时间排序，最早截止时间优先
- CBS 防止任务超过声明的带宽
- GRUB 机制允许回收未使用的带宽
- 支持推拉迁移（`pushable_dl_tasks_root`）
- 使用高精度定时器 (`hrtimer`) 跟踪截止时间

### 3.3 Real-Time 调度类 (SCHED_FIFO/SCHED_RR)

**文件**: `kernel/sched/rt.c`

实现 POSIX 实时调度策略，优先级范围 0-99（数值越大优先级越高）。

**SCHED_FIFO** (First-In-First-Out)：
- 没有时间片限制，运行直到主动让出或被更高优先级任务抢占
- 相同优先级任务按 FIFO 顺序运行

**SCHED_RR** (Round-Robin)：
- 与 SCHED_FIFO 类似，但相同优先级任务有固定时间片 (`sched_rr_timeslice`)
- 时间片用完时，任务被移动到队列末尾

核心数据结构：
- `struct rt_prio_array`：优先级位图 + 链表数组，实现 O(1) 调度
- 每个优先级对应一个链表，位图快速找到最高优先级非空队列

### 3.4 CFS 调度类 (SCHED_NORMAL/SCHED_BATCH/SCHED_IDLE)

**文件**: `kernel/sched/fair.c`

实现默认的完全公平调度器，当前版本使用 EEVDF (Earliest Eligible Virtual Deadline First) 算法，是 CFS 的演进。

**SCHED_NORMAL**：标准分时调度策略
**SCHED_BATCH**：批处理（减少唤醒抢占）
**SCHED_IDLE**：极低优先级（仅在其他任务不运行时运行）

核心算法详见第 5 节。

### 3.5 Idle 调度类

**文件**: `kernel/sched/idle.c`

优先级最低的调度类，对应每 CPU 的空闲任务。当 CPU 上没有其他可运行任务时，idle 任务运行，进入 CPU 空闲状态（如 `cpuidle`）。

关键特性：
- `pick_task_idle`：返回 `rq->idle`
- `dequeue_task_idle`：不合法操作，触发 WARN
- 没有 `enqueue_task` 实现

### 3.6 EXT 调度类 (SCHED_EXT)

**文件**: `kernel/sched/ext.c`, `kernel/sched/ext.h`, `kernel/sched/ext_idle.c`

sched_ext 是 Linux 内核的可扩展调度框架，允许通过 BPF 程序实现自定义调度策略，无需修改内核代码。

关键特性：
- 通过 BPF 加载自定义调度器
- `scx_enabled()` 检查是否加载了 BPF 调度器
- `scx_switched_all()` 检查是否所有 fair 任务都被接管
- SCX 运行队列 (`struct scx_rq`) 包含本地调度队列 (`local_dsq`) 和可运行任务列表
- BPF 调度器通过 ops 回调（`ops.enqueue`, `ops.dispatch` 等）实现调度逻辑

### 3.7 调度类优先级与遍历

调度类按链接器段的排列顺序决定优先级，使用 `sched_class_above()` 宏比较：

```c
#define sched_class_above(_a, _b)  ((_a) < (_b))
#define for_each_active_class(class) \
    for_active_class_range(class, __sched_class_highest, __sched_class_lowest)
```

`class` 指针值越小，优先级越高。`next_active_class()` 负责跳过当前不活跃的类（如 fair 被 SCX 接管时跳过 fair，SCX 未启用时跳过 ext）。

---

## 4. 核心调度流程

### 4.1 schedule() 入口

`schedule()` 是调度器的主要入口点，定义在 `kernel/sched/core.c` 的 6998 行：

```c
asmlinkage __visible void __sched schedule(void)
{
    struct task_struct *tsk = current;

    if (!task_is_running(tsk))
        sched_submit_work(tsk);    // 提交阻塞 I/O 工作
    __schedule_loop(SM_NONE);      // 进入调度循环
}
```

其他入口点：
- `schedule_user()`：从用户态返回时调度
- `preempt_schedule()`：抢占调度入口
- `preempt_schedule_irq()`：中断返回时的抢占调度
- `preempt_schedule_notrace()`：trace 友好的抢占调度

### 4.2 __schedule() 核心调度函数

`__schedule()` 定义在 `kernel/sched/core.c` 的 6764 行，是调度的核心实现：

```
__schedule(sched_mode):
  ├── 获取当前 CPU 的 rq 和 prev 任务
  ├── schedule_debug(prev, preempt)          // 检查调度正确性
  ├── rq_lock(rq, &rf)                      // 获取运行队列锁
  ├── update_rq_clock(rq)                   // 更新时间戳
  ├── 处理 prev 任务状态：
  │   ├── SM_IDLE: 若 rq 为空，直接 goto picked
  │   └── 非抢占且 prev_state 非 RUNNING:
  │       └── try_to_block_task() 尝试出队
  │
  ├── pick_next_task(rq, prev, &rf)         // 选择下一个任务
  │
  ├── context_switch(rq, prev, next, &rf)   // 上下文切换
  │   └── 返回新的 rq（可能因迁移而不同）
  │
  └── 解锁与清理
```

### 4.3 pick_next_task() 任务选择

`pick_next_task()` 是调度类遍历的核心，定义在 `kernel/sched/core.c` 的 5909 行：

```
__pick_next_task(rq, prev, rf):
  ├── 优化路径：若所有任务都是 fair 类任务
  │   └── 直接调用 pick_next_task_fair()
  │
  ├── 常规路径：遍历调度类
  │   for_each_active_class(class):
  │     if class->pick_next_task:
  │       p = class->pick_next_task(rq, prev, rf)
  │     else:
  │       p = class->pick_task(rq, rf)
  │       put_prev_set_next_task(rq, prev, p)
  │     if p: return p
  │
  └── 后备：idle 类必须始终返回任务
```

当 `CONFIG_SCHED_CORE` 启用时，`pick_next_task()` 需要执行核心调度选择（详见第 11 节）。

### 4.4 context_switch() 上下文切换

`context_switch()` 执行实际的 CPU 上下文切换，包括：

**切换 MM**：
- 若 prev 和 next 的地址空间不同，调用 `switch_mm()` 切换页表
- 若相同，调用 `switch_mm_irqs_off()` 进行轻量级切换

**切换寄存器**：
- 调用 `switch_to()` 架构相关函数
- 保存 prev 的寄存器，恢复 next 的寄存器
- 切换栈指针 (SP)

**finish_task_switch()**：
- 清理 prev 任务的资源
- 释放运行队列锁
- 处理 RCU 通知

### 4.5 调度模式 (sched_mode)

`__schedule()` 接收 `sched_mode` 参数，区分不同的调度触发原因：

```c
#define SM_IDLE         (-1)    // 空闲时调度
#define SM_NONE         0       // 普通调度
#define SM_PREEMPT      1       // 抢占调度
#define SM_RTLOCK_WAIT  2       // RT 锁等待
```

- `preempt = sched_mode > SM_NONE` 判断是否为抢占
- `preempt = sched_mode == SM_PREEMPT` 用于任务状态跟踪

### 4.6 PREEMPT_RT 调度整合

PREEMPT_RT 开启后，调度器新增了以下关键机制：

#### 4.6.1 schedule_rtlock() —— RT 锁调度入口

```c
// kernel/sched/core.c:7069
#ifdef CONFIG_PREEMPT_RT
void __sched notrace schedule_rtlock(void)
{
    __schedule_loop(SM_RTLOCK_WAIT);
}
#endif
```

`schedule_rtlock()` 用于 RT 锁（`spinlock_t` 在 RT 下的 rt_mutex 实现）等待时的调度：
- 使用 `SM_RTLOCK_WAIT` 调度模式
- `sched_mode > SM_NONE` 使其被 `schedule_debug()` 和 RCU 视为一次抢占
- 但实际上是任务主动让出 CPU 等待锁，与普通抢占的触发路径不同

#### 4.6.2 TASK_RTLOCK_WAIT 任务状态

```c
// include/linux/sched.h:123
#define TASK_RTLOCK_WAIT    0x00001000
```

新增的任务状态，用于 RT 锁等待。状态转换通过 `current_save_and_set_rtlock_wait_state()` 和 `current_restore_rtlock_saved_state()` 宏管理：
- 保存原始任务状态到 `task_struct->saved_state`
- 设置 `__state = TASK_RTLOCK_WAIT`
- 锁获取后恢复原始状态
- `ttwu_state_match()` 将其视为不可中断等待

#### 4.6.3 TTWU_QUEUE 特性关闭

```c
// kernel/sched/features.h:74-77
#ifdef CONFIG_PREEMPT_RT
SCHED_FEAT(TTWU_QUEUE, false)
#endif
```

PREEMPT_RT 下关闭 TTWU_QUEUE 特性，远程唤醒直接发送 IPI 而不是通过 ksoftirqd 排队，降低唤醒延迟。

#### 4.6.4 SCHED_NR_MIGRATE_BREAK 减小

```c
// kernel/sched/sched.h:3007-3011
#ifdef CONFIG_PREEMPT_RT
# define SCHED_NR_MIGRATE_BREAK 8
#else
# define SCHED_NR_MIGRATE_BREAK 32
#endif
```

负载均衡时每次迁移处理的任务数量从 32 减少到 8，减少锁持有时间，降低调度延迟。

#### 4.6.5 migrate_disable 替代 preempt_disable

PREEMPT_RT 下，许多原语使用 `migrate_disable()` 替代 `preempt_disable()` 保护 per-CPU 数据：
- `preempt_disable()`：禁用抢占，防止迁移，同时保证原子性
- `migrate_disable()`：允许抢占，但防止任务迁移到其他 CPU
- 这使得更高优先级的任务获得更低的唤醒延迟

---

## 5. CFS/EEVDF 完全公平调度器

### 5.1 EEVDF 算法概述

CFS 调度器从 Linux 6.6 开始采用 EEVDF (Earliest Eligible Virtual Deadline First) 算法，取代了传统的基于 vruntime 最小值的简单选择策略。

EEVDF 核心概念：
- **Eligible (符合条件的)**：任务的虚拟运行时间必须不大于当前时间点的虚拟时间，否则不可选择
- **Virtual Deadline (虚拟截止时间)**：每个任务分配一个虚拟截止时间，选择最早截止时间的 eligible 任务

关键公式：
- 虚拟时间: `V = sum(w_i * v_i) / sum(w_i)`，其中 w_i 是权重，v_i 是虚拟运行时间
- tracked as: `v0 = cfs_rq->zero_vruntime`, `sum(w_i * (v_i - v0)) = cfs_rq->sum_w_vruntime`, `sum(w_i) = cfs_rq->sum_weight`
- 虚拟截止时间: `deadline = vruntime + slice / weight`
- 滞后 (lag): `lag = v_i - V`

### 5.2 虚拟运行时间 (vruntime)

vruntime 是 EEVDF 的基础，表示任务实际运行时间的加权度量：

```
vruntime += delta_exec * NICE_0_LOAD / weight
```

- 较高权重的任务 vruntime 增长较慢，因此获得更多 CPU 时间
- nice 值映射到权重表 `sched_prio_to_weight[]`
- 64 位架构使用更高的精度: `NICE_0_LOAD_SHIFT = SCHED_FIXEDPOINT_SHIFT * 2`

### 5.3 enqueue_task_fair / dequeue_task_fair

**enqueue_task_fair**：
1. 检查延迟出队任务（`finish_delayed_dequeue_entity`）
2. 更新 `h_nr_runnable` 计数
3. 调用 `enqueue_entity()` 将实体插入红黑树
4. 计算平均负载（`update_load_avg`）
5. 检查带宽控制（`check_enqueue_throttle`）
6. CGroup 支持：沿层级向上传播负载

**dequeue_task_fair**：
1. 调用 `dequeue_entity()` 从红黑树移除
2. 若 `DELAY_DEQUEUE` 特性启用，对于非 eligible 任务仅标记 `sched_delayed` 而不实际移除
3. 更新负载统计
4. 沿层级向上传播

### 5.4 pick_next_task_fair 与延迟出队

`pick_next_task_fair()` 是 CFS 的任务选择函数：

1. 调用 `pick_task_fair()` 选择 eligible 且 deadline 最早的任务
2. 若没有任务，进入 idle 路径
3. CGroup 优化：若 prev 和 next 在同一个 cgroup，只切换必要的层级
4. 处理延迟出队任务：实际出队并重新选择

延迟出队（`DELAY_DEQUEUE`）特性：
- 当任务被抢占时，如果不是 eligible 任务，不会被实际出队
- 延迟出队的任务留在红黑树中，可以继续消耗其负滞后
- 当任务被选中时，其滞后会自动变为正数
- 减少不必要的出队/入队操作，提高性能

### 5.5 CFS 带宽控制 (cfs_bandwidth)

CFS 带宽控制允许限制任务组在给定周期内的 CPU 使用量：

```c
struct cfs_bandwidth {
    raw_spinlock_t      lock;
    ktime_t             period;             // 周期
    u64                 quota;              // 配额
    s64                 hierarchical_quota; // 层级配额
    int                 idle;              // 空闲状态
    struct hrtimer      period_timer;       // 周期定时器
    struct hrtimer      slack_timer;        // 松弛定时器
    struct list_head    throttled_cfs_rq;   // 被节流的 cfs_rq 列表
};
```

- 当 `cfs_rq->runtime_remaining` 耗尽时，cfs_rq 被节流 (throttled)
- 节流的 cfs_rq 不会调度任务，直到下一个周期
- `throttled_list` 跟踪所有被节流的 cfs_rq

---

## 6. 实时调度 (RT)

### 6.1 SCHED_FIFO 与 SCHED_RR

**SCHED_FIFO**：
- 没有时间片，运行直到主动让出（`sched_yield`）、休眠或被更高优先级抢占
- 相同优先级任务按队列顺序依次运行

**SCHED_RR**：
- 与 FIFO 类似，但相同优先级任务有固定时间片
- 时间片用完 → 移动到队列末尾，重新调度
- 时间片为 `sched_rr_timeslice = 100ms`（默认）

### 6.2 RT 优先级数组

`struct rt_prio_array` 是 RT 调度类的核心数据结构，实现 O(1) 调度：

```c
struct rt_prio_array {
    DECLARE_BITMAP(bitmap, MAX_RT_PRIO+1);  // 101 位位图
    struct list_head queue[MAX_RT_PRIO];     // 100 个链表
};
```

- 优先级 0-99 各对应一个链表
- 位图用于快速查找最高优先级非空队列
- 使用 `__ffs()` 或 `find_first_bit()` 找到第一个置位位
- 入队/出队操作均为 O(1)

### 6.3 RT 推拉迁移

RT 调度类实现推拉迁移机制，确保实时任务获得确定的 CPU 时间：

**Push 机制**（`push_rt_task`）：
- 当高优先级任务入队时，检查是否有低优先级任务正在运行
- 尝试将低优先级任务推送到其他 CPU
- 使用 `find_lock_lowest_rq()` 找到最低优先级的目标 CPU

**Pull 机制**（`pull_rt_task`）：
- 当 CPU 空闲时，从其他 CPU 的 RT 队列拉取任务
- `rt_rq->pushable_tasks` 优先级列表存储可推送的任务
- 需要 `HAVE_RT_PUSH_IPI` 配置支持 IPI 推送

### 6.4 RT 带宽控制

RT 带宽控制防止实时任务消耗过多 CPU 时间：

```c
struct rt_bandwidth {
    raw_spinlock_t      rt_runtime_lock;
    ktime_t             rt_period;          // 周期 (默认 1s)
    u64                 rt_runtime;         // 运行时 (默认 0.95s)
    struct hrtimer      rt_period_timer;    // 周期定时器
};
```

- `sysctl_sched_rt_period`：RT 周期 (us)
- `sysctl_sched_rt_runtime`：每个周期内 RT 任务的最大运行时间 (us)
- `rt_runtime = -1` 表示禁用带宽限制

---

## 7. 截止时间调度 (SCHED_DEADLINE)

### 7.1 Earliest Deadline First (EDF) 算法

SCHED_DEADLINE 使用 EDF 算法，结合 Constant Bandwidth Server (CBS) 机制：

- 每个任务声明 `runtime`、`deadline`、`period` 三个参数
- 任务在周期开始时获得 `runtime` 时间配额
- 必须在 `deadline` 之前完成
- 使用红黑树按绝对截止时间排序，最早截止时间优先

CBS 机制：
- 防止任务超过声明的带宽
- 当任务用完 `runtime` 时，被节流直到下一个周期
- 若任务提前用完 `runtime`，其截止时间被推迟到下一个周期

### 7.2 dl_rq 数据结构

`struct dl_rq` 管理截止时间任务：

- `root`：红黑树，按截止时间排序
- `earliest_dl.curr`：当前运行任务的截止时间
- `earliest_dl.next`：最早可运行任务的截止时间
- `pushable_dl_tasks_root`：可推送任务树（用于迁移）
- `running_bw`：当前活跃带宽
- `this_bw`：已分配总带宽
- `extra_bw` / `max_bw`：GRUB 带宽回收用

### 7.3 GRUB 带宽回收

GRUB (Greedy Reclamation of Unused Bandwidth) 允许截止时间任务回收系统中未使用的带宽：

- `running_bw`：当前实际使用的带宽
- `this_bw`：已分配给该 CPU 的任务的总带宽
- `max_bw`：最大可回收带宽
- `bw_ratio`：带宽比例，用于计算回收量
- 当任务运行时，`running_bw` 增加；当任务阻塞时，`running_bw` 减少
- 未使用的带宽（`max_bw - running_bw`）可被其他任务回收

### 7.4 DL 推拉迁移

与 RT 调度类似，DL 调度类也实现推拉迁移：

**Push 机制**（`push_dl_task`）：
- 当新的截止时间任务入队时，检查是否有更晚截止时间的任务在运行
- 尝试将晚截止时间任务推送到其他 CPU
- 使用 `find_lock_later_rq()` 找到目标 CPU

**Pull 机制**（`pull_dl_task`）：
- 当 CPU 空闲时，从其他 CPU 拉取截止时间最早的任务
- 基于 `pushable_dl_tasks_root` 红黑树

---

## 8. 负载均衡

### 8.1 调度域 (sched_domain) 与调度组 (sched_group)

调度器使用层级调度域结构管理 CPU 拓扑：

```
struct sched_domain {
    struct sched_domain *parent;     // 父域
    struct sched_domain *child;      // 子域
    struct sched_group *groups;      // 调度组
    unsigned long flags;             // 域标志
    unsigned int level;              // 层级
    unsigned int nr_balance_failed;  // 均衡失败次数
    unsigned int span_weight;        // CPU 数量
    struct cpumask *span;            // CPU 位图
    char *name;                      // 域名称
};
```

典型调度域层次（以 ARM64 为例）：
```
MC (Multi-Core) 域: 包含所有 CPU
  └── SMT 域: 包含 SMT 线程（如果支持）
```

每个调度域包含多个调度组（`struct sched_group`），负载均衡就是通过域层次在组之间迁移任务。

### 8.2 负载均衡流程

负载均衡的核心函数是 `sched_balance_rq()`，在 `update_sd_lb_stats()` 中收集统计信息：

```
rebalance_domains(rq):
  └── 遍历每个调度域 (从最底层开始)
      └── load_balance(cpu, rq, sd, idle)
          ├── find_busiest_group()     // 找到最忙的组
          ├── find_busiest_queue()     // 找到最忙的 CPU
          └── move_tasks() / detach_tasks()  // 迁移任务
```

### 8.3 新空闲负载均衡 (newidle_balance)

当 CPU 变为空闲时，`newidle_balance()` 尝试从其他 CPU 拉取任务：

- 仅在 CPU 进入空闲状态时触发
- 从底层调度域开始向上遍历
- 使用 `sched_balance_find_dst_cpu()` 找到目标 CPU
- `CONFIG_NO_HZ_COMMON` 支持 tickless 空闲 CPU 的负载均衡

### 8.4 主动均衡 (active_balance)

当检测到严重负载不均时，启动主动均衡：

- `rq->active_balance = 1` 标记主动均衡
- 使用 `stop_one_cpu()` 在目标 CPU 上执行均衡
- `active_balance_work` 处理实际的迁移操作
- 适用于 misfit 任务迁移等场景

### 8.5 misfit 任务迁移

misfit 任务是指负载超过当前 CPU 容量的任务，需要迁移到更高容量的 CPU：

- `rq->misfit_task_load` 跟踪 misfit 负载
- 在 `update_sg_lb_stats()` 中检测 misfit 任务
- 常见于 big.LITTLE 架构（容量不同的 CPU 混合）
- 使用 `arch_asym_cpu_priority()` 确定 CPU 优先级

---

## 9. PELT —— Per-Entity Load Tracking

### 9.1 PELT 基本原理

PELT (Per-Entity Load Tracking) 跟踪每个调度实体（任务或任务组）和运行队列的平均负载，使用半衰期衰减算法：

- 负载采样以 1024us 为周期
- 历史负载以指数衰减方式叠加
- 半衰期约为 32ms（经过 32 个周期衰减一半）
- 公式：`load = load * y + delta`，其中 `y = 0.978...`（32 次方近似 0.5）

### 9.2 struct sched_avg

```c
struct sched_avg {
    u64             last_update_time;   // 最后更新时间
    u64             load_sum;           // 负载和
    u64             runnable_sum;       // 可运行负载和
    unsigned long   load_avg;           // 平均负载 (load_sum / 1024)
    unsigned long   runnable_avg;       // 平均可运行负载
    unsigned long   util_avg;           // 平均利用率
    unsigned long   util_est;           // 估计利用率 (util_avg + 抖动)
};
```

- `load_avg`：跟踪任务权重（weight）的负载
- `runnable_avg`：跟踪任务是否可运行
- `util_avg`：跟踪任务实际 CPU 使用率
- `util_est`：util_avg 加上最近最大值的估计，用于快速响应负载变化

### 9.3 PELT 在负载均衡中的应用

PELT 在负载均衡中提供关键的负载度量：

- **CPU 容量**：`cpu_capacity = capacity_orig * (1 - 利用率/效用)`
- **任务负载**：`task_util(p) = p->se.avg.util_avg`
- **运行队列负载**：`cpu_load(rq) = cfs_rq_load_avg(&rq->cfs)`
- 用于调度域级别的不均衡检测
- 用于 EAS (Energy-Aware Scheduling) 的能源计算

---

## 10. 任务组与 CGroup 集成

### 10.1 struct task_group

```c
struct task_group {
    struct cgroup_subsys_state css;

#ifdef CONFIG_FAIR_GROUP_SCHED
    struct sched_entity **se;           // 每 CPU 的调度实体
    struct cfs_rq **cfs_rq;            // 每 CPU 的 CFS 运行队列
    unsigned long shares;              // 组权重
    atomic_long_t load_avg;            // 组负载
#endif

#ifdef CONFIG_RT_GROUP_SCHED
    struct sched_rt_entity **rt_se;
    struct rt_rq **rt_rq;
    struct rt_bandwidth rt_bandwidth;
#endif

    struct task_group *parent;         // 父组
    struct list_head children;         // 子组列表
    struct list_head siblings;         // 兄弟组列表

    struct cfs_bandwidth cfs_bandwidth; // CFS 带宽控制
};
```

### 10.2 CFS 组调度 (CONFIG_FAIR_GROUP_SCHED)

CFS 组调度允许将任务分组，为每个组分配 CPU 时间份额：

- 每个 CPU 在每个层级上有一个 `cfs_rq` 和 `sched_entity`
- 组实体（`task_group->se[cpu]`）挂载到父组的 `cfs_rq` 上
- 组内的 `cfs_rq`（`task_group->cfs_rq[cpu]`）管理组内任务
- `leaf_cfs_rq_list` 连接所有叶子 `cfs_rq`，用于负载均衡
- `shares` 控制组之间的 CPU 时间分配比例

### 10.3 RT 组调度 (CONFIG_RT_GROUP_SCHED)

RT 组调度与 CFS 组调度类似，为 RT 任务提供组级别带宽控制：

- 每个组有 `rt_bandwidth` 控制 RT 带宽
- 组内 RT 任务共享组带宽
- `rt_rq->rt_time` / `rt_rq->rt_runtime` 跟踪组级别带宽使用

---

## 11. Core Scheduling (CONFIG_SCHED_CORE)

### 11.1 核心调度原理

核心调度 (Core Scheduling) 是 SMT 安全特性，确保同一物理核心上的 SMT 线程只运行来自同一信任域的任务：

- 通过 `core_cookie` 标识任务的信任域
- `pick_next_task()` 进行核心级选择，确保 SMT 线程上运行的任务具有相同 cookie
- 当 cookie 不匹配时，强制空闲（forceidle）

### 11.2 core_cookie 与 SMT 隔离

- `sched_core_alloc_cookie()` / `sched_core_put_cookie()`：cookie 引用计数管理
- `sched_core_cookie_match()`：检查任务 cookie 是否匹配核心 cookie
- `sched_core_idle_cpu()`：检查核心是否空闲（用于 SMT 隔离）
- `core_tree`：每个核心的红黑树，按 cookie 分组

### 11.3 forceidle 机制

当 SMT 线程上的任务 cookie 不匹配时，无法运行的任务进入 forceidle 状态：

- `rq->core_forceidle_count`：forceidle 计数
- `rq->core_forceidle_occupation`：forceidle 空间占用
- `__sched_core_account_forceidle()`：统计 forceidle 时间
- `__account_forceidle_time()`：记录到任务统计中

---

## 12. sched_ext —— 可扩展调度 (BPF)

### 12.1 架构概览

sched_ext 允许通过 BPF 程序加载自定义调度策略，提供完整的调度灵活性：

- BPF 调度器通过 `SCHED_EXT` 策略注册
- 当 BPF 调度器加载时，`scx_enabled()` 返回 true
- 可选接管所有 fair 类任务（`scx_switched_all()`）
- BPF 调度器通过 ops 结构与内核交互

### 12.2 SCX 运行队列与调度流程

```c
struct scx_rq {
    struct scx_dispatch_q   local_dsq;          // 本地调度队列
    struct list_head        runnable_list;      // 可运行任务列表
    struct list_head        ddsp_deferred_locals; // 延迟调度
    unsigned long           ops_qseq;
    u32                     nr_running;
    u32                     flags;
    cpumask_var_t           cpus_to_kick;        // 需要 IPI 的 CPU
    cpumask_var_t           cpus_to_kick_if_idle;
    cpumask_var_t           cpus_to_preempt;
};
```

调度流程：
1. `ops.enqueue()`：BPF 调度器将任务入队到调度队列
2. `ops.dispatch()`：BPF 调度器将任务分发到 DSQ
3. 内核从 DSQ 中取出任务运行
4. `ops.tick()`：时钟节拍通知 BPF 调度器

---

## 13. NUMA 平衡

### 13.1 NUMA 故障统计

NUMA 平衡通过跟踪内存访问故障（fault）来优化内存访问局部性：

- `task_struct->numa_faults[]`：按节点和类型统计的故障计数
- `numa_preferred_nid`：首选 NUMA 节点
- 使用 `task_numa_fault()` 记录每次 NUMA 故障
- 定期扫描（`task_numa_work()`）检查和迁移页面

### 13.2 NUMA 迁移策略

- 当任务在非本地节点上产生大量故障时，触发迁移
- `numa_migrate_on`：正在迁移到该 CPU 的任务数
- 使用 `sched_setnuma()` 更新首选节点
- 自动平衡节点间的任务分布

---

## 14. 调度器特性与可调参数

### 14.1 features.h 调度特性

`kernel/sched/features.h` 定义了调度器特性开关：

| 特性 | 默认值 | 说明 |
|------|--------|------|
| `PLACE_LAG` | on | 使用 avg_vruntime 正确保留睡眠/唤醒间的滞后 (EEVDF) |
| `PLACE_DEADLINE_INITIAL` | on | 新任务获得半个时间片以平稳进入竞争 |
| `PLACE_REL_DEADLINE` | on | 迁移后保留相对虚拟截止时间 |
| `RUN_TO_PARITY` | on | 阻止唤醒抢占，直到当前任务达到 0 滞后点或耗尽时间片 |
| `PREEMPT_SHORT` | on | 允许更短时间片的任务取消 RUN_TO_PARITY |
| `NEXT_BUDDY` | off | 优先调度上次唤醒的任务 |
| `PICK_BUDDY` | on | 允许考虑 cfs_rq->next 候选 |
| `CACHE_HOT_BUDDY` | on | 将 buddy 视为缓存热，减少迁移 |
| `DELAY_DEQUEUE` | on | 延迟出队非 eligible 任务 |
| `DELAY_ZERO` | on | 出队时将滞后裁剪为 0 |
| `WAKEUP_PREEMPTION` | on | 允许唤醒时抢占 |
| `HRTICK` | off | 高精度定时器调度 |
| `HRTICK_DL` | off | 截止时间调度的高精度定时器 |
| `NONTASK_CAPACITY` | on | 基于非任务运行时间降低 CPU 容量 |
| `TTWU_QUEUE` | on* | 在目标 CPU 排队唤醒任务 (*PREEMPT_RT 下关闭) |

### 14.2 关键可调参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `sysctl_sched_base_slice` | 700000ns | CFS 基础时间片 |
| `sysctl_sched_migration_cost` | 500000ns | 任务迁移成本估计 |
| `sysctl_sched_rt_period` | 1000000us | RT 周期 |
| `sysctl_sched_rt_runtime` | 950000us | RT 每周期最大运行时间 |
| `sched_rr_timeslice` | 100ms | SCHED_RR 时间片 |
| `sysctl_sched_tunable_scaling` | LOG | 可调参数缩放策略 |
| `sysctl_numa_balancing` | 动态 | NUMA 平衡开关 |

---

## 15. 调度相关系统调用

### 15.1 sched_setattr / sched_getattr

`kernel/sched/syscalls.c` 中的调度策略系统调用：

- `sys_sched_setattr()`：设置任务的调度策略和参数
- `sys_sched_setscheduler()`：传统设置接口
- `sys_sched_setparam()`：设置调度参数
- `__sched_setscheduler()`：内部实现，处理调度类切换

调度类切换流程：
```
__sched_setscheduler()
  ├── 获取任务锁 (task_rq_lock)
  ├── 检查权限和参数合法性
  ├── 调用 prev_class->switching_from()
  ├── 更新 task->sched_class
  ├── 调用 next_class->switching_to()
  ├── 重新入队任务
  ├── 调用 switched_from / switched_to
  └── 释放锁
```

### 15.2 sched_yield

`sys_sched_yield()` 让当前任务主动让出 CPU：

```c
do_sched_yield():
    rq = this_rq_lock_irq(&rf);
    rq->donor->sched_class->yield_task(rq);
    rq_unlock_irq(rq, &rf);
    schedule();
```

### 15.3 set_cpus_allowed / sched_setaffinity

CPU 亲和性设置：

- `set_cpus_allowed_ptr()`：设置任务可运行的 CPU 掩码
- `set_cpus_allowed_common()`：调度类通用实现
- 迁移禁用（`migration_disabled`）时不能迁移
- 使用 `struct affinity_context` 传递上下文

---

## 16. 调度器初始化

### 16.1 sched_init()

`sched_init()` 在系统启动早期初始化调度器：

1. 初始化 `runqueues` 数组（每 CPU 一个）
2. 初始化每个运行队列的 `cfs_rq`、`rt_rq`、`dl_rq`
3. 创建空闲任务 (`idle_task`)
4. 初始化调度类的全局状态
5. 设置 `scheduler_running = 1`

### 16.2 sched_init_smp()

`sched_init_smp()` 在 SMP 初始化完成后调用：

1. 初始化调度域拓扑 (`sched_init_domains()`)
2. 构建调度域层次结构
3. 初始化 `root_domain`
4. 设置 CPU 容量信息
5. 初始化 NUMA 平衡相关数据结构

---

## 17. 附录

### 17.1 关键文件清单

| 文件 | 说明 |
|------|------|
| `kernel/sched/sched.h` | 调度器内部类型和方法定义 |
| `kernel/sched/core.c` | 核心调度函数（__schedule, pick_next_task, context_switch） |
| `kernel/sched/fair.c` | CFS/EEVDF 调度类实现 |
| `kernel/sched/rt.c` | 实时调度类实现 |
| `kernel/sched/deadline.c` | 截止时间调度类实现 |
| `kernel/sched/stop_task.c` | 停靠任务调度类 |
| `kernel/sched/idle.c` | 空闲任务调度类 |
| `kernel/sched/ext.c` | sched_ext BPF 调度框架 |
| `kernel/sched/ext.h` | sched_ext 头文件 |
| `kernel/sched/ext_idle.c` | sched_ext 空闲处理 |
| `kernel/sched/core_sched.c` | Core Scheduling 实现 |
| `kernel/sched/topology.c` | 调度域拓扑管理 |
| `kernel/sched/syscalls.c` | 调度系统调用 |
| `kernel/sched/features.h` | 调度特性开关 |
| `kernel/sched/pelt.c` | PELT 负载跟踪实现 |
| `kernel/sched/pelt.h` | PELT 头文件 |
| `kernel/sched/clock.c` | 调度时钟 |
| `kernel/sched/cputime.c` | CPU 时间统计 |
| `kernel/sched/loadavg.c` | 系统负载平均 |
| `kernel/sched/debug.c` | 调度器调试接口 |
| `kernel/sched/cpupri.c` | CPU 优先级管理 |
| `kernel/sched/cpudeadline.c` | CPU 截止时间管理 |
| `kernel/sched/autogroup.c` | 自动分组 |
| `kernel/sched/completion.c` | 完成量机制 |
| `kernel/sched/cpufreq.c` | CPU 频率与调度交互 |
| `kernel/sched/cpufreq_schedutil.c` | schedutil 调频器 |
| `kernel/sched/isolation.c` | CPU 隔离 |
| `kernel/sched/membarrier.c` | 内存屏障跟踪 |
| `kernel/sched/psi.c` | PSI (Pressure Stall Information) |
| `kernel/sched/stats.c` | 调度统计 |
| `kernel/sched/swait.c` | 简单等待队列 |
| `kernel/sched/wait.c` | 等待队列 |
| `kernel/sched/wait_bit.c` | 位等待队列 |
| `kernel/sched/build_policy.c` | 策略类构建文件 |
| `kernel/sched/build_utility.c` | 工具类构建文件 |

### 17.2 数据结构关系图

```
task_struct
  ├── sched_class (指针) ──────────→ struct sched_class (接口表)
  ├── se (sched_entity) ───────────→ cfs_rq (红黑树管理)
  ├── rt (sched_rt_entity) ────────→ rt_rq (优先级数组)
  ├── dl (sched_dl_entity) ────────→ dl_rq (红黑树管理)
  └── policy ──────────────────────→ 调度策略

struct rq (per-CPU)
  ├── cfs (struct cfs_rq) ─────────→ tasks_timeline (红黑树)
  ├── rt (struct rt_rq) ───────────→ active (优先级位图+队列)
  ├── dl (struct dl_rq) ───────────→ root (红黑树)
  ├── curr ────────────────────────→ task_struct (当前运行)
  ├── idle ────────────────────────→ task_struct (空闲任务)
  ├── stop ────────────────────────→ task_struct (停靠任务)
  ├── sd ──────────────────────────→ sched_domain (调度域)
  └── rd ──────────────────────────→ root_domain (根域)

sched_domain (层级)
  ├── parent → 上层域
  ├── child  → 下层域
  └── groups → sched_group (调度组)

调度类遍历顺序:
  stop → dl → rt → fair → ext → idle
  (优先级从高到低)
```