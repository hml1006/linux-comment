# Panic 触发与行为控制

## 1. 概述

Linux 内核提供了多种机制来触发 panic 和控制 panic 行为，用于调试和故障诊断。

---

## 2. 触发 Panic

### 2.1 BUG_ON()

`BUG_ON()` 是开发者主动触发 panic 的宏，当条件为真时调用 `panic()`。

```c
#define BUG_ON(condition) do { \
    if (unlikely(condition)) \
        BUG(); \
} while (0)
```

**使用场景：**
- 检测到不可能发生的状态
- 严重的编程错误

### 2.2 Kernel Oops → Panic

通过配置将 Oops 升级为 panic：

```bash
# 编译时配置
CONFIG_PANIC_ON_OOPS=y

# 内核启动参数
oops=panic

# 运行时配置
echo 1 > /proc/sys/kernel/panic_on_oops
```

### 2.3 panic_on_warn

让 `WARN_ON` 警告也触发 panic：

```c
int panic_on_warn __read_mostly;

void check_panic_on_warn(const char *origin)
{
    unsigned int limit;

    if (panic_on_warn)
        panic("%s: panic_on_warn set ...\n", origin);

    limit = READ_ONCE(warn_limit);
    if (atomic_inc_return(&warn_count) >= limit && limit)
        panic("%s: system warned too often (kernel.warn_limit is %d)",
              origin, limit);
}
```

**配置方式：**

```bash
# 内核启动参数
panic_on_warn=1

# 运行时配置
echo 1 > /proc/sys/kernel/panic_on_warn
```

### 2.4 SysRq 手动触发

```bash
echo c > /proc/sysrq-trigger
```

---

## 3. 控制 Panic 行为

### 3.1 panic_timeout

panic 后自动重启的等待秒数。

```c
int panic_timeout = CONFIG_PANIC_TIMEOUT;
EXPORT_SYMBOL_GPL(panic_timeout);
```

**配置方式：**

```bash
# 编译时配置 (Linux 7.0+)
CONFIG_PANIC_TIMEOUT=60

# 内核启动参数
panic=60

# 运行时配置
echo 60 > /proc/sys/kernel/panic
```

**注意：** 值为 0 表示无限等待，负值表示立即重启。

### 3.2 panic_on_oops

控制发生 Oops 时是否触发 panic。

```c
int panic_on_oops = IS_ENABLED(CONFIG_PANIC_ON_OOPS);
```

**配置方式：**

```bash
# 编译时配置
CONFIG_PANIC_ON_OOPS=y

# 运行时配置
echo 1 > /proc/sys/kernel/panic_on_oops
```

### 3.3 panic_print / panic_sys_info

控制 panic 时打印哪些额外调试信息。

**panic_print 已废弃，推荐使用 panic_sys_info：**

```c
unsigned long panic_print;

static void panic_print_deprecated(void)
{
    pr_info_once("Kernel: The 'panic_print' parameter is now deprecated. "
                 "Please use 'panic_sys_info' and 'panic_console_replay' instead.\n");
}
```

**配置方式：**

```bash
# 内核启动参数
panic_sys_info=tasks,mem,locks,ftrace,all_bt

# 运行时配置
echo 3 > /proc/sys/kernel/panic_sys_info
```

**可用选项：**

| 选项 | 说明 |
|------|------|
| `tasks` | 打印任务列表 |
| `mem` | 打印内存信息 |
| `locks` | 打印锁信息 |
| `ftrace` | 打印 ftrace 缓冲区 |
| `all_bt` | 打印所有 CPU 堆栈 |
| `console_replay` | 重放控制台日志 |

### 3.4 panic_on_taint

系统被"污染"（tainted）后触发 panic。

```c
unsigned long panic_on_taint;
bool panic_on_taint_nousertaint = false;
```

**工作原理：**

```c
void add_taint(unsigned flag, enum lockdep_ok lockdep_ok)
{
    if (lockdep_ok == LOCKDEP_NOW_UNRELIABLE && __debug_locks_off())
        pr_warn("Disabling lock debugging due to kernel taint\n");

    set_bit(flag, &tainted_mask);

    if (tainted_mask & panic_on_taint) {
        panic_on_taint = 0;
        panic("panic_on_taint set ...");
    }
}
```

**配置方式：**

```bash
# 内核启动参数 (十六进制位掩码)
panic_on_taint=0x100
```

**常用 Taint 标志：**

| 标志 | 值 | 说明 |
|------|-----|------|
| `TAINT_PROPRIETARY_MODULE` | 0x0001 | 加载了专有模块 |
| `TAINT_OOT_MODULE` | 0x0002 | 加载了 out-of-tree 模块 |
| `TAINT_UNSIGNED_MODULE` | 0x0004 | 加载了未签名模块 |
| `TAINT_CRAP` | 0x0020 | 检测到内核错误 |
| `TAINT_FIRMWARE_WORKAROUND` | 0x0080 | 使用了固件 workaround |
| `TAINT_RANDSTRUCT` | 0x4000000 | 使用了随机化结构布局 |

---

## 4. 核心数据结构

```c
int panic_on_oops = IS_ENABLED(CONFIG_PANIC_ON_OOPS);
int panic_on_warn __read_mostly;
unsigned long panic_on_taint;
bool panic_on_taint_nousertaint = false;

int panic_timeout = CONFIG_PANIC_TIMEOUT;
unsigned long panic_print;

static unsigned int warn_limit __read_mostly;
static atomic_t warn_count = ATOMIC_INIT(0);
```

---

## 5. 运行时控制接口

| 文件 | 说明 |
|------|------|
| `/proc/sys/kernel/panic` | panic_timeout 控制 |
| `/proc/sys/kernel/panic_on_oops` | Oops 时是否 panic |
| `/proc/sys/kernel/panic_on_warn` | Warn 时是否 panic |
| `/proc/sys/kernel/panic_print` | panic 时打印信息（已废弃） |
| `/proc/sys/kernel/panic_sys_info` | panic 时打印信息 |
| `/proc/sys/kernel/warn_limit` | 警告次数上限 |
| `/sys/kernel/warn_count` | 当前警告次数 |
| `/proc/sys/kernel/tainted` | 系统污染状态 |
| `/proc/sys/kernel/oops_all_cpu_backtrace` | Oops 时打印所有 CPU 堆栈 |

---

## 6. panic 处理流程

```
panic()
    │
    ├── 1. 禁用本地中断和抢占
    ├── 2. 尝试在目标 CPU 上执行 panic
    ├── 3. 调用 panic_notifier_list 通知链
    ├── 4. 输出 panic 信息
    ├── 5. 根据 panic_print 打印额外信息
    ├── 6. 触发其他 CPU 堆栈转储
    ├── 7. 关闭其他 CPU
    ├── 8. 调用 kmsg_dump() 保存日志
    ├── 9. 调用 crash_kexec() 执行崩溃转储
    ├── 10. 根据 panic_timeout 等待或重启
    └── 11. 进入死循环
```

---

## 7. 代码位置

- `kernel/panic.c`
- `include/linux/panic.h`