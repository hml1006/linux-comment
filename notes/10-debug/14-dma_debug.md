# DMA API Debug

## 概述

DMA API Debug 是 Linux 内核提供的 DMA 映射调试工具，用于检测驱动程序在使用 DMA API 时的错误。它可以追踪 DMA 映射/解除映射操作，检测内存泄漏、栈内存映射、非法区域映射、重叠映射等问题。

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                      DMA API Debug Architecture                     │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                      DMA API 调用                           │   │
│  │  dma_map_single() / dma_map_sg() / dma_alloc_coherent()     │   │
│  │  dma_unmap_single() / dma_unmap_sg() / dma_free_coherent()  │   │
│  │  dma_sync_single_for_cpu() / dma_sync_single_for_device()   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    debug_dma_*() 包装函数                    │   │
│  │  - 检测无效内存地址                                           │   │
│  │  - 检测栈内存映射                                             │   │
│  │  - 检测非法区域映射 (text/rodata)                            │   │
│  │  - 记录映射信息到哈希表                                       │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    核心数据结构                               │   │
│  │  • dma_entry_hash[]   - 哈希表存储映射条目                    │   │
│  │  • free_entries       - 空闲条目链表                         │   │
│  │  • dma_active_cacheline - radix tree 追踪活跃cacheline       │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    错误检测机制                               │   │
│  │  • 内存泄漏检测 (设备解绑时检查)                              │   │
│  │  • 重叠映射检测 (cacheline 追踪)                             │   │
│  │  • 映射错误检查缺失检测                                       │   │
│  │  • 方向/大小/类型不匹配检测                                   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                      │
│                              ▼                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    debugfs 接口                              │   │
│  │  /sys/kernel/debug/dma-api/                                 │   │
│  │  - disabled          - 全局禁用标志                          │   │
│  │  - error_count       - 错误计数                              │   │
│  │  - all_errors        - 是否显示所有错误                       │   │
│  │  - num_errors        - 显示错误数量                          │   │
│  │  - driver_filter     - 驱动过滤器                            │   │
│  │  - dump              - 转储所有映射                          │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 核心数据结构

### dma_debug_entry

```c
struct dma_debug_entry {
    struct list_head list;
    struct device    *dev;          /* 设备指针 */
    u64              dev_addr;      /* DMA 地址 */
    u64              size;          /* 映射大小 */
    int              type;          /* 映射类型 */
    int              direction;     /* DMA 方向 */
    int              sg_call_ents;  /* sg 映射的条目数 */
    int              sg_mapped_ents;/* 实际映射的 sg 条目数 */
    phys_addr_t      paddr;         /* 物理地址 */
    enum map_err_types map_err_type;/* 映射错误检查状态 */
    bool             is_cache_clean;/* 是否缓存干净 */
#ifdef CONFIG_STACKTRACE
    unsigned int     stack_len;     /* 栈追踪长度 */
    unsigned long    stack_entries[DMA_DEBUG_STACKTRACE_ENTRIES];
#endif
} ____cacheline_aligned_in_smp;
```

该结构用于追踪每个 DMA 映射的详细信息。

### hash_bucket

```c
struct hash_bucket {
    struct list_head list;
    spinlock_t lock;
};

static struct hash_bucket dma_entry_hash[HASH_SIZE];
```

哈希桶结构，用于存储和快速查找 DMA 映射条目。

### 映射类型

| 类型 | 值 | 描述 |
|------|-----|------|
| `dma_debug_single` | 0 | 单页映射 |
| `dma_debug_sg` | 1 | scatter-gather 映射 |
| `dma_debug_coherent` | 2 | 一致性分配 |
| `dma_debug_noncoherent` | 3 | 非一致性分配 |
| `dma_debug_phy` | 4 | 物理地址映射 |

### DMA 方向

| 方向 | 描述 |
|------|------|
| `DMA_BIDIRECTIONAL` | 双向传输 |
| `DMA_TO_DEVICE` | 从 CPU 到设备 |
| `DMA_FROM_DEVICE` | 从设备到 CPU |
| `DMA_NONE` | 无方向 |

### 映射错误类型

| 类型 | 值 | 描述 |
|------|-----|------|
| `MAP_ERR_CHECK_NOT_APPLICABLE` | 0 | 不适用 |
| `MAP_ERR_NOT_CHECKED` | 1 | 未检查 |
| `MAP_ERR_CHECKED` | 2 | 已检查 |

## 工作流程

### DMA 映射流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                     DMA Mapping Flow                               │
│                                                                     │
│  dma_map_single() / dma_map_sg() / dma_alloc_coherent()             │
│            │                                                        │
│            ▼                                                        │
│  debug_dma_map_*()                                                  │
│            │                                                        │
│            ├── 检查是否禁用调试                                      │
│            ├── 检查映射错误 (dma_mapping_error)                      │
│            ├── 分配 dma_debug_entry                                 │
│            ├── 记录映射信息 (设备、地址、大小、方向、类型)            │
│            ├── 检查栈内存映射                                        │
│            ├── 检查非法区域映射 (text/rodata)                        │
│            ├── 添加到哈希表                                          │
│            └── 插入 cacheline radix tree                            │
│                      │                                              │
│                      ▼                                              │
│           返回 DMA 地址                                              │
└─────────────────────────────────────────────────────────────────────┘
```

### DMA 解除映射流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                     DMA Unmapping Flow                             │
│                                                                     │
│  dma_unmap_single() / dma_unmap_sg() / dma_free_coherent()          │
│            │                                                        │
│            ▼                                                        │
│  debug_dma_unmap_*()                                                │
│            │                                                        │
│            ▼                                                        │
│  check_unmap()                                                      │
│            │                                                        │
│            ├── 在哈希表中查找映射条目                                │
│            ├── 检查条目是否存在                                      │
│            ├── 检查大小是否匹配                                      │
│            ├── 检查类型是否匹配                                      │
│            ├── 检查方向是否匹配                                      │
│            ├── 检查物理地址是否匹配 (coherent)                       │
│            ├── 检查 sg 条目数是否匹配                                │
│            ├── 检查映射错误是否被检查                                │
│            ├── 从哈希表删除条目                                      │
│            └── 释放 dma_debug_entry                                 │
└─────────────────────────────────────────────────────────────────────┘
```

### DMA 同步流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                     DMA Sync Flow                                  │
│                                                                     │
│  dma_sync_single_for_cpu() / dma_sync_single_for_device()           │
│            │                                                        │
│            ▼                                                        │
│  check_sync()                                                       │
│            │                                                        │
│            ├── 在哈希表中查找包含该地址的映射                         │
│            ├── 检查同步范围是否在分配范围内                          │
│            ├── 检查方向是否匹配                                      │
│            ├── 检查同步方向是否正确 (cpu/device)                     │
│            └── 检查 sg 条目数是否匹配                                │
└─────────────────────────────────────────────────────────────────────┘
```

## 错误检测机制

### 内存泄漏检测

```c
static int dma_debug_device_change(struct notifier_block *nb, 
                                   unsigned long action, void *data)
{
    struct device *dev = data;
    struct dma_debug_entry *entry;
    int count;

    switch (action) {
    case BUS_NOTIFY_UNBOUND_DRIVER:
        count = device_dma_allocations(dev, &entry);
        if (count == 0)
            break;
        err_printk(dev, entry, "device driver has pending "
                "DMA allocations while released from device "
                "[count=%d]\n", count);
        break;
    }
    return 0;
}
```

当设备驱动解绑时，检查是否还有未释放的 DMA 映射。

### 栈内存映射检测

```c
static void check_for_stack(struct device *dev, phys_addr_t phys)
{
    void *addr;
    struct vm_struct *stack_vm_area = task_stack_vm_area(current);

    if (!stack_vm_area) {
        if (PhysHighMem(phys))
            return;
        addr = phys_to_virt(phys);
        if (object_is_on_stack(addr))
            err_printk(dev, NULL, 
                "device driver maps memory from stack [addr=%p]\n", addr);
    } else {
        /* 处理 vmalloc 栈 */
        ...
    }
}
```

检测驱动程序是否尝试映射栈内存。

### 非法区域映射检测

```c
static void check_for_illegal_area(struct device *dev, void *addr, 
                                    unsigned long len)
{
    if (memory_intersects(_stext, _etext, addr, len) ||
        memory_intersects(__start_rodata, __end_rodata, addr, len))
        err_printk(dev, NULL, 
            "device driver maps memory from kernel text or rodata "
            "[addr=%p] [len=%lu]\n", addr, len);
}
```

检测驱动程序是否尝试映射内核代码段或只读数据段。

### 重叠映射检测

```c
static void active_cacheline_inc_overlap(phys_addr_t cln, bool is_cache_clean)
{
    int overlap = active_cacheline_read_overlap(cln);

    overlap = active_cacheline_set_overlap(cln, ++overlap);

    WARN_ONCE(!is_cache_clean && overlap > ACTIVE_CACHELINE_MAX_OVERLAP,
        pr_fmt("exceeded %d overlapping mappings of cacheline %pa\n"),
        ACTIVE_CACHELINE_MAX_OVERLAP, &cln);
}
```

通过 radix tree 追踪每个 cacheline 的映射次数，检测重叠映射。

### 映射错误检查缺失检测

```c
if (entry->map_err_type == MAP_ERR_NOT_CHECKED) {
    err_printk(ref->dev, entry,
        "device driver failed to check map error"
        "[device address=0x%016llx] [size=%llu bytes] "
        "[mapped as %s]",
        ref->dev_addr, ref->size,
        type2name[entry->type]);
}
```

检测驱动程序是否检查了 `dma_mapping_error()` 的返回值。

## 哈希表设计

### 哈希函数

```c
#define HASH_SIZE       16384ULL
#define HASH_FN_SHIFT   13
#define HASH_FN_MASK    (HASH_SIZE - 1)

static int hash_fn(struct dma_debug_entry *entry)
{
    return (entry->dev_addr >> HASH_FN_SHIFT) & HASH_FN_MASK;
}
```

使用 DMA 地址的高位作为哈希索引。

### 查找算法

```c
static struct dma_debug_entry *__hash_bucket_find(struct hash_bucket *bucket,
                                                 struct dma_debug_entry *ref,
                                                 match_fn match)
{
    struct dma_debug_entry *entry, *ret = NULL;
    int matches = 0, match_lvl, last_lvl = -1;

    list_for_each_entry(entry, &bucket->list, list) {
        if (!match(ref, entry))
            continue;

        matches += 1;
        match_lvl = 0;
        entry->size         == ref->size         ? ++match_lvl : 0;
        entry->type         == ref->type         ? ++match_lvl : 0;
        entry->direction    == ref->direction    ? ++match_lvl : 0;
        entry->sg_call_ents == ref->sg_call_ents ? ++match_lvl : 0;

        if (match_lvl == 4) {
            return entry;  /* 完美匹配 */
        } else if (match_lvl > last_lvl) {
            last_lvl = match_lvl;
            ret      = entry;
        }
    }

    ret = (matches == 1) ? ret : NULL;  /* 只有一个匹配才返回 */

    return ret;
}
```

实现了最佳匹配算法，处理同一物理地址多次映射的情况。

## debugfs 接口

```bash
# 目录位置
/sys/kernel/debug/dma-api/

# 可用文件
disabled          - 全局禁用标志 (只读)
error_count       - 错误计数 (只读)
all_errors        - 是否显示所有错误 (读写)
num_errors        - 显示错误数量 (读写)
num_free_entries  - 空闲条目数 (只读)
min_free_entries  - 最小空闲条目数 (只读)
nr_total_entries  - 总条目数 (只读)
driver_filter     - 驱动过滤器 (读写)
dump              - 转储所有映射 (只读)
```

### 驱动过滤器

```bash
# 启用驱动过滤器
echo "my_driver" > /sys/kernel/debug/dma-api/driver_filter

# 禁用驱动过滤器
echo "" > /sys/kernel/debug/dma-api/driver_filter
```

只显示指定驱动的错误信息。

### 转储映射

```bash
cat /sys/kernel/debug/dma-api/dump
```

显示所有当前的 DMA 映射信息。

## 编译配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_DMA_API_DEBUG` | 启用 DMA API 调试 |
| `CONFIG_STACKTRACE` | 启用栈追踪记录 |

## 内核参数

| 参数 | 说明 |
|------|------|
| `dma_debug=off` | 禁用 DMA 调试 |
| `dma_debug_entries=N` | 设置预分配的调试条目数 |
| `dma_debug_driver=NAME` | 设置驱动过滤器 |

## 性能影响

| 方面 | 影响 |
|------|------|
| **内存开销** | 每个映射增加约 80 字节 |
| **CPU 开销** | 每次映射/解除映射增加约 1-2 微秒 |
| **启动时间** | 预分配条目时略有增加 |

## 使用场景

1. **驱动开发**：在开发过程中启用 DMA Debug，检测 DMA API 使用错误
2. **问题排查**：当系统出现 DMA 相关问题时，启用调试定位问题
3. **CI/CD 测试**：在测试流程中运行，确保驱动没有 DMA API 错误

## 代码位置

| 文件 | 说明 |
|------|------|
| `kernel/dma/debug.c` | DMA 调试核心实现 |
| `kernel/dma/debug.h` | DMA 调试头文件 |
| `kernel/dma/mapping.c` | DMA 映射接口 |