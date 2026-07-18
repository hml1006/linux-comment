# setdomainname 系统调用分析

## 1. 概述

`setdomainname` 设置主机的 NIS/YP 域名。该值存储在 UTS 命名空间的 `domainname` 字段中，可通过 `uname()` 系统调用或 `/proc/sys/kernel/domainname` 读取。

**原型：**

```c
SYSCALL_DEFINE2(setdomainname, char __user *, name, int, len)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `name` | `char __user *` | 用户空间传入的域名缓冲区指针 |
| `len` | `int` | 域名长度（字节） |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- `domainname` 命令设置 NIS 域名
- 容器运行时为容器设置独立的域名（每个 UTS 命名空间可独立设置）
- 系统初始化脚本配置网络域名
- 内核中 `gethostname()` 可通过 `uname()` 实现，但 `getdomainname()` 在某些架构上有独立实现

## 3. 函数调用栈

```
SYSCALL_DEFINE2(setdomainname, name, len)              // kernel/sys.c
  ├─ 权限检查: ns_capable(uts_ns->user_ns, CAP_SYS_ADMIN)
  │    无 CAP_SYS_ADMIN → 返回 -EPERM
  ├─ 参数校验: len < 0 || len > __NEW_UTS_LEN (64)
  │    非法长度 → 返回 -EINVAL
  ├─ copy_from_user(tmp, name, len)                     // 从用户空间拷贝数据
  │    拷贝失败 → 返回 -EFAULT
  ├─ add_device_randomness(tmp, len)                    // 增加系统熵池随机性
  ├─ down_write(&uts_sem)                               // 获取 UTS 写锁
  ├─ memcpy(u->domainname, tmp, len)                    // 写入 domainname
  ├─ memset(u->domainname + len, 0, 剩余空间)           // 清空尾部
  ├─ uts_proc_notify(UTS_PROC_DOMAINNAME)               // 通知 /proc/sys 更新
  ├─ up_write(&uts_sem)                                 // 释放 UTS 写锁
  └─ return 0
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

### 4.2 uts_sem（UTS 信号量）

```c
// kernel/sys.c
DECLARE_RWSEM(uts_sem);  // 保护 UTS 命名空间字段的读写信号量
```

- 读操作（如 `uname()`）使用 `down_read()`/`up_read()`
- 写操作（如 `sethostname()`/`setdomainname()`）使用 `down_write()`/`up_write()`

### 4.3 utsname() 内联函数

```c
// include/linux/utsname.h
static inline struct new_utsname *utsname(void)
{
    return &current->nsproxy->uts_ns->name;
}
```

通过 `current->nsproxy->uts_ns` 访问当前进程的 UTS 命名空间，使得每个容器可以拥有独立的主机名和域名。

## 5. 流程图

```
用户态调用 setdomainname(name, len)
    │
    ▼
┌─────────────────────────────────────┐
│  系统调用入口 (sys_setdomainname)    │
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
│  memcpy(u->domainname, tmp, len)   │  ← 写入域名
│  memset(尾部, 0, 剩余空间)          │  ← 清空尾部
│  uts_proc_notify()                 │  ← 通知 proc 文件系统
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
    const char *domain = "example.com";
    int len = strlen(domain);

    // 设置 NIS 域名
    if (syscall(SYS_setdomainname, domain, len) == -1) {
        perror("setdomainname");
        return 1;
    }

    printf("Domain name set successfully\n");

    // 通过 uname 验证
    struct utsname buf;
    if (uname(&buf) == 0) {
        printf("Domainname: %s\n", buf.domainname);
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#系统标识与信息)
- 源码: `kernel/sys.c`
- 头文件: `include/uapi/linux/utsname.h`, `include/linux/utsname.h`
- `/proc/sys/kernel/domainname` 通过 `kernel/utsname_sysctl.c` 中的 `uts_kern_table` 导出