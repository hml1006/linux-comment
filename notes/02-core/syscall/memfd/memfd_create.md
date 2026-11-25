# memfd_create 系统调用分析

## 1. 概述

`memfd_create` 创建一个匿名内存文件（memory file），返回一个文件描述符。该文件完全在内存中（基于 tmpfs），可用于共享内存、文件密封（sealing）等场景。

**原型：**

```c
SYSCALL_DEFINE2(memfd_create, const char __user *, uname, unsigned int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `uname` | `const char __user *` | 文件名（仅用于调试，在 `/proc/self/fd/` 中可见） |
| `flags` | `unsigned int` | 标志位（`MFD_CLOEXEC`、`MFD_ALLOW_SEALING`、`MFD_HUGETLB` 等） |

**返回值：**
- 成功返回新的文件描述符
- 失败返回负的错误码

## 2. 使用场景

- 共享内存（不需要实际文件系统路径）
- 文件密封（sealing）机制防止篡改
- 内存映射 I/O（mmap）
- 替代 `tmpfile()` 的更安全实现
- 图形驱动和媒体编解码（传递文件描述符）

## 3. 函数调用栈

```
__do_sys_memfd_create(uname, flags)                      // mm/memfd.c
  └─ do_memfd_create(uname, flags)
       ├─ get_unused_fd_flags(flags & MFD_CLOEXEC ? O_CLOEXEC : 0) // 获取 fd 编号
       ├─ shmem_file_setup(name, 0, VM_NORESERVE)        // 创建 tmpfs 文件
       │    └─ shmem_get_inode(sb, ...)                  // 创建 tmpfs inode
       │         └─ new_inode(sb)                         // 分配新 inode
       ├─ file->f_mode = FMODE_READ | FMODE_WRITE | FMODE_LSEEK
       ├─ [MFD_ALLOW_SEALING] file->f_op = &memfd_fops    // 设置文件操作（支持密封）
       │   [无 MFD_ALLOW_SEALING] file->f_op = &shmem_fops
       ├─ [MFD_HUGETLB] 设置 hugetlb 标志
       └─ fd_install(fd, file)                           // 安装 fd 到当前进程
```

### 3.1 memfd 文件操作

```c
// mm/memfd.c
static const struct file_operations memfd_fops = {
    .open = simple_open,
    .release = memfd_release,
    .read_iter = memfd_read_iter,
    .write_iter = memfd_write_iter,
    .llseek = memfd_llseek,
    .mmap = memfd_mmap,
    .get_unmapped_area = memfd_get_unmapped_area,
};
```

## 4. 关键数据结构

### 4.1 memfd_create 标志位

```c
// include/uapi/linux/memfd.h
#define MFD_CLOEXEC         0x0001U  // 设置 FD_CLOEXEC 标志
#define MFD_ALLOW_SEALING   0x0002U  // 允许文件密封操作
#define MFD_HUGETLB         0x0004U  // 使用 hugetlb 页面
#define MFD_NOEXEC_SEAL     0x0008U  // 不允许执行，且启用密封
#define MFD_EXEC            0x0010U  // 允许执行

// hugetlb 大小编码（在 MFD_HUGETLB 标志中编码）
#define MFD_HUGE_SHIFT      26       // hugetlb 大小编码移位
#define MFD_HUGE_MASK       0x3f     // hugetlb 大小掩码
#define MFD_HUGE_64KB       (16 << MFD_HUGE_SHIFT)  // 64KB hugetlb
#define MFD_HUGE_512KB      (19 << MFD_HUGE_SHIFT)  // 512KB hugetlb
#define MFD_HUGE_1MB        (20 << MFD_HUGE_SHIFT)  // 1MB hugetlb
#define MFD_HUGE_2MB        (21 << MFD_HUGE_SHIFT)  // 2MB hugetlb
#define MFD_HUGE_8MB        (23 << MFD_HUGE_SHIFT)  // 8MB hugetlb
#define MFD_HUGE_16MB       (24 << MFD_HUGE_SHIFT)  // 16MB hugetlb
#define MFD_HUGE_32MB       (25 << MFD_HUGE_SHIFT)  // 32MB hugetlb
#define MFD_HUGE_256MB      (28 << MFD_HUGE_SHIFT)  // 256MB hugetlb
#define MFD_HUGE_512MB      (29 << MFD_HUGE_SHIFT)  // 512MB hugetlb
#define MFD_HUGE_1GB        (30 << MFD_HUGE_SHIFT)  // 1GB hugetlb
#define MFD_HUGE_2GB        (31 << MFD_HUGE_SHIFT)  // 2GB hugetlb
#define MFD_HUGE_16GB       (34 << MFD_HUGE_SHIFT)  // 16GB hugetlb
```

## 5. 流程图

```
用户态调用 memfd_create(name, flags)
    │
    ▼
┌─────────────────────────────────────┐
│  参数校验                           │
│  name 拷贝到内核空间                │
│  flags 有效性检查                   │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  get_unused_fd_flags()              │  ← 分配新的 fd 编号
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  shmem_file_setup(name, 0, ...)    │  ← 创建 tmpfs 文件
│  ├─ shmem_get_inode()              │
│  │    └─ new_inode()               │  ← 分配 inode
│  └─ alloc_file()                   │  ← 分配 file 结构体
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  f_mode = FMODE_READ|WRITE|LSEEK   │
│  [MFD_ALLOW_SEALING]               │
│     f_op = &memfd_fops             │  ← 支持密封操作
│  [else]                            │
│     f_op = &shmem_fops             │  ← 使用普通 tmpfs 操作
│  [MFD_HUGETLB] 设置 hugetlb        │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  fd_install(fd, file)              │  ← 安装 fd
│  return fd                         │
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | `flags` 包含未定义的位，或组合无效 |
| `-EMFILE` | 文件描述符表满 | 当前进程的 fd 数已达上限 |
| `-ENOMEM` | 内存不足 | 内核分配内存失败 |
| `-ENOSYS` | 不支持 | 内核未配置 tmpfs 或 hugetlb |
| `-EFAULT` | 地址错误 | `uname` 指向的用户空间地址不可读 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <fcntl.h>

int main(void)
{
    // 创建 memfd，允许文件密封
    int fd = syscall(SYS_memfd_create, "myregion", MFD_ALLOW_SEALING);
    if (fd < 0) {
        perror("memfd_create");
        return 1;
    }

    // 扩展文件大小
    if (ftruncate(fd, 4096) < 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    // 写入数据
    const char *msg = "Hello, memfd!";
    write(fd, msg, strlen(msg) + 1);

    // 内存映射
    void *addr = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    printf("Content: %s\n", (char *)addr);

    // 使用文件密封锁定内容
    // fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);

    munmap(addr, 4096);
    close(fd);
    return 0;
}
```

## 8. 参考

- 源码: `mm/memfd.c`（`__do_sys_memfd_create()` 和 `do_memfd_create()`）
- 头文件: `include/uapi/linux/memfd.h`
- 底层 tmpfs 实现: `mm/shmem.c`
- 相关系统调用: `memfd_secret()`, `fcntl(F_ADD_SEALS)`, `mmap()`