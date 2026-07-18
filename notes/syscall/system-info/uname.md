# uname 系统调用分析

## 1. 概述

`uname`（及 `newuname`）获取当前系统的名称和版本信息，包括系统名、主机名、内核版本、内核版本号、硬件架构和域名。信息存储在 UTS 命名空间中，每个容器可以有独立的设置。

**原型：**

```c
// 现代版本（推荐使用）
SYSCALL_DEFINE1(newuname, struct new_utsname __user *, name)

// 旧版本（兼容）
SYSCALL_DEFINE1(uname, struct old_utsname __user *, name)
SYSCALL_DEFINE1(olduname, struct oldold_utsname __user *, name)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `name` | `struct new_utsname __user *` | 输出缓冲区，接收 UTS 名称信息 |

**返回值：**
- 成功返回 0
- 失败返回 `-EFAULT`

## 2. 使用场景

- `uname -a` 命令显示系统信息
- 程序检测内核版本以适配特性
- `hostname` 命令读取主机名（通过 `uname` 获取 `nodename`）
- 容器内获取主机名
- 版本兼容性检查

## 3. 函数调用栈

```
SYSCALL_DEFINE1(newuname, name)                          // kernel/sys.c
  ├─ down_read(&uts_sem)                                  // 获取 UTS 读锁
  ├─ memcpy(&tmp, utsname(), sizeof(tmp))                 // 拷贝 UTS 信息
  ├─ up_read(&uts_sem)                                    // 释放 UTS 读锁
  ├─ copy_to_user(name, &tmp, sizeof(tmp))                // 拷贝到用户空间
  │    拷贝失败 → 返回 -EFAULT
  ├─ override_release(name->release, sizeof(name->release)) // 可选：覆盖 release
  ├─ override_architecture(name)                           // 可选：覆盖架构
  └─ return 0
```

### 3.1 旧版本兼容

```c
// 旧版 uname（struct old_utsname 使用更短的字段）
SYSCALL_DEFINE1(uname, struct old_utsname __user *, name)
{
    // 类似 newuname，但使用 __OLD_UTS_LEN（8）而非 __NEW_UTS_LEN（64）
}

// 更旧的版本（struct oldold_utsname）
SYSCALL_DEFINE1(olduname, struct oldold_utsname __user *, name)
{
    // 更小的字段长度，兼容非常古老的二进制程序
}
```

### 3.2 override_architecture

```c
// 当运行 32 位兼容模式时，将 machine 字段覆盖为 32 位架构名
#ifdef COMPAT_UTS_MACHINE
#define override_architecture(name) \
    (personality(current->personality) == PER_LINUX32 && \
     copy_to_user(name->machine, COMPAT_UTS_MACHINE, sizeof(COMPAT_UTS_MACHINE)))
#else
#define override_architecture(name) 0
#endif
```

## 4. 关键数据结构

### 4.1 struct new_utsname（UTS 名称信息）

```c
// include/uapi/linux/utsname.h
#define __NEW_UTS_LEN 64

struct new_utsname {
    char sysname[__NEW_UTS_LEN + 1];    // 系统名（如 "Linux"）
    char nodename[__NEW_UTS_LEN + 1];   // 主机名（由 sethostname 设置）
    char release[__NEW_UTS_LEN + 1];    // 内核版本（如 "6.12.0-rc1"）
    char version[__NEW_UTS_LEN + 1];    // 版本号（如 "#1 SMP PREEMPT ..."）
    char machine[__NEW_UTS_LEN + 1];    // 硬件架构（如 "aarch64", "x86_64"）
    char domainname[__NEW_UTS_LEN + 1]; // 域名（由 setdomainname 设置）
};
```

### 4.2 旧版本 uname 结构体

```c
// include/uapi/linux/utsname.h
struct old_utsname {
    char sysname[9];      // 更短的字段（9 字节）
    char nodename[9];
    char release[9];
    char version[9];
    char machine[9];
};
```

### 4.3 utsname() 访问函数

```c
// include/linux/utsname.h
static inline struct new_utsname *utsname(void)
{
    return &current->nsproxy->uts_ns->name;
}
```

## 5. 流程图

```
用户态调用 uname(name)
    │
    ▼
┌─────────────────────────────────────┐
│  down_read(&uts_sem)                │  ← 获取读锁
│  memcpy(&tmp, utsname(), sizeof)    │  ← 拷贝内核 UTS 数据
│  up_read(&uts_sem)                  │  ← 释放读锁
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  copy_to_user(name, &tmp, sizeof)  │
│  失败 → 返回 -EFAULT                │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  override_release()                 │  ← 可选：覆盖内核版本号
│  override_architecture()            │  ← 可选：覆盖硬件架构
│  (32位兼容模式时生效)               │
└─────────────────────────────────────┘
    │
    ▼
  返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EFAULT` | 地址错误 | `name` 指向的用户空间地址不可写 |

## 7. 使用示例

```c
#include <sys/utsname.h>
#include <stdio.h>

int main(void)
{
    struct utsname buf;

    if (uname(&buf) == -1) {
        perror("uname");
        return 1;
    }

    printf("Sysname:    %s\n", buf.sysname);
    printf("Nodename:   %s\n", buf.nodename);
    printf("Release:    %s\n", buf.release);
    printf("Version:    %s\n", buf.version);
    printf("Machine:    %s\n", buf.machine);

#ifdef _GNU_SOURCE
    printf("Domainname: %s\n", buf.domainname);
#endif

    return 0;
}
```

### 内核版本解析示例

```c
#include <sys/utsname.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct utsname buf;
    uname(&buf);

    int major, minor, patch;
    if (sscanf(buf.release, "%d.%d.%d", &major, &minor, &patch) == 3) {
        printf("Kernel version: %d.%d.%d\n", major, minor, patch);
        if (major >= 6) {
            printf("Rseq support available\n");
        }
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#系统标识与信息)
- 源码: `kernel/sys.c`（`SYSCALL_DEFINE1(newuname)`）
- 头文件: `include/uapi/linux/utsname.h`, `include/linux/utsname.h`
- 相关系统调用: `sethostname()`, `setdomainname()`, `gethostname()`
- 用户态命令: `uname`（coreutils 包）