# SLUB Debug

## 概述

SLUB Debug 是 Linux 内核 SLUB 内存分配器自带的调试选项，用于检测内存损坏、越界访问、双重释放等内存问题。SLUB Debug 通过在对象周围添加 red zone、填充 poison 值、记录分配/释放者信息等方式来检测内存错误。

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                       SLUB Debug Architecture                       │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                   SLUB Memory Layout                        │   │
│  │                                                             │   │
│  │  ┌─────────────────────────────────────────────────────┐   │   │
│  │  │            struct slab (页头)                        │   │   │
│  │  │  - freelist: 空闲对象链表                            │   │   │
│  │  │  - objects: 对象总数                                 │   │   │
│  │  │  - inuse: 已使用对象数                               │   │   │
│  │  │  - counters: 统计信息                                │   │   │
│  │  └─────────────────────────────────────────────────────┘   │   │
│  │                            │                                │   │
│  │                            ▼                                │   │
│  │  ┌─────────────────────────────────────────────────────┐   │   │
│  │  │            Red Zone (左侧)                           │   │   │
│  │  │  - 填充 POISON 值 (0xdeadbeef)                       │   │   │
│  │  │  - 检测左侧越界访问                                  │   │   │
│  │  └─────────────────────────────────────────────────────┘   │   │
│  │                            │                                │   │
│  │                            ▼                                │   │
│  │  ┌─────────────────────────────────────────────────────┐   │   │
│  │  │            用户对象数据                               │   │   │
│  │  │  - 实际分配给用户的内存区域                           │   │   │
│  │  └─────────────────────────────────────────────────────┘   │   │
│  │                            │                                │   │
│  │                            ▼                                │   │
│  │  ┌─────────────────────────────────────────────────────┐   │   │
│  │  │            Red Zone (右侧)                           │   │   │
│  │  │  - 填充 POISON 值 (0xdeadbeef)                       │   │   │
│  │  │  - 检测右侧越界访问                                  │   │   │
│  │  └─────────────────────────────────────────────────────┘   │   │
│  │                            │                                │   │
│  │                            ▼                                │   │
│  │  ┌─────────────────────────────────────────────────────┐   │   │
│  │  │            Object Extensions (可选)                   │   │   │
│  │  │  - ALLOC/CONSTRUCT/DESTRUCT/FREE 栈追踪              │   │   │
│  │  │  - 分配者信息 (SLAB_STORE_USER)                      │   │   │
│  │  └─────────────────────────────────────────────────────┘   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Debug Checks                             │   │
│  │                                                             │   │
│  │  • SLAB_RED_ZONE      - Red zone 检查                      │   │
│  │  • SLAB_POISON        - Poison 值检查                      │   │
│  │  • SLAB_STORE_USER    - 记录分配/释放者信息                 │   │
│  │  • SLAB_CONSISTENCY_CHECKS - 一致性检查                    │   │
│  │  • SLAB_TRACE          - 跟踪分配/释放流程                  │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 调试标志

| 标志 | 描述 |
|------|------|
| `SLAB_RED_ZONE` | 在对象周围添加 red zone，检测越界访问 |
| `SLAB_POISON` | 使用 poison 值填充空闲对象和 red zone |
| `SLAB_STORE_USER` | 记录对象分配者和释放者的栈追踪信息 |
| `SLAB_CONSISTENCY_CHECKS` | 启用一致性检查，验证 slab 状态 |
| `SLAB_TRACE` | 跟踪每个对象的分配和释放 |
| `SLAB_DEBUG_OBJECTS` | 启用对象调试（与 debug_objects 配合） |

默认调试标志：
```c
#define DEBUG_DEFAULT_FLAGS (SLAB_CONSISTENCY_CHECKS | SLAB_RED_ZONE | \
				SLAB_POISON | SLAB_STORE_USER)
```

## 核心数据结构

### kmem_cache_node

```c
struct kmem_cache_node {
    spinlock_t list_lock;
    unsigned long nr_partial;
    struct list_head partial;
#ifdef CONFIG_SLUB_DEBUG
    atomic_long_t nr_slabs;       /* slab 数量 */
    atomic_long_t total_objects;  /* 总对象数 */
    struct list_head full;        /* 满 slab 链表 */
#endif
    struct node_barn *barn;
};
```

该结构在调试模式下增加了额外的统计信息和满 slab 链表。

### slab_flags_t

```c
typedef unsigned int slab_flags_t;

#define SLAB_DEBUG_FLAGS (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER | \
			  SLAB_CONSISTENCY_CHECKS | SLAB_TRACE | \
			  SLAB_DEBUG_OBJECTS)
```

调试标志类型定义。

## 工作流程

### 对象分配流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                   Object Allocation Flow                           │
│                                                                     │
│  kmem_cache_alloc() / kmalloc()                                     │
│            │                                                        │
│            ▼                                                        │
│  ___slab_alloc()                                                    │
│            │                                                        │
│            ├── SLAB_CONSISTENCY_CHECKS:                             │
│            │   • 检查 slab 状态                                     │
│            │   • 检查 freelist 完整性                               │
│            │                                                        │
│            ├── SLAB_POISON:                                         │
│            │   • 清除对象中的 poison 值                             │
│            │                                                        │
│            ├── SLAB_STORE_USER:                                     │
│            │   • 记录分配者的栈追踪                                 │
│            │                                                        │
│            └── SLAB_RED_ZONE:                                      │
│                • 确保 red zone 完整                                 │
│                      │                                              │
│                      ▼                                              │
│           返回对象指针 (跳过左侧 red zone)                           │
└─────────────────────────────────────────────────────────────────────┘
```

### 对象释放流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Object Free Flow                                │
│                                                                     │
│  kmem_cache_free() / kfree()                                        │
│            │                                                        │
│            ▼                                                        │
│  ___slab_free()                                                     │
│            │                                                        │
│            ├── SLAB_CONSISTENCY_CHECKS:                             │
│            │   • 检查对象是否属于正确的 slab                        │
│            │   • 检查 slab 状态                                     │
│            │                                                        │
│            ├── SLAB_RED_ZONE:                                       │
│            │   • 检查左侧 red zone 是否被破坏                       │
│            │   • 检查右侧 red zone 是否被破坏                       │
│            │   • 破坏则报告错误                                     │
│            │                                                        │
│            ├── SLAB_POISON:                                         │
│            │   • 用 poison 值填充对象                               │
│            │                                                        │
│            └── SLAB_STORE_USER:                                     │
│                • 记录释放者的栈追踪                                 │
│                      │                                              │
│                      ▼                                              │
│           对象放回 freelist                                         │
└─────────────────────────────────────────────────────────────────────┘
```

## 关键函数

### kmem_cache_debug()

```c
static inline bool kmem_cache_debug(struct kmem_cache *s)
{
    return kmem_cache_debug_flags(s, SLAB_DEBUG_FLAGS);
}
```

检查缓存是否启用了任何调试标志。

### kmem_cache_debug_flags()

```c
static inline bool kmem_cache_debug_flags(struct kmem_cache *s, slab_flags_t flags)
{
    return (s->flags & flags) != 0;
}
```

检查缓存是否启用了指定的调试标志。

### fixup_red_left()

```c
void *fixup_red_left(struct kmem_cache *s, void *p)
{
    if (kmem_cache_debug_flags(s, SLAB_RED_ZONE))
        p += s->red_left_pad;
    return p;
}
```

跳过左侧 red zone，返回用户可见的对象指针。

### restore_red_left()

```c
static inline void *restore_red_left(struct kmem_cache *s, void *p)
{
    if (s->flags & SLAB_RED_ZONE)
        p -= s->red_left_pad;
    return p;
}
```

恢复左侧 red zone，返回实际的对象起始地址。

### validate_slab_ptr()

```c
static inline bool validate_slab_ptr(struct slab *slab)
{
    return PageSlab(slab_page(slab));
}
```

验证 slab 指针是否有效。

### __fill_map()

```c
static void __fill_map(unsigned long *obj_map, struct kmem_cache *s,
                       struct slab *slab)
{
    void *addr = slab_address(slab);
    void *p;

    bitmap_zero(obj_map, slab->objects);

    for (p = slab->freelist; p; p = get_freepointer(s, p))
        set_bit(__obj_to_index(s, addr, p), obj_map);
}
```

填充对象映射，标记空闲对象。

## 错误检测

### Red Zone 破坏检测

```c
/* 在对象释放时检查 red zone */
if (kmem_cache_debug_flags(s, SLAB_RED_ZONE)) {
    /* 检查左侧 red zone */
    if (check_bytes(restore_red_left(s, obj), s->red_left_pad, POISON_INUSE))
        slab_error(s, "Red zone violation at start of object");
    
    /* 检查右侧 red zone */
    if (check_bytes(obj + s->object_size, s->red_right_pad, POISON_INUSE))
        slab_error(s, "Red zone violation at end of object");
}
```

### Poison 值检测

```c
/* 在对象分配时清除 poison */
if (kmem_cache_debug_flags(s, SLAB_POISON)) {
    memset(obj, 0, size_from_object(s));
}

/* 在对象释放时填充 poison */
if (kmem_cache_debug_flags(s, SLAB_POISON)) {
    memset(obj, POISON_FREE, size_from_object(s));
}
```

### 一致性检查

```c
/* 检查 slab 状态 */
if (kmem_cache_debug_flags(s, SLAB_CONSISTENCY_CHECKS)) {
    check_slab(s, slab);
}
```

## Poison 值定义

| 值 | 定义 | 用途 |
|----|------|------|
| `POISON_INUSE` | `0x6b6b6b6b` | 已分配对象中的 poison |
| `POISON_FREE` | `0x5a5a5a5a` | 空闲对象中的 poison |
| `POISON_END` | `0xdeadbeef` | red zone 中的 poison |

## 编译配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_SLUB_DEBUG` | 启用 SLUB 调试支持 |
| `CONFIG_SLUB_DEBUG_ON` | 默认启用调试（所有缓存） |
| `CONFIG_SLUB_STATS` | 启用 SLUB 统计信息 |
| `CONFIG_SLUB_MEMCG_SYSFS_ON` | 在 sysfs 中显示 memcg 信息 |

## 运行时配置

可以通过内核参数或 sysfs 接口配置 SLUB Debug：

### 内核参数

```bash
slub_debug=FLAGS[,cache]
```

示例：
```bash
slub_debug=FPZ		# 启用 POISON、RED_ZONE、STORE_USER
slub_debug=FZP,kmalloc-64	# 仅对 kmalloc-64 启用调试
```

### sysfs 接口

```bash
# 查看缓存信息
cat /sys/kernel/slab/<cache>/<attribute>

# 启用/禁用调试
echo "on" > /sys/kernel/slab/<cache>/debug
echo "off" > /sys/kernel/slab/<cache>/debug
```

可用属性：
- `red_left_pad` - 左侧 red zone 大小
- `red_right_pad` - 右侧 red zone 大小
- `poison` - 是否启用 poison
- `store_user` - 是否记录用户信息
- `trace` - 是否启用追踪

## debugfs 接口

当同时启用 `CONFIG_DEBUG_FS` 和 `CONFIG_SLUB_DEBUG` 时，可以通过 debugfs 查看详细信息：

```bash
# 查看所有缓存信息
cat /sys/kernel/debug/slab/<cache>/objects

# 查看特定 slab 的对象状态
cat /sys/kernel/debug/slab/<cache>/slabs
```

## 性能影响

| 方面 | 影响 |
|------|------|
| **内存开销** | 增加 10-20%，取决于调试标志 |
| **CPU 开销** | 增加 20-50%，取决于调试标志 |
| **启动时间** | 影响较小 |

## 使用场景

1. **开发阶段**：在开发过程中启用 SLUB Debug，检测内存错误
2. **测试阶段**：在 CI/CD 流程中运行测试，确保没有内存错误
3. **问题排查**：当系统出现不稳定时，启用 SLUB Debug 复现问题

## 代码位置

| 文件 | 说明 |
|------|------|
| `mm/slub.c` | SLUB 内存分配器核心实现 |
| `mm/slab.h` | SLUB 数据结构定义 |
| `mm/slab_common.c` | slab 通用功能 |