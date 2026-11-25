# lookup_dcookie 系统调用分析

> 所属分类：其他杂项（已废弃） | [ARM64 系统调用表](../arm64-syscall-table.md#30-其他杂项)

## 1. 概述

`lookup_dcookie` 曾经用于把内核返回的 "dcookie"（dentry cookie）反查为完整路径字符串，主要服务于 OProfile 的文件名采样。cookie 是 dentry 地址的哈希标识，内核侧由 `get_dcookie()` 分配，用户态通过该系统调用取回路径。

**当前状态：该调用在 ARM64 上映射到 `sys_ni_syscall`，永远返回 `-ENOSYS`。**

- 64 位：`include/uapi/asm-generic/unistd.h:73` 定义 `__NR_lookup_dcookie 18`，`:74` 绑定到 `sys_ni_syscall`；
- 32 位兼容：`arch/arm64/tools/syscall_32.tbl:264` 为 `249 common lookup_dcookie sys_ni_syscall`；
- 原实现文件 `fs/dcookies.c` 已于 2021-01-14 由提交 `be65de6b03aa "fs: Remove dcookies support"` 删除（同时删除 `include/linux/dcookies.h` 与 `fs/Makefile`/`kernel/sys.c` 中的相关引用），删除前该文件共 356 行。

因此本文的"内核实现路径"描述的是历史实现，便于理解 ABI；在当前 7.2 源码树中不存在任何 `do_lookup_dcookie()` 代码。

## 2. 函数原型与参数

历史用户空间原型：

```c
int lookup_dcookie(u64 cookie, char *buffer, size_t len);
```

历史内核入口（已删除的 `fs/dcookies.c`）：

```c
SYSCALL_DEFINE3(lookup_dcookie, u64, cookie64, char __user *, buf, size_t, len)
{
    return do_lookup_dcookie(cookie64, buf, len);
}
```

兼容 32 位 ABI：

```c
COMPAT_SYSCALL_DEFINE4(lookup_dcookie, u32, w0, u32, w1,
                       char __user *, buf, compat_size_t, len)
```

| 参数 | 说明 |
|------|------|
| `cookie` | 由 `get_dcookie()` 产生的 64 位 cookie（实际是 `dentry` 指针的低位值） |
| `buf` | 输出缓冲区，接收以 NUL 结尾的绝对路径 |
| `len` | 缓冲区长度 |

当前系统调用表中该编号的入口是：

```c
/* include/uapi/asm-generic/unistd.h */
#define __NR_lookup_dcookie 18
__SYSCALL(__NR_lookup_dcookie, sys_ni_syscall)
```

## 3. 内核实现路径

当前（7.2，ARM64）：

```
__arm64_sys_lookup_dcookie(cookie, buf, len)
  └─ sys_ni_syscall()
       └─ return -ENOSYS;
```

历史实现（`fs/dcookies.c`，已删除）：

```
__arm64_sys_lookup_dcookie(cookie64, buf, len)
  └─ do_lookup_dcookie(cookie64, buf, len)
       ├─ capable(CAP_SYS_ADMIN) ? 否则 -EPERM
       │    /* 防止向无目录读权限的用户泄露路径 */
       ├─ mutex_lock(&dcookie_mutex)
       ├─ is_live() ? 否则 -EINVAL
       │    /* dcookie_users 链表为空表示没有内核用户注册 */
       ├─ dcs = find_dcookie(cookie)
       │    未找到 → -EINVAL
       ├─ kbuf = kmalloc(PAGE_SIZE, GFP_KERNEL)
       │    失败 → -ENOMEM
       ├─ path = d_path(&dcs->path, kbuf, PAGE_SIZE)
       ├─ mutex_unlock(&dcookie_mutex)
       ├─ path 为错误指针 → 返回其错误码
       ├─ pathlen = kbuf + PAGE_SIZE - path
       ├─ pathlen <= len ? copy_to_user(buf, path, pathlen) : -ERANGE
       └─ 返回 pathlen（包含结尾 NUL）
```

内核侧配套接口（同样已删除）：

```
get_dcookie(path, &cookie)        // 供 OProfile 等内核模块调用
  ├─ is_live() 为假 → -EINVAL
  ├─ dentry 带 DCACHE_COOKIE → find_dcookie()
  └─ 否则 alloc_dcookie() → 建立 dentry 地址到 path 的哈希项
```

以及 `dcookie_register()` / `dcookie_unregister()`，用于内核使用者注册/注销；只有存在注册者时 `is_live()` 才为真。

## 4. 核心数据结构

历史结构（已随文件删除）：

```c
// fs/dcookies.c
struct dcookie_struct {
    struct path path;             /* 关联的 dentry + vfsmount */
    struct list_head hash_list;   /* 哈希桶链 */
};

static LIST_HEAD(dcookie_users);          /* 已注册的内核使用者 */
static DEFINE_MUTEX(dcookie_mutex);
static struct kmem_cache *dcookie_cache;
static struct list_head *dcookie_hashtable;
static size_t hash_size;
```

cookie 值直接取 dentry 指针：

```c
static inline unsigned long dcookie_value(struct dcookie_struct *dcs)
{
    return (unsigned long)dcs->path.dentry;
}
```

因此 cookie 不是稳定的文件标识：dentry 被回收后 cookie 失效，同一路径重新解析可能得到不同 cookie。这也是该接口最终被淘汰的原因之一。

`DCACHE_COOKIE` 是 dentry 标志，表示该 dentry 已分配 dcookie。

## 5. 关键流程

```
【历史】内核采样模块（如 OProfile）
        │
        ├─ dcookie_register()          → 加入 dcookie_users
        ├─ get_dcookie(&path, &cookie) → 创建/复用哈希项
        └─ 把 cookie 记录到采样数据
        │
        ▼
用户态分析工具
        │
        ▼
lookup_dcookie(cookie, buf, len)
  ├─ CAP_SYS_ADMIN 检查
  ├─ cookie 查表
  ├─ d_path() 生成绝对路径
  └─ 拷贝到用户缓冲区，返回路径长度

【当前】
lookup_dcookie(...) → sys_ni_syscall() → -ENOSYS
```

删除该接口的动因：OProfile 的 dcookie 使用已被移除，dcookie 依赖 dentry 地址，语义脆弱且在容器/命名空间环境下路径表示不唯一；同时维护一个仅服务于单一 profiling 工具的全局哈希表不划算。现代性能分析使用 `perf` 的 mmap 记录、`d_path` 的 eBPF 辅助或直接读取 `/proc/<pid>/maps`。

## 6. 返回值与错误码

当前实现：

| 返回值 | 说明 |
|--------|------|
| `-ENOSYS` | 恒定返回，功能不存在 |

历史错误码：

| 错误码 | 触发条件 |
|--------|----------|
| `-EPERM` | 调用者没有 `CAP_SYS_ADMIN` |
| `-EINVAL` | 没有 dcookie 使用者注册（`is_live()` 为假），或 cookie 查表失败 |
| `-ENOMEM` | 分配路径缓冲区失败 |
| `-ERANGE` | 用户缓冲区小于路径长度（含 NUL） |
| `-EFAULT` | `copy_to_user()` 失败 |
| 正数 | 成功时返回写入的字节数（包含结尾 `\0`） |

## 7. 使用示例

当前内核上调用会直接失败：

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    char buf[4096];
    long ret = syscall(__NR_lookup_dcookie, 0ULL, buf, sizeof(buf));
    if (ret < 0)
        printf("lookup_dcookie: %s (errno=%d)\n", strerror(errno), errno);
    return 0;
}
```

在 7.2 ARM64 上输出恒为 `Function not implemented`（`ENOSYS`）。

历史用法示意（仅供理解 ABI，已无法运行）：

```c
/* 假设内核模块已通过 get_dcookie() 把 cookie 交给了用户态 */
unsigned long cookie = ...;
char path[4096];
int n = syscall(__NR_lookup_dcookie, (unsigned long long)cookie,
                path, sizeof(path));
if (n > 0)
    printf("path = %s (%d bytes)\n", path, n);
```

替代方案：需要路径信息时使用 `/proc/<pid>/maps`、`/proc/<pid>/fd/*` 的 `readlink()`，或 perf 的 `PERF_RECORD_MMAP` 事件。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `include/uapi/asm-generic/unistd.h` | `__NR_lookup_dcookie 18`，绑定 `sys_ni_syscall`（73-74 行） |
| `arch/arm64/include/generated/uapi/asm/unistd_64.h` | 64 位编号 18 |
| `arch/arm64/tools/syscall_32.tbl` | 32 位兼容编号 249 → `sys_ni_syscall` |
| `kernel/sys_ni.c` | `sys_ni_syscall()` 实现（返回 `-ENOSYS`） |
| 历史文件（已删除）`fs/dcookies.c` | `SYSCALL_DEFINE3(lookup_dcookie)`、`do_lookup_dcookie()`、`get_dcookie()` |
| 历史文件（已删除）`include/linux/dcookies.h` | `dcookie_register()`/`get_dcookie()` 声明 |
