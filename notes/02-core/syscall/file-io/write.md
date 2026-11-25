# write 系统调用完整路径分析

## 1 概述

`write` 是 Linux 最基础的**文件写入**系统调用，用于将用户空间缓冲区中的数据写入文件描述符。write 支持两种写入模式：**缓冲写（Buffered Write）** 和 **直接 I/O（Direct I/O）**。

### 关键特点

- **缓冲写（Buffered Write）**：数据先写入页缓存（Page Cache），标记为脏页，由后台 writeback 机制异步刷盘
- **直接 I/O（Direct I/O）**：数据绕过页缓存，直接写入磁盘，需要用户缓冲区对齐
- **O_SYNC/O_DSYNC**：同步写入，等待数据落盘后才返回
- **延迟分配（Delayed Allocation）**：EXT4 缓冲写仅在内存中标记延迟 extent，回写时才分配物理块

### 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | ksys_write (fs/read_write.c) |
| **VFS** | vfs_write → new_sync_write (fs/read_write.c) |
| **ext4 缓冲写** | ext4_buffered_write_iter → generic_perform_write (fs/ext4/file.c, mm/filemap.c) |
| **ext4 DIO** | ext4_dio_write_iter → iomap_dio_rw (fs/ext4/file.c, fs/iomap/direct-io.c) |
| **Page Cache** | write_begin/write_end 操作 (mm/filemap.c) |
| **ext4 回写** | ext4_writepages → BIO 构造 (fs/ext4/inode.c, fs/ext4/page-io.c) |
| **Block Layer** | BIO 提交 (block/blk-core.c, blk-mq.c) |
| **NVMe 驱动** | 命令提交 + 中断完成 (drivers/nvme/host/pci.c) |

---

## 2 系统调用入口

### 2.1 SYSCALL_DEFINE3(write) - fs/read_write.c:922

```c
SYSCALL_DEFINE3(write, unsigned int, fd, const char __user *, buf,
        size_t, count)
{
    return ksys_write(fd, buf, count);
}
```

### 2.2 ksys_write - fs/read_write.c:903

```c
ssize_t ksys_write(unsigned int fd, const char __user *buf, size_t count)
{
    CLASS(fd_pos, f)(fd);         // 通过 fd 获取 struct fd（auto cleanup 管理）
    ssize_t ret = -EBADF;

    if (!fd_empty(f)) {
        loff_t pos, *ppos = file_ppos(fd_file(f));  // 获取文件位置指针
        if (ppos) {
            pos = *ppos;           // 保存当前 f_pos
            ppos = &pos;           // 使用栈上临时变量
        }
        ret = vfs_write(fd_file(f), buf, count, ppos);
        if (ret >= 0 && ppos)
            fd_file(f)->f_pos = pos;  // 更新文件位置
    }

    return ret;
}
```

**关键说明**：
- `CLASS(fd_pos, f)(fd)`：Linux 6.8+ 的 auto cleanup 宏，自动管理 fd 引用生命周期
- `file_ppos(fd_file(f))`：获取文件位置指针（常规文件返回 `&f_pos`，不可定位文件返回 NULL）
- `vfs_write` 返回后，`fd_file(f)->f_pos = pos` 更新文件偏移量

---

## 3 VFS 层：vfs_write → new_sync_write

```c
ssize_t vfs_write(struct file *file, const char __user *buf, size_t count,
          loff_t *pos)
{
    ssize_t ret;

    if (!(file->f_mode & FMODE_WRITE))
        return -EBADF;
    if (!(file->f_mode & FMODE_CAN_WRITE))
        return -EINVAL;

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    ret = rw_verify_area(WRITE, file, pos, count);  // 区域检查
    if (ret)
        return ret;
    if (count > MAX_RW_COUNT)
        count = MAX_RW_COUNT;

    file_start_write(file);                          // 写前冻结保护
    if (file->f_op->write)
        ret = file->f_op->write(file, buf, count, pos);
    else if (file->f_op->write_iter)
        ret = new_sync_write(file, buf, count, pos);  // 核心写调用
    else
        ret = -EINVAL;
    if (ret > 0) {
        fsnotify_modify(file);                        // inotify 写事件通知
        add_wchar(current, ret);                      // 统计写入字节数
    }
    inc_syscw(current);                               // 统计系统调用次数
    file_end_write(file);                             // 写后解冻
    return ret;
}
```

**vfs_write 关键流程**：
1. `FMODE_WRITE` / `FMODE_CAN_WRITE` 权限检查
2. `access_ok` 用户缓冲区地址合法性检查
3. `rw_verify_area` 区域验证（文件锁、安全钩子、边界检查）
4. `file_start_write` → `sb_start_write`：防止文件系统 freeze 并发
5. 方法选择：`f_op->write`（传统）或 `f_op->write_iter`（现代，ext4 使用）
6. `fsnotify_modify`：inotify 文件修改通知
7. `file_end_write` → `sb_end_write`：写后解冻

### new_sync_write - fs/read_write.c:686

```c
static ssize_t new_sync_write(struct file *filp, const char __user *buf,
                              size_t len, loff_t *ppos)
{
    struct kiocb kiocb;
    struct iov_iter iter;
    ssize_t ret;

    init_sync_kiocb(&kiocb, filp);                       // 初始化同步 kiocb
    kiocb.ki_pos = (ppos ? *ppos : 0);                   // 设置写入位置
    iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)buf, len);  // 初始化单段 iov_iter

    ret = filp->f_op->write_iter(&kiocb, &iter);         // → ext4_file_write_iter
    BUG_ON(ret == -EIOCBQUEUED);                         // 同步路径不应异步

    if (ret > 0 && ppos)
        *ppos = kiocb.ki_pos;                            // 更新位置
    return ret;
}
```

---

## 4 ext4_file_write_iter 路由

```
/* ========== ext4_file_write_iter 路由逻辑 ========== */
/* 根据文件打开模式和标志, 选择不同的写入路径 */

ext4_file_write_iter(iocb, from)                        // fs/ext4/file.c:844
  │
  ├─ ext4_emergency_state(inode)                         // 检查文件系统是否异常
  │
  ├─ IS_DAX(inode) → ext4_dax_write_iter(iocb, from)    // DAX 直接访问路径
  │
  ├─ IOCB_ATOMIC → 原子写入校验                          // 需要原子语义
  │
  ├─ [iocb->ki_flags & IOCB_DIRECT]                      // Direct I/O 路径
  │  │  # 跳过页缓存, 直接写磁盘
  │  │  # 要求: 用户缓冲区地址和长度对齐到扇区大小
  │  │
  │  └─ ext4_dio_write_iter(iocb, from)
  │      └─ iomap_dio_rw(iocb, from, &ext4_iomap_ops, ...)
  │         └─ __iomap_dio_rw(iocb, from, ...)
  │            ├─ iomap_iter(iomap_iter, ...)             // 循环处理每个映射
  │            │  └─ ops->iomap_begin() → ext4_iomap_begin()
  │            │     ├─ ext4_map_blocks()                  // 查找映射
  │            │     └─ [未分配] → ext4_iomap_alloc()      // 分配物理块
  │            └─ iomap_dio_iter()                         // 构造 BIO 提交
  │
  └─ [默认] → 缓冲写路径
     │  # 数据写入页缓存, 后台 writeback 异步刷盘
     │
     └─ ext4_buffered_write_iter(iocb, from)
         ├─ inode_lock(inode)                             // 获取 inode 锁
         ├─ ext4_write_checks(iocb, from)                 // 写入前检查
         └─ generic_perform_write(iocb, from)              // 核心缓冲写循环
```

---

## 5 缓冲写路径（Buffered Write）

### 5.1 generic_perform_write 核心循环

```
/* ========== generic_perform_write 核心写循环 ========== */
/* 每次循环处理一个 folio, 直到所有数据写入完毕 */

generic_perform_write(iocb, i)                             // mm/filemap.c:4374
  │
  └─ [循环: 每次处理一个 folio, 直到 iov_iter 数据耗尽]
      │
      ├─ balance_dirty_pages_ratelimited(mapping)           // 脏页限速
      │  # 如果当前进程产生的脏页比例超过阈值, 主动触发回写
      │  # 避免单个进程占用过多脏页
      │
      ├─ a_ops->write_begin(iocb, mapping, pos, bytes, &folio, &fsdata)
      │  │  # mm/filemap.c:4402 — 获取/创建 folio 并准备写入
      │  │
      │  └─ ext4_write_begin(iocb, mapping, pos, len, ...)  // fs/ext4/inode.c:1360
      │      ├─ write_begin_get_folio(mapping, pos)          // 获取或创建页缓存 folio
      │      │  # 优先在页缓存中查找, 未命中则分配新 folio
      │      │
      │      ├─ [首次写入该数据块] → __block_write_begin()
      │      │  # 创建 buffer_head, 检查是否已映射物理块
      │      │  # 未映射 → ext4_da_get_block_prep() 触发延迟分配
      │      │  #   → ext4_map_blocks() 只查找不分配
      │      │  #   → ext4_es_insert_extent() 插入 DELAYED 标记
      │      │
      │      └─ ext4_journal_start(handle, ...)              // 启动 jbd2 事务
      │
      ├─ copy_folio_from_iter_atomic(folio, offset, bytes, i)
      │  │  # mm/highmem.c — 将用户数据拷贝到 folio 页面
      │  │  # ★ 这是缓冲写路径中唯一的 CPU 数据拷贝 ★
      │  │  # 使用 kmap_local 映射 folio 页面, 然后 memcpy
      │  │
      │  └─ iov_iter 从 ITER_SOURCE 读取数据, 写入 folio 页面
      │
      └─ a_ops->write_end(iocb, mapping, pos, bytes, copied, folio, fsdata)
          │  # mm/filemap.c:4423 — 完成写入, 标记脏页并提交事务
          │
          └─ ext4_write_end(iocb, mapping, pos, len, copied, folio, ...)
              │  # fs/ext4/inode.c:1514
              │
              ├─ block_write_end(pos, len, copied, folio)     // 标记 buffer_dirty
              ├─ ext4_update_inode_size(inode, pos + copied)  // 更新 i_size
              ├─ folio_unlock(folio)                          // 解锁 folio
              ├─ folio_put(folio)                              // 释放引用
              └─ ext4_journal_stop(handle)                     // 提交 jbd2 事务
```

### 5.2 EXT4 延迟分配（Delayed Allocation）

```
/* ========== EXT4 延迟分配流程 ========== */
/* 缓冲写时, 物理块在 write_begin 阶段不分配, 仅在回写时分配 */

write_begin 阶段:
  ext4_da_write_begin()
    └─ ext4_block_write_begin()
         └─ ext4_da_get_block_prep()              // 延迟分配准备
              └─ ext4_map_blocks(inode, map, EXT4_GET_BLOCKS_DELALLOC_RESERVE)
                   # 查找逻辑块映射, 但 ★ 不分配 ★ 物理块
                   # 如果未映射, 仅做以下操作:
                   └─ ext4_es_insert_extent(inode, lblk, len, ...)
                        # 在内存 Extent Status Tree 中插入 DELAYED 标记
                        # 标记: EXT4_ES_DELAYED | EXT4_ES_UNWRITEN
                        # 物理块号填为 0 (尚未分配)

writeback 回写阶段:
  ext4_writepages(mapping, wbc)                   // 回写触发
    └─ __mpage_da_writepage(folio, wbc, mpd)
         └─ mpage_da_map_blocks(mpd)              // 真正分配物理块
              └─ ext4_map_blocks(inode, map, EXT4_GET_BLOCKS_CREATE)
                   # ★ 此时才分配物理磁盘块 ★
                   └─ ext4_ext_map_blocks(...)
                        └─ ext4_ext_insert_extent(...)  // 修改 EXT4 Extent 树
```

---

## 6 直接 I/O 路径（Direct I/O）

### 6.1 ext4_dio_write_iter 流程

```
/* ========== ext4_dio_write_iter 直接 I/O 路径 ========== */
/* 数据绕过页缓存, 直接写入磁盘, 需要用户缓冲区对齐 */

ext4_dio_write_iter(iocb, from)                               // fs/ext4/file.c
  │
  ├─ 处理 inode 锁
  │  # 取消 inode 锁 (避免与缓冲写死锁), 获取 dio 锁
  │  # DIO 路径使用 inode->i_dio_count 跟踪并发 DIO
  │
  ├─ iomap_dio_rw(iocb, from, &ext4_iomap_ops, ...)           // fs/iomap/direct-io.c
  │  │
  │  └─ __iomap_dio_rw(iocb, from, ...)
  │      │  # 初始化 iomap_dio 结构
  │      │  # 设置 iomap_dio->dops = &ext4_dio_write_ops
  │      │
  │      └─ [循环: iomap_iter()]
  │          │  # 每次迭代处理一个文件映射段
  │          │
  │          ├─ ops->iomap_begin(inode, pos, length, ...)
  │          │  │  # → ext4_iomap_begin()
  │          │  │
  │          │  ├─ ext4_map_blocks(inode, map, ...)            // 查找映射
  │          │  │
  │          │  └─ [未分配物理块]
  │          │     └─ ext4_iomap_alloc(inode, map, ...)        // 分配物理块
  │          │        └─ ext4_map_blocks(inode, map, EXT4_GET_BLOCKS_CREATE)
  │          │           └─ ext4_ext_map_blocks(...)            // 修改 Extent 树
  │          │               # 分配物理块, 标记为 unwritten
  │          │               # 在 I/O 完成时转换为 written
  │          │
  │          ├─ ext4_set_iomap(inode, iomap, map)              // 转换映射状态
  │          │  # 检查 map->m_flags:
  │          │  #   EXT4_MAP_UNWRITTEN → iomap->type = IOMAP_UNWRITTEN
  │          │  #   EXT4_MAP_MAPPED    → iomap->type = IOMAP_MAPPED
  │          │
  │          └─ iomap_dio_iter(iter, dio, iocb, iomi, ...)     // 构造并提交 BIO
  │             │
  │             ├─ bio_iov_iter_get_pages(bio, iter)           // 用户页映射到 BIO
  │             │  # 将用户空间的页面通过 GUP 锁定, 映射到 BIO
  │             │  # ★ DIO 路径: 用户数据直接 DMA 到磁盘, 无需 CPU 拷贝 ★
  │             │
  │             └─ submit_bio(bio)                             // 提交 BIO 到块层
  │
  └─ [等待 I/O 完成]
      iomap_dio_complete(dio, ret)
        ├─ [尚未分配 unwritten 块]
        │  # → ext4_end_io_end() → ext4_convert_unwritten_extents()
        │  # 将 unwritten extent 转换为 written
        │
        └─ 返回实际写入字节数
```

---

## 7 脏页回写路径（Writeback）

```
/* ========== 脏页回写路径 ========== */
/* 缓冲写仅标记脏页, 实际磁盘写入由内核 writeback 机制触发 */

/* 触发条件 */
[脏页比例超限] [后台 flusher 线程] [fsync/fdatasync] [内存压力回收]
       │
       └─ write_cache_pages(mapping, wbc, __mpage_da_writepage)
            │
            └─ ext4_writepages(mapping, wbc)                   // fs/ext4/inode.c:3089
                 │
                 ├─ [循环: write_cache_pages]
                 │    └─ 对每个脏 folio:
                 │         └─ __mpage_da_writepage(folio, wbc, mpd)
                 │              ├─ mpage_add_bh_to_extent(mpd)  // 聚合并区 extent
                 │              └─ [extent 满] → mpage_da_map_blocks(mpd)
                 │                   └─ ext4_map_blocks(inode, map, ...)
                 │                        # ★ 真正分配物理块 ★
                 │                        └─ ext4_ext_map_blocks(...)
                 │
                 └─ [收尾: mpage_da_submit_io(mpd)]
                      └─ ext4_bio_write_folio(io, folio, len)  // fs/ext4/page-io.c:458
                           ├─ io_submit_init_bio(io, bh)       // bio_alloc(REQ_OP_WRITE)
                           │    ├─ bio->bi_end_io = ext4_end_bio
                           │    └─ bio->bi_iter.bi_sector = bh->b_blocknr
                           │
                           ├─ io_submit_add_bh(io, inode, folio, ...)  // bio_add_folio
                           │
                           └─ ext4_io_submit(io)                // 提交 BIO
                                └─ blk_crypto_submit_bio(io->io_bio)
```

---

## 8 NVMe 驱动层

```
/* ========== NVMe 命令提交与完成 ========== */

/* 命令提交 */
blk_crypto_submit_bio(bio)
  └─ submit_bio(bio)                                          // block/blk-core.c
       └─ blk_mq_submit_bio(bio)                              // block/blk-mq.c
            ├─ blk_mq_get_request(hctx, bio)                   // 获取 request
            ├─ blk_mq_bio_to_request(req, bio)                 // BIO → request
            ├─ blk_add_rq_to_plug(plug, req)                   // Plug 聚合
            └─ __blk_mq_issue_directly(hctx, req)
                 └─ hctx->ops->queue_rq(hctx, bd)
                      └─ nvme_queue_rq(hctx, bd)               // drivers/nvme/host/pci.c
                           ├─ nvme_prep_rq(req)                // 准备命令
                           │    ├─ nvme_setup_cmd(ns, req, cmd)  // 设置 nvme_cmd_write
                           │    │    └─ nvme_setup_rw(ns, req, cmd, nvme_cmd_write)
                           │    └─ nvme_map_data(req, cmd)      // PRP/SGL DMA 映射
                           │         └─ dma_map_sg(bio pages → DMA 地址)
                           │
                           ├─ nvme_sq_copy_cmd(nvmeq, req)     // memcpy 到 SQ ring buffer
                           │
                           └─ nvme_write_sq_db(nvmeq)           // writel MMIO doorbell
                                # 写 SQ 门铃寄存器, 通知 NVMe 控制器有新的命令

/* 中断完成 */
nvme_irq(irq, nvmeq)                                          // drivers/nvme/host/pci.c
  └─ nvme_poll_cq(nvmeq, ...)                                 // 检查完成队列
       ├─ nvme_handle_cqe(nvmeq, cqe)                         // 处理 CQE
       │    ├─ nvme_find_rq(hctx, cqe)                        // 定位对应的 request
       │    └─ blk_mq_add_to_batch(req, ...)                   // 批量完成
       │
       └─ nvme_ring_cq_doorbell(nvmeq)                        // 写 CQ 门铃 (释放 CQ 条目)
            └─ nvme_pci_complete_batch(breq)                   // 批量完成回调
                 └─ blk_mq_end_request_batch(...)
                      └─ bio_endio(bio)
                           └─ bio->bi_end_io(bio)
                                └─ ext4_end_bio(bio)           // fs/ext4/page-io.c
                                     └─ ext4_finish_bio(bio)
                                          └─ 遍历每个 folio:
                                               ├─ folio_end_writeback(folio)
                                               │    # 清除写回标记, 解锁 folio
                                               │    # 唤醒等待该 folio 的进程
                                               │
                                               └─ ext4_io_end_callback(io_end)
                                                    # 事务提交、discard 等收尾
```

---

## 9 函数调用栈

```
/* ========== write 缓冲写路径 (Buffered Write) ========== */

SYSCALL_DEFINE3(write, fd, buf, count)                          // fs/read_write.c:922 — 系统调用入口
└─ ksys_write(fd, buf, count)                                   // fs/read_write.c:903
   ├─ CLASS(fd_pos, f)(fd)                                       // fs/file.c — 通过 fd 获取 struct fd（auto cleanup）
   ├─ [fd_empty(f)] → return -EBADF                              // 无效 fd 检查
   ├─ loff_t pos, *ppos = file_ppos(fd_file(f))                  // 获取文件位置指针
   │  └─ [ppos != NULL] → pos = *ppos; ppos = &pos              // 保存 f_pos 副本
   │
   └─ vfs_write(fd_file(f), buf, count, ppos)                    // fs/read_write.c:804 — VFS 写入口
      ├─ [!(file->f_mode & FMODE_WRITE)] → return -EBADF        // 写权限检查
      ├─ [!(file->f_mode & FMODE_CAN_WRITE)] → return -EINVAL   // 写能力检查
      ├─ rw_verify_area(WRITE, file, &pos, count)                // 区域验证
      │  └─ security_file_permission(file, MAY_WRITE)            // LSM 安全钩子检查
      ├─ file_start_write(file)                                  // fs/file_table.c — 防止文件系统 freeze 并发
      │  └─ sb_start_write(inode->i_sb)                         // 超级块级写冻结保护
      │
      └─ [f_op->write 不存在] → new_sync_write(...)              // fs/read_write.c:686 — 同步写封装
         ├─ init_sync_kiocb(&kiocb, filp)                        // include/linux/fs.h — 初始化同步 kiocb
         ├─ kiocb.ki_pos = (ppos ? *ppos : 0)                    // 设置写入位置（write 使用 f_pos）
         ├─ iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)buf, len) // lib/iov_iter.c — 初始化写迭代器
         │                                                        // ITER_SOURCE 表示数据从用户空间写入文件
         │
         └─ filp->f_op->write_iter(&kiocb, &iter)                // → ext4_file_write_iter
            │                                                     // fs/ext4/file.c:844 — ext4 写分发
            ├─ [IS_DAX(inode)] → ext4_dax_write_iter()           // DAX 直接访问路径
            ├─ [iocb->ki_flags & IOCB_ATOMIC] → ext4_atomic_write_iter() // 原子写路径
            ├─ [iocb->ki_flags & IOCB_DIRECT] → ext4_dio_write_iter()    // DirectIO 路径（绕过页缓存）
            │
            └─ ext4_buffered_write_iter(iocb, from)              // fs/ext4/file.c:348 — 缓冲写路径（默认）
               ├─ inode_lock(inode)                              // fs/inode.c — 获取 i_rwsem 互斥锁
               ├─ ext4_write_checks(iocb, from)                  // fs/ext4/file.c — 权限/大小/限额检查
               │
               └─ generic_perform_write(iocb, from)              // mm/filemap.c:4374 — 页缓存写循环
                  ├─ balance_dirty_pages_ratelimited(mapping)     // mm/page-writeback.c — 脏页限速
                  ├─ [iov_iter_count(i) == 0] → 退出循环
                  │
                  └─ [循环体: 每次写入一个 folio]
                     ├─ a_ops->write_begin                       // → ext4_da_write_begin (fs/ext4/inode.c:1360)
                     │  ├─ write_begin_get_folio(mapping, pos)    // mm/filemap.c — 获取/分配页缓存 folio
                     │  │  └─ filemap_grab_folio                 // 尝试从页缓存获取，否则分配新 folio
                     │  ├─ __block_write_begin                   // fs/ext4/inode.c — 检查 buffer_head 映射状态
                     │  └─ ext4_da_get_block_prep                  // fs/ext4/inode.c — 延迟分配预留
                     │     └─ ext4_map_blocks(create=0)           // 检查 extent 树，若未分配则插入 DELAYED 标记
                     │
                     ├─ copy_folio_from_iter_atomic(folio, offset, from) // mm/highmem.c — 用户数据 CPU 拷贝
                     │                                              // ★ 唯一 CPU 拷贝: 用户数据 → folio ★
                     │
                     └─ a_ops->write_end                         // → ext4_write_end (fs/ext4/inode.c:1514)
                        ├─ ext4_update_inode_size(inode, pos + copied) // 更新 i_size
                        ├─ block_write_end(pos, len, copied, folio)    // fs/buffer.c — 标记 buffer_dirty
                        ├─ folio_unlock(folio)                    // 解锁 folio
                        ├─ folio_put(folio)                      // 释放引用
                        └─ ext4_journal_stop(handle)             // 提交 jbd2 事务

  [完成后:]
  ├─ [ret > 0] → *ppos = kiocb.ki_pos                            // 更新 kiocb 位置
  ├─ [ret > 0] → fsnotify_modify(file)                           // inotify 文件修改事件
  ├─ inc_syscw(current)                                          // 系统调用写计数
  ├─ file_end_write(file)                                        // 写后解冻
  └─ ksys_write 返回 → fd_file(f)->f_pos = pos                   // 更新文件 f_pos

/* ========== write 直接 I/O 路径 (Direct I/O) ========== */

SYSCALL_DEFINE3(write, fd, buf, count)                           // 系统调用入口 (同上)
└─ ... ext4_file_write_iter(iocb, from)                           // fs/ext4/file.c:844
   │
   └─ [DIO] → ext4_dio_write_iter(iocb, from)                    // fs/ext4/file.c
      └─ iomap_dio_rw(iocb, from, &ext4_iomap_ops, ...)          // fs/iomap/direct-io.c
         └─ __iomap_dio_rw(iocb, from, ...)
            └─ [循环: iomap_iter()]
               ├─ ext4_iomap_begin(inode, pos, length, ...)      // 块映射
               │  ├─ ext4_map_blocks(inode, map, ...)             // 查找映射
               │  └─ [未分配] → ext4_iomap_alloc(inode, map, ...)  // 分配物理块
               │     └─ ext4_map_blocks(inode, map, EXT4_GET_BLOCKS_CREATE)
               │        └─ ext4_ext_map_blocks(...)                // 修改 Extent 树
               │
               └─ iomap_dio_iter(iter, dio, iocb, iomi, ...)     // 构造 BIO
                  └─ submit_bio(bio)                              // 提交到块层

/* ========== 异步 writeback 回写路径 ========== */

[触发条件: 脏页超限/后台 flusher/fsync/内存压力]
  └─ ext4_writepages(mapping, wbc)                               // fs/ext4/inode.c:3089
       └─ write_cache_pages(mapping, wbc, __mpage_da_writepage)  // 遍历脏页
            └─ __mpage_da_writepage(folio, wbc, mpd)             // 处理每个脏页
                 ├─ mpage_da_map_blocks(mpd)                     // 分配物理块
                 │  └─ ext4_map_blocks(inode, map, ...)           // ★ 真正分配 ★
                 │     └─ ext4_ext_map_blocks(...)
                 │
                 └─ mpage_da_submit_io(mpd)                       // 提交 BIO
                      └─ ext4_bio_write_folio(io, folio, len)     // fs/ext4/page-io.c
                           └─ ext4_io_submit(io)                  // 提交到块层
                                └─ submit_bio(bio)
```

---

## 10 关键数据结构 (C代码 + 注释)

```c
// ===== I/O 控制块 =====
// write 系统调用的核心 I/O 控制结构, 描述一次写操作的全部上下文
struct kiocb {
    struct file *ki_filp;          // 目标文件指针
    loff_t ki_pos;                 // 文件写入位置（write 从 file->f_pos 或用户指定偏移获取）
    void (*ki_complete)(struct kiocb *, long, long);  // 异步完成回调
    unsigned short ki_flags;       // IOCB_* 标志（IOCB_DIRECT 等）
    short ki_ioprio;               // I/O 优先级
};

// ===== 文件对象 =====
// write 的目标文件描述符
struct file {
    struct path f_path;            // 文件路径
    struct inode *f_inode;         // 指向 inode（ext4_inode_info 等）
    const struct file_operations *f_op;  // 文件操作函数表（write_iter 等）
    atomic_long_t f_count;         // 引用计数
    loff_t f_pos;                  // 当前读写位置（write 默认使用）
    fmode_t f_mode;                // 打开模式（FMODE_WRITE, FMODE_CAN_WRITE 等）
    unsigned int f_flags;          // 文件状态标志（O_DIRECT, O_SYNC 等）
};

// ===== I/O 迭代器 =====
// 描述待写入的数据来源, write 从 ITER_SOURCE 读取数据
struct iov_iter {
    u8 iter_type;                  // ITER_SOURCE（write 时内核从用户缓冲区读取）
    loff_t start;                  // 起始偏移
    size_t count;                  // 剩余数据量（拷贝时递减）
    union {
        const struct iovec *iov;   // iovec 数组（指向用户缓冲区）
        struct {
            void __user *ubuf;     // 用户缓冲区基地址（单段模式）
            size_t len;            // 缓冲区长度
        };
    };
    unsigned long nr_segs;         // 分段数
    // write 使用: iov_iter_ubuf(&iter, ITER_SOURCE, buf, len)
    // copy_folio_from_iter_atomic 从 iov_iter 读取数据写入 folio
};

// ===== folio——页缓存单元 =====
// 缓冲写的核心载体, 数据先写入 folio, 再标记为脏页由 writeback 刷盘
struct folio {
    unsigned long flags;           // PG_dirty（脏页）、PG_locked（锁定）、PG_uptodate 等
    struct address_space *mapping; // 所属的 address_space（页缓存索引, 通过 mapping->host 获取 inode）
    loff_t index;                  // 文件内页索引（pos >> PAGE_SHIFT）
    atomic_t _refcount;            // 引用计数
    // write 流程:
    //   write_begin → 获取/创建 folio
    //   copy_folio_from_iter_atomic → 写入用户数据
    //   write_end → 标记 folio 为 dirty
    //   writeback → folio_end_writeback → 清除 dirty 标记
};

// ===== buffer_head =====
// 块映射描述符, 用于 folio 与磁盘块之间的映射关系
// 延迟分配时, buffer_head 的 BH_Delay 标志表示尚未分配物理块
struct buffer_head {
    struct page *b_page;           // 所属页面
    sector_t b_blocknr;            // 磁盘块号（延迟分配时为 0）
    size_t b_size;                 // 块大小
    struct block_device *b_bdev;   // 块设备
    unsigned long b_state;         // BH_Uptodate, BH_Dirty, BH_Delay 等标志
    // BH_Delay: 延迟分配标记, 表示逻辑块已预留但未分配物理块
    // BH_Mapped: 已映射到物理块
};

// ===== EXT4 iomap 操作结构 =====
// DIO 路径中, 描述 ext4 文件的逻辑块到磁盘物理块的映射
struct iomap {
    u64 addr;                      // 物理地址（磁盘块号 << 块大小偏移）
    loff_t offset;                 // 文件内偏移
    u64 length;                    // 映射长度
    u16 type;                      // IOMAP_MAPPED / IOMAP_UNWRITTEN / IOMAP_DELALLOC
    u16 flags;                     // IOMAP_F_* 标志
    struct block_device *bdev;     // 块设备
    // DIO 写: 新分配的块类型为 IOMAP_UNWRITTEN
    // I/O 完成时通过 ext4_end_io_end 转换为 written
};

// ===== DIO 描述结构 =====
// Direct I/O 操作的完整上下文
struct iomap_dio {
    struct kiocb *iocb;            // 关联的 kiocb
    struct iov_iter *iter;         // 数据迭代器
    const struct iomap_dio_ops *dops;  // DIO 操作回调（ext4_dio_write_ops）
    size_t size;                   // 总大小
    loff_t i_size;                 // 文件大小
    int error;                     // 错误码
};

// ===== BIO——块 I/O 请求 =====
// 描述一次块设备 I/O 操作, 包含数据页面的 DMA 映射
struct bio {
    struct block_device *bi_bdev;  // 目标块设备
    sector_t bi_iter.bi_sector;    // 起始扇区号
    unsigned int bi_opf;           // 操作标志（REQ_OP_WRITE 等）
    bio_end_io_t *bi_end_io;       // I/O 完成回调（ext4_end_bio）
    struct bio_vec *bi_io_vec;     // 数据页面数组（bio_vec 数组）
    // DIO 路径: bi_io_vec 指向用户页面（通过 bio_iov_iter_get_pages 获取）
    // 缓冲写路径: bi_io_vec 指向页缓存 folio 页面
};
```

| 数据结构 | 头文件 | 在 write 中的作用 |
|----------|--------|------------------|
| `struct kiocb` | `include/linux/fs.h` | I/O 控制块, 描述写操作上下文 |
| `struct file` | `include/linux/fs.h` | 目标文件描述符 |
| `struct iov_iter` | `include/linux/uio.h` | 用户数据来源迭代器 |
| `struct folio` | `include/linux/mm_types.h` | 页缓存单元, 缓冲写的数据载体 |
| `struct buffer_head` | `include/linux/buffer_head.h` | 块映射描述符, 延迟分配标记 |
| `struct iomap` | `include/linux/iomap.h` | DIO 路径的块映射描述 |
| `struct iomap_dio` | `include/linux/iomap.h` | DIO 操作上下文 |
| `struct bio` | `include/linux/blk_types.h` | 块 I/O 请求 |

---

## 11 总结

write 系统调用提供了两种写入路径：

1. **缓冲写（Buffered Write）**：数据先写入页缓存 folio，通过 `copy_folio_from_iter_atomic` 进行一次 CPU 拷贝，标记为脏页后返回。实际的磁盘 I/O 由后台 writeback 机制异步触发。EXT4 采用延迟分配策略，仅在 writeback 时分配物理块。

2. **直接 I/O（Direct I/O）**：数据绕过页缓存，通过 `iomap_dio_rw` 直接构造 BIO 提交到块层。用户页面通过 `bio_iov_iter_get_pages` 直接映射到 BIO，**无需 CPU 拷贝**。新分配的块标记为 unwritten，I/O 完成时转换为 written。

3. **性能关键点**：
   - 缓冲写：延迟分配减少磁盘碎片，但 writeback 刷盘时机不可控
   - 直接 I/O：需要用户缓冲区对齐，适合大块顺序写入
   - O_SYNC：缓冲写 + 同步等待 writeback 完成