# RCU callback — RCU 回调

## 1 实现原理

RCU（Read-Copy-Update）回调是 RCU 机制的核心组成部分，在宽限期（Grace Period）结束后执行预注册的回调函数。核心设计如下：

- **分段回调链表**：`rcu_segcblist` 将回调分为 4 个段（`RCU_DONE_TAIL`、`RCU_WAIT_TAIL`、`RCU_NEXT_READY_TAIL`、`RCU_NEXT_TAIL`），分别表示不同宽限期阶段注册的回调。
- **宽限期驱动**：`call_rcu()` 注册回调后，等待所有 pre-existing 读端临界区完成，然后调用回调。
- **执行上下文**：可在 `RCU_SOFTIRQ`（主线）或专用 kthread（nocb 模式）中执行。
- **批量处理**：回调按批执行，单次处理完一个段的所有回调。

## 2 使用场景

- **资源延迟释放**：`kfree_rcu()` 在宽限期后释放内存。
- **RCU 保护的数据结构更新**：替换指针后，等待旧数据结构不再被引用再释放。
- **SLAB 销毁**、**模块卸载**、**文件系统 inode 回收**。

## 3 代码调用栈

```
注册:
call_rcu(&head, my_callback)
  └→ __call_rcu_common()
      └→ 将 head 加入 rcu_data->cblist 的 RCU_NEXT_TAIL 段
      └→ 如果需要 → 启动新的宽限期

宽限期结束:
  └→ rcu_gp_cleanup() 或 rcu_report_qs_rdp()
      └→ rcu_advance_cbs()  (将回调分段前移)
      └→ raise_softirq(RCU_SOFTIRQ)

执行:
RCU_SOFTIRQ → rcu_core()
  └→ rcu_do_batch()
      └→ rcu_segcblist_extract_done_cbs()  // 取出 DONE 段回调
      └→ 遍历每个回调:
          ├→ rhp->func(rhp)  // 执行回调
          └→ cond_resched()
      └→ rcu_segcblist_add_len()  // 更新计数
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    RCU Callback 流程                              │
│                                                                   │
│  call_rcu(&head, func)                                           │
│    └→ 将回调追加到 rcu_data->cblist RCU_NEXT_TAIL 段             │
│    └→ 如果当前无活跃宽限期 → 启动 GP                             │
│                                                                   │
│  宽限期生命周期:                                                  │
│                                                                   │
│  时间线:                                                          │
│  ──────┬──────────────┬──────────────┬────────────────→          │
│        │              │              │                            │
│    call_rcu()      GP 开始       GP 结束                         │
│        │              │              │                            │
│        │  NEXT_TAIL → WAIT_TAIL → DONE_TAIL → 执行回调           │
│        │              │              │                            │
│  分段迁移:                                                        │
│  rcu_advance_cbs() 在 GP 结束时将回调逐步前移                     │
│  RCU_NEXT_TAIL → RCU_NEXT_READY_TAIL → RCU_WAIT_TAIL → DONE_TAIL│
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    rcu_do_batch() 执行回调                        │
│                                                                   │
│  1. rcu_segcblist_extract_done_cbs() 取出 DONE 段                 │
│  2. 循环处理:                                                     │
│     ┌─────────────────────────────────────────────────────┐       │
│     │ while (head) {                                     │       │
│     │   next = head->next;                               │       │
│     │   local_bh_disable()                               │       │
│     │   head->func(head);    ← 回调函数                   │       │
│     │   local_bh_enable()                                │       │
│     │   head = next;                                     │       │
│     │   cond_resched();    ← 允许抢占                    │       │
│     │ }                                                  │       │
│     └─────────────────────────────────────────────────────┘       │
│  3. 更新计数，检查是否需要重新加速                               │
│                                                                   │
│  限制: 每次最多处理 10 个回调 (或根据限额)                        │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct rcu_head` — RCU 回调描述符

```c
// include/linux/types.h
struct rcu_head {
    struct rcu_head *next;          // 链表下一个节点
    void (*func)(struct rcu_head *head);  // 宽限期结束后调用的回调函数
};
```

### `struct rcu_segcblist` — 分段回调链表

```c
// include/linux/rcu_segcblist.h
struct rcu_segcblist {
    struct rcu_head *head;                  // 链表头
    struct rcu_head **tails[RCU_CBLIST_NSEGS];  // 4 个段的尾指针
    unsigned long gp_seq[RCU_CBLIST_NSEGS];     // 各段对应的 GP 序列号
    atomic_long_t len;                      // 总长度
    long seglen[RCU_CBLIST_NSEGS];          // 各段长度
    u8 flags;                               // 标志位
};

// 段索引:
#define RCU_DONE_TAIL       0   // 宽限期已结束，可执行回调
#define RCU_WAIT_TAIL       1   // 等待当前宽限期结束
#define RCU_NEXT_READY_TAIL 2   // 可以处理下一个宽限期
#define RCU_NEXT_TAIL       3   // 最新注册的回调
```

### `struct rcu_cblist` — 简易回调链表

```c
// include/linux/rcu_segcblist.h
struct rcu_cblist {
    struct rcu_head *head;    // 链表头
    struct rcu_head **tail;   // 链表尾指针
    long len;                 // 回调数量
};
```

### `struct rcu_synchronize` — 同步等待 RCU

```c
// include/linux/rcupdate_wait.h
struct rcu_synchronize {
    struct rcu_head head;             // RCU 回调头
    struct completion completion;     // 完成量，等待回调执行
    struct rcu_gp_oldstate oldstate;  // 调试用
};

// 使用: synchronize_rcu() 内部使用该结构
//   call_rcu(&rs.head, wakeme_after_rcu);
//   wait_for_completion(&rs.completion);
```

### 关键 API

```c
// 注册回调 (通用)
void call_rcu(struct rcu_head *head, rcu_callback_t func);

// 紧急回调 (非 lazy 模式)
void call_rcu_hurry(struct rcu_head *head, rcu_callback_t func);

// kfree 快捷方式 - 宽限期后自动 kfree
#define kfree_rcu(ptr, rhf)  kvfree_rcu_arg_2(ptr, rhf)

// 同步等待 (内部使用 call_rcu + completion)
void synchronize_rcu(void);
```

## 6 本树形态核实：Tree RCU + PREEMPT_RT

先读源码确认本树形态：`.config` 中 `CONFIG_TREE_RCU=y`、`CONFIG_PREEMPT_RCU=y`、`CONFIG_PREEMPT_RT=y`，故走 `kernel/rcu/tree.c` + `tree_plugin.h`（而非 `tiny.c`）。一个关键事实是 `use_softirq` 的取值：

```c
// kernel/rcu/tree.c
static bool use_softirq = !IS_ENABLED(CONFIG_PREEMPT_RT);
module_param(use_softirq, bool, 0444);
```

本树 RT=y，所以 `use_softirq=false`。这直接影响回调执行上下文：`rcu_init()` 只在 `use_softirq` 为真时才 `open_softirq(RCU_SOFTIRQ, rcu_core_si)`；`invoke_rcu_core()` 也据此二选一：

```c
// kernel/rcu/tree.c: invoke_rcu_core()
if (use_softirq)
    raise_softirq(RCU_SOFTIRQ);
else
    invoke_rcu_core_kthread();   // 本树走这里：唤醒 rcuc/%u
```

因此**本树 RCU 回调不在 RCU_SOFTIRQ 里跑，而在每 CPU 的 `rcuc/%u` 内核线程**（`rcu_cpu_thread_spec`，`thread_comm="rcuc/%u"`，由 `rcu_spawn_core_kthreads()` 注册）。第 1 节"可在 RCU_SOFTIRQ（主线）"的说法对应非 RT 配置；本树该 softirq 路径源码存在，当前配置未启用。

另一个核实点：`rcu_head` 在本树是 `callback_head` 的宏别名（`include/linux/types.h`），并非独立结构：

```c
// include/linux/types.h
struct callback_head {
    struct callback_head *next;
    void (*func)(struct callback_head *head);
} __attribute__((aligned(sizeof(void *))));
#define rcu_head callback_head
```

对齐到指针大小是为了保证 `next` 的 bit0 恒为 0（供 lazy RCU 复用低位、并与 `struct page` 的 PageTail 编码区分）。

## 7 call_rcu 注册路径

`call_rcu()` → `__call_rcu_common(head, func, enable_rcu_lazy)`。本树 `CONFIG_RCU_LAZY` 未启用，`enable_rcu_lazy=false`，故 `call_rcu()` 与 `call_rcu_hurry()` 实际都传 `lazy=false`（lazy 分支源码存在，当前配置未启用）。`__call_rcu_common()` 要点：

```c
// kernel/rcu/tree.c: __call_rcu_common() 片段
if (debug_rcu_head_queue(head)) {        // 检测重复 call_rcu
    WRITE_ONCE(head->func, rcu_leak_callback);  // 泄漏而非二次释放
    return;
}
head->func = func; head->next = NULL;
local_irq_save(flags);
rdp = this_cpu_ptr(&rcu_data);
if (unlikely(rcu_rdp_is_offloaded(rdp)))
    call_rcu_nocb(rdp, head, func, flags, lazy);   // nocb 卸载
else
    call_rcu_core(rdp, head, func, flags);
local_irq_restore(flags);
```

`call_rcu_core()` 调 `rcutree_enqueue()` → `rcu_segcblist_enqueue()` 把回调挂到 `RCU_NEXT_TAIL` 段；若回调数超过 `qhimark` 高水位，则把 `blimit` 提到 `DEFAULT_MAX_RCU_BLIMIT`（10000）并 `rcu_force_quiescent_state()` 催促宽限期，或 `rcu_accelerate_cbs_unlocked()` 启动新 GP。

## 8 rcu_core 与 rcu_do_batch 执行

`rcu_core()`（`kernel/rcu/tree.c`）是每 CPU 的 RCU 核心处理：报告延迟静止态（`rcu_preempt_deferred_qs`）、`rcu_check_quiescent_state()`、必要时加速回调分段，然后若有就绪回调且非 offloaded 就 `rcu_do_batch()`，跑完若仍有剩余再 `invoke_rcu_core()`。

`rcu_do_batch()` 的批量限额与执行：

```c
// kernel/rcu/tree.c: rcu_do_batch() 片段
pending = rcu_segcblist_get_seglen(&rdp->cblist, RCU_DONE_TAIL);
div = READ_ONCE(rcu_divisor);            // 默认 7
bl = max(rdp->blimit, pending >> div);   // blimit 默认 10
rcu_segcblist_extract_done_cbs(&rdp->cblist, &rcl);
for (; rhp; rhp = rcu_cblist_dequeue(&rcl)) {
    f = rhp->func;
    WRITE_ONCE(rhp->func, (rcu_callback_t)0L);
    f(rhp);                              // 执行回调
    if (in_serving_softirq()) {
        if (count >= bl && (need_resched() || !is_idle_task(current)))
            break;
    } else {                             // rcuc/rcuo 线程上下文
        local_bh_enable();
        cond_resched_tasks_rcu_qs();
        local_bh_disable();
    }
}
rcu_segcblist_insert_done_cbs(&rdp->cblist, &rcl);  // 剩余回填
rcu_segcblist_add_len(&rdp->cblist, -count);
```

注意第 4 节"每次最多处理 10 个"是 `blimit` 默认值（`DEFAULT_RCU_BLIMIT=10`），实际限额是 `max(blimit, pending>>rcu_divisor)`，回调洪水时会放大。在 softirq 上下文（非 RT）受 `need_resched`/时间限额约束以免饿死其他向量；在 rcuc 线程上下文（本树）则 `local_bh_enable()` + `cond_resched_tasks_rcu_qs()` 主动让出，并报告 Tasks RCU 静止态。

## 9 分段链表与 nocb

`struct rcu_segcblist`（`include/linux/rcu_segcblist.h`）用 4 个尾指针把单链表切成 4 段，`gp_seq[]` 记录各段对应的宽限期序号：

```c
// include/linux/rcu_segcblist.h
struct rcu_segcblist {
    struct rcu_head *head;
    struct rcu_head **tails[RCU_CBLIST_NSEGS];
    unsigned long gp_seq[RCU_CBLIST_NSEGS];
#ifdef CONFIG_RCU_NOCB_CPU
    atomic_long_t len;     // nocb 下需原子
#else
    long len;              // 本树走这里
#endif
    long seglen[RCU_CBLIST_NSEGS];
    u8 flags;              // SEGCBLIST_ENABLED / SEGCBLIST_OFFLOADED
};
```

宽限期推进时 `rcu_advance_cbs()` 把回调从 `NEXT_TAIL` → `NEXT_READY_TAIL` → `WAIT_TAIL` → `DONE_TAIL` 前移，`DONE_TAIL` 段即可执行。

nocb（callback offload）：本树 `.config` 未设 `CONFIG_RCU_NOCB_CPU`，故 `rcu_rdp_is_offloaded()` 恒假，`call_rcu_nocb()`/`rcuo` 线程等路径源码存在（`kernel/rcu/tree_nocb.h`），当前配置未启用。启用后回调注册与执行都卸载到 `rcuo` kthread，`len` 改为原子、`rcu_do_batch()` 用 `rcu_nocb_lock_irqsave()` 保护。

## 10 初始化、配置与调试

`rcu_init()` 在 `start_kernel()` 中调用（`init/main.c`）：`rcu_init_geometry()` 建 `rcu_node` 树、`rcu_init_one()` 初始化 `rcu_state`，按 `use_softirq` 决定是否注册 softirq，并创建 `rcu_gp_wq`（`WQ_MEM_RECLAIM|WQ_PERCPU`）与 `sync_wq` 两个工作队列供 SRCU/加速宽限期使用。

- 可调参数（`kernel/rcu/tree.c`，module_param）：`blimit`/`qhimark`/`qlowmark`（0444）、`rcu_divisor`（0644）、`use_softirq`（0444）。
- `CONFIG_RCU_TRACE=y` 但 `CONFIG_FTRACE` 当前配置未启用，`trace_rcu_batch_start/end`、`trace_rcu_invoke_callback` 等运行时不可用。
- `CONFIG_DEBUG_OBJECTS_RCU_HEAD` 可检测重复 `call_rcu`（`debug_rcu_head_queue`）；`rcu_do_batch()` 末尾多个 `WARN_ON_ONCE` 校验计数一致性。
- 回调执行上下文：本树为 `rcuc/%u` 线程，可用 `ps` 观察；非 RT 为 `RCU_SOFTIRQ`（见 [软中断](02-softirq.md)）。

## 11 关键文件清单

| 文件 | 作用 |
|--|--|
| `kernel/rcu/tree.c` | `call_rcu`/`rcu_core`/`rcu_do_batch`/`rcu_init`/rcuc 线程 |
| `kernel/rcu/tree_plugin.h` | PREEMPT_RCU 读端/静止态插件 |
| `kernel/rcu/tree_nocb.h` | nocb 卸载（当前配置未启用） |
| `include/linux/rcu_segcblist.h` | 分段回调链表 |
| `include/linux/types.h` | `callback_head`/`rcu_head` 别名 |
| `include/linux/rcupdate.h` | `call_rcu`/`synchronize_rcu` 声明 |

相关笔记：[软中断](02-softirq.md)、[内核线程](09-kthread.md)、[异步执行概要](async-execution-summary.md)。