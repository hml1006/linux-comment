# listns 系统调用分析

## 1. 概述

`listns` 列出指定进程的命名空间（namespace）ID。用于查询进程所属的所有命名空间类型及其标识符，是 `lsns` 命令的底层实现。

**原型：**

```c
SYSCALL_DEFINE4(listns, const struct ns_id_req __user *, req,
                u64 __user *, ns_ids, size_t, nr_ns_ids, unsigned int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `req` | `const struct ns_id_req __user *` | 请求参数，包含目标进程的 pidfd 等信息 |
| `ns_ids` | `u64 __user *` | 输出缓冲区，接收命名空间 ID 数组 |
| `nr_ns_ids` | `size_t` | `ns_ids` 缓冲区的大小（元素个数） |
| `flags` | `unsigned int` | 保留标志位，当前必须为 0 |

**返回值：**
- 成功返回写入 `ns_ids` 的命名空间 ID 数量
- 失败返回负的错误码

## 2. 使用场景

- `lsns` 命令列出系统命名空间
- 容器运行时查询容器命名空间
- 诊断和调试命名空间相关问题
- 监控容器隔离状态

## 3. 函数调用栈

```
SYSCALL_DEFINE4(listns, req, ns_ids, nr_ns_ids, flags)   // kernel/nstree.c
  ├─ flags != 0 → 返回 -EINVAL
  ├─ nr_ns_ids > 1000000 → 返回 -EOVERFLOW
  ├─ access_ok(ns_ids, nr_ns_ids * sizeof(*ns_ids)) → 检查用户空间缓冲区
  │    不可访问 → 返回 -EFAULT
  ├─ copy_ns_id_req(req, &kreq)                           // 拷贝请求参数
  │    拷贝失败 → 返回 -EFAULT
  ├─ prepare_klistns(&klns, &kreq, ns_ids, nr_ns_ids)     // 准备 namespace 列表
  │    失败 → 返回错误码
  ├─ [kreq.user_ns_id]
  │    ├─ 是 → do_listns_userns(&klns)                    // 列出用户命名空间
  │    └─ 否 → do_listns(&klns)                           // 列出普通命名空间
  └─ 返回结果
```

### 3.1 命名空间类型

Linux 支持以下命名空间类型（每种类型对应一个 `ns_operations`）：

| 类型 | 标识 | 说明 |
|------|------|------|
| `CLONE_NEWCGROUP` | CGROUP | cgroup 命名空间 |
| `CLONE_NEWIPC` | IPC | SystemV IPC 和 POSIX 消息队列 |
| `CLONE_NEWNET` | NET | 网络栈（网络设备、路由表等） |
| `CLONE_NEWNS` | MNT | 挂载点 |
| `CLONE_NEWPID` | PID | 进程 ID |
| `CLONE_NEWTIME` | TIME | 时间命名空间 |
| `CLONE_NEWUSER` | USER | 用户和用户组 ID |
| `CLONE_NEWUTS` | UTS | 主机名和域名 |

## 4. 关键数据结构

### 4.1 struct ns_id_req（命名空间 ID 请求）

```c
// include/uapi/linux/nstree.h
struct ns_id_req {
    __u64 pidfd;            // 目标进程的 pidfd
    __u64 user_ns_id;       // 用户命名空间 ID（可选过滤）
    __u64 request_mask;     // 请求掩码（指定需要哪些命名空间）
};
```

### 4.2 struct klistns（内核命名空间列表）

```c
// kernel/nstree.c (内部结构)
struct klistns {
    // 用于遍历和收集命名空间 ID 的临时状态
    struct ns_id_req req;           // 用户请求参数
    u64 __user *ns_ids;             // 用户空间输出缓冲区
    size_t nr_ns_ids;               // 缓冲区大小
    size_t count;                   // 已收集的 ID 数量
    // ...
};
```

## 5. 流程图

```
用户态调用 listns(req, ns_ids, nr_ns_ids, flags)
    │
    ▼
┌─────────────────────────────────────┐
│  参数校验                           │
│  flags != 0 → 返回 -EINVAL          │
│  nr_ns_ids > 1M → 返回 -EOVERFLOW   │
│  access_ok 检查缓冲区               │
│  失败 → 返回 -EFAULT                │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  copy_ns_id_req() 拷贝请求参数      │
│  失败 → 返回 -EFAULT                │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  prepare_klistns() 准备列表         │
│  → 通过 pidfd 查找目标进程          │
│  → 遍历进程的 nsproxy 获取各命名空间│
│  → 获取每个命名空间的 ID (inode)    │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  根据 user_ns_id 选择输出策略       │
│  ├─ 有 user_ns_id → do_listns_userns│
│  │    过滤属于指定 user_ns 的ns     │
│  └─ 无 user_ns_id → do_listns       │
│       输出所有命名空间 ID            │
└─────────────────────────────────────┘
    │
    ▼
  返回命名空间 ID 数量
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | `flags` 非零 |
| `-EOVERFLOW` | 溢出 | `nr_ns_ids > 1000000` |
| `-EFAULT` | 地址错误 | `req` 或 `ns_ids` 指向的用户空间地址不可访问 |
| `-EBADF` | 无效 pidfd | `req.pidfd` 不是有效的文件描述符 |
| `-ESRCH` | 进程不存在 | pidfd 对应的进程已退出 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/nstree.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(void)
{
    // 获取当前进程的 pidfd
    int pidfd = syscall(SYS_pidfd_open, getpid(), 0);
    if (pidfd < 0) {
        perror("pidfd_open");
        return 1;
    }

    struct ns_id_req req = {
        .pidfd = pidfd,
        .user_ns_id = 0,       // 不限制 user_ns
        .request_mask = 0,     // 请求所有命名空间
    };

    u64 ns_ids[16];
    ssize_t ret = syscall(SYS_listns, &req, ns_ids, 16, 0);
    if (ret < 0) {
        perror("listns");
        close(pidfd);
        return 1;
    }

    printf("Number of namespaces: %zd\n", ret);
    for (ssize_t i = 0; i < ret; i++) {
        printf("  ns_id[%zd] = %lu\n", i, ns_ids[i]);
    }

    close(pidfd);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#系统标识与信息)
- 源码: `kernel/nstree.c`（`SYSCALL_DEFINE4(listns)`）
- 头文件: `include/uapi/linux/nstree.h`
- 相关系统调用: `setns()`, `unshare()`, `pidfd_open()`
- 用户态命令: `lsns`（util-linux 包）