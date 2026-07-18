# process_vm_readv 系统调用分析

## 1. 概述

`process_vm_readv` 是 Linux 内核提供的跨进程内存读取系统调用。它允许一个进程直接读取另一个进程的用户空间内存，而无需通过 `ptrace`、`/proc/pid/mem` 或 `pipe` 等传统 IPC 机制。该调用使用 `iovec` 向量化 I/O 模式，支持在本地和远程进程间进行分散/聚集（scatter/gather）数据传输。

**原型：**

```c
SYSCALL_DEFINE6(process_vm_readv, pid_t, pid,
                const struct iovec __user *, lvec,
                unsigned long, liovcnt,
                const struct iovec __user *, rvec,
                unsigned long, riovcnt,
                unsigned long, flags);
```

- `pid`: 目标进程的 PID。
- `lvec`: 本地进程的 `iovec` 数组，指定数据应写入的本地内存区域。
- `liovcnt`: `lvec` 数组的元素个数。
- `rvec`: 远程进程的 `iovec` 数组，指定从远程进程的哪些地址读取数据。
- `riovcnt`: `rvec` 数组的元素个数。
- `flags`: 保留标志位，当前必须为 0。

**返回值：** 成功时返回实际读取的字节数；失败时返回负的错误码。

**注意：** 读取的字节数可能小于请求的总字节数（例如，在部分读取过程中遇到错误时）。

## 2. 使用场景

- **调试器**：读取被调试进程的内存，无需 `ptrace` 暂停目标进程
- **性能分析工具**：采样分析目标进程的堆栈、堆内存等
- **检查点/恢复（CRIU）**：保存和恢复进程的内存状态
- **进程间大数据传输**：避免 `pipe`/`socket` 的额外拷贝，直接读取目标进程内存
- **安全监控**：检查其他进程的内存内容（需要足够权限）

## 3. 函数调用栈

```
process_vm_readv (系统调用入口)
└── process_vm_rw(pid, lvec, liovcnt, rvec, riovcnt, flags, vm_write=0)
    ├── import_iovec(ITER_DEST, lvec, liovcnt, ...)  // 导入本地 iovec 到 iov_iter
    ├── iovec_from_user(rvec, riovcnt, ...)           // 导入远程 iovec
    └── process_vm_rw_core(pid, &iter, iov_r, riovcnt, flags, vm_write=0)
        ├── 计算所需最大页数 (nr_pages)
        ├── 若 nr_pages > 16: kmalloc 分配 struct page* 数组
        ├── find_get_task_by_vpid(pid)                // 查找目标进程 task_struct
        ├── mm_access(task, PTRACE_MODE_ATTACH_REALCREDS)  // 获取目标进程 mm_struct
        │   └── 权限检查: ptrace_may_access()
        └── 循环处理每个远程 iovec 条目:
            └── process_vm_rw_single_vec(addr, len, iter, pages, mm, task, 0)
                ├── 计算地址范围和页数
                ├── mmap_read_lock(mm)                 // 获取 mmap 读锁
                ├── pin_user_pages_remote(mm, addr, ...) // 锁定远程页面
                ├── mmap_read_unlock(mm)
                └── process_vm_rw_pages(pages, offset, len, iter, 0)
                    └── copy_page_to_iter(page, offset, copy, iter)
                        // 将远程页内容拷贝到本地 iov_iter
```

## 4. 关键数据结构

### struct iovec（用户空间 I/O 向量）

```c
struct iovec {
    void __user *iov_base;  /* 缓冲区起始地址 */
    __kernel_size_t iov_len; /* 缓冲区长度 */
};
```

### struct iov_iter（内核 I/O 迭代器）

```c
// 内核内部使用的迭代器，用于遍历 iovec 数组
// 在 process_vm_rw 中由 import_iovec() 创建
struct iov_iter {
    u8 iter_type;          // 迭代器类型 (ITER_DEST 或 ITER_SOURCE)
    loff_t start;          // 起始偏移
    ...
    union {
        const struct iovec *iov;  // 指向 iovec 数组
        ...
    };
    unsigned long count;   // 剩余字节数
    ...
};
```

### 核心内联函数原型

```c
/* 将远程页中的数据拷贝到本地迭代器 (vm_write=0 时的读操作) */
static int process_vm_rw_pages(struct page **pages,
                               unsigned offset,
                               size_t len,
                               struct iov_iter *iter,
                               int vm_write)
{
    while (len && iov_iter_count(iter)) {
        struct page *page = *pages++;
        size_t copy = PAGE_SIZE - offset;
        size_t copied;

        if (copy > len)
            copy = len;

        if (vm_write)
            copied = copy_page_from_iter(page, offset, copy, iter);
        else
            copied = copy_page_to_iter(page, offset, copy, iter);
            // 读操作: 从远程页拷贝到本地

        len -= copied;
        if (copied < copy && iov_iter_count(iter))
            return -EFAULT;
        offset = 0;
    }
    return 0;
}
```

## 5. 流程图

```
用户态: process_vm_readv(pid, lvec, liovcnt, rvec, riovcnt, flags=0)
                              │
                              ▼
                    ┌─────────────────────────┐
                    │  process_vm_rw()          │
                    │  (vm_write = 0)           │
                    │                           │
                    │  import_iovec(ITER_DEST)   │ ← 导入本地 iovec
                    │  iovec_from_user()         │ ← 导入远程 iovec
                    └───────────┬───────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │  process_vm_rw_core()    │
                    │                           │
                    │  计算所需最大页数         │
                    │  分配 pages 数组          │
                    └───────────┬───────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │  find_get_task_by_vpid() │
                    │  → 获取 task_struct       │
                    └───────────┬───────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │  mm_access(ATTACH_REAL)  │
                    │  → ptrace_may_access()   │
                    │  → 获取 mm_struct         │
                    ├─────────────────────────┤
                    │  权限不足? → -EPERM      │
                    │  进程不存在? → -ESRCH    │
                    └───────────┬───────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │  对每个远程 iovec 条目:   │
                    │                           │
                    │  ┌─────────────────────┐  │
                    │  │process_vm_rw_single_│  │
                    │  │vec()                │  │
                    │  │                     │  │
                    │  │  pin_user_pages_    │  │
                    │  │  remote()           │  │
                    │  │  → 锁定远程页面      │  │
                    │  │                     │  │
                    │  │  process_vm_rw_     │  │
                    │  │  pages()            │  │
                    │  │  → copy_page_to_    │  │
                    │  │    iter()           │  │
                    │  │  → 拷贝到本地 iovec  │  │
                    │  └─────────┬───────────┘  │
                    └───────────┬───────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │  返回实际读取字节数      │
                    │  或错误码                │
                    └─────────────────────────┘
```

## 6. 错误处理

| 错误码 | 触发条件 |
|--------|---------|
| `-EFAULT` | `import_iovec` 或 `iovec_from_user` 访问用户空间失败 |
| `-EINVAL` | `flags` 参数非零 |
| `-ESRCH` | 指定的 `pid` 对应的进程不存在 |
| `-EPERM` | `mm_access` 权限检查失败（非 root 且未附加 ptrace） |
| `-ENOMEM` | `kmalloc` 分配 pages 数组失败 |
| `-EFAULT` | `copy_page_to_iter` 拷贝过程中出错 |

**部分读取语义：** 如果已经成功读取了一些字节但中途遇到错误，函数返回已读取的字节数（而非错误码）。仅当没有任何字节被读取时，才返回错误码。

## 7. 使用示例

### 示例 1: 读取另一进程的字符串

```c
#include <sys/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    pid_t pid = atoi(argv[1]);
    unsigned long remote_addr = strtoul(argv[2], NULL, 16);
    char buf[256];
    struct iovec local[1], remote[1];
    ssize_t nread;

    local[0].iov_base = buf;
    local[0].iov_len  = sizeof(buf) - 1;

    remote[0].iov_base = (void *)remote_addr;
    remote[0].iov_len  = sizeof(buf) - 1;

    nread = process_vm_readv(pid, local, 1, remote, 1, 0);
    if (nread < 0) {
        perror("process_vm_readv");
        exit(1);
    }

    buf[nread] = '\0';
    printf("Read %zd bytes from PID %d at 0x%lx: %s\n",
           nread, pid, remote_addr, buf);
    return 0;
}
```

### 示例 2: 分散读取 - 从远程读取到多个本地缓冲区

```c
#include <sys/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    pid_t pid = atoi(argv[1]);
    unsigned long remote_addr = strtoul(argv[2], NULL, 16);

    char buf1[64], buf2[64], buf3[64];
    struct iovec local[3], remote[1];
    ssize_t nread;

    local[0].iov_base = buf1;
    local[0].iov_len  = sizeof(buf1);
    local[1].iov_base = buf2;
    local[1].iov_len  = sizeof(buf2);
    local[2].iov_base = buf3;
    local[2].iov_len  = sizeof(buf3);

    remote[0].iov_base = (void *)remote_addr;
    remote[0].iov_len  = sizeof(buf1) + sizeof(buf2) + sizeof(buf3);

    nread = process_vm_readv(pid, local, 3, remote, 1, 0);
    if (nread < 0) {
        perror("process_vm_readv");
        exit(1);
    }
    printf("Read %zd bytes total\n", nread);
    return 0;
}
```

### 示例 3: 使用 gdb 时的等效操作

```bash
# 调试器底层使用 process_vm_readv 读取目标进程内存
gdb -p 1234
(gdb) x/16x 0x7fff12345678
(gdb) dump memory /tmp/dump.bin 0x7fff12345678 0x7fff12346678
```

## 8. 注意事项

- **权限要求**：调用者需要对目标进程有 `PTRACE_MODE_ATTACH_REALCREDS` 权限（通常是同一用户或 root）
- **原子性**：`process_vm_readv` 不保证对目标进程的原子性访问，目标进程可能同时修改其内存
- **性能**：与共享内存（`mmap`）相比，`process_vm_readv` 涉及页表锁定和额外的页面拷贝，适合一次性或低频率的访问
- **SELinux/AppArmor**：LSM 模块可能通过 `ptrace_access_check` 进一步限制访问
- **flags 参数**：当前必须为 0，未来可能用于扩展（如支持 `O_NONBLOCK` 语义）

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 内核源码: `mm/process_vm_access.c`
- 内核源码: `include/uapi/uio.h` (struct iovec)
- `man process_vm_readv(2)`