# NVMe 数据结构定义

## 1. 概述

NVMe 的数据结构分为两层：

- **协议结构**：定义在 `include/linux/nvme.h`，直接对应 NVMe 规范中的命令、完成项、Identify 返回体；
- **驱动结构**：定义在 `drivers/nvme/host/`，描述 Linux 驱动如何管理控制器、队列与请求。

本文只列核心结构，字段含义以 NVMe 规范与内核注释为准。

---

## 2. 命令结构

### 2.1 nvme_command

```c
// include/linux/nvme.h:1992
struct nvme_command {
    union {
        struct nvme_common_command common;
        struct nvme_rw_command     rw;
        struct nvme_identify       identify;
        struct nvme_features       features;
        struct nvme_create_cq      create_cq;
        struct nvme_create_sq      create_sq;
        struct nvme_delete_queue   delete_queue;
        struct nvme_download_firmware dlfw;
        struct nvme_format_cmd     format;
        struct nvme_dsm_cmd        dsm;
        struct nvme_write_zeroes_cmd write_zeroes;
        struct nvme_zone_mgmt_send_cmd zms;
        struct nvme_zone_mgmt_recv_cmd zmr;
        struct nvme_abort_cmd      abort;
        struct nvme_get_log_page_command get_log_page;
        struct nvmf_common_command fabrics;
        struct nvmf_connect_command connect;
        /* ... */
        struct nvme_dbbuf          dbbuf;
        struct nvme_directive_cmd  directive;
        struct nvme_io_mgmt_recv_cmd imr;
    };
};
```

所有成员共享 64 字节布局，因此可以直接 memcpy 到 SQ 槽位；用哪个成员解释由 `opcode` 决定。

### 2.2 公共命令字段

```c
// include/linux/nvme.h:1082
struct nvme_common_command {
    __u8  opcode;
    __u8  flags;
    __u16 command_id;
    __le32 nsid;
    __le32 cdw2[2];
    __le64 metadata;
    union nvme_data_ptr dptr;
    __le32 cdw10;
    __le32 cdw11;
    __le32 cdw12;
    __le32 cdw13;
    __le32 cdw14;
    __le32 cdw15;
};
```

| 字段 | 说明 |
|------|------|
| `opcode` | 操作码（Admin 或 I/O） |
| `flags` | bit7:0 = FUSE，bit7:6 = PSDT（数据指针类型） |
| `command_id` | 主机分配，完成项原样返回，用于匹配请求 |
| `nsid` | 命名空间 ID；Admin 命令可为 0 或 `0xFFFFFFFF` |
| `dptr` | PRP1/PRP2 或 SGL 描述符 |
| `cdw10~cdw15` | 命令相关参数 |

### 2.3 读写命令

```c
// include/linux/nvme.h:1100
struct nvme_rw_command {
    __u8   opcode;
    __u8   flags;
    __u16  command_id;
    __le32 nsid;
    __le32 cdw2;
    __le32 cdw3;
    __le64 metadata;
    union nvme_data_ptr dptr;
    __le64 slba;       // 起始逻辑块地址
    __le16 length;     // 传输块数 - 1
    __le16 control;    // FUA / LR / PRINFO 等
    __le32 dsmgmt;
    __le32 reftag;
    __le16 lbat;
    __le16 lbatm;
};
```

---

## 3. 完成项

```c
// include/linux/nvme.h:2252
struct nvme_completion {
    union nvme_result {
        __le16 u16;
        __le32 u32;
        __le64 u64;
    } result;
    __le16 sq_head;     // SQ 中可回收的位置
    __le16 sq_id;
    __u16  command_id;
    __le16 status;
};
```

`status` 的位布局：

| 位 | 含义 |
|----|------|
| bit 0 | Phase Tag（P）：与主机期望值一致才表示新完成项 |
| bit 8:1 | Status Code（SC） |
| bit 11:9 | Status Code Type（SCT）：0=通用，1=命令特定，2=媒体错误，7=厂商 |
| bit 15:12 | 保留 |

内核封装了 `nvme_is_error()` 等辅助函数，并可通过 `CONFIG_NVME_VERBOSE_ERRORS` 输出可读错误字符串。

---

## 4. Identify 结构

### 4.1 控制器信息

```c
// include/linux/nvme.h:309（节选）
struct nvme_id_ctrl {
    __le16 vid;        // PCI Vendor ID
    __le16 ssvid;      // 子系统 Vendor ID
    char   sn[20];     // 序列号
    char   mn[40];     // 型号
    char   fr[8];      // 固件版本
    __u8   mdts;       // 最大数据传输大小（2^mdts × 页大小）
    __le16 cntlid;     // 控制器 ID
    __le32 ver;        // NVMe 版本
    __u8   elpe;       // 错误日志页数
    __u8   npss;       // 电源状态数
    __le16 wctemp;     // 告警温度
    __le16 cctemp;     // 临界温度
    __u8   tnvmcap[16];// 总容量（字节）
    __u8   unvmcap[16];// 未分配容量
    /* ... */
};
```

### 4.2 命名空间信息

```c
// include/linux/nvme.h:434（节选）
struct nvme_id_ns {
    __le64 nsze;        // 命名空间总块数
    __le64 ncap;        // 可用块数
    __le64 nuse;        // 已使用块数
    __u8   nlbaf;       // LBA 格式数量 - 1
    __u8   flbas;       // 当前 LBA 格式索引
    __u8   mc;          // 元数据能力
    __u8   dlfeat;      // 数据擦除特性
    __u8   nvmcap[16];  // 命名空间容量
    __le16 npwg;        // 优选写粒度
    __le16 npwa;        // 优选写对齐
    __u8   nguid[16];
    __u8   eui64[8];
    struct nvme_lbaf lbaf[64];

    /* ... */
};

struct nvme_lbaf {
    __le16 ms;          // 元数据大小
    __u8   ds;          // LBA 数据大小（2^ds）
    __u8   rp;          // 相对性能
};
```

`flbas & 0xf` 选择 `lbaf[]` 中的一项，`ds` 决定逻辑块大小（如 `ds=9` 表示 512B，`ds=12` 表示 4KB）。

---

## 5. 数据指针

```c
// include/linux/nvme.h:1050
union nvme_data_ptr {
    struct {
        __le64 prp1;
        __le64 prp2;
    };
    struct nvme_sgl_desc      sgl;
    struct nvme_keyed_sgl_desc ksgl;
};
```

| 结构 | 用途 |
|------|------|
| `prp1` / `prp2` | 物理页描述符；PRP2 可指向 PRP List 页 |
| `nvme_sgl_desc` | 普通 SGL 段（地址 + 长度 + 类型） |
| `nvme_keyed_sgl_desc` | 带 Key 的 SGL（如 NVMe over Fabrics） |

---

## 6. 驱动侧结构

### 6.1 nvme_dev（PCIe 传输层）

```c
// drivers/nvme/host/pci.c:294（节选）
struct nvme_dev {
    struct nvme_queue *queues;         // 队列数组：[0]=Admin，其余为 I/O
    struct blk_mq_tag_set tagset;      // I/O 队列标签集
    struct blk_mq_tag_set admin_tagset;
    u32 __iomem *dbs;                  // Doorbell 基址
    struct device *dev;
    unsigned online_queues;
    unsigned max_qid;
    unsigned io_queues[HCTX_MAX_TYPES];
    u32 q_depth;
    u32 db_stride;                     // 每个队列 Doorbell 的跨度
    void __iomem *bar;                 // BAR0 映射
    struct nvme_ctrl ctrl;             // 通用控制器对象
    /* ... */
};
```

### 6.2 nvme_queue

```c
// drivers/nvme/host/pci.c:365（节选）
struct nvme_queue {
    struct nvme_dev *dev;
    spinlock_t sq_lock;
    void *sq_cmds;                     // SQ 虚拟地址
    struct nvme_completion *cqes;      // CQ 虚拟地址
    dma_addr_t sq_dma_addr;
    dma_addr_t cq_dma_addr;
    u32 __iomem *q_db;                 // 该队列的 Doorbell
    u32 q_depth;
    u16 cq_vector;                     // MSI-X 向量
    u16 sq_tail;                       // 主机写指针
    u16 last_sq_tail;
    u16 cq_head;                       // 主机读指针
    u16 qid;
    u8  cq_phase;                      // 期望的 Phase Tag
    u8  sqes;                          // SQ 项大小（2^sqes × 64B）
};
```

`cq_phase` 是完成项消费的核心：控制器每绕一圈 CQ 就翻转一次 phase，主机据此区分新完成项与上轮残留。

---

## 7. 队列与 Doorbell 布局

```text
BAR0
 ├─ 0x0000: CAP / VS / INTMS / INTMC / CC / CSTS / AQA / ASQ / ACQ
 ├─ 0x1000: SQ0 Tail Doorbell
 ├─ 0x1004: CQ0 Head Doorbell
 ├─ 0x1008: SQ1 Tail Doorbell
 ├─ 0x100c: CQ1 Head Doorbell
 └─ ...                                    // 每对队列间隔 8 字节（db_stride）

内存
 ├─ Admin SQ (ASQ)：64 字节 × 队列深度
 ├─ Admin CQ (ACQ)：16 字节 × 队列深度
 └─ I/O SQ/CQ：由 Create CQ/SQ 命令分配
```

主机只写 **SQ Tail** 与 **CQ Head** 两个 Doorbell；SQ Head 与 CQ Tail 由控制器维护，并通过 CQE 的 `sq_head` 字段回传给主机。

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `include/linux/nvme.h` | NVMe 协议结构、操作码、状态码 |
| `drivers/nvme/host/nvme.h` | `struct nvme_ctrl`、`nvme_ns`、请求结构 |
| `drivers/nvme/host/pci.c` | `struct nvme_dev`、`struct nvme_queue` |
| `drivers/nvme/host/core.c` | 命令构造与命名空间管理 |

相关文档：[NVMe 设备基本原理](arch.md)、[NVMe 命令](cmd.md)、[NVMe 简介](intro.md)。
