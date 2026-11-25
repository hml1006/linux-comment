# hugetlbfs — 大页文件系统

## 1. 概述与实现机制

hugetlbfs 是大页（HugeTLB）机制的文件系统接口，允许进程通过 `mmap` 映射大页内存。大页降低 TLB miss 概率，显著提升大量内存访问的性能。

### 大页尺寸

- **x86**：2MB（默认），1GB（可选）
- **ARM64**：64KB，2MB，32MB，1GB（取决于配置）
- **PowerPC**：64KB，2MB，16MB，1GB

### 核心特性

- **TLB 覆盖范围大**：一个 2MB 大页替换 512 个 4KB 页表项
- **预分配**：大页需提前预留（`/proc/sys/vm/nr_hugepages`）
- **mmap 访问**：大页文件通常通过 mmap 映射访问
- **不支持 read/write**：hugetlbfs 的 file_operations 不支持 read_iter/write_iter

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  fd = open("/dev/hugepages/myfile", ...)                    │
│  addr = mmap(NULL, 2MB, PROT_RW, MAP_SHARED, fd, 0)        │
│  // 使用 addr 访问大页内存                                   │
└────────────────────────┬─────────────────────────────────────┘
                         │ 系统调用
                         ▼
┌──────────────────────────────────────────────────────────────┐
│              hugetlbfs 层 (fs/hugetlbfs/inode.c)            │
│  hugetlbfs_file_mmap() → 设置 VMA 为大页映射                │
│  hugetlbfs_fallocate() → 预分配大页                         │
│  hugetlbfs_read_iter / write_iter → 不支持 / 有限支持       │
│  hugetlbfs_file_operations → mmap 为主                      │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│               大页核心 (mm/hugetlb.c)                        │
│  hugetlb_fault() → 大页缺页处理                             │
│  hugetlb_no_page() → 分配大页页面                           │
│  alloc_huge_page() → 从预留池分配                           │
│  huge_pte_alloc() → 分配大页页表                            │
│  hugetlb_vma_op_lock() → VMA 操作锁                        │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 hstate — 大页状态

```c
// 文件: include/linux/hugetlb.h
struct hstate {
    struct mutex resize_lock;        // 调整大小锁
    int next_nid_to_alloc;           // 下一个分配 NUMA 节点
    int next_nid_to_free;            // 下一个释放 NUMA 节点
    unsigned int order;              // 大页阶数 (2MB = 2^9 = 512 页)
    unsigned int demote_order;       // 降级阶数
    unsigned long free_huge_pages;   // 空闲大页数
    unsigned long resv_huge_pages;   // 预留大页数
    unsigned long surplus_huge_pages; // 盈余大页数
    unsigned long nr_huge_pages;     // 总大页数
    unsigned long nr_overcommit_huge_pages; // 超配大页数
    struct list_head hugepage_freelists[MAX_NUMNODES]; // 每 NUMA 节点空闲列表
    unsigned int max_huge_pages;     // 最大大页数
    unsigned int nr_huge_pages_node[MAX_NUMNODES]; // 每节点大页数
    unsigned int free_huge_pages_node[MAX_NUMNODES]; // 每节点空闲大页数
    unsigned int surplus_huge_pages_node[MAX_NUMNODES]; // 每节点盈余大页数
    char name[HSTATE_NAME_LEN];      // 大页名称 (如 "hugepages-2048kB")
    unsigned int *nr_huge_pages_node_pending;  // 待处理大页数
    unsigned int *free_huge_pages_node_pending; // 待处理空闲大页数
};
```

### 2.2 hugetlbfs_inode_info — hugetlbfs inode 信息

```c
// 文件: fs/hugetlbfs/inode.c
struct hugetlbfs_inode_info {
    struct shared_policy policy;     // 共享内存策略
    struct inode vfs_inode;          // 嵌入的 VFS inode
};
```

### 2.3 hugetlbfs_file_operations — 大页文件操作

```c
// 文件: fs/hugetlbfs/inode.c
static const struct file_operations hugetlbfs_file_operations = {
    .read_iter      = hugetlbfs_read_iter,      // 有限支持
    .write_iter     = hugetlbfs_write_iter,     // 有限支持
    .mmap           = hugetlbfs_file_mmap,      // 核心操作
    .fsync          = noop_fsync,               // 同步
    .get_unmapped_area = hugetlb_get_unmapped_area, // 获取未映射区域
    .fallocate      = hugetlbfs_fallocate,      // 预分配
    .open           = hugetlbfs_file_open,      // 打开
    .release        = hugetlbfs_file_release,   // 释放
};
```

### 2.4 大页页表项

```c
// 文件: include/linux/hugetlb.h
// 大页使用 PMD (2MB) 或 PUD (1GB) 级别页表项
// 不经过 PT 级别页表，跳过 PTE
// 大页 PTE 结构:
// ┌─────────────────────────────────────────────────────┐
// │  PFN (物理页帧号)          │  Flags                 │
// │  (大页物理地址)             │  (P bit, RW, UXN等)  │
// └─────────────────────────────────────────────────────┘
// 大页页表项在 PMD/PUD 级别，不占用 PTE 级别
```

---

## 3. API 与使用方法

### 3.1 用户空间使用

```bash
# 1. 预留大页
echo 20 > /proc/sys/vm/nr_hugepages            # 预留 20 个 2MB 大页 (x86)
echo 4 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages  # 预留 4 个 1GB 大页

# 2. 查看大页状态
cat /proc/meminfo | grep Huge
# HugePages_Total:    20
# HugePages_Free:     20
# HugePages_Rsvd:     0
# HugePages_Surp:     0
# Hugepagesize:       2048 kB

# 3. 挂载 hugetlbfs
mount -t hugetlbfs none /dev/hugepages
# 或指定大页大小
mount -t hugetlbfs -o pagesize=2M none /dev/hugepages

# 4. 使用大页
cat > /tmp/hugepage_test.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main()
{
    int fd;
    void *addr;

    fd = open("/dev/hugepages/myfile", O_CREAT | O_RDWR, 0755);
    if (fd < 0) { perror("open"); return 1; }

    // 映射 2MB 大页内存
    addr = mmap(NULL, 2*1024*1024, PROT_READ|PROT_WRITE,
                MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) { perror("mmap"); return 1; }

    // 使用大页内存 (写入触发缺页，分配大页)
    memset(addr, 0xAA, 2*1024*1024);
    printf("Huge page test passed!\n");

    munmap(addr, 2*1024*1024);
    close(fd);
    unlink("/dev/hugepages/myfile");
    return 0;
}
EOF
gcc -o /tmp/hugepage_test /tmp/hugepage_test.c
/tmp/hugepage_test
```

### 3.2 内核内部 API

```c
#include <linux/hugetlb.h>

// 大页分配
struct folio *alloc_huge_page(struct vm_area_struct *vma,
                              unsigned long addr, int avoid_reserve);
struct folio *alloc_huge_page_nodemask(struct hstate *h, ...);
struct folio *alloc_migrate_huge_page(struct hstate *h, gfp_t gfp_mask, ...);

// 大页释放
void free_huge_page(struct folio *folio);
void free_huge_folio(struct folio *folio);

// 大页缺页
vm_fault_t hugetlb_fault(struct mm_struct *mm, struct vm_area_struct *vma,
                         unsigned long address, unsigned int flags);
vm_fault_t hugetlb_no_page(struct mm_struct *mm, struct vm_area_struct *vma,
                           struct address_space *mapping, ...);

// 大页页表
pte_t *huge_pte_alloc(struct mm_struct *mm, struct vm_area_struct *vma,
                      unsigned long addr, unsigned long sz);
pte_t *huge_pte_offset(struct mm_struct *mm, unsigned long addr,
                       unsigned long sz);

// hugetlbfs 操作
int hugetlbfs_file_mmap(struct file *file, struct vm_area_struct *vma);
long hugetlbfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len);
```

---

## 4. 函数调用栈

### 4.1 大页 mmap 映射

```
mmap(NULL, 2MB, PROT_RW, MAP_SHARED, fd, 0)
  ↓ sys_mmap_pgoff() → do_mmap()
    → call_mmap(file, vma)                          // 调用文件系统 mmap
      → hugetlbfs_file_mmap(file, vma)              // hugetlbfs mmap
        → vma->vm_flags |= VM_HUGETLB | VM_MTE_ALLOWED  // 设置大页标志
        → vma->vm_ops = &hugetlb_vm_ops            // 设置 VMA 操作
        → hugetlb_vm_op_open(vma)                   // 打开 VMA
        → hugetlb_reserve_pages(inode, ...)         // 预留大页
          → vma_resv_map_alloc(vma)                 // 分配预留映射
          → hugetlb_reserve_pages_file()            // 文件级别预留
          → 从预留池中标记预留
        → return 0
```

### 4.2 大页缺页处理

```
访问大页内存 (首次访问触发缺页)
  ↓ do_kernel_fault → do_page_fault → handle_mm_fault()
    → hugetlb_fault(mm, vma, addr, flags)           // 大页缺页入口
      → huge_pte_offset(mm, addr, sz)               // 查找大页 PTE
      → pte = huge_ptep_get(ptep)                   // 获取 PTE
      → if (!huge_pte_none(pte))                     // 已有映射
          → 处理写时拷贝等
      → hugetlb_no_page(mm, vma, mapping, idx, addr) // 分配新页
        → alloc_huge_page(vma, addr, avoid_reserve)  // 从预留池分配
          → dequeue_huge_page_vma(h, vma, addr, ...) // 出队大页
            → dequeue_huge_page_nodemask(h, ...)     // 从 NUMA 节点出队
              → list_move(&page->lru, &h->hugepage_activelist)
              → SetPageHugeTLB(page)
              → SetPageHugeTemporary(page) (可选)
          → SetHPageRestoreReserve(page)             // 标记恢复预留
        → set_huge_pte_at(mm, addr, ptep, ...)       // 设置大页 PTE
          → 设置 PMD/PUD 级别页表项
          → 设置物理地址和权限位
        → hugetlb_count_add(pages, mm)               // 更新统计
```

### 4.3 大页 fallocate 预分配

```
fallocate(fd, 0, 0, 2MB)
  ↓ sys_fallocate() → vfs_fallocate() → hugetlbfs_fallocate()
    → hugetlbfs_fallocate(file, mode, offset, len)
      → inode_lock(inode)                            // 加锁
      → for (index = start; index < end; index++) {
          // 为每个大页预分配
          alloc_huge_page(inode->i_mapping, vma, addr, ...);
          // 添加到大页缓存
          huge_add_to_folio_cache(folio, mapping, index);
          // 设置页表映射
          set_huge_pte_at(mm, addr, ptep, ...);
        }
      → inode_unlock(inode)                          // 解锁
```

---

## 5. 流程图

### 5.1 大页映射流程

```
用户进程
    │
    ├── open("/dev/hugepages/myfile", O_CREAT|O_RDWR)
    │     → hugetlbfs_file_open()
    │
    ├── mmap(NULL, 2MB, PROT_RW, MAP_SHARED, fd, 0)
    │     → hugetlbfs_file_mmap()
    │       → vma->vm_flags |= VM_HUGETLB
    │       → hugetlb_reserve_pages()  # 预留大页
    │
    └── memset(addr, 0xAA, 2MB)  ← 首次访问触发缺页
          │
          ▼
    hugetlb_fault()
          │
          ├── huge_pte_offset()  # 查找大页 PTE
          │
          ├── huge_pte_none()?   # PTE 为空?
          │     │
          │     └── 是 → hugetlb_no_page()
          │           │
          │           ├── alloc_huge_page()  # 从预留池分配
          │           │     │
          │           │     ├── dequeue_huge_page_vma()
          │           │     │     → 从空闲列表取出大页
          │           │     │
          │           │     └── 返回 folio (大页物理内存)
          │           │
          │           ├── huge_add_to_page_cache()
          │           │     → 添加到页缓存 (address_space)
          │           │
          │           └── set_huge_pte_at()  # 设置 PMD 页表项
          │                 → 映射大页物理地址
          │
          └── 返回 0 → 缺页处理完成
                │
                ▼
          memset 正常执行，数据写入大页
```

### 5.2 大页与普通页对比

```
普通 4KB 页:
┌─────────────────────────────────────────────────────┐
│  虚拟地址空间                         物理内存       │
│                                                     │
│  ┌──────────────────┐                 ┌─────┐       │
│  │ 4KB 区域 (PTE)   │───→ 页表 ───→  │ 页 0 │       │
│  ├──────────────────┤                 ├─────┤       │
│  │ 4KB 区域 (PTE)   │───→ 页表 ───→  │ 页 1 │       │
│  ├──────────────────┤                 ├─────┤       │
│  │ ... (512 项)     │                 │ ... │       │
│  ├──────────────────┤                 ├─────┤       │
│  │ 4KB 区域 (PTE)   │───→ 页表 ───→  │ 页511│       │
│  └──────────────────┘                 └─────┘       │
│  需要 512 个页表项和 512 次 TLB 填充                  │
│                                                     │
│  2MB 大页:                                           │
│  ┌──────────────────┐                 ┌─────────────┐│
│  │ 2MB 区域 (PMD)   │───→ 页表 ───→  │ 大页 (2MB)  ││
│  └──────────────────┘                 └─────────────┘│
│  只需要 1 个页表项和 1 次 TLB 填充                    │
└─────────────────────────────────────────────────────┘
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **数据库** | 减少 TLB miss，提升大量内存访问性能 | Oracle、PostgreSQL 配置大页 |
| **DPDK** | 数据平面开发套件，需要大页进行包处理 | 网卡驱动使用大页存储 DMA 缓冲区 |
| **KVM 虚拟化** | Guest 虚拟机使用大页内存 | `-mem-path /dev/hugepages/` |
| **HPC 高性能计算** | 科学计算工作负载 | 大规模矩阵运算，减少 TLB miss |
| **内存密集型应用** | 内存数据库、缓存系统 | Redis、Memcached 大页支持 |
| **JVM 大页** | Java 虚拟机使用大页堆 | `-XX:+UseLargePages` |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `mm/hugetlb.c` | 大页核心实现（分配、释放、缺页、页表管理） |
| `fs/hugetlbfs/inode.c` | hugetlbfs 文件系统实现（mmap、fallocate、文件操作） |
| `include/linux/hugetlb.h` | 大页数据结构定义和 API 声明 |
| `arch/x86/mm/hugetlbpage.c` | x86 架构大页页表操作 |
| `arch/arm64/mm/hugetlbpage.c` | ARM64 架构大页页表操作 |