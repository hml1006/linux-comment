# linkat 系统调用分析

## 1. 原理与功能

**linkat** 用于创建硬链接，是 `link(2)` 的 `at` 系列扩展版本。硬链接允许一个文件有多个目录项指向同一个 inode。

**ARM64 系统调用号：** 37 (__NR_linkat)

**原型：**

```c
int linkat(int olddirfd, const char *oldpath,
           int newdirfd, const char *newpath, int flags);
```

**参数说明：**
- `olddirfd`：源文件路径的目录 fd
- `oldpath`：源文件路径
- `newdirfd`：新链接路径的目录 fd
- `newpath`：新链接路径
- `flags`：控制标志位
  - `AT_SYMLINK_NOFOLLOW`（0x100）：不跟随源符号链接（与 link 不同，linkat 默认跟随符号链接）
  - `AT_EMPTY_PATH`（0x1000）：允许通过 olddirfd 作为源文件

**限制：**
- 不能跨文件系统创建硬链接（`vfs_link` 中检查 `dir->i_sb != inode->i_sb` 返回 `-EXDEV`）
- 不能为目录创建硬链接（`vfs_link` 中检查 `S_ISDIR(inode->i_mode)` 返回 `-EPERM`）
- 需要目标文件路径的写权限和 inode 索引节点链接计数支持
- `EXT4_LINK_MAX`（65000）限制单个文件的硬链接数

---

## 2. 执行流程总览

```
linkat(olddirfd, oldpath, newdirfd, newpath, flags)
                          │
                          ├─ 检查 flags 合法性
                          │
                          ├─ filename_lookup(olddfd, old, ...)     // 查找源文件路径
                          │    └─ path_openat → link_path_walk    // 沿目录树查找源文件
                          │         └─ ext4_lookup(dir, dentry)   // 在父目录中搜索
                          │              ├─ ext4_lookup_entry()   // 查找 ext4_dir_entry_2
                          │              └─ ext4_iget(inode_no)   // 读取 inode
                          │
                          ├─ filename_create(newdfd, new, ...)     // 创建目标路径 dentry
                          │
                          ├─ 检查 old_path.mnt == new_path.mnt    // 不可跨文件系统
                          │
                          ├─ may_linkat(&old_path)                // 安全策略检查
                          │
                          └─ vfs_link(old_dentry, idmap, dir, new_dentry, ...)
                               │
                               ├─ may_create_dentry()             // 目标目录权限检查
                               ├─ IS_APPEND/IMMUTABLE 检查        // 源文件不可追加/不可变
                               ├─ security_inode_link()           // LSM 钩子
                               ├─ inode_lock(source_inode)        // 锁住源 inode
                               ├─ i_nlink 检查                    // 未删除且未超限
                               ├─ dir->i_op->link()               // 文件系统实现
                               │    └─ ext4_link(old_dentry, dir, new_dentry)
                               │         └─ __ext4_link(dir, inode, dentry)
                               │              ├─ ext4_journal_start()  // 开启事务
                               │              ├─ ext4_inc_count(inode) // i_nlink++
                               │              ├─ ext4_add_entry()      // 添加目录项
                               │              │    ├─ ext4_add_dirent_to_inline (inline)
                               │              │    ├─ ext4_dx_add_entry  (HTree)
                               │              │    └─ add_dirent_to_buf  (线性)
                               │              │         └─ ext4_insert_dentry()
                               │              ├─ ext4_mark_inode_dirty() // 写回 inode
                               │              ├─ d_instantiate()         // dentry ← inode
                               │              └─ ext4_fc_track_link()   // 快速提交日志
                               │
                               └─ fsnotify_link()                 // 通知
```

---

## 3. 函数调用栈

```
linkat (用户态)
  └─ syscall(__NR_linkat, olddirfd, oldpath, newdirfd, newpath, flags)
       └─ __arm64_sys_linkat()
            └─ filename_linkat(olddfd, old, newdfd, new, flags)         // fs/namei.c:6132
                 ├─ filename_lookup(olddfd, old, how, &old_path, NULL)  // 查找源文件
                 │    └─ path_openat / link_path_walk
                 │         └─ ext4_lookup(dir, dentry, flags)           // fs/ext4/namei.c:1860
                 │              ├─ ext4_lookup_entry(dir, dentry, &de)  // 查找目录项
                 │              │    └─ __ext4_find_entry(dir, &fname, &de, NULL)
                 │              │         ├─ [inline] → 在 inline 数据中查找
                 │              │         ├─ [HTree]  → ext4_dx_find_entry()
                 │              │         └─ [线性]   → 遍历目录块:
                 │              │              ├─ ext4_read_dirblock()  // 读取目录块
                 │              │              └─ search_dirblock()     // 搜索目录项
                 │              │                   └─ ext4_match()     // 匹配文件名
                 │              │
                 │              ├─ le32_to_cpu(de->inode)               // 获取 inode 号
                 │              └─ ext4_iget(dir->i_sb, ino, ...)       // 读取 inode
                 │                   └─ __ext4_iget()                   // fs/ext4/inode.c:5429
                 │                        ├─ ext4_get_inode_loc()      // 定位 inode 块
                 │                        ├─ ext4_read_inode_bitmap()   // 校验 inode 有效性
                 │                        ├─ ext4_inode_csum_verify()   // 校验和
                 │                        └─ inode_set_flags()          // 初始化 VFS inode
                 │
                 ├─ filename_create(newdfd, new, &new_path, ...)        // 准备目标路径
                 ├─ may_linkat(idmap, &old_path)                        // 安全策略检查
                 │    ├─ protected_hardlinks sysctl 检查                // 非所有者禁止链接
                 │    └─ inode_owner_or_capable() 检查                  // 权限检查
                 │
                 └─ vfs_link(old_dentry, idmap, dir, new_dentry, NULL)  // fs/namei.c:6058
                      ├─ may_create_dentry(idmap, dir, new_dentry)      // 目标目录可写检查
                      ├─ IS_APPEND / IS_IMMUTABLE 检查                  // 源文件属性检查
                      ├─ HAS_UNMAPPED_ID 检查                           // idmap 一致性
                      ├─ security_inode_link(old_dentry, dir, new_dentry) // LSM
                      ├─ inode_lock(source_inode)                       // 锁源 inode
                      ├─ i_nlink 完整性检查                             // 0 → -ENOENT
                      └─ ext4_link(old_dentry, dir, new_dentry)         // fs/ext4/namei.c:3602
                           ├─ fscrypt_prepare_link()                    // 加密准备
                           ├─ 项目 ID 一致性检查 (EXT4_INODE_PROJINHERIT)
                           └─ __ext4_link(dir, inode, dentry)           // fs/ext4/namei.c:3564
                                ├─ ext4_journal_start()                 // 开启 JBD2 事务
                                ├─ inode_set_ctime_current(inode)       // 更新 ctime
                                ├─ ext4_inc_count(inode)                // i_nlink++
                                │    └─ inc_nlink(inode)
                                ├─ ihold(inode)                         // 增加引用计数
                                ├─ ext4_add_entry(handle, dentry, inode) // 添加目录项
                                │    └─ [HTree] ext4_dx_add_entry()
                                │         ├─ dx_probe()                 // 定位 HTree 节点
                                │         ├─ add_dirent_to_buf()        // 插入目录项
                                │         │    └─ ext4_insert_dentry()  // 写入 ext4_dir_entry_2
                                │         │         ├─ de->inode = cpu_to_le32(inode->i_ino)
                                │         │         ├─ ext4_set_de_type()  // 设置文件类型
                                │         │         └─ 写入文件名
                                │         └─ ext4_handle_dirty_dx_node() // 写回 HTree 节点
                                │
                                ├─ ext4_mark_inode_dirty(handle, inode)  // 标记 inode 脏
                                ├─ [i_nlink == 1] → ext4_orphan_del()   // 删除孤儿标记
                                ├─ d_instantiate(dentry, inode)          // dentry 关联 inode
                                └─ ext4_fc_track_link()                  // 快速提交日志
```

---

## 4. ext4 硬链接创建过程与数据变化

### 4.1 磁盘数据变化

创建硬链接时，ext4 文件系统涉及以下磁盘数据结构的修改：

```
[硬链接创建前]

  inode #100 (原文件, i_nlink=1)
    ├─ i_links_count = 1
    ├─ i_blocks = 8
    └─ 数据块 [0] [1]    ← 实际文件数据

  目录块 (dir block, 父目录)
    ├─ ext4_dir_entry_2: inode=100, name="original.txt"
    ├─ ext4_dir_entry_2: inode=200, name="other_file"
    └─ [空闲空间]


[硬链接创建后]

  inode #100 (i_nlink=2)          ← 同一 inode，同一份数据
    ├─ i_links_count = 2           ← 关键变化：链接计数 +1
    ├─ i_blocks = 8                ← 不变，不占用额外数据块
    └─ 数据块 [0] [1]             ← 不变，数据完全共享

  目录块 (父目录)
    ├─ ext4_dir_entry_2: inode=100, name="original.txt"
    ├─ ext4_dir_entry_2: inode=100, name="hardlink.txt"  ← 新增条目
    ├─ ext4_dir_entry_2: inode=200, name="other_file"
    └─ [减少的空闲空间]
```

### 4.2 关键代码路径

#### `ext4_link` — 入口 (fs/ext4/namei.c:3602)

```c
static int ext4_link(struct dentry *old_dentry,
                     struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(old_dentry);
    int err;

    // 检查硬链接数是否超过 EXT4_LINK_MAX (65000)
    if (inode->i_nlink >= EXT4_LINK_MAX)
        return -EMLINK;

    // 加密文件系统准备
    err = fscrypt_prepare_link(old_dentry, dir, dentry);
    if (err)
        return err;

    // 项目继承模式：检查目录与文件的 project ID 一致
    if ((ext4_test_inode_flag(dir, EXT4_INODE_PROJINHERIT)) &&
        (!projid_eq(EXT4_I(dir)->i_projid,
                    EXT4_I(old_dentry->d_inode)->i_projid)))
        return -EXDEV;

    err = dquot_initialize(dir);   // 配额初始化
    if (err)
        return err;
    return __ext4_link(dir, inode, dentry);
}
```

#### `__ext4_link` — 核心实现 (fs/ext4/namei.c:3564)

```c
int __ext4_link(struct inode *dir, struct inode *inode, struct dentry *dentry)
{
    handle_t *handle;
    int err, retries = 0;
retry:
    // 开启 JBD2 事务，预留足够的数据块和索引块空间
    handle = ext4_journal_start(dir, EXT4_HT_DIR,
        (EXT4_DATA_TRANS_BLOCKS(dir->i_sb) +
         EXT4_INDEX_EXTRA_TRANS_BLOCKS) + 1);
    if (IS_ERR(handle))
        return PTR_ERR(handle);

    if (IS_DIRSYNC(dir))
        ext4_handle_sync(handle);

    // 1. 更新 inode 的 ctime 时间戳
    inode_set_ctime_current(inode);

    // 2. 增加 inode 的硬链接计数 (i_nlink++)
    ext4_inc_count(inode);

    // 3. 增加 inode 引用计数，防止在后续操作中被释放
    ihold(inode);

    // 4. 在父目录中添加新的目录项 (ext4_dir_entry_2)
    //    写入 inode 号、文件名、文件类型
    err = ext4_add_entry(handle, dentry, inode);
    if (!err) {
        // 5. 标记 inode 为脏（将 i_links_count 等字段写回磁盘）
        err = ext4_mark_inode_dirty(handle, inode);
        // 如果是 tmpfile 首次被链接，从孤儿列表删除
        if (inode->i_nlink == 1)
            ext4_orphan_del(handle, inode);
        // 6. 将 dentry 与 inode 关联（dcache 缓存）
        d_instantiate(dentry, inode);
        // 7. 记录快速提交日志
        ext4_fc_track_link(handle, dentry);
    } else {
        // 失败时回滚：恢复 i_nlink
        drop_nlink(inode);
        iput(inode);
    }
    ext4_journal_stop(handle);
    if (err == -ENOSPC && ext4_should_retry_alloc(dir->i_sb, &retries))
        goto retry;
    return err;
}
```

#### `ext4_inc_count` — 链接计数递增 (fs/ext4/namei.c:2868)

```c
static void ext4_inc_count(struct inode *inode)
{
    inc_nlink(inode);                // i_nlink++
    // 对于 HTree 索引目录的特殊处理：
    // 如果目录的链接数超过 EXT4_LINK_MAX (65000) 或变为 2，
    // 将链接数重置为 1，防止目录硬链接数溢出
    if (is_dx(inode) &&
        (inode->i_nlink > EXT4_LINK_MAX || inode->i_nlink == 2))
        set_nlink(inode, 1);
}
```

#### `ext4_insert_dentry` — 插入目录项 (fs/ext4/namei.c:2184)

```c
void ext4_insert_dentry(struct inode *dir,
                        struct inode *inode,
                        struct ext4_dir_entry_2 *de,
                        int buf_size,
                        struct ext4_filename *fname)
{
    int nlen, rlen;

    nlen = ext4_dir_rec_len(de->name_len, dir);    // 已有条目长度
    rlen = ext4_rec_len_from_disk(de->rec_len, buf_size); // 总记录长度

    if (de->inode) {
        // 如果当前位置已被占用，在条目末尾分裂出空闲空间
        struct ext4_dir_entry_2 *de1 =
            (struct ext4_dir_entry_2 *)((char *)de + nlen);
        de1->rec_len = ext4_rec_len_to_disk(rlen - nlen, buf_size);
        de->rec_len = ext4_rec_len_to_disk(nlen, buf_size);
        de = de1;  // 新条目写入分裂出的空间
    }
    // 写入目录项数据
    de->file_type = EXT4_FT_UNKNOWN;
    de->inode = cpu_to_le32(inode->i_ino);  // 写入 inode 号
    ext4_set_de_type(inode->i_sb, de, inode->i_mode);  // 设置文件类型
    // 写入文件名
    ...
}
```

### 4.3 磁盘数据结构变化总结

| 磁盘结构 | 变化 | 说明 |
|---------|------|------|
| 源文件 inode 表项 | `i_links_count++` | 关键变化，inode 的链接计数递增 |
| 源文件 inode 表项 | `i_ctime` 更新 | 时间戳更新 |
| 父目录目录块 | 新增 `ext4_dir_entry_2` | 写入 inode 号、文件名、文件类型 |
| 父目录 inode | `i_mtime`/`i_ctime` 更新 | 目录修改时间更新 |
| HTree 索引块 | 可能更新哈希树 | 大目录需要调整 HTree 结构 |
| 数据块 | **无变化** | 硬链接不复制数据，指向同一组数据块 |

---

## 5. 打开硬链接文件时如何找到真正文件

### 5.1 核心原理

硬链接不存储"指向原文件的路径"，而是**直接指向同一个 inode**。打开硬链接时，通过 `ext4_lookup` 从目录项中读取 inode 号，然后通过 `ext4_iget` 直接读取该 inode，**无论有多少个硬链接，最终都定位到同一个 inode**。

```
文件名 A (original.txt)  ─→  inode=100  ─→  ext4_iget(100)  ─→  数据块
文件名 B (hardlink.txt)  ─→  inode=100  ─→  ext4_iget(100)  ─→  数据块 (同一份)
```

关键区别对比：

| 特性 | 硬链接 | 符号链接 |
|------|--------|---------|
| inode 号 | 与源文件相同 | 独立 inode，内容为路径字符串 |
| 数据块 | 共享同一组 | 符号链接自身占用一个 inode |
| 删除源文件 | 数据仍可通过硬链接访问 | 链接失效（悬空） |
| 查找方式 | `ext4_lookup` → `ext4_iget(inode_no)` | `ext4_lookup` → `ext4_iget(symlink_inode)` → `ext4_get_link` → 读取路径 → 重新 `path_openat` |

### 5.2 打开硬链接的完整流程

```
open("/path/to/hardlink.txt", O_RDONLY)
  │
  └─ do_sys_open()                                    // fs/open.c
       └─ do_filp_open(dfd, filename, flags)           // fs/namei.c
            └─ path_openat(dfd, filename, flags)       // fs/namei.c
                 └─ link_path_walk(name, nd)           // 逐级解析路径
                      │
                      └─ walk_component(nd, ...)       // 解析路径分量
                           │
                           └─ lookup_fast(nd, ...)     // 尝试 dcache 快速查找
                                │ 或
                                └─ lookup_slow(nd, ...) // dcache 未命中，调用文件系统
                                     │
                                     └─ inode->i_op->lookup(dir, dentry, flags)
                                          │
                                          └─ ext4_lookup(dir, dentry, flags)
                                               │         // fs/ext4/namei.c:1860
                                               │
                                               ├─ ext4_lookup_entry(dir, dentry, &de)
                                               │    │
                                               │    └─ __ext4_find_entry(dir, &fname, &de, NULL)
                                               │         │ // fs/ext4/namei.c:1530
                                               │         │ 在父目录中搜索文件名
                                               │         │
                                               │         ├─ [HTree 目录]
                                               │         │    └─ ext4_dx_find_entry()
                                               │         │         ├─ dx_probe()  // 定位 HTree 节点
                                               │         │         └─ search_dirblock()  // 搜索叶子块
                                               │         │
                                               │         └─ [线性目录]
                                               │              └─ 遍历目录块:
                                               │                   ├─ ext4_read_dirblock()  // 读目录块
                                               │                   └─ search_dirblock()
                                               │                        └─ ext4_match()  // 匹配文件名
                                               │
                                               ├─ le32_to_cpu(de->inode)
                                               │    // 从 ext4_dir_entry_2 中提取 inode 号
                                               │    // 此 inode 号与源文件的 inode 号完全相同
                                               │
                                               └─ ext4_iget(dir->i_sb, ino, EXT4_IGET_NORMAL)
                                                    │ // fs/ext4/inode.c:5429
                                                    │ 根据 inode 号从磁盘读取 inode
                                                    │
                                                    ├─ ext4_get_inode_loc()    // 定位 inode 表位置
                                                    │    // 根据 ino 计算 inode 所在的块组和块号
                                                    │    // block_group = (ino-1) / inodes_per_group
                                                    │    // inode_table_block = bg->bg_inode_table
                                                    │    // offset = ((ino-1) % inodes_per_group) * inode_size
                                                    │
                                                    ├─ ext4_read_inode_bitmap() // 校验 inode 有效性
                                                    │
                                                    ├─ 读取磁盘 inode 块到 buffer_head
                                                    │    // 读取 ext4_inode 原始数据
                                                    │
                                                    ├─ ext4_inode_csum_verify() // 校验 inode 校验和
                                                    │
                                                    └─ 填充 VFS struct inode:
                                                         ├─ inode->i_mode = le16_to_cpu(raw->i_mode)
                                                         ├─ inode->i_nlink = le16_to_cpu(raw->i_links_count)
                                                         ├─ inode->i_size = le32_to_cpu(raw->i_size_lo)
                                                         ├─ i_data 从 raw->i_block[] 读取
                                                         └─ inode->i_fop = &ext4_file_operations
                                                              // 设置文件操作表
                                                              // 后续读写都通过此表操作
```

### 5.3 `ext4_lookup` 源码分析 (fs/ext4/namei.c:1860)

```c
static struct dentry *ext4_lookup(struct inode *dir, struct dentry *dentry,
                                  unsigned int flags)
{
    struct inode *inode;
    struct ext4_dir_entry_2 *de = NULL;
    struct buffer_head *bh;

    // 检查文件名长度
    if (dentry->d_name.len > EXT4_NAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    // 1. 在父目录中查找目录项
    bh = ext4_lookup_entry(dir, dentry, &de);
    if (IS_ERR(bh))
        return ERR_CAST(bh);

    inode = NULL;
    if (bh) {
        // 2. 从目录项中提取 inode 号
        __u32 ino = le32_to_cpu(de->inode);
        brelse(bh);  // 释放目录块缓冲区

        // 3. 校验 inode 号合法性
        if (!ext4_valid_inum(dir->i_sb, ino)) {
            EXT4_ERROR_INODE(dir, "bad inode number: %u", ino);
            return ERR_PTR(-EFSCORRUPTED);
        }
        // 检查目录项是否错误地指向父目录自身
        if (unlikely(ino == dir->i_ino)) {
            EXT4_ERROR_INODE(dir, "'%pd' linked to parent dir", dentry);
            return ERR_PTR(-EFSCORRUPTED);
        }

        // 4. 关键步骤：通过 inode 号读取 inode
        //    无论有多少个硬链接指向同一个文件，
        //    这里读取的都是同一个 inode 对象
        inode = ext4_iget(dir->i_sb, ino, EXT4_IGET_NORMAL);
        // 如果 inode 已被删除，返回 -ESTALE
        if (inode == ERR_PTR(-ESTALE)) {
            ...
        }
    }
    // 5. 将 dentry 与新找到的 inode 关联
    return d_splice_alias(inode, dentry);
}
```

### 5.4 `ext4_iget` — 从 inode 号读取 inode (fs/ext4/inode.c:5429)

```c
struct inode *__ext4_iget(struct super_block *sb, unsigned long ino,
                          ext4_iget_flags flags, const char *function,
                          unsigned int line)
{
    struct ext4_iloc iloc;         // inode 磁盘位置
    struct ext4_inode *raw_inode;  // 磁盘原始 inode 数据
    struct inode *inode;           // VFS inode
    ...

    // 1. 计算 inode 在磁盘上的位置
    //    inode 号 → 块组号 → inode 表块号 → 块内偏移
    //    ext4_get_inode_loc() 根据 ino 计算:
    //      block_group = (ino - 1) / EXT4_INODES_PER_GROUP(sb)
    //      block = ext4_inode_table(sb, gdp) +
    //              (ino - 1) % EXT4_INODES_PER_GROUP(sb) /
    //              EXT4_INODES_PER_BLOCK(sb)
    //      offset = ((ino - 1) % EXT4_INODES_PER_GROUP(sb)) %
    //               EXT4_INODES_PER_BLOCK(sb) * EXT4_INODE_SIZE(sb)
    err = ext4_get_inode_loc(sb, ino, &iloc);
    if (err)
        return ERR_PTR(err);

    // 2. 分配 VFS inode 结构
    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))
        return inode;  // 缓存命中，直接返回

    // 3. 读取磁盘 inode 数据
    raw_inode = ext4_raw_inode(&iloc);
    // 从 ext4_inode 填充 VFS inode:
    inode->i_mode = le16_to_cpu(raw_inode->i_mode);
    inode->i_nlink = le16_to_cpu(raw_inode->i_links_count);
    inode->i_size = (le32_to_cpu(raw_inode->i_size_lo) |
                     ((loff_t)le32_to_cpu(raw_inode->i_size_high) << 32));
    // ... 其他字段填充

    // 4. 设置文件操作表
    if (S_ISREG(inode->i_mode)) {
        inode->i_op = &ext4_file_inode_operations;
        inode->i_fop = &ext4_file_operations;  // 普通文件读写操作
    } else if (S_ISDIR(inode->i_mode)) {
        inode->i_op = &ext4_dir_inode_operations;
        inode->i_fop = &ext4_dir_operations;   // 目录操作
    }
    // ...

    unlock_new_inode(inode);
    return inode;
}
```

### 5.5 打开流程示意图

```
硬链接文件路径解析:  "/data/hardlink.txt"

  1. link_path_walk("/data/hardlink.txt")
     │
     ├─ 查找 "/"       → root dentry (dcache)
     ├─ 查找 "data"     → ext4_lookup(/, "data")
     │                    └─ ext4_iget(sb, data_inode_no)
     └─ 查找 "hardlink.txt" → ext4_lookup(data_dir, "hardlink.txt")
                              │
                              ├─ __ext4_find_entry(data_dir, "hardlink.txt")
                              │    └─ 在 data 目录的目录块中搜索:
                              │        ext4_dir_entry_2 {
                              │            .inode = 100,        ← 提取 inode 号
                              │            .name_len = 11,
                              │            .file_type = DT_REG,
                              │            .name = "hardlink.txt"
                              │        }
                              │
                              ├─ ino = le32_to_cpu(de->inode)  // ino = 100
                              │
                              └─ ext4_iget(sb, 100)            // 读取 inode #100
                                   │
                                   ├─ 读取磁盘 inode 表块
                                   │    └─ ext4_inode {
                                   │         .i_links_count = 2,  // 两个硬链接
                                   │         .i_mode = S_IFREG,
                                   │         .i_size = 4096,
                                   │         .i_blocks = 8,
                                   │         .i_block = [data_block_0, ...]
                                   │    }
                                   │
                                   ├─ 初始化 VFS struct inode
                                   │    ├─ inode->i_nlink = 2
                                   │    ├─ inode->i_size = 4096
                                   │    ├─ inode->i_fop = &ext4_file_operations
                                   │    └─ inode->i_mapping = address_space
                                   │
                                   └─ d_splice_alias(inode, dentry)  // dentry → inode
                                        │
                                        └─ 后续 open 操作:
                                             ├─ do_dentry_open()
                                             ├─ ext4_file_open()     // 文件打开
                                             └─ fd_install(fd, file) // 安装 fd

  2. 返回 fd，后续 read/write 通过 ext4_file_operations
     直接操作 inode #100 的数据块
```

---

## 6. 软链接（符号链接）创建过程与数据变化

### 6.1 linkat 与 symlinkat 的区别

`linkat` 创建硬链接，`symlinkat` 创建符号链接（软链接）。两者在 ext4 上的实现有本质区别：

| 比较维度 | 硬链接 (linkat) | 软链接 (symlinkat) |
|---------|---------------|------------------|
| 系统调用 | `linkat()` | `symlinkat()` |
| inode | 共享同一 inode | 创建**新 inode** |
| 数据块 | 共享同一组数据块 | 存储**目标路径字符串** |
| i_nlink | 源文件 `i_nlink++` | 新 inode `i_nlink=1` |
| 跨文件系统 | 不允许 | 允许 |
| 目录链接 | 不允许 | 允许 |
| 悬空链接 | 不可能 | 可能（目标被删除后） |
| 文件类型 | 继承源文件类型 | `S_IFLNK` |

### 6.2 ext4 软链接创建流程

```
symlinkat(target_path, newdirfd, linkname)
  │
  └─ do_symlinkat(dfd, name, &target_path)         // fs/namei.c
       └─ vfs_symlink(idmap, dir, dentry, symname)  // 通用 VFS 层
            └─ dir->i_op->symlink()  →  ext4_symlink()
                 │
                 └─ ext4_symlink(idmap, dir, dentry, symname)  // fs/ext4/namei.c:3470
                      │
                      ├─ fscrypt_prepare_symlink()     // 加密文件系统准备
                      │
                      ├─ ext4_new_inode_start_handle() // ★ 分配新 inode
                      │    └─ 分配 inode 号、位图操作、事务处理
                      │
                      ├─ 判断 [快/慢] 软链接:
                      │    │
                      │    ├─ [快软链接] 目标路径 ≤ 60 字节
                      │    │    inode->i_op = &ext4_fast_symlink_inode_operations
                      │    │    ext4_clear_inode_flag(inode, EXT4_INODE_EXTENTS)
                      │    │    memcpy(EXT4_I(inode)->i_data, symname, len)  // ★ 存入 inode 内
                      │    │    inode->i_size = len - 1
                      │    │    inode_set_cached_link(inode, i_data, i_size)  // 设置 i_link 缓存
                      │    │    └─ 磁盘变化: inode.i_block[0..14] 存储路径字符串，无需额外数据块
                      │    │
                      │    └─ [慢软链接] 目标路径 > 60 字节
                      │         inode->i_op = &ext4_symlink_inode_operations
                      │         ext4_init_symlink_block(handle, inode, &disk_link)
                      │              └─ ext4_bread(handle, inode, 0, CREATE)  // ★ 分配数据块
                      │              └─ memcpy(bh->b_data, symname, len)       // 路径写入数据块
                      │              └─ ext4_handle_dirty_metadata()           // 写回磁盘
                      │         └─ 磁盘变化: 分配一个数据块，inode.i_block[0] 指向该块
                      │
                      └─ ext4_add_nondir(handle, dentry, &inode)  // 添加目录项
                           └─ ext4_add_entry()  → 写入 ext4_dir_entry_2
                           └─ ext4_mark_inode_dirty()
                           └─ d_instantiate_new(dentry, inode)
```

### 6.3 关键代码分析

#### `ext4_symlink` — 完整实现 (fs/ext4/namei.c:3470)

```c
static int ext4_symlink(struct mnt_idmap *idmap, struct inode *dir,
                         struct dentry *dentry, const char *symname)
{
    handle_t *handle;
    struct inode *inode;
    int err, len = strlen(symname);
    int credits;
    struct fscrypt_str disk_link;
    int retries = 0;

    // 加密文件系统准备（可能加密 symlink 目标路径）
    err = fscrypt_prepare_symlink(dir, symname, len, dir->i_sb->s_blocksize,
                                  &disk_link);
    if (err)
        return err;

    // 配额初始化
    err = dquot_initialize(dir);
    if (err)
        return err;

    // 用 JBD2 事务创建新 inode
    credits = EXT4_DATA_TRANS_BLOCKS(dir->i_sb) +
              EXT4_INDEX_EXTRA_TRANS_BLOCKS + 3;
retry:
    inode = ext4_new_inode_start_handle(idmap, dir, S_IFLNK|S_IRWXUGO,
                                        &dentry->d_name, 0, NULL,
                                        EXT4_HT_DIR, credits);
    handle = ext4_journal_current_handle();
    if (IS_ERR(inode))
        goto out_retry;

    // 选择 inode 操作表：
    //   加密 → ext4_encrypted_symlink_inode_operations
    //   慢软链接 → ext4_symlink_inode_operations
    //   快软链接 → ext4_fast_symlink_inode_operations
    if (IS_ENCRYPTED(inode)) {
        inode->i_op = &ext4_encrypted_symlink_inode_operations;
    } else {
        if ((disk_link.len > EXT4_N_BLOCKS * 4)) {  // > 60 字节
            inode->i_op = &ext4_symlink_inode_operations;
        } else {                                     // ≤ 60 字节
            inode->i_op = &ext4_fast_symlink_inode_operations;
        }
    }

    // ★ 核心：存储目标路径
    if ((disk_link.len > EXT4_N_BLOCKS * 4)) {
        // [慢软链接] 路径 > 60 字节，需要分配数据块
        err = ext4_init_symlink_block(handle, inode, &disk_link);
    } else {
        // [快软链接] 路径 ≤ 60 字节，直接存入 inode.i_data
        ext4_clear_inode_flag(inode, EXT4_INODE_EXTENTS);
        memcpy((char *)&EXT4_I(inode)->i_data, disk_link.name,
               disk_link.len);
        inode->i_size = disk_link.len - 1;
        EXT4_I(inode)->i_disksize = inode->i_size;
        if (!IS_ENCRYPTED(inode))
            inode_set_cached_link(inode,
                (char *)&EXT4_I(inode)->i_data, inode->i_size);
    }

    // 添加目录项并写回
    err = ext4_add_nondir(handle, dentry, &inode);
    // ...
}
```

#### `ext4_init_symlink_block` — 慢软链接数据块分配 (fs/ext4/namei.c:3444)

```c
static int ext4_init_symlink_block(handle_t *handle, struct inode *inode,
                                   struct fscrypt_str *disk_link)
{
    struct buffer_head *bh;
    char *kaddr;
    int err = 0;

    // ★ 分配一个数据块 (ext4_bread + 参数 CREATE)
    bh = ext4_bread(handle, inode, 0, EXT4_GET_BLOCKS_CREATE);
    if (IS_ERR(bh))
        return PTR_ERR(bh);

    // 获取写权限（JBD2 事务保护）
    err = ext4_journal_get_write_access(handle, inode->i_sb, bh,
                                        EXT4_JTR_NONE);
    if (err)
        goto out;

    // ★ 将目标路径字符串写入数据块
    kaddr = (char *)bh->b_data;
    memcpy(kaddr, disk_link->name, disk_link->len);
    inode->i_size = disk_link->len - 1;
    EXT4_I(inode)->i_disksize = inode->i_size;

    // 标记数据块为脏，写回磁盘
    err = ext4_handle_dirty_metadata(handle, inode, bh);
out:
    brelse(bh);
    return err;
}
```

#### `ext4_get_link` — 打开软链接时读取目标路径 (fs/ext4/symlink.c:73)

```c
static const char *ext4_get_link(struct dentry *dentry, struct inode *inode,
                                 struct delayed_call *callback)
{
    struct buffer_head *bh;
    char *inline_link;

    // 处理 inline data 的情况（特殊场景遗留）
    if (ext4_has_inline_data(inode)) {
        inline_link = ext4_read_inline_link(inode);
        if (!IS_ERR(inline_link))
            set_delayed_call(callback, kfree_link, inline_link);
        return inline_link;
    }

    // 读 inode 第 0 块（即软链接数据块）
    if (!dentry) {
        // RCU 路径：尝试从缓存获取
        bh = ext4_getblk(NULL, inode, 0, EXT4_GET_BLOCKS_CACHED_NOWAIT);
        if (IS_ERR(bh) || !bh)
            return ERR_PTR(-ECHILD);
        if (!ext4_buffer_uptodate(bh)) {
            brelse(bh);
            return ERR_PTR(-ECHILD);
        }
    } else {
        // 正常路径：从磁盘读取
        bh = ext4_bread(NULL, inode, 0, 0);
        if (IS_ERR(bh))
            return ERR_CAST(bh);
        if (!bh) {
            EXT4_ERROR_INODE(inode, "bad symlink.");
            return ERR_PTR(-EFSCORRUPTED);
        }
    }

    // 返回数据块中的路径字符串
    set_delayed_call(callback, ext4_free_link, bh);
    nd_terminate_link(bh->b_data, inode->i_size,
                      inode->i_sb->s_blocksize - 1);
    return bh->b_data;  // ★ 返回目标路径，VFS 据此重新解析
}
```

### 6.4 磁盘数据变化对比

#### 快软链接（目标路径 ≤ 60 字节，如 `ln -s "file.txt" link`）

```
创建前:
  (无)

创建后:
  inode #200 (S_IFLNK, i_nlink=1, i_size=8)
    ├─ i_mode = S_IFLNK | 0777
    ├─ i_links_count = 1
    ├─ i_blocks = 0                    ← 无数据块分配
    └─ i_block[0..14] = "file.txt\0"   ← ★ 路径直接存在 inode 内
                                       ← 共 60 字节可用 (EXT4_N_BLOCKS * 4)

  父目录目录块:
    ├─ ext4_dir_entry_2: inode=200, name="link", file_type=EXT4_FT_SYMLINK
    └─ ...
```

#### 慢软链接（目标路径 > 60 字节，如 `ln -s "very/long/path/..." link`）

```
创建后:
  inode #200 (S_IFLNK, i_nlink=1, i_size=200)
    ├─ i_mode = S_IFLNK | 0777
    ├─ i_links_count = 1
    ├─ i_blocks = 8                    ← 分配了一个 4K 数据块
    └─ i_block[0] = block_500          ← 指向数据块 #500

  数据块 #500:
    └─ "very/long/path/..."            ← ★ 路径字符串存储在独立数据块中

  父目录目录块:
    ├─ ext4_dir_entry_2: inode=200, name="link", file_type=EXT4_FT_SYMLINK
    └─ ...
```

#### 与硬链接的磁盘数据对比

```
硬链接:                         软链接:
                               
inode #100 (i_nlink=2)         inode #200 (S_IFLNK, i_nlink=1)
  ├─ data block [0]              ├─ i_block[0..14] = "target"  (快软链接)
  ├─ data block [1]              └─ 或 data block = "target"  (慢软链接)
                               
目录项:                         目录项:
  original.txt → inode #100       target       → inode #100 (S_IFREG)
  hardlink.txt → inode #100       symlink      → inode #200 (S_IFLNK)
  ★ 两个目录项指向同一 inode      ★ 各自独立 inode，软链接 inode 存路径
```

### 6.5 打开软链接时的解析流程

```
open("/path/symlink", O_RDONLY)
  │
  └─ path_openat() → link_path_walk()
       │
       ├─ walk_component()  → 发现 S_IFLNK
       │
       └─ step_into() → pick_link()
            │
            ├─ inode->i_op->get_link()  →  ext4_get_link() / simple_get_link()
            │    │
            │    ├─ [快软链接] simple_get_link()
            │    │    └─ return inode->i_link  // 直接从内存返回路径
            │    │
            │    └─ [慢软链接] ext4_get_link()
            │         └─ ext4_bread(inode, 0)  // 读数据块
            │         └─ return bh->b_data      // 返回路径字符串
            │
            └─ VFS 获取到目标路径 "target"
                 └─ 重新调用 link_path_walk("target", nd)
                      └─ ... 最终找到目标文件
```

### 6.6 软链接 inode 操作表

```c
// fs/ext4/symlink.c

// 快软链接：路径存储在 inode->i_link 中
// simple_get_link 直接返回 inode->i_link
const struct inode_operations ext4_fast_symlink_inode_operations = {
    .get_link   = simple_get_link,     // 直接返回缓存路径
    .setattr    = ext4_setattr,
    .getattr    = ext4_getattr,
    .listxattr  = ext4_listxattr,
};

// 慢软链接：路径存储在数据块中
// ext4_get_link 从磁盘读取数据块返回路径
const struct inode_operations ext4_symlink_inode_operations = {
    .get_link   = ext4_get_link,       // 从磁盘读取数据块
    .setattr    = ext4_setattr,
    .getattr    = ext4_getattr,
    .listxattr  = ext4_listxattr,
};

// 加密软链接
const struct inode_operations ext4_encrypted_symlink_inode_operations = {
    .get_link   = ext4_encrypted_get_link,  // 解密后返回路径
    .setattr    = ext4_setattr,
    .getattr    = ext4_encrypted_symlink_getattr,
    .listxattr  = ext4_listxattr,
};
```

---

## 7. 关键数据结构 (C代码 + 注释)

```c
// ===== 磁盘 inode 结构 (ext4 磁盘上的 inode 格式) =====
// 硬链接的 inode 号就指向这个结构
struct ext4_inode {
    __le16  i_mode;              // 文件类型和权限 (S_IFREG, S_IFDIR 等)
    __le16  i_uid;               // 用户 ID 低 16 位
    __le32  i_size_lo;           // 文件大小低 32 位
    __le32  i_atime;             // 访问时间
    __le32  i_ctime;             // 状态变更时间 (linkat 会更新此字段)
    __le32  i_mtime;             // 修改时间
    __le32  i_dtime;             // 删除时间
    __le16  i_gid;               // 组 ID 低 16 位
    __le16  i_links_count;       // ★ 硬链接计数 (关键字段)
    __le32  i_blocks_lo;         // 文件占用的块数 (512 字节扇区)
    __le32  i_flags;             // 文件标志 (EXT4_*_FL)
    __le32  i_block[EXT4_N_BLOCKS]; // 指向数据块的指针 (extent 或间接块)
    __le32  i_generation;        // 文件版本号 (NFS 使用)
    __le32  i_file_acl_lo;       // 文件 ACL 块
    __le32  i_size_high;         // 文件大小高 32 位 (大文件)
    __le32  i_obso_faddr;        // 废弃的片段地址
    // ... 扩展属性区域 ...
    // 硬链接创建时: i_links_count++, i_ctime 更新
    // 数据块 (i_block) 不变，硬链接不复制数据

    // 示例: 两个硬链接指向同一 inode
    //   original.txt → inode #100, i_links_count = 2
    //   hardlink.txt → inode #100, i_links_count = 2 (同一个)
    //   i_blocks = 8, i_block = [block_0, block_1] (共享)
};

// ===== ext4 磁盘目录项结构 =====
// 存储在目录的数据块中，每个条目指向一个文件或子目录
// 硬链接创建时，在父目录中添加一个新条目指向同一个 inode
struct ext4_dir_entry_2 {
    __le32  inode;              // ★ inode 号 (硬链接指向的目标 inode)
    __le16  rec_len;            // 目录项记录长度 (包括自身和对齐填充)
    __u8    name_len;           // 文件名的实际长度
    __u8    file_type;          // 文件类型 (EXT4_FT_REG, EXT4_FT_DIR 等)
    char    name[EXT4_NAME_LEN]; // 文件名 (变长，最多 255 字节)
    // 示例:
    //   硬链接创建前: inode=100, name="original.txt"
    //   硬链接创建后: inode=100, name="hardlink.txt"  ← 新增的同 inode 目录项
    //                 inode=100, name="original.txt"  (不变)
    // 两者 inode 字段相同，指向同一 inode 和数据块
};

// ===== VFS inode 结构 (内存中的 inode) =====
// 当打开硬链接文件时，ext4_iget 填充此结构
struct inode {
    umode_t         i_mode;      // 文件类型和权限
    unsigned short  i_opflags;   // 操作标志
    kuid_t          i_uid;       // 用户 ID
    kgid_t          i_gid;       // 组 ID
    unsigned int    i_flags;     // 文件系统标志
    nlink_t         i_nlink;     // ★ 硬链接计数 (从 ext4_inode.i_links_count 读取)
    loff_t          i_size;      // 文件大小
    struct timespec64 i_atime;   // 访问时间
    struct timespec64 i_mtime;   // 修改时间
    struct timespec64 i_ctime;   // 状态变更时间
    const struct inode_operations   *i_op;  // inode 操作表
    const struct file_operations    *i_fop; // ★ 文件操作表
    struct address_space            *i_mapping;  // 地址空间 (页缓存)
    // 硬链接所有名称共享同一个 inode 对象
    // 无论通过哪个路径打开，都得到同一个 struct inode
};

// ===== VFS 目录项结构 (dentry) =====
// 缓存路径名到 inode 的映射
struct dentry {
    unsigned char d_name[];      // 文件名 (每个硬链接有不同名称)
    struct inode *d_inode;       // ★ 指向 inode (所有硬链接指向同一 inode)
    struct dentry *d_parent;     // 父目录 dentry
    struct list_head d_child;    // 兄弟目录项链表
    // 硬链接的 dentry 关系:
    //   dentry("original.txt") → d_inode = &inode_100
    //   dentry("hardlink.txt") → d_inode = &inode_100 (相同指针)
    //   inode_100.i_nlink = 2  ← 反映有两个 dentry 指向它
};

// ===== ext4 目录项查找上下文 =====
// ext4_lookup_entry 使用此结构在目录中搜索
struct ext4_filename {
    const struct qstr *usr_fname;  // 用户态文件名
    struct fscrypt_str disk_name;  // 磁盘上的加密文件名
    struct dx_hash_info hinfo;     // HTree 哈希信息
    int     crypto_buf;            // 加密缓冲区
};

// ===== ext4 块组描述符 =====
// 用于计算 inode 在磁盘上的位置
// ext4_get_inode_loc 通过 inode 号定位:
//   block_group = (ino - 1) / s_inodes_per_group
//   inode_table = ext4_inode_table(sb, gdp) + block_group
//   block = inode_table + ((ino-1) % s_inodes_per_group) / s_inodes_per_block
//   offset = ((ino-1) % s_inodes_per_block) * EXT4_INODE_SIZE(sb)
struct ext4_group_desc {
    __le32  bg_block_bitmap_lo;     // 块位图块号
    __le32  bg_inode_bitmap_lo;     // inode 位图块号
    __le32  bg_inode_table_lo;      // ★ inode 表起始块号
    __le16  bg_free_blocks_count_lo;// 空闲块计数
    __le16  bg_free_inodes_count_lo;// 空闲 inode 计数
    __le16  bg_used_dirs_count_lo;  // 目录数
    // ...
};
```

| 数据结构 | 头文件 | 在 linkat/symlinkat 中的作用 |
|----------|--------|------------------|
| `struct ext4_inode` | `fs/ext4/ext4.h` | 磁盘 inode，`i_links_count` 记录硬链接数，`i_data` 存储快软链接路径 |
| `struct ext4_dir_entry_2` | `fs/ext4/ext4.h` | 目录项，`inode` 字段指向文件 inode |
| `struct inode` | `include/linux/fs.h` | VFS 层 inode，硬链接共享同一对象，软链接新增独立对象 |
| `struct dentry` | `include/linux/dcache.h` | 目录项缓存，每个硬链接/软链接有独立 dentry |
| `struct ext4_filename` | `fs/ext4/ext4.h` | 查找文件名上下文 |
| `struct ext4_group_desc` | `fs/ext4/ext4.h` | 块组描述符，用于定位 inode 表位置 |

---

## 8. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int main(void)
{
    // 示例1: 基本硬链接创建
    if (linkat(AT_FDCWD, "/tmp/original.txt",
               AT_FDCWD, "/tmp/hardlink.txt", 0) == 0) {
        printf("成功创建硬链接\n");
        // 两个文件名指向同一个 inode，数据完全相同
        // 验证: stat 查看 inode 号相同
        struct stat st1, st2;
        stat("/tmp/original.txt", &st1);
        stat("/tmp/hardlink.txt", &st2);
        printf("inode: %lu == %lu, nlink: %lu\n",
               st1.st_ino, st2.st_ino, st1.st_nlink);
        // 输出: inode: 1234567 == 1234567, nlink: 2
    } else {
        perror("linkat");
    }

    // 示例2: 不跟随符号链接
    if (linkat(AT_FDCWD, "/tmp/symlink_target",
               AT_FDCWD, "/tmp/hardlink2.txt",
               AT_SYMLINK_NOFOLLOW) == 0) {
        // 如果 /tmp/symlink_target 是符号链接，则创建指向符号链接本身的硬链接
        // 但不推荐这样做，因为符号链接通常不应有硬链接
        printf("成功创建指向符号链接的硬链接\n");
    }

    // 示例3: 使用 AT_EMPTY_PATH 通过 fd 创建链接
    int fd = open("/tmp/original.txt", O_RDONLY);
    if (fd >= 0) {
        if (linkat(fd, "", AT_FDCWD, "/tmp/hardlink_by_fd.txt",
                   AT_EMPTY_PATH) == 0) {
            printf("通过文件描述符成功创建硬链接\n");
        }
        close(fd);
    }

    // 示例4: 打开硬链接文件——验证数据一致性
    {
        // 通过原始文件名写入数据
        int fd1 = open("/tmp/original.txt", O_WRONLY | O_CREAT, 0644);
        write(fd1, "Hello, Hard Link!", 17);
        close(fd1);

        // 通过硬链接读取数据——得到相同内容
        int fd2 = open("/tmp/hardlink.txt", O_RDONLY);
        char buf[32] = {0};
        read(fd2, buf, sizeof(buf) - 1);
        printf("通过硬链接读取: %s\n", buf);  // 输出: Hello, Hard Link!
        close(fd2);

        // 删除原始文件，硬链接仍然有效
        unlink("/tmp/original.txt");
        int fd3 = open("/tmp/hardlink.txt", O_RDONLY);
        if (fd3 >= 0) {
            printf("删除原始文件后，硬链接仍可访问\n");
            // i_nlink 从 2 变为 1，但数据块未被释放
            close(fd3);
        }
        // 最后删除硬链接，数据块才被释放
        unlink("/tmp/hardlink.txt");
    }

    exit(EXIT_SUCCESS);
}
```

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件元数据与属性)
- `fs/namei.c` — `vfs_link`、`filename_linkat` 实现
- `fs/ext4/namei.c` — `ext4_link`、`__ext4_link`、`ext4_inc_count`、`ext4_add_entry`、`ext4_insert_dentry`、`ext4_lookup`、`ext4_lookup_entry`、`__ext4_find_entry`
- `fs/ext4/inode.c` — `__ext4_iget` 从 inode 号读取 inode
- `fs/ext4/ext4.h` — `struct ext4_inode`、`struct ext4_dir_entry_2`、`struct ext4_group_desc`
- `include/linux/fs.h` — `struct inode`、`struct dentry`
- `fs/ext4/symlink.c` — `ext4_get_link`、`ext4_fast_symlink_inode_operations`、`ext4_symlink_inode_operations`
- `fs/ext4/namei.c` — `ext4_symlink`、`ext4_init_symlink_block`