# sysctl 系统调用

## 概述

> **注意：sysctl 系统调用已从 Linux 内核中完全移除。** 在较新的内核版本（5.x 之后）中，`sys_sysctl()` 系统调用实现已被删除，`kernel/sysctl_binary.c` 文件也不再存在。当前内核中 `kernel/sysctl.c` 仅提供 `/proc/sys` 文件系统接口（即 `register_sysctl()` 等内核 API），而非系统调用。

`sysctl` 是一个历史遗留的系统调用，曾经用于查询和设置内核运行时参数。在现代 Linux 中，其所有功能已通过 `/proc/sys` 虚拟文件系统接口替代。在 ARM64 架构上，`sysctl` 从未拥有独立的系统调用编号。

---

## 历史函数原型

```c
#include <sys/syscall.h>
#include <linux/sysctl.h>

int sysctl(struct __sysctl_args *args);
```

### 参数说明

| 参数 | 类型 | 描述 |
|------|------|------|
| `args` | `struct __sysctl_args *` | 包含 name、nlen、oldval、oldlenp、newval、newlen 等字段的结构体指针 |

### __sysctl_args 结构体

```c
struct __sysctl_args {
    int *name;       /* 整形数组，表示 MIB 名称 */
    int nlen;        /* name 数组中的元素个数 */
    void *oldval;    /* 旧值缓冲区 */
    size_t *oldlenp; /* 旧值缓冲区大小 */
    void *newval;    /* 新值缓冲区 */
    size_t newlen;   /* 新值缓冲区大小 */
    unsigned long __unused[4];
};
```

---

## 功能说明

`sysctl` 系统调用曾经支持的操作：

- **读取内核运行时参数**：通过 `name` 数组指定 MIB（Management Information Base）路径，读取内核参数值
- **修改内核运行时参数**：通过 `newval` 和 `newlen` 设置新的参数值

---

## 当前替代方案

### 1. `/proc/sys` 文件系统接口

所有内核运行时参数通过 `/proc/sys` 目录下的文件暴露：

```bash
# 读取参数
cat /proc/sys/kernel/hostname

# 修改参数（需 root 权限）
echo "new-hostname" > /proc/sys/kernel/hostname
```

### 2. `sysctl` 命令工具

```bash
# 读取参数
sysctl kernel.hostname

# 修改参数
sysctl -w kernel.hostname="new-hostname"

# 列出所有参数
sysctl -a
```

### 3. 编程接口 — 直接读写 `/proc/sys` 文件

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int read_sysctl(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1)
        return -1;

    ssize_t ret = read(fd, buf, size - 1);
    if (ret > 0)
        buf[ret] = '\0';

    close(fd);
    return ret;
}

int write_sysctl(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd == -1)
        return -1;

    ssize_t ret = write(fd, value, strlen(value));
    close(fd);
    return ret;
}
```

---

## 内核 sysctl 框架（当前实现）

虽然 `sysctl` 系统调用已移除，但内核内部的 sysctl 框架仍然活跃，用于管理 `/proc/sys` 接口。核心数据结构如下：

### ctl_table — sysctl 表项

```c
// include/linux/sysctl.h
struct ctl_table {
    const char *procname;     /* /proc/sys 下的文件名 */
    void *data;               /* 指向内核变量的指针 */
    int maxlen;               /* 数据的最大长度 */
    umode_t mode;             /* 文件权限 */
    proc_handler *proc_handler; /* 读写回调函数 */
    struct ctl_table_poll *poll;
    void *extra1;             /* 额外参数（如最小值） */
    void *extra2;             /* 额外参数（如最大值） */
};
```

### 注册 sysctl 表

```c
// 示例：注册内核参数
static const struct ctl_table kern_table[] = {
    {
        .procname   = "hostname",
        .data       = &system_utsname.nodename,
        .maxlen     = __NEW_UTS_LEN,
        .mode       = 0644,
        .proc_handler = proc_dostring,
    },
    // ... 更多表项
    { }
};

// 注册到 /proc/sys/kernel 目录
register_sysctl_init("kernel", kern_table);
```

---

## 执行流程（历史版本）

```
sysctl(args)                                // 用户空间调用
  │
  └─ sys_sysctl(args)                       // kernel/sysctl_binary.c（已删除）
       │
       ├─ 权限检查 (CAP_SYS_ADMIN)
       │
       ├─ name → 路径转换（MIB → /proc/sys 路径）
       │
       ├─ 读取操作：将内核变量值复制到 oldval
       │
       └─ 写入操作：将 newval 写入内核变量
```

---

## 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-ENOSYS` | 功能未实现 | 当前内核版本中，sysctl 系统调用已被完全移除 |
| `-EPERM` | 权限不足 | 需要 `CAP_SYS_ADMIN` 能力（历史版本） |
| `-EINVAL` | 无效参数 | name 数组无效或参数错误（历史版本） |

---

## 使用示例（当前替代方案）

### 1. 读取内核参数

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(void)
{
    char buf[256];
    int fd = open("/proc/sys/kernel/hostname", O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    ssize_t ret = read(fd, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        printf("hostname: %s", buf);
    }

    close(fd);
    return 0;
}
```

### 2. 使用 sysctl 命令

```bash
# 读取
sysctl kernel.hostname
sysctl vm.swappiness
sysctl net.ipv4.tcp_keepalive_time

# 写入
sysctl -w vm.swappiness=10
sysctl -w net.ipv4.ip_forward=1
```

---

## 源码位置

| 文件 | 说明 |
|------|------|
| `kernel/sysctl.c` | 当前内核 sysctl 框架（仅 `/proc/sys` 接口，非系统调用） |
| `include/linux/sysctl.h` | `struct ctl_table` 定义和注册 API |
| `fs/proc/proc_sysctl.c` | `/proc/sys` 文件系统实现 |
| `kernel/printk/sysctl.c` | printk 相关的 sysctl 参数注册 |