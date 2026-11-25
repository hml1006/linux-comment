# Device Mapper 与 MD RAID

## 1. 概述与分层

Linux 有两套块设备虚拟化框架，都位于 `drivers/md/`：

- **Device Mapper (DM)**：通用映射框架。用户态通过 ioctl 下发“表”，表由若干 target 组成，每个 target 负责一段逻辑扇区到物理设备的映射。LVM、dm-crypt、dm-verity、dm-thin、dm-integrity、dm-multipath 都构建在它之上。
- **MD (Multiple Devices)**：软件 RAID。`mddev` 管理一组成员盘，personality（raid0/raid1/raid5/raid6/raid10/linear）实现条带、镜像与校验，并负责 resync、recovery、reshape。

```
用户态 dmsetup / LVM / cryptsetup
   | ioctl: DM_DEV_CREATE / DM_TABLE_LOAD / DM_DEV_SUSPEND
dm-ioctl.c（控制面）
   |
dm.c: mapped_device / dm_io / bio 克隆与重映射（数据面）
   |
dm-table.c: dm_table / dm_target
   |
target: dm-linear / dm-crypt / dm-verity / dm-thin / ...
   |
底层 block_device
```

当前 `.config` 中 `# CONFIG_MD is not set`，因此 `CONFIG_BLK_DEV_MD`、`CONFIG_BLK_DEV_DM` 及其 target 全部不会编译。下文描述源码机制，本内核中不可用（不会出现 `/dev/mapper`、`/dev/md*`）。

---

## 2. Device Mapper 数据结构

以下结构体仅列关键字段，完整定义以当前源码为准。

### 2.1 mapped_device

`drivers/md/dm-core.h`。一个 DM 设备对应一个 `mapped_device`，拥有自己的 `gendisk` 与 `request_queue`：

```c
struct mapped_device {
    struct mutex suspend_lock;
    struct list_head table_devices;
    void __rcu *map;          /* 当前 dm_table，经 SRCU 访问 */
    unsigned long flags;      /* DMF_BLOCK_IO_FOR_SUSPEND 等 */
    enum dm_queue_mode type;  /* DM_TYPE_BIO_BASED / REQUEST_BASED */
    struct request_queue *queue;
    atomic_t holders, open_count;
    struct dm_target *immutable_target;
    char name[16];
    struct gendisk *disk;
    struct dax_device *dax_dev;
    unsigned long __percpu *pending_io;
    struct workqueue_struct *wq;
    struct bio_list deferred;      /* 挂起期间暂存的 bio */
    struct work_struct requeue_work;
    struct dm_io *requeue_list;
    wait_queue_head_t eventq;
    struct dm_stats stats;
};
```

`dm_get_live_table(md, &srcu_idx)` 在 SRCU 读临界区取出 `map`；表切换由 `dm_swap_table()` 在 `synchronize_srcu()` 后替换指针。

### 2.2 dm_table

`drivers/md/dm-core.h`。表内 target 按逻辑扇区排序，`targets[]` 存 `dm_target`，`highs[]` 存每个 target 的结束扇区；`index[DM_TABLE_MAX_DEPTH]` / `counts[]` 构成多层索引，供 `dm_table_find_target(t, sector)` 逐层二分定位。`flush_bypasses_map` 允许 linear/stripe 跳过 map 直接下刷；`mempools` 指向 bio 克隆的内存池；`devices` 列出表引用的所有 `dm_dev`；`singleton` 标记单 target 表。

`dm_table_find_target(t, sector)` 返回覆盖该扇区的 `dm_target`，是每次 IO 映射的第一步。

### 2.3 dm_target 与 target_type

`include/linux/device-mapper.h`：

```c
struct dm_target {
    struct dm_table *table;
    struct target_type *type;
    sector_t begin;      /* DM 设备内起始扇区 */
    sector_t len;        /* 映射长度（扇区） */
    uint32_t max_io_len; /* 单次下发的最大 IO，0 表示不限 */
    unsigned int num_flush_bios, num_discard_bios;
    unsigned int num_secure_erase_bios, num_write_zeroes_bios;
    unsigned int per_io_data_size;  /* 每个 bio 的额外私有区 */
    void *private;                  /* ctr() 分配，dtr() 释放 */
};

struct target_type {
    uint64_t features;   /* DM_TARGET_SINGLETON / IMMUTABLE / WILDCARD */
    const char *name;
    struct module *module;
    unsigned int version[3];
    dm_ctr_fn ctr;       /* 构造：解析参数、打开设备 */
    dm_dtr_fn dtr;
    dm_map_fn map;       /* bio-based 映射回调 */
    dm_endio_fn end_io;  /* 克隆 bio 完成回调，可返回 REQUEUE */
    dm_clone_and_map_request_fn clone_and_map_rq; /* request-based */
    dm_release_clone_request_fn release_clone_rq;
    dm_presuspend_fn presuspend;
    dm_postsuspend_fn postsuspend;
    dm_preresume_fn preresume;
    dm_resume_fn resume;
    dm_status_fn status;
    dm_message_fn message;
    dm_iterate_devices_fn iterate_devices;
    dm_io_hints_fn io_hints;
};
```

关键特性位：`DM_TARGET_SINGLETON`（表中只能有一个，如 thin pool）、`DM_TARGET_IMMUTABLE`（不能与其他类型混用）、`DM_TARGET_WILDCARD`（可替换任意 target）。

### 2.4 dm_io 与 dm_target_io

一个上层 bio 可能跨多个 target：`dm_io` 是“总账”，记录 `orig_bio`、`sector_offset`、`sectors`、`io_count`（未完成克隆数）、`status`、`start_time` 与 `md`；每个克隆 bio 对应一个 `dm_target_io`，记录 `io`、`ti`、`target_bio_nr`、`old_sector`，并在结构体尾部内嵌 `struct bio clone`。

`clone_to_tio(bio)` 由 bio 反推 `dm_target_io`；`dm_per_bio_data(bio, ti->per_io_data_size)` 返回 target 私有区。`struct dm_dev { bdev, bdev_file, dax_dev, mode, name }` 表示 target 打开的底层设备，由 `dm_get_device()` / `dm_put_device()` 管理。

---

## 3. DM bio 重映射流程

### 3.1 入口

DM 的 gendisk 使用 bio-based 队列，`dm_submit_bio()`（`drivers/md/dm.c`）：

```
dm_submit_bio(bio)
  ├─ md = bio->bi_bdev->bd_disk->private_data
  ├─ map = dm_get_live_table(md, &srcu_idx)
  ├─ 若 DMF_BLOCK_IO_FOR_SUSPEND:
  │    REQ_NOWAIT -> bio_wouldblock_error
  │    REQ_RAHEAD -> bio_io_error
  │    否则 queue_io(md, bio) 挂到 md->deferred
  └─ dm_split_and_process_bio(md, map, bio)
```

`dm_split_and_process_bio()` 的要点：

1. `is_abnormal_io()` 判断 discard / write zeroes / secure erase，异常 IO 先 `bio_split_to_limits()`；
2. `alloc_io(md, bio, GFP_NOIO|GFP_NOWAIT)` 分配 `dm_io`；
3. `init_clone_info()` 记录当前 target 与剩余扇区；
4. 若 `REQ_PREFLUSH` 且 `map->flush_bypasses_map`（linear/stripe），flush 可直接下发到底层；
5. 循环 `__split_and_process_bio()`，直到原始 bio 全部映射完。

### 3.2 单次映射 __map_bio

```c
static void __map_bio(struct bio *clone)
{
    struct dm_target_io *tio = clone_to_tio(clone);
    struct dm_target *ti = tio->ti;
    struct dm_io *io = tio->io;
    int r;

    clone->bi_end_io = clone_endio;
    tio->old_sector = clone->bi_iter.bi_sector;

    if (likely(ti->type->map == linear_map))
        r = linear_map(ti, clone);          /* 快速路径 */
    else if (ti->type->map == stripe_map)
        r = stripe_map(ti, clone);
    else
        r = ti->type->map(ti, clone);

    switch (r) {
    case DM_MAPIO_SUBMITTED: break;  /* target 自己接管，如 dm-thin */
    case DM_MAPIO_REMAPPED: dm_submit_bio_remap(clone, NULL); break;
    case DM_MAPIO_KILL:     dm_io_dec_pending(io, BLK_STS_IOERR); break;
    case DM_MAPIO_REQUEUE:  dm_io_dec_pending(io, BLK_STS_DM_REQUEUE); break;
    }
}
```

`linear_map` 只做 `bio_set_dev()` 与扇区偏移，被内联为快速路径；`stripe_map` 处理 dm-stripe 的条带计算。

### 3.3 完成 clone_endio

```c
static void clone_endio(struct bio *bio)
{
    blk_status_t error = bio->bi_status;
    struct dm_target_io *tio = clone_to_tio(bio);
    struct dm_target *ti = tio->ti;
    dm_endio_fn endio = ti ? ti->type->end_io : NULL;
    struct dm_io *io = tio->io;

    if (endio) {
        int r = endio(ti, bio, &error);
        if (r == DM_ENDIO_REQUEUE)
            error = BLK_STS_DM_REQUEUE;
        else if (r == DM_ENDIO_INCOMPLETE)
            return;                 /* target 稍后自行完成 */
    }
    free_tio(bio);
    dm_io_dec_pending(io, error);
}
```

`dm_io_dec_pending()` 递减 `io_count`；归零时把 `orig_bio` 的 `bi_status` 汇总后 `bio_endio(orig_bio)`。`BLK_STS_DM_REQUEUE` 触发 `dm_requeue_work` 重新排队，避免中断上下文递归。`dm_start_io_acct()` / `dm_end_io_acct()` 以 `dm_stats` 记录延迟与扇区数。

---

## 4. DM 控制面

### 4.1 ioctl 分发

用户态通过 `/dev/mapper/control` 发 ioctl，`ctl_ioctl()`（`drivers/md/dm-ioctl.c`）按 `param->cmd` 分发：

| 命令 | 处理函数 | 作用 |
|------|----------|------|
| `DM_VERSION` | `version()` | 查询协议版本 |
| `DM_DEV_CREATE` | `dev_create()` | 创建 `mapped_device` 与 gendisk |
| `DM_DEV_REMOVE` | `dev_remove()` | 销毁设备 |
| `DM_TABLE_LOAD` | `table_load()` | 解析表并构建 `dm_table` |
| `DM_DEV_SUSPEND` | `dev_suspend()` | 挂起/恢复 IO |
| `DM_DEV_STATUS` | `__dev_status()` | 返回设备状态与事件号 |
| `DM_TABLE_STATUS` | `table_status()` | 返回各 target 的 status 字符串 |
| `DM_DEV_RENAME` | `dev_rename()` | 重命名 `/dev/mapper/name` |
| `DM_TARGET_MSG` | `target_message()` | 向 target 发私有消息 |

### 4.2 表加载

```
table_load
  ├─ 复制用户态参数到内核缓冲
  ├─ dm_table_create(&t, flags, target_count, md)
  ├─ 逐个 target:
  │    ├─ dm_table_add_target(t, type, start, len, params)
  │    │    ├─ dm_get_target_type(type)  查找注册的 target_type
  │    │    └─ ti->type->ctr(ti, params) 解析参数、打开设备
  │    └─ 失败则 dm_table_destroy(t)
  ├─ dm_table_complete(t)   /* 排序、建索引树、合并 queue_limits */
  └─ 暂存为 pending 表，等待 DM_DEV_SUSPEND/RESUME 生效
```

`dm_table_complete()` 调用每个 target 的 `iterate_devices` 收集底层队列限制，合并成 DM 设备的 `queue_limits`（最大扇区、discard 粒度、zoned 模型等）。

### 4.3 表切换

`do_resume()` 中：`dm_swap_table(md, new_map)` 在 suspend 状态下替换 `md->map`，内部 `__bind(md, new_map)` 绑定 mempools 并更新 queue limits，随后 `rcu_assign_pointer(md->map, new_map)`；`synchronize_srcu()` 确保没有旧表读者后旧表才能释放。挂起期间新到的 bio 进入 `md->deferred`，恢复时按序重放。

---

## 5. 典型 target

### 5.1 dm-crypt

`drivers/md/dm-crypt.c`。`crypt_config` 保存底层设备 `dev`、起始偏移 `start`、`io_queue`/`crypt_queue` 两条工作队列、`write_tree`/`write_thread`（写排序）、`cipher_string`/`key_string`、`iv_gen_ops`/`iv_offset`/`iv_size`、`sector_size`，以及每 CPU 的 `cipher_tfm`（skcipher 或 aead）与 `tfms_count`。`REQ_PREFLUSH` 与 `REQ_OP_DISCARD` 直接映射到底层（不加密）；普通 IO 分配 `dm_crypt_io` 并按 `max_sectors` 切分；读走 `kcryptd_queue_read()` 先读密文再解密，写走 `kcryptd_queue_crypt()` 先加密再写；加解密在 `crypt_queue` 异步完成，`kcryptd_async_done()` 收尾。`write_tree` + `write_thread` 按扇区排序串行提交，解决并发写同一扇区的 IV 顺序问题。

### 5.2 dm-verity

`drivers/md/dm-verity-target.c` / `dm-verity.h`。`dm_verity` 保存 `data_dev`/`hash_dev`、哈希块缓存 `bufio`、`shash_tfm`、根摘要 `root_digest`、`salt`、`zero_digest`、`hash_start`/`hash_end`/`data_blocks`、数据与哈希块位数、树层级 `levels`、校验模式 `mode`/`error_mode`、`verify_wq`、每层起始块 `hash_level_block[]` 与已验证位图 `validated_blocks`。只读完整性校验，用 Merkle 哈希树验证每个数据块。

`verity_map()` 把 bio 重定向到 `data_dev` 并做对齐/范围检查，写请求直接 `DM_MAPIO_KILL`，设置 `verity_end_io`。完成时 `verity_verify_io()` 逐块计算哈希，沿 `hash_level_block[]` 向上校验直到根摘要；失败按 `mode` 处理（`EIO` / `PANIC` / `RESTART` / `IGNORE`），`validated_blocks` 位图避免重复校验。

### 5.3 dm-thin

`drivers/md/dm-thin.c`。thin provisioning 由 pool 与 thin 两个 target 组成：

```c
struct pool {
    struct dm_target *ti;
    struct mapped_device *pool_md;
    struct block_device *data_dev, *md_dev;
    struct dm_pool_metadata *pmd;      /* 元数据（dm-thin-metadata） */
    dm_block_t low_water_blocks;
    uint32_t sectors_per_block;
    struct dm_bio_prison *prison;      /* 按块加锁 */
    struct dm_kcopyd_client *copier;   /* copy-on-write 数据拷贝 */
    struct work_struct worker;
    struct bio_list deferred_flush_bios;
    struct list_head prepared_mappings, prepared_discards;
    struct list_head active_thins;
    struct dm_deferred_set *shared_read_ds, *all_io_ds;
};

struct thin_c {
    struct dm_dev *pool_dev, *origin_dev;
    dm_thin_id dev_id;
    struct pool *pool;
    struct dm_thin_device *td;
    struct mapped_device *thin_md;
    struct bio_list deferred_bio_list, retry_on_resume_list;
    struct rb_root sort_bio_list;
};
```
- `pool_map()` 是 singleton 快速路径，直接把 bio 指向数据设备；
- `thin_map()` → `thin_bio_map()` → `dm_thin_find_block()` 查询映射；未映射块触发 `alloc_data_block()`，经 `dm_kcopyd` 从 origin 拷贝，实现 copy-on-write；
- 元数据通过 `dm_pool_commit_metadata()` 写入 `md_dev`，由 block manager 保证事务性；
- `prison` 保证同一数据块同时只有一个分配者。

---

## 6. MD 架构

### 6.1 mddev 与 md_rdev

`drivers/md/md.h`：

```c
struct mddev {
    void *private;                 /* personality 私有结构，如 r5conf */
    struct md_personality *pers;
    dev_t unit;
    int md_minor;
    struct list_head disks;        /* 所有 md_rdev */
    unsigned long flags;           /* MD_BROKEN / MD_HAS_SUPERBLOCK ... */
    unsigned long sb_flags;
    int suspended;
    struct mutex suspend_mutex;
    struct percpu_ref active_io;   /* 在途请求引用 */
    int ro;
    struct gendisk *gendisk;
    struct gendisk *dm_gendisk;    /* dm-raid 使用 */
    int major_version, minor_version, patch_version;
    int persistent, external;
    char metadata_type[17];
    int chunk_sectors;
    int level, layout;
    int raid_disks, max_disks;
    sector_t dev_sectors, array_sectors;
    unsigned int logical_block_size;
    __u64 events;                  /* 事件计数，成员盘一致性 */
    char uuid[16];
    sector_t reshape_position;     /* MaxSector 表示不在 reshape */
    int delta_disks, new_level, new_layout;
    int new_chunk_sectors;
    int reshape_backwards;
    struct md_thread __rcu *thread;       /* 管理线程 */
    struct md_thread __rcu *sync_thread;  /* resync/recover/reshape */
    enum sync_action last_sync_action;
    sector_t curr_resync, curr_resync_completed;
    atomic64_t resync_mismatches;
};
```
`md_rdev` 描述成员盘：`bdev`、`dev_sectors`、`data_offset`、`sb_page`、`recovery_offset`、`raid_disk` 与 flags（`Faulty`、`In_sync`、`WriteMostly` 等），由 `mddev->disks` 串起。

### 6.2 personality

```c
struct md_personality {
    struct md_submodule_head head;
    bool (*make_request)(struct mddev *mddev, struct bio *bio);
    int (*run)(struct mddev *mddev);
    int (*start)(struct mddev *mddev);
    void (*free)(struct mddev *mddev, void *priv);
    void (*status)(struct seq_file *seq, struct mddev *mddev);
    void (*error_handler)(struct mddev *mddev, struct md_rdev *rdev);
    int (*hot_add_disk)(struct mddev *mddev, struct md_rdev *rdev);
    int (*hot_remove_disk)(struct mddev *mddev, struct md_rdev *rdev);
    int (*spare_active)(struct mddev *mddev);
    sector_t (*sync_request)(struct mddev *mddev, sector_t sector_nr,
                             sector_t max_sector, int *skipped);
    int (*resize)(struct mddev *mddev, sector_t sectors);
    int (*check_reshape)(struct mddev *mddev);
    int (*start_reshape)(struct mddev *mddev);
    void (*finish_reshape)(struct mddev *mddev);
    void (*quiesce)(struct mddev *mddev, int quiesce);
    void *(*takeover)(struct mddev *mddev);
};
```
`register_md_personality()` 把 personality 挂到全局链表；`md_run()` 按 `mddev->level` / `clevel` 选择并调用 `pers->run()`。`raid5.c` 中 `raid5_personality`、`raid6_personality`、`raid4_personality` 共用一套实现，仅参数不同。

### 6.3 管理线程

`struct md_thread` 包含 `run` 回调、`mddev`、等待队列 `wqueue`、`tsk`、`timeout` 与 `private`。`mddev->thread` 处理元数据更新、错误与状态变更；`mddev->sync_thread` 执行 resync / recovery / reshape，入口为 `sync_request()`。`md_wakeup_thread()` 置位并唤醒，`timeout` 支持无事件时的周期性检查。

---

## 7. MD 请求路径与恢复

### 7.1 提交

```
md_submit_bio(bio)
  ├─ mddev = bio->bi_bdev->bd_disk->private_data
  ├─ MD_BROKEN 且写 -> bio_io_error
  ├─ bio_split_to_limits(bio)
  ├─ 只读设备收到写 -> BLK_STS_IOERR
  └─ md_handle_request(mddev, bio)
       ├─ md_cloned_bio() 已持有引用则直接 percpu_ref_get
       ├─ 否则 is_suspended() 检查，REQ_NOWAIT 则 wouldblock，
       │    普通 bio 在 sb_wait 上等待 suspend 区间结束
       ├─ percpu_ref_tryget_live(&mddev->active_io)
       └─ mddev->pers->make_request(mddev, bio)
```

`make_request` 返回 false 表示稍后重试（dm-raid suspend 场景），true 表示已接管。

### 7.2 raid5 的 make_request

`raid5_make_request()`（`drivers/md/raid5.c`）：

1. `REQ_PREFLUSH` 先走 journal 设备（r5l）或 `md_flush_request()`；
2. `md_write_start()` 标记阵列进入写模式；
3. 非降级且不在 reshape 时，读请求走 `chunk_aligned_read()` 快速路径；
4. `REQ_OP_DISCARD` 走 `make_discard_request()`；
5. 普通 IO 按 stripe 切分，`make_stripe_request()` 获取/分配 stripe；读缺失盘触发 reconstruct-read，写时计算 P/Q 校验并下发；
6. `md_write_end()` 递减写计数，必要时唤醒 `mddev->thread` 更新超级块。

### 7.3 写意图与超级块

`md_write_start()`：`percpu_ref_get(&mddev->writes_pending)` 记录在途写；若 `mddev->in_sync`，清除并置 `MD_SB_CHANGE_CLEAN | MD_SB_CHANGE_PENDING`，唤醒 `mddev->thread` 把超级块标记 dirty；有超级块时在 `sb_wait` 上等待变更落盘，保证“先 dirty 后写数据”。`md_write_end()` 在最后一个写完成时把阵列重新标记 clean（`set_in_sync()`）并更新 `events`。

### 7.4 resync / recovery / reshape

- **resync**：`sync_request()` 从 `curr_resync` 起逐条带读取并校验/重算校验块，进度写入 `curr_resync_completed` 与超级块 `resync_offset`。
- **recovery**：替换成员盘后，仅重建该盘 `recovery_offset` 之后的区域。
- **reshape**：改变级别、磁盘数或 chunk 大小时，`mddev->reshape_position` 记录进度；raid5 的 `reshape_request()` 按 `reshape_backwards` 迁移数据，`conf->reshape_progress` 与 `reshape_safe` 控制安全边界，完成后 `finish_reshape()` 清零（置 `MaxSector`）。
- **bitmap**：`md-bitmap.c` 的写意图位图记录脏区域，`md_bitmap_start_sync()` / `md_bitmap_end_sync()` / `md_bitmap_dirty_bits()` 支持增量 resync，避免整盘重建。

### 7.5 错误处理

成员盘 IO 失败时 personality 调用 `md_error()`：置 `Faulty`、清 `In_sync`、增加 `degraded` 计数、唤醒 `mddev->thread`。raid1/raid10 把读重定向到镜像副本；raid5/6 在降级状态下用剩余盘重建。降级超过容忍度时阵列进入 `MD_BROKEN`，写请求被拒绝。

---

## 8. 当前配置状态

| 配置项 | 值 | 影响 |
|--------|----|------|
| `CONFIG_MD` | `is not set` | `drivers/md/` 整体不编译，无 `md_mod`、`dm_mod` |
| `CONFIG_BLK_DEV_MD` | 不可见 | 无 `/dev/md*`、`/sys/block/md*` |
| `CONFIG_BLK_DEV_DM` | 不可见 | 无 `/dev/mapper/control`，dmsetup/LVM/cryptsetup 不可用 |
| `CONFIG_DM_CRYPT` / `DM_VERITY` / `DM_THIN_PROVISIONING` | 不可见 | target 仅存在于源码 |

因此本内核无法创建 DM/MD 设备；上述路径只能作为源码阅读对象。要验证需打开 `CONFIG_MD`、`CONFIG_BLK_DEV_DM` 及具体 target 并重新构建。

---

## 关键文件清单

| 文件 | 内容 |
|------|------|
| `include/linux/device-mapper.h` | `dm_target`、`target_type`、`dm_dev`、`DM_MAPIO_*` |
| `drivers/md/dm-core.h` | `mapped_device`、`dm_table`、`dm_io`、`dm_target_io` |
| `drivers/md/dm.c` | `dm_submit_bio`、`dm_split_and_process_bio`、`__map_bio`、`clone_endio`、`dm_swap_table` |
| `drivers/md/dm-table.c` | `dm_table_create`、`dm_table_add_target`、`dm_table_complete`、`dm_table_find_target` |
| `drivers/md/dm-ioctl.c` | `ctl_ioctl`、`table_load`、`do_resume`、`dev_suspend`、`__dev_status` |
| `drivers/md/dm-linear.c` | `linear_map` 快速路径；`dm-stripe.c` 的 `stripe_map` |
| `drivers/md/dm-crypt.c` | `crypt_config`、`crypt_map`、kcryptd 加解密队列 |
| `drivers/md/dm-verity-target.c` | `verity_map`、`verity_end_io`、哈希树校验 |
| `drivers/md/dm-verity.h` | `dm_verity` 结构 |
| `drivers/md/dm-thin.c` | `pool`、`thin_c`、`thin_map`、copy-on-write |
| `drivers/md/dm-thin-metadata.c` | thin 元数据事务（block manager） |
| `drivers/md/dm-bio-prison-v1.c` / `dm-bio-prison-v2.c` | 按块加锁，避免并发分配冲突 |
| `drivers/md/md.h` | `mddev`、`md_rdev`、`md_personality`、`md_thread` |
| `drivers/md/md.c` | `md_submit_bio`、`md_handle_request`、`md_write_start`、`do_md_run`、`md_run` |
| `drivers/md/md-bitmap.c` | 写意图位图与增量 resync |
| `drivers/md/raid5.c` | `raid5_make_request`、`make_stripe_request`、`reshape_request` |
| `drivers/md/raid1.c` | raid1 镜像读写与错误重定向 |
| `drivers/md/raid10.c` | raid10 条带镜像布局 |
| `drivers/md/raid0.c` | raid0 条带拆分 |
