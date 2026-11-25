# kexec_file_load 系统调用分析

## 1. 概述

`kexec_file_load` 通过文件描述符加载 kexec 内核。与 `kexec_load` 不同，它直接从文件读取内核和 initrd，支持签名验证（`CONFIG_KEXEC_SIG`），提供更安全的内核加载方式。

**原型：**

```c
SYSCALL_DEFINE5(kexec_file_load, int, kernel_fd, int, initrd_fd,
                unsigned long, cmdline_len, const char __user *, cmdline_ptr,
                unsigned long, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `kernel_fd` | `int` | 内核镜像文件的文件描述符 |
| `initrd_fd` | `int` | initrd 文件的文件描述符（-1 表示无 initrd） |
| `cmdline_len` | `unsigned long` | 命令行参数字符串长度 |
| `cmdline_ptr` | `const char __user *` | 命令行参数字符串指针 |
| `flags` | `unsigned long` | 保留标志位，当前必须为 0 |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- `kexec -l` 命令加载新内核（基于文件描述符）
- kdump 内核加载（与 `kexec_load` 类似但更安全）
- 需要签名验证的安全启动场景
- 快速重启和内核热替换

## 3. 函数调用栈

```
SYSCALL_DEFINE5(kexec_file_load, kernel_fd, initrd_fd, ...) // kernel/kexec_file.c
  ├─ 权限检查: capable(CAP_SYS_BOOT)
  │    无权限 → 返回 -EPERM
  ├─ flags != 0 → 返回 -EINVAL
  ├─ 检查 kexec 是否被禁用 (kexec_load_disabled)
  │    已禁用 → 返回 -EPERM
  ├─ 检查锁定状态 (kexec_lock mutex)
  ├─ kexec_file_load(kernel_fd, initrd_fd, cmdline_len, cmdline_ptr, flags)
  │    ├─ kernel_read_file_from_fd(kernel_fd, ...)           // 读取内核文件
  │    ├─ [initrd_fd >= 0]
  │    │    └─ kernel_read_file_from_fd(initrd_fd, ...)      // 读取 initrd 文件
  │    ├─ arch_kexec_kernel_image_probe(image, ...)           // 探测内核格式
  │    │    ├─ 尝试 ELF 格式 (kexec_elf_probe)
  │    │    └─ 尝试 PE 格式 (kexec_pe_probe) 等
  │    ├─ [CONFIG_KEXEC_SIG] signature verification           // 签名验证
  │    │    └─ verify_pefile_signature() / verify_pkcs7_signature()
  │    ├─ kimage_alloc_init()                                 // 分配 kexec 镜像
  │    ├─ kimage_load_file_segments(image, segments)          // 加载各段
  │    └─ 清理临时数据
  └─ return 0
```

### 3.1 内核格式探测

`kexec_file_load` 支持多种内核镜像格式：

| 格式 | 探测函数 | 说明 |
|------|---------|------|
| ELF (vmlinux) | `kexec_elf_probe()` | 标准的 ELF 格式内核 |
| PE (bzImage) | `kexec_pe_probe()` | x86 的 bzImage 格式 |
| PE (Image) | `kexec_pe_probe()` | ARM64 的 Image 格式 |

## 4. 关键数据结构

### 4.1 struct kimage（kexec 镜像）

```c
// include/linux/kexec.h
struct kimage {
    kimage_entry_t head;               // 段链表头
    unsigned long start;               // 入口点地址
    struct page *control_code_page;    // 控制代码页
    unsigned long nr_segments;         // 段数量
    struct kexec_segment segment[];    // 段数组
    unsigned long type;                // 镜像类型
    struct purgatory_info purgatory;   // 清洗（purgatory）信息
    // ...
};
```

### 4.2 struct kexec_segment（kexec 段）

```c
// include/uapi/linux/kexec.h
struct kexec_segment {
    const void *buf;       // 源缓冲区（用户空间）
    size_t bufsz;          // 源缓冲区大小
    const void *mem;       // 目标内存地址
    size_t memsz;          // 目标内存大小
};
```

## 5. 流程图

```
用户态调用 kexec_file_load(kernel_fd, initrd_fd, ...)
    │
    ▼
┌─────────────────────────────────────┐
│  权限检查: CAP_SYS_BOOT             │
│  kexec_load_disabled 检查           │
│  无权限 → 返回 -EPERM               │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  kernel_read_file_from_fd()         │  ← 从 fd 读取内核镜像
│  [initrd_fd >= 0]                   │  ← 读取 initrd
│    kernel_read_file_from_fd()       │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  arch_kexec_kernel_image_probe()    │  ← 探测内核格式
│  ├─ kexec_elf_probe()               │  ← ELF 格式?
│  ├─ 或 kexec_pe_probe()             │  ← PE 格式?
│  └─ 解析内核段信息                  │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  [CONFIG_KEXEC_SIG]                 │
│  signature verification             │  ← 签名验证
│  ├─ 验证内核签名                    │
│  └─ 验证失败 → 返回 -EKEYREJECTED  │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  kimage_alloc_init()                │  ← 分配镜像结构
│  kimage_load_file_segments()        │  ← 加载各段到内存
│  └─ 将内核和 initrd 复制到预分配内存│
└─────────────────────────────────────┘
    │
    ▼
  返回 0 (kexec 镜像已加载)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EPERM` | 权限不足 | 调用者没有 `CAP_SYS_BOOT`，或 `kexec_load_disabled` 被设置 |
| `-EINVAL` | 无效参数 | `flags` 非零，或内核镜像格式无效 |
| `-EBADF` | 文件描述符无效 | `kernel_fd` 或 `initrd_fd` 无效 |
| `-ENOEXEC` | 格式错误 | 内核镜像不是可识别的格式 |
| `-EKEYREJECTED` | 签名验证失败 | 签名验证失败（启用 `CONFIG_KEXEC_SIG` 时） |
| `-ENOMEM` | 内存不足 | 内核分配内存失败 |
| `-EBUSY` | kexec 忙 | 另一个 kexec 操作正在进行中 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <string.h>

int main(void)
{
    // 打开内核镜像文件
    int kernel_fd = open("/boot/vmlinuz-6.12.0", O_RDONLY);
    if (kernel_fd < 0) {
        perror("open kernel");
        return 1;
    }

    // 打开 initrd 文件
    int initrd_fd = open("/boot/initrd.img-6.12.0", O_RDONLY);
    if (initrd_fd < 0) {
        perror("open initrd");
        close(kernel_fd);
        return 1;
    }

    const char *cmdline = "root=/dev/sda1 ro quiet";

    // 加载 kexec 内核
    if (syscall(SYS_kexec_file_load, kernel_fd, initrd_fd,
                strlen(cmdline) + 1, cmdline, 0) == -1) {
        perror("kexec_file_load");
        close(kernel_fd);
        close(initrd_fd);
        return 1;
    }

    printf("Kexec kernel loaded successfully via fd\n");
    printf("Run 'kexec -e' to execute the new kernel\n");

    close(kernel_fd);
    close(initrd_fd);
    return 0;
}
```

## 8. 参考

- 源码: `kernel/kexec_file.c`（`SYSCALL_DEFINE5(kexec_file_load)`）
- 头文件: `include/linux/kexec.h`, `include/uapi/linux/kexec.h`
- 架构相关: `arch/*/kernel/kexec_elf.c`, `arch/*/kernel/kexec_pe.c`
- 配置选项: `CONFIG_KEXEC_FILE`, `CONFIG_KEXEC_SIG`
- 用户态命令: `kexec`（kexec-tools 包）
- 相关系统调用: `kexec_load()`, `reboot()`