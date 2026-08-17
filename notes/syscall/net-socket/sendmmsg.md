# sendmmsg 系统调用分析

## 1. 概述

`sendmmsg` 系统调用在一次调用中批量发送多个消息，减少了系统调用次数，从而提高性能。它是 `sendmsg` 的批量版本。

**原型：**

```c
#include <sys/socket.h>

int sendmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
             unsigned int flags);
```

**参数：**
- `sockfd`：套接字文件描述符
- `msgvec`：指向 `struct mmsghdr` 数组的指针
- `vlen`：`msgvec` 数组的大小（最大 `UIO_MAXIOV`）
- `flags`：消息标志

**返回值：**
- 成功：返回发送成功的消息数量
- 失败：返回 -1 并设置 `errno`

## 2. 内核实现入口

```c
// net/socket.c:2759
SYSCALL_DEFINE4(sendmmsg, int, fd, struct mmsghdr __user *, mmsg,
        unsigned int, vlen, unsigned int, flags)
{
    return __sys_sendmmsg(fd, mmsg, vlen, flags, true);
}
```

## 3. 详细的函数调用链

```
sendmmsg (系统调用入口)
└── __sys_sendmmsg(fd, mmsg, vlen, flags, true)  [net/socket.c:2690]
    ├── if (forbid_cmsg_compat && (flags & MSG_CMSG_COMPAT)) return -EINVAL
    │
    ├── if (vlen > UIO_MAXIOV) vlen = UIO_MAXIOV  → 截断上限
    │
    ├── CLASS(fd, f)(fd)  → 获取 struct file（只做一次）
    ├── if (fd_empty(f)) return -EBADF
    ├── sock = sock_from_file(fd_file(f))  → 获取 struct socket
    ├── if (unlikely(!sock)) return -ENOTSOCK
    │
    ├── used_address.name_len = UINT_MAX  → 初始化地址缓存
    ├── entry = mmsg
    ├── flags |= MSG_BATCH  → 内部标记批量发送
    │
    ├── while (datagrams < vlen):
    │   ├── if (datagrams == vlen - 1)
    │   │   flags = oflags  → 最后一个消息恢复原始 flags
    │   │
    │   ├── if (MSG_CMSG_COMPAT & flags):
    │   │   err = ___sys_sendmsg(sock, (struct user_msghdr *)compat_entry,
    │   │                        &msg_sys, flags, &used_address, MSG_EOR)
    │   │   put_user(err, &compat_entry->msg_len)
    │   │   compat_entry++
    │   │
    │   ├── else:
    │   │   err = ___sys_sendmsg(sock, (struct user_msghdr *)entry,
    │   │                        &msg_sys, flags, &used_address, MSG_EOR)
    │   │   put_user(err, &entry->msg_len)
    │   │   entry++
    │   │
    │   ├── if (err < 0):
    │   │   ├── if (datagrams) break  → 已发送至少一个消息
    │   │   └── else return err  → 第一个消息就失败
    │   │
    │   ├── datagrams++
    │   │
    │   ├── if (msg_data_left(&msg_sys))  → 数据未完全发送
    │   │   break  → 停止批量发送
    │   │
    │   └── cond_resched()  → 内核抢占点
    │
    └── if (datagrams == 0) return err  → 返回最后一次错误
    └── return datagrams  → 返回成功发送的消息数量
```

## 4. 关键优化：地址缓存

```c
// net/socket.c
struct used_address {
    struct sockaddr_storage name;  // 缓存的目标地址
    unsigned int name_len;         // 地址长度
};
```

`sendmmsg` 使用 `used_address` 缓存优化：

1. 初始化 `used_address.name_len = UINT_MAX`，确保第一个消息不命中缓存
2. 对于每个消息，如果目标地址与前一个相同，则跳过 LSM 检查（`sock_sendmsg_nosec`）
3. 如果地址不同，则正常进行 `__sock_sendmsg`（含 LSM 检查）并更新缓存

```c
// ____sys_sendmsg 中的优化逻辑
if (used_address && msg_sys->msg_name &&
    used_address->name_len == msg_sys->msg_namelen &&
    !memcmp(&used_address->name, msg_sys->msg_name,
            used_address->name_len)) {
    // 地址相同，跳过 LSM 检查
    err = sock_sendmsg_nosec(sock, msg_sys);
} else {
    // 地址不同，正常发送（含 LSM 检查）
    err = __sock_sendmsg(sock, msg_sys);
}
```

## 5. 流程图

```
用户态: sendmmsg(fd, msgvec, vlen, flags)
                │
                ▼
   ┌─────────────────────────────────────┐
   │  SYSCALL_DEFINE4(sendmmsg)          │  net/socket.c:2759
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  __sys_sendmmsg()                   │  net/socket.c:2690
   │  ├─ 截断 vlen                       │
   │  ├─ CLASS(fd) → 获取 socket（一次） │
   │  ├─ flags |= MSG_BATCH              │
   │  └─ while (datagrams < vlen):       │
   │      └─ ___sys_sendmsg()            │  ← 循环调用
   │         └─ → tcp_sendmsg/udp_sendmsg│
   │      └─ datagrams++                 │
   │      └─ cond_resched()              │
   └─────────────────────────────────────┘
                │
                ▼
   ┌─────────────────────────────────────┐
   │  return 发送成功的消息数量           │
   └─────────────────────────────────────┘
```

## 6. sendmmsg 的性能优势

| 对比 | 系统调用次数 | LSM 检查次数 |
|------|-------------|-------------|
| 发送 N 个消息 (sendmsg × N) | N 次 | N 次 |
| 发送 N 个消息 (sendmmsg × 1) | 1 次 | 1~N 次（地址缓存优化） |

**核心优化：**
1. **一次 fd 查找**：`CLASS(fd, f)` 和 `sock_from_file` 只调用一次
2. **地址缓存**：相同目标地址跳过 LSM 检查
3. **MSG_BATCH 标记**：内部标记，协议层可以针对批量发送优化
4. **cond_resched**：避免长时间占用 CPU

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EBADF` | 无效文件描述符 | `sockfd` 不是有效的文件描述符 |
| `ENOTSOCK` | 不是套接字 | 文件描述符指向的不是套接字 |
| `EINVAL` | 无效参数 | `flags` 包含 `MSG_CMSG_COMPAT` 或 `vlen` 无效 |
| `EAGAIN` | 资源暂时不可用 | 非阻塞模式且发送缓冲区满 |
| `EFAULT` | 地址指针无效 | `msgvec` 指向不可访问的区域 |
| `EMSGSIZE` | 消息长度错误 | 消息大于套接字发送缓冲区 |
| `EINTR` | 被信号中断 | 阻塞发送时收到信号 |

## 8. 使用示例

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

int main() {
    int sockfd;
    struct sockaddr_in addr;
    struct mmsghdr msgs[3];
    struct iovec iov[3];
    char data1[] = "Hello";
    char data2[] = "World";
    char data3[] = "!";

    // 创建 UDP 套接字
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // 初始化 3 个消息（都发往同一地址）
    iov[0].iov_base = data1;
    iov[0].iov_len = strlen(data1) + 1;
    iov[1].iov_base = data2;
    iov[1].iov_len = strlen(data2) + 1;
    iov[2].iov_base = data3;
    iov[2].iov_len = strlen(data3) + 1;

    for (int i = 0; i < 3; i++) {
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_name = &addr;
        msgs[i].msg_hdr.msg_namelen = sizeof(addr);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    // 批量发送 3 个消息
    int ret = sendmmsg(sockfd, msgs, 3, 0);
    if (ret < 0) { perror("sendmmsg"); exit(1); }

    printf("Sent %d messages\n", ret);
    for (int i = 0; i < ret; i++) {
        printf("  msg[%d]: sent %u bytes\n", i, msgs[i].msg_len);
    }

    close(sockfd);
    return 0;
}
```

## 9. sendmmsg vs sendmsg

| 特性 | sendmsg | sendmmsg |
|------|---------|----------|
| 单次调用发送 | 1 个消息 | 多个消息 |
| 地址缓存 | 不支持 | 支持（相同地址跳过 LSM） |
| 内核入口 | 每次调用 1 次 | 1 次处理多个消息 |
| 适用场景 | 通用发送 | 高性能批量发送 |
| Linux 引入版本 | 早期 | 2.6.39 |

## 10. 注意事项

1. **MSG_BATCH 标记**：内核在内部设置 `MSG_BATCH` 标记，协议层可据此优化（如 TCP 的 Nagle 算法行为变化）
2. **部分发送**：如果某个消息发送失败但之前已成功发送了一些消息，`sendmmsg` 返回已成功发送的数量
3. **数据残留**：如果某个消息的数据未完全发送（`msg_data_left` 非零），循环会提前终止
4. **vlen 上限**：`vlen` 被截断到 `UIO_MAXIOV`（通常为 1024），防止内核空间消耗过多

## 11. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#网络与socket)
- Linux 内核源码：`net/socket.c`
- `man 2 sendmmsg`
- Linux 2.6.39 引入