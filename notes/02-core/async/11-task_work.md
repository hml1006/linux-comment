# task_work — 返回用户态时执行

## 1 概述

task_work 是一种在任务（进程/线程）即将从内核态返回用户态时执行回调的异步机制。它利用了内核的 `TIF_NOTIFY_RESUME` 标志位，在 `exit_to_user_mode()` 路径中触发回调执行。

- **文件**: `kernel/task_work.c`, `include/linux/task_work.h`
- **执行上下文**: 进程上下文（可睡眠）
- **触发点**: `exit_to_user_mode()` → `task_work_run()`

## 2 实现原理

### 2.1 核心数据结构

```c
// include/linux/types.h
struct callback_head {
    struct callback_head *next;      // 链表节点，连接多个 task_work
    void (*func)(struct callback_head *head);  // 回调函数
} __attribute__((aligned(sizeof(void *))));
```

每个 `task_struct` 中有一个 `task_works` 字段，指向一个由 `struct callback_head` 组成的单链表（LIFO 顺序）：

```c
// include/linux/sched.h
struct task_struct {
    struct callback_head *task_works;  // 待执行的 task_work 链表头
    ...
};
```

### 2.2 通知模式

task_work_add() 支持多种通知模式，决定如何唤醒目标任务：

| 模式 | 说明 |
|--|--|
| `TWA_NONE` | 不通知，等待目标任务自行调度 |
| `TWA_RESUME` | 设置 `TIF_NOTIFY_RESUME`，在返回用户态时执行 |
| `TWA_SIGNAL` | 发送信号打断目标任务，强制其返回用户态执行 |
| `TWA_SIGNAL_NO_IPI` | 类似 `TWA_SIGNAL`，但不发送 IPI |
| `TWA_NMI_CURRENT` | 仅用于当前任务，且当前上下文为 NMI |

### 2.3 执行流程

```
task_work_add(task, work, notify)
  │
  ├─ 1. 将 work 通过 CAS 原子操作插入 task->task_works 链表头 (LIFO)
  │
  └─ 2. 根据 notify 模式设置通知：
       ├─ TWA_RESUME       → set_notify_resume(task) → 设置 TIF_NOTIFY_RESUME
       ├─ TWA_SIGNAL       → set_notify_signal(task) → 发送信号
       ├─ TWA_SIGNAL_NO_IPI → __set_notify_signal(task)
       └─ TWA_NMI_CURRENT  → 设置 TIF_NOTIFY_RESUME + irq_work_queue (IPI)
```

```
task_work_run()  — 在 exit_to_user_mode() 路径中调用
  │
  ├─ 1. 通过 CAS 原子摘取整个 task->task_works 链表
  ├─ 2. 遍历链表，逐个调用 work->func(work)
  └─ 3. 如果链表为空且任务正在退出 (PF_EXITING)，设置 work_exited 哨兵
```

### 2.4 执行流程图

```
用户态 syscall 进入内核
  │
  ├─ syscall 处理 (可能调用 task_work_add 添加 work)
  │
  └─ syscall 返回用户态
       │
       └─ exit_to_user_mode()
            │
            └─ 检查 TIF_NOTIFY_RESUME
                 │
                 └─ true → task_work_run()
                      │
                      ├─ CAS 摘取 task->task_works 链表
                      ├─ 遍历链表
                      │    └─ work->func(work)  ← 可睡眠，可添加新的 task_work
                      └─ 链表为空 → 返回用户态
```

## 3 使用场景

| 场景 | 说明 |
|--|--|
| 延迟 fput() | 文件引用计数归零后，延迟释放文件结构体 |
| 信号投递 | SIGURG 等信号通过 task_work 在安全点投递 |
| io_uring 完成通知 | 异步 I/O 完成时，通过 task_work 唤醒用户态 |
| POSIX 定时器 | CPU 定时器到期时，通过 task_work 处理 |
| cpuset 内存迁移 | 内存迁移完成后，通过 task_work 刷新 |

## 4 关键 API

| API | 说明 |
|--|--|
| `task_work_add(task, work, notify)` | 添加 task_work，返回 0 成功或 -ESRCH |
| `task_work_run()` | 执行当前任务所有待处理的 task_work |
| `task_work_cancel(task, cb)` | 取消指定 work |
| `task_work_cancel_func(task, func)` | 取消匹配指定回调函数的 work |
| `init_task_work(work, func)` | 静态初始化 task_work |

## 5 代码调用栈

### 5.1 添加 task_work (io_uring 完成通知示例)

```
io_req_task_complete()
  └─ io_req_task_work_add(req)
       └─ task_work_add(current, &req->io_task_work, TWA_RESUME)
            ├─ CAS 插入链表
            └─ set_notify_resume(current)
```

### 5.2 执行 task_work

```
el0_sync()           // ARM64 系统调用返回
  └─ ret_to_user()
       └─ exit_to_user_mode()
            ├─ ct_work_flush()
            └─ task_work_run()
                 └─ work->func(work)
                      └─ io_req_task_complete()
```

## 6 与其他异步机制的对比

| 特性 | task_work | irq_work | Workqueue |
|--|--|--|--|
| 执行上下文 | 进程（可睡眠） | NMI/硬中断 | 进程（可睡眠） |
| 触发时机 | 返回用户态 | 中断返回路径 | kworker 调度 |
| 延迟 | 低 | 极低 | 中 |
| 指定目标任务 | 是（按 task） | 否（按 CPU） | 否（按 worker） |
| 使用场景 | 任务本地异步操作 | 跨 CPU 通知 | 通用延迟工作 |

## 7 task_work_add 实现核实

`kernel/task_work.c` 用静态哨兵 `work_exited`（只需 `->next==NULL`）标记任务已退出。`task_work_add()` 以 CAS 循环把 work 头插到 `task->task_works`（LIFO），若链表头已是 `work_exited` 则返回 `-ESRCH`：

```c
// kernel/task_work.c: task_work_add()
head = READ_ONCE(task->task_works);
do {
    if (unlikely(head == &work_exited))
        return -ESRCH;
    work->next = head;
} while (!try_cmpxchg(&task->task_works, &head, work));
switch (notify) {
case TWA_RESUME:        set_notify_resume(task);        break;
case TWA_SIGNAL:        set_notify_signal(task);        break;
case TWA_SIGNAL_NO_IPI: __set_notify_signal(task);      break;
case TWA_NMI_CURRENT:
    set_tsk_thread_flag(current, TIF_NOTIFY_RESUME);
    irq_work_queue(this_cpu_ptr(&irq_work_NMI_resume));
    break;
}
```

`TWA_NMI_CURRENT` 要求 `task==current` 且 `CONFIG_IRQ_WORK=y`，否则 `-EINVAL`；它借助一个 no-op 回调的 `irq_work_NMI_resume`（`IRQ_WORK_INIT_HARD`）发 IPI，只为"挠一下"返回用户态路径，TIF 标志已先行置好。`enum task_work_notify_mode`（`include/linux/task_work.h`）取值依次为 `TWA_NONE=0`、`TWA_RESUME`、`TWA_SIGNAL`、`TWA_SIGNAL_NO_IPI`、`TWA_NMI_CURRENT`。

## 8 task_work_run 与取消

`task_work_run()` 外层 `for(;;)`：CAS 把整条链表摘为 NULL（若 `PF_EXITING` 且链表空则换成 `work_exited` 哨兵，阻止后续 add）；随后 `raw_spin_lock_irq(&task->pi_lock)` 再解锁，与 `task_work_cancel_match()` 同步；内层遍历 `work->func(work)` 并 `cond_resched()`。因为回调里可能再 `task_work_add()`，外层循环会重新摘取直到链表空：

```c
// kernel/task_work.c: task_work_run() 内层
do {
    next = work->next;
    work->func(work);
    work = next;
    cond_resched();
} while (work);
```

`task_work_cancel_match()` 持 `pi_lock` 遍历，用 `try_cmpxchg(pprev, &work, work->next)` 摘除匹配项；`task_work_cancel_func()` 按 `cb->func==func` 匹配（取消最后入队的一个），`task_work_cancel()` 按指针匹配。[线程化中断](08-threaded-irq.md) 的 `irq_thread` 正是用 `task_work_add(TWA_NONE)` 注册退出兜底、正常退出时 `task_work_cancel_func()` 撤销。

## 9 返回用户态路径核实（arm64）

第 5.2 节示意的 `el0_sync → ret_to_user → do_notify_resume` 是旧形态。本树 arm64 已改用**通用 entry 框架**：`arch/arm64/kernel/signal.c` 中不再有 `do_notify_resume`/`task_work_run` 调用点，实际路径为：

```
arm64_syscall_exit_to_user_mode()      // arch/arm64/kernel/entry-common.c
  → exit_to_user_mode()
    → exit_to_user_mode_loop()         // kernel/entry/common.c
      → __exit_to_user_mode_loop()
        → if (ti_work & _TIF_NOTIFY_RESUME)
              resume_user_mode_work(regs)   // include/linux/resume_user_mode.h
                → task_work_run()
```

`resume_user_mode_work()` 先 `clear_thread_flag(TIF_NOTIFY_RESUME)`，`smp_mb__after_atomic()` 与 `task_work_add()` 的置位配对，再 `task_work_pending()` 判断后 `task_work_run()`；此外还处理 memcg 超限、blkcg 节流、rseq 慢路径。`set_notify_resume()`（同头文件）用 `test_and_set_tsk_thread_flag(TIF_NOTIFY_RESUME)` 去重后 `kick_process(task)` 把正在用户态跑的目标任务踢回内核。任务退出时 `do_exit()` 调 `exit_task_work()`（`kernel/exit.c`）→ `task_work_run()` 收尾。

## 10 配置、调试与关键文件

- `CONFIG_IRQ_WORK=y`（本树已启用），`TWA_NMI_CURRENT` 可用；`CONFIG_PREEMPT_RT=y` 下返回用户态路径同样经通用 entry。
- `kasan_record_aux_stack(work)` 记录辅助栈供 KASAN 报告；本树未启用 KASAN 时无开销。
- task_work 无专用 tracepoint；调试依赖回调自身日志与 `task_work_cancel` 返回值。
- 与 [irq_work](01-irq_work.md) 的分工：irq_work 按 CPU、在硬中断跑；task_work 按 task、在进程上下文返回用户态时跑，可睡眠。

| 文件 | 作用 |
|--|--|
| `kernel/task_work.c` | add/run/cancel 主体、NMI irq_work |
| `include/linux/task_work.h` | `callback_head` API、TWA 枚举、`exit_task_work` |
| `include/linux/resume_user_mode.h` | `set_notify_resume`/`resume_user_mode_work` |
| `kernel/entry/common.c` | `exit_to_user_mode_loop` 通用返回路径 |
| `arch/arm64/kernel/entry-common.c` | arm64  syscall/irq 返回用户态入口 |
| `kernel/exit.c` | `do_exit` → `exit_task_work` |

## 11 典型用户与通知模式选择

| 用户 | 机制 | 说明 |
|--|--|--|
| 延迟 `fput()` | `task_work_add(TWA_RESUME)` | 文件引用归零后推迟到返回用户态释放，避免在原子上下文释放 |
| io_uring 完成 | `TWA_RESUME`/`TWA_SIGNAL` | 异步 I/O 完成后通知目标任务收割 CQE |
| POSIX CPU 定时器 | task_work | 到期处理在安全点执行 |
| irq_thread 退出兜底 | `TWA_NONE` | 见 [线程化中断](08-threaded-irq.md)，正常退出再 cancel |

通知模式选择要点：

| 模式 | 是否唤醒目标 | 执行时机 | 适用 |
|--|--|--|--|
| `TWA_NONE` | 否 | 目标自行经过返回点/退出 | 目标本就会很快返回内核 |
| `TWA_RESUME` | `kick_process` | 返回用户态/进入 guest 前 | 常规延迟工作 |
| `TWA_SIGNAL` | 发信号+IPI | 尽快打断目标 | 需立即处理 |
| `TWA_SIGNAL_NO_IPI` | 发信号不发 IPI | 目标下次进内核 | 不要求极低延迟 |
| `TWA_NMI_CURRENT` | 自 IPI | NMI 后返回用户态 | 仅 current、NMI 上下文 |

`TWA_RESUME` 对阻塞任务不会主动唤醒（`set_notify_resume` 注释明确），若目标长期睡眠需改用 `TWA_SIGNAL`。

## 12 与信号机制的关系

`TWA_SIGNAL` 复用 `TIF_NOTIFY_SIGNAL`，在 `exit_to_user_mode_loop()` 里与 `_TIF_SIGPENDING` 一并触发 `arch_do_signal_or_restart()`，但 task_work 本身不是信号：它在 `resume_user_mode_work()` 中执行，不经过信号投递队列。[fasync/SIGIO](15-fasync_sigio.md) 则是真正的信号通知，二者可配合：驱动用 SIGIO 通知就绪，进程侧用 task_work 完成收尾。

## 13 顺序与同步保证

- **LIFO 顺序**：`task_work_add()` 头插，`task_work_run()` 从 head 顺序执行，因此后添加的先执行；源码注释明确"no ordering guarantee"，调用者不应依赖跨 work 的先后。
- **add/run 同步**：run 用 CAS 摘链，add 用 CAS 头插，二者无锁竞争；摘链后 run 持 `pi_lock` 空转一次，确保 cancel 不会误删正在执行的 head。
- **run/cancel 同步**：cancel 持 `pi_lock` 遍历，`try_cmpxchg` 失败说明与 add 或 run 竞争，重试即可。
- **置位/清标志配对**：`set_notify_resume()` 的 `test_and_set` 与 `resume_user_mode_work()` 的 `clear + smp_mb__after_atomic()` 配对，保证不漏执行。
- **退出收尾**：`PF_EXITING` 时 run 把空链表换成 `work_exited`，此后 add 返回 `-ESRCH`，调用者需自备替代释放路径。

综上，task_work 以"按任务挂载 + 返回用户态收割"填补了 irq_work（按 CPU、原子上下文）与 workqueue（按 worker、通用延迟）之间的空档，是任务本地异步收尾的首选。

相关笔记：[irq_work](01-irq_work.md)、[线程化中断](08-threaded-irq.md)、[fasync/SIGIO](15-fasync_sigio.md)、[异步执行概要](async-execution-summary.md)。