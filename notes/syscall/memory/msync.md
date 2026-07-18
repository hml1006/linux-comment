# msync 系统调用分析

## 1. 概述

`msync` 系统调用用于将文件映射内存中的修改同步回底层文件。它确保对 `MAP_SHARED` 映射的写入被写回到磁盘上的文件。

**内核源码位置：** `mm/msync.c`

**原型：**

```c
SYSCALL_DEFINE3(msync, unsigned long, start, size_t, len, int, flags)
```

| 参数 | 描述 |
|------|------|
| `start` | 起始地址（必须页对齐） |
| `len` | 区域长度（字节） |
| `flags` | 同步标志（见下方） |

**flags 值：**

| 标志 | 描述 |
|------|------|
| `MS_ASYNC` | 异步写入，不等待 I/O 完成 |
| `MS_SYNC` | 同步写入，等待 I/O 完成 |
| `MS_INVALIDATE` | 使缓存失效，从文件重新读取数据 |

**返回值：**
- 成功返回 0
- 失败返回负数错误码

## 2. 使用场景

- **保证数据持久化**：确保 MAP_SHARED 映射的修改被写入磁盘
- **数据库事务**：在提交事务后同步数据文件
- **内存映射文件编辑器**：保存修改到文件

## 3. 函数调用链分析

```
msync(start, len, flags)                                // 系统调用入口
  ├─ untagged_addr(start)
  ├─ 参数验证：
  │    ├─ flags 检查（仅 MS_ASYNC | MS_INVALIDATE | MS_SYNC）
  │    ├─ start 页对齐检查
  │    ├─ MS_ASYNC 和 MS_SYNC 互斥检查
  │    └─ len 页对齐，end 溢出检查
  ├─ mmap_read_lock(mm)                                 // 获取读锁
  └─ 遍历 VMA 循环：
       ├─ find_vma(mm, start)                           // 查找起始 VMA
       ├─ 检查 VMA 是否存在
       ├─ 处理 VMA 之间的空洞（仅 MS_ASYNC 时跳过）
       ├─ MS_INVALIDATE 时检查 VM_LOCKED → -EBUSY
       ├─ 计算文件偏移：
       │    fstart = (start - vma->vm_start) + (vma->vm_pgoff << PAGE_SHIFT)
       │    fend = fstart + (min(end, vma->vm_end) - start) - 1
       ├─ 如果 MS_SYNC 且文件映射且 VM_SHARED：
       │    ├─ get_file(file)
       │    ├─ mmap_read_unlock(mm)                     // 释放读锁（避免锁反转）
       │    ├─ vfs_fsync_range(file, fstart, fend, 1)   // 文件系统同步
       │    │    └─ file->f_op->fsync()                 // 文件系统 fsync 回调
       │    └─ fput(file)
       │    (如果尚未完成，重新获取读锁继续)
       └─ 如果是 MS_ASYNC 或非共享映射：
            └─ 继续遍历下一个 VMA
  └─ mmap_read_unlock(mm)                               // 释放读锁
```

## 4. 关键数据结构

### msync 使用的 VMA 属性

```c
struct vm_area_struct {
    // ...
    struct file *vm_file;         /* 映射的文件 */
    unsigned long vm_pgoff;       /* 文件偏移（页为单位） */
    unsigned long vm_flags;       /* VM_SHARED 等标志 */
    // ...
};
```

### `vfs_fsync_range` 文件同步

```c
int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync)
{
    if (!file->f_op->fsync)
        return -EINVAL;
    return file->f_op->fsync(file, start, end, datasync);
}
```

## 5. 流程图

```
  用户态调用 msync(start, len, flags)
         │
         ▼
  ┌──────────────────────────────┐
  │  参数验证                    │
  │  ├─ flags 有效性             │
  │  ├─ start 页对齐             │
  │  └─ len 页对齐 + 溢出        │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_read_lock(mm)          │  获取读锁
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  find_vma(mm, start)         │  查找起始 VMA
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  VMA 循环遍历:               │
  │  ┌──────────────────────┐    │
  │  │ 检查 VMA 是否存在     │    │
  │  │ 是 → 继续              │    │
  │  │ 否 → -ENOMEM           │    │
  │  │                        │    │
  │  │ 处理空洞               │    │
  │  │ start < vma->vm_start  │    │
  │  │ → MS_ASYNC? 直接返回   │    │
  │  │   否则调整 start 并记录│    │
  │  │   未映射错误           │    │
  │  │                        │    │
  │  │ MS_INVALIDATE +        │    │
  │  │ VM_LOCKED? → -EBUSY    │    │
  │  │                        │    │
  │  │ ┌────────────────────┐ │    │
  │  │ │ MS_SYNC + 文件 +   │ │    │
  │  │ │ VM_SHARED?         │ │    │
  │  │ │ 是 →               │ │    │
  │  │ │ 释放读锁            │ │    │ 避免锁反转
  │  │ │ vfs_fsync_range()  │ │    │ 真正执行文件同步
  │  │ │ 获取读锁            │ │    │
  │  │ │ 否 → 继续遍历       │ │    │
  │  │ └────────────────────┘ │    │
  │  │                        │    │
  │  │ 更新 start = vma->    │ │    │
  │  │   vm_end              │ │    │
  │  │ 查找下一个 VMA        │ │    │
  │  └──────────────────────┘ │    │
  │  until start >= end       │    │
  └─────────────┬────────────────┘
                ▼
  ┌──────────────────────────────┐
  │  mmap_read_unlock(mm)        │
  └─────────────┬────────────────┘
                ▼
              返回 0
```

## 6. 错误处理

| 错误码 | 条件 |
|--------|------|
| `-EINVAL` | flags 无效、start 未页对齐、MS_ASYNC 和 MS_SYNC 同时设置 |
| `-ENOMEM` | 地址范围包含未映射区域 |
| `-EBUSY` | MS_INVALIDATE 且 VMA 被锁定（VM_LOCKED） |
| `-EIO` | 文件系统同步 I/O 错误 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main() {
    int fd = open("/tmp/msync_test.txt", O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }

    /* 设置文件大小 */
    ftruncate(fd, 4096);

    /* 创建共享文件映射 */
    char *addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    /* 写入数据 */
    strcpy(addr, "Hello, msync!");

    /* 同步写入到文件（等待 I/O 完成） */
    if (msync(addr, 4096, MS_SYNC) == -1) {
        perror("msync");
    }

    /* 修改数据后异步同步 */
    strcpy(addr, "Async update");
    if (msync(addr, 4096, MS_ASYNC) == -1) {
        perror("msync async");
    }

    /* 重新读取文件内容（使缓存失效） */
    char buf[64] = {0};
    if (msync(addr, 4096, MS_INVALIDATE) == -1) {
        perror("msync invalidate");
    }

    munmap(addr, 4096);
    close(fd);
    return 0;
}
```

## 8. 与相关系统调用的比较

| 特性 | msync | fsync | fdatasync | sync |
|------|-------|-------|-----------|------|
| 作用对象 | 内存映射 | 文件描述符 | 文件描述符 | 全局 |
| 操作范围 | 地址范围 | 整个文件 | 文件数据（不含元数据） | 所有文件 |
| 是否异步 | MS_ASYNC | 否 | 否 | 否 |
| 需要文件描述符 | 否 | 是 | 是 | 否 |

## 9. 关键实现细节

1. **MS_ASYNC 现在不做任何事情**：如源码注释所述，从 Linux 2.6.17 开始，MS_ASYNC 不再启动 I/O 或标记页面脏。脏页追踪由内核自动完成，应用程序可以调用 `fsync()` 或 `fadvise(FADV_DONTNEED)` 来触发异步写回。

2. **锁释放与获取**：`MS_SYNC` 处理时，内核会先释放 `mmap_read_lock` 再调用 `vfs_fsync_range()`，这是为了避免锁反转（lock inversion）——文件系统操作可能需要获取文件锁，而文件系统回调可能又需要 mmap 锁。

3. **空洞处理**：当地址范围包含未映射的 VMA 间隙时，会记录 `-ENOMEM` 错误，但继续处理其余部分。对于 `MS_ASYNC`，遇到空洞会直接返回。

4. **文件偏移计算**：`fstart = (start - vma->vm_start) + (vma->vm_pgoff << PAGE_SHIFT)` 将虚拟地址转换为文件偏移。

5. **仅共享映射有效**：只有 `VM_SHARED` 映射的修改需要同步到文件。`MAP_PRIVATE` 映射的修改是写时复制（COW）的，不会被写回文件。

## 10. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#内存管理)
- 内核源码：`mm/msync.c`
- 联机手册：`msync(2)`