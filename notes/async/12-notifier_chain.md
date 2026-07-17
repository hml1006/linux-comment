# notifier chain — 内核事件通知链

## 1 概述

notifier chain 是 Linux 内核中一种通用的发布-订阅通知机制，允许内核子系统在特定事件发生时通知已注册的模块。它通过链表维护多个回调函数，事件发生时依次调用。

- **文件**: `kernel/notifier.c`, `include/linux/notifier.h`
- **执行上下文**: 取决于链类型（原子/阻塞/SRCU/raw）

## 2 实现原理

### 2.1 核心数据结构

```c
// include/linux/notifier.h
struct notifier_block {
    notifier_fn_t notifier_call;              // 回调函数
    struct notifier_block __rcu *next;         // 链表下一个节点
    int priority;                              // 优先级（数值越大越先执行）
};

typedef int (*notifier_fn_t)(struct notifier_block *nb,
                              unsigned long action, void *data);
```

### 2.2 四种通知链类型

| 类型 | 结构体 | 保护机制 | 执行上下文 | 说明 |
|--|--|--|--|--|
| 原子通知链 | `atomic_notifier_head` | spinlock + RCU | 原子上下文（不可睡眠） | 最常用，如 die_chain、panic 通知 |
| 阻塞通知链 | `blocking_notifier_head` | rw_semaphore | 进程上下文（可睡眠） | 如 reboot 通知、热插拔通知 |
| Raw 通知链 | `raw_notifier_head` | 无（调用者提供） | 未定义 | 最低开销，适合特殊场合 |
| SRCU 通知链 | `srcu_notifier_head` | mutex + SRCU | 进程上下文（可睡眠） | 使用 SRCU 保护，睡眠安全 |

```c
struct atomic_notifier_head {
    spinlock_t lock;
    struct notifier_block __rcu *head;
};

struct blocking_notifier_head {
    struct rw_semaphore rwsem;
    struct notifier_block __rcu *head;
};

struct raw_notifier_head {
    struct notifier_block __rcu *head;
};

struct srcu_notifier_head {
    struct mutex mutex;
    struct srcu_usage srcuu;
    struct srcu_struct srcu;
    struct notifier_block __rcu *head;
};
```

### 2.3 注册与注销

```
notifier_chain_register(&head, nb, unique_priority)
  │
  └─ 按 priority 降序插入链表
       │
       ├─ 如果 nb 已存在 → WARN 并返回 -EEXIST
       ├─ 如果 priority 相同且 unique_priority → 返回 -EBUSY
       └─ 否则按 priority 排序插入
```

```
notifier_chain_unregister(&head, nb)
  │
  └─ 从链表中移除 nb，使用 RCU 保证同步
```

### 2.4 事件通知执行流程

```
notifier_call_chain(&head, val, v, nr_to_call, nr_calls)
  │
  ├─ 1. rcu_dereference 获取链表头
  │
  ├─ 2. 循环遍历链表：
  │    ├─ 调用 nb->notifier_call(nb, val, v)
  │    ├─ 记录返回值 ret
  │    └─ 如果 ret & NOTIFY_STOP_MASK → 停止遍历
  │
  └─ 3. 返回最后一个回调的返回值
```

### 2.5 返回值语义

| 返回值 | 含义 |
|--|--|
| `NOTIFY_DONE` | 回调未处理该事件 |
| `NOTIFY_OK` | 回调已处理 |
| `NOTIFY_BAD` | 错误，应停止通知 |
| `NOTIFY_STOP` | 停止后续通知 |
| `NOTIFY_STOP_MASK` | 与上述值 OR 后表示停止 |

## 3 使用场景

| 场景 | 链类型 | 说明 |
|--|--|--|
| 重启通知 | `blocking_notifier_chain` | 系统重启时通知各模块 |
| 关机通知 | `blocking_notifier_chain` | 系统关机时通知各模块 |
| CPU 热插拔 | `atomic_notifier_chain` | CPU 上线/下线时通知 |
| 内存热插拔 | `blocking_notifier_chain` | 内存热插拔事件 |
| 网络设备事件 | `blocking_notifier_chain` | 网络设备注册/注销 |
| 内核死机 (die) | `atomic_notifier_chain` | 内核异常时通知调试模块 |
| OOM 通知 | `atomic_notifier_chain` | 内存不足时通知 |
| 模块加载/卸载 | `atomic_notifier_chain` | 模块事件通知 |

## 4 关键 API

| API | 说明 |
|--|--|
| `atomic_notifier_chain_register()` | 注册原子通知链 |
| `atomic_notifier_call_chain()` | 触发原子通知链 |
| `blocking_notifier_chain_register()` | 注册阻塞通知链 |
| `blocking_notifier_call_chain()` | 触发阻塞通知链 |
| `raw_notifier_chain_register()` | 注册 raw 通知链 |
| `raw_notifier_call_chain()` | 触发 raw 通知链 |
| `srcu_notifier_chain_register()` | 注册 SRCU 通知链 |
| `srcu_notifier_call_chain()` | 触发 SRCU 通知链 |
| `notifier_call_chain_robust()` | 带回滚的通知（val_up 失败时调用 val_down） |

## 5 代码示例

### 5.1 注册重启通知

```c
static int my_reboot_notifier(struct notifier_block *nb,
                               unsigned long action, void *data)
{
    switch (action) {
    case SYS_RESTART:
        pr_info("System is restarting\n");
        break;
    case SYS_HALT:
        pr_info("System is halting\n");
        break;
    case SYS_POWER_OFF:
        pr_info("System is powering off\n");
        break;
    }
    return NOTIFY_OK;
}

static struct notifier_block my_reboot_nb = {
    .notifier_call = my_reboot_notifier,
    .priority = 0,
};

register_reboot_notifier(&my_reboot_nb);
```

### 5.2 注册 CPU 热插拔通知

```c
static int my_cpu_notifier(struct notifier_block *nb,
                            unsigned long action, void *data)
{
    switch (action) {
    case CPU_ONLINE:
    case CPU_ONLINE_FROZEN:
        pr_info("CPU online\n");
        break;
    case CPU_DEAD:
    case CPU_DEAD_FROZEN:
        pr_info("CPU dead\n");
        break;
    }
    return NOTIFY_OK;
}

static struct notifier_block my_cpu_nb = {
    .notifier_call = my_cpu_notifier,
    .priority = 0,
};

register_cpu_notifier(&my_cpu_nb);
```

## 6 内部实现 — 通知链遍历流程

```
notifier_call_chain(&head, val, v, -1, NULL)
  │
  ├─ nb = rcu_dereference_raw(*nl)
  │
  └─ while (nb && nr_to_call) {
       │
       ├─ next_nb = rcu_dereference_raw(nb->next)
       ├─ ret = nb->notifier_call(nb, val, v)
       │
       ├─ if (ret & NOTIFY_STOP_MASK) → break
       │
       └─ nb = next_nb
           nr_to_call--
     }

  └─ return ret  (最后一个回调的返回值)
```

## 7 与事件通知相关的其他机制对比

| 特性 | notifier chain | RCU callback | signal |
|--|--|--|--|
| 模型 | 发布-订阅 | 单次回调 | 异步信号 |
| 支持多个回调 | 是（链表） | 否（单次） | 是（信号处理函数） |
| 执行上下文 | 取决于链类型 | softirq/进程 | 信号处理上下文 |
| 过滤机制 | 通过 action 参数 | 无 | 通过信号编号 |
| 典型用途 | 子系统的全局事件通知 | 资源释放 | 进程间通信 |