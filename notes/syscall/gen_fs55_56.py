#!/usr/bin/env python3
"""Generate complete system call documents for sections 5.5 and 5.6."""

import os

FS_DIR = "/home/louis/code/linux/notes/syscall/filesystem-mount"
DP_DIR = "/home/louis/code/linux/notes/syscall/directory-path"

# ============================================================
# Common data structures used across multiple files
# ============================================================

MOUNT_DATA_STRUCTURES = """```c
// ===== struct mount (挂载实例, include/linux/mount.h) =====
struct mount {
    struct hlist_node mnt_hash;           // 挂载哈希链表节点
    struct mount *mnt_parent;             // 父挂载
    struct dentry *mnt_mountpoint;        // 挂载点的 dentry
    struct vfsmount mnt;                  // 通用 VFS 挂载结构
    struct list_head mnt_mounts;          // 子挂载链表
    struct list_head mnt_child;           // 兄弟挂载链表
    struct list_head mnt_instance;        // 超级块关联链表
    const char *mnt_devname;              // 设备名（如 /dev/sda1）
    struct list_head mnt_list;            // 挂载命名空间链表
    int mnt_id;                           // 挂载 ID
    int mnt_group_id;                     // 挂载组 ID
    struct mnt_pcp __percpu *mnt_pcp;     // 每 CPU 引用计数
};

// ===== struct vfsmount (VFS 挂载, include/linux/mount.h) =====
struct vfsmount {
    struct dentry *mnt_root;              // 挂载的根 dentry
    struct super_block *mnt_sb;           // 指向超级块
    int mnt_flags;                        // 挂载标志
    struct user_namespace *mnt_userns;    // 挂载的用户命名空间
};

// ===== struct super_block (超级块, include/linux/fs.h) =====
struct super_block {
    struct list_head s_list;              // 全局超级块链表
    dev_t s_dev;                          // 设备号
    unsigned long s_blocksize;            // 块大小（字节）
    struct file_system_type *s_type;      // 文件系统类型
    const struct super_operations *s_op;  // 超级块操作函数集
    struct dentry *s_root;                // 根 dentry
    struct list_head s_inodes;            // 所有 inode 链表
    struct list_head s_mounts;            // 关联的挂载链表
    void *s_fs_info;                      // 文件系统私有数据
};

// ===== struct mnt_id_req (挂载 ID 查询请求, include/uapi/linux/mount.h) =====
struct mnt_id_req {
    __u32 size;           // 结构体大小
    __u64 mnt_id;         // 挂载 ID
    __u64 last_mnt_id;    // 上次查询的最后一个 ID（listmount 分页用）
    __u64 mnt_group_id;   // 挂载组 ID
    __u32 param;          // 预留参数
};

// ===== struct statmount (挂载点统计信息, include/uapi/linux/statmount.h) =====
struct statmount {
    __u32 size;             // 结构体大小
    __u64 mnt_id;           // 挂载 ID
    __u64 mnt_parent_id;    // 父挂载 ID
    __u64 mnt_id_old;       // 旧挂载 ID
    __u64 mnt_parent_id_old; // 旧父挂载 ID
    __u64 mnt_attr;         // 挂载属性
    __u64 mnt_attr_userns;  // 用户命名空间属性
    __u64 mnt_root;         // 挂载根
    __u64 mount_options;    // 挂载选项
};
```

## 参考

- `fs/namespace.c` — 挂载命名空间核心实现
- `include/linux/mount.h` — 挂载结构定义
- `include/linux/fs.h` — 超级块定义
- `include/uapi/linux/mount.h` — 用户态挂载接口
- `include/uapi/linux/statmount.h` — statmount 接口定义"""

SYNC_DATA_STRUCTURES = """```c
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

## 参考

- `fs/sync.c` — sync/fsync/fdatasync/syncfs/sync_file_range 实现
- `fs/buffer.c` — buffer_head 写回逻辑
- `mm/page-writeback.c` — 页写回核心逻辑
- `include/linux/fs.h` — file/super_block 定义"""

SWAP_DATA_STRUCTURES = """```c
// ===== struct swap_info_struct (交换空间信息, include/linux/swap.h) =====
struct swap_info_struct {
    unsigned long flags;                  // SWP_* 标志位
    signed short prio;                    // 交换优先级
    signed char type;                     // 交换类型/索引
    unsigned char *swap_map;              // 页槽位映射数组
    unsigned int cluster_info;            // 集群分配信息
    unsigned int lowest_bit;              // 最低可用位
    unsigned int highest_bit;             // 最高可用位
    unsigned int pages;                   // 总页数
    unsigned int max;                     // 最大槽位索引
    unsigned int inuse_pages;             // 已使用的页数
    struct block_device *bdev;            // 块设备
    struct file *swap_file;               // 交换文件
    struct percpu_cluster __percpu *percpu_cluster; // 每 CPU 集群
};

// ===== struct swap_header (交换空间头部, include/linux/swap.h) =====
// 交换空间头部，位于交换分区/文件的第一个扇区
union swap_header {
    struct {
        char reserved[PAGE_SIZE - 10];    // 保留
        char magic[10];                   // 魔数 "SWAPSPACE2" 或 "SWAP-SPACE"
    } magic;
    struct {
        char bootbits[1024];              // 引导扇区（保留）
        unsigned int version;             // 版本
        unsigned int last_page;           // 最后一页
        unsigned int nr_badpages;         // 坏页数量
        unsigned int padding[125];        // 填充
        unsigned int badpages[1];         // 坏页列表
    } info;
};
```

## 参考

- `mm/swapfile.c` — swapon/swapoff 实现
- `include/linux/swap.h` — swap_info_struct 定义
- `include/linux/swapops.h` — 交换项操作宏"""

QUOTA_DATA_STRUCTURES = """```c
// ===== 配额命令宏 (include/uapi/linux/quota.h) =====
#define Q_QUOTAON  0x010001  // 启用配额
#define Q_QUOTAOFF 0x010002  // 停用配额
#define Q_GETQUOTA 0x030007  // 获取配额
#define Q_SETQUOTA 0x030008  // 设置配额
#define Q_SYNC     0x060001  // 同步配额文件

// ===== struct mem_dqblk (内存配额限制, include/linux/quota.h) =====
struct mem_dqblk {
    qsize_t dqb_bhardlimit;   // 块硬限制
    qsize_t dqb_bsoftlimit;   // 块软限制
    qsize_t dqb_curspace;     // 当前使用的空间
    qsize_t dqb_ihardlimit;   // inode 硬限制
    qsize_t qsize_t dqb_isoftlimit; // inode 软限制
    qsize_t dqb_curinodes;    // 当前 inode 数
    time64_t dqb_btime;       // 块限制宽限期
    time64_t dqb_itime;       // inode 限制宽限期
};

// ===== struct dquot (磁盘配额节点, include/linux/quota.h) =====
struct dquot {
    struct hlist_node dq_hash;     // 哈希链表
    struct list_head dq_inuse;     // 使用中链表
    struct list_head dq_free;      // 空闲链表
    struct list_head dq_dirty;     // 脏链表
    struct mutex dq_lock;          // 互斥锁
    spinlock_t dq_dqb_lock;       // 数据锁
    struct kqid dq_id;             // 配额 ID（用户/组）
    unsigned int dq_flags;         // 标志
    struct mem_dqblk dq_dqb;      // 配额限制数据
    struct super_block *dq_sb;     // 所属超级块
};
```

## 参考

- `fs/quota/quota.c` — quotactl/quotactl_fd 实现
- `fs/quota/dquot.c` — 配额核心逻辑
- `include/linux/quota.h` — 配额数据结构
- `include/uapi/linux/quota.h` — 用户态配额接口""" 

ACCT_DATA_STRUCTURES = """```c
// ===== struct acct (进程记账记录, include/uapi/linux/acct.h) =====
struct acct {
    char ac_flag;               // 记账标志（AFORK/ASU/ACOMPAT/ACORE/AXSIG）
    char ac_version;            // 版本号
    char ac_16bit_spare[2];     // 预留
    __u16 ac_tty;               // 控制终端
    __u32 ac_exitcode;          // 退出码
    __u32 ac_uid;               // 用户 ID
    __u32 ac_gid;               // 组 ID
    __u32 ac_pid;               // 进程 ID
    __u32 ac_ppid;              // 父进程 ID
    __u32 ac_btime;             // 开始时间（秒）
    float ac_etime;             // 已执行时间
    comp_t ac_utime;            // 用户态 CPU 时间
    comp_t ac_stime;            // 内核态 CPU 时间
    comp_t ac_mem;              // 平均内存使用量
    comp_t ac_io;               // 读写操作数
    comp_t ac_rw;               // 读写字节数
    char ac_comm[16];           // 命令名
};

// ===== struct pacct_struct (进程记账信息, kernel/acct.c) =====
struct pacct_struct {
    struct acct ac;             // 记账记录
    unsigned long ac_flag;      // 记账标志
    unsigned long ac_emul;      // 模拟标志
    atomic_t ac_count;          // 引用计数
};
```

## 参考

- `kernel/acct.c` — 进程记账实现
- `include/uapi/linux/acct.h` — 记账记录结构定义"""

DIR_OP_DATA_STRUCTURES = """```c
// ===== struct file_handle (文件句柄, include/uapi/linux/fs.h) =====
struct file_handle {
    unsigned int handle_bytes;   // 句柄数据大小
    int handle_type;             // 句柄类型
    unsigned char f_handle[];    // 句柄数据（可变长度）
};

// ===== struct file_lock (文件锁, include/linux/fs.h) =====
struct file_lock {
    struct file_lock *fl_next;       // 同一 inode 上的下一个锁
    struct list_head fl_list;        // 锁链表
    struct hlist_node fl_link;       // 哈希链表
    fl_owner_t fl_owner;             // 锁所有者
    unsigned int fl_flags;           // 锁标志
    unsigned char fl_type;           // 锁类型 (F_RDLCK/F_WRLCK/F_UNLCK)
    unsigned int fl_pid;             // 持有锁的进程 PID
    struct pid *fl_nspid;            // 命名空间 PID
    wait_queue_head_t fl_wait;       // 等待队列
    struct file *fl_file;            // 关联的文件
};

// ===== struct open_how (openat2 参数, include/uapi/linux/openat2.h) =====
struct open_how {
    __u64 flags;     // O_* 打开标志
    __u64 mode;      // 创建模式（O_CREAT 时有效）
    __u64 resolve;   // 路径解析控制标志
};
// resolve 标志位:
// RESOLVE_NO_XDEV       - 禁止跨设备
// RESOLVE_NO_MAGICLINKS - 禁止 magic 符号链接
// RESOLVE_NO_SYMLINKS   - 禁止符号链接
// RESOLVE_BENEATH       - 限制在 dfd 下
// RESOLVE_IN_ROOT       - 以根目录为锚点
```

## 参考

- `fs/open.c` — open/openat/openat2 实现
- `fs/namespace.c` — open_tree/open_tree_attr 实现
- `fs/fhandle.c` — name_to_handle_at/open_by_handle_at 实现
- `fs/locks.c` — flock 实现
- `fs/d_path.c` — getcwd 实现
- `fs/ioctl.c` — ioctl 实现
- `include/uapi/linux/openat2.h` — openat2 参数定义
- `include/uapi/linux/fs.h` — file_handle 定义"""


# ============================================================
# Helper function to write a file
# ============================================================
def write_file(filepath, content):
    with open(filepath, 'w') as f:
        f.write(content)
    print(f"  ✓ {os.path.basename(filepath)}")


# ============================================================
# Section 5.5 - 文件系统挂载与结构
# ============================================================

def gen_acct():
    content = """# acct 系统调用分析

## 1. 概述

启用或禁用进程记账（process accounting）。当启用时，内核在每个进程终止时向记账文件写入一条记录，记录该进程的资源使用情况。

**原型：**

```c
SYSCALL_DEFINE1(acct, const char __user *, name)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `name` | `const char *` | 记账文件的路径名；传入 NULL 表示停止记账 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_PACCT` 权限
  - `-ENOMEM` — 内存不足
  - `-EFAULT` — 用户态指针无效
  - `-EACCES` — 指定文件不可写

## 2. 使用场景

- **系统审计**: 记录所有用户进程的 CPU 时间、内存使用、I/O 操作等
- **资源计费**: 多用户环境下按使用量计费
- **性能分析**: 收集进程生命周期统计信息
- **安全监控**: 记录异常退出的进程（core dump、信号终止）

## 3. 函数调用栈

```
acct(name) (系统调用入口)
└─ ksys_acct(name)                                    // kernel/acct.c
   ├─ [name == NULL] → acct_file = NULL               // 关闭记账
   │
   └─ [name != NULL] → 打开记账文件
      ├─ filp_open(name, O_WRONLY|O_APPEND, 0)         // 打开记账文件
      ├─ acct_file = file                              // 保存记账文件
      │
      └─ [进程退出时]
         └─ acct_process()                             // kernel/acct.c
            └─ do_acct_process(acct_file, ...)         // 写记账记录
               ├─ fill_ac(acct)                        // 填充 acct 结构
               │  ├─ ac->ac_uid = from_kuid(uid)       // 用户 ID
               │  ├─ ac->ac_gid = from_kgid(gid)       // 组 ID
               │  ├─ ac->ac_pid = pid_nr(task_pid)     // 进程 ID
               │  ├─ ac->ac_btime = task->start_time   // 开始时间
               │  ├─ ac->ac_utime = ...                // 用户态 CPU 时间
               │  ├─ ac->ac_stime = ...                // 内核态 CPU 时间
               │  └─ ac->ac_comm = task->comm          // 命令名
               └─ file_write(acct_file, &ac, sizeof(ac)) // 写入文件
```

## 4. 关键数据结构

{ACCT_DATA_STRUCTURES}

## 5. 流程图

```
用户态调用 acct("/var/log/account/pacct")
  │
  v
ksys_acct(name)
  │
  ├─ filp_open(name, O_WRONLY|O_APPEND)  // 打开记账文件
  │
  ├─ 保存到全局 acct_file
  │
  └─ 进程退出时 ────────────────────┐
                                   v
                            do_acct_process()
                                   │
                                   ├─ fill_ac() 填充 acct 记录
                                   │   ├─ 进程基本信息 (pid, uid, gid, comm)
                                   │   ├─ CPU 时间 (utime, stime)
                                   │   └─ 内存使用 (mem)
                                   │
                                   └─ file_write() 写入记账文件
```

## 6. 使用示例

```c
#include <unistd.h>
#include <sys/acct.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 启用进程记账，记录到 /var/log/account/pacct
    if (acct("/var/log/account/pacct") == -1) {
        perror("acct");
        return 1;
    }
    printf("Accounting enabled\\n");

    // 执行一些操作...
    system("sleep 1");
    system("ls -l /tmp");

    // 停止记账
    if (acct(NULL) == -1) {
        perror("acct stop");
        return 1;
    }
    printf("Accounting disabled\\n");
    return 0;
}
```

## 7. 参考

- `kernel/acct.c` — 进程记账核心实现
- `include/uapi/linux/acct.h` — 记账记录结构定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "acct.md"), content)


def gen_chroot():
    content = """# chroot 系统调用分析

## 1. 概述

改变当前进程的根目录。将进程的根目录（`/`）改为指定目录，进程及其子进程将无法访问该目录之外的任何文件。

**原型：**

```c
SYSCALL_DEFINE1(chroot, const char __user *, filename)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `filename` | `const char *` | 新的根目录路径 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_CHROOT` 权限
  - `-EACCES` — 无执行权限
  - `-ENOENT` — 路径不存在
  - `-ENOTDIR` — 路径不是目录

## 2. 使用场景

- **chroot 监狱**: 隔离进程文件系统访问，用于测试或安全加固
- **系统恢复**: 在 Live CD 环境中 chroot 到损坏的系统
- **构建环境**: 为软件构建创建隔离的文件系统视图
- **容器初始化**: 容器运行时切换根文件系统的基础步骤

## 3. 函数调用栈

```
chroot(filename) (系统调用入口)
└─ ksys_chroot(filename)                              // fs/open.c
   ├─ user_path_at(AT_FDCWD, filename, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path)
   │                                                    // 路径解析
   ├─ inode_lock(path.dentry->d_inode)                 // 加锁
   ├─ mnt_want_write(path.mnt)                         // 可写检查
   │
   ├─ [权限检查]
   │  ├─ capable(CAP_SYS_CHROOT)                       // 需要 CAP_SYS_CHROOT
   │  └─ inode_permission(inode, MAY_EXEC | MAY_ACCESS) // 执行权限
   │
   ├─ set_fs_root(current->fs, &path)                  // 更新进程根目录
   │
   ├─ mnt_drop_write(path.mnt)                         // 释放写锁
   ├─ inode_unlock(path.dentry->d_inode)               // 解锁
   └─ path_put(&path)                                  // 释放路径引用
```

## 4. 关键数据结构

```c
// ===== struct fs_struct (进程文件系统信息, include/linux/fs_struct.h) =====
struct fs_struct {
    struct path root;              // 进程根目录
    struct path pwd;               // 进程当前工作目录
    struct seqcount rw_seqcount;   // 顺序锁
    int in_exec;                   // 是否正在执行 exec
};

// ===== struct path (路径, include/linux/path.h) =====
struct path {
    struct vfsmount *mnt;          // 挂载点
    struct dentry *dentry;         // 目录项
};
```

## 5. 流程图

```
用户态调用 chroot("/newroot")
  │
  v
ksys_chroot(filename)
  │
  ├─ user_path_at()  // 解析路径 → 获取 dentry+mnt
  │
  ├─ capable(CAP_SYS_CHROOT)  // 权限检查
  │
  ├─ inode_permission(MAY_EXEC)  // 新根目录需可执行
  │
  └─ set_fs_root(current->fs, &path)
       │
       ├─ path_get(&path)       // 增加引用计数
       ├─ path_put(&fs->root)   // 释放旧根引用
       └─ fs->root = path       // 设置新根
```

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

int main(void)
{
    // 准备 chroot 环境（需要 root 权限）
    // mkdir -p /tmp/jail/{bin,lib,lib64}
    // cp /bin/bash /tmp/jail/bin/
    // cp /lib/x86_64-linux-gnu/l* /tmp/jail/lib/

    if (chroot("/tmp/jail") == -1) {
        perror("chroot");
        return 1;
    }

    // 进入 chroot 环境后，需要切换工作目录
    if (chdir("/") == -1) {
        perror("chdir");
        return 1;
    }

    // 现在只能看到 /tmp/jail 中的文件
    // 执行 /bin/bash 会失败，因为找不到 /bin/bash
    // 需要先复制必要的二进制和库到 /tmp/jail

    printf("chroot to /tmp/jail successful\\n");
    return 0;
}
```

## 7. 参考

- `fs/open.c` — chroot 实现
- `include/linux/fs_struct.h` — fs_struct 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "chroot.md"), content)


def gen_fdatasync():
    content = """# fdatasync 系统调用分析

## 1. 概述

将文件数据同步到磁盘（仅同步必要元数据）。与 `fsync` 的区别在于，`fdatasync` 不强制同步不需要用于后续读取的元数据（如 `st_atime`），因此性能更好。

**原型：**

```c
SYSCALL_DEFINE1(fdatasync, unsigned int, fd)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的文件描述符
  - `-EIO` — I/O 错误
  - `-EINVAL` — 无效参数

## 2. 使用场景

- **数据库事务提交**: 确保 WAL 日志写入磁盘（如 SQLite 的 `PRAGMA synchronous = FULL`）
- **关键数据持久化**: 配置文件写入后确保不丢失
- **日志系统**: 保证日志记录已持久化

## 3. 函数调用栈

```
fdatasync(fd) (系统调用入口)
└─ do_fsync(fd, 1)                                    // fs/sync.c (datasync=1)
   ├─ fdget(fd)                                        // 获取 fd 对应的 file 结构
   ├─ file->f_op->fsync(file, start, end, 1)           // datasync=1
   │    └─ ext4_sync_file(file, 1)                     // ext4 实现
   │         ├─ filemap_write_and_wait_range()          // 等待脏页写回
   │         ├─ ext4_sync_inode(file, 1)               // 同步必要元数据
   │         │    └─ datasync=1 → 跳过 atime 等元数据
   │         └─ jbd2 事务提交
   └─ fdput(fd)                                        // 释放 fd 引用
```

## 4. 关键数据结构

{SYNC_DATA_STRUCTURES}

## 5. 流程图

```
fdatasync(fd)
  │
  v
do_fsync(fd, 1)  // datasync=1
  │
  ├─ fdget(fd) → 获取 file 对象
  │
  └─ ext4_sync_file(file, 1)
       │
       ├─ filemap_write_and_wait_range()
       │    ├─ 遍历脏页链表
       │    └─ 提交写回请求
       │
       ├─ ext4_sync_inode(file, 1)
       │    └─ 仅同步必要元数据（跳过 atime）
       │
       └─ jbd2 事务提交
            └─ 等待日志刷写到磁盘
```

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    int fd = open("/tmp/test.dat", O_WRONLY | O_CREAT, 0644);
    if (fd == -1) { perror("open"); return 1; }

    const char *data = "important data";
    write(fd, data, strlen(data));

    // 只同步数据和必要元数据（比 fsync 快）
    if (fdatasync(fd) == -1) {
        perror("fdatasync");
        return 1;
    }

    printf("Data synced\\n");
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — fdatasync/fsync 实现
- `fs/ext4/fsync.c` — ext4 同步实现
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "fdatasync.md"), content)


def gen_listmount():
    content = """# listmount 系统调用分析

## 1. 概述

列出当前挂载命名空间中的所有挂载点 ID，支持分页查询。这是 Linux 5.2+ 新挂载 API 的一部分。

**原型：**

```c
SYSCALL_DEFINE4(listmount, const struct mnt_id_req __user *, req,
        u64 __user *, mnt_ids, size_t, nr_mnt_ids, unsigned int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | `const struct mnt_id_req *` | 查询请求（包含 mnt_id、last_mnt_id 等） |
| `mnt_ids` | `__u64 *` | 接收挂载 ID 数组的缓冲区 |
| `nr_mnt_ids` | `size_t` | 缓冲区可容纳的 ID 数量 |
| `flags` | `unsigned int` | 标志位（`LISTMOUNT_REVERSE`） |

**返回值：**

- 成功返回写入的挂载 ID 数量
- 失败返回负值错误码

## 2. 使用场景

- **`findmnt` 命令**: 列出所有已挂载文件系统
- **容器管理**: 查看容器命名空间中的挂载点
- **系统监控**: 监控挂载点的变化

## 3. 函数调用栈

```
listmount(req, mnt_ids, nr_mnt_ids, flags) (系统调用入口)
└─ ksys_listmount(req, mnt_ids, nr_mnt_ids, flags)   // fs/namespace.c
   ├─ copy_from_user(&kreq, req, size)                 // 拷贝请求参数
   ├─ get_mnt_ns(current)                              // 获取当前挂载命名空间
   │
   ├─ [遍历挂载链表]
   │  └─ list_for_each_entry(mnt, &ns->list, mnt_list)
   │       ├─ 判断是否满足过滤条件
   │       └─ 将 mnt_id 写入 user buffer
   │
   └─ put_mnt_ns(ns)                                   // 释放命名空间引用
```

## 4. 关键数据结构

{MOUNT_DATA_STRUCTURES}

## 5. 流程图

```
listmount()
  │
  ├─ 拷贝用户参数 (mnt_id_req)
  │
  ├─ 获取当前挂载命名空间
  │
  └─ 遍历挂载链表
       │
       ├─ for each mount in ns->list:
       │    ├─ 过滤 (mnt_id, last_mnt_id)
       │    └─ 写入 mnt_ids[] 数组
       │
       └─ 返回写入的 ID 数量
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    struct mnt_id_req req = {
        .size = sizeof(struct mnt_id_req),
        .mnt_id = 0,
        .last_mnt_id = 0,
    };
    __u64 ids[1024];
    long ret = syscall(SYS_listmount, &req, ids, 1024, 0);
    if (ret < 0) { perror("listmount"); return 1; }

    printf("Mount count: %ld\\n", ret);
    for (long i = 0; i < ret; i++)
        printf("  mnt_id: %lu\\n", ids[i]);
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — listmount/statmount 实现
- `include/uapi/linux/mount.h` — mnt_id_req 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "listmount.md"), content)


def gen_statmount():
    content = """# statmount 系统调用分析

## 1. 概述

获取指定挂载点的详细信息，包括挂载 ID、父挂载 ID、挂载属性、选项等。这是 Linux 5.2+ 新挂载 API 的一部分。

**原型：**

```c
SYSCALL_DEFINE4(statmount, const struct mnt_id_req __user *, req,
        struct statmount __user *, buf, size_t, bufsize, unsigned int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | `const struct mnt_id_req *` | 查询请求（指定要查询的 mnt_id） |
| `buf` | `struct statmount *` | 接收挂载信息的缓冲区 |
| `bufsize` | `size_t` | 缓冲区大小 |
| `flags` | `unsigned int` | 标志位 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码

## 2. 使用场景

- **`statmount` 命令**: 获取挂载点详细信息
- **容器管理**: 查询容器命名空间的挂载属性
- **系统工具**: `df`、`mount` 等命令获取挂载信息

## 3. 函数调用栈

```
statmount(req, buf, bufsize, flags) (系统调用入口)
└─ ksys_statmount(req, buf, bufsize, flags)          // fs/namespace.c
   ├─ copy_from_user(&kreq, req, size)                 // 拷贝请求参数
   ├─ mnt_id = kreq.mnt_id                             // 获取目标挂载 ID
   ├─ mount = __lookup_mnt_id(ns, mnt_id)              // 通过 ID 查找挂载
   │
   └─ fill_statmount(ks, mount, flags)                 // 填充 statmount 结构
        ├─ ks->mnt_id = mount->mnt_id                  // 挂载 ID
        ├─ ks->mnt_parent_id = parent->mnt_id          // 父挂载 ID
        ├─ ks->mnt_attr = mount->mnt.mnt_flags         // 挂载属性
        ├─ ks->mnt_root = mount->mnt.mnt_root          // 挂载根
        └─ [更多字段...]
```

## 4. 关键数据结构

{MOUNT_DATA_STRUCTURES}

## 5. 流程图

```
statmount()
  │
  ├─ 拷贝用户参数 (mnt_id_req)
  │
  ├─ 通过 mnt_id 查找挂载实例
  │
  └─ fill_statmount()
       ├─ 填充 mnt_id / mnt_parent_id
       ├─ 填充 mnt_attr / mnt_attr_userns
       ├─ 填充 mnt_root / mount_options
       └─ copy_to_user(buf, ks, bufsize)
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <linux/statmount.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    struct mnt_id_req req = {
        .size = sizeof(struct mnt_id_req),
        .mnt_id = 1,  // 查询 mnt_id=1 的挂载点
    };
    struct statmount buf;
    long ret = syscall(SYS_statmount, &req, &buf, sizeof(buf), 0);
    if (ret < 0) { perror("statmount"); return 1; }

    printf("mnt_id: %lu\\n", buf.mnt_id);
    printf("parent_id: %lu\\n", buf.mnt_parent_id);
    printf("mnt_attr: %lu\\n", buf.mnt_attr);
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — statmount 实现
- `include/uapi/linux/statmount.h` — statmount 结构定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "statmount.md"), content)


def gen_swapoff():
    content = """# swapoff 系统调用分析

## 1. 概述

停用交换分区或交换文件，释放交换空间。

**原型：**

```c
SYSCALL_DEFINE1(swapoff, const char __user *, specialfile)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `specialfile` | `const char *` | 交换分区/文件的路径 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-ENOENT` — 指定的交换设备不存在
  - `-EBUSY` — 交换空间正在使用中
  - `-ENOMEM` — 内存不足，无法换回页面

## 2. 使用场景

- **系统关闭**: 关机前停用所有交换空间
- **交换空间调整**: 更换或删除交换分区前停用
- **内存回收**: 停用交换，强制将交换出的页面换回内存

## 3. 函数调用栈

```
swapoff(specialfile) (系统调用入口)
└─ ksys_swapoff(specialfile)                          // mm/swapfile.c
   ├─ path = user_path(specialfile)                    // 路径解析
   ├─ swap_info = find_swap_info_by_bdev(bdev)         // 查找 swap_info
   │
   ├─ [同步] ─── try_to_unuse(type, ...)               // 将交换出的页面换回
   │    ├─ for_each_possible_cpu()                     // 遍历所有 CPU
   │    ├─ for_each_online_pgdat()                     // 遍历所有内存节点
   │    │    └─ shrink_all_memory()                    // 回收内存
   │    └─ unuse_pte_range()                           // 逐个恢复 PTE 映射
   │
   ├─ [清理] ─── destroy_swap_extents(p)               // 释放交换区映射
   ├─ [关闭] ─── bdev_fput(swap_file)                  // 关闭块设备
   └─ [释放] ─── free_swap_info(p)                     // 释放 swap_info_struct
```

## 4. 关键数据结构

{SWAP_DATA_STRUCTURES}

## 5. 流程图

```
swapoff("/dev/sda2")
  │
  v
ksys_swapoff()
  │
  ├─ 查找 swap_info_struct
  │
  ├─ try_to_unuse()  ──────────────────────────┐
  │   (将交换出的页面换回内存)                     │
  │    ├─ 遍历所有进程的 VMA                      │
  │    ├─ 找到使用该交换区的 PTE                   │
  │    └─ 分配物理页，从交换区读回数据               │
  │                                              │
  ├─ destroy_swap_extents()  // 清理映射           │
  ├─ bdev_fput()            // 关闭设备            │
  └─ free_swap_info()       // 释放数据结构          │
```

## 6. 使用示例

```c
#include <unistd.h>
#include <sys/swap.h>
#include <stdio.h>

int main(void)
{
    // 停用交换分区
    if (swapoff("/dev/sda2") == -1) {
        perror("swapoff");
        return 1;
    }
    printf("Swap off /dev/sda2\\n");
    return 0;
}
```

## 7. 参考

- `mm/swapfile.c` — swapon/swapoff 实现
- `include/linux/swap.h` — swap_info_struct 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "swapoff.md"), content)


def gen_syncfs():
    content = """# syncfs 系统调用分析

## 1. 概述

刷新指定文件描述符所在文件系统的所有缓冲区到磁盘，类似 `sync` 但只作用于一个文件系统。

**原型：**

```c
SYSCALL_DEFINE1(syncfs, int, fd)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符（其所在文件系统将被同步） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的文件描述符
  - `-EIO` — I/O 错误

## 2. 使用场景

- **数据库**: 确保特定文件系统的数据持久化
- **外部存储卸载**: 卸载 USB 设备前同步其文件系统
- **文件系统管理**: 只同步指定文件系统，避免全面同步的性能开销

## 3. 函数调用栈

```
syncfs(fd) (系统调用入口)
└─ ksys_syncfs(fd)                                    // fs/sync.c
   ├─ fdget(fd)                                        // 获取 file 对象
   ├─ sb = file->f_path.dentry->d_sb                   // 获取超级块
   │
   └─ sync_filesystem(sb)                              // 同步文件系统
        ├─ filemap_write_and_wait(sb->s_mapping)        // 写回脏页
        ├─ sb->s_op->sync_fs(sb, 0)                    // 文件系统同步
        └─ sync_blockdev(sb->s_bdev)                   // 同步块设备
```

## 4. 关键数据结构

{SYNC_DATA_STRUCTURES}

## 5. 流程图

```
syncfs(fd)
  │
  ├─ fdget(fd) → 获取 file 对象
  │
  ├─ file->f_path.dentry->d_sb → 获取超级块
  │
  └─ sync_filesystem(sb)
       ├─ filemap_write_and_wait()  // 写回所有脏页
       ├─ sync_fs(sb, 0)            // 文件系统元数据同步
       └─ sync_blockdev()           // 同步块设备缓存
```

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

int main(void)
{
    int fd = open("/mnt/usb/test.txt", O_RDONLY);
    if (fd == -1) { perror("open"); return 1; }

    // 同步 /mnt/usb 所在文件系统
    if (syncfs(fd) == -1) {
        perror("syncfs");
        return 1;
    }
    printf("File system synced\\n");

    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — syncfs 实现
- `include/linux/fs.h` — super_block 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "syncfs.md"), content)


def gen_quotactl_fd():
    content = """# quotactl_fd 系统调用分析

## 1. 概述

通过文件描述符进行磁盘配额操作，与 `quotactl` 功能相同但通过 fd 而非路径指定设备。

**原型：**

```c
SYSCALL_DEFINE4(quotactl_fd, unsigned int, fd, unsigned int, cmd,
        qid_t, id, void __user *, addr)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符（所在文件系统） |
| `cmd` | `unsigned int` | 配额命令（Q_QUOTAON/Q_QUOTAOFF/Q_GETQUOTA/Q_SETQUOTA 等） |
| `id` | `qid_t` | 用户/组 ID |
| `addr` | `void *` | 配额数据缓冲区地址 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的 fd
  - `-EPERM` — 权限不足
  - `-EINVAL` — 无效命令

## 2. 使用场景

- 同 `quotactl`，但通过 fd 指定文件系统（避免路径竞争条件）
- 适用于已打开的文件描述符场景

## 3. 函数调用栈

```
quotactl_fd(fd, cmd, id, addr) (系统调用入口)
└─ ksys_quotactl_fd(fd, cmd, id, addr)               // fs/quota/quota.c
   ├─ fdget(fd)                                        // 获取 file 对象
   ├─ sb = file->f_inode->i_sb                         // 获取超级块
   │
   └─ do_quotactl(sb, cmd, id, addr)                  // 执行配额操作
        ├─ [cmd 分发]
        │  ├─ Q_QUOTAON  → sb->s_op->quota_on(sb, type, ...)  // 启用
        │  ├─ Q_QUOTAOFF → sb->s_op->quota_off(sb, type)      // 停用
        │  ├─ Q_GETQUOTA → dqget(sb, id, type) → 读取配额信息
        │  ├─ Q_SETQUOTA → dqget + dqcommit → 写入配额信息
        │  └─ Q_SYNC     → sb->s_op->sync_fs(sb, ...)         // 同步
        └─ ...
```

## 4. 关键数据结构

{QUOTA_DATA_STRUCTURES}

## 5. 流程图

```
quotactl_fd(fd, cmd, id, addr)
  │
  ├─ fdget(fd) → sb = file->f_inode->i_sb
  │
  └─ do_quotactl(sb, cmd, id, addr)
       │
       ├─ Q_QUOTAON  → 启用配额文件
       ├─ Q_QUOTAOFF → 停用配额
       ├─ Q_GETQUOTA → 获取用户/组的配额限制
       ├─ Q_SETQUOTA → 设置用户/组的配额限制
       └─ Q_SYNC     → 同步配额文件
```

## 6. 使用示例

```c
#include <sys/quota.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/mnt/data", O_RDONLY);
    if (fd == -1) { perror("open"); return 1; }

    struct dqblk quota;
    // 获取用户 1000 的磁盘配额
    if (quotactl_fd(fd, Q_GETQUOTA, 1000, (void *)&quota) == -1) {
        perror("quotactl_fd");
        return 1;
    }
    printf("User 1000 quota: cur=%lld, soft=%lld, hard=%lld\\n",
           quota.dqb_curspace, quota.dqb_bsoftlimit, quota.dqb_bhardlimit);
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/quota/quota.c` — quotactl_fd 实现
- `include/linux/quota.h` — 配额数据结构
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "quotactl_fd.md"), content)


def gen_sync_file_range():
    content = """# sync_file_range 系统调用分析

## 1. 概述

同步文件指定范围内的数据到磁盘，提供比 `fsync`/`fdatasync` 更细粒度的同步控制。

**原型：**

```c
SYSCALL_DEFINE4(sync_file_range, int, fd, loff_t, offset, loff_t, nbytes,
        unsigned int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符 |
| `offset` | `loff_t` | 同步起始偏移 |
| `nbytes` | `loff_t` | 同步字节数（0 表示从 offset 到文件末尾） |
| `flags` | `unsigned int` | 控制标志（见下表） |

**flags 标志：**

| 标志 | 值 | 说明 |
|------|-----|------|
| `SYNC_FILE_RANGE_WAIT_BEFORE` | 1 | 写入前等待页面的脏数据回写完成 |
| `SYNC_FILE_RANGE_WRITE` | 2 | 发起脏页的写回请求 |
| `SYNC_FILE_RANGE_WAIT_AFTER` | 4 | 写入后等待所有写回完成 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的 fd
  - `-EINVAL` — 无效的 flags
  - `-EIO` — I/O 错误

## 2. 使用场景

- **数据库预写日志**: 仅同步 WAL 文件的特定区域
- **大文件增量同步**: 分批同步大文件的已修改区域
- **零拷贝数据传输**: 在 `splice` 等操作后同步特定区域

## 3. 函数调用栈

```
sync_file_range(fd, offset, nbytes, flags) (系统调用入口)
└─ ksys_sync_file_range(fd, offset, nbytes, flags)   // fs/sync.c
   ├─ fdget(fd)                                        // 获取 file 对象
   ├─ file_write_and_wait_range(file, offset, endbyte)  // 先等待正在写回的页
   │    └─ __filemap_fdatawait_range(mapping, offset, endbyte)
   │
   ├─ filemap_fdatawrite_range(mapping, offset, endbyte) // 发起写回
   │    └─ do_writepages(mapping, &wbc)                 // 页面写回
   │
   └─ filemap_fdatawait_range(mapping, offset, endbyte)  // 等待写回完成
        └─ wait_on_page_writeback_range()
```

## 4. 关键数据结构

{SYNC_DATA_STRUCTURES}

## 5. 流程图

```
sync_file_range(fd, offset, nbytes, flags)
  │
  ├─ [WAIT_BEFORE]  → 等待 offset~offset+nbytes 的脏页写回完成
  │
  ├─ [WRITE]        → 发起 offset~offset+nbytes 的脏页写回
  │
  └─ [WAIT_AFTER]   → 等待写回完成
```

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    int fd = open("/tmp/largefile.dat", O_WRONLY | O_CREAT, 0644);
    if (fd == -1) { perror("open"); return 1; }

    char buf[4096] = {0};
    for (int i = 0; i < 1000; i++) {
        write(fd, buf, 4096);
    }

    // 分步同步：
    // 1. 等待前 1MB 的写回完成
    sync_file_range(fd, 0, 1024*1024, SYNC_FILE_RANGE_WAIT_BEFORE);
    // 2. 发起前 1MB 的写回
    sync_file_range(fd, 0, 1024*1024, SYNC_FILE_RANGE_WRITE);
    // 3. 等待写回完成
    sync_file_range(fd, 0, 1024*1024, SYNC_FILE_RANGE_WAIT_AFTER);

    printf("First 1MB synced\\n");
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — sync_file_range 实现
- `mm/page-writeback.c` — 页写回核心逻辑
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "sync_file_range.md"), content)


def gen_swapon():
    content = """# swapon 系统调用分析

## 1. 概述

启用交换分区或交换文件，使系统可以将内存页面换出到指定设备。

**原型：**

```c
SYSCALL_DEFINE2(swapon, const char __user *, specialfile, int, swap_flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `specialfile` | `const char *` | 交换分区/文件的路径 |
| `swap_flags` | `int` | 交换标志（`SWAP_FLAG_PREFER` 等） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-EBUSY` — 设备已被用作交换空间
  - `-EINVAL` — 无效的交换签名
  - `-ENOMEM` — 内存不足

## 2. 使用场景

- **系统启动**: 启用预设的交换分区（`/etc/fstab`）
- **临时扩展内存**: 添加交换文件以应对内存压力
- **休眠支持**: 启用休眠交换分区

## 3. 函数调用栈

```
swapon(specialfile, swap_flags) (系统调用入口)
└─ ksys_swapon(specialfile, swap_flags)               // mm/swapfile.c
   ├─ blkdev_get_by_path(path, FMODE_READ|FMODE_WRITE, ...)  // 打开块设备
   ├─ alloc_swap_info()                                // 分配 swap_info_struct
   │
   ├─ read_swap_header()                               // 读取交换空间头部
   │    └─ 校验魔数 ("SWAPSPACE2" 或 "SWAP-SPACE")
   │
   ├─ setup_swap_extents(p, ...)                       // 设置交换区映射
   │    └─ swapon_swapfile()                           // 交换文件检查
   │
   └─ enable_swap_info(p, prio, swap_map, ...)        // 启用交换空间
        ├─ insert_swap_info(p)                         // 插入 swap_info 数组
        └─ 更新全局 swap 优先级链表
```

## 4. 关键数据结构

{SWAP_DATA_STRUCTURES}

## 5. 流程图

```
swapon("/dev/sda2", 0)
  │
  v
ksys_swapon()
  │
  ├─ blkdev_get_by_path()  // 打开块设备
  │
  ├─ alloc_swap_info()     // 分配 swap_info_struct
  │
  ├─ read_swap_header()    // 读取交换头部
  │    └─ 校验魔数 "SWAPSPACE2"
  │
  ├─ setup_swap_extents()  // 建立槽位映射
  │
  └─ enable_swap_info()    // 启用交换
       ├─ swap_map = kvzalloc(pages)  // 分配槽位映射数组
       ├─ insert_swap_info()          // 插入全局数组
       └─ 更新优先级链表
```

## 6. 使用示例

```c
#include <unistd.h>
#include <sys/swap.h>
#include <stdio.h>

int main(void)
{
    // 启用交换分区
    if (swapon("/dev/sda2", 0) == -1) {
        perror("swapon");
        return 1;
    }
    printf("Swap on /dev/sda2 enabled\\n");
    return 0;
}
```

## 7. 参考

- `mm/swapfile.c` — swapon 实现
- `include/linux/swap.h` — swap_info_struct 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "swapon.md"), content)


# ============================================================
# Section 5.6 - 目录与路径操作
# ============================================================

def gen_open_by_handle_at():
    content = """# open_by_handle_at 系统调用分析

## 1. 概述

通过文件句柄（file handle）打开文件。与 `name_to_handle_at` 配对使用，用于实现 NFS 风格的持久化文件引用。

**原型：**

```c
SYSCALL_DEFINE3(open_by_handle_at, int, mountdirfd,
        struct file_handle __user *, handle, int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `mountdirfd` | `int` | 文件系统挂载点的 fd（由 `name_to_handle_at` 的 `mnt_id` 获得） |
| `handle` | `struct file_handle *` | 文件句柄 |
| `flags` | `int` | `O_*` 打开标志 |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码：
  - `-EACCES` — 权限不足
  - `-ESTALE` — 文件句柄已过期（文件被删除或移动）
  - `-EINVAL` — 无效的句柄

## 2. 使用场景

- **NFS 文件句柄**: 通过 NFS 导出的文件句柄重新打开文件
- **文件系统备份**: 备份时保存文件句柄，恢复时用句柄打开
- **文件系统检查**: `fsck` 工具通过 inode 号打开文件

## 3. 函数调用栈

```
open_by_handle_at(mountdirfd, handle, flags) (系统调用入口)
└─ do_handle_open(mountdirfd, handle, flags)           // fs/fhandle.c
   ├─ getname(handle->f_handle, handle->handle_bytes)   // 获取文件系统句柄
   │
   ├─ exportfs_decode_fh(sb, fh, flags, ...)           // 解码句柄为 dentry
   │    └─ sb->s_export_op->fh_to_dentry(sb, fh, ...)  // 文件系统特定实现
   │         └─ [ext4] → ext4_fh_to_dentry()
   │              └─ ext4_get_inode_by_inum()           // 通过 inode 号查找
   │
   └─ vfs_open(path, file)                              // 打开文件
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
open_by_handle_at(mountdirfd, handle, flags)
  │
  ├─ 通过 mountdirfd 找到文件系统超级块
  │
  ├─ exportfs_decode_fh(sb, fh)
  │    └─ fh_to_dentry(sb, fh)  // 解码句柄
  │         └─ ext4_fh_to_dentry()
  │              └─ ext4_get_inode_by_inum(inode_num)
  │                   └─ 读取 inode 表
  │
  └─ vfs_open(path, file)  // 打开文件并返回 fd
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/fs.h>

int main(void)
{
    // 1. 先获取文件句柄
    struct file_handle *handle;
    handle = malloc(sizeof(struct file_handle) + 64);
    handle->handle_bytes = 64;
    int mount_id;
    if (name_to_handle_at(AT_FDCWD, "/tmp/test",
                          handle, &mount_id, 0) == -1) {
        perror("name_to_handle_at");
        return 1;
    }

    // 2. 打开挂载点
    int mnt_fd = open("/", O_RDONLY);
    if (mnt_fd == -1) { perror("open mount"); return 1; }

    // 3. 通过句柄打开文件
    int fd = open_by_handle_at(mnt_fd, handle, O_RDWR);
    if (fd == -1) { perror("open_by_handle_at"); return 1; }

    printf("Opened by handle: fd=%d\\n", fd);
    close(fd);
    close(mnt_fd);
    free(handle);
    return 0;
}
```

## 7. 参考

- `fs/fhandle.c` — name_to_handle_at/open_by_handle_at 实现
- `include/uapi/linux/fs.h` — file_handle 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "open_by_handle_at.md"), content)


def gen_open_tree_attr():
    content = """# open_tree_attr 系统调用分析

## 1. 概述

打开挂载点并获取其属性 fd，通过 `mount_attr` 结构查询或修改挂载属性。这是 Linux 5.2+ 新挂载 API 的一部分。

**原型：**

```c
SYSCALL_DEFINE5(open_tree_attr, int, dfd, const char __user *, filename,
        unsigned, flags, struct mount_attr __user *, uattr,
        size_t, usize)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd |
| `filename` | `const char *` | 路径名 |
| `flags` | `unsigned int` | 打开标志（`OPEN_TREE_CLONE` 等） |
| `uattr` | `struct mount_attr *` | 挂载属性（查询/设置） |
| `usize` | `size_t` | 属性结构体大小 |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码

## 2. 使用场景

- **挂载属性查询**: 查询挂载点的传播类型、挂载标志等
- **挂载属性修改**: 设置挂载为只读、递归从属等
- **容器管理**: 管理容器命名空间的挂载属性

## 3. 函数调用栈

```
open_tree_attr(dfd, filename, flags, uattr, usize) (系统调用入口)
└─ ksys_open_tree_attr(dfd, filename, flags, uattr, usize)  // fs/namespace.c
   ├─ vfs_open_tree(dfd, filename, flags)                // 打开挂载树
   │    └─ 获取 mount 实例
   │
   └─ set_mount_attributes(mnt, uattr, ...)             // 设置/查询属性
        ├─ copy_from_user(&attr, uattr, size)            // 拷贝用户参数
        ├─ [查询] → 读取 mount 属性
        └─ [设置] → 修改 mount 属性
             ├─ mount->mnt.mnt_flags = attr.mnt_attr
             └─ 更新传播关系
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
open_tree_attr(dfd, path, flags, uattr, usize)
  │
  ├─ vfs_open_tree() → 获取 mount 实例
  │
  └─ set_mount_attributes()
       ├─ copy_from_user(uattr)  // 获取属性请求
       ├─ [查询] → 读取 mnt_flags 并返回
       └─ [设置] → 修改 mnt_flags
            ├─ 设置只读/读写
            ├─ 设置传播类型
            └─ 返回新 fd
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(void)
{
    struct mount_attr attr = {
        .attr_set = MOUNT_ATTR_RDONLY,  // 设置为只读
        .attr_clr = 0,
    };

    // 打开 /mnt 挂载点并设置属性
    int fd = syscall(SYS_open_tree_attr, AT_FDCWD,
                     "/mnt", 0, &attr, sizeof(attr));
    if (fd < 0) { perror("open_tree_attr"); return 1; }

    printf("Mount attribute set, fd=%d\\n", fd);
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — open_tree/open_tree_attr 实现
- `include/uapi/linux/mount.h` — mount_attr 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "open_tree_attr.md"), content)


def gen_fsync():
    content = """# fsync 系统调用分析

## 1. 概述

将文件所有数据和元数据同步到磁盘（完整同步）。与 `fdatasync` 的区别在于，`fsync` 会同步所有元数据（包括 atime、mtime 等），确保文件完全持久化。

**原型：**

```c
SYSCALL_DEFINE1(fsync, unsigned int, fd)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的文件描述符
  - `-EIO` — I/O 错误
  - `-EINVAL` — 无效参数

## 2. 使用场景

- **数据库事务提交**: 确保事务日志和数据文件完全持久化
- **关键文件写入**: 配置文件、密码文件等写入后立即同步
- **邮件系统**: 确保邮件投递后文件已写入磁盘

## 3. 函数调用栈

```
fsync(fd) (系统调用入口)
└─ do_fsync(fd, 0)                                    // fs/sync.c (datasync=0)
   ├─ fdget(fd)                                        // 获取 fd 对应的 file 结构
   ├─ file->f_op->fsync(file, start, end, 0)           // datasync=0
   │    └─ ext4_sync_file(file, 0)                     // ext4 实现
   │         ├─ filemap_write_and_wait_range()          // 等待脏页写回
   │         ├─ ext4_sync_inode(file, 0)               // 同步完整 inode 元数据
   │         │    └─ datasync=0 → 同步所有元数据
   │         └─ jbd2 事务提交
   └─ fdput(fd)                                        // 释放 fd 引用
```

## 4. 关键数据结构

{SYNC_DATA_STRUCTURES}

## 5. 流程图

```
fsync(fd)
  │
  v
do_fsync(fd, 0)  // datasync=0
  │
  ├─ fdget(fd) → 获取 file 对象
  │
  └─ ext4_sync_file(file, 0)
       │
       ├─ filemap_write_and_wait_range()
       │    ├─ 遍历脏页链表
       │    └─ 提交写回请求
       │
       ├─ ext4_sync_inode(file, 0)
       │    └─ 同步完整元数据（包括 atime/mtime）
       │
       └─ jbd2 事务提交
            └─ 等待日志刷写到磁盘
```

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    int fd = open("/tmp/test.dat", O_WRONLY | O_CREAT, 0644);
    if (fd == -1) { perror("open"); return 1; }

    const char *data = "critical data";
    write(fd, data, strlen(data));

    // 完整同步（数据和所有元数据）
    if (fsync(fd) == -1) {
        perror("fsync");
        return 1;
    }

    printf("Data and metadata synced\\n");
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — fsync/fdatasync 实现
- `fs/ext4/fsync.c` — ext4 同步实现
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "fsync.md"), content)


def gen_sync():
    content = """# sync 系统调用分析

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

## 4. 关键数据结构

{SYNC_DATA_STRUCTURES}

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

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    // 同步所有文件系统
    sync();
    printf("All file systems synced\\n");
    return 0;
}
```

## 7. 参考

- `fs/sync.c` — sync 实现
- `include/linux/fs.h` — super_block 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "sync.md"), content)


def gen_mount():
    content = """# mount 系统调用分析

## 1. 概述

挂载文件系统。将设备上的文件系统关联到目录树的某个挂载点，使文件和目录可访问。

**原型：**

```c
SYSCALL_DEFINE5(mount, const char __user *, dev_name, const char __user *, dir_name,
        const char __user *, type, unsigned long, flags, void __user *, data)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dev_name` | `const char *` | 设备名（如 `/dev/sda1`）或伪文件系统名（如 `proc`） |
| `dir_name` | `const char *` | 挂载点路径 |
| `type` | `const char *` | 文件系统类型（如 `ext4`、`xfs`、`tmpfs`） |
| `flags` | `unsigned long` | 挂载标志（`MS_RDONLY`、`MS_NOEXEC` 等） |
| `data` | `void *` | 文件系统特定选项 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-ENOENT` — 设备或挂载点不存在
  - `-EBUSY` — 设备或挂载点正忙
  - `-EINVAL` — 无效参数

## 2. 使用场景

- **系统启动**: 挂载根文件系统
- **外部存储**: 挂载 USB 设备、SD 卡
- **虚拟文件系统**: 挂载 procfs、sysfs、tmpfs、devtmpfs
- **容器**: 挂载容器文件系统

## 3. 函数调用栈

```
mount(dev_name, dir_name, type, flags, data) (系统调用入口)
└─ ksys_mount(dev_name, dir_name, type, flags, data)  // fs/namespace.c
   └─ do_mount(dev_name, dir_name, type, flags, data)  // 核心挂载逻辑
        ├─ user_path_at(AT_FDCWD, dir_name, LOOKUP_FOLLOW, &path)
        │                                                 // 解析挂载点路径
        ├─ [权限检查]
        │  ├─ capable(CAP_SYS_ADMIN)                     // 需要 CAP_SYS_ADMIN
        │  └─ inode_permission(inode, MAY_WRITE)         // 挂载点需要可写
        │
        ├─ [根据 flags 分发]
        │  ├─ MS_REMOUNT    → do_remount()               // 重新挂载
        │  ├─ MS_BIND       → do_loopback()              // 绑定挂载
        │  ├─ MS_MOVE       → do_move_mount()            // 移动挂载
        │  └─ [默认]        → do_new_mount()             // 全新挂载
        │
        └─ do_new_mount(&path, type, flags, options, dev_name)
             ├─ type = get_fs_type(type)                 // 查找文件系统类型
             ├─ sb = sget_fc(fc, ...)                     // 获取或创建超级块
             └─ do_add_mount(sb, path, mnt_flags)         // 添加到挂载树
```

## 4. 关键数据结构

{MOUNT_DATA_STRUCTURES}

## 5. 流程图

```
mount("/dev/sda1", "/mnt", "ext4", 0, NULL)
  │
  v
do_mount()
  │
  ├─ 解析挂载点路径 → dentry + mnt
  │
  ├─ 权限检查 (CAP_SYS_ADMIN)
  │
  └─ do_new_mount()
       │
       ├─ get_fs_type("ext4")  → 查找 ext4 文件系统驱动
       │
       ├─ vfs_get_tree(fc)     → 读取超级块
       │    └─ ext4_fill_super()  → 读取 ext4 超级块
       │
       └─ do_add_mount()       → 挂载到命名空间
            ├─ graft_tree()    → 挂载树嫁接
            └─ mnt_id 分配
```

## 6. 使用示例

```c
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 挂载 ext4 分区
    if (mount("/dev/sda1", "/mnt/data", "ext4", 0, NULL) == -1) {
        perror("mount");
        return 1;
    }
    printf("/dev/sda1 mounted to /mnt/data\\n");
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — mount 核心实现
- `include/linux/mount.h` — 挂载结构定义
- `include/uapi/linux/mount.h` — 用户态挂载标志
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "mount.md"), content)


def gen_pivot_root():
    content = """# pivot_root 系统调用分析

## 1. 概述

将当前进程的根文件系统移动到指定目录，并设置新的根文件系统。用于容器技术中切换进程的根文件系统。

**原型：**

```c
SYSCALL_DEFINE2(pivot_root, const char __user *, new_root, const char __user *, put_old)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `new_root` | `const char *` | 新的根文件系统路径 |
| `put_old` | `const char *` | 旧根文件系统移动到的目录 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-EBUSY` — 挂载点正忙
  - `-EINVAL` — 无效参数（如 new_root 不在当前根下）

## 2. 使用场景

- **容器运行时**: Docker/containerd 切换容器根文件系统
- **initramfs 过渡**: 从 initramfs 切换到真实根文件系统
- **系统安装**: 安装过程中切换根文件系统

## 3. 函数调用栈

```
pivot_root(new_root, put_old) (系统调用入口)
└─ ksys_pivot_root(new_root, put_old)                 // fs/namespace.c
   ├─ user_path_at(AT_FDCWD, new_root, ...)            // 解析新根路径
   ├─ user_path_at(AT_FDCWD, put_old, ...)             // 解析旧根存放路径
   │
   ├─ [权限检查]
   │  ├─ capable(CAP_SYS_ADMIN)                        // 需要 CAP_SYS_ADMIN
   │  └─ 检查 new_root 是挂载点
   │
   ├─ attach_mnt_tree(root_mnt, old_mnt)               // 移动旧根到 put_old
   │
   ├─ set_fs_root(current->fs, &new_root_path)         // 更新进程根目录
   │
   └─ chroot_fs_refs()                                 // 更新命名空间引用
```

## 4. 关键数据结构

{MOUNT_DATA_STRUCTURES}

## 5. 流程图

```
pivot_root(new_root, put_old)
  │
  ├─ 解析 new_root 和 put_old 路径
  │
  ├─ 权限检查 (CAP_SYS_ADMIN)
  │
  ├─ 将当前根移动到 put_old
  │
  ├─ 将 new_root 设置为新的根
  │
  └─ 更新进程 fs_struct 和挂载命名空间
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 准备新根目录（需要 root 权限）
    // mkdir -p /newroot /put_old

    // 切换根文件系统
    if (syscall(SYS_pivot_root, "/newroot", "/put_old") == -1) {
        perror("pivot_root");
        return 1;
    }

    // 确保 /put_old 不在新根下
    // mount --bind /put_old /put_old
    // pivot_root /newroot /put_old

    printf("Root filesystem changed\\n");
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — pivot_root 实现
- `include/linux/mount.h` — 挂载结构定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "pivot_root.md"), content)


def gen_quotactl():
    content = """# quotactl 系统调用分析

## 1. 概述

磁盘配额管理，控制用户/组对磁盘空间的使用。通过指定设备路径和命令来管理配额。

**原型：**

```c
SYSCALL_DEFINE4(quotactl, unsigned int, cmd, const char __user *, special,
        qid_t, id, void __user *, addr)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `cmd` | `unsigned int` | 配额命令（Q_QUOTAON/Q_QUOTAOFF 等） |
| `special` | `const char *` | 块设备路径（如 `/dev/sda1`） |
| `id` | `qid_t` | 用户/组 ID |
| `addr` | `void *` | 配额数据缓冲区地址 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 权限不足
  - `-EINVAL` — 无效命令
  - `-ENOENT` — 设备不存在

## 2. 使用场景

- **多用户服务器**: 限制用户磁盘使用量
- **共享主机**: 防止单个用户耗尽磁盘空间
- **配额管理工具**: `quota`、`edquota`、`repquota` 命令

## 3. 函数调用栈

```
quotactl(cmd, special, id, addr) (系统调用入口)
└─ ksys_quotactl(cmd, special, id, addr)              // fs/quota/quota.c
   ├─ sb = quotactl_block(special)                     // 通过设备路径找到超级块
   │
   └─ do_quotactl(sb, cmd, id, addr)                  // 执行配额操作
        ├─ [cmd 分发]
        │  ├─ Q_QUOTAON  → quota_on(sb, type, ...)     // 启用配额
        │  ├─ Q_QUOTAOFF → quota_off(sb, type)         // 停用配额
        │  ├─ Q_GETQUOTA → dqget(sb, id, type) → 读取
        │  ├─ Q_SETQUOTA → dqget + dqcommit → 写入
        │  └─ Q_SYNC     → sync_filesystem(sb)         // 同步
        └─ ...
```

## 4. 关键数据结构

{QUOTA_DATA_STRUCTURES}

## 5. 流程图

```
quotactl(Q_GETQUOTA, "/dev/sda1", 1000, &dqblk)
  │
  ├─ 通过设备路径找到超级块
  │
  └─ do_quotactl(sb, cmd, id, addr)
       │
       ├─ Q_QUOTAON  → 读取配额文件 (aquota.user/aquota.group)
       ├─ Q_QUOTAOFF → 关闭配额跟踪
       ├─ Q_GETQUOTA → 返回用户/组的配额限制和使用量
       ├─ Q_SETQUOTA → 设置用户/组的配额限制
       └─ Q_SYNC     → 同步配额文件到磁盘
```

## 6. 使用示例

```c
#include <sys/quota.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dqblk quota;
    // 获取用户 1000 在 /dev/sda1 上的配额
    if (quotactl(Q_GETQUOTA, "/dev/sda1", 1000, (void *)&quota) == -1) {
        perror("quotactl");
        return 1;
    }
    printf("User 1000 quota on /dev/sda1:\\n");
    printf("  Current space: %lld\\n", quota.dqb_curspace);
    printf("  Soft limit: %lld\\n", quota.dqb_bsoftlimit);
    printf("  Hard limit: %lld\\n", quota.dqb_bhardlimit);
    return 0;
}
```

## 7. 参考

- `fs/quota/quota.c` — quotactl 实现
- `include/linux/quota.h` — 配额数据结构
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "quotactl.md"), content)


def gen_umount():
    content = """# umount 系统调用分析

## 1. 概述

卸载文件系统。这是传统接口，在 ARM64 上无独立系统调用编号，通过 `umount2`（syscall #39）实现。在新代码中应使用 `umount2`。

**原型：**

```c
SYSCALL_DEFINE1(umount, const char __user *, name)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `name` | `const char *` | 挂载点路径 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-EBUSY` — 文件系统正忙
  - `-EINVAL` — 无效路径

## 2. 使用场景

- **系统管理**: 卸载不需要的文件系统
- **外部存储**: 安全移除 USB 设备前卸载
- **兼容性**: 旧代码中使用的传统接口

## 3. 函数调用栈

```
umount(name) (系统调用入口)
└─ ksys_umount(name, 0)                                // fs/namespace.c
   └─ path_umount(path, flags)                         // flags=0
        ├─ user_path_mountpoint_at(AT_FDCWD, name, ...) // 查找挂载点
        ├─ may_mount()                                  // 权限检查
        └─ do_umount(mnt, 0)                            // 执行卸载
             ├─ may_umount(mnt)                         // 检查使用情况
             ├─ sb->s_op->sync_fs(sb, 1)                // 同步超级块
             └─ do_umount_root(mnt)                     // 从挂载树移除
```

## 4. 关键数据结构

{MOUNT_DATA_STRUCTURES}

## 5. 流程图

```
umount("/mnt")
  │
  ├─ 查找挂载点
  ├─ 权限检查 (CAP_SYS_ADMIN)
  └─ do_umount()
       ├─ 检查是否有进程在使用
       ├─ 同步文件系统
       └─ 从挂载树移除
```

## 6. 使用示例

```c
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 卸载文件系统
    if (umount("/mnt/data") == -1) {
        perror("umount");
        return 1;
    }
    printf("/mnt/data unmounted\\n");
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — umount/umount2 实现
- `include/linux/mount.h` — 挂载结构定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "umount.md"), content)


def gen_umount2():
    content = """# umount2 系统调用分析

## 1. 概述

卸载文件系统（增强版）。支持 `MNT_FORCE`、`MNT_DETACH`、`MNT_EXPIRE` 等标志，比 `umount` 提供更灵活的卸载控制。

**原型：**

```c
SYSCALL_DEFINE2(umount2, const char __user *, name, int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `name` | `const char *` | 挂载点路径 |
| `flags` | `int` | 卸载标志 |

**flags 标志：**

| 标志 | 值 | 说明 |
|------|-----|------|
| `MNT_FORCE` | 1 | 强制卸载（即使设备忙） |
| `MNT_DETACH` | 2 | 延迟卸载（立即分离，进程退出后清理） |
| `MNT_EXPIRE` | 4 | 过期标记卸载（仅当上次访问后未使用） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-EBUSY` — 文件系统正忙（未使用 MNT_FORCE）
  - `-EINVAL` — 无效参数

## 2. 使用场景

- **强制卸载**: 卸载 NFS 挂载点（服务器离线时）
- **延迟卸载**: 安全移除正在使用的设备
- **自动挂载管理**: 过期自动卸载（autofs）

## 3. 函数调用栈

```
umount2(name, flags) (系统调用入口)
└─ ksys_umount(name, flags)                            // fs/namespace.c
   └─ path_umount(path, flags)
        ├─ user_path_mountpoint_at(AT_FDCWD, name, ...) // 查找挂载点
        ├─ may_mount()                                  // 权限检查
        │
        └─ do_umount(mnt, flags)
             ├─ [flags & MNT_FORCE] → 强制卸载
             │    ├─ sb->s_op->umount_begin(sb)          // 通知文件系统
             │    └─ 绕过使用检查
             │
             ├─ [flags & MNT_DETACH] → 延迟卸载
             │    ├─ __detach_mounts(mnt)                // 立即分离
             │    └─ 引用计数归零后清理
             │
             └─ [flags & MNT_EXPIRE] → 过期卸载
                  └─ 检查是否在过期时间内未被使用
```

## 4. 关键数据结构

{MOUNT_DATA_STRUCTURES}

## 5. 流程图

```
umount2("/mnt", MNT_DETACH)
  │
  ├─ 查找挂载点
  │
  ├─ 权限检查 (CAP_SYS_ADMIN)
  │
  └─ do_umount(mnt, flags)
       │
       ├─ MNT_FORCE → 强制卸载
       │    └─ 即使有进程使用也强制分离
       │
       ├─ MNT_DETACH → 延迟卸载
       │    ├─ 立即从挂载树分离
       │    └─ 最后一个引用释放后清理
       │
       └─ MNT_EXPIRE → 过期卸载
            └─ 仅当挂载点未被访问时才卸载
```

## 6. 使用示例

```c
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 延迟卸载（安全移除正在使用的设备）
    if (umount2("/mnt/usb", MNT_DETACH) == -1) {
        perror("umount2");
        return 1;
    }
    printf("/mnt/usb detached (will clean up later)\\n");

    // 强制卸载 NFS 挂载
    if (umount2("/mnt/nfs", MNT_FORCE) == -1) {
        perror("umount2 force");
        return 1;
    }
    printf("/mnt/nfs force unmounted\\n");
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — umount2 实现
- `include/uapi/linux/mount.h` — MNT_* 标志定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)"""
    write_file(os.path.join(FS_DIR, "umount2.md"), content)


# ============================================================
# Section 5.6 - 目录与路径操作
# ============================================================

def gen_flock():
    content = """# flock 系统调用分析

## 1. 概述

在打开的文件上施加或释放建议性文件锁（advisory lock）。与 POSIX 记录锁（fcntl）不同，flock 锁整个文件且不可与 fcntl 锁混用。

**原型：**

```c
SYSCALL_DEFINE2(flock, unsigned int, fd, unsigned int, cmd)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符 |
| `cmd` | `unsigned int` | 操作命令 |

**cmd 命令：**

| 命令 | 说明 |
|------|------|
| `LOCK_SH` | 共享锁（多个进程可同时持有） |
| `LOCK_EX` | 排他锁（仅一个进程可持有） |
| `LOCK_UN` | 释放锁 |
| `LOCK_NB` | 非阻塞模式（与 LOCK_SH 或 LOCK_EX 组合使用） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EBADF` — 无效的文件描述符
  - `-EINVAL` — 无效命令
  - `-EWOULDBLOCK` — 非阻塞模式下无法获取锁

## 2. 使用场景

- **进程同步**: 多个进程协调对同一文件的访问
- **配置文件**: 防止并发修改配置文件
- **守护进程**: 确保单实例运行（如 PID 文件加锁）

## 3. 函数调用栈

```
flock(fd, cmd) (系统调用入口)
└─ ksys_flock(fd, cmd)                                 // fs/locks.c
   └─ flock_make_lock(file, &fl, type)                  // 创建文件锁结构
        └─ locks_lock_file_wait(file, &fl)              // 获取锁（可能阻塞）
             └─ locks_lock_inode_wait(BF_INODE(file), &fl)
                  └─ __locks_lock_inode(inode, &fl)
                       └─ posix_lock_inode(inode, &fl)  // 实际锁操作
                            ├─ locks_find_conflict()    // 查找冲突锁
                            └─ locks_insert_lock()      // 插入锁记录
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
flock(fd, LOCK_EX)
  │
  ├─ 创建 file_lock 结构 (LOCK_EX 类型)
  │
  └─ locks_lock_file_wait()
       │
       ├─ locks_find_conflict() → 检查冲突锁
       │    ├─ 无冲突 → 插入锁记录
       │    └─ 有冲突 → 等待 (LOCK_NB 则返回 EWOULDBLOCK)
       │
       └─ 锁释放后唤醒等待进程
```

## 6. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>

int main(void)
{
    int fd = open("/tmp/lockfile", O_RDWR | O_CREAT, 0644);
    if (fd == -1) { perror("open"); return 1; }

    // 获取排他锁（阻塞模式）
    if (flock(fd, LOCK_EX) == -1) {
        perror("flock");
        return 1;
    }
    printf("Lock acquired\\n");

    // 临界区操作...
    sleep(5);

    // 释放锁
    flock(fd, LOCK_UN);
    printf("Lock released\\n");
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/locks.c` — flock 实现
- `include/linux/fs.h` — file_lock 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "flock.md"), content)


def gen_getcwd():
    content = """# getcwd 系统调用分析

## 1. 概述

获取当前工作目录的绝对路径。

**原型：**

```c
SYSCALL_DEFINE2(getcwd, char __user *, buf, unsigned long, size)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `buf` | `char *` | 存放路径的缓冲区 |
| `size` | `unsigned long` | 缓冲区大小 |

**返回值：**

- 成功返回路径字符串长度（包括结尾的 `\\0`）
- 失败返回负值错误码：
  - `-ERANGE` — 缓冲区太小
  - `-EFAULT` — 用户态指针无效

## 2. 使用场景

- **`pwd` 命令**: 获取当前工作目录
- **路径管理**: 程序需要保存或显示当前目录
- **日志记录**: 记录进程的工作目录

## 3. 函数调用栈

```
getcwd(buf, size) (系统调用入口)
└─ ksys_getcwd(buf, size)                              // fs/d_path.c
   └─ d_path(&current->fs->pwd, buf, size)             // 将路径转换为字符串
        └─ prepend_path()                               // 从 dentry 向上遍历到根
             ├─ 从当前 dentry 开始
             ├─ 逐级获取父 dentry 和分量名
             └─ 拼接到缓冲区前面
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
getcwd(buf, size)
  │
  └─ d_path(&current->fs->pwd, buf, size)
       │
       ├─ 从当前 dentry 开始
       ├─ 逐级向上遍历父目录
       │    ├─ 获取每个分量名
       │    └─ prepend 到缓冲区前面
       └─ 到达根 dentry 后返回
```

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 自动分配缓冲区
    char *cwd = getcwd(NULL, 0);
    if (cwd == NULL) {
        perror("getcwd");
        return 1;
    }
    printf("Current directory: %s\\n", cwd);
    free(cwd);
    return 0;
}
```

## 7. 参考

- `fs/d_path.c` — getcwd 实现
- `include/linux/fs_struct.h` — fs_struct 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "getcwd.md"), content)


def gen_ioctl():
    content = """# ioctl 系统调用分析

## 1. 概述

设备 I/O 控制。向设备驱动程序发送控制命令，执行标准的 read/write 无法完成的操作。

**原型：**

```c
SYSCALL_DEFINE3(ioctl, unsigned int, fd, unsigned int, cmd, unsigned long, arg)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 文件描述符 |
| `cmd` | `unsigned int` | 设备特定命令码 |
| `arg` | `unsigned long` | 命令参数（通常为指针） |

**返回值：**

- 成功返回 `0`（部分命令可能返回正数）
- 失败返回负值错误码：
  - `-EBADF` — 无效的 fd
  - `-ENOTTY` — fd 不支持 ioctl
  - `-EINVAL` — 无效命令

## 2. 使用场景

- **终端控制**: `TIOCGWINSZ` 获取终端大小
- **网络设备**: `SIOCGIFADDR` 获取网络接口地址
- **块设备**: `BLKGETSIZE64` 获取设备大小
- **驱动控制**: 自定义设备驱动命令

## 3. 函数调用栈

```
ioctl(fd, cmd, arg) (系统调用入口)
└─ ksys_ioctl(fd, cmd, arg)                            // fs/ioctl.c
   └─ do_vfs_ioctl(file, cmd, arg)                     // 分发 ioctl 请求
        ├─ [通用 cmd 处理]
        │  ├─ FIONCLEX → 清除 close-on-exec 标志
        │  ├─ FIOCLEX  → 设置 close-on-exec 标志
        │  ├─ FIONBIO  → 设置非阻塞 I/O
        │  ├─ FIOASYNC → 设置异步 I/O
        │  └─ FIONREAD → 获取可读字节数
        │
        └─ vfs_ioctl(file, cmd, arg)                   // 设备特定 ioctl
             └─ file->f_op->unlocked_ioctl(file, cmd, arg) // 驱动实现
                  └─ 设备驱动 switch(cmd) 处理
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
ioctl(fd, cmd, arg)
  │
  v
do_vfs_ioctl(file, cmd, arg)
  │
  ├─ 是通用 cmd?
  │    ├─ FIONCLEX/FIOCLEX → 设置 close-on-exec
  │    ├─ FIONBIO → 设置非阻塞
  │    └─ FIONREAD → 返回可读字节数
  │
  └─ 否 → vfs_ioctl()
       └─ file->f_op->unlocked_ioctl(file, cmd, arg)
            └─ 设备驱动 switch cmd 处理
```

## 6. 使用示例

```c
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/fs.h>

int main(void)
{
    int fd = open("/dev/nvme0n1", O_RDONLY);
    if (fd == -1) { perror("open"); return 1; }

    unsigned long long size;
    // 获取块设备大小
    if (ioctl(fd, BLKGETSIZE64, &size) == -1) {
        perror("ioctl");
        return 1;
    }
    printf("Device size: %llu bytes\\n", size);
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/ioctl.c` — ioctl 实现
- `include/uapi/linux/fs.h` — 通用 ioctl 命令定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "ioctl.md"), content)


def gen_name_to_handle_at():
    content = """# name_to_handle_at 系统调用分析

## 1. 概述

获取文件句柄（file handle），用于 NFS 风格的持久化文件引用。与 `open_by_handle_at` 配对使用，可在重启后通过句柄重新打开文件。

**原型：**

```c
SYSCALL_DEFINE5(name_to_handle_at, int, dfd, const char __user *, name,
        struct file_handle __user *, handle, int __user *, mnt_id, int, flag)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd |
| `name` | `const char *` | 文件路径 |
| `handle` | `struct file_handle *` | 输出：文件句柄 |
| `mnt_id` | `int *` | 输出：挂载点 ID |
| `flag` | `int` | 路径解析标志（`AT_EMPTY_PATH` 等） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EFAULT` — 用户态指针无效
  - `-EINVAL` — 无效参数
  - `-ENOENT` — 文件不存在

## 2. 使用场景

- **NFS 文件句柄**: 获取 NFS 导出的文件句柄
- **文件系统备份**: 备份时保存文件句柄
- **文件系统检查**: 通过 inode 号引用文件

## 3. 函数调用栈

```
name_to_handle_at(dfd, name, handle, mnt_id, flag) (系统调用入口)
└─ ksys_name_to_handle_at(dfd, name, handle, mnt_id, flag)  // fs/fhandle.c
   ├─ getname(name)                                      // 拷贝文件名
   ├─ path = user_path_at(dfd, name, flag, &path)        // 路径解析
   │
   ├─ [获取句柄]
   │  └─ exportfs_encode_fh(path.dentry, fh, &handle_size, ...)  // 编码句柄
   │       └─ dentry->d_sb->s_export_op->encode_fh()            // 文件系统实现
   │            └─ [ext4] → ext4_encode_fh()
   │                 └─ 编码 inode 号 + 生成数
   │
   └─ copy_to_user(handle, &fh, ...)                     // 拷贝到用户态
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
name_to_handle_at(AT_FDCWD, "/tmp/test", handle, &mnt_id, 0)
  │
  ├─ 解析路径 → dentry
  │
  └─ exportfs_encode_fh(dentry)
       └─ ext4_encode_fh(dentry)
            ├─ 从 dentry 获取 inode 号
            ├─ 获取文件系统 UUID
            └─ 编码到 file_handle 结构
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/fs.h>

int main(void)
{
    struct file_handle *handle;
    handle = malloc(sizeof(struct file_handle) + 64);
    handle->handle_bytes = 64;
    int mount_id;

    if (name_to_handle_at(AT_FDCWD, "/tmp/test",
                          handle, &mount_id, 0) == -1) {
        perror("name_to_handle_at");
        return 1;
    }
    printf("File handle obtained, mount_id=%d\\n", mount_id);
    free(handle);
    return 0;
}
```

## 7. 参考

- `fs/fhandle.c` — name_to_handle_at 实现
- `include/uapi/linux/fs.h` — file_handle 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "name_to_handle_at.md"), content)


def gen_open():
    content = """# open 系统调用分析

## 1. 概述

打开或创建文件，返回文件描述符。这是最基础的文件打开接口，在新代码中推荐使用 `openat` 或 `openat2`。

**原型：**

```c
SYSCALL_DEFINE3(open, const char __user *, filename, int, flags, umode_t, mode)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `filename` | `const char *` | 文件路径 |
| `flags` | `int` | O_* 打开标志 |
| `mode` | `umode_t` | 创建模式（O_CREAT 时有效） |

**返回值：**

- 成功返回文件描述符（非负整数）
- 失败返回负值错误码：
  - `-EACCES` — 权限不足
  - `-ENOENT` — 文件不存在
  - `-EEXIST` — 文件已存在（O_CREAT | O_EXCL）
  - `-EINVAL` — 无效标志

## 2. 使用场景

- **文件读写**: 打开文件进行 read/write 操作
- **文件创建**: 使用 O_CREAT 创建新文件
- **设备访问**: 打开设备文件

## 3. 函数调用栈

```
open(filename, flags, mode) (系统调用入口)
└─ ksys_open(filename, flags, mode)                    // fs/open.c
   └─ do_sys_open(AT_FDCWD, filename, flags, mode)     // AT_FDCWD 表示当前目录
        └─ do_sys_openat2(AT_FDCWD, filename, &how)    // 核心入口
             └─ do_filp_open(AT_FDCWD, tmp, &op)       // 打开文件
                  └─ path_openat(nd, file, op)          // 路径解析+打开
                       ├─ path_init(nd, AT_FDCWD, name)  // 初始化路径查找
                       ├─ link_path_walk(name, nd)       // 逐分量解析路径
                       │    └─ walk_component(nd, ...)    // 解析单个路径分量
                       ├─ do_last(nd, file, op)          // 打开最后一个分量
                       │    ├─ lookup_open(nd, file, op)  // 查找/创建 dentry
                       │    └─ vfs_open(nd, file)         // VFS 打开文件
                       └─ fd_install(file, fd)            // 安装 fd
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
open("/tmp/test.txt", O_RDWR | O_CREAT, 0644)
  │
  v
do_sys_openat2(AT_FDCWD, "tmp/test.txt", &how)
  │
  └─ path_openat()
       │
       ├─ path_init() → 初始化路径查找上下文
       │
       ├─ link_path_walk() → 逐分量解析路径
       │    ├─ "tmp" → lookup 找到 dentry
       │    └─ "test.txt" → do_last()
       │
       └─ do_last()
            ├─ lookup_open() → 查找或创建 dentry
            ├─ vfs_open() → 调用文件系统 open
            └─ fd_install() → 分配 fd
```

## 6. 使用示例

```c
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/tmp/test.txt", O_RDWR | O_CREAT, 0644);
    if (fd == -1) { perror("open"); return 1; }

    write(fd, "hello", 5);
    close(fd);
    printf("File written\\n");
    return 0;
}
```

## 7. 参考

- `fs/open.c` — open/openat/openat2 实现
- `include/uapi/asm-generic/fcntl.h` — O_* 标志定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "open.md"), content)


def gen_openat():
    content = """# openat 系统调用分析

## 1. 概述

通过目录 fd 打开文件。与 `open` 的区别在于可以指定路径解析的起始目录，避免 TOCTOU 竞争条件。

**原型：**

```c
SYSCALL_DEFINE4(openat, int, dfd, const char __user *, filename,
        int, flags, umode_t, mode)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd（`AT_FDCWD` 表示当前目录） |
| `filename` | `const char *` | 文件路径 |
| `flags` | `int` | O_* 打开标志 |
| `mode` | `umode_t` | 创建模式（O_CREAT 时有效） |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码

## 2. 使用场景

- **相对路径打开**: 相对于已打开的目录 fd 打开文件
- **避免竞争**: 避免 TOCTOU（time-of-check-time-of-use）竞争
- **容器**: 通过容器根目录 fd 打开文件

## 3. 函数调用栈

```
openat(dfd, filename, flags, mode) (系统调用入口)
└─ ksys_openat(dfd, filename, flags, mode)             // fs/open.c
   └─ do_sys_openat2(dfd, filename, &how)              // 核心入口
        └─ do_filp_open(dfd, tmp, &op)                 // 打开文件
             └─ path_openat(nd, file, op)              // 路径解析+打开
                  ├─ path_init(nd, dfd, name)           // 从 dfd 开始
                  ├─ link_path_walk(name, nd)           // 逐分量解析
                  └─ do_last(nd, file, op)              // 打开最后分量
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
openat(dirfd, "test.txt", O_RDWR, 0)
  │
  ├─ 从 dirfd 指向的目录开始解析路径
  │
  └─ path_openat()
       ├─ path_init(nd, dirfd, "test.txt")
       ├─ link_path_walk → 解析 "test.txt"
       └─ do_last → lookup_open + vfs_open + fd_install
```

## 6. 使用示例

```c
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int dir_fd = open("/tmp", O_RDONLY);
    if (dir_fd == -1) { perror("open dir"); return 1; }

    // 相对于 /tmp 打开文件
    int fd = openat(dir_fd, "test.txt", O_RDWR | O_CREAT, 0644);
    if (fd == -1) { perror("openat"); return 1; }

    write(fd, "hello", 5);
    close(fd);
    close(dir_fd);
    return 0;
}
```

## 7. 参考

- `fs/open.c` — openat 实现
- `include/uapi/asm-generic/fcntl.h` — AT_* 标志定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "openat.md"), content)


def gen_openat2():
    content = """# openat2 系统调用分析

## 1. 概述

扩展的打开文件接口。通过 `struct open_how` 结构提供更精细的路径解析控制，支持安全路径解析（如禁止符号链接、禁止跨设备等）。

**原型：**

```c
SYSCALL_DEFINE4(openat2, int, dfd, const char __user *, filename,
        struct open_how __user *, how, size_t, usize)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd |
| `filename` | `const char *` | 文件路径 |
| `how` | `struct open_how *` | 打开参数（flags + mode + resolve） |
| `usize` | `size_t` | how 结构体大小 |

**resolve 标志：**

| 标志 | 说明 |
|------|------|
| `RESOLVE_NO_XDEV` | 禁止跨设备 |
| `RESOLVE_NO_MAGICLINKS` | 禁止 magic 符号链接（如 `/proc/self/fd/`） |
| `RESOLVE_NO_SYMLINKS` | 禁止符号链接 |
| `RESOLVE_BENEATH` | 限制在 dfd 目录下 |
| `RESOLVE_IN_ROOT` | 以根目录为锚点 |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码

## 2. 使用场景

- **安全文件访问**: 防止路径遍历攻击
- **沙箱**: 限制文件访问范围
- **容器**: 确保文件访问不逃逸出容器根目录

## 3. 函数调用栈

```
openat2(dfd, filename, how, usize) (系统调用入口)
└─ ksys_openat2(dfd, filename, how, usize)             // fs/open.c
   ├─ copy_from_user(&tmp, how, usize)                  // 拷贝用户参数
   ├─ build_open_how(&tmp, &how)                        // 校验并构建 open_how
   │
   └─ do_sys_openat2(dfd, filename, &how)              // 核心入口
        └─ do_filp_open(dfd, tmp, &op)                 // 打开文件
             └─ path_openat(nd, file, op)              // 路径解析+打开
                  ├─ path_init(nd, dfd, name)           // 从 dfd 开始
                  ├─ [resolve 标志检查]
                  │  ├─ RESOLVE_NO_SYMLINKS → 禁止符号链接
                  │  ├─ RESOLVE_BENEATH → 检查路径在 dfd 下
                  │  └─ RESOLVE_NO_XDEV → 禁止跨设备
                  ├─ link_path_walk(name, nd)           // 逐分量解析
                  └─ do_last(nd, file, op)              // 打开最后分量
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
openat2(dirfd, "test.txt", {O_RDWR, 0, RESOLVE_BENEATH}, sizeof(open_how))
  │
  ├─ copy_from_user → 获取 open_how 参数
  │
  └─ do_sys_openat2()
       └─ path_openat()
            ├─ path_init(nd, dirfd, "test.txt")
            │
            ├─ [RESOLVE_BENEATH] → 检查路径不逃逸 dirfd
            │
            ├─ link_path_walk → 解析 "test.txt"
            │    └─ [RESOLVE_NO_SYMLINKS] → 禁止符号链接
            │
            └─ do_last → lookup_open + vfs_open + fd_install
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    struct open_how how = {
        .flags = O_RDWR | O_CREAT,
        .mode = 0644,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS,
    };
    int fd;
    // 使用 syscall 直接调用（glibc 可能未封装）
    fd = syscall(SYS_openat2, AT_FDCWD, "/tmp/test.txt", &how, sizeof(how));
    if (fd < 0) { perror("openat2"); return 1; }

    write(fd, "hello", 5);
    close(fd);
    printf("File opened with openat2\\n");
    return 0;
}
```

## 7. 参考

- `fs/open.c` — openat2 实现
- `include/uapi/linux/openat2.h` — open_how 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "openat2.md"), content)


def gen_open_tree():
    content = """# open_tree 系统调用分析

## 1. 概述

获取一个挂载点的文件描述符。这是 Linux 5.2+ 新挂载 API 的一部分，返回的 fd 可用于后续的 `move_mount()` 等操作。

**原型：**

```c
SYSCALL_DEFINE3(open_tree, int, dfd, const char __user *, filename, unsigned, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `dfd` | `int` | 目录 fd |
| `filename` | `const char *` | 挂载点路径 |
| `flags` | `unsigned int` | 打开标志（`OPEN_TREE_CLONE` 等） |

**flags 标志：**

| 标志 | 说明 |
|------|------|
| `OPEN_TREE_CLONE` | 克隆挂载树（创建新挂载实例，不从原挂载树分离） |
| `AT_EMPTY_PATH` | 允许 dfd 指向的路径为空 |

**返回值：**

- 成功返回文件描述符
- 失败返回负值错误码

## 2. 使用场景

- **挂载树操作**: 获取挂载点 fd 用于 `move_mount()`
- **挂载克隆**: 克隆挂载树用于创建新命名空间
- **容器管理**: 管理容器挂载命名空间

## 3. 函数调用栈

```
open_tree(dfd, filename, flags) (系统调用入口)
└─ ksys_open_tree(dfd, filename, flags)                // fs/namespace.c
   └─ vfs_open_tree(dfd, filename, flags)              // 打开挂载树
        ├─ path = user_path_at(dfd, filename, ...)      // 解析路径
        │
        ├─ [flags & OPEN_TREE_CLONE]
        │    └─ clone_mnt(old_mnt, path.dentry, flag)  // 克隆挂载
        │         ├─ alloc_vfsmnt(old_mnt)              // 分配新挂载
        │         └─ copy_mnt_ns()                      // 复制挂载命名空间
        │
        └─ [默认] → 获取挂载点 fd
             └─ anon_inode_getfd("[mount]", &mount_fops, mnt, ...)
```

## 4. 关键数据结构

{DIR_OP_DATA_STRUCTURES}

## 5. 流程图

```
open_tree(AT_FDCWD, "/mnt", OPEN_TREE_CLONE)
  │
  ├─ 解析路径 → 找到挂载点
  │
  └─ clone_mnt(old_mnt)
       ├─ alloc_vfsmnt() → 分配新的 mount 结构
       ├─ 复制挂载标志和属性
       └─ 返回指向新挂载的 fd
```

## 6. 使用示例

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(void)
{
    // 克隆 /mnt 挂载点
    int fd = syscall(SYS_open_tree, AT_FDCWD,
                     "/mnt", OPEN_TREE_CLONE);
    if (fd < 0) { perror("open_tree"); return 1; }

    printf("Open tree fd=%d\\n", fd);
    // 可用于 move_mount() 等操作
    close(fd);
    return 0;
}
```

## 7. 参考

- `fs/namespace.c` — open_tree 实现
- `include/uapi/linux/mount.h` — OPEN_TREE_* 标志定义
- [ARM64 系统调用表](../arm64-syscall-table.md#目录与路径操作)"""
    write_file(os.path.join(DP_DIR, "open_tree.md"), content)


# ============================================================
# Main
# ============================================================

def main():
    print("=== Section 5.5: 文件系统挂载与结构 ===")
    gen_acct()
    gen_chroot()
    gen_fdatasync()
    gen_fsync()
    gen_listmount()
    gen_mount()
    gen_pivot_root()
    gen_quotactl()
    gen_quotactl_fd()
    gen_statmount()
    gen_swapoff()
    gen_swapon()
    gen_sync()
    gen_sync_file_range()
    gen_syncfs()
    gen_umount()
    gen_umount2()
    print()

    print("=== Section 5.6: 目录与路径操作 ===")
    gen_flock()
    gen_getcwd()
    gen_ioctl()
    gen_name_to_handle_at()
    gen_open()
    gen_open_by_handle_at()
    gen_open_tree()
    gen_open_tree_attr()
    gen_openat()
    gen_openat2()
    print()

    print("Done! 27 files generated.")


if __name__ == "__main__":
    main()