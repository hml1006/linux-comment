# Runtime Verification — 运行时验证框架

## 概述

Runtime Verification (RV) 是 Linux 内核提供的运行时验证框架，通过分析系统实际执行的轨迹，与形式化规范进行比较，检测系统行为是否符合预期。RV 是一种轻量级但严格的方法，用于补充传统的穷尽验证技术（如模型检查和定理证明）。

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Runtime Verification Architecture             │
│                                                                     │
│  Linux 领域 +----------------- RV Monitor -------------------+ 形式化领域│
│            |                                                 |        │
│  ┌──────────────────┐              ┌──────────────┐  ┌──────────────┐ │
│  │   Linux Kernel   │              │  Monitor     │  │  Reference   │ │
│  │    Tracing       │  ──────────→ │  Instance(s) │ ← │    Model     │ │
│  │ (instrumentation)│              │(verification)│   │(specification)││
│  └──────────────────┘              └──────┬───────┘  └──────────────┘ │
│                                           │                          │
│                                           ▼                          │
│                                  ┌──────────────┐                    │
│                                  │   Reaction   │                    │
│                                  └──────┬───────┘                    │
│                                         │                            │
│                    ┌────────────────────┼────────────────────┐       │
│                    │                    │                    │       │
│                    ▼                    ▼                    ▼       │
│            ┌──────────┐      ┌──────────────┐      ┌──────────────┐  │
│            │ trace    │      │    panic     │      │ user-specified│  │
│            │  output  │      │              │      │    reaction   │  │
│            └──────────┘      └──────────────┘      └──────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## 核心概念

### Monitor（监视器）

Monitor 是运行时验证的核心组件，包含：

- **Reference Model（参考模型）**: 系统的形式化规范
- **Monitor Instance（监视器实例）**: 验证执行的实例（全局、每 CPU、每任务）
- **Instrumentation（插桩）**: 通过 trace 事件与内核连接
- **Reaction（反应）**: 检测到异常时的响应

### Reactor（反应器）

Reactor 定义了检测到异常时的反应行为：

- **nop**: 无操作（默认）
- **printk**: 输出日志
- **panic**: 触发系统 panic
- **user-specified**: 用户自定义反应

### Monitor 类型

| 类型 | 宏定义 | 说明 |
|------|--------|------|
| 全局 | `RV_MON_GLOBAL` | 单个全局实例 |
| 每 CPU | `RV_MON_PER_CPU` | 每个 CPU 一个实例 |
| 每任务 | `RV_MON_PER_TASK` | 每个任务一个实例 |

## 核心数据结构

### rv_monitor

```
struct rv_monitor {
    const char               *name;           /* 监视器名称 */
    const char               *description;    /* 描述 */
    int                      (*enable)(void); /* 启用回调 */
    void                     (*disable)(void);/* 禁用回调 */
    struct list_head         list;           /* 监视器链表 */
    struct dentry            *dir;           /* tracefs 目录 */
    struct rv_reactor        *reactor;       /* 当前反应器 */
    bool                     enabled;        /* 是否启用 */
    u32                      monitor_type;   /* 监视器类型 */
};
```

### da_monitor（确定性自动机监视器）

```
struct da_monitor {
    bool                     monitoring;     /* 是否正在监控 */
    unsigned int             curr_state;     /* 当前状态 */
};
```

### ltl_monitor（线性时态逻辑监视器）

```
struct ltl_monitor {
    DECLARE_BITMAP(states, RV_MAX_BA_STATES);      /* Buchi 自动机状态 */
    DECLARE_BITMAP(atoms, RV_MAX_LTL_ATOM);        /* 原子命题值 */
    DECLARE_BITMAP(unknown_atoms, RV_MAX_LTL_ATOM);/* 未知原子命题 */
};
```

### rv_reactor（反应器）

```
struct rv_reactor {
    const char               *name;           /* 反应器名称 */
    const char               *description;    /* 描述 */
    void                     (*react)(const char *msg, va_list args); /* 反应函数 */
    struct list_head         list;           /* 反应器链表 */
};
```

## 内置监视器

### 监控器列表

| 名称 | 说明 | 类型 |
|------|------|------|
| wip | Waiting In Preemption | Per-task |
| wwnr | Waiting With No Reschedule | Per-task |
| nrp | No Run Preemption | Per-cpu |
| opid | Ordered Preemption IDs | Per-cpu |
| pagefault | Page Fault Monitor | Per-task |
| rtapp | Real-Time Application Monitor | Global |
| sched | Scheduler Monitor | Per-cpu |
| sco | Scheduling Class Order | Per-cpu |
| scpd | Scheduling Class Priority Domain | Per-cpu |
| sleep | Sleep Monitor | Per-task |
| snep | Simple Nesting Preemption | Per-task |
| snroc | Simple Nesting RunOn CPU | Per-task |
| sssw | Single-Step Scheduler Watcher | Per-cpu |
| sts | State Transition System | Per-task |

### WIP Monitor（Waiting In Preemption）

检测任务在抢占上下文中等待的情况：

```
状态机:
  IDLE → PREEMPTED → WAITING → VIOLATION
       ↓ (preempt) ↓ (wait)    ↓ (detect)
       ↓            ↓            ↓
       PREEMPTED   WAITING     VIOLATION (报告)
```

### WWNR Monitor（Waiting With No Reschedule）

检测任务等待但未触发重调度的情况：

```
状态机:
  RUNNING → WAITING → NO_RESCHEDULE → VIOLATION
          ↓ (wait)  ↓ (no resched)   ↓ (detect)
```

### NRP Monitor（No Run Preemption）

检测 CPU 上没有运行时抢占的情况：

```
状态机:
  IDLE → RUNNING → NO_PREEMPT → VIOLATION
       ↓ (start) ↓ (no preempt) ↓ (timeout)
```

## 用户接口

### tracefs 目录结构

```
/sys/kernel/tracing/rv/
├── available_monitors          # 可用监视器列表
├── enabled_monitors            # 启用的监视器列表
├── reacting_on                 # 反应器总开关
├── available_reactors          # 可用反应器列表
└── monitors/                   # 监视器目录
    ├── wip/
    │   └── reactors            # wip 的反应器选择
    ├── wwnr/
    │   └── reactors
    ├── nrp/
    │   └── reactors
    └── ...
```

### 使用示例

```bash
# 查看可用监视器
cat /sys/kernel/tracing/rv/available_monitors

# 启用 WIP 监视器
echo wip > /sys/kernel/tracing/rv/enabled_monitors

# 查看已启用监视器
cat /sys/kernel/tracing/rv/enabled_monitors

# 禁用 WIP 监视器
echo "!wip" > /sys/kernel/tracing/rv/enabled_monitors

# 查看可用反应器
cat /sys/kernel/tracing/rv/available_reactors

# 为 WIP 监视器设置 panic 反应器
echo panic > /sys/kernel/tracing/rv/monitors/wip/reactors

# 查看 WIP 当前反应器
cat /sys/kernel/tracing/rv/monitors/wip/reactors

# 关闭所有反应
echo 0 > /sys/kernel/tracing/rv/reacting_on
```

## 监视器注册

### 注册流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Monitor Registration Flow                   │
│                                                                     │
│  rv_register_monitor():                                              │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  1. 检查监视器名称是否已存在                                │   │
│  │  2. 创建 tracefs 目录 (monitors/NAME/)                     │   │
│  │  3. 初始化反应器接口                                        │   │
│  │  4. 添加到 rv_monitors_list 链表                           │   │
│  │  5. 更新 available_monitors 文件                          │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  rv_unregister_monitor():                                            │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  1. 如果监视器已启用，先禁用                                │   │
│  │  2. 从 rv_monitors_list 链表移除                           │   │
│  │  3. 删除 tracefs 目录                                       │   │
│  │  4. 更新 available_monitors 文件                          │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### 注册示例

```c
static struct rv_monitor my_monitor = {
    .name           = "my_monitor",
    .description    = "My custom monitor",
    .enable         = my_monitor_enable,
    .disable        = my_monitor_disable,
    .monitor_type   = RV_MON_PER_CPU,
};

static int __init my_monitor_init(void)
{
    return rv_register_monitor(&my_monitor);
}

static void __exit my_monitor_exit(void)
{
    rv_unregister_monitor(&my_monitor);
}

module_init(my_monitor_init);
module_exit(my_monitor_exit);
```

## 反应器注册

### 注册流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Reactor Registration Flow                    │
│                                                                     │
│  rv_register_reactor():                                              │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  1. 检查反应器名称是否已存在                                │   │
│  │  2. 添加到 rv_reactors_list 链表                           │   │
│  │  3. 更新 available_reactors 文件                          │   │
│  │  4. 为每个已启用监视器更新 reactors 文件                    │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  rv_unregister_reactor():                                            │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  1. 如果某个监视器使用该反应器，切换到 nop                   │   │
│  │  2. 从 rv_reactors_list 链表移除                           │   │
│  │  3. 更新 available_reactors 文件                          │   │
│  │  4. 为每个监视器更新 reactors 文件                          │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### 内置反应器

```
1. nop 反应器:
   ┌─────────────────────────────────────────────────────────────┐
   │  static void reactor_nop_react(const char *msg, va_list args)│
   │  {                                                          │
   │      /* 无操作 */                                            │
   │  }                                                          │
   └─────────────────────────────────────────────────────────────┘

2. printk 反应器:
   ┌─────────────────────────────────────────────────────────────┐
   │  static void reactor_printk_react(const char *msg, va_list args)│
   │  {                                                          │
   │      vprintk(msg, args);                                    │
   │  }                                                          │
   └─────────────────────────────────────────────────────────────┘

3. panic 反应器:
   ┌─────────────────────────────────────────────────────────────┐
   │  static void reactor_panic_react(const char *msg, va_list args)│
   │  {                                                          │
   │      char buf[512];                                         │
   │      vsnprintf(buf, sizeof(buf), msg, args);                │
   │      panic("RV: %s", buf);                                  │
   │  }                                                          │
   └─────────────────────────────────────────────────────────────┘
```

## 编译配置

| 配置项 | 说明 |
|--------|------|
| CONFIG_RV | 启用运行时验证框架 |
| CONFIG_RV_LTL_MONITOR | 启用 LTL 监视器支持 |
| CONFIG_RV_REACTORS | 启用反应器支持 |
| CONFIG_RV_MON_WIP | 启用 WIP 监视器 |
| CONFIG_RV_MON_WWNR | 启用 WWNR 监视器 |
| CONFIG_RV_MON_NRP | 启用 NRP 监视器 |
| CONFIG_RV_MON_OPID | 启用 OPID 监视器 |
| CONFIG_RV_MON_PAGEFAULT | 启用 Page Fault 监视器 |
| CONFIG_RV_MON_RTAPP | 启用 RTAPP 监视器 |
| CONFIG_RV_MON_SCHED | 启用 Sched 监视器 |
| CONFIG_RV_MON_SCO | 启用 SCO 监视器 |
| CONFIG_RV_MON_SCPD | 启用 SCPD 监视器 |
| CONFIG_RV_MON_SLEEP | 启用 Sleep 监视器 |
| CONFIG_RV_MON_SNEP | 启用 SNEP 监视器 |
| CONFIG_RV_MON_SNROC | 启用 SNROC 监视器 |
| CONFIG_RV_MON_SSSW | 启用 SSSW 监视器 |
| CONFIG_RV_MON_STS | 启用 STS 监视器 |

## 性能影响

| 方面 | 影响 |
|------|------|
| 监视器启用 | 取决于具体监视器的复杂度 |
| 事件处理 | 每个 trace 事件增加少量开销 |
| 状态转换 | 取决于状态机复杂度 |
| 反应器 | nop 几乎无开销，panic 终止系统 |

## API 接口

### 监视器管理

```c
int rv_register_monitor(struct rv_monitor *monitor);
int rv_unregister_monitor(struct rv_monitor *monitor);
int rv_enable_monitor(struct rv_monitor *mon);
int rv_disable_monitor(struct rv_monitor *mon);
```

### 反应器管理

```c
int rv_register_reactor(struct rv_reactor *reactor);
int rv_unregister_reactor(struct rv_reactor *reactor);
```

### LTL 监视器辅助函数

```c
bool rv_ltl_valid_state(struct ltl_monitor *mon);
bool rv_ltl_all_atoms_known(struct ltl_monitor *mon);
```

## 使用场景

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Use Cases                                   │
│                                                                     │
│  1. 实时系统验证:                                                    │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • 检测调度器违反实时约束                                   │   │
│  │  • 检测抢占延迟超标                                         │   │
│  │  • 检测优先级反转                                           │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  2. 安全关键系统:                                                    │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • 检测异常状态转换                                         │   │
│  │  • 检测资源泄露                                             │   │
│  │  • 检测竞争条件                                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  3. 调试和测试:                                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • 检测内核 bug                                             │   │
│  │  • 验证修复效果                                             │   │
│  │  • 自动化测试断言                                           │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 总结

Runtime Verification 框架提供了一种形式化的运行时验证方法：

1. **轻量级验证**：基于 trace 事件的验证，性能开销可控
2. **形式化规范**：支持确定性自动机和 LTL 模型
3. **灵活反应**：支持 nop、printk、panic 和自定义反应
4. **多种监视器**：内置多个针对实时和调度的监视器
5. **用户友好**：通过 tracefs 提供直观的控制接口

RV 框架特别适用于实时系统和安全关键系统的运行时验证，能够在检测到违规时及时响应，避免故障传播。