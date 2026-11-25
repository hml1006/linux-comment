# overlayfs 与网络文件系统

## 1. 概述与分层

overlayfs 是“联合挂载”文件系统：把只读的 lower 层与可写的 upper 层叠加成单一视图，写时触发 copy-up，删除用 whiteout 表示。网络文件系统（NFS、9p、Ceph）则把 VFS 请求翻译成网络协议，并共享 `fs/netfs/` 提供的缓冲 IO 框架。

```
        VFS (inode / dentry / address_space / file)
           |                         |
   fs/overlayfs/               fs/netfs/  (netfs_io_request 通用读写框架)
   lower/upper 合成               |            |           |
   copy-up / whiteout        fs/nfs/       fs/9p/      fs/ceph/
                              NFS RPC     9P RPC      OSD/MDS 请求
```

两条主线：

- **overlayfs**：不产生新数据，只做命名空间合并。关键在 dentry 的 `ovl_entry`（layers 栈）、inode 的 `ovl_inode`（upper dentry + lower 数据）与 copy-up 时机。
- **netfs**：把“读/写一个文件区间”抽象成 `netfs_io_request`，拆成多个 `netfs_io_subrequest` 并发下发；文件系统只需实现 `netfs_request_ops`。9p 与 Ceph 已全面接入，NFS 主要用于 fscache 路径。

当前 `.config`：`# CONFIG_OVERLAY_FS is not set`、`# CONFIG_NETWORK_FILESYSTEMS is not set`（因此 NFS/9P/Ceph 与 `NETFS_SUPPORT` 均不可用）、`# CONFIG_CEPH_LIB is not set`。本文所述均为源码机制，本内核中不能挂载。

---

## 2. overlayfs 数据结构

### 2.1 ovl_fs 与 ovl_layer

`fs/overlayfs/ovl_entry.h`：

```c
struct ovl_fs {
    unsigned int numlayer;      /* 包含 upper 的总层数 */
    unsigned int numfs;         /* 唯一底层文件系统数 */
    unsigned int numdatalayer;  /* data-only lower 层数 */
    struct ovl_layer *layers;
    struct ovl_sb *fs;
    struct dentry *workbasedir, *workdir;
    long namelen;
    struct ovl_config config;
    const struct cred *creator_cred;
    bool tmpfile, noxattr, nofh;
    bool upperdir_locked, workdir_locked;
    int xino_mode;              /* -1 关闭；0 同 fs；1..32 未用 ino 位数 */
    atomic_long_t last_ino;
    struct dentry *whiteout;    /* 共享 whiteout 缓存 */
    struct mutex whiteout_lock;
};
```

`struct ovl_layer` 保存 `mnt`（必须是第一个成员）、`trap` inode、`fs`、`idx`（upper 为 0）、`fsid` 与 `has_xwhiteouts`。`layers[0]` 是 upper，其余是 lower；`ovl_upper_mnt(ofs)` 直接返回 `layers[0].mnt`。

### 2.2 ovl_entry 与 ovl_inode

dentry 的私有数据是 `ovl_entry`，按层记录路径：`struct ovl_path { const struct ovl_layer *layer; struct dentry *dentry; }`，`struct ovl_entry { unsigned int __numlower; struct ovl_path __lowerstack[] __counted_by(__numlower); }`。

`ovl_inode` 内嵌 VFS inode，保存 upper dentry 与完整的 `ovl_entry`：

```c
struct ovl_inode {
    union {
        struct ovl_dir_cache *cache;       /* 目录 */
        const char *lowerdata_redirect;    /* 普通文件 */
    };
    const char *redirect;
    u64 version;
    unsigned long flags;
    struct inode vfs_inode;
    struct dentry *__upperdentry;  /* 一旦 copy-up 即非空 */
    struct ovl_entry *oe;
    struct mutex lock;             /* 串行化 copy-up */
};
```

`OVL_I(inode)` 由 `vfs_inode` 反推 `ovl_inode`；`OVL_E(dentry)` 取 `ovl_entry`。`__upperdentry` 用 `ovl_upperdentry_dereference()` 读取，copy-up 成功后原子替换。

---

## 3. overlayfs 查找与 copy-up

### 3.1 lookup

`ovl_lookup()`（`namei.c`）在 upper 与各 lower 层依次查找同名 dentry：

1. 先查 upper；命中且不是 whiteout 则以 upper 为主；
2. 再逐层查 lower，构造 `ovl_entry.lowerstack[]`；
3. 遇到 whiteout（char device 0/0 或 `overlay.whiteout` xattr）即停止向下；upper 目录带 opaque xattr（`trusted.overlay.opaque=y`）时忽略所有 lower；
4. 用 `ovl_get_inode()` 建立/复用 `ovl_inode`，其 `i_ino` 由 xino 规则生成（同 fs 用真实 ino，否则分配持久/非持久 ino）。

目录的 `redirect` 与 `index` 特性允许把 lower 目录重定向到 upper 的其他位置，用于 `redirect_dir=on` 与 NFS export。

### 3.2 copy-up 触发点

以下操作在 upper 缺失时必须先 copy-up：

- `open(O_WRONLY/O_RDWR)`、`truncate`、`setattr`、`chmod/chown`；
- `link`、`rename`、`unlink`、`mkdir` 等修改目录项；
- `mmap(MAP_SHARED)` 写、`fallocate`；
- 设置 xattr/ACL。

`ovl_copy_up()`（`copy_up.c`）→ `ovl_copy_up_flags()` → 逐级 `ovl_copy_up_one(parent, dentry, flags)`，父目录先 copy-up，保证 upper 路径完整。

### 3.3 copy-up 实现

`ovl_copy_up_one()` 的关键步骤：

1. 在 workdir 创建临时文件（`ovl_create_temp()`），继承 lower 的 mode/uid/gid；
2. `ovl_copy_up_data(&ctx, &temp)` 复制数据：普通文件用 `vfs_copy_file_range()` 或读写循环，稀疏文件保留 hole；
3. 复制 xattr、ACL、security label；处理 origin 文件句柄（`trusted.overlay.origin`）以支持 index；
4. `ovl_set_redirect()` 视需要写入 redirect xattr；
5. `ovl_do_rename()` 把临时文件原子改名到 upper 目标位置；
6. 更新 `ovl_inode.__upperdentry`，释放临时 inode。

copy-up 由 `ovl_inode.lock` 串行化；并发写同一文件时只有一个执行复制，其余等待。大文件 copy-up 可能很慢，因此 overlayfs 支持 `metacopy=on`：只复制元数据，数据仍指向 lower，首次写数据时再触发完整 copy-up。

---

## 4. 重命名与 whiteout

### 4.1 whiteout 表示

删除 lower 中的文件时不能真的删 lower，于是在 upper 放一个 whiteout，使 lookup 在该层停止：

- 传统形式：char device，major/minor 均为 0（`ovl_whiteout()` 创建）；
- xattr 形式：`trusted.overlay.whiteout`（userxattr 时用 `user.overlay.whiteout`），用于不支持设备节点的底层；
- 目录整体屏蔽：upper 目录设置 `trusted.overlay.opaque=y`。

`ovl_is_whiteout()` 判断 dentry 是否为 whiteout；`ovl_path_is_whiteout()` 用于路径级别检查。`ovl_fs.whiteout` 缓存一个共享 whiteout dentry，配合 `whiteout_lock` 减少重复创建。

### 4.2 rename

`ovl_rename()`（`dir.c`，经 `ovl_rename_start()` / `ovl_rename_upper()`）处理跨层重命名：

1. 源在 lower、目标在 upper：先 copy-up 源，再在 upper rename；
2. 源在 upper：直接 rename；
3. 源是 lower 且只做“删除式 rename”：在源位置创建 whiteout，再在目标位置创建链接/副本；
4. 目标已存在时按 VFS 语义覆盖，并在 upper 放 whiteout 屏蔽被覆盖的 lower 项；
5. 目录 rename 需要检查 `redirect_dir`，必要时写 redirect xattr 并复制目录元数据。

底层 `ovl_do_rename()` 封装 `vfs_rename()`，可带 `RENAME_WHITEOUT` 由底层文件系统原子创建 whiteout（ext4/xfs 支持时）。

### 4.3 一致性与限制

- overlayfs 自身不维护日志，一致性依赖 upper 文件系统；同一文件不能同时被多个 overlay 挂载以不同 upper 写入（无跨挂载锁）；
- `volatile` 挂载跳过 fsync，适合容器临时层；`index=on` 用 origin 文件句柄把 copy-up 后的 upper 与 lower 关联，支持硬链接与 NFS export。

---

## 5. netfs 库

### 5.1 请求模型

`include/linux/netfs.h`：

`netfs_io_request` 记录 `work`（结果收集）、`inode`/`mapping`/`iocb`、`cache_resources`、两条 `io_streams[2]`、`netfs_priv`/`netfs_priv2`、区间 `start`/`len`/`i_size`、进度 `submitted`/`transferred`/`issued_to`/`collected_to`/`cleaned_to`、`error`、切分粒度 `rsize`/`wsize`、`origin`（READAHEAD/READPAGE/WRITEBACK）与 `flags`。

`netfs_io_subrequest` 记录 `rreq`、`io_iter`、`start`/`len`/`transferred`、`error`、`retry_count`、`source`（pagecache / fscache / 远端 / 洞）、`stream_nr` 与标志位。一个 `rreq` 覆盖一段文件区间，按 `rsize`/`wsize` 切分成多个 `subreq`；`io_streams[2]` 支持读写并行。

### 5.2 文件系统回调

`struct netfs_request_ops`：

| 回调 | 作用 |
|------|------|
| `init_request` / `free_request` | 请求生命周期 |
| `expand_readahead` | 调整预读窗口 |
| `prepare_read` / `issue_read` | 准备并下发读子请求 |
| `is_still_valid` | 判断缓存内容是否仍有效 |
| `check_write_begin` | 写前检查（如 Ceph 的 caps） |
| `begin_writeback` / `prepare_write` / `issue_write` | 回写下发 |
| `retry_request` | 子请求失败后的重试策略 |
| `invalidate_cache` | 失效缓存 |
| `update_i_size` / `post_modify` | 更新 i_size 与后续处理 |

### 5.3 入口

- `netfs_readahead()`（`buffered_read.c`）：预读，按 `expand_readahead` 扩展窗口；
- `netfs_read_folio()`：同步读单个 folio，缺页路径；
- `netfs_write_begin()`：缓冲写准备，复用 pagecache；
- `netfs_writepages()`（`write_issue.c`）：回写脏页，按 `wsize` 切分并调用 `issue_write`；
- `netfs_unbuffered_read_iter()` / `netfs_unbuffered_write_iter()`：直接 IO，绕过 pagecache。

子请求完成后由 `netfs_write_collect` / `read_collect` 汇总，更新 `transferred`，最后一个子请求结束时唤醒 `rreq->waitq`。

---

## 6. NFS

### 6.1 nfs_inode

`include/linux/nfs_fs.h`：

```c
struct nfs_inode {
    struct nfs_fh fh;            /* 文件句柄 */
    unsigned long flags;
    unsigned long cache_validity;
    struct timespec64 btime;
    unsigned long read_cache_jiffies;
    unsigned long attrtimeo;     /* 属性缓存有效期 */
    unsigned long attrtimeo_timestamp;
    unsigned long attr_gencount;
    struct rb_root access_cache;
    struct list_head access_cache_entry_lru, access_cache_inode_lru;
    union {
        struct { unsigned long cache_change_attribute;
                 __be32 cookieverf[NFS_DIR_VERIFIER_SIZE];
                 struct rw_semaphore rmdir_sem; };
        struct { atomic_long_t nrequests, redirtied_pages;
                 struct nfs_mds_commit_info commit_info; };
    };
};
```

属性缓存由 `attrtimeo` 控制：命中则直接用缓存，超时则发 GETATTR。`cache_validity` 位图标记哪些字段失效。

### 6.2 读写路径

- `nfs_file_read()`：直接 IO 走 `nfs_file_direct_read()`；否则 `nfs_start_io_read()` → `nfs_revalidate_mapping()` → `generic_file_read_iter()`。缓冲读最终到 `nfs_read_folio()`。
- `nfs_read_folio()`：先 `nfs_wb_folio()` 刷同文件未完成写，再尝试 `nfs_netfs_read_folio()`（仅启用 fscache 且存在 cookie 时），否则 `nfs_do_read_folio()` 走普通 RPC。
- `nfs_file_write()`：`generic_write_checks()` + `generic_perform_write()`；`NFS_MOUNT_WRITE_EAGER` 时立即 `filemap_fdatawrite_range()` 回写。

### 6.3 delegation / lease

NFSv4 用 delegation 表示服务器授予客户端的缓存权限，等价于“租约”：

`nfs_delegation` 记录 `hash`/`super_list` 链表、`cred`、`inode`、`stateid`、`type`（READ/WRITE）、`pagemod_limit`、`change_attr`、`flags`（NEED_RECLAIM、RETURN_IF_CLOSED 等）、`refcount` 与 `rcu`。

`nfs_inode_set_delegation()` 在收到 OPEN/OPEN_CONFIRM 的 delegation 时建立，`nfs_delegation_claim_opens()` 把已打开的 state 绑定到 delegation。客户端可在 delegation 有效期内本地缓存读写而不通知服务器；服务器需要回收时通过 CB_RECALL 回调，客户端 `nfs4_inode_return_delegation()` 刷回 dirty 数据并返回 stateid。`nfs_setlease()` 提供 POSIX 文件锁语义，与 delegation 相互独立。

---

## 7. 9p

### 7.1 客户端与 fid

`include/net/9p/client.h`：

```c
struct p9_client {
    spinlock_t lock;
    unsigned int msize;            /* 协商的最大消息长度 */
    unsigned char proto_version;
    struct p9_trans_module *trans_mod;
    enum p9_trans_status status;
    void *trans;
    struct kmem_cache *fcall_cache;
    struct idr fids;               /* fid 表 */
    struct idr reqs;               /* tag 表 */
    char name[__NEW_UTS_LEN + 1];
};
```

每个打开文件对应一个 `p9_fid`，由 `v9fs_fid_clone()`/`v9fs_fid_add()` 管理，`idr` 用 fid 号索引。传输层可选 tcp、unix、virtio、fd；`p9_client_rpc(c, type, fmt, ...)` 构造 `P9_T*` 请求，分配 tag，发送并等待 `P9_R*` 应答。

### 7.2 读写

`v9fs_file_read_iter()`（`fs/9p/vfs_file.c`）：

- `fid->mode & P9L_DIRECT` 时走 `netfs_unbuffered_read_iter()`；
- 否则 `netfs_file_read_iter()`，即 9p 已把缓冲读写交给 netfs 库。

底层实际收发在 `p9_client_read()` / `p9_client_write()`：按 `msize` 切分 iov，循环发 `P9_TREAD`/`P9_TWRITE`，`p9_client_write_subreq()` 直接作为 netfs 的 `issue_write` 回调。`cache=loose|fscache|mmap` 控制是否启用页缓存与持久缓存。

---

## 8. Ceph

### 8.1 ceph_inode_info 与 caps

`struct ceph_inode_info`（`fs/ceph/super.h`）内嵌 `struct netfs_inode netfs`，因此 Ceph 的缓冲 IO 直接复用 netfs：

```c
struct ceph_inode_info {
    struct netfs_inode netfs;   /* netfs 上下文 + VFS inode */
    struct ceph_vino i_vino;    /* ino + snap */
    spinlock_t i_ceph_lock;
    u64 i_version, i_inline_version;
    unsigned long i_ceph_flags;
    atomic64_t i_release_count, i_ordered_count;
    atomic64_t i_complete_seq[2];
    struct ceph_file_layout i_layout, i_cached_layout;
    char *i_symlink;
    u64 i_rbytes, i_rfiles, i_rsubdirs, i_rsnaps;
    u64 i_max_bytes, i_max_files;
    u64 i_subvolume_id;
};
```

Ceph 的“租约”是 caps（capabilities）。`struct ceph_cap`（`super.h`）记录 `issued`/`implemented`（MDS 已授予/实现的能力）、`mds_wanted`、`cap_id`、所属 `session` 与 `ci_node`（per-inode cap 树）。`ceph_get_caps(filp, CEPH_CAP_FILE_RD, want, ...)` 在读写前申请能力，MDS 通过 cap message 授予或回收。

### 8.2 读写路径

`ceph_read_iter()`（`fs/ceph/file.c`）：

1. `ceph_start_io_read()` / `ceph_start_io_direct()` 做并发控制；
2. 计算 `want`：非同步读且非 direct 时请求 `CEPH_CAP_FILE_CACHE`，lazy IO 请求 `CEPH_CAP_FILE_LAZYIO`；
3. `ceph_get_caps()` 获取 caps；
4. 有 cache cap 且非 direct/同步时走缓冲读（netfs + pagecache）；否则 `ceph_sync_read()` 直接发 OSD 请求。

`ceph_write_iter()` 对称：申请 `CEPH_CAP_FILE_WR`，缓冲写走 netfs，`O_DIRECT`/同步写走 `ceph_sync_write()`。OSD 请求由 `ceph_osdc_new_request()` 构造、`ceph_osdc_start_request()` 下发，`struct ceph_osd_client` 维护 OSD 会话、请求队列与超时重发。

### 8.3 与 netfs 的关系

Ceph 的 `address_space_operations`（`addr.c`）把 readahead/read_folio/writepages 交给 netfs，`netfs_priv` 存放 OSD 请求上下文；`netfs_request_ops` 的 `check_write_begin`、`issue_read`、`issue_write` 由 Ceph 实现。这样 pagecache、fscache 与直接 OSD 路径共用一套请求拆分/重试逻辑。

---

## 9. 当前配置

| 配置项 | 值 | 影响 |
|--------|----|------|
| `CONFIG_OVERLAY_FS` | `is not set` | `fs/overlayfs/` 不编译，无 `overlay` 类型 |
| `CONFIG_NETWORK_FILESYSTEMS` | `is not set` | NFS/9P/Ceph 的 Kconfig 不出现 |
| `CONFIG_NFS_FS` / `NFS_V4` | 不可见 | 无 NFS 客户端、无 delegation |
| `CONFIG_9P_FS` | 不可见 | 无 9p 客户端 |
| `CONFIG_CEPH_LIB` / `CEPH_FS` | `is not set` | 无 Ceph 客户端 |
| `CONFIG_NETFS_SUPPORT` | 不可见 | `fs/netfs/` 不编译 |

因此本内核无法挂载 overlayfs 或任何网络文件系统。源码中的 copy-up、whiteout、netfs 请求模型与 NFS delegation/Ceph caps 逻辑仍可按文件阅读；要验证需打开 `CONFIG_OVERLAY_FS`、`CONFIG_NETWORK_FILESYSTEMS` 及具体协议。

---

## 关键文件清单

| 文件 | 内容 |
|------|------|
| `fs/overlayfs/ovl_entry.h` | `ovl_fs`、`ovl_layer`、`ovl_entry`、`ovl_inode` |
| `fs/overlayfs/super.c` | 挂载/卸载、层解析、`ovl_fill_super` |
| `fs/overlayfs/namei.c` | `ovl_lookup`、whiteout 判定、redirect/index |
| `fs/overlayfs/inode.c` | inode 操作、属性与 xattr 转发 |
| `fs/overlayfs/copy_up.c` | `ovl_copy_up`、`ovl_copy_up_one`、`ovl_copy_up_data` |
| `fs/overlayfs/dir.c` | `ovl_whiteout`、`ovl_rename`、创建/删除目录项 |
| `fs/overlayfs/file.c` | overlay 的 read/write/mmap 转发 |
| `fs/overlayfs/readdir.c` | 目录合并与 `ovl_dir_cache` |
| `include/linux/netfs.h` | `netfs_io_request`、`netfs_io_subrequest`、`netfs_request_ops` |
| `fs/netfs/buffered_read.c` | `netfs_readahead`、`netfs_read_folio`、`netfs_write_begin` |
| `fs/netfs/write_issue.c` | `netfs_writepages`、写子请求下发 |
| `fs/netfs/read_collect.c` / `write_collect.c` | 子请求结果汇总 |
| `include/linux/nfs_fs.h` | `nfs_inode`、NFS inode 标志 |
| `fs/nfs/file.c` | `nfs_file_read`、`nfs_file_write`、`nfs_read_folio` |
| `fs/nfs/read.c` / `write.c` | RPC 读写与回写 |
| `fs/nfs/delegation.c` | `nfs_inode_set_delegation`、delegation 回收 |
| `include/net/9p/client.h` | `p9_client`、`p9_fid`、`p9_client_read/write` |
| `net/9p/client.c` | `p9_client_rpc`、请求 tag 与传输 |
| `fs/9p/vfs_file.c` | `v9fs_file_read_iter`/`write_iter`（netfs 接入） |
| `fs/ceph/super.h` | `ceph_inode_info`、`ceph_cap` |
| `fs/ceph/file.c` | `ceph_read_iter`、`ceph_write_iter`、caps 申请 |
| `fs/ceph/addr.c` | address_space 操作、netfs 回调 |
| `net/ceph/osd_client.c` | `ceph_osdc_new_request`、OSD 请求队列 |
