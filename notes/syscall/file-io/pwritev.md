# pwritev 系统调用完整路径分析

## 1 概述

pwritev 系统调用将 **定位写（positioned I/O）** 和 **分散/聚集 I/O（scatter-gather I/O）** 结合为一体。它在指定文件偏移量处，使用多个 `iovec` 缓冲区执行写操作，且**不改变**文件当前的 `f_pos`。

### 关键特点

- **定位语义**：使用调用者提供的 `pos` 参数（栈局部变量），不更新 `file->f_pos`
- **分散/聚集**：通过 `import_iovec` 从用户空间导入多个 `iovec` 段，支持 `UIO_FASTIOV` 栈优化
- **权限检查**：pwritev 需要 `FMODE_PWRITE`
- **ARM64 参数编码**：`loff_t pos` 由 `pos_h`(高32位) 和 `pos_l`(低32位) 拼装而成
- **pwritev2 扩展**：支持 `RWF_*` 标志（如 `RWF_NOWAIT`, `RWF_DSYNC`, `RWF_APPEND`）
- **下游路径**：pwritev 与 write 共享完全相同的 ext4→block→NVMe 路径

---

## 2 涉及的内核层

| 层 | 说明 |
|--|--|
| **Syscall Entry** | pwritev/pwritev2 系统调用入口 (fs/read_write.c) |
| **VFS** | vfs_writev → do_iter_readv_writev (fs/read_write.c) |
| **ext4** | ext4_file_write_iter → ext4_buffered_write_iter (fs/ext4/file.c) |
| **Page Cache** | generic_perform_write → 循环写入 folio (mm/filemap.c) |
| **ext4 写路径** | ext4_da_write_begin / ext4_da_write_end (fs/ext4/inode.c) |
| **Block Layer** | blk-mq 提交 (block/blk-core.c, blk-mq.c) |
| **NVMe 驱动** | 写命令提交 + 中断完成 (drivers/nvme/host/pci.c) |

---

## 3 系统调用入口

### 3.1 SYSCALL_DEFINE5(pwritev) - fs/read_write.c:1475

```c
SYSCALL_DEFINE5(pwritev, unsigned long, fd, const struct iovec __user *, vec,
        unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
    loff_t pos = pos_from_hilo(pos_h, pos_l);
    return do_pwritev(fd, vec, vlen, pos, 0);
}

SYSCALL_DEFINE6(pwritev2, unsigned long, fd, const struct iovec __user *, vec,
        unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h,
        rwf_t, flags)
{
    loff_t pos = pos_from_hilo(pos_h, pos_l);
    if (pos == -1)
        return do_writev(fd, vec, vlen, flags);   // pos=-1 → 用 f_pos

    return do_pwritev(fd, vec, vlen, pos, flags);
}
```

### 3.2 ARM64 特殊的参数编码

ARM64 系统调用号码使用 `x8` 寄存器传递，参数使用 `x0-x5` 寄存器。由于 ARM64 寄存器宽度为 64 位，一个 `loff_t`（64位）可以用一个寄存器，而 `pwritev` 的签名在用户态是：

```c
ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);
```

但在内核中 ARM64 的 `SYSCALL_DEFINE5` 无法直接传递 64 位参数，因此拆分为 `pos_l`（低32位）和 `pos_h`（高32位）：

```c
// arch/arm64/include/asm/syscall_wrapper.h
#define pos_from_hilo(h, l) (((loff_t)(h) << 32) | (loff_t)(l))
```

在 syscall_64.tbl 中定义为：
```
70  common  pwritev     sys_pwritev
```

### 3.3 do_pwritev - fs/read_write.c:1422

```c
static ssize_t do_pwritev(unsigned long fd, const struct iovec __user *vec,
             unsigned long vlen, loff_t pos, rwf_t flags)
{
    ssize_t ret = -EBADF;

    if (pos < 0)
        return -EINVAL;

    CLASS(fd, f)(fd);
    if (!fd_empty(f)) {
        ret = -ESPIPE;
        if (fd_file(f)->f_mode & FMODE_PWRITE)
            ret = vfs_writev(fd_file(f), vec, vlen, &pos, flags);
    }

    if (ret > 0)
        add_wchar(current, ret);
    inc_syscw(current);
    return ret;
}
```

---

## 4 VFS 分散/聚集 I/O 层

### 4.1 import_iovec - iov_iter 导入机制

pwritev 与 writev 共享同一个关键机制：从用户空间导入多个 iovec 段。

```c
// lib/iov_iter.c
ssize_t import_iovec(int type, const struct iovec __user *uvec,
             unsigned nr_segs, unsigned fast_segs,
             struct iovec **iovp, struct iov_iter *iter)
{
    struct iovec *iov = *iovp;
    ssize_t ret;

    ret = __import_iovec(type, uvec, nr_segs, fast_segs, iovp, iter);
    // 1. 如果 nr_segs <= UIO_FASTIOV（通常为8），使用栈上数组 iovstack
    //    避免 kmalloc 分配
    // 2. 拷贝用户空间 iovec 数组到内核
    // 3. 初始化 iov_iter 结构体
    return ret;
}
```

### 4.2 vfs_writev - fs/read_write.c:1225

```c
static ssize_t vfs_writev(struct file *file, const struct iovec __user *vec,
              unsigned long vlen, loff_t *pos, rwf_t flags)
{
    struct iovec iovstack[UIO_FASTIOV];
    struct iovec *iov = iovstack;
    struct iov_iter iter;
    size_t tot_len;
    ssize_t ret = 0;

    if (!(file->f_mode & FMODE_WRITE))
        return -EBADF;
    if (!(file->f_mode & FMODE_CAN_WRITE))
        return -EINVAL;

    ret = import_iovec(ITER_SOURCE, vec, vlen, ARRAY_SIZE(iovstack), &iov, &iter);
    if (ret < 0)
        return ret;

    tot_len = iov_iter_count(&iter);
    if (!tot_len)
        goto out;

    ret = rw_verify_area(WRITE, file, pos, tot_len);
    if (ret < 0)
        goto out;

    file_start_write(file);
    if (file->f_op->write_iter)
        ret = do_iter_readv_writev(file, &iter, pos, WRITE, flags);
    else
        ret = do_loop_readv_writev(file, &iter, pos, WRITE, flags);
    file_end_write(file);
out:
    if (ret >= 0)
        fsnotify_modify(file);
    kfree(iov);
    return ret;
}
```

### 4.3 do_iter_readv_writev - 核心分发函数

```c
static ssize_t do_iter_readv_writev(struct file *filp, struct iov_iter *iter,
                    loff_t *ppos, int type, rwf_t flags)
{
    struct kiocb kiocb;
    ssize_t ret;

    init_sync_kiocb(&kiocb, filp);
    ret = kiocb_set_rw_flags(&kiocb, flags, type);
    if (ret)
        return ret;
    kiocb.ki_pos = (ppos ? *ppos : 0);

    // pwritev: type=WRITE → f_op->write_iter → ext4_file_write_iter
    if (type == WRITE)
        ret = filp->f_op->write_iter(&kiocb, iter);

    BUG_ON(ret == -EIOCBQUEUED);
    if (ppos)
        *ppos = kiocb.ki_pos;
    return ret;
}
```

关键点：
- pwritev 使 `type=WRITE` → `f_op->write_iter` → `ext4_file_write_iter`
- 从 `do_iter_readv_writev` 开始，pwritev 与 write 共享完全相同的下游路径
- 区别仅在于：`vfs_writev` 使用 `import_iovec(ITER_SOURCE, ...)` 导入多段 iovec，`vfs_write` 使用 `iov_iter_init` 初始化单段

---

## 5 下游路径（pwritev = write）

```
do_iter_readv_writev(filp, iter, &pos, WRITE, flags)
  └─ filp->f_op->write_iter(&kiocb, iter)
       └─ ext4_file_write_iter(iocb, iter)         // fs/ext4/file.c:278
            ├─ IS_DAX → ext4_dax_write_iter
            ├─ IOCB_DIRECT → ext4_dio_write_iter
            └─ ext4_buffered_write_iter(iocb, from)  // fs/ext4/file.c:300
                 └─ generic_perform_write(iocb, from)  // mm/filemap.c:3390
                      └─ [循环写入 folio]
                           ├─ a_ops->write_begin → ext4_da_write_begin  // fs/ext4/inode.c:3100
                           │    └─ 延迟块分配（ext4_da_reserve_space）
                           │
                           ├─ copy_folio_from_iter_atomic → 用户→内核拷贝
                           │    └─ 将 iovec 各段数据拷贝到页缓存 folio
                           │
                           └─ a_ops->write_end → ext4_da_write_end     // fs/ext4/inode.c:3260
                                └─ 标记 folio 脏 + 解锁
                                     └─ [后续回写] writeback 线程将脏页写入磁盘
```

> pwritev 与 write 的差异仅在于 `vfs_write` vs `vfs_writev`：
> - `vfs_write`：`iov_iter_init(&iter, ITER_SOURCE, &iov, 1, count)` — 单段
> - `vfs_writev`：`import_iovec(ITER_SOURCE, vec, vlen, ...)` — 多段
> - `do_iter_readv_writev` 之后的路径完全一致

---

## 6 UIO_FASTIOV 优化

pwritev 使用 `import_iovec` 导入用户空间 iovec 数组，当 iovec 数量较少（≤ 8）时使用栈上数组避免 kmalloc：

```c
struct iovec iovstack[UIO_FASTIOV];   // UIO_FASTIOV 通常 = 8
struct iovec *iov = iovstack;
// ...
ret = import_iovec(ITER_SOURCE, vec, vlen, ARRAY_SIZE(iovstack), &iov, &iter);
// ...
kfree(iov);   // 如果 iov != iovstack，释放 kmalloc 的内存
```

| iovec 数量 | 分配方式 | 性能特征 |
|--|--|--|
| ≤ 8 | 栈上 `iovstack[8]` | 零分配，最快 |
| > 8 | `kmalloc` 动态分配 | 有分配开销 |

---

## 7 pwritev2 RWF 标志

pwritev2 通过 `flags` 参数支持额外的 `RWF_*` 标志：

| 标志 | 值 | 说明 |
|--|--|--|
| `RWF_DSYNC` | 0x01 | 类似 O_DSYNC，写完成前等待数据完整性 |
| `RWF_HIPRI` | 0x02 | 高优先级，polling 模式（需块设备支持） |
| `RWF_SYNC` | 0x04 | 类似 O_SYNC，写完成前等待数据+元数据完整性 |
| `RWF_NOWAIT` | 0x08 | 非阻塞，若 I/O 可能阻塞立即返回 -EAGAIN |
| `RWF_APPEND` | 0x10 | 追加模式，忽略 pos 参数，从 f_pos 处写入 |

`kiocb_set_rw_flags` 负责解析这些标志并设置 `kiocb.ki_flags`：

```c
static inline int kiocb_set_rw_flags(struct kiocb *ki, rwf_t flags, int type)
{
    if (flags & ~RWF_SUPPORTED)
        return -EOPNOTSUPP;

    if (flags & RWF_NOWAIT)
        ki->ki_flags |= IOCB_NOWAIT;
    if (flags & RWF_HIPRI)
        ki->ki_flags |= IOCB_HIPRI;
    if (flags & RWF_DSYNC)
        ki->ki_flags |= IOCB_DSYNC;
    if (flags & RWF_SYNC)
        ki->ki_flags |= IOCB_SYNC;
    return 0;
}
```

`RWF_APPEND` 标志的处理在 `do_pwritev` 中特殊处理：

```c
// 在 do_pwritev 中，RWF_APPEND 使 pos 失效
// 当 flags & RWF_APPEND 时，file_start_write 前需设置
// iocb->ki_flags |= IOCB_APPEND，之后 ext4 层会忽略 ki_pos
// 使用 file->f_pos 的当前值（等效于 O_APPEND 语义）
```

---

## 8 函数调用栈

```
/* ========== pwritev 主路径 ========== */
/* 结合定位语义 + 分散/聚集 I/O */

SYSCALL_DEFINE5(pwritev, fd, vec, vlen, pos_l, pos_h)  // fs/read_write.c:1475 — 系统调用入口
└─ pos_from_hilo(pos_h, pos_l)                         // arch/arm64/include/asm — 拼接 64 位 pos
└─ do_pwritev(fd, vec, vlen, pos, 0)                   // fs/read_write.c:1422 — 参数验证层
   ├─ [pos < 0] → return -EINVAL                       // 位置参数合法性校验
   ├─ CLASS(fd, f)(fd)                                  // fs/file.c — 通过 fd 获取 struct fd
   ├─ [fd_empty(f)] → return -EBADF                     // 文件描述符有效性检查
   ├─ [!(fd_file(f)->f_mode & FMODE_PWRITE)] → return -ESPIPE // 定位写权限检查
   │
   └─ vfs_writev(fd_file(f), vec, vlen, &pos, flags)   // fs/read_write.c:1225 — VFS 写入口
      │                                                  // &pos 指向栈上变量，不更新 f_pos
      ├─ [!(file->f_mode & FMODE_WRITE)] → return -EBADF // 写权限检查
      ├─ [!(file->f_mode & FMODE_CAN_WRITE)] → return -EINVAL // 写能力检查
      │
      ├─ import_iovec(ITER_SOURCE, vec, vlen, ...)      // lib/iov_iter.c — 导入多段 iovec
      │  ├─ [nr_segs ≤ UIO_FASTIOV(8)]                 // 栈上 iovstack[8] 零分配
      │  │  └─ iov = iovstack                           // 无 kmalloc 开销
      │  ├─ [nr_segs > 8]                               // 动态分配
      │  │  └─ iov = kmalloc_array(nr_segs, ...)        // 需要后续 kfree
      │  └─ __import_iovec() → 拷贝用户空间 iovec → 初始化 iov_iter
      │     └─ iter.iter_type = ITER_IOVEC              // 类型：多段 iovec
      │        iter.data_source = ITER_SOURCE           // 方向：从用户到内核
      │        iter.nr_segs = vlen                       // 段数
      │        iter.iov = iov                            // 指向 iovec 数组
      │
      ├─ rw_verify_area(WRITE, file, pos, tot_len)      // fs/read_write.c — 区域验证
      │  └─ security_file_permission(file, MAY_WRITE)   // LSM 安全钩子
      │
      ├─ file_start_write(file)                         // fs/file.c — 写保护（挂载冻结检测）
      │
      └─ do_iter_readv_writev(file, &iter, pos, WRITE, flags) // fs/read_write.c:1003 — 核心分发
         ├─ init_sync_kiocb(&kiocb, filp)               // include/linux/fs.h — 初始化 kiocb
         ├─ kiocb_set_rw_flags(&kiocb, flags, WRITE)    // 解析 RWF_* → IOCB_* 标志
         ├─ kiocb.ki_pos = *ppos                         // 赋值栈 pos（非 file->f_pos）
         │
         └─ filp->f_op->write_iter(&kiocb, &iter)       // → ext4_file_write_iter
            │                                             // fs/ext4/file.c:278 — ext4 写分发
            ├─ [IS_DAX(inode)] → ext4_dax_write_iter()   // DAX 直接访问路径
            ├─ [iocb->ki_flags & IOCB_DIRECT] → ext4_dio_write_iter() // DirectIO 路径
            │
            └─ ext4_buffered_write_iter(iocb, from)      // fs/ext4/file.c:300 — 缓冲写
               └─ generic_perform_write(iocb, from)      // mm/filemap.c:3390 — 通用页缓存写
                  │
                  │ [循环: 每轮写入一段数据]
                  │
                  ├─ a_ops->write_begin(&iocb, mapping, pos, ...) → ext4_da_write_begin
                  │  // fs/ext4/inode.c:3100 — 准备写入 folio
                  │  ├─ ext4_da_reserve_space(inode, ...)  // 预留延迟块
                  │  ├─ ext4_da_check_grid_need(...)       // 检查块组连续性
                  │  ├─ folio = __filemap_get_folio(...)   // 获取/创建页缓存 folio
                  │  │  ├─ [页缓存已存在] → 直接返回
                  │  │  └─ [页缓存不存在] → 分配新 folio 并加入页缓存
                  │  │
                  │  └─ grab_cache_page_write_begin(...)   // 锁定 folio 准备写入
                  │     └─ folio_lock(folio)               // 加锁，等待可能的并发读完成
                  │
                  ├─ copy_folio_from_iter_atomic(folio, ...)  // 用户→内核拷贝
                  │  └─ iov_iter_copy_from_user_atomic()      // 拷贝 iovec 各段数据到 folio
                  │     └─ [逐段拷贝] 将 iovec 各段数据写入页缓存
                  │
                  └─ a_ops->write_end(&iocb, mapping, pos, ...) → ext4_da_write_end
                     // fs/ext4/inode.c:3260 — 完成写入
                     ├─ ext4_da_reserve_metadata(inode, ...)  // 预留元数据块
                     ├─ ext4_mark_inode_dirty(inode, ...)     // 标记 inode 脏
                     ├─ ext4_journal_blocks_to_write(...)     // 日志块标记
                     └─ folio_unlock(folio)                   // 解锁 folio
                        folio_mark_dirty(folio)               // 标记脏页（供回写）
                        folio_put(folio)                       // 释放引用

  [完成后:]
  ├─ file_end_write(file)                                    // 写保护解除
  ├─ kfree(iov)                                              // 释放 iovec（若动态分配）
  ├─ [ret > 0] → add_wchar(current, ret)                     // 写字节统计
  └─ inc_syscw(current)                                      // 写系统调用计数
```

---

## 9 流程图

```
                   pwritev(fd, iov, iovcnt, offset)
                           |
                    +------v------+
                    | SYSCALL_    |  系统调用入口
                    | DEFINE5     |  (fs/read_write.c:1475)
                    | (pwritev)   |
                    +------+------+
                           |
                    +------v------+
                    | pos_from_   |  -- 拼接 64 位 pos
                    | hilo        |
                    +------+------+
                           |
                    +------v------+
                    | do_pwritev  |  -- [pos < 0] → -EINVAL
                    | (fs/read_   |  -- CLASS(fd, f) 获取 fd
                    |  write.c)   |  -- [FMODE_PWRITE] 检查
                    |  1422       |  -- vfs_writev 进入 VFS
                    +------+------+
                           |
                    +------v------+
                    | vfs_writev  |  -- [FMODE_WRITE] 检查
                    | (fs/read_   |  -- [FMODE_CAN_WRITE] 检查
                    |  write.c)   |  -- import_iovec(ITER_SOURCE)
                    |  1225       |  -- rw_verify_area
                    +------+------+
                           |
                    +------v------+
                    | import_iovec|  -- 导入用户空间多段 iovec
                    | (lib/iov_   |  -- [nr_segs ≤ 8] 栈上 iovstack
                    |  iter.c)    |  -- [nr_segs > 8] kmalloc
                    +------+------+
                           |
                    +------v------+
                    | file_start_ |  -- 写保护（挂载冻结检测）
                    | write       |
                    +------+------+
                           |
                    +------v------+
                    | do_iter_    |  -- init_sync_kiocb
                    | readv_writev|  -- kiocb.ki_pos = pos
                    | (fs/read_   |  -- f_op->write_iter
                    |  write.c)   |
                    +------+------+
                           |
                    +------v------+
                    | ext4_file_  |  ext4 写分发
                    | write_iter  |  (fs/ext4/file.c:278)
                    | (iocb,iter) |
                    +------+------+
                           |
              +-----v-----+-----v-----+
              |                   |
       +------v------+   +------v------+
       | IOCB_DIRECT  |   | 缓冲写路径  |
       | → ext4_dio_  |   | ext4_buffer |
       |   write_iter  |   | ed_write_  |
       +------+------+   |   iter      |
              |          +------+------+
                         +------v------+
                         | generic_    |
                         | perform_    |
                         | write       |
                         | (mm/filemap.|
                         |  c:3390)    |
                         +------+------+
                                |
                   +-----v-----+-----v-----+
                   | 循环每轮写入 folio     |
                   |                        |
             +------v------+       +------v------+
             | ext4_da_    |       | ext4_da_    |
             | write_begin |       | write_end   |
             | (fs/ext4/   |       | (fs/ext4/   |
             |  inode.c:   |       |  inode.c:   |
             |  3100)      |       |  3260)      |
             +------+------+       +------+------+
                    |                     |
             +------v------+             |
             | 延迟块分配   |             |
             | 获取 folio  |             |
             | + 锁定 folio|             |
             +------+------+             |
                    |                     |
             +------v------+             |
             | copy_folio_ |             |
             | from_iter_  |             |
             | atomic      |             |
             | (用户→内核   |             |
             |  拷贝)       |             |
             +------+------+             |
                    |                     |
                    +------+------+------+
                           |
                    +------v------+
                    | 标记 folio  |
                    | 脏 + 解锁   |
                    | (folio_     |
                    |  mark_dirty,|
                    |  folio_     |
                    |  unlock)    |
                    +------+------+
                           |
                    +------v------+
                    | file_end_   |  -- 写保护解除
                    | write       |
                    +------+------+
                           |
                    +------v------+
                    | 返回写入字节数|
                    | 或错误码      |
                    +-------------+
```

---

## 10 与相近系统调用的对比

| 维度 | writev | pwritev | write | pwrite64 |
|--|--|--|--|--|
| **偏移来源** | `file->f_pos` | 栈变量 `pos` | `file->f_pos` | 栈变量 `pos` |
| **偏移更新** | 是 | 否 | 是 | 否 |
| **分散/聚集** | 是（多 iovec） | 是（多 iovec） | 否（单 buf） | 否（单 buf） |
| **FMODE_PWRITE** | 不需要 | 需要 | 不需要 | 需要 |
| **写保护** | `file_start_write` | `file_start_write` | `file_start_write` | `file_start_write` |
| **下游路径** | ext4→...→NVMe | ext4→...→NVMe | ext4→...→NVMe | ext4→...→NVMe |

---

## 11 关键数据结构 (C代码 + 注释)

```c
// ===== VFS 层 =====

// 用户空间 I/O 向量——pwritev 通过 iovec 数组传递多个写缓冲区
struct iovec {
    void __user *iov_base;   // 用户空间缓冲区基地址（pwritev 写入数据的来源）
    size_t       iov_len;    // 该段缓冲区长度
};

// 多段缓冲区迭代器——pwritev 使用 ITER_SOURCE（数据从用户空间写入文件）
struct iov_iter {
    u8 iter_type;            // 迭代器类型：ITER_IOVEC（多段 iovec，pwritev 使用）/ ITER_UBUF（单段）
    u8 data_source;          // 数据方向：ITER_SOURCE（pwritev: 从 iovec 各段写入文件）
    size_t iov_offset;       // 当前 iovec 段内的偏移（跨段续传时使用）
    size_t count;            // 剩余未传输字节总数
    union {
        const struct iovec *iov;       // 指向 iovec 数组（import_iovec 导入）
        struct {
            void __user *ubuf;         // 用户缓冲区基地址（单段模式）
            size_t len;                // 缓冲区长度
        };
    };
    unsigned long nr_segs;   // iovec 段数（pwritev 核心参数，vlen 传入）
};

// I/O 控制块——携带 pwritev 写操作的所有上下文
struct kiocb {
    struct file      *ki_filp;       // 目标文件对象（通过 fd 查找获得）
    loff_t            ki_pos;        // 写入位置（pwritev 使用栈变量 pos，不影响 file->f_pos）
    unsigned short    ki_opcode;     // I/O 操作码
    unsigned short    ki_flags;      // I/O 标志：IOCB_DIRECT（直接 I/O）、IOCB_NOWAIT（非阻塞）等
    short             ki_ioprio;     // I/O 优先级
    void              *private;      // 文件系统私有数据
    union {
        void          (*ki_complete)(struct kiocb *iocb, long ret);
        // 异步 I/O 完成回调（同步操作时为 NULL）
    };
};

// 页缓存 folio——pwritev 写入数据的载体
struct folio {
    unsigned long flags;     // folio 标志：PG_dirty（脏页）、PG_uptodate（数据有效）、PG_locked（锁定）等
    struct address_space *mapping;  // 所属的 address_space（文件页缓存树）
    loff_t index;            // 在文件内的页索引（pos >> PAGE_SHIFT）
    void *private;           // 文件系统私有数据（ext4 的 buffer_head 链表）
    atomic_t _mapcount;      // 映射计数
    atomic_t _refcount;      // 引用计数（页缓存引用 + 进程映射）
};

// ===== ext4 文件系统层 =====

// ext4 I/O 提交结构——管理写路径的 bio 构建和提交
struct ext4_io_submit {
    struct bio          *io_bio;          // 当前正在构建的 bio
    struct ext4_io_end  *io_end;          // I/O 完成处理结构
    sector_t             io_next_block;   // 下一个要写入的块扇区号
    struct super_block   *io_sb;          // 超级块（错误处理）
    unsigned int         io_flags;        // 提交标志（REQ_FUA、REQ_SYNC）
};

// ext4 I/O 完成结构——写完成后的回调处理
struct ext4_io_end {
    struct inode        *inode;           // 所属 inode
    loff_t               offset;          // 写入偏移范围起始
    size_t               size;            // 写入大小
    int                  error;           // 错误码
    struct work_struct   work;            // 工作队列项（异步完成处理）
};

// ===== 块层 =====

// 块 I/O 请求——提交到块设备的核心 I/O 单元
struct bio {
    struct bio          *bi_next;         // bio 链表（plug 聚合时使用）
    struct block_device *bi_bdev;         // 目标块设备
    blk_opf_t            bi_opf;          // 操作标志：REQ_OP_WRITE（pwritev 写操作）
    unsigned short       bi_flags;        // bio 标志
    unsigned short       bi_ioprio;       // I/O 优先级
    struct bio_vec       *bi_io_vec;      // 数据段数组
    unsigned int         bi_vcnt;         // bio_vec 段数
    struct bvec_iter     bi_iter;         // 当前迭代位置（bi_sector 为起始扇区）
    bio_end_io_t         *bi_end_io;      // 完成回调：pwritev → ext4_end_bio
    void                 *bi_private;     // 私有数据
};

// ===== NVMe 驱动层 =====

// NVMe 命令结构——提交到 NVMe 控制器的写命令
struct nvme_command {
    struct {
        u8  opcode;          // 操作码：nvme_cmd_write = 0x01（pwritev 写命令）
        u8  flags;           // 命令标志
        u16 command_id;      // 命令 ID（用于匹配完成）
    };
    __le32 nsid;             // 命名空间 ID
    __le64 prp1;             // PRP1 物理区域指针（DMA 数据来源地址）
    __le64 prp2;             // PRP2（若数据跨页）
    __le32 cdw10;            // 起始 LBA（逻辑块地址）
    __le16 cdw11;            // 块数 (length - 1)
};

// NVMe 队列——SQ/CQ 对，管理命令提交和完成
struct nvme_queue {
    struct nvme_dev     *dev;             // NVMe 设备
    struct nvme_command *sq_cmds;         // 提交队列 (SQ) 环缓冲区
    volatile struct nvme_completion *cqes; // 完成队列 (CQ) 环缓冲区
    dma_addr_t           sq_dma_addr;     // SQ DMA 地址
    dma_addr_t           cq_dma_addr;     // CQ DMA 地址
    u32 __iomem          *sq_tail_doorbell_addr;  // SQ 门铃寄存器（MMIO 写，通知硬件取命令）
    u32 __iomem          *cq_head_doorbell_addr;  // CQ 门铃寄存器（MMIO 写，通知硬件已处理完成）
    unsigned int         sq_tail;         // SQ 环尾指针
    unsigned int         cq_head;         // CQ 环头指针
    unsigned int         cq_phase;        // CQ 阶段位（区分新旧完成项）
};
```

| 数据结构 | 头文件 | 在 pwritev 中的作用 |
|----------|--------|-------------------|
| `struct iovec` | `include/uapi/linux/uio.h` | 用户空间多段缓冲区描述（分散写的基础） |
| `struct iov_iter` | `include/linux/uio.h` | 管理多段 iovec 迭代，data_source=ITER_SOURCE |
| `struct kiocb` | `include/linux/fs.h` | 携带 I/O 位置 pos 和标志，传递到 ext4 层 |
| `struct folio` | `include/linux/mm_types.h` | 页缓存单元，pwritev 写入后标记脏 |
| `struct ext4_io_submit` | `fs/ext4/ext4.h` | ext4 写 bio 管理 |
| `struct ext4_io_end` | `fs/ext4/ext4.h` | ext4 写完成回调处理 |
| `struct bio` | `include/linux/blk_types.h` | 块层 I/O 单元，REQ_OP_WRITE |
| `struct nvme_command` | `drivers/nvme/host/nvme.h` | NVMe 写命令 (opcode=0x01) |
| `struct nvme_queue` | `drivers/nvme/host/nvme.h` | SQ/CQ 队列管理，MMIO 门铃操作 |

---

## 12 总结

pwritev 将**定位 I/O**和**分散/聚集 I/O**两个特性结合：

1. **定位语义**（来自 pwrite64）：栈局部变量 `pos`，不更新 `f_pos`，消除偏移竞争
2. **分散/聚集**（来自 writev）：`import_iovec` 导入多段 iovec，`UIO_FASTIOV` 栈优化
3. **pwritev2 扩展**：`RWF_*` 标志支持 NOWAIT、DSYNC、HIPRI、APPEND 等特性
4. **下游路径完全共享**：pwritev 在 `do_iter_readv_writev` 之后与 write 完全相同
5. **ARM64 参数编码**：64 位 `loff_t pos` 从两个 32 位参数 `pos_h`/`pos_l` 拼装

关键函数调用等价关系：
```
pwritev(fd, vec, vlen, pos)  =  writev 的分散/聚集 + pwrite64 的定位语义
pwritev2(fd, vec, vlen, pos, RWF_APPEND) = pwritev + 追加语义
```