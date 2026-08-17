# dup 系统调用完整路径分析

## 1 概述

`dup` 系统调用用于复制一个文件描述符，返回一个新的文件描述符，该描述符与原始描述符指向相同的文件对象（`struct file`）。复制后，两个文件描述符共享文件偏移量、文件状态标志和文件锁。

### 关键特点

- 返回当前可用的**最小**文件描述符编号
- 复制的文件描述符与原描述符共享同一个 `struct file`（不复制文件对象本身）
- 通过 `get_file()` 增加 `struct file` 的引用计数
- 不设置 `O_CLOEXEC` 标志（新 fd 在 exec 时保持打开）
- 不涉及任何文件系统或块设备操作，纯内存操作

---

## 2 涉及的内核层

| 层 | 说明 |
| --- | --- |
| **Syscall Entry** | dup 系统调用分发 (fs/file.c) |
| **fd Table** | 文件描述符表操作 (fs/file.c) |
| **VFS** | 文件引用计数管理 (fs/file_table.c) |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE1(dup) - fs/file.c:1566

```c
SYSCALL_DEFINE1(dup, unsigned int, fildes)
{
    int ret = -EBADF;                    // 默认错误码为 EBADF
    struct file *file = fget_raw(fildes); // 获取原始 fd 对应的 file 结构体

    if (file) {                          // 如果成功获取到 file 结构体
        ret = get_unused_fd_flags(0);    // 分配一个未使用的 fd 号（最小可用）
        if (ret >= 0)                    // 如果成功分配 fd 号
            fd_install(ret, file);       // 将 file 安装到新 fd 槽位
        else                             // 如果分配 fd 号失败
            fput(file);                  // 释放 file 引用（补偿 fget_raw 的引用）
    }
    return ret;                          // 返回新 fd 或 -EBADF
}
```

关键点：

- `fget_raw` 在成功时已增加 file 引用计数（`file_ref_get`），因此后续 `fd_install` 成功安装后，新 fd 持有一个引用
- 如果 `get_unused_fd_flags` 失败，需要调用 `fput` 释放 `fget_raw` 获取的引用，否则会造成引用泄漏
- `dup` 不设置 `O_CLOEXEC`，与 `dup2` 和 `dup3` 不同

---

## 4 核心辅助函数

### 4.1 fget_raw - fs/file.c:1163

```c
struct file *fget_raw(unsigned int fd)
{
    return __fget(fd, 0);  // mask = 0，不排除 FMODE_PATH 文件
}
```

与 `fget(unsigned int fd)`（使用 `FMODE_PATH` 作为 mask，排除仅路径文件）不同，`fget_raw` 使用 `mask = 0`，接受所有类型的文件，包括 `O_PATH` 打开的文件。

### 4.2 __fget - fs/file.c:1152

```c
static inline struct file *__fget(unsigned int fd, fmode_t mask)
{
    return __fget_files(current->files, fd, mask);
}
```

### 4.3 __fget_files - fs/file.c:1140

```c
static struct file *__fget_files(struct files_struct *files, unsigned int fd,
                                 fmode_t mask)
{
    struct file *file;

    rcu_read_lock();
    file = __fget_files_rcu(files, fd, mask);
    rcu_read_unlock();

    return file;
}
```

### 4.4 __fget_files_rcu - fs/file.c:1064（RCU 无锁读取）

```c
static inline struct file *__fget_files_rcu(struct files_struct *files,
       unsigned int fd, fmode_t mask)
{
    for (;;) {
        struct file *file;
        struct fdtable *fdt = rcu_dereference_raw(files->fdt);
        struct file __rcu **fdentry;
        unsigned long nospec_mask;

        // Spectre 防护：对无效 fd 返回 0，有效 fd 返回 ~0
        nospec_mask = array_index_mask_nospec(fd, fdt->max_fds);

        // 计算 fdentry 指针，无效 fd 时指向 fdt->fd[0]（安全读取）
        fdentry = fdt->fd + (fd & nospec_mask);

        // RCU 读取 file 指针，无效 fd 时结果为 NULL
        file = rcu_dereference_raw(*fdentry);
        file = (void *)(nospec_mask & (unsigned long)file);
        if (unlikely(!file))
            return NULL;

        // 尝试增加引用计数——如果 file 正在被释放则失败
        if (unlikely(!file_ref_get(&file->f_ref)))
            continue;  // 重试

        // 验证：确保 file 指针没有在并发下被篡改
        if (unlikely(file != rcu_dereference_raw(*fdentry)) ||
            unlikely(rcu_dereference_raw(files->fdt) != fdt)) {
            fput(file);   // 释放引用，重试
            continue;
        }

        // mask 检查：排除特定类型的文件（fget_raw 使用 mask=0 跳过）
        if (unlikely(file->f_mode & mask)) {
            fput(file);
            return NULL;
        }

        return file;
    }
}
```

该函数使用 RCU（Read-Copy-Update）机制实现无锁读取 fdtable，是 Linux 内核的高性能设计模式：

1. **无锁读取**：在 RCU 读锁保护下直接从 fd 数组读取 file 指针
2. **引用计数验证**：通过 `file_ref_get` 尝试增加引用计数，失败则重试（说明文件正在被并发释放）
3. **双重检查**：验证读取的 file 指针在增加引用计数前后一致，确保没有 ABA 问题
4. **Spectre 防护**：`array_index_mask_nospec` 防止 Spectre v1 变种越界访问

### 4.5 get_unused_fd_flags - fs/file.c:630

```c
int get_unused_fd_flags(unsigned flags)
{
    return __get_unused_fd_flags(flags, rlimit(RLIMIT_NOFILE));
}
EXPORT_SYMBOL(get_unused_fd_flags);
```

### 4.6 __get_unused_fd_flags - fs/file.c:625

```c
int __get_unused_fd_flags(unsigned flags, unsigned long nofile)
{
    return alloc_fd(0, nofile, flags);  // start=0, end=nofile
}
```

### 4.7 alloc_fd - fs/file.c:578（核心分配逻辑）

```c
static int alloc_fd(unsigned start, unsigned end, unsigned flags)
{
    struct files_struct *files = current->files;
    unsigned int fd;
    int error;
    struct fdtable *fdt;

    spin_lock(&files->file_lock);
repeat:
    fdt = files_fdtable(files);
    fd = start;
    if (fd < files->next_fd)
        fd = files->next_fd;  // 从 next_fd 优化起点开始查找

    if (likely(fd < fdt->max_fds))
        fd = find_next_fd(fdt, fd);  // 在位图中查找第一个空闲位

    error = -EMFILE;
    if (unlikely(fd >= end))          // 超过 RLIMIT_NOFILE 限制
        goto out;

    if (unlikely(fd >= fdt->max_fds)) {  // 需要扩容 fdtable
        error = expand_files(files, fd);
        if (error < 0)
            goto out;
        goto repeat;                    // 扩容后重新查找
    }

    if (start <= files->next_fd)
        files->next_fd = fd + 1;        // 更新优化起点

    __set_open_fd(fd, fdt, flags & O_CLOEXEC);  // 在位图中标记为已用
    error = fd;
    VFS_BUG_ON(rcu_access_pointer(fdt->fd[fd]) != NULL);  // 确保槽位为空

out:
    spin_unlock(&files->file_lock);
    return error;
}
```

关键设计：

- **next_fd 优化**：`files->next_fd` 记录了上次分配的 fd 号 + 1，作为下次查找的起点，避免每次从头扫描位图
- **find_next_fd**：使用两级位图（`open_fds` + `full_fds_bits`）加速查找
- **扩容**：当需要超过当前 fdtable 容量时，调用 `expand_files` 扩容

### 4.8 find_next_fd - fs/file.c:552

```c
static unsigned int find_next_fd(struct fdtable *fdt, unsigned int start)
{
    unsigned int maxfd = fdt->max_fds;
    unsigned int maxbit = maxfd / BITS_PER_LONG;
    unsigned int bitbit = start / BITS_PER_LONG;
    unsigned int bit;

    // 先尝试在当前 BITS_PER_LONG 块内查找
    bit = find_next_zero_bit(&fdt->open_fds[bitbit], BITS_PER_LONG,
                             start & (BITS_PER_LONG - 1));
    if (bit < BITS_PER_LONG)
        return bit + bitbit * BITS_PER_LONG;

    // 当前块全满，使用 full_fds_bits 跳过全满的块
    bitbit = find_next_zero_bit(fdt->full_fds_bits, maxbit, bitbit) * BITS_PER_LONG;
    if (bitbit >= maxfd)
        return maxfd;
    if (bitbit > start)
        start = bitbit;
    return find_next_zero_bit(fdt->open_fds, maxfd, start);
}
```

两级位图优化：`full_fds_bits` 记录哪些 `BITS_PER_LONG` 块已全部被占用，允许快速跳过全满的块，避免逐位扫描。

### 4.9 fd_install - fs/file.c:690

```c
void fd_install(unsigned int fd, struct file *file)
{
    struct files_struct *files = current->files;
    struct fdtable *fdt;

    if (WARN_ON_ONCE(unlikely(file->f_mode & FMODE_BACKING)))
        return;

    rcu_read_lock_sched();
    if (unlikely(files->resize_in_progress)) {
        rcu_read_unlock_sched();
        fd_install_slowpath(fd, file);  // 扩容中的慢路径
        return;
    }
    /* coupled with smp_wmb() in expand_fdtable() */
    smp_rmb();
    fdt = rcu_dereference_sched(files->fdt);
    VFS_BUG_ON(rcu_access_pointer(fdt->fd[fd]) != NULL);  // 确保槽位为空
    rcu_assign_pointer(fdt->fd[fd], file);  // 安装 file 指针
    rcu_read_unlock_sched();
}
```

关键设计：

- **快速路径**：没有 `resize_in_progress` 时，直接 RCU 赋值
- **慢速路径**：扩容进行中时，走 `fd_install_slowpath`（持锁安装）
- **内存屏障**：`smp_rmb()` 与 `expand_fdtable` 中的 `smp_wmb()` 配对，确保在扩容完成后看到最新的 fdt

### 4.10 __set_open_fd - fs/file.c:341

```c
static inline void __set_open_fd(unsigned int fd, struct fdtable *fdt, bool set)
{
    __set_bit(fd, fdt->open_fds);                       // 标记 fd 为已用
    __set_close_on_exec(fd, fdt, set);                  // 设置/清除 cloexec
    fd /= BITS_PER_LONG;
    if (!~fdt->open_fds[fd])                             // 如果该块全满
        __set_bit(fd, fdt->full_fds_bits);              // 标记 full_fds
}
```

### 4.11 __set_close_on_exec - fs/file.c:330

```c
static inline void __set_close_on_exec(unsigned int fd, struct fdtable *fdt,
                                       bool set)
{
    if (set) {
        __set_bit(fd, fdt->close_on_exec);              // 设置 cloexec
    } else {
        if (test_bit(fd, fdt->close_on_exec))
            __clear_bit(fd, fdt->close_on_exec);        // 清除 cloexec
    }
}
```

---

## 5 执行流程

```
dup(oldfd)
  │
  ├─ fget_raw(oldfd)                    // 获取 file 对象，增加引用计数
  │   └─ __fget(fd, 0)
  │        └─ __fget_files(files, fd, 0)
  │             └─ __fget_files_rcu(files, fd, 0)
  │                  ├─ RCU 读取 fdt->fd[fd]  // 无锁读取
  │                  ├─ file_ref_get(&file->f_ref)  // 增加引用计数
  │                  └─ 验证 file 指针一致性
  │
  ├─ [file 为空] → return -EBADF
  │
  └─ [file 非空]
       ├─ get_unused_fd_flags(0)         // 分配最小可用 fd 号
       │   └─ alloc_fd(0, nofile, 0)
       │        ├─ find_next_fd(fdt, 0)  // 查找空闲 fd
       │        ├─ expand_files(files, fd)  // 必要时扩容
       │        └─ __set_open_fd(fd, fdt, 0)  // 位图标记为已用
       │
       ├─ [ret >= 0] → fd_install(ret, file)  // 安装 file 到新 fd
       │   └─ rcu_assign_pointer(fdt->fd[ret], file)
       │
       └─ [ret < 0] → fput(file)  // 释放 fget_raw 获取的引用
            └─ __fput (异步，通过 task_work)
```

---

## 6 完整流程图

```
                    dup(oldfd)
                       |
              +--------v--------+
              |  fget_raw(fd)   |  获取 file 对象
              |  (RCU 无锁读取)  |  增加引用计数
              +--------+--------+
                       |
            +----------+----------+
            |                     |
    +-------v-------+     +-------v-------+
    | file == NULL  |     | file != NULL  |
    | return -EBADF |     +-------+-------+
    +---------------+             |
                         +--------v--------+
                         | get_unused_fd_   |
                         | flags(0)         |
                         | 分配最小可用 fd  |
                         +--------+--------+
                                  |
                     +------------+------------+
                     |                         |
             +-------v-------+         +-------v-------+
             | ret >= 0      |         | ret < 0      |
             | (分配成功)     |         | (分配失败)    |
             +-------+-------+         +-------+-------+
                     |                         |
             +-------v-------+         +-------v-------+
             | fd_install(    |         | fput(file)    |
             | ret, file)     |         | 释放引用      |
             | RCU 安装到     |         +---------------+
             | fdtable        |
             +-------+-------+
                     |
             +-------v-------+
             | return ret    |
             | (新 fd 号)    |
             +---------------+
```

---

## 7 函数调用栈

```
/* ========== dup 系统调用主路径 ========== */

SYSCALL_DEFINE1(dup, fildes)                            // fs/file.c:1566
├─ fget_raw(fildes)                                     // fs/file.c:1163
│  └─ __fget(fildes, 0)                                 // fs/file.c:1152
│       └─ __fget_files(files, fildes, 0)               // fs/file.c:1140
│            └─ __fget_files_rcu(files, fildes, 0)      // fs/file.c:1064
│                 ├─ rcu_dereference_raw(files->fdt)     // 获取 fdtable
│                 ├─ array_index_mask_nospec(fd, max_fds) // Spectre 防护
│                 ├─ rcu_dereference_raw(fdt->fd[fd])    // 读取 file 指针
│                 ├─ file_ref_get(&file->f_ref)          // 增加引用计数
│                 ├─ [验证] 检查 file 指针一致性
│                 └─ return file
│
├─ [file 非空]
│  ├─ get_unused_fd_flags(0)                            // fs/file.c:630
│  │  └─ __get_unused_fd_flags(0, RLIMIT_NOFILE)        // fs/file.c:625
│  │       └─ alloc_fd(0, nofile, 0)                    // fs/file.c:578
│  │            ├─ spin_lock(&files->file_lock)
│  │            ├─ find_next_fd(fdt, 0)                 // 查找空闲 fd
│  │            │    ├─ find_next_zero_bit(open_fds, BITS_PER_LONG, 0)
│  │            │    └─ [必要时] find_next_zero_bit(full_fds_bits, ...)
│  │            ├─ [必要时] expand_files(files, fd)      // 扩容 fdtable
│  │            │    ├─ expand_fdtable(files, nr)        // fs/file.c:252
│  │            │    │    ├─ alloc_fdtable(nr+1)         // 分配新表
│  │            │    │    ├─ copy_fdtable(new_fdt, cur_fdt) // 复制旧表
│  │            │    │    ├─ rcu_assign_pointer(files->fdt, new_fdt)
│  │            │    │    └─ call_rcu(&cur_fdt->rcu, free_fdtable_rcu)
│  │            │    └─ wake_up_all(&files->resize_wait)
│  │            ├─ __set_open_fd(fd, fdt, 0)             // 标记 fd 为已用
│  │            │    ├─ __set_bit(fd, open_fds)
│  │            │    ├─ __clear_close_on_exec(fd, fdt)   // 清除 cloexec
│  │            │    └─ [全满时] __set_bit(block, full_fds_bits)
│  │            └─ spin_unlock(&files->file_lock)
│  │
│  ├─ [ret >= 0] → fd_install(ret, file)                // fs/file.c:690
│  │    ├─ rcu_read_lock_sched()
│  │    ├─ [扩容中?] → fd_install_slowpath (慢路径)
│  │    ├─ smp_rmb()  // 与 expand_fdtable 的 smp_wmb() 配对
│  │    ├─ rcu_dereference_sched(files->fdt)
│  │    ├─ VFS_BUG_ON(fdt->fd[fd] != NULL)              // 确保槽位为空
│  │    ├─ rcu_assign_pointer(fdt->fd[fd], file)         // 安装 file
│  │    └─ rcu_read_unlock_sched()
│  │
│  └─ [ret < 0] → fput(file)                            // 释放引用
│       └─ [延迟] task_work → ____fput → __fput
│
└─ return ret
```

---

## 8 关键数据结构

### 8.1 struct files_struct（进程文件描述符表） - include/linux/fdtable.h

```c
struct files_struct {
    atomic_t count;                  // 引用计数，clone 时共享 fd 表
    bool resize_in_progress;         // fd 表扩容标志
    wait_queue_head_t resize_wait;   // 扩容等待队列
    struct fdtable __rcu *fdt;       // 指向当前 fdtable（RCU 保护）
    struct fdtable fdtab;            // 嵌入式 fdtable（默认 64 个 fd）
    spinlock_t file_lock;            // 保护 fd 表的自旋锁
    unsigned int next_fd;            // 下一个可用的 fd 号（分配优化）
    unsigned long close_on_exec_init[1];  // 初始 exec 时关闭的 fd 位图
    unsigned long open_fds_init[1];       // 初始已打开的 fd 位图
    unsigned long full_fds_bits_init[1];  // 初始 full_fds 位图
    struct file __rcu *fd_array[NR_OPEN_DEFAULT];  // 初始 fd 数组（默认 64）
};
```

> `NR_OPEN_DEFAULT`（通常为 64）个 fd 嵌入在 `files_struct` 中，避免小规模 fd 操作时的内存分配。这就是为什么大多数进程的 fd 0/1/2 不需要额外分配内存。

### 8.2 struct fdtable - include/linux/fdtable.h

```c
struct fdtable {
    unsigned int max_fds;            // fd[] 数组的最大容量
    struct file __rcu **fd;          // fd 指针数组（RCU 保护）
    unsigned long *close_on_exec;    // 位图：exec 时自动关闭的 fd
    unsigned long *open_fds;         // 位图：已分配的 fd 号
    unsigned long *full_fds_bits;    // 位图：full_fds 的优化位（加速查找）
    struct rcu_head rcu;             // RCU 回调，用于延迟释放
};
```

### 8.3 struct file（文件对象） - include/linux/fs.h

```c
struct file {
    struct path f_path;              // 文件路径（dentry + mount）
    struct inode *f_inode;           // 指向 inode 的快捷方式
    const struct file_operations *f_op;  // 文件操作函数表
    file_ref_t f_ref;                // 引用计数（关键！dup 增加此值）
    unsigned int f_flags;            // 文件状态标志
    fmode_t f_mode;                  // 打开模式
    loff_t f_pos;                    // 文件偏移量（dup 后共享！）
    void *private_data;              // 文件系统私有数据
    struct address_space *f_mapping; // 页缓存映射
    // ... 其他字段
};
```

> `dup` 复制的关键是 `file_ref_get(&file->f_ref)`，增加引用计数而不复制文件对象本身。两个 fd 共享同一个 `f_pos`，因此通过一个 fd 读写会改变另一个 fd 的偏移量。

---

## 9 错误处理

| 错误码 | 条件 | 说明 |
|--------|------|------|
| `EBADF` | `oldfd` 不是有效的打开文件描述符 | `fget_raw` 返回 NULL |
| `EMFILE` | 进程已打开的文件数达到 `RLIMIT_NOFILE` 限制 | `alloc_fd` 中 `fd >= end` |
| `ENFILE` | 系统范围内打开文件数达到上限 | 较少见，`expand_files` 可能触发 |

---

## 10 使用案例

### 示例 1：重定向标准输出到文件

```c
int fd = open("output.txt", O_WRONLY | O_CREAT, 0644);
close(STDOUT_FILENO);     // 关闭标准输出（fd 1）
dup(fd);                  // 新 fd 为 1（最小可用号），指向 output.txt
close(fd);                // 关闭原 fd
printf("This goes to file\n");  // 输出到文件
```

### 示例 2：保存标准输出副本

```c
int saved_stdout = dup(STDOUT_FILENO);  // 保存副本（fd 指向同一 file）
// ... 重定向输出 ...
dup2(saved_stdout, STDOUT_FILENO);      // 恢复
close(saved_stdout);                    // 关闭副本
```

### 示例 3：Shell 重定向实现

```c
// 模拟 shell 的 2>&1
close(STDERR_FILENO);
dup(STDOUT_FILENO);  // stderr 现在指向 stdout 的 file 对象
// 现在 stderr 和 stdout 共享同一个文件偏移量
```

---

## 11 与 dup2/dup3 的对比

| 特性 | dup | dup2 | dup3 |
|------|-----|------|------|
| 指定目标 fd | 否（最小可用） | 是 | 是 |
| 自动关闭目标 fd | 不适用 | 是 | 是 |
| 支持 O_CLOEXEC | 否 | 否 | 是 |
| oldfd == newfd 处理 | 不适用 | 返回当前 fd | 返回 -EINVAL |
| 原子性 | 是 | 是 | 是 |

---

## 12 性能特点

`dup` 是一个纯内存操作，不涉及任何 I/O：

1. **RCU 无锁读取**：`fget_raw` 使用 RCU 机制，避免锁竞争
2. **位图分配**：`alloc_fd` 使用两级位图加速空闲 fd 查找
3. **嵌入式优化**：小规模 fdtable 嵌入在 `files_struct` 中，避免内存分配
4. **无 I/O 路径**：不涉及文件系统、块设备或驱动层

---

## 13 总结

```
dup(oldfd)
  │
  ├─(1) 获取文件对象
  │   └─ fget_raw → __fget_files_rcu
  │        ├─ RCU 读取 fdtable
  │        ├─ file_ref_get 增加引用
  │        └─ 双重检查验证
  │
  ├─(2) 分配 fd 号
  │   └─ get_unused_fd_flags → alloc_fd
  │        ├─ find_next_fd 位图查找
  │        ├─ expand_files 扩容（可选）
  │        └─ __set_open_fd 标记位图
  │
  └─(3) 安装 file 对象
      └─ fd_install
           └─ rcu_assign_pointer 写入 fdtable
```

`dup` 是最简单的文件描述符复制操作，通过引用计数共享文件对象，不涉及任何文件系统操作。它的实现充分利用了 RCU 无锁读、两级位图查找和嵌入式数据结构等内核优化技术，是一个典型的高性能系统调用。