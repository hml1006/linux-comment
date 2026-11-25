# SCSI / UFS / eMMC 存储子系统

## 1. 概述与分层

Linux 的 SCSI 子系统不仅是 SCSI 协议栈，也是大量“类 SCSI”设备的统一框架：UFS、USB 存储、SATA（libata）、iSCSI、virtio-scsi 都把命令交给 SCSI midlayer 排队、超时和错误恢复。MMC/eMMC 走独立的 `mmc` 框架（`drivers/mmc/core`），同样以块设备形式暴露给上层。

```
上层驱动 ULP: sd / sr / st / sg
        | scsi_cmnd
SCSI midlayer: scsi_lib.c(队列/完成) scsi_error.c(EH) hosts.c scsi_scan.c
        | scsi_host_template
低层驱动 LLD: ufshcd(UFS) / libata / virtio_scsi

MMC: core.c(请求) mmc.c(eMMC) block.c(块队列) mmc_ops.c(初始化)
        | mmc_host_ops
MMC host 控制器驱动
```

关键点：

- SCSI midlayer 复用块层 blk-mq：`Scsi_Host` 内嵌 `struct blk_mq_tag_set tag_set`，每个 `scsi_device` 对应一个 `request_queue`。
- UFS 驱动自己注册 `scsi_host_template`，对 midlayer 来说只是一个 LLD；UIC/DME 与 Query 请求走独立通道。
- MMC 不使用 SCSI midlayer，直接在 `mmc_queue` 上实现 blk-mq 的 `queue_rq`。
- 当前 `.config`：`CONFIG_SCSI=y`；`CONFIG_MMC is not set`、`CONFIG_SCSI_UFSHCD is not set`。因此 MMC/UFS 源码存在、机制可读，但本内核不编译、不启用。

---

## 2. 核心数据结构

### 2.1 Scsi_Host

`include/scsi/scsi_host.h`，由 `scsi_host_alloc()` 分配，`shost_priv()` 取驱动私有区。

```c
struct Scsi_Host {
    struct list_head  __devices, __targets, starved_list;
    spinlock_t        default_lock;
    spinlock_t       *host_lock;
    struct list_head  eh_abort_list, eh_cmd_q; /* 待恢复命令 */
    struct task_struct *ehandler;    /* scsi_error_handler 线程 */
    const struct scsi_host_template *hostt;
    struct scsi_transport_template  *transportt;
    struct blk_mq_tag_set tag_set;   /* 共享 tag map */
    atomic_t           host_blocked;
    unsigned int       host_failed;  /* 失败命令数 */
    unsigned int       host_eh_scheduled, host_no;
    int                eh_deadline;
    unsigned int       max_channel, max_id;
    u64                max_lun;
    unsigned short     max_cmd_len;
    int                can_queue;
    unsigned int       nr_hw_queues, nr_maps;
    enum scsi_host_state shost_state; /* SHOST_CREATED/RUNNING/RECOVERY... */
};
```

`scsi_host_template` 是 LLD 回调表：`queuecommand(shost, cmd)` 提交一条 `scsi_cmnd`（返回 0 或 `SCSI_MLQUEUE_*`）；`commit_rqs` 做批量提交补充通知；`eh_abort_handler` / `eh_device_reset_handler` / `eh_target_reset_handler` / `eh_bus_reset_handler` / `eh_host_reset_handler` 构成从命令中止到 HBA 复位的多级恢复；`map_queues` 把硬件队列映射到中断向量。

### 2.2 scsi_device

`include/scsi/scsi_device.h`：

```c
struct scsi_device {
    struct Scsi_Host *host;
    struct request_queue *request_queue;
    struct list_head siblings, same_target_siblings;
    struct sbitmap budget_map;              /* 每设备预算 */
    atomic_t device_blocked;                /* QUEUE_FULL 后阻塞 */
    unsigned short queue_depth, max_queue_depth;
    unsigned int id, channel;
    u64 lun;
    unsigned int sector_size, eh_timeout;
    unsigned char type;                     /* 0=disk, 5=CDROM */
    struct scsi_target *sdev_target;
    enum scsi_device_state sdev_state;      /* SDEV_RUNNING/OFFLINE/DEL... */
    void *hostdata;
    unsigned long sdev_data[];              /* 驱动私有区 */
};
```

`enum scsi_device_state` 决定命令是否放行：`SDEV_RUNNING` 全部允许；`SDEV_QUIESCE` 只允许 midlayer 内部命令；`SDEV_BLOCK`/`SDEV_OFFLINE` 拒绝块层命令；`SDEV_DEL` 直接返回 `DID_NO_CONNECT`。

### 2.3 scsi_cmnd

作为 blk-mq request 的 PDU 内嵌（`blk_mq_rq_to_pdu`），生命周期与 request 绑定：

```c
struct scsi_cmnd {
    struct scsi_device *device;
    struct list_head eh_entry;      /* 挂到 eh_cmd_q / eh_abort_list */
    struct delayed_work abort_work;
    struct rcu_head rcu;
    int eh_eflags, budget_token, retries, allowed;
    enum scsi_cmnd_submitter submitter;
    unsigned short cmd_len;
    enum dma_data_direction sc_data_direction;
    unsigned char cmnd[32];         /* CDB，最多 32 字节 */
    struct scsi_data_buffer sdb;    /* sg_table + length */
    struct scsi_data_buffer *prot_sdb;
    unsigned underflow, transfersize, resid_len, sense_len;
    unsigned char *sense_buffer;    /* 自动 REQUEST SENSE 缓冲区 */
    int flags;                      /* SCMD_TAGGED / SCMD_LAST */
    unsigned long state;            /* SCMD_STATE_INFLIGHT/COMPLETE */
    unsigned char *host_scribble;
    int result;                     /* host byte << 16 | driver byte << 8 */
};
```

`scsi_cmd_priv(cmd)` 返回 `cmd + 1`，即 LLD 私有数据区（UFS 放 `ufshcd_lrb`）。

### 2.4 ufs_hba 与 ufshcd_lrb

`include/ufs/ufshcd.h`：

```c
struct ufs_hba {
    void __iomem *mmio_base;
    struct utp_transfer_cmd_desc *ucdl_base_addr;   /* 命令描述符池 */
    struct utp_transfer_req_desc *utrdl_base_addr;  /* 请求描述符池 */
    struct utp_task_req_desc *utmrdl_base_addr;     /* 任务管理池 */
    struct Scsi_Host *host;
    struct scsi_device *ufs_device_wlun;  /* well-known LUN 0xC0 */
    struct scsi_device *ufs_rpmb_wlun;    /* RPMB LUN 0xC4 */
    enum ufs_dev_pwr_mode curr_dev_pwr_mode;
    enum uic_link_state uic_link_state;
    unsigned long outstanding_tasks, outstanding_reqs;
    int nutrs, nortt, nutmrs;             /* 各类请求上限 */
    u32 mcq_capabilities;
    const struct ufs_hba_variant_ops *vops;
    unsigned int quirks;
    struct blk_mq_tag_set tmf_tag_set;
    struct request_queue *tmf_queue;
    struct uic_command *active_uic_cmd;
    struct mutex uic_cmd_mutex;
    enum ufshcd_state ufshcd_state;       /* RESET/OPERATIONAL/EH_*/ERROR */
    u32 eh_flags, errors, uic_error;
    struct work_struct eh_work, eeh_work;
    bool force_reset;
};
```

```c
struct ufshcd_lrb {
    struct utp_transfer_req_desc *utr_descriptor_ptr;
    struct utp_upiu_req *ucd_req_ptr;   /* 命令 UPIU */
    struct utp_upiu_rsp *ucd_rsp_ptr;   /* 响应 UPIU */
    struct ufshcd_sg_entry *ucd_prdt_ptr; /* PRDT，物理区域描述表 */
    dma_addr_t utrd_dma_addr, ucd_req_dma_addr;
    dma_addr_t ucd_rsp_dma_addr, ucd_prdt_dma_addr;
    int scsi_status, command_type;
    u8 lun;
    bool intr_cmd;
    ktime_t issue_time_stamp, compl_time_stamp;
};
```

### 2.5 mmc_host / mmc_card / mmc_request

```c
/* include/linux/mmc/host.h：f_min/f_max 时钟范围，caps/caps2 为总线能力 */
struct mmc_host {
    struct device *parent;
    const struct mmc_host_ops *ops;  /* request/set_ios/get_cd */
    unsigned int f_min, f_max, f_init;
    u32 ocr_avail;
    u32 caps;    /* MMC_CAP_4_BIT_DATA / 8_BIT / HS200 ... */
    u32 caps2;
};

/* include/linux/mmc/card.h：raw_cid/csd/scr 与 ext_csd 为寄存器快照 */
struct mmc_card {
    struct mmc_host *host;
    u32 ocr;
    unsigned int rca, type, state, quirks;
    unsigned int erase_size;
    u32 raw_cid[4], raw_csd[4], raw_scr[2];
    struct mmc_cid cid;
    struct mmc_csd csd;
    struct mmc_ext_csd ext_csd;  /* eMMC 扩展 CSD */
    bool written_flag;
};

/* include/linux/mmc/core.h */
struct mmc_request {
    struct mmc_command *sbc;   /* CMD23 SET_BLOCK_COUNT */
    struct mmc_command *cmd;
    struct mmc_data    *data;
    struct mmc_command *stop;
    struct completion   completion, cmd_completion;
    void (*done)(struct mmc_request *);
    void (*recovery_notifier)(struct mmc_request *);
    struct mmc_host *host;
    int tag;                   /* CQE 队列 tag */
};
```

---

## 3. SCSI 命令队列与提交路径

### 3.1 blk-mq 接入

`drivers/scsi/scsi_lib.c` 注册 `scsi_mq_ops`：`get_budget/put_budget`、`queue_rq=scsi_queue_rq`、`commit_rqs`、`complete=scsi_complete`、`timeout=scsi_timeout`、`init_request/exit_request`、`cleanup_rq`、`busy=scsi_mq_lld_busy`、`map_queues`、`init_hctx`、`poll`。

`scsi_mq_setup_tags()` 以 `hostt->cmd_size + sizeof(struct scsi_cmnd)` 作为每 request 的 PDU 大小，设置 `tag_set.nr_hw_queues = shost->nr_hw_queues`。`host_tagset` 为真时所有硬件队列共享同一 tag 空间。

### 3.2 提交链

```
blk_mq_run_hw_queue
  -> scsi_queue_rq()
       -> scsi_prepare_cmd(req)        /* 初始化 scsi_cmnd、CDB、SG */
       -> blk_mq_start_request(req)
       -> scsi_dispatch_cmd(cmd)
            -> host->hostt->queuecommand(host, cmd)
                 -> ufshcd_queuecommand() / libata / virtio_scsi
```

`scsi_dispatch_cmd()` 做最后检查：`SDEV_DEL` → `DID_NO_CONNECT`；`scsi_device_blocked()` → `SCSI_MLQUEUE_DEVICE_BUSY`；`cmd_len > max_cmd_len` → `DID_ABORT`；`SHOST_DEL` → `DID_NO_CONNECT`；否则调 `hostt->queuecommand()`，返回非 0 时归一化为 `SCSI_MLQUEUE_HOST_BUSY`（`DEVICE_BUSY`/`TARGET_BUSY` 保留）。

### 3.3 完成链

```
LLD 中断 -> scsi_done(cmd)
  -> blk_mq_complete_request(req)
       -> scsi_complete()
            -> scsi_io_completion(cmd, good_bytes)
                 -> scsi_end_request(req, error, bytes)
                      -> blk_update_request() / blk_mq_end_request()
```

`scsi_io_completion_action()` 根据 `cmd->result` 的 host/driver byte 决定重试、部分完成、请求 sense、交给 EH 或直接失败；`scsi_result_to_blk_status()` 把 SCSI 状态翻译为 `BLK_STS_IOERR`、`BLK_STS_RESOURCE`、`BLK_STS_AGAIN` 等。

### 3.4 队列深度、预算与饿死列表

- `shost->can_queue` 是 HBA 并发上限，`nr_reserved_cmds` 为 EH/管理命令预留；`sdev->queue_depth` 是每 LUN 深度，`scsi_change_queue_depth()` 可动态调整。
- `budget_map`：`scsi_mq_get_budget()` 取 token、`scsi_mq_put_budget()` 归还，耗尽返回 `BLK_STS_RESOURCE` 让块层稍后重试。
- 设备返回 `QUEUE_FULL` 时 `scsi_handle_queue_full()` 降低深度，空闲后 `scsi_handle_queue_ramp_up()` 恢复。
- target/host 忙时 `scsi_queue_rq()` 把设备挂到 `shost->starved_list`；完成后 `scsi_run_queue()` → `scsi_starved_list_run()` 重新调度。LLD 调用 `scsi_device_block()` 置 `device_blocked` 后设备进入 `SDEV_BLOCK`，直到 LLD 解除。

---

## 4. SCSI 错误处理（EH）

### 4.1 触发方式

| 触发源 | 路径 |
|--------|------|
| 命令超时 | `scsi_timeout()` → `scsi_eh_scmd_add(cmd)` |
| 完成后判定需恢复 | `scsi_decide_disposition()` → `ADD_TO_MLQUEUE` → `scsi_eh_scmd_add()` |
| LLD 主动上报 | `scsi_schedule_eh(shost)` 置 `host_eh_scheduled` |
| 中止请求 | `scsi_abort_command()` → `scmd_eh_abort_handler()` |

`scsi_eh_scmd_add()` 把命令加入 `shost->eh_cmd_q`，`scsi_eh_inc_host_failed()` 增加 `host_failed`，`scsi_eh_wakeup()` 唤醒 `ehandler`。

### 4.2 EH 线程主循环

`scsi_error_handler()` 是每 host 一个 kthread（`scsi_host_alloc()` 内 `kthread_run()` 创建）：

```c
while (true) {
    set_current_state(TASK_INTERRUPTIBLE);
    if (kthread_should_stop()) break;
    if ((shost->host_failed == 0 && shost->host_eh_scheduled == 0) ||
        shost->host_failed != scsi_host_busy(shost)) {
        schedule(); continue;
    }
    __set_current_state(TASK_RUNNING);
    if (shost->transportt->eh_strategy_handler)
        shost->transportt->eh_strategy_handler(shost);
    else
        scsi_unjam_host(shost);   /* 标准 EH 策略 */
    shost->host_failed = 0;
}
```

`host_failed != scsi_host_busy()` 就睡眠，意味着 EH 必须等在途命令全部有结果（完成或计入失败）才动手，避免与正常完成竞争。

### 4.3 标准恢复策略 scsi_unjam_host

```
scsi_unjam_host()
  ├─ list_splice_init(eh_cmd_q -> eh_work_q)
  ├─ scsi_eh_get_sense()     /* CHECK CONDITION 自动发 REQUEST SENSE */
  └─ scsi_eh_ready_devs()
       ├─ scsi_eh_test_devices()      /* TUR 判断设备存活 */
       ├─ scsi_eh_try_stu()           /* START STOP UNIT 唤醒 */
       ├─ 复位升级: bus_device -> target -> bus -> host
       └─ scsi_eh_offline_sdevs()     /* 全部失败则离线设备 */
```

`enum scsi_disposition`（`include/scsi/scsi.h`）表示单条命令的处置结论：`SUCCESS`、`NEEDS_RETRY`、`FAILED`、`ADD_TO_MLQUEUE`、`TIMEOUT_ERROR` 等；`scsi_decide_disposition()` 综合 sense key、host byte 与 retries 给出结论。

### 4.4 时间与并发约束

- `shost->eh_deadline` 是 EH 总时长上限，`scsi_host_eh_past_deadline()` 检查，超时则跳过后续复位直接离线。
- `scsi_block_when_processing_errors()` 在 EH 期间阻塞新的打开/扫描；`scsi_eh_prep_cmnd()` / `scsi_eh_restore_cmnd()` 在借用原命令发 TUR/REQUEST SENSE 时保存并恢复现场。
- EH 完成后 `scsi_restart_operations()` → `scsi_run_host_queues()` 恢复设备队列。

---

## 5. UFS host/core

### 5.1 硬件模型

UFS 使用 UTP + UniPro + M-PHY。UFSHCI 通过三段 DMA 结构描述请求：UTRD（请求描述符）内含命令 UPIU、响应 UPIU 和一个 PRDT（指向数据 sg）。`ufs_hba` 的 `ucdl_base_addr`/`utrdl_base_addr`/`utmrdl_base_addr` 分别是命令、请求、任务管理描述符池；每个 tag 对应一个 `ufshcd_lrb`。

### 5.2 命令提交

```
scsi_dispatch_cmd -> ufshcd_queuecommand(shost, cmd)
   ├─ 按 hba->ufshcd_state 分流:
   │    OPERATIONAL / EH_SCHEDULED_NON_FATAL -> 继续
   │    EH_SCHEDULED_FATAL -> PM 命令直接 DID_BAD_TARGET
   │    RESET -> SCSI_MLQUEUE_HOST_BUSY; ERROR -> DID_ERROR
   ├─ ufshcd_hold() -> ufshcd_setup_scsi_cmd() -> ufshcd_map_sg()
   └─ ufshcd_send_command(hba, cmd, hwq)
```

`ufshcd_send_command()` 两条路径：

```c
if (hba->mcq_enabled) {
    /* MCQ: UTRD 拷入硬件提交队列槽位，更新 sq_tail */
    dest = hwq->sqe_base_addr + hwq->sq_tail_slot;
    memcpy(dest, src, sizeof(struct utp_transfer_req_desc));
    ufshcd_inc_sq_tail(hwq);
} else {
    /* 传统单队列: 置 outstanding 位，写门铃寄存器 */
    __set_bit(tag, &hba->outstanding_reqs);
    ufshcd_writel(hba, 1 << tag, REG_UTP_TRANSFER_REQ_DOOR_BELL);
}
```

### 5.3 MCQ（Multi-Circular Queue）

`hba->mcq_capabilities` 表明支持时，`ufshcd_mcq_init()`（`drivers/ufs/core/ufs-mcq.c`）为每个硬件队列分配提交队列（SQ）与完成队列（CQ），并用 `ufshcd_mcq_config_esi()` 把 CQ 绑定到 MSI-X 向量。完成侧 `ufshcd_compl_one_cqe(hba, tag, cqe)` 解析 CQE 后调 `scsi_done()`；轮询走 `ufshcd_mcq_poll_cqe_lock()`。`nutmrs` 是 MCQ 下 task management 请求数。

### 5.4 设备管理与错误恢复

- UIC 命令（DME_GET/SET、DME_LINKSTARTUP 等）通过 `uic_command` + `uic_cmd_mutex` 串行执行，不能与 SCSI 命令混用。
- Query 请求（读写描述符/属性/flag）通过 well-known LUN `ufs_device_wlun` 的 `scsi_cmnd` 下发，构造 Query UPIU。
- `ufs_trigger_eh(hba)` 在提交失败或错误位置起时调度 `eh_work`；UFS EH 执行 link startup、host reset、设备复位，失败则进入 `UFSHCD_STATE_ERROR`。
- `ufs_hba_variant_ops` 提供厂商钩子：`init`、`hce_enable_notify`、`link_startup_notify`、`pwr_change_notify`、`setup_xfer_req`、`setup_task_mgmt`、`resume`、`suspend` 等。
- UFS 只注册一个 `Scsi_Host`，含 well-known LUN（UFS device 0xC0、RPMB 0xC4）与普通 LUN；`ufshcd_slave_alloc/configure` 按 LUN 类型设置 `sdev->type`、写保护与队列深度，因此 `sd` 看到的是标准 SCSI 磁盘。

---

## 6. MMC / eMMC 核心与请求路径

### 6.1 初始化与枚举

```
mmc_alloc_host() -> mmc_add_host() -> mmc_rescan()  /* workqueue 探测 */
  -> mmc_rescan_try_freq() -> mmc_attach_mmc() / mmc_attach_sd()
  -> mmc_power_up() / mmc_go_idle() / mmc_send_op_cond()
```

`mmc_attach_mmc()`：CMD1 读 OCR → CMD3 分配 RCA → 读 CSD/CID → `mmc_select_hs400es()`/`mmc_select_hs200()` 切换时序 → `mmc_add_card()` → `mmc_blk_alloc_req()` 创建 `gendisk`。eMMC 的 `ext_csd`（`struct mmc_ext_csd`）描述分区、RPMB、cache、boot 配置。

### 6.2 请求路径

```
blk_mq queue_rq
  -> mmc_blk_mq_issue_rq(mq, req)     /* drivers/mmc/core/block.c */
       ├─ mmc_blk_part_switch()       /* 切换到对应硬件分区 */
       ├─ mmc_issue_type():
       │    MMC_ISSUE_SYNC:  discard/secure erase/flush 同步执行
       │    MMC_ISSUE_DCMD:  CQE 直接命令
       │    MMC_ISSUE_ASYNC: 普通 read/write
       ├─ cqe_enabled ? mmc_blk_cqe_issue_rw_rq() : mmc_blk_mq_issue_rw_rq()
       └─ mmc_start_request(host, &mqrq->brq.mrq)
```

`mmc_start_request()`（`drivers/mmc/core/core.c`）做前置校验（`mmc_card_removed()`、`host->claimed`、`mmc_mrq_prep()`），再 `__mmc_start_request()` → `host->ops->request(host, mrq)`。

完成：

```
控制器中断 -> mmc_request_done(host, mrq)
  -> mrq->done(mrq)              /* = mmc_blk_mq_req_done() */
       -> mmc_blk_mq_complete_rq(mq, req)
            -> blk_mq_end_request()
```

`mmc_blk_mq_complete_rq()` 依据 `bytes_xfered` 与 `brq->data.error` 决定完成整个请求、部分完成还是重发当前块。

### 6.3 CQE（Command Queue Engine）

eMMC 5.1+ 的 CQE 允许最多 32 条任务排队，硬件按 tag 执行：

- `mmc_blk_cqe_issue_rw_rq()` 经 `mmc_cqe_start_req()` 提交，`mrq->tag` 来自块层 tag；
- 完成由 `mmc_cqe_irq()` → `mmc_cqe_request_done()` 处理；
- 出错时 `mmc_cqe_recovery()` 复位 CQE，并通过 `mmc_cqe_recovery_notifier` 通知块层；块层走 `mmc_blk_mq_recovery()`。

### 6.4 与 SCSI 的对比

| 维度 | SCSI | MMC |
|------|------|-----|
| 队列框架 | blk-mq + tag_set，可多硬件队列 | blk-mq，通常单队列；CQE 提供 tag 并发 |
| 命令抽象 | CDB + `scsi_cmnd` | `mmc_command` + `mmc_data` 组成的 `mmc_request` |
| 错误恢复 | 独立 EH 线程，多级复位 | `mmc_blk_mq_recovery` / CQE recovery，重发请求 |
| 设备模型 | target/LUN 两级 | 每卡一个 `mmc_card`，硬件分区独立 `mmc_blk_data` |
| 识别方式 | INQUIRY / READ CAPACITY | CID/CSD/EXT_CSD 寄存器 |

---

## 7. 当前配置状态与调试视角

| 配置项 | 值 | 影响 |
|--------|----|------|
| `CONFIG_SCSI` | `y` | midlayer、`scsi_lib.o`、`scsi_error.o`、`hosts.o` 编译进内核 |
| `CONFIG_BLK_DEV_SD` | 可见 | `sd` 磁盘驱动可用，但无底层 HBA 时不产生磁盘 |
| `CONFIG_SCSI_UFSHCD` | `is not set` | `drivers/ufs/` 不参与编译，仅作源码参考 |
| `CONFIG_MMC` | `is not set` | `drivers/mmc/core` 不编译，无 `/sys/class/mmc_host`、`mmcblk*` |
| `CONFIG_SCSI_LOGGING` | `is not set` | `SCSI_LOG_*` 宏为空，`scsi_logging_level` 不生效 |

因此在本内核上：可读 SCSI midlayer 通用路径，但不会有真实 UFS/eMMC 设备；`scsi_host_alloc()`/EH 线程逻辑仍会为任何注册的 LLD 建立。要验证 UFS/MMC 需打开对应 Kconfig 并重新构建。

---

## 关键文件清单

| 文件 | 内容 |
|------|------|
| `include/scsi/scsi_host.h` | `Scsi_Host`、`scsi_host_template`、`enum scsi_host_state` |
| `include/scsi/scsi_device.h` | `scsi_device`、`scsi_target`、`enum scsi_device_state` |
| `include/scsi/scsi_cmnd.h` | `scsi_cmnd`、`scsi_data_buffer`、`SCMD_*` 标志 |
| `include/scsi/scsi.h` | `enum scsi_disposition`、`SCSI_MLQUEUE_*` |
| `drivers/scsi/hosts.c` | host 分配/注册/释放，创建 `scsi_error_handler` 线程 |
| `drivers/scsi/scsi_lib.c` | `scsi_queue_rq`、`scsi_dispatch_cmd`、`scsi_io_completion`、预算与饿死列表 |
| `drivers/scsi/scsi_error.c` | `scsi_error_handler`、`scsi_unjam_host`、各类 reset handler |
| `drivers/scsi/scsi_scan.c` | 设备发现与 LUN 扫描 |
| `drivers/scsi/scsi_sysfs.c` | sysfs 属性与设备生命周期 |
| `include/ufs/ufshcd.h` | `ufs_hba`、`ufshcd_lrb`、`enum ufshcd_state`、quirks |
| `include/ufs/ufshci.h` | UFSHCI 寄存器、UTRD/UTMRD 布局 |
| `drivers/ufs/core/ufshcd.c` | `ufshcd_queuecommand`、`ufshcd_send_command`、初始化与 EH |
| `drivers/ufs/core/ufs-mcq.c` | MCQ 队列初始化、ESI 配置、CQE 处理 |
| `drivers/ufs/core/ufs-sysfs.c` | UFS sysfs 属性 |
| `drivers/ufs/core/ufs_bsg.c` | UFS BSG 透传接口 |
| `include/linux/mmc/host.h` | `mmc_host`、`mmc_host_ops`、`MMC_CAP_*` |
| `include/linux/mmc/card.h` | `mmc_card`、quirks、`mmc_ext_csd` |
| `include/linux/mmc/core.h` | `mmc_command`、`mmc_data`、`mmc_request` |
| `drivers/mmc/core/core.c` | `mmc_start_request`、`mmc_request_done`、`mmc_rescan` |
| `drivers/mmc/core/mmc.c` | eMMC 识别、时序切换、`mmc_attach_mmc` |
| `drivers/mmc/core/block.c` | `mmc_blk_mq_issue_rq`、完成与恢复、CQE 路径 |
| `drivers/mmc/core/queue.c` | `mmc_queue` 与 blk-mq 队列创建 |
| `drivers/mmc/core/mmc_ops.c` | CMD0/CMD1/CMD3/CMD6 等基础命令 |
