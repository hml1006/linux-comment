# process_vm_writev 系统调用分析

## 1. 概述

`process_vm_writev` 是 Linux 内核提供的跨进程内存写入系统调用。它允许一个进程直接写入另一个进程的用户空间内存，而无需通过 `ptrace`、`/proc/pid/mem` 等传统机制。与 `process_vm_readv` 对称，该调用使用 `iovec` 向量化 I/O 模式，支持从本地进程的多个缓冲区向远程进程的多个目标地址进行聚集/分散（gather/scatter）数据传输。

**原型：**

```c
SYSCALL_DEFINE6(process_vm_writev, pid_t, pid,
                const struct iovec __user *, lvec,
                unsigned long, liovcnt,
                const struct iovec __user *, rvec,
                unsigned long, riovcnt,
                unsigned long, flags);
```

- `pid`: 目标进程的 PID。
- `lvec`: 本地进程的 `iovec` 数组，指定数据来源的本地内存区域。
- `liovcnt`: `lvec` 数组的元素个数。
- `rvec`: 远程进程的 `iovec` 数组，指定远程进程中的目标地址。
- `riovcnt`: `rvec` 数组的元素个数。
- `flags`: 保留标志位，当前必须为 0。

**返回值：** 成功时返回实际写入的字节数；失败时返回负的错误码。

## 2. 使用场景

- **调试器**：修改被调试进程的内存（如设置断点、修改变量值）
- **检查点/恢复（CRIU）**：恢复进程时写入内存状态
- **进程间数据注入**：向目标进程的特定内存区域写入数据
- **性能分析工具**：在目标进程中写入分析标记或计数器
- **动态代码注入**：在目标进程的内存中写入代码（需要与架构相关的内存权限设置配合）

## 3. 函数调用栈

```
process_vm_writev (系统调用入口)
└── process_vm_rw(pid, lvec, liovcnt, rvec, riovcnt, flags, vm_write=1)
    ├── import_iovec(ITER_SOURCE, lvec, liovcnt, ...)  // 导入本地 iovec 到 iov_iter
    ├── iovec_from_user(rvec, riovcnt, ...)             // 导入远程 iovec
    └── process_vm_rw_core(pid, &iter, iov_r, riovcnt, flags, vm_write=1)
        ├── 计算所需最大页数 (nr_pages)
        ├── 若 nr_pages > 16: kmalloc 分配 struct page* 数组
        ├── find_get_task_by_vpid(pid)                  // 查找目标进程 task_struct
        ├── mm_access(task, PTRACE_MODE_ATTACH_REALCREDS)  // 获取目标进程 mm_struct
        │   └── 权限检查: ptrace_may_access()
        └── 循环处理每个远程 iovec 条目:
            └── process_vm_rw_single_vec(addr, len, iter, pages, mm, task, 1)
                ├── 计算地址范围和页数
                ├── flags |= FOLL_WRITE                  // 页锁定需要写权限
                ├── mmap_read_lock(mm)                   // 获取 mmap 读锁
                ├── pin_user_pages_remote(mm, addr, ...)  // 锁定远程页面 (FOLL_WRITE)
                ├── mmap_read_unlock(mm)
                └── process_vm_rw_pages(pages, offset, len, iter, 1)
                    └── copy_page_from_iter(page, offset, copy, iter)
                        // 将本地数据拷贝到远程页
```

## 4. 关键数据结构

### struct iovec（用户空间 I/O 向量）

```c
struct iovec {
    void __user *iov_base;  /* 缓冲区起始地址 */
    __kernel_size_t iov_len; /* 缓冲区长度 */
};
```

### 与 process_vm_readv 的区别

| 方面 | process_vm_readv | process_vm_writev |
|------|-----------------|-------------------|
| `vm_write` 参数 | 0 | 1 |
| `iov_iter` 方向 | `ITER_DEST`（数据写入本地） | `ITER_SOURCE`（数据来自本地） |
| 页锁定标志 | 无 `FOLL_WRITE` | `FOLL_WRITE` |
| 拷贝函数 | `copy_page_to_iter()` | `copy_page_from_iter()` |
| 数据流向 | 远程 → 本地 | 本地 → 远程 |

### 核心写入函数

```c
/* 将本地迭代器中的数据拷贝到远程页 (vm_write=1 时的写操作) */
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
            // 写操作: 从本地迭代器拷贝到远程页
        else
            copied = copy_page_to_iter(page, offset, copy, iter);

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
用户态: process_vm_writev(pid, lvec, liovcnt, rvec, riovcnt, flags=0)
                              │
                              ▼
                    ┌─────────────────────────┐
                    │  process_vm_rw()          │
                    │  (vm_write = 1)           │
                    │                           │
                    │  import_iovec(ITER_SOURCE) │ ← 导入本地数据源
                    │  iovec_from_user()         │ ← 导入远程目标地址
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
                    │  │  remote(FOLL_WRITE) │  │
                    │  │  → 锁定远程页面      │  │
                    │  │    (可写)           │  │
                    │  │                     │  │
                    │  │  process_vm_rw_     │  │
                    │  │  pages()            │  │
                    │  │  → copy_page_from_  │  │
                    │  │    iter()           │  │
                    │  │  → 从本地拷贝到远程   │  │
                    │  └─────────┬───────────┘  │
                    └───────────┬───────────────┘
                                │
                                ▼
                    ┌─────────────────────────┐
                    │  返回实际写入字节数      │
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
| `-EFAULT` | `copy_page_from_iter` 拷贝过程中出错 |

**部分写入语义：** 如果已经成功写入了一些字节但中途遇到错误，函数返回已写入的字节数（而非错误码）。仅当没有任何字节被写入时，才返回错误码。

## 7. 使用示例

### 示例 1: 修改目标进程的变量值

```c
#include <sys/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    pid_t pid = atoi(argv[1]);
    unsigned long remote_addr = strtoul(argv[2], NULL, 16);
    int new_value = 42;  /* 要写入的新值 */
    struct iovec local[1], remote[1];
    ssize_t nwritten;

    local[0].iov_base = &new_value;
    local[0].iov_len  = sizeof(new_value);

    remote[0].iov_base = (void *)remote_addr;
    remote[0].iov_len  = sizeof(new_value);

    nwritten = process_vm_writev(pid, local, 1, remote, 1, 0);
    if (nwritten < 0) {
        perror("process_vm_writev");
        exit(1);
    }
    printf("Wrote %zd bytes to PID %d at 0x%lx\n",
           nwritten, pid, remote_addr);
    return 0;
}
```

### 示例 2: 从多个本地缓冲区写入远程进程

```c
#include <sys/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[])
{
    pid_t pid = atoi(argv[1]);
    unsigned long remote_addr = strtoul(argv[2], NULL, 16);

    char header[] = "[HEADER]";
    char data[]   = "Hello from another process!";
    char footer[] = "[FOOTER]";

    struct iovec local[3], remote[1];
    ssize_t nwritten;

    local[0].iov_base = header;
    local[0].iov_len  = strlen(header);
    local[1].iov_base = data;
    local[1].iov_len  = strlen(data) + 1;
    local[2].iov_base = footer;
    local[2].iov_len  = strlen(footer);

    remote[0].iov_base = (void *)remote_addr;
    remote[0].iov_len  = local[0].iov_len + local[1].iov_len + local[2].iov_len;

    nwritten = process_vm_writev(pid, local, 3, remote, 1, 0);
    if (nwritten < 0) {
        perror("process_vm_writev");
        exit(1);
    }
    printf("Wrote %zd bytes total\n", nwritten);
    return 0;
}
```

### 示例 3: 配合 ptrace 设置断点（软件断点指令）

```c
#include <sys/uio.h>
#include <sys/ptrace.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    pid_t pid = atoi(argv[1]);
    unsigned long target_addr = strtoul(argv[2], NULL, 16);

    /* x86: int3 指令 = 0xCC */
    unsigned char breakpoint = 0xCC;
    struct iovec local, remote;
    ssize_t nwritten;

    /* 先附加到目标进程 */
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
        perror("ptrace ATTACH");
        exit(1);
    }
    waitpid(pid, NULL, 0);

    local.iov_base = &breakpoint;
    local.iov_len  = sizeof(breakpoint);
    remote.iov_base = (void *)target_addr;
    remote.iov_len  = sizeof(breakpoint);

    nwritten = process_vm_writev(pid, &local, 1, &remote, 1, 0);
    if (nwritten < 0) {
        perror("process_vm_writev");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        exit(1);
    }

    printf("Breakpoint set at 0x%lx in PID %d\n", target_addr, pid);

    ptrace(PTRACE_CONT, pid, NULL, NULL);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return 0;
}
```

## 8. 注意事项

- **权限要求**：需要对目标进程有 `PTRACE_MODE_ATTACH_REALCREDS` 权限
- **内存保护**：写入的目标页必须可写（`VM_WRITE`），否则 `pin_user_pages_remote(FOLL_WRITE)` 会失败
- **原子性**：`process_vm_writev` 不提供原子性保证，目标进程可能正在并发读写同一内存区域
- **与 readv 的对称性**：`process_vm_readv` 和 `process_vm_writev` 共享同一核心实现 `process_vm_rw()`，仅通过 `vm_write` 标志区分
- **安全风险**：写入操作可能被用于恶意目的（如代码注入），因此 Linux 内核通过严格的权限检查和 LSM 钩子进行防护
- **COW 页**：如果目标页是写时复制（COW）页，写入操作会触发 COW 机制，创建私有副本

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 内核源码: `mm/process_vm_access.c`
- 内核源码: `include/uapi/uio.h` (struct iovec)
- `man process_vm_writev(2)`
- `process_vm_readv` 文档: `process_vm_readv.md`