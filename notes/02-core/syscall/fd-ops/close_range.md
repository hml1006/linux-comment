# close_range 系统调用完整路径分析

## 1 概述

`close_range` 系统调用（Linux 5.9+）用于批量关闭指定范围内的文件描述符，支持 `CLOSE_RANGE_UNSHARE` 和 `CLOSE_RANGE_CLOEXEC` 标志。相比逐个调用 `close()`，`close_range` 只需一次系统调用即可完成批量操作，显著减少系统调用开销。

### 关键特点

- 批量关闭 [fd, max_fd] 范围内的所有文件描述符
- `CLOSE_RANGE_UNSHARE`：解除共享 fd 表后再关闭，用于多线程安全
- `CLOSE_RANGE_CLOEXEC`：设置 `close_on_exec` 位图，而非真正关闭文件
- 内部使用位图（`open_fds`）遍历打开的描述符，避免遍历所有 fd 号
- 关闭每个 fd 时释放文件锁并调用 `cond_resched()`，避免长时间持锁

---

## 2 涉及的内核层

| 层 | 说明 |
| --- | --- |
| **Syscall Entry** | close_range 系统调用分发 (fs/file.c) |
| **fd Table** | 文件描述符表操作 (fs/file.c) |
| **VFS** | 文件结构和 dentry 释放 (fs/file_table.c, fs/dcache.c) |
| **ext4** | ext4_release_file / 延迟分配回写 (fs/ext4/file.c) |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE3(close_range) - fs/file.c:854

```c
SYSCALL_DEFINE3(close_range, unsigned int, fd, unsigned int, max_fd,
                unsigned int, flags)
{
    struct task_struct *me = current;
    struct files_struct *cur_fds = me->files, *fds = NULL;

    // 检查 flags 参数，只允许 CLOSE_RANGE_UNSHARE 和 CLOSE_RANGE_CLOEXEC
    if (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC))
        return -EINVAL;

    // 检查 fd 范围有效性
    if (fd > max_fd)
        return -EINVAL;

    // 处理 UNSHARE 标志：如果 fd 表被共享，则复制一份私有表
    if ((flags & CLOSE_RANGE_UNSHARE) && atomic_read(&cur_fds->count) > 1) {
        struct fd_range range = {fd, max_fd}, *punch_hole = &range;

        // 如果同时设置 CLOEXEC，则复制所有 fd（不"打孔"）
        if (flags & CLOSE_RANGE_CLOEXEC)
            punch_hole = NULL;

        // 复制 fd 表，punch_hole 指定不复制范围内的 fd
        fds = dup_fd(cur_fds, punch_hole);
        if (IS_ERR(fds))
            return PTR_ERR(fds);
        swap(cur_fds, fds);  // 切换到新 fd 表
    }

    // 根据 flags 执行 cloexec 或直接关闭操作
    if (flags & CLOSE_RANGE_CLOEXEC)
        __range_cloexec(cur_fds, fd, max_fd);
    else
        __range_close(cur_fds, fd, max_fd);

    // 如果创建了新 fd 表，安装并释放旧表
    if (fds) {
        task_lock(me);
        me->files = cur_fds;
        task_unlock(me);
        put_files_struct(fds);
    }

    return 0;
}
```

关键点：

- `dup_fd` 的 `punch_hole` 机制：复制 fd 表时，不在 [fd, max_fd] 范围内的 fd 会被复制到新表，范围内的 fd 在复制时被跳过（相当于提前关闭），避免后续再次遍历关闭
- `CLOSE_RANGE_CLOEXEC` 和 `CLOSE_RANGE_UNSHARE` 可以同时使用，此时先解除共享再设置 cloexec
- 该函数始终返回 0，关闭单个 fd 的错误被忽略

---

## 4 核心辅助函数

### 4.1 __range_cloexec - fs/file.c:787

```c
static inline void __range_cloexec(struct files_struct *cur_fds,
                                   unsigned int fd, unsigned int max_fd)
{
    struct fdtable *fdt;

    spin_lock(&cur_fds->file_lock);
    fdt = files_fdtable(cur_fds);
    // 限制 max_fd 不超过 fdtable 的最大值
    max_fd = min(last_fd(fdt), max_fd);
    if (fd <= max_fd)
        // 批量设置 close_on_exec 位图
        bitmap_set(fdt->close_on_exec, fd, max_fd - fd + 1);
    spin_unlock(&cur_fds->file_lock);
}
```

该函数仅操作位图，不涉及文件对象。`bitmap_set` 是内核位图操作函数，批量设置指定范围内的位。`last_fd(fdt)` 返回 `fdt->max_fds - 1`。

### 4.2 __range_close - fs/file.c:801

```c
static inline void __range_close(struct files_struct *files, unsigned int fd,
                                 unsigned int max_fd)
{
    struct file *file;
    struct fdtable *fdt;
    unsigned n;

    spin_lock(&files->file_lock);
    fdt = files_fdtable(files);
    n = last_fd(fdt);
    max_fd = min(max_fd, n);  // 限制范围不超过当前 fdtable 的最大值

    // 遍历位图中 [fd, max_fd] 范围内所有打开的 fd
    for (fd = find_next_bit(fdt->open_fds, max_fd + 1, fd);
         fd <= max_fd;
         fd = find_next_bit(fdt->open_fds, max_fd + 1, fd + 1)) {
        // 从 fdtable 移除 fd（持锁操作）
        file = file_close_fd_locked(files, fd);
        if (file) {
            spin_unlock(&files->file_lock);
            filp_close(file, files);  // 释放文件，可能触发 __fput
            cond_resched();           // 主动让出 CPU，避免长时间持锁
            spin_lock(&files->file_lock);
            fdt = files_fdtable(files);  // 重新获取 fdt（可能已扩容）
        } else if (need_resched()) {
            spin_unlock(&files->file_lock);
            cond_resched();
            spin_lock(&files->file_lock);
            fdt = files_fdtable(files);
        }
    }
    spin_unlock(&files->file_lock);
}
```

关键设计点：

1. **位图遍历**：使用 `find_next_bit` 在位图中查找下一个打开的 fd，避免遍历所有 fd 号
2. **锁释放**：每次关闭文件时释放 `file_lock`，因为 `filp_close` 可能触发较重的文件释放操作
3. **可抢占**：通过 `cond_resched()` 提供抢占点，防止批量关闭大量 fd 时导致调度延迟
4. **fdtable 刷新**：重新获取 `fdt` 指针，因为在释放锁期间可能发生 fdtable 扩容

### 4.3 file_close_fd_locked - fs/file.c:737

```c
struct file *file_close_fd_locked(struct files_struct *files, unsigned fd)
{
    struct fdtable *fdt = files_fdtable(files);
    struct file *file;

    lockdep_assert_held(&files->file_lock);  // 确保持锁

    if (fd >= fdt->max_fds)
        return NULL;

    fd = array_index_nospec(fd, fdt->max_fds);  // 防止 Spectre 越界
    file = rcu_dereference_raw(fdt->fd[fd]);    // 获取 file 指针
    if (file) {
        rcu_assign_pointer(fdt->fd[fd], NULL);  // 清空 fd 槽位
        __put_unused_fd(files, fd);             // 回收 fd 号（位图清除）
    }
    return file;
}
```

### 4.4 filp_close - fs/open.c:1498

```c
int filp_close(struct file *filp, fl_owner_t id)
{
    int retval;

    retval = filp_flush(filp, id);  // 调用 f_op->flush（ext4 无）
    fput_close(filp);               // 延迟 fput（通过 task_work）

    return retval;
}
```

与 `SYSCALL_DEFINE1(close)` 不同，`close_range` 使用 `fput_close`（延迟释放）而非 `fput_close_sync`（同步释放）。这是因为 `close_range` 的调用者可能不是立即返回用户态，且在批量场景下延迟释放可以累积多个 task_work 统一处理。

### 4.5 dup_fd（UNSHARE 路径的核心） - fs/file.c:391

```c
struct files_struct *dup_fd(struct files_struct *oldf, struct fd_range *punch_hole)
{
    struct files_struct *newf;
    struct file **old_fds, **new_fds;
    unsigned int open_files, i;
    struct fdtable *old_fdt, *new_fdt;

    newf = kmem_cache_alloc(files_cachep, GFP_KERNEL);
    if (!newf)
        return ERR_PTR(-ENOMEM);

    atomic_set(&newf->count, 1);
    spin_lock_init(&newf->file_lock);
    newf->resize_in_progress = false;
    init_waitqueue_head(&newf->resize_wait);
    newf->next_fd = 0;

    // 初始化使用嵌入式 fdtable（最多 NR_OPEN_DEFAULT 个 fd）
    new_fdt = &newf->fdtab;
    new_fdt->max_fds = NR_OPEN_DEFAULT;
    new_fdt->close_on_exec = newf->close_on_exec_init;
    new_fdt->open_fds = newf->open_fds_init;
    new_fdt->full_fds_bits = newf->full_fds_bits_init;
    new_fdt->fd = &newf->fd_array[0];

    spin_lock(&oldf->file_lock);
    old_fdt = files_fdtable(oldf);
    open_files = sane_fdtable_size(old_fdt, punch_hole);  // 考虑 punch_hole

    // 如果 open_files 超过嵌入式大小，分配更大的 fdtable
    while (unlikely(open_files > new_fdt->max_fds)) {
        spin_unlock(&oldf->file_lock);
        if (new_fdt != &newf->fdtab)
            __free_fdtable(new_fdt);
        new_fdt = alloc_fdtable(open_files);
        if (IS_ERR(new_fdt)) {
            kmem_cache_free(files_cachep, newf);
            return ERR_CAST(new_fdt);
        }
        spin_lock(&oldf->file_lock);
        old_fdt = files_fdtable(oldf);
        open_files = sane_fdtable_size(old_fdt, punch_hole);
    }

    // 复制位图
    copy_fd_bitmaps(new_fdt, old_fdt, open_files / BITS_PER_LONG);

    // 复制 fd 数组
    old_fds = old_fdt->fd;
    new_fds = new_fdt->fd;
    for (i = open_files; i != 0; i--) {
        struct file *f = rcu_dereference_raw(*old_fds++);
        if (f) {
            get_file(f);          // 增加 file 引用计数
        } else {
            __clear_open_fd(open_files - i, new_fdt);  // 清理未使用的位
        }
        rcu_assign_pointer(*new_fds++, f);
    }
    spin_unlock(&oldf->file_lock);

    // 清空剩余部分
    memset(new_fds, 0, (new_fdt->max_fds - open_files) * sizeof(struct file *));

    rcu_assign_pointer(newf->fdt, new_fdt);
    return newf;
}
```

`sane_fdtable_size` 函数（fs/file.c:372）在 `punch_hole` 非空时，会计算 `punch_hole` 范围内的 fd 数，使得新表省略这些 fd——这就是"打孔"语义：UNSHARE 时，范围内的 fd 不会复制到新表，相当于提前关闭。

### 4.6 __put_unused_fd - fs/file.c:636

```c
static void __put_unused_fd(struct files_struct *files, unsigned int fd)
{
    struct fdtable *fdt = files_fdtable(files);
    __clear_open_fd(fd, fdt);       // 在位图中清除 fd 标记
    if (fd < files->next_fd)
        files->next_fd = fd;        // 更新 next_fd 优化值
}
```

### 4.7 __clear_open_fd - fs/file.c:350

```c
static inline void __clear_open_fd(unsigned int fd, struct fdtable *fdt)
{
    __clear_bit(fd, fdt->open_fds);          // 清除 open_fds 位
    fd /= BITS_PER_LONG;
    if (test_bit(fd, fdt->full_fds_bits))
        __clear_bit(fd, fdt->full_fds_bits); // 清除 full_fds_bits 位
}
```

---

## 5 执行流程

### 5.1 普通关闭模式（flags = 0）

```
close_range(fd, max_fd, 0)
  │
  ├─ [参数验证]
  │   ├─ flags 检查（无非法位）
  │   └─ fd <= max_fd 检查
  │
  ├─ [UNSHARE 检查] cur_fds->count > 1? 否，跳过
  │
  └─ __range_close(cur_fds, fd, max_fd)
       ├─ spin_lock(&files->file_lock)
       ├─ max_fd = min(max_fd, last_fd(fdt))
       ├─ [遍历 open_fds 位图]:
       │   ├─ fd = find_next_bit(open_fds, max_fd+1, fd)
       │   ├─ file_close_fd_locked(files, fd)  // 移除 fd
       │   │   ├─ rcu_assign_pointer(fdt->fd[fd], NULL)
       │   │   └─ __put_unused_fd(files, fd)
       │   ├─ filp_close(file, files)          // 释放文件
       │   │   ├─ filp_flush(file, id)         // f_op->flush
       │   │   └─ fput_close(file)             // 延迟 fput
       │   └─ cond_resched()                   // 让出 CPU
       └─ spin_unlock(&files->file_lock)
```

### 5.2 UNSHARE 模式（flags = CLOSE_RANGE_UNSHARE）

```
close_range(fd, max_fd, CLOSE_RANGE_UNSHARE)
  │
  ├─ [参数验证] 通过
  │
  ├─ [UNSHARE 检查] cur_fds->count > 1? 是
  │   └─ dup_fd(cur_fds, &range)  // punch_hole = [fd, max_fd]
  │        ├─ 分配新的 files_struct
  │        ├─ sane_fdtable_size(old_fdt, &range)  // 计算排除 punch_hole 后的大小
  │        ├─ 复制位图（排除 punch_hole 范围的 fd）
  │        ├─ 复制 fd 数组（排除 punch_hole 范围的 file 指针）
  │        │   └─ 对每个复制的 file 调用 get_file() 增加引用
  │        └─ 返回新 files_struct
  │
  ├─ swap(cur_fds, fds)  // 切换到新 fd 表
  │
  ├─ __range_close(cur_fds, fd, max_fd)  // 在新表上关闭（此时已无共享 fd）
  │
  └─ [安装新表]
      ├─ task_lock(me)
      ├─ me->files = cur_fds
      ├─ task_unlock(me)
      └─ put_files_struct(fds)  // 释放旧表
```

### 5.3 CLOEXEC 模式（flags = CLOSE_RANGE_CLOEXEC）

```
close_range(fd, max_fd, CLOSE_RANGE_CLOEXEC)
  │
  ├─ [参数验证] 通过
  │
  ├─ [UNSHARE 检查] 跳过（或同时执行 UNSHARE）
  │
  └─ __range_cloexec(cur_fds, fd, max_fd)
       ├─ spin_lock(&cur_fds->file_lock)
       ├─ max_fd = min(last_fd(fdt), max_fd)
       └─ bitmap_set(fdt->close_on_exec, fd, max_fd - fd + 1)
            └─ spin_unlock(&cur_fds->file_lock)
```

### 5.4 UNSHARE + CLOEXEC 组合模式

```
close_range(fd, max_fd, CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)
  │
  ├─ [参数验证] 通过
  │
  ├─ [UNSHARE] cur_fds->count > 1? 是
  │   └─ dup_fd(cur_fds, NULL)  // punch_hole = NULL，复制所有 fd
  │        └─ 复制所有 fd（不"打孔"），因为调用者仍要使用它们
  │
  ├─ swap(cur_fds, fds)
  │
  ├─ __range_cloexec(cur_fds, fd, max_fd)  // 设置 close_on_exec 位
  │
  └─ [安装新表] 同上
```

---

## 6 完整流程图

```
                          close_range(fd, max_fd, flags)
                                     |
                            +--------v--------+
                            |  参数验证        |
                            |  flags 合法性    |
                            |  fd <= max_fd   |
                            +--------+--------+
                                     |
                   +--------+--------+--------+
                   |        |                  |
                   |  [UNSHARE 标志且 count>1] |
                   |        |                  |
                   +--------v--------+         |
                   |  dup_fd()       |         |
                   |  (punch_hole)   |         |
                   +--------+--------+         |
                            |                  |
                   +--------v--------+         |
                   | swap(cur_fds,   |         |
                   | fds)            |         |
                   +--------+--------+         |
                            |                  |
                   +--------+--------+--------+
                   |        |                  |
          +--------v--------+       +---------v--------+
          | flags & CLOEXEC |       | else             |
          +--------+--------+       +---------+--------+
                   |                          |
          +--------v--------+       +---------v--------+
          | __range_cloexec |       | __range_close    |
          | bitmap_set()    |       | 遍历位图关闭 fd  |
          +--------+--------+       +---------+--------+
                   |                          |
                   +--------+--------+--------+
                            |
                   +--------v--------+
                   | [如果创建了新表]  |
                   | task_lock       |
                   | me->files = cur |
                   | task_unlock     |
                   | put_files_struct|
                   +--------+--------+
                            |
                   +--------v--------+
                   |  return 0       |
                   +-----------------+
```

---

## 7 函数调用栈

```
/* ========== 主路径：__range_close（默认关闭） ========== */

SYSCALL_DEFINE3(close_range, fd, max_fd, flags)         // fs/file.c:854
├─ [条件: UNSHARE && count > 1]
│  └─ dup_fd(cur_fds, punch_hole)                        // fs/file.c:391 — 复制 fd 表
│       ├─ kmem_cache_alloc(files_cachep)                // 分配新 files_struct
│       ├─ sane_fdtable_size(old_fdt, punch_hole)        // 计算需要的大小
│       ├─ alloc_fdtable(open_files)                     // 分配 fdtable（如需要）
│       ├─ copy_fd_bitmaps(new_fdt, old_fdt, ...)        // 复制位图
│       ├─ 遍历 fd 数组:
│       │   ├─ get_file(f)                               // 增加 file 引用计数
│       │   └─ rcu_assign_pointer(new_fds++, f)          // 复制到新表
│       └─ return newf
│
├─ __range_close(cur_fds, fd, max_fd)                     // fs/file.c:801
│  ├─ spin_lock(&files->file_lock)
│  ├─ max_fd = min(max_fd, last_fd(fdt))
│  ├─ [循环: find_next_bit 遍历 open_fds 位图]
│  │  ├─ file_close_fd_locked(files, fd)                 // fs/file.c:737
│  │  │  ├─ array_index_nospec(fd, fdt->max_fds)         // Spectre 防护
│  │  │  ├─ rcu_dereference_raw(fdt->fd[fd])             // 获取 file 指针
│  │  │  ├─ rcu_assign_pointer(fdt->fd[fd], NULL)        // 清空槽位
│  │  │  └─ __put_unused_fd(files, fd)                   // 回收 fd 号
│  │  │       ├─ __clear_open_fd(fd, fdt)                // 清除位图
│  │  │       └─ 更新 next_fd
│  │  ├─ spin_unlock(&files->file_lock)
│  │  ├─ filp_close(file, files)                         // fs/open.c:1498
│  │  │  ├─ filp_flush(file, id)                         // fs/open.c:1462
│  │  │  │  ├─ f_op->flush(filp, id)                     // ext4 无，跳过
│  │  │  │  ├─ dnotify_flush(filp, id)                   // dnotify 清理
│  │  │  │  └─ locks_remove_posix(filp, id)              // POSIX 锁清理
│  │  │  └─ fput_close(file)                             // fs/file_table.c
│  │  │       └─ [延迟] task_work → ____fput → __fput    // 异步释放
│  │  ├─ cond_resched()                                  // 让出 CPU
│  │  └─ spin_lock(&files->file_lock)                    // 重新加锁
│  └─ spin_unlock(&files->file_lock)
│
├─ [条件: 创建了新表]
│  ├─ task_lock(me)
│  ├─ me->files = cur_fds
│  ├─ task_unlock(me)
│  └─ put_files_struct(fds)                              // 释放旧表
│       └─ atomic_dec_and_test(&files->count) → 0
│            └─ close_files(fdt)                         // 关闭旧表中剩余文件
│                 └─ 遍历位图 → filp_close → fput_close
│
└─ return 0


/* ========== 路径 2: __range_cloexec（设置 CLOEXEC） ========== */

__range_cloexec(cur_fds, fd, max_fd)                     // fs/file.c:787
├─ spin_lock(&cur_fds->file_lock)
├─ max_fd = min(last_fd(fdt), max_fd)
├─ bitmap_set(fdt->close_on_exec, fd, max_fd - fd + 1)   // 批量设置位图
└─ spin_unlock(&cur_fds->file_lock)


/* ========== 路径 3: UNSHARE + CLOEXEC 组合 ========== */

// 与路径 1 类似，但:
// 1. dup_fd 时 punch_hole = NULL（复制所有 fd）
// 2. 后续调用 __range_cloexec 而非 __range_close
```

---

## 8 关键数据结构

### 8.1 标志位定义 - include/uapi/linux/close_range.h

```c
#define CLOSE_RANGE_UNSHARE  (1U << 1)   // 解除共享 fd 表再关闭
#define CLOSE_RANGE_CLOEXEC  (1U << 2)   // 设置 close_on_exec 而非关闭
```

### 8.2 struct fd_range - include/linux/fdtable.h:104

```c
struct fd_range {
    unsigned int from, to;   // 范围 [from, to]，用于 dup_fd 的 punch_hole
};
```

### 8.3 struct files_struct（进程文件描述符表） - include/linux/fdtable.h

```c
struct files_struct {
    atomic_t count;                  // 引用计数，clone 时共享 fd 表
    bool resize_in_progress;         // fd 表扩容标志
    wait_queue_head_t resize_wait;   // 扩容等待队列
    struct fdtable __rcu *fdt;       // 指向当前 fdtable（RCU 保护，可动态扩容）
    struct fdtable fdtab;            // 嵌入式 fdtable（默认 64 个 fd）
    spinlock_t file_lock;            // 保护 fd 表的自旋锁
    unsigned int next_fd;            // 下一个可用的 fd 号（分配优化）
    unsigned long close_on_exec_init[1];  // 初始 exec 时关闭的 fd 位图
    unsigned long open_fds_init[1];       // 初始已打开的 fd 位图
    unsigned long full_fds_bits_init[1];  // 初始 full_fds 位图
    struct file __rcu *fd_array[NR_OPEN_DEFAULT];  // 初始 fd 指针数组（默认 64）
};
```

### 8.4 struct fdtable - include/linux/fdtable.h

```c
struct fdtable {
    unsigned int max_fds;            // fd[] 数组的最大容量
    struct file __rcu **fd;          // fd 指针数组（指向 struct file 或 NULL）
    unsigned long *close_on_exec;    // 位图：exec 时自动关闭的 fd
    unsigned long *open_fds;         // 位图：已分配的 fd 号
    unsigned long *full_fds_bits;    // 位图：full_fds 的优化位
    struct rcu_head rcu;             // RCU 回调，用于延迟释放
};
```

### 8.5 struct file（文件对象） - include/linux/fs.h

```c
struct file {
    struct path f_path;              // 文件路径（dentry + mount）
    struct inode *f_inode;           // 指向 inode 的快捷方式
    const struct file_operations *f_op;  // 文件操作函数表
    file_ref_t f_ref;                // 引用计数（file_ref 封装）
    unsigned int f_flags;            // 文件状态标志
    fmode_t f_mode;                  // 打开模式
    loff_t f_pos;                    // 当前读写位置
    void *private_data;              // 文件系统私有数据
    struct address_space *f_mapping; // 页缓存映射
    // ... 其他字段
};
```

---

## 9 错误处理

| 错误码 | 条件 | 说明 |
|--------|------|------|
| `EINVAL` | `flags` 包含非法位 | 只允许 `CLOSE_RANGE_UNSHARE` 和 `CLOSE_RANGE_CLOEXEC` |
| `EINVAL` | `fd > max_fd` | 起始 fd 大于结束 fd |
| `ENOMEM` | `dup_fd` 分配失败 | 仅在 UNSHARE 模式下，内存不足时返回 |
| 忽略 | 单个 fd 关闭失败 | `__range_close` 中单个 fd 关闭错误被忽略，继续关闭后续 fd |

注意：`close_range` 的语义是"尽力关闭"——即使部分 fd 关闭失败，也继续关闭后续 fd，最终返回 0。这与逐个调用 `close()` 的行为不同。

---

## 10 使用案例

### 示例 1：exec() 前关闭所有非必要 fd

```c
// 关闭除 0/1/2 外的所有 fd（安全地，即使 fd 表被共享）
close_range(3, ~0U, CLOSE_RANGE_UNSHARE);
execve("/bin/ls", argv, envp);
```

### 示例 2：批量设置 close_on_exec

```c
// 设置 fd 3~100 在 exec() 时自动关闭
close_range(3, 100, CLOSE_RANGE_CLOEXEC);
```

### 示例 3：关闭指定范围的 fd

```c
// 关闭 fd 10~20
close_range(10, 20, 0);
```

### 示例 4：多线程中安全关闭 fd

```c
// 多线程中，先解除共享再关闭，避免影响其他线程的 fd 表
close_range(3, 100, CLOSE_RANGE_UNSHARE);
```

---

## 11 性能对比

| 方法 | 关闭 100 个 fd | 关闭 1000 个 fd |
|------|---------------|-----------------|
| 逐个 close() | 100 次系统调用 | 1000 次系统调用 |
| close_range() | 1 次系统调用 | 1 次系统调用 |
| 加速比 | ~100x | ~1000x |

性能优势来自：

1. **减少系统调用次数**：从 N 次减少到 1 次
2. **位图遍历优化**：使用 `find_next_bit` 跳过已关闭的 fd，无需遍历所有 fd 号
3. **批量位图操作**：`__range_cloexec` 使用 `bitmap_set` 一次设置整个范围
4. **punch_hole 优化**：UNSHARE 模式下，`dup_fd` 在复制时即可跳过目标范围的 fd

---

## 12 与 close 的对比

| 特性 | close | close_range |
|------|-------|-------------|
| 关闭范围 | 单个 fd | [fd, max_fd] 范围 |
| 系统调用次数 | 1 次/每个 fd | 1 次/整个范围 |
| fput 方式 | 同步 (`fput_close_sync`) | 延迟 (`fput_close`) |
| 错误处理 | 返回错误码 | 忽略错误，返回 0 |
| UNSHARE 支持 | 不适用 | 支持 |
| CLOEXEC 支持 | 不适用 | 支持 |

---

## 13 总结

```
close_range(fd, max_fd, flags)
  │
  ├─ 参数验证
  │
  ├─ [UNSHARE] 解除共享 fd 表
  │   └─ dup_fd() → 复制 fd 表（punch_hole 优化）
  │
  ├─ [CLOEXEC] 设置 close_on_exec 位图
  │   └─ __range_cloexec() → bitmap_set()
  │
  └─ [默认] 批量关闭
      └─ __range_close()
           ├─ 位图遍历（find_next_bit）
           ├─ 逐个关闭（file_close_fd_locked + filp_close）
           └─ cond_resched() 让出 CPU
```

`close_range` 是 Linux 5.9+ 引入的高效批量关闭接口，通过位图操作、punch_hole 复制和批量关闭策略，显著优于逐个调用 `close()`。它在容器初始化、进程执行前清理、文件描述符表安全隔离等场景中尤其有用。