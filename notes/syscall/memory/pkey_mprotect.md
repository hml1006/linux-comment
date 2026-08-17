# pkey_mprotect 系统调用分析

## 1. 概述

`pkey_mprotect` 系统调用类似于 `mprotect`，但在设置内存保护权限的同时，还将内存保护密钥（pkey）关联到指定的内存区域。这允许通过硬件密钥机制进行更细粒度的访问控制。

**内核源码位置：** `mm/mprotect.c`（需要 `CONFIG_ARCH_HAS_PKEYS`）

**原型：**

```c
SYSCALL_DEFINE4(pkey_mprotect, unsigned long, start, size_t, len,
                unsigned long, prot, int, pkey)
```

| 参数 | 描述 |
|------|------|
| `start` | 起始地址（必须页对齐） |
| `len` | 区域长度（字节） |
| `prot` | 保护标志（PROT_READ/WRITE/EXEC/NONE） |
| `pkey` | 要关联的 pkey 编号（-1 表示清除 pkey 关联） |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **pkey 与内存关联**：将已分配的 pkey 绑定到特定的内存区域
- **动态权限隔离**：结合 `pkey_set()` 在运行时动态切换访问权限
- **安全沙箱**：为不同模块的数据分配不同的 pkey

## 3. 函数调用链分析

```
pkey_mprotect(start, len, prot, pkey)                    // 系统调用入口
  └─ do_mprotect_pkey(start, len, prot, pkey)            // 核心处理
       └─ 与 mprotect 相同的调用链，但 pkey 参数不同
            ├─ 检查 pkey 是否已分配（pkey != -1 时）
            │    └─ mm_pkey_is_allocated(mm, pkey)
            ├─ 遍历 VMA 时：
            │    └─ new_vma_pkey = arch_override_mprotect_pkey(vma, prot, pkey)
            │         └─ 将 pkey 编码到 vm_flags 中
            ├─ mprotect_fixup() 修改 VMA 标志
            └─ change_protection() 更新页表
                 └─ 将 pkey 写入 PTE 的 pkey 字段
```

## 4. 关键数据结构

### PTE 中的 pkey 字段

```c
/* x86: PTE 的 62:59 位存储 pkey（4 位） */
#define _PAGE_PKEY_BIT0  59
#define _PAGE_PKEY_BIT1  60
#define _PAGE_PKEY_BIT2  61
#define _PAGE_PKEY_BIT3  62
#define _PAGE_PKEY_MASK  (_AC(0xf, UL) << 56)

/* ARM64: PTE 的 63:60 或 62:60 位存储 pkey */
```

### VMA 中的 pkey 编码

```c
/* vm_flags 中编码 pkey 的高位 */
#define VM_PKEY_BIT0  VM_HIGH_ARCH_BIT_0  /* pkey 位 0 */
#define VM_PKEY_BIT1  VM_HIGH_ARCH_BIT_1  /* pkey 位 1 */
#define VM_PKEY_BIT2  VM_HIGH_ARCH_BIT_2  /* pkey 位 2 */
#define VM_PKEY_BIT3  VM_HIGH_ARCH_BIT_3  /* pkey 位 3 */
#define VM_PKEY_BIT4  VM_HIGH_ARCH_BIT_4  /* pkey 位 4 (ARM64) */
```

## 5. 流程图

```
  用户态调用 pkey_mprotect(start, len, prot, pkey)
         │
         ▼
  ┌──────────────────────────────┐
  │  参数验证                    │
  │  └─ 同 mprotect              │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_lock_killable()  │  获取写锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  pkey 检查                   │
  │  pkey != -1?                 │
  │  └─ 检查 mm_pkey_is_        │
  │      allocated()             │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  for_each VMA in range:     │
  │  ┌──────────────────────┐    │
  │  │ arch_override_       │    │  将 pkey 编码到新标志中
  │  │ mprotect_pkey()      │    │
  │  │                      │    │
  │  │ 同 mprotect 的检查    │    │
  │  │ mprotect_fixup()     │    │
  │  │ change_protection()  │    │  更新 PTE 包含 pkey
  │  │ flush_tlb_range()    │    │
  │  └──────────────────────┘    │
  │  next VMA                    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_unlock()         │
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | pkey 未分配、start 未对齐、prot 无效 |
| `-ENOMEM` | 地址范围包含未映射区域 |
| `-EACCES` | 权限不允许 |
| `-EPERM` | 内存被 mseal 密封 |
| `-EINTR` | 获取 mmap 写锁时被信号中断 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    /* 分配一个 pkey，初始禁用写访问 */
    int pkey = pkey_alloc(0, PKEY_DISABLE_WRITE);
    if (pkey == -1) {
        perror("pkey_alloc");
        return 1;
    }

    /* 分配内存 */
    size_t len = 4096;
    char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        pkey_free(pkey);
        return 1;
    }

    /* 将 pkey 关联到内存区域 */
    if (pkey_mprotect(addr, len, PROT_READ | PROT_WRITE, pkey) == -1) {
        perror("pkey_mprotect");
        return 1;
    }

    /* 此时写入会被禁止（因为 pkey 初始有 PKEY_DISABLE_WRITE） */
    // addr[0] = 'A';  /* 可能触发 SIGSEGV */

    /* 临时允许写入 */
    if (pkey_set(pkey, 0) == -1) {
        perror("pkey_set");
    }
    addr[0] = 'A';  /* 现在可以写入 */
    printf("Written: %c\n", addr[0]);

    /* 恢复写保护 */
    pkey_set(pkey, PKEY_DISABLE_WRITE);

    pkey_free(pkey);
    munmap(addr, len);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | pkey_mprotect | mprotect | pkey_alloc | mmap |
|------|--------------|----------|------------|------|
| 功能 | 权限 + pkey | 仅权限 | 仅分配密钥 | 创建映射 |
| pkey 关联 | 是 | 否 | 不适用 | 否 |
| 硬件依赖 | 是 | 否 | 是 | 否 |

## 9. 关键实现细节

1. **与 mprotect 共享核心**：`pkey_mprotect` 与 `mprotect` 共享相同的 `do_mprotect_pkey()` 实现，区别仅在于 `pkey` 参数不同（`mprotect` 传入 `-1`）。

2. **pkey 编码到 vm_flags**：pkey 编号被编码到 VMA 的 `vm_flags` 高位中（`VM_PKEY_BIT0` 到 `VM_PKEY_BIT4`），然后在页表创建时从 `vm_flags` 提取并写入 PTE 的 pkey 字段。

3. **动态权限控制**：pkey 的强大之处在于，关联后可以通过 `pkey_set()` 函数（用户态）在不修改页表的情况下动态切换访问权限，这对性能至关重要。

4. **pkey 分配检查**：`pkey_mprotect` 会检查传入的 pkey 是否已通过 `pkey_alloc` 分配，未分配的 pkey 不能使用。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mprotect.c`
- 联机手册：`pkey_mprotect(2)`
- Intel MPK 和 ARM64 权限覆盖扩展文档