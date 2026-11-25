# pstore / Ramoops — 崩溃日志持久化

## 概述

pstore 是 Linux 内核的持久存储机制，用于在系统崩溃后保存关键调试信息。Ramoops 是 pstore 的一种 RAM 后端实现，利用预留的内存区域存储崩溃日志，系统重启后可从 `/sys/fs/pstore/` 读取上次崩溃的信息。

### 工作原理

```
系统启动时:
┌─────────────────────────────────────────────────────────────┐
│                    System Memory                            │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  预留 RAM 区域 (ramoops.mem_address + ramoops.mem_size)│   │
│  │  该区域在重启后仍保持数据                              │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘

系统运行时:
┌─────────────────────────────────────────────────────────────┐
│                    pstore 子系统                            │
│  ┌──────────────────┐  ┌──────────────────┐                │
│  │   dmesg 缓冲区   │  │   console 缓冲区  │                │
│  ├──────────────────┤  ├──────────────────┤                │
│  │   ftrace 缓冲区  │  │   pmsg 缓冲区    │                │
│  └──────────────────┘  └──────────────────┘                │
│          ↓                                                    │
│  定期或触发时写入预留 RAM                                       │
└─────────────────────────────────────────────────────────────┘

系统崩溃时:
┌─────────────────────────────────────────────────────────────┐
│                    panic / oops                             │
│  1. kmsg_dump() 回调                                         │
│  2. pstore_dump() 保存日志到预留 RAM                         │
│  3. 系统重启                                                 │
└─────────────────────────────────────────────────────────────┘

系统重启后:
┌─────────────────────────────────────────────────────────────┐
│                    用户空间读取                              │
│  $ mount -t pstore pstore /sys/fs/pstore                   │
│  $ cat /sys/fs/pstore/dmesg-ramoops-0                      │
│  $ cat /sys/fs/pstore/console-ramoops-0                    │
└─────────────────────────────────────────────────────────────┘
```

## 核心数据结构

### struct pstore_record

描述一个 pstore 记录条目：

```c
struct pstore_record {
    struct pstore_info   *psi;           /* 后端驱动信息 */
    enum pstore_type_id  type;           /* 记录类型 */
    u64                  id;             /* 类型内唯一标识 */
    struct timespec64    time;           /* 时间戳 */
    char                 *buf;           /* 记录内容 */
    ssize_t              size;           /* buf 大小 */
    ssize_t              ecc_notice_size;/* ECC 信息大小 */
    void                 *priv;          /* 后端私有数据 */

    int                  count;          /* Oops 计数 */
    enum kmsg_dump_reason reason;        /* kdump 原因 */
    unsigned int         part;           /* 多部分记录的位置 */
    bool                 compressed;     /* 是否压缩 */
};
```

### struct pstore_info

pstore 后端驱动接口：

```c
struct pstore_info {
    struct module       *owner;          /* 所属模块 */
    const char          *name;           /* 后端名称 */

    raw_spinlock_t      buf_lock;        /* buf 访问锁 */
    char                *buf;            /* 预分配的崩溃缓冲区 */
    size_t              bufsize;         /* 缓冲区大小 */

    struct mutex        read_mutex;      /* 序列化 open/read/close/erase */
    int                 flags;           /* 支持的前端类型 */
    int                 max_reason;      /* 最大 kmsg_dump_reason */
    void                *data;           /* 后端私有数据 */

    int                 (*open)(struct pstore_info *psi);
    int                 (*close)(struct pstore_info *psi);
    ssize_t             (*read)(struct pstore_record *record);
    int                 (*write)(struct pstore_record *record);
    int                 (*write_user)(struct pstore_record *record,
                                     const char __user *buf);
    int                 (*erase)(struct pstore_record *record);
};
```

### struct persistent_ram_buffer

Ramoops 的循环缓冲区结构：

```c
struct persistent_ram_buffer {
    uint32_t      sig;                   /* 签名 (DBGC) */
    atomic_t      start;                 /* 缓冲区起始位置 */
    atomic_t      size;                  /* 有效数据大小 */
    uint8_t       data[];                /* 缓冲区内容 */
};
```

### struct ramoops_platform_data

Ramoops 平台数据：

```c
struct ramoops_platform_data {
    unsigned long      mem_size;         /* 内存大小 */
    phys_addr_t        mem_address;      /* 物理地址 */
    unsigned int       mem_type;         /* 内存类型 */
    unsigned long      record_size;      /* 记录大小 */
    unsigned long      console_size;     /* console 缓冲区大小 */
    unsigned long      ftrace_size;      /* ftrace 缓冲区大小 */
    unsigned long      pmsg_size;        /* pmsg 缓冲区大小 */
    int                max_reason;       /* 最大 dump 原因 */
    u32                flags;            /* 标志 */
    struct persistent_ram_ecc_info ecc_info; /* ECC 信息 */
};
```

## 记录类型

```c
enum pstore_type_id {
    PSTORE_TYPE_DMESG       = 0,         /* 内核日志 */
    PSTORE_TYPE_MCE         = 1,         /* 机器检查异常 */
    PSTORE_TYPE_CONSOLE     = 2,         /* 控制台输出 */
    PSTORE_TYPE_FTRACE      = 3,         /* ftrace 追踪 */

    PSTORE_TYPE_PPC_RTAS    = 4,         /* PPC64 RTAS */
    PSTORE_TYPE_PPC_OF      = 5,         /* PPC64 Open Firmware */
    PSTORE_TYPE_PPC_COMMON  = 6,         /* PPC64 通用 */
    PSTORE_TYPE_PMSG        = 7,         /* 用户空间消息 */
    PSTORE_TYPE_PPC_OPAL    = 8,         /* PPC64 OPAL */

    PSTORE_TYPE_MAX
};
```

## 工作流程

### 1. 注册阶段

```
platform_driver_probe()
    → ramoops_probe()
        → persistent_ram_new()           /* 创建持久化 RAM 缓冲区 */
        → ramoops_info.psi.open = ramoops_open
        → ramoops_info.psi.read = ramoops_read
        → ramoops_info.psi.write = ramoops_write
        → ramoops_info.psi.erase = ramoops_erase
        → pstore_register(&ramoops_info.psi)
```

### 2. 日志写入阶段

```
panic() / oops
    → kmsg_dump(KMSG_DUMP_OOPS/KMSG_DUMP_PANIC)
        → pstore_dump()
            → pstore_do_kmsg_dump()
                → zlib_compress()        /* 压缩日志 */
                → psi->write(record)     /* 写入后端 */
                    → ramoops_write()
                        → persistent_ram_write_buf()
                            → 写入循环缓冲区
```

### 3. 日志读取阶段

```
mount pstore
    → pstore_fill_super()
        → psi->open()
            → ramoops_open()
        → for (;;) {
            psi->read(record)
                → ramoops_read()
            create_inode(record)
        }
        → psi->close()
```

## 内存布局

```
Ramoops 内存布局:
┌──────────────────────────────────────────────────────────────────┐
│  mem_address                                                     │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │  struct persistent_ram_buffer                              │ │
│  │  ┌──────────────────────────────────────────────────────┐  │ │
│  │  │ sig (4 bytes)    │ 0x43474244 (DBGC)                 │  │ │
│  │  ├──────────────────────────────────────────────────────┤  │ │
│  │  │ start (4 bytes)  │ 缓冲区起始位置                    │  │ │
│  │  ├──────────────────────────────────────────────────────┤  │ │
│  │  │ size (4 bytes)   │ 有效数据大小                      │  │ │
│  │  ├──────────────────────────────────────────────────────┤  │ │
│  │  │ data[]           │ 实际日志数据                      │  │ │
│  │  │   ┌─────────────────────────────────────────────┐   │  │ │
│  │  │   │ dmesg 记录                                  │   │  │ │
│  │  │   ├─────────────────────────────────────────────┤   │  │ │
│  │  │   │ console 记录                                │   │  │ │
│  │  │   ├─────────────────────────────────────────────┤   │  │ │
│  │  │   │ ftrace 记录                                 │   │  │ │
│  │  │   └─────────────────────────────────────────────┘   │  │ │
│  │  └──────────────────────────────────────────────────────┘  │ │
│  └────────────────────────────────────────────────────────────┘ │
│  mem_address + mem_size                                         │
└──────────────────────────────────────────────────────────────────┘
```

## 循环缓冲区机制

```
循环缓冲区写入:
┌────────────────────────────────────────────────────────────┐
│ [  ][  ][  ][  ][  ][  ][  ][  ][  ][  ]                   │
│      ↑start=2                                              │
│      ↑size=0 (空缓冲区)                                    │
└────────────────────────────────────────────────────────────┘

写入 "abcde":
┌────────────────────────────────────────────────────────────┐
│ [  ][ab][cd][ e][  ][  ][  ][  ][  ][  ]                   │
│      ↑start=2                                              │
│           ↑size=5                                          │
└────────────────────────────────────────────────────────────┘

写入 "fghij" (超过容量):
┌────────────────────────────────────────────────────────────┐
│ [gh][ij][cd][ e][fg][  ][  ][  ][  ][  ]                   │
│           ↑start=4 (旧数据被覆盖)                           │
│                ↑size=10 (满)                               │
└────────────────────────────────────────────────────────────┘
```

## ECC 支持

Ramoops 支持 Reed-Solomon ECC 纠错：

```c
struct persistent_ram_ecc_info {
    int block_size;                      /* 数据块大小 */
    int ecc_size;                        /* ECC 大小 */
    int symsize;                         /* 符号大小 */
    int poly;                            /* 多项式 */
    uint16_t *par;                       /* 预计算表 */
};
```

当内存区域可能存在位翻转时（如断电场景），ECC 可以检测并纠正错误。

## 编译配置

```
CONFIG_PSTORE=y                        # pstore 核心支持
CONFIG_PSTORE_RAM=y                    # Ramoops RAM 后端
CONFIG_PSTORE_RAM_ECC=y                # ECC 支持
CONFIG_PSTORE_CONSOLE=y                # console 前端
CONFIG_PSTORE_FTRACE=y                 # ftrace 前端
CONFIG_PSTORE_PMSG=y                   # pmsg 前端
CONFIG_PSTORE_DEFAULT_KMSG_BYTES=65536 # 默认 kmsg 大小
```

## 内核参数

### Ramoops 模块参数

```bash
# 通过内核启动参数配置
ramoops.mem_address=0x80000000         # 预留内存的物理地址
ramoops.mem_size=0x100000              # 内存大小 (1MB)
ramoops.record_size=0x10000            # 每条记录大小 (64KB)
ramoops.console_size=0x20000           # console 缓冲区大小 (128KB)
ramoops.ftrace_size=0x20000            # ftrace 缓冲区大小 (128KB)
ramoops.pmsg_size=0x10000              # pmsg 缓冲区大小 (64KB)
ramoops.ecc=1                          # 启用 ECC

# 通过模块参数配置
modprobe ramoops mem_address=0x80000000 mem_size=0x100000
```

### pstore 模块参数

```bash
pstore.backend=ramoops                 # 指定后端
pstore.compress=deflate                # 压缩算法 (deflate/none)
pstore.kmsg_bytes=65536                # kmsg 快照大小
pstore.update_ms=1000                  # 定期更新间隔 (毫秒)
```

## 使用方法

### 挂载 pstore

```bash
# 自动挂载 (systemd)
systemctl start pstore

# 手动挂载
mount -t pstore pstore /sys/fs/pstore

# 查看挂载的文件
ls /sys/fs/pstore/
dmesg-ramoops-0
dmesg-ramoops-1
console-ramoops-0
ftrace-ramoops-0
pmsg-ramoops-0
```

### 查看崩溃日志

```bash
# 查看内核日志
cat /sys/fs/pstore/dmesg-ramoops-0

# 查看控制台输出
cat /sys/fs/pstore/console-ramoops-0

# 查看 ftrace 追踪
cat /sys/fs/pstore/ftrace-ramoops-0
```

### 清除记录

```bash
# 删除单条记录
rm /sys/fs/pstore/dmesg-ramoops-0

# 清空所有记录
echo 1 > /sys/fs/pstore/clear
```

### 用户空间消息 (pmsg)

```bash
# 写入用户空间消息
echo "test message" > /sys/fs/pstore/pmsg-ramoops-0

# 重启后读取
cat /sys/fs/pstore/pmsg-ramoops-0
```

## 后端类型

pstore 支持多种后端：

| 后端 | 描述 | 配置 |
|------|------|------|
| **ramoops** | RAM 后端，使用预留内存 | `CONFIG_PSTORE_RAM` |
| **pstore_blk** | 块设备后端，使用分区 | `CONFIG_PSTORE_BLK` |
| **efi-pstore** | EFI 变量后端 | `CONFIG_EFI_VARS_PSTORE` |
| **zone** | 内存区域后端 | `CONFIG_PSTORE_ZONE` |

## 性能影响

- **内存开销**: 预留的内存区域对系统不可用
- **写入开销**: 崩溃时的写入操作通常在中断上下文中执行
- **压缩开销**: 使用 zlib deflate 压缩，会占用 CPU 时间

## 代码位置

```
fs/pstore/platform.c          # pstore 核心实现
fs/pstore/ram_core.c          # Ramoops 核心
fs/pstore/ram.c               # Ramoops 平台驱动
fs/pstore/zone.c              # 内存区域后端
fs/pstore/blk.c               # 块设备后端
fs/pstore/ftrace.c            # ftrace 前端
fs/pstore/pmsg.c              # pmsg 前端
fs/pstore/inode.c             # 文件系统接口
include/linux/pstore.h        # pstore 头文件
include/linux/pstore_ram.h    # Ramoops 头文件
Documentation/admin-guide/pstore-blk.rst  # 官方文档
```