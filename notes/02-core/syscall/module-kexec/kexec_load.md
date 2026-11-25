# kexec_load 系统调用分析

## 1. 概述

`kexec_load` 加载一个新的内核镜像到内存中，用于后续的热重启（`kexec` 重启）。调用者通过 `kexec_segment` 数组指定要加载的各个段（内核代码、initrd、命令行等）。

**原型：**

```c
SYSCALL_DEFINE4(kexec_load, unsigned long, entry, unsigned long, nr_segments,
                struct kexec_segment __user *, segments, unsigned long, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `entry` | `unsigned long` | 新内核的入口点地址 |
| `nr_segments` | `unsigned long` | 段数量 |
| `segments` | `struct kexec_segment __user *` | 段数组指针 |
| `flags` | `unsigned long` | 标志位（`KEXEC_ON_CRASH` 等） |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- `kexec -l` 命令加载新内核
- kdump 捕获内核加载（`KEXEC_ON_CRASH` 标志）
- 快速重启（跳过 BIOS/UEFI 初始化）
- 内核热升级

## 3. 函数调用栈

```
__sys_kexec_load(entry, nr_segments, segments, flags)     // kernel/kexec.c
  └─ kexec_load(entry, nr_segments, segments, flags)
       ├─ 权限检查: capable(CAP_SYS_BOOT)
       │    无权限 → 返回 -EPERM
       ├─ 检查 kexec 是否被禁用
       │    已禁用 → 返回 -EPERM
       ├─ [nr_segments > KEXEC_SEGMENT_MAX] → 返回 -EINVAL
       │    段数量超限
       ├─ copy_from_user(segments, ...)                    // 拷贝段数据到内核
       │    失败 → 返回 -EFAULT
       ├─ kimage_alloc_init(entry, nr_segments, segments, flags) // 分配镜像
       │    ├─ kimage_alloc(entry, flags)                  // 分配 kimage 结构
       │    └─ 验证段的合法性
       │         ├─ 检查段地址不重叠
       │         └─ 检查段在物理内存范围内
       ├─ kimage_load_segments(image, segments)            // 加载各段
       │    ├─[KEXEC_DESTINATION] → kimage_load_normal_segment()
       │    │    └─ 将段数据复制到目标内存位置
       │    └─[KEXEC_SOURCE] → kimage_load_crash_segment()
       │         └─ 将段数据复制到保留的 crash 内存区域
       ├─ 保存 kexec 镜像
       └─ return 0
```

### 3.1 段加载方式

```c
// kernel/kexec.c
static int kimage_load_normal_segment(struct kimage *image,
                                      struct kexec_segment *segment)
{
    // 将段数据逐页复制到目标内存
    // 使用 kimage_alloc_page() 分配物理页面
    // 通过 copy_to_user() 或 memcpy() 复制数据
}
```

## 4. 关键数据结构

### 4.1 struct kexec_segment（kexec 段）

```c
// include/uapi/linux/kexec.h
struct kexec_segment {
    const void *buf;       // 用户空间源缓冲区指针
    size_t bufsz;          // 源缓冲区大小
    const void *mem;       // 目标物理内存地址
    size_t memsz;          // 目标内存大小
};
```

### 4.2 struct kimage（kexec 镜像）

```c
// include/linux/kexec.h
struct kimage {
    kimage_entry_t head;               // 段链表头
    unsigned long start;               // 入口点
    struct page *control_code_page;    // 控制代码页
    unsigned long control_code_data;   // 控制代码数据
    unsigned long nr_segments;         // 段数量
    struct kexec_segment segment[];    // 段数组
    unsigned long segment_mem[];       // 段内存地址
    struct list_head control_pages;    // 控制页链表
    struct list_head dest_pages;       // 目标页链表
    struct list_head unuseable_pages;  // 不可用页链表
    struct list_head free_pages;       // 空闲页链表
    unsigned long type;                // 镜像类型
    struct purgatory_info purgatory;   // 清洗信息
};
```

### 4.3 kexec 标志位

```c
// include/uapi/linux/kexec.h
#define KEXEC_ON_CRASH        0x00000001  // 加载 crash dump 内核
#define KEXEC_PRESERVE_CONTEXT 0x00000002 // 保留硬件上下文
#define KEXEC_ARCH_MASK       0xffff0000  // 架构掩码
#define KEXEC_ARCH_DEFAULT    0x00000000  // 默认架构
#define KEXEC_ARCH_32         0x00002000  // 32 位架构
#define KEXEC_ARCH_64         0x00004000  // 64 位架构
```

## 5. 流程图

```
用户态调用 kexec_load(entry, nr_segments, segments, flags)
    │
    ▼
┌─────────────────────────────────────┐
│  权限检查: CAP_SYS_BOOT             │
│  kexec_load_disabled 检查           │
│  无权限 → 返回 -EPERM               │
│  nr_segments > 上限 → 返回 -EINVAL  │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  copy_from_user(segments)           │  ← 拷贝段信息
│  kimage_alloc_init()               │  ← 分配镜像
│  ├─ 验证段地址合法性                │
│  ├─ 分配物理页面                    │
│  └─ 初始化控制代码                  │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  kimage_load_segments()             │  ← 加载各段
│  ├─ 对每个 segment:                 │
│  │  ├─ kimage_alloc_page()         │  ← 分配物理页
│  │  └─ 复制段数据到目标内存        │
│  └─ 所有段加载完成                  │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  保存 kexec 镜像到全局变量          │
│  (kexec_image 或 crash_kexec_image) │
│  返回 0                             │
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EPERM` | 权限不足 | 调用者没有 `CAP_SYS_BOOT`，或 `kexec_load_disabled` 被设置 |
| `-EINVAL` | 无效参数 | `nr_segments` 超限，或段地址无效 |
| `-EFAULT` | 地址错误 | 从用户空间拷贝 `segments` 数据失败 |
| `-ENOMEM` | 内存不足 | 内核分配内存或物理页面失败 |
| `-EBUSY` | kexec 忙 | 另一个 kexec 操作正在进行中 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/kexec.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(void)
{
    // 读取内核镜像
    int fd = open("/boot/vmlinuz-6.12.0", O_RDONLY);
    if (fd < 0) {
        perror("open kernel");
        return 1;
    }

    struct stat st;
    fstat(fd, &st);

    void *kernel_buf = mmap(NULL, st.st_size, PROT_READ,
                            MAP_PRIVATE, fd, 0);
    if (kernel_buf == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    // 构建 kexec 段
    // 实际应用中需要解析 ELF 或 PE 格式来确定入口点和段位置
    // 这里仅为示例
    struct kexec_segment segs[2];
    segs[0].buf = kernel_buf;     // 内核代码段
    segs[0].bufsz = st.st_size;
    segs[0].mem = (void *)0x1000000UL;  // 目标物理地址
    segs[0].memsz = st.st_size;

    // 加载 kexec 镜像
    if (syscall(SYS_kexec_load, 0x1000000UL, 1, segs, 0) == -1) {
        perror("kexec_load");
        munmap(kernel_buf, st.st_size);
        close(fd);
        return 1;
    }

    printf("Kexec kernel loaded successfully\n");
    printf("Run 'kexec -e' to execute the new kernel\n");

    munmap(kernel_buf, st.st_size);
    close(fd);
    return 0;
}
```

## 8. 参考

- 源码: `kernel/kexec.c`（`__sys_kexec_load()` 和 `kimage_load_segments()`）
- 头文件: `include/linux/kexec.h`, `include/uapi/linux/kexec.h`
- 配置选项: `CONFIG_KEXEC`, `CONFIG_KEXEC_FILE`, `CONFIG_CRASH_DUMP`
- 用户态命令: `kexec`（kexec-tools 包）
- 相关系统调用: `kexec_file_load()`, `reboot()`