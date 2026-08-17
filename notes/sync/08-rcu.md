# 8. RCU 机制

## 8.1 概述

RCU (Read-Copy-Update) 是一种无锁同步机制，允许多个读者并发访问共享数据，而写者通过"复制-修改-提交"的方式更新数据，并延迟回收旧版本。

**核心思想：**

- 读者无锁：读操作没有任何锁开销 (不需要原子操作、内存屏障)
- 写者复制：修改数据时先复制一份，在副本上修改
- 延迟回收：等待所有现有读者完成后再释放旧数据

## 8.2 核心原理

### 8.2.1 宽限期 (Grace Period)

```
                                        ┌─ 宽限期 ─┐
读者 1:  ──R───────R──────────────────────R───────────→
读者 2:  ──────R──────R─────────────────────────────────→
写者:     ──────W(移除指针)────────────────────────────W(回收旧数据)──→
                                                   ↑
                                                宽限期结束
                                                所有之前的读者已完成
```

**宽限期条件：** 所有在写者移除指针之前就已经开始的 RCU 读端都必须已经结束。

### 8.2.2 三个关键阶段

```
RCU 更新流程:

阶段 1: 移除引用
  ┌─────────────────────────────────────────────┐
  │  rcu_assign_pointer(ptr, new_version)       │
  │  所有新读者看到新版本                        │
  └─────────────────────────────────────────────┘
                      │
                      ▼
阶段 2: 等待宽限期
  ┌─────────────────────────────────────────────┐
  │  synchronize_rcu() 或 call_rcu()            │
  │  等待所有旧读者完成                          │
  └─────────────────────────────────────────────┘
                      │
                      ▼
阶段 3: 回收旧数据
  ┌─────────────────────────────────────────────┐
  │  kfree(old_ptr) 或 callback 释放            │
  │  旧版本安全回收                              │
  └─────────────────────────────────────────────┘
```

## 8.3 核心 API

### 8.3.1 读者端 API

定义在 [include/linux/rcupdate.h](file:///home/louis/code/linux/include/linux/rcupdate.h)：

```c
// 读者端 — 声明 RCU 读端临界区
rcu_read_lock();
// ... 通过 rcu_dereference() 读取共享指针 ...
rcu_read_unlock();

// 可抢占 RCU (PREEMPT_RT)
rcu_read_lock_sched();
// ...
rcu_read_unlock_sched();

// RCU 读端休眠 (bh)
rcu_read_lock_bh();
// ...
rcu_read_unlock_bh();

// 指针读取 — 将 RCU 保护的指针安全地读到本地
#define rcu_dereference(p) \
    ({ \
        typeof(*p) *_________p1 = READ_ONCE(p); \
        __rcu_dereference_check(p); \
        (_________p1); \
    })

// 示例: 在 RCU 读端临界区中读取
struct foo *p;

rcu_read_lock();
p = rcu_dereference(g_foo);    // 读取全局指针
if (p) {
    // 安全使用 p->field
    do_something(p);
}
rcu_read_unlock();
```

### 8.3.2 写者端 API

```c
// 指针发布 — 将新值安全地发布到 RCU 保护的指针
#define rcu_assign_pointer(p, v) \
    ({ \
        __rcu_assign_pointer((p), (v)); \
    })

// 示例: 更新 RCU 保护的指针
struct foo *new_foo = kmalloc(sizeof(*new_foo), GFP_KERNEL);
new_foo->field = 42;
rcu_assign_pointer(g_foo, new_foo);  // 原子切换指针

// 同步等待宽限期
void synchronize_rcu(void);
// 阻塞直到所有现存的 RCU 读端临界区完成

// 异步回调注册
void call_rcu(struct rcu_head *head, rcu_callback_t func);
// 注册回调, 宽限期结束后调用 func(head)

// 结构体中的回调节点
struct rcu_head {
    struct rcu_head *next;
    void (*func)(struct rcu_head *head);
};
```

### 8.3.3 完整更新示例

```c
// 受 RCU 保护的全局链表头
struct foo {
    struct list_head list;
    int data;
    struct rcu_head rcu;        // 用于回调的 RCU 节点
};

static LIST_HEAD(g_foo_list);

// 添加节点
void foo_add(int data)
{
    struct foo *f = kmalloc(sizeof(*f), GFP_KERNEL);
    f->data = data;

    spin_lock(&foo_lock);       // 保护链表修改
    list_add_rcu(&f->list, &g_foo_list);
    spin_unlock(&foo_lock);
}

// 遍历读取 (RCU 保护)
void foo_read(void)
{
    struct foo *f;

    rcu_read_lock();
    list_for_each_entry_rcu(f, &g_foo_list, list) {
        printk("data = %d\n", f->data);
    }
    rcu_read_unlock();
}

// 删除节点 (含延迟回收)
void foo_del(struct foo *old)
{
    spin_lock(&foo_lock);
    list_del_rcu(&old->list);   // 从链表中移除
    spin_unlock(&foo_lock);

    synchronize_rcu();          // 等待宽限期
    kfree(old);                 // 安全回收
}

// 或使用异步回调
void foo_del_async(struct foo *old)
{
    spin_lock(&foo_lock);
    list_del_rcu(&old->list);
    spin_unlock(&foo_lock);

    call_rcu(&old->rcu, foo_free_cb);  // 异步回收
}

static void foo_free_cb(struct rcu_head *rh)
{
    struct foo *f = container_of(rh, struct foo, rcu);
    kfree(f);
}
```

## 8.4 Tree RCU 实现

### 8.4.1 数据结构

定义在 [kernel/rcu/tree.h](file:///home/louis/code/linux/kernel/rcu/tree.h)：

```c
// 每 CPU 的 RCU 数据
struct rcu_data {
    unsigned long           gp_seq;         // 当前宽限期序列号
    unsigned long           completed;      // 已完成的宽限期
    unsigned long           gpwrap;         // 序列号回绕记录
    int                     cpu;            // 所属 CPU

    // 回调队列
    struct rcu_head         *nocb_head;     // 回调链表头
    struct rcu_head         **nocb_tail;    // 回调链表尾
    int                     nocb_defer_wakeup;  // 延迟唤醒标志

    // 宽限期跟踪
    bool                    gp_start;       // 宽限期是否开始
    bool                    gp_rcu;         // 宽限期类型 (RCU)
    bool                    gp_tasks;       // 宽限期类型 (Tasks RCU)
    unsigned long           rdp_gp_seq;     // 本地宽限期序列号
    struct rcu_node         *mynode;        // 所属的 rcu_node

    // CPU 状态
    unsigned long           ticks_this_gp;  // 宽限期内的 tick 数
    bool                    cpu_no_qs;      // CPU 无 quiescent state
    bool                    core_needs_qs;  // 需要报告 QS
    bool                    beenonline;     // 是否曾在线
};

// 树节点
struct rcu_node {
    raw_spinlock_t          __private lock; // 保护本节点
    unsigned long           gp_seq;         // 宽限期序列号
    unsigned long           gp_seq_needed;  // 需要的宽限期序列号

    // 位图
    unsigned long           qsmask;         // 需要 QS 的子节点位图
    unsigned long           qsmaskinit;     // 初始 QS 位图
    unsigned long           qsmaskinitnext; // 下次初始 QS 位图

    // 层级
    int                     grplo;          // 组内最小编号 CPU
    int                     grphi;          // 组内最大编号 CPU
    u8                      grpnum;         // 组编号
    u8                      level;          // 树层级 (0=叶子)

    // 父节点
    struct rcu_node         *parent;        // 父节点指针
};
```

### 8.4.2 Tree RCU 层次结构

```
Tree RCU 层次:

根节点 (level 2):
  ┌─────────────────────────────────────────────────────┐
  │  rcu_node (root)                                    │
  │  qsmask = 位图, 表示需要哪些子节点报告 QS            │
  └──────────────────────┬──────────────────────────────┘
                         │
           ┌─────────────┼─────────────┐
           ▼             ▼             ▼
     ┌──────────┐  ┌──────────┐  ┌──────────┐
     │rcu_node  │  │rcu_node  │  │rcu_node  │  ...  (level 1)
     │group=0   │  │group=1   │  │group=2   │
     └────┬─────┘  └────┬─────┘  └────┬─────┘
          │              │              │
     ┌────┴────┐    ┌────┴────┐    ┌────┴────┐
     ▼         ▼    ▼         ▼    ▼         ▼
   ┌────┐   ┌────┐ ┌────┐   ┌────┐ ┌────┐   ┌────┐
   │CPU0│   │CPU1│ │CPU2│   │CPU3│ │CPU4│   │CPU5│  ... (leaf level 0)
   │rcu │   │rcu │ │rcu │   │rcu │ │rcu │   │rcu │
   │data│   │data│ │data│   │data│ │data│   │data│
   └────┘   └────┘ └────┘   └────┘ └────┘   └────┘
```

### 8.4.3 宽限期传播流程

```
synchronize_rcu() 调用:

  1. 开始新宽限期:
     rcu_gp_init():
       └── 遍历所有 rcu_node
             ├── 初始化 qsmask = qsmaskinit
             └── 传播到根节点

  2. 等待 quiescent state:
     rcu_gp_fqs():
       └── 循环等待:
             ├── 检查每个 CPU 是否报告 QS
             │     (context switch, idle, user mode)
             │
             ├── 叶子节点: 所有 CPU 报告 QS → 通知父节点
             │
             ├── 中间节点: 所有子节点报告 QS → 通知父节点
             │
             └── 根节点: 所有子节点报告 QS → 宽限期结束

  3. 宽限期结束:
     rcu_gp_cleanup():
       └── 执行所有等待的回调函数
```

## 8.5 SRCU (可睡眠 RCU)

### 8.5.1 概述

SRCU (Sleepable RCU) 允许读者在 RCU 读端临界区内睡眠：

```c
// include/linux/srcu.h
struct srcu_struct {
    struct srcu_data __percpu *sda;     // per-CPU 数据
    struct srcu_node *node;              // 树节点
    struct srcu_node *level[SRCU_MAX_LEVEL];  // 层级数组
    struct mutex srcu_gp_mutex;          // 宽限期互斥锁
    atomic_t srcu_gp_in_progress;        // 宽限期进行中
    unsigned long srcu_gp_seq;           // 宽限期序列号
    unsigned long srcu_gp_seq_needed;    // 需要的宽限期
    unsigned long srcu_gp_seq_needed_exp; // 需要的快速宽限期
    spinlock_t srcu_gp_lock;             // 宽限期锁
    bool srcu_size_state;                // 大小状态
};

// API
int init_srcu_struct(struct srcu_struct *ssp);
void cleanup_srcu_struct(struct srcu_struct *ssp);

int srcu_read_lock(struct srcu_struct *ssp) __acquires(ssp);
void srcu_read_unlock(struct srcu_struct *ssp, int idx) __releases(ssp);

void synchronize_srcu(struct srcu_struct *ssp);
void call_srcu(struct srcu_struct *ssp, struct rcu_head *head,
               rcu_callback_t func);
```

**SRCU 使用场景：**

- 读者需要睡眠 (如等待 I/O)
- 读者需要获取 mutex
- 读者需要执行可能导致调度的操作

## 8.6 Tasks RCU

### 8.6.1 概述

Tasks RCU 专门用于等待内核线程退出 (如 trampoline 卸载)：

```c
// include/linux/rcupdate.h
// API
void synchronize_rcu_tasks(void);
void synchronize_rcu_tasks_rude(void);
void synchronize_rcu_tasks_trace(void);

// 使用场景: 等待所有任务至少经过一次 voluntary 调度
// 用于: 动态跟踪 (ftrace, BPF) 的 trampoline 安全卸载
```

## 8.7 RCU 与 PREEMPT_RT

### 8.7.1 修改

在 PREEMPT_RT 下，`rcu_read_lock()` 变为可抢占：

```c
// 非 RT: rcu_read_lock() 禁用抢占
// RT:  rcu_read_lock() 变为纯读端标记

// 非 RT 下的实现
static inline void __rcu_read_lock(void)
{
    preempt_disable();           // 禁止抢占 = 隐式 RCU 保护
}

// PREEMPT_RT 下的实现
static inline void __rcu_read_lock(void)
{
    // 不禁用抢占, 使用 RCU 锁保护
    current->rcu_read_lock_nesting++;
    barrier();
}
```

### 8.7.2 差异

| 特性     | 非 RT               | PREEMPT_RT      |
| -------- | ------------------- | --------------- |
| 读者抢占 | 读端禁用抢占        | 读端可抢占      |
| 宽限期   | 上下文切换 + 用户态 | 显式跟踪读者    |
| 读端延迟 | 低                  | 略高 (跟踪开销) |
| 兼容性   | 完全                | 需额外配置      |

## 8.8 使用场景

| 场景           | 使用变体  | 说明                      |
| -------------- | --------- | ------------------------- |
| 指针保护       | 标准 RCU  | 受保护指针的读-复制-更新  |
| 链表遍历       | 标准 RCU  | list_for_each_entry_rcu() |
| 文件系统路径   | SRCU      | 路径遍历中可睡眠          |
| BPF trampoline | Tasks RCU | 等待所有任务退出          |
| 网络路由表     | 标准 RCU  | 路由表读多写少            |

## 8.9 关键文件

| 文件                                                                              | 说明              |
| --------------------------------------------------------------------------------- | ----------------- |
| [include/linux/rcupdate.h](file:///home/louis/code/linux/include/linux/rcupdate.h) | RCU 核心 API      |
| [include/linux/rcu_sync.h](file:///home/louis/code/linux/include/linux/rcu_sync.h) | RCU 同步辅助      |
| [include/linux/srcu.h](file:///home/louis/code/linux/include/linux/srcu.h)         | SRCU API          |
| [kernel/rcu/tree.c](file:///home/louis/code/linux/kernel/rcu/tree.c)               | Tree RCU 实现     |
| [kernel/rcu/tree.h](file:///home/louis/code/linux/kernel/rcu/tree.h)               | Tree RCU 数据结构 |
| [kernel/rcu/srcu.c](file:///home/louis/code/linux/kernel/rcu/srcu.c)               | SRCU 实现         |
| [kernel/rcu/tasks.h](file:///home/louis/code/linux/kernel/rcu/tasks.h)             | Tasks RCU 实现    |
