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