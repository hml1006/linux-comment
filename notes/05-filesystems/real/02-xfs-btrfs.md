# XFS 与 Btrfs

## 1. 概述与对比

XFS 与 Btrfs 都是面向大容量存储的现代文件系统，但设计取向不同：

| 维度 | XFS | Btrfs |
|------|-----|-------|
| 分配单元 | 分配组（AG），彼此独立的空闲空间与 inode | chunk（逻辑地址到设备地址的映射），block group |
| 元数据索引 | 每 AG 一套 B+tree（bnobt/cntbt/rmapbt/refcountbt）+ inode fork 的 extent B+tree | 全局 COW B-tree（root tree、chunk tree、extent tree、fs tree） |
| 数据一致性 | 写时只改元数据，日志（xlog）+ CIL 保证崩溃一致性 | 全 COW，superblock 中 root 指针原子切换 |
| 快照 | 无原生快照（依赖 reflink/LVM） | 原生 subvolume/snapshot，root item 引用计数 |
| 校验 | 元数据 CRC，可选 reflink/rmap | 数据与元数据 csum，scrub 在线校验 |
| 在线调整 | `xfs_growfs` 扩容 | 设备增删、balance、resize |

当前 `.config`：`CONFIG_XFS_FS is not set`、`CONFIG_BTRFS_FS is not set`，两者均不编译。`fs/xfs/`、`fs/btrfs/` 源码完整存在，可作为阅读对象，但本内核无法挂载。

---

## 2. XFS 磁盘布局

### 2.1 分配组 AG

XFS 把设备划分为若干等大的 AG（allocation group），每个 AG 独立管理空闲空间与 inode，因而分配可以并行：

```
AG 0                        AG 1                       ...
┌────────┬──────┬──────┬──────┬────────┬─────┐
│ SB 副本│ AGF  │ AGI  │ AGFL │ B+tree │ 数据 │
└────────┴──────┴──────┴──────┴────────┴─────┘
```

- **AGF**（`struct xfs_agf`，`libxfs/xfs_format.h`）：空闲空间 B+tree 根（`agf_bno_root`、`agf_cnt_root`）、rmap 根、各级树高、freelist 索引、`agf_freeblks`、`agf_longest`、`agf_btreeblks`。
- **AGI**：inode B+tree 根与空闲 inode 计数。
- **AGFL**：AG free list，存放 btree 分裂时预留的块。
- 每个 AG 的 btree 高度、空闲块数在 `struct xfs_perag`（`libxfs/xfs_ag.h`）中缓存：`pagf_bno_level`、`pagf_cnt_level`、`pagf_rmap_level`、`pagf_freeblks`、`pagf_longest`、`pagi_freecount`、`pagi_count`，以及元数据/rmap 预留 `pag_meta_resv`、`pag_rmapbt_resv`。

### 2.2 inode 与 fork

XFS inode 有两个 fork：数据 fork `i_df` 与属性 fork `i_af`，reflink 时还有 COW fork `i_cowfp`。每个 fork 用 `struct xfs_ifork` 表示，其格式可以是：

- `XFS_DINODE_FMT_EXTENTS`：extent 记录直接内嵌在 inode；
- `XFS_DINODE_FMT_BTREE`：根在 inode 内，指向外部 B+tree 块；
- `XFS_DINODE_FMT_LOCAL`：目录/符号链接数据直接内嵌。

extent B+tree 的键是文件逻辑块号，记录是 `xfs_bmbt_rec`（起始块、块数、状态）。`xfs_bmapi_*` 系列在 `libxfs/xfs_bmap.c` 中维护它。

### 2.3 日志

XFS 日志是环形缓冲区，记录“重做日志”（redo），不写数据本身。日志项（log item）由各模块注册，提交时进入 CIL（Committed Item List），由 `xfs_log_cil.c` 合并后写入 log，再挂到 AIL（Active Item List）等待回写完成。

---

## 3. XFS 核心数据结构

### 3.1 xfs_mount

`fs/xfs/xfs_mount.h`：

```c
typedef struct xfs_mount {
    struct xfs_sb        m_sb;        /* 超级块副本 */
    struct super_block  *m_super;
    struct xfs_ail      *m_ail;       /* active log item list */
    struct xfs_buf      *m_sb_bp;
    struct xfs_da_geometry *m_dir_geo, *m_attr_geo;
    struct xlog         *m_log;
    struct xfs_inode    *m_rootip;
    struct xfs_inode    *m_metadirip; /* metadata directory */
    struct xfs_inode    *m_rtdirip;
    struct xfs_buftarg  *m_ddev_targp, *m_logdev_targp, *m_rtdev_targp;
    void __percpu       *m_inodegc;
    struct workqueue_struct *m_buf_workqueue, *m_unwritten_workqueue;
    struct workqueue_struct *m_reclaim_workqueue, *m_sync_workqueue;
    int                  m_bsize;     /* 逻辑块大小 */
    uint                 m_blockmask, m_blockwsize;
    uint                 m_alloc_mxr[2], m_alloc_mnr[2];
    uint                 m_bmap_dmxr[2], m_bmap_dmnr[2];
    uint                 m_rmap_mxr[2], m_refc_mxr[2];
    uint                 m_alloc_maxlevels, m_bm_maxlevels[2];
    uint                 m_rmap_maxlevels, m_refc_maxlevels;
    xfs_extlen_t         m_ag_prealloc_blocks;
    uint                 m_alloc_set_aside, m_ag_max_usable;
} xfs_mount_t;
```

`m_bm_maxlevels[]` 等字段在挂载时按块大小与 AG 大小算好，B+tree 操作直接用这些上限。

### 3.2 xfs_inode 与 xfs_trans

```c
typedef struct xfs_inode {
    struct xfs_mount *i_mount;
    struct xfs_dquot *i_udquot, *i_gdquot, *i_pdquot;
    struct xfs_imap   i_imap;       /* inode 在磁盘上的位置 */
    struct xfs_ifork *i_cowfp;      /* reflink COW fork */
    struct xfs_ifork  i_df;         /* data fork */
    struct xfs_ifork  i_af;         /* attr fork */
    struct xfs_inode_log_item *i_itemp;
    struct rw_semaphore i_lock;
    atomic_t          i_pincount;
    uint16_t          i_checked, i_sick;
    unsigned long     i_flags;
    uint64_t          i_delayed_blks; /* 延迟分配块数 */
    xfs_fsize_t       i_disk_size;
    xfs_rfsblock_t    i_nblocks;
    xfs_extlen_t      i_extsize;
    /* ... */
} xfs_inode_t;
```

`struct xfs_trans`（`fs/xfs/xfs_trans.h`）记录一次原子修改：预留的 log 空间 `t_log_res`/`t_log_count`、预留块数 `t_blk_res`、日志 ticket `t_ticket`、挂接的 log item 链表 `t_items`、busy extent 链表、deferred ops 链表，以及超级块计数字段的增量（`t_icount_delta`、`t_fdblocks_delta` 等）。

### 3.3 B+tree 游标

所有 XFS B+tree 共用一套通用操作（`libxfs/xfs_btree.c`）：`xfs_btree_lookup()`、`xfs_btree_insert()`、`xfs_btree_delete()`、`xfs_btree_delrec()`、`xfs_btree_split()`、`xfs_btree_new_root()`。具体树通过 `struct xfs_btree_ops` 提供键比较、记录读写、块分配/释放、根指针更新等回调；游标 `struct xfs_btree_cur` 保存当前树、层级路径与指针。

---

## 4. XFS 关键流程

### 4.1 缓冲写与延迟分配

```
write() -> iomap -> xfs_buffered_write_iomap_begin()
  ├─ 若已有 extent，直接 iomap 映射
  ├─ 若为 hole，调用 xfs_bmapi_reserve_delalloc()
  │    ├─ 预留空闲块（不写 bmap）
  │    └─ 增加 ip->i_delayed_blks
  └─ 返回 IOMAP_DELALLOC 映射，页缓存标记 dirty

回写 -> xfs_bmapi_convert_delalloc()
  ├─ 真正分配 extent（xfs_bmapi_write -> xfs_alloc_vextent*）
  ├─ 插入 bmap B+tree（xfs_bmap_add_extent_*）
  └─ 递减 i_delayed_blks
```

延迟分配把“分配决策”推迟到回写，使连续写更容易合并成大 extent，减少碎片。

### 4.2 直接 IO 与 unwritten extent

`xfs_direct_write_iomap_begin()` 对已对齐的直接写直接分配 unwritten extent；写完由 `xfs_iomap_write_unwritten()` 通过 `xfs_bmapi_write()` 把 extent 状态转为 written。这样崩溃时未完成的数据不会被误读。

### 4.3 日志提交

```
xfs_trans_commit(tp)
  -> xfs_log_reserve(mp, t_log_res, t_log_count, ...)
  -> 各 log item 的 iop_committing / iop_commit 回调
  -> xlog_cil_insert_items() 合并到 CIL
  -> xlog_cil_push_work() 写 iclog，xlog_sync() 刷盘
  -> 完成后把 item 移到 AIL，xfs_ail_push_all() 在回写完成后释放
```

`xfs_trans_commit()` 返回即表示日志已持久（或已进入可恢复的 CIL），数据块可以稍后回写；`xfs_ail_push_all()` 在回写完成后推动 AIL 释放。恢复时 `xlog_recover()` 重放日志项。

### 4.4 空间分配

`xfs_alloc_vextent*()`（`libxfs/xfs_alloc.c`）在 AG 的空闲空间 B+tree（bnobt 按块号、cntbt 按长度）中查找合适 extent，优先“近邻”分配以提高局部性；跨 AG 时逐个尝试（`xfs_alloc_vextent_start_ag`、`xfs_alloc_vextent_near_bno`、`xfs_alloc_vextent_exact_bno` 等）。rmapbt 记录每个物理块属于谁，refcountbt 支持 reflink 的共享块计数。

---

## 5. Btrfs 架构

### 5.1 fs_info 与全局 root

`struct btrfs_fs_info`（`fs/btrfs/fs.h`）是挂载实例的根：

```c
struct btrfs_fs_info {
    u8 chunk_tree_uuid[BTRFS_UUID_SIZE];
    unsigned long flags;
    struct btrfs_root *tree_root;      /* root tree */
    struct btrfs_root *chunk_root;     /* chunk tree */
    struct btrfs_root *dev_root;
    struct btrfs_root *fs_root;        /* 默认 subvolume */
    struct btrfs_root *quota_root;
    struct btrfs_root *uuid_root;
    struct btrfs_root *data_reloc_root;
    struct btrfs_root *block_group_root;
    struct btrfs_root *stripe_root;
    struct btrfs_root *remap_root;
    struct btrfs_root *log_root_tree;

    rwlock_t global_root_lock;
    struct rb_root global_root_tree;
    spinlock_t fs_roots_radix_lock;
    struct radix_tree_root fs_roots_radix;

    rwlock_t block_group_cache_lock;
    struct rb_root_cached block_group_cache_tree;
    atomic64_t free_chunk_space;

    struct extent_io_tree excluded_extents;
    struct rb_root_cached mapping_tree;   /* logical -> physical */
    rwlock_t mapping_tree_lock;

    struct btrfs_block_rsv global_block_rsv;
    struct btrfs_block_rsv trans_block_rsv;
    struct btrfs_block_rsv chunk_block_rsv;
    struct btrfs_block_rsv delayed_refs_rsv;
    u64 generation, last_trans_committed;
    unsigned long long mount_opt;
    /* ... */
};
```

`struct btrfs_root`（`fs/btrfs/ctree.h`）代表一棵树的根：`node`（当前根块）、`commit_root`（上次提交的根，供快照/只读遍历）、`log_root`（fsync 日志树）、`root_item`（磁盘上的 `btrfs_root_item`）、`root_key`、`fs_info`、`dirty_log_pages`、`block_rsv`、`log_mutex` 与 log 事务状态。`btrfs_root_item` 中的 `bytenr`、`generation`、`refs`、`drop_progress`、`drop_level` 是快照与删除的关键。

### 5.2 chunk 与设备映射

Btrfs 的逻辑地址不直接等于物理地址。chunk tree 把逻辑区间映射到一组设备上的 stripe：

```c
struct btrfs_chunk_map {
    struct rb_node rb_node;
    int verified_stripes;
    refcount_t refs;
    u64 start;          /* 逻辑起始 */
    u64 chunk_len;
    u64 stripe_size;
    u64 type;           /* DATA / METADATA / SYSTEM + RAID profile */
    int io_align, io_width;
    int num_stripes, sub_stripes;
    struct btrfs_io_stripe stripes[];
};
```

`mapping_tree`（`rb_root_cached`）按逻辑地址索引 `btrfs_chunk_map`；`btrfs_map_block()` 把逻辑 bio 拆成各设备的物理 stripe。`btrfs_alloc_chunk()`（`volumes.c`）在设备空闲空间中按 RAID profile 选择 stripe 并写 chunk item。

### 5.3 btrfs_inode

```c
struct btrfs_inode {
    struct btrfs_root *root;          /* 所属 subvolume */
    u8 prop_compress, defrag_compress;
    spinlock_t lock;
    struct extent_map_tree extent_tree; /* 文件逻辑 -> 磁盘映射缓存 */
    struct extent_io_tree io_tree;      /* DIRTY / LOCKED / ORDERED */
    struct extent_io_tree *file_extent_tree;
    struct mutex log_mutex;
    /* ... */
};
```

`extent_tree` 是内存中的 extent map 缓存，避免每次读都查 extent tree；`io_tree` 跟踪页的脏/锁定/写回状态。

---

## 6. Btrfs COW 与 B-tree

### 6.1 查找与 COW

```
btrfs_search_slot(trans, root, key, p, ins_len, cow)
  ├─ 从 root->node 逐层下降，读子节点（extent buffer）
  ├─ 若 cow == 1 且路径上的块属于旧事务：
  │    btrfs_cow_block(trans, root, buf, parent, slot, &cow, ...)
  │      ├─ 分配新块（btrfs_alloc_tree_block）
  │      ├─ 拷贝旧块内容
  │      ├─ 修改父节点指针指向新块（递归 COW 父节点）
  │      └─ 旧块加入 delayed ref，事务提交后释放
  └─ 返回叶子节点与 slot
```

`btrfs_cow_block()` 在 `fs/btrfs/ctree.c`。COW 的传播是自底向上的：任何叶子修改都会导致到根的整条路径被复制，因此 Btrfs 的写放大与树高成正比。为降低开销，Btrfs 用 `btrfs_join_transaction()` / `btrfs_start_transaction()` 把多次修改合并到一个事务，只在事务首次触碰某块时 COW。

### 6.2 delayed refs

extent 的引用计数变更不立即写 extent tree，而是记录为 delayed ref，由 `btrfs_run_delayed_refs(trans, min_bytes)`（`extent-tree.c`）批量处理。好处：

- 同一事务内对同一块的多次引用增减可以合并；
- 避免在关键路径上随机写 extent tree；
- 事务提交前统一更新，减少锁竞争。

`struct btrfs_trans_handle`（`fs/btrfs/transaction.h`）记录 `transid`、`bytes_reserved`、`delayed_refs_bytes_reserved`、`block_rsv`，以及 `pending_snapshot`（创建快照时挂接）。

### 6.3 事务与 superblock 切换

Btrfs 的提交是“写新树 + 切根”：

1. 事务内所有修改写入新分配的 COW 块；
2. 提交时把各 root 的新根块写盘，更新 root tree 中的 `btrfs_root_item`；
3. 写 superblock 的 `root`、`chunk_root`、`generation` 等字段（多个 superblock 副本）；
4. 旧树块在 delayed ref 处理完后回收。

因此崩溃后要么看到旧根，要么看到新根，不会出现半写状态。

---

## 7. Btrfs 快照与 scrub

### 7.1 快照

快照本质是“共享同一批 extent、但拥有独立 root”的 subvolume。`BTRFS_IOC_SNAP_CREATE` → `btrfs_ioctl_snap_create()` → `btrfs_mksubvol()` 创建 `btrfs_pending_snapshot`，挂到当前事务的 `trans->pending_snapshot`；事务提交时 `create_pending_snapshot()`（`transaction.c`）：

- 复制源 root 的 `root_item`，设置新 `bytenr`/`generation`；
- `refs` 增加，源 root 与新 root 共享 extent；
- 把新 root 插入 root tree 与 `fs_roots_radix`；
- 返回新的 subvolume id。

删除快照走 `btrfs_drop_snapshot()`：按 `drop_progress`/`drop_level` 逐层释放 extent，引用计数归零的块才真正回收，因此删除大快照是后台渐进过程。

### 7.2 scrub

`btrfs_scrub_dev(fs_info, devid, start, end, ...)`（`fs/btrfs/scrub.c`）在线校验：

- 以 `scrub_stripe` 为单位读取数据与 csum，对 RAID1/10 多副本分别读取并比较，发现坏块时从好副本修复；
- RAID5/6 用校验重建；元数据块额外校验 header csum 与 tree 结构；
- 结果通过 `BTRFS_IOC_SCRUB_PROGRESS` 汇报，错误计数写入 `btrfs_scrub_progress`。

scrub 只读不修改数据（除修复副本外），可与正常 IO 并发。

---

## 8. 当前配置与结论

| 配置项 | 值 | 影响 |
|--------|----|------|
| `CONFIG_XFS_FS` | `is not set` | `fs/xfs/` 不编译，无 `xfs` 文件系统类型 |
| `CONFIG_BTRFS_FS` | `is not set` | `fs/btrfs/` 不编译，无 `btrfs` 类型与 ioctl |
| `CONFIG_XFS_QUOTA` / `XFS_PNFS` 等 | 不可见 | 相关子功能仅在源码中 |
| `CONFIG_BTRFS_FS_POSIX_ACL` / `BTRFS_DEBUG` | 不可见 | ACL、调试设施不编译 |

本内核无法挂载 XFS/Btrfs；但两者的磁盘格式、B+tree/COW 算法与日志/事务机制在源码中完整可见，适合做静态分析。要实际验证需打开对应 Kconfig 并重新构建。

---

## 关键文件清单

| 文件 | 内容 |
|------|------|
| `fs/xfs/xfs_mount.h` | `xfs_mount`、挂载参数与几何字段 |
| `fs/xfs/xfs_inode.h` | `xfs_inode`、fork 与延迟分配计数 |
| `fs/xfs/xfs_trans.h` | `xfs_trans`、日志预留与增量计数 |
| `fs/xfs/libxfs/xfs_format.h` | `xfs_agf`/`xfs_agi`/`xfs_agfl`、inode 与 btree 磁盘格式 |
| `fs/xfs/libxfs/xfs_ag.h` | `xfs_perag`、AG 缓存与预留 |
| `fs/xfs/libxfs/xfs_btree.c` | 通用 B+tree 查找/插入/删除/分裂 |
| `fs/xfs/libxfs/xfs_bmap.c` | extent B+tree、`xfs_bmapi_write`、`xfs_bmapi_convert_delalloc` |
| `fs/xfs/libxfs/xfs_alloc.c` | AG 空闲空间分配 `xfs_alloc_vextent*` |
| `fs/xfs/xfs_iomap.c` | `xfs_buffered_write_iomap_begin`、`xfs_direct_write_iomap_begin`、`xfs_bmapi_reserve_delalloc` |
| `fs/xfs/xfs_log.c` | `xlog_sync`、日志写与恢复、AIL push |
| `fs/xfs/xfs_log_cil.c` | CIL 合并与 push |
| `fs/btrfs/fs.h` | `btrfs_fs_info`、全局 root 与 block group 缓存 |
| `fs/btrfs/ctree.h` | `btrfs_root`、`btrfs_root_item`、树操作声明 |
| `fs/btrfs/ctree.c` | `btrfs_search_slot`、`btrfs_cow_block` |
| `fs/btrfs/extent-tree.c` | `btrfs_run_delayed_refs`、`btrfs_drop_snapshot`、block group |
| `fs/btrfs/volumes.c` | `btrfs_chunk_map`、`btrfs_alloc_chunk`、`btrfs_map_block` |
| `fs/btrfs/transaction.c` | 事务提交、`create_pending_snapshot` |
| `fs/btrfs/scrub.c` | `btrfs_scrub_dev`、`scrub_stripe`；`ioctl.c` 为快照/scrub ioctl 入口 |
