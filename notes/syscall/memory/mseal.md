# mseal 系统调用分析

## 1. 概述

`mseal` 系统调用用于密封（seal）内存区域，一旦密封，该区域的内存布局和权限不能被后续的系统调用修改。这是一个安全特性，用于防止攻击者修改关键内存区域的属性。

**内核源码位置：** `mm/mseal.c`

**原型：**

```c
SYSCALL_DEFINE3(mseal, unsigned long, start, size_t, len, unsigned long, flags)
```

| 参数 | 描述 |
|------|------|
| `start` | 起始地址（必须页对齐） |
| `len` | 区域长度（字节） |
| `flags` | 保留标志（当前必须为 0） |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **安全强化**：在初始化后密封关键内存区域，防止攻击者修改
- **只读后密封**：设置敏感数据为只读后密封，确保权限不被回滚
- **缓解漏洞利用**：防止通过 `mprotect`、`munmap`、`mmap(MAP_FIXED)` 等修改密封区域
- **C/R 安全**：在检查点/恢复场景中保护内存布局

## 3. 函数调用链分析

```
mseal(start, len, flags)                                // 系统调用入口
  └─ do_mseal(start, len, flags)                        // 核心处理
       ├─ flags 检查（必须为 0）
       ├─ untagged_addr(start)
       ├─ PAGE_ALIGNED(start) 检查                      // 必须页对齐
       ├─ PAGE_ALIGN(len) + 溢出检查
       ├─ end == start → 返回 0
       ├─ mmap_write_lock_killable(mm)                  // 获取写锁
       ├─ range_contains_unmapped(mm, start, end)       // 检查是否有空洞
       │    └─ for_each_vma_range(vmi, vma, end)
       │         ├─ 检查 VMA 之间的间隙
       │         └─ 检查起始和结束地址是否在 VMA 内
       └─ mseal_apply(mm, start, end)                   // 应用密封
            └─ for_each_vma_range(vmi, vma, end)
                 └─ 如果未密封：
                      └─ vma_modify_flags(&vmi, prev, vma,
                             curr_start, curr_end, &vm_flags)
                           └─ vm_flags_set(vma, VM_SEALED)
       └─ mmap_write_unlock(mm)                         // 释放写锁
```

## 4. 关键数据结构

### VMA 密封标志

```c
/* VMA 标志位中与密封相关的位 */
#define VM_SEALED   BIT(63 - MAPLE_SEAL_COUNT_SHIFT)  /* VMA 被密封 */
```

### 密封操作拦截

`mseal` 通过以下方式拦截对密封 VMA 的操作：

1. `mmap(MAP_FIXED)` → `do_vmi_munmap()` 中检查 `VM_SEALED`
2. `munmap` → `do_vmi_munmap()` 中检查 `VM_SEALED`
3. `mprotect` / `pkey_mprotect` → `mprotect_fixup()` 中检查 `VM_SEALED`
4. `mremap` → `check_prep_vma()` 中检查 `VM_SEALED`
5. `madvise` 的修改行为 → `can_madvise_modify()` 中检查 `VM_SEALED`

## 5. 流程图

```
  用户态调用 mseal(start, len, flags)
         │
         ▼
  ┌──────────────────────────────┐
  │  参数验证                    │
  │  ├─ flags == 0               │
  │  ├─ start 页对齐             │
  │  └─ len 页对齐 + 溢出检查    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_lock_killable()  │  获取写锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  range_contains_unmapped()   │  检查是否包含未映射区域
  │  ┌──────────────────────┐    │
  │  │ for_each VMA:        │    │
  │  │ ├─ vma->vm_start >   │    │
  │  │ │   prev_end → 有空洞│    │
  │  │ │   → 返回 -ENOMEM   │    │
  │  │ └─ prev_end = vma->  │    │
  │  │      vm_end          │    │
  │  │ 最终检查 prev_end    │    │
  │  │ < end → 有空洞       │    │
  │  └──────────────────────┘    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mseal_apply()               │  应用密封
  │  ┌──────────────────────┐    │
  │  │ for_each VMA:        │    │
  │  │ ┌────────────────┐   │    │
  │  │ │ 如果未密封:     │   │    │
  │  │ │ vma->vm_flags  │   │    │
  │  │ │ |= VM_SEALED   │   │    │
  │  │ │ 通过 vma_      │   │    │
  │  │ │ modify_flags() │   │    │
  │  │ └────────────────┘   │    │
  │  │ next VMA             │    │
  │  └──────────────────────┘    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_unlock()         │  释放写锁
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | flags 非零、start 未页对齐、len 向上对齐后溢出 |
| `-ENOMEM` | 地址范围包含未映射区域（空洞） |
| `-EINTR` | 获取 mmap 写锁时被信号中断 |
| `-EPERM` | 32 位架构不支持密封（仅限 64 位） |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

int main() {
    size_t len = 4096;
    char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    strcpy(addr, "Important data");

    /* 密封内存区域 */
    if (syscall(__NR_mseal, addr, len, 0) == -1) {
        perror("mseal");
        return 1;
    }

    /* 以下操作都会失败（返回 -EPERM）： */
    // mprotect(addr, len, PROT_READ);       /* 修改权限 */
    // munmap(addr, len);                     /* 解除映射 */
    // mmap(addr, len, PROT_READ,             /* MAP_FIXED 覆盖 */
    //      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    /* 但读取仍然可以 */
    printf("Data: %s\n", addr);

    /* 无法取消密封，直到进程退出 */
    munmap(addr, len);  /* 这也会失败 */
    return 0;
}
```

## 8. 被拦截的操作

| 系统调用 | 被拦截的操作 | 返回错误 |
|---------|-------------|---------|
| `munmap` | 解除密封 VMA 的映射 | `-EPERM` |
| `mmap(MAP_FIXED)` | 覆盖密封 VMA | `-EPERM` |
| `mprotect` | 修改密封 VMA 权限 | `-EPERM` |
| `pkey_mprotect` | 修改密封 VMA 权限 | `-EPERM` |
| `mremap` | 移动/扩展/缩小密封 VMA | `-EPERM` |
| `madvise` | 破坏性行为（DONTNEED 等） | `-EPERM` |

## 9. 关键实现细节

1. **不可逆操作**：`mseal` 是不可逆的，一旦设置 `VM_SEALED` 标志，没有任何系统调用可以将其清除。没有对应的 `unseal` 操作。

2. **空洞检查**：`mseal` 要求整个 `[start, end)` 范围完全由已映射的 VMA 覆盖，不允许有未映射的空洞。这是为了防止在空洞中创建新映射来绕过密封。

3. **多次密封**：对已密封的 VMA 再次调用 `mseal` 是无操作的（no-op），不会返回错误。

4. **32 位限制**：在 32 位架构上，`mseal` 返回 `-EPERM`，因为位宽限制无法容纳 `VM_SEALED` 标志位。

5. **实现方式**：通过在关键系统调用的 VMA 修改路径中添加 `VM_SEALED` 标志检查来实现。修改 VMA 的函数（如 `vma_modify_flags()`、`do_vmi_munmap()`）会检查目标 VMA 是否被密封。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mseal.c`
- 内核源码：`include/linux/mm.h`（VM_SEALED 定义）
- 提交信息：`mseal` 由 Google 的 Jeff Xu 在 2023-2024 年贡献