# vmsplice 系统调用完整路径分析

## 1 概述

vmsplice 是 Linux 的**零拷贝**系统调用，用于在用户空间内存和管道（pipe）之间传输数据。vmsplice 提供两种方向的操作：

- **用户→pipe（写）**：通过 `get_user_pages_fast`（GUP）锁定用户态页面，将页面引用直接传递给 pipe buffer，**零拷贝**
- **pipe→用户（读）**：从 pipe buffer 读取数据，通过 `copy_page_to_iter` 拷贝到用户空间，**涉及一次 CPU 拷贝**

### 关键特点

- **SPLICE_F_GIFT**：vmsplice 专用标志，用户放弃页面所有权，内核无需拷贝即可直接使用页面
- **零拷贝写入**：用户→pipe 路径通过 GUP 锁定用户页面，pipe buffer 直接引用用户页面
- **用户空间 I/O 向量**：使用 `struct iovec` 描述用户内存区域
- **典型场景**：高性能管道写入、零拷贝日志系统

---

## 2 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | vmsplice 系统调用入口 (fs/splice.c) |
| **pipe 层** | pipe buffer 操作 (fs/pipe.c, include/linux/pipe_fs_i.h) |
| **VFS** | vmsplice_to_pipe / vmsplice_to_user |
| **MM（GUP）** | get_user_pages_fast（用户→pipe 路径锁定用户页） |
| **MM（拷贝）** | copy_page_to_iter（pipe→用户路径拷贝数据） |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE4(vmsplice) - fs/splice.c:1578

```c
SYSCALL_DEFINE4(vmsplice, int, fd, const struct iovec __user *, uiov,
        unsigned long, nr_segs, unsigned int, flags)
{
    struct iovec iovstack[UIO_FASTIOV];
    struct iovec *iov = iovstack;
    struct iov_iter iter;
    ssize_t error;
    int type;

    if (unlikely(flags & ~SPLICE_F_ALL))
        return -EINVAL;

    CLASS(fd, f)(fd);
    if (fd_empty(f))
        return -EBADF;
    if (fd_file(f)->f_mode & FMODE_WRITE)
        type = ITER_SOURCE;     // 写入 pipe → 用户页面锁定到 pipe
    else if (fd_file(f)->f_mode & FMODE_READ)
        type = ITER_DEST;       // 从 pipe 读取 → 拷贝到用户
    else
        return -EBADF;

    error = import_iovec(type, uiov, nr_segs,
                 ARRAY_SIZE(iovstack), &iov, &iter);
    if (error < 0)
        return error;

    if (!iov_iter_count(&iter))
        error = 0;
    else if (type == ITER_SOURCE)
        error = vmsplice_to_pipe(fd_file(f), &iter, flags);   // 用户→pipe
    else
        error = vmsplice_to_user(fd_file(f), &iter, flags);   // pipe→用户

    kfree(iov);
    return error;
}
```

关键逻辑：
- `type == ITER_SOURCE`：fd 以写模式打开 → 用户数据写入 pipe
- `type == ITER_DEST`：fd 以读模式打开 → 从 pipe 读取数据到用户
- `import_iovec`：从用户空间拷贝 iovec 数组，初始化 iov_iter

---

## 4 用户→pipe 路径（GUP 零拷贝）

```
/* ========== vmsplice 用户→pipe 零拷贝写入路径 ========== */
/* 数据流: 用户空间页面 → pipe buffer (GUP 锁定, 零拷贝) */
/* 调用链: vmsplice_to_pipe → iter_to_pipe → pipe_buffer 填充 */
/* 关键: get_user_pages_fast 锁定用户页, 避免页面被换出 */

vmsplice(fd, uiov, nr_segs, flags)
  │  # 系统调用入口
  │
  ├─ import_iovec(ITER_SOURCE, uiov, nr_segs, ...)
  │   # 从用户空间拷贝 iovec 数组
  │   # 初始化 iov_iter: iter.count = 总字节数
  │   # iter.iov = iov 数组 (用户空间内存区域描述)
  │
  └─ vmsplice_to_pipe(fd_file(f), &iter, flags)
      │  # fs/splice.c:1485 — 用户→pipe 的入口
      │
      └─ iter_to_pipe(pipe, &iter, flags)
          │  # fs/splice.c:1475 — 核心实现
          │
          └─ [for each iovec segment]:
              │  # 遍历 iov_iter 中的每个内存段
              │
              ├─ 获取用户页面
              │   # page = alloc_pages(GFP_USER, 0) 或
              │   # get_user_pages_fast(addr, 1, FOLL_WRITE, &page)
              │   # ★ 锁定用户页面, 防止页面被换出或释放 ★
              │   # 如果 SPLICE_F_GIFT 设置, 无需 get_user_pages
              │   # 因为用户已放弃页面所有权
              │
              ├─ pipe_lock(pipe)
              │   # 获取 pipe 互斥锁
              │
              ├─ wait_for_space(pipe, flags)
              │   # 等待 pipe 有可用空间
              │
              ├─ 填充 pipe buffer
              │   # buf = &pipe->bufs[pipe->head & mask]
              │   # buf->page   = page          ← 用户页面指针 (零拷贝!)
              │   # buf->offset = offset_in_page ← 页内偏移
              │   # buf->len    = this_len       ← 当前段长度
              │   # buf->ops    = user_page_pipe_buf_ops
              │   #   → release: put_page (释放页面引用)
              │   #   → try_steal: 尝试窃取页面所有权
              │   # buf->flags  = 0
              │   # pipe->head++                ← 推进 head
              │
              ├─ pipe_unlock(pipe)
              │   # 释放 pipe 锁
              │
              ├─ wakeup_pipe_readers(pipe)
              │   # 唤醒等待数据的读者
              │
              └─ iov_iter_advance(&iter, this_len)
                  # 推进 iov_iter 到下一个数据段
```

### SPLICE_F_GIFT 语义

```
/* ========== SPLICE_F_GIFT 标志行为 ========== */
/* SPLICE_F_GIFT 是 vmsplice 专用标志 */
/* 用户承诺放弃页面所有权, 内核可直接使用页面 */

带 SPLICE_F_GIFT:                          不带 SPLICE_F_GIFT:
  用户放弃页面所有权                           内核通过 GUP 锁定页面
  内核无需拷贝, 直接使用用户页面                   get_user_pages_fast(addr, 1, FOLL_WRITE, &page)
  用户不能再访问这些页面                         用户仍可访问页面
  风险: 用户违反契约可能导致数据不一致              安全但性能略低
  性能: 最佳 (无 GUP 开销)                     性能: 好 (有 GUP 锁定开销)
```

---

## 5 pipe→用户路径（CPU 拷贝）

```
/* ========== vmsplice pipe→用户读取路径 ========== */
/* 数据流: pipe buffer 页面 → 用户空间 (CPU 拷贝) */
/* 调用链: vmsplice_to_user → pipe_to_user → copy_page_to_iter */
/* 关键: 需要从内核 pipe buffer 拷贝数据到用户空间 */

vmsplice(fd, uiov, nr_segs, flags)
  │  # 系统调用入口
  │
  ├─ import_iovec(ITER_DEST, uiov, nr_segs, ...)
  │   # 从用户空间拷贝 iovec 数组
  │   # 初始化 iov_iter 作为数据接收目标
  │
  └─ vmsplice_to_user(fd_file(f), &iter, flags)
      │  # fs/splice.c:1510 — pipe→用户的入口
      │
      └─ __splice_from_pipe(pipe, &sd, pipe_to_user)
          │  # fs/splice.c:1120 — 通用 pipe 消费框架
          │
          └─ [for each pipe buffer in pipe]:
              │  # 从 pipe->tail 开始逐个处理 pipe buffer
              │
              └─ pipe_to_user(pipe, buf, sd)
                  │  # fs/splice.c:1060 — 将 pipe buffer 数据拷贝到用户
                  │
                  ├─ 获取 iov_iter 中的目标位置
                  │   # 从 sd->u.iter 获取用户空间目标地址
                  │
                  └─ copy_page_to_iter(buf->page, buf->offset, buf->len, sd->u.iter)
                      │  # lib/iov_iter.c — ★ CPU 拷贝发生在这里 ★
                      │
                      ├─ 如果 iov_iter 类型为 ITER_IOVEC:
                      │   # iov_iter 指向用户空间 iovec 数组
                      │   # copyout(to, from, n) → __copy_to_user
                      │   # 从内核 pipe buffer 页面拷贝到用户空间
                      │
                      └─ 如果 iov_iter 类型为 ITER_KVEC:
                          # 目标在内核空间 (特殊场景)
                          # memcpy(to, from, n)
```

---

## 6 函数调用栈

```
/* ========== vmsplice 用户→pipe 路径 (零拷贝, GUP 锁定) ========== */

SYSCALL_DEFINE4(vmsplice, fd, uiov, nr_segs, flags)              // fs/splice.c:1578 — 系统调用入口
└─ import_iovec(ITER_SOURCE, uiov, nr_segs, ...)                  // 从用户拷贝 iovec
   └─ vmsplice_to_pipe(fd_file(f), &iter, flags)                  // fs/splice.c:1485 — 用户→pipe
      └─ iter_to_pipe(pipe, &iter, flags)                         // fs/splice.c:1475 — 核心
         └─ [for each iovec segment]
            ├─ get_user_pages_fast(addr, 1, FOLL_WRITE, &page)    // mm/gup.c — GUP 锁定用户页
            ├─ pipe_lock(pipe)                                    // 获取 pipe 锁
            ├─ wait_for_space(pipe, flags)                        // 等待 pipe 空间
            ├─ buf->page = page                                   // ★ 零拷贝: 用户页面引用 ★
            │  buf->offset = offset_in_page
            │  buf->len = this_len
            │  buf->ops = user_page_pipe_buf_ops
            ├─ pipe->head++                                       // 推进 head
            ├─ pipe_unlock(pipe)                                  // 释放 pipe 锁
            └─ wakeup_pipe_readers(pipe)                          // 唤醒读者

/* ========== vmsplice pipe→用户路径 (CPU 拷贝) ========== */

SYSCALL_DEFINE4(vmsplice, fd, uiov, nr_segs, flags)              // fs/splice.c:1578 — 系统调用入口
└─ import_iovec(ITER_DEST, uiov, nr_segs, ...)                    // 从用户拷贝 iovec
   └─ vmsplice_to_user(fd_file(f), &iter, flags)                  // fs/splice.c:1510 — pipe→用户
      └─ __splice_from_pipe(pipe, &sd, pipe_to_user)              // fs/splice.c:1120
         └─ [for each pipe buffer]
            └─ pipe_to_user(pipe, buf, sd)                        // fs/splice.c:1060
               └─ copy_page_to_iter(buf->page, buf->offset,       // lib/iov_iter.c
                      buf->len, sd->u.iter)                       // ★ CPU 拷贝 ★
                  └─ __copy_to_user(to, from, n)                  // 从内核拷贝到用户空间
```

---

## 7 关键数据结构 (C代码 + 注释)

```c
// ===== 用户空间 I/O 向量 =====
// vmsplice 使用 iovec 描述用户空间内存区域
struct iovec {
    void __user *iov_base;         // 用户空间起始地址（vmsplice 从此处读取或写入）
    __kernel_size_t iov_len;       // 数据长度（字节）
};

// ===== I/O 迭代器 =====
// vmsplice 通过 iov_iter 管理用户内存区域的遍历
struct iov_iter {
    u8 iter_type;                  // ITER_SOURCE（写入 pipe）或 ITER_DEST（从 pipe 读取）
    loff_t start;                  // 起始偏移
    size_t count;                  // 剩余数据量（遍历时递减）
    const struct iovec *iov;       // iovec 数组（指向用户空间内存区域）
    unsigned long nr_segs;         // iovec 分段数
    // vmsplice 使用:
    //   ITER_SOURCE: 从 iov 读取数据写入 pipe
    //   ITER_DEST: 从 pipe 读取数据写入 iov
};

// ===== 管道信息结构体 =====
// vmsplice 将用户页面引用放入 pipe ring buffer
struct pipe_inode_info {
    struct mutex mutex;            // 互斥锁（保护 ring buffer 并发访问）
    wait_queue_head_t rd_wait;     // 读者等待队列
    wait_queue_head_t wr_wait;     // 写者等待队列（vmsplice 写入满时等待）
    unsigned int head;             // 头指针（vmsplice 写入时推进）
    unsigned int tail;             // 尾指针（读者读出位置）
    unsigned int max_usage;        // 最大使用量
    unsigned int ring_size;        // 环形缓冲区大小（PIPE_DEF_BUFFERS = 16）
    bool readers;                  // 是否有读者
    bool writers;                  // 是否有写者
    struct pipe_buffer *bufs;      // 环形缓冲区数组
};

// ===== 管道缓冲区条目 =====
// vmsplice 用户→pipe 路径的核心载体, 持有用户页面引用
struct pipe_buffer {
    struct page *page;             // ★ 页面指针（vmsplice 用户→pipe: 指向用户页面）
    unsigned int offset;           // 页内偏移
    unsigned int len;              // 有效数据长度
    const struct pipe_buf_operations *ops;  // 操作函数表
    unsigned int flags;            // PIPE_BUF_FLAG_* 标志
    // vmsplice 用户→pipe: ops = user_page_pipe_buf_ops
    //   → release: put_page (释放用户页面引用)
    //   → try_steal: 尝试窃取页面所有权
};

// ===== 页面结构体 =====
// vmsplice 通过 GUP 锁定用户页面, 或通过 SPLICE_F_GIFT 直接获取
struct page {
    unsigned long flags;           // PG_locked（GUP 锁定）、PG_swapbacked 等
    atomic_t _refcount;            // 引用计数（GUP 增加引用, 防止页面被换出）
    struct address_space *mapping; // 所属地址空间
    unsigned long index;           // 页索引
    // vmsplice 使用:
    //   get_user_pages_fast → 增加 _refcount, 锁定页面
    //   SPLICE_F_GIFT → 用户放弃所有权, 无需增加引用
};

// ===== splice 分发参数结构 =====
// vmsplice pipe→用户路径中控制传输参数
struct splice_desc {
    size_t len;                    // 当前要传输的字节数
    size_t total_len;              // 总传输字节数
    unsigned int flags;            // SPLICE_F_* 标志
    loff_t pos;                    // 当前位置（未使用）
    loff_t *opos;                  // 目标偏移（未使用）
    union {
        struct iov_iter *iter;     // pipe→用户路径: 指向用户空间 iov_iter
    } u;
    const struct pipe_buf_operations *ops;
    size_t used;
};
```

| 数据结构 | 头文件 | 在 vmsplice 中的作用 |
|----------|--------|------------------|
| `struct iovec` | `include/uapi/linux/uio.h` | 用户空间内存区域描述 |
| `struct iov_iter` | `include/linux/uio.h` | 遍历用户内存区域的迭代器 |
| `struct pipe_inode_info` | `include/linux/pipe_fs_i.h` | 管道 ring buffer 管理器 |
| `struct pipe_buffer` | `include/linux/pipe_fs_i.h` | 用户页面引用的载体 |
| `struct page` | `include/linux/mm_types.h` | 通过 GUP 锁定的用户页面 |
| `struct splice_desc` | `include/linux/splice.h` | pipe→用户路径的传输参数 |

---

## 8 性能分析

| 操作 | 数据拷贝次数 | 主要开销 | 适用场景 |
|------|-------------|---------|---------|
| `vmsplice(用户→pipe, 带 GIFT)` | 0 次 | GUP 略过 + pipe 锁定 | 高性能管道写入 |
| `vmsplice(用户→pipe, 无 GIFT)` | 0 次 | GUP 锁定 + pipe 锁定 | 安全管道写入 |
| `vmsplice(pipe→用户)` | 1 次 | copy_page_to_iter | 管道读取到用户 |
| 普通 write(pipe) | 1 次 | copy_from_user | 通用管道写入 |

---

## 9 总结

vmsplice 提供了**用户空间↔管道**的零拷贝数据传输路径：

1. **用户→pipe（写）**：真正的**零拷贝**路径。通过 `get_user_pages_fast` 锁定用户页面（或 `SPLICE_F_GIFT` 直接获取），将用户页面的引用直接放入 pipe buffer，**没有 CPU 数据拷贝**。

2. **pipe→用户（读）**：涉及**一次 CPU 拷贝**。通过 `copy_page_to_iter` 将 pipe buffer 中的数据拷贝到用户空间，这是不可避免的，因为用户空间页面不能被内核直接映射（除非使用 vmap 等特殊机制）。

3. **SPLICE_F_GIFT**：vmsplice 专用标志，是零拷贝的关键优化。用户承诺放弃页面所有权，内核无需通过 GUP 锁定页面，直接使用用户页面。但用户必须遵守契约——不能再访问这些页面。

4. **典型应用场景**：
   - 高性能日志系统：应用程序直接通过 vmsplice 将日志数据写入管道
   - 零拷贝 IPC：结合 splice 使用，实现用户→pipe→socket 的完整零拷贝路径
   - 数据采集：从用户空间内存区域直接传输到管道，避免中间缓冲