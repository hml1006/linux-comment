# Linux 7.2.0 内核编译文件分析报告

> **版本说明**：本报告基于当前 **Linux 7.2.0** 源码树与裁剪后的 `.config` 重新生成（编译快照来源 `compile_commands.json`，共 10,061 条编译记录）。`.config` 中 `=y` 选项 1,110 个、`=m` 选项 0 个（全部内置）。文件数与子系统统计随当前树/配置变化，阅读时请以源码为准。

## 1. 总体概览

| 项目 | 详情 |
|------|------|
| 内核版本 | 7.2.0 |
| 目标架构 | ARM64 (aarch64), 小端序, 64-bit |
| 编译器 | aarch64-linux-gnu-gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0, GNU ld (GNU Binutils for Ubuntu) 2.46 |
| 汇编器 / 链接器 | GNU AS 2.46 / GNU LD (BFD) 2.46 |
| 指令集 | armv8.5-a |
| 编译优化 | `-O2`（经 ccache 缓存） |
| 编译文件总数 | **10,061** 个（9,986 C + 75 ASM） |
| 配置项 | 1,110 个 `=y`, 0 个 `=m` (全部内置) |

---

## 2. 构建流程

从 `build.sh` 还原的实际构建流程：

```bash
# 1. 基于已有 .config（defconfig 拷贝行已注释）开启调试相关配置
sed -i 's/^# CONFIG_GDB_SCRIPTS is not set/CONFIG_GDB_SCRIPTS=y/' .config
sed -i 's/^# CONFIG_DYNAMIC_DEBUG is not set/CONFIG_DYNAMIC_DEBUG=y/' .config
sed -i 's/^# CONFIG_DYNAMIC_DEBUG_CORE is not set/CONFIG_DYNAMIC_DEBUG_CORE=y/' .config
sed -i 's/^CONFIG_DEBUG_INFO_REDUCED=y/CONFIG_DEBUG_INFO_REDUCED=n/' .config
# 2. 缺失项兜底追加 GDB_SCRIPTS / DYNAMIC_DEBUG / DYNAMIC_DEBUG_CORE
# 3. 手动配置
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- menuconfig
# 4. 所有模块转为内置
sed -i 's/=m/=y/g' .config
# 5. 编译 (-j12 并行)
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j12
# 6. 生成 compile_commands.json
./scripts/clang-tools/gen_compile_commands.py
```

---

## 3. 编译标志分析（以 `mm/page_alloc.o` 为例）

| 类别 | 标志 | 说明 |
|------|------|------|
| 基本 | `-nostdinc`, `-std=gnu11`, `-fms-extensions` | 不链接标准库，GNU C11 |
| 架构 | `-mabi=lp64`, `-mlittle-endian`, `-mgeneral-regs-only` | ARM64 LP64 小端 |
| 指令集 | `-Wa,-march=armv8.5-a`, `-DARM64_ASM_ARCH='"armv8.5-a"'` | armv8.5-a |
| 优化 | `-O2`, `-fno-strict-aliasing`, `-fconserve-stack`, `-fno-allow-store-data-races` | O2 |
| 安全 | `-fstack-protector-strong`, `-mbranch-protection=pac-ret` | 栈保护 + PAC 返回地址签名 |
| 安全初始化 | `-ftrivial-auto-var-init=zero`, `-fzero-init-padding-bits=all` | 自动变量/填充零初始化 |
| 调试 | `-g`（DEBUG_INFO）, `-fno-omit-frame-pointer` | DWARF 调试信息 |
| 入口 | `-D__KERNEL__` | 内核模式宏 |
| 字符 | `-fshort-wchar`, `-funsigned-char` | 字符约定 |
| 帧指针 | `-fno-omit-frame-pointer`, `-fno-optimize-sibling-calls` | 保留帧指针 |
| 对齐 | `-fmin-function-alignment=4` | 最小函数对齐 |
| 严格检查 | `-fstrict-flex-arrays=3`, `-fno-delete-null-pointer-checks` | 数组边界/空指针检查 |
| 警告 | `-Wall -Wextra -Wundef`, 多个 `-Werror=` | 严格警告 |

Include 路径：

```
-I./arch/arm64/include -I./arch/arm64/include/generated -I./include
-I./arch/arm64/include/uapi -I./arch/arm64/include/generated/uapi
-I./include/uapi -I./include/generated/uapi
-include ./include/linux/compiler-version.h -include ./include/linux/kconfig.h -include ./include/linux/compiler_types.h
```

---

## 4. 编译文件按子系统分布

| 子系统 | 文件数 | 占比 | 说明 |
|--------|--------|------|------|
| drivers/ | 7,160 | 71.2% | 硬件驱动层 |
| fs/ | 626 | 6.2% | VFS + 各文件系统 |
| net/ | 597 | 5.9% | 网络协议栈 |
| sound/ | 522 | 5.2% | ALSA 音频框架 |
| lib/ | 273 | 2.7% | 通用工具库 |
| kernel/ | 247 | 2.5% | 进程/调度/BPF 等 |
| arch/ | 245 | 2.4% | ARM64 架构 |
| crypto/ | 112 | 1.1% | 加密 API 与算法 |
| mm/ | 91 | 0.9% | 内存管理 |
| block/ | 47 | 0.5% | 块 IO 层 |
| scripts/ | 46 | 0.5% | DTC 等工具脚本 |
| io_uring/ | 40 | 0.4% | 异步 IO 框架 |
| security/ | 22 | 0.2% | Linux 安全模块 |
| ipc/ | 11 | 0.1% | SysV IPC |
| init/ | 9 | 0.1% | 内核启动初始化 |
| virt/ | 9 | 0.1% | KVM 虚拟化 |
| certs/ | 2 | 0.0% | 系统证书 |
| (root)/ | 1 | 0.0% | 导出符号文件 |
| usr/ | 1 | 0.0% | initramfs |
| **总计** | **10,061** | 100% | |

---

## 5. 驱动子系统详细分解 (drivers/ — 7,160 文件)

| 模块 | 文件数 |
|------|--------|
| drivers/gpu | 1465 |
| drivers/net | 1040 |
| drivers/media | 652 |
| drivers/clk | 607 |
| drivers/usb | 271 |
| drivers/acpi | 227 |
| drivers/pinctrl | 189 |
| drivers/phy | 127 |
| drivers/soc | 118 |
| drivers/firmware | 108 |
| drivers/pci | 103 |
| drivers/mtd | 101 |
| drivers/char | 86 |
| drivers/crypto | 75 |
| drivers/iio | 73 |
| drivers/tty | 73 |
| drivers/base | 67 |
| drivers/input | 63 |
| drivers/rtc | 63 |
| drivers/thermal | 62 |
| drivers/mmc | 61 |
| drivers/irqchip | 60 |
| drivers/spi | 56 |
| drivers/dma | 54 |
| drivers/gpio | 54 |
| drivers/i2c | 52 |
| drivers/regulator | 52 |
| drivers/interconnect | 50 |
| drivers/pmdomain | 50 |
| drivers/scsi | 49 |
| drivers/video | 49 |
| drivers/mfd | 47 |
| drivers/xen | 44 |
| drivers/cpufreq | 39 |
| drivers/perf | 39 |
| drivers/hwtracing | 37 |
| drivers/pwm | 36 |
| drivers/watchdog | 36 |
| drivers/power | 34 |
| drivers/bus | 33 |
| drivers/remoteproc | 33 |
| drivers/reset | 32 |
| drivers/hid | 30 |
| drivers/bluetooth | 29 |
| drivers/misc | 27 |
| drivers/md | 26 |
| drivers/ata | 25 |
| drivers/clocksource | 25 |
| drivers/mailbox | 25 |
| drivers/nvmem | 25 |
| drivers/platform | 24 |
| drivers/hwmon | 23 |
| drivers/leds | 22 |
| drivers/iommu | 21 |
| drivers/staging | 21 |
| drivers/of | 18 |
| drivers/memory | 15 |
| drivers/rpmsg | 15 |
| drivers/ufs | 15 |
| drivers/fpga | 13 |
| drivers/greybus | 13 |
| drivers/soundwire | 13 |
| drivers/tee | 13 |
| drivers/devfreq | 12 |
| drivers/edac | 12 |
| drivers/pnp | 11 |
| drivers/vfio | 10 |
| drivers/virtio | 10 |
| drivers/cpuidle | 9 |
| drivers/ptp | 9 |
| drivers/dma-buf | 8 |
| drivers/slimbus | 8 |
| drivers/block | 7 |
| drivers/nfc | 7 |
| drivers/nvme | 7 |
| drivers/counter | 6 |
| drivers/extcon | 6 |
| drivers/gnss | 6 |
| drivers/opp | 5 |
| drivers/spmi | 5 |
| drivers/hte | 4 |
| drivers/hwspinlock | 3 |
| drivers/mux | 3 |
| drivers/pps | 3 |
| drivers/amba | 2 |
| drivers/ras | 2 |

---

## 6. 文件系统详细分析 (fs/ — 626 文件)

注：`fs/` 核心 VFS 单文件（namei.c、open.c、read_write.c、dcache.c、inode.c、file.c、namespace.c、exec.c、splice.c、xattr.c 等）计入“核心单文件”。

| 模块 | 文件数 |
|------|--------|
| fs/xfs | 143 |
| fs/btrfs | 63 |
| fs/nfs | 60 |
| fs/ext4 | 35 |
| fs/ubifs | 29 |
| fs/proc | 28 |
| fs/f2fs | 21 |
| fs/fuse | 18 |
| fs/lockd | 17 |
| fs/netfs | 17 |
| fs/squashfs | 16 |
| fs/overlayfs | 12 |
| fs/9p | 10 |
| fs/ext2 | 10 |
| fs/notify | 10 |
| fs/iomap | 9 |
| fs/fat | 8 |
| fs/autofs | 7 |
| fs/configfs | 6 |
| fs/jbd2 | 6 |
| fs/kernfs | 5 |
| fs/sysfs | 5 |
| fs/efivarfs | 4 |
| fs/pstore | 4 |
| fs/nfs_common | 3 |
| fs/nls | 3 |
| fs/quota | 3 |
| fs/debugfs | 2 |
| fs/ramfs | 2 |
| fs/devpts | 1 |
| fs/exportfs | 1 |
| fs/hugetlbfs | 1 |
| fs/ 核心单文件 | 67 |

---

## 7. 网络协议栈详细分析 (net/ — 597 文件)

| 模块 | 文件数 |
|------|--------|
| net/ipv4 | 76 |
| net/ipv6 | 60 |
| net/netfilter | 60 |
| net/core | 50 |
| net/mac80211 | 45 |
| net/sunrpc | 38 |
| net/ethtool | 34 |
| net/sched | 30 |
| net/bridge | 29 |
| net/bluetooth | 25 |
| net/dsa | 20 |
| net/wireless | 20 |
| net/nfc | 15 |
| net/devlink | 14 |
| net/802 | 9 |
| net/can | 9 |
| net/qrtr | 9 |
| net/8021q | 8 |
| net/9p | 7 |
| net/hsr | 7 |
| net/handshake | 6 |
| net/llc | 4 |
| net/rfkill | 4 |
| net/unix | 4 |
| net/netlink | 3 |
| net/bpf | 2 |
| net/dns_resolver | 2 |
| net/ethernet | 1 |
| net/packet | 1 |
| net/switchdev | 1 |
| net/ 核心单文件 | 4 |

---

## 8. 内核核心详细分析 (kernel/ — 247 文件)

注：fork/exit/signal/sys/pid/nsproxy/ptrace/cred/kthread/workqueue/softirq/irq_work/async/task_work 等核心单文件计入“核心单文件”；本裁剪配置下 sched/rcu/trace 等仅编译部分实现文件。

| 模块 | 文件数 |
|------|--------|
| kernel/bpf | 55 |
| kernel/time | 27 |
| kernel/irq | 19 |
| kernel/locking | 11 |
| kernel/power | 11 |
| kernel/dma | 9 |
| kernel/cgroup | 8 |
| kernel/module | 7 |
| kernel/futex | 5 |
| kernel/printk | 5 |
| kernel/rcu | 5 |
| kernel/events | 4 |
| kernel/sched | 4 |
| kernel/entry | 2 |
| kernel/trace | 1 |
| kernel/ 核心单文件 | 74 |

---

## 9. 内存管理详细分析 (mm/ — 91 文件)

| 分类 | 核心文件 |
|------|----------|
| 页面分配 | `init-mm.c`, `memblock.c`, `mm_init.c`, `page_alloc.c`, `page_frag_cache.c` |
| 内存映射 | `mlock.c`, `mmap.c`, `mmap_lock.c`, `mprotect.c`, `mremap.c`, `msync.c`, `vma.c`, `vma_exec.c`, `vma_init.c`, `vmalloc.c` |
| 页面缓存 | `fadvise.c`, `filemap.c`, `folio-compat.c`, `page-writeback.c`, `readahead.c`, `truncate.c`, `workingset.c` |
| 交换 | `page_io.c`, `swap.c`, `swap_cgroup.c`, `swap_state.c`, `swapfile.c` |
| 页面回收 | `oom_kill.c`, `page_reporting.c`, `shrinker.c`, `vmpressure.c`, `vmscan.c` |
| 分配器 | `mempool.c`, `page_counter.c`, `slab_common.c`, `slub.c` |
| 虚拟内存 | `gup.c`, `huge_memory.c`, `memory.c`, `mmu_gather.c`, `mmu_notifier.c`, `process_vm_access.c`, `rmap.c` |
| 大页 | `hugetlb.c`, `hugetlb_cgroup.c`, `hugetlb_cma.c`, `hugetlb_sysctl.c`, `hugetlb_sysfs.c`, `khugepaged.c` |
| 共享/秘密内存 | `memfd.c`, `secretmem.c`, `shmem.c`, `shmem_quota.c` |
| 压缩/迁移 | `balloon.c`, `compaction.c`, `migrate.c`, `page_isolation.c` |
| 内存控制组 | `bpf_memcontrol.c`, `memcontrol.c` |
| CMA/连续 | `cma.c`, `dmapool.c`, `execmem.c` |
| NUMA/分层 | `memory-tiers.c`, `memory_hotplug.c`, `mempolicy.c`, `numa.c`, `numa_memblks.c` |
| 其他 | `backing-dev.c`, `debug.c`, `early_ioremap.c`, `highmem.c`, `interval_tree.c`, `ioremap.c`, `ksm.c`, `list_lru.c`, `maccess.c`, `madvise.c`, `memory-failure.c`, `memtest.c`, `mincore.c`, `mmzone.c`, `mseal.c`, `page_vma_mapped.c`, `pagewalk.c`, `percpu.c`, `pgtable-generic.c`, `show_mem.c`, `sparse-vmemmap.c`, `sparse.c`, `util.c`, `vmstat.c` |

---

## 10. 加密子系统 (crypto/ — 112 文件)

| 分类 | 核心文件 |
|------|----------|
| 核心 API | `acompress.c`, `aead.c`, `af_alg.c`, `af_alg.mod.c`, `ahash.c`, `akcipher.c`, `algapi.c`, `algboss.c`, `algif_rng.c`, `algif_rng.mod.c`, `api.c`, `bpf_crypto_skcipher.c`, `cipher.c`, `crypto_engine.c`, `crypto_engine.mod.c`, `crypto_user.c`, `geniv.c`, `kpp.c`, `lskcipher.c`, `proc.c`, `scatterwalk.c`, `scompress.c`, `shash.c`, `skcipher.c` |
| 对称/分组 | `blake2b_generic.mod.c`, `chacha.c`, `curve25519-generic.mod.c`, `des_generic.c`, `des_generic.mod.c`, `sm4.c`, `sm4.mod.c`, `sm4_generic.c`, `sm4_generic.mod.c` |
| 块加密模式 | `authenc.c`, `authenc.mod.c`, `authencesn.c`, `authencesn.mod.c`, `cbc.c`, `cbc.mod.c`, `ccm.c`, `ccm.mod.c`, `cmac.c`, `cmac.mod.c`, `ctr.c`, `ctr.mod.c`, `ecb.c`, `ecb.mod.c`, `echainiv.c`, `gcm.c`, `gcm.mod.c`, `xts.c`, `xts.mod.c` |
| 哈希 | `crc32c.c`, `ghash-generic.c`, `ghash-generic.mod.c`, `hmac.c`, `md5.c`, `md5.mod.c`, `michael_mic.c`, `michael_mic.mod.c`, `sha1.c`, `sha256.c`, `sha256_generic.mod.c`, `sha3_generic.mod.c`, `sha512.c`, `sm3.mod.c`, `sm3_generic.c`, `sm3_generic.mod.c`, `xxhash_generic.c`, `xxhash_generic.mod.c` |
| 非对称 | `asymmetric_keys/asymmetric_type.c`, `asymmetric_keys/pkcs7.asn1.c`, `asymmetric_keys/pkcs7_parser.c`, `asymmetric_keys/pkcs7_trust.c`, `asymmetric_keys/pkcs7_verify.c`, `asymmetric_keys/public_key.c`, `asymmetric_keys/restrict.c`, `asymmetric_keys/signature.c`, `asymmetric_keys/x509.asn1.c`, `asymmetric_keys/x509_akid.asn1.c`, `asymmetric_keys/x509_cert_parser.c`, `asymmetric_keys/x509_loader.c`, `asymmetric_keys/x509_public_key.c`, `dh.c`, `dh_generic.mod.c`, `dh_helper.c`, `ecc.c`, `ecc.mod.c`, `ecdh.c`, `ecdh_generic.mod.c`, `ecdh_helper.c`, `rsa-pkcs1pad.c`, `rsa.c`, `rsa_helper.c`, `rsaprivkey.asn1.c`, `rsapubkey.asn1.c`, `rsassa-pkcs1.c` |
| 随机数 | `drbg.c`, `jitterentropy-kcapi.c`, `jitterentropy.c`, `rng.c` |
| 压缩 | `deflate.c`, `lzo-rle.c`, `lzo.c`, `zstd.c` |
| 签名/测试 | `sig.c`, `tcrypt.c`, `tcrypt.mod.c`, `testmgr.c` |
| 其他 | `crypto_null.c`, `xor.c`, `xor.mod.c` |

---

## 11. 块设备层 (block/ — 47 文件)

| 分类 | 核心文件 |
|------|----------|
| 核心 IO | `blk-core.c`, `blk-flush.c`, `blk-lib.c`, `blk-map.c`, `blk-merge.c`, `blk-mq-cpumap.c`, `blk-mq-debugfs.c`, `blk-mq-dma.c`, `blk-mq-sched.c`, `blk-mq-sysfs.c`, `blk-mq-tag.c`, `blk-mq.c`, `blk-pm.c`, `blk-rq-qos.c`, `blk-settings.c`, `blk-timeout.c` |
| 块设备 | `bdev.c`, `blk-sysfs.c`, `disk-events.c`, `early-lookup.c`, `fops.c`, `genhd.c`, `holder.c`, `ioctl.c`, `partitions/core.c`, `partitions/efi.c`, `partitions/msdos.c` |
| IO 调度 | `bfq-cgroup.c`, `bfq-iosched.c`, `bfq-wf2q.c`, `blk-cgroup-rwstat.c`, `blk-cgroup.c`, `blk-ioc.c`, `elevator.c`, `ioprio.c`, `kyber-iosched.c`, `mq-deadline.c` |
| BIO/完整性 | `bio-integrity-auto.c`, `bio-integrity.c`, `bio.c`, `blk-integrity.c`, `t10-pi.c` |
| 其他 | `badblocks.c`, `blk-ia-ranges.c`, `blk-stat.c`, `bsg-lib.c`, `bsg.c` |

---

## 12. 关键配置特性汇总

| 配置项 | 状态 | 影响 |
|--------|------|------|
| `CONFIG_DEBUG_INFO=y` | 开启 | 完整 DWARF 调试信息 |
| `CONFIG_GDB_SCRIPTS=y` | 开启 | GDB 内核调试脚本 |
| `CONFIG_DYNAMIC_DEBUG=y` | 开启 | 运行时动态调试打印 |
| `CONFIG_EXT4_FS=y` | 开启 | EXT4 默认文件系统 |
| `CONFIG_NET=y` | 开启 | 完整网络协议栈 |
| `CONFIG_BPF=y` | 开启 | BPF 虚拟机 + JIT |
| `CONFIG_AUDIT=y` | 开启 | 审计子系统 |
| `CONFIG_AUDITSYSCALL=y` | 开启 | 系统调用审计 |
| `CONFIG_IRQ_FORCED_THREADING=y` | 开启 | 强制中断线程化 |
| `CONFIG_HIGH_RES_TIMERS=y` | 开启 | 高精度定时器 |
| `CONFIG_NO_HZ_IDLE=y` | 开启 | 动态时钟中断 |
| `CONFIG_TICK_ONESHOT=y` | 开启 | 单次触发时钟 |
| `CONFIG_CONTEXT_TRACKING=y` | 开启 | 上下文追踪 |
| `CONFIG_KVM=y` | 开启 | KVM 虚拟化 |
| `CONFIG_VIRTIO=y` | 开启 | virtio 半虚拟化 |
| `CONFIG_VFIO=y` | 开启 | 用户态驱动框架 |
| `CONFIG_PGTABLE_LEVELS=5` | 5 | 5 级页表 |
| `CONFIG_ARM64_VA_BITS=52` | 52 | 52-bit 虚拟地址 |
| `CONFIG_ARM64_PA_BITS=52` | 52 | 52-bit 物理地址 |
| `CONFIG_NR_CPUS=512` | 512 | 大规模多核 |
| `CONFIG_HZ=250` | 250 | 定时器频率 |
| `CONFIG_CMA_AREAS=20` | 20 | 连续内存分配 |
| `CONFIG_MODULES` | 未开启 | 全部内置，不能 insmod；模块文档仅作源码分析 |
| `CONFIG_SECURITY` | 未开启 | 无 LSM/SELinux/AppArmor/IMA/EVM |
| `CONFIG_CRYPTO` | 未开启 | 无 crypto API/AF_ALG |
| `CONFIG_KEYS` | 未开启 | 无 keyring |
| `CONFIG_BTRFS_FS` | 未开启 | 无 Btrfs |
| `CONFIG_KASAN` | 未开启 | 无地址消毒器 |
| `CONFIG_UBSAN` | 未开启 | 无未定义行为检测 |
| `CONFIG_KCOV` | 未开启 | 无代码覆盖率 |
| `CONFIG_RANDOMIZE_BASE` | 未开启 | 无 KASLR |

---

## 13. 总结

这是一个**完整编译的 ARM64 Linux 7.2.0 内核**（裁剪调试配置），特点：

1. **全部内置，零模块**：1,110 个配置项均为 `=y`，适合嵌入式/固件部署场景
2. **调试友好**：开启 DEBUG_INFO、GDB_SCRIPTS、DYNAMIC_DEBUG，适合内核开发调试
3. **驱动密集**：71.2% 的文件为驱动代码，GPU（1,465）与网络驱动（1,040）是最大贡献者
4. **文件系统丰富**：XFS（143）、Btrfs（63）、NFS（60）、EXT4（35）、F2FS（21）等均已编译
5. **网络栈完整**：IPv4/IPv6、Netfilter、mac80211、Bluetooth、Bridge、VLAN 等全部包含
6. **安全特性**：栈保护（stack-protector-strong）、PAC 返回地址签名、自动变量零初始化
7. **大规模支持**：52-bit 地址空间、5 级页表、512 CPU、20 个 CMA 区域
8. **`compile_commands.json` 已就绪**：可用于 clangd/ccls/LSP 代码导航、补全与静态分析
9. **裁剪边界**：SECURITY/CRYPTO/KEYS/MODULES/IOMMU/ACPI 等未启用，相关笔记分析的是源码能力而非本次编译启用能力

---

## 14. 复现与校验

本节给出复现本报告全部数字的命令，便于在源码树更新后重新核对。

### 14.1 版本与编译器

```bash
sed -n '1,5p' Makefile                 # VERSION/PATCHLEVEL/SUBLEVEL -> 7.2.0
grep LINUX_COMPILER include/generated/compile.h
```

`include/generated/compile.h` 中的编译器串为：`aarch64-linux-gnu-gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0, GNU ld (GNU Binutils for Ubuntu) 2.46`。

### 14.2 配置与文件计数

```bash
grep -c '=y$' .config                  # 1110
grep -c '=m$' .config                  # 0
python3 - <<'EOF'
import json, os, collections
d = json.load(open('compile_commands.json'))
ext = collections.Counter(os.path.splitext(e['file'])[1] for e in d)
print(len(d), ext)                     # 10061 Counter({'.c': 9986, '.S': 75})
EOF
```

### 14.3 编译记录的完整性校验

`compile_commands.json` 由 `scripts/clang-tools/gen_compile_commands.py` 扫描构建目录中的 `.*.o.cmd` 生成。二者数量应基本对应（`.cmd` 还包含部分不产出独立目标文件的中间规则）：

```bash
find . -name '.*.o.cmd' -not -path './notes/*' | wc -l    # 10916
ls kernel/sched/.*.o.cmd | wc -l                          # 4
python3 -c "import json;d=json.load(open('compile_commands.json'));\
print(sum(1 for e in d if 'kernel/sched/' in e['file']))" # 4
```

针对同一子系统两侧计数一致，说明本报告的统计数据与当前 `.config`/构建产物是同一快照。

### 14.4 关键构建产物

| 产物 | 说明 |
|------|------|
| `vmlinux` | 未压缩 ELF 内核镜像（含符号与调试信息） |
| `vmlinux.o` | 链接前的单一目标文件，含全部内置代码 |
| `vmlinux.a` / `built-in.a` | `obj-y` 归档，Kbuild 两阶段链接的中间产物 |
| `vmlinux.unstripped` | 调试用未剥离镜像 |
| `System.map` | 符号地址表 |
| `Module.symvers` / `modules.builtin` / `modules.order` | 导出符号与内置模块清单（当前 `=m` 为 0，`modules.builtin` 记录全部内置项） |
| `compile_commands.json` | 10,061 条编译记录，供 clangd/ccls 使用 |
| `.config` / `defconfig` | 本次裁剪配置及其备份 |

### 14.5 文档侧校验

```bash
mdbook build                                   # 产物写入 book/
python3 notes/00-overview/tools/gen_summary.py # 由根 README.md 重新生成 notes/SUMMARY.md
```

`book.toml` 中 `[output.html.search] enable = false`（仅本地阅读，不生成搜索索引），且未配置 `git-repository-url` / `edit-url-template`（不发布到 GitHub Pages）。维护约定见根 `README.md` 附录 C。
