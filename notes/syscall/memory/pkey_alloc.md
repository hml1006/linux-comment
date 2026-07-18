# pkey_alloc 系统调用分析

## 1. 概述

`pkey_alloc` 系统调用用于分配一个内存保护密钥（Protection Key）。内存保护密钥是 x86 和 ARM64 等架构提供的一种硬件特性，允许对内存页面进行细粒度的访问控制，而无需修改页表。

**内核源码位置：** `mm/mprotect.c`（需要 `CONFIG_ARCH_HAS_PKEYS`）

**原型：**

```c
SYSCALL_DEFINE2(pkey_alloc, unsigned long, flags, unsigned long, init_val)
```

| 参数 | 描述 |
|------|------|
| `flags` | 保留标志（当前必须为 0） |
| `init_val` | 初始访问权限（`PKEY_DISABLE_ACCESS` 或 `PKEY_DISABLE_WRITE`） |

**返回值：**
- 成功返回 pkey 编号（非负整数）
- 失败返回负数错误码

## 2. 使用场景

- **细粒度内存保护**：为不同数据区域分配不同的 pkey，实现精细的访问控制
- **安全隔离**：防止非授权代码访问敏感数据
- **库的沙箱化**：为第三方库的内存分配不同的 pkey

## 3. 函数调用链分析

```
pkey_alloc(flags, init_val)                              // 系统调用入口
  ├─ 参数验证：flags 必须为 0
  ├─ init_val 检查（必须为 PKEY_ACCESS_MASK 内的值）
  ├─ mmap_write_lock(current->mm)                        // 获取写锁
  ├─ mm_pkey_alloc(current->mm)                          // 分配 pkey 号
  │    └─ 在 mm->pkey_bitmap 中查找空闲位
  ├─ 如果 pkey == -1 → -ENOSPC（无可用 pkey）
  ├─ arch_set_user_pkey_access(current, pkey, init_val)  // 架构特定设置
  │    └─ x86: wrmsr(MSR_IA32_PKEY_BITS_PERMANENT, ...)
  │    └─ ARM64: 设置 POR_EL0 寄存器
  ├─ 如果失败：mm_pkey_free(mm, pkey)                    // 回滚分配
  └─ mmap_write_unlock(current->mm)                      // 释放写锁
```

## 4. 关键数据结构

```c
struct mm_struct {
    // ...
    unsigned long pkey_bitmap;  /* 已分配的 pkey 位图 */
    // ...
};

/* pkey 权限值 */
#define PKEY_DISABLE_ACCESS  0x1  /* 禁用所有访问 */
#define PKEY_DISABLE_WRITE   0x2  /* 禁用写入 */
#define PKEY_ACCESS_MASK     (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE)
```

## 5. 流程图

```
  用户态调用 pkey_alloc(flags, init_val)
         │
         ▼
  ┌──────────────────────────────┐
  │  flags != 0?                 │
  │  init_val 超出范围?          │
  │  → 返回 -EINVAL              │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_lock()           │  获取写锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mm_pkey_alloc()             │  在位图中找空闲 pkey
  │  └─ pkey == -1?              │
  │       └─ 是 → -ENOSPC        │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  arch_set_user_pkey_access() │  设置硬件 pkey 权限
  │  └─ 失败 → 回滚释放 pkey    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_unlock()         │
  └─────────────┬────────────────┘
                ▼
           返回 pkey 编号
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | flags 非零 或 init_val 无效 |
| `-ENOSPC` | 没有可用的 pkey（所有 16 或 32 个 pkey 已分配） |
| `-EINTR` | 获取 mmap 写锁时被信号中断 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    int pkey = pkey_alloc(0, PKEY_DISABLE_WRITE);
    if (pkey == -1) {
        perror("pkey_alloc");
        return 1;
    }
    printf("Allocated pkey: %d\n", pkey);

    /* 分配内存并关联 pkey */
    size_t len = 4096;
    void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        pkey_free(pkey);
        return 1;
    }

    /* 将 pkey 关联到内存区域 */
    if (pkey_mprotect(addr, len, PROT_READ | PROT_WRITE, pkey) == -1) {
        perror("pkey_mprotect");
    }

    /* 通过 pkey_set() 可以动态修改此区域的权限 */
    /* 写入被 PKEY_DISABLE_WRITE 禁止，但可以临时用 pkey_set(pkey, 0) 允许 */

    pkey_free(pkey);
    munmap(addr, len);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | pkey_alloc | pkey_free | pkey_mprotect | mprotect |
|------|------------|-----------|---------------|----------|
| 功能 | 分配密钥 | 释放密钥 | 映射 + 密钥 | 仅修改权限 |
| 硬件依赖 | 是 | 是 | 是 | 否 |
| 动态权限切换 | 支持 | 不适用 | 支持 | 不适用 |
| 架构支持 | x86, ARM64 | x86, ARM64 | x86, ARM64 | 所有架构 |

## 9. 关键实现细节

1. **pkey 数量限制**：x86 上最多 16 个 pkey（4 位），ARM64 上最多 32 个（5 位）。`mm->pkey_bitmap` 用于跟踪哪些 pkey 已被分配。

2. **架构特定操作**：
   - x86：通过 `wrmsr` 指令设置 `IA32_PKEY_BITS_PERMANENT` MSR 寄存器
   - ARM64：通过 `POR_EL0` 寄存器控制权限覆盖

3. **init_val 权限**：`pkey_alloc` 时指定的初始权限是永久性的（直到 pkey 被释放），后续通过 `pkey_set()` 可以动态调整当前线程的 pkey 权限。

4. **回滚机制**：如果 `arch_set_user_pkey_access()` 失败，内核会回滚 pkey 分配（`mm_pkey_free`），避免泄漏 pkey 号。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mprotect.c`
- 联机手册：`pkey_alloc(2)`
- Intel MPK (Memory Protection Keys) 文档