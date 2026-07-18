# reboot 系统调用

## 概述

`reboot` 系统调用用于重启、关机或执行系统级别的电源管理操作。它是系统管理员执行系统关机、重启等操作的核心接口。由于此操作具有系统级影响，内核对此实施了严格的权限控制——仅允许具有 `CAP_SYS_BOOT` 能力的进程执行，且需要通过特定的 magic 参数来防止误调用。

在 ARM64 架构上，`reboot` 系统调用编号为 `__NR_reboot = 142`。

---

## 函数原型

```c
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/syscall.h>

long syscall(SYS_reboot, int magic1, int magic2, unsigned int cmd, void __user *arg);
```

或使用库函数封装：

```c
#include <sys/reboot.h>

int reboot(int cmd);
```

### 参数说明

| 参数 | 类型 | 描述 |
|------|------|------|
| `magic1` | `int` | 第一个魔数，必须为 `LINUX_REBOOT_MAGIC1`（`0xfee1dead`） |
| `magic2` | `int` | 第二个魔数，必须为 `LINUX_REBOOT_MAGIC2`（`0x28121969`）或其变体 |
| `cmd` | `unsigned int` | 操作命令（见下方命令表） |
| `arg` | `void __user *` | 命令参数指针（仅 `RESTART2` 使用，传递重启命令字符串） |

### 魔数定义

```c
// include/uapi/linux/reboot.h
#define LINUX_REBOOT_MAGIC1  0xfee1dead
#define LINUX_REBOOT_MAGIC2  0x28121969
#define LINUX_REBOOT_MAGIC2A 0x05121996
#define LINUX_REBOOT_MAGIC2B 0x16041998
#define LINUX_REBOOT_MAGIC2C 0x20112000
```

### 命令列表

| 宏定义 | 值 | 描述 |
|--------|-----|------|
| `LINUX_REBOOT_CMD_RESTART` | `0x01234567` | 重启系统 |
| `LINUX_REBOOT_CMD_HALT` | `0xCDEF0123` | 停止系统（halt） |
| `LINUX_REBOOT_CMD_CAD_ON` | `0x89ABCDEF` | 启用 Ctrl-Alt-Del 重启 |
| `LINUX_REBOOT_CMD_CAD_OFF` | `0x00000000` | 禁用 Ctrl-Alt-Del 重启 |
| `LINUX_REBOOT_CMD_POWER_OFF` | `0x4321FEDC` | 关闭系统电源 |
| `LINUX_REBOOT_CMD_RESTART2` | `0xA1B2C3D4` | 带参重启 |
| `LINUX_REBOOT_CMD_SW_SUSPEND` | `0xD000FCE2` | 休眠（需 `CONFIG_HIBERNATION`） |
| `LINUX_REBOOT_CMD_KEXEC` | `0x45584543` | 通过 kexec 启动新内核（需 `CONFIG_KEXEC_CORE`） |

---

## 详细调用链分析

### 内核入口

```c
// kernel/reboot.c
SYSCALL_DEFINE4(reboot, int, magic1, int, magic2, unsigned int, cmd,
                void __user *, arg)
{
    struct pid_namespace *pid_ns = task_active_pid_ns(current);
    char buffer[256];
    int ret = 0;

    /* 权限检查：需要 CAP_SYS_BOOT 能力 */
    if (!ns_capable(pid_ns->user_ns, CAP_SYS_BOOT))
        return -EPERM;

    /* 魔数检查：防止误调用 */
    if (magic1 != LINUX_REBOOT_MAGIC1 ||
            (magic2 != LINUX_REBOOT_MAGIC2 &&
             magic2 != LINUX_REBOOT_MAGIC2A &&
             magic2 != LINUX_REBOOT_MAGIC2B &&
             magic2 != LINUX_REBOOT_MAGIC2C))
        return -EINVAL;

    /* 处理 pid 命名空间内的 reboot */
    ret = reboot_pid_ns(pid_ns, cmd);
    if (ret)
        return ret;

    /* 如果系统不支持电源关闭，回退到 halt */
    if ((cmd == LINUX_REBOOT_CMD_POWER_OFF) && !kernel_can_power_off()) {
        poweroff_fallback_to_halt = true;
        cmd = LINUX_REBOOT_CMD_HALT;
    }

    mutex_lock(&system_transition_mutex);
    switch (cmd) {
    case LINUX_REBOOT_CMD_RESTART:
        kernel_restart(NULL);
        break;
    case LINUX_REBOOT_CMD_CAD_ON:
        C_A_D = 1;
        break;
    case LINUX_REBOOT_CMD_CAD_OFF:
        C_A_D = 0;
        break;
    case LINUX_REBOOT_CMD_HALT:
        kernel_halt();
        do_exit(0);
    case LINUX_REBOOT_CMD_POWER_OFF:
        kernel_power_off();
        do_exit(0);
        break;
    case LINUX_REBOOT_CMD_RESTART2:
        ret = strncpy_from_user(&buffer[0], arg, sizeof(buffer) - 1);
        if (ret < 0) {
            ret = -EFAULT;
            break;
        }
        buffer[sizeof(buffer) - 1] = '\0';
        kernel_restart(buffer);
        break;
#ifdef CONFIG_KEXEC_CORE
    case LINUX_REBOOT_CMD_KEXEC:
        ret = kernel_kexec();
        break;
#endif
#ifdef CONFIG_HIBERNATION
    case LINUX_REBOOT_CMD_SW_SUSPEND:
        ret = hibernate();
        break;
#endif
    default:
        ret = -EINVAL;
        break;
    }
    mutex_unlock(&system_transition_mutex);
    return ret;
}
```

### 完整调用链

```
SYSCALL_DEFINE4(reboot, magic1, magic2, cmd, arg)    // kernel/reboot.c
  │
  ├─ 检查 magic1 == LINUX_REBOOT_MAGIC1
  ├─ 检查 magic2 == LINUX_REBOOT_MAGIC2 / 2A / 2B / 2C
  ├─ ns_capable(pid_ns->user_ns, CAP_SYS_BOOT)
  ├─ reboot_pid_ns(pid_ns, cmd)          // 子命名空间处理
  │
  └─ mutex_lock(&system_transition_mutex)
       └─ switch(cmd):
            │
            ├─ LINUX_REBOOT_CMD_RESTART:
            │    └─ kernel_restart(NULL)
            │         ├─ kernel_restart_prepare(NULL)
            │         │    ├─ blocking_notifier_call_chain(reboot_notifier_list, SYS_RESTART, NULL)
            │         │    ├─ usermodehelper_disable()
            │         │    └─ device_shutdown()
            │         ├─ do_kernel_restart_prepare()
            │         ├─ migrate_to_reboot_cpu()
            │         ├─ syscore_shutdown()
            │         └─ machine_restart(NULL)
            │              └─ arch-specific (ARM64: psci_system_reset)
            │
            ├─ LINUX_REBOOT_CMD_RESTART2:
            │    ├─ strncpy_from_user(buffer, arg, 255)
            │    └─ kernel_restart(buffer)
            │
            ├─ LINUX_REBOOT_CMD_POWER_OFF:
            │    └─ kernel_power_off()
            │         ├─ kernel_shutdown_prepare(SYSTEM_POWER_OFF)
            │         │    ├─ reboot_notifier_call_chain(SYS_POWER_OFF)
            │         │    ├─ usermodehelper_disable()
            │         │    └─ device_shutdown()
            │         ├─ do_kernel_power_off_prepare()
            │         ├─ migrate_to_reboot_cpu()
            │         ├─ syscore_shutdown()
            │         └─ machine_power_off()
            │              └─ arch-specific (ARM64: psci_system_off)
            │
            ├─ LINUX_REBOOT_CMD_HALT:
            │    └─ kernel_halt() → machine_halt()
            │
            ├─ LINUX_REBOOT_CMD_CAD_ON / CAD_OFF:
            │    └─ 设置 C_A_D 标志位
            │
            ├─ LINUX_REBOOT_CMD_KEXEC:
            │    └─ kernel_kexec()  // 通过 kexec 加载新内核
            │
            └─ LINUX_REBOOT_CMD_SW_SUSPEND:
                 └─ hibernate()    // 进入休眠状态
```

---

## 关键数据结构

### sys_off_handler — 系统关闭操作处理器

```c
// kernel/reboot.c
struct sys_off_handler {
    struct notifier_block nb;
    int (*sys_off_cb)(struct sys_off_data *data);
    void *cb_data;
    enum sys_off_mode mode;
    bool blocking;
    void *list;
    struct device *dev;
};
```

### 重启通知链

内核使用通知链（notifier chain）机制，允许内核模块在重启/关机时执行清理操作：

```c
// kernel/reboot.c
static BLOCKING_NOTIFIER_HEAD(reboot_notifier_list);
```

- `register_reboot_notifier()` / `unregister_reboot_notifier()`：注册/注销重启通知回调
- `devm_register_reboot_notifier()`：资源管理的注册版本

### 关键全局变量

```c
// kernel/reboot.c
static int C_A_D = 1;                     /* Ctrl-Alt-Del 是否启用 */
enum reboot_mode reboot_mode;             /* 重启模式（冷/热/硬） */
int reboot_cpu;                           /* 重启时使用的 CPU */
enum reboot_type reboot_type = BOOT_ACPI; /* 重启类型 */
int reboot_force;                         /* 强制重启标志 */
```

---

## 执行流程（ASCII 流程图）

```
                    ┌───────────────┐
                    │  reboot()     │
                    │  (userspace)  │
                    └───────┬───────┘
                            │ syscall
                            ▼
                    ┌───────────────┐
                    │  sys_reboot() │
                    │  kernel/      │
                    │  reboot.c     │
                    └───────┬───────┘
                            │
                    ┌───────┴───────┐
                    │ 魔数检查通过? │────✗──→ -EINVAL
                    └───────┬───────┘
                            │ ✓
                    ┌───────┴───────┐
                    │ CAP_SYS_BOOT? │────✗──→ -EPERM
                    └───────┬───────┘
                            │ ✓
                    ┌───────┴──────────────────────────┐
                    │  switch(cmd):                     │
                    │                                    │
                    │  RESTART ──► kernel_restart()      │
                    │       │                            │
                    │       ├─ kernel_restart_prepare()  │
                    │       ├─ migrate_to_reboot_cpu()   │
                    │       ├─ syscore_shutdown()        │
                    │       └─ machine_restart()         │
                    │                                    │
                    │  POWER_OFF ──► kernel_power_off()  │
                    │       │                            │
                    │       └─ machine_power_off()       │
                    │                                    │
                    │  HALT ──► kernel_halt()            │
                    │       │                            │
                    │       └─ machine_halt()            │
                    │                                    │
                    │  KEXEC ──► kernel_kexec()          │
                    │                                    │
                    │  SW_SUSPEND ──► hibernate()        │
                    └────────────────────────────────────┘
```

---

## 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EPERM` | 权限不足 | 调用者没有 `CAP_SYS_BOOT` 能力 |
| `-EINVAL` | 无效参数 | `magic1` 或 `magic2` 不正确，或 `cmd` 无效 |
| `-EFAULT` | 用户空间地址错误 | `RESTART2` 时 `arg` 指向不可访问地址 |
| `-ENOSYS` | 功能未实现 | `KEXEC` 未配置 `CONFIG_KEXEC_CORE`，或 `SW_SUSPEND` 未配置 `CONFIG_HIBERNATION` |

### 安全限制

- 必须具有 `CAP_SYS_BOOT` 能力（通常在初始命名空间中的 root 用户拥有）
- 子 pid 命名空间中的 `reboot()` 调用会执行 `reboot_pid_ns()`，该函数仅发送信号到父进程，不会真正重启系统
- 魔数机制防止应用程序意外调用 `reboot` 系统调用

---

## 使用示例

### 1. 重启系统

```c
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    sync();  /* 同步文件系统 */

    long ret = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1,
                       LINUX_REBOOT_MAGIC2,
                       LINUX_REBOOT_CMD_RESTART, NULL);
    if (ret == -1) {
        perror("reboot");
        return 1;
    }
    /* 不会到达这里 */
    return 0;
}
```

### 2. 关闭系统电源

```c
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <stdio.h>

int main(void)
{
    sync();

    long ret = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1,
                       LINUX_REBOOT_MAGIC2,
                       LINUX_REBOOT_CMD_POWER_OFF, NULL);
    if (ret == -1) {
        perror("reboot");
        return 1;
    }
    return 0;
}
```

### 3. 带参数重启

```c
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <stdio.h>

int main(void)
{
    sync();

    /* 传递重启命令（如引导加载程序参数） */
    long ret = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1,
                       LINUX_REBOOT_MAGIC2,
                       LINUX_REBOOT_CMD_RESTART2, "recovery");
    if (ret == -1) {
        perror("reboot");
        return 1;
    }
    return 0;
}
```

### 4. 使用库函数封装

```c
#include <sys/reboot.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    sync();

    if (reboot(RB_POWER_OFF) == -1) {  /* RB_POWER_OFF 对应 LINUX_REBOOT_CMD_POWER_OFF */
        perror("reboot");
        return 1;
    }
    return 0;
}
```

> **注意**：`reboot()` 库函数内部会处理魔数参数，但需要调用者具有 `CAP_SYS_BOOT` 能力。

---

## 源码位置

| 文件 | 说明 |
|------|------|
| `kernel/reboot.c` | `SYSCALL_DEFINE4(reboot)` 实现，`kernel_restart()`、`kernel_power_off()`、`kernel_halt()` 等核心函数 |
| `include/uapi/linux/reboot.h` | `LINUX_REBOOT_MAGIC1/2` 和 `LINUX_REBOOT_CMD_*` 宏定义 |
| `include/linux/syscalls.h` | `sys_reboot` 声明 |
| `include/uapi/asm-generic/unistd.h` | `__NR_reboot` 系统调用编号定义 |