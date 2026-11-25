# sync 系统调用分析

## 1. 概述

刷新所有文件系统缓冲区到磁盘。遍历所有已挂载的超级块，将脏页和元数据写入磁盘。

**原型：**

```c
SYSCALL_DEFINE0(sync)
```

**参数：** 无

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EIO` — I/O 错误

### 1.1 源码核对：sync() 实际上总是返回 0

本树 `fs/sync.c` 的入口非常薄：

```c
SYSCALL_DEFINE0(sync)
{
	ksys_sync();
	return 0;
}
```

也就是说 `ksys_sync()` 的返回值被丢弃，`sync(2)` 在 Linux 7.2 中**不会**把单个文件系统的回写错误上报给调用者。历史上 `sync()` 曾返回统计出的错误数并以 `-EIO` 结束，但在当前实现里错误语义已迁移到 `fsync()`/`syncfs()`：它们通过 `errseq_t`（`sb->s_wb_err` 与 `file->f_sb_err`）把"自上次检查以来新产生的写回错误"返回给应用。因此：

| 调用 | 能否获知写回错误 | 机制 |
|--|--|--|
| `sync()` | 否 | 返回值恒为 0，错误只进 `errseq` |
| `syncfs(fd)` | 是 | `errseq_check_and_advance(&sb->s_wb_err, &file->f_sb_err)` |
| `fsync(fd)` | 是 | `->fsync()` 逐文件上报 |
| `sync_file_range()` | 部分 | 仅 `WAIT_BEFORE/WAIT_AFTER` 返回 EIO/ENOSPC |

### 1.2 sync 家族的分工

`fs/sync.c` 一个文件里同时住着四组接口，它们的范围与"是否写元数据"差别很大：

| 接口 | 范围 | 写数据 | 写元数据 | 等待 I/O | 阻塞粒度 |
|--|--|--|--|--|--|
| `sync()` | 所有已挂载 sb + 所有块设备 | 是 | 是（`->sync_fs`） | 是 | 全局 |
| `syncfs(fd)` | 该 fd 所属超级块 | 是 | 是 | 是 | 单文件系统 |
| `fsync(fd)` | 单个文件 | 是 | 是 | 是 | 单文件 |
| `fdatasync(fd)` | 单个文件 | 是 | 仅访问所需 | 是 | 单文件 |
| `sync_file_range()` | 单文件区间 | 是 | **否** | 可选 | 单区间 |

`vfs_fsync_range()` 的实现解释了 `fdatasync` 的差异：

```c
int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file->f_mapping->host;

	if (!file->f_op->fsync)
		return -EINVAL;
	if (!datasync)
		sync_lazytime(inode);
	return file->f_op->fsync(file, start, end, datasync);
}
```

`datasync == 0` 时先把 `lazytime` 记账的时间戳落盘，再交给文件系统的 `->fsync`；`datasync == 1` 时由具体文件系统只保证"读回数据所必需"的元数据（例如 ext4 只写 extent 索引，不写 `mtime`）。

## 2. 使用场景

- **系统关机/重启**: 关机前刷新所有缓存
- **`sync` 命令**: 手动触发全局同步
- **紧急数据持久化**: 在关键操作后确保所有数据落盘

## 3. 函数调用栈

```
sync() (系统调用入口)
└─ ksys_sync()                                        // fs/sync.c
   └─ iterate_supers(sync_fs_one, sb)                  // 遍历所有超级块
        └─ sync_fs_one(sb, ...)
             ├─ sync_filesystem(sb)                    // 同步单个文件系统
             │    ├─ sb->s_op->sync_fs(sb, 0)          // 文件系统同步
             │    ├─ writeback_inodes_sb(sb, WB_REASON_SYNC)  // 写回脏 inode
             │    └─ wait_sb_inodes(sb)                // 等待所有写回完成
             └─ sync_blockdev(sb->s_bdev)              // 同步块设备缓存
```

### 3.1 ksys_sync 的真实执行顺序

`fs/sync.c` 中 `ksys_sync()` 只有六行，但每一行的目的不同：

```c
void ksys_sync(void)
{
	int nowait = 0, wait = 1;

	wakeup_flusher_threads(WB_REASON_SYNC);
	iterate_supers(sync_inodes_one_sb, NULL);
	iterate_supers(sync_fs_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &wait);
	sync_bdevs(false);
	sync_bdevs(true);
}
```

逐行说明：

| 步骤 | 调用 | 作用 |
|--|--|--|
| 1 | `wakeup_flusher_threads(WB_REASON_SYNC)` | 唤醒所有 `bdi` 的 flusher 线程，让回写在**所有设备上并行**开始 |
| 2 | `iterate_supers(sync_inodes_one_sb)` | 对每个非只读 sb 调用 `sync_inodes_sb()`，同步地把脏 inode 写完并等待 |
| 3 | `iterate_supers(sync_fs_one_sb, &nowait)` | 第一遍 `->sync_fs(sb, 0)`：不等待，只把元数据交给文件系统，避免"等日志提交"拖慢整轮 |
| 4 | `iterate_supers(sync_fs_one_sb, &wait)` | 第二遍 `->sync_fs(sb, 1)`：此时数据已在盘上，元数据状态稳定，可以安全等待日志/事务提交 |
| 5 | `sync_bdevs(false)` | 遍历块设备 inode，不等待地冲洗 `bd_mapping` |
| 6 | `sync_bdevs(true)` | 再遍历一次并等待，覆盖 ext2 这类"只把元数据写进块设备页缓存、不在 `->sync_fs` 里自己同步"的文件系统 |

`sync_fs_one_sb()` 的过滤条件值得单独记住：

```c
static void sync_fs_one_sb(struct super_block *sb, void *arg)
{
	if (!sb_rdonly(sb) && !(sb->s_iflags & SB_I_SKIP_SYNC) &&
	    sb->s_op->sync_fs)
		sb->s_op->sync_fs(sb, *(int *)arg);
}
```

三个条件分别是：只读文件系统直接跳过；`SB_I_SKIP_SYNC` 标记的超级块跳过——overlayfs 在 `fs/overlayfs/super.c` 里就设置了这个标志，因为它的数据实际由下层/上层文件系统负责落盘；没有实现 `->sync_fs` 的文件系统自然无事可做。

### 3.2 sync_filesystem 的精确顺序

`syncfs()` 和挂载/卸载路径复用的是 `sync_filesystem()`，它比 `ksys_sync()` 更细致，先 nowait 再 wait：

```c
int sync_filesystem(struct super_block *sb)
{
	int ret = 0;

	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	if (sb_rdonly(sb))
		return 0;

	writeback_inodes_sb(sb, WB_REASON_SYNC);
	if (sb->s_op->sync_fs) {
		ret = sb->s_op->sync_fs(sb, 0);
		if (ret)
			return ret;
	}
	ret = sync_blockdev_nowait(sb->s_bdev);
	if (ret)
		return ret;

	sync_inodes_sb(sb);
	if (sb->s_op->sync_fs) {
		ret = sb->s_op->sync_fs(sb, 1);
		if (ret)
			return ret;
	}
	return sync_blockdev(sb->s_bdev);
}
EXPORT_SYMBOL(sync_filesystem);
```

要点：

1. `WARN_ON(!rwsem_is_locked(&sb->s_umount))` 表明调用者必须持有 `s_umount`，防止同步期间文件系统从 r/o 切到 r/w（或反之）。`syncfs()` 用的是 `down_read()`，而冻结/卸载路径用写锁。
2. `writeback_inodes_sb()` 只是"发起"回写，不等待；随后的 `sync_inodes_sb()` 才是等待点，它内部会调用 `wait_sb_inodes()`，用 `s_inode_wblist_lock` + RCU 把 `s_inodes_wb` 上的 inode 摘到临时链表，再对每个 mapping 做 `filemap_fdatawait_keep_errors()`。保留错误（keep_errors）是为了让应用之后能用 `fsync()` 看到写回失败。
3. `sync_blockdev_nowait()` 是 `filemap_flush(bdev->bd_mapping)`，`sync_blockdev()` 是 `filemap_write_and_wait()`。前者只提交、后者等待，正好对应 `->sync_fs(sb, 0)` 与 `->sync_fs(sb, 1)` 的两阶段语义。

### 3.3 syncfs 的独立路径

```c
SYSCALL_DEFINE1(syncfs, int, fd)
{
	CLASS(fd, f)(fd);
	struct super_block *sb;
	int ret, ret2;

	if (fd_empty(f))
		return -EBADF;
	sb = fd_file(f)->f_path.dentry->d_sb;

	down_read(&sb->s_umount);
	ret = sync_filesystem(sb);
	up_read(&sb->s_umount);

	ret2 = errseq_check_and_advance(&sb->s_wb_err, &fd_file(f)->f_sb_err);

	return ret ? ret : ret2;
}
```

注意两点：`f_path` 是挂载路径，`dentry->d_sb` 因此取到的是**该 fd 所见**的超级块（bind mount 的情况下就是目标文件系统）；`errseq_check_and_advance()` 以文件为单位记录"已报告到哪个序号"，所以同一错误不会被同一 fd 重复报告，但另一个 fd 仍能各自看到一次。

## 4. 关键数据结构

```c
// ===== struct file (文件对象, include/linux/fs.h) =====
struct file {
    struct path f_path;                    // 文件路径
    struct inode *f_inode;                 // 指向 inode
    const struct file_operations *f_op;    // 文件操作函数集
    atomic_long_t f_count;                 // 引用计数
    unsigned int f_flags;                  // 打开标志
    loff_t f_pos;                          // 文件偏移
    struct address_space *f_mapping;       // 地址空间映射
};

// ===== struct address_space (地址空间, include/linux/fs.h) =====
struct address_space {
    struct inode *host;                    // 所属 inode
    struct xarray i_pages;                 // 页缓存基数树
    unsigned long nrpages;                 // 页缓存页数
    const struct address_space_operations *a_ops;  // 地址空间操作
};

// ===== struct super_block 超级块 (见 mount 相关文档) =====
```

### 4.1 与同步直接相关的字段

`struct super_block` 在 7.2 中已搬到 `include/linux/fs/super_types.h`，同步路径真正用到的成员如下：

| 字段 | 类型 | 作用 |
|--|--|--|
| `s_umount` | `struct rw_semaphore` | 保护"挂载状态 + r/o 与 r/w 切换"；`sync_filesystem()` 要求持有 |
| `s_op` | `const struct super_operations *` | 提供 `->sync_fs(sb, wait)` |
| `s_bdev` | `struct block_device *` | 底层块设备；无设备的伪文件系统为 NULL |
| `s_iflags` | `unsigned long` | `SB_I_SKIP_SYNC` 等内部标志 |
| `s_sync_lock` | `struct mutex` | 序列化对同一 sb 的 `sync_inodes_sb()` |
| `s_inode_wblist_lock` | `spinlock_t` | 保护 `s_inodes_wb` 链表 |
| `s_inodes_wb` | `struct list_head` | 处于回写状态的 inode 队列 |
| `s_wb_err` | `errseq_t` | 该 sb 最近一次写回错误的序号 |

`struct super_operations` 里的同步回调只有一行：

```c
int (*sync_fs)(struct super_block *sb, int wait);
```

`wait` 为 0 表示"异步提交元数据"，为 1 表示"提交并等待完成"。文件系统若返回错误，`sync_filesystem()` 会立刻向上返回，不再执行后续步骤。

`errseq_t` 的实现在 `lib/errseq.c`：低 `ERRSEQ_SHIFT`（= `ilog2(MAX_ERRNO) + 1`，即 12）位存负错误码，第 12 位是 `ERRSEQ_SEEN` 采样标志，再往上是序号计数器。`errseq_set()` 记录错误并在"上次记录后无人采样"时递增序号；`errseq_check_and_advance()` 比较调用者保存的 `since`，序号变了才返回错误并把 `since` 推进到最新。这样多个 fd 共享同一个 `sb->s_wb_err` 也不会互相"吃掉"错误。

## 5. 流程图

```
sync()
  │
  v
ksys_sync()
  │
  └─ iterate_supers(sync_fs_one)
       │
       └─ for each super_block:
            │
            ├─ sync_filesystem(sb)
            │    ├─ sync_fs(sb, 0)            // 元数据同步
            │    ├─ writeback_inodes_sb()     // 脏页写回
            │    └─ wait_sb_inodes()          // 等待完成
            │
            └─ sync_blockdev(sb->s_bdev)     // 块设备缓存同步
```

### 5.1 顺序为什么不能随意调换

这条流程的每一步都在为下一步"消除不确定性"：

```
wakeup_flusher_threads
   ↓ 让并行回写先跑起来
sync inodes（等待数据页写回完成）
   ↓ 数据落盘后，元数据才不会再被数据写回"拽着变"
->sync_fs(sb, 0)  →  块设备 nowait flush
   ↓ 元数据先提交一轮
->sync_fs(sb, 1)  →  块设备 wait flush
   ↓ 等待日志/事务提交完成，并覆盖"元数据只写块设备页缓存"的 fs
返回
```

如果先做 `->sync_fs()` 再写数据，则文件系统可能已经提交了一份"指向尚未落盘数据"的元数据；如果只做一次 `->sync_fs(sb, 0)`，ext3/ext4 这类日志文件系统可能还没等到 journal commit 返回就报告成功。

### 5.2 锁与阻塞行为

| 层面 | 保护 | 说明 |
|--|--|--|
| 挂载状态 | `sb->s_umount`（rw_semaphore） | `__iterate_supers()` 先 `sb->s_count++` 防止 sb 被释放，再在没有 `SUPER_ITER_UNLOCKED` 标志时用 `super_lock(sb, excl)` 持读锁调用回调——这正好满足 `sync_filesystem()` 的 `WARN_ON(!rwsem_is_locked(&sb->s_umount))`；`syncfs()` 则显式 `down_read()` |
| inode 回写队列 | `sb->s_inode_wblist_lock` + RCU | `wait_sb_inodes()` 摘链后 `__iget()` 稳住 inode |
| 同一 sb 的并发 sync | `sb->s_sync_lock`（mutex） | 防止两个 `sync_inodes_sb()` 交叉等待 |
| 进程可中断性 | 不可中断 | `ksys_sync()` 全路径没有 `signal_pending()` 检查，只能等它跑完 |

`sync()` 是不可中断的：在大量脏页或慢设备上可能阻塞数秒到数十秒，这也是"不要在交互线程里随手调 `sync()`"的原因。需要可中断性时应改用 `syncfs()` 配合 `fsync()`。

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    // 同步所有文件系统
    sync();
    printf("All file systems synced\n");
    return 0;
}
```

### 6.1 错误码与返回值

| 接口 | 错误码 | 触发条件（源码位置） |
|--|--|--|
| `sync()` | 无（恒 0） | `SYSCALL_DEFINE0(sync)` 丢弃结果 |
| `syncfs(fd)` | `-EBADF` | `fd_empty(f)` 为真 |
| `syncfs(fd)` | `-EIO` / `-ENOSPC` | `sync_filesystem()` 或 `errseq_check_and_advance()` 返回 |
| `fsync(fd)` | `-EBADF` | `fd_empty(f)` |
| `fsync(fd)` | `-EINVAL` | `file->f_op->fsync == NULL`（如管道、socket） |
| `fsync(fd)` | `-EIO` / `-ENOSPC` / `-EROFS` | 由具体文件系统的 `->fsync` 返回 |
| `sync_file_range()` | `-EBADF` | fd 无效 |
| `sync_file_range()` | `-EINVAL` | 非法 flags，或 offset/endbyte 为负、`endbyte < offset` |
| `sync_file_range()` | `-ESPIPE` | 既不是普通文件、块设备，也不是目录 |

`sync_file_range()` 的 flags 组合有专门的语义约定（见 `fs/sync.c` 注释）：

```
SYNC_FILE_RANGE_WAIT_BEFORE|WRITE         数据完整性写，不等待完成
SYNC_FILE_RANGE_WRITE                     异步冲洗，不适合作数据完整性
SYNC_FILE_RANGE_WAIT_AFTER                等待先前 WRITE 的结果
WAIT_BEFORE|WRITE|WAIT_AFTER (=WRITE_AND_WAIT)  传统 sync 语义（不含磁盘缓存 flush）
```

它**不写文件元数据**，所以对"新建/扩展文件后立即掉电"的场景没有保护作用。

### 6.2 内核内部的 sync 使用点

| 场景 | 调用 | 位置 |
|--|--|--|
| SysRq `s` | `emergency_sync()` | `drivers/tty/sysrq.c` |
| 重启/关机前 | `emergency_sync()` | `kernel/reboot.c` |
| SCMI 电源控制 | `emergency_sync()` | `drivers/firmware/arm_scmi/scmi_power_control.c` |
| ext4 remount 只读 | `sync_filesystem(sb)` | `fs/ext4/super.c` |

`emergency_sync()` 不能直接复用 `ksys_sync()`，因为它可能在中断/原子上下文被触发，于是把工作丢给 workqueue，并且**同步两轮**以弥补"第一次遍历时某些 inode 恰好被锁住"的漏网：

```c
static void do_sync_work(struct work_struct *work)
{
	int nowait = 0;
	int wait = 1;

	iterate_supers(sync_inodes_one_sb, NULL);
	iterate_supers(sync_fs_one_sb, &nowait);
	sync_bdevs(false);
	iterate_supers(sync_inodes_one_sb, NULL);
	iterate_supers(sync_fs_one_sb, &wait);
	sync_bdevs(false);
	printk("Emergency Sync complete\n");
	kfree(work);
}
```

注意它用 `kmalloc_obj(*work, GFP_ATOMIC)` 分配工作项、不调用 `wakeup_flusher_threads()`、最后打印 `Emergency Sync complete`——这些都体现了"在异常路径上尽量少依赖调度器"的取向。

### 6.3 相关系统调用

| 系统调用 | 范围 | 笔记 |
|--|--|--|
| `fsync` | 单文件（数据+元数据） | [fsync.md](fsync.md) |
| `fdatasync` | 单文件（数据+必要元数据） | [fdatasync.md](fdatasync.md) |
| `syncfs` | 单超级块 | [syncfs.md](syncfs.md) |
| `sync_file_range` | 单文件区间 | [sync_file_range.md](sync_file_range.md) |

## 7. 参考

- `fs/sync.c` — sync 实现
- `include/linux/fs.h` — super_block 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#5-文件系统挂载与结构)