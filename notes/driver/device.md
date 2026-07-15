# Linux 设备驱动模型 (Device Model) 核心流程分析

> 本文档覆盖 Linux 内核设备模型的 6 个核心主题:
> 从设备模型初始化到 PCI 设备枚举、驱动注册匹配、资源分配, 以及关键数据结构详解
> 每个流程均以函数调用栈 ASCII 树形式呈现, 并附有关键操作注释

## 目录

- [1. 设备模型核心初始化](#1-设备模型核心初始化)
- [2. PCI 总线驱动探测](#2-pci-总线驱动探测)
  - [2.1 驱动注册与探测调用栈](#21-驱动注册与探测调用栈)
  - [2.2 pci_host_probe 内部枚举过程](#22-pci_host_probe-内部枚举过程)
- [3. PCI 驱动注册流程](#3-pci-驱动注册流程)
- [4. 资源分配过程](#4-资源分配过程)
- [5. device_driver 结构体](#5-device_driver-结构体)
- [6. bus_type 结构体](#6-bus_type-结构体)
- [7. device 结构体](#7-device-结构体)

---

## 1. 设备模型核心初始化

```text
# 设备模型核心初始化 - 函数调用栈 (start_kernel → driver_init)
#
# 在 PCI 总线驱动探测之前, 必须先初始化设备模型的基础设施
# 包括: 设备注册, 总线注册, 类注册, 平台总线初始化等

start_kernel
  │  # 内核 C 语言入口
  │
  └─ rest_init
      │  # 创建 kernel_init 和 kthreadd 进程
      │
      └─ kernel_init
          │  # 1 号 init 进程入口
          │
          └─ kernel_init_freeable
              │  # 可释放的 init 阶段
              │
              ├─ do_basic_setup
              │   │  # 驱动/子系统的基本设置入口
              │   │
              │   └─ driver_init
              │       │  # 设备模型核心初始化
              │       │  # 创建 /sys 下的设备模型目录结构
              │       │
              │       ├─ devtmpfs_init
              │       │   # 初始化 devtmpfs 文件系统
              │       │   # 创建 /dev 目录, 用于动态设备节点管理
              │       │   # 在设备注册时自动创建 /dev/xxx 节点
              │       │
              │       ├─ devices_init
              │       │   # 创建 /sys/devices 目录
              │       │   # 系统中所有设备都在此目录下有子目录
              │       │   # 按拓扑结构 (如 PCI 总线树) 组织
              │       │
              │       ├─ buses_init
              │       │   # 创建 /sys/bus 目录
              │       │   # 每种总线类型在此创建子目录
              │       │   # 如 /sys/bus/pci, /sys/bus/platform
              │       │   # 每个总线目录下有 devices/ 和 drivers/ 子目录
              │       │
              │       ├─ classes_init
              │       │   # 创建 /sys/class 目录
              │       │   # 按设备功能分类组织
              │       │   # 如 /sys/class/net, /sys/class/block
              │       │   # 一个设备可属于多个 class
              │       │
              │       └─ platform_bus_init
              │           # 初始化平台总线 (/sys/bus/platform)
              │           # 注册 platform_bus_type
              │           # 创建 platform 虚拟设备
              │           # 平台总线用于无硬件枚举能力的设备 (SoC)
              │
              └─ do_initcalls
                  │  # 遍历所有 initcall 级别并执行
                  │  # 所有设备驱动通过 initcall 注册
                  │
                  └─ do_initcall_level
                      │  # 执行指定 initcall 级别的所有函数
                      │
                      └─ do_one_initcall
                          # 执行单个 initcall 函数
                          # 如 gen_pci_driver_init

# 设备模型初始化后的 /sys 目录结构:
#
#   /sys/
#   ├── devices/         # 所有设备 (按拓扑结构)
#   │   ├── system/      # 系统设备 (CPU, 内存等)
#   │   ├── pci0000:00/  # PCI 总线域
#   │   │   ├── 0000:00:00.0/  # PCI 设备
#   │   │   └── ...
#   │   └── platform/    # 平台设备
#   │
#   ├── bus/             # 总线类型
#   │   ├── pci/
#   │   │   ├── devices/  # 软链接到 /sys/devices 下的设备
#   │   │   └── drivers/  # PCI 驱动
#   │   ├── platform/
#   │   └── ...
#   │
#   ├── class/           # 设备功能分类
#   │   ├── net/
#   │   ├── block/
#   │   ├── input/
#   │   └── ...
#   │
#   └── dev/             # 设备号索引
#       ├── block/       # 块设备号
#       └── char/        # 字符设备号
```

---

## 2. PCI 总线驱动探测

宏 module_platform_driver(gen_pci_driver) 会展开两个函数，并放到 initcall

* gen_pci_driver_init: 注册 gen_pci_driver 平台驱动
* gen_pci_driver_exit: 注销 gen_pci_driver 平台驱动

```c
#define platform_driver_register(drv) \
    __platform_driver_register(drv, THIS_MODULE)

static struct platform_driver gen_pci_driver = {
    .driver = {
        .name = "pci-host-generic",
        .of_match_table = gen_pci_of_match,
    },
    .probe = pci_host_common_probe,
    .remove = pci_host_common_remove,
};
```

## 驱动注册与探测调用栈

```text
# PCI 总线驱动探测 - 函数调用栈 (kernel_init → pci_host_probe)

kernel_init
  │  # 内核 init 进程入口, 1 号进程执行所有初始化
  │
  └─ kernel_init_freeable
      │  # 可释放的 init 阶段, 初始化核心子系统
      │
      └─ do_basic_setup
          │  # 驱动/子系统的基本设置入口
          │
          └─ do_initcalls
              │  # 遍历所有 initcall 级别并执行
              │
              └─ do_initcall_level
                  │  # 执行指定 initcall 级别的所有函数
                  │
                  └─ do_one_initcall
                      │  # 执行单个 initcall 函数
                      │
                      └─ gen_pci_driver_init
                          │  # module_platform_driver 宏展开的 init 函数
                          │  # 注册 gen_pci_driver 平台驱动
                          │
                          └─ __platform_driver_register
                              │  # 平台驱动注册, 封装 driver_register
                              │
                              └─ driver_register
                                  │  # 通用驱动注册接口
                                  │  # 将驱动添加到总线驱动链表
                                  │
                                  └─ bus_add_driver
                                      │  # 将驱动添加到总线驱动列表
                                      │  # 创建 sysfs 属性 (drivers/ 目录)
                                      │
                                      └─ driver_attach
                                          │  # 遍历总线上的所有设备, 尝试匹配
                                          │
                                          └─ bus_for_each_dev
                                              │  # 遍历总线设备链表, 对每个设备调用回调
                                              │
                                              └─ __driver_attach
                                                  │  # 检查驱动是否能匹配设备
                                                  │  # 匹配成功则调用 driver_probe_device
                                                  │
                                                  └─ driver_probe_device
                                                      │  # 检查设备状态, 准备进行 probe
                                                      │
                                                      └─ __driver_probe_device
                                                          │  # 实际执行 probe 前检查
                                                          │  # 获取设备锁, 确保设备未绑定
                                                          │
                                                          └─ really_probe
                                                              │  # 真正执行设备探测
                                                              │  # 初始化设备, 分配资源
                                                              │  # 调用 call_driver_probe
                                                              │
                                                              └─ call_driver_probe
                                                                  │  # 调用驱动的 probe 方法
                                                                  │
                                                                  └─ platform_probe
                                                                      │  # 平台驱动 probe 桥接
                                                                      │  # 从 platform_driver 中提取 probe 函数
                                                                      │
                                                                      └─ pci_host_common_probe
                                                                          │  # PCI host 通用探测函数
                                                                          │  # 解析设备树, 获取 PCI 配置资源
                                                                          │  # 映射配置空间, 初始化 IRQ
                                                                          │
                                                                          └─ pci_host_common_init
                                                                              │  # PCI host 控制器初始化
                                                                              │  # 设置 ECAM 配置空间映射
                                                                              │
                                                                              └─ pci_host_probe
                                                                                  # PCI 主机桥探测入口
                                                                                  # 创建根总线, 扫描 PCI 总线树
                                                                                  # 枚举设备, 分配资源
```

## pci_host_probe 内部枚举过程

```text
# pci_host_probe 内部调用栈 (PCI 总线枚举核心流程)

pci_host_probe
  │  # PCI 主机桥探测入口
  │  # 负责创建根总线并扫描所有 PCI 设备
  │
  ├─ pci_scan_root_bus_bridge
  │   │  # 创建并扫描根总线桥
  │   │  # 参数: struct pci_host_bridge *bridge
  │   │
  │   ├─ pci_create_root_bus
  │   │   │  # 创建根总线结构 (struct pci_bus)
  │   │   │  # 内部操作:
  │   │   │  #   1. 分配 pci_bus 结构体
  │   │   │  #   2. 设置 bus->number = bridge->busnr (通常是 0)
  │   │   │  #   3. 设置 bus->ops = bridge->ops (配置空间访问方法)
  │   │   │  #   4. 设置 bus->sysdata = bridge->sysdata
  │   │   │  #   5. 分配 bus->resource[] 资源数组
  │   │   │  #   6. 注册 bus->dev 设备 (device_register)
  │   │   │  #   7. 将 bus 添加到 pci_root_buses 链表
  │   │   │
  │   │   └─ pci_alloc_child_bus
  │   │       # 为根总线分配子总线列表等内部结构
  │   │
  │   ├─ pci_scan_child_bus_extend
  │   │   │  # 扫描根总线上的所有设备
  │   │   │  # 这是 PCI 枚举的核心函数
  │   │   │
  │   │   ├─ pci_scan_slot
  │   │   │   │  # 扫描总线上的一个槽位 (slot)
  │   │   │   │  # 每个总线有 32 个 slot (0-31)
  │   │   │   │  # 每个 slot 最多 8 个 function
  │   │   │   │
  │   │   │   └─ pci_scan_single_device
  │   │   │       │  # 扫描单个 PCI 设备
  │   │   │       │  # 参数: bus, devfn (slot<<3 | function)
  │   │   │       │
  │   │   │       ├─ pci_bus_read_dev_vendor_id
  │   │   │       │   # 读取 Vendor ID 和 Device ID
  │   │   │       │   # Vendor ID = 0xFFFF 表示该槽位无设备
  │   │   │       │
  │   │   │       ├─ pci_alloc_dev
  │   │   │       │   # 分配 struct pci_dev 结构体
  │   │   │       │   # 设置 dev->bus = bus, dev->devfn = devfn
  │   │   │       │
  │   │   │       └─ pci_device_add
  │   │   │           │  # 将 PCI 设备添加到系统
  │   │   │           │  # 初始化设备并注册到设备模型
  │   │   │           │
  │   │   │           ├─ pci_setup_device
  │   │   │           │   │  # 读取 PCI 配置空间, 初始化设备信息
  │   │   │           │   │
  │   │   │           │   ├─ pci_read_bases
  │   │   │           │   │   # 读取 BAR (Base Address Register)
  │   │   │           │   │   # 最多 6 个 BAR (PCI) 或 2 个 BAR (PCIe)
  │   │   │           │   │   # 读取每个 BAR 的地址和大小
  │   │   │           │   │
  │   │   │           │   ├─ pci_read_irq
  │   │   │           │   │   # 读取中断引脚和中断线
  │   │   │           │   │   # 从配置空间 0x3C/0x3D 偏移读取
  │   │   │           │   │
  │   │   │           │   ├─ pci_set_class
  │   │   │           │   │   # 设置设备分类 (Class Code)
  │   │   │           │   │   # 包括 Base Class, Sub Class, Interface
  │   │   │           │   │
  │   │   │           │   └─ pci_set_master
  │   │   │           │       # 设置设备为 Bus Master
  │   │   │           │       # 设置命令寄存器的 Bus Master 位
  │   │   │           │
  │   │   │           ├─ device_initialize
  │   │   │           │   # 初始化通用设备结构 (struct device)
  │   │   │           │   # 设置 dev->bus = &pci_bus_type
  │   │   │           │   # 设置 dev->parent = bridge->dev
  │   │   │           │   # 设置 dev->release = pci_release_dev
  │   │   │           │   # 初始化 kobject 和 klist
  │   │   │           │
  │   │   │           └─ pci_bus_add_device
  │   │   │               │  # 将 PCI 设备添加到总线设备列表
  │   │   │               │
  │   │   │               └─ device_add
  │   │   │                   │  # 将设备注册到 Linux 设备模型
  │   │   │                   │  # 创建 sysfs 条目
  │   │   │                   │  # 触发 uevent 事件
  │   │   │                   │
  │   │   │                   └─ bus_probe_device
  │   │   │                       │  # 触发设备探测
  │   │   │                       │
  │   │   │                       └─ device_initial_probe
  │   │   │                           # 对设备执行初始 probe
  │   │   │                           # 调用 device_attach
  │   │   │                           # 遍历总线驱动列表, 匹配驱动
  │   │   │
  │   │   └─ pci_scan_bridge_extend
  │   │       # 扫描 PCI-PCI 桥后面的次级总线
  │   │       # 递归调用 pci_scan_child_bus_extend
  │   │       # 处理桥的 Primary/Secondary/Subordinate 总线号
  │   │       # 实现完整的 PCI 总线树枚举
  │   │
  │   └─ pci_bus_add_devices
  │       # 添加所有已扫描到的 PCI 设备
  │       # 对总线上每个设备调用 pci_bus_add_device
  │       # 触发设备与 PCI 驱动的匹配和绑定
  │
  └─ pci_assign_unassigned_resources
      # 为未分配资源的设备分配资源
      # 包括 MMIO, IO 端口, 总线号等
      # 在热插拔场景中尤为重要
      #
      # 内部流程:
      #   pci_bus_assign_resources
      #     ├─ pci_setup_bridge  - 配置 PCI-PCI 桥的窗口
      #     └─ pci_assign_resource - 分配 BAR 资源
```

# PCI 驱动注册流程

`__pci_register_driver` 有 3 种调用路径:

**路径 1: `module_pci_driver` 宏** (最常用, 适用于可加载模块)
```c
// include/linux/pci.h:1695
#define module_pci_driver(__pci_driver) \
    module_driver(__pci_driver, pci_register_driver, pci_unregister_driver)

// module_driver 展开为 (include/linux/device/driver.h:266):
//   static int __init __pci_driver##_init(void) { return pci_register_driver(&__pci_driver); }
//   module_init(__pci_driver##_init);
```

**路径 2: `builtin_pci_driver` 宏** (适用于内置驱动)
```c
// include/linux/pci.h:1708
#define builtin_pci_driver(__pci_driver) \
    builtin_driver(__pci_driver, pci_register_driver)

// builtin_driver 展开为 (include/linux/device/driver.h:293):
//   static int __init __pci_driver##_init(void) { return pci_register_driver(&__pci_driver); }
//   device_initcall(__pci_driver##_init);
```

**路径 3: 直接调用 `pci_register_driver`** (少数驱动在自定义 init 函数中调用)
```c
// include/linux/pci.h:1673
// pci_register_driver 必须定义为宏, 以便 KBUILD_MODNAME 在调用处展开
#define pci_register_driver(driver) \
    __pci_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)
```

```text
# PCI 驱动注册流程 - 函数调用栈 (do_initcalls → pci_device_probe)
#
# PCI 驱动通过 initcall 或模块加载注册, 最终都进入 __pci_register_driver
# 注册时遍历总线上已存在的设备, 尝试匹配和绑定
# 匹配成功后调用驱动的 probe 函数

kernel_init
  │  # 内核 init 进程入口, 1 号进程执行所有初始化
  │
  └─ kernel_init_freeable
      │  # 可释放的 init 阶段
      │
      └─ do_basic_setup
          │  # 驱动/子系统的基本设置入口
          │
          └─ do_initcalls
              │  # 遍历所有 initcall 级别并执行
              │  # 所有设备驱动的 init 函数都在此调用
              │
              └─ do_initcall_level
                  │  # 执行指定 initcall 级别的所有函数
                  │  # 如 device_initcall 级别 (level 6)
                  │
                  └─ do_one_initcall
                      │  # 执行单个 initcall 函数
                      │
                      └─ xxx_driver_init
                          │  # module_pci_driver / builtin_pci_driver 宏展开的 init 函数
                          │  # 如 e1000_driver_init, ahci_driver_init, nvme_driver_init
                          │  # 内部调用 pci_register_driver(&xxx_driver)
                          │
                          └─ pci_register_driver
                              │  # 宏, 展开为:
                              │  # __pci_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)
                              │
                              └─ __pci_register_driver
                                  │  # PCI 驱动注册核心
                                  │  # 参数: struct pci_driver * (如 e1000_driver)
                                  │  # 设置 drv->driver.bus = &pci_bus_type
                                  │  # 设置 drv->driver.owner = owner
                                  │  # 设置 drv->driver.mod_name = mod_name
                                  │  # 初始化 dynids 动态 ID 列表
                                  │
                                  └─ driver_register
                                      │  # 通用驱动注册接口 (设备模型核心)
                                      │
                                      └─ bus_add_driver
                                          │  # 将驱动添加到总线驱动列表
                                          │  # 在 sysfs 中创建 driver 目录
                                          │  # 如 /sys/bus/pci/drivers/e1000/
                                          │
                                          ├─ driver_register_sysfs
                                          │   │  # 注册驱动的 sysfs 属性
                                          │   │
                                          │   └─ sysfs_create_groups
                                          │       # 创建驱动属性组
                                          #       # 如 /sys/bus/pci/drivers/e1000/uevent
                                          │
                                          ├─ driver_add_groups
                                          │   # 添加驱动默认属性组
                                          │   # 如 /sys/bus/pci/drivers/e1000/module
                                          │
                                          └─ driver_attach
                                              │  # 遍历总线上的所有设备, 尝试匹配
                                              │
                                              └─ bus_for_each_dev
                                                  │  # 遍历 pci_bus_type 设备链表
                                                  │  # 对每个设备调用 __driver_attach
                                                  │
                                                  └─ __driver_attach
                                                      │  # 检查驱动是否能匹配设备
                                                      │  # 匹配成功则调用 driver_probe_device
                                                      │
                                                      ├─ driver_match_device
                                                      │   │  # 调用 bus->match (pci_bus_match)
                                                      │   │  # 检查设备 ID 是否匹配
                                                      │   │
                                                      │   └─ pci_bus_match
                                                      │       │  # PCI 总线匹配函数
                                                      │       │  # 参数: struct device *dev, struct device_driver *drv
                                                      │       │
                                                      │       └─ pci_match_device
                                                      │           │  # PCI 设备匹配核心
                                                      │           │  # 依次检查驱动支持的 pci_device_id 表
                                                      │           │
                                                      │           └─ pci_match_id
                                                      │               │  # 遍历 pci_driver->id_table
                                                      │               │  # 按以下优先级匹配:
                                                      │               │
                                                      │               └─ pci_device_id 匹配顺序:
                                                      │                   │  1. vendor & device 精确匹配
                                                      │                   │     drv->id_table[i].vendor == dev->vendor
                                                      │                   │     drv->id_table[i].device == dev->device
                                                      │                   │
                                                      │                   │  2. class 匹配
                                                      │                   │     drv->id_table[i].class == dev->class
                                                      │                   │     class_mask 控制匹配精度
                                                      │                   │
                                                      │                   │  3. subvendor & subdevice 匹配
                                                      │                   │     用于特定子系统修订版的匹配
                                                      │                   │
                                                      │                   │  4. PCI_ANY_ID 通配符
                                                      │                   │     vendor == PCI_ANY_ID (0xFFFF)
                                                      │                   │     匹配所有厂商的设备
                                                      │                   │
                                                      │                   └─ 匹配结果:
                                                      │                       # 成功: 返回匹配的 pci_device_id 指针
                                                      │                       # 失败: 返回 NULL, 驱动不绑定此设备
                                                      │
                                                      └─ driver_probe_device
                                                          │  # 驱动-设备匹配成功后, 准备 probe
                                                          │
                                                          └─ __driver_probe_device
                                                              │  # 检查设备是否已绑定驱动
                                                              │  # 获取设备锁, 确保设备可用
                                                              │
                                                              └─ really_probe
                                                                  │  # 真正执行设备探测
                                                                  │  # 管理设备生命周期状态
                                                                  │
                                                                  ├─ driver_sysfs_add
                                                                  │   # 在 sysfs 中创建设备-驱动链接
                                                                  #   # /sys/bus/pci/drivers/e1000/0000:00:xx.x
                                                                  │
                                                                  ├─ call_driver_probe
                                                                  │   │  # 调用 bus->probe 或 drv->probe
                                                                  │   │
                                                                  │   └─ pci_device_probe
                                                                  │       │  # PCI 设备探测
                                                                  │       │  # 参数: struct device *dev
                                                                  │       │  # 从 dev 中获取 struct pci_dev *
                                                                  │       │
                                                                  │       ├─ pci_assign_device_fixed
                                                                  │       │   # 分配固定的 PCI 资源
                                                                  │       │   # 如固件已分配的 BAR 地址
                                                                  │       │
                                                                  │       ├─ pci_pm_init
                                                                  │       │   # 初始化 PCI 电源管理
                                                                  │       │   # 读取 PM Capabilities
                                                                  │       │   # 设置设备电源状态
                                                                  │       │
                                                                  │       ├─ pci_pme_init
                                                                  │       │   # 初始化 PME (Power Management Event)
                                                                  │       │   # 支持从低功耗状态唤醒
                                                                  │       │
                                                                  │       ├─ pci_acpi_setup
                                                                  │       │   # ACPI 设置 (如果启用)
                                                                  │       │   # 配置 ACPI 电源状态
                                                                  │       │
                                                                  │       ├─ pci_msi_setup_pci_dev
                                                                  │       │   # 初始化 MSI/MSI-X 中断
                                                                  │       │   # 读取 MSI Capability
                                                                  │       │   # 计算可用 MSI 向量数
                                                                  │       │
                                                                  │       └─ pci_driver->probe
                                                                  │           # 调用具体 PCI 驱动的 probe
                                                                  #           # 如 e1000_probe
                                                                  #           # 初始化硬件, 注册网络设备
                                                                  │
                                                                  ├─ pm_runtime_put_suppliers
                                                                  │   # 运行时电源管理: 释放引用
                                                                  │
                                                                  └─ devm_kfree
                                                                      # 释放设备资源管理数据
                                                                      # 清理 probe 过程中的临时分配

# pci_device_id 结构 (定义: include/linux/mod_devicetable.h):
#
# struct pci_device_id {
#     __u32 vendor;      # 厂商 ID (PCI_ANY_ID=0xFFFF 匹配所有)
#     __u32 device;      # 设备 ID
#     __u32 subvendor;   # 子系统厂商 ID
#     __u32 subdevice;   # 子系统设备 ID
#     __u32 class;       # 设备类 (分类代码)
#     __u32 class_mask;  # 类掩码 (控制匹配精度)
#     kernel_ulong_t driver_data;  # 驱动私有数据
# };

# PCI 驱动 ID 表示例 (e1000):
#
# static const struct pci_device_id e1000_id_table[] = {
#     { PCI_VDEVICE(INTEL, 0x1000), .driver_data = e1000_ich8 },
#     { PCI_VDEVICE(INTEL, 0x1001), .driver_data = e1000_ich9 },
#     { ... }
#     { 0, }  # 终止符
# };
```

---

## 4. 资源分配过程

```text
# 资源分配过程 - 函数调用栈 (pci_assign_unassigned_resources 内部)
#
# 在 PCI 设备枚举完成后, 需要为设备分配总线资源
# 包括: MMIO 地址, IO 端口地址, 总线号等
# 资源分配涉及 PCI-PCI 桥的窗口配置

pci_assign_unassigned_resources
  │  # 为未分配资源的设备分配资源入口
  │  # 通常在 pci_scan_root_bus_bridge 后调用
  │
  └─ pci_bus_assign_resources
      │  # 遍历总线树, 为所有设备分配资源
      │
      ├─ pci_setup_bridge (对每个 PCI-PCI 桥)
      │   │  # 配置 PCI-PCI 桥的转发窗口
      │   │
      │   ├─ pci_setup_bridge_io
      │   │   # 配置 IO 端口窗口
      │   │   # 写 bridge->io_base / io_limit 寄存器
      │   │   # 控制桥后面的 IO 端口范围
      │   │
      │   ├─ pci_setup_bridge_mmio
      │   │   # 配置非预取 MMIO 窗口
      │   │   # 写 bridge->memory_base / memory_limit
      │   │   # 控制桥后面的 32 位 MMIO 范围
      │   │
      │   └─ pci_setup_bridge_mmio_pref
      │       # 配置预取 MMIO 窗口
      #       # 写 bridge->pref_memory_base / pref_memory_limit
      #       # 控制桥后面的 64 位 MMIO 范围
      │
      ├─ pci_bridge_check_type
      │   # 检查桥类型, 确定支持的窗口类型
      │
      └─ pci_assign_resource (对每个未分配的设备)
          │  # 为单个设备分配 BAR 资源
          │
          ├─ pci_resource_size
          │   # 读取 BAR 寄存器确定所需大小
          │   # 写全 1 到 BAR, 读出值为大小
          │
          ├─ pci_bus_alloc_resource
          │   # 在总线资源池中分配指定大小的区域
          │   # 考虑对齐要求 (至少 4K 或 128 字节)
          │
          ├─ pcibios_align_resource
          │   # 架构特定的资源对齐
          │   # 某些平台要求特定对齐
          │
          └─ pci_write_config_dword
              # 将分配的地址写入 BAR 寄存器
              # 完成设备资源分配
              # 设备现在可以响应分配的地址范围

# PCI 资源分配优先级:
#   1. 固件分配的固定资源 (ACPI / Device Tree)
#   2. 桥窗口内的资源
#   3. 剩余未分配的资源池
#   4. 如果资源不足, 尝试重新分配
```

# device_driver 结构体

```text
# struct device_driver - 设备驱动结构
# 定义: include/linux/device/driver.h
#
# 设备驱动模型的核心结构, 跟踪系统中所有已知的驱动
# 主要目的是使驱动核心能够将驱动与新设备匹配

struct device_driver {
    ── 基本标识 ──
    name              : const char *          # 驱动名称, 用于匹配
    bus               : const struct bus_type * # 所属总线类型
    owner             : struct module *       # 模块所有者 (THIS_MODULE)
    mod_name          : const char *          # 内置模块的名称

    ── 匹配与绑定 ──
    suppress_bind_attrs  : bool               # 是否禁用 sysfs 绑定/解绑定
    probe_type           : enum probe_type    # 探测类型 (同步/异步)
    of_match_table       : const struct of_device_id *  # 设备树匹配表
    acpi_match_table     : const struct acpi_device_id * # ACPI 匹配表

    ── 属性与电源管理 ──
    groups            : const struct attribute_group **  # 驱动默认属性组
    dev_groups        : const struct attribute_group **  # 设备实例属性组
    pm                : const struct dev_pm_ops *        # 电源管理操作

    ── 私有数据 ──
    p                 : struct driver_private *  # 驱动核心私有数据

    ── 回调函数 ──
    probe             : int (*)(struct device *)       # 探测并绑定驱动
    sync_state        : void (*)(struct device *)      # 同步设备状态
    remove            : int (*)(struct device *)       # 解绑驱动
    shutdown          : void (*)(struct device *)      # 关机静默
    suspend           : int (*)(struct device *, pm_message_t)  # 睡眠
    resume            : int (*)(struct device *)       # 唤醒
    coredump          : void (*)(struct device *)      # 核心转储
};

enum probe_type {
    PROBE_DEFAULT_STRATEGY      # 默认策略
    PROBE_PREFER_ASYNCHRONOUS   # 首选异步探测
    PROBE_FORCE_SYNCHRONOUS     # 强制同步探测
};
```

# bus_type 结构体

```text
# struct bus_type - 总线类型结构
# 定义: include/linux/device/bus.h
#
# 总线是处理器和一个或多个设备之间的通道
# 在设备模型中, 所有设备都通过总线连接
# 即使是内部的、虚拟的、"平台"总线也遵循此模型
# 总线可以相互嵌套, 例如 USB 控制器通常是 PCI 设备

struct bus_type {
    ── 标识与属性 ──
    name              : const char *              # 总线名称
    dev_name          : const char *              # 设备名称格式 (子系统枚举)
    bus_groups        : const struct attribute_group **  # 总线默认属性组
    dev_groups        : const struct attribute_group **  # 设备默认属性组
    drv_groups        : const struct attribute_group **  # 驱动默认属性组

    ── 电源管理 ──
    pm                : const struct dev_pm_ops *  # 电源管理操作
    need_parent_lock  : bool                       # 是否需要锁定父设备

    ── 回调函数: 匹配与生命周期 ──
    match             : int (*)(struct device *, struct device_driver *)
                      # 匹配设备和驱动
                      # PCI 总线使用 pci_bus_match

    uevent            : int (*)(const struct device *, struct kobj_uevent_env *)
                      # 处理 uevent 事件

    probe             : int (*)(struct device *)   # 探测并初始化设备
    sync_state        : void (*)(struct device *)  # 同步设备状态

    remove            : void (*)(struct device *)  # 从总线移除设备
    shutdown          : void (*)(struct device *)  # 关机时静默

    ── 回调函数: 设备管理 ──
    online            : int (*)(struct device *)   # 设备上线 (热插拔)
    offline           : int (*)(struct device *)   # 设备离线 (热插拔)

    ── 回调函数: 电源管理 ──
    suspend           : int (*)(struct device *, pm_message_t)  # 睡眠
    resume            : int (*)(struct device *)   # 唤醒

    ── 回调函数: 功能管理 ──
    num_vf            : int (*)(struct device *)   # 虚拟功能数量 (SR-IOV)
    dma_configure     : int (*)(struct device *)   # DMA 配置
    dma_cleanup       : void (*)(struct device *)  # DMA 清理
    irq_get_affinity  : const struct cpumask *(*)(struct device *, unsigned int)
                      # IRQ 亲和性掩码
};

# 总线通知事件枚举 (bus_notifier_event)
# 用于通知总线上的设备/驱动生命周期变化

enum bus_notifier_event {
    BUS_NOTIFY_ADD_DEVICE         # 设备添加到总线
    BUS_NOTIFY_DEL_DEVICE         # 设备即将移除
    BUS_NOTIFY_REMOVED_DEVICE     # 设备已移除
    BUS_NOTIFY_BIND_DRIVER        # 驱动即将绑定
    BUS_NOTIFY_BOUND_DRIVER       # 驱动已绑定
    BUS_NOTIFY_UNBIND_DRIVER      # 驱动即将解绑
    BUS_NOTIFY_UNBOUND_DRIVER     # 驱动已解绑
    BUS_NOTIFY_DRIVER_NOT_BOUND   # 驱动绑定失败
};
```

# device 结构体

```text
# struct device - 通用设备结构
# 定义: include/linux/device.h
#
# Linux 设备模型中最核心的结构
# 系统中的每一个设备都对应一个 struct device

struct device {
    ── 基本信息 ──
    kobj              : struct kobject            # 内核对象 (sysfs 表示)
    parent            : struct device *           # 父设备指针
    p                 : struct device_private *   # 驱动核心私有数据
    init_name         : const char *              # 设备初始名称
    type              : const struct device_type * # 设备类型

    ── 总线与驱动 ──
    bus               : const struct bus_type *   # 所属总线类型
    driver            : struct device_driver *    # 绑定的驱动
    platform_data     : void *                    # 平台特定数据
    driver_data       : void *                    # 驱动私有数据
    mutex             : struct mutex              # 同步互斥锁

    ── 电源管理 ──
    power             : struct dev_pm_info        # 电源管理信息
    pm_domain         : struct dev_pm_domain *    # 电源域

    ── 设备链接 ──
    links             : struct dev_links_info     # 设备链接 (supplier-consumer)

    ── 能耗模型 ──
    em_pd             : struct em_perf_domain *   # 能耗模型性能域

    ── 引脚控制 ──
    pins              : struct dev_pin_info *     # 引脚控制信息

    ── MSI 中断 ──
    msi               : struct dev_msi_info       # MSI 中断信息

    ── DMA 操作 ──
    dma_ops           : const struct dma_map_ops *  # DMA 映射操作
    dma_mask          : u64 *                       # DMA 掩码
    coherent_dma_mask : u64                         # 一致性 DMA 掩码
    bus_dma_limit     : u64                         # 总线 DMA 限制
    dma_range_map     : const struct bus_dma_region *  # DMA 范围映射
    dma_parms         : struct device_dma_parameters * # DMA 参数
    dma_pools         : struct list_head               # DMA 池列表
    dma_mem           : struct dma_coherent_mem *      # 一致性 DMA 内存
    cma_area          : struct cma *                   # 连续内存分配区
    dma_io_tlb_mem    : struct io_tlb_mem *            # 软件 IO TLB
    dma_io_tlb_pools  : struct list_head               # IO TLB 内存池
    dma_io_tlb_lock   : spinlock_t                     # IO TLB 锁
    dma_uses_io_tlb   : bool                           # 使用 IO TLB 标志

    ── 架构特定 ──
    archdata          : struct dev_archdata        # 架构特定数据

    ── 设备树 / 固件 ──
    of_node           : struct device_node *       # 设备树节点
    fwnode            : struct fwnode_handle *     # 固件设备节点

    ── NUMA ──
    numa_node         : int                        # NUMA 节点号

    ── 设备标识 ──
    devt              : dev_t                      # 设备号
    id                : u32                        # 设备实例 ID

    ── 资源管理 ──
    devres_lock       : spinlock_t                 # 资源锁
    devres_head       : struct list_head           # 资源列表头

    ── 类与属性 ──
    class             : const struct class *       # 设备类
    groups            : const struct attribute_group **  # 属性组

    ── 释放与 IOMMU ──
    release           : void (*)(struct device *)  # 释放回调
    iommu_group       : struct iommu_group *       # IOMMU 组
    iommu             : struct dev_iommu *         # IOMMU 运行时数据

    ── 物理位置 ──
    physical_location : struct device_physical_location *  # 物理位置
    removable         : enum device_removable              # 可移除属性

    ── 设备状态标志 ──
    offline_disabled  : bool    # 禁用离线
    offline           : bool    # 离线状态
    of_node_reused    : bool    # 设备树节点重用
    state_synced      : bool    # 状态已同步
    can_match         : bool    # 可匹配标志
    dma_coherent      : bool    # DMA 一致性
    dma_ops_bypass    : bool    # DMA 操作旁路
    dma_skip_sync     : bool    # 跳过 DMA 同步
    dma_iommu         : bool    # 使用 IOMMU DMA
};
```
