# KASAN (Kernel Address SANitizer)

## 概述

KASAN 是 Linux 内核的内存错误检测工具，用于检测内核中的越界访问（out-of-bounds）和释放后使用（use-after-free）等内存错误。KASAN 通过在编译时插桩内存访问操作，在运行时检查内存访问的合法性。

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                         KASAN Architecture                          │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                        Compiler Instrumentation              │   │
│  │                                                             │   │
│  │  • 编译时在每个内存访问前插入检查代码                        │   │
│  │  • __asan_loadN() / __asan_storeN() 系列函数               │   │
│  │  • 对栈变量和全局变量添加 redzone                           │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                        Runtime Check                         │   │
│  │                                                             │   │
│  │  • kasan_check_range() - 检查内存访问合法性                  │   │
│  │  • kasan_byte_accessible() - 检查单字节是否可访问            │   │
│  │  • 根据模式不同，检查方式不同：                               │   │
│  │    - Generic: 检查 shadow memory                             │   │
│  │    - SW Tags: 检查软件标签                                   │   │
│  │    - HW Tags: 检查硬件标签 (MTE)                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                      Error Reporting                        │   │
│  │                                                             │   │
│  │  • kasan_report() - 生成详细错误报告                         │   │
│  │  • 显示访问地址、大小、类型（读/写）                         │   │
│  │  • 显示分配和释放的栈追踪                                    │   │
│  │  • 显示内存状态元数据                                       │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                      Quarantine                             │   │
│  │                                                             │   │
│  │  • 释放后的对象放入隔离区，延迟真正释放                       │   │
│  │  • 提高 use-after-free 检测概率                             │   │
│  │  • per-cpu 队列 + 全局队列                                   │   │
│  │  • 基于内存压力动态调整隔离区大小                            │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 支持模式

| 模式 | 配置项 | 描述 |
|------|--------|------|
| **Generic** | `CONFIG_KASAN_GENERIC` | 使用 shadow memory，每 8 字节内存对应 1 字节 shadow |
| **SW Tags** | `CONFIG_KASAN_SW_TAGS` | 软件标签模式，使用指针的高 8 位作为标签 |
| **HW Tags** | `CONFIG_KASAN_HW_TAGS` | 硬件标签模式，基于 ARM MTE（Memory Tagging Extension） |

## 核心数据结构

### kasan_track

```c
struct kasan_track {
    u32 pid;                              /* 进程 ID */
    depot_stack_handle_t stack;           /* 栈追踪句柄 */
#ifdef CONFIG_KASAN_EXTRA_INFO
    u64 cpu:20;                           /* CPU 编号 */
    u64 timestamp:44;                     /* 时间戳 */
#endif
};
```

该结构用于保存内存分配或释放时的上下文信息，包括进程 ID 和栈追踪。

### kasan_report_info

```c
struct kasan_report_info {
    enum kasan_report_type type;          /* 报告类型 */
    const void *access_addr;              /* 访问地址 */
    size_t access_size;                   /* 访问大小 */
    bool is_write;                        /* 是否为写操作 */
    unsigned long ip;                     /* 指令指针 */
    const void *first_bad_addr;           /* 第一个错误地址 */
    struct kmem_cache *cache;             /* 所属缓存 */
    void *object;                         /* 所属对象 */
    size_t alloc_size;                    /* 分配大小 */
    const char *bug_type;                 /* 错误类型 */
    struct kasan_track alloc_track;       /* 分配追踪 */
    struct kasan_track free_track;        /* 释放追踪 */
};
```

该结构用于存储错误报告的详细信息。

### kasan_alloc_meta / kasan_free_meta

```c
struct kasan_alloc_meta {
    struct kasan_track alloc_track;       /* 分配追踪信息 */
    depot_stack_handle_t aux_stack[2];    /* 辅助栈追踪 */
};

struct kasan_free_meta {
    struct qlist_node quarantine_link;    /* 隔离区链表节点 */
    struct kasan_track free_track;        /* 释放追踪信息 */
};
```

这些结构存储每个 slab 对象的元数据，用于追踪分配和释放信息。

### qlist_head / qlist_node

```c
struct qlist_node {
    struct qlist_node *next;              /* 下一个节点 */
};

struct qlist_head {
    struct qlist_node *head;              /* 链表头 */
    struct qlist_node *tail;              /* 链表尾 */
    size_t bytes;                         /* 总字节数 */
    bool offline;                         /* 是否离线 */
};
```

这些结构用于实现隔离区队列。

## Shadow Memory 机制

### Generic 模式

在 Generic 模式下，KASAN 使用 shadow memory 来跟踪内存状态：

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Shadow Memory Layout                         │
│                                                                     │
│  应用内存地址:  0xffff888000000000 ──┐                             │
│                0xffff888000000001    │                             │
│                ...                   │                             │
│                0xffff888000000007    │                             │
│                0xffff888000000008 ──┼── 映射到 ──>  Shadow: 0x01  │
│                ...                   │                             │
│                                      │                             │
│  Shadow 内存:  每个字节对应 8 字节应用内存                          │
│                0x00 = 全部可访问                                   │
│                0x01-0x07 = 部分可访问（最后几个字节）               │
│                0xfa = 已释放但保留元数据                            │
│                0xfb = 已释放                                      │
│                0xfc = slab redzone                                │
│                0xfd = 页面 redzone                                │
│                0xfe = 大分配 redzone                              │
│                0xff = 完全不可访问                                 │
└─────────────────────────────────────────────────────────────────────┘
```

### Tag-Based 模式

在 SW Tags 和 HW Tags 模式下，使用指针标签：

```
┌─────────────────────────────────────────────────────────────────────┐
│                      Tag-Based Memory Layout                        │
│                                                                     │
│  指针格式:  [tag: 8 bits][address: 56 bits]                         │
│                                                                     │
│  访问检查:                                                          │
│    1. 获取指针中的 tag (get_tag())                                  │
│    2. 获取内存中的 tag (hw_get_mem_tag() 或 shadow lookup)          │
│    3. 比较两个 tag 是否匹配                                         │
│    4. 不匹配则报告错误                                              │
│                                                                     │
│  Tag 值:                                                            │
│    KASAN_TAG_KERNEL (0x00) = 内核空间，不检查                       │
│    KASAN_TAG_INVALID (0xff) = 无效标签，禁止访问                     │
│    其他值 = 随机标签，用于检测 use-after-free                        │
└─────────────────────────────────────────────────────────────────────┘
```

## 工作流程

### 内存分配

```
┌─────────────────────────────────────────────────────────────────────┐
│                      Memory Allocation Flow                         │
│                                                                     │
│  kmalloc() / kmem_cache_alloc()                                     │
│            │                                                        │
│            ▼                                                        │
│  __kasan_slab_alloc()                                               │
│            │                                                        │
│            ├── 分配隔离区空间（如果需要）                             │
│            ├── 生成随机标签 (kasan_random_tag())                     │
│            ├── 设置对象标签 (set_tag())                              │
│            ├── 取消对象 poison (kasan_unpoison())                    │
│            └── 保存分配信息 (kasan_save_alloc_info())                │
│                      │                                              │
│                      ▼                                              │
│           返回带标签的指针                                           │
└─────────────────────────────────────────────────────────────────────┘
```

### 内存释放

```
┌─────────────────────────────────────────────────────────────────────┐
│                      Memory Free Flow                               │
│                                                                     │
│  kfree() / kmem_cache_free()                                        │
│            │                                                        │
│            ▼                                                        │
│  __kasan_slab_pre_free()                                            │
│            │                                                        │
│            ├── 检查对象是否属于正确的 slab                           │
│            └── 检查对象是否已经被释放                                │
│                      │                                              │
│                      ▼                                              │
│  __kasan_slab_free()                                                │
│            │                                                        │
│            ├── 标记对象为 poison (kasan_poison())                    │
│            ├── 保存释放信息 (kasan_save_free_info())                 │
│            └── 放入隔离区 (kasan_quarantine_put())                  │
│                      │                                              │
│                      ▼                                              │
│           对象延迟真正释放                                           │
└─────────────────────────────────────────────────────────────────────┘
```

### 内存访问检查

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Memory Access Check Flow                         │
│                                                                     │
│  内存访问指令 (load/store)                                           │
│            │                                                        │
│            ▼                                                        │
│  __asan_loadN() / __asan_storeN() (编译器插入)                       │
│            │                                                        │
│            ▼                                                        │
│  kasan_check_range()                                                │
│            │                                                        │
│            ├── 获取 shadow byte 或 tag                              │
│            ├── 检查访问是否合法                                      │
│            └── 非法访问则调用 kasan_report()                         │
│                      │                                              │
│                      ▼                                              │
│           继续正常执行或报告错误                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## 关键函数

### kasan_check_range()

```c
bool kasan_check_range(const void *addr, size_t size, bool write,
                       unsigned long ret_ip);
```

检查内存访问范围是否合法，返回 `true` 表示访问有效，`false` 表示访问无效。

### kasan_report()

```c
bool kasan_report(const void *addr, size_t size, bool is_write,
                  unsigned long ip);
```

生成详细的错误报告，包括：
- 错误类型和位置
- 访问地址、大小和类型
- 分配和释放的栈追踪
- 内存状态元数据

### __kasan_slab_alloc()

```c
void *__kasan_slab_alloc(struct kmem_cache *cache, void *object,
                         gfp_t flags, bool init);
```

slab 分配的 KASAN 钩子函数，负责：
- 生成随机标签
- 取消对象的 poison 标记
- 保存分配信息

### __kasan_slab_free()

```c
bool __kasan_slab_free(struct kmem_cache *cache, void *object,
                       bool init, bool still_accessible, bool no_quarantine);
```

slab 释放的 KASAN 钩子函数，负责：
- 标记对象为 poison
- 保存释放信息
- 将对象放入隔离区

### kasan_quarantine_put()

```c
bool kasan_quarantine_put(struct kmem_cache *cache, void *object);
```

将释放的对象放入隔离区，延迟真正释放以提高 use-after-free 检测概率。

### kasan_quarantine_reduce()

```c
void kasan_quarantine_reduce(void);
```

当隔离区超过最大限制时，释放部分隔离的对象。

## 错误报告格式

KASAN 生成的错误报告包含以下信息：

```
BUG: KASAN: slab-use-after-free in some_function+0x123/0x456
Write of size 8 at addr ffff888123456788 by task myprog/1234

Allocated by task 1234:
stack_trace_save+0x10/0x20
__kasan_slab_alloc+0x45/0x60
kmem_cache_alloc+0x23/0x40
some_function+0x56/0x80

Freed by task 1234:
stack_trace_save+0x10/0x20
__kasan_slab_free+0x34/0x50
kmem_cache_free+0x12/0x30
another_function+0x78/0xa0

The buggy address belongs to the object at ffff888123456700
 which belongs to the cache kmalloc-64 of size 64
The buggy address is located 136 bytes to the right of
 freed 64-byte region [ffff888123456700, ffff888123456740)

Memory state around the buggy address:
 ffff888123456780: fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb
 ffff888123456790: fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb
>ffff8881234567a0: fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb
 ffff8881234567b0: fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb
 ffff8881234567c0: fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb fb
```

## 编译配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_KASAN` | 启用 KASAN 支持 |
| `CONFIG_KASAN_GENERIC` | 启用 Generic 模式 |
| `CONFIG_KASAN_SW_TAGS` | 启用软件标签模式 |
| `CONFIG_KASAN_HW_TAGS` | 启用硬件标签模式 |
| `CONFIG_KASAN_VMALLOC` | 启用 vmalloc 区域检测 |
| `CONFIG_KASAN_STACK` | 启用栈内存检测 |
| `CONFIG_KASAN_EXTRA_INFO` | 启用额外信息（CPU、时间戳） |
| `CONFIG_KASAN_KUNIT_TEST` | 启用 KUnit 测试 |

## 内核参数

| 参数 | 说明 |
|------|------|
| `kasan.fault=report` | 检测到错误时仅报告（默认） |
| `kasan.fault=panic` | 检测到错误时触发 panic |
| `kasan.fault=panic_on_write` | 检测到写错误时触发 panic |
| `kasan_multi_shot` | 允许报告多个错误 |

## 性能影响

| 方面 | 影响 |
|------|------|
| **内存开销** | Generic 模式约增加 1/8 内存，Tag 模式约增加 1/16 |
| **CPU 开销** | 约 2x-3x 性能下降 |
| **启动时间** | 显著增加，需要初始化 shadow memory |

## 使用场景

1. **开发阶段**：在开发过程中启用 KASAN，检测内存错误
2. **测试阶段**：在 CI/CD 流程中运行测试，确保没有内存错误
3. **问题排查**：当系统出现不稳定时，启用 KASAN 复现问题

## 代码位置

| 文件 | 说明 |
|------|------|
| `mm/kasan/kasan.h` | KASAN 内部头文件 |
| `mm/kasan/init.c` | Shadow memory 初始化 |
| `mm/kasan/common.c` | 通用功能实现 |
| `mm/kasan/report.c` | 错误报告 |
| `mm/kasan/quarantine.c` | 隔离区管理 |
| `mm/kasan/generic.c` | Generic 模式实现 |
| `mm/kasan/sw_tags.c` | 软件标签模式实现 |
| `mm/kasan/hw_tags.c` | 硬件标签模式实现 |
| `mm/kasan/shadow.c` | Shadow memory 操作 |
| `mm/kasan/tags.c` | 标签操作 |