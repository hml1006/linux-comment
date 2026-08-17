# remap_file_pages 系统调用分析

## 1. 概述

`remap_file_pages` 系统调用用于创建非线性文件映射（non-linear file mappings），即允许将文件的不同页面映射到虚拟地址空间的不同位置，而不需要多次调用 `mmap`。该调用已被标记为**废弃**（deprecated）。

**内核源码位置：** `mm/mmap.c`

**原型：**

```c
SYSCALL_DEFINE5(remap_file_pages, unsigned long, start, unsigned long, size,
                unsigned long, prot, unsigned long, pgoff, unsigned long, flags)
```

| 参数 | 描述 |
|------|------|
| `start` | 起始地址（必须是已有映射的地址） |
| `size` | 区域大小（字节） |
| `prot` | 必须为 0（当前实现中不再使用） |
| `pgoff` | 文件偏移（以页为单位） |
| `flags` | 标志位（仅支持 MAP_NONBLOCK） |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **历史遗留支持**：该接口是为旧版非线性映射设计的
- **迁移指导**：新代码应使用 `mmap(MAP_FIXED)` 替代

## 3. 函数调用链分析

```
remap_file_pages(start, size, prot, pgoff, flags)         // 系统调用入口
  ├─ pr_warn_once()                                      // 打印废弃警告
  ├─ 参数验证：
  │    ├─ prot 必须为 0
  │    ├─ start 页对齐
  │    ├─ size 页对齐
  │    └─ 溢出检查
  ├─ mmap_read_lock_killable(mm)                          // 获取读锁
  ├─ vma_lookup(mm, start)                               // 查找起始 VMA
  ├─ 检查 VMA 是否有效：
  │    ├─ VMA 必须存在
  │    └─ VMA 必须为 VM_SHARED
  ├─ 计算 prot 和 flags
  ├─ security_mmap_file(file, prot, flags)                // LSM 安全检查
  ├─ mmap_read_unlock → mmap_write_lock                  // 升级为写锁
  └─ 重新检查 VMA 状态（未发生改变）
       └─ do_mmap(file, start, size, prot, flags, ...)   // 重新映射
            └─ 创建新的文件映射覆盖原有区域
```

## 4. 关键实现细节

### 模拟实现

当前 `remap_file_pages` 的实现在内核中是一个**模拟**（emulation），它不再真正创建非线性映射，而是通过调用 `do_mmap()` 创建常规的文件映射来覆盖指定地址范围。

```c
/* 内核打印的一次性警告 */
pr_warn_once("%s (%d) uses deprecated remap_file_pages() syscall. "
             "See Documentation/mm/remap_file_pages.rst.\n",
             current->comm, current->pid);
```

### 参数转换

```c
// 从 VMA 继承权限
prot |= vma->vm_flags & VM_READ ? PROT_READ : 0;
prot |= vma->vm_flags & VM_WRITE ? PROT_WRITE : 0;
prot |= vma->vm_flags & VM_EXEC ? PROT_EXEC : 0;

// 构造 flags
flags &= MAP_NONBLOCK;
flags |= MAP_SHARED | MAP_FIXED | MAP_POPULATE;
if (vma->vm_flags & VM_LOCKED)
    flags |= MAP_LOCKED;
```

## 5. 流程图

```
  用户态调用 remap_file_pages(start, size, prot, pgoff, flags)
         │
         ▼
  ┌──────────────────────────────┐
  │  pr_warn_once()              │  打印废弃警告
  │  "deprecated remap_file_     │
  │   pages() syscall"           │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  参数验证                    │
  │  ├─ prot != 0 → -EINVAL     │
  │  ├─ start 页对齐             │
  │  └─ size 页对齐 + 溢出检查   │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_read_lock(mm)          │  获取读锁
  │  vma_lookup(mm, start)      │
  │  VMA 必须为 VM_SHARED       │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  从 VMA 获取 prot            │
  │  构造 MAP_FIXED 标志         │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  security_mmap_file()        │
  │  mmap_read_unlock(mm)        │
  │  mmap_write_lock(mm)         │  升级为写锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  重新检查 VMA 状态           │
  │  do_mmap(file, start, size,  │  创建新的文件映射
  │          prot, flags, ...)   │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_write_unlock(mm)       │
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | prot 非零、VMA 不存在或非 VM_SHARED |
| `-ENOMEM` | 内存不足 |
| `-EINTR` | 获取 mmap 锁时被信号中断 |

## 7. 使用示例

**注意：** 该调用已废弃，新代码不应使用。以下示例仅用于说明。

```c
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    /* 传统非线性映射的使用方式（已废弃） */
    int fd = open("/tmp/file", O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    ftruncate(fd, 8192);

    /* 创建文件映射 */
    void *addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    /* 传统方式：将文件第 1 页映射到已映射区域（已废弃，使用 mmap 替代） */
    unsigned long start = (unsigned long)addr;
    if (syscall(__NR_remap_file_pages, start, 4096, 0, 1, 0) == -1) {
        perror("remap_file_pages");
    }

    /* 现代替代方案：使用 mmap(MAP_FIXED) */
    void *new_addr = mmap(addr, 4096, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED, fd, 1);
    if (new_addr == MAP_FAILED) {
        perror("mmap fixed");
    }

    munmap(addr, 4096);
    close(fd);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | remap_file_pages | mmap(MAP_FIXED) | mremap |
|------|-----------------|-----------------|--------|
| 状态 | **废弃** | 推荐 | 活跃 |
| 文件映射 | 是 | 是 | 是 |
| 地址指定 | 覆盖已有 | 覆盖已有 | 可移动 |
| 非线性映射 | 曾是 | 不支持 | 不支持 |

## 9. 废弃原因

1. **复杂性**：非线性映射增加了内核内存管理的复杂性，收益却很小。

2. **性能问题**：非线性映射导致 TLB 缓存的效率降低，因为相邻的虚拟地址不再对应相邻的物理/文件位置。

3. **替代方案**：`mmap(MAP_FIXED)` 可以更清晰、更高效地实现相同的功能。

4. **内核维护负担**：非线性映射的实现需要大量特殊处理，且很少被使用。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/mmap.c`
- 内核文档：`Documentation/mm/remap_file_pages.rst`
- 联机手册：`remap_file_pages(2)`