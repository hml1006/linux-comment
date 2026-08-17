# readv 系统调用完整路径分析

## 1 概述

`readv` 是 Linux 的**分散输入/聚集输出（scatter-gather）** 读系统调用。与 `read` 的核心区别在于：`readv` 使用 `struct iovec` 数组描述**多个不连续的用户缓冲区**，内核将数据分散填入这些缓冲区，且全程在内核态完成缓冲区列表的拷贝与迭代。

### readv 与 read 的架构关系

```
read(fd, buf, count)                          readv(fd, iovec, iovcnt)
  │                                              │
  └─ new_sync_read()                            └─ do_iter_readv_writev()
       │                                             │
       └─ init_sync_kiocb + iov_iter_ubuf()          └─ init_sync_kiocb + import_iovec()
            │                                             │
            └─── 两者在此汇聚 ───┐
                                │
                filp->f_op->read_iter(&kiocb, &iter)
                                │
                          ext4_file_read_iter
```

**关键区别**：
- `new_sync_read` 通过 `iov_iter_ubuf()` 将**单缓冲区**包装为 `iov_iter`
- `do_iter_readv_writev` 通过 `import_iovec()` 从用户态拷贝**多段 iovec** 构建 `iov_iter`
- 汇聚后，下游路径（ext4、page cache、block、NVMe）完全一致

### 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | readv 系统调用分发 (fs/read_write.c) |
| **VFS** | iovec 导入 + kiocb 初始化 (fs/read_write.c) |
| **ext4** | ext4_file_read_iter → 读/预读 (fs/ext4/file.c) |
| **Page Cache** | filemap_read → 缓存查询/填充 (mm/filemap.c) |
| **Block Layer** | BIO 提交 (block/blk-core.c, blk-mq.c) |
| **NVMe 驱动** | 命令提交 + 中断完成 (drivers/nvme/host/pci.c) |

---

## 2 系统调用入口

### 2.1 SYSCALL_DEFINE3(readv)

```c
// fs/read_write.c:1338
SYSCALL_DEFINE3(readv, unsigned long, fd,
                 const struct iovec __user *, vec,
                 unsigned long, vlen)
{
    return do_readv(fd, vec, vlen, 0);
}
```

| 参数 | 类型 | 说明 |
|--|--|--|
| `fd` | `unsigned long` | 文件描述符 |
| `vec` | `const struct iovec __user *` | 用户态 iovec 数组指针 |
| `vlen` | `unsigned long` | iovec 数组元素个数 |

### 2.2 do_readv - fd 查找与位置获取

```c
// fs/read_write.c:1244
static ssize_t do_readv(unsigned long fd, const struct iovec __user *vec,
                        unsigned long vlen, rwf_t flags)
{
    CLASS(fd_pos, f)(fd);        // 通过 fd 查找 struct fd，持有引用
    ssize_t ret = -EBADF;

    if (!fd_empty(f)) {
        loff_t pos, *ppos = file_ppos(fd_file(f));  // 获取文件位置指针
        if (ppos) {
            pos = *ppos;
            ppos = &pos;
        }
        ret = vfs_readv(fd_file(f), vec, vlen, ppos, flags);
        if (ret >= 0 && ppos)
            fd_file(f)->f_pos = pos;  // 更新文件位置
    }

    if (ret > 0)
        add_rchar(current, ret);   // 统计读取字节数
    inc_syscr(current);            // 统计系统调用次数
    return ret;
}
```

---

## 3 VFS 层：vfs_readv

### 3.1 vfs_readv 完整流程

```c
// fs/read_write.c:1166
static ssize_t vfs_readv(struct file *file, const struct iovec __user *vec,
                         unsigned long vlen, loff_t *pos, rwf_t flags)
{
    struct iovec iovstack[UIO_FASTIOV];     // 栈上快速缓冲区 (8个 iovec)
    struct iovec *iov = iovstack;
    struct iov_iter iter;
    size_t tot_len;
    ssize_t ret = 0;

    // 权限检查
    if (!(file->f_mode & FMODE_READ))
        return -EBADF;
    if (!(file->f_mode & FMODE_CAN_READ))
        return -EINVAL;

    // 从用户态拷贝 iovec 数组 → 构建 iov_iter
    ret = import_iovec(ITER_DEST, vec, vlen,
                       ARRAY_SIZE(iovstack), &iov, &iter);
    if (ret < 0) return ret;

    tot_len = iov_iter_count(&iter);    // 所有段的总长度
    if (!tot_len) goto out;

    ret = rw_verify_area(READ, file, pos, tot_len);  // 读写区域验证
    if (ret < 0) goto out;

    // 核心：通过 read_iter 方法读取
    if (file->f_op->read_iter)
        ret = do_iter_readv_writev(file, &iter, pos, READ, flags);
    else
        ret = do_loop_readv_writev(file, &iter, pos, READ, flags);
    // ext4 使用 read_iter → do_iter_readv_writev 路径
out:
    if (ret >= 0)
        fsnotify_access(file);
    kfree(iov);        // 如果 iovec 超过 UIO_FASTIOV，之前已用 kmalloc 分配
    return ret;
}
```

### 3.2 import_iovec - 用户态 iovec 导入

```c
// lib/iov_iter.c:1436
ssize_t import_iovec(int type, const struct iovec __user *uvec,
                     unsigned nr_segs, unsigned fast_segs,
                     struct iovec **iovp, struct iov_iter *i)
```

核心流程：

```
import_iovec()
  │
  ├─ nr_segs > UIO_FASTIOV(8) ?  // 小量用栈，大量用堆
  │    ├─ 是 → kmalloc(+UIOEVENT) 分配堆内存
  │    └─ 否 → 使用栈上 iovstack
  │
  ├─ copy_from_user(iov, uvec, ...)  // 从用户态拷贝 iovec 数组
  │
  ├─ iov 合法性校验:
  │    ├─ 每个 iov.iov_len 检测
  │    ├─ 总长度不能超过 MAX_RW_COUNT
  │    └─ 每个缓冲区的 access_ok 检查
  │
  └─ iov_iter_init(i, type, iov, nr_segs, total_len)
       → 初始化 iov_iter 结构体
```

### 3.3 do_iter_readv_writev - kiocb 初始化

```c
// fs/read_write.c:988
static ssize_t do_iter_readv_writev(struct file *filp, struct iov_iter *iter,
                                    loff_t *ppos, int type, rwf_t flags)
{
    struct kiocb kiocb;
    ssize_t ret;

    init_sync_kiocb(&kiocb, filp);      // 初始化同步 kiocb
    ret = kiocb_set_rw_flags(&kiocb, flags, type);  // 设置 RWF_* 标志
    if (ret) return ret;
    kiocb.ki_pos = (ppos ? *ppos : 0);  // 设置 I/O 偏移

    // 对于 ext4 文件系统，read_iter 指向 ext4_file_read_iter
    ret = filp->f_op->read_iter(&kiocb, iter);
    BUG_ON(ret == -EIOCBQUEUED);        // 同步路径不应返回异步标记

    if (ppos)
        *ppos = kiocb.ki_pos;           // 更新文件位置
    return ret;
}
```

### 3.4 do_loop_readv_writev - 回退路径（无 read_iter）

```c
// fs/read_write.c:1011
static ssize_t do_loop_readv_writev(struct file *filp, struct iov_iter *iter,
                                    loff_t *ppos, int type, rwf_t flags)
{
    ssize_t ret = 0;
    while (iov_iter_count(iter)) {
        // 逐段调用 f_op->read（传统接口）
        nr = filp->f_op->read(filp, iter_iov_addr(iter),
                              iter_iov_len(iter), ppos);
        if (nr < 0) break;
        ret += nr;
        if (nr != iter_iov_len(iter)) break;
        iov_iter_advance(iter, nr);     // 推进迭代器到下一个 iovec 段
    }
    return ret;
}
```

---

## 4 readv 与 read 的汇聚点

以下路径与 `SYSCALL_DEFINE3(read)` 的 `new_sync_read` 路径完全一致。

| 维度 | read | readv |
|--|--|--|
| 入口 | `new_sync_read` | `do_iter_readv_writev` |
| kiocb 初始化 | `init_sync_kiocb` | `init_sync_kiocb` |
| iter 初始化 | `iov_iter_ubuf`（单 buf） | `import_iovec`（多段 iovec） |
| read_iter 调用 | `filp->f_op->read_iter` | `filp->f_op->read_iter` |
| 实际跳转 | `ext4_file_read_iter` | `ext4_file_read_iter` |

**结论**：readv 经过 VFS 层后，**完全复用** read 的 ext4/page cache/block/NVMe 路径。

---

## 5 ext4 文件系统层

### 5.1 ext4_file_read_iter

```c
// fs/ext4/file.c:186
static ssize_t ext4_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    if (unlikely(ext4_forced_shutdown(inode->i_sb)))
        return -EIO;

    if (!iov_iter_count(to))
        return 0;        // 零长度请求

    // DAX 路径
    if (IS_DAX(inode))
        return ext4_dax_read_iter(iocb, to);

    // Direct I/O 路径
    if (iocb->ki_flags & IOCB_DIRECT)
        return ext4_dio_read_iter(iocb, to);

    // 缓冲 I/O 路径 + 预读
    return generic_file_read_iter(iocb, to);
}
```

### 5.2 generic_file_read_iter → filemap_read

```
generic_file_read_iter(iocb, iter)          // mm/filemap.c:3014
  ├─ 检查 IOCB_DIRECT → 无
  │
  └─ filemap_read(iocb, iter, ret)          // mm/filemap.c:2794
       ├─ filemap_get_pages(iocb, iter, &fbat)  // 获取 folio 页
       │    ├─ filemap_get_read_batch(mapping, ...)  // 批量查找缓存
       │    ├─ page_cache_sync_ra(&ractl, ...)      // 同步预读
       │    └─ filemap_create_folio(mapping, ...)   // mm/filemap.c:2626
       │         └─ a_ops->read_folio(file, folio)
       │              └─ ext4_read_folio(filp, folio)  // → BIO 提交
       │
       └─ copy_page_to_iter(folio, offset, bytes, iter)
            → 将页缓存数据拷贝到用户缓冲区
            → iov_iter 原生支持多段 scatter-gather
```

### 5.3 ext4_read_folio → ext4_mpage_readpages

```
ext4_read_folio(file, folio)                  // fs/ext4/readpage.c:395
  └─ ext4_mpage_readpages(inode, folio, NULL, folio)
       ├─ ext4_map_blocks(...)                // 块映射 (extent 查找)
       ├─ bio_alloc(bdev, BIO_MAX_VECS, ...) // 分配 BIO
       ├─ bio_add_folio(bio, folio, ...)     // 添加数据页
       ├─ bio->bi_end_io = mpage_end_io      // 设置完成回调
       └─ blk_crypto_submit_bio(bio)         // 提交 BIO
```

---

## 6 块设备层

```
blk_crypto_submit_bio(bio)                    // 加密/直接提交
  └─ submit_bio(bio)                          // block/blk-core.c:992
       └─ __submit_bio(bio)                   // block/blk-core.c:636
            └─ blk_mq_submit_bio(bio)         // block/blk-mq.c:3151
                 ├─ bio split / merge 检查
                 ├─ blk_mq_get_request()      // 分配 request
                 ├─ blk_mq_bio_to_request()   // 绑 bio 到 request
                 ├─ blk_add_rq_to_plug()      // 尝试 plug 聚合
                 └─ __blk_mq_issue_directly() // 或直接提交
                      └─ hctx->ops->queue_rq()
                           └─ nvme_queue_rq
```

---

## 7 NVMe 驱动层

### 7.1 命令提交

```
nvme_queue_rq(hctx, bd)                        // pci.c:1405
  └─ nvme_prep_rq(req)                         // pci.c:1368
       ├─ nvme_setup_cmd(ns, req, cmd)         // nvme_cmd_read
       └─ nvme_map_data(req, cmd)              // PRP/SGL DMA 映射
  └─ nvme_sq_copy_cmd(nvmeq, req)              // pci.c:730 memcpy 到 SQ ring
  └─ nvme_write_sq_db(nvmeq)                   // pci.c:713 writel MMIO doorbell
```

### 7.2 中断完成

```
nvme_irq(irq, nvmeq)                           // pci.c:1599
  └─ nvme_poll_cq(nvmeq, ...)                  // pci.c:1578
       ├─ nvme_handle_cqe(nvmeq, cqe)          // pci.c:1531
       └─ nvme_ring_cq_doorbell(nvmeq)
  └─ nvme_pci_complete_batch(breq)             // pci.c:1563
       └─ blk_mq_end_request_batch()
            └─ bio_endio(bio)
                 └─ bio->bi_end_io()
                      └─ mpage_end_io(bio)     // fs/ext4/readpage.c:167
                           └─ __read_end_io(folio)
                                └─ folio_end_read(folio)  // 标记 uptodate + 解锁
```

---

## 8 数据回传：copy_page_to_iter

IO 完成后，在 `filemap_read` 中调用 `copy_page_to_iter` 将数据从页缓存拷贝到用户缓冲区：

```
filemap_read()
  │
  └─ copy_page_to_iter(folio, offset, bytes, iter)
       │
       └─ iov_iter 迭代：
            ├─ 段1: sg_copy_to_buffer(iov[0].base, iov[0].len)
            ├─ 段2: sg_copy_to_buffer(iov[1].base, iov[1].len)
            ├─ ...
            └─ 直到所有数据拷贝完毕
```

`copy_page_to_iter` 能**自动跨越 iov_iter 中的多个 iovec 段**，将一页数据连续拷贝到多个不连续的用户缓冲区中——这是 readv 相对于 read 的核心优势。

---

## 9 readv vs read 对比

| 对比项 | read | readv |
|--|--|--|
| 用户参数 | `buf, count` | `vec, vlen` (iovec 数组) |
| 缓冲区数量 | 1 | 任意 (最多 UIO_FASTIOV 或更多) |
| 用户态到内核态拷贝 | 无额外拷贝 | `import_iovec` 拷贝 iovec 数组 |
| I/O 路径 | `new_sync_read` | `vfs_readv → do_iter_readv_writev` |
| page cache → 用户拷贝 | `copy_page_to_iter` | 同上（自动 scatter-gather） |
| 下游 ext4/block/NVMe | 完全一致 | 完全一致 |

---

## 10 函数调用栈

```
/* ========== readv 主路径 ========== */
/* 分散/聚集 I/O 读路径 */

SYSCALL_DEFINE3(readv, fd, vec, vlen)              // fs/read_write.c:1338 — 系统调用入口
└─ do_readv(fd, vec, vlen, 0)                      // fs/read_write.c:1244 — 参数验证层
   ├─ CLASS(fd_pos, f)(fd)                          // fs/file.c — 通过 fd 获取 struct fd
   ├─ [fd_empty(f)] → return -EBADF                 // 文件描述符有效性检查
   │
   ├─ loff_t pos, *ppos = file_ppos(fd_file(f))    // 获取 file->f_pos 指针
   │  └─ [ppos != NULL] → pos = *ppos; ppos = &pos  // 保存当前 f_pos
   │
   └─ vfs_readv(fd_file(f), vec, vlen, ppos, flags) // fs/read_write.c:1166 — VFS 读入口
      │                                                // ppos 指向 f_pos 或栈副本
      ├─ [!(file->f_mode & FMODE_READ)] → return -EBADF  // 读权限检查
      ├─ [!(file->f_mode & FMODE_CAN_READ)] → return -EINVAL // 读能力检查
      │
      ├─ import_iovec(ITER_DEST, vec, vlen, ...)    // lib/iov_iter.c — 导入多段 iovec
      │  ├─ [nr_segs ≤ UIO_FASTIOV(8)]             // 栈上 iovstack[8] 零分配
      │  │  └─ iov = iovstack                       // 无 kmalloc 开销
      │  ├─ [nr_segs > 8]                           // 动态分配
      │  │  └─ iov = kmalloc_array(nr_segs, ...)    // 需要后续 kfree
      │  └─ __import_iovec() → 拷贝用户空间 iovec → 初始化 iov_iter
      │     └─ iter.iter_type = ITER_IOVEC          // 类型：多段 iovec
      │        iter.data_source = ITER_DEST         // 方向：从内核到用户
      │        iter.nr_segs = vlen                   // 段数
      │        iter.iov = iov                        // 指向 iovec 数组
      │
      ├─ rw_verify_area(READ, file, pos, tot_len)   // fs/read_write.c — 区域验证
      │  └─ security_file_permission(file, MAY_READ) // LSM 安全钩子
      │
      ├─ [file->f_op->read_iter]                     // 优先使用 read_iter 接口
      │  └─ do_iter_readv_writev(file, &iter, ppos, READ, flags) // fs/read_write.c:988 — 核心分发
      │     ├─ init_sync_kiocb(&kiocb, filp)         // include/linux/fs.h — 初始化 kiocb
      │     ├─ kiocb_set_rw_flags(&kiocb, flags, READ) // 解析 RWF_* → IOCB_* 标志
      │     ├─ kiocb.ki_pos = (ppos ? *ppos : 0)     // 赋值 f_pos 当前位置
      │     │
      │     └─ filp->f_op->read_iter(&kiocb, &iter)  // → ext4_file_read_iter
      │        │                                       // fs/ext4/file.c:186 — ext4 读分发
      │        ├─ [IS_DAX(inode)] → ext4_dax_read_iter()  // DAX 直接访问路径
      │        ├─ [iocb->ki_flags & IOCB_DIRECT] → ext4_dio_read_iter() // DirectIO 路径
      │        │
      │        └─ generic_file_read_iter(iocb, iter)  // mm/filemap.c:3014 — 页缓存读路径
      │           └─ filemap_read(iocb, iter, retval) // mm/filemap.c:2794 — 页缓存读核心
      │              │
      │              │ [循环: 每轮读取一组 folio]
      │              │
      │              ├─ filemap_get_pages(iocb, iter, ...)  // mm/filemap.c:2693 — 获取页
      │              │  └─ filemap_get_read_batch(mapping, ...)  // XArray 批量查找
      │              │     ├─ xa_load(&mapping->i_pages, index)  // 基数树查找
      │              │     │  ├─ [页缓存命中] → folio 引用 + 添加
      │              │     │  └─ [页缓存未命中] → 进入预读路径
      │              │     │
      │              │     └─ [页缓存未命中] → page_cache_sync_readahead(...)
      │              │        └─ ra_alloc_folio(...)             // 分配 folio 并加入页缓存
      │              │
      │              ├─ [页缓存命中]
      │              │  └─ copy_page_to_iter(folio, offset, bytes, iter)
      │              │     └─ 数据拷贝到 iovec 各段（多段分散拷贝）
      │              │
      │              └─ [页缓存未命中]
      │                 └─ filemap_create_folio(mapping, index)  // mm/filemap.c:2626 — 创建新 folio
      │                    └─ filemap_read_folio(file, ...)      // 触发磁盘 I/O
      │                       └─ mapping->a_ops->read_folio(file, folio)  // → ext4_read_folio
      │                          │
      │                          └─ ext4_read_folio(file, folio)  // fs/ext4/readpage.c:395
      │                             └─ ext4_mpage_readpages(mapping, ...)  // fs/ext4/readpage.c:211
      │                                ├─ ext4_map_blocks(inode, &map, ...)  // fs/ext4/inode.c:600
      │                                │  → 逻辑块号 → 物理块号映射
      │                                │
      │                                ├─ bio_alloc(bdev, nr_vecs, REQ_OP_READ, ...)  // 分配 bio
      │                                ├─ bio_add_folio(bio, folio, ...)              // folio 加入 bio
      │                                ├─ bio->bi_end_io = mpage_end_io               // 设置完成回调
      │                                │
      │                                └─ submit_bio(bio)                             // 提交到块层
      │                                   └─ submit_bio_noacct(bio)                    // block/blk-core.c
      │                                      └─ __submit_bio(bio)                      // 块层入口
      │                                         └─ blk_mq_submit_bio(bio)             // block/blk-mq.c:3151
      │                                            ├─ bio_split_to_limits(bio, ...)   // 拆分超限 bio
      │                                            ├─ blk_mq_attempt_bio_merge(...)   // 尝试合并
      │                                            ├─ blk_mq_get_new_requests(...)     // 分配 request
      │                                            ├─ blk_mq_bio_to_request(...)       // bio→request 绑定
      │                                            ├─ blk_add_rq_to_plug(rq)           // plug 批处理
      │                                            └─ blk_finish_plug(...)             // 刷新 plug 列表
      │                                               └─ blk_mq_dispatch_plug_list(...)
      │                                                  └─ q->mq_ops->queue_rq(...)  // → nvme_queue_rq
      │                                                     │
      │                                                     └─ nvme_queue_rq(hctx, bd, ...)  // drivers/nvme/host/pci.c
      │                                                        ├─ nvme_prep_rq(dev, req)        // 准备命令
      │                                                        │  ├─ nvme_setup_cmd(req, cmd)    // 构造 NVMe 命令
      │                                                        │  │  └─ cmd->opcode = nvme_cmd_read (0x02)
      │                                                        │  │     cmd->nsid = nsid
      │                                                        │  │     cmd->slba = 起始 LBA
      │                                                        │  │     cmd->length = 块数 - 1
      │                                                        │  │
      │                                                        │  └─ nvme_map_data(dev, req, ...)  // DMA 地址映射
      │                                                        │     └─ dma_map_sg(dev, ...)       // PRP/SGL 表
      │                                                        │
      │                                                        ├─ nvme_sq_copy_cmd(nvmeq, cmd)   // memcpy 到 SQ 环
      │                                                        │
      │                                                        └─ nvme_write_sq_db(nvmeq)        // writel MMIO 门铃
      │                                                           └─ writel(tail, doorbell_addr)   // 通知硬件取命令
      │
      ├─ [file->f_op->read]  // 回退路径（无 read_iter 的老接口）
      │  └─ do_loop_readv_writev(file, &iter, pos, READ, flags)  // fs/read_write.c:1011 — 逐段 read
      │     └─ [while 循环]
      │        ├─ filp->f_op->read(filp, iter_iov_addr(iter), iter_iov_len(iter), ppos)
      │        └─ iov_iter_advance(iter, nr)  // 推进到下一 iovec 段
      │
      └─ [完成后:]
         ├─ fsnotify_access(file)                           // 文件访问通知
         ├─ kfree(iov)                                      // 释放 iovec（若动态分配）
         └─ 返回 ret

  [do_readv 返回后:]
  ├─ [ret >= 0 && ppos] → fd_file(f)->f_pos = pos           // 更新 f_pos（readv 更新文件位置）
  ├─ [ret > 0] → add_rchar(current, ret)                     // 读字节统计
  └─ inc_syscr(current)                                      // 读系统调用计数

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
        └─ iterate_and_advance(iter, bytes, ...)       // 自动跨越多个 iovec 段
           └─ [逐段拷贝] 将数据从页缓存拷贝到 iovec 各段
```

---

## 11 流程图

```
                    readv(fd, iov, iovcnt)
                           |
                    +------v------+
                    | SYSCALL_    |  系统调用入口
                    | DEFINE3     |  (fs/read_write.c:1338)
                    | (readv)     |
                    +------+------+
                           |
                    +------v------+
                    | do_readv    |  -- CLASS(fd_pos, f) 获取 fd
                    | (fs/read_   |  -- file_ppos → f_pos
                    |  write.c)   |  -- vfs_readv 进入 VFS
                    +------+------+
                           |
                    +------v------+
                    | vfs_readv   |  -- [FMODE_READ] 检查
                    | (fs/read_   |  -- [FMODE_CAN_READ] 检查
                    |  write.c)   |  -- import_iovec(ITER_DEST)
                    |  1166       |  -- rw_verify_area
                    +------+------+
                           |
                    +------v------+
                    | import_iovec|  -- 导入用户空间多段 iovec
                    | (lib/iov_   |  -- [nr_segs ≤ 8] 栈上 iovstack
                    |  iter.c)    |  -- [nr_segs > 8] kmalloc
                    | 1436        |  -- 初始化 iov_iter (ITER_DEST)
                    +------+------+
                           |
              +-----v-----+-----v-----+
              |                   |
       +------v------+   +------v------+
       | 有 read_iter |   | 无 read_iter |
       | 接口         |   | → 回退路径   |
       +------+------+   +------+------+
              |                  |
       +------v------+   +------v------+
       | do_iter_    |   | do_loop_    |
       | readv_writev|   | readv_writev|
       | (fs/read_   |   | → 逐段调用  |
       |  write.c    |   |   f_op->read|
       |  988)       |   +------+------+
       +------+------+
              |
       +------v------+
       | init_sync_  |
       | kiocb       |
       | kiocb.ki_   |
       | pos = f_pos |
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
                    |  c:2794)     |
                    +------+------+
                           |
                    +------v------+
                    | filemap_get_ |
                    | pages        |
                    | (mm/filemap. |
                    |  c:2693)     |
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
        |  用户 iovec |   |  readpage.c |
        |  各段)      |   |  395)       |
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
               | 更新 f_pos  |
               | 返回读取字节数|
               | 或错误码      |
               +-------------+
```

---

## 12 关键数据结构 (C代码 + 注释)

```c
// ===== VFS 层 =====

// 用户空间 I/O 向量——readv 通过 iovec 数组传递多个读缓冲区
struct iovec {
    void __user *iov_base;   // 用户空间缓冲区基地址（readv 读入数据的目标）
    size_t       iov_len;    // 该段缓冲区长度
};

// 多段缓冲区迭代器——readv 使用 ITER_DEST（数据从文件读入用户空间）
struct iov_iter {
    u8 iter_type;            // 迭代器类型：ITER_IOVEC（多段 iovec，readv 使用）/ ITER_UBUF（单段）
    u8 data_source;          // 数据方向：ITER_DEST（readv: 从文件读入 iovec 各段）
    size_t iov_offset;       // 当前 iovec 段内的偏移（跨段续传时使用）
    size_t count;            // 剩余未传输字节总数
    union {
        const struct iovec *iov;       // 指向 iovec 数组（import_iovec 导入）
        struct {
            void __user *ubuf;         // 用户缓冲区基地址（单段模式，read/write 使用）
            size_t len;                // 缓冲区长度
        };
    };
    unsigned long nr_segs;   // iovec 段数（readv 核心参数，vlen 传入）
};

// I/O 控制块——携带 readv 读操作的所有上下文
struct kiocb {
    struct file      *ki_filp;       // 目标文件对象（通过 fd 查找获得）
    loff_t            ki_pos;        // 读取位置（readv 从 file->f_pos 获取，完成后更新 f_pos）
    unsigned short    ki_opcode;     // I/O 操作码
    unsigned short    ki_flags;      // I/O 标志：IOCB_DIRECT（直接 I/O）、IOCB_NOWAIT（非阻塞）等
    short             ki_ioprio;     // I/O 优先级
    void              *private;      // 文件系统私有数据
    union {
        void          (*ki_complete)(struct kiocb *iocb, long ret);
        // 异步 I/O 完成回调（同步操作时为 NULL）
    };
};

// 文件对象——每个 open 创建一个
struct file {
    struct path f_path;              // 文件路径（dentry + mount）
    struct inode *f_inode;           // 指向 inode 的快捷方式
    const struct file_operations *f_op;  // 文件操作函数表
    spinlock_t f_lock;               // 自旋锁
    atomic_long_t f_count;           // 引用计数
    unsigned int f_flags;            // 文件状态标志（O_RDONLY 等）
    fmode_t f_mode;                  // 打开模式（FMODE_READ, FMODE_CAN_READ 等）
    loff_t f_pos;                    // 当前读写位置（readv 完成后更新此字段）
    void *private_data;              // 文件系统私有数据
    struct address_space *f_mapping; // 页缓存映射
};

// 页缓存 folio——readv 读取数据的目标
struct folio {
    unsigned long flags;     // folio 标志：PG_uptodate（数据有效）、PG_locked（锁定）等
    struct address_space *mapping;  // 所属的 address_space（文件页缓存树）
    loff_t index;            // 在文件内的页索引（pos >> PAGE_SHIFT）
    void *private;           // 文件系统私有数据（ext4 的 buffer_head 链表）
    atomic_t _mapcount;      // 映射计数
    atomic_t _refcount;      // 引用计数（页缓存引用 + 进程映射）
};

// ===== 块层 =====

// 块 I/O 请求——readv 的磁盘读请求载体
struct bio {
    struct bio          *bi_next;         // bio 链表（plug 聚合时使用）
    struct block_device *bi_bdev;         // 目标块设备
    blk_opf_t            bi_opf;          // 操作标志：REQ_OP_READ（readv 读操作）
    unsigned short       bi_flags;        // bio 标志
    unsigned short       bi_ioprio;       // I/O 优先级
    struct bio_vec       *bi_io_vec;      // 数据段数组
    unsigned int         bi_vcnt;         // bio_vec 段数
    struct bvec_iter     bi_iter;         // 当前迭代位置（bi_sector 为起始扇区）
    bio_end_io_t         *bi_end_io;      // 完成回调：readv → mpage_end_io
    void                 *bi_private;     // 私有数据
};

// ===== NVMe 驱动层 =====

// NVMe 命令结构——提交到 NVMe 控制器的读命令
struct nvme_command {
    struct {
        u8  opcode;          // 操作码：nvme_cmd_read = 0x02（readv 读命令）
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

| 数据结构 | 头文件 | 在 readv 中的作用 |
|----------|--------|------------------|
| `struct iovec` | `include/uapi/linux/uio.h` | 用户空间多段缓冲区描述（分散读的基础） |
| `struct iov_iter` | `include/linux/uio.h` | 管理多段 iovec 迭代，data_source=ITER_DEST |
| `struct kiocb` | `include/linux/fs.h` | 携带 I/O 位置 pos 和标志，传递到 ext4 层 |
| `struct file` | `include/linux/fs.h` | 文件对象，f_pos 被 readv 更新 |
| `struct folio` | `include/linux/mm_types.h` | 页缓存单元，readv 读入数据 |
| `struct bio` | `include/linux/blk_types.h` | 块层 I/O 单元，REQ_OP_READ |
| `struct nvme_command` | `drivers/nvme/host/nvme.h` | NVMe 读命令 (opcode=0x02) |
| `struct nvme_queue` | `drivers/nvme/host/nvme.h` | SQ/CQ 队列管理，MMIO 门铃操作 |

---

## 13 关键优化机制

### 13.1 快速 iovec 路径 (UIO_FASTIOV)

```c
struct iovec iovstack[UIO_FASTIOV];  // 8 个 iovec 的栈上数组
struct iovec *iov = iovstack;
```

当 `vlen <= UIO_FASTIOV(8)` 时，iovec 数组直接在栈上分配，无需堆分配。超过 8 个段时才通过 `kmalloc` 分配。这是 readv 的微优化。

### 13.2 iov_iter scatter-gather

`filemap_read` 中的 `copy_page_to_iter()` 使用 `iov_iter` 的原生 scatter-gather 能力，自动处理多段用户缓冲区。具体流程：

```
copy_page_to_iter(folio, offset, bytes, iter)
  │
  └─ iterate_and_advance(iter, bytes, ...)
       │
       └─ 对 iov_iter 中的每一段:
            ├─ memcpy(page_addr + off, iov[i].base, seg_len)
            └─ iov_iter_advance(iter, seg_len)
```

### 13.3 下游路径优化（与 read 共享）

所有读路径优化均继承自 read 系统调用：
- **Page cache 批量查找**：`filemap_get_read_batch` 一次取多个 folio
- **同步预读**：`page_cache_sync_ra` 预测后续读请求
- **NVMe 批量完成**：`nvme_pci_complete_batch` 批量处理 CQ 条目
- **Block plug**：`blk_add_rq_to_plug` 合并相邻小请求

---

## 14 总结

```
                    readv 系统调用完整数据流

用户态 readv(fd, [{buf1,4K}, {buf2,8K}, {buf3,4K}], 3)
  │
  │   [用户态 → 内核态]
  │
  ├─ 1. 系统调用入口 (SYSCALL_DEFINE3)
  │
  ├─ 2. VFS 层
  │    ├─ import_iovec: 拷贝 iovec[3] 到内核 (验证每个段)
  │    ├─ rw_verify_area: 权限检查
  │    └─ do_iter_readv_writev: init_sync_kiocb → read_iter
  │
  ├─ 3. ext4 文件系统
  │    └─ ext4_file_read_iter → generic_file_read_iter
  │
  ├─ 4. Page Cache
  │    ├─ filemap_get_pages: 缓存查找/填充
  │    └─ ext4_read_folio (未命中时) → ext4_mpage_readpages
  │
  ├─ 5. Block 层
  │    └─ submit_bio → blk_mq_submit_bio → nvme_queue_rq
  │
  ├─ 6. NVMe 驱动
  │    ├─ nvme_prep_rq → nvme_sq_copy_cmd → writel(Doorbell)
  │    └─ [硬件 DMA 读取数据到 DRAM]
  │
  ├─ 7. 中断完成
  │    └─ nvme_irq → mpage_end_io → folio_end_read
  │
  └─ 8. 数据回传
       └─ copy_page_to_iter(folio, iter)
            └─ scatter-gather: buf1[4K] ← 部分, buf2[6K] ← 剩余, buf3[2K] ← 继续
```

**核心要点**：
- readv 与 read 共享 **ext4 → Block → NVMe** 完整下游路径
- 唯一区别在 VFS 入口层：`import_iovec`  vs `iov_iter_ubuf`
- `copy_page_to_iter` 负责 scatter-gather 数据分发，对下游完全透明
- 当 `iovec` 段数 ≤ 8 时，使用栈上数组，零堆分配开销
