# sched_yield 系统调用分析

## 1. 概述

主动让出 CPU，使当前进程自愿放弃处理器，调度器会选择另一个可运行的任务来执行。如果没有其他可运行的任务，调用进程会立即继续执行。

**原型：**

```c
SYSCALL_DEFINE0(sched_yield)
```

**参数：** 无

**返回值：**

- 总是返回 `0`

## 2. 使用场景

- **忙等待替代**: 在自旋等待循环中定期让出 CPU
- **协作式多任务**: 用户态协程或线程库中的调度点
- **优先级让渡**: 实时任务暂时让出 CPU 给其他任务执行

## 3. 函数调用栈

```
sched_yield()  (系统调用入口)
└─ kernel/sched/syscalls.c
   └─ do_sched_yield()
        ├─ rq = this_rq_lock_irq(&rf)          // 获取当前 CPU 的运行队列
        │
        ├─ schedstat_inc(rq->yld_count)        // 统计 yield 次数
        │
        ├─ rq->donor->sched_class->yield_task(rq)  // 调度类特定的 yield 处理
        │    ├─ [CFS] → fair_sched_class.yield_task()
        │    │    └─ 将当前任务放到运行队列末尾
        │    │
        │    ├─ [RT] → rt_sched_class.yield_task()
        │    │    └─ 将当前任务放到 RT 队列末尾
        │    │
        │    └─ [DL] → dl_sched_class.yield_task()
        │         └─ 将当前任务放到 Deadline 队列末尾
        │
        ├─ preempt_disable()                    // 关闭抢占
        ├─ rq_unlock_irq(rq, &rf)               // 释放运行队列锁
        │
        └─ schedule()                           // 主动调度
             └─ __schedule(SM_NONE)
                  ├─ pick_next_task()           // 选择下一个任务
                  └─ context_switch()           // 切换上下文
```

## 4. 关键数据结构

```c
// kernel/sched/sched.h
struct rq {
    ...
    unsigned int yld_count;       // yield 统计计数器
    struct task_struct *donor;    // 当前"捐赠"CPU 的任务
    ...
};

// 调度类 yield 操作
// CFS (fair.c): 将当前任务的虚拟运行时间提前，使其位于红黑树末尾
// RT (rt.c): 将当前任务移到 RT 优先级队列的末尾
// DL (deadline.c): 将当前任务移到 Deadline 队列的末尾
```

## 5. 流程图

```
sched_yield()
  │
  ├─ 获取当前 CPU 的运行队列锁
  │
  ├─ CFS 调度类:
  │    └─ yield_task_fair()
  │         ├─ dequeue_entity()  // 出队
  │         ├─ update_min_vruntime()  // 更新 vruntime
  │         └─ enqueue_entity()  // 重新入队（到末尾）
  │
  ├─ 释放运行队列锁，关闭抢占
  │
  └─ schedule()
       ├─ pick_next_task()  → 选择新任务
       │    └─ 如果当前任务是唯一可运行任务 → 继续运行当前任务
       │
       └─ context_switch()  → 切换到新任务
```

## 6. 使用示例

```c
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 主动让出 CPU
    printf("Yielding CPU...\n");
    sched_yield();
    printf("Back after yield\n");

    // 忙等待循环中使用 yield
    volatile int done = 0;
    // 假设另一个线程会将 done 设为 1
    while (!done) {
        sched_yield();  // 让出 CPU，避免忙等
    }

    return 0;
}
```

## 7. 注意事项

- `sched_yield` 总是返回 0（不会失败）
- 调度器在调用 `yield` 后仍然可以再次选择同一个任务（如果它是优先级最高的可运行任务）
- 不应依赖 `sched_yield` 来实现同步（如 `while(!event) yield()`），这可能导致优先级反转
- 内核文档明确警告：99% 的情况下使用 `yield` 都是错误的，正确做法是使用条件变量或 futex

## 8. 参考

- `kernel/sched/syscalls.c` — sched_yield/do_sched_yield 实现
- `kernel/sched/fair.c` — yield_task_fair 实现
- `kernel/sched/rt.c` — yield_task_rt 实现
- `kernel/sched/core.c` — schedule 实现
- [ARM64 系统调用表](../arm64-syscall-table.md#进程调度)