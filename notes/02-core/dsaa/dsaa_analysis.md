# Linux 内核数据结构和算法分析

## 目录

1. [概述](#1-概述)
2. [列表结构](#2-列表结构)
   - 2.1 [双向循环链表 (list_head)](#21-双向循环链表-list_head)
   - 2.2 [哈希链表 (hlist_head)](#22-哈希链表-hlist_head)
3. [树结构](#3-树结构)
   - 3.1 [红黑树 (rb_node)](#31-红黑树-rb_node)
   - 3.2 [基数树 (Radix Tree)](#32-基数树-radix-tree)
   - 3.3 [Maple Tree](#33-maple-tree)
   - 3.4 [B树变体](#34-b树变体)
4. [哈希表](#4-哈希表)
5. [堆与优先队列](#5-堆与优先队列)
   - 5.1 [实时调度优先级队列](#51-实时调度优先级队列)
   - 5.2 [定时器时间轮](#52-定时器时间轮)
6. [查找与索引结构](#6-查找与索引结构)
   - 6.1 [位图 (bitmap)](#61-位图-bitmap)
   - 6.2 [Trie树（前缀树）](#62-trie树前缀树)
7. [映射与关联结构](#7-映射与关联结构)
   - 7.1 [IDR](#71-idr)
   - 7.2 [XArray](#72-xarray)
   - 7.3 [eBPF Maps](#73-ebpf-maps)
8. [队列与缓冲区](#8-队列与缓冲区)
   - 8.1 [环形缓冲区 (kfifo)](#81-环形缓冲区-kfifo)
   - 8.2 [工作队列](#82-工作队列)
9. [伙伴系统 (Buddy System)](#9-伙伴系统-buddy-system)
10. [引用计数 (kref)](#10-引用计数-kref)
11. [核心思想：权衡与优化](#11-核心思想权衡与优化)
12. [附录：关键文件索引](#12-附录关键文件索引)

---

## 1. 概述

Linux 内核作为一个大型系统软件，在其各个子系统中广泛使用了多种经典的数据结构和算法。这些数据结构经过精心设计，满足了内核在性能、内存占用和实时性等方面的严格要求。

内核设计数据结构时的核心权衡是**时间和空间的平衡**：

- **链表**：遍历慢，但插入/删除快（O(1)），适合动态变化频繁的场景
- **哈希表**：查找/插入/删除近乎 O(1)，但需要处理哈希冲突
- **树结构**：操作 O(log n)，且有顺序性优势
- **位图**：空间极小，适合表示大量"有/无"状态

本文档按**算法类别**对内核中的核心数据结构和算法进行系统性分析，涵盖其实现原理、核心数据结构定义、关键操作函数以及在内核中的典型应用场景。

---

## 2. 列表结构

### 2.1 双向循环链表 (list_head)

#### 2.1.1 概述

`list_head` 是 Linux 内核中最基础、最广泛使用的数据结构。它是一个双向循环链表，所有操作的时间复杂度均为 O(1)。通过将 `list_head` 嵌入到自定义结构体中，借助 `container_of` 宏实现类型无关的通用链表操作。

**内核实现 vs 传统实现的差异**

传统链表（如 C++ STL `std::list<T>`）是**非侵入式**的：链表节点存储的是数据的副本，数据本身不包含链表链接信息。内核的 `list_head` 是**侵入式**的：数据结构体内部嵌入 `list_head` 成员，链表操作直接操作嵌入的节点指针。

| 特性 | 内核侵入式 (`list_head`) | 传统非侵入式 |
|---|---|---|
| 内存布局 | 节点指针在数据内部 | 数据在节点内部 |
| 内存分配 | 一次 `kmalloc` 即可 | 需要额外分配节点包装器 |
| 空链表开销 | 1 个 `list_head`（16 字节） | 1 个哨兵节点（通常 24+ 字节） |
| 删除操作 | 不需要数据地址，只需 `list_del(&node)` | 需要从数据找到节点容器 |
| 类型安全 | 通过 `container_of` 实现 | 模板/泛型实现 |
| 数据移动 | 节点指针不变，链表关系不变 | 移动数据需要更新链表<br>（或额外封装） |

#### 2.1.2 核心数据结构

```c
// include/linux/types.h
struct list_head {
    struct list_head *next, *prev;
};
```

#### 2.1.3 初始化

```c
// 静态初始化
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

// 动态初始化
static inline void INIT_LIST_HEAD(struct list_head *list)
{
    WRITE_ONCE(list->next, list);
    WRITE_ONCE(list->prev, list);
}
```

#### 2.1.4 核心操作函数

**插入操作**

```c
// 在头部插入 — 在 head 之后插入 new
static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}

// 在尾部插入 — 在 head 之前插入 new
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

// 内部实现：在 prev 和 next 之间插入 new
static inline void __list_add(struct list_head *new,
                              struct list_head *prev,
                              struct list_head *next)
{
    if (!__list_add_valid(new, prev, next))
        return;
    next->prev = new;
    new->next = next;
    new->prev = prev;
    WRITE_ONCE(prev->next, new);
}
```

**删除操作**

```c
// 删除节点（节点内存不会被释放）
static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = LIST_POISON1;
    entry->prev = LIST_POISON2;
}

// 删除并重新初始化
static inline void list_del_init(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

// 内部实现
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    WRITE_ONCE(prev->next, next);
}
```

**移动与合并**

```c
// 将 list 移动到新头部
static inline void list_move(struct list_head *list, struct list_head *head)
{
    __list_del(list->prev, list->next);
    list_add(list, head);
}

// 将 list 移动到新尾部
static inline void list_move_tail(struct list_head *list, struct list_head *head)
{
    __list_del(list->prev, list->next);
    list_add_tail(list, head);
}

// 判断链表是否为空
static inline int list_empty(const struct list_head *head)
{
    return READ_ONCE(head->next) == head;
}

// 拼接两个链表
static inline void list_splice(struct list_head *list, struct list_head *head)
{
    if (!list_empty(list))
        __list_splice(list, head, head->next);
}
```

#### 2.1.5 遍历宏

```c
// 遍历链表节点（返回 list_head 指针）
#define list_for_each(pos, head) \
    for (pos = (head)->next; !list_is_head(pos, (head)); pos = pos->next)

// 安全遍历（支持删除操作）
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; \
         !list_is_head(pos, (head)); \
         pos = n, n = pos->next)

// 遍历并获取宿主结构体
#define list_for_each_entry(pos, head, member)                          \
    for (pos = list_first_entry(head, typeof(*pos), member);            \
         !list_entry_is_head(pos, head, member);                        \
         pos = list_next_entry(pos, member))

// 反向遍历
#define list_for_each_entry_reverse(pos, head, member)                  \
    for (pos = list_last_entry(head, typeof(*pos), member);             \
         !list_entry_is_head(pos, head, member);                        \
         pos = list_prev_entry(pos, member))
```

#### 2.1.6 关键辅助宏

```c
// 从 list_head 指针获取宿主结构体
#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

// 获取第一个/最后一个宿主结构体
#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member) \
    list_entry((ptr)->prev, type, member)
```

#### 2.1.7 完整 API 参考

| 函数/宏 | 功能 | 时间复杂度 |
|---|---|---|
| `list_add(new, head)` | 在头部插入 | O(1) |
| `list_add_tail(new, head)` | 在尾部插入 | O(1) |
| `list_del(entry)` | 删除节点 | O(1) |
| `list_del_init(entry)` | 删除并重新初始化 | O(1) |
| `list_replace(old, new)` | 替换节点 | O(1) |
| `list_move(list, head)` | 移动到头部 | O(1) |
| `list_move_tail(list, head)` | 移动到尾部 | O(1) |
| `list_is_last(pos, head)` | 判断是否为最后一个节点 | O(1) |
| `list_empty(head)` | 判断是否为空 | O(1) |
| `list_is_head(pos, head)` | 判断是否为头节点 | O(1) |
| `list_cut_position(head, list, entry)` | 从 entry 处切割 | O(n) |
| `list_splice(list, head)` | 拼接链表到头部 | O(1) |
| `list_splice_tail(list, head)` | 拼接链表到尾部 | O(1) |
| `list_splice_init(list, head)` | 拼接并初始化原链表 | O(1) |
| `list_bulk_move_tail(head, first, last)` | 批量移动节点到尾部 | O(1) |
| `list_for_each(pos, head)` | 遍历（list_head 指针） | O(n) |
| `list_for_each_safe(pos, n, head)` | 安全遍历（支持删除） | O(n) |
| `list_for_each_entry(pos, head, member)` | 遍历宿主结构体 | O(n) |
| `list_for_each_entry_safe(pos, n, head, member)` | 安全遍历宿主结构体 | O(n) |
| `list_for_each_entry_reverse(pos, head, member)` | 反向遍历 | O(n) |
| `list_for_each_prev(pos, head)` | 反向遍历 list_head | O(n) |
| `list_first_entry_or_null(ptr, type, member)` | 取第一个或 NULL | O(1) |
| `list_entry_is_head(pos, head, member)` | 判断是否到头 | O(1) |
| `list_rotate_to_front(head, entry)` | 将 entry 旋转到头部 | O(1) |
| `list_rotate_left(head)` | 链表左旋 | O(1) |

#### 2.1.8 完整使用示例

```c
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/printk.h>

// 定义一个嵌入 list_head 的结构体
struct my_task {
    int id;
    char name[32];
    struct list_head node;  // 嵌入 list_head
};

// 创建链表头
static LIST_HEAD(task_list);

// 创建并添加节点
void add_task(int id, const char *name)
{
    struct my_task *task = kmalloc(sizeof(*task), GFP_KERNEL);
    if (!task)
        return;
    task->id = id;
    strncpy(task->name, name, sizeof(task->name) - 1);
    list_add_tail(&task->node, &task_list);  // 尾部插入
}

// 遍历所有节点
void print_all_tasks(void)
{
    struct my_task *pos;
    list_for_each_entry(pos, &task_list, node) {
        pr_info("task id=%d, name=%s\n", pos->id, pos->name);
    }
}

// 查找并删除
void remove_task_by_id(int id)
{
    struct my_task *pos, *n;
    list_for_each_entry_safe(pos, n, &task_list, node) {
        if (pos->id == id) {
            list_del(&pos->node);  // 从链表移除
            kfree(pos);            // 释放内存
            return;
        }
    }
}

// 移动到尾部（重新排序）
void move_to_tail(int id)
{
    struct my_task *pos;
    list_for_each_entry(pos, &task_list, node) {
        if (pos->id == id) {
            list_move_tail(&pos->node, &task_list);
            return;
        }
    }
}

// 原子地取出第一个节点
struct my_task *pop_first_task(void)
{
    struct my_task *task = list_first_entry_or_null(&task_list, typeof(*task), node);
    if (task)
        list_del(&task->node);
    return task;
}
```

#### 2.1.9 应用场景

`list_head` 在内核中无处不在，典型应用包括：
- **进程链表**：`task_struct` 中的 `tasks` 字段将所有进程链接为双向循环链表
- **文件系统**：目录项缓存（dentry）、inode 链表
- **内存管理**：空闲页链表（`free_area.free_list`）、LRU 链表
- **设备驱动**：设备模型中的各种链表

---

### 2.2 哈希链表 (hlist_head)

#### 2.2.1 概述

`hlist_head` 是内核专门为哈希表实现设计的链表结构。与 `list_head` 不同，`hlist_head` 只有一个指向第一个节点的 `first` 指针（而非双向循环），从而节省哈希表数组的内存开销。`hlist_node` 使用 `pprev`（二级指针）实现高效的删除操作。

**内核实现 vs 传统实现（`list_head`）的差异**

| 特性 | `hlist_head` | `list_head` |
|---|---|---|
| 头节点大小 | 1 指针（8 字节） | 2 指针（16 字节） |
| 普通节点大小 | 2 指针（16 字节） | 2 指针（16 字节） |
| 遍历方向 | 单向 | 双向 |
| 删除操作 | 通过 `pprev` 二级指针实现 | 直接操作 `prev`/`next` |
| 适用场景 | 哈希表桶数组（大量桶时空间减半） | 通用双向链表 |
| 空间效率 | 桶数组：`N × 8` 字节 | 桶数组：`N × 16` 字节 |
| 优点 | 节省大量内存（哈希表可能有数千个桶） | 操作更灵活，支持反向遍历 |
| 缺点 | 反向遍历需要重新从头开始，删除需 `hlist_del` | 作为哈希表桶时浪费一半空间 |

#### 2.2.2 核心数据结构

```c
// include/linux/types.h
struct hlist_head {
    struct hlist_node *first;
};

struct hlist_node {
    struct hlist_node *next, **pprev;
};
```

`pprev` 是指向上一个节点的 `next` 指针（或 `hlist_head.first`）的指针。这种设计使得删除节点时无需遍历链表来查找前驱节点。

#### 2.2.3 初始化

```c
#define HLIST_HEAD_INIT { .first = NULL }
#define HLIST_HEAD(name) struct hlist_head name = { .first = NULL }

static inline void INIT_HLIST_HEAD(struct hlist_head *h)
{
    h->first = NULL;
}

static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
    h->next = NULL;
    h->pprev = NULL;
}
```

#### 2.2.4 核心操作函数

```c
// 判断链表是否为空
static inline int hlist_empty(const struct hlist_head *h)
{
    return !READ_ONCE(h->first);
}

// 在头部插入
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
    struct hlist_node *first = h->first;
    WRITE_ONCE(n->next, first);
    if (first)
        WRITE_ONCE(first->pprev, &n->next);
    WRITE_ONCE(h->first, n);
    WRITE_ONCE(n->pprev, &h->first);
}

// 删除节点
static inline void hlist_del(struct hlist_node *n)
{
    __hlist_del(n);
    WRITE_ONCE(n->next, LIST_POISON1);
    WRITE_ONCE(n->pprev, LIST_POISON2);
}

static inline void __hlist_del(struct hlist_node *n)
{
    struct hlist_node *next = n->next;
    struct hlist_node **pprev = n->pprev;
    WRITE_ONCE(*pprev, next);
    if (next)
        WRITE_ONCE(next->pprev, pprev);
}
```

#### 2.2.5 遍历宏

```c
#define hlist_for_each_entry(pos, head, member)                         \
    for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member); \
         pos;                                                           \
         pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

#define hlist_for_each_entry_safe(pos, n, head, member)                 \
    for (pos = hlist_entry_safe((head)->first, typeof(*pos), member);   \
         pos && ({ n = pos->member.next; 1; });                        \
         pos = hlist_entry_safe(n, typeof(*pos), member))
```

#### 2.2.6 完整 API 参考

| 函数/宏 | 功能 | 时间复杂度 |
|---|---|---|
| `hlist_add_head(n, h)` | 插入到头部 | O(1) |
| `hlist_add_before(n, next)` | 在指定节点前插入 | O(1) |
| `hlist_add_behind(n, prev)` | 在指定节点后插入 | O(1) |
| `hlist_del(n)` | 删除节点 | O(1) |
| `hlist_del_init(n)` | 删除并重新初始化 | O(1) |
| `hlist_move_list(old, new)` | 移动整个链表 | O(1) |
| `hlist_unhashed(n)` | 判断节点是否未被哈希 | O(1) |
| `hlist_empty(h)` | 判断链表是否为空 | O(1) |
| `hlist_for_each(pos, head)` | 遍历（hlist_node 指针） | O(n) |
| `hlist_for_each_entry(pos, head, member)` | 遍历宿主结构体 | O(n) |
| `hlist_for_each_entry_safe(pos, n, head, member)` | 安全遍历 | O(n) |
| `hlist_entry(ptr, type, member)` | 获取宿主结构体 | O(1) |

#### 2.2.7 完整使用示例

```c
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/jhash.h>

// 定义一个嵌入 hlist_node 的结构体
struct my_cache_entry {
    u32 key;
    void *data;
    struct hlist_node node;  // 哈希链表节点
};

// 定义哈希表（256 个桶）
#define MY_BITS 8
static DEFINE_HASHTABLE(my_cache, MY_BITS);

// 哈希函数
static inline u32 my_hash(u32 key)
{
    return jhash_1word(key, 0xdeadbeef);
}

// 插入缓存项
void cache_add(u32 key, void *data)
{
    struct my_cache_entry *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return;
    entry->key = key;
    entry->data = data;
    // 插入到哈希表
    hash_add(my_cache, &entry->node, my_hash(key));
}

// 查找缓存项
void *cache_lookup(u32 key)
{
    struct my_cache_entry *entry;
    // 只在对应桶中遍历
    hash_for_each_possible(my_cache, entry, node, my_hash(key)) {
        if (entry->key == key)
            return entry->data;
    }
    return NULL;
}

// 删除缓存项
void cache_remove(u32 key)
{
    struct my_cache_entry *entry;
    struct hlist_node *tmp;
    hash_for_each_possible_safe(my_cache, entry, tmp, node, my_hash(key)) {
        if (entry->key == key) {
            hash_del(&entry->node);
            kfree(entry);
            return;
        }
    }
}

// 遍历所有缓存项
void cache_dump(void)
{
    struct my_cache_entry *entry;
    int bkt;

    hash_for_each(my_cache, bkt, entry, node) {
        pr_info("bucket %d: key=%u\n", bkt, entry->key);
    }
}
```

#### 2.2.8 应用场景

- **文件系统**：dentry 哈希缓存、inode 哈希表
- **网络子系统**：连接跟踪表、路由缓存
- **进程管理**：PID 哈希表（`pid_hash`）
- **内存管理**：页缓存哈希查找

---

## 3. 树结构

### 3.1 红黑树 (rb_node)

#### 3.1.1 概述

红黑树是一种自平衡二叉查找树，Linux 内核使用它来管理需要有序存储的数据。红黑树的特性保证插入、删除和查找操作的时间复杂度均为 O(log n)。内核中红黑树的实现不包含搜索和插入逻辑（这些由使用者实现），只提供颜色管理和平衡旋转等核心操作。

**内核实现 vs 传统实现的差异**

内核红黑树采用**分离式接口**设计：将"查找插入位置"（由调用者负责）和"插入后平衡修复"（由内核负责）明确分开。这与传统库（如 C++ STL `std::map`）的封装式设计不同。

| 特性 | 内核实现 (分离式) | 传统实现 (封装式, 如 `std::map`) |
|---|---|---|
| 插入流程 | 两步：① 调用者用 while 循环找位置并 `rb_link_node`；② 调用 `rb_insert_color` 修复 | 一步：`map.insert(key, value)` 内部完成全部操作 |
| 比较逻辑 | 调用者用自定义 `if/else` 实现，可嵌入复杂逻辑（如区间匹配、模糊查找） | 通过模板比较器或 `operator<`，固定为全序比较 |
| 查找策略 | 完全自定义，可支持非标准查找（如查找最接近的、查找范围） | 固定为精确查找或 `lower_bound`/`upper_bound` |
| 对调用者的要求 | 必须理解红黑树插入语义，否则可能跳过 `rb_insert_color` 导致树不平衡 | 无需了解内部实现 |
| 优点 | 灵活可控，适合需要特殊查找逻辑的场景（如内核 VMA 查找：按地址区间匹配） | 使用简单、安全，不易出错 |
| 缺点 | 容易出错（忘记平衡修复、比较逻辑错误），代码模板化重复 | 灵活性不足，不适合需要自定义查找策略的场景 |

#### 3.1.2 红黑树性质

1. 每个节点是红色或黑色
2. 根节点是黑色
3. 所有叶子节点（NIL）是黑色
4. 红色节点的两个子节点都是黑色（即不能有连续红色节点）
5. 从任一节点到其每个叶子节点的所有路径包含相同数量的黑色节点

#### 3.1.3 核心数据结构

```c
// include/linux/rbtree.h
struct rb_node {
    unsigned long  __rb_parent_color;  // 父节点指针和颜色编码（低2位）
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rb_root {
    struct rb_node *rb_node;
};
```

**关键设计**：`__rb_parent_color` 字段将父节点指针和颜色标志位编码在一个 `unsigned long` 中。由于 `rb_node` 按 `sizeof(long)` 对齐，地址的低 2 位始终为 0，可用于存储颜色信息（红/黑）。

#### 3.1.4 辅助宏和内联函数

```c
// 获取父节点指针（屏蔽低2位）
#define rb_parent(r)   ((struct rb_node *)((r)->__rb_parent_color & ~3))

// 获取/设置颜色
#define rb_color(r)     __rb_color((r)->__rb_parent_color)
#define rb_is_red(r)    __rb_is_red((r)->__rb_parent_color)
#define rb_is_black(r)  __rb_is_black((r)->__rb_parent_color)
#define rb_set_red(r)   __rb_set_red(&(r)->__rb_parent_color)
#define rb_set_black(r) __rb_set_black(&(r)->__rb_parent_color)

// 设置父节点
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                                struct rb_node **rb_link)
{
    node->__rb_parent_color = (unsigned long)parent;
    node->rb_left = node->rb_right = NULL;
    *rb_link = node;
}
```

#### 3.1.5 核心操作函数

**插入后颜色修复**

```c
extern void rb_insert_color(struct rb_node *, struct rb_root *);
```

插入新节点后，调用 `rb_insert_color` 修复红黑树性质。该函数在 `lib/rbtree.c` 中实现，主要处理以下情况：
- 叔节点为红色：颜色翻转
- 叔节点为黑色：旋转操作

**删除操作**

```c
extern void rb_erase(struct rb_node *, struct rb_root *);
```

**增强删除**（缓存了最左节点，适用于频繁删除场景）

```c
extern void rb_erase_cached(struct rb_node *node, struct rb_root_cached *root);
```

#### 3.1.6 查找和遍历

```c
// 查找树中最左边的节点（最小值）
static inline struct rb_node *rb_first(const struct rb_root *root)
{
    struct rb_node *n = root->rb_node;
    if (!n)
        return NULL;
    while (n->rb_left)
        n = n->rb_left;
    return n;
}

// 查找后继节点（中序遍历的下一个）
static inline struct rb_node *rb_next(const struct rb_node *node)
{
    struct rb_node *parent;
    if (node->rb_right) {
        node = node->rb_right;
        while (node->rb_left)
            node = node->rb_left;
        return (struct rb_node *)node;
    }
    while ((parent = rb_parent(node)) && node == parent->rb_right)
        node = parent;
    return parent;
}

// 查找前驱节点
static inline struct rb_node *rb_prev(const struct rb_node *node)
{
    // ... 对称实现
}
```

#### 3.1.7 插入与删除的平衡原理

**插入平衡（`rb_insert_color`）**

插入新节点时，新节点总是被着色为红色。然后根据父节点和叔节点的颜色进行修复：

```
Case 1: 父节点是黑色
        → 直接插入，无需修复（红黑树性质仍满足）

Case 2: 父节点是红色，叔节点是红色
        → 颜色翻转：将父节点和叔节点变黑，祖父节点变红
        → 递归检查祖父节点（可能产生新的冲突）

Case 3: 父节点是红色，叔节点是黑色（LL / LR / RL / RR 型）
        → 旋转操作，消除连续红色节点
        ┌──────────────────────────────────────────────┐
        │  插入场景        旋转类型        操作          │
        ├──────────────────────────────────────────────┤
        │  父是左子，新是左子  LL → 右旋祖父节点        │
        │  父是左子，新是右子  LR → 先左旋父，再右旋祖父 │
        │  父是右子，新是右子  RR → 左旋祖父节点        │
        │  父是右子，新是左子  RL → 先右旋父，再左旋祖父 │
        └──────────────────────────────────────────────┘
```

**删除平衡（`rb_erase`）**

删除节点时，如果删除的节点是黑色，会破坏红黑性质 5（黑色路径长度相等）。修复函数通过以下方式恢复平衡：

```
Case 1: 兄弟节点是红色
        → 将兄弟变黑，父变红，旋转父节点
        → 转化为兄弟是黑色的情况

Case 2: 兄弟节点是黑色，兄弟的两个子节点都是黑色
        → 将兄弟变红，问题向上推给父节点

Case 3: 兄弟节点是黑色，兄弟的左子是红色，右子是黑色
        → 将兄弟变红，左子变黑，右旋兄弟节点
        → 转化为 Case 4

Case 4: 兄弟节点是黑色，兄弟的右子是红色
        → 将兄弟设为父的颜色，父变黑，兄弟右子变黑，左旋父节点
        → 修复完成
```

#### 3.1.8 完整 API 参考

| 函数/宏 | 功能 | 时间复杂度 |
|---|---|---|
| `rb_insert_color(node, root)` | 插入后修复颜色 | O(log n) |
| `rb_erase(node, root)` | 删除节点 | O(log n) |
| `rb_erase_cached(node, root)` | 从缓存根中删除 | O(log n) |
| `rb_link_node(node, parent, rb_link)` | 链接节点到树中 | O(1) |
| `rb_replace_node(old, new, root)` | 替换节点 | O(1) |
| `rb_replace_node_cached(old, new, root)` | 在缓存根中替换 | O(1) |
| `rb_first(root)` | 查找最小值 | O(log n) |
| `rb_last(root)` | 查找最大值 | O(log n) |
| `rb_next(node)` | 查找后继节点 | O(1) avg |
| `rb_prev(node)` | 查找前驱节点 | O(1) avg |
| `rb_find(addr, root, cmp)` | 二叉查找 | O(log n) |
| `rb_find_add(node, root, cmp)` | 查找并插入 | O(log n) |
| `rb_add(node, root, cmp)` | 简单插入 | O(log n) |
| `rb_add_cached(node, root, less)` | 插入到缓存根 | O(log n) |
| `rb_first_postorder(root)` | 后序遍历第一个 | O(log n) |
| `rb_next_postorder(node)` | 后序遍历下一个 | O(1) |
| `rb_entry(ptr, type, member)` | 获取宿主结构体 | O(1) |
| `RB_EMPTY_ROOT(root)` | 判断树是否为空 | O(1) |
| `RB_CLEAR_NODE(node)` | 清除节点状态 | O(1) |
| `rbtree_postorder_for_each_entry_safe` | 安全后序遍历 | O(n) |

#### 3.1.9 增强根节点

```c
// 缓存最左节点（最小值），加速某些场景
struct rb_root_cached {
    struct rb_node *rb_node;
    struct rb_node *rb_leftmost;
};
```

#### 3.1.10 完整使用示例

```c
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/printk.h>

// 定义需要按 key 排序的结构体
struct my_node {
    u32 key;
    struct rb_node node;  // 嵌入 rb_node
};

// 插入（标准模式）
int my_insert(struct rb_root *root, struct my_node *new)
{
    struct rb_node **link = &root->rb_node;
    struct rb_node *parent = NULL;
    struct my_node *entry;

    while (*link) {
        parent = *link;
        entry = rb_entry(parent, struct my_node, node);
        if (new->key < entry->key)
            link = &parent->rb_left;
        else if (new->key > entry->key)
            link = &parent->rb_right;
        else
            return -EEXIST;  // 键已存在
    }
    rb_link_node(&new->node, parent, link);
    rb_insert_color(&new->node, root);
    return 0;
}

// 插入（使用辅助宏 rb_add，更简洁）
static bool my_less(struct rb_node *a, const struct rb_node *b)
{
    return rb_entry(a, struct my_node, node)->key <
           rb_entry(b, struct my_node, node)->key;
}

int my_insert_simple(struct rb_root *root, struct my_node *new)
{
    struct rb_node *dup = rb_add(&new->node, root, my_less);
    if (dup)
        return -EEXIST;  // 重复键
    return 0;
}

// 查找
struct my_node *my_search(struct rb_root *root, u32 key)
{
    struct rb_node *node = root->rb_node;
    while (node) {
        struct my_node *entry = rb_entry(node, struct my_node, node);
        if (key < entry->key)
            node = node->rb_left;
        else if (key > entry->key)
            node = node->rb_right;
        else
            return entry;
    }
    return NULL;
}

// 查找（使用 rb_find 辅助函数）
static int my_cmp(const void *key, const struct rb_node *node)
{
    u32 k = *(u32 *)key;
    struct my_node *entry = rb_entry(node, struct my_node, node);
    if (k < entry->key) return -1;
    if (k > entry->key) return 1;
    return 0;
}

struct my_node *my_search_v2(struct rb_root *root, u32 key)
{
    struct rb_node *node = rb_find(&key, root, my_cmp);
    return node ? rb_entry(node, struct my_node, node) : NULL;
}

// 中序遍历（按 key 升序）
void my_print_all(struct rb_root *root)
{
    struct rb_node *node;
    for (node = rb_first(root); node; node = rb_next(node)) {
        struct my_node *entry = rb_entry(node, struct my_node, node);
        pr_info("key=%u\n", entry->key);
    }
}

// 删除
void my_delete(struct rb_root *root, u32 key)
{
    struct my_node *entry = my_search(root, key);
    if (entry) {
        rb_erase(&entry->node, root);
        kfree(entry);
    }
}
```

#### 3.1.11 应用场景

- **CFS 调度器**：用红黑树管理可运行进程，以 `vruntime` 为键值，每次选择 `vruntime` 最小的进程运行
- **内存管理**：VMA（虚拟内存区域）管理，以地址为键值实现快速查找
- **文件系统**：inode 缓存管理
- **网络子系统**：TCP 连接管理、路由表

---

### 3.2 基数树 (Radix Tree)

#### 3.2.1 概述

基数树（Radix Tree，又称 Patricia Trie）是一种按位索引的压缩树结构。它通过将键值按位拆分，构建多级树来高效管理稀疏的整数索引映射。在内核中，基数树曾被广泛用于**页缓存管理**和 **IDR 分配器**，但在现代内核中已被 XArray 替代。

#### 3.2.2 核心数据结构

```c
// include/linux/radix-tree.h (历史实现)
struct radix_tree_root {
    spinlock_t              xa_lock;     // 与 XArray 兼容
    gfp_t                   gfp_mask;
    struct radix_tree_node  *rnode;      // 根节点指针
};

struct radix_tree_node {
    unsigned char   shift;       // 当前节点对应的偏移位数
    unsigned char   offset;      // 在父节点中的 slot 索引
    unsigned int    count;       // 非空子节点计数
    union {
        struct list_head private_list;  // 内部节点链表
        struct rcu_head rcu_head;       // RCU 回收
    };
    void __rcu      *slots[RADIX_TREE_MAP_SIZE];  // 子节点指针数组（64个）
    unsigned long    tags[RADIX_TREE_MAX_TAGS][BITS_TO_LONGS(RADIX_TREE_MAP_SIZE)];
};
```

#### 3.2.3 关键设计

- **层级结构**：每层处理 6 位（`RADIX_TREE_MAP_SHIFT = 6`），即每个节点有 64 个 slot
- **标记系统**：每个节点维护 tags 位图，用于标记页面的脏、回写等状态
- **RCU 安全**：支持 RCU 无锁读取

#### 3.2.4 历史地位

基数树是 XArray 的前身，XArray 保留了其多级树的核心结构，但提供了更简洁的 API 和更好的 RCU 安全保证。在 Linux 4.20+ 内核中，`address_space.i_pages` 已从 `radix_tree_root` 迁移为 `struct xarray`。

#### 3.2.5 应用场景

- **页缓存**：管理文件页的索引（已被 XArray 替代）
- **IDR 分配器**：整数 ID 到指针的映射（已被 XArray 替代）

---

### 3.3 Maple Tree

#### 3.3.1 概述

Maple Tree 是 Linux 6.1+ 引入的、为**范围查询优化**的 B-Tree 变体，正在逐步替代红黑树和基数树，用于管理进程地址空间的 VMA。其核心优势在于范围查找效率更高，且天然支持 RCU 无锁读取。

**Maple Tree vs 红黑树（传统范围管理方式）的差异**

| 特性 | Maple Tree | 红黑树 |
|---|---|---|
| 存储单元 | 区间 `[start, last]` | 单点 `key → value` |
| 范围查找 | 直接区间匹配 O(log n) | 需要遍历找到包含该地址的节点 |
| 区间插入/删除 | 天然支持，可合并/分割区间 | 需要额外逻辑处理区间重叠 |
| 节点扇出 | 每个节点多个 slot（类似 B-Tree） | 每个节点 1 个元素 |
| 树高度 | 较低（大扇出） | 较高（二叉） |
| RCU 安全 | 内建支持 | 需要外部 RCU 保护 |
| 空间局部性 | 好（节点内连续存储） | 差（分散的节点分配） |
| 优点 | 范围操作高效，RCU 安全，缓存友好 | 实现简单成熟，理论验证充分 |
| 缺点 | 实现复杂，较新（6.1+），工具链支持有限 | 范围操作需要额外逻辑，内存碎片化 |

#### 3.3.2 核心数据结构

```c
// include/linux/maple_tree.h
struct maple_tree {
    spinlock_t ma_lock;       // 保护树的锁
    unsigned int ma_flags;    // 标志位
    void __rcu *ma_root;      // 根节点（RCU 保护）
};

// 内部节点结构（简化）
struct maple_node {
    unsigned long type;       // 节点类型（叶/内部）
    struct maple_metadata *parent;
    union {
        struct {
            struct maple_slot *slots;  // 子节点 slot 数组
            unsigned long *pivots;     // 区间分割点
        };
        struct {
            void *data;                // 叶节点数据
            unsigned long end;         // 区间结束
        };
    };
};
```

#### 3.3.3 核心操作

```c
// 初始化
void mt_init(struct maple_tree *mt);
#define MT_FLAGS_ALLOC_RANGE  // 启用 IDA 风格分配

// 查找
void *mt_find(struct maple_tree *mt, unsigned long *index, unsigned long max);
void *mt_find_after(struct maple_tree *mt, unsigned long *index, unsigned long max);

// 区间存储（核心优势）
int mt_store_range(struct maple_tree *mt, unsigned long start,
                   unsigned long last, void *entry, gfp_t gfp);

// 单点操作
int mt_insert(struct maple_tree *mt, unsigned long index, void *entry, gfp_t gfp);
void *mt_erase(struct maple_tree *mt, unsigned long index);

// 遍历
void *mt_first(struct maple_tree *mt, unsigned long *index);
void *mt_next(struct maple_tree *mt, void *entry, unsigned long max);
void *mt_last(struct maple_tree *mt, unsigned long *index);
void *mt_prev(struct maple_tree *mt, void *entry, unsigned long min);

// 区间操作
void *mt_find_range(struct maple_tree *mt, unsigned long *start,
                    unsigned long *last, unsigned long min, unsigned long max);
void *mt_find_within(struct maple_tree *mt, struct ma_state *mas,
                     unsigned long *index, unsigned long *last, unsigned long min, unsigned long max);

// 遍历宏
#define mt_for_each(mt, entry, index, max)  // 遍历所有条目
#define mas_for_each(mas, entry, max)       // 使用 maple state 遍历

// 分配 ID（IDA 风格）
int mt_alloc_range(struct maple_tree *mt, unsigned long *start,
                   unsigned long *last, unsigned long min, unsigned long max, gfp_t gfp);
int mt_alloc_cyclic(struct maple_tree *mt, unsigned long *start,
                    unsigned long *last, unsigned long min, unsigned long max, gfp_t gfp);
```

#### 3.3.4 原理细节：B-Tree 风格的区间管理

Maple Tree 的核心创新在于将 B-Tree 的区间管理能力与 RCU 安全结合：

**节点结构**

```
每个内部节点有 2 个关键数组：
  pivots[] = [10, 30, 60, MAX]         ← 区间分割点
  slots[]  = [child0, child1, ...]     ← 对应子节点

查找地址 25 的过程：
  pivots[0]=10, pivots[1]=30 → 25 在 (10, 30] 之间 → 进入 slot[1]
```

**范围存储而非单点**

传统红黑树存储的是单个键值对，存储一个区间 `[start, end]` 需要两个节点或特殊处理。Maple Tree 天然支持以区间为单元的存储和查找：

```
红黑树：  key=0x1000 → 指向 VMA1
         key=0x2000 → 指向 VMA2

Maple Tree:  range [0x1000, 0x1FFF] → VMA1
             range [0x2000, 0x3FFF] → VMA2
```

这意味着查找 `addr=0x1500` 时，红黑树需要遍历找到包含该地址的节点，而 Maple Tree 直接用区间匹配即可。

**RCU 无锁读取**

- 写操作通过 COW（Copy-on-Write）方式更新节点
- 读操作在 RCU 临界区内可以直接访问旧版本或新版本
- 节点回收通过 RCU 回调延迟释放

#### 3.3.5 完整使用示例

```c
#include <linux/maple_tree.h>
#include <linux/slab.h>
#include <linux/printk.h>

// 定义 Maple Tree（VMA 管理场景）
static struct maple_tree vma_tree;
static DEFINE_SPINLOCK(vma_lock);

// 初始化
void vma_init(void)
{
    mt_init(&vma_tree);
}

// 添加 VMA 映射（区间映射）
int vma_add(unsigned long start, unsigned long end, void *vma)
{
    int ret;

    // 加锁保护写操作
    mas_lock(&vma_tree);
    ret = mt_store_range(&vma_tree, start, end - 1, vma, GFP_KERNEL);
    mas_unlock(&vma_tree);
    return ret;
}

// 查找某个地址所属的 VMA
void *vma_find(unsigned long addr)
{
    unsigned long index = addr;
    void *entry;

    // RCU 读锁保护
    rcu_read_lock();
    entry = mt_find(&vma_tree, &index, ULONG_MAX);
    rcu_read_unlock();
    return entry;
}

// 删除 VMA
void *vma_remove(unsigned long addr)
{
    void *entry;

    mas_lock(&vma_tree);
    entry = mt_erase(&vma_tree, addr);
    mas_unlock(&vma_tree);
    return entry;
}

// 遍历所有 VMA 区间
void vma_dump_all(void)
{
    unsigned long index = 0;
    void *entry;

    rcu_read_lock();
    mt_for_each(&vma_tree, entry, index, ULONG_MAX) {
        pr_info("vma at %lx\n", index);
    }
    rcu_read_unlock();
}
```

#### 3.3.6 应用场景

- **VMA 管理**：`mm_struct` 中的 `mm_mt` 字段，替代了旧的红黑树实现
- **地址空间区间查找**：快速查找某个地址所属的 VMA

---

### 3.4 B树变体

#### 3.4.1 概述

B-Tree（及其变体 B+Tree）是文件系统和存储场景的核心数据结构。与红黑树每个节点仅存一个键值不同，B-Tree 每个节点可以包含多个键值，具有很大的扇出（fanout），大幅减少树的高度和磁盘 I/O 次数。

**B-Tree 与 B+Tree 的核心区别**

| 特性 | B-Tree | B+Tree |
|------|--------|--------|
| 数据存储 | 内部节点和叶节点都存储数据 | 仅叶节点存储数据，内部节点只存键 |
| 键的冗余 | 键不重复，每个键唯一存在 | 键在内部节点中冗余，叶节点包含所有键 |
| 叶节点链表 | 无 | 叶节点形成双向链表，支持范围扫描 |
| 内部节点扇出 | 较小（因为要存数据指针） | 更大（只存键，节点可容纳更多条目） |
| 范围查询 | 需中序遍历，跨节点复杂 | 叶节点链表顺序扫描，高效 |
| 查找稳定性 | 任何节点找到即返回，不稳定 | 必须到叶节点，路径长度恒定 |
| 空间利用率 | 较差（节点分裂后内部节点可能空闲） | 更好（内部节点只存键，密度高） |

**内核 B-Tree 变体的设计权衡**

内核中 B-Tree 变体的设计遵循一个核心原则：**根据数据访问模式选择最合适的 B-Tree 形态**。具体来说：

- **磁盘 vs 内存**：磁盘场景追求高扇出（4K 大节点）以减少 I/O 次数；内存场景追求缓存友好（小节点 128~256 字节）
- **写时复制 vs 原地更新**：COW 变体（Btrfs）支持快照和事务回滚，但写放大更大；原地更新（Ext4 Htree、XFS）写放大较小
- **通用 vs 专用**：通用实现（`lib/btree.c`、XFS On-Disk）需要适配多种场景，灵活性高但优化空间有限；专用实现（XFS In-Core Extent）针对特定场景极致优化
- **哈希 vs 有序**：Ext4 Htree 用哈希值作为键，查找快但无法范围查询；有序 B+Tree 支持范围扫描和顺序访问

内核中出现了多种 B-Tree 变体实现，各有侧重。

#### 3.4.2 内核 B-Tree 变体分类

| 变体 | 位置 | 用途 | 核心特点 |
|------|------|------|----------|
| **通用 B+Tree** | `lib/btree.c` — `include/linux/btree.h` | 通用的内存 B+Tree | 纯内存实现，支持 u32/u64/128bit 键，基于 mempool 的节点分配 |
| **Btrfs COW B-Tree** | `fs/btrfs/` | 文件系统元数据管理 | **Copy-on-Write** 写时复制，支持事务、快照，节点大小 4K~64K |
| **Ext4 Htree** | `fs/ext4/namei.c` | 目录项索引 | 哈希 B-Tree 变体，将文件名哈希为键，最多 2 层 |
| **XFS In-Core B+Tree** | `fs/xfs/libxfs/xfs_iext_tree.c` | 内存中 extent 映射 | 专用 B+Tree，节点大小 256 字节，叶节点形成双向链表 |
| **XFS On-Disk B+Tree** | `fs/xfs/libxfs/xfs_btree*.c` | 通用磁盘 B+Tree | 通用 B+Tree 框架，支持多种类型（inode、空闲空间、映射、引用计数等） |

#### 3.4.3 通用 B+Tree 实现（`lib/btree.c`）

##### 3.4.3.1 概述

`lib/btree.c` 提供了一种**纯内存**的通用 B+Tree 实现，用于需要稀疏地址空间查找的场景。它通过 `btree_geo`（几何结构）定义键长度和每节点条目数，支持 32 位、64 位和 128 位键。

##### 3.4.3.2 核心数据结构

```c
// include/linux/btree.h

// B+Tree 句柄
struct btree_head {
    unsigned long *node;      // 根节点指针
    mempool_t *mempool;       // 节点分配内存池
    int height;               // 树高度
};

// 几何结构（定义键长和扇出）
struct btree_geo {
    int keylen;               // 键长（unsigned long 为单位）
    int no_pairs;             // 每个节点的键值对数量
    int no_longs;             // 节点中 key 区域占用的 unsigned long 数
};

// 预定义几何结构
extern struct btree_geo btree_geo32;   // 32 位键
extern struct btree_geo btree_geo64;   // 64 位键
extern struct btree_geo btree_geo128;  // 128 位键（2 × u64）
```

##### 3.4.3.3 节点布局

```c
// 节点大小：max(L1_CACHE_BYTES, 128)，通常为 128 字节
// 以 64 位系统、btree_geo64 为例：
//   keylen = 1, no_pairs = NODESIZE / sizeof(long) / 2 = 8
//   每个节点 = 8 个 key + 8 个 val = 16 个 long = 128 字节

// 节点内部布局：
// [key1] [key2] ... [keyN] [val1] [val2] ... [valN]
//  key 区域：从 node[0] 到 node[no_longs-1]
//  val 区域：从 node[no_longs] 到 node[2*no_longs-1]
//  N = no_pairs = 8（每个节点最多 8 个键值对）

// 约定：最小键值在最右侧，未使用的槽填 NULL
// 查找时顺序遍历，遇到第一个 NULL 键终止
```

##### 3.4.3.4 核心 API

```c
// 初始化/销毁
int btree_init(struct btree_head *head);                         // 创建内部 mempool
void btree_init_mempool(struct btree_head *head, mempool_t *mp); // 使用外部 mempool
void btree_destroy(struct btree_head *head);                     // 销毁 mempool

// 查找
void *btree_lookup(struct btree_head *head, struct btree_geo *geo,
                   unsigned long *key);                          // 精确查找
void *btree_last(struct btree_head *head, struct btree_geo *geo,
                 unsigned long *key);                            // 最后一个条目
void *btree_get_prev(struct btree_head *head, struct btree_geo *geo,
                     unsigned long *key);                        // 前一个条目

// 插入/删除
int btree_insert(struct btree_head *head, struct btree_geo *geo,
                 unsigned long *key, void *val, gfp_t gfp);      // 插入（键不能已存在）
int btree_update(struct btree_head *head, struct btree_geo *geo,
                 unsigned long *key, void *val);                 // 更新已有键的值
void *btree_remove(struct btree_head *head, struct btree_geo *geo,
                   unsigned long *key);                          // 删除

// 遍历
void btree_visit(struct btree_head *head, struct btree_geo *geo,
                 void (*func)(void *elem));                      // 遍历所有值

// 类型特化
#define btree_for_each_safe64(head, key, val)  // 安全遍历 64 位 B+Tree
```

##### 3.4.3.5 实现要点

```c
// lib/btree.c 核心逻辑

// 1. 查找（btree_lookup）：从根节点逐层向下
//    每层遍历所有键，找到第一个大于等于目标键的槽
//    叶节点中直接返回 val

// 2. 插入（btree_insert）：先查找到叶节点应插入位置
//    如果叶节点已满（no_pairs 个槽全满），则分裂：
//    a. 分配新节点
//    b. 将原节点后半部分键值对移到新节点
//    c. 在父节点中插入新键和指针
//    d. 如需分裂递归向上

// 3. 删除（btree_remove）：先找到目标条目
//    如果叶节点条目数低于阈值，尝试从兄弟节点借用或合并

// 键比较（longcmp）：逐 unsigned long 比较，支持多字键
static int longcmp(const unsigned long *l1, const unsigned long *l2, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (l1[i] < l2[i]) return -1;
        if (l1[i] > l2[i]) return  1;
    }
    return 0;
}

// 辅助函数：获取节点中第 n 个键的指针
static unsigned long *bkey(struct btree_geo *geo, unsigned long *node, int n)
{
    return &node[n * geo->keylen];
}

// 辅助函数：获取节点中第 n 个值的指针
static void *bval(struct btree_geo *geo, unsigned long *node, int n)
{
    return (void *)node[geo->no_longs + n];
}
```

##### 3.4.3.6 注意点

- **无二分查找**：节点内使用线性扫描（`longcmp` 逐个比较），而非二分查找。文档注释明确说明 "we currently do not use binary search"，这是因为节点尺寸小（128 字节），线性扫描的缓存局部性更好
- **NULL 值不可存储**：lookup 返回 NULL 表示未找到，因此不能存储 NULL 指针
- **内存池**：节点通过 mempool 分配，保证在内存压力下仍能分配节点（可能阻塞等待）

#### 3.4.4 Btrfs COW B-Tree

##### 3.4.4.1 概述

Btrfs 的 B-Tree 实现是最复杂的变体，其核心特点是 **Copy-on-Write（写时复制）**：每次修改节点时，不直接覆盖原节点，而是写入新的块，然后更新父节点指针。这为快照、事务回滚和数据校验提供了基础。

##### 3.4.4.2 核心数据结构

```c
// include/uapi/linux/btrfs_tree.h

// ---------- 键（Key） ----------
// 三元组 (objectid, type, offset) 唯一标识一条记录
struct btrfs_disk_key {
    __le64 objectid;     // 对象 ID（如 inode 号、块号）
    __u8   type;         // 键类型（如 INODE_ITEM、EXTENT_ITEM、DIR_ITEM）
    __le64 offset;       // 偏移量（如文件偏移、块组偏移）
} __attribute__ ((__packed__));

// 内存中的键（与磁盘格式一致，小端时无需转换）
struct btrfs_key {
    __u64 objectid;
    __u8  type;
    __u64 offset;
} __attribute__ ((__packed__));

// ---------- 树节点 ----------
// 所有树块（叶节点和非叶节点）以相同头部开始
struct btrfs_header {
    __u8  csum[BTRFS_CSUM_SIZE];         // 校验和
    __u8  fsid[BTRFS_FSID_SIZE];          // 文件系统 UUID
    __le64 bytenr;                        // 块物理地址
    __le64 flags;                         // 标志位
    __u8  chunk_tree_uuid[BTRFS_UUID_SIZE];
    __le64 generation;                    // 事务世代号
    __le64 owner;                         // 所属树根对象 ID
    __le32 nritems;                       // 条目数
    __u8  level;                          // 层级（0 = 叶节点）
} __attribute__ ((__packed__));

// 内部节点：键 + 指针数组
struct btrfs_key_ptr {
    struct btrfs_disk_key key;    // 子节点中最小键
    __le64 blockptr;              // 子节点块地址
    __le64 generation;            // 子节点世代号
} __attribute__ ((__packed__));

struct btrfs_node {
    struct btrfs_header header;
    struct btrfs_key_ptr ptrs[];  // 变长数组
} __attribute__ ((__packed__));

// 叶节点：条目数组（条目本身存储在数据区）
struct btrfs_item {
    struct btrfs_disk_key key;    // 条目键
    __le32 offset;                // 数据区偏移
    __le32 size;                  // 数据大小
} __attribute__ ((__packed__));

struct btrfs_leaf {
    struct btrfs_header header;
    struct btrfs_item items[];    // 变长数组
} __attribute__ ((__packed__));

// 叶节点布局：
// [item0][item1]...[itemN] [free space] [dataN]...[data1][data0]
//  item 区从头部向尾部增长
//  data 区从尾部向头部增长
//  item 中的 offset 指向数据区，size 标记数据大小

// ---------- 路径（Path）----------
// 搜索路径：从根到叶的完整路径，用于避免重复遍历
// fs/btrfs/ctree.h
struct btrfs_path {
    struct extent_buffer *nodes[BTRFS_MAX_LEVEL];  // 每层节点
    int slots[BTRFS_MAX_LEVEL];                    // 每层当前槽位
    u8 locks[BTRFS_MAX_LEVEL];                     // 锁定状态
    u8 reada;                                      // 预读策略
    u8 lowest_level;                               // 最低锁定层级
    // ... 其他标志位
};
```

##### 3.4.4.3 树结构

```
Btrfs 文件系统包含多个 B-Tree，通过 btrfs_fs_info 管理：

  btrfs_fs_info
  ├── tree_root      → 根树（存储所有子卷的根节点信息）
  ├── chunk_root     → 块组树（逻辑块→物理块映射）
  ├── dev_root       → 设备树
  ├── fs_root        → 文件系统根（子卷入口）
  ├── csum_root      → 校验和树
  ├── extent_root    → 扩展分配树（空闲空间管理）
  ├── quota_root     → 配额树
  ├── uuid_root      → UUID 树
  ├── block_group_root → 块组树
  └── log_root_tree  → 日志树

每个 B-Tree 由 btrfs_root 管理：
  struct btrfs_root {
      struct extent_buffer *node;     // 根节点（内存中）
      struct btrfs_root_item root_item; // 根信息（磁盘格式）
      struct btrfs_key root_key;       // 根键
      // ...
  };
```

##### 3.4.4.4 核心操作：`btrfs_search_slot`

`btrfs_search_slot` 是 Btrfs B-Tree 最核心的查找函数，它从根节点出发沿路径向下搜索，找到目标键应该插入的位置。

```
btrfs_search_slot(trans, root, key, path, ins_len, cow)
  │
  ├─ 1. 从 root->node 开始，level = root_level
  │
  ├─ 2. 对每个层级：
  │     ├─ 如果 cow（写时复制），调用 btrfs_cow_block() 复制当前节点
  │     │  └─ 分配新 extent_buffer，复制旧数据，更新父节点指针
  │     ├─ 在节点中执行二分查找（btrfs_bin_search）
  │     │  └─ 找到第一个大于等于 key 的槽位，存入 slots[level]
  │     └─ 如果 level > 0（非叶节点）：
  │        ├─ 设置 path->nodes[level] = 当前节点
  │        ├─ 通过 ptrs[slot].blockptr 获取子节点块地址
  │        └─ 下降到下一层：level--
  │
  ├─ 3. 到达叶节点（level = 0）：
  │     ├─ path->nodes[0] = 叶节点
  │     ├─ path->slots[0] = 目标槽位
  │     └─ 返回：
  │        0  = 精确匹配（key 已存在）
  │        1  = 未匹配（slot 指向第一个大于 key 的条目，即插入位置）
  │
  └─ 4. 如果 ins_len > 0（插入操作），节点可能分裂：
        └─ btrfs_insert_empty_item() → 如果空间不足 → btrfs_split_item()
           └─ 分裂后递归向上插入新键/指针
```

##### 3.4.4.5 COW 写时复制机制

```c
// btrfs_cow_block 是 COW 的核心实现
// 当需要修改一个节点时，不直接覆盖原块，而是：
//
// 1. 分配一个新的 extent_buffer
// 2. 复制原节点内容到新节点
// 3. 在新节点上修改（原节点保持不变）
// 4. 更新父节点中的指针，指向新节点
// 5. 原节点等待被垃圾回收（当再无引用时）

// 这个机制带来的特性：
// - 快照（Snapshot）：只需复制根节点指针，即可获得整个树的快照
// - 事务回滚：未提交的修改只影响新节点，原数据完整保留
// - 数据校验：每次写入都计算 checksum，保证数据完整性

// 涉及的关键函数：
int btrfs_cow_block(struct btrfs_trans_handle *trans,
                    struct btrfs_root *root,
                    struct extent_buffer *buf,        // 原节点
                    struct extent_buffer *parent,     // 父节点
                    int parent_slot,                  // 在父节点中的槽位
                    struct extent_buffer **cow_ret,   // 输出：新节点
                    enum btrfs_lock_nesting nest);
```

##### 3.4.4.6 节点分裂过程

```
叶节点分裂示例（假设节点容量为 4 个条目）：

分裂前（叶节点已满）：
  [item1][item2][item3][item4]  ← 满
  [data1][data2][data3][data4]

分裂后：
  左叶节点：          [item1][item2]
                      [data1][data2]
  右叶节点：          [item3][item4]
                      [data3][data4]
  父节点中插入新键：  [key(item3)] → 指向右叶节点

如果父节点也满了，递归向上分裂，最终可能增加树高度。
```

##### 3.4.4.7 关键参数

```c
// 节点大小 = 文件系统块大小（通常 4K 或 16K）
// 节点扇出（每个非叶节点可容纳的子节点指针数）：
BTRFS_NODEPTRS_PER_BLOCK(info) = BTRFS_LEAF_DATA_SIZE(info) / sizeof(struct btrfs_key_ptr)
// 4K 块时：约 (4096 - sizeof(btrfs_header)) / sizeof(btrfs_key_ptr) ≈ (4096-93)/29 ≈ 138
// 即每个内部节点可以指向约 138 个子节点

// 树高度计算：
// 扇出 138，如果根节点是叶节点 → 高度 1
// 137^2 ≈ 19000 个条目 → 高度 2
// 137^3 ≈ 260 万个条目 → 高度 3
// 137^4 ≈ 3.6 亿个条目 → 高度 4
// 一般文件系统树的层级为 2~4 层
```

#### 3.4.5 Ext4 Htree（哈希树目录索引）

##### 3.4.5.1 概述

Ext4 的 Htree（Hash Tree）是一种**哈希 B-Tree 变体**，专门用于目录项的高效查找。它将文件名通过哈希函数转为哈希值，然后以哈希值作为键在 B-Tree 中索引，实现大规模目录下的 O(log N) 查找。

##### 3.4.5.2 核心数据结构

```c
// fs/ext4/namei.c

// Htree 索引块（根节点或中间节点）
struct dx_root {
    struct fake_dirent fake;              // 伪装成目录项（'.' 和 '..'）
    struct dx_root_info {
        __le32 reserved_zero;
        __u8 hash_version;                // 哈希函数版本（TEA/SIPHASH 等）
        __u8 info_length;                 // 信息长度
        __u8 indirect_levels;             // 间接层数（0 或 1）
        __u8 unused_flags;
    } info;
    struct dx_entry entries[];            // 索引条目数组
};

// 索引条目（一个槽位）
struct dx_entry {
    __le32 hash;                          // 哈希值（键）
    __le32 block;                         // 逻辑块号（值）
};

// 帧：搜索路径中的一层
struct dx_frame {
    struct buffer_head *bh;               // 索引块的缓冲区头
    struct dx_entry *entries;             // 条目数组起始
    struct dx_entry *at;                  // 当前命中的条目
};

// 校验和尾部（位于每个索引块末尾）
struct dx_tail {
    __u32 dt_reserved;
    __le32 dt_checksum;                   // CRC32C 校验和
};

// 内部节点（非根中间节点）
struct dx_node {
    struct fake_dirent fake;
    struct dx_entry entries[];
};

// 哈希信息
struct dx_hash_info {
    __u32 hash;                           // 当前哈希值
    __u32 minor_hash;                     // 次要哈希（用于 64 位哈希）
    // ...
};
```

##### 3.4.5.3 树结构

```
Htree 目录索引结构（最多 2 层）：

根块（block 0）：
  [fake dirent '.' 和 '..'] [dx_root_info] [dx_entry...]
  │                                                  │
  └─────── 指向叶块（或内部节点） ──────────────────┘
              │
              ↓
        叶块（存放实际目录项）：
          [ext4_dir_entry_2...] [ext4_dir_entry_2...]
          按哈希值排序存储

大目录时（超过 1 个块）：
  根块（1 层索引）→ 内部节点（2 层索引）→ 叶块

  实际中 htree 的 indirect_levels 通常为 0 或 1
  最多支持 EXT4_HTREE_LEVEL = 2 层索引
```

##### 3.4.5.4 查找过程：`dx_probe`

```c
// ext4_dx_find_entry → dx_probe → 二分查找索引条目

// dx_probe 实现要点：
// 1. 计算文件名哈希值：hash = dx_hack_hash(name, len)
// 2. 在根块的 dx_entry 数组中用二分查找定位目标条目
// 3. 如果 indirect_levels > 0，递归向下查找
// 4. 最终找到目标叶块，在其中线性搜索目录项

// 二分查找的关键代码：
p = entries + 1;
q = entries + count - 1;
while (p <= q) {
    m = p + (q - p) / 2;
    if (dx_get_hash(m) > hash)
        q = m - 1;
    else
        p = m + 1;
}
at = p - 1;  // 找到第一个不大于 hash 的条目
// 通过 at->block 获取目标叶块号
```

##### 3.4.5.5 分裂过程

```c
// ext4_dx_add_entry 处理目录项插入

// 当叶块已满时：
// 1. 将叶块中的条目按哈希值分成两组
// 2. 分配新块，将后一半条目移到新块
// 3. 在父索引块中插入新 dx_entry（指向新块）
// 4. 如果父索引块也满，递归向上分裂
// 5. 如果根索引块也满，增加 indirect_levels

// 如果所有层级的索引块都满，且 indirect_levels 已达上限：
// 返回 -ENOSPC，认为目录太大
```

##### 3.4.5.6 哈希函数

```c
// Ext4 支持多种哈希版本：
// DX_HASH_LEGACY       — TEA 哈希（传统）
// DX_HASH_HALF_MD4     — 半 MD4
// DX_HASH_TEA          — TEA 扩展
// DX_HASH_SIPHASH      — SipHash（现代，防 hash-DoS）

// 哈希种子存储于超级块，防止被预测
```

#### 3.4.6 XFS B+Tree

##### 3.4.6.1 概述

XFS 使用了两种 B+Tree 实现：
1. **In-Core Extent B+Tree**（`xfs_iext_tree.c`）：内存中的 extent 映射管理，用于快速查找文件逻辑块到物理块的映射
2. **On-Disk B+Tree**（`xfs_btree*.c`）：通用的磁盘 B+Tree 框架，用于管理多种元数据（inode、空闲空间、引用计数等）

##### 3.4.6.2 In-Core Extent B+Tree

```c
// fs/xfs/libxfs/xfs_iext_tree.c

// 节点大小固定为 256 字节
#define NODE_SIZE       256
#define KEYS_PER_NODE   NODE_SIZE / (sizeof(uint64_t) + sizeof(void *))
#define RECS_PER_LEAF   (NODE_SIZE - 2 * sizeof(struct xfs_iext_leaf *)) / \
                        sizeof(struct xfs_iext_rec)

// 叶子节点：存储 extent 记录，形成双向链表
struct xfs_iext_leaf {
    struct xfs_iext_rec recs[RECS_PER_LEAF];  // extent 记录数组
    struct xfs_iext_leaf *prev;                // 前一个叶节点
    struct xfs_iext_leaf *next;                // 后一个叶节点
};

// 内部节点：存储键和指针
struct xfs_iext_node {
    uint64_t keys[KEYS_PER_NODE];              // 键数组
    void *ptrs[KEYS_PER_NODE];                 // 指针数组
};

// extent 记录：64 位低 + 64 位高，紧凑编码
struct xfs_iext_rec {
    uint64_t lo;    // 低 54 位 = startoff，高 10 位 = startblock 低 10 位
    uint64_t hi;    // 低 21 位 = length，位 21 = unwritten，高 42 位 = startblock 高 42 位
};

// 节点布局：
// 叶节点：  [rec1][rec2]...[recN][prev_ptr][next_ptr]
// 内部节点：[key1][key2]...[keyN][ptr1][ptr2]...[ptrN]
```

**查找过程**：

```
xfs_iext_lookup_extent(inode, ifp, offset, &irec)
  │
  ├─ 1. 从根节点开始
  ├─ 2. 如果是内部节点：
  │     └─ 线性扫描 keys，找到第一个大于等于 offset 的键
  │        └─ 进入对应的 ptrs 指向的子节点
  ├─ 3. 如果是叶节点：
  │     └─ 线性扫描 recs，找到匹配的 extent 记录
  └─ 4. 返回找到的 extent 映射
```

**插入/分裂**：

```
xfs_iext_insert(inode, &irec)
  │
  ├─ 1. 找到目标叶节点
  ├─ 2. 如果叶节点未满，插入记录，保持排序
  ├─ 3. 如果叶节点已满：
  │    └─ 分裂：分配新叶节点，将后半部分记录移到新节点
  │       └─ 在父节点中插入新键和指针
  │          └─ 递归向上，直到根节点
  └─ 4. 更新叶节点链表的 prev/next 指针
```

##### 3.4.6.3 On-Disk B+Tree 框架

XFS 的 On-Disk B+Tree 是一个**通用 B+Tree 框架**，通过函数指针表（`xfs_btree_ops`）抽象出各类型 B-Tree 的差异，使得同一套查找/插入/删除/分裂/合并代码可以服务于多种不同的元数据管理需求。

**磁盘块格式**

```c
// fs/xfs/libxfs/xfs_format.h

// 磁盘上的 B+Tree 块（每个块 = 文件系统块大小，通常 4K）
struct xfs_btree_block {
    __be32  bb_magic;      // 魔数（标识 B-Tree 类型，如 BMBT、ABTB、IBTB 等）
    __be16  bb_level;      // 层级（0 = 叶节点）
    __be16  bb_numrecs;    // 当前条目数

    union {
        // short form（32 位指针，用于 AG 内 B-Tree）
        struct xfs_btree_block_shdr {
            __be32  bb_leftsib;    // 左兄弟块
            __be32  bb_rightsib;   // 右兄弟块
            __be64  bb_blkno;      // 块编号（用于 CRC 校验）
            __be64  bb_lsn;        // 日志序列号
            uuid_t  bb_uuid;       // 文件系统 UUID
            __be32  bb_owner;      // 所属 AG 编号
            __le32  bb_crc;        // CRC32C 校验和
        } s;

        // long form（64 位指针，用于文件内 B-Tree 如 BMBT）
        struct xfs_btree_block_lhdr {
            __be64  bb_leftsib;    // 左兄弟块
            __be64  bb_rightsib;   // 右兄弟块
            __be64  bb_blkno;
            __be64  bb_lsn;
            uuid_t  bb_uuid;
            __be64  bb_owner;      // 所属 inode 编号
            __le32  bb_crc;
            __be32  bb_pad;        // 对齐填充
        } l;
    } bb_u;
};

// 块布局：
// +------------------+  ← 0
// | xfs_btree_block  |  块头（固定大小，short form ~56B，long form ~72B）
// +------------------+
// | keys[0..N-1]     |  键数组（从低地址到高地址递增）
// +------------------+
// | ptrs[0..N-1]     |  指针数组（内部节点）或
// | recs[0..N-1]     |  记录数组（叶节点）
// +------------------+  ← 块大小（通常 4K）
```

**Cursor 机制**

```c
// fs/xfs/libxfs/xfs_btree.h

// 每层上下文
struct xfs_btree_level {
    struct xfs_buf  *bp;       // 缓冲区指针（磁盘块在内存中的缓存）
    uint16_t        ptr;       // 当前槽位号（1-based，指向当前正在处理的键/记录）
    uint16_t        ra;        // 预读标志
};

// B-Tree 游标：封装一次查找/插入/删除操作的完整路径
struct xfs_btree_cur {
    struct xfs_trans            *bc_tp;        // 当前事务
    struct xfs_mount            *bc_mp;        // 文件系统挂载点
    const struct xfs_btree_ops  *bc_ops;       // 类型相关操作函数表
    unsigned int                bc_flags;      // 标志位
    union xfs_btree_irec        bc_rec;        // 当前查找/插入的记录值
    uint8_t                     bc_nlevels;    // 当前树的高度
    uint8_t                     bc_maxlevels;  // 最大允许高度

    // 类型相关信息
    union {
        // 文件 B-Tree（BMBT）：与 inode 关联
        struct {
            struct xfs_inode    *ip;
            short               forksize;
            char                whichfork;
        } bc_ino;
        // AG B-Tree（AGBT/IOBT/RMAP/REFC）：与 AG 关联
        struct {
            struct xfs_buf      *agbp;         // AG 头缓冲区
        } bc_ag;
    };

    // 每层状态（变长数组，按树高度分配）
    struct xfs_btree_level  bc_levels[];       // 从根到叶的完整路径
};

// 操作函数表（约 20 个函数指针，抽象出类型差异）
struct xfs_btree_ops {
    // 块读取与写入
    int     (*get_block)(struct xfs_btree_cur *cur, union xfs_btree_ptr *ptr,
                         struct xfs_btree_block **block, struct xfs_buf **bpp);
    void    (*init_ptr_from_cur)(struct xfs_btree_cur *cur,
                                 union xfs_btree_ptr *ptr);
    // 键/记录比较
    int     (*key_diff)(struct xfs_btree_cur *cur, union xfs_btree_key *key);
    int     (*recs_inorder)(struct xfs_btree_cur *cur,
                            const union xfs_btree_rec *r1,
                            const union xfs_btree_rec *r2);
    // 键/记录/指针 的获取/设置
    union xfs_btree_key *(*get_keys)(struct xfs_btree_cur *cur,
                            struct xfs_btree_block *block, int n);
    union xfs_btree_rec *(*get_recs)(struct xfs_btree_cur *cur,
                            struct xfs_btree_block *block, int n);
    // 块分配与释放
    int     (*alloc_block)(struct xfs_btree_cur *cur,
                           union xfs_btree_ptr *start,
                           union xfs_btree_ptr *new, int *stat);
    // 更新父节点键
    void    (*update_cursor)(struct xfs_btree_cur *src,
                             struct xfs_btree_cur *dst);
    // ...
};
```

**查找操作：`xfs_btree_lookup`**

```
xfs_btree_lookup(cur, dir, &stat)
  // dir: XFS_LOOKUP_EQ (=), XFS_LOOKUP_LE (≤), XFS_LOOKUP_GE (≥)
  │
  ├─ 1. 从根节点开始（level = bc_nlevels - 1）
  │
  ├─ 2. 对每个层级：
  │     ├─ xfs_btree_lookup_get_block(cur, level, pp, &block)
  │     │  └─ 从磁盘读取块到缓冲区（或从缓存获取）
  │     │
  │     ├─ while (low <= high) {   // 二分查找
  │     │     keyno = (low + high) >> 1;
  │     │     kp = ops->get_keys(cur, block, keyno);
  │     │     cmp_r = ops->key_diff(cur, kp);  // 比较键
  │     │     if (cmp_r > 0)      // 目标键 > 当前键 → 向右
  │     │         low = keyno + 1;
  │     │     else if (cmp_r < 0) // 目标键 < 当前键 → 向左
  │     │         high = keyno - 1;
  │     │     else                // 相等
  │     │         break;
  │     │ }
  │     │
  │     ├─ 设置 cur->bc_levels[level].ptr = keyno
  │     │
  │     └─ 如果 level > 0（非叶节点）：
  │         └─ 通过 ptrs[keyno] 获取子节点指针
  │            └─ level--，继续下一层
  │
  └─ 3. 到达叶节点（level = 0）：
        └─ cur->bc_levels[0].ptr 指向目标记录位置
        └─ *stat = 1（找到）或 0（未找到）
```

**插入操作：`xfs_btree_insert`**

```
xfs_btree_insert(cur, &stat)
  │
  ├─ 1. xfs_btree_lookup(cur, XFS_LOOKUP_LE, &stat)
  │     └─ 找到插入位置（叶节点中第一个 ≤ 目标键的槽位 + 1）
  │
  ├─ 2. xfs_btree_insrec(cur, 0, &ptr, &rec, &key, &ncur, &stat)
  │     ├─ 获取叶节点块
  │     ├─ 检查是否已满（numrecs == maxrecs）
  │     │  ├─ 未满 → 将条目右移，插入新记录
  │     │  └─ 已满 → xfs_btree_make_block_unfull()
  │     │     └─ 触发分裂（__xfs_btree_split）
  │     │
  │     └─ 插入记录到叶节点
  │
  └─ 3. 如果出现了分裂，递归向上插入新键/指针
        └─ 如果到达根节点且根节点也满 → xfs_btree_new_root()
           └─ 分配新根节点，树高度 +1
```

**分裂过程：`__xfs_btree_split`**

```
__xfs_btree_split(cur, level, &nptr, &nkey, &ncur, &stat)
  │
  ├─ 1. 获取当前块（左块）和缓冲区
  │
  ├─ 2. xfs_btree_alloc_block(cur, &lptr, &rptr, &stat)
  │     └─ 分配新块（右块），通过 ops->alloc_block 回调
  │
  ├─ 3. 计算分裂点：
  │     lrecs = xfs_btree_get_numrecs(left);  // 当前条目数
  │     rrecs = lrecs / 2;                     // 右块条目数
  │     if ((lrecs & 1) && cur->ptr <= rrecs + 1)
  │         rrecs++;  // 确保插入位置在正确的一侧
  │     src_index = (lrecs - rrecs + 1);       // 复制起始位置
  │
  ├─ 4. 将左块后半部分复制到右块：
  │     ├─ 内部节点：复制 keys + ptrs
  │     └─ 叶节点：复制 recs
  │
  ├─ 5. 更新左右块的兄弟指针：
  │     right->bb_rightsib = left->bb_rightsib;
  │     left->bb_rightsib = right 的块号;
  │     right->bb_leftsib = left 的块号;
  │
  ├─ 6. 更新磁盘（标记缓冲区脏）
  │
  └─ 7. 向上递归：
        └─ 在父节点中插入新键（右块第一个键）和指针（右块块号）
           └─ 如果父节点也满 → 继续分裂，递归向上
```

**删除操作：`xfs_btree_delete`**

```
xfs_btree_delete(cur, &stat)
  │
  ├─ 1. for (level = 0, i = 2; i == 2; level++)
  │     └─ xfs_btree_delrec(cur, level, &i)
  │        ├─ 从叶节点删除指定槽位的记录
  │        ├─ 检查是否需要合并（条目数低于阈值）
  │        │  ├─ 尝试从兄弟节点借用
  │        │  │  └─ 从兄弟节点移动一个条目到当前节点
  │        │  └─ 如果兄弟节点也不够 → 合并两个节点
  │        │     └─ 释放一个兄弟块
  │        │     └─ 返回 2（需要继续处理上层）
  │        └─ 如果不需要合并 → 返回 0（完成）
  │
  ├─ 2. 如果发生了合并，更新父节点的高键（overlapping B-Tree 需要）
  │
  └─ 3. 如果根节点条目数为 0 → 释放根节点，树高度 -1
```

**XFS On-Disk B+Tree 的类型支持**

| B-Tree 类型 | 指针格式 | 用途 | 每个 AG | 关键键 |
|-------------|----------|------|---------|--------|
| **BMBT** (Block Map Tree) | Long (64-bit) | 文件 extent 映射（逻辑块→物理块） | 每个文件一个 | (startoff, blockcount) |
| **AGBT** (Allocation Group B-Tree) | Short (32-bit) | 空闲空间管理（按块号和大小） | 2 棵（bno+cnt） | (blockno, length) |
| **IOBT** (Inode B-Tree) | Short (32-bit) | inode 分配管理 | 1 棵 | (inode #) |
| **RMAP** (Reverse Mapping B-Tree) | Short (32-bit) | 反向映射（物理块→所有者） | 1 棵 | (blockno, owner, offset) |
| **REFC** (Reference Count B-Tree) | Short (32-bit) | 引用计数（reflink/dedupe） | 1 棵 | (blockno, refcount) |

**B-Tree 类型专用操作函数示例**

```c
// fs/xfs/libxfs/xfs_alloc.c — 空闲空间 B-Tree 的查找封装

// 查找第一个 ≥ [bno, len] 的记录
int xfs_alloc_lookup_ge(struct xfs_btree_cur *cur,
                        xfs_agblock_t bno, xfs_extlen_t len, int *stat)
{
    cur->bc_rec.a.ar_startblock = bno;
    cur->bc_rec.a.ar_blockcount = len;
    return xfs_btree_lookup(cur, XFS_LOOKUP_GE, stat);
}

// 查找第一个 ≤ [bno, len] 的记录
int xfs_alloc_lookup_le(struct xfs_btree_cur *cur,
                        xfs_agblock_t bno, xfs_extlen_t len, int *stat)
{
    cur->bc_rec.a.ar_startblock = bno;
    cur->bc_rec.a.ar_blockcount = len;
    return xfs_btree_lookup(cur, XFS_LOOKUP_LE, stat);
}

// 精确查找
int xfs_alloc_lookup_eq(struct xfs_btree_cur *cur,
                        xfs_agblock_t bno, xfs_extlen_t len, int *stat)
{
    cur->bc_rec.a.ar_startblock = bno;
    cur->bc_rec.a.ar_blockcount = len;
    return xfs_btree_lookup(cur, XFS_LOOKUP_LE, stat);
}

// 空闲空间分配中的典型操作序列：
// 1. xfs_alloc_lookup_ge(cnt_cur, needed, 1, &i)  // 按大小查找
// 2. xfs_btree_get_rec(cnt_cur, &rec, &i)         // 获取记录
// 3. xfs_btree_delete(cnt_cur, &i)                // 删除原记录
// 4. 如果剩余空间 > 0 → xfs_btree_insert(cnt_cur, &i)  // 插入剩余部分
```

#### 3.4.7 内核 B-Tree 变体的实际调用栈

##### 3.4.7.1 Btrfs 文件读取路径

```
read() → sys_read() → vfs_read() → btrfs_file_read_iter()
  │
  └─ btrfs_buffered_read() → btrfs_get_blocks_snap()
       │
       ├─ btrfs_lookup_file_extent()           // 查找文件 extent
       │    └─ btrfs_search_slot()              // B-Tree 查找
       │         ├─ btrfs_cow_block()           // COW 复制（写入时）
       │         ├─ btrfs_bin_search()          // 二分查找
       │         └─ 沿路径下降至叶节点
       │
       ├─ btrfs_map_block()                     // 逻辑块→物理块映射
       └─ submit_extent_page()                  // 提交 IO
```

##### 3.4.7.2 Btrfs 文件写入路径

```
write() → sys_write() → vfs_write() → btrfs_file_write_iter()
  │
  └─ btrfs_buffered_write() → btrfs_prealloc_file_range()
       │
       ├─ btrfs_insert_reserved_file_extent()   // 插入 extent 记录
       │    ├─ btrfs_search_slot(ins_len > 0)    // 查找插入位置（COW 模式）
       │    ├─ btrfs_insert_empty_item()         // 插入空条目
       │    │    └─ btrfs_search_slot() → 节点分裂
       │    └─ btrfs_setup_item_for_insert()     // 填充条目数据
       │
       ├─ btrfs_finish_ordered_io()             // 完成有序 IO
       │    └─ btrfs_search_slot() → btrfs_del_item()  // 更新校验和树
       │
       └─ btrfs_log_inode_parent()              // 日志记录
            └─ btrfs_search_slot() → 日志树操作
```

##### 3.4.7.3 Ext4 目录查找路径

```
open() → sys_open() → path_openat() → link_path_walk()
  │
  └─ ext4_lookup() → ext4_find_entry()
       │
       ├─ ext4_dx_find_entry()                  // Htree 查找
       │    ├─ dx_probe()                        // 二分查找索引块
       │    │    ├─ dx_hack_hash()               // 计算文件名哈希
       │    │    └─ 二分查找 dx_entry 数组
       │    │
       │    ├─ ext4_dx_find_entry()              // 在目标叶块中线性搜索
       │    │    └─ ext4_search_dir()            // 搜索实际目录项
       │    │
       │    └─ dx_release()                      // 释放索引帧
       │
       └─ [如果目录很小，使用传统线性搜索]
            └─ ext4_search_dir()                 // 块内线性搜索
```

##### 3.4.7.4 XFS 文件读取路径（In-Core Extent + On-Disk BMBT）

```
read() → sys_read() → vfs_read() → xfs_file_read_iter()
  │
  └─ xfs_file_buffered_read() → xfs_read_buftarg()
       │
       ├─ xfs_bmapi_read()                       // 逻辑块→物理块映射
       │    ├─ xfs_iext_lookup_extent()           // 内存中 In-Core B+Tree 查找
       │    │    └─ xfs_iext_find_level()         // 从根节点逐层下降
       │    │
       │    ├─ [如果内存中 extent 不在缓存中]
       │    │    └─ xfs_bmbt_read_extent()        // 从磁盘 BMBT 读取
       │    │         └─ xfs_btree_lookup()        // 磁盘 B+Tree 查找
       │    │
       │    └─ xfs_iext_insert()                  // 将磁盘读取的 extent 缓存到内存中
       │
       └─ iomap_readpage()                       // 提交 IO
```

##### 3.4.7.5 XFS 空闲空间分配路径（AGBT）

```
xfs_alloc_fix_freelist() → xfs_alloc_ag_vextent()
  │
  ├─ [按大小查找]
  │    ├─ xfs_alloc_lookup_ge(cnt_cur, needed, 1, &i)  // AGBT by-size 树查找
  │    ├─ xfs_btree_get_rec(cnt_cur, &rec, &i)         // 获取记录
  │    └─ xfs_btree_delete(cnt_cur, &i)                // 删除原记录
  │
  ├─ [按块号更新]
  │    ├─ xfs_alloc_lookup_eq(bno_cur, block, len, &i) // AGBT by-block 树查找
  │    └─ xfs_btree_delete(bno_cur, &i)                // 删除原记录
  │
  ├─ [插入剩余空间]
  │    └─ xfs_btree_insert(cnt_cur, &i)                // 将剩余空间插回 by-size 树
  │
  └─ [更新 AG 头]
       └─ xfs_alloc_update_agf()                       // 更新 AGF（AG 空闲空间头）
```

#### 3.4.8 各变体对比总结

| 维度 | 通用 B+Tree (`lib/btree.c`) | Btrfs COW B-Tree | Ext4 Htree | XFS In-Core Extent | XFS On-Disk B+Tree |
|------|------|------|------|------|------|
| **用途** | 通用内存查找 | 文件系统元数据 | 目录索引 | 内存中 extent 映射 | 通用磁盘元数据 |
| **存储** | 纯内存 | 磁盘 + 内存 | 磁盘 + 内存 | 纯内存 | 磁盘 + 内存 |
| **写时复制** | 否 | 是（核心特性） | 否 | 否 | 否 |
| **节点大小** | 128 字节 | 4K~64K（块大小） | 4K（块大小） | 256 字节 | 4K（块大小） |
| **扇出** | 4~8（小扇出） | ~138（大扇出） | ~340 个条目/块 | 21 个条目/叶节点 | ~500+（大扇出） |
| **查找方式** | 线性扫描 | 二分查找 | 二分查找 | 线性扫描 | 二分查找 |
| **键类型** | u32/u64/u128 | 三元组 (objectid, type, offset) | 哈希值 | 文件 offset | 多种（BMBT/AGBT/IOBT/RMAP/REFC） |
| **节点分裂** | 等分键值对 | 等分条目 | 按哈希值等分 | 等分记录 | 等分条目（保证平衡） |
| **事务支持** | 无 | 完整事务 + 日志 | 日志 (jbd2) | 无 | 完整事务 + 日志 |
| **快照/回滚** | 无 | 原生支持 | 不支持 | 不支持 | 不支持 |
| **并发控制** | 调用者负责 | 节点级锁 + 路径锁 | 缓冲区锁 | 调用者负责 | 事务 + 缓冲区锁 |
| **复杂度** | 低 | 高 | 中 | 中 | 高 |
| **代码行数** | ~500 | ~10000+ | ~1000 | ~1000 | ~5000+ |

**B-Tree 与红黑树的选择权衡**

| 场景 | 推荐结构 | 原因 |
|------|----------|------|
| 内存中少量元素（<1000） | 红黑树 / B+Tree | 红黑树实现简单，B-Tree 小扇出优势不明显 |
| 内存中大量元素 | B+Tree（小节点） | 缓存局部性更好，访存次数更少 |
| 磁盘存储（元数据） | B+Tree（大节点） | 高扇出减少 I/O 次数，顺序访问高效 |
| 频繁插入/删除 | 红黑树 | B-Tree 分裂/合并开销大 |
| 范围查询 | B+Tree | 叶节点链表，顺序扫描高效 |
| 写时复制快照 | Btrfs COW B-Tree | 专为 COW 优化 |
| 通用磁盘元数据 | XFS On-Disk B+Tree | 通用框架，支持多种类型 |
| 哈希索引 | Ext4 Htree | 哈希 B-Tree，O(log N) 目录查找 |

---

## 4. 哈希表

### 4.1 概述

哈希表（Hash Table）通过哈希函数将键映射到桶（bucket）中，实现近乎 O(1) 的查找、插入和删除操作。内核中哈希表的实现基于 `hlist_head`（见 2.2 节），桶数组通常静态分配，每个桶是一个哈希链表，用于处理哈希冲突。

**内核实现 vs 传统哈希表实现的差异**

| 特性 | 内核实现（拉链法 + `hlist_head`） | 传统实现（如开放地址法） |
|---|---|---|
| 冲突解决 | 拉链法（`hlist_head` 链表） | 开放地址法（线性探测、二次探测等） |
| 内存分配 | 哈希表条目动态分配，`hlist_node` 嵌入 | 所有条目存储在桶数组本身 |
| 删除操作 | 简单（`hlist_del`），O(1) | 复杂（需要处理探测链断裂），需要标记删除 |
| 扩容 | 通常不自动扩容（桶大小在编译时确定） | 负载因子达到阈值时自动扩容 |
| 缓存局部性 | 差（链表节点分散在内存中） | 好（连续存储，遍历桶时缓存命中率高） |
| 缺点 | 链表节点分散，缓存不友好，需要额外内存分配 | 删除复杂，扩容开销大，负载因子过高时性能急剧下降 |

### 4.2 哈希函数

内核提供多种哈希函数，适用于不同场景：

```c
// include/linux/jhash.h — Jenkins 哈希（通用、平台无关）
u32 jhash(const void *key, u32 length, u32 initval);
u32 jhash2(const u32 *k, u32 length, u32 initval);
u32 jhash_1word(u32 a, u32 initval);
u32 jhash_2words(u32 a, u32 b, u32 initval);
u32 jhash_3words(u32 a, u32 b, u32 c, u32 initval);

// include/linux/hash.h — 整型哈希（快速）
u32 hash_32(u32 val, unsigned int bits);
u64 hash_64(u64 val, unsigned int bits);

// include/linux/cryptohash.h — 加密级哈希
// 用于需要防 hashdos 攻击的场景

// 辅助宏：将哈希值压缩到指定位数
#define hash_min(val, bits) (sizeof(val) <= 4 ? hash_32(val, bits) : hash_64(val, bits))
```

### 4.3 完整 API 参考

| 宏/函数 | 功能 |
|---|---|
| `DEFINE_HASHTABLE(name, bits)` | 静态定义哈希表 |
| `DECLARE_HASHTABLE(name, bits)` | 声明哈希表 |
| `hash_init(htable)` | 初始化哈希表 |
| `hash_add(htable, node, key)` | 插入节点 |
| `hash_del(node)` | 删除节点 |
| `hash_for_each(htable, bkt, obj, member)` | 遍历所有桶 |
| `hash_for_each_safe(htable, bkt, tmp, obj, member)` | 安全遍历 |
| `hash_for_each_possible(htable, obj, member, key)` | 在指定桶中遍历 |
| `hash_for_each_possible_safe(htable, obj, tmp, member, key)` | 安全遍历指定桶 |
| `hash_for_each_key(htable, obj, member, key)` | 遍历指定键的桶 |

### 4.4 完整使用示例

```c
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/jhash.h>

// 定义哈希表（256 个桶，bits=8）
DEFINE_HASHTABLE(my_ht, 8);

// 哈希表条目结构体
struct my_entry {
    u32 id;
    char data[64];
    struct hlist_node node;  // 哈希链
};

// 插入
int ht_insert(u32 id, const char *data)
{
    struct my_entry *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return -ENOMEM;
    entry->id = id;
    strncpy(entry->data, data, sizeof(entry->data) - 1);
    hash_add(my_ht, &entry->node, id);  // 直接以 id 为键
    return 0;
}

// 查找
struct my_entry *ht_lookup(u32 id)
{
    struct my_entry *entry;
    hash_for_each_possible(my_ht, entry, node, id) {
        if (entry->id == id)
            return entry;
    }
    return NULL;
}

// 删除
void ht_remove(u32 id)
{
    struct my_entry *entry;
    struct hlist_node *tmp;
    hash_for_each_possible_safe(my_ht, entry, tmp, node, id) {
        if (entry->id == id) {
            hash_del(&entry->node);
            kfree(entry);
            return;
        }
    }
}

// 遍历整个哈希表
void ht_dump(void)
{
    struct my_entry *entry;
    int bkt;

    hash_for_each(my_ht, bkt, entry, node) {
        pr_info("bucket %d: id=%u, data=%s\n", bkt, entry->id, entry->data);
    }
}
```

### 4.5 应用场景

- **网络连接跟踪 (conntrack)**：用哈希表管理海量网络连接状态，以五元组为键快速查找
- **文件系统 dentry 缓存**：通过哈希快速定位目录项，避免遍历目录
- **PID 查找**：`pid_hash[]` 数组，根据 PID 快速找到对应 `task_struct`
- **内存管理页缓存**：通过文件索引的哈希值快速定位缓存页
- **设备号管理**：`dev_name_hash` 用于设备查找

---

## 5. 堆与优先队列

### 5.1 实时调度优先级队列

#### 5.1.1 概述

实时调度器（`SCHED_FIFO` / `SCHED_RR`）为每个优先级（0~99）维护一个运行队列，整体上构成一个基于优先级的调度结构。

#### 5.1.2 核心数据结构

```c
// kernel/sched/sched.h
struct rt_prio_array {
    DECLARE_BITMAP(bitmap, MAX_RT_PRIO+1);  // 位图标记非空队列
    struct list_head queue[MAX_RT_PRIO];     // 每个优先级一个链表
};
```

#### 5.1.3 操作流程

```
选择最高优先级进程：
  1. 使用 sched_find_first_bit() 在位图中找到第一个置位位
  2. 对应 queue[prio] 链表头部即为下一个要运行的进程
  3. 时间复杂度：O(1)（位图扫描由硬件指令加速）
```

#### 5.1.4 应用场景

- **实时任务调度**：`SCHED_FIFO`（先入先出，直到被更高优先级抢占或主动让出）
- **时间片轮转**：`SCHED_RR`（在 FIFO 基础上增加时间片轮转）
- **中断处理线程**：`irq/xxx` 内核线程以高实时优先级运行

---

### 5.2 定时器时间轮

#### 5.2.1 概述

内核定时器子系统使用**时间轮（Timing Wheel）**算法高效管理海量定时器。时间轮将时间划分为多个层级，每个层级是一个环形桶数组，定时器根据超时时间插入对应层级，到期时逐级"级联"（cascade）。

**内核分级时间轮 vs 简单时间轮（不分级）的差异**

| 特性 | 内核分级时间轮 | 简单时间轮（不分级） |
|---|---|---|
| 定时器容量 | 无上限（通过级联扩展到任意超时时间） | 受限于桶数组大小 |
| 插入复杂度 | O(1) | O(1) |
| 到期处理 | 每个 tick O(1)（摊销），级联偶尔 O(n) | 每个 tick O(1) |
| 精度 | 恒定（1 tick） | 随超时时间增加而降低（除非增加桶大小） |
| 内存占用 | 固定大小（4 × 64 个桶），不随定时器数量增加 | 需要大量桶来支持长超时时间 |
| 支持超时范围 | 任意大（通过级联上级桶） | 受限于桶数组大小（如 64 个桶只能支持 64 ticks） |
| 优点 | 内存占用固定，可支持任意超时时间，精度恒定 | 实现简单，无级联开销 |
| 缺点 | 实现复杂，级联操作有瞬时开销（摊销 O(1)） | 范围受限，内存和精度不可兼得 |

#### 5.2.2 核心数据结构

```c
// kernel/time/timer.c
struct timer_base {
    spinlock_t      lock;
    struct list_head pending_map[WHEEL_SIZE];  // 每个桶是一个链表
    unsigned long   clk;                        // 当前时钟节拍
    int             pending_idx;                // 当前处理位置
    ...
};

// 时间轮层级（旧版实现，现代的层级数可能不同）
#define LVL_DEPTH 4          // 4 级时间轮
#define LVL_BITS  6          // 每级 6 位（64 个桶）
#define WHEEL_SIZE (1 << LVL_BITS)  // 64
```

#### 5.2.3 原理细节：级联（Cascade）机制

时间轮的核心思想是通过**分级+级联**在 O(1) 时间内完成定时器的插入和到期处理：

**插入算法**

```
定时器超时时间 = expires，当前时间 = clk
delta = expires - clk

if delta < 64:        → 插入 Level 0，桶索引 = delta
if delta < 4096:      → 插入 Level 1，桶索引 = delta >> 6
if delta < 262144:    → 插入 Level 2，桶索引 = delta >> 12
else:                 → 插入 Level 3，桶索引 = delta >> 18
```

**级联过程**

```
每个 tick 处理 Level 0 的当前桶：
  1. 遍历该桶中的所有定时器，执行回调
  2. 如果 Level 0 的一个桶处理完（即指针绕了一圈）
     → 从 Level 1 取出对应桶的定时器
     → 根据各定时器的剩余时间重新插入到 Level 0-2 的合适位置
  3. 如果 Level 1 的一个桶也处理完 → 级联 Level 2，以此类推
```

**复杂度分析**

| 操作 | 时间复杂度 | 说明 |
|---|---|---|
| 插入定时器 | O(1) | 直接计算桶索引，插入链表头部 |
| 到期处理（每个 tick） | O(1) avg | 多数情况只需处理 Level 0 的一个桶 |
| 级联（偶尔发生） | O(n) amortized | 每次级联将 N 个定时器重新分发，但分摊到每个 tick 为 O(1) |

#### 5.2.4 核心 API

```c
// include/linux/timer.h

// 定时器结构体
struct timer_list {
    struct list_head entry;          // 在时间轮桶中的链表节点
    unsigned long expires;           // 超时时间（jiffies）
    void (*function)(struct timer_list *);  // 回调函数
    u32 flags;                       // 标志位
    ...
};

// 初始化
void timer_setup(struct timer_list *timer,
                 void (*callback)(struct timer_list *), u32 flags);
#define TIMER_INITIALIZER(_function, _flags)  // 静态初始化

// 添加定时器
void add_timer(struct timer_list *timer);
void add_timer_on(struct timer_list *timer, int cpu);  // 指定 CPU
int mod_timer(struct timer_list *timer, unsigned long expires);  // 修改超时时间

// 删除定时器
int del_timer(struct timer_list *timer);
int del_timer_sync(struct timer_list *timer);  // 等待其他 CPU 完成

// 定时器待处理状态
int timer_pending(const struct timer_list *timer);
```

#### 5.2.5 完整使用示例

```c
#include <linux/timer.h>
#include <linux/module.h>
#include <linux/printk.h>

// 定时器回调函数
static void my_timer_callback(struct timer_list *t)
{
    pr_info("timer expired at jiffies=%lu\n", jiffies);
}

// 定义定时器
static struct timer_list my_timer;

// 启动定时器
void start_my_timer(unsigned long delay_jiffies)
{
    // 初始化定时器
    timer_setup(&my_timer, my_timer_callback, 0);

    // 设置超时时间并添加
    my_timer.expires = jiffies + delay_jiffies;
    add_timer(&my_timer);

    pr_info("timer started, will expire at jiffies=%lu\n",
            my_timer.expires);
}

// 修改定时器（在到期前修改超时时间）
void reschedule_my_timer(unsigned long new_delay_jiffies)
{
    mod_timer(&my_timer, jiffies + new_delay_jiffies);
    pr_info("timer rescheduled to jiffies=%lu\n", my_timer.expires);
}

// 停止定时器
void stop_my_timer(void)
{
    int ret = del_timer_sync(&my_timer);  // 确保其他 CPU 上的回调已完成
    if (ret)
        pr_info("timer was pending and deleted\n");
    else
        pr_info("timer was not pending\n");
}
```

#### 5.2.6 应用场景

- **内核定时器**：`timer_list`、`hrtimer` 等定时器机制
- **TCP 超时**：重传定时器、保活定时器
- **调度器**：时间片调度、负载均衡定时

---

## 6. 查找与索引结构

### 6.1 位图 (bitmap)

#### 6.1.1 概述

位图是内核中广泛使用的空间高效数据结构，用于表示大量二进制状态。其核心操作包括置位、清位、测试位以及位扫描等，时间复杂度为 O(1) 或 O(n/bits-per-word)。

#### 6.1.2 核心操作

```c
// include/linux/bitmap.h
// 声明位图
DECLARE_BITMAP(name, bits);  // 展开为 unsigned long name[BITS_TO_LONGS(bits)]

// 清零
void bitmap_zero(unsigned long *dst, unsigned int nbits);

// 全部置1
void bitmap_fill(unsigned long *dst, unsigned int nbits);

// 位操作
void set_bit(unsigned int nr, volatile unsigned long *p);
void clear_bit(unsigned int nr, volatile unsigned long *p);
int test_bit(unsigned int nr, const volatile unsigned long *p);

// 查找第一个置位/清零位
unsigned long find_first_bit(const unsigned long *addr, unsigned long size);
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size);
unsigned long find_next_bit(const unsigned long *addr, unsigned long size,
                            unsigned long offset);
unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size,
                                 unsigned long offset);

// 位图运算
void bitmap_and(unsigned long *dst, const unsigned long *src1,
                const unsigned long *src2, unsigned int nbits);
void bitmap_or(unsigned long *dst, const unsigned long *src1,
               const unsigned long *src2, unsigned int nbits);
void bitmap_xor(unsigned long *dst, const unsigned long *src1,
                const unsigned long *src2, unsigned int nbits);
```

#### 6.1.3 完整 API 参考

| 函数/宏 | 功能 |
|---|---|
| `DECLARE_BITMAP(name, bits)` | 声明位图 |
| `bitmap_zero(dst, nbits)` | 全部清零 |
| `bitmap_fill(dst, nbits)` | 全部置 1 |
| `bitmap_copy(dst, src, nbits)` | 复制位图 |
| `bitmap_set(dst, start, nbits)` | 从 start 开始置 nbits 个 1 |
| `bitmap_clear(dst, start, nbits)` | 从 start 开始清零 nbits 个 |
| `bitmap_shift_right(dst, src, shift, nbits)` | 右移 |
| `bitmap_shift_left(dst, src, shift, nbits)` | 左移 |
| `bitmap_and(dst, src1, src2, nbits)` | 与运算 |
| `bitmap_or(dst, src1, src2, nbits)` | 或运算 |
| `bitmap_xor(dst, src1, src2, nbits)` | 异或运算 |
| `bitmap_andnot(dst, src1, src2, nbits)` | A & ~B |
| `bitmap_complement(dst, src, nbits)` | 取反 |
| `bitmap_weight(src, nbits)` | 统计置位个数 |
| `bitmap_empty(src, nbits)` | 判断是否全部清零 |
| `bitmap_full(src, nbits)` | 判断是否全部置 1 |
| `bitmap_equal(a, b, nbits)` | 判断是否相等 |
| `bitmap_intersects(a, b, nbits)` | 判断是否有交集 |
| `bitmap_subset(a, b, nbits)` | 判断 a 是否为 b 的子集 |
| `bitmap_find_free_region(bitmap, nbits, order)` | 查找空闲区域 |
| `bitmap_allocate_region(bitmap, pos, order)` | 分配区域 |
| `bitmap_release_region(bitmap, pos, order)` | 释放区域 |
| `set_bit(nr, addr)` | 原子置位 |
| `clear_bit(nr, addr)` | 原子清位 |
| `change_bit(nr, addr)` | 原子翻转 |
| `test_bit(nr, addr)` | 测试位 |
| `test_and_set_bit(nr, addr)` | 原子置位并返回旧值 |
| `test_and_clear_bit(nr, addr)` | 原子清位并返回旧值 |
| `find_first_bit(addr, size)` | 找到第一个置位位 |
| `find_first_zero_bit(addr, size)` | 找到第一个清零位 |
| `find_next_bit(addr, size, offset)` | 找到下一个置位位 |
| `find_next_zero_bit(addr, size, offset)` | 找到下一个清零位 |
| `find_last_bit(addr, size)` | 找到最后一个置位位 |
| `for_each_set_bit(bit, addr, size)` | 遍历所有置位位 |
| `for_each_clear_bit(bit, addr, size)` | 遍历所有清零位 |
| `for_each_set_bit_from(bit, addr, size)` | 从指定位置开始遍历 |

#### 6.1.4 完整使用示例

```c
#include <linux/bitmap.h>
#include <linux/printk.h>

// 声明位图
DECLARE_BITMAP(my_bitmap, 128);  // 128 位

void bitmap_demo(void)
{
    // 全部清零
    bitmap_zero(my_bitmap, 128);
    pr_info("empty: %d\n", bitmap_empty(my_bitmap, 128));  // 1

    // 置位 0, 10, 20, 30
    set_bit(0, my_bitmap);
    set_bit(10, my_bitmap);
    set_bit(20, my_bitmap);
    set_bit(30, my_bitmap);
    pr_info("weight: %d\n", bitmap_weight(my_bitmap, 128));  // 4

    // 遍历所有置位位
    int bit;
    pr_info("set bits:");
    for_each_set_bit(bit, my_bitmap, 128) {
        pr_cont(" %d", bit);
    }
    pr_cont("\n");

    // 范围置位
    bitmap_set(my_bitmap, 50, 10);  // 置位 50~59
    pr_info("weight after set: %d\n", bitmap_weight(my_bitmap, 128));  // 14

    // 范围清零
    bitmap_clear(my_bitmap, 50, 10);  // 清空 50~59
    pr_info("weight after clear: %d\n", bitmap_weight(my_bitmap, 128));  // 4

    // 查找第一个置位位
    int first = find_first_bit(my_bitmap, 128);
    pr_info("first set bit: %d\n", first);  // 0

    // 位图运算
    DECLARE_BITMAP(bitmap_a, 64);
    DECLARE_BITMAP(bitmap_b, 64);
    DECLARE_BITMAP(bitmap_result, 64);

    bitmap_zero(bitmap_a, 64);
    bitmap_zero(bitmap_b, 64);
    set_bit(0, bitmap_a);
    set_bit(1, bitmap_a);
    set_bit(1, bitmap_b);
    set_bit(2, bitmap_b);

    bitmap_and(bitmap_result, bitmap_a, bitmap_b, 64);
    pr_info("intersection: weight=%d\n", bitmap_weight(bitmap_result, 64));  // 1 (bit 1)
}
```

#### 6.1.5 应用场景

- **CPU 位掩码**：`cpumask` 表示 CPU 集合，用于进程亲和性、中断绑定等
- **内存节点掩码**：`nodemask` 表示 NUMA 内存节点集合
- **物理内存管理**：伙伴系统用位图跟踪页框状态（空闲/已用）
- **中断号分配**：`irq_alloc_descs` 用位图管理中断号
- **实时调度器**：`rt_prio_array.bitmap` 标记非空优先级队列

---

### 6.2 sbitmap（可扩展位图）

#### 6.2.1 概述

`sbitmap`（Scalable Bitmap）是 Facebook 为 Linux 内核贡献的高性能位图实现，专为**多核高并发**场景设计。相比传统位图，sbitmap 通过以下设计解决缓存伪共享（cache line bouncing）问题：

- **按 CPU 缓存行对齐**：每个 `sbitmap_word` 独立占用一个缓存行，避免多核并发修改不同位时互相影响
- **Per-CPU 分配提示**：每个 CPU 维护自己的 `alloc_hint`，减少跨 CPU 的原子操作竞争
- **延迟清除**：`->cleared` 与 `->word` 分离，`sbitmap_put` 不直接清除位，而是记录到 `->cleared`，由后续分配者在 `sbitmap_deferred_clear` 中批量处理

**sbitmap vs 传统位图的差异**

| 特性 | sbitmap | 传统位图 (`DECLARE_BITMAP`) |
|---|---|---|
| 缓存行对齐 | 每个 word 独占缓存行（`____cacheline_aligned_in_smp`） | 连续存储，多核竞争时频繁伪共享 |
| 分配策略 | Per-CPU `alloc_hint`，分散到不同 word 起始搜索 | 全局搜索，每次从 0 开始 |
| 清除机制 | 延迟清除（`->cleared` + `sbitmap_deferred_clear`） | 直接清除（`clear_bit`） |
| 并发性能 | 高并发下线性扩展性好 | 高并发下因伪共享急剧下降 |
| 内存开销 | 高（每个 word 独占缓存行，align 到 64/128 字节） | 极低（紧凑存储） |
| 适用场景 | 多核 I/O 标签分配（blk-mq）、高并发资源分配 | 单线程或低并发位图操作 |
| 优点 | 多核可扩展性极好，支持批量分配和等待队列 | 简单、内存高效、API 丰富 |
| 缺点 | 内存占用大，实现复杂 | 高并发下性能瓶颈明显 |

#### 6.2.2 核心数据结构

```c
// include/linux/sbitmap.h

// 每个 word 独占一个缓存行，避免伪共享
struct sbitmap_word {
    unsigned long word;           // 空闲位标记（0 = 空闲，1 = 已用）
    unsigned long cleared ____cacheline_aligned_in_smp;  // 延迟清除位
    raw_spinlock_t swap_lock;     // 保护 word 和 cleared 的批量交换
} ____cacheline_aligned_in_smp;

struct sbitmap {
    unsigned int depth;           // 总位数
    unsigned int shift;           // 每个 word 的位数 = 2^shift
    unsigned int map_nr;          // word 数量
    bool round_robin;             // 是否严格轮询
    struct sbitmap_word *map;     // word 数组
    unsigned int __percpu *alloc_hint;  // Per-CPU 分配提示
};

// 带等待队列的 sbitmap（用于需要在资源不足时休眠的场景）
struct sbitmap_queue {
    struct sbitmap sb;            // 底层位图
    unsigned int wake_batch;      // 每次唤醒的批大小
    atomic_t wake_index;          // 当前唤醒的等待队列索引（轮询）
    struct sbq_wait_state *ws;    // 8 个等待队列，分散锁竞争
    atomic_t ws_active;           // 活跃等待队列数
    unsigned int min_shallow_depth; // 最小浅分配深度
    atomic_t completion_cnt;      // 已完成释放的计数
    atomic_t wakeup_cnt;          // 已触发唤醒的计数
};
```

#### 6.2.3 原理细节：缓存行伪共享与延迟清除

**问题：伪共享（False Sharing）**

传统位图的所有 bit 存储在连续的 `unsigned long` 数组中。当两个 CPU 同时修改同一个缓存行中的不同位时，即使它们操作的是不同的 `unsigned long`，也会导致缓存行失效（cache line invalidation），触发昂贵的 MESI 协议同步。

**sbitmap 的解决方案**

```
传统位图布局（缓存行问题）：
  Cache Line 0: [word0] [word1] [word2] [word3]  ← 4 个 word 共享一个缓存行
  CPU0 写 word0 → 失效整行 → CPU1 读 word1 需要重新加载

sbitmap 布局（每个 word 独占缓存行）：
  Cache Line 0: [sbitmap_word0]
  Cache Line 1: [sbitmap_word1]
  Cache Line 2: [sbitmap_word2]
  ...
  CPU0 写 word0 → 不影响 CPU1 的 word1 缓存
```

**延迟清除（Deferred Clear）**

`word` 存储当前已分配位（1 = 已用），`cleared` 存储已释放位（1 = 待清除）：

```
初始状态：       word = 00000000,  cleared = 00000000
分配 bit 0:      word = 00000001,  cleared = 00000000
分配 bit 1:      word = 00000011,  cleared = 00000000
释放 bit 0:      word = 00000011,  cleared = 00000001  ← 不直接清除 word
下次分配时:
  sbitmap_deferred_clear:
    cleared = 00000001 → 交换清零
    word = word & ~cleared = 00000011 & 11111110 = 00000010
    然后分配可用位
```

这种设计的好处：
1. **释放操作（`sbitmap_put`）是轻量级的**：只需一个 `set_bit` 到 `cleared`，无需 CAS 循环
2. **批量清除减少原子操作**：多个释放累积后，一次 `xchg` + `atomic_long_andnot` 完成批量清除
3. **分配时顺带清除**：`sbitmap_find_bit_in_word` 在发现 word 满时自动调用 `sbitmap_deferred_clear`

#### 6.2.4 完整 API 参考

**sbitmap 核心 API**

| 函数 | 功能 |
|---|---|
| `sbitmap_init_node(sb, depth, shift, flags, node, round_robin, alloc_hint)` | 初始化 sbitmap |
| `sbitmap_free(sb)` | 释放内存 |
| `sbitmap_resize(sb, depth)` | 调整大小（不重新分配） |
| `sbitmap_get(sb)` | 分配一个空闲位 |
| `sbitmap_put(sb, bitnr)` | 释放位（延迟清除） |
| `sbitmap_test_bit(sb, bitnr)` | 测试位是否已分配 |
| `sbitmap_any_bit_set(sb)` | 检查是否有任何位被设置 |
| `sbitmap_weight(sb)` | 统计已分配且未清除的位数 |
| `sbitmap_for_each_set(sb, fn, data)` | 遍历所有已设置位 |
| `sbitmap_show(sb, m)` | 打印调试信息 |

**sbitmap_queue 核心 API**

| 函数 | 功能 |
|---|---|
| `sbitmap_queue_init_node(sbq, depth, shift, round_robin, flags, node)` | 初始化带等待队列的 sbitmap |
| `sbitmap_queue_free(sbq)` | 释放内存 |
| `sbitmap_queue_resize(sbq, depth)` | 调整大小 |
| `__sbitmap_queue_get(sbq)` | 分配位（需已禁用抢占） |
| `__sbitmap_queue_get_batch(sbq, nr_tags, offset)` | 批量分配多个连续位 |
| `sbitmap_queue_get_shallow(sbq, shallow_depth)` | 浅分配（限制最大使用量） |
| `sbitmap_queue_clear(sbq, nr, cpu)` | 释放位并唤醒等待者 |
| `sbitmap_queue_wake_up(sbq, nr)` | 手动触发唤醒 |
| `sbitmap_queue_wake_all(sbq)` | 唤醒所有等待者 |
| `sbitmap_prepare_to_wait(sbq, ws, sbq_wait, state)` | 准备等待 |
| `sbitmap_finish_wait(sbq, ws, sbq_wait)` | 结束等待 |
| `sbitmap_queue_min_shallow_depth(sbq, depth)` | 设置最小浅分配深度 |
| `sbitmap_queue_show(sbq, m)` | 打印调试信息 |

#### 6.2.5 分配算法详解

```
sbitmap_get(sb)
  ├─ 1. update_alloc_hint_before_get()
  │    从 percpu alloc_hint 读取 hint，确保 hint < depth
  │
  ├─ 2. __sbitmap_get(sb, hint)
  │    ├─ index = SB_NR_TO_INDEX(sb, hint)  // hint → word 索引
  │    ├─ if round_robin: alloc_hint = SB_NR_TO_BIT(sb, hint)
  │    │  else: alloc_hint = 0              // 非轮询模式从 0 开始搜索
  │    │
  │    └─ sbitmap_find_bit(sb, ..., index, alloc_hint, wrap)
  │         for i = 0..map_nr:              // 遍历所有 word
  │           ├─ sbitmap_find_bit_in_word()
  │           │   do {
  │           │     ├─ __sbitmap_get_word()  // find_next_zero_bit + test_and_set_bit_lock
  │           │     └─ if fail: sbitmap_deferred_clear()  // 批量清除并重试
  │           │   } while (1)
  │           └─ if found: return global bit nr
  │
  └─ 3. update_alloc_hint_after_get()
        ├─ if nr == -1: hint = 0            // 全满，重置 hint
        └─ else: hint = nr + 1               // 更新 hint 到下一个可能位置
```

#### 6.2.6 完整使用示例

```c
#include <linux/sbitmap.h>
#include <linux/printk.h>

// 1. 基本 sbitmap 使用
static struct sbitmap my_sb;

int init_sbitmap_example(void)
{
    // 初始化：1024 位，每个 word 64 位（由 shift=6 指定）
    // 最终 map_nr = 1024 / 64 = 16 个 word
    return sbitmap_init_node(&my_sb, 1024, 6,
                             GFP_KERNEL, NUMA_NO_NODE,
                             false, true);
}

void sbitmap_alloc_free_example(void)
{
    int bit;

    // 分配一个位
    bit = sbitmap_get(&my_sb);
    if (bit >= 0) {
        pr_info("allocated bit %d\n", bit);

        // 使用位...
        // 使用完后释放
        sbitmap_put(&my_sb, bit);
        pr_info("freed bit %d\n", bit);
    }

    // 遍历所有已分配位
    sbitmap_for_each_set(&my_sb, my_callback, NULL);
}

static bool my_callback(struct sbitmap *sb, unsigned int bitnr, void *data)
{
    pr_info("bit %d is set\n", bitnr);
    return true;  // 继续遍历
}

// 2. sbitmap_queue 使用（带等待队列，典型 blk-mq 场景）
static struct sbitmap_queue my_tag_queue;

int init_tag_queue(void)
{
    // 初始化：128 个标签，自动计算 shift
    return sbitmap_queue_init_node(&my_tag_queue, 128, -1,
                                   false, GFP_KERNEL, NUMA_NO_NODE);
}

int get_tag(void)
{
    // 分配标签（必须在禁用抢占的上下文中调用）
    int tag = __sbitmap_queue_get(&my_tag_queue);
    if (tag < 0) {
        // 没有可用标签，可能需要等待
        pr_info("no tags available, need to wait\n");
    }
    return tag;
}

void put_tag(int tag, int cpu)
{
    // 释放标签并唤醒等待者
    sbitmap_queue_clear(&my_tag_queue, tag, cpu);
}

// 3. 批量分配
unsigned int get_tag_batch(int nr_tags)
{
    unsigned int offset;
    unsigned long mask;

    mask = __sbitmap_queue_get_batch(&my_tag_queue, nr_tags, &offset);
    if (mask) {
        // 成功分配了 hweight(mask) 个连续标签，起始于 offset
        pr_info("batch allocated %lu tags starting at %u\n",
                hweight_long(mask), offset);
    }
    return mask;
}
```

#### 6.2.7 应用场景

- **blk-mq 标签分配**：块设备多队列（Multi-Queue）的 I/O 标签管理，每个硬件队列需要独立的高并发位图
- **virtio 队列**：virtio 虚拟队列的描述符索引分配
- **多核网络设备**：高吞吐量网络设备的缓冲区描述符管理
- **任何需要高并发位图操作的场景**：当传统位图因伪共享成为性能瓶颈时

#### 6.2.8 sbitmap_queue 唤醒机制详解

`sbitmap_queue` 在底层 `sbitmap` 基础上增加了**等待-唤醒**能力，用于资源不足时挂起任务、资源可用时唤醒。其核心设计目标是避免高并发下等待队列自旋锁的竞争。

**等待队列数组（8 路分散）**

```
SBQ_WAIT_QUEUES = 8 个等待队列

  sbq->ws[0]  sbq->ws[1]  ...  sbq->ws[7]
    ↓            ↓                  ↓
  waitqueue   waitqueue          waitqueue
  (cacheline) (cacheline)        (cacheline)

每个 ws 独占一个缓存行（____cacheline_aligned_in_smp），避免伪共享
```

**滚动唤醒（Rolling Wakeups）**

为避免所有等待者都挤在同一个等待队列上，`sbitmap_queue` 使用 `wake_index` 原子变量实现轮询唤醒：

```c
// 唤醒流程：__sbitmap_queue_wake_up(sbq, nr)
//
// 1. 从 wake_index 指向的 ws 开始
// 2. 唤醒最多 nr 个等待者
// 3. 如果该队列的等待者不够 nr 个，继续唤醒下一个 ws
// 4. 更新 wake_index 到最后一个被唤醒的 ws 的下一个

wake_index = atomic_read(&sbq->wake_index);
for (i = 0; i < SBQ_WAIT_QUEUES; i++) {
    ws = &sbq->ws[wake_index];
    wake_index = sbq_index_inc(wake_index);  // 先推进索引，再唤醒

    if (waitqueue_active(&ws->wait)) {
        woken = wake_up_nr(&ws->wait, nr);
        if (woken == nr) break;
        nr -= woken;
    }
}
atomic_set(&sbq->wake_index, wake_index);
```

**批唤醒阈值（wake_batch）**

`wake_batch` 决定每释放多少个位才触发一次唤醒，避免频繁的唤醒操作：

```c
// 初始化时：wake_batch = max(1, min(depth, min_shallow_depth) / 8)
// 上限：SBQ_WAKE_BATCH = 8
// 例如 depth=128 时，wake_batch = 128/8 = 16

// 唤醒条件判断（sbitmap_queue_wake_up）：
// completion_cnt - wakeup_cnt >= wake_batch
// 即：累计释放的位数 - 已触发的唤醒次数 × wake_batch ≥ wake_batch
//
// 例如 wake_batch=16：
//   第 1 次释放 16 位 → completion_cnt=16, wakeup_cnt=0 → 触发唤醒
//   第 2 次释放 16 位 → completion_cnt=32, wakeup_cnt=16 → 触发唤醒
//   第 3 次释放 8 位  → completion_cnt=40, wakeup_cnt=32 → 不触发（差 8 位）

void sbitmap_queue_wake_up(struct sbitmap_queue *sbq, int nr)
{
    atomic_add(nr, &sbq->completion_cnt);
    wakeups = atomic_read(&sbq->wakeup_cnt);

    do {
        if (atomic_read(&sbq->completion_cnt) - wakeups < wake_batch)
            return;  // 未达到阈值，不唤醒
    } while (!atomic_try_cmpxchg(&sbq->wakeup_cnt,
                                 &wakeups, wakeups + wake_batch));

    __sbitmap_queue_wake_up(sbq, wake_batch);
}
```

**等待者生命周期**

```
任务需要资源但资源不足时：

  1. sbitmap_prepare_to_wait(sbq, ws, sbq_wait, TASK_UNINTERRUPTIBLE)
     ├─ ws_active++ (标记该等待队列有活跃等待者)
     └─ prepare_to_wait_exclusive(&ws->wait, &sbq_wait->wait, state)

  2. 再次尝试分配资源（__sbitmap_queue_get）
     ├─ 成功 → sbitmap_finish_wait() → 取消等待，继续执行
     └─ 失败 → schedule() → 睡眠，等待唤醒

  3. 被唤醒后：
     ├─ sbitmap_finish_wait(sbq, ws, sbq_wait)
     │  └─ finish_wait() + 如果 sbq_wait->sbq 非空则 ws_active--
     └─ 再次尝试分配
```

**ws_active 优化**

`ws_active` 原子变量标记是否有等待队列中有活跃等待者。`sbitmap_queue_wake_up` 首先检查 `ws_active`，如果为 0 则直接返回，避免在无等待者时走完整的唤醒路径。

#### 6.2.9 批量分配与浅分配机制

**批量分配（__sbitmap_queue_get_batch）**

批量分配用于 I/O 场景中一次分配多个连续标签（如 blk-mq 的多个 I/O 请求）。它使用 `cmpxchg` 循环而非 `test_and_set_bit` 来减少原子操作次数：

```c
// 调用示例：
// mask = __sbitmap_queue_get_batch(&sbq, 4, &offset);
// 成功时 mask 的二进制表示中连续 1 的个数即为分配到的标签数

// 实现流程：
//
// 1. 遍历所有 word（从 alloc_hint 对应的 word 开始）
// 2. 对每个 word：
//    a. sbitmap_deferred_clear() — 先延迟清除
//    b. 读取 word 当前值 val
//    c. 如果 word 已满（val == (1<<map_depth) - 1），跳过
//    d. 找到第一个空闲位 nr
//    e. 检查是否有连续 nr_tags 个空闲位：nr + nr_tags <= map_depth
//    f. 构造 get_mask = ((1<<nr_tags) - 1) << nr
//    g. 使用 atomic_long_try_cmpxchg 原子设置所有位
//    h. 返回实际分配到的 mask（可能因并发竞争少于请求数）

// 关键代码：
for (i = 0; i < sb->map_nr; i++) {
    sbitmap_deferred_clear(map, 0, 0, 0);
    val = READ_ONCE(map->word);
    if (val == (1UL << (map_depth - 1)) - 1)
        goto next;  // word 已满

    nr = find_first_zero_bit(&val, map_depth);
    if (nr + nr_tags <= map_depth) {
        get_mask = ((1UL << nr_tags) - 1) << nr;
        while (!atomic_long_try_cmpxchg(ptr, &val, get_mask | val))
            ;  // CAS 循环，直到成功或 val 被其他线程修改
        get_mask = (get_mask & ~val) >> nr;
        if (get_mask) {
            *offset = nr + (index << sb->shift);
            return get_mask;  // 返回实际分配数
        }
    }
next:
    // 跳转到下一个 word
}
```

**注意**：批量分配不支持 `round_robin` 模式，如果设置了 `round_robin` 则直接返回 0。

**浅分配（sbitmap_queue_get_shallow）**

浅分配机制允许不同优先级类别的使用者限制各自的分配深度，防止低优先级类耗尽所有资源：

```c
// 场景：两个用户共享同一个 sbitmap_queue
// 高优先级用户 → sbitmap_queue_get() → 可分配任意深度
// 低优先级用户 → sbitmap_queue_get_shallow(sbq, depth/2) → 最多分配一半

// 原理：
// 每个 word 的可分配深度被比例缩小：
//   shallow_word_depth = word_depth * shallow_depth / sb->depth
// 例如 depth=128, map_nr=2, 每个 word 64 位
//   浅分配 depth=64 时，每个 word 可用 64*64/128 = 32 位
//   即低优先级用户在每个 word 中最多使用 32 位

// 最小浅分配深度保护：
// sbitmap_queue_min_shallow_depth(sbq, min_depth)
// 设置后，所有 sbitmap_queue_get_shallow 的 shallow_depth 不得低于此值
// 这用于防止 min_shallow_depth 被设置得太低，导致唤醒阈值计算异常
```

#### 6.2.10 内存屏障与清除路径

sbitmap 的清除路径包含精心设计的内存屏障，确保多核并发下的正确性。

**sbitmap_queue_clear 单路清除**

```c
void sbitmap_queue_clear(struct sbitmap_queue *sbq, unsigned int nr,
                         unsigned int cpu)
{
    // ① 释放屏障：确保在清除位之前，所有对该位关联对象的读写已完成
    // 这与 __sbitmap_get_word 中 test_and_set_bit_lock 的 acquire 语义配对
    smp_mb__before_atomic();

    // ② 延迟清除：将位设置到 cleared，而非直接清除 word
    sbitmap_deferred_clear_bit(&sbq->sb, nr);

    // ③ 获取屏障：确保 cleared 的写入在 waitqueue_active 检查之前可见
    // 与 set_current_state() 中的内存屏障配对
    smp_mb__after_atomic();

    // ④ 触发唤醒
    sbitmap_queue_wake_up(sbq, 1);

    // ⑤ 更新 Per-CPU alloc_hint 到当前 CPU
    sbitmap_update_cpu_hint(&sbq->sb, cpu, nr);
}
```

**sbitmap_queue_clear_batch 批量清除**

```c
// 批量清除多个标签，优化：将同一 word 中的多个清除合并为一次原子操作
void sbitmap_queue_clear_batch(struct sbitmap_queue *sbq, int offset,
                               int *tags, int nr_tags)
{
    // 遍历所有标签，对同一 word 的标签合并为一张 mask
    for (i = 0; i < nr_tags; i++) {
        tag = tags[i] - offset;
        this_addr = &sb->map[SB_NR_TO_INDEX(sb, tag)].word;

        if (!addr) {
            addr = this_addr;              // 首个 word
        } else if (addr != this_addr) {
            // 切换到另一个 word，先清除当前累积的 mask
            atomic_long_andnot(mask, (atomic_long_t *)addr);
            mask = 0;
            addr = this_addr;
        }
        mask |= (1UL << SB_NR_TO_BIT(sb, tag));
    }

    if (mask)
        atomic_long_andnot(mask, (atomic_long_t *)addr);
    // 所有清除完成后，统一触发唤醒
    sbitmap_queue_wake_up(sbq, nr_tags);
}
```

**关键内存屏障配对关系**

| 操作 | 屏障 | 配对操作 | 保证 |
|------|------|----------|------|
| `sbitmap_queue_clear` | `smp_mb__before_atomic()` | `__sbitmap_get_word` 中的 `test_and_set_bit_lock` (acquire) | 确保释放者对关联对象的写入在清除位之前完成，避免分配者看到未初始化的对象 |
| `sbitmap_queue_clear` | `smp_mb__after_atomic()` | `set_current_state()` 中的屏障 | 确保 `cleared` 位的设置在检查 `waitqueue_active` 之前全局可见，避免丢失唤醒 |
| `sbitmap_queue_wake_up` | `atomic_try_cmpxchg` 隐含的完整屏障 | `sbitmap_prepare_to_wait` 中的 `prepare_to_wait_exclusive` | 确保 `completion_cnt` 的更新和 `waitqueue_active` 的检查正确排序 |

---

### 6.3 Trie树（前缀树）

#### 6.3.1 概述

Trie 树（又称前缀树）是一种按字符串前缀组织的树结构，用于高效的前缀匹配。内核中主要使用 **LPC-Trie（LC-Trie）** 变体实现路由查找。

#### 6.3.2 核心数据结构

```c
// net/ipv4/fib_trie.c (IPv4 路由表)
struct trie {
    struct key_vector *kv;      // 键向量（节点）
    ...
};

struct key_vector {
    t_key key;                  // 键值
    unsigned char pos;          // 当前位位置
    unsigned char bits;         // 处理的位数
    struct key_vector *tnode;   // 子节点
    struct list_head *leaf;     // 叶节点（路由条目）
};
```

#### 6.3.3 原理细节：最长前缀匹配（LPM）

Trie 树在内核中最核心的应用是路由表查找，实现最长前缀匹配（Longest Prefix Match, LPM）：

```
IP 地址 192.168.1.0/24 的路由查找过程：

输入：192.168.1.55 (二进制 11000000 10101000 00000001 00110111)

  Level 0: 按位 0-7 匹配 → 11000000 → 节点 192
  Level 1: 按位 8-15 匹配 → 10101000 → 节点 168
  Level 2: 按位 16-23 匹配 → 00000001 → 节点 1
  Level 3: 按位 24-31 匹配 → 00110111 → 叶节点（路由条目）
                         ↑ 匹配到 /24 时命中路由
                         如果还有更长的 /32 路由，继续匹配
```

**LPC-Trie（LC-Trie）优化**

- **多比特匹配**：每层处理多个比特位，而非单个比特，减少树深度
- **路径压缩**：跳过无分支的中间节点，直接指向分叉点
- **叶节点数组**：连续存储叶节点，改善缓存局部性

#### 6.3.4 核心 API（路由表查找示例）

```c
// net/ipv4/fib_trie.c — 基于 Trie 的路由查找

// 路由查找核心函数
struct fib_result *fib_table_lookup(struct fib_table *tb,
                                    const struct flowi4 *flp,
                                    struct fib_result *res, int flags);

// 遍历前缀树
int trie_leaf_remove(struct trie *t, t_key key);
struct trie *fib_trie_unmerge(struct fib_table *main_tb);

// 统计信息
void fib_trie_seq_show(struct seq_file *seq, struct trie *trie);
```

#### 6.3.5 应用场景

- **路由表查找**：IPv4/IPv6 路由表，实现最长前缀匹配（LPM）
- **设备命名空间管理**：设备路径的前缀匹配
- **网络过滤**：iptables/nftables 规则匹配

---

## 7. 映射与关联结构

### 7.1 IDR

#### 7.1.1 概述

IDR（ID Radix Tree）是将整数 ID 映射到指针的结构。它曾基于基数树实现，用于管理设备号、PID 等 ID 分配。在现代内核中，IDR 已迁移到基于 XArray 的实现。

#### 7.1.2 核心数据结构

```c
// include/linux/idr.h
struct idr {
    struct xarray xa;     // 基于 XArray 实现
    unsigned int idr_next;  // 下一次分配的位置
};
```

#### 7.1.3 完整 API 参考

```c
// include/linux/idr.h
// 初始化
void idr_init(struct idr *idr);
#define IDR_INIT(name)    // 静态初始化

// 分配 ID 并存储指针
int idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);
int idr_alloc_u32(struct idr *idr, void *ptr, u32 *nextid, u32 max, gfp_t gfp);
int idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);

// 查找
void *idr_find(const struct idr *idr, unsigned long id);
void *idr_get_next(struct idr *idr, int *nextid);  // 获取下一个有效 ID

// 删除
void *idr_remove(struct idr *idr, unsigned long id);
void idr_destroy(struct idr *idr);  // 销毁所有条目
void idr_preload(gfp_t gfp);       // 预分配（用于原子上下文）

// 遍历
#define idr_for_each_entry(idr, entry, id)          // 遍历所有条目
#define idr_for_each_entry_continue(idr, entry, id)  // 从指定 ID 继续遍历
#define idr_for_each_entry_ul(idr, entry, id)        // 使用 unsigned long 版本

// 辅助宏
#define idr_is_empty(idr)   // 判断是否为空
```

#### 7.1.4 完整使用示例

```c
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/printk.h>

// 定义 IDR
static DEFINE_IDR(my_idr);

// 分配 ID 并存储对象
int my_idr_alloc(void *data)
{
    // 在原子上下文分配前需要预加载
    idr_preload(GFP_KERNEL);
    int id = idr_alloc(&my_idr, data, 0, INT_MAX, GFP_ATOMIC);
    idr_preload_end();
    return id;
}

// 根据 ID 查找对象
void *my_idr_find(int id)
{
    return idr_find(&my_idr, id);
}

// 删除 ID 映射
void *my_idr_remove(int id)
{
    return idr_remove(&my_idr, id);
}

// 遍历所有条目
void my_idr_dump(void)
{
    unsigned long id = 0;
    void *entry;

    idr_for_each_entry_ul(&my_idr, entry, id) {
        pr_info("id=%lu, entry=%p\n", id, entry);
    }
}
```

#### 7.1.5 应用场景

- **设备号管理**：`MKDEV(ma, mi)` 到设备对象的映射
- **PID 分配**：PID 到 `task_struct` 的映射
- **IPC 对象**：信号量、共享内存的 ID 管理

---

### 7.2 XArray

#### 7.2.1 概述

XArray 是 Linux 内核中用于替代传统基数树（radix tree）和 IDR 的可扩展数组实现。它支持稀疏索引、范围操作和标记功能，提供了更简洁的 API 和更好的 RCU 安全保证。XArray 是内核中整数键值映射的现代化解决方案。

**XArray vs 传统基数树（Radix Tree）的差异**

| 特性 | XArray | 传统基数树 |
|---|---|---|
| API 设计 | 统一、简洁（`xa_load`/`xa_store`/`xa_erase`） | 分散（`radix_tree_insert`/`radix_tree_delete`/`radix_tree_lookup`） |
| 标记操作 | 集成在 API 中（`xa_set_mark`/`xa_clear_mark`/`xa_get_mark`） | 单独的函数（`radix_tree_tag_set`/`radix_tree_tag_clear`） |
| 内部实现 | 基于相同的多级树结构，但优化了节点编码 | 直接暴露节点结构 |
| 条目类型 | 内部编码（使用指针低位标记特殊条目） | 需要调用者自行处理空条目 |
| 迭代器 | 多种迭代器（`xa_for_each`/`xa_for_each_range`/`xa_for_each_marked`） | 有限的迭代支持 |
| 锁管理 | 统一的锁原语（`xa_lock`/`xa_unlock`） | 调用者需自行管理锁 |
| 兼容性 | 向前兼容，可替代 IDR 和 radix tree | 被逐步淘汰 |
| 优点 | API 统一简洁，RCU 集成更好，标记操作更直观 | 历史悠久，文档和工具链支持成熟 |
| 缺点 | 相对较新（4.20+），部分旧代码不兼容 | API 分散，使用复杂，维护成本高 |

#### 7.2.2 核心数据结构

```c
// include/linux/xarray.h
struct xarray {
    spinlock_t xa_lock;       // 保护 XArray 的锁
    gfp_t xa_flags;           // 内存分配标志
    void __rcu *xa_head;      // 头节点指针（RCU 保护）
};
```

#### 7.2.3 初始化

```c
// 静态初始化
#define XARRAY_INIT(name, flags) {                     \
    .xa_lock = __SPIN_LOCK_UNLOCKED(name.xa_lock),     \
    .xa_flags = flags,                                 \
    .xa_head = NULL,                                   \
}

// 动态初始化
static inline void xa_init(struct xarray *xa)
{
    xa_init_flags(xa, 0);
}
```

#### 7.2.4 核心操作函数

```c
// 查找
void *xa_load(struct xarray *xa, unsigned long index);

// 存储
void *xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);

// 删除
void *xa_erase(struct xarray *xa, unsigned long index);

// 条件更新
void *xa_cmpxchg(struct xarray *xa, unsigned long index,
                 void *old, void *entry, gfp_t gfp);

// 插入（不覆盖已有值）
void *xa_insert(struct xarray *xa, unsigned long index,
                void *entry, gfp_t gfp);
```

#### 7.2.5 标记（Mark）操作

```c
// 设置标记
bool xa_set_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);

// 清除标记
bool xa_clear_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);

// 测试标记
bool xa_get_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);

// 查找带特定标记的条目
void *xa_find(struct xarray *xa, unsigned long *index,
              unsigned long max, xa_mark_t filter);
```

#### 7.2.6 遍历

```c
#define xa_for_each(xa, index, entry) \
    xa_for_each_start(xa, index, entry, 0)

#define xa_for_each_range(xa, index, entry, start, last) \
    for (index = 0, entry = xa_find(xa, &index, last, XA_PRESENT); \
         entry; \
         entry = xa_find_after(xa, &index, last, XA_PRESENT))
```

#### 7.2.7 完整 API 参考

| 函数/宏 | 功能 |
|---|---|
| `xa_init(xa)` / `xa_init_flags(xa, flags)` | 初始化 |
| `xa_load(xa, index)` | 加载条目 |
| `xa_store(xa, index, entry, gfp)` | 存储条目 |
| `xa_erase(xa, index)` | 删除条目 |
| `xa_cmpxchg(xa, index, old, entry, gfp)` | 条件更新 |
| `xa_insert(xa, index, entry, gfp)` | 不覆盖插入 |
| `xa_alloc(xa, id, entry, limit, gfp)` | 分配 ID 并存储 |
| `xa_alloc_cyclic(xa, id, entry, limit, gfp)` | 循环分配 ID |
| `xa_set_mark(xa, index, mark)` | 设置标记 |
| `xa_clear_mark(xa, index, mark)` | 清除标记 |
| `xa_get_mark(xa, index, mark)` | 测试标记 |
| `xa_find(xa, index, max, filter)` | 查找带标记的条目 |
| `xa_find_after(xa, index, max, filter)` | 查找下一个 |
| `xa_extract(xa, dst, start, max, n, filter)` | 批量提取 |
| `xa_for_each(xa, index, entry)` | 遍历所有条目 |
| `xa_for_each_range(xa, index, entry, start, last)` | 遍历指定范围 |
| `xa_for_each_marked(xa, index, entry, filter)` | 遍历带标记的条目 |
| `xa_destroy(xa)` | 销毁所有条目 |
| `xa_lock(xa)` / `xa_unlock(xa)` | 加锁/解锁 |
| `xa_lock_irqsave(xa, flags)` | 中断安全加锁 |

#### 7.2.8 内部实现

XArray 内部使用多级树结构：

- **叶节点**：直接存储条目指针
- **内部节点**：包含指向子节点的指针数组（64 个 slot）
- **节点编码**：指针的低位用于标记条目类型
- **RCU 安全**：读操作可无锁并发

#### 7.2.9 完整使用示例

```c
#include <linux/xarray.h>
#include <linux/slab.h>
#include <linux/printk.h>

// 定义 XArray
static DEFINE_XARRAY(my_xa);

// 存储条目
int xa_store_entry(unsigned long index, void *data)
{
    void *old = xa_store(&my_xa, index, data, GFP_KERNEL);
    if (xa_is_err(old))
        return xa_err(old);  // 返回负的错误码
    return 0;
}

// 查找条目
void *xa_load_entry(unsigned long index)
{
    return xa_load(&my_xa, index);
}

// 删除条目
void *xa_remove_entry(unsigned long index)
{
    return xa_erase(&my_xa, index);
}

// 使用标记：标记索引 0-100 为"脏"
void xa_mark_dirty(void)
{
    unsigned long i;
    for (i = 0; i < 100; i++)
        xa_set_mark(&my_xa, i, XA_MARK_0);
}

// 遍历所有脏条目
void xa_process_dirty(void)
{
    unsigned long index = 0;
    void *entry;

    xa_for_each_marked(&my_xa, index, entry, XA_MARK_0) {
        pr_info("dirty entry at index %lu\n", index);
        // 处理完后清除标记
        xa_clear_mark(&my_xa, index, XA_MARK_0);
    }
}

// 使用 IDR 风格的自动 ID 分配
int xa_alloc_id(void *data)
{
    u32 id;
    return xa_alloc(&my_xa, &id, data, XA_LIMIT(0, U32_MAX), GFP_KERNEL);
}

// 遍历所有条目
void xa_dump_all(void)
{
    unsigned long index = 0;
    void *entry;

    xa_for_each(&my_xa, index, entry) {
        pr_info("index=%lu, entry=%p\n", index, entry);
    }
}
```

#### 7.2.10 应用场景

- **文件系统页缓存**：`address_space.i_pages`（替代 radix tree）
- **文件描述符表**：`struct fdtable.fd_array` 的扩展存储
- **IDR 分配器**：XArray 替代了旧的 IDR 实现
- **内存管理**：管理文件页的缓存状态和脏/回写标记

---

### 7.3 eBPF Maps

#### 7.3.1 概述

eBPF Maps 是内核提供的通用键值存储结构，允许用户空间和 eBPF 程序之间共享数据。eBPF Maps 支持多种底层实现，包括哈希表、数组、LRU、红黑树等。

#### 7.3.2 支持的 Map 类型

| Map 类型 | 底层实现 | 特点 |
|---|---|---|
| `BPF_MAP_TYPE_HASH` | 哈希表 | 通用键值存储 |
| `BPF_MAP_TYPE_ARRAY` | 数组 | 固定大小，访问快 |
| `BPF_MAP_TYPE_LRU_HASH` | LRU + 哈希表 | 限制大小，自动淘汰 |
| `BPF_MAP_TYPE_PERCPU_HASH` | Per-CPU 哈希表 | 减少锁竞争 |
| `BPF_MAP_TYPE_STACK` | 栈 | LIFO 操作 |
| `BPF_MAP_TYPE_QUEUE` | FIFO 队列 | 先进先出 |
| `BPF_MAP_TYPE_RINGBUF` | 环形缓冲区 | 高性能数据传递 |

#### 7.3.3 核心 API（用户空间）

```c
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

// 创建 Map
int bpf_map_create(enum bpf_map_type type, const char *name,
                   __u32 key_size, __u32 value_size,
                   __u32 max_entries, const struct bpf_map_create_opts *opts);

// 查找
int bpf_map_lookup_elem(int fd, const void *key, void *value);

// 更新
int bpf_map_update_elem(int fd, const void *key, const void *value,
                        __u64 flags);

// 删除
int bpf_map_delete_elem(int fd, const void *key);

// 遍历（获取下一个键）
int bpf_map_get_next_key(int fd, const void *key, void *next_key);

// 批量操作
int bpf_map_lookup_batch(int fd, void *in_batch, void *out_batch,
                         void *keys, void *values, __u32 *count,
                         struct bpf_map_batch_opts *opts);
int bpf_map_update_batch(int fd, void *keys, void *values,
                         __u32 *count, struct bpf_map_batch_opts *opts);
int bpf_map_delete_batch(int fd, void *keys, __u32 *count,
                         struct bpf_map_batch_opts *opts);
```

#### 7.3.4 核心 API（eBPF 程序内部）

```c
// 在 eBPF 程序中访问 Map
// 查找
static __always_inline void *bpf_map_lookup_elem(struct bpf_map *map,
                                                  const void *key);

// 更新
static __always_inline long bpf_map_update_elem(struct bpf_map *map,
                                                 const void *key,
                                                 const void *value,
                                                 __u64 flags);

// 删除
static __always_inline long bpf_map_delete_elem(struct bpf_map *map,
                                                 const void *key);

// 遍历
static __always_inline long bpf_for_each_map_elem(struct bpf_map *map,
                                                   void *callback_fn,
                                                   void *callback_ctx,
                                                   __u64 flags);
```

#### 7.3.5 完整使用示例

**eBPF 内核侧程序**

```c
// my_kern.c
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

// 定义哈希表 Map
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, u32);
    __type(value, u64);
} my_map SEC(".maps");

// 定义数组 Map
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 64);
    __type(key, u32);
    __type(value, u64);
} my_array SEC(".maps");

// 跟踪点程序：记录系统调用次数
SEC("tracepoint/syscalls/sys_enter_write")
int count_write(void *ctx)
{
    u32 key = 0;
    u64 *count, new_count = 1;

    count = bpf_map_lookup_elem(&my_map, &key);
    if (count)
        new_count = *count + 1;

    bpf_map_update_elem(&my_map, &key, &new_count, BPF_ANY);
    return 0;
}
```

**用户空间加载程序**

```c
// my_user.c
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "my_kern.skel.h"

int main(void)
{
    struct my_kern *skel;
    int err;
    u32 key = 0;
    u64 value;

    // 加载并附加 BPF 程序
    skel = my_kern__open_and_load();
    if (!skel)
        return 1;

    err = my_kern__attach(skel);
    if (err)
        goto cleanup;

    // 读取 Map 中的值
    bpf_map_lookup_elem(bpf_map__fd(skel->maps.my_map), &key, &value);
    printf("write count: %lu\n", value);

    // 写入 Map
    u64 new_val = 42;
    bpf_map_update_elem(bpf_map__fd(skel->maps.my_map), &key, &new_val, BPF_ANY);

cleanup:
    my_kern__destroy(skel);
    return 0;
}
```

#### 7.3.6 应用场景

- **跟踪和性能分析**：存储统计数据和事件
- **网络过滤**：存储过滤规则和连接状态
- **安全监控**：存储进程白名单、系统调用过滤规则

---

## 8. 队列与缓冲区

### 8.1 环形缓冲区 (kfifo)

#### 8.1.1 概述

kfifo 是内核中实现的无锁环形缓冲区（Ring Buffer），用于单生产者/单消费者场景下的高效数据传输。其设计充分利用了无符号整数溢出和 2 的幂次对齐的特性，使得队列满/空判断和指针移动操作非常简洁。

**内核实现 vs 传统环形缓冲区的差异**

| 特性 | 内核 kfifo | 传统环形缓冲区 |
|---|---|---|
| 满/空判断 | `(in - out) == size` 利用无符号溢出 | 通常使用额外标志位或预留一个空位 |
| 指针移动 | `in & (size - 1)` 位运算代替取模 | 通常使用 `in % size` 取模运算 |
| 缓冲区大小 | 必须是 2 的幂次（强制约束） | 任意大小 |
| 并发安全 | 单生产者/单消费者无需加锁 | 通常需要加锁或使用 CAS |
| 无符号溢出 | 利用溢出特性，`in` 永远递增 | 通常对 `size` 取模，手动处理回绕 |
| 优点 | 无锁高性能，无分支的指针运算，简洁优雅 | 缓冲区大小灵活，通用性强 |
| 缺点 | 大小必须为 2 的幂次（浪费少量空间） | 取模运算性能较差，满/空判断复杂 |

#### 8.1.2 核心数据结构

```c
// include/linux/kfifo.h
struct kfifo {
    unsigned char *buffer;    // 缓冲区指针
    unsigned int size;        // 缓冲区大小（2 的幂次）
    unsigned int in;          // 写入位置（无符号，可溢出）
    unsigned int out;         // 读取位置（无符号，可溢出）
};
```

#### 8.1.3 核心操作

```c
// 初始化
int kfifo_alloc(struct kfifo *fifo, unsigned int size, gfp_t gfp);

// 入队
unsigned int kfifo_in(struct kfifo *fifo, const void *from, unsigned int len);

// 出队
unsigned int kfifo_out(struct kfifo *fifo, void *to, unsigned int len);

// 查看队列头（不出队）
unsigned int kfifo_out_peek(struct kfifo *fifo, void *to, unsigned int len);

// 判断空/满
static inline bool kfifo_is_empty(const struct kfifo *fifo);
static inline bool kfifo_is_full(const struct kfifo *fifo);

// 获取可读/可写长度
unsigned int kfifo_len(const struct kfifo *fifo);
unsigned int kfifo_avail(const struct kfifo *fifo);
```

#### 8.1.4 完整 API 参考

| 函数/宏 | 功能 |
|---|---|
| `kfifo_alloc(fifo, size, gfp)` | 分配并初始化 kfifo |
| `kfifo_init(fifo, buffer, size)` | 使用现有缓冲区初始化 |
| `kfifo_free(fifo)` | 释放 kfifo 缓冲区 |
| `kfifo_in(fifo, buf, len)` | 入队 |
| `kfifo_out(fifo, buf, len)` | 出队 |
| `kfifo_out_peek(fifo, buf, len)` | 查看（不出队） |
| `kfifo_in_spinlocked(fifo, buf, len, lock)` | 加锁入队 |
| `kfifo_out_spinlocked(fifo, buf, len, lock)` | 加锁出队 |
| `kfifo_skip(fifo, len)` | 跳过 len 字节数据 |
| `kfifo_avail(fifo)` | 获取剩余可写空间 |
| `kfifo_len(fifo)` | 获取已用数据长度 |
| `kfifo_is_empty(fifo)` | 判断是否为空 |
| `kfifo_is_full(fifo)` | 判断是否为满 |
| `kfifo_size(fifo)` | 获取缓冲区总大小 |
| `kfifo_reset(fifo)` | 重置（丢弃所有数据） |
| `kfifo_to_user(fifo, to, len, copied)` | 出队到用户空间 |
| `kfifo_from_user(fifo, from, len, copied)` | 从用户空间入队 |

#### 8.1.5 无锁原理

```
关键设计：size 为 2 的幂次，in/out 为无符号整数

in & (size - 1)  // 等价于 in % size（利用位运算）
(in - out)        // 已用长度（利用无符号溢出）
size - (in - out) // 剩余空间

满：in - out == size
空：in == out
```

#### 8.1.6 完整使用示例

```c
#include <linux/kfifo.h>
#include <linux/slab.h>
#include <linux/printk.h>

// 定义 kfifo（静态方式）
static DEFINE_KFIFO(my_fifo, unsigned char, 64);  // 64 字节的 FIFO

// 生产者
void producer(void)
{
    unsigned char data[10];
    int i;

    for (i = 0; i < 10; i++) {
        data[i] = i;
    }
    // 入队
    unsigned int written = kfifo_in(&my_fifo, data, 10);
    pr_info("producer: wrote %u bytes, avail=%u\n",
            written, kfifo_avail(&my_fifo));
}

// 消费者
void consumer(void)
{
    unsigned char buf[5];
    unsigned int read;

    // 出队
    read = kfifo_out(&my_fifo, buf, sizeof(buf));
    pr_info("consumer: read %u bytes:", read);
    for (unsigned int i = 0; i < read; i++)
        pr_cont(" %02x", buf[i]);
    pr_cont("\n");
}

// 动态分配方式
struct my_driver {
    struct kfifo fifo;
    spinlock_t lock;
};

int my_driver_init(struct my_driver *drv)
{
    spin_lock_init(&drv->lock);
    return kfifo_alloc(&drv->fifo, PAGE_SIZE, GFP_KERNEL);
}

void my_driver_exit(struct my_driver *drv)
{
    kfifo_free(&drv->fifo);
}

// 加锁安全的入队/出队
void my_driver_write(struct my_driver *drv, const void *data, size_t len)
{
    kfifo_in_spinlocked(&drv->fifo, data, len, &drv->lock);
}

void my_driver_read(struct my_driver *drv, void *buf, size_t len)
{
    kfifo_out_spinlocked(&drv->fifo, buf, len, &drv->lock);
}
```

#### 8.1.7 应用场景

- **内核日志 (dmesg)**：`__log_buf` 使用环形缓冲区存储内核日志
- **驱动数据传输**：串口、网络设备驱动中的数据缓冲
- **CPU 间通信**：`trace_printk` 的 per-CPU 缓冲区
- **eBPF Ringbuf**：`BPF_MAP_TYPE_RINGBUF` 基于环形缓冲区

---

### 8.2 工作队列

#### 8.2.1 概述

工作队列（Workqueue）是一种将延迟任务排队的机制，由内核线程异步处理。它本质上是**生产者-消费者队列**，生产者提交工作项，消费者（内核线程）从队列中取出并执行。

#### 8.2.2 核心数据结构

```c
// include/linux/workqueue.h
struct work_struct {
    atomic_long_t data;          // 工作状态和函数指针
    struct list_head entry;      // 链表节点，连接到工作队列
    work_func_t func;            // 工作函数
};

struct worker_pool {
    spinlock_t          lock;        // 保护队列的锁
    struct list_head    worklist;    // 待处理工作链表
    int                 nr_workers;  // 工作线程数
    ...
};
```

#### 8.2.3 操作流程

```
schedule_work(work)
  └─ __queue_work()
       ├─ 选择目标 worker_pool
       ├─ 将 work 加入 pool->worklist 链表
       └─ 唤醒 worker 内核线程

worker 线程：
  while (1) {
      work = list_first_entry(&pool->worklist, ...);
      list_del(&work->entry);
      work->func(work);        // 执行工作函数
  }
```

#### 8.2.4 应用场景

- **延迟任务执行**：中断下半部、定时回调
- **驱动编程**：设备初始化、I/O 完成处理
- **系统管理**：内存回收、文件系统同步

---

## 9. 伙伴系统 (Buddy System)

### 9.1 概述

伙伴系统是 Linux 内核物理内存管理的核心算法。它通过管理 2^order 个连续页框（page frame）的块，实现高效的分配和回收。当释放内存时，伙伴系统会尝试合并相邻的空闲块，减少外部碎片。

**内核实现 vs 传统伙伴系统的差异**

| 特性 | 内核伙伴系统 | 传统伙伴系统 |
|---|---|---|
| 迁移类型 | 按 `MIGRATE_TYPES` 分区管理（可移动/不可移动/可回收） | 无分区，所有页框统一管理 |
| Per-CPU 缓存 | 使用 `pcp`（per-CPU pages）缓存单页，减少锁竞争 | 通常无 per-CPU 优化 |
| 避免碎片 | 通过迁移类型将不同寿命的页分开，减少不可移动页导致的碎片 | 碎片问题严重，长期运行后无法分配大块连续内存 |
| 内存回收 | 集成 kswapd 和 direct reclaim，内存不足时回收 | 通常只返回分配失败 |
| 页迁移 | 支持 `MIGRATE_MOVABLE` 页的迁移（内存规整 compact） | 不支持 |
| 优点 | 碎片控制能力强，支持内存规整，per-CPU 缓存提高性能 | 实现简单，易于理解 |
| 缺点 | 实现复杂，内存管理策略高度耦合 | 碎片问题严重，不适合长期运行 |

### 9.2 核心数据结构

**空闲页链表**

```c
// include/linux/mmzone.h
struct free_area {
    struct list_head    free_list[MIGRATE_TYPES];  // 按迁移类型划分的空闲链表
    unsigned long       nr_free;                   // 当前 order 的空闲页块数
};

struct zone {
    ...
    struct free_area    free_area[MAX_ORDER];      // 每个 order 一个 free_area
    ...
};

// 最大 order 定义
#define MAX_ORDER 11  // 即最大可分配 2^10 = 1024 个连续页框
```

**页描述符中的伙伴系统字段**

```c
// include/linux/mm_types.h
struct page {
    unsigned long flags;           // 标志位（PageBuddy 等）
    union {
        struct {
            unsigned long private; // 存储伙伴阶数（order）
            ...
        };
        ...
    };
    ...
};
```

### 9.3 核心辅助函数

```c
// mm/internal.h
// 获取伙伴页框的 PFN
static inline unsigned long __find_buddy_pfn(unsigned long page_pfn,
                                             unsigned int order)
{
    return page_pfn ^ (1 << order);
}

// 判断是否为伙伴页
static inline bool page_is_buddy(struct page *page, struct page *buddy,
                                 unsigned int order)
{
    if (!page_is_guard(buddy) && !PageBuddy(buddy))
        return false;
    if (buddy_order(buddy) != order)
        return false;
    if (page_zone_id(page) != page_zone_id(buddy))
        return false;
    VM_BUG_ON_PAGE(page_count(buddy) != 0, buddy);
    return true;
}

// 获取页块的阶数
static inline unsigned int buddy_order(struct page *page)
{
    return page_private(page);
}
```

### 9.4 分配算法

```c
// mm/page_alloc.c
struct page *__rmqueue_smallest(struct zone *zone, unsigned int order,
                                int migratetype)
{
    unsigned int current_order;
    struct free_area *area;
    struct page *page;

    // 从请求的 order 开始向上查找
    for (current_order = order; current_order < MAX_ORDER; ++current_order) {
        area = &(zone->free_area[current_order]);
        page = get_page_from_free_area(area, migratetype);
        if (!page)
            continue;
        // 从空闲链表中删除
        del_page_from_free_area(page, area);
        // 如果分配的块比请求的大，将剩余部分拆分并加入低阶链表
        expand(zone, page, order, current_order, migratetype);
        return page;
    }
    return NULL;
}
```

**分配流程**

```
alloc_pages(gfp_mask, order)
  └─ __alloc_pages()
       └─ get_page_from_freelist()
            └─ rmqueue()
                 └─ __rmqueue_smallest()
                      ├─ 从 order 开始向上查找空闲块
                      ├─ 找到后从空闲链表移除
                      └─ 如果块过大，调用 expand() 拆分
```

### 9.5 释放算法

```c
// mm/page_alloc.c
static inline void __free_one_page(struct page *page,
        unsigned long pfn, struct zone *zone, unsigned int order,
        int migratetype, bool fpi_skip_pcp)
{
    unsigned long buddy_pfn;
    unsigned long combined_pfn;
    struct page *buddy;

    // 尝试反复合并伙伴块
    while (order < MAX_ORDER - 1) {
        buddy_pfn = __find_buddy_pfn(pfn, order);
        buddy = page + (buddy_pfn - pfn);
        if (!page_is_buddy(page, buddy, order))
            break;  // 伙伴不可用，停止合并

        // 从空闲链表移除伙伴
        del_page_from_free_area(buddy, &zone->free_area[order]);
        // 合并：取两个块中较小的 PFN
        combined_pfn = buddy_pfn & pfn;
        page = page + (combined_pfn - pfn);
        pfn = combined_pfn;
        order++;
    }

    // 将合并后的块加入对应 order 的空闲链表
    set_page_order(page, order);
    add_page_to_free_area(page, &zone->free_area[order], migratetype);
}
```

**释放流程**

```
__free_pages(page, order)
  └─ __free_one_page()
       ├─ 循环检查伙伴是否可以合并
       ├─ 合并后将 order 提升
       └─ 将最终块加入空闲链表
```

### 9.6 伙伴关系示例

```
order 0:  [0] [1] [2] [3] [4] [5] [6] [7] ...
order 1:  [0-1]     [2-3]     [4-5]     [6-7]     ...
order 2:  [0-3]                 [4-7]                 ...

伙伴对：
- order 0: (0,1), (2,3), (4,5), (6,7), ...
- order 1: (0-1, 2-3), (4-5, 6-7), ...
- order 2: (0-3, 4-7), ...
```

### 9.7 迁移类型

为了减少碎片，伙伴系统引入了迁移类型（MIGRATE_TYPES）：

```c
enum migratetype {
    MIGRATE_UNMOVABLE,      // 不可移动页（内核分配）
    MIGRATE_MOVABLE,        // 可移动页（用户空间分配）
    MIGRATE_RECLAIMABLE,    // 可回收页（文件缓存）
    MIGRATE_PCPTYPES,       // Per-CPU 页类型
    MIGRATE_HIGHATOMIC,     // 高优先级原子分配保留
    MIGRATE_TYPES
};
```

### 9.8 完整 API 参考

| 函数/宏 | 功能 |
|---|---|
| `alloc_pages(gfp_mask, order)` | 分配 2^order 个连续页框 |
| `alloc_page(gfp_mask)` | 分配单个页框（order=0） |
| `__get_free_pages(gfp_mask, order)` | 分配并返回虚拟地址 |
| `__get_free_page(gfp_mask)` | 分配单个页框并返回虚拟地址 |
| `get_zeroed_page(gfp_mask)` | 分配清零的页框 |
| `__free_pages(page, order)` | 释放页框 |
| `__free_page(page)` | 释放单个页框 |
| `free_pages(addr, order)` | 按虚拟地址释放 |
| `free_page(addr)` | 按虚拟地址释放单个页框 |
| `alloc_pages_exact(size, gfp_mask)` | 分配指定大小的连续页（非 2 的幂次） |
| `free_pages_exact(addr, size)` | 释放 `alloc_pages_exact` 分配的页 |
| `__get_dma_pages(gfp_mask, order)` | 分配 DMA 兼容的页框 |
| `PageBuddy(page)` | 判断页框是否在伙伴系统空闲链表中 |
| `set_page_order(page, order)` | 设置页框的伙伴阶数 |
| `split_page(page, order)` | 手动拆分大页块 |

### 9.9 完整使用示例

```c
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/printk.h>

// 分配 4 个连续页框（order=2，即 16KB）
struct page *alloc_big_buffer(void)
{
    struct page *pages;
    void *addr;

    // 分配 2^2 = 4 个连续页框
    pages = alloc_pages(GFP_KERNEL, 2);
    if (!pages)
        return NULL;

    // 获取虚拟地址
    addr = page_address(pages);
    pr_info("allocated 4 pages at %p (pfn=%lu)\n",
            addr, page_to_pfn(pages));
    return pages;
}

// 释放
void free_big_buffer(struct page *pages)
{
    __free_pages(pages, 2);
}

// 使用 __get_free_pages（直接返回虚拟地址）
void *alloc_small_buffer(void)
{
    void *addr = (void *)__get_free_pages(GFP_KERNEL, 0);  // 1 页
    if (!addr)
        return NULL;
    pr_info("allocated 1 page at %p\n", addr);
    return addr;
}

void free_small_buffer(void *addr)
{
    free_pages((unsigned long)addr, 0);
}

// 分配非 2 的幂次大小（如 3 页）
void *alloc_exact_3pages(void)
{
    void *addr = alloc_pages_exact(3 * PAGE_SIZE, GFP_KERNEL);
    if (!addr)
        return NULL;
    pr_info("allocated 3 pages at %p\n", addr);
    return addr;
}

void free_exact_3pages(void *addr)
{
    free_pages_exact(addr, 3 * PAGE_SIZE);
}
```

### 9.10 应用场景

- **物理页框分配**：`alloc_pages`、`__get_free_pages` 等接口的底层实现
- **Slab 分配器**：从伙伴系统获取连续页框作为 slab 缓存
- **DMA 缓冲区**：分配连续的 DMA 缓冲区
- **内核镜像**：启动时分配连续内存

---

## 10. 引用计数 (kref)

### 10.1 概述

`kref` 是内核中通用的引用计数机制，用于管理对象的生命周期。当引用计数降为 0 时，释放对象。

### 10.2 核心数据结构

```c
// include/linux/kref.h
struct kref {
    refcount_t refcount;
};
```

### 10.3 核心操作

```c
// 初始化
void kref_init(struct kref *kref);

// 获取引用（递增计数）
void kref_get(struct kref *kref);

// 释放引用（递减计数，若为 0 则调用 release 回调）
int kref_put(struct kref *kref, void (*release)(struct kref *kref));

// 安全获取（如果计数已为 0，则不递增）
int kref_get_unless_zero(struct kref *kref);
```

### 10.4 完整 API 参考

| 函数 | 功能 |
|---|---|
| `kref_init(kref)` | 初始化引用计数为 1 |
| `kref_read(kref)` | 读取当前引用计数 |
| `kref_get(kref)` | 递增引用计数（必须确保对象存活） |
| `kref_get_unless_zero(kref)` | 安全递增（计数为 0 则不操作） |
| `kref_put(kref, release)` | 递减，若为 0 则调用 release |
| `kref_put_mutex(kref, release, mutex)` | 在 mutex 保护下 put |
| `kref_put_lock(kref, release, lock)` | 在 spinlock 保护下 put |
| `kref_sub(kref, count, release)` | 一次性递减多个计数 |
| `kref_mutex_init(kref, release, mutex)` | 初始化并关联 mutex |

### 10.5 完整使用示例

```c
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/module.h>

// 引用计数管理的数据结构
struct my_object {
    struct kref refcount;  // 必须作为第一个字段（或使用 container_of）
    int id;
    void *data;
};

// 释放回调
static void my_object_release(struct kref *ref)
{
    struct my_object *obj = container_of(ref, struct my_object, refcount);
    pr_info("releasing object id=%d\n", obj->id);
    kfree(obj);
}

// 初始化（引用计数 = 1）
struct my_object *my_object_alloc(int id)
{
    struct my_object *obj = kmalloc(sizeof(*obj), GFP_KERNEL);
    if (!obj)
        return NULL;
    obj->id = id;
    kref_init(&obj->refcount);  // refcount = 1
    return obj;
}

// 获取引用
void my_object_get(struct my_object *obj)
{
    kref_get(&obj->refcount);  // 原子递增
}

// 释放引用
void my_object_put(struct my_object *obj)
{
    kref_put(&obj->refcount, my_object_release);  // 原子递减，到 0 时调用 release
}

// 使用场景示例
void usage_example(void)
{
    struct my_object *obj = my_object_alloc(42);
    if (!obj)
        return;

    // 当前引用计数 = 1

    my_object_get(obj);  // 引用计数 = 2
    // ... 在其他线程中并发使用 ...
    my_object_put(obj);  // 引用计数 = 1
    my_object_put(obj);  // 引用计数 = 0 → 调用 release → kfree
}
```

### 10.6 应用场景

- **设备模型**：`struct device` 的生命周期管理
- **文件系统**：`struct file`、`struct inode` 的引用计数
- **网络协议栈**：socket 对象的引用管理

---

## 11. 核心思想：权衡与优化

### 11.1 时间与空间的权衡

内核设计数据结构和算法时，始终在**时间**和**空间**之间做权衡：

| 数据结构 | 查找 | 插入/删除 | 空间开销 | 适用场景 |
|---|---|---|---|---|
| 双向链表 | O(n) | O(1) | 2 指针/节点 | 动态变化频繁的集合 |
| 哈希表 | O(1) avg | O(1) | 桶数组 + 链表 | 快速精确查找 |
| 红黑树 | O(log n) | O(log n) | 3 指针/节点 | 需要有序性 |
| 基数树/XArray | O(k) | O(k) | 多级节点 | 稀疏整数索引 |
| 位图 | O(1) | O(1) | 1 bit/元素 | 大量"有/无"状态 |
| 伙伴系统 | O(log n) | O(log n) | 链表数组 | 连续内存管理 |

### 11.2 演进趋势

Linux 内核数据结构在不断演进，核心趋势是**向范围查询优化和缓存友好方向**发展：

- **Maple Tree 替代红黑树**：VMA 管理从红黑树迁移到 Maple Tree，范围查找效率更高
- **XArray 替代基数树**：页缓存管理从 radix tree 迁移到 XArray，API 更简洁、RCU 安全更好
- **eBPF Maps 统一化**：提供多种底层实现的通用键值接口
- **无锁和 RCU 化**：越来越多的数据结构支持 RCU 无锁读取，适应多核扩展

### 11.3 选择指南

在内核开发中选择数据结构时，遵循以下原则：

1. **需要有序集合**？ → 红黑树（Maple Tree 正在成为新标准）
2. **需要快速精确查找**？ → 哈希表
3. **需要范围查询**？ → Maple Tree / B-Tree
4. **需要动态插入/删除**？ → 链表
5. **需要稀疏整数索引**？ → XArray
6. **需要连续内存分配**？ → 伙伴系统
7. **需要大量二进制状态**？ → 位图
8. **需要生产者-消费者通信**？ → kfifo / 工作队列

---

## 12. 附录：关键文件索引

| 数据结构/算法 | 头文件 | 实现文件 |
|---|---|---|
| 双向链表 (list_head) | `include/linux/list.h` | 内联实现 |
| 哈希链表 (hlist_head) | `include/linux/list.h` | 内联实现 |
| 哈希表 | `include/linux/hashtable.h` | 内联实现 |
| 红黑树 (rb_node) | `include/linux/rbtree.h` | `lib/rbtree.c` |
| 基数树 (历史) | `include/linux/radix-tree.h` | `lib/radix-tree.c` |
| XArray | `include/linux/xarray.h` | `lib/xarray.c` |
| Maple Tree | `include/linux/maple_tree.h` | `lib/maple_tree.c` |
| 伙伴系统 | `include/linux/mmzone.h` | `mm/page_alloc.c` |
| 位图 | `include/linux/bitmap.h` | `lib/bitmap.c` |
| 引用计数 (kref) | `include/linux/kref.h` | `lib/kref.c` |
| IDR | `include/linux/idr.h` | `lib/idr.c` |
| kfifo | `include/linux/kfifo.h` | `lib/kfifo.c` |
| 工作队列 | `include/linux/workqueue.h` | `kernel/workqueue.c` |
| eBPF Maps | `include/linux/bpf.h` | `kernel/bpf/` |
| 时间轮定时器 | `include/linux/timer.h` | `kernel/time/timer.c` |
| 实时调度队列 | `kernel/sched/sched.h` | `kernel/sched/rt.c` |
| 路由 Trie | `include/net/fib_trie.h` | `net/ipv4/fib_trie.c` |
| RCU 链表 | `include/linux/rculist.h` | 内联实现 |

---

## 数据结构关系图

```
                    +------------------+
                    |  物理内存管理     |
                    |  (伙伴系统)       |
                    +--------+---------+
                             |
              +--------------+--------------+
              |              |              |
         +----v---+   +-----v----+   +-----v----+
         | 页缓存 |   | Slab分配器|   | VMA管理  |
         | (XArray)|   | (list_head)|  |(MapleTree)|
         +--------+   +----------+   +----------+
              |              |              |
              v              v              v
         +--------+   +----------+   +----------+
         | 文件系统 |   | 内核对象 |   | 进程地址  |
         | 页缓存 |   | 缓存管理 |   | 空间管理  |
         +--------+   +----------+   +----------+

         +-------------------+   +-------------------+
         | 进程调度          |   | 文件系统 dentry   |
         | CFS: 红黑树       |   | 哈希表管理路径缓存 |
         | 实时: 位图+链表   |   +-------------------+
         | 定时器: 时间轮    |
         +-------------------+

         +-------------------+   +-------------------+
         | 网络子系统        |   | 驱动与通信        |
         | 路由表: Trie树    |   | kfifo: 环形缓冲区 |
         | conntrack: 哈希表 |   | 工作队列: 延迟执行 |
         | TCP: 红黑树       |   | eBPF Maps: 键值对 |
         +-------------------+   +-------------------+
```

---