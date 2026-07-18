# brk 系统调用分析

## 1. 概述

`brk` 系统调用用于改变进程堆（heap）的边界。堆是进程数据段之后的一段动态内存区域，通常通过 `brk`/`sbrk` 或 `malloc` 库函数进行管理。

**内核源码位置：** `mm/mmap.c`

**原型：**

```c
SYSCALL_DEFINE1(brk, unsigned long, brk)
```

| 参数 | 描述 |
|------|------|
| `brk` | 新的堆结束地址（0 表示返回当前 brk 值） |

**返回值：**
- 成功时返回新的 brk 地址
- 失败时返回原来的 brk 地址（注意：不是返回 -1 错误码）

## 2. 使用场景

- **堆内存分配**：`malloc`/`free` 库函数在底层通过 `brk` 或 `mmap` 管理堆
- **动态内存管理**：小内存分配优先使用 brk，大分配使用 `mmap`
- **sbrk 兼容**：POSIX `sbrk` 函数基于 `brk` 实现

## 3. 函数调用链分析

```
brk(brk)                                          // 系统调用入口
  └─ mmap_write_lock_killable(mm)                  // 获取写锁
  └─ 参数验证
       ├─ check_data_rlimit()                      // 检查 RLIMIT_DATA 限制
       ├─ PAGE_ALIGN(brk)                         // 页对齐
       └─ 与当前 brk 比较
            ├─ 缩小 (brk <= mm->brk)               // 收缩堆
            │    └─ do_vmi_align_munmap()          // 解除映射
            └─ 扩大 (brk > mm->brk)               // 扩展堆
                 ├─ check_brk_limits()              // 检查堆限制
                 ├─ stack_guard_gap 检查             // 栈保护间隙检查
                 └─ do_brk_flags()                  // 执行堆扩展
                      ├─ get_unmapped_area          // 查找可用区域
                      ├─ security_vm_enough_memory_mm // 安全检查
                      ├─ vma_merge / vma_expand     // 扩展或合并 VMA
                      └─ vma_link                   // 连接 VMA
  └─ mm_populate()                                // 如果 VM_LOCKED，预填充页表
  └─ mmap_write_unlock(mm)                        // 释放写锁
```

## 4. 关键数据结构

### `struct mm_struct` 中与 brk 相关的字段

```c
struct mm_struct {
    unsigned long start_brk;    /* 堆的起始地址（通常与 end_data 相同） */
    unsigned long brk;          /* 当前堆的结束地址 */
    unsigned long start_data;   /* 数据段起始地址 */
    unsigned long end_data;     /* 数据段结束地址 */
    unsigned long start_stack;  /* 栈起始地址 */
    unsigned long map_count;    /* 当前 VMA 数量 */
    // ...
};
```

### 堆与 VMA 关系

```
+----------------+ 0xFFFF_FFFF_FFFF_FFFF  (内核空间 TOP)
|   kernel VAS   |
+----------------+ TASK_SIZE
|      ...       |
|   stack (↓)    |
|   mmap 区域 (↑) |
|      ...       |
+----------------+
|     brk  →     |  ← do_brk_flags() 在此扩展堆
|   heap         |
+----------------+
|   end_data     |
|   data         |
+----------------+
|   text         |
+----------------+ 0x0000_0000_0000_0000
```

## 5. 流程图

```
  用户态调用 brk(new_addr)
         │
         ▼
  ┌─────────────────────────────┐
  │  mmap_write_lock_killable() │  获取 mmap 写锁
  └─────────────┬───────────────┘
                ▼
  ┌─────────────────────────────┐
  │  保存 origbrk = mm->brk     │
  └─────────────┬───────────────┘
                ▼
  ┌─────────────────────────────┐
  │  brk < min_brk ?            │───yes──→ 返回 origbrk
  └─────────────┬───────────────┘
                │ no
                ▼
  ┌─────────────────────────────┐
  │  check_data_rlimit()        │  检查 RLIMIT_DATA
  └─────────────┬───────────────┘
                ▼
  ┌─────────────────────────────┐
  │  oldbrk == newbrk ?         │───yes──→ mm->brk = brk; 返回 brk
  └─────────────┬───────────────┘
                │ no
                ▼
       ┌─────────────────┐
       │  brk <= mm->brk │  缩小还是扩大？
       └────────┬────────┘
    缩小(yes)   │            扩大(no)
       │        ▼               │
       │  缩小堆路径             │  扩大堆路径
       │        │               │
       ▼        ▼               ▼
  ┌──────────────────┐    ┌──────────────────┐
  │ do_vmi_align_    │    │ check_brk_limits()│
  │ munmap()         │    │ stack_guard_gap   │
  └────────┬─────────┘    │ 检查              │
           │              └────────┬─────────┘
           │                       │
           │              ┌────────▼─────────┐
           │              │ do_brk_flags()   │
           │              │ 扩展堆区域        │
           │              └────────┬─────────┘
           │                       │
           └───────────┬───────────┘
                       ▼
              ┌──────────────────┐
              │ mm->brk = brk    │
              │ mm_populate()    │  如果 def_flags 有 VM_LOCKED
              └────────┬─────────┘
                       ▼
              ┌──────────────────┐
              │ mmap_write_unlock│
              │ 返回 brk         │
              └──────────────────┘
```

## 6. 错误处理

| 错误条件 | 处理方式 |
|---------|---------|
| 获取 mmap 写锁被中断 | 返回 `-EINTR` |
| brk < min_brk（start_brk 或 end_data） | 返回原 brk 值 |
| 超过 RLIMIT_DATA 限制 | 返回原 brk 值 |
| 缩小堆时与现有非 brk VMA 冲突 | 返回原 brk 值 |
| 扩大堆时超过限制 | 返回原 brk 值 |
| 扩大堆时与栈保护间隙冲突 | 返回原 brk 值 |
| do_brk_flags 失败 | 返回原 brk 值 |

## 7. 使用示例

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

    /* 使用堆内存 */
    int *p = (int *)curr_brk;
    *p = 42;
    printf("Allocated at %p, value = %d\n", p, *p);

    /* 收缩堆回原位置 */
    brk(curr_brk);

    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | brk | mmap | sbrk |
|------|-----|------|------|
| 功能 | 设置堆边界 | 创建内存映射 | 增量调整 brk |
| 灵活性 | 仅堆区域 | 任意地址 / 文件映射 | 同 brk |
| 线程安全 | 需加锁 | 内核保证 | 需加锁 |
| 大内存分配 | 不适合 | 适合 | 不适合 |
| 释放方式 | brk 回退 | munmap | brk 回退 |

## 9. 关键实现细节

1. **成功/失败返回值特殊设计**：`brk` 在失败时返回**原来的 brk 值**而非负数错误码，这是 Unix 传统设计。调用者通过比较返回值与请求值是否相等来判断成功与否。

2. **CONFIG_COMPAT_BRK**：兼容旧版 brk 行为，当未启用地址空间随机化时，min_brk 使用 `end_data` 而非 `start_brk`。

3. **堆收缩实现**：收缩堆时调用 `do_vmi_align_munmap()` 释放页表，且会在成功时释放锁（`unlock = true`）。

4. **VM_LOCKED 处理**：如果 `mm->def_flags` 包含 `VM_LOCKED`（通过 `mlockall(MCL_FUTURE)` 设置），扩展的堆区域会自动被填充并锁定。

5. **栈保护间隙**：扩展堆时会检查是否与下一个 VMA 的栈保护间隙（`stack_guard_gap`）冲突，防止堆向栈方向增长过近。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mmap.c`
- 内核源码：`include/linux/mm_types.h`