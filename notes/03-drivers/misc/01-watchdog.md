# Watchdog（看门狗）子系统

看门狗是“系统失联就复位”的最后保险：独立计数器在超时前必须被周期性“喂狗”，否则拉低复位引脚
或让 PMIC 断电重启。本笔记基于本树（`Makefile:2-4` 为 7.2.0）阅读 `drivers/watchdog/` 与
`include/linux/watchdog.h`。注意 `.config:2506` 为 `# CONFIG_WATCHDOG is not set`，
下文代码路径都只存在于源码中，**当前配置未启用**。

## 1. 定位与分层

三个同名事物要分清：

| 名称 | 位置 | 作用 | 本树配置 |
|---|---|---|---|
| 硬件看门狗 | `drivers/watchdog/` | 控制 WDT，暴露 `/dev/watchdogN`，超时复位整机 | 未启用 |
| lockup detector | `kernel/watchdog.c` | hrtimer + perf/NMI 采样检测 soft/hard lockup，只打印或 panic，**不复位** | `CONFIG_SOFTLOCKUP_DETECTOR`、`CONFIG_HARDLOCKUP_DETECTOR` 未启用（`.config:3625-3627`） |
| workqueue watchdog | `kernel/workqueue.c` | 检测 workqueue stall | `CONFIG_WQ_WATCHDOG` 未启用（`.config:3629`） |

最本质的区别是“谁复位”：`drivers/watchdog/` 依赖独立硬件计数器，CPU 完全死锁也能复位；
`kernel/watchdog.c` 只是采样诊断，系统彻底挂死时它也无能为力。生产环境靠硬件看门狗。

`drivers/watchdog/Makefile:7-12` 把核心编成 `watchdog.o`（`watchdog_core.o` +
`watchdog_dev.o`），并按配置追加 `watchdog_pretimeout.o`、`watchdog_hrtimer_pretimeout.o`。

| 层 | 文件 | 职责 |
|---|---|---|
| 用户接口 | `watchdog_dev.c` | `/dev/watchdogN` 字符设备、兼容 `/dev/watchdog`（misc 10:130）、open/write/ioctl/release |
| 设备管理 | `watchdog_core.c` | 注册/注销、`ida` 分配 id、reboot/restart/pm notifier、deferred 注册 |
| 策略扩展 | `watchdog_pretimeout.c`、`watchdog_hrtimer_pretimeout.c` | 预超时 governor 与软件预超时定时器 |
| 具体驱动 | `softdog.c`、`iTCO_wdt.c`、`imx2_wdt.c`…… | 实现 `watchdog_ops`，读写硬件寄存器 |

```
 用户空间 (watchdogd / systemd / busybox watchdog)
      │ open(); write() 喂狗; ioctl(WDIOC_*); close()
      ▼
 /dev/watchdog (misc 10:130)      /dev/watchdogN (cdev, 动态主设备号)
      └──────────────┬────────────────────┘
                     ▼
      watchdog_dev.c : watchdog_fops (open/write/ioctl/release)
      hrtimer + kthread worker（内核侧代喂）
                     │ wdd->ops->start()/ping()/stop()/set_timeout()
                     ▼
      watchdog_core.c : 注册、id、notifier、deferred
                     ▼
      具体驱动 ──► 硬件 WDT 寄存器 / PMIC / GPIO ──► SoC reset
```

`watchdog_dev_init()`（`watchdog_dev.c:1223-1253`）创建 FIFO 优先级的 `watchdogd` 线程，
注册 `watchdog` class，并 `alloc_chrdev_region(&watchdog_devt, 0, MAX_DOGS, "watchdog")`。
`MAX_DOGS` 为 32（`watchdog_core.h:36`），即支持 `watchdog0..watchdog31`。

## 2. 核心数据结构

### 2.1 `watchdog_ops`

`include/linux/watchdog.h:48`，驱动回调表：

```c
struct watchdog_ops {
	struct module *owner;
	/* mandatory operations */
	int (*start)(struct watchdog_device *);
	/* optional operations */
	int (*stop)(struct watchdog_device *);
	int (*ping)(struct watchdog_device *);
	unsigned int (*status)(struct watchdog_device *);
	int (*set_timeout)(struct watchdog_device *, unsigned int);
	int (*set_pretimeout)(struct watchdog_device *, unsigned int);
	unsigned int (*get_timeleft)(struct watchdog_device *);
	int (*restart)(struct watchdog_device *, unsigned long, void *);
	long (*ioctl)(struct watchdog_device *, unsigned int, unsigned long);
};
```

`start` 绝对必需；`stop` 在声明了 `max_hw_heartbeat_ms` 时可省略（框架用 `start` 重触发即可），
校验见 `watchdog_core.c:245-250`。

### 2.2 `watchdog_device`

`include/linux/watchdog.h:102` 描述一个看门狗设备。关键字段含义：

| 字段 | 含义 |
|---|---|
| `id` | core 分配，决定 `watchdog%d` 与次设备号；0 号额外获得 `/dev/watchdog` |
| `parent` / `groups` | 父设备（sysfs、DT alias 取号）与附加属性组 |
| `info` / `ops` | 能力描述与回调表 |
| `gov` | 绑定的 pretimeout governor |
| `bootstatus` | 启动时硬件报告的复位原因（如 `WDIOF_CARDRESET`） |
| `timeout` / `pretimeout` | 逻辑超时、预超时（秒），后者必须小于前者 |
| `min_timeout`/`max_timeout` | 用户可设范围（秒） |
| `min_hw_heartbeat_ms`/`max_hw_heartbeat_ms` | 硬件最小/最大喂狗间隔（毫秒）；后者取代 `max_timeout` |
| `status` | 状态位图，见下 |
| `deferred` | 延迟注册链表节点 |

`status` 位定义（`include/linux/watchdog.h:122-128`）：

```c
/* Bit numbers for status flags */
#define WDOG_ACTIVE		0	/* Is the watchdog running/active */
#define WDOG_NO_WAY_OUT		1	/* Is 'nowayout' feature set ? */
#define WDOG_STOP_ON_REBOOT	2	/* Should be stopped on reboot */
#define WDOG_HW_RUNNING		3	/* True if HW watchdog running */
#define WDOG_STOP_ON_UNREGISTER	4	/* Should be stopped on unregister */
#define WDOG_NO_PING_ON_SUSPEND	5	/* Ping worker should be stopped on suspend */
```

操作函数在 `include/linux/watchdog.h:136-173`：`watchdog_active()`、`watchdog_hw_running()`、
`watchdog_set_nowayout()`、`watchdog_stop_on_reboot()`、`watchdog_stop_on_unregister()`、
`watchdog_stop_ping_on_suspend()`。

### 2.3 `watchdog_info` 与能力位

UAPI 结构在 `include/uapi/linux/watchdog.h:18`：`options`、`firmware_version`、`identity[32]`。
常用 `options` 位（同文件 `:40-52`）：

| 位 | 含义 |
|---|---|
| `WDIOF_SETTIMEOUT` 0x0080 | 支持 `WDIOC_SETTIMEOUT`，否则 core 返回 `-EOPNOTSUPP` |
| `WDIOF_MAGICCLOSE` 0x0100 | 写 `V` 后 close 才停 |
| `WDIOF_PRETIMEOUT` 0x0200 | 硬件支持预超时 |
| `WDIOF_KEEPALIVEPING` 0x8000 | 支持 `WDIOC_KEEPALIVE`，并在状态中回报 |
| `WDIOF_CARDRESET` 0x0020 | 上次复位由看门狗触发（bootstatus） |

`WDIOS_DISABLECARD`/`WDIOS_ENABLECARD`/`WDIOS_TEMPPANIC` 是 `WDIOC_SETOPTIONS` 的参数值，
其中 TEMPPANIC 不是 core 处理的语义。

### 2.4 `watchdog_governor` 与 `watchdog_core_data`

`struct watchdog_governor` 不在公共头，而在 `drivers/watchdog/watchdog_pretimeout.h:9-12`：
只有 `char name[20]` 和 `void (*pretimeout)(struct watchdog_device *)`。默认 governor 由
`WATCHDOG_PRETIMEOUT_DEFAULT_GOV` 宏决定（同文件 `:27-31`）。

每个设备的内部数据 `struct watchdog_core_data`（`watchdog_core.h:46-63`）包含内嵌 `device`
与 `cdev`、指回 `wdd` 的指针、串行化锁、三个时间戳（`last_keepalive`、
`last_hw_keepalive`、`open_deadline`）、`hrtimer` + `kthread_work`、可选
`pretimeout_timer`，以及三个内部位：`_WDOG_DEV_OPEN`（是否已打开）、
`_WDOG_ALLOW_RELEASE`（是否收到 `V`）、`_WDOG_KEEPALIVE`（是否收到 keepalive）。

## 3. 驱动注册流程

```
probe()
 → devm_watchdog_register_device() / watchdog_register_device()
   → __watchdog_register_device()
     → ___watchdog_register_device()
       → of_alias_get_id() / ida_alloc_max()        分配 wdd->id
       → watchdog_dev_register()
         → watchdog_cdev_register()
           → device_initialize() + dev_set_name("watchdog%d")
           → misc_register()                       仅 id==0，/dev/watchdog
           → cdev_device_add()                     /dev/watchdogN + sysfs
         → watchdog_register_pretimeout()          绑定 gov
       → register_reboot_notifier()                仅 WDOG_STOP_ON_REBOOT
       → register_restart_handler()                仅 ops->restart
       → register_pm_notifier()                    仅 WDOG_NO_PING_ON_SUSPEND
```

参数校验在 `watchdog_core.c:245-250`：`info`、`ops` 必须非空，`start` 必需，`stop` 仅在
提供了 `max_hw_heartbeat_ms` 时可省略。入口 `watchdog_register_device()`
（`watchdog_core.c:368-380`）有“延迟注册”分支：core 未就绪（`!wtd_deferred_reg_done`）时
只把设备挂到 `wtd_deferred_reg_list` 并返回 0，真正注册由
`subsys_initcall_sync(watchdog_init)` 触发 `watchdog_deferred_registration()`
（`watchdog_core.c:458-472`）。原因见 `watchdog_core.c:51-62`：部分驱动必须用很高的
initcall 级别尽早加载，但那时 misc 子系统还没就绪。

0 号设备的 misc 注册是 `/dev/watchdog` 的来源（`watchdog_dev.c:1053-1067`）；分配到的 id
若与 legacy 驱动冲突（`-EBUSY`），core 会改从 1..31 重新取号重试。其余编号通过
`cdev_device_add()` 生成 `/dev/watchdogN` 与 `/sys/class/watchdog/watchdogN`。
若注册时硬件已在跑，core 会 `__module_get()` + `get_device()`，并默认立即启动 hrtimer
代喂（`watchdog_dev.c:1090-1103`，受 `CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED` 控制）。

`devm_watchdog_register_device()`（`watchdog_core.c:435-455`）只是把注销挂进 devres。
注销严格反向：

```
watchdog_unregister_device() → __watchdog_unregister_device()
 → unregister_restart_handler() → unregister_reboot_notifier() → unregister_pm_notifier()
 → watchdog_dev_unregister() → watchdog_unregister_pretimeout() → watchdog_cdev_unregister()
   → cdev_device_del() → misc_deregister()（id 0）
   → 若 active && WDOG_STOP_ON_UNREGISTER → watchdog_stop()
   → 取消 hrtimer 与 work
 → ida_free()
```

## 4. 用户空间接口

### 4.1 fops、open 与单次打开

`watchdog_fops`（`watchdog_dev.c:987-994`）**没有 `.read`**：

```c
static const struct file_operations watchdog_fops = {
	.owner		= THIS_MODULE,
	.write		= watchdog_write,
	.unlocked_ioctl	= watchdog_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.open		= watchdog_open,
	.release	= watchdog_release,
};
```

`watchdog_open()`（`watchdog_dev.c:859-915`）用内部位互斥，第二次 open 返回 `-EBUSY`：

```c
	/* the watchdog is single open! */
	if (test_and_set_bit(_WDOG_DEV_OPEN, &wd_data->status))
		return -EBUSY;
```

之后：硬件未跑则 `try_module_get()` 防卸载；`watchdog_start()` 启动；`get_device()`；
`open_deadline` 置 `KTIME_MAX`（内核不再按开机期限代喂）；`stream_open()` 声明不可 seek。
0 号设备走 `old_wd_data`，其余用 `container_of(inode->i_cdev, ...)` 取私有数据。

### 4.2 write：写即喂狗

`watchdog_write()`（`watchdog_dev.c:694-733`）逐字节扫描 `V` 标记，然后 ping：

```c
	/* scan to see whether or not we got the magic character */
	for (i = 0; i != len; i++) {
		if (get_user(c, data + i))
			return -EFAULT;
		if (c == 'V')
			set_bit(_WDOG_ALLOW_RELEASE, &wd_data->status);
	}

	/* someone wrote to us, so we send the watchdog a keepalive ping */
```

任意非空写入都等价于 `WDIOC_KEEPALIVE`；`V` 只是给随后 close 一个“允许停机”的许可。

### 4.3 release：magic close 与 nowayout

`watchdog_release()`（`watchdog_dev.c:937-985`）只在收到 `V`、或驱动不支持
`WDIOF_MAGICCLOSE` 时才真正停止；否则打一条
`pr_crit("watchdog%d: watchdog did not stop!")` 并补一次 ping 让狗继续跑。
关闭语义：写 `V` 后 close 且无 nowayout → 停；未写 `V` 且支持 MAGICCLOSE → 继续跑；
不支持 MAGICCLOSE → close 即尝试停；nowayout → `watchdog_stop()` 返回 `-EBUSY`。
无论是否停成功都会清 `_WDOG_DEV_OPEN` 允许重新打开，只有硬件确实停了才
`module_put()` + `put_device()`。

### 4.4 ioctl 全表

`watchdog_ioctl()`（`watchdog_dev.c:747-846`）先给驱动 `ops->ioctl` 机会，返回非
`-ENOIOCTLCMD` 就直接返回；否则进核心 switch：

| 命令 | 参数 | core 行为 |
|---|---|---|
| `WDIOC_GETSUPPORT` | 输出 `watchdog_info` | `copy_to_user(wdd->info)` |
| `WDIOC_GETSTATUS` | 输出 `int` | `ops->status()`，否则由 `bootstatus` 掩码 + MAGICCLOSE + 粘滞 KEEPALIVEPING + 可选 PRETIMEOUT 组成 |
| `WDIOC_GETBOOTSTATUS` | 输出 `int` | 直接返回 `wdd->bootstatus` |
| `WDIOC_SETOPTIONS` | 输入 `int` | `WDIOS_DISABLECARD`→stop；`WDIOS_ENABLECARD`→start |
| `WDIOC_KEEPALIVE` | 忽略 | 要求 `WDIOF_KEEPALIVEPING`，否则 `-EOPNOTSUPP`；成功即一次 ping |
| `WDIOC_SETTIMEOUT` | 输入/输出 `int` | 设超时 → ping → `fallthrough` 到 GETTIMEOUT 回填实际值 |
| `WDIOC_GETTIMEOUT` | 输出 `int` | `timeout==0` 表示未知，返回 `-EOPNOTSUPP` |
| `WDIOC_GETTIMELEFT` | 输出 `int` | `ops->get_timeleft()`，否则按 `last_keepalive` 推算 |
| `WDIOC_SETPRETIMEOUT` | 输入 `int` | 需支持 pretimeout，越界 `-EINVAL` |
| `WDIOC_GETPRETIMEOUT` | 输出 `int` | 返回 `wdd->pretimeout` |
| `WDIOC_GETTEMP` | — | core **未实现**，落到 `default: -ENOTTY`；仅 pcwd/wdrtas 等在 `ops->ioctl` 里支持 |

`WDIOS_TEMPPANIC` 同属驱动私有语义。`WDIOC_SETTIMEOUT` 的 `fallthrough` 让设置成功后
顺便把驱动真正接受的值回填给用户。本树 fops 无 `.read`，`read(2)` 返回 `-EINVAL`，
观测请用 `WDIOC_GETSTATUS` 或 sysfs——老资料“read 返回状态”的说法不适用。

## 5. 喂狗与超时

```
write() / WDIOC_KEEPALIVE / WDIOC_SETTIMEOUT
 → watchdog_ping()            仅在 WDOG_HW_RUNNING 时继续，记录 last_keepalive
   → __watchdog_ping()
     → 受 min_hw_heartbeat_ms 节流（必要时只启动 hrtimer 稍后再发）
     → ops->ping()，未实现则回退 ops->start()
     → watchdog_hrtimer_pretimeout_start() → watchdog_update_worker()
```

`__watchdog_ping()`（`watchdog_dev.c:144-177`）用 `HRTIMER_MODE_REL_HARD` 排下一次心跳，
并在驱动无 `ping` 时调用 `start` 重触发——这是很多老驱动只实现 `start` 仍能工作的原因。

有些硬件单次最大间隔很短，而用户想要 60 秒超时，框架用 hrtimer + `watchdogd` 线程代喂，
把逻辑超时与硬件心跳解耦。`watchdog_need_worker()`（`watchdog_dev.c:76-97`）：

```c
static inline bool watchdog_need_worker(struct watchdog_device *wdd)
{
	/* All variables in milli-seconds */
	unsigned int hm = wdd->max_hw_heartbeat_ms;
	unsigned int t = wdd->timeout * 1000;

	/*
	 * A worker to generate heartbeat requests is needed if all of the
	 * following conditions are true.
	 * - Userspace activated the watchdog.
	 * - The driver provided a value for the maximum hardware timeout, and
	 *   thus is aware that the framework supports generating heartbeat
	 *   requests.
	 * - Userspace requests a longer timeout than the hardware can handle.
	 *
	 * Alternatively, if userspace has not opened the watchdog
	 * device, we take care of feeding the watchdog if it is
	 * running.
	 */
	return (hm && watchdog_active(wdd) && t > hm) ||
		(t && !watchdog_active(wdd) && watchdog_hw_running(wdd));
}
```

两个场景：用户超时大于硬件上限（拆多次心跳），或用户尚未接管但硬件已在跑（开机代喂）。
后者受 `open_deadline` 约束：注册时按 `CONFIG_WATCHDOG_OPEN_TIMEOUT`（默认 0=无限）设定；
用户 `open()` 后改为 `KTIME_MAX` 表示接管；若一直无人 open，
`watchdog_worker_should_ping()` 返回 false，内核停止代喂，硬件复位。硬件已跑很久、
驱动无法立刻 ping 时，可调 `watchdog_set_last_hw_keepalive(wdd, last_ping_ms)`
（`watchdog_dev.c:1194-1214`）校正起点。

| 字段 | 单位 | 用途 |
|---|---|---|
| `timeout` | 秒 | 用户可见逻辑超时 |
| `min_timeout`/`max_timeout` | 秒 | `WDIOC_SETTIMEOUT` 合法区间 |
| `min_hw_heartbeat_ms` | 毫秒 | 硬件最小间隔，节流 ping |
| `max_hw_heartbeat_ms` | 毫秒 | 硬件单次最大间隔，设置后忽略 `max_timeout` |

合法性由 `watchdog_timeout_invalid()`（`include/linux/watchdog.h:176-193`）判定：
超过 `UINT_MAX/1000`、小于 `min_timeout`、或超过 `max_timeout` 都非法。
`watchdog_set_timeout()`（`watchdog_dev.c:368-392`）要求 `WDIOF_SETTIMEOUT`；
驱动有 `ops->set_timeout` 就交给硬件，否则只更新 `wdd->timeout`，并在
`pretimeout >= timeout` 时清零 pretimeout。

## 6. pretimeout 机制

`watchdog_have_pretimeout()`（`watchdog_core.h:73-77`）的判定是
`info->options & WDIOF_PRETIMEOUT` **或** `CONFIG_WATCHDOG_HRTIMER_PRETIMEOUT` 已启用。

| 路径 | 触发者 | 条件 |
|---|---|---|
| 硬件预超时 | 驱动 `ops->set_pretimeout()` | 需 `WDIOF_PRETIMEOUT` |
| 软件预超时 | `watchdog_hrtimer_pretimeout.c` | 需 `CONFIG_WATCHDOG_HRTIMER_PRETIMEOUT`，且驱动无硬件预超时 |

软件路径在每次 ping 后重排定时器，触发时刻为 `timeout - pretimeout`
（`watchdog_hrtimer_pretimeout.c:30-39`）。

governor 框架在 `watchdog_pretimeout.c`：`governor_list` 存已注册策略，
`pretimeout_list` 存支持预超时的设备，`default_gov` 为默认策略。绑定流程：

```
governor init → watchdog_register_governor()
  若名字 == WATCHDOG_PRETIMEOUT_DEFAULT_GOV → default_gov = gov，并回填所有无 gov 的设备
设备注册 → watchdog_register_pretimeout() → wdd->gov = default_gov
```

事件上报只有一条路（`watchdog_pretimeout.c:102-115`）：

```c
void watchdog_notify_pretimeout(struct watchdog_device *wdd)
{
	unsigned long flags;

	spin_lock_irqsave(&pretimeout_lock, flags);
	if (!wdd->gov) {
		spin_unlock_irqrestore(&pretimeout_lock, flags);
		return;
	}

	wdd->gov->pretimeout(wdd);
	spin_unlock_irqrestore(&pretimeout_lock, flags);
}
```

若 `CONFIG_WATCHDOG_PRETIMEOUT_GOV` 未启用，头文件 `:214-221` 提供内联退化版，只打印
`"watchdog%d: pretimeout event\n"`。内置 governor 两个：`pretimeout_noop.c` 只
`pr_alert()`；`pretimeout_panic.c` 直接 panic。Kconfig 的默认 choice 是 panic
（`Kconfig:116-118`）。

sysfs 侧仅当设备支持预超时且 governor 启用时可见（`watchdog_dev.c:619-634`）：
`pretimeout`（只读）、`pretimeout_available_governors`（只读）、
`pretimeout_governor`（读写，写入不存在的名字返回 `-EINVAL`）。切换 governor 只改
`wdd->gov`，不改硬件；`WDIOC_SETPRETIMEOUT` 才改实际预超时值。

## 7. 启动、停止与 nowayout

| 位 | 置位者 | 清除者 | 含义 |
|---|---|---|---|
| `WDOG_ACTIVE` | `watchdog_start()` | `watchdog_stop()` | 逻辑上已启用 |
| `WDOG_HW_RUNNING` | start 成功 / stop 时无 `stop` 回调 | stop 且有 `stop` | 硬件确实在跑 |
| `WDOG_NO_WAY_OUT` | `watchdog_set_nowayout()`、sysfs | 不可清除 | nowayout 生效 |
| `WDOG_STOP_ON_REBOOT` | 驱动或 core 模块参数 | core 模块参数 | 重启前停狗 |
| `WDOG_STOP_ON_UNREGISTER` | 驱动 | — | 注销时停狗 |
| `WDOG_NO_PING_ON_SUSPEND` | 驱动 | — | 挂起时停 worker |

`watchdog_start()`（`watchdog_dev.c:248-280`）：若 `WDOG_HW_RUNNING` 且驱动有 `ping`，
只 ping 一下再置 ACTIVE，不重新编程硬件；否则调 `ops->start()` 并置 ACTIVE/HW_RUNNING，
重置两个时间戳。`watchdog_stop()`（`:292-320`）遇到 nowayout 立即返回：

```c
	if (test_bit(WDOG_NO_WAY_OUT, &wdd->status)) {
		pr_info("watchdog%d: nowayout prevents watchdog being stopped!\n",
			wdd->id);
		return -EBUSY;
	}
```

驱动没有 `stop` 时，它只能把 `WDOG_HW_RUNNING` 重新置位——表示“停不掉，硬件继续跑”。

- **reboot**：仅 `WDOG_STOP_ON_REBOOT` 且驱动有 `stop` 时注册
  `watchdog_reboot_notifier()`（`watchdog_core.c:159-177`），只处理 `SYS_DOWN`/
  `SYS_HALT`/`SYS_POWER_OFF`，且 `watchdog_hw_running()` 时调 `ops->stop()`；失败返回
  `NOTIFY_BAD` 中止重启。core 模块参数 `stop_on_reboot`（默认 `-1` 不干预）可强制覆盖
  所有驱动的策略（`:295-301`）。
- **restart**：驱动有 `ops->restart` 时注册 restart handler，优先级由
  `watchdog_set_restart_priority()` 设置，建议 0/128/255（`:221-239`）。
- **pm**：仅 `WDOG_NO_PING_ON_SUSPEND` 时注册。挂起前 `watchdog_dev_suspend()` 最后
  ping 一次并取消 hrtimer/work；恢复时 `watchdog_dev_resume()` 重新 ping 恢复 worker
  （`watchdog_dev.c:1267-1312`）。

注销时的 e-stop 在 `watchdog_cdev_unregister()` 中：若 `watchdog_active()` 且
`WDOG_STOP_ON_UNREGISTER`，调 `watchdog_stop()`，避免“驱动走了狗还在跑，几十秒后莫名
重启”；nowayout 下这步同样会失败。模块引用有三重保护：`open()` 时硬件未跑则
`try_module_get()`；注册时硬件在跑则 `__module_get()` + `get_device()`；
`release()` 只有硬件确实停了才 `module_put()` + `put_device()`。因此“狗在跑”时驱动
无法卸载。状态迁移可概括为：open 或 `WDIOS_ENABLECARD` 置 ACTIVE+HW_RUNNING；
close+magic 或 `WDIOS_DISABLECARD` 且 stop 成功则两者同清；若驱动无 `stop` 回调，
ACTIVE 清零而 HW_RUNNING 保持，内核 worker 继续代喂。

## 8. softdog 与常见硬件驱动

`softdog.c` 用 hrtimer 模拟硬件看门狗，`soft_margin` 默认 60 秒（`:31-36`）。
它的 `watchdog_info.options` 为 `WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
WDIOF_MAGICCLOSE`，`ops` 为 `.owner = THIS_MODULE, .start = softdog_ping,
.stop = softdog_stop`——**没有 `.ping`**：`start` 就是 `softdog_ping()`，于是框架 ping 时
走 `__watchdog_ping()` 的 else 分支重复调 `start`，是“ping 回退到 start”的活例子。
`softdog_ping()`（`:133-150`）在定时器未激活时 `__module_get(THIS_MODULE)`（之后由
hrtimer 回调驱动，模块必须常驻），再按 `w->timeout` 重启 `softdog_ticktock`；启用
`CONFIG_SOFT_WATCHDOG_PRETIMEOUT` 时还按 `timeout - pretimeout` 启动独立预超时定时器。

超时回调 `softdog_fire()`（`:78-122`）的行为：

| 参数 | 默认 | 触发时行为 |
|---|---|---|
| `soft_noboot` | 0 | 为 1 只打印 `Triggered - Reboot ignored` |
| `soft_panic` | 0 | 为 1 则 `panic("Software Watchdog Timer expired")` |
| `soft_reboot_cmd` | 未设置 | 非空时在独立 kthread 中 `kernel_restart(cmd)`，并把 timer 再推 60s 兜底 |
| `soft_active_on_boot` | false | 注册前先 ping 一次，开机即运行 |

`nowayout` 默认取 `WATCHDOG_NOWAYOUT`（`:38-42`），初始化时
`watchdog_set_nowayout()` + `watchdog_stop_on_reboot()`（`:186-188`）。softdog 的局限：
计时依赖 CPU 与中断，内核关中断死循环或硬件彻底挂死时 hrtimer 不会触发，也就不会复位；
它只适合开发测试或无硬件 WDT 时的兜底，不能替代真正的硬件看门狗。

`drivers/watchdog/Makefile:17-23` 注释说明传统探测顺序：ISA/PCI/USB 扩展卡 → 架构专用
驱动 → softdog。今天多数 SoC 通过设备树匹配，`of_alias_get_id()` 让编号与 DT alias 一致。
本树 `drivers/watchdog/` 共 192 个 `.c` 文件（含核心与 governor），常见代表：

| 驱动 | 适用场景 |
|---|---|
| `softdog.c` | 任意平台，纯软件 |
| `iTCO_wdt.c`、`sp5100_tco.c` | Intel/AMD 芯片组 TCO |
| `imx2_wdt.c`、`dw_wdt.c` | i.MX / DesignWare |
| `qcom-wdt.c`、`arm_smc_wdt.c`、`sbsa_gwdt.c` | Qualcomm / ARM SMC / SBSA |
| `gpio_wdt.c`、`wdat_wdt.c` | 外部 GPIO 喂狗 / ACPI WDAT |
| `cros_ec_wdt.c`、`bd9576_wdt.c` | EC / PMIC 内置看门狗 |

多个驱动可同时注册，但只有 id 0 拥有 `/dev/watchdog`，其余只能走 `/dev/watchdogN`。

## 9. 调试与观测

`/sys/class/watchdog/watchdogN/` 的属性由 `wdt_attrs[]`（`watchdog_dev.c:635-651`）定义，
受 `CONFIG_WATCHDOG_SYSFS` 控制：

| 属性 | 含义 |
|---|---|
| `state` | `active` / `inactive`（WDOG_ACTIVE） |
| `identity`、`options`、`fw_version` | 驱动名、能力位、固件版本 |
| `timeout`、`min_timeout`、`max_timeout` | 超时配置（秒） |
| `pretimeout` | 预超时（不支持时隐藏） |
| `timeleft` | 距复位剩余秒数（无 `get_timeleft` 时按 `last_keepalive` 估算） |
| `bootstatus`、`status` | 复位原因、当前状态（KEEPALIVEPING 读后清零） |
| `nowayout` | 可写，但只能 0→1，反向写入返回 `-EPERM` |
| `pretimeout_governor`、`pretimeout_available_governors` | 预超时策略（不满足条件时隐藏） |

常见日志与 trace：

| 输出 | 出处 | 含义 |
|---|---|---|
| `nowayout prevents watchdog being stopped!` | `watchdog_dev.c:300` | nowayout 下试图停狗 |
| `watchdog%d: watchdog did not stop!` | `watchdog_dev.c:963` | close 未 magic close 或 nowayout |
| `running and kernel based pre-userspace handler disabled` | `watchdog_dev.c:1101` | 关闭了开机代喂 |
| `initialized. soft_noboot=... soft_margin=...` | `softdog.c:205` | softdog 加载成功 |

`include/trace/events/watchdog.h` 提供 `watchdog_start`、`watchdog_ping`、
`watchdog_stop`、`watchdog_set_timeout` 四个 tracepoint，可从
`/sys/kernel/tracing/events/watchdog/` 打开，用于分辨“驱动没喂”还是“框架没调”。

常见误用：① 喂狗线程被 OOM/调度饿死导致复位，日志来不及落盘，建议配合 pstore（见
[panic 调试](../../10-debug/28-panic_debug.md)）；② nowayout 一旦生效无法关闭，
`DISABLECARD`、close、驱动卸载都可能停不掉，只能复位；③ 未写 `V` 就 close，
支持 MAGICCLOSE 的驱动会继续跑，程序退出后系统照样重启；④ 多个进程想共享设备，
第二次 open 返回 `-EBUSY`；⑤ 用 `read()` 取状态（本树无 `.read`，返回 `-EINVAL`）；
⑥ 把 lockup detector 当看门狗用，它不会复位机器。

## 10. 配置项

`.config:2506` 为 `# CONFIG_WATCHDOG is not set`，下列选项**源码存在，当前配置未启用**：

| 配置项 | 类型/默认 | 作用 |
|---|---|---|
| `CONFIG_WATCHDOG` | menuconfig | 总开关，关闭则整个目录不编译 |
| `CONFIG_WATCHDOG_CORE` | tristate | 框架本体（`watchdog_core.o` + `watchdog_dev.o`） |
| `CONFIG_WATCHDOG_NOWAYOUT` | bool，默认 n | 启动后不可停止，驱动用 `WATCHDOG_NOWAYOUT` 宏读取 |
| `CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED` | bool，默认 y | 用户空间接管前由内核代喂已运行的狗 |
| `CONFIG_WATCHDOG_OPEN_TIMEOUT` | int，默认 0 | 接管期限（秒），0 表示无限 |
| `CONFIG_WATCHDOG_SYSFS` | bool | 创建第 9 节的 sysfs 属性 |
| `CONFIG_WATCHDOG_HRTIMER_PRETIMEOUT` | bool | 为无硬件预超时的设备提供 hrtimer 版 pretimeout |
| `CONFIG_WATCHDOG_PRETIMEOUT_GOV` | bool | 启用 governor 框架 |
| `CONFIG_WATCHDOG_PRETIMEOUT_GOV_NOOP` / `_PANIC` | tristate | noop / panic 两个内置策略 |
| `CONFIG_WATCHDOG_PRETIMEOUT_DEFAULT_GOV_NOOP` / `_PANIC` | choice | 默认策略，choice 默认 panic |
| `CONFIG_SOFT_WATCHDOG` / `_PRETIMEOUT` | tristate / bool | softdog 及其预超时定时器 |

模块参数（`watchdog_core.c:47-49`、`watchdog_dev.c:1314-1322`）：

| 参数 | 权限 | 含义 |
|---|---|---|
| `watchdog.stop_on_reboot` | 0444 | `-1` 不干预、`0` 保持喂、`1` 重启前停 |
| `watchdog.handle_boot_enabled` | 0444 | 是否内核代喂开机已运行的狗 |
| `watchdog.open_timeout` | 0644 | 覆盖 `CONFIG_WATCHDOG_OPEN_TIMEOUT`，0 表示无限 |

## 11. 关键文件清单

| 文件 | 作用 |
|---|---|
| `include/linux/watchdog.h` | `watchdog_device`、`watchdog_ops`、状态位与辅助函数 |
| `include/uapi/linux/watchdog.h` | `watchdog_info`、`WDIOC_*`、`WDIOF_*`、`WDIOS_*` |
| `drivers/watchdog/watchdog_core.c` | 注册/注销、id 分配、notifier、deferred 注册 |
| `drivers/watchdog/watchdog_dev.c` | 字符设备、open/write/ioctl/release、ping worker、sysfs |
| `drivers/watchdog/watchdog_core.h` | `watchdog_core_data`、`MAX_DOGS`、内部状态位 |
| `drivers/watchdog/watchdog_pretimeout.c` | governor 注册与默认策略、pretimeout 设备链表 |
| `drivers/watchdog/watchdog_pretimeout.h` | `struct watchdog_governor` 与默认 gov 宏 |
| `drivers/watchdog/watchdog_hrtimer_pretimeout.c` | 软件预超时 hrtimer |
| `drivers/watchdog/pretimeout_noop.c` / `pretimeout_panic.c` | 两个内置 governor |
| `drivers/watchdog/softdog.c` | 纯软件看门狗 |
| `drivers/watchdog/Kconfig` / `Makefile` | 配置项与 192 个驱动的编译规则 |
| `include/trace/events/watchdog.h` | start/ping/stop/set_timeout tracepoint |

## 12. 延伸阅读

- 源码文档：`Documentation/watchdog/watchdog-api.rst`、`watchdog-kernel-api.rst`、
  `watchdog-parameters.rst`、`watchdog-pm.rst`；最小示例 `samples/watchdog/watchdog-simple.c`。
- 本树相关章节：[子系统全景（第 10 节 RTC/Watchdog/LED）](../01-subsystem-panorama.md)、
  [设备驱动模型](../driver-model/01-driver-model.md)、
  [panic 与崩溃诊断](../../10-debug/28-panic_debug.md)。
- 与 lockup detector 的对照见 `kernel/watchdog.c` 的 `watchdog_thresh`、`nmi_watchdog=`、
  `/proc/sys/kernel/watchdog`：它负责“发现问题”，`drivers/watchdog/` 负责“复位恢复”，
  两者互补而非替代。
