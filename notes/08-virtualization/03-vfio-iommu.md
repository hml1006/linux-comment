# VFIO / IOMMU

## 1. 概述

IOMMU（I/O Memory Management Unit）为设备 DMA 提供地址翻译与隔离：设备发出的
IOVA（I/O 虚拟地址）经 IOMMU 页表翻译为系统物理地址，从而实现设备直通、DMA 保护与
大页映射。VFIO 是建立在此之上的“非特权用户态驱动框架”，把设备、
中断、DMA 映射通过 fd + ioctl 暴露给 QEMU 等用户态程序。

源码分布：

```
drivers/iommu/             IOMMU 核心（iommu.c、iova.c、dma-iommu.c）与各厂商驱动
drivers/iommu/arm/         ARM SMMU（arm-smmu.c / arm-smmu-v3/）
drivers/iommu/iommufd/     IOMMU 用户态 API（/dev/iommu）
drivers/iommu/io-pgtable-arm.c   ARMv8 长描述符页表（stage-1/stage-2）
drivers/vfio/              VFIO 核心：container、group、iommu type1、virqfd
drivers/vfio/pci/          vfio-pci 总线驱动
drivers/vfio/platform/     vfio-platform（当前未编译）
include/linux/iommu.h      IOMMU 内核 API 与核心结构
include/uapi/linux/vfio.h  VFIO 用户态 ABI
include/uapi/linux/iommufd.h  iommufd 用户态 ABI
```

当前 `.config` 的关键差异（**源码存在 ≠ 已启用**）：

```
CONFIG_IOMMU_API=y               # IOMMU 核心 API 被编译
# CONFIG_IOMMU_SUPPORT is not set # 硬件 IOMMU 支持关闭
CONFIG_VFIO=y
CONFIG_VFIO_GROUP=y
CONFIG_VFIO_CONTAINER=y
CONFIG_VFIO_IOMMU_TYPE1=y
CONFIG_VFIO_VIRQFD=y
CONFIG_VFIO_PCI_CORE=y
CONFIG_VFIO_PCI=y
# CONFIG_VFIO_NOIOMMU is not set
# CONFIG_VFIO_PLATFORM is not set
# CONFIG_IOMMUFD 未设置（n）
# CONFIG_ARM_SMMU_V3 未设置（受 IOMMU_SUPPORT 约束）
# CONFIG_VIRTIO_IOMMU 未设置
# CONFIG_IOMMU_DMA / CONFIG_IOMMU_SVA 未设置
```

`drivers/iommu/Makefile` 中 `obj-$(CONFIG_IOMMU_API) += iommu.o`，因此
`iommu.c` 仍然被编入；但 `obj-$(CONFIG_IOMMU_SUPPORT) += iommu-pages.o`，
且所有硬件驱动都在 `if IOMMU_SUPPORT` 之内，所以当前内核**没有任何 IOMMU 硬件驱动**，
`iommu_device_register()` 不会被调用，设备也不会被 `iommu_probe_device()` 赋予
`iommu_group`。VFIO 虽被编译，但 type1 容器无法真正 attach 设备；
`VFIO_NOIOMMU` 也未启用，因此没有“无 IOMMU 直通”兜底路径。

## 2. IOMMU 核心抽象

### 2.1 struct iommu_domain

`include/linux/iommu.h`：

```c
struct iommu_domain {
    unsigned type;
    enum iommu_domain_cookie_type cookie_type;
    bool is_iommupt;
    const struct iommu_domain_ops *ops;
    const struct iommu_dirty_ops *dirty_ops;
    const struct iommu_ops *owner;
    unsigned long pgsize_bitmap;
    struct iommu_domain_geometry geometry;
    int (*iopf_handler)(struct iopf_group *group);
    union {   /* cookie：DMA / iommufd HWPT / fault handler / SVA mm */
        struct iommu_dma_cookie *iova_cookie;
        struct iommufd_hw_pagetable *iommufd_hwpt;
        struct { iommu_fault_handler_t handler; void *handler_token; };
        struct { struct mm_struct *mm; int users; struct list_head next; };
    };
};
```

域类型（`iommu_domain_alloc()` 的入参）：

| 类型 | 含义 |
| --- | --- |
| `IOMMU_DOMAIN_BLOCKED` | 阻断所有 DMA（默认 do-not-use） |
| `IOMMU_DOMAIN_IDENTITY` | 恒等映射，用于“关闭 IOMMU”语义 |
| `IOMMU_DOMAIN_UNMANAGED` | 由内核使用者（VFIO、vfio-iommu 等）自行管理映射 |
| `IOMMU_DOMAIN_DMA` | DMA-IOMMU 默认域，配合 `iova_cookie` 实现 dma_map_* |
| `IOMMU_DOMAIN_DMA_FQ` | 带延迟刷新的 DMA 域（non-strict） |
| `IOMMU_DOMAIN_SVA` | 共享虚拟地址（PASID）域 |
| `IOMMU_DOMAIN_NESTED` | 嵌套翻译（guest 页表 + host 页表） |

### 2.2 struct iommu_ops

```c
struct iommu_ops {
    bool (*capable)(struct device *dev, enum iommu_cap);
    struct iommu_domain *(*domain_alloc_identity)(struct device *dev);
    struct iommu_domain *(*domain_alloc_paging_flags)(struct device *dev, u32 flags,
                                                      const struct iommu_user_data *);
    struct iommu_domain *(*domain_alloc_paging)(struct device *dev);
    struct iommu_domain *(*domain_alloc_sva)(struct device *dev, struct mm_struct *mm);
    struct iommu_domain *(*domain_alloc_nested)(struct device *dev,
                                                struct iommu_domain *parent, u32 flags,
                                                const struct iommu_user_data *);
    struct iommu_device *(*probe_device)(struct device *dev);
    void (*release_device)(struct device *dev);
    struct iommu_group *(*device_group)(struct device *dev);
    int (*of_xlate)(struct device *dev, const struct of_phandle_args *args);
    int (*def_domain_type)(struct device *dev);
    const struct iommu_domain_ops *default_domain_ops;
    struct iommu_domain *identity_domain, *blocked_domain;
    struct iommu_domain *release_domain, *default_domain;
};
```

`struct iommu_domain_ops` 提供 `attach_dev`/`detach_dev`、`map_pages`/`unmap_pages`、
`iova_to_phys`、`set_dev_pasid`、`read_and_clear_dirty` 等。

### 2.3 group / device / fwspec

`struct iommu_group` 定义在 `drivers/iommu/iommu.c`（对驱动不透明）：

```c
struct iommu_group {
    struct kobject kobj;
    struct list_head devices;
    struct mutex mutex;
    void *iommu_data;
    int id;
    struct iommu_domain *default_domain;
    struct iommu_domain *blocking_domain;
    struct iommu_domain *domain;      /* 当前 attach 的域 */
    unsigned int owner_cnt;
    void *owner;
};
```

group 是“无法被 IOMMU 区分的最小设备集合”：同一 group 内设备必须共享同一地址空间，
VFIO 直通以 group 为最小单位（所以有 ACS 隔离问题）。

`struct dev_iommu`（挂在 `dev->iommu`）保存 `fwspec`、`iommu_dev`、`fault_param`、
`max_pasids`、`attach_deferred` 等；`struct iommu_fwspec` 保存固件描述
（`iommu_fwnode`、`flags`、`ids[]`，用于 OF/ACPI 的 stream ID 映射）。

## 3. DMA remapping 流程

### 3.1 注册与探测

```
IOMMU 驱动 probe
  ├─ iommu_device_register(&iommu->iommu, &ops, hwdev)   /* 挂到 bus 的 iommu 列表 */
  └─ iommu_device_link() / sysfs 节点
设备加入总线/驱动 probe
  └─ iommu_probe_device(dev)
      ├─ iommu_init_device(dev)：of_xlate / 分配 dev->iommu
      ├─ ops->probe_device(dev) → iommu_device
      ├─ ops->device_group(dev) → iommu_group
      ├─ 设置默认域：__iommu_group_set_domain(dev, IOMMU_DOMAIN_DMA 或 IDENTITY)
      └─ ops->probe_finalize(dev)
```

默认域选择由 `ops->def_domain_type(dev)` 与 `iommu_default_passthrough()` 决定；
`iommu_setup_dma_ops()`（`drivers/iommu/dma-iommu.c`）在默认 DMA 域上安装
`iommu_dma_ops`，使 `dma_map_sg()` 等走 IOMMU。

### 3.2 映射与解除

```c
int iommu_map(struct iommu_domain *domain, unsigned long iova,
              phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
    ret = iommu_map_nosync(domain, iova, paddr, size, prot, gfp);
    if (ret) return ret;
    ret = iommu_sync_map(domain, iova, size);
    if (ret) iommu_unmap(domain, iova, size);
    return ret;
}

size_t iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
    struct iommu_iotlb_gather iotlb_gather;
    iommu_iotlb_gather_init(&iotlb_gather);
    ret = __iommu_unmap(domain, iova, size, &iotlb_gather);
    iommu_iotlb_sync(domain, &iotlb_gather);   /* 批量 TLB 失效 */
    return ret;
}
```

批量版本 `iommu_map_sg()` 先探测 `ops->map_pages` 能力，再按页合并；
`iommu_iova_to_phys()` 走 `ops->iova_to_phys` 做软件页表遍历；
`iommu_iotlb_gather` 收集待失效区间，最后一次性 `iotlb_sync` 减少 TLB 刷新开销。
`iommu_map_sg`/`iommu_map` 的权限常量：`IOMMU_READ`、`IOMMU_WRITE`、
`IOMMU_CACHE`、`IOMMU_NOEXEC`、`IOMMU_MMIO`、`IOMMU_PRIV`。

### 3.3 attach 语义

```c
int iommu_attach_group(struct iommu_domain *domain, struct iommu_group *group);
void iommu_detach_group(struct iommu_domain *domain, struct iommu_group *group);
```

attach 会对 group 内每个设备调用 `ops->attach_dev()`，成功后
`group->domain = domain`。同一时刻一个 group 只能属于一个非默认域；
`iommu_group_set_domain()` 系列内部通过 `blocking_domain` 做过渡，避免
“先 detach 再 attach”期间出现未翻译 DMA 的窗口。

## 4. ARM SMMU 与 io-pgtable

### 4.1 SMMUv3

`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c` 实现 ARM SMMUv3：

- `arm_smmu_device_probe()` 解析 MMIO 寄存器、探测 `IDR0`–`IDR5`、
  配置 CMDQ/EVENTQ/PRIQ、建立 Stream Table；
- `arm_smmu_domain_alloc_paging_flags()` 分配 `struct arm_smmu_domain`，
  内部用 `io_pgtable_init_fns`（ARMv8 LPAE 格式）建立 CD（Context Descriptor）；
- `arm_smmu_attach_dev()` 通过 STE（Stream Table Entry）把设备的 stream ID
  指向对应 CD，从而实现 per-device 地址空间；
- `arm_smmu_map_pages()` / `arm_smmu_unmap_pages()` 操作 CD 指向的页表；
- `arm_smmu_iova_to_phys()` 做软件遍历；
- 事件队列处理 `arm_smmu_evtq_thread()`，把 translation fault 上报
  （可经 IOPF 交给 `io-pgfault.c` 或 SVA）。

SMMUv2 由 `drivers/iommu/arm/arm-smmu/arm-smmu.c` 实现，使用
`arm_smmu_domain` + `arm_smmu_cb`（Context Bank）。

### 4.2 io-pgtable

`drivers/iommu/io-pgtable-arm.c` 提供 `arm_lpae_io_pgtable_ops`：

- `arm_lpae_map_pages()` 按 `IAS`/`OAS`/`T0SZ` 建立多级表，支持 4K/16K/64K 粒度
  与 block mapping；
- `arm_lpae_unmap_pages()` 支持部分 unmap 并返回实际解除长度；
- `arm_lpae_iova_to_phys()` 软件遍历；
- `arm_lpae_read_and_clear_dirty()` 支持硬件脏页跟踪（若 SMMU 支持 DBM）。

当前 `CONFIG_IOMMU_IO_PGTABLE_LPAE` 受 `IOMMU_SUPPORT` 约束未启用，
相关代码不参与编译，但接口与实现均存在于源码。

## 5. IOMMU 用户态接口：iommufd

`drivers/iommu/iommufd/` 提供 `/dev/iommu`，是 VFIO container 的新替代：
用户态创建 IOAS（IO 地址空间）、分配 HWPT（硬件页表）、绑定设备，
通过 `IOMMU_IOAS_ALLOC`、`IOMMU_IOAS_MAP`、`IOMMU_IOAS_UNMAP`、
`IOMMU_HWPT_ALLOC`、`IOMMU_DEVICE_GET_INFO`、`IOMMU_VFIO_IOAS` 等 ioctl 操作。
VFIO 侧对应 `drivers/vfio/iommufd.c` 中的 `vfio_iommufd_*` 兼容层。

当前 `CONFIG_IOMMUFD` 未设置，`/dev/iommu` 不存在，`VFIO_DEVICE_CDEV` 也不启用，
只能走传统 container/group 路径。

## 6. VFIO 架构：container / group / device

```
/dev/vfio/vfio             container（一个容器 = 一个 IOMMU 地址空间，由 VFIO_GROUP_SET_CONTAINER 绑定）
/dev/vfio/<group_id>       group（对应 iommu_group）
   └── VFIO_GROUP_GET_DEVICE_FD("0000:01:00.0") 得到设备 fd
       └── VFIO_DEVICE_GET_REGION_INFO / SET_IRQS / RESET
```

核心结构（`drivers/vfio/vfio.h`、`include/linux/vfio.h`）：

```c
struct vfio_group {
    struct device dev;
    struct cdev cdev;
    refcount_t drivers;
    unsigned int container_users;
    struct iommu_group *iommu_group;
    struct vfio_container *container;
    struct list_head device_list;
    enum vfio_group_type type;   /* VFIO_IOMMU / VFIO_NO_IOMMU / EMULATED */
    struct kvm *kvm;
};

struct vfio_device {
    struct device *dev;
    const struct vfio_device_ops *ops;
    const struct vfio_migration_ops *mig_ops;
    const struct vfio_log_ops *log_ops;
    struct vfio_group *group;
    struct vfio_device_set *dev_set;
    unsigned int index;
    struct device device;
    refcount_t refcount;
    unsigned int open_count;
};
```

`struct vfio_device_ops` 由总线驱动实现：`init/release`、`open_device/close_device`、
`read/write/ioctl/mmap`、`bind_iommufd/unbind_iommufd`、`attach_ioas/detach_ioas`、
`dma_unmap`、`device_feature`。vfio-pci 的实现在 `drivers/vfio/pci/`。

## 7. VFIO ioctl 与生命周期

### 7.1 打开 container

```
open("/dev/vfio/vfio") → vfio_fops_open()（kzalloc container、kref_init）
```

`vfio_fops_unl_ioctl()` 处理：

- `VFIO_GET_API_VERSION`：返回 `VFIO_API_VERSION`；
- `VFIO_CHECK_EXTENSION`：查询 `VFIO_TYPE1_IOMMU`、`VFIO_TYPE1v2_IOMMU`、
  `VFIO_UNMAP_UNPIN`、`VFIO_DIRTY_PAGE_TRACKING` 等；
- `VFIO_SET_IOMMU`：选定 IOMMU 后端（如 `VFIO_TYPE1v2_IOMMU`），
  内部 `vfio_iommu_driver_get()`。

### 7.2 打开 group 并绑定

```
open("/dev/vfio/<group_id>")
  └─ vfio_group_fops_open()                 drivers/vfio/group.c
      └─ 按 id 找到 vfio_group，检查 VFIO_GROUP_FLAGS_VIABLE

ioctl(group_fd, VFIO_GROUP_GET_STATUS)
  └─ vfio_group_ioctl_get_status()          /* flags + num_devices */

ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd)
  └─ vfio_group_ioctl_set_container()
      ├─ vfio_group_use_container()
      └─ vfio_container_attach_group()
          └─ iommu_driver->ops->attach_group()
              └─ vfio_iommu_type1_attach_group()

ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, "0000:01:00.0")
  └─ vfio_group_ioctl_get_device_fd()
      └─ vfio_device_open_file() → vfio_pci 的 open_device()
```

group fd 的 ioctl 分发表在 `vfio_group_fops_unl_ioctl()`：
`VFIO_GROUP_GET_DEVICE_FD`、`VFIO_GROUP_GET_STATUS`、
`VFIO_GROUP_SET_CONTAINER`、`VFIO_GROUP_UNSET_CONTAINER`。

### 7.3 设备操作

设备 fd 上由总线驱动处理（`vfio_pci_core_ioctl()`）：

- `VFIO_DEVICE_GET_INFO`：返回 `num_regions`、`num_irqs`、`flags`；
- `VFIO_DEVICE_GET_REGION_INFO`：BAR 与 config 空间区间，可 mmap；
- `VFIO_DEVICE_GET_IRQ_INFO` / `VFIO_DEVICE_SET_IRQS`：INTx/MSI/MSI-X 与 eventfd 绑定；
- `VFIO_DEVICE_RESET`、`VFIO_DEVICE_FEATURE`、`VFIO_DEVICE_IOEVENTFD`。

中断注入依赖 `drivers/vfio/virqfd.c`：`vfio_virqfd_enable()` 把 eventfd 挂到
IRQ bypass 框架，实现 guest 直接收中断而不经宿主。

## 8. vfio_iommu_type1

`drivers/vfio/vfio_iommu_type1.c` 实现“Type1”（IOVA 由用户态指定的 DMA 域）：

```c
struct vfio_iommu {
    struct list_head domain_list;      /* 每个 iommu_domain */
    struct list_head iova_list;
    struct mutex lock;
    struct rb_root dma_list;           /* IOVA → vfio_dma */
    struct list_head device_list;
    unsigned int dma_avail;
    unsigned int vaddr_invalid_count;
    uint64_t pgsize_bitmap;
    bool v2;
    bool dirty_page_tracking;
};

struct vfio_domain {
    struct iommu_domain *domain;
    struct list_head group_list;
    bool enforce_cache_coherency;
};

struct vfio_dma {
    struct rb_node node;
    dma_addr_t iova;
    unsigned long vaddr;
    size_t size;
    int prot;
    bool iommu_mapped;
    struct task_struct *task;
    struct rb_root pfn_list;
    struct mm_struct *mm;
};
```

关键流程：

1. `vfio_iommu_type1_attach_group()`：取 `iommu_group`，
   `iommu_domain_alloc(IOMMU_DOMAIN_UNMANAGED)` 建域；若已有域兼容且开启 v2，
   则复用；否则新建 `vfio_domain` 并 `iommu_attach_group()`；
2. `VFIO_IOMMU_MAP_DMA` → `vfio_iommu_type1_map_dma()`：校验对齐与范围，
   `pin_user_pages_remote()` 固定用户页（`vfio_pin_pages_remote()`），
   再 `iommu_map()`；大页由 `pgsize_bitmap` 与 VMA 对齐情况决定；
3. `VFIO_IOMMU_UNMAP_DMA` → `vfio_iommu_type1_unmap_dma()`：`iommu_unmap()`，
   可选 `unmap_unpin`；`VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP` 取脏页位图；
4. `VFIO_IOMMU_GET_INFO`：返回 `pgsize_bitmap`、`iova_pgsizes`、
   `VFIO_IOMMU_INFO_CAPS`（dirty tracking 等）；
5. `vfio_iommu_type1_pin_pages()`：供 mdev/vfio 内部按 IOVA 取 page；
6. MMU notifier：`vfio_iommu_type1_mmu_notifier` 监听进程地址空间变化，
   把失效 IOVA 加入 `iova_list` 并 `iommu_unmap()`。

## 9. vfio-pci

`drivers/vfio/pci/vfio_pci.c` 注册 PCI 驱动（`vfio_pci_driver`），
`vfio_pci_probe()` → `vfio_pci_core_register_device()`；设备结构：

```c
struct vfio_pci_core_device {
    struct vfio_device vdev;
    struct pci_dev *pdev;
    const struct vfio_pci_device_ops *pci_ops;
    void __iomem *barmap[PCI_STD_NUM_BARS];
    bool bar_mmap_supported[PCI_STD_NUM_BARS];
    u8 *pci_config_map, *vconfig;      /* vconfig 为影子 config space */
    struct perm_bits *msi_perm;
    struct xarray ctx;                 /* 中断上下文（INTx/MSI/MSI-X） */
    int irq_type;
    u8 msi_qmax, msix_bar;
    u16 msix_size;
    u32 msix_offset;
    bool has_dyn_msix:1, nointx:1, disable_idle_d3:1, sriov_active:1;
    struct pci_saved_state *pci_saved_state;
    struct list_head ioeventfds_list;
    struct rw_semaphore memory_lock;
    struct list_head dmabufs;
};
```

要点：

- **BAR 与 config space**：`vfio_pci_core_ioctl()` 处理 `VFIO_DEVICE_GET_REGION_INFO`，
  BAR 可 `mmap`（`vfio_pci_mmap_ops` 直接映射物理 BAR）；`vconfig` 是影子副本，
  写操作经 `vfio_pci_config_write()` 过滤（隐藏 MSI-X table、ROM 只读等）；
- **中断**：`vfio_pci_set_irqs_ioctl()` 分派到 `vfio_pci_intx`、`vfio_pci_msi`、
  `vfio_pci_msix`；eventfd 由 `virqfd` 转发；
- **ioeventfd**：`vfio_pci_ioeventfd()` 把 guest MMIO/PIO 写路由到 eventfd，
  配合 KVM 的 `KVM_IOEVENTFD` 实现零退出；
- **reset / 迁移**：`VFIO_DEVICE_RESET` 走 `vfio_pci_core_reset()`（FLR 或 PM reset）；
  `mig_ops`/`log_ops` 支持状态迁移与脏页日志。

## 10. 当前配置下的实际能力

| 能力 | 源码位置 | 当前配置 |
| --- | --- | --- |
| IOMMU 核心 API | `drivers/iommu/iommu.c` | 编译（`IOMMU_API=y`） |
| ARM SMMUv3 | `drivers/iommu/arm/arm-smmu-v3/` | 未编译（`IOMMU_SUPPORT=n`） |
| io-pgtable LPAE / DMA-IOMMU | `io-pgtable-arm.c` / `dma-iommu.c` | 未编译 |
| VFIO container/group | `drivers/vfio/` | 编译 |
| vfio_iommu_type1 | `drivers/vfio/vfio_iommu_type1.c` | 编译 |
| vfio-pci | `drivers/vfio/pci/` | 编译 |
| iommufd | `drivers/iommu/iommufd/` | 未编译 |
| VFIO no-IOMMU | `VFIO_NOIOMMU` | 未启用 |

结论：`/dev/vfio/vfio`、`/dev/vfio/<iommu_group>` 与 vfio-pci 设备节点可以存在，
但没有 IOMMU 驱动时 `iommu_probe_device()` 不会给设备建立 group，
`vfio_iommu_type1_attach_group()` 拿不到 `iommu_group`，设备直通无法完成 DMA 映射。

## 11. 端到端调用链

```
启用 IOMMU（有硬件驱动时）:
  SMMU probe → iommu_device_register → iommu_device_link
  设备 probe → iommu_probe_device → ops->probe_device → ops->device_group
             → 默认域 IOMMU_DOMAIN_DMA → iommu_setup_dma_ops

VFIO 直通:
  open(/dev/vfio/vfio) → vfio_fops_open
  ioctl(VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU)
  open(/dev/vfio/<gid>) → vfio_group_fops_open
  ioctl(group, VFIO_GROUP_SET_CONTAINER, &container_fd)
    → vfio_container_attach_group → vfio_iommu_type1_attach_group
    → iommu_domain_alloc + iommu_attach_group
  ioctl(group, VFIO_GROUP_GET_DEVICE_FD, "0000:01:00.0")
    → vfio_device_open_file → vfio_pci_core_open
  ioctl(container, VFIO_IOMMU_MAP_DMA, {iova, vaddr, size, prot})
    → vfio_iommu_type1_map_dma → pin_user_pages_remote → iommu_map
  ioctl(dev, VFIO_DEVICE_GET_REGION_INFO/SET_IRQS) + mmap(BAR)
  KVM 侧: KVM_SET_DEVICE_ATTR / KVM_IRQFD 把中断与 MMIO 事件接入 guest
```

## 12. 关键文件清单

| 路径 | 作用 |
| --- | --- |
| `include/linux/iommu.h` | `iommu_domain`、`iommu_ops`、`iommu_device`、`iommu_fwspec`、IOMMU API |
| `include/uapi/linux/vfio.h` | VFIO ioctl ABI、`vfio_region_info`、`vfio_irq_info` |
| `include/uapi/linux/iommufd.h` | iommufd ioctl ABI |
| `include/linux/vfio.h` | `struct vfio_device`、`struct vfio_device_ops` |
| `drivers/vfio/vfio.h` | `struct vfio_group`、`vfio_iommu_driver_ops` |
| `drivers/vfio/vfio_main.c` | VFIO 设备注册/注销、设备 fd 打开 |
| `drivers/vfio/container.c` | container fd、`VFIO_SET_IOMMU`、`VFIO_IOMMU_MAP_DMA` 分发 |
| `drivers/vfio/group.c` | group fd、`VFIO_GROUP_*` ioctl |
| `drivers/vfio/vfio_iommu_type1.c` | Type1 IOMMU：pin/unpin、`iommu_map`、脏页跟踪 |
| `drivers/vfio/virqfd.c` | eventfd 中断转发 |
| `drivers/vfio/iommufd.c` | VFIO 与 iommufd 的兼容层 |
| `drivers/vfio/pci/vfio_pci.c` | vfio-pci PCI 驱动注册 |
| `drivers/vfio/pci/vfio_pci_core.c` | `vfio_pci_core_device`、BAR/config/ioctl/mmap |
| `drivers/vfio/pci/vfio_pci_intrs.c` | INTx/MSI/MSI-X 中断设置 |
| `drivers/iommu/iommu.c` / `iova.c` | IOMMU 核心（group/device/domain）与 IOVA 分配器 |
| `drivers/iommu/dma-iommu.c` | DMA-IOMMU 默认域与 `dma_map_*` 实现 |
| `drivers/iommu/io-pgtable-arm.c` | ARMv8 LPAE 页表操作 |
| `drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c` | SMMUv3 驱动（`.h` 含 CD/STE 定义） |
| `drivers/iommu/iommufd/main.c` | `/dev/iommu` 字符设备与 ioctl 分发 |
| `drivers/iommu/virtio-iommu.c` | virtio-iommu 前端（虚拟化场景） |
