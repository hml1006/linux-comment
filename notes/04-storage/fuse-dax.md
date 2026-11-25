# FUSE、virtiofs 与 DAX

## 1. 概述

FUSE（Filesystem in Userspace）把文件系统实现搬到用户态：内核只负责 VFS 适配、请求排队与页缓存，真正的元数据/数据操作由用户态守护进程完成。内核与守护进程之间通过 `/dev/fuse` 字符设备（或 virtiofs 的 virtqueue）交换 FUSE 协议消息。

```
用户进程 read/write/mmap -> VFS
  -> fs/fuse (inode.c / file.c / dir.c)
  -> fs/fuse/dev.c: fuse_req 排队、拷贝、等待应答
  -> /dev/fuse           或 virtiofs virtqueue
  -> libfuse 守护进程    或 virtiofsd
```

三条数据通路：

- **缓冲 IO**：走页缓存，`fuse_perform_write()` 把 folio 内容封装成 FUSE_WRITE 发往守护进程；读在缺页时发 FUSE_READ。
- **直接 IO**：`FOPEN_DIRECT_IO` 时 `fuse_direct_IO()` 绕过页缓存，通过 FUSE_READ/FUSE_WRITE 直接传用户缓冲。
- **DAX**：守护进程把文件数据映射到一段共享内存窗口，内核用 iomap + `dax_iomap_rw()` 直接访问，读写不经过 FUSE 请求。

当前 `.config`：`CONFIG_FUSE_FS=y`、`CONFIG_FUSE_PASSTHROUGH=y`、`CONFIG_FUSE_IO_URING=y`；`CONFIG_VIRTIO_FS is not set`、`CONFIG_DAX is not set`（因此 `CONFIG_FUSE_DAX` 也不可用）。本内核可挂载普通 FUSE，但 virtiofs 与 DAX 路径仅存在于源码。

---

## 2. FUSE 协议

### 2.1 消息头

`include/uapi/linux/fuse.h` 定义两条消息头：`fuse_in_header`（`len`、`opcode`、`unique`、`nodeid`、`uid`/`gid`/`pid`、`total_extlen`）与 `fuse_out_header`（`len`、`error`、`unique`）。`unique` 用于匹配请求与应答；`error` 为 0 或负 errno；`unique == 0` 表示守护进程主动通知。

常用 opcode：`FUSE_INIT=26`、`FUSE_LOOKUP`、`FUSE_GETATTR`、`FUSE_OPEN`、`FUSE_READ=15`、`FUSE_WRITE=16`、`FUSE_FLUSH`、`FUSE_RELEASE`、`FUSE_FSYNC`、`FUSE_SETXATTR`；DAX 专用 `FUSE_SETUPMAPPING=48`、`FUSE_REMOVEMAPPING=49`。

### 2.2 INIT 协商

挂载后内核发 `FUSE_INIT`，携带 `fuse_init_in`（major/minor、max_readahead、flags）与 `fuse_init_out`（max_write、max_background、congestion_threshold、flags2）。`process_init_reply()`（`fs/fuse/inode.c`）根据 INIT 应答设置 `max_write`、`max_pages`、`max_read` 与能力位（`FUSE_BIG_WRITES`、`FUSE_ASYNC_READ`、`FUSE_WRITEBACK_CACHE`、`FUSE_MAP_ALIGNMENT`、`FUSE_HAS_INODE_DAX` 等）；只有声明 `FUSE_MAP_ALIGNMENT` 的连接才允许使用 DAX 窗口。`fuse_adjust_compat()` 处理旧协议（minor < 4/9/12）的结构体大小差异。

### 2.3 请求/应答匹配

每个请求有唯一 `unique`。守护进程读 `/dev/fuse` 得到 `fuse_in_header` + 参数，处理后写回 `fuse_out_header` + 结果；内核按 `unique` 在 `fpq->processing` 中查找对应 `fuse_req`。`unique == 0` 是守护进程主动通知（invalidate、store、retrieve），走 `fuse_notify()`。中断请求的 `unique` 带 `FUSE_INT_REQ_BIT`。

---

## 3. 内核数据结构

### 3.1 fuse_conn 与 fuse_chan

`struct fuse_conn`（`fs/fuse/fuse_i.h`）是每个挂载实例的全局状态：

```c
struct fuse_conn {
    spinlock_t lock;
    refcount_t count;
    atomic_t epoch;                 /* dentry 失效纪元 */
    kuid_t user_id;
    kgid_t group_id;
    struct pid_namespace *pid_ns;
    struct user_namespace *user_ns;
    unsigned max_read, max_write;
    unsigned int max_pages, max_pages_limit;
    struct fuse_chan *chan;         /* 传输层对象 */
    atomic64_t khctr;               /* 下一个文件句柄 */
    struct dax_device *dax_dev;
    unsigned int inode_dax:1;
    enum fuse_dax_mode dax_mode;    /* ALWAYS / NEVER / INODE_USER */
    struct fuse_conn_dax *dax;
};
```

`struct fuse_chan`（`fs/fuse/fuse_dev_i.h`）代表一条连接通道，内含输入队列 `iq`、所属 `devices` 列表、后台请求计数（`max_background`、`num_background`）以及 `initialized`/`blocked` 状态。

### 3.2 fuse_iqueue 与 fuse_pqueue

```c
struct fuse_iqueue {
    unsigned connected;
    spinlock_t lock;
    wait_queue_head_t waitq;     /* 守护进程 read 在此等待 */
    u64 reqctr;                  /* unique 计数器 */
    struct list_head pending;    /* 待守护进程取走的请求 */
    struct list_head interrupts; /* 待发送的 INTERRUPT */
    struct fuse_forget_link forget_list_head;
    /* ... */
};
```

`fuse_pqueue`（每 `/dev/fuse` 打开实例）保存 `processing`（已发给用户态、等待应答）、`io`（正在拷贝应答）与 tag 位图。`fuse_chan` 是连接级，`fuse_pqueue` 是设备级，支持多个进程同时打开 `/dev/fuse`。

### 3.3 fuse_req

```c
struct fuse_req {
    struct list_head list;       /* pending 或 io 链表 */
    struct list_head intr_entry;
    struct fuse_args *args;
    refcount_t count;
    unsigned long flags;         /* FR_PENDING / FR_SENT / FR_FINISHED ... */
    struct { struct fuse_in_header h; } in;
    struct { struct fuse_out_header h; } out;
    wait_queue_head_t waitq;     /* 调用者等待完成 */
#if IS_ENABLED(CONFIG_VIRTIO_FS)
    void *argbuf;                /* virtio-fs 物理连续缓冲 */
#endif
    struct fuse_chan *chan;
#ifdef CONFIG_FUSE_IO_URING
    void *ring_entry, *ring_queue;
#endif
    unsigned long create_time;
};
```

`fuse_args`（`fs/fuse/args.h`）描述一次调用的参数布局：`nodeid`、`opcode`、`uid/gid/pid`、`in_numargs`/`out_numargs`、`in_args[4]`/`out_args[2]`，以及 `force`、`noreply`、`in_pages`、`out_pages`、`page_zeroing`、`page_replace`、`may_block`、`is_ext`、`abort_on_kill` 等布尔标志和 `end` 回调。`fuse_simple_request(fm, args)` 是最常用的同步封装：分配 `fuse_req`、填头、入队并等待应答。

### 3.4 fuse_inode

`struct fuse_inode` 内嵌 `struct inode`：

```c
struct fuse_inode {
    struct inode inode;
    u64 nodeid;            /* 用户态 inode 标识 */
    u64 nlookup;           /* LOOKUP 计数，FORGET 时递减 */
    struct fuse_forget_link *forget;
    u64 i_time;            /* 属性有效期 */
    u32 inval_mask;
    umode_t orig_i_mode;
    u64 orig_ino;
    u64 attr_version;
    union {
        struct { struct list_head write_files, queued_writes;
                 int writectr, iocachectr;
                 wait_queue_head_t page_waitq, direct_io_waitq; };
        struct { bool cached; loff_t size, pos; u64 version;
                 struct timespec64 mtime; int epoch; u64 iversion;
                 spinlock_t lock; } rdc;
    };
    unsigned long state;
    struct mutex mutex;
    spinlock_t lock;
    struct fuse_inode_dax *dax;              /* CONFIG_FUSE_DAX */
    struct fuse_submount_lookup *submount_lookup;
    struct file *fb;                         /* CONFIG_FUSE_PASSTHROUGH */
};
```

`nodeid` 是内核与守护进程之间的 inode 键；VFS 的 `i_ino` 可以是 `orig_ino` 或本地合成值。

---

## 4. 请求路径

### 4.1 内核 -> 用户态

```
fuse_simple_request / fuse_request_send
  -> fuse_send_one(fiq, req)         /* 放入 fiq->pending，唤醒等待者 */
  -> request_wait_answer(req)        /* 在 req->waitq 上等待 FR_FINISHED */
```

```
fuse_dev_do_read()
  ├─ 校验用户缓冲 >= FUSE_MIN_READ_BUFFER（且容纳 max_write）
  ├─ 在 fiq->waitq 等待 request_pending()，非阻塞返回 -EAGAIN
  ├─ 优先处理 interrupts（FUSE_INTERRUPT）与批量 forget
  ├─ 从 fiq->pending 取请求，清 FR_PENDING，挂到 fpq->processing
  └─ 拷贝头部与参数到用户空间
```

### 4.2 用户态 -> 内核

```
fuse_dev_do_write()
  ├─ 读 fuse_out_header，校验 oh.len == nbytes
  ├─ unique == 0 -> fuse_notify()（要求 initialized 且 connected）
  ├─ 校验 oh.error ∈ [-511, 0]，按 unique 在 fpq->processing 找 req
  ├─ unique 带 FUSE_INT_REQ_BIT -> 中断应答特殊处理
  ├─ 清 FR_SENT，挂到 fpq->io，置 FR_LOCKED，拷入应答数据
  └─ fuse_request_end(req)，唤醒等待者
```

### 4.3 中断与超时

`FUSE_INTERRUPT` 允许守护进程请求中止某个未完成请求：内核在 `fiq->interrupts` 排队一个带 `FUSE_INT_REQ_BIT` 的请求；守护进程若回 `-EAGAIN` 表示暂不能中断，回 `-ENOSYS` 则内核置 `no_interrupt` 不再尝试。`request_wait_answer()` 处理 `FR_INTERRUPTED`、`FR_FORCE`、`FR_BACKGROUND` 与信号（`abort_on_kill`）组合。

`FUSE_FORGET` 是 no-reply 请求：`nlookup` 降到 0 时内核批量发送 forget，减少与守护进程的往返。

---

## 5. 文件读写路径

### 5.1 分发

`fuse_file_read_iter()` / `fuse_file_write_iter()`（`fs/fuse/file.c`）按打开标志分流：

```c
if (FUSE_IS_DAX(inode))                 return fuse_dax_*_iter(...);
if (ff->open_flags & FOPEN_DIRECT_IO)   return fuse_direct_*_iter(...);
else if (fuse_file_passthrough(ff))     return fuse_passthrough_*_iter(...);
else                                    return fuse_cache_*_iter(...);
```

`FOPEN_DIRECT_IO` 优先级高于 passthrough；passthrough 是 7.x 新增的“直接转发到 backing 文件”机制（`fs/fuse/passthrough.c`）。

### 5.2 缓冲写

```
fuse_cache_write_iter -> fuse_perform_write
  ├─ fuse_folios_alloc(nr_pages)；fuse_fill_write_pages() 取/建 folio 并拷贝
  ├─ fuse_send_write_pages() 发 FUSE_WRITE（携带 folio 页）
  └─ fuse_write_update_attr(inode, pos, res)
```

`FUSE_I_SIZE_UNSTABLE` 在写超过 EOF 时置位，防止并发读取到临时 i_size。短写（`num_written != count`）被当作 `-EIO`。

### 5.3 直接 IO

`fuse_direct_IO()` 用 `iov_iter` 直接构造 FUSE_READ/FUSE_WRITE，数据页通过 `fuse_args_pages` 传给守护进程；`FOPEN_DIRECT_IO` 保证不经过页缓存，适合数据库等自管理缓存的场景。写需要独占锁（`fuse_direct_write_iter` 中的 `inode_lock` 判断）。

### 5.4 通知与失效

守护进程可发 `FUSE_NOTIFY_INVAL_INODE`、`FUSE_NOTIFY_INVAL_ENTRY`、`FUSE_NOTIFY_STORE`、`FUSE_NOTIFY_RETRIEVE`、`FUSE_NOTIFY_DELETE`。内核在 `fuse_notify()` 中分派：失效 inode 走 `fuse_invalidate_attr`/`fuse_invalidate_entry`，store 把数据写入页缓存，retrieve 从页缓存读回。`fc->epoch` 机制批量失效旧 dentry。

---

## 6. virtiofs

`fs/fuse/virtio_fs.c` 把 FUSE 传输层从 `/dev/fuse` 换成 virtio。`struct virtio_fs`：

```c
struct virtio_fs {
    struct kobject kobj;
    struct list_head list;
    char *tag;                       /* mount tag */
    struct virtio_fs_vq *vqs;
    unsigned int nvqs;
    unsigned int num_request_queues;
    struct dax_device *dax_dev;
    unsigned int *mq_map;            /* CPU -> 请求队列 */
    void *window_kaddr;              /* DAX 共享窗口 */
    phys_addr_t window_phys_addr;
    size_t window_len;
};
```

- **队列划分**：一个 hiprio 队列处理 forget/interrupt 等高优先级消息，其余为 request 队列；`virtio_fs_setup_vqs()` 按 CPU 建立 `mq_map`，实现多队列并行。
- **请求提交**：`virtio_fs_enqueue_req()` 把 `fuse_req` 的 in/out 参数组装成 scatterlist，`virtqueue_add_sgs()` 后 kick；完成中断里 `fuse_request_end()`。
- **DAX 窗口**：`virtio_fs_setup_dax()` 把 virtio 共享内存区域注册为 `dax_device`，窗口物理地址写入 `virtio_fs`；FUSE_SETUPMAPPING 让后端把文件区间映射到该窗口的偏移。
- **与 /dev/fuse 的关系**：两者共用 `fuse_conn`、`fuse_req`、协议头，只是 `fuse_iqueue_ops` 不同（`send_req`/`send_forget`/`send_interrupt` 由 virtio 实现）。

本内核 `CONFIG_VIRTIO_FS is not set`，该路径不会编译。

---

## 7. DAX

### 7.1 概念

DAX（Direct Access）让文件数据直接映射到持久内存或共享内存窗口，读写不经过页缓存，也不需要块层 IO。通用实现在 `fs/dax.c`，文件系统通过 iomap 提供映射：

```c
struct dax_operations {
    long (*direct_access)(struct dax_device *, pgoff_t, long,
                          enum dax_access_mode, void **, unsigned long *);
    int (*zero_page_range)(struct dax_device *, pgoff_t, size_t);
    size_t (*recovery_write)(struct dax_device *, pgoff_t, void *,
                             size_t, struct iov_iter *);
};
```

### 7.2 通用读写路径

- `dax_iomap_rw(iocb, iter, ops)`：按 `iomap_iter()` 逐段取映射，`dax_iomap_iter()` 把用户 iov 直接拷入/拷出 DAX 地址；写路径要求持有 `i_rwsem` 写锁。
- `dax_iomap_fault(vmf, order, ...)`：缺页时 `dax_iomap_pte_fault()` 或 `dax_iomap_pmd_fault()`，把 DAX 页的 pfn 填入 PTE/PMD，实现 mmap 直接访问。
- `dax_finish_sync_fault()` 在需要保证持久化时刷 cache。

### 7.3 FUSE DAX

`fs/fuse/dax.c` 用区间树管理“文件偏移 -> DAX 窗口偏移”的映射：

```c
struct fuse_conn_dax {
    struct dax_device *dev;
    spinlock_t lock;
    unsigned long nr_busy_ranges;
    struct list_head busy_ranges;    /* 已映射区间 */
    struct delayed_work free_work;   /* 回收空闲窗口 */
    wait_queue_head_t range_waitq;
    long nr_free_ranges;
    struct list_head free_ranges;    /* 空闲窗口块 */
    unsigned long nr_ranges;
};
```

每个映射用 `fuse_dax_mapping` 表示：`itn`（文件内 start_idx..last_idx）、`window_offset`（窗口内偏移）、`refcnt`、`writable` 与所属 `inode`，挂在 `fi->dax->tree` 区间树上。

映射建立流程：

1. `fuse_iomap_begin()` 以 `pos >> FUSE_DAX_SHIFT`（窗口块 2MB）查 `fi->dax->tree`；
2. 命中且可写则 `fuse_fill_iomap()`；命中但只读且本次要写，则 `fuse_upgrade_dax_mapping()`；
3. 未命中且 `pos < i_size`，`fuse_setup_new_dax_mapping()` → `alloc_dax_mapping_reclaim()` 从 free/busy 池取窗口块；
4. `fuse_setup_one_mapping()` 发 `FUSE_SETUPMAPPING`（`fuse_setupmapping_in` 带 `foffset`、`moffset`、`len`、读写 flag），后端把文件区间映射到窗口；
5. `fuse_fill_iomap()` 填 `iomap->addr = window_offset`、`iomap->dax_dev = fc->dax->dev`、长度与 flag。

回收：`free_work` 在空闲区间低于阈值时 `dmap_removemapping_one()` 发 `FUSE_REMOVEMAPPING`；inode 删除时 `fuse_dax_inode_cleanup()` 清理整棵区间树。`fuse_dax_break_layouts()` 在写入前打断现有 DAX 映射，保证一致性。

DAX 与普通路径的边界：`fuse_iomap_begin()` 不支持 `IOMAP_REPORT`（FIEMAP），越界读返回 hole；DAX 只对声明 `FUSE_MAP_ALIGNMENT` 的连接启用，且要求文件偏移与窗口按 `FUSE_DAX_SZ` 对齐。

当前 `CONFIG_DAX is not set`，`fs/dax.c` 与 `fs/fuse/dax.c` 均不编译。

---

## 8. 当前配置与可观察行为

| 配置项 | 值 | 影响 |
|--------|----|------|
| `CONFIG_FUSE_FS` | `y` | 可挂载 `fuse`/`fuseblk`，`/dev/fuse` 可用 |
| `CONFIG_FUSE_PASSTHROUGH` | `y` | `fs/fuse/passthrough.c` 编译，支持 backing 文件直通 |
| `CONFIG_FUSE_IO_URING` | `y` | `fs/fuse/dev_uring.c` 编译，可用 io_uring 提交 FUSE 请求 |
| `CONFIG_VIRTIO_FS` | `is not set` | 无 virtiofs 传输层，`virtio_fs.c` 不编译 |
| `CONFIG_DAX` | `is not set` | `fs/dax.c`、`fs/fuse/dax.c` 不编译，DAX 选项不可用 |

因此本内核可做普通 FUSE 的实验（例如 libfuse 示例），但 virtiofs 与 DAX 只能阅读源码。FUSE 的 `/dev/fuse` 读写路径、请求生命周期与协议协商逻辑与上游一致。

---

## 关键文件清单

| 文件 | 内容 |
|------|------|
| `include/uapi/linux/fuse.h` | FUSE 协议：消息头、opcode、各 in/out 结构、SETUPMAPPING |
| `fs/fuse/fuse_i.h` | `fuse_conn`、`fuse_inode`、`fuse_mount`、`fuse_simple_request` 内联封装 |
| `fs/fuse/fuse_dev_i.h` | `fuse_chan`、`fuse_iqueue`、`fuse_pqueue`、`fuse_req`、`fuse_dev` |
| `fs/fuse/args.h` | `fuse_args`、`fuse_args_pages`、参数描述 |
| `fs/fuse/dev.c` | `fuse_dev_do_read`、`fuse_dev_do_write`、`request_wait_answer`、`fuse_dev_install` |
| `fs/fuse/dev_uring.c` | io_uring 方式的请求提交与完成 |
| `fs/fuse/inode.c` | 挂载/卸载、`process_init_reply`、连接参数与 DAX 能力协商 |
| `fs/fuse/file.c` | `fuse_file_read_iter`/`write_iter`、`fuse_perform_write`、`fuse_direct_IO` |
| `fs/fuse/dir.c` | lookup/create/unlink 等元数据操作 |
| `fs/fuse/dax.c` | `fuse_iomap_begin`、窗口映射建立/回收、`fuse_conn_dax` |
| `fs/fuse/virtio_fs.c` | virtiofs 队列、请求提交、DAX 窗口注册 |
| `fs/dax.c` | `dax_iomap_rw`、`dax_iomap_fault`、DAX 页表与回写 |
| `include/linux/dax.h` | `dax_device`、`dax_operations` |
| `fs/iomap/iter.c` | `iomap_iter()`，DAX 与普通文件系统共用 |
