# Kdump — 崩溃转储与分析

## 概述

Kdump 是基于 kexec 的崩溃转储解决方案。当系统发生 panic 时，Kdump 使用 kexec 快速启动一个预先加载的 dump-capture 内核，将系统内核的内存镜像（vmcore）保存下来，供后续分析使用。

### 工作原理

```
系统内核启动时:
┌─────────────────────────────────────────────────────────────┐
│                    System Kernel                            │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  预留 crashkernel 内存区域 (例如: crashkernel=1G@1G) │   │
│  │  crashk_res.start ~ crashk_res.end                   │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘

用户空间加载 dump-capture 内核:
$ kexec -p vmlinuz-dump --initrd=initramfs-dump.img --append="..."
┌─────────────────────────────────────────────────────────────┐
│                    System Kernel                            │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  crashkernel 区域:                                   │   │
│  │  [dump-capture kernel] + [initramfs] + [cmdline]    │   │
│  └──────────────────────────────────────────────────────┘   │
│  kexec_crash_image = &image (已加载的 kimage 结构)         │
└─────────────────────────────────────────────────────────────┘

系统 panic 时:
panic() → crash_kexec() → __crash_kexec() → machine_kexec()
┌─────────────────────────────────────────────────────────────┐
│                    System Kernel (panic)                    │
│  1. crash_setup_regs() - 保存寄存器状态                    │
│  2. crash_save_vmcoreinfo() - 保存 vmcoreinfo             │
│  3. machine_crash_shutdown() - 关闭设备                   │
│  4. machine_kexec() - 切换到 dump-capture 内核             │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                  Dump-Capture Kernel                        │
│  /proc/vmcore → ELF 格式的内存镜像                          │
│  $ cp /proc/vmcore /mnt/vmcore                            │
│  $ makedumpfile -d 31 /proc/vmcore vmcore-filtered        │
└─────────────────────────────────────────────────────────────┘
```

## 核心数据结构

### struct kimage

kimage 是 kexec 的核心数据结构，描述要加载的新内核镜像：

```c
struct kimage {
    kimage_entry_t head;                          /* 链表头 */
    kimage_entry_t *entry;                        /* 当前入口 */
    kimage_entry_t *last_entry;                   /* 最后入口 */

    unsigned long start;                          /* 镜像起始地址 */
    struct page *control_code_page;               /* 控制代码页 */
    struct page *swap_page;                       /* 交换页 */
    void *vmcoreinfo_data_copy;                   /* vmcoreinfo 副本 */

    unsigned long nr_segments;                    /* 段数量 */
    struct kexec_segment segment[KEXEC_SEGMENT_MAX]; /* 段描述数组 */

    struct list_head control_pages;               /* 控制页链表 */
    struct list_head dest_pages;                  /* 目标页链表 */
    struct list_head unusable_pages;              /* 不可用页链表 */

    unsigned int type : 1;                        /* 类型: DEFAULT 或 CRASH */
#define KEXEC_TYPE_DEFAULT 0
#define KEXEC_TYPE_CRASH   1
    unsigned int preserve_context : 1;            /* 保留上下文 */
    unsigned int file_mode:1;                     /* 文件模式 */

    void *elf_headers;                            /* ELF 头缓冲区 */
    unsigned long elf_headers_sz;                 /* ELF 头大小 */
    unsigned long elf_load_addr;                  /* ELF 加载地址 */
};
```

### struct kexec_segment

描述内核镜像的一个段：

```c
struct kexec_segment {
    union {
        void __user *buf;      /* 用户空间缓冲区 */
        void *kbuf;            /* 内核空间缓冲区 */
    };
    size_t bufsz;              /* 缓冲区大小 */
    unsigned long mem;         /* 目标内存地址 */
    size_t memsz;              /* 目标内存大小 */
};
```

## 核心函数

### crash_kexec()

panic 时调用的入口函数，确保只有一个 CPU 执行崩溃转储：

```c
__bpf_kfunc void crash_kexec(struct pt_regs *regs)
{
    if (panic_try_start()) {
        __crash_kexec(regs);
        panic_reset();
    }
}
```

### __crash_kexec()

实际执行崩溃转储的核心函数：

```c
void __noclone __crash_kexec(struct pt_regs *regs)
{
    if (kexec_trylock()) {
        if (kexec_crash_image) {
            struct pt_regs fixed_regs;

            crash_setup_regs(&fixed_regs, regs);   /* 设置寄存器 */
            crash_save_vmcoreinfo();               /* 保存 vmcoreinfo */
            machine_crash_shutdown(&fixed_regs);   /* 关闭设备 */
            crash_cma_clear_pending_dma();         /* 等待 DMA 完成 */
            machine_kexec(kexec_crash_image);      /* 启动 dump 内核 */
        }
        kexec_unlock();
    }
}
```

### crash_prepare_elf64_headers()

准备 ELF 格式的内核转储头，将系统内存映射为 ELF 段：

```c
int crash_prepare_elf64_headers(struct crash_mem *mem, int need_kernel_map,
                                void **addr, unsigned long *sz)
{
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    unsigned long nr_phdr = nr_cpus + 1 + mem->nr_ranges + 1;

    /* 创建 ELF 头部 */
    ehdr->e_type = ET_CORE;
    ehdr->e_machine = ELF_ARCH;

    /* 为每个 CPU 创建 PT_NOTE 段 */
    for_each_possible_cpu(cpu) {
        phdr->p_type = PT_NOTE;
        phdr->p_offset = per_cpu_ptr_to_phys(per_cpu_ptr(crash_notes, cpu));
        phdr++;
    }

    /* 创建 vmcoreinfo 的 PT_NOTE 段 */
    /* 创建 PT_LOAD 段描述内核内存区域 */
    ...
}
```

## 工作流程

### 1. 系统启动阶段

```
start_kernel()
    → setup_arch()
        → parse_crashkernel()          /* 解析 crashkernel= 参数 */
        → reserve_crashkernel()        /* 预留 crashkernel 内存 */
```

### 2. 加载 dump-capture 内核

```
用户空间: kexec -p vmlinuz-dump
    → sys_kexec_file_load()
        → kexec_file_load()
            → kimage_alloc_init()      /* 分配并初始化 kimage */
            → image->type = KEXEC_TYPE_CRASH
            → kexec_load_purgatory()   /* 加载 purgatory */
            → kexec_image_post_load_cleanup()
            → kexec_crash_image = image
```

### 3. 崩溃触发阶段

```
panic()
    → crash_kexec(regs)
        → panic_try_start()            /* 确保只有一个 CPU 执行 */
        → __crash_kexec(regs)
            → kexec_trylock()          /* 获取 kexec_lock */
            → crash_setup_regs()       /* 保存寄存器 */
            → crash_save_vmcoreinfo()  /* 保存 vmcoreinfo */
            → machine_crash_shutdown() /* 关闭设备 */
            → machine_kexec()          /* 启动新内核 */
```

### 4. Dump-Capture 内核运行阶段

```
dump-capture 内核启动
    → 解析 elfcorehdr= 参数
    → 创建 /proc/vmcore 接口
        → vmcore_read()
            → 将物理内存映射为 ELF 格式
    → 用户空间保存 vmcore
        $ cp /proc/vmcore /mnt/vmcore
```

## 内存布局

```
物理内存布局:
┌──────────────────────────────────────────────────────────────────┐
│ 0x00000000 - 0x00100000  │ 低内存 (BIOS/boot 区域)              │
├──────────────────────────────────────────────────────────────────┤
│ 0x00100000 - crashk_res.start │ 系统内核使用的内存               │
├──────────────────────────────────────────────────────────────────┤
│ crashk_res.start - crashk_res.end │ crashkernel 预留区域         │
│   ┌────────────────────────────────────────────────────────┐    │
│   │ dump-capture kernel (vmlinuz-dump)                    │    │
│   ├────────────────────────────────────────────────────────┤    │
│   │ initramfs (initramfs-dump.img)                        │    │
│   ├────────────────────────────────────────────────────────┤    │
│   │ ELF headers (elfcorehdr)                              │    │
│   └────────────────────────────────────────────────────────┘    │
├──────────────────────────────────────────────────────────────────┤
│ crashk_res.end - max_phys_addr │ 系统内核使用的内存             │
└──────────────────────────────────────────────────────────────────┘
```

## ELF 核心转储格式

vmcore 是 ELF 格式的核心转储文件：

```
ELF Header (Elf64_Ehdr)
├── e_type = ET_CORE (核心文件)
├── e_machine = ELF_ARCH (架构类型)
├── e_phoff = ELF header size
└── e_phnum = CPU数量 + 1(vmcoreinfo) + 内存区域数量 + 1(kernel text)

Program Headers (Elf64_Phdr[])
├── PT_NOTE: CPU 0 寄存器状态 (crash_notes[0])
├── PT_NOTE: CPU 1 寄存器状态 (crash_notes[1])
├── ...
├── PT_NOTE: vmcoreinfo (内核版本、符号表等)
├── PT_LOAD: 内核 text 区域 (虚拟地址映射)
├── PT_LOAD: 内存区域 0 (物理地址映射)
├── PT_LOAD: 内存区域 1
└── ...
```

## 编译配置

### 系统内核配置

```
CONFIG_KEXEC_CORE=y              # kexec 核心支持
CONFIG_KEXEC=y                   # kexec 系统调用
CONFIG_KEXEC_FILE=y              # 文件模式 kexec
CONFIG_CRASH_DUMP=y              # 崩溃转储支持
CONFIG_PROC_VMCORE=y             # /proc/vmcore 接口
```

### Dump-Capture 内核配置

```
CONFIG_KEXEC_CORE=y              # 必须启用
CONFIG_CRASH_DUMP=y              # 必须启用
CONFIG_PROC_VMCORE=y             # 必须启用
# 建议禁用不必要的功能以减小内核体积
CONFIG_SERIAL_8250_CONSOLE=y     # 串口控制台
CONFIG_NET=y                     # 网络支持 (用于远程传输)
CONFIG_EXT4_FS=y                 # 文件系统支持
```

## 内核参数

### 系统内核参数

```bash
# 预留 crashkernel 内存
crashkernel=1G@1G           # 从 1G 地址开始预留 1G 内存
crashkernel=256M            # 自动选择位置预留 256M 内存
crashkernel=auto            # 自动计算所需内存

# 控制崩溃行为
panic_on_oops=1             # Oops 时触发 panic
panic_timeout=60            # panic 后 60 秒自动重启
crash_kexec_post_notifiers  # 在 panic notifier 之后执行 crash_kexec
```

### Dump-Capture 内核参数

```bash
elfcorehdr=0x10000000       # ELF 核心头的物理地址
root=/dev/sda1              # 根文件系统
console=ttyS0,115200        # 串口控制台
```

## 常用工具

### kexec-tools

用户空间工具，用于加载 dump-capture 内核：

```bash
# 加载 dump-capture 内核
kexec -p /boot/vmlinuz-dump \
      --initrd=/boot/initramfs-dump.img \
      --append="root=/dev/sda1 console=ttyS0,115200"

# 卸载已加载的 crash 内核
kexec -u
```

### makedumpfile

过滤和压缩 vmcore：

```bash
# 生成过滤后的 vmcore (只包含内核数据)
makedumpfile -d 31 /proc/vmcore /mnt/vmcore-filtered

# 生成压缩的 vmcore
makedumpfile -z -d 31 /proc/vmcore /mnt/vmcore.gz

# 常用选项:
# -d 31: 过滤所有用户空间内存
# -z: gzip 压缩
# -c: 控制台日志
```

### crash

分析 vmcore 的交互式工具：

```bash
# 启动 crash 分析
crash /usr/lib/debug/lib/modules/$(uname -r)/vmlinux /mnt/vmcore

# crash 命令示例:
crash> bt                    # 查看崩溃时的堆栈
crash> ps                    # 查看进程状态
crash> vm                    # 查看虚拟内存信息
crash> log                   # 查看内核日志
crash> mod                   # 查看已加载模块
crash> kmem                  # 查看内存使用情况
crash> struct task_struct    # 查看 task_struct 结构
```

## 使用场景

### 生产环境部署

```bash
# 1. 安装 kexec-tools
yum install kexec-tools

# 2. 配置 grub (在系统内核启动参数中添加)
crashkernel=1G@1G

# 3. 创建 dump-capture 内核 initramfs
dracut -f /boot/initramfs-dump.img $(uname -r)

# 4. 加载 dump-capture 内核
kexec -p /boot/vmlinuz-$(uname -r) \
      --initrd=/boot/initramfs-dump.img \
      --append="root=/dev/sda1 console=ttyS0,115200"

# 5. 配置自动保存 vmcore (在 dump-capture 内核的 initramfs 中)
echo 'cp /proc/vmcore /mnt/vmcore-$(date +%Y%m%d-%H%M%S)' >> /etc/rc.d/rc.sysinit
```

### 手动测试

```bash
# 触发 panic (需要 CONFIG_MAGIC_SYSRQ)
echo c > /proc/sysrq-trigger

# 验证 crash 内核是否已加载
cat /sys/kernel/kexec_crash_loaded   # 1 表示已加载
```

## 性能影响

- **内存开销**: crashkernel 预留的内存对系统内核不可用
- **加载开销**: `kexec -p` 会占用一定的 CPU 和 I/O 资源
- **崩溃转储时间**: 取决于系统内存大小和存储设备速度

## 代码位置

```
kernel/kexec_core.c          # kexec 核心实现
kernel/kexec_file.c          # 文件模式 kexec
kernel/kexec_elf.c           # ELF 格式支持
kernel/crash_core.c          # 崩溃核心支持
kernel/kexec.c               # kexec 系统调用
include/linux/kexec.h        # kexec 头文件
include/linux/crash_core.h   # 崩溃核心头文件
Documentation/admin-guide/kdump/kdump.rst  # 官方文档
```