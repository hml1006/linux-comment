# pipe2 系统调用分析

## 1. 概述

`pipe2` 用于创建一个管道，返回两个文件描述符：`fds[0]` 为读端，`fds[1]` 为写端。`pipe2` 是 `pipe` 的增强版，支持设置标志位。

**原型：**

```c
SYSCALL_DEFINE2(pipe2, int __user *, fildes, int, flags)
// 实际调用:
return do_pipe2(fildes, flags);
```

**pipe (旧版)：**

```c
SYSCALL_DEFINE1(pipe, int __user *, fildes)
{
    return do_pipe2(fildes, 0);  // 等同于 pipe2(fildes, 0)
}
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `fildes` | 输出参数，长度为 2 的 int 数组，接收读端和写端 fd |
| `flags` | 标志位（见下表） |

**flags 标志：**

| 标志 | 说明 |
|------|------|
| `O_CLOEXEC` | 设置读端和写端的 close-on-exec 标志 |
| `O_NONBLOCK` | 设置管道的非阻塞 I/O 模式 |
| `O_DIRECT` | 创建直接 I/O 管道（Linux 特有，减少数据拷贝） |
| `O_NOTIFICATION_PIPE` | 创建通知管道 |

## 3. 函数调用链

```
pipe2 (系统调用入口)
  └─ do_pipe2(fildes, flags)
       └─ __do_pipe_flags(fd, files, flags)
            ├─ flags 校验（只允许 O_CLOEXEC | O_NONBLOCK | O_DIRECT | O_NOTIFICATION_PIPE）
            │
            ├─ create_pipe_files(flags)                    // 创建管道文件
            │    ├─ get_pipe_inode()                        // 获取/创建 pipe 的 inode
            │    │    ├─ new_inode_pseudo(sb)               // 分配 inode
            │    │    ├─ inode->i_fop = &pipefifo_fops      // 设置文件操作函数表
            │    │    └─ alloc_file_pseudo(inode, ...)       // 管线文件操作
            │    ├─ alloc_file_pseudo(inode, pipe_mnt, ...) // 创建读端 file
            │    │    └─ file->f_op = &pipefifo_fops
            │    └─ alloc_file_pseudo(inode, pipe_mnt, ...) // 创建写端 file
            │         └─ file->f_op = &pipefifo_fops
            │
            ├─ get_unused_fd_flags(flags)                   // 分配读端 fd
            ├─ get_unused_fd_flags(flags)                   // 分配写端 fd
            ├─ fd_install(fds[0], files[0])                 // 安装读端 fd
            ├─ fd_install(fds[1], files[1])                 // 安装写端 fd
            │
            └─ copy_to_user(fildes, fd, sizeof(fd))         // 将 fd 数组拷贝回用户空间
```

## 4. 关键数据结构

### 4.1 struct pipe_inode_info（管道信息）

```c
// include/linux/pipe_fs_i.h
struct pipe_inode_info {
    struct mutex mutex;                /* 互斥锁，保护管道操作 */
    wait_queue_head_t rd_wait;         /* 读等待队列 */
    wait_queue_head_t wr_wait;         /* 写等待队列 */
    unsigned int head;                 /* 环形缓冲区头指针（生产者位置） */
    unsigned int tail;                 /* 环形缓冲区尾指针（消费者位置） */
    unsigned int max_usage;            /* 最大已用缓冲区数 */
    unsigned int ring_size;            /* 环形缓冲区大小（页数） */
    unsigned int nr_accounted;         /* 已记账页数 */
    unsigned int readers;              /* 读端计数（有多少个读端 fd 打开） */
    unsigned int writers;              /* 写端计数（有多少个写端 fd 打开） */
    unsigned int files;                /* 文件计数 */
    unsigned int r_counter;            /* 读端引用计数（含 fork 副本） */
    unsigned int w_counter;            /* 写端引用计数（含 fork 副本） */
    struct page *tmp_page;             /* 临时页（用于拼接） */
    struct pipe_buffer *bufs;          /* 环形缓冲区数组 */
};
```

### 4.2 struct pipe_buffer（单个缓冲区）

```c
// include/linux/pipe_fs_i.h
struct pipe_buffer {
    struct page *page;                 /* 物理页框 */
    unsigned int offset;               /* 数据在页内的偏移 */
    unsigned int len;                  /* 数据长度 */
    const struct pipe_buf_operations *ops; /* 缓冲区操作函数 */
    unsigned int flags;                /* 标志位 */
    unsigned long private;             /* 私有数据 */
};
```

### 4.3 struct file_operations pipefifo_fops（管道文件操作）

```c
// fs/pipe.c
const struct file_operations pipefifo_fops = {
    .open      = pipe_open,            // 打开管道
    .release   = pipe_release,         // 关闭管道
    .read_iter = pipe_read,            // 读操作
    .write_iter = pipe_write,          // 写操作
    .poll      = pipe_poll,            // 轮询
    .fasync    = pipe_fasync,          // 异步通知
};
```

## 5. 流程图

```
用户态调用 pipe2(fds, flags)
  │
  v
do_pipe2(fildes, flags)
  │
  v
__do_pipe_flags(fd, files, flags)
  │
  ├── 验证 flags 合法性
  │
  ├── create_pipe_files(flags)
  │    ├── get_pipe_inode()
  │    │    ├── 分配 inode
  │    │    └── 初始化 pipe_inode_info
  │    ├── alloc_file_pseudo() → 读端 file 结构
  │    └── alloc_file_pseudo() → 写端 file 结构
  │
  ├── get_unused_fd_flags() → 读端 fd
  ├── get_unused_fd_flags() → 写端 fd
  │
  ├── fd_install(fd[0], files[0])  // 安装读端
  ├── fd_install(fd[1], files[1])  // 安装写端
  │
  └── copy_to_user(fildes, fd, sizeof(fd))
       │
       └── 返回 0 (成功)
```

## 6. 数据流示意

```
     写端 fd[1]                         读端 fd[0]
         │                                  │
         │  write(fd[1], buf, n)            │  read(fd[0], buf, n)
         v                                  v
   ┌─────────────────────────────────────────────┐
   │            pipe_inode_info                   │
   │  ┌──────┬──────┬──────┬──────┬──────┬──────┐│
   │  │ buf0 │ buf1 │ buf2 │ buf3 │ ...  │ bufN ││  ← 环形缓冲区
   │  └──────┴──────┴──────┴──────┴──────┴──────┘│
   │  head → 写入位置     tail → 读取位置         │
   └─────────────────────────────────────────────┘
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | flags 包含非法标志位 |
| `EMFILE` | 进程 fd 数超限 | 进程已打开的文件描述符数达到上限 `RLIMIT_NOFILE` |
| `ENFILE` | 系统文件数超限 | 系统已打开的文件总数达到上限 |
| `EFAULT` | 地址错误 | fildes 指针不可访问 |

## 8. 使用示例

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

int main() {
    int fds[2];
    pid_t pid;

    // 创建管道（设置 close-on-exec 和非阻塞）
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == -1) {
        perror("pipe2");
        exit(1);
    }

    pid = fork();
    if (pid == 0) {
        // 子进程：关闭写端，从读端读取
        close(fds[1]);
        char buf[256];
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            buf[n] = '\0';
            printf("Child received: %s\n", buf);
        }
        close(fds[0]);
        exit(0);
    } else {
        // 父进程：关闭读端，向写端写入
        close(fds[0]);
        const char *msg = "Hello from parent!";
        write(fds[1], msg, strlen(msg) + 1);
        close(fds[1]);
        wait(NULL);
    }
    return 0;
}
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`fs/pipe.c`
- 内核头文件：`include/linux/pipe_fs_i.h`