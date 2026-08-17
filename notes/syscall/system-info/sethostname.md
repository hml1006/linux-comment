# sethostname 系统调用分析

## 1. 概述

`sethostname` 设置当前主机的主机名。该值存储在 UTS 命名空间的 `nodename` 字段中，可通过 `uname()` 系统调用或 `/proc/sys/kernel/hostname` 读取。

**原型：**

```c
SYSCALL_DEFINE2(sethostname, char __user *, name, int, len)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `name` | `char __user *` | 用户空间传入的主机名缓冲区指针 |
| `len` | `int` | 主机名长度（字节） |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- `hostname` 命令设置主机名
- 系统初始化脚本（如 `systemd`）配置主机名
- 容器运行时为容器设置独立的主机名（每个 UTS 命名空间可独立设置）
- 动态主机名更新（如 DHCP 客户端）

## 3. 函数调用栈

```
SYSCALL_DEFINE2(sethostname, name, len)                // kernel/sys.c
  ├─ 权限检查: ns_capable(uts_ns->user_ns, CAP_SYS_ADMIN)
  │    无 CAP_SYS_ADMIN → 返回 -EPERM
  ├─ 参数校验: len < 0 || len > __NEW_UTS_LEN (64)
  │    非法长度 → 返回 -EINVAL
  ├─ copy_from_user(tmp, name, len)                     // 从用户空间拷贝数据
  │    拷贝失败 → 返回 -EFAULT
  ├─ add_device_randomness(tmp, len)                    // 增加系统熵池随机性
  ├─ down_write(&uts_sem)                               // 获取 UTS 写锁
  ├─ u = utsname()                                      // 获取当前 UTS 命名空间
  ├─ memcpy(u->nodename, tmp, len)                      // 写入 nodename
  ├─ memset(u->nodename + len, 0, sizeof(u->nodename) - len) // 清空尾部
  ├─ errno = 0
  ├─ uts_proc_notify(UTS_PROC_HOSTNAME)                 // 通知 /proc/sys 更新
  ├─ up_write(&uts_sem)                                 // 释放 UTS 写锁
  └─ return errno
```

## 4. 关键数据结构

### 4.1 struct new_utsname（UTS 名称信息）

```c
// include/uapi/linux/utsname.h
struct new_utsname {
    char sysname[__NEW_UTS_LEN + 1];    // 系统名（如 "Linux"），__NEW_UTS_LEN = 64
    char nodename[__NEW_UTS_LEN + 1];   // 主机名（由 sethostname 设置）
    char release[__NEW_UTS_LEN + 1];    // 内核版本（如 "6.12.0"）
    char version[__NEW_UTS_LEN + 1];    // 版本号
    char machine[__NEW_UTS_LEN + 1];    // 硬件架构（如 "aarch64"）
    char domainname[__NEW_UTS_LEN + 1]; // 域名（由 setdomainname 设置）
};
```

### 4.2 默认主机名定义

```c
// include/linux/uts.h
#ifndef UTS_NODENAME
#define UTS_NODENAME CONFIG_DEFAULT_HOSTNAME  // 由 sethostname() 设置
#endif
```

内核编译时通过 `CONFIG_DEFAULT_HOSTNAME` 配置默认主机名（通常为 `"(none)"`）。

### 4.3 uts_sem（UTS 读写信号量）

```c
// kernel/sys.c
DECLARE_RWSEM(uts_sem);
```

- 保护 UTS 命名空间所有字段的并发访问
- 写操作使用 `down_write()`/`up_write()` 互斥访问

### 4.4 相关 sysctl 表项

```c
// kernel/utsname_sysctl.c
static const struct ctl_table uts_kern_table[] = {
    {
        .procname   = "hostname",
        .data       = init_uts_ns.name.nodename,
        .maxlen     = sizeof(init_uts_ns.name.nodename),
        .mode       = 0644,
        .proc_handler = proc_do_uts_string,
        .poll       = &hostname_poll,
    },
    // ...
};
```

通过 `/proc/sys/kernel/hostname` 可读写主机名。

## 5. 流程图

```
用户态调用 sethostname(name, len)
    │
    ▼
┌─────────────────────────────────────┐
│  系统调用入口 (sys_sethostname)     │
│  kernel/sys.c                       │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  权限检查                           │
│  ns_capable(uts_ns->user_ns,       │
│             CAP_SYS_ADMIN)          │
│  无权限 → 返回 -EPERM               │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  参数校验                           │
│  len < 0 || len > 64               │
│  非法 → 返回 -EINVAL                │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  copy_from_user(tmp, name, len)    │
│  拷贝失败 → 返回 -EFAULT            │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  add_device_randomness(tmp, len)   │  ← 增加熵池随机性
│  down_write(&uts_sem)              │  ← 获取写锁
│  utsname()->nodename = tmp         │  ← 写入主机名
│  memset(尾部, 0)                   │  ← 清空尾部
│  uts_proc_notify(HOSTNAME)         │  ← 通知 proc 文件系统
│  up_write(&uts_sem)                │  ← 释放写锁
└─────────────────────────────────────┘
    │
    ▼
  返回 0 (成功)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EPERM` | 权限不足 | 调用者没有 `CAP_SYS_ADMIN` 能力 |
| `-EINVAL` | 无效参数 | `len < 0` 或 `len > __NEW_UTS_LEN` (64) |
| `-EFAULT` | 地址错误 | 从用户空间拷贝 `name` 数据失败 |

## 7. 使用示例

```c
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(void)
{
    const char *hostname = "my-machine";
    int len = strlen(hostname);

    // 设置主机名
    if (syscall(SYS_sethostname, hostname, len) == -1) {
        perror("sethostname");
        return 1;
    }

    printf("Hostname set successfully\n");

    // 通过 uname 验证
    struct utsname buf;
    if (uname(&buf) == 0) {
        printf("Nodename: %s\n", buf.nodename);
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#系统标识与信息)
- 源码: `kernel/sys.c`
- 头文件: `include/uapi/linux/utsname.h`, `include/linux/utsname.h`, `include/linux/uts.h`
- sysctl 实现: `kernel/utsname_sysctl.c`
- 配套系统调用: `gethostname()`（条件编译，`__ARCH_WANT_SYS_GETHOSTNAME`），`uname()`