# io_cancel 系统调用分析

## 1. 概述

`io_cancel` 用于取消一个已提交的异步 I/O 请求。如果请求尚未完成，尝试取消它；如果已经完成，则返回 EINVAL。

**原型：**

```c
SYSCALL_DEFINE3(io_cancel, aio_context_t, ctx_id, struct iocb __user *, iocb,
                struct io_event __user *, result)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `ctx_id` | AIO 上下文 ID（由 `io_setup` 返回） |
| `iocb` | 要取消的 iocb 结构指针 |
| `result` | 输出完成事件（如果取消成功） |

## 3. 函数调用链

```
io_cancel (系统调用入口)
  │
  ├─ ctx = lookup_ioctx(ctx_id)                    // 查找 AIO 上下文
  │    └─ idr_find / xa_load 查找 kioctx
  │
  ├─ [ctx 不存在] → return -EINVAL
  │
  ├─ spin_lock(&ctx->ctx_lock)
  │
  ├─ 遍历 ctx->active_reqs 链表:
  │    └─ 查找匹配 kiocb->ki_res.obj == iocb 的 aio_kiocb
  │         ├─ 未找到:
  │         │    └─ spin_unlock → return -EINVAL (已完成或不存在)
  │         │
  │         └─ 找到:
  │              ├─ kiocb->ki_cancel(kiocb)         // 调用取消函数
  │              │    └─ 对于普通文件: aio_cancel
  │              │         └─ kiocb_cancel(&req->rw)
  │              │              └─ file->f_op->cancel 或 -EINVAL
  │              │
  │              ├─ list_del(&kiocb->ki_list)       // 从活跃列表移除
  │              ├─ spin_unlock(&ctx->ctx_lock)
  │              │
  │              ├─ 构造 io_event 并 copy_to_user(result)
  │              ├─ iocb_put(kiocb)                  // 释放引用
  │              └─ return 0
  │
  └─ (如果未找到) return -EINVAL
```

## 4. 关键数据结构

```c
// fs/aio.c
struct aio_kiocb {
    union {
        struct file     *ki_filp;
        struct kiocb     rw;           /* 读写请求上下文 */
        struct fsync_iocb fsync;        /* fsync 请求 */
        struct poll_iocb poll;          /* poll 请求 */
    };

    struct kioctx       *ki_ctx;        /* 所属 AIO 上下文 */
    kiocb_cancel_fn     *ki_cancel;     /* 取消函数 */
    struct io_event      ki_res;        /* 完成事件 */
    struct list_head     ki_list;       /* 用于取消的链表 */
    refcount_t           ki_refcnt;     /* 引用计数 */
    /* ... */
};
```

## 5. 流程图

```
用户态调用 io_cancel(ctx_id, iocb, result)
  │
  ├── lookup_ioctx() 查找上下文
  │    └── 未找到 → -EINVAL
  │
  ├── 遍历 active_reqs 查找匹配的 iocb
  │    ├── 未找到 → -EINVAL (已完成或不存在)
  │    │
  │    └── 找到:
  │         ├── ki_cancel() 尝试取消
  │         ├── 从 active_reqs 移除
  │         ├── 构造 io_event 返回给用户
  │         └── 释放 kiocb 引用
  │
  └── 返回 0 (成功) 或错误码
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | ctx_id 无效或 iocb 未找到（已完成或不存在） |
| `EFAULT` | 地址错误 | result 指针不可访问 |
| `ENOMEM` | 内存不足 | 无法分配临时内存 |

## 7. 使用示例

```c
#include <linux/aio_abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

int main() {
    aio_context_t ctx = 0;
    struct iocb iocb;
    struct iocb *iocbs[] = {&iocb};
    struct io_event events[1];
    char buf[4096];
    int fd;

    // 创建 AIO 上下文
    if (io_setup(128, &ctx) < 0) {
        perror("io_setup");
        exit(1);
    }

    fd = open("/tmp/test_file", O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    // 准备读请求
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

    // 尝试取消请求
    if (io_cancel(ctx, &iocb, events) == 0) {
        printf("I/O request cancelled\n");
    } else if (errno == EINVAL) {
        printf("Request already completed or not found\n");
    } else {
        perror("io_cancel");
    }

    close(fd);
    io_destroy(ctx);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#异步-i/o-aio)
- 源码位置：`fs/aio.c`
- 用户态头文件：`linux/aio_abi.h`