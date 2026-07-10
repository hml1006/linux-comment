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
pci_scan_root_bus_bridge(bridge)
    └── pci_register_host_bridge(bridge)       ← 先注册 bridge
        └── pci_scan_child_bus(bus)            ← 开始枚举
            └── pci_scan_child_bus_extend(bus, 0)
```

### 6.2 核心枚举函数 `pci_scan_child_bus_extend()`

```
pci_scan_child_bus_extend(bus, available_buses)
    │
    ├── [阶段1: 扫描所有设备号]
    │   for (devnr = 0; devnr < 32; devnr++) {
    │       pci_scan_slot(bus, PCI_DEVFN(devnr, 0))
    │   }
    │
    ├── [阶段2: 扫描桥接设备，递归枚举]
    │   list_for_each_entry(dev, &bus->devices, bus_list)
    │       if (pci_is_bridge(dev))
    │           pci_scan_bridge_extend(bus, dev, ...)
    │               └── pci_scan_child_bus_extend(child_bus, ...)  ← 递归
    │
    └── return max_busnr
```

### 6.3 `pci_scan_slot()` — 扫描单个槽位

```
pci_scan_slot(bus, devfn)
    │
    ├── pci_scan_single_device(bus, devfn)  ← 扫描 Function 0
    │
    └── if (dev->multifunction)            ← 多功能设备
            for (fn = 1; fn < 8; fn++)
                pci_scan_single_device(bus, devfn + fn)
```

### 6.4 `pci_scan_single_device()` — 扫描单个设备

```
pci_scan_single_device(bus, devfn)
    │
    ├── pci_get_slot(bus, devfn)           ← 检查设备是否已存在
    │   └── 如果已存在 → 直接返回
    │
    ├── pci_scan_device(bus, devfn)        ← 读取配置空间创建设备
    │   ├── pci_bus_read_dev_vendor_id()   ← 读取 Vendor ID 判断设备存在
    │   │   └── 读 Vendor/Device ID 寄存器 (配置空间偏移 0x00-0x03)
    │   │   └── 读到 0xFFFFFFFF → 设备不存在，返回 NULL
    │   │
    │   ├── pci_alloc_dev(bus)             ← 分配 struct pci_dev
    │   │   └── kzalloc(sizeof(*dev))
    │   │   └── 初始化链表头、spinlock 等
    │   │
    │   └── pci_setup_device(dev)          ← 读取配置空间关键信息
    │       ├── 读取 vendor/device/class/revision/hdr_type
    │       ├── 根据 hdr_type 做不同处理:
    │       │   0x00 (普通设备): pci_read_bases() 读取 6 个 BAR
    │       │   0x01 (PCI桥):    读取 primary/secondary/subordinate 总线号
    │       │                     读取 2 个 BAR + 桥窗口寄存器
    │       │   0x02 (CardBus):  读取 CardBus 寄存器
    │       └── pci_read_irq()              ← 读取 IRQ 引脚/线号
    │
    └── pci_device_add(dev, bus)           ← 添加到内核设备模型
        ├── dev->bus = bus
        ├── dev->devfn = devfn
        ├── dev->dev.bus = &pci_bus_type
        │
        ├── pci_configure_device(dev)       ← 配置 PCIe 功能
        │   ├── pcie_configure_device()     ← 读取 PCIe 宽度/速度
        │   └── 设置 dev->mmio_always_on 等
        │
        ├── device_initialize(&dev->dev)    ← 初始化通用设备
        │
        ├── pci_fixup_device(pci_fixup_header, dev)  ← 执行 Header 修复
        │   └── 遍历 pci_fixup_header 链表，执行匹配的修复函数
        │
        ├── pci_init_capabilities(dev)      ← 读取所有能力结构
        │   ├── pci_find_capability(dev, PCI_CAP_ID_PME)  ← 查找 PM 能力
        │   ├── pci_pm_init(dev)            ← 电源管理初始化
        │   │   └── 读取 PM 能力寄存器，设置 PME 支持位
        │   ├── pci_msi_init(dev)           ← MSI 中断初始化
        │   │   └── 读取 MSI Message Control 寄存器
        │   ├── pci_msix_init(dev)          ← MSI-X 中断初始化
        │   │   └── 读取 MSI-X 能力寄存器
        │   ├── pci_iov_init(dev)           ← SR-IOV 初始化
        │   ├── pci_ats_init(dev)           ← ATS 初始化
        │   ├── pci_aer_init(dev)           ← AER 初始化
        │   ├── pci_acs_init(dev)           ← ACS 初始化
        │   ├── pci_ptm_init(dev)           ← PTM 初始化
        │   ├── pci_aspm_init(dev)          ← ASPM 初始化
        │   └── ... 共 17 种能力检测
        │
        ├── pci_device_add_bus(dev)         ← 添加设备到总线设备链表
        │   └── list_add_tail(&dev->bus_list, &bus->devices)
        │
        └── dev->dev.release = pci_release_dev
```

### 6.5 `pci_init_capabilities()` 详解

```c
// drivers/pci/probe.c
static void pci_init_capabilities(struct pci_dev *dev)
{
    pci_pm_init(dev);              // Power Management Capability
    pci_pcie_cap(dev);             // PCI Express Capability
    pci_configure_relaxed_ordering(dev);
    pci_configure_l1ss(dev);
    pci_msi_init(dev);             // MSI Capability
    pci_msix_init(dev);            // MSI-X Capability
    pci_setup_fw_overrides(dev);   // 固件覆盖
    pci_configure_device(dev);     // 设备配置
    pci_iov_init(dev);             // SR-IOV Capability
    pci_cfg_access_init(dev);
    pci_enable_acs(dev);           // Access Control Services
    pci_ptm_init(dev);             // Precision Time Measurement
    pci_aer_init(dev);             // Advanced Error Reporting
    pci_aspm_init(dev);            // Active State Power Management
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