# Linux 7.2 内核子系统地图

本文是整套笔记的总览入口，回答三个问题：

1. 内核源码目录分别对应什么子系统；
2. 一次请求/一次中断/一个进程从用户态到硬件的路径经过哪些层；
3. 阅读笔记时应该按什么顺序切入。

> 说明：仓库中的 `.config` 是为缩短编译时间刻意裁剪的调试配置。笔记描述的是 Linux 7.2 源码能力；涉及当前配置未启用的功能时，会在对应文档中单独标注。

---

## 1. 分层视图

```
用户态
  │  syscall / vDSO / io_uring / netlink
  ▼
┌────────────────────────────────────────────────────────────┐
│ 系统调用层   arch/arm64/kernel/syscall.c, fs/, mm/, kernel/ │
├────────────────────────────────────────────────────────────┤
│ 核心子系统                                                  │
│ 进程调度  kernel/sched/        内存管理  mm/                │
│ 时间      kernel/time/         同步      kernel/locking/     │
│ 异步      kernel/irq/, workqueue, rcu, bpf                  │
│ VFS       fs/                  网络      net/               │
│ 安全      security/            容器      kernel/cgroup/     │
├────────────────────────────────────────────────────────────┤
│ 驱动与总线                                                  │
│ driver core  drivers/base/     设备树    drivers/of/        │
│ PCIe         drivers/pci/      块层      block/             │
│ NVMe/SCSI    drivers/nvme,scsi 网络设备  drivers/net/       │
│ 虚拟化       virt/, drivers/virtio, drivers/vfio            │
├────────────────────────────────────────────────────────────┤
│ 架构相关                                                    │
│ arch/arm64/  MMU、异常向量、GIC、PSCI、KVM、启动            │
└────────────────────────────────────────────────────────────┘
  ▼
硬件
```

---

## 2. 源码目录速查

| 目录 | 职责 | 笔记入口 |
|---|---|---|
| `arch/arm64/` | ARM64 架构、启动、异常、MMU、KVM | [armv8a](../01-architecture/armv8a/1.summary.md) · [boot](../01-architecture/boot/start_kernel.md) · [interrupt](../01-architecture/interrupt/arm64_interrupt_subsystem_analysis.md) |
| `kernel/sched/` | 调度类、负载均衡、EEVDF、RT/DL | [调度子系统](../02-core/sched/schedule_subsystem_analysis.md) |
| `kernel/time/` | clocksource/clockevent、tick、hrtimer、NTP | [时间子系统](../02-core/time/time_subsystem_analysis.md) |
| `mm/` | 伙伴系统、SLUB、页表、回收、swap、THP | [内存管理](../02-core/mm/01-overview-core.md) |
| `kernel/locking/`, `kernel/rcu/` | 锁、RCU、内存屏障 | [并发与同步](../02-core/sync/concurrency_sync_analysis.md) |
| `kernel/irq/`, `kernel/workqueue.c` | 中断处理、软中断、工作队列 | [异步执行](../02-core/async/async-execution-summary.md) · [genirq](../01-architecture/interrupt/03-genirq.md) |
| `fs/` | VFS、ext4/XFS/Btrfs、procfs/sysfs | [VFS](../05-filesystems/vfs/fs.md) · [真实文件系统](../05-filesystems/real/01-ext4.md) |
| `block/` | bio、blk-mq、I/O 调度 | [块层](../04-storage/block/block-01-core-framework.md) |
| `net/` | socket、TCP/IP、netfilter、XDP | [网络核心](../06-networking/core/network_subsystem_analysis.md) · [高级网络](../06-networking/advanced/01-netfilter.md) |
| `drivers/base/` | device/driver/bus 模型 | [驱动模型](../03-drivers/driver-model/01-driver-model.md) |
| `drivers/pci/` | PCI/PCIe 枚举与驱动 | [PCIe](../03-drivers/pcie/pcie.md) |
| `security/` | LSM、SELinux、IMA/EVM、keyring | [安全子系统](../07-security/01-lsm.md) |
| `virt/`, `drivers/virtio/` | KVM、virtio、vhost | [虚拟化](../08-virtualization/01-kvm-arm64.md) |
| `kernel/cgroup/` | cgroup v1/v2、控制器 | [cgroup v2](../09-containers/01-cgroup-v2.md) |
| `kernel/module/`, `kernel/livepatch/` | 模块加载、热补丁、kexec | [模块与构建](../11-build/02-module-loader.md) |
| `rust/` | Rust for Linux | [Rust for Linux](../12-rust/01-rust-for-linux.md) |

---

## 3. 四条典型路径

### 3.1 一次系统调用

```
用户态 svc #0
  → arch/arm64/kernel/entry.S: el0_svc
  → do_el0_svc()
  → invoke_syscall()
  → sys_call_table[nr]
  → 子系统实现（fs/、mm/、kernel/…）
  → ret_to_user
```

入口笔记：[异常和中断](../01-architecture/armv8a/10.exception_intr.md)、[系统调用表](../02-core/syscall/arm64-syscall-table.md)。

### 3.2 一次中断

```
GICv3 产生 IRQ
  → arch/arm64/kernel/entry.S: el1_irq
  → irq_handler()
  → gic_handle_irq()
  → generic_handle_domain_irq()
  → irq_desc->handle_irq()
  → 驱动 handler / threaded IRQ
  → irq_exit() → softirq
```

入口笔记：[ARM64 中断子系统](../01-architecture/interrupt/arm64_interrupt_subsystem_analysis.md)、[genirq](../01-architecture/interrupt/03-genirq.md)。

### 3.3 一次文件读取

```
read(fd, buf, len)
  → ksys_read()
  → vfs_read()
  → file->f_op->read_iter()
  → 文件系统 read_iter（ext4/XFS/…）
  → filemap_read() / page cache
  → 缺页或回写：block 层
  → blk-mq → NVMe/SCSI 驱动
```

入口笔记：[VFS](../05-filesystems/vfs/fs.md)、[页缓存](../02-core/mm/03-page-cache-objects.md)、[块层](../04-storage/block/block-01-core-framework.md)。

### 3.4 一次进程创建

```
clone()/fork()
  → kernel_clone()
  → copy_process()
  → 复制 mm/files/fs/signal/nsproxy
  → wake_up_new_task()
  → schedule()
  → context_switch()
```

入口笔记：[进程创建退出](../02-core/sched/fork_exec_analysis.md)、[调度子系统](../02-core/sched/schedule_subsystem_analysis.md)。

---

## 4. 推荐阅读顺序

1. **架构基础**：ARMv8a 体系结构 → 启动过程 → 异常/中断。
2. **内核核心**：系统调用 → 进程与调度 → 内存管理 → 时间 → 同步/异步。
3. **数据与设备**：VFS → 块层 → PCIe/NVMe → 驱动模型与其他总线。
4. **网络与安全**：socket/TCP-IP → netfilter/XDP → LSM/IMA/crypto。
5. **进阶**：虚拟化 → 容器与隔离 → 调试/追踪 → 构建与模块。

---

## 5. 文档状态约定

| 标记 | 含义 |
|---|---|
| 完整 | 已有源码级分析，覆盖核心数据结构与调用链 |
| 部分 | 已有主体分析，但某些子方向仍需展开 |
| 新增 | 本轮补充的概览级文档，后续可继续加深 |
| 配置未启用 | 源码存在，但当前 `.config` 未开启，运行验证需先改配置 |

---

## 6. 关键文件清单

| 文件 | 作用 |
|---|---|
| `Makefile` | 内核版本与顶层构建入口 |
| `.config` | 当前裁剪配置，决定实际编入的功能 |
| `init/main.c` | start_kernel 与初始化顺序 |
| `arch/arm64/kernel/entry.S` | 异常/系统调用入口 |
| `kernel/sched/core.c` | 调度核心 |
| `mm/page_alloc.c`, `mm/slub.c` | 物理页与 SLUB |
| `fs/namespace.c`, `fs/namei.c` | VFS 挂载与路径查找 |
| `block/blk-mq.c` | 多队列块层 |
| `net/core/dev.c` | 网络设备与收发核心 |
| `drivers/base/dd.c` | 驱动 probe 核心 |
| `security/security.c` | LSM hook 分发 |
| `virt/kvm/kvm_main.c` | KVM 核心 |
| `kernel/cgroup/cgroup.c` | cgroup 核心 |
