# RTC 子系统：rtc-class、/dev/rtcN、闹钟唤醒与 NTP 回写

> 基于本仓库 Linux 7.2 源码逐文件核对，`文件:行号` 均可直接定位；代码块为真实源码摘录，省略处用 `...` 标明。
>
> 配置提醒：`01-subsystem-panorama.md:344` 写“RTC 未启用”，但本树 `.config:2699-2713` 实际为 `CONFIG_RTC_CLASS=y`、`RTC_HCTOSYS=y`、`RTC_SYSTOHC=y`、`RTC_NVMEM=y`，`RTC_INTF_{SYSFS,PROC,DEV}` 也全为 `y`；仅 `RTC_DEBUG`、`RTC_INTF_DEV_UIE_EMUL`、`RTC_DRV_TEST` 未开启。本笔记以源码为准，差异见第 10 节。

## 1. 定位与分层

### 1.1 RTC 是什么

RTC 是**由纽扣电池/超级电容独立供电的日历时钟**，主电源断开后仍走时。它不提供高精度时间，职责是**在关机期间保存墙上时间**，并在开机、唤醒、闹钟时刻把信息交给系统。`Documentation/admin-guide/rtc.rst:4-12` 的定义是：跟踪墙上时间、带电池、通常存 UTC 而不存时区。

| 硬件能力 | 典型实现 | 内核入口 | 特性位 |
|---|---|---|---|
| 计时 | BCD 日历或 32 位秒计数器 | `read_time`/`set_time` | — |
| 一次性闹钟 | 匹配时分秒或日/月/年 | `read_alarm`/`set_alarm` | `RTC_FEATURE_ALARM` |
| 唤醒 | 闹钟引脚接 PMIC/SoC wakeup | `device_init_wakeup()`、`wakealarm` | `RTC_FEATURE_ALARM` |
| 更新/周期中断 | 1Hz 进位中断或 2^N Hz 分频方波 | UIE/PIE | `RTC_FEATURE_UPDATE_INTERRUPT` |
| 频率校准 | 调整 32768Hz 晶振的秒长 | `read_offset`/`set_offset` | `RTC_FEATURE_CORRECTION` |
| 后备存储 | 电池域 SRAM/EEPROM | `devm_rtc_nvmem_register()` | — |
| 备份电源切换 | 主电/备电策略 | `param_get/param_set` | `RTC_FEATURE_BACKUP_SWITCH_MODE` |

### 1.2 与系统时间的关系

运行期 `CLOCK_REALTIME` 由 timekeeping 用 clocksource 推进，**不会每次读时间都访问 RTC**。RTC 只在三处参与：

```text
 断电/休眠          开机                运行期                NTP 同步后
   │                 │                   │                      │
   ▼                 ▼                   ▼                      ▼
 [RTC 走时] ─hctosys→ timekeeping ─clocksource→ 系统时间 ─systohc→ [RTC]
```

- **hctosys**：启动/resume 时把 RTC 时间灌入系统时钟（第 8 节）；
- **systohc**：NTP 判定同步后约每 11 分钟回写 RTC（第 7 节）；
- 用户态 `hwclock`、`timedatectl` 最终也走这两条路径。

`struct rtc_time` 是日历字段，`time64_t` 是 1970 起秒数。RTC 通常只存整秒，故 hctosys 把 `tv_nsec` 设为 `NSEC_PER_SEC >> 1`（`class.c:62-64`），即“截断整秒 + 0.5s”的最佳猜测。

### 1.3 内核中的三层

```text
用户态   /dev/rtcN (ioctl/read/poll)  /sys/class/rtc/rtcN  /proc/driver/rtc
   │
接口层   dev.c  sysfs.c  proc.c  nvmem.c
   │
核心层   class.c（设备/class/PM/hctosys）  interface.c（策略/定时器）
   │  struct rtc_class_ops
驱动层   rtc-hym8563.c / rtc-ds1307.c / rtc-cmos.c ...
   │  I2C/SPI/MMIO
RTC 硬件（电池域）
```

| 文件 | 行数 | 职责 |
|---|---|---|
| `drivers/rtc/class.c` / `interface.c` | 491 / 1159 | 设备/class/PM/hctosys；对内 API、timerqueue、AIE/UIE/PIE、offset |
| `drivers/rtc/dev.c` | 576 | `/dev/rtcN`、ioctl、read/poll/fasync |
| `drivers/rtc/sysfs.c` | 351 | `/sys/class/rtc/rtcN` 属性 |
| `drivers/rtc/proc.c` | 92 | `/proc/driver/rtc`（仅 hctosys 设备） |
| `drivers/rtc/nvmem.c` | 31 | 后备存储注册为 nvmem |
| `drivers/rtc/lib.c` | 211 | 日历/秒数互转、`rtc_valid_tm()` |

`drivers/rtc/` 共 **205 个 `.c`**，`Kconfig` 有 192 个 `RTC_DRV_*`，本树启用 19 个 `=y`（第 10 节）。

## 2. 核心数据结构

### 2.1 `struct rtc_device`

`include/linux/rtc.h:87-164`，内嵌 `struct device`，天然挂在 `rtc` class 上：

```c
struct rtc_device {
	struct device dev;
	struct module *owner;
	int id;
	const struct rtc_class_ops *ops;
	struct mutex ops_lock;
	struct cdev char_dev;
	unsigned long flags;
	unsigned long irq_data;
	spinlock_t irq_lock;
	wait_queue_head_t irq_queue;
	struct fasync_struct *async_queue;
	int irq_freq;
	int max_user_freq;
	struct timerqueue_head timerqueue;
	struct rtc_timer aie_timer;
	struct rtc_timer uie_rtctimer;
	struct hrtimer pie_timer; /* sub second exp, so needs hrtimer */
	int pie_enabled;
	struct work_struct irqwork;
	...
};
```

| 字段组 | 含义 |
|---|---|
| `dev`/`owner`/`id` | 设备节点、模块引用、实例号（决定 `rtc%d` 与次设备号） |
| `ops`/`ops_lock` | 驱动回调表；硬件访问被 `ops_lock` 串行化（`rtc_lock()`，`rtc.h:167`） |
| `char_dev`/`flags`/`irq_data`/`irq_lock`/`irq_queue`/`async_queue` | cdev 与 `RTC_NO_CDEV`；中断事件字（低字节标志、高位计数）与 read/poll/fasync 等待队列 |
| `irq_freq`/`max_user_freq` | PIE 频率与用户上限，默认 1 / 64 |
| `timerqueue`/`aie_timer`/`uie_rtctimer`/`pie_timer`/`irqwork` | 软件定时器队列与闹钟/更新中断软实现（第 6 节） |
| `set_offset_nsec`/`features[]`/`range_*`/`alarm_offset_max`/`start_secs`/`offset_secs` | 写对齐偏移、能力位、时间范围、闹钟上限、范围映射（2.4/2.5） |

注：`rtc.h:135-137` 注释写默认“10 msec”，实际赋值 `class.c:223` 是 `5 * NSEC_PER_MSEC`，以代码为准。

### 2.2 `struct rtc_class_ops`

`include/linux/rtc.h:59-71`，驱动只需实现硬件相关回调：

```c
struct rtc_class_ops {
	int (*ioctl)(struct device *, unsigned int, unsigned long);
	int (*read_time)(struct device *, struct rtc_time *);
	int (*set_time)(struct device *, struct rtc_time *);
	int (*read_alarm)(struct device *, struct rtc_wkalrm *);
	int (*set_alarm)(struct device *, struct rtc_wkalrm *);
	int (*proc)(struct device *, struct seq_file *);
	int (*alarm_irq_enable)(struct device *, unsigned int enabled);
	int (*read_offset)(struct device *, long *offset);
	int (*set_offset)(struct device *, long offset);
	int (*param_get)(struct device *, struct rtc_param *param);
	int (*param_set)(struct device *, struct rtc_param *param);
};
```

| 回调 | 说明 |
|---|---|
| `read_time`/`set_time` | 读写日历；参数是**父设备**（i2c_client/platform_device），不是 `rtc_device` |
| `read_alarm`/`set_alarm`/`alarm_irq_enable` | 闹钟读写与中断开关；`set_alarm` 缺失会在注册时清 `RTC_FEATURE_ALARM` |
| `read_offset`/`set_offset` | ppb 校准，可选 |
| `param_get/param_set`、`proc`/`ioctl` | 能力查询/配置；追加 proc 信息；接收核心未处理的老命令（PLL/VL/WIE/EPOCH） |

### 2.3 用户 ABI 结构

`include/uapi/linux/rtc.h` 定义 `struct rtc_time`（9 个 `int` 字段）与 `struct rtc_wkalrm`：

```c
struct rtc_wkalrm {
	unsigned char enabled;	/* 0 = alarm disabled, 1 = alarm enabled */
	unsigned char pending;  /* 0 = alarm not pending, 1 = alarm pending */
	struct rtc_time time;	/* time the alarm is set to */
};
```

- `tm_year` 相对 1900，`tm_mon` 为 0-11；`rtc_valid_tm()` 要求 `tm_year >= 70`。
- `rtc_wkalrm.enabled` 让 `RTC_WKALM_SET` 一次完成“设时间 + 使能”，避免 `RTC_ALM_SET` 后再 `RTC_AIE_ON` 的竞态（`dev.c:289-301`）。
- `struct rtc_param`（`uapi:70-79`）用 `param` 选 `RTC_PARAM_FEATURES/CORRECTION/BACKUP_SWITCH_MODE`；`struct rtc_pll_info`（`uapi:60-68`）是老式 PLL 校准 ABI，核心不解析，直接透传驱动。

### 2.4 特性位

`uapi/linux/rtc.h:129-137`，驱动在 probe 中 `set_bit()`，核心在注册时补默认值：

| 位 | 值 | 含义 | 本树设置点 |
|---|---|---|---|
| `RTC_FEATURE_ALARM` | 0 | 支持闹钟 | `class.c:246` 默认置位，无 `set_alarm` 时清除（`class.c:402-403`） |
| `RTC_FEATURE_ALARM_RES_MINUTE` | 1 | 闹钟精度只到分钟 | 驱动设置，如 `rtc-hym8563.c:556` |
| `RTC_FEATURE_NEED_WEEK_DAY` / `RTC_FEATURE_ALARM_RES_2S` | 2/3 | 设闹钟需 `tm_wday` / 精度 2 秒 | 驱动设置 |
| `RTC_FEATURE_UPDATE_INTERRUPT` | 4 | 硬件 1Hz 更新中断 | `class.c:247` 默认置位 |
| `RTC_FEATURE_CORRECTION` | 5 | 支持 offset 校准 | `class.c:405-406` 依 `set_offset` 置位 |
| `RTC_FEATURE_BACKUP_SWITCH_MODE` | 6 | 支持备份电源切换 | 如 `rtc-rv3028.c:1012` |
| `RTC_FEATURE_ALARM_WAKEUP_ONLY` | 7 | 闹钟只能用于唤醒 | 驱动设置 |

`RTC_PARAM_FEATURES` 把该位图原样返回用户态（`dev.c:403-407`）。

### 2.5 范围扩展：`offset_secs`

很多 RTC 只能表示 2000-2099 或 1970-2069。设备树给 `start-year` 时，`rtc_device_get_offset()`（`class.c:273-338`）计算：

```text
range_secs = range_max - range_min + 1
offset_secs = start_secs - range_min   （start 在范围外）
            = range_secs               （start > range_min，跳过重叠区）
            = -range_secs              （start < range_min）
            = 0                        （恰好对齐）
```

读时 `rtc_add_offset()`、写时 `rtc_subtract_offset()`（`interface.c:23-65`）只在“硬件原始范围之外”加/减偏移，使 100 年量程芯片表现出更宽逻辑范围。`/sys/.../range` 显示的是**硬件原始** `[range_min, range_max]`（`sysfs.c:242-248`）。

## 3. 注册流程

### 3.1 典型驱动写法

本树启用的 HYM8563（I2C）示例（`rtc-hym8563.c:512-563`，节选）：

```c
hym8563->rtc = devm_rtc_allocate_device(&client->dev);
if (client->irq > 0 ||
    device_property_read_bool(&client->dev, "wakeup-source")) {
	device_init_wakeup(&client->dev, true);
}
hym8563->rtc->ops = &hym8563_rtc_ops;
set_bit(RTC_FEATURE_ALARM_RES_MINUTE, hym8563->rtc->features);
clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, hym8563->rtc->features);
...
return devm_rtc_register_device(hym8563->rtc);
```

固定三段式：**分配 → 填 `ops`/`features` → 注册**。`devm_rtc_register_device` 是宏，展开为 `__devm_rtc_register_device(THIS_MODULE, rtc)`（`rtc.h:248-249`）。最小模板见 `drivers/rtc/rtc-test.c:115-142`（软件定时器模拟硬件，本树未启用）。

### 3.2 ID 分配与命名

`devm_rtc_allocate_device()`（`class.c:363-390`）先调用 `rtc_device_get_id()`（`class.c:252-271`）：有 `of_node` 时读 `of_alias_get_id(np, "rtc")`（否则看父节点）；有别名则 `ida_alloc_range()` 精确占位，失败仅告警；否则 `ida_alloc()` 取最小空闲号。随后 `dev_set_name(&rtc->dev, "rtc%d", id)`，并挂 `put_device` 的 devm 动作。ID 同时决定 `/sys/class/rtc/rtcN`、`/dev/rtcN` 次设备号与 `/proc/driver/rtc` 归属。

### 3.3 注册核心

`__devm_rtc_register_device()`（`class.c:392-441`）按序执行：

```c
if (!rtc->ops)
	return -EINVAL;
if (!rtc->ops->set_alarm)
	clear_bit(RTC_FEATURE_ALARM, rtc->features);
if (rtc->ops->set_offset)
	set_bit(RTC_FEATURE_CORRECTION, rtc->features);
rtc->owner = owner;
rtc_device_get_offset(rtc);
err = __rtc_read_alarm(rtc, &alrm);
if (!err)
	rtc_initialize_alarm(rtc, &alrm);
rtc_dev_prepare(rtc);
err = cdev_device_add(&rtc->char_dev, &rtc->dev);
...
rtc_proc_add_device(rtc);
...
if (!strcmp(dev_name(&rtc->dev), CONFIG_RTC_HCTOSYS_DEVICE))
	rtc_hctosys(rtc);
```

调用链：

```text
probe() → devm_rtc_allocate_device() → rtc_device_get_id()/rtc_allocate_device()
       → rtc->ops = ...; set_bit(features)
       → devm_rtc_register_device() → __devm_rtc_register_device()
            → rtc_device_get_offset()
            → __rtc_read_alarm() → rtc_initialize_alarm()
            → rtc_dev_prepare() → cdev_device_add() → /dev/rtcN + sysfs
            → rtc_proc_add_device() → rtc_hctosys()（名字匹配时）
            → devm_add_action_or_reset(devm_rtc_unregister_device)
```

`rtc_allocate_device()`（`class.c:207-250`）的默认值：`set_offset_nsec = 1s+5ms`、`irq_freq=1`、`max_user_freq=64`、`dev.class=&rtc_class`、`dev.release=rtc_device_release`，并初始化 aie/uie/pie 定时器与 `irqwork`。本树用较新的 `kzalloc_obj(*rtc)` 与 `hrtimer_setup()`（旧资料常见 `kzalloc(sizeof(*rtc))`/`hrtimer_init()`）。`rtc_class` 是 `const struct class`，由 `rtc_init()`（`class.c:479-491`）在 `subsys_initcall` 中 `class_register()`，并调 `rtc_dev_init()` 分配 `0..RTC_DEV_MAX-1` 主设备号区（`dev.c:569-576`）。

### 3.4 注销与释放

`devm_rtc_unregister_device()`（`class.c:340-354`）：`rtc_proc_del_device()` → `cdev_device_del()`（除非 `RTC_NO_CDEV`）→ `rtc->ops = NULL`，阻止后续 `rtc_class_open()` 访问。`rtc_device_release()`（`class.c:25-41`）：清空 timerqueue、`cancel_work_sync(&rtc->irqwork)`、`ida_free()`、销毁 mutex、`kfree(rtc)`。

## 4. 用户空间接口

### 4.1 `/dev/rtcN` ioctl 全表

`rtc_dev_ioctl()`（`dev.c:212-474`）先做权限检查（`dev.c:227-249`），未识别的命令才落到 `ops->ioctl`（`dev.c:459-468`）。

| ioctl | 参数 | 权限 | 核心处理 |
|---|---|---|---|
| `RTC_RD_TIME` / `RTC_SET_TIME` | `rtc_time` | SET 需 `CAP_SYS_TIME` | `rtc_read_time()` / `rtc_set_time()` |
| `RTC_ALM_READ/SET` | `rtc_time` | — | 读闹钟；设置时用当前日/月/年补齐，必要时 +24h |
| `RTC_WKALM_RD/SET` | `rtc_wkalrm` | — | 带 `enabled` 的完整闹钟读写 |
| `RTC_AIE_ON/OFF`、`RTC_UIE_ON/OFF` | — | — | `rtc_alarm_irq_enable()` / `rtc_update_irq_enable()` |
| `RTC_PIE_ON/OFF` | — | 频率超限需 `CAP_SYS_RESOURCE` | `rtc_irq_set_state()` |
| `RTC_IRQP_READ/SET` | `unsigned long` | SET 超 `max_user_freq` 需 `CAP_SYS_RESOURCE` | 频率，上限 `RTC_MAX_FREQ=8192` |
| `RTC_EPOCH_READ/SET`、`RTC_PLL_GET/SET` | `unsigned long`/`rtc_pll_info` | EPOCH_SET 需 `CAP_SYS_TIME` | 透传驱动 `ioctl` |
| `RTC_PARAM_GET/SET` | `rtc_param` | SET 需 `CAP_SYS_TIME` | FEATURES 返回位图；CORRECTION 走 offset；其余透传 |
| `RTC_VL_READ/CLR`、`RTC_WIE_ON/OFF` | `unsigned int`/— | — | 透传驱动（电压低标志、看门狗输出） |

ABI 细节：`RTC_WKALM_SET` 与 `RTC_WIE_ON` 都用 `'p',0x0f`，靠 `_IOW` 的 size 位区分；`RTC_VL_*` 与 `RTC_PARAM_*` 复用 `0x13/0x14`，靠方向位区分。32 位兼容层转换 `RTC_IRQP_*`、`RTC_EPOCH_SET`（`dev.c:476-502`）。

### 4.2 read / poll / fasync：闹钟事件

`/dev/rtcN` 读的是**一个中断事件字**：低字节 `RTC_IRQF(0x80)` 加 `RTC_PF(0x40)/RTC_AF(0x20)/RTC_UF(0x10)`，高字节是计数。`rtc_dev_read()`（`dev.c:141-189`）要求 `count == sizeof(unsigned int)` 或 `count >= sizeof(unsigned long)`，核心循环是“取 `irq_data` 并清零 → 非零返回 → 否则 `schedule()`”，`O_NONBLOCK` 返回 `-EAGAIN`。`poll()` 在 `irq_data != 0` 时返回 `EPOLLIN|EPOLLRDNORM`（`dev.c:191-210`）；`fasync` 注册后中断 `kill_fasync(..., SIGIO, POLL_IN)`（`interface.c:695`）。打开是**独占**的（`RTC_DEV_BUSY`，`dev.c:23-38`）；`release()` 关闭 UIE/PIE 但**保留闹钟**（`dev.c:511-531`）。

### 4.3 sysfs

`/sys/class/rtc/rtcN/` 的属性由 `rtc_attrs[]`（`sysfs.c:250-261`）定义：

| 属性 | 读写 | 可见条件 | 含义 |
|---|---|---|---|
| `name` | R | 总是 | `驱动名 父设备名` |
| `date`/`time` | R | 总是 | `%ptRd`/`%ptRt` 格式化当前 RTC |
| `since_epoch` | R | 总是 | `rtc_tm_to_time64()` 秒数 |
| `max_user_freq` | RW | 总是 | 写值须 `0 < v < 4096` |
| `hctosys` | R | 总是 | 本次启动是否由它设系统时间 |
| `wakealarm` | RW | 父设备可唤醒且有闹钟 | 秒数；写入即 `rtc_set_alarm()` |
| `offset` | RW | `ops->set_offset` 存在 | ppb 校准值 |
| `range` | R | `range_max != range_min` | `[min,max]` 秒 |

`wakealarm_store()`（`sysfs.c:147-211`）支持 `+3600`（相对当前）、`+=60`（在活动闹钟上顺延）；已有活动闹钟时普通写入返回 `-EBUSY`；写过去时间用于关闭（`enabled=0`，并给 `now+300` 的合法未来值）。可见性由 `rtc_attr_is_visible()` 动态裁剪（`sysfs.c:276-295`），`rtc_does_wakealarm()` 要求 `device_can_wakeup(rtc->dev.parent)`（`sysfs.c:268-274`）——这是“没有 `wakealarm` 文件”的最常见原因。驱动可用 `rtc_add_group()/rtc_add_groups()` 追加属性（`sysfs.c:308-350`）。

### 4.4 procfs

`/proc/driver/rtc` 是**单文件**，只描述 hctosys 设备：`is_rtc_hctosys()` 在配置了 `CONFIG_RTC_HCTOSYS_DEVICE` 时比较名字，否则退化为 `id == 0`（`proc.c:20-37`）。输出 `rtc_time/date`、`alrm_time/date`、`alarm_IRQ`、`alrm_pending`、UIE/PIE 状态与频率，固定打印 `24hr : yes`，再调 `ops->proc()`（`proc.c:39-79`）。

## 5. 时间读写

### 5.1 读路径与校验

`rtc_read_time()`（`interface.c:110-124`）持 `ops_lock` 调 `__rtc_read_time()`（`interface.c:84-108`）：清零 `tm` → `ops->read_time(rtc->dev.parent, tm)` → `rtc_add_offset()` → `rtc_valid_tm()`。后者（`lib.c:159-174`）规则：

```c
if (tm->tm_year < 70 ||
    tm->tm_year > (INT_MAX - 1900) ||
    ((unsigned int)tm->tm_mon) >= 12 ||
    tm->tm_mday < 1 ||
    tm->tm_mday > rtc_month_days(tm->tm_mon,
				 ((unsigned int)tm->tm_year + 1900)) ||
    ((unsigned int)tm->tm_hour) >= 24 ||
    ((unsigned int)tm->tm_min) >= 60 ||
    ((unsigned int)tm->tm_sec) >= 60)
	return -EINVAL;
```

`tm_sec` 允许 0-59，**不接受闰秒 60**；`tm_wday/tm_yday/tm_isdst` 不参与校验。驱动返回 -1 的“通配”字段只在 `__rtc_read_alarm()` 中补齐，时间读取不允许。

### 5.2 写路径

`rtc_set_time()`（`interface.c:126-176`）：`rtc_valid_tm()` → `rtc_valid_range()` → `rtc_subtract_offset()` → 若 UIE 运行则先关闭（避免写时间产生假 1Hz 中断）→ 持锁调 `ops->set_time()` → `pm_stay_awake()` → 解锁后 `schedule_work(&rtc->irqwork)`（“可能有定时器刚好过期”）→ 重开 UIE。`rtc_valid_range()`（`interface.c:67-82`）在 `range_min != range_max` 时以 `start_secs` 为逻辑下限、`start_secs + range_max - range_min` 为逻辑上限，越界 `-ERANGE`。

### 5.3 缓存、精度与掉电

RTC 核心**不做时间缓存**：每次 `RTC_RD_TIME`、`cat since_epoch`、`hwclock -r` 都实际访问 I2C/SPI/MMIO。真正的状态保存在别处：

| 场景 | 谁保存 | 说明 |
|---|---|---|
| 启动 | timekeeping | `rtc_hctosys()` 只读一次，之后靠 clocksource |
| NTP 回写 | `kernel/time/ntp.c` | 约 11 分钟写一次，不读 |
| 闹钟 | `timerqueue` | 核心缓存“下一个到期时刻”，硬件只存最近一个 |
| 范围 | `offset_secs` | 表示范围映射，不是时间缓存 |

- **整秒**：`class.c:47-55` 明确“RTC 只存整秒”，hctosys 用截断值 + 0.5s。
- **走时误差**：晶振温漂典型 ±20ppm，每天约 ±1.7 秒，靠 `set_offset()` 校准。
- **掉电**：靠备份电池/超级电容；电压过低时驱动通过 `RTC_VL_*` 报告，用户态应据此判定时间无效。
- **写延迟**：`set_offset_nsec` 对齐“写命令发出”与“RTC 下一秒进位”，默认 1s+5ms；慢总线驱动应调大（`rtc.h:114-142`）。

## 6. 闹钟与唤醒

### 6.1 软硬两层

RTC 硬件通常只有**一个**闹钟寄存器，而内核里可能同时有多个使用者（`wakealarm` sysfs、`alarmtimer`、驱动内部）。核心用 `timerqueue` + `aie_timer` 做多路复用：

```text
用户/内核请求              核心软件层                    硬件
  wakealarm sysfs ─┐
  alarmtimer ──────┼→ timerqueue（按 expires 排序）→ 取最早节点
  驱动 rtc_timer ──┘         │
                             ▼
                     __rtc_set_alarm() ──→ ops->set_alarm() ──→ 闹钟寄存器
                             ▲
  IRQ → rtc_update_irq() → irqwork → rtc_timer_do_work() → 出队/回调/重编程
```

### 6.2 设置流程

`rtc_set_alarm()`（`interface.c:513-554`）：校验 `tm`/范围 → 持锁 → 若 `aie_timer` 已启用先 `rtc_timer_remove()` → `alarm_time = rtc_tm_to_ktime()` → 若置 `RTC_FEATURE_ALARM_RES_MINUTE` 则**向下取整到分钟**（保证不漏截止时间）→ 写入 `aie_timer.node.expires`、`period=0` → `enabled` 时 `rtc_timer_enqueue()`。

真正的“过去时间”检查在 `__rtc_set_alarm()`（`interface.c:450-511`）：`scheduled <= now` 返回 `-ETIME`，并对“设置后 1 秒内已过期”的竞态做二次读时间确认。`rtc_timer_enqueue()`（`interface.c:870-910`）只在新定时器比队首更早时写硬件，失败回滚出队。`rtc_initialize_alarm()`（`interface.c:557-587`）在注册时把硬件中已存在且未到期的闹钟接回软件队列，避免丢掉 bootloader 设置的唤醒闹钟。另有 `rtc_read_next_alarm()`（`interface.c:399-425`）直接从 timerqueue 返回真正最早到期的闹钟，区别于 `rtc_read_alarm()` 只反映 `aie_timer`。

### 6.3 中断到用户态

驱动在中断中调用 `rtc_update_irq()`，本树实现（`interface.c:751-759`）是：

```c
void rtc_update_irq(struct rtc_device *rtc,
		    unsigned long num, unsigned long events)
{
	if (IS_ERR_OR_NULL(rtc))
		return;
	pm_stay_awake(rtc->dev.parent);
	schedule_work(&rtc->irqwork);
}
```

它**忽略 `num` 与 `events`**：到期判定由 `rtc_timer_do_work()`（`interface.c:968-1039`）重新读 RTC、比较 `timerqueue` 得出，再调 `rtc_aie_update_irq()`/`rtc_uie_update_irq()`，最终进 `rtc_handle_legacy_irq()`（`interface.c:685-696`）填 `irq_data`、唤醒 `irq_queue`、发 `SIGIO`。旧资料常写“驱动用 events 上报 RTC_AF”，本树中该参数已不参与分发，是阅读时容易踩的坑。`rtc_timer_do_work()` 对 `__rtc_set_alarm()` 失败最多重试 3 次，失败则丢弃节点并 `dev_err`。

### 6.4 唤醒与 `RTC_FEATURE_ALARM_WAKEUP_ONLY`

闹钟要能唤醒系统，需三件事同时成立：驱动 `device_init_wakeup(父设备, true)`（如 `rtc-hym8563.c:544`、`rtc-cmos.c:873`）；驱动实现 `set_alarm`，核心保留 `RTC_FEATURE_ALARM`；中断线在 suspend 期间仍能触发。三者都满足 `wakealarm` 属性才出现（`sysfs.c:268-274,283-285`）。`RTC_FEATURE_ALARM_WAKEUP_ONLY` 表示该闹钟只能用于唤醒（如接 PMIC 的电源键式输入），不适合作为通用 alarmtimer 定时源。

## 7. 校准与 NTP

### 7.1 硬件校准

`rtc_read_offset()`/`rtc_set_offset()`（`interface.c:1108-1159`）单位是 **ppb**，调整公式为 `t = t0 * (1 + offset * 1e-9)`，即 `offset=1000` 表示每秒快 1ppm（每天约 0.086 秒）。驱动把 ppb 换算成校准寄存器（PCF8523 的 offset 模式、RV3028 的 EEPROM 校准）。核心发现 `ops->set_offset` 就置 `RTC_FEATURE_CORRECTION`（`class.c:405-406`），`/sys/.../offset` 随之可见；`RTC_PARAM_CORRECTION` 是同一功能的 param 通道。

### 7.2 systohc：11 分钟回写

NTP 守护进程通过 `adjtimex()` 清除 `STA_UNSYNC` 后，内核才认为系统时间可信。`kernel/time/ntp.c` 用 hrtimer 周期回写：

```c
#define SYNC_PERIOD_NS (11ULL * 60 * NSEC_PER_SEC)

static enum hrtimer_restart sync_timer_callback(struct hrtimer *timer)
{
	queue_work(system_freezable_power_efficient_wq, &sync_work);
	return HRTIMER_NORESTART;
}
```

`sync_hw_clock()`（`ntp.c:627-666`）：`!ntp_synced()` 或定时器已排队则返回；`rtc_tv_nsec_ok()` 判断是否在写入窗口；`persistent_clock_is_local` 时减去时区秒数；先试老 `update_persistent_clock64()`，返回 `-ENODEV` 才走 `update_rtc()`（`rtc_class_open(RTC_SYSTOHC_DEVICE)` → `rtc_set_time()`）；失败 2 秒后重试，成功约 11 分钟后再来。首次 `update_rtc()` 因 `*offset_nsec` 还是默认 500ms 而返回 `-EAGAIN`，记下 `rtc->set_offset_nsec` 后下一轮再写（`ntp.c:590-598`）。

### 7.3 相位对齐：`rtc_tv_nsec_ok()`，而非 `rtc_calc_irqsync`

旧资料常见的 `rtc_calc_irqsync()` 在**本树不存在**（全树 grep 无此符号），对应实现是 `ntp.c:539-561`：

```c
static inline bool rtc_tv_nsec_ok(unsigned long set_offset_nsec,
				  struct timespec64 *to_set,
				  const struct timespec64 *now)
{
	/* Allowed error in tv_nsec, arbitrarily set to 5 jiffies in ns. */
	const unsigned long TIME_SET_NSEC_FUZZ = TICK_NSEC * 5;
	struct timespec64 delay = {.tv_sec = -1, .tv_nsec = set_offset_nsec};

	*to_set = timespec64_add(*now, delay);
	if (to_set->tv_nsec < TIME_SET_NSEC_FUZZ) {
		to_set->tv_nsec = 0;
		return true;
	}
	...
	return false;
}
```

它把“现在 + (set_offset_nsec - 1s)”是否接近整秒作为窗口判据，容忍 5 个 tick。窗口内写入的 `to_set` 正好是 `t2 - 1s`，RTC 在 `t2` 的进位就与系统时钟整秒对齐。

## 8. 启动集成

### 8.1 hctosys

`CONFIG_RTC_HCTOSYS=y` + `CONFIG_RTC_HCTOSYS_DEVICE="rtc0"` 时，`__devm_rtc_register_device()` 在设备名匹配时调 `rtc_hctosys()`（`class.c:58-89`）：

```c
	err = rtc_read_time(rtc, &tm);
	if (err) {
		dev_err(rtc->dev.parent,
			"hctosys: unable to read the hardware clock\n");
		goto err_read;
	}
	tv64.tv_sec = rtc_tm_to_time64(&tm);
	err = do_settimeofday64(&tv64);
	...
err_read:
	rtc_hctosys_ret = err;
```

结果记录在 `rtc_hctosys_ret`，暴露为 `/sys/.../hctosys`。32 位机器上超过 `INT_MAX` 的 RTC 时间会被拒绝（摘录省略了该 `BITS_PER_LONG == 32` 检查）。注意它**在设备注册时**执行，不是统一 `late_initcall`；`rtc0` 必须早期 probe，否则系统时间停在时钟源初值，直到用户态 `hwclock -s`。

### 8.2 suspend/resume 补偿

`CONFIG_PM_SLEEP` 与 `CONFIG_RTC_HCTOSYS_DEVICE` 同时存在时，`rtc_class` 挂 `rtc_suspend`/`rtc_resume`（`class.c:100-193`）：suspend 只对 hctosys 设备生效，若 `timekeeping_rtc_skipsuspend()` 则跳过，快照 `old_rtc/old_system` 并用 `old_delta` 抑制反复 suspend 的 ±1 秒累积漂移；resume 若 `timekeeping_rtc_skipresume()` 则跳过，否则 `sleep_time = (new_rtc-old_rtc) - (new_system-old_system)`，再 `timekeeping_inject_sleeptime64()`。两个判据见 `timekeeping.c:2125-2142`，优先级是“非停止 clocksource > persistent clock > RTC”，RTC 只是最后兜底。

### 8.3 nvmem：后备电池域存储

`devm_rtc_nvmem_register()`（`nvmem.c:13-30`）把电池备份 SRAM/EEPROM 注册成标准 nvmem 设备：

```c
nvmem_config->dev = dev;
nvmem_config->owner = rtc->owner;
nvmem_config->add_legacy_fixed_of_cells = true;
nvmem = devm_nvmem_register(dev, nvmem_config);
```

之后用户态可通过 `/sys/bus/nvmem/devices/*/nvmem` 读写，驱动也可用 nvmem consumer API 读取 MAC、校准值。`rtc.h:257-266` 在未开 `CONFIG_RTC_NVMEM` 时提供返回 0 的空实现，驱动可无条件调用。

## 9. 调试与观测

### 9.1 观测点

| 入口 | 能看到什么 |
|---|---|
| `/sys/class/rtc/rtcN/{date,time,since_epoch,wakealarm,offset,range,hctosys}` | 当前时间/秒数（每次读都访问硬件）、闹钟、校准、范围、是否设置过系统时间 |
| `/proc/driver/rtc` | hctosys 设备的时间、闹钟、UIE/PIE 汇总 |
| `/dev/rtcN` + `read()` | 中断事件字（低字节标志、高字节计数） |
| `dmesg` | `registered as rtcN`、`setting system clock to ...`、`hctosys: unable to read...`、`alarm rollover not handled`、`invalid alarm value` |
| tracefs | `include/trace/events/rtc.h` 的 `rtc_read_time`、`rtc_set_alarm`、`rtc_timer_*` |
| `CONFIG_RTC_DEBUG` | 打开后 `ccflags -DDEBUG`（`Makefile:6`），驱动 `dev_dbg` 全部可见 |

`/proc/driver/rtc` 典型内容（字段来自 `proc.c:49-73`）：

```text
rtc_time	: 12:34:56
rtc_date	: 2026-11-26
alrm_time	: 07:00:00
alarm_IRQ	: yes
alrm_pending	: no
update IRQ enabled	: no
periodic IRQ enabled	: no
periodic IRQ frequency	: 1
max user IRQ frequency	: 64
```

### 9.2 常见问题

| 现象 | 常见原因 | 排查 |
|---|---|---|
| 没有 `wakealarm` | 父设备不支持唤醒或驱动没实现 `set_alarm` | 查 `wakeup-source`、`device_init_wakeup()`、`features` |
| 写 `wakealarm` 返回 `-EBUSY`/`-ETIME` | 已有活动闹钟 / 时间已过或对齐后过期 | 先写 `0` 关闭，或用 `+=`、`+秒数` |
| 闹钟不触发 | 无 IRQ 线、未 `request_irq`、`alarm_irq_enable` 为空、精度只到分钟 | `/proc/driver/rtc` 看 `alarm_IRQ`，dmesg 看中断 |
| 开机时间错乱 | 备份电池失效、RTC 存本地时间 | 读 `RTC_VL_READ`，检查时区策略 |
| `/dev/rtcN` 打不开 `-EBUSY` | 字符设备单次打开限制 | 关闭占用进程 |
| 多 RTC 编号不对 | 设备树没写 `aliases { rtc0 = ...; }` | 检查 `of_alias_get_id()` 与 dmesg |
| 分钟级误差 | 未校准、晶振温漂 | 写 `offset`（ppb）或 `hwclock --systohc` |

## 10. 配置项

本树 `.config:2698-2799` 实际值：

| 配置 | 值 | 作用 | 备注 |
|---|---|---|---|
| `CONFIG_RTC_LIB` / `CONFIG_RTC_CLASS` | `y` | 日历换算库 / rtc-class 核心 | `Makefile:8-9` |
| `CONFIG_RTC_HCTOSYS` | `y` | 启动用 RTC 设系统时间 | `Kconfig:25-31` |
| `CONFIG_RTC_HCTOSYS_DEVICE` | `"rtc0"` | 指定 hctosys 设备 | `Kconfig:33-51` |
| `CONFIG_RTC_SYSTOHC` | `y` | NTP 同步后回写 | `Kconfig:53-59` |
| `CONFIG_RTC_SYSTOHC_DEVICE` | `"rtc0"` | 回写目标 | `Kconfig:61-70` |
| `CONFIG_RTC_NVMEM` | `y` | 后备存储 | `Kconfig:87-93` |
| `CONFIG_RTC_INTF_{SYSFS,PROC,DEV}` | `y` | 三个用户接口 | `Kconfig:97-133` |
| `CONFIG_RTC_INTF_DEV_UIE_EMUL` | 未启用 | 轮询模拟 UIE | `Kconfig:135-145`，源码存在 |
| `CONFIG_RTC_DEBUG` / `CONFIG_RTC_DRV_TEST` | 未启用 | 调试 `-DDEBUG` / 软件 RTC 测试驱动 | `Kconfig:72-76,147-159`，源码存在 |

已启用 19 个硬件驱动（`.config:2722-2799`）：`DS1307`、`HYM8563`、`MAX77686`、`RK808`、`ISL1208`、`PCF85363`、`PCF8563`、`M41T80`、`BQ32K`、`TPS6594`、`RX8581`、`RV3028`、`RV8803`、`S5M`、`DS3232`、`PCF2127`、`PCF85063`、`DA9063`、`MT6397`。没有 `RTC_DRV_CMOS`、`RTC_DRV_EFI` 或平台 RTC（`PL031` 等），说明该配置面向 ARM 开发板而非 x86 PC。

需要纠正 `01-subsystem-panorama.md:344` 的“RTC 未启用”：本树 RTC core、三个用户接口、hctosys/systohc 均启用，只有调试与测试选项关闭。

## 11. 关键文件清单

| 文件 | 行数 | 关键符号 |
|---|---|---|
| `include/linux/rtc.h` / `include/uapi/linux/rtc.h` | 284 / 152 | `rtc_device`(87)、`rtc_class_ops`(59)、`rtc_timer`(75)；`rtc_time`(25)、`rtc_wkalrm`(41)、ioctl(86-120) |
| `drivers/rtc/class.c` | 491 | `rtc_allocate_device`(207)、`__devm_rtc_register_device`(392)、`rtc_hctosys`(58)、`rtc_init`(479) |
| `drivers/rtc/interface.c` | 1159 | `rtc_read_time`(110)、`rtc_set_time`(126)、`rtc_set_alarm`(513)、`rtc_timer_do_work`(968) |
| `drivers/rtc/dev.c` | 576 | `rtc_dev_ioctl`(212)、`rtc_dev_read`(141)、`rtc_dev_prepare`(548) |
| `drivers/rtc/sysfs.c` | 351 | `wakealarm_store`(148)、`offset_store`(228)、`rtc_attr_is_visible`(276) |
| `drivers/rtc/proc.c` / `nvmem.c` | 92 / 31 | `rtc_proc_show`(39)、`is_rtc_hctosys`(20)；`devm_rtc_nvmem_register`(13) |
| `drivers/rtc/lib.c` | 211 | `rtc_valid_tm`(159)、`rtc_time64_to_tm`(52)、`rtc_tm_to_time64`(180) |
| `drivers/rtc/rtc-core.h` | 43 | 接口层条件编译桩 |
| `drivers/rtc/Kconfig` / `Makefile` | 2175 / 201 | 配置项；`rtc-core-y := class.o interface.o`(11) |
| `kernel/time/ntp.c` | — | `SYNC_PERIOD_NS`(497)、`rtc_tv_nsec_ok`(539)、`sync_hw_clock`(627) |
| `kernel/time/timekeeping.c` | — | `persistent_clock_is_local`(1763)、`timekeeping_rtc_skip*`(2125/2139) |

## 12. 延伸阅读

- 同目录上层概览：[驱动子系统全景：输入、显示、音频、温控、稳压、监控与 IIO](../01-subsystem-panorama.md)
- 设备模型基础：[driver core：device/driver/bus 与 probe](../driver-model/01-driver-model.md)
- 系统时间与 clocksource：[时间子系统分析](../../02-core/time/time_subsystem_analysis.md)
- 内核文档与范例：`Documentation/admin-guide/rtc.rst`（两套用户 API、闹钟语义）；`drivers/rtc/rtc-test.c`（最小软件模型）、`rtc-hym8563.c`（I2C+IRQ+wakeup）、`rtc-cmos.c`（x86 PC 兼容 RTC）
- 用户态工具：`util-linux` 的 `hwclock`（`--hctosys`/`--systohc`）、`rtcwake`（基于 `wakealarm`）
