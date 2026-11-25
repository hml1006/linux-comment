# copy_file_range 系统调用完整路径分析

## 1 概述

copy_file_range 是一个**内核内文件区间拷贝**系统调用，用于在内核中直接完成两个文件之间的数据拷贝，避免用户空间的中间缓冲。

### 关键特点

- **三级策略**：优先尝试硬件 offload（reflink），回退到 splice 零拷贝路径
- **同文件系统 reflink**：通过 extent 共享实现**零数据拷贝**，仅元数据操作
- **跨文件系统回退**：使用 `do_splice_direct`，通过 pipe buffer 页面引用传递
- **零拷贝条件**：同文件系统且支持 reflink 时为真正零拷贝；跨文件系统需 CPU 拷贝到目标页缓存

---

## 2 涉及的内核层

| 层                           | 说明                                                     |
| ---------------------------- | -------------------------------------------------------- |
| **Syscall Entry**      | copy_file_range (fs/read_write.c)                        |
| **VFS**                | vfs_copy_file_range (fs/read_write.c)                    |
| **splice 核心**        | do_splice_direct → splice_direct_to_actor (fs/splice.c) |
| **pipe 层**            | 匿名管道缓冲区（作为中转）                               |
| **ext4（源）**         | filemap_splice_read / ext4_file_splice_read              |
| **ext4（目标）**       | iter_file_splice_write / ext4_file_write_iter            |
| **ext4 reflink**       | ext4_remap_file_range（同 FS 优先路径）                  |
| **Page Cache**         | 页面引用传递或 CPU 拷贝                                  |
| **Block Layer / NVMe** | 仅在页缓存未命中时触及                                   |

---

## 3 copy_file_range 系统调用

### 3.1 SYSCALL_DEFINE6(copy_file_range) - fs/read_write.c:1929

```c
SYSCALL_DEFINE6(copy_file_range, int, fd_in, loff_t __user *, off_in,
        int, fd_out, loff_t __user *, off_out,
        size_t, len, unsigned int, flags)
{
    loff_t pos_in;
    loff_t pos_out;
    ssize_t ret = -EBADF;

    // 输入/输出文件获取
    CLASS(fd, f_in)(fd_in);
    CLASS(fd, f_out)(fd_out);

    // 偏移量处理
    if (off_in) {
        if (copy_from_user(&pos_in, off_in, sizeof(loff_t)))
            return -EFAULT;
    } else {
        pos_in = fd_file(f_in)->f_pos;      // 使用 f_pos
    }
    if (off_out) {
        if (copy_from_user(&pos_out, off_out, sizeof(loff_t)))
            return -EFAULT;
    } else {
        pos_out = fd_file(f_out)->f_pos;
    }

    if (flags != 0)
        return -EINVAL;

    ret = vfs_copy_file_range(fd_file(f_in), pos_in, fd_file(f_out),
                  pos_out, len, flags);
    // 更新偏移量
    if (ret > 0) {
        pos_in += ret;
        pos_out += ret;
        if (off_in) {
            if (copy_to_user(off_in, &pos_in, sizeof(loff_t)))
                ret = -EFAULT;
        } else {
            fd_file(f_in)->f_pos = pos_in;
        }
        if (off_out) {
            if (copy_to_user(off_out, &pos_out, sizeof(loff_t)))
                ret = -EFAULT;
        } else {
            fd_file(f_out)->f_pos = pos_out;
        }
    }
    return ret;
}
```

### 3.2 vfs_copy_file_range - fs/read_write.c:1833

```c
ssize_t vfs_copy_file_range(struct file *file_in, loff_t pos_in,
                struct file *file_out, loff_t pos_out,
                size_t len, unsigned int flags)
{
    ssize_t ret;
    bool splice = flags & COPY_FILE_SPLICE;
    bool samesb = file_inode(file_in)->i_sb == file_inode(file_out)->i_sb;

    // 通用检查
    ret = generic_copy_file_checks(file_in, pos_in, file_out, pos_out, &len, flags);
    if (ret) return ret;

    // 权限验证
    ret = rw_verify_area(READ, file_in, &pos_in, len);
    ret = rw_verify_area(WRITE, file_out, &pos_out, len);
    if (len == 0) return 0;

    file_start_write(file_out);

    // --- 策略 1: copy_file_range op（FS 特定实现）---
    if (!splice && file_out->f_op->copy_file_range) {
        ret = file_out->f_op->copy_file_range(file_in, pos_in,
                              file_out, pos_out, len, flags);
    }
    // --- 策略 2: remap_file_range（同 FS reflink）---
    else if (!splice && file_in->f_op->remap_file_range && samesb) {
        ret = file_in->f_op->remap_file_range(file_in, pos_in,
                file_out, pos_out, len, REMAP_FILE_CAN_SHORTEN);
        if (ret <= 0)
            splice = true;    // reflink 失败，回退到 splice
    }
    // --- 策略 3: 无 reflink，回退 splice---
    else if (samesb) {
        splice = true;
    }

    file_end_write(file_out);

    // --- 回退路径：do_splice_direct ---
    if (!splice)
        goto done;

    ret = do_splice_direct(file_in, &pos_in, file_out, &pos_out, len, 0);
done:
    // 统计更新
    if (ret > 0) {
        fsnotify_access(file_in);
        add_rchar(current, ret);
        fsnotify_modify(file_out);
        add_wchar(current, ret);
    }
    inc_syscr(current);
    inc_syscw(current);
    return ret;
}
```

### 3.3 copy_file_range 三种策略对比

| 策略                                   | 条件                              | 函数路径              | 数据拷贝                                        | 性能 |
| -------------------------------------- | --------------------------------- | --------------------- | ----------------------------------------------- | ---- |
| **1 FS copy_file_range**         | FS 实现`f_op->copy_file_range`  | ext4_copy_file_range  | 可硬件 offload                                  | 最快 |
| **2 remap_file_range (reflink)** | 同 FS +`f_op->remap_file_range` | ext4_remap_file_range | **元数据操作，无数据拷贝**                | 极快 |
| **3 do_splice_direct (回退)**    | 不同 FS 或 reflink 不支持         | do_splice_direct      | **页面引用传递（零拷贝）+ 可能 CPU 拷贝** | 中   |

### 3.4 ext4 的 copy_file_range 实现

```
ext4_copy_file_range(file_in, pos_in, file_out, pos_out, len, flags)
  └─ ext4_remap_file_range(file_in, pos_in, file_out, pos_out, len, ...)
       └─ ext4_clone_range(file_in, pos_in, file_out, pos_out, len)
            └─ 基于 extent 共享的 reflink
  └─ 若 reflink 失败 → 回退到 splice_file_range
       └─ splice_file_range(file_in, &pos_in, file_out, &pos_out, len)
            └─ do_splice_direct_actor(..., splice_file_range_actor)
```

---

## 4 do_splice_direct 内部路径

```c
ssize_t do_splice_direct(struct file *in, loff_t *ppos, struct file *out,
             loff_t *opos, size_t len, unsigned int flags)
{
    return do_splice_direct_actor(in, ppos, out, opos, len, flags,
                      direct_splice_actor);
}

// 内部实现：
static ssize_t do_splice_direct_actor(struct file *in, loff_t *ppos,
                      struct file *out, loff_t *opos,
                      size_t len, unsigned int flags,
                      splice_direct_actor *actor)
{
    struct splice_desc sd = {
        .len     = len,
        .total_len = len,
        .flags   = flags,
        .pos     = *ppos,
        .u.file  = out,
        .opos    = opos,
    };
    ssize_t ret;

    // 检查输出是否可写
    if (unlikely(!(out->f_mode & FMODE_WRITE)))
        return -EBADF;
    if (unlikely(out->f_flags & O_APPEND))
        return -EINVAL;

    // 核心函数
    ret = splice_direct_to_actor(in, &sd, actor);
    if (ret > 0)
        *ppos = sd.pos;
    return ret;
}
```

### splice_direct_to_actor 内部逻辑

```
splice_direct_to_actor(in, &sd, actor)
  ├─ 分配内部 pipe（通过 pipe_create 创建匿名 pipe）
  ├─ while (sd->total_len > 0):
  │    ├─ bytes = do_splice_read(in, &sd->pos, pipe, ...)
  │    │    → file->f_op->splice_read → filemap_splice_read
  │    │    → add_to_pipe（页面引用传递到 pipe buffer）
  │    │
  │    ├─ sd->total_len -= bytes
  │    │
  │    └─ while (bytes > 0):
  │         └─ written = actor(pipe, &sd)
  │              ├─ for copy_file_range: splice_file_range_actor
  │              │    └─ out->f_op->splice_write → iter_file_splice_write
  │              │         → pipe buffer 映射到 iov_iter
  │              │         → call_write_iter → ext4_file_write_iter
  │              │
  │              └─ (actor 可根据需要扩展)
  │
  └─ 销毁内部 pipe
```

---

## 5 完整流程图

```
                    copy_file_range(fd_in, off_in, fd_out, off_out, len, flags)
                                       |
                             +---------v----------+
                             |  SYSCALL_DEFINE6    |  系统调用入口
                             |  (fs/read_write.c:  |  (fs/read_write.c:1929)
                             |   1929)             |
                             +---------+----------+
                                       |
                             +---------v----------+
                             |  vfs_copy_file_    |  VFS 层
                             |  range(file_in,    |  权限验证 + 路由选择
                             |  pos_in, file_out, |  (fs/read_write.c:1833)
                             |  pos_out, len, 0)  |
                             +---------+----------+
                                       |
                      +----------------+-----------------+
                      |                |                 |
              +-------v------+  +-----v-------+  +------v-------+
              |  [策略1:     |  | [策略2:     |  | [策略3:     |
              |  f_op->copy_ |  | f_op->remap_|  | do_splice_  |
              |  file_range] |  | file_range] |  | direct]     |
              |  FS 特定实现 |  | 同 FS reflink|  | 跨 FS 回退  |
              +------+-------+  +-----+-------+  +------+-------+
                     |                 |                 |
              +------v-------+  +------v-------+  +------v-------+
              | ext4_copy_   |  | ext4_remap_  |  | splice_direct|
              | file_range   |  | file_range   |  | _to_actor()  |
              | (硬件 offload)|  | (extent 共享) |  | (pipe 中转)  |
              | 零数据拷贝    |  | 零数据拷贝    |  | 最多 1 次   |
              +------+-------+  +------+-------+  | CPU 拷贝     |
                     |                 |          +------+-------+
                     |                 |                 |
                     +--------+--------+                 |
                              |                          |
                              |              +-----------v-----------+
                              |              | 创建内部匿名 pipe     |
                              |              | (fs/pipe.c)           |
                              |              +-----------+-----------+
                              |                          |
                              |              +-----------v-----------+
                              |              | do_splice_read()      |
                              |              | → filemap_splice_read |
                              |              | → add_to_pipe()       |
                              |              | (零拷贝: 页面引用)    |
                              |              +-----------+-----------+
                              |                          |
                              |              +-----------v-----------+
                              |              | splice_file_range_   |
                              |              | actor()                |
                              |              | → iter_file_splice_   |
                              |              |   write()              |
                              |              | → call_write_iter()    |
                              |              | → ext4_file_write_iter |
                              |              | (CPU 拷贝到页缓存)    |
                              |              +-----------+-----------+
                              |                          |
                              |              +-----------v-----------+
                              |              | 还有数据?              |
                              |              | (sd->total_len > 0)   |
                              |              +-----------+-----------+
                              |                  yes |    | no
                              |        +-------------+    |
                              |        |                  |
                              |        +-------+  +-------v-----------+
                              |                |  | 销毁 pipe          |
                              |                |  +-------------------+
                              |                |
                              +-------+--------+
                                      |
                              +-------v--------+
                              | 更新偏移/统计  |
                              | 返回传输字节数  |
                              +----------------+

        [页缓存未命中路径 - 条件触发]
        filemap_splice_read
               |
        +------v-------+
        | filemap_get_ |  → 缺页:  page_cache_sync_readahead
        | folio        |  → 预读:  page_cache_async_readahead
        +------+-------+  → 最终:  ext4_read_folio → BIO → NVMe
               |
        +------v-------+
        | add_to_pipe  |  页面引用传递到 pipe buffer
        +--------------+
```

## 6 完整函数调用链

### 6.1 copy_file_range 同 FS reflink 路径

```
/* ========== 同文件系统 reflink 路径 ========== */
/* 条件: file_in 和 file_out 在同一文件系统，且文件系统支持 reflink */
/* 特点: 零数据拷贝，仅修改 extent 元数据 */

SYSCALL_DEFINE6(copy_file_range, fd_in, off_in, fd_out, off_out, len, flags)
                                                // fs/read_write.c:1929
└─ vfs_copy_file_range(file_in, pos_in, file_out, pos_out, len, flags)
                                                // fs/read_write.c:1833 — VFS 路由
   ├─ generic_copy_file_checks(...)             // 基本检查（fd 有效性、文件类型等）
   ├─ rw_verify_area(READ, file_in, ...)        // 读权限验证
   ├─ rw_verify_area(WRITE, file_out, ...)       // 写权限验证
   ├─ file_start_write(file_out)                // 文件写冻结保护
   │
   ├─ [策略1: f_op->copy_file_range 存在?]
   │  └─ file_out->f_op->copy_file_range(...)   // → ext4_copy_file_range (fs/ext4/file.c)
   │       └─ ext4_remap_file_range(file_in, pos_in, file_out, pos_out, len, ...)
   │                                            // fs/ext4/extents.c — reflink 入口
   │            └─ ext4_clone_range(file_in, pos_in, file_out, pos_out, len)
   │                                            // fs/ext4/extents.c — extent 克隆
   │                 ├─ ext4_ext_replay_shrink_goal_iblock()  // 调整 extent 范围对齐
   │                 ├─ ext4_es_register_shrinker()           // extent 状态管理
   │                 ├─ ext4_claim_cluster()                  // 声明磁盘 cluster
   │                 └─ ext4_insert_cloned_extent()           // 插入克隆 extent
   │                      ├─ ext4_ext_insert_extent()         // extent 树操作
   │                      └─ up_write(&EXT4_I(inode)->i_data_sem)  // 释放写锁
   │
   ├─ [策略2: !splice && samesb && remap_file_range 存在?]
   │  └─ file_in->f_op->remap_file_range(...)    // — 同策略1，reflink 回退
   │       └─ [失败] → splice = true             // reflink 失败，标记回退
   │
   └─ file_end_write(file_out)                   // 结束写保护
```

> 同 FS reflink 路径为**零数据拷贝**，仅通过 CoW extent 共享实现。
> 如果策略 1 的 `copy_file_range` 不可用，内核会尝试策略 2 的 `remap_file_range`。

### 6.2 copy_file_range 跨 FS splice 回退路径

```
/* ========== 跨文件系统 splice 回退路径 ========== */
/* 条件: 非同一文件系统，或 reflink 不支持/失败 */
/* 特点: 通过内部匿名 pipe 中转，源端零拷贝，目标端 CPU 拷贝 */

/* ── 接 vfs_copy_file_range 策略 3 ── */

vfs_copy_file_range(...)                        // fs/read_write.c:1833
└─ [splice == true 回退路径]
   └─ do_splice_direct(file_in, &pos_in, file_out, &pos_out, len, 0)
                                                // fs/splice.c:1225
        └─ do_splice_direct_actor(in, ppos, out, opos, len, flags,
                                  splice_file_range_actor)
                                                // fs/splice.c:1180
             └─ splice_direct_to_actor(in, &sd, splice_file_range_actor)
                                                // fs/splice.c — splice 核心循环
                  ├─ pipe_create()              // fs/pipe.c — 创建内部匿名 pipe
                  │    ├─ alloc_pipe_info()     // 分配 pipe_inode_info
                  │    └─ pipe_inode_info->bufs = kcalloc(ring_size, ...)
                  │                             // 分配 pipe_buffer ring buffer
                  │
                  ├─ while (sd->total_len > 0): // 循环传输直到数据耗尽
                  │    │
                  │    ├─ do_splice_read(in, &sd->pos, pipe, sd->len)
                  │    │    ├─ file->f_op->splice_read
                  │    │    │    → ext4_file_splice_read   // fs/ext4/file.c
                  │    │    │    → filemap_splice_read     // mm/filemap.c
                  │    │    │         └─ filemap_get_folio()  // 获取页缓存 folio
                  │    │    │              ├─ [命中] → 直接使用
                  │    │    │              └─ [未命中] → page_cache_sync_readahead
                  │    │    │                   → ext4_read_folio → BIO → NVMe
                  │    │    └─ add_to_pipe(pipe, folio_page)  // 页面引用->pipe buffer
                  │    │         └─ pipe->bufs[pipe->head].page = page  // 零拷贝
                  │    │
                  │    ├─ sd->total_len -= bytes
                  │    │
                  │    └─ splice_file_range_actor(pipe, &sd)
                  │         └─ out->f_op->splice_write
                  │              → iter_file_splice_write    // fs/splice.c
                  │                   └─ call_write_iter(out, &kiocb, &iov_iter)
                  │                        → ext4_file_write_iter  // fs/ext4/file.c
                  │                             └─ ext4_buffered_write_iter
                  │                                  └─ iomap_file_buffered_write
                  │                                       └─ iomap_write_begin/end
                  │                                            └─ copy_page_from_iter_atomic
                  │                                                 /* CPU 拷贝到目标页缓存 */
                  │
                  └─ pipe_destroy()             // 销毁内部匿名 pipe
                       └─ free_pipe_info()      // 释放 pipe_buffer ring buffer
```

> 跨 FS 路径的**源端**为零拷贝（页面引用传递到 pipe buffer），**目标端**需要一次 CPU 拷贝（`copy_page_from_iter_atomic` 将 pipe buffer 数据拷贝到目标页缓存）。

---

## 7 零拷贝路径的数据流对比

```

copy_file_range 同 FS reflink:
  [extent in file_in]  ──共享 extent──→ [file_out]
                            零数据拷贝（元数据操作）

copy_file_range 跨 FS:
  [ext4 页缓存 folio]  ──页面引用──→ [pipe buffer]  ──iov_iter映射──→ [file_out 页缓存]
                                     零拷贝                  CPU 拷贝（write_iter）
```

---

## 8 性能分析

| 操作                                  | 数据拷贝次数 | 主要开销               | 适用场景         |
| ------------------------------------- | ------------ | ---------------------- | ---------------- |
| `copy_file_range(same FS, reflink)` | 0 次         | extent 元数据操作      | 文件克隆         |
| `copy_file_range(cross FS, splice)` | 1 次         | splice 中转 + CPU 拷贝 | 跨文件系统拷贝   |
| read + write（用户态）                | 2 次         | 用户态缓冲             | 通用（最差性能） |
| mmap + write                          | 1 次         | page fault + CPU 拷贝  | 中等场景         |

---

## 9 关键数据结构

```c
// ========== splice 描述符 (fs/splice.c) ==========

// splice 操作的核心描述符，贯穿整个 splice 传输过程
struct splice_desc {
    size_t total_len;                // 剩余待传输的总字节数
    unsigned int len;                // 当前本次传输的字节数
    unsigned int flags;              // 标志位（SPLICE_F_MOVE 等）
    loff_t pos;                      // 源文件当前偏移量
    loff_t *opos;                    // 指向目标文件偏移量的指针
    struct file *u.file;             // 目标文件（用于 splice 写）
    unsigned int nr_pages;           // 已使用的 page 数
    unsigned int nr_pages_max;       // 最大 page 数（pipe 容量）
    struct splice_eof *splice_eof;   // 结束处理（可选）
};

// ========== 管道 inode 信息 (include/linux/pipe_fs_i.h) ==========

// 匿名管道——作为 splice 中转缓冲
// sendfile/copy_file_range 内部创建，传输完成后销毁
struct pipe_inode_info {
    struct mutex mutex;              // 保护 pipe 操作
    wait_queue_head_t rd_wait;       // 读等待队列
    wait_queue_head_t wr_wait;       // 写等待队列
    unsigned int head;               // 写指针（ring buffer 头部）
    unsigned int tail;               // 读指针（ring buffer 尾部）
    unsigned int max_usage;          // 最大使用槽位数
    unsigned int ring_size;          // ring buffer 大小（页数）
    unsigned int nr_accounted;       // 已记账页面数
    unsigned int readers;            // 读端引用计数（splice 设为 1）
    unsigned int writers;            // 写端引用计数（splice 设为 1）
    struct pipe_buffer *bufs;        // pipe_buffer 数组（ring buffer）
};

// 管道缓冲区——每个 slot 对应一个物理页面
// splice 通过 add_to_pipe 将源文件页缓存映射到此
struct pipe_buffer {
    struct page *page;               // 物理页面指针（零拷贝关键）
    unsigned int offset;             // 数据在页面内的偏移
    unsigned int len;                // 数据长度
    const struct pipe_buf_operations *ops;  // 操作函数（释放/确认等）
    unsigned int flags;              // 标志位（PIPE_BUF_FLAG_*）
};

// ========== 文件操作参数 (include/linux/fs.h) ==========

// 内核 I/O 控制块——用于 splice 写路径
// copy_file_range 的 splice 回退路径使用此结构传递参数
struct kiocb {
    struct file *ki_filp;            // 目标文件
    loff_t ki_pos;                   // 文件偏移量
    void (*ki_complete)(struct kiocb *, long);  // 异步完成回调
    unsigned int ki_flags;           // 标志位（IOCB_*）
    union {
        void __user *ki_to_user;     // 用户缓冲区指针
        struct iov_iter *ki_iter;    // I/O 向量迭代器
    };
};

// ========== copy_file_range 三级策略参数 (fs/read_write.c) ==========

// copy_file_range 系统调用用户态参数
struct copy_file_range_args {
    int fd_in;                       // 源文件描述符
    int fd_out;                      // 目标文件描述符
    loff_t __user *off_in;           // 源偏移指针（用户态）
    loff_t __user *off_out;          // 目标偏移指针（用户态）
    size_t len;                      // 拷贝长度
    unsigned int flags;              // 标志位（当前必须为 0）
};

// remap_file_range 参数（用于 reflink 路径）
// 通过 f_op->remap_file_range 传递
struct remap_file_range_args {
    struct file *file_in;            // 源文件
    loff_t pos_in;                   // 源起始偏移
    struct file *file_out;           // 目标文件
    loff_t pos_out;                  // 目标起始偏移
    loff_t len;                      // 长度
    unsigned int remap_flags;        // 重映射标志（REMAP_FILE_CAN_SHORTEN 等）
};
```

## 10 总结

copy_file_range 代表 Linux 内核中**最高效的文件区间拷贝路径**：

1. **同文件系统 reflink**：通过 extent 共享实现**零数据拷贝**，仅元数据操作。这是最理想的路径，但要求源和目标在同一文件系统，且文件系统支持 reflink。
2. **跨文件系统 splice 回退**：使用 `do_splice_direct` 通过匿名 pipe 中转，结合 `filemap_splice_read`（零拷贝读）和 `iter_file_splice_write`（可能需要 CPU 拷贝写）。虽然不是真正的零拷贝，但仍然避免了用户态缓冲区。
3. **三级策略**：`f_op->copy_file_range` → `f_op->remap_file_range` → `do_splice_direct`，逐级尝试最高效的路径。
