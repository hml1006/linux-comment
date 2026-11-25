# userfaultfd 系统调用分析

> 所属分类：内存管理 | [ARM64 系统调用表](../arm64-syscall-table.md#7-内存管理)

## 1. 概述

`userfaultfd` 创建一个"用户态缺页处理"文件描述符。注册到该 fd 的 VMA 发生缺页时不由内核直接处理，而是把事件写入 fd 读队列，交给用户态线程；后者用 `UFFDIO_COPY`、`UFFDIO_ZEROPAGE`、`UFFDIO_CONTINUE`、`UFFDIO_MOVE` 等 ioctl 填充页面并唤醒故障线程。它用于热迁移（QEMU post-copy）、用户态内存管理、按需加载等。

**当前源码状态与配置：**

- 本树实现已从 `fs/userfaultfd.c` 合并到 `mm/userfaultfd.c`（提交 `7bc5e747bba2 "userfaultfd: merge fs/userfaultfd.c into mm/userfaultfd.c"`），`fs/userfaultfd.c` 已不存在；`mm/Makefile:131` 为 `obj-$(CONFIG_USERFAULTFD) += userfaultfd.o`。
- 当前 `.config` 为裁剪调试配置，**未启用** `CONFIG_USERFAULTFD`（`# CONFIG_USERFAULTFD is not set`）。源码存在，但该内核不会编译此实现，系统调用落到 `sys_ni_syscall` 并返回 `-ENOSYS`。

ARM64 系统调用号：`__NR_userfaultfd = 282`。

## 2. 函数原型与参数

```c
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>

int userfaultfd(int flags);
```

内核入口（`mm/userfaultfd.c:4498`）：

```c
SYSCALL_DEFINE1(userfaultfd, int, flags)
{
    if (!userfaultfd_syscall_allowed(flags))
        return -EPERM;

    return new_userfaultfd(flags);
}
```

| 参数 | 说明 |
|------|------|
| `flags` | `O_CLOEXEC`、`O_NONBLOCK`，可选 `UFFD_USER_MODE_ONLY`（值 1） |

`UFFD_USER_MODE_ONLY`（`include/uapi/linux/userfaultfd.h:384`）表示只处理用户态缺页，不接管内核态缺页，是非特权进程的安全模式。

权限判定 `userfaultfd_syscall_allowed()`（4481）：

```c
if (flags & UFFD_USER_MODE_ONLY)
    return true;                    /* 用户态缺页总是允许 */
if (capable(CAP_SYS_PTRACE))
    return true;                    /* 特权进程可处理内核缺页 */
return sysctl_unprivileged_userfaultfd;
```

## 3. 内核实现路径

```
__arm64_sys_userfaultfd(flags)
  └─ SYSCALL_DEFINE1(userfaultfd)                  // mm/userfaultfd.c:4498
       ├─ userfaultfd_syscall_allowed(flags)       // 4481
       └─ new_userfaultfd(flags)                   // 4443
            ├─ kmem_cache_alloc(userfaultfd_ctx_cachep)
            ├─ 初始化 flags/features/released/mmap_changing/mm
            ├─ anon_inode_create_getfile("[userfaultfd]", &userfaultfd_fops, ctx, ...)
            ├─ mmgrab(ctx->mm)
            └─ fd_publish()

ioctl 分发（userfaultfd_ioctl，4348）：
  UFFDIO_API(4284) / REGISTER(3573) / UNREGISTER(3747) / WAKE(3884)
  / COPY(3916) / ZEROPAGE(3976) / MOVE(4214) / WRITEPROTECT(4030)
  / CONTINUE(4082) / POISON(4143)
```

`UFFDIO_API` 必须最先调用，成功后 `ctx->features` 经 `cmpxchg()` 置位，其他 ioctl 才被允许（`userfaultfd_is_initialized()`）。

缺页核心 `handle_userfault()`（2693）由 `mm/memory.c` 调用（5326、5377 等）：

```
handle_userfault(vmf, reason)
  ├─ PF_EXITING/PF_DUMPCORE → 不处理
  ├─ ctx = vma->vm_userfaultfd_ctx.ctx；为空 → 不处理
  ├─ UFFD_FEATURE_SIGBUS → 返回 SIGBUS
  ├─ 非用户缺页且 UFFD_USER_MODE_ONLY → 不处理
  ├─ 构造 userfaultfd_wait_queue uwq
  ├─ __add_wait_queue(&ctx->fault_pending_wqh, &uwq.wq)
  ├─ set_current_state(blocking_state)
  ├─ userfaultfd_must_wait() / userfaultfd_huge_must_wait()
  ├─ release_fault_lock(vmf)
  ├─ wake_up_poll(&ctx->fd_wqh, EPOLLIN)   /* 通知管理线程 */
  ├─ schedule()
  └─ 清理 wait queue，返回 VM_FAULT_RETRY
```

管理线程在 `read()` 阻塞，`userfaultfd_ctx_read()`（3306）把事件从 `fault_pending_wqh` 重排到 `fault_wqh` 并拷贝 `struct uffd_msg`；`userfaultfd_poll()`（3252）支持 `poll/epoll` 等待。填充页面走 `mfill_atomic()`（962）及变体，最后 `wake_userfault()`（3515）唤醒故障线程。

## 4. 核心数据结构

### 4.1 struct userfaultfd_ctx（`include/linux/userfaultfd_k.h:52`）

```c
struct userfaultfd_ctx {
    wait_queue_head_t fault_pending_wqh; /* 尚未被 read() 取走的缺页 */
    wait_queue_head_t fault_wqh;         /* 已读取、等待处理完成的缺页 */
    wait_queue_head_t fd_wqh;            /* 唤醒 poll/read */
    wait_queue_head_t event_wqh;         /* fork/remap 等事件 */
    seqcount_spinlock_t refile_seq;
    refcount_t refcount;
    unsigned int flags;                  /* O_* 与 UFFD_USER_MODE_ONLY */
    unsigned int features;               /* UFFDIO_API 协商后的特性 */
    bool released;
    struct rw_semaphore map_changing_lock;
    atomic_t mmap_changing;              /* 非协作事件导致的映射变化 */
    struct mm_struct *mm;
};
```

### 4.2 struct uffd_msg（`include/uapi/linux/userfaultfd.h:108`）

`event` 为 `UFFD_EVENT_PAGEFAULT(0x12)`/`FORK(0x13)`/`REMAP`/`REMOVE`/`UNMAP`；`arg` 联合体按事件类型提供 `pagefault.{flags,address,ptid}`、`fork.ufd`、`remap.{from,to,len}`、`remove.{start,end}` 等字段。

### 4.3 struct uffdio_api / uffdio_register

```c
struct uffdio_api {          /* userfaultfd.h:162 */
    __u64 api;               /* 输入 UFFD_API(0xAA) */
    __u64 features;          /* 输入请求，输出内核支持 */
    __u64 ioctls;            /* 输出支持的 ioctl 位图 */
};

struct uffdio_register {     /* userfaultfd.h:261 */
    struct uffdio_range range;
    __u64 mode;              /* MISSING(1<<0)/WP(1<<1)/MINOR(1<<2) */
    __u64 ioctls;
};
```

`userfaultfd_api()` 校验 `api == UFFD_API`；请求 `UFFD_FEATURE_EVENT_FORK` 需 `CAP_SYS_PTRACE`。注册时若区间已被另一个 uffd 拥有，返回 `-EBUSY`。

## 5. 关键流程

```
创建：userfaultfd(flags) → 权限检查 → 分配 ctx + anon inode fd
初始化：ioctl(UFFDIO_API) → 校验 API → 回写特性与 ioctl 位图
注册：ioctl(UFFDIO_REGISTER) → 校验 mode → 设置 VM_UFFD_* 与 ctx
缺页：访问缺失页 → handle_userfault()
  ├─ 挂入 fault_pending_wqh
  ├─ wake_up_poll(fd_wqh, EPOLLIN)
  └─ schedule()
处理：read(fd, &msg) 收到 UFFD_EVENT_PAGEFAULT
  └─ UFFDIO_COPY/ZEROPAGE/CONTINUE/MOVE/POISON → wake_userfault()
```

## 6. 返回值与错误码

成功返回非负 fd；`close(fd)` 释放所有注册并唤醒仍在等待的故障线程。

| 错误码 | 触发条件 |
|--------|----------|
| `-ENOSYS` | 未启用 `CONFIG_USERFAULTFD`（**当前 .config 即此情况**） |
| `-EPERM` | 未指定 `UFFD_USER_MODE_ONLY`，且无 `CAP_SYS_PTRACE`、`vm.unprivileged_userfaultfd=0` |
| `-EINVAL` | ioctl 参数非法；`api` 不匹配；`mode` 非法；区间未页对齐 |
| `-EFAULT` | 拷贝用户结构失败 |
| `-EBUSY` | 区间 VMA 已被另一个 userfaultfd 注册 |
| `-EEXIST` | `UFFDIO_COPY`/`ZEROPAGE` 目标页已存在 |
| `-ENOENT` | `UFFDIO_CONTINUE` 找不到页缓存中的页 |
| `-EAGAIN` | 期间发生 `mmap_changing`（fork/remap），需重试 |
| `-ENOSPC` | `UFFDIO_POISON` 等操作无法完成 |

## 7. 使用示例

下面骨架展示常见流程；**当前 `.config` 下 `userfaultfd()` 返回 `-1/ENOSYS`**，需启用 `CONFIG_USERFAULTFD` 后重新编译。

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>
#include <sys/syscall.h>

int main(void)
{
    long fd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        perror("userfaultfd");      /* 当前 .config 下为 ENOSYS */
        return 1;
    }

    struct uffdio_api api = { .api = UFFD_API, .features = 0 };
    if (ioctl(fd, UFFDIO_API, &api) == -1) {
        perror("UFFDIO_API");
        return 1;
    }
    size_t len = 2 * 4096;
    char *area = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct uffdio_register reg = {
        .range = { .start = (unsigned long)area, .len = len },
        .mode  = UFFDIO_REGISTER_MODE_MISSING,
    };
    if (ioctl(fd, UFFDIO_REGISTER, &reg) == -1) {
        perror("UFFDIO_REGISTER");
        return 1;
    }
    close(fd);
    munmap(area, len);
    return 0;
}
```

完整实现还需处理 `UFFD_EVENT_FORK/REMAP/REMOVE/UNMAP`，并在 `read()` 返回 `EAGAIN` 时继续 poll，以免在 `mmap_changing` 时处理错误。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `mm/userfaultfd.c` | `SYSCALL_DEFINE1(userfaultfd)`（4498）、`new_userfaultfd()`（4443）、`userfaultfd_api()`（4284）、`userfaultfd_ioctl()`（4348）、`handle_userfault()`（2693）、`mfill_atomic()`（962） |
| `include/linux/userfaultfd_k.h` | `struct userfaultfd_ctx`、`handle_userfault()` 声明 |
| `include/uapi/linux/userfaultfd.h` | `UFFD_API`、`uffd_msg`、`uffdio_api/register`、`UFFD_USER_MODE_ONLY` |
| `mm/Makefile` | `obj-$(CONFIG_USERFAULTFD) += userfaultfd.o`（131 行） |
| `.config` | `# CONFIG_USERFAULTFD is not set` |
