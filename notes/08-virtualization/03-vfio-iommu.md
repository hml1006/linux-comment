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

## 13. VFIO 的设备模型：注册、fd 与转发

第 6 节给出了 `struct vfio_device` 的字段，但没有说明「一个 PCI 设备如何变成
一个可以被 `open()` 的 VFIO 设备」。答案在 `drivers/vfio/vfio_main.c`。

注册分两层：

```c
static int __vfio_register_dev(struct vfio_device *device, ...);
int vfio_register_group_dev(struct vfio_device *device);
void vfio_unregister_group_dev(struct vfio_device *device);
```

- `__vfio_register_dev()`（约 324 行）做真正的 `device_register()`：
  分配 `device->device`、设置 `dev_set`、把 `vfio_device` 挂到父设备下。
  它是内部函数，因为「注册」必须与「加入 group」成对出现。
- `vfio_register_group_dev()`（约 378 行）在 `__vfio_register_dev()` 之上
  把设备加入 `vfio_group->device_list`，并设置 `device->group`。
  vfio-pci 的 `probe` 最终调用的就是它。
- `vfio_unregister_group_dev()`（约 397 行）反向操作，同时等待
  `open_count` 归零。这一点很重要：设备不能被「还有进程持有 fd」时注销，
  否则用户态下一次 ioctl 会命中已释放的 `vfio_device`。

`vfio_device_release()`（约 196 行）是 `device` 的 release 回调，
负责在最后一个引用消失时释放 `vfio_device`。它与 `vfio_device_put()` 配合，
构成「设备引用计数」与「fd 引用计数」两条独立的生命周期：
前者由 group 持有，后者由 `open()` 持有。

用户态拿到的设备 fd 是 `vfio_device_fops`，其中
`vfio_device_fops_mmap()`（约 1431 行）只做一件事：把 mmap 转发给
`device->ops->mmap()`。**VFIO 核心不解释任何 region 语义**——
它不知道 BAR、config space、MSI-X table 分别是什么，这些全在
`drivers/vfio/pci/vfio_pci_core.c` 里。同理，设备 fd 上的 ioctl 也是
`ops->ioctl(device, cmd, arg)` 的直接转发，核心只负责
「找到 device、检查 `open_count`、加锁、调用、解锁」。

这种「核心极薄、总线驱动极厚」的分层有一个直接后果：**不同总线
（PCI / platform / mdev）可以定义完全不同的 region 编号与 ioctl 语义**，
而用户态只能通过 `VFIO_DEVICE_GET_INFO` 返回的
`num_regions`/`num_irqs`/`flags` 来泛化处理。

## 14. container 的生命周期与 SET_IOMMU 的门槛

`open("/dev/vfio/vfio")` 落到 `vfio_fops_open()`（`drivers/vfio/container.c`
约 359 行）：

```c
container = kzalloc_obj(*container, GFP_KERNEL_ACCOUNT);
INIT_LIST_HEAD(&container->group_list);
init_rwsem(&container->group_lock);
kref_init(&container->kref);
filep->private_data = container;
```

注意 `group_lock` 是 **rw_semaphore** 而不是 mutex：因为
`VFIO_GROUP_SET_CONTAINER`/`UNSET_CONTAINER` 会在持有 group 锁的情况下
去拿 container 的锁，而 SET_IOMMU 只需要写侧独占。用 rwsem 可以让
「多个 group 同时查询 container 状态」与「SET_IOMMU 独占修改」共存。

container 的 ioctl 分发表很小：

```c
switch (cmd) {
case VFIO_GET_API_VERSION:  ret = VFIO_API_VERSION; break;
case VFIO_CHECK_EXTENSION:  ret = vfio_container_ioctl_check_extension(container, arg); break;
case VFIO_SET_IOMMU:        ret = vfio_ioctl_set_iommu(container, arg); break;
default:
    driver = container->iommu_driver;
    data = container->iommu_data;
    if (driver)   /* passthrough all unrecognized ioctls */
        ret = driver->ops->ioctl(data, cmd, arg);
}
```

也就是说 `VFIO_IOMMU_MAP_DMA`/`UNMAP_DMA`/`GET_INFO`/`IOMMU_GET_INFO`
等全部是「未识别命令」，被透传给 type1 后端。这个设计让「新增一个
IOMMU 后端」不需要改核心分发表，只要注册自己的 `vfio_iommu_driver_ops`。

`VFIO_SET_IOMMU` 的门槛值得逐条读（约 260 行）：

```c
down_write(&container->group_lock);
if (list_empty(&container->group_list) || container->iommu_driver) {
    up_write(&container->group_lock);
    return -EINVAL;
}
mutex_lock(&vfio.iommu_drivers_lock);
list_for_each_entry(driver, &vfio.iommu_drivers_list, vfio_next) {
    if (!vfio_iommu_driver_allowed(container, driver)) continue;
    if (!try_module_get(driver->ops->owner)) continue;
    if (driver->ops->ioctl(NULL, VFIO_CHECK_EXTENSION, arg) <= 0) {
        module_put(driver->ops->owner);
        continue;
    }
    data = driver->ops->open(arg);
    ...
    ret = __vfio_container_attach_groups(container, driver, data);
    ...
    container->iommu_driver = driver;
    container->iommu_data = data;
    break;
}
```

三点：

1. **必须先有 group 才能设 IOMMU。** 源码注释解释了原因：
   container 本身是「非特权接口」，group 才是被分配给特定用户的资源。
   只有把 group 加进 container，用户才获得「启用 IOMMU」的特权，
   因为启用会消耗有限资源（DMA 映射表项、IOVA 空间）。
2. **没有 unset_iommu。** 注释明确说：通过把 container 里的 group 全部移除，
   container 会「去特权化」并回到未设置状态。这避免了「先 unset 再 set 另一个
   后端」时的状态不一致。
3. **`arg` 同时充当「扩展号」与「后端选择器」。** `VFIO_SET_IOMMU` 的参数
   与 `VFIO_CHECK_EXTENSION` 的 magic 相同（`VFIO_TYPE1_IOMMU`、
   `VFIO_TYPE1v2_IOMMU`），所以核心可以拿同一个值去问每个 driver
   「你支持这个扩展吗」，支持的那一个就是被选中的后端。
   源码注释也点明了这个「一值两用」的设计。

错误路径的 `module_put()` 必须与 `try_module_get()` 成对，否则
「尝试了但不支持的 driver」会让模块引用计数泄漏，最终无法卸载。

## 15. group 与 domain 的绑定：三级函数与 blocking domain

`iommu_attach_group()`（`drivers/iommu/iommu.c` 约 2384 行）本身很短：

```c
int iommu_attach_group(struct iommu_domain *domain, struct iommu_group *group)
{
    int ret;
    mutex_lock(&group->mutex);
    ret = __iommu_attach_group(domain, group);
    mutex_unlock(&group->mutex);
    return ret;
}
```

真正的逻辑在 `__iommu_attach_group()` → `__iommu_group_set_domain_internal()`
（约 2457 行）→ 对 group 内每个设备调 `__iommu_device_set_domain()`。
源码在 `__iommu_group_set_domain_internal()` 上方留了一段很重要的注释：

> IOMMU groups are really the natural working unit of the IOMMU, but the IOMMU
> API works on domains and devices. Bridge that gap by iterating over the
> devices in a group.

这就是为什么必须先有 group、再逐个设备 attach：硬件以 group 为单位隔离，
但内核 API 以 device 为单位描述。同一个 group 内的设备共享同一个
stream ID 集合，所以只能共享同一个 domain。

`__iommu_device_set_domain()`（约 2396 行）有两条容易踩的规则：

```c
if (dev->iommu->require_direct &&
    (new_domain->type == IOMMU_DOMAIN_BLOCKED ||
     new_domain == group->blocking_domain)) {
    dev_warn(dev, "Firmware has requested this device have a 1:1 IOMMU mapping, ...");
    return -EINVAL;
}

if (dev->iommu->attach_deferred) {
    if (new_domain == group->default_domain)
        return 0;
    dev->iommu->attach_deferred = 0;
}

ret = __iommu_attach_device(new_domain, dev, old_domain);
if (ret) {
    if ((flags & IOMMU_SET_DOMAIN_MUST_SUCCEED) &&
        group->blocking_domain && group->blocking_domain != new_domain)
        __iommu_attach_device(group->blocking_domain, dev, old_domain);
    return ret;
}
```

1. **`require_direct` 设备不能用 blocked domain。** 这类设备（固件声明需要
   1:1 映射，通常是因为设备有隐式 DMA 或 ROM 访问）如果被挂到 blocking domain，
   会立即失去必需的恒等映射，导致设备无法工作。注释还指出这会顺带排除
   `iommu_group_claim_dma_owner()`，从而阻止 VFIO/iommufd 接管。
2. **attach 失败时回退到 blocking domain，是为了避免 UAF。** 如果新 domain 挂不上，
   设备就处于「没有有效 domain」的状态，硬件可能继续用旧页表 DMA，
   而旧页表可能正在被释放。用一个「阻断所有 DMA」的 domain 兜底，
   宁可让设备不可用，也不能让它写坏内存。注释明确说这是
   `to avoid a kernel UAF`。

`iommu_detach_group()`（约 2539 行）的语义是把 group 从 domain 上摘下来，
通常等价于「挂回 blocking domain」而不是「彻底无 domain」，
原因同上。

## 16. iommu_map/unmap 的页大小协商

`iommu_map()`（约 2708 行）的三段式结构在第 3.2 节已列出，这里补两个
实现细节。

```c
int iommu_map(struct iommu_domain *domain, unsigned long iova,
              phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
    int ret;
    ret = iommu_map_nosync(domain, iova, paddr, size, prot, gfp);
    if (ret) return ret;
    ret = iommu_sync_map(domain, iova, size);
    if (ret) iommu_unmap(domain, iova, size);
    return ret;
}
```

`iommu_sync_map()` 的作用是「让映射对硬件可见」。对某些架构，写页表后需要
显式同步（例如通过命令队列），不能只靠 `wmb()`。若同步失败，
代码会调 `iommu_unmap()` 回滚——这一步不能省，否则会留下
「软件认为映射成功、硬件看不到」的页表项。

unmap 侧的页大小协商在 `__iommu_unmap_domain_pgtbl()`（约 2725 行）：

```c
min_pagesz = 1 << __ffs(domain->pgsize_bitmap);
if (!IS_ALIGNED(iova | size, min_pagesz)) {
    pr_err("unaligned: iova 0x%lx size 0x%zx min_pagesz 0x%x\n", ...);
    return 0;
}
while (unmapped < size) {
    pgsize = iommu_pgsize(domain, iova, iova, size - unmapped, &count);
    unmapped_page = ops->unmap_pages(domain, iova, pgsize, count, iotlb_gather);
    if (!unmapped_page) break;
    ...
}
```

`iova | size` 一起做对齐检查（而不是分别检查）是一个技巧：只要两者任一
不按最小页对齐，按位或的结果就会带低位，检查一次即可。
`iommu_pgsize()` 会在「剩余长度允许的最大页」与「iova 对齐允许的最大页」之间
取小，这样既能用大页加速，又不会跨越非对齐边界。

`io-pgtable-arm.c` 的 `arm_lpae_map_pages()`（约 583 行）在真正建表前做三道检查：

```c
if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize)) return -EINVAL;
if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1) iaext = ~iaext;
if (WARN_ON(iaext || paddr >> cfg->oas)) return -ERANGE;
if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE))) return -EINVAL;
```

- 第一条：页大小必须是该页表支持的粒度（4K/16K/64K 或 block）。
- 第二条：`iaext` 是「iova 超出 IAS 宽度的符号扩展」，正常必须为 0；
  TTBR1 的 quirk 会把它取反，因为 TTBR1 使用高位地址。
  `paddr >> cfg->oas` 检查物理地址是否超出 OAS。
- 第三条：**不允许「既不读也不写」的映射**。这条看似多余，实则防的是
  「用 `IOMMU_NOEXEC` 之类只带属性的 prot 建出一个谁也不能访问的页」，
  这种页表项在硬件上行为未定义。

映射完成后 `wmb()` 再返回，注释说明理由：
`Synchronise all PTE updates for the new mapping before there's a chance for
anything to kick off a table walk for the new iova`。也就是说，
必须保证「页表项已写入内存」先于「设备开始遍历页表」。

## 17. SMMUv3 的 domain 与 stage 选择

`arm_smmu_domain_alloc_paging_flags()`（本树约 3857 行）是 iommufd 风格的
「带标志分配」入口，它的第一段是白名单校验：

```c
const u32 PAGING_FLAGS = IOMMU_HWPT_ALLOC_DIRTY_TRACKING |
                         IOMMU_HWPT_ALLOC_PASID |
                         IOMMU_HWPT_ALLOC_NEST_PARENT;
if (flags & ~PAGING_FLAGS) return ERR_PTR(-EOPNOTSUPP);
if (user_data) return ERR_PTR(-EOPNOTSUPP);
```

`user_data` 被直接拒绝，说明这个版本的 SMMUv3 驱动还不支持
「由用户态提供页表配置」的 HWPT 分配（那属于 iommufd 的更高级用法）。
当前 `CONFIG_IOMMUFD` 未启用，这条路径本来也不会被走到。

随后按 `flags` 选 stage：

```c
switch (flags) {
case 0:
    if (smmu->features & ARM_SMMU_FEAT_TRANS_S1)
        smmu_domain->stage = ARM_SMMU_DOMAIN_S1;
    else
        smmu_domain->stage = ARM_SMMU_DOMAIN_S2;
    break;
case IOMMU_HWPT_ALLOC_NEST_PARENT:
    if (!(smmu->features & ARM_SMMU_FEAT_NESTING)) { ret = -EOPNOTSUPP; goto err_free; }
    smmu_domain->stage = ARM_SMMU_DOMAIN_S2;
    smmu_domain->nest_parent = true;
    break;
case IOMMU_HWPT_ALLOC_DIRTY_TRACKING:
case IOMMU_HWPT_ALLOC_DIRTY_TRACKING | IOMMU_HWPT_ALLOC_PASID:
case IOMMU_HWPT_ALLOC_PASID:
    if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S1)) { ret = -EOPNOTSUPP; goto err_free; }
    smmu_domain->stage = ARM_SMMU_DOMAIN_S1;
    break;
default:
    ret = -EOPNOTSUPP;
    goto err_free;
}
```

这段代码把三种需求映射到两种 stage：

| 需求 | stage | 原因 |
| --- | --- | --- |
| 普通 DMA 映射 | S1（优先），否则 S2 | S1 是设备的「主翻译」，S2 只在「guest 页表 + 宿主页表」时需要 |
| 嵌套父域（`NEST_PARENT`） | S2 | 父域提供「IPA → PA」，子域由 guest 提供 |
| dirty tracking / PASID | S1 | 这两个特性只在 S1 上下文中定义 |

这里的 stage 是 **SMMU 的 stage**，与 KVM 的 stage-2 不是同一层概念：
KVM 的 stage-2 是 CPU 侧的「GPA → HPA」，SMMU 的 S2 是「设备侧 IPA → PA」。
在「VFIO 直通 + guest 内虚拟化」的组合里，两者会同时存在：
CPU 用 KVM stage-2 翻译普通内存访问，设备用 SMMU S2 翻译 DMA，
而 SMMU S1 则留给 guest 的 vIOMMU（如果 guest 自己开了 IOMMU）。

domain 的最后初始化是：

```c
smmu_domain->domain.type = IOMMU_DOMAIN_UNMANAGED;
smmu_domain->domain.ops = arm_smmu_ops.default_domain_ops;
ret = arm_smmu_domain_finalise(smmu_domain, smmu, flags);
```

`IOMMU_DOMAIN_UNMANAGED` 是关键：VFIO 要求「映射完全由用户态决定」，
所以不能用 `IOMMU_DOMAIN_DMA`（那样内核会自己分配 IOVA）。
`arm_smmu_domain_finalise()` 负责建 CD（Context Descriptor）与页表，
并根据 stage 决定 CD 里填 S1 还是 S2 的配置。

映射函数只是薄薄一层转发：

```c
static int arm_smmu_map_pages(struct iommu_domain *domain, unsigned long iova,
                              phys_addr_t paddr, size_t pgsize, size_t pgcount,
                              int prot, gfp_t gfp, size_t *mapped)
{
    struct io_pgtable_ops *ops = to_smmu_domain(domain)->pgtbl_ops;
    if (!ops) return -ENODEV;
    return ops->map_pages(ops, iova, paddr, pgsize, pgcount, prot, gfp, mapped);
}
```

也就是说「SMMU 驱动」与「页表格式」是解耦的：SMMUv3 只负责
STE/CD/命令队列/事件队列，真正的页表操作交给 `io_pgtable_ops`
（ARMv8 LPAE 格式）。这也是为什么 `io-pgtable-arm.c` 被单独
编译成一个模块化组件。

`arm_smmu_attach_dev()`（约 3546 行）才是真正把设备接上去的地方：
它根据 domain 的 stage 与设备的 stream ID 填写 STE，把 stream ID 指向
对应的 CD。除此之外还有两个专用变体：

- `arm_smmu_attach_dev_identity()`（约 3810 行）：把设备挂到
  「恒等映射」domain，用于 `iommu=pt` 或「关闭 IOMMU」的语义。
- `arm_smmu_attach_dev_blocked()`（约 3833 行）：把设备挂到
  「阻断所有 DMA」domain。

注意这些函数在 `CONFIG_IOMMU_SUPPORT=n` 时**完全不编译**，
所以下面第 20 节会说明：当前配置下这些路径都不可达。

## 18. VFIO type1 的数据结构全貌

第 8 节给出的三个结构是最小集。本树的 `vfio_iommu_type1.c` 实际上有
七类结构，各自负责一件事：

| 结构 | 作用 | 本树新增/关键字段 |
| --- | --- | --- |
| `struct vfio_iommu` | 一个 container 的 type1 状态 | `num_non_pinned_groups`、`emulated_iommu_groups` |
| `struct vfio_domain` | 一个 `iommu_domain` 及其 group 列表 | `next`、`enforce_cache_coherency:1` |
| `struct vfio_dma` | 一段 IOVA → 用户 VA 的映射 | `lock_cap`、`vaddr_invalid`、`has_rsvd`、`bitmap`、`locked_vm` |
| `struct vfio_batch` | 批量 pin 的临时数组 | `pages`、`fallback_page`、`capacity`/`size`/`offset` |
| `struct vfio_iommu_group` | group 在 type1 里的包装 | `pinned_page_dirty_scope` |
| `struct vfio_iova` | 失效 IOVA 区间 | `start`/`end` |
| `struct vfio_pfn` | 已 pin 的宿主页 | `pfn`、`ref_count` |

几个字段的含义值得单独说明：

- `lock_cap`：记录「创建映射时进程是否有 `CAP_IPC_LOCK`」。有特权的进程
  不受 `RLIMIT_MEMLOCK` 限制，unpin 时的记账要走不同路径。`locked_vm`
  则是这段映射计入 `mm->locked_vm` 的页数，用于精确回退。
- `vaddr_invalid`：MMU notifier 把某段 vaddr 标为失效后置位。
  此时不能再对这段做 `pin_user_pages`，但 IOVA 映射可能还没解除，
  所以需要一个「待处理」状态。
- `has_rsvd`：这段映射里是否存在「reserved pfn」（例如设备 MMIO 被
  错误地当作 RAM pin 住）。有 reserved pfn 时不能走大页合并，
  否则会把 MMIO 空间也映射进去。
- `bitmap`：dirty tracking 的位图，按 `pgsize` 粒度记录脏页。
  它与 `vfio_dma` 一对一，而不是全局一张图，便于按映射范围取用。
- `vfio_batch.fallback_page`：当批量数组分配失败时，退化为「一次 pin 一页」，
  用一个预分配的页兜底，避免在内存紧张时直接失败。
- `pinned_page_dirty_scope`：标记「这个 group 的 page 是否参与 dirty 统计」。
  VFIO 只对「真正会被 guest 写的 page」做 dirty tracking，
  纯读映射（如 ROM）不需要，这个标记避免了无谓的开销。

映射主流程在 `vfio_dma_do_map()`（约 1680 行），它做四件事：

1. 校验 IOVA 范围不越界、不与已有 `vfio_dma` 重叠（`dma_list` 是红黑树）。
2. 检查 `dma_avail`（模块参数 `dma_entry_limit`，默认 65535）：
   每个 container 的映射条目数有上限，防止用户态用海量小映射耗尽内核内存。
3. 逐批 `pin_user_pages_remote()`，把结果填进 `vfio_batch`，
   再按 `pgsize_bitmap` 尝试合并成大页调用 `iommu_map()`。
4. 成功后插入 `vfio_dma`，并把 `locked_vm` 记账。

`vfio_iommu_type1_pin_pages()`（约 893 行）是给「按 IOVA 取 page」的内部
接口用的（例如 mdev 或嵌套场景）。它与 MAP_DMA 的区别是：不建立新映射，
只查已有 `vfio_dma` 并 pin 对应页，返回 `struct page *` 数组。

## 19. vfio-pci 的 region / irq / mmap 细节

`vfio_pci_ioctl_get_region_info()`（`drivers/vfio/pci/vfio_pci_core.c`
约 1127 行）按 `info->index` 分支：

```c
switch (info->index) {
case VFIO_PCI_CONFIG_REGION_INDEX:
    info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
    info->size = pdev->cfg_size;
    info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
    break;
case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
    info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
    info->size = pci_resource_len(pdev, info->index);
    if (!info->size) { info->flags = 0; break; }
    info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
    if (vdev->bar_mmap_supported[info->index]) {
        info->flags |= VFIO_REGION_INFO_FLAG_MMAP;
        if (info->index == vdev->msix_bar) msix_mmappable_cap(vdev, caps);
    }
    break;
case VFIO_PCI_ROM_REGION_INDEX: { ... pci_map_rom() ... }
case VFIO_PCI_VGA_REGION_INDEX:
    if (!vdev->has_vga) return -EINVAL;
    info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
    info->size = 0xc0000;
    ...
```

四个要点：

1. **region 编号是「逻辑编号」，不是 BAR 号。** `VFIO_PCI_INDEX_TO_OFFSET()`
   把逻辑编号换算成一个「伪偏移」，用户态 mmap 时用 `vm_pgoff` 携带它。
   这个偏移的高位编码 region 编号，低位是 region 内的页偏移
   （`VFIO_PCI_OFFSET_SHIFT`）。这样「一个 fd 的 mmap 空间」就能覆盖
   所有 region，不需要为每个 BAR 单独开 fd。
2. **ROM 需要临时打开 memory decode。** 代码里 `vfio_pci_memory_lock_and_enable()`
   在 `pci_map_rom()` 前后配对使用。原因是 ROM 访问需要设备打开
   memory decode，而 VFIO 平时可能把 decode 关掉（防止设备乱 DMA）。
   读取 ROM 内容只为得到「是否有效」，所以读完立刻恢复。
3. **MSI-X table 所在 BAR 需要额外的 mmap capability。** `msix_mmappable_cap()`
   在 region info 的 caps 链里告诉用户态「这个 BAR 的哪一段是 MSI-X table」，
   用户态必须把这段排除在直通映射之外，否则会破坏中断。
4. **VGA region 固定 0xc0000 大小。** 这是 x86 的 legacy VGA 窗口；
   ARM64 上 `has_vga` 通常为 false，所以这个分支会返回 `-EINVAL`。

mmap 路径 `vfio_pci_core_mmap()`（约 1814 行）的检查链很值得学习：

```c
index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);
if (index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions) return -EINVAL;
if (vma->vm_end < vma->vm_start) return -EINVAL;
if ((vma->vm_flags & VM_SHARED) == 0) return -EINVAL;
if (index >= VFIO_PCI_NUM_REGIONS) { ... 交给 region->ops->mmap ... }
if (index >= VFIO_PCI_ROM_REGION_INDEX) return -EINVAL;   /* ROM 不可 mmap */
if (!vdev->bar_mmap_supported[index]) return -EINVAL;

phys_len = PAGE_ALIGN(pci_resource_len(pdev, index));
req_len  = vma->vm_end - vma->vm_start;
pgoff    = vma->vm_pgoff & ((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);
req_start = pgoff << PAGE_SHIFT;
if (req_start + req_len > phys_len) return -EINVAL;

bar_io = vfio_pci_core_get_iomap(vdev, index);
if (IS_ERR(bar_io)) return PTR_ERR(bar_io);

vma->vm_private_data = vdev;
vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);
vm_flags_set(vma, VM_ALLOW_ANY_UNCACHED | VM_IO | VM_PFNMAP |
                  VM_DONTEXPAND | VM_DONTDUMP);
vma->vm_ops = &vfio_pci_mmap_ops;
```

上面每一行的作用：

- `VM_SHARED` 必须置位：BAR 是共享资源，私有映射会导致 COW 语义错误。
- 越界检查用 `req_start + req_len > phys_len`：防止用户态用超大的
  `vm_end - vm_start` 或超大的 `vm_pgoff` 访问到 BAR 之外。
- `pgprot_noncached()`：MMIO 不能被 cache，否则会读到过期值。
- `pgprot_decrypted()`：在 AMD SME/SEV 等内存加密平台上，MMIO 必须是
  未加密的，否则访问会失败。
- `VM_IO | VM_PFNMAP`：标记这是「直接映射物理页」的区域，内核不要
  尝试用 `struct page` 管理它；`VM_DONTEXPAND`/`VM_DONTDUMP` 则禁止
  mremap 扩张与 core dump。
- **`VM_ALLOW_ANY_UNCACHED` 是 ARM64 特有的。** 源码注释解释得最清楚：
  这个标志允许 KVM 的 stage-2 把设备映射配置成 **Normal-NC** 而不是
  `DEVICE_nGnRE`，从而让 guest 能使用 write-combining。但 ARM 并不保证
  Normal-NC 对所有 MMIO 都安全——注释点名「GICv2 的 VCPU 接口」
  在 Normal-NC 下可能触发 uncontained fault。因此这是「平台必须担保
  安全」的前提下才可用的优化，VFIO 默认设置它，把风险判断留给平台。

`vfio_pci_core_ioctl()`（约 1555 行）本身是一个大 switch，把
`VFIO_DEVICE_GET_INFO`/`GET_REGION_INFO`/`GET_IRQ_INFO`/`SET_IRQS`/
`RESET`/`FEATURE`/`IOEVENTFD` 分派到各自实现。它不做业务逻辑，
只做「copy_from_user 参数 → 调实现 → copy_to_user 结果」，
并把 `vfio_info_cap` 链（capability 链）追加到用户态缓冲区之后。

## 20. IOMMU_SUPPORT 未启用时的可运行性分析

第 10 节给出了能力表。这里把「失败点」逐一定位，说明为什么不是
「编译了就能用」。

| 环节 | 依赖 | 当前状态 | 结果 |
| --- | --- | --- | --- |
| `iommu_device_register()` | IOMMU 硬件驱动 probe | 无驱动编译 | 从未被调用 |
| `iommu_probe_device(dev)` | 总线上存在 `iommu_ops` | `iommu_device_register()` 未运行 | 设备拿不到 `dev->iommu` 与 group |
| `/sys/kernel/iommu_groups/*` | `iommu_probe_device()` | 无 group 创建 | 目录为空，`/dev/vfio/<gid>` 不存在 |
| `vfio_iommu_type1_attach_group()` | `iommu_group` | 无 group | 返回 `-EINVAL`/`-ENODEV` |
| `VFIO_IOMMU_MAP_DMA` | `iommu_map()` 可用的 domain | 无 domain | 无法建立映射 |
| `VFIO_NOIOMMU` 兜底 | `CONFIG_VFIO_NOIOMMU` | 未启用 | 无「无 IOMMU 直通」路径 |
| vfio-pci 设备节点 | 仅需 VFIO 核心 | 已编译 | 节点可创建，但无法完成 DMA 映射 |

需要强调的是：**`vfio-pci` 能 probe 成功，不代表能直通**。`vfio_pci_probe()`
只要求 PCI 设备存在且没被其它驱动占用；它不检查 IOMMU。真正的隔离
发生在 `VFIO_GROUP_SET_CONTAINER` → `attach_group` 这一步，而那一步
必须有 group。

把链条画出来：

```
CONFIG_IOMMU_SUPPORT=n
  ├─ arm-smmu-v3.c 不编译        → 无 iommu_device_register()
  │    └─ iommu_probe_device() 找不到 ops → 不建 iommu_group
  │         └─ /dev/vfio/ 下没有 group 节点
  │              └─ VFIO_GROUP_GET_DEVICE_FD 无 group 可开
  └─ io-pgtable-arm.c 不编译      → 即使有 domain 也没有页表实现
```

若要让 VFIO 直通可用，至少需要：`CONFIG_IOMMU_SUPPORT=y`、
`CONFIG_ARM_SMMU_V3=y`（或对应平台的 SMMU 驱动）、
`CONFIG_IOMMU_IO_PGTABLE_LPAE`（LPAE 页表）、以及设备树/ACPI 里的
`iommus = <&smmu ...>` 描述。仅打开 `CONFIG_VFIO=y` 是不够的。

另外，本树 `CONFIG_ACPI` 未启用，所以「通过 ACPI IORT 表描述 SMMU」
这条路也不通；只能依赖 DT 的 `iommus` 属性。这也是 ARM64 服务器
与嵌入式平台在 VFIO 配置上的一个实际差异。

## 21. 调试与排错

| 现象 | 可能原因 | 检查点 |
| --- | --- | --- |
| `VFIO_SET_IOMMU` 返回 `-EINVAL` | container 里没有 group，或已设过 IOMMU | 先 `VFIO_GROUP_SET_CONTAINER`，且一个 container 只设一次 |
| `VFIO_SET_IOMMU` 返回 `-ENODEV` | 没有匹配的 iommu driver | 确认 `VFIO_TYPE1v2_IOMMU` 后端已注册 |
| `VFIO_GROUP_GET_DEVICE_FD` 返回 `-ENODEV` | 该 group 下没有此设备 | 检查 BDF 字符串与 group 内容 |
| `VFIO_IOMMU_MAP_DMA` 返回 `-EINVAL` | IOVA 未按 `pgsize_bitmap` 对齐，或与已有映射重叠 | 先 `VFIO_IOMMU_GET_INFO` 取 `iova_pgsizes` |
| `MAP_DMA` 返回 `-ENOMEM` | pin 失败或 `dma_entry_limit` 耗尽 | 检查 `RLIMIT_MEMLOCK`/`locked_vm`，或增大 `dma_entry_limit` |
| mmap BAR 返回 `-EINVAL` | 未用 `MAP_SHARED`、越界、或该 BAR 不许 mmap | 对照 `VFIO_REGION_INFO_FLAG_MMAP` |
| guest 收到 spurious 中断 | MSI-X table 被直通映射进 guest | 确认用户态排除了 MSI-X table 区间 |
| `dmesg` 出现 `Firmware has requested this device have a 1:1 IOMMU mapping` | 设备带 `IOMMU_RESV_DIRECT` | 不能挂 blocked domain；需保留恒等映射 |

两个实用的观测入口：

- `/sys/kernel/iommu_groups/`：正常情况下每个 group 一个目录，里面有
  `devices/`（该 group 的设备）与 `type`。当前配置下这个目录为空，
  是判断「IOMMU 完全没工作」最快的方法。
- `/sys/kernel/debug/iommu/`（若 `CONFIG_IOMMU_DEBUGFS` 开启）：
  可以 dump 每个 domain 的页表。本树该选项未启用，因此只能靠
  `iommu_iova_to_phys()` 或厂商驱动的 debugfs。

关于 `dma_entry_limit`：本文件顶部的
`MODULE_PARM_DESC(dma_entry_limit, "Maximum number of user DMA mappings per container (65535)")`
说明默认上限是 65535 条映射。对于「巨页 + 大量小映射」的 VMM，
这个上限可能不够，需要显式调大；但调大意味着更多内核内存被
`vfio_dma`/`vfio_pfn` 占用，属于用空间换灵活性的权衡。

## 22. 补充文件清单（本篇新增章节涉及）

| 路径 | 作用 |
| --- | --- |
| `drivers/vfio/vfio_main.c` | `__vfio_register_dev()`、`vfio_register_group_dev()`、`vfio_unregister_group_dev()`、`vfio_device_release()`、`vfio_device_fops_mmap()` |
| `drivers/vfio/container.c` | `vfio_fops_open()`、`vfio_fops_unl_ioctl()`、`vfio_ioctl_set_iommu()`、`__vfio_container_attach_groups()`、`vfio_container_attach_group()` |
| `drivers/vfio/vfio_iommu_type1.c` | `struct vfio_iommu`/`vfio_domain`/`vfio_dma`/`vfio_batch`/`vfio_pfn`、`vfio_dma_do_map()`、`vfio_iommu_type1_map_dma()`/`unmap_dma()`、`vfio_iommu_type1_attach_group()`、`vfio_iommu_type1_pin_pages()` |
| `drivers/vfio/pci/vfio_pci_core.c` | `vfio_pci_ioctl_get_region_info()`、`vfio_pci_core_ioctl()`、`vfio_pci_core_mmap()`、`vfio_pci_core_register_device()` |
| `drivers/iommu/iommu.c` | `iommu_attach_group()`、`iommu_detach_group()`、`__iommu_attach_group()`、`__iommu_group_set_domain_internal()`、`__iommu_device_set_domain()`、`iommu_map()`、`__iommu_unmap_domain_pgtbl()` |
| `drivers/iommu/io-pgtable-arm.c` | `struct arm_lpae_io_pgtable`、`arm_lpae_map_pages()`、`arm_lpae_unmap_pages()`、`arm_lpae_iova_to_phys()` |
| `drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c` | `arm_smmu_domain_alloc_paging_flags()`、`arm_smmu_map_pages()`、`arm_smmu_unmap_pages()`、`arm_smmu_attach_dev()`、`arm_smmu_attach_dev_identity/blocked()`、`arm_smmu_device_probe()` |
| `drivers/iommu/iova.c` | IOVA 分配器（`find_iova()` 等），供 DMA-IOMMU 路径使用 |
