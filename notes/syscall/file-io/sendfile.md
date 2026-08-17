# sendfile 系统调用完整路径分析

## 1 概述

sendfile 是 Linux 的**零拷贝文件传输**系统调用，用于在内核中直接完成文件到 socket（或文件到 pipe）的数据传输，避免用户空间的中间缓冲。

### 关键特点

- **sendfile**：零拷贝从文件到 socket（或到 pipe）的传输，避免用户态中间缓冲区
- **sendfile64**：支持 64 位偏移量的 sendfile 变体
- **基于 splice 机制**：sendfile 内部调用 `do_splice_direct`，使用内部 pipe 作为中转
- **零拷贝条件**：文件→socket 路径通过 pipe buffer 页面引用传递，pipe→socket 通过 sendpage 零拷贝

---

## 2 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | sendfile/sendfile64 (fs/read_write.c) |
| **VFS** | do_sendfile (fs/read_write.c) |
| **splice 核心** | do_splice_direct → splice_direct_to_actor (fs/splice.c) |
| **pipe 层** | 内部匿名管道缓冲区（作为中转） |
| **ext4（源）** | filemap_splice_read / ext4_file_splice_read |
| **ext4（目标）** | iter_file_splice_write / ext4_file_write_iter（仅文件→文件路径） |
| **Page Cache** | 页面引用传递 |
| **Block Layer / NVMe** | 仅在页缓存未命中时触及 |

---

## 3 sendfile 系统调用

### 3.1 SYSCALL_DEFINE4(sendfile) - fs/read_write.c:1679

```c
SYSCALL_DEFINE4(sendfile, int, out_fd, int, in_fd, off_t __user *, offset,
        size_t, count)
{
    loff_t pos;
    off_t off;
    ssize_t ret;

    if (offset) {
        if (unlikely(get_user(off, offset)))
            return -EFAULT;
        pos = off;
        ret = do_sendfile(out_fd, in_fd, &pos, count, MAX_NON_LFS);
        if (unlikely(put_user(pos, offset)))
            return -EFAULT;
        return ret;
    }

    return do_sendfile(out_fd, in_fd, NULL, count, 0);
}

SYSCALL_DEFINE4(sendfile64, int, out_fd, int, in_fd, loff_t __user *, offset,
        size_t, count)
{
    loff_t pos;
    ssize_t ret;

    if (offset) {
        if (unlikely(copy_from_user(&pos, offset, sizeof(loff_t))))
            return -EFAULT;
        ret = do_sendfile(out_fd, in_fd, &pos, count, 0);
        if (unlikely(put_user(pos, offset)))
            return -EFAULT;
        return ret;
    }

    return do_sendfile(out_fd, in_fd, NULL, count, 0);
}
```

关键差异：
- **sendfile**：`offset` 使用 `off_t`（32位），`MAX_NON_LFS` 限制（2GB）
- **sendfile64**：`offset` 使用 `loff_t`（64位），无 LFS 限制
- `offset != NULL`：使用指定偏移，**不更新** `file->f_pos`
- `offset == NULL`：使用 `file->f_pos`，并**更新** `f_pos`

### 3.2 do_sendfile - fs/read_write.c:1583

```c
static ssize_t do_sendfile(int out_fd, int in_fd, loff_t *ppos,
               size_t count, loff_t max)
{
    struct pipe_inode_info *opipe;
    loff_t pos;
    loff_t out_pos;
    ssize_t retval;
    int fl;

    // --- 输入文件检查 ---
    CLASS(fd, in)(in_fd);
    // ... FMODE_READ 校验 ...
    if (!ppos) {
        pos = fd_file(in)->f_pos;           // 使用 f_pos
    } else {
        pos = *ppos;
        if (!(fd_file(in)->f_mode & FMODE_PREAD))
            return -ESPIPE;                 // 需要 FMODE_PREAD
    }
    retval = rw_verify_area(READ, fd_file(in), &pos, count);

    // --- 输出文件检查 ---
    CLASS(fd, out)(out_fd);
    // ... FMODE_WRITE 校验 ...
    out_pos = fd_file(out)->f_pos;

    // 检查传输大小限制
    if (!max)
        max = min(in_inode->i_sb->s_maxbytes, out_inode->i_sb->s_maxbytes);

    // --- 路由选择（基于输出类型）---
    opipe = get_pipe_info(fd_file(out), true);
    if (!opipe) {
        // 输出不是 pipe → 通过内部 pipe 中转
        retval = do_splice_direct(fd_file(in), &pos, fd_file(out), &out_pos,
                      count, fl);
    } else {
        // 输出是 pipe → 直接 file→pipe 拼接
        retval = splice_file_to_pipe(fd_file(in), opipe, &pos, count, fl);
    }

    // --- 统计更新 ---
    if (retval > 0) {
        add_rchar(current, retval);
        add_wchar(current, retval);
        fsnotify_access(fd_file(in));
        fsnotify_modify(fd_file(out));
        fd_file(out)->f_pos = out_pos;
        if (ppos)
            *ppos = pos;
        else
            fd_file(in)->f_pos = pos;
    }
    inc_syscr(current);
    inc_syscw(current);
    return retval;
}
```

### 3.3 sendfile 两条路径

#### 路径 A：文件→socket（通过内部 pipe 中转，零拷贝）

```
sendfile(socket_fd, file_fd, offset, count)
  └─ do_sendfile(out_fd=file_out_not_pipe, in_fd=file_in, ...)
       └─ do_splice_direct(file_in, &pos, file_out, &out_pos, count, 0)
            └─ splice_direct_to_actor(in, &sd, direct_splice_actor)
                 ├─ 创建内部匿名 pipe
                 ├─ do_splice_read(in, &pos, pipe, len, flags)
                 │    → file_in->f_op->splice_read
                 │    → ext4_file_splice_read
                 │    → filemap_splice_read
                 │    → add_to_pipe（零拷贝：页面引用传递到 pipe）
                 ├─ sd.u.file = file_out (socket)
                 └─ sd.actor → direct_splice_actor
                      └─ pipe_to_sendpage(pipe, buf, sd)
                           └─ out->f_op->splice_write(pipe, out, opos, len, flags)
                           或 pipe buf → sendpage
```

对于 socket 输出，`direct_splice_actor` 会调用 `pipe_to_sendpage`，直接将 pipe buffer 中的页面描述符传递给网络协议栈（如 tcp_sendpage），**真正的零拷贝**。

#### 路径 B：文件→pipe（直接拼接）

```
sendfile(pipe_fd, file_fd, offset, count)
  └─ do_sendfile(out_fd=pipe, in_fd=file, ...)
       └─ splice_file_to_pipe(file_in, opipe, &pos, count, fl)
            └─ do_splice_read(in, offset, opipe, len, flags)
                 → filemap_splice_read → add_to_pipe（零拷贝）
```

---

## 4 do_splice_direct 内部路径

sendfile 的路由逻辑：
- 输出不是 pipe（如 socket）→ `do_splice_direct`（通过内部 pipe 中转）
- 输出是 pipe → `splice_file_to_pipe`（直接拼接）

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
              └─ direct_splice_actor(pipe, &sd)
                   └─ out->f_op->splice_write → iter_file_splice_write
                        → pipe buffer 映射到 iov_iter
                        → call_write_iter → ext4_file_write_iter
  │
  └─ 销毁内部 pipe
```

---

## 5 函数调用栈

```
/* ========== sendfile 主路径 ========== */
/* 零拷贝文件→socket/pipe 传输 */

SYSCALL_DEFINE4(sendfile, out_fd, in_fd, offset, count)        // fs/read_write.c:1679 — 系统调用入口
└─ do_sendfile(out_fd, in_fd, ppos, count, max)                // fs/read_write.c:1583 — 核心实现
   │
   ├─ CLASS(fd, in)(in_fd)                                      // 获取输入文件描述符
   ├─ [!(in->f_mode & FMODE_READ)] → return -EBADF             // 读权限检查
   │
   ├─ [ppos == NULL] → pos = fd_file(in)->f_pos                // 使用 f_pos
   │  └─ [ppos != NULL] → pos = *ppos                          // 使用指定偏移
   │     └─ [!(fd_file(in)->f_mode & FMODE_PREAD)] → return -ESPIPE // 需要定位读
   │
   ├─ rw_verify_area(READ, fd_file(in), &pos, count)           // 区域验证
   │
   ├─ CLASS(fd, out)(out_fd)                                    // 获取输出文件描述符
   ├─ [!(out->f_mode & FMODE_WRITE)] → return -EBADF           // 写权限检查
   │
   ├─ opipe = get_pipe_info(fd_file(out), true)                 // 检查输出是否为 pipe
   │  │
   │  ├─ [opipe != NULL] → 输出是 pipe
   │  │  └─ splice_file_to_pipe(fd_file(in), opipe, &pos, count, fl)  // 直接 file→pipe 拼接
   │  │     └─ do_splice_read(in, offset, opipe, len, flags)          // fs/splice.c
   │  │        └─ in->f_op->splice_read(in, &sd)                      // → ext4_file_splice_read
   │  │           └─ filemap_splice_read(in, ppos, pipe, len, flags)  // mm/filemap.c
   │  │              └─ [逐 folio 处理]
   │  │                 ├─ filemap_get_folio(mapping, index)          // 查找页缓存
   │  │                 │  ├─ [命中] → 直接返回 folio
   │  │                 │  └─ [未命中] → filemap_read_folio → ext4_read_folio
   │  │                 └─ add_to_pipe(pipe, &buf)                    // ★ 零拷贝 ★
   │  │                    └─ buf.page = folio_page(folio, 0)         // 页面引用传递
   │  │                       buf.offset = offset_in_folio
   │  │                       buf.len = this_len
   │  │                       buf.ops = page_cache_pipe_buf_ops
   │  │
   │  └─ [opipe == NULL] → 输出不是 pipe（如 socket）
   │     └─ do_splice_direct(fd_file(in), &pos, fd_file(out), &out_pos, count, fl)
   │        │  // fs/splice.c:1225 — 通过内部匿名 pipe 中转
   │        │
   │        └─ do_splice_direct_actor(in, &sd, direct_splice_actor)
   │           └─ splice_direct_to_actor(in, &sd, direct_splice_actor)
   │              │  // fs/splice.c:1202 — 核心循环
   │              │
   │              ├─ 创建内部匿名 pipe
   │              │  // 使用 pipe_create 创建 pipe_inode_info
   │              │  // pipe->bufs[16] 环形缓冲区
   │              │  // pipe->readers = 1, pipe->writers = 1
   │              │
   │              ├─ [循环: sd->total_len > 0]
   │              │  │
   │              │  ├─ do_splice_read(in, &sd->pos, pipe, ...)
   │              │  │  └─ filemap_splice_read → add_to_pipe
   │              │  │     // 页缓存 folio → pipe buffer（零拷贝页面引用）
   │              │  │
   │              │  ├─ sd->total_len -= bytes
   │              │  │
   │              │  └─ [循环: bytes > 0]
   │              │     └─ direct_splice_actor(pipe, &sd)
   │              │        └─ out->f_op->splice_write(pipe, out, ...)
   │              │           │  // 对于 socket: pipe_to_sendpage
   │              │           │  // 将 pipe buffer 的页面描述符传给 socket
   │              │           │  // → tcp_sendpage → skb 零拷贝
   │              │           │
   │              │           └─ [对于普通文件: iter_file_splice_write]
   │              │              └─ call_write_iter(out, &kiocb, &iter)
   │              │                 └─ ext4_file_write_iter → generic_perform_write
   │              │                    └─ copy_folio_from_iter_atomic  // CPU 拷贝
   │              │
   │              └─ 销毁内部 pipe
   │
   ├─ [retval > 0]
   │  ├─ add_rchar(current, retval)                              // 读字节统计
   │  ├─ add_wchar(current, retval)                              // 写字节统计
   │  ├─ fsnotify_access(fd_file(in))                            // 源文件访问通知
   │  ├─ fsnotify_modify(fd_file(out))                           // 目标文件修改通知
   │  ├─ fd_file(out)->f_pos = out_pos                           // 更新目标 f_pos
   │  ├─ [ppos != NULL] → *ppos = pos                            // 更新用户偏移
   │  └─ [ppos == NULL] → fd_file(in)->f_pos = pos              // 更新源 f_pos
   │
   ├─ inc_syscr(current)                                          // 读系统调用计数
   └─ inc_syscw(current)                                          // 写系统调用计数
```

---

## 6 零拷贝数据传输路径

```
sendfile 文件→socket:
  [ext4 页缓存 folio]  ──页面引用──→ [pipe buffer]  ──sendpage──→ [socket sk_buff]
                                     零拷贝            零拷贝
                                    ↑ add_to_pipe      ↑ pipe_to_sendpage

sendfile 文件→pipe:
  [ext4 页缓存 folio]  ──页面引用──→ [pipe buffer]
                                     零拷贝

sendfile 文件→文件:
  [ext4 页缓存 folio]  ──页面引用──→ [pipe buffer]  ──iov_iter映射──→ [file_out 页缓存]
                                     零拷贝                  CPU 拷贝（write_iter）
```

---

## 7 性能分析

| 操作 | 数据拷贝次数 | 主要开销 | 适用场景 |
|--|--|--|--|
| `sendfile(file→socket)` | 0 次 | pipe 锁定 + 上下文切换 | 静态文件服务器 |
| `sendfile(file→file)` | 1 次 | CPU 拷贝到目标页缓存 | NFS gateways |
| `sendfile(file→pipe)` | 0 次 | pipe 锁定 | 管道传输 |
| read + write（用户态） | 2 次 | 用户态缓冲 | 通用（最差性能） |
| mmap + write | 1 次 | page fault + CPU 拷贝 | 中等场景 |

---

## 8 关键数据结构 (C代码 + 注释)

```c
// ===== VFS 层 =====

// sendfile 的分发参数结构——控制内部 pipe 中转的完整上下文
struct splice_desc {
    size_t len;                    // 当前要传输的字节数
    size_t total_len;              // 总传输字节数
    unsigned int flags;            // SPLICE_F_* 标志（SPLICE_F_MOVE 等）
    loff_t pos;                    // 源文件当前偏移（sendfile 从 offset 或 f_pos 获取）
    loff_t *opos;                  // 目标文件偏移指针（sendfile 更新 out_fd 的 f_pos）
    union {
        struct file *file;         // 目标文件（sendfile 的输出文件/socket）
    } u;
    const struct pipe_buf_operations *ops;  // pipe buffer 操作函数表
    size_t used;                   // 已使用的字节数
};

// 管道信息结构体——sendfile 内部中转 pipe
struct pipe_inode_info {
    struct mutex mutex;            // 互斥锁
    wait_queue_head_t rd_wait;     // 读者等待队列
    wait_queue_head_t wr_wait;     // 写者等待队列
    unsigned int head;             // 头指针（写者写入位置）
    unsigned int tail;             // 尾指针（读者读出位置）
    unsigned int max_usage;        // 最大使用量
    unsigned int ring_size;        // 环形缓冲区大小（PIPE_DEF_BUFFERS = 16）
    bool readers;                  // 是否有读者
    bool writers;                  // 是否有写者
    struct pipe_buffer *bufs;      // 环形缓冲区数组
};

// 管道缓冲区条目——sendfile 零拷贝的核心载体
struct pipe_buffer {
    struct page *page;             // ★ 页面指针（零拷贝关键：只传递引用，不拷贝数据）
    unsigned int offset;           // 页内偏移
    unsigned int len;              // 有效数据长度
    const struct pipe_buf_operations *ops;  // 操作函数表（release/confirm/steal）
    unsigned int flags;            // PIPE_BUF_FLAG_* 标志
};

// 文件对象——sendfile 的输入/输出文件
struct file {
    struct path f_path;            // 文件路径
    struct inode *f_inode;         // 指向 inode
    const struct file_operations *f_op;  // 文件操作函数表
    atomic_long_t f_count;         // 引用计数
    loff_t f_pos;                  // 当前读写位置（sendfile offset=NULL 时使用）
    fmode_t f_mode;                // 打开模式（FMODE_READ, FMODE_PREAD 等）
    unsigned int f_flags;          // 文件状态标志（O_APPEND 等）
};

// I/O 控制块——用于写路径的 kiocb
struct kiocb {
    struct file *ki_filp;          // 目标文件
    loff_t ki_pos;                 // 写入位置
    unsigned short ki_flags;       // IOCB_* 标志
    short ki_ioprio;               // I/O 优先级
};

// ===== Page Cache 层 =====

// folio——页缓存单元，sendfile 读取源文件时通过 add_to_pipe 传递页面引用
struct folio {
    unsigned long flags;           // PG_uptodate（数据有效）、PG_locked（锁定）等
    struct address_space *mapping; // 所属的 address_space
    loff_t index;                  // 文件内页索引
    atomic_t _refcount;            // 引用计数（pipe buffer 引用 + 页缓存引用）
};

// ===== 网络层（socket 路径）=====

// 套接字缓冲区——sendfile 文件→socket 时，pipe buffer 页面直接挂载到 skb
struct sk_buff {
    struct sock *sk;               // 所属 socket
    struct skb_shared_info *shinfo; // 共享信息（包含 frags[] 数组）
    // 零拷贝关键：shinfo->frags[] 直接引用 pipe buffer 的页面
    // 通过 tcp_sendpage → skb_set_frag_page 实现
};

// 网络设备 DMA 数据片段——sendfile 零拷贝的最终载体
struct skb_shared_info {
    struct skb_shared_hwtstamps hwtstamps;  // 硬件时间戳
    unsigned int nr_frags;                  // 片段数
    skb_frag_t frags[MAX_SKB_FRAGS];        // 数据片段数组（页面引用）
};
```

| 数据结构 | 头文件 | 在 sendfile 中的作用 |
|----------|--------|------------------|
| `struct splice_desc` | `include/linux/splice.h` | 控制内部 pipe 中转的传输参数 |
| `struct pipe_inode_info` | `include/linux/pipe_fs_i.h` | 内部匿名 pipe，作为零拷贝中转 |
| `struct pipe_buffer` | `include/linux/pipe_fs_i.h` | 页面引用传递的载体（零拷贝关键） |
| `struct file` | `include/linux/fs.h` | 输入/输出文件描述符 |
| `struct kiocb` | `include/linux/fs.h` | 写路径的 I/O 控制块 |
| `struct folio` | `include/linux/mm_types.h` | 页缓存单元，通过 add_to_pipe 传递 |
| `struct sk_buff` | `include/linux/skbuff.h` | socket 路径的页面承载 |
| `struct skb_shared_info` | `include/linux/skbuff.h` | 数据片段数组，指向 pipe buffer 页面 |

---

## 9 总结

sendfile 代表 Linux 内核中**最高效的文件传输路径**：

1. **文件→socket**：真正的**零拷贝**路径。通过创建内部匿名 pipe，从文件读取时将页缓存 folio 的页面引用传递到 pipe buffer（`add_to_pipe`），再通过 `sendpage` 将 pipe buffer 的页面描述符传递给网络协议栈（`pipe_to_sendpage`）。整个过程**没有一次 CPU 数据拷贝**。

2. **文件→pipe**：直接拼接路径（`splice_file_to_pipe`），同样通过 `add_to_pipe` 的页面引用传递实现零拷贝。

3. **文件→文件**：通过 `do_splice_direct` 内部 pipe 中转，读路径零拷贝（页面引用传递），写路径需要 CPU 拷贝到目标文件页缓存（`copy_folio_from_iter_atomic`），涉及**一次 CPU 拷贝**。
