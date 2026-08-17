# getdents64 系统调用完整路径分析

## 1 概述

`getdents64` 是 Linux 用于**读取目录条目**的系统调用，用户态库函数 `readdir` 和 `ls` 等命令最终都通过它获取目录中的文件列表。

### 原理分析

`getdents64` 采用**回调驱动的迭代器模式**：

```
用户态                    内核态
  readdir()  ──→  getdents64(fd, buf, count)
                     │
                     ├─ iterate_dir(file, &ctx)        // VFS 统一入口
                     │   ├─ down_read(&inode->i_rwsem) // 获取读锁
                     │   └─ f_op->iterate_shared()     // 文件系统实现
                     │       └─ 对每个目录项:
                     │           └─ dir_emit(name, ino, type)
                     │               └─ ctx->actor() → filldir64()
                     │                   └─ 写入用户缓冲区
                     │
                     └─ 返回实际填充的字节数
```

核心思想：**文件系统遍历目录结构，每找到一个条目就通过回调写入用户缓冲区**。这种方式避免了内核态分配大缓冲区，也无需在文件系统和用户态之间复制中间数据。

### 关键特点

- **目录迭代器模式**：通过 `struct dir_context` 的回调机制（`filldir64`），将目录项逐个拷贝到用户缓冲区
- **共享锁**：使用 `inode->i_rwsem` 的读锁（`down_read_killable`），支持并发读目录
- **cursor 游标**：利用 `file->f_pos` 作为读目录的游标位置，支持 `lseek` 定位
- **两种目录格式**：
  - `getdents`（旧）：返回 `struct linux_dirent`，`d_off` 字段类型为 `unsigned long`
  - `getdents64`（新）：返回 `struct linux_dirent64`，`d_off` 字段类型为 `s64`
- **文件系统无关**：VFS 层提供 `iterate_dir` 统一入口，各文件系统实现 `iterate_shared` 回调

### 使用场景

| 场景 | 说明 | 典型调用链 |
|------|------|-----------|
| **目录遍历** | 列出目录中所有文件和子目录 | `ls -la` → `opendir` → `readdir` → `getdents64` |
| **文件查找** | 在目录中搜索特定文件 | `find /path -name "*.c"` → `fdopendir` → `getdents64` |
| **文件监控** | 检测目录内容变化 | `inotify` 内部注册目录变更事件 |
| **路径解析** | 路径名查找过程中的目录项遍历 | `path_openat` → `link_path_walk` → 逐级查找目录项 |
| **备份与同步** | 递归遍历文件系统树 | `rsync`、`tar` 等工具递归读取目录 |

### 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | SYSCALL_DEFINE3(getdents64) (fs/readdir.c:397) |
| **VFS** | iterate_dir → f_op->iterate_shared (fs/readdir.c:83) |
| **Filesystem** | ext4_readdir / proc_readdir 等 (fs/ext4/dir.c, fs/proc/generic.c) |
| **filldir 回调** | filldir64 将目录项拷贝到用户空间 (fs/readdir.c:336) |

---

## 2 系统调用入口

### 2.1 SYSCALL_DEFINE3(getdents64) - fs/readdir.c:397

```c
SYSCALL_DEFINE3(getdents64, unsigned int, fd,
        struct linux_dirent64 __user *, dirent, unsigned int, count)
{
    CLASS(fd_pos, f)(fd);                           // 通过 fd 查找 struct fd 并获取文件位置
    struct getdents_callback64 buf = {
        .ctx.actor = filldir64,                      // 设置回调函数（填充目录项到用户空间）
        .ctx.count = count,                          // 用户缓冲区剩余容量
        .ctx.dt_flags_mask = FILLDIR_FLAG_NOINTR,    // 允许信号中断
        .current_dir = dirent                        // 当前写入位置（用户缓冲区指针）
    };
    int error;

    if (fd_empty(f))
        return -EBADF;

    error = iterate_dir(fd_file(f), &buf.ctx);       // 核心调用：遍历目录
    if (error >= 0)
        error = buf.error;                           // 优先使用 filldir 设置的具体错误
    if (buf.prev_reclen) {
        // 更新上一个目录项的 d_off 字段（指向当前条目，即下一个读取位置）
        struct linux_dirent64 __user *lastdirent;
        typeof(lastdirent->d_off) d_off = buf.ctx.pos;

        lastdirent = (void __user *)buf.current_dir - buf.prev_reclen;
        if (put_user(d_off, &lastdirent->d_off))
            error = -EFAULT;
        else
            // 返回实际填充的字节数 = 用户缓冲区大小 - 剩余空间
            error = count - buf.ctx.count;
    }
    return error;
}
```

### 2.2 用户态原型

```c
// 用户态不可直接调用，通过 libc 封装：
//    ssize_t getdents64(int fd, void *dirp, size_t count);
// 或通过 syscall() 调用：
//    syscall(SYS_getdents64, fd, dirp, count);
//
// 更常见的用法是 libc 的 readdir() 系列函数：
//    DIR *dir = opendir("/path");
//    struct dirent *entry;
//    while ((entry = readdir(dir)) != NULL) {
//        printf("name=%s, ino=%lu\n", entry->d_name, entry->d_ino);
//    }
//    closedir(dir);
```

---

## 3 核心路径：iterate_dir → filesystem → filldir64

```
/* ========== getdents64 核心路径 ========== */
/* 基于回调模式：VFS 遍历目录 → 文件系统提供条目 → filldir 写入用户缓冲区 */

SYSCALL_DEFINE3(getdents64, fd, dirent, count)                     // fs/readdir.c:397
  │
  │  # 初始化回调上下文
  │  # struct getdents_callback64 {
  │  #     .ctx.actor     = filldir64         回调函数
  │  #     .ctx.count     = count             用户缓冲区容量
  │  #     .current_dir   = dirent            当前写入位置
  │  #     .prev_reclen   = 0                 上一个条目的长度
  │  # }
  │
  └─ iterate_dir(fd_file(f), &buf.ctx)                              // fs/readdir.c:83
       │
       ├─ [文件系统不支持 iterate_shared] → return -ENOTDIR
       │
       ├─ security_file_permission(file, MAY_READ)                  // 安全权限检查
       │
       ├─ down_read_killable(&inode->i_rwsem)                       // 获取 inode 读锁
       │  # 多个进程可以同时读同一个目录
       │  # 写操作（如创建/删除文件）需要写锁，与读锁互斥
       │
       ├─ [IS_DEADDIR(inode)] → return -ENOENT                      // 目录已删除
       │
       ├─ ctx->pos = file->f_pos                                    // 从文件位置开始
       │
       ├─ file->f_op->iterate_shared(file, ctx)                     // 调用文件系统回调
       │  │  # 如 ext4_readdir / proc_readdir / kernfs_fop_readdir 等
       │  │
       │  └─ [文件系统实现] 遍历目录块:
       │      │  ext4: 读取磁盘上的目录块, 解析 ext4_dir_entry_2
       │      │  proc: 遍历内核 proc_dir_entry 链表
       │      │  kernfs: 遍历 kernfs_node 红黑树
       │      │
       │      └─ [对每个条目] 调用 dir_emit(ctx, name, namlen, ino, type)
       │           │  # include/linux/fs.h:3571
       │           │
       │           └─ ctx->actor(ctx, name, namlen, ctx->pos, ino, type)
       │                │  # → filldir64()
       │                │
       │                └─ filldir64(ctx, name, namlen, offset, ino, d_type)
       │                    │  # fs/readdir.c:336
       │                    │  # 将目录项写入用户缓冲区
       │                    │
       │                    ├─ 计算 reclen = ALIGN(offsetof(dirent64.d_name) + namlen + 1, 8)
       │                    │  # 每条目录项记录对齐到 8 字节
       │                    │
       │                    ├─ [reclen > ctx->count] → return false    // 缓冲区满
       │                    │
       │                    ├─ [信号 pending] → return false           // 可中断
       │                    │
       │                    ├─ 写入用户空间:
       │                    │  ├─ unsafe_put_user(offset, &prev->d_off)  // 更新上个条目的 d_off
       │                    │  ├─ unsafe_put_user(ino, &dirent->d_ino)   // 写入 inode 号
       │                    │  ├─ unsafe_put_user(reclen, &dirent->d_reclen)  // 写入记录长度
       │                    │  ├─ unsafe_put_user(d_type, &dirent->d_type)    // 写入文件类型
       │                    │  └─ unsafe_copy_dirent_name(dirent->d_name, name, namlen)  // 文件名
       │                    │
       │                    ├─ buf->prev_reclen = reclen                // 记录当前条目长度
       │                    ├─ buf->current_dir += reclen               // 前进到下一个写入位置
       │                    └─ ctx->count -= reclen                     // 减少剩余容量
       │
       ├─ file->f_pos = ctx->pos                                     // 更新文件位置
       │
       ├─ fsnotify_access(file)                                      // 通知文件访问
       │
       └─ inode_unlock_shared(inode)                                 // 释放 inode 读锁
```

---

## 4 函数调用栈

```
/* ========== getdents64 完整函数调用栈 ========== */

SYSCALL_DEFINE3(getdents64, fd, dirent, count)                      // fs/readdir.c:397
└─ iterate_dir(fd_file(f), &buf.ctx)                                 // fs/readdir.c:83
   ├─ security_file_permission(file, MAY_READ)                       // 安全权限检查
   ├─ down_read_killable(&inode->i_rwsem)                            // 获取 inode 读锁
   │
   └─ file->f_op->iterate_shared(file, ctx)                         // 文件系统目录遍历
      │
      ├─ [ext4 文件系统] → ext4_readdir(file, ctx)                  // fs/ext4/dir.c:129
      │  ├─ [HTree 索引目录] → ext4_dx_readdir(file, ctx)           // 哈希树加速
      │  │  ├─ ext4_htree_fill_tree(...)                             // 填充 hash 树
      │  │  └─ 对每个条目 → call_filldir(file, ctx, fname)           // 调用 dir_emit
      │  │
      │  └─ [普通目录] → 循环读取目录块:
      │     ├─ ext4_map_blocks(NULL, inode, &map, 0)                 // 映射目录块
      │     ├─ ext4_bread(NULL, inode, map.m_lblk, 0)                // 读取目录块
      │     └─ 对每个 ext4_dir_entry_2:
      │        └─ dir_emit(ctx, de->name, de->name_len,            // 发出目录项
      │                     le32_to_cpu(de->inode),
      │                     get_dtype(sb, de->file_type))
      │
      ├─ [procfs] → proc_readdir(file, ctx)                          // fs/proc/generic.c:327
      │  └─ proc_readdir_de(file, ctx, PDE(inode))                   // 遍历 proc_dir_entry
      │
      └─ [kernfs] → kernfs_fop_readdir(file, ctx)                   // fs/kernfs/dir.c
         └─ 遍历 kernfs_node 红黑树                                   // 每个节点发出一个条目
            └─ dir_emit(ctx, name, len, ino, type)
               │
               └─ [所有文件系统最终调用 dir_emit → ctx->actor]
                  │
                  └─ filldir64(ctx, name, namlen, offset, ino, d_type)  // fs/readdir.c:336
                     ├─ 计算 reclen (对齐到 8 字节)
                     ├─ 检查缓冲区剩余容量
                     ├─ unsafe_put_user: 写入 d_ino, d_reclen, d_type
                     └─ unsafe_copy_dirent_name: 写入 d_name

/* ========== 收尾（返回用户态后） ========== */

  [iterate_dir 返回后]
  ├─ file->f_pos = ctx->pos                                         // 更新文件位置
  ├─ inode_unlock_shared(inode)                                      // 释放 inode 读锁
  │
  └─ [SYSCALL_DEFINE3 收尾]
     └─ 更新上一个目录项的 d_off 字段（指向下一个条目位置）
        put_user(buf.ctx.pos, &lastdirent->d_off)
        return count - buf.ctx.count                                 // 返回填充字节数
```

---

## 5 关键数据结构 (C代码 + 注释)

```c
// ===== 用户态目录项结构 (getdents64 返回格式) =====
// 每个目录项以变长结构存储在用户缓冲区中
struct linux_dirent64 {
    u64      d_ino;          // inode 号（64 位，无溢出风险）
    s64      d_off;          // 下一个目录项的偏移（用于 lseek 定位）
    unsigned short d_reclen; // 本记录长度（包括 d_name 实际长度 + 对齐填充）
    unsigned char  d_type;   // 文件类型（DT_REG, DT_DIR, DT_LNK 等）
    char     d_name[];       // 文件名（变长，以 '\0' 结尾，尾部对齐填充）
    // 布局示例:
    // [d_ino:8][d_off:8][d_reclen:2][d_type:1][d_name:...][padding:...]
    // 总长度通过 ALIGN(offsetof(d_name) + namlen + 1, 8) 计算
};

// ===== EXT4 磁盘目录项结构 =====
// ext4 文件系统磁盘上的目录项格式
struct ext4_dir_entry_2 {
    __le32  inode;           // inode 号（小端，磁盘格式）
    __le16  rec_len;         // 目录项长度（磁盘上记录长度）
    __u8    name_len;        // 文件名长度
    __u8    file_type;       // 文件类型（EXT4_FT_* 常量）
    char    name[];          // 文件名（变长，不以 '\0' 结尾）
    // ext4_readdir 从磁盘读取 ext4_dir_entry_2 后,
    // 通过 dir_emit() 转换为 linux_dirent64 返回给用户
};

// ===== VFS 目录上下文 =====
// 核心回调机制：文件系统遍历目录时，通过 actor 回调将条目逐个写入用户缓冲区
struct dir_context {
    filldir_t actor;        // 回调函数指针 → filldir64
    loff_t pos;             // 当前目录偏移（从 file->f_pos 初始化，遍历时递增）
    int count;              // 用户缓冲区剩余容量（字节数，递减）
    unsigned int dt_flags_mask;  // d_type 额外标志掩码（FILLDIR_FLAG_NOINTR 等）
    // 流程:
    //   1. iterate_dir 设置 ctx->pos = file->f_pos
    //   2. 文件系统对每个条目调用 dir_emit(ctx, name, len, ino, type)
    //   3. dir_emit 调用 ctx->actor → filldir64
    //   4. filldir64 将条目拷贝到用户缓冲区，递减 ctx->count
    //   5. 当 ctx->count 不足时返回 false 停止遍历
};

// ===== getdents64 回调上下文 =====
// 包装 dir_context，添加 filldir64 所需的额外状态
struct getdents_callback64 {
    struct dir_context ctx;             // 嵌入的 VFS 上下文（actor/count/pos）
    struct linux_dirent64 __user *current_dir;  // 当前写入位置（用户缓冲区指针）
    int prev_reclen;                     // 上一个条目的 reclen（用于更新 d_off）
    int error;                           // filldir 过程中设置的错误码
    // filldir64 内部状态:
    //   current_dir: 开始时指向用户缓冲区起始，每次写入后递增 reclen
    //   prev_reclen: 延迟写入 d_off——写入当前条目时更新上一个条目的 d_off
    //   error: 在 verify_dirent_name 或 EFAULT 时设置
};

// ===== 文件操作函数表（目录相关） =====
// 目录对应的 file_operations 需要实现 iterate_shared
struct file_operations {
    // ...
    int (*iterate_shared)(struct file *, struct dir_context *);
    // 文件系统通过此回调遍历目录内容
    // ext4: ext4_readdir, proc: proc_readdir, kernfs: kernfs_fop_readdir
    // ...
};

// ===== 目录项发出辅助函数 =====
// 文件系统通过 dir_emit 将目录项交给 VFS 层
// dir_emit_dots 自动处理 "." 和 ".." 两个特殊条目
static inline bool dir_emit(struct dir_context *ctx,
                    const char *name, int namelen,
                    u64 ino, unsigned type)
{
    unsigned int dt_mask = S_DT_MASK | ctx->dt_flags_mask;
    // 调用 ctx->actor → filldir64
    return ctx->actor(ctx, name, namelen, ctx->pos, ino, type & dt_mask);
}

static inline bool dir_emit_dots(struct file *file, struct dir_context *ctx)
{
    // 如果 ctx->pos == 0，发出 "." 条目，设置 ctx->pos = 1
    // 如果 ctx->pos == 1，发出 ".." 条目，设置 ctx->pos = 2
    // 通常文件系统在遍历前先调用 dir_emit_dots
}
```

### d_type 文件类型枚举值

`d_type` 字段定义在 `include/uapi/linux/dirent.h`，用于标识目录项的文件类型，避免对每个文件额外发起 `stat` 调用：

| 常量 | 值 | 说明 | 对应 stat 宏 |
|------|----|------|------------|
| `DT_UNKNOWN` | 0 | 未知类型（需要额外 stat 确认） | — |
| `DT_DIR` | 4 | 目录 | `S_ISDIR(m)` |
| `DT_REG` | 8 | 普通文件 | `S_ISREG(m)` |
| `DT_LNK` | 10 | 符号链接 | `S_ISLNK(m)` |
| `DT_BLK` | 6 | 块设备 | `S_ISBLK(m)` |
| `DT_CHR` | 2 | 字符设备 | `S_ISCHR(m)` |
| `DT_FIFO` | 1 | 命名管道 | `S_ISFIFO(m)` |
| `DT_SOCK` | 12 | Unix 域套接字 | `S_ISSOCK(m)` |

> 注意：某些文件系统（如 FUSE）可能返回 `DT_UNKNOWN`，此时用户态可调用 `stat` 获取确切类型。

### 关键数据结构对照表

| 数据结构 | 头文件 | 在 getdents64 中的作用 |
|----------|--------|----------------------|
| `struct linux_dirent64` | `include/linux/dirent.h` | 返回给用户态的目录项格式 |
| `struct ext4_dir_entry_2` | `fs/ext4/ext4.h` | EXT4 磁盘目录项格式 |
| `struct dir_context` | `include/linux/fs.h` | VFS 目录上下文，actor 回调机制 |
| `struct getdents_callback64` | `fs/readdir.c` | getdents64 回调上下文 |
| `struct file_operations` | `include/linux/fs.h` | 文件操作表，含 iterate_shared |

---

## 6 关于 d_off 与 telldir/seekdir

`d_off` 字段是 `getdents64` 返回的**关键游标字段**，用于支持目录流的随机访问：

### d_off 的语义

```
条目 0 (.)         d_off = 24        ← 指向条目 1 的偏移
条目 1 (..)        d_off = 48        ← 指向条目 2 的偏移
条目 2 (file1)     d_off = 80        ← 指向条目 3 的偏移
条目 3 (file2)     d_off = 112       ← 指向下一个未知条目的偏移
                     ↑
          每个条目写入时，d_off 被设置为
          buf.ctx.pos（即当前 file->f_pos 的快照）
```

### 工作机制

- **`telldir()`**：返回当前目录流位置的 `d_off` 值
- **`seekdir(loc)`**：通过 `lseek(fd, loc, SEEK_SET)` 设置 `file->f_pos`，下次 `getdents64` 从该位置继续读取
- **实现细节**：`getdents64` 在返回前会**延迟更新上一个条目的 `d_off`**（`put_user(buf.ctx.pos, &lastdirent->d_off)`），确保每个条目的 `d_off` 指向下一个条目的起始位置

### 注意事项

- `d_off` 对用户态是不透明的 cookie，**不应直接解释为字节偏移**（某些文件系统如 ext4 使用 HTree hash 作为偏移）
- 唯一保证是：**传入 `seekdir` 的 `d_off` 值必须来自之前 `telldir` 或 `getdents64` 返回的值**
- 目录中添加或删除文件后，先前保存的 `d_off` 值可能失效（指向已删除的条目或错误的条目）

---

## 7 ext4 目录结构与 HTree 索引

### 7.1 ext4 目录磁盘布局

```
ext4 目录文件 = 一组目录块 (block size = 4KB)

传统模式 (dir_index=0):
  [Block 0] [Block 1] [Block 2] ...
    │         │
    └─ 线性链表，每个块内存放多个 ext4_dir_entry_2
       大目录遍历速度慢 (O(n))

HTree 模式 (dir_index=1, 默认开启):
  [dx_root/inline]  ─→  [dx_node]  ─→  [dx_node]  ─→  [leaf block 0]
                           │                                       │
                           └→  [leaf block 1]  ←──────────────────┘
                           │
                           └→  [leaf block 2]
                                ...
  查找 O(log n)，大幅加速大目录遍历
```

### 7.2 HTree 索引工作原理

`ext4` 默认使用 HTree（哈希树）索引加速大目录操作：

```c
// ext4 目录分为两种类型：
// 1. 传统线性目录（小目录，直接遍历）
// 2. HTree 索引目录（大目录，哈希树加速）

// HTree 目录结构:
//   dx_root (Block 0)
//     ├─ 头部: fake_dirent + dx_root_info
//     ├─ 条目: [".", ".."] 两个特殊条目
//     └─ dx_entry[]: 指向子节点的哈希索引
//          │
//          ├─ dx_node (内部节点)
//          │   └─ dx_entry[]: 指向更细粒度的子节点
//          │
//          └─ leaf_block (叶子节点，实际存放目录项)
//              └─ ext4_dir_entry_2[]: 线性链表

// 查找流程:
//   1. 对文件名计算 hash (half_md4)
//   2. 在 dx_root 的 dx_entry 数组中二分查找
//   3. 递归下降到对应子节点（dx_node 或 leaf_block）
//   4. 在 leaf_block 中线性查找匹配文件名
//   5. 遍历时将 hash 值编码到 ctx->pos 中
//      → pos 高 32 位 = hash 值，低 32 位 = 块内偏移

// ext4_readdir 的两种路径:
//   ext4_readdir(file, ctx)
//     ├─ 检查是否 HTree 目录:
//     │   ├─ 是 → ext4_dx_readdir(file, ctx)
//     │   │      ├─ 通过 HTree 索引定位到 leaf block
//     │   │      └─ 遍历 leaf block 中的条目
//     │   │
//     │   └─ 否 → 传统线性遍历:
//     │          ├─ ext4_bread() 顺序读取目录块
//     │          └─ 解析 ext4_dir_entry_2 链
//     │
//     └─ 对每个条目调用 dir_emit()
```

### 7.3 HTree 的 d_off 编码

HTree 目录中 `ctx->pos` 的编码方式（`fs/ext4/dx_root.info`）：

```
sizeof(loff_t) = 64 bits:
  ┌───────────────┬───────────────────┐
  │  high 32 bits  │   low 32 bits     │
  │  hash 值       │  块内偏移         │
  │  (half_md4)    │  (byte offset)    │
  └───────────────┴───────────────────┘

示例:
  pos = 0x00000001_00000048
        ↑ hash=1   ↑ 偏移=72 字节
```

这种编码使得 `telldir`/`seekdir` 能够通过 `lseek` 精确定位到 HTree 的特定位置，而不需要重新计算 hash。

---

## 8 完整流程总结

```
用户态:
  fd = open("/path", O_RDONLY | O_DIRECTORY)     // 打开目录
  buf = malloc(4096)                              // 分配缓冲区
  nread = getdents64(fd, buf, 4096)               // 读取目录项

内核态:
  getdents64(fd, buf, 4096)
    └─ iterate_dir(file, &ctx)                     // VFS 层
         ├─ 获取 inode 读锁
         └─ ext4_readdir(file, &ctx)               // EXT4 文件系统
              └─ 循环读取目录块:
                   ├─ 读取块 0: [.][..][file1][file2]
                   │   ├─ dir_emit(".", ino=A, DT_DIR)
                   │   │   └─ filldir64 → buf[0]: d_ino=A, d_name=".", d_reclen=24
                   │   ├─ dir_emit("..", ino=B, DT_DIR)
                   │   │   └─ filldir64 → buf[24]: d_ino=B, d_name="..", d_reclen=24
                   │   ├─ dir_emit("file1", ino=C, DT_REG)
                   │   │   └─ filldir64 → buf[48]: d_ino=C, d_name="file1", d_reclen=32
                   │   └─ dir_emit("file2", ino=D, DT_REG)
                   │       └─ filldir64 → buf[80]: d_ino=D, d_name="file2", d_reclen=32
                   │           # 缓冲区已满? → 返回 false，停止遍历
                   │
                   └─ [返回后]
                        ctx->count = 4096 - 112 = 3984
                        file->f_pos = ctx->pos = 112
                        buf[48].d_off = 80 (下一个条目偏移)
                        buf[80].d_off = 112 (下一个条目偏移)
                        return 4096 - 3984 = 112 字节

用户态:
  // buf 中数据:
  // [d_ino=A][d_off=24][reclen=24][type=DT_DIR][name="."]
  // [d_ino=B][d_off=48][reclen=24][type=DT_DIR][name=".."]
  // [d_ino=C][d_off=80][reclen=32][type=DT_REG][name="file1"]
  // [d_ino=D][d_off=112][reclen=32][type=DT_REG][name="file2"]
  //
  // 重复调用 getdents64 读取剩余条目
  // 当 getdents64 返回 0 时表示目录结束
```

---

## 9 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/dirent.h>

// 直接调用 getdents64 系统调用（不使用 libc 封装）
// 等同于 libc 的 readdir() 系列函数

int main(int argc, char *argv[])
{
    const char *dir_path = argc > 1 ? argv[1] : ".";
    int fd = open(dir_path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    char buf[4096];
    ssize_t nread;
    struct linux_dirent64 *entry;

    // 循环读取目录条目
    while ((nread = syscall(SYS_getdents64, fd, buf, sizeof(buf))) > 0) {
        long offset = 0;
        while (offset < nread) {
            entry = (struct linux_dirent64 *)(buf + offset);
            printf("ino=%lu  type=%d  name=%s\n",
                   entry->d_ino,
                   entry->d_type,
                   entry->d_name);
            offset += entry->d_reclen;
        }
    }

    if (nread < 0)
        perror("getdents64");

    close(fd);
    return 0;
}
```

---

## 10 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#文件元数据与属性)
- `fs/readdir.c` — getdents64 系统调用核心实现
- `fs/ext4/dir.c` — EXT4 目录遍历实现（含 HTree 索引）
- `fs/ext4/namei.c` — EXT4 目录项查找（HTree 查找）
- `include/linux/fs.h` — `struct dir_context` 和 `dir_emit` 辅助函数
- `include/linux/dirent.h` — `struct linux_dirent64` 定义
- `include/uapi/linux/dirent.h` — `d_type` 枚举常量定义
- `fs/ext4/ext4.h` — `struct ext4_dir_entry_2` 和 `struct dx_root_info` 定义