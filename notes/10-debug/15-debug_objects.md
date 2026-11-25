# debug_objects

## 概述

debug_objects 是 Linux 内核提供的通用对象生命周期调试基础设施，用于追踪内核对象的状态转换，检测对象使用错误，如：
- 重复初始化
- 在活跃状态下销毁
- 在未初始化状态下激活
- 释放仍在使用的对象
- 对象状态机违规

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                     debug_objects Architecture                     │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    对象生命周期状态机                        │   │
│  │                                                             │   │
│  │     NONE ───init──→ INIT ───activate──→ ACTIVE             │   │
│  │      │                   │                │                  │   │
│  │      │                   │                │                  │   │
│  │      │                   │                ▼                  │   │
│  │      │                   └───────←─── INACTIVE ←─deactivate-│   │
│  │      │                                           │            │   │
│  │      │                                           │            │   │
│  │      └────free←─── DESTROYED ←──destroy───────┘            │   │
│  │                                                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    核心数据结构                               │   │
│  │  • debug_obj          - 追踪对象状态                         │   │
│  │  • debug_obj_descr    - 对象类型描述符                       │   │
│  │  • debug_bucket       - 哈希桶                              │   │
│  │  • obj_pool           - 对象池（全局+每CPU）                  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    对象池管理                                │   │
│  │  • pool_global       - 全局对象池                           │   │
│  │  • pool_pcpu[]       - 每CPU对象池                          │   │
│  │  • pool_to_free      - 待释放对象池                         │   │
│  │  • batch 机制        - 批量分配/释放                        │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    错误检测与修复                            │   │
│  │  • debug_print_object() - 打印错误信息                       │   │
│  │  • debug_object_fixup() - 调用类型特定修复函数               │   │
│  │  • fixup_init/fixup_activate/fixup_destroy/fixup_free      │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    debugfs 接口                              │   │
│  │  /sys/kernel/debug/debug_objects/stats                     │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 核心数据结构

### debug_obj

```c
struct debug_obj {
    struct hlist_node		node;
    enum debug_obj_state		state;
    unsigned int			astate;
    union {
        void			*object;
        struct hlist_node	*batch_last;
    };
    const struct debug_obj_descr *descr;
};
```

- `node`: 哈希桶链表节点
- `state`: 对象状态
- `astate`: 当前活跃子状态
- `object`: 指向实际对象的指针
- `batch_last`: batch 中最后一个节点（用于对象池管理）
- `descr`: 对象类型描述符

### debug_obj_descr

```c
struct debug_obj_descr {
    const char		*name;
    void *(*debug_hint)(void *addr);
    bool (*is_static_object)(void *addr);
    bool (*fixup_init)(void *addr, enum debug_obj_state state);
    bool (*fixup_activate)(void *addr, enum debug_obj_state state);
    bool (*fixup_destroy)(void *addr, enum debug_obj_state state);
    bool (*fixup_free)(void *addr, enum debug_obj_state state);
    bool (*fixup_assert_init)(void *addr, enum debug_obj_state state);
};
```

- `name`: 对象类型名称
- `debug_hint`: 返回与对象相关的地址（用于标识）
- `is_static_object`: 判断对象是否为静态对象
- `fixup_*`: 各种状态转换失败时的修复函数

### debug_bucket

```c
struct debug_bucket {
    struct hlist_head	list;
    raw_spinlock_t		lock;
};

static struct debug_bucket	obj_hash[ODEBUG_HASH_SIZE];
```

哈希桶结构，用于存储和快速查找对象。

### obj_pool

```c
struct obj_pool {
    struct hlist_head	objects;
    unsigned int		cnt;
    unsigned int		min_cnt;
    unsigned int		max_cnt;
    struct pool_stats	stats;
} ____cacheline_aligned;
```

对象池结构，用于管理 debug_obj 的分配和回收。

## 对象状态

| 状态 | 值 | 描述 |
|------|-----|------|
| `ODEBUG_STATE_NONE` | 0 | 未初始化 |
| `ODEBUG_STATE_INIT` | 1 | 已初始化 |
| `ODEBUG_STATE_INACTIVE` | 2 | 已停用 |
| `ODEBUG_STATE_ACTIVE` | 3 | 活跃中 |
| `ODEBUG_STATE_DESTROYED` | 4 | 已销毁 |
| `ODEBUG_STATE_NOTAVAILABLE` | 5 | 不可用 |

## 状态转换规则

```
┌─────────────────────────────────────────────────────────────────────┐
│                     State Transition Rules                        │
│                                                                     │
│  debug_object_init()                                               │
│  ├─ NONE → INIT                                                    │
│  ├─ INIT → INIT (允许)                                             │
│  ├─ INACTIVE → INIT                                                │
│  └─ ACTIVE/DESTROYED → ERROR                                       │
│                                                                     │
│  debug_object_activate()                                           │
│  ├─ INIT → ACTIVE                                                  │
│  ├─ INACTIVE → ACTIVE                                              │
│  ├─ ACTIVE → ERROR                                                │
│  └─ DESTROYED → ERROR                                             │
│                                                                     │
│  debug_object_deactivate()                                         │
│  ├─ ACTIVE → INACTIVE (astate == 0)                                │
│  ├─ INIT → INACTIVE                                                │
│  ├─ INACTIVE → INACTIVE                                            │
│  └─ DESTROYED → ERROR (astate != 0)                                │
│                                                                     │
│  debug_object_destroy()                                            │
│  ├─ INIT → DESTROYED                                               │
│  ├─ INACTIVE → DESTROYED                                           │
│  ├─ ACTIVE → ERROR                                                 │
│  └─ DESTROYED → ERROR                                             │
│                                                                     │
│  debug_object_free()                                               │
│  ├─ NONE/INIT/INACTIVE/DESTROYED → FREE                           │
│  └─ ACTIVE → ERROR                                                 │
└─────────────────────────────────────────────────────────────────────┘
```

## 对象池管理

### 池结构

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Object Pool Architecture                    │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     pool_global                             │   │
│  │  - 全局对象池，预分配 ODEBUG_POOL_SIZE (1024) 个对象        │   │
│  │  - min_cnt: 最小保留数量 (256)                              │   │
│  │  - max_cnt: 最大数量 (1024)                                │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    pool_pcpu[] (per CPU)                    │   │
│  │  - 每 CPU 对象池，减少锁竞争                                │   │
│  │  - max_cnt: ODEBUG_POOL_PERCPU_SIZE (128)                   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     pool_to_free                            │   │
│  │  - 待释放对象池，通过 workqueue 异步释放                     │   │
│  │  - 限制释放频率：最大 10Hz，每次最多释放 1024 个对象         │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### 分配流程

```
pcpu_alloc()
    │
    ├─ 尝试从 per CPU 池分配
    │       │
    │       ├─ 成功 → 返回对象
    │       │
    │       └─ 失败 → 从全局池或待释放池补充 batch
    │
    └─ 仍失败 → 返回 NULL (触发 OOM)
```

### 释放流程

```
pcpu_free()
    │
    ├─ 将对象放回 per CPU 池
    │
    ├─ 检查 per CPU 池是否已满
    │       │
    │       └─ 已满 → 移动一个 batch 到全局池或待释放池
    │
    └─ 调度 workqueue 异步释放待释放池中的对象
```

### Batch 机制

```c
#define ODEBUG_BATCH_SIZE    16  /* 批量大小，必须是 2 的幂 */

struct debug_obj {
    struct hlist_node *batch_last;  /* batch 中最后一个节点 */
    ...
};
```

通过 batch 机制减少锁竞争和内存分配次数。

## 哈希表设计

```c
#define ODEBUG_HASH_BITS     14
#define ODEBUG_HASH_SIZE     (1 << ODEBUG_HASH_BITS)  /* 16384 */

static struct debug_bucket obj_hash[ODEBUG_HASH_SIZE];

static struct debug_bucket *get_bucket(unsigned long addr)
{
    unsigned long hash;
    hash = hash_long((addr >> ODEBUG_CHUNK_SHIFT), ODEBUG_HASH_BITS);
    return &obj_hash[hash];
}
```

使用对象地址的高位作为哈希索引，便于检查被释放的对象。

## 错误检测机制

### debug_print_object()

```c
static void debug_print_object(struct debug_obj *obj, char *msg)
{
    const struct debug_obj_descr *descr = obj->descr;
    
    if (limit < 5 && descr != descr_test) {
        void *hint = descr->debug_hint ? descr->debug_hint(obj->object) : NULL;
        limit++;
        WARN(1, KERN_ERR "ODEBUG: %s %s (active state %u) "
                 "object: %p object type: %s hint: %pS\n",
             msg, obj_states[obj->state], obj->astate,
             obj->object, descr->name, hint);
    }
    debug_objects_warnings++;
}
```

打印对象状态违规信息，限制最多打印 5 次。

### debug_object_fixup()

```c
static bool debug_object_fixup(bool (*fixup)(void *addr, enum debug_obj_state state),
                               void *addr, enum debug_obj_state state)
{
    if (fixup && fixup(addr, state)) {
        debug_objects_fixups++;
        return true;
    }
    return false;
}
```

调用类型特定的修复函数，尝试修复状态违规。

## 活跃子状态跟踪

```c
void debug_object_active_state(void *addr, const struct debug_obj_descr *descr,
                               unsigned int expect, unsigned int next)
{
    /* 检查当前 astate 是否等于 expect */
    /* 如果是，将 astate 更新为 next */
    /* 否则报告错误 */
}
```

用于跟踪对象内部的子状态转换，如信号量的 wait 计数。

## debugfs 接口

```bash
# 目录位置
/sys/kernel/debug/debug_objects/

# 可用文件
stats - 统计信息 (只读)
```

### stats 文件内容

```bash
max_chain     : 哈希桶中最长链表长度
max_checked   : 单次检查的最大对象数
warnings      : 警告次数
fixups        : 修复次数
pool_free     : 空闲对象数（全局+per CPU）
pool_pcp_free : per CPU 空闲对象数
pool_min_free : 最小空闲对象数
pool_used     : 已使用对象数
pool_max_used : 最大使用对象数
on_free_list  : 待释放对象数
objs_allocated: 已分配对象总数
objs_freed    : 已释放对象总数
```

## 编译配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_DEBUG_OBJECTS` | 启用对象调试 |
| `CONFIG_DEBUG_OBJECTS_ENABLE_DEFAULT` | 默认启用对象调试 |
| `CONFIG_DEBUG_OBJECTS_FREE` | 检查释放内存中是否包含活跃对象 |
| `CONFIG_DEBUG_OBJECTS_TIMERS` | 调试定时器对象 |
| `CONFIG_DEBUG_OBJECTS_WORK` | 调试工作队列对象 |
| `CONFIG_DEBUG_OBJECTS_RCU_HEAD` | 调试 RCU head 对象 |
| `CONFIG_DEBUG_OBJECTS_PERCPU_CACHE` | 调试 per CPU 缓存对象 |

## 内核参数

| 参数 | 说明 |
|------|------|
| `debug_objects` | 启用对象调试 |
| `no_debug_objects` | 禁用对象调试 |

## 性能影响

| 方面 | 影响 |
|------|------|
| **内存开销** | 每个追踪对象增加约 32-48 字节 |
| **CPU 开销** | 每次状态转换增加约 0.5-1 微秒 |
| **锁竞争** | 通过 per CPU 池和 batch 机制减少 |
| **内存分配** | 通过对象池复用减少 kmem_cache 分配 |

## 使用场景

1. **驱动开发**：在开发过程中检测对象生命周期错误
2. **内核开发**：检测内核子系统中的对象使用错误
3. **问题排查**：当系统出现不稳定时，启用 debug_objects 定位问题

## 代码位置

| 文件 | 说明 |
|------|------|
| `lib/debugobjects.c` | 对象调试核心实现 |
| `include/linux/debugobjects.h` | 对象调试头文件 |
| `kernel/locking/lockdep_debug.c` | lockdep 集成 |