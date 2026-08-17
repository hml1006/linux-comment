# 块层 — NVMe驱动实例分析 (Part V)

> 本文档拆分自 [block_layer_analysis.md](block_layer_analysis.md) Part V，涵盖NVMe驱动块设备注册与移除流程、读写I/O流程

## Part V: 驱动实例分析

## 20. NVMe 驱动：块设备注册与移除流程

NVMe（Non-Volatile Memory Express）是当前最主流的 SSD 接口协议。Linux NVMe 驱动位于 `drivers/nvme/` 目录，分为 `host/`（主机端）、`target/`（目标端）、`common/`（公共代码）三个子目录。本章以 PCIe NVMe 驱动为主线，分析块设备注册与移除的完整流程。

### 20.1 关键数据结构

#### 20.1.1 nvme_ctrl — NVMe 控制器

文件：`drivers/nvme/host/nvme.h` L334-L460

```c
struct nvme_ctrl {
    bool comp_seen;
    bool identified;                         // 是否已完成 Identify 初始化
    enum nvme_ctrl_state state;              // 控制器状态: LIVE/RESETTING/CONNECTING/DELETING/DEAD
    spinlock_t lock;
    struct mutex scan_lock;                  // 保护扫描过程的互斥锁
    const struct nvme_ctrl_ops *ops;         // 传输层操作函数集（PCI/TCP/RDMA/FC）
    struct request_queue *admin_q;           // Admin 命令队列
    struct request_queue *connect_q;         // 连接命令队列（Fabrics）
    struct request_queue *fabrics_q;         // Fabrics 命令队列
    struct device *dev;                      // 指向 PCI 设备的 device
    int instance;                            // 控制器实例编号（如 nvme0）
    int numa_node;                           // NUMA 节点
    struct blk_mq_tag_set *tagset;           // IO 队列的 tag set
    struct blk_mq_tag_set *admin_tagset;     // Admin 队列的 tag set
    struct list_head namespaces;             // 该控制器上的所有 namespace 链表
    struct mutex namespaces_lock;            // namespaces 链表保护锁
    struct srcu_struct srcu;                 // 保护 namespaces 遍历的 SRCU
    struct device ctrl_device;               // 控制器 device（/sys/class/nvme/）
    struct device *device;                   // 字符设备 device（/dev/nvme0）
    struct cdev cdev;                        // 字符设备结构体
    struct work_struct reset_work;           // 复位工作项
    struct work_struct delete_work;          // 删除工作项
    struct nvme_subsystem *subsys;           // 所属子系统
    struct opal_dev *opal_dev;               // TCG Opal 自加密设备
    u16 cntlid;                              // 控制器 ID
    u32 ctrl_config;                         // CC 寄存器配置
    u32 queue_count;                         // 已创建的队列数
    u64 cap;                                 // CAP 寄存器
    u32 max_hw_sectors;                      // 最大硬件扇区数
    u32 max_segments;                        // 最大段数
    u32 max_namespaces;                      // 最大 namespace 数
    u8 vwc;                                  // 易失性写缓存
    u32 vs;                                  // 版本号
    struct work_struct scan_work;            // 扫描 namespace 的工作项
    struct work_struct async_event_work;     // 异步事件处理
    struct delayed_work ka_work;             // Keep-Alive 定时器
    unsigned long events;                    // AER 事件位图
    struct nvme_fault_inject fault_inject;   // 故障注入
};
```

**控制器状态机**（`enum nvme_ctrl_state`）：

```
NVME_CTRL_NEW ──► NVME_CTRL_CONNECTING ──► NVME_CTRL_LIVE
                       │       ▲                    │
                       ▼       │                    ▼
                 NVME_CTRL_RESETTING          NVME_CTRL_DELETING
                                                  │
                                                  ▼
                                          NVME_CTRL_DELETING_NOIO
                                                  │
                                                  ▼
                                           NVME_CTRL_DEAD
```

#### 18.1.2 nvme_ns_head — Namespace 头部

文件：`drivers/nvme/host/nvme.h` L526-L575

```c
struct nvme_ns_head {
    struct list_head list;                   // 链接到 subsystem 的 nsheads 链表
    struct srcu_struct srcu;                 // SRCU 保护
    struct nvme_subsystem *subsys;           // 所属子系统
    struct nvme_ns_ids ids;                  // 唯一标识符（eui64/nguid/uuid/csi）
    u8 lba_shift;                            // LBA 偏移（扇区大小 = 2^lba_shift）
    u16 ms;                                  // 元数据大小
    u16 pi_size;                             // 保护信息大小
    u8 pi_type;                              // 保护信息类型
    u8 guard_type;                           // 保护类型
    struct list_head entry;                  // 链接到 subsystem 的 nsheads
    struct kref ref;                         // 引用计数
    bool shared;                             // 是否共享 namespace
    u64 nuse;                                // 已使用的 LBA 数
    unsigned ns_id;                          // Namespace ID
    int instance;                            // 实例编号
    struct gendisk *disk;                    // 多路径聚合盘（仅多路径模式）
    unsigned long features;                  // 特性位图
    struct cdev cdev;                        // 字符设备（/dev/ngXnY）
    struct device cdev_device;               // 字符设备 device
};
```

#### 18.1.3 nvme_ns — Namespace 实例

文件：`drivers/nvme/host/nvme.h` L585-L610

```c
struct nvme_ns {
    struct list_head list;                   // 链接到 ctrl->namespaces 链表
    struct nvme_ctrl *ctrl;                  // 所属控制器
    struct request_queue *queue;             // 块层请求队列
    struct gendisk *disk;                    // 通用磁盘（gendisk）结构体
    struct list_head siblings;               // 链接到 ns_head->list（同一 ns 的多个路径）
    struct kref kref;                        // 引用计数
    struct nvme_ns_head *head;              // 指向 namespace 头部
    unsigned long flags;                     // 标志位：
#define NVME_NS_REMOVING           0         //   正在移除
#define NVME_NS_ANA_PENDING        2         //   ANA 状态待更新
#define NVME_NS_FORCE_RO           3         //   强制只读
#define NVME_NS_READY              4         //   namespace 就绪
    struct cdev cdev;                        // 字符设备
    struct device cdev_device;               // 字符设备 device
    struct nvme_fault_inject fault_inject;   // 故障注入
};
```

#### 20.1.4 nvme_dev — PCIe 设备（含控制器）

文件：`drivers/nvme/host/pci.c` L294-L368

```c
struct nvme_dev {
    struct nvme_queue *queues;               // 队列数组（queues[0]=admin queue）
    struct blk_mq_tag_set tagset;            // IO 队列的 tag set
    struct blk_mq_tag_set admin_tagset;      // Admin 队列的 tag set
    u32 __iomem *dbs;                        // Doorbell 寄存器基址
    struct device *dev;                      // 指向 PCI 设备
    unsigned online_queues;                  // 已上线的队列数
    unsigned max_qid;                        // 最大队列 ID
    unsigned io_queues[HCTX_MAX_TYPES];      // 各类型 IO 队列数
    unsigned int num_vecs;                   // 中断向量数
    u32 q_depth;                             // IO 队列深度
    int io_sqes;                             // IO SQ Entry 大小
    u32 db_stride;                           // Doorbell 步长
    void __iomem *bar;                       // MMIO BAR 地址
    struct mutex shutdown_lock;              // 关断锁
    struct nvme_ctrl ctrl;                   // 嵌入的控制器结构体（通过 container_of 获取）
    u32 last_ps;                             // 上一次电源状态
    mempool_t *dmavec_mempool;               // DMA vec 内存池
    // ... 门铃缓冲区、主机内存缓冲区等
};
```

#### 20.1.5 nvme_queue — 硬件队列

文件：`drivers/nvme/host/pci.c` L365-L414

```c
struct nvme_queue {
    struct nvme_dev *dev;                    // 所属设备
    spinlock_t sq_lock;                      // 提交队列自旋锁
    void *sq_cmds;                           // 提交队列命令缓冲区
    spinlock_t cq_poll_lock;                 // 完成队列轮询锁
    struct nvme_completion *cqes;            // 完成队列条目数组（DMA 一致性内存）
    dma_addr_t sq_dma_addr;                  // 提交队列 DMA 地址
    dma_addr_t cq_dma_addr;                  // 完成队列 DMA 地址
    u32 __iomem *q_db;                       // 队列门铃寄存器地址
    u32 q_depth;                             // 队列深度
    u16 cq_vector;                           // 完成队列中断向量
    u16 sq_tail;                             // 提交队列尾指针
    u16 cq_head;                             // 完成队列头指针
    u16 qid;                                 // 队列 ID
    u8 cq_phase;                             // 完成队列阶段位（Phase Tag）
    u8 sqes;                                 // 提交队列条目大小（2^sqes 字节）
    unsigned long flags;                     // 标志位：
#define NVMEQ_ENABLED              0         //   队列已启用
#define NVMEQ_SQ_CMB               1         //   提交队列在 CMB 中
#define NVMEQ_POLLED               3         //   轮询队列
};
```

#### 18.1.6 nvme_ns_info — Namespace 扫描信息

文件：`drivers/nvme/host/core.c` L40-L52

```c
struct nvme_ns_info {
    struct nvme_ns_ids ids;                  // 唯一标识符
    u32 nsid;                                // Namespace ID
    __le32 anagrpid;                         // ANA 组 ID
    u8 pi_offset;                            // 保护信息偏移
    u16 endgid;                              // 耐久性组 ID
    u64 runs;                                // 可恢复单元数
    bool is_shared;                          // 是否共享
    bool is_readonly;                        // 是否只读
    bool is_ready;                           // 是否就绪
    bool is_removed;                         // 是否已移除
    bool is_rotational;                      // 是否为旋转介质
    bool no_vwc;                             // 无易失性写缓存
};
```

---

### 20.2 注册流程（Probe）

当 NVMe PCIe 设备被 PCI 子系统枚举到时，触发 `nvme_probe()`。整个注册流程分为 **控制器初始化** 和 **Namespace 扫描** 两大阶段。

#### 20.2.1 注册流程总览

```
nvme_probe()                                      [pci.c]
  │
  ├─ 1. nvme_pci_alloc_dev()                      分配 nvme_dev + 初始化 controller
  │
  ├─ 2. nvme_add_ctrl()                           注册字符设备 /dev/nvmeX
  │     └─ cdev_init(&ctrl->cdev, &nvme_dev_fops) 设置字符设备 fops
  │     └─ cdev_device_add()                      添加 cdev + device 到内核
  │
  ├─ 3. nvme_dev_map()                            MMIO BAR 地址映射
  │
  ├─ 4. nvme_pci_alloc_iod_mempool()             DMA 向量内存池分配
  │
  ├─ 5. nvme_pci_enable()                         PCI 设备使能（pci_enable_device + 中断）
  │
  ├─ 6. nvme_alloc_admin_tag_set()               分配 Admin Tag Set
  │     └─ blk_mq_alloc_tag_set()                分配 blk-mq tag set（bitmap 管理）
  │
  ├─ 7. nvme_change_ctrl_state(CONNECTING)      状态 → CONNECTING
  │
  ├─ 8. nvme_init_ctrl_finish()                  控制器初始化完成
  │     ├─ 读取 VS 寄存器（版本号）
  │     ├─ 设置 sqsize（MQES）
  │     ├─ 判断 subsystem 模式（NSSRC）
  │     ├─ nvme_init_identify()                  发送 Identify Controller 命令
  │     ├─ nvme_configure_apst()                 配置自动电源状态转换
  │     ├─ nvme_configure_timestamp()            配置时间戳
  │     ├─ nvme_configure_host_options()         配置主机选项
  │     ├─ nvme_configure_opal()                 配置 Opal 自加密
  │     └─ nvme_start_keep_alive()              启动 Keep-Alive 定时器
  │
  ├─ 9. nvme_setup_io_queues()                   创建 IO 队列
  │     ├─ 计算 nr_io_queues（CPU 数 + write/poll 队列）
  │     ├─ nvme_set_queue_count()                Set Features (Number of Queues)
  │     ├─ nvme_create_io_queues()              循环创建 IO 队列
  │     │     └─ nvme_alloc_queue()             为每个队列：
  │     │           ├─ 分配 CQ DMA 内存（cqes）
  │     │           ├─ 分配 SQ 命令内存（sq_cmds）
  │     │           ├─ 初始化锁（sq_lock, cq_poll_lock）
  │     │           ├─ 设置 doorbell 地址
  │     │           └─ ctrl->queue_count++
  │     └─ queue_request_irq()                   为每个队列注册中断
  │
  ├─ 10. nvme_alloc_io_tag_set()                分配 IO Tag Set
  │      └─ blk_mq_alloc_tag_set()              分配 IO tag set
  │
  ├─ 11. nvme_change_ctrl_state(LIVE)           状态 → LIVE
  │
  ├─ 12. nvme_start_ctrl()                      启动控制器
  │      ├─ nvme_enable_aen()                   启用异步事件通知
  │      ├─ nvme_queue_scan()                   调度 namespace 扫描
  │      │     └─ queue_work(nvme_wq, &ctrl->scan_work)
  │      └─ nvme_unquiesce_io_queues()          解冻 IO 队列
  │
  └─ 13. flush_work(&ctrl->scan_work)           等待扫描完成
```

#### 18.2.2 Namespace 扫描详细流程

`nvme_start_ctrl()` 触发 `nvme_scan_work()` 工作项，该函数是 namespace 发现的核心。

```
nvme_scan_work()                                  [core.c]
  │
  ├─ 检查条件：state == LIVE && ctrl->tagset != NULL
  │
  ├─ nvme_init_non_mdts_limits()                 读取非 MDTS 限制
  │
  ├─ 处理 AER NS_CHANGED 事件
  │     └─ nvme_clear_changed_ns_log()           清除变更日志
  │
  ├─ 扫描 namespace（两种方式）：
  │   │
  │   ├─ 方式 A：nvme_scan_ns_list()             使用 Identify NS List
  │   │     └─ 发送 Identify 命令获取 NSID 列表
  │   │     └─ 对每个 NSID 调用 nvme_scan_ns()
  │   │
  │   └─ 方式 B：nvme_scan_ns_sequential()       顺序扫描（兜底）
  │         ├─ nvme_identify_ctrl()              获取 nn（最大 namespace 数）
  │         └─ for (i = 1; i <= nn; i++)        循环扫描
  │               └─ nvme_scan_ns(ctrl, i)
  │
  └─ 检查是否需要重新扫描（防止遗漏 AEN）
```

**单个 Namespace 扫描**（`nvme_scan_ns()`）：

```
nvme_scan_ns(ctrl, nsid)                          [core.c]
  │
  ├─ nvme_identify_ns_descs()                    获取 NS 描述符（UUID/EUI64/NGUID）
  │
  ├─ 获取 NS 基本信息（尝试两种方式）：
  │   ├─ nvme_ns_info_from_id_cs_indep()         Command Set Independent Identify
  │   │     └─ 发送 Identify (CNS=NS_CS_INDEP) 命令
  │   │     └─ 提取：anagrpid, is_shared, is_readonly, is_ready,
  │   │             is_rotational, no_vwc, endgid
  │   │
  │   └─ nvme_ns_info_from_identify()           传统 Identify Namespace
  │         └─ nvme_identify_ns()               发送 Identify 命令
  │         └─ 提取：anagrpid, is_shared, is_readonly, ncap
  │         └─ 如果 ncap == 0 → is_removed = true
  │
  ├─ 如果 is_removed → nvme_ns_remove_by_nsid() 移除已删除的 ns
  │
  ├─ 如果 !is_ready → return（等待 AEN 通知）
  │
  ├─ nvme_find_get_ns(ctrl, nsid)               查找是否已存在
  │   │
  │   ├─ 已存在 → nvme_validate_ns(ns, &info)   验证并更新
  │   │     ├─ 检查 ID 是否变化
  │   │     └─ nvme_update_ns_info()            更新 NS 信息
  │   │
  │   └─ 不存在 → nvme_alloc_ns(ctrl, &info)    创建新的 namespace
  │
  └─ nvme_put_ns(ns)                            释放引用
```

**创建新 Namespace**（`nvme_alloc_ns()`）：

```
nvme_alloc_ns(ctrl, info)                           [core.c]
  │
  ├─ 1. kzalloc_node(sizeof(*ns))                 分配 nvme_ns 结构体
  │
  ├─ 2. blk_mq_alloc_disk(ctrl->tagset, &lim, ns) 分配 gendisk + request_queue
  │     └─ 块层接口：创建磁盘和请求队列
  │     └─ disk->fops = &nvme_bdev_ops            设置块设备操作函数
  │     └─ disk->private_data = ns                反向指针
  │
  ├─ 3. 初始化 ns 字段：disk, queue, ctrl, kref
  │
  ├─ 4. nvme_init_ns_head(ns, info)               初始化 namespace head
  │     └─ 创建或查找 ns_head（共享 namespace 复用）
  │     └─ 将 ns 加入 ns_head->list（siblings）
  │
  ├─ 5. 设置磁盘名称（根据多路径模式）：
  │     ├─ 多路径 nvme_ns_head_multipath: "nvme%dc%dn%d"
  │     │     └─ disk->flags |= GENHD_FL_HIDDEN  隐藏底层盘
  │     ├─ 多路径非聚合: "nvme%dn%d"
  │     └─ 单路径: "nvme%dn%d"
  │
  ├─ 6. nvme_update_ns_info(ns, info)             更新 namespace 块层信息
  │     │
  │     └─ nvme_update_ns_info_block(ns, info)   块设备信息更新
  │           ├─ nvme_identify_ns()               获取 IDENTIFY NS 数据
  │           ├─ 检查 ncap == 0?
  │           ├─ 计算 lbaf（LBA 格式）
  │           ├─ 获取 ZNS 信息（如果是 Zoned 设备）
  │           ├─ queue_limits_start_update()      开始更新队列限制
  │           ├─ blk_mq_freeze_queue()            冻结队列
  │           ├─ 设置：lba_shift, nuse, capacity
  │           ├─ nvme_set_ctrl_limits()           设置控制器限制
  │           ├─ nvme_configure_metadata()        配置元数据
  │           ├─ nvme_config_discard()            配置 DISCARD 支持
  │           ├─ 设置写缓存/FUA 特性
  │           ├─ nvme_init_integrity()            初始化完整性保护
  │           ├─ 设置写流（write streams）
  │           ├─ queue_limits_commit_update()     提交队列限制
  │           ├─ set_capacity_and_notify()        设置容量 + 发送 uevent
  │           ├─ set_disk_ro()                    设置只读状态
  │           ├─ set_bit(NVME_NS_READY)           标记就绪
  │           └─ blk_mq_unfreeze_queue()          解冻队列
  │
  ├─ 7. nvme_ns_add_to_ctrl_list(ns)              将 ns 加入 ctrl->namespaces
  │
  ├─ 8. device_add_disk(ctrl->device, ns->disk)  向内核注册块设备
  │     └─ 块层接口：创建 /dev/nvmeXnY、sysfs 属性等
  │
  ├─ 9. nvme_add_ns_cdev(ns)                      创建字符设备 /dev/ngXnY
  │
  └─ 10. nvme_mpath_add_disk(ns, anagrpid)        多路径磁盘添加
```

---

### 20.3 移除流程（Remove）

当 NVMe 设备被拔出或驱动卸载时，触发 `nvme_remove()`。移除流程与注册流程基本对称逆向。

#### 20.3.1 移除流程总览

```
nvme_remove(pdev)                                    [pci.c]
  │
  ├─ 1. nvme_change_ctrl_state(DELETING)         状态 → DELETING
  │
  ├─ 2. 检查设备是否仍在位
  │     └─ 如果设备已拔出 → nvme_change_ctrl_state(DEAD)
  │     └─ nvme_dev_disable(dev, true)           硬件关闭
  │
  ├─ 3. flush_work(&ctrl->reset_work)            等待正在进行的复位完成
  │
  ├─ 4. nvme_stop_ctrl(&dev->ctrl)               停止控制器
  │     ├─ nvme_mpath_stop(ctrl)                 停止多路径
  │     ├─ nvme_auth_stop(ctrl)                  停止认证
  │     ├─ nvme_stop_failfast_work(ctrl)         停止快速失败
  │     ├─ flush_work(&ctrl->async_event_work)   等待异步事件处理完成
  │     └─ ctrl->ops->stop_ctrl(ctrl)            传输层停止
  │
  ├─ 5. nvme_remove_namespaces(&dev->ctrl)       移除所有 namespace
  │     │
  │     ├─ nvme_mpath_clear_ctrl_paths(ctrl)     清除多路径
  │     ├─ nvme_unquiesce_io_queues(ctrl)        解冻 IO 队列（防止挂起 IO）
  │     ├─ flush_work(&ctrl->scan_work)          等待扫描完成
  │     ├─ 如果 DEAD → nvme_mark_namespaces_dead() 标记所有盘为 dead
  │     ├─ nvme_change_ctrl_state(DELETING_NOIO) 状态 → DELETING_NOIO
  │     ├─ list_splice_init_rcu()                将 namespaces 链表移出
  │     └─ 对每个 ns 调用 nvme_ns_remove(ns)
  │
  ├─ 6. nvme_dev_disable(dev, true)              硬件禁用
  │     ├─ nvme_quiesce_io_queues()              静默 IO 队列
  │     ├─ nvme_delete_io_queues()               删除 IO 队列（发送 Delete I/O SQ/CQ 命令）
  │     ├─ nvme_disable_ctrl()                   设置 CC.EN=0 禁用控制器
  │     ├─ nvme_suspend_io_queues()              挂起所有 IO 队列
  │     ├─ nvme_suspend_queue(dev, 0)            挂起 Admin 队列
  │     ├─ pci_free_irq_vectors()                释放中断向量
  │     ├─ nvme_reap_pending_cqes()             回收待处理完成队列条目
  │     ├─ nvme_cancel_tagset()                  取消 IO tagset 中所有请求
  │     └─ nvme_cancel_admin_tagset()            取消 Admin tagset 中所有请求
  │
  ├─ 7. nvme_free_host_mem(dev)                  释放主机内存缓冲区
  │
  ├─ 8. nvme_dev_remove_admin(dev)               移除 Admin Tag Set
  │     └─ nvme_remove_admin_tag_set()           释放 Admin tag set
  │
  ├─ 9. nvme_dbbuf_dma_free(dev)                 释放门铃缓冲区 DMA
  │
  ├─ 10. nvme_free_queues(dev, 0)                释放所有队列内存
  │      └─ 从高到低：nvme_free_queue(&dev->queues[i])
  │
  ├─ 11. mempool_destroy(dev->dmavec_mempool)    销毁 DMA 内存池
  │
  ├─ 12. nvme_dev_unmap(dev)                     取消 MMIO 映射
  │
  └─ 13. nvme_uninit_ctrl(&dev->ctrl)            控制器反初始化
       ├─ nvme_stop_keep_alive(ctrl)             停止 Keep-Alive
       ├─ nvme_hwmon_exit(ctrl)                  退出硬件监控
       ├─ cdev_device_del(&ctrl->cdev, ctrl->device) 删除字符设备 /dev/nvmeX
       └─ nvme_put_ctrl(ctrl)                    释放控制器引用
```

#### 20.3.2 单个 Namespace 移除详细流程

```
nvme_ns_remove(ns)                                   [core.c]
  │
  ├─ 1. test_and_set_bit(NVME_NS_REMOVING)        原子标记移除中（防止重复）
  │
  ├─ 2. clear_bit(NVME_NS_READY)                  清除就绪标志
  │
  ├─ 3. set_capacity(ns->disk, 0)                 容量设置为 0
  │
  ├─ 4. nvme_fault_inject_fini()                  清理故障注入
  │
  ├─ 5. synchronize_srcu(&ns->head->srcu)         等待所有读者退出
  │
  ├─ 6. nvme_mpath_clear_current_path(ns)         清除多路径当前路径
  │
  ├─ 7. 从 ns_head->list 中删除（siblings）
  │     └─ 如果是最后一个路径 → 标记 last_path = true
  │
  ├─ 8. nvme_cdev_del(&ns->cdev)                  删除字符设备 /dev/ngXnY
  │
  ├─ 9. nvme_mpath_remove_sysfs_link(ns)          删除多路径 sysfs 链接
  │
  ├─ 10. del_gendisk(ns->disk)                    向块层注销块设备
  │      └─ 块层接口：删除 /dev/nvmeXnY、sysfs 属性
  │
  ├─ 11. 从 ctrl->namespaces 中删除
  │
  ├─ 12. 如果是最后一个路径：
  │      └─ nvme_mpath_remove_disk(ns->head)      移除多路径聚合盘
  │
  └─ 13. nvme_put_ns(ns)                          释放引用（kref_put → 最终释放 ns）
```

---

### 18.4 与块层的交互接口

#### 18.4.1 概述

NVMe 驱动与通用块层的交互贯穿整个 **块设备注册** 和 **移除** 流程。从驱动视角看，核心交互点包括：

```
NVMe 驱动层                         通用块层
─────────────────                   ─────────────────
nvme_probe()
  ├─ nvme_alloc_admin_tag_set()  →  blk_mq_alloc_tag_set()    # 分配 Admin TagSet
  ├─ nvme_setup_io_queues()      →  (PCI 队列创建)
  ├─ nvme_alloc_io_tag_set()     →  blk_mq_alloc_tag_set()    # 分配 IO TagSet
  └─ nvme_start_ctrl()
       └─ nvme_queue_scan()
            └─ nvme_scan_work()
                 └─ nvme_alloc_ns()
                      ├─ blk_mq_alloc_disk()     →  blk_mq_alloc_queue() + __alloc_disk_node()
                      ├─ nvme_update_ns_info()
                      │    └─ queue_limits_start_update()
                      │    └─ blk_mq_freeze_queue()
                      │    └─ queue_limits_commit_update()
                      │    └─ set_capacity_and_notify()
                      │    └─ blk_mq_unfreeze_queue()
                      └─ device_add_disk()        →  add_disk_fwnode() → __add_disk() + add_disk_final()

nvme_remove()
  ├─ nvme_remove_namespaces()
  │    └─ nvme_ns_remove()
  │         ├─ set_capacity(disk, 0)
  │         └─ del_gendisk(ns->disk)    →  __del_gendisk()
  ├─ nvme_dev_disable()
  │    └─ nvme_cancel_tagset() / cancel_admin_tagset()
  └─ nvme_dev_remove_admin()
       └─ nvme_remove_admin_tag_set()
            └─ blk_mq_destroy_queue() + blk_mq_free_tag_set()
```

#### 20.4.2 块设备注册流程（Driver → Block Layer 视角）

##### 20.4.2.1 Tag Set 分配：`blk_mq_alloc_tag_set()`

**调用位置**：`nvme_alloc_admin_tag_set()` / `nvme_alloc_io_tag_set()`

**函数原型**：`block/blk-mq.c`

```c
int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set)
```

**调用栈**：

```
nvme_alloc_admin_tag_set(ctrl, &dev->admin_tagset, &nvme_mq_admin_ops, cmd_size)
  └─ blk_mq_alloc_tag_set(set)
       ├─ blk_mq_update_queue_map(set)        # 建立 CPU→HW Queue 映射
       ├─ blk_mq_alloc_tag_set_tags(set)      # 分配 tags[] 数组（nr_hw_queues 个）
       │    └─ for_each hw_queue:
       │         blk_mq_alloc_rq_maps(set)    # 分配 request 内存池
       │           └─ blk_mq_alloc_map_and_rqs()
       │                ├─ __sbitmap_queue_init_node()   # 初始化 sbitmap 位图
       │                └─ blk_mq_alloc_rqs(set, tags, ...)  # 分配 struct request 数组
       │                    # 对每个 tag:
       │                    #   rqs[i] = kmalloc_node(sizeof(struct request) + cmd_size)
       │                    #   rqs[i]->tag = i  （预设 tag 索引）
       │                    #   rqs[i]->mq_ctx = NULL （后续分配队列时绑定）
       │
       └─ set->tagset_set_flag(BLK_MQ_TAGSET_ALLOCED)  # 标记已分配
```

**关键数据结构**：`struct blk_mq_tag_set`（详见 5.14.5 节）

**NVMe 驱动调用示例**（PCI 驱动）：

```c
// Admin TagSet: 1 个硬件队列，深度 NVME_AQ_MQ_TAG_DEPTH（通常 31）
nvme_alloc_admin_tag_set(&dev->ctrl, &dev->admin_tagset,
    &nvme_mq_admin_ops, sizeof(struct nvme_iod));
// → set->nr_hw_queues = 1
// → set->queue_depth = 31
// → set->cmd_size = sizeof(struct nvme_iod)  (request 尾部紧邻驱动私有数据)
// → set->ops = &nvme_mq_admin_ops             (queue_rq = nvme_queue_rq)

// IO TagSet: 多个硬件队列（通常等于 CPU 数），深度 ctrl->sqsize
nvme_alloc_io_tag_set(&dev->ctrl, &dev->tagset, &nvme_mq_ops,
    nvme_pci_nr_maps(dev), sizeof(struct nvme_iod));
// → set->nr_hw_queues = ctrl->queue_count - 1 (减去 admin queue)
// → set->queue_depth = min(ctrl->sqsize, BLK_MQ_MAX_DEPTH - 1)
// → set->nr_maps = 3  (HCTX_TYPE_DEFAULT / HCTX_TYPE_READ / HCTX_TYPE_POLL)
// → set->ops = &nvme_mq_ops
```

**内核分配的内存布局**（每个 tag）：

```
┌──────────────────────────────────────────────┐
│ struct request  (kmalloc 分配)               │
│  ├─ .tag = i                                │  ← tag 索引 (0 ~ queue_depth-1)
│  ├─ .mq_ctx = NULL / ptr                    │  ← 软件队列指针
│  ├─ .mq_hctx = NULL / ptr                   │  ← 硬件队列指针
│  ├─ .bio = NULL                             │  ← bio 链表头
│  ├─ .q = NULL / ptr                         │  ← 请求队列指针
│  └─ ...                                     │
├──────────────────────────────────────────────┤
│ struct nvme_iod  (cmd_size 字节, 紧邻 request) │  ← 驱动私有数据
│  ├─ .cmd = nvme_command                     │  ← NVMe 命令（SQE）
│  ├─ .n_meta = ...                           │
│  └─ ...                                     │
└──────────────────────────────────────────────┘
```

##### 20.4.2.2 gendisk + request_queue 创建：`blk_mq_alloc_disk()`

**调用位置**：`nvme_alloc_ns()` 第 2 步

**函数原型**：`block/blk-mq.c`

```c
struct gendisk *__blk_mq_alloc_disk(struct blk_mq_tag_set *set,
        struct queue_limits *lim, void *queuedata,
        struct lock_class_key *lkclass)
```

**宏定义**：
```c
#define blk_mq_alloc_disk(set, lim, queuedata)  \
    __blk_mq_alloc_disk(set, lim, queuedata, NULL)
```

**调用栈**：

```
nvme_alloc_ns(ctrl, info)
  └─ disk = blk_mq_alloc_disk(ctrl->tagset, &lim, ns)
       └─ __blk_mq_alloc_disk(set, &lim, ns, NULL)
            ├─ q = blk_mq_alloc_queue(set, lim, queuedata)
            │    ├─ blk_alloc_queue(lim, node)           # 分配 request_queue 结构体
            │    │    └─ kzalloc_node(sizeof(*q))         # 分配队列内存
            │    │    └─ blk_queue_limits_set(q, lim)    # 设置初始队列限制
            │    │    └─ percpu_ref_init(&q->q_usage_counter, ...)  # 初始化引用计数
            │    │    └─ q->disk = NULL                   # 尚未绑定 gendisk
            │    │
            │    └─ blk_mq_init_allocated_queue(set, q)   # 初始化多队列
            │         ├─ q->mq_ops = set->ops             # 绑定 blk-mq 操作函数
            │         ├─ q->queue_ctx = alloc_percpu()    # 分配 per-CPU 软件队列
            │         ├─ q->nr_hw_queues = set->nr_hw_queues
            │         ├─ q->queue_hw_ctx = kcalloc(nr_hw_queues)  # 分配硬件队列数组
            │         ├─ for_each hw_queue:
            │         │    blk_mq_init_hctx(q, set, hctx_idx)
            │         │      └─ hctx->tags = set->tags[hctx_idx]  # 绑定 tag set
            │         │      └─ hctx->queue = q
            │         │      └─ hctx->ctxs = 分配 CPU→ctx 映射
            │         │      └─ hctx->flags = ...
            │         │
            │         └─ blk_mq_add_queue_tag_set(set, q)  # 将队列注册到 tag set
            │              └─ list_add_tail(&q->tag_set_list, &set->tag_list)
            │
            └─ disk = __alloc_disk_node(q, node, lkclass)  # 分配 gendisk
                 ├─ kzalloc_node(sizeof(*disk))             # 分配 gendisk 内存
                 ├─ disk->queue = q                         # 绑定 request_queue
                 ├─ q->disk = disk                          # 反向绑定
                 ├─ xa_init(&disk->part_tbl)                # 初始化分区表
                 ├─ bdev_alloc(disk, 0)                     # 分配 part0（块设备）
                 └─ set_bit(GD_OWNS_QUEUE, &disk->state)   # 标记磁盘拥有队列
```

**关键绑定关系**（创建后）：

```
nvme_ns                     gendisk                   request_queue
┌──────────────┐        ┌──────────────────┐        ┌──────────────────────┐
│ .disk ───────┼───────►│ .private_data ───┼───────►│ .mq_ops = &nvme_mq_ops│
│ .queue ──────┼───────►│ .queue ──────────┼───────►│ .queuedata = ns      │
│ .ctrl        │        │ .fops = &nvme_   │        │ .tag_set = ctrl->    │
│ .head        │        │   bdev_ops       │        │   tagset             │
│ .flags       │        │ .part0 = bdev    │        │ .queue_hw_ctx[]      │
└──────────────┘        └──────────────────┘        │ .queue_ctx (per-CPU) │
                                                     └──────────────────────┘
```

##### 20.4.2.3 队列限制更新：`queue_limits_start_update()` / `queue_limits_commit_update()`

**调用位置**：`nvme_update_ns_info_block()` 中，在冻结队列后、解冻队列前

**调用栈**：

```
nvme_update_ns_info_block(ns, info)
  ├─ lim = queue_limits_start_update(ns->disk->queue)
  │    └─ mutex_lock(&q->limits_lock)
  │    └─ return q->limits  (当前限制快照)
  │
  ├─ 修改 lim 的各个字段：
  │    ├─ nvme_set_ctrl_limits(ns->ctrl, &lim, false)
  │    │    └─ lim.max_hw_sectors = ctrl->max_hw_sectors
  │    │    └─ lim.max_segments = ctrl->max_segments
  │    │    └─ lim.max_integrity_segments = ctrl->max_integrity_segments
  │    │    └─ lim.dma_alignment = 3
  │    │    └─ lim.features |= BLK_FEAT_READ_ONLY (if applicable)
  │    │
  │    ├─ nvme_configure_metadata(ns->ctrl, ns->head, id, nvm, info)
  │    │    └─ 设置 lim.logical_block_size = 1 << ns->head->lba_shift
  │    │    └─ 设置 lim.physical_block_size = 1 << ns->head->lba_shift
  │    │    └─ 设置 lim.io_min = 1 << ns->head->lba_shift
  │    │
  │    ├─ nvme_set_chunk_sectors(ns, id, &lim)
  │    │    └─ lim.chunk_sectors = iob  (IO 块条带大小)
  │    │
  │    ├─ nvme_config_discard(ns, &lim)
  │    │    └─ lim.max_hw_discard_sectors = ...
  │    │    └─ lim.discard_granularity = ...
  │    │
  │    ├─ lim.features |= BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA  (if vwc present)
  │    ├─ lim.features |= BLK_FEAT_ROTATIONAL  (if is_rotational)
  │    └─ nvme_init_integrity(ns->head, &lim, info)
  │         └─ 设置 lim.integrity (保护信息类型、元数据大小等)
  │
  ├─ ret = queue_limits_commit_update(ns->disk->queue, &lim)
  │    └─ blk_queue_limits_set(q, lim)  # 原子性提交所有限制
  │    └─ mutex_unlock(&q->limits_lock)
  │    └─ return 0
  │
  └─ 后续设置：
       ├─ set_capacity_and_notify(ns->disk, capacity)
       ├─ set_disk_ro(ns->disk, readonly)
       └─ set_bit(NVME_NS_READY, &ns->flags)
```

##### 18.4.2.4 冻结/解冻队列：`blk_mq_freeze_queue()` / `blk_mq_unfreeze_queue()`

**作用**：冻结队列阻止新 I/O 提交，确保所有 in-flight I/O 完成，用于安全更新队列参数。

**调用栈**：

```
nvme_update_ns_info_block(ns, info)
  │
  ├─ memflags = blk_mq_freeze_queue(ns->disk->queue)
  │    └─ blk_freeze_queue_start(q)
  │    │    └─ percpu_ref_kill(&q->q_usage_counter)
  │    │    └─ blk_queue_flag_set(QUEUE_FLAG_FREEZING, q)
  │    │    └─ synchronize_rcu()                     # 等待所有 percpu_ref 引用者退出
  │    │    └─ blk_queue_flag_clear(QUEUE_FLAG_FREEZING, q)
  │    │
  │    └─ blk_freeze_queue_wait(q)
  │         └─ wait_event(q->mq_freeze_wq, percpu_ref_is_zero(&q->q_usage_counter))
  │         # 等待所有 I/O 完成（包括排队中尚未提交的请求）
  │
  │  ┌─ 此时队列已冻结：所有新 bio 在 blk_mq_submit_bio() 的
  │  │  bio_queue_enter() 处等待，不会进入 I/O 路径
  │  │
  │  ├─ 修改队列参数（见 18.4.2.3）
  │  │
  │  └─ blk_mq_unfreeze_queue(ns->disk->queue, memflags)
  │       └─ percpu_ref_resurrect(&q->q_usage_counter)  # 恢复引用计数
  │       └─ wake_up_all(&q->mq_freeze_wq)              # 唤醒等待的 I/O 提交者
  │       └─ memalloc_nofs_restore(memflags)             # 恢复内存分配标志
```

##### 18.4.2.5 容量设置：`set_capacity_and_notify()`

**调用位置**：`nvme_update_ns_info_block()` 末尾

**函数原型**：`block/genhd.c`

```c
void set_capacity_and_notify(struct gendisk *disk, sector_t capacity)
```

**内部逻辑**：
```
set_capacity_and_notify(disk, capacity)
  ├─ old_capacity = get_capacity(disk)       # 保存旧容量
  ├─ disk->part0->bd_nr_sectors = capacity   # 设置新容量
  │
  ├─ if (old_capacity != capacity)
  │    └─ kobject_uevent_env(&disk_to_dev(disk)->kobj, KOBJ_CHANGE, envp)
  │    # 发送 RESIZE=1 的 uevent 到用户空间（如 udev）
  │
  └─ # 如果是首次注册（容量从 0 变为非 0），
     # __add_disk() 中的 add_disk_final() 会扫描分区表
```

##### 20.4.2.6 块设备注册：`device_add_disk()`

**调用位置**：`nvme_alloc_ns()` 第 8 步

**调用栈**：

```
device_add_disk(ctrl->device, ns->disk, nvme_ns_attr_groups)
  └─ add_disk_fwnode(parent, disk, groups, NULL)
       ├─ 获取 tag_set->update_nr_hwq_lock（读锁）
       │
       ├─ __add_disk(parent, disk, groups, NULL)
       │    ├─ 设备号分配
       │    │    ├─ if (disk->major == 0)  # NVMe 驱动不设置 major
       │    │    │    ret = blk_alloc_ext_minor()
       │    │    │    disk->major = BLOCK_EXT_MAJOR
       │    │    │    disk->first_minor = ret
       │    │    └─ ddev->devt = MKDEV(disk->major, disk->first_minor)
       │    │
       │    ├─ ddev->parent = parent
       │    ├─ dev_set_name(ddev, "%s", disk->disk_name)  # 如 "nvme0n1"
       │    │
       │    ├─ ret = device_add(ddev)
       │    │    # 创建 /sys/block/nvme0n1 设备节点
       │    │    # 注册 sysfs 属性 (nvme_ns_attr_groups)
       │    │
       │    ├─ disk_alloc_events(disk)           # 分配事件处理
       │    │
       │    ├─ sysfs_create_link(block_depr, ...)  # 创建 /sys/block/ 链接
       │    │
       │    ├─ bdi_register(disk->bdi, "nvme0n1")  # 注册 BDI (backing device info)
       │    │
       │    ├─ blk_register_queue(disk)           # 注册 /sys/block/nvme0n1/queue/
       │    │    ├─ 创建 queue 相关 sysfs 文件
       │    │    ├─ 初始化调度器 (elevator_init)
       │    │    └─ 注册 mq sysfs 属性
       │    │
       │    ├─ 创建分区相关目录
       │    │    └─ device_add_disk 时还不够分区
       │    │
       │    └─ 释放 uevent suppress
       │         └─ dev_set_uevent_suppress(ddev, 0)
       │
       ├─ 释放 update_nr_hwq_lock 读锁
       │
       └─ add_disk_final(disk)
            ├─ register_disk(disk)              # 创建 /dev/nvme0n1 设备节点
            └─ blk_add_disk_partitions(disk)    # 扫描并添加分区
                 └─ bdev_disk_changed(disk, true)
                      └─ blk_scan_partitions(disk)
                           └─ 读取分区表 → 为每个分区添加 partN
                           └─ 创建 /dev/nvme0n1p1, /dev/nvme0n1p2, ...
```

**关键数据结构：`struct block_device_operations nvme_bdev_ops`**

```c
const struct block_device_operations nvme_bdev_ops = {
    .owner          = THIS_MODULE,
    .ioctl          = nvme_ioctl,              // 块设备 ioctl 处理
    .compat_ioctl   = blkdev_compat_ptr_ioctl, // 32-bit 兼容 ioctl
    .open           = nvme_open,               // open /dev/nvmeXnY
    .release        = nvme_release,            // close /dev/nvmeXnY
    .getgeo         = nvme_getgeo,             // 获取磁盘几何信息
    .get_unique_id  = nvme_get_unique_id,      // 获取唯一标识符
    .report_zones   = nvme_report_zones,       // 报告 ZNS 分区信息
    .pr_ops         = &nvme_pr_ops,            // 持久化保留操作
};
```

**注册完成后的内核可见状态**：

```
/sys/block/nvme0n1/                  ← 块设备 sysfs 目录
  ├── queue/                          ← 请求队列参数
  ├── device -> ../../../nvme0/       ← 指向父设备（控制器）
  ├── capability
  ├── ext_range
  ├── ...
/dev/nvme0n1                         ← 块设备节点
/dev/nvme0n1p1                       ← 分区节点（如果存在分区表）
```

#### 20.4.3 块设备移除流程（Driver → Block Layer 视角）

##### 20.4.3.1 标记磁盘死亡：`blk_mark_disk_dead()`

**调用位置**：`nvme_mark_namespaces_dead()`，在控制器突然断开（如热拔出）时调用

**调用栈**：

```
nvme_mark_namespaces_dead(ctrl)                    # 遍历所有 ns
  └─ for_each ns:
       blk_mark_disk_dead(ns->disk)
         └─ __blk_mark_disk_dead(disk)
              ├─ test_and_set_bit(GD_DEAD, &disk->state)  # 设置 GD_DEAD 标志
              │
              ├─ if (GD_OWNS_QUEUE)
              │    blk_queue_flag_set(QUEUE_FLAG_DYING, q)  # 设置 DYING 标志
              │
              ├─ set_capacity(disk, 0)                      # 容量清零
              │
              └─ blk_queue_start_drain(disk->queue)          # 启动 drain 模式
                   └─ percpu_ref_kill(&q->q_usage_counter)   # 拒绝新 I/O
                   └─ if (q->mq_ops)
                        blk_mq_wake_awaiters(q)              # 唤醒等待者
         └─ blk_report_disk_dead(disk, surprise=true)
              └─ xa_for_each(&disk->part_tbl, idx, bdev)
                   bdev_mark_dead(bdev, surprise)            # 通知文件系统
```

**状态变化**：

```
GD_DEAD=0 → GD_DEAD=1   # 磁盘标记死亡
QUEUE_FLAG_DYING=0 → QUEUE_FLAG_DYING=1  # 队列标记销毁中
# 此后所有新 bio 提交返回 BLK_STS_IOERR
# 所有排队中的请求取消或失败
```

##### 18.4.3.2 注销块设备：`del_gendisk()`

**调用位置**：`nvme_ns_remove()` 第 10 步

**调用栈**：

```
nvme_ns_remove(ns)
  ├─ ... 前置步骤 ...
  │
  └─ del_gendisk(ns->disk)
       └─ if (queue_is_mq(q))
            ├─ disable_elv_switch(q)           # 禁止调度器切换
            │
            └─ __del_gendisk(disk)
                 ├─ disk_del_events(disk)       # 删除事件处理
                 │
                 ├─ 阻止新 opener
                 │    └─ xa_for_each(&disk->part_tbl, idx, part)
                 │         bdev_unhash(part)    # 从 bdev 缓存中摘除
                 │
                 ├─ blk_report_disk_dead(disk, false)  # 通知文件系统
                 │    └─ xa_for_each(&disk->part_tbl, idx, bdev)
                 │         bdev_mark_dead(bdev, false)  # 优雅关闭
                 │
                 ├─ __blk_mark_disk_dead(disk)  # 标记死亡（如果尚未标记）
                 │    └─ blk_freeze_acquire_lock(q)  # 获取冻结锁
                 │
                 ├─ 删除所有分区
                 │    └─ xa_for_each_start(&disk->part_tbl, idx, part, 1)
                 │         drop_partition(part)  # 删除每个分区
                 │
                 ├─ 卸载 BDI 和 sysfs
                 │    ├─ sysfs_remove_link(..., "bdi")
                 │    ├─ bdi_unregister(disk->bdi)
                 │    └─ blk_unregister_queue(disk)
                 │         ├─ 删除 /sys/block/nvme0n1/queue/
                 │         └─ elevator_exit()
                 │
                 ├─ 清理磁盘设备
                 │    ├─ kobject_put(disk->part0->bd_holder_dir)
                 │    ├─ part_stat_set_all(disk->part0, 0)
                 │    ├─ device_del(disk_to_dev(disk))  # 删除 /sys/block/nvme0n1
                 │    └─ pm_runtime_set_memalloc_noio(..., false)
                 │
                 ├─ blk_mq_freeze_queue_wait(q)  # 等待所有 I/O 完全停止
                 │
                 ├─ blk_throtl_cancel_bios(disk)  # 取消限流队列
                 ├─ blk_sync_queue(q)             # 同步队列
                 ├─ blk_flush_integrity()          # 清理完整性检查
                 ├─ blk_mq_cancel_work_sync(q)    # 取消所有工作队列
                 ├─ rq_qos_exit(q)               # 退出 QoS
                 │
                 └─ if (GD_OWNS_QUEUE)
                      blk_mq_exit_queue(q)        # 退出多队列
                      # 内部调用：
                      #   blk_mq_destroy_queue_ctxs()
                      #   blk_mq_free_queue_rqs(q)
                      #   blk_mq_free_queue_rcu()
                      #   percpu_ref_exit()
```

##### 18.4.3.3 队列销毁：`blk_mq_destroy_queue()`

**调用位置**：`nvme_remove_admin_tag_set()` 中

**调用栈**：

```
nvme_remove_admin_tag_set(ctrl)
  └─ blk_mq_destroy_queue(ctrl->admin_q)
       └─ blk_queue_flag_set(QUEUE_FLAG_DYING, q)  # 标记 DYING
       └─ blk_queue_start_drain(q)                  # 启动 drain
       └─ blk_mq_exit_queue(q)                      # 退出多队列
            ├─ blk_mq_destroy_queue_ctxs(q)         # 释放软件队列
            ├─ blk_mq_free_queue_rqs(q)             # 释放 request 内存
            └─ blk_mq_free_queue_rcu(q)             # RCU 延迟释放
```

##### 20.4.3.4 Tag Set 释放：`blk_mq_free_tag_set()`

**调用位置**：`nvme_remove_admin_tag_set()` / `nvme_remove_io_tag_set()`

**调用栈**：

```
nvme_remove_io_tag_set(ctrl)
  └─ blk_mq_free_tag_set(ctrl->tagset)
       ├─ list_del(&set->tag_set_list)              # 从全局 tag_set_list 摘除
       ├─ for_each hw_queue:
       │    blk_mq_free_map_and_rqs(set, tags, hctx_idx)
       │      ├─ sbitmap_queue_free(&tags->bitmap_tags)     # 释放 sbitmap
       │      └─ blk_mq_free_rqs(set, tags, hctx_idx)
       │           └─ for_each tag:
       │                kfree(rqs[i])               # 释放 struct request + cmd_size
       │                # 注意：此时 nvme_iod 作为 request 的私有数据一同释放
       │
       └─ kfree(set->tags)                          # 释放 tags[] 指针数组
```

**移除流程中各步骤的时序关系**：

```
nvme_remove(pdev)
  │
  ├─ nvme_stop_ctrl()           # 停止 I/O 活动
  │
  ├─ nvme_remove_namespaces()
  │    └─ nvme_ns_remove(ns)
  │         ├─ set_capacity(disk, 0)          # 1. 容量清零
  │         └─ del_gendisk(ns->disk)          # 2. 注销块设备
  │              ├─ GD_DEAD=1                 #   a. 标记死亡
  │              ├─ 删除分区表                #   b. 删除分区
  │              ├─ 删除 sysfs                #   c. 删除 sysfs 节点
  │              └─ blk_mq_exit_queue(q)      #   d. 释放 request 队列
  │
  ├─ nvme_dev_disable()         # 硬件禁用
  │    ├─ 删除 IO 队列
  │    └─ 取消所有 tagset 请求
  │
  ├─ nvme_dev_remove_admin()    # 释放 Admin 资源
  │    └─ nvme_remove_admin_tag_set()
  │         ├─ blk_mq_destroy_queue(admin_q) # 销毁 Admin 队列
  │         └─ blk_mq_free_tag_set(admin_tagset)  # 释放 Admin TagSet
  │
  ├─ nvme_free_queues()         # 释放硬件队列内存
  │
  └─ nvme_uninit_ctrl()         # 控制器反初始化
       └─ cdev_device_del()     # 删除字符设备 /dev/nvmeX
```

#### 20.4.4 关键块层数据结构（与 NVMe 驱动交互相关）

##### 18.4.4.1 `struct gendisk` — 通用磁盘表示

文件：`include/linux/blkdev.h` L144

```c
struct gendisk {
    int major;                          // 主设备号（NVMe 使用 BLOCK_EXT_MAJOR）
    int first_minor;                    // 次设备号起始
    int minors;                         // 次设备号数量（分区数 + 1）

    char disk_name[DISK_NAME_LEN];      // 磁盘名称，如 "nvme0n1"

    struct xarray part_tbl;             // 分区表（xarray 索引：0=whole disk, 1+ = partitions）
    struct block_device *part0;         // 整个磁盘的块设备（bdev）

    const struct block_device_operations *fops;  // 块设备操作（nvme_bdev_ops）
    struct request_queue *queue;        // 请求队列
    void *private_data;                 // 驱动私有数据（指向 nvme_ns）

    struct bio_set bio_split;           // 用于 bio 拆分的 mempool

    int flags;                          // 通用标志
    unsigned long state;                // 状态位图
#define GD_NEED_PART_SCAN       0       // 需要分区扫描
#define GD_READ_ONLY            1       // 只读
#define GD_DEAD                 2       // 已死亡（移除流程中设置）
#define GD_NATIVE_CAPACITY      3       // 原生容量
#define GD_ADDED                4       // 已通过 device_add_disk() 注册
#define GD_SUPPRESS_PART_SCAN   5       // 抑制分区扫描
#define GD_OWNS_QUEUE           6       // 拥有队列（由 blk_mq_alloc_disk 创建）

    struct mutex open_mutex;            // open/close 互斥锁
    unsigned open_partitions;           // 已打开分区数

    struct backing_dev_info *bdi;       // 回写设备信息
    struct kobject *slave_dir;          // 从设备目录
    struct disk_events *ev;             // 事件处理
    int node_id;                        // NUMA 节点
    u64 diskseq;                        // 全局唯一序列号
    blk_mode_t open_mode;               // 当前打开模式
};
```

##### 20.4.4.2 `struct request_queue` — 请求队列

文件：`include/linux/blkdev.h` L478

```c
struct request_queue {
    void *queuedata;                    // 驱动私有数据（指向 nvme_ns）

    struct elevator_queue *elevator;    // I/O 调度器（如 mq-deadline）
    const struct blk_mq_ops *mq_ops;    // blk-mq 操作函数表（nvme_mq_ops）

    struct blk_mq_ctx __percpu *queue_ctx;  // per-CPU 软件队列

    unsigned long queue_flags;          // QUEUE_FLAG_* 标志
    unsigned int rq_timeout;            // 请求超时时间

    unsigned int queue_depth;           // 队列深度（= tag set 的 queue_depth）
    refcount_t refs;                    // 引用计数

    unsigned int nr_hw_queues;          // 硬件队列数
    struct blk_mq_hw_ctx * __rcu *queue_hw_ctx;  // 硬件队列指针数组

    struct percpu_ref q_usage_counter;  // I/O 使用计数（冻结/解冻控制）
    struct gendisk *disk;               // 反向指向 gendisk
    struct queue_limits limits;         // 队列限制（NVMe 在 nvme_update_ns_info_block 中设置）

    struct kobject *mq_kobj;            // 多队列 sysfs kobject
    unsigned int nr_requests;           // 最大请求数
    unsigned int async_depth;           // 异步请求深度
};
```

##### 18.4.4.3 `struct queue_limits` — 队列限制

文件：`include/linux/blkdev.h` L370

```c
struct queue_limits {
    blk_features_t features;            // 特性位图（BLK_FEAT_*）
    blk_flags_t flags;                  // 内部标志
    unsigned long seg_boundary_mask;    // DMA 段边界掩码
    unsigned long virt_boundary_mask;   // 虚拟边界掩码

    unsigned int max_hw_sectors;        // 最大硬件扇区数（NVMe: ctrl->max_hw_sectors）
    unsigned int max_dev_sectors;       // 设备最大扇区数
    unsigned int chunk_sectors;         // 条带块大小（NVMe: noiob/nvme_set_chunk_sectors）
    unsigned int max_sectors;           // 最大 I/O 扇区数（min of hw/dev/user）
    unsigned int max_segment_size;      // 最大段字节数
    unsigned int physical_block_size;   // 物理块大小（NVMe: 1 << lba_shift, 通常 512/4096）
    unsigned int logical_block_size;    // 逻辑块大小（同上）
    unsigned int alignment_offset;      // 对齐偏移
    unsigned int io_min;               // 最小 I/O 大小
    unsigned int io_opt;               // 最佳 I/O 大小

    unsigned int max_discard_sectors;    // 最大 DISCARD 扇区数
    unsigned int max_write_zeroes_sectors;  // 最大 WRITE ZEROES 扇区数
    unsigned int discard_granularity;   // DISCARD 粒度
    unsigned int discard_alignment;     // DISCARD 对齐

    unsigned short max_segments;        // 最大 DMA 段数（NVMe: NVMe_MAX_SEGS）
    unsigned short max_integrity_segments;  // 最大完整性段数
    unsigned short max_discard_segments;    // 最大 DISCARD 段数
    unsigned short max_write_streams;   // 最大写流数

    unsigned int dma_alignment;         // DMA 对齐要求（NVMe: 3, 即 8 字节对齐）
    unsigned int dma_pad_mask;          // DMA 填充掩码

    struct blk_integrity integrity;     // 完整性/元数据配置
};
```

##### 18.4.4.4 `struct block_device_operations` — 块设备操作函数表

文件：`include/linux/blkdev.h` L1648

```c
struct block_device_operations {
    void (*submit_bio)(struct bio *bio);  // 非 blk-mq 驱动使用（NVMe 不设置）
    int (*poll_bio)(struct bio *bio, ...); // 轮询 bio（NVMe 不设置）
    int (*open)(struct gendisk *disk, blk_mode_t mode);  // 打开设备
    void (*release)(struct gendisk *disk);  // 关闭设备
    int (*ioctl)(struct block_device *bdev, blk_mode_t mode,
                 unsigned cmd, unsigned long arg);  // ioctl 处理
    int (*compat_ioctl)(...);             // 32-bit 兼容 ioctl
    int (*getgeo)(struct gendisk *, struct hd_geometry *);  // 磁盘几何信息
    int (*set_read_only)(struct block_device *bdev, bool ro);  // 设置只读
    void (*free_disk)(struct gendisk *disk);  // 释放磁盘回调
    int (*report_zones)(...);             // ZNS 分区报告
    int (*get_unique_id)(...);            // 获取唯一标识符
    struct module *owner;                 // 所属模块
    const struct pr_ops *pr_ops;          // 持久化保留操作
};
```

#### 20.4.5 块层交互 API 汇总表

| 块层 API | 调用位置 | 阶段 | 功能描述 |
|----------|----------|------|----------|
| `blk_mq_alloc_tag_set()` | `nvme_alloc_admin_tag_set()` / `nvme_alloc_io_tag_set()` | 注册 | 分配 TagSet，初始化 sbitmap 位图和 request 数组 |
| `blk_mq_alloc_disk()` | `nvme_alloc_ns()` | 注册 | 创建 gendisk + request_queue，绑定到 TagSet |
| `queue_limits_start_update()` | `nvme_update_ns_info_block()` | 注册 | 获取队列限制锁，准备更新 |
| `blk_mq_freeze_queue()` | `nvme_update_ns_info_block()` | 注册 | 冻结队列，等待所有 I/O 完成 |
| `queue_limits_commit_update()` | `nvme_update_ns_info_block()` | 注册 | 提交队列限制更新（原子性） |
| `set_capacity_and_notify()` | `nvme_update_ns_info_block()` | 注册 | 设置磁盘容量，发送 RESIZE uevent |
| `set_disk_ro()` | `nvme_update_ns_info_block()` | 注册 | 设置磁盘只读状态 |
| `blk_mq_unfreeze_queue()` | `nvme_update_ns_info_block()` | 注册 | 解冻队列，恢复 I/O 提交 |
| `device_add_disk()` | `nvme_alloc_ns()` | 注册 | 注册块设备：创建 sysfs、设备节点、扫描分区 |
| `blk_mark_disk_dead()` | `nvme_mark_namespaces_dead()` | 移除 | 标记磁盘死亡（GD_DEAD），拒绝新 I/O |
| `set_capacity()` | `nvme_ns_remove()` | 移除 | 容量清零 |
| `del_gendisk()` | `nvme_ns_remove()` | 移除 | 注销块设备：删除分区、sysfs、设备节点 |
| `blk_mq_destroy_queue()` | `nvme_remove_admin_tag_set()` | 移除 | 销毁请求队列（标记 DYING，释放资源） |
| `blk_mq_free_tag_set()` | `nvme_remove_admin_tag_set()` / `nvme_remove_io_tag_set()` | 移除 | 释放 TagSet：释放 sbitmap 和所有 request 内存 |
| `blk_queue_dying()` | `nvme_dev_remove_admin()` | 移除 | 检查队列是否 DYING（决定是否跳过销毁） |

#### 20.4.6 完整生命周期时序图

```
nvme_probe()                         块层                                    用户空间
    │                                  │                                        │
    ├─ blk_mq_alloc_tag_set()  ───────►│ 分配 sbitmap + request[]              │
    │                                  │                                        │
    ├─ nvme_scan_work()                │                                        │
    │   └─ nvme_alloc_ns()             │                                        │
    │      ├─ blk_mq_alloc_disk() ────►│ 创建 gendisk + request_queue           │
    │      │                           │ 绑定 nvme_mq_ops                       │
    │      │                           │ 初始化 hw_ctx / sw_ctx                 │
    │      │                           │                                        │
    │      ├─ blk_mq_freeze_queue() ──►│ percpu_ref_kill() → 等待 I/O 完成      │
    │      ├─ queue_limits_commit() ──►│ 设置 max_sectors, block_size, 等       │
    │      ├─ set_capacity_and_notify()│ 设置 bd_nr_sectors                     │
    │      ├─ blk_mq_unfreeze_queue()►│ percpu_ref_resurrect() → 恢复 I/O       │
    │      │                           │                                        │
    │      └─ device_add_disk() ─────►│ device_add(ddev)          ────────────►│ /sys/block/nvme0n1
    │                                 │ blk_register_queue()     ────────────►│ /sys/block/nvme0n1/queue/
    │                                 │ add_disk_final()          ────────────►│ /dev/nvme0n1
    │                                 │   └─ 分区扫描            ────────────►│ /dev/nvme0n1p1
    │                                  │                                        │
    │    ▲ 设备正常工作 ▲              │                                        │
    │    ▼ 设备移除     ▼              │                                        │
    │                                  │                                        │
nvme_remove()                          │                                        │
    │                                  │                                        │
    ├─ nvme_ns_remove()                │                                        │
    │   ├─ set_capacity(disk, 0) ─────►│ bd_nr_sectors = 0                     │
    │   │                              │                                        │
    │   └─ del_gendisk(disk) ────────►│ disk_del_events()                      │
    │                                 │ blk_report_disk_dead()                  │
    │                                 │   └─ bdev_mark_dead()  ───────────────►│ 通知文件系统
    │                                 │ drop_partition(part)                   │
    │                                 │ blk_unregister_queue() ───────────────►│ 删除 /sys/block/nvme0n1/queue/
    │                                 │ device_del(ddev)       ───────────────►│ 删除 /sys/block/nvme0n1
    │                                 │ blk_mq_exit_queue()                    │
    │                                 │   └─ 释放 hw_ctx/sw_ctx               │
    │                                 │   └─ 释放 request 内存                 │
    │                                 │                                        │
    └─ nvme_remove_admin_tag_set()     │                                        │
         └─ blk_mq_free_tag_set() ───►│ 释放 sbitmap + tags[]                  │
                                      │                                        │
                                      │   最终：设备节点和 sysfs 全部删除       │
```

### 20.5 关键工作队列

NVMe 驱动使用三个全局工作队列（`core.c`）：

| 工作队列 | 用途 |
|----------|------|
| `nvme_wq` | 扫描、AEN 处理、固件激活、Keep-Alive、周期性重连 |
| `nvme_reset_wq` | 控制器复位（会 flush nvme_wq 上的工作） |
| `nvme_delete_wq` | 控制器删除（会 flush nvme_reset_wq 上的工作） |

### 20.6 涉及的文件清单

| 文件 | 行数 | 作用 |
|------|------|------|
| `drivers/nvme/host/core.c` | ~5,200 | NVMe 核心逻辑：namespace 管理、控制器生命周期、Admin 命令 |
| `drivers/nvme/host/pci.c` | ~3,900 | PCIe 传输层：probe/remove、队列创建/销毁、中断处理 |
| `drivers/nvme/host/nvme.h` | ~700 | 核心数据结构定义（nvme_ctrl/nvme_ns/nvme_ns_head 等） |
| `drivers/nvme/host/multipath.c` | — | 多路径支持（ANA 状态管理） |
| `drivers/nvme/host/sysfs.c` | — | NVMe 特定的 sysfs 属性 |
| `drivers/nvme/host/ioctl.c` | — | NVMe 私有 ioctl 处理 |
| `drivers/nvme/host/pr.c` | — | Persistent Reservation 支持 |
| `drivers/nvme/host/fabrics.c` | — | NVMe-oF Fabrics 通用代码 |
| `drivers/nvme/host/tcp.c` | — | NVMe/TCP 传输层 |
| `drivers/nvme/host/rdma.c` | — | NVMe/RDMA 传输层 |
| `drivers/nvme/host/fc.c` | — | NVMe/FC 传输层 |

---

## 21. NVMe 驱动：块设备读写 I/O 流程

本章分析从用户态程序 `open()` / `read()` / `write()` 一个 NVMe 块设备（如 `/dev/nvme0n1`），到最终数据通过 PCIe 总线传输到硬件 SSD 的完整 I/O 路径，以及从中断/轮询完成到唤醒用户态程序的逆过程。

### 21.1 I/O 流程总览

用户态 `read()`/`write()` 调用经过以下层次：

```
用户态 Application
  │  read(fd, buf, len) / write(fd, buf, len)
  ▼
════════════════════════ VFS 层 ════════════════════════
  │  file->f_op->read_iter() / write_iter()    [block/fops.c]
  │  blkdev_read_iter() / blkdev_write_iter()   O_DIRECT 路径
  │     └─ __blkdev_direct_IO_simple() / __blkdev_direct_IO()
  │           └─ 构建 struct bio
  │              └─ submit_bio(bio)           [block/blk-core.c]
  ▼
════════════════════════ 通用块层 ════════════════════════
  │  submit_bio_noacct()                      [block/blk-core.c]
  │     └─ __submit_bio()
  │           └─ blk_mq_submit_bio(bio)       [block/blk-mq.c]
  │                 ├─ 尝试合并 (bio merge)
  │                 ├─ 分配 request (blk_mq_get_new_requests)
  │                 ├─ 绑定 bio 到 request
  │                 ├─ QoS 限流 (rq_qos_track)
  │                 ├─ 加密 (blk_crypto_rq_get_keyslot)
  │                 ├─ plug 缓存 / 调度器入队
  │                 └─ blk_mq_try_issue_directly(hctx, rq)
  │                       └─ blk_mq_request_issue_directly()
  │                             └─ __blk_mq_issue_directly()
  │                                   └─ q->mq_ops->queue_rq()  ← 进入 NVMe 驱动
  ▼
═══════════════════════ NVMe 驱动层 ═════════════════════════
  │  nvme_queue_rq()                          [drivers/nvme/host/pci.c]
  │     ├─ 检查 NVMEQ_ENABLED
  │     ├─ nvme_check_ready()               控制器状态检查
  │     ├─ nvme_prep_rq(req)                准备请求
  │     │     ├─ nvme_setup_cmd(ns, req)    构造 NVMe 命令 (SLBA, length, opcode...)
  │     │     │     └─ nvme_setup_rw(ns, req, cmd, op)  填充 READ/WRITE 命令
  │     │     ├─ nvme_map_data(req)         DMA 映射 (PRP/SGL)
  │     │     ├─ nvme_map_metadata(req)     元数据 DMA 映射
  │     │     └─ nvme_start_request(req)    设置超时，更新统计
  │     ├─ nvme_sq_copy_cmd(nvmeq, &iod->cmd)  拷贝命令到 SQ
  │     └─ nvme_write_sq_db(nvmeq, bd->last)   写 Doorbell 通知硬件
  ▼
════════════════════════ 硬件层 ═════════════════════════
  │  NVMe SSD 控制器
  │     ├─ 从 SQ 取出命令
  │     ├─ 执行 READ/WRITE (DMA 传输)
  │     └─ 写入 CQ 完成条目 + 发送 MSI-X 中断
  ▼
═══════════════════════ 中断处理 ═════════════════════════
  │  nvme_irq()                               [drivers/nvme/host/pci.c]
  │     └─ nvme_poll_cq(nvmeq, &iob)
  │           └─ while (nvme_cqe_pending(nvmeq)):
  │                 ├─ nvme_handle_cqe(nvmeq, iob, idx)
  │                 │     ├─ nvme_find_rq()         找到对应的 request
  │                 │     └─ nvme_try_complete_req() 尝试完成请求
  │                 │           └─ nvme_pci_complete_rq(req)
  │                 │                 ├─ nvme_pci_unmap_rq(req)  DMA 反映射
  │                 │                 └─ nvme_complete_rq(req)   NVMe 完成处理
  │                 │                       └─ nvme_end_req(req)
  │                 │                             └─ blk_mq_end_request(req, status)
  │                 └─ nvme_update_cq_head(nvmeq)  更新 CQ 头指针
  │           └─ nvme_ring_cq_doorbell(nvmeq)  写 Doorbell 通知硬件已消费
  │
  │  blk_mq_end_request() → bio_endio() → bio->bi_end_io()
  │     └─ blkdev_bio_end_io()                [block/fops.c]
  │           └─ 唤醒等待的 io_complete() 或 唤醒同步等待的进程
  ▼
  用户态  (read()/write() 返回)
```

---

### 21.2 涉及的关键数据结构

#### 21.2.1 nvme_iod — I/O 描述符（Per-Request）

文件：`drivers/nvme/host/pci.c` L431-L444

```c
/*
 * nvme_iod 描述一次 I/O 操作的数据载荷。
 * 每个 blk-mq request 携带一个 nvme_iod（通过 blk_mq_rq_to_pdu 获取）。
 * 在 tag set 分配时预留空间：cmd_size = sizeof(struct nvme_iod)。
 */
struct nvme_iod {
    struct nvme_request req;                     // 通用 NVMe 请求（含 ctrl 指针、状态、重试次数）
    struct nvme_command cmd;                     // 硬件 NVMe 命令（64 字节，直接拷贝到 SQ）
    u8 flags;                                    // 标志位：IOD_ABORTED/SMALL_DESCRIPTOR/SINGLE_SEGMENT/P2P/MMIO
    u8 nr_descriptors;                           // 描述符数量（PRP list 或 SGL 段数）

    size_t total_len;                            // 数据总长度
    struct dma_iova_state dma_state;             // DMA 映射的 IOVA 状态跟踪
    void *descriptors[NVME_MAX_NR_DESCRIPTORS];  // 描述符指针数组（PRP list 页或 SGL 段）
    struct nvme_dma_vec *dma_vecs;               // DMA 向量数组（用于 DMA 重复映射）
    unsigned int nr_dma_vecs;                    // DMA 向量数量

    dma_addr_t meta_dma;                         // 元数据 DMA 地址
    size_t meta_total_len;                       // 元数据总长度
    struct dma_iova_state meta_dma_state;        // 元数据 DMA 状态
    struct nvme_sgl_desc *meta_descriptor;       // 元数据 SGL 描述符
};
```

**iod_flags 标志位**：

| 标志 | 含义 |
|------|------|
| `IOD_ABORTED` | 命令已被超时处理程序中止 |
| `IOD_SMALL_DESCRIPTOR` | 使用小描述符池（小块内存） |
| `IOD_SINGLE_SEGMENT` | 单段 DMA 映射 |
| `IOD_DATA_P2P` | 数据载荷包含 P2P 内存 |
| `IOD_META_P2P` | 元数据包含 P2P 内存 |
| `IOD_DATA_MMIO` | 数据载荷包含 MMIO 内存 |
| `IOD_META_MMIO` | 元数据包含 MMIO 内存 |
| `IOD_SINGLE_META_SEGMENT` | 使用非合并 MPTR 的元数据 |

#### 21.2.2 nvme_dma_vec — DMA 向量

文件：`drivers/nvme/host/pci.c` L421-L424

```c
struct nvme_dma_vec {
    dma_addr_t addr;       // DMA 地址
    unsigned int len;      // 长度
};
```

#### 21.2.3 nvme_command — NVMe 硬件命令

文件：`drivers/nvme/host/nvme.h` L108-L175（部分）

```c
/*
 * NVMe 命令结构体（64 字节），对应硬件 SQ Entry 格式。
 * 支持多种命令类型（RW/Identify/Features/Admin 等），通过 union 复用空间。
 */
struct nvme_command {
    union {
        struct nvme_common_command common;       // 公共字段：opcode, flags, command_id, nsid, cdw2-15
        struct nvme_rw_command rw;               // 读写命令：opcode, nsid, slba, length, control, dsmgmt
        struct nvme_identify identify;           // Identify 命令
        struct nvme_features features;           // Set/Get Features 命令
        struct nvme_dsm_range dsm;               // DataSet Management 命令
    };
};
```

**nvme_rw_command 字段**（读写命令）：

| 字段 | 宽度 | 含义 |
|------|------|------|
| `opcode` | u8 | 操作码：0x02=Read, 0x01=Write |
| `flags` | u8 | 标志位 |
| `command_id` | u16 | 命令 ID（对应 tag） |
| `nsid` | u32 | Namespace ID |
| `slba` | u64 | 起始逻辑块地址 |
| `length` | u16 | 块数 - 1（0-based） |
| `control` | u16 | 控制位：FUA, LR, PRACT, PRCHK 等 |
| `dsmgmt` | u32 | DataSet Management |

#### 21.2.4 nvme_request — 通用 NVMe 请求

文件：`drivers/nvme/host/nvme.h` L259-L286

```c
/*
 * nvme_request 嵌入在 nvme_iod 中，提供 NVMe 请求的通用状态跟踪。
 * 通过 blk_mq_rq_to_pdu(req) → nvme_req(req) 宏获取。
 */
struct nvme_request {
    struct nvme_ctrl *ctrl;      // 所属控制器
    struct nvme_command *cmd;    // 指向 nvme_iod.cmd
    union nvme_result result;    // 完成结果（CQE 中的 DW0-1）
    u16 status;                  // 命令状态码
    u8 retries;                  // 重试次数
    u8 flags;                    // 标志位
    u8 opcode;                   // 操作码
};
```

#### 21.2.5 blkdev_dio — 块设备直接 I/O 描述符

文件：`block/fops.c` L122-L130

```c
/*
 * 块设备直接 I/O 描述符，用于 O_DIRECT 读写。
 * 同步/异步 I/O 通过 flags 中的 DIO_IS_SYNC 区分。
 */
struct blkdev_dio {
    union {
        struct kiocb *iocb;              // 异步 I/O 的 kiocb
        struct task_struct *waiter;      // 同步 I/O 的等待进程
    };
    size_t size;                         // 已完成字节数
    atomic_t ref;                        // 引用计数（多 bio 模式）
    unsigned int flags;                  // DIO_SHOULD_DIRTY | DIO_IS_SYNC
    struct bio bio;                      // 嵌入的 bio 结构体
};
```

#### 21.2.6 nvme_ns_head — Namespace 头（共享信息）

文件：`drivers/nvme/host/nvme.h` L526-L568

```c
/*
 * nvme_ns_head 保存 namespace 的全局共享信息。多路径场景下多个 nvme_ns
 * 共享同一个 nvme_ns_head。读写 I 路径中通过 ns->head->lba_shift 计算
 * LBA 地址，通过 ns->head->ns_id 填充命令字的 NSID 字段。
 */
struct nvme_ns_head {
    struct list_head list;               // 挂入 subsystem 的 ns 链表
    struct nvme_subsystem *subsys;       // 所属子系统
    struct nvme_ns_ids ids;              // namespace 标识符 (EUI64, NGUID, UUID)
    u8 lba_shift;                        // LBA 大小偏移：block_size = 1 << lba_shift
    u16 ms;                              // 元数据大小（每 LBA）
    u16 pi_size;                         // 保护信息大小
    u8 pi_type;                          // 保护信息类型 (Type 1/2/3)
    u8 guard_type;                       // Guard 校验类型 (CRC16/CRC64)
    struct kref ref;                     // 引用计数
    bool shared;                         // 是否多路径共享
    u64 nuse;                            // 已分配 LBA 数量
    unsigned ns_id;                      // Namespace ID (写入 NVMe 命令 NSID 字段)
    int instance;                        // 实例号 (用于生成 /dev/nvmeXnY)
    u64 zsze;                            // Zone Size (zoned 设备)
    struct gendisk *disk;                // 多路径聚合 gendisk
    u16 nr_plids;                        // 放置标识符数量（写流）
    u16 *plids;                          // 放置标识符数组
};
```

#### 21.2.7 nvme_ns — Namespace 实例（Per-Controller）

文件：`drivers/nvme/host/nvme.h` L585-L617

```c
/*
 * nvme_ns 代表一个 NVMe namespace 在特定控制器上的实例。
 * 每个 I 请求通过 req->q->queuedata 指向所属的 nvme_ns。
 * 在 nvme_setup_cmd() 中通过 ns 获取 LBA 偏移、NSID 等。
 */
struct nvme_ns {
    struct list_head list;               // 挂入 controller 的 ns 链表
    struct nvme_ctrl *ctrl;              // 所属控制器
    struct request_queue *queue;         // 关联的请求队列 (req->q)
    struct gendisk *disk;                // 关联的 gendisk
    struct kref kref;                    // 引用计数
    struct nvme_ns_head *head;           // 指向共享的 ns head
    unsigned long flags;                 // NVME_NS_REMOVING | NVME_NS_READY | ...
};
```

#### 21.2.8 nvme_queue — NVMe 硬件队列（SQ/CQ 元数据）

文件：`drivers/nvme/host/pci.c` L365-L394

```c
/*
 * nvme_queue 封装一个 NVMe Submission Queue (SQ) 和 Completion Queue (CQ) 对。
 * 每个 blk-mq 硬件队列 (hctx) 对应一个 nvme_queue（通过 hctx->driver_data 访问）。
 * SQ 位于主机内存，通过 Doorbell 通知硬件读取；CQ 由硬件写入，中断通知主机。
 *
 * SQ 是环形缓冲区，主机写命令 → 硬件读命令。
 * CQ 是环形缓冲区，硬件写完成条目 → 主机读完成条目。
 */
struct nvme_queue {
    struct nvme_dev *dev;                           // 所属 nvme 设备
    spinlock_t sq_lock;                             // SQ 自旋锁（保护 sq_cmds 写入）
    void *sq_cmds;                                  // SQ 命令缓冲区（DMA 一致性内存，映射到硬件）
    spinlock_t cq_poll_lock;                        // CQ 轮询锁（仅 poll 队列）
    struct nvme_completion *cqes;                   // CQ 完成条目数组（DMA 一致性内存）
    dma_addr_t sq_dma_addr;                         // SQ 的 DMA 地址
    dma_addr_t cq_dma_addr;                         // CQ 的 DMA 地址
    u32 __iomem *db;                                // Doorbell 寄存器地址（MMIO）
    u32 q_depth;                                    // 队列深度（SQ 和 CQ 条目数）
    u16 cq_vector;                                  // 中断向量号
    u16 sq_tail;                                    // SQ 尾指针（主机写入位置）
    u16 last_sq_tail;                               // 上次写 Doorbell 时的 SQ 尾（用于批量优化）
    u16 cq_head;                                    // CQ 头指针（主机读取位置）
    u16 qid;                                        // 队列 ID（0 = Admin Queue）
    u8 cq_phase;                                    // CQ Phase Tag（与 CQE 的 Phase 位比较）
    u8 sqes;                                        // SQ Entry Size (1 << sqes = 条目字节数)
    unsigned long flags;                            // NVMEQ_ENABLED | NVMEQ_SQ_CMB | NVMEQ_POLLED
    __le32 *dbbuf_sq_db;                            // Doorbell Buffer：SQ Tail Doorbell
    __le32 *dbbuf_cq_db;                            // Doorbell Buffer：CQ Head Doorbell
    __le32 *dbbuf_sq_ei;                            // Doorbell Buffer：SQ Event Index
    __le32 *dbbuf_cq_ei;                            // Doorbell Buffer：CQ Event Index
    struct completion delete_done;                   // 队列删除完成信号
};
```

#### 21.2.9 blk_mq_queue_data — blk-mq 派发数据

文件：`include/linux/blk-mq.h`

```c
/*
 * blk_mq_queue_data 是 blk-mq 调用 mq_ops->queue_r() 时传递的参数。
 * 包含要派发的请求 (rq) 和是否为最后一个请求的标志 (last)。
 * last 标志用于 NVMe 驱动的批量 Doorbell 优化：
 *   仅当 last=true 时才写 Doorbell，减少 MMIO 写入次数。
 */
struct blk_mq_queue_data {
    struct request *rq;      // 要派发的请求
    bool last;               // 是否为当前批量中的最后一个请求
};
```

---

### 21.3 详细函数调用栈

#### 21.3.1 用户态到块层：open 路径

```
用户态 open("/dev/nvme0n1", O_RDWR)
  └─ syscall
        └─ file_open() → do_dentry_open()
              └─ blkdev_open(inode, filp)                      [block/fops.c]
                    │
                    ├─ bdev = blkdev_get_by_dev(inode->i_rdev, ...)
                    │     └─ bdev = blkdev_get_no_open(dev, ...) 获取或创建 bdev
                    │
                    ├─ disk->fops->open(bdev, filp->f_mode)    调用 nvme_open()
                    │     └─ nvme_ns_open(disk->private_data)   [core.c]
                    │           ├─ nvme_ns_head_multipath() 检查（多路径盘不可直接打开）
                    │           ├─ nvme_get_ns(ns)           增加 namespace 引用计数
                    │           └─ try_module_get()          防止驱动卸载
                    │
                    └─ filp->f_mapping = bdev->bd_inode->i_mapping  设置 address_space
```

#### 21.3.2 用户态到块层：read 路径（O_DIRECT）

```
用户态 read(fd, buf, len)
  └─ syscall
        └─ ksys_read()
              └─ vfs_read()
                    └─ file->f_op->read_iter()  → blkdev_read_iter()  [block/fops.c]
                          │
                          ├─ blkdev_direct_IO()                     [block/fops.c]
                          │     │
                          │     ├─ __blkdev_direct_IO_simple()     单 bio 路径
                          │     │     ├─ bio_init(&bio, bdev, vecs, nr_pages, REQ_OP_READ)
                          │     │     ├─ bio.bi_iter.bi_sector = pos >> SECTOR_SHIFT
                          │     │     ├─ bio.bi_end_io = blkdev_bio_end_io
                          │     │     ├─ blkdev_iov_iter_get_pages()  pin 用户页
                          │     │     └─ submit_bio_wait(&bio)        提交并等待
                          │     │
                          │     └─ __blkdev_direct_IO()           多 bio 路径
                          │           ├─ 构建 blkdev_dio 结构体
                          │           ├─ 循环：
                          │           │     ├─ bio_alloc() + bio_init()
                          │           │     ├─ blkdev_iov_iter_get_pages()
                          │           │     └─ submit_bio(bio)        提交每个 bio
                          │           ├─ 同步：等待 waiter 被唤醒
                          │           └─ 异步：返回 -EIOCBQUEUED
                          │
                          └─ 返回已读字节数
```

#### 21.3.3 块层 submit_bio 到 NVMe queue_rq

```
submit_bio(bio)                                           [block/blk-core.c]
  │
  ├─ bio_set_ioprio(bio)                                设置 I/O 优先级
  ├─ submit_bio_noacct(bio)                             无统计计数提交
  │     └─ __submit_bio(bio)                             [block/blk-core.c]
  │           ├─ blk_crypto_submit_bio(bio)             内联加密处理
  │           └─ blk_mq_submit_bio(bio)                  [block/blk-mq.c]
  │                 │
  │                 ├─ rq = blk_mq_peek_cached_request() 检查 plug 缓存
  │                 ├─ bio_queue_enter(bio)              获取队列引用
  │                 ├─ 检查对齐、poll 支持
  │                 ├─ __bio_split_to_limits()           按队列限制拆分 bio
  │                 ├─ bio_integrity_prep()              完整性校验准备
  │                 ├─ blk_mq_attempt_bio_merge()        尝试与已有请求合并
  │                 │
  │                 ├─ blk_mq_get_new_requests()         分配新 request
  │                 │     └─ blk_mq_rq_ctx_init()       初始化 request 上下文
  │                 │
  │                 ├─ rq_qos_track(q, rq, bio)          QoS 记录
  │                 ├─ blk_mq_bio_to_request(rq, bio)    绑定 bio 到 request
  │                 ├─ blk_crypto_rq_get_keyslot(rq)     加密密钥槽
  │                 │
  │                 ├─ [有 plug] → blk_add_rq_to_plug(plug, rq)
  │                 │              → 稍后批量由 blk_mq_flush_plug_list() 下发
  │                 │
  │                 └─ [无 plug] → blk_mq_try_issue_directly(hctx, rq)
  │                       └─ blk_mq_request_issue_directly(rq, last)
  │                             └─ __blk_mq_issue_directly(hctx, rq, last)
  │                                   └─ q->mq_ops->queue_rq(hctx, &bd)
  │                                         └─ nvme_queue_rq()          [pci.c]
```

#### 21.3.4 NVMe 驱动：请求提交

```
nvme_queue_rq(hctx, bd)                                    [pci.c]
  │
  ├─ 1. nvmeq = hctx->driver_data                        获取硬件队列
  │     dev = nvmeq->dev
  │     req = bd->rq
  │     iod = blk_mq_rq_to_pdu(req)                      获取 I/O 描述符
  │
  ├─ 2. test_bit(NVMEQ_ENABLED, &nvmeq->flags)           队列是否已启用
  │
  ├─ 3. nvme_check_ready(&dev->ctrl, req, true)          控制器状态检查
  │     └─ 检查 state == LIVE && !resetting
  │
  ├─ 4. nvme_prep_rq(req)                                准备请求
  │     │
  │     ├─ 4a. 重置 iod 字段（flags, nr_descriptors, total_len 等）
  │     │
  │     ├─ 4b. nvme_setup_cmd(ns, req)                  构造 NVMe 命令
  │     │     │
  │     │     ├─ nvme_clear_nvme_request(req)            清除 nvme_request 状态
  │     │     │
  │     │     └─ switch (req_op(req)):
  │     │           ├─ REQ_OP_READ  → nvme_setup_rw(ns, req, cmd, nvme_cmd_read)
  │     │           ├─ REQ_OP_WRITE → nvme_setup_rw(ns, req, cmd, nvme_cmd_write)
  │     │           ├─ REQ_OP_FLUSH → nvme_setup_flush(ns, cmd)
  │     │           ├─ REQ_OP_DISCARD → nvme_setup_discard(ns, req, cmd)
  │     │           └─ REQ_OP_WRITE_ZEROES → nvme_setup_write_zeroes(ns, req, cmd)
  │     │
  │     │     nvme_setup_rw(ns, req, cmd, op):           读写命令构造
  │     │       ├─ 设置 FUA / LR 控制位
  │     │       ├─ cmd->rw.opcode = op (0x02=Read, 0x01=Write)
  │     │       ├─ cmd->rw.nsid = cpu_to_le32(ns->head->ns_id)
  │     │       ├─ cmd->rw.slba = cpu_to_le64(nvme_sect_to_lba(blk_rq_pos(req)))
  │     │       ├─ cmd->rw.length = cpu_to_le16((bytes >> lba_shift) - 1)
  │     │       ├─ cmd->rw.control = cpu_to_le16(control)  (FUA, LR, PRACT, PRCHK)
  │     │       └─ cmd->rw.dsmgmt = cpu_to_le32(dsmgmt)   (预取、写流)
  │     │
  │     ├─ 4c. nvme_map_data(req)                       DMA 数据映射
  │     │     │
  │     │     ├─ 单段路径：nvme_pci_setup_data_simple()
  │     │     │     ├─ dma_map_single() 单段 DMA 映射
  │     │     │     └─ 填充 iod->descriptors[0] (PRP1)
  │     │     │
  │     │     ├─ 多段 PRP 路径：nvme_pci_setup_data_prp()
  │     │     │     ├─ blk_rq_dma_map_iter_start() 遍历 bio 的 DMA 段
  │     │     │     ├─ 第一段 → PRP1
  │     │     │     ├─ 第二段 → PRP2（直接）
  │     │     │     └─ 后续段 → PRP list（分配描述符页）
  │     │     │
  │     │     └─ 多段 SGL 路径：nvme_pci_setup_data_sgl()
  │     │           ├─ 分配 SGL 描述符数组
  │     │           └─ 填充 SGL 描述符（Data Block + Last Segment）
  │     │
  │     ├─ 4d. nvme_map_metadata(req)                   元数据 DMA 映射
  │     │     └─ nvme_pci_setup_meta_iter(req)
  │     │          ├─ blk_rq_integrity_dma_map_iter_start() 遍历完整性段
  │     │          ├─ 单段 → 直接使用 meta_dma
  │     │          └─ 多段 → 分配 meta SGL 描述符
  │     │
  │     └─ 4e. nvme_start_request(req)                  设置超时 & 统计
  │           ├─ blk_mq_start_request(req)              记录开始时间戳
  │           └─ nvme_req(req)->timeout = 超时时间
  │
  ├─ 5. spin_lock(&nvmeq->sq_lock)                      获取 SQ 锁
  │
  ├─ 6. nvme_sq_copy_cmd(nvmeq, &iod->cmd)              拷贝命令到 SQ
  │     │
  │     └─ memcpy(nvmeq->sq_cmds + (sq_tail << sqes), cmd, sizeof(*cmd))
  │         // sq_cmds 是 DMA 一致性内存，映射到硬件 SQ
  │         nvmeq->sq_tail++                            推进 SQ 尾指针
  │
  ├─ 7. nvme_write_sq_db(nvmeq, bd->last)              写 Doorbell
  │     │
  │     ├─ 如果 bd->last == false && 未到 last_sq_tail:
  │     │     return                                    延迟写 DB（批量优化）
  │     │
  │     └─ writel(nvmeq->sq_tail, nvmeq->q_db)         写 SQ Tail Doorbell
  │         // 硬件收到 Doorbell 后开始处理 SQ 中的命令
  │
  └─ 8. spin_unlock(&nvmeq->sq_lock)
```

**SQ 命令拷贝与 Doorbell 机制详解**（[pci.c](file:///home/louis/code/linux/drivers/nvme/host/pci.c)）：

```c
/*
 * nvme_sq_copy_cmd — 将 64 字节 NVMe 命令拷贝到 SQ 环形缓冲区。
 * sq_cmds 是 DMA 一致性内存（dma_alloc_coherent），硬件可直接访问。
 * sqes 是 SQ Entry Size 的 log2 值（通常为 6，即 64 字节）。
 */
static inline void nvme_sq_copy_cmd(struct nvme_queue *nvmeq,
                    struct nvme_command *cmd)
{
    memcpy(nvmeq->sq_cmds + (nvmeq->sq_tail << nvmeq->sqes),
        absolute_pointer(cmd), sizeof(*cmd));
    if (++nvmeq->sq_tail == nvmeq->q_depth)
        nvmeq->sq_tail = 0;  // 环形缓冲区回绕
}

/*
 * nvme_write_sq_db — 写 SQ Tail Doorbell 通知硬件有新命令。
 * 批量优化：仅在 bd->last == true 或 SQ 尾指针回绕到 last_sq_tail 时才写 DB。
 * 如果支持 Doorbell Buffer（dbbuf），使用 dbbuf 机制代替 MMIO 写入，
 * 进一步减少 PCIe 事务。
 */
static inline void nvme_write_sq_db(struct nvme_queue *nvmeq, bool write_sq)
{
    if (!write_sq) {
        // 批量模式：延迟写 DB，直到 SQ 尾指针与上次写 DB 的位置一致
        u16 next_tail = nvmeq->sq_tail + 1;
        if (next_tail == nvmeq->q_depth)
            next_tail = 0;
        if (next_tail != nvmeq->last_sq_tail)
            return;  // 尚未追上，继续延迟
    }

    // Doorbell Buffer 优化：仅在 event index 超限时才写 MMIO
    if (nvme_dbbuf_update_and_check_event(nvmeq->sq_tail,
            nvmeq->dbbuf_sq_db, nvmeq->dbbuf_sq_ei))
        writel(nvmeq->sq_tail, nvmeq->q_db);  // 写 MMIO Doorbell 寄存器
    nvmeq->last_sq_tail = nvmeq->sq_tail;
}
```

#### 21.3.5 NVMe 驱动：中断处理与完成

```
════════════════════════ 硬件完成 I/O，发送 MSI-X 中断 ════════════════════════

nvme_irq(irq, data)                                        [pci.c]
  │
  ├─ nvmeq = data                                        获取队列
  ├─ DEFINE_IO_COMP_BATCH(iob)                           定义完成批处理
  │
  └─ nvme_poll_cq(nvmeq, &iob)                           轮询完成队列
        │
        └─ while (nvme_cqe_pending(nvmeq)):              CQE 有效（Phase Tag 匹配）
              │
              ├─ 1. dma_rmb()                            DMA 读屏障
              │
              ├─ 2. nvme_handle_cqe(nvmeq, iob, cq_head) 处理单个 CQE
              │     │
              │     ├─ cqe = &nvmeq->cqes[cq_head]       获取 CQE 条目
              │     ├─ command_id = cqe->command_id      命令 ID = tag
              │     │
              │     ├─ [AEN] → nvme_complete_async_event() 异步事件特殊处理
              │     │
              │     └─ [普通命令]:
              │           ├─ req = nvme_find_rq(tagset, command_id)  通过 tag 找到 request
              │           │
              │           └─ nvme_try_complete_req(req, status, result)
              │                 │
              │                 ├─ 设置 nvme_req(req)->result = result
              │                 ├─ 设置 nvme_req(req)->status = status
              │                 ├─ blk_mq_complete_request_remote(req) 跨 CPU 完成
              │                 │     └─ blk_mq_complete_request(req)
              │                 │           └─ blk_mq_complete_request_remote_cpu()
              │                 │
              │                 └─ [或直接] nvme_pci_complete_rq(req)
              │
              ├─ 3. nvme_update_cq_head(nvmeq)           更新 CQ 头指针
              │     └─ cq_head++; if (cq_head == q_depth) { cq_head=0; cq_phase^=1; }
              │
              └─ 4. 循环回到步骤 1

nvme_ring_cq_doorbell(nvmeq)                               写 CQ Head Doorbell
  └─ writel(nvmeq->cq_head, nvmeq->q_db + db_stride)
      // 通知硬件 CQ 条目已被消费，可复用

nvme_pci_complete_rq(req)                                  完成请求处理
  │
  ├─ nvme_pci_unmap_rq(req)                              DMA 反映射
  │     ├─ blk_rq_nr_phys_segments(req) → nvme_unmap_data(req)
  │     │     ├─ blk_rq_dma_unmap() 释放 DMA 映射
  │     │     └─ 释放 PRP list / SGL 描述符
  │     └─ blk_integrity_rq(req) → nvme_unmap_metadata(req)
  │
  └─ nvme_complete_rq(req)                               [core.c]
        │
        ├─ nvme_cleanup_cmd(req)                         清理命令（释放特殊载荷）
        ├─ nvme_decide_disposition(req):                 决定处理方式
        │     ├─ status == 0 → COMPLETE                  正常完成
        │     ├─ 可重试错误 → RETRY                       重新排队
        │     ├─ 路径错误 → FAILOVER                      切换到备份路径
        │     └─ 认证错误 → AUTHENTICATE                  重新认证
        │
        └─ [COMPLETE] nvme_end_req(req)
              ├─ nvme_end_req_zoned(req)                 Zone Append 特殊处理
              ├─ nvme_trace_bio_complete(req)            追踪
              └─ blk_mq_end_request(req, status)         块层完成
                    └─ blk_update_request()             更新统计
                    └─ __blk_mq_end_request()
                          └─ blk_mq_free_request(rq)     释放 tag
                          └─ bio_endio(bio)              调用 bio->bi_end_io()
                                └─ blkdev_bio_end_io(bio)  [block/fops.c]
                                      ├─ 同步: 唤醒 waiter (wake_up_process)
                                      └─ 异步: dio->iocb->ki_complete(iocb, ret)
```

---

### 21.4 数据拷贝路径

#### 21.4.1 读操作数据流

```
NVMe 设备 DMA → 主机物理内存 → 用户态缓冲区

1. 硬件 DMA 写入主机物理内存
   └─ NVMe SSD 通过 PCIe 将数据 DMA 到主机内存
      └─ 目标地址由 nvme_map_data() 中的 dma_map_single/page() 确定

2. blkdev_iov_iter_get_pages() 预先 pin 用户页
   └─ bio_iov_iter_get_pages() → __bio_iov_iter_get_pages()
      └─ 将用户态虚拟地址对应的物理页 pin 住
      └─ 填充到 bio->bi_io_vec[] 中

3. 硬件 DMA 直接写入用户页
   └─ 零拷贝：硬件 DMA 直接写入用户态缓冲区
   └─ 无需内核中转拷贝

4. 完成时 bio_release_pages()
   └─ 释放 pin 的页
   └─ 如果设置了 DIO_SHOULD_DIRTY → bio_set_pages_dirty()
```

#### 21.4.2 写操作数据流

```
用户态缓冲区 → 主机物理内存 → NVMe 设备 DMA

1. blkdev_iov_iter_get_pages() 预先 pin 用户页
   └─ 同读操作，pin 住用户态缓冲区的物理页

2. nvme_map_data() 建立 DMA 映射
   └─ dma_map_single() / dma_map_page() 建立 DMA 地址映射
   └─ 填充 PRP1/PRP2 或 PRP list 或 SGL

3. 硬件 DMA 从主机内存读取数据
   └─ NVMe SSD 通过 PCIe 从主机内存 DMA 读取数据

4. 完成时 DMA 反映射
   └─ nvme_unmap_data() → dma_unmap_single/page()
   └─ bio_release_pages() 释放 pin
```

---

### 21.5 PRP 与 SGL 数据描述符

NVMe 协议支持两种数据描述符格式，用于告诉硬件数据在主机内存中的位置：

| 特性 | PRP (Physical Region Page) | SGL (Scatter Gather) |
|------|---------------------------|--------------------------|
| **定义** | NVMe 原生格式，由 PR1/PR2 + PRP list 组成 | NVMe 1.2+ 引入的通用格式 |
| **描述** | 每项描述一个物理页（4KB 对齐） | 每项描述任意长度的数据段 |
| **效率** | 小 I 高效（1-2 页直接入命令） | 大 I 更灵活（段数少） |
| **限制** | 每页偏移必须一致 | 每个段可独立指定偏移和长度 |
| **使用条件** | 默认使用 | 启用 SGL 或超过阈值（`sgl_threshold`）时 |

**数据映射决策流程**（`nvme_map_data()`，[pci.c](:///drivers/nvme/host/pci.c)）：

```
nvme_map_data(req)
  │
  ├─ blk_r_nr_phys_segments(req) == 1 → nvme_pci_setup_data_simple()
  │                                       单段快速路径，原地完成
  │
  ├─ blk_r_dma_map_iter_start() → 遍历 bio 的 DMA 段，建立映射
  │
  └─ 判断使用 PRP 还是 SGL：
        ├─ use_sgl == SGL_FORCED → nvme_pci_setup_data_sgl()
        ├─ use_sgl == SGL_SUPPORTED && 平均段大小 >= sgl_threshold → nvme_pci_setup_data_sgl()
        └─ 否则 → nvme_pci_setup_data_prp()
```

#### 21.5.1 PRP 单段映射（nvme_pci_setup_data_simple）

适用场景：I/O 请求只有一个物理段（常见于 4KB 小块 I/O）。

```c
/*
 * 单段快速路径：跳过 DMA 迭代器，直接处理单个 bio_vec。
 * 如果数据跨页边界但 ≤ 2 页，使用 PRP1+PRP2 直接描述。
 * 如果必须使用 SGL，则填充 SGL 描述符。
 * 返回 BLK_STS_AGAIN 表示需要走多段路径。
 */
static blk_status_t nvme_pci_setup_data_simple(struct request *req,
        enum nvme_use_sgl use_sgl)
{
    struct bio_vec bv = req_bvec(req);
    unsigned int prp1_offset = bv.bv_offset & (NVME_CTRL_PAGE_SIZE - 1);
    bool prp_possible = prp1_offset + bv.bv_len <= NVME_CTRL_PAGE_SIZE * 2;
    dma_addr_t dma_addr;

    // 不能使用 PRP 描述 → 返回 AGAIN 走多段路径
    if (!use_sgl && !prp_possible)
        return BLK_STS_AGAIN;
    if (is_pci_p2pdma_page(bv.bv_page))
        return BLK_STS_AGAIN;

    dma_addr = dma_map_bvec(dev->dev, &bv, rq_dma_dir(req), 0);

    // SGL 路径：填充 SGL 描述符
    if (use_sgl == SGL_FORCED || !prp_possible) {
        iod->cmd.common.flags = NVME_CMD_SGL_METABUF;
        iod->cmd.common.dptr.sgl.addr = cpu_to_le64(dma_addr);
        iod->cmd.common.dptr.sgl.length = cpu_to_le32(bv.bv_len);
        iod->cmd.common.dptr.sgl.type = NVME_SGL_FMT_DATA_DESC << 4;
    }
    // PRP 路径：PRP1 + 可选 PRP2
    else {
        unsigned int first_prp_len = NVME_CTRL_PAGE_SIZE - prp1_offset;
        iod->cmd.common.dptr.prp1 = cpu_to_le64(dma_addr);
        iod->cmd.common.dptr.prp2 = 0;
        if (bv.bv_len > first_prp_len)
            iod->cmd.common.dptr.prp2 =
                cpu_to_le64(dma_addr + first_prp_len);
    }
    iod->flags |= IOD_SINGLE_SEGMENT;
    return BLK_STS_OK;
}
```

#### 21.5.2 PRP 多段映射（nvme_pci_setup_data_prp）

适用场景：多个物理段，不使用 SGL 时的默认路径。

```
nvme_pci_setup_data_prp(req, iter)
  │
  ├─ 第一段 → PR1 (cmd->common.d.prp1)
  │
  ├─ 第二段：直接写入 PR2 (cmd->common.d.prp2)
  │
  ├─ 后续段 (3+)：
  │     ├─ 分配 PRP list 页（从 descriptor_pools 小池或大池分配）
  │     │     └─ dma_pool_alloc() → 获取 DMA 一致性内存页
  │     ├─ PR2 = PRP list 的 DMA 地址
  │     └─ 循环填充 PRP list 条目
  │
  └─ 设置 iod->nr_descriptors, iod->total_len
```

#### 21.5.3 SGL 映射（nvme_pci_setup_data_sgl）

适用场景：启用 SGL，或分段数超过阈值。

```
nvme_pci_setup_data_sgl(req, iter)
  │
  ├─ 计算需要的 SGL 段数 (nr_entries)
  ├─ 分配 SGL 描述符数组（dma_pool_alloc）
  ├─ 设置 cmd->common.d.sgl = SGL 描述符的 DMA 地址
  ├─ cmd->common.flags |= NVME_CMD_SGL_METABUF
  │
  └─ 循环填充 SGL 描述符：
        ├─ nvme_pci_sgl_set_data(&sg_list[i], iter)  Data Block 描述符
        └─ 最后一个段 → Last Segment 描述符
```

**PRP 布局**：

```
单页 (≤ 1 页):   PRP1 = 页地址, PRP2 = 0
两页 (≤ 2 页):   PRP1 = 页1地址, PRP2 = 页2地址
多页 (> 2 页):   PRP1 = 页1地址, PRP2 = PRP List 地址
                  PRP List = [页2地址, 页3地址, 页4地址, ...]
```

**SGL 布局**：

```
SGL Segment 1:  {地址1, 长度1, SGL_DATA_BLOCK}
SGL Segment 2:  {地址2, 长度2, SGL_DATA_BLOCK}
...
SGL Last Segment: {地址N, 长度N, SGL_LAST_SEGMENT}
```

---

### 21.6 完成路径详解

#### 21.6.1 nvme_handle_cqe — CQE 处理

文件：`drivers/nvme/host/pci.c` L1531-L1576

```c
/*
 * nvme_handle_cqe 在中断处理中消费单个 CQE 条目。
 * 核心逻辑：
 *   1. 从 CQE 读取 command_id → 通过 tag 找到对应的 request
 *   2. 尝试同步完成 (nvme_try_complete_re)
 *   3. 否则加入 IO 完成批处理 (blk_m_add_to_batch)
 *   4. 无法加入批处理 → 直接调用 nvme_pci_complete_r
 */
static inline void nvme_handle_cqe(struct nvme_queue *nvme,
            struct io_comp_batch *iob, u16 idx)
{
    struct nvme_completion *cqe = &nvme->cqes[idx];
    __u16 command_id = READ_ONCE(cqe->command_id);
    struct request *req;

    // AEN (Async Event Notification) 特殊处理：没有对应的 request
    if (unlikely(nvme_is_aen_re(nvme->qid, command_id))) {
        nvme_complete_async_event(...);
        return;
    }

    // 通过 tag 找到对应的 request
    req = nvme_find_r(nvme_queue_tagset(nvme), command_id);
    if (unlikely(!req)) {
        dev_warn(..., "invalid id %d completed\n", command_id);
        return;
    }

    // 尝试同步完成，否则加入批处理
    if (!nvme_try_complete_req(req, cqe->status, cqe->result) &&
        !blk_mq_add_to_batch(req, iob,
                   nvme_req(req)->status != NVME_SC_SUCCESS,
                   nvme_pci_complete_batch))
        nvme_pci_complete_rq(req);
}
```

**nvme_find_rq — Tag 匹配与 Genctr 校验**（[nvme.h](file:///home/louis/code/linux/drivers/nvme/host/nvme.h)）：

```c
/*
 * command_id 编码：[genctr(8bit)][tag(16bit)]
 * genctr 用于防止 tag 重用导致的 ABA 问题：
 *   每次分配新 tag 时 genctr 递增，CQE 中携带生成时的 genctr，
 *   匹配时校验 genctr 确保 request 没有被重新分配。
 */
static inline struct request *nvme_find_rq(struct blk_mq_tags *tags,
        u16 command_id)
{
    u8 genctr = nvme_genctr_from_cid(command_id);  // 提取 genctr
    u16 tag = nvme_tag_from_cid(command_id);        // 提取 tag

    rq = blk_mq_tag_to_rq(tags, tag);               // 通过 tag 查找 request
    if (unlikely(!rq))
        return NULL;

    // ABA 防护：校验 genctr 是否匹配
    if (unlikely(nvme_genctr_mask(nvme_req(rq)->genctr) != genctr))
        return NULL;  // genctr 不匹配，说明 request 已被重新分配

    return rq;
}
```

**nvme_try_complete_req — 跨 CPU 完成**（[nvme.h](file:///home/louis/code/linux/drivers/nvme/host/nvme.h)）：

```c
/*
 * 尝试在当前 CPU 上完成请求。如果 request 的 CPU 亲和性与当前 CPU 不匹配，
 * blk_mq_complete_request_remote 会通过 IPI 将完成工作派发到目标 CPU。
 * 返回值：true = 已成功完成，false = 需要在当前上下文继续处理。
 */
static inline bool nvme_try_complete_req(struct request *req, __le16 status,
        union nvme_result result)
{
    rq->genctr++;                             // 递增 genctr（用于 ABA 防护）
    rq->status = le16_to_cpu(status) >> 1;   // 解析状态码（去掉 Phase 位）
    rq->result = result;                      // 保存完成结果
    nvme_should_fail(req);                    // 故障注入检查
    if (unlikely(blk_should_fake_timeout(req->q)))
        return true;                          // 假超时，不真正完成
    return blk_mq_complete_request_remote(req); // 远程完成（如需要）
}
```

#### 21.6.2 nvme_complete_rq — NVMe 完成处理

文件：`drivers/nvme/host/core.c` L457-L496

```
nvme_complete_rq(req)                            [core.c]
  │
  ├─ trace_nvme_complete_rq(req)                 tracepoint
  ├─ nvme_cleanup_cmd(req)                     清理特殊载荷（discard page 等）
  │
  └─ switch (nvme_decide_disposition(req)):
        ├─ COMPLETE:   nvme_end_req(req)          → blk_mq_end_request()
        ├─ RETRY:      nvme_retry_req(req)        重新排队
        ├─ FAILOVER:   nvme_failover_req(req)     切换到备份路径
        └─ AUTHENTICATE: 重新认证 + 重试
```

**nvme_decide_disposition 决策逻辑**（[core.c](file:///home/louis/code/linux/drivers/nvme/host/core.c)）：

```
nvme_decide_disposition(req):
  │
  ├─ status == 0                                    → COMPLETE（正常完成）
  │
  ├─ blk_noretry_request(req)                       → COMPLETE（禁止重试）
  │  status & NVME_STATUS_DNR                       → COMPLETE（Do Not Retry）
  │  retries >= nvme_max_retries                    → COMPLETE（超过重试上限）
  │
  ├─ status & NVME_SCT_SC_MASK == NVME_SC_AUTH_REQUIRED  → AUTHENTICATE
  │
  ├─ req->cmd_flags & REQ_NVME_MPATH (多路径请求):
  │     ├─ nvme_is_path_error(status)               → FAILOVER
  │     └─ blk_queue_dying(req->q)                  → FAILOVER
  │
  ├─ !多路径请求:
  │     └─ blk_queue_dying(req->q)                  → COMPLETE（队列已销毁，放弃）
  │
  └─ 其他所有情况                                    → RETRY
```

**关键点**：
- `NVME_STATUS_DNR`（Do Not Retry）位由硬件设置，指示该错误不可重试
- `nvme_max_retries` 默认值为 5，可通过模块参数 `max_retries` 调整
- 多路径模式下，路径错误会触发 FAILOVER 切换到备用路径；非多路径模式则直接返回错误

#### 21.6.3 nvme_end_req — 最终完成块层请求

```c
void nvme_end_req(struct request *req)
{
    blk_status_t status = nvme_status(nvme_re(req)->status);

    __nvme_end_req(req);       // 错误日志 + Zone Append 处理 + bio trace
    blk_mq_end_request(req, status);  // → blk_update_request() → bio_endio()
}
```

---

### 21.7 NVMe 驱动：open/release 详细流程

#### 21.7.1 open 路径

```
用户态 open("/dev/nvme0n1", O_RDWR)
  └─ syscall
        └─ file_open_name() → do_dentry_open()
              └─ blkdev_open(inode, filp)                        [block/fops.c L674]
                    │
                    ├─ 1. file_to_blk_mode(filp)  将文件标志转为块设备打开模式
                    │     └─ FMODE_READ → BLK_OPEN_READ
                    │     └─ FMODE_WRITE → BLK_OPEN_WRITE
                    │     └─ O_EXCL → BLK_OPEN_EXCL
                    │
                    ├─ 2. bdev_permission(dev, mode, holder)  权限检查
                    │
                    ├─ 3. blkdev_get_no_open(dev, true)  获取/创建 bdev
                    │     └─ 查找或创建 block_device 实例
                    │
                    ├─ 4. bdev_open(bdev, mode, holder, NULL, filp)
                    │     │
                    │     ├─ blkdev_get_whole(bdev, mode, holder)
                    │     │     ├─ 增加 bdev->bd_holders（排他打开检查）
                    │     │     └─ disk->fops->open(disk, mode)  → nvme_open()
                    │     │
                    │     └─ bdev_set_file_inode(filp, bdev)
                    │           └─ filp->f_mapping = bdev->bd_inode->i_mapping
                    │
                    └─ 5. 设置文件能力标志
                          ├─ 支持 atomic write → FMODE_CAN_ATOMIC_WRITE
                          └─ 有 integrity → FMODE_HAS_METADATA

nvme_open(disk, mode)                                          [core.c L1801]
  └─ nvme_ns_open(disk->private_data)                          [core.c L1775]
        ├─ WARN_ON_ONCE(nvme_ns_head_multipath(ns->head))  多路径盘不可直接打开
        ├─ nvme_get_ns(ns)                                  增加 namespace 引用计数
        └─ try_module_get(ns->ctrl->ops->module)             防止驱动模块卸载
```

#### 21.7.2 release 路径

```
用户态 close(fd)
  └─ syscall
        └─ __close_fd() → filp_close()
              └─ blkdev_release(inode, filp)                    [block/fops.c L731]
                    └─ bdev_release(filp)
                          ├─ blkdev_put_whole(bdev_whole(bdev))
                          │     ├─ disk->fops->release(disk)  → nvme_release()
                          │     └─ 减少 bd_holders
                          └─ blkdev_put_no_open(bdev)
                                └─ 减少引用计数，可能释放 bdev

nvme_release(disk)                                             [core.c L1806]
  └─ nvme_ns_release(disk->private_data)                      [core.c L1797]
        ├─ module_put(ns->ctrl->ops->module)                  允许驱动模块卸载
        └─ nvme_put_ns(ns)                                   减少 namespace 引用计数
```

---

### 21.8 批量提交优化

#### 21.8.1 plug 机制

```c
/* 用户态通过 io_uring 或 libaio 批量提交时 */

struct blk_plug *plug = current->plug;
// 如果 plug 存在，多个 request 先缓存到 plug 中
blk_add_rq_to_plug(plug, rq);   // 不会立即下发

// 当 plug 被 flush 时，批量下发
blk_mq_flush_plug_list(plug, ...)
  └─ blk_mq_issue_direct(rqs)    批量下发
        └─ 对每个 request:
              └─ blk_mq_request_issue_directly(rq, last)
                    └─ nvme_queue_rq(hctx, &bd)
                          └─ nvme_write_sq_db(nvmeq, bd->last)
                              // bd->last=true 才写 DB，减少 DB 写入次数
```

#### 21.8.2 nvme_queue_rqs 批量提交

```
nvme_queue_rqs(rqlist)                                    [pci.c]
  │
  ├─ 遍历 rqlist，按队列分组
  │     ├─ 对每个 rq：nvme_prep_rq_batch(nvmeq, req)    准备命令
  │     └─ 收集到 submit_list
  │
  └─ nvme_submit_cmds(nvmeq, &submit_list)              批量提交
        ├─ spin_lock(&nvmeq->sq_lock)
        ├─ while (rq = rq_list_pop(rqlist)):
        │     nvme_sq_copy_cmd(nvmeq, &iod->cmd)         拷贝命令到 SQ
        └─ nvme_write_sq_db(nvmeq, true)                 一次写 DB
        spin_unlock(&nvmeq->sq_lock)
```

#### 21.8.3 完成批处理

```c
// 中断处理中批量消费 CQE
DEFINE_IO_COMP_BATCH(iob);       // 定义完成批处理

nvme_poll_cq(nvmeq, &iob)
  └─ while (nvme_cqe_pending(nvmeq)):
        nvme_handle_cqe(nvmeq, &iob, nvmeq->cq_head)
          └─ nvme_try_complete_req(req, status, result)
                └─ blk_mq_add_to_batch(req, &iob, ...)   加入批处理
                      // 多个 CQE 收集到 iob 中

// 退出循环后统一处理
nvme_pci_complete_batch(&iob)
  └─ nvme_complete_batch(&iob, nvme_pci_unmap_rq)
        └─ 遍历 iob 中的请求，批量完成
```

---

### 21.9 涉及的文件清单

| 文件 | 作用 |
|------|------|
| `block/fops.c` | 块设备文件操作：`blkdev_open()`, `blkdev_read_iter()`, `blkdev_write_iter()` |
| `block/blk-core.c` | `submit_bio()`, `submit_bio_noacct()` |
| `block/blk-mq.c` | `blk_mq_submit_bio()`, `blk_mq_dispatch_rq_list()`, `blk_mq_request_issue_directly()` |
| `drivers/nvme/host/pci.c` | `nvme_queue_rq()`, `nvme_queue_rqs()`, `nvme_prep_rq()`, `nvme_map_data()`, `nvme_irq()`, `nvme_poll_cq()`, `nvme_handle_cqe()` |
| `drivers/nvme/host/core.c` | `nvme_setup_cmd()`, `nvme_setup_rw()`, `nvme_complete_rq()`, `nvme_end_req()`, `nvme_open()`, `nvme_release()` |
| `drivers/nvme/host/nvme.h` | `struct nvme_request`, `struct nvme_command`, `struct nvme_rw_command` |

---