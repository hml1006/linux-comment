# mprotect 系统调用分析

## 1. 概述

`mprotect` 系统调用用于修改进程地址空间中指定内存区域的访问权限。它可以设置页面为可读、可写、可执行或不可访问。

**内核源码位置：** `mm/mprotect.c`

**原型：**

```c
SYSCALL_DEFINE3(mprotect, unsigned long, start, size_t, len,
                unsigned long, prot)
```

| 参数 | 描述 |
|------|------|
| `start` | 起始地址（必须页对齐） |
| `len` | 区域长度（字节） |
| `prot` | 新的保护标志（PROT_NONE/READ/WRITE/EXEC 的组合） |

**prot 值：**

| 标志 | 描述 |
|------|------|
| `PROT_NONE` | 无访问权限（访问会触发 SIGSEGV） |
| `PROT_READ` | 可读 |
| `PROT_WRITE` | 可写 |
| `PROT_EXEC` | 可执行 |
| `PROT_GROWSDOWN` | 向下增长（栈区域） |
| `PROT_GROWSUP` | 向上增长 |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **设置只读内存**：保护关键数据结构不被意外修改
- **实现 JIT 编译器**：先分配 RW 内存写入代码，再改为 RX 执行
- **栈保护**：在栈附近设置 guard page
- **动态权限调整**：在运行时修改内存映射的保护属性

## 3. 函数调用链分析

```
mprotect(start, len, prot)                              // 系统调用入口
  └─ do_mprotect_pkey(start, len, prot, pkey=-1)        // 核心处理
       ├─ untagged_addr(start)                          // 去除地址标签
       ├─ 参数验证：
       │    ├─ prot 验证（PROT_GROWSDOWN|PROT_GROWSUP 冲突）
       │    ├─ start 页对齐检查
       │    ├─ len 页对齐与溢出检查
       │    └─ arch_validate_prot(prot)                 // 架构特定权限检查
       ├─ mmap_write_lock_killable(mm)                  // 获取写锁
       ├─ 如果 pkey != -1，检查 pkey 是否已分配
       ├─ vma_iter_init + vma_find                      // 查找起始 VMA
       ├─ 处理 PROT_GROWSDOWN / PROT_GROWSUP
       ├─ tlb_gather_mmu(&tlb, mm)                     // 初始化 TLB 收集
       └─ for_each_vma_range(vmi, vma, end)             // 遍历受影响 VMA
            ├─ 计算新 vm_flags：
            │    ├─ calc_vm_prot_bits(prot, new_vma_pkey)
            │    └─ 检查新标志的合法性（VM_MAY 标志验证）
            ├─ map_deny_write_exec() 检查               // W^X 检查
            ├─ arch_validate_flags()                     // 架构标志验证
            ├─ security_file_mprotect()                  // LSM 安全检查
            ├─ vma->vm_ops->mprotect()                   // 文件系统特定 mprotect
            └─ mprotect_fixup(&vmi, &tlb, vma, ...)      // 核心修正
                 ├─ 修改 vma->vm_flags
                 ├─ vma_set_page_prot(vma)               // 更新页表权限
                 └─ change_protection_range()            // 批量修改 PTE
                      └─ flush_tlb_range()               // 刷新 TLB
       └─ tlb_finish_mmu(&tlb)                          // 完成 TLB 刷新
       └─ mmap_write_unlock(mm)                         // 释放写锁
```

## 4. 关键数据结构

### 保护标志映射表

```c
/* 将 PROT_READ/WRITE/EXEC 组合映射为页表权限 */
pgprot_t protection_map[16];
/* 索引计算：PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4
 * 例: PROT_READ|PROT_WRITE = 3 → 索引 3
 */

/* 权限标志位掩码 */
#define VM_ACCESS_FLAGS (VM_READ | VM_WRITE | VM_EXEC)
#define VM_FLAGS_CLEAR  (VM_READ | VM_WRITE | VM_EXEC | \
                         VM_SHARED | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC)
```

### `mprotect_fixup` 核心

```c
static int mprotect_fixup(struct vma_iterator *vmi, struct mmu_gather *tlb,
                          struct vm_area_struct *vma,
                          struct vm_area_struct **prev,
                          unsigned long start, unsigned long end,
                          unsigned long newflags)
{
    // 处理 VMA 分割（如果范围不覆盖整个 VMA）
    // 修改 vm_flags 和 vm_page_prot
    // 调用 change_protection() 批量修改页表
    // 如果是 VM_LOCKED 变化，调用 mlock_fixup()
}
```

## 5. 流程图

```
  用户态调用 mprotect(start, len, prot)
         │
         ▼
  ┌──────────────────────────────┐
  │  参数验证                    │
  │  ├─ prot 合法性              │
  │  ├─ start 页对齐             │
  │  └─ len 溢出检查             │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_lock_killable()  │  获取写锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  查找起始 VMA                │
  │  vma_iter_init + vma_find   │
  │  ├─ PROT_GROWSDOWN 处理      │
  │  └─ PROT_GROWSUP 处理        │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  tlb_gather_mmu()            │  初始化 TLB 收集器
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  for_each VMA in range:      │
  │  ┌──────────────────────┐    │
  │  │ 计算新 vm_flags      │    │
  │  │ check:               │    │
  │  │ ├─ VM_MAY 权限检查   │    │
  │  │ ├─ W^X 检查          │    │  不能同时有写和执行
  │  │ ├─ 架构标志验证      │    │
  │  │ └─ LSM 安全检查      │    │
  │  │                      │    │
  │  │ 如果通过:             │    │
  │  │ ├─ vma->vm_ops->     │    │  文件系统回调
  │  │ │   mprotect()       │    │
  │  │ └─ mprotect_fixup()  │    │  修改 VMA 和页表
  │  │      ├─ VMA 分割     │    │
  │  │      ├─ change_prot  │    │  批量修改 PTE
  │  │      │  ection()     │    │
  │  │      └─ flush_tlb    │    │  TLB 刷新
  │  │          range()     │    │
  │  └──────────────────────┘    │
  │  next VMA                    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  tlb_finish_mmu()            │  完成 TLB 刷新
  │  mmap_write_unlock()         │  释放写锁
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | start 未页对齐、prot 无效、PROT_GROWSDOWN 和 PROT_GROWSUP 同时设置 |
| `-ENOMEM` | 地址范围包含未映射区域 |
| `-EACCES` | 新权限请求了 VM_MAY 未允许的权限（如对只读映射设置 PROT_WRITE） |
| `-EPERM` | 内存已被 mseal 密封、map_deny_write_exec 阻止 |
| `-EINTR` | 获取 mmap 写锁时被信号中断 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

int main() {
    /* 分配 RW 内存 */
    size_t len = 4096;
    char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* 写入数据 */
    strcpy(addr, "Sensitive data");

    /* 改为只读，防止修改 */
    if (mprotect(addr, len, PROT_READ) == -1) {
        perror("mprotect readonly");
        return 1;
    }

    // addr[0] = 'X';  /* 这会触发 SIGSEGV */

    /* 改为可读写 */
    if (mprotect(addr, len, PROT_READ | PROT_WRITE) == -1) {
        perror("mprotect rw");
        return 1;
    }

    /* 模拟 JIT：写代码然后改为可执行 */
    char *jit_code = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* 写入机器码... */
    /* 改为可读可执行 */
    mprotect(jit_code, 4096, PROT_READ | PROT_EXEC);

    munmap(addr, len);
    munmap(jit_code, 4096);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | mprotect | pkey_mprotect | mmap(MAP_FIXED) |
|------|----------|---------------|-----------------|
| 功能 | 修改权限 | 修改权限 + pkey | 创建新映射覆盖 |
| 是否需要 VMA | 需要 | 需要 | 不需要 |
| 是否改变物理内存 | 否 | 否 | 是 |
| 密钥保护 | 不支持 | 支持 | 不支持 |
| 性能 | 快（仅修改页表） | 快 | 慢（需创建 VMA） |

## 9. 关键实现细节

1. **VM_MAY 检查**：`mprotect` 不能设置 `VM_MAYREAD/MAYWRITE/MAYEXEC` 未允许的权限。例如，使用 `mmap(PROT_READ)` 映射的区域，`mprotect` 不能设置 `PROT_EXEC`，因为 `VM_MAYEXEC` 未设置。

2. **W^X 保护**：`map_deny_write_exec()` 检查是否同时设置了写和执行权限。如果内核配置了 `ARCH_HAS_DEVMEM_IS_ALLOWED` 且受保护，则 `PROT_WRITE | PROT_EXEC` 会被拒绝。

3. **change_protection**：批量修改 PTE 权限，并调用 `flush_tlb_range()` 刷新 TLB，确保修改对所有 CPU 核心可见。

4. **VMA 分割**：如果 `mprotect` 范围只覆盖 VMA 的一部分，内核会先分割 VMA（`vma_modify_flags()`），然后修改其中一部分的权限。

5. **mseal 交互**：被密封（mseal）的内存区域不能通过 `mprotect` 修改权限，会返回 `-EPERM`。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mprotect.c`
- 内核源码：`include/linux/mm.h`（VMA 标志定义）