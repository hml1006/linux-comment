# pipefs — 管道文件系统

## 1. 概述与实现机制

pipefs 是 Linux 管道机制的基础，为 `pipe()` 系统调用返回的文件描述符提供 VFS 文件对象支持。当进程调用 `pipe()` 时，内核在 pipefs 中分配 inode 和一对 file 结构（读端和写端）。

### 核心原理

- `pipe()` 系统调用返回两个 fd：读端和写端
- 读端和写端共享同一个 pipe_inode_info
- 数据通过环形缓冲区（pipe->bufs[]）传递
- 阻塞式读写通过等待队列管理

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  int fd[2]; pipe(fd);  // 创建管道                           │
│  write(fd[1], buf, len);  // 写端写入                        │
│  read(fd[0], buf, len);   // 读端读取                        │
└────────────────────────┬─────────────────────────────────────┘
                         │ VFS 系统调用
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                pipefs 层 (fs/pipe.c)                         │
│  create_pipe_files() → 创建读端和写端 file                  │
│  get_pipe_inode() → 在 pipefs 中分配 inode                  │
│  pipe_read() / pipe_write() → 读写操作                      │
│  pipe_poll() / pipe_release() → 轮询/释放                   │
│  pipe_inode_info → 核心数据结构                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 pipe_inode_info — 管道信息

```c
// 文件: include/linux/pipe_fs_i.h
struct pipe_inode_info {
    struct mutex mutex;          // 互斥锁
    wait_queue_head_t rd_wait;   // 读端等待队列 (读阻塞时等待)
    wait_queue_head_t wr_wait;   // 写端等待队列 (写阻塞时等待)
    unsigned int head;           // 写入位置 (生产者)
    unsigned int tail;           // 读取位置 (消费者)
    unsigned int max_usage;      // 最大使用量
    unsigned int ring_size;      // 环形缓冲区大小
    unsigned int nr_accounted;   // 已计数的缓冲区数
    unsigned int readers;        // 读端引用计数
    unsigned int writers;        // 写端引用计数
    unsigned int files;          // 文件引用计数
    unsigned int r_counter;      // 读端总计数 (用于 poll)
    unsigned int w_counter;      // 写端总计数 (用于 poll)
    struct page *tmp_page;       // 临时页 (用于部分写入)
    struct fasync_struct *fasync_readers; // 读端异步通知
    struct fasync_struct *fasync_writers; // 写端异步通知
    struct pipe_buffer *bufs;    // 缓冲区数组
    struct user_struct *user;    // 用户信息 (用于限额)
};
```

### 2.2 pipe_buffer — 管道缓冲区

```c
// 文件: include/linux/pipe_fs_i.h
struct pipe_buffer {
    struct page *page;           // 指向物理页
    unsigned int offset;         // 页内偏移
    unsigned int len;            // 有效数据长度
    const struct pipe_buf_operations *ops; // 缓冲区操作
    unsigned int flags;          // 标志位
    unsigned long private;       // 私有数据
};
```

### 2.3 pipe_buf_operations — 缓冲区操作

```c
// 文件: include/linux/pipe_fs_i.h
struct pipe_buf_operations {
    int (*confirm)(struct pipe_inode_info *, struct pipe_buffer *);  // 确认
    void (*release)(struct pipe_inode_info *, struct pipe_buffer *); // 释放
    bool (*try_steal)(struct pipe_inode_info *, struct pipe_buffer *); // 尝试偷取
    bool (*get)(struct pipe_inode_info *, struct pipe_buffer *);     // 获取
};
```

### 2.4 管道文件操作

```c
// 文件: fs/pipe.c
const struct file_operations pipefifo_fops = {
    .open        = pipe_open,       // 打开
    .read_iter   = pipe_read,       // 读取
    .write_iter  = pipe_write,      // 写入
    .poll        = pipe_poll,       // 轮询
    .unlocked_ioctl = pipe_ioctl,   // IOCTL
    .release     = pipe_release,    // 释放
    .fasync      = pipe_fasync,     // 异步通知
    .splice_write = iter_file_splice_write, // splice 写入
    .splice_read  = copy_splice_read,       // splice 读取
};
```

---

## 3. API 与使用方法

### 3.1 用户空间 API

```c
#include <unistd.h>
#include <fcntl.h>

// 创建管道
int pipe(int pipefd[2]);      // pipefd[0] = 读端, pipefd[1] = 写端
int pipe2(int pipefd[2], int flags);  // 带标志位 (O_NONBLOCK, O_CLOEXEC)

// 使用管道
int fd[2];
pipe(fd);
write(fd[1], data, len);     // 写端写入
read(fd[0], buf, len);       // 读端读取

// 调整管道大小
int fcntl(fd, F_SETPIPE_SZ, size);  // 设置管道缓冲区大小
int fcntl(fd, F_GETPIPE_SZ);        // 获取管道缓冲区大小

// 命名管道 (FIFO)
mkfifo("/tmp/myfifo", 0666);  // 创建 FIFO 文件
open("/tmp/myfifo", O_RDONLY);  // 打开读端
open("/tmp/myfifo", O_WRONLY);  // 打开写端
```

### 3.2 内核内部 API

```c
#include <linux/pipe_fs_i.h>
#include <linux/pipe_fs_i.h>

// 创建管道文件
int create_pipe_files(struct file **res, int flags);
struct pipe_inode_info *get_pipe_inode(void);

// 分配/释放 pipe_inode_info
struct pipe_inode_info *alloc_pipe_info(void);
void free_pipe_info(struct pipe_inode_info *pipe);

// 读取/写入
ssize_t pipe_read(struct kiocb *iocb, struct iov_iter *to);
ssize_t pipe_write(struct kiocb *iocb, struct iov_iter *from);

// 添加/获取缓冲区
unsigned int pipe_space_for_user(unsigned int ring_size);
int pipe_resize_ring(struct pipe_inode_info *pipe, unsigned int nr_slots);
struct pipe_buffer *pipe_head_buf(struct pipe_inode_info *pipe);
```

### 3.3 使用示例

```c
// 内核内部使用管道 (如 splice 机制)
// fs/splice.c
static int splice_from_pipe_feed(struct pipe_inode_info *pipe, ...)
{
    struct pipe_buffer *buf;
    unsigned int i;
    int ret;

    for (i = 0; i < pipe->ring_size; i++) {
        buf = &pipe->bufs[(pipe->tail + i) & (pipe->ring_size - 1)];
        if (!buf->len)
            continue;

        // 处理缓冲区数据
        ret = pipe_buf_confirm(pipe, buf);
        if (ret)
            return ret;

        // 将数据发送到目标
        ret = actor(pipe, buf, ...);
        if (ret <= 0)
            return ret;

        // 消耗缓冲区
        pipe_buf_release(pipe, buf);
        pipe->tail++;
    }

    wake_up_interruptible(&pipe->wr_wait);  // 唤醒写端
    return 0;
}
```

---

## 4. 函数调用栈

### 4.1 pipe() 系统调用

```
pipe(fd)
  ↓ sys_pipe2() → do_pipe2()
    → __do_pipe_flags(fd, files, flags)           // 核心函数
      → create_pipe_files(files, flags)            // 创建管道文件
        → get_pipe_inode()                          // 在 pipefs 中分配 inode
          → alloc_pipe_info()                       // 分配 pipe_inode_info
            → kzalloc_obj(struct pipe_inode_info)   // 分配 pipe 结构
            → kzalloc_objs(pipe_buffer, 16)         // 分配 16 个缓冲区 (默认)
            → init_waitqueue_head(&pipe->rd_wait)   // 初始化读等待队列
            → init_waitqueue_head(&pipe->wr_wait)   // 初始化写等待队列
            → pipe->r_counter = pipe->w_counter = 1
            → pipe->max_usage = pipe->ring_size = 16
          → inode = new_inode(sb)                   // 分配 inode
          → inode->i_fop = &pipefifo_fops           // 设置文件操作
          → inode->i_pipe = pipe                    // 关联 pipe 结构
          → return inode
        → alloc_file_pseudo(inode, pipe_mnt, ...)   // 分配读端 file
          → file->f_op = &pipefifo_fops
          → file->private_data = pipe
        → alloc_file_pseudo(inode, pipe_mnt, ...)   // 分配写端 file
          → file->f_op = &pipefifo_fops
          → file->private_data = pipe
      → __alloc_fd(files, 0, 0, flags)              // 分配读端 fd
      → __alloc_fd(files, 0, 0, flags)              // 分配写端 fd
      → fd_install(fd[0], files[0])                  // 安装读端 fd
      → fd_install(fd[1], files[1])                  // 安装写端 fd
```

### 4.2 管道写入

```
write(fd[1], buf, len)
  ↓ vfs_write() → pipe_write(iocb, from)
    → pipe_write(iocb, from)                        // 写入核心
      → mutex_lock(&pipe->mutex)                    // 加锁
      → for (;;) {
          // 检查写端是否还有空间
          head = pipe->head;
          tail = READ_ONCE(pipe->tail);
          if (!pipe->readers) {                     // 读端已关闭
              mutex_unlock(&pipe->mutex);
              return -EPIPE;                        // 发送 SIGPIPE
          }

          // 如果缓冲区满, 等待
          if (pipe_full(head, tail, pipe->max_usage)) {
              wait_event_interruptible(pipe->wr_wait,
                  !pipe_full(head, tail, pipe->max_usage));
              continue;
          }

          // 分配新页作为缓冲区
          page = alloc_page(GFP_HIGHUSER | __GFP_ACCOUNT);
          pipe->bufs[head & (pipe->ring_size - 1)] = (struct pipe_buffer) {
              .page = page,
              .offset = 0,
              .len = copied,
              .ops = &anon_pipe_buf_ops,
          };
          pipe->head = head + 1;                    // 推进写入位置

          // 拷贝数据到页
          copied = copy_page_from_iter(page, 0, PAGE_SIZE, from);

          // 唤醒读端
          if (pipe->head > head)
              wake_up_interruptible(&pipe->rd_wait);
        }
      → mutex_unlock(&pipe->mutex)                  // 解锁
```

### 4.3 管道读取

```
read(fd[0], buf, len)
  ↓ vfs_read() → pipe_read(iocb, to)
    → pipe_read(iocb, to)                          // 读取核心
      → mutex_lock(&pipe->mutex)                    // 加锁
      → for (;;) {
          tail = pipe->tail;
          if (tail != pipe->head) {                 // 缓冲区非空
              struct pipe_buffer *buf = &pipe->bufs[tail & mask];
              // 从当前缓冲区拷贝数据
              written = copy_page_to_iter(buf->page, buf->offset, avail, to);
              if (written) {
                  buf->offset += written;
                  buf->len -= written;
                  if (!buf->len) {                  // 缓冲区已读空
                      pipe_buf_release(pipe, buf);
                      pipe->tail = tail + 1;        // 推进读取位置
                  }
                  total_len += written;
                  break;
              }
          }
          if (!pipe->writers) {                     // 写端已关闭且无数据
              break;
          }
          // 缓冲区空, 等待
          wait_event_interruptible(pipe->rd_wait,
              pipe->head != pipe->tail);
        }
      → if (total_len)
          wake_up_interruptible_sync(&pipe->wr_wait);  // 唤醒写端
      → mutex_unlock(&pipe->mutex)                  // 解锁
```

---

## 5. 流程图

### 5.1 管道数据流

```
写进程                          pipe_inode_info              读进程
    │                              │                          │
    │ write(fd[1])                 │                          │
    │──────────────────────────────┤                          │
    │                              │                          │
    │                              ▼                          │
    │  pipe_write()          ┌─────────────┐                  │
    │  → alloc_page()        │  head=3     │                  │
    │  → copy_from_user()    │             │                  │
    │  → 推进 head           │  bufs[]:    │                  │
    │                        │  [0] ■■■■■■│─── 已写入        │
    │                        │  [1] ■■■■■■│─── 已写入        │
    │                        │  [2] ■■■■■■│─── 当前写入      │
    │                        │  [3] □□□□□□│─── 空闲          │
    │                        │  [4] □□□□□□│                  │
    │                        │  [5] □□□□□□│                  │
    │                        │  ...       │                  │
    │                        │  tail=0    │                  │
    │                        └─────┬───────┘                  │
    │                              │                          │
    │                              │  wake_up(rd_wait)        │
    │                              ├──────────────────────────┤
    │                              │                          │
    │                              │                          │ read(fd[0])
    │                              ├──────────────────────────┤
    │                              │  pipe_read()             │
    │                              │  → copy_to_user()        │
    │                              │  → 推进 tail             │
    │                              │                          │
    │  wake_up(wr_wait) ◄──────────┘                          │
    │                              │                          │
```

### 5.2 环形缓冲区状态

```
初始状态:                          满状态:
head=0, tail=0                    head=16, tail=0
┌────┬────┬────┬────┬────┐        ┌────┬────┬────┬────┬────┐
│  0 │  1 │  2 │  ..│ 15 │        │  0 │  1 │  2 │  ..│ 15 │
│ ░░░│ ░░░│ ░░░│ ░░░│ ░░░│        │ ▓▓▓│ ▓▓▓│ ▓▓▓│ ▓▓▓│ ▓▓▓│
│空  │空  │空  │空  │空  │        │有  │有  │有  │有  │有  │
└────┴────┴────┴────┴────┘        └────┴────┴────┴────┴────┘
  ▲                               ▲
  h/t                             h

部分写入:                        部分读取:
head=3, tail=0                   head=3, tail=1
┌────┬────┬────┬────┬────┐        ┌────┬────┬────┬────┬────┐
│  0 │  1 │  2 │  3 │  4 │        │  0 │  1 │  2 │  3 │  4 │
│ ▓▓▓│ ▓▓▓│ ▓▓▓│ ░░░│ ░░░│        │ ░░░│ ▓▓▓│ ▓▓▓│ ░░░│ ░░░│
│有  │有  │有  │空  │空  │        │已读│有  │有  │空  │空  │
└────┴────┴────┴────┴────┘        └────┴────┴────┴────┴────┘
  ▲         ▲                       ▲         ▲
  t         h                       t         h
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **进程间通信** | 父子进程间数据传递 | shell 管道 `ls | grep` |
| **线程间通信** | 同一进程内线程间数据传递 | 线程 A 写 → 线程 B 读 |
| **splice 零拷贝** | 文件到管道的零拷贝传输 | `splice(fd_in, ...)` |
| **命名管道 (FIFO)** | 无亲缘关系进程间通信 | `mkfifo` 创建命名管道 |
| **信号驱动 I/O** | 管道数据可用的异步通知 | `fcntl(fd, F_SETFL, O_ASYNC)` |
| **epoll 监听** | 多路复用监听管道事件 | `epoll_create()` 监听管道 |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `fs/pipe.c` | 管道核心实现（读写、创建、poll、release） |
| `include/linux/pipe_fs_i.h` | pipe_inode_info 和 pipe_buffer 数据结构定义 |
| `include/linux/pipe_fs_i.h` | 管道文件系统 API 声明 |
| `fs/fifo.c` | 命名管道 (FIFO) 实现 |
| `fs/splice.c` | splice 系统调用（管道零拷贝） |