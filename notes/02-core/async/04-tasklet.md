# Tasklet — 小任务（已弃用）

## 1 实现原理

Tasklet 是一种基于 Softirq（`TASKLET_SOFTIRQ` 和 `HI_SOFTIRQ`）的下半部机制，已被标记为 deprecated，建议迁移到 BH Workqueue。核心设计如下：

- **每 CPU 链表**：每个 CPU 维护 `tasklet_vec`（普通）和 `tasklet_hi_vec`（高优先级）两个链表头。
- **串行保证**：同一类型的 tasklet 在同一 CPU 上串行执行，不同 tasklet 之间可并行。
- **disable 计数**：`tasklet_struct.count` 原子计数，非零时禁止执行，可用于临时禁用。
- **状态标志**：`state` 字段包含 `TASKLET_STATE_SCHED`（已调度）和 `TASKLET_STATE_RUN`（正在执行）。

## 2 使用场景

- **旧式驱动下半部**：网卡、USB 等传统驱动。
- **简单串行化任务**：不需要 sleep 的简短处理。
- **状态**：已被标记为 deprecated，新代码应使用 BH Workqueue。

## 3 代码调用栈

```
调度 tasklet:
tasklet_schedule(t)
  └→ __tasklet_schedule(t)
      └→ t->next = NULL
      └→ 将 t 加入 per-CPU tasklet_vec 链表尾部
      └→ raise_softirq_irqoff(TASKLET_SOFTIRQ)

执行:
softirq 处理 → TASKLET_SOFTIRQ
  └→ tasklet_action()
      └→ tasklet_action_common(&tasklet_vec, TASKLET_SOFTIRQ)
          └→ 取链表头，清空链表
          └→ 遍历每个 tasklet:
              ├→ tasklet_trylock(t)  (确保不并发执行)
              ├→ atomic_read(&t->count) == 0? (未被禁用)
              ├→ tasklet_clear_sched(t)
              ├→ t->callback(t) 或 t->func(t->data)
              └→ tasklet_unlock(t)
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    Tasklet 调度与执行流程                          │
│                                                                   │
│  tasklet_schedule(t)                                              │
│    └→ 加锁 (local_irq_save)                                      │
│    └→ 将 t 加入 per-CPU tasklet_vec 链表尾部                     │
│    └→ raise_softirq_irqoff(TASKLET_SOFTIRQ)                     │
│    └→ 解锁 (local_irq_restore)                                   │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    tasklet_action_common()                        │
│                                                                   │
│  1. 关中断，取链表头，清空链表                                   │
│  2. 开中断                                                       │
│  3. 遍历链表:                                                    │
│     ┌─────────────────────────────────────────────────────┐       │
│     │ for each tasklet in list:                           │       │
│     │   ├─ tasklet_trylock() 成功?                        │       │
│     │   │   ├─ count == 0? (未被禁用)                     │       │
│     │   │   │   ├─ 清除 SCHED 标志                        │       │
│     │   │   │   ├─ 执行 callback 或 func                  │       │
│     │   │   │   └─ tasklet_unlock()                       │       │
│     │   │   └─ count != 0 → 放回链表尾部                  │       │
│     │   └─ tasklet_trylock() 失败 → 放回链表尾部          │       │
│     └─────────────────────────────────────────────────────┘       │
│                                                                   │
│  tasklet_struct 状态机:                                           │
│  ┌─────────┐  tasklet_schedule()  ┌───────────┐                  │
│  │  idle   │ ───────────────────→ │  SCHED    │                  │
│  └─────────┘                      └─────┬─────┘                  │
│       ▲                                │                         │
│       │                             执行开始                      │
│       │                                │                         │
│       │                          ┌─────▼─────┐                  │
│       │                          │ RUN│SCHED │                  │
│       │                          └─────┬─────┘                  │
│       │                             执行结束                      │
│       └──────────────────────────────────┘                       │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct tasklet_struct` — Tasklet 描述符

```c
// include/linux/interrupt.h (或相关头文件)
struct tasklet_struct {
    struct tasklet_struct *next;        // 链表下一个节点
    unsigned int state;                 // 状态: TASKLET_STATE_SCHED, TASKLET_STATE_RUN
    atomic_t count;                     // 禁用计数: 0=可用, >0=禁用
    bool use_callback;                  // true: 使用 callback, false: 使用 func/data
    union {
        void (*func)(unsigned long data);  // 旧式回调 (use_callback=false)
        void (*callback)(struct tasklet_struct *t); // 新式回调 (use_callback=true)
    };
    unsigned long data;                 // 传递给 func 的参数
};
```

### 状态标志位

```c
// include/linux/interrupt.h
enum {
    TASKLET_STATE_SCHED,    // 0: 已调度，等待执行
    TASKLET_STATE_RUN       // 1: 正在执行中
};
```

### 每 CPU 链表

```c
// kernel/softirq.c
struct tasklet_head {
    struct tasklet_struct *head;    // 链表头
    struct tasklet_struct **tail;   // 链表尾指针
};

static DEFINE_PER_CPU(struct tasklet_head, tasklet_vec);     // 普通 tasklet
static DEFINE_PER_CPU(struct tasklet_head, tasklet_hi_vec);  // 高优先级 tasklet
```

### 初始化 API

```c
// 新式 (推荐)
void tasklet_setup(struct tasklet_struct *t,
                   void (*callback)(struct tasklet_struct *));

// 旧式
void tasklet_init(struct tasklet_struct *t,
                  void (*func)(unsigned long), unsigned long data);

// 调度
void tasklet_schedule(struct tasklet_struct *t);
void tasklet_hi_schedule(struct tasklet_struct *t);

// 禁用/启用
void tasklet_disable(struct tasklet_struct *t);
void tasklet_enable(struct tasklet_struct *t);
```