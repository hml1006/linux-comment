# 块层 — 文件系统交互 (Part IV)

> 本文档拆分自 [block_layer_analysis.md](block_layer_analysis.md) Part IV，涵盖Page Cache与buffer_head在块设备读写中的作用、Writeback回写机制

## Part IV: 文件系统交互

## 17. Page Cache 与 buffer_head 在块设备读写中的作用

### 17.1 概述

Page Cache 和 buffer_head 是 Linux 块设备 I/O 栈中承上启下的两个核心机制：

- **Page Cache（页缓存）**：以 `struct folio`（页面）为单位的缓存层，位于 VFS/文件系统与块层之间。所有文件数据的读写都经过页缓存，实现了"缓存命中则零拷贝返回，未命中则触发块设备 I/O"。
- **buffer_head**：以块（block）为单位的 I/O 描述符，将页缓存中的页面与磁盘块号建立映射，并通过 `submit_bh()` 将 I/O 请求转化为 bio 提交给块层。

**在 I/O 栈中的位置**：

```
用户空间    read() / write() / mmap()
   │
   ▼
VFS        generic_file_read_iter() / generic_file_write_iter()
   │
   ▼
Page Cache  filemap_read() → 查找页缓存 (address_space.i_pages)
   │         ├─ 缓存命中 → copy_to_user() (零磁盘 I/O)
   │         └─ 缓存未命中 → 缺页 → read_folio() → 块设备 I/O
   │
   ▼
buffer_head  ext4_read_folio() → block_read_full_folio()
   │         ├─ 块号映射: get_block() 将逻辑块号转为物理块号
   │         └─ submit_bh() → bio_alloc() + submit_bio()
   │
   ▼
Bio 层      submit_bio() → blk_mq_submit_bio() → 驱动
```

### 17.2 核心数据结构

#### 17.2.1 `struct address_space` — 页缓存管理器

（[fs.h](file:///home/louis/code/linux/include/linux/fs.h)）每个文件/块设备的 inode 关联一个 `address_space`，管理该文件的所有缓存页面：

```c
struct address_space {
    struct inode         *host;           /* 所属 inode（文件或块设备） */
    struct xarray         i_pages;        /* 页面索引树（key=页偏移, value=folio） */
    struct rw_semaphore   invalidate_lock;/* 缓存失效保护锁 */
    gfp_t                 gfp_mask;       /* 页面分配标志 */
    unsigned long         nrpages;        /* 缓存页面总数 */
    pgoff_t               writeback_index;/* 回写起始索引 */
    const struct address_space_operations *a_ops; /* 操作函数表（关键！） */
    unsigned long         flags;           /* AS_* 标志位 */
    errseq_t              wb_err;         /* 写回错误序列号 */
    struct list_head       i_private_list;  /* 私有链表（buffer_head 关联） */
    void                 *i_private_data; /* 私有数据 */
};
```

**`i_pages`（xarray）**：高效的基数树结构，以页面索引（folio->index）为 key，存储 `struct folio*`。支持三种 XArray 标签（mark），用于高效查找特定状态的页面：

| 标签 | 含义 |
|------|------|
| `PAGECACHE_TAG_DIRTY` | 脏页（需要回写） |
| `PAGECACHE_TAG_WRITEBACK` | 正在回写中 |
| `PAGECACHE_TAG_TOWRITE` | 标记为待写入 |

#### 17.2.2 `struct address_space_operations` — 文件系统操作接口

（[fs.h](file:///home/louis/code/linux/include/linux/fs.h)）每个文件系统实现自己的 `a_ops`，将通用的页缓存操作与文件系统特定的逻辑解耦：

```c
struct address_space_operations {
    int (*read_folio)(struct file *, struct folio *);       /* 读一页 */
    int (*writepages)(struct address_space *, struct wbc *); /* 批量回写 */
    bool (*dirty_folio)(struct address_space *, struct folio *); /* 标记脏页 */
    void (*readahead)(struct readahead_control *);           /* 预读 */
    int (*write_begin)(...);                                 /* 写开始（准备页面） */
    int (*write_end)(...);                                   /* 写结束（提交数据） */
    ssize_t (*direct_IO)(struct kiocb *, struct iov_iter *); /* 直接 I/O */
    // ...
};
```

**ext4 的实现**（[ext4/inode.c](file:///home/louis/code/linux/fs/ext4/inode.c)）：
- `read_folio` → `ext4_read_folio()` → `ext4_mpage_readpages()`
- `readahead` → `ext4_readahead()` → `ext4_mpage_readpages()`
- `writepages` → `ext4_writepages()` → `ext4_do_writepages()` → `mpage_add_bh_to_extent()` → `submit_bh()`

#### 17.2.3 `struct buffer_head` — 块 I/O 桥梁

（[buffer_head.h](file:///home/louis/code/linux/include/linux/buffer_head.h)）将一个页面内的每个块（block）与磁盘块号建立映射：

```c
struct buffer_head {
    unsigned long        b_state;          /* BH_* 状态标志位 */
    struct buffer_head  *b_this_page;      /* 同一页面内的循环链表 */
    union {
        struct page     *b_page;           /* 所属页面（已废弃） */
        struct folio    *b_folio;          /* 所属 folio（推荐使用） */
    };
    sector_t             b_blocknr;        /* 磁盘块号（物理） */
    size_t               b_size;           /* 块大小（通常 4KB） */
    char                *b_data;           /* 指向页面内数据 */
    struct block_device *b_bdev;           /* 目标块设备 */
    bh_end_io_t         *b_end_io;         /* I/O 完成回调 */
    void                *b_private;        /* 回调私有数据 */
    struct list_head      b_assoc_buffers;  /* 关联链表 */
    struct address_space *b_assoc_map;      /* 关联的 address_space */
    atomic_t              b_count;          /* 引用计数 */
    spinlock_t             b_uptodate_lock;  /* 页面内多 bh 的完成同步锁 */
};
```

**buffer_head 状态标志（`b_state`）**：

| 标志 | 含义 |
|------|------|
| `BH_Uptodate` | 缓冲区包含有效数据 |
| `BH_Dirty` | 缓冲区数据已修改，需要回写 |
| `BH_Lock` | 缓冲区已锁定（I/O 进行中） |
| `BH_Req` | 已提交 I/O 请求 |
| `BH_Mapped` | 已建立磁盘块号映射 |
| `BH_New` | 映射是新创建的（get_block 刚分配） |
| `BH_Async_Read` | 异步读取中 |
| `BH_Async_Write` | 异步写入中 |
| `BH_Delay` | 延迟分配（尚未分配磁盘块） |
| `BH_Meta` | 包含元数据 |
| `BH_Boundary` | 块后存在不连续区域 |

**一个页面内的 buffer_head 布局**：

```
folio (4KB, blocksize=1KB)
┌──────────────────────────────────────┐
│ ┌──────────┬──────────┬──────────┬──────────┐ │
│ │ bh[0]    │ bh[1]    │ bh[2]    │ bh[3]    │ │
│ │ block=100│ block=101│ block=102│ block=103│ │
│ │ offset=0 │ offset=1K│ offset=2K│ offset=3K│ │
│ │ len=1KB  │ len=1KB  │ len=1KB  │ len=1KB  │ │
│ └──────────┴──────────┴──────────┴──────────┘ │
└──────────────────────────────────────┘
  bh[0]→b_this_page→bh[1]→b_this_page→bh[2]→b_this_page→bh[3]→b_this_page→bh[0]
  (循环链表，head = folio_buffers(folio))
```

### 17.3 Page Cache 读路径

#### 17.3.1 完整调用栈

```text
# 文件读取时的 Page Cache 和 buffer_head 交互流程
#
# 数据流: 用户空间 ← copy_to_user ← 页缓存 folio ← buffer_head ← bio ← 块设备
# 关键: 缓存命中直接返回; 缓存未命中通过 buffer_head 触发块设备 I/O

read(fd, buf, count)
  │  # 用户空间系统调用
  │
  └─ ksys_read() → vfs_read()
      │  # VFS 层
      │
      └─ file->f_op->read_iter()
          │
          └─ generic_file_read_iter(iocb, iter)
              │  # mm/filemap.c
              │  # 通用文件读取迭代器
              │
              └─ filemap_read(iocb, iter, 0)
                  │  # mm/filemap.c
                  │  # 核心: 页缓存读取循环
                  │
                  └─ [do while more data]:
                      │
                      ├─ filemap_get_pages(iocb, count, &fbatch, false)
                      │   │  # mm/filemap.c
                      │   │  # 批量获取页缓存中的 folio
                      │   │
                      │   ├─ filemap_get_read_batch(mapping, index, last_index, fbatch)
                      │   │   │  # 通过 xarray 查找页缓存中已存在的 folio
                      │   │   │  # 遍历 i_pages 树, 收集 uptodate 的 folio
                      │   │   │  # 放入 fbatch 数组, 一次性最多 15 个 folio
                      │   │   │
                      │   │   ├─ [缓存命中] fbatch 中有 folio
                      │   │   │   # 页缓存已包含所需数据, 无需磁盘 I/O
                      │   │   │   # 直接跳到 copy_to_user 步骤
                      │   │   │
                      │   │   └─ [缓存未命中] fbatch 为空
                      │   │       │
                      │   │       ├─ page_cache_sync_ra(&ractl, count)
                      │   │       │   # mm/readahead.c
                      │   │       │   # 同步预读: 提前加载后续页面
                      │   │       │   # 调用 a_ops->readahead() → ext4_readahead()
                      │   │       │
                      │   │       └─ filemap_create_folio(iocb, fbatch)
                      │   │           │  # mm/filemap.c
                      │   │           │  # 分配新 folio 并加入页缓存
                      │   │           │
                      │   │           └─ filemap_read_folio(file, mapping->a_ops->read_folio)
                      │   │               │  # ★ 关键: 调用文件系统的 read_folio ★
                      │   │               │  # 触发实际的块设备 I/O
                      │   │               │
                      │   │               └─ a_ops->read_folio(file, folio)
                      │   │                   │  # 对于 ext4: ext4_read_folio()
                      │   │                   │  # fs/ext4/readpage.c
                      │   │                   │
                      │   │                   └─ ext4_mpage_readpages(inode, NULL, NULL, folio)
                      │   │                       │  # fs/ext4/readpage.c
                      │   │                       │  # 核心: 将逻辑块号映射到物理块号
                      │   │                       │  # 构造 bio, 提交给块层
                      │   │                       │  # 注意: ext4 直接构造 bio, 不经过 buffer_head!
                      │   │                       │  # (ext4 使用 mpage 机制, 绕过 buffer_head 直接提交 bio)
                      │   │                       │
                      │   │                       └─ [for each logical block in folio]:
                      │   │                           │  # 遍历 folio 范围内的每个逻辑块
                      │   │                           │
                      │   │                           ├─ ext4_map_blocks(inode, iblock, &map)
                      │   │                           │   # 逻辑块号 → 物理块号映射
                      │   │                           │   # 查询 extent tree 或 indirect block
                      │   │                           │   # 返回: map.m_pblk = 物理块号, map.m_len = 连续块数
                      │   │                           │
                      │   │                           └─ bio_add_folio_nofail(bio, folio, len, offset)
                      │   │                               # 将 folio 页面添加到 bio
                      │   │                               # 设置 bio->bi_iter.bi_sector = 物理块号 << (blkbits - 9)
                      │   │
                      │   └─ [等待 I/O 完成]
                      │       # folio_lock() 等待 folio 被标记为 uptodate
                      │       # bio 完成后: end_io → folio_mark_uptodate → folio_unlock
                      │
                      └─ copy_folio_to_iter(folio, offset, bytes, iter)
                          │  # 将 folio 中的数据拷贝到用户空间 iov_iter
                          │  # 内部调用 copy_to_user()
                          │  # ★ 数据从页缓存到用户空间, 涉及 1 次 CPU 拷贝 ★
                          │
                          └─ folio_mark_accessed(folio)
                              # 标记页面被访问, 用于 LRU 回收决策
```

#### 17.3.2 非 ext4 文件系统的 buffer_head 读路径

对于依赖 buffer_head 的传统文件系统（如 ext2、FAT、`block_read_full_folio` 路径）：

```text
# buffer_head 读路径（传统文件系统）
# 调用链: filemap_read → read_folio → block_read_full_folio → submit_bh

read_folio(file, folio)
  │
  └─ block_read_full_folio(folio, get_block)
      │  # fs/buffer.c
      │  # 为 folio 创建 buffer_head 链表, 逐个提交 I/O
      │
      ├─ folio_create_buffers(folio, inode, 0)
      │   │  # 将 folio 按 blocksize 切分为多个 buffer_head
      │   │  # 例如: 4KB folio, 1KB blocksize → 4 个 buffer_head
      │   │  # 每个 buffer_head 通过 b_this_page 形成循环链表
      │   │
      │   └─ 返回 head (第一个 buffer_head)
      │
      └─ [for each buffer_head in folio]:
          │  # 遍历 folio 的 buffer_head 循环链表
          │
          ├─ if (buffer_uptodate(bh)) → continue
          │   # 该块已包含有效数据, 跳过
          │
          ├─ if (!buffer_mapped(bh)):
          │   │  # 该块尚未建立磁盘映射
          │   │
          │   └─ get_block(inode, iblock, bh, 0)
          │       │  # ★ 文件系统回调: 逻辑块号 → 物理块号 ★
          │       │  # 文件系统负责查找块映射表
          │       │  # 成功: bh->b_blocknr = 物理块号, set_buffer_mapped(bh)
          │       │  # 失败: 超出文件末尾 → folio_zero_range() 填充零
          │       │
          │       └─ 若映射成功: set_buffer_mapped(bh)
          │
          ├─ lock_buffer(bh)
          │   # 锁定 buffer_head, 确保 I/O 互斥
          │
          ├─ mark_buffer_async_read(bh)
          │   # 设置 BH_Async_Read, 设置 bh->b_end_io = end_buffer_async_read
          │
          └─ submit_bh(REQ_OP_READ, bh)
              │  # fs/buffer.c
              │  # 将 buffer_head 转换为 bio 并提交
              │
              └─ submit_bh_wbc(REQ_OP_READ, bh, hint, NULL)
                  │  # fs/buffer.c
                  │
                  ├─ 分配 bio: bio_alloc(bh->b_bdev, 1, REQ_OP_READ, GFP_NOIO)
                  │
                  ├─ 设置扇区号: bio->bi_iter.bi_sector = bh->b_blocknr * (bh->b_size >> 9)
                  │   # 将块号转换为 512 字节扇区号
                  │   # 例如: b_blocknr=100, b_size=4096 → sector=800
                  │
                  ├─ 绑定页面: bio_add_folio_nofail(bio, bh->b_folio, bh->b_size, bh_offset(bh))
                  │   # bio->bi_io_vec->bv_page 指向 bh->b_folio 的页面
                  │   # bv_offset = bh->b_data - page_address(page)
                  │
                  ├─ 设置完成回调: bio->bi_end_io = end_bio_bh_io_sync
                  │   # I/O 完成后回调 bh->b_end_io (即 end_buffer_async_read)
                  │
                  └─ blk_crypto_submit_bio(bio)
                      # 加密处理（如需要）后提交 bio 到块层

# I/O 完成回调
end_buffer_async_read(bh, uptodate)
  │  # 若 uptodate: set_buffer_uptodate(bh)
  │  # 若 !uptodate: clear_buffer_uptodate(bh)
  │
  ├─ unlock_buffer(bh)
  │   # 释放 BH_Lock 锁, 唤醒等待者
  │
  ├─ 检查同一 folio 内所有 bh 是否都完成
  │   # 使用 head->b_uptodate_lock 自旋锁保护
  │   # 遍历 b_this_page 链表检查所有 bh 的 BH_Lock 状态
  │
  └─ if (所有 bh 都已完成):
      │
      └─ folio_end_read(folio, success)
          # 标记 folio 为 uptodate, 解锁 folio
          # 唤醒 filemap_get_pages 中等待的进程
```

### 17.4 Page Cache 写路径

#### 17.4.1 完整调用栈

```text
# 文件写入时的 Page Cache 和 buffer_head 交互流程
#
# 数据流: 用户空间 → copy_from_user → 页缓存 folio → buffer_head 标记脏 → writeback 回写
# 关键: 写入先修改页缓存(标记脏); 回写由内核线程异步完成

write(fd, buf, count)
  │  # 用户空间系统调用
  │
  └─ ksys_write() → vfs_write()
      │
      └─ file->f_op->write_iter()
          │
          └─ generic_file_write_iter(iocb, iter)
              │  # mm/filemap.c
              │
              └─ __generic_file_write_iter(iocb, iter)
                  │
                  ├─ [Direct I/O 路径]
                  │   └─ generic_file_direct_write()
                  │       # 绕过页缓存, 直接构造 bio 提交
                  │
                  └─ [Buffered I/O 路径]
                      └─ generic_perform_write(iocb, iter)
                          │  # mm/filemap.c
                          │
                          └─ [for each folio in write range]:
                              │
                              ├─ a_ops->write_begin(file, mapping, pos, len, &folio, &fsdata)
                              │   │  # 文件系统回调, 准备要写入的 folio
                              │   │  # ext4: ext4_write_begin()
                              │   │
                              │   ├─ grab_cache_folio_write_begin(mapping, index, flags)
                              │   │   # 从页缓存获取或创建 folio
                              │   │   # 如果在页缓存中已存在: 直接返回
                              │   │   # 如果不存在: 分配新 folio, 加入 i_pages xarray
                              │   │
                              │   └─ 若 folio 非 uptodate:
                              │       └─ ext4_read_folio() → ext4_mpage_readpages()
                              │           # 从磁盘读取 folio 内容（部分页写入场景）
                              │
                              ├─ copy_folio_from_iter_atomic(folio, offset, bytes, iter)
                              │   │  # ★ 数据拷贝发生在这里 ★
                              │   │  # 从用户空间 iov_iter 拷贝数据到 folio 页面
                              │   │  # 内部调用 copy_from_user()
                              │   │  # 涉及 1 次 CPU 拷贝
                              │   │
                              │   └─ folio 内容被修改, 但尚未标记为脏
                              │
                              ├─ folio_flush_mapping()  (如果必要)
                              │   # 清除 folio 的私有数据（如旧的 buffer_head）
                              │
                              └─ a_ops->write_end(file, mapping, pos, len, copied, folio, fsdata)
                                  │  # 文件系统回调, 完成写入
                                  │  # ext4: ext4_write_end()
                                  │
                                  ├─ block_write_end(file, mapping, pos, len, copied, folio, fsdata)
                                  │   │  # fs/buffer.c
                                  │   │  # 更新 buffer_head 状态
                                  │   │
                                  │   ├─ __block_commit_write(inode, folio, from, to)
                                  │   │   │  # 标记修改范围内的 buffer_head 为脏
                                  │   │   │
                                  │   │   └─ [for each buffer_head in range]:
                                  │   │       │  # 遍历被修改的 buffer_head
                                  │   │       │
                                  │   │       ├─ set_buffer_uptodate(bh)
                                  │   │       │   # 缓冲区现在包含有效数据
                                  │   │       │
                                  │   │       └─ mark_buffer_dirty(bh)
                                  │   │           │  # ★ 标记 buffer_head 为脏 ★
                                  │   │           │  # 设置 BH_Dirty 标志
                                  │   │           │
                                  │   │           └─ __set_page_dirty(folio_page(folio, 0), mapping, 0)
                                  │   │               │  # 标记 folio 为脏
                                  │   │               │  # 将 folio 加入 mapping 的脏页链表
                                  │   │               │  # 设置 PAGECACHE_TAG_DIRTY 标签
                                  │   │               │
                                  │   │               └─ 将 inode 加入 bdi_writeback 的脏页列表
                                  │   │                   # 定期回写 (writeback) 将回写脏页
                                  │   │
                                  │   └─ folio_mark_uptodate(folio)
                                  │
                                  └─ folio_unlock(folio)
                                      # 解锁 folio, 写路径完成

# ═══════════════════════════════════════════════════════════════
# 异步回写路径 (writeback)
# ═══════════════════════════════════════════════════════════════

[内核回写线程 kworker/writeback]
  │  # 周期性唤醒, 或由 balance_dirty_pages() 触发
  │
  └─ wb_workfn() → wb_do_writeback()
      │
      └─ wb_writeback() → writeback_sb_inodes()
          │
          └─ __writeback_single_inode(inode, wbc)
              │
              └─ do_writepages(mapping, wbc)
                  │
                  └─ a_ops->writepages(mapping, wbc)
                      │  # ext4: ext4_writepages()
                      │  # fs/ext4/inode.c
                      │
                      └─ mpage_prepare_extent_to_map() → mpage_submit_folio()
                          │  # ext4 同样使用 mpage 机制, 直接构造 bio
                          │  # 不经过 buffer_head 的 submit_bh()
                          │
                          └─ bio_add_folio_nofail() → blk_crypto_submit_bio()

# ═══════════════════════════════════════════════════════════════
# buffer_head 写路径（传统文件系统: __block_write_full_folio）
# ═══════════════════════════════════════════════════════════════

__block_write_full_folio(inode, folio, get_block, wbc)
  │  # fs/buffer.c
  │  # 写回一个 folio 的所有脏 buffer_head
  │
  ├─ folio_create_buffers(folio, inode, BH_Dirty|BH_Uptodate)
  │   # 确保 folio 有 buffer_head 链表
  │
  ├─ [Phase 1: 映射所有脏 buffer_head]
  │   └─ [for each buffer_head]:
  │       │
  │       ├─ if (超出文件大小) → clear_buffer_dirty(bh)
  │       │
  │       └─ if (!buffer_mapped(bh) && buffer_dirty(bh)):
  │           └─ get_block(inode, block, bh, 1)
  │               # 分配磁盘块（如果需要）并返回物理块号
  │               # 例如: ext2_get_block() → ext2_alloc_branch()
  │
  ├─ [Phase 2: 锁定并标记异步写入]
  │   └─ [for each buffer_head]:
  │       │
  │       ├─ lock_buffer(bh)
  │       │   # 锁定 buffer_head, 防止并发修改
  │       │
  │       └─ if (test_clear_buffer_dirty(bh)):
  │           └─ mark_buffer_async_write_endio(bh, end_buffer_async_write)
  │               # 设置 BH_Async_Write, bh->b_end_io = end_buffer_async_write
  │
  ├─ folio_start_writeback(folio)
  │   # 标记 folio 为 writeback 状态
  │
  └─ [Phase 3: 提交所有 buffer_head]
      └─ [for each buffer_head]:
          └─ if (buffer_async_write(bh)):
              └─ submit_bh_wbc(REQ_OP_WRITE | write_flags, bh, hint, wbc)
                  │  # 每个 buffer_head 转为一个 bio 提交
                  │  # 注意: 这是一个 O(n) 的 bio 提交, 效率较低
                  │  # 这也是 ext4 转向 mpage 机制的原因之一
                  │
                  └─ ... (bio 提交, 与读路径相同)
```

### 17.5 buffer_head 到 bio 的转换

```text
# submit_bh_wbc: buffer_head → bio 的完整转换流程
# 位置: fs/buffer.c

submit_bh_wbc(REQ_OP_READ, bh, hint, wbc)
  │
  ├─ 状态检查
  │   # BUG_ON(!buffer_locked(bh))      ← 必须已锁定
  │   # BUG_ON(!buffer_mapped(bh))      ← 必须有磁盘映射
  │   # BUG_ON(!bh->b_end_io)           ← 必须有完成回调
  │   # BUG_ON(buffer_delay(bh))        ← 不能是延迟分配
  │
  ├─ 设置操作标志
  │   # if (buffer_meta(bh)) → opf |= REQ_META    ← 元数据 I/O 标记
  │   # if (buffer_prio(bh)) → opf |= REQ_PRIO    ← 优先级 I/O 标记
  │
  ├─ bio = bio_alloc(bh->b_bdev, 1, opf, GFP_NOIO)
  │   # 分配 bio, 最多 1 个 bio_vec
  │   # 一个 buffer_head 对应一个 bio
  │   # 注意: 每个 bh 提交一个 bio, 效率较低
  │   #  ext4 的 mpage 机制将多个连续块合并到一个 bio
  │
  ├─ 设置扇区号
  │   # bio->bi_iter.bi_sector = bh->b_blocknr * (bh->b_size >> 9)
  │   # 例如: b_blocknr=500, b_size=4096
  │   #   → bi_sector = 500 * 8 = 4000
  │   # 块号乘以 (块大小/512) 转换为 512 字节扇区号
  │
  ├─ 绑定页面到 bio
  │   # bio_add_folio_nofail(bio, bh->b_folio, bh->b_size, bh_offset(bh))
  │   # 设置 bio->bi_io_vec[0]:
  │   #   bv_page   = folio_page(bh->b_folio, 0)  ← 页面指针
  │   #   bv_offset = bh_offset(bh)               ← 页内偏移
  │   #   bv_len    = bh->b_size                  ← 块大小
  │
  ├─ 设置完成回调
  │   # bio->bi_end_io = end_bio_bh_io_sync
  │   # bio->bi_private = bh
  │   # I/O 完成后: end_bio_bh_io_sync(bio) → bh->b_end_io(bh, uptodate)
  │
  ├─ guard_bio_eod(bio)
  │   # 检查 bio 是否超出设备边界, 如有需要则截断
  │
  ├─ [如果 wbc 非 NULL]
  │   # wbc_init_bio(wbc, bio)           ← 关联 cgroup 写回控制
  │   # wbc_account_cgroup_owner(wbc, ...) ← 统计 cgroup 写回量
  │
  └─ blk_crypto_submit_bio(bio)
      # 内联加密处理（如需要）后提交到块层
      # 最终调用 submit_bio_noacct() → blk_mq_submit_bio()
```

### 17.6 块设备自身的 Page Cache（bdev mapping）

块设备也有自己的 `address_space`（`bdev->bd_mapping`），用于缓存块设备级别的元数据（如 superblock、bitmap）以及通过 `dd` 等直接访问时的数据：

```text
# 块设备 address_space 的初始化
# block/bdev.c

bdev_alloc_inode(sb)
  │
  ├─ inode = alloc_inode_sb(sb, bdev_inode_cachep, GFP_KERNEL)
  │
  └─ inode->i_data.a_ops = &def_blk_aops
      # 设置块设备的 a_ops
      # def_blk_aops 定义在 block/fops.c 中
      # 包含 read_folio, writepages 等操作
```

**块设备与文件系统 address_space 的区别**：

| 类型 | 所属 | 用途 | a_ops |
|------|------|------|-------|
| 文件 address_space | `inode->i_mapping` | 缓存文件数据 | ext4_aops / ext2_aops 等 |
| 块设备 address_space | `bdev->bd_mapping` | 缓存块设备 raw 数据 | def_blk_aops |

**`__find_get_block_slow()`**（[buffer.c](file:///home/louis/code/linux/fs/buffer.c)）在块设备 address_space 中查找特定块号的 buffer_head：

```c
// 通过 bdev->bd_mapping->i_pages 查找块号对应的 folio
// 然后在 folio 的 buffer_head 链表中查找匹配的 bh
// 如果找到 → 增加引用计数返回
// 如果未找到 → 返回 NULL
```

### 17.7 关键数据流对比

```text
# ═══════════════════════════════════════════════════════════════
# 路径 A: ext4 mpage 读（绕过 buffer_head, 直接 bio）
# ═══════════════════════════════════════════════════════════════

filemap_read()
  └─ filemap_get_pages() → 页缓存未命中
      └─ filemap_read_folio() → ext4_read_folio()
          └─ ext4_mpage_readpages()
              ├─ ext4_map_blocks()  ← 逻辑块→物理块映射
              └─ bio_add_folio_nofail() + submit_bio()
                  # ★ 一个 bio 可包含多个连续块, 高效 ★

# ═══════════════════════════════════════════════════════════════
# 路径 B: buffer_head 读（传统文件系统, 每个 bh 一个 bio）
# ═══════════════════════════════════════════════════════════════

filemap_read()
  └─ filemap_get_pages() → 页缓存未命中
      └─ filemap_read_folio() → block_read_full_folio()
          ├─ folio_create_buffers()  ← 切分 folio 为 buffer_head 链表
          ├─ get_block() × N         ← 每个 bh 获取块映射
          └─ submit_bh() × N         ← 每个 bh 提交一个 bio
              # ★ 每个 bh 一个 bio, 碎片化, 效率较低 ★

# ═══════════════════════════════════════════════════════════════
# 路径 C: 直接 I/O（绕过 Page Cache）
# ═══════════════════════════════════════════════════════════════

generic_file_direct_write()
  └─ a_ops->direct_IO()
      └─ 直接构造 bio, 数据从用户页面直接到块设备
          # ★ 完全不经过页缓存和 buffer_head ★
```

### 17.8 总结

**Page Cache 的核心作用**：
1. **缓存层**：通过 `address_space.i_pages`（xarray）管理文件的所有缓存页面，命中时避免磁盘 I/O
2. **统一接口**：通过 `address_space_operations` 将文件系统与页缓存解耦，不同的文件系统只需实现自己的 `a_ops`
3. **回写管理**：通过 `PAGECACHE_TAG_DIRTY` 标签追踪脏页，由 writeback 机制异步回写
4. **预读优化**：通过 `readahead` 机制提前加载后续页面，减少 I/O 等待

**buffer_head 的核心作用**：
1. **块映射**：将页面内的每个块（block）与磁盘物理块号建立映射（`b_blocknr`）
2. **状态追踪**：通过 `BH_Uptodate`、`BH_Dirty`、`BH_Lock` 等标志管理每个块的状态
3. **I/O 提交**：通过 `submit_bh()` 将 buffer_head 转换为 bio 提交给块层
4. **向后兼容**：为传统文件系统（ext2、FAT 等）提供块 I/O 接口

**ext4 的优化**：ext4 使用 `mpage` 机制，在 `ext4_mpage_readpages()` 中直接构造 bio，将多个连续块合并到一个 bio 中，绕过了 buffer_head 的 `submit_bh()` 路径，显著提高了 I/O 效率。

---

## 18. Writeback 回写机制 — Flush 线程下刷数据流程

### 18.1 概述

Buffer Write 场景下，用户态 `write()` 系统调用仅将数据从用户空间拷贝到 Page Cache 并标记脏页，**实际的磁盘 I/O 由 Flush 线程（Writeback Flusher Thread）异步完成**。Flush 线程是内核中负责将脏页写回磁盘的核心机制，本节分析其完整的工作流程。

**Flush 线程在 I/O 栈中的位置**：

```
用户空间 write(fd, buf, count)
  │
  ▼
VFS / Page Cache 层
  │  └─ 数据写入页缓存，标记脏页
  │  └─ balance_dirty_pages() 限流控制
  │
  ▼
Flush 线程 (wb_workfn)            ← 本节分析的核心
  │  └─ wb_do_writeback()
  │      └─ wb_writeback()
  │          └─ writeback_sb_inodes()
  │              └─ __writeback_single_inode()
  │                  └─ do_writepages()
  │                      └─ a_ops->writepages()  ← 文件系统回调
  │
  ▼
Buffer Head 层 / Bio 层
  │  └─ submit_bh() / mpage_writepages()
  │      └─ submit_bio()
  │
  ▼
块层 (blk-mq)
  └─ blk_mq_submit_bio() → ... → 驱动
```

**涉及的核心文件**：

| 文件 | 行数 | 功能 |
|------|------|------|
| `fs/fs-writeback.c` | 2,994 | Flush 线程主实现：work 管理、inode 回写 |
| `mm/page-writeback.c` | 2,500+ | 脏页比率控制、`do_writepages()`、`balance_dirty_pages()` |
| `fs/buffer.c` | 3,500+ | `submit_bh()` — buffer_head 转 bio |
| `fs/mpage.c` | 500+ | `mpage_writepages()` — 批量回写 |
| `include/linux/writeback.h` | 378 | `struct writeback_control`、`struct wb_domain` |
| `include/linux/backing-dev-defs.h` | 280+ | `struct bdi_writeback`、`struct backing_dev_info` |

### 18.2 核心数据结构

#### 18.2.1 `struct bdi_writeback` — 回写设备管理器

（[backing-dev-defs.h](file:///home/louis/code/linux/include/linux/backing-dev-defs.h)）每个块设备（`backing_dev_info`）关联一个 `bdi_writeback`，管理该设备的所有回写操作：

```c
struct bdi_writeback {
    struct backing_dev_info *bdi;       /* 所属 backing device */

    unsigned long state;                 /* WB_* 状态位（原子位操作） */
    unsigned long last_old_flush;        /* 上次老旧数据刷新时间 */

    /* Inode 链表 — 按脏页状态分类 */
    struct list_head b_dirty;            /* 有脏页的 inode（待回写） */
    struct list_head b_io;               /* 正在回写的 inode */
    struct list_head b_more_io;          /* 回写未完成的 inode（需要更多 I/O） */
    struct list_head b_dirty_time;       /* 时间戳脏的 inode */
    spinlock_t list_lock;                /* 保护 b_* 链表 */

    atomic_t writeback_inodes;           /* 正在回写的 inode 数 */
    struct percpu_counter stat[NR_WB_STAT_ITEMS]; /* 统计：WB_RECLAIMABLE/WB_WRITEBACK 等 */

    /* 带宽估算 */
    unsigned long bw_time_stamp;         /* 上次带宽更新的时间戳 */
    unsigned long dirtied_stamp;
    unsigned long written_stamp;
    unsigned long write_bandwidth;       /* 估算的写带宽 */
    unsigned long avg_write_bandwidth;   /* 平滑后的写带宽 */
    unsigned long dirty_ratelimit;       /* 脏页速率限制 */
    unsigned long balanced_dirty_ratelimit; /* 平衡脏页速率 */

    struct fprop_local_percpu completions; /* 写完成比例统计 */
    int dirty_exceeded;                  /* 是否超过脏页阈值 */
    enum wb_reason start_all_reason;     /* 全量回写的触发原因 */

    /* Work 管理 */
    spinlock_t work_lock;                /* 保护 work_list 和 dwork 调度 */
    struct list_head work_list;          /* 待处理的回写工作链表 */
    struct delayed_work dwork;           /* 延迟工作项（Flush 线程入口） */
    struct delayed_work bw_dwork;        /* 带宽估算更新工作项 */
};
```

**`b_dirty` / `b_io` / `b_more_io` 三链表的作用**：

```
   b_dirty                     b_io                      b_more_io
  ┌──────────┐             ┌──────────┐              ┌──────────┐
  │ inode_A  │  queue_io   │ inode_A  │  writeback   │ inode_A  │
  │ inode_B  │  ───────→   │ inode_B  │  ───────→    │ inode_B  │ ← 部分回写完成
  │ inode_C  │             │ inode_C  │              │          │   但仍有脏页
  └──────────┘             └──────────┘              └──────────┘
    脏页 inode               正在/即将回写               需要更多 I/O
```

- **`b_dirty`**：所有脏 inode 的候诊队列，等待被调度
- **`b_io`**：当前批次正在回写的 inode，`queue_io()` 从 `b_dirty` 搬入
- **`b_more_io`**：某 inode 回写完一批后仍有脏页，搬入此链表继续下一轮

**`wb->state` 状态位**：

| 标志 | 含义 |
|------|------|
| `WB_registered` | `bdi_register()` 已完成 |
| `WB_writeback_running` | 正在执行回写 |
| `WB_has_dirty_io` | `b_{dirty\|io\|more_io}` 上有脏 inode |
| `WB_start_all` | 全量回写请求已发出（防止重复入队） |

#### 18.2.2 `struct wb_writeback_work` — 回写工作项

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）表示一个具体的回写任务，由 Flush 线程从 `wb->work_list` 取出执行：

```c
struct wb_writeback_work {
    long nr_pages;                       /* 需要回写的页数 */
    struct super_block *sb;              /* 目标超级块（NULL=所有） */
    enum writeback_sync_modes sync_mode; /* WB_SYNC_NONE / WB_SYNC_ALL */
    unsigned int tagged_writepages:1;    /* 使用 tag-and-write 避免活锁 */
    unsigned int for_kupdate:1;          /* 周期性老旧数据回写 */
    unsigned int range_cyclic:1;         /* 循环扫描范围 */
    unsigned int for_background:1;       /* 后台回写（超过 bg_thresh） */
    unsigned int for_sync:1;             /* sync(2) WB_SYNC_ALL 回写 */
    unsigned int auto_free:1;            /* 完成后自动 kfree */
    enum wb_reason reason;               /* 触发原因 */

    struct list_head list;               /* 链接到 wb->work_list */
    struct wb_completion *done;          /* 调用者等待完成 */
};
```

**`sync_mode` 的区别**：

| 模式 | 含义 | 行为 |
|------|------|------|
| `WB_SYNC_NONE` | 异步回写 | 不等待 I/O 完成，尽可能多写页面 |
| `WB_SYNC_ALL` | 同步回写 | 等待每次 I/O 完成，`sync()` 系统调用使用 |

**`reason` 枚举**（触发原因）：

| 原因 | 触发场景 |
|------|---------|
| `WB_REASON_BACKGROUND` | `balance_dirty_pages()` 检测到超过后台阈值 |
| `WB_REASON_VMSCAN` | 内存回收需要释放脏页 |
| `WB_REASON_SYNC` | `sync()` / `fsync()` 系统调用 |
| `WB_REASON_PERIODIC` | 定时器到期（`dirty_writeback_interval`） |
| `WB_REASON_FS_FREE_SPACE` | 文件系统剩余空间不足 |
| `WB_REASON_FOREIGN_FLUSH` | Cgroup 写回中检测到外来源 inode |

#### 18.2.3 `struct writeback_control` — 回写控制参数

（[writeback.h](file:///home/louis/code/linux/include/linux/writeback.h)）传递给文件系统 `->writepages()` 回调的控制参数，通常分配在栈上：

```c
struct writeback_control {
    long nr_to_write;                    /* 还需写多少页 */
    long pages_skipped;                  /* 跳过的页数 */
    loff_t range_start;                  /* 写回范围起始偏移 */
    loff_t range_end;                    /* 写回范围结束偏移（包含） */
    enum writeback_sync_modes sync_mode; /* WB_SYNC_NONE / WB_SYNC_ALL */

    unsigned int for_kupdate:1;          /* 周期性回写 */
    unsigned int for_background:1;       /* 后台回写 */
    unsigned int tagged_writepages:1;    /* tag-and-write 模式 */
    unsigned int range_cyclic:1;         /* 循环扫描 */
    unsigned int for_sync:1;             /* sync(2) 同步回写 */
    unsigned int unpinned_netfs_wb:1;    /* 已清除 I_PINNING_NETFS_WB */

    /* 内部字段 */
    struct folio_batch fbatch;           /* folio 批量处理 */
    pgoff_t index;                       /* 当前扫描索引 */
    int saved_err;                       /* 保存的错误码 */

#ifdef CONFIG_CGROUP_WRITEBACK
    struct bdi_writeback *wb;            /* 当前回写所属的 wb */
    struct inode *inode;                 /* 正在回写的 inode */
    /* 外来源 inode 检测 */
    int wb_id;                           /* 当前 wb id */
    int wb_lcand_id;                     /* 上一个候选 wb id */
    int wb_tcand_id;                     /* 当前候选 wb id */
    size_t wb_bytes;                     /* 当前 wb 写入字节数 */
    size_t wb_lcand_bytes;
    size_t wb_tcand_bytes;
#endif
};
```

**`wbc_to_write_flags()`** 将 `sync_mode` 映射为 bio 的 `REQ_*` 标志：

```c
static inline blk_opf_t wbc_to_write_flags(struct writeback_control *wbc)
{
    blk_opf_t flags = 0;
    if (wbc->sync_mode == WB_SYNC_ALL)
        flags |= REQ_SYNC;                  /* 同步写：高优先级 */
    else if (wbc->for_kupdate || wbc->for_background)
        flags |= REQ_BACKGROUND;            /* 后台写：低优先级 */
    return flags;
}
```

#### 18.2.4 `struct backing_dev_info` — 后备设备信息

（[backing-dev-defs.h](file:///home/louis/code/linux/include/linux/backing-dev-defs.h)）每个块设备对应一个 `backing_dev_info`，描述其回写能力：

```c
struct backing_dev_info {
    u64 id;                                /* 唯一标识符 */
    struct rb_node rb_node;                /* 按 id 排序的红黑树节点 */
    struct list_head bdi_list;             /* 全局 bdi 链表 */
    unsigned long ra_pages;                /* 最大预读页数 */
    unsigned long io_pages;                /* 最大 I/O 页数 */
    struct kref refcnt;
    unsigned int capabilities;             /* BDI_CAP_* 能力标志 */
    unsigned int min_ratio;                /* 最小脏页比率 */
    unsigned int max_ratio;                /* 最大脏页比率 */
    atomic_long_t tot_write_bandwidth;     /* 所有 wb 的写带宽总和 */
    unsigned long last_bdp_sleep;          /* 上次限流睡眠时间 */
    struct bdi_writeback wb;              /* 根 wb（内嵌） */
    struct list_head wb_list;              /* 所有 wb 链表（含 cgroup wb） */
    wait_queue_head_t wb_waitq;            /* 等待队列 */
};
```

**BDI_CAP 能力标志**：

| 标志 | 含义 |
|------|------|
| `BDI_CAP_WRITEBACK` | 支持回写（大多数块设备） |
| `BDI_CAP_WRITEBACK_ACCT` | 支持回写统计 |
| `BDI_CAP_STRICTLIMIT` | 严格限制（不借用其他 BDI 的额度） |

### 18.3 Flush 线程的创建与初始化

#### 18.3.1 创建时机

Flush 线程在设备注册时创建，通过 `bdi_alloc()` 初始化 `bdi_writeback`，然后通过 `bdi_register()` 将 `wb->dwork` 绑定到全局 workqueue `bdi_wq`。

**调用链**：

```
设备驱动注册块设备
  └─ device_add_disk()
      └─ blk_register_queue()
          └─ bdi_register()                   ← 注册 backing device
              └─ bdi_register_va()
                  └─ wb_init(wb, bdi, GFP_KERNEL)  ← 初始化 bdi_writeback
                      │
                      ├─ spin_lock_init(&wb->work_lock)
                      ├─ INIT_LIST_HEAD(&wb->work_list)
                      ├─ INIT_DELAYED_WORK(&wb->dwork, wb_workfn)  ← 绑定工作函数
                      ├─ INIT_DELAYED_WORK(&wb->bw_dwork, wb_update_bandwidth_workfn)
                      ├─ percpu_counter_init()  ← 初始化统计计数器
                      │
                      └─ fprop_local_init_percpu()  ← 初始化比例统计
```

**关键点**：Flush 线程不是一个独立的 `kthread`，而是**通过工作队列（workqueue）** 实现的延迟工作项 `wb->dwork`。每次有回写需求时，通过 `mod_delayed_work(bdi_wq, &wb->dwork, delay)` 调度执行。

#### 18.3.2 全局 workqueue `bdi_wq`

`bdi_wq` 在系统初始化时创建，所有块设备的 Flush 线程共享此工作队列：

```c
// mm/backing-dev.c
static int __init default_bdi_init(void)
{
    bdi_wq = alloc_workqueue("writeback", WQ_MEM_RECLAIM | WQ_FREEZABLE | WQ_SYSFS,
                             WQ_UNBOUND_MAX_ACTIVE);
    // WQ_MEM_RECLAIM: 内存回收路径可用（避免死锁）
    // WQ_FREEZABLE: 系统休眠时冻结
    // WQ_UNBOUND: 不限 CPU，可迁移
    // WQ_UNBOUND_MAX_ACTIVE: 最大活跃工作数 = CPU 数
    ...
}
```

### 18.4 Work 入队流程

#### 18.4.1 `wb_queue_work()` — 入队回写工作

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）

```c
static void wb_queue_work(struct bdi_writeback *wb,
                          struct wb_writeback_work *work)
{
    trace_writeback_queue(wb, work);

    if (work->done)
        atomic_inc(&work->done->cnt);       /* 增加完成计数器 */

    spin_lock_irq(&wb->work_lock);

    if (test_bit(WB_registered, &wb->state)) {
        list_add_tail(&work->list, &wb->work_list);  /* 加入工作链表尾部 */
        mod_delayed_work(bdi_wq, &wb->dwork, 0);      /* 立即调度 Flush 线程 */
    } else
        finish_writeback_work(work);                   /* 设备已注销，直接完成 */

    spin_unlock_irq(&wb->work_lock);
}
```

**流程**：
1. 将 `work` 加入 `wb->work_list` 链表尾部（FIFO 顺序）
2. 调用 `mod_delayed_work(bdi_wq, &wb->dwork, 0)` 立即唤醒 Flush 线程
3. 如果设备已注销，直接调用 `finish_writeback_work()` 完成工作

#### 18.4.2 触发回写的 5 种路径

**① 后台回写（Background Writeback）** — 通过 `balance_dirty_pages()` 触发：

```
write() → generic_perform_write()
  → 写入页缓存，标记脏页
  → balance_dirty_pages_ratelimited(mapping)  ← 速率限制的脏页检查
    → balance_dirty_pages(bdi, dirty_thresh)
      → 如果 wb_dirty > wb_thresh（超过脏页阈值）
        → wb_start_background_writeback(wb)  ← 唤醒 Flush 线程
          → wb_wakeup(wb)
            → mod_delayed_work(bdi_wq, &wb->dwork, 0)
```

**② 周期性回写（Periodic / Kupdate）** — 定时器触发：

```c
// 默认 dirty_writeback_interval = 500 厘秒 = 5 秒
// wb_workfn() 执行完毕后：
if (!list_empty(&wb->work_list))
    wb_wakeup(wb);                              /* 还有工作，立即唤醒 */
else if (wb_has_dirty_io(wb) && dirty_writeback_interval)
    wb_wakeup_delayed(wb);                      /* 5 秒后再次检查 */
    // → queue_delayed_work(bdi_wq, &wb->dwork, timeout)
```

**③ 同步回写（Sync / Fsync）** — 直接入队并等待完成：

```c
sync_inodes_sb(sb)
  → 遍历所有 BDI，对每个 wb：
    DEFINE_WB_COMPLETION(done, bdi);           /* 定义完成标记 */
    struct wb_writeback_work work = {
        .sb       = sb,
        .sync_mode = WB_SYNC_ALL,
        .nr_pages  = LONG_MAX,
        .for_sync  = 1,
        .done      = &done,                    /* 设置完成通知 */
    };
    wb_queue_work(wb, &work);                  /* 入队 */
    wb_wait_for_completion(&done);             /* 等待回写完成 */
```

**④ 内存回收触发（Vmscan）** — 页面回收需要先写回脏页：

```
shrink_folio_list() 或 shrink_inactive_list()
  → pageout(folio, mapping)                   /* 尝试换出脏页 */
    → mapping->a_ops->writepages(mapping, wbc) /* 直接触发回写 */
    // 或
  → wakeup_flusher_threads(WB_REASON_VMSCAN)  /* 异步唤醒 Flush 线程 */
    → wb_start_writeback(wb, reason)
      → wb_wakeup(wb)
```

**⑤ 文件系统主动触发** — 空间不足等情况：

```c
writeback_inodes_sb(sb, WB_REASON_FS_FREE_SPACE);
writeback_inodes_sb_nr(sb, nr, WB_REASON_FS_FREE_SPACE);
```

### 18.5 Flush 线程主循环

#### 18.5.1 `wb_workfn()` — Flush 线程入口

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）这是 Flush 线程的核心工作函数，每次 `wb->dwork` 被调度时执行：

```c
void wb_workfn(struct work_struct *work)
{
    struct bdi_writeback *wb = container_of(to_delayed_work(work),
                                            struct bdi_writeback, dwork);
    long pages_written;

    set_worker_desc("flush-%s", bdi_dev_name(wb->bdi));

    if (likely(!current_is_workqueue_rescuer() ||
               !test_bit(WB_registered, &wb->state))) {
        /* 正常路径：持续回写直到 work_list 为空 */
        do {
            pages_written = wb_do_writeback(wb);
            trace_writeback_pages_written(pages_written);
        } while (!list_empty(&wb->work_list));
    } else {
        /* 紧急救援路径：workqueue 工作线程不足，不能独占总资源 */
        pages_written = writeback_inodes_wb(wb, 1024,
                                            WB_REASON_FORKER_THREAD);
        trace_writeback_pages_written(pages_written);
    }

    /* 结束后检查是否需要再次调度 */
    if (!list_empty(&wb->work_list))
        wb_wakeup(wb);                                   /* 还有 work，立即唤醒 */
    else if (wb_has_dirty_io(wb) && dirty_writeback_interval)
        wb_wakeup_delayed(wb);                           /* 有脏页，延迟唤醒 */
}
```

**关键逻辑**：
1. 循环调用 `wb_do_writeback()` 处理 `work_list` 中所有 work 项
2. 如果是在 rescue 模式（workqueue 线程不足），只写 1024 页就退出，避免饿死其他 work 项
3. 结束后根据是否有残留 work 或脏页，决定是否重新调度

#### 18.5.2 `wb_do_writeback()` — 分发回写任务

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）

```c
static long wb_do_writeback(struct bdi_writeback *wb)
{
    struct wb_writeback_work *work;
    long wrote = 0;

    set_bit(WB_writeback_running, &wb->state);           /* 标记回写进行中 */

    /* 1. 处理 work_list 中的显式 work 项 */
    while ((work = get_next_work_item(wb)) != NULL) {
        trace_writeback_exec(wb, work);
        wrote += wb_writeback(wb, work);                 /* 执行回写 */
        finish_writeback_work(work);                     /* 完成通知 */
    }

    /* 2. 检查全量回写请求 */
    wrote += wb_check_start_all(wb);

    /* 3. 检查周期性老旧数据回写 */
    wrote += wb_check_old_data_flush(wb);

    /* 4. 检查后台回写（超过 bg_thresh） */
    wrote += wb_check_background_flush(wb);

    clear_bit(WB_writeback_running, &wb->state);
    return wrote;
}
```

**执行顺序**：
1. **显式 work**（通过 `wb_queue_work()` 入队的）优先级最高
2. **全量回写**（`wb_start_writeback()` 标记 `WB_start_all` 的）
3. **周期性回写**（`wb_check_old_data_flush()` — 检查老旧数据）
4. **后台回写**（`wb_check_background_flush()` — 检查是否超过后台阈值）

#### 18.5.3 `wb_writeback()` — 核心回写循环

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）

```c
static long wb_writeback(struct bdi_writeback *wb,
                         struct wb_writeback_work *work)
{
    long nr_pages = work->nr_pages;
    unsigned long dirtied_before = jiffies;
    struct blk_plug plug;
    bool queued = false;

    blk_start_plug(&plug);                               /* 开启批量提交优化 */

    for (;;) {
        if (work->nr_pages <= 0)                         /* 页数已写够 */
            break;

        /* 后台/周期性回写遇到其他 work，让出 CPU */
        if ((work->for_background || work->for_kupdate) &&
            !list_empty(&wb->work_list))
            break;

        /* 后台回写：低于阈值即停止 */
        if (work->for_background && !wb_over_bg_thresh(wb))
            break;

        spin_lock(&wb->list_lock);
        trace_writeback_start(wb, work);

        /* 如果 b_io 为空，从 b_dirty 搬入 */
        if (list_empty(&wb->b_io)) {
            if (work->for_kupdate)
                dirtied_before = jiffies -
                    msecs_to_jiffies(dirty_expire_interval * 10);
            else if (work->for_background)
                dirtied_before = jiffies;
            queue_io(wb, work, dirtied_before);          /* b_dirty → b_io */
            queued = true;
        }

        /* 回写 b_io 中的 inode */
        if (work->sb)
            progress = writeback_sb_inodes(work->sb, wb, work);
        else
            progress = __writeback_inodes_wb(wb, work);

        trace_writeback_written(wb, work);

        if (progress || !queued) {
            spin_unlock(&wb->list_lock);
            continue;
        }

        if (list_empty(&wb->b_more_io)) {                /* 没有更多 I/O 了 */
            spin_unlock(&wb->list_lock);
            break;
        }

        /* 写了 0 进度，等待 inode 完成 */
        trace_writeback_wait(wb, work);
        inode = wb_inode(wb->b_more_io.prev);
        spin_lock(&inode->i_lock);
        spin_unlock(&wb->list_lock);
        inode_sleep_on_writeback(inode);                 /* 睡眠等待 I/O 完成 */
    }

    blk_finish_plug(&plug);                              /* 刷新 plug，批量提交 */
    return nr_pages - work->nr_pages;
}
```

**核心循环逻辑**：

```
                  ┌──────────────────────────────┐
                  │  开始回写循环                   │
                  └──────────────┬───────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  work->nr_pages <= 0?   │──→ 退出（已写完目标页数）
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  有其他 work 等待?       │──→ 退出（让出 CPU）
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  后台回写低于阈值?        │──→ 退出（已完成任务）
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  b_io 为空?             │──→ queue_io() 从 b_dirty 搬入
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  writeback_sb_inodes()   │  ← 实际回写 inode 的脏页
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  有进度 or 首次入队?      │──→ 继续循环
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  b_more_io 为空?         │──→ 退出
                   └────────────┬────────────┘
                                │
                   ┌────────────▼────────────┐
                   │  inode_sleep_on_writeback│  ← 等待 I/O 完成
                   └────────────┬────────────┘
                                │
                                └──→ 继续循环
```

### 18.6 Inode 回写路径

#### 18.6.1 `writeback_sb_inodes()` — 遍历 inode 链表

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）遍历 `wb->b_io` 链表，对每个 inode 调用 `__writeback_single_inode()`：

```c
static long writeback_sb_inodes(struct super_block *sb,
                                struct bdi_writeback *wb,
                                struct wb_writeback_work *work)
{
    while (!list_empty(&wb->b_io)) {
        struct inode *inode = wb_inode(wb->b_io.prev);
        struct super_block *inode_sb = inode->i_sb;

        if (inode_sb != sb)              /* 跳过不属于目标 super_block 的 inode */
            redirty_tail(inode, wb);
            continue;

        if (!sb_is_blkdev_sb(inode_sb)) {
            if (!super_trylock_shared(inode_sb)) {  /* 尝试获取 s_umount 读锁 */
                redirty_tail(inode, wb);            /* 获取失败，重试 */
                continue;
            }
        }

        /* 提取 inode 并调用 __writeback_single_inode */
        __writeback_single_inode(inode, &wbc);

        /* 进度检查：每 100ms 或写够后退出循环 */
        if (wrote) {
            if (time_is_before_jiffies(start_time + HZ / 10UL))
                break;
            if (work->nr_pages <= 0)
                break;
        }
    }
    return wrote;
}
```

**关键点**：
- 使用 `super_trylock_shared()` 获取 `s_umount` 读锁（非阻塞），避免与 `umount` 竞争
- 每 100ms 或写够目标页数后退出，让出 CPU
- 如果 `inode_sb != work->sb`，将 inode 重新放回 `b_dirty` 尾部

#### 18.6.2 `__writeback_single_inode()` — 单个 inode 回写

（[fs-writeback.c](file:///home/louis/code/linux/fs/fs-writeback.c)）对单个 inode 的脏页和元数据执行回写：

```c
static int __writeback_single_inode(struct inode *inode,
                                    struct writeback_control *wbc)
{
    struct address_space *mapping = inode->i_mapping;
    long nr_to_write;
    int ret;

    spin_lock(&inode->i_lock);

    if (!(inode_state_read(inode) & I_DIRTY_ALL))    /* 没有脏数据 */
        goto out_unlock_inode;

    if (inode_state_read(inode) & I_SYNC) {           /* 已有回写在进行 */
        if (wbc->sync_mode != WB_SYNC_ALL)
            goto out_unlock_inode;                    /* 异步模式：跳过 */
        inode_wait_for_writeback(inode);              /* 同步模式：等待 */
    }

    inode->i_state |= I_SYNC;                         /* 标记 I_SYNC 防止并发 */
    spin_unlock(&inode->i_lock);

    write_chunk = writeback_chunk_size(inode->i_sb, wb, work);
    wbc->nr_to_write = write_chunk;

    /*
     * 1. 回写脏页（数据）
     */
    ret = do_writepages(mapping, wbc);                /* ★ 核心：写回脏页 ★ */

    /*
     * 2. 回写 inode 元数据（时间戳、大小等）
     */
    if (need_resched()) {
        blk_flush_plug(current->plug, false);
        cond_resched();
    }
    write_inode(inode, wbc);                          /* 调用 s_op->write_inode() */

    /*
     * 3. 更新 inode 状态
     */
    spin_lock(&inode->i_lock);
    inode->i_state &= ~I_SYNC;                        /* 清除 I_SYNC */

    if (!(inode_state_read(inode) & I_DIRTY_ALL)) {
        /* 完全干净：从回写链表移除 */
        list_del_init(&inode->i_io_list);
        inode->i_state &= ~I_DIRTY_ALL;
        ...
    } else {
        /* 仍有脏页：搬入 b_more_io 等待下一轮 */
        requeue_inode(inode, wb, wbc, dirtied_before);
    }
    ...
}
```

**回写顺序**：
1. **先写数据**（`do_writepages` → 文件系统 `->writepages`）
2. **再写元数据**（`write_inode` → `s_op->write_inode`）
3. **更新状态**（清除 `I_SYNC`，如果仍有脏页则搬入 `b_more_io`）

#### 18.6.3 `do_writepages()` — 写回脏页到块层

（[mm/page-writeback.c](file:///home/louis/code/linux/mm/page-writeback.c)）调用文件系统注册的 `->writepages` 回调，将脏页批量写入块层：

```c
int do_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
    int ret;
    struct bdi_writeback *wb;

    if (wbc->nr_to_write <= 0)
        return 0;

    wb = inode_to_wb_wbc(mapping->host, wbc);
    wb_bandwidth_estimate_start(wb);

    while (1) {
        if (mapping->a_ops->writepages)
            ret = mapping->a_ops->writepages(mapping, wbc);  /* 文件系统回调 */
        else
            ret = 0;                                         /* 无回调，跳过 */

        if (ret != -ENOMEM || wbc->sync_mode != WB_SYNC_ALL)
            break;

        reclaim_throttle(NODE_DATA(numa_node_id()), VMSCAN_THROTTLE_WRITEBACK);
    }

    if (time_is_before_jiffies(READ_ONCE(wb->bw_time_stamp) + BANDWIDTH_INTERVAL))
        wb_update_bandwidth(wb);                              /* 更新带宽估算 */

    return ret;
}
```

**关键点**：
- `mapping->a_ops->writepages` 是文件系统注册的回写函数
- 如果 `ENOMEM` 且是 `WB_SYNC_ALL` 模式，会节流重试
- 写完后更新带宽估算值（用于 `balance_dirty_pages` 限流）

### 18.7 文件系统回写实现

#### 18.7.1 ext4 的 `writepages` 路径

（[fs/ext4/inode.c](file:///home/louis/code/linux/fs/ext4/inode.c)）ext4 使用 `ext4_writepages()`，通过 `mpage` 机制将脏页转换为 bio 提交：

```
ext4_writepages(mapping, wbc)
  │
  ├─ ext4_do_writepages(wbc, mpd)              ← 核心回写函数
  │   │
  │   ├─ 循环遍历脏页：
  │   │   │
  │   │   ├─ mpage_add_bh_to_extent(mpd, ...)  ← 将 buffer_head 添加到 extent
  │   │   │   │ 如果当前 extent 已满或不连续：
  │   │   │   │   → mpage_submit_extent(mpd)   ← 提交当前 extent
  │   │   │   │     → ext4_mpage_write_folio() 或 submit_bh()
  │   │   │   │
  │   │   │   └─ 否则：合并到当前 extent
  │   │   │
  │   │   └─ writeback_iter(mapping, wbc, ...)  ← 遍历脏页
  │   │
  │   └─ mpage_submit_extent(mpd)              ← 提交最后一个 extent
  │
  └─ ext4_io_submit(&mpd.io_submit)            ← 提交所有 bio
```

**ext4 的两种回写路径**：

```
ext4 回写
  │
  ├─ 路径 A：mpage 批量提交（推荐）
  │   ext4_mpage_write_folio()
  │     → 将连续块合并到一个 bio
  │     → 直接调用 submit_bio()
  │
  └─ 路径 B：buffer_head 逐个提交（回退）
      mpage_add_bh_to_extent() 失败时
        → submit_bh(REQ_OP_WRITE, bh)
          → submit_bh_wbc()  ← 每个 bh 一个 bio
            → bio_alloc() + submit_bio()
```

#### 18.7.2 ext2 的 `writepages` 路径（Buffer Head 方式）

（[fs/ext2/inode.c](file:///home/louis/code/linux/fs/ext2/inode.c)）ext2 使用 `mpage_writepages()`，通过 `__mpage_writepages()` 批量提交：

```c
// ext2 的 a_ops
const struct address_space_operations ext2_aops = {
    .writepages     = ext2_writepages,          // → mpage_writepages()
    .dirty_folio    = block_dirty_folio,        // 标记 bh 为脏
    ...
};

// mpage_writepages 内部
int __mpage_writepages(struct address_space *mapping,
                       struct writeback_control *wbc,
                       get_block_t get_block,
                       int (*write_folio)(struct folio *, struct writeback_control *))
{
    struct mpage_data mpd = { .get_block = get_block };
    struct folio *folio = NULL;
    struct blk_plug plug;
    int error;

    blk_start_plug(&plug);                              /* 批量提交优化 */

    /* 遍历所有脏页 */
    while ((folio = writeback_iter(mapping, wbc, folio, &error))) {
        if (write_folio) {
            error = write_folio(folio, wbc);            /* 逐个 folio 回写 */
            if (error <= 0)
                continue;
        }
        error = mpage_write_folio(wbc, folio, &mpd);    /* 将 folio 添加到 bio */
    }

    if (mpd.bio)
        mpage_bio_submit_write(mpd.bio);                /* 提交最后一个 bio */

    blk_finish_plug(&plug);                             /* 刷新 plug */
    return error;
}
```

**`mpage_write_folio()` 的关键逻辑**：
1. 遍历 folio 的 buffer_head 链表
2. 对每个脏的 `buffer_head`，调用 `get_block()` 获取物理块号
3. 如果物理块号连续，合并到当前 bio；否则提交当前 bio 并创建新 bio
4. 最终通过 `mpage_bio_submit_write()` 提交 bio

#### 18.7.3 `submit_bh()` — Buffer Head 到 Bio 的转换

（[fs/buffer.c](file:///home/louis/code/linux/fs/buffer.c)）将 buffer_head 转换为 bio 并提交给块层：

```c
void submit_bh(blk_opf_t opf, struct buffer_head *bh)
{
    submit_bh_wbc(opf, bh, WRITE_LIFE_NOT_SET, NULL);
}

static void submit_bh_wbc(blk_opf_t opf, struct buffer_head *bh,
                          enum rw_hint write_hint, struct writeback_control *wbc)
{
    const enum req_op op = opf & REQ_OP_MASK;
    struct bio *bio;

    BUG_ON(!buffer_locked(bh));
    BUG_ON(!buffer_mapped(bh));
    BUG_ON(!bh->b_end_io);
    BUG_ON(buffer_delay(bh));
    BUG_ON(buffer_unwritten(bh));

    /* 检查写错误标志 */
    if (test_set_buffer_req(bh) && (op == REQ_OP_WRITE))
        clear_buffer_write_io_error(bh);

    /* 设置 bio 标志 */
    if (buffer_meta(bh))
        opf |= REQ_META;                    /* 元数据 I/O */
    if (buffer_prio(bh))
        opf |= REQ_PRIO;                    /* 高优先级 I/O */

    /* 分配 bio */
    bio = bio_alloc(bh->b_bdev, 1, opf, GFP_NOIO);
    fscrypt_set_bio_crypt_ctx_bh(bio, bh, GFP_NOIO);

    /* 设置扇区号：块号 → 512 字节扇区号 */
    bio->bi_iter.bi_sector = bh->b_blocknr * (bh->b_size >> 9);

    bio->bi_write_hint = write_hint;

    /* 绑定页面数据 */
    bio_add_folio_nofail(bio, bh->b_folio, bh->b_size, bh_offset(bh));

    /* 设置完成回调 */
    bio->bi_end_io = end_bio_bh_io_sync;
    bio->bi_private = bh;

    /* cgroup 写回关联 */
    if (wbc) {
        wbc_init_bio(wbc, bio);
        wbc_account_cgroup_owner(wbc, bh->b_folio, bh->b_size);
    }

    /* 提交到块层 */
    submit_bio(bio);
}
```

**`submit_bh()` 的完整流程**：

```
submit_bh(REQ_OP_WRITE, bh)
  │
  ├─ [1] 参数检查
  │     ├─ buffer_locked(bh)       ← 必须已锁定
  │     ├─ buffer_mapped(bh)       ← 必须有物理块映射
  │     └─ !buffer_delay(bh)       ← 不能是延迟分配
  │
  ├─ [2] 设置 REQ 标志
  │     ├─ buffer_meta(bh) → REQ_META     ← 元数据
  │     └─ buffer_prio(bh) → REQ_PRIO     ← 高优先级
  │
  ├─ [3] bio_alloc(bh->b_bdev, 1, REQ_OP_WRITE, GFP_NOIO)
  │     └─ 从 fs_bio_set 分配一个 bio，容量 1 个 bio_vec
  │
  ├─ [4] 设置扇区号
  │     bi_sector = bh->b_blocknr * (bh->b_size >> 9)
  │     └─ 例：b_blocknr=1000, b_size=4096 → bi_sector=8000
  │
  ├─ [5] bio_add_folio_nofail(bio, bh->b_folio, bh->b_size, bh_offset(bh))
  │     └─ 将 folio 中的数据段绑定到 bio
  │
  ├─ [6] 设置完成回调
  │     bio->bi_end_io = end_bio_bh_io_sync
  │     bio->bi_private = bh
  │     └─ I/O 完成后：end_bio_bh_io_sync() → bh->b_end_io() → folio 解锁
  │
  └─ [7] submit_bio(bio)
        └─ submit_bio_noacct() → blk_mq_submit_bio() → 块层
```

**I/O 完成回调链**：

```
硬件完成中断
  → 块层完成 (bio_endio)
    → end_bio_bh_io_sync(bio)               ← submit_bh 设置的回调
      ├─ bio->bi_private = bh（取出 buffer_head）
      ├─ if (bio->bi_status) → clear_buffer_uptodate(bh)
      ├─ else → set_buffer_uptodate(bh)
      ├─ unlock_buffer(bh)                  ← 释放 BH_Lock 锁
      │     └─ 唤醒等待该 bh 的进程
      └─ bh->b_end_io(bh, uptodate)         ← 文件系统设置的回调
            └─ 如 end_buffer_async_write()
                ├─ folio 内所有 bh 完成？
                │   ├─ 是 → folio_end_writeback(folio)  ← 通知页缓存
                │   └─ 否 → 等待其他 bh 完成
                └─ bio_put(bio)
```

### 18.8 完整函数调用栈

#### 18.8.1 后台回写路径（Buffer Write 典型场景）

```
用户态 write(fd, buf, count)
  │
  ├─ sys_write() → vfs_write()
  │   └─ generic_perform_write(iocb, iter)           [mm/filemap.c]
  │       └─ 循环写入页缓存：
  │           ├─ a_ops->write_begin() → ext4_write_begin()
  │           │   └─ grab_cache_folio_write_begin()  ← 获取/创建 folio
  │           ├─ copy_folio_from_iter_atomic()       ← 用户数据拷贝到 folio
  │           └─ a_ops->write_end() → ext4_write_end()
  │               └─ block_dirty_folio()             ← 标记脏页
  │                   └─ __set_page_dirty()           ← 设置 PAGECACHE_TAG_DIRTY
  │                       └─ __mark_inode_dirty()     ← 将 inode 加入 wb->b_dirty
  │
  └─ balance_dirty_pages_ratelimited(mapping)         [mm/page-writeback.c]
      └─ balance_dirty_pages(bdi, dirty_thresh)      限流检查
          └─ 如果 wb_dirty > wb_thresh:
              └─ wb_start_background_writeback(wb)   唤醒 Flush 线程
                  └─ wb_wakeup(wb)
                      └─ mod_delayed_work(bdi_wq, &wb->dwork, 0)

┌─────────────────────────────────────────────────────────────────────┐
│  Flush 线程被调度执行                                                 │
└─────────────────────────────────────────────────────────────────────┘

wb_workfn(work)                                      [fs/fs-writeback.c]
  │
  └─ wb_do_writeback(wb)
      │
      ├─ wb_check_background_flush(wb)               ← 检查后台阈值
      │   └─ wb_writeback(wb, &work)                 ← 执行后台回写
      │       │
      │       ├─ queue_io(wb, work, dirtied_before)  ← b_dirty → b_io
      │       │
      │       └─ __writeback_inodes_wb(wb, work)     ← 遍历 b_io
      │           └─ writeback_sb_inodes(sb, wb, work)
      │               └─ __writeback_single_inode(inode, &wbc)
      │                   │
      │                   ├─ do_writepages(mapping, wbc)  ← 写回脏页
      │                   │   │                          [mm/page-writeback.c]
      │                   │   │
      │                   │   └─ mapping->a_ops->writepages(mapping, wbc)
      │                   │       │
      │                   │       ├─ [ext4] ext4_writepages()
      │                   │       │   └─ ext4_do_writepages()
      │                   │   │       ├─ 循环脏页：
      │                   │   │       │   └─ mpage_add_bh_to_extent()
      │                   │   │       │       └─ mpage_submit_extent()
      │                   │   │       │           └─ submit_bh() 或 submit_bio()
      │                   │   │       └─ ext4_io_submit()
      │                   │   │
      │                   │   └─ [ext2] mpage_writepages()
      │                   │       └─ __mpage_writepages()
      │                   │           ├─ mpage_write_folio()  ← 添加 folio 到 bio
      │                   │           │   └─ 遍历 buffer_head 链表
      │                   │           │       ├─ get_block()  ← 块号映射
      │                   │           │       └─ 合并或提交 bio
      │                   │           └─ mpage_bio_submit_write()  ← 提交 bio
      │                   │
      │                   ├─ write_inode(inode, wbc)    ← 写回元数据
      │                   │   └─ s_op->write_inode()
      │                   │
      │                   └─ requeue_inode() 或清除 I_DIRTY
      │
      └─ wb_check_old_data_flush(wb)                  ← 周期性老旧数据回写
          └─ wb_writeback(wb, &work)                  ← 同上
```

#### 18.8.2 同步回写路径（`sync()` / `fsync()`）

```
sync() / fsync(fd)
  │
  └─ sync_inodes_sb(sb)                              [fs/fs-writeback.c]
      │
      └─ 对每个 BDI：
          DEFINE_WB_COMPLETION(done, bdi)             ← 定义完成标记
          struct wb_writeback_work work = {
              .sb       = sb,
              .sync_mode = WB_SYNC_ALL,
              .nr_pages  = LONG_MAX,
              .for_sync  = 1,
              .done      = &done,
          };
          wb_queue_work(wb, &work);                   ← 入队
          wb_wait_for_completion(&done);              ← 等待完成
              │
              ▼
          wb_workfn()
            └─ wb_do_writeback()
                └─ get_next_work_item() → wb_writeback(wb, &work)
                    └─ writeback_sb_inodes(sb, wb, work)
                        └─ __writeback_single_inode(inode, &wbc)
                            ├─ do_writepages()       ← WB_SYNC_ALL: 等待 I/O
                            └─ write_inode()         ← 等待元数据写入
                                │
                                ▼
              finish_writeback_work(work)              ← 通知完成
                └─ atomic_dec_and_test(&done->cnt)
                    → wake_up_all(done->waitq)
```

### 18.9 触发条件对比总结

| 触发类型 | 触发条件 | `sync_mode` | `nr_pages` | 是否等待 |
|---------|---------|-------------|-----------|---------|
| **后台回写** | `wb_dirty > wb_thresh`（超过后台阈值） | `WB_SYNC_NONE` | `LONG_MAX` | 否 |
| **周期性回写** | `dirty_writeback_interval` 定时器到期（默认 5s） | `WB_SYNC_NONE` | `LONG_MAX` | 否 |
| **老旧数据回写** | 脏页驻留超过 `dirty_expire_interval`（默认 30s） | `WB_SYNC_NONE` | `LONG_MAX` | 否 |
| **内存回收** | vmscan 需要回收脏页 | `WB_SYNC_NONE` | 1024 | 否 |
| **sync()** | 用户态调用 `sync()` | `WB_SYNC_ALL` | `LONG_MAX` | 是 |
| **fsync()** | 用户态调用 `fsync(fd)` | `WB_SYNC_ALL` | `LONG_MAX` | 是 |
| **文件系统主动** | 空间不足等 | `WB_SYNC_NONE` | 指定数量 | 否 |

### 18.10 关键设计要点总结

| 设计点 | 说明 |
|-------|------|
| **异步回写** | `write()` 仅写入页缓存，Flush 线程异步回写，写操作不阻塞 |
| **三链表模型** | `b_dirty` → `b_io` → `b_more_io` 实现脏 inode 的调度与重试 |
| **Workqueue 实现** | Flush 线程不依赖独立内核线程，通过 `delayed_work` 在 `bdi_wq` 上调度 |
| **Plug 批量提交** | `wb_writeback()` 使用 `blk_start_plug/blk_finish_plug` 批量提交 I/O 请求 |
| **带宽估算** | 通过 `wb_update_bandwidth()` 动态估算写带宽，用于 `balance_dirty_pages()` 限流 |
| **Cgroup 写回** | `wbc_init_bio()` 将 bio 关联到 cgroup，实现 per-cgroup 的 I/O 隔离 |
| **I_SYNC 互斥** | 同一 inode 的并发回写通过 `I_SYNC` 标志互斥，防止数据竞争 |
| **优先级区分** | `REQ_SYNC`（同步写）vs `REQ_BACKGROUND`（后台写），块层据此调度 |

---