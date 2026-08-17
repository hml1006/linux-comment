# preadv 系统调用完整路径分析

## 1 概述

preadv 系统调用将 **定位读（positioned I/O）** 和 **分散/聚集 I/O（scatter-gather I/O）** 结合为一体。它在指定文件偏移量处，使用多个 `iovec` 缓冲区执行读操作，且**不改变**文件当前的 `f_pos`。

### 关键特点

- **定位语义**：使用调用者提供的 `pos` 参数（栈局部变量），不更新 `file->f_pos`
- **分散/聚集**：通过 `import_iovec` 从用户空间导入多个 `iovec` 段，支持 `UIO_FASTIOV` 栈优化
- **权限检查**：preadv 需要 `FMODE_PREAD`
- **ARM64 参数编码**：`loff_t pos` 由 `pos_h`(高32位) 和 `pos_l`(低32位) 拼装而成
- **preadv2 扩展**：支持 `RWF_*` 标志（如 `RWF_NOWAIT`, `RWF_DSYNC`）
- **下游路径**：preadv 与 read 共享完全相同的 ext4→block→NVMe 路径

---

## 2 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | preadv/preadv2 系统调用入口 (fs/read_write.c) |
| **VFS** | vfs_readv → do_iter_readv_writev (fs/read_write.c) |
| **ext4** | ext4_file_read_iter → generic_file_read_iter (fs/ext4/file.c) |
| **Page Cache** | filemap_read → filemap_get_pages (mm/filemap.c) |
| **ext4 读 BIO** | ext4_mpage_readpages (fs/ext4/readpage.c) |
| **Block Layer** | blk-mq 提交 (block/blk-core.c, blk-mq.c) |
| **NVMe 驱动** | 读命令提交 + 中断完成 (drivers/nvme/host/pci.c) |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE5(preadv) - fs/read_write.c:1455

```c
SYSCALL_DEFINE5(preadv, unsigned long, fd, const struct iovec __user *, vec,
        unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
    loff_t pos = pos_from_hilo(pos_h, pos_l);
    return do_preadv(fd, vec, vlen, pos, 0);
}

SYSCALL_DEFINE6(preadv2, unsigned long, fd, const struct iovec __user *, vec,
        unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h,
        rwf_t, flags)
{
    loff_t pos = pos_from_hilo(pos_h, pos_l);
    if (pos == -1)
        return do_readv(fd, vec, vlen, flags);   // pos=-1 → 用 f_pos

    return do_preadv(fd, vec, vlen, pos, flags);
}
```

### 3.2 ARM64 特殊的参数编码

ARM64 系统调用号码使用 `x8` 寄存器传递，参数使用 `x0-x5` 寄存器。由于 ARM64 寄存器宽度为 64 位，一个 `loff_t`（64位）可以用一个寄存器，而 `preadv` 的签名在用户态是：

```c
ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
```

但在内核中 ARM64 的 `SYSCALL_DEFINE5` 无法直接传递 64 位参数，因此拆分为 `pos_l`（低32位）和 `pos_h`（高32位）：

```c
// arch/arm64/include/asm/syscall_wrapper.h
#define pos_from_hilo(h, l) (((loff_t)(h) << 32) | (loff_t)(l))
```

在 syscall_64.tbl 中定义为：
```
69  common  preadv      sys_preadv
```

### 3.3 do_preadv - fs/read_write.c:1401

```c
static ssize_t do_preadv(unsigned long fd, const struct iovec __user *vec,
             unsigned long vlen, loff_t pos, rwf_t flags)
{
    ssize_t ret = -EBADF;

    if (pos < 0)
        return -EINVAL;

    CLASS(fd, f)(fd);
    if (!fd_empty(f)) {
        ret = -ESPIPE;
        if (fd_file(f)->f_mode & FMODE_PREAD)
            ret = vfs_readv(fd_file(f), vec, vlen, &pos, flags);
    }

    if (ret > 0)
        add_rchar(current, ret);
    inc_syscr(current);
    return ret;
}
```

---

## 4 VFS 分散/聚集 I/O 层

### 4.1 import_iovec - iov_iter 导入机制

preadv 与 readv 共享同一个关键机制：从用户空间导入多个 iovec 段。

```c
// lib/iov_iter.c
ssize_t import_iovec(int type, const struct iovec __user *uvec,
             unsigned nr_segs, unsigned fast_segs,
             struct iovec **iovp, struct iov_iter *iter)
{
    struct iovec *iov = *iovp;
    ssize_t ret;

    ret = __import_iovec(type, uvec, nr_segs, fast_segs, iovp, iter);
    // 1. 如果 nr_segs <= UIO_FASTIOV（通常为8），使用栈上数组 iovstack
    //    避免 kmalloc 分配
    // 2. 拷贝用户空间 iovec 数组到内核
    // 3. 初始化 iov_iter 结构体
    return ret;
}
```

### 4.2 vfs_readv - fs/read_write.c:1202

```c
static ssize_t vfs_readv(struct file *file, const struct iovec __user *vec,
             unsigned long vlen, loff_t *pos, rwf_t flags)
{
    struct iovec iovstack[UIO_FASTIOV];
    struct iovec *iov = iovstack;
    struct iov_iter iter;
    size_t tot_len;
    ssize_t ret = 0;

    if (!(file->f_mode & FMODE_READ))
        return -EBADF;
    if (!(file->f_mode & FMODE_CAN_READ))
        return -EINVAL;

    ret = import_iovec(ITER_DEST, vec, vlen, ARRAY_SIZE(iovstack), &iov, &iter);
    if (ret < 0)
        return ret;

    tot_len = iov_iter_count(&iter);
    if (!tot_len)
        goto out;

    ret = rw_verify_area(READ, file, pos, tot_len);
    if (ret < 0)
        goto out;

    if (file->f_op->read_iter)
        ret = do_iter_readv_writev(file, &iter, pos, READ, flags);
    else
        ret = do_loop_readv_writev(file, &iter, pos, READ, flags);
out:
    if (ret >= 0)
        fsnotify_access(file);
    kfree(iov);
    return ret;
}
```

### 4.3 do_iter_readv_writev - 核心分发函数

```c
static ssize_t do_iter_readv_writev(struct file *filp, struct iov_iter *iter,
                    loff_t *ppos, int type, rwf_t flags)
{
    struct kiocb kiocb;
    ssize_t ret;

    init_sync_kiocb(&kiocb, filp);
    ret = kiocb_set_rw_flags(&kiocb, flags, type);
    if (ret)
        return ret;
    kiocb.ki_pos = (ppos ? *ppos : 0);

    // preadv: type=READ → f_op->read_iter → ext4_file_read_iter
    if (type == READ)
        ret = filp->f_op->read_iter(&kiocb, iter);

    BUG_ON(ret == -EIOCBQUEUED);
    if (ppos)
        *ppos = kiocb.ki_pos;
    return ret;
}
```

关键点：
- preadv 使 `type=READ` → `f_op->read_iter` → `ext4_file_read_iter`
- 从 `do_iter_readv_writev` 开始，preadv 与 read 共享完全相同的下游路径
- 区别仅在于：`vfs_readv` 使用 `import_iovec(ITER_DEST, ...)` 导入多段 iovec，`vfs_read` 使用 `iov_iter_init` 初始化单段

---

## 5 下游路径（preadv = read）

```
do_iter_readv_writev(filp, iter, &pos, READ, flags)
  └─ filp->f_op->read_iter(&kiocb, iter)
       └─ ext4_file_read_iter(iocb, iter)        // fs/ext4/file.c:186
            ├─ IS_DAX → ext4_dax_read_iter
            ├─ IOCB_DIRECT → ext4_dio_read_iter
            └─ generic_file_read_iter(iocb, iter)  // mm/filemap.c:3014
                 └─ filemap_read(iocb, iter, ...)   // mm/filemap.c:2620
                      └─ filemap_get_pages(iocb, iter, ...) // mm/filemap.c
                           ├─ filemap_get_folio → 查找页缓存命中 → 直接返回数据
                           └─ page_cache_sync_readahead → 预读未命中
                                └─ mapping->a_ops->read_folio
                                     └─ ext4_read_folio          // fs/ext4/readpage.c:395
                                          └─ ext4_mpage_readpages // fs/ext4/readpage.c:211
                                               ├─ ext4_map_blocks → 块映射
                                               ├─ bio_alloc(bdev, BIO_MAX_VECS, REQ_OP_READ, GFP_NOIO)
                                               ├─ bio_add_folio → 构建 BIO
                                               ├─ bio->bi_end_io = mpage_end_io
                                               └─ submit_bio → blk_mq_submit_bio
                      └─ copy_page_to_iter(folio, offset, bytes, iter)
                           └─ 将数据拷贝到 iovec 各段（多段 vs read 单段）
```

> preadv 与 read 的差异仅在于 `vfs_read` vs `vfs_readv`：
> - `vfs_read`：`iov_iter_init(&iter, ITER_DEST, &iov, 1, count)` — 单段
> - `vfs_readv`：`import_iovec(ITER_DEST, vec, vlen, ...)` — 多段
> - `do_iter_readv_writev` 之后的路径完全一致

---

## 6 UIO_FASTIOV 优化

preadv 使用 `import_iovec` 导入用户空间 iovec 数组，当 iovec 数量较少（≤ 8）时使用栈上数组避免 kmalloc：

```c
struct iovec iovstack[UIO_FASTIOV];   // UIO_FASTIOV 通常 = 8
struct iovec *iov = iovstack;
// ...
ret = import_iovec(ITER_DEST, vec, vlen, ARRAY_SIZE(iovstack), &iov, &iter);
// ...
kfree(iov);   // 如果 iov != iovstack，释放 kmalloc 的内存
```

| iovec 数量 | 分配方式 | 性能特征 |
|--|--|--|
| ≤ 8 | 栈上 `iovstack[8]` | 零分配，最快 |
| > 8 | `kmalloc` 动态分配 | 有分配开销 |

---

## 7 preadv2 RWF 标志

preadv2 通过 `flags` 参数支持额外的 `RWF_*` 标志：

| 标志 | 值 | 说明 |
|--|--|--|
| `RWF_DSYNC` | 0x01 | 类似 O_DSYNC，读完成前等待数据完整性 |
| `RWF_HIPRI` | 0x02 | 高优先级，polling 模式（需块设备支持） |
| `RWF_SYNC` | 0x04 | 类似 O_SYNC，读完成前等待数据+元数据完整性 |
| `RWF_NOWAIT` | 0x08 | 非阻塞，若 I/O 可能阻塞立即返回 -EAGAIN |

`kiocb_set_rw_flags` 负责解析这些标志并设置 `kiocb.ki_flags`：

```c
static inline int kiocb_set_rw_flags(struct kiocb *ki, rwf_t flags, int type)
{
    if (flags & ~RWF_SUPPORTED)
        return -EOPNOTSUPP;

    if (flags & RWF_NOWAIT)
        ki->ki_flags |= IOCB_NOWAIT;
    if (flags & RWF_HIPRI)
        ki->ki_flags |= IOCB_HIPRI;
    if (flags & RWF_DSYNC)
        ki->ki_flags |= IOCB_DSYNC;
    if (flags & RWF_SYNC)
        ki->ki_flags |= IOCB_SYNC;
    return 0;
}
```

---

## 8 函数调用栈

```
/* ========== preadv 主路径 ========== */
/* 结合定位语义 + 分散/聚集 I/O */

SYSCALL_DEFINE5(preadv, fd, vec, vlen, pos_l, pos_h)  // fs/read_write.c:1455 — 系统调用入口
└─ pos_from_hilo(pos_h, pos_l)                         // arch/arm64/include/asm — 拼接 64 位 pos
└─ do_preadv(fd, vec, vlen, pos, 0)                    // fs/read_write.c:1401 — 参数验证层
   ├─ [pos < 0] → return -EINVAL                       // 位置参数合法性校验
   ├─ CLASS(fd, f)(fd)                                  // fs/file.c — 通过 fd 获取 struct fd
   ├─ [fd_empty(f)] → return -EBADF                     // 文件描述符有效性检查
   ├─ [!(fd_file(f)->f_mode & FMODE_PREAD)] → return -ESPIPE // 定位读权限检查
   │
   └─ vfs_readv(fd_file(f), vec, vlen, &pos, flags)    // fs/read_write.c:1202 — VFS 读入口
      │                                                  // &pos 指向栈上变量，不更新 f_pos
      ├─ [!(file->f_mode & FMODE_READ)] → return -EBADF  // 读权限检查
      ├─ [!(file->f_mode & FMODE_CAN_READ)] → return -EINVAL // 读能力检查
      │
      ├─ import_iovec(ITER_DEST, vec, vlen, ...)        // lib/iov_iter.c — 导入多段 iovec
      │  ├─ [nr_segs ≤ UIO_FASTIOV(8)]                 // 栈上 iovstack[8] 零分配
      │  │  └─ iov = iovstack                           // 无 kmalloc 开销
      │  ├─ [nr_segs > 8]                               // 动态分配
      │  │  └─ iov = kmalloc_array(nr_segs, ...)        // 需要后续 kfree
      │  └─ __import_iovec() → 拷贝用户空间 iovec → 初始化 iov_iter
      │     └─ iter.iter_type = ITER_IOVEC              // 类型：多段 iovec
      │        iter.data_source = ITER_DEST             // 方向：从内核到用户
      │        iter.nr_segs = vlen                       // 段数
      │        iter.iov = iov                            // 指向 iovec 数组
      │
      ├─ rw_verify_area(READ, file, pos, tot_len)       // fs/read_write.c — 区域验证
      │  └─ security_file_permission(file, MAY_READ)    // LSM 安全钩子
      │
      └─ do_iter_readv_writev(file, &iter, pos, READ, flags) // fs/read_write.c:1003 — 核心分发
         ├─ init_sync_kiocb(&kiocb, filp)               // include/linux/fs.h — 初始化 kiocb
         ├─ kiocb_set_rw_flags(&kiocb, flags, READ)     // 解析 RWF_* → IOCB_* 标志
         ├─ kiocb.ki_pos = *ppos                         // 赋值栈 pos（非 file->f_pos）
         │
         └─ filp->f_op->read_iter(&kiocb, &iter)        // → ext4_file_read_iter
            │                                             // fs/ext4/file.c:186 — ext4 读分发
            ├─ [IS_DAX(inode)] → ext4_dax_read_iter()    // DAX 直接访问路径
            ├─ [iocb->ki_flags & IOCB_DIRECT] → ext4_dio_read_iter() // DirectIO 路径
            │
            └─ generic_file_read_iter(iocb, iter)        // mm/filemap.c:3014 — 页缓存读路径
               └─ filemap_read(iocb, iter, retval)       // mm/filemap.c:2620 — 页缓存读核心
                  │
                  │ [循环: 每轮读取一组 folio]
                  │
                  ├─ filemap_get_pages(iocb, iter, ...)   // mm/filemap.c:2400 — 获取页
                  │  └─ filemap_get_read_batch(mapping, ...)  // XArray 批量查找
                  │     ├─ xa_load(&mapping->i_pages, index)  // 基数树查找
                  │     │  ├─ [页缓存命中] → folio 引用 + 添加
                  │     │  └─ [页缓存未命中] → 进入预读路径
                  │     │
                  │     └─ [页缓存未命中] → page_cache_sync_readahead(...)
                  │        └─ ra_alloc_folio(...)             // 分配 folio 并加入页缓存
                  │
                  ├─ [页缓存命中]
                  │  └─ copy_page_to_iter(folio, offset, bytes, iter)
                  │     └─ 数据拷贝到 iovec 各段（多段分散拷贝）
                  │
                  └─ [页缓存未命中]
                     └─ filemap_create_folio(mapping, index)  // 创建新 folio
                        └─ filemap_read_folio(file, ...)      // 触发磁盘 I/O
                           └─ mapping->a_ops->read_folio(file, folio)  // → ext4_read_folio
                              │
                              └─ ext4_read_folio(file, folio)  // fs/ext4/readpage.c:395
                                 └─ ext4_mpage_readpages(mapping, ...)  // fs/ext4/readpage.c:211
                                    ├─ ext4_map_blocks(inode, &map, ...)  // fs/ext4/inode.c:600
                                    │  → 逻辑块号 → 物理块号映射
                                    │
                                    ├─ bio_alloc(bdev, nr_vecs, REQ_OP_READ, ...)  // 分配 bio
                                    ├─ bio_add_folio(bio, folio, ...)              // folio 加入 bio
                                    ├─ bio->bi_end_io = mpage_end_io               // 设置完成回调
                                    │
                                    └─ submit_bio(bio)                             // 提交到块层
                                       └─ submit_bio_noacct(bio)                    // block/blk-core.c
                                          └─ __submit_bio(bio)                      // 块层入口
                                             └─ blk_mq_submit_bio(bio)             // block/blk-mq.c:2200
                                                ├─ bio_split_to_limits(bio, ...)   // 拆分超限 bio
                                                ├─ blk_mq_attempt_bio_merge(...)   // 尝试合并
                                                ├─ blk_mq_get_new_requests(...)     // 分配 request
                                                ├─ blk_mq_bio_to_request(...)       // bio→request 绑定
                                                ├─ blk_add_rq_to_plug(rq)           // plug 批处理
                                                └─ blk_finish_plug(...)             // 刷新 plug 列表
                                                   └─ blk_mq_dispatch_plug_list(...)
                                                      └─ q->mq_ops->queue_rq(...)  // → nvme_queue_rq
                                                         │
                                                         └─ nvme_queue_rq(hctx, bd, ...)  // drivers/nvme/host/pci.c
                                                            ├─ nvme_prep_rq(dev, req)        // 准备命令
                                                            │  ├─ nvme_setup_cmd(req, cmd)    // 构造 NVMe 命令
                                                            │  │  └─ cmd->opcode = nvme_cmd_read (0x02)
                                                            │  │     cmd->nsid = nsid
                                                            │  │     cmd->slba = 起始 LBA
                                                            │  │     cmd->length = 块数 - 1
                                                            │  │
                                                            │  └─ nvme_map_data(dev, req, ...)  // DMA 地址映射
                                                            │     └─ dma_map_sg(dev, ...)       // PRP/SGL 表
                                                            │
                                                            ├─ nvme_sq_copy_cmd(nvmeq, cmd)   // memcpy 到 SQ 环
                                                            │
                                                            └─ nvme_write_sq_db(nvmeq)        // writel MMIO 门铃
                                                               └─ writel(tail, doorbell_addr)   // 通知硬件取命令

  [等待 I/O 完成]
  ├─ folio_wait_bit(folio_bit)          // 等待 folio 解锁
  │
  ├─ [NVMe 中断处理]
  │  └─ nvme_irq(irq, dev)              // drivers/nvme/host/pci.c
  │     └─ nvme_poll_cq(nvmeq)          // 轮询完成队列
  │        ├─ nvme_cqe_pending(nvmeq)   // 检查阶段位 (phase bit)
  │        │  └─ dma_rmb()              // 读内存屏障（DMA 一致性）
  │        │
  │        ├─ nvme_handle_cqe(nvmeq, cqe)  // 处理完成条目
  │        │  ├─ nvme_find_rq(nvmeq, cqe)  // command_id → request
  │        │  └─ nvme_try_complete_req(...) // 完成请求
  │        │     └─ blk_mq_complete_request_remote(req)  // 上报块层
  │        │
  │        └─ nvme_ring_cq_doorbell(nvmeq)  // 释放 CQ 槽位
  │
  ├─ [完成回调链]
  │  └─ blk_update_request(req, ...)         // block/blk-mq.c
  │     └─ bio->bi_end_io(bio)               // → mpage_end_io
  │        └─ __read_end_io(folio, error)    // mm/filemap.c
  │           └─ folio_end_read(folio, true) // 标记 uptodate + 解锁
  │              └─ folio_unlock(folio)      // 唤醒等待进程
  │
  └─ [I/O 完成，folio 数据就绪]
     └─ copy_page_to_iter(folio, offset, bytes, iter)  // 页缓存→用户空间拷贝（多段 iovec）
        └─ copy_page_to_iter_nofault()                  // 内核拷贝到用户 buf 各段

  [完成后:]
  ├─ kfree(iov)                                         // 释放 iovec（若动态分配）
  ├─ [ret > 0] → add_rchar(current, ret)                // 读字节统计
  └─ inc_syscr(current)                                  // 读系统调用计数
```

---

## 9 流程图

```
                    preadv(fd, iov, iovcnt, offset)
                           |
                    +------v------+
                    | SYSCALL_    |  系统调用入口
                    | DEFINE5     |  (fs/read_write.c:1455)
                    | (preadv)    |
                    +------+------+
                           |
                    +------v------+
                    | pos_from_   |  -- 拼接 64 位 pos
                    | hilo        |
                    +------+------+
                           |
                    +------v------+
                    | do_preadv   |  -- [pos < 0] → -EINVAL
                    | (fs/read_   |  -- CLASS(fd, f) 获取 fd
                    |  write.c)   |  -- [FMODE_PREAD] 检查
                    |  1401       |  -- vfs_readv 进入 VFS
                    +------+------+
                           |
                    +------v------+
                    | vfs_readv   |  -- [FMODE_READ] 检查
                    | (fs/read_   |  -- [FMODE_CAN_READ] 检查
                    |  write.c)   |  -- import_iovec(ITER_DEST)
                    |  1202       |  -- rw_verify_area
                    +------+------+
                           |
                    +------v------+
                    | import_iovec|  -- 导入用户空间多段 iovec
                    | (lib/iov_   |  -- [nr_segs ≤ 8] 栈上 iovstack
                    |  iter.c)    |  -- [nr_segs > 8] kmalloc
                    +------+------+
                           |
                    +------v------+
                    | do_iter_    |  -- init_sync_kiocb
                    | readv_writev|  -- kiocb.ki_pos = pos
                    | (fs/read_   |  -- f_op->read_iter
                    |  write.c)   |
                    +------+------+
                           |
                    +------v------+
                    | ext4_file_  |  ext4 读分发
                    | read_iter   |  (fs/ext4/file.c:186)
                    | (iocb,iter) |
                    +------+------+
                           |
              +-----v-----+-----v-----+
              |                   |
       +------v------+   +------v------+
       | IOCB_DIRECT  |   | 页缓存读路径  |
       | → ext4_dio_  |   | generic_file|
       |   read_iter  |   | _read_iter  |
       +------+------+   +------+------+
              |                  |
                         +------v------+
                         | filemap_read |
                         | (mm/filemap. |
                         |  c:2620)     |
                         +------+------+
                                |
                         +------v------+
                         | filemap_get_ |
                         | pages        |
                         | (mm/filemap. |
                         |  c)          |
                         +------+------+
                                |
                   +-----v-----+-----v-----+
                   | 页缓存命中        | 页缓存未命中
                   |                   |
             +------v------+   +------v------+
             | filemap_get_ |   | page_cache_ |
             | folio        |   | sync_reada- |
             | (直接返回)   |   | head        |
             +------+------+   +------+------+
                    |                  |
             +------v------+   +------v------+
             | copy_page_  |   | ext4_read_  |
             | to_iter     |   | folio       |
             | (数据拷贝到  |   | (fs/ext4/   |
             |  用户 iovec)|   |  readpage.c |
             | 各段)       |   |  395)       |
             +------+------+   +------+------+
                    |                  |
                         +------v------+
                         | ext4_mpage_ |
                         | readpages   |
                         | (fs/ext4/   |
                         |  readpage.c |
                         |  211)       |
                         +------+------+
                                |
                    +------v-----+------+
                    | ext4_map_blocks  |
                    | → 逻辑块→物理扇区 |
                    +------+------+------+
                           |
                    +------v------+
                    | bio_alloc    |
                    | (REQ_OP_READ)|
                    | bio_add_folio|
                    | bio->bi_end_ |
                    | io = mpage_  |
                    | end_io       |
                    | submit_bio   |
                    +------+------+
                           |
                    +------v------+
                    | blk_mq_      |
                    | submit_bio   |
                    | → bio_split  |
                    | → bio_merge  |
                    | → alloc_     |
                    |   request    |
                    | → bio_to_    |
                    |   request    |
                    +------+------+
                           |
                    +------v------+
                    | nvme_queue_  |
                    | rq           |
                    | (pci.c)      |
                    +------+------+
                           |
                    +------v------+
                    | nvme_setup_  |
                    | cmd(nvme_cmd |
                    | _read)       |
                    | nvme_map_    |
                    | data(PRP)    |
                    +------+------+
                           |
                    +------v------+
                    | nvme_sq_     |
                    | copy_cmd     |
                    | (memcpy 到   |
                    |  SQ 环)      |
                    +------+------+
                           |
                    +------v------+
                    | nvme_write_  |
                    | sq_db        |
                    | (writel MMIO)|
                    +------+------+
                           |
                    +------v------+
                    | [NVMe 控制器]|
                    | DMA 读数据   |
                    | → 写 CQE     |
                    | → 触发 MSI-X |
                    +------+------+
                           |
                    +------v------+
                    | nvme_irq    |
                    | → nvme_     |
                    |   poll_cq   |
                    | → nvme_     |
                    |   handle_cqe|
                    | → blk_mq_   |
                    |   end_      |
                    |   request   |
                    +------+------+
                           |
                    +------v------+
                    | bio_endio   |
                    | → mpage_end_|
                    |   io        |
                    | → folio_end_|
                    |   read      |
                    | → folio_    |
                    |   unlock    |
                    +------+------+
                           |
                    +------v------+
                    | copy_page_  |
                    | to_iter     |
                    | (页缓存→    |
                    |  用户 iovec |
                    |  各段)      |
                    | kfree(iov)  |
                    +------+------+
                           |
                    +------v------+
                    | 返回读取字节数|
                    | 或错误码      |
                    +-------------+
```

---

## 10 与相近系统调用的对比

| 维度 | readv | preadv | read | pread64 |
|--|--|--|--|--|
| **偏移来源** | `file->f_pos` | 栈变量 `pos` | `file->f_pos` | 栈变量 `pos` |
| **偏移更新** | 是 | 否 | 是 | 否 |
| **分散/聚集** | 是（多 iovec） | 是（多 iovec） | 否（单 buf） | 否（单 buf） |
| **FMODE_PREAD** | 不需要 | 需要 | 不需要 | 需要 |
| **iov 导入** | `import_iovec` | `import_iovec` | `iov_iter_init` | `iov_iter_init` |
| **下游路径** | ext4→...→NVMe | ext4→...→NVMe | ext4→...→NVMe | ext4→...→NVMe |

---

## 11 关键数据结构 (C代码 + 注释)

```c
// ===== VFS 层 =====

// 用户空间 I/O 向量——preadv 通过 iovec 数组传递多个读缓冲区
struct iovec {
    void __user *iov_base;   // 用户空间缓冲区基地址（preadv 读入数据的目标）
    size_t       iov_len;    // 该段缓冲区长度
};

// 多段缓冲区迭代器——preadv 使用 ITER_DEST（数据从文件读入用户空间）
struct iov_iter {
    u8 iter_type;            // 迭代器类型：ITER_IOVEC（多段 iovec，preadv 使用）/ ITER_UBUF（单段）
    u8 data_source;          // 数据方向：ITER_DEST（preadv: 从文件读入 iovec 各段）
    size_t iov_offset;       // 当前 iovec 段内的偏移（跨段续传时使用）
    size_t count;            // 剩余未传输字节总数
    union {
        const struct iovec *iov;       // 指向 iovec 数组（import_iovec 导入）
        struct {
            void __user *ubuf;         // 用户缓冲区基地址（单段模式）
            size_t len;                // 缓冲区长度
        };
    };
    unsigned long nr_segs;   // iovec 段数（preadv 核心参数，vlen 传入）
};

// I/O 控制块——携带 preadv 读操作的所有上下文
struct kiocb {
    struct file      *ki_filp;       // 目标文件对象（通过 fd 查找获得）
    loff_t            ki_pos;        // 读取位置（preadv 使用栈变量 pos，不影响 file->f_pos）
    unsigned short    ki_opcode;     // I/O 操作码
    unsigned short    ki_flags;      // I/O 标志：IOCB_DIRECT（直接 I/O）、IOCB_NOWAIT（非阻塞）等
    short             ki_ioprio;     // I/O 优先级
    void              *private;      // 文件系统私有数据
    union {
        void          (*ki_complete)(struct kiocb *iocb, long ret);
        // 异步 I/O 完成回调（同步操作时为 NULL）
    };
};

// 页缓存 folio——preadv 读取数据的目标
struct folio {
    unsigned long flags;     // folio 标志：PG_uptodate（数据有效）、PG_locked（锁定）等
    struct address_space *mapping;  // 所属的 address_space（文件页缓存树）
    loff_t index;            // 在文件内的页索引（pos >> PAGE_SHIFT）
    void *private;           // 文件系统私有数据（ext4 的 buffer_head 链表）
    atomic_t _mapcount;      // 映射计数
    atomic_t _refcount;      // 引用计数（页缓存引用 + 进程映射）
};

// ===== 块层 =====

// 块 I/O 请求——preadv 的磁盘读请求载体
struct bio {
    struct bio          *bi_next;         // bio 链表（plug 聚合时使用）
    struct block_device *bi_bdev;         // 目标块设备
    blk_opf_t            bi_opf;          // 操作标志：REQ_OP_READ（preadv 读操作）
    unsigned short       bi_flags;        // bio 标志
    unsigned short       bi_ioprio;       // I/O 优先级
    struct bio_vec       *bi_io_vec;      // 数据段数组
    unsigned int         bi_vcnt;         // bio_vec 段数
    struct bvec_iter     bi_iter;         // 当前迭代位置（bi_sector 为起始扇区）
    bio_end_io_t         *bi_end_io;      // 完成回调：preadv → mpage_end_io
    void                 *bi_private;     // 私有数据
};

// ===== NVMe 驱动层 =====

// NVMe 命令结构——提交到 NVMe 控制器的读命令
struct nvme_command {
    struct {
        u8  opcode;          // 操作码：nvme_cmd_read = 0x02（preadv 读命令）
        u8  flags;           // 命令标志
        u16 command_id;      // 命令 ID（用于匹配完成）
    };
    __le32 nsid;             // 命名空间 ID
    __le64 prp1;             // PRP1 物理区域指针（DMA 目标地址）
    __le64 prp2;             // PRP2（若数据跨页）
    __le32 cdw10;            // 起始 LBA（逻辑块地址）
    __le16 cdw11;            // 块数 (length - 1)
};

// NVMe 队列——SQ/CQ 对，管理命令提交和完成
struct nvme_queue {
    struct nvme_dev     *dev;             // NVMe 设备
    struct nvme_command *sq_cmds;         // 提交队列 (SQ) 环缓冲区
    volatile struct nvme_completion *cqes; // 完成队列 (CQ) 环缓冲区
    dma_addr_t           sq_dma_addr;     // SQ DMA 地址
    dma_addr_t           cq_dma_addr;     // CQ DMA 地址
    u32 __iomem          *sq_tail_doorbell_addr;  // SQ 门铃寄存器（MMIO 写）
    u32 __iomem          *cq_head_doorbell_addr;  // CQ 门铃寄存器（MMIO 写）
    unsigned int         sq_tail;         // SQ 环尾指针
    unsigned int         cq_head;         // CQ 环头指针
    unsigned int         cq_phase;        // CQ 阶段位（区分新旧完成项）
};
```

| 数据结构 | 头文件 | 在 preadv 中的作用 |
|----------|--------|------------------|
| `struct iovec` | `include/uapi/linux/uio.h` | 用户空间多段缓冲区描述（分散读的基础） |
| `struct iov_iter` | `include/linux/uio.h` | 管理多段 iovec 迭代，data_source=ITER_DEST |
| `struct kiocb` | `include/linux/fs.h` | 携带 I/O 位置 pos 和标志，传递到 ext4 层 |
| `struct folio` | `include/linux/mm_types.h` | 页缓存单元，preadv 读入数据 |
| `struct bio` | `include/linux/blk_types.h` | 块层 I/O 单元，REQ_OP_READ |
| `struct nvme_command` | `drivers/nvme/host/nvme.h` | NVMe 读命令 (opcode=0x02) |
| `struct nvme_queue` | `drivers/nvme/host/nvme.h` | SQ/CQ 队列管理，MMIO 门铃操作 |

---

## 12 总结

preadv 将**定位 I/O**和**分散/聚集 I/O**两个特性结合：

1. **定位语义**（来自 pread64）：栈局部变量 `pos`，不更新 `f_pos`，消除偏移竞争
2. **分散/聚集**（来自 readv）：`import_iovec` 导入多段 iovec，`UIO_FASTIOV` 栈优化
3. **preadv2 扩展**：`RWF_*` 标志支持 NOWAIT、DSYNC、HIPRI 等特性
4. **下游路径完全共享**：preadv 在 `do_iter_readv_writev` 之后与 read 完全相同
5. **ARM64 参数编码**：64 位 `loff_t pos` 从两个 32 位参数 `pos_h`/`pos_l` 拼装

关键函数调用等价关系：
```
preadv(fd, vec, vlen, pos)  =  readv 的分散/聚集 + pread64 的定位语义
preadv2(fd, vec, vlen, pos, RWF_NOWAIT) = preadv + 非阻塞语义
```