# NVMe PCIe Host 驱动分析文档

## 1. 概述

NVMe（Non-Volatile Memory Express）PCIe Host 驱动是 Linux 内核中用于管理 NVMe SSD 的核心驱动程序。该驱动位于 `drivers/nvme/host/pci.c`，采用 PCIe 总线协议与 NVMe 控制器通信。

### 代码架构分层

```
┌─────────────────────────────────────┐
│          Block Layer (blk-mq)       │  ← 通用块层
├─────────────────────────────────────┤
│    nvme-core (core.c / ioctl.c)     │  ← NVMe 协议核心层
├─────────────────────────────────────┤
│       nvme (pci.c - PCIe 驱动)       │  ← PCIe 传输层
├─────────────────────────────────────┤
│         PCIe 总线 / NVMe 控制器       │  ← 硬件
└─────────────────────────────────────┘
```

### 模块依赖

根据 Makefile 定义：

- **nvme-core.o**: core.o, ioctl.o, sysfs.o, pr.o 等核心功能
- **nvme.o**: pci.o（PCIe 传输层）
- 其他传输方式: rdma.o（RDMA）、fc.o（Fibre Channel）、tcp.o（TCP）

---

## 2. 核心数据结构

### 2.1 nvme_dev —— PCIe 设备结构体

```c
struct nvme_dev {
    struct nvme_queue *queues;           // 所有队列（admin + I/O）
    struct blk_mq_tag_set tagset;        // I/O 队列标签集
    struct blk_mq_tag_set admin_tagset;  // Admin 队列标签集
    u32 __iomem *dbs;                    // Doorbell 寄存器映射
    struct device *dev;                  // PCIe 设备
    unsigned online_queues;              // 在线队列数
    unsigned max_qid;                    // 最大队列 ID
    unsigned io_queues[HCTX_MAX_TYPES];  // 不同类型 I/O 队列数
    unsigned int num_vecs;               // 中断向量数
    u32 q_depth;                         // 队列深度
    int io_sqes;                         // I/O SQ Entry 大小(2^sqes)
    u32 db_stride;                       // Doorbell 步长
    void __iomem *bar;                   // BAR 映射地址
    unsigned long bar_mapped_size;       // BAR 映射大小
    struct mutex shutdown_lock;          // 关闭/复位锁
    bool subsystem;                      // 是否支持 NVM Subsystem Reset
    u64 cmb_size;                        // Controller Memory Buffer 大小
    bool cmb_use_sqes;                   // 使用 CMB 存放 SQ
    u32 cmbsz, cmbloc;                   // CMB 大小/位置寄存器值
    struct nvme_ctrl ctrl;               // 通用控制器（父类）
    u32 last_ps;                         // 上次电源状态
    bool hmb;                            // Host Memory Buffer 启用标志
    /* Shadow Doorbell Buffer */
    __le32 *dbbuf_dbs, *dbbuf_eis;      // Shadow Doorbell 缓冲区
    /* Host Memory Buffer */
    u64 host_mem_size;                   // HMB 大小
    struct nvme_host_mem_buf_desc *host_mem_descs;
    struct nvme_descriptor_pools descriptor_pools[];  // DMA 池（per-NUMA）
};
```

### 2.2 nvme_queue —— NVMe 队列结构体

每个队列由一个 Submission Queue (SQ) 和 Completion Queue (CQ) 组成：

```c
struct nvme_queue {
    struct nvme_dev *dev;                // 所属设备
    struct nvme_descriptor_pools pools;  // DMA 描述符池
    spinlock_t sq_lock;                  // SQ 自旋锁
    void *sq_cmds;                       // SQ 命令缓冲区（DMA 一致内存）
    struct nvme_completion *cqes;        // CQ 完成条目缓冲区（DMA 一致内存）
    dma_addr_t sq_dma_addr;              // SQ DMA 地址
    dma_addr_t cq_dma_addr;              // CQ DMA 地址
    u32 __iomem *q_db;                   // 队列 Doorbell 寄存器
    u32 q_depth;                         // 队列深度
    u16 cq_vector;                       // CQ 中断向量号
    u16 sq_tail, last_sq_tail;           // SQ 尾指针
    u16 cq_head;                         // CQ 头指针
    u16 qid;                             // 队列 ID（0=admin）
    u8 cq_phase;                         // CQ 阶段位（用于检测新完成项）
    u8 sqes;                            // SQ Entry 大小
    unsigned long flags;                 // 标志位
#define NVMEQ_ENABLED     0              // 队列已启用
#define NVMEQ_SQ_CMB      1              // SQ 位于 CMB
#define NVMEQ_DELETE_ERROR 2             // 删除出错
#define NVMEQ_POLLED       3             // 轮询队列（无中断）
};
```

### 2.3 nvme_iod —— I/O 请求描述符

```c
struct nvme_iod {
    struct nvme_request req;             // 请求基类
    struct nvme_command cmd;             // NVMe 命令
    u8 flags;                            // 标志位
    u8 nr_descriptors;                   // PRP/SGL 描述符数量
    size_t total_len;                    // 数据总长度
    struct dma_iova_state dma_state;     // DMA IOVA 状态
    void *descriptors[NVME_MAX_NR_DESCRIPTORS]; // PRP/SGL 描述符指针
    struct nvme_dma_vec *dma_vecs;       // DMA 向量数组
    unsigned int nr_dma_vecs;            // DMA 向量数
    dma_addr_t meta_dma;                 // 元数据 DMA 地址
    struct nvme_sgl_desc *meta_descriptor; // 元数据 SGL 描述符
};
```

### 2.4 nvme_ctrl —— 通用控制器（核心层）

位于 `nvme.h`，是 PCIe/RDMA/FC/TCP 各传输方式的统一抽象：

- **状态机**: `NEW → CONNECTING → LIVE ↔ RESETTING → DELETING → DEAD`
- **关键字段**: admin_q、tagset、cap、vs、oncs、oacs 等能力寄存器缓存
- **工作机制**: reset_work（复位工作项）、ka_work（Keep Alive）、scan_work（命名空间扫描）

---

## 3. NVMe 控制器状态机

```
                 ┌──────────┐
                 │   NEW    │  ← 初始分配
                 └────┬─────┘
                      │ nvme_init_ctrl_finish()
                      ▼
              ┌───────────────┐
         ┌────│  CONNECTING   │◄──────────────┐
         │    └───────┬───────┘               │
         │            │ 配置完成                │
         │            ▼                        │
         │    ┌───────────────┐                │
         │    │     LIVE      │    reset_work  │
         │    └───┬───┬───────┘  ─────────────┘
         │        │   │
         │        │   │ 异常/超时
         │        │   ▼
         │        │ ┌───────────────┐
         │        │ │  RESETTING    │── 重新使能设备 → CONNECTING → LIVE
         │        │ └───────────────┘
         │        │
         │        │ 删除/移除
         │        ▼
         │  ┌──────────────┐
         │  │   DELETING   │
         │  └──────┬───────┘
         │         │ Async Event 处理完毕
         │         ▼
         │  ┌──────────────────┐
         │  │ DELETING_NOIO    │
         │  └──────┬───────────┘
         │         │ Namespace 移除完成
         │         ▼
         │  ┌──────────────┐
         └──│     DEAD     │  ← 终态
            └──────────────┘
```

---

## 4. 关键流程分析

### 4.1 设备初始化流程 (nvme_probe)

```
nvme_probe(pdev, id)
│
├─ nvme_pci_alloc_dev()
│  ├─ 分配 nvme_dev (含 per-NUMA descriptor_pools)
│  ├─ INIT_WORK(&ctrl.reset_work, nvme_reset_work)
│  ├─ 分配 queues 数组 (nr_allocated_queues)
│  ├─ nvme_init_ctrl() → 设置状态为 NEW
│  │  ├─ 初始化 spinlock / srcu / workqueue
│  │  ├─ scan_work / async_event_work / ka_work 等
│  │  ├─ 分配 instance id (nvme0, nvme1...)
│  │  └─ 初始化 ctrl_device 字符设备
│  ├─ DMA mask 设置 (64bit / 48bit quirk)
│  └─ 设置 max_hw_sectors、max_segments
│
├─ nvme_add_ctrl() → 注册 ctrl_device
│
├─ nvme_dev_map()
│  ├─ pci_request_mem_regions()
│  └─ nvme_remap_bar() → ioremap BAR
│
├─ nvme_pci_alloc_iod_mempool()
│
├─ nvme_pci_enable()
│  ├─ pci_enable_device_mem()
│  ├─ pci_set_master()
│  ├─ pci_alloc_irq_vectors() (预分配 1 个向量)
│  ├─ 读取 CAP 寄存器: q_depth / db_stride
│  ├─ nvme_map_cmb() → 映射 Controller Memory Buffer
│  └─ nvme_pci_configure_admin_queue()
│     ├─ nvme_remap_bar() (扩展 BAR 映射)
│     ├─ nvme_disable_ctrl() → 关闭控制器
│     ├─ nvme_alloc_queue(0, NVME_AQ_DEPTH) → 分配 admin 队列
│     ├─ 写 AQA/ASQ/ACQ 寄存器
│     ├─ nvme_enable_ctrl() → 使能控制器
│     ├─ nvme_init_queue() → 初始化队列
│     └─ queue_request_irq() → 注册 admin 队列中断
│
├─ nvme_alloc_admin_tag_set() → 创建 admin blk-mq tagset
│
├─ 设置状态为 CONNECTING
│
├─ nvme_init_ctrl_finish()
│  ├─ 读取 VS 寄存器
│  ├─ nvme_init_identify() → 发送 IDENTIFY 命令
│  │  ├─ ADMIN: 获取控制器属性 (onncs, oacs, sqsize 等)
│  │  └─ NAMESPACE: 扫描命名空间
│  ├─ nvme_configure_apst()
│  └─ nvme_configure_timestamp()
│
├─ nvme_dbbuf_dma_alloc() → 分配 Shadow Doorbell 缓冲区
├─ nvme_setup_host_mem() → 分配 Host Memory Buffer (可选)
├─ nvme_setup_io_queues()
│  ├─ nvme_set_queue_count() → 协商 I/O 队列数
│  ├─ nvme_setup_irqs() → 分配 MSI/MSI-X 中断
│  │  ├─ nvme_calc_irq_sets() → 计算 default/read/poll 队列分布
│  │  └─ pci_alloc_irq_vectors_affinity()
│  └─ nvme_create_io_queues()
│     ├─ nvme_alloc_queue() → 分配 SQ/CQ DMA 缓冲区
│     └─ nvme_create_queue() (循环)
│        ├─ adapter_alloc_cq() → admin 命令创建 CQ
│        ├─ adapter_alloc_sq() → admin 命令创建 SQ
│        ├─ nvme_init_queue()
│        ├─ queue_request_irq() → 注册中断
│        └─ 设置 NVMEQ_ENABLED
│
├─ nvme_alloc_io_tag_set() → 创建 I/O blk-mq tagset
├─ nvme_dbbuf_set() → 通知控制器 Shadow Doorbell 地址
├─ 设置状态为 LIVE
├─ nvme_start_ctrl()
│  ├─ nvme_enable_aen() → 注册异步事件通知
│  ├─ nvme_queue_scan() → 扫描命名空间
│  └─ nvme_unquiesce_io_queues()
└─ flush_work(&scan_work) → 等待扫描完成
```

### 4.2 I/O 请求提交流程

```
nvme_queue_rq(hctx, bd)
│
├─ 检查 NVMEQ_ENABLED 状态
├─ nvme_check_ready() → 检查控制器状态是否为 LIVE
│
├─ nvme_prep_rq()
│  ├─ nvme_setup_cmd() → 构造 NVMe 命令 (core 层)
│  ├─ nvme_map_data() → DMA 映射数据缓冲区
│  │  ├─ nvme_pci_setup_data_simple() — 单段快速路径
│  │  ├─ nvme_pci_setup_data_prp() — PRP 列表方式
│  │  └─ nvme_pci_setup_data_sgl() — SGL 方式
│  └─ nvme_map_metadata() → DMA 映射元数据(保护信息)
│
├─ spin_lock(&sq_lock)
├─ nvme_sq_copy_cmd() → memcpy 命令到 SQ
├─ nvme_write_sq_db() → 写 Doorbell 通知控制器
└─ spin_unlock(&sq_lock)

// 批处理路径 (多个请求)
nvme_queue_rqs(rqlist)
├─ 遍历请求，根据 nvmeq 分组
├─ nvme_prep_rq_batch() → 准备每个请求
├─ nvme_submit_cmds() → 批量提交到同一个队列
│  ├─ 循环 nvme_sq_copy_cmd()
│  └─ 最后 nvme_write_sq_db(true)
└─ 将失败的请求放入 requeue_list
```

### 4.3 中断完成流程

```
nvme_irq(irq, data)
│
└─ nvme_poll_cq(nvmeq, &iob)
   │
   ├─ while (nvme_cqe_pending())  → 检查 CQ 阶段位
   │  ├─ dma_rmb()               → DMA 读内存屏障
   │  ├─ nvme_handle_cqe()
   │  │  ├─ nvme_complete_async_event() — AEN 特殊处理
   │  │  └─ nvme_find_rq() → 根据 command_id 找到 request
   │  │     └─ nvme_try_complete_req()
   │  │        ├─ 设置 status / result
   │  │        └─ blk_mq_complete_request_remote()
   │  └─ nvme_update_cq_head()
   │
   └─ nvme_ring_cq_doorbell() → 写 CQ Doorbell (通知控制器释放 CQE)
```

### 4.4 控制器复位流程 (nvme_reset_work)

```
nvme_reset_work(work)
│
├─ nvme_dev_disable(dev, false)
│  ├─ nvme_start_freeze() → 冻结 I/O
│  ├─ nvme_quiesce_io_queues()
│  ├─ nvme_delete_io_queues() → 删除 I/O 队列 (admin cmd)
│  │  ├─ __nvme_delete_io_queues(SQ) → 逐个删除
│  │  └─ __nvme_delete_io_queues(CQ) → 逐个删除
│  ├─ nvme_disable_ctrl() → 设置 CC.EN=0
│  ├─ nvme_suspend_io_queues() → 挂起队列 + 释放中断
│  ├─ pci_free_irq_vectors()
│  └─ pci_disable_device()
│
├─ nvme_pci_enable() → 重新使能 PCIe 设备
├─ 设置状态 CONNECTING
├─ nvme_init_ctrl_finish() → 重新初始化
├─ nvme_dbbuf_dma_alloc() / nvme_setup_host_mem()
├─ nvme_setup_io_queues() → 重新创建 I/O 队列
├─ nvme_dbbuf_set() / nvme_unquiesce_io_queues()
├─ nvme_pci_update_nr_queues()
│  └─ blk_mq_update_nr_hw_queues()
├─ 设置状态 LIVE
└─ nvme_start_ctrl()
```

### 4.5 超时处理 (nvme_timeout)

```
nvme_timeout(req)
│
├─ 检查 PCIe 设备是否断开 → 设置 DELETING 状态
├─ 检查是否处于终态 → 直接 disable
├─ 检查 CSTS.CFS / NSSRO → 控制器故障，触发 reset
├─ 轮询 CQ（防止假中断丢失）
│  ├─ poll 队列: nvme_poll()
│  └─ 中断队列: nvme_poll_irqdisable() → disable_irq + poll + enable_irq
├─ 如果请求已完成 → 返回 BLK_EH_DONE
│
├─ 根据控制器状态处理:
│  ├─ CONNECTING/DELETING → 标记 CANCELLED + disable
│  ├─ RESETTING → 返回 BLK_EH_RESET_TIMER
│  └─ LIVE → 发送 ABORT 命令
│     ├─ atomic_dec(&abort_limit)
│     ├─ 构造 nvme_admin_abort_cmd
│     ├─ blk_execute_rq_nowait(abort_req)
│     └─ 返回 BLK_EH_RESET_TIMER
│
└─ 若需要 reset:
   ├─ 设置 RESETTING 状态
   ├─ nvme_dev_disable()
   └─ nvme_try_sched_reset() → schedule reset_work
```

---

## 5. 关键特性

### 5.1 PRP 与 SGL 数据描述方式

NVMe 支持两种描述数据传输方式：

| 特性     | PRP (Physical Region Page) | SGL (Scatter Gather List)       |
| -------- | -------------------------- | ------------------------------- |
| 描述格式 | PRP1 + PRP2，PRP2 可为链表 | SGL 段描述符链表                |
| 适用场景 | 传统方式，所有控制器支持   | 较新控制器，支持非对齐传输      |
| 切换阈值 | 默认 32KB 以下倾向 PRP     | 大于等于 sgl_threshold 时用 SGL |
| 页面间隙 | 不支持                     | 支持                            |

判断入口：`nvme_pci_use_sgls()`：

- 如果 `req_phys_gap_mask()` 检测到页面间隙 → 强制 SGL
- 如果是用户命令（passthrough）→ 强制 SGL
- 如果平均段大小 >= sgl_threshold → 使用 SGL

### 5.2 Shadow Doorbell Buffer (SDBB)

NVMe 1.3+ 可选特性，通过主机内存中的影子门铃减少 MMIO 写操作：

```
初始化: nvme_dbbuf_dma_alloc() → 分配 DMA 一致内存
配置:   nvme_dbbuf_set() → admin 命令设置主机地址
运行:   nvme_dbbuf_update_and_check_event()
        1. wmb() + 写影子门铃
        2. mb() + 读取事件索引
        3. 如果不需要触发中断 → 跳过 MMIO
        4. 否则 writel() → 写实际门铃寄存器
```

### 5.3 Controller Memory Buffer (CMB)

NVMe 1.2+ 特性，控制器提供片上内存用于存放 SQ 条目：

- 检测: `nvme_map_cmb()` → 读取 CMBSZ/CMBLOC 寄存器
- 使用: `nvme_alloc_sq_cmds()` → 优先从 CMB 分配 SQ
- 条件: `use_cmb_sqes` 模块参数控制（默认开启）

### 5.4 Host Memory Buffer (HMB)

NVMe 1.2+ 特性，主机分配内存给控制器加速：

- 分配策略: `nvme_alloc_host_mem()` → 从大到小尝试
- 单段: IOMMU 合并支持时使用 dma_alloc_noncontiguous
- 多段: 按 chunk_size 分段分配，每段不超过 MAX_ORDER
- 模块参数: `max_host_mem_size_mb = 128`（默认上限）

### 5.5 中断与队列映射

```
队列类型:
├─ HCTX_TYPE_DEFAULT — 默认读写队列
├─ HCTX_TYPE_READ   — 专用读队列（可选）
└─ HCTX_TYPE_POLL   — 轮询队列（无中断）

中断分配:
├─ nvme_setup_irqs()
│  ├─ pre_vectors = 1 (admin 队列固定占用向量 0)
│  ├─ nvme_calc_irq_sets() 回调
│  └─ pci_alloc_irq_vectors_affinity()
│
└─ 队列创建时:
   ├─ 中断队列: vector = qid (num_vecs > 1)
   └─ poll 队列: 设置 NVMEQ_POLLED，不注册中断
```

### 5.6 硬件 quirk 机制

驱动通过 `nvme_id_table` 中的 `driver_data` 字段为不同设备启用 workaround：

| Quirk 标志                      | 问题描述                    |
| ------------------------------- | --------------------------- |
| NVME_QUIRK_STRIPE_SIZE          | 需要对齐 stripe size        |
| NVME_QUIRK_DELAY_BEFORE_CHK_RDY | 检查 RDY 前需要延迟         |
| NVME_QUIRK_NO_APST              | 禁用自动电源状态转换        |
| NVME_QUIRK_SINGLE_VECTOR        | 所有队列共享一个向量        |
| NVME_QUIRK_BROKEN_MSI           | MSI 中断不工作，只使用 INTx |
| NVME_QUIRK_DISABLE_WRITE_ZEROES | Write Zeroes 命令有问题     |
| NVME_QUIRK_BOGUS_NID            | 命名空间标识符错误          |

---

## 6. 整体流程图

### 6.1 驱动生命周期总图

```text
# NVMe PCIe 驱动生命周期总图 - 函数调用栈
#
# 模块加载 → 设备探测 → 运行期 → 异常恢复/设备移除
# 两条主线: 正常探测(nvme_probe) 和 设备移除(nvme_remove)

nvme_init
  │  # 模块初始化入口, 注册 PCI 驱动
  │
  └─ pci_register_driver
      │  # 注册 nvme_pci_driver 到 pci_bus_type 驱动列表
      │  # 内部调用 __pci_register_driver → driver_register
      │
      └─ PCI 总线匹配 nvme_id_table
          │  # 扫描 PCI 总线, 匹配 Vendor/Device ID
          │  # 匹配成功 → nvme_probe; 设备拔除 → nvme_remove
          │
          ├─ [匹配成功] → nvme_probe (设备探测阶段)
          │   │  # PCI 设备探测回调, 初始化 NVMe 控制器
          │   │
          │   ├─ nvme_pci_alloc_dev
          │   │   # 分配 nvme_dev 结构体, 初始化 per-NUMA DMA 描述符池
          │   │
          │   ├─ nvme_dev_map
          │   │   # ioremap BAR0, 映射 NVMe 控制器寄存器空间
          │   │
          │   ├─ nvme_pci_enable
          │   │   # pci_enable_device, pci_set_master, 设置 DMA mask
          │   │   # 启用 MSI/MSI-X 中断
          │   │
          │   ├─ nvme_pci_configure_admin_queue
          │   │   # 配置 Admin SQ/CQ: 写 AQA/ASQ/ACQ 寄存器
          │   │   # 设置 CC.EN=1 启用控制器
          │   │
          │   ├─ nvme_init_ctrl_finish
          │   │   # Admin IDENTIFY 命令, 读取控制器能力
          │   │   # 配置 APST 电源管理, 分配 HMB
          │   │
          │   ├─ nvme_setup_io_queues
          │   │   # 创建 I/O SQ/CQ, 分配中断向量
          │   │   # 分配 I/O tag set, 配置 Shadow Doorbell
          │   │
          │   └─ nvme_start_ctrl
          │       # 设置控制器状态 LIVE, 启动控制器
          │       # 启用 AEN 异步事件通知, 扫描 Namespace
          │       │
          │       ├─ [运行期] I/O 请求处理
          │       │   # 处理 blk-mq 提交的读写请求 (nvme_queue_rq)
          │       │
          │       ├─ [运行期] 中断处理 (nvme_irq)
          │       │   # CQ 中断 → nvme_poll_cq → nvme_handle_cqe
          │       │
          │       ├─ [运行期] Keep Alive
          │       │   # 定期发送 Keep Alive 命令, 防止控制器超时
          │       │
          │       ├─ [运行期] Async Event
          │       │   # 异步事件通知 (温度/健康状态/Namespace 变更等)
          │       │
          │       └─ [异常恢复] I/O 超时
          │           │  # 当 I/O 请求超时时触发 nvme_timeout
          │           │
          │           └─ nvme_timeout
          │               │  # 超时处理: 读取 CSTS 寄存器
          │               │  # 判断设备是否断开 / 控制器是否故障
          │               │
          │               └─ [控制器故障] nvme_reset_work
          │                   │  # 异步复位工作队列
          │                   │  # nvme_dev_disable → nvme_pci_enable
          │                   │  # nvme_init_ctrl_finish → nvme_setup_io_queues
          │                   │  # nvme_start_ctrl → 回到运行期
          │                   │
          │                   └─ 回到 nvme_start_ctrl (运行期)
          │
          └─ [设备拔除] → nvme_remove (设备移除阶段)
              │  # PCI 设备移除回调, 安全关闭控制器
              │
              ├─ 设置 DELETING 状态
              │   # 标记控制器状态为 DELETING, 阻止新 I/O 请求
              │
              ├─ flush reset_work
              │   # 等待正在进行的复位操作完成
              │
              ├─ nvme_stop_ctrl
              │   # 停止控制器: 发送 Shutdown 通知 (CC.SHN)
              │
              ├─ 移除 Namespaces
              │   # 删除所有命名空间设备节点 (/dev/nvmeXnY)
              │
              ├─ nvme_dev_disable
              │   # 关闭控制器: CC.EN=0, 释放中断向量
              │
              └─ 释放资源
                  # 释放 DMA 内存, iounmap BAR, 释放 nvme_dev
```

### 6.2 初始化流程详细图

```text
# NVMe PCIe 初始化流程详细调用栈
#
# PCI Probe → Admin 队列配置 → IDENTIFY → I/O 队列创建 → 设备就绪
# 分为三个大阶段: 控制器初始化 / IDENTIFY / I/O 队列设置

nvme_probe (PCI Probe)
  │  # PCI 设备探测入口, 总线匹配成功后调用
  │
  ├─ nvme_pci_alloc_dev
  │   # 分配 nvme_dev 结构体
  │   # 初始化 per-NUMA DMA 描述符池 (nvme_descriptor_pools)
  │
  ├─ nvme_init_ctrl
  │   # 初始化通用控制器 (nvme_ctrl) 字段
  │   # 设置控制器状态: NVME_CTRL_NEW
  │   # 设置 ops 回调表 (nvme_pci_ctrl_ops)
  │
  └─ nvme_add_ctrl
      │  # 将控制器注册到 NVMe 子系统
      │
      ├─ nvme_dev_map
      │   # pci_request_mem_regions: 请求 PCI 内存区域
      │   # ioremap BAR0: 映射 NVMe 寄存器空间
      │
      ├─ nvme_pci_enable
      │   # pci_enable_device: 使能 PCI 设备
      │   # pci_set_master: 启用总线主控
      │   # 设置 DMA mask (64/32 bit)
      │   # pci_alloc_irq_vectors: 分配 MSI/MSI-X 中断向量
      │
      ├─ 读取 CAP 寄存器
      │   # 从 BAR0 偏移 0x00 读取 Controller Capabilities
      │   # 获取: MQES(最大队列深度), CQR(需要连续队列),
      │   #       DSTRD(Doorbell 步长), TO(超时), CSS(命令集)
      │
      ├─ nvme_map_cmb
      │   # 映射 Controller Memory Buffer (CMB)
      │   # 读取 CMBSZ/CMBLOC 寄存器, ioremap CMB 区域
      │   # CMB 可用于存放 SQ 以减少 PCIe 读延迟
      │
      ├─ nvme_pci_configure_admin_queue
      │   │  # 配置 Admin Submission/Completion Queue
      │   │
      │   ├─ nvme_disable_ctrl
      │   │   # 写 CC.EN=0, 确保控制器处于禁用状态
      │   │   # 等待 CSTS.RDY=0 (控制器就绪位清零)
      │   │
      │   ├─ nvme_alloc_queue
      │   │   # 分配 Admin SQ 和 CQ 的 DMA 一致内存
      │   │   # SQ: 提交队列条目缓冲区 (nvme_command)
      │   │   # CQ: 完成队列条目缓冲区 (nvme_completion)
      │   │
      │   ├─ 写 AQA/ASQ/ACQ 寄存器
      │   │   # AQA: Admin Queue Attributes (SQ/CQ 大小)
      │   │   # ASQ: Admin SQ 基地址 (低32位 + 高32位)
      │   │   # ACQ: Admin CQ 基地址 (低32位 + 高32位)
      │   │
      │   ├─ nvme_enable_ctrl
      │   │   # 写 CC.EN=1, CC.CSS=NVM, CC.MPS=页大小
      │   │   # 等待 CSTS.RDY=1 (控制器就绪)
      │   │   # 超时: CAP.TO * 500ms (默认 60s)
      │   │
      │   └─ nvme_init_queue
      │       # 初始化 Admin 队列的 Doorbell 指针
      │       # 设置 SQ tail = 0, CQ head = 0
      │       # 写 SQ0TDBL/CQ0HDBL 寄存器
      │
      ├─ queue_request_irq
      │   # 注册 Admin CQ 中断处理函数
      │   # 中断向量: pci_irq_vector(dev, 0)
      │   # 中断处理: nvme_irq → nvme_poll_cq
      │
      ├─ nvme_alloc_admin_tag_set
      │   # 分配 Admin tag set (blk-mq)
      │   # Admin 队列深度通常为 64 (NVME_AQ_DEPTH)
      │   # 用于 Admin 命令的 blk-mq 请求管理
      │
      ├─ 设置状态: CONNECTING
      │   # 控制器状态机: NEW → CONNECTING
      │   # 表示正在与控制器建立连接
      │
      └─ nvme_init_ctrl_finish
          │  # 完成控制器初始化, 发送 IDENTIFY 命令
          │
          ├─ nvme_init_identify
          │   │  # IDENTIFY 命令序列
          │   │
          │   ├─ Admin IDENTIFY (CNS=1)
          │   │   # 读取控制器能力: NVME_ID_CNS_CTRL
          │   │   # 获取: 型号(MN), 固件版本(FR), 序列号(SN)
          │   │   #       最大 Namespace 数, 可选特性 (CMB/HMB/SGL)
          │   │   #       电源状态 (APST), Sanitize 能力
          │   │
          │   └─ Namespace IDENTIFY (CNS=0)
          │       # 扫描所有 Namespace: NVME_ID_CNS_NS
          │       # 获取: 容量, LBA 格式, 端到端保护
          │       #       最优 I/O 大小, Namespace 特性
          │
          ├─ nvme_configure_apst
          │   # 配置 Autonomous Power State Transition (APST)
          │   # 根据空闲时间自动切换电源状态
          │   # 电源状态: PS0(满功耗) → PS4(最低功耗)
          │
          ├─ nvme_dbbuf_dma_alloc
          │   # 分配 Shadow Doorbell Buffer (DMA 内存)
          │   # 减少 MMIO 写操作, 提升提交性能
          │   # 仅当控制器支持时启用
          │
          ├─ nvme_setup_host_mem
          │   # 配置 Host Memory Buffer (HMB)
          │   # 主机端内存供控制器使用 (存放逻辑到物理地址映射表)
          │   # 减少控制器端 DRAM 需求
          │
          ├─ nvme_setup_io_queues
          │   │  # 创建 I/O Submission/Completion Queue
          │   │
          │   ├─ nvme_set_queue_count
          │   │   # 协商 I/O 队列数量
          │   │   # 发送 Set Features (Number of Queues)
          │   │   # 返回: 控制器支持的 SQ/CQ 数量
          │   │   # 取 min(驱动请求, 控制器支持, CPU 核心数)
          │   │
          │   ├─ nvme_setup_irqs
          │   │   # 重新分配 MSI/MSI-X 中断向量
          │   │   # 向量数 = I/O 队列数 + 1 (Admin)
          │   │   # 配置中断亲和性 (IRQ affinity)
          │   │
          │   ├─ nvme_create_io_queues
          │   │   │  # 为每个 I/O 队列对 (SQ+CQ) 执行:
          │   │   │
          │   │   ├─ nvme_alloc_queue
          │   │   │   # 分配 SQ/CQ 的 DMA 一致内存
          │   │   │   # 设置 SQ Entry 大小 (io_sqes)
          │   │   │
          │   │   ├─ adapter_alloc_cq
          │   │   │   # Admin 命令: Create I/O Completion Queue
          │   │   │   # 参数: CQID, CQ 大小, 中断向量号
          │   │   │   # 控制器返回: CQ 创建成功
          │   │   │
          │   │   ├─ adapter_alloc_sq
          │   │   │   # Admin 命令: Create I/O Submission Queue
          │   │   │   # 参数: SQID, CQID, SQ 大小, 队列优先级
          │   │   │   # 控制器返回: SQ 创建成功
          │   │   │
          │   │   ├─ nvme_init_queue
          │   │   │   # 初始化队列 Doorbell 指针
          │   │   │   # 写 SQyTDBL/CQyHDBL 寄存器
          │   │   │
          │   │   └─ queue_request_irq
          │   │       # 注册 I/O CQ 中断处理函数
          │   │       # 每个 I/O 队列独立的中断向量
          │   │
          │   └─ nvme_alloc_io_tag_set
          │       # 分配 I/O tag set (blk-mq)
          │       # I/O 队列深度 = q_depth (通常 1024)
          │       # 设置 blk_mq_ops: nvme_mq_ops
          │       #   .queue_rq = nvme_queue_rq
          │       #   .commit_rqs = nvme_commit_rqs
          │       #   .poll = nvme_poll
          │       #   .timeout = nvme_timeout
          │
          ├─ nvme_dbbuf_set
          │   # 配置 Shadow Doorbell Buffer
          │   # 发送 Set Features (Host Controlled Thermal)
          │   # 控制器通过写入 Shadow Doorbell 通知主机更新
          │
          ├─ 设置状态: LIVE
          │   # 控制器状态机: CONNECTING → LIVE
          │   # 表示控制器已就绪, 可以处理 I/O
          │
          └─ nvme_start_ctrl
              │  # 启动控制器运行
              │
              ├─ nvme_enable_aen
              │   # 启用 Async Event Notification
              │   # 发送 Set Features (Async Event Configuration)
              │   # 订阅: 温度/健康/Namespace 变更等事件
              │
              └─ nvme_queue_scan
                  # 扫描 Namespace, 创建块设备节点
                  # 为每个 Namespace 创建 /dev/nvmeXnY
                  # 注册到 block layer (blk-mq)
                  #
                  # [设备就绪] 可以接收 I/O 请求
```

### 6.3 I/O 提交与完成流程

```text
# NVMe I/O 提交与完成流程 - 函数调用栈
#
# 包含四个路径: 提交路径 / 完成路径(中断) / 请求完成回调 / 轮询路径
# 数据流: 块层 → SQ(Doorbell) → NVMe控制器 → CQ → 中断 → 完成回调

# ═══════════════════════════════════════════════════════════════
# 提交路径 (Submission Path)
# ═══════════════════════════════════════════════════════════════

blk-mq queue_rq (nvme_queue_rq)
  │  # 块层多队列框架调用, 提交一个 I/O 请求
  │  # 参数: struct blk_mq_hw_ctx *hctx, struct blk_mq_queue_data *bd
  │
  ├─ nvme_check_ready
  │   │  # 检查控制器是否处于 LIVE 状态
  │   │  # 如果控制器正在 RESETTING/DELETING, 返回 BLK_STS_RESOURCE
  │   │  # 使 blk-mq 稍后重试请求
  │   │
  │   └─ [LIVE] 继续处理
  │
  ├─ nvme_prep_rq
  │   # 预处理请求: 分配 nvme_iod (I/O 描述符)
  │   # 设置 nvme_iod->nvme_req 的 rq, qid 字段
  │
  ├─ nvme_setup_cmd
  │   # 根据请求类型构造 NVMe 命令
  │   #   READ → nvme_cmd_read (opcode=0x02)
  │   #   WRITE → nvme_cmd_write (opcode=0x01)
  │   #   FLUSH → nvme_cmd_flush (opcode=0x00)
  │   #   DISCARD → nvme_cmd_dsm (opcode=0x09)
  │   # 设置命令的 NSID, SLBA, NLB 等字段
  │
  ├─ nvme_map_data
  │   │  # 映射数据缓冲区, 建立 DMA 映射
  │   │  # 根据控制器支持的特性选择传输方式:
  │   │
  │   ├─ [单段] nvme_pci_setup_data_simple
  │   │   # 单个物理段 (single segment)
  │   │   # 直接使用 PRP1/PRP2 描述单段数据
  │   │   # 适用于小型 I/O 或物理连续内存
  │   │
  │   ├─ [PRP] nvme_pci_setup_data_prp
  │   │   # Physical Region Page 模式
  │   │   # PRP1: 第一个物理页 (或偏移)
  │   │   # PRP2: PRP List 指针 (页对齐的 PRP 条目数组)
  │   │   # 每个 PRP 条目指向一个 4KB 物理页
  │   │   # 仅支持页对齐的传输
  │   │
  │   └─ [SGL] nvme_pci_setup_data_sgl
  │       # Scatter-Gather List 模式
  │       # 使用 SGL 描述符描述任意大小的内存段
  │       # 支持非页对齐的传输, 更灵活
  │       # 仅当 NVME_CTRL_SGL_SUPPORTED 时可用
  │
  ├─ nvme_map_metadata
  │   │  # 映射元数据 (Metadata) 缓冲区
  │   │  # 用于端到端数据保护 (T10 PI/DIF)
  │   │  # 每个 sector 附加 8 字节保护信息
  │   │  # 如未启用 PI, 则跳过
  │   │
  │   └─ [完成] 所有数据映射完成
  │
  ├─ nvme_sq_copy_cmd
  │   # memcpy 将构造好的 NVMe 命令拷贝到 SQ 条目
  │   # 拷贝到 SQ 的当前 tail 位置
  │   # 命令大小: 64 字节 (nvme_command)
  │
  └─ nvme_write_sq_db
      # 写 Submission Queue Tail Doorbell
      # writel(sq->sq_tail, sq->q_db)
      # 控制器收到 DB 更新后开始处理命令
      #
      # [返回 BLK_STS_OK] 请求已提交, 等待完成

# ═══════════════════════════════════════════════════════════════
# 完成路径 (Completion Path) - 中断处理
# ═══════════════════════════════════════════════════════════════

nvme_irq
  │  # 中断处理函数入口
  │  # 每个 I/O CQ 拥有独立的中断向量
  │  # 调用 nvme_poll_cq 处理该队列的完成条目
  │
  └─ nvme_poll_cq
      │  # 轮询 Completion Queue, 收割完成条目
      │  # 参数: struct nvme_queue *nvmeq, int *start, int *done
      │
      └─ [循环] nvme_cqe_pending
          │  # 检查 CQ 是否有新的完成条目
          │  # 通过比较 CQ entry 的 phase bit 与 cq_phase 判断
          │  # 新条目: cqe.status.p != cq_phase → 已更新
          │
          ├─ [Yes: 有新条目]
          │   │
          │   ├─ dma_rmb
          │   │   # 读内存屏障, 确保读取到完整的 CQE
          │   │   # 防止 CPU 乱序读取导致读到不完整的数据
          │   │
          │   ├─ nvme_handle_cqe
          │   │   │  # 处理单个 Completion Queue Entry
          │   │   │  # 从 CQE 中提取: command_id, status, result
          │   │   │
          │   │   ├─ nvme_find_rq
          │   │   │   # 通过 command_id 查找对应的 request
          │   │   │   # command_id 由 blk-mq tag 分配
          │   │   │   # 查找: blk_mq_tag_to_rq(tagset, command_id)
          │   │   │
          │   │   └─ nvme_try_complete_req
          │   │       # 尝试完成请求
          │   │       # 检查 status: 成功/失败/重试
          │   │       # 失败时调用 nvme_error_status 解析错误
          │   │       # 将请求标记为完成, 进入回调阶段
          │   │
          │   └─ nvme_update_cq_head
          │       # 更新 CQ head 指针
          │       # 翻转 cq_phase 当 head 回绕到 0
          │       # 继续循环检查下一个 CQE
          │
          └─ [No: 无新条目]
              │
              └─ nvme_ring_cq_doorbell
                  # 写 Completion Queue Head Doorbell
                  # writel(cq_head, cq_h_db)
                  # 通知控制器: 已处理完这些 CQE, 可以释放
                  #
                  # 进入批量完成回调

# ═══════════════════════════════════════════════════════════════
# 请求完成回调 (Request Completion Callback)
# ═══════════════════════════════════════════════════════════════

nvme_pci_complete_batch
  │  # 批量完成请求处理
  │  # 遍历已完成请求的链表
  │
  ├─ nvme_pci_unmap_rq
  │   │  # 解除 DMA 映射
  │   │
  │   ├─ nvme_unmap_data
  │   │   # 解除数据缓冲区的 DMA 映射
  │   │   # dma_unmap_sg / dma_unmap_single
  │   │   # 根据 PRP/SGL 模式释放对应的映射
  │   │
  │   └─ nvme_unmap_metadata
  │       # 解除元数据缓冲区的 DMA 映射
  │       # 仅当 PI 启用时有效
  │
  └─ nvme_complete_rq
      # 调用 blk_mq_complete_request
      # 通知块层: 请求已完成
      # 块层负责: 释放 tag, 调用 bio end_io, 唤醒等待者

# ═══════════════════════════════════════════════════════════════
# 轮询路径 (Polling Path)
# ═══════════════════════════════════════════════════════════════

nvme_poll
  │  # blk-mq 轮询接口, 用于低延迟场景
  │  # 用户态通过 io_uring IORING_SETUP_IOPOLL 触发
  │  # 不依赖中断, 主动轮询 CQ
  │
  ├─ spin_lock(&nvmeq->cq_poll_lock)
  │   # 获取 CQ 轮询锁, 保护并发轮询
  │
  └─ nvme_poll_cq
      # 与中断路径共用同一个 CQ 收割函数
      # 处理 CQE, 完成请求
      # 释放 cq_poll_lock
```

### 6.4 超时处理与复位流程

```text
# NVMe 超时处理与复位流程 - 函数调用栈
#
# 当 I/O 请求超时时触发, 依次检查设备状态决定处理策略
# 核心路径: nvme_timeout → nvme_reset_work → 重新初始化

nvme_timeout
  │  # blk-mq 超时回调, 当 I/O 请求超时 (默认 30s) 时触发
  │  # 参数: struct request *rq
  │
  ├─ 读取 CSTS 寄存器
  │   # 读取 Controller Status 寄存器
  │   # 检查 CSTS.CFS (Controller Fatal Status)
  │   #  CFS=1: 控制器发生致命错误
  │   # 检查 CSTS.SHST (Shutdown Status)
  │
  ├─ pci_dev_is_disconnected
  │   │  # 检查 PCI 设备是否已断开连接
  │   │  # 读取 PCI 配置空间 Vendor ID
  │   │  #  0xFFFF: 设备已拔出/链路断开
  │   │
  │   ├─ [是: 设备已断开]
  │   │   │
  │   │   └─ 设置 DELETING 状态
  │   │       # 标记控制器状态为 NVME_CTRL_DELETING
  │   │       # 调用 nvme_dev_disable 关闭设备
  │   │       # 返回 BLK_EH_DONE
  │   │
  │   └─ [否: 设备仍存在, 继续]
  │
  ├─ nvme_should_reset
  │   │  # 检查 CSTS.CFS 位
  │   │
  │   ├─ [CSTS.CFS=1: 控制器故障]
  │   │   │
  │   │   └─ nvme_warn_reset
  │   │       # 打印警告日志: "controller is down; will reset"
  │   │       # 设置控制器状态为 RESETTING
  │   │       │
  │   │       └─ nvme_dev_disable
  │   │           │  # 关闭控制器: CC.EN=0
  │   │           │  # 释放中断向量
  │   │           │
  │   │           └─ nvme_try_sched_reset
  │   │               # 调度复位工作队列
  │   │               # 调用 nvme_reset_work
  │   │
  │   └─ [正常: 无 CFS]
  │
  └─ [CSTS 正常] nvme_poll_irqdisable
      │  # 禁用中断后轮询 CQ
      │  # 检查超时请求是否已完成
      │  # (可能只是中断延迟)
      │
      ├─ [已完成]
      │   # 返回 BLK_EH_DONE
      │   # 请求已正常完成, 无需处理
      │
      └─ [未完成: 需要进一步检查]
          │
          └─ nvme_ctrl_state (检查控制器状态)
              │
              ├─ [CONNECTING] → nvme_dev_disable
              │   # 正在连接中, 直接关闭设备
              │
              ├─ [DELETING] → nvme_dev_disable
              │   # 正在删除中, 直接关闭设备
              │
              ├─ [RESETTING] → BLK_EH_RESET_TIMER
              │   # 正在复位中, 延长超时定时器
              │   # 等待复位完成
              │
              └─ [LIVE] → 发送 ABORT 命令
                  │  # 控制器正常运行, 尝试 Abort 超时命令
                  │
                  ├─ [abort_limit > 0]
                  │   │
                  │   ├─ 构造 Admin Abort 命令
                  │   │   # 构造 Abort 命令 (opcode=0x08)
                  │   │   # 参数: Command Identifier (SQID<<16 | CMDID)
                  │   │   # 发送到 Admin SQ
                  │   │
                  │   └─ abort_limit--
                  │       # 防止无限 Abort 重试
                  │       # 返回 BLK_EH_RESET_TIMER
                  │
                  └─ [abort_limit == 0]
                      # Abort 次数用完, 触发复位
                      # 设置 RESETTING 状态
                      └─ nvme_dev_disable
                          └─ nvme_try_sched_reset
                              └─ nvme_reset_work

# ═══════════════════════════════════════════════════════════════
# nvme_reset_work 详细流程
# ═══════════════════════════════════════════════════════════════

nvme_reset_work
  │  # 异步复位工作队列项
  │  # 当控制器需要复位时调度执行
  │
  ├─ nvme_dev_disable
  │   # 关闭设备, 确保控制器处于干净状态
  │   # CC.EN=0, 释放所有中断向量
  │   # 等待 CSTS.RDY=0
  │
  ├─ nvme_pci_enable
  │   # 重新使能 PCI 设备
  │   # pci_enable_device, pci_set_master
  │   # 重新分配 MSI/MSI-X 中断向量
  │
  ├─ 设置状态: CONNECTING
  │   # 控制器状态机: RESETTING → CONNECTING
  │   # 进入重新连接阶段
  │
  ├─ nvme_init_ctrl_finish
  │   # 重新 IDENTIFY 控制器
  │   # 发送 Admin IDENTIFY 命令
  │   # 重新扫描 Namespace
  │   # 重新配置 APST/HMB
  │
  ├─ nvme_setup_io_queues
  │   # 重建 I/O 队列
  │   # 重新协商队列数量
  │   # 重新分配中断向量
  │   # 重新创建 SQ/CQ
  │   # 重新分配 I/O tag set
  │
  ├─ 设置状态: LIVE
  │   # 控制器状态机: CONNECTING → LIVE
  │   # 控制器已恢复, 可以处理 I/O
  │
  └─ nvme_start_ctrl
      # 启动控制器
      # 重新启用 AEN
      # 重新扫描 Namespace
      # 恢复 I/O 处理
```

### 6.5 设备移除流程

```text
# NVMe 设备移除流程 - 函数调用栈
#
# 当 PCI 设备被拔出或驱动卸载时, nvme_remove 被调用
# 安全关闭控制器, 释放所有资源

nvme_remove
  │  # PCI 设备移除回调, 设备拔出或驱动卸载时触发
  │
  ├─ 设置 DELETING 状态
  │   # 设置控制器状态: NVME_CTRL_DELETING
  │   # 阻止任何新的 I/O 请求
  │   # 阻止新的 Admin 命令
  │
  ├─ pci_device_is_present
  │   │  # 检查 PCI 设备是否仍然存在
  │   │  # 读取 PCI 配置空间, 检查 Vendor ID
  │   │
  │   ├─ [否: 设备已不存在]
  │   │   # 设置 DEAD 状态
  │   │   # 跳过后续硬件操作 (设备已不可达)
  │   │   # 直接进入资源清理阶段
  │   │
  │   └─ [是: 设备仍存在]
  │
  ├─ flush reset_work
  │   # 等待正在进行的复位工作完成
  │   # 确保没有并发的 reset_work 在执行
  │
  ├─ nvme_stop_ctrl
  │   # 优雅地停止控制器
  │   # 设置 CC.SHN (Shutdown Notification)
  │   # 通知控制器: 即将关闭
  │   # 等待 CSTS.SHST 确认 (可选)
  │
  ├─ nvme_remove_namespaces
  │   # 移除所有 Namespace
  │   # 删除 /dev/nvmeXnY 块设备节点
  │   # 从 block layer 注销
  │   # 取消所有正在进行的 I/O
  │
  └─ nvme_dev_disable
      │  # 关闭和禁用 NVMe 设备
      │
      ├─ nvme_start_freeze
      │   # 冻结 I/O 队列
      │   # 阻止新的 I/O 请求进入队列
      │   # 等待所有正在进行的 I/O 完成
      │
      ├─ nvme_quiesce_io_queues
      │   # 静默所有 I/O 队列
      │   # 停止处理 I/O 队列的 CQ
      │   # 确保没有新的 I/O 被处理
      │
      ├─ nvme_delete_io_queues
      │   │  # 删除所有 I/O SQ/CQ
      │   │  # 对每个 I/O 队列发送:
      │   │
      │   ├─ Delete I/O SQ (Admin opcode=0x00)
      │   │   # 删除 I/O Submission Queue
      │   │   # 控制器释放 SQ 资源
      │   │
      │   └─ Delete I/O CQ (Admin opcode=0x04)
      │       # 删除 I/O Completion Queue
      │       # 控制器释放 CQ 资源
      │
      ├─ nvme_disable_ctrl
      │   # 禁用控制器: 写 CC.EN=0
      │   # 等待 CSTS.RDY=0
      │   # 控制器停止处理所有命令
      │
      ├─ nvme_poll_irqdisable
      │   # 禁用中断后最后一次轮询 Admin CQ
      │   # 收割可能残留的 Admin 完成条目
      │   # 避免遗漏 Delete Queue 的完成
      │
      ├─ nvme_suspend_io_queues
      │   # 挂起所有 I/O 队列
      │   # 释放每个队列的中断
      │   # 释放 I/O tag set
      │
      ├─ pci_free_irq_vectors
      │   # 释放所有 MSI/MSI-X 中断向量
      │   # 包括 Admin 和 I/O 队列的中断
      │
      ├─ pci_disable_device
      │   # 禁用 PCI 设备
      │   # 释放 PCI 资源 (BAR, 配置空间)
      │
      ├─ nvme_reap_pending_cqes
      │   # 收集残留的 CQE
      │   # 处理可能遗漏的完成条目
      │   # 确保所有请求都被完成或取消
      │
      ├─ nvme_cancel_tagset
      │   # 取消所有未完成的请求
      │   # 遍历 Admin tag set 和 I/O tag set
      │   # 将所有未完成的请求标记为失败
      │   # 请求返回 -ENODEV 给上层
      │
      ├─ nvme_free_host_mem
      │   # 释放 Host Memory Buffer (HMB)
      │   # 如果启用了 HMB, 释放 DMA 内存
      │   # 通知控制器 HMB 已释放
      │
      ├─ nvme_dev_remove_admin
      │   # 移除 Admin 队列
      │   # 释放 Admin tag set
      │   # 停止 Admin 队列
      │
      ├─ nvme_dbbuf_dma_free
      │   # 释放 Shadow Doorbell Buffer
      │   # 如果启用了 Shadow Doorbell, 释放 DMA 内存
      │
      ├─ nvme_free_queues
      │   # 释放所有队列的内存
      │   # 释放 SQ/CQ 的 DMA 缓冲区
      │   # 释放 nvme_queue 结构体
      │
      ├─ mempool_destroy
      │   # 销毁内存池
      │   # 释放 nvme_iod 和 nvme_command 的内存池
      │
      ├─ nvme_release_descriptor_pools
      │   # 释放 DMA 描述符池
      │   # 每个 NUMA 节点的 descriptor_pools
      │   # 包括 PRP list 和 SGL 描述符池
      │
      ├─ nvme_dev_unmap
      │   # 解除 BAR 映射
      │   # iounmap(dev->bar)
      │   # 释放 pci_release_mem_regions
      │
      └─ nvme_uninit_ctrl
          # 反初始化控制器
          # 释放 nvme_ctrl 资源
          # 释放 nvme_dev 结构体
          #
          # [完成] 设备完全移除
```

---

## 7. 文件清单

| 文件                                 | 说明                                       |
| ------------------------------------ | ------------------------------------------ |
| `drivers/nvme/host/pci.c`          | PCIe 传输层主文件 (4307 行)                |
| `drivers/nvme/host/nvme.h`         | 通用头文件，控制器/命名空间/请求结构体定义 |
| `drivers/nvme/host/core.c`         | 核心协议层，IDENTIFY/状态机/Keep Alive 等  |
| `drivers/nvme/host/ioctl.c`        | 用户态 ioctl/uring_cmd 接口                |
| `drivers/nvme/host/sysfs.c`        | sysfs 属性接口                             |
| `drivers/nvme/host/multipath.c`    | 多路径支持 (ANA)                           |
| `drivers/nvme/host/fabrics.c`      | Fabrics 传输层共享代码                     |
| `drivers/nvme/host/pr.c`           | 持久性预留 (Persistent Reservation)        |
| `drivers/nvme/host/zns.c`          | Zoned Namespace 支持                       |
| `drivers/nvme/host/hwmon.c`        | 硬件监控接口                               |
| `drivers/nvme/host/trace.c`        | ftrace 追踪点                              |
| `drivers/nvme/host/auth.c`         | DHCHAP 认证 (主机端)                       |
| `drivers/nvme/host/constants.c`    | NVMe 状态码/操作码字符串                   |
| `drivers/nvme/host/fault_inject.c` | 故障注入调试                               |
| `drivers/nvme/host/trace.h`        | 追踪事件头文件                             |
| `drivers/nvme/host/fabrics.h`      | Fabrics 传输头文件                         |
