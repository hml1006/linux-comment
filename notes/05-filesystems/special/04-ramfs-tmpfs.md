# ramfs / tmpfs — 内存文件系统

## 1. 概述与实现机制

ramfs 和 tmpfs 将数据完全存储在内存中，不关联任何块设备。写入的数据会占用物理内存，系统重启后数据丢失。

### ramfs

ramfs 是最早的内存文件系统，基于 **simple 文件系统框架**，实现极简。

- **无大小限制**：可以写满所有内存（危险）
- **不可 swap**：数据始终在物理内存中
- **用于 rootfs**：初始根文件系统

### tmpfs

tmpfs 是 ramfs 的增强版，基于 **shmem（共享内存）子系统**，功能更完善。

- **支持大小限制**：通过 `mount -o size=N` 限制
- **支持 swap 后备**：不活跃页面可换出
- **支持文件链接和权限**
- **用于 /tmp, /dev/shm** 等

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                       用户空间                                │
│  mount -t ramfs ...  |  mount -t tmpfs ...  |  mmap SHM     │
└────────────────────────┬─────────────────────────────────────┘
                         │ VFS 系统调用
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                    ramfs (fs/ramfs/)                         │
│  ramfs_get_inode() → ramfs_file_inode_operations            │
│  ramfs_file_operations → generic_file_read_iter/write_iter  │
│  地址空间操作: ram_aops (基于页缓存)                         │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                    tmpfs (mm/shmem.c)                        │
│  shmem_get_inode() → shmem_file_operations                   │
│  shmem_read_iter() / shmem_write_iter()                      │
│  地址空间操作: shmem_aops (支持 swap)                        │
│  shmem_get_folio() → 分配 folio                              │
│  shmem_writepage() → swap 写出                                │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 shmem_sb_info — tmpfs 超级块信息

```c
// 文件: include/linux/shmem_fs.h
struct shmem_sb_info {
    unsigned long max_blocks;     // 最大块数 (大小限制)
    struct percpu_counter used_blocks; // 已使用的块数
    unsigned long max_inodes;     // 最大 inode 数
    struct percpu_counter used_inodes; // 已使用的 inode 数
    uid_t uid;                    // 挂载时指定的 uid
    gid_t gid;                    // 挂载时指定的 gid
    umode_t mode;                 // 挂载时指定的模式
    struct mempolicy *mpol;       // 内存策略
    struct list_head shrinklist;  // 收缩列表
    unsigned long shrinklist_len; // 收缩列表长度
    spinlock_t shrinklist_lock;   // 收缩列表锁
    struct percpu_counter *ino_next; // 下一个 inode 号
};
```

### 2.2 shmem_inode_info — tmpfs inode 信息

```c
// 文件: include/linux/shmem_fs.h
struct shmem_inode_info {
    struct shared_policy        policy;     // 内存策略
    struct list_head            swaplist;   // 交换列表
    struct simple_xattrs        xattrs;     // 扩展属性
    struct inode                vfs_inode;  // 嵌入的 VFS inode
};
```

### 2.3 ramfs 地址空间操作

```c
// 文件: fs/ramfs/inode.c
static const struct address_space_operations ram_aops = {
    .read_folio = ramfs_read_folio,     // 读取 folio
    .write_begin = simple_write_begin,  // 开始写入
    .write_end = simple_write_end,      // 结束写入
    .dirty_folio = noop_dirty_folio,    // 标记脏页 (无操作)
};
```

### 2.4 tmpfs 地址空间操作

```c
// 文件: mm/shmem.c
static const struct address_space_operations shmem_aops = {
    .writepage    = shmem_writepage,    // 写回 (可 swap 出)
    .dirty_folio  = noop_dirty_folio,
    .read_folio   = shmem_read_folio,
    .write_begin  = shmem_write_begin,
    .write_end    = shmem_write_end,
    .swap_in      = shmem_swapin_folio, // 从 swap 换入
};
```

---

## 3. API 与使用方法

### 3.1 挂载使用

```bash
# 挂载 ramfs
mount -t ramfs none /mnt/ram
mount -t ramfs -o maxsize=100M none /mnt/ram  # ramfs 也支持 maxsize

# 挂载 tmpfs
mount -t tmpfs none /tmp
mount -t tmpfs -o size=1G,uid=1000,gid=1000,mode=0755 tmpfs /mytmp

# 查看 tmpfs 使用情况
df -h /tmp
```

### 3.2 内核内部使用

```c
// 内核内部创建 ramfs 文件 (用于 rootfs)
// init/main.c
static int __init populate_rootfs(void)
{
    // 将 initramfs 解压到 rootfs
    unpack_to_rootfs(__initramfs_start, __initramfs_size);
    return 0;
}
```

### 3.3 tmpfs 实现 POSIX 共享内存

```c
// 用户空间使用 /dev/shm (tmpfs)
// 创建共享内存文件
int fd = shm_open("/my_shm", O_CREAT | O_RDWR, 0666);
ftruncate(fd, 4096);
void *addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

// 或直接使用 /dev/shm
int fd = open("/dev/shm/myfile", O_CREAT | O_RDWR, 0666);
```

---

## 4. 函数调用栈

### 4.1 ramfs 挂载

```
mount -t ramfs none /mnt/ram
  ↓ sys_mount() → do_new_mount()
    → vfs_get_tree(&ramfs_fs_type)
      → ramfs_init_fs_context(fc)
      → ramfs_fill_super(sb, fc)
        → sb->s_op = &ramfs_ops
        → ramfs_get_inode(sb, NULL, S_IFDIR | 0755, 0)
          → new_inode(sb)
          → inode->i_mapping->a_ops = &ram_aops
          → inode->i_op = &ramfs_dir_inode_operations
          → inode->i_fop = &simple_dir_operations
        → sb->s_root = d_make_root(inode)
```

### 4.2 ramfs 文件写入

```
write(fd, buf, len)
  → vfs_write() → generic_perform_write()
    → a_ops->write_begin()  → simple_write_begin()
      → grab_cache_page_write_begin()  // 获取页缓存页
        → pagecache_get_page()
    → iov_iter_copy_from_user_atomic()  // 将用户数据拷贝到页缓存
    → a_ops->write_end()  → simple_write_end()
      → mark_page_accessed()  // 标记页为已访问
      → unlock_page()         // 解锁页
```

### 4.3 tmpfs 文件写入

```
write(fd, buf, len)
  → vfs_write() → shmem_file_write_iter()
    → generic_perform_write()
      → shmem_write_begin()
        → shmem_get_folio(inode, index, &folio, SGP_WRITE)
          → shmem_alloc_folio(gfp, info, pgoff)  // 分配 folio
            → alloc_pages_mpol()  // 从伙伴系统分配物理页
          → shmem_recalc_inode()  // 更新 inode 统计
          → folio_mark_uptodate(folio)  // 标记 folio 为最新
      → copy_page_from_iter_atomic()  // 拷贝数据
      → shmem_write_end()
        → folio_mark_dirty(folio)  // 标记脏页
        → folio_unlock(folio)      // 解锁
```

### 4.4 tmpfs 内存压力回收

```
kswapd/直接回收
  → shrink_node() → shrink_list()
    → shmem_writepage(page, wbc)           // tmpfs 写回回调
      → shmem_swapout_folio(mapping, folio) // 换出到 swap
        → shmem_recalc_inode()             // 更新统计
        → __add_to_swap_cache(folio, ...)  // 加入 swap 缓存
        → swap_writepage()                 // 写入 swap 分区
      → folio_unlock(folio)
```

---

## 5. 流程图

### 5.1 ramfs vs tmpfs 对比

```
┌─────────────────────────────────────────────────────────────────┐
│                     ramfs                  tmpfs                │
│                                                                 │
│  创建文件:                             创建文件:                │
│  write(fd)                              write(fd)               │
│    ↓                                      ↓                     │
│  simple_write_begin()                  shmem_write_begin()      │
│    ↓                                      ↓                     │
│  grab_cache_page()                     shmem_get_folio()        │
│    ↓                                      ↓                     │
│  alloc_pages() ← 伙伴系统              shmem_alloc_folio()      │
│    ↓                                      ↓                     │
│  页 → 页缓存 (不可回收)               页 → 页缓存 → 可 swap   │
│    ↓                                      ↓                     │
│  [内存压力]                            shmem_writepage()        │
│  无法回收 → OOM                        → swap_writepage()      │
│                                         → 释放物理页            │
│                                                                 │
│  大小限制: 无 (危险)                   mount -o size=N          │
│  Swap: 不支持                         支持 swap 后备               │
│  典型用途: rootfs, initramfs          /tmp, /dev/shm            │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 tmpfs 内存生命周期

```
用户空间写入
    │
    ▼
┌─────────────┐
│  shmem_get  │
│  _folio()   │
└──────┬──────┘
       │
       ▼
┌──────────────────────┐
│  alloc_pages_mpol()  │  ← 从伙伴系统分配物理页
│  (物理内存中)        │
└──────────────────────┘
       │
       ├── 活跃使用 → 数据在页缓存中
       │
       ├── 内存压力 → shmem_writepage()
       │     │
       │     ▼
       │  ┌───────────────┐
       │  │ add_to_swap   │  ← 加入 swap 缓存
       │  │ _cache()      │
       │  └───────┬───────┘
       │          │
       │          ▼
       │  ┌───────────────┐
       │  │ swap_writepage│  ← 写入 swap 分区/文件
       │  │ (物理页释放)  │
       │  └───────────────┘
       │
       └── 再次访问 → shmem_swapin_folio()
             │
             ▼
          ┌───────────────┐
          │ swap_readpage │  ← 从 swap 读回
          │ (重新分配物理页)│
          └───────────────┘
```

---

## 6. 使用场景

| 场景 | 文件系统 | 描述 |
|------|----------|------|
| **rootfs (initramfs)** | ramfs | 内核启动时的初始根文件系统 |
| **/tmp** | tmpfs | 临时文件存储，重启后自动清空 |
| **/dev/shm** | tmpfs | POSIX 共享内存，`shm_open()` 实现 |
| **容器临时存储** | tmpfs | Docker 容器挂载临时数据卷 |
| **内核构建临时文件** | tmpfs | 将编译临时文件放在 tmpfs 加速 |
| **浏览器缓存** | tmpfs | 将浏览器缓存指向 tmpfs 减少磁盘写入 |
| **数据库临时表** | tmpfs | 将临时表空间放在 tmpfs 提升性能 |
| **系统日志** | tmpfs | `/var/log` 挂载 tmpfs，减少磁盘 I/O |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `fs/ramfs/inode.c` | ramfs 核心实现（inode 操作、超级块操作） |
| `fs/ramfs/file-mmu.c` | ramfs 文件操作（MMU 版本） |
| `fs/ramfs/file-nommu.c` | ramfs 文件操作（无 MMU 版本） |
| `mm/shmem.c` | tmpfs 核心实现（基于 shmem 子系统） |
| `include/linux/shmem_fs.h` | tmpfs/shmem 数据结构定义 |