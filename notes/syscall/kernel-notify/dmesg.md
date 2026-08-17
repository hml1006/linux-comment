# dmesg 系统调用

## 概述

`dmesg`（或 `syslog`）系统调用是内核日志管理接口，用于读取和控制内核环形缓冲区（ring buffer）中的日志消息。在 ARM64 架构上，`dmesg` 没有独立的系统调用编号，而是通过 `syslog` 系统调用（`__NR_syslog = 116`）实现。用户空间的 `dmesg` 命令工具通过 `klogctl()` 库函数封装此系统调用。

该接口提供了 11 种操作类型，涵盖日志读取、缓冲区控制、控制台日志级别管理等功能，是系统调试和监控的核心接口之一。

---

## 函数原型

```c
#include <sys/klog.h>

int klogctl(int type, char *buf, int len);
```

等价于直接使用系统调用：

```c
#include <unistd.h>
#include <sys/syscall.h>

long syscall(SYS_syslog, int type, char __user *buf, int len);
```

### 参数说明

| 参数 | 类型 | 描述 |
|------|------|------|
| `type` | `int` | 操作类型（详见下方操作类型表） |
| `buf` | `char *` | 用户空间缓冲区指针，用于读取或写入日志数据 |
| `len` | `int` | 缓冲区大小（字节数） |

### 操作类型

| 宏定义 | 值 | 描述 |
|--------|-----|------|
| `SYSLOG_ACTION_CLOSE` | 0 | 关闭日志（当前为空操作） |
| `SYSLOG_ACTION_OPEN` | 1 | 打开日志（当前为空操作） |
| `SYSLOG_ACTION_READ` | 2 | 从日志读取（消费模式，读取后数据标记为已读） |
| `SYSLOG_ACTION_READ_ALL` | 3 | 读取环形缓冲区中所有剩余消息（非消费模式） |
| `SYSLOG_ACTION_READ_CLEAR` | 4 | 读取并清除环形缓冲区中所有消息 |
| `SYSLOG_ACTION_CLEAR` | 5 | 清除环形缓冲区 |
| `SYSLOG_ACTION_CONSOLE_OFF` | 6 | 禁用控制台日志输出 |
| `SYSLOG_ACTION_CONSOLE_ON` | 7 | 启用控制台日志输出 |
| `SYSLOG_ACTION_CONSOLE_LEVEL` | 8 | 设置控制台日志级别（1-8） |
| `SYSLOG_ACTION_SIZE_UNREAD` | 9 | 返回未读字符数 |
| `SYSLOG_ACTION_SIZE_BUFFER` | 10 | 返回日志缓冲区总大小 |

---

## 详细调用链分析

### 内核入口

```c
// kernel/printk/printk.c
SYSCALL_DEFINE3(syslog, int, type, char __user *, buf, int, len)
{
    return do_syslog(type, buf, len, SYSLOG_FROM_READER);
}
```

系统调用直接委托给 `do_syslog()` 函数，其中 `source` 参数为 `SYSLOG_FROM_READER`（表示来自系统调用），区别于来自 `/proc/kmsg` 的访问（`SYSLOG_FROM_PROC`）。

### do_syslog() 核心分发函数

```c
// kernel/printk/printk.c
int do_syslog(int type, char __user *buf, int len, int source)
{
    struct printk_info info;
    bool clear = false;
    static int saved_console_loglevel = LOGLEVEL_DEFAULT;
    int error;

    error = check_syslog_permissions(type, source);
    if (error)
        return error;

    switch (type) {
    case SYSLOG_ACTION_CLOSE:   /* Close log */
        break;
    case SYSLOG_ACTION_OPEN:    /* Open log */
        break;
    case SYSLOG_ACTION_READ:    /* Read from log */
        if (!buf || len < 0)
            return -EINVAL;
        if (!len)
            return 0;
        if (!access_ok(buf, len))
            return -EFAULT;
        error = syslog_print(buf, len);
        break;
    case SYSLOG_ACTION_READ_CLEAR:
        clear = true;
        fallthrough;
    case SYSLOG_ACTION_READ_ALL:
        if (!buf || len < 0)
            return -EINVAL;
        if (!len)
            return 0;
        if (!access_ok(buf, len))
            return -EFAULT;
        error = syslog_print_all(buf, len, clear);
        break;
    case SYSLOG_ACTION_CLEAR:
        syslog_clear();
        break;
    case SYSLOG_ACTION_CONSOLE_OFF:
        if (saved_console_loglevel == LOGLEVEL_DEFAULT)
            saved_console_loglevel = console_loglevel;
        console_loglevel = minimum_console_loglevel;
        break;
    case SYSLOG_ACTION_CONSOLE_ON:
        if (saved_console_loglevel != LOGLEVEL_DEFAULT) {
            console_loglevel = saved_console_loglevel;
            saved_console_loglevel = LOGLEVEL_DEFAULT;
        }
        break;
    case SYSLOG_ACTION_CONSOLE_LEVEL:
        if (len < 1 || len > 8)
            return -EINVAL;
        if (len < minimum_console_loglevel)
            len = minimum_console_loglevel;
        console_loglevel = len;
        saved_console_loglevel = LOGLEVEL_DEFAULT;
        break;
    case SYSLOG_ACTION_SIZE_UNREAD:
        // 计算未读消息的字符数
        ...
        break;
    case SYSLOG_ACTION_SIZE_BUFFER:
        error = log_buf_len;
        break;
    default:
        error = -EINVAL;
        break;
    }
    return error;
}
```

### 权限检查

```c
// kernel/printk/printk.c
static int check_syslog_permissions(int type, int source)
{
    if (source == SYSLOG_FROM_PROC && type != SYSLOG_ACTION_OPEN)
        goto ok;

    if (syslog_action_restricted(type)) {
        if (capable(CAP_SYSLOG))
            goto ok;
        return -EPERM;
    }
ok:
    return security_syslog(type);
}
```

权限检查涉及两个层面：
1. **内核能力检查**：若 `dmesg_restrict` 或 `kptr_restrict` 限制生效，需要 `CAP_SYSLOG` 或 `CAP_SYS_ADMIN` 能力
2. **LSM 安全钩子**：通过 `security_syslog()` 调用 Linux 安全模块（SELinux、AppArmor 等）进行额外检查

### 完整调用链

```
用户空间调用
    │
    ├─ dmesg 命令工具
    │    └─ klogctl(SYSLOG_ACTION_READ_ALL, buf, len)
    │
    ├─ syslog(2) 库函数
    │    └─ syscall(__NR_syslog, type, buf, len)
    │
    └─ /dev/kmsg 设备文件
         └─ devkmsg_read() / devkmsg_write()

内核处理
    │
    └─ SYSCALL_DEFINE3(syslog, type, buf, len)    // kernel/printk/printk.c
         │
         └─ do_syslog(type, buf, len, SYSLOG_FROM_READER)
              │
              ├─ check_syslog_permissions(type, source)
              │    ├─ syslog_action_restricted()  // 检查 dmesg_restrict
              │    ├─ capable(CAP_SYSLOG)          // 权限检查
              │    └─ security_syslog()            // LSM 安全钩子
              │
              └─ switch(type):
                   │
                   ├─ SYSLOG_ACTION_READ (2):
                   │    └─ syslog_print(buf, len)           // 消费模式读取
                   │         └─ _prb_read_valid()           // 从环形缓冲区读取
                   │
                   ├─ SYSLOG_ACTION_READ_ALL (3):
                   │    └─ syslog_print_all(buf, len, false) // 非消费模式读取
                   │         ├─ prb_first_valid_seq()       // 获取首个有效序列号
                   │         └─ prb_read_valid()            // 循环读取所有记录
                   │
                   ├─ SYSLOG_ACTION_READ_CLEAR (4):
                   │    └─ syslog_print_all(buf, len, true)  // 读取并清除
                   │
                   ├─ SYSLOG_ACTION_CLEAR (5):
                   │    └─ syslog_clear()                    // 清除日志
                   │
                   ├─ SYSLOG_ACTION_CONSOLE_OFF (6):
                   │    └─ console_loglevel = minimum_console_loglevel
                   │
                   ├─ SYSLOG_ACTION_CONSOLE_ON (7):
                   │    └─ 恢复 console_loglevel
                   │
                   ├─ SYSLOG_ACTION_CONSOLE_LEVEL (8):
                   │    └─ console_loglevel = len (1-8)
                   │
                   ├─ SYSLOG_ACTION_SIZE_UNREAD (9):
                   │    └─ 遍历环形缓冲区计算未读消息大小
                   │
                   └─ SYSLOG_ACTION_SIZE_BUFFER (10):
                        └─ 返回 log_buf_len
```

---

## 关键数据结构

### printk_info — 日志记录元信息

```c
// kernel/printk/printk_ringbuffer.h
struct printk_info {
    u64     seq;            /* 序列号，单调递增 */
    u64     ts_nsec;        /* 时间戳（纳秒） */
    u16     text_len;       /* 文本消息长度 */
    u8      facility;       /* syslog 设施类型 */
    u8      flags:5;        /* 内部记录标志位 */
    u8      level:3;        /* syslog 日志级别（0-7） */
    u32     caller_id;      /* 线程 ID 或处理器 ID */
    struct dev_printk_info dev_info;  /* 设备信息 */
};
```

### printk_record — 日志记录

```c
// kernel/printk/printk_ringbuffer.h
struct printk_record {
    struct printk_info *info;    /* 指向记录元信息的指针 */
    char *text_buf;              /* 文本缓冲区 */
    unsigned int text_buf_size;  /* 文本缓冲区大小 */
};
```

### printk_ringbuffer — 环形缓冲区

```c
// kernel/printk/printk_ringbuffer.h
struct printk_ringbuffer {
    struct prb_desc_ring desc_ring;      /* 描述符环形缓冲区 */
    struct prb_data_ring text_data_ring; /* 文本数据环形缓冲区 */
    atomic_long_t __reserve_failures;    /* 保留失败计数 */
    atomic_long_t __extract_failures;    /* 提取失败计数 */
};
```

printk 环形缓冲区由三个内部环形缓冲区组成：
1. **desc_ring（描述符环）**：存储描述符及其元数据（序列号、时间戳、日志级别等），以及文本数据在数据环中的逻辑位置
2. **text_data_ring（文本数据环）**：存储实际的文本字符串数据块
3. 通过无锁（lockless）机制实现读者和写者的同步访问

---

## 执行流程（ASCII 流程图）

```
                    ┌───────────────┐
                    │  Userspace    │
                    │  dmesg/syslog │
                    └───────┬───────┘
                            │ klogctl() / syscall()
                            ▼
                    ┌───────────────┐
                    │  sys_syslog() │
                    │  printk.c     │
                    └───────┬───────┘
                            │
                            ▼
                    ┌───────────────┐
                    │ do_syslog()   │
                    └───────┬───────┘
                            │
                    ┌───────┴───────┐
                    │ 权限检查      │
                    │ CAP_SYSLOG    │
                    └───────┬───────┘
                            │
                    ┌───────┴──────────────────────────────┐
                    │         switch(type)                  │
                    │                                       │
                    │  READ_ALL ──► syslog_print_all()      │
                    │                    │                  │
                    │                    ├─ prb_first_valid  │
                    │                    │   _seq()          │
                    │                    └─ prb_read_valid() │
                    │                       (循环)           │
                    │                                       │
                    │  READ ────► syslog_print()            │
                    │                                       │
                    │  CLEAR ───► syslog_clear()            │
                    │                                       │
                    │  CONSOLE_LEVEL ─► console_loglevel=   │
                    │                                       │
                    │  SIZE_BUFFER ─► return log_buf_len    │
                    └───────────────────────────────────────┘
```

---

## 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EPERM` | 权限不足 | 没有 `CAP_SYSLOG` 能力，且 `dmesg_restrict` 已设置 |
| `-EINVAL` | 无效参数 | `type` 无效，或 `buf` 为 NULL 且 `len > 0`，或 `len < 0` |
| `-EFAULT` | 用户空间地址错误 | `buf` 指向不可访问的用户空间地址 |
| `-ENOSYS` | 系统未实现 | 内核编译时未启用 `CONFIG_PRINTK` |

### 安全限制

- **dmesg_restrict**（`/proc/sys/kernel/dmesg_restrict`）：设置为 1 时，非特权用户无法读取内核日志缓冲区
- **kptr_restrict**（`/proc/sys/kernel/kptr_restrict`）：控制内核指针在日志中的显示方式

---

## 使用示例

### 1. 读取所有内核日志

```c
#include <sys/klog.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int len = klogctl(10, NULL, 0);  /* SYSLOG_ACTION_SIZE_BUFFER */
    if (len == -1) {
        perror("klogctl");
        return 1;
    }

    char *buf = malloc(len);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    int ret = klogctl(3, buf, len);  /* SYSLOG_ACTION_READ_ALL */
    if (ret == -1) {
        perror("klogctl");
        free(buf);
        return 1;
    }

    buf[ret] = '\0';
    printf("%s", buf);

    free(buf);
    return 0;
}
```

### 2. 设置控制台日志级别

```c
#include <sys/klog.h>
#include <stdio.h>

int main(void)
{
    /* 设置控制台日志级别为 4（只显示 KERN_WARNING 及以上级别） */
    if (klogctl(8, NULL, 4) == -1) {  /* SYSLOG_ACTION_CONSOLE_LEVEL */
        perror("klogctl");
        return 1;
    }
    printf("Console log level set to 4 (KERN_WARNING)\n");
    return 0;
}
```

### 3. 清除内核日志缓冲区

```c
#include <sys/klog.h>
#include <stdio.h>

int main(void)
{
    if (klogctl(5, NULL, 0) == -1) {  /* SYSLOG_ACTION_CLEAR */
        perror("klogctl");
        return 1;
    }
    printf("Kernel log buffer cleared\n");
    return 0;
}
```

---

## 源码位置

| 文件 | 说明 |
|------|------|
| `kernel/printk/printk.c` | `sys_syslog()` 和 `do_syslog()` 实现 |
| `kernel/printk/printk_ringbuffer.h` | printk 环形缓冲区数据结构定义 |
| `kernel/printk/printk_ringbuffer.c` | 环形缓冲区读写操作实现 |
| `include/linux/syslog.h` | `SYSLOG_ACTION_*` 宏定义和 `do_syslog()` 声明 |
| `include/uapi/asm-generic/unistd.h` | `__NR_syslog` 系统调用编号定义 |