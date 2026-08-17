# madvise 系统调用分析

## 1. 概述

`madvise` 系统调用向内核提供关于内存使用模式的建议（advisory），帮助内核优化内存管理策略，如预读（readahead）、页面回收、透明大页合并等。建议是 advisory 的，内核可以忽略。

**内核源码位置：** `mm/madvise.c`

**原型：**

```c
SYSCALL_DEFINE3(madvise, unsigned long, start, size_t, len_in, int, behavior)
```

| 参数 | 描述 |
|------|------|
| `start` | 起始地址（必须页对齐） |
| `len_in` | 区域长度（字节） |
| `behavior` | 建议行为（见下方） |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 支持的 behavior 值

| behavior 值 | 描述 | 锁模式 |
|------------|------|--------|
| `MADV_NORMAL` | 默认策略，无特殊预读 | 写锁 |
| `MADV_RANDOM` | 随机访问，禁用预读 | 写锁 |
| `MADV_SEQUENTIAL` | 顺序访问，积极预读，访问后释放 | 写锁 |
| `MADV_WILLNEED` | 预读页面到内存 | 读锁 |
| `MADV_DONTNEED` | 立即释放页面（匿名内存零填充） | VMA 读锁 |
| `MADV_FREE` | 标记页面可回收（延迟释放） | VMA 读锁 |
| `MADV_REMOVE` | 释放页面及后备存储（fallocate 类似） | 读锁 |
| `MADV_DONTFORK` | fork 时不复制此区域 | 写锁 |
| `MADV_DOFORK` | 取消 MADV_DONTFORK | 写锁 |
| `MADV_WIPEONFORK` | fork 后子进程此区域零填充 | 写锁 |
| `MADV_KEEPONFORK` | 取消 MADV_WIPEONFORK | 写锁 |
| `MADV_COLD` | 标记页面为冷页，优先回收 | 读锁 |
| `MADV_PAGEOUT` | 立即换出页面 | 读锁 |
| `MADV_HUGEPAGE` | 建议使用透明大页（THP） | 写锁 |
| `MADV_NOHUGEPAGE` | 不建议使用透明大页 | 写锁 |
| `MADV_COLLAPSE` | 同步合并页面为 THP | 读锁 |
| `MADV_MERGEABLE` | 启用 KSM 页面合并 | 写锁 |
| `MADV_UNMERGEABLE` | 禁用 KSM 页面合并 | 写锁 |
| `MADV_DONTDUMP` | 排除在 core dump 之外 | 写锁 |
| `MADV_DODUMP` | 包含在 core dump 中 | 写锁 |
| `MADV_POPULATE_READ` | 预填充页表（读缺页） | 读锁 |
| `MADV_POPULATE_WRITE` | 预填充页表（写缺页） | 读锁 |
| `MADV_GUARD_INSTALL` | 安装保护页 | VMA 读锁 |
| `MADV_GUARD_REMOVE` | 移除保护页 | VMA 读锁 |

## 3. 函数调用链分析

```
madvise(addr, len, behavior)                        // 系统调用入口
  └─ do_madvise(mm, start, len_in, behavior)        // 核心处理
       ├─ madvise_should_skip()                     // 跳过空区域
       ├─ madvise_lock()                            // 根据 behavior 获取锁
       ├─ madvise_init_tlb()                        // 初始化 TLB 收集
       ├─ madvise_do_behavior()                     // 执行具体行为
       │    └─ madvise_walk_vmas()                   // 遍历 VMA 并应用
       │         ├─ madvise_vma_behavior()           // 对单个 VMA 应用行为
       │         │    ├─ MADV_REMOVE
       │         │    │    └─ madvise_remove()
       │         │    │         └─ zap_page_range() / fallocate()
       │         │    ├─ MADV_WILLNEED
       │         │    │    └─ madvise_willneed()
       │         │    │         └─ force_page_cache_readahead()
       │         │    ├─ MADV_COLD
       │         │    │    └─ madvise_cold()
       │         │    │         └─ madvise_cold_or_pageout_pte_range()
       │         │    │              └─ folio_deactivate()
       │         │    ├─ MADV_PAGEOUT
       │         │    │    └─ madvise_pageout()
       │         │    │         └─ madvise_cold_or_pageout_pte_range()
       │         │    │              └─ folio_reclaim / folio_isolate_lru
       │         │    ├─ MADV_FREE/DONTNEED/DONTNEED_LOCKED
       │         │    │    └─ madvise_dontneed_free()
       │         │    │         └─ zap_page_range() / lru_add_drain_all()
       │         │    ├─ MADV_COLLAPSE
       │         │    │    └─ madvise_collapse()
       │         │    │         └─ khugepaged 机制
       │         │    ├─ MADV_MERGEABLE/UNMERGEABLE
       │         │    │    └─ ksm_madvise()
       │         │    ├─ MADV_HUGEPAGE/NOHUGEPAGE
       │         │    │    └─ hugepage_madvise()
       │         │    └─ 其他（修改 vm_flags 的行为）
       │         │         └─ madvise_update_vma()
       │         │              └─ vma_modify_flags()
       │         └─ 返回 unmapped_error
       ├─ madvise_finish_tlb()                      // 完成 TLB 刷新
       └─ madvise_unlock()                          // 释放锁
```

## 4. 关键数据结构

### `struct madvise_behavior`

```c
struct madvise_behavior {
    struct mm_struct *mm;              /* 目标 mm_struct */
    int behavior;                      /* 建议行为类型 */
    struct mmu_gather *tlb;            /* TLB 收集器 */
    struct madvise_behavior_range range; /* 地址范围 */
    struct vm_area_struct *prev;       /* 前一个 VMA */
    struct vm_area_struct *vma;        /* 当前 VMA */
    struct anon_vma_name *anon_name;   /* 匿名 VMA 名称 */
    bool lock_dropped;                 /* 锁是否已释放 */
    enum madvise_lock_mode lock_mode;  /* 锁模式 */
};
```

### `struct madvise_behavior_range`

```c
struct madvise_behavior_range {
    unsigned long start;    /* 起始地址 */
    unsigned long end;      /* 结束地址 */
};
```

### 锁模式枚举

```c
enum madvise_lock_mode {
    MADVISE_NO_LOCK,        /* 不需要锁（如内存错误注入） */
    MADVISE_MMAP_READ_LOCK, /* mmap 读锁 */
    MADVISE_MMAP_WRITE_LOCK,/* mmap 写锁 */
    MADVISE_VMA_READ_LOCK,  /* VMA 读锁（更细粒度） */
};
```

## 5. 流程图

```
  用户态调用 madvise(addr, len, behavior)
         │
         ▼
  ┌──────────────────────────────┐
  │  do_madvise()                │
  │  madvise_should_skip()       │
  │  madvise_lock()              │  根据 behavior 选择锁模式
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  madvise_do_behavior()       │
  │  ┌────────────────────────┐  │
  │  │  madvise_walk_vmas()   │  │  遍历 VMA 列表
  │  │  for each VMA:         │  │
  │  │  ┌──────────────────┐  │  │
  │  │  │ madvise_vma_     │  │  │  对单个 VMA 处理
  │  │  │ behavior()       │  │  │
  │  │  │                  │  │  │
  │  │  │ switch(behavior) │  │  │
  │  │  │ ├─ MADV_NORMAL   │  │  │  → 设置 vm_flags
  │  │  │ ├─ MADV_WILLNEED │  │  │  → 预读页面
  │  │  │ ├─ MADV_DONTNEED │  │  │  → 释放页面
  │  │  │ ├─ MADV_COLD     │  │  │  → 冷页处理
  │  │  │ ├─ MADV_PAGEOUT  │  │  │  → 换出页面
  │  │  │ ├─ MADV_MERGEABLE│  │  │  → KSM 合并
  │  │  │ ├─ MADV_HUGEPAGE │  │  │  → THP 建议
  │  │  │ └─ ...           │  │  │
  │  │  └──────────────────┘  │  │
  │  │  next VMA              │  │
  │  └────────────────────────┘  │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  madvise_finish_tlb()        │  TLB 刷新
  │  madvise_unlock()            │
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | start 未页对齐、无效 behavior、start+len 溢出 |
| `-ENOMEM` | 地址范围包含未映射区域 |
| `-EPERM` | 内存已被 mseal 密封 |
| `-EACCES` | 操作不允许（如对只读区域执行 MADV_REMOVE） |
| `-EBADF` | 映射不是文件映射（MADV_REMOVE 需要文件映射） |
| `-EAGAIN` | 内核资源暂时不可用 |
| `-EIO` | I/O 错误（MADV_WILLNEED 时） |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

int main() {
    size_t len = 1024 * 1024;  /* 1MB */
    char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* 写入数据 */
    memset(addr, 'A', len);

    /* 建议顺序访问 */
    madvise(addr, len, MADV_SEQUENTIAL);

    /* 顺序读取 */
    for (size_t i = 0; i < len; i += 4096) {
        volatile char c = addr[i];
        (void)c;
    }

    /* 告诉内核不再需要这些数据 */
    madvise(addr, len, MADV_DONTNEED);

    /* 再次访问（匿名内存 MADV_DONTNEED 后零填充） */
    printf("After DONTNEED: %c\n", addr[0]);  /* 输出 '\0' */

    munmap(addr, len);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | madvise | fadvise | process_madvise | mprotect |
|------|---------|---------|----------------|---------|
| 作用对象 | 进程自身内存 | 文件描述符 | 其他进程内存 | 进程自身内存 |
| 权限要求 | 无 | 无 | CAP_SYS_NICE | 无 |
| 行为类型 | 20+ 种 | 6 种 | 有限子集 | 仅权限修改 |
| 是否修改 VMA | 部分行为是 | 否 | 部分行为 | 是 |
| 是否触发 I/O | WILLNEED 可 | 是 | 有限 | 否 |

## 9. 关键实现细节

1. **锁模式选择**：`get_lock_mode()` 根据 behavior 类型选择不同的锁粒度。修改 VMA 标志的行为需要写锁，纯遍历的行为只需要读锁，MADV_DONTNEED 等使用 VMA 级别的读锁以提高并发性。

2. **MADV_DONTNEED vs MADV_FREE**：
   - MADV_DONTNEED：立即释放页面，匿名内存下次访问时零填充
   - MADV_FREE：仅标记页面可回收，内存压力时才真正回收，内容保留

3. **MADV_COLD 与 MADV_PAGEOUT**：
   - MADV_COLD：将页面移到 inactive 列表尾部，使其优先被回收
   - MADV_PAGEOUT：立即尝试将页面换出到交换分区

4. **MADV_WILLNEED 实现**：通过 `force_page_cache_readahead()` 触发文件映射的预读，对匿名映射则触发缺页中断。

5. **mseal 交互**：对于已密封（mseal）的内存区域，任何修改 VMA 的 madvise 操作都会返回 `-EPERM`。

6. **MADV_POPULATE_READ/WRITE**：通过触发缺页异常（page fault）来预填充页表，当下次访问时不会触发缺页。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/madvise.c`
- 内核源码：`include/uapi/asm-generic/mman-common.h`（behavior 常量定义）