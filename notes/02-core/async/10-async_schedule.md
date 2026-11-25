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

## 6 核心状态与初始化

`kernel/async.c` 的全局状态：`next_cookie` 从 1 递增，`MAX_WORK=32768`，`ASYNC_COOKIE_MAX=ULLONG_MAX`；`async_global_pending` 汇总所有 registered 域的 pending，`async_dfl_domain` 是默认域（`ASYNC_DOMAIN` 宏，`registered=1`），`async_lock` 自旋锁保护，`async_done` 等待队列，`entry_count` 原子计数。

`async_init()` 创建专用 unbound 工作队列，并把 `min_active` 提到 `WQ_DFL_ACTIVE`：

```c
// kernel/async.c: async_init()
async_wq = alloc_workqueue("async", WQ_UNBOUND, 0);
BUG_ON(!async_wq);
workqueue_set_min_active(async_wq, WQ_DFL_ACTIVE);
```

注释说明：async 可能调度大量相互依赖的 work，unbound 队列默认 `min_active=8` 不足以支撑、会卡死，故提高下限。`async_entry` 内嵌 `work_struct`，回调统一为 `async_run_entry_fn`。

## 7 调度与退化路径

`async_schedule_node_domain()` 用 `GFP_ATOMIC` 分配 entry（允许 irq-off 调用者），内存不足或 `entry_count > MAX_WORK` 时**同步执行**退化：

```c
// kernel/async.c: async_schedule_node_domain()
entry = kzalloc_obj(struct async_entry, GFP_ATOMIC);
if (!entry || atomic_read(&entry_count) > MAX_WORK) {
    kfree(entry);
    spin_lock_irqsave(&async_lock, flags);
    newcookie = next_cookie++;
    spin_unlock_irqrestore(&async_lock, flags);
    func(data, newcookie);          // 就地同步执行
    return newcookie;
}
return __async_schedule_node_domain(func, data, node, domain, entry);
```

`__async_schedule_node_domain()` 在锁内分配 cookie、把 entry 挂到 `domain->pending`，若 `domain->registered` 再挂到 `async_global_pending`，`entry_count++`，最后 `queue_work_node(node, async_wq, &entry->work)` 按 NUMA 节点就近入队。`async_schedule_dev()`（`include/linux/async.h`）即 `async_schedule_node(func, dev, dev_to_node(dev))`。

`async_schedule_dev_nocall()` 是变体：分配失败或超限时**返回 false 而非同步执行**，由调用者自行决定回退。

## 8 执行与同步等待

`async_run_entry_fn()` 在 kworker 中执行：调 `entry->func(entry->data, entry->cookie)`，锁内从两条 pending 链表删除、`kfree(entry)`、`atomic_dec(&entry_count)`，最后 `wake_up(&async_done)`。前后用 `pr_debug` 打印 cookie、函数与耗时（微秒）。

同步等待的核心是 `lowest_in_progress()`：取域（或全局）pending 链表第一个 entry 的 cookie，空则返回 `ASYNC_COOKIE_MAX`。`async_synchronize_cookie_domain()` 据此等待：

```c
// kernel/async.c
void async_synchronize_full(void)
{
    async_synchronize_full_domain(NULL);   // → cookie_domain(ASYNC_COOKIE_MAX, NULL)
}
// async_synchronize_cookie_domain():
wait_event(async_done, lowest_in_progress(domain) >= cookie);
```

因为 cookie 单调递增、pending 链表按入队序排列，"最小未完成 cookie >= 目标 cookie"即等价于"所有更早提交的任务已完成"。`ASYNC_DOMAIN_EXCLUSIVE`（`registered=0`）的域不参与全局同步，适合局部作用域自行 `async_synchronize_full_domain()`。

## 9 与 driver probe 的关系

async 的主要用户是设备驱动核心 `drivers/base/dd.c`。驱动可设 `probe_type = PROBE_PREFER_ASYNCHRONOUS`（`include/linux/device/driver.h`）声明偏好异步 probe：

```c
// drivers/base/dd.c（调用点）
async_schedule_dev(__device_attach_async_helper, dev);   // __device_attach 路径
async_schedule_dev(__driver_attach_async_helper, dev);   // __driver_attach 路径
```

`__device_attach()` 同步匹配失败且允许异步、存在异步驱动时，`get_device()` 后 `async_schedule_dev()` 把 probe 丢到 async_wq 并行执行。`wait_for_device_probe()` 末尾调 `async_synchronize_full()` 确保所有异步 probe 完成后再继续（如启动阶段进入用户态前）。`current_is_async()` 通过 `worker->current_func == async_run_entry_fn` 判断当前是否 async worker，供驱动核心区分上下文。

## 10 配置、调试与关键文件

- 本树 `CONFIG_SCSI_SCAN_ASYNC` 未启用（源码存在，当前配置未启用），否则 SCSI 扫描也走 async。
- `pr_debug` 输出需动态调试开启；`async_synchronize_cookie_domain()` 记录等待耗时。
- 与 [工作队列](07-workqueue.md) 的关系：async 是 workqueue 之上的"有序异步调用"封装，额外提供 cookie 排序与同步域；普通 workqueue 无全局顺序保证。

| 文件 | 作用 |
|--|--|
| `kernel/async.c` | cookie/domain/调度/执行/同步主体 |
| `include/linux/async.h` | `async_domain`/`async_entry` API、`async_schedule_dev` |
| `drivers/base/dd.c` | 异步 probe 调用点、`wait_for_device_probe` |
| `include/linux/device/driver.h` | `probe_type`/`PROBE_PREFER_ASYNCHRONOUS` |

## 11 与其他异步机制的对比

| 维度 | async_schedule | workqueue | kthread |
|--|--|--|--|
| 顺序保证 | cookie 全局有序 | 无全局序 | 无 |
| 同步原语 | `async_synchronize_*` | `flush_work` | `kthread_stop` |
| 执行载体 | 专用 unbound wq | 各类 wq | 独立线程 |
| 退化策略 | 超限/缺内存同步执行 | 无 | 无 |
| 典型场景 | 启动期并行 probe | 通用延迟工作 | 长驻后台 |

async 的"乱序执行、有序可见"类似 CPU 乱序退休：回调可并行跑，但对外可见操作（如注册设备号）前应先 `async_synchronize_cookie()` 自己的 cookie，确保更早的任务已完成。

相关笔记：[工作队列](07-workqueue.md)、[内核线程](09-kthread.md)、[异步执行概要](async-execution-summary.md)。