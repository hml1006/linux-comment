# mq_open 系统调用分析

## 1. 概述

`mq_open` 用于创建或打开一个 POSIX 消息队列。消息队列通过名称（以 `/` 开头的字符串）标识，返回一个消息队列描述符供后续操作使用。

**原型：**

```c
SYSCALL_DEFINE4(mq_open, const char __user *, u_name, int, oflag,
                umode_t, mode, struct mq_attr __user *, u_attr)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `u_name` | 消息队列名称（以 `/` 开头，如 `/my_queue`） |
| `oflag` | 打开标志（O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_EXCL, O_NONBLOCK） |
| `mode` | 权限位（oflag 包含 O_CREAT 时有效） |
| `u_attr` | 队列属性（oflag 包含 O_CREAT 时可为 NULL，使用默认值） |

**oflag 标志：**

| 标志 | 说明 |
|------|------|
| `O_RDONLY` | 只读打开（仅接收） |
| `O_WRONLY` | 只写打开（仅发送） |
| `O_RDWR` | 读写打开（发送和接收） |
| `O_CREAT` | 队列不存在时创建 |
| `O_EXCL` | 与 O_CREAT 一起使用，队列已存在时返回 EEXIST |
| `O_NONBLOCK` | 非阻塞模式 |

## 3. 函数调用链

```
mq_open (系统调用入口)
  │
  ├─ [O_CREAT] copy_from_user(&attr, u_attr, sizeof(mq_attr))
  │
  └─ do_mq_open(u_name, oflag, mode, u_attr ? &attr : NULL)
       │
       └─ do_mq_open(dir, path, oflag, mode, attr)
            │
            ├─ 如果 O_CREAT:
            │    ├─ 验证 attr 参数:
            │    │    ├─ attr->mq_maxmsg > 0 且 <= DFLT_MAXMSGS
            │    │    ├─ attr->mq_msgsize > 0 且 <= DFLT_MSGSIZE
            │    │    └─ attr->mq_flags == 0
            │    │
            │    ├─ mnt_want_write(mnt)                    // 检查写权限
            │    │
            │    ├─ path_create(&dentry, &mnt, dir, path, ...)  // 创建目录项
            │    │
            │    └─ mqueue_create(mnt, dentry, oflag, mode, attr)
            │         ├─ alloc_inode_sb()                   // 分配 inode
            │         ├─ inode_init_owner(inode, ...)        // 设置所有者
            │         ├─ info = MQUEUE_I(inode)
            │         ├─ 初始化 info 字段:
            │         │    ├─ attr->mq_maxmsg, attr->mq_msgsize
            │         │    └─ msg_tree, msg_list, wait_q 等
            │         └─ d_instantiate(dentry, inode)
            │
            ├─ 如果未创建（仅打开）:
            │    └─ path_openat 等 VFS 操作
            │
            ├─ do_dentry_open(file, d_inode(dentry), NULL)  // 打开文件
            │    └─ file->f_op = &mqueue_file_operations
            │
            └─ fd_install(fd, file)                         // 安装 fd
```

## 4. 关键数据结构

### 4.1 struct mqueue_inode_info

```c
// ipc/mqueue.c
struct mqueue_inode_info {
    spinlock_t lock;                       /* 自旋锁 */
    struct inode vfs_inode;                /* VFS inode */
    struct rb_root_cached msg_tree;        /* 消息红黑树（按优先级排序） */
    struct list_head msg_list;             /* 消息链表 */
    unsigned long qsize;                   /* 当前队列总字节数 */
    unsigned long qcount;                  /* 当前消息数 */
    unsigned long q_maxsize;               /* 最大字节数 */
    unsigned long q_maxmsg;                /* 最大消息数 */
    unsigned long q_msgsize;               /* 单条消息最大大小 */
    int q_flags;                           /* 队列标志 */
    struct mq_attr attr;                   /* 队列属性 */
    struct sigevent notify;                /* 通知配置 */
    struct pid *notify_owner;              /* 通知所有者 */
    struct user_struct *user;              /* 创建用户 */
    struct sock *notify_sock;              /* 通知 socket */
    struct file *notify_file;              /* 通知文件 */
    wait_queue_head_t wait_q;              /* 等待队列 */
};
```

## 5. 流程图

```
用户态调用 mq_open(name, oflag, mode, attr)
  │
  ├── [O_CREAT] 拷贝 attr 到内核
  │
  v
do_mq_open(name, oflag, mode, attr)
  │
  ├── [O_CREAT] 创建新队列:
  │    ├── 验证 attr 有效性
  │    ├── 在 mqueue 文件系统中创建 inode
  │    ├── 初始化 mqueue_inode_info
  │    └── d_instantiate()
  │
  ├── 打开文件 (do_dentry_open)
  │    └── 设置 f_op = mqueue_file_operations
  │
  ├── fd_install() 安装文件描述符
  │
  └── 返回 mqd_t (fd)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | 名称格式错误、attr 无效（maxmsg/msgsize 为 0 或超限） |
| `EEXIST` | 队列已存在 | O_CREAT|O_EXCL 且队列已存在 |
| `ENOENT` | 队列不存在 | 未指定 O_CREAT 且队列不存在 |
| `EACCES` | 权限不足 | 无指定访问权限 |
| `ENOMEM` | 内存不足 | 无法分配 inode 或 kmem 结构 |
| `EMFILE` | 进程 fd 数超限 | 进程已打开文件数达 RLIMIT_NOFILE 上限 |
| `ENFILE` | 系统文件数超限 | 系统已打开文件数达上限 |
| `ENOSPC` | 空间不足 | mqueue 文件系统空间不足 |

## 7. 使用示例

```c
#include <mqueue.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    struct mq_attr attr = {
        .mq_flags   = 0,
        .mq_maxmsg  = 10,           // 最多 10 条消息
        .mq_msgsize = 1024,         // 每条消息最大 1024 字节
        .mq_curmsgs = 0,
    };

    // 创建消息队列
    mqd_t mq = mq_open("/my_queue", O_CREAT | O_RDWR, 0644, &attr);
    if (mq == (mqd_t)-1) {
        perror("mq_open");
        exit(1);
    }
    printf("Message queue opened: fd=%d\n", mq);

    // 以只读方式打开已存在的队列
    mqd_t mq_ro = mq_open("/my_queue", O_RDONLY);
    if (mq_ro == (mqd_t)-1) {
        perror("mq_open readonly");
        exit(1);
    }
    printf("Read-only descriptor: fd=%d\n", mq_ro);

    mq_close(mq);
    mq_close(mq_ro);
    mq_unlink("/my_queue");
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#posix-消息队列)
- 源码位置：`ipc/mqueue.c`
- 用户态头文件：`mqueue.h`、`fcntl.h`