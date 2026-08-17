# memfd_secret 系统调用分析

## 1. 概述

`memfd_secret` 创建一个"秘密"内存区域的文件描述符。该内存区域对内核的其他部分不可见——只有创建者进程可以通过 mmap 访问，防止来自内核其他子系统（如调试器、`/proc/$(pid)/maps`）的访问。

**原型：**

```c
SYSCALL_DEFINE1(memfd_secret, unsigned int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `flags` | `unsigned int` | 保留标志位，当前必须为 0 |

**返回值：**
- 成功返回新的文件描述符
- 失败返回负的错误码

## 2. 使用场景

- 加密密钥和安全敏感数据的内存管理
- 防止内核侧信道攻击（如 Spectre）
- 内存中的秘密数据保护（防止被 root 或内核模块读取）
- 安全敏感应用的内存隔离

## 3. 函数调用栈

```
__do_sys_memfd_secret(flags)                             // mm/secretmem.c
  └─ secretmem_create(flags)
       ├─ 检查是否支持 secretmem
       │    不支持 → 返回 -ENOSYS
       ├─ 权限检查 (capable(CAP_SYS_ADMIN))
       │    无 CAP_SYS_ADMIN → 返回 -EPERM
       ├─ 检查 flags → 非零返回 -EINVAL
       ├─ fd = get_unused_fd_flags(O_CLOEXEC)            // 获取 fd 编号
       ├─ file = secretmem_create_file()                  // 创建 secretmem 文件
       │    └─ alloc_file_pseudo(secretmem_inode, ...)    // 分配伪文件
       ├─ fd_install(fd, file)                            // 安装 fd
       └─ return fd
```

### 3.1 secretmem 文件操作

```c
// mm/secretmem.c
static const struct file_operations secretmem_fops = {
    .release = secretmem_release,
    .mmap = secretmem_mmap,
};
```

### 3.2 secretmem_mmap 关键流程

```c
// mm/secretmem.c
static int secretmem_mmap(struct file *file, struct vm_area_struct *vma)
{
    // 设置 VMA 标志
    vma->vm_flags |= VM_LOCKED | VM_DONTDUMP | VM_DONTCOPY;
    // VM_LOCKED: 页面锁定在内存中，不允许换出
    // VM_DONTDUMP: 不在 core dump 中包含此区域
    // VM_DONTCOPY: fork 时不复制此区域

    // 使用 secretmem 专用的页分配器
    // 分配的内存页在释放时会用零填充，避免数据残留
    return 0;
}
```

## 4. 关键数据结构

### 4.1 secretmem 底层机制

secretmem 使用以下技术确保安全性：

| 机制 | 说明 |
|------|------|
| `VM_LOCKED` | 页面锁定在物理内存中，不可换出 |
| `VM_DONTDUMP` | 不包含在 core dump 中 |
| `VM_DONTCOPY` | fork 时不复制到子进程 |
| 专用页分配器 | 分配时使用特殊页池，释放时清空内容 |
| 内核不可见 | 其他内核子系统无法访问这些页面 |

### 4.2 secretmem 的隔离特性

```
正常内存映射:
  进程A ──→ 物理页面 ←── 内核其他部分（可访问）
                    ←── /proc/$(pid)/maps

secretmem 映射:
  进程A ──→ 秘密物理页面 ←✗── 内核其他部分（不可访问）
                    ←✗── /proc/$(pid)/maps（不显示详细映射）
```

## 5. 流程图

```
用户态调用 memfd_secret(flags)
    │
    ▼
┌─────────────────────────────────────┐
│  检查 secretmem 支持                │
│  (依赖于 CONFIG_SECRETMEM)          │
│  不支持 → 返回 -ENOSYS              │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  权限检查                           │
│  capable(CAP_SYS_ADMIN)             │
│  无权限 → 返回 -EPERM               │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  flags != 0 → 返回 -EINVAL          │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  get_unused_fd_flags(O_CLOEXEC)     │  ← 分配新 fd
│  secretmem_create_file()            │  ← 创建 secretmem 文件
│  fd_install(fd, file)               │  ← 安装 fd
│  return fd                          │
└─────────────────────────────────────┘
    │
    ▼
用户态 mmap(fd, ...)
    │
    ▼
┌─────────────────────────────────────┐
│  secretmem_mmap()                   │
│  ├─ VM_LOCKED   (不可换出)          │
│  ├─ VM_DONTDUMP (不可 dump)         │
│  ├─ VM_DONTCOPY (不可 fork 复制)    │
│  └─ 分配秘密内存页                  │
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-ENOSYS` | 不支持 | 内核未配置 `CONFIG_SECRETMEM` |
| `-EPERM` | 权限不足 | 调用者没有 `CAP_SYS_ADMIN` 能力 |
| `-EINVAL` | 无效参数 | `flags` 非零 |
| `-EMFILE` | 文件描述符表满 | 当前进程的 fd 数已达上限 |
| `-ENOMEM` | 内存不足 | 内核分配内存失败 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/secretmem.h>  /* 如果头文件可用 */

#ifndef __NR_memfd_secret
#define __NR_memfd_secret 447
#endif

int main(void)
{
    // 创建 secret memfd
    int fd = syscall(__NR_memfd_secret, 0);
    if (fd < 0) {
        perror("memfd_secret");
        return 1;
    }

    // 映射秘密内存区域
    size_t size = 4096;
    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    // 存储敏感数据
    const char *secret = "my-secret-key-12345";
    memcpy(addr, secret, strlen(secret) + 1);

    printf("Secret stored at %p: %s\n", addr, (char *)addr);

    // 注意：此区域在 /proc/self/maps 中不会显示详细映射
    // 且不会出现在 core dump 中

    // 使用完毕后清空
    memset(addr, 0, size);
    munmap(addr, size);
    close(fd);
    return 0;
}
```

## 8. 参考

- 源码: `mm/secretmem.c`（`__do_sys_memfd_secret()` 和 `secretmem_create()`）
- 配置选项: `CONFIG_SECRETMEM`
- 相关系统调用: `memfd_create()`, `mmap()`
- 安全特性: 防止内核侧信道攻击、保护敏感数据