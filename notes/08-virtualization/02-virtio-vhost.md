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

## 13. virtqueue 的分发表：四张 ops 表与 VIRTQUEUE_CALL

第 3.3 节给出的 `struct vring_virtqueue` 只是一个「外壳」，真正决定行为的是
`vq->layout` 与一组函数指针表。本树的实现比早期版本更规整：
`drivers/virtio/virtio_ring.c` 里定义了 `struct virtqueue_ops`（约 173 行），
每一个操作都是一个回调：

```c
struct virtqueue_ops {
    int  (*add)(struct vring_virtqueue *vq, struct scatterlist *sgs[],
                unsigned int total_sg, unsigned int out_sgs,
                unsigned int in_sgs, void *data, void *ctx,
                bool premapped, gfp_t gfp, unsigned long attr);
    void *(*get)(struct vring_virtqueue *vq, unsigned int *len, void **ctx);
    bool (*kick_prepare)(struct vring_virtqueue *vq);
    void (*disable_cb)(struct vring_virtqueue *vq);
    bool (*enable_cb_delayed)(struct vring_virtqueue *vq);
    unsigned int (*enable_cb_prepare)(struct vring_virtqueue *vq);
    bool (*poll)(const struct vring_virtqueue *vq, unsigned int last_used_idx);
    void *(*detach_unused_buf)(struct vring_virtqueue *vq);
    bool (*more_used)(const struct vring_virtqueue *vq);
    int  (*resize)(struct vring_virtqueue *vq, u32 num);
    void (*reset)(struct vring_virtqueue *vq);
};
```

然后有 **四张** 实例表（约 2680–2734 行）：

| 表名 | 适用条件 | `add` | `get` |
| --- | --- | --- | --- |
| `split_ops` | split ring | `virtqueue_add_split` | `virtqueue_get_buf_ctx_split` |
| `packed_ops` | packed ring | `virtqueue_add_packed` | `virtqueue_get_buf_ctx_packed` |
| `split_in_order_ops` | split + `VIRTIO_F_IN_ORDER` | `virtqueue_add_split`（共用） | `virtqueue_get_buf_ctx_split_in_order` |
| `packed_in_order_ops` | packed + `VIRTIO_F_IN_ORDER` | `virtqueue_add_packed_in_order` | `virtqueue_get_buf_ctx_packed_in_order` |

注意 `split_in_order_ops.add` 与 `split_ops.add` 是同一个函数：在 split ring 上，
「顺序」只影响**回收**侧（驱动可以按 avail 顺序批量回收，不必记每个 buffer 的 id），
不影响添加侧。packed 则连添加都要维护 wrap counter 与 in-order 指针，
所以需要独立的 `virtqueue_add_packed_in_order`。

分发靠宏 `VIRTQUEUE_CALL`（约 2778 行）：

```c
#define VIRTQUEUE_CALL(vq, op, ...)                     \
    ({                                                  \
        typeof(vq) __VIRTQUEUE_CALL_vq = (vq);          \
        typeof(split_ops.op(__VIRTQUEUE_CALL_vq, ##__VA_ARGS__)) ret; \
        switch (__VIRTQUEUE_CALL_vq->layout) {          \
        case VQ_LAYOUT_SPLIT:                           \
            ret = split_ops.op(__VIRTQUEUE_CALL_vq, ##__VA_ARGS__);   \
        ...
```

这个宏有三个作用：一是把 `switch` 收敛到一处，避免每个导出函数都写一遍；
二是通过 `typeof(split_ops.op(...))` 让编译器检查各表回调签名一致；
三是让 `split_ops` 成为「签名基准」，新增布局时必须与它对齐。
代价是每次调用有一次 `layout` 分支——但 `layout` 在队列创建时就固定，
分支预测几乎总是命中。

`struct vring_virtqueue_split`（约 106 行）与 `struct vring_virtqueue_packed`
（约 135 行）的字段差异，正好对应两种布局的本质不同：

| 字段 | split | packed | 说明 |
| --- | --- | --- | --- |
| `vring` | `struct vring`（desc/avail/used 三个指针） | 匿名结构（`desc`/`driver`/`device`） | packed 只有 desc 数组 + 两个 event 结构 |
| `avail_flags_shadow` / `event_flags_shadow` | avail 侧 | driver event 侧 | 「影子」避免每次读设备内存 |
| `avail_idx_shadow` | 有 | 无 | packed 用 `next_avail_idx` |
| `avail_wrap_counter` | 无 | 有 | packed 的所有权判定依赖 wrap |
| `next_avail_idx` | 无 | 有 | packed 的「下一个可写槽」 |
| `desc_state` / `desc_extra` | 有 | 有 | 每描述符的软件状态与 DMA 信息 |
| DMA 地址字段 | `queue_dma_addr` / `queue_size_in_bytes` | `ring_dma_addr` + 两个 event 地址 + 两个 size | packed 是三段独立映射 |
| 创建参数 | `vring_align`、`may_reduce_num` | 无 | split 允许在内存不足时减小队列深度 |

「影子字段」是这个设计的性能关键：`avail->flags` 与 `avail->idx` 位于共享内存，
每次读都要走一次可能失效的 cache line，甚至可能被对端同时改写。
驱动用 `*_shadow` 保存自己最后写入的值，只有当**值真的变化**时才写回共享内存，
大幅减少对共享页的写流量。

## 14. split ring 的 add / kick / get 全流程

第 5 节给出了调用链骨架。这里按本树代码补上真实的分层：
所有 `virtqueue_add_*` 最终都走 `virtqueue_add()`，再由 `VIRTQUEUE_CALL` 分发。

```c
int virtqueue_add_sgs(struct virtqueue *_vq, struct scatterlist *sgs[],
                      unsigned int out_sgs, unsigned int in_sgs,
                      void *data, gfp_t gfp)
{
    unsigned int i, total_sg = 0;
    for (i = 0; i < out_sgs + in_sgs; i++) {
        struct scatterlist *sg;
        for (sg = sgs[i]; sg; sg = sg_next(sg))
            total_sg++;
    }
    return virtqueue_add(_vq, sgs, total_sg, out_sgs, in_sgs,
                         data, NULL, false, gfp, 0);
}
```

`virtqueue_add_sgs()` 先**数总段数**再调用，因为底层需要预先知道要占多少描述符。
这也是为什么它可能返回 `-ENOSPC`：调用者必须先 `virtqueue_get_buf()` 腾出空间。
源码注释特别说明 `ENOSPC` 是「可重试」的信号，而 `-ENOMEM` 才是真的分配失败。

各 `virtqueue_add_*` 的参数差异只体现在 `out_sgs`/`in_sgs` 与 `premapped`：

| 函数 | `out_sgs` | `in_sgs` | `premapped` |
| --- | --- | --- | --- |
| `virtqueue_add_outbuf` | 1 | 0 | false |
| `virtqueue_add_outbuf_premapped` | 1 | 0 | true |
| `virtqueue_add_inbuf` | 0 | 1 | false |
| `virtqueue_add_inbuf_ctx` | 0 | 1 | false（带 ctx） |
| `virtqueue_add_inbuf_premapped` | 0 | 1 | true |
| `virtqueue_add_inbuf_cache_clean` | 0 | 1 | 特殊（需 cache clean） |

`premapped` 的含义是「scatterlist 里的地址已经是 DMA 地址，不需要再走
`dma_map_sg()`」。这在 vDPA 或「上层已经做过映射」的场景下避免二次映射。
`inbuf_cache_clean` 则用于非一致性 DMA 架构：设备会写这块内存，
必须在交给设备前 clean cache，否则设备可能读到脏数据。

kick 侧被拆成两个可独立调用的函数：

```c
bool virtqueue_kick_prepare(struct virtqueue *_vq)
{
    struct vring_virtqueue *vq = to_vvq(_vq);
    return VIRTQUEUE_CALL(vq, kick_prepare);
}

bool virtqueue_notify(struct virtqueue *_vq)
{
    struct vring_virtqueue *vq = to_vvq(_vq);
    if (unlikely(vq->broken))
        return false;
    if (!vq->notify(_vq)) {
        vq->broken = true;
        return false;
    }
    return true;
}

bool virtqueue_kick(struct virtqueue *vq)
{
    if (virtqueue_kick_prepare(vq))
        return virtqueue_notify(vq);
    return true;
}
```

拆开的原因写在注释里：`kick_prepare` 需要与其它队列操作**串行化**（它要读
`avail_idx` 与对端的 event 配置），而 `virtqueue_notify()` **不需要**串行化。
把两者拆开，驱动就可以「持锁做完 prepare，放锁后再 notify」，
减少 doorbell 写操作在临界区里的时间。这在高速网络/块设备上是可测量的收益。

`virtqueue_notify()` 里的 `broken` 处理也值得注意：一旦 `vq->notify()`
返回 false（传输层写 doorbell 失败），就把队列永久标记为 `broken`，
后续所有操作都会快速失败。这是「设备已经不响应，别再浪费 CPU」的策略，
配合 `virtio_break_device()` 可以在设备热拔除时安全地叫醒所有等待者。

回收侧：

```c
void *virtqueue_get_buf_ctx(struct virtqueue *_vq, unsigned int *len, void **ctx)
{
    struct vring_virtqueue *vq = to_vvq(_vq);
    return VIRTQUEUE_CALL(vq, get, len, ctx);
}
void *virtqueue_get_buf(struct virtqueue *_vq, unsigned int *len)
{
    return virtqueue_get_buf_ctx(_vq, len, NULL);
}
```

`_ctx` 变体是给「一个 buffer 需要携带额外上下文」的驱动用的
（例如 `virtio_net` 需要记住这个 buffer 属于哪个 XDP 帧）。
`len` 由设备写回，表示「实际写入的字节数」，因此短写时驱动不需要预先清零
缓冲区——注释明确说明这一点，它是防止内核信息泄漏到 guest 的关键约定。

## 15. packed ring、IN_ORDER 与事件抑制

packed ring 的所有权判定与 split 完全不同：没有独立的 `avail.idx`/`used.idx`，
而是每个描述符带 `VRING_PACKED_DESC_F_AVAIL`/`USED` 位，配合一个
**wrap counter** 表示「绕环第几圈」。

- `vring_virtqueue_packed.avail_wrap_counter`：驱动侧的当前圈数。
- `vring_virtqueue_packed.next_avail_idx`：下一个要写的槽。
- `vring_virtqueue_packed.avail_used_flags`：驱动最后一次写入的 flags 影子。
- `vring_virtqueue_packed.event_flags_shadow`：driver event 的影子。

判定「这个槽是不是我的」的规则是「AVAIL 位是否等于当前 wrap」，
而不是比较索引。这样索引可以自由回绕，不需要在 16 位空间里做特殊处理。

`VIRTIO_F_IN_ORDER` 让「添加顺序」与「完成顺序」一致，从而简化回收：
驱动不需要为每个 buffer 记录 id，只要按顺序从 used ring 取即可。
本树为此增加了 `struct vring_virtqueue` 的 `batch_last` 字段（约 225 行）：

```c
/*
 * With IN_ORDER, once we see an in-order batch, this stores
 * this last entry, and until we return the last buffer.
 * After this, id is set to UINT_MAX to mark it invalid.
 * Unused without IN_ORDER.
 */
struct used_entry {
    u32 id;
    u32 len;
} batch_last;
```

也就是说，IN_ORDER 下驱动可以「一次看到一批」，把最后一个 entry 暂存在
`batch_last` 里，逐个返回；返回完最后一个后把 `id` 置 `UINT_MAX` 表示失效。
这避免了为每个 buffer 单独查 `desc_state`。

`free_head` 的语义在三种模式下各不相同（注释写得很清楚）：

| 模式 | `free_head` 含义 |
| --- | --- |
| 普通（非 IN_ORDER） | 空闲描述符链表的头 |
| IN_ORDER + SPLIT | 下一个可用的 buffer 索引 |
| IN_ORDER + PACKED | 未使用 |

事件抑制方面，split 用 `avail->flags` 的 `VRING_AVAIL_F_NO_INTERRUPT`
与 `used->flags` 的 `VRING_USED_F_NO_NOTIFY`，packed 用 driver/device 两个
`vring_packed_desc_event` 结构里的 `off_wrap` 与 `flags`。
`virtqueue_disable_cb()` / `virtqueue_enable_cb()` /
`virtqueue_enable_cb_delayed()` 三个接口在两种布局下都被映射到
`split_ops`/`packed_ops` 的对应回调，语义保持一致：

- `disable_cb()`：告诉对端「暂时别发中断」。
- `enable_cb()`：告诉对端「可以发了」，并返回「是否已经有未处理项」。
- `enable_cb_delayed()`：延迟使能，用于 NAPI 式合并。

`virtqueue_poll()` 是不依赖中断的轮询接口，`virtqueue_enable_cb_prepare()`
+ `virtqueue_poll()` 的组合让驱动可以「先使能中断再检查」，
避免「检查完、睡下、中断已过」的丢事件窗口。

## 16. 设备枚举与 feature 协商的逐行对照

第 6 节给出了 `virtio_dev_probe()` 的九步。这里把它与真实代码对齐
（`drivers/virtio/virtio.c` 约 270 行），并补上「为什么这样排序」。

```c
virtio_add_status(dev, VIRTIO_CONFIG_S_DRIVER);
virtio_get_features(dev, device_features);
virtio_features_zero(driver_features);
for (i = 0; i < drv->feature_table_size; i++) {
    unsigned int f = drv->feature_table[i];
    if (!WARN_ON_ONCE(f >= VIRTIO_FEATURES_BITS))
        virtio_features_set_bit(driver_features, f);
}
if (drv->feature_table_legacy) { ... } else { driver_features_legacy = driver_features[0]; }

if (virtio_features_test_bit(device_features, VIRTIO_F_VERSION_1)) {
    for (i = 0; i < VIRTIO_FEATURES_U64S; ++i)
        dev->features_array[i] = driver_features[i] & device_features[i];
} else {
    virtio_features_from_u64(dev->features_array,
                             driver_features_legacy & device_features[0]);
}
for (i = VIRTIO_TRANSPORT_F_START; i < VIRTIO_TRANSPORT_F_END; i++)
    if (virtio_features_test_bit(device_features, i))
        __virtio_set_bit(dev, i);
err = dev->config->finalize_features(dev);
if (drv->validate) { ... 若特性被改动则再次 finalize ... }
err = virtio_features_ok(dev);
err = drv->probe(dev);
if (!(dev->config->get_status(dev) & VIRTIO_CONFIG_S_DRIVER_OK))
    virtio_device_ready(dev);
if (drv->scan) drv->scan(dev);
virtio_config_core_enable(dev);
```

四个关键点：

1. **特性位图是「数组 + 位数」的抽象。** `VIRTIO_FEATURES_U64S` 个 `u64` 组成
   位图，`virtio_features_set_bit()`/`test_bit()` 封装了跨越 64 位边界的操作。
   现代 virtio 允许 64 位以上的特性（例如 `VIRTIO_F_ADMIN_VQ` 已是 41 位），
   所以不能再用单个 `u64`。
2. **legacy 设备只协商低 64 位。** 若设备没有 `VIRTIO_F_VERSION_1`，
   就走 `driver_features_legacy & device_features[0]` 这条窄路径。
   注意 `driver_features_legacy` 的来源：驱动若提供了 `feature_table_legacy`
   就用它，否则退化为 `driver_features[0]`。这个「退化」保证了
   不声明 legacy 表的驱动仍能与 legacy 设备协商。
3. **传输特性无条件保留。** `VIRTIO_TRANSPORT_F_START`(28) 到
   `VIRTIO_TRANSPORT_F_END`(42) 之间的位表示「这个传输层支持的能力」
   （如 `VIRTIO_F_RING_PACKED`、`VIRTIO_F_ACCESS_PLATFORM`），
   它们不属于设备功能，而是「设备与传输层共同的约定」，所以只要设备支持就置位，
   不参与驱动侧的 feature_table 过滤。
4. **`drv->validate()` 可能改写特性集，因此要二次 finalize。** 这是容易被忽略的一步：
   驱动可以在 `validate` 里根据设备状态**放弃**某些已协商特性（例如看到设备
   不支持某个组合），此时必须重新写回设备，否则设备与驱动对特性的认知不一致。

`virtio_features_ok()`（约 204 行）做最后一道校验：

```c
if (virtio_check_mem_acc_cb(dev)) {
    if (!virtio_has_feature(dev, VIRTIO_F_VERSION_1)) { ... return -ENODEV; }
    if (!virtio_has_feature(dev, VIRTIO_F_ACCESS_PLATFORM)) { ... return -ENODEV; }
}
if (!virtio_has_feature(dev, VIRTIO_F_VERSION_1))
    return 0;
virtio_add_status(dev, VIRTIO_CONFIG_S_FEATURES_OK);
status = dev->config->get_status(dev);
if (!(status & VIRTIO_CONFIG_S_FEATURES_OK)) { ... return -ENODEV; }
```

第一条检查的语义是：**只要传输层提供了内存访问回调（IOMMU/IOMMU 模拟），
就必须同时协商 `VERSION_1` 与 `ACCESS_PLATFORM`**。原因很实际：
`ACCESS_PLATFORM` 才是「设备访问内存要走平台映射（IOMMU）」的正式信号，
而只有 `VERSION_1` 才保证设备理解这个信号。缺任何一个，设备都可能
直接拿 GPA 去 DMA，绕过 IOMMU，破坏隔离。

最后 `FEATURES_OK` 的**回读**是关键：规范允许设备拒绝驱动提出的特性组合，
拒绝时设备不会置这一位。KVM 的 virtio 设备与 vDPA 硬件都可能拒绝，
所以这一步不能省。

`virtio_dev_remove()`（约 368 行）的顺序也值得注意：

```c
virtio_config_core_disable(dev);
drv->remove(dev);
WARN_ON_ONCE(dev->config->get_status(dev));   /* 驱动应该已 reset */
virtio_add_status(dev, VIRTIO_CONFIG_S_ACKNOWLEDGE);
of_node_put(dev->dev.of_node);
```

`WARN_ON_ONCE` 断言「驱动 remove 后设备状态已清零」。这条断言经常在
驱动忘记 `virtio_reset_device()` 时触发，是排查卸载路径泄漏的第一线索。

## 17. vhost 的 worker 模型

vhost 的早期实现是「每个 vhost_dev 一个 kthread + 一个 work 链表」。
本树的结构更细：`struct vhost_worker`（`drivers/vhost/vhost.h` 约 40 行）
是独立实体，并且可以被多个 vq 共享/挂接。

```c
struct vhost_worker {
    struct task_struct *kthread_task;
    struct vhost_task    *vtsk;
    struct vhost_dev     *dev;
    struct mutex         mutex;        /* 序列化 device-wide flush 与 worker 切换 */
    struct llist_head    work_list;
    struct kcov_common_handle_id kcov_handle;
    u32                  id;
    int                  attachment_cnt;
    bool                 killed;
    const struct vhost_worker_ops *ops;
};
```

五个字段值得解释：

- `work_list` 是 **llist**（无锁单链表），不是普通 `list_head`。
  因为入队方可能处于中断/软中断上下文（kick eventfd 的 wakeup 回调），
  不能睡眠，`llist_add()` 的 `cmpxchg` 循环是唯一安全的选择。
- `ops->wakeup()` 是「叫醒 worker」的抽象。内核态实现用
  `vhost_task`/kthread；`CONFIG_VHOST_ENABLE_FORK_OWNER_CONTROL`
  与 `fork_owner` 则允许把 worker 绑定到 QEMU 的进程上下文，
  以便权限/调度属性跟随 VMM。
- `attachment_cnt` 与 `killed` 是 flush 逻辑的守卫：`__vhost_worker_flush()`
  在 `!attachment_cnt || killed` 时直接返回，避免向已经停止的 worker
  投递 flush 工作后永久等待。
- `mutex` 的用途写在注释里：`Used to serialize device wide flushing with worker
  swapping`。`__vhost_worker_flush()` 会**主动释放**这个 mutex 再等待完成，
  因为 worker 被 kill 时可能需要拿同一把锁来清理，若不释放就会死锁。
- `kcov_handle` 让 vhost 的工作可以被 KCOV 覆盖统计，方便 fuzz。

入队路径：

```c
static void vhost_worker_queue(struct vhost_worker *worker, struct vhost_work *work)
{
    if (!test_and_set_bit(VHOST_WORK_QUEUED, &work->flags)) {
        llist_add(&work->node, &worker->work_list);
        worker->ops->wakeup(worker);
    }
}

bool vhost_vq_work_queue(struct vhost_virtqueue *vq, struct vhost_work *work)
{
    struct vhost_worker *worker;
    bool queued = false;
    rcu_read_lock();
    worker = rcu_dereference(vq->worker);
    if (worker) { queued = true; vhost_worker_queue(worker, work); }
    rcu_read_unlock();
    return queued;
}
```

`test_and_set_bit(VHOST_WORK_QUEUED)` 是「每个 work 同时只能在队列里一次」的
去重机制。如果没有它，同一个 vq 的多次 kick 会堆积多个相同的 work，
导致重复处理（虽然 usually 无害，但会放大 CPU 占用）。
`rcu_dereference(vq->worker)` 说明 worker 指针可以在运行期被替换
（由 vhost 核心在 worker 创建/切换/销毁时更新 `vq->worker`），所以读侧必须走 RCU。

flush 路径：

```c
static void __vhost_worker_flush(struct vhost_worker *worker)
{
    struct vhost_flush_struct flush;
    if (!worker->attachment_cnt || worker->killed)
        return;
    init_completion(&flush.wait_event);
    vhost_work_init(&flush.work, vhost_flush_work);
    vhost_worker_queue(worker, &flush.work);
    mutex_unlock(&worker->mutex);
    wait_for_completion(&flush.wait_event);
    mutex_lock(&worker->mutex);
}

void vhost_dev_flush(struct vhost_dev *dev)
{
    struct vhost_worker *worker;
    unsigned long i;
    xa_for_each(&dev->worker_xa, i, worker)
        vhost_worker_flush(worker);
}
```

「投递一个 flush work 然后等它完成」是经典的做法：因为 `work_list` 是 FIFO
（llist 的 `llist_add` 头插，但 worker 处理时会 `llist_del_all` 后反转或按序处理），
只要 flush work 完成，之前排队的 work 就都完成了。
`xa_for_each(&dev->worker_xa, ...)` 表明一个 `vhost_dev` 可以有多个 worker
（每个 vq 一个），flush 必须遍历全部。

`vhost_poll`（约 56 行）把「等待 kick eventfd 可读」变成 work：

```c
struct vhost_poll {
    poll_table        table;
    wait_queue_head_t *wqh;
    wait_queue_entry_t wait;
    struct vhost_work  work;
    __poll_t           mask;
    struct vhost_dev  *dev;
    struct vhost_virtqueue *vq;
};
```

`vhost_poll_init(poll, fn, mask, dev, vq)` 把 `fn`（如 `handle_tx_kick`）
登记为 work 的回调；`vhost_poll_start(poll, file)` 把 wait queue 挂到 kick fd 上；
`vhost_poll_queue(poll)` 等价于 `vhost_vq_work_queue(poll->vq, &poll->work)`。
这三步分离的好处是：QEMU 可以「先配置 poll 但不启动」，等所有 vring 参数就绪
再 `start`，避免用半初始化的 ring 触发数据面。

## 18. vhost-net 数据面：copy 与 zerocopy 双路径

第 8.2 节提到 `handle_tx()`/`handle_rx()`。本树的 `drivers/vhost/net.c`
实际把它们拆得更细：

| 函数 | 作用 |
| --- | --- |
| `handle_tx_copy()`（约 761 行） | 把 guest 数据拷贝到内核缓冲再 `sendmsg` |
| `handle_tx_zerocopy()`（约 864 行） | 用 `ubuf_info` 把 guest 页直接交给 skb，零拷贝 |
| `handle_tx()`（约 974 行） | 根据 `vq->ubufs` 选择上面两条 |
| `handle_rx()`（约 1140 行） | 从 TAP 读，填充 guest 预投的 buffer |
| `get_rx_bufs()`（约 1053 行） | 从 RX vq 预取 guest buffer 并建立 iov |
| `vhost_net_build_xdp()`（约 688 行） | 构造 XDP 帧头（XDP 场景） |
| `handle_tx_kick()` / `handle_rx_kick()`（约 1297/1306 行） | poll 回调 |
| `handle_tx_net()` / `handle_rx_net()`（约 1315/1322 行） | 另一种回调入口（按需） |

zerocopy 路径的关键是 `ubuf_info`：发送完成后内核通过它回调
`vhost_zerocopy_callback`，把描述符「延迟归还」。因此 zerocopy 下
`used.idx` 的推进与「网络栈真正发完」之间存在延迟，需要额外的
`done_idx`/`upend_idx` 记账（本树用 `vhost_net_virtqueue` 的字段维护）。
这也是 zerocopy 必须处理「guest 在数据发完前就 reset 设备」的原因。

`vhost_net_set_features()` 决定 `vhost_hlen`（virtio-net header 长度）：
`VIRTIO_NET_F_MRG_RXBUF` 或 `VIRTIO_F_VERSION_1` 时为 12 字节
（`virtio_net_hdr_mrg_rxbuf`），否则 10 字节（旧 `virtio_net_hdr`）。
头部长度的差异会直接影响 `get_rx_bufs()` 里「第一个 iov 要留多少空间」，
所以必须在协商完成之后、任何数据面操作之前确定。

`vhost_net_stop()`（约 1414 行）的顺序体现了「先停数据面、再断 socket」：
它会 `vhost_net_disable_vq()`（清 `VHOST_VRING_ENABLE`）→ `vhost_poll_stop()`
→ 关闭 TAP → `vhost_dev_stop()`。反过来做会导致「socket 已关，poll 还在
接收 EPOLLHUP，worker 继续处理空 ring」。

**当前配置下的注意事项**：`# CONFIG_VHOST_NET is not set`，上述代码完全不编译，
`/dev/vhost-net` 不存在，`vhost_net_open()` 也不会被调用。因此本节是
「源码存在但未启用」的说明；guest 网络只能走 QEMU 用户态 virtio-net 后端
或纯 TAP（无 vhost 加速）。这也意味着 QEMU 的 `-netdev tap,vhost=on`
在本内核上会失败，只能退化为 `vhost=off`。

## 19. vhost 的地址翻译：umem 与 IOTLB

vhost 拿到的是 guest 的 GPA（在 QEMU 的视角是「guest 物理地址」，
在宿主视角是「某个用户态进程的虚拟地址」）。`vhost_get_vq_desc_n()`
（约 2834 行）与 `vhost_get_vq_desc()`（约 2975 行）负责把这层翻译做完。

翻译分两级：

1. **`umem`（user memory）**：由 `VHOST_SET_MEM_TABLE` 下发的
   `struct vhost_memory` 描述「GPA 区间 → 用户态 VA 区间」。
   它覆盖整个 guest 物理地址空间，是 vhost 的「基础页表」。
2. **`iotlb`**：由 `VHOST_IOTLB_UPDATE` 一类消息更新的「GPA → IOVA」映射，
   在 `VIRTIO_F_ACCESS_PLATFORM` 协商后启用。它对应「设备侧 IOMMU」
   的地址空间，vhost 需要先查 iotlb 得到 IOVA，再查 umem 得到宿主 VA。

代码里 `vq->iotlb` 与 `vq->umem` 并存（`vhost.h` 约 149–150 行），
`vhost_get_vq_desc_n()` 的处理顺序是「先 iotlb，再 umem」：
若 `vq->iotlb` 存在且命中，就用 iotlb 给出的地址；否则回落到 umem。
`meta_iotlb[VHOST_NUM_ADDRS]` 缓存 desc/avail/used 三个地址的翻译结果，
避免每个 kick 都重新查表——这三个地址在队列生命周期内几乎不变。

IOTLB 的失效用 `vhost_iotlb_notify()` 处理（约 1591 行）：
它遍历所有 vq，对受影响的区间做 `vhost_vq_meta_reset()`（约 339 行），
把 `meta_iotlb[]` 清空，强制下次重新翻译。`vhost_init_device_iotlb()`
（约 2286 行）则在设备初始化时建立空的 iotlb 并注册消息处理器。

`vhost_dev_ioctl()`（约 2317 行）是设备级 ioctl 的总入口，
它按命令分派到：
`VHOST_SET_MEM_TABLE`（更新 umem，并 reset 所有 vq 的 meta）、
`VHOST_SET_LOG_BASE`/`VHOST_SET_LOG_FD`（脏页日志）、
`VHOST_SET_OWNER`（`vhost_dev_set_owner()`，约 1093 行，绑定 mm 与 worker）、
`VHOST_RESET_OWNER`、`VHOST_SET_VRING_*`（转发到 `vhost_vring_ioctl()`）、
`VHOST_IOTLB_*`。队列级命令由 `vhost_vring_ioctl()` 处理，
它会在每次改地址后调用 `vhost_vq_meta_reset()`，保证缓存不会指向旧地址。

## 20. 与 KVM eventfd 的闭环

vhost 的 kick/call eventfd 之所以能「不经过 QEMU」，靠的是 KVM 的
`irqfd`/`ioeventfd`（`virt/kvm/eventfd.c`）。这一节把两端接起来。

### 20.1 irqfd：从 eventfd 到 guest 中断

`irqfd` 的方向是「宿主某处 signal eventfd → guest 收到中断」。

```c
static void irqfd_inject(struct work_struct *work)
{
    struct kvm_kernel_irqfd *irqfd =
        container_of(work, struct kvm_kernel_irqfd, inject);
    struct kvm *kvm = irqfd->kvm;

    if (!irqfd->resampler) {
        kvm_set_irq(kvm, KVM_USERSPACE_IRQ_SOURCE_ID, irqfd->gsi, 1, false);
        kvm_set_irq(kvm, KVM_USERSPACE_IRQ_SOURCE_ID, irqfd->gsi, 0, false);
    } else {
        kvm_set_irq(kvm, KVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID, irqfd->gsi, 1, false);
    }
}
```

注意非 resampler 情况下的「先 1 再 0」：这是**脉冲**语义，对应 edge-triggered
中断。而 resampler 情况只拉高不拉低，因为拉低由 guest 的 EOI 触发
（`irqfd_resampler_ack()`），这样才符合 level-triggered 的完整握手。

`irqfd` 的注册在 `kvm_irqfd_assign()`（约 371 行）：

```c
INIT_WORK(&irqfd->inject, irqfd_inject);
init_waitqueue_func_entry(&irqfd->wait, irqfd_wakeup);
if (args->flags & KVM_IRQFD_FLAG_RESAMPLE) { ... }
irqfd->consumer.add_producer = kvm_arch_irq_bypass_add_producer;
irqfd->consumer.del_producer = kvm_arch_irq_bypass_del_producer;
irqfd->consumer.stop = kvm_arch_irq_bypass_stop;
irqfd->consumer.start = kvm_arch_irq_bypass_start;
```

`irq_bypass` 是「中断直通」框架：当生产者（如 VFIO 设备的中断）与消费者
（KVM 的 vCPU）能直接对接时，可以绕过「宿主中断处理 → eventfd → workqueue」
这一长链。`kvm_arch_irq_bypass_*` 在 ARM64 上是弱符号（约 352–357 行），
默认实现为空，由具体架构或 VFIO 侧决定是否启用。

撤销路径 `irqfd_shutdown()`（约 123 行）的顺序是：
`synchronize_srcu_expedited(&kvm->irq_srcu)` → `eventfd_ctx_remove_wait_queue()`
→ `flush_work(&irqfd->inject)` → 释放 resampler → `irq_bypass_unregister_consumer()`
→ `eventfd_ctx_put()`。先摘 wait queue 再 flush work，保证「不会再有新的
inject 被排队」；`synchronize_srcu` 则保证「正在跑的 inject 已经看到完整的
irqfd 状态」。

### 20.2 ioeventfd：从 guest MMIO 写到 eventfd

`ioeventfd` 的方向相反：「guest 写某个地址 → signal eventfd」。
它被实现成一个 `kvm_io_device`：

```c
static int ioeventfd_write(struct kvm_vcpu *vcpu, struct kvm_io_device *this,
                           gpa_t addr, int len, const void *val)
{
    struct _ioeventfd *p = to_ioeventfd(this);
    if (!ioeventfd_in_range(p, addr, len, val))
        return -EOPNOTSUPP;
    eventfd_signal(p->eventfd);
    return 0;
}

static const struct kvm_io_device_ops ioeventfd_ops = {
    .write      = ioeventfd_write,
    .destructor = ioeventfd_destructor,
};
```

`ioeventfd_in_range()` 会检查地址区间，并在 `datamatch` 有效时比较写入值
（约 796 行的 `_val == p->datamatch`）。`datamatch` 的意义是把「写任意值」
收窄为「写特定值」——virtio 的 doorbell 通常只关心队列索引，
所以可以用它过滤掉无关写入，减少 eventfd 唤醒次数。

注册路径 `kvm_assign_ioeventfd_idx()`（约 861 行）：

```c
eventfd = eventfd_ctx_fdget(args->fd);
p = kzalloc_obj(*p, GFP_KERNEL_ACCOUNT);
INIT_LIST_HEAD(&p->list);
p->addr = args->addr;
p->bus_idx = bus_idx;
p->length = args->len;
p->eventfd = eventfd;
```

之后会做 `ioeventfd_check_collision()`（约 836 行）检测冲突：
同一个 bus、同一地址、长度/值匹配规则重叠就拒绝。这条检查防止
两个 ioeventfd 争抢同一个 MMIO 地址，导致「谁先注册谁生效」的歧义。

总线选择由 `ioeventfd_bus_from_flags()`（约 852 行）决定：

```c
if (flags & KVM_IOEVENTFD_FLAG_PIO) return KVM_PIO_BUS;
if (flags & KVM_IOEVENTFD_FLAG_VIRTIO_CCW_NOTIFY) return KVM_VIRTIO_CCW_NOTIFY_BUS;
return KVM_MMIO_BUS;
```

这解释了为什么 `ioeventfd_write()` 只实现 `.write`：它只关心「写」事件，
读操作（`.read` 为空）会返回 `-EOPNOTSUPP`，让上层继续走 MMIO 模拟。

### 20.3 闭环：一次 TX 的完整路径

把两端接起来，一次 virtio-net TX 在本树（若 `VHOST_NET` 开启）是：

```
guest: start_xmit → virtqueue_add_outbuf → virtqueue_kick
  → vp_notify() 写 notify 区（MMIO/PIO）
  → 若该地址注册了 ioeventfd：ioeventfd_write() 命中 → eventfd_signal(kick_fd)
  → vhost 的 vhost_poll 被唤醒 → vhost_worker_queue(&poll->work)
  → worker 执行 handle_tx_kick → handle_tx → vhost_get_vq_desc
  → 查 iotlb/umem 得到宿主 VA → sendmsg 到 TAP
  → vhost_add_used_and_signal → eventfd_signal(call_fd)
  → 若该 fd 绑定了 irqfd：irqfd_wakeup → 排队 irqfd_inject
  → irqfd_inject → kvm_set_irq(gsi, 1) → VGIC 置 pending → kvm_vcpu_kick
  → guest 收到中断，回收 used ring
```

这条路径的价值在于**两端都不经过 QEMU 用户态**。代价是
「kick 与 call 都必须注册成 KVM 的 eventfd」——这正是 QEMU 在
`vhost=on` 时做的事：把 `VHOST_SET_VRING_KICK` 的 fd 通过
`KVM_IOEVENTFD` 绑到 notify 地址，把 `VHOST_SET_VRING_CALL` 的 fd 通过
`KVM_IRQFD` 绑到 GSI。

当前配置下 `VHOST_NET` 未启用，所以这条闭环的中间段（vhost 侧）不存在；
但 `KVM_IRQFD`/`KVM_IOEVENTFD` 本身是启用的（`CONFIG_HAVE_KVM_IRQCHIP` +
`CONFIG_HAVE_KVM_IRQ_ROUTING`），因此 QEMU 用户态后端仍然可以享受
「guest 中断/MMIO 不退出」的优化，只是数据面在用户态完成。

## 21. vDPA 与 vhost-vDPA 的结构补充

第 10 节已给出 `struct vdpa_device`/`struct vdpa_config_ops` 的骨架。
这里补充三点结构上的说明，便于对照源码阅读。

1. **vDPA 的核心是「把 virtio 的控制面数据结构化」。**
   `vdpa_config_ops` 里的 `set_vq_address`/`set_vq_num`/`set_vq_ready`/`kick_vq`
   分别对应 virtio-pci 的 `queue_desc`/`queue_size`/`queue_enable`/`notify` 寄存器。
   硬件的「doorbell」被抽象成 `kick_vq()`，中断被抽象成 `set_vq_cb()` 注册的
   `vdpa_callback`。因此 `virtio_vdpa.c` 可以把一个 vDPA 设备当成普通
   `virtio_device` 喂给现有驱动。
2. **三条使用路径共用同一个设备。** `virtio_vdpa`（前端驱动直接用）、
   `vhost_vdpa`（用户态 VMM 通过 `/dev/vhost-vdpa` 控制）、
   以及厂商自定义路径。三者对 `vdpa_config_ops` 的调用是互斥的：
   同一时刻只能有一个「driver」绑定到 vDPA 设备（由 `vdpa_driver` 的
   probe/remove 语义保证）。
3. **netlink 是管理面，不是数据面。** `VDPA_CMD_DEV_NEW`/`DEV_DEL`/`DEV_GET`
   只负责「创建/销毁/查询 vDPA 设备实例」，真正的队列配置仍走
   `vdpa_config_ops`。这与 vhost 的 `/dev/vhost-*` ioctl 是两套完全不同的接口，
   不要混淆。

当前 `CONFIG_VDPA` 未启用，`drivers/vdpa/`、`virtio_vdpa.c`、`vhost/vdpa.c`
都不编译。若启用，它们会引入对 `CONFIG_VHOST` 的依赖（`vhost/vdpa.c`），
而 `CONFIG_VHOST` 又由 `VHOST_NET`/`VHOST_SCSI`/`VHOST_VSOCK` select——
这就是为什么「只开 VDPA 不开 VHOST_NET」在 Kconfig 上需要额外注意。

## 22. 配置与调试速查

| 主题 | 观测点 | 说明 |
| --- | --- | --- |
| 前端队列状态 | `/sys/bus/virtio/devices/*/` | 设备 ID、状态位、feature |
| vring 布局 | `virtio_ring.c` 的 `layout` 字段 | split 还是 packed，由 `VIRTIO_F_RING_PACKED` 决定 |
| 队列深度 | `vq->num_max` / `num_free` | `num_free == 0` 时 `add` 返回 `-ENOSPC` |
| broken 队列 | `vq->broken` | `notify` 失败后永久置位，需 reset 恢复 |
| vhost worker | `/proc/<qemu>/task/*/comm` | worker 线程名，配合 `fork_owner` 观察 |
| vhost IOTLB | `VHOST_IOTLB_*` ioctl 返回值 | `-EINVAL` 常见于未协商 `ACCESS_PLATFORM` |
| irqfd 冲突 | `KVM_IRQFD` 返回 `-EEXIST` | 同一 GSI 重复绑定 |
| ioeventfd 冲突 | `KVM_IOEVENTFD` 返回 `-EEXIST` | 同 bus/地址/长度/值重叠 |

几个容易踩的配置事实（按本树 `.config`）：

- `CONFIG_VIRTIO_PCI_LEGACY=y` 意味着 legacy 设备也能被驱动，但
  legacy 路径只能协商低 64 位特性，`VIRTIO_F_ACCESS_PLATFORM` 无法使用，
  因此在有 IOMMU 的宿主上 legacy 设备会被 `virtio_features_ok()` 拒绝
  （若传输层提供了 mem access 回调）。
- `CONFIG_VHOST_ENABLE_FORK_OWNER_CONTROL=y` 允许用户态通过
  `VHOST_SET_OWNER` 的 fork 变体把 worker 绑到自己的进程，便于
  用 cgroup/权限约束 vhost 线程。它只影响 `fork_owner` 路径，
  不影响数据面语义。
- `CONFIG_VHOST_CROSS_ENDIAN_LEGACY` 未启用，因此 `vhost.h` 里的
  `user_be` 字段不编译，legacy 大端 guest 的 vhost 加速不可用。

## 23. 补充文件清单（本篇新增章节涉及）

| 路径 | 作用 |
| --- | --- |
| `drivers/virtio/virtio_ring.c` | `struct virtqueue_ops`、四张 ops 表、`VIRTQUEUE_CALL`、`virtqueue_add_sgs/outbuf/inbuf`、`virtqueue_kick_prepare/notify`、`virtqueue_get_buf_ctx` |
| `drivers/virtio/virtio.c` | `virtio_dev_probe()`、`virtio_features_ok()`、`virtio_dev_remove()`、`virtio_reset_device()`、`register_virtio_device()` |
| `drivers/vhost/vhost.h` | `struct vhost_work`、`struct vhost_worker`、`struct vhost_poll`、`struct vhost_vring_call`、`struct vhost_virtqueue` |
| `drivers/vhost/vhost.c` | `vhost_worker_queue()`、`vhost_vq_work_queue()`、`__vhost_worker_flush()`、`vhost_dev_flush()`、`vhost_get_vq_desc_n()`、`vhost_dev_ioctl()`、`vhost_init_device_iotlb()`、`vhost_iotlb_notify_vq()` |
| `drivers/vhost/net.c` | `handle_tx_copy()`、`handle_tx_zerocopy()`、`handle_rx()`、`get_rx_bufs()`、`vhost_net_build_xdp()`、`handle_tx_kick()`、`vhost_net_stop()` |
| `virt/kvm/eventfd.c` | `irqfd_inject()`、`irqfd_wakeup()`、`irqfd_shutdown()`、`kvm_irqfd_assign()`、`kvm_irqfd_deassign()`、`ioeventfd_write()`、`ioeventfd_in_range()`、`kvm_assign_ioeventfd_idx()`、`kvm_ioeventfd()` |
| `virt/kvm/irqchip.c` | `kvm_set_irq()` 的实现，是 irqfd 注入的终点（GSI → VGIC） |
