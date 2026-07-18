# 内存管理 — 总结与附录 (Part VI)

> 本文档拆分自 [memory_management.md](memory_management.md) Part VI，涵盖总结、架构层次、设计原则、代码量统计

## Part VI: 附录

## 24. 总结

### 24.1 架构层次

```
用户空间 (malloc/free/mmap/munmap)
        │
        ▼
┌──────────────────────────────────────┐
│  系统调用层                            │
│  (mmap, mremap, mprotect, mlock,     │
│   madvise, mseal, mbind, ...)         │
├──────────────────────────────────────┤
│  VMA 管理 (mmap.c, vma.c, vma_exec.c) │
│  └─ 红黑树查找/合并/分裂/扩展/收缩     │
├──────────────────────────────────────┤
│  缺页处理 (memory.c)                  │
│  ├─ do_anonymous_page / do_fault      │
│  ├─ do_swap_page / do_numa_page       │
│  └─ wp_page_copy (COW)               │
├──────────────────────────────────────┤
│  Page Cache (filemap.c, readahead.c)  │
│  └─ XArray 页面查找/预读/写回
├──────────────────────────────────────┤
│  SLUB (slub.c) / vmalloc (vmalloc.c) │
│  ├─ kmalloc/kfree (小对象快速分配)     │
│  └─ vmalloc/vfree (大块虚拟连续)      │
├──────────────────────────────────────┤
│  伙伴系统 (page_alloc.c)              │
│  ├─ 2^N 阶空闲链表 / 迁移类型         │
│  ├─ Per-CPU pageset (order-0 加速)    │
│  └─ 水位线控制 / 回收触发             │
├──────────────────────────────────────┤
│  MemBlock (memblock.c)               │
│  └─ 启动时物理内存分配器              │
└──────────────────────────────────────┘
```

### 24.2 关键平衡策略

1. **内存 vs 性能**：Per-CPU pageset、SLUB cpu_slab 等缓存机制减少锁竞争
2. **内存 vs I/O**：Page Cache 缓存文件数据，脏页写回平衡内存与磁盘
3. **回收 vs 分配**：kswapd 异步回收 vs 直接回收，水位线控制阈值
4. **压缩 vs 碎片**：kcompactd 后台压缩 vs 分配时直接压缩
5. **公平 vs 效率**：memcg 按 cgroup 限制，LRU 按活跃度排序

### 24.3 代码量统计

| 模块 | 行数 | 占比 |
|------|------|------|
| 核心分配器（slub + page_alloc） | 17,695 | 8.2% |
| 虚拟内存管理（memory + vma + vmalloc + mmap） | 17,789 | 8.3% |
| 页面回收与交换（vmscan + swap*） | 12,993 | 6.1% |
| 大页（hugetlb + huge_memory + khugepaged + ksm） | 19,210 | 9.0% |
| 文件缓存（filemap + writeback + readahead） | 8,784 | 4.1% |
| 控制组（memcontrol + memcontrol-v1 + vmpressure） | 8,022 | 3.7% |
| 调试工具（kasan + kfence + kmsan + debug*） | ~15,000 | 7.0% |
| DAMON（core + sysfs + vaddr + paddr） | ~10,000 | 4.7% |
| NUMA/策略（mempolicy + memory-tiers + numa*） | ~7,000 | 3.3% |
| 其他（migrate + compaction + mmap_lock + cma + zswap + 等） | ~50,000 | 23.3% |
| 头文件（include/linux/mm*.h + include/linux/*.h） | ~30,000 | 14.0% |
| **总计** | ~214,524 | 100% |

### 24.4 架构设计原则

- **分层抽象**：从伙伴系统到 SLUB/vmalloc 再到用户空间 VMA，层层封装
- **惰性分配**：页表、物理页面在真正访问时才分配（缺页处理）
- **批量操作**：Per-CPU pageset 批量填充、swap 簇预读、LRU 批量隔离
- **异步处理**：kswapd、kcompactd、khugepaged、DAMON 等内核线程异步执行
- **可配置性**：大量 Kconfig 选项和 sysctl 参数，适应不同场景
- **可观测性**：/proc/meminfo、/sys/devices/system/node/、memcg 统计、tracepoints

### 24.5 Linux 7.0 内存管理关键更新汇总

| 特性 | 模块 | 收益 |
|------|------|------|
| **kmalloc_obj API** | SLUB | 基于类型的分配，更精细的内存管理 |
| **Sheaves 缓存** | SLUB | 降低锁开销，提高 NUMA 扩展性 |
| **Large Folios 批量回收** | vmscan | 大页回收速度提升 50%~75% |
| **Gigantic Folio 分配加速** | page_alloc | 1GB 巨页分配 8.4x 加速 |
| **Swap 代码清理** | swap | Redis 基准测试性能提升 20% |
| **Zram 写回** | zram | 更好的内存压力管理，能效提升 |
| **PT_RECLAIM** | mm/memory | 更高效地回收页表内存 |
| **更智能的分配策略** | page_alloc | 减少碎片，提高分配成功率 |

### 24.6 启动内存管理总结

Linux 内核启动内存管理是 **从 memblock 到 Buddy + Slab 的平稳过渡**：

```
start_kernel
  │
  ├─ setup_arch()
  │    └─ arm64_memblock_init()    // memblock 启动分配器
  │    └─ paging_init()            // 建立内核页表映射
  │
  └─ mm_init()
       └─ kmem_cache_init()        // SLUB 初始化
       └─ mem_init()               // memblock → 伙伴系统移交
       └─ page_alloc_init()        // 伙伴系统初始化
```

三个阶段：
1. **memblock 阶段**：极简分配器，管理物理内存
2. **地址映射建立**：打通虚拟地址到物理内存的通道
3. **核心分配器就绪**：伙伴系统管理物理页，SLUB 管理小对象