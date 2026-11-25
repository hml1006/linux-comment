# bdevfs — 块设备文件系统

## 1. 概述与实现机制

bdevfs（bdev）是一个特殊的伪文件系统，用于管理块设备的 inode 和页缓存。**不直接挂载**到文件系统树中，而是通过内核内部函数访问。

### 核心作用

1. **为每个块设备分配 inode 和 address_space**
2. **管理块设备的页缓存**（块设备本身也有页缓存）
3. **提供块设备打开/关闭操作**

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                    文件系统层                                 │
│  ext4_read_inode() → sb_bread() → __getblk()                │
│  → 在 bdev 页缓存中查找/创建 buffer_head                     │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                    bdevfs (block/bdev.c)                     │
│  blockdev_superblock → 全局块设备超级块                      │
│  bdev_alloc() → 分配 block_device + inode                   │
│  bdev_open() → 打开块设备                                   │
│  blkdev_read_folio() / blkdev_write_begin()                 │
│  def_blk_aops → 块设备地址空间操作                          │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                    块设备层                                   │
│  submit_bio() → 提交块设备 I/O 请求                         │
│  block_device → gendisk → request_queue → NVMe/SCSI 驱动    │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 block_device — 块设备实例

```c
// 文件: include/linux/blk_types.h
struct block_device {
    dev_t                   bd_dev;         // 设备号 (主:次)
    struct inode            *bd_inode;      // 块设备的 VFS inode
    struct gendisk          *bd_disk;       // 通用磁盘结构
    struct request_queue    *bd_queue;      // 请求队列
    struct backing_dev_info *bd_bdi;        // 后备设备信息
    struct block_device     *bd_contains;   // 指向整个磁盘
    struct block_device     *bd_part;       // 分区信息
    unsigned int            bd_block_size;  // 块大小
    struct address_space    *bd_mapping;    // 块设备页缓存
    struct super_block      *bd_frozen_sb;  // 冻结时的超级块
    int                     bd_fsfreeze_count; // 冻结计数
    atomic_t                bd_openers;     // 打开计数
    spinlock_t              bd_size_lock;   // 大小锁
    struct mutex            bd_holder_lock; // 持有者锁
    struct kobject           *bd_holder_dir;  // 持有者目录
    u8                      bd_partno;      // 分区号
    unsigned long            __bd_flags;     // 标志位
};
```

### 2.2 buffer_head — 缓冲区头

```c
// 文件: include/linux/buffer_head.h
struct buffer_head {
    struct folio            *b_folio;       // 映射到的 folio
    struct block_device     *b_bdev;        // 所属块设备
    sector_t                b_blocknr;      // 逻辑块号
    size_t                  b_size;         // 块大小
    char                    *b_data;        // 数据指针 (folio 内偏移)
    struct page             *b_page;        // 映射到的页 (旧版)

    atomic_t                b_count;        // 引用计数
    void                    (*b_end_io)(struct buffer_head *bh, int uptodate); // I/O 完成回调
    struct buffer_head      *b_this_page;   // 同一页中的下一个 buffer_head
    struct list_head        b_assoc_buffers; // 关联链表的 buffer_head

    unsigned long           b_state;        // 状态标志位
    // 状态位: BH_Uptodate, BH_Dirty, BH_Lock, BH_Mapped, BH_New 等
};
```

### 2.3 address_space 操作 (块设备)

```c
// 文件: block/bdev.c
static const struct address_space_operations def_blk_aops = {
    .read_folio     = blkdev_read_folio,     // 读取 folio
    .write_begin    = blkdev_write_begin,    // 开始写入
    .write_end      = blkdev_write_end,      // 结束写入
    .writepages     = blkdev_writepages,     // 批量写回
    .dirty_folio    = block_dirty_folio,     // 标记脏页
    .invalidate_folio = block_invalidate_folio, // 失效 folio
    .release_folio  = block_release_folio,   // 释放 folio
    .direct_IO      = blkdev_direct_IO,      // 直接 I/O
};
```

---

## 3. API 与使用方法

### 3.1 核心 API

```c
#include <linux/blkdev.h>
#include <linux/buffer_head.h>

// 块设备分配和释放
struct block_device *bdev_alloc(struct gendisk *disk, u8 partno);
struct block_device *blkdev_get_by_path(const char *path, fmode_t mode, void *holder);
struct block_device *blkdev_get_by_dev(dev_t dev, fmode_t mode, void *holder);
void blkdev_put(struct block_device *bdev, fmode_t mode);

// 块设备打开/关闭
int bdev_open(struct block_device *bdev, blk_mode_t mode, void *holder,
              const struct blk_holder_ops *hops, struct bdev_handle *handle);
void bdev_release(struct bdev_handle *handle);

// 块设备操作
int bdev_read_page(struct block_device *bdev, sector_t sector,
                   struct page *page);
int bdev_write_page(struct block_device *bdev, sector_t sector,
                    struct page *page, struct writeback_control *wbc);

// buffer_head 操作
struct buffer_head *__getblk(struct block_device *bdev, sector_t block,
                             unsigned size);
struct buffer_head *__getblk_gfp(struct block_device *bdev, sector_t block,
                                 unsigned size, gfp_t gfp);
struct buffer_head *sb_bread(struct super_block *sb, sector_t block);
struct buffer_head *sb_bread_unmovable(struct super_block *sb, sector_t block);
void brelse(struct buffer_head *bh);
void bforget(struct buffer_head *bh);
```

### 3.2 使用示例

```c
// 示例1: 文件系统通过 sb_bread 读取元数据
// ext4 读取 inode 表
static struct inode *ext4_read_inode(struct super_block *sb, unsigned long ino)
{
    struct ext4_group_desc *gdp;
    struct buffer_head *bh;
    struct ext4_inode *raw_inode;
    int inodes_per_block = EXT4_SB(sb)->s_inodes_per_block;
    int ino_offset = (ino - 1) % inodes_per_block;
    sector_t block;

    // 计算 inode 所在的块号
    block = ext4_inode_table(sb, gdp) + (ino - 1) / inodes_per_block;

    // 从块设备页缓存中读取块
    bh = sb_bread(sb, block);
    if (!bh)
        return ERR_PTR(-EIO);

    // 从 buffer_head 中解析 inode 数据
    raw_inode = (struct ext4_inode *)bh->b_data;
    // ... 解析 raw_inode[ino_offset] ...

    brelse(bh);  // 释放 buffer_head
    return inode;
}
```

```c
// 示例2: 直接打开块设备
struct bdev_handle *bdev_handle;
bdev_handle = bdev_open_by_path("/dev/sda1", BLK_OPEN_READ, NULL, NULL);
if (IS_ERR(bdev_handle))
    return PTR_ERR(bdev_handle);

struct block_device *bdev = bdev_handle->bdev;
// 使用 bdev 进行操作
pr_info("Block device size: %llu sectors\n", bdev_nr_sectors(bdev));

bdev_release(bdev_handle);
```

---

## 4. 函数调用栈

### 4.1 bdevfs 初始化

```
start_kernel()
  → vfs_caches_init()
    → mnt_init()
      → bdev_cache_init()                      // fs/block_dev.c
        → init_special_inode()                 // 创建 bdev 伪文件系统
        → register_filesystem(&bd_type)        // 注册 bdev_fs_type
        → kern_mount(&bd_type)                 // 内核挂载 bdevfs
          → vfs_kern_mount()
            → bdev_fs_type.init_fs_context()
            → bdev_fill_super()
              → blockdev_superblock = sget()   // 获取/创建超级块
              → sb->s_op = &bdev_sops
              → sb->s_iflags |= SB_I_RMTREE
              → sb->s_iflags |= SB_I_NODEV
```

### 4.2 块设备分配

```
bdev_alloc(disk, partno)
  → new_inode(blockdev_superblock)             // 在 bdevfs 中分配 inode
    → alloc_inode(blockdev_superblock)
      → bdev_alloc_inode()                     // 分配 bdev_inode
        → kmem_cache_alloc(bdev_cachep)        // 从 slab 分配
        → inode->i_mode = S_IFBLK              // 设置为块设备文件
        → inode->i_data.a_ops = &def_blk_aops  // 设置地址空间操作
        → mapping_set_gfp_mask(&inode->i_data, GFP_USER)
  → bdev = I_BDEV(inode)                       // 从 inode 获取 block_device
  → bdev->bd_mapping = &inode->i_data          // 设置页缓存映射
  → bdev->bd_disk = disk                       // 关联通用磁盘
  → bdev->bd_queue = disk->queue               // 关联请求队列
```

### 4.3 文件系统通过 sb_bread 读取元数据

```
ext4_read_inode(sb, ino, raw_inode)
  → ext4_inode_table(sb, gdp) + offset        // 计算块号
  → sb_bread(sb, block)                        // 读取块设备块
    → __bread_gfp(sb->s_bdev, block, sb->s_blocksize, __GFP_MOVABLE)
      → __getblk_gfp(bdev, block, size, gfp)  // 在 bdev 页缓存中查找
        → __find_get_block(bdev, block, size)  // 查找已有 buffer_head
          → BUG_ON() 如果未找到
          → 从 page cache 中查找页
          → 在页内查找/创建 buffer_head
      → 如果 buffer_head 未缓存:
        → __bread_slow(bh)                     // 发起 I/O 读取
          → submit_bh(REQ_OP_READ, bh)         // 提交读请求
            → submit_bio(bio)                  // 创建并提交 bio
              → __submit_bio()                 // 进入块设备层
              → blk_mq_submit_bio()            // 多队列提交
                → nvme_queue_rq()              // NVMe 驱动处理
                  → 实际 DMA 读取
```

---

## 5. 流程图

### 5.1 bdevfs 架构

```
bdevfs (伪文件系统, 不直接挂载)
    │
    ├── blockdev_superblock (全局超级块)
    │     │
    │     ├── inode #1 (bdev: 8:0)  ←→  block_device (sda)
    │     │      └── address_space → 页缓存 (sda 的元数据)
    │     │
    │     ├── inode #2 (bdev: 8:1)  ←→  block_device (sda1)
    │     │      └── address_space → 页缓存 (sda1 的元数据)
    │     │
    │     └── inode #3 (bdev: 8:2)  ←→  block_device (sda2)
    │            └── address_space → 页缓存 (sda2 的元数据)
    │
    └── 每个 block_device 的 bd_inode 指向对应的 inode
```

### 5.2 文件系统元数据读取流程

```
文件系统 (ext4)                     bdev 页缓存                块设备层
    │                                  │                          │
    │ sb_bread(sb, block)              │                          │
    │─────────────────────────────────►│                          │
    │                                  │                          │
    │ __find_get_block()               │                          │
    │                                  │                          │
    │ 在 bdev 页缓存中查找 block       │                          │
    │                                  │                          │
    │ 命中? ──是──► 返回 bh            │                          │
    │   │                              │                          │
    │   否                             │                          │
    │   │                              │                          │
    │   ▼                              │                          │
    │ __bread_slow()                   │                          │
    │   → alloc_buffer_head()          │                          │
    │   → 分配页并关联到 folio         │                          │
    │   → submit_bh()                  │                          │
    │──────────────────────────────────────────────────────────►  │
    │                                  │     submit_bio()         │
    │                                  │     → NVMe 驱动         │
    │                                  │     → DMA 读取          │
    │                                  │                          │
    │◄──────────────────────────────────────────────────────────  │
    │  bio 完成回调                       bh->b_end_io()          │
    │                                  │                          │
    │  ◄── 返回 bh 给文件系统          │                          │
    │                                  │                          │
    │  (从 bh->b_data 解析元数据)      │                          │
    │                                  │                          │
    │  brelse(bh) 释放 bh              │                          │
    │                                  │                          │
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **文件系统元数据缓存** | 缓存 super_block、inode 表、块组描述符 | `sb_bread()` 读取文件系统元数据 |
| **块设备直接访问** | 直接读写块设备（不经过文件系统） | `dd if=/dev/sda` |
| **文件系统挂载** | 挂载时读取块设备超级块 | `ext4_fill_super()` → `sb_bread()` |
| **swap 分区** | 将块设备用作交换分区 | `swapon /dev/sda2` |
| **原始设备 I/O** | 数据库绕过文件系统直接访问块设备 | O_DIRECT 打开块设备 |
| **设备映射** | device mapper 层管理块设备映射 | dm-crypt, LVM |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `block/bdev.c` | 块设备核心实现（分配、打开、关闭） |
| `fs/buffer.c` | buffer_head 管理（__getblk, sb_bread, submit_bh） |
| `fs/block_dev.c` | bdevfs 初始化、块设备文件操作 |
| `include/linux/blkdev.h` | block_device 数据结构定义 |
| `include/linux/buffer_head.h` | buffer_head 数据结构定义和 API |