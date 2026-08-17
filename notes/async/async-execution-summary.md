# Linux 内核异步执行机制概要

## 1 概述

Linux 内核提供多种异步执行机制，覆盖从硬中断上下文到进程上下文的全部优先级范围。本文档对这些机制进行系统性梳理和对比。

---

## 2 异步执行机制总览

```
优先级高 ────────────────────────────────────────────────────────── 优先级低
硬中断上下文 ────────────────→ 软中断上下文 ────→ 进程上下文

irq_work → Softirq → BH Workqueue/Tasklet → Timer → Workqueue → kthread
                    ↕
              RCU callback
```

---

## 3 各机制详解

### 3.1 irq_work — 中断上下文工作队列

| 项目 | 说明 |
|--|--|
| 执行上下文 | NMI / 硬中断 |
| 可睡眠 | 否 |
| 并行度 | 按 CPU 独立 |
| 文件 | `kernel/irq_work.c` |
| 关键 API | `irq_work_queue()`, `irq_work_queue_on()` |
| 典型用途 | 跨 CPU 中断通知、perf 事件、RCU 紧急回调 |
| 特点 | 在硬件中断返回路径中执行，比 softirq 优先 |

### 3.2 Softirq — 软中断

| 项目 | 说明 |
|--|--|
| 执行上下文 | 硬中断 |
| 可睡眠 | 否 |
| 并行度 | 高（多 CPU 并行执行同一 softirq） |
| 文件 | `kernel/softirq.c` |
| 关键 API | `raise_softirq()`, `open_softirq()` |
| 典型用途 | 网络 RX/TX、块设备层、RCU、定时器 |
| 特点 | 编译时静态分配，优先级最高下半部 |

### 3.3 BH Workqueue — 下半部工作队列（替代 Tasklet）

| 项目 | 说明 |
|--|--|
| 执行上下文 | softirq（硬中断） |
| 可睡眠 | 否 |
| 并行度 | 每 CPU 单执行上下文，串行 |
| 文件 | `kernel/workqueue.c` |
| 关键 API | `queue_work(system_bh_wq, ...)` |
| 典型用途 | 替代 tasklet 的新标准下半部机制 |
| 特点 | workqueue 接口封装 softirq，支持 lockdep |

### 3.4 Tasklet — 小任务（已弃用）

| 项目 | 说明 |
|--|--|
| 执行上下文 | softirq（硬中断） |
| 可睡眠 | 否 |
| 并行度 | 低（同一 tasklet 单 CPU 串行） |
| 文件 | `kernel/softirq.c`（`TASKLET_SOFTIRQ`） |
| 关键 API | `tasklet_schedule()`, `tasklet_init()` |
| 典型用途 | 旧式驱动下半部（网卡、USB） |
| 状态 | 被标记 deprecated，建议迁移到 BH Workqueue |

### 3.5 Timer / hrtimer — 定时器

| 项目 | 说明 |
|--|--|
| 执行上下文 | softirq |
| 可睡眠 | 否 |
| 并行度 | 按 CPU 分桶 |
| 文件 | `kernel/time/timer.c`, `kernel/time/hrtimer.c` |
| 关键 API | `timer_setup()`, `add_timer()`, `hrtimer_start()` |
| 典型用途 | 超时处理、周期性任务、高精度定时 |

### 3.6 RCU callback — RCU 回调

| 项目 | 说明 |
|--|--|
| 执行上下文 | softirq 或专用 kthread |
| 可睡眠 | 否 |
| 文件 | `kernel/rcu/` |
| 关键 API | `call_rcu()`, `call_rcu_hurry()`, `synchronize_rcu()` |
| 典型用途 | RCU 宽限期结束后释放资源 |
| 特点 | 读端无锁同步的基础 |

### 3.7 Workqueue — 工作队列

| 项目 | 说明 |
|--|--|
| 执行上下文 | 进程（kworker 线程） |
| 可睡眠 | 是 |
| 并行度 | 可配置（多线程并发） |
| 文件 | `kernel/workqueue.c` |
| 关键 API | `schedule_work()`, `queue_work()`, `alloc_workqueue()` |
| 典型用途 | 需要睡眠的延迟工作、设备 probe、文件系统回写 |
| 分类 | 系统 wq / per-CPU wq / unbound wq / ordered wq |

### 3.8 Threaded IRQ — 中断线程化

| 项目 | 说明 |
|--|--|
| 执行上下文 | 内核线程（进程） |
| 可睡眠 | 是 |
| 文件 | `kernel/irq/manage.c` |
| 关键 API | `request_threaded_irq()`, `devm_request_threaded_irq()` |
| 典型用途 | 减少关中断时间，RT 内核必须 |
| 特点 | 中断下半部搬到内核线程执行 |

### 3.9 kthread — 内核线程

| 项目 | 说明 |
|--|--|
| 执行上下文 | 独立内核线程 |
| 可睡眠 | 是 |
| 文件 | `kernel/kthread.c` |
| 关键 API | `kthread_create()`, `kthread_run()`, `kthread_stop()` |
| 典型用途 | watchdog、migration、内存回收、长时间后台任务 |

### 3.10 async_schedule — 异步函数调度

| 项目 | 说明 |
|--|--|
| 执行上下文 | kworker 线程（进程） |
| 可睡眠 | 是 |
| 文件 | `kernel/async.c`, `include/linux/async.h` |
| 关键 API | `async_schedule()`, `async_schedule_dev()`, `async_synchronize_full()` |
| 典型用途 | 启动阶段并行化设备初始化 |
| 特点 | 返回 cookie 可用于同步等待，超限时退化为同步执行 |

### 3.11 task_work — 返回用户态时执行

| 项目 | 说明 |
|--|--|
| 执行上下文 | 进程上下文（syscall 返回用户态路径） |
| 可睡眠 | 是 |
| 文件 | `kernel/task_work.c` |
| 关键 API | `task_work_add()`, `task_work_run()` |
| 典型用途 | 延迟 fput()、信号投递、SIGURG 通知 |
| 触发点 | `exit_to_user_mode()` → `task_work_run()` |

### 3.12 notifier chain — 内核事件通知链

| 项目 | 说明 |
|--|--|
| 文件 | `kernel/notifier.c`, `include/linux/notifier.h` |
| 类型 | 多种（原子/阻塞/SRCU/raw） |
| 典型用途 | 重启通知、设备热插拔、CPU 热插拔、网络事件 |

### 3.13 后台内核线程 — 异步内存管理

| 线程 | 触发条件 | 行为 |
|--|--|--|
| kswapd | 内存水位不足 | 异步回收页面 |
| kcompactd | 内存碎片化 | 异步内存压缩 |
| flusher (writeback) | 脏页超限 | 异步回写脏页 |
| khugepaged | 透明大页扫描 | 异步合并小页面为 THP |
| oom_reaper | OOM 触发 | 异步杀死进程释放内存 |

### 3.14 io_uring — 异步 I/O 框架

| 项目 | 说明 |
|--|--|
| 执行上下文 | 用户态发起，内核态处理 |
| 文件 | `io_uring/` |
| 关键 API | `io_uring_setup()`, `io_uring_enter()`, `io_uring_register()` |
| 典型用途 | 高性能存储/网络，零拷贝异步 I/O |
| 特点 | 共享 ring buffer 避免系统调用开销 |

### 3.15 fasync / SIGIO — 信号驱动 I/O（已过时）

| 项目 | 说明 |
|--|--|
| API | `fasync_helper()`, `kill_fasync()` |
| 典型用途 | 文件描述符就绪时发送 SIGIO 信号 |
| 状态 | 已被 epoll / io_uring 取代 |

---

## 4 综合对比表

| 机制 | 上下文 | 可睡眠 | 并行度 | 主要用途 |
|--|--|--|--|--|
| irq_work | NMI/硬中断 | 否 | 按 CPU | 跨 CPU 通知 |
| Softirq | 硬中断 | 否 | 高（多 CPU 并行） | 网络、块层 |
| BH Workqueue | softirq | 否 | 每 CPU 串行 | 替代 tasklet |
| Tasklet | softirq | 否 | 每 tasklet 串行 | 旧驱动下半部 |
| Timer | softirq | 否 | 按 CPU 分桶 | 超时处理 |
| RCU callback | softirq/kthread | 否 | 按宽限期 | 读端无锁同步 |
| Workqueue | 进程 | 是 | 可配置 | 通用延迟工作 |
| Threaded IRQ | 进程 | 是 | 按设备 | 中断线程化 |
| kthread | 进程 | 是 | 完全可控 | 长时间后台任务 |
| async_schedule | 进程 | 是 | 按 wq | 启动并行化 |
| task_work | 进程 | 是 | 按 task | 返回用户态执行 |
| io_uring | 用户/进程 | 是 | 高 | 异步 I/O |
| notifier chain | 取决于类型 | 取决于类型 | 链式回调 | 事件通知 |

---

## 5 完整执行路径全景图

```
硬件中断到达
  │
  ├─ irq_work (NMI/硬中断上下文, 最低延迟)
  │
  ├─ irq_exit → invoke_softirq()
  │    ├─ Softirq (网络/块层)
  │    │    ├─ Tasklet (已弃用)
  │    │    └─ BH Workqueue (替代 tasklet)
  │    ├─ Timer (timer_list / hrtimer)
  │    └─ RCU callback (call_rcu)
  │
  ├─ Threaded IRQ (内核线程)
  │
  ├─ kworker 线程池
  │    ├─ Workqueue (普通/可睡眠)
  │    ├─ async_schedule (启动并行化)
  │    └─ irq_work_queue (跨 CPU 通知)
  │
  ├─ 专用内核线程
  │    ├─ kthread (直接创建)
  │    ├─ kswapd / kcompactd / writeback
  │    └─ ktimersoftd (RT 内核)
  │
  ├─ syscall 返回用户态路径
  │    └─ task_work (延迟 fput/信号投递)
  │
  └─ 用户态
       ├─ io_uring (共享 ring buffer)
       ├─ epoll (事件通知)
       └─ signal/SIGIO (信号驱动, 已过时)
```

---

## 6 选择指南

```
需要异步执行一个任务时，按以下条件判断：

1. 可以在硬中断/softirq 上下文执行（不能睡眠）？
   ├─ 需要跨 CPU 通知，最低延迟  → irq_work
   ├─ 高性能，可并行             → Softirq
   ├─ 简单串行化，替代 tasklet   → BH Workqueue
   └─ 按时间触发                → Timer / hrtimer

2. 需要在进程上下文执行（可以睡眠）？
   ├─ 通用延迟工作              → Workqueue
   ├─ 中断下半部简化            → Threaded IRQ
   ├─ 长时间后台任务            → kthread
   ├─ 启动阶段并行化            → async_schedule
   ├─ 返回用户态时执行          → task_work
   └─ 高性能异步 I/O           → io_uring
```