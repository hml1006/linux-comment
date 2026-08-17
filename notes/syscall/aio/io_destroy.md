# io_destroy 系统调用分析

## 1. 概述

`io_destroy` 用于销毁一个 AIO 上下文。所有尚未完成的 I/O 请求将被取消，等待它们完成后释放所有资源。

**原型：**

```c
SYSCALL_DEFINE1(io_destroy, aio_context_t, ctx)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `ctx` | 要销毁的 AIO 上下文 ID |

## 3. 函数调用链

```
io_destroy (系统调用入口)
  │
  ├─ ioctx = lookup_ioctx(ctx)                    // 查找 AIO 上下文
  │    └─ idr_find / xa_load 查找 kioctx
  │
  ├─ [ioctx 不存在] → return -EINVAL
  │
  ├─ 创建 ctx_rq_wait 等待结构
  │
  ├─ percpu_ref_kill(&ioctx->users)               // 标记销毁，阻止新引用
  │
  ├─ 遍历并取消所有活跃请求:
  │    ├─ spin_lock(&ctx->ctx_lock)
  │    ├─ 遍历 active_reqs 链表:
  │    │    └─ 对每个请求调用 ki_cancel()
  │    └─ spin_unlock(&ctx->ctx_lock)
  │
  ├─ wait_for_completion(&wait.comp)              // 等待所有请求完成
  │    └─ 引用计数归零时 completion 被触发
  │
  ├─ 释放 kioctx 资源:
  │    ├─ 释放环形缓冲区 (aio_ring_info)
  │    ├─ 释放 kioctx 内存
  │    └─ 从当前进程的 ctx_table 中移除
  │
  └─ return 0
```

## 4. 关键数据结构

### 4.1 struct kioctx（AIO 上下文）

```c
// fs/aio.c
struct kioctx {
    struct percpu_ref    users;           /* 用户引用计数 */
    atomic_t             dead;            /* 标记是否已销毁 */

    struct percpu_ref    reqs;            /* 请求引用计数 */

    unsigned long        user_id;         /* 用户态 ctx ID */

    struct kioctx_cpu __percpu *cpu;      /* per-CPU 数据 */

    unsigned             req_batch;       /* 请求批处理大小 */
    unsigned             max_reqs;        /* 最大请求数 */

    /* 环形缓冲区信息 */
    struct aio_ring_info ring_info;

    /* 完成事件 FIFO */
    struct __kfifo       completed_events;

    /* 等待队列 */
    wait_queue_head_t    wait;

    spinlock_t           ctx_lock;        /* 上下文锁 */
    struct list_head     active_reqs;     /* 活跃请求链表 */

    /* ... */
};
```

## 5. 流程图

```
用户态调用 io_destroy(ctx)
  │
  ├── lookup_ioctx() 查找上下文
  │    └── 未找到 → -EINVAL
  │
  ├── percpu_ref_kill() 标记销毁
  │
  ├── 遍历并取消所有活跃请求
  │
  ├── wait_for_completion() 等待所有请求完成
  │
  ├── 释放环形缓冲区和其他资源
  │
  └── 释放 kioctx 内存
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | ctx 无效或不存在 |

## 7. 使用示例

```c
#include <linux/aio_abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

int main() {
    aio_context_t ctx = 0;

    // 创建 AIO 上下文
    if (io_setup(128, &ctx) < 0) {
        perror("io_setup");
        exit(1);
    }
    printf("AIO context created: %ld\n", ctx);

    // ... 使用 AIO 上下文提交请求 ...

    // 销毁 AIO 上下文
    if (io_destroy(ctx) < 0) {
        perror("io_destroy");
        exit(1);
    }
    printf("AIO context destroyed\n");

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#异步-i/o-aio)
- 源码位置：`fs/aio.c`
- 用户态头文件：`linux/aio_abi.h`