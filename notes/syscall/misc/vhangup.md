# vhangup 系统调用分析

## 1. 概述

`vhangup`（Virtual Hangup）系统调用用于模拟当前控制终端上的挂起（hangup）操作。它通常由 `login` 程序在用户注销时调用，以确保所有进程获得干净的终端环境。

**原型：**

```c
SYSCALL_DEFINE0(vhangup);
```

### 返回值

- 成功时返回 0
- 权限不足时返回 `-EPERM`

## 2. 使用场景

- **用户登录/注销**：`login` 程序在用户登录前调用 `vhangup` 确保终端干净
- **终端重置**：需要重置终端状态时
- **守护进程**：某些守护进程在切换终端时使用
- **安全隔离**：确保后续用户无法读取前一个用户的数据

## 3. 完整实现

```c
// fs/open.c
SYSCALL_DEFINE0(vhangup)
{
    if (capable(CAP_SYS_TTY_CONFIG)) {
        tty_vhangup_self();
        return 0;
    }
    return -EPERM;
}
```

## 4. 函数调用栈

```
sys_vhangup()                                          // fs/open.c
  │
  ├─ capable(CAP_SYS_TTY_CONFIG)  // 检查权限
  │    └─ 无权限 → 返回 -EPERM
  │
  └─ tty_vhangup_self()                                // drivers/tty/tty_io.c
       │
       ├─ get_current_tty()     // 获取当前控制终端
       │
       └─ tty_vhangup(tty)                              // drivers/tty/tty_io.c
            │
            └─ __tty_hangup(tty, 0)                     // 核心实现
                 │
                 ├─ tty_release_redirect(tty)           // 清除重定向
                 │
                 ├─ 遍历 tty->tty_files 链表            // 所有打开该终端的文件
                 │    ├─ __tty_fasync(-1, filp, 0)      // 清除异步通知
                 │    └─ filp->f_op = &hung_up_tty_fops  // 替换操作函数为 hung_up
                 │
                 ├─ tty_signal_session_leader(tty, 0)   // 发送 SIGHUP 信号
                 │    └─ 向会话领导进程组发送 SIGHUP
                 │
                 ├─ tty_ldisc_hangup(tty, ...)           // 挂起线路规程
                 │    ├─ 刷新缓冲区
                 │    └─ 重置线路规程
                 │
                 ├─ 清除终端控制状态
                 │    ├─ clear_bit(TTY_THROTTLED)
                 │    ├─ clear_bit(TTY_DO_WRITE_WAKEUP)
                 │    ├─ put_pid(tty->ctrl.session)
                 │    └─ put_pid(tty->ctrl.pgrp)
                 │
                 ├─ tty->ops->hangup(tty)               // 驱动特定的 hangup 操作
                 │
                 └─ tty_kref_put(tty)                    // 释放 tty 引用
```

## 5. 关键数据结构

### tty_struct

```c
// include/linux/tty.h
struct tty_struct {
    int  magic;                    // 魔数，用于完整性检查
    struct tty_driver *driver;     // tty 驱动
    const struct tty_operations *ops;  // tty 操作函数表
    int index;                     // 索引号
    struct ld_semaphore ldisc_sem; // 线路规程信号量
    struct tty_ldisc *ldisc;       // 当前线路规程
    struct hlist_head tty_files;   // 打开此 tty 的文件链表
    int count;                     // 引用计数
    unsigned long flags;           // 标志位
    // ...
    struct tty_ctrl {
        spinlock_t lock;
        struct pid *pgrp;          // 前台进程组
        struct pid *session;       // 会话 ID
        unsigned char pktstatus;   // 数据包状态
        // ...
    } ctrl;
    struct work_struct hangup_work; // 挂起工作项
    // ...
};
```

### hung_up_tty_fops

```c
// drivers/tty/tty_io.c
// 挂起后的 tty 文件操作函数表
static const struct file_operations hung_up_tty_fops = {
    .llseek     = no_llseek,
    .read_iter  = hung_up_tty_read,
    .write_iter = hung_up_tty_write,
    .poll       = hung_up_tty_poll,
    .unlocked_ioctl = hung_up_tty_ioctl,
    .compat_ioctl   = hung_up_tty_compat_ioctl,
    .release   = tty_release,
};
```

### 信号发送

```c
// drivers/tty/tty_io.c
int tty_signal_session_leader(struct tty_struct *tty, int exit_session)
{
    struct pid *pgrp;
    unsigned long flags;
    int refs = 0;
    int ret;

    spin_lock_irqsave(&tty->ctrl.lock, flags);
    pgrp = get_pid(tty->ctrl.pgrp);
    if (exit_session)
        ret = tty->ctrl.session != NULL;
    else
        ret = 1;
    spin_unlock_irqrestore(&tty->ctrl.lock, flags);

    if (pgrp) {
        // 向进程组发送 SIGHUP
        kill_pgrp(pgrp, SIGHUP, exit_session ? 1 : 0);
        if (!exit_session)
            kill_pgrp(pgrp, SIGCONT, 0);
        put_pid(pgrp);
    }
    return refs;
}
```

## 6. 流程图

```
用户态调用 vhangup()
  │
  ▼
sys_vhangup()
  │
  ├─ capable(CAP_SYS_TTY_CONFIG)?
  │    │
  │    ├─ 否 ──→ 返回 -EPERM
  │    │
  │    └─ 是
  │         │
  │         ▼
  │    tty_vhangup_self()
  │         │
  │         ├─ get_current_tty()  ── NULL ──→ 返回（无操作）
  │         │
  │         └─ tty_vhangup(tty)
  │              │
  │              ▼
  │         __tty_hangup(tty, 0)
  │              │
  │              ├─ 清除 tty 重定向
  │              │
  │              ├─ 遍历所有打开该 tty 的文件
  │              │    └─ 替换 f_op 为 hung_up_tty_fops
  │              │
  │              ├─ 向进程组发送 SIGHUP + SIGCONT
  │              │
  │              ├─ 挂起线路规程
  │              │
  │              ├─ 清除终端控制信息
  │              │
  │              ├─ 调用驱动 hangup 回调
  │              │
  │              └─ 释放 tty 引用
  │
  ▼
返回 0
```

## 7. 使用示例

```c
#include <unistd.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 尝试 vhangup，需要 CAP_SYS_TTY_CONFIG 权限
    if (syscall(__NR_vhangup) == -1) {
        perror("vhangup");
        exit(EXIT_FAILURE);
    }
    
    printf("vhangup 成功\n");
    return 0;
}
```

## 8. 关键设计要点

### 权限控制

- 需要 `CAP_SYS_TTY_CONFIG` 能力（capability）
- 通常在 `login` 程序以 root 权限运行时调用
- 普通用户进程没有此权限，调用会返回 `-EPERM`

### 挂起后的操作

终端被挂起后，所有对该终端的读写操作都会返回错误：
- `read()` 返回 `-EIO`
- `write()` 返回 `-EIO`（或部分写入后返回错误）
- `poll()` 返回 `POLLHUP | POLLERR`
- `ioctl()` 部分命令可能仍然有效

### 安全性

- 挂起操作确保后续进程无法访问前一个进程的终端数据
- 防止信息泄露（如 `su` 或 `login` 后读取前一个用户的输入）

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#其他杂项)
- 源码位置：`fs/open.c`、`drivers/tty/tty_io.c`