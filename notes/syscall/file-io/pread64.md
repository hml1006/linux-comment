# pread64 系统调用完整路径分析

## 1 概述

`pread64` 是 Linux 的**定位读（positioned read）** 系统调用。与 `read` 的核心区别在于：`pread64` 由调用者**指定读取位置**，且**不改变文件指针** `file->f_pos`，因此特别适合多线程并发读取同一文件的不同位置。

### pread64 vs read 核心差异

| 对比项 | read | pread64 |
|--|--|--|
| 位置来源 | `file->f_pos`（文件指针） | 系统调用参数 `pos` |
| 位置更新 | 读取后更新 `file->f_pos` | **不更新** |
| 文件模式要求 | 无特殊要求 | 需 `FMODE_PREAD`（常规文件默认支持） |
| 多线程安全 | 不安全（共享位置） | **安全**（位置在栈上） |
| 参数签名 | `read(fd, buf, count)` | `pread64(fd, buf, count, pos)` |

### 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | pread64 系统调用分发 (fs/read_write.c) |
| **VFS** | vfs_read → new_sync_read (fs/read_write.c) |
| **ext4** | ext4_file_read_iter → 预读 (fs/ext4/file.c) |
| **Page Cache** | filemap_read → 缓存查询/填充 (mm/filemap.c) |
| **Block Layer** | BIO 提交 (block/blk-core.c, blk-mq.c) |
| **NVMe 驱动** | 命令提交 + 中断完成 (drivers/nvme/host/pci.c) |

---

## 2 系统调用入口

### 2.1 SYSCALL_DEFINE4(pread64)

```c
// fs/read_write.c:944
SYSCALL_DEFINE4(pread64, unsigned int, fd, char __user *, buf,
                size_t, count, loff_t, pos)
{
    return ksys_pread64(fd, buf, count, pos);
}
```

| 参数 | 类型 | 说明 |
|--|--|--|
| `fd` | `unsigned int` | 文件描述符 |
| `buf` | `char __user *` | 用户空间缓冲区 |
| `count` | `size_t` | 读取字节数 |
| `pos` | `loff_t` | **读取起始位置**（与 `read` 的核心区别） |

### 2.2 ksys_pread64 - 内核实现

```c
// fs/read_write.c:928
ssize_t ksys_pread64(unsigned int fd, char __user *buf, size_t count,
                     loff_t pos)
{
    if (pos < 0)
        return -EINVAL;

    CLASS(fd, f)(fd);                    // 通过 fd 获取 struct fd（含自动引用释放）
    if (fd_empty(f))
        return -EBADF;

    if (fd_file(f)->f_mode & FMODE_PREAD)   // 检查文件是否支持定位读
        return vfs_read(fd_file(f), buf, count, &pos);
    //                     注意：&pos 指向栈上局部变量，非 file->f_pos

    return -ESPIPE;                      // 不支持定位操作的设备（如管道）
}
```

**核心设计**：`pos` 是栈上局部变量的地址，`vfs_read` 读取时使用此位置，但不会回写 `file->f_pos`。这正是 `pread64` "不改变文件指针" 语义的实现方式。

---

## 3 VFS 层：vfs_read

```c
// fs/read_write.c:624
ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    ssize_t ret;

    if (!(file->f_mode & FMODE_READ))
        return -EBADF;
    if (!(file->f_mode & FMODE_CAN_READ))
        return -EINVAL;
    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    ret = rw_verify_area(READ, file, pos, count);   // 区域验证（锁、边界）
    if (ret) return ret;
    if (count > MAX_RW_COUNT)
        count = MAX_RW_COUNT;

    if (file->f_op->read)
        ret = file->f_op->read(file, buf, count, pos);  // 传统 read 方法
    else if (file->f_op->read_iter)
        ret = new_sync_read(file, buf, count, pos);     // 适配到 read_iter
    else
        ret = -EINVAL;

    if (ret > 0) {
        fsnotify_access(file);            // inotify 事件通知
        add_rchar(current, ret);          // 统计读取字节数
    }
    inc_syscr(current);                   // 统计系统调用次数
    return ret;
}
```

### 文件操作方法选择

对于 ext4 文件系统：
- `file->f_op->read` → **未设置**（NULL）
- `file->f_op->read_iter` → `ext4_file_read_iter`

因此 ext4 走 **`new_sync_read`** 路径（与 `read` 系统调用完全一致）。

---

## 4 new_sync_read - kiocb 与 iov_iter 初始化

```c
// fs/read_write.c:527
static ssize_t new_sync_read(struct file *filp, char __user *buf, size_t len,
                             loff_t *ppos)
{
    struct kiocb kiocb;
    struct iov_iter iter;
    ssize_t ret;

    init_sync_kiocb(&kiocb, filp);           // 初始化同步 kiocb
    kiocb.ki_pos = (ppos ? *ppos : 0);       // 使用传入的 pos（来自 pread64 参数）

    iov_iter_ubuf(&iter, ITER_DEST, buf, len);  // 单缓冲区 iov_iter

    ret = filp->f_op->read_iter(&kiocb, &iter);  // → ext4_file_read_iter
    BUG_ON(ret == -EIOCBQUEUED);

    if (ppos)
        *ppos = kiocb.ki_pos;                // 更新栈上 pos，但不影响 file->f_pos

    return ret;
}
```

**关键区别**：`*ppos = kiocb.ki_pos` 回写的是栈上变量（`ksys_pread64` 的局部 `pos`），而非 `file->f_pos`。因此 `pread64` 不修改文件指针。

---

## 5 ext4 文件系统层

### 5.1 ext4_file_read_iter → generic_file_read_iter

```
ext4_file_read_iter(iocb, to)                    // fs/ext4/file.c:186
  ├─ ext4_forced_shutdown 检查
  ├─ iov_iter_count 零长度检查
  ├─ IS_DAX → ext4_dax_read_iter                 // DAX 绕过页缓存
  ├─ IOCB_DIRECT → ext4_dio_read_iter             // Direct I/O
  └─ 缓冲 I/O → generic_file_read_iter(iocb, to) // 默认路径（含预读）
```

```c
// mm/filemap.c:3014
generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
    // ...
    if (iocb->ki_flags & IOCB_DIRECT)
        // 直接 I/O
    else
        ret = filemap_read(iocb, iter, ret);     // 缓冲读
}
```

---

## 6 Page Cache 层

### 6.1 filemap_read - 核心读路径

```
filemap_read(iocb, iter, ret)                    // mm/filemap.c:2794
  │
  ├─ 循环 (每次处理一组 folio):
  │    │
  │    ├─ filemap_get_pages(iocb, iter, &fbat)   // 批量获取 folio
  │    │    ├─ filemap_get_read_batch(mapping, ...)  // 缓存中批量查找
  │    │    ├─ [未命中] page_cache_sync_ra(...)      // 同步预读
  │    │    └─ [未命中] filemap_create_folio(...)    // 创建新 folio
  │    │         └─ a_ops->read_folio(file, folio)
  │    │              └─ ext4_read_folio             // → BIO 提交
  │    │
  │    ├─ 将 folio 标记为最近访问 (folio_activate)
  │    │
  │    └─ copy_page_to_iter(folio, offset, bytes, iter)
  │         → 将页缓存数据拷贝到用户缓冲区 buf
  │
  └─ 更新 iocb->ki_pos += bytes 读取
```

### 6.2 缺页路径：ext4_read_folio

```
filemap_create_folio → a_ops->read_folio
  └─ ext4_read_folio(filp, folio)                // fs/ext4/readpage.c:395
       └─ ext4_mpage_readpages(inode, folio, NULL, folio)
            ├─ ext4_map_blocks(...)              // 块映射（extent 查找）
            ├─ bio_alloc(bdev, BIO_MAX_VECS, ...) // 分配 BIO
            ├─ bio_add_folio(bio, folio, ...)    // 添加数据页
            ├─ bio->bi_end_io = mpage_end_io     // 设置完成回调
            └─ blk_crypto_submit_bio(bio)        // 提交 BIO
```

---

## 7 块设备层

```
blk_crypto_submit_bio(bio)
  └─ submit_bio(bio)                             // block/blk-core.c:992
       └─ __submit_bio(bio)                      // block/blk-core.c:636
            └─ blk_mq_submit_bio(bio)            // block/blk-mq.c:3151
                 ├─ blk_mq_get_request()
                 ├─ blk_mq_bio_to_request()
                 ├─ blk_add_rq_to_plug()         // Plug 聚合
                 └─ __blk_mq_issue_directly()
                      └─ hctx->ops->queue_rq
                           └─ nvme_queue_rq
```

---

## 8 NVMe 驱动层

### 8.1 命令提交

```
nvme_queue_rq(hctx, bd)                          // pci.c:1405
  └─ nvme_prep_rq(req)                           // pci.c:1368
       ├─ nvme_setup_cmd(ns, req, cmd)           // nvme_cmd_read
       │    └─ nvme_setup_rw(ns, req, cmd, nvme_cmd_read)
       └─ nvme_map_data(req, cmd)                // PRP/SGL DMA 映射
  └─ nvme_sq_copy_cmd(nvmeq, req)                // memcpy 到 SQ ring
  └─ nvme_write_sq_db(nvmeq)                     // writel MMIO doorbell
```

### 8.2 中断完成

```
nvme_irq(irq, nvmeq)                             // pci.c:1599
  └─ nvme_poll_cq(nvmeq, ...)                    // pci.c:1578
       ├─ nvme_handle_cqe(nvmeq, cqe)            // pci.c:1531
       └─ nvme_ring_cq_doorbell(nvmeq)
  └─ nvme_pci_complete_batch(breq)
       └─ blk_mq_end_request_batch()
            └─ bio_endio(bio)
                 └─ mpage_end_io(bio)            // fs/ext4/readpage.c:167
                      └─ __read_end_io(folio)
                           └─ folio_end_read(folio)  // 标记 uptodate + 解锁
```

---

## 9 pread64 vs read 调用链对比

| 步骤 | read | pread64 |
|--|--|--|
| Syscall | `SYSCALL_DEFINE3(read)` | `SYSCALL_DEFINE4(pread64)` |
| 中间层 | `ksys_read` → `vfs_read(&f->f_pos)` | `ksys_pread64` → `vfs_read(&pos)` |
| 位置变量 | `file->f_pos` 的地址 | 栈上局部 `pos` 的地址 |
| vfs_read | 完全一致 | 完全一致 |
| new_sync_read | 完全一致 | 完全一致 |
| kiocb.ki_pos 来源 | `*ppos = f_pos` | `*ppos = 栈上 pos` |
| file->f_pos 更新 | **是**（`file_pos_write`） | **否** |
| 下游 ext4/Block/NVMe | 完全一致 | 完全一致 |

---

## 10 完整流程图 (ASCII)

```
                   pread64(fd, buf, count, pos)
                              |
                    +---------v----------+
                    |  SYSCALL_DEFINE4    |  系统调用入口
                    |  (fs/read_write.c:  |  (fs/read_write.c:944)
                    |   944)              |
                    +---------+----------+
                              |
                    +---------v----------+
                    |  ksys_pread64      |  检查 pos >= 0
                    |  (fs/read_write.c: |  CLASS(fd) 获取 file
                    |   928)             |  FMODE_PREAD 检查
                    +---------+----------+
                              |
                    +---------v----------+
                    |  vfs_read(file,    |  VFS 层
                    |  buf, count, &pos) |  rw_verify_area
                    |  (fs/read_write.c: |  count 截断
                    |   624)             |
                    +---------+----------+
                              |
                    +---------v----------+
                    |  new_sync_read()   |  kiocb 初始化
                    |  (fs/read_write.c: |  kiocb.ki_pos = pos
                    |   527)             |  iov_iter_ubuf(buf)
                    +---------+----------+
                              |
                    +---------v----------+
                    |  ext4_file_read_   |  ext4 层
                    |  iter(iocb, iter)  |  (fs/ext4/file.c:186)
                    +---------+----------+
                              |
         +-------------------+-------------------+
         |                                       |
+--------v--------+                    +---------v--------+
| IS_DAX?         |                    | 通用缓冲 I/O      |
| → ext4_dax_     |                    | generic_file_read_|
|   read_iter     |                    | iter(iocb, iter)  |
+-----------------+                    +---------+---------+
         |                                       |
         |                             +---------v---------+
         |                             |  filemap_read()    |
         |                             |  (mm/filemap.c:    |
         |                             |   2794)            |
         |                             +---------+---------+
         |                                       |
         |                             +---------v---------+
         |                             | filemap_get_pages  |
         |                             | 批量获取 folio     |
         |                             +---------+---------+
         |                                       |
         |                    +------------------+------------------+
         |                    |                                     |
         |           +--------v--------+                  +--------v--------+
         |           | 页缓存命中       |                  | 页缓存未命中     |
         |           | → 直接拷贝      |                  | → page_cache_    |
         |           |                 |                  |   sync_ra()      |
         |           +--------+--------+                  | → filemap_       |
         |                    |                           |   create_folio() |
         |                    |                           +--------+--------+
         |                    |                                    |
         |                    |                           +--------v--------+
         |                    |                           | ext4_read_folio |
         |                    |                           | → ext4_mpage_   |
         |                    |                           |   readpages     |
         |                    |                           | → ext4_map_     |
         |                    |                           |   blocks        |
         |                    |                           +--------+--------+
         |                    |                                    |
         |                    |                           +--------v--------+
         |                    |                           | bio_alloc()     |
         |                    |                           | bio_add_folio() |
         |                    |                           | bi_end_io =     |
         |                    |                           |   mpage_end_io  |
         |                    |                           +--------+--------+
         |                    |                                    |
         |                    |                           +--------v--------+
         |                    |                           |  blk_crypto_    |
         |                    |                           |  submit_bio()   |
         |                    |                           +--------+--------+
         |                    |                                    |
         |                    |                           +--------v--------+
         |                    |                           |  submit_bio()   |
         |                    |                           |  → __submit_bio |
         |                    |                           |  → blk_mq_      |
         |                    |                           |    submit_bio   |
         |                    |                           +--------+--------+
         |                    |                                    |
         |                    |                           +--------v--------+
         |                    |                           | nvme_queue_rq   |
         |                    |                           | → nvme_prep_rq  |
         |                    |                           | → nvme_setup_cmd|
         |                    |                           |   (nvme_cmd_    |
         |                    |                           |    read)        |
         |                    |                           | → nvme_map_data |
         |                    |                           | → nvme_sq_copy_ |
         |                    |                           |   cmd           |
         |                    |                           | → nvme_write_sq_|
         |                    |                           |   db (MMIO)     |
         |                    |                           +--------+--------+
         |                    |                                    |
         |                    |                           +--------v--------+
         |                    |                           | [NVMe 中断完成] |
         |                    |                           | nvme_irq()      |
         |                    |                           | → nvme_poll_cq  |
         |                    |                           | → nvme_handle_  |
         |                    |                           |   cqe           |
         |                    |                           | → blk_mq_end_   |
         |                    |                           |   request_batch |
         |                    |                           | → mpage_end_io  |
         |                    |                           | → folio_end_read|
         |                    |                           +--------+--------+
         |                    |                                    |
         +---------+----------+------------------------------------+
                   |
          +--------v--------+
          | copy_page_to_   |  页缓存数据拷贝到用户缓冲区
          | iter(folio,     |  (mm/filemap.c)
          | offset, bytes,  |
          | iter)           |
          +--------+--------+
                   |
          +--------v--------+
          | 更新 kiocb.     |  *ppos = kiocb.ki_pos
          | ki_pos += bytes |  回写栈上 pos 变量
          +--------+--------+  (不影响 file->f_pos)
                   |
          +--------v--------+
          |  fsnotify       |  inotify 事件 + 统计
          |  _access(file)  |  add_rchar + inc_syscr
          +--------+--------+
                   |
          +--------v--------+
          |  返回读取字节数  |
          |  或错误码        |
          +-----------------+
```

---

## 11 函数调用栈

```
/* ========== 主路径：始终执行 ========== */
/* 从用户态系统调用入口到返回用户态的完整路径 */

SYSCALL_DEFINE4(pread64, fd, buf, count, pos)       // fs/read_write.c:944
└─ ksys_pread64(fd, buf, count, pos)                // fs/read_write.c:928
   ├─ [pos < 0] → return -EINVAL                    // 位置参数校验
   ├─ CLASS(fd, f)(fd)                               // fs/file.c — 通过 fd 获取 struct fd
   │  ├─ fdget(fd)                                   // 获取 fd 对应的 file 指针（refcount 递增）
   │  └─ [fd_empty(f)] → return -EBADF               // 无效 fd 检查
   ├─ [!(f->f_mode & FMODE_PREAD)] → return -ESPIPE  // 不支持定位读检查
   └─ vfs_read(fd_file(f), buf, count, &pos)         // fs/read_write.c:624
        │                                            // &pos 指向栈上局部变量，非 file->f_pos
        ├─ rw_verify_area(READ, file, pos, count)    // fs/read_write.c:638
        │  ├─ mandatory_lock 检查                     // 强制锁验证
        │  ├─ security_file_permission               // LSM 权限检查
        │  └─ [count > MAX_RW_COUNT] → count 截断    // 最大读长度限制
        │
        └─ [file->f_op->read_iter 存在]
           └─ new_sync_read(file, buf, count, pos)   // fs/read_write.c:527
                ├─ init_sync_kiocb(&kiocb, filp)     // include/linux/fs.h — 初始化同步 kiocb
                │  └─ kiocb.ki_complete = NULL       // 同步 I/O 无完成回调
                ├─ kiocb.ki_pos = *ppos              // 使用栈上 pos（pread64 参数）
                ├─ iov_iter_ubuf(&iter, ITER_DEST, buf, len)
                │                                    // lib/iov_iter.c — 单用户缓冲区描述
                │  └─ iter.iter_type = ITER_UBUF     // 类型：单用户缓冲区
                │  └─ iter.data_source = ITER_DEST   // 方向：从内核到用户
                │
                └─ filp->f_op->read_iter(&kiocb, &iter)  // → ext4_file_read_iter
                                                                  //
                                                                  // [ext4 层路由]
                                                                  └─ ext4_file_read_iter(iocb, to)  // fs/ext4/file.c:186
                                                                       ├─ [IS_DAX(inode)] → ext4_dax_read_iter
                                                                       │                     // DAX 路径，绕过页缓存
                                                                       ├─ [iocb->ki_flags & IOCB_DIRECT]
                                                                       │  └─ ext4_dio_read_iter    // 直接 I/O 路径
                                                                       │
                                                                       └─ [缓冲 I/O 默认路径]
                                                                          └─ generic_file_read_iter(iocb, iter)
                                                                                               // mm/filemap.c:3014
                                                                              └─ filemap_read(iocb, iter, ret)
                                                                                                   // mm/filemap.c:2794
                                                                                   │
                                                                                   ├─ 循环 (每轮处理一组 folio):
                                                                                   │    │
                                                                                   │    ├─ filemap_get_pages(iocb, iter, &fbat)
                                                                                   │    │                     // mm/filemap.c:2693
                                                                                   │    │  └─ filemap_get_read_batch(mapping, index, ...
                                                                                   │    │       // mm/filemap.c — 批量查找页缓存 folio
                                                                                   │    │       ├─ xa_load(&mapping->i_pages, index)
                                                                                   │    │       │  // XArray 查找，RCU 保护
                                                                                   │    │       │  ├─ [命中] → folio 引用 + 添加
                                                                                   │    │       │  └─ [未命中] → 见下方分支
                                                                                   │    │       └─ [未命中] → page_cache_sync_ra(...)
                                                                                   │    │            // mm/readahead.c — 同步预读
                                                                                   │    │
                                                                                   │    ├─ [页缓存命中]
                                                                                   │    │  └─ folio 已就绪 → 直接拷贝
                                                                                   │    │
                                                                                   │    ├─ [页缓存未命中]
                                                                                   │    │  └─ filemap_create_folio(mapping, ...)
                                                                                   │    │       // mm/filemap.c:2626
                                                                                   │    │       └─ a_ops->read_folio(file, folio)
                                                                                   │    │            // mm/filemap.c:2517
                                                                                   │    │            └─ ext4_read_folio(filp, folio)
                                                                                   │    │                 // fs/ext4/readpage.c:395
                                                                                   │    │                 └─ ext4_mpage_readpages(...)
                                                                                   │    │                      // fs/ext4/readpage.c:211
                                                                                   │    │                      ├─ ext4_map_blocks(...)
                                                                                   │    │                      │  // fs/ext4/inode.c — extent 映射
                                                                                   │    │                      ├─ bio_alloc(bdev, BIO_MAX_VECS, ...)
                                                                                   │    │                      │  // block/bio.c — 分配 BIO
                                                                                   │    │                      ├─ bio_add_folio(bio, folio, ...)
                                                                                   │    │                      │  // block/bio.c — 添加数据页
                                                                                   │    │                      ├─ bio->bi_end_io = mpage_end_io
                                                                                   │    │                      │  // 设置完成回调
                                                                                   │    │                      └─ blk_crypto_submit_bio(bio)
                                                                                   │    │                           // block/blk-crypto.c
                                                                                   │    │                           └─ submit_bio(bio)
                                                                                   │    │                                // block/blk-core.c:992
                                                                                   │    │                                └─ __submit_bio(bio)
                                                                                   │    │                                     // block/blk-core.c:636
                                                                                   │    │                                     └─ blk_mq_submit_bio(bio)
                                                                                   │    │                                          // block/blk-mq.c:3151
                                                                                   │    │                                          ├─ blk_mq_get_request(q, bio)
                                                                                   │    │                                          │  // 分配 request
                                                                                   │    │                                          ├─ blk_mq_bio_to_request(rq, bio)
                                                                                   │    │                                          │  // 绑定 bio 到 request
                                                                                   │    │                                          └─ __blk_mq_issue_directly(hctx, rq)
                                                                                   │    │                                               └─ hctx->ops->queue_rq
                                                                                   │    │                                                    └─ nvme_queue_rq(hctx, bd)
                                                                                   │    │                                                         // drivers/nvme/host/pci.c:1405
                                                                                   │    │                                                         ├─ nvme_prep_rq(req)
                                                                                   │    │                                                         │  // pci.c:1368
                                                                                   │    │                                                         │  ├─ nvme_setup_cmd(ns, req, cmd)
                                                                                   │    │                                                         │  │  // core.c:1081
                                                                                   │    │                                                         │  │  └─ nvme_setup_rw(..., nvme_cmd_read)
                                                                                   │    │                                                         │  │  // 构造 NVMe 读命令
                                                                                   │    │                                                         │  └─ nvme_map_data(req, cmd)
                                                                                   │    │                                                         │  // DMA 映射 (PRP/SGL)
                                                                                   │    │                                                         ├─ nvme_sq_copy_cmd(nvmeq, req)
                                                                                   │    │                                                         │  // pci.c:730 — memcpy 到 SQ 环
                                                                                   │    │                                                         └─ nvme_write_sq_db(nvmeq)
                                                                                   │    │                                                              // pci.c:713 — writel MMIO 门铃
                                                                                   │    │
                                                                                   │    │              /* ── NVMe 异步中断完成 ── */
                                                                                   │    │              nvme_irq(irq, nvmeq)           // pci.c:1599
                                                                                   │    │              └─ nvme_poll_cq(nvmeq, ...)   // pci.c:1578
                                                                                   │    │                   ├─ nvme_handle_cqe(nvmeq, cqe) // pci.c:1531
                                                                                   │    │                   │    ├─ nvme_find_rq(hctx, cqe)
                                                                                   │    │                   │    └─ blk_mq_add_to_batch(req)
                                                                                   │    │                   └─ nvme_ring_cq_doorbell(nvmeq)
                                                                                   │    │              └─ nvme_pci_complete_batch(breq)
                                                                                   │    │                   └─ blk_mq_end_request_batch()
                                                                                   │    │                        └─ bio_endio(bio)
                                                                                   │    │                             └─ mpage_end_io(bio)
                                                                                   │    │                                  // fs/ext4/readpage.c:167
                                                                                   │    │                                  └─ __read_end_io(folio)
                                                                                   │    │                                       └─ folio_end_read(folio)
                                                                                   │    │                                            // 标记 uptodate + 解锁
                                                                                   │    │
                                                                                   │    └─ copy_page_to_iter(folio, offset, bytes, iter)
                                                                                   │         // mm/filemap.c — 页缓存数据拷贝到用户缓冲区
                                                                                   │
                                                                                   └─ iocb->ki_pos += bytes         // 更新 kiocb 位置
                                                                                   └─ [离开循环] → 返回读取字节数

        /* ── 返回路径 ── */
        *ppos = kiocb.ki_pos                          // 回写栈上 pos 变量
        fsnotify_access(file)                         // inotify 事件通知
        add_rchar(current, ret)                       // 统计读取字节数
        inc_syscr(current)                            // 统计系统调用次数
        return ret                                    // 返回读取字节数或错误码
```

> 页缓存未命中路径（`ext4_read_folio` → BIO → NVMe → 中断完成）仅在 `filemap_get_read_batch` 未找到缓存 folio 时触发。
> DAX 和 Direct I/O 路径分别由 `ext4_dax_read_iter` 和 `ext4_dio_read_iter` 处理，不经过 filemap_read。

---

## 12 与 read/readv/preadv 体系的对比

### 12.1 四类读系统调用的对比

| 特性 | read | readv | pread64 | preadv |
|--|--|--|--|--|
| 参数 | `(fd, buf, count)` | `(fd, iovec, iovcnt)` | `(fd, buf, count, pos)` | `(fd, iovec, iovcnt, pos)` |
| 位置语义 | 使用 `file->f_pos`，**更新** | 使用 `file->f_pos`，**更新** | **参数传入**，不更新 | **参数传入**，不更新 |
| 位置参数 | 无 | 无 | `loff_t pos` | `loff_t pos` |
| FMODE 要求 | `FMODE_READ` | `FMODE_READ` | `FMODE_READ + FMODE_PREAD` | `FMODE_READ + FMODE_PREAD` |
| 缓冲区 | 单缓冲 | 多段 scatter-gather | 单缓冲 | 多段 scatter-gather |
| VFS 入口 | `vfs_read` | `vfs_readv` | `vfs_read` | `do_preadv → vfs_readv` |
| 多线程安全 | 否 | 否 | **是** | **是** |

### 12.2 位置变量的生命周期差异

```
read(fd, buf, count):
  file->f_pos = 0x100
  ksys_read → vfs_read(f, buf, count, &f->f_pos)
  → ext4 读取 0x100 处数据
  → file_pos_write(f, f->f_pos + ret)     ← 更新 f_pos
  file->f_pos = 0x200

pread64(fd, buf, count, 0x100):
  栈上局部变量: pos = 0x100 (来自参数)
  ksys_pread64 → vfs_read(f, buf, count, &pos)
  → ext4 读取 0x100 处数据
  → *ppos = kiocb.ki_pos                  ← 回写栈上变量
  → 局部变量 pos 被丢弃
  file->f_pos 不变                           ← 不影响 f_pos
```

---

## 13 关键数据结构

```c
// ========== I/O 控制块 (include/linux/fs.h) ==========

// 内核 I/O 控制块——pread64 通过此结构传递读参数
// 在 new_sync_read 中初始化，由 ext4_file_read_iter 消费
struct kiocb {
    struct file *ki_filp;            // 目标文件对象
    loff_t ki_pos;                   // 读/写偏移量（pread64 时来自栈上 pos）
    void (*ki_complete)(struct kiocb *, long);  // 异步完成回调（同步 I/O 为 NULL）
    unsigned int ki_flags;           // 标志位（IOCB_* 系列）
    unsigned short ki_ioprio;        // I/O 优先级（I/O 调度使用）
    union {
        struct kiocb_cookie *ki_cookie;  // 异步 cookie
        void *private;                   // 文件系统私有数据
    };
};

// ========== I/O 向量迭代器 (include/linux/uio.h) ==========

// 描述用户缓冲区（pread64 使用单缓冲区模式）
// ITER_DEST 表示数据方向为"写入目标"（即从内核读取到用户空间）
struct iov_iter {
    u8 iter_type;                    // 迭代器类型（ITER_UBUF = 单用户缓冲区）
    u8 data_source;                  // 数据方向（ITER_DEST = 从内核到用户）
    u8 nofault;                      // 禁止页错误标志
    u8 user_backed;                  // 是否用户空间映射
    size_t iov_offset;               // 当前 iov 内的偏移
    size_t count;                    // 剩余未传输字节数
    union {
        // ITER_UBUF 类型：单个用户缓冲区
        struct {
            void __user *ubuf;       // 用户缓冲区基地址
            // 通过 iov_iter_ubuf() 初始化
        };
        // ITER_IOVEC 类型：多段 iovec 数组（readv 场景）
        const struct iovec *iov;
        // 其他类型（BVEC, KVEC, XARRAY 等）
    };
    // 其他源信息（pipe, xarray 等）
};

// ========== 文件对象 (include/linux/fs.h) → pread64 相关字段 ==========

// 文件对象核心字段——pread64 读取时使用
// 关键区别：pread64 不修改 file->f_pos，使用栈上临时 pos
struct file {
    struct path f_path;              // 文件路径（dentry + mount）
    struct inode *f_inode;           // 指向 inode（快捷方式）
    const struct file_operations *f_op;  // 文件操作函数表（ext4 为 ext4_file_operations）
    spinlock_t f_lock;               // 保护 f_pos 的自旋锁
    atomic_long_t f_count;           // 引用计数
    unsigned int f_flags;            // 文件状态标志（O_RDONLY, O_SYNC 等）
    fmode_t f_mode;                  // 打开模式（含 FMODE_READ, FMODE_PREAD）
    loff_t f_pos;                    // 文件当前位置指针（pread64 不修改此值）
    struct address_space *f_mapping; // 页缓存映射（通常等于 inode->i_mapping）
};

// ========== folio 批量获取结果 (mm/filemap.c) ==========

// filemap_get_pages 的批量读取结果
// 用于优化：一次查找获取多个连续 folio，减少 RCU 锁操作
struct folio_batch {
    unsigned char nr;                // 本次获取的 folio 数量
    unsigned char max;               // 数组最大容量
    bool percpu_pvec_drained;        // percpu 向量是否已排空
    struct folio *folios[PAGEVEC_SIZE];  // folio 指针数组
};
```
```

---

## 14 关键优化机制

### 14.1 预读 (readahead)

`pread64` 的缺页路径会触发同步预读：

```
filemap_get_pages → 缓存未命中
  → page_cache_sync_ra(&ractl, folio->index, req_count)
       → 预测后续读取范围，批量发起异步 BIO
```

### 14.2 批量缓存查找

```
filemap_get_read_batch(mapping, index, ...)
  → 一次查找 xarray 中多个连续的 folio
  → 减少 RCU 锁竞争
```

### 14.3 NVMe 批量完成

```
nvme_poll_cq → nvme_handle_cqe → blk_mq_add_to_batch
  → 累积多个完成后调用 nvme_pci_complete_batch
  → blk_mq_end_request_batch 批量完成，减少锁开销
```

---

## 15 总结

```
                    pread64 系统调用完整数据流

用户态 pread64(fd, buf, 4K, offset=0x1000)
  │
  │  pos=0x1000 栈上局部变量（不影响 file->f_pos）
  │
  ├─ 1. SYSCALL_DEFINE4(pread64)
  │    └─ ksys_pread64
  │         └─ FMODE_PREAD 检查 → vfs_read(&pos)
  │
  ├─ 2. VFS
  │    └─ new_sync_read → init kiocb + iov_iter(ubuf)
  │         └─ kiocb.ki_pos = 0x1000
  │
  ├─ 3. ext4
  │    └─ ext4_file_read_iter → generic_file_read_iter
  │
  ├─ 4. Page Cache
  │    ├─ filemap_get_pages: 批量读取
  │    ├─ [缺页] ext4_read_folio → ext4_mpage_readpages
  │    └─ copy_page_to_iter: 数据拷贝到用户 buf
  │
  ├─ 5. Block / NVMe
  │    └─ submit_bio → nvme_queue_rq → Doorbell
  │
  ├─ 6. 中断完成
  │    └─ nvme_irq → mpage_end_io → folio_end_read
  │
  └─ 7. 返回
       ├─ file->f_pos = 0x1000 (保持不变)
       └─ 用户得到 4K 数据
```

**核心要点**：
- `pread64` 是 **定位读** 系统调用，位置由参数传入，文件指针**不更新**
- 其 VFS/文件系统/块设备/驱动路径与 `read` 完全一致
- 多线程并发 `pread64` 无需加锁——每个线程的 `pos` 在各自栈上
- ext4 无 `.read` 方法，通过 `new_sync_read` 适配到 `.read_iter` 路径
- 从 `new_sync_read` 往下的所有层（ext4/page cache/block/NVMe）与 `read`/`readv` 共享代码
