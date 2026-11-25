# virtio / vhost 虚拟化 I/O

## 1. 概述

virtio 是半虚拟化 I/O 的标准化设备模型：guest 内的前端驱动（virtio-net、virtio-blk 等）
通过共享内存中的 virtqueue 与后端交换请求，后端可以是 QEMU 用户态、内核 vhost、vDPA
硬件或 virtio-mmio 模拟设备。内核侧相关的三块代码：

| 目录 | 角色 |
| --- | --- |
| `drivers/virtio/` | 前端：核心总线/设备模型、ring 实现、PCI/MMIO 传输层 |
| `drivers/vhost/` | 后端加速：在宿主内核里直接处理 ring，避免 QEMU 陷入 |
| `drivers/vdpa/` | vDPA 框架：把具备 virtio 数据面能力的硬件抽象成 vDPA 设备 |
| `include/uapi/linux/virtio*.h` | 前端 UAPI：`virtio_ring.h`、`virtio_config.h`、`virtio_pci.h`、`virtio_mmio.h` 等 |
| `include/uapi/linux/vhost.h` | vhost 后端 ioctl ABI |

当前 `.config` 的相关状态（务必区分“源码存在”与“已编译”）：

```
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_PCI_LEGACY=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_NET=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO_BALLOON=y
CONFIG_TUN=y
CONFIG_VHOST_MENU=y
CONFIG_VHOST_ENABLE_FORK_OWNER_CONTROL=y

# CONFIG_VHOST_NET is not set
# CONFIG_VHOST_CROSS_ENDIAN_LEGACY is not set
# CONFIG_VDPA is not set
# CONFIG_VIRTIO_FS / CONFIG_VIRTIO_INPUT / CONFIG_VIRTIO_RTC is not set
```

注意：`CONFIG_VHOST` 本身是隐藏符号，由 `VHOST_NET`/`VHOST_SCSI`/`VHOST_VSOCK`
等 `select`。当前没有任何后端被启用，因此 `drivers/vhost/` 虽有源码但不会被编入内核；
`CONFIG_VDPA` 未启用，`drivers/vdpa/` 与 `virtio_vdpa.c` 同样不参与构建。
`virtio_ring.c`、`virtio_pci_*`、`virtio_mmio.c`、`virtio_net.c` 等前端仍然编译。

## 2. 分层模型

```
   guest 用户进程
        │ 系统调用
   guest 内核：virtio_net / virtio_blk 驱动   (drivers/net/virtio_net.c, drivers/block/virtio_blk.c)
        │ virtqueue_add_* / virtqueue_kick
   virtio 核心：virtqueue 抽象                 (drivers/virtio/virtio_ring.c)
        │ vq->notify()
   传输层：virtio-pci / virtio-mmio / virtio-vdpa
        │ MMIO / PIO / doorbell
   ─────────────── 虚拟机边界 ───────────────
   后端：QEMU vhost-user / 内核 vhost-net / vDPA 硬件
```

前端只认识 `struct virtio_device` 与 `struct virtqueue`，传输差异封装在
`struct virtio_config_ops` 中。

## 3. 核心数据结构

### 3.1 struct virtio_device / virtio_driver

`include/linux/virtio.h`：

```c
struct virtio_device {
    int index;
    bool failed;
    bool config_core_enabled;
    spinlock_t config_lock;
    struct device dev;
    struct virtio_device_id id;
    const struct virtio_config_ops *config;
    const struct vringh_config_ops *vringh_config;
    const struct virtio_map_ops *map;
    struct list_head vqs;
    VIRTIO_DECLARE_FEATURES(features);   /* 已协商特性位图 */
    void *priv;
    union virtio_map vmap;
};
```

`struct virtio_driver` 描述前端驱动：`id_table` 匹配设备 ID，`feature_table` 声明
驱动支持的特性，另有 `probe/remove/config_changed/freeze/restore/reset_prepare/reset_done`。

`struct virtio_config_ops`（`include/linux/virtio_config.h`）是传输层回调集合，关键项：

```c
struct virtio_config_ops {
    void (*get)(struct virtio_device *, unsigned offset, void *buf, unsigned len);
    void (*set)(struct virtio_device *, unsigned offset, const void *buf, unsigned len);
    u8   (*get_status)(struct virtio_device *);
    void (*set_status)(struct virtio_device *, u8 status);
    void (*reset)(struct virtio_device *);
    int  (*find_vqs)(struct virtio_device *, unsigned int nvqs,
                     struct virtqueue *vqs[], struct virtqueue_info vqs_info[],
                     struct irq_affinity *desc);
    void (*del_vqs)(struct virtio_device *);
    u64  (*get_features)(struct virtio_device *);
    int  (*finalize_features)(struct virtio_device *);
};
```

### 3.2 struct virtqueue

```c
struct virtqueue {
    struct list_head list;
    void (*callback)(struct virtqueue *vq);
    const char *name;
    struct virtio_device *vdev;
    unsigned int index;
    unsigned int num_free;   /* 剩余可用描述符数 */
    unsigned int num_max;    /* 队列深度 */
    bool reset;
    void *priv;              /* 指向 vring_virtqueue */
};
```

### 3.3 struct vring_virtqueue

`drivers/virtio/virtio_ring.c` 中 split 与 packed 两种布局共用一个外壳：

```c
struct vring_virtqueue {
    struct virtqueue vq;
    bool use_map_api;       /* 是否走 DMA API */
    bool weak_barriers;
    bool broken;
    bool indirect;          /* 支持 indirect descriptor */
    bool event;             /* 支持 avail/used event idx */
    enum vq_layout layout;  /* SPLIT / PACKED */
    unsigned int free_head;
    struct used_entry batch_last;
    unsigned int num_added;
    u16 last_used_idx;
    u16 last_used;
    bool event_triggered;
    union {
        struct vring_virtqueue_split split;
        struct vring_virtqueue_packed packed;
    };
    bool (*notify)(struct virtqueue *vq);
    bool we_own_ring;
    union virtio_map map;
};
```

split 分支内含 `struct vring vring`（desc/avail/used 指针）、`vring_desc_state_split[]`；
packed 分支内含 `struct vring_packed`、`vring_desc_state_packed[]`。

## 4. vring 布局与描述符链

`include/uapi/linux/virtio_ring.h` 定义 split ring 的 ABI：

```c
struct vring_desc {          /* 16 字节 */
    __virtio64 addr;         /* guest 物理地址 */
    __virtio32 len;
    __virtio16 flags;        /* VRING_DESC_F_NEXT/WRITE/INDIRECT */
    __virtio16 next;
};

struct vring_avail {         /* 驱动 → 设备 */
    __virtio16 flags;
    __virtio16 idx;
    __virtio16 ring[];
};

struct vring_used_elem { __virtio32 id; __virtio32 len; };

struct vring_used {          /* 设备 → 驱动 */
    __virtio16 flags;
    __virtio16 idx;
    vring_used_elem_t ring[];
};
```

内存布局为 `desc[num] | avail | padding | used | padding`，由 `vring_init()` 计算；
`vring_size()` 给出总字节数。要求 `num` 为 2 的幂，desc 表 16 字节对齐，
avail 2 字节对齐，used 4 字节对齐。

关键语义：

- 驱动把缓冲区挂到 `desc`，用 `flags` 的 `VRING_DESC_F_NEXT` 串成链；
- `avail.idx` 递增表示“有新链可用”，设备读 `avail.ring[]` 取链头；
- 设备处理完把链头写入 `used.ring[]` 并递增 `used.idx`；
- `VRING_AVAIL_F_NO_INTERRUPT` / `VRING_USED_F_NO_NOTIFY` 用于中断抑制；
- `VIRTIO_F_EVENT_IDX` 启用后改用 avail/used event 计数值精细控制通知；
- packed ring（`VIRTIO_F_RING_PACKED`）把 desc/avail/used 合并为一个环，
  用 `wrap counter` 与 `VRING_PACKED_DESC_F_AVAIL/USED` 标志区分所有权。

## 5. virtqueue 操作与 kick/notify

典型发送路径（以 virtio-net TX 为例）：

```
virtqueue_add_outbuf(vq, sg, num, data, GFP_ATOMIC)
  └─ virtqueue_add_sgs(vq, sgs, out_sgs=1, in_sgs=0, data, gfp)
      └─ virtqueue_add(vq, ...)
          ├─ split:  virtqueue_add_split()
          │    ├─ alloc_indirect()（若协商 VIRTIO_F_INDIRECT_DESC）
          │    ├─ 逐个填充 desc，设置 VRING_DESC_F_NEXT
          │    ├─ vring_avail->ring[avail_idx % num] = head
          │    ├─ virtio_store_mb(weak_barriers, &vring_avail->idx, ++avail_idx)
          │    └─ 返回 vq->num_free 是否为 0
          └─ packed: virtqueue_add_packed()
               ├─ 逐项填充 packed desc，设置 AVAIL/USED 位
               └─ 更新 avail wrap counter
virtqueue_kick(vq)
  ├─ virtqueue_kick_prepare(vq)    /* 判断对端是否需要通知 */
  └─ virtqueue_notify(vq) → vq->notify(vq)
```

弱内存序优化：`vring_need_event()` 判断是否真的需要写 doorbell；
若对端已声明 `VRING_AVAIL_F_NO_INTERRUPT`，则跳过。

接收路径由设备写 used ring 并触发中断，驱动在
`virtqueue_get_buf(vq, &len)` 中读取 `used.idx`、取出 `used.ring[]` 的 id/len，
回收描述符并返回原始 `data` 指针。

`virtqueue_disable_cb()` / `virtqueue_enable_cb()` / `virtqueue_enable_cb_delayed()`
用于 NAPI 式中断合并。`virtqueue_resize()` / `virtqueue_reset()` 支持
`VIRTIO_F_RING_RESET`。

## 6. 特性协商

`virtio_dev_probe()`（`drivers/virtio/virtio.c`）流程：

1. `virtio_add_status(dev, VIRTIO_CONFIG_S_DRIVER)`；
2. `virtio_get_features(dev, device_features)` 取设备特性；
3. 按 `drv->feature_table[]` 构造驱动特性位图；若驱动提供
   `feature_table_legacy`，在 legacy 设备上另用一套；
4. 若设备支持 `VIRTIO_F_VERSION_1`，逐 64 位取交集；否则只取低 64 位 legacy 交集；
5. 保留 `VIRTIO_TRANSPORT_F_START`(28) 到 `VIRTIO_TRANSPORT_F_END`(42) 区间的传输特性；
6. `dev->config->finalize_features(dev)` 写回设备；
7. 可选 `drv->validate()`，若其修改了特性集则再次 finalize；
8. `virtio_features_ok()` 校验：若走内存访问回调（IOMMU）则必须同时具备
   `VIRTIO_F_VERSION_1` 与 `VIRTIO_F_ACCESS_PLATFORM`；随后置
   `VIRTIO_CONFIG_S_FEATURES_OK` 并回读确认；
9. 置 `VIRTIO_CONFIG_S_DRIVER_OK`，调用 `drv->probe()`。

状态位定义（`include/uapi/linux/virtio_config.h`）：
`ACKNOWLEDGE`(1)、`DRIVER`(2)、`DRIVER_OK`(4)、`FEATURES_OK`(8)、
`NEEDS_RESET`(0x40)、`FAILED`(0x80)。

常见特性位：`VIRTIO_F_NOTIFY_ON_EMPTY`(24)、`VIRTIO_F_ANY_LAYOUT`(27)、
`VIRTIO_F_VERSION_1`(32)、`VIRTIO_F_ACCESS_PLATFORM`(33)、
`VIRTIO_F_RING_PACKED`(34)、`VIRTIO_F_IN_ORDER`(35)、
`VIRTIO_F_ORDER_PLATFORM`(36)、`VIRTIO_F_SR_IOV`(37)、
`VIRTIO_F_NOTIFICATION_DATA`(38)、`VIRTIO_F_RING_RESET`(40)、`VIRTIO_F_ADMIN_VQ`(41)。

## 7. 传输层：virtio-pci 与 virtio-mmio

### 7.1 virtio-pci modern

`drivers/virtio/virtio_pci_modern_dev.c` 负责 capability 解析：

- `vp_modern_probe()` 遍历 PCI vendor-specific capability，找到
  `common`、`notify`、`isr`、`device` 四类配置区，分别用
  `vp_modern_map_capability()` 映射（支持 MMIO 与 doorbell 两种 notify）；
- `vp_modern_get_status()` / `vp_modern_set_status()` 读写 `common` 区的
  `device_status`；
- `vp_modern_get_extended_features()` / `vp_modern_set_extended_features()`
  支持 64 位以上特性；
- `vp_modern_queue_address()` 写 `queue_desc/queue_driver/queue_device`；
- `vp_modern_set_queue_enable()` / `vp_modern_get_queue_enable()` 控制队列使能；
- `vp_modern_get_queue_reset()` / `vp_modern_set_queue_reset()` 支持 ring reset。

`drivers/virtio/virtio_pci_modern.c` 把这些封装成 `virtio_config_ops`，
`vp_notify()`（`VIRTIO_F_NOTIFICATION_DATA` 时用 `vp_notify_with_data()`）
向 notify 区写入队列索引触发 doorbell。

legacy 设备由 `virtio_pci_legacy_dev.c` 处理：通过 BAR0 的 I/O 端口偏移访问
`VIRTIO_PCI_HOST_FEATURES`/`GUEST_FEATURES`/`QUEUE_PFN` 等寄存器
（`vp_legacy_get_features()`、`vp_legacy_set_features()`）。

### 7.2 virtio-mmio

`drivers/virtio/virtio_mmio.c` 面向嵌入式与 microVM。`vm_probe()` 校验
`VIRTIO_MMIO_MAGIC_VALUE`（0x74726976，"virt"）与 version；寄存器定义在
`include/uapi/linux/virtio_mmio.h`（`VIRTIO_MMIO_QUEUE_SEL`、`QUEUE_NUM_MAX`、
`QUEUE_DESC_LOW/HIGH`、`QUEUE_READY`、`QUEUE_NOTIFY`、`INTERRUPT_STATUS`、
`STATUS`、`CONFIG_GENERATION` 等）。`vm_find_vqs()` 为每个队列申请
platform irq 并注册 `virtqueue_info`。

## 8. vhost 与 vhost-net

### 8.1 vhost 核心

vhost 让宿主内核直接读写 guest 的 virtqueue，QEMU 只负责配置。用户态通过
`/dev/vhost-net`（或 `/dev/vhost-vsock`、`/dev/vhost-scsi`）下发 ioctl：

| ioctl | 作用 |
| --- | --- |
| `VHOST_GET_FEATURES` / `VHOST_SET_FEATURES` | 协商后端特性 |
| `VHOST_SET_OWNER` / `VHOST_RESET_OWNER` | 绑定当前进程为 owner |
| `VHOST_SET_MEM_TABLE` | 下发 guest 物理内存映射（`struct vhost_memory`） |
| `VHOST_SET_LOG_BASE` / `VHOST_SET_LOG_FD` | 脏页日志 |
| `VHOST_SET_VRING_NUM` | 队列深度 |
| `VHOST_SET_VRING_ADDR` | desc/avail/used 的用户态地址 |
| `VHOST_SET_VRING_BASE` | 初始 avail/used 索引 |
| `VHOST_SET_VRING_KICK` | 绑定 kick eventfd（guest 通知宿主） |
| `VHOST_SET_VRING_CALL` | 绑定 call eventfd（宿主通知 guest） |
| `VHOST_SET_VRING_ERR` | 错误事件 fd |

实现分两层：`vhost_dev_ioctl()` 处理设备级命令，`vhost_vring_ioctl()`
处理队列级命令（`drivers/vhost/vhost.c`）。

```c
struct vhost_dev {
    struct mm_struct *mm;
    struct mutex mutex;
    struct vhost_virtqueue **vqs;
    int nvqs;
    struct vhost_iotlb *umem;
    struct vhost_iotlb *iotlb;
    spinlock_t iotlb_lock;
    struct list_head read_list;
    struct list_head pending_list;
    int iov_limit;
    int weight;
    int byte_weight;
    struct xarray worker_xa;
    bool use_worker;
    bool fork_owner;
};

struct vhost_virtqueue {
    struct vhost_dev *dev;
    struct mutex mutex;
    unsigned int num;
    vring_desc_t __user *desc;
    vring_avail_t __user *avail;
    vring_used_t __user *used;
    struct file *kick;
    struct vhost_vring_call call_ctx;
    struct eventfd_ctx *error_ctx;
    struct eventfd_ctx *log_ctx;
    struct vhost_poll poll;
    vhost_work_fn_t handle_kick;
    u16 last_avail_idx, avail_idx, last_used_idx;
    struct iovec iov[UIO_MAXIOV];
    struct iovec *indirect;
    struct vring_used_elem *heads;
    struct vhost_iotlb *umem;
    struct vhost_iotlb *iotlb;
    VIRTIO_DECLARE_FEATURES(acked_features);
    u64 acked_backend_features;
    bool is_le;
};
```

数据面原语：

- `vhost_get_vq_desc()`：读 `avail.ring[last_avail_idx % num]`，沿 `desc.next`
  展开成 `iov[]`，把 GPA 经 `umem`/`iotlb` 翻译成 HVA；
- `vhost_add_used()` / `vhost_add_used_and_signal()`：写 `used.ring[]` 与 `used.idx`，
  按需触发 call eventfd；
- `vhost_signal()` / `vhost_notify()`：通知 guest；
- `vhost_poll`：把 kick eventfd 挂到 epoll，回调 `handle_kick`；
- `vhost_exceeds_weight()`：按包数与字节数限制单次处理量，避免软锁死；
- `vhost_iotlb` 与 `msg_handler` 支持 `VIRTIO_F_ACCESS_PLATFORM` 下的 IOTLB 消息。

### 8.2 vhost-net

`drivers/vhost/net.c` 提供两个 virtqueue：`VHOST_NET_VQ_RX`(0) 与
`VHOST_NET_VQ_TX`(1)。`vhost_net_open()` 分配 `struct vhost_net`、批量缓冲区、
XDP 数组，注册 `handle_tx_kick`/`handle_rx_kick`，并
`vhost_poll_init()` 绑定 TX 的 `EPOLLOUT` 与 RX 的 `EPOLLIN`。

`vhost_net_set_features()` 根据协商结果确定 `vhost_hlen`（virtio-net header 长度）：

- `VIRTIO_NET_F_MRG_RXBUF` 或 `VIRTIO_F_VERSION_1` → 12 字节 `virtio_net_hdr_mrg_rxbuf`；
- 协商 `VHOST_NET_F_VIRTIO_NET_HDR` 时由后端自己提供/消费头部；
- `VHOST_F_LOG_ALL` 时记录 used ring 写日志；
- `VIRTIO_F_ACCESS_PLATFORM` 时启用 IOTLB。

`handle_tx()` 从 TX virtqueue 取描述符，拼接 virtio-net header 后
`vhost_net_build_xdp()` / `sock->ops->sendmsg()` 发到 TAP 后端；
`handle_rx()` 用 `get_rx_bufs()` 预取 guest 缓冲区，从 TAP 读取后写 used ring。
支持 zerocopy（`ubufs`/`ubuf_info`）、busy poll（`busyloop_timeout`）与 XDP。

当前配置 `# CONFIG_VHOST_NET is not set`，上述代码不参与编译，
`/dev/vhost-net` 不存在；guest 网络只能走 QEMU 用户态 virtio-net 或 TAP。

## 9. vhost-user

vhost-user 是**用户态协议**（QEMU 与 DPDK/SPDK 等 vhost 后端之间通过 Unix socket
交换 `VHOST_USER_SET_MEM_TABLE`、`VHOST_USER_SET_VRING_ADDR`、
`VHOST_USER_SET_VRING_KICK` 等消息），内核并不实现该协议。内核侧只提供两种落点：

1. 内核 vhost：QEMU 作为前端，把 ring 与内存表通过 `/dev/vhost-net` 的 ioctl
   交给内核（本文第 8 节）；
2. 用户态 vhost 后端：完全在用户态完成 ring 处理，内核不参与。

因此本源码树中不存在 `vhost-user` 的协议解析代码，只有 `vhost.c` 提供的
“接受 ring 配置并直接处理”的通用后端框架。

## 10. vDPA 与 virtio-vdpa

vDPA（virtio Data Path Acceleration）把支持 virtio 数据面语义的硬件抽象为
`struct vdpa_device`，控制面仍走标准 virtio 配置，数据面由硬件直接 DMA。

```c
struct vdpa_device {
    struct device dev;
    struct vdpa_mgmtdev *mgmtdev;
    const struct vdpa_config_ops *config;
    unsigned int nvqs;
    ...
};

struct vdpa_config_ops {
    int  (*set_vq_address)(struct vdpa_device *, u16 idx, u64 desc, u64 driver, u64 device);
    void (*set_vq_num)(struct vdpa_device *, u16 idx, u32 num);
    void (*kick_vq)(struct vdpa_device *, u16 idx);
    void (*set_vq_cb)(struct vdpa_device *, u16 idx, struct vdpa_callback *);
    void (*set_vq_ready)(struct vdpa_device *, u16 idx, bool ready);
    u64  (*get_device_features)(struct vdpa_device *);
    int  (*set_driver_features)(struct vdpa_device *, u64 features);
    u8   (*get_status)(struct vdpa_device *);
    void (*set_status)(struct vdpa_device *, u8 status);
    void (*set_config)(struct vdpa_device *, const void *buf, unsigned int len, unsigned int off);
    void (*get_config)(struct vdpa_device *, void *buf, unsigned int len, unsigned int off);
    ...
};
```

`drivers/vdpa/vdpa.c` 用 generic netlink 提供管理接口：
`VDPA_CMD_DEV_NEW` / `VDPA_CMD_DEV_DEL` / `VDPA_CMD_DEV_GET`，
属性含 `VDPA_ATTR_DEV_NAME`、`VDPA_ATTR_MGMTDEV_DEV_NAME` 等；
`vdpa_register_device()` / `__vdpa_alloc_device()` 完成设备注册。

`drivers/virtio/virtio_vdpa.c` 在 vDPA 设备之上创建标准 `virtio_device`，
使现有 virtio 驱动可直接使用：`virtio_vdpa_find_vqs()` 为每个队列注册
`set_vq_cb` 与 kick，`virtio_vdpa_set_status()` 转发到 `vdpa_config_ops`。

`drivers/vhost/vdpa.c` 则是 vhost-vDPA 后端：它注册一个 vDPA driver，
创建设备节点，用户态用 `VHOST_VDPA_SET_STATUS`、`VHOST_VDPA_SET_VRING_ENABLE`、
`VHOST_VDPA_GET_CONFIG`/`SET_CONFIG`、`VHOST_VDPA_SUSPEND` 等 ioctl 控制。

当前 `CONFIG_VDPA` 未启用，以上 vDPA 代码均不编译。

## 11. 端到端调用链

```
前端发送（virtio-net）:
  start_xmit → virtqueue_add_outbuf → virtqueue_add_sgs → virtqueue_add_split
    → virtqueue_kick → virtqueue_notify
    → virtio-pci: vp_notify / vp_notify_with_data 写 notify 区 → 后端

内核 vhost 后端（CONFIG_VHOST_NET=y 时）:
  kick eventfd 就绪 → vhost_poll 唤醒 → handle_tx_kick → handle_tx
    → vhost_get_vq_desc 展开 iov
    → tun 发送 / get_rx_bufs + used ring 回填
    → vhost_add_used_and_signal → call eventfd → guest 中断

设备注册（前端）:
  virtio_pci_probe → vp_modern_probe（解析 capability）
    → register_virtio_device → virtio_bus 匹配 virtio_driver
    → virtio_dev_probe → 特性协商 → drv->probe → find_vqs

vDPA（CONFIG_VDPA=y 时）:
  vdpa 硬件驱动 → _vdpa_register_device → virtio_vdpa_probe
    → register_virtio_device → 前端驱动走标准 virtio 路径，数据面由硬件完成
```

## 12. 关键文件清单

| 路径 | 作用 |
| --- | --- |
| `include/uapi/linux/virtio_ring.h` | split/packed ring ABI（`vring_desc/avail/used`） |
| `include/uapi/linux/virtio_config.h` | 特性位与 `VIRTIO_CONFIG_S_*` 状态位 |
| `include/uapi/linux/virtio_pci.h` | virtio-pci capability 与 legacy 寄存器 |
| `include/uapi/linux/virtio_mmio.h` | virtio-mmio 寄存器偏移 |
| `include/uapi/linux/virtio_ids.h` | 设备 ID 常量 |
| `include/uapi/linux/vhost.h` | vhost/vhost-vDPA ioctl ABI |
| `include/uapi/linux/vdpa.h` | vDPA netlink 命令与属性 |
| `include/linux/virtio.h` | `virtqueue`、`virtio_device`、`virtio_driver`、`virtqueue_add_*` |
| `include/linux/virtio_config.h` | `struct virtio_config_ops`、特性辅助宏 |
| `include/linux/vdpa.h` | `struct vdpa_device`、`struct vdpa_config_ops` |
| `drivers/virtio/virtio.c` | 总线、设备注册、`virtio_dev_probe`、特性协商 |
| `drivers/virtio/virtio_ring.c` | `vring_virtqueue`、split/packed ring 实现 |
| `drivers/virtio/virtio_pci_modern_dev.c` | modern capability 解析与寄存器访问 |
| `drivers/virtio/virtio_pci_modern.c` | modern 传输的 `virtio_config_ops` 与 notify |
| `drivers/virtio/virtio_pci_legacy_dev.c` | legacy I/O 端口访问 |
| `drivers/virtio/virtio_pci_common.c` | PCI probe/remove、`vp_find_vqs` 公共逻辑 |
| `drivers/virtio/virtio_mmio.c` | MMIO 传输层 |
| `drivers/virtio/virtio_vdpa.c` | vDPA 之上的 virtio 前端 |
| `drivers/vhost/vhost.c` | vhost 核心：内存表、ring 访问、ioctl |
| `drivers/vhost/vhost.h` | `struct vhost_dev`、`struct vhost_virtqueue` |
| `drivers/vhost/net.c` | vhost-net 后端（RX/TX、XDP、zerocopy） |
| `drivers/vhost/vringh.c` | 内核内 ring 访问库（vringh） |
| `drivers/vhost/iotlb.c` | vhost IOTLB 实现 |
| `drivers/vhost/vdpa.c` | vhost-vDPA 后端 |
| `drivers/vdpa/vdpa.c` | vDPA 核心与 netlink 管理接口 |
| `drivers/net/virtio_net.c` | virtio-net 前端驱动示例 |
