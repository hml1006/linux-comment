# mdBook 内容完整度报告

**对象**：《Linux 7.2 内核源码分析笔记》（`notes/book.toml` → `src = "."`，19 个顶级章节）
**日期**：2026-11-25
**方法**：SUMMARY 目录解析 + 全量文件扫描 + 链接/锚点校验（锚点对照 `mdbook build` 渲染产物逐一验证）+ 系统调用表比对（`include/uapi/asm-generic/unistd.h`）+ 实际构建验证。

> **注**：本报告第 1–7 节为**首次审计快照**；第 8 节记录两轮整改（结构整改 + 内容深度整改），第 9 节为**整改后最终核算**。以第 9 节数据为准。

---

## 1. 总体结论

| 维度 | 结果 | 评价 |
|---|---|---|
| 目录完整性 | SUMMARY 引用 534 个文件，磁盘缺失 **0**，重复条目 **0** | 优 |
| 构建 | `mdbook build` 成功（`create-missing = false` 严格模式） | 优 |
| 相对链接 | 1023 条全部有效，死链 **0** | 优 |
| 锚点链接 | 991 条全部命中渲染后 HTML 的 heading id，失效 **0** | 优 |
| 系统调用覆盖 | arm64 适用 327 个系统调用，笔记覆盖 **327/327 = 100%** | 优 |
| 子系统覆盖 | 架构/启动/中断/MM/调度/时间/同步/异步/VFS/存储/网络/安全/虚拟化/容器/调试/构建/Rust 均有专章 | 优 |
| 内容深度 | 整改后 525 页中 stub 0 / thin 110（全部为系统调用页、速查页、总结页豁免类）/ medium 362 / deep 53；**19 个章节评级全部为「优」**（见第 9 节） | 优 |
| 写作占位 | 疑似 TODO 标记 42 处，逐条人工复核**全部为正文术语**（如"等待完成"、swap 迁移"占位"entry），真实写作 TODO 为 **0** | 优 |
| 链接可移植性 | 833 条 `file://` 绝对路径链接，本地目标全部存在，但 **Web 版读者无法打开** | 需改进 |

**总评**：结构完整度与覆盖度接近满分，无缺页、无死链、无占位空章；内容体量充足（23.9 万行、47.5 万汉字、7616 个代码块）。经两轮整改后，**19 个章节的内容深度全部达到「优」**，原 SSD 专题洼地与 833 条 file:// 链接问题均已处理（链接按用户本地阅读场景保留）。

---

## 2. 规模统计

| 指标 | 数值 |
|---|---|
| Markdown 文件总数 | 530（SUMMARY 引用 528 + 主 SUMMARY.md + 简介页 kernel-map.md） |
| 顶级章节 | 19（0 总览 ～ 18 构建模块与 Rust） |
| 目录条目 | 602 个列表项 + 1 个简介链接；其中 67 个为空链接分组节点（mdBook 正常用法，非缺页） |
| 总行数 | 241,147（首次审计 222,495） |
| 汉字数（正文，不含代码块） | 487,683 |
| 代码块 | 7,680 个 |
| 最大单篇 | mm/memory_management.md（13,784 行）、block/block_layer_analysis.md（12,588 行）、虚拟化 01-kvm-arm64.md（1,698 行） |
| 标题规范 | 530 个文件全部有且仅有一个 H1（SUMMARY.md 除外） |

系统调用章（第 3 章）占全书篇幅 42.3%（94,112 行）、汉字 39.3%，是绝对主体。

---

## 3. 分章节完整度明细

深度分级按加权体量（代码块外汉字数 + 代码行×8）：stub < 200，thin < 1200，medium < 6000，deep ≥ 6000。下表为**整改后**数据（原始审计数据见第 8 节前文）。

**评级口径**（可复核）：「优」= 无 stub + 无低于 medium 的**实质页**；系统调用单页（≥600）、速查页（≥600）、总结/附录页（≥600）作为豁免类单独判定。deep 篇数仅作信息展示，不作为评级门槛（与首次审计对第 12、17 章的评级保持一致）。

| 章节 | 页数 | 行数 | 汉字 | 代码块 | stub/thin/medium/deep | 评估 |
|---|---|---|---|---|---|---|
| 0 总览 | 2 | 717 | 4,385 | 7 | 0/0/2/0 | 优 |
| 1 ARM64 体系结构 | 12 | 7,080 | 20,159 | 202 | 0/0/7/5 | 优 |
| 2 启动与中断 | 11 | 6,451 | 10,719 | 160 | 0/1/7/3 | 优（thin 为速查页 net-syscall.md，豁免） |
| 3 系统调用 | 353 | 94,901 | 155,863 | 2,853 | 0/108/239/6 | 优（thin 均为单调用页且 ≥600，豁免） |
| 4 内存管理 | 7 | 25,788 | 41,946 | 880 | 0/1/0/6 | 优（thin 为附录 summary 页，豁免） |
| 5 进程管理与调度 | 3 | 4,734 | 10,495 | 145 | 0/0/1/2 | 优 |
| 6 时间子系统 | 2 | 3,252 | 5,905 | 100 | 0/0/0/2 | 优 |
| 7 并发与同步 | 18 | 7,791 | 15,739 | 295 | 0/0/16/2 | 优 |
| 8 异步执行 | 16 | 3,908 | 11,887 | 155 | 0/0/16/0 | 优 |
| 9 数据结构与算法 | 2 | 4,687 | 11,633 | 130 | 0/0/1/1 | 优 |
| 10 驱动模型总线电源 | 8 | 6,405 | 13,668 | 161 | 0/0/5/3 | 优（含 kobject 专章） |
| 11 存储栈 | 21 | 30,609 | 69,392 | 1,007 | 0/0/14/7 | 优 |
| 12 文件系统 | 21 | 10,650 | 16,742 | 310 | 0/0/19/2 | 优（含 kernfs 专章） |
| 13 网络子系统 | 8 | 7,631 | 10,779 | 225 | 0/0/5/3 | 优 |
| 14 安全子系统 | 5 | 3,527 | 20,123 | 122 | 0/0/1/4 | 优 |
| 15 虚拟化 | 3 | 3,999 | 18,703 | 106 | 0/0/0/3 | 优 |
| 16 容器与隔离 | 2 | 2,107 | 9,656 | 73 | 0/0/0/2 | 优 |
| 17 调试与追踪 | 30 | 12,624 | 20,267 | 555 | 0/0/30/0 | 优 |
| 18 构建模块与 Rust | 4 | 4,227 | 18,857 | 194 | 0/0/0/4 | 优 |
| **合计** | **528** | **241,147** | **487,683** | **7,680** | **0/110/363/55** | **全部优** |

---

## 4. 系统调用覆盖度（核心亮点）

以 `include/uapi/asm-generic/unistd.h`（arm64 使用的通用表）为基准：

- 数字定义共 **348** 个（含 `__NR3264_` 族 11 个：fcntl/sendfile/mmap/stat/fstatat/lseek/truncate 等）。
- 其中 **21 个不适用于 arm64**：20 个 `*_time64`（32 位时间兼容调用，403–423）+ `sync_file_range2`（84，powerpc/arm32 变体）。
- **arm64 适用 327 个，笔记覆盖 327 个，覆盖率 100%**。
- 笔记共 350 篇 = 327 个适用调用 + 23 篇附加（别名：fork/open/stat/access/chmod/chown/umount/newfstatat/nice/setegid/seteuid；其他架构：arc_gettls/arc_settls/arc_usr_cmpxchg/or1k_atomic/riscv_flush_icache/riscv_hwprobe/cacheflush/set_thread_area；已废弃：sysfs/sysctl/dmesg/syscall）。
- 按 3.1–3.30 共 30 个类别组织，另含分类总表（arm64-syscall-table.md）与 vdso 机制篇。

---

## 5. 问题清单（首次审计记录，均已在整改中处理）

> **处理状态**：P1-1（SSD 洼地）→ 已删除 lightnvm/zns、nand/nvme 并入主书并扩写；P1-2（file:// 链接）→ 按本地阅读场景保留（见第 8 节）；P2-3（ARM64 章偏薄）→ 4 篇扩至 758–1055 行；P2-4（异步章概览化）→ 10 篇扩至 250–294 行；P2-5（搜索索引）→ 已关闭；P2-6（版本/编号/行号）→ 快照更新至 7.2、编号补齐 07–15、5 处行号修正。以下为原始记录。

### P1 — 建议处理

**1. SSD 专题（11.6）是全书唯一的内容洼地**
- 全部 13 个 stub 集中于此：`lightnvm代码分析`（rw.md 3 行、epilog.md 4 行）、`nand原理`（README 3 行）、`nvme原理`（intro/arch 各约 10 行）、`zns`（libzbd.md 仅 2 行正文）。
- `lightnvm` 子系统**已从当前 7.2 源码树移除**（`drivers/lightnvm` 不存在），该子书属历史资料，建议标注"已移除子系统，仅作背景阅读"。
- `zns` 只覆盖用户态工具（zenfs/libzbd），未覆盖内核 ZNS 块层支持；`nvme原理` 与已有的 NVMe 深度分析（11.2）深度差距大。
- 4 个嵌套子书的 SUMMARY.md 未挂入主 SUMMARY（其子页已直接挂入），成为孤儿文件；构建无影响，可清理。

**2. 833 条 `file://` 绝对路径链接（48 个文件）**
- 目标在本机全部存在（0 失效），但发布到 GitHub Pages 后读者点击无效。
- 重灾区：block_layer_analysis.md（148）、complete_data_flow_analysis.md（102）、block-01-core-framework.md（74）、concurrency_sync_analysis.md（65）、pcie.md（44）。
- 建议批量替换为 `https://github.com/hml1006/linux-comment/blob/v7.2-comment/<path>#L<n>`（与 book.toml 的 edit-url-template 同分支）。

### P2 — 可选改进

3. **ARM64 体系结构章偏薄**：4.RAS.md 与 9.ABI.md 各仅 27 行，为 ARM 规格摘译，未与 `arch/arm64` 内核实现（如 RAS 处理、syscall ABI 入口）关联；5.GIC.md/6.Debug.md 亦偏薄，而第 2 章已有 GIC 深度分析，可互链或合并。
4. **异步执行章（8）概览化**：15 种机制全覆盖，但 10/16 篇为 thin（约 130–150 行），深度低于同步章（7）；softirq/workqueue 等核心机制值得扩写。
5. **搜索索引过大**：构建警告 `search index is very large (23,655,726 bytes)`（约 23.7 MB），影响 Web 版加载；可考虑限制索引范围或拆分书籍。
6. **版本与编号小瑕疵**：
   - 00-overview/analysis_report.md 为 7.0.0 编译快照（文内已自行标注与 7.2 的差异，可接受，建议更新）；
   - 12.2 特殊文件系统编号跳过 07（06-sockfs → 08-cgroup），纯编号缺口，无内容缺失；
   - 抽查源码行号引用存在个别漂移：close.md 引用 `fs/open.c:1498`，当前树实际在 1524 行（注释增补导致偏移）；crypto 篇 `algapi.c:302` 则精确命中。建议行号视为近似定位。

---

## 6. 覆盖度结论（对照内核源码树）

已有专章深度覆盖：`arch/arm64`（含启动/异常/GIC/MMU/KVM）、`kernel/sched`、`mm`、`kernel/time`、`kernel/locking`+`kernel/rcu`、中断/软中断/工作队列、`kernel/irq`、`fs`（VFS+ext4/XFS/Btrfs/overlayfs/网络 fs/闪存 fs+15 特殊 fs）、`block`+`drivers/nvme`+SCSI/UFS/MMC+DM/MD+FUSE/DAX、`net`（协议栈+netfilter+XDP+无线）、`security`（LSM/MAC/IMA/EVM/crypto/keyring/加固）、`virt`+virtio/vhost/vDPA+VFIO/IOMMU、cgroup/namespace/seccomp、调试追踪 30 项、Kbuild/模块/livepatch/kexec、Rust for Linux、io_uring、BPF、audit。

设计上的全景式（非逐行）覆盖：DRM/KMS、ALSA/ASoC、USB、HID、thermal/regulator/hwmon/IIO/V4L2（10.4 设备类全景）、Intel 网卡之外的驱动。这属于合理的详略取舍，不构成缺口。

---

## 7. 方法与局限

- 脚本：SUMMARY 解析、文件比对、篇幅/代码块/标题统计、TODO 正则扫描（42 处命中全部人工复核）、链接分类校验；锚点校验以 `mdbook build` 产物中的真实 `id` 为准（991/991 通过）。
- 局限：`file://` 与源码引用仅验证目标文件存在性，未逐条核对行号与代码片段一致性（抽查 2 处：1 处准确、1 处行号漂移）；内容正确性（技术准确性）不在本次完整度审计范围内。

---

## 8. 整改记录（2026-11-25，按用户要求执行）

**要求**：删除 lightnvm/zns 内容；关闭 mdBook 搜索索引（不发布 GitHub Pages）；移除 GitHub 链接引用；嵌套子书改造并入当前 mdBook。

| # | 整改项 | 具体变更 |
|---|---|---|
| 1 | 删除 lightnvm/zns | 移除 `04-storage/ssd/lightnvm代码分析/`（6 页 + 6 图 + 子书 book.toml/SUMMARY）与 `04-storage/ssd/zns/`（2 页 + 1 图 + 子书配置）；SUMMARY 与 README 对应条目删除 |
| 2 | 嵌套子书并入主书 | `nand原理`、`nvme原理` 的 9 个页面与图片目录从 `src/` 上移一层（`git mv`，图片相对引用不变）；删除嵌套 book.toml/SUMMARY.md/.gitignore；SUMMARY 链接同步更新 |
| 3 | 关闭搜索索引 | `book.toml` `[output.html.search] enable = false`；构建不再生成约 23.7MB 的 searchindex.js，构建警告消失 |
| 4 | 移除 GitHub 链接 | 删除 `book.toml` 的 `git-repository-url` 与 `edit-url-template`；渲染产物中不再含仓库/编辑按钮链接 |
| 5 | 生成链路同步 | SUMMARY.md 由 README.md 经 `gen_summary.py` 生成：已同步更新 README 11.6 节（直链扁平化页面）、附录 A SSD 行、文档规模（526 篇/22.16 万行），并重写 `emit_ssd()`（SSD_BOOKS 常量）；重新生成 SUMMARY，diff 确认仅 SSD 节变化 |

**保留项**：833 条 `file://` 源码链接按用户本地阅读场景与 README 附录 C 约定保留（目标全部存在）；7 条外部链接均为规范/文档站（nvmexpress.org、developer.arm.com、man7.org、mdBook 文档），非 GitHub 仓库引用，保留。

**整改后验证**（全部通过）：
- `mdbook build` 成功，无警告；产物中无 searchindex.js、无 lightnvm/zns 页面、无 GitHub 链接。
- SUMMARY 引用 526 个文件：缺失 **0**，孤儿 **0**（整改前 4 个孤儿子书 SUMMARY 已消除）。
- 相对链接 993 条 + 锚点 991 条全部有效，死链 **0**；`file://` 目标 0 失效。
- 磁盘 Markdown 527 个（526 章节 + 主 SUMMARY.md），全部有 H1。

**当时遗留**：P2 项 3/4/6 未处理——已在第二轮整改（第 9 节）全部完成。

---

## 9. 第二轮整改：内容深度与 P2 项（2026-11-25）

**要求**：第 3 节分章节完整度明细中，所有章节的内容深度需达到「优」；P2 可选改进项需完成。

### 9.1 P2 项处理

| 项 | 处理方式 | 证据 |
|---|---|---|
| P2-3 ARM64 章偏薄 | 4 篇扩写：RAS 27→758 行、ABI 27→1004、GIC 106→989、Debug 98→1055；全部转为源码实现分析（ESB/SCTLR_EL1.IESB、pt_regs 契约、gic_init_bases、DBGBVR/MDSCR_EL1 等），并补充与第 2 章中断、第 17 章调试的互链 | 章节 0/0/7/5，评级优 |
| P2-4 异步执行概览化 | 10 篇由 125–181 行扩至 250–294 行，补充结构体定义、初始化入口、执行链、RT 形态与机制对比 | 章节 0/0/16/0，评级优 |
| P2-5 搜索索引过大 | `book.toml` 关闭 `search.enable` | 构建无 searchindex.js、无警告 |
| P2-6a 7.0.0 快照 | 按当前树重写 `analysis_report.md`：7.2.0、10,061 条编译记录、1,110 个 `=y`、GCC 15.2.0，全部表格重算，并新增“复现与校验”一节给出可重跑命令 | 章节 0/0/1/0（V=1582），评级优 |
| P2-6b 编号缺口 | 特殊文件系统 08–16 → 07–15 连续编号（`git mv` 9 个文件），README/SUMMARY 同步 | 目录 01–15 连续 |
| P2-6c 行号漂移 | 审计 776 处 `path:line` 引用；修正 5 处经核实的定义漂移（close 1498→1524、clone3 2934→3029、key_alloc 192→224、`__schedule` 6768→7068、kfree_rcu 2718→2821）；其余命中为调用点引用（审计误报）未改动；README 附录 C 补充“行号视为近似定位”约定 | 逐条 grep 核实 |

### 9.2 深度整改（7 个原评级非「优」章节 + 收尾补厚）

| 章节 | 整改前（行 / 汉字 / 评级） | 整改后（行 / 汉字 / 评级） | 主要改动 |
|---|---|---|---|
| 0 总览 | 450 / 1,790 / — | 459 / 1,310 / 优 | 快照重写至 7.2，新增复现与校验 |
| 1 ARM64 | 3,536 / 9,278 / 良 | 7,080 / 20,159 / 优 | 4 篇 deep |
| 3 系统调用 | 94,112 / 150,469 / 优 | 94,901 / 155,863 / 优 | 5 篇单调用页补至 ≥600 |
| 7 并发与同步 | 6,774 / 9,543 / 优 | 7,791 / 15,739 / 优 | 4 篇补至 ≥1200 |
| 8 异步执行 | 2,762 / 5,813 / 中 | 3,908 / 11,887 / 优 | 10 篇全部 ≥250 行 |
| 11 存储栈 | 28,600 / 52,558 / 优（SSD 弱） | 30,609 / 69,392 / 优 | SSD 5 篇 medium+，导读扩写；lightnvm/zns 移除 |
| 13 网络 | 7,316 / 9,555 / 优 | 7,631 / 10,779 / 优 | 无线/蓝牙篇 249→567 行 |
| 14 安全 | 1,847 / 8,695 / 良 | 3,527 / 20,123 / 优 | 5 篇均 deep（650–806 行） |
| 15 虚拟化 | 1,503 / 3,647 / 良 | 3,999 / 18,703 / 优 | 3 篇 deep（1,082–1,698 行） |
| 16 容器 | 947 / 4,020 / 良 | 2,107 / 9,656 / 优 | 2 篇 deep（1,008/1,099 行） |
| 17 调试 | 12,091 / 18,241 / 优 | 12,624 / 20,267 / 优 | trace/panic 两篇补至 ≥1300 |
| 18 构建/Rust | 1,390 / 5,564 / 良 | 4,227 / 18,857 / 优 | 4 篇 deep（966–1,242 行） |
| 合计 | 222,495 行 | **239,151 行** | 净增 16,656 行 |

所有扩写均为**追加式**：`git diff --numstat` 对 60 余个目标文件的删除行数合计为 0，原有分析未被改写。

### 9.3 扩写中依据源码订正的事实（抽样）

- 本树 `struct semaphore` 使用 `first_waiter` 单链表（无 `wait_list`），`up()` 走 wake_q；`struct percpu_rw_semaphore` 已无 `rw_sem` 成员（改用 `rcu_sync` + `read_count` + `rcuwait`）。
- 本树不存在 `DEFINE_LOCAL_LOCK()`；PREEMPT_RT 下 `local_lock_irq*` 并不关中断。
- ARC 的 `arc_gettls/arc_settls/arc_usr_cmpxchg` 编号在 `scripts/syscall.tbl` 中带 `arc` ABI 标签，arm64 生成表时被过滤，属**未分配编号**（落到 `sys_ni_syscall`），并非有 stub 实现。
- `personality()` 的 arm64 包装在不支持 32 位 EL0 时返回 `-EINVAL`；`UNAME26` 实际输出 `2.6.<PATCHLEVEL+60>`。
- `panic=` 在本树是 `core_param` 而非 `__setup`；`blkdev_issue_discard()` 会吞掉 `-EOPNOTSUPP` 返回 0。
- kexec：`CONFIG_KEXEC` 未设置（`CONFIG_PM=n` 链路），`kexec_load` 不可用；arm64 无 ELF 加载器（走 `arch/arm64/kernel/kexec_image.c`），也未定义 purgatory。
- Rust：本树不存在 `rust/core.rs` 与 `samples/rust/rust_chrdev.rs`；`RUST_IS_AVAILABLE` 未出现在 `.config`，因缺 `bindgen` 而不可选。
- 本树 `arch/arm64/kernel/entry.S` 已无显式 `esb` 指令，内核异常入口改用 `SCTLR_EL1.IESB` 隐式屏障，`esb` 宏仅剩 KVM hyp 向量使用。

---

## 10. 整改后最终核算（权威结论）

### 10.1 结构与构建

| 检查项 | 结果 |
|---|---|
| `mdbook build` | 成功，无警告（搜索索引已关闭，无超大索引告警） |
| SUMMARY 引用文件 | 528 个，缺失 **0** |
| 孤儿 Markdown | **0**（530 个文件 = 528 章节 + 主 SUMMARY.md + 简介页 kernel-map.md） |
| 相对链接 | 1,204 条全部有效，死链 **0** |
| 锚点链接 | 991 条全部命中渲染产物 heading id，失效 **0** |
| `file://` 源码链接 | 833 条目标全部存在（本地阅读场景保留） |
| H1 规范 | 530 个文件均有且仅有一个 H1 |
| 系统调用覆盖 | arm64 适用 327/327 = **100%** |

### 10.2 深度核算

- 全库 528 页：**stub 0 / thin 110 / medium 363 / deep 55**。
- 110 篇 thin 全部落在豁免类：系统调用单页（≥600）、速查页 `net-syscall.md`、总结页 `mm/06-summary.md`。
- **19 个章节评级全部为「优」**（0 个 stub、0 个低于 medium 的实质页），明细见第 3 节。
- 深度提升集中体现为：deep 篇数 36 → **55**，章节总行数 222,495 → **241,147**。

### 10.3 复核方式

```bash
cd notes && mdbook build                        # 构建 + 无警告，产物在 notes/book/
python3 <核算脚本>                              # 按第 3 节口径重算 stub/thin/medium/deep
python3 notes/00-overview/tools/gen_summary.py  # 目录与 README 一致性
```

核算口径与豁免规则已在第 3 节显式给出，任何一节若新增低于该口径的实质页，评级会自动回落，便于后续维护时自查。

---

## 11. 第三轮整改：mdBook 收敛到 `notes/`（2026-11-25）

**要求**：整个 mdBook 放入 `notes/`，避免污染 Linux 内核源码树根目录。

| 项 | 变更前 | 变更后 |
|---|---|---|
| 配置文件 | `<repo>/book.toml` | `notes/book.toml` |
| 源目录 | `src = "notes"` | `src = "."`（book.toml 所在目录） |
| 构建产物 | `<repo>/book/` | `notes/book/` |
| 审计报告 | `<repo>/mdbook-completeness-report.md` | `notes/00-overview/mdbook-completeness-report.md`，并登记为第 0 章页面 |
| `.gitignore` | `/book/` | `/notes/book/` |
| 构建命令 | `mdbook build`（仓库根） | `cd notes && mdbook build` |

**关键点**：

- `src = "."` 与 `build-dir = "book"` 同层时，mdBook 会**自动排除构建目录**，不会出现 `book/book/` 递归复制（已用最小样例连跑两次构建验证）。
- 根目录 `README.md` 仍是 GitHub 入口与 `gen_summary.py` 的输入，位于 `notes/` 之外，因此不会进入书籍源目录。
- 收敛后，内核源码树根目录不再出现任何 mdBook 相关文件；仅保留 `README.md`、`.gitignore` 与内核源码自身文件。

**复验结果**（收敛后重新执行）：`cd notes && mdbook build` 成功无警告；SUMMARY 引用 526 个文件、缺失 0、孤儿 0；相对链接 1,184 条 0 死链；锚点 991 条 0 失效；19 章评级仍全部为「优」。

---

## 12. 第四轮补充：kernfs 专章（2026-11-25）

**背景**：审计发现 kernfs 只作为"sysfs 的底层框架"在第 12 章 sysfs 篇中被概述（约 35 处提及、1 个结构体、4 条调用栈），没有独立的框架教程，`fs/kernfs/` 的内部拆分、完整 API、`kernfs_ops` 回调族与 VFS 对接均缺失。

**新增**：`notes/05-filesystems/kernfs/01-kernfs.md`（846 行，加权体量 7,369，deep），共 11 节：

1. 定位与设计（问题域、三个使用者、分层）
2. 核心数据结构（`kernfs_node` 与三种 elem、`kernfs_root`、`kernfs_super_info`、`kernfs_iattrs`、`kernfs_open_file`、count/active 双计数）
3. 节点生命周期（`kernfs_new_node` → `kernfs_add_one` → `kernfs_link_sibling` → `kernfs_activate`；remove/drain/put；rename）
4. `kernfs_ops` 与文件 I/O（open、seq/裸读写、mmap、poll/notify、release/drain）
5. VFS 对接（`kernfs_get_tree`/`fill_super`/`kill_sb`、super_operations、exportfs、inode 与 lookup/revalidate）
6. 目录与查找（哈希/rb 排序、`kernfs_find_and_get_ns`、readdir 游标、symlink 路径）
7. 并发与锁（`kernfs_rwsem`/`kernfs_iattr_rwsem`/`kernfs_supers_rwsem`/`kernfs_rename_lock`、`node_mutex[]`、RCU、active 保护）
8. 使用方分析（sysfs、cgroup、resctrl）
9. 配置与调试
10. 关键文件清单
11. 延伸阅读（链回 sysfs、cgroup、VFS、getdents64 等篇）

**依据源码订正的事实**：本树 `fs/kernfs/` 无 `sysfs.c` 与 `kernfs_sysfs_*` 符号，`/sys/fs/kernfs` 不存在，超级块登记由 `kernfs_get_tree()`/`kernfs_kill_sb()` 承担；锁不是单一 `kernfs_mutex`，而是上述多把 rwsem 加全局哈希 mutex；`kernfs_drain()` 带 `drop_supers` 参数；`fs/resctrl/rdtgroup.c` 是 sysfs/cgroup 之外的第三个 `kernfs_create_root()` 使用者。

**目录同步**：README 第 12 章新增 `12.2 kernfs 框架`，原「特殊文件系统」「真实文件系统」顺延为 12.3/12.4；目录结构树与附录 A 状态矩阵同步更新；`gen_summary.py` 重新生成 SUMMARY，新增条目位于 12.2。

**维护提示（本轮发现）**：`book-dir` 位于 `src` 内时，构建偶发出现残缺产物（本轮两次遇到 322/467 个 HTML，而预期为 532）。该现象不可稳定复现（随后连续三次全量构建均正常），因此**校验前应核对 `find notes/book -name '*.html' | wc -l` 是否为 532**，若偏少则重新构建；否则锚点检查会出现大量假失效。

**复验结果**：清空产物后全量构建成功；SUMMARY 引用 527 个文件、缺失 0、孤儿 0；相对链接 1,192 条 0 死链；锚点 991 条 0 失效；第 12 章 21 页 0/0/19/2，19 章评级仍全部为「优」。

---

## 13. 第五轮补充：kobject 专章（2026-11-25）

**背景**：kobject 内容散落在 22 个文件、共 150 处提及，其中 sysfs 篇 67 处（偏"如何用 sysfs"）、驱动模型篇 15 处，没有从 kobject 自身语义出发的专篇。

**新增**：`notes/03-drivers/driver-model/00-kobject.md`（833 行，加权体量 7,186，deep），12 节：

1. 定位与设计（与 kset/kobj_type/sysfs/uevent 的关系）
2. 核心数据结构（`struct kobject` 全字段、`kref`、`kset`、`kobj_type`、`kobj_attribute`/`sysfs_ops`、`kobj_uevent_env`/`kset_uevent_ops`、`kobj_ns_type_operations`）
3. 引用计数与生命周期（get/put/get_unless_zero、release 与 cleanup 的区别、延迟清理、无 release 约束、状态机）
4. 创建与注册（两段式 API、`kobject_add_varg`、`kobject_add_internal`、错误回滚不变量）
5. 命名与路径（`kobject_set_name_vargs`、`kobject_get_path`、rename/move 约束）
6. sysfs/kernfs 映射（`sd`、属性文件反查链、四类操作对照、与 kernfs 专章的分界）
7. uevent（action 表、`kobject_uevent_env` 全流程、netlink 广播、helper、synth、suppress）
8. 命名空间（`kobject_namespace`、ns 类型操作、netns 与 uevent 定向）
9. 在 device 模型中的嵌入（`device_initialize/add/del` 与 kobject 生命周期对应、class/bus 的 kset 角色）
10. 常见错误与调试
11. 关键文件清单
12. 延伸阅读

**根据源码订正的事实**：本树无 `lib/kref.c`（kref 全为 `include/linux/kref.h` 内联）；无 `kref_replace_release()`，只有 `kref_put_mutex`/`kref_put_lock`；`kobject_create`/`kobject_add_varg`/`kobject_add_internal`/`kobject_cleanup`/`kobject_release` 等为 static；不存在 `kobject_synthetic_uevent()`，只有 `kobject_synth_uevent()`；`CONFIG_UEVENT_HELPER`、`CONFIG_DEBUG_KOBJECT` 未启用。

**顺带修正**：`notes/05-filesystems/special/02-sysfs.md` §2.3 原 `struct kobj_type` 摘录含本树不存在的 `child_sysfs_direntry`、且 `namespace` 类型写成 `const void *`；已按 `include/linux/kobject.h` 实际定义更正为 `const struct ns_common *(*namespace)` 与 `get_ownership`，并加注历史差异与回链。

**目录同步**：README 第 10.1 节首位新增该篇，附录 A「驱动模型/总线」行补充 kobject 说明；`gen_summary.py` 重新生成 SUMMARY。

**复验结果**：全量构建 533 个 HTML；SUMMARY 引用 528 个文件、缺失 0、孤儿 0；相对链接 1,204 条 0 死链；锚点 991 条 0 失效；第 10 章 8 页 0/0/5/3，19 章评级仍全部为「优」。
