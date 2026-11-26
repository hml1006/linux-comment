# 驱动子系统全景：输入、显示、音频、温控、稳压、监控与 IIO

> 本文补齐“驱动模型/总线”之外的设备类框架（device class）。这些子系统共享 driver core 的 probe 机制，但各自有一套面向用户态的字符设备、sysfs 或框架内 API。
>
> 当前 `.config` 只启用了少量驱动类：`I2C`、`GPIOLIB`、`PINCTRL`、`DMA_ENGINE`、`PCI` 为 `y`；`USB`、`MMC`、`SPI`、`DRM`、`SOUND`、`THERMAL`、`HWMON`、`REGULATOR` 均未启用。下文描述的是 Linux 7.2 源码能力。

---

## 1. 设备类框架的共同模式

一个设备类通常由四层组成：

```
用户态接口        /dev/xxx、sysfs、debugfs、ioctl
    │
框架核心          class、core API、注册/注销、事件分发
    │
具体驱动          vendor driver、总线 driver（platform/i2c/spi/pci）
    │
硬件              控制器、传感器、编解码器、面板
```

共性机制：

- **注册**：驱动调用框架的 `xxx_register()`，框架分配核心对象并挂到 class/bus；
- **probe**：总线匹配后调用驱动 `probe()`，驱动初始化硬件并向框架注册能力；
- **用户态**：字符设备（input/event、drm/card0、snd/pcmC0D0p、iio:device0）或 sysfs（hwmon、thermal、regulator、leds）；
- **电源管理**：`dev_pm_ops` 的 suspend/resume/runtime PM 回调；
- **设备树/ACPI**：通过 `of_match_table` / `acpi_match_table` 匹配。

---

## 2. 输入子系统（Input）与 HID

### 2.1 分层

```
硬件 → 总线驱动（USB/I2C/SPI/GPIO）→ input_dev
                                      │
                                 input core
                                      │
                    evdev / joydev / event handler
                                      │
                                 /dev/input/eventX
```

### 2.2 核心对象

| 对象 | 作用 |
|---|---|
| `struct input_dev` | 一个输入设备：能力位图（EV_KEY/EV_REL/EV_ABS）、名称、ID |
| `struct input_handler` | 事件处理器：evdev、joydev、sysrq 等 |
| `struct input_handle` | device 与 handler 的绑定 |
| `struct input_event` | 用户态事件：type/code/value |

### 2.3 事件路径

```
驱动中断/轮询
  → input_report_key() / input_report_abs() / input_report_rel()
  → input_event()
       ├─ 更新 input_dev 状态
       ├─ 分发给所有 handle
       └─ evdev 写入 client 缓冲区
  → 用户态 read(/dev/input/eventX)
```

### 2.4 HID

HID 是 USB/蓝牙/I2C-HID 等传输层之上的“人机接口设备”协议：

- `drivers/hid/hid-core.c` 解析 HID report descriptor；
- `hid_device` 表示一个 HID 设备，`hid_driver` 表示驱动；
- `hid-input.c` 把 HID usage 映射为 input 事件；
- 支持 `hidraw` 直接把 report 暴露给用户态。

### 2.5 关键源码

| 文件 | 作用 |
|---|---|
| `drivers/input/input.c` | input core |
| `drivers/input/evdev.c` | evdev 字符设备 |
| `include/linux/input.h` | `input_dev`、事件类型 |
| `drivers/hid/hid-core.c` | HID descriptor 解析 |
| `drivers/hid/hid-input.c` | HID → input 映射 |

---

## 3. 显示子系统（DRM/KMS）

### 3.1 分层

```
用户态 (libdrm, Mesa, compositor)
  │ ioctl / mmap
DRM core (drivers/gpu/drm/drm_ioctl.c)
  ├─ KMS：CRTC、encoder、connector、plane、bridge
  ├─ GEM：drm_gem_object、dma-buf
  ├─ 调度：drm_sched、fence
  └─ 显存管理：TTM / GEM VRAM
具体 GPU 驱动 (i915, amdgpu, msm, panfrost, ...)
```

### 3.2 KMS 对象

| 对象 | 作用 |
|---|---|
| `struct drm_device` | DRM 设备 |
| `struct drm_crtc` | 扫描输出控制器 |
| `struct drm_encoder` | 把 CRTC 信号编码到 connector |
| `struct drm_connector` | 物理输出（HDMI/DP/eDP/DSI） |
| `struct drm_plane` | 图层（primary/cursor/overlay） |
| `struct drm_bridge` | 显示桥（DSI→eDP、HDMI 转换等） |

### 3.3 典型路径

```
open(/dev/dri/card0)
  → drm_open()
  → DRM_IOCTL_MODE_GETRESOURCES
  → DRM_IOCTL_MODE_ATOMIC
       → drm_atomic_commit()
       → 驱动 atomic_check/atomic_commit
       → 写寄存器、提交 fence
  → page flip 完成 → vblank 事件
```

### 3.4 关键源码

| 文件 | 作用 |
|---|---|
| `drivers/gpu/drm/drm_drv.c` | DRM 设备注册 |
| `drivers/gpu/drm/drm_ioctl.c` | ioctl 分发 |
| `drivers/gpu/drm/drm_atomic.c` | atomic modeset |
| `drivers/gpu/drm/drm_gem.c` | GEM 对象 |
| `include/drm/drm_device.h` | `drm_device` |
| `include/drm/drm_crtc.h` | KMS 对象 |

---

## 4. 音频子系统（ALSA / ASoC）

### 4.1 ALSA 分层

```
用户态 (PulseAudio/PipeWire/alsa-lib)
  │ /dev/snd/pcmCxDyp, controlCx, timer
ALSA core (sound/core/)
  ├─ PCM：playback/capture substream
  ├─ Control：kcontrol
  ├─ Timer
  └─ Sequencer
ASoC (sound/soc/)
  ├─ Machine driver：把 CPU DAI 与 Codec DAI 连起来
  ├─ Platform driver：DMA/DAI 控制器
  └─ Codec driver：编解码器
```

### 4.2 核心对象

| 对象 | 作用 |
|---|---|
| `struct snd_pcm` | PCM 设备 |
| `struct snd_pcm_substream` | playback 或 capture 流 |
| `struct snd_soc_component` | ASoC 组件（platform/codec） |
| `struct snd_soc_dai` | 数字音频接口 |
| `struct snd_soc_dapm_widget` | DAPM 电源管理节点 |

### 4.3 播放路径

```
write(/dev/snd/pcmC0D0p)
  → snd_pcm_writei()
  → snd_pcm_lib_write()
  → copy_from_user 到 DMA buffer
  → snd_pcm_period_elapsed()
  → 触发 pointer/period 中断
  → 应用调用 snd_pcm_avail()/mmap 推进
```

### 4.4 关键源码

| 文件 | 作用 |
|---|---|
| `sound/core/pcm.c` | PCM 核心 |
| `sound/core/pcm_lib.c` | PCM 读写/period |
| `sound/core/control.c` | control 接口 |
| `sound/soc/soc-core.c` | ASoC 核心 |
| `sound/soc/soc-dapm.c` | DAPM |
| `include/sound/soc-component.h` | `snd_soc_component` |

---

## 5. 温控子系统（Thermal）

### 5.1 模型

```
温度传感器 → thermal_zone_device
                  │
            thermal governor（step_wise/power_allocator/...）
                  │
            cooling_device（cpufreq/风扇/GPU）
```

核心对象：

| 对象 | 作用 |
|---|---|
| `struct thermal_zone_device` | 一个温区：温度读取、trip point、governor |
| `struct thermal_cooling_device` | 可降温设备：cpufreq、风扇、devfreq |
| `struct thermal_trip` | 触发点：passive/active/hot/critical |

关键路径：

```
thermal_zone_device_update()
  → 读取温度
  → 遍历 trip point
  → governor->throttle()
  → cooling_device->set_cur_state()
```

关键源码：`drivers/thermal/thermal_core.c`、`thermal_governor.c`、`gov_step_wise.c`、`gov_power_allocator.c`、`include/linux/thermal.h`。

---

## 6. 稳压器子系统（Regulator）

### 6.1 模型

```
regulator_dev（硬件稳压器）
  → regulator（消费者句柄）
  → regulator_consumer_supply（设备树/board 约束）
```

核心对象：

| 对象 | 作用 |
|---|---|
| `struct regulator_dev` | 稳压器设备：enable、voltage、current、mode |
| `struct regulator` | 消费者句柄 |
| `struct regulation_constraints` | 电压/电流范围、always-on、boot-on |

关键路径：

```
regulator_get(dev, "vdd")
  → regulator_enable()
  → regulator_set_voltage()
  → 驱动 ops->enable/set_voltage
```

关键源码：`drivers/regulator/core.c`、`drivers/regulator/devres.c`、`include/linux/regulator/driver.h`、`include/linux/regulator/consumer.h`。

---

## 7. 硬件监控（HWMON）

HWMON 把传感器暴露到 `/sys/class/hwmon/hwmonN/`：

- `temp1_input`、`in0_input`、`fan1_input`、`pwm1` 等属性；
- 驱动实现 `hwmon_ops` 的 `read`/`write`/`is_visible`；
- 支持 `hwmon_device_register_with_info()` 新式注册。

核心源码：`drivers/hwmon/hwmon.c`、`include/linux/hwmon.h`。

---

## 8. 工业 I/O（IIO）

IIO 面向 ADC/DAC、加速度计、陀螺仪、光感、磁力计等：

```
用户态：/dev/iio:deviceN + /sys/bus/iio/devices/iio:deviceN/
  │
IIO core：iio_dev、iio_chan_spec、buffer/trigger
  │
具体传感器驱动
```

核心对象：

| 对象 | 作用 |
|---|---|
| `struct iio_dev` | IIO 设备 |
| `struct iio_chan_spec` | 通道描述：type、channel、scan_type |
| `struct iio_buffer` | 采样缓冲区 |
| `struct iio_trigger` | 触发源 |

关键路径：

```
iio_device_register()
  → 创建 /dev/iio:deviceN
  → 用户态使能 buffer/trigger
  → 驱动 push 采样（iio_push_to_buffers）
  → 用户态 read/mmap
```

关键源码：`drivers/iio/industrialio-core.c`、`industrialio-buffer.c`、`industrialio-trigger.c`、`include/linux/iio/iio.h`。

---

## 9. 多媒体（V4L2）

V4L2 覆盖摄像头、视频采集、编解码：

- `video_device` 表示 `/dev/videoN`；
- `v4l2_subdev` 表示 sensor/ISP/CSI 等子设备；
- `vb2_queue` 管理 videobuf2 队列；
- Media controller 描述 pipeline（sensor → CSI → ISP → video node）。

关键源码：`drivers/media/v4l2-core/`、`drivers/media/platform/`、`include/media/v4l2-dev.h`、`include/media/v4l2-subdev.h`。

---

## 10. RTC、Watchdog 与 LED

| 子系统 | 用户态接口 | 核心对象 | 关键源码 |
|---|---|---|---|
| RTC | `/dev/rtcN`、`/sys/class/rtc` | `struct rtc_device` | `drivers/rtc/class.c`、`interface.c` |
| Watchdog | `/dev/watchdogN` | `struct watchdog_device` | `drivers/watchdog/watchdog_core.c` |
| LED | `/sys/class/leds` | `struct led_classdev` | `drivers/leds/led-class.c` |

这些子系统都比较薄，核心是“注册设备 + 实现 ops + 暴露字符设备/sysfs”。

---

## 11. 配置与调试

| 功能 | 当前 `.config` | 调试入口 |
|---|---|---|
| Input | 未单独列出，随总线启用 | `/dev/input/eventX`、`evtest` |
| HID | 依赖 USB/I2C-HID | `/sys/bus/hid`、`hidraw` |
| DRM | 未启用 | `/dev/dri/card0`、`modetest` |
| ALSA/ASoC | 未启用 | `/proc/asound`、`aplay -l` |
| Thermal | 未启用 | `/sys/class/thermal` |
| Regulator | 未启用 | `/sys/class/regulator`、`debugfs/regulator` |
| HWMON | 未启用 | `/sys/class/hwmon`、`sensors` |
| IIO | 未启用 | `/sys/bus/iio/devices`、`iio_info` |
| V4L2 | 未启用 | `/dev/videoN`、`v4l2-ctl` |
| RTC | **启用** | `/sys/class/rtc`、`/dev/rtcN`；`RTC_CLASS=y`、`RTC_HCTOSYS=y`；详见 [RTC 专章](misc/02-rtc.md) |
| Watchdog / LED | 未启用 | `/dev/watchdog`、`/sys/class/leds`；`CONFIG_WATCHDOG`、`CONFIG_NEW_LEDS` 均未设置；框架见 [watchdog 专章](misc/01-watchdog.md) |

---

## 12. 关键文件清单

| 文件 | 作用 |
|---|---|
| `drivers/input/input.c` | input core |
| `drivers/input/evdev.c` | evdev |
| `drivers/hid/hid-core.c` | HID core |
| `drivers/gpu/drm/drm_drv.c` | DRM 设备 |
| `drivers/gpu/drm/drm_atomic.c` | atomic modeset |
| `sound/core/pcm.c` | ALSA PCM |
| `sound/soc/soc-core.c` | ASoC core |
| `drivers/thermal/thermal_core.c` | thermal core |
| `drivers/regulator/core.c` | regulator core |
| `drivers/hwmon/hwmon.c` | hwmon core |
| `drivers/iio/industrialio-core.c` | IIO core |
| `drivers/media/v4l2-core/` | V4L2 core |
| `drivers/rtc/class.c` | RTC core |
| `drivers/watchdog/watchdog_core.c` | watchdog core |
| `drivers/leds/led-class.c` | LED core |
