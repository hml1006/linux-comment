# tee 系统调用完整路径分析

## 1 概述

tee 是 Linux 的**零拷贝**系统调用，用于在两个管道文件描述符之间复制数据。tee 的独特之处在于**不消费输入**——它复制 pipe buffer 的页面引用（增加引用计数），输入管道的数据仍然保留，可被后续读取。

### 关键特点

- **零拷贝（Zero-Copy）**：通过 pipe buffer 页面引用传递，不拷贝数据
- **不消费输入**：区别于 splice 的 pipe→pipe 模式，tee 不推进输入管道的 tail 指针
- **仅限管道**：tee 要求输入和输出**都必须是管道**
- **典型场景**：tee 命令实现、数据分发（一个输入管道分发给多个输出管道）

### 与 splice 的区别

| 特性 | splice(pipe→pipe) | tee(pipe→pipe) |
|------|-------------------|----------------|
| 输入必须为 pipe | 是 | 是 |
| 输出必须为 pipe | 是 | 是 |
| 消费输入 | 是（推进 tail） | 否（保留数据） |
| 页面引用 | 传递并消费 | 复制并增加引用计数 |
| 典型使用 | 管道间数据搬移 | 数据分发/复制 |

---

## 2 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | tee 系统调用入口 (fs/splice.c) |
| **pipe 层** | pipe buffer 操作 (fs/pipe.c, include/linux/pipe_fs_i.h) |
| **VFS** | do_tee → splice_pipe_to_pipe |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE4(tee) - fs/splice.c:1977

```c
SYSCALL_DEFINE4(tee, int, fdin, int, fdout, size_t, len, unsigned int, flags)
{
    if (unlikely(flags & ~SPLICE_F_ALL))
        return -EINVAL;
    if (unlikely(!len))
        return 0;

    CLASS(fd, in)(fdin);
    if (fd_empty(in))
        return -EBADF;

    CLASS(fd, out)(fdout);
    if (fd_empty(out))
        return -EBADF;

    return do_tee(fd_file(in), fd_file(out), len, flags);
}
```

### 3.2 do_tee - fs/splice.c:1955

```c
ssize_t do_tee(struct file *in, struct file *out, size_t len, unsigned int flags)
{
    struct pipe_inode_info *ipipe = get_pipe_info(in, true);
    struct pipe_inode_info *opipe = get_pipe_info(out, true);

    // 校验: 输入和输出都必须是管道
    if (unlikely(!ipipe || !opipe))
        return -EINVAL;

    if (unlikely(ipipe->readers == 0 || opipe->writers == 0))
        return -EINVAL;

    // 调用 splice_pipe_to_pipe, 但 tee 不消费输入
    return splice_pipe_to_pipe(ipipe, opipe, len, flags);
}
```

---

## 4 pipe→pipe 路径（tee 模式）

```
/* ========== tee pipe→pipe 零拷贝路径 ========== */
/* 数据流: pipe buffer 页面引用 → pipe buffer (零拷贝, tee 不消费输入) */
/* 调用链: do_tee → splice_pipe_to_pipe */
/* 关键: tee 不推进 ipipe->tail, 仅增加页面引用计数 */

/* splice_pipe_to_pipe 主流程 (fs/splice.c:1716) */
/* tee 和 splice 共享此函数, 区别在于 tail 推进行为 */

splice_pipe_to_pipe(ipipe, opipe, len, flags)
  │  # fs/splice.c:1716 — tee 的核心实现
  │  # tee 模式: 不消费输入 (不推进 ipipe->tail)
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
  │   # 如果 ipipe == opipe 只加一次锁 (tee 到自身)
  │
  └─ [主循环: 遍历 ipipe buffer]:
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
      ├─ ★ tee 关键: 增加页面引用计数, 不消费输入 ★
      │   # 与 splice 不同, tee 不推进 ipipe->tail
      │   # 而是增加页面引用计数, 确保数据在输入管道中保留
      │   # get_page(ibuf->page)  ← 增加页面引用计数
      │   # 输入管道的数据保持不变, 可被后续 tee 或 read 再次读取
      │
      ├─ 更新统计
      │   # ret += this_len
      │   # len -= this_len
      │   # if (len == 0) break          ← 满足传输量后退出
      │
      └─ [循环结束]
          ├─ pipe_double_unlock(ipipe, opipe)     # 释放双锁
          ├─ wakeup_pipe_readers(opipe)            # 唤醒输出管道读者
          └─ # 注意: 不唤醒输入管道写者
            # 因为 tee 不消费输入, 输入管道数据未被移除
            # 输入管道写者不需要被唤醒 (空间未被释放)
```

### tee 与 splice 在 pipe→pipe 中的关键区别

```
splice(pipe→pipe):                          tee(pipe→pipe):
  obuf->page = ibuf->page                      obuf->page = ibuf->page
  obuf->offset = ibuf->offset                  obuf->offset = ibuf->offset
  obuf->len = this_len                         obuf->len = this_len
  opipe->head++      ← 推进输出               opipe->head++      ← 推进输出
  ibuf->offset += this_len                     get_page(ibuf->page)  ← ★ 增加引用
  ibuf->len -= this_len                        不推进 ipipe->tail   ← ★ 不消费
  if (ibuf->len == 0): ipipe->tail++           输入数据保留, 可再次读取
  ← ★ 消费输入
```

---

## 5 函数调用栈

```
/* ========== tee 主路径 (pipe→pipe, 零拷贝, 不消费) ========== */

SYSCALL_DEFINE4(tee, fdin, fdout, len, flags)                  // fs/splice.c:1977 — 系统调用入口
└─ do_tee(fd_file(in), fd_file(out), len, flags)               // fs/splice.c:1955 — 校验并路由
   │
   ├─ ipipe = get_pipe_info(in, true)                           // 检查输入是管道
   ├─ opipe = get_pipe_info(out, true)                          // 检查输出是管道
   ├─ [!ipipe || !opipe] → return -EINVAL                      // 必须都是管道
   │
   └─ splice_pipe_to_pipe(ipipe, opipe, len, flags)             // fs/splice.c:1716 — 核心实现
      │
      ├─ ipipe_prep(ipipe, flags)                               // 等待输入有数据
      ├─ opipe_prep(opipe, flags)                               // 等待输出有空间
      ├─ pipe_double_lock(ipipe, opipe)                         // 避免死锁
      │
      └─ [主循环: 遍历 ipipe buffer]
         ├─ 读取 ibuf = &ipipe->bufs[ipipe->tail & mask]       // 获取输入 buffer
         ├─ 写入 obuf = &opipe->bufs[opipe->head & mask]       // 获取输出 buffer
         ├─ obuf->page = ibuf->page                             // ★ 页面引用传递 (零拷贝) ★
         ├─ get_page(ibuf->page)                                // ★ 增加引用计数 (不消费) ★
         ├─ opipe->head++                                       // 推进输出
         └─ [不推进 ipipe->tail]                                // ★ 保留输入数据 ★
```

---

## 6 关键数据结构 (C代码 + 注释)

```c
// ===== 管道信息结构体 =====
// tee 的核心中转站, 管理环形 pipe buffer 数组
struct pipe_inode_info {
    struct mutex mutex;            // 互斥锁（保护 ring buffer 并发访问）
    wait_queue_head_t rd_wait;     // 读者等待队列（pipe 空时等待）
    wait_queue_head_t wr_wait;     // 写者等待队列（pipe 满时等待）
    unsigned int head;             // 头指针（写者写入位置, tee 推进输出 pipe 的 head）
    unsigned int tail;             // 尾指针（读者读出位置, tee 不推进输入 pipe 的 tail）
    unsigned int max_usage;        // 最大使用量
    unsigned int ring_size;        // 环形缓冲区大小（PIPE_DEF_BUFFERS = 16）
    bool readers;                  // 是否有读者
    bool writers;                  // 是否有写者
    struct pipe_buffer *bufs;      // 环形缓冲区数组（每个条目指向一个页面）
};

// ===== 管道缓冲区条目 =====
// tee 零拷贝的核心载体, 持有页面引用而非数据拷贝
struct pipe_buffer {
    struct page *page;             // ★ 页面指针（零拷贝关键: 只传递引用, 不拷贝数据）
    unsigned int offset;           // 页内偏移
    unsigned int len;              // 有效数据长度
    const struct pipe_buf_operations *ops;  // 操作函数表（release/confirm/steal）
    unsigned int flags;            // PIPE_BUF_FLAG_* 标志
};

// ===== 页面结构体 =====
// tee 通过 get_page() 增加引用计数, 保留输入管道中的数据
struct page {
    unsigned long flags;           // PG_* 标志
    atomic_t _refcount;            // ★ 引用计数（tee 通过 get_page 增加, 确保数据保留）
    struct address_space *mapping; // 所属地址空间
    unsigned long index;           // 页索引
    // tee 使用: get_page(ibuf->page) 增加引用计数
    // 这样输入和输出管道都持有该页面的引用
    // 输入管道数据不会被释放, 可被后续读取
};

// ===== pipe buffer 操作函数表 =====
// 定义 pipe buffer 的生命周期管理回调
struct pipe_buf_operations {
    int (*confirm)(struct pipe_inode_info *, struct pipe_buffer *);
    // 确认 buffer 数据可用（通常只是返回 0）
    void (*release)(struct pipe_inode_info *, struct pipe_buffer *);
    // 释放 buffer（页面引用计数减1）
    // tee 不调用 release, 因为不消费输入
    bool (*try_steal)(struct pipe_inode_info *, struct pipe_buffer *);
    // 尝试窃取页面所有权（SPLICE_F_GIFT 相关）
    bool (*get)(struct pipe_inode_info *, struct pipe_buffer *);
    // 获取额外引用
};

// ===== 文件对象 =====
// tee 的输入/输出管道文件描述符
struct file {
    struct path f_path;            // 文件路径
    struct inode *f_inode;         // 指向 inode（管道 inode）
    const struct file_operations *f_op;  // 文件操作函数表
    atomic_long_t f_count;         // 引用计数
    fmode_t f_mode;                // 打开模式（FMODE_READ, FMODE_WRITE）
};
```

| 数据结构 | 头文件 | 在 tee 中的作用 |
|----------|--------|------------------|
| `struct pipe_inode_info` | `include/linux/pipe_fs_i.h` | 管道 ring buffer 管理器 |
| `struct pipe_buffer` | `include/linux/pipe_fs_i.h` | 页面引用传递的载体（零拷贝关键） |
| `struct page` | `include/linux/mm_types.h` | 通过引用计数保留数据 |
| `struct pipe_buf_operations` | `include/linux/pipe_fs_i.h` | pipe buffer 生命周期回调 |
| `struct file` | `include/linux/fs.h` | 输入/输出管道文件描述符 |

---

## 7 总结

tee 的核心价值在于**零拷贝的管道间数据复制**，且**不消费输入**：

1. **零拷贝**：通过 `splice_pipe_to_pipe` 将输入 pipe buffer 的页面引用复制到输出 pipe buffer，**没有 CPU 数据拷贝**。

2. **不消费输入**：tee 不推进输入管道的 `tail` 指针，而是通过 `get_page()` 增加页面引用计数。输入管道的数据仍然保留，可被后续的 `tee` 或 `read` 再次读取。这是 tee 与 splice pipe→pipe 的核心区别。

3. **典型应用场景**：
   - `tee` 命令行工具：同时将数据写入文件并输出到终端
   - 数据分发：一个输入管道分发给多个输出管道（多个 tee 调用）
   - 管道监控：在不影响数据流的情况下复制管道数据进行分析

4. **限制**：tee 要求输入和输出**都必须是管道**，无法用于文件或 socket 传输。