# fasync / SIGIO — 信号驱动 I/O

## 1 概述

fasync/SIGIO 是传统的信号驱动 I/O 机制，当文件描述符就绪（可读/可写/异常）时，内核向进程发送 SIGIO 信号。该机制已被 `epoll` 和 `io_uring` 取代，目前在部分旧驱动和特殊场景中仍有使用。

- **文件**: `fs/fcntl.c`, `include/linux/fs.h`
- **执行上下文**: 取决于调用方（通常为硬中断或进程上下文）
- **状态**: 已过时，建议使用 epoll / io_uring

## 2 实现原理

### 2.1 核心数据结构

```c
// include/linux/fs.h
struct fasync_struct {
    rwlock_t fa_lock;                   // 保护 fd 和 file 的读写锁
    int magic;                          // 魔数 (FASYNC_MAGIC = 0x4601)
    int fa_fd;                          // 文件描述符
    struct fasync_struct *fa_next;      // 单向链表下一节点
    struct file *fa_file;               // 指向目标文件
    struct rcu_head fa_rcu;             // RCU 释放回调
};
```

每个文件结构体中包含 fasync 链表头和文件所有者信息：

```c
struct file {
    struct fown_struct f_owner;         // 文件所有者（接收信号的目标）
    ...
};

struct fown_struct {
    spinlock_t lock;                     // 保护锁
    struct pid *pid;                     // 接收信号的进程/进程组
    enum pid_type pid_type;              // PID 类型（PID/PGID/TGID）
    int signum;                          // 信号编号 (默认 SIGIO)
    uid_t euid, uid;                     // 权限检查
};
```

### 2.2 注册流程

```
fcntl(fd, F_SETFL, flags | FASYNC)
  │
  └─ ioctl_fioasync()
       └─ filp->f_op->fasync(fd, filp, on)
            │
            └─ fasync_helper(fd, filp, on, &filp->fasync)
                 │
                 ├─ on == 0 (移除):
                 │    └─ fasync_remove_entry()
                 │         └─ 从链表移除，清除 FASYNC 标志
                 │
                 └─ on == 1 (添加):
                      └─ fasync_add_entry()
                           ├─ fasync_alloc() 分配 fasync_struct
                           └─ fasync_insert_entry() 插入链表
                                └─ 设置 filp->f_flags |= FASYNC
```

### 2.3 信号发送流程

当文件 I/O 状态变化时，驱动调用 `kill_fasync()` 通知进程：

```
kill_fasync(&fp, sig, band)
  │
  └─ rcu_read_lock()
       └─ kill_fasync_rcu(fa, sig, band)
            ├─ 遍历 fasync 链表
            ├─ read_lock(&fa->fa_lock)
            ├─ send_sigio(fown, fa->fa_fd, band)
            │    └─ send_sigio_to_task(p, fown, fd, band, type)
            │         ├─ signum = READ_ONCE(fown->signum)
            │         └─ if (signum != 0)
            │              ├─ 构造 siginfo_t (si_signo, si_fd, si_band)
            │              └─ do_send_sig_info(signum, &si, p, type)
            │              else
            │              └─ do_send_sig_info(SIGIO, SEND_SIG_PRIV, p, type)
            └─ read_unlock(&fa->fa_lock)
       └─ rcu_read_unlock()
```

### 2.4 信号带的数据

通过 `F_SETSIG` 可以设置自定义信号编号（默认 SIGIO），信号处理函数可通过 `si_fd` 和 `si_band` 获取具体信息：

```c
siginfo_t si;
si.si_signo = signum;     // 信号编号
si.si_errno = 0;          // 错误码
si.si_code  = reason;     // POLL_IN / POLL_OUT / POLL_MSG / POLL_ERR / POLL_PRI
si.si_band  = band;       // POLLIN / POLLOUT / POLLRDHUP 等
si.si_fd    = fd;         // 就绪的文件描述符
```

## 3 使用场景

| 场景 | 说明 | 示例 |
|--|--|--|
| 管道/ FIFO | 管道可读/可写时通知 | `fs/pipe.c` |
| FUSE 文件系统 | 设备文件 I/O 就绪 | `fs/fuse/dev.c` |
| 串口设备 | 串口数据到达 | 串口驱动 |
| 文件租约 (lease) | 文件租约被破坏 | `fs/locks.c` |
| 核心转储 | 管道写入完成 | `fs/coredump.c` |

## 4 关键 API

| API | 说明 |
|--|--|
| `fasync_helper(fd, filp, on, fapp)` | 辅助函数，处理 fasync 链表添加/移除 |
| `fasync_alloc()` | 分配 fasync_struct |
| `fasync_insert_entry(fd, filp, fapp, new)` | 插入新 fasync 条目 |
| `fasync_remove_entry(filp, fapp)` | 移除 fasync 条目 |
| `kill_fasync(fapp, sig, band)` | 向所有注册者发送信号 |
| `send_sigio(fown, fd, band)` | 发送 SIGIO 信号 |
| `send_sigurg(file)` | 发送 SIGURG 信号 |

## 5 驱动实现示例

### 5.1 字符设备驱动中使用 fasync

```c
static int mydev_fasync(int fd, struct file *filp, int on)
{
    struct mydev *dev = filp->private_data;
    return fasync_helper(fd, filp, on, &dev->fasync);
}

static int mydev_release(struct inode *inode, struct file *filp)
{
    struct mydev *dev = filp->private_data;
    // 移除 fasync 条目
    mydev_fasync(-1, filp, 0);
    return 0;
}

// 数据到达时通知
static void mydev_data_ready(struct mydev *dev)
{
    kill_fasync(&dev->fasync, SIGIO, POLL_IN);
}

static const struct file_operations mydev_fops = {
    .fasync = mydev_fasync,
    .release = mydev_release,
    ...
};
```

### 5.2 用户态使用

```c
int fd = open("/dev/mydev", O_RDWR);
// 设置信号处理
signal(SIGIO, my_signal_handler);
// 设置文件描述符所有者
fcntl(fd, F_SETOWN, getpid());
// 设置信号编号（可选，默认 SIGIO）
fcntl(fd, F_SETSIG, SIGRTMIN);
// 启用异步通知
fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | FASYNC);
```

## 6 与 epoll / io_uring 的对比

| 特性 | fasync/SIGIO | epoll | io_uring |
|--|--|--|--|
| 通知方式 | 信号 | 事件队列 | 共享 ring buffer |
| 支持大量 fd | 否（信号队列有限） | 是 | 是 |
| 性能 | 低（信号处理开销大） | 中 | 高 |
| 可伸缩性 | 差 | 好 | 优秀 |
| 支持边缘触发 | 否 | 是 | 是 |
| 支持网络 I/O | 是 | 是 | 是 |
| 支持文件 I/O | 部分 | 部分 | 全部 |
| 内核复杂度 | 低 | 中 | 高 |
| 当前状态 | 已过时 | 推荐 | 推荐（新项目） |