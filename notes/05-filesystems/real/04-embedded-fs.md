# 嵌入式与闪存文件系统：F2FS / EROFS / SquashFS / UBIFS / FAT

> 本文覆盖 ARM64 嵌入式与移动设备常见的非 ext4/XFS 文件系统。重点是“闪存约束 → 磁盘布局 → 内存对象 → I/O 路径”的对应关系，不展开每个函数的逐行实现。
>
> 当前 `.config` 只启用了 ext4；本文涉及的 F2FS、EROFS、SquashFS、UBIFS、FAT/exFAT、NTFS3 均为“源码存在，当前配置未启用”。

---

## 1. 为什么需要这些文件系统

通用服务器文件系统（ext4/XFS）假设底层是随机读写块设备；嵌入式场景的约束不同：

| 约束 | 影响 | 典型选择 |
|---|---|---|
| NAND/NOR 闪存有擦除块、写放大 | 需要日志结构、GC、磨损均衡 | F2FS、UBIFS |
| 只读系统分区 | 需要高压缩比、随机读、低内存 | EROFS、SquashFS |
| 掉电频繁 | 需要 checkpoint/日志/原子提交 | F2FS、UBIFS、ext4 |
| 可移动介质 | 需要兼容 PC/相机/车机 | FAT/exFAT、NTFS3 |
| 内存/CPU 受限 | 解压要快、元数据要小 | EROFS、SquashFS |

Linux 的文件系统统一通过 VFS 接入：每个文件系统实现 `super_operations`、`inode_operations`、`file_operations`、`address_space_operations`，再通过 `register_filesystem()` 注册 `file_system_type`。这些文件系统的差异主要在三处：

1. **磁盘/闪存布局**：超级块、inode、数据块、日志区如何组织；
2. **块映射**：逻辑块号如何映射到物理位置（F2FS 的 NAT/SIT、UBIFS 的 LEB、FAT 的簇链）；
3. **空间回收**：GC/checkpoint/压缩块回收策略。

---

## 2. F2FS（Flash-Friendly File System）

### 2.1 设计目标

F2FS 是面向 NAND 的日志结构文件系统，核心思想是“顺序写、分段管理、冷热分离”：

- 闪存按 **segment**（通常 2MB）分配，多个 segment 组成 **section**，多个 section 组成 **zone**；
- 数据、节点、元数据分开写，减少随机写和写放大；
- 通过 **checkpoint** 保存一致性状态，通过 **GC** 回收无效块；
- 支持压缩、内联数据、多设备、zoned 块设备。

### 2.2 磁盘布局

F2FS 的超级块位于分区的固定偏移，关键区域包括：

```
+----------------------+  block 0
| Superblock #0        |
+----------------------+
| Superblock #1        |
+----------------------+
| Checkpoint (CP)      |  ← 两个 checkpoint 轮换
+----------------------+
| SIT (Segment Info)   |  ← segment 有效块计数
+----------------------+
| NAT (Node Address)   |  ← node id → 物理块地址
+----------------------+
| SSA (Summary Area)   |  ← segment 内各块的摘要
+----------------------+
| Main Area            |  ← data / node / meta 三类 block
+----------------------+
```

关键结构：

| 结构 | 作用 |
|---|---|
| `struct f2fs_super_block` | 磁盘超级块：魔数、段大小、区域偏移、feature |
| `struct f2fs_checkpoint` | 一致性快照：版本、CP pack、NAT/SIT 位图、当前段 |
| `struct f2fs_sb_info` | 内存超级块：指向上述区域、全局锁、统计 |
| `struct f2fs_inode` | 磁盘 inode：内联数据、extent 信息、NAT 指针 |
| `struct f2fs_node` | node block 的通用封装（inode/direct/indirect） |
| `struct f2fs_nm_info` | NAT 缓存与位图 |
| `struct f2fs_sm_info` | segment 管理、空闲段、GC 候选 |

### 2.3 写路径

以 buffered write 为例：

```
write()
  → generic_perform_write()
  → f2fs_write_begin()
  → f2fs_get_block() / f2fs_map_blocks()
  → f2fs_do_write_data_page()
       ├─ 分配新的数据块（f2fs_allocate_data_block）
       ├─ 更新 NAT（node 地址）
       ├─ 写 SSA / SIT
       └─ 提交 bio
  → f2fs_write_end()
```

关键点：

- **追加写**：数据块尽量写到当前活跃段，避免覆盖旧块；
- **NAT 间接层**：inode 中的 node id 通过 NAT 找到物理地址，更新地址即可完成“覆盖写”；
- **checkpoint**：在 `f2fs_write_checkpoint()` 中把 NAT/SIT/SSA 的脏状态落盘，并切换 CP pack；
- **GC**：`f2fs_gc()` 选择 victim segment，把有效块搬到新段，再回收旧段。前台 GC 由 `f2fs_balance_fs()` 触发，后台由 `f2fs_gc_thread` 触发；
- **压缩**：`f2fs_compress_write_end()` 等路径把 cluster 压缩后写入，读取时解压。

### 2.4 关键源码

| 文件 | 作用 |
|---|---|
| `fs/f2fs/super.c` | 挂载、`f2fs_sb_info` 初始化 |
| `fs/f2fs/checkpoint.c` | checkpoint 读写与恢复 |
| `fs/f2fs/node.c` | NAT 管理 |
| `fs/f2fs/segment.c` | SIT/SSA、段分配 |
| `fs/f2fs/data.c` | 数据读写、`f2fs_do_write_data_page()` |
| `fs/f2fs/gc.c` | 前台/后台 GC |
| `fs/f2fs/compress.c` | 压缩 cluster |
| `fs/f2fs/f2fs.h` | 全部核心结构体 |

---

## 3. EROFS（Enhanced Read-Only File System）

### 3.1 设计目标

EROFS 是只读、压缩、面向移动设备的文件系统，强调：

- 压缩块按需解压，支持 LZ4/LZMA/Zstd/Deflate；
- 元数据紧凑，支持 inline data 和 tail-packing；
- 支持 fscache/erofs over file，便于容器镜像分层；
- 挂载快、内存占用低。

### 3.2 磁盘布局

EROFS 的超级块在固定偏移，核心区域：

```
+----------------------+
| Superblock           |  magic, blocksize, root_nid, meta_blkaddr
+----------------------+
| Inode table          |  erofs_inode_compact / erofs_inode_extended
+----------------------+
| Directory data       |  目录项（name + nid）
+----------------------+
| Compressed data      |  压缩 cluster + index
+----------------------+
```

关键结构：

| 结构 | 作用 |
|---|---|
| `struct erofs_sb_info` | 内存超级块：块大小、根 inode、特性、压缩配置 |
| `struct erofs_inode` | 内存 inode：nid、layout、压缩信息、xattr |
| `struct erofs_map_blocks` | 逻辑块到物理块的映射结果 |
| `struct z_erofs_decompress_req` | 压缩解压请求 |

### 3.3 读路径

```
read()
  → filemap_read()
  → erofs_read_folio()
  → erofs_map_blocks()
       ├─ 普通块：直接返回物理块号
       └─ 压缩块：返回压缩 cluster 的物理范围
  → 若压缩：z_erofs_decompress()
       ├─ 选择算法（LZ4/LZMA/Zstd/Deflate）
       ├─ 解压到 folio
       └─ 处理 page cache 中的重叠/部分页
```

关键点：

- **压缩索引**：每个压缩 cluster 带 index，记录逻辑块到压缩块的偏移；
- **按需解压**：只解压被访问的 cluster，不预解压整个文件；
- **inline data**：小文件数据直接放在 inode 尾部，减少一次 I/O；
- **fscache**：`erofs_fscache_*` 支持把远端 blob 作为后端，适合容器镜像。

### 3.4 关键源码

| 文件 | 作用 |
|---|---|
| `fs/erofs/super.c` | 挂载、超级块解析 |
| `fs/erofs/inode.c` | inode 读取与布局解析 |
| `fs/erofs/data.c` | `erofs_read_folio()`、map_blocks |
| `fs/erofs/decompressor.c` | LZ4 等解压器注册与调度 |
| `fs/erofs/zdata.c` | 压缩 cluster 的读取与解压 |
| `fs/erofs/internal.h` | `erofs_sb_info`、`erofs_inode` |

---

## 4. SquashFS

### 4.1 设计目标

SquashFS 是经典的只读压缩文件系统，常用于 LiveCD、嵌入式 rootfs、容器镜像：

- 块级压缩（默认 128KB），元数据也压缩；
- 支持 fragment（小文件尾部合并）、稀疏文件、硬链接；
- 只读，挂载后不需要写回。

### 4.2 布局与读路径

```
+----------------------+
| Superblock           |
+----------------------+
| Compression options  |
+----------------------+
| Data blocks          |  ← 压缩的数据块
+----------------------+
| Inode table          |  ← 压缩的 inode 元数据
+----------------------+
| Directory table      |  ← 压缩的目录项
+----------------------+
| Fragment table       |  ← 小文件尾部
+----------------------+
| Export table         |  ← NFS export / inode 查找
+----------------------+
```

读路径：

```
squashfs_readpage()
  → squashfs_read_data()
       ├─ 读压缩块
       ├─ 调用 decompressor（LZ4/LZO/XZ/Zstd）
       └─ 填充 page cache
  → 若为 fragment：squashfs_readpage_fragment()
  → 若为稀疏块：squashfs_readpage_sparse()
```

关键结构：

- `struct squashfs_sb_info`：块大小、压缩器、cache、fragment 表；
- `struct squashfs_base_inode`：inode 类型、mode、uid/gid；
- `struct squashfs_reg_inode` / `squashfs_lreg_inode`：普通文件块列表；
- `struct squashfs_fragment_entry`：fragment 块位置。

### 4.3 关键源码

| 文件 | 作用 |
|---|---|
| `fs/squashfs/super.c` | 挂载与超级块 |
| `fs/squashfs/inode.c` | inode 解析 |
| `fs/squashfs/file.c` | `squashfs_readpage()` |
| `fs/squashfs/block.c` | 压缩块读取 |
| `fs/squashfs/decompressor.c` | 解压器注册 |
| `fs/squashfs/fragment.c` | fragment 表 |
| `fs/squashfs/squashfs_fs.h` | 磁盘格式定义 |

---

## 5. UBIFS

### 5.1 设计目标

UBIFS 建立在 UBI（Unsorted Block Images）之上，面向裸 NAND：

- UBI 负责磨损均衡、坏块管理、逻辑擦除块（LEB）到物理擦除块（PEB）的映射；
- UBIFS 负责日志、索引、空间预算、GC；
- 掉电安全依赖 journal + commit。

### 5.2 分层

```
VFS
  → UBIFS
       ├─ journal（日志区）
       ├─ index（B-tree 索引）
       ├─ budgeting（空间预算）
       └─ commit（提交）
  → UBI
       ├─ volume table
       ├─ LEB → PEB 映射
       └─ wear leveling / bad block
  → MTD
       ├─ NAND/NOR driver
       └─ ECC
```

关键结构：

| 结构 | 作用 |
|---|---|
| `struct ubifs_info` | 超级块：LEB 大小、日志、索引、预算、GC |
| `struct ubifs_znode` | 索引 B-tree 节点 |
| `struct ubifs_jhead` | journal head，记录日志区写位置 |
| `struct ubifs_bud` | budget 节点，跟踪脏空间 |

### 5.3 写路径与提交

```
write()
  → ubifs_write_begin()
  → ubifs_jnl_write_data()
       ├─ 写 data node 到 journal
       ├─ 更新 index（TNC）
       └─ 记录 budget
  → ubifs_write_end()
  → fsync() / writeback
       └─ ubifs_jnl_commit() → commit checkpoint
```

关键点：

- **journal**：所有元数据/数据先写日志区，保证掉电可恢复；
- **commit**：把 journal 中的变更合并到主区，更新 master node；
- **GC**：`ubifs_garbage_collect()` 选择 victim LEB，搬移有效数据；
- **budget**：`ubifs_budget_space()` 在写之前预留空间，避免中途 ENOSPC。

### 5.4 关键源码

| 文件 | 作用 |
|---|---|
| `fs/ubifs/super.c` | 挂载、`ubifs_info` 初始化 |
| `fs/ubifs/journal.c` | `ubifs_jnl_write_data()`、commit |
| `fs/ubifs/tnc.c` | 索引 B-tree |
| `fs/ubifs/gc.c` | 垃圾回收 |
| `fs/ubifs/budget.c` | 空间预算 |
| `fs/ubifs/ubifs.h` | 核心结构体 |
| `drivers/mtd/ubi/` | UBI 层 |

---

## 6. FAT / exFAT / NTFS3

### 6.1 FAT

FAT 的核心是 **簇链**：

```
目录项 (8.3 或 LFN)
  ├─ 起始簇号
  └─ 文件大小
FAT 表
  └─ 每个簇指向下一个簇，EOF 结束
数据区
  └─ 按簇存放文件数据
```

关键结构：

- `struct msdos_sb_info`：FAT 类型（12/16/32）、簇大小、FAT 表位置、根目录；
- `struct msdos_inode_info`：起始簇、目录项位置；
- `fat_get_cluster()` / `fat_chain_add()`：簇链遍历与追加；
- `fat_ent_read()` / `fat_ent_write()`：FAT 表项读写。

FAT 没有日志，掉电可能损坏簇链；Linux 通过 `fsck.fat` 修复。

### 6.2 exFAT

exFAT 针对大容量 SD 卡：

- 使用 **簇位图** 而不是 FAT 链分配空闲簇；
- 目录项是 32 字节的 entry set，支持文件名哈希、连续簇、流扩展；
- 支持 upcase 表、TexFAT（事务性 FAT）。

关键结构：

- `struct exfat_sb_info`：簇大小、FAT/位图位置、根目录；
- `struct exfat_inode_info`：起始簇、entry 位置、连续簇标志；
- `exfat_alloc_cluster()` / `exfat_free_cluster()`：位图分配。

### 6.3 NTFS3

NTFS3 是 Linux 内核对 NTFS 的读写实现：

- 以 **MFT（Master File Table）** 组织文件记录；
- 属性（$DATA、$INDEX_ALLOCATION、$BITMAP 等）挂在文件记录上；
- 支持日志（$LogFile）、压缩、稀疏、重解析点；
- 源码 `fs/ntfs3/`，核心是 `fs/ntfs3/fsntfs.c`、`inode.c`、`file.c`、`attrib.c`。

### 6.4 关键源码

| 文件 | 作用 |
|---|---|
| `fs/fat/inode.c` | FAT 挂载与超级块 |
| `fs/fat/fatent.c` | FAT 表项读写 |
| `fs/fat/dir.c` | 目录项解析 |
| `fs/exfat/super.c` | exFAT 挂载 |
| `fs/exfat/balloc.c` | 簇位图分配 |
| `fs/exfat/dir.c` | entry set 解析 |
| `fs/ntfs3/fsntfs.c` | NTFS 卷/MFT |
| `fs/ntfs3/attrib.c` | 属性解析 |

---

## 7. 对比与选型

| 文件系统 | 读写 | 压缩 | 掉电安全 | 典型场景 |
|---|---|---|---|---|
| F2FS | 读写 | 支持 cluster 压缩 | checkpoint + journal | Android data 分区、UFS/eMMC |
| EROFS | 只读 | LZ4/LZMA/Zstd/Deflate | 不适用（只读） | 容器镜像、只读系统分区 |
| SquashFS | 只读 | 块级多算法 | 不适用（只读） | LiveCD、rootfs、恢复分区 |
| UBIFS | 读写 | 支持压缩 | journal + commit | 裸 NAND、路由器、IoT |
| FAT | 读写 | 无 | 无日志 | SD 卡、U 盘、EFI 分区 |
| exFAT | 读写 | 无 | TexFAT 可选 | 大容量 SD 卡、移动存储 |
| NTFS3 | 读写 | 支持 | $LogFile | Windows 双系统、移动硬盘 |

选型建议：

- **只读系统分区**：优先 EROFS（压缩比/随机读/内存综合较好），兼容性要求高时用 SquashFS；
- **Android/data 分区**：F2FS；
- **裸 NAND**：UBIFS + UBI；
- **可移动介质/EFI**：FAT/exFAT；
- **Windows 互操作**：NTFS3。

---

## 8. 关键文件清单

| 文件 | 作用 |
|---|---|
| `fs/f2fs/f2fs.h` | F2FS 核心结构体 |
| `fs/f2fs/checkpoint.c` | checkpoint |
| `fs/f2fs/segment.c` | 段管理/SIT |
| `fs/f2fs/node.c` | NAT |
| `fs/f2fs/data.c` | 数据读写 |
| `fs/f2fs/gc.c` | GC |
| `fs/erofs/internal.h` | EROFS 核心结构体 |
| `fs/erofs/data.c` | EROFS 读路径 |
| `fs/erofs/zdata.c` | 压缩 cluster |
| `fs/squashfs/file.c` | SquashFS 读路径 |
| `fs/squashfs/block.c` | 压缩块读取 |
| `fs/ubifs/journal.c` | UBIFS 日志 |
| `fs/ubifs/tnc.c` | UBIFS 索引 |
| `fs/ubifs/gc.c` | UBIFS GC |
| `fs/fat/fatent.c` | FAT 表项 |
| `fs/exfat/balloc.c` | exFAT 簇位图 |
| `fs/ntfs3/fsntfs.c` | NTFS3 MFT |
| `drivers/mtd/ubi/` | UBI 层 |
