# io_getevents 系统调用分析

## 1. 概述

`io_getevents` 用于等待 AIO 操作的完成事件，并将其读取到用户提供的缓冲区中。支持最小事件数和超时等待。

**原型：**

```c
SYSCALL_DEFINE5(io_getevents, aio_context_t, ctx_id,
                long, min_nr, long, nr,
                struct io_event __user *, events,
                struct __kernel_timespec __user *, timeout)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `ctx_id` | AIO 上下文 ID |
| `min_nr` | 最少返回事件数（在超时前至少等待到这么多事件） |
| `nr` | 最大返回事件数（events 缓冲区大小） |
| `events` | 输出事件数组 |
| `timeout` | 超时时间（NULL 表示无限等待） |

## 3. 函数调用链

```
io_getevents (系统调用入口)
  │
  └─ read_events(ctx, min_nr, nr, events, timeout)
       │
       ├─ [timeout] copy_from_user(&ts, timeout, sizeof(ts))
       │
       ├─ while (1) {
       │    │
       │    ├─ aio_read_events(ctx, min_nr, nr, events, &ret)   // 读已完成事件
       │    │    ├─ spin_lock(&ctx->ctx_lock)
       │    │    ├─ 从 completed_events FIFO 取出事件
       │    │    ├─ 转换为 io_event 格式
       │    │    ├─ copy_to_user(events, io_event, ret)
       │    │    ├─ 更新环形缓冲区头指针
       │    │    └─ spin_unlock(&ctx->ctx_lock)
       │    │
       │    ├─ [ret >= min_nr] → break (已满足最少事件数)
       │    │
       │    ├─ [超时] → break (超时返回)
       │    │
       │    ├─ prepare_to_wait(&ctx->wait, &wait, TASK_INTERRUPTIBLE)
       │    │
       │    └─ schedule_timeout(timeout)                     // 阻塞等待
       │         └─ 被 aio_complete 唤醒
       │  }
       │
       └─ 返回 ret (实际读取的事件数)
```

## 4. 关键数据结构

### 4.1 struct io_event（完成事件）

```c
// include/uapi/linux/aio_abi.h
struct io_event {
    __u64 data;          /* 用户数据（来自 iocb->aio_data） */
    __u64 obj;           /* 指向原始 iocb 的指针 */
    __s64 res;           /* 结果码（如读取的字节数或错误码） */
    __s64 res2;          /* 次要结果码 */
};
```

### 4.2 struct kioctx（关键字段）

```c
// fs/aio.c
struct kioctx {
    /* ... */
    struct aio_ring_info ring_info;    /* 环形缓冲区信息 */
    struct __kfifo       completed_events; /* 完成事件 FIFO */
    wait_queue_head_t    wait;         /* 等待队列 */
    spinlock_t           ctx_lock;     /* 上下文锁 */
    /* ... */
};
```

## 5. 流程图

```
用户态调用 io_getevents(ctx, min_nr, nr, events, timeout)
  │
  v
read_events(ctx, min_nr, nr, events, timeout)
  │
  ├── 循环:
  │    ├── aio_read_events() 从完成队列读取事件
  │    │    └── copy_to_user() 到用户空间
  │    │
  │    ├── [ret >= min_nr] → 退出
  │    │
  │    └── [不足]:
  │         ├── prepare_to_wait() 加入等待队列
  │         ├── schedule_timeout() 阻塞
  │         │    ├── 被 aio_complete 唤醒 → 继续
  │         │    └── 超时 → 退出
  │         └── 重试
  │
  └── 返回事件数
```

## 6. 完成路径

```
I/O 完成时:
  aio_complete(kiocb, res, res2)
    ├─ 填充 ki_res (obj, data, res, res2)
    ├─ 写入 aio_ring_info 环形缓冲区
    ├─ 将事件放入 completed_events FIFO
    └─ wake_up(&ctx->wait) 唤醒 io_getevents 等待者
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | ctx_id 无效或 min_nr/nr 超出范围 |
| `EFAULT` | 地址错误 | events 或 timeout 指针不可访问 |
| `EINTR` | 信号中断 | 等待时被信号中断 |

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
    struct io_event events[2];
    char buf[4096];
    int fd;

    io_setup(128, &ctx);

    fd = open("/tmp/test_file", O_CREAT | O_RDWR, 0644);
    write(fd, "Hello AIO!", 10);

    // 准备读请求
    memset(&iocb, 0, sizeof(iocb));
    iocb.aio_fildes = fd;
    iocb.aio_lio_opcode = IOCB_CMD_PREAD;
    iocb.aio_buf = (unsigned long)buf;
    iocb.aio_nbytes = 4096;
    iocb.aio_offset = 0;

    io_submit(ctx, 1, iocbs);

    // 等待完成（至少 1 个事件，最多 2 个）
    int ret = io_getevents(ctx, 1, 2, events, NULL);
    if (ret > 0) {
        printf("Got %d events\n", ret);
        printf("Result: %lld\n", events[0].res);
        buf[events[0].res] = '\0';
        printf("Data: %s\n", buf);
    }

    close(fd);
    io_destroy(ctx);
    return 0;
}
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#异步-i/o-aio)
- 源码位置：`fs/aio.c`（第 2250 行）
- 用户态头文件：`linux/aio_abi.h`