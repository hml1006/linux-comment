# platform 总线与辅助总线（auxiliary bus）分析

## 1. 概述

platform 总线是“无真实可枚举总线”的设备的统一容器：SoC 内部的 UART、I2C 控制器、
GPIO 控制器、时钟控制器等通常没有硬件 ID，只能由 DT/ACPI 或板级代码描述，再通过
platform bus 交给驱动。源码集中在 `drivers/base/platform.c`，接口声明于
`include/linux/platform_device.h`。

auxiliary bus 解决另一类问题：一个物理设备（如网卡 SoC、音频 DSP）内部包含多个
功能块，父驱动需要把子功能拆成独立 device/driver 交给不同驱动。它复用 driver core
的 bus 机制，源码在 `drivers/base/auxiliary.c`。

当前 `.config` 中 `CONFIG_OF=y`、`CONFIG_ACPI` 未启用，因此 platform 匹配的 ACPI 分支
源码存在但不会走到；`CONFIG_MODULES` 未启用，所有 platform driver 均为内建。

## 2. `struct platform_device`

定义于 `include/linux/platform_device.h:23`：

```c
struct platform_device {
    const char *name;              /* 设备名，名字匹配与 uevent 使用 */
    int id;                        /* 实例号，PLATFORM_DEVID_NONE/AUTO 特殊 */
    bool id_auto;
    struct device dev;             /* 内嵌通用设备对象 */
    u64 platform_dma_mask;
    struct device_dma_parameters dma_parms;
    u32 num_resources;
    struct resource *resource;     /* 资源数组 */
    const struct platform_device_id *id_entry; /* 匹配到的 id_table 项 */
    struct mfd_cell *mfd_cell;     /* MFD 子设备回指 */
    struct pdev_archdata archdata;
};
```

`dev_is_platform(dev)` 用 `dev->bus == &platform_bus_type` 判断；
`to_platform_device(x)` 用 `container_of()` 从 `struct device` 取回 `platform_device`。
`platform_get_device_id(pdev)` 返回 `id_entry`，驱动可据此区分具体变体。

## 3. `struct platform_driver`

定义于 `include/linux/platform_device.h:270`：

```c
struct platform_driver {
    int (*probe)(struct platform_device *);
    void (*remove)(struct platform_device *);
    void (*shutdown)(struct platform_device *);
    int (*suspend)(struct platform_device *, pm_message_t state);
    int (*resume)(struct platform_device *);
    struct device_driver driver;          /* 通用驱动对象 */
    const struct platform_device_id *id_table;
    bool prevent_deferred_probe;
    bool driver_managed_dma;              /* 驱动自行管理 DMA/IOMMU 域 */
};
```

注册宏（同文件 295 行）：

```c
#define platform_driver_register(drv) \
    __platform_driver_register(drv, THIS_MODULE, KBUILD_MODNAME)
```

`__platform_driver_register()`（platform.c:920）只做三件事：

```c
drv->driver.owner = owner;
drv->driver.bus = &platform_bus_type;
drv->driver.mod_name = mod_name;
return driver_register(&drv->driver);
```

随后进入通用 driver core：查重、`bus_add_driver()`、`driver_attach()` 扫描已有设备、
创建 sysfs 目录与 `bind`/`unbind`。`platform_driver_unregister()` 直接
`driver_unregister(&drv->driver)`。

## 4. platform 总线类型

`platform_bus_type` 定义于 `drivers/base/platform.c:1509`：

```c
const struct bus_type platform_bus_type = {
    .name            = "platform",
    .dev_groups      = platform_dev_groups,
    .driver_override = true,
    .match           = platform_match,
    .uevent          = platform_uevent,
    .probe           = platform_probe,
    .remove          = platform_remove,
    .shutdown        = platform_shutdown,
    .dma_configure   = platform_dma_configure,
    .dma_cleanup     = platform_dma_cleanup,
    .pm              = &platform_dev_pm_ops,
};
```

`platform_bus`（platform.c:42）是一个仅设置 `init_name = "platform"` 的根设备，
所有无父设备的 platform device 都挂到它下面，形成 `/sys/devices/platform/`。

`platform_dev_pm_ops` 使用 `SET_RUNTIME_PM_OPS(pm_generic_runtime_suspend,
pm_generic_runtime_resume, NULL)` 加 `USE_PLATFORM_PM_SLEEP_OPS`，即由
`drivers/base/power/generic_ops.c` 的通用回调转调驱动自己的 `dev_pm_ops`。

## 5. resource 模型

`struct resource`（`include/linux/ioport.h:22`）：

```c
struct resource {
    resource_size_t start;
    resource_size_t end;
    const char *name;
    unsigned long flags;
    unsigned long desc;
    struct resource *parent, *sibling, *child;
};
```

类型位掩码：`IORESOURCE_IO`、`IORESOURCE_MEM`、`IORESOURCE_REG`、`IORESOURCE_IRQ`、
`IORESOURCE_DMA`、`IORESOURCE_BUS`。`resource_type(r)` 取 `flags & IORESOURCE_TYPE_BITS`。

常用查询 API（platform.c）：

| 函数 | 行号 | 行为 |
|------|------|------|
| `platform_get_resource(pdev, type, num)` | 55 | 线性扫描 `pdev->resource`，返回第 num 个指定类型资源 |
| `platform_get_mem_or_io(pdev, num)` | 71 | 只接受 MEM/IO |
| `platform_get_irq_optional()` / `platform_get_irq()` | 278/301 | 从 IORESOURCE_IRQ 取中断号，可选版本允许缺失 |
| `platform_get_resource_byname()` | 461 | 按 `resource->name` 查找 |
| `platform_get_irq_byname()` | 509 | 按名字取 IRQ |

`platform_device_add()` 在把设备交给 driver core 前会为每个资源补名字、设置父节点：
`IORESOURCE_MEM` 默认父为 `iomem_resource`，`IORESOURCE_IO` 默认父为
`ioport_resource`，然后 `insert_resource()` 把区间插入资源树，冲突则注册失败。
这保证了同一地址区间不会被两个 platform device 同时声明。

## 6. platform device 注册

### 6.1 分配与填充

`platform_device_alloc(name, id)`（platform.c:623）分配 `struct platform_device`
并初始化 `dev.release = platform_device_release`、`dev.bus = &platform_bus_type`、
名字与 id。随后可用：

- `platform_device_add_resources(pdev, res, n)`（651）：拷贝资源数组。
- `platform_device_add_data(pdev, data, size)`（679）：拷贝 `platform_data`。

`platform_device_register_full()` 提供一次性封装：接收 `struct
platform_device_info`（name/id/res/parent/of_node/fwnode/properties 等），
内部 `platform_device_alloc()` + `add_resources()` + 可选
`device_create_managed_software_node()`，最后 `platform_device_add()`。

### 6.2 `platform_device_add()`（platform.c:703）

```text
platform_device_add(pdev)
├─ 无 parent → dev->parent = &platform_bus
├─ dev->bus = &platform_bus_type
├─ 命名:
│   id >= 0        → "%s.%d"
│   PLATFORM_DEVID_NONE → "%s"
│   PLATFORM_DEVID_AUTO → ida_alloc() 后 "%s.%d.auto"
├─ 遍历 num_resources:
│   补 r->name = dev_name(dev)
│   无 parent 时按类型挂 iomem_resource / ioport_resource
│   insert_resource() 占位
├─ device_add(dev)
└─ 失败: 逆序 release_resource() + 释放自动 id
```

`platform_device_register(pdev)`（821）就是 `platform_device_add(pdev)`；
`platform_device_unregister()`（837）= `platform_device_del()` +
`platform_device_put()`。`platform_device_del()`（792）反向 `device_del()`、
释放资源、清理自动 id；`platform_device_put()` 最终 `put_device()` 触发
`platform_device_release()`（597）释放内存。

## 7. driver 注册的两种方式

### 7.1 常规 `platform_driver_register()`

适用于可热插拔或设备可能晚于驱动出现的场景。注册后 `driver_attach()` 会扫描
`platform_bus_type` 上所有未绑定设备，匹配成功即 probe；未匹配的设备保留，
后续设备注册时再反向匹配。允许返回 `-EPROBE_DEFER` 参与延迟探测。

### 7.2 `platform_driver_probe()`

用于“驱动与设备都必须在 init 阶段完成绑定”的不可热插拔驱动，
`__platform_driver_probe()`（platform.c:974 附近）：

```c
drv->driver.probe_type = PROBE_FORCE_SYNCHRONOUS;  /* 禁止异步 probe */
drv->prevent_deferred_probe = true;                /* 禁止 -EPROBE_DEFER */
drv->driver.suppress_bind_attrs = true;            /* 不建 bind/unbind */
drv->probe = probe;                                /* 临时挂上 __init 函数 */
__platform_driver_register(drv, module, mod_name);
drv->probe = platform_probe_fail;                  /* 之后禁止再 probe */
if (!bus_for_each_dev(&platform_bus_type, NULL, &drv->driver, is_bound_to_driver))
    retval = -ENODEV, platform_driver_unregister(drv);
```

`platform_probe_fail()` 返回 `-ENXIO`，`platform_probe()` 检测到该函数指针时
直接失败，避免调用已释放的 `__init` 代码。

## 8. 匹配规则

`platform_match()`（platform.c:1368）按固定优先级：

1. `device_match_driver_override(dev, drv)`：若设备设置了 `driver_override`，
   只允许同名驱动，返回负值表示未设置，继续后续匹配。
2. `of_driver_match_device(dev, drv)`：比较 `of_match_table` 的 compatible。
3. `acpi_driver_match_device(dev, drv)`：ACPI `_HID`/`_CID`（当前 `.config`
   未启用 ACPI，源码存在但不生效）。
4. `pdrv->id_table`：`platform_match_id()` 逐项比较 `platform_device_id.name`。
5. 名字回退：`strcmp(pdev->name, drv->name) == 0`。

`platform_uevent()`（1395）优先使用 OF 的 modalias，其次 ACPI，最后生成
`MODALIAS=platform:<pdev->name>`。

设备侧还可通过 `/sys/bus/platform/devices/<dev>/driver_override` 强制绑定，
这也是 `.driver_override = true` 的作用；`device_match_driver_override()` 的
三态语义（>0 命中、0 不命中、<0 未设置）让 bus 能区分“已强制”和“继续匹配”。

## 9. probe / remove 路径

### 9.1 `platform_probe()`（platform.c:1413）

```c
if (unlikely(drv->probe == platform_probe_fail))
    return -ENXIO;
ret = of_clk_set_defaults(_dev->of_node, false);
if (ret < 0)
    return ret;
ret = dev_pm_domain_attach(_dev, PD_FLAG_ATTACH_POWER_ON |
                                 PD_FLAG_DETACH_POWER_OFF);
if (ret)
    goto out;
if (drv->probe)
    ret = drv->probe(dev);
out:
if (drv->prevent_deferred_probe && ret == -EPROBE_DEFER) {
    dev_warn(_dev, "probe deferral not supported\n");
    ret = -ENXIO;
}
return ret;
```

要点：

- `of_clk_set_defaults()` 按 DT 的 `assigned-clocks`/`assigned-clock-rates` 设置时钟。
- `dev_pm_domain_attach()` 在 probe 前挂接 genpd/ACPI PM domain；`PD_FLAG_DETACH_POWER_OFF`
  让解绑时执行掉电。当前 `.config` 未开 `CONFIG_PM`，该路径被编译裁剪，
  但源码完整存在于 `drivers/base/power/common.c`。
- 真正的驱动 probe 由 `drv->probe()` 完成；返回 `-EPROBE_DEFER` 时进入 deferred probe。
- `driver_managed_dma` 影响 `platform_dma_configure()`：
  为真时 platform 不再调用 `iommu_device_use_default_domain()`，
  由驱动自行管理 IOMMU 域（典型为 VFIO）。

### 9.2 remove / shutdown

`platform_remove()`（1450）仅调用 `drv->remove(dev)`；
`platform_shutdown()` 调用 `drv->shutdown(dev)`。
`remove` 之后 driver core 统一执行 `device_unbind_cleanup()`，自动释放 devres、
断开 PM domain、清空 drvdata。因此 platform 驱动应优先使用 `devm_*` 管理资源，
无需在 remove 中手工回收。

## 10. auxiliary bus

### 10.1 设计目标

父驱动（如 SoC 网卡驱动）发现硬件包含多个子功能，需要把每个子功能暴露为独立
device，让不同驱动 probe。auxiliary bus 不依赖 DT/ACPI 枚举，而是由父驱动显式创建，
因此适合“一个 PCI/平台设备内部再拆分”的场景。

### 10.2 数据结构

`include/linux/auxiliary_bus.h:145`：

```c
struct auxiliary_device {
    struct device dev;              /* dev.parent / dev.release 必须填写 */
    const char *name;               /* 子功能名，如 "foo_dev" */
    u32 id;                         /* 同名设备的唯一实例号 */
    struct {
        struct xarray irqs;
        struct mutex lock;
        bool irq_dir_exists;
    } sysfs;
    void *registration_data_rust;
};

struct auxiliary_driver {
    int  (*probe)(struct auxiliary_device *, const struct auxiliary_device_id *id);
    void (*remove)(struct auxiliary_device *);
    void (*shutdown)(struct auxiliary_device *);
    int  (*suspend)(struct auxiliary_device *, pm_message_t state);
    int  (*resume)(struct auxiliary_device *);
    const char *name;
    struct device_driver driver;
    const struct auxiliary_device_id *id_table;
};
```

### 10.3 命名与匹配

`match_name = "<父驱动 KBUILD_MODNAME>.<name>"`；注册到总线上的设备名再拼接 id：
`foo_mod.foo_dev.0`。驱动 id_table 中填写 `match_name`，例如：

```c
static const struct auxiliary_device_id foo_ids[] = {
    { .name = "foo_mod.foo_dev" },
    {}
};
```

`auxiliary_match()`（auxiliary.c:191）只比较 `id_table` 与 `auxdev` 的 match_name；
`auxiliary_bus_type`（auxiliary.c:249）注册 `probe/remove/shutdown/match/uevent`。
`__auxiliary_driver_register()`（350）要求 `probe` 与非空 `id_table`，
驱动名为 `modname.name` 或仅 `modname`，然后 `driver_register()`。

### 10.4 三步注册

1. 父驱动分配 `auxiliary_device`，设置 `name`、`id`、`dev.parent`、
   `dev.release`（释放内存的唯一位置）。
2. `auxiliary_device_init(auxdev)`（275）：校验字段、`dev.bus = &auxiliary_bus_type`、
   `device_initialize()`、初始化 sysfs 锁。
3. `auxiliary_device_add(auxdev)`（宏 → `__auxiliary_device_add()`，315）：
   设置 `dev` 名字并 `device_add()`。

卸载顺序相反：先 `auxiliary_device_delete()`（`device_del()`）再
`auxiliary_device_uninit()`（`put_device()`），父驱动必须在自己的 remove 完成前
注销所有子设备；常用 `devm_add_action_or_reset()` 保证顺序。

`auxiliary_device_create()`（408）与 `__devm_auxiliary_device_create()`（477）
提供更高层封装；总线初始化由 `auxiliary_bus_init()`（509）注册。

## 11. 当前配置状态

| 配置项 | 当前值 | 影响 |
|--------|--------|------|
| `CONFIG_OF` | y | platform/I2C/SPI 的 DT 匹配、`of_clk_set_defaults()` 生效 |
| `CONFIG_ACPI` | 未启用 | `acpi_driver_match_device()` 等源码存在但不参与匹配 |
| `CONFIG_MODULES` | 未启用 | 所有 platform/auxiliary driver 内建，不产生 .ko |
| `CONFIG_PM` | 未启用 | platform probe 中的 PM domain attach 路径被裁剪 |
| `CONFIG_IOMMU_SUPPORT` | 视配置 | `platform_dma_configure()` 的默认域逻辑随之变化 |

源码层面上述功能均存在；文档所述“未启用”仅指 `.config` 裁剪，不代表内核缺失实现。

## 12. 关键调用链

```text
注册 platform 设备
platform_device_alloc(name, id)
  ├─ platform_device_add_resources() / platform_device_add_data()
  └─ platform_device_add(pdev)
       ├─ dev->parent = &platform_bus; dev->bus = &platform_bus_type
       ├─ dev_set_name("%s.%d" / "%s" / "%s.%d.auto")
       ├─ insert_resource() 占位每个 resource
       └─ device_add()
            └─ bus_probe_device() → __device_attach()
                 └─ platform_match() → platform_probe()
                      ├─ of_clk_set_defaults()
                      ├─ dev_pm_domain_attach()
                      └─ drv->probe(pdev)

注册 platform 驱动
platform_driver_register(drv)
  └─ __platform_driver_register()
       ├─ drv->driver.bus = &platform_bus_type
       └─ driver_register()
            ├─ bus_add_driver()
            │    └─ driver_attach() → bus_for_each_dev()
            │         └─ really_probe() → platform_probe()
            └─ kobject_uevent(KOBJ_ADD)

注册 auxiliary 设备
auxiliary_device_init(auxdev)
  └─ auxiliary_device_add(auxdev)
       └─ device_add() → auxiliary_match() → auxiliary_bus_probe()
            └─ auxdrv->probe(auxdev, id)
```

## 13. 关键文件清单

| 路径 | 内容 |
|------|------|
| `drivers/base/platform.c` | platform device/driver 注册、resource 查询、match/probe/remove |
| `include/linux/platform_device.h` | `platform_device`、`platform_driver`、注册宏 |
| `include/linux/ioport.h` | `struct resource`、IORESOURCE_* 类型与标志 |
| `drivers/base/auxiliary.c` | auxiliary bus 注册、match/probe、设备创建/删除 |
| `include/linux/auxiliary_bus.h` | `auxiliary_device`、`auxiliary_driver` 与 API |
| `include/linux/device-id/auxiliary.h` | `auxiliary_device_id` 定义 |
| `drivers/base/dd.c` | 通用 match/probe/bind 与 deferred probe |
| `drivers/base/core.c` | `device_add()`、uevent、device links |
| `drivers/base/power/common.c` | `dev_pm_domain_attach()` / `dev_pm_domain_detach()` |
| `drivers/base/pinctrl.c` | probe 前的引脚状态绑定 |
