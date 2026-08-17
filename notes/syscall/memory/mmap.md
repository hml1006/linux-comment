# mmap 系统调用分析

## 1. 概述

`mmap` 系统调用用于在进程的虚拟地址空间中创建内存映射。它可以用于文件映射（将文件内容映射到内存）和匿名映射（分配内存）。这是 Linux 中最核心的内存管理系统调用之一。

**内核源码位置：** `mm/mmap.c`, `mm/vma.c`, `mm/memory.c`, `mm/filemap.c`

**原型：**

```c
// mm/mmap.c:612
SYSCALL_DEFINE6(mmap_pgoff, unsigned long, addr, unsigned long, len,
                unsigned long, prot, unsigned long, flags,
                unsigned long, fd, unsigned long, pgoff)
```

各架构包装函数可能不同（如 `mmap`、`mmap2`），但最终都调用 `ksys_mmap_pgoff()`。

| 参数 | 描述 |
|------|------|
| `addr` | 期望的映射起始地址（hint，MAP_FIXED 时为强制） |
| `len` | 映射长度（字节） |
| `prot` | 内存保护标志（PROT_READ/WRITE/EXEC/NONE） |
| `flags` | 映射类型和选项（MAP_SHARED/PRIVATE/FIXED/ANONYMOUS 等） |
| `fd` | 文件描述符（匿名映射时为 -1） |
| `pgoff` | 文件偏移（以页为单位） |

**返回值：**
- 成功返回映射的起始地址
- 失败返回 `MAP_FAILED`（即 `(void*)-1`）

## 2. 使用场景

- **内存分配**：`malloc` 在大内存分配时使用 `mmap(MAP_ANONYMOUS)`
- **文件 I/O**：将文件映射到内存，通过内存访问替代 read/write
- **共享内存**：`MAP_SHARED` 用于进程间共享内存
- **动态库加载**：加载器使用 `mmap` 将共享库映射到进程地址空间
- **内核/驱动接口**：mmap 设备内存用于 DMA 或 MMIO

## 3. 完整调用链总览

### 3.1 两阶段架构

mmap 采用**两阶段架构**：

```
第一阶段：mmap() 系统调用          第二阶段：缺页异常（Page Fault）
─────────────────────────────     ─────────────────────────────
只创建 VMA（虚拟内存区域）          实际分配物理页并建立页表映射
不分配物理页                       按需延迟分配（需求分页）
更新进程地址空间管理结构            真正的物理内存分配点
```

### 3.2 完整调用链

```
mmap(addr, len, prot, flags, fd, offset)              // 架构特定入口
  │
  └─ ksys_mmap_pgoff(addr, len, prot, flags, fd, pgoff) // mm/mmap.c:567
       ├─ 文件映射：fget(fd), 大页检查
       └─ 匿名映射：MAP_HUGETLB 处理
       └─ vm_mmap_pgoff(file, addr, len, prot, flags, pgoff)
            └─ mmap_write_lock_killable(mm)           // 获取写锁
            └─ do_mmap(file, addr, len, prot, flags, vm_flags, pgoff, ...)
                 ├─ 参数验证与 vm_flags 计算
                 ├─ __get_unmapped_area()              // 查找空闲地址空间
                 └─ mmap_region(file, addr, len, vm_flags, pgoff, uf)
                      └─ __mmap_region()               // mm/vma.c:2720
                           ├─ __mmap_setup()           // 清理已有映射、计算内存
                           ├─ vma_merge_new_range()    // 尝试与相邻 VMA 合并
                           └─ __mmap_new_vma()         // 创建新 VMA
                                ├─ vm_area_alloc()     // 分配 VMA 结构
                                ├─ 文件映射:
                                │    └─ __mmap_new_file_vma()
                                │         └─ mmap_file() → file->f_op->mmap()
                                │              └─ ext4_file_mmap() 等
                                ├─ 匿名共享映射:
                                │    └─ shmem_zero_setup()
                                └─ 匿名私有映射:
                                     └─ vma_set_anonymous()
                           └─ __mmap_complete()         // 统计更新
            └─ mmap_write_unlock(mm)
  └─ 如果 VM_LOCKED 或 MAP_POPULATE：
       └─ mm_populate()                                // 触发缺页预填充

第二阶段：缺页异常（Page Fault）
─────────────────────────────────
  用户态访问映射地址 → CPU 触发缺页
    │
    └─ ARM64 缺页处理 (arch/arm64/mm/fault.c)
         └─ do_mem_abort() → do_page_fault()
              └─ handle_mm_fault(vma, addr, flags, regs)  // mm/memory.c:6589
                   └─ __handle_mm_fault(vma, addr, flags)  // mm/memory.c:6355
                        ├─ pgd_offset() → p4d_alloc() → pud_alloc() → pmd_alloc()
                        │  (逐级查找/分配页表)
                        └─ handle_pte_fault(vmf)            // mm/memory.c:6273
                             ├─ PTE 为空 → do_pte_missing()
                             │    ├─ 匿名映射 → do_anonymous_page()  // mm/memory.c:5217
                             │    └─ 文件映射 → do_fault()          // mm/memory.c:5903
                             │         ├─ 读缺页 → do_read_fault()
                             │         │    └─ __do_fault() → vma->vm_ops->fault()
                             │         │         └─ filemap_fault() // mm/filemap.c:3590
                             │         ├─ 写私有(COW) → do_cow_fault()
                             │         │    └─ __do_fault() + 复制页
                             │         └─ 写共享 → do_shared_fault()
                             │              └─ __do_fault() + page_mkwrite
                             ├─ PTE 存在但非 Present → do_swap_page()  // 换入
                             ├─ NUMA 迁移 → do_numa_page()
                             └─ 写保护页 → do_wp_page()  // 写时复制
```

## 4. 第一阶段：mmap() 系统调用

### 4.1 ksys_mmap_pgoff 入口

```c
// mm/mmap.c:567
unsigned long ksys_mmap_pgoff(unsigned long addr, unsigned long len,
                              unsigned long prot, unsigned long flags,
                              unsigned long fd, unsigned long pgoff)
{
    struct file *file = NULL;
    unsigned long retval = -EBADF;

    // 检查 MAP_DROPPABLE 标志
    if (!IS_ENABLED(CONFIG_MMU) && (flags & MAP_DROPPABLE))
        return -EINVAL;

    // 文件映射：获取文件引用
    if (!(flags & MAP_ANONYMOUS)) {
        audit_mmap_fd(fd, flags);
        file = fget(fd);
        if (!file)
            goto out;
        // 检查文件是否支持 mmap
        if (!file->f_op->mmap)
            goto out_fput;

        // 大页文件检查
        if (is_file_hugepages(file))
            len = ALIGN(len, huge_page_size(hstate_file(file)));
    } else if (flags & MAP_HUGETLB) {
        // 匿名大页映射，创建 hugetlb 文件
        struct hstate *hs;

        hs = hstate_sizelog((flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK);
        if (!hs)
            return -EINVAL;

        len = ALIGN(len, huge_page_size(hs));
        file = hugetlb_file_setup(HUGETLB_ANON_FILE, len,
                VM_NORESERVE, &user, HUGETLB_ANONHUGE_INODE,
                (flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK);
        if (IS_ERR(file))
            return PTR_ERR(file);
        retval = -EINVAL;
    }

    // 参数对齐和偏移转换
    flags &= ~MAP_HUGETLB;  // 标志已使用
    if (flags & MAP_ANONYMOUS)
        pgoff = 0;          // 匿名映射偏移固定为 0

    // 调用核心实现
    retval = vm_mmap_pgoff(file, addr, len, prot, flags, pgoff);
out_fput:
    if (file)
        fput(file);
out:
    return retval;
}
```

### 4.2 do_mmap 参数验证与 vm_flags 计算

```c
// mm/mmap.c:335
unsigned long do_mmap(struct file *file, unsigned long addr,
                      unsigned long len, unsigned long prot,
                      unsigned long flags, vm_flags_t vm_flags,
                      unsigned long pgoff, unsigned long *populate,
                      struct list_head *uf)
{
    struct mm_struct *mm = current->mm;

    *populate = 0;

    if (!len) return -EINVAL;

    // READ_IMPLIES_EXEC 处理
    if ((prot & PROT_READ) && (current->personality & READ_IMPLIES_EXEC))
        if (!(file && path_noexec(&file->f_path)))
            prot |= PROT_EXEC;

    // MAP_FIXED_NOREPLACE 需要 MAP_FIXED
    if (flags & MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED;

    // 非 MAP_FIXED 时，地址对齐到最小粒度
    if (!(flags & MAP_FIXED))
        addr = round_hint_to_min(addr);

    // 页对齐和溢出检查
    len = PAGE_ALIGN(len);
    if (!len) return -ENOMEM;
    if ((pgoff + (len >> PAGE_SHIFT)) < pgoff)
        return -EOVERFLOW;

    // VMA 数量限制
    if (mm->map_count > sysctl_max_map_count)
        return -ENOMEM;

    // 计算 vm_flags
    vm_flags |= calc_vm_prot_bits(prot, pkey) |
                calc_vm_flag_bits(file, flags) |
                mm->def_flags | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;

    // 查找空闲地址空间
    addr = __get_unmapped_area(file, addr, len, pgoff, flags, vm_flags);
    if (IS_ERR_VALUE(addr))
        return addr;

    // MAP_FIXED_NOREPLACE 检查地址是否已被占用
    if (flags & MAP_FIXED_NOREPLACE) {
        if (find_vma_intersection(mm, addr, addr + len))
            return -EEXIST;
    }

    // 文件映射权限检查
    if (file) {
        // ... 检查文件读写权限、MAP_SHARED/MAP_PRIVATE 合法性
        // ... 检查 memfd seals
    } else {
        // 匿名映射标志处理
        switch (flags & MAP_TYPE) {
        case MAP_SHARED:
            vm_flags |= VM_SHARED | VM_MAYSHARE;
            break;
        case MAP_PRIVATE:
            pgoff = addr >> PAGE_SHIFT;  // 匿名映射 pgoff 基于地址
            break;
        case MAP_DROPPABLE:
            vm_flags |= VM_DROPPABLE | VM_NORESERVE | VM_WIPEONFORK | VM_DONTDUMP;
            break;
        }
    }

    // 调用核心 mmap_region
    addr = mmap_region(file, addr, len, vm_flags, pgoff, uf);
    if (!IS_ERR_VALUE(addr) &&
        ((vm_flags & VM_LOCKED) ||
         (flags & (MAP_POPULATE | MAP_NONBLOCK)) == MAP_POPULATE))
        *populate = len;  // 需要预填充页表

    return addr;
}
```

### 4.3 __get_unmapped_area：地址空间查找

```c
// mm/mmap.c:2260
unsigned long __get_unmapped_area(struct file *file, unsigned long addr,
                                  unsigned long len, unsigned long pgoff,
                                  unsigned long flags, vm_flags_t vm_flags)
{
    unsigned long (*get_area)(struct file *, unsigned long,
                              unsigned long, unsigned long, unsigned long);

    // 地址范围合法性检查
    if (flags & MAP_FIXED) {
        if (addr > addr + len)  // 溢出
            return -EINVAL;
        if (addr < mmap_min_addr)  // 低于最小地址
            return -EINVAL;
        return addr;  // MAP_FIXED 直接返回请求地址
    }

    // 优先使用文件系统的 get_unmapped_area
    if (file && file->f_op->get_unmapped_area)
        get_area = file->f_op->get_unmapped_area;
    else
        get_area = current->mm->get_unmapped_area;  // 架构默认

    return get_area(file, addr, len, pgoff, flags);
}
```

**地址查找算法：**

| 算法 | 方向 | 适用场景 | 实现 |
|------|------|----------|------|
| `unmapped_area()` | 自底向上 | MAP_FIXED, 栈 | `mm/vma.c:2947` |
| `unmapped_area_topdown()` | 自顶向下 | 默认（mmap 分配） | `mm/vma.c:3004` |

**查找过程：**
1. 遍历 VMA 红黑树找到空闲区间
2. 考虑对齐要求（`align_mask`）和间隙（`start_gap`）
3. 检查与相邻 VMA 的间隙是否足够
4. ARM64 默认使用 topdown 算法

### 4.4 mmap_region / __mmap_region：核心 VMA 创建

#### 4.4.1 __mmap_setup：清理已有映射

```c
// mm/vma.c:2392
static int __mmap_setup(struct mmap_state *map, struct vm_area_desc *desc,
                        struct list_head *uf)
{
    // 1. 查找重叠的 VMA
    vms->vma = vma_find(vmi, map->end);
    init_vma_munmap(vms, vmi, vms->vma, map->addr, map->end, uf, false);

    // 2. 如果有重叠区域，准备解除映射
    if (vms->vma) {
        error = vms_gather_munmap_vmas(vms, &map->mas_detach);
        // 收集需要解除映射的 VMA 信息
    }

    // 3. 检查地址空间限制
    if (!may_expand_vm(map->mm, map->vm_flags, map->pglen - vms->nr_pages))
        return -ENOMEM;

    // 4. 可写的私有映射：检查内存可用性（overcommit 检查）
    if (accountable_mapping(map->file, map->vm_flags)) {
        error = security_vm_enough_memory_mm(map->mm, map->charged);
        if (error) return -ENOMEM;
        map->vm_flags |= VM_ACCOUNT;
    }

    // 5. 清除原有 PTE（解除旧映射的页表）
    vms_clean_up_area(vms, &map->mas_detach);

    return 0;
}
```

#### 4.4.2 __mmap_new_vma：创建新 VMA

```c
// mm/vma.c:2506
static int __mmap_new_vma(struct mmap_state *map, struct vm_area_struct **vmap)
{
    int error = 0;
    struct vm_area_struct *vma;

    // 1. 分配 VMA 结构体
    vma = vm_area_alloc(map->mm);
    if (!vma) return -ENOMEM;

    // 2. 设置 VMA 范围、标志和页保护
    vma_set_range(vma, map->addr, map->end, map->pgoff);
    vm_flags_init(vma, map->vm_flags);
    vma->vm_page_prot = map->page_prot;

    // 3. 根据映射类型设置 VMA 操作
    if (map->file)
        // ★ 文件映射：调用文件系统的 mmap 回调
        error = __mmap_new_file_vma(map, vma);
    else if (map->vm_flags & VM_SHARED)
        // ★ 匿名共享映射：shmem 零页
        error = shmem_zero_setup(vma);
    else
        // ★ 匿名私有映射：标记为匿名
        vma_set_anonymous(vma);

    // 4. 将 VMA 插入地址空间
    vma_start_write(vma);
    vma_iter_store_new(vmi, vma);
    map->mm->map_count++;
    vma_link_file(vma, map->hold_file_rmap_lock);

    *vmap = vma;
    return 0;
}
```

#### 4.4.3 __mmap_new_file_vma：文件映射回调

```c
// mm/vma.c:2455
static int __mmap_new_file_vma(struct mmap_state *map,
                               struct vm_area_struct *vma)
{
    vma->vm_file = map->file;
    get_file(map->file);

    // 调用文件系统的 mmap 回调
    error = mmap_file(vma->vm_file, vma);
    // mmap_file() 最终调用 file->f_op->mmap()
    // 例如 ext4: ext4_file_mmap(vma)
    //  → vma->vm_ops = ext4_file_vm_ops

    return error;
}
```

**文件系统 mmap 回调的作用：**

| 文件系统 | mmap 回调 | 设置 vm_ops |
|----------|-----------|-------------|
| ext4 | `ext4_file_mmap()` | `ext4_file_vm_ops` |
| ext2 | `ext2_file_mmap()` | `ext2_file_vm_ops` |
| btrfs | `btrfs_file_mmap()` | `btrfs_file_vm_ops` |
| xfs | `xfs_file_mmap()` | `xfs_file_vm_ops` |
| shmem | `shmem_mmap()` | `shmem_vm_ops` |

设置的 `vm_ops` 中包含 `fault` 回调（用于缺页处理）和 `map_pages` 回调（用于预读）：

```c
// 例：ext4_file_vm_ops
static const struct vm_operations_struct ext4_file_vm_ops = {
    .fault       = ext4_filemap_fault,  // 缺页处理
    .map_pages   = filemap_map_pages,   // 批量映射页
    .page_mkwrite = ext4_page_mkwrite,  // 页写保护处理
};
```

#### 4.4.4 __mmap_complete：完成映射

```c
// mm/vma.c:2580
static void __mmap_complete(struct mmap_state *map, struct vm_area_struct *vma)
{
    // 1. 解除旧映射（如果有）
    if (vms->vmas) {
        remove_vma_structure(vms, &map->mas_detach);
        unmap_region(vms);  // 刷新 TLB
    }

    // 2. 更新内存统计
    vm_stat_account(mm, vm_flags, len >> PAGE_SHIFT);

    // 3. 更新各种 vm 计数
    mm->total_vm += len >> PAGE_SHIFT;
    if (vm_flags & VM_SHARED)
        mm->shared_vm += len >> PAGE_SHIFT;
    else if (vm_flags & VM_STACK_FLAGS)
        mm->stack_vm += len >> PAGE_SHIFT;
    else
        mm->data_vm += len >> PAGE_SHIFT;
    if (vm_flags & VM_LOCKED)
        mm->locked_vm += len >> PAGE_SHIFT;
}
```

### 4.5 文件映射 vs 匿名映射创建对比

| 特性 | 文件映射 | 匿名映射 |
|------|----------|----------|
| `file` 参数 | 非 NULL | NULL |
| `vm_file` | 指向映射的文件 | NULL |
| `vm_ops` | 文件系统设置（如 `ext4_file_vm_ops`） | 匿名映射为 NULL |
| `pgoff` | 用户指定的文件偏移 | `addr >> PAGE_SHIFT`（私有）或 0（共享） |
| 缺页回调 | `filemap_fault()` | 内核直接处理（`do_anonymous_page()`） |
| 物理页来源 | 页缓存（page cache） | 零页或新分配的匿名页 |
| 写回 | 脏页通过文件系统写回磁盘 | 换出到交换分区 |

### 4.6 关键数据结构

#### struct vm_area_struct (VMA)

```c
struct vm_area_struct {
    unsigned long vm_start;          /* VMA 起始地址 */
    unsigned long vm_end;            /* VMA 结束地址 */
    struct vm_area_struct *vm_next;  /* 链表中的下一个 VMA */
    struct rb_node vm_rb;            /* 红黑树节点 */
    struct mm_struct *vm_mm;         /* 所属的 mm_struct */
    pgprot_t vm_page_prot;           /* 页表访问权限 */
    unsigned long vm_flags;          /* VMA 标志位 */
    struct file *vm_file;            /* 映射的文件（文件映射时） */
    unsigned long vm_pgoff;          /* 文件偏移（页为单位） */
    const struct vm_operations_struct *vm_ops;  /* VMA 操作回调 */
    struct anon_vma *anon_vma;       /* 匿名映射反向映射 */
};
```

#### VMA 标志位

| 标志 | 含义 | 典型场景 |
|------|------|----------|
| `VM_READ` | 可读 | 所有映射 |
| `VM_WRITE` | 可写 | 数据段、堆 |
| `VM_EXEC` | 可执行 | 代码段 |
| `VM_SHARED` | 可共享（多个进程） | MAP_SHARED |
| `VM_MAYREAD/WRITE/EXEC` | 权限可变更 | mprotect 时使用 |
| `VM_GROWSDOWN` | 可向下扩展 | 栈 |
| `VM_LOCKED` | 锁定在内存中 | mlock |
| `VM_IO` | 内存映射 I/O | 设备映射 |
| `VM_DROPPABLE` | 内容可丢弃 | 缓存映射 |
| `VM_PFNMAP` | 直接 PFN 映射 | 设备驱动 |
| `VM_ANON` | 匿名映射 | brk, mmap ANONYMOUS |
| `VM_ACCOUNT` | 已记账内存 | overcommit 跟踪 |

#### mm_struct 地址空间管理

```c
struct mm_struct {
    struct vm_area_struct *mmap;          /* VMA 链表 */
    struct rb_root mm_rb;                 /* VMA 红黑树根 */
    unsigned long total_vm;               /* 总映射页数 */
    unsigned long locked_vm;              /* 锁定页数 */
    unsigned long data_vm;                /* 数据页数 */
    unsigned long stack_vm;               /* 栈页数 */
    unsigned long (*get_unmapped_area)();  /* 地址查找函数 */
    pgd_t *pgd;                           /* 页全局目录 */
};
```

## 5. 第二阶段：缺页异常（Page Fault）

### 5.1 ARM64 缺页处理入口

```c
// arch/arm64/mm/fault.c:570
static int __kprobes do_page_fault(unsigned long far, unsigned long esr,
                                   struct pt_regs *regs)
{
    unsigned long vm_flags;
    unsigned long addr = untagged_addr(far);
    unsigned int mm_flags = FAULT_FLAG_DEFAULT;
    struct vm_area_struct *vma;
    vm_fault_t fault;
    unsigned int si_code;

    if (user_mode(regs))
        mm_flags |= FAULT_FLAG_USER;

    // 解析 ESR 寄存器确定缺页类型
    if (is_el0_instruction_abort(esr)) {
        vm_flags = VM_EXEC;
        mm_flags |= FAULT_FLAG_INSTRUCTION;
    } else if (is_write_abort(esr)) {
        vm_flags = VM_WRITE;
        mm_flags |= FAULT_FLAG_WRITE;  // 写缺页
    } else {
        vm_flags = VM_READ;            // 读缺页
    }

    // 快速路径：VMA 锁（RCU 读锁，避免 mmap_lock 竞争）
    vma = lock_vma_under_rcu(mm, addr);
    if (vma) {
        // VMA 权限检查
        if (!(vma->vm_flags & vm_flags))
            goto bad_area;  // 权限错误

        fault = handle_mm_fault(vma, addr, mm_flags | FAULT_FLAG_VMA_LOCK, regs);
        if (!(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
            vma_end_read(vma);
        if (!(fault & VM_FAULT_RETRY))
            goto done;
        // 需要 mmap_lock 重试
    }

    // 慢速路径：获取 mmap_lock
lock_mmap:
    vma = lock_mm_and_find_vma(mm, addr, regs);
    if (!vma) {
        si_code = SEGV_MAPERR;  // 地址不在任何 VMA 中
        goto bad_area;
    }

    // VMA 权限检查
    if (!(vma->vm_flags & vm_flags)) {
        si_code = SEGV_ACCERR;  // 权限不匹配
        goto bad_area;
    }

    // 核心缺页处理
    fault = handle_mm_fault(vma, addr, mm_flags, regs);

    if (fault & VM_FAULT_RETRY) {
        mm_flags |= FAULT_FLAG_TRIED;
        goto retry;
    }
    mmap_read_unlock(mm);

done:
    // 处理缺页结果
    if (fault & VM_FAULT_MAJOR)
        // 主缺页（需要磁盘 I/O）
        perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS_MAJ, 1, regs, addr);
    else
        // 次缺页（无需磁盘 I/O）
        perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS_MIN, 1, regs, addr);
    return 0;

bad_area:
    // 发送 SIGSEGV 信号
    arm64_force_sig_fault(SIGSEGV, si_code, addr, ...);
}
```

### 5.2 handle_mm_fault → __handle_mm_fault：页表遍历

```c
// mm/memory.c:6589
vm_fault_t handle_mm_fault(struct vm_area_struct *vma, unsigned long address,
                           unsigned int flags, struct pt_regs *regs)
{
    vm_fault_t ret;

    // 内核虚拟地址不能缺页
    __set_current_state(TASK_RUNNING);

    // 架构特定初始化
    if (arch_vma_access_permitted(vma, flags & FAULT_FLAG_WRITE,
                                  flags & FAULT_FLAG_INSTRUCTION,
                                  flags & FAULT_FLAG_REMOTE))
        return VM_FAULT_SIGSEGV;

    // 调用 __handle_mm_fault
    ret = __handle_mm_fault(vma, address, flags);

    // 统计
    if (ret & VM_FAULT_MAJOR)
        current->maj_flt++;
    else
        current->min_flt++;

    return ret;
}
```

**__handle_mm_fault：页表层级遍历**

```c
// mm/memory.c:6355
static vm_fault_t __handle_mm_fault(struct vm_area_struct *vma,
        unsigned long address, unsigned int flags)
{
    struct vm_fault vmf = {
        .vma = vma,
        .address = address & PAGE_MASK,
        .real_address = address,
        .flags = flags,
        .pgoff = linear_page_index(vma, address),  // 文件偏移
        .gfp_mask = __get_fault_gfp_mask(vma),
    };
    unsigned long vm_flags = vma->vm_flags;
    pgd_t *pgd;
    p4d_t *p4d;
    vm_fault_t ret;

    // 第1级：PGD（页全局目录）
    pgd = pgd_offset(mm, address);
    p4d = p4d_alloc(mm, pgd, address);
    if (!p4d) return VM_FAULT_OOM;

    // 第2级：P4D
    vmf.pud = pud_alloc(mm, p4d, address);
    if (!vmf.pud) return VM_FAULT_OOM;

    // 第3级：PUD
    if (pud_none(*vmf.pud) && transparent_hugepage_enabled(vma)) {
        // 尝试 THP 大页映射
        ret = create_huge_pmd(&vmf);
        if (!(ret & VM_FAULT_FALLBACK))
            return ret;
    }
    vmf.pmd = pmd_alloc(mm, vmf.pud, address);
    if (!vmf.pmd) return VM_FAULT_OOM;

    // 第4级：PMD（检查 PMD 级大页）
    if (pmd_none(*vmf.pmd) && transparent_hugepage_enabled(vma)) {
        ret = create_huge_pmd(&vmf);
        if (!(ret & VM_FAULT_FALLBACK))
            return ret;
    } else {
        pmd_t orig_pmd = *vmf.pmd;
        barrier();
        if (unlikely(pmd_trans_huge(orig_pmd) || pmd_devmap(orig_pmd) ||
                     uffd_wp(orig_pmd))) {
            // 处理透明大页的缺页
            return do_huge_pmd_numa_page(&vmf);
        }
    }

    // 第5级：PTE（页表项）
    // 进入 handle_pte_fault 处理 PTE 级缺页
    return handle_pte_fault(&vmf);
}
```

### 5.3 handle_pte_fault：PTE 级缺页路由

```c
// mm/memory.c:6273
static vm_fault_t handle_pte_fault(struct vm_fault *vmf)
{
    pte_t entry;

    // 获取 PTE 指针
    if (unlikely(pmd_none(*vmf->pmd))) {
        // PMD 不存在，延迟 PTE 页表分配
        vmf->pte = NULL;
        vmf->flags &= ~FAULT_FLAG_ORIG_PTE_VALID;
    } else {
        vmf->pte = pte_offset_map_rw_nolock(vmf->vma->vm_mm, vmf->pmd,
                                            vmf->address, &dummy_pmdval,
                                            &vmf->ptl);
        vmf->orig_pte = ptep_get_lockless(vmf->pte);
        vmf->flags |= FAULT_FLAG_ORIG_PTE_VALID;

        if (pte_none(vmf->orig_pte)) {
            pte_unmap(vmf->pte);
            vmf->pte = NULL;
        }
    }

    // ★ 缺页路由：根据 PTE 状态分派
    if (!vmf->pte)
        return do_pte_missing(vmf);   // PTE 为空 → 缺页

    if (!pte_present(vmf->orig_pte))
        return do_swap_page(vmf);     // PTE 存在但非 Present → 换入

    if (pte_protnone(vmf->orig_pte) && vma_is_accessible(vmf->vma))
        return do_numa_page(vmf);     // NUMA 迁移

    // PTE 存在且 Present → 检查写权限
    spin_lock(vmf->ptl);
    entry = vmf->orig_pte;
    if (unlikely(!pte_same(ptep_get(vmf->pte), entry))) {
        update_mmu_tlb(vmf->vma, vmf->address, vmf->pte);
        goto unlock;
    }
    if (vmf->flags & (FAULT_FLAG_WRITE|FAULT_FLAG_UNSHARE)) {
        if (!pte_write(entry))
            return do_wp_page(vmf);  // 写保护页 → 写时复制
        else if (likely(vmf->flags & FAULT_FLAG_WRITE))
            entry = pte_mkdirty(entry);
    }
    // 标记访问位
    entry = pte_mkyoung(entry);
    if (ptep_set_access_flags(vmf->vma, vmf->address, vmf->pte, entry,
                              vmf->flags & FAULT_FLAG_WRITE))
        update_mmu_cache_range(vmf, vmf->vma, vmf->address, vmf->pte, 1);
}
```

**缺页路由图：**

```
handle_pte_fault(vmf)
    │
    ├── PTE 为 NULL（PTE 不存在）
    │    └── do_pte_missing(vmf)
    │         ├── 匿名映射 → do_anonymous_page()
    │         └── 文件映射 → do_fault()
    │              ├── 读缺页 → do_read_fault()
    │              │    └── __do_fault() → filemap_fault()
    │              ├── 写私有 → do_cow_fault()
    │              │    └── __do_fault() + 复制页
    │              └── 写共享 → do_shared_fault()
    │                   └── __do_fault() + page_mkwrite
    │
    ├── PTE 存在但非 Present
    │    └── do_swap_page()  (从交换分区/文件换入)
    │
    ├── PTE 存在但 protnone
    │    └── do_numa_page()  (NUMA 迁移页)
    │
    └── PTE Present 但写保护
         └── do_wp_page()  (写时复制)
```

## 6. 匿名映射缺页（do_anonymous_page）

### 6.1 流程分析

```c
// mm/memory.c:5217
static vm_fault_t do_anonymous_page(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct folio *folio;
    pte_t entry;
    int nr_pages = 1;

    // 共享匿名映射不允许（应用 should 使用 shmem）
    if (vma->vm_flags & VM_SHARED)
        return VM_FAULT_SIGBUS;

    // 分配 PTE 页表（如果 PMD 还不存在）
    if (pte_alloc(vma->vm_mm, vmf->pmd))
        return VM_FAULT_OOM;

    // ★ 读缺页且不禁用零页 → 使用全局零页（零成本）
    if (!(vmf->flags & FAULT_FLAG_WRITE) &&
            !mm_forbids_zeropage(vma->vm_mm)) {
        entry = pte_mkspecial(pfn_pte(my_zero_pfn(vmf->address),
                                      vma->vm_page_prot));
        // 映射到全局零页（只读）
        vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd,
                                       vmf->address, &vmf->ptl);
        if (vmf_pte_changed(vmf)) goto unlock;
        goto setpte;
    }

    // ★ 写缺页：分配实际的匿名物理页
    ret = vmf_anon_prepare(vmf);  // 准备反向映射结构
    if (ret) return ret;

    folio = alloc_anon_folio(vmf);  // 分配物理页
    if (IS_ERR(folio)) return 0;
    if (!folio) goto oom;

    __folio_mark_uptodate(folio);  // 标记页为有效

    // 创建页表项
    entry = folio_mk_pte(folio, vma->vm_page_prot);
    entry = pte_sw_mkyoung(entry);
    if (vma->vm_flags & VM_WRITE)
        entry = pte_mkwrite(pte_mkdirty(entry), vma);

    // 建立映射
    vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd, addr, &vmf->ptl);
    setpte:
    set_ptes(vma->vm_mm, addr, vmf->pte, entry, nr_pages);
    // ★ 插入反向映射
    folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE);
    folio_add_lru_vma(folio, vma);  // 加入 LRU 链表
    update_mmu_cache_range(vmf, vma, addr, vmf->pte, nr_pages);
}
```

### 6.2 匿名映射缺页流程图

```
用户态访问匿名映射地址
    │
    ├── 读缺页（首次读取）
    │    │
    │    └── 映射到全局零页（my_zero_pfn）
    │         ├── 所有进程共享同一个物理零页
    │         ├── 不分配物理内存
    │         ├── PTE 标记为 pte_special
    │         └── 零页在试图写入时触发 COW
    │
    └── 写缺页（首次写入）
         │
         ├── vmf_anon_prepare() 准备反向映射
         │    └── 分配 anon_vma 结构
         │
         ├── alloc_anon_folio() 分配物理页
         │    ├── 从伙伴系统分配 (GFP_HIGHUSER_MOVABLE)
         │    ├── 可能分配大页（mTHP, 如 16K/32K/64K）
         │    └── 页内容清零
         │
         ├── 建立 PTE 映射
         │    ├── set_ptes() 写入页表
         │    └── pte_mkwrite(pte_mkdirty(entry)) 标记可写/脏
         │
         ├── folio_add_new_anon_rmap() 插入反向映射
         │    └── 用于页面回收时找到映射的 VMA
         │
         └── folio_add_lru_vma() 加入 LRU 链表
              └── 用于页面回收和换出
```

## 7. 文件映射缺页（filemap_fault）

### 7.1 流程分析

```c
// mm/filemap.c:3590
vm_fault_t filemap_fault(struct vm_fault *vmf)
{
    struct file *file = vmf->vma->vm_file;
    struct address_space *mapping = file->f_mapping;
    struct inode *inode = mapping->host;
    pgoff_t index = vmf->pgoff;
    struct folio *folio;
    vm_fault_t ret = 0;

    // 检查文件大小边界
    max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
    if (unlikely(index >= max_idx))
        return VM_FAULT_SIGBUS;  // 超出文件尾

    trace_mm_filemap_fault(mapping, index);

    // ★ 1. 尝试在页缓存中查找
    folio = filemap_get_folio(mapping, index);
    if (likely(!IS_ERR(folio))) {
        // 页缓存命中
        // 发起异步预读
        if (!(vmf->flags & FAULT_FLAG_TRIED))
            fpin = do_async_mmap_readahead(vmf, folio);
        if (unlikely(!folio_test_uptodate(folio))) {
            // 页存在但内容未就绪
            filemap_invalidate_lock_shared(mapping);
            mapping_locked = true;
        }
    } else {
        // ★ 2. 页缓存未命中 → 需要从磁盘读取
        count_vm_event(PGMAJFAULT);  // 主缺页统计
        ret = VM_FAULT_MAJOR;

        // 发起同步预读
        fpin = do_sync_mmap_readahead(vmf);

        // 创建页缓存页
        folio = __filemap_get_folio(mapping, index,
                                    FGP_CREAT|FGP_FOR_MMAP,
                                    vmf->gfp_mask);
        if (IS_ERR(folio))
            return VM_FAULT_OOM;
    }

    // 3. 锁定页（可能等待 I/O 完成）
    if (!lock_folio_maybe_drop_mmap(vmf, folio, &fpin))
        goto out_retry;

    // 4. 检查页是否被截断
    if (unlikely(folio->mapping != mapping))
        goto retry_find;

    // 5. 检查页内容是否就绪
    if (unlikely(!folio_test_uptodate(folio))) {
        // ★ 从磁盘读取页内容
        fpin = maybe_unlock_mmap_for_io(vmf, fpin);
        error = filemap_read_folio(file, mapping->a_ops->read_folio, folio);
        // read_folio → 块设备 I/O
        if (error) return VM_FAULT_SIGBUS;
    }

    // 6. 返回已加锁的页
    vmf->page = folio_file_page(folio, index);
    return ret | VM_FAULT_LOCKED;
}
```

### 7.2 文件映射缺页流程图

```
用户态访问文件映射地址
    │
    ▼
filemap_fault(vmf)
    │
    ├── 检查文件大小边界
    │    └── index >= max_idx → SIGBUS
    │
    ├── 查找页缓存（page cache）
    │    │
    │    ├── 页缓存命中
    │    │    │
    │    │    ├── 异步预读（do_async_mmap_readahead）
    │    │    │    └── 提前读取相邻页，提高顺序访问性能
    │    │    │
    │    │    └── 页内容就绪（uptodate）？
    │    │         ├── 是 → 锁定页，返回
    │    │         └── 否 → 等待 I/O 完成
    │    │
    │    └── 页缓存未命中（★ 主缺页）
    │         │
    │         ├── 同步预读（do_sync_mmap_readahead）
    │         │    └── 读取本页和相邻页到页缓存
    │         │
    │         ├── __filemap_get_folio(FGP_CREAT)
    │         │    └── 在页缓存中分配新页
    │         │         └── 如果页缓存已满 → 回收旧页
    │         │
    │         └── 回到页缓存查找（重试）
    │
    ├── 锁定页
    │    └── lock_folio_maybe_drop_mmap()
    │         └── 如果页正在 I/O，可能暂时释放 mmap_lock
    │
    ├── 检查页内容
    │    │
    │    └── 页未就绪（!uptodate）
    │         │
    │         └── filemap_read_folio()
    │              │
    │              └── mapping->a_ops->read_folio(file, folio)
    │                   │
    │                   ├── ext4_read_folio() / btrfs_read_folio() 等
    │                   │
    │                   └── 提交块 I/O 请求 (submit_bio)
    │                        │
    │                        ├── 构造 bio 结构
    │                        ├── 通过块层提交到块设备驱动
    │                        │    ├── IO 调度器
    │                        │    └── 块设备驱动程序
    │                        │         └── 硬件（NVMe/SATA/SD等）
    │                        │
    │                        └── I/O 完成后唤醒等待进程
    │
    └── finish_fault() 建立 PTE 映射
         │
         ├── 尝试 PMD 级大页映射（do_set_pmd）
         │    └── 适用大页且已对齐 → 2MB 映射
         │
         ├── 安装 PTE 页表（pmd_install / pte_alloc）
         │
         └── set_ptes() 写入页表项
              ├── 标记访问位
              └── 更新 TLB
```

### 7.3 页缓存与块设备 I/O 路径

#### 7.3.1 总体 I/O 路径

```
filemap_fault()
  └── filemap_read_folio()
       └── mapping->a_ops->read_folio(file, folio)
            │
            └── ext4_read_folio()  // 以 ext4 为例
                 │
                 ├── [文件系统层] 逻辑块号 → 物理块号
                 │    └── ext4_mpage_readpages()
                 │         └── ext4_map_blocks()    // 查询 extent 树
                 │              └── ext4_es_lookup_extent()  // 内存缓存
                 │                   └── ext4_ext_map_blocks()  // 磁盘 extent 树
                 │
                 ├── [块层] 构造并提交 bio
                 │    ├── bio_alloc() + bio_set_dev()
                 │    ├── bio_add_page()
                 │    └── submit_bio(bio)
                 │         └── blk_mq_submit_bio()  // 多队列块层
                 │              ├── 分配 request
                 │              ├── bio → request 绑定
                 │              ├── I/O 调度器 (mq-deadline / none)
                 │              └── q->mq_ops->queue_rq()
                 │                   └── nvme_queue_rq()  // NVMe 驱动
                 │
                 └── I/O 完成
                      └── nvme_irq() → bio_endio()
                           └── folio_end_read()
                                └── 解锁页，唤醒等待进程
```

#### 7.3.2 ext4 文件系统块映射（ext4_map_blocks）

ext4 使用 **extent 树**（一种 B 树）管理文件逻辑块到物理块号的映射。

**extent 数据结构**（`fs/ext4/ext4_extents.h`）:

```c
// 叶子节点：记录一段连续物理空间的映射
struct ext4_extent {
    __le32  ee_block;       // 覆盖的第一个逻辑块号
    __le16  ee_len;         // 覆盖的块数（bit15=1 表示未写入/预分配）
    __le16  ee_start_hi;    // 物理块号高16位
    __le32  ee_start_lo;    // 物理块号低32位
};

// 中间节点：指向下一级树块
struct ext4_extent_idx {
    __le32  ei_block;       // 覆盖的逻辑块起始号
    __le32  ei_leaf_lo;     // 下一级块的物理块号低32位
    __le16  ei_leaf_hi;     // 下一级块的物理块号高16位
};

// 树块头部：每个树块（索引节点或叶子节点）的开头
struct ext4_extent_header {
    __le16  eh_magic;       // 魔数 0xf30a
    __le16  eh_entries;     // 当前块中有效条目数
    __le16  eh_max;         // 最大容量
    __le16  eh_depth;       // 树深度（0=叶子，1=一层索引，...）
    __le32  eh_generation;  // 世代号
};
```

**extent 树结构**：

```
ext4_extent_header (inode->i_data 中，树根)
  |
  ├── eh_depth > 0: ext4_extent_idx[]
  │     └── 二分查找 → 读下一级树块
  │           └── ext4_extent_header (下一级块)
  │                 ├── eh_depth > 0: 继续索引层
  │                 └── eh_depth = 0: ext4_extent[] (叶子)
  │
  └── eh_depth = 0: ext4_extent[] (叶子节点，直接映射)
```

**ext4_map_blocks 双层缓存设计**（`fs/ext4/inode.c:732`）:

```c
int ext4_map_blocks(handle_t *handle, struct inode *inode,
                    struct ext4_map_blocks *map, int flags)
{
    // 第一阶段：内存 extent 状态树 (es_tree) 缓存查找
    if (ext4_es_lookup_extent(inode, map->m_lblk, NULL, &es, &map->m_seq)) {
        if (ext4_es_is_written(&es) || ext4_es_is_unwritten(&es)) {
            // 物理块号 = 缓存起始物理块 + (逻辑块号 - 缓存起始逻辑块)
            map->m_pblk = ext4_es_pblock(&es) +
                    map->m_lblk - es.es_lblk;
            map->m_flags |= EXT4_MAP_MAPPED;
            goto found;  // 缓存命中，直接返回
        }
    }

    // 第二阶段：缓存未命中，进入磁盘 extent 树查找
    down_read(&EXT4_I(inode)->i_data_sem);
    retval = ext4_map_query_blocks(handle, inode, map, flags);
    up_read(&EXT4_I(inode)->i_data_sem);

    // 将结果缓存到 es_tree，加速后续访问
    ext4_es_cache_extent(inode, map->m_lblk, map->m_len, map->m_pblk, status);
}
```

**extent 树遍历**（`fs/ext4/extents.c:4282`）:

```c
// 1. 逐层二分查找，直到叶子节点
path = ext4_find_extent(inode, map->m_lblk, NULL, 0);

// 2. 获取叶子节点中的 extent
ex = path[depth].p_ext;
ee_block = le32_to_cpu(ex->ee_block);       // extent 逻辑起始块
ee_start = ext4_ext_pblock(ex);             // extent 物理起始块
ee_len = ext4_ext_get_actual_len(ex);       // extent 覆盖块数

// 3. ★ 核心公式：物理块号 = 物理起始块 + (逻辑块号 - 逻辑起始块)
newblock = map->m_lblk - ee_block + ee_start;

// 4. 返回结果
map->m_pblk = newblock;                     // 物理块号
map->m_len = min(allocated, map->m_len);    // 实际映射长度
map->m_flags |= EXT4_MAP_MAPPED;
```

**物理块号到 bio 扇区号的转换**：

```c
// ext4_mpage_readpages() 中构造 bio 时
bio->bi_iter.bi_sector = first_block << (blkbits - 9);
// blkbits 是块大小对数（4K = 12），12 - 9 = 3
// 即每个 4KB 块 = 8 个 512 字节扇区
```

#### 7.3.3 块层 I/O 提交（blk_mq_submit_bio）

**bio 结构**（`include/linux/blk_types.h`）:

```c
struct bio {
    struct bio           *bi_next;        // 请求链表
    struct block_device  *bi_bdev;        // 目标块设备
    blk_opf_t            bi_opf;          // 操作码+标志 (REQ_OP_READ/WRITE)
    struct bio_vec       *bi_io_vec;      // 数据段向量数组
    struct bvec_iter     bi_iter;         // 迭代位置 (bi_sector, bi_size, bi_idx)
    bio_end_io_t         *bi_end_io;      // 完成回调
    unsigned short       bi_vcnt;         // bio_vec 数量
};

struct bio_vec {
    struct page    *bv_page;    // 物理页
    unsigned int   bv_len;      // 长度
    unsigned int   bv_offset;   // 页内偏移
};

struct bvec_iter {
    sector_t        bi_sector;    // 扇区位置 (512B 单位)
    unsigned int    bi_size;      // 剩余字节数
    unsigned int    bi_idx;       // 当前 bio_vec 索引
};
```

**submit_bio 到 blk_mq_submit_bio**:

```
submit_bio(bio)                              // block/blk-core.c:992
  └── submit_bio_noacct(bio)                 // 统计、重映射
       └── __submit_bio(bio)                 // block/blk-core.c:636
            └── blk_mq_submit_bio(bio)       // NVMe 等走此路径
                 ├── 尝试 plug 缓存合并 (blk_attempt_plug_merge)
                 ├── 尝试调度器合并 (blk_mq_sched_bio_merge)
                 ├── 分配 request: blk_mq_get_new_requests()
                 │    └── __blk_mq_alloc_requests()  // 从 tag 池分配
                 ├── 绑定 bio→request: blk_mq_bio_to_request()
                 │    ├── rq->bio = rq->biotail = bio
                 │    ├── rq->__sector = bio->bi_iter.bi_sector
                 │    └── rq->__data_len = bio->bi_iter.bi_size
                 └── 分发决策:
                      ├── 有 plug → blk_add_rq_to_plug() (批量下发)
                      ├── 有调度器 → blk_mq_insert_request()
                      │    └── dd_insert_requests()  // mq-deadline
                      │         └── 插入红黑树 + FIFO 链表
                      └── 无调度器 → blk_mq_try_issue_directly()
                           └── q->mq_ops->queue_rq(hctx, &bd)
                                └── nvme_queue_rq()
```

**request 结构**（`include/linux/blk-mq.h`）:

```c
struct request {
    struct request_queue    *q;           // 所属请求队列
    struct blk_mq_ctx       *mq_ctx;      // 软件队列上下文
    struct blk_mq_hw_ctx    *mq_hctx;     // 硬件队列上下文
    blk_opf_t               cmd_flags;    // 操作码和标志
    int                     tag;          // 驱动标签 (硬件队列中的位置)
    struct bio              *bio;         // 关联的 bio 链表头
    struct bio              *biotail;     // 链表尾
    unsigned short          nr_phys_segments;  // DMA 映射段数
    unsigned long           deadline;     // 超时时间
};
```

一个 request 可以包含多个 bio（通过 bio 合并），`blk_mq_bio_to_request` 建立绑定关系。

**I/O 调度器（mq-deadline）**:

```c
// block/mq-deadline.c
struct dd_per_prio {
    struct rb_root sort_list[DD_DIR_COUNT];  // 按扇区排序的红黑树
    struct list_head fifo_list[DD_DIR_COUNT]; // FIFO 过期队列
    sector_t latest_pos[DD_DIR_COUNT];        // 最近下发位置
};

struct deadline_data {
    struct list_head dispatch;               // 直通分发队列
    struct dd_per_prio per_prio[DD_PRIO_COUNT]; // RT/BE/IDLE 三级优先级
    unsigned int batching;                   // 批量下发计数
    ...
};
```

mq-deadline 调度策略：
- 维护按扇区排序的红黑树（实现电梯算法，减少寻道）
- 维护 FIFO 过期队列（防止请求饥饿，默认读 500ms、写 5s 超时）
- 三级优先级（RT > BE > IDLE）
- 批量下发时优先读（防止写饥饿）

#### 7.3.4 NVMe 驱动 I/O 提交

**nvme_queue_rq**（`drivers/nvme/host/pci.c:1405`）:

```c
static blk_status_t nvme_queue_rq(struct blk_mq_hw_ctx *hctx,
                 const struct blk_mq_queue_data *bd)
{
    struct nvme_queue *nvmeq = hctx->driver_data;  // NVMe 硬件队列
    struct request *req = bd->rq;
    struct nvme_iod *iod = blk_mq_rq_to_pdu(req);  // 驱动私有数据

    // 1. 准备 NVMe 命令 + DMA 映射
    ret = nvme_prep_rq(req);
    //    ├── nvme_setup_cmd() 填充 nvme_command
    //    │    └── nvme_setup_rw() 设置 opcode, nsid, slba, length
    //    └── nvme_map_data()  DMA 映射 (PRP 或 SGL)

    // 2. 提交到硬件
    spin_lock(&nvmeq->sq_lock);
    nvme_sq_copy_cmd(nvmeq, &iod->cmd);   // 命令拷贝到 SQ 环形缓冲区
    nvme_write_sq_db(nvmeq, bd->last);    // 写 Doorbell 通知设备
    spin_unlock(&nvmeq->sq_lock);
    return BLK_STS_OK;
}
```

**NVMe 命令提交到硬件的两个关键步骤**:

```c
// 1. 拷贝命令到 Submission Queue 环形缓冲区
static inline void nvme_sq_copy_cmd(struct nvme_queue *nvmeq,
                    struct nvme_command *cmd)
{
    memcpy(nvmeq->sq_cmds + (nvmeq->sq_tail << nvmeq->sqes),
           absolute_pointer(cmd), sizeof(*cmd));
    if (++nvmeq->sq_tail == nvmeq->q_depth)
        nvmeq->sq_tail = 0;  // 循环
}

// 2. 写 Doorbell 寄存器通知设备
static inline void nvme_write_sq_db(struct nvme_queue *nvmeq, bool write_sq)
{
    if (nvme_dbbuf_update_and_check_event(nvmeq->sq_tail,
            nvmeq->dbbuf_sq_db, nvmeq->dbbuf_sq_ei))
        writel(nvmeq->sq_tail, nvmeq->q_db);  // MMIO 写
    nvmeq->last_sq_tail = nvmeq->sq_tail;
}
```

**NVMe 命令格式**（`include/linux/nvme.h`）:

```c
struct nvme_rw_command {
    __u8            opcode;        // 操作码 (nvme_cmd_read / nvme_cmd_write)
    __u8            flags;
    __u16           command_id;    // 对应 request tag
    __le32          nsid;          // Namespace ID
    union nvme_data_ptr dptr;      // 数据指针 (PRP 或 SGL)
    __le64          slba;          // 起始 LBA
    __le16          length;        // 传输长度 (LBA 为单位减1)
    __le16          control;       // FUA, LR 等控制位
    __le32          dsmgmt;        // 数据流管理 hint
};
```

**NVMe 队列结构**（`drivers/nvme/host/pci.c`）:

```c
struct nvme_queue {
    struct nvme_dev *dev;
    void *sq_cmds;                         // SQ 内存区域 (DMA 一致)
    struct nvme_completion *cqes;          // CQ 条目数组 (DMA 一致)
    dma_addr_t sq_dma_addr;                // SQ DMA 地址
    u32 __iomem *q_db;                     // Doorbell 寄存器 MMIO 地址
    u32 q_depth;                           // 队列深度
    u16 sq_tail;                           // SQ 尾指针 (下次写入位置)
    u16 last_sq_tail;                      // 上次写入的 tail
    u16 cq_head;                           // CQ 头指针
    u16 qid;                               // 队列 ID
    u8 cq_phase;                           // CQ 阶段位 (检测新完成)
};
```

**完整 NVMe I/O 数据流**:

```
应用层
  │
  ▼
文件系统 (ext4_map_blocks → 物理块号)
  │
  ▼
submit_bio(bio)
  │
  ▼
blk_mq_submit_bio()
  ├── bio 合并 (plug + 调度器)
  ├── 分配 request (tag 池)
  ├── bio → request 绑定
  └── 分发 (直通/调度器)
       │
       ▼
nvme_queue_rq()
  ├── nvme_setup_cmd()  → 填充 nvme_command (opcode, nsid, slba, dptr)
  ├── nvme_map_data()   → DMA 映射 (PRP/SGL 描述符)
  ├── nvme_sq_copy_cmd() → memcpy 到 SQ 环形缓冲区
  └── nvme_write_sq_db() → writel() MMIO 写 Doorbell
       │
       ▼
NVMe 控制器 (PCIe DMA 读取数据)
  ├── 控制器读取 SQ 中的命令
  ├── 执行 DMA 传输 (从设备到主机内存)
  └── 写入 CQ 条目 + 发送 MSI-X 中断
       │
       ▼
完成中断 → nvme_irq()
  └── nvme_process_cq()
       └── 遍历 CQ 条目
            └── bio_endio(bio)
                 └── folio_end_read(folio)
                      └── 解锁 folio，唤醒等待进程
```

## 8. 写时复制（COW）

### 8.1 do_wp_page 和 do_cow_fault

写时复制用于 MAP_PRIVATE 映射：多个进程可以共享同一个物理页，直到其中一个进程写入时才会复制。

**写私有映射缺页路径：**

```
do_fault(vmf)  // 文件映射写缺页
  └── do_cow_fault(vmf)
       │
       ├── vmf_anon_prepare(vmf)  // 准备反向映射
       │
       ├── folio_prealloc()  // 分配新的匿名页
       │    └── 从伙伴系统分配物理页
       │
       ├── __do_fault(vmf)  // 从文件读取原始页
       │    └── filemap_fault()  → 获取页缓存页
       │
       ├── copy_mc_user_highpage()  // 复制页内容
       │    └── 从文件页缓存复制到新分配的匿名页
       │
       ├── __folio_mark_uptodate()
       │
       └── finish_fault(vmf)  // 建立 COW 页的 PTE 映射
            └── set_ptes()  // 新页标记为可写
```

**do_wp_page 用于匿名页的写时复制：**

```c
// mm/memory.c:4149
static vm_fault_t do_wp_page(struct vm_fault *vmf)
    __releases(vmf->ptl)
{
    // 处理写保护页的写入
    // 1. 检查页映射计数
    // 2. 如果只有 1 个映射 → 直接标记可写（无需复制）
    // 3. 如果有多个映射 → 复制页内容
    // 4. 更新 RMAP 反向映射
    // 5. 建立新 PTE 映射
}
```

### 8.2 COW 流程图

```
写私有映射的 COW 流程：

第一次写入（对 MAP_PRIVATE 文件映射或零页）

写缺页
    │
    ├── 匿名页（零页）的写缺页 → do_wp_page()
    │    │
    │    └── 零页是全局共享的，必须复制
    │         ├── alloc_anon_folio() 分配新页
    │         ├── 复制零页内容（实际上清零）
    │         └── 映射到新页（可写）
    │
    └── 文件页（页缓存）的写缺页 → do_cow_fault()
         │
         ├── 页缓存页是文件系统共享的
         │
         ├── folio_prealloc() 分配匿名页
         ├── filemap_fault() 获取页缓存页（共享）
         ├── copy_mc_user_highpage() 复制内容到匿名页
         │
         └── finish_fault()
              ├── 建立新页的 PTE 映射（可写）
              └── 页缓存页的引用计数减1
```

## 9. 交换（swap）

### 9.1 do_swap_page：从交换空间换入

```c
// mm/memory.c:4706
vm_fault_t do_swap_page(struct vm_fault *vmf)
{
    // 1. 从 PTE 中提取交换条目信息
    entry = softleaf_from_pte(vmf->orig_pte);

    // 2. 获取交换设备
    si = get_swap_device(entry);

    // 3. 尝试在交换缓存中查找
    folio = swap_cache_get_folio(entry);
    if (!folio) {
        // 换入：从交换设备读取
        if (data_race(si->flags & SWP_SYNCHRONOUS_IO))
            folio = swapin_folio(entry, alloc_swap_folio(vmf));
        else
            folio = swapin_readahead(entry, GFP_HIGHUSER_MOVABLE, vmf);

        // 块 I/O 读取交换数据
    }

    // 4. 建立 PTE 映射
    set_pte_at(vma->vm_mm, address, vmf->pte, pte);
    folio_add_lru_vma(folio, vma);
    folio_add_new_anon_rmap(folio, vma, address, RMAP_EXCLUSIVE);
}
```

### 9.2 换出路径

```
kswapd / 直接回收
    │
    └── shrink_folio_list()
         │
         ├── 匿名页 → swap_writepage() 写入交换
         │    │
         │    └── 检查页是否被多个进程映射
         │         ├── 是 → 保留页缓存，复制到交换
         │         └── 否 → 直接写入交换
         │
         ├── 文件页 → 脏页写回
         │    │
         │    └── 如果文件页是干净的 → 直接丢弃
         │
         └── 更新 PTE 为交换条目
              └── set_pte_at(mm, addr, pte, swp_entry_to_pte(entry))
                   └── PTE 中保存交换设备号和偏移量
```

## 10. 文件映射的脏页回写

### 10.1 do_shared_fault 的脏页处理

```c
// mm/memory.c:5853
static vm_fault_t do_shared_fault(struct vm_fault *vmf)
{
    // 1. 调用文件系统 fault 回调（如 filemap_fault）
    ret = __do_fault(vmf);

    // 2. 通知文件系统页面即将变为可写
    if (vma->vm_ops->page_mkwrite) {
        tmp = do_page_mkwrite(vmf, folio);
        // 文件系统设置页为脏，准备写回
    }

    // 3. 建立 PTE 映射
    ret |= finish_fault(vmf);

    // 4. 标记脏页
    ret |= fault_dirty_shared_page(vmf);
    // 将页标记为脏，触发 writeback
}
```

### 10.2 脏页写回路径

```
MAP_SHARED 写入后的脏页写回：

fault_dirty_shared_page(vmf)
    │
    └── folio_mark_dirty(folio)
         │
         └── 将页标记为脏
              │
              └── 后台回写（writeback）
                   │
                   ├── pdflush / wb_workfn
                   │
                   └── writeback_single_inode()
                        └── mapping->a_ops->writepages()
                             │
                             └── ext4_writepages()  // 以 ext4 为例
                                  ├── ext4_map_blocks() 查找物理块
                                  ├── 提交 bio 写请求
                                  └── submit_bio()
                                       └── 块设备写入
```

## 11. 页表操作详解

### 11.1 ARM64 页表层级

```
ARM64 使用 4 级或 5 级页表（取决于配置）：

虚拟地址 (48位 或 52位)
    │
    ├── PGD (页全局目录)     : 9位  → 512 个表项
    │    └── pgd_t pgd[512]  : 每个表项指向一个 P4D
    │
    ├── P4D (页四级目录)     : 9位  → 512 个表项
    │    └── p4d_t p4d[512]  : 每个表项指向一个 PUD
    │
    ├── PUD (页上级目录)     : 9位  → 512 个表项
    │    └── pud_t pud[512]  : 每个表项指向一个 PMD
    │
    ├── PMD (页中间目录)     : 9位  → 512 个表项
    │    └── pmd_t pmd[512]  : 每个表项指向一个 PTE 数组
    │         └── 大页支持：PMD 直接映射 2MB 连续物理页
    │
    ├── PTE (页表项)         : 9位  → 512 个表项
    │    └── pte_t pte[512]  : 每个表项映射 4KB 物理页
    │
    └── 页内偏移             : 12位 → 4KB 页内寻址

页大小：4KB（默认）
大页：2MB（PMD 级）、1GB（PUD 级）
```

### 11.2 PTE 格式

```c
// ARM64 PTE 格式（每项 64 位）
struct {
    unsigned long present:1;       // 页在内存中
    unsigned long writable:1;      // 可写
    unsigned long user:1;          // 用户态可访问
    unsigned long write_through:1; // 写直达
    unsigned long cache_disable:1; // 禁止缓存
    unsigned long accessed:1;      // 访问位
    unsigned long dirty:1;         // 脏位
    unsigned long huge:1;          // 大页
    unsigned long global:1;        // 全局映射
    unsigned long pxn:1;           // 禁止特权执行
    unsigned long uxn:1;           // 禁止用户执行
    unsigned long pfn:36;          // 物理页帧号
    unsigned long sw_reserved:16;  // 软件保留位（交换条目等）
};
```

### 11.3 缺页处理中的页表操作

```c
// 缺页处理中分配页表
vmf->prealloc_pte = pte_alloc_one(vma->vm_mm);  // 预分配 PTE 页表
pmd_install(mm, vmf->pmd, &vmf->prealloc_pte);  // 安装 PMD → PTE 链接

// 设置 PTE 映射
set_ptes(vma->vm_mm, addr, vmf->pte, entry, nr_pages);
// 相当于 set_pte_at() 循环设置每个 PTE

// 刷新 TLB（当需要时）
update_mmu_cache_range(vmf, vma, addr, vmf->pte, nr_pages);
```

### 11.4 页表页的物理分配

缺页处理中，页表页本身也是物理内存，从伙伴系统分配。

#### 11.4.1 分配链

```
pte_alloc_one(mm)
  └── __pte_alloc_one_noprof(mm, GFP_PGTABLE_USER)
       └── pagetable_alloc_noprof(gfp, 0)       // include/linux/mm.h:3402
            └── alloc_pages_noprof(gfp | __GFP_COMP, 0)
                 └── 伙伴系统分配 1 个 4KB 物理页
       └── pagetable_pte_ctor(mm, ptdesc)        // 初始化页表锁
```

所有中间页表层级（PTE、PMD、PUD）都从伙伴系统分配**单个 4KB 物理页**：

```c
// include/linux/mm.h:3402
static inline struct ptdesc *pagetable_alloc_noprof(gfp_t gfp, unsigned int order)
{
    struct page *page = alloc_pages_noprof(gfp | __GFP_COMP, order);
    return page_ptdesc(page);
}
```

返回的 `struct ptdesc` 是页表描述符，与 `struct page` 共用同一存储空间。`GFP_PGTABLE_USER` 为 `GFP_KERNEL | __GFP_ACCOUNT`。

#### 11.4.2 各层级页表分配函数

| 层级 | 分配函数 | 调用位置 |
|------|----------|----------|
| PTE | `pte_alloc_one(mm)` | `__pte_alloc()` → `do_anonymous_page()`, `finish_fault()` |
| PMD | `pmd_alloc_one(mm, addr)` | `__pmd_alloc()` → `pmd_alloc()` → `__handle_mm_fault()` |
| PUD | `pud_alloc_one(mm, addr)` | `__pud_alloc()` → `pud_alloc()` → `__handle_mm_fault()` |

所有函数都调用 `pagetable_alloc_noprof(gfp, 0)`，分配 4KB 物理页。

#### 11.4.3 页表安装过程

**`pte_alloc()` 宏**（`include/linux/mm.h:3570`）：

```c
#define pte_alloc(mm, pmd) (unlikely(pmd_none(*(pmd))) && __pte_alloc(mm, pmd))
```

先检查 PMD 是否为空（快速路径），为空时才调用 `__pte_alloc()`。

**`__pte_alloc()`**（`mm/memory.c:464`）:

```c
int __pte_alloc(struct mm_struct *mm, pmd_t *pmd)
{
    pgtable_t new = pte_alloc_one(mm);  // 分配 PTE 页表页
    if (!new) return -ENOMEM;

    pmd_install(mm, pmd, &new);         // 安装到 PMD 项
    if (new) pte_free(mm, new);         // 竞态失败则释放
    return 0;
}
```

**`pmd_install()`**（`mm/memory.c:438`）:

```c
void pmd_install(struct mm_struct *mm, pmd_t *pmd, pgtable_t *pte)
{
    spinlock_t *ptl = pmd_lock(mm, pmd);
    if (likely(pmd_none(*pmd))) {
        mm_inc_nr_ptes(mm);             // 统计计数
        smp_wmb();                       // 写屏障确保页表可见
        pmd_populate(mm, pmd, *pte);    // 将 PTE 页表物理地址填入 PMD
        *pte = NULL;                     // 标记已安装
    }
    spin_unlock(ptl);
}
```

**`pmd_populate()` ARM64 实现**（`arch/arm64/include/asm/pgalloc.h`）:

```c
static inline void pmd_populate(struct mm_struct *mm, pmd_t *pmdp, pgtable_t ptep)
{
    __pmd_populate(pmdp, page_to_phys(ptep),               // PTE 页表的物理地址
               PMD_TYPE_TABLE | PMD_TABLE_AF | PMD_TABLE_PXN);
}
```

PMD 表项中填入 PTE 页表的物理地址，并设置类型位（`PMD_TYPE_TABLE`）、访问位（`AF`）和特权执行禁止位（`PXN`）。

#### 11.4.4 缺页路径中的延迟分配策略

```c
// mm/memory.c:6273, handle_pte_fault()
if (unlikely(pmd_none(*vmf->pmd))) {
    /* Leave __pte_alloc() until later: because vm_ops->fault may
     * want to allocate huge page, and if we expose page table
     * for an instant, it will be difficult to retract from
     * concurrent faults and from rmap lookups. */
    vmf->pte = NULL;
    vmf->flags &= ~FAULT_FLAG_ORIG_PTE_VALID;
}
```

**延迟分配**：当 PMD 为空时，`handle_pte_fault()` 不立即分配 PTE 页表，而是推迟到 `do_fault_around()` 或 `do_anonymous_page()` 中。原因：文件系统的 `vm_ops->fault` 可能希望分配透明大页（THP），提前暴露 PTE 页表会导致后续无法回退。

### 11.5 ARM64 set_ptes 与 TLB 维护

#### 11.5.1 set_ptes() ARM64 实现

```c
// arch/arm64/include/asm/pgtable.h:1763
static __always_inline void set_ptes(struct mm_struct *mm, unsigned long addr,
                pte_t *ptep, pte_t pte, unsigned int nr)
{
    pte = pte_mknoncont(pte);      // 清除连续位标记

    if (likely(nr == 1)) {
        contpte_try_unfold(mm, addr, ptep, __ptep_get(ptep));  // 展开连续 PTE
        __set_ptes(mm, addr, ptep, pte, 1);                    // 设置单个 PTE
        contpte_try_fold(mm, addr, ptep, pte);                  // 尝试折叠回连续
    } else {
        contpte_set_ptes(mm, addr, ptep, pte, nr);              // 批量设置
    }
}
```

ARM64 特有 **contpte（连续 PTE）优化**：当相邻 PTE 具有相同属性时，合并为连续条目，减少 TLB 占用。

底层 `__set_ptes_anysz()` 写入 PTE 的流程：

```c
static inline void __set_ptes_anysz(struct mm_struct *mm, unsigned long addr,
                    pte_t *ptep, pte_t pte, unsigned int nr, ...)
{
    // 1. 页表安全检查
    page_table_check_ptes_set(mm, addr, ptep, pte, nr);
    // 2. 同步缓存和标签
    __sync_cache_and_tags(pte, nr * stride);
    // 3. 循环写入 PTE
    for (;;) {
        __check_safe_pte_update(mm, ptep, pte);
        __set_pte_nosync(ptep, pte);  // WRITE_ONCE(*ptep, pte)
        if (--nr == 0) break;
        ptep++;
        pte = pte_advance_pfn(pte, stride);
    }
}
```

#### 11.5.2 update_mmu_cache_range() ARM64 实现

```c
// arch/arm64/include/asm/pgtable.h:1563
static inline void update_mmu_cache_range(struct vm_fault *vmf,
        struct vm_area_struct *vma, unsigned long addr, pte_t *ptep,
        unsigned int nr)
{
    /* We don't do anything here, so there's a very small chance of
     * us retaking a user fault which we just fixed up. The alternative
     * is doing a dsb(ishst), but that penalises the fastpath. */
}
```

ARM64 上 **`update_mmu_cache_range()` 为空函数**。原因：
- ARM64 的 cache 一致性由硬件维护（`__set_ptes_anysz` 中的 `__sync_cache_and_tags` 已处理）
- 不做 `dsb(ishst)` 避免在快速路径中引入额外开销

#### 11.5.3 ARM64 TLB 刷新

**`flush_tlb_page()` 单页刷新**：

```c
// arch/arm64/include/asm/tlbflush.h:364
static inline void flush_tlb_page(struct vm_area_struct *vma, unsigned long uaddr)
{
    flush_tlb_page_nosync(vma, uaddr);
    __tlbi_sync_s1ish();  // dsb(ish) + isb
}

// 底层实现
static inline void __flush_tlb_page_nosync(struct mm_struct *mm, unsigned long uaddr)
{
    dsb(ishst);                                      // 确保之前 PTE 写入完成
    addr = __TLBI_VADDR(uaddr, ASID(mm));            // 构建含 ASID 的 VA 参数
    __tlbi(vale1is, addr);                           // TLBI VALE1IS（叶级失效）
    __tlbi_user(vale1is, addr);                      // EL0 映射再刷一次
    mmu_notifier_arch_invalidate_secondary_tlbs(mm, ...);
}
```

使用 `vale1is` 指令（叶级失效），只刷最后一级 TLB 条目，不涉及 walk cache。

**`flush_tlb_range()` 范围刷新**：

```c
// arch/arm64/include/asm/tlbflush.h:542
static inline void flush_tlb_range(struct vm_area_struct *vma,
                   unsigned long start, unsigned long end)
{
    // last_level = false: 非叶级失效（可能操作页表项）
    // tlb_level = TLBI_TTL_UNKNOWN: 由硬件自行查找
    __flush_tlb_range(vma, start, end, PAGE_SIZE, false, TLBI_TTL_UNKNOWN);
}
```

**TLBI 范围优化**（`__flush_tlb_range_op`）：

当 CPU 支持 TLB 范围操作时，使用 `r#op` 系列指令（如 `rVALE1IS`）批量刷新，避免逐页刷新的开销。不支持时回退到逐页刷新。

```
TLB 刷新指令选择：
  flush_tlb_mm()       → TLBI ASIDE1IS    (全 ASID 失效)
  flush_tlb_page()     → TLBI VALE1IS     (单页叶级失效)
  flush_tlb_range()    → TLBI VAE1IS      (范围非叶级失效)
                         TLBI rVAE1IS     (范围操作，硬件支持时)
```

### 11.6 透明大页（THP）

#### 11.6.1 __handle_mm_fault 中的 THP 检查

```c
// mm/memory.c:6355, __handle_mm_fault()

// PUD 级大页检查
if (pud_none(*vmf.pud) &&
    thp_vma_allowable_order(vma, vm_flags, TVA_PAGEFAULT, PUD_ORDER)) {
    ret = create_huge_pud(&vmf);  // 仅文件映射支持
    if (!(ret & VM_FAULT_FALLBACK))
        return ret;
}

// PMD 级大页检查（核心路径）
if (pmd_none(*vmf.pmd) &&
    thp_vma_allowable_order(vma, vm_flags, TVA_PAGEFAULT, PMD_ORDER)) {
    ret = create_huge_pmd(&vmf);
    if (ret & VM_FAULT_FALLBACK)
        goto fallback;  // 回退到 4K 小页
    else
        return ret;
}

// 已有 PMD 大页的后续处理
if (pmd_trans_huge(vmf.orig_pmd)) {
    if (pmd_protnone(vmf.orig_pmd))
        return do_huge_pmd_numa_page(&vmf);  // NUMA 迁移
    if ((flags & FAULT_FLAG_WRITE) && !pmd_write(vmf.orig_pmd))
        return wp_huge_pmd(&vmf);             // THP 写时复制
}
```

#### 11.6.2 create_huge_pmd 分发器

```c
// mm/memory.c:6139
static inline vm_fault_t create_huge_pmd(struct vm_fault *vmf)
{
    if (vma_is_anonymous(vma))
        return do_huge_pmd_anonymous_page(vmf);  // 匿名：分配 THP
    if (vma->vm_ops->huge_fault)
        return vma->vm_ops->huge_fault(vmf, PMD_ORDER);  // 文件：文件系统回调
    return VM_FAULT_FALLBACK;
}
```

#### 11.6.3 匿名 THP 缺页

```c
// mm/huge_memory.c:1461
vm_fault_t do_huge_pmd_anonymous_page(struct vm_fault *vmf)
{
    // 1. 检查地址对齐
    if (!thp_vma_suitable_order(vma, haddr, PMD_ORDER))
        return VM_FAULT_FALLBACK;
    // 2. 准备反向映射
    ret = vmf_anon_prepare(vmf);
    // 3. 注册到 khugepaged 扫描列表
    khugepaged_enter_vma(vma, vma->vm_flags);
    // 4. 只读缺页 → 使用 PMD 级零页
    if (!(vmf->flags & FAULT_FLAG_WRITE) && ...)
        return set_huge_zero_folio(vmf);
    // 5. 写缺页 → 分配真实大页
    return __do_huge_pmd_anonymous_page(vmf);
}
```

**大页物理页分配**：

```c
// mm/huge_memory.c:1323
static vm_fault_t __do_huge_pmd_anonymous_page(struct vm_fault *vmf)
{
    // 分配 order=9 的 folio（2MB 连续物理内存）
    folio = vma_alloc_anon_folio_pmd(vma, vmf->address);
    if (unlikely(!folio))
        return VM_FAULT_FALLBACK;

    // 预分配 PTE 页表（用于分拆时回退）
    pgtable = pte_alloc_one(vma->vm_mm);

    // 建立 PMD 级映射
    map_anon_folio_pmd_pf(folio, vmf->pmd, vma, haddr);
    // 内部调用 set_pmd_at() 设置 PMD 表项
}
```

分配参数：`order = HPAGE_PMD_ORDER = 9`，即 2MB 连续物理内存。GFP 标志由 `vma_thp_gfp_mask` 根据 defrag 策略确定：
- `always` → `GFP_TRANSHUGE`（同步压缩）
- `defer` → `GFP_TRANSHUGE_LIGHT | __GFP_KSWAPD_RECLAIM`
- `madvise` → 仅 `MADV_HUGEPAGE` 的 VMA 使用同步压缩

#### 11.6.4 THP 写时复制（COW）

```c
// mm/huge_memory.c:2060
vm_fault_t do_huge_pmd_wp_page(struct vm_fault *vmf)
{
    // 1. 零页 → 分配真实大页
    if (is_huge_zero_pmd(orig_pmd))
        return do_huge_zero_wp_pmd(vmf);

    // 2. 检查引用计数，尝试复用
    if (folio_ref_count(folio) == 1) {
        // 唯一映射 → 直接标记可写，无需复制
        entry = pmd_mkyoung(orig_pmd);
        entry = maybe_pmd_mkwrite(pmd_mkdirty(entry), vma);
        pmdp_set_access_flags(...);
        return 0;
    }

    // 3. 无法复用 → 分拆为 512 个 4K 小页
    __split_huge_pmd(vma, vmf->pmd, vmf->address, false);
    return VM_FAULT_FALLBACK;  // 回退到 do_wp_page 逐个处理
}
```

核心策略：尽量复用大页（引用计数为 1 且独占），否则将大页分拆为 512 个小页，由 `do_wp_page` 逐个处理 COW。

#### 11.6.5 khugepaged 后台合并

mmap 创建 VMA 时注册到 khugepaged 扫描列表：

```c
// mm/vma.c:2562, __mmap_new_vma() 末尾
khugepaged_enter_vma(vma, vma->vm_flags);
```

khugepaged 内核线程定期扫描已注册进程的 VMA，尝试将 4K 小页合并为 2MB 大页：

```
khugepaged 内核线程               // mm/khugepaged.c:2612
  │
  ├── 遍历所有注册的 mm_struct
  │    └── khugepaged_scan_mm_slot()
  │         └── 遍历进程的 VMA 列表
  │              └── hpage_collapse_scan_pmd()
  │                   │
  │                   ├── 扫描 PMD 范围内的 512 个 PTE
  │                   ├── 检查合并条件:
  │                   │    ├── 空页 ≤ khugepaged_max_ptes_none
  │                   │    ├── 交换页 ≤ khugepaged_max_ptes_swap
  │                   │    ├── 共享页 ≤ khugepaged_max_ptes_shared
  │                   │    ├── 所有页都是匿名页
  │                   │    └── 足够的年轻引用
  │                   │
  │                   └── 条件满足 → collapse_huge_page()
  │                        └── 分配 2MB 连续物理页
  │                             └── 复制 512 个小页内容
  │                                  └── 替换 PMD 为大页映射
  │
  └── 休眠 (khugepaged_pages_to_scan 页后)
```

#### 11.6.6 mTHP（multi-size THP）支持

Linux 内核支持**多尺寸透明大页**，允许使用 2MB 以外的其他大页尺寸（如 64KB, 128KB, 256KB 等）。

**全局控制位图**（`mm/huge_memory.c`）:

```c
unsigned long huge_anon_orders_always __read_mostly;   // always 策略
unsigned long huge_anon_orders_madvise __read_mostly;  // madvise 策略
unsigned long huge_anon_orders_inherit __read_mostly;  // inherit 策略
```

每个 bit 表示对应 order 的大页是否启用。支持的 order 范围：

```c
#define THP_ORDERS_ALL_ANON    ((BIT(PMD_ORDER + 1) - 1) & ~(BIT(0) | BIT(1)))
// order 2 ~ PMD_ORDER (通常为 9)，即 16KB ~ 2MB
```

**sysfs 配置接口**：每个 order 有独立的 `/sys/kernel/mm/transparent_hugepage/hugepages-<size>kB/` 目录，支持 `always` / `inherit` / `madvise` / `never` 四种状态。

**缺页路径中的 order 筛选**（`include/linux/huge_mm.h`）:

```c
unsigned long thp_vma_allowable_orders(struct vm_area_struct *vma,
                                       vm_flags_t vm_flags,
                                       enum tva_type type,
                                       unsigned long orders)
{
    if (vma_is_anonymous(vma)) {
        unsigned long mask = READ_ONCE(huge_anon_orders_always);
        if (vm_flags & VM_HUGEPAGE)
            mask |= READ_ONCE(huge_anon_orders_madvise);
        if (hugepage_global_always() ||
            ((vm_flags & VM_HUGEPAGE) && hugepage_global_enabled()))
            mask |= READ_ONCE(huge_anon_orders_inherit);
        orders &= mask;  // 取交集，只保留允许的 order
    }
    return __thp_vma_allowable_orders(vma, vm_flags, type, orders);
}
```

**地址对齐检查**：

```c
static inline bool thp_vma_suitable_order(struct vm_area_struct *vma,
        unsigned long addr, int order)
{
    haddr = ALIGN_DOWN(addr, PAGE_SIZE << order);
    // 大页必须完全落在 VMA 范围内
    if (haddr < vma->vm_start || haddr + hpage_size > vma->vm_end)
        return false;
    return true;
}
```

## 12. 完整流程图

### 12.1 文件映射完整流程

```
用户态: mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)
    │
    ▼
第一阶段: mmap 系统调用
────────────────────────────────────────────────────────────────────
    │
    ├─ ksys_mmap_pgoff()
    │    ├─ fget(fd)              // 获取文件引用，增加引用计数
    │    └─ vm_mmap_pgoff()
    │         ├─ mmap_write_lock()
    │         └─ do_mmap()
    │              ├─ calc_vm_prot_bits(prot)   // PROT_READ|WRITE → VM_READ|VM_WRITE
    │              ├─ calc_vm_flag_bits(flags)  // MAP_SHARED → VM_SHARED|VM_MAYSHARE
    │              ├─ __get_unmapped_area()
    │              │    └─ arch_get_unmapped_area_topdown()
    │              │         └─ unmapped_area_topdown()  // 遍历 VMA 红黑树
    │              └─ mmap_region()
    │                   └─ __mmap_region()
    │                        ├─ __mmap_setup()
    │                        │    ├─ 查找重叠 VMA（如果有，准备解除映射）
    │                        │    ├─ may_expand_vm()  // 检查地址空间限制
    │                        │    └─ security_vm_enough_memory_mm()  // overcommit 检查
    │                        │
    │                        ├─ vma_merge_new_range()  // 尝试合并到相邻 VMA
    │                        │    └─ 合并条件：相同文件、连续偏移、相同权限
    │                        │
    │                        └─ __mmap_new_vma()  // 创建新 VMA（合并失败时）
    │                             ├─ vm_area_alloc()  // kmem_cache_alloc
    │                             │    └─ 分配 ~200 字节的 VMA 结构体
    │                             │
    │                             ├─ vma_set_range()  // 设置 vm_start, vm_end, vm_pgoff
    │                             ├─ vm_flags_init()  // 设置 vm_flags
    │                             │
    │                             ├─ __mmap_new_file_vma()
    │                             │    ├─ vma->vm_file = file
    │                             │    └─ mmap_file() → ext4_file_mmap()
    │                             │         └─ vma->vm_ops = ext4_file_vm_ops
    │                             │              └─ .fault = filemap_fault
    │                             │
    │                             ├─ vma_iter_store_new()  // 插入 VMA 红黑树
    │                             │    └─ mm->map_count++
    │                             │
    │                             └─ vma_link_file()  // 建立 address_space 映射
    │                                  └─ i_mmap_lock → i_mmap_tree 中插入
    │
    └─ mmap_write_unlock()
    │
    ▼
    VMA 创建完成，但未分配物理页
    ─────────────────────────────
    VMA 信息:
      vm_start   = 0x7f...000
      vm_end     = 0x7f...000 + len
      vm_file    = file (ext4 inode)
      vm_ops     = ext4_file_vm_ops
      vm_flags   = VM_READ|VM_WRITE|VM_SHARED|VM_MAYSHARE
      vm_pgoff   = 0

第二阶段: 缺页异常（首次访问）
────────────────────────────────────────────────────────────────────
    │
    用户态: *ptr = 'A';  // 首次写入映射地址
    │
    ▼
    CPU 触发缺页 (Data Abort)
    │
    ├─ MMU 查找页表未命中 → 缺页异常
    │    └─ ESR_ELx 记录: DFSC = 0b000101 (translation fault, level 3)
    │
    ├─ arm64 缺页入口
    │    └─ do_mem_abort(esr, regs)
    │         └─ do_page_fault(far, esr, regs)
    │              ├─ 解析 ESR: is_write_abort = true → FAULT_FLAG_WRITE
    │              ├─ lock_vma_under_rcu()  // 快速路径：VMA 锁
    │              │    └─ 检查 VMA 权限：VM_WRITE OK
    │              │
    │              └─ handle_mm_fault(vma, addr, flags, regs)
    │                   └─ __handle_mm_fault(vma, addr, flags)
    │                        │
    │                        ├─ pgd_offset() → pgd 存在，跳过
    │                        ├─ p4d_alloc() → p4d 存在，跳过
    │                        ├─ pud_alloc() → pud 存在，跳过
    │                        ├─ pmd_alloc() → 分配 PMD 页表（如果不存在）
    │                        │    └─ 从伙伴系统分配 4KB 页表页
    │                        │
    │                        └─ handle_pte_fault(vmf)
    │                             │
    │                             ├─ pmd_none() = false
    │                             ├─ pte_offset_map_rw_nolock() → 获取 PTE 指针
    │                             ├─ pte_none(orig_pte) = true  // PTE 为空
    │                             │
    │                             └─ do_pte_missing() → do_fault(vmf)
    │                                  │
    │                                  └─ FAULT_FLAG_WRITE & !VM_SHARED
    │                                       │
    │                                       └─ do_cow_fault(vmf)
    │                                            │
    │                                            ├─ vmf_anon_prepare()
    │                                            │    └─ 分配 anon_vma
    │                                            │
    │                                            ├─ folio_prealloc()  // 分配 COW 页
    │                                            │    └─ 伙伴系统分配物理页
    │                                            │
    │                                            ├─ __do_fault(vmf)
    │                                            │    │
    │                                            │    └─ vma->vm_ops->fault(vmf)
    │                                            │         │
    │                                            │         └─ filemap_fault(vmf)
    │                                            │              │
    │                                            │              ├─ filemap_get_folio()
    │                                            │              │    └─ xa_load(&mapping->i_pages, index)
    │                                            │              │         └─ 页缓存查找 → 未命中
    │                                            │              │
    │                                            │              ├─ do_sync_mmap_readahead()
    │                                            │              │    └─ 发起同步预读
    │                                            │              │
    │                                            │              ├─ __filemap_get_folio(FGP_CREAT)
    │                                            │              │    └─ 在页缓存中分配新页
    │                                            │              │         └─ xa_store(&mapping->i_pages, ...)
    │                                            │              │
    │                                            │              ├─ lock_folio_maybe_drop_mmap()
    │                                            │              │
    │                                            │              └─ filemap_read_folio()
    │                                            │                   │
    │                                            │                   └─ mapping->a_ops->read_folio()
    │                                            │                        │
    │                                            │                        └─ ext4_read_folio()
    │                                            │                             │
    │                                            │                             ├─ ext4_map_blocks()
    │                                            │                             │    └─ 查询 extent 树 → 物理块号
    │                                            │                             │
    │                                            │                             ├─ bio_alloc()
    │                                            │                             ├─ bio_set_dev()
    │                                            │                             ├─ bio_add_page()
    │                                            │                             │
    │                                            │                             └─ submit_bio(bio)
    │                                            │                                  │
    │                                            │                                  ├─ blk_mq_submit_bio()
    │                                            │                                  │    ├─ 选择硬件队列
    │                                            │                                  │    ├─ 构造 request
    │                                            │                                  │    └─ nvme_queue_rq()
    │                                            │                                  │         └─ 写入 SQ 门铃
    │                                            │                                  │
    │                                            │                                  └─ 磁盘 I/O 完成中断
    │                                            │                                       ├─ nvme_irq()
    │                                            │                                       ├─ bio_endio()
    │                                            │                                       └─ folio_end_read()
    │                                            │                                            └─ 唤醒等待进程
    │                                            │
    │                                            ├─ copy_mc_user_highpage()
    │                                            │    └─ 从页缓存页复制到 COW 匿名页
    │                                            │
    │                                            └─ finish_fault(vmf)
    │                                                 │
    │                                                 ├─ pmd_none() = false (已分配)
    │                                                 │
    │                                                 ├─ pmd_install() 跳过
    │                                                 │
    │                                                 └─ set_ptes(mm, addr, pte, entry, 1)
    │                                                      │
    │                                                      └─ set_pte_at(mm, addr, pte, entry)
    │                                                           │
    │                                                           └─ 写入 PTE 到页表
    │                                                                PTE 值:
    │                                                                │  present=1, writable=1
    │                                                                │  user=1, dirty=1, accessed=1
    │                                                                │  pfn=COW_匿名页_物理帧号
    │
    ▼
    缺页处理完成，返回用户态
    ─────────────────────────
    CPU 重新执行指令 *ptr = 'A'，此时 PTE 有效，写入成功
```

### 12.2 匿名映射完整流程

```
用户态: ptr = mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    │
    ▼
第一阶段: mmap 系统调用
────────────────────────────────────────────────────────────────────
    │
    └─ do_mmap()
         └─ mmap_region()
              └─ __mmap_region()
                   ├─ __mmap_setup()
                   └─ __mmap_new_vma()
                        ├─ vma_set_anonymous(vma)
                        │    └─ vma->vm_ops = NULL
                        │    └─ vma->anon_vma = NULL (延迟分配)
                        │
                        └─ vma_iter_store_new(vma)  // 插入红黑树
    │
    ▼
    VMA 创建完成（vm_flags 包含 VM_ANON | VM_READ | VM_WRITE）

第二阶段: 第一次读访问
────────────────────────────────────────────────────────────────────
    │
    char val = *ptr;  // 读操作
    │
    ▼
    do_pte_missing() → do_anonymous_page()
         │
         ├── 非写缺页，不禁用零页
         │
         ├── pte_mkspecial(pfn_pte(my_zero_pfn(), vma->vm_page_prot))
         │    └── 映射到全局零页 (PFN 0 或特殊零页 PFN)
         │
         └── set_ptes() 设置 PTE
              PTE: present=1, writable=0 (只读), special=1, pfn=零页PFN
    │
    ▼
    返回零页（读取值 = 0）

第三阶段: 第一次写访问
────────────────────────────────────────────────────────────────────
    │
    *ptr = 42;  // 写操作
    │
    ▼
    do_wp_page()  // 写零页触发 COW
         │
         ├── 零页只有只读映射
         │
         ├── vmf_anon_prepare() 分配 anon_vma
         │
         ├── alloc_anon_folio() 分配物理页
         │    └── 伙伴系统分配 4KB 页，清零
         │
         ├── 复制内容（零页内容 = 全 0）
         │
         ├── 建立新 PTE 映射
         │    PTE: present=1, writable=1, dirty=1, pfn=新物理页PFN
         │
         └── 插入反向映射 + LRU
    │
    ▼
    *ptr = 42 成功写入

第四阶段: 第二次读访问
────────────────────────────────────────────────────────────────────
    │
    val = *ptr;  // 读操作
    │
    ▼
    PTE 存在且 Present → 直接读取，不触发缺页
```

## 13. 关键函数调用栈

### 13.1 mmap 系统调用栈

```
mmap(addr, len, prot, flags, fd, offset)
  └── arch/arm64/kernel/sys.c: __arm64_sys_mmap()
       └── ksys_mmap_pgoff(addr, len, prot, flags, fd, pgoff)  // mm/mmap.c:567
            └── vm_mmap_pgoff(file, addr, len, prot, flags, pgoff)  // mm/mmap.c:1350
                 └── mmap_lock_write_lock_killable(mm)
                 └── do_mmap(file, addr, len, prot, flags, vm_flags, pgoff, populate, uf)  // mm/mmap.c:335
                      ├── calc_vm_prot_bits(prot, pkey)  // 计算 vm_flags
                      ├── calc_vm_flag_bits(flags, file)
                      ├── __get_unmapped_area(file, addr, len, pgoff, flags, vm_flags)  // mm/mmap.c:2260
                      │    └── arch_get_unmapped_area_topdown(addr, len, pgoff, flags, vm_flags)
                      │         └── unmapped_area_topdown(info)  // mm/vma.c:3004
                      ├── find_vma_intersection()  // MAP_FIXED_NOREPLACE
                      ├── FILE 映射检查
                      └── mmap_region(file, addr, len, vm_flags, pgoff, uf)  // mm/vma.c:2818
                           ├── __mmap_region(file, addr, len, vm_flags, pgoff, uf)  // mm/vma.c:2720
                           │    ├── __mmap_setup(map, desc, uf)  // mm/vma.c:2392
                           │    │    ├── vma_find() → init_vma_munmap()  // 查找重叠 VMA
                           │    │    ├── vms_gather_munmap_vmas()  // 收集要解除映射的 VMA
                           │    │    ├── may_expand_vm()  // 地址空间限制检查
                           │    │    ├── security_vm_enough_memory_mm()  // overcommit 检查
                           │    │    └── vms_clean_up_area()  // 清除旧 PTE
                           │    │
                           │    ├── vma_merge_new_range()  // 尝试合并 VMA (mm/vma.c 或 mm/vma.h)
                           │    │    └── vma_merge_struct() 检查合并条件
                           │    │
                           │    └── __mmap_new_vma(map, &vma)  // 创建新 VMA
                           │         ├── vm_area_alloc(mm)  // 分配 VMA 结构体
                           │         ├── vma_set_range(vma, addr, end, pgoff)
                           │         ├── vm_flags_init(vma, flags)
                           │         ├── vma->vm_page_prot = map->page_prot
                           │         ├── FILE 映射:
                           │         │    └── __mmap_new_file_vma(map, vma)
                           │         │         └── mmap_file(file, vma) → file->f_op->mmap(file, vma)
                           │         │              └── ext4_file_mmap(vma)  // 例: ext4
                           │         │                   └── vma->vm_ops = ext4_file_vm_ops
                           │         ├── ANON 共享:
                           │         │    └── shmem_zero_setup(vma)  // 创建 shmem 文件
                           │         ├── ANON 私有:
                           │         │    └── vma_set_anonymous(vma)  // vma->vm_ops = NULL
                           │         ├── vma_iter_store_new(vmi, vma)  // 插入红黑树
                           │         ├── vma_link_file(vma)  // 链接 address_space
                           │         └── khugepaged_enter_vma(vma)  // THP 检查
                           │
                           └── __mmap_complete(map, vma)  // mm/vma.c:2580
                                ├── unmap_region()  // 解除旧映射 + TLB 刷新
                                ├── vm_stat_account()  // 统计
                                └── perf_event_mmap(vma)  // 性能监控
```

### 13.2 缺页异常调用栈

```
用户态访问映射地址 → CPU Data Abort
  └── el0_sync 异常向量入口  // arch/arm64/kernel/entry.S
       └── do_mem_abort(addr, esr, regs)  // arch/arm64/kernel/fault.c
            └── do_page_fault(far, esr, regs)  // arch/arm64/mm/fault.c:570
                 ├── lock_vma_under_rcu()  // 快速路径
                 │    └── handle_mm_fault(vma, addr, flags|FAULT_FLAG_VMA_LOCK, regs)
                 │         └── __handle_mm_fault(vma, addr, flags)  // mm/memory.c:6355
                 │              └── handle_pte_fault(vmf)  // mm/memory.c:6273
                 │                   └── do_pte_missing(vmf)  // mm/memory.c:4472
                 │                        ├── 匿名 → do_anonymous_page(vmf)  // mm/memory.c:5217
                 │                        └── 文件 → do_fault(vmf)  // mm/memory.c:5903
                 │
                 └── lock_mm_and_find_vma()  // 慢速路径
                      └── handle_mm_fault(vma, addr, flags, regs)  // mm/memory.c:6589
                           └── __handle_mm_fault(vma, addr, flags)
                                ├── pgd_offset() → p4d_alloc() → pud_alloc() → pmd_alloc()
                                └── handle_pte_fault(vmf)
                                     ├── PTE 为空 → do_pte_missing(vmf)
                                     │    ├── 匿名:
                                     │    │    └── do_anonymous_page(vmf)
                                     │    │         ├── 读: pte_mkspecial(my_zero_pfn())  // 零页
                                     │    │         └── 写: alloc_anon_folio() + set_ptes()
                                     │    └── 文件:
                                     │         └── do_fault(vmf)
                                     │              ├── 读 → do_read_fault(vmf)
                                     │              │    └── __do_fault(vmf) → filemap_fault(vmf)
                                     │              │         ├── filemap_get_folio()  // 页缓存查找
                                     │              │         ├── do_sync_mmap_readahead()  // 预读
                                     │              │         ├── __filemap_get_folio(FGP_CREAT)  // 创建页缓存
                                     │              │         ├── filemap_read_folio() → read_folio()  // 磁盘 I/O
                                     │              │         └── 返回 VM_FAULT_LOCKED
                                     │              ├── 写私有 → do_cow_fault(vmf)
                                     │              │    ├── folio_prealloc()  // 分配 COW 页
                                     │              │    ├── __do_fault(vmf)  // 读取原始文件页
                                     │              │    ├── copy_mc_user_highpage()  // 复制内容
                                     │              │    └── finish_fault(vmf)  // 建立映射
                                     │              └── 写共享 → do_shared_fault(vmf)
                                     │                   ├── __do_fault(vmf)  // 读取文件页
                                     │                   ├── page_mkwrite()  // 通知文件系统
                                     │                   ├── finish_fault(vmf)  // 建立映射
                                     │                   └── fault_dirty_shared_page()  // 标记脏
                                     │
                                     ├── PTE 非 Present → do_swap_page(vmf)  // mm/memory.c:4706
                                     │    ├── swap_cache_get_folio()  // 交换缓存查找
                                     │    ├── swapin_readahead()  // 从交换设备预读
                                     │    └── 建立 PTE 映射
                                     │
                                     └── PTE Present 写保护 → do_wp_page(vmf)  // mm/memory.c:4149
                                          ├── 映射计数=1 → 直接标记可写
                                          └── 映射计数>1 → 复制页 + COW 映射
```

## 14. 写时复制（COW）详细流程

### 14.1 零页 → 写时复制

```
首次写入匿名映射的零页
    │
    ├── PTE 状态: present=1, writable=0, special=1, pfn=zero_pfn
    │
    ├── handle_pte_fault()
    │    ├── pte_present() = true
    │    ├── FAULT_FLAG_WRITE && !pte_write(entry) → do_wp_page()
    │
    ├── do_wp_page()
    │    ├── wp_page_reuse()  // 检查是否可以重用
    │    │    └── 零页是全局共享 → 必须复制
    │    │
    │    ├── alloc_anon_folio(vmf)  // 分配新匿名页
    │    │    └── 从伙伴系统获取物理页，清零
    │    │
    │    ├── folio_add_new_anon_rmap()  // 建立反向映射
    │    │    └── 记录页→VMA 映射关系（用于回收）
    │    │
    │    ├── folio_add_lru_vma()  // 加入 LRU 链表
    │    │
    │    └── set_ptes()  // 建立新 PTE
    │         PTE: present=1, writable=1, dirty=1, pfn=新页PFN
    │
    └── 写入成功
```

### 14.2 文件页 → 写时复制

```
首次写入 MAP_PRIVATE 文件映射（do_cow_fault）
    │
    ├── vmf_anon_prepare(vmf)  // 准备 anon_vma
    │
    ├── folio_prealloc()  // 分配 COW 目标页
    │    └── 从伙伴系统分配物理页
    │
    ├── __do_fault(vmf)
    │    └── filemap_fault(vmf)  // 获取文件页缓存页
    │         └── 返回页缓存页（共享的，只读）
    │
    ├── copy_mc_user_highpage(vmf->cow_page, vmf->page, ...)
    │    └── 从页缓存页复制内容到 COW 页
    │
    ├── __folio_mark_uptodate(cow_page)
    │
    ├── finish_fault(vmf)
    │    ├── set_ptes()  // 映射 COW 页（可写）
    │    └── folio_add_new_anon_rmap()  // 反向映射
    │
    └── 释放页缓存页引用
         └── unlock_page() + put_page()
```

## 15. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | len=0、flags 无效、prot 无效、offset 未页对齐 |
| `-ENOMEM` | 无法分配地址空间、超出 VMA 数量限制、OOM |
| `-EBADF` | 文件映射且 fd 无效 |
| `-EACCES` | 文件映射权限不匹配（如写保护文件要求 MAP_SHARED\|PROT_WRITE） |
| `-EPERM` | 文件 noexec 挂载要求 PROT_EXEC、MAP_LOCKED 无权限 |
| `-EEXIST` | MAP_FIXED_NOREPLACE 且地址已被占用 |
| `-EOVERFLOW` | 文件偏移溢出 |
| `-EAGAIN` | mlock 锁定限制 |
| `-ENODEV` | 不支持 mmap 的文件系统 |
| `-EOPNOTSUPP` | MAP_SHARED_VALIDATE 包含不支持标志 |

**缺页异常错误：**

| 信号 | si_code | 条件 |
|------|---------|------|
| `SIGSEGV` | `SEGV_MAPERR` | 地址不在任何 VMA 中 |
| `SIGSEGV` | `SEGV_ACCERR` | 权限不匹配（如写只读映射） |
| `SIGBUS` | `BUS_ADRERR` | 访问超出文件尾 |
| `SIGBUS` | `BUS_ADRERR` | 文件系统 I/O 错误 |
| `SIGKILL` | - | OOM 被杀死 |

## 16. 使用示例

### 16.1 文件映射

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main() {
    /* 示例1：匿名映射分配内存 */
    size_t len = 4096;
    char *anon = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) {
        perror("mmap anonymous");
        return 1;
    }
    // 首次写入触发 COW（从零页复制到新分配页）
    strcpy(anon, "Hello, mmap!");
    printf("Anonymous: %s\n", anon);
    munmap(anon, len);

    /* 示例2：文件映射（MAP_SHARED） */
    int fd = open("/tmp/test.txt", O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    ftruncate(fd, 4096);

    char *file_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    if (file_map == MAP_FAILED) {
        perror("mmap file");
        close(fd);
        return 1;
    }
    // 写入触发缺页 → 页缓存读取 → 脏页标价
    strcpy(file_map, "File-backed mapping!");
    // munmap 时脏页不会立即写回（由 pdflush 后台处理）
    munmap(file_map, 4096);
    close(fd);

    return 0;
}
```

### 16.2 观察缺页行为

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

int main()
{
    size_t len = 1024 * 1024;  // 1MB
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // 读取 /proc/self/status 查看内存使用
    // 初始：VmRSS 很小，因为没有缺页

    // 第一次访问：触发缺页（次缺页，使用零页）
    printf("p[0] = %d\n", p[0]);
    // VmRSS 不变（零页不增加 RSS）

    // 写入：触发 COW 缺页（次缺页，分配物理页）
    p[0] = 'A';
    // VmRSS 增加 4KB

    // 顺序写入 1MB：触发大量缺页
    memset(p, 0, len);
    // VmRSS 增加约 1MB（256 个 4KB 页）

    printf("Done. RSS should be ~1MB\n");
    printf("Check: grep VmRSS /proc/self/status\n");

    sleep(10);
    munmap(p, len);
    return 0;
}
```

## 17. 与相关系统调用的比较

| 特性 | mmap | brk | shmat | mmap(MAP_FIXED) |
|------|------|-----|-------|-----------------|
| 用途 | 通用映射 | 堆管理 | System V 共享内存 | 固定地址映射 |
| 灵活性 | 高 | 低 | 中 | 高 |
| 文件映射 | 支持 | 不支持 | 不支持 | 支持 |
| 进程间共享 | MAP_SHARED | 不支持 | 支持 | 支持 |
| 物理页分配 | 延迟（缺页时） | 延迟 | 立即 | 延迟 |

## 18. 关键实现细节

1. **两阶段架构**：`mmap` 仅创建 VMA（虚拟内存区域），物理页分配推迟到**缺页异常**（page fault）时。这是需求分页（demand paging）的核心。

2. **文件映射缺页**：通过 `filemap_fault()` 从页缓存读取。页缓存未命中时，通过 `mapping->a_ops->read_folio()` 发起块设备 I/O，经过块层（`submit_bio`）最终到达 NVMe/SCSI 等块设备驱动。

3. **匿名映射读缺页**：使用全局零页（`my_zero_pfn`），所有进程共享同一个物理零页，不消耗实际物理内存。

4. **匿名映射写缺页**：通过 `do_anonymous_page()` 分配物理页（`alloc_anon_folio`），建立反向映射（`folio_add_new_anon_rmap`），加入 LRU 链表。

5. **写时复制（COW）**：MAP_PRIVATE 映射的写入通过 `do_wp_page()` 或 `do_cow_fault()` 实现。文件页的 COW 需要从页缓存复制内容到新分配的匿名页。

6. **get_unmapped_area**：查找空闲地址区间，两个算法：`unmapped_area()`（自底向上）和 `unmapped_area_topdown()`（自顶向下），默认使用 topdown。

7. **vma_merge**：新映射尝试与相邻 VMA 合并，减少 VMA 数量。合并条件包括：权限相同、同一文件、连续地址等。

8. **文件系统 mmap 回调**：文件映射通过 `file->f_op->mmap()` 调用文件系统特定的 mmap 处理，设置 `vm_ops` 为文件系统特定的操作集（如 `ext4_file_vm_ops`）。

9. **页缓存**：文件映射的缺页通过页缓存（page cache）进行。页缓存是文件系统 I/O 的核心，使用基数树（xarray）管理缓存页。

10. **块设备 I/O 路径**：文件系统缺页通过 `mapping->a_ops->read_folio()` 发起块设备 I/O，经过文件系统映射层（extent 树）→ 块层（bio）→ 块设备驱动（NVMe/SCSI）。

## 19. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mmap.c` — mmap 系统调用入口和参数验证
- 内核源码：`mm/vma.c` — VMA 创建、管理、合并、查找
- 内核源码：`mm/memory.c` — 缺页处理核心
- 内核源码：`mm/filemap.c` — 文件映射缺页和页缓存
- 内核源码：`mm/rmap.c` — 反向映射
- 内核源码：`mm/swapfile.c` — 交换子系统
- 内核源码：`arch/arm64/mm/fault.c` — ARM64 缺页异常处理
- 内核源码：`include/linux/mm.h` — VMA 相关结构定义
- 内核源码：`include/uapi/linux/mman.h` — 用户态常量定义