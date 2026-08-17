# io_submit 系统调用分析

## 1. 概述

`io_submit` 用于向 AIO 上下文提交异步 I/O 请求。可以一次提交多个请求，内核会异步处理它们。

**原型：**

```c
SYSCALL_DEFINE3(io_submit, aio_context_t, ctx_id, long, nr,
                struct iocb __user * __user *, iocbpp)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `ctx_id` | AIO 上下文 ID |
| `nr` | 提交的 iocb 数量 |
| `iocbpp` | 指向 iocb 指针数组的指针 |

**支持的 iocb 命令（aio_lio_opcode）：**

| 命令 | 说明 |
|------|------|
| `IOCB_CMD_PREAD` | 异步读（pread） |
| `IOCB_CMD_PWRITE` | 异步写（pwrite） |
| `IOCB_CMD_FSYNC` | 异步 fsync |
| `IOCB_CMD_FDSYNC` | 异步 fdatasync |
| `IOCB_CMD_POLL` | 异步 poll（Linux 5.0+） |

## 3. 函数调用链

```
io_submit (系统调用入口)
  │
  ├─ ctx = lookup_ioctx(ctx_id)                     // 查找 AIO 上下文
  │
  ├─ for (i = 0; i < nr; i++) {
  │    │
  │    ├─ copy_from_user(&iocb, iocbpp[i], sizeof(iocb))  // 拷贝 iocb
  │    │
  │    └─ io_submit_one(ctx, iocbpp[i], false)
  │         │
  │         ├─ req = aio_get_req(ctx)               // 分配 aio_kiocb
  │         │    └─ percpu_ref_tryget 等
  │         │
  │         └─ __io_submit_one(ctx, &iocb, user_iocb, req, false)
  │              │
  │              ├─ fget(iocb->aio_fildes)           // 获取文件
  │              │    └─ req->ki_filp = file
  │              │
  │              ├─ [IOCB_FLAG_RESFD] eventfd_ctx_fdget(iocb->aio_resfd)
  │              │
  │              ├─ aio_prep_rw(req, iocb)           // 准备读写请求
  │              │    ├─ 设置 kiocb 参数 (offset, length, buf)
  │              │    └─ req->ki_cancel = aio_cancel
  │              │
  │              ├─ kiocb_set_cancel_fn(req, aio_cancel)  // 设置取消函数
  │              │
  │              └─ aio_rw_done(req, ...)
  │                   │
  │                   ├─ [读/写] call_read_iter / call_write_iter
  │                   │    └─ file->f_op->read_iter / write_iter
  │                   │
  │                   ├─ [同步完成] → aio_complete(req, res, res2)
  │                   │
  │                   └─ [异步] → 由驱动回调 kiocb->ki_complete 触发完成
  │  }
  │
  └─ 返回成功提交的请求数
```

## 4. 关键数据结构

### 4.1 struct iocb（用户态 I/O 请求控制块）

```c
// include/uapi/linux/aio_abi.h
struct iocb {
    __u64  aio_data;         /* 用户数据（在 io_event.data 中返回） */
    __u32  aio_key;          /* 内部使用 */
    __u16  aio_reserved1;
    __u16  aio_lio_opcode;   /* I/O 命令 */
    __s32  aio_fildes;       /* 文件描述符 */
    __u64  aio_buf;          /* 缓冲区地址 */
    __u64  aio_nbytes;       /* 字节数 */
    __s64  aio_offset;       /* 文件偏移 */
    __u64  aio_reserved2;
    __u32  aio_flags;        /* 标志 */
    __u32  aio_resfd;        /* eventfd 描述符 */
};
```

### 4.2 struct aio_kiocb（内核请求控制块）

```c
// fs/aio.c
struct aio_kiocb {
    union {
        struct file          *ki_filp;
        struct kiocb          rw;        /* 读写请求上下文 */
        struct fsync_iocb    fsync;      /* fsync 请求 */
        struct poll_iocb     poll;       /* poll 请求 */
    };

    struct kioctx            *ki_ctx;    /* 所属 AIO 上下文 */
    kiocb_cancel_fn          *ki_cancel; /* 取消函数 */
    struct io_event           ki_res;    /* 完成事件 */
    struct list_head          ki_list;   /* 用于取消的链表 */
    refcount_t                ki_refcnt; /* 引用计数 */
};
```

## 5. 流程图

```
用户态调用 io_submit(ctx_id, nr, iocbpp)
  │
  ├── lookup_ioctx() 查找上下文
  │
  └── 循环处理每个 iocb:
       │
       ├── copy_from_user() 拷贝 iocb
       │
       ├── aio_get_req() 分配 aio_kiocb
       │
       ├── fget() 获取文件
       │
       ├── aio_prep_rw() 准备读写
       │
       └── aio_rw_done():
            ├── call_read_iter() / call_write_iter()
            ├── [同步完成] → aio_complete()
            └── [异步] → 等待驱动回调完成
```

## 6. 完成路径

```
aio_rw_done() 后的两种路径:

  1. 同步完成 (驱动立即返回):
     aio_complete(req, res, res2)
       ├─ 填充 ki_res
       ├─ 写入环形缓冲区
       └─ wake_up()

  2. 异步完成 (驱动稍后回调):
     kiocb->ki_complete(req->rw, res)
       └─ aio_complete(req, res, 0)
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | ctx_id 无效或 iocb 参数错误 |
| `EAGAIN` | 资源不足 | 上下文请求数已达上限 |
| `EFAULT` | 地址错误 | iocbpp 或 iocb 指针不可访问 |
| `EBADF` | 无效文件描述符 | aio_fildes 无效 |
| `EOPNOTSUPP` | 不支持操作 | aio_lio_opcode 不支持 |

## 8. 使用示例

```c
#include <linux/aio_abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main() {
    aio_context_t ctx = 0;
    struct iocb iocb;
    struct iocb *iocbs[] = {&iocb};
    struct io_event events[1];
    char buf[4096];
    int fd;

    io_setup(128, &ctx);

    fd = open("/tmp/test_file", O_CREAT | O_RDWR, 0644);
    write(fd, "Hello AIO!", 10);

    // 准备异步读请求
    memset(&iocb, 0, sizeof(iocb));
    iocb.aio_fildes = fd;
    iocb.aio_lio_opcode = IOCB_CMD_PREAD;
    iocb.aio_buf = (unsigned long)buf;
    iocb.aio_nbytes = 4096;
    iocb.aio_offset = 0;

    // 提交请求
    if (io_submit(ctx, 1, iocbs) != 1) {
        perror("io_submit");
        exit(1);
    }
    printf("AIO request submitted\n");

    // 等待完成
    io_getevents(ctx, 1, 1, events, NULL);
    printf("AIO completed, result: %lld\n", events[0].res);

    close(fd);
    io_destroy(ctx);
    return 0;
}
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#异步-i/o-aio)
- 源码位置：`fs/aio.c`（第 2081 行）
- 用户态头文件：`linux/aio_abi.h`