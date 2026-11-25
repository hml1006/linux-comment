# pwrite64 系统调用完整路径分析

## 1 概述

pwrite64 系统调用在指定文件偏移量处写入数据，**不改变**文件当前的 `f_pos` 值。与 write 的核心差异在于：pwrite64 使用调用者提供的 `pos` 参数（栈局部变量）而非 `file->f_pos`。

### 关键特点

- **定位语义**：与 pread64 对称，使用栈局部变量 `pos`，不更新 `file->f_pos`
- **权限检查**：需要 `FMODE_PWRITE` 标志（管道等不可定位文件返回 `-ESPIPE`）
- **写保护**：与 write 相同，调用 `file_start_write` 防止文件系统冻结
- **下游路径**：从 `vfs_write` 开始与 write 完全一致（Buffered/DIO 分叉）
- **页缓存**：Buffered 写路径通过 `generic_perform_write` 循环写入选定 folio
- **脏页回写**：异步 writeback 线程触发物理块分配和 NVMe 提交

---

## 2 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | pwrite64 系统调用入口 (fs/read_write.c) |
| **VFS** | vfs_write / rw_verify_area / file_start_write (fs/read_write.c) |
| **ext4** | ext4_file_write_iter / ext4_buffered_write_iter (fs/ext4/file.c) |
| **Page Cache** | generic_perform_write / write_begin/write_end (mm/filemap.c) |
| **ext4 延迟分配** | ext4_da_write_begin / ext4_da_write_end (fs/ext4/inode.c) |
| **Writeback** | ext4_writepages → ext4_bio_write_folio (fs/ext4/inode.c, page-io.c) |
| **Block Layer** | blk-mq 提交 (block/blk-core.c, blk-mq.c) |
| **NVMe 驱动** | 写命令提交 + 中断完成 (drivers/nvme/host/pci.c) |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE4(pwrite64) - fs/read_write.c:974

```c
SYSCALL_DEFINE4(pwrite64, unsigned int, fd, const char __user *, buf,
                 size_t, count, loff_t, pos)
{
    return ksys_pwrite64(fd, buf, count, pos);
}
```

### 3.2 ksys_pwrite64 - fs/read_write.c:958

```c
ssize_t ksys_pwrite64(unsigned int fd, const char __user *buf,
                      size_t count, loff_t pos)
{
    if (pos < 0)
        return -EINVAL;

    CLASS(fd, f)(fd);
    if (fd_empty(f))
        return -EBADF;

    if (fd_file(f)->f_mode & FMODE_PWRITE)
        return vfs_write(fd_file(f), buf, count, &pos);

    return -ESPIPE;
}
```

关键点：
1. `pos` 是调用者传入的**栈局部变量**，非 `file->f_pos`
2. `CLASS(fd, f)` 使用 Linux 6.8+ 的 **auto cleanup** 机制自动管理 fd 引用
3. `FMODE_PWRITE` 检查：管道、socket 等不可定位文件返回 `-ESPIPE`
4. 调用 `vfs_write(fd_file(f), buf, count, &pos)`——与 write 完全相同的 VFS 写入路径

### 3.3 与 ksys_write 对比

| 维度 | ksys_write | ksys_pwrite64 |
|--|--|--|
| 偏移来源 | `file->f_pos` | 调用者传入的 `pos` |
| 偏移更新 | 写入成功后更新 `f_pos` | 不更新 `f_pos`（栈局部变量） |
| 权限检查 | `FMODE_WRITE` | `FMODE_WRITE` + `FMODE_PWRITE` |
| 传给 vfs_write | `&file->f_pos` | `&pos`（栈地址） |
| VFS 下游路径 | 完全相同 | 完全相同 |

---

## 4 VFS 写分发层

### 4.1 vfs_write - fs/read_write.c:686

```c
ssize_t vfs_write(struct file *file, const char __user *buf,
                  size_t count, loff_t *pos)
{
    ssize_t ret;

    if (!(file->f_mode & FMODE_WRITE))
        return -EBADF;
    if (!(file->f_mode & FMODE_CAN_WRITE))
        return -EINVAL;

    ret = rw_verify_area(WRITE, file, pos, count);
    if (ret)
        return ret;
    if (count > MAX_RW_COUNT)
        count = MAX_RW_COUNT;

    file_start_write(file);     // → sb_start_write：防止 freeze 并发
    ret = __vfs_write(file, buf, count, pos);
    file_end_write(file);       // → sb_end_write

    if (ret > 0) {
        fsnotify_modify(file);
        add_wchar(current, ret);
    }
    inc_syscw(current);
    return ret;
}
```

### 4.2 __vfs_write - fs/read_write.c:630

```c
ssize_t __vfs_write(struct file *file, const char __user *buf,
                    size_t count, loff_t *pos)
{
    if (file->f_op->write)
        return file->f_op->write(file, buf, count, pos);
    else if (file->f_op->write_iter)
        return new_sync_write(file, buf, count, pos);
    else
        return -EINVAL;
}
```

ext4 没有实现 `f_op->write`，因此走 `new_sync_write` → `f_op->write_iter` → `ext4_file_write_iter`。

### 4.3 new_sync_write - fs/read_write.c:516

```c
static ssize_t new_sync_write(struct file *filp, const char __user *buf,
                              size_t len, loff_t *ppos)
{
    struct iovec iov = { .iov_base = (void __user *)buf, .iov_len = len };
    struct kiocb kiocb;
    struct iov_iter iter;
    ssize_t ret;

    init_sync_kiocb(&kiocb, filp);        // 初始化同步 kiocb
    kiocb.ki_pos = *ppos;                 // 将 ppos（栈 pos）写入 kiocb
    kiocb.ki_flags = 0;
    iov_iter_init(&iter, ITER_SOURCE, &iov, 1, len);  // ITER_SOURCE = 写方向

    ret = call_write_iter(filp, &kiocb, &iter);  // → ext4_file_write_iter
    if (ret > 0)
        *ppos = kiocb.ki_pos;      // 更新 pos（对 pwrite64 无影响，栈变量）
    return ret;
}
```

---

## 5 ext4 文件系统层

### 5.1 ext4_file_write_iter - fs/ext4/file.c:844

```c
static ssize_t ext4_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct inode *inode = file_inode(iocb->ki_filp);

    if (!iocb->ki_filp->f_mode & FMODE_WRITE)
        return -EBADF;

    if (unlikely(ext4_forced_shutdown(inode->i_sb)))
        return -EIO;

    // DAX 路径
    if (IS_DAX(inode))
        return ext4_dax_write_iter(iocb, from);

    // 支持原子写（atomic write）
    if (iocb->ki_flags & IOCB_ATOMIC)
        return ext4_atomic_write_iter(iocb, from);

    // DirectIO 路径
    if (iocb->ki_flags & IOCB_DIRECT)
        return ext4_dio_write_iter(iocb, from);

    // Buffered 写路径（最常用）
    return ext4_buffered_write_iter(iocb, from);
}
```

### 5.2 ext4_buffered_write_iter - fs/ext4/file.c:348

```c
static ssize_t ext4_buffered_write_iter(struct kiocb *iocb,
                                        struct iov_iter *from)
{
    ssize_t ret;
    struct inode *inode = file_inode(iocb->ki_filp);

    inode_lock(inode);        // i_rwsem 互斥锁
    ret = ext4_write_checks(iocb, from);  // 权限/大小检查
    if (ret <= 0)
        goto out;

    ret = generic_perform_write(iocb, from);  // 核心写循环
out:
    inode_unlock(inode);
    if (unlikely(ret <= 0))
        return ret;
    return ret;
}
```

### 5.3 generic_perform_write - mm/filemap.c:4374

```c
ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *i)
{
    struct address_space *mapping = file->f_mapping;
    const struct address_space_operations *a_ops = mapping->a_ops;
    // ...
    do {
        // 1. balance_dirty_pages_ratelimited：限速脏页
        // 2. a_ops->write_begin → ext4_da_write_begin：获取/创建 folio
        // 3. copy_folio_from_iter_atomic：从用户态拷贝数据
        // 4. a_ops->write_end → ext4_da_write_end：标记脏页
        // 5. cond_resched：主动调度
    } while (iov_iter_count(i));
    // ...
}
```

核心写数据循环（改写自原文代码段）：

### 5.4 ext4_da_write_begin - fs/ext4/inode.c:1360

```c
static int ext4_da_write_begin(struct file *file, struct address_space *mapping,
                               loff_t pos, unsigned len,
                               struct folio **foliop, void **fsdata)
{
    // 1. write_begin_get_folio → folio 分配（若不存在）
    // 2. ext4_block_write_begin → 检查 buffer_head 映射状态
    // 3. 若 block 未映射 → ext4_da_get_block_prep
    //    → ext4_map_blocks(create=0) 检查 extent 树
    //    → 未分配时在 Extent Status Tree 插入 DELAYED 标记
    // 4. 返回 folio（稍后拷贝用户数据）
}
```

### 5.5 ext4_da_write_end - fs/ext4/inode.c:1514

```c
static int ext4_da_write_end(struct file *file, struct address_space *mapping,
                             loff_t pos, unsigned len, unsigned copied,
                             struct folio *folio, void *fsdata)
{
    // 1. ext4_da_do_write_end → block_write_end
    //    → mark_buffer_dirty(bh) 标记 buffer_head 脏
    // 2. __folio_mark_dirty(folio) 标记 folio 脏
    // 3. 更新 inode 大小 (i_disksize)
    // 4. ext4_da_write_credits → 预留 journal 空间
}
```

---

## 6 脏页回写路径（异步 Writeback）

### 6.1 writeback 触发时机

generic_perform_write 仅将数据写入页缓存并标记脏页，**不等待**物理 I/O 完成。脏页的物理回写在以下时机触发：

| 时机 | 触发者 | 函数路径 |
|--|--|--|
| 脏页比例超限 | 当前进程 | `balance_dirty_pages` → `wb_start_background_writeback` |
| writeback 内核线程 | kworker/uXXX:wb_writeback | `wb_workfn` → `writeback_sb_inodes` |
| 周期性回写 | kworker 定时器 | `wakeup_worker` → `wb_workfn` |
| sync/fsync 系统调用 | 调用进程 | `do_writepages` → `ext4_writepages` |

### 6.2 ext4_writepages - fs/ext4/inode.c:3089

```
write_cache_pages(mapping, wbc, __mpage_da_writepage)
  └─ __mpage_da_writepage
       └─ mpage_da_map_blocks        // 物理块分配（延迟分配转换）
            └─ ext4_map_blocks(create=1)  // → ext4_ext_map_blocks
                 └─ ext4_ext_insert_extent // extent 树插入
       └─ mpage_da_submit_io         // 提交 I/O
            └─ ext4_bio_write_folio  // fs/ext4/page-io.c:458
```

### 6.3 ext4_bio_write_folio - fs/ext4/page-io.c:458

```c
static void ext4_bio_write_folio(struct ext4_io_submit *io, struct folio *folio,
                                 size_t len)
{
    // 1. io_submit_init_bio → bio_alloc(bdev, BIO_MAX_VECS, REQ_OP_WRITE, GFP_NOIO)
    // 2. bio->bi_end_io = ext4_end_bio（写完成回调）
    // 3. io_submit_add_bh → bio_add_folio 将 folio 追加到 bio
    // 4. 若 bio 满 → ext4_io_submit 提交当前 bio，创建新 bio
}
```

### 6.4 ext4_io_submit - fs/ext4/page-io.c:398

```c
static void ext4_io_submit(struct ext4_io_submit *io)
{
    if (!io->io_bio)
        return;
    // 附加 ext4_io_end 完成处理结构
    // 设置 bio->bi_iter.bi_sector = first_block << (blocksize_bits - 9)
    blk_crypto_submit_bio(io->io_bio);  // 进入 Block 层
    io->io_bio = NULL;
}
```

---

## 7 块设备层

### 7.1 submit_bio 路径

```
blk_crypto_submit_bio(bio)         // block/blk-crypto.c 或 inline
  └─ submit_bio(bio)               // block/blk-core.c:992
       └─ submit_bio_noacct(bio)
            └─ __submit_bio(bio)
                 └─ blk_mq_submit_bio(bio)  // block/blk-mq.c:3151
```

### 7.2 blk_mq_submit_bio

```
blk_mq_submit_bio(bio)
  ├─ blk_ia_range_merge_bio(bio)       // I/O 范围合并
  ├─ bio->bi_end_io 保持不变           // ext4_end_bio
  ├─ blk_mq_get_bio_set_tag_set(bio)   // 获取 tag
  ├─ blk_mq_get_request(q, bio)        // 分配 request
  ├─ blk_mq_rq_ctx_init(rq, ...)       // 初始化 request
  ├─ blk_mq_bio_to_request(rq, bio)    // 绑定 bio → request
  ├─ blk_add_rq_to_plug(plug, rq)      // 尝试 plug 聚合
  └─ 或直接提交:
       └─ blk_mq_try_issue_directly(hctx, rq)
            └─ __blk_mq_issue_directly
                 └─ hctx->ops->queue_rq → nvme_queue_rq
```

---

## 8 NVMe 驱动层

### 8.1 写命令提交

```
nvme_queue_rq(hctx, bd)                 // drivers/nvme/host/pci.c
  └─ nvme_prep_rq(req)
       ├─ nvme_setup_cmd(ns, req, cmd)  // nvme/core.c
       │    └─ nvme_setup_rw(ns, req, cmd, nvme_cmd_write)  // 写命令
       └─ nvme_map_data(req, cmd)       // DMA 映射 (PRP/SGL)
  └─ nvme_sq_copy_cmd(nvmeq, req)       // memcpy 到 SQ 环
  └─ nvme_write_sq_db(nvmeq)            // 写门铃寄存器
       └─ writel(nvmeq->sq_tail_doorbell_addr, db_value)  // MMIO
```

### 8.2 写命令 vs 读命令

| 操作 | nvme_cmd_opcode | bio 方向 |
|--|--|--|
| 读 | `nvme_cmd_read` (0x02) | REQ_OP_READ |
| 写 | `nvme_cmd_write` (0x01) | REQ_OP_WRITE |

### 8.3 中断完成

写命令完成路径（ext4_end_bio 回调）：

```
nvme_irq(irq, nvmeq)
  └─ nvme_poll_cq(nvmeq, ...)
       ├─ nvme_handle_cqe(nvmeq, cqe)
       │    ├─ nvme_find_rq(hctx, cqe)      // 找到完成的 request
       │    └─ blk_mq_add_to_batch(req)      // 批量完成
       └─ nvme_ring_cq_doorbell(nvmeq)       // 写 CQ 门铃
  └─ nvme_pci_complete_batch(breq)
       └─ blk_mq_end_request_batch()
            └─ bio_endio(bio)
                 └─ bio->bi_end_io()
                      └─ ext4_end_bio         // fs/ext4/page-io.c
                           ├─ folio_end_writeback(folio)  // 清理脏页标志
                           └─ ext4_io_end_callback()
                                └─ ext4_finish_bio()
                                     └─ bio_for_each_folio_all
                                          → folio_end_writeback
```

---

## 9 完整路径流程图 (ASCII)

```
                    pwrite64(fd, buf, count, pos)
                           |
                    +------v------+
                    | 系统调用入口  |  SYSCALL_DEFINE4
                    |  (fs/read_   |  (fs/read_write.c:974)
                    |   write.c:   |
                    |   974)       |
                    +------+------+
                           |
                    +------v------+
                    | ksys_pwrite64|  -- pos < 0 → EINVAL
                    | (fs/read_    |  -- fd_empty → EBADF
                    |  write.c:    |  -- !FMODE_PWRITE → ESPIPE
                    |  958)        |
                    +------+------+
                           |
                    +------v------+
                    |  vfs_write   |  -- rw_verify_area
                    |  (fs/read_   |  -- file_start_write
                    |   write.c:   |
                    |   686)       |
                    +------+------+
                           |
                    +------v------+
                    | new_sync_    |  -- init_sync_kiocb
                    | write        |  -- kiocb.ki_pos = *pos
                    | (fs/read_    |  -- iov_iter_init(ITER_SOURCE)
                    |  write.c:    |
                    |  516)        |
                    +------+------+
                           |
                    +------v------+
                    | ext4_file_   |  ext4 写分发
                    | write_iter   |  (fs/ext4/file.c:844)
                    | (iocb, from) |
                    +------+------+
                           |
               +----v----+----v----+
               |                 |
        +------v------+   +------v------+
        | IOCB_DIRECT  |   | 缓冲写路径    |
        | → ext4_dio_  |   | ext4_buffered_|
        |   write_iter |   | write_iter   |
        +------+------+   +------+------+
               |                 |
                          +------v------+
                          | inode_lock   |
                          | ext4_write_  |
                          | checks       |
                          +------+------+
                                 |
                          +------v------+
                          | generic_     |
                          | perform_write|
                          | (mm/filemap. |
                          |  c:4374)     |
                          +------+------+
                                 |
                          +------v------+
                          | 还有数据?     |
                          | iov_iter_    |
                          | count(i) > 0 |
                          +------+------+
                                 |
                    +------v-----+-----v------+
                    | 是                  | 否
                    |                    |
              +------v------+           |
              | balance_dirty_|         |
              | pages_ratelimit|        |
              +------+------+           |
                     |                  |
              +------v------+           |
              | ext4_da_write_|         |
              | begin        |          |
              | (folio 分配, |          |
              |  延迟分配预留)|          |
              +------+------+           |
                     |                  |
              +------v------+           |
              | copy_folio_  |          |
              | from_iter_   |          |
              | atomic       |          |
              | (CPU 拷贝)   |          |
              +------+------+           |
                     |                  |
              +------v------+           |
              | ext4_da_     |          |
              | write_end    |          |
              | (mark_buffer |          |
              |  _dirty,     |          |
              |  __folio_mark|          |
              |  _dirty)     |          |
              +------+------+           |
                     |                  |
              +------v------+           |
              | cond_resched |          |
              +------+------+           |
                     |                  |
              +------v------+           |
              | 回到循环头    |----------+
              +------+------+
                     |
              +------v------+
              | inode_unlock |
              +------+------+
                     |
              +------v------+
              | *ppos =      |
              | kiocb.ki_pos |
              | (不影响 f_   |
              |  pos)        |
              +------+------+
                     |
              +------v------+
              | fsnotify_    |
              | modify       |
              | add_wchar    |
              | inc_syscw    |
              +------+------+
                     |
              +------v------+
              | 返回写入字节数|
              | 或错误码      |
              +------+------+

  ====== 异步 writeback 路径（条件触发）======

  generic_perform_write 返回后，writeback 内核线程唤醒
                     |
              +------v------+
              | ext4_        |
              | writepages   |
              | (fs/ext4/    |
              |  inode.c:    |
              |  3089)       |
              +------+------+
                     |
              +------v------+
              | write_cache_ |
              | pages        |
              | → __mpage_da_|
              |   writepage  |
              +------+------+
                     |
        +----v----+----v----+
        | 物理块分配    | 提交 I/O
        | (延迟分配转换) |
        |               |
  +------v------+  +------v------+
  | mpage_da_   |  | mpage_da_   |
  | map_blocks  |  | submit_io   |
  | → ext4_map_ |  | → ext4_bio_ |
  |   blocks(   |  |   write_    |
  |   create=1) |  |   folio     |
  | → ext4_ext_ |  | → ext4_io_  |
  |   map_blocks|  |   submit    |
  | → ext4_ext_ |  +------+------+
  |   insert_   |         |
  |   extent    |         |
  +------+------+         |
         |                |
         +----v----+------+
                   |
            +------v------+
            | submit_bio   |
            | → blk_mq_    |
            |   submit_bio |
            | → nvme_queue_|
            |   rq         |
            | → nvme_setup_|
            |   cmd(write) |
            | → nvme_sq_   |
            |   copy_cmd   |
            | → nvme_write_|
            |   sq_db(MMIO)|
            +------+------+
                   |
            +------v------+
            | [NVMe 中断]  |
            | nvme_irq     |
            | → blk_mq_end_|
            |   request_   |
            |   batch      |
            | → ext4_end_  |
            |   bio        |
            | → folio_end_ |
            |   writeback  |
            +------+------+
                   |
            +------v------+
            | 物理写完成    |
            +-------------+
```

---

## 10 文本函数调用栈

```
SYSCALL_DEFINE4(pwrite64, fd, buf, count, pos)          // fs/read_write.c:974 — 系统调用入口
└─ ksys_pwrite64(fd, buf, count, pos)                    // fs/read_write.c:958 — 参数验证层
   ├─ [pos < 0] → return -EINVAL                         // 位置参数合法性校验
   ├─ CLASS(fd, f)(fd)                                    // fs/file.c — 通过 fd 获取 struct fd（auto cleanup）
   ├─ [fd_empty(f)] → return -EBADF                       // 文件描述符有效性检查
   ├─ [!(fd_file(f)->f_mode & FMODE_PWRITE)] → return -ESPIPE // 可定位写检查（管道等不支持）
   │
   └─ vfs_write(fd_file(f), buf, count, &pos)             // fs/read_write.c:969 — VFS 写入口
      ├─ [!(file->f_mode & FMODE_WRITE)] → return -EBADF  // 写权限检查
      ├─ [!(file->f_mode & FMODE_CAN_WRITE)] → return -EINVAL // 写能力检查
      ├─ rw_verify_area(WRITE, file, pos, count)          // fs/read_write.c:700 — 区域验证
      │  └─ security_file_permission(file, MAY_WRITE)     // LSM 安全钩子检查
      ├─ file_start_write(file)                            // fs/file_table.c — 防止文件系统 freeze 并发
      │  └─ sb_start_write(inode->i_sb)                   // 超级块级写冻结保护
      │
      └─ __vfs_write(file, buf, count, pos)               // fs/read_write.c:695 — 实际写分发
         └─ [f_op->write 不存在] → new_sync_write(...)     // fs/read_write.c:516 — 同步写封装
            ├─ init_sync_kiocb(&kiocb, filp)               // include/linux/fs.h — 初始化同步 kiocb
            ├─ kiocb.ki_pos = *ppos                        // 将栈 pos 赋值给 kiocb（非 file->f_pos）
            ├─ iov_iter_init(&iter, ITER_SOURCE, &iov, 1, len) // lib/iov_iter.c — 初始化写迭代器
            │                                              // ITER_SOURCE 表示数据从用户空间写入文件
            │
            └─ call_write_iter(filp, &kiocb, &iter)       // → ext4_file_write_iter
               │                                           // fs/ext4/file.c:844 — ext4 写分发
               ├─ [IS_DAX(inode)] → ext4_dax_write_iter()  // DAX 直接访问路径
               ├─ [iocb->ki_flags & IOCB_ATOMIC] → ext4_atomic_write_iter() // 原子写路径
               ├─ [iocb->ki_flags & IOCB_DIRECT] → ext4_dio_write_iter()    // DirectIO 路径（绕过页缓存）
               │
               └─ ext4_buffered_write_iter(iocb, from)     // fs/ext4/file.c:348 — 缓冲写路径（默认）
                  ├─ inode_lock(inode)                      // fs/inode.c — 获取 i_rwsem 互斥锁
                  ├─ ext4_write_checks(iocb, from)          // fs/ext4/file.c — 权限/大小/限额检查
                  │
                  └─ generic_perform_write(iocb, from)      // mm/filemap.c:4374 — 页缓存写循环
                     ├─ balance_dirty_pages_ratelimited(mapping) // mm/page-writeback.c — 脏页限速
                     ├─ [iov_iter_count(i) == 0] → 退出循环
                     │
                     └─ [循环体: 每次写入一个 folio]
                        ├─ a_ops->write_begin              // → ext4_da_write_begin (fs/ext4/inode.c:1360)
                        │  ├─ write_begin_get_folio         // mm/filemap.c — 获取/分配页缓存 folio
                        │  │  └─ filemap_grab_folio        // 尝试从页缓存获取，否则分配新 folio
                        │  ├─ ext4_block_write_begin        // fs/ext4/inode.c — 检查 buffer_head 映射状态
                        │  └─ ext4_da_get_block_prep         // fs/ext4/inode.c — 延迟分配预留
                        │     └─ ext4_map_blocks(create=0)  // 检查 extent 树，若未分配则插入 DELAYED 标记
                        │
                        ├─ copy_folio_from_iter_atomic(folio, offset, from) // lib/iov_iter.c — 用户数据 CPU 拷贝
                        │
                        ├─ a_ops->write_end                // → ext4_da_write_end (fs/ext4/inode.c:1514)
                        │  ├─ ext4_da_do_write_end          // fs/ext4/inode.c
                        │  │  └─ block_write_end            // fs/buffer.c — 写入 buffer_head
                        │  │     └─ mark_buffer_dirty(bh)  // 标记 buffer_head 脏
                        │  ├─ __folio_mark_dirty(folio)     // mm/page-writeback.c — 标记 folio 脏
                        │  ├─ ext4_da_write_credits         // fs/ext4/inode.c — 预留 journal 空间
                        │  └─ 更新 inode i_disksize         // 扩展文件大小（若需要）
                        │
                        └─ cond_resched()                   // 主动让出 CPU，避免软锁

  [完成后:]
  ├─ inode_unlock(inode)                                   // 释放 i_rwsem
  ├─ [ret > 0] → *ppos = kiocb.ki_pos                      // 回写栈上 pos（不影响 file->f_pos）
  ├─ [ret > 0] → fsnotify_modify(file)                     // inotify 文件修改事件
  ├─ [ret > 0] → add_wchar(current, ret)                   // 进程写字节数统计
  └─ inc_syscw(current)                                    // 系统调用写计数

  ====== 异步 writeback 路径（条件触发，与上路径并行）======

  [writeback 内核线程唤醒]
  └─ wb_workfn(wb)                                          // mm/backing-dev.c — writeback 工作线程
     └─ writeback_sb_inodes(sb, wb, wbc)                   // fs/fs-writeback.c — 遍历脏 inode
        └─ ext4_writepages(mapping, wbc)                    // fs/ext4/inode.c:3089 — ext4 回写入口
           └─ write_cache_pages(mapping, wbc, __mpage_da_writepage) // mm/page-writeback.c — 遍历脏页
              └─ __mpage_da_writepage(mapping, wbc, folio) // fs/ext4/inode.c — 每页处理
                 ├─ mpage_da_map_blocks(mpio, ...)          // fs/ext4/inode.c — 物理块分配（延迟分配转换）
                 │  └─ ext4_map_blocks(create=1)            // fs/ext4/inode.c — 分配物理块
                 │     └─ ext4_ext_map_blocks(inode, ...)    // fs/ext4/extents.c — extent 树操作
                 │        └─ ext4_ext_insert_extent(et, ...) // fs/ext4/extents.c — 插入新 extent
                 │
                 └─ mpage_da_submit_io(mpio)                // fs/ext4/inode.c — 提交 bio
                    └─ ext4_bio_write_folio(io, folio, len)  // fs/ext4/page-io.c:458 — 创建 bio
                       ├─ io_submit_init_bio(io, ...)        // fs/ext4/page-io.c — bio_alloc 分配
                       │  └─ bio_alloc(bdev, BIO_MAX_VECS, REQ_OP_WRITE, GFP_NOIO)
                       ├─ bio->bi_end_io = ext4_end_bio     // 设置写完成回调
                       ├─ io_submit_add_bh(io, ...)          // fs/ext4/page-io.c — bio_add_folio 追加
                       └─ ext4_io_submit(io)                 // fs/ext4/page-io.c:398 — 提交 bio 到块层
                          └─ blk_crypto_submit_bio(io->io_bio) // bio 加密（可选）→ submit_bio
                             └─ submit_bio(bio)              // block/blk-core.c:992 — 块层入口
                                └─ submit_bio_noacct(bio)    // block/blk-core.c
                                   └─ __submit_bio(bio)      // block/blk-core.c
                                      └─ blk_mq_submit_bio(bio) // block/blk-mq.c:3151 — blk-mq 提交
                                         ├─ blk_mq_get_request(q, bio)   // 分配 request
                                         ├─ blk_mq_rq_ctx_init(rq, ...)  // 初始化 request
                                         ├─ blk_mq_bio_to_request(rq, bio) // 绑定 bio → request
                                         ├─ [plug 聚合] → blk_add_rq_to_plug(plug, rq)
                                         └─ [直接提交] → blk_mq_try_issue_directly(hctx, rq)
                                            └─ __blk_mq_issue_directly(rq)
                                               └─ hctx->ops->queue_rq(hctx, bd) // → nvme_queue_rq
                                                  └─ nvme_queue_rq(hctx, bd)    // drivers/nvme/host/pci.c
                                                     ├─ nvme_prep_rq(req)
                                                     │  ├─ nvme_setup_cmd(ns, req, cmd) // nvme/core.c
                                                     │  │  └─ nvme_setup_rw(ns, req, cmd, nvme_cmd_write) // 写命令 opcode=0x01
                                                     │  └─ nvme_map_data(req, cmd)       // DMA 映射（PRP 或 SGL）
                                                     ├─ nvme_sq_copy_cmd(nvmeq, req)     // memcpy 到 SQ 环
                                                     └─ nvme_write_sq_db(nvmeq)          // writel MMIO 写门铃
                                                        └─ writel(nvmeq->sq_tail_doorbell_addr, db_value) // 通知 NVMe 控制器

  [NVMe 中断完成]
  └─ nvme_irq(irq, nvmeq)                                  // drivers/nvme/host/pci.c — 硬件中断处理
     └─ nvme_poll_cq(nvmeq, ...)                            // 轮询 CQ 完成队列
        ├─ nvme_handle_cqe(nvmeq, cqe)                      // 处理完成队列元素
        │  └─ nvme_find_rq(hctx, cqe)                      // 找到对应的 request
        │     └─ blk_mq_add_to_batch(req, ...)              // 批量完成收集
        └─ nvme_ring_cq_doorbell(nvmeq)                     // 写 CQ 门铃（通知控制器已处理）
     └─ nvme_pci_complete_batch(breq)                       // 批量完成
        └─ blk_mq_end_request_batch(...)                    // block/blk-mq.c — 批量结束请求
           └─ bio_endio(bio)                                // block/bio.c — 通知 bio 完成
              └─ bio->bi_end_io(bio)                        // → ext4_end_bio (fs/ext4/page-io.c)
                 ├─ [错误处理] → ext4_io_end_callback()      // 错误时的清理回调
                 │  └─ ext4_finish_bio(bio)                 // 遍历 bio 中所有 folio
                 │     └─ bio_for_each_folio_all → folio_end_writeback(folio) // 清理脏页标志
                 └─ [正常完成] → folio_end_writebook(folio)  // 标记 folio 写回完成
                    └─ folio_end_writeback(folio)           // mm/filemap.c — 唤醒等待写完成的进程
```

> 异步 writeback 路径在 generic_perform_write 返回后由 writeback 内核线程执行，与主路径并行。

---

## 11 与 write 系统调用的完整对比

| 维度 | write | pwrite64 |
|--|--|--|
| **偏移来源** | `file->f_pos` | 栈局部变量 `pos` |
| **偏移更新** | 更新 `file->f_pos` | 不更新（栈变量） |
| **额外检查** | `FMODE_WRITE` | `FMODE_WRITE` + `FMODE_PWRITE` |
| **原子性** | 多 write 并发相互影响偏移 | 无偏移竞争问题 |
| **线程安全** | 需外部锁保护偏移 | 天然线程安全（偏移参数化） |
| **管道的支持** | 支持 | 不支持（返回 -ESPIPE） |
| **VFS 写路径** | `vfs_write(file, buf, count, &f_pos)` | `vfs_write(file, buf, count, &pos)` |
| **下游路径** | 完全相同 | 完全相同 |

---

## 12 关键数据结构 (C代码 + 注释)

```c
// ===== VFS 层 =====

// I/O 控制块——pwrite64 的核心数据结构，携带 I/O 操作的所有上下文
struct kiocb {
    struct file      *ki_filp;       // 目标文件对象（通过 fd 查找获得）
    loff_t            ki_pos;        // 写入位置（pwrite64 使用栈变量 pos，不影响 file->f_pos）
    unsigned short    ki_opcode;     // I/O 操作码（针对特定设备）
    unsigned short    ki_flags;      // I/O 标志位，如 IOCB_DIRECT（直接 I/O）、IOCB_ATOMIC（原子写）
    short             ki_ioprio;     // I/O 优先级（I/O 调度使用）
    void              *private;      // 文件系统私有数据（ext4 用于存储写入上下文）
    union {
        void          (*ki_complete)(struct kiocb *iocb, long ret);
        // 异步 I/O 完成回调（同步写为 NULL）
    };
};

// 数据迭代器——管理用户态缓冲区的读写位置和剩余字节
struct iov_iter {
    u8 iter_type;            // 迭代器类型：ITER_IOVEC（多段缓冲区）/ ITER_UBUF（单段）
    u8 data_source;          // 数据方向：ITER_SOURCE（写操作，从用户空间读取）
    size_t iov_offset;       // 当前 iovec 段内的偏移（跨段续传使用）
    size_t count;            // 剩余未传输字节总数
    union {
        const struct iovec *iov;       // 指向 iovec 数组（多段缓冲区）
        struct {
            void __user *ubuf;         // 用户缓冲区基地址（单段模式）
            size_t len;                // 缓冲区长度
        };
    };
    unsigned long nr_segs;   // iovec 段数（ITER_IOVEC 时有效）
};

// 用户空间缓冲区描述——pwrite64 使用单段 iovec（指向 buf 和 count）
struct iovec {
    void __user *iov_base;   // 用户空间缓冲区基地址
    size_t       iov_len;    // 该段缓冲区长度
};

// 页缓存 folio——内存页的抽象，pwrite64 将数据写入 folio 后标记脏
struct folio {
    unsigned long flags;     // folio 标志（PG_dirty 脏页、PG_writeback 回写中等）
    struct address_space *mapping;  // 所属的 address_space（即文件页缓存树）
    loff_t index;            // 在文件内的页索引（pos >> PAGE_SHIFT）
    void *private;           // 文件系统私有数据（ext4 的 buffer_head 链表）
    atomic_t _mapcount;      // 映射计数（页表映射数）
    atomic_t _refcount;      // 引用计数（页缓存 + 进程映射）
};

// ===== ext4 文件系统层 =====

// ext4 I/O 提交结构——管理一组 bio 的提交和完成回调
struct ext4_io_submit {
    struct bio          *io_bio;          // 当前正在构建的 bio（写请求）
    struct ext4_io_end  *io_end;          // I/O 完成处理结构（错误处理、清理）
    sector_t             io_next_block;   // 下一个要写入的块扇区号
    struct super_block   *io_sb;          // 超级块（用于错误处理）
    unsigned int         io_flags;        // 提交标志（如 REQ_FUA、REQ_SYNC）
};

// ext4 I/O 完成结构——记录写完成后的回调处理信息
struct ext4_io_end {
    struct inode        *inode;           // 所属 inode（用于写完成后的 inode 更新）
    loff_t               offset;          // 写入偏移范围起始
    size_t               size;            // 写入大小
    unsigned int         flag;            // 标志：DIO/缓冲写/fallback 等
    struct work_struct   work;            // 工作队列项（异步完成处理）
};

// ===== 块层 =====

// 块 I/O 请求——提交到块层的核心 I/O 单元
struct bio {
    struct bio          *bi_next;         // bio 链表（plug 聚合时使用）
    struct block_device *bi_bdev;         // 目标块设备
    blk_opf_t            bi_opf;          // 操作标志：REQ_OP_WRITE（写操作）
    unsigned short       bi_flags;        // bio 标志（如 BIO_PAGE_REFFED）
    unsigned short       bi_ioprio;       // I/O 优先级
    struct bio_vec       *bi_io_vec;      // 数据段数组（bio_vec 列表）
    unsigned int         bi_vcnt;         // bio_vec 段数
    struct bvec_iter     bi_iter;         // 当前迭代位置（bi_sector 为起始扇区）
    bio_end_io_t         *bi_end_io;      // 完成回调 → ext4_end_bio
    void                 *bi_private;     // 私有数据（io_end 指针）
};

// blk-mq 请求——块层多队列 I/O 请求
struct request {
    struct request_queue *q;              // 所属请求队列
    struct bio           *bio;            // 关联的 bio（链表的第一个）
    struct bio           *biotail;        // bio 链表的最后一个
    unsigned int         cmd_flags;       // 命令标志（REQ_OP_WRITE 等）
    sector_t             __sector;        // 起始扇区号
    struct gendisk       *rq_disk;        // 目标磁盘
    struct blk_mq_ctx    *mq_ctx;         // blk-mq 软件上下文
    struct blk_mq_hw_ctx *mq_hctx;        // blk-mq 硬件队列
    void                 *end_io_data;    // 完成回调数据
};

// ===== NVMe 驱动层 =====

// NVMe 命令结构——提交到 NVMe 控制器的命令
struct nvme_command {
    // 命令 dword 0
    struct {
        u8  opcode;          // 操作码：nvme_cmd_write (0x01) 写 / nvme_cmd_read (0x02) 读
        u8  flags;           // 命令标志
        u16 command_id;      // 命令 ID（用于匹配完成）
    };
    // 命令 dword 1-15
    __le32 nsid;             // 命名空间 ID（NVMe 设备内的逻辑单元）
    __le64 metadata;         // 元数据指针（PRP1 条目）
    __le64 prp1;             // PRP1 物理区域指针（DMA 地址）
    __le64 prp2;             // PRP2（若数据跨页，指向 PRP 列表）
    __le32 cdw10;            // 起始 LBA（逻辑块地址）
    __le16 cdw11;            // 块数 (length - 1)
    // ... 后续 dword 用于特定功能
};

// NVMe 队列——SQ/CQ 对，管理命令提交和完成
struct nvme_queue {
    struct nvme_dev     *dev;             // NVMe 设备
    struct nvme_command *sq_cmds;         // 提交队列 (SQ) 环缓冲区
    volatile struct nvme_completion *cqes; // 完成队列 (CQ) 环缓冲区
    dma_addr_t           sq_dma_addr;     // SQ DMA 地址（硬件可访问）
    dma_addr_t           cq_dma_addr;     // CQ DMA 地址
    u32 __iomem          *sq_tail_doorbell_addr;  // SQ 门铃寄存器地址（MMIO 写）
    u32 __iomem          *cq_head_doorbell_addr;  // CQ 门铃寄存器地址（MMIO 写）
    unsigned int         sq_tail;         // SQ 环尾指针（下次写入位置）
    unsigned int         cq_head;         // CQ 环头指针（下次读取位置）
    unsigned int         cq_phase;        // CQ 阶段位（用于区分新旧完成项）
};
```

| 数据结构 | 头文件 | 在 pwrite64 中的作用 |
|----------|--------|---------------------|
| `struct kiocb` | `include/linux/fs.h` | 携带写入位置 pos 和标志，传递到 ext4 层 |
| `struct iov_iter` | `include/linux/uio.h` | 管理用户缓冲区迭代，方向为 ITER_SOURCE |
| `struct iovec` | `include/uapi/linux/uio.h` | 单段用户缓冲区描述（buf + count） |
| `struct folio` | `include/linux/mm_types.h` | 页缓存单元，写入后标记 PG_dirty |
| `struct ext4_io_submit` | `fs/ext4/ext4.h` | 管理 ext4 写 bio 的构建和提交 |
| `struct ext4_io_end` | `fs/ext4/ext4.h` | 写完成后的回调处理 |
| `struct bio` | `include/linux/blk_types.h` | 块层 I/O 请求单元，REQ_OP_WRITE |
| `struct request` | `include/linux/blk-mq.h` | blk-mq 层请求，绑定 bio |
| `struct nvme_command` | `drivers/nvme/host/nvme.h` | NVMe 写命令 (opcode=0x01) |
| `struct nvme_queue` | `drivers/nvme/host/nvme.h` | SQ/CQ 队列管理，MMIO 门铃操作 |

---

## 13 总结

pwrite64 系统调用完整路径总结：

```
用户态 pwrite64(fd, buf, count, pos)
  │
  ├─(1) 参数获取与验证
  │   └─ ksys_pwrite64 → pos < 0 检查 → FMODE_PWRITE 检查
  │
  ├─(2) VFS 写分发
  │   └─ vfs_write → rw_verify_area → file_start_write
  │       → new_sync_write → init_sync_kiocb → iov_iter_init(ITER_SOURCE)
  │       → call_write_iter → ext4_file_write_iter
  │
  ├─(3) ext4 写路径选择
  │   └─ DIO? → ext4_dio_write_iter
  │   └─ Buffered → ext4_buffered_write_iter
  │
  ├─(4) 页缓存写循环 (generic_perform_write)
  │   └─ write_begin → copy_from_iter → write_end 循环
  │   └─ 数据写入 folio + 标记脏页 + 延迟分配
  │
  └─[异步 Writeback 路径]
       └─ ext4_writepages → mpage_da_map_blocks (物理块分配)
            → ext4_bio_write_folio → ext4_io_submit
            → blk_crypto_submit_bio → blk_mq_submit_bio
            → nvme_queue_rq → nvme_setup_cmd(nvme_cmd_write)
            → nvme_sq_copy_cmd → nvme_write_sq_db (writel MMIO)
            → [NVMe 完成中断]
            → nvme_irq → nvme_poll_cq → nvme_handle_cqe
            → blk_mq_end_request_batch → ext4_end_bio
            → folio_end_writeback / ext4_finish_bio
```

pwrite64 与 write 的唯一语义差异在**偏移量的来源**：pwrite64 使用栈局部变量 `pos`（不更新 `f_pos`），消除了多线程竞争文件偏移的问题。从 VFS 层开始，所有下游路径完全一致。
