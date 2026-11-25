# 总线与设备类框架：I2C / SPI / GPIO / pinctrl / regmap / USB / MMC / UFS

## 1. 概述

本文横向对比 8 个常用子系统如何接入设备模型。共同骨架是：

```text
控制器驱动 → 注册控制器对象(device_add) → 子系统总线 match()
→ 设备驱动 probe() → 字符/块/网络等上层接口
```

不同之处在于“设备如何被发现”：I2C/SPI 由控制器解析 DT/ACPI 子节点创建 client/device；
USB 由 hub 枚举；MMC 由 `mmc_rescan()` 扫描卡；UFS 由 `ufshcd_probe_hba()` 读取设备描述符
并由 SCSI 层创建 LUN。GPIO 与 pinctrl 更偏“资源提供者”，regmap 则是总线访问的通用抽象。

## 2. I2C

### 2.1 核心对象

`include/linux/i2c.h`：

- `struct i2c_adapter`：一个 I2C 控制器，含 `owner`、`algo`（`i2c_algorithm`）、`nr`、
  `name`、`dev`、`bus_lock/mux_lock`、`timeout`、`quirks`、`bus_recovery_info`。
- `struct i2c_client`：挂在该 adapter 上的从设备，含 `addr`、`flags`、`name`、
  `adapter`、`dev`、`irq`、`init_irq`。
- `struct i2c_driver`：含 `id_table`（`i2c_device_id`）、`probe/remove/shutdown`、
  内嵌 `device_driver`、`address_list`、`clients` 链表、`detect`。

### 2.2 总线类型与匹配

`drivers/i2c/i2c-core-base.c:699`：

```c
const struct bus_type i2c_bus_type = {
    .name    = "i2c",
    .match   = i2c_device_match,
    .probe   = i2c_device_probe,
    .remove  = i2c_device_remove,
    .shutdown= i2c_device_shutdown,
};
```

`i2c_device_match()`（140）顺序：OF（`i2c_of_match_device`）→ ACPI
（`acpi_driver_match_device`）→ `i2c_match_id(driver->id_table, client)`。
当前 `.config` 未启用 ACPI，ACPI 分支源码存在但不生效。

### 2.3 probe 路径

`i2c_device_probe()`（491）：

1. `client->irq = client->init_irq`；若无 IRQ，按 Host Notify / OF `interrupts`
   / ACPI 顺序解析，`-EPROBE_DEFER` 直接返回。
2. 校验驱动至少提供 `id_table` 或 OF/ACPI 匹配表，否则 `-ENODEV`。
3. 处理 wakeup：`I2C_CLIENT_WAKE` 时解析 `wakeup` 中断并
   `device_init_wakeup()`、`dev_pm_set_wake_irq()`。
4. `of_clk_set_defaults()`、`dev_pm_domain_attach()`、`devres_open_group()`。
5. 调用 `driver->probe(client)`；失败时 `devres_release_group()` 只回滚本组资源。
6. `i2c_device_remove()` 再次释放组、清 wake irq、`client->irq = 0`。

注意 I2C 核心**不关闭** probe 中打开的组，因此 probe 之后驱动追加的资源也会在
remove 时统一释放。

### 2.4 注册

- `i2c_register_driver()`（2029）：设 `driver->driver.bus = &i2c_bus_type` 后
  `driver_register()`，再 `i2c_for_each_dev(__process_new_driver)` 匹配已有 adapter。
- `i2c_add_adapter()`（1679）/`i2c_add_numbered_adapter()`（1717）→
  `i2c_register_adapter()`：初始化锁、申请 id、设 `dev.bus`/`dev.type`、
  `device_initialize()`、runtime PM 设置、`device_add()`；随后
  `of_i2c_register_devices()`、`i2c_acpi_register_devices()`、静态 board info，
  最后 `bus_for_each_drv(__process_new_adapter)` 通知驱动。

## 3. SPI

### 3.1 核心对象

`include/linux/spi/spi.h`：

- `struct spi_controller`：主机控制器，含 `bus_num`、`num_chipselect`、`mode_bits`、
  `setup()`、`transfer_one()`、`transfer_one_message()`、`prepare_message()`、
  `set_cs()`、DMA 参数、`dev`。
- `struct spi_device`：从设备，含 `modalias`、`max_speed_hz`、`chip_select`、`mode`、
  `irq`、`controller`、`dev`。
- `struct spi_driver`：`probe/remove/shutdown`、`id_table`、内嵌 `device_driver`。

### 3.2 总线类型与匹配

`drivers/spi/spi.c:456`：

```c
const struct bus_type spi_bus_type = {
    .name       = "spi",
    .dev_groups = spi_dev_groups,
    .match      = spi_match_device,
    .uevent     = spi_uevent,
    .probe      = spi_probe,
    .remove     = spi_remove,
    .shutdown   = spi_shutdown,
};
```

`spi_match_device()`（370）：driver_override → OF → ACPI → `id_table`
（`spi_match_id(sdrv->id_table, spi->modalias)`）→ `modalias` 与驱动名比较。
SPI 的 modalias 由 DT compatible 或 `spi_device_id` 生成；源码注释特别说明
DT 场景下仍需 `spi_device_id` 才能生成 `spi:` modalias 供模块自动加载，
当前无模块，此点仅影响用户态。

### 3.3 probe 与注册

`spi_probe()`（407）：`of_clk_set_defaults()`；OF 用 `of_irq_get(dev->of_node, 0)`，
ACPI 用 `acpi_dev_gpio_irq_get()` 取 IRQ；`dev_pm_domain_attach()`
（`PD_FLAG_ATTACH_POWER_ON | PD_FLAG_DETACH_POWER_OFF`）；最后 `sdrv->probe(spi)`。

`__spi_register_driver()`（475）设置 `owner`、`bus = &spi_bus_type` 后
`driver_register()`。`spi_register_controller()`（3451）先
`spi_controller_check_ops()`，再确定 `bus_num`（DT alias 或动态分配）、
`dev_set_name("spi%u")`、`device_add(&ctlr->dev)`，然后枚举 DT/ACPI/board_info
子设备；卸载走 `spi_unregister_controller()`（3614）。

## 4. GPIO

### 4.1 抽象层次

- `struct gpio_chip`（`include/linux/gpio/driver.h`）：控制器驱动实现，
  含 `label`、`parent`、`request/free`、`direction_input/output`、`get/set`、
  `get_direction`、`ngpio`、`base`、`can_sleep`、`irq_chip` 等。
- `struct gpio_device`（`drivers/gpio/gpiolib.h`）：gpiolib 对 chip 的封装，
  含 `id`、`dev`、`descs[]`、`ngpio`、`label`、`srcu`、`line_state_notifier`。
- `struct gpio_desc`：单个引脚，含 `gdev`、`flags`、`label`、`name`、`hog` 等。

### 4.2 控制器注册

`gpiochip_add_data_with_key()`（`drivers/gpio/gpiolib.c:1172`）：

1. 分配 `gpio_device`（`gc->gpiodev = gdev`），`ida_alloc(&gpio_ida)` 取 id，
   `dev_set_name("gpiochip%d")`。
2. `device_initialize()`，设 `dev.type = &gpio_dev_type`、
   `dev.bus = &gpio_bus_type`、parent 指向 `gc->parent`。
3. 读取 `ngpio`、分配 `descs`、复制 label、初始化 line state 通知链。
4. 分配全局 GPIO 编号 base，建立 pin range，最终 `device_add()` 发布到 sysfs。
   `gpiochip_remove()`（1411）反向注销并释放。

### 4.3 消费者获取

`gpiod_get()`（4943）→ `gpiod_get_index()`（5063）→
`gpiod_find_and_request()`（4779）：

- 先按 fwnode（DT/ACPI）查找 `gpio`/`gpios` 属性；shared line 建立 proxy lookup。
- fwnode 未命中时回退平台 lookup table（`gpiod_find()`）。
- `gpiod_request(desc, label)` 申请独占；`gpiod_configure_flags()` 应用
  `GPIOD_OUT_HIGH/LOW`、`GPIOD_IN`、`GPIOD_ASIS` 等。

`gpiod_direction_output()`（3150）与 `gpiod_set_value()` 最终调用
`gpio_chip` 的 `direction_output`/`set` 回调；`can_sleep` 的控制器使用
`gpiod_*_cansleep()` 变体。

## 5. pinctrl

### 5.1 核心对象

`drivers/pinctrl/core.h`：

- `struct pinctrl_dev`（52）：一个引脚控制器，含 `desc`（`pinctrl_desc`）、
  `pin_desc_tree`、`gpio_ranges`、`dev`、`p`（本控制器的 pinctrl 句柄）、
  `hog_default/hog_sleep`、`mutex`。
- `struct pinctrl`（87）：某个设备使用的引脚控制句柄，含 `dev`、`states`、
  当前 `state`、`dt_maps`、`kref users`。
- `struct pinctrl_state`（102）：一个状态（default/init/sleep/idle），含
  `settings` 链表。
- `struct pinctrl_setting`：一条 mux 或 config 设置，指向 group/function 或
  pinconf 参数。

### 5.2 注册与查找

- `pinctrl_register_and_init()`（`drivers/pinctrl/core.c:2307`）创建 `pinctrl_dev`、
  解析 pin/group/function 描述、注册 pinmux/pinconf、建立 sysfs/debugfs，
  再由 `pinctrl_enable()` 发布。
- `pinctrl_unregister()`（2335）注销并释放。
- `pinctrl_get(dev)`（1184）：已有句柄则 `kref_get()`，否则
  `create_pinctrl()` 解析该设备的 DT pinctrl 映射，建立 `pinctrl_state` 链表。
- `pinctrl_lookup_state(p, name)`（1275）按名字查找；未找到且启用
  `pinctrl_dummy_state` 时创建 dummy 状态。
- `pinctrl_select_state(p, state)`（1422）把状态里的 mux/config 设置下发硬件。

### 5.3 与设备模型衔接

`really_probe()` 在调用驱动 probe 前调用 `pinctrl_bind_pins(dev)`
（`drivers/base/pinctrl.c:23`）：

1. `devm_kzalloc` 分配 `dev->pins`。
2. `devm_pinctrl_get(dev)`；无句柄时忽略 `-ENOENT`。
3. 查找 `default` 状态；再查 `init` 状态，有 init 则选 init，否则选 default。
4. `CONFIG_PM` 打开时还查找 `sleep`、`idle` 状态，供 suspend/runtime PM 切换。

probe 成功后 `pinctrl_init_done()`（core.c:1629）把 `init` 态切换到 `default` 态；
从而允许驱动在 probe 期间使用临时引脚配置，结束后进入正常工作配置。

## 6. regmap

### 6.1 作用

regmap 把“寄存器读写”从具体总线中抽象出来：同一份驱动代码可跑在 I2C、SPI、
MMIO、SCCB、SDW、SLIMbus 等后端上，并统一提供缓存、访问权限检查、IRQ 芯片支持。

### 6.2 核心结构

`drivers/base/regmap/internal.h:50` 的 `struct regmap` 关键字段：

```c
struct regmap {
    union { struct mutex mutex; spinlock_t spinlock; raw_spinlock_t raw_spinlock; };
    regmap_lock lock; regmap_unlock unlock; void *lock_arg;
    struct device *dev;           /* 执行 I/O 的设备 */
    void *work_buf;               /* 格式化读写缓冲 */
    struct regmap_format format;  /* reg/val 字节序、位宽、pad */
    const struct regmap_bus *bus; /* 后端总线操作 */
    void *bus_context;
    unsigned int max_register;
    bool (*writeable_reg)(...); bool (*readable_reg)(...); bool (*volatile_reg)(...);
    int (*reg_read)(...); int (*reg_write)(...); int (*reg_update_bits)(...);
    int (*read)(...); int (*write)(...);   /* bulk */
    const struct regcache_ops *cache_ops;
    enum regcache_type cache_type;
};
```

### 6.3 初始化与读写

- `__regmap_init(dev, bus, bus_context, config, ...)`（regmap.c:677）：
  按 `config->disable_locking`、自定义 lock、`use_hwlock`、`fast_io` 选择
  互斥锁/自旋锁/raw spinlock；校验 `reg_bits/val_bits`、`max_register`、
  读写权限表；选择 regcache 实现。
- `devm_regmap_init()`（1190）把 `regmap_exit()` 注册为 devres。
- `regmap_write()`（1978）→ `_regmap_raw_write_impl()`（1637）：先写 regcache
  （`cache_only` 时只写缓存），再格式化 `work_buf`，必要时按 window 拆分，
  最终调用 `map->bus->write()`。
- `regmap_read()`（2864）→ `_regmap_raw_read()`（2754）：缓存命中则直接返回，
  否则总线读并回填缓存。

### 6.4 后端

- `regmap-i2c.c`：`__regmap_init_i2c()`（419）由 `regmap_get_i2c_bus()`（350）
  根据 `config->reg_bits/val_bits/read_flag_mask` 选择 SMBus 或 raw I2C 传输。
- `regmap-spi.c`：`__regmap_init_spi()`（139）。
- `regmap-mmio.c`：MMIO 后端。
- `regcache.c` + `regcache-flat/rbtree/maple.c`：缓存实现。
- `regmap-irq.c`：把设备寄存器中断映射为 Linux IRQ domain。

## 7. USB

### 7.1 对象模型

`include/linux/usb.h`：

- `struct usb_device`：物理设备，含 `devnum`、`devpath`、`descriptor`、
  `config`、`actconfig`、`ep0`、`state`、`parent`、`port`。
- `struct usb_interface`：一个配置接口，含 `altsetting[]`、`cur_altsetting`、
  `num_altsetting`、`condition`、`dev`；`usb_host_endpoint`/`usb_host_interface`
  描述端点和备用设置。
- `struct usb_driver`：接口驱动，含 `id_table`、`probe/remove`、
  `supports_autosuspend`、`disable_hub_initiated_lpm`；
  `struct usb_device_driver` 是整设备驱动（如 usbfs）。
- `struct urb`：USB 请求块，异步传输的基本单位。

### 7.2 总线与匹配

`usb_bus_type`（`drivers/usb/core/driver.c:2076`）的 `match = usb_device_match`。
`usb_device_match()`（868）区分两类对象：

- `is_usb_device(dev)`：只与 `usb_device_driver` 匹配；无 id_table/match 时
  返回 1 交由 probe 判断，否则 `usb_driver_applicable()`。
- `is_usb_interface(dev)`：只与 `usb_driver` 匹配，先 `usb_match_id()`，
  再 `usb_match_dynamic_id()`（sysfs `new_id` 动态添加）。

### 7.3 probe / remove

`usb_probe_interface()`（driver.c:318）：

1. 检查 `usb_device_is_owned()`、设备与接口的 `authorized`。
2. `usb_match_dynamic_id()` / `usb_match_id()` 再次确认 id。
3. `usb_autoresume_device()` 保证父设备活跃。
4. `intf->condition = USB_INTERFACE_BINDING`；`pm_runtime_set_active(dev)`；
   驱动支持 autosuspend 时 `pm_runtime_enable()`。
5. 按 `disable_hub_initiated_lpm` 关闭 LPM；必要时 `usb_set_interface(..., 0)`
   切回 altsetting 0。
6. 调用 `driver->probe(intf, id)`；成功后 `condition = USB_INTERFACE_BOUND`，
   平衡 LPM 引用并 `usb_autosuspend_device()`。
7. 失败路径清 `intfdata`、清 remote wakeup、恢复 LPM、关闭 runtime PM。

`usb_unbind_interface()`（428）负责停止 URB、释放带宽、清 endpoint、
关闭 runtime PM。`usb_register_driver()`（1060）与
`usb_register_device_driver()`（993）分别注册接口驱动与设备驱动。

### 7.4 枚举

USB 设备发现由 hub 驱动完成：`hub_event()` → `hub_port_connect()` →
`usb_alloc_dev()` → `hub_port_init()` → `usb_new_device()` → `device_add()`。
枚举读取设备/配置/字符串描述符，为每个接口创建 `usb_interface` 设备。
源码在 `drivers/usb/core/hub.c`、`message.c`、`config.c`。
当前 `.config` 未启用 `CONFIG_USB_SUPPORT`，源码存在但不会编译进内核。

## 8. MMC

### 8.1 对象模型

`include/linux/mmc/host.h`、`core.h`：

- `struct mmc_host`：主机控制器，含 `ops`（`mmc_host_ops`）、`class_dev`、
  `parent`、`index`、`ocr`、`caps/caps2`、`f_min/f_max`、`bus_ops`、
  `detect`（delayed work）、`slot`、`mmc`（当前卡）。
- `struct mmc_card`：一张卡，含 `host`、`type`（MMC/SD/SDIO/SDcombo）、
  `state`、`cid/csd/scr`、`cis`（SDIO）、`dev`。
- `struct mmc_driver`：卡驱动（如 mmc_block），含 `probe/remove/shutdown`、
  `device_driver`。
- `struct mmc_host_ops`：`request`、`set_ios`、`get_ro`、`get_cd`、
  `enable_sdio_irq`、`start_signal_voltage_switch` 等。
- `struct mmc_bus_ops`：按卡类型实现的 `detect/remove/suspend/resume/power_off`。

### 8.2 总线与注册

`mmc_bus_type`（`drivers/mmc/core/bus.c:220`）：

```c
static const struct bus_type mmc_bus_type = {
    .name       = "mmc",
    .dev_groups = mmc_dev_groups,
    .uevent     = mmc_bus_uevent,
    .probe      = mmc_bus_probe,
    .remove     = mmc_bus_remove,
    .shutdown   = mmc_bus_shutdown,
    .pm         = &mmc_bus_pm_ops,
};
```

注意它**没有 `.match`**；`driver_match_device()` 在无 match 时默认返回 1，
因此 `mmc_bus_probe()` 直接 `drv->probe(card)`。卡类型区分靠 `mmc_card->type`
与 `mmc_bus_ops`，SDIO 功能驱动另走 `sdio_bus.c`。

`mmc_alloc_host(extra, dev)`（host.c:512）：分配 `mmc_host + extra`，
`rescan_disable = 1`，从 DT alias 或 ida 取 index，`dev_set_name("mmc%d")`，
class 设为 `mmc_host_class`，初始化 detect 工作为 `mmc_rescan`。
`mmc_add_host()`（649）`device_add(&host->class_dev)` 后 `mmc_start_host()`；
`mmc_remove_host()`（679）反向停止。

### 8.3 扫描与卡初始化

`mmc_rescan()`（`drivers/mmc/core/core.c:2252`，delayed work）：

1. `rescan_disable` 时直接返回；不可移除卡只扫描一次；已有 `host->bus_ops`
   时先 `bus_ops->detect(host)` 校验卡是否仍在。
2. 可移除卡用 `ops->get_cd()` 判断插槽；无卡则 `mmc_power_off()`。
3. 依次尝试 SD Express、UHS-II（`mmc_attach_sd_uhs2()`）；再按 `freqs[]`
   从高到低 `mmc_rescan_try_freq()`，发 CMD0/CMD1/ACMD41 识别 MMC/SD。
4. 成功则 `mmc_attach_mmc()` / `mmc_attach_sd()` / `mmc_attach_sdio()`
   设置 `host->bus_ops` 并 `device_add(&card->dev)`，触发 `mmc_bus_probe()`。
5. `MMC_CAP_NEEDS_POLL` 时重新调度 detect work。

当前 `.config` 未启用 `CONFIG_MMC`，上述源码存在但未编译。

## 9. UFS

### 9.1 对象模型

`include/ufs/ufshcd.h`：

- `struct ufs_hba`：UFS 主机控制器抽象，含 `mmio_base`、`irq`、`host`
  （`Scsi_Host`）、`dev`、`vops`（`ufs_hba_variant_ops`）、`caps`、`quirks`、
  `nutrs/nutmq`、`ufs_version`、`rpm_lvl/spm_lvl`、`clk_gating`、
  `clk_scaling`、`ahit`、`mcq_enabled`。
- `struct ufs_hba_variant_ops`：厂商/平台回调（`init`、`hce_enable_notify`、
  `link_startup_notify`、`pwr_change_notify`、`setup_clocks`、`setup_regulators`、
  `apply_dev_quirks`、`suspend/resume`）。
- `struct ufs_dev_info`：设备描述符、`wb`、`auto_hibern8` 等。
- UFS 是 SCSI 传输层：`ufshcd_host_template` 提供 `queuecommand`、`eh_*`，
  LUN 以 SCSI device 形式出现。

### 9.2 平台 probe 路径

`ufshcd_pltfrm_init()`（`drivers/ufs/host/ufshcd-pltfrm.c:492`）流程：
`devm_platform_ioremap_resource()` → `platform_get_irq()` →
`ufshcd_alloc_host()` → 保存 `vops` → `ufshcd_parse_clock_info()` /
`ufshcd_parse_regulator_info()` / `ufshcd_parse_operating_points()` →
`ufshcd_init(hba, mmio_base, irq)` → `pm_runtime_set_active/enable()`。

`ufshcd_init()`（`drivers/ufs/core/ufshcd.c:11059`）关键步骤：

1. `dev_set_drvdata(dev, hba)`；记录 `mmio_base`、`irq`、`vps`，初始化锁、
   `rpm_lvl/spm_lvl`、`vcc_off_delay_us`。
2. `ufshcd_hba_init()`：时钟、regulator、PHY、variant `init`；读 capabilities、
   UFS 版本、中断掩码并设置 DMA mask。
3. `ufshcd_memory_alloc()` 分配 UTRL/UTRD/任务请求并配置 LRB。
4. 设置 `Scsi_Host` 参数（`can_queue`、`max_lun`、`max_cmd_len`、
   `queuecommand_may_block`），创建错误处理/异常事件工作队列。
5. 清中断状态、注册 `ufshcd_intr` IRQ、`ufshcd_device_reset()`、`ufshcd_hba_enable()`。
6. `ufshcd_add_scsi_host()` 注册 SCSI host；`pm_runtime_get_sync()` 持住
   自动挂起直到扫描完成。
7. `ufshcd_async_scan()`（9452）获取 `host_sem`，`ufshcd_probe_hba(hba, true)`
   初始化 link、读取设备描述符、配置 Write Booster/auto-hibernate，
   再 `ufshcd_add_lus()` 创建 LUN，最后 `pm_runtime_put_sync()`。

`ufshcd_probe_hba()`（9403）还处理 `UFSHCD_QUIRK_REINIT_AFTER_MAX_GEAR_SWITCH`：
最大 gear 切换后复位设备与控制器并重新初始化；`ufshcd_remove()`（10820）
释放 IRQ、SCSI host、工作队列、时钟/regulator。

当前 `.config` 未启用 `CONFIG_SCSI_UFSHCD`，源码存在但未编译。

## 10. 横向对比

| 子系统 | 控制器对象 | 设备发现方式 | match 依据 | probe 入口 |
|--------|-----------|--------------|-----------|-----------|
| I2C | `i2c_adapter` | DT/ACPI 子节点、board info | OF/ACPI/id_table | `i2c_device_probe` |
| SPI | `spi_controller` | DT/ACPI 子节点、board info | OF/ACPI/id_table/modalias | `spi_probe` |
| GPIO | `gpio_chip`/`gpio_device` | 驱动注册 chip | 无（资源提供者） | 消费者 `gpiod_get` |
| pinctrl | `pinctrl_dev` | 驱动注册 + DT 映射 | 无（资源提供者） | `pinctrl_bind_pins` |
| regmap | 无独立设备 | 由上层驱动创建 | 无（访问抽象） | `__regmap_init` |
| USB | `usb_device`/`usb_interface` | hub 枚举 | id_table/dynamic id | `usb_probe_interface` |
| MMC | `mmc_host` | `mmc_rescan` 扫描 | 无 `.match`，默认匹配 | `mmc_bus_probe` |
| UFS | `ufs_hba` + `Scsi_Host` | UFS 初始化 + SCSI 扫描 | SCSI/平台 | `ufshcd_pltfrm_init` |

共同点：控制器先 `device_add()`；probe 用 `devm_*` 取资源；remove 由 driver core
统一清理 devres 与 PM domain。

## 11. 配置状态说明

当前 `.config`：`CONFIG_I2C=y`、`CONFIG_REGMAP=y`、`CONFIG_GPIOLIB=y`、
`CONFIG_PINCTRL=y`；`CONFIG_SPI`、`CONFIG_USB_SUPPORT`、`CONFIG_MMC`、
`CONFIG_SCSI_UFSHCD` 未启用。未启用只表示调试配置裁剪，源码完整存在。

## 12. 关键文件清单

| 路径 | 内容 |
|------|------|
| `drivers/i2c/i2c-core-base.c` | I2C adapter/client/driver 注册、match、probe |
| `include/linux/i2c.h` | `i2c_adapter`、`i2c_client`、`i2c_driver` |
| `drivers/spi/spi.c` | SPI 控制器/设备/驱动、match、probe |
| `include/linux/spi/spi.h` | `spi_controller`、`spi_device`、`spi_driver` |
| `drivers/gpio/gpiolib.c` | gpiochip 注册、gpiod 查找/申请 |
| `drivers/gpio/gpiolib.h` | `gpio_device`、`gpio_desc` 内部结构 |
| `drivers/pinctrl/core.c` | pinctrl 设备注册、状态查找与切换 |
| `drivers/pinctrl/core.h` | `pinctrl_dev`、`pinctrl`、`pinctrl_state` |
| `drivers/base/pinctrl.c` | `pinctrl_bind_pins()` |
| `drivers/base/regmap/regmap.c` | regmap 初始化、读写、缓存 |
| `drivers/base/regmap/internal.h` | `struct regmap` |
| `drivers/base/regmap/regmap-i2c.c` / `regmap-spi.c` | I2C / SPI 后端 |
| `drivers/usb/core/driver.c` | USB 总线匹配、接口 probe/unbind、驱动注册 |
| `drivers/usb/core/hub.c` | USB 枚举与端口状态机 |
| `drivers/mmc/core/bus.c` / `host.c` | `mmc_bus_type`、`mmc_alloc_host()` |
| `drivers/mmc/core/core.c` | `mmc_rescan()` 扫描与卡初始化 |
| `drivers/ufs/core/ufshcd.c` | `ufshcd_init()`、`ufshcd_probe_hba()`、异步扫描 |
| `drivers/ufs/host/ufshcd-pltfrm.c` | `ufshcd_pltfrm_init()` 平台 probe |
