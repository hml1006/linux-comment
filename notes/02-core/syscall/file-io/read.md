# Linux `read` 系统调用完整流程解析：从 VFS 到 NVMe 驱动

本文将从 `/home/louis/code/linux/fs/read_write.c` 中的 `ksys_read` 函数开始，详细梳理一个 `read` 系统调用如何穿透 VFS 层、Ext4 文件系统层、Block 块设备层，最终到达 NVMe 驱动层发送命令，并在数据读取完成后返回给用户进程的完整生命周期。

---

## 一、 整体架构流程图

```
                    read(fd, buf, count)
                           |
                    +------v------+
                    | SYSCALL_    |  系统调用入口
                    | DEFINE3(read)|  (fs/read_write.c)
                    +------+------+
                           |
                    +------v------+
                    | ksys_read   |  -- fdget_pos 获取 struct fd
                    | (fs/read_   |  -- file_pos_read 读取 f_pos
                    |  write.c)   |  -- vfs_read 进入 VFS 层
                    +------+------+
                           |
                    +------v------+
                    | vfs_read    |  -- FMODE_READ 检查
                    | (fs/read_   |  -- rw_verify_area 区域验证
                    |  write.c)   |  -- new_sync_read 分发
                    +------+------+
                           |
                    +------v------+
                    | new_sync_   |  -- init_sync_kiocb
                    | read        |  -- kiocb.ki_pos = *ppos
                    | (fs/read_   |  -- iov_iter_init(ITER_DEST)
                    |  write.c)   |  -- f_op->read_iter
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
       | → ext4_dio_  |   | generic_file_|
       |   read_iter  |   | read_iter    |
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
             | (XArray 查找) |  | head        |
             | 数据已就绪    |   | (ra 预读)   |
             +------+------+   +------+------+
                    |                  |
             +------v------+   +------v------+
             | copy_page_  |   | filemap_    |
             | to_iter     |   | create_folio|
             | (数据拷贝到  |   | (创建新 folio|
             |  用户 buf)   |   |  并锁住)    |
             +------+------+   +------+------+
                    |                  |
             +------v------+   +------v------+
             | 返回读取字节数|   | filemap_read|
             | (不需要 I/O)  |   | _folio      |
             +------+------+   | → a_ops->read|
                    |          |   _folio     |
                    |          +------+------+
                    |                 |
                    |          +------v------+
                    |          | ext4_read_   |
                    |          | folio        |
                    |          | (fs/ext4/    |
                    |          |  readpage.c  |
                    |          |  395)        |
                    |          +------+------+
                    |                 |
                    |          +------v------+
                    |          | ext4_mpage_  |
                    |          | readpages    |
                    |          | (fs/ext4/    |
                    |          |  readpage.c  |
                    |          |  211)        |
                    |          +------+------+
                    |                 |
                    |     +-----v-----+------+
                    |     | ext4_map_blocks |
                    |     | → 逻辑块→物理扇区|
                    |     +------+------+----+
                    |            |
                    |     +------v------+
                    |     | bio_alloc    |
                    |     | (REQ_OP_READ)|
                    |     | bio_add_folio|
                    |     | bio->bi_end_ |
                    |     | io = mpage_  |
                    |     | end_io       |
                    |     +------+------+
                    |            |
                    |     +------v------+
                    |     | submit_bio   |
                    |     +------+------+
                    |            |
                    |     +------v------+
                    |     | blk_mq_     |
                    |     | submit_bio  |
                    |     | → bio_split |
                    |     | → bio_merge |
                    |     | → alloc_    |
                    |     |   request   |
                    |     | → bio_to_   |
                    |     |   request   |
                    |     +------+------+
                    |            |
                    |     +------v------+
                    |     | nvme_queue_ |
                    |     | rq          |
                    |     | (drivers/   |
                    |     |  nvme/host/ |
                    |     |  pci.c)     |
                    |     +------+------+
                    |            |
                    |     +------v------+
                    |     | nvme_prep_rq|
                    |     | → nvme_setup|
                    |     |   _cmd(nvme_|
                    |     |   cmd_read) |
                    |     | → nvme_map_ |
                    |     |   data(PRP) |
                    |     +------+------+
                    |            |
                    |     +------v------+
                    |     | nvme_sq_    |
                    |     | copy_cmd    |
                    |     | (memcpy 到  |
                    |     |  SQ 环)     |
                    |     +------+------+
                    |            |
                    |     +------v------+
                    |     | nvme_write_ |
                    |     | sq_db       |
                    |     | (writel MMIO)|
                    |     +------+------+
                    |            |
                    |     +------v------+
                    |     | [NVMe 控制器]|
                    |     | DMA 读取     |
                    |     | → 写 CQE     |
                    |     | → 触发 MSI-X |
                    |     +------+------+
                    |            |
                    |     +------v------+
                    |     | nvme_irq    |
                    |     | → nvme_     |
                    |     |   poll_cq   |
                    |     | → nvme_     |
                    |     |   handle_cqe|
                    |     | → blk_mq_   |
                    |     |   end_      |
                    |     |   request   |
                    |     +------+------+
                    |            |
                    |     +------v------+
                    |     | bio_endio   |
                    |     | → mpage_end_|
                    |     |   io        |
                    |     | → folio_end_|
                    |     |   read      |
                    |     | → folio_    |
                    |     |   unlock    |
                    |     +------+------+
                    |            |
                    +--v----+----v--+
                    |             |
             +------v------+     |
             | copy_page_  |<----+
             | to_iter     |
             | (页缓存→用户 |
             |  buf 拷贝)   |
             +------+------+
                    |
             +------v------+
             | file_pos_   |
             | write(f.file,|
             | pos)        |
             | (更新 f_pos) |
             +------+------+
                    |
             +------v------+
             | 返回读取字节数|
             +-------------+
```

---

## 二、 详细流程梳理

### 1. 系统调用层与 VFS 层入口

当用户态进程调用 `read(fd, buf, count)` 时，通过软中断（如 `int 0x80` 或 `syscall` 指令）进入内核，最终调用到 `ksys_read`。

**代码路径：`/home/louis/code/linux/fs/read_write.c`**

```c
ssize_t ksys_read(unsigned int fd, char __user *buf, size_t count)
{
    struct fd f = fdget_pos(fd);
    ssize_t ret = -EBADF;

    if (f.file) {
        loff_t pos = file_pos_read(f.file);
        ret = vfs_read(f.file, buf, count, &pos);
        if (ret >= 0)
            file_pos_write(f.file, pos);
        fdput_pos(f);
    }
    return ret;
}
```

**VFS 层处理 (`vfs_read`)：**
1. 通过 `fdget_pos` 获取当前进程的 `fdtable`，进而找到对应的 `struct file` 结构体。
2. 检查文件模式、权限（如 `FMODE_READ`）。
3. 调用 `vfs_read`，如果 `file->f_op->read` 存在（老式驱动），则调用它；否则调用 `new_sync_read`，最终调用 `file->f_op->read_iter`。现代文件系统（包括 ext4）均采用 `read_iter` 接口。

---

### 2. VFS 到 Ext4 文件系统层

Ext4 在注册时，会将 `ext4_file_operations` 赋给 `file->f_op`。

**代码路径：`fs/ext4/file.c`**
```c
const struct file_operations ext4_file_operations = {
    .read_iter  = ext4_file_read_iter,
    // ...
};
```

**流程：**
1. `vfs_read` 调用 `ext4_file_read_iter`。
2. `ext4_file_read_iter` 进行一些 ext4 特有的检查（如是否加密文件、是否为 inline data），然后调用 VFS 的通用读函数 `generic_file_read_iter`。
3. `generic_file_read_iter` 最终调用 `generic_file_buffered_read`，这是页缓存的通用处理逻辑。

---

### 3. 页缓存 与 Ext4 的交互

`generic_file_buffered_read` 的核心逻辑是查找 Page Cache：
- **命中**：如果数据已经在内存中，直接通过 `copy_page_to_iter` 将数据拷贝到用户态的 `buf` 中，流程结束。
- **未命中**：分配新的 Page，并调用 `mapping->a_ops->readpage` 或 `readpages` 从磁盘读取数据。

Ext4 的地址空间操作集为 `ext4_aops`：
**代码路径：`fs/ext4/inode.c`**
```c
static const struct address_space_operations ext4_aops = {
    .readpage       = ext4_readpage,
    .readpages      = ext4_readpages,
    // ...
};
```

**Ext4 构建读请求：**
1. VFS 调用 `ext4_readpages`（现代内核倾向于调用 `readpages` 批量读）。
2. `ext4_readpages` 调用 `ext4_mpage_readpages`。
3. `ext4_mpage_readpages` 的核心工作是：将逻辑文件偏移量转换为磁盘的物理块号，并构建内核的 I/O 请求结构体 `struct bio`。

---

### 4. Block 块设备层

Ext4 将构建好的 `bio` 提交给 Block 层。

**代码调用链：**
`ext4_mpage_readpages` -> `submit_bio` -> `submit_bio_noacct` -> `blk_mq_submit_bio`

**Block 层的职责：**
1. **合并与排序**：将 `bio` 与电梯队列中的请求合并（如果物理地址相邻）。
2. **分配请求**：将 `bio` 封装为 `struct request`（代表一个具体的块设备指令）。
3. **软件队列排队**：将 `request` 放入对应的软件队列（硬件上下文）。
4. **硬件队列映射**：通过哈希映射，将请求导向特定的 NVMe 硬件队列 `struct blk_mq_hw_ctx`。
5. **触发执行**：调用 `blk_mq_run_hw_queue`，最终调用 `q->mq_ops->queue_rq`，即 NVMe 驱动注册的回调函数。

---

### 5. NVMe 驱动层与命令发送

NVMe 驱动在初始化时注册了 `nvme_mq_ops`：
**代码路径：`drivers/nvme/host/pci.c` (或 core.c)**
```c
static const struct blk_mq_ops nvme_mq_ops = {
    .queue_rq     = nvme_queue_rq,
    // ...
};
```

**NVMe 发送命令过程：**
1. Block 层调用 `nvme_queue_rq`。
2. 驱动从 NVMe 的 Submission Queue (SQ) 中获取一个空槽位。
3. 驱动将 `request` 转换为 NVMe 规范定义的 `struct nvme_command`（填写 Opcode 为 `nvme_cmd_read`，PRP1/PRP2 指向物理内存地址等）。
4. 将命令写入 SQ，并更新 SQ 的尾指针（SQ Tail Doorbell 寄存器），**这一步触发了硬件开始工作**。

---

### 6. 硬件执行与中断返回

**数据读取与中断：**
1. NVMe 控制器看到 SQ 有新命令，通过 DMA 引擎直接将磁盘数据读取到命令中 PRP 指向的物理内存（即 Page Cache 对应的物理页）。
2. 读取完成后，控制器将完成信息写入 Completion Queue (CQ)，并触发 MSI-X 中断。

**中断处理与数据返回：**
1. CPU 响应中断，进入 `nvme_irq` -> `nvme_complete_rq`。
2. 驱动从 CQ 中获取完成状态，通过 `blk_mq_complete_request` 将完成事件上报给 Block 层。
3. Block 层调用 `bio_endio`，唤醒之前阻塞在等待 Page Cache I/O 完成的进程（在 `generic_file_buffered_read` 中等待的 `wait_on_page_locked_killable`）。
4. 此时，数据已经从 NVMe 磁盘通过 DMA 拷贝到了 Page Cache 中。

---

### 7. 数据拷贝回用户空间与系统调用返回

1. Page Cache 的 I/O 状态被标记为 Uptodate（数据有效）且解锁。
2. 进程被唤醒，从睡眠中恢复执行。
3. `generic_file_buffered_read` 继续执行 `copy_page_to_iter`，通过 CPU 指令将 Page Cache 中的数据**拷贝到用户态的 `buf` 中**。
4. 更新 `struct file` 中的文件偏移量 `f_pos`。
5. 沿着调用栈原路返回：`ext4_file_read_iter` -> `vfs_read` -> `ksys_read`。
6. `ksys_read` 将读取的字节数作为返回值，通过系统调用出口返回给用户态进程。

至此，一次完整的 `read` 系统调用流程结束。

---

## 三、 函数调用栈

### 3.1 read 系统调用——缓冲读路径（页缓存未命中）

```
SYSCALL_DEFINE3(read, fd, buf, count)                       // fs/read_write.c:1370 — 系统调用入口
└─ ksys_read(fd, buf, count)                                 // fs/read_write.c:1377 — 核心实现
   ├─ fdget_pos(fd)                                           // fs/file.c:1010 — 通过 fd 获取 struct fd
   ├─ file_pos_read(f.file)                                   // 读取当前文件偏移 f_pos
   ├─ vfs_read(f.file, buf, count, &pos)                      // fs/read_write.c:669 — VFS 读入口
   │  ├─ rw_verify_area(READ, file, &pos, count)              // fs/read_write.c:420 — 区域验证（锁/安全）
   │  │  └─ security_file_permission(file, MAY_READ)          // 安全模块检查（SELinux/AppArmor）
   │  │
   │  └─ new_sync_read(file, buf, count, pos)                 // fs/read_write.c:643 — 同步读分发
   │     ├─ init_sync_kiocb(&kiocb, file)                     // 初始化 kiocb（设置 ki_filp）
   │     ├─ kiocb.ki_pos = *ppos                              // 设置 I/O 位置（栈变量 pos）
   │     ├─ iov_iter_init(&iter, ITER_DEST, buf, count)       // 初始化 iov_iter（单段读方向）
   │     │  └─ iter.iter_type = ITER_IOVEC                    // 单段 = 1 个 iovec
   │     │     iter.data_source = ITER_DEST                   // 读方向（数据从文件→用户 buf）
   │     │     iter.count = count
   │     │
   │     └─ ret = file->f_op->read_iter(&kiocb, &iter)        // → ext4_file_read_iter
   │        │
   │        └─ ext4_file_read_iter(iocb, iter)                 // fs/ext4/file.c:186 — ext4 分发
   │           ├─ [IOCB_DIRECT] → ext4_dio_read_iter(iocb, iter)   // 直接 I/O 路径（跳过页缓存）
   │           │
   │           └─ generic_file_read_iter(iocb, iter)           // mm/filemap.c:2620 — 页缓存读路径
   │              └─ filemap_read(iocb, iter, retval)          // mm/filemap.c:2620 — 页缓存读核心
   │                 │
   │                 │ [循环: 每次读取一个 folio 大小的数据]
   │                 │
   │                 ├─ filemap_get_pages(iocb, iter, ...)     // mm/filemap.c:2400 — 获取页
   │                 │  ├─ filemap_get_read_batch(mapping, ...)  // XArray 查找页缓存 folio 批量
   │                 │  │  └─ xa_load(&mapping->i_pages, index)  // 基数树查找
   │                 │  │
   │                 │  ├─ [页缓存未命中] → 进入预读路径
   │                 │  │  ├─ page_cache_sync_readahead(...)    // mm/readahead.c:630 — 同步预读
   │                 │  │  │  └─ ra_alloc_folio(...)            // 分配 folio 并加入页缓存
   │                 │  │  │
   │                 │  │  └─ filemap_create_folio(mapping, index)  // mm/filemap.c:2300 — 创建新 folio
   │                 │  │     └─ filemap_alloc_folio(...)          // 分配物理页
   │                 │  │        └─ folio = page_cache_alloc()     // 伙伴系统分配
   │                 │  │
   │                 │  └─ [页缓存命中] → 检查 folio 状态
   │                 │     └─ folio_test_uptodate(folio)          // 检查数据是否有效
   │                 │        ├─ [是] → 直接返回 folio（数据就绪）
   │                 │        └─ [否] → folio 需从磁盘读取
   │                 │
   │                 ├─ [数据已在页缓存中] → 直接拷贝
   │                 │  └─ copy_page_to_iter(folio, offset, ...)  // mm/filemap.c:2700 — 页缓存→用户空间
   │                 │     └─ copy_user_highpage()                 // 内核→用户空间拷贝指令
   │                 │
   │                 ├─ [数据不在页缓存中 或 folio 未就绪] → 触发 I/O
   │                 │  └─ filemap_read_folio(file, ...)          // mm/filemap.c:2350 — 触发磁盘 I/O
   │                 │     └─ mapping->a_ops->read_folio(file, folio)  // → ext4_read_folio
   │                 │        │
   │                 │        └─ ext4_read_folio(file, folio)      // fs/ext4/readpage.c:395 — ext4 读 folio
   │                 │           └─ ext4_mpage_readpages(mapping, ..)  // fs/ext4/readpage.c:211
   │                 │              ├─ ext4_map_blocks(inode, &map, ...)  // fs/ext4/inode.c:600
   │                 │              │  → 逻辑块号 → 物理块号映射（ext4 extent tree 查找）
   │                 │              │
   │                 │              ├─ bio_alloc(bdev, nr_vecs, REQ_OP_READ, ...)  // 分配 bio
   │                 │              ├─ bio_add_folio(bio, folio, ...)              // folio 加入 bio
   │                 │              ├─ bio->bi_end_io = mpage_end_io               // 设置完成回调
   │                 │              │
   │                 │              └─ submit_bio(bio)                             // 提交到块层
   │                 │                 └─ submit_bio_noacct(bio)                    // block/blk-core.c
   │                 │                    └─ __submit_bio(bio)                      // 块层入口
   │                 │                       └─ blk_mq_submit_bio(bio)             // block/blk-mq.c:2200
   │                 │                          ├─ bio_split_to_limits(bio, ...)   // 拆分超限 bio
   │                 │                          ├─ blk_mq_attempt_bio_merge(...)   // 尝试合并
   │                 │                          ├─ blk_mq_get_new_requests(...)     // 分配 request
   │                 │                          ├─ blk_mq_bio_to_request(...)       // bio→request 绑定
   │                 │                          ├─ blk_add_rq_to_plug(rq)           // plug 批处理
   │                 │                          └─ blk_finish_plug(...)             // 刷新 plug 列表
   │                 │                             └─ blk_mq_dispatch_plug_list(...)
   │                 │                                └─ q->mq_ops->queue_rq(...)   // → nvme_queue_rq
   │                 │                                   │
   │                 │                                   └─ nvme_queue_rq(hctx, bd, ...)  // drivers/nvme/host/pci.c
   │                 │                                      ├─ nvme_prep_rq(dev, req)        // 准备命令
   │                 │                                      │  ├─ nvme_setup_cmd(req, cmd)    // 构造 NVMe 命令
   │                 │                                      │  │  └─ cmd->opcode = nvme_cmd_read (0x02)
   │                 │                                      │  │     cmd->nsid = nsid
   │                 │                                      │  │     cmd->slba = 起始 LBA
   │                 │                                      │  │     cmd->length = 块数 - 1
   │                 │                                      │  │
   │                 │                                      │  └─ nvme_map_data(dev, req, ...)  // DMA 地址映射
   │                 │                                      │     └─ dma_map_sg(dev, ...)       // PRP/SGL 表
   │                 │                                      │
   │                 │                                      ├─ nvme_sq_copy_cmd(nvmeq, cmd)   // memcpy 到 SQ 环
   │                 │                                      │
   │                 │                                      └─ nvme_write_sq_db(nvmeq)        // writel MMIO 门铃
   │                 │                                         └─ writel(tail, doorbell_addr)   // 通知硬件取命令
   │                 │
   │                 │  [等待 I/O 完成]
   │                 │  ├─ folio_wait_bit(folio_bit)          // 等待 folio 解锁
   │                 │  │
   │                 │  └─ [NVMe 中断处理]
   │                 │     └─ nvme_irq(irq, dev)              // drivers/nvme/host/pci.c
   │                 │        └─ nvme_poll_cq(nvmeq)          // 轮询完成队列
   │                 │           ├─ nvme_cqe_pending(nvmeq)   // 检查阶段位 (phase bit)
   │                 │           │  └─ dma_rmb()              // 读内存屏障（DMA 一致性）
   │                 │           │
   │                 │           ├─ nvme_handle_cqe(nvmeq, cqe)  // 处理完成条目
   │                 │           │  ├─ nvme_find_rq(nvmeq, cqe)  // command_id → request
   │                 │           │  └─ nvme_try_complete_req(...) // 完成请求
   │                 │           │     └─ blk_mq_complete_request_remote(req)  // 上报块层
   │                 │           │
   │                 │           └─ nvme_ring_cq_doorbell(nvmeq)  // 释放 CQ 槽位
   │                 │
   │                 │  [完成回调链]
   │                 │  └─ blk_update_request(req, ...)         // block/blk-mq.c
   │                 │     └─ bio->bi_end_io(bio)               // → mpage_end_io
   │                 │        └─ __read_end_io(folio, error)    // mm/filemap.c
   │                 │           └─ folio_end_read(folio, true) // 标记 uptodate + 解锁
   │                 │              └─ folio_unlock(folio)      // 唤醒等待进程
   │                 │
   │                 └─ [I/O 完成，folio 数据就绪]
   │                    └─ copy_page_to_iter(folio, offset, ...)  // 页缓存→用户空间拷贝
   │                       └─ copy_page_to_iter_nofault()         // 内核拷贝到用户 buf
   │
   ├─ [ret >= 0] → file_pos_write(f.file, pos)               // 更新文件偏移 f_pos
   │
   └─ fdput_pos(f)                                           // 释放 fd 引用
```

### 3.2 关键条件分支说明

```
[页缓存命中]
  read(fd, buf, count)
  → ksys_read → vfs_read → new_sync_read
  → ext4_file_read_iter → generic_file_read_iter → filemap_read
  → filemap_get_pages → filemap_get_read_batch (XArray 命中)
  → copy_page_to_iter (直接拷贝，无磁盘 I/O)
  → 返回读取字节数

[页缓存未命中]
  read(fd, buf, count)
  → ksys_read → vfs_read → new_sync_read
  → ext4_file_read_iter → generic_file_read_iter → filemap_read
  → filemap_get_pages → filemap_create_folio (分配新页)
  → filemap_read_folio → ext4_read_folio
  → ext4_mpage_readpages → submit_bio → blk_mq_submit_bio
  → nvme_queue_rq → nvme_write_sq_db → [DMA 磁盘读取]
  → [MSI-X 中断] → nvme_irq → bio_endio → folio_end_read → folio_unlock
  → copy_page_to_iter (数据拷贝到用户空间)
  → 返回读取字节数

[直接 I/O 路径 (O_DIRECT)]
  read(fd, buf, count)
  → ksys_read → vfs_read → new_sync_read
  → ext4_file_read_iter → ext4_dio_read_iter
  → iomap_dio_rw (绕过页缓存，直接磁盘 I/O)
  → submit_bio → blk_mq_submit_bio → nvme_queue_rq
  → [DMA 读取直接到用户缓冲区]
  → 完成回调 → 返回读取字节数
```

---

## 四、 关键数据结构 (C代码 + 注释)

```c
// ===== VFS 层 =====

// 用户空间缓冲区描述——read 系统调用将数据从文件读取到用户 buf
struct iovec {
    void __user *iov_base;   // 用户空间缓冲区基地址（read 的数据写入目标）
    size_t       iov_len;    // 缓冲区长度（read 期望读取的字节数）
};

// 单段缓冲区迭代器——read 使用 ITER_DEST 表示数据从文件流向用户空间
// read 系统调用只有一个 buf，iov_iter 退化为单段（iter_type = ITER_UBUF 或 nr_segs=1）
struct iov_iter {
    u8 iter_type;            // 迭代器类型：ITER_UBUF（单段）/ ITER_IOVEC（多段，readv 使用）
    u8 data_source;          // 数据方向：ITER_DEST（读方向，数据从文件→用户 buf）
    size_t iov_offset;       // 当前 iovec 段内偏移（跨段续传时使用）
    size_t count;            // 剩余未传输字节总数
    union {
        const struct iovec *iov;       // 指向 iovec 数组（多段模式）
        struct {
            void __user *ubuf;         // 用户缓冲区基地址（单段模式，read 常用）
            size_t len;                // 缓冲区长度
        };
    };
    unsigned long nr_segs;   // iovec 段数（read 时为 1）
};

// I/O 控制块——在 read 调用中携带 I/O 上下文
// new_sync_read 中 init_sync_kiocb 初始化，ki_pos 从 file->f_pos 拷贝
struct kiocb {
    struct file      *ki_filp;       // 目标文件对象（通过 fget 或 fdget 获得）
    loff_t            ki_pos;        // 读取位置（从 file->f_pos 拷贝，I/O 完成后更新 f_pos）
    unsigned short    ki_opcode;     // I/O 操作码（针对特定设备）
    unsigned short    ki_flags;      // I/O 标志：IOCB_DIRECT（直接 I/O，绕过页缓存）、IOCB_NOWAIT（非阻塞）等
    short             ki_ioprio;     // I/O 优先级
    void              *private;      // 文件系统私有数据
    union {
        void          (*ki_complete)(struct kiocb *iocb, long ret);
        // 异步 I/O 完成回调（同步 read 时为 NULL）
    };
};

// 文件对象——进程打开文件的抽象，read 操作的核心上下文
struct file {
    struct path             f_path;          // 文件的 dentry + mount 路径
    struct inode            *f_inode;        // 指向 inode（文件元数据）
    const struct file_operations *f_op;      // 文件操作集（ext4 → ext4_file_operations）
    loff_t                  f_pos;           // 当前文件读写偏移（read 后更新）
    unsigned int            f_flags;         // 文件打开标志（O_RDONLY / O_DIRECT / O_SYNC 等）
    fmode_t                 f_mode;          // 文件模式（FMODE_READ / FMODE_WRITE）
    struct address_space    *f_mapping;      // 页缓存地址空间（读操作的数据来源）
};

// 页缓存 folio——read 的页缓存核心单元
// 页缓存命中时直接读取，未命中时从磁盘读取后填充
struct folio {
    unsigned long flags;     // folio 标志：PG_locked（锁定）、PG_uptodate（数据有效）、PG_readahead（预读标记）等
    struct address_space *mapping;  // 所属 address_space（通过 mapping->i_pages XArray 索引）
    loff_t index;            // 在文件内的页索引（pos >> PAGE_SHIFT）
    void *private;           // 文件系统私有数据（ext4 的 buffer_head 链表）
    atomic_t _mapcount;      // 映射计数
    atomic_t _refcount;      // 引用计数（页缓存引用 + 进程映射）
};

// 地址空间——管理文件页缓存，连接 inode 和物理内存页
struct address_space {
    struct inode            *host;           // 所属 inode
    struct xarray           i_pages;         // 页缓存 XArray 树（folio 索引）
    unsigned long           nrpages;         // 页缓存中 folio 总数
    const struct address_space_operations *a_ops;  // 地址空间操作集（ext4 → ext4_aops）
};

// ===== Ext4 文件系统层 =====

// Ext4 块映射结果——read_folio 将逻辑块号映射到物理扇区
struct ext4_map_blocks {
    ext4_fsblk_t  m_pblk;       // 映射后的物理块号（磁盘扇区号）
    ext4_lblk_t   m_lblk;       // 原始逻辑块号（文件偏移 >> 块大小）
    unsigned int  m_len;        // 连续块数
    unsigned int  m_flags;      // 映射标志：EXT4_MAP_NEW（新分配）、EXT4_MAP_MAPPED（已映射）等
};

// ===== 块层 =====

// 块 I/O 请求——read 的磁盘 I/O 请求载体
struct bio {
    struct bio          *bi_next;         // bio 链表（plug 聚合时使用）
    struct block_device *bi_bdev;         // 目标块设备
    blk_opf_t            bi_opf;          // 操作标志：REQ_OP_READ（read 操作）
    unsigned short       bi_flags;        // bio 标志：BIO_PAGE_REFFED（页引用）、BIO_CLONED（克隆）等
    unsigned short       bi_ioprio;       // I/O 优先级
    struct bio_vec       *bi_io_vec;      // 数据段数组（bio_add_folio 填充）
    unsigned int         bi_vcnt;         // bio_vec 段数
    struct bvec_iter     bi_iter;         // 当前迭代位置（bi_sector 为起始扇区）
    bio_end_io_t         *bi_end_io;      // 完成回调：read → mpage_end_io
    void                 *bi_private;     // 私有数据
};

// 块设备 I/O 请求——块层调度的基本单元
struct request {
    struct request_queue    *q;            // 所属请求队列
    struct bio              *bio;          // 关联的 bio 链表
    struct blk_mq_ctx       *mq_ctx;       // MQ 软件上下文
    struct blk_mq_hw_ctx    *mq_hctx;      // MQ 硬件上下文
    unsigned int            cmd_flags;     // 命令标志：REQ_OP_READ
    sector_t                __sector;      // 起始扇区号
    struct gendisk          *rq_disk;      // 目标磁盘
};

// ===== NVMe 驱动层 =====

// NVMe 命令结构——提交到 NVMe 控制器的读命令格式
struct nvme_command {
    struct {
        u8  opcode;          // 操作码：nvme_cmd_read = 0x02（读命令）
        u8  flags;           // 命令标志
        u16 command_id;      // 命令 ID（唯一标识，用于 CQE 匹配）
    };
    __le32 nsid;             // 命名空间 ID（NVMe 多命名空间支持）
    __le64 prp1;             // PRP1 物理区域指针（DMA 目标地址，指向数据缓冲区页）
    __le64 prp2;             // PRP2（跨页时指向下一页或 PRP List）
    __le32 cdw10;            // 起始 LBA（逻辑块地址）
    __le16 cdw11;            // 块数 (length - 1)
};

// NVMe 队列——SQ/CQ 对，管理命令提交和完成
struct nvme_queue {
    struct nvme_dev     *dev;             // NVMe 设备
    struct nvme_command *sq_cmds;         // 提交队列 (SQ) 环缓冲区（驱动写入命令）
    volatile struct nvme_completion *cqes; // 完成队列 (CQ) 环缓冲区（硬件写入完成条目）
    dma_addr_t           sq_dma_addr;     // SQ DMA 地址（硬件 DMA 读取命令）
    dma_addr_t           cq_dma_addr;     // CQ DMA 地址（硬件 DMA 写入完成）
    u32 __iomem          *sq_tail_doorbell_addr;  // SQ 门铃寄存器（MMIO 写，通知硬件 SQ 有新命令）
    u32 __iomem          *cq_head_doorbell_addr;  // CQ 门铃寄存器（MMIO 写，通知硬件已处理完成）
    unsigned int         sq_tail;         // SQ 环尾指针（驱动写入位置）
    unsigned int         cq_head;         // CQ 环头指针（驱动读取位置）
    unsigned int         cq_phase;        // CQ 阶段位（硬件翻转该位标记新完成项，区分新旧条目）
};

// NVMe 完成队列条目——硬件写入的 I/O 完成状态
struct nvme_completion {
    __le32 result;           // 命令特定结果（读操作返回数据传输字节数等）
    __le32 sq_head;          // SQ 头指针（硬件已处理的 SQ 位置）
    __le16 sq_id;            // SQ 标识符
    __le16 command_id;       // 命令 ID（匹配 nvme_command.command_id）
    __le16 status;           // 状态字段（bit15: phase bit，低位: 状态码）
};
```

| 数据结构 | 头文件 | 在 read 系统调用中的作用 |
|----------|--------|--------------------------|
| `struct iovec` | `include/uapi/linux/uio.h` | 用户空间缓冲区描述（read 单段 buf 包装） |
| `struct iov_iter` | `include/linux/uio.h` | 管理读缓冲区迭代，data_source=ITER_DEST |
| `struct kiocb` | `include/linux/fs.h` | 携带 I/O 位置 pos，传递到 ext4 层 |
| `struct file` | `include/linux/fs.h` | 读操作的上下文，含 f_pos、f_op、f_mapping |
| `struct folio` | `include/linux/mm_types.h` | 页缓存单元，read 时页缓存查找/填充/拷贝 |
| `struct address_space` | `include/linux/fs.h` | 页缓存管理，XArray 索引 folio，a_ops 定义读操作 |
| `struct ext4_map_blocks` | `fs/ext4/ext4.h` | 逻辑块→物理块映射，read_folio 块映射结果 |
| `struct bio` | `include/linux/blk_types.h` | 块层 I/O 单元，read 时 REQ_OP_READ |
| `struct request` | `include/linux/blk-mq.h` | 块层调度单元，封装 bio 后提交到 NVMe |
| `struct nvme_command` | `drivers/nvme/host/nvme.h` | NVMe 读命令 (opcode=0x02) |
| `struct nvme_queue` | `drivers/nvme/host/nvme.h` | SQ/CQ 队列管理，MMIO 门铃操作 |
| `struct nvme_completion` | `drivers/nvme/host/nvme.h` | NVMe 完成条目，含 phase bit 和状态码 |

---

## 五、 总结

Linux 的 I/O 栈是一个高度分层的架构。VFS 提供了统一的抽象，Ext4 负责文件逻辑到磁盘逻辑的映射，Block 层负责 I/O 调度和多队列管理，NVMe 驱动负责将抽象请求转化为具体的硬件协议。数据流动的核心是 **"先到 Page Cache，缺页则构建 bio 交由底层 DMA 读取，读回后唤醒进程并拷贝至用户态"**。

对比 read 与 preadv/pwritev 系列系统调用：

| 维度 | read | preadv | pwritev |
|------|------|--------|---------|
| **定位** | 使用 f_pos（共享偏移） | 栈变量 pos（无竞争） | 栈变量 pos（无竞争） |
| **分散/聚集** | 单 buf | 多 iovec | 多 iovec |
| **数据方向** | 读（文件→用户） | 读（文件→用户） | 写（用户→文件） |
| **页缓存** | 有（O_DIRECT 除外） | 有（O_DIRECT 除外） | 有（O_DIRECT 除外） |