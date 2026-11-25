# async_schedule — 异步函数调度

## 1 实现原理

`async_schedule` 是专为内核启动阶段并行化设备初始化而设计的异步执行机制，基于 workqueue 实现。核心设计如下：

- **sequence cookie 排序**：每个异步调用分配一个递增的 sequence cookie（`async_cookie_t`），用于确保有序性。
- **同步域（Domain）**：支持多个同步域，同一域内的异步调用相对于该域有序。全局域参与 `async_synchronize_full()` 同步。
- **基于 workqueue**：内部使用专用的 `async_wq`（unbound workqueue）执行异步函数。
- **退化为同步执行**：当内存不足或入队数超过 `MAX_WORK（32768）` 时，退化为同步执行。
- **启动时并行化**：主要用于设备驱动 `probe()` 阶段，允许同时初始化多个设备。

## 2 使用场景

- **设备初始化并行化**：启动阶段同时 probe 多个设备，缩短启动时间。
- **异步发现操作**：硬件设备发现、固件加载等可并行执行的操作。
- **同步域隔离**：使用 `ASYNC_DOMAIN_EXCLUSIVE` 创建独立域，退出作用域即完成同步。

## 3 代码调用栈

```
调度:
async_schedule(func, data)
  └→ async_schedule_node(func, data, NUMA_NO_NODE)
      └→ async_schedule_node_domain(func, data, node, &async_dfl_domain)
          └→ 分配 async_entry (GFP_ATOMIC)
          └→ 如果内存不足或 entry_count > MAX_WORK
              └→ func(data, newcookie)  ← 同步执行 (退化)
          └→ __async_schedule_node_domain()
              └→ 分配 cookie, 加入 pending 链表
              └→ queue_work_node(node, async_wq, &entry->work)

执行:
async_run_entry_fn(work)  ← 在 kworker 线程中执行
  └→ entry->func(entry->data, entry->cookie)  ← 回调
  └→ 从 pending 链表移除
  └→ kfree(entry)
  └→ wake_up(&async_done)  ← 唤醒等待者

同步等待:
async_synchronize_full()
  └→ async_synchronize_cookie_domain(ASYNC_COOKIE_MAX, NULL)
      └→ wait_event(async_done, lowest_in_progress(domain) >= cookie)
```

## 4 流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    async_schedule 流程                            │
│                                                                   │
│  async_schedule(func, data)                                      │
│    │                                                              │
│    ▼                                                              │
│  分配 async_entry (GFP_ATOMIC)                                   │
│    │                                                              │
│    ├─ 失败 → func(data, cookie) 同步执行                          │
│    │                                                              │
│    └─ 成功 → __async_schedule_node_domain()                      │
│        │                                                          │
│        ▼                                                          │
│  1. newcookie = next_cookie++                                    │
│  2. 将 entry 加入 domain->pending 链表                            │
│  3. 如果 domain->registered → 加入全局 pending 链表               │
│  4. entry_count++                                                 │
│  5. queue_work_node(node, async_wq, &entry->work)                 │
│                                                                   │
│  返回 cookie 给调用者                                              │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    async_run_entry_fn() 执行                     │
│                                                                   │
│  kworker 线程执行:                                                │
│  1. entry->func(entry->data, entry->cookie)  ← 用户回调          │
│  2. 从 domain_list 和 global_list 删除                            │
│  3. kfree(entry)                                                  │
│  4. atomic_dec(&entry_count)                                      │
│  5. wake_up(&async_done)  ← 唤醒 async_synchronize_*() 等待者    │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    async_synchronize_full() 同步                 │
│                                                                   │
│  调用者等待所有已提交的异步函数完成:                               │
│  wait_event(async_done, lowest_in_progress(domain) >= cookie)    │
│    └→ lowest_in_progress() 返回 pending 链表中最小 cookie         │
│    └→ 当最小 cookie >= 等待的 cookie 时，说明所有更早的任务完成   │
│                                                                   │
│  典型用法: 设备驱动 init 函数末尾                                  │
│    async_schedule(probe_fn, dev);                                 │
│    // ... 其他初始化                                              │
│    async_synchronize_full();  // 等待所有异步 probe 完成           │
└─────────────────────────────────────────────────────────────────┘
```

## 5 关键数据结构

### `struct async_domain` — 同步域

```c
// include/linux/async.h
struct async_domain {
    struct list_head pending;   // 域内挂起的 async_entry 链表
    unsigned registered:1;      // 是否注册到全局域 (影响 async_synchronize_full)
};

// 注册到全局域 (参与全局同步)
#define ASYNC_DOMAIN(_name) \
    struct async_domain _name = { .pending = LIST_HEAD_INIT(_name.pending), \
                                  .registered = 1 }

// 独立域 (不参与全局同步，退出作用域即完成)
#define ASYNC_DOMAIN_EXCLUSIVE(_name) \
    struct async_domain _name = { .pending = LIST_HEAD_INIT(_name.pending), \
                                  .registered = 0 }
```

### `struct async_entry` — 异步执行条目

```c
// kernel/async.c
struct async_entry {
    struct list_head domain_list;    // 挂入 domain->pending 链表
    struct list_head global_list;    // 挂入全局 async_global_pending 链表
    struct work_struct work;         // 内嵌 work_struct (在 async_wq 上执行)
    async_cookie_t cookie;           // 序列 cookie (递增)
    async_func_t func;               // 用户回调函数
    void *data;                      // 用户数据
    struct async_domain *domain;     // 所属同步域
};
```

### 关键 API

```c
// 类型定义
typedef u64 async_cookie_t;
typedef void (*async_func_t)(void *data, async_cookie_t cookie);

// 调度
async_cookie_t async_schedule(async_func_t func, void *data);
async_cookie_t async_schedule_domain(async_func_t func, void *data,
                                     struct async_domain *domain);
async_cookie_t async_schedule_dev(async_func_t func, struct device *dev);
bool async_schedule_dev_nocall(async_func_t func, struct device *dev);

// 同步等待
void async_synchronize_full(void);
void async_synchronize_cookie(async_cookie_t cookie);
void async_synchronize_cookie_domain(async_cookie_t cookie,
                                     struct async_domain *domain);
```