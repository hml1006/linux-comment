# syslog 系统调用

## 概述

`syslog` 系统调用是内核日志管理的核心接口，用于读取和控制内核环形缓冲区（ring buffer）中的日志消息。在 ARM64 架构上，`syslog` 的系统调用编号为 `__NR_syslog = 116`。用户空间的 `dmesg` 命令和 `klogctl()` 库函数均基于此系统调用实现。

`syslog` 系统调用提供了 11 种操作类型（`SYSLOG_ACTION_*`），涵盖日志读取、缓冲区控制、控制台日志级别管理等功能。

---

## 函数原型

```c
#include <sys/syscall.h>
#include <unistd.h>

long syscall(SYS_syslog, int type, char __user *buf, int len);
```

库函数封装：

```c
#include <sys/klog.h>

int klogctl(int type, char *buf, int len);
```

### 参数说明

| 参数 | 类型 | 描述 |
|------|------|------|
| `type` | `int` | 操作类型（见下方操作类型表） |
| `buf` | `char __user *` | 用户空间缓冲区指针，用于读取或写入日志数据 |
| `len` | `int` | 缓冲区大小（字节数） |

### 操作类型

| 宏定义 | 值 | 描述 |
|--------|-----|------|
| `SYSLOG_ACTION_CLOSE` | 0 | 关闭日志（当前为空操作 NOP） |
| `SYSLOG_ACTION_OPEN` | 1 | 打开日志（当前为空操作 NOP） |
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

    /* 步骤 1：权限检查 */
    error = check_syslog_permissions(type, source);
    if (error)
        return error;

    /* 步骤 2：根据 type 分发处理 */
    switch (type) {
    case SYSLOG_ACTION_CLOSE:   /* 0: 关闭日志，NOP */
        break;

    case SYSLOG_ACTION_OPEN:    /* 1: 打开日志，NOP */
        break;

    case SYSLOG_ACTION_READ:    /* 2: 消费模式读取 */
        if (!buf || len < 0)
            return -EINVAL;
        if (!len)
            return 0;
        if (!access_ok(buf, len))
            return -EFAULT;
        error = syslog_print(buf, len);
        break;

    case SYSLOG_ACTION_READ_CLEAR: /* 4: 读取并清除 */
        clear = true;
        fallthrough;

    case SYSLOG_ACTION_READ_ALL: /* 3: 读取所有 */
        if (!buf || len < 0)
            return -EINVAL;
        if (!len)
            return 0;
        if (!access_ok(buf, len))
            return -EFAULT;
        error = syslog_print_all(buf, len, clear);
        break;

    case SYSLOG_ACTION_CLEAR:   /* 5: 清除日志 */
        syslog_clear();
        break;

    case SYSLOG_ACTION_CONSOLE_OFF: /* 6: 关闭控制台输出 */
        if (saved_console_loglevel == LOGLEVEL_DEFAULT)
            saved_console_loglevel = console_loglevel;
        console_loglevel = minimum_console_loglevel;
        break;

    case SYSLOG_ACTION_CONSOLE_ON:  /* 7: 启用控制台输出 */
        if (saved_console_loglevel != LOGLEVEL_DEFAULT) {
            console_loglevel = saved_console_loglevel;
            saved_console_loglevel = LOGLEVEL_DEFAULT;
        }
        break;

    case SYSLOG_ACTION_CONSOLE_LEVEL: /* 8: 设置日志级别 */
        if (len < 1 || len > 8)
            return -EINVAL;
        if (len < minimum_console_loglevel)
            len = minimum_console_loglevel;
        console_loglevel = len;
        saved_console_loglevel = LOGLEVEL_DEFAULT;
        break;

    case SYSLOG_ACTION_SIZE_UNREAD: /* 9: 未读字符数 */
        mutex_lock(&syslog_lock);
        if (!prb_read_valid_info(prb, syslog_seq, &info, NULL)) {
            mutex_unlock(&syslog_lock);
            return 0;
        }
        if (info.seq != syslog_seq) {
            syslog_seq = info.seq;
            syslog_partial = 0;
        }
        if (source == SYSLOG_FROM_PROC) {
            /* /proc/kmsg 的 poll 快捷方式 */
            error = prb_next_seq(prb) - syslog_seq;
        } else {
            bool time = syslog_partial ? syslog_time : printk_time;
            unsigned int line_count;
            u64 seq;

            prb_for_each_info(syslog_seq, prb, seq, &info, &line_count) {
                error += get_record_print_text_size(&info, line_count,
                                                    true, time);
                time = printk_time;
            }
            error -= syslog_partial;
        }
        mutex_unlock(&syslog_lock);
        break;

    case SYSLOG_ACTION_SIZE_BUFFER: /* 10: 缓冲区总大小 */
        error = log_buf_len;
        break;

    default:
        error = -EINVAL;
        break;
    }

    return error;
}
```

### 权限检查函数

```c
// kernel/printk/printk.c
static int check_syslog_permissions(int type, int source)
{
    /*
     * 如果来自 /proc/kmsg 且已打开，则在 open 时已完成权限检查
     */
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
1. **内核能力检查**：若 `dmesg_restrict` 限制生效，需要 `CAP_SYSLOG` 能力
2. **LSM 安全钩子**：通过 `security_syslog()` 调用 Linux 安全模块（SELinux、AppArmor 等）进行额外检查

### 完整调用链

```
用户空间
    │
    ├─ klogctl(type, buf, len)          // glibc 封装
    │    └─ syscall(__NR_syslog, type, buf, len)
    │
    └─ syslog(2)                        // 直接系统调用
         └─ syscall(__NR_syslog, type, buf, len)

内核
    │
    └─ SYSCALL_DEFINE3(syslog, type, buf, len)    // kernel/printk/printk.c:1855
         │
         └─ do_syslog(type, buf, len, SYSLOG_FROM_READER)
              │
              ├─ 1. 权限检查
              │    ├─ check_syslog_permissions(type, source)
              │    │    ├─ syslog_action_restricted() → dmesg_restrict
              │    │    ├─ capable(CAP_SYSLOG)
              │    │    └─ security_syslog() → LSM hook
              │
              └─ 2. 操作分发
                   │
                   ├─ SYSLOG_ACTION_READ (2)
                   │    └─ syslog_print(buf, len)
                   │         └─ _prb_read_valid(prb, &seq, r)
                   │              ├─ desc_read_finalized_seq()
                   │              └─ copy_data()  → 复制到用户空间
                   │
                   ├─ SYSLOG_ACTION_READ_ALL (3)
                   │    └─ syslog_print_all(buf, len, false)
                   │         ├─ prb_first_valid_seq(prb)
                   │         └─ prb_read_valid(prb, seq, r)  // 循环
                   │
                   ├─ SYSLOG_ACTION_READ_CLEAR (4)
                   │    └─ syslog_print_all(buf, len, true)
                   │
                   ├─ SYSLOG_ACTION_CLEAR (5)
                   │    └─ syslog_clear()
                   │         ├─ logbuf_lock
                   │         └─ 重置环形缓冲区状态
                   │
                   ├─ SYSLOG_ACTION_CONSOLE_OFF (6)
                   │    └─ console_loglevel = minimum_console_loglevel
                   │
                   ├─ SYSLOG_ACTION_CONSOLE_ON (7)
                   │    └─ 恢复 console_loglevel
                   │
                   ├─ SYSLOG_ACTION_CONSOLE_LEVEL (8)
                   │    └─ console_loglevel = len
                   │
                   ├─ SYSLOG_ACTION_SIZE_UNREAD (9)
                   │    └─ 遍历 prb，计算未读消息大小
                   │
                   └─ SYSLOG_ACTION_SIZE_BUFFER (10)
                        └─ return log_buf_len
```

---

## 关键数据结构

### printk_ringbuffer — printk 环形缓冲区

```c
// kernel/printk/printk_ringbuffer.h
struct printk_ringbuffer {
    struct prb_desc_ring desc_ring;      /* 描述符环 */
    struct prb_data_ring text_data_ring; /* 文本数据环 */
    atomic_long_t __reserve_failures;
    atomic_long_t __extract_failures;
};
```

### printk_info — 日志记录元信息

```c
// kernel/printk/printk_ringbuffer.h
struct printk_info {
    u64     seq;            /* 序列号 */
    u64     ts_nsec;        /* 时间戳（纳秒） */
    u16     text_len;       /* 文本长度 */
    u8      facility;       /* syslog 设施类型 */
    u8      flags:5;        /* 记录标志 */
    u8      level:3;        /* 日志级别（0-7） */
    u32     caller_id;      /* 调用者 ID */
    struct dev_printk_info dev_info;  /* 设备信息 */
};
```

### printk_record — 日志记录

```c
// kernel/printk/printk_ringbuffer.h
struct printk_record {
    struct printk_info *info;    /* 元信息指针 */
    char *text_buf;              /* 文本缓冲区 */
    unsigned int text_buf_size;  /* 缓冲区大小 */
};
```

### SYSLOG_ACTION_* 宏定义

```c
// include/linux/syslog.h
#define SYSLOG_ACTION_CLOSE          0
#define SYSLOG_ACTION_OPEN           1
#define SYSLOG_ACTION_READ           2
#define SYSLOG_ACTION_READ_ALL       3
#define SYSLOG_ACTION_READ_CLEAR     4
#define SYSLOG_ACTION_CLEAR          5
#define SYSLOG_ACTION_CONSOLE_OFF    6
#define SYSLOG_ACTION_CONSOLE_ON     7
#define SYSLOG_ACTION_CONSOLE_LEVEL  8
#define SYSLOG_ACTION_SIZE_UNREAD    9
#define SYSLOG_ACTION_SIZE_BUFFER   10

#define SYSLOG_FROM_READER  0    /* 来自系统调用 */
#define SYSLOG_FROM_PROC    1    /* 来自 /proc/kmsg */
```

---

## 执行流程（ASCII 流程图）

```
                    ┌───────────────┐
                    │  Userspace    │
                    │  klogctl()    │
                    └───────┬───────┘
                            │ syscall
                            ▼
                    ┌───────────────┐
                    │ sys_syslog()  │
                    │ printk.c:1855 │
                    └───────┬───────┘
                            │
                            ▼
                    ┌───────────────┐
                    │ do_syslog()   │
                    │ printk.c:1742 │
                    └───────┬───────┘
                            │
                    ┌───────┴───────┐
                    │ 权限检查通过? │────✗──→ -EPERM
                    └───────┬───────┘
                            │ ✓
                    ┌───────┴──────────────────────────────┐
                    │         switch(type)                  │
                    │                                       │
                    │  READ ──────► syslog_print()          │
                    │    │              │                   │
                    │    │              └─ _prb_read_valid()│
                    │    │                  (消费模式)       │
                    │    │                                   │
                    │  READ_ALL ──► syslog_print_all()       │
                    │    │              │                   │
                    │    │              └─ prb_read_valid()  │
                    │    │                 (循环,非消费)     │
                    │    │                                   │
                    │  READ_CLEAR ─► syslog_print_all()      │
                    │    │              (clear=true)         │
                    │    │                                   │
                    │  CLEAR ────► syslog_clear()            │
                    │                                       │
                    │  CONSOLE_OFF/ON/LEVEL ─► 控制台级别   │
                    │                                       │
                    │  SIZE_UNREAD ─► 遍历缓冲区计算大小    │
                    │                                       │
                    │  SIZE_BUFFER ─► return log_buf_len    │
                    └───────────────────────────────────────┘
```

---

## 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EPERM` | 权限不足 | 没有 `CAP_SYSLOG` 能力，且 `dmesg_restrict` 已设置 |
| `-EINVAL` | 无效参数 | `type` 无效，或 `buf` 为 NULL 且 `len > 0`，或 `len < 0`，或 `CONSOLE_LEVEL` 超出 1-8 范围 |
| `-EFAULT` | 用户空间地址错误 | `buf` 指向不可访问的用户空间地址 |
| `-ENOSYS` | 系统未实现 | 内核编译时未启用 `CONFIG_PRINTK` |

### 安全限制

- **dmesg_restrict**（`/proc/sys/kernel/dmesg_restrict`）：=1 时，非特权用户无法读取内核日志
- **kptr_restrict**（`/proc/sys/kernel/kptr_restrict`）：控制内核指针在日志中的显示方式
- **CAP_SYSLOG**：绕过 `dmesg_restrict` 限制所需的能力
- **CAP_SYS_ADMIN**：旧版本内核中绕过日志限制需要此能力

---

## 使用示例

### 1. 读取所有内核日志（非消费模式）

```c
#include <sys/klog.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    /* 获取缓冲区大小 */
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

    /* 读取所有日志 */
    int ret = klogctl(3, buf, len);  /* SYSLOG_ACTION_READ_ALL */
    if (ret == -1) {
        perror("klogctl");
        free(buf);
        return 1;
    }

    buf[ret] = '\0';
    printf("=== Kernel log (%d bytes) ===\n%s", ret, buf);

    free(buf);
    return 0;
}
```

### 2. 消费模式读取日志

```c
#include <sys/klog.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char buf[4096];
    int ret;

    /* 消费模式读取：每次读取都会消耗缓冲区中的数据 */
    while ((ret = klogctl(2, buf, sizeof(buf) - 1)) > 0) {
        buf[ret] = '\0';
        printf("%s", buf);
    }

    if (ret == -1 && ret != 0)
        perror("klogctl");

    return 0;
}
```

### 3. 设置控制台日志级别

```c
#include <sys/klog.h>
#include <stdio.h>

int main(void)
{
    /* 设置控制台日志级别为 3（KERN_ERR 及以上）
     * 级别: 0=KERN_EMERG, 1=KERN_ALERT, 2=KERN_CRIT,
     *       3=KERN_ERR, 4=KERN_WARNING, 5=KERN_NOTICE,
     *       6=KERN_INFO, 7=KERN_DEBUG
     */
    if (klogctl(8, NULL, 3) == -1) {  /* SYSLOG_ACTION_CONSOLE_LEVEL */
        perror("klogctl");
        return 1;
    }
    printf("Console log level set to 3 (KERN_ERR and above)\n");
    return 0;
}
```

### 4. 完全禁用控制台日志输出

```c
#include <sys/klog.h>
#include <stdio.h>

int main(void)
{
    /* 禁用控制台日志输出 */
    if (klogctl(6, NULL, 0) == -1) {  /* SYSLOG_ACTION_CONSOLE_OFF */
        perror("klogctl");
        return 1;
    }
    printf("Console logging disabled\n");

    /* ... 执行某些操作 ... */

    /* 恢复控制台日志输出 */
    if (klogctl(7, NULL, 0) == -1) {  /* SYSLOG_ACTION_CONSOLE_ON */
        perror("klogctl");
        return 1;
    }
    printf("Console logging re-enabled\n");
    return 0;
}
```

---

## 与其他接口的关系

| 接口 | 类型 | 说明 |
|------|------|------|
| `syslog(2)` / `klogctl(2)` | 系统调用 | 本文档描述的核心接口 |
| `dmesg` 命令 | 命令行工具 | 用户空间工具，通过 `klogctl()` 读取内核日志 |
| `/dev/kmsg` | 设备文件 | 允许以文件 I/O 方式读取/写入内核日志 |
| `/proc/kmsg` | 设备文件 | 旧版接口，仅支持消费模式读取（已废弃） |
| `printk()` | 内核 API | 内核空间写入日志的接口 |

---

## 源码位置

| 文件 | 说明 |
|------|------|
| `kernel/printk/printk.c` | `sys_syslog()` 和 `do_syslog()` 实现 |
| `kernel/printk/printk_ringbuffer.h` | 环形缓冲区数据结构定义 |
| `kernel/printk/printk_ringbuffer.c` | 环形缓冲区读写操作实现 |
| `include/linux/syslog.h` | `SYSLOG_ACTION_*` 宏定义和 `do_syslog()` 声明 |
| `include/uapi/asm-generic/unistd.h` | `__NR_syslog` 系统调用编号定义 |