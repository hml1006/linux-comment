# splice 系统调用完整路径分析

## 1 概述

splice 是 Linux 的**零拷贝**系统调用，用于在两个文件描述符之间传输数据，其中**至少一端必须是管道（pipe）**。splice 通过 pipe buffer 传递页面引用而非数据，避免用户空间的 CPU 数据拷贝。

### 关键特点

- **零拷贝（Zero-Copy）**：数据在内核内直接传递，不经过用户空间缓冲区
- **管道中介**：至少一端必须是管道（pipe），pipe buffer 传递的是页面引用而非数据
- **splice(fd_in, fd_out)**：文件↔管道或管道↔文件（和管道↔管道），**消费输入**（推进 tail 指针）
- **SPLICE_F_MOVE**：预期移交页面所有权（当前内核忽略，等同于 SPLICE_F_MOVE 优化合并）

### 三种传输模式

| 模式 | 数据流 | 拷贝次数 | 典型场景 |
|------|--------|---------|---------|
| file→pipe | 页缓存 → pipe buffer | 0 次 | 文件读入管道 |
| pipe→file | pipe buffer → 页缓存 | 1 次 CPU 拷贝 | 管道写入文件 |
| pipe→pipe | pipe buffer 引用复制 | 0 次 | 管道间传递 |

---

## 2 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | splice 系统调用入口 (fs/splice.c) |
| **pipe 层** | pipe buffer 操作 (fs/pipe.c, include/linux/pipe_fs_i.h) |
| **VFS** | do_splice / do_splice_read / splice_file_to_pipe / splice_from_pipe |
| **ext4** | file 读/写路径（当 splice 从/向常规文件时） |
| **Page Cache** | page 引用传递 |
| **Block Layer** | 当常规文件缺页时（读路径进入块设备） |
| **NVMe 驱动** | 仅当常规文件读缺页时触及 |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE6(splice) - fs/splice.c:1616

```c
SYSCALL_DEFINE6(splice, int, fd_in, loff_t __user *, off_in,
        int, fd_out, loff_t __user *, off_out,
        size_t, len, unsigned int, flags)
{
    if (unlikely(!len))
        return 0;
    if (unlikely(flags & ~SPLICE_F_ALL))
        return -EINVAL;

    CLASS(fd, in)(fd_in);
    if (fd_empty(in))
        return -EBADF;

    CLASS(fd, out)(fd_out);
    if (fd_empty(out))
        return -EBADF;

    return __do_splice(fd_file(in), off_in, fd_file(out), off_out, len, flags);
}
```

---

## 4 __do_splice → do_splice 路由逻辑

### 4.1 __do_splice - fs/splice.c:1397

```c
static ssize_t __do_splice(struct file *in, loff_t __user *off_in,
               struct file *out, loff_t __user *off_out,
               size_t len, unsigned int flags)
{
    struct pipe_inode_info *ipipe, *opipe;
    loff_t offset, *__off_in = NULL, *__off_out = NULL;
    ssize_t ret;

    ipipe = get_pipe_info(in, true);   // 输入是管道？
    opipe = get_pipe_info(out, true);  // 输出是管道？

    if (ipipe) { if (off_in) return -ESPIPE; pipe_clear_nowait(in); }
    if (opipe) { if (off_out) return -ESPIPE; pipe_clear_nowait(out); }

    // 从用户空间拷贝偏移量
    if (off_out) { copy_from_user(&offset, off_out, ...); __off_out = &offset; }
    if (off_in) { copy_from_user(&offset, off_in, ...); __off_in = &offset; }

    ret = do_splice(in, __off_in, out, __off_out, len, flags);

    // 拷贝偏移量回用户空间
    if (__off_out) copy_to_user(off_out, __off_out, ...);
    if (__off_in) copy_to_user(off_in, __off_in, ...);
    return ret;
}
```

### 4.2 do_splice 路由选择 - fs/splice.c:1300

```c
ssize_t do_splice(struct file *in, loff_t *off_in, struct file *out,
          loff_t *off_out, size_t len, unsigned int flags)
{
    struct pipe_inode_info *ipipe, *opipe;

    ipipe = get_pipe_info(in, true);
    opipe = get_pipe_info(out, true);

    if (ipipe && opipe) {
        // pipe → pipe (splice 消费模式)
        ret = splice_pipe_to_pipe(ipipe, opipe, len, flags);
    } else if (ipipe) {
        // pipe → file (splice 读 pipe 写入文件)
        // → splice_from_pipe → do_splice_from → file->write_iter
        //   → ext4_file_write_iter (当目标为 ext4 文件时)
    } else if (opipe) {
        // file → pipe (splice 读文件写入 pipe)
        // → splice_file_to_pipe → do_splice_read
        //   → file->read_iter → ext4_file_read_iter (当源为 ext4 文件时)
    }
}
```

### 4.3 splice 三种传输模式

| 输入 | 输出 | 函数路径 | 数据传递方式 |
|------|------|---------|-------------|
| 管道 | 管道 | `splice_pipe_to_pipe` | pipe buffer 引用传递（零拷贝，消费输入） |
| 管道 | 文件 | `splice_from_pipe` → `f_op->write_iter` | pipe buffer → 页缓存（1 次 CPU 拷贝） |
| 文件 | 管道 | `splice_file_to_pipe` → `f_op->read_iter` | 页缓存 → pipe buffer（零拷贝） |

---

## 5 file→pipe 路径（零拷贝读）

```
/* ========== splice file→pipe 零拷贝读取路径 ========== */
/* 数据流: 页缓存 folio → pipe buffer (页面引用传递, 不拷贝数据) */
/* 调用链: do_splice → splice_file_to_pipe → do_splice_read → ext4_file_splice_read */
/* 关键: add_to_pipe 将 folio 的 page 指针直接添加到 pipe ring buffer */

do_splice(file_in, &pos, opipe, NULL, len, flags)
  │  # 路由到 file→pipe 路径 (opipe != NULL)
  │
  └─ splice_file_to_pipe(file_in, opipe, &pos, len, flags)
      │  # fs/splice.c:1280 — file→pipe 的入口函数
      │
      ├─ pipe_lock(opipe)
      │   # 获取 pipe 互斥锁, 保护 pipe ring buffer 的并发访问
      │
      ├─ wait_for_space(opipe, flags)
      │   # 等待 pipe 有可用空间
      │   # 如果 pipe 满且非 NONBLOCK, 睡眠等待读者消费数据
      │   # pipe 容量: PIPE_DEF_BUFFERS = 16 个 buffer, 每个 1 页
      │
      ├─ do_splice_read(in, offset, opipe, len, flags)
      │   │  # fs/splice.c:1290 — 初始化 splice_desc 结构体
      │   │  # sd.len = len, sd.total_len = len
      │   │  # sd.flags = flags, sd.pos = *offset, sd.u.file = in
      │   │
      │   └─ in->f_op->splice_read(file_in, &sd)
      │       │  # VFS 层调用, 实际调用文件系统注册的 splice_read
      │       │  # 对于 ext4: ext4_file_splice_read
      │       │
      │       └─ ext4_file_splice_read(in, ppos, pipe, len, flags)
      │           │  # fs/ext4/file.c — 设置读取标志
      │           │
      │           └─ filemap_splice_read(in, ppos, pipe, len, flags)
      │               │  # mm/filemap.c — 核心: 逐 folio 处理
      │               │
      │               └─ [for each folio in file range]:
      │                   │  # 遍历文件偏移范围内的每个 folio
      │                   │
      │                   ├─ filemap_get_folio(mapping, index)
      │                   │   │  # 在页缓存中查找 folio
      │                   │   │
      │                   │   ├─ [命中] 直接返回 folio 指针
      │                   │   │   # folio 已在页缓存中, 增加引用计数
      │                   │   │
      │                   │   └─ [未命中] 触发缺页读
      │                   │       ├─ page_cache_sync_readahead  // 同步预读
      │                   │       └─ filemap_read_folio
      │                   │           └─ ext4_read_folio  // 分配 bio, 提交到块层
      │                   │
      │                   ├─ 计算页面偏移和长度
      │                   │   # folio_pos = folio->index << PAGE_SHIFT
      │                   │   # offset_in_folio = pos - folio_pos
      │                   │   # this_len = min(folio_size - offset, remaining)
      │                   │
      │                   └─ add_to_pipe(pipe, &buf)
      │                       │  # ★ 零拷贝关键 ★
      │                       │  # 将 folio 的页面引用添加到 pipe ring buffer
      │                       │  # buf.page   = folio_page(folio, 0)  ← 页面指针
      │                       │  # buf.offset = offset_in_folio       ← 页内偏移
      │                       │  # buf.len    = this_len              ← 有效数据长度
      │                       │  # buf.ops    = page_cache_pipe_buf_ops ← 释放回调
      │                       │  # 不拷贝数据! 只传递 struct page 指针
      │                       │
      │                       └─ 唤醒 pipe 读者
      │                           # wake_up_interruptible_sync_poll(&pipe->wait, EPOLLIN)
      │
      ├─ pipe_unlock(opipe)           # 释放 pipe 锁
      └─ wakeup_pipe_readers(opipe)   # 最终唤醒所有等待的 pipe 读者
```

**零拷贝的关键**：`add_to_pipe` 将 `folio` 的**页面引用**直接添加到 pipe buffer 中，不拷贝数据。

---

## 6 pipe→file 路径（1 次 CPU 拷贝）

```
/* ========== splice pipe→file 写入路径 ========== */
/* 数据流: pipe buffer 页面 → iov_iter → 文件页缓存 (需要 1 次 CPU 拷贝) */
/* 调用链: do_splice → splice_from_pipe → do_splice_from → ext4_file_splice_write */
/* 关键: 将 pipe buffer 的页面重新映射为 iov_iter, 通过常规 write_iter 写入 */

do_splice(ipipe, NULL, file_out, &pos, len, flags)
  │  # 路由到 pipe→file 路径 (ipipe != NULL, opipe == NULL)
  │
  └─ splice_from_pipe(ipipe, out, &offset, len, flags)
      │  # fs/splice.c:1250 — pipe→file 的入口函数
      │  # 构造 splice_desc, 初始化 sd.len/total_len/flags/pos
      │
      ├─ pipe_lock(ipipe)               # 获取 pipe 互斥锁
      ├─ ipipe_prep(ipipe, flags)       # 等待 pipe 有数据可读
      │
      └─ do_splice_from(ipipe, out, &offset, len, flags)
          │  # fs/splice.c:1234 — 调用 __splice_from_pipe
          │
          └─ __splice_from_pipe(pipe, &sd, splice_from_pipe_actor)
              │  # fs/splice.c:1120 — 通用 pipe 消费框架
              │
              └─ [for each pipe buffer in pipe]:
                  │  # 从 pipe->tail 开始逐个处理 pipe buffer
                  │
                  └─ splice_from_pipe_actor(pipe, buf, sd)
                      │  # fs/splice.c:1180 — actor 回调
                      │
                      ├─ 构造 I/O 参数
                      │   # init_sync_kiocb(&kiocb, out)  ← 同步 kiocb
                      │   # iov_iter_init(&iter, ITER_SOURCE, &iov, 1, buf->len)
                      │   # iov.iov_base = page_address(buf->page) + buf->offset
                      │   # kiocb.ki_pos = *ppos   ← 文件写入位置
                      │
                      └─ call_write_iter(out, &kiocb, &iter)
                          │  # VFS 层调用 → ext4_file_write_iter
                          │
                          └─ ext4_file_write_iter(iocb, from)
                              │  # fs/ext4/file.c
                              │
                              └─ generic_perform_write(iocb, from)
                                  │  # mm/filemap.c — 逐 folio 写入
                                  │
                                  └─ [for each folio in write range]:
                                      ├─ 查找/分配目标 folio
                                      │
                                      ├─ copy_folio_from_iter_atomic(folio, offset, bytes, i)
                                      │   │  # ★ CPU 拷贝发生在这里 ★
                                      │   │  # 将 iov_iter 中的数据拷贝到 folio 页面
                                      │   │  # 涉及 1 次 CPU 内存拷贝
                                      │   │
                                      │   └─ 数据从 pipe buffer page → 文件页缓存 page
                                      │
                                      └─ folio_mark_uptodate(folio)
                                          # 标记 folio 为 uptodate
```

该路径将 pipe buffer 中的页面**重新映射到 iov_iter**，通过常规的 `write_iter` 路径写入目标文件。数据从 pipe buffer 的 page 经由 `copy_folio_from_iter_atomic` 拷贝到文件页缓存（**涉及一次 CPU 拷贝**）。

---

## 7 pipe→pipe 路径（splice 消费模式）

```
/* ========== splice pipe→pipe 零拷贝路径 ========== */
/* 数据流: pipe buffer 页面引用 → pipe buffer (零拷贝, splice 消费输入) */
/* 调用链: do_splice → splice_pipe_to_pipe */
/* 关键: splice 推进 ipipe->tail (消费输入), tee 不推进 */

/* splice_pipe_to_pipe 主流程 (fs/splice.c:1716) */

splice_pipe_to_pipe(ipipe, opipe, len, flags)
  │  # fs/splice.c:1716 — splice 和 tee 的核心实现
  │  # splice 模式: 消费输入 (推进 ipipe->tail)
  │  # 参数: ipipe=输入管道, opipe=输出管道, len=传输字节数
  │
  ├─ ipipe_prep(ipipe, flags)
  │   # 等待输入管道有数据可读
  │   # 如果 pipe 空且非 NONBLOCK, 睡眠等待写者产生数据
  │
  ├─ opipe_prep(opipe, flags)
  │   # 等待输出管道有可用空间
  │   # 如果 pipe 满且非 NONBLOCK, 睡眠等待读者消费数据
  │
  ├─ pipe_double_lock(ipipe, opipe)
  │   # 按地址排序后加锁, 避免 ABBA 死锁
  │   # 如果 ipipe == opipe 只加一次锁
  │
  └─ [主循环: 遍历 pipe buffer]:
      │  # 从 ipipe->tail 开始逐个处理 pipe buffer
      │
      ├─ 取输入 pipe buffer
      │   # ibuf = &ipipe->bufs[ipipe->tail & mask]
      │   # 读取: ibuf->page, ibuf->offset, ibuf->len, ibuf->ops, ibuf->flags
      │
      ├─ 计算可传输长度
      │   # space = opipe->max_usage - (opipe->head - opipe->tail)
      │   # this_len = min(ibuf->len, space)
      │   # 如果 opipe 满了, 等待空间
      │
      ├─ 写入输出 pipe buffer
      │   # obuf = &opipe->bufs[opipe->head & mask]
      │   # obuf->page   = ibuf->page     ← 页面指针传递 (零拷贝!)
      │   # obuf->offset = ibuf->offset    ← 页内偏移
      │   # obuf->len    = this_len        ← 数据长度
      │   # obuf->ops    = ibuf->ops       ← 操作函数表
      │   # obuf->flags  = ibuf->flags     ← PIPE_BUF_FLAG_* 标志
      │   # opipe->head++                  ← 推进输出 head
      │
      ├─ 更新输入 pipe buffer (splice 消费模式)
      │   # ibuf->offset += this_len     ← 推进页内偏移
      │   # ibuf->len    -= this_len     ← 减少剩余长度
      │   # if (ibuf->len == 0):
      │   #   ipipe->tail++              ← ★ splice 推进 tail (消费数据) ★
      │   #   ibuf->ops->release(...)    ← 释放页面引用
      │   #   这区别于 tee: tee 不推进 tail, 仅增加引用计数
      │
      ├─ 更新统计
      │   # ret += this_len
      │   # len -= this_len
      │   # if (len == 0) break          ← 满足传输量后退出
      │
      └─ [循环结束]
          ├─ pipe_double_unlock(ipipe, opipe)     # 释放双锁
          ├─ wakeup_pipe_readers(opipe)            # 唤醒输出管道读者
          └─ wakeup_pipe_writers(ipipe)            # 唤醒输入管道写者
```

---

## 8 函数调用栈

```
/* ========== splice 主路径 (file→pipe, 零拷贝) ========== */

SYSCALL_DEFINE6(splice, fd_in, off_in, fd_out, off_out, len, flags)  // fs/splice.c:1616 — 系统调用入口
└─ __do_splice(fd_file(in), off_in, fd_file(out), off_out, len, flags)  // fs/splice.c:1397
   └─ do_splice(in, __off_in, out, __off_out, len, flags)                // fs/splice.c:1300 — 路由选择
      │
      ├─ [file→pipe] — opipe != NULL
      │  └─ splice_file_to_pipe(in, opipe, off_in, len, flags)           // fs/splice.c:1280
      │     └─ do_splice_read(in, offset, opipe, len, flags)             // fs/splice.c:1290
      │        └─ in->f_op->splice_read(in, &sd)                         // VFS 调用
      │           └─ ext4_file_splice_read(in, ppos, pipe, len, flags)   // fs/ext4/file.c
      │              └─ filemap_splice_read(in, ppos, pipe, len, flags)  // mm/filemap.c
      │                 └─ [逐 folio] filemap_get_folio → add_to_pipe    // ★ 零拷贝: 页面引用传递 ★
      │
      ├─ [pipe→file] — ipipe != NULL
      │  └─ splice_from_pipe(ipipe, out, off_out, len, flags)            // fs/splice.c:1250
      │     └─ do_splice_from(ipipe, out, &offset, len, flags)           // fs/splice.c:1234
      │        └─ __splice_from_pipe(pipe, &sd, splice_from_pipe_actor)  // fs/splice.c:1120
      │           └─ [逐 buffer] splice_from_pipe_actor(pipe, buf, sd)   // fs/splice.c:1180
      │              └─ call_write_iter(out, &kiocb, &iter)              // VFS 写
      │                 └─ ext4_file_write_iter(iocb, from)              // fs/ext4/file.c
      │                    └─ generic_perform_write(iocb, from)           // mm/filemap.c
      │                       └─ copy_folio_from_iter_atomic(...)        // ★ 1 次 CPU 拷贝 ★
      │
      └─ [pipe→pipe] — ipipe != NULL && opipe != NULL
         └─ splice_pipe_to_pipe(ipipe, opipe, len, flags)                // fs/splice.c:1716
            └─ [主循环: 遍历 ipipe buffer]
               ├─ 页面引用复制: obuf->page = ibuf->page                  // ★ 零拷贝 ★
               └─ splice 消费: ipipe->tail++                              // 推进输入 tail
```

---

## 9 关键数据结构 (C代码 + 注释)

```c
// ===== splice 分发参数结构 =====
// 控制 splice 完整传输路径的上下文, 用于 file→pipe 和 pipe→file 路径
struct splice_desc {
    size_t len;                    // 当前要传输的字节数
    size_t total_len;              // 总传输字节数（循环递减到 0 结束）
    unsigned int flags;            // SPLICE_F_* 标志（SPLICE_F_MOVE 等）
    loff_t pos;                    // 源文件当前偏移（file→pipe 时使用）
    loff_t *opos;                  // 目标文件偏移指针（pipe→file 时更新）
    union {
        struct file *file;         // 目标文件（pipe→file 的输出文件）
    } u;
    const struct pipe_buf_operations *ops;  // pipe buffer 操作函数表
    size_t used;                   // 已使用的字节数
};

// ===== 管道信息结构体 =====
// splice 的核心中转站, 管理环形 pipe buffer 数组
struct pipe_inode_info {
    struct mutex mutex;            // 互斥锁（保护 ring buffer 并发访问）
    wait_queue_head_t rd_wait;     // 读者等待队列（pipe 空时等待）
    wait_queue_head_t wr_wait;     // 写者等待队列（pipe 满时等待）
    unsigned int head;             // 头指针（写者写入位置, splice pipe→pipe 时推进）
    unsigned int tail;             // 尾指针（读者读出位置, splice 消费时推进）
    unsigned int max_usage;        // 最大使用量
    unsigned int ring_size;        // 环形缓冲区大小（PIPE_DEF_BUFFERS = 16）
    bool readers;                  // 是否有读者
    bool writers;                  // 是否有写者
    struct pipe_buffer *bufs;      // 环形缓冲区数组（每个条目指向一个页面）
};

// ===== 管道缓冲区条目 =====
// splice 零拷贝的核心载体, 持有页面引用而非数据拷贝
struct pipe_buffer {
    struct page *page;             // ★ 页面指针（零拷贝关键: 只传递引用, 不拷贝数据）
    unsigned int offset;           // 页内偏移（splice 消费时逐步推进）
    unsigned int len;              // 有效数据长度（splice 消费时递减）
    const struct pipe_buf_operations *ops;  // 操作函数表（release/confirm/steal）
    unsigned int flags;            // PIPE_BUF_FLAG_* 标志
};

// ===== 文件对象 =====
// splice 的输入/输出文件描述符
struct file {
    struct path f_path;            // 文件路径
    struct inode *f_inode;         // 指向 inode
    const struct file_operations *f_op;  // 文件操作函数表（splice_read/splice_write）
    atomic_long_t f_count;         // 引用计数
    loff_t f_pos;                  // 当前读写位置
    fmode_t f_mode;                // 打开模式（FMODE_READ, FMODE_WRITE 等）
    unsigned int f_flags;          // 文件状态标志（O_NONBLOCK 等）
};

// ===== I/O 控制块 =====
// pipe→file 写路径的同步 I/O 控制块
struct kiocb {
    struct file *ki_filp;          // 目标文件
    loff_t ki_pos;                 // 写入位置
    unsigned short ki_flags;       // IOCB_* 标志
    short ki_ioprio;               // I/O 优先级
};

// ===== I/O 迭代器 =====
// pipe→file 路径中, 将 pipe buffer 页面映射为 iov_iter 供 write_iter 使用
struct iov_iter {
    u8 iter_type;                  // ITER_SOURCE / ITER_DEST 等
    loff_t start;                  // 起始偏移
    size_t count;                  // 剩余数据量
    const struct iovec *iov;       // iovec 数组（指向 pipe buffer 页面）
    unsigned long nr_segs;         // 分段数
    // splice 使用: iov.iov_base = page_address(buf->page) + buf->offset
};

// ===== folio——页缓存单元 =====
// file→pipe 路径中, 通过 add_to_pipe 传递页面引用
struct folio {
    unsigned long flags;           // PG_uptodate（数据有效）、PG_locked（锁定）等
    struct address_space *mapping; // 所属的 address_space（页缓存索引）
    loff_t index;                  // 文件内页索引
    atomic_t _refcount;            // 引用计数（pipe buffer 引用 + 页缓存引用）
};
```

| 数据结构 | 头文件 | 在 splice 中的作用 |
|----------|--------|------------------|
| `struct splice_desc` | `include/linux/splice.h` | 控制 splice 传输参数的上下文 |
| `struct pipe_inode_info` | `include/linux/pipe_fs_i.h` | 管道 ring buffer 管理器 |
| `struct pipe_buffer` | `include/linux/pipe_fs_i.h` | 页面引用传递的载体（零拷贝关键） |
| `struct file` | `include/linux/fs.h` | 输入/输出文件描述符 |
| `struct kiocb` | `include/linux/fs.h` | pipe→file 写路径的 I/O 控制块 |
| `struct iov_iter` | `include/linux/uio.h` | pipe→file 路径的页面映射 |
| `struct folio` | `include/linux/mm_types.h` | 页缓存单元, file→pipe 时传递 |

---

## 10 总结

splice 的核心价值在于**零拷贝**的数据传输：

1. **file→pipe**：真正的**零拷贝**路径。通过 `add_to_pipe` 将页缓存 folio 的页面引用直接传递到 pipe buffer，**没有 CPU 数据拷贝**。

2. **pipe→file**：涉及**一次 CPU 拷贝**。pipe buffer 的页面通过 `iov_iter` 映射后，经由 `copy_folio_from_iter_atomic` 拷贝到目标文件的页缓存。

3. **pipe→pipe**：**零拷贝**路径。通过 `splice_pipe_to_pipe` 将 pipe buffer 的页面引用复制到输出 pipe，同时**消费输入**（推进 `ipipe->tail`）。splice 的 pipe→pipe 模式区别于 `tee`——splice 消费输入数据，而 tee 保留输入。

关键设计：splice 的任意路径都**至少一端必须是 pipe**，pipe buffer 的 `page` 指针传递是零拷贝的核心机制。