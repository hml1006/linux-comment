# pvpanic — 虚拟化崩溃通知

## 概述

pvpanic 是虚拟化场景下的崩溃通知机制，当 Guest 虚拟机发生 panic 时，通过 MMIO 或 PCI 设备向 Host 发送通知，使 Host 能够及时感知 Guest 的崩溃状态并采取相应措施（如保存内存快照、重启虚拟机等）。

### 工作原理

```
虚拟化环境:
┌─────────────────────────────────────────────────────────────┐
│                        Host                                │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              QEMU / Hypervisor                       │   │
│  │  ┌────────────────────────────────────────────────┐ │   │
│  │  │  pvpanic 设备模拟                              │ │   │
│  │  │  - MMIO 地址空间 (e.g., 0xfed00000)           │ │   │
│  │  │  - PCI 设备 (Vendor: RedHat, Device: 0x0011)  │ │   │
│  │  │  - 监听 Guest 的 I/O 操作                      │ │   │
│  │  └────────────────────────────────────────────────┘ │   │
│  │         ↓                                             │   │
│  │  Guest panic 时收到通知 → 保存状态 / 生成 dump        │   │
│  └──────────────────────────────────────────────────────┘   │
│                              ↑                              │
│                    MMIO / PCI 通信                          │
│                              │                              │
├──────────────────────────────┼──────────────────────────────┤
│                              │                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                     Guest                            │   │
│  │  ┌────────────────────────────────────────────────┐ │   │
│  │  │  pvpanic 驱动                                 │ │   │
│  │  │  - 注册 panic_notifier                        │ │   │
│  │  │  - panic 时向 MMIO/PCI 写入事件码              │ │   │
│  │  └────────────────────────────────────────────────┘ │   │
│  │         ↓                                             │   │
│  │  panic() → panic_notifier → pvpanic_send_event()     │   │
│  │         ↓                                             │   │
│  │  iowrite8(PVPANIC_PANICKED, base) → Host             │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 事件类型

```c
#define PVPANIC_PANICKED      _BITUL(0)    /* Guest 发生 panic */
#define PVPANIC_CRASH_LOADED  _BITUL(1)    /* Guest 已加载 crash 内核 */
#define PVPANIC_SHUTDOWN      _BITUL(2)    /* Guest 正常关闭 */
```

| 事件 | 位 | 描述 |
|------|----|------|
| `PVPANIC_PANICKED` | 0 | Guest 发生 panic |
| `PVPANIC_CRASH_LOADED` | 1 | Guest 已加载 kdump crash 内核 |
| `PVPANIC_SHUTDOWN` | 2 | Guest 正常关闭 |

## 核心数据结构

### struct pvpanic_instance

描述一个 pvpanic 实例：

```c
struct pvpanic_instance {
    void __iomem          *base;           /* MMIO/PCI 基地址 */
    unsigned int          capability;      /* 设备支持的事件能力 */
    unsigned int          events;          /* 当前启用的事件 */
    struct sys_off_handler *sys_off;       /* 系统关闭处理器 */
    struct list_head      list;            /* 实例链表节点 */
};
```

### 全局变量

```c
static struct list_head pvpanic_list;      /* pvpanic 实例链表 */
static spinlock_t pvpanic_lock;            /* 保护链表的自旋锁 */
```

## 核心函数

### pvpanic_send_event()

向所有注册的 pvpanic 设备发送事件：

```c
static void pvpanic_send_event(unsigned int event)
{
    struct pvpanic_instance *pi_cur;

    if (!spin_trylock(&pvpanic_lock))
        return;

    list_for_each_entry(pi_cur, &pvpanic_list, list) {
        if (event & pi_cur->capability & pi_cur->events)
            iowrite8(event, pi_cur->base);
    }
    spin_unlock(&pvpanic_lock);
}
```

### pvpanic_panic_notify()

panic notifier 回调函数：

```c
static int pvpanic_panic_notify(struct notifier_block *nb,
                                unsigned long code, void *unused)
{
    unsigned int event = PVPANIC_PANICKED;

    if (kexec_crash_loaded())
        event = PVPANIC_CRASH_LOADED;

    pvpanic_send_event(event);

    return NOTIFY_DONE;
}
```

### devm_pvpanic_probe()

通用探测函数，初始化 pvpanic 实例：

```c
int devm_pvpanic_probe(struct device *dev, void __iomem *base)
{
    struct pvpanic_instance *pi;

    pi = devm_kmalloc(dev, sizeof(*pi), GFP_KERNEL);
    if (!pi)
        return -ENOMEM;

    pi->base = base;
    pi->capability = PVPANIC_PANICKED | PVPANIC_CRASH_LOADED | PVPANIC_SHUTDOWN;

    /* 通过读取设备寄存器初始化实际能力 */
    pi->capability &= ioread8(base);
    pi->events = pi->capability;

    pi->sys_off = NULL;
    pvpanic_synchronize_sys_off_handler(dev, pi);

    spin_lock(&pvpanic_lock);
    list_add(&pi->list, &pvpanic_list);
    spin_unlock(&pvpanic_lock);

    dev_set_drvdata(dev, pi);

    return devm_add_action_or_reset(dev, pvpanic_remove, pi);
}
```

## 工作流程

### 1. 初始化阶段

```
pvpanic_init()
    → INIT_LIST_HEAD(&pvpanic_list)      /* 初始化实例链表 */
    → spin_lock_init(&pvpanic_lock)       /* 初始化自旋锁 */
    → atomic_notifier_chain_register(&panic_notifier_list, &pvpanic_panic_nb)
        /* 注册 panic notifier，优先级 INT_MAX（最高）*/
```

### 2. 设备探测阶段

**MMIO 设备**:
```
platform_driver_probe()
    → pvpanic_mmio_probe()
        → platform_get_mem_or_io()        /* 获取资源 */
        → devm_ioremap_resource()         /* 映射 MMIO */
        → devm_pvpanic_probe()            /* 初始化实例 */
```

**PCI 设备**:
```
pci_driver_probe()
    → pvpanic_pci_probe()
        → pcim_enable_device()            /* 启用 PCI 设备 */
        → pcim_iomap()                    /* 映射 BAR0 */
        → devm_pvpanic_probe()            /* 初始化实例 */
```

### 3. 崩溃通知阶段

```
panic()
    → atomic_notifier_call_chain(&panic_notifier_list, 0, NULL)
        → pvpanic_panic_notify()          /* 最高优先级被调用 */
            → kexec_crash_loaded()        /* 检查是否有 crash 内核 */
            → pvpanic_send_event(event)
                → spin_lock(&pvpanic_lock)
                → list_for_each_entry(pi, &pvpanic_list, list)
                    → iowrite8(event, pi->base)  /* 写入 MMIO 地址 */
                → spin_unlock(&pvpanic_lock)
```

### 4. 系统关闭阶段

```
kernel_power_off()
    → __orderly_poweroff(true)
        → sys_off_handlers_call(SYS_OFF_MODE_POWER_OFF)
            → pvpanic_sys_off()
                → pvpanic_send_event(PVPANIC_SHUTDOWN)
```

## 能力协商机制

pvpanic 支持能力协商，Guest 通过读取设备寄存器获取 Host 支持的事件类型：

```c
/* 初始化时读取设备能力 */
pi->capability = PVPANIC_PANICKED | PVPANIC_CRASH_LOADED | PVPANIC_SHUTDOWN;
pi->capability &= ioread8(base);  /* 与设备寄存器值做 AND */
```

Host 通过设置 MMIO 寄存器的值来声明支持的事件类型：

| Host 寄存器值 | 支持的事件 |
|--------------|-----------|
| 0x01 | 仅支持 PANICKED |
| 0x03 | 支持 PANICKED + CRASH_LOADED |
| 0x07 | 支持所有事件 |

## sysfs 接口

pvpanic 提供 sysfs 接口供用户配置事件：

```bash
# 查看设备能力
cat /sys/bus/platform/devices/pvpanic-mmio.0/capability
0x7

# 查看当前启用的事件
cat /sys/bus/platform/devices/pvpanic-mmio.0/events
0x7

# 修改启用的事件（只启用 PANICKED）
echo 0x1 > /sys/bus/platform/devices/pvpanic-mmio.0/events
```

## 设备类型

pvpanic 支持两种设备类型：

### MMIO 设备

通过 Device Tree 或 ACPI 描述：

**Device Tree**:
```dts
pvpanic@fed00000 {
    compatible = "qemu,pvpanic-mmio";
    reg = <0xfed00000 0x1>;
};
```

**ACPI**:
```asl
Device (PVP0) {
    Name (_HID, "QEMU0001")
    Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xfed00000, 0x1)
    })
}
```

### PCI 设备

通过 PCI Vendor/Device ID 匹配：

```c
#define PCI_DEVICE_ID_REDHAT_PVPANIC 0x0011

static const struct pci_device_id pvpanic_pci_id_tbl[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_REDHAT, PCI_DEVICE_ID_REDHAT_PVPANIC) },
    {}
};
```

## 编译配置

```
CONFIG_PVPANIC=y                      # pvpanic 核心支持
CONFIG_PVPANIC_MMIO=y                 # MMIO 设备支持
CONFIG_PVPANIC_PCI=y                  # PCI 设备支持
```

## QEMU 配置

### 启用 pvpanic MMIO 设备

```bash
qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -device pvpanic-mmio,addr=0xfed00000 \
    -drive file=guest.img \
    -smp 4 \
    -m 4G
```

### 启用 pvpanic PCI 设备

```bash
qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -device pvpanic-pci \
    -drive file=guest.img \
    -smp 4 \
    -m 4G
```

### QEMU 监控命令

```bash
(qemu) info pvpanic
pvpanic-mmio: enabled
  events: PANICKED CRASH_LOADED SHUTDOWN
  addr: 0xfed00000
```

## 使用场景

### 1. 自动保存崩溃快照

Host 收到 pvpanic 通知后，自动保存虚拟机内存快照：

```bash
# QEMU 监控脚本示例
(qemu) event_add pvpanic
(qemu) event_set pvpanic action=savevm
```

### 2. 集成 Kdump

当 Guest 加载了 crash 内核时，Host 可以配合 Kdump 使用：

```
Guest panic → PVPANIC_CRASH_LOADED → Host 等待 kdump 完成 → 保存 vmcore
```

### 3. 虚拟机健康监控

监控系统可以通过 pvpanic 事件判断虚拟机状态：

```python
import libvirt

conn = libvirt.open()
dom = conn.lookupByName('guest')

# 监听 pvpanic 事件
events = dom.eventRegister(
    libvirt.VIR_DOMAIN_EVENT_ID_PVPANIC,
    callback
)
```

## 代码位置

```
drivers/misc/pvpanic/pvpanic.c         # pvpanic 核心实现
drivers/misc/pvpanic/pvpanic-mmio.c    # MMIO 设备驱动
drivers/misc/pvpanic/pvpanic-pci.c     # PCI 设备驱动
drivers/misc/pvpanic/pvpanic.h         # 内部头文件
include/uapi/misc/pvpanic.h            # 用户空间头文件
Documentation/devicetree/bindings/misc/pvpanic-mmio.txt  # DT 绑定文档
```