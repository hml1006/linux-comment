# PCIe 总线注册、驱动注册、设备枚举完整流程分析

## 一、总体概览

PCIe 子系统初始化分为以下 4 个阶段：

| 阶段 | 功能 | 关键函数 | 调用层级 |
|------|------|----------|----------|
| **1. 总线子系统注册** | 注册 `pci_bus_type` 到内核驱动模型 | `pci_driver_init()` | `postcore_initcall` |
| **2. 主机控制器初始化** | 发现 Host Bridge 设备，创建 Root Bus | `acpi_pci_root_add()` / `pci_host_probe()` | `arch_initcall` / `module_init` |
| **3. 设备枚举** | 扫描 PCI 总线树，发现所有设备 | `pci_scan_child_bus_extend()` | 阶段 2 内部 |
| **4. 驱动注册与绑定** | 注册 PCI 驱动，匹配设备，执行 probe | `__pci_register_driver()` | `module_init` |

---

## 二、关键数据结构详解

### 2.1 `struct pci_host_bridge` — 主机桥

**定义位置**: [include/linux/pci.h:627](file:///home/louis/code/linux/include/linux/pci.h#L627)

```c
struct pci_host_bridge {
    struct device   dev;            // 内核设备模型中的 device 对象
    struct pci_bus  *bus;           // 指向该桥管理的 root bus
    struct pci_ops  *ops;           // 配置空间访问操作函数集
    struct pci_ops  *child_ops;     // 子设备配置空间访问操作（通常同 ops）
    void            *sysdata;       // 平台特定数据
    int             busnr;          // 根总线号
    int             domain_nr;      // PCI Domain 号
    struct list_head windows;       // 资源窗口链表（resource_entry）
    struct list_head dma_ranges;    // DMA 范围资源链表

    /* 平台回调函数 */
    u8 (*swizzle_irq)(struct pci_dev *, u8 *);
    int (*map_irq)(const struct pci_dev *, u8, u8);

    /* 功能/特性标志位 */
    unsigned int    native_aer:1;           // OS 控制 PCIe AER
    unsigned int    native_pcie_hotplug:1;  // OS 控制 PCIe 热插拔
    unsigned int    native_pme:1;           // OS 控制 PCIe PME
    unsigned int    native_ltr:1;           // OS 控制 PCIe LTR
    unsigned int    native_dpc:1;           // OS 控制 PCIe DPC
    unsigned int    preserve_config:1;      // 保留固件资源设置
    unsigned int    msi_domain:1;           // 需要 MSI domain

    unsigned long   private[];              // 驱动私有数据（变长数组）
};
```

**生命周期变化**：

| 阶段 | 关键字段变化 |
|------|-------------|
| 分配阶段 (`pci_alloc_host_bridge`) | 所有字段为 0，`native_*` 标志默认置 1，`dev.type` 设为 `pci_host_bridge_type` |
| 初始化阶段 (`pci_host_common_probe` / `acpi_pci_root_create`) | `ops` 设为 ECAM 操作，`busnr` 设总线号，`windows` 添加资源窗口 |
| 注册阶段 (`pci_register_host_bridge`) | `bus` 指向分配的 `pci_bus`，`sysdata` 传递给 `bus->sysdata`，`domain_nr` 被赋值 |

---

### 2.2 `struct pci_bus` — PCI 总线

**定义位置**: [include/linux/pci.h:698](file:///home/louis/code/linux/include/linux/pci.h#L698)

```c
struct pci_bus {
    struct list_head node;          // 全局 pci_root_buses 链表节点
    struct pci_bus  *parent;        // 父总线（root bus 为 NULL）
    struct list_head children;      // 子总线链表
    struct list_head devices;       // 总线上设备链表
    struct pci_dev  *self;          // 桥设备自身（root bus 为 NULL）
    struct list_head slots;         // 槽位链表

    struct resource *resource[PCI_BRIDGE_RESOURCE_NUM]; // 桥窗口资源
    struct list_head resources;     // 地址空间资源链表
    struct resource busn_res;       // 总线号范围

    struct pci_ops  *ops;           // 配置空间访问函数
    void            *sysdata;       // 平台数据

    unsigned char   number;         // 本总线号
    unsigned char   primary;        // 主总线号（桥的 primary side）
    unsigned char   max_bus_speed;  // 最大总线速度
    unsigned char   cur_bus_speed;  // 当前总线速度
    int             domain_nr;      // Domain 号

    char            name[48];       // 总线名称
    struct device   *bridge;        // 指向 bridge device
    struct device   dev;            // 内核设备模型对象
    unsigned int    is_added:1;     // 是否已添加到设备模型
};
```

**生命周期变化**：

| 阶段 | 关键字段变化 |
|------|-------------|
| 分配阶段 (`pci_alloc_bus`) | `node/children/devices/slots/resources` 链表头初始化，`max/cur_bus_speed` 设为 `PCI_SPEED_UNKNOWN` |
| 注册阶段 (`pci_register_host_bridge`) | `number` 设总线号，`ops` 设配置访问函数，`sysdata` 从 bridge 传入，`dev.class` 设 `pcibus_class`，`device_register()` 注册到内核 |
| 枚举阶段 (`pci_scan_child_bus_extend`) | `devices` 链表添加发现的设备，`children` 链表添加发现的子总线 |
| 添加阶段 (`pci_bus_add_devices`) | `is_added` 置 1 |

---

### 2.3 `struct pci_dev` — PCI 设备

**定义位置**: [include/linux/pci.h:343](file:///home/louis/code/linux/include/linux/pci.h#L343)

```c
struct pci_dev {
    struct list_head bus_list;      // 挂入 bus->devices 链表
    struct pci_bus  *bus;           // 所在总线
    struct pci_bus  *subordinate;   // 桥设备指向的下级总线

    unsigned int    devfn;          // 编码的设备号和功能号 (PCI_DEVFN)
    unsigned short  vendor;         // Vendor ID
    unsigned short  device;         // Device ID
    unsigned short  subsystem_vendor; // Subsystem Vendor ID
    unsigned short  subsystem_device; // Subsystem Device ID
    unsigned int    class;          // 分类码 (base class, sub class, prog-if)
    u8              revision;       // 修订版本
    u8              hdr_type;       // Header 类型 (0=普通, 1=桥, 2=CardBus)

    /* 能力指针 */
    u8              pcie_cap;       // PCIe 能力偏移
    u8              msi_cap;        // MSI 能力偏移
    u8              msix_cap;       // MSI-X 能力偏移
    u16             aer_cap;        // AER 能力偏移

    struct pci_driver *driver;      // 已绑定的驱动

    pci_power_t     current_state;  // 当前电源状态 (D0-D3)
    unsigned int    irq;            // 中断号
    struct resource resource[DEVICE_COUNT_RESOURCE]; // BAR 资源 (6 个标准 BAR + ROM)

    /* 标志位 */
    unsigned int    multifunction:1;    // 多功能设备
    unsigned int    is_busmaster:1;     // 总线主控
    unsigned int    is_physfn:1;        // 物理功能 (PF)
    unsigned int    is_virtfn:1;        // 虚拟功能 (VF)
    unsigned int    is_hotplug_bridge:1;// 热插拔桥
    unsigned int    is_probed:1;        // 正在探测驱动

    struct device   dev;            // 内核设备模型对象
};
```

**生命周期变化**：

| 阶段 | 关键字段变化 |
|------|-------------|
| 扫描阶段 (`pci_scan_device`) | `vendor/device/class/revision/hdr_type` 从配置空间读取，`devfn` 设为扫描参数 |
| 设置阶段 (`pci_setup_device`) | `resource[]` 通过 `pci_read_bases()` 读取 BAR 值，`irq` 通过 `pci_read_irq()` 读取，`subsystem_vendor/device` 读取 |
| 添加阶段 (`pci_device_add`) | `bus` 指向所在总线，`pci_init_capabilities()` 读取所有能力寄存器（MSI/PM/AER/ATS/IOV 等），`device_add()` 注册到设备模型 |
| 能力初始化 (`pci_init_capabilities`) | `pcie_cap/msi_cap/msix_cap/pm_cap/aer_cap/ats_cap` 等能力偏移地址被填充 |
| 驱动绑定后 | `driver` 指向匹配的 `pci_driver` |

---

### 2.4 `struct pci_driver` — PCI 驱动

**定义位置**: [include/linux/pci.h:1019](file:///home/louis/code/linux/include/linux/pci.h#L1019)

```c
struct pci_driver {
    const char              *name;           // 驱动名称
    const struct pci_device_id *id_table;    // 支持的设备 ID 表（必须非空）

    /* 回调函数 */
    int  (*probe)(struct pci_dev *dev, const struct pci_device_id *id);  // 设备探测
    void (*remove)(struct pci_dev *dev);     // 设备移除
    int  (*suspend)(struct pci_dev *dev, pm_message_t state);
    int  (*resume)(struct pci_dev *dev);
    void (*shutdown)(struct pci_dev *dev);

    /* SR-IOV 相关 */
    int  (*sriov_configure)(struct pci_dev *dev, int num_vfs);
    int  (*sriov_set_msix_vec_count)(struct pci_dev *vf, int msix_vec_count);

    const struct pci_error_handlers *err_handler;
    struct device_driver    driver;          // 内核驱动模型基类
    struct pci_dynids       dynids;          // 动态 ID 链表（sysfs new_id 添加）
    bool driver_managed_dma;
};
```

---

### 2.5 `struct pci_device_id` — 设备匹配 ID

**定义位置**: [include/linux/mod_devicetable.h:44](file:///home/louis/code/linux/include/linux/mod_devicetable.h#L44)

```c
struct pci_device_id {
    __u32 vendor, device;           // Vendor ID 和 Device ID（或 PCI_ANY_ID）
    __u32 subvendor, subdevice;     // 子系统 Vendor/Device ID
    __u32 class, class_mask;        // 分类码及掩码
    kernel_ulong_t driver_data;     // 驱动私有数据
    __u32 override_only;            // 仅匹配 driver_override
};
```

匹配规则：`pci_match_one_device()` 逐字段比较，支持 `PCI_ANY_ID`（`~0`）通配。

---

### 2.6 `struct pci_ops` — 配置空间访问操作

**定义位置**: [include/linux/pci.h:870](file:///home/louis/code/linux/include/linux/pci.h#L870)

```c
struct pci_ops {
    int (*add_bus)(struct pci_bus *bus);         // 总线添加时回调
    void (*remove_bus)(struct pci_bus *bus);     // 总线移除时回调
    void __iomem *(*map_bus)(struct pci_bus *bus, unsigned int devfn, int where); // 映射配置空间
    int (*read)(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val);  // 读配置
    int (*write)(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 val);  // 写配置
};
```

---

### 2.7 `struct pci_slot` — PCI 槽位

**定义位置**: [include/linux/pci.h:76](file:///home/louis/code/linux/include/linux/pci.h#L76)

```c
struct pci_slot {
    struct pci_bus      *bus;       // 槽位所在总线
    struct list_head    list;       // 挂入 bus->slots 链表
    struct hotplug_slot *hotplug;   // 热插拔信息
    unsigned char       number;     // 槽位号 (PCI_SLOT(devfn))
    struct kobject      kobj;
};
```

---

## 三、数据结构关系图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                       数据结构关联关系                                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  pci_host_bridge                                                           │
│  ┌──────────────────────────────────────┐                                  │
│  │ .bus ─────────────────────────────┐  │                                  │
│  │ .ops                             │  │                                  │
│  │ .windows (resource list)         │  │                                  │
│  │ .dev (device)                    │  │                                  │
│  └──────────────────────────────────┼──┘                                  │
│                                     │                                      │
│                   ┌─────────────────┘                                      │
│                   ▼                                                         │
│  pci_bus (root bus)                                                        │
│  ┌──────────────────────────────────────┐                                  │
│  │ .parent = NULL                       │                                  │
│  │ .self = NULL                         │                                  │
│  │ .children  ──────┐                  │                                  │
│  │ .devices   ──────┼──────┐           │                                  │
│  │ .number = 0x00   │      │           │                                  │
│  │ .ops = bridge->ops│      │           │                                  │
│  │ .dev (device)     │      │           │                                  │
│  └───────────────────┼──────┼───────────┘                                  │
│                      │      │                                              │
│         ┌────────────┘      │                                              │
│         ▼                    │                                              │
│  pci_bus (child bus)        │                                              │
│  ┌───────────────────────┐  │                                              │
│  │ .parent = &root_bus   │  │                                              │
│  │ .self = &bridge_dev   │  │                                              │
│  │ .children = ...       │  │              pci_dev (普通设备)               │
│  │ .devices = ...        │  │    ┌──────────────────────────────┐          │
│  │ .number = 0x01        │  │    │ .bus = &root_bus             │          │
│  │ .primary = 0x00       │  │    │ .devfn = 0x00 (Dev 0, Fn 0)  │          │
│  └───────────────────────┘  │    │ .vendor = 0x8086             │          │
│                             │    │ .device = 0x1234             │          │
│  pci_dev (桥设备)            │    │ .class = 0x020000           │          │
│  ┌──────────────────────────┘    │ .resource[0] = BAR0 值      │          │
│  │ .bus = &root_bus              │ .irq = 16                   │          │
│  │ .devfn = 0x00                 │ .driver = &some_driver      │          │
│  │ .hdr_type = 0x01 (桥)         │ .dev (device)               │          │
│  │ .subordinate = &child_bus     └──────────────────────────────┘          │
│  └───────────────────────────────                                        │
│                                                                             │
│  pci_driver                                                                │
│  ┌──────────────────────────────────────┐                                  │
│  │ .name = "e1000"                      │                                  │
│  │ .id_table = { { 0x8086, 0x1234, ... } }                               │
│  │ .probe = e1000_probe                │                                  │
│  │ .driver.bus = &pci_bus_type         │                                  │
│  └──────────────────────────────────────┘                                  │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 四、阶段 1：总线子系统注册

### 4.1 流程

```
postcore_initcall (优先级 2)
    │
    ├── pcibus_class_init()              [drivers/pci/probe.c:114]
    │   └── class_register(&pcibus_class)
    │       └── 创建 /sys/class/pci_bus/ 类
    │
    └── pci_driver_init()                [drivers/pci/pci-driver.c:1747]
        ├── alloc_workqueue("pcie_probe_wq")  ← 创建异步 probe 专用工作队列
        │
        ├── bus_register(&pci_bus_type)   ← 注册 PCI 总线类型
        │   ├── 创建 /sys/bus/pci/
        │   ├── 设置 .match  = pci_bus_match
        │   ├── 设置 .probe  = pci_device_probe
        │   ├── 设置 .remove = pci_device_remove
        │   ├── 设置 .uevent = pci_uevent
        │   └── 设置 .dma_configure = pci_dma_configure
        │
        └── bus_register(&pcie_port_bus_type)  ← 注册 PCIe 端口总线类型
```

### 4.2 `pci_bus_type` 定义

```c
// drivers/pci/pci-driver.c:1726
struct bus_type pci_bus_type = {
    .name           = "pci",
    .match          = pci_bus_match,       // 设备-驱动匹配
    .probe          = pci_device_probe,    // 驱动探测
    .remove         = pci_device_remove,   // 设备移除
    .shutdown       = pci_device_shutdown,
    .dev_uevent     = pci_uevent,
    .dev_groups     = pci_dev_groups,
    .bus_groups     = pci_bus_groups,
    .drv_groups     = pci_drv_groups,
    .dma_configure  = pci_dma_configure,
    .pm             = PCI_PM_OPS_PTR,
    .num_vf         = pci_bus_num_vf,
    .force_dma_unset = pci_dma_unset,
};
```

### 4.3 数据结构变化

```
注册前: 内核中无 pci 总线类型
注册后:
  pci_bus_type = {
      .name = "pci",
      .p = &subsys_private {
          .drivers_autoprobe = 1,    // 自动 probe 使能
          .klist_devices = {},       // 设备链表（空）
          .klist_drivers = {},       // 驱动链表（空）
      }
  }
  /sys/bus/pci/ 目录创建
```

---

## 五、阶段 2：主机控制器初始化

### 5.1 ACPI 路径

```
arch_initcall (优先级 3)
    │
    └── acpi_pci_init()                       [drivers/pci/pci-acpi.c:1516]
        └── acpi_pci_root_init()              [drivers/acpi/pci_root.c:1061]
            └── acpi_scan_add_handler_with_hotplug(&pci_root_handler, "pci_root")
                └── ACPI 扫描到 PNP0A08/PNP0A03 设备时 →
                    acpi_pci_root_add()        [drivers/acpi/pci_root.c:880]
                        │
                        ├── acpi_pci_root_create()  [drivers/acpi/pci_root.c:1000]
                        │   ├── 准备资源 (IO/MEM 窗口)
                        │   ├── pci_create_root_bus(NULL, busnum, ops, sysdata, &resources)
                        │   │   └── pci_register_host_bridge()  ← 注册 bridge
                        │   └── pci_scan_child_bus()            ← 枚举设备
                        │
                        └── pci_bus_add_devices()  ← 添加设备，触发 probe
```

### 5.2 Device Tree 路径

```
module_init (优先级 6)
    │
    └── platform_driver_register(&pci_host_common_driver)
        └── of_match_table 匹配 DTS 节点 →
            pci_host_common_probe()             [drivers/pci/controller/pci-host-common.c:85]
                │
                ├── devm_pci_alloc_host_bridge()     ← 分配初始化 bridge
                ├── pci_host_common_init()           [pci-host-common.c:60]
                │   ├── pci_host_common_ecam_create()  ← 创建 ECAM 配置空间映射
                │   │   └── pci_ecam_create()           ← ioremap 配置空间物理地址
                │   └── bridge->ops = &pci_ecam_ops   ← 设置 ECAM 读写操作
                │
                └── pci_host_probe(bridge)            [drivers/pci/probe.c:3271]
                    ├── pci_scan_root_bus_bridge(bridge)  ← 注册 + 枚举
                    │   ├── pci_register_host_bridge()   ← 注册 bridge
                    │   └── pci_scan_child_bus()          ← 递归枚举
                    │
                    ├── pci_bus_claim_resources(bus)      ← 声明固件资源
                    ├── pci_assign_unassigned_root_bus_resources(bus)  ← 分配未分配资源
                    │
                    └── pci_bus_add_devices(bus)          ← 添加设备，触发 probe
```

### 5.3 `pci_host_probe()` 完整流程

```c
// drivers/pci/probe.c:3271
int pci_host_probe(struct pci_host_bridge *bridge)
{
    struct pci_bus *bus, *child;
    int ret;

    // 1. 注册 Host Bridge 并扫描总线树
    ret = pci_scan_root_bus_bridge(bridge);
    if (ret < 0)
        return ret;

    bus = bridge->bus;

    // 2. 声明固件已配置的资源
    pci_bus_claim_resources(bus);

    // 3. 分配未被固件配置的资源
    pci_assign_unassigned_root_bus_resources(bus);

    // 4. 添加所有设备，触发 driver 绑定
    pci_bus_add_devices(bus);

    // 5. 递归处理子总线
    list_for_each_entry(child, &bus->children, node)
        pci_bus_add_devices(child);

    return 0;
}
```

### 5.4 数据结构变化

```
初始化前:
  pci_host_bridge = { .bus = NULL, .ops = NULL, .busnr = 0, ... }

pci_alloc_host_bridge 后:
  pci_host_bridge = {
      .dev.type = &pci_host_bridge_type,
      .native_aer = 1, .native_pcie_hotplug = 1, ...
      .windows = LIST_HEAD_INIT,
      .dma_ranges = LIST_HEAD_INIT,
  }

pci_register_host_bridge 后:
  pci_host_bridge = {
      .bus = &pci_bus {                ← 新分配的 pci_bus
          .number = 0x00,              ← bridge->busnr
          .ops = &pci_ecam_ops,        ← bridge->ops
          .sysdata = bridge->sysdata,
          .dev.class = &pcibus_class,
          .dev.parent = &bridge->dev,
          .name = "0000:00",
          .node → 已加入 pci_root_buses 链表
      },
      .dev → 已注册到设备模型
  }
```

---

## 六、阶段 3：PCIe 设备枚举

### 6.1 枚举入口

```
pci_scan_root_bus_bridge(bridge)                          [probe.c:3254]
    └── pci_register_host_bridge(bridge)                  ← 先注册 bridge
        └── pci_scan_child_bus(bus)                       ← 开始枚举
            └── pci_scan_child_bus_extend(bus, 0)
```

### 6.2 核心枚举函数 `pci_scan_child_bus_extend()`

**源码位置**: [drivers/pci/probe.c:3080](file:///home/louis/code/linux/drivers/pci/probe.c#L3080)

```
pci_scan_child_bus_extend(bus, available_buses)
    │
    ├── [阶段A: 扫描所有设备/功能号]
    │   for (devnr = 0; devnr < PCI_MAX_NR_DEVS; devnr++)
    │       pci_scan_slot(bus, PCI_DEVFN(devnr, 0))
    │
    ├── [SR-IOV 预留总线号]
    │   used_buses = pci_iov_bus_range(bus)
    │   max += used_buses
    │
    ├── [架构相关修复]
    │   pcibios_fixup_bus(bus)
    │   bus->is_added = 1
    │
    ├── [统计热插拔桥和普通桥数量]
    │   for_each_pci_bridge(dev, bus)
    │       if (dev->is_hotplug_bridge) hotplug_bridges++
    │       else normal_bridges++
    │
    ├── [Pass 0: 扫描BIOS已配置的桥]
    │   for_each_pci_bridge(dev, bus)
    │       max = pci_scan_bridge_extend(bus, dev, max, 0, 0)   ← pass=0
    │
    ├── [Pass 1: 扫描需要重新配置的桥]
    │   for_each_pci_bridge(dev, bus)
    │       if (唯一桥)  buses = available_buses
    │       else if (热插拔桥) buses = available_buses / hotplug_bridges
    │       max = pci_scan_bridge_extend(bus, dev, cmax, buses, 1)  ← pass=1
    │
    └── return max
```

### 6.3 `pci_scan_slot()` — 扫描单个槽位

**源码位置**: [drivers/pci/probe.c:2866](file:///home/louis/code/linux/drivers/pci/probe.c#L2866)

```
pci_scan_slot(bus, devfn)
    │
    ├── [仅一个子设备限制]
    │   if (only_one_child(bus) && (devfn > 0))
    │       return 0  ← 已扫描过整个槽位
    │
    ├── do {
    │   ├── pci_scan_single_device(bus, devfn + fn)  ← 扫描 Function fn
    │   │
    │   ├── if (dev && fn > 0)
    │   │       dev->multifunction = 1  ← 标记为多功能设备
    │   │
    │   ├── if (fn == 0 && !dev && !hypervisor_isolated_pci_functions())
    │   │       break  ← Function 0 不存在，跳过整个槽位
    │   │
    │   └── fn = next_fn(bus, dev, fn)  ← 获取下一个 Function 号
    │       ├── 如果 ARI 使能: next_ari_fn()  ← 读取 ARI Capability 的 Next Function
    │       ├── 否则: fn + 1 (最多 7)
    │       └── 如果非多功能设备且 fn > 0: 返回 -ENODEV
    │
    └── } while (fn >= 0)
```

### 6.4 `pci_scan_single_device()` — 扫描单个设备

**源码位置**: [drivers/pci/probe.c:2787](file:///home/louis/code/linux/drivers/pci/probe.c#L2787)

```
pci_scan_single_device(bus, devfn)
    │
    ├── pci_get_slot(bus, devfn)           ← 检查设备是否已存在
    │   └── 遍历 bus->devices 链表，匹配 devfn
    │   └── 如果已存在 → pci_dev_put() 后直接返回
    │
    ├── pci_scan_device(bus, devfn)        ← 读取配置空间创建设备
    │   ├── pci_bus_read_dev_vendor_id(bus, devfn, &l, 60*1000)
    │   │   └── 通过 pci_ops->map_bus 映射配置空间地址
    │   │   └── 读取 Config[0x00] Vendor/Device ID 寄存器
    │   │   └── 读到 0xFFFFFFFF → 设备不存在，返回 NULL
    │   │   └── 支持 RRS (Request Retry Status) 机制，最多等待 60 秒
    │   │
    │   ├── pci_alloc_dev(bus)             ← 分配 struct pci_dev
    │   │   └── kzalloc(sizeof(*dev))
    │   │   └── 初始化链表头、spinlock 等
    │   │
    │   └── pci_setup_device(dev)          ← 读取配置空间关键信息
    │       ├── 读取 vendor/device/class/revision/hdr_type
    │       ├── 设置 pcie_port_type (PCIe 设备类型)
    │       ├── 设置 dev->dma_mask = 0xffffffff
    │       ├── 设置 dev_name = "%04x:%02x:%02x.%d"
    │       │
    │       ├── 根据 hdr_type 做不同处理:
    │       │
    │       │   0x00 (PCI_HEADER_TYPE_NORMAL/普通设备):
    │       │       ├── pci_read_irq(dev)          ← Config[0x3C] IRQ Line
    │       │       ├── pci_read_bases(dev, 6, PCI_ROM_ADDRESS1)
    │       │       │   └── 读取 Config[0x10~0x24] 6 个 BAR 寄存器
    │       │       │   └── 每个 BAR: 读原值 → 写全1 → 读回大小 → 恢复原值
    │       │       └── 读取 subsystem_vendor/device
    │       │
    │       │   0x01 (PCI_HEADER_TYPE_BRIDGE/PCI桥):
    │       │       ├── pci_read_irq(dev)
    │       │       ├── pci_read_bases(dev, 2, PCI_ROM_ADDRESS1)  ← 2 个 BAR
    │       │       ├── pci_read_bridge_windows(dev)
    │       │       │   ├── 读 Config[0x18] Primary/Secondary/Subordinate Bus
    │       │       │   ├── 读 Config[0x1C] I/O Base/Limit
    │       │       │   ├── 读 Config[0x20] Memory Base/Limit
    │       │       │   └── 读 Config[0x24] Prefetchable Memory Base/Limit
    │       │       ├── dev->transparent = (class & 0xff == 1)
    │       │       └── set_pcie_hotplug_bridge(dev)
    │       │
    │       │   0x02 (PCI_HEADER_TYPE_CARDBUS):
    │       │       ├── pci_read_irq(dev)
    │       │       ├── pci_read_bases(dev, 1, 0)
    │       │       └── 读取 subsystem_vendor/device
    │       │
    │       └── 返回 0 成功
    │
    └── pci_device_add(dev, bus)           ← 添加到总线
        ├── pci_configure_device(dev)      ← 配置 MPS/扩展标签/LTR/ASPM 等
        ├── device_initialize(&dev->dev)
        ├── pci_fixup_device(pci_fixup_header, dev)  ← 修复头部
        ├── pci_init_capabilities(dev)     ← 读取所有能力寄存器 (17 种)
        ├── list_add_tail(&dev->bus_list, &bus->devices)  ← 加入总线设备链表
        └── device_add(&dev->dev)          ← 注册到设备模型
```

### 6.5 `pci_init_capabilities()` — 能力初始化

**源码位置**: [drivers/pci/probe.c:2280](file:///home/louis/code/linux/drivers/pci/probe.c#L2280)

```c
static void pci_init_capabilities(struct pci_dev *dev)
{
    pci_ea_init(dev);              // Enhanced Allocation
    pci_pm_init(dev);              // Power Management
    pci_pcie_cap(dev);             // PCIe 能力寄存器
    pci_configure_ari(dev);        // Alternative Routing-ID Interpretation
    pci_af_init(dev);              // Alternate Address (扩展功能)
    pci_atomic_ops_init(dev);     // Atomic Operations
    pci_mmio_always_on(dev);      // MMIO Always On
    pci_vpd_init(dev);             // Vital Product Data
    pci_iov_init(dev);             // SR-IOV
    pci_acs_init(dev);             // Access Control Services
    pci_ptm_init(dev);             // Precision Time Measurement
    pci_aer_init(dev);             // Advanced Error Reporting
    pci_dpc_init(dev);             // Downstream Port Containment
    pci_rcec_init(dev);            // RCEC (Root Complex Event Collector)
    pci_ats_init(dev);             // Address Translation Services
    pci_pri_init(dev);             // Page Request Interface
    pci_pasid_init(dev);           // Process Address Space ID
    pci_flr_wait(dev);             // Function Level Reset
    pci_notify_init(dev);          // 通知
    pci_register_notify(dev);      // 注册通知回调
}
```

### 6.6 数据结构变化（枚举过程）

```
枚举前: 只有 pci_host_bridge 和 root bus
  pci_bus = {
      .devices = LIST_HEAD_INIT,    // 空链表
      .children = LIST_HEAD_INIT,   // 空链表
      .number = 0x00,
  }

扫描到设备后:
  pci_dev = {
      .bus = &root_bus,
      .devfn = PCI_DEVFN(0, 0),       // Device 0, Function 0
      .vendor = 0x8086,
      .device = 0x1234,
      .class = 0x060000,              // Bridge device
      .hdr_type = 0x01,               // PCI-to-PCI Bridge
      .resource[0] = { .start = 0xE0000000, .end = 0xEFFFFFFF, .flags = IORESOURCE_MEM },
      .resource[1] = { .start = 0x1000, .end = 0x1FFF, .flags = IORESOURCE_IO },
      .pcie_cap = 0x40,               // PCIe 能力在配置空间偏移 0x40
      .msi_cap = 0x50,                // MSI 能力在偏移 0x50
  }
  pci_bus.devices = [dev0, ...]       // 设备已加入链表

扫描桥设备后:
  pci_bus (子总线) = {
      .parent = &root_bus,
      .self = &bridge_dev,            // 指向桥设备
      .number = 0x01,                 // 次级总线号
      .primary = 0x00,                // 主总线号
      .devices = [child_dev0, ...]
  }
  pci_bus.children = [child_bus, ...] // 子总线已加入链表

  bridge_dev.subordinate = &child_bus // 桥设备的 subordinate 指向子总线
```

### 6.7 硬件 RC 配置空间初始化（ECAM/MCFG）

PCIe 使用 **ECAM (Enhanced Configuration Access Mechanism)** 访问配置空间，将配置空间映射到内存地址空间。

#### 6.7.1 ECAM 地址映射公式

```
配置空间物理地址 = ECAM基地址 + (Bus号 << 20) + (Device号 << 15) + (Function号 << 12) + 寄存器偏移

即:  addr = base + (bus << 20) | (dev << 15) | (fn << 12) | offset

其中:
  - bus_shift = 20 (每个总线占用 1MB 地址空间)
  - devfn_shift = 12 (每个 Function 占用 4KB 地址空间)
  - 标准配置空间: 256 字节 (偏移 0x00~0xFF)
  - 扩展配置空间: 4KB (偏移 0x00~0xFFF, PCIe 设备支持)
```

#### 6.7.2 ACPI 路径（MCFG 表解析）

```
ACPI 固件提供 MCFG (Memory Mapped Configuration Space) 表
    │
    └── pci_mcfg_lookup(root, &cfgres, &ecam_ops)   [drivers/acpi/pci_mcfg.c]
        ├── 遍历 MCFG 表中的所有 Entry
        ├── 匹配 Segment Group Number 和 Bus Range
        ├── 返回 cfgres (ECAM 物理地址范围) 和 ecam_ops
        └── 平台特定 quirk 可能替换 ops (如 pci_32b_ops)
    │
    └── pci_ecam_create(dev, &cfgres, bus_res, ecam_ops)  [drivers/pci/ecam.c:27]
        ├── 计算 bus_range = resource_size(&cfg->busr)
        ├── 计算 bsz = 1 << bus_shift (通常 1MB)
        ├── claim ECAM 内存区域 (request_resource_conflict)
        ├── pci_remap_cfgspace(cfgres->start, bus_range * bsz)  ← ioremap
        ├── 调用 ops->init(cfg)  ← 平台特定初始化
        └── 返回 cfg 包含 { .win = iomem 基址, .busr = 总线范围 }
    │
    └── ri->cfg = ...  ← 存入 acpi_pci_generic_root_info
```

#### 6.7.3 Device Tree 路径

```
DTS 节点 (如 "pci-host-ecam-generic")
    │
    └── pci_host_common_probe()                    [pci-host-common.c:85]
        ├── devm_pci_alloc_host_bridge()           ← 分配 bridge
        ├── pci_host_common_ecam_create(dev, bridge, &pci_generic_ecam_ops)
        │   ├── of_get_address() 解析 DTS 的 reg 属性
        │   ├── of_pci_get_bus_range() 获取总线范围
        │   └── pci_ecam_create() 映射配置空间
        └── bridge->sysdata = cfg
        └── bridge->ops = &pci_ecam_ops
```

#### 6.7.4 `pci_ecam_map_bus()` — 配置空间访问核心

**源码位置**: [drivers/pci/ecam.c:126](file:///home/louis/code/linux/drivers/pci/ecam.c#L126)

```c
void __iomem *pci_ecam_map_bus(struct pci_bus *bus, unsigned int devfn,
                                int where)
{
    struct pci_config_window *cfg = bus->sysdata;
    unsigned int bus_shift = cfg->ops->bus_shift;  // 通常 20
    unsigned int devfn_shift = cfg->ops->bus_shift - 8;  // 12
    unsigned int busn = bus->number;
    void __iomem *base;

    if (busn < cfg->busr.start || busn > cfg->busr.end)
        return NULL;

    busn -= cfg->busr.start;
    base = cfg->win;  // ECAM 映射基址

    // addr = base + (bus << 20) | (devfn << 12) | where
    return base + PCIE_ECAM_OFFSET(busn, devfn, where);
}
```

#### 6.7.5 硬件 RC 寄存器变化

```
ECAM 映射前:
  PCIe RC 配置空间物理地址 → 由固件在 MCFG 表中描述

ECAM 映射后:
  RC 的配置空间通过 MMIO 可访问:
    virt_addr = ioremap(phys_base, size)
    通过 pci_ecam_map_bus() 对任意 Bus/Dev/Fn 生成访存地址

Root Bus 配置空间读:
  pci_generic_config_read(bus, devfn, where, size, val)
    → pci_ecam_map_bus(bus, devfn, where)  ← 生成 MMIO 地址
    → readb/readw/readl(virt_addr)          ← 直接读内存映射
    → 返回读取值

Root Port 硬件初始化 (以 mvebu 为例):
  PCIE_CTRL_OFF |= RC_MODE          ← 设置 RC 模式
  PCI_EXP_LNKCAP &= ~MLW           ← 设置链路宽度 (x1/x4)
  PCI_COMMAND &= ~(IO|MEM|MASTER)  ← 禁用 IO/MEM/主控
  Class Code → 0x060400 (PCI Bridge) ← 修正分类码
```

### 6.8 PCIe Switch 拓扑分析

PCIe Switch 内部包含：
- **1 个 Upstream Port**：面向 Root Complex 方向
- **N 个 Downstream Port**：面向 Endpoint 方向
- 内部虚拟 PCI 总线连接 Upstream 和 Downstream 端口

#### 6.8.1 Switch 拓扑枚举流程

```
                    ┌──────────────────┐
                    │   Root Complex   │
                    │    Bus 0x00      │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  Upstream Port   │  ← PCIe Switch (hdr_type=0x01, bridge)
                    │  Bus 0x00→0x01   │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  Internal Bus    │  ← Bus 0x01 (虚拟总线)
                    └───┬───┬────┬─────┘
                        │   │    │
          ┌─────────────┘   │    └──────────────┐
          ▼                 ▼                   ▼
   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
   │Downstream 1  │  │Downstream 2  │  │Downstream 3  │  ← 各 Downstream Port
   │Bus 0x01→0x02 │  │Bus 0x01→0x03 │  │Bus 0x01→0x04 │
   └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
          │                 │                 │
          ▼                 ▼                 ▼
   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
   │  Endpoint    │  │  Endpoint    │  │  Endpoint    │
   │  Bus 0x02    │  │  Bus 0x03    │  │  Bus 0x04    │
   └──────────────┘  └──────────────┘  └──────────────┘
```

#### 6.8.2 Switch 枚举过程详解

```
pci_scan_child_bus_extend(Bus 0x00, ...)
    │
    ├── 扫描 Bus 0x00 上的设备:
    │   ├── Dev 0, Fn 0 → Upstream Port (hdr_type=0x01, bridge)
    │   │   ├── pci_setup_device() 读取配置空间
    │   │   │   ├── hdr_type = 0x01 (PCI-to-PCI Bridge)
    │   │   │   ├── pci_read_bridge_windows() 读 Bus 号、窗口
    │   │   │   └── set_pcie_port_type() → PCI_EXP_TYPE_UPSTREAM
    │   │   │
    │   │   └── pci_device_add() → 加入 bus->devices
    │   │
    │   └── Dev X, Fn Y → 其他设备...
    │
    ├── [Pass 0] 扫描桥设备:
    │   └── Upstream Port → pci_scan_bridge_extend(Bus 0x00, dev, 0, 0, 0)
    │       │
    │       ├── 读 Config[0x18] Primary/Secondary/Subordinate
    │       │   = 0x00 0x01 0x04 (BIOS 已配置)
    │       │
    │       ├── pci_add_new_bus(Bus 0x00, dev, 0x01) → Bus 0x01
    │       │
    │       └── pci_scan_child_bus_extend(Bus 0x01, buses=3)
    │           │
    │           ├── 扫描 Bus 0x01 上的设备:
    │           │   ├── Dev 0, Fn 0 → Downstream Port 1 (bridge)
    │           │   ├── Dev 1, Fn 0 → Downstream Port 2 (bridge)
    │           │   └── Dev 2, Fn 0 → Downstream Port 3 (bridge)
    │           │
    │           ├── [Pass 0] 扫描各 Downstream Port:
    │           │   ├── DS Port 1 → pci_scan_bridge_extend(Bus 0x01, dev, 0, 0, 0)
    │           │   │   └── 读 Config[0x18] = 0x01 0x02 0x02
    │           │   │   └── pci_add_new_bus() → Bus 0x02
    │           │   │   └── pci_scan_child_bus_extend(Bus 0x02, ...)
    │           │   │       └── 扫描到 Endpoint 设备
    │           │   │
    │           │   ├── DS Port 2 → pci_scan_bridge_extend(Bus 0x01, dev, 2, 0, 0)
    │           │   │   └── 读 Config[0x18] = 0x01 0x03 0x03
    │           │   │   └── pci_add_new_bus() → Bus 0x03
    │           │   │   └── pci_scan_child_bus_extend(Bus 0x03, ...)
    │           │   │
    │           │   └── DS Port 3 → pci_scan_bridge_extend(Bus 0x01, dev, 3, 0, 0)
    │           │       └── 读 Config[0x18] = 0x01 0x04 0x04
    │           │       └── pci_add_new_bus() → Bus 0x04
    │           │       └── pci_scan_child_bus_extend(Bus 0x04, ...)
    │           │
    │           └── [Pass 1] 重新配置 (无未配置桥)
    │
    └── [Pass 1] 重新配置 (无未配置桥)
```

#### 6.8.3 Switch 端口类型识别

```c
// set_pcie_port_type() 在 pci_setup_device() 中调用
// 通过 PCI Express Capability 的 Port Type 字段识别

PCI_EXP_TYPE_ROOT_PORT       = 0x4  // Root Port (RC 内部)
PCI_EXP_TYPE_UPSTREAM        = 0x5  // Switch Upstream Port
PCI_EXP_TYPE_DOWNSTREAM      = 0x6  // Switch Downstream Port
PCI_EXP_TYPE_PCI_BRIDGE      = 0x7  // PCIe-to-PCI/PCI-X Bridge
PCI_EXP_TYPE_RC_END          = 0x9  // Root Complex Integrated Endpoint
PCI_EXP_TYPE_RC_EC           = 0xA  // Root Complex Event Collector

// 内核还对上游/下游端口做正确性检查
// 如果下游端口的上游设备也是下游端口 → 修正为上游端口
if (type == PCI_EXP_TYPE_DOWNSTREAM &&
    pcie_downstream_port(parent)) {
    pdev->pcie_flags_reg &= ~PCI_EXP_FLAGS_TYPE;
    pdev->pcie_flags_reg |= PCI_EXP_TYPE_UPSTREAM;
}
```

### 6.9 桥片总线号分配算法（两遍扫描）

**源码位置**: [drivers/pci/probe.c:1375](file:///home/louis/code/linux/drivers/pci/probe.c#L1375)

`pci_scan_bridge_extend()` 使用 **两遍扫描 (Two-Pass)** 算法：

#### 6.9.1 Pass 0：处理 BIOS 已配置的桥

```
pci_scan_bridge_extend(bus, dev, max, 0, 0)  ← pass=0
    │
    ├── 读 Config[0x18] = Primary/Secondary/Subordinate
    ├── 检查配置是否有效:
    │   └── primary != bus->number || secondary <= bus->number || secondary > subordinate
    │       → broken = 1 (标记为需要重新配置)
    │
    ├── 如果配置有效 (!broken && !pcibios_assign_all_busses()):
    │   ├── child = pci_find_bus()  ← 检查子总线是否已存在
    │   ├── child = pci_add_new_bus(bus, dev, secondary)  ← 创建子总线
    │   │   └── pci_alloc_child_bus() 设置:
    │   │       ├── child->number = secondary
    │   │       ├── child->primary = bus->number
    │   │       ├── child->busn_res = { secondary, subordinate }
    │   │       └── child->self = dev
    │   │
    │   ├── cmax = pci_scan_child_bus_extend(child, buses)  ← 递归扫描
    │   └── max = max(subordinate, max)
    │
    └── 如果配置无效 (broken):
        ├── 暂时禁用桥转发:
        │   pci_write_config_dword(dev, PCI_PRIMARY_BUS, 0)
        │   ← 只保留 Latency Timer，清除 Bus 号
        └── goto out  ← 等待 Pass 1 重新配置
```

#### 6.9.2 Pass 1：分配新总线号并重新配置

```
pci_scan_bridge_extend(bus, dev, cmax, buses, 1)  ← pass=1
    │
    ├── 清除错误状态:
    │   pci_write_config_word(dev, PCI_STATUS, 0xffff)
    │
    ├── 检查 EA (Enhanced Allocation) Capability 是否固定总线号:
    │   fixed_buses = pci_ea_fixed_busnrs(dev, &fixed_sec, &fixed_sub)
    │
    ├── 计算新总线号:
    │   if (fixed_buses)
    │       next_busnr = fixed_sec
    │   else
    │       next_busnr = max + 1          ← 当前最大总线号 + 1
    │
    ├── 创建子总线:
    │   child = pci_add_new_bus(bus, dev, next_busnr)
    │   child->busn_res.end = bus->busn_res.end
    │
    ├── 写入桥配置寄存器 (单次写入三个值):
    │   buses = FIELD_PREP(PCI_PRIMARY_BUS_MASK, child->primary)       |
    │           FIELD_PREP(PCI_SECONDARY_BUS_MASK, child->busn_res.start) |
    │           FIELD_PREP(PCI_SUBORDINATE_BUS_MASK, child->busn_res.end)
    │   pci_write_config_dword(dev, PCI_PRIMARY_BUS, buses)
    │   ← 写 Config[0x18] 一次性设置 Primary/Secondary/Subordinate
    │
    ├── 递归扫描子总线:
    │   max = pci_scan_child_bus_extend(child, available_buses)
    │
    └── 更新 Subordinate 总线号:
        if (fixed_buses) max = fixed_sub
        pci_bus_update_busn_res_end(child, max)
        pci_write_config_byte(dev, PCI_SUBORDINATE_BUS, max)
        ← 写 Config[0x1A] 更新 Subordinate Bus 号
```

#### 6.9.3 总线号分配示例

```
初始状态:
  Bus 0x00 ──[桥A]── Bus 0x01 ──[桥B]── Bus 0x02
  Bus 0x00 ──[桥C]── (未配置，broken)

Pass 0:
  ─ 扫描桥A: 读 Config[0x18] = 0x00 0x01 0x02 (有效)
    └── 递归扫描 Bus 0x01 → 发现桥B
    └── 扫描桥B: 读 Config[0x18] = 0x01 0x02 0x02 (有效)
        └── 递归扫描 Bus 0x02 → 发现 Endpoint
  ─ 扫描桥C: 读 Config[0x18] = 0x00 0x00 0x00 (无效，broken)
    └── 暂时禁用 → 等待 Pass 1
  ─ max = 2

Pass 1:
  ─ 扫描桥C: next_busnr = max + 1 = 3
    └── 写入 Config[0x18] = 0x00 0x03 0xFF (Primary=0, Secondary=3, Sub=255)
    └── 递归扫描 Bus 0x03 → 发现 Endpoint
    └── 更新 Config[0x1A] = 0x03 (Subordinate=3)
  ─ max = 3
```

### 6.10 桥片窗口配置（资源分配）

桥片窗口配置在 **资源分配阶段** 完成，由 `pci_bus_assign_resources()` 调用。

#### 6.10.1 桥窗口寄存器

```
PCI Bridge 配置空间 (Type 1 Header) 窗口寄存器:

┌──────────────┬────────────┬─────────────────────────────────┐
│ 偏移         │ 寄存器      │ 格式                             │
├──────────────┼────────────┼─────────────────────────────────┤
│ 0x1C         │ I/O Base   │ [7:4] Base (高4位), [15:8] Limit │
│ 0x20         │ Mem Base   │ [31:16] Base, [15:0] Limit       │
│ 0x24         │ Pref Mem   │ [31:16] Base, [15:0] Limit       │
│ 0x28         │ Pref Mem   │ 高32位 Base (64位)               │
│ 0x2C         │ Pref Mem   │ 高32位 Limit (64位)              │
│ 0x30         │ I/O Base   │ 高16位 (32位I/O)                 │
│ 0x32         │ I/O Limit  │ 高16位 (32位I/O)                 │
└──────────────┴────────────┴─────────────────────────────────┘
```

#### 6.10.2 窗口配置函数

**源码位置**: [drivers/pci/setup-bus.c:781](file:///home/louis/code/linux/drivers/pci/setup-bus.c#L781)

```
pci_setup_bridge(bus)                              [setup-bus.c:892]
    │
    ├── pci_setup_bridge_io(bridge)                [setup-bus.c:781]
    │   ├── 读 bridge->resource[PCI_BRIDGE_IO_WINDOW]
    │   ├── 计算 io_base_lo = (region.start >> 8) & io_mask
    │   ├── 计算 io_limit_lo = (region.end >> 8) & io_mask
    │   ├── 写 Config[0x1C] = (io_limit_lo << 8) | io_base_lo
    │   └── 如果支持 32-bit I/O，写 Config[0x30~0x32] 高16位
    │
    ├── pci_setup_bridge_mmio(bridge)              [setup-bus.c:833]
    │   ├── 读 bridge->resource[PCI_BRIDGE_MEM_WINDOW]
    │   ├── 计算 l = (region.start >> 16) & 0xfff0  |  (region.end & 0xfff00000)
    │   └── 写 Config[0x20] = l
    │
    ├── pci_setup_bridge_mmio_pref(bridge)         [setup-bus.c:853]
    │   ├── 读 bridge->resource[PCI_BRIDGE_PREF_MEM_WINDOW]
    │   ├── 计算低32位 base/limit
    │   ├── 写 Config[0x24] = 低32位
    │   └── 如果 64-bit: 写 Config[0x28~0x2C] 高32位
    │
    └── pci_write_config_word(bridge, PCI_BRIDGE_CONTROL, bus->bridge_ctl)
        └── 写 Config[0x3E] Bridge Control 寄存器
```

#### 6.10.3 窗口关闭规则

PCI-PCI Bridge 规范要求：如果桥后没有对应类型的资源，必须通过 `base > limit` 关闭窗口：

```
I/O 窗口关闭:    Config[0x1C] = 0x00F0  (base=0x00, limit=0x0F, base > limit)
Mem 窗口关闭:    Config[0x20] = 0x0000FFF0  (base=0x0000, limit=0xFFF0)
Pref Mem 关闭:   Config[0x24] = 0x0000FFF0
```

### 6.11 多功能设备与 ARI 扫描

#### 6.11.1 标准多功能设备扫描

```
pci_scan_slot(bus, PCI_DEVFN(dev, 0))
    │
    ├── pci_scan_single_device(bus, PCI_DEVFN(dev, 0))  ← Fn 0
    │   └── 读 Config[0x0E] Header Type
    │       └── bit 7 (PCI_HEADER_TYPE_MFD) = 1 → multifunction = 1
    │
    └── if (dev->multifunction)
            for (fn = 1; fn < 8; fn++)
                pci_scan_single_device(bus, PCI_DEVFN(dev, fn))
                ← 每个 Fn 独立读取 Vendor ID，0xFFFFFFFF → 不存在
                ← 如果 Fn>0 存在，设置 dev->multifunction = 1
```

#### 6.11.2 ARI (Alternative Routing-ID Interpretation)

ARI 允许单个设备支持最多 **256 个 Function**（标准仅 8 个），通过 ARI Capability 实现。

```
ARI 使能条件:
  1. 设备支持 ARI Capability (PCI_EXT_CAP_ID_ARI)
  2. 上游端口支持 ARI Forwarding
  3. 总线启用了 ARI (pci_ari_enabled(bus) 返回 true)

next_fn() 逻辑:                          [probe.c:2844]
    if (pci_ari_enabled(bus))
        return next_ari_fn(bus, dev, fn)  ← 读取 ARI Cap 的 Next Function Number
    if (fn >= 7)
        return -ENODEV
    if (dev && !dev->multifunction)
        return -ENODEV
    return fn + 1

ARI Capability 寄存器:
    Config[ARI_CAP] bit [15:8] = Next Function Number (NFN)
    ← 形成一个链表，跳过不存在的 Function
```

### 6.12 完整枚举流程图

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                      PCIe 设备枚举完整流程图                                      │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│  开始                                                                           │
│    │                                                                           │
│    ▼                                                                           │
│  pci_scan_root_bus_bridge(bridge)                                              │
│    │                                                                           │
│    ▼                                                                           │
│  pci_register_host_bridge(bridge)                                              │
│    ├── pci_alloc_bus(NULL) → 分配 root bus                                     │
│    ├── bridge->bus = bus                                                       │
│    ├── bus->number = bridge->busnr                                             │
│    ├── bus->ops = bridge->ops                                                  │
│    ├── bus->sysdata = bridge->sysdata                                          │
│    ├── device_add(&bridge->dev)  ← 注册 bridge device                          │
│    ├── device_register(&bus->dev)  ← 注册 bus device                           │
│    ├── 添加 root bus 资源 (IO/MEM 窗口)                                        │
│    └── list_add(&bus->node, &pci_root_buses)                                   │
│    │                                                                           │
│    ▼                                                                           │
│  pci_scan_child_bus(bus) = pci_scan_child_bus_extend(bus, 0)                   │
│    │                                                                           │
│    ▼                                                                           │
│  ┌────────────────────────────────────────────────────────────────────────────┐ │
│  │ pci_scan_child_bus_extend(bus, 0)              [probe.c:3080]             │ │
│  │                                                                           │ │
│  │  ┌────────────────────────────────────────────────────────────────────┐   │ │
│  │  │ 阶段A: 扫描所有设备号                                              │   │ │
│  │  │ for (devnr = 0; devnr < 32; devnr++)                              │   │ │
│  │  │   pci_scan_slot(bus, PCI_DEVFN(devnr, 0))                         │   │ │
│  │  │     │                                                              │   │ │
│  │  │     ├── pci_scan_single_device(bus, devfn)  ← 扫描 Fn 0           │   │ │
│  │  │     │   ├── pci_get_slot() 检查是否已存在                          │   │ │
│  │  │     │   ├── pci_scan_device() 读 Vendor ID 创建设备               │   │ │
│  │  │     │   │   ├── pci_alloc_dev() 分配 pci_dev                      │   │ │
│  │  │     │   │   └── pci_setup_device() 读配置空间                    │   │ │
│  │  │     │   │       ├── pci_read_bases() 读 BAR                       │   │ │
│  │  │     │   │       └── pci_read_bridge_windows() 读桥窗口 (桥设备)  │   │ │
│  │  │     │   └── pci_device_add() 添加设备到总线                       │   │ │
│  │  │     │       ├── pci_init_capabilities() 读所有能力                │   │ │
│  │  │     │       └── device_add(&dev->dev) 注册到设备模型              │   │ │
│  │  │     │                                                              │   │ │
│  │  │     └── if (dev->multifunction) ← 多功能设备                       │   │ │
│  │  │             for (fn = 1; fn < 8; fn++)                             │   │ │
│  │  │               pci_scan_single_device(bus, devfn + fn)             │   │ │
│  │  │                                                                   │   │ │
│  │  └────────────────────────────────────────────────────────────────────┘   │ │
│  │                                                                           │ │
│  │  ┌────────────────────────────────────────────────────────────────────┐   │ │
│  │  │ 阶段B: SR-IOV 预留总线号                                           │   │ │
│  │  │ used_buses = pci_iov_bus_range(bus)                                │   │ │
│  │  │ max += used_buses                                                  │   │ │
│  │  └────────────────────────────────────────────────────────────────────┘   │ │
│  │                                                                           │ │
│  │  ┌────────────────────────────────────────────────────────────────────┐   │ │
│  │  │ 阶段C: 架构修复 + 统计桥类型                                       │   │ │
│  │  │ pcibios_fixup_bus(bus)                                             │   │ │
│  │  │ bus->is_added = 1                                                  │   │ │
│  │  │ for_each_pci_bridge(dev, bus) 统计 hotplug/normal 桥数量            │   │ │
│  │  └────────────────────────────────────────────────────────────────────┘   │ │
│  │                                                                           │ │
│  │  ┌────────────────────────────────────────────────────────────────────┐   │ │
│  │  │ 阶段D: Pass 0 — 扫描 BIOS 已配置的桥                               │   │ │
│  │  │ for_each_pci_bridge(dev, bus)                                       │   │ │
│  │  │   pci_scan_bridge_extend(bus, dev, max, 0, 0)                      │   │ │
│  │  │     │                                                              │   │ │
│  │  │     ├── 读 Config[0x18] Primary/Secondary/Subordinate              │   │ │
│  │  │     ├── 检查配置是否有效                                            │   │ │
│  │  │     ├── 有效: 创建子总线, 递归 pci_scan_child_bus_extend()         │   │ │
│  │  │     │         └── 对子总线重复阶段 A~D                             │   │ │
│  │  │     └── 无效: 暂时禁用桥转发, 等 Pass 1                            │   │ │
│  │  └────────────────────────────────────────────────────────────────────┘   │ │
│  │                                                                           │ │
│  │  ┌────────────────────────────────────────────────────────────────────┐   │ │
│  │  │ 阶段E: Pass 1 — 重新配置未配置的桥                                 │   │ │
│  │  │ for_each_pci_bridge(dev, bus)                                       │   │ │
│  │  │   pci_scan_bridge_extend(bus, dev, cmax, buses, 1)                 │   │ │
│  │  │     │                                                              │   │ │
│  │  │     ├── 计算 next_busnr = max + 1                                  │   │ │
│  │  │     ├── pci_add_new_bus() 创建子总线, 分配总线号                    │   │ │
│  │  │     ├── 写 Config[0x18] = Primary/Secondary/Subordinate            │   │ │
│  │  │     ├── 递归 pci_scan_child_bus_extend()                           │   │ │
│  │  │     └── 写 Config[0x1A] 更新 Subordinate Bus 号                    │   │ │
│  │  └────────────────────────────────────────────────────────────────────┘   │ │
│  │                                                                           │ │
│  └────────────────────────────────────────────────────────────────────────────┘ │
│    │                                                                           │
│    ▼                                                                           │
│  pci_bus_claim_resources(bus)  ← 声明固件资源                                  │
│    │                                                                           │
│    ▼                                                                           │
│  pci_assign_unassigned_root_bus_resources(bus)  ← 分配未分配资源                │
│    │                                                                           │
│    ▼                                                                           │
│  pci_bus_add_devices(bus)  ← 添加设备，触发 driver probe                       │
│    │                                                                           │
│    ▼                                                                           │
│  结束                                                                          │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 6.13 硬件配置寄存器变化全过程

```
阶段 A: 扫描设备阶段
═══════════════════════════════════════════════════════════════════════════════════
Root Bus 0x00 扫描:

pci_bus_read_dev_vendor_id(bus, PCI_DEVFN(0,0), &l, timeout)
    → pci_ecam_map_bus(bus, 0, 0x00)  → 映射到 ECAM 基址
    → readl(ECAM_base + 0x00)          → 读 Vendor ID (16位) + Device ID (16位)
    → 返回 0x80861234 (Vendor=0x8086, Device=0x1234)

pci_setup_device(dev):
    Config[0x00] Vendor ID    = 0x8086    → dev->vendor  = 0x8086
    Config[0x02] Device ID    = 0x1234    → dev->device  = 0x1234
    Config[0x08] Revision     = 0x02      → dev->revision = 0x02
    Config[0x09] Prog IF      = 0x00     ┐
    Config[0x0A] Subclass     = 0x04     ├→ dev->class   = 0x060400
    Config[0x0B] Base Class   = 0x06     ┘
    Config[0x0E] Header Type  = 0x01     → dev->hdr_type = 0x01 (Bridge)
                                          → dev->multifunction = 0

pci_read_bases(dev, 2, PCI_ROM_ADDRESS1):
    ┌─ 第一步: 读 BAR 原始值
    │   Config[0x10] BAR0 = 0xE000000C  → 64-bit MEM, 可预取, 已解码
    │   Config[0x14] BAR1 = 0x00000001  → I/O, 已解码
    │
    ├─ 第二步: 写全1测大小
    │   pci_write_config_dword(dev, 0x10, 0xFFFFFFFF)
    │   pci_read_config_dword(dev, 0x10, &size)
    │   size = 0xFFFFFF00  → 取反+1 = 0x100 (256 bytes)
    │   pci_write_config_dword(dev, 0x10, 0xE000000C)  ← 恢复原值
    │
    └─ 第三步: 设置 resource
        dev->resource[0] = { start=0xE0000000, end=0xE00000FF, flags=MEM_64|PREFETCH }

pci_read_bridge_windows(dev):
    Config[0x18] Primary/Secondary/Subordinate = 0x00000100
        → Primary=0x00, Secondary=0x01, Subordinate=0x00
    Config[0x1C] I/O Base/Limit = 0xF0F0  → 无 I/O 范围
    Config[0x20] Mem Base/Limit = 0xE000EFF0  → MEM 范围 [0xE0000000, 0xEFFFFFFF]
    Config[0x24] Pref Mem Base/Limit = 0xFFF0FFF0  → 无 Pref MEM 范围

阶段 D: 桥扫描 Pass 0
═══════════════════════════════════════════════════════════════════════════════════
pci_scan_bridge_extend(bus, dev, max=0, available=0, pass=0):
    ─ 读 Config[0x18] = 0x00000100
    ─ secondary=0x01, subordinate=0x00, 配置无效 (secondary > subordinate)
    ─ broken = 1
    ─ 写 Config[0x18] = 0x00000000  ← 暂时禁用桥转发

阶段 E: 桥扫描 Pass 1
═══════════════════════════════════════════════════════════════════════════════════
pci_scan_bridge_extend(bus, dev, cmax=0, buses=0, pass=1):
    ─ pci_write_config_word(dev, PCI_STATUS, 0xffff)  ← 清除错误
    ─ next_busnr = max + 1 = 1
    ─ pci_add_new_bus(bus, dev, 1)  → child bus number=1
    ─
    ─ 写 Config[0x18] = 0x000100FF
    ─   Primary=0x00, Secondary=0x01, Subordinate=0xFF
    ─   ← 单次 DWORD 写入，同时设置三个总线号
    ─
    ─ pci_scan_child_bus_extend(child, 0)  → 递归扫描 Bus 0x01
    ─   └── 发现 Endpoint 设备, 设置 max = 1
    ─
    ─ pci_bus_update_busn_res_end(child, 1)
    ─ pci_write_config_byte(dev, PCI_SUBORDINATE_BUS, 1)
    ─   ← 写 Config[0x1A] = 0x01

资源分配阶段 (pci_assign_unassigned_root_bus_resources):
═══════════════════════════════════════════════════════════════════════════════════
pci_setup_bridge(bus):
    ─ 写 Config[0x1C] = 0x00F0  ← 关闭 I/O 窗口 (base > limit)
    ─ 写 Config[0x20] = 0xE000EFF0  ← MEM 窗口 [0xE0000000, 0xEFFFFFFF]
    ─ 写 Config[0x24] = 0x0000FFF0  ← 关闭 Pref MEM 窗口
    ─ 写 Config[0x3E] = 0x0000  ← 桥控制寄存器
```

### 6.14 关键函数调用栈

```
场景 1: PCIe 设备枚举 (从 RC 初始化到设备发现)
═══════════════════════════════════════════════════════════════════════════════════
start_kernel()
  └── rest_init()
      └── kernel_init()
          └── do_basic_setup()
              └── do_initcalls()
                  │
                  ├── [postcore_initcall] pci_driver_init()        [pci-driver.c:1747]
                  │   └── bus_register(&pci_bus_type)
                  │
                  └── [arch_initcall] acpi_pci_init()              [pci-acpi.c:1516]
                      └── acpi_pci_root_init()                     [pci_root.c:1061]
                          └── acpi_scan_add_handler_with_hotplug()
                              └── acpi_pci_root_add()              [pci_root.c:880]
                                  └── acpi_pci_root_create()       [pci_root.c:1000]
                                      │
                                      ├── acpi_pci_setup_ecam_mapping()  [pci-acpi.c:1594]
                                      │   ├── pci_mcfg_lookup()       ← 查 MCFG 表
                                      │   └── pci_ecam_create()       ← ioremap 配置空间
                                      │
                                      └── pci_create_root_bus()       [probe.c:3244]
                                          └── pci_register_host_bridge()  [probe.c:991]
                                              └── pci_scan_child_bus()     [probe.c:3219]
                                                  └── pci_scan_child_bus_extend()  [probe.c:3080]
                                                      │
                                                      ├── [循环] pci_scan_slot()  [probe.c:2866]
                                                      │   └── [循环] pci_scan_single_device()  [probe.c:2787]
                                                      │       ├── pci_scan_device()     [probe.c:2591]
                                                      │       │   ├── pci_bus_read_dev_vendor_id() [probe.c:2571]
                                                      │       │   │   └── pci_ecam_map_bus()  [ecam.c:126]
                                                      │       │   ├── pci_alloc_dev()
                                                      │       │   └── pci_setup_device()   [probe.c:1958]
                                                      │       │       ├── pci_read_bases()  [probe.c:346]
                                                      │       │       │   └── __pci_read_base() [probe.c:297]
                                                      │       │       └── pci_read_bridge_windows() [probe.c:520]
                                                      │       └── pci_device_add()   [probe.c:2733]
                                                      │           ├── pci_init_capabilities() [probe.c:2280]
                                                      │           │   ├── pci_pm_init()
                                                      │           │   ├── pci_pcie_cap()
                                                      │           │   ├── pci_msi_init()
                                                      │           │   ├── pci_iov_init()
                                                      │           │   └── pci_aer_init()
                                                      │           └── device_add(&dev->dev)
                                                      │
                                                      ├── [Pass 0] pci_scan_bridge_extend()  [probe.c:1375]
                                                      │   └── pci_scan_child_bus_extend()     ← 递归
                                                      │
                                                      └── [Pass 1] pci_scan_bridge_extend()  [probe.c:1375]
                                                          └── pci_scan_child_bus_extend()     ← 递归

场景 2: 桥片总线号分配 (Pass 1 详细展开)
═══════════════════════════════════════════════════════════════════════════════════
pci_scan_bridge_extend(bus, dev, cmax, available_buses, pass=1)  [probe.c:1375]
    │
    ├── pci_write_config_word(dev, PCI_STATUS, 0xffff)           ← 清除错误
    │
    ├── pci_ea_fixed_busnrs(dev, &fixed_sec, &fixed_sub)        ← 检查 EA 固定总线
    │
    ├── next_busnr = (fixed_buses) ? fixed_sec : cmax + 1       ← 计算新总线号
    │
    ├── pci_find_bus(pci_domain_nr(bus), next_busnr)             ← 检查重复
    │
    ├── pci_add_new_bus(bus, dev, next_busnr)                    ← 创建子总线
    │   └── pci_alloc_child_bus(parent, bridge, busnr)           [probe.c:1204]
    │       ├── pci_alloc_bus(parent)
    │       ├── child->parent = parent
    │       ├── child->self = bridge
    │       ├── child->number = child->busn_res.start = busnr
    │       ├── child->primary = parent->busn_res.start
    │       ├── child->ops = parent->ops (或 host->child_ops)
    │       └── child->sysdata = parent->sysdata
    │
    ├── pci_bus_insert_busn_res(child, next_busnr, bus->busn_res.end)
    │
    ├── 写 Config[0x18] = 新 Primary/Secondary/Subordinate       ← 硬件配置
    │   pci_write_config_dword(dev, PCI_PRIMARY_BUS, buses)
    │
    ├── child->bridge_ctl = bctl
    │
    ├── pci_scan_child_bus_extend(child, available_buses)        ← 递归枚举
    │
    ├── pci_bus_update_busn_res_end(child, max)                  ← 更新总线号范围
    │
    └── pci_write_config_byte(dev, PCI_SUBORDINATE_BUS, max)     ← 写回硬件

场景 3: 资源分配 (窗口配置)
═══════════════════════════════════════════════════════════════════════════════════
pci_bus_assign_resources(bus)                                    [setup-bus.c:1496]
    └── __pci_bus_assign_resources(bus, ...)
        ├── pbus_assign_resources_sorted(bus)                    ← 分配排序
        │
        └── for_each_dev(dev, bus)
            └── for_each_sub_bus(b)
                ├── __pci_bus_assign_resources(b, ...)           ← 递归子总线
                │
                └── pci_setup_bridge(b)                          [setup-bus.c:892]
                    ├── pci_setup_bridge_io(bridge)

### 6.15 BAR (Base Address Register) 地址详解

#### 6.15.1 什么是 BAR

BAR (Base Address Register) 是 PCI/PCIe 设备配置空间中用于**声明设备所需 MMIO/I/O 地址空间**的寄存器。每个设备通过 BAR 告诉系统：
- 它需要多大的地址空间
- 它需要什么类型的地址空间 (Memory 或 I/O)
- 它的地址空间基址（由系统分配）

**配置空间位置**：Type 0 Header (普通设备) 有 6 个 BAR，位于偏移 0x10~0x24；Type 1 Header (PCI桥) 有 2 个 BAR，位于偏移 0x10~0x14。

#### 6.15.2 BAR 寄存器位布局

```
Memory BAR (32-bit):
┌─────┬──────┬────┬──────────────────────────────────────────────┐
│ Bit │  3   │  2 │       1       │             31:4             │
├─────┼──────┼────┼───────────────┼──────────────────────────────┤
│     │ Pref │ 类型 │      0       │      Base Address (16字节对齐) │
│     │etch  │     │  (Memory)    │                               │
└─────┴──────┴────┴───────────────┴──────────────────────────────┘

Memory BAR (64-bit, 占用两个连续的 BAR 槽位):
  ┌─ 低 32 位 (BAR n):     同 32-bit Memory BAR 格式
  └─ 高 32 位 (BAR n+1):   低 4 位保留 (bit 31:0 为高32位地址)

I/O BAR:
┌─────┬──────┬──────────────────────────────────────────────────┐
│ Bit │  1   │                        31:2                      │
├─────┼──────┼──────────────────────────────────────────────────┤
│     │  1   │            Base Address (4字节对齐)               │
│     │(I/O) │                                                   │
└─────┴──────┴──────────────────────────────────────────────────┘

ROM BAR (偏移 0x30/0x38):
┌─────┬──────┬──────────────────────────────────────────────────┐
│ Bit │  0   │                        31:11                     │
├─────┼──────┼──────────────────────────────────────────────────┤
│     │Enable│            ROM Base Address (2KB对齐)             │
└─────┴──────┴──────────────────────────────────────────────────┘
```

#### 6.15.3 BAR 寄存器详细定义

**源码位置**: [include/uapi/linux/pci_regs.h:96](file:///home/louis/code/linux/include/uapi/linux/pci_regs.h#L96)

```c
// BAR 配置空间偏移 (Type 0 Header)
#define PCI_BASE_ADDRESS_0      0x10    // BAR 0, 偏移 0x10
#define PCI_BASE_ADDRESS_1      0x14    // BAR 1, 偏移 0x14
#define PCI_BASE_ADDRESS_2      0x18    // BAR 2, 偏移 0x18
#define PCI_BASE_ADDRESS_3      0x1c    // BAR 3, 偏移 0x1C
#define PCI_BASE_ADDRESS_4      0x20    // BAR 4, 偏移 0x20
#define PCI_BASE_ADDRESS_5      0x24    // BAR 5, 偏移 0x24

// BAR 位定义
#define PCI_BASE_ADDRESS_SPACE         0x01    // 0=Memory, 1=I/O
#define PCI_BASE_ADDRESS_SPACE_IO      0x01    // I/O 空间
#define PCI_BASE_ADDRESS_SPACE_MEMORY  0x00    // Memory 空间
#define PCI_BASE_ADDRESS_MEM_TYPE_MASK 0x06    // [2:1] Memory 类型
#define PCI_BASE_ADDRESS_MEM_TYPE_32   0x00    // 32-bit 地址
#define PCI_BASE_ADDRESS_MEM_TYPE_1M   0x02    // 1M 以下 (已废弃)
#define PCI_BASE_ADDRESS_MEM_TYPE_64   0x04    // 64-bit 地址
#define PCI_BASE_ADDRESS_MEM_PREFETCH  0x08    // 可预取
#define PCI_BASE_ADDRESS_MEM_MASK     (~0x0fUL) // Memory 地址掩码 (16字节对齐)
#define PCI_BASE_ADDRESS_IO_MASK      (~0x03UL) // I/O 地址掩码 (4字节对齐)

// ROM BAR
#define PCI_ROM_ADDRESS         0x30    // 偏移 0x30 (Type 0 Header)
#define PCI_ROM_ADDRESS1        0x38    // 偏移 0x38 (Type 1 Header, 桥设备)
#define PCI_ROM_ADDRESS_ENABLE  0x01    // ROM 使能位
#define PCI_ROM_ADDRESS_MASK    (~0x7ffU) // ROM 地址掩码 (2KB对齐)
```

#### 6.15.4 `decode_bar()` — BAR 类型解码

**源码位置**: [drivers/pci/probe.c:138](file:///home/louis/code/linux/drivers/pci/probe.c#L138)

```c
static inline unsigned long decode_bar(struct pci_dev *dev, u32 bar)
{
    u32 mem_type;
    unsigned long flags;

    // 判断是否为 I/O 空间
    if ((bar & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {
        flags = bar & ~PCI_BASE_ADDRESS_IO_MASK;  // 保留低 2 位 (I/O 类型信息)
        flags |= IORESOURCE_IO;
        return flags;
    }

    // Memory 空间处理
    flags = bar & ~PCI_BASE_ADDRESS_MEM_MASK;     // 保留低 4 位 (类型信息)
    flags |= IORESOURCE_MEM;

    if (flags & PCI_BASE_ADDRESS_MEM_PREFETCH)     // 可预取标志
        flags |= IORESOURCE_PREFETCH;

    mem_type = bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK;
    switch (mem_type) {
    case PCI_BASE_ADDRESS_MEM_TYPE_32:             // 32-bit
        break;
    case PCI_BASE_ADDRESS_MEM_TYPE_1M:             // 1M 以下 (已废弃)
        break;
    case PCI_BASE_ADDRESS_MEM_TYPE_64:             // 64-bit
        flags |= IORESOURCE_MEM_64;
        break;
    }
    return flags;
}
```

#### 6.15.5 BAR 大小探测算法 (BAR Sizing)

PCI 规范定义了一种**无损探测**算法来确定 BAR 需要多大的地址空间：

```
BAR Sizing 算法 (每个 BAR 独立执行):
═══════════════════════════════════════════════════════════════════════════════════

步骤 1: 关闭设备解码
  pci_read_config_word(dev, PCI_COMMAND, &orig_cmd)
  pci_write_config_word(dev, PCI_COMMAND, orig_cmd & ~DECODE_ENABLE)
  ← 关闭 Memory/I/O 解码，防止探测时设备对地址产生误响应

步骤 2: 批量读取所有 BAR 掩码 (__pci_size_bars)
  for (i = 0; i < count; i++) {
      pci_read_config_dword(dev, pos, &orig)    ← 读原始值 (含基址)
      pci_write_config_dword(dev, pos, ~0)       ← 写全 1
      pci_read_config_dword(dev, pos, &mask)     ← 读回 (硬件将可寻址位写 1)
      pci_write_config_dword(dev, pos, orig)     ← 恢复原始值
  }

步骤 3: 计算每个 BAR 大小 (__pci_read_base)
  ┌──── 物理含义 ──────────────────────────────────────────────────────┐
  │                                                                     │
  │  写全 1 后读回的值中，为 1 的位表示该地址位是可编程的 (可寻址位)。     │
  │  最低为 1 的位决定了 BAR 的对齐要求和大小。                           │
  │                                                                     │
  │  示例:                                                              │
  │    写全 1 后读回 BAR = 0xFFFFFF00                                    │
  │    → 低 8 位为 0 (不可编程), 最低可编程位是 bit 8                    │
  │    → 大小 = 2^8 = 256 字节                                           │
  │    → 对齐 = 256 字节边界                                             │
  │                                                                     │
  │  pci_size() 函数:                                                    │
  │    size = mask & maxbase;   // 取有效位                              │
  │    size = size & ~(size-1); // 取最低置位位 = 大小                   │
  │    return size;                                                      │
  └──────────────────────────────────────────────────────────────────────┘

步骤 4: 恢复解码
  pci_write_config_word(dev, PCI_COMMAND, orig_cmd)  ← 恢复原始解码状态

步骤 5: 设置 resource 结构
  region.start = l64;          // 原始基址
  region.end   = l64 + sz - 1; // 基址 + 大小 - 1
  pcibios_bus_to_resource(dev->bus, res, &region);  // 转换为 CPU 地址
```

#### 6.15.6 `pci_read_bases()` — BAR 读取流程

**源码位置**: [drivers/pci/probe.c:346](file:///home/louis/code/linux/drivers/pci/probe.c#L346)

```c
static void pci_read_bases(struct pci_dev *dev, unsigned int howmany, int rom)
{
    u32 rombar, stdbars[PCI_STD_NUM_BARS];  // PCI_STD_NUM_BARS = 6
    u16 orig_cmd;

    // 非兼容设备跳过
    if (dev->non_compliant_bars) return;
    if (dev->is_virtfn) return;  // VF BAR 为只读零

    // 关闭设备解码 (防止探测期间产生副作用)
    if (!dev->mmio_always_on) {
        pci_read_config_word(dev, PCI_COMMAND, &orig_cmd);
        if (orig_cmd & PCI_COMMAND_DECODE_ENABLE)
            pci_write_config_word(dev, PCI_COMMAND,
                orig_cmd & ~PCI_COMMAND_DECODE_ENABLE);
    }

    // 批量读取 BAR 掩码
    __pci_size_stdbars(dev, howmany, PCI_BASE_ADDRESS_0, stdbars);
    // 读取 ROM BAR 掩码
    if (rom) __pci_size_rom(dev, rom, &rombar);

    // 恢复解码
    if (!dev->mmio_always_on && (orig_cmd & PCI_COMMAND_DECODE_ENABLE))
        pci_write_config_word(dev, PCI_COMMAND, orig_cmd);

    // 逐个解析 BAR
    for (pos = 0; pos < howmany; pos++) {
        struct resource *res = &dev->resource[pos];
        reg = PCI_BASE_ADDRESS_0 + (pos << 2);
        // __pci_read_base 返回 1 表示 64-bit BAR (占用两个槽位)
        pos += __pci_read_base(dev, pci_bar_unknown, res, reg, &stdbars[pos]);
    }

    // 解析 ROM BAR
    if (rom) {
        struct resource *res = &dev->resource[PCI_ROM_RESOURCE];
        dev->rom_base_reg = rom;
        res->flags = IORESOURCE_MEM | IORESOURCE_PREFETCH |
                     IORESOURCE_READONLY | IORESOURCE_SIZEALIGN;
        __pci_read_base(dev, pci_bar_mem32, res, rom, &rombar);
    }
}
```

#### 6.15.7 `__pci_read_base()` — 单 BAR 解析

**源码位置**: [drivers/pci/probe.c:297](file:///home/louis/code/linux/drivers/pci/probe.c#L297)

```c
int __pci_read_base(struct pci_dev *dev, enum pci_bar_type type,
                    struct resource *res, unsigned int pos, u32 *sizes)
{
    u32 l, sz;
    u64 l64, sz64, mask64;

    // 步骤 1: 读取 BAR 原始值
    pci_read_config_dword(dev, pos, &l);
    sz = sizes[0];  // 来自 __pci_size_bars 的掩码值

    // 步骤 2: 解码 BAR 类型
    if (type == pci_bar_unknown) {
        res->flags = decode_bar(dev, l);  // 确定 Memory/I/O, 32/64-bit
        res->flags |= IORESOURCE_SIZEALIGN;

        if (res->flags & IORESOURCE_IO) {
            l64 = l & PCI_BASE_ADDRESS_IO_MASK;    // 取基址
            sz64 = sz & PCI_BASE_ADDRESS_IO_MASK;
            mask64 = PCI_BASE_ADDRESS_IO_MASK & IO_SPACE_LIMIT;
        } else {
            l64 = l & PCI_BASE_ADDRESS_MEM_MASK;   // 取基址
            sz64 = sz & PCI_BASE_ADDRESS_MEM_MASK;
            mask64 = PCI_BASE_ADDRESS_MEM_MASK;
        }
    } else {
        // ROM BAR 处理
        if (l & PCI_ROM_ADDRESS_ENABLE)
            res->flags |= IORESOURCE_ROM_ENABLE;
        l64 = l & PCI_ROM_ADDRESS_MASK;
        sz64 = sz & PCI_ROM_ADDRESS_MASK;
        mask64 = PCI_ROM_ADDRESS_MASK;
    }

    // 步骤 3: 64-bit BAR 读取高 32 位
    if (res->flags & IORESOURCE_MEM_64) {
        pci_read_config_dword(dev, pos + 4, &l);
        sz = sizes[1];  // 下一个槽位的掩码
        l64 |= ((u64)l << 32);
        sz64 |= ((u64)sz << 32);
        mask64 |= ((u64)~0 << 32);
    }

    // 步骤 4: 计算 BAR 大小
    sz64 = pci_size(l64, sz64, mask64);
    if (!sz64) goto fail;  // 无效 BAR

    // 步骤 5: 设置 resource 范围
    region.start = l64;
    region.end = l64 + sz64 - 1;
    pcibios_bus_to_resource(dev->bus, res, &region);
    // ...
}
```

#### 6.15.8 BAR 与 Header Type 的关系

```
Type 0 Header (普通设备/Endpoint):
┌──────────┬──────────┬──────────────────────────────────────────────┐
│ 偏移     │ 寄存器    │ 说明                                         │
├──────────┼──────────┼──────────────────────────────────────────────┤
│ 0x10     │ BAR 0    │ 第 1 个 BAR                                   │
│ 0x14     │ BAR 1    │ 第 2 个 BAR                                   │
│ 0x18     │ BAR 2    │ 第 3 个 BAR                                   │
│ 0x1C     │ BAR 3    │ 第 4 个 BAR                                   │
│ 0x20     │ BAR 4    │ 第 5 个 BAR                                   │
│ 0x24     │ BAR 5    │ 第 6 个 BAR                                   │
│ 0x30     │ ROM BAR  │ Expansion ROM Base Address                    │
│ 0x3C     │ IRQ Line │ 中断线                                        │
└──────────┴──────────┴──────────────────────────────────────────────┘
  注: 如果某个 BAR 是 64-bit 类型，它会占用相邻的两个 BAR 槽位。
      例如 BAR 0 为 64-bit 时，BAR 0 存低 32 位，BAR 1 存高 32 位，
      此时 BAR 1 不可独立使用，实际可用的 BAR 数量减少 1 个。

Type 1 Header (PCI桥/Bridge):
┌──────────┬──────────┬──────────────────────────────────────────────┐
│ 偏移     │ 寄存器    │ 说明                                         │
├──────────┼──────────┼──────────────────────────────────────────────┤
│ 0x10     │ BAR 0    │ 第 1 个 BAR (桥设备自身使用)                  │
│ 0x14     │ BAR 1    │ 第 2 个 BAR (桥设备自身使用)                  │
│ 0x18     │ Bus 号   │ Primary/Secondary/Subordinate Bus Number     │
│ 0x1C     │ I/O 窗口 │ I/O Base/Limit                               │
│ 0x20     │ Mem 窗口 │ Memory Base/Limit                            │
│ 0x24     │ Pref 窗口│ Prefetchable Memory Base/Limit               │
│ 0x28~0x2C│ Pref高32 │ Prefetchable Memory 高 32 位 (64-bit)       │
│ 0x30     │ I/O高16  │ I/O Base/Limit 高 16 位 (32-bit I/O)        │
│ 0x38     │ ROM BAR  │ Expansion ROM Base Address                   │
└──────────┴──────────┴──────────────────────────────────────────────┘
  注: 桥设备只有 2 个 BAR (BAR 0, BAR 1)，用于桥自身的控制寄存器。
      桥通过窗口寄存器 (I/O Base/Limit, Mem Base/Limit, Pref Base/Limit)
      转发下游设备的地址访问，而不是通过 BAR。
```

#### 6.15.9 BAR 地址空间分配示例

假设一个 Endpoint 设备有 3 个 BAR：

```
硬件设计:
  BAR 0: 64-bit Memory, 可预取, 大小 = 1MB    (占用 BAR 0 + BAR 1)
  BAR 2: 32-bit Memory, 不可预取, 大小 = 4KB
  BAR 3: I/O, 大小 = 256 字节

枚举阶段 (pci_read_bases) 读取结果:
  BAR 0 原始值 = 0x00000000  → 写全1读回 = 0xFFFFF000  → 大小 = 1MB
  BAR 1 原始值 = 0x00000000  → 写全1读回 = 0x00000000  (64-bit 高32位)
  BAR 2 原始值 = 0x00000000  → 写全1读回 = 0xFFFFF000  → 大小 = 4KB
  BAR 3 原始值 = 0x00000001  → 写全1读回 = 0xFFFFFF01  → 大小 = 256B

  dev->resource[0] = { start=0, end=0xFFFFF, flags=MEM_64|PREFETCH }
  dev->resource[1] = { start=0, end=0, flags=0 }  ← 被 BAR 0 占用
  dev->resource[2] = { start=0, end=0xFFF, flags=MEM }
  dev->resource[3] = { start=0, end=0xFF, flags=IO }

资源分配阶段 (pci_assign_resource) 分配地址:
  BAR 0: 分配 0xE0000000~0xE00FFFFF  → 写 Config[0x10] = 0xE000000C
                                     → 写 Config[0x14] = 0x00000000
  BAR 2: 分配 0xE0100000~0xE0100FFF  → 写 Config[0x18] = 0xE0100001
  BAR 3: 分配 0x1000~0x10FF          → 写 Config[0x1C] = 0x00001001

配置空间写入结果:
  Config[0x10] = 0xE000000C  ← bit 0=0 (Memory), bit 2=1 (64-bit), bit 3=1 (Prefetch)
  Config[0x14] = 0x00000000  ← 64-bit 高 32 位
  Config[0x18] = 0xE0100001  ← bit 0=0 (Memory), 不可预取
  Config[0x1C] = 0x00001001  ← bit 0=1 (I/O)
```

#### 6.15.10 BAR 与 `struct resource` 的关系

每个 BAR 对应 `struct pci_dev` 中的一个 `struct resource`：

```c
struct pci_dev {
    ...
    struct resource resource[DEVICE_COUNT_RESOURCE];  // 最多 19 个资源
    // resource[0..5]  → BAR 0~5
    // resource[6]     → ROM BAR (PCI_ROM_RESOURCE = 6)
    // resource[7..11] → 桥窗口 (PCI_BRIDGE_RESOURCES 起)
    // ...
};

// bridge 设备的 resource 索引:
#define PCI_BRIDGE_RESOURCES          7
#define PCI_BRIDGE_IO_WINDOW          0  // resource[7]  → I/O 窗口
#define PCI_BRIDGE_MEM_WINDOW         1  // resource[8]  → Mem 窗口
#define PCI_BRIDGE_PREF_MEM_WINDOW    2  // resource[9]  → Pref Mem 窗口

// resource 标志位 (BAR 解码结果):
IORESOURCE_IO          // bit 0: I/O 空间
IORESOURCE_MEM         // bit 1: Memory 空间
IORESOURCE_PREFETCH    // bit 3: 可预取
IORESOURCE_MEM_64      // bit 24: 64-bit Memory
IORESOURCE_SIZEALIGN   // bit 13: 大小对齐 (自动计算)
IORESOURCE_ROM_ENABLE  // 非标准位: ROM 使能
```

#### 6.15.11 BAR 地址转换流程

```
PCI 总线地址 → CPU 地址转换:
───────────────────────────────────────────────────────────────────────

pci_dev.resource[].start/end 存储的是 CPU 地址 (通过 pcibios_bus_to_resource 转换)

转换关系:
  pci_bus_address(dev, bar)  →  PCI 总线地址 (配置空间中 BAR 寄存器的值)
  dev->resource[bar].start   →  CPU 地址 (Linux 内核使用的地址)

转换函数:
  pcibios_bus_to_resource(bus, res, region)   ← PCI 总线地址 → CPU 地址
  pcibios_resource_to_bus(bus, &region, res)   ← CPU 地址 → PCI 总线地址

在 x86 上:
  PCI 总线地址 == CPU 地址 (直接映射)
  所以 pcibios_bus_to_resource 是空操作

在 ARM/ARM64 上:
  PCI 总线地址 ≠ CPU 地址
  需要通过 pci_address_to_pio / pci_ioremap 等进行转换

设备驱动使用:
  void __iomem *addr = pci_iomap(pdev, bar, maxlen);
  // 等价于:
  //   resource_size_t start = pci_resource_start(pdev, bar);
  //   resource_size_t len   = pci_resource_len(pdev, bar);
  //   void __iomem *addr = ioremap(start, len);
  // 之后通过 readl/writel 访问 BAR 映射后的 MMIO 地址
```

#### 6.15.12 BAR 探测的特殊情况

```
1. BAR 未实现 (Unimplemented BAR):
   ─ 写全 1 后读回为 0 → 该 BAR 不存在，跳过
   ─ 对应 resource 的 flags 为 0

2. 64-bit BAR 跨越两个槽位:
   ─ __pci_read_base 返回 1 (占用 2 个槽位)
   ─ 调用者 pci_read_bases 中: pos += __pci_read_base(...)
   ─ 被占用的下一个 resource 被标记为 flags=0

3. 非兼容 BAR (Non-compliant BAR):
   ─ dev->non_compliant_bars = true 时跳过整个 BAR 探测
   ─ 某些设备 BAR 不符合 PCI 规范 (如一些 IDE 控制器)

4. 桥设备的 BAR:
   ─ Type 1 Header 只有 2 个 BAR (BAR 0, BAR 1)
   ─ 在 pci_setup_device() 中调用 pci_read_bases(dev, 2, PCI_ROM_ADDRESS1)
   ─ 桥的地址转发通过窗口寄存器实现，不是 BAR

5. VF BAR (SR-IOV 虚拟功能):
   ─ dev->is_virtfn = true 时 BAR 为只读零
   ─ VF 的 BAR 空间由 PF 的 SR-IOV Capability 管理

6. ROM BAR 特殊处理:
   ─ 偏移 0x30 (Type 0) 或 0x38 (Type 1)
   ─ bit 0 为 Enable 位 (区别于普通 BAR 的 type 位)
   ─ 大小按 2KB 对齐
   ─ 探测时 mask = PCI_ROM_ADDRESS_MASK (~0x7ff)
   ─ 资源标志: IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_READONLY
```

---

## 七、阶段 4：PCI 驱动注册

### 7.1 驱动注册入口

```
module_init(xxx_pci_driver_init)
    │
    └── pci_register_driver(&xxx_driver)          ← 宏定义
        └── __pci_register_driver(drv, THIS_MODULE, KBUILD_MODNAME)
            │
            ├── drv->driver.name = drv->name
            ├── drv->driver.bus = &pci_bus_type    ← 关联 PCI 总线
            ├── drv->driver.probe = pci_device_probe  ← 设置 probe 回调
            ├── drv->driver.remove = pci_device_remove
            │
            ├── spin_lock_init(&drv->dynids.lock)
            ├── INIT_LIST_HEAD(&drv->dynids.list)
            │
            └── driver_register(&drv->driver)     ← 内核驱动核心
                └── bus_add_driver(drv)
                    ├── klist_add_tail(&drv->p->knode_bus, &bus->p->klist_drivers)
                    │   ← 驱动加入总线驱动链表
                    │
                    └── driver_attach(drv)        ← 匹配已注册的设备
                        └── 遍历 bus->p->klist_devices
                            └── __driver_attach(dev, drv)
                                └── pci_bus_match(dev, drv)  ← 匹配
                                    └── 匹配成功 →
                                        driver_probe_device(dev, drv)
                                        └── pci_device_probe(dev)
```

### 7.2 匹配机制 `pci_bus_match()`

```
pci_bus_match(dev, drv)                     [drivers/pci/pci-driver.c:1535]
    │
    └── pci_match_device(drv, pci_dev)      [pci-driver.c:133]
        │
        ├── [1. 检查 driver_override]
        │   if (dev->driver_override && strcmp(dev->driver_override, drv->name))
        │       return NULL
        │
        ├── [2. 检查动态 ID (sysfs new_id)]
        │   spin_lock(&drv->dynids.lock)
        │   list_for_each_entry(dynid, &drv->dynids.list, node)
        │       if (pci_match_one_device(&dynid->id, dev))
        │           return &dynid->id
        │   spin_unlock(&drv->dynids.lock)
        │
        ├── [3. 检查静态 ID 表]
        │   for (ids = drv->id_table; ids->vendor || ids->subvendor || ids->class_mask; ids++)
        │       if (pci_match_one_device(ids, dev))
        │           return ids
        │
        └── [4. driver_override 兜底]
            if (dev->driver_override)
                return &pci_device_id_any
            return NULL
```

### 7.3 `pci_match_one_device()` — 单条匹配规则

```c
// drivers/pci/pci-driver.c:58
static inline bool pci_match_one_device(const struct pci_device_id *id,
                                        const struct pci_dev *dev)
{
    // 逐一比较 vendor/device/subvendor/subdevice/class
    if ((id->vendor == PCI_ANY_ID || id->vendor == dev->vendor) &&
        (id->device == PCI_ANY_ID || id->device == dev->device) &&
        (id->subvendor == PCI_ANY_ID || id->subvendor == dev->subsystem_vendor) &&
        (id->subdevice == PCI_ANY_ID || id->subdevice == dev->subsystem_device) &&
        !((id->class ^ dev->class) & id->class_mask))
        return true;
    return false;
}
```

### 7.4 数据结构变化

```
注册前:
  pci_driver = {
      .name = "e1000",
      .id_table = { { 0x8086, 0x100E, ... } },
      .probe = e1000_probe,
      .driver = { .bus = NULL, ... },
  }

driver_register 后:
  pci_driver = {
      .driver = {
          .bus = &pci_bus_type,        ← 已关联总线
          .p = &driver_private {
              .knode_bus → 已加入 bus->p->klist_drivers
          }
      }
  }

  pci_bus_type.p->klist_drivers = [drv1, drv2, ...]
```

---

## 八、阶段 5：设备 Probe (驱动绑定)

### 8.1 触发时机

有两种触发方式：

**方式 A: 枚举后触发**（`pci_bus_add_devices` → `pci_bus_add_device` → `device_initial_probe`）

```
pci_bus_add_devices(bus)                    [drivers/pci/bus.c:377]
    │
    └── for_each_dev_on_bus(bus, dev)
        │
        └── pci_bus_add_device(dev)          [bus.c:345]
            ├── pcibios_bus_add_device(dev)   ← 架构相关
            ├── pci_fixup_device(pci_fixup_final, dev)  ← 最终修复
            ├── of_pci_make_dev_node(dev)     ← DT 节点
            ├── pci_create_sysfs_dev_files(dev) ← sysfs 接口
            ├── pci_proc_attach_device(dev)    ← /proc/bus/pci
            ├── pci_bridge_d3_update(dev)
            ├── pci_save_state(dev)            ← 保存配置空间
            ├── pm_runtime_enable(&dev->dev)   ← 启用运行时 PM
            │
            └── device_initial_probe(&dev->dev)  ← 触发驱动绑定
                │
                └── __device_attach(dev, true)  [drivers/base/dd.c]
                    │
                    └── bus_for_each_drv(dev->bus, NULL, dev, __device_attach_driver)
                        │
                        └── __device_attach_driver(dev, drv)
                            ├── pci_bus_match(dev, drv)    ← 匹配
                            └── driver_probe_device(drv, dev)  ← probe
                                └── pci_device_probe(dev)  ← PCI 总线 probe
```

**方式 B: 驱动注册后触发**（`driver_register` → `driver_attach`）

```
driver_register(drv)
    └── driver_attach(drv)
        └── bus_for_each_dev(drv->bus, NULL, drv, __driver_attach)
            └── __driver_attach(dev, drv)
                └── pci_bus_match(dev, drv)     ← 匹配已存在的设备
                    └── driver_probe_device(drv, dev)
                        └── pci_device_probe(dev)
```

### 8.2 `pci_device_probe()` — 完整 probe 流程

```
pci_device_probe(dev)                        [drivers/pci/pci-driver.c:551]
    │
    ├── pci_assign_irq(dev)                   ← 分配 IRQ
    │   └── 根据 dev->irq 和平台 IRQ 路由表分配
    │
    ├── pci_configure_device_desc(dev)         ← 配置设备描述
    │
    ├── __pci_device_probe(drv, pci_dev)       [pci-driver.c:405]
    │   │
    │   ├── pci_match_device(drv, pci_dev)     ← 再次匹配以获取 ID
    │   │
    │   └── pci_call_probe(drv, dev, id)        [pci-driver.c:364]
    │       │
    │       ├── [确定目标 CPU 节点]
    │       │   node = dev_to_node(&dev->dev)
    │       │
    │       ├── [异步或同步执行]
    │       │   if (node >= 0 && node_online(node))
    │       │       queue_work_on(cpu, pci_probe_wq, &arg.work)
    │       │       flush_work(&arg.work)      ← 等待异步完成
    │       │   else
    │       │       local_pci_probe(&ddi)       ← 直接执行
    │       │
    │       └── local_pci_probe(ddi)            [pci-driver.c:305]
    │           ├── pm_runtime_get_sync(dev)    ← 唤醒设备
    │           ├── pci_dev->driver = pci_drv   ← 设置 driver 指针
    │           │
    │           ├── rc = pci_drv->probe(pci_dev, ddi->id)  ← ★ 调用设备驱动 probe
    │           │   └── 例如 e1000_probe(dev, id)
    │           │       ├── pci_enable_device(dev)  ← 启用设备
    │           │       ├── pci_request_regions(dev) ← 请求资源
    │           │       ├── pci_set_master(dev)      ← 设置总线主控
    │           │       ├── dma_set_mask_and_coherent()
    │           │       ├── 注册中断 (request_irq / pci_request_irq)
    │           │       └── 注册网络设备 (register_netdev)
    │           │
    │           └── return rc
    │
    └── [失败时回滚]
        if (rc < 0)
            pci_dev->driver = NULL              ← 清除 driver 指针
```

### 8.3 数据结构变化

```
Probe 前:
  pci_dev = {
      .driver = NULL,                  ← 未绑定驱动
      .is_probed = 0,
      .dev.driver = NULL,
  }

local_pci_probe 中:
  pci_dev = {
      .driver = &e1000_driver,         ← 已绑定驱动
      .is_probed = 1,
      .dev.driver = &e1000_driver.driver,
  }

Probe 成功后:
  PCI 设备已完全初始化，可通过 /sys/bus/pci/devices/ 访问
  /sys/bus/pci/drivers/e1000/ 目录下出现设备链接
```

---

## 九、完整调用链全景图

```
┌──────────────────────────────────────────────────────────────────────────────────────────────┐
│                                    PCIe 完整调用链                                            │
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                              │
│  postcore_initcall                                                                          │
│  ┌──────────────────────────────────────────────────────────────────────────────────────┐   │
│  │ pcibus_class_init()  →  class_register(&pcibus_class)    (/sys/class/pci_bus/)       │   │
│  │ pci_driver_init()    →  bus_register(&pci_bus_type)      (/sys/bus/pci/)             │   │
│  │                      →  bus_register(&pcie_port_bus_type)                             │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                              │
│  arch_initcall / module_init                                                                │
│  ┌──────────────────────────────────────────────────────────────────────────────────────┐   │
│  │ ACPI: acpi_pci_init() → acpi_pci_root_init()                                         │   │
│  │ DT:   pci_host_common_probe() → pci_host_probe(bridge)                               │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
│                                     │                                                       │
│                                     ▼                                                       │
│  ┌──────────────────────────────────────────────────────────────────────────────────────┐   │
│  │ pci_host_probe(bridge)                           [drivers/pci/probe.c:3271]          │   │
│  │                                                                                      │   │
│  │  ┌────────────────────────────────────────────────────────────────────────────────┐  │   │
│  │  │ 1. pci_scan_root_bus_bridge(bridge)                                           │  │   │
│  │  │    ├─ pci_register_host_bridge(bridge)                                        │  │   │
│  │  │    │   ├─ pci_alloc_bus(NULL)         → struct pci_bus (root)                  │  │   │
│  │  │    │   ├─ device_add(&bridge->dev)    → 注册 bridge device                     │  │   │
│  │  │    │   ├─ device_register(&bus->dev)  → 注册 bus device                        │  │   │
│  │  │    │   └─ list_add(&bus->node, &pci_root_buses)                                │  │   │
│  │  │    │                                                                          │  │   │
│  │  │    └─ pci_scan_child_bus(bus) = pci_scan_child_bus_extend(bus, 0)             │  │   │
│  │  │         for (devnr = 0; devnr < 32; devnr++)                                  │  │   │
│  │  │           pci_scan_slot(bus, PCI_DEVFN(devnr, 0))                             │  │   │
│  │  │             pci_scan_single_device(bus, devfn)                                │  │   │
│  │  │               ├─ pci_scan_device(bus, devfn)                                  │  │   │
│  │  │               │   ├─ pci_bus_read_dev_vendor_id()  ← 读 Vendor ID 判定存在     │  │   │
│  │  │               │   ├─ pci_alloc_dev(bus)            ← 分配 pci_dev             │  │   │
│  │  │               │   └─ pci_setup_device(dev)         ← 读配置空间               │  │   │
│  │  │               │       ├─ pci_read_bases()   ← 读 BAR 寄存器                   │  │   │
│  │  │               │       └─ pci_read_irq()     ← 读 IRQ 引脚                    │  │   │
│  │  │               │                                                              │  │   │
│  │  │               └─ pci_device_add(dev, bus)          ← 添加设备                 │  │   │
│  │  │                   ├─ device_initialize(&dev->dev)                             │  │   │
│  │  │                   ├─ pci_fixup_device(pci_fixup_header)  ← 修复               │  │   │
│  │  │                   ├─ pci_init_capabilities(dev)      ← 17 种能力              │  │   │
│  │  │                   │   ├─ pci_pm_init()        ← PM 电源管理                   │  │   │
│  │  │                   │   ├─ pci_pcie_cap()       ← PCIe 能力                     │  │   │
│  │  │                   │   ├─ pci_msi_init()       ← MSI 中断                     │  │   │
│  │  │                   │   ├─ pci_msix_init()      ← MSI-X 中断                   │  │   │
│  │  │                   │   ├─ pci_iov_init()       ← SR-IOV                       │  │   │
│  │  │                   │   ├─ pci_aer_init()       ← AER 错误报告                  │  │   │
│  │  │                   │   ├─ pci_ats_init()       ← ATS 地址转换                  │  │   │
│  │  │                   │   ├─ pci_acs_init()       ← ACS 访问控制                  │  │   │
│  │  │                   │   ├─ pci_aspm_init()      ← ASPM 电源管理                 │  │   │
│  │  │                   │   └─ ...                                               │  │   │
│  │  │                   ├─ list_add(&dev->bus_list, &bus->devices)                  │  │   │
│  │  │                   └─ device_add(&dev->dev)  → 注册到设备模型                   │  │   │
│  │  │                                                                              │  │   │
│  │  │         /* 递归扫描桥设备 */                                                  │  │   │
│  │  │         for_each_pci_bridge(dev, bus)                                        │  │   │
│  │  │           pci_scan_bridge_extend(bus, dev, ...)                              │  │   │
│  │  │             └─ pci_scan_child_bus_extend(child_bus, ...)  ← 递归              │  │   │
│  │  └────────────────────────────────────────────────────────────────────────────┘  │   │
│  │                                                                                      │   │
│  │  ┌────────────────────────────────────────────────────────────────────────────────┐  │   │
│  │  │ 2. pci_bus_claim_resources(bus)       ← 声明固件配置的资源                       │  │   │
│  │  └────────────────────────────────────────────────────────────────────────────────┘  │   │
│  │                                                                                      │   │
│  │  ┌────────────────────────────────────────────────────────────────────────────────┐  │   │
│  │  │ 3. pci_assign_unassigned_root_bus_resources(bus)  ← 分配未分配资源               │  │   │
│  │  └────────────────────────────────────────────────────────────────────────────────┘  │   │
│  │                                                                                      │   │
│  │  ┌────────────────────────────────────────────────────────────────────────────────┐  │   │
│  │  │ 4. pci_bus_add_devices(bus)         ← 添加设备，触发 probe                      │  │   │
│  │  │    └─ pci_bus_add_device(dev)                                                   │  │   │
│  │  │        └─ device_initial_probe(&dev->dev)                                       │  │   │
│  │  │            └─ __device_attach(dev, true)                                        │  │   │
│  │  │                └─ driver_probe_device(dev, drv)                                 │  │   │
│  │  │                    └─ pci_device_probe(dev)                                     │  │   │
│  │  │                        ├─ pci_assign_irq(dev)                                   │  │   │
│  │  │                        └─ __pci_device_probe(drv, dev)                          │  │   │
│  │  │                            └─ pci_call_probe(drv, dev, id)                      │  │   │
│  │  │                                └─ local_pci_probe(ddi)                          │  │   │
│  │  │                                    └─ drv->probe(pci_dev, id)  ★ 设备驱动 probe  │  │   │
│  │  └────────────────────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                              │
│  驱动注册 (可独立于枚举，随时发生)                                                            │
│  ┌──────────────────────────────────────────────────────────────────────────────────────┐   │
│  │ __pci_register_driver(drv)         [drivers/pci/pci-driver.c:1464]                   │   │
│  │  └─ driver_register(&drv->driver)                                                    │   │
│  │      └─ bus_add_driver(drv) → driver_attach(drv)                                     │   │
│  │          └─ 遍历已有设备: pci_bus_match(dev, drv) → driver_probe_device()             │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 十、关键函数索引

| 函数 | 文件:行号 | 作用 |
|------|-----------|------|
| `pci_driver_init()` | [pci-driver.c:1747](file:///home/louis/code/linux/drivers/pci/pci-driver.c#L1747) | 注册 `pci_bus_type` 和 `pcie_port_bus_type` |
| `pcibus_class_init()` | [probe.c:114](file:///home/louis/code/linux/drivers/pci/probe.c#L114) | 注册 `pcibus_class` (sysfs 类) |
| `pci_alloc_host_bridge()` | [probe.c:718](file:///home/louis/code/linux/drivers/pci/probe.c#L718) | 分配并初始化 `pci_host_bridge` |
| `pci_register_host_bridge()` | [probe.c:991](file:///home/louis/code/linux/drivers/pci/probe.c#L991) | 注册 host bridge，创建 root bus |
| `pci_host_probe()` | [probe.c:3271](file:///home/louis/code/linux/drivers/pci/probe.c#L3271) | 主机桥探测入口（注册 + 枚举 + 添加） |
| `pci_scan_root_bus_bridge()` | [probe.c:3222](file:///home/louis/code/linux/drivers/pci/probe.c#L3222) | 注册 bridge 并扫描总线 |
| `pci_scan_child_bus_extend()` | [probe.c:2894](file:///home/louis/code/linux/drivers/pci/probe.c#L2894) | 核心枚举函数，扫描总线上所有设备 |
| `pci_scan_slot()` | [probe.c:2835](file:///home/louis/code/linux/drivers/pci/probe.c#L2835) | 扫描单个槽位（含多功能） |
| `pci_scan_single_device()` | [probe.c:2787](file:///home/louis/code/linux/drivers/pci/probe.c#L2787) | 扫描单个设备 |
| `pci_scan_device()` | [probe.c:2737](file:///home/louis/code/linux/drivers/pci/probe.c#L2737) | 读取配置空间，创建 `pci_dev` |
| `pci_device_add()` | [probe.c:2680](file:///home/louis/code/linux/drivers/pci/probe.c#L2680) | 初始化能力并注册设备 |
| `pci_init_capabilities()` | [probe.c:2590](file:///home/louis/code/linux/drivers/pci/probe.c#L2590) | 初始化所有 PCIe 能力结构 |
| `pci_scan_bridge_extend()` | [probe.c:1651](file:///home/louis/code/linux/drivers/pci/probe.c#L1651) | 递归扫描桥设备下的子总线 |
| `pci_bus_add_devices()` | [bus.c:377](file:///home/louis/code/linux/drivers/pci/bus.c#L377) | 添加总线上所有设备，触发 probe |
| `pci_bus_add_device()` | [bus.c:345](file:///home/louis/code/linux/drivers/pci/bus.c#L345) | 添加单个设备，触发 probe |
| `__pci_register_driver()` | [pci-driver.c:1464](file:///home/louis/code/linux/drivers/pci/pci-driver.c#L1464) | PCI 驱动注册入口 |
| `pci_bus_match()` | [pci-driver.c:1535](file:///home/louis/code/linux/drivers/pci/pci-driver.c#L1535) | 设备和驱动匹配 |
| `pci_match_device()` | [pci-driver.c:133](file:///home/louis/code/linux/drivers/pci/pci-driver.c#L133) | 执行匹配逻辑（动态 ID + 静态 ID） |
| `pci_match_one_device()` | [pci-driver.c:58](file:///home/louis/code/linux/drivers/pci/pci-driver.c#L58) | 单条 ID 匹配规则 |
| `pci_device_probe()` | [pci-driver.c:551](file:///home/louis/code/linux/drivers/pci/pci-driver.c#L551) | PCI 总线 probe 回调 |
| `__pci_device_probe()` | [pci-driver.c:405](file:///home/louis/code/linux/drivers/pci/pci-driver.c#L405) | 执行 probe 前的匹配检查 |
| `pci_call_probe()` | [pci-driver.c:364](file:///home/louis/code/linux/drivers/pci/pci-driver.c#L364) | 异步/同步执行 probe |
| `local_pci_probe()` | [pci-driver.c:305](file:///home/louis/code/linux/drivers/pci/pci-driver.c#L305) | 最终调用 `drv->probe()` |
| `device_initial_probe()` | [dd.c:1143](file:///home/louis/code/linux/drivers/base/dd.c#L1143) | 内核设备模型初始 probe 入口 |
| `acpi_pci_root_create()` | [pci_root.c:1000](file:///home/louis/code/linux/drivers/acpi/pci_root.c#L1000) | ACPI PCI root 创建 |
| `pci_host_common_probe()` | [pci-host-common.c:85](file:///home/louis/code/linux/drivers/pci/controller/pci-host-common.c#L85) | DT 平台 PCI host probe |

---

## 十一、数据结构字段映射关系

### 枚举阶段字段变化

```
配置空间 (硬件)                     pci_dev (软件)                    pci_bus (软件)
─────────────────                 ────────────────                  ────────────────
Config[0x00] Vendor ID    ─────→  .vendor = 0x8086
Config[0x02] Device ID    ─────→  .device = 0x1234
Config[0x08] Revision     ─────→  .revision = 0x02
Config[0x09] Prog IF      ─┐
Config[0x0A] Subclass     ─┼─→  .class = 0x060000
Config[0x0B] Base Class   ─┘
Config[0x0E] Header Type  ─────→  .hdr_type = 0x01
Config[0x10-0x27] BARs    ─────→  .resource[0..5]
Config[0x2C] Subsys Ven   ─────→  .subsystem_vendor = 0x8086
Config[0x2E] Subsys ID    ─────→  .subsystem_device = 0x0001
Config[0x3C] IRQ Line     ─────→  .irq = 16
Config[0x3D] IRQ Pin      ─────→  .pin = 1

PCIe Extended Capability          .pcie_cap = 0x40
MSI Capability                    .msi_cap = 0x50
MSI-X Capability                  .msix_cap = 0x60
PM Capability                     .pm_cap = 0x70
AER Capability                    .aer_cap = 0x100

Bridge 设备特殊:
Config[0x18] Primary Bus   ──→  .bus->primary = 0x00
Config[0x19] Secondary Bus ──→  .bus->number = 0x01
Config[0x1A] Subordinate   ──→  .subordinate->number = 0x01
                                .bus->self = &bridge_dev
```

### 设备树层次结构

```
pci_bus (root, number=0x00)
├── .devices:
│   ├── pci_dev (devfn=0x00, vendor=0x8086, hdr_type=0x01)  ← 桥设备
│   │   └── .subordinate → pci_bus (secondary, number=0x01)
│   │       ├── .devices:
│   │       │   ├── pci_dev (devfn=0x00, vendor=0x10EC, class=0x020000)  ← 网卡
│   │       │   └── pci_dev (devfn=0x01, vendor=0x8086, class=0x080000)  ← 其他
│   │       └── .children: (可能为空)
│   │
│   ├── pci_dev (devfn=0x08, vendor=0x8086, hdr_type=0x00)  ← 普通设备
│   │
│   └── ...
│
└── .children:
    └── pci_bus (number=0x01, parent=&root_bus)
        └── ...
```

---

## 十二、重要设计点总结

1. **两阶段设备添加**：`pci_device_add()` 只注册设备到内核但不绑定驱动（因为资源尚未分配），`pci_bus_add_devices()` 才通过 `device_initial_probe()` 触发驱动绑定。

2. **递归枚举**：PCI 总线树通过 `pci_scan_bridge_extend() → pci_scan_child_bus_extend()` 递归遍历所有桥设备。第一次扫描只发现一级设备，然后在设备链表中查找桥设备，对每个桥设备递归扫描其子总线。

3. **异步 probe**：`pci_call_probe()` 使用 `pci_probe_wq` 工作队列将 probe 调度到设备所在 NUMA 节点上执行，提高 NUMA 亲和性。

4. **匹配优先级**：`pci_bus_match()` 先检查 `driver_override`，再检查动态 ID（sysfs `new_id`），最后检查静态 `id_table`，支持运行时动态添加设备 ID。

5. **资源管理**：枚举后先 `claim` 固件配置的资源，再 `assign_unassigned` 分配未被固件配置的资源，确保资源不冲突。

6. **能力延迟初始化**：`pci_init_capabilities()` 在 `pci_setup_device()` 之后独立调用，通过 PCI 配置空间的 Capability 链表遍历发现所有能力结构，每种能力独立初始化。

7. **热插拔支持**：`pci_scan_bridge_extend()` 会为支持热插拔的桥预留额外总线号，`pci_scan_child_bus_extend()` 的 `available_buses` 参数用于管理可分配的总线号数量。