# Linux 设备驱动模型（driver core）分析

## 1. 概述

Linux 设备模型位于 `drivers/base/`，把“总线（bus）—设备（device）—驱动（driver）”抽象为统一对象，
向上为 sysfs、uevent、电源管理、热插拔提供基础设施，向下由各子系统（platform、I2C、SPI、USB、PCI……）
以 `struct bus_type` 为锚点接入。核心文件与职责：

| 文件 | 职责 |
|------|------|
| `drivers/base/core.c` | device 对象生命周期、sysfs/uevent、class、device links |
| `drivers/base/driver.c` | `driver_register()` / `driver_unregister()` |
| `drivers/base/bus.c` | bus 注册、设备/驱动挂入总线、probe 触发入口 |
| `drivers/base/dd.c` | match→probe→bind 主流程、deferred probe、异步 probe |
| `drivers/base/devres.c` | 设备资源托管（devm_*） |
| `drivers/base/class.c` | 设备类注册与 class 接口 |
| `drivers/base/base.h` | `struct device_private`、`subsys_private` 及内部接口 |

本笔记基于当前源码树（Makefile 7.2.0）。当前 `.config` 未启用 `CONFIG_MODULES`，但设备模型是
常驻核心代码，与模块支持无关；区别只在驱动只能内建、uevent 不会触发模块自动加载。

## 2. 核心数据结构

### 2.1 `struct device`

定义于 `include/linux/device.h:703`。关键字段：

```c
struct device {
    struct kobject kobj;              /* sysfs 中的代表对象 */
    struct device *parent;            /* 父设备，决定 sysfs 层次与 PM 顺序 */
    struct device_private *p;         /* driver core 私有数据 */
    const char *init_name;
    const struct bus_type *bus;       /* 所属总线 */
    struct device_driver *driver;     /* 已绑定驱动 */
    void *platform_data;
    void *driver_data;                /* dev_set_drvdata() 使用 */
    struct { const char *name; spinlock_t lock; } driver_override;
    struct mutex mutex;               /* 串行化对驱动的调用 */
    struct dev_links_info links;      /* 消费者/供应者链表与状态 */
    struct dev_pm_info power;         /* runtime PM / system PM 状态 */
    struct dev_pm_domain *pm_domain;
#ifdef CONFIG_PINCTRL
    struct dev_pin_info *pins;
#endif
    struct device_node *of_node;      /* DT 节点 */
    struct fwnode_handle *fwnode;     /* 统一固件节点 */
    dev_t devt;
};
```

`device` 内嵌 `kobject`，因此设备天然出现在 sysfs；`p`（`struct device_private`）承载
`knode_parent`、`knode_driver`、`knode_bus`、`knode_class` 等 klist 节点以及
`deferred_probe` 链表节点和 `dead`、`ready_to_probe` 标志。

### 2.2 `struct bus_type`

定义于 `include/linux/device/bus.h:83`：

```c
struct bus_type {
    const char *name;
    const char *dev_name;             /* 自动命名前缀，如 "i2c" */
    const struct attribute_group *const *bus_groups;
    const struct attribute_group *const *dev_groups;
    const struct attribute_group *const *drv_groups;

    int  (*match)(struct device *dev, const struct device_driver *drv);
    int  (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
    int  (*probe)(struct device *dev);       /* 总线级 probe，优先于 driver->probe */
    void (*sync_state)(struct device *dev);
    void (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);
    ...
    int  (*dma_configure)(struct device *dev);
    void (*dma_cleanup)(struct device *dev);
    const struct dev_pm_ops *pm;
    bool driver_override;
    bool need_parent_lock;
};
```

总线注册后由 `subsys_private`（`drivers/base/base.h`）保存 `klist_devices`、`klist_drivers`、
`devices_kset`、`drivers_kset`、`interfaces` 等运行期数据。

### 2.3 `struct device_driver`

定义于 `include/linux/device/driver.h:100`：

```c
struct device_driver {
    const char *name;
    const struct bus_type *bus;
    struct module *owner;
    const char *mod_name;
    bool suppress_bind_attrs;         /* 禁止 sysfs bind/unbind */
    enum probe_type probe_type;       /* sync / async / async_driver */
    const struct of_device_id *of_match_table;
    const struct acpi_device_id *acpi_match_table;
    int  (*probe)(struct device *dev);
    void (*sync_state)(struct device *dev);
    int  (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);
    const struct attribute_group *const *groups;
    const struct attribute_group *const *dev_groups;
    const struct dev_pm_ops *pm;
    struct driver_private *p;
};
```

`driver_private` 含 `klist_devices`、`knode_bus`、`kobj`，使驱动也拥有 sysfs 目录
`/sys/bus/<bus>/drivers/<drv>`，并可提供 `bind`/`unbind` 属性。

### 2.4 `struct class`

定义于 `include/linux/device/class.h:50`。class 是“设备功能分类”，与总线正交，
典型如 `mmc_host_class`、`tty_class`、`block_class`。字段包括 `name`、`dev_groups`、
`dev_uevent()`、`devnode()`、`dev_release()`、`shutdown_pre()`、`ns_type/namespace`、`pm`。

### 2.5 `struct device_link`

定义于 `include/linux/device.h:849`：

```c
struct device_link {
    struct device *supplier;
    struct list_head s_node;
    struct device *consumer;
    struct list_head c_node;
    struct device link_dev;           /* 可选，暴露到 sysfs 的 link 设备 */
    enum device_link_state status;
    u32 flags;
    refcount_t rpm_active;
    struct kref kref;
    struct work_struct rm_work;
    bool supplier_preactivated;
};
```

状态机见 `enum device_link_state`（`include/linux/device.h:415`）：
`DL_STATE_DORMANT → AVAILABLE → CONSUMER_PROBE → ACTIVE`，供应者解绑时进入
`SUPPLIER_UNBIND`。标志位定义于同文件 437 行起：

| 标志 | 含义 |
|------|------|
| `DL_FLAG_STATELESS` | 核心不自动删除该 link |
| `DL_FLAG_AUTOREMOVE_CONSUMER` | consumer 驱动解绑时自动删除 |
| `DL_FLAG_AUTOREMOVE_SUPPLIER` | supplier 驱动解绑时自动删除 |
| `DL_FLAG_PM_RUNTIME` | 运行时 PM 强制 supplier 活跃 |
| `DL_FLAG_RPM_ACTIVE` | 建链时对 supplier 执行 `pm_runtime_get_sync()` |
| `DL_FLAG_AUTOPROBE_CONSUMER` | supplier 绑定后自动 probe consumer |
| `DL_FLAG_MANAGED` | 核心跟踪双方驱动存在性（内部） |
| `DL_FLAG_SYNC_STATE_ONLY` | 仅影响 `sync_state()` |
| `DL_FLAG_INFERRED` | 由固件/数据推断，而非驱动显式创建 |
| `DL_FLAG_CYCLE` | 检测到环形依赖 |

## 3. device 注册流程

### 3.1 `device_initialize()`（core.c:3227）

```c
dev->kobj.kset = devices_kset;
kobject_init(&dev->kobj, &device_ktype);
INIT_LIST_HEAD(&dev->dma_pools);
mutex_init(&dev->mutex);
spin_lock_init(&dev->driver_override.lock);
spin_lock_init(&dev->devres_lock);
INIT_LIST_HEAD(&dev->devres_head);
device_pm_init(dev);
set_dev_node(dev, NUMA_NO_NODE);
INIT_LIST_HEAD(&dev->links.consumers);
INIT_LIST_HEAD(&dev->links.suppliers);
INIT_LIST_HEAD(&dev->links.defer_sync);
dev->links.status = DL_DEV_NO_DRIVER;
```

只做初始化、不发布。引用计数从 1 开始，失败路径必须 `put_device()`。

### 3.2 `device_add()`（core.c:3639）

顺序固定，任一步失败按标签逆序回滚：

1. `get_device(dev)`；`dev->p` 为空则 `device_private_init()`。
2. 解析名字：`init_name` → `dev_set_name()`；否则 `bus->dev_name + id`。
3. `get_device_parent()` 算出 sysfs 父 kobject，继承 NUMA 节点。
4. `kobject_add(&dev->kobj, parent, NULL)`，设备出现在 `/sys/devices/...`。
5. `device_create_file(&dev_attr_uevent)` → class 链接 → 属性组（`device_add_attrs`）。
6. `bus_add_device()`：总线属性、`driver_override`、`/sys/bus/<bus>/devices/` 链接、
   `subsystem` 链接、挂入 `klist_devices`。
7. `dpm_sysfs_add()` + `device_pm_add()`；有 `devt` 时建 `dev` 属性并 `devtmpfs_create_node()`。
8. `bus_notify(BUS_NOTIFY_ADD_DEVICE)` → `kobject_uevent(KOBJ_ADD)`。
9. 带 fwnode 则 `fw_devlink_link_device()`，让等待该 supplier 的 consumer 建链。
10. `dev_set_ready_to_probe(dev)` 解除 probe 闸门，`bus_probe_device()` 触发匹配。
11. 挂入父设备 `klist_children` 与 class 的 `klist_devices`，回调 `class_interface->add_dev()`。

### 3.3 `device_register()` 与 `device_del()`

```c
int device_register(struct device *dev)   /* core.c:3867 */
{
    device_initialize(dev);
    return device_add(dev);
}
```

`device_del()`（core.c:3931）是逆过程：`device_pm_remove()`、`bus_remove_device()`、
`device_remove_attrs()`、删 class 链接、`kobject_del()`、`kobject_uevent(KOBJ_REMOVE)`
并释放 `dev->p`。注意 `device_del()` 之后仍需 `put_device()` 释放最后引用。

## 4. driver 注册流程

`driver_register()`（driver.c:150）：

1. `bus_is_registered(drv->bus)` 校验；总线未注册直接 `-EINVAL`。
2. 若 bus 与 driver 同时实现 probe/remove/shutdown，打印“use bus_type methods”警告。
3. `driver_find(drv->name, drv->bus)` 查重，重名返回 `-EBUSY`。
4. `bus_add_driver(drv)`。
5. `driver_add_groups(drv, drv->groups)`；失败回滚 `bus_remove_driver()`。
6. `kobject_uevent(&drv->p->kobj, KOBJ_ADD)`，并 `deferred_probe_extend_timeout()`。

`bus_add_driver()`（bus.c:733）：分配 `driver_private`、`kobject_init_and_add()`
生成 `/sys/bus/<bus>/drivers/<name>`、挂入 `sp->klist_drivers`；
`sp->drivers_autoprobe` 为真时立即 `driver_attach(drv)` 扫描未绑定设备；
随后 `module_add_driver()`、创建 `uevent`/`drv_groups`，未设 `suppress_bind_attrs`
时由 `add_bind_files()` 创建 `bind`/`unbind`。
`driver_unregister()` = `driver_remove_groups()` + `bus_remove_driver()`
（删 sysfs、`driver_detach()`、`kobject_put()`）。

## 5. bus 注册与设备挂接

`bus_register()`（bus.c:946）分配 `subsys_private`，注册 `subsys` kset，
创建 `devices`/`drivers` kset 与 `drivers_autoprobe`/`drivers_probe`/`uevent` 属性，
并挂到 `bus_kset`；`bus_unregister()` 反向清理。

`bus_probe_device()`（bus.c:612）先 `device_initial_probe(dev)`
（即 `__device_attach(dev, true)`），再遍历 `sp->interfaces` 调用
`sif->add_dev(dev, sif)`。`subsys_interface` 是总线内的“横切接口”，
例如 cpufreq 用它注册 `cpufreq_add_dev`，在 CPU 设备出现时创建 policy。
遍历辅助函数 `bus_for_each_dev()`、`bus_find_device()`、
`bus_find_device_reverse()`、`bus_for_each_drv()` 均基于 klist 并在迭代时持引用。

## 6. match 与 probe 主流程

### 6.1 match

`driver_match_device()`（base.h）：

```c
return drv->bus->match ? drv->bus->match(dev, drv) : 1;
```

即总线没有 `match` 回调时默认视为匹配，由 probe 自行判断。各总线 match 实现：
platform 依次尝试 driver_override、OF、ACPI、id_table、名字；I2C/SPI 类似；
USB 区分 device 与 interface 两套匹配。

### 6.2 `really_probe()`（dd.c:652）

```text
really_probe(dev, drv)
├─ defer_all_probes ? return -EPROBE_DEFER
├─ device_links_check_suppliers()   supplier 未 ready → -EPROBE_DEFER
├─ devres_head 非空 → -EBUSY
├─ repeat:
│  ├─ device_set_driver(dev, drv)
│  ├─ pinctrl_bind_pins(dev)        /* drivers/base/pinctrl.c */
│  ├─ bus->dma_configure(dev)
│  ├─ driver_sysfs_add(dev)
│  ├─ pm_domain->activate(dev)
│  ├─ call_driver_probe(dev, drv)   /* bus->probe 优先，否则 drv->probe */
│  ├─ device_add_groups(dev, drv->dev_groups)
│  ├─ pinctrl_init_done(dev)        /* init 态切到 default 态 */
│  └─ driver_bound(dev)
└─ 失败: driver_sysfs_remove → BUS_NOTIFY_DRIVER_NOT_BOUND
         → dma_cleanup → device_links_no_driver → device_unbind_cleanup
```

`call_driver_probe()`（dd.c:621）把 `-EPROBE_DEFER`、`-ENODEV/-ENXIO`、其他错误分别
打印后原样返回；`really_probe` 再取负，便于上层区分“probe 错误”和“框架错误”。
`device_unbind_cleanup()`（dd.c:593）依次：Rust `post_unbind_rust` →
`devres_release_all()` → 清 DMA ops/`dma_range_map` → 清 `driver` 与 drvdata →
`dev_pm_domain_detach()` → `pm_domain->dismiss()` → `pm_runtime_reinit()`。
`driver_bound()`（dd.c:446）把设备挂入 `drv->p->klist_devices`，
推进 `device_links_driver_bound()`、校验 PM 回调、移出 deferred 链表并重触发，
最后发 `BUS_NOTIFY_BOUND_DRIVER` 与 `KOBJ_BIND`。

### 6.3 从 device 侧发起：`__device_attach()`

`device_add()` → `bus_probe_device()` → `device_initial_probe()` →
`__device_attach(dev, true)`（dd.c:1070）：若已绑定直接返回；否则
`bus_for_each_drv()` 遍历驱动，回调 `__device_attach_driver()` 做
`driver_match_device()` 后 `driver_probe_device()`；遇到 `-EPROBE_DEFER`
加入 deferred 链表并停止本轮。允许异步时走
`__device_attach_async_helper()`，第一遍同步、第二遍异步。

## 7. deferred probe

延迟探测解决“设备先到、依赖的 provider 后到”问题。`struct device_private` 内嵌
`deferred_probe` 节点，全局有 `deferred_probe_pending_list` 与 `deferred_probe_mutex`。

- `driver_deferred_probe_add()` 挂入 pending 链表并记录 `deferred_probe_reason`
  （`dev_err_probe()` 写入）。
- `driver_deferred_probe_trigger()` 调度 `deferred_probe_work`，工作函数再次
  遍历 pending 设备调用 `device_attach()`。
- `deferred_probe_initcall()` 为 `late_initcall`：打开 `driver_deferred_probe_enable`、
  排空队列、置 `initcalls_done = true`；无模块时调用 `fw_devlink_drivers_done()`。
- 超时由 `driver_deferred_probe_timeout`（默认
  `CONFIG_DRIVER_DEFERRED_PROBE_TIMEOUT`，参数 `deferred_probe_timeout=` 可覆盖）
  控制，到期后再次触发并打印仍 pending 的设备。
- `driver_deferred_probe_check_state()`（dd.c:285）：无模块且 initcalls 完成
  返回 `-ENODEV`；超时为 0 返回 `-ETIMEDOUT`；否则 `-EPROBE_DEFER`。
  当前 `.config` 未开 `CONFIG_MODULES`，因此 initcall 后不会无限延迟。
- 调试入口：debugfs `devices_deferred`；新驱动注册时
  `deferred_probe_extend_timeout()` 会延长窗口。

## 8. devres（设备资源托管）

`drivers/base/devres.c:19`：

```c
struct devres {
    struct devres_node node;
    dr_release_t release;
    u8 __aligned(ARCH_DMA_MINALIGN) data[];
};
struct devres_group { struct devres_node node[2]; void *id; int color; };
```

`devres_node` 含 `entry`（挂 `dev->devres_head`）、`release`、`free_node` 与调试信息，
链表由 `dev->devres_lock` 保护。核心 API：

- `__devres_alloc_node(release, size, gfp, nid, name)`（devres.c:184）：
  `alloc_dr()` 分配 `sizeof(struct devres)+size`，清零数据区。
- `devres_add(dev, res)`（devres.c:277）：`add_dr()` 尾插到 `devres_head`。
- `devm_add_action()` → `__devm_add_action()`（devres.c:790）包装任意回调。
- `devres_release_all(dev)`（devres.c:558）：逆序执行 release 并释放节点，
  返回释放数量；组标记单独处理。
- 分组：`devres_open_group()`（600）、`devres_close_group()`、
  `devres_release_group()`；I2C probe 用组实现“只回滚本次 probe 资源”。

`devm_kzalloc()`、`devm_ioremap_resource()`、`devm_clk_get()` 等都基于此机制，
因此 `really_probe()` 要求进入 probe 时 `devres_head` 为空。

## 9. sysfs、uevent 与 class

sysfs 布局：

- `/sys/devices/...`：设备树层次（`kobject_add`）。
- `/sys/bus/<bus>/devices/<dev>`：设备链接（`bus_add_device`）。
- `/sys/bus/<bus>/drivers/<drv>/<dev>`：驱动侧反向链接（`driver_sysfs_add`）。
- `/sys/class/<class>/<dev>`：class 链接（`device_add_class_symlinks`，core.c:3487）。

`uevent` 属性由 `dev_uevent()` 汇总 `device_type->uevent`、`class->dev_uevent`、
`bus->uevent`；总线负责 `MODALIAS`（platform `platform:<name>`、I2C
`i2c:<name>`、SPI `spi:<modalias>`）。`kobject_uevent()` 经 netlink 广播；
`CONFIG_MODULES=n` 时用户态无法据此自动加载模块。

`class_register()`（class.c:178）把 class 注册为 `class_kset` 下的子系统，提供
`devnode()`（/dev 节点）、`dev_release()`、`dev_uevent()`、`shutdown_pre()`、
`namespace()/ns_type`。`device_create()` 是“分配 + 设 class + `device_register()`”
的封装，失败用 `device_destroy()` 回收。

## 10. device links

`device_link_add(consumer, supplier, flags)`（core.c:800）规则：

- 禁止自连；`DL_FLAG_STATELESS` 不能与托管标志混用。
- `DL_FLAG_RPM_ACTIVE` 必须配 `DL_FLAG_PM_RUNTIME`，建链时对 supplier
  `pm_runtime_get_sync()`。
- 非 `STATELESS` 自动补 `DL_FLAG_MANAGED`。
- supplier 必须 `device_pm_initialized()`，否则返回 NULL；非 `SYNC_STATE_ONLY`
  时用 `device_is_dependent()` 检测反向依赖/环。
- 已存在同一对 link 时合并 flags；否则新建并双向 `get_device()`。

生命周期：consumer probe 前 `device_links_check_suppliers()`（core.c:1123）
遍历 suppliers，非 `DL_STATE_AVAILABLE` 即 `-EPROBE_DEFER`（best-effort 下
inferred supplier 可 `-EAGAIN`）；成功后 `device_links_driver_bound()`
（core.c:1368）置 `DL_STATE_ACTIVE` 并可触发 `sync_state()`；probe 失败由
`device_links_no_driver()`（core.c:1522）回退；supplier 解绑进入
`DL_STATE_SUPPLIER_UNBIND`；删除用 `device_link_del()`（core.c:1032）或
`AUTOREMOVE_*` 自动完成。

`fw_devlink` 在 `device_add()` 末尾由 `fw_devlink_link_device()` 依据
DT/ACPI 的 `phandle`、`power-domains`、`supplier` 等属性创建
`DL_FLAG_INFERRED | DL_FLAG_MANAGED` 链接，provider 未就绪时自动延迟 consumer。

## 11. 与配置裁剪的关系

- 设备模型、sysfs、devres、device links 无条件编译。
- `CONFIG_MODULES=n`：驱动全部内建；deferred probe 在 initcall 结束后返回
  `-ENODEV`；uevent 的 MODALIAS 仍生成但不触发模块加载。
- `CONFIG_PINCTRL=y` 时 `device->pins` 与 `pinctrl_bind_pins()` 生效，关闭时
  相关字段/调用被条件编译，源码仍在树中。
- 当前 `.config` 关闭 `CONFIG_PM`，`dev_pm_info` 多数字段与 runtime PM 路径被
  裁剪，但 `drivers/base/power/` 源码完整存在。

## 12. 关键调用链汇总

```text
设备注册并 probe（驱动已内建）
platform_device_register() → platform_device_add() → device_add()
  ├─ bus_add_device() / device_pm_add() / kobject_uevent(KOBJ_ADD)
  ├─ fw_devlink_link_device()
  └─ bus_probe_device() → device_initial_probe()
       → __device_attach(dev, true) → bus_for_each_drv()
       → __device_attach_driver()
          ├─ driver_match_device() → bus->match()
          └─ driver_probe_device() → really_probe()
             ├─ device_links_check_suppliers()
             ├─ pinctrl_bind_pins()
             ├─ call_driver_probe() → bus->probe()/drv->probe()
             └─ driver_bound()

驱动注册并扫描已有设备
driver_register() → driver_find() → bus_add_driver()
  → driver_attach() → bus_for_each_dev(__driver_attach)
  → driver_probe_device() → really_probe()
  → kobject_uevent(KOBJ_ADD)

解绑
device_release_driver() → __device_release_driver()
  ├─ bus->remove()/drv->remove() / device_remove_groups()
  ├─ driver_sysfs_remove() / device_links_no_driver()
  └─ device_unbind_cleanup()
     ├─ devres_release_all()
     ├─ dev_pm_domain_detach()
     └─ pm_runtime_reinit()
```

## 13. 关键文件清单

| 路径 | 内容 |
|------|------|
| `include/linux/device.h` | `struct device`、`device_link`、DL 标志、生命周期 API |
| `include/linux/device/bus.h` | `struct bus_type`、`bus_attribute`、遍历/查找 API |
| `include/linux/device/driver.h` | `struct device_driver`、`driver_register()` |
| `include/linux/device/class.h` | `struct class`、class 迭代 API |
| `include/linux/device/devres.h` | `devres_node`、`devm_*` 宏与内联 |
| `drivers/base/core.c` | device 生命周期、sysfs/uevent、class 链接、device links |
| `drivers/base/driver.c` | driver 注册/注销 |
| `drivers/base/bus.c` | bus 注册、设备/驱动挂接、probe 入口 |
| `drivers/base/dd.c` | match/probe/bind、deferred probe、异步 probe |
| `drivers/base/devres.c` | devres 分配/释放/分组 |
| `drivers/base/class.c` | class 注册与释放 |
| `drivers/base/base.h` | `device_private`、`subsys_private`、内部接口 |
| `drivers/base/pinctrl.c` | `pinctrl_bind_pins()` 在 probe 前绑定引脚状态 |
| `drivers/base/init.c` | `driver_init()` 初始化设备模型各子系统 |
