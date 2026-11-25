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

### 6.1 vpanic() 的真实执行顺序

上面的流程图是概念版。本树真正的顺序在 `vpanic()` 里，且与流程图有两处重要差异：**kdump 可能在 notifier 之前**、**kmsg_dump 在 notifier 之后**。

```c
void vpanic(const char *fmt, va_list args)
{
	static char buf[PANIC_MSG_BUFSZ];
	...
	if (panic_on_warn)
		panic_on_warn = 0;          /* 防止 panic 路径里再 WARN 导致递归 panic */

	local_irq_disable();
	preempt_disable_notrace();

	if (panic_try_force_cpu(fmt, args)) {
		set_cpu_online(smp_processor_id(), false);
		panic_smp_self_stop();
	}

	if (panic_try_start()) {
		/* 第一个到达的 CPU */
	} else if (panic_on_other_cpu())
		panic_smp_self_stop();

	console_verbose();
	bust_spinlocks(1);
	len = vscnprintf(buf, sizeof(buf), fmt, args);
	...
	pr_emerg("Kernel panic - not syncing: %s\n", buf);

	if (atomic_read(&panic_redirect_cpu) != PANIC_CPU_INVALID &&
	    panic_force_cpu == raw_smp_processor_id()) {
		pr_emerg("panic: Redirected from CPU %d, skipping stack dump.\n", ...);
	} else if (test_taint(TAINT_DIE) || oops_in_progress > 1) {
		panic_this_cpu_backtrace_printed = true;
	} else if (IS_ENABLED(CONFIG_DEBUG_BUGVERBOSE)) {
		dump_stack();
		panic_this_cpu_backtrace_printed = true;
	}

	kgdb_panic(buf);

	if (!_crash_kexec_post_notifiers)
		__crash_kexec(NULL);

	panic_other_cpus_shutdown(_crash_kexec_post_notifiers);

	printk_legacy_allow_panic_sync();

	atomic_notifier_call_chain(&panic_notifier_list, 0, buf);

	sys_info(panic_print);

	kmsg_dump_desc(KMSG_DUMP_PANIC, buf);

	if (_crash_kexec_post_notifiers)
		__crash_kexec(NULL);

	console_unblank();
	debug_locks_off();
	console_flush_on_panic(CONSOLE_FLUSH_PENDING);
	...
}
```

顺序要点：

| 步骤 | 代码 | 为什么在这个位置 |
|--|--|--|
| 1 | `local_irq_disable()` + `preempt_disable_notrace()` | 防止 panic 过程中被中断再次 panic |
| 2 | `panic_try_start()` | `cmpxchg` 抢占 `panic_cpu`，只有第一个 CPU 继续 |
| 3 | `dump_stack()` | 只在 `CONFIG_DEBUG_BUGVERBOSE` 且不是"oops 升级"时打印，避免重复栈 |
| 4 | `kgdb_panic()` | 必须在关其他 CPU 之前，否则调试器无法接管 |
| 5 | `__crash_kexec(NULL)`（默认） | 默认**先 kdump**，因为 kdump 需要其他 CPU 还活着执行 `machine_crash_shutdown()` |
| 6 | `panic_other_cpus_shutdown()` | 按 `panic_print & SYS_INFO_ALL_BT` 决定是否先抓所有 CPU 栈；再 `smp_send_stop()` 或 `crash_smp_send_stop()` |
| 7 | `panic_notifier_list` | 通知链里的回调可能写 pstore/leds/看门狗，必须在 kmsg_dump 写日志之前 |
| 8 | `sys_info(panic_print)` | 按位打印任务/内存/锁/ftrace 等信息 |
| 9 | `kmsg_dump_desc(KMSG_DUMP_PANIC, buf)` | 把内核日志交给 pstore/ramoops 等 dumper |
| 10 | `console_flush_on_panic()` | 把控制台缓冲刷出去，保证串口能看到 |
| 11 | `panic_timeout` 循环 → `emergency_restart()` | 超时后硬重启 |

`panic()` 本身只是 `vpanic()` 的变参包装：

```c
void panic(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vpanic(fmt, args);
	va_end(args);
}
```

### 6.2 panic_notifier_list 与 crash_kexec_post_notifiers

`panic_notifier_list` 是一个 `ATOMIC_NOTIFIER_HEAD`：

```c
ATOMIC_NOTIFIER_HEAD(panic_notifier_list);
EXPORT_SYMBOL(panic_notifier_list);
```

"atomic" 表示回调里不能睡眠——panic 路径可能来自中断/NMI，且其他 CPU 已经被停。典型注册者包括 pstore、leds trigger、watchdog、virtio 等。

`crash_kexec_post_notifiers` 是一个全局 `bool`，决定 kdump 与 notifier 的先后：

| 取值 | 顺序 | 风险 |
|--|--|--|
| false（默认） | `__crash_kexec()` → `panic_other_cpus_shutdown()` → notifier → kmsg_dump | notifier 里的日志不会进入 crash dump |
| true | `panic_other_cpus_shutdown()` → notifier → kmsg_dump → `__crash_kexec()` | notifier 可能让内核更不稳定，增加 kdump 失败概率（源码注释明确警告） |

### 6.3 panic_print / panic_sys_info 的位定义

`panic_print` 是一个位掩码，定义在 `include/linux/sys_info.h`：

```c
#define SYS_INFO_TASKS			0x00000001
#define SYS_INFO_MEM			0x00000002
#define SYS_INFO_TIMERS			0x00000004
#define SYS_INFO_LOCKS			0x00000008
#define SYS_INFO_FTRACE			0x00000010
#define SYS_INFO_PANIC_CONSOLE_REPLAY	0x00000020
#define SYS_INFO_ALL_BT			0x00000040
#define SYS_INFO_BLOCKED_TASKS		0x00000080
```

三个入口都指向同一个变量：

| 入口 | 实现 |
|--|--|
| 启动参数 `panic_sys_info=tasks,mem,locks,ftrace,all_bt` | `__setup("panic_sys_info=", setup_panic_sys_info)` → `sys_info_parse_param(buf)` |
| 启动参数 `panic_print=...` | 已废弃，写 sysctl 时 `sysctl_panic_print_handler()` 打印一次弃用提示 |
| 运行时 `/proc/sys/kernel/panic_sys_info` | `sysctl_sys_info_handler()`（`proc_doulongvec_minmax` 的包装） |

`SYS_INFO_PANIC_CONSOLE_REPLAY` 比较特殊：它只在 panic 路径有意义，作用是让 `console_flush_on_panic(CONSOLE_REPLAY_ALL)` 重放整个控制台缓冲，而不是只刷未输出的部分。

### 6.4 panic_timeout 的三段语义

```c
int panic_timeout = CONFIG_PANIC_TIMEOUT;
EXPORT_SYMBOL_GPL(panic_timeout);
```

| 取值 | 行为 |
|--|--|
| `> 0` | 进入 `panic_timeout * 1000 / PANIC_TIMER_STEP` 次 `mdelay(100)` 的循环，期间 `panic_blink()` 闪灯、`touch_nmi_watchdog()`；循环结束若 `panic_timeout != 0` 则 `emergency_restart()` |
| `== 0` | 不重启，停在死循环并持续 `touch_softlockup_watchdog()` |
| `< 0` | 跳过等待，直接 `emergency_restart()` |

`PANIC_TIMER_STEP` 是 100（毫秒），`PANIC_BLINK_SPD` 是 18，`panic_blink` 默认指向 `no_blink()`，由体系结构或 leds 驱动替换。

运行时设置走 `/proc/sys/kernel/panic`（`proc_dointvec`，允许负值）；启动参数 `panic=N` 在本树是通过 `core_param(panic, panic_timeout, int, 0644)` 实现的（走 `kernel/params.c`，不是 `__setup`），因此它同时也出现在 `/sys/module/kernel/parameters/panic`。

重启模式可以单独指定：`reboot=panic_warm` / `panic_cold` 会被 `kernel/reboot.c` 的 `reboot_setup()` 解析到 `panic_reboot_mode`，panic 时据此覆盖全局 `reboot_mode`。

### 6.5 各 `panic_on_*` 的判定位置

| 开关 | 判定位置 | 说明 |
|--|--|--|
| `panic_on_oops` | 体系结构 `die()`（arm64：`arch/arm64/kernel/traps.c` 的 `die()`） | `if (panic_on_oops) panic("%s: Fatal exception", str);`；`in_interrupt()` 时无条件 panic |
| `panic_on_warn` | `check_panic_on_warn()`（`kernel/panic.c`） | 被 `__warn()`、`scheduling while atomic`、KCSAN、UBSAN 等调用 |
| `warn_limit` | 同上 | `atomic_inc_return(&warn_count) >= limit` 时 panic |
| `panic_on_taint` | `add_taint()` | `tainted_mask & panic_on_taint` 时先把 `panic_on_taint` 清零再 panic，避免递归 |
| `panic_on_stackoverflow` | x86_32 / parisc 的栈溢出检测 | 仅这两个架构注册该 sysctl |
| `panic_force_cpu` | `panic_try_force_cpu()` | `early_param("panic_force_cpu", ...)`，把 panic 重定向到指定 CPU 执行 |

`panic_on_taint` 配套还有 `panic_on_taint_nousertaint`：写 `/proc/sys/kernel/tainted` 时，如果用户试图设置的 taint 位会立刻触发 panic，`proc_taint()` 直接返回 `-EINVAL`，避免"用户自己把机器打崩"。

### 6.6 与 pstore / ramoops 的衔接

panic 路径里唯一"持久化日志"的动作是：

```c
kmsg_dump_desc(KMSG_DUMP_PANIC, buf);
```

它遍历 `dump_list` 上注册的 `kmsg_dumper`，并按每个 dumper 的 `max_reason` 过滤：

```c
	rcu_read_lock();
	list_for_each_entry_rcu(dumper, &dump_list, list) {
		enum kmsg_dump_reason max_reason = dumper->max_reason;

		if (max_reason == KMSG_DUMP_UNDEF) {
			max_reason = always_kmsg_dump ? KMSG_DUMP_MAX :
							KMSG_DUMP_OOPS;
		}
		if (reason > max_reason)
			continue;
		...
	}
```

pstore 的 dumper 在 `fs/pstore/platform.c`：

```c
static struct kmsg_dumper pstore_dumper = {
	.dump = pstore_dump,
};

kmsg_dump_register(&pstore_dumper);
```

`pstore_dump()` 从日志尾部往前取最多 `kmsg_bytes`，按 `record_size` 切成多条记录，写入后端。ramoops 后端（`fs/pstore/ram.c`）通过 `pstore_register()` 注册，提供 `ramoops_pstore_write` 等方法，把数据写到预留的 persistent RAM：

```c
		.write	= ramoops_pstore_write,
		.write_user	= ramoops_pstore_write_user,
		.erase	= ramoops_pstore_erase,
```

几个关键参数：

| 参数 | 位置 | 作用 |
|--|--|--|
| `record_size` | `fs/pstore/ram.c` 模块参数 | 单条记录的最大字节数 |
| `max_reason` | 模块参数 / 设备树 | 默认 `KMSG_DUMP_OOPS`；只有 reason ≤ max_reason 才 dump |
| `dump_oops` | 已废弃参数 | 1 → `KMSG_DUMP_OOPS`，0 → 仅 `KMSG_DUMP_PANIC` |
| `ecc_info` | `struct ramoops_context` | persistent RAM 的 ECC 配置 |

因此链路是：

```
panic()
  → kmsg_dump_desc(KMSG_DUMP_PANIC)
  → pstore_dump()（platform.c, 受 max_reason 过滤）
  → ramoops_pstore_write()（ram.c）
  → persistent RAM（重启后由 pstore 文件系统读回）
```

注意 `max_reason` 的默认值决定了"panic 时到底会不会被保存"：如果 dumper 的 `max_reason < KMSG_DUMP_PANIC`，panic 日志会被跳过。这也是"ramoops 里只有 oops 没有 panic"的常见原因。本树 `include/generated/autoconf.h` 未定义 `CONFIG_PSTORE`，`fs/pstore/` 不参与编译；但 `CONFIG_CRASH_DUMP=y`，`__crash_kexec()` 与 `machine_crash_shutdown()` 路径是编译进去的（`kernel/crash_core.c`），因此 kdump 能力存在而 pstore 不存在。

### 6.7 panic 之后为什么"卡住"

`vpanic()` 的最后是一个永不返回的循环：

```c
	local_irq_enable();
	for (i = 0; ; i += PANIC_TIMER_STEP) {
		touch_softlockup_watchdog();
		if (i >= i_next) {
			i += panic_blink(state ^= 1);
			i_next = i + 3600 / PANIC_BLINK_SPD;
		}
		mdelay(PANIC_TIMER_STEP);
	}
```

它故意重新打开本地中断（让 NMI watchdog、串口等仍能工作），并持续喂 softlockup watchdog，避免"panic 后又被 softlockup 报一次"。如果 `panic_timeout != 0`，这段循环根本不会到达——`emergency_restart()` 已经接管。

---

## 7. 代码位置

- `kernel/panic.c`
- `include/linux/panic.h`