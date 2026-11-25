# mlock2 系统调用分析

> 所属分类：内存管理 | [ARM64 系统调用表](../arm64-syscall-table.md#7-内存管理)

## 1. 概述

`mlock2` 是 `mlock` 的扩展版本，多了一个 `flags` 参数。当前唯一支持的标志是 `MLOCK_ONFAULT`：只给 VMA 打上 `VM_LOCKONFAULT` 标记，**不立即预填充页表**；页面在首次缺页时才被锁定，之后保持常驻。

这解决了 `mlock` 的两难：立即锁整段内存会造成启动延迟和内存浪费，尤其大缓冲区只用到一部分时；`MLOCK_ONFAULT` 把成本推迟到真正访问时。

ARM64 系统调用号：`__NR_mlock2 = 284`。

## 2. 函数原型与参数

```c
#define _GNU_SOURCE
#include <sys/mman.h>

int mlock2(const void *addr, size_t len, unsigned int flags);
```

内核入口（`mm/mlock.c:670`）：

```c
SYSCALL_DEFINE3(mlock2, unsigned long, start, size_t, len, int, flags)
{
    vm_flags_t vm_flags = VM_LOCKED;

    if (flags & ~MLOCK_ONFAULT)
        return -EINVAL;

    if (flags & MLOCK_ONFAULT)
        vm_flags |= VM_LOCKONFAULT;

    return do_mlock(start, len, vm_flags);
}
```

| 参数 | 说明 |
|------|------|
| `start` | 起始地址，内部做 `untagged_addr()` 与页对齐 |
| `len` | 长度（字节），扩展到页边界 |
| `flags` | 0 等同 `mlock`；`MLOCK_ONFAULT` 表示延迟锁定 |

`MLOCK_ONFAULT` 定义于 `include/uapi/asm-generic/mman-common.h:39`：

```c
#define MLOCK_ONFAULT   0x01  /* Lock pages in range after they are faulted in,
                                 do not prefault */
```

`include/uapi/linux/mman.h` 包含该头，因此 `<sys/mman.h>` 中可见。

## 3. 内核实现路径

```
__arm64_sys_mlock2(start, len, flags)
  └─ SYSCALL_DEFINE3(mlock2)                       // mm/mlock.c:670
       ├─ flags & ~MLOCK_ONFAULT → -EINVAL
       ├─ flags & MLOCK_ONFAULT → vm_flags |= VM_LOCKONFAULT
       └─ do_mlock(start, len, vm_flags)           // 618
            ├─ start = untagged_addr(start)
            ├─ can_do_mlock()                      // 40
            │    ├─ rlimit(RLIMIT_MEMLOCK) != 0 → true
            │    └─ capable(CAP_IPC_LOCK) → true
            │    否则 -EPERM
            ├─ len = PAGE_ALIGN(len + offset_in_page(start))
            ├─ start &= PAGE_MASK
            ├─ lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT
            ├─ mmap_write_lock_killable(current->mm)   // 失败 → -EINTR
            ├─ locked = (len >> PAGE_SHIFT) + mm->locked_vm
            ├─ 超限且无 CAP_IPC_LOCK:
            │    locked -= count_mm_mlocked_page_nr()  // 扣除已锁定重叠部分
            ├─ locked <= lock_limit 或 CAP_IPC_LOCK ?
            │    └─ apply_vma_lock_flags(start, len, flags)  // 520
            │         └─ for_each_vma_range()
            │              └─ mlock_fixup()        // 467
            │                   ├─ 设置 VM_LOCKED / VM_LOCKONFAULT
            │                   ├─ 必要时拆分/合并 VMA
            │                   └─ mm->locked_vm +=/-= nr_pages
            ├─ mmap_write_unlock(current->mm)
            └─ __mm_populate(start, len, 0)
                 └─ populate_vma_page_range()
                      └─ VM_LOCKONFAULT 时立即返回（不预填充）
```

`populate_vma_page_range()` 的提前返回（`mm/gup.c:1832`）：

```c
/*
 * Rightly or wrongly, the VM_LOCKONFAULT case has never used
 * faultin_page() to break COW, so it has no work to do here.
 */
if (vma->vm_flags & VM_LOCKONFAULT)
    return nr_pages;
```

因此 `do_mlock()` 虽统一调用 `__mm_populate()`，但在 `MLOCK_ONFAULT` 场景下立刻返回；真正的锁定发生在后续访问触发缺页时，由 `mm/memory.c` 的缺页路径依据 `VM_LOCKED` 标记把页面加入 unevictable 链表。

## 4. 核心数据结构

### 4.1 VMA 锁定标志（`include/linux/mm.h`）

```c
#define VM_LOCKED        INIT_VM_FLAG(LOCKED)       /* 页面常驻，不可换出 */
#define VM_LOCKONFAULT   INIT_VM_FLAG(LOCKONFAULT)  /* 首次缺页时锁定 */
#define VM_LOCKED_MASK   (VM_LOCKED | VM_LOCKONFAULT)
```

`apply_vma_lock_flags()` 用 `flags` 覆盖 `VM_LOCKED_MASK` 位：

```c
newflags = vma->vm_flags & ~VM_LOCKED_MASK;
newflags |= flags;
```

### 4.2 mlock_fixup 的计数逻辑（`mm/mlock.c:467`）

`mlock_fixup()` 先跳过 `vma_is_secretmem()` 与不支持 mlock 的 VMA；随后 `vma_modify_flags()` 拆分/合并 VMA，并按新旧 `VMA_LOCKED_BIT` 计算 `nr_pages`（加锁为正值、解锁取负、原本已锁则为 0），最后累加到 `mm->locked_vm`。

### 4.3 mm_struct 计数

```c
// include/linux/mm_types.h
struct mm_struct {
    atomic_long_t locked_vm;   /* 已锁定页数（以页为单位） */
    /* ... */
};
```

`RLIMIT_MEMLOCK` 以字节为单位，`do_mlock()` 右移 `PAGE_SHIFT` 后与 `locked_vm` 比较。

## 5. 关键流程

```
mlock2(addr, len, MLOCK_ONFAULT)
        ├─ flags 校验（仅允许 bit0）
        ├─ do_mlock(addr, len, VM_LOCKED | VM_LOCKONFAULT)
        │    ├─ can_do_mlock()：RLIMIT_MEMLOCK != 0 或 CAP_IPC_LOCK
        │    ├─ 页对齐 + mmap_write_lock_killable()
        │    ├─ 计算 locked_vm 增量并检查 RLIMIT_MEMLOCK
        │    │    └─ 超限且无 CAP_IPC_LOCK → -ENOMEM
        │    ├─ apply_vma_lock_flags() → mlock_fixup() 打标 + 计数
        │    ├─ mmap_write_unlock()
        │    └─ __mm_populate()：VM_LOCKONFAULT 时直接返回
        └─ 返回 0（页面在首次访问缺页时才被锁定）
```

对比 `mlock()`：传入 `VM_LOCKED`（无 `VM_LOCKONFAULT`），`__mm_populate()` 会遍历页表并触发缺页，把整段区间立即填充并锁定。

## 6. 返回值与错误码

成功返回 0。

| 错误码 | 触发条件 |
|--------|----------|
| `-EINVAL` | `flags` 含 `MLOCK_ONFAULT` 之外的位；地址范围溢出（`end < start`） |
| `-EPERM` | `RLIMIT_MEMLOCK` 为 0 且无 `CAP_IPC_LOCK`（`can_do_mlock()`） |
| `-EINTR` | `mmap_write_lock_killable()` 被信号打断 |
| `-ENOMEM` | 锁定页数超过 `RLIMIT_MEMLOCK`；区间存在未映射的洞；VMA 拆分失败 |
| `-EAGAIN` | `__mm_populate()` 的 `-ENOMEM` 经 `__mlock_posix_error_return()` 转换（609） |

`__mlock_posix_error_return()` 的转换规则：

```c
if (retval == -EFAULT)      retval = -ENOMEM;
else if (retval == -ENOMEM) retval = -EAGAIN;
```

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    size_t len = 64UL << 20;   /* 64 MiB：全部 mlock 代价很大 */
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* 只标记：首次访问到的页面才锁定，未访问的不占锁定配额 */
    if (mlock2(p, len, MLOCK_ONFAULT) == -1) {
        fprintf(stderr, "mlock2: %s\n", strerror(errno));
        munmap(p, len);
        return 1;
    }

    /* 只 touch 前 4 KiB，仅该页进入 unevictable 链表 */
    memset(p, 0x5a, 4096);

    if (munlock(p, len) == -1)
        perror("munlock");

    munmap(p, len);
    return 0;
}
```

若进程 `RLIMIT_MEMLOCK` 为 0（常见于容器默认配置），需要 `CAP_IPC_LOCK` 或调大 `ulimit -l` 后再测试。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `mm/mlock.c` | `SYSCALL_DEFINE3(mlock2)`（670）、`do_mlock()`（618）、`apply_vma_lock_flags()`（520）、`mlock_fixup()`（467）、`count_mm_mlocked_page_nr()`（577） |
| `mm/gup.c` | `populate_vma_page_range()` 对 `VM_LOCKONFAULT` 的提前返回（1832） |
| `include/linux/mm.h` | `VM_LOCKED`、`VM_LOCKONFAULT`、`VM_LOCKED_MASK` |
| `include/uapi/asm-generic/mman-common.h` | `MLOCK_ONFAULT`（39 行） |
| `include/linux/mm_types.h` | `mm_struct.locked_vm` |
| `arch/arm64/include/generated/uapi/asm/unistd_64.h` | `__NR_mlock2 = 284` |
