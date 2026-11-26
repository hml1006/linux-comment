# libata 子系统：ATA/SATA 设备的 SCSI 化中间层

ATA/SATA 设备在 Linux 中最终以 `/dev/sdX`、`/dev/srX` 出现，走 SCSI 的块设备与错误恢复框架。把它们粘合起来的是 `drivers/ata/` 下的 libata：向下实现 ATA 协议（taskfile、IDENTIFY、NCQ、SFF/BMDMA），向上导出一个“模拟的” SCSI 主机。本章基于本源码树（Linux 7.2 带中文注释）实际代码，重点覆盖 `libata.h`、`libata-core.c`、`libata-scsi.c`、`libata-eh.c`、`libata-sff.c`、`libahci.c`、`ahci.c`。`drivers/ata/` 下（含 `pata_parport/` 子目录）共 132 个 `.c` 文件，多数是厂商 HBA 边缘驱动，共享逻辑集中在 libata 核心。

## 1. 定位与分层：把 ATA 设备伪装成 SCSI

核心设计写在 SHT 初始化宏里：每个 ATA port 分配一个 `Scsi_Host`，`emulated = 1`、`this_id = -1`，`queuecommand` 指向 `ata_scsi_queuecmd`。上层看到标准 SCSI 主机，下层驱动看到 `ata_port_operations`。

```c
#define __ATA_BASE_SHT(drv_name)				\
	.module			= THIS_MODULE,			\
	.name			= drv_name,			\
	.ioctl			= ata_scsi_ioctl,		\
	ATA_SCSI_COMPAT_IOCTL					\
	.queuecommand		= ata_scsi_queuecmd,		\
	.eh_timed_out		= ata_scsi_eh_timed_out,	\
	.dma_need_drain		= ata_scsi_dma_need_drain,	\
	.this_id		= ATA_SHT_THIS_ID,		\
	.emulated		= ATA_SHT_EMULATED,		\
	.proc_name		= drv_name,			\
	.sdev_init		= ata_scsi_sdev_init,		\
	.sdev_destroy		= ata_scsi_sdev_destroy,	\
	.bios_param		= ata_std_bios_param,		\
	.unlock_native_capacity	= ata_scsi_unlock_native_capacity,\
	.max_sectors		= ATA_MAX_SECTORS_LBA48
```

`ATA_SHT_THIS_ID = -1`、`ATA_SHT_EMULATED = 1` 见 `include/linux/libata.h:137`，是“ATA 对 SCSI 隐身”的开关。两边概念对应如下：

| ATA 世界 | SCSI 世界 | libata 中的体现 |
|---|---|---|
| ATA host adapter | `Scsi_Host` | 每个 `ata_port` 一个 `scsi_host` |
| ATA port | SCSI host | 每个 port 一个 `Scsi_Host` |
| ATA link（含 PMP 端口） | SCSI channel | `ata_link`，PMP 时 `channel = link->pmp` |
| ATA device（master/slave） | SCSI target | `ata_device->devno` → `id` |
| ATA taskfile 命令 | SCSI CDB | `ata_scsi_translate()` |
| ATA status/error | SCSI sense | `ata_to_sense_error()` |
| ATA NCQ tag | SCSI queue tag | `qc->tag` / `qc->hw_tag` |

分层与调用方向如下。注意错误处理是“反向”的：IRQ 先冻结 port，再由 SCSI EH 线程回调 `ata_scsi_error()` 进入 libata EH。

```
 用户态 / 文件系统 → block 层 → SCSI 中间层（sd/sr/scsi_error）
        │ queuecommand                    ▲ scsi_done
        ▼                                 │
 libata-scsi.c：翻译 / 模拟 / sense 生成
        │ ata_qc_issue                    ▲ ata_qc_complete
        ▼                                 │
 libata-core.c：qc 生命周期、EH 调度
        │ ops->qc_prep / qc_issue         ▲ IRQ
        ├──────────────┬─────────────────┘
        ▼              ▼
 libata-sff.c      libahci.c（共享引擎）→ ahci.c（PCI glue）/平台驱动
 （SFF/BMDMA）            │
                          ▼
                       SATA HBA
```

目录职责分“共享核心 / 可选核心 / 边缘驱动”三档：

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/linux/libata.h` / `ata.h` | 2183 / 1050 | 结构、flag、IDENTIFY word、命令码 |
| `libata-core.c` | 6967 | host/port/link/dev、qc、IDENTIFY、模式设置 |
| `libata-scsi.c` | 5169 | 翻译、模拟、sense、扫描、热插拔 |
| `libata-eh.c` | 4266 | 异常检测、重置、重试、上报 |
| `libata-sff.c` | 3207 | SFF taskfile、BMDMA、PIO 状态机 |
| `libata-sata.c` / `libata-pmp.c` | 1737 / 1118 | SATA 链路/SCR/LPM、Port Multiplier |
| `libata-acpi.c` / `libata-zpodd.c` | 1103 / 294 | ACPI `_GTF`、零功耗光驱 |
| `libata-transport.c` / `libata-trace.c` | 803 / 257 | sysfs transport class、tracepoint |
| `ahci.c` / `libahci.c` / `ahci.h` | 3123 / 2774 / 475 | AHCI PCI 驱动、通用引擎、寄存器 |

`libata.o` 组成由 `Makefile:126` 决定：`libata-core.o libata-scsi.o libata-eh.o libata-transport.o libata-trace.o` 必选，`libata-sata.o`、`libata-sff.o`、`libata-pmp.o`、`libata-acpi.o`、`libata-zpodd.o`、`libata-pata-timings.o` 按配置追加。这解释了关掉 `CONFIG_ATA_SFF` 后 SFF/BMDMA 代码完全不参与编译。

## 2. 核心数据结构
对象层次是 host → port → link → device。`ata_port` 内嵌默认 `ata_link`，PMP 场景再分配 `pmp_link[]`；每个 link 内嵌 `device[ATA_MAX_DEVICES]`（值为 2，见 `include/linux/ata.h:26`）。qc 是命令载体，32 个普通 tag 加 1 个内部 tag，因此 `qcmd[ATA_MAX_QUEUE + 1]`（`libata.h:902`）。

```
ata_host ─ lock / eh_mutex / kref / n_tags / ports[]
 └─ ata_port ─ scsi_host / ops / qcmd[33] / qc_active(u64) / link / pmp_link[]
     └─ ata_link ─ active_tag / sactive / sata_spd / lpm_policy
         │          eh_info / eh_context / device[2]
         └─ ata_device ─ devno / class / id[256] / n_sectors / sdev / quirks
```

| 结构 | 关键字段 | 说明 |
|---|---|---|
| `ata_host` | `lock`、`eh_mutex`、`n_ports`、`n_tags`、`ports[]` | 一个 HBA 一个 host，`n_tags` 来自 `can_queue` |
| `ata_port` | `scsi_host`、`ops`、`pflags`、`qcmd[]`、`qc_active` | `pflags` 由 EH 主导，`qc_active` 记录在飞命令 |
| `ata_link` | `pmp`、`active_tag`、`sactive`、`sata_spd` | 非 NCQ 只能有一个 `active_tag`；NCQ 用 `sactive` |
| `ata_device` | `devno`、`class`、`id[]`、`xfer_mode`、`sdev` | `class` 为 ATA/ATAPI/PMP/ZAC 等 |
| `ata_queued_cmd` | `scsicmd`、`tf`、`tag`、`hw_tag`、`sg`、`err_mask` | 一个 SCSI 命令对应一个 qc |
| `ata_eh_context` | `i.action/err_mask/serror`、`tries[]`、`cmd_timeout_idx[][]`、`classes[]`、`saved_ncq_enabled`、`last_reset` | EH 期间每设备重试、分类与省电状态 |

`ata_queued_cmd` 把 SCSI 命令、ATA taskfile、DMA 表、结果放在同一对象里：

```c
struct ata_queued_cmd {
	struct ata_port		*ap;
	struct ata_device	*dev;

	struct scsi_cmnd	*scsicmd;
	void			(*scsidone)(struct scsi_cmnd *);

	struct ata_taskfile	tf;
	u8			cdb[ATAPI_CDB_LEN];

	unsigned long		flags;		/* ATA_QCFLAG_xxx */
	unsigned int		tag;		/* libata core tag */
	unsigned int		hw_tag;		/* driver tag */
	unsigned int		n_elem;
```

其余字段 `dma_dir`、`nbytes`、`sg/cursg`、`err_mask`、`result_tf`、`complete_fn` 分别描述 DMA 方向、字节数、散列表、错误掩码、结果寄存器与完成回调。

EH 的决策状态在 `ata_eh_context`，由 IRQ 上下文写入的 `ata_eh_info` 复制而来；字段包括 `i.action`、`err_mask`、`serror`、`tries[ATA_MAX_DEVICES]`、`cmd_timeout_idx[][]`（EH 内部命令超时表索引）、`classes[ATA_MAX_DEVICES]`、`did_probe_mask`、`unloaded_mask`、`saved_ncq_enabled`、`saved_xfer_mode[]` 与 `last_reset`。

锁约定：`ap->lock` 通常就是 `host->lock`（`ata_port_alloc()` 里 `ap->lock = &host->lock`，`libata-core.c:5704`），保护 qc 状态与 tag 位图；`host->eh_mutex` 保证同一 host 只有一个 EH。`ap->pflags` 中的 `ATA_PFLAG_FROZEN`、`ATA_PFLAG_EH_PENDING`、`ATA_PFLAG_RESETTING` 是“能否下发命令”的第一道门。

## 3. 初始化与探测

边缘驱动遵循固定模板：`ata_host_alloc_pinfo()` 分配 host/port，`ata_host_activate()` 内部先 `ata_host_start()`（调 `ops->port_start`），再申请 IRQ，最后 `ata_host_register()`。以 AHCI 为例：

```
ahci_init_one → ata_host_alloc_pinfo → ahci_host_activate
 → ata_host_activate → ata_host_start → ap->ops->port_start = ahci_port_start
 → devm_request_irq → ata_host_register
    → ata_tport_add（sysfs）
    → ata_scsi_add_hosts（每个 port 一个 Scsi_Host）
    → async_schedule(async_port_probe)
```

`ata_port_alloc()`（`libata-core.c:5694`）把新 port 标成 `INITIALIZING | FROZEN`，初始化 `hotplug_task`、`scsi_rescan_task`、`fastdrain_timer`，并调用 `ata_sff_port_init()`。`ata_host_register()` 用 SHT 的 `can_queue` 夹出 `n_tags`，打印 “SATA max UDMA/133 ...” 横幅，然后异步探测：

```c
	/* perform each probe asynchronously */
	for (i = 0; i < host->n_ports; i++) {
		struct ata_port *ap = host->ports[i];
		ap->cookie = async_schedule(async_port_probe, ap);
	}
```

真正探测不是直接在注册路径里做，而是“先排 EH，再让 EH 扫描”。`ata_port_probe()` 只设标志并调度异常处理：

```c
	ehi->probe_mask |= ATA_ALL_DEVICES;
	ehi->action |= ATA_EH_RESET;
	ehi->flags |= ATA_EHI_NO_AUTOPSY | ATA_EHI_QUIET;

	ap->pflags &= ~ATA_PFLAG_INITIALIZING;
	ap->pflags |= ATA_PFLAG_LOADING;
	ata_port_schedule_eh(ap);
```

`async_port_probe()` 在 `ata_port_probe()` 后调用 `ata_port_wait_eh()`，再同步扫描 SCSI 设备（`libata-core.c:6116`）。因此 **开机识别设备完全复用错误恢复路径**：EH 先 reset，再读 IDENTIFY，最后 attach SCSI 设备，使“上电探测”与“热插拔/错误后重探”共享同一套代码。

探测核心是 IDENTIFY DEVICE / IDENTIFY PACKET DEVICE。`ata_dev_read_id()`（`libata-core.c:1792`）按 class 选 `ATA_CMD_ID_ATA` 或 `ATA_CMD_ID_ATAPI`，协议固定 `ATA_PROT_PIO` 并强制 `ATA_TFLAG_POLLING`。读回 256 个 16 位 word 后做字节序交换与合法性检查。`ata_dev_configure()`（`libata-core.c:2971`）解析：

| word | 常量 | 用途 |
|---:|---|---|
| 0 | `ATA_ID_CONFIG` | 设备类型、removable、LBA |
| 1/3/6 | `ATA_ID_CYLS/HEADS/SECTORS` | CHS 几何 |
| 10–19 / 23–26 / 27–46 | `ATA_ID_SERNO/FW_REV/PROD` | 序列号、固件、型号（供 INQUIRY） |
| 60–61 / 100–103 | `ATA_ID_LBA_CAPACITY[_2]` | LBA28 / LBA48 容量 |
| 75 | `ATA_ID_QUEUE_DEPTH` | NCQ 队列深度（低 5 位 + 1） |
| 76 | `ATA_ID_SATA_CAPABILITY` | bit8 = NCQ 支持 |
| 80 / 85–86 / 88 | 版本、命令集使能、UDMA 模式 | 特性判定 |

容量、逻辑扇区大小、FUA、DevSlp、ZAC、CDL 等特性都在这里翻译成 `dev->flags` / `dev->quirks`。NCQ 还要结合 host 能力，逻辑在 `ata_dev_config_ncq()`（`libata-core.c:2408`）：

```c
	if (ap->flags & ATA_FLAG_NCQ) {
		hdepth = min(ap->scsi_host->can_queue, ATA_MAX_QUEUE);
		dev->flags |= ATA_DFLAG_NCQ;
	}
```

`hdepth` 是 host 深度，`ddepth = ata_id_queue_depth(dev->id)` 是设备声明深度，取小后打印 “NCQ (depth N)”；若支持 FPDMA AA 还会发 SET FEATURES 打开 Auto-Activate。只有 `ATA_DFLAG_NCQ` 置位，后续 SCSI 队列深度才会提高。

配置完成后 EH 置 `ATA_PFLAG_SCSI_HOTPLUG`，`hotplug_task` 调 `ata_scsi_scan_host()` → `__scsi_add_device()` 创建 `scsi_device`，`sd`/`sr` 绑定并生成 `/dev/sdX`。至此 ATA 设备正式“变成” SCSI 设备。

## 4. 命令提交路径

一次读写的完整链路：

```
submit_bio → blk-mq → scsi_queue_rq
 → shost->hostt->queuecommand = ata_scsi_queuecmd
    → ata_scsi_find_dev → __ata_scsi_queuecmd
       → ata_get_xlat_func（opcode 查表）
       → ata_scsi_translate
          → ata_scsi_qc_new（取 tag，初始化 qc）
          → xlat_func（如 ata_scsi_rw_xlat → ata_build_rw_tf）
          → ata_scsi_qc_issue → ops->qc_defer（必要时推迟）
             → ata_qc_issue → ops->qc_prep → ops->qc_issue
```

`ata_scsi_queuecmd()`（`libata-scsi.c:4699`）只做：拿 `ap->lock`、把 `scsi_device` 映射成 `ata_device`、调用 `__ata_scsi_queuecmd()`。找不到设备则 `DID_BAD_TARGET`。`ata_scsi_translate()` 是“分配 qc + 翻译 + 下发”的骨架：

```c
	qc = ata_scsi_qc_new(dev, cmd);
	if (!qc)
		return 0;

	/* data is present; dma-map it */
	if (cmd->sc_data_direction == DMA_FROM_DEVICE ||
	    cmd->sc_data_direction == DMA_TO_DEVICE) {
		if (unlikely(scsi_bufflen(cmd) < 1)) {
			ata_dev_warn(dev, "WARNING: zero len r/w req\n");
			cmd->result = (DID_ERROR << 16);
			goto done;
		}
		ata_sg_init(qc, scsi_sglist(cmd), scsi_sg_count(cmd));
		qc->dma_dir = cmd->sc_data_direction;
	}
	qc->complete_fn = ata_scsi_qc_complete;
	if (xlat_func(qc))
		goto done;
	return ata_scsi_qc_issue(ap, qc);
```

本版本 tag 直接来自块层 `rq->tag`（SAS host 例外，用 `cmd->budget_token`），不再有独立的 `ata_qc_new_init()` 轮询分配，`ata_scsi_qc_new()` 见 `libata-scsi.c:740`。

`ata_qc_issue()`（`libata-core.c:5195`）负责“记账 + 下硬件”：先检查“非 NCQ 同时只能有一个在飞”，按协议更新 `sactive` 或 `active_tag`，再调 `qc_prep` / `qc_issue`：

```c
	if (ata_is_ncq(prot)) {
		WARN_ON_ONCE(link->sactive & (1 << qc->hw_tag));
		if (!link->sactive)
			ap->nr_active_links++;
		link->sactive |= 1 << qc->hw_tag;
	} else {
		WARN_ON_ONCE(link->sactive);
		ap->nr_active_links++;
		link->active_tag = qc->tag;
	}
	qc->flags |= ATA_QCFLAG_ACTIVE;
	ap->qc_active |= 1ULL << qc->tag;
```

若 `qc_prep` 返回错误（如 DMA 映射失败），`ata_qc_issue()` 直接 `ata_qc_complete(qc)` 走错误完成，而不是继续写硬件。底层 `qc_issue` 有三种典型实现：

| 实现 | 位置 | 下发动作 |
|---|---|---|
| SFF（PIO/nodata） | `ata_sff_qc_issue()`，`libata-sff.c:1282` | 选设备、`sff_tf_load`、`sff_exec_command`，PIO 交给任务或 IRQ |
| BMDMA（SFF + DMA） | `ata_bmdma_qc_issue()`，`libata-sff.c:2641` | 非 DMA 回退 SFF；DMA 时 `bmdma_setup` + `bmdma_start` |
| AHCI | `ahci_qc_issue()`，`libahci.c:2044` | 写 `PORT_CMD_ISSUE` 的 tag 位，硬件自行取命令表 |

`ata_sff_qc_issue()` 用 `ap->hsm_task_state` 表示主机状态机位置（`HSM_ST_FIRST`、`HSM_ST`、`HSM_ST_LAST`）；PIO 读由中断推进，PIO 写先排 `ata_sff_pio_task`，ATAPI 无 CDB 中断时也走轮询任务。SFF taskfile 加载就是一组 `iowrite8`：

```c
	if (is_addr) {
		iowrite8(tf->feature, ioaddr->feature_addr);
		iowrite8(tf->nsect, ioaddr->nsect_addr);
		iowrite8(tf->lbal, ioaddr->lbal_addr);
		iowrite8(tf->lbam, ioaddr->lbam_addr);
		iowrite8(tf->lbah, ioaddr->lbah_addr);
	}

	if (tf->flags & ATA_TFLAG_DEVICE)
		iowrite8(tf->device, ioaddr->device_addr);

	ata_wait_idle(ap);
```

完成路径与提交对称：IRQ → 驱动读完成位图 → `ata_qc_complete_multiple()` 或 `ata_qc_complete()` → `__ata_qc_complete()` 清 `ATA_QCFLAG_ACTIVE` 与 tag 位 → `qc->complete_fn(qc)`。普通 SCSI 命令的回调是 `ata_scsi_qc_complete()`（`libata-scsi.c:1816`），生成 sense 后 `scsi_done(cmd)`。

`ata_qc_complete_multiple()`（`libata-sata.c:757`）用新旧 `qc_active` 异或求完成位图，若新位图出现“未在飞却声称完成”的位则报 “illegal qc_active transition” 并触发 HSM 错误。NCQ 完成前先调 `ops->qc_ncq_fill_rtf()` 填结果 taskfile。

## 5. AHCI 控制器实现
`ahci.c` 是 PCI 驱动与板级 quirk，`libahci.c` 是可复用的引擎；`ahci_ops` 在 `libahci.c:155` 定义，`ahci_port_start`（`libahci.c:2479`）为每个 port 分配三块 DMA 内存。HBA 全局寄存器（相对 ABAR，`ahci.h:69` 起）：

| 偏移 | 寄存器 | 作用 |
|---:|---|---|
| 0x00 | `HOST_CAP` | NCQ(bit30)、64bit(bit31)、PMP、FBS、SSS |
| 0x04 | `HOST_CTL` | `RESET`、`IRQ_EN`、`AHCI_EN` |
| 0x08 | `HOST_IRQ_STAT` | 全局中断汇总（电平锁存，处理完写清） |
| 0x0c / 0x10 | `HOST_PORTS_IMPL` / `HOST_VERSION` | 端口位图 / 版本 |

每 port 寄存器块从 `0x100 + port_no * 0x80` 开始：`PORT_LST_ADDR(0x00)` 命令列表基址、`PORT_FIS_ADDR(0x08)` 接收 FIS 基址、`PORT_IRQ_STAT/MASK(0x10/0x14)`、`PORT_CMD(0x18)`（`START`、`FIS_RX`、`PMP`、ICC）、`PORT_SIG(0x24)`、`PORT_SCR_STAT/CTL/ERR/ACT(0x28–0x34)`、`PORT_CMD_ISSUE(0x38)`、`PORT_SCR_NTF(0x3c)`、`PORT_FBS(0x40)`。

port 私有内存在 `ahci_port_start()` 中一次 `dmam_alloc_coherent()` 分配：

```
 pp->cmd_slot → [32 × ahci_cmd_hdr(32B)] = 1024 B
 pp->rx_fis   → [256 B：0x20 PIO Setup、0x40 D2H、0x58 SDB、0x60 Unknown]
 pp->cmd_tbl  → [32 × (128B 头部 + 168 × 16B PRDT)]
```

`ahci_cmd_hdr` 为 32 字节：`opts`、`status`、`tbl_addr`、`tbl_addr_hi` 与保留（`ahci.h:283`）。`opts` 的 `n_elem << 16` 是 PRDT 项数，低 5 位是 CFIS 长度，bit5/6/7 为 ATAPI/WRITE/PREFETCH。PRDT 项 `ahci_sg` 为 16 字节：`addr`、`addr_hi`、保留、`flags_size`（长度减 1）。填表与下发分别由 `ahci_qc_prep()` 和 `ahci_qc_issue()` 完成：

```c
	cmd_tbl = pp->cmd_tbl + qc->hw_tag * AHCI_CMD_TBL_SZ;
	ata_tf_to_fis(&qc->tf, qc->dev->link->pmp, 1, cmd_tbl);
	if (is_atapi) {
		memset(cmd_tbl + AHCI_CMD_TBL_CDB, 0, 32);
		memcpy(cmd_tbl + AHCI_CMD_TBL_CDB, qc->cdb, qc->dev->cdb_len);
	}
	n_elem = 0;
	if (qc->flags & ATA_QCFLAG_DMAMAP)
		n_elem = ahci_fill_sg(qc, cmd_tbl);
	opts = cmd_fis_len | n_elem << 16 | (qc->dev->link->pmp << 12);
	if (qc->tf.flags & ATA_TFLAG_WRITE)
		opts |= AHCI_CMD_WRITE;
	if (is_atapi)
		opts |= AHCI_CMD_ATAPI | AHCI_CMD_PREFETCH;
	ahci_fill_cmd_slot(pp, qc->hw_tag, opts);
```

```c
	if (ata_is_ncq(qc->tf.protocol))
		writel(1 << qc->hw_tag, port_mmio + PORT_SCR_ACT);
	if (pp->fbs_enabled && pp->fbs_last_dev != qc->dev->link->pmp) {
		u32 fbs = readl(port_mmio + PORT_FBS);
		fbs &= ~(PORT_FBS_DEV_MASK | PORT_FBS_DEC);
		fbs |= qc->dev->link->pmp << PORT_FBS_DEV_OFFSET;
		writel(fbs, port_mmio + PORT_FBS);
		pp->fbs_last_dev = qc->dev->link->pmp;
	}
	writel(1 << qc->hw_tag, port_mmio + PORT_CMD_ISSUE);
```

中断分两种：单 IRQ 读 `HOST_IRQ_STAT`，与 `port_map` 掩码后逐 port 分发；MSI/MSI-X 每 port 一个向量，`ahci_multi_irqs_intr_hard()` 直接读该 port 的 `PORT_IRQ_STAT`。两条路进 `ahci_handle_port_interrupt()`（`libahci.c:1891`）：先处理错误（`PORT_IRQ_ERROR`），再处理 SDB FIS（异步通知），最后 `ahci_qc_complete()`：

```c
	if (unlikely(status & PORT_IRQ_ERROR)) {
		/*
		 * Before getting the error notification, we may have
		 * received SDB FISes notifying successful completions.
		 * Handle these first and then handle the error.
		 */
		ahci_qc_complete(ap, port_mmio);
		ahci_error_intr(ap, status);
		return;
	}
```

`ahci_qc_complete()` 读 `PORT_SCR_ACT`（NCQ）或 `PORT_CMD_ISSUE`（非 NCQ）得在飞 tag 位图，交给 `ata_qc_complete_multiple()`；若返回负值且不在重置中，置 `AC_ERR_HSM` 并 `ata_port_freeze()`。

AHCI 还支持 FBS（用于 PMP）：`ahci_pmp_qc_defer()` 决定是否按 PMP 命令切换推迟，`PORT_FBS.DEV` 指定本次 FIS 发给哪个下挂设备。`HOST_CAP_NCQ` 打开 `ATA_FLAG_NCQ | ATA_FLAG_FPDMA_AA | ATA_FLAG_FPDMA_AUX`，`HOST_CAP_PMP` 打开 `ATA_FLAG_PMP`（`ahci.c:2962` 起）。

## 6. 错误处理

libata 的异常处理寄生在 SCSI EH 上。触发链：

```
设备报错 / IRQ 超时 / HSM 违反
 → 驱动置 eh_info（err_mask、action、serror）并 ata_port_freeze
 → scsi_schedule_eh → SCSI EH 线程 → ata_scsi_error
    → ata_scsi_cmd_error_handler（整理超时命令）
    → ata_scsi_port_error_handler（取得 EH 所有权，跑 error_handler）
       → ata_std_error_handler
          → ata_eh_autopsy  → ata_eh_report
          → ata_eh_recover  → ata_eh_finish
```

`ata_scsi_error()`（`libata-eh.c:569`）先调 `ata_scsi_cmd_error_handler()` 统计超时；只有存在超时或 `ata_port_eh_scheduled(ap)` 为真时才进入完整 recovery，否则只 flush 已完成队列。这个快速路径避免无失败的 EH 走完整重置。

默认 `ata_std_error_handler()`（`libata-eh.c:4109`）串起“尸检—报告—恢复—收尾”：`ata_eh_autopsy()` 分析 SError 与 taskfile 决定 action，`ata_eh_report()` 打印 Emask/SAct/SErr，`ata_eh_recover()` 重置、重验、重设模式并处理失败退让，最后 `ata_eh_finish()` 完成收尾命令。若 recover 返回非 0，该 link 上所有设备被 `ata_dev_disable()`。

错误分类用 `AC_ERR_*` 位掩码，贯穿 qc、eh_info 与日志：

| 位 | 名称 | 含义 |
|---:|---|---|
| bit0 | `AC_ERR_DEV` | 设备通过 status/error 报错 |
| bit1 | `AC_ERR_HSM` | 主机状态机违反（协议错误） |
| bit2 | `AC_ERR_TIMEOUT` | 命令超时 |
| bit3 | `AC_ERR_MEDIA` | 介质错误 |
| bit4 | `AC_ERR_ATA_BUS` | ATA 总线错误（SError 持久/数据错误） |
| bit5 | `AC_ERR_HOST_BUS` | 主机总线/DMA 错误 |
| bit6/7/8 | `AC_ERR_SYSTEM/INVALID/OTHER` | 内部错误 / 参数非法 / 未知 |
| bit9/10 | `AC_ERR_NODEV_HINT/NCQ` | 无设备提示 / NCQ 失败标记（日志 `<F>`） |

`ata_eh_analyze_serror()` 把 SError 翻译成动作：持久/数据错误 → ATA 总线错误 + 重置；协议错误 → HSM 违反 + 重置；内部错误 → 系统错误 + 重置；PHYRDY 变化或设备交换视为热插拔：

```c
	if (serror & (SERR_PERSISTENT | SERR_DATA)) {
		err_mask |= AC_ERR_ATA_BUS;
		action |= ATA_EH_RESET;
	}
	if (serror & SERR_PROTOCOL) {
		err_mask |= AC_ERR_HSM;
		action |= ATA_EH_RESET;
	}
	if (serror & SERR_INTERNAL) {
		err_mask |= AC_ERR_SYSTEM;
		action |= ATA_EH_RESET;
	}
```

`ata_eh_reset()`（`libata-eh.c:2827`）策略是“硬重置优先，软重置兜底”，按逐次放宽的超时表重试（`libata-eh.c:81`）：

```c
static const unsigned int ata_eh_reset_timeouts[] = {
	10000,	/* most drives spin up by 10sec */
	10000,	/* > 99% working drives spin up before 20sec */
	35000,	/* give > 30 secs of idleness for outlier devices */
	 5000,	/* and sweet one last chance */
	UINT_MAX, /* > 1 min has elapsed, give up */
};
```

流程为 `prereset → softreset/hardreset → postreset`。`prereset` 返回 `-ENOENT` 视为“端口被禁用”而非错误；每次尝试前若距上次重置太近，会等 `ATA_EH_RESET_COOL_DOWN`。重置后由 signature 重新分类设备（`ata_dev_classify()`：`0/0 → ATA`、`0x14/0xeb → ATAPI`、`0x69/0x96 → PMP`、`0x3c/0xc3 → SEMB`、`0xcd/0xab → ZAC`）。

重试与退让在 `ata_eh_handle_dev_fail()`（`libata-eh.c:3739`）：

| 返回值 | 含义 | 处理 |
|---|---|---|
| `-ENODEV` | 设备消失或 IDENTIFY 非法 | 排入 `probe_mask`，按 `-EINVAL` 处理 |
| `-EINVAL` | 配置非法 | 只再给一次机会（`tries = min(tries, 1)`） |
| `-EIO` | I/O 失败 | 最后一次时降速（`sata_down_spd_limit`）并降 PIO |
| `-EAGAIN` | 无损失重试 | 不扣 `tries` |

`tries[]` 初值 `ATA_EH_DEV_TRIES = 3`；用尽后 `ata_dev_disable()`，链路离线还会 `ata_eh_detach_dev()` 移除 SCSI 设备。EH 状态机由 `ap->pflags` 驱动：

```
INITIALIZING ─probe→ LOADING ─EH→ EH_IN_PROGRESS ─成功→ EH complete
任意时刻 error IRQ → EH_PENDING；超时/冻结 → FROZEN ─reset 成功→ 解冻
UNLOADING → EH 自杀（UNLOADED）
```

同一 host 的 EH 串行化由 `host->eh_mutex` 与 `ata_eh_acquire/release` 保证；`ata_scsi_port_error_handler()` 末尾若发现 `EH_PENDING` 又被置位会 `goto repeat`，最多 `ATA_EH_MAX_TRIES = 5` 次，之后打印 “EH pending after 5 tries, giving up”。

## 7. SCSI 翻译层

`libata-scsi.c` 把 CDB 分两类：**可翻译**（读、写、flush、verify、WRITE SAME、ATA pass-through、ZBC、安全协议、START STOP）查表得 `xlat_func`，生成 ATA taskfile；**可模拟**（INQUIRY、MODE SENSE、READ CAPACITY、REQUEST SENSE、TEST UNIT READY）由 libata 自己造响应。分派表在 `ata_get_xlat_func()`（`libata-scsi.c:4495`）：

```c
	switch (cmd) {
	case READ_6:
	case READ_10:
	case READ_16:
	case WRITE_6:
	case WRITE_10:
	case WRITE_16:
		return ata_scsi_rw_xlat;
	case WRITE_SAME_16:
		return ata_scsi_write_same_xlat;
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		if (ata_try_flush_cache(dev))
			return ata_scsi_flush_xlat;
		break;
	case VERIFY:
	case VERIFY_16:
		return ata_scsi_verify_xlat;
	case ATA_12:
	case ATA_16:
		return ata_scsi_pass_thru;
	case VARIABLE_LENGTH_CMD:
		return ata_scsi_var_len_cdb_xlat;
```

其余还包括 `MODE_SELECT[_10]`、`ZBC_IN/OUT`、`SECURITY_PROTOCOL_IN/OUT`、`START_STOP`。未命中翻译表的命令交给 `ata_scsi_simulate()`。

读写翻译是热路径。`ata_scsi_rw_xlat()`（`libata-scsi.c:1546`）按 CDB 长度取 LBA/块数，处理 FUA，并修正“6 字节 CDB 的 0 表示 256 块”：

```c
	/* Check and compose ATA command */
	if (!n_block)
		/* For 10-byte and 16-byte SCSI R/W commands, transfer
		 * length 0 means transfer 0 block of data.
		 * However, for ATA R/W commands, sector count 0 means
		 * 256 or 65536 sectors, not 0 sectors as in SCSI.
		 *
		 * WARNING: one or two older ATA drives treat 0 as 0...
		 */
		goto nothing_to_do;

	qc->flags |= ATA_QCFLAG_IO;
	qc->nbytes = n_block * scmd->device->sector_size;

	rc = ata_build_rw_tf(qc, block, n_block, tf_flags, dld, class);
	if (likely(rc == 0))
		return 0;
```

`ata_build_rw_tf()` 依据 NCQ/LBA48/FUA 选择 `READ/WRITE FPDMA QUEUED`、`READ/WRITE DMA EXT` 或 PIO 版本，并设置 `ATA_TFLAG_LBA48` / `ATA_TFLAG_FUA`。超范围 LBA 返回 `ILLEGAL_REQUEST, asc=0x21`。

模拟命令走 `ata_scsi_simulate()`。INQUIRY 由一组 `ata_scsiop_inq_*()` 处理，标准 INQUIRY 用 IDENTIFY 的型号/固件串填 vendor/product/revision；还模拟 VPD 页 `0x00`、`0x80`（序列号）、`0x83`（device identification）、`0x89`（ATA info）、`0xb0`（block limits）、`0xb1`、`0xb2`、`0xb6`（ZBC）、`0xb9`。响应缓冲通过全局 `ata_scsi_rbuf` 加锁复用（`ata_scsi_rbuf_fill()`，`libata-scsi.c:2005`）。

错误返回时 `ata_to_sense_error()` 把 ATA status/error 映射成 sense key / ASC / ASCQ。status 表优先处理 BUSY、DF、CORR；error 位表按最佳匹配扫描：

```c
	static const unsigned char sense_table[][4] = {
		/* BBD|ECC|ID|MAR */
		{0xd1,		ABORTED_COMMAND, 0x00, 0x00},
			// Device busy                  Aborted command
		/* BBD|ECC|ID */
		{0xd0,		ABORTED_COMMAND, 0x00, 0x00},
			// Device busy                  Aborted command
		/* ECC|MC|MARK */
		{0x61,		HARDWARE_ERROR, 0x00, 0x00},
			// Device fault                 Hardware error
		/* ICRC|ABRT */		/* NB: ICRC & !ABRT is BBD */
		{0x84,		ABORTED_COMMAND, 0x47, 0x00},
			// Data CRC error               SCSI parity error
		/* MC|ID|ABRT|TRK0|MARK */
		{0x37,		NOT_READY, 0x04, 0x00},
```

例如 `0x40`（不可纠正 ECC）与 `0x80`（块标记坏）都映射为 `MEDIUM_ERROR, 0x11, 0x04`；`0x10`（ID not found）→ `ILLEGAL_REQUEST, 0x21`；`0x20`（介质变化）→ `UNIT_ATTENTION, 0x28`。status 侧：`ATA_BUSY → ABORTED_COMMAND`、`ATA_DF → HARDWARE_ERROR/0x44`、`ATA_CORR → RECOVERED_ERROR`。

最终 sense 由 `ata_gen_ata_sense()` 或 `ata_gen_passthru_sense()` 写入 `scmd->sense_buffer`，并可用 `ata_scsi_set_sense_information()` 附带出错 LBA。ATA pass-through（`ATA_12/16`）在 `CK_COND=1` 时即使成功也返回 sense，hdparm/ smartctl 依赖这一行为。

## 8. 电源管理与热插拔

libata 的 PM 只有薄薄一层设备模型回调，真正工作交给 EH。`ata_port_pm_ops`（`libata-core.c:5513`）注册 suspend/freeze/poweroff/resume/thaw/restore 与 runtime 三件套；这些函数最终都调用 `ata_port_request_pm()`，把 PM 请求变成 EH 动作：

```c
	/* Request PM operation to EH */
	ap->pm_mesg = mesg;
	ap->pflags |= ATA_PFLAG_PM_PENDING;
	ata_for_each_link(link, ap, HOST_FIRST) {
		link->eh_info.action |= action;
		link->eh_info.flags |= ehi_flags;
	}
	ata_port_schedule_eh(ap);
```

挂起时 EH 调 `ata_eh_handle_port_suspend()`：设备置 standby（FREEZE 除外，因为随后还有 HIBERNATE）、quiesce SCSI 设备、调 `ops->port_suspend`；恢复时 `ata_eh_handle_port_resume()` 重新 reset、置 `ATA_EH_SET_ACTIVE` 唤醒设备并重验。`ata_port_runtime_idle()` 对非 ZPODD 的 ATAPI 光驱直接返回 `-EBUSY`，避免上层每几秒轮询介质导致反复 reset。

链路省电由 `enum ata_lpm_policy` 与 sysfs `link_power_management_policy` 控制，可选值来自 `ata_lpm_policy_names[]`（`libata-sata.c:894`）：`keep_firmware_settings`、`max_performance`、`medium_power`、`med_power_with_dipm`、`min_power_with_partial`、`min_power`。写该属性只改 `ap->target_lpm_policy` 并调度 EH，由 `ata_eh_link_set_lpm()` → `ops->set_lpm`（AHCI 为 `ahci_set_lpm`）落到 `PORT_CMD.ICC` 的 active/partial/slumber。

热插拔同样是“SError → EH → SCSI 扫描”。`ata_ehi_hotplugged()` 设置 `probe_mask`、`ATA_EH_RESET | ATA_EH_ENABLE_LINK` 和 `AC_ERR_ATA_BUS`；EH 恢复时 `ata_eh_revalidate_and_attach()` 对未知设备重新 `ata_dev_read_id()` + `ata_dev_configure()`，并置 `ATA_PFLAG_SCSI_HOTPLUG`：

```
ata_eh_recover → PFLAG_SCSI_HOTPLUG → hotplug_task = ata_scsi_hotplug
 → ata_scsi_handle_link_detach（下线并移除旧 scsi_device）
 → ata_scsi_scan_host → __scsi_add_device(shost, channel, id, 0)
```

PMP 下挂设备用 `channel = link->pmp`，主机直连设备用 `id = dev->devno`。设备被拔掉时 `ata_scsi_offline_dev()` 先把 sdev 置 `SDEV_OFFLINE`，再由 `ata_scsi_remove_dev()` 调 `scsi_remove_device()`。

ZPODD（Zero Power ODD）在 `libata-zpodd.c` 实现：`ata_dev_configure()` 对带 `ATA_DFLAG_DA` 的 ATAPI 设备调 `zpodd_init(dev)`，用 GET CONFIGURATION 判断槽式/抽屉式光驱，runtime suspend 时真正断电。它依赖 `ATA_ACPI && PM`，而本树 `.config` 中 `CONFIG_ACPI` 与 `CONFIG_PM` 均未启用，因此 **源码存在，当前配置未启用**。

## 9. 调试与观测
libata 通过 transport class 导出三层 sysfs 对象，名字为 `ata_port`、`ata_link`、`ata_device`（`libata-transport.c:198/497/628`）：

```
/sys/class/ata_port/ata1/     id  port_no  nr_pmp_links  idle_irq
/sys/class/ata_link/ata1.0/   hw_sata_spd_limit  sata_spd_limit  sata_spd
/sys/class/ata_device/dev1.0/ class  id  pio_mode  dma_mode  xfer_mode
                              spdn_cnt  ering  gscr  trim
```

`ata_device` 的只读属性包括 `class`、`pio_mode`、`dma_mode`、`xfer_mode`、`spdn_cnt`、`ering`、`id`、`gscr`、`trim`（`ata_device_attr_attrs[]`，`libata-transport.c:480`）。`id` 打印 256 个 IDENTIFY word，`ering` 是最近 32 条错误历史（`ATA_ERING_SIZE = 32`）。

`port_no` 属性故意加 1，以与日志中的 `ata1` 对齐；link 的三个速度属性经 `sata_spd_string()` 转成 `1.5 Gbps` / `3.0 Gbps` / `6.0 Gbps`。

启动参数 `libata.force=` 是定位兼容性问题的利器。解析函数 `ata_parse_force_one()`（`libata-core.c:6658`），语法 `[port[.dev]:]value[,...]`，port 对应 `ap->print_id`（即 `ataN` 的 N）。常用值：

| 类别 | 示例 | 效果 |
|---|---|---|
| 线缆 | `40c`、`80c`、`sata`、`unk` | 强制 `ATA_CBL_*` |
| 速率 | `1.5Gbps`、`3.0Gbps`、`6.0Gbps` | 限制 SATA 链路速度 |
| 传输模式 | `pio0`…`pio6`、`mwdma0`…`4`、`udma0`…`7` | 强制 xfer mask |
| 链路行为 | `nohrst`、`nosrst`、`norst`、`rstonce`、`dbdelay` | 禁用硬/软重置或只重置一次 |
| quirk | `noncq`、`noncqtrim`、`notrim`、`nodma`、`nolpm`、`nosetxfer` | 打对应 quirk |
| 其他 | `disable`、`dump_id`、`max_sec_lba48`、`fua` | 禁用设备/打印 IDENTIFY 等 |

例如 `libata.force=1.00:noncq,2:udma5` 表示“ata1 的 0 号设备禁用 NCQ，ata2 限制 UDMA5”。force 在 `ata_dev_configure()` 与 port/link 初始化阶段生效，并打印 `FORCE:` 提示。

常见 dmesg 形态：

| 日志片段 | 含义 |
|---|---|
| `ata1: SATA max UDMA/133 abar ...` | port 注册时的能力横幅 |
| `ata1.00: ATA-8: ..., 500118192 sectors` | IDENTIFY 成功，型号/容量 |
| `ata1.00: configured for UDMA/133` | 传输模式设置完成 |
| `exception Emask 0x0 SAct 0x... SErr 0x... action 0x6 frozen` | EH 报告：错误掩码、NCQ 活跃位、SError、动作、已冻结 |
| `SError: { BadCRC ... }` | SATA 物理层错误，多为线缆/接触问题 |
| `failed command: READ FPDMA QUEUED` | 失败命令名，`<F>` 表示 NCQ 失败命令 |
| `Emask 0x1 (device error)` | 设备通过 status/error 报错 |
| `hard resetting link` / `link is slow to respond, please be patient` | EH 正在硬重置 / 链路长时间未就绪 |
| `ata1.00: disabled` / `ata1: EH complete` | 重试用尽设备被禁用 / 本次 EH 结束 |

`libata.force=1.00:dump_id` 会以十六进制打印完整 IDENTIFY 页，配合 `/sys/class/ata_device/dev1.0/id` 可对比“设备声明”与“内核解析”。tracepoint 定义在 `include/trace/events/libata.h`，可在 `/sys/kernel/tracing/events/libata/` 打开 `ata_qc_issue`、`ata_qc_complete_*`、`ata_eh_link_autopsy`、`ata_link_hardreset_begin/end` 观察时序。

排障经验：`BadCRC`/`Dispar`/`10B8B` 优先查线缆与背板；频繁 `hard resetting link` 且伴随 `link is slow to respond` 多为电源/背板/硬盘启动慢；`Emask 0x1 (device error)` 且 LBA 固定多为介质坏块；`illegal qc_active transition` 或 `HSM violation` 则指向控制器/DMA 配置问题。

## 10. 配置项

`drivers/ata/Kconfig:12` 的 `menuconfig ATA` 是一个 tristate 总开关：依赖 `HAS_IOMEM`、`BLOCK`，并 `select SCSI`、`select GLOB`；因此 `CONFIG_ATA=y` 会连带打开 SCSI 核心，而 `CONFIG_ATA=n` 时整个 `drivers/ata/` 不参与编译。

本树 `.config` 实际启用项：

| 配置 | 值 | 作用 |
|---|---|---|
| `CONFIG_ATA` | `y` | libata 总开关 |
| `CONFIG_SATA_HOST` | `y` | SATA 链路/SCR/NCQ 辅助（`libata-sata.o`） |
| `CONFIG_SATA_AHCI` / `_PLATFORM` | `y` | AHCI PCI 驱动 / 平台框架（`ahci.o libahci.o`） |
| `CONFIG_SATA_SIL24` | `y` | Sil24 SATA 控制器 |
| `CONFIG_ATA_SFF` / `ATA_BMDMA` | `y` | SFF/IDE 兼容接口与总线主 DMA |
| `CONFIG_SATA_PMP` | `y` | Port Multiplier（`libata-pmp.o`） |
| `CONFIG_ATA_VERBOSE_ERROR` / `ATA_FORCE` | `y` | 详细 SError 日志 / `libata.force=` |
| `CONFIG_SATA_MOBILE_LPM_POLICY` | `3` | 默认 LPM（`med_power_with_dipm`） |
| `CONFIG_PATA_PLATFORM` / `_OF_PLATFORM` | `y` | 平台 PATA 驱动 |

当前配置未启用、但源码完整存在的项：

| 配置 | 状态 | 影响 |
|---|---|---|
| `CONFIG_ACPI` | 未设置 | `libata-acpi.o` 不编译，`ATA_ACPI`/ZPODD 无法启用 |
| `CONFIG_PM` | 未设置 | `ata_port_pm_ops` 与 runtime PM 代码不编译 |
| `CONFIG_ATA_ACPI` | 未启用（依赖 ACPI） | 无 `_GTF/_GTM` 处理 |
| `CONFIG_SATA_ZPODD` | 未启用（依赖 ATA_ACPI && PM） | 光驱零功耗功能不可用 |
| `CONFIG_ATA_PIIX` / `ATA_GENERIC` | 未设置 | 无 Intel PIIX / 通用 PCI IDE 驱动 |
| `CONFIG_SATA_MV/NV/PROMISE` | 未设置 | 对应厂商 SATA 驱动不编译 |
| `CONFIG_PATA_TIMINGS` | 未启用 | 无 PATA 时序表支持 |

`libata.o` 的可选成员由 Makefile 控制：

```make
libata-y	:= libata-core.o libata-scsi.o libata-eh.o \
	libata-transport.o libata-trace.o
libata-$(CONFIG_SATA_HOST)	+= libata-sata.o
libata-$(CONFIG_ATA_SFF)	+= libata-sff.o
libata-$(CONFIG_SATA_PMP)	+= libata-pmp.o
libata-$(CONFIG_ATA_ACPI)	+= libata-acpi.o
libata-$(CONFIG_SATA_ZPODD)	+= libata-zpodd.o
libata-$(CONFIG_PATA_TIMINGS)	+= libata-pata-timings.o
```

## 11. 关键文件清单
| 路径 | 行数 | 阅读价值 |
|---|---:|---|
| `include/linux/libata.h` / `ata.h` | 2183 / 1050 | 结构、flag、IDENTIFY word、命令码 |
| `drivers/ata/libata-core.c` | 6967 | host/port/dev、qc、IDENTIFY、模式设置 |
| `drivers/ata/libata-scsi.c` | 5169 | 翻译、模拟、sense、扫描、热插拔 |
| `drivers/ata/libata-eh.c` | 4266 | 异常检测、重置、重试、报告 |
| `drivers/ata/libata-sff.c` | 3207 | SFF taskfile、BMDMA、PIO 状态机 |
| `drivers/ata/libata-sata.c` | 1737 | SATA 链路、SCR、LPM、NCQ 完成 |
| `drivers/ata/libata-pmp.c` | 1118 | PMP GSCR 与命令切换 |
| `drivers/ata/libata-acpi.c` | 1103 | ACPI `_GTF/_GTM`（当前未编译） |
| `drivers/ata/libata-transport.c` | 803 | sysfs transport class |
| `drivers/ata/libata-zpodd.c` | 294 | ZPODD（当前未编译） |
| `drivers/ata/ahci.c` / `libahci.c` / `ahci.h` | 3123 / 2774 / 475 | AHCI PCI 驱动、通用引擎、寄存器 |
| `drivers/ata/Kconfig` / `Makefile` | 1194 / 133 | 配置项与 `libata.o` 组成 |

## 12. 延伸阅读

- 块层基础：[`../block/block-01-core-framework.md`](../block/block-01-core-framework.md)
- SCSI/UFS/eMMC 与中间层：[`../scsi-ufs/01-scsi-ufs-emmc.md`](../scsi-ufs/01-scsi-ufs-emmc.md)
- 设备模型与总线绑定：[`../../03-drivers/bus/01-bus-framework.md`](../../03-drivers/bus/01-bus-framework.md)
- NVMe 主机驱动对照：[`../nvme/nvme-pcie-host-analysis.md`](../nvme/nvme-pcie-host-analysis.md)
- 内核文档：`Documentation/driver-api/libata.rst`
- 启动参数：`Documentation/admin-guide/kernel-parameters.txt`（`libata.force`、`libata.noacpi`）
- 规范：ATA/ATAPI-8（ACS-3/4）、Serial ATA 3.x、AHCI 1.3.1
