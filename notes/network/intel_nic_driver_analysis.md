# Linux 内核 Intel e1000 网卡驱动分析

## 目录

1. [概述](#1-概述)
2. [硬件行为与配置](#2-硬件行为与配置)
   - 2.1 [e1000 硬件概述](#21-e1000-硬件概述)
   - 2.2 [PCI 配置空间](#22-pci-配置空间)
   - 2.3 [MMIO 寄存器空间](#23-mmio-寄存器空间)
   - 2.4 [中断配置](#24-中断配置)
   - 2.5 [硬件特性卸载](#25-硬件特性卸载)
3. [驱动架构与初始化](#3-驱动架构与初始化)
   - 3.1 [PCI 驱动注册](#31-pci-驱动注册)
   - 3.2 [e1000_probe 流程](#32-e1000_probe-流程)
   - 3.3 [e1000_open 流程](#33-e1000_open-流程)
   - 3.4 [e1000_configure 流程](#34-e1000_configure-流程)
4. [核心数据结构](#4-核心数据结构)
   - 4.1 [struct e1000_adapter](#41-struct-e1000_adapter)
   - 4.2 [struct e1000_hw](#42-struct-e1000_hw)
   - 4.3 [struct e1000_tx_ring](#43-struct-e1000_tx_ring)
   - 4.4 [struct e1000_rx_ring](#44-struct-e1000_rx_ring)
   - 4.5 [struct e1000_tx_desc](#45-struct-e1000_tx_desc)
   - 4.6 [struct e1000_rx_desc](#46-struct-e1000_rx_desc)
5. [数据发送路径](#5-数据发送路径)
   - 5.1 [e1000_xmit_frame 发送入口](#51-e1000_xmit_frame-发送入口)
   - 5.2 [TX 描述符填充与 DMA](#52-tx-描述符填充与-dma)
   - 5.3 [发送完成中断处理](#53-发送完成中断处理)
   - 5.4 [发送路径完整调用链](#54-发送路径完整调用链)
6. [数据接收路径](#6-数据接收路径)
   - 6.1 [中断处理与 NAPI 调度](#61-中断处理与-napi-调度)
   - 6.2 [e1000_clean_rx_irq 接收处理](#62-e1000_clean_rx_irq-接收处理)
   - 6.3 [NAPI 轮询机制](#63-napi-轮询机制)
   - 6.4 [接收路径完整调用链](#64-接收路径完整调用链)
7. [NAPI 与中断协同](#7-napi-与中断协同)
   - 7.1 [中断节流 (ITR)](#71-中断节流-itr)
   - 7.2 [NAPI 调度流程](#72-napi-调度流程)
8. [硬件配置与链路管理](#8-硬件配置与链路管理)
   - 8.1 [PHY 管理](#81-phy-管理)
   - 8.2 [链路检测与 Watchdog](#82-链路检测与-watchdog)
   - 8.3 [Wake on LAN](#83-wake-on-lan)
9. [关键函数接口](#9-关键函数接口)
   - 9.1 [net_device_ops 操作向量](#91-net_device_ops-操作向量)
   - 9.2 [PCI 驱动接口](#92-pci-驱动接口)
10. [附录：关键文件列表](#10-附录关键文件列表)

---

## 1. 概述

Intel e1000 驱动是 Linux 内核中用于 Intel PRO/1000 系列千兆以太网控制器的驱动程序。本分析基于内核源码 `drivers/net/ethernet/intel/e1000/` 目录，涵盖驱动初始化、硬件配置、发送/接收路径、NAPI 中断机制和硬件行为。

e1000 系列硬件包括：
- 82542、82543、82544、82545、82546、82547 等 MAC 控制器
- 82540、82541、82545、82546 等集成 PHY 的控制器
- CE4100 等 SoC 集成控制器

---

## 2. 硬件行为与配置

### 2.1 e1000 硬件概述

e1000 控制器是 Intel 的千兆以太网 MAC 控制器，通常与外部 PHY 或集成 PHY 配合使用。硬件架构如下：

```
                      ┌──────────────────────────────┐
                      │        PCI/PCI-X 总线          │
                      └──────────────┬───────────────┘
                                     │
                      ┌──────────────▼───────────────┐
                      │        DMA 引擎               │
                      │   ┌────────┐  ┌────────┐     │
                      │   │ TX DMA │  │ RX DMA │     │
                      │   └───┬────┘  └───┬────┘     │
                      │       │           │           │
                      │   ┌───▼───────────▼────┐      │
                      │   │   FIFO 缓冲        │      │
                      │   └───┬───────────┬────┘      │
                      │       │           │           │
                      │   ┌───▼────┐  ┌───▼────┐      │
                      │   │ TX MAC │  │ RX MAC │      │
                      │   └───┬────┘  └───┬────┘      │
                      └───────┼───────────┼───────────┘
                              │           │
                      ┌───────▼───────────▼───────────┐
                      │          PHY (集成/外部)       │
                      │   ┌──────────────────────┐    │
                      │   │  自动协商 / 链路管理   │    │
                      │   └──────────────────────┘    │
                      └──────────────┬────────────────┘
                                     │
                                     RJ45 接口
```

**硬件关键特性：**
- 支持 10/100/1000 Mbps 速率
- 支持全双工和半双工
- 支持 TCP/IP 校验和卸载（TX/RX）
- 支持 TSO（TCP Segmentation Offload）
- 支持 VLAN 标签卸载
- 支持 Jumbo Frame（最大 16110 字节）
- 支持 Wake on LAN (WoL)
- 支持中断节流 (Interrupt Throttling)

### 2.2 PCI 配置空间

e1000 驱动通过 PCI 子系统与硬件通信，使用标准 PCI 配置空间访问：

```c
// PCI 配置空间读取（在 e1000_init_hw_struct 中）
hw->vendor_id = pdev->vendor;           // 厂商 ID (0x8086 = Intel)
hw->device_id = pdev->device;           // 设备 ID
hw->subsystem_vendor_id = pdev->subsystem_vendor;
hw->subsystem_id = pdev->subsystem_device;
hw->revision_id = pdev->revision;
pci_read_config_word(pdev, PCI_COMMAND, &hw->pci_cmd_word);
```

**BAR 映射：**
- `BAR_0`：MMIO 寄存器空间，通过 `pci_ioremap_bar(pdev, BAR_0)` 映射到 `hw->hw_addr`
- `BAR_1` 及后续：可能包含 IO 端口空间（用于旧硬件）

### 2.3 MMIO 寄存器空间

硬件寄存器通过 MMIO 访问，使用 `er32()` / `ew32()` 宏进行读写：

```c
// 读寄存器宏
#define er32(reg)   readl(hw->hw_addr + reg)

// 写寄存器宏
#define ew32(reg, value)  writel((value), hw->hw_addr + reg)

// 写刷新宏
#define E1000_WRITE_FLUSH()  er32(STATUS)
```

**关键寄存器：**

| 寄存器 | 偏移 | 说明 |
|--------|------|------|
| `E1000_CTRL` | 0x0000 | 设备控制寄存器 |
| `E1000_STATUS` | 0x0008 | 设备状态寄存器 |
| `E1000_ICR` | 0x00C0 | 中断原因寄存器（读清除） |
| `E1000_IMS` | 0x00D0 | 中断掩码设置寄存器 |
| `E1000_IMC` | 0x00D8 | 中断掩码清除寄存器 |
| `E1000_RCTL` | 0x0100 | 接收控制寄存器 |
| `E1000_TCTL` | 0x0400 | 发送控制寄存器 |
| `E1000_TDBAL` | 0x0408 | TX 描述符基址低 32 位 |
| `E1000_TDBAH` | 0x040C | TX 描述符基址高 32 位 |
| `E1000_TDLEN` | 0x0410 | TX 描述符环长度 |
| `E1000_TDH` | 0x0418 | TX 描述符头部指针（硬件读取） |
| `E1000_TDT` | 0x041C | TX 描述符尾部指针（驱动写入） |
| `E1000_RDBAL` | 0x2800 | RX 描述符基址低 32 位 |
| `E1000_RDBAH` | 0x2804 | RX 描述符基址高 32 位 |
| `E1000_RDLEN` | 0x2808 | RX 描述符环长度 |
| `E1000_RDH` | 0x2810 | RX 描述符头部指针（硬件读取） |
| `E1000_RDT` | 0x2818 | RX 描述符尾部指针（驱动写入） |

**控制寄存器关键位：**

```c
/* 设备控制 (CTRL) */
#define E1000_CTRL_FD       0x00000001  /* 全双工 */
#define E1000_CTRL_ASDE     0x00000020  /* 自动速度检测使能 */
#define E1000_CTRL_SLU      0x00000040  /* 强制链路建立 */
#define E1000_CTRL_ILOS     0x00000080  /* 无效 LED 关闭 */
#define E1000_CTRL_RST      0x04000000  /* 全局复位 */

/* 接收控制 (RCTL) */
#define E1000_RCTL_EN       0x00000002  /* 接收使能 */
#define E1000_RCTL_SBP      0x00000004  /* 存储坏包 */
#define E1000_RCTL_UPE      0x00000008  /* 单播混杂模式 */
#define E1000_RCTL_MPE      0x00000010  /* 多播混杂模式 */
#define E1000_RCTL_LPE      0x00000020  /* 长包使能 */
#define E1000_RCTL_LBM      0x000000C0  /* 回环模式 */
#define E1000_RCTL_RDMTS    0x00000300  /* RX 描述符最小阈值 */
#define E1000_RCTL_BAM      0x00008000  /* 广播使能 */
#define E1000_RCTL_BSIZE    0x00030000  /* RX 缓冲区大小 */
#define E1000_RCTL_SECRC    0x04000000  /* 剥除 CRC */

/* 发送控制 (TCTL) */
#define E1000_TCTL_EN       0x00000002  /* 发送使能 */
#define E1000_TCTL_PSP      0x00000008  /* 填充短包 */
#define E1000_TCTL_CT       0x00000FF0  /* 冲突阈值 */
#define E1000_TCTL_COLD     0x003FF000  /* 冲突距离 */
```

### 2.4 中断配置

e1000 支持传统中断和 MSI 中断：

```c
// 中断请求（在 e1000_open 中）
static int e1000_request_irq(struct e1000_adapter *adapter)
{
    struct net_device *netdev = adapter->netdev;
    irq_handler_t handler = e1000_intr;

    // 尝试 MSI 中断
    if (adapter->msi_enabled)
        return pci_request_irq(adapter->pdev, handler, NULL,
                               &adapter->napi, netdev->name, netdev);
    // 回退到传统中断
    return pci_request_irq(adapter->pdev, handler, NULL,
                           netdev, netdev->name, netdev);
}
```

**中断节流 (ITR)：**
- 通过 `E1000_ITR` 寄存器控制中断频率
- `E1000_MIN_ITR_USECS` = 10us (100000 irq/sec)
- `E1000_MAX_ITR_USECS` = 10000us (100 irq/sec)
- 驱动根据流量动态调整 ITR 值

### 2.5 硬件特性卸载

e1000 硬件支持多种协议卸载功能，在 `e1000_probe` 中配置：

```c
// 硬件特性卸载
netdev->hw_features = NETIF_F_SG |           // 分散/聚合 I/O
                      NETIF_F_HW_CSUM |      // 硬件校验和计算
                      NETIF_F_HW_VLAN_CTAG_RX; // 硬件 VLAN 接收标记

if ((hw->mac_type >= e1000_82544) && (hw->mac_type != e1000_82547))
    netdev->hw_features |= NETIF_F_TSO;      // TCP 分段卸载

netdev->features |= netdev->hw_features;
netdev->hw_features |= NETIF_F_RXCSUM |      // 接收校验和
                       NETIF_F_RXALL |        // 接收所有包
                       NETIF_F_RXFCS;         // 保留 FCS
```

---

## 3. 驱动架构与初始化

### 3.1 PCI 驱动注册

定义在 [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) 中：

```c
static struct pci_driver e1000_driver = {
    .name     = e1000_driver_name,
    .id_table = e1000_pci_tbl,       // PCI 设备 ID 表
    .probe    = e1000_probe,         // 设备探测
    .remove   = e1000_remove,        // 设备移除
    .driver.pm = pm_sleep_ptr(&e1000_pm_ops),  // 电源管理
    .shutdown = e1000_shutdown,      // 关机
    .err_handler = &e1000_err_handler,  // PCI 错误处理
};

module_pci_driver(e1000_driver);
```

### 3.2 e1000_probe 流程

```
e1000_probe(pdev, ent)  [e1000_main.c:915]
  │
  ├── pci_enable_device() / pci_enable_device_mem() → 启用 PCI 设备
  ├── pci_request_selected_regions() → 请求 PCI 资源
  ├── pci_set_master() → 启用总线主控 DMA
  │
  ├── alloc_etherdev(sizeof(struct e1000_adapter)) → 分配 net_device
  ├── SET_NETDEV_DEV() → 设置设备关联
  │
  ├── e1000_init_hw_struct() → 初始化硬件结构体
  │   ├── 读取 PCI 配置空间（vendor, device, revision 等）
  │   ├── e1000_set_mac_type() → 识别 MAC 类型
  │   └── 设置 max_frame_size / min_frame_size
  │
  ├── pci_ioremap_bar(pdev, BAR_0) → 映射 MMIO 寄存器空间
  │
  ├── dma_set_mask_and_coherent() → 设置 DMA 掩码（64位/32位）
  │
  ├── 设置 net_device 操作：
  │   ├── netdev->netdev_ops = &e1000_netdev_ops
  │   ├── e1000_set_ethtool_ops() → ethtool 操作
  │   ├── netif_napi_add() → 注册 NAPI
  │   └── netdev->watchdog_timeo = 5 * HZ
  │
  ├── e1000_sw_init() → 软件初始化
  │
  ├── 设置硬件特性卸载标志 (NETIF_F_*)
  │
  ├── e1000_reset_hw() → 硬件复位
  ├── e1000_validate_eeprom_checksum() → 验证 EEPROM 校验和
  ├── e1000_read_mac_addr() → 读取 MAC 地址
  ├── eth_hw_addr_set() → 设置 MAC 地址
  │
  ├── 初始化工作队列：
  │   ├── INIT_DELAYED_WORK(&watchdog_task, e1000_watchdog)
  │   ├── INIT_DELAYED_WORK(&fifo_stall_task, ...)
  │   ├── INIT_DELAYED_WORK(&phy_info_task, ...)
  │   └── INIT_WORK(&reset_task, e1000_reset_task)
  │
  └── register_netdev(netdev) → 注册网络设备
```

### 3.3 e1000_open 流程

```
e1000_open(netdev)  [e1000_main.c:1352]
  │
  ├── netif_carrier_off(netdev) → 初始链路状态为 down
  │
  ├── e1000_setup_all_tx_resources() → 分配 TX 描述符环
  │   └── e1000_setup_tx_resources()
  │       ├── dma_alloc_coherent() → 分配 DMA 一致的描述符内存
  │       └── 初始化 buffer_info 数组
  │
  ├── e1000_setup_all_rx_resources() → 分配 RX 描述符环
  │   └── e1000_setup_rx_resources()
  │       ├── dma_alloc_coherent() → 分配 DMA 一致的描述符内存
  │       └── 初始化 buffer_info 数组
  │
  ├── e1000_power_up_phy() → PHY 上电
  ├── e1000_configure() → 配置硬件寄存器
  │
  ├── e1000_request_irq() → 注册中断处理函数
  │
  ├── clear_bit(__E1000_DOWN) → 标志设备 up
  ├── napi_enable() → 启用 NAPI
  ├── netif_queue_set_napi() → 关联队列与 NAPI
  └── e1000_irq_enable() → 启用硬件中断
```

### 3.4 e1000_configure 流程

```
e1000_configure(adapter)  [e1000_main.c:357]
  │
  ├── e1000_set_rx_mode() → 设置接收模式（混杂、多播等）
  ├── e1000_restore_vlan() → 恢复 VLAN 配置
  ├── e1000_init_manageability() → 初始化管理功能
  │
  ├── e1000_configure_tx() → 配置 TX 引擎
  │   ├── 设置 TCTL 寄存器（使能、冲突阈值、填充短包）
  │   ├── 设置 TX 描述符环基址 (TDBAL/TDBAH)
  │   ├── 设置 TX 描述符环长度 (TDLEN)
  │   └── 初始化 TDH/TDT 指针
  │
  ├── e1000_setup_rctl() → 设置接收控制寄存器
  │   ├── 设置 RCTL 使能位
  │   ├── 配置缓冲区大小
  │   └── 配置广播/混杂模式
  │
  ├── e1000_configure_rx() → 配置 RX 引擎
  │   ├── 设置 RX 描述符环基址 (RDBAL/RDBAH)
  │   ├── 设置 RX 描述符环长度 (RDLEN)
  │   └── 初始化 RDH/RDT 指针
  │
  └── adapter->alloc_rx_buf() → 分配 RX 数据缓冲区
      └── e1000_alloc_rx_buffers()
```

---

## 4. 核心数据结构

### 4.1 struct e1000_adapter

定义在 [drivers/net/ethernet/intel/e1000/e1000.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000.h) 中，是驱动的私有数据结构，通过 `netdev_priv(netdev)` 获取。

```c
struct e1000_adapter {
    /* VLAN */
    unsigned long active_vlans[BITS_TO_LONGS(VLAN_N_VID)];
    u16 mng_vlan_id;

    /* 统计信息 */
    u32 bd_number;
    u32 rx_buffer_len;
    u32 wol;                    /* Wake on LAN 标志 */
    u32 en_mng_pt;
    u16 link_speed;             /* 当前链路速度 */
    u16 link_duplex;            /* 当前链路双工模式 */
    spinlock_t stats_lock;

    /* TX 相关 */
    struct e1000_tx_ring *tx_ring;      /* TX 环（每个队列一个） */
    unsigned int restart_queue;
    u32 txd_cmd;                /* TX 描述符默认命令 */
    u32 tx_int_delay;           /* TX 中断延迟 */
    u32 tx_abs_int_delay;       /* TX 绝对中断延迟 */
    u64 gotcl_old;              /* 统计用 */
    u32 tx_timeout_count;       /* TX 超时计数 */
    u8  tx_timeout_factor;
    atomic_t tx_fifo_stall;     /* FIFO 暂停标志 */
    bool detect_tx_hung;

    /* RX 相关 */
    bool (*clean_rx)(struct e1000_adapter *, struct e1000_rx_ring *,
                     int *, int);  /* RX 清理函数指针 */
    void (*alloc_rx_buf)(struct e1000_adapter *, struct e1000_rx_ring *,
                         int);     /* RX 缓冲区分配函数指针 */
    struct e1000_rx_ring *rx_ring;  /* RX 环（每个队列一个） */
    struct napi_struct napi;        /* NAPI 结构 */

    int num_tx_queues;          /* TX 队列数量 */
    int num_rx_queues;          /* RX 队列数量 */

    u64 hw_csum_err;            /* 硬件校验和错误计数 */
    u64 hw_csum_good;           /* 硬件校验和正确计数 */
    u32 alloc_rx_buff_failed;   /* RX 缓冲区分配失败计数 */
    u32 rx_int_delay;
    u32 rx_abs_int_delay;
    bool rx_csum;               /* RX 校验和卸载使能 */

    /* OS 定义的结构 */
    struct net_device *netdev;  /* 网络设备 */
    struct pci_dev *pdev;       /* PCI 设备 */

    /* 硬件相关结构 */
    struct e1000_hw hw;         /* 硬件抽象层 */
    struct e1000_hw_stats stats; /* 硬件统计 */
    struct e1000_phy_info phy_info;
    struct e1000_phy_stats phy_stats;

    /* 中断节流 */
    u32 itr;                    /* 当前中断节流率 */
    u32 itr_setting;            /* ITR 设置 */
    u16 tx_itr;
    u16 rx_itr;

    /* 工作队列 */
    struct work_struct reset_task;
    struct delayed_work watchdog_task;
    struct delayed_work fifo_stall_task;
    struct delayed_work phy_info_task;

    /* 状态标志 */
    unsigned long flags;
    bool discarding;            /* 丢弃多描述符帧 */
};
```

### 4.2 struct e1000_hw

定义在 [drivers/net/ethernet/intel/e1000/e1000_hw.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.h) 中，硬件抽象层结构，封装了与硬件直接交互的字段。

```c
struct e1000_hw {
    u8 __iomem *hw_addr;            /* MMIO 寄存器基址 */
    u8 __iomem *flash_address;      /* Flash 映射地址 */
    void __iomem *ce4100_gbe_mdio_base_virt;  /* CE4100 MDIO 基址 */

    e1000_mac_type mac_type;        /* MAC 类型 (82542 ~ 82547) */
    e1000_phy_type phy_type;        /* PHY 类型 */
    e1000_media_type media_type;    /* 介质类型 (铜缆/光纤) */

    void *back;                     /* 回指 adapter 结构 */

    /* EEPROM */
    struct e1000_eeprom_info eeprom;

    /* 流控 */
    e1000_fc_type fc;               /* 流控配置 */
    u16 fc_high_water;              /* 流控高水位 */
    u16 fc_low_water;               /* 流控低水位 */
    u16 fc_pause_time;              /* 暂停帧时间 */

    /* 总线信息 */
    e1000_bus_speed bus_speed;      /* 总线速度 */
    e1000_bus_width bus_width;      /* 总线宽度 */
    e1000_bus_type bus_type;        /* 总线类型 */
    unsigned long io_base;          /* IO 端口基址 */

    /* 设备标识 */
    u16 device_id;
    u16 vendor_id;
    u16 subsystem_id;
    u16 subsystem_vendor_id;
    u8 revision_id;

    /* MAC 地址 */
    u8 mac_addr[NODE_ADDRESS_SIZE];     /* 当前 MAC 地址 */
    u8 perm_mac_addr[NODE_ADDRESS_SIZE]; /* 永久 MAC 地址 */

    /* 帧参数 */
    u32 max_frame_size;
    u32 min_frame_size;

    /* 硬件特性 */
    bool get_link_status;           /* 需要查询链路状态 */
    bool serdes_has_link;
    bool tbi_compatibility_en;
    bool phy_reset_disable;
    bool fc_send_xon;
    bool fc_strict_ieee;
};
```

### 4.3 struct e1000_tx_ring

定义在 [drivers/net/ethernet/intel/e1000/e1000.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000.h) 中，TX 描述符环结构。

```c
struct e1000_tx_ring {
    void *desc;                    /* 描述符环虚拟地址 */
    dma_addr_t dma;                /* 描述符环物理地址（DMA 地址） */
    unsigned int size;             /* 描述符环字节数 */
    unsigned int count;            /* 描述符数量 */
    unsigned int next_to_use;      /* 下一个要使用的描述符索引 */
    unsigned int next_to_clean;    /* 下一个要检查 DD 状态的描述符索引 */
    struct e1000_tx_buffer *buffer_info;  /* 缓冲区信息数组 */
    u16 tdh;                       /* 硬件头部指针缓存 */
    u16 tdt;                       /* 硬件尾部指针缓存 */
    bool last_tx_tso;
};
```

### 4.4 struct e1000_rx_ring

定义在 [drivers/net/ethernet/intel/e1000/e1000.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000.h) 中，RX 描述符环结构。

```c
struct e1000_rx_ring {
    void *desc;                    /* 描述符环虚拟地址 */
    dma_addr_t dma;                /* 描述符环物理地址 */
    unsigned int size;             /* 描述符环字节数 */
    unsigned int count;            /* 描述符数量 */
    unsigned int next_to_use;      /* 下一个要使用的描述符索引 */
    unsigned int next_to_clean;    /* 下一个要检查 DD 状态的描述符索引 */
    struct e1000_rx_buffer *buffer_info;  /* 缓冲区信息数组 */
    struct sk_buff *rx_skb_top;    /* 当前正在构建的 skb（大包时） */
    int cpu;                       /* 绑定的 CPU */
    u16 rdh;                       /* 硬件头部指针缓存 */
    u16 rdt;                       /* 硬件尾部指针缓存 */
};
```

### 4.5 struct e1000_tx_desc

定义在 [drivers/net/ethernet/intel/e1000/e1000_hw.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.h) 中，TX 描述符格式。

```c
struct e1000_tx_desc {
    __le64 buffer_addr;            /* 数据缓冲区 DMA 地址 */
    union {
        __le32 data;
        struct {
            __le16 length;         /* 数据缓冲区长度 */
            u8 cso;               /* 校验和偏移 */
            u8 cmd;               /* 描述符控制命令 */
        } flags;
    } lower;
    union {
        __le32 data;
        struct {
            u8 status;            /* 描述符状态 */
            u8 css;               /* 校验和开始 */
            __le16 special;
        } fields;
    } upper;
};

/* TX 描述符命令标志 */
#define E1000_TXD_CMD_EOP    0x01000000  /* 包结束 */
#define E1000_TXD_CMD_IFCS   0x02000000  /* 插入 FCS */
#define E1000_TXD_CMD_IC     0x04000000  /* 插入校验和 */
#define E1000_TXD_CMD_RS     0x08000000  /* 报告状态 */
#define E1000_TXD_CMD_RPS    0x10000000  /* 报告包已发送 */
#define E1000_TXD_CMD_DEXT   0x20000000  /* 扩展描述符 */
#define E1000_TXD_CMD_VLE    0x40000000  /* VLAN 标签使能 */
#define E1000_TXD_CMD_IDE    0x80000000  /* 中断延迟使能 */

/* TX 描述符状态标志 */
#define E1000_TXD_STAT_DD    0x01        /* 描述符完成 */
#define E1000_TXD_STAT_EC    0x02        /* 早期警告 */
#define E1000_TXD_STAT_LC    0x04        /* 碰撞 */
#define E1000_TXD_STAT_TU    0x08        /* 传输欠载 */
```

### 4.6 struct e1000_rx_desc

定义在 [drivers/net/ethernet/intel/e1000/e1000_hw.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.h) 中，RX 描述符格式（传统格式）。

```c
struct e1000_rx_desc {
    __le64 buffer_addr;            /* 数据缓冲区 DMA 地址 */
    __le16 length;                 /* DMA 写入的数据长度 */
    __le16 csum;                   /* 数据包校验和 */
    u8 status;                     /* 描述符状态 */
    u8 errors;                     /* 描述符错误 */
    __le16 special;                /* VLAN 标签等 */
};

/* RX 描述符状态标志 */
#define E1000_RXD_STAT_DD    0x01  /* 描述符完成 */
#define E1000_RXD_STAT_EOP   0x02  /* 包结束 */
#define E1000_RXD_STAT_IXSM  0x04  /* IP 校验和 */
#define E1000_RXD_STAT_VP    0x08  /* VLAN 标签存在 */
#define E1000_RXD_STAT_TCPCS 0x20  /* TCP 校验和 */
#define E1000_RXD_STAT_IPCS  0x40  /* IP 校验和 */
#define E1000_RXD_STAT_PIF   0x80  /* 无错误 */

/* RX 描述符错误标志 */
#define E1000_RXD_ERR_CE     0x01  /* CRC 错误 */
#define E1000_RXD_ERR_SE     0x02  /* 符号错误 */
#define E1000_RXD_ERR_SEQ    0x04  /* 序列错误 */
#define E1000_RXD_ERR_CXE    0x10  /* 载波扩展错误 */
#define E1000_RXD_ERR_TCPE   0x20  /* TCP 校验和错误 */
#define E1000_RXD_ERR_IPE    0x40  /* IP 校验和错误 */
#define E1000_RXD_ERR_RXE    0x80  /* RX 数据错误 */
```

---

## 5. 数据发送路径

### 5.1 e1000_xmit_frame 发送入口

定义在 [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) 中，是 `net_device_ops` 的 `ndo_start_xmit` 回调。

```
e1000_xmit_frame(skb, netdev)  [e1000_main.c:3097]
  │
  ├── eth_skb_pad() → 填充短包到 60 字节
  │
  ├── 计算 TSO 参数：
  │   ├── mss = skb_shinfo(skb)->gso_size
  │   └── 根据 mss 调整 max_per_txd
  │
  ├── 计算所需描述符数量：
  │   ├── count++ (offload context 描述符)
  │   ├── count += TXD_USE_COUNT(len) (头部数据)
  │   └── count += TXD_USE_COUNT(frag->size) (各分片)
  │
  ├── e1000_maybe_stop_tx() → 检查 TX 环是否有足够空间
  │
  ├── e1000_tx_queue() → 填充 TX 描述符
  │   ├── 设置 buffer_addr (DMA 地址)
  │   ├── 设置 length (数据长度)
  │   ├── 设置 cmd (EOP, RS, IC, IFCS 等标志)
  │   └── 设置 cso/css (校验和偏移)
  │
  ├── 处理 VLAN 标签
  │
  ├── wmb() → 写内存屏障，确保描述符写入完成
  │
  └── writel(i, hw->hw_addr + tx_ring->tdt) → 更新 TDT 寄存器
      └── 硬件 DMA 引擎开始传输
```

### 5.2 TX 描述符填充与 DMA

TX 描述符环的 DMA 操作：

```
                  ┌──────────────────────────┐
                  │   TX 描述符环 (DMA 一致)  │
                  │                          │
                  │  [0] buffer_addr ────────┼─────► skb->data (DMA 映射)
                  │      length, cmd, status │
                  │  [1] buffer_addr ────────┼─────► skb_frag (DMA 映射)
                  │      length, cmd, status │
                  │  ...                     │
                  │  [N] buffer_addr         │
                  │                          │
                  └──────────────────────────┘
                      ▲              ▲
                      │              │
                   TDH (硬件)    TDT (驱动)
                   (消费端)      (生产端)
```

**TDT 寄存器更新：** 驱动写入 `TDT` 寄存器通知硬件有新包待发送。硬件通过 DMA 读取描述符，从缓冲区中取出数据，添加 MAC 头部和 FCS，通过 PHY 发送到网络。

### 5.3 发送完成中断处理

```
e1000_clean_tx_irq(adapter, tx_ring)  [e1000_main.c:3827]
  │
  ├── 读取 TDH 硬件指针 → 确定硬件已处理到的位置
  │
  └── while (i != tx_ring->next_to_use) 循环
      │
      ├── 检查描述符 status 中的 DD 位
      │
      ├── dma_unmap_single() → 解除 DMA 映射
      │
      ├── dev_kfree_skb_any() → 释放 skb
      │
      └── 更新缓冲区状态
  │
  ├── tx_ring->next_to_clean = i → 更新清理指针
  │
  └── netif_wake_queue() → 如果队列被停用，重新唤醒
```

### 5.4 发送路径完整调用链

```
用户空间: write() / send() / sendmsg()
    │
    ▼
VFS: sock_write_iter() → __sock_sendmsg() → inet_sendmsg()
    │
    ▼
TCP: tcp_sendmsg() → tcp_write_xmit() → __tcp_transmit_skb()
    │
    ▼
IP: ip_queue_xmit() → ip_local_out() → ip_output() → dev_queue_xmit()
    │
    ▼
设备层: __dev_xmit_skb() → sch_direct_xmit() → dev_hard_start_xmit()
    │
    ▼
驱动: e1000_xmit_frame()  [e1000_main.c:3097]
    │
    ├── e1000_tx_queue() → 填充 TX 描述符
    ├── wmb() → 写内存屏障
    └── writel(TDT) → 通知硬件
        │
        ▼
    ┌─── 硬件行为 ──────────────────────────────────┐
    │ 1. 硬件 DMA 引擎读取 TX 描述符                │
    │ 2. DMA 从系统内存读取数据到 FIFO              │
    │ 3. MAC 添加前导码、帧起始定界符               │
    │ 4. 插入 VLAN 标签（如果需要）                 │
    │ 5. 计算并插入 FCS (CRC)                       │
    │ 6. PHY 编码并通过 RJ45 发送                   │
    │ 7. 发送完成后写回 DD 状态位                   │
    │ 8. 触发 TX 中断（如果启用了 RS 位）           │
    └───────────────────────────────────────────────┘
        │
        ▼
中断: e1000_intr() → napi_schedule() → e1000_clean()
    │
    ▼
NAPI: e1000_clean_tx_irq() → 释放已发送的 skb
```

---

## 6. 数据接收路径

### 6.1 中断处理与 NAPI 调度

```
e1000_intr(irq, data)  [e1000_main.c:3746]
  │
  ├── er32(ICR) → 读取中断原因寄存器（读清除）
  │
  ├── 检查 ICR 值：
  │   ├── 0 → IRQ_NONE (非本设备中断)
  │   └── 非 0 → 处理中断
  │
  ├── if (ICR & (RXSEQ | LSC)) → 链路状态变化
  │   ├── hw->get_link_status = 1
  │   └── schedule_delayed_work(&watchdog_task, 1)
  │
  ├── ew32(IMC, ~0) → 禁用所有中断（防止中断风暴）
  ├── E1000_WRITE_FLUSH() → 写刷新
  │
  └── napi_schedule_prep() + __napi_schedule() → 调度 NAPI
      │
      └── 将 adapter->napi 添加到当前 CPU 的 poll_list
```

### 6.2 e1000_clean_rx_irq 接收处理

```
e1000_clean_rx_irq(adapter, rx_ring, work_done, work_to_do)  [e1000_main.c:4356]
  │
  └── while (rx_desc->status & E1000_RXD_STAT_DD) 循环
      │
      ├── dma_rmb() → DMA 读内存屏障
      │
      ├── 读取 status、length 等字段
      │
      ├── e1000_copybreak() → 小包拷贝优化（默认 256 字节）
      │   ├── true → 拷贝到新 skb，回收原缓冲区
      │   └── false → napi_build_skb() 直接包装
      │
      ├── 检查 EOP 位：
      │   ├── 未设置 → 多描述符帧，启用 discarding 标志
      │   └── 已设置 → 正常处理
      │
      ├── 检查 errors 字段：
      │   ├── 有错误 → 检查 TBI 兼容性，或丢弃
      │   └── 无错误 → 继续处理
      │
      ├── 调整长度（减去 FCS 4字节）
      │
      ├── e1000_rx_checksum() → 硬件校验和验证
      │
      ├── skb_put() / skb_trim() → 设置 skb 数据长度
      │
      ├── e1000_receive_skb() → 递送 skb
      │   ├── eth_type_trans() → 设置协议类型
      │   ├── __vlan_hwaccel_put_tag() → 添加 VLAN 标签
      │   └── napi_gro_receive() → 送入 GRO 处理
      │
      └── 清理描述符，分配新缓冲区
```

### 6.3 NAPI 轮询机制

```
e1000_clean(napi, budget)  [e1000_main.c:3827]
  │
  ├── e1000_clean_tx_irq() → 清理 TX 完成描述符
  │
  ├── adapter->clean_rx() → 清理 RX 描述符
  │   (e1000_clean_rx_irq 或 e1000_clean_jumbo_rx_irq)
  │
  ├── 判断是否继续轮询：
  │   ├── work_done == budget → 返回 budget (继续轮询)
  │   └── 否则 → napi_complete_done() 退出轮询
  │       ├── e1000_set_itr() → 动态调整中断节流
  │       └── e1000_irq_enable() → 重新启用中断
  │
  └── return work_done
```

### 6.4 接收路径完整调用链

```
网卡硬件行为 ────────────────────────────────────────────────┐
│ 1. PHY 从 RJ45 接收模拟信号，解码为数字位流                │
│ 2. MAC 检测帧起始定界符，开始接收帧                        │
│ 3. 检查目标 MAC 地址是否匹配                              │
│ 4. DMA 将数据写入 RX 描述符指向的缓冲区                   │
│ 5. 写入完成后，设置描述符 DD 位和 status 字段             │
│ 6. 更新 RDH 指针                                          │
│ 7. 触发中断 (ICR 中相应位置位)                            │
└────────────────────────────────────────────────────────────┘
    │
    ▼
中断: e1000_intr()  [e1000_main.c:3746]
    │
    ├── 读取 ICR → 识别中断源
    ├── 禁用中断 (IMC)
    └── __napi_schedule() → 调度 NAPI
    │
    ▼
软中断: net_rx_action()  [net/core/dev.c]
    │
    ├── 从 poll_list 取出 napi 结构
    └── napi->poll() → e1000_clean()  [e1000_main.c:3827]
        │
        ├── e1000_clean_tx_irq() → TX 完成清理
        │
        └── e1000_clean_rx_irq()  [e1000_main.c:4356]
            │
            └── e1000_receive_skb()  [e1000_main.c:3999]
                │
                └── napi_gro_receive()  [net/core/dev.c]
                    │
                    └── gro_cell_poll() / netif_receive_skb()
                        │
                        └── __netif_receive_skb_core()
                            │
                            ├── VLAN 处理
                            ├── tcp_v4_rcv()  [IP 协议分派]
                            └── ...
```

---

## 7. NAPI 与中断协同

### 7.1 中断节流 (ITR)

e1000 驱动支持动态中断节流，通过 `e1000_set_itr()` 函数实现：

```
e1000_set_itr(adapter)
  │
  ├── 计算 TX 和 RX 的平均包间隔
  │
  ├── 根据流量负载调整 ITR 值：
  │   ├── 高负载 → 降低中断频率 (增大 ITR)
  │   └── 低负载 → 提高中断频率 (减小 ITR)
  │
  └── writel(ITR, hw->hw_addr + E1000_ITR)
```

### 7.2 NAPI 调度流程

```
中断触发
    │
    ▼
e1000_intr() ──── 禁用中断 ──── __napi_schedule()
    │                                               │
    │                                    ┌──────────┘
    │                                    ▼
    │                          net_rx_action() (软中断)
    │                                    │
    │                                    ▼
    │                          e1000_clean() (轮询)
    │                                    │
    │                    ┌───────────────┴───────────────┐
    │                    │                               │
    │               TX 清理                          RX 接收
    │                    │                               │
    │                    └───────────────┬───────────────┘
    │                                    │
    │                    判断 work_done == budget ?
    │                    ├── 是 → 继续轮询 (不启用中断)
    │                    └── 否 → napi_complete_done()
    │                                    │
    │                                    ├── e1000_set_itr()
    │                                    └── e1000_irq_enable()
    │                                             │
    └──────────────────── 启用中断 ────────────────┘
```

---

## 8. 硬件配置与链路管理

### 8.1 PHY 管理

e1000 驱动通过 MDIO 接口管理 PHY：

```c
// PHY 寄存器读写
e1000_read_phy_reg(hw, PHY_CTRL, &mii_reg);
e1000_write_phy_reg(hw, PHY_CTRL, mii_reg);

// PHY 上电
e1000_power_up_phy(adapter)
    │
    ├── e1000_read_phy_reg(PHY_CTRL)
    ├── mii_reg &= ~MII_CR_POWER_DOWN
    └── e1000_write_phy_reg(PHY_CTRL, mii_reg)

// PHY 断电
e1000_power_down_phy(adapter)
    │
    ├── 检查 WoL 和 AMT 状态
    └── 设置 PHY 电源管理寄存器
```

### 8.2 链路检测与 Watchdog

```c
// e1000_watchdog() 定时检查链路状态
// 调度周期：2 秒
e1000_watchdog(adapter)
  │
  ├── 检查链路状态：
  │   ├── 读 STATUS 寄存器
  │   ├── 检查链路速度 (10/100/1000)
  │   └── 检查双工模式 (全双工/半双工)
  │
  ├── 更新介质类型和自动协商设置
  │
  ├── 更新统计计数
  │
  └── 重新调度：schedule_delayed_work(&watchdog_task, 2 * HZ)
```

### 8.3 Wake on LAN

WoL 配置在 probe 时从 EEPROM 读取：

```c
// 在 e1000_probe 中
e1000_read_eeprom(hw, EEPROM_INIT_CONTROL2_REG, 1, &eeprom_data);
adapter->eeprom_wol = (eeprom_data & eeprom_apme_mask) ? 1 : 0;

// 通过 ethtool 可以设置 WoL 标志
// 支持：Magic Packet, 链路状态变化等
```

---

## 9. 关键函数接口

### 9.1 net_device_ops 操作向量

定义在 [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) 中：

```c
static const struct net_device_ops e1000_netdev_ops = {
    .ndo_open           = e1000_open,          // ifconfig up
    .ndo_stop           = e1000_close,         // ifconfig down
    .ndo_start_xmit     = e1000_xmit_frame,    // 发送数据包
    .ndo_set_rx_mode    = e1000_set_rx_mode,   // 设置接收模式
    .ndo_set_mac_address = e1000_set_mac,      // 设置 MAC 地址
    .ndo_tx_timeout     = e1000_tx_timeout,    // TX 超时处理
    .ndo_change_mtu     = e1000_change_mtu,    // MTU 修改
    .ndo_eth_ioctl      = e1000_ioctl,         // 以太网 ioctl
    .ndo_validate_addr  = eth_validate_addr,   // MAC 地址验证
    .ndo_vlan_rx_add_vid  = e1000_vlan_rx_add_vid,  // 添加 VLAN
    .ndo_vlan_rx_kill_vid = e1000_vlan_rx_kill_vid, // 删除 VLAN
    .ndo_fix_features   = e1000_fix_features,  // 特性修正
    .ndo_set_features   = e1000_set_features,  // 设置特性
};
```

### 9.2 PCI 驱动接口

```c
static struct pci_driver e1000_driver = {
    .name     = e1000_driver_name,
    .id_table = e1000_pci_tbl,       // 支持的设备 ID 表
    .probe    = e1000_probe,         // 设备探测/初始化
    .remove   = e1000_remove,        // 设备移除
    .driver.pm = pm_sleep_ptr(&e1000_pm_ops),  // 挂起/恢复
    .shutdown = e1000_shutdown,      // 系统关机
    .err_handler = &e1000_err_handler,  // PCI 错误处理
};
```

---

## 10. 附录：关键文件列表

| 文件路径 | 说明 |
|---------|------|
| [drivers/net/ethernet/intel/e1000/e1000_main.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_main.c) | 主驱动文件（probe、open、xmit、中断、NAPI） |
| [drivers/net/ethernet/intel/e1000/e1000.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000.h) | 驱动私有数据结构定义 |
| [drivers/net/ethernet/intel/e1000/e1000_hw.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.c) | 硬件抽象层实现 |
| [drivers/net/ethernet/intel/e1000/e1000_hw.h](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_hw.h) | 硬件结构体和寄存器定义 |
| [drivers/net/ethernet/intel/e1000/e1000_ethtool.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_ethtool.c) | ethtool 接口实现 |
| [drivers/net/ethernet/intel/e1000/e1000_param.c](file:///home/louis/code/linux/drivers/net/ethernet/intel/e1000/e1000_param.c) | 模块参数定义 |
| [include/linux/netdevice.h](file:///home/louis/code/linux/include/linux/netdevice.h) | net_device 核心结构定义 |
| [include/linux/pci.h](file:///home/louis/code/linux/include/linux/pci.h) | PCI 子系统接口 |