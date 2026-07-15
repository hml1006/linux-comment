# splice / tee / vmsplice 系统调用完整路径分析

## 1 概述

splice、tee 和 vmsplice 是 Linux 特有的**零拷贝**系统调用族，通过管道缓冲区（pipe buffer）在文件描述符之间传递数据，避免用户空间的 CPU 数据拷贝。

### 关键特点

- **零拷贝（Zero-Copy）**：数据在内核内直接传递，不经过用户空间缓冲区
- **管道中介**：至少一端必须是管道（pipe），pipe buffer 传递的是页面引用而非数据
- **splice(fd_in, fd_out)**：文件↔管道或管道↔文件（和管道↔管道），消费输入
- **tee(fd_in, fd_out)**：管道→管道，拷贝页面引用但不消费输入（输入保留）
- **vmsplice(fd, iov)**：用户页面↔管道（写：`GUP` 锁定用户页；读：`copy_page_to_iter`）
- **SPLICE_F_MOVE**：预期移交页面所有权（当前内核忽略，等同于 SPLICE_F_MOVE 优化合并）
- **SPLICE_F_GIFT**：vmsplice 专用，用户放弃页面所有权（无需拷贝）

---

## 2 涉及的内核层

| 层                      | 说明                                                                |
| ----------------------- | ------------------------------------------------------------------- |
| **Syscall Entry** | splice/tee/vmsplice 系统调用入口 (fs/splice.c)                      |
| **pipe 层**       | pipe buffer 操作 (fs/pipe.c, include/linux/pipe_fs_i.h)             |
| **VFS**           | do_splice / do_splice_read / splice_file_to_pipe / do_splice_direct |
| **ext4**          | file 读/写路径（当 splice 从/向常规文件时）                         |
| **Page Cache**    | page 引用传递 / GUP 锁定                                            |
| **Block Layer**   | 当常规文件缺页时（读路径进入块设备）                                |
| **NVMe 驱动**     | 仅当常规文件读缺页时触及                                            |

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

### 3.2 SYSCALL_DEFINE4(tee) - fs/splice.c:1977

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

### 3.3 SYSCALL_DEFINE4(vmsplice) - fs/splice.c:1578

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
        // pipe → pipe (tee 风格)
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

### 4.3 四种 splice 传输模式

| 输入   | 输出 | 函数路径                                       | 数据传递方式                          |
| ------ | ---- | ---------------------------------------------- | ------------------------------------- |
| 管道   | 管道 | `splice_pipe_to_pipe`                        | pipe buffer 引用传递（零拷贝）        |
| 管道   | 文件 | `splice_from_pipe` → `f_op->write_iter`   | pipe buffer → 页缓存（可能拷贝）     |
| 文件   | 管道 | `splice_file_to_pipe` → `f_op->read_iter` | 页缓存 → pipe buffer（零拷贝）       |
| 用户页 | 管道 | `vmsplice_to_pipe` → `iter_to_pipe`       | GUP 锁定 → pipe buffer（零拷贝）     |
| 管道   | 用户 | `vmsplice_to_user` → `pipe_to_user`       | pipe → copy_page_to_iter（需要拷贝） |

---

## 5 file→pipe 路径（零拷贝读）

```text
# splice file→pipe 零拷贝读取路径
#
# 数据流: 页缓存 folio → pipe buffer (页面引用传递, 不拷贝数据)
# 调用链: do_splice → splice_file_to_pipe → do_splice_read → ext4_file_splice_read
# 关键: add_to_pipe 将 folio 的 page 指针直接添加到 pipe ring buffer

do_splice(file_in, &pos, opipe, NULL, len, flags)
  │  # 路由到 file→pipe 路径 (opipe != NULL)
  │
  └─ splice_file_to_pipe(file_in, opipe, &pos, len, flags)
      │  # fs/splice.c:1280
      │  # file→pipe 的入口函数
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
      │   │  # fs/splice.c:1290
      │   │  # 初始化 splice_desc 结构体:
      │   │  #   sd.len = len          # 剩余要读的字节数
      │   │  #   sd.total_len = len    # 总字节数
      │   │  #   sd.flags = flags      # 传递 splice 标志
      │   │  #   sd.pos = *offset      # 文件偏移
      │   │  #   sd.u.file = in        # 源文件
      │   │
      │   └─ in->f_op->splice_read(file_in, &sd)
      │       │  # VFS 层调用, 实际调用文件系统注册的 splice_read
      │       │  # 对于 ext4: ext4_file_splice_read
      │       │  # 对于不支持 splice_read 的文件系统: 回退到 generic_file_splice_read
      │       │
      │       └─ ext4_file_splice_read(in, ppos, pipe, len, flags)
      │           │  # fs/ext4/file.c
      │           │  # 设置读取标志, 调用通用 filemap_splice_read
      │           │
      │           └─ filemap_splice_read(in, ppos, pipe, len, flags)
      │               │  # mm/filemap.c
      │               │  # 核心: 逐 folio 处理, 将页面引用添加到 pipe
      │               │
      │               └─ [for each folio in file range]:
      │                   │  # 遍历文件偏移范围内的每个 folio
      │                   │  # folio = 复合页 (可以是 order-0 的普通页或大页)
      │                   │
      │                   ├─ filemap_get_folio(mapping, index)
      │                   │   │  # mm/filemap.c
      │                   │   │  # 在页缓存中查找 folio
      │                   │   │  # 页缓存 key = (mapping, index)
      │                   │   │
      │                   │   ├─ [命中] 直接返回 folio 指针
      │                   │   │   # folio 已在页缓存中, 无需读磁盘
      │                   │   │   # 增加 folio 引用计数
      │                   │   │
      │                   │   └─ [未命中] 触发缺页读
      │                   │       │  # 页缓存中没有该 folio
      │                   │       │
      │                   │       ├─ page_cache_sync_readahead
      │                   │       │   # 同步预读: 提前加载后续页面
      │                   │       │   # 减少磁盘 I/O 次数
      │                   │       │
      │                   │       └─ filemap_read_folio
      │                   │           │  # 从磁盘读取一个 folio
      │                   │           │
      │                   │           └─ ext4_read_folio
      │                   │               # ext4 文件系统层
      │                   │               # 分配 bio, 提交到块层
      │                   │               # 等待 I/O 完成, folio 标记为 uptodate
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
      │                       │  # buf.flags  = 0
      │                       │  # 不拷贝数据! 只传递 struct page 指针
      │                       │
      │                       └─ 唤醒 pipe 读者
      │                           # wake_up_interruptible_sync_poll(&pipe->wait, EPOLLIN)
      │                           # 通知等待数据的进程
      │
      ├─ pipe_unlock(opipe)
      │   # 释放 pipe 锁
      │
      └─ wakeup_pipe_readers(opipe)
          # 最终唤醒所有等待的 pipe 读者
```

**零拷贝的关键**：`add_to_pipe` 将 `folio` 的**页面引用**直接添加到 pipe buffer 中，不拷贝数据。pipe buffer 结构包含 `page` 指针、`offset` 和 `len`。

---

## 6 pipe→ 路径

```text
# splice pipe→ 写入路径
#
# 数据流: pipe buffer 页面 → iov_iter → 文件页缓存 (需要 1 次 CPU 拷贝)
# 调用链: do_splice → splice_from_pipe → do_splice_from → ext4_file_splice_write → iter_file_splice_write
# 关键: 将 pipe buffer 的页面重新映射为 iov_iter, 通过常规 write_iter 写入

do_splice(ipipe, NULL, file_out, &pos, len, flags)
  │  # 路由到 pipe→ 路径 (ipipe NULL)
  │
  └─ splice_from_pipe(ipipe, out, &offset, len, flags)
      │  # fs/splice.c:1250
      │  # pipe→ 的入口函数
      │  # 构造 splice_desc, 初始化 sd.len/total_len/flags/pos/splice_eof
      │
      ├─ pipe_lock(ipipe)
      │   # 获取 pipe 互斥锁
      │
      ├─ ipipe_prep(ipipe, flags)
      │   # 等待 pipe 有数据可读
      │   # 如果 pipe 空且非 NONBLOCK, 睡眠等待写者产生数据
      │
      └─ do_splice_from(ipipe, out, &offset, len, flags)
          │  # fs/splice.c:1234
          │  # 调用 __splice_from_pipe 部署 splice_from_pipe_actor
          │
          └─ __splice_from_pipe(pipe, &sd, splice_from_pipe_actor)
              │  # fs/splice.c:1120
              │  # 通用 pipe 消费框架: 遍历 pipe buffer 并调用 actor
              │
              ├─ wait_for_space(pipe, flags) 前的准备
              │   # 检查 pipe 有数据, 否则等待
              │
              └─ [for each pipe buffer in pipe]:
                  │  # 从 pipe->tail 开始逐个处理 pipe buffer
                  │
                  └─ splice_from_pipe_actor(pipe, buf, sd)
                      │  # fs/splice.c:1180
                      │  # actor 回调: 将单个 pipe buffer 写入文件
                      │
                      ├─ 构造 I/O 参数
                      │   # init_sync_kiocb(&kiocb, out)  ← 同步 kiocb
                      │   # iov_iter_init(&iter, ITER_SOURCE, &iov, 1, buf->len)
                      │   # iov.iov_base = page_address(buf->page) + buf->offset
                      │   # kiocb.ki_pos = *ppos   ← 文件写入位置
                      │
                      └─ call_write_iter(out, &kiocb, &iter)
                          │  # VFS 层调用, 实际调用文件系统注册的 write_iter
                          │
                          └─ ext4_file_write_iter(iocb, from)
                              │  # fs/ext4/file.c
                              │  # ext4 文件系统写入口
                              │
                              └─ generic_perform_write(iocb, from)
                                  │  # mm/filemap.c
                                  │  # 核心: 逐 folio 写入文件页缓存
                                  │
                                  └─ [for each folio in write range]:
                                      │  # 遍历写入范围内的每个 folio
                                      │
                                      ├─ 查找/分配目标 folio
                                      │   # 在文件页缓存中查找或分配新 folio
                                      │
                                      ├─ copy_folio_from_iter_atomic(folio, offset, bytes, i)
                                      │   │  # ★ CPU 拷贝发生在这里 ★
                                      │   │  # 将 iov_iter 中的数据拷贝到 folio 页面
                                      │   │  # 拷贝量 = min(folio_size - offset, bytes)
                                      │   │  # 原子上下文: 使用 kmap_local 映射页面
                                      │   │
                                      │   └─ 数据从 pipe buffer page → 文件页缓存 page
                                      │       # 涉及 1 次 CPU 内存拷贝
                                      │
                                      └─ folio_mark_uptodate(folio)
                                          # 标记 folio 为 uptodate
                                          # 后续同步到磁盘由 writeback 机制处理
```

该路径将 pipe buffer 中的页面**重新映射到 iov_iter**，通过常规的 `write_iter` 路径写入目标文件。数据从 pipe buffer 的 page 经由 `copy_folio_from_iter_atomic` 拷贝到文件页缓存（**涉及一次 CPU 拷贝**）。

---

## 7 pipe→pipe（tee 路径）

```text
# splice/tee pipe→pipe 零拷贝路径
#
# 数据流: pipe buffer 页面引用 → pipe buffer (零拷贝)
# 调用链: do_splice/do_tee → splice_pipe_to_pipe
# 关键: splice 消费(tail++), tee 不消费(仅增加引用计数)
# 共同点: 都通过 obuf->page = ibuf->page 传递页面指针, 不拷贝数据

do_splice(ipipe, &off, opipe, NULL, len, flags)   ← splice(pipe→pipe)
  │  # 路由到 pipe→pipe 路径
  │
  └─ splice_pipe_to_pipe(ipipe, opipe, len, flags)
      └─ ... (见下方主流程)

do_tee(in, out, len, flags)                        ← tee(pipe→pipe)
  │  # fs/splice.c:1955
  │  # 校验两端都是管道, 否则返回 -EINVAL
  │
  └─ splice_pipe_to_pipe(ipipe, opipe, len, flags)
      └─ ... (见下方主流程)

# ═══════════════════════════════════════════════════════════════
# splice_pipe_to_pipe 主流程 (fs/splice.c:1716)
# ═══════════════════════════════════════════════════════════════

splice_pipe_to_pipe(ipipe, opipe, len, flags)
  │  # fs/splice.c:1716
  │  # splice 和 tee 的核心实现
  │  # 参数: ipipe=管道, opipe=输出管道, len=传输字节数, flags=SPLICE_F_*
  │
  ├─ ipipe_prep(ipipe, flags)
  │   # 等待管道有数据可读
  │   # 如果 pipe 空且非 NONBLOCK, 睡眠等待
  │
  ├─ opipe_prep(opipe, flags)
  │   # 等待输出管道有可用空间
  │   # 如果 pipe 满且非 NONBLOCK, 睡眠等待
  │
  ├─ pipe_double_lock(ipipe, opipe)
  │   # 按地址排序后加锁, 避免 ABBA 死锁
  │   # 如果 ipipe == opipe 只加一次锁
  │
  └─ [主循环: 遍历 pipe buffer]:
      │  # 从 ipipe->tail 开始逐个处理 pipe buffer
      │
      ├─ 取 pipe buffer
      │   # ibuf = &ipipe->bufs[ipipe->tail & mask]
      │   # 读取: ibuf->page, ibuf->offset, ibuf->len, ibuf->ops, ibuf->flags
      │
      ├─ 计算可传输长度
      │   # 检查 opipe 剩余空间: nrbufs = opipe->max_usage - (opipe->head - opipe->tail)
      │   # this_len = min(ibuf->len, space_in_opipe)
      │   # 如果 opipe 满了, 等待空间
      │
      ├─ 写入输出 pipe buffer
      │   # obuf = &opipe->bufs[opipe->head & mask]
      │   # obuf->page   = ibuf->page     ← 页面指针传递 (零拷贝!)
      │   # obuf->offset = ibuf->offset    ← 页内偏移
      │   # obuf->len    = this_len        ← 数据长度
      │   # obuf->ops    = ibuf->ops       ← 操作函数表 (release/confirm)
      │   # obuf->flags  = ibuf->flags     ← PIPE_BUF_FLAG_* 标志
      │   # opipe->head++                  ← 推进输出 head
      │
      ├─ 更新 pipe buffer
      │   # if (splice 消费模式):
      │   #   ibuf->offset += this_len     ← 推进页内偏移
      │   #   ibuf->len    -= this_len     ← 减少剩余长度
      │   #   if (ibuf->len == 0):
      │   #     ipipe->tail++              ← ★ splice 推进 tail (消费数据) ★
      │   #     pipe_buf_release(ipipe, ibuf)  ← 释放 pipe buffer
      │   #
      │   # if (tee 保留模式):
      │   #   ibuf->offset 和 ibuf->len 不变
      │   #   ipipe->tail 不动                ← ★ tee 不推进 tail (保留数据) ★
      │   #   仅增加 page 引用计数: get_page(ibuf->page)
      │
      ├─ count += this_len
      │   # 累加已传输字节数
      │
      └─ continue while (len > 0 opipe 有空间)
          # 循环直到请求长度完成或输出管道满

  ├─ pipe_unlock(opipe)
  └─ pipe_unlock(ipipe)
      # 按地址序释放锁

  ├─ wakeup_pipe_readers(opipe)
  │   # 唤醒等待输出 pipe 的读者
  │
  └─ wakeup_pipe_writers(ipipe)  [仅 splice 消费模式]
      # 唤醒等待 pipe 的写者 (数据被消费后有新空间)
```

**tee 与 splice 的核心差异**：

- **splice（pipe→pipe）**：消费 pipe 的数据（tail 推进），数据减少，`pipe_buf_release` 释放 buffer
- **tee（pipe→pipe）**：不消费 pipe（tail 不动），数据保留，仅通过 `get_page` 增加 `page` 的引用计数
- 两者都通过 `obuf->page = ibuf->page` 传递页面指针，实现**零拷贝**
- tee 的输出 buffer 释放时调用 `ibuf->ops->release` 减少引用计数，不会过早释放页面

---

## 8 vmsplice 用户页→pipe（零拷贝写）

```text
# vmsplice 用户→pipe 零拷贝路径
#
# 数据流: 用户空间页面 → GUP 锁定 → pipe buffer (零拷贝)
# 调用链: vmsplice_to_pipe → iter_to_pipe → iov_iter_get_pages2 → get_user_pages_fast
# 关键: GUP pin 用户页面, 不拷贝数据, 只传递 page 指针

vmsplice_to_pipe(file, iter, flags)
  │  # fs/splice.c:1534
  │  # vmsplice 用户→pipe 的入口
  │  # 参数: file=管道文件, iter=用户空间 iov_iter, flags=SPLICE_F_GIFT 等
  │
  ├─ get_pipe_info(file) → pipe
  │   # 获取 pipe_inode_info 指针, 如果不是管道返回 -EINVAL
  │
  ├─ 确定 buf_flag
  │   # if (flags & SPLICE_F_GIFT):
  │   #   buf_flag = PIPE_BUF_FLAG_GIFT  ← 用户放弃页面所有权
  │   # else:
  │   #   buf_flag = 0                   ← 正常引用计数管理
  │
  ├─ pipe_lock(pipe)
  │   # 获取 pipe 互斥锁
  │
  ├─ wait_for_space(pipe, flags)
  │   # 等待 pipe 有可用空间
  │   # 如果 pipe 满且非 NONBLOCK, 睡眠等待
  │
  ├─ iter_to_pipe(iter, pipe, buf_flag)
  │   │  # fs/splice.c:1443
  │   │  # 核心: 遍历 iov_iter 中的用户页面, 添加到 pipe
  │   │
  │   └─ [while iov_iter_count(from) > 0]:
  │       │  # 循环直到 iov_iter 中所有数据都处理完
  │       │
  │       ├─ 计算本次处理量
  │       │   # maxsize = iov_iter_count(from)  ← 剩余数据
  │       │   # 限制: 最多一次锁定 16 个页面 (PIPE_BUF_FLAG_WHOLE)
  │       │
  │       ├─ iov_iter_get_pages2(from, pages, maxsize, 16, &start)
  │       │   │  # lib/iov_iter.c
  │       │   │  # 从 iov_iter 获取用户空间页面
  │       │   │  # pages[]: 输出参数, 存放页面指针数组
  │       │   │  #: 输出参数, 第一个页面的页内偏移
  │       │   │  # 返回: 获取的总字节数
  │       │   │
  │       │   └─ get_user_pages_fast(addr, nr_pages, gup_flags, pages)
  │       │       │  # mm/gup.c
  │       │       │  # 快速路径: 锁定(pin)用户空间页面
  │       │       │  # gup_flags = FOLL_WRITE | FOLL_LONGTERM (如需)
  │       │       │  # 遍历页表, 增加 page->_refcount
  │       │       │  # 页面不会被换出或移动 (pinned)
  │       │       │
  │       │       └─ 返回 pinned 页面数组
  │       │           # pages[0..nr_pages-1] = struct page* 指针
  │       │
  │       └─ [for each pinned page]:
  │           │  # 将每个锁定页面添加到 pipe ring buffer
  │           │
  │           ├─ 构造 pipe_buffer
  │           │   # buf.page   = pages[i]        ← 用户页面指针 (零拷贝!)
  │           │   # buf.offset =            ← 第一个页面的页内偏移
  │           │   # buf.len    = min(PAGE_SIZE -, remaining)
  │           │   # buf.ops    = 根据 buf_flag:
  │           │   #   无 GIFT: user_page_pipe_buf_ops (正常引用计数)
  │           │   #   有 GIFT: nosteal_pipe_buf_ops  (用户放弃所有权)
  │           │   # buf.flags  = buf_flag
  │           │
  │           └─ add_to_pipe(pipe, &buf)
  │               # 将 buffer 添加到 pipe 环形缓冲区
  │               # 唤醒等待的 pipe 读者
  │
  ├─ pipe_unlock(pipe)
  │   # 释放 pipe 锁
  │
  └─ wakeup_pipe_readers(pipe)
      # 最终唤醒所有等待的 pipe 读者
```

### 8.1 SPLICE_F_GIFT 标志

```c
if (flags & SPLICE_F_GIFT)
    buf_flag = PIPE_BUF_FLAG_GIFT;
```

- **无 SPLICE_F_GIFT**：pipe buffer 的 `ops` 设为 `user_page_pipe_buf_ops`
  - 接收方读取后，`page` 引用计数递减，页面返回给用户
- **有 SPLICE_F_GIFT**：用户放弃页面所有权（"gift"）
  - pipe buffer 释放时直接释放页面
  - 避免额外的引用计数操作
  - 需要用户保证不再使用该页面

### 8.2 vmsplice pipe→用户（非零拷贝）

```text
# vmsplice pipe→用户 非零拷贝路径
#
# 数据流: pipe buffer 页面 → copy_page_to_iter → 用户空间 (需要 1 次 CPU 拷贝)
# 调用链: vmsplice_to_user → __splice_from_pipe → pipe_to_user → copy_page_to_iter
# 关键: 由于用户空间页面和内核页面不可直接交换, 必须 CPU 拷贝

vmsplice_to_user(file, iter, flags)
  │  # fs/splice.c:1501
  │  # vmsplice pipe→用户 的入口
  │  # 参数: file=管道文件, iter=用户空间 iov_iter (ITER_DEST), flags=SPLICE_F_*
  │
  ├─ get_pipe_info(file) → pipe
  │   # 获取 pipe_inode_info 指针
  │
  ├─ pipe_lock(pipe)
  │   # 获取 pipe 互斥锁
  │
  ├─ ipipe_prep(pipe, flags)
  │   # 等待 pipe 有数据可读
  │
  └─ __splice_from_pipe(pipe, &sd, pipe_to_user)
      │  # fs/splice.c:1120
      │  # 通用 pipe 消费框架, actor = pipe_to_user
      │  # sd.u.data = &iter  ← 用户空间 iov_iter
      │
      └─ [for each pipe buffer]:
          │  # 从 pipe->tail 开始逐个处理 pipe buffer
          │
          └─ pipe_to_user(pipe, buf, sd)
              │  # fs/splice.c:1150
              │  # actor 回调: 将单个 pipe buffer 拷贝到用户空间
              │  # 参数: pipe=输入管道, buf=当前 pipe buffer, sd=splice_desc
              │
              ├─ 计算拷贝长度
              │   # this_len = min(buf->len, sd->len)
              │   # 取 pipe buffer 可用数据和 splice_desc 请求长度的最小值
              │
              └─ copy_page_to_iter(buf->page, buf->offset, sd->len, sd->u.data)
                  │  # 将页面数据拷贝到用户空间 iov_iter
                  │  # 内部调用 copy_to_user (涉及 1 次 CPU 拷贝)
                  │  # ★ 这是零拷贝路径中唯一的 CPU 拷贝 ★
                  │
                  └─ 数据从 pipe buffer page → 用户空间缓冲区
                      # 更新 pipe buffer: buf->offset += ret, buf->len -= ret
                      # 如果 buf->len == 0: pipe_buf_release(pipe, buf)
```

> 从 pipe 读取数据到用户空间**不是**零拷贝的。由于用户空间页面和内核页面不可直接交换（vm tricks 太复杂且受限），当前实现通过 `copy_page_to_iter` 进行数据拷贝。

---

## 9 完整流程总图

```text
# splice / tee / vmsplice 系统调用总体流程
#
# 三个系统调用最终都汇聚到 pipe buffer 操作
# 核心数据结构: pipe_inode_info (环形缓冲区), pipe_buffer (页面引用)
# 关键概念: 零拷贝通过页面引用传递实现, 不拷贝数据本身

# ═══════════════════════════════════════════════════════════════
# 系统调用入口层
# ═══════════════════════════════════════════════════════════════

SYSCALL_DEFINE6(splice, fd_in, off_in, fd_out, off_out, len, flags)
  │  # fs/splice.c:1616
  │  # splice 是最通用的零拷贝系统调用, 至少一端必须是管道
  │  # 参数: fd_in/fd_out=文件描述符, off_in/off_out=偏移量(NULL=使用当前文件位置)
  │  #       len=传输字节数, flags=SPLICE_F_MOVE|SPLICE_F_NONBLOCK|SPLICE_F_MORE
  │
  └─ __do_splice(in, off_in, out, off_out)
      │  # fs/splice.c:1397
      │  # 清除 NOWAIT 标志, 从用户空间拷贝偏移量(如非 NULL)
      │  # 调用完成后将偏移量拷回用户空间
      │
      └─ do_splice(in, off_in, out, off_out, len, flags)  ← 路由核心
          │  # fs/splice.c:1300
          │  # 通过 get_pipe_info() 判断输入/输出是否为管道
          │  # 根据四种组合选择不同路径:
          │
          ├─ [file→pipe] splice_file_to_pipe → do_splice_read
          │   │  # 零拷贝读取: 页缓存 folio → pipe buffer (页面引用传递)
          │   │  # 调用链: file→pipe 路径 (详见第 5 节)
          │   │
          │   └─ ... (见下方第 5 节详细调用栈)
          │
          ├─ [pipe→file] splice_from_pipe → do_splice_from
          │   │  # 需要 1 次 CPU 拷贝: pipe buffer 页面 → 文件页缓存
          │   │  # 调用链: pipe→file 路径 (详见第 6 节)
          │   │
          │   └─ ... (见下方第 6 节详细调用栈)
          │
          ├─ [pipe→pipe] splice_pipe_to_pipe
          │   │  # 零拷贝: pipe buffer 引用共享, 消费输入数据(tail 推进)
          │   │  # 调用链: pipe→pipe 路径 (详见第 7 节)
          │   │
          │   └─ ... (见下方第 7 节详细调用栈)
          │
          └─ [file→file] 返回 -EINVAL
              # splice 不支持两端都是常规文件
              # 可用 copy_file_range 代替

SYSCALL_DEFINE4(tee, fdin, fdout, len, flags)
  │  # fs/splice.c:1977
  │  # tee 是 splice 的 "保留输入" 版本, 两端都必须是管道
  │  # 与 splice(pipe→pipe) 的区别: 不消费输入数据
  │  # 参数: fdin/fdout=管道文件描述符, len=传输字节数
  │  #       flags=SPLICE_F_NONBLOCK|SPLICE_F_MORE|SPLICE_F_MOVE
  │
  └─ do_tee(in, out, len, flags)
      │  # fs/splice.c:1955
      │  # 校验两端都是管道, 否则返回 -EINVAL
      │
      └─ splice_pipe_to_pipe(ipipe, opipe, len, flags)
          │  # 零拷贝: pipe buffer 引用共享
          │  # 关键差异: 不推进 ipipe->tail (tee 不消费输入)
          │  # 通过增加 page 引用计数保留数据, 可多次读取
          │
          └─ ... (见下方第 7 节详细调用栈)

SYSCALL_DEFINE4(vmsplice, fd, uiov, nr_segs, flags)
  │  # fs/splice.c:1578
  │  # 用户空间与管道之间的零拷贝
  │  # 方向由 fd 的文件模式决定: FMODE_WRITE→用户到管道, FMODE_READ→管道到用户
  │  # 参数: fd=管道文件描述符, uiov=用户空间 iovec 数组
  │  #       nr_segs=iovec 数量, flags=SPLICE_F_GIFT 等
  │
  ├─ [用户→pipe: fd 可写]
  │   │  # 零拷贝路径: GUP 锁定用户页面 → pipe buffer
  │   │
  │   ├─ import_iovec(ITER_SOURCE, uiov, nr_segs, ...)
  │   │   # 从用户空间拷贝 iovec 数组到内核栈
  │   │   # 初始化 iov_iter 为 ITER_SOURCE(数据源) 类型
  │   │
  │   └─ vmsplice_to_pipe(file, &iter, flags)
  │       │  # fs/splice.c:1534
  │       │
  │       └─ iter_to_pipe(&iter, pipe, buf_flag)
  │           │  # fs/splice.c:1443
  │           │  # 遍历 iovec, 逐个锁定用户页面
  │           │
  │           ├─ iov_iter_get_pages2(from, pages, maxsize, 16, &start)
  │           │   │  # lib/iov_iter.c
  │           │   │  # 一次最多锁定 16 个页面 (PIPE_BUF_FLAG_WHOLE)
  │           │   │
  │           │   └─ get_user_pages_fast(addr, nr_pages, FOLL_WRITE, pages)
  │           │       # mm/gup.c
  │           │       # 固定(pin)用户页面, 返回 page 指针数组
  │           │       # 页面不会被换出或移动
  │           │
  │           └─ add_to_pipe(pipe, &buf)
  │               # 将页面引用添加到 pipe ring buffer
  │               # 设置 buf.ops:
  │               #  SPLICE_F_GIFT → PIPE_BUF_FLAG_GIFT (用户放弃所有权)
  │               #  无 GIFT → user_page_pipe_buf_ops (正常引用计数)
  │
  └─ [pipe→用户: fd 可读]
      │  # 非零拷贝路径: 需要 CPU 拷贝
      │
      ├─ import_iovec(ITER_DEST, uiov, nr_segs, ...)
      │   # 从用户空间拷贝 iovec 数组到内核栈
      │   # 初始化 iov_iter 为 ITER_DEST(数据目标) 类型
      │
      └─ vmsplice_to_user(file, &iter, flags)
          │  # fs/splice.c:1501
          │
          └─ __splice_from_pipe(pipe, &sd, pipe_to_user)
              └─ pipe_to_user(pipe, buf, sd)
                  └─ copy_page_to_iter(buf->page, buf->offset, sd->len, sd->u.data)
                      # 拷贝数据到用户空间 iovec
                      # 涉及 1 次 CPU 拷贝

# ═══════════════════════════════════════════════════════════════
# do_splice 路由决策树
# ═══════════════════════════════════════════════════════════════

do_splice(in, off_in, out, off_out, len, flags)   # fs/splice.c:1300
  │
  ├─ get_pipe_info(in, true)  → ipipe
  │   # 检查输入文件描述符是否为管道
  │   # 如果是管道, 返回 pipe_inode_info 指针
  │
  ├─ get_pipe_info(out, true) → opipe
  │   # 检查输出文件描述符是否为管道
  │
  └─ 路由决策:
      │
      ├─ ipipe && opipe → pipe→pipe
      │   # splice_pipe_to_pipe(ipipe, opipe, len, flags)
      │   # 零拷贝, 消费输入
      │
      ├─ ipipe && !opipe → pipe→file
      │   # splice_from_pipe(ipipe, out, &offset, len, flags)
      │   # 需要 1 次 CPU 拷贝
      │
      ├─ !ipipe && opipe → file→pipe
      │   # splice_file_to_pipe(in, opipe, &offset, len, flags)
      │   # 零拷贝 (页面引用传递)
      │
      └─ !ipipe && !opipe → file→file
          # 返回 -EINVAL
          # splice 不支持两端都是常规文件

# ═══════════════════════════════════════════════════════════════
# 零拷贝原理: add_to_pipe 页面引用传递
# ═══════════════════════════════════════════════════════════════

add_to_pipe(pipe, &buf)   # fs/splice.c
  │  # 将页面引用添加到 pipe 环形缓冲区
  │  # 不拷贝数据, 只传递 struct page 指针
  │
  ├─ 检查 pipe 空间: pipe->head - pipe->tail < pipe->max_usage
  │
  ├─ 将 buf 写入 pipe->bufs[head & (ring_size - 1)]
  │   # pipe buffer 内容:
  │   #   .page   = buf->page     ← 页面指针 (引用传递)
  │   #   .offset = buf->offset   ← 页内偏移
  │   #   .len    = buf->len      ← 数据长度
  │   #   .ops    = buf->ops      ← 操作函数表 (release/confirm)
  │   #   .flags  = buf->flags    ← PIPE_BUF_FLAG_*
  │
  ├─ pipe->head++
  │   # 推进 head 指针, 新数据已写入
  │
  └─ wake_up_interruptible_sync_poll(&pipe->wait, EPOLLIN)
      # 唤醒等待在 pipe 上的读者
```

---

## 10 完整函数调用链

### 10.1 splice - file→pipe 路径

| 步骤 | 函数                                                    | 文件:行号        | 说明                           |
| ---- | ------------------------------------------------------- | ---------------- | ------------------------------ |
| 1    | `SYSCALL_DEFINE6(splice)`                             | fs/splice.c:1616 | 系统调用入口                   |
| 2    | `__do_splice(in, off_in, out, off_out, len, flags)`   | fs/splice.c:1397 | 参数校验与偏移拷贝             |
| 3    | `do_splice(in, __off_in, out, __off_out, len, flags)` | fs/splice.c:1300 | 路由选择                       |
| 4    | `splice_file_to_pipe(in, opipe, &offset, len, flags)` | fs/splice.c:1280 | file→pipe 入口                |
| 5    | `do_splice_read(in, offset, opipe, len, flags)`       | fs/splice.c:1290 | 读调度                         |
| 6    | `in->f_op->splice_read(file, ppos, pipe, len, flags)` | VFS              | ext4_file_splice_read          |
| 7    | `filemap_splice_read(in, ppos, pipe, len, flags)`     | mm/filemap.c     | 逐 folio 处理                  |
| 8    | `filemap_get_folio(mapping, index)`                   | mm/filemap.c     | 查找页缓存                     |
| 9    | `page_cache_sync_readahead` / `read_folio`          | mm/readahead.c   | 缺页处理（可选）               |
| 10   | `add_to_pipe(pipe, &buf)`                             | fs/splice.c      | **零拷贝**：页面引用传递 |

### 10.2 splice - pipe→file 路径

| 步骤 | 函数                                                     | 说明                   |
| ---- | -------------------------------------------------------- | ---------------------- |
| 1-3  | 同上                                                     | 路由选择               |
| 4    | `do_splice_from(ipipe, out, ppos, len, flags)`         | pipe→file             |
| 5    | `out->f_op->splice_write(pipe, out, ppos, len, flags)` | ext4_file_splice_write |
| 6    | `iter_file_splice_write(pipe, out, ppos, len, flags)`  | fs/splice.c            |
| 7    | `call_write_iter` → `ext4_file_write_iter`          | 常规写路径             |
| 8    | `generic_perform_write` → CPU 拷贝到页缓存            | mm/filemap.c           |

### 10.3 tee - pipe→pipe 路径

| 步骤 | 函数                                              | 说明             |
| ---- | ------------------------------------------------- | ---------------- |
| 1    | `SYSCALL_DEFINE4(tee)`                          | fs/splice.c:1977 |
| 2    | `do_tee(in, out, len, flags)`                   | fs/splice.c      |
| 3    | `splice_pipe_to_pipe(ipipe, opipe, len, flags)` | fs/splice.c:1716 |
| 4    | `ipipe_prep` + `opipe_prep`                   | 准备读写         |
| 5    | 页面指针共享，不推进 tail                         | **零拷贝** |

### 10.4 vmsplice - 用户→pipe 路径

| 步骤 | 函数                                      | 说明             |
| ---- | ----------------------------------------- | ---------------- |
| 1    | `SYSCALL_DEFINE4(vmsplice)`             | fs/splice.c:1578 |
| 2    | `import_iovec(ITER_SOURCE, ...)`        | lib/iov_iter.c   |
| 3    | `vmsplice_to_pipe(file, &iter, flags)`  | fs/splice.c:1534 |
| 4    | `iter_to_pipe(iter, pipe, buf_flag)`    | fs/splice.c:1443 |
| 5    | `iov_iter_get_pages2(from, pages, ...)` | lib/iov_iter.c   |
| 6    | `get_user_pages_fast(addr, ...)`        | mm/gup.c         |
| 7    | `add_to_pipe(pipe, &buf)`               | fs/splice.c      |

---

## 11 四种系统调用的零拷贝特征对比

| 操作                           | 场景         | CPU 数据拷贝                                                     | 页面操作                       | 备注                  |
| ------------------------------ | ------------ | ---------------------------------------------------------------- | ------------------------------ | --------------------- |
| `splice(file, pipe)`         | 文件→管道   | **0 次**                                                   | 页缓存页面引用传递             | 纯零拷贝              |
| `splice(pipe, file)`         | 管道→文件   | **1 次**                                                   | pipe→iov_iter→文件页缓存     | 需 CPU 拷贝到文件页   |
| `splice(pipe, pipe)`         | 管道→管道   | **0 次**                                                   | pipe buffer 引用共享           | 零拷贝                |
| `tee(pipe, pipe)`            | 管道→管道   | **0 次**                                                   | pipe buffer 引用共享（不消费） | 零拷贝 + 保留输入     |
| `vmsplice(pipe, iov)`        | 用户页→管道 | **0 次**                                                   | GUP 锁定用户页→pipe buffer    | 零拷贝（GIFT 无开销） |
| `vmsplice(iov, pipe)`        | 管道→用户   | **1 次**                                                   | copy_page_to_iter 到用户       | 需 CPU 拷贝           |
| `sendfile(file, socket)`     | 文件→socket | **0 次**                                                   | 通过 pipe buffer 传递          | 内部使用 splice       |
| `copy_file_range(file,file)` | 文件→文件   | **0 次**（同 FS reflink）或 **1 次**（跨 FS splice） | 优先 reflink，回退 splice      | 同 FS 可零拷贝        |

---

## 12 关键数据结构

```
struct pipe_inode_info           struct pipe_buffer
+------------------------+       +----------------------+
| head (unsigned int)    |       | page (struct page*)   | ← 页面指针
| tail (unsigned int)    |       | offset (unsigned int) | ← 页内偏移
| max_usage (unsigned)   |       | len (unsigned int)    | ← 数据长度
| ring_size (unsigned)   |       | ops (pipe_buf_ops*)   | ← 操作函数
| readers (unsigned int) |       | flags (unsigned int)  | ← PIPE_BUF_FLAG_*
| writers (unsigned int) |       +----------------------+
| bufs[ring_size]        |
| → struct pipe_buffer   |       struct pipe_buf_ops
| wait (wait_queue_head) |       +----------------------+
+------------------------+       | ->confirm             |
                                  | ->release             |
struct splice_desc                | ->try_steal           |
+------------------------+       +----------------------+
| len / total_len        |
| flags (SPLICE_F_*)    |        struct iov_iter (vmsplice)
| pos (loff_t)           |       +---------------------------+
| u (file / data ptr)    |       | iter_type = ITER_IOVEC     |
| splice_eof             |       | data_source = ITER_SOURCE  |
+------------------------+       | iov → struct iovec[]      |
                                  | nr_segs / count            |
                                  +---------------------------+
```

---

## 13 总结

splice/tee/vmsplice 是 Linux **零拷贝 I/O 的核心机制**：

1. **splice**：任意文件↔管道间传输，至少一端是管道。`file→pipe` 路径通过页面引用传递实现**真正的零拷贝**；`pipe→file` 路径需要将 pipe buffer 页面拷贝到文件页缓存。
2. **tee**：pipe→pipe 传输，**不消费**输入 pipe 的数据。通过共享 pipe buffer 的 `page` 指针和增加引用计数实现零拷贝，数据在输入 pipe 中保留可再次读取。
3. **vmsplice**：用户内存↔管道。`用户→pipe` 路径通过 `get_user_pages_fast` 锁定用户页面并将页面引用传递给 pipe buffer，实现零拷贝；`pipe→用户` 路径因为技术限制只能进行 CPU 拷贝。
4. **性能关键点**：

   - 所有零拷贝路径都通过 `add_to_pipe` 在 pipe ring buffer 中传递页面引用
   - `SPLICE_F_GIFT` 消除引用计数开销
   - pipe buffer 的大小（`PIPE_DEF_BUFFERS` = 16）影响批量传输效率
