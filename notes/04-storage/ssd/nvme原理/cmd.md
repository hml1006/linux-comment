# NVMe 命令

## 1. 命令集概述

NVMe 把主机与 SSD 之间的交互抽象为“命令 / 完成”对，分为两个命令集：

| 命令集 | 队列 | 用途 |
|--------|------|------|
| Admin 命令集 | Admin SQ/CQ（队列 0） | 控制器管理：识别、特性、队列创建、日志、固件升级 |
| NVM I/O 命令集 | I/O SQ/CQ（队列 1..N） | 数据读写：Read/Write/Flush/DSM/Write Zeroes 等 |

所有命令由主机放入提交队列（SQ），控制器执行后把完成项（CQE）写入完成队列（CQ），并通过中断或轮询通知主机。

---

## 2. 命令与完成项格式

### 2.1 提交项（SQE，64 字节）

```c
// include/linux/nvme.h:1082
struct nvme_common_command {
    __u8  opcode;          // 操作码
    __u8  flags;           // FUSE / PSDT（PRP 或 SGL）
    __u16 command_id;      // 主机分配的标签，完成项原样返回
    __le32 nsid;           // 命名空间 ID
    __le32 cdw2[2];
    __le64 metadata;       // 元数据指针
    union nvme_data_ptr dptr;   // PRP1/PRP2 或 SGL
    __le32 cdw10;
    __le32 cdw11;
    __le32 cdw12;
    __le32 cdw13;
    __le32 cdw14;
    __le32 cdw15;
};
```

不同命令复用同一 64 字节布局，用 `union nvme_command` 解释（见 [define](define.md)）。`cdw10~cdw15` 的含义由操作码决定。

### 2.2 完成项（CQE，16 字节）

```c
// include/linux/nvme.h:2252
struct nvme_completion {
    union nvme_result {
        __le16 u16;
        __le32 u32;
        __le64 u64;
    } result;              // 命令返回值（如 Identify 无、Get Features 有）
    __le16 sq_head;        // SQ 头指针，告知主机可回收的槽位
    __le16 sq_id;          // 产生该完成项的 SQ
    __u16  command_id;     // 对应 SQE 的 command_id
    __le16 status;         // 状态字段：phase bit + status code
};
```

`status` 的低 1 位是 **Phase Tag**：主机用它判断该 CQE 是新的还是上一轮残留；高 15 位是状态码（`SC`）与状态类型（`SCT`）。

---

## 3. Admin 命令操作码

```c
// include/linux/nvme.h:1306
enum nvme_admin_opcode {
    nvme_admin_delete_sq       = 0x00,
    nvme_admin_create_sq       = 0x01,
    nvme_admin_get_log_page    = 0x02,
    nvme_admin_delete_cq       = 0x04,
    nvme_admin_create_cq       = 0x05,
    nvme_admin_identify        = 0x06,
    nvme_admin_abort_cmd       = 0x08,
    nvme_admin_set_features    = 0x09,
    nvme_admin_get_features    = 0x0a,
    nvme_admin_async_event     = 0x0c,
    nvme_admin_ns_mgmt         = 0x0d,
    nvme_admin_activate_fw     = 0x10,
    nvme_admin_download_fw     = 0x11,
    nvme_admin_dev_self_test   = 0x14,
    nvme_admin_ns_attach       = 0x15,
    nvme_admin_keep_alive      = 0x18,
    nvme_admin_directive_send  = 0x19,
    nvme_admin_directive_recv  = 0x1a,
    nvme_admin_virtual_mgmt    = 0x1c,
    nvme_admin_nvme_mi_send    = 0x1d,
    nvme_admin_nvme_mi_recv    = 0x1e,
    nvme_admin_format_nvm      = 0x80,
    nvme_admin_security_send   = 0x81,
    nvme_admin_security_recv   = 0x82,
    nvme_admin_sanitize_nvm    = 0x84,
    nvme_admin_get_lba_status  = 0x86,
};
```

驱动初始化时最常用的几条：

| 命令 | 作用 |
|------|------|
| `Identify` | 读取控制器/命名空间/数据结构布局 |
| `Set/Get Features` | 配置队列深度、中断合并、温度阈值等 |
| `Create/Delete CQ/SQ` | 建立 I/O 队列 |
| `Get Log Page` | 读取 SMART/健康、错误日志、固件槽信息 |
| `Async Event Request` | 订阅控制器异步事件（热插拔、SMART 告警） |
| `Download/Activate Firmware` | 固件升级 |

---

## 4. NVM I/O 命令操作码

```c
// include/linux/nvme.h:956
enum nvme_opcode {
    nvme_cmd_flush        = 0x00,
    nvme_cmd_write        = 0x01,
    nvme_cmd_read         = 0x02,
    nvme_cmd_write_uncor  = 0x04,
    nvme_cmd_compare      = 0x05,
    nvme_cmd_write_zeroes = 0x08,
    nvme_cmd_dsm          = 0x09,
    nvme_cmd_verify       = 0x0c,
    nvme_cmd_resv_register = 0x0d,
    nvme_cmd_resv_report  = 0x0e,
    nvme_cmd_resv_acquire = 0x11,
    nvme_cmd_resv_release = 0x15,
    nvme_cmd_zone_mgmt_send = 0x79,
    nvme_cmd_zone_mgmt_recv = 0x7a,
    nvme_cmd_zone_append  = 0x7d,
};
```

### 4.1 Read / Write 命令

```c
// include/linux/nvme.h:1100
struct nvme_rw_command {
    __u8   opcode;
    __u8   flags;
    __u16  command_id;
    __le32 nsid;          // 命名空间
    __le32 cdw2;
    __le32 cdw3;
    __le64 metadata;
    union nvme_data_ptr dptr;
    __le64 slba;          // 起始 LBA
    __le16 length;        // 传输长度 - 1（以 LBA 为单位）
    __le16 control;       // 保护信息、FUA、LR 等
    __le32 dsmgmt;
    __le32 reftag;
    __le16 lbat;
    __le16 lbatm;
};
```

注意 `length` 是 **N-1** 编码：写 0 表示传输 1 个 LBA。`control` 中的 `FUA`（Force Unit Access）要求数据落盘后再完成。

---

## 5. 命令处理流程

```text
主机                                              SSD 控制器
 │                                                    │
 ├─ 1. 构造 64B SQE，写入 SQ 槽位 (sq_tail)            │
 ├─ 2. 写 SQ Tail Doorbell ──────────────────────────► │
 │                                                    ├─ 3. 通过 PRP/SGL 读取 SQE
 │                                                    ├─ 4. 执行命令（DMA 读写数据）
 │                                                    ├─ 5. 写 16B CQE 到 CQ
 │ ◄────────────────────────────── 6. MSI-X 中断 ─────┤
 ├─ 7. 读 CQE，检查 phase bit 与 command_id            │
 ├─ 8. 写 CQ Head Doorbell ──────────────────────────► │
 └─ 9. 完成该请求，回收 SQ 槽位                         │
```

Linux 驱动中的对应关系：

| 步骤 | 函数 |
|------|------|
| 构造命令 | `nvme_setup_cmd()`（`drivers/nvme/host/core.c:1097`） |
| 映射数据 | `nvme_map_data()`（PRP/SGL + DMA） |
| 写 SQE | `nvme_sq_copy_cmd()` |
| 敲门铃 | `nvme_write_sq_db()` |
| 处理完成 | `nvme_irq()` → `nvme_handle_cqe()` → `nvme_poll_cq()` |

---

## 6. 数据指针：PRP 与 SGL

SQE 中的 `dptr` 是一个联合体：

```c
// include/linux/nvme.h:1050
union nvme_data_ptr {
    struct {
        __le64 prp1;
        __le64 prp2;
    };
    struct nvme_sgl_desc     sgl;
    struct nvme_keyed_sgl_desc ksgl;
};
```

| 方式 | 说明 |
|------|------|
| PRP（Physical Region Page） | 物理页描述符；PRP1 指向第一页，PRP2 可指向第二页或 PRP List |
| SGL（Scatter Gather List） | 通用分散/聚集描述符，支持更大、更不连续的传输 |

`flags` 中的 `PSDT` 位选择使用 PRP 还是 SGL。普通块 I/O 通常用 PRP；大数据量或需要复杂描述时用 SGL。

---

## 7. 示例：Identify 控制器

```c
struct nvme_command cmd = { };
struct nvme_id_ctrl *id;

id = dma_alloc_coherent(dev, sizeof(*id), &dma_addr, GFP_KERNEL);

cmd.identify.opcode = nvme_admin_identify;
cmd.identify.cns    = NVME_ID_CNS_CTRL;     // 读取控制器结构
cmd.identify.nsid   = 0;
cmd.identify.dptr.prp1 = cpu_to_le64(dma_addr);

/* 提交到 admin 队列并等待完成；成功后 id 中即为控制器信息 */
submit_admin_cmd(&cmd);
```

`Identify` 的 `cns`（Controller or Namespace Structure）决定返回哪种结构：控制器、命名空间、LBA 格式列表、命名空间 ID 列表等。

---

## 8. 相关源码

| 文件 | 说明 |
|------|------|
| `include/linux/nvme.h:956` | NVM I/O 操作码 |
| `include/linux/nvme.h:1306` | Admin 操作码 |
| `include/linux/nvme.h:1082` | `struct nvme_common_command` |
| `include/linux/nvme.h:1100` | `struct nvme_rw_command` |
| `include/linux/nvme.h:2252` | `struct nvme_completion` |
| `drivers/nvme/host/core.c:1097` | `nvme_setup_cmd()` |
| `drivers/nvme/host/pci.c` | `nvme_queue_rq()` / `nvme_irq()` |

相关文档：[NVMe 设备基本原理](arch.md)、[NVMe 数据结构定义](define.md)、[NVMe 简介](intro.md)。
