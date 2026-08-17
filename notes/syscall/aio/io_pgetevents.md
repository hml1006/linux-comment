# io_pgetevents 系统调用分析

## 1. 概述

`io_pgetevents` 是 `io_getevents` 的增强版，在等待 AIO 完成事件的同时，可以原子性地设置信号掩码。这解决了信号处理中的竞态条件问题。

**原型：**

```c
SYSCALL_DEFINE6(io_pgetevents,
        aio_context_t, ctx_id,
        long, min_nr,
        long, nr,
        struct io_event __user *, events,
        struct __kernel_timespec __user *, timeout,
        const struct __kernel_sigset_t __user *, sigmask)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `ctx_id` | AIO 上下文 ID |
| `min_nr` | 最少返回事件数 |
| `nr` | 最大返回事件数 |
| `events` | 输出事件数组 |
| `timeout` | 超时时间 |
| `sigmask` | 临时替换的信号掩码（原子操作） |

**io_pgetevents 与 io_getevents 的区别：**

```
io_pgetevents 内部:
  1. 保存当前信号掩码
  2. 设置新信号掩码（sigmask）
  3. 调用 read_events() 等待
  4. 恢复原始信号掩码

这确保了在等待期间信号掩码的修改是原子的，
避免了设置信号掩码和开始等待之间的竞态条件。
```

## 3. 函数调用链

```
io_pgetevents (系统调用入口)
  │
  ├─ copy_from_user(&sigmask, sigmask, sizeof(sigset_t))
  │
  ├─ set_user_sigmask(sigmask, &saved_sigmask, &restore)  // 设置临时信号掩码
  │
  ├─ ret = read_events(ctx, min_nr, nr, events, timeout)  // 等待事件
  │
  └─ restore_user_sigmask(saved_sigmask, &restore)        // 恢复原始信号掩码
```

## 4. 关键数据结构

### 4.1 struct __kernel_sigset_t（信号掩码）

```c
// include/uapi/asm-generic/signal.h
struct __kernel_sigset_t {
    unsigned long sig[_NSIG_WORDS];  /* 信号集合位图 */
};
```

## 5. 流程图

```
用户态调用 io_pgetevents(ctx, min_nr, nr, events, timeout, sigmask)
  │
  ├── 拷贝 sigmask 到内核
  │
  ├── set_user_sigmask()
  │    └── 原子性替换当前信号掩码
  │
  ├── read_events() 等待 AIO 完成事件
  │    └── 与 io_getevents 相同逻辑
  │
  ├── restore_user_sigmask()
  │    └── 恢复原始信号掩码
  │
  └── 返回事件数
```

## 6. 信号安全设计

```
传统方式（竞态条件）:
  sigprocmask(SIG_SETMASK, &newmask, &oldmask);  // 设置新掩码
  io_getevents(ctx, 1, nr, events, NULL);         // 等待（可能被信号打断）
  sigprocmask(SIG_SETMASK, &oldmask, NULL);        // 恢复

  问题: 信号可能在 sigprocmask 和 io_getevents 之间到达

io_pgetevents（原子操作）:
  io_pgetevents(ctx, 1, nr, events, NULL, &newmask);
  // 设置掩码和等待是原子的，信号不会丢失
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | ctx_id 无效 |
| `EFAULT` | 地址错误 | 用户指针不可访问 |
| `EINTR` | 信号中断 | 等待时被信号中断 |

## 8. 使用示例

```c
#include <linux/aio_abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>

int main() {
    aio_context_t ctx = 0;
    struct io_event events[1];
    char buf[4096];
    int fd;

    io_setup(128, &ctx);

    fd = open("/tmp/test_file", O_RDONLY);
    // ... 提交 IO 请求 ...

    sigset_t newmask;
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGINT);  // 阻塞 SIGINT

    // 等待完成，同时阻塞 SIGINT
    int ret = io_pgetevents(ctx, 1, 1, events, NULL, &newmask);
    if (ret > 0) {
        printf("Got event, result: %lld\n", events[0].res);
    }

    close(fd);
    io_destroy(ctx);
    return 0;
}
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#异步-i/o-aio)
- 源码位置：`fs/aio.c`
- 用户态头文件：`linux/aio_abi.h`