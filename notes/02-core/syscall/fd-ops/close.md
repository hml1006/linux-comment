# close 系统调用完整路径分析

## 1 概述

close 系统调用用于关闭一个文件描述符。与 read/write 不同，close 的路径主要涉及**文件描述符的清理**和**文件资源的释放**，在特定场景（文件有未落盘的延迟分配块）下才会触发脏数据回写，从而进入 Block 层和 NVMe 驱动。

### 关键特点

- close 的 fd 清理是**同步**的（立即从 fdtable 中移除）
- 文件对象（struct file）的最终释放可能是**同步**（返回用户态路径）或**异步**（task_work/deferred fput）
- ext4 没有实现 `f_op->flush`，所以 `filp_flush` 对 ext4 文件是**空操作**
- 若文件打开了 `EXT4_STATE_DA_ALLOC_CLOSE` 且有延迟分配块，`ext4_release_file` 会触发脏数据回写
- 通过 `dput` → `iput` → `ext4_evict_inode` 链可触发 inode 回收

---

## 2 涉及的内核层

| 层                      | 说明                                                       |
| ----------------------- | ---------------------------------------------------------- |
| **Syscall Entry** | close 系统调用分发 (fs/open.c)                             |
| **fd Table**      | 文件描述符表清理 (fs/file.c)                               |
| **VFS**           | 文件结构和 dentry 释放 (fs/file_table.c, fs/dcache.c)      |
| **ext4**          | ext4_release_file / 延迟分配回写 (fs/ext4/file.c, inode.c) |
| **Page Cache**    | 脏页回写 (writeback path, 仅条件触发)                      |
| **Block Layer**   | blk-mq 提交 (block/blk-core.c, blk-mq.c)                   |
| **NVMe 驱动**     | 命令提交 + 中断完成 (drivers/nvme/host/pci.c)              |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE1(close) - fs/open.c:1498

```c
SYSCALL_DEFINE1(close, unsigned int, fd)
{
    int retval;
    struct file *file;

    file = file_close_fd(fd);          // 从 fdtable 移除 fd
    if (!file)
        return -EBADF;

    retval = filp_flush(file, current->files);  // 调用 f_op->flush (ext4 无)

    // 返回用户态，使用同步 fput
    fput_close_sync(file);

    if (likely(retval == 0))
        return 0;
    return retval;
}
```

关键点：

- `file_close_fd` 是**同步**的，立即从 fdtable 清除 fd 条目
- `fput_close_sync` 是 `__fput` 的同步版本，与 `__fput_sync` 等价但针对 last-reference 场景优化
- 由于已确定返回用户态，不需要 defer fput

### 3.2 close_fd - fs/file.c:744（其他内核路径）

```c
int close_fd(unsigned fd)
{
    struct files_struct *files = current->files;
    struct file *file;

    spin_lock(&files->file_lock);
    file = file_close_fd_locked(files, fd);   // 持锁移除 fd
    spin_unlock(&files->file_lock);
    if (!file)
        return -EBADF;

    return filp_close(file, files);   // filp_flush + fput_close(deferred)
}
```

与系统调用版本的区别：

- `close_fd` 使用 `fput_close`（deferred 版本），通过 task_work 或 delayed work 执行 `__fput`
- `SYSCALL_DEFINE1(close)` 使用 `fput_close_sync`（同步版本），直接调用 `__fput`

---

## 4 fd 表操作层

### 4.1 file_close_fd - fs/file.c:889

```c
struct file *file_close_fd(unsigned int fd)
{
    struct files_struct *files = current->files;
    struct file *file;

    spin_lock(&files->file_lock);
    file = file_close_fd_locked(files, fd);
    spin_unlock(&files->file_lock);
    return file;
}
```

调用 `file_close_fd_locked` 完成实际工作。

### 4.2 file_close_fd_locked - fs/file.c:725

```c
struct file *file_close_fd_locked(struct files_struct *files, unsigned fd)
{
    struct fdtable *fdt = files_fdtable(files);
    struct file *file;

    if (fd >= fdt->max_fds)
        return NULL;
    fd = array_index_nospec(fd, fdt->max_fds);
    file = rcu_dereference_raw(fdt->fd[fd]);
    if (file) {
        rcu_assign_pointer(fdt->fd[fd], NULL);   // 清空 fd 槽位
        __put_unused_fd(files, fd);              // 回收 fd 号
    }
    return file;
}
```

核心行为：

1. 校验 fd 范围
2. 从 fdtable 的 `fd[]` 数组中取出 file 指针
3. 将 fd 槽位置为 NULL
4. 调用 `__put_unused_fd` 在 `open_fds` 位图中清除对应位（fd 号可重用）
5. 返回 file 指针（引用计数未变，调用者负责后续放回）

---

## 5 VFS 文件释放层

### 5.1 filp_flush - fs/open.c:1462

```c
static int filp_flush(struct file *filp, fl_owner_t id)
{
    int retval = 0;

    if (CHECK_DATA_CORRUPTION(file_count(filp) == 0, ...))
        return 0;

    if (filp->f_op->flush)
        retval = filp->f_op->flush(filp, id);    // ext4 无 flush

    if (likely(!(filp->f_mode & FMODE_PATH))) {
        dnotify_flush(filp, id);      // dnotify 清理
        locks_remove_posix(filp, id); // POSIX 锁清理
    }
    return retval;
}
```

> ext4 的 `file_operations` 没有实现 `.flush`，所以 `f_op->flush` 为 NULL，此函数仅做 dnotify 和 lock 清理。

### 5.2 __fput - fs/file_table.c:467（核心释放函数）

```c
static void __fput(struct file *file)
{
    struct dentry *dentry = file->f_path.dentry;
    struct vfsmount *mnt = file->f_path.mnt;
    struct inode *inode = file->f_inode;
    fmode_t mode = file->f_mode;

    if (unlikely(!(file->f_mode & FMODE_OPENED)))
        goto out;

    fsnotify_close(file);                       // 文件系统事件通知
    eventpoll_release(file);                     // epoll 清理
    locks_remove_file(file);                     // 文件锁清理
    security_file_release(file);                  // LSM 钩子

    if (unlikely(file->f_flags & FASYNC))
        if (file->f_op->fasync)
            file->f_op->fasync(-1, file, 0);    // FASYNC 清理

    if (file->f_op->release)
        file->f_op->release(inode, file);        // → ext4_release_file

    fops_put(file->f_op);                        // module_put
    put_file_access(file);
    dput(dentry);                                // 释放 dentry → 可能触发 iput
    mntput(mnt);                                 // 释放 mount
out:
    file_free(file);                             // 释放 file 对象
}
```

### 5.3 fput 的 defer 机制

`__fput` 可以通过三种方式执行：

| 入口                      | 执行方式                              | 使用场景                                |
| ------------------------- | ------------------------------------- | --------------------------------------- |
| `fput_close_sync`       | 同步，直接调用`__fput`              | `SYSCALL_DEFINE1(close)` 返回用户态时 |
| `fput_close` / `fput` | task_work (TWA_RESUME) →`____fput` | 内核路径 close_fd，当前进程退出时执行   |
| fallback                  | delayed work (delayed_fput_work)      | task_work 添加失败时                    |

---

## 6 ext4 文件系统层

### 6.1 ext4_release_file - fs/ext4/file.c:229

```c
static int ext4_release_file(struct inode *inode, struct file *filp)
{
    // 情况1：需要立即分配延迟块（DA_ALLOC_CLOSE）
    if (ext4_test_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE)) {
        ext4_alloc_da_blocks(inode);              // → 触发脏页回写
        ext4_clear_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE);
    }

    // 情况2：最后一个 writer 丢弃预分配块
    if ((filp->f_mode & FMODE_WRITE) &&
            (atomic_read(&inode->i_writecount) == 1) &&
            !EXT4_I(inode)->i_reserved_data_blocks) {
        down_write(&EXT4_I(inode)->i_data_sem);
        ext4_discard_preallocations(inode);       // 丢弃预分配
        up_write(&EXT4_I(inode)->i_data_sem);
    }

    // 情况3：dx 目录释放 htree 信息
    if (is_dx(inode) && filp->private_data)
        ext4_htree_free_dir_info(filp->private_data);

    return 0;
}
```

### 6.2 DA_ALLOC_CLOSE 回写路径

当文件使用延迟分配（delalloc）且设置 `EXT4_STATE_DA_ALLOC_CLOSE` 时：

```
ext4_release_file
  └─ ext4_alloc_da_blocks(inode)
       └─ filemap_flush(inode->i_mapping)      // mm/filemap.c:450
            └─ filemap_writeback(mapping, ...)  // mm/filemap.c:371
                 └─ do_writepages(mapping, wbc) // mm/page-writeback.c:2564
                      └─ mapping->a_ops->writepages()
                           └─ ext4_writepages  // fs/ext4/inode.c:3089
```

> `ext4_alloc_da_blocks` 在 i_reserved_data_blocks > 0 时调用 `filemap_flush`，该函数使用 `WB_SYNC_NONE` 模式（非阻塞），只发起回写但不等待完成。

### 6.3 ext4_writepages 回写路径

```
ext4_writepages
  └─ write_cache_pages(mapping, wbc, __mpage_da_writepage)
       └─ __mpage_da_writepage
            └─ mpage_da_map_blocks            // 块映射 + 分配
            └─ mpage_da_submit_io             // 提交 I/O
                 └─ ext4_bio_write_folio      // fs/ext4/page-io.c:458
                      └─ io_submit_add_bh     // 构造 BIO
                      └─ ext4_io_submit        // 提交 BIO
                           └─ blk_crypto_submit_bio(io->io_bio)
```

BIO 构造关键参数：

- `bio_alloc(bdev, BIO_MAX_VECS, REQ_OP_WRITE, GFP_NOIO)`
- `bio->bi_end_io = ext4_end_bio`（回写完成回调）
- `bio->bi_iter.bi_sector = bh->b_blocknr * (bh->b_size >> 9)`

### 6.4 dput → iput → ext4_evict_inode 链

`__fput` 中调用 `dput(dentry)` 释放 dentry：

```
dput(dentry)                             // fs/dcache.c:919
  └─ fast_dput() 成功 → 返回
  └─ finish_dput(dentry)
       └─ __dentry_kill(dentry)          // fs/dcache.c:647
            └─ dentry_unlink_inode(dentry)  // fs/dcache.c:451
                 └─ iput(inode)           // 释放 inode 引用
                      └─ iput_final(inode) // fs/inode.c
                           └─ sb->s_op->evict_inode(inode)
                                └─ ext4_evict_inode  // fs/ext4/inode.c:170
```

#### ext4_evict_inode 流程

```c
void ext4_evict_inode(struct inode *inode)
{
    if (inode->i_nlink) {
        // 文件仍有链接：仅截断 page cache
        truncate_inode_pages_final(&inode->i_data);
        goto no_delete;     // 跳过删除
    }

    // i_nlink == 0：需要删除 inode
    truncate_inode_pages_final(&inode->i_data);
    // ... 启动 journal 事务 ...
    ext4_orphan_del(handle, inode);       // 从孤儿链删除
    ext4_mark_inode_dirty(handle, inode); // 标记脏
    ext4_free_inode(handle, inode);       // 释放 inode 块
}
```

---

## 7 块设备层（回写路径）

### 7.1 submit_bio 路径

```
blk_crypto_submit_bio(bio)         // block/blk-crypto.c 或 inline
  └─ submit_bio(bio)               // block/blk-core.c:992
       └─ submit_bio_noacct(bio)
            └─ __submit_bio(bio)   // block/blk-core.c:636
                 └─ blk_mq_submit_bio(bio)  // block/blk-mq.c:3151
```

### 7.2 blk_mq_submit_bio

```
blk_mq_submit_bio(bio)
  ├─ blk_ia_range_merge_bio(bio)      // I/O 范围合并
  ├─ bio->bi_end_io = bi_end_io       // 保存完成回调
  ├─ blk_mq_get_bio_set_tag_set(bio)  // 获取 tag
  ├─ blk_mq_get_request(q, bio)       // 分配 request
  ├─ blk_mq_rq_ctx_init(rq, ...)      // 初始化 request
  ├─ blk_mq_bio_to_request(rq, bio)   // 绑定 bio 到 request
  ├─ blk_check_plug_flush(plug, rq)   // flush 检查
  ├─ blk_add_rq_to_plug(plug, rq)     // 尝试 plug 聚合
  └─ 或直接提交:
       └─ blk_mq_try_issue_directly(hctx, rq)
            └─ __blk_mq_issue_directly
                 └─ hctx->ops->queue_rq → nvme_queue_rq
```

---

## 8 NVMe 驱动层

### 8.1 命令提交

```
nvme_queue_rq(hctx, bd)                 // drivers/nvme/host/pci.c:1405
  └─ nvme_prep_rq(req)                  // nvme_prep_rq:1368
       ├─ nvme_setup_cmd(ns, req, cmd)  // nvme/core.c:1081
       │    └─ nvme_setup_rw(ns, req, cmd, nvme_cmd_write)  // 写命令
       └─ nvme_map_data(req, cmd)       // DMA 映射 (PRP/SGL)
  └─ nvme_sq_copy_cmd(nvmeq, req)       // pci.c:730 - memcpy 到 SQ 环
  └─ nvme_write_sq_db(nvmeq)            // pci.c:713 - 写门铃寄存器
       └─ writel(nvmeq->sq_tail_doorbell_addr, db_value)  // MMIO
```

### 8.2 写命令 vs 读命令

| 操作 | nvme_cmd_opcode    | bio 方向 |
| ---- | ------------------ | -------- |
| 读   | `nvme_cmd_read`  | READ     |
| 写   | `nvme_cmd_write` | WRITE    |

其余流程（DMA 映射、SQ 拷贝、门铃写入）完全一致。

### 8.3 中断完成

写命令完成路径：

```
nvme_irq(irq, nvmeq)                    // pci.c:1599
  └─ nvme_poll_cq(nvmeq, ...)           // pci.c:1578
       ├─ nvme_handle_cqe(nvmeq, cqe)   // pci.c:1531
       │    ├─ nvme_find_rq(hctx, cqe)  // 找到完成 request
       │    └─ blk_mq_add_to_batch(req) // 批量完成
       └─ nvme_ring_cq_doorbell(nvmeq)  // 写 CQ 门铃
  └─ nvme_pci_complete_batch(breq)      // 批量完成
       └─ blk_mq_end_request_batch()
            └─ bio_endio(bio)
                 └─ bio->bi_end_io()    // → ext4_end_bio
                      └─ folio_end_writeback(folio)
                      └─ ext4_io_end_callback()
                           └─ ext4_finish_bio()
```

---

## 9 文件关闭的四种场景

| 场景                           | 条件                            | 触发路径                                                                                         |
| ------------------------------ | ------------------------------- | ------------------------------------------------------------------------------------------------ |
| **只读关闭**             | 文件只读打开，无延迟分配        | `sys_close` → `__fput` → `ext4_release_file` (仅丢弃预分配) → `dput` → `file_free` |
| **写关闭（有延迟块）**   | 写打开，`DA_ALLOC_CLOSE` 标志 | 同上 +`ext4_alloc_da_blocks` → `filemap_flush` → do_writepages → ext4 → block → NVMe    |
| **写关闭（无延迟块）**   | 写打开，无延迟分配块            | 同只读关闭                                                                                       |
| **最后一个 dentry 释放** | 无其他引用                      | `dput` → `iput` → `ext4_evict_inode` → truncate/释放 inode                              |

---

## 10 完整流程图

```
                          close(fd)
                             |
                   +---------v----------+
                   |  SYSCALL_DEFINE1    |  系统调用入口
                   |  (fs/open.c:1498)   |
                   +---------+----------+
                             |
                   +---------v----------+
                   |  file_close_fd(fd)  |  fd 表操作
                   +---------+----------+  (fs/file.c:889)
                             |
                   +---------v----------+
                   | file_close_fd_     |  从 fdtable 移除 fd
                   | locked(files, fd)  |  fd[] 置 NULL
                   +---------+----------+  回收 fd 号
                             |
                   +---------v----------+
                   |  filp_flush(file,  |  VFS 层
                   |  current->files)   |  ext4 无 flush
                   +---------+----------+  仅清理锁/dnotify
                             |
                   +---------v----------+
                   |  fput_close_sync() |  同步 fput
                   +---------+----------+
                             |
                   +---------v----------+
                   |     __fput()       |  VFS 文件释放
                   +---------+----------+  (fs/file_table.c:467)
                             |
            +----------------+----------------+
            |                                 |
    +-------v--------+              +---------v--------+
    | eventpoll_release|             | fsnotify_close   |
    | locks_remove_file|             | security_file_   |
    +------------------+             | release          |
                                     +---------+--------+
                                               |
                                     +---------v--------+
                                     |  f_op->release()  |
                                     +---------+--------+
                                               |
                                     +---------v--------+
                                     |  ext4_release_file|  ext4 层
                                     |  (fs/ext4/        |  (fs/ext4/
                                     |   file.c:229)     |   inode.c:170)
                                     +---------+--------+
                                               |
                          +--------------------+--------------------+
                          |                                         |
                          |  [条件: DA_ALLOC_CLOSE]                  |
                          |  && i_reserved_data_blocks > 0           |
                          |                                         |
                 +--------v--------+                      +---------v--------+
                 | ext4_alloc_da_  |                      | 无延迟分配 /     |
                 | blocks(inode)   |                      | 非 DA_ALLOC_CLOSE |
                 +--------+--------+                      |                  |
                          |                               +---------+--------+
                 +--------v--------+                                |
                 | filemap_flush() |   (fs/ext4/                    |
                 | (WB_SYNC_NONE)  |    inode.c:3378)               |
                 +--------+--------+                                |
                          |                                         |
                 +--------v--------+                                |
                 | do_writepages() |  脏页回写                      |
                 | ext4_writepages |  (mm/page-writeback.c)         |
                 +--------+--------+                                |
                          |                                         |
                 +--------v--------+                                |
                 | ext4_bio_write_ |  构造 BIO                      |
                 | folio()         |  (fs/ext4/                     |
                 +--------+--------+   page-io.c:458)               |
                          |                                         |
                 +--------v--------+                                |
                 | ext4_io_submit()|  提交 BIO                      |
                 +--------+--------+                                |
                          |                                         |
                 +--------v--------+                                |
                 | submit_bio() -> |  Block 层                      |
                 | blk_mq_submit_  |  (block/blk-core.c)            |
                 | bio()           |                                |
                 +--------+--------+                                |
                          |                                         |
                 +--------v--------+                                |
                 | nvme_queue_rq() |  NVMe 驱动                     |
                 | nvme_prep_rq()  |  (drivers/nvme/host/pci.c)     |
                 | nvme_setup_cmd()|                                |
                 | nvme_sq_copy_   |                                |
                 | cmd()           |                                |
                 | nvme_write_sq_  |                                |
                 | db() (MMIO)     |                                |
                 +--------+--------+                                |
                          |                                         |
                 +--------v--------+                                |
                 | [异步中断完成]  |                                |
                 | nvme_irq() ->   |  写完成回调                    |
                 | blk_mq_end_     |  (drivers/nvme/host/pci.c)     |
                 | request_batch() |                                |
                 | -> ext4_end_bio()|                               |
                 | -> folio_end_   |                                |
                 |    writeback()  |                                |
                 +-----------------+                                |
                          |                                         |
                          +--------------------+--------------------+
                                               |
                                     +---------v--------+
                                     |  dput(dentry)     |  VFS 层
                                     |  (fs/dcache.c)    |  释放 dentry
                                     +---------+--------+
                                               |
                                     +---------v--------+
                                     |  [条件: 最后引用]  |
                                     |  iput(inode)      |
                                     +---------+--------+
                                               |
                          +--------------------+--------------------+
                          |                                         |
                 +--------v--------+                      +---------v--------+
                 | i_nlink == 0   |                      | i_nlink > 0      |
                 | ext4_evict_    |                      | (文件仍有链接)   |
                 | inode()        |                      | 仅截断 page cache|
                 | 删除 inode     |                      |                  |
                 | 释放 inode 块  |                      +---------+--------+
                 +--------+--------+                                |
                          |                                         |
                          +--------------------+--------------------+
                                               |
                                     +---------v--------+
                                     |  file_free(file)  |  释放 file 对象
                                     +------------------+
```

---

## 11 函数调用栈

```
/* ========== 主路径：始终执行 ========== */

SYSCALL_DEFINE1(close, fd)                          // fs/open.c:1498
└─ file_close_fd(fd)                                // fs/file.c:889 — 从 fdtable 移除 fd
   └─ file_close_fd_locked(files, fd)               // fs/file.c:725 — 持锁操作
        ├─ rcu_dereference_raw(fdt->fd[fd])         // 取出 file 指针
        ├─ rcu_assign_pointer(fdt->fd[fd], NULL)    // 清空 fd 槽位
        └─ __put_unused_fd(files, fd)               // fs/file.c — 回收 fd 号（位图清除）
└─ filp_flush(file, current->files)                 // fs/open.c:1462 — VFS flush
   ├─ f_op->flush(filp, id)                         // ext4 无 flush → NULL，跳过
   ├─ dnotify_flush(filp, id)                       // dnotify 目录监听清理
   └─ locks_remove_posix(filp, id)                  // POSIX 记录锁清理
└─ fput_close_sync(file)                            // fs/file_table.c:595 — 同步 fput
   └─ __fput(file)                                  // fs/file_table.c:467 — 文件对象释放
        ├─ fsnotify_close(file)                     // inotify/fsnotify 事件通知
        ├─ eventpoll_release(file)                  // epoll 清理（从 epoll 集中移除）
        ├─ locks_remove_file(file)                  // 文件锁清理（FL_FLOCK 等）
        ├─ security_file_release(file)              // LSM 钩子（SELinux/AppArmor）
        ├─ f_op->release(inode, file)               // → ext4_release_file (见下方分支)
        ├─ fops_put(file->f_op)                     // module_put（释放文件操作模块）
        ├─ put_file_access(file)                    // 文件访问权限计数递减
        ├─ dput(dentry)                             // fs/dcache.c:919 — dentry 引用释放
        │  └─ [dentry 引用归零] → __dentry_kill    // fs/dcache.c:647
        │       └─ dentry_unlink_inode(dentry)      // fs/dcache.c:451
        │            └─ iput(inode)                 // fs/inode.c — inode 引用释放
        │                 └─ [inode 引用归零] → iput_final
        │                      └─ ext4_evict_inode  // fs/ext4/inode.c:170 (见下方分支)
        ├─ mntput(mnt)                              // 挂载引用释放
        └─ file_free(file)                          // 释放 file 对象内存


/* ========== 条件分支 1: ext4_release_file ========== */
/* 触发条件: f_op->release 时调用，始终执行 */

ext4_release_file(inode, filp)                      // fs/ext4/file.c:229
├─ [条件: DA_ALLOC_CLOSE && i_reserved_data_blocks > 0]
│  └─ ext4_alloc_da_blocks(inode)                   // fs/ext4/inode.c:3378 — 分配延迟块
│       └─ filemap_flush(mapping)                   // mm/filemap.c:450 — 脏页回写（WB_SYNC_NONE）
│            └─ do_writepages(mapping, wbc)         // mm/page-writeback.c:2564
│                 └─ ext4_writepages(mapping, wbc)  // fs/ext4/inode.c:3089
│                      └─ write_cache_pages(...)    // 遍历脏页
│                           └─ __mpage_da_writepage // 每页处理
│                                ├─ mpage_da_map_blocks      // 块映射 + 分配
│                                └─ mpage_da_submit_io       // 提交 I/O
│                                     └─ ext4_bio_write_folio // fs/ext4/page-io.c:458
│                                          ├─ io_submit_add_bh // 构造 BIO（逐个 block buffer）
│                                          └─ ext4_io_submit   // fs/ext4/page-io.c:398
│                                               └─ blk_crypto_submit_bio(bio)  // 加密/直接提交
│                                                    └─ submit_bio(bio)        // block/blk-core.c:992
│                                                         └─ __submit_bio(bio) // block/blk-core.c:636
│                                                              └─ blk_mq_submit_bio(bio) // block/blk-mq.c:3151
│                                                                   ├─ blk_mq_get_request(q, bio)  // 分配 request
│                                                                   ├─ blk_mq_bio_to_request(rq, bio) // 绑定 bio
│                                                                   └─ blk_mq_try_issue_directly(hctx, rq) // 直接下发
│                                                                        └─ nvme_queue_rq(hctx, bd) // drivers/nvme/host/pci.c:1405
│                                                                             ├─ nvme_prep_rq(req)  // pci.c:1368
│                                                                             │    ├─ nvme_setup_cmd(ns, req, cmd) // core.c:1081
│                                                                             │    │    └─ nvme_setup_rw(..., nvme_cmd_write) // 写命令
│                                                                             │    └─ nvme_map_data(req, cmd)  // DMA 映射 (PRP/SGL)
│                                                                             ├─ nvme_sq_copy_cmd(nvmeq, req)  // pci.c:730 — memcpy 到 SQ 环
│                                                                             └─ nvme_write_sq_db(nvmeq)       // pci.c:713 — writel MMIO 门铃
│                                                    /* ── 异步中断完成 ── */
│                                                    nvme_irq(irq, nvmeq)           // pci.c:1599
│                                                    └─ nvme_poll_cq(nvmeq, ...)   // pci.c:1578
│                                                         ├─ nvme_handle_cqe(nvmeq, cqe) // pci.c:1531
│                                                         │    ├─ nvme_find_rq(hctx, cqe)  // 找到完成 request
│                                                         │    └─ blk_mq_add_to_batch(req) // 批量完成
│                                                         └─ nvme_ring_cq_doorbell(nvmeq) // 写 CQ 门铃
│                                                    └─ nvme_pci_complete_batch(breq) // 批量完成
│                                                         └─ blk_mq_end_request_batch()
│                                                              └─ bio_endio(bio)
│                                                                   └─ ext4_end_bio(bio) // fs/ext4/page-io.c
│                                                                        ├─ folio_end_writeback(folio) // 标记写回完成
│                                                                        └─ ext4_io_end_callback()
│                                                                             └─ ext4_finish_bio() // 完成回调
│
├─ [条件: FMODE_WRITE && i_writecount == 1 && !i_reserved_data_blocks]
│  └─ ext4_discard_preallocations(inode)            // 丢弃预分配块
│
└─ [条件: is_dx(inode) && private_data]
   └─ ext4_htree_free_dir_info(filp->private_data)  // 释放 dx 目录 htree 信息


/* ========== 条件分支 2: ext4_evict_inode ========== */
/* 触发条件: dput → iput 后 inode 引用计数归零 */

ext4_evict_inode(inode)                             // fs/ext4/inode.c:170
├─ [条件: i_nlink > 0]
│  ├─ truncate_inode_pages_final(&inode->i_data)    // 截断 page cache
│  ├─ clear_inode(inode)                            // 清理 inode 状态
│  └─ goto no_delete                                // 跳过删除
│
└─ [条件: i_nlink == 0]                             // 文件已被删除
   ├─ truncate_inode_pages_final(&inode->i_data)    // 截断 page cache
   ├─ [启动 jbd2 事务] ext4_journal_start()         // 开启 journal 事务
   ├─ ext4_orphan_del(handle, inode)                // 从孤儿链表删除
   ├─ ext4_mark_inode_dirty(handle, inode)          // 标记 inode 脏
   └─ ext4_free_inode(handle, inode)                // 释放 inode 块（磁盘空间）
```

> 条件分支 1 的 `DA_ALLOC_CLOSE` 路径仅在 `EXT4_STATE_DA_ALLOC_CLOSE` 标志置位且 `i_reserved_data_blocks > 0` 时执行。
> 条件分支 2 仅在 dentry 引用计数归零触发 `dput` → `iput` 链时执行。

---

## 12 关键数据结构

```c
// ========== fd 表相关 (include/linux/fdtable.h) ==========

// 进程文件描述符表
// 每个进程的 task_struct 中通过 files 指针引用
struct files_struct {
    atomic_t count;                  // 引用计数，clone 时共享 fd 表
    bool resize_in_progress;         // fd 表扩容标志
    wait_queue_head_t resize_wait;   // 扩容等待队列
    struct fdtable __rcu *fdt;       // 指向当前 fdtable（RCU 保护，可动态扩容）
    struct fdtable fdtab;            // 嵌入式 fdtable（默认 64 个 fd）
    spinlock_t file_lock;            // 保护 fd 表的自旋锁
    unsigned int next_fd;            // 下一个可用的 fd 号（分配优化）
    unsigned long close_on_exec_init[1];  // 初始 exec 时关闭的 fd 位图
    unsigned long open_fds_init[1];       // 初始已打开的 fd 位图
    unsigned long full_fds_bits_init[1];  // 初始 full_fds 位图
    struct file __rcu *fd_array[NR_OPEN_DEFAULT];  // 初始 fd 指针数组（默认 64）
};

// fd 表描述符
// 当 NR_OPEN_DEFAULT 不够时，动态分配更大的 fd[] 数组替代 fd_array
struct fdtable {
    unsigned int max_fds;            // fd[] 数组的最大容量
    struct file __rcu **fd;          // fd 指针数组（指向 struct file 或 NULL）
    unsigned long *close_on_exec;    // 位图：exec 时自动关闭的 fd
    unsigned long *open_fds;         // 位图：已分配的 fd 号
    unsigned long *full_fds_bits;    // 位图：full_fds 的优化位
    struct rcu_head rcu;             // RCU 回调，用于延迟释放
};

// ========== 文件对象 (include/linux/fs.h) ==========

// 文件对象——每个 open 创建一个
// 多个 fd 可以指向同一个 file（通过 dup()）
struct file {
    struct path f_path;              // 文件路径（dentry + mount）
    struct inode *f_inode;           // 指向 inode 的快捷方式（f_path.dentry->d_inode）
    const struct file_operations *f_op;  // 文件操作函数表
    spinlock_t f_lock;               // 保护 f_pos 等字段的自旋锁
    atomic_long_t f_count;           // 引用计数（file_ref_t 封装）
    unsigned int f_flags;            // 文件状态标志（O_RDONLY, O_SYNC 等）
    fmode_t f_mode;                  // 打开模式（FMODE_READ, FMODE_WRITE 等）
    loff_t f_pos;                    // 当前读写位置（文件偏移量）
    struct fown_struct f_owner;      // 异步 I/O 所有者（SIGIO 信号）
    u64 f_version;                   // 版本号（用于 NFS 等网络文件系统）
    void *private_data;              // 文件系统私有数据（ext4 的 htree 信息等）
    struct address_space *f_mapping; // 页缓存映射（通常等于 inode->i_mapping）
};

// ========== 内存 inode (include/linux/fs.h) ==========

// 内存中的 inode 节点
// 每个文件/目录在内存中对应一个 inode 对象
struct inode {
    umode_t i_mode;                  // 文件类型和权限
    kuid_t i_uid;                    // 所有者 UID
    kgid_t i_gid;                    // 所有者 GID
    unsigned int i_nlink;            // 硬链接计数（close 时通过 dput->iput 检查）
    dev_t i_rdev;                    // 设备号（块/字符设备）
    loff_t i_size;                   // 文件大小（字节）
    struct timespec64 i_atime;       // 最后访问时间
    struct timespec64 i_mtime;       // 最后修改时间
    struct timespec64 i_ctime;       // 最后状态变更时间
    spinlock_t i_lock;               // 保护 inode 字段的自旋锁
    atomic_t i_count;                // 引用计数（dentry 引用 + 文件引用）
    struct address_space *i_mapping; // 页缓存地址空间
    const struct inode_operations *i_op;  // inode 操作表
    const struct file_operations *i_fop;  // 默认文件操作表
    struct super_block *i_sb;        // 所属超级块
    struct list_head i_dentry;       // 关联的 dentry 链表（别名列表）
    // 文件系统私有数据（ext4 使用 ext4_inode_info 扩展）
};

// ========== 目录项 dentry (include/linux/dcache.h) ==========

// 目录项缓存——内存中的路径组件
struct dentry {
    struct dentry *d_parent;         // 父目录 dentry
    struct qstr d_name;              // 文件名（快速比较）
    struct inode *d_inode;           // 指向的 inode（可为 NULL，表示负 dentry）
    const struct dentry_operations *d_op;  // dentry 操作表
    struct lockref d_lockref;        // 引用计数 + 自旋锁（dput 操作的核心）
    struct list_head d_lru;          // LRU 链表（dcache 回收）
    struct list_head d_child;        // 父目录的 child 链表
    struct list_head d_subdirs;      // 子目录链表
    unsigned int d_flags;            // 标志位（DCACHE_* 系列）
};

// ========== ext4 扩展 inode (fs/ext4/ext4.h) ==========

// ext4 文件系统的扩展 inode 信息
// 通过 EXT4_I(inode) 宏从 struct inode 获取
struct ext4_inode_info {
    // --- 延迟分配相关 ---
    ext4_lblk_t i_reserved_data_blocks;  // 预留的数据块数（>0 时有延迟分配块）
    ext4_lblk_t i_reserved_meta_blocks;  // 预留的元数据块数
    ext4_lblk_t i_da_metadata_calc_last_lblock;  // 延迟分配元数据计算
    // --- 磁盘空间 ---
    loff_t i_disksize;               // 磁盘上实际大小（与 i_size 区别）
    loff_t i_da_write_begin;         // 延迟分配写起始位置
    // --- 事务 ---
    struct jbd2_inode *jinode;       // journal inode 信息
    // --- 预分配 ---
    struct ext4_prealloc_tree i_prealloc_tree;  // 预分配块树
    // --- 扩展属性 ---
    struct inode vfs_inode;          // 嵌入式 VFS inode（必须放在最后）
};

// ========== close 特有的引用计数优化 (include/linux/file_ref.h) ==========

// 文件引用计数优化（用于 close 的 fput_close_sync）
// 将 refcount 从 1 -> 0 的路径（last reference）做快速判断
// 避免在 close 热点路径上使用慢速的 atomic_dec_and_test
struct file_ref {
    union {
        // 正常路径：使用 atomic_long_t 计数
        atomic_long_t refcnt;
        // 计数为 0 时，标记为 NOREF 状态
        // 计数为 1 时，可快速进入 __fput 无需额外原子操作
    };
};
// 关键状态：
//   FILE_REF_NOREF  (0):      无引用，可释放
//   FILE_REF_ONEREF (1):      仅有一个引用（close 的典型场景）
//   FILE_REF_MAX    (>1):     多个引用，需要正常 atomic_dec
```

---

## 13 优化机制

### 13.1 fput 延迟释放

- **task_work**: 当前进程返回用户态前执行 `____fput`，避免在中断/softirq 中执行耗时操作
- **delayed fput work**: 当 task_work 添加失败，使用系统 workqueue 延迟执行

### 13.2 file_ref_put_close 优化

`fput_close_sync` 使用 `file_ref_put_close` 快速判断：

- 如果 refcount 已从 1 变为 NOREF（最后引用），直接调用 `__fput`
- 避免了额外的原子操作和分支

### 13.3 DA_ALLOC_CLOSE 回写优化

- `filemap_flush` 使用 `WB_SYNC_NONE`（非阻塞），仅发起回写不等待
- 实际 I/O 提交后立即返回，不阻塞 close 调用者
- 写完成由 `ext4_end_bio` 异步回调处理

### 13.4 批量完成

NVMe `nvme_pci_complete_batch` 使用 `blk_mq_end_request_batch` 批量完成多个 request，减少锁竞争和函数调用开销。

---

## 14 总结

close 系统调用完整路径总结：

```

用户态 close(fd)
  │
  ├─(1) fd 清理 (同步)
  │   └─ file_close_fd → 清空 fd 槽位 + 回收 fd 号
  │
  ├─(2) VFS 清理 (条件触发 flush)
  │   └─ filp_flush → dnotify + POSIX locks (ext4 无 flush)
  │
  ├─(3) 文件对象释放 (同步/异步)
  │   └─ __fput
  │        ├─ epoll / lock / security 清理
  │        ├─ ext4_release_file
  │        │    ├─ 丢弃预分配块
  │        │    └─ DA_ALLOC_CLOSE → filemap_flush (脏页回写起点)
  │        ├─ dput → iput → ext4_evict_inode (可选)
  │        └─ file_free
  │
  └─[脏页回写路径 - 条件触发]
       └─ filemap_flush → do_writepages → ext4_writepages
            → ext4_bio_write_folio → ext4_io_submit
            → blk_crypto_submit_bio → submit_bio
            → blk_mq_submit_bio → nvme_queue_rq
            → nvme_prep_rq → nvme_sq_copy_cmd
            → nvme_write_sq_db (writel MMIO)
            → [NVMe 完成中断]
            → nvme_irq → nvme_poll_cq → nvme_handle_cqe
            → blk_mq_end_request_batch → ext4_end_bio
            → folio_end_writeback

```

close 是典型的**控制路径远长于数据路径**的系统调用。大多数 close 调用仅涉及 fd 表和 VFS 层的元数据操作，只有在回写脏数据时才触及存储栈的最底层（NVMe 门铃寄存器写入）。

```

```
