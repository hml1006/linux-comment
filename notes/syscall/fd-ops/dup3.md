# dup3 系统调用完整路径分析

## 1 概述

`dup3` 系统调用是 `dup2` 的增强版本，用于将文件描述符复制到指定编号，并支持设置 `O_CLOEXEC` 标志。与 `dup` 不同，`dup3` 允许指定目标文件描述符编号，并在目标已打开时先自动关闭。

### 关键特点

- 将 `oldfd` 复制到 `newfd`（可指定目标编号）
- 如果 `newfd` 已打开，先自动关闭（原子操作，在持锁状态下完成）
- 支持 `O_CLOEXEC` 标志：新描述符在执行 `exec()` 时自动关闭
- `oldfd` 和 `newfd` 共享同一个 `struct file`（通过引用计数共享）
- 当 `oldfd == newfd` 时返回 `-EINVAL`（与 `dup2` 不同，dup2 在此情况下返回当前 fd）

---

## 2 涉及的内核层

| 层 | 说明 |
| --- | --- |
| **Syscall Entry** | dup3 系统调用分发 (fs/file.c) |
| **fd Table** | 文件描述符表操作 (fs/file.c) |
| **VFS** | 文件引用计数管理 (fs/file_table.c) |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE3(dup3) - fs/file.c:1531

```c
SYSCALL_DEFINE3(dup3, unsigned int, oldfd, unsigned int, newfd, int, flags)
{
    return ksys_dup3(oldfd, newfd, flags);
}
```

### 3.2 ksys_dup3 - fs/file.c:1484（核心实现）

```c
static int ksys_dup3(unsigned int oldfd, unsigned int newfd, int flags)
{
    int err = -EBADF;
    struct file *file;
    struct files_struct *files = current->files;

    // 检查 flags 合法性，只允许 O_CLOEXEC
    if ((flags & ~O_CLOEXEC) != 0)
        return -EINVAL;

    // oldfd == newfd 时返回错误（dup2 在这里返回当前 fd）
    if (unlikely(oldfd == newfd))
        return -EINVAL;

    // 检查 newfd 不超过 RLIMIT_NOFILE 限制
    if (newfd >= rlimit(RLIMIT_NOFILE))
        return -EBADF;

    // 持锁操作
    spin_lock(&files->file_lock);

    // 确保 fdtable 能容纳 newfd（必要时扩容）
    err = expand_files(files, newfd);

    // 查找 oldfd 对应的 file 结构体
    file = files_lookup_fd_locked(files, oldfd);
    if (unlikely(!file))
        goto Ebadf;

    // 如果扩容失败
    if (unlikely(err < 0)) {
        if (err == -EMFILE)
            goto Ebadf;
        goto out_unlock;
    }

    // 执行实际的复制操作
    return do_dup2(files, file, newfd, flags);

Ebadf:
    err = -EBADF;
out_unlock:
    spin_unlock(&files->file_lock);
    return err;
}
```

关键点：

- `ksys_dup3` 是 `dup3` 和 `dup2` 的公共实现，`dup2` 也调用此函数但 `flags = 0`
- 在持锁状态下完成整个操作，确保原子性
- `expand_files` 在持锁时调用，但可能临时释放锁（扩容时）
- 与 `dup2` 的区别：`oldfd == newfd` 时 `dup2` 返回当前 fd，`dup3` 返回 `-EINVAL`

---

## 4 核心辅助函数

### 4.1 do_dup2 - fs/file.c:1340（核心复制逻辑）

```c
static int do_dup2(struct files_struct *files,
    struct file *file, unsigned fd, unsigned flags)
__releases(&files->file_lock)
{
    struct file *tofree;
    struct fdtable *fdt;

    /*
     * 竞态条件说明：
     * 用户空间可能在多线程下出现以下时序：
     *   fd = get_unused_fd_flags();  // fd 槽位预留，->fd[fd] == NULL
     *   file = hard_work_goes_here();
     *   fd_install(fd, file);        // 此时 ->fd[fd] 才被填充
     *
     * 如果 dup3 落在上述窗口之间，会看到 fd 已分配但槽位为 NULL 的情况。
     * 此时返回 -EBUSY 避免破坏 fd_install 的不变性。
     */
    fdt = files_fdtable(files);
    fd = array_index_nospec(fd, fdt->max_fds);  // Spectre 防护
    tofree = rcu_dereference_raw(fdt->fd[fd]);  // 读取目标槽位

    // 检测竞态：槽位为 NULL 但位图显示已分配
    if (!tofree && fd_is_open(fd, fdt))
        goto Ebusy;

    get_file(file);                              // 增加 file 引用计数
    rcu_assign_pointer(fdt->fd[fd], file);       // 安装 file 到目标槽位
    __set_open_fd(fd, fdt, flags & O_CLOEXEC);   // 更新位图，设置 cloexec
    spin_unlock(&files->file_lock);              // 释放锁

    // 在锁外关闭旧文件（如果存在）
    if (tofree)
        filp_close(tofree, files);

    return fd;

Ebusy:
    spin_unlock(&files->file_lock);
    return -EBUSY;
}
```

关键设计点：

1. **原子性**：在持锁状态下完成槽位读取、引用计数增加、安装和位图更新
2. **自动关闭**：如果目标槽位已有文件，先获取其指针（`tofree`），在安装新文件后，在锁外关闭旧文件
3. **竞态检测**：检测 `fd_install` 与 `do_dup2` 之间的竞态（槽位预留但未安装），返回 `-EBUSY`
4. **Spectre 防护**：`array_index_nospec` 防止越界访问
5. **CLOEXEC 设置**：通过 `__set_open_fd` 的第二个参数控制是否设置 `close_on_exec`

### 4.2 expand_files - fs/file.c:292

```c
static int expand_files(struct files_struct *files, unsigned int nr)
    __releases(files->file_lock)
    __acquires(files->file_lock)
{
    struct fdtable *fdt;
    int error;

repeat:
    fdt = files_fdtable(files);

    // 不需要扩容
    if (nr < fdt->max_fds)
        return 0;

    // 正在扩容中，等待
    if (unlikely(files->resize_in_progress)) {
        spin_unlock(&files->file_lock);
        wait_event(files->resize_wait, !files->resize_in_progress);
        spin_lock(&files->file_lock);
        goto repeat;
    }

    // 检查是否超过系统限制
    if (unlikely(nr >= sysctl_nr_open))
        return -EMFILE;

    // 执行扩容
    files->resize_in_progress = true;
    error = expand_fdtable(files, nr);
    files->resize_in_progress = false;
    wake_up_all(&files->resize_wait);
    return error;
}
```

### 4.3 expand_fdtable - fs/file.c:252

```c
static int expand_fdtable(struct files_struct *files, unsigned int nr)
    __releases(files->file_lock)
    __acquires(files->file_lock)
{
    struct fdtable *new_fdt, *cur_fdt;

    spin_unlock(&files->file_lock);
    new_fdt = alloc_fdtable(nr + 1);  // 分配新 fdtable（不持锁）

    // 多线程共享 fd 表时，等待所有 RCU 读者完成
    if (atomic_read(&files->count) > 1)
        synchronize_rcu();

    spin_lock(&files->file_lock);
    if (IS_ERR(new_fdt))
        return PTR_ERR(new_fdt);
    cur_fdt = files_fdtable(files);
    BUG_ON(nr < cur_fdt->max_fds);
    copy_fdtable(new_fdt, cur_fdt);              // 复制旧表内容
    rcu_assign_pointer(files->fdt, new_fdt);     // 安装新表（RCU 原子切换）

    // 异步释放旧表（RCU 宽限期后）
    if (cur_fdt != &files->fdtab)
        call_rcu(&cur_fdt->rcu, free_fdtable_rcu);
    /* coupled with smp_rmb() in fd_install() */
    smp_wmb();
    return 0;
}
```

### 4.4 files_lookup_fd_locked - include/linux/fdtable.h:88

```c
static inline struct file *files_lookup_fd_locked(
    struct files_struct *files, unsigned int fd)
{
    RCU_LOCKDEP_WARN(!lockdep_is_held(&files->file_lock),
                     "suspicious rcu_dereference_check() usage");
    return files_lookup_fd_raw(files, fd);
}
```

### 4.5 files_lookup_fd_raw - include/linux/fdtable.h:72

```c
static inline struct file *files_lookup_fd_raw(
    struct files_struct *files, unsigned int fd)
{
    struct fdtable *fdt = rcu_dereference_raw(files->fdt);
    unsigned long mask = array_index_mask_nospec(fd, fdt->max_fds);
    struct file *needs_masking;

    // 使用 array_index_mask_nospec 防止 Spectre v1
    // 无效 fd 时 mask = 0，返回 NULL
    // 有效 fd 时 mask = ~0，返回实际 file 指针
    needs_masking = rcu_dereference_raw(fdt->fd[fd & (unsigned long)mask]);
    return (struct file *)((unsigned long)mask & (unsigned long)needs_masking);
}
```

### 4.6 get_file - include/linux/file.h

```c
// 增加 file 引用计数
static inline struct file *get_file(struct file *f)
{
    // 使用 file_ref_get 原子增加引用计数
    if (unlikely(!file_ref_get(&f->f_ref)))
        // 如果引用计数已为 0，触发警告
        atomic_long_inc_not_zero(&f->f_ref);  // 实际实现
    return f;
}
```

### 4.7 filp_close - fs/open.c:1498

```c
int filp_close(struct file *filp, fl_owner_t id)
{
    int retval;

    retval = filp_flush(filp, id);  // 调用 f_op->flush（ext4 无）
    fput_close(filp);               // 延迟 fput（通过 task_work）

    return retval;
}
```

### 4.8 __set_open_fd - fs/file.c:341

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

### 4.9 fd_is_open - fs/file.c:358

```c
static inline bool fd_is_open(unsigned int fd, const struct fdtable *fdt)
{
    return test_bit(fd, fdt->open_fds);
}
```

---

## 5 执行流程

### 5.1 正常路径（newfd 未打开）

```
dup3(oldfd, newfd, flags)
  │
  ├─ [参数验证]
  │   ├─ flags 只允许 O_CLOEXEC
  │   ├─ oldfd == newfd → -EINVAL
  │   └─ newfd >= RLIMIT_NOFILE → -EBADF
  │
  ├─ spin_lock(&files->file_lock)
  │
  ├─ expand_files(files, newfd)  // 确保 fdtable 足够大
  │
  ├─ files_lookup_fd_locked(files, oldfd)  // 获取 oldfd 的 file
  │   └─ [oldfd 无效] → -EBADF
  │
  ├─ do_dup2(files, file, newfd, flags)
  │   ├─ rcu_dereference_raw(fdt->fd[newfd])  // 读取目标槽位
  │   ├─ [目标槽位为空且位图未标记] → 正常
  │   ├─ get_file(file)           // 增加引用计数
  │   ├─ rcu_assign_pointer(fdt->fd[newfd], file)  // 安装到目标槽位
  │   ├─ __set_open_fd(fd, fdt, flags & O_CLOEXEC)  // 更新位图
  │   └─ spin_unlock(&files->file_lock)
  │
  └─ return newfd
```

### 5.2 目标已打开路径（newfd 已打开）

```
dup3(oldfd, newfd, flags)
  │
  ├─ ... 参数验证 ...
  │
  ├─ do_dup2(files, file, newfd, flags)
  │   ├─ tofree = rcu_dereference_raw(fdt->fd[newfd])  // 保存旧 file 指针
  │   ├─ get_file(file)           // 增加 oldfd 对应 file 的引用
  │   ├─ rcu_assign_pointer(fdt->fd[newfd], file)  // 覆盖为新 file
  │   ├─ __set_open_fd(fd, fdt, flags & O_CLOEXEC)  // 更新位图
  │   ├─ spin_unlock(&files->file_lock)  // 释放锁
  │   │
  │   └─ [tofree 非空]
  │        └─ filp_close(tofree, files)  // 在锁外关闭旧文件
  │             ├─ filp_flush(tofree, id)  // f_op->flush
  │             └─ fput_close(tofree)      // 延迟 fput
  │
  └─ return newfd
```

### 5.3 竞态路径（fd_install 窗口命中）

```
dup3(oldfd, newfd, flags)
  │
  ├─ do_dup2(files, file, newfd, flags)
  │   ├─ tofree = rcu_dereference_raw(fdt->fd[newfd])
  │   ├─ tofree == NULL 但 fd_is_open(newfd) == true  // 竞态！
  │   │   // 另一个线程刚刚调用了 get_unused_fd_flags 分配了 newfd
  │   │   // 但尚未调用 fd_install 填充槽位
  │   └─ goto Ebusy → 返回 -EBUSY
  │
  └─ return -EBUSY
```

---

## 6 完整流程图

```
                    dup3(oldfd, newfd, flags)
                           |
                  +--------v--------+
                  |  参数验证        |
                  |  flags 仅 O_CLOEXEC |
                  |  oldfd != newfd  |
                  |  newfd < NOFILE  |
                  +--------+--------+
                           |
                  +--------v--------+
                  | spin_lock       |
                  | (file_lock)     |
                  +--------+--------+
                           |
                  +--------v--------+
                  | expand_files    |
                  | (扩容 fdtable)   |
                  +--------+--------+
                           |
                  +--------v--------+
                  | files_lookup_fd_|
                  | locked(oldfd)   |
                  +--------+--------+
                           |
              +-----------+-----------+
              |                       |
      +-------v--------+     +-------v--------+
      | file == NULL   |     | file != NULL   |
      | goto Ebadf     |     +-------+--------+
      +--------+-------+             |
               |             +-------v--------+
               |             | do_dup2(files, |
               |             | file, newfd,   |
               |             | flags)         |
               |             +-------+--------+
               |                     |
               |            +--------v--------+
               |            | tofree =        |
               |            | fdt->fd[newfd]  |
               |            +--------+--------+
               |                     |
               |            +--------v--------+
               |            | [竞态检测]       |
               |            | !tofree &&      |
               |            | fd_is_open?     |
               |            +--------+--------+
               |                     |
               |        +------------+------------+
               |        |                         |
               |   +----v----+            +-------v--------+
               |   | 是      |            | 否              |
               |   | -EBUSY  |            +-------+--------+
               |   +---------+                    |
               |                        +--------v--------+
               |                        | get_file(file)  |
               |                        | 增加引用计数    |
               |                        +--------+--------+
               |                                 |
               |                        +--------v--------+
               |                        | rcu_assign_ptr  |
               |                        | fdt->fd[newfd]  |
               |                        | = file          |
               |                        +--------+--------+
               |                                 |
               |                        +--------v--------+
               |                        | __set_open_fd   |
               |                        | 更新位图        |
               |                        | 设置 cloexec    |
               |                        +--------+--------+
               |                                 |
               |                        +--------v--------+
               |                        | spin_unlock     |
               |                        +--------+--------+
               |                                 |
               |                        +--------v--------+
               |                        | [tofree 非空]   |
               |                        | filp_close      |
               |                        | (锁外关闭旧文件) |
               |                        +--------+--------+
               |                                 |
               +------------+--------------------+
                            |
                   +--------v--------+
                   | return newfd    |
                   | (或错误码)      |
                   +-----------------+
```

---

## 7 函数调用栈

```
/* ========== dup3 系统调用主路径 ========== */

SYSCALL_DEFINE3(dup3, oldfd, newfd, flags)              // fs/file.c:1531
└─ ksys_dup3(oldfd, newfd, flags)                       // fs/file.c:1484
     ├─ [参数验证]
     │   ├─ flags & ~O_CLOEXEC → return -EINVAL
     │   ├─ oldfd == newfd → return -EINVAL
     │   └─ newfd >= rlimit(RLIMIT_NOFILE) → return -EBADF
     │
     ├─ spin_lock(&files->file_lock)
     │
     ├─ expand_files(files, newfd)                      // fs/file.c:292
     │   ├─ [需要扩容]
     │   │   ├─ expand_fdtable(files, nr)               // fs/file.c:252
     │   │   │    ├─ spin_unlock(&files->file_lock)
     │   │   │    ├─ alloc_fdtable(nr + 1)              // 分配新表
     │   │   │    │    └─ kmalloc + kcalloc 分配位图和 fd 数组
     │   │   │    ├─ [共享 fd 表] synchronize_rcu()
     │   │   │    ├─ spin_lock(&files->file_lock)
     │   │   │    ├─ copy_fdtable(new_fdt, cur_fdt)     // 复制内容
     │   │   │    ├─ rcu_assign_pointer(files->fdt, new_fdt) // 安装新表
     │   │   │    ├─ call_rcu(&cur_fdt->rcu, free_fdtable_rcu) // 释放旧表
     │   │   │    └─ smp_wmb()  // 内存屏障
     │   │   └─ wake_up_all(&files->resize_wait)
     │   └─ [不需要扩容] return 0
     │
     ├─ files_lookup_fd_locked(files, oldfd)            // include/linux/fdtable.h:88
     │   └─ files_lookup_fd_raw(files, oldfd)           // include/linux/fdtable.h:72
     │        ├─ rcu_dereference_raw(files->fdt)
     │        ├─ array_index_mask_nospec(fd, max_fds)    // Spectre 防护
     │        └─ rcu_dereference_raw(fdt->fd[fd])
     │
     ├─ [file == NULL] → goto Ebadf → -EBADF
     │
     ├─ [err < 0] → goto out_unlock
     │
     └─ do_dup2(files, file, newfd, flags)              // fs/file.c:1340
          ├─ array_index_nospec(fd, fdt->max_fds)        // Spectre 防护
          ├─ tofree = rcu_dereference_raw(fdt->fd[fd])   // 读取目标槽位
          ├─ [竞态检测] !tofree && fd_is_open(fd, fdt)
          │   └─ goto Ebusy → spin_unlock → return -EBUSY
          ├─ get_file(file)                              // 增加引用计数
          │   └─ file_ref_get(&file->f_ref)
          ├─ rcu_assign_pointer(fdt->fd[fd], file)       // 安装 file
          ├─ __set_open_fd(fd, fdt, flags & O_CLOEXEC)   // 更新位图
          │   ├─ __set_bit(fd, fdt->open_fds)
          │   ├─ __set_close_on_exec(fd, fdt, set)
          │   │   ├─ [set] __set_bit(fd, close_on_exec)
          │   │   └─ [!set] __clear_bit(fd, close_on_exec)
          │   └─ [全满] __set_bit(fd/BITS_PER_LONG, full_fds_bits)
          ├─ spin_unlock(&files->file_lock)
          │
          └─ [tofree 非空]
               └─ filp_close(tofree, files)             // fs/open.c:1498
                    ├─ filp_flush(tofree, id)            // fs/open.c:1462
                    │   ├─ f_op->flush(filp, id)         // ext4 无，跳过
                    │   ├─ dnotify_flush(filp, id)       // dnotify 清理
                    │   └─ locks_remove_posix(filp, id)  // POSIX 锁清理
                    └─ fput_close(tofree)                // fs/file_table.c
                         └─ [延迟] task_work → ____fput → __fput
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

### 8.2 struct fdtable - include/linux/fdtable.h

```c
struct fdtable {
    unsigned int max_fds;            // fd[] 数组的最大容量
    struct file __rcu **fd;          // fd 指针数组（RCU 保护）
    unsigned long *close_on_exec;    // 位图：exec 时自动关闭的 fd
    unsigned long *open_fds;         // 位图：已分配的 fd 号
    unsigned long *full_fds_bits;    // 位图：full_fds 的优化位
    struct rcu_head rcu;             // RCU 回调，用于延迟释放
};
```

### 8.3 struct file（文件对象） - include/linux/fs.h

```c
struct file {
    struct path f_path;              // 文件路径（dentry + mount）
    struct inode *f_inode;           // 指向 inode 的快捷方式
    const struct file_operations *f_op;  // 文件操作函数表
    file_ref_t f_ref;                // 引用计数（dup3 增加此值）
    unsigned int f_flags;            // 文件状态标志
    fmode_t f_mode;                  // 打开模式
    loff_t f_pos;                    // 文件偏移量（共享！）
    void *private_data;              // 文件系统私有数据
    struct address_space *f_mapping; // 页缓存映射
    // ... 其他字段
};
```

### 8.4 O_CLOEXEC 标志定义 - include/uapi/linux/fcntl.h

```c
#define O_CLOEXEC   02000000    // 设置 close_on_exec 标志
```

---

## 9 错误处理

| 错误码 | 条件 | 说明 |
|--------|------|------|
| `EBADF` | `oldfd` 不是有效的打开文件描述符 | `files_lookup_fd_locked` 返回 NULL |
| `EBADF` | `newfd >= RLIMIT_NOFILE` | 目标 fd 超过进程限制 |
| `EINVAL` | `flags` 包含 `O_CLOEXEC` 以外的位 | 只允许 `O_CLOEXEC` 标志 |
| `EINVAL` | `oldfd == newfd` | 与 `dup2` 不同，`dup3` 禁止相同 fd |
| `EMFILE` | `newfd >= sysctl_nr_open` | 超过系统最大文件数限制 |
| `EBUSY` | 目标 fd 正在被并发分配 | 多线程竞态，另一个线程在 `get_unused_fd_flags` 和 `fd_install` 之间 |

---

## 10 使用案例

### 示例 1：Shell 重定向 stderr -> stdout

```c
// 将 stderr 重定向到 stdout（带 O_CLOEXEC）
dup3(STDOUT_FILENO, STDERR_FILENO, O_CLOEXEC);
// 现在 stderr 和 stdout 指向同一个 file 对象
// exec() 时不会继承这两个 fd
```

### 示例 2：创建带 O_CLOEXEC 的管道副本

```c
int p[2];
pipe2(p, O_CLOEXEC);  // 创建带 cloexec 的管道

// 复制管道读端到 fd 10，带 O_CLOEXEC
int newfd = dup3(p[0], 10, O_CLOEXEC);
close(p[0]);  // 关闭原 fd

// 现在 fd 10 是管道读端，exec() 时自动关闭
```

### 示例 3：原子重定向到文件

```c
int fd = open("log.txt", O_WRONLY | O_CREAT, 0644);

// 原子操作：将 fd 复制到 STDOUT_FILENO
// 如果 STDOUT_FILENO 已打开，自动关闭
// 设 O_CLOEXEC 防止 exec 后泄漏
dup3(fd, STDOUT_FILENO, O_CLOEXEC);

close(fd);  // 关闭原 fd
```

### 示例 4：与 dup2 的对比

```c
// dup2 允许 oldfd == newfd，返回当前 fd
int ret = dup2(5, 5);  // ret = 5（如果 fd 5 有效）

// dup3 禁止 oldfd == newfd，返回 -EINVAL
int ret = dup3(5, 5, 0);  // ret = -EINVAL, errno = EINVAL
```

---

## 11 与 dup/dup2 的对比

| 特性 | dup | dup2 | dup3 |
|------|-----|------|------|
| 指定目标 fd | 否（最小可用） | 是 | 是 |
| 自动关闭目标 fd | 不适用 | 是 | 是 |
| 支持 O_CLOEXEC | 否 | 否 | 是 |
| oldfd == newfd 处理 | 不适用 | 返回当前 fd | 返回 -EINVAL |
| 原子性 | 是 | 是 | 是 |
| 实现方式 | `fget_raw` + `alloc_fd` + `fd_install` | 调用 `ksys_dup3` | 直接调用 `ksys_dup3` |
| 内核版本 | 一直存在 | 一直存在 | Linux 2.6.27+ |

---

## 12 性能特点

`dup3` 是一个纯内存操作，不涉及任何 I/O：

1. **持锁原子操作**：在 `file_lock` 保护下完成所有 fdtable 操作
2. **RCU 读取**：`files_lookup_fd_raw` 使用 RCU 读取 fdtable
3. **延迟关闭**：旧文件在锁外通过 `filp_close` 关闭，避免长时间持锁
4. **扩容优化**：`expand_fdtable` 在扩容时释放锁，避免阻塞其他线程

---

## 13 总结

```
dup3(oldfd, newfd, flags)
  │
  ├─(1) 参数验证
  │   ├─ flags 仅允许 O_CLOEXEC
  │   ├─ oldfd != newfd
  │   └─ newfd < RLIMIT_NOFILE
  │
  ├─(2) 持锁操作
  │   ├─ expand_files 扩容 fdtable（可选）
  │   ├─ files_lookup_fd_locked 获取 oldfd
  │   └─ do_dup2 核心复制
  │        ├─ 读取目标槽位（竞态检测）
  │        ├─ get_file 增加引用
  │        ├─ 安装 file 到目标槽位
  │        ├─ 更新位图 + cloexec
  │        └─ 释放锁
  │
  └─(3) 锁外操作
       └─ filp_close 关闭旧文件（如果有）
```

`dup3` 是 `dup2` 的增强版本，主要增加了 `O_CLOEXEC` 标志支持。它在持锁状态下原子地完成文件描述符的复制、替换和位图更新，确保多线程环境下的安全性。与 `dup2` 的关键区别是 `oldfd == newfd` 时返回 `-EINVAL`，以及支持 `O_CLOEXEC` 标志设置。