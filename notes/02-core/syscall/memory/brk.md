# brk 系统调用分析

## 1. 概述

`brk` 系统调用用于改变进程堆（heap）的边界。堆是进程数据段之后的一段动态内存区域，通常通过 `brk`/`sbrk` 或 `malloc` 库函数进行管理。

**构建配置:**

- 架构: ARM64 (arm64)
- 页大小: 4KB (`CONFIG_ARM64_4K_PAGES=y`, `PAGE_SHIFT=12`)
- 兼容 brk: 未启用 (`# CONFIG_COMPAT_BRK is not set` → `min_brk = mm->start_brk`)

**原型:**

```c
SYSCALL_DEFINE1(brk, unsigned long, brk)
```

| 参数 | 描述 |
|------|------|
| `brk` | 新的堆结束地址（0 表示返回当前 brk 值） |

**返回值：**
- 成功时返回新的 brk 地址
- 失败时返回原来的 brk 地址（注意：不是返回 -1 错误码）

## 2. brk 完整调用链

### 2.1 系统调用入口（ARM64）

```
用户态: brk(new_addr)
    ↓ SVC #0 异常, CPU 切换至 EL1
    ↓ ESR_EL1.EC = 0x15 (SVC from AArch64)

[arch/arm64/kernel/entry.S]
  vectors:
    kernel_ventry 0, t, 64, sync       // EL0 64-bit 同步异常向量
    ↓

[arch/arm64/kernel/entry-common.c]
  el0t_64_sync_handler()
    switch (ESR_ELx_EC(esr)) {
    case ESR_ELx_EC_SVC64:  // 0x15
      el0_svc(regs)                     // 系统调用入口
        ↓
      do_el0_svc(regs)
        el0_svc_common(regs, regs->regs[8], __NR_syscalls, sys_call_table)
          ↓

[arch/arm64/kernel/syscall.c]
  invoke_syscall(regs, scno, sc_nr, syscall_table)
    syscall_fn = syscall_table[__NR_brk]  // 查系统调用表
    ret = __invoke_syscall(regs, syscall_fn)
    syscall_set_return_value(current, regs, 0, ret)
    ↓ 通过函数指针调用

  brk 系统调用编号 (ARM64: __NR_brk = 214)
```

### 2.2 brk 内核实现路径

```
SYSCALL_DEFINE1(brk, unsigned long, brk)     [mm/mmap.c]
  │
  ├─ mmap_write_lock_killable(mm)             // 获取 mmap 写锁
  ├─ origbrk = mm->brk                        // 保存原始 brk
  ├─ min_brk = mm->start_brk                  // CONFIG_COMPAT_BRK 未设置
  │                                            // 所以 min_brk = start_brk
  ├─ brk < min_brk ? → goto out               // 不允许 brk 低于堆起始
  ├─ check_data_rlimit(...)                    // 检查 RLIMIT_DATA
  ├─ newbrk = PAGE_ALIGN(brk)                 // 页对齐
  ├─ oldbrk = PAGE_ALIGN(mm->brk)
  │
  ├─ oldbrk == newbrk ?                       // 页面对齐后无变化
  │   └─ mm->brk = brk; goto success          // 仅更新未对齐的偏移
  │
  ├─ brk <= mm->brk ?                         // 缩小堆路径
  │   ├─ vma_find(&vmi, oldbrk)               // 查找包含 oldbrk 的 VMA
  │   ├─ brkvma->vm_start >= oldbrk → out     // 非 brk VMA 冲突
  │   ├─ mm->brk = brk                        // 先更新 brk
  │   ├─ do_vmi_align_munmap(... unlock=true)  // 解除映射（成功时释放锁）
  │   └─ goto success_unlocked
  │
  └─ brk > mm->brk ?                          // 扩大堆路径
      ├─ check_brk_limits(oldbrk, newbrk - oldbrk)
      │   └─ get_unmapped_area(NULL, addr, len, 0, MAP_FIXED)
      │       检查地址空间是否可用（不与现有映射重叠）
      │   └─ mlock_future_ok()                 // 检查 mlock 限制
      │
      ├─ stack_guard_gap 检查                  // 检查与下一个 VMA 的栈间隙
      │   └─ vma_find(&vmi, newbrk + PAGE_SIZE + stack_guard_gap)
      │        newbrk + PAGE_SIZE > vm_start_gap(next) → out
      │
      ├─ brkvma = vma_prev_limit(&vmi, mm->start_brk)
      │
      ├─ do_brk_flags(&vmi, brkvma, oldbrk, newbrk - oldbrk, 0)
      │   [mm/vma.c]
      │   │
      │   ├─ vm_flags = VM_DATA_DEFAULT_FLAGS | VM_ACCOUNT | mm->def_flags
      │   │   ARM64 下 VM_DATA_DEFAULT_FLAGS = VM_DATA_FLAGS_TSK_EXEC | VM_MTE_ALLOWED
      │   │   = (VM_READ | VM_WRITE | TASK_EXEC | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC) | VM_MTE_ALLOWED
      │   │
      │   ├─ may_expand_vm(mm, vm_flags, len >> PAGE_SHIFT)  // 检查地址空间限制
      │   ├─ mm->map_count > sysctl_max_map_count → -ENOMEM
      │   ├─ security_vm_enough_memory_mm(mm, len)            // LSM 安全审计
      │   │
      │   ├─ [尝试扩展已有 VMA]
      │   │   brkvma && brkvma->vm_end == addr ?
      │   │   └─ vma_merge_new_range(&vmg)                   // 向后扩展相邻 VMA
      │   │       vmg.just_expand = true                      // 仅扩展标志
      │   │
      │   └─ [创建新 VMA]
      │       ├─ vm_area_alloc(mm)                            // 分配 VMA 结构
      │       ├─ vma_set_anonymous(vma)                       // 标记为匿名映射
      │       ├─ vma_set_range(vma, addr, addr+len, pgoff)    // 设置地址范围
      │       ├─ vm_flags_init(vma, vm_flags)                 // 初始化标志
      │       ├─ vma->vm_page_prot = vm_get_page_prot(vm_flags) // 页权限
      │       ├─ vma_iter_store_gfp(vmi, vma)                 // 插入 maple tree
      │       └─ mm->map_count++
      │
      ├─ perf_event_mmap(vma)                                // 性能事件通知
      ├─ mm->total_vm += len >> PAGE_SHIFT
      ├─ mm->data_vm += len >> PAGE_SHIFT
      │
      ├─ mm->brk = brk                                       // 更新堆边界
      ├─ mm->def_flags & VM_LOCKED → populate = true         // 需要预填充
      │
      ├─ [success]
      │   mmap_write_unlock(mm)
      ├─ mm_populate(oldbrk, newbrk - oldbrk)                // 锁定/预填充页表
      └─ return brk
```

## 3. 缺页中断流程（访问新 brk 内存时触发）

当用户态访问 `brk` 扩展后但尚未分配物理页的内存时，MMU 触发缺页中断。

### 3.1 硬件异常捕获

```
用户态: *p = 42 （访问堆内存）
    ↓ CPU 无法翻译虚拟地址, 触发 Data Abort
    ↓ 硬件填写 FAR_EL1 = 访问地址, ESR_EL1 = 异常综合征

[arm64 异常向量]
  el0t_64_sync_handler()
    ESR_ELx_EC = 0x24 (DABT_LOW: 数据中止, EL0)
    → el0_da(regs, esr)

[arch/arm64/kernel/entry-common.c]
  el0_da()
    ├─ arm64_enter_from_user_mode(regs)       // 退出用户模式
    ├─ far = read_sysreg(far_el1)             // 读取故障地址
    ├─ local_daif_restore(DAIF_PROCCTX)       // 使能本地中断
    ├─ do_mem_abort(far, esr, regs)           // C 语言异常处理入口
    └─ arm64_exit_to_user_mode(regs)
```

### 3.2 异常分发

```
[arch/arm64/mm/fault.c]
  do_mem_abort(far, esr, regs)
    ├─ inf = esr_to_fault_info(esr)            // 根据 FSC 查询 fault_info 表
    │   ESR_ELx_FSC = 0b000101 (level 3 translation fault)
    │   fault_info[0x05] = {do_translation_fault, SIGSEGV, SEGV_MAPERR, "level 3 translation fault"}
    └─ inf->fn(far, esr, regs)                 // 调用 do_translation_fault

  do_translation_fault(far, esr, regs)
    └─ is_ttbr0_addr(addr) ?                  // 用户地址?
         → do_page_fault(far, esr, regs)       // 是, 进入缺页处理
```

### 3.3 缺页处理主路径

```
[arch/arm64/mm/fault.c]
  do_page_fault(far, esr, regs)
    │
    ├─ 确定访问类型:
    │   is_write_abort(esr) → vm_flags = VM_WRITE
    │                          mm_flags |= FAULT_FLAG_WRITE
    │   user_mode(regs)     → mm_flags |= FAULT_FLAG_USER
    │
    ├─ perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS, ...)  // 统计缺页
    │
    ├─ [快速路径: VMA lock under RCU]
    │   if (mm_flags & FAULT_FLAG_USER) {
    │     vma = lock_vma_under_rcu(mm, addr)           // 获取 VMA 读锁
    │     if (!vma) goto lock_mmap                     // 失败则降级
    │     vma->vm_flags & vm_flags 检查               // 权限校验
    │     fault = handle_mm_fault(vma, addr, mm_flags | FAULT_FLAG_VMA_LOCK, regs)
    │     if (!(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
    │         vma_end_read(vma)                        // 释放 VMA 锁
    │     if (!(fault & VM_FAULT_RETRY))  goto done    // 成功
    │   }
    │
    ├─ [慢速路径: mmap_lock 保护]
    │   lock_mmap:
    │   vma = lock_mm_and_find_vma(mm, addr, regs)    // 获取 mmap 读锁 + 查找 VMA
    │   if (!vma) → SEGV_MAPERR                        // 未找到 VMA
    │   vma->vm_flags & vm_flags 检查                  // 权限校验
    │   fault = handle_mm_fault(vma, addr, mm_flags, regs)
    │   mmap_read_unlock(mm)
    │
    └─ [错误处理]
        fault & VM_FAULT_OOM   → pagefault_out_of_memory()
        fault & VM_FAULT_SIGBUS → arm64_force_sig_fault(SIGBUS)
        else                    → arm64_force_sig_fault(SIGSEGV)
```

### 3.4 核心缺页处理

```
[mm/memory.c]
  handle_mm_fault(vma, address, flags, regs)
    ├─ sanitize_fault_flags(vma, &flags)               // 清理/校验 flags
    ├─ arch_vma_access_permitted(vma, ...)             // 架构权限检查
    ├─ mem_cgroup_enter_user_fault()                   // memcg OOM 上下文
    ├─ is_vm_hugetlb_page(vma) ? 巨页路径 : 常规路径
    │
    └─ __handle_mm_fault(vma, address, flags)
         │
         ├─ pgd = pgd_offset(mm, address)               // 获取 PGD
         ├─ p4d = p4d_alloc(mm, pgd, address)           // 分配 p4d 页表 (如需要)
         ├─ vmf.pud = pud_alloc(mm, p4d, address)       // 分配 pud 页表 (如需要)
         ├─ vmf.pmd = pmd_alloc(mm, vmf.pud, address)   // 分配 pmd 页表 (如需要)
         │
         ├─ [检查 PMD 是否是透明巨页]
         │   pmd_none() && thp_vma_allowable_order() → create_huge_pmd()
         │
         └─ handle_pte_fault(&vmf)                      // 进入 PTE 级别处理
              │
              ├─ pmd_none(*vmf->pmd) ?                  // PMD 为空(无页表)
              │   → vmf->pte = NULL
              │   → 否则 pte_offset_map_rw_nolock + 读取 orig_pte
              │
              ├─ vmf->pte == NULL ?                     // PTE 为空
              │   → do_pte_missing(vmf)
              │
              └─ do_pte_missing(vmf)                    // 处理缺页
                   └─ vma_is_anonymous(vmf->vma) ?      // 匿名映射 VMA?
                        → do_anonymous_page(vmf)        // 分配匿名页
```

### 3.5 匿名页分配

```
[mm/memory.c]
  do_anonymous_page(vmf)
    │
    ├─ VM_SHARED → VM_FAULT_SIGBUS                     // 匿名页不支持共享
    ├─ pte_alloc(vma->vm_mm, vmf->pmd)                 // 确保 PTE 页表存在
    │
    ├─ [读零页优化] 非写故障 && 不禁用零页?
    │   → 使用 my_zero_pfn 的零页 (pte_special)
    │     entry = pte_mkspecial(pfn_pte(my_zero_pfn, vma->vm_page_prot))
    │     goto setpte
    │
    ├─ [写故障: 分配物理页]
    │   ├─ vmf_anon_prepare(vmf)
    │   │    ├─ vma->anon_vma ? 直接返回              // 已有 anon_vma
    │   │    └─ __anon_vma_prepare(vma)                // 创建 anon_vma
    │   │
    │   ├─ alloc_anon_folio(vmf)                       // 分配 folio
    │   │    ├─ 优先尝试 large folio (根据 VMA 和系统状态)
    │   │    └─ 回退至单页 (PAGE_SIZE = 4096)
    │   │
    │   ├─ __folio_mark_uptodate(folio)                // 标记为 up-to-date
    │   │
    │   ├─ 构建 PTE:
    │   │   entry = folio_mk_pte(folio, vma->vm_page_prot)
    │   │   entry = pte_sw_mkyoung(entry)               // 标记 young (AF)
    │   │   VM_WRITE → pte_mkwrite(pte_mkdirty(entry))  // 标记可写 + 脏
    │   │
    │   ├─ pte_offset_map_lock(...)                     // 获取 PTE 指针和锁
    │   ├─ pte_range_none / vmf_pte_changed 检查        // 并发检测
    │   │
    │   ├─ add_mm_counter(vma->vm_mm, MM_ANONPAGES, nr_pages)  // 统计
    │   ├─ folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE)  // 反向映射
    │   ├─ folio_add_lru_vma(folio, vma)               // 加入 LRU
    │   │
    │   └─ setpte:
    │       set_ptes(vma->vm_mm, addr, vmf->pte, entry, nr_pages)  // 设置 PTE
    │       update_mmu_cache_range(...)                 // 更新 TLB/MMU 缓存
    │
    └─ 返回 0 (成功)
```

### 3.6 缺页流程图

```
用户态访问 brk 内存 (*p = 42)
        │
        ▼
┌──────────────────────────────────┐
│ CPU 触发 Data Abort              │
│ FAR_EL1 = 访问地址               │
│ ESR_EL1.EC = 0x24 (DABT_LOW)    │
│ ESR_EL1.FSC = 0x05 (translation)│
└──────────────┬───────────────────┘
               ▼
┌──────────────────────────────────┐
│ el0t_64_sync_handler()           │ ─── el0_da()
│ do_mem_abort(far, esr, regs)    │ ─── do_translation_fault()
└──────────────┬───────────────────┘                   
               ▼                                       
┌──────────────────────────────────┐
│ do_page_fault(far, esr, regs)    │
│ 确定: user_mode, 写操作           │
└──────────────┬───────────────────┘
               ▼
┌──────────────────────────────────┐
│ lock_vma_under_rcu /             │
│ lock_mm_and_find_vma             │
│  → 找到堆区域的 VMA              │
└──────────────┬───────────────────┘
               ▼
┌──────────────────────────────────┐
│ handle_mm_fault(vma, addr, ...)  │
│  → __handle_mm_fault()           │
│    → handle_pte_fault()          │
│      → do_pte_missing()          │
│        → do_anonymous_page()     │
│          分配物理页并设置 PTE     │
└──────────────┬───────────────────┘
               ▼
┌──────────────────────────────────┐
│ set_ptes() 写入页表               │
│  ↓                              │
│ CPU 重试指令, 翻译地址成功        │
│ 访问正常完成                     │
└──────────────────────────────────┘
```

## 4. 关键数据结构

### 4.1 `struct mm_struct` 中与 brk 相关的字段

```c
struct mm_struct {
    unsigned long start_brk;    /* 堆的起始地址（通常与 end_data 相同） */
    unsigned long brk;          /* 当前堆的结束地址 */
    unsigned long start_data;   /* 数据段起始地址 */
    unsigned long end_data;     /* 数据段结束地址 */
    unsigned long start_stack;  /* 栈起始地址 */
    unsigned long map_count;    /* 当前 VMA 数量 */
    unsigned long total_vm;     /* 总虚拟页面数 */
    unsigned long data_vm;      /* 数据段页面数 (含堆) */
    unsigned long locked_vm;    /* 锁定页面数 */
};
```

### 4.2 brk VMA 标志（ARM64 实际配置）

```
VM_DATA_FLAGS_TSK_EXEC = VM_READ | VM_WRITE | TASK_EXEC |
                          VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC

ARM64 实际 VMA 标志:
  vm_flags = VM_READ | VM_WRITE | VM_EXEC |              // ARM64 下 TASK_EXEC = VM_EXEC
             VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC |
             VM_ACCOUNT | VM_MTE_ALLOWED |
             mm->def_flags (如 VM_LOCKED 等)
```

### 4.3 堆与 VMA 关系

```
+----------------+ 0xFFFF_FFFF_FFFF_FFFF  (内核空间 TOP)
|   kernel VAS   |
+----------------+ TASK_SIZE (2^48 - 1 for ARM64 48-bit VA)
|      ...       |
|   stack (↓)    |
|   mmap 区域 (↑) |
|      ...       |
+----------------+
|     brk  →     |  ← do_brk_flags() 在此扩展堆
|   heap         |     VMA: 匿名映射, 读/写/执行
+----------------+
|   end_data     |
|   data         |
+----------------+
|   text         |
+----------------+ 0x0000_0000_0000_0000
```

### 4.4 ARM64 页表层级（4KB 页面, 48-bit VA）

```
地址翻译: VA[47:39] → PGD (PGD_LEVEL 0)
          VA[38:30] → P4D (与 PGD 折叠) / PUD
          VA[29:21] → PMD
          VA[20:12] → PTE
          VA[11:0]  → 页内偏移

页表创建顺序 (从顶向下):
  pgd → p4d (折叠) → pud → pmd → pte

缺页时页表分配路径 (由 __handle_mm_fault 执行):
  p4d_alloc → pud_alloc → pmd_alloc → pte_alloc
```

## 5. 错误处理

| 错误条件 | 处理方式 |
|---------|---------|
| 获取 mmap 写锁被中断 | 返回 `-EINTR` |
| brk < min_brk（start_brk） | 返回原 brk 值 |
| 超过 RLIMIT_DATA 限制 | 返回原 brk 值 |
| 缩小堆时与现有非 brk VMA 冲突 | 返回原 brk 值 |
| 扩大堆时超过限制 | 返回原 brk 值 |
| 扩大堆时与栈保护间隙冲突 | 返回原 brk 值 |
| do_brk_flags 失败 | 返回原 brk 值 |
| 缺页时 VM_FAULT_OOM | OOM killer, 重试 |
| 缺页时无对应 VMA | SIGSEGV (SEGV_MAPERR) |
| 缺页时权限不匹配 | SIGSEGV (SEGV_ACCERR) |

## 6. 使用示例

```c
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

int main() {
    void *curr_brk = sbrk(0);  /* 获取当前堆边界 */
    printf("Current brk: %p\n", curr_brk);

    /* 扩展堆 4096 字节 */
    if (brk(curr_brk + 4096) == (void*)-1) {
        perror("brk");
        return 1;
    }

    /* 使用堆内存 — 此时触发缺页中断 */
    int *p = (int *)curr_brk;
    *p = 42;  /* ← 触发 do_anonymous_page, 分配物理页 */
    printf("Allocated at %p, value = %d\n", p, *p);

    /* 收缩堆回原位置 */
    brk(curr_brk);

    return 0;
}
```

## 7. 与相关系统调用的比较

| 特性 | brk | mmap | sbrk |
|------|-----|------|------|
| 功能 | 设置堆边界 | 创建内存映射 | 增量调整 brk |
| 灵活性 | 仅堆区域 | 任意地址 / 文件映射 | 同 brk |
| 线程安全 | 需加锁 | 内核保证 | 需加锁 |
| 大内存分配 | 不适合 | 适合 | 不适合 |
| 释放方式 | brk 回退 | munmap | brk 回退 |
| VMA 操作 | merge/expand | 新建 VMA | 同 brk |
| 物理页分配 | 缺页时按需分配 | 缺页时按需分配 | 缺页时按需分配 |

## 8. 关键实现细节

1. **成功/失败返回值特殊设计**：`brk` 在失败时返回**原来的 brk 值**而非负数错误码，这是 Unix 传统设计。调用者通过比较返回值与请求值是否相等来判断成功与否。

2. **CONFIG_COMPAT_BRK**：兼容旧版 brk 行为。本项目**未启用** (`# CONFIG_COMPAT_BRK is not set`), `min_brk = mm->start_brk`。

3. **堆收缩实现**：收缩堆时调用 `do_vmi_align_munmap()` 释放 VMAs 和页表，且会在成功时释放锁（`unlock = true`），标记为 `success_unlocked` 路径。

4. **VM_LOCKED 处理**：如果 `mm->def_flags` 包含 `VM_LOCKED`（通过 `mlockall(MCL_FUTURE)` 设置），扩展的堆区域会自动调用 `mm_populate()` 预填充并锁定物理页。

5. **栈保护间隙**：扩展堆时会检查是否与下一个 VMA 的栈保护间隙（`stack_guard_gap`）冲突，防止堆向栈方向增长过近。

6. **缺页按需分配**：`brk` 系统调用**仅修改 VMA 边界**，不分配物理页。物理内存通过后续缺页处理中的 `do_anonymous_page()` 按需分配。

7. **VMA 合并优化**：`do_brk_flags()` 尝试通过 `vma_merge_new_range()` 扩展已有的堆 VMA（`vmg.just_expand = true`），避免创建不必要的新 VMA 结构。

8. **页表延迟分配**：`__handle_mm_fault()` 在缺页路径中按需分配各级页表（p4d→pud→pmd→pte），其中 p4d 在 ARM64 4KB 页 + 48-bit VA 配置下与 pgd 折叠。

## 9. 文件位置参考

| 文件 | 函数/符号 |
|------|----------|
| `mm/mmap.c` | `SYSCALL_DEFINE1(brk)` — brk 系统调用主实现 |
| `mm/vma.c` | `do_brk_flags()` — 堆扩展/新建 VMA |
| `mm/memory.c` | `handle_mm_fault()` → `__handle_mm_fault()` → `handle_pte_fault()` → `do_pte_missing()` → `do_anonymous_page()` — 缺页处理链 |
| `arch/arm64/kernel/entry.S` | `vectors` — 异常向量表 |
| `arch/arm64/kernel/entry-common.c` | `el0t_64_sync_handler()` → `el0_svc()` / `el0_da()` — 异常分发 |
| `arch/arm64/kernel/syscall.c` | `do_el0_svc()` → `invoke_syscall()` — 系统调用查表 |
| `arch/arm64/mm/fault.c` | `do_mem_abort()` → `do_page_fault()` — ARM64 缺页处理 |
| `include/linux/mm.h` | `VM_DATA_DEFAULT_FLAGS` 宏定义 |
| `include/linux/mm_types.h` | `struct mm_struct` 定义 |
| `arch/arm64/include/asm/page.h` | ARM64 `VM_DATA_DEFAULT_FLAGS` 重定义 |
| `.config` | `CONFIG_ARM64_4K_PAGES=y`, `# CONFIG_COMPAT_BRK is not set` |