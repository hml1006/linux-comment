# KVM/ARM64 虚拟化

## 1. 概述

KVM 是 Linux 内核内置的 Type-2 Hypervisor。在 ARM64 上，KVM 依赖 AArch64 的
虚拟化扩展（EL2）：宿主机内核运行在 EL1，KVM 在 EL2 中运行 vCPU 的上下文切换代码，
guest 运行在 EL0/EL1，Stage-2 翻译（VTTBR_EL2 + VTCR_EL2）负责 GPA→HPA 的映射。

源码分布：

```
virt/kvm/                 与架构无关的核心（VM/vCPU 生命周期、memslot、io bus、eventfd、irqfd）
arch/arm64/kvm/           ARM64 架构实现（stage-2、异常退出、VGIC、定时器、PSCI、VHE/pKVM 入口）
arch/arm64/kvm/hyp/       运行在 EL2 的 hypervisor 代码（vhe/ 与 nvhe/ 两套编译）
arch/arm64/kvm/vgic/     虚拟 GIC（v2/v3/v4/v5、ITS）
include/uapi/linux/kvm.h  用户态 ABI（ioctl 号、struct kvm_run、KVM_EXIT_* 等）
include/linux/kvm_host.h  内核内部核心数据结构
include/kvm/arm_vgic.h    VGIC 内部数据结构
```

当前 `.config` 中的相关选项：

```
CONFIG_KVM=y
CONFIG_KVM_COMMON=y
CONFIG_KVM_MMIO=y
CONFIG_HAVE_KVM_IRQCHIP=y
CONFIG_HAVE_KVM_IRQ_ROUTING=y
CONFIG_HAVE_KVM_DIRTY_RING=y
CONFIG_KVM_GENERIC_DIRTYLOG_READ_PROTECT=y
CONFIG_KVM_GUEST_MEMFD=y
CONFIG_KVM_VFIO=y
CONFIG_ARM64_LPA2=y
```

本文只讨论源码中真实存在的实现。当前配置未启用 pKVM（受保护虚拟机）相关能力时，
`is_protected_kvm_enabled()` 为假，`KVM_PGT_FN()` 宏展开为普通 `kvm_pgtable_*` 函数，
pKVM 分支仅作说明，不作为当前配置的运行路径。

## 2. 代码组织与关键入口

用户态通过 `/dev/kvm` 上的 ioctl 驱动全部操作，入口集中在 `virt/kvm/kvm_main.c`：

| ioctl | 内核入口 | 说明 |
| --- | --- | --- |
| `KVM_GET_API_VERSION` | `kvm_dev_ioctl()` | 返回 `KVM_API_VERSION` |
| `KVM_CREATE_VM` | `kvm_dev_ioctl_create_vm()` | 创建 VM，返回 VM fd |
| `KVM_CREATE_VCPU` | `kvm_vm_ioctl_create_vcpu()` | 创建 vCPU，返回 vCPU fd |
| `KVM_SET_USER_MEMORY_REGION(2)` | `kvm_vm_ioctl_set_memory_region()` | 注册 memory slot |
| `KVM_RUN` | `kvm_vcpu_ioctl()` → `kvm_arch_vcpu_ioctl_run()` | 进入 guest |
| `KVM_GET/SET_ONE_REG` | `kvm_arm_get_reg()` / `kvm_arm_set_reg()` | 通用寄存器访问 |
| `KVM_IRQFD` / `KVM_IOEVENTFD` / `KVM_SET_GSI_ROUTING` | `eventfd.c` / `irqchip.c` | 中断注入与路由 |

ARM64 私有 ioctl 定义在 `include/uapi/linux/kvm.h`，实现在
`arch/arm64/kvm/arm.c` 的 `kvm_arch_vcpu_ioctl()` 与 `kvm_arch_vm_ioctl()`：

- `KVM_ARM_VCPU_INIT`（`_IOW(KVMIO, 0xae, struct kvm_vcpu_init)`）→ `kvm_arch_vcpu_ioctl_vcpu_init()`
- `KVM_ARM_SET_DEVICE_ADDR` → 设置 VGIC 的 distributor/CPU 接口地址
- `KVM_ARM_PREFERRED_TARGET` → 返回默认 `struct kvm_vcpu_init`
- `KVM_SET_DEVICE_ATTR` / `KVM_ARM_VCPU_FINALIZE` → 设备属性与 vCPU 特性冻结

## 3. 核心数据结构

### 3.1 struct kvm

定义于 `include/linux/kvm_host.h`。与 ARM64 相关的字段：

```c
struct kvm {
    struct mutex slots_lock;                    /* 保护 memslot 更新 */
    struct mutex slots_arch_lock;
    struct kvm_memslots __memslots[KVM_MAX_NR_ADDRESS_SPACES][2];
    struct kvm_memslots __rcu *memslots[KVM_MAX_NR_ADDRESS_SPACES];
    struct xarray vcpu_array;
    atomic_t online_vcpus;
    int max_vcpus;
    int created_vcpus;
    struct mutex lock;
    struct kvm_io_bus __rcu *buses[KVM_NR_BUSES];  /* KVM_MMIO_BUS / KVM_PIO_BUS */
    struct kvm_irq_routing_table __rcu *irq_routing;
    struct mmu_notifier mmu_notifier;
    struct srcu_struct srcu;
    struct kvm_arch arch;
};
```

memslot 采用“双份 + generation”的无锁读方案：`__memslots[as_id][0/1]` 中一份为 active，
更新时先写入 inactive 再 `rcu_assign_pointer()` 切换，读侧靠 `srcu` + `generation`
判断缓存是否仍有效（见 `kvm_vcpu.last_used_slot` / `last_used_slot_gen`）。

### 3.2 struct kvm_memory_slot

```c
struct kvm_memory_slot {
    struct hlist_node id_node[2];
    struct interval_tree_node hva_node[2];
    struct rb_node gfn_node[2];
    gfn_t base_gfn;
    unsigned long npages;
    unsigned long *dirty_bitmap;
    struct kvm_arch_memory_slot arch;
    unsigned long userspace_addr;   /* HVA */
    u32 flags;                      /* KVM_MEM_LOG_DIRTY_PAGES / KVM_MEM_READONLY / KVM_MEM_GUEST_MEMFD */
    short id;
    u16 as_id;
};
```

三套索引树：HVA 区间树（`hva_node`）、GFN 红黑树（`gfn_node`）、id 哈希。
ARM64 的 `kvm_arch_memory_slot` 在 `arch/arm64/include/asm/kvm_mmu.h`，承载 stage-2 的 per-slot 信息。

### 3.3 struct kvm_vcpu

```c
struct kvm_vcpu {
    struct kvm *kvm;
    int cpu;
    int vcpu_id;      /* 用户态指定 */
    int vcpu_idx;     /* 在 kvm->vcpu_array 中的下标 */
    int mode;         /* IN_GUEST_MODE / OUTSIDE_GUEST_MODE */
    u64 requests;     /* KVM_REQ_* 位图 */
    struct kvm_run *run;      /* 与用户态共享的一页 */
    struct kvm_vcpu_arch arch;
    struct kvm_vcpu_stat stat;
    struct kvm_dirty_ring dirty_ring;
    struct kvm_memory_slot *last_used_slot;
    u64 last_used_slot_gen;
};
```

`vcpu->run` 由 `kvm_vm_ioctl_create_vcpu()` 用 `alloc_page()` 分配，用户态通过
`mmap(vcpu_fd, offset=0)`（`KVM_GET_VCPU_MMAP_SIZE` 通常为 `PAGE_SIZE`）映射。

### 3.4 ARM64 的 kvm_arch / kvm_vcpu_arch

`struct kvm_arch`（`arch/arm64/include/asm/kvm_host.h`）核心字段：

```c
struct kvm_arch {
    struct kvm_s2_mmu mmu;           /* 规范 stage-2 MMU */
    u64 fgu[__NR_FGT_GROUP_IDS__];   /* Fine-Grained UNDEF 跟踪 */
    struct kvm_s2_mmu *nested_mmus;  /* NV guest 用的额外 stage-2 */
    struct vgic_dist vgic;
    struct arch_timer_vm_data timer_data;
    u32 psci_version;
    struct mutex config_lock;
    unsigned long flags;             /* KVM_ARCH_FLAG_* */
    DECLARE_BITMAP(vcpu_features, KVM_VCPU_MAX_FEATURES);
    struct kvm_mpidr_data *mpidr_data;
    struct arm_pmu *arm_pmu;
    struct kvm_smccc_features smccc_feat;
};
```

`struct kvm_vcpu_arch` 关键字段：`ctxt`（`kvm_cpu_context`，guest 通用寄存器/系统寄存器）、
`sve_state`/`fp_type`（FPSIMD/SVE）、`hw_mmu`（下次切换时硬件使用的 stage-2）、
`hcr_el2`/`mdcr_el2`/`fgt[]`（trap 配置）、`fault`（ESR/FAR/HPFAR 快照）、
`vgic_cpu`、`timer_cpu`、`pmu`、`mp_state`、`mmu_page_cache`。

## 4. VM 与 vCPU 生命周期

### 4.1 创建 VM：KVM_CREATE_VM

```
用户态 ioctl(/dev/kvm, KVM_CREATE_VM, type)
  └─ kvm_dev_ioctl()                    virt/kvm/kvm_main.c
      └─ kvm_dev_ioctl_create_vm(type)
          ├─ get_unused_fd_flags(O_CLOEXEC)
          ├─ kvm_create_vm(type, fdname)
          │   ├─ kvm_arch_init(type)          /* 架构前置检查 */
          │   ├─ hardware_enable_all()        /* 首次创建时使能 EL2 */
          │   ├─ kvm_arch_init_vm(kvm, type)
          │   ├─ kvm_init_mmu_notifier()
          │   └─ kvm_create_vm_debugfs()
          └─ anon_inode_getfile("kvm-vm", &kvm_vm_fops, kvm, O_RDWR)
```

`kvm_arch_init_vm()`（`arch/arm64/kvm/arm.c`）做的事：

1. 校验收到的 machine type 只含 `KVM_VM_TYPE_ARM_MASK` 允许位；
2. `mutex_init(&kvm->arch.config_lock)`，`kvm_init_nested(kvm)`；
3. `kvm_share_hyp(kvm, kvm + 1)`：把 `struct kvm` 所在页与 hyp 共享（nVHE 下必需）；
4. 分配 `supported_cpus` cpumask；
5. `kvm_init_stage2_mmu(kvm, &kvm->arch.mmu, type)` 建立 stage-2 页表；
6. 若启用 pKVM，则 `pkvm_init_host_vm()`；否则若 type 带 `KVM_VM_TYPE_ARM_PROTECTED` 返回 `-EINVAL`；
7. `kvm_vgic_early_init()`、`kvm_timer_init_vm()`；
8. `kvm->max_vcpus = kvm_arm_default_max_vcpus()`（受宿主 GIC 模型限制）；
9. `kvm_arm_init_hypercalls()`，清零 `vcpu_features`。

### 4.2 创建 vCPU：KVM_CREATE_VCPU

```
kvm_vm_ioctl(kvm, KVM_CREATE_VCPU, id)
  └─ kvm_vm_ioctl_create_vcpu(kvm, id)
      ├─ 校验 id < KVM_MAX_VCPU_IDS、created_vcpus < max_vcpus
      ├─ kvm_arch_vcpu_precreate(kvm, id)         /* ARM64：检查 vcpu_features 未冻结 */
      ├─ kmem_cache_zalloc(kvm_vcpu_cache)
      ├─ alloc_page() → vcpu->run
      ├─ kvm_vcpu_init(vcpu, kvm, id)
      ├─ kvm_arch_vcpu_create(vcpu)               /* 建 vgic_cpu、timer、pmu、FP 状态 */
      ├─ kvm_dirty_ring_alloc()（若配置了 dirty ring）
      ├─ xa_insert(&kvm->vcpu_array, vcpu->vcpu_idx, vcpu)
      └─ anon_inode_getfile_fmode("kvm-vcpu", &kvm_vcpu_fops, ...)
```

`kvm_arch_vcpu_create()`（`arch/arm64/kvm/arm.c`）会调用 `kvm_vgic_vcpu_init()`、
`kvm_timer_vcpu_init()`、`kvm_pmu_vcpu_init()`，并在 NV 场景下预建 stage-2 影子页表。

### 4.3 vCPU 初始化：KVM_ARM_VCPU_INIT

`kvm_arch_vcpu_ioctl_vcpu_init(vcpu, &init)`：校验 `init.target` 与宿主
`KVM_ARM_TARGET_*` 匹配；按 `init.features` 置位 `kvm->arch.vcpu_features`
（首次 INIT 锁定，后续 INIT 必须一致）；处理 `KVM_ARM_VCPU_POWER_OFF`、
`KVM_ARM_VCPU_EL1_32BIT`、`KVM_ARM_VCPU_PSCI_0_2`、`KVM_ARM_VCPU_PMU_V3`、
`KVM_ARM_VCPU_SVE`、`KVM_ARM_VCPU_PTRAUTH_*`、`KVM_ARM_VCPU_HAS_EL2` 等；
最后设置 `vcpu->arch.reset_state` 与 MP state。

### 4.4 运行：KVM_RUN

```
kvm_vcpu_ioctl(filp, KVM_RUN, 0)
  ├─ 若 current 变了则 kvm_arch_vcpu_run_pid_change(vcpu)
  ├─ vcpu->wants_to_run = !run->immediate_exit
  ├─ kvm_arch_vcpu_ioctl_run(vcpu)
  └─ vcpu->wants_to_run = false
```

`kvm_arch_vcpu_ioctl_run()` 的循环（`arch/arm64/kvm/arm.c`）：

1. 若上次退出原因是 `KVM_EXIT_MMIO`，先 `kvm_handle_mmio_return()` 把用户态填入的数据写回 guest 寄存器；
2. `vcpu_load(vcpu)` → `kvm_arch_vcpu_load()`：加载 `hw_mmu`、timer、debug、FGT、VHE 上下文；
3. `kvm_sigset_activate()`；
4. `while (ret > 0)`：
   - `kvm_xfer_to_guest_mode_handle_work()`、`check_vcpu_requests()` 处理 `KVM_REQ_*`；
   - `preempt_disable()` → `local_irq_disable()`；
   - `kvm_nested_flush_hwstate()`、`kvm_pmu_flush_hwstate()`、`kvm_vgic_flush_hwstate()`；
   - `smp_store_mb(vcpu->mode, IN_GUEST_MODE)`；
   - `kvm_vcpu_exit_request()` 判断是否需要放弃进入 guest；
   - `kvm_arm_vcpu_enter_exit()` → `kvm_call_hyp_ret(__kvm_vcpu_run, vcpu)`；
   - 返回后 `vcpu->mode = OUTSIDE_GUEST_MODE`，`kvm_pmu_sync_hwstate()`、`kvm_vgic_sync_hwstate()`；
   - `handle_exit()` 依据 ESR 分发处理，返回值 >0 则继续循环。

### 4.5 销毁

VM fd 关闭触发 `kvm_vm_release()` → `kvm_put_kvm()`；引用计数归零时
`kvm_destroy_vm()` 调用 `kvm_arch_destroy_vm()`：

```c
void kvm_arch_destroy_vm(struct kvm *kvm)
{
    bitmap_free(kvm->arch.pmu_filter);
    free_cpumask_var(kvm->arch.supported_cpus);
    kvm_vgic_destroy(kvm);
    if (is_protected_kvm_enabled())
        pkvm_destroy_hyp_vm(kvm);
    kvm_uninit_stage2_mmu(kvm);          /* 释放 stage-2 页表 */
    kvm_destroy_mpidr_data(kvm);
    kfree(kvm->arch.sysreg_masks);
    kvm_destroy_vcpus(kvm);
    kvm_unshare_hyp(kvm, kvm + 1);
    kvm_arm_teardown_hypercalls(kvm);
}
```

## 5. Stage-2 页表

### 5.1 结构

- `struct kvm_s2_mmu`（`arch/arm64/include/asm/kvm_mmu.h`）持有 `pgt`（`struct kvm_pgtable`）、
  `vtcr`、`pgd_phys`、`last_vcpu_ran`、`split_page_cache` 等；
- `kvm_pgtable_stage2_init()`（`arch/arm64/kvm/hyp/pgtable.c`）负责建立 PGD；
- `kvm_init_ipa_range()` 根据宿主的 IPA 上限（`get_kvm_ipa_limit()`）和 LPA2 支持设置 `VTCR_EL2`：
  `T0SZ`、`SL0`、`PS`、`TG0`、`HA`/`HD` 等；`VTCR_EL2_IPA(vtcr)` 给出 guest 可见的 IPA 宽度。

### 5.2 初始化与销毁

```
kvm_arch_init_vm()
  └─ kvm_init_stage2_mmu(kvm, &kvm->arch.mmu, type)
      ├─ kvm_init_ipa_range(mmu, type)
      ├─ kzalloc(struct kvm_pgtable)
      ├─ mmu->arch = &kvm->arch
      ├─ kvm_pgtable_stage2_init(pgt, mmu, &kvm_s2_mm_ops)
      ├─ alloc_percpu(last_vcpu_ran)
      └─ mmu->pgd_phys = __pa(pgt->pgd)
```

销毁路径为 `kvm_uninit_stage2_mmu()` → `kvm_free_stage2_pgd()` →
`kvm_pgtable_stage2_destroy()`。

### 5.3 缺页处理

guest 访问未被 stage-2 映射的 IPA 产生 Data/Instruction Abort，ESR_EL2 的 `FSC`
表明 fault 类型。`kvm_handle_guest_abort()`（`arch/arm64/kvm/mmu.c`）流程：

1. `kvm_vcpu_abt_issea()` → `kvm_handle_guest_sea()`；
2. 取 `fault_ipa = kvm_vcpu_get_fault_ipa(vcpu)`；超出 `get_kvm_ipa_limit()` →
   `kvm_inject_size_fault()`；落在 IPA 与 PARange 之间 → `kvm_inject_sea()`；
3. 校验 FSC 属于 translation / permission / access-flag / exclusive-atomic 之一；
4. `srcu_read_lock(&kvm->srcu)`；
5. 嵌套 guest 且存在 shadow stage-2 时，`kvm_walk_nested_s2()` 把 L2 IPA 翻译到 L1 IPA；
6. `gfn_to_memslot()` + `gfn_to_hva_memslot_prot()`；HVA 非法且是写 fault：
   取指 → `-ENOEXEC`，否则 `io_mem_abort()` 走 MMIO 模拟；
7. access-flag fault → `handle_access_fault()` 只做 `kvm_pgtable_stage2_mkyoung()`；
8. 否则构造 `struct kvm_s2_fault_desc`，进入 `user_mem_abort()`（`gmem_abort()` 处理
   guest_memfd，`pkvm_mem_abort()` 处理 pKVM）：取 VMA → 固定 pfn → 计算
   `KVM_PGTABLE_PROT_*` → `kvm_pgtable_stage2_map()`。

memslot 变更时通过 `kvm_unmap_gfn_range()` / `kvm_set_spte_gfn()` 等 MMU notifier
回调同步 stage-2，`kvm_arch_flush_shadow_memslot()` 用于强制失效。

## 6. 异常退出与 MMIO

### 6.1 退出分发表

`handle_exit()`（`arch/arm64/kvm/handle_exit.c`）根据 `ESR_EL2.EC` 查
`arm_exit_handlers[]`：

```c
static exit_handle_fn arm_exit_handlers[] = {
    [0 ... ESR_ELx_EC_MAX] = kvm_handle_unknown_ec,
    [ESR_ELx_EC_WFx]       = kvm_handle_wfx,
    [ESR_ELx_EC_SYS64]     = kvm_handle_sys_reg,
    [ESR_ELx_EC_IABT_LOW]  = kvm_handle_guest_abort,
    [ESR_ELx_EC_DABT_LOW]  = kvm_handle_guest_abort,
    ...
};
```

`handle_hvc()` 处理 guest 的 HVC（PSCI / KVM hypercall），`handle_smc()` 处理
配置为 trap 的 SMC，`kvm_handle_wfx()` 实现 WFI/WFE 的阻塞与 vCPU 调度出。

### 6.2 MMIO 模拟

`io_mem_abort(vcpu, fault_ipa)`（`arch/arm64/kvm/mmio.c`）：

1. 读取 ESR；若 `!kvm_vcpu_dabt_isvalid()`（NISV）：
   - 受保护 VM → `kvm_inject_sea_dabt()`；
   - 用户态声明了 `KVM_CAP_ARM_NISV_TO_USER`（`KVM_ARCH_FLAG_RETURN_NISV_IO_ABORT_TO_USER`）
     → 填 `run->arm_nisv`，`exit_reason = KVM_EXIT_ARM_NISV`；
   - 否则 `-ENOSYS`；
2. 若 DFSC 表明是 LD64B/ST64B 等复合访问，返回 `KVM_EXIT_ARM_LDST64B`；
3. `kvm_vcpu_dabt_get_as()` 取访问宽度，`kvm_vcpu_dabt_get_rd()` 取目标寄存器；
4. 读：`kvm_io_bus_read(vcpu, KVM_MMIO_BUS, addr, len, data_buf)`；
   写：`kvm_io_bus_write(...)`；
5. 填充 `run->mmio = { .is_write, .phys_addr, .len, .data }`，置 `vcpu->mmio_needed = 1`；
6. 若 in-kernel io device 命中（`ret == 0`），直接 `kvm_handle_mmio_return()` 并返回 1；
   否则 `run->exit_reason = KVM_EXIT_MMIO` 返回 0，交给用户态。

用户态完成模拟后再次 `KVM_RUN`，内核在入口处调用 `kvm_handle_mmio_return()`：
对读操作把 `run->mmio.data` 解码并按符号扩展写入 `kvm_vcpu_dabt_get_rd()` 指定的寄存器，
然后 `kvm_incr_pc()` 跳过该指令。

`struct kvm_run`（`include/uapi/linux/kvm.h`）是共享页，`exit_reason` 取值为
`KVM_EXIT_MMIO`(6)、`KVM_EXIT_ARM_NISV`(28)、`KVM_EXIT_SYSTEM_EVENT`(24)、
`KVM_EXIT_ARM_SEA`(41)、`KVM_EXIT_MEMORY_FAULT`(39) 等。

## 7. VGIC

### 7.1 模型

宿主 GIC 驱动在初始化时通过 `vgic_set_kvm_info()` 把 `struct gic_kvm_info` 交给 KVM，
KVM 用 `kvm_vgic_hyp_init()` 建立 hyp 侧入口。用户态用 `KVM_CREATE_DEVICE`
（`KVM_DEV_TYPE_ARM_VGIC_V2/V3/V4/V5`）触发 `kvm_vgic_create()`：

- `kvm_vgic_early_init()` 在 `kvm_arch_init_vm()` 中初始化 `vgic_dist` 锁与链表；
- `kvm_vgic_create()` 分配 `spis[]`（SPI 描述符数组）、初始化 `dist_iodev`/`cpuif_iodev`
  并注册进 `KVM_MMIO_BUS`；
- `kvm_vgic_vcpu_init()` 为每个 vCPU 建立私有中断（SGI/PPI）数组与 redistributor 设备；
- `vgic_lazy_init()` / `vgic_init()` 在首次运行前完成延迟初始化；
- `kvm_vgic_map_resources()` 在用户态设置完地址后真正建立映射。

### 7.2 数据结构

```c
struct vgic_irq {
    raw_spinlock_t irq_lock;
    u32 intid;
    struct list_head ap_list;      /* 在目标 vCPU 的 ap_list 上 */
    struct kvm_vcpu *vcpu;
    struct kvm_vcpu *target_vcpu;
    bool pending_latch:1;
    enum vgic_irq_config config:1;
    bool line_level:1;
    bool enabled:1;
    bool active:1;
    bool hw:1;
    bool on_lr:1;
    refcount_t refcount;
    u32 hwintid;
    unsigned int host_irq;
    union { u8 targets; u32 mpidr; };
    u8 priority;
    u8 group;
    const struct irq_ops *ops;
    void *owner;
};
```

`struct vgic_dist` 持有 `vgic_model`、`nr_spis`、`spis`、`dist_iodev`、`cpuif_iodev`、
`propbaser`、`lpi_xa`、`its_vm` 等。

### 7.3 注入与运行期同步

- 设备注入：`kvm_vgic_inject_irq()`（`vgic.c`）→ 找到 `vgic_irq` → 置 pending → 若目标
  vCPU 未运行则 `kvm_vcpu_kick()`；
- irqfd：`vgic_irqfd_set_irq()`（`vgic-irqfd.c`）→ `kvm_vgic_inject_irq()`；
- 进入 guest 前 `kvm_vgic_flush_hwstate()` 把 `ap_list` 上的 pending 中断写入
  list registers（GICv2/v3）或设置 `ICH_LR*_EL2`；
- 退出后 `kvm_vgic_sync_hwstate()` 回收 LR、更新 active/pending 状态；
- GICv4 的 `its_vm` 允许 VLPI 直接投递到 vCPU，避免 VM exit。

## 8. VHE 与 EL2 运行模式

### 8.1 判定

`has_vhe()`（`arch/arm64/include/asm/virt.h`）在非 hyp 编译单元中读
`ARM64_HAS_VIRT_HOST_EXTN` capability；在 VHE 编译单元恒为 true，在 nVHE 编译单元恒为 false，
以便编译器裁掉无关代码。`is_protected_kvm_enabled()` 对应 `ARM64_KVM_PROTECTED_MODE`。

### 8.2 VHE 下的 vCPU 运行

`__kvm_vcpu_run_vhe()`（`arch/arm64/kvm/hyp/vhe/switch.c`）：

```
fpsimd_lazy_switch_to_guest(vcpu)
sysreg_save_host_state_vhe(host_ctxt)
__activate_traps(vcpu)              /* 写 HCR_EL2/HCRX_EL2/MDCR_EL2/FGT */
__kvm_adjust_pc(vcpu)
sysreg_restore_guest_state_vhe(guest_ctxt)
__debug_switch_to_guest(vcpu)
do { exit_code = __guest_enter(vcpu); }         /* eret 到 guest */
while (fixup_guest_exit(vcpu, &exit_code));     /* 原地处理无需出 hyp 的退出 */
sysreg_save_guest_state_vhe(guest_ctxt)
__deactivate_traps(vcpu)
sysreg_restore_host_state_vhe(host_ctxt)
__debug_switch_to_host(vcpu)
isb(); fpsimd_lazy_switch_to_host(vcpu)
```

VHE 下宿主内核本身运行在 EL2，KVM 代码无需切换页表即可访问宿主；
`kvm_update_va_mask()`（`arch/arm64/kvm/va_layout.c`）在启动时按 VA 布局计算
hyp 符号的 tag/掩码（`tag_lsb`、`tag_val`、`va_mask`），并打补丁到
`kvm_hyp_vector` 的跳转指令中。

nVHE 模式下同一逻辑由 `arch/arm64/kvm/hyp/nvhe/` 下代码实现，通过
`kvm_call_hyp_ret()` 经 `hvc` 陷入 EL2；pKVM 则进一步把 EL2 代码隔离为独立 hypervisor。

## 9. 当前配置下的行为差异

- `CONFIG_IOMMU_SUPPORT`/`CONFIG_ACPI` 未启用：不影响 KVM 本身，但影响 VFIO 直通
  （见 `03-vfio-iommu.md`）；ARM64 上 KVM 的 stage-2 不依赖 IOMMU。
- `CONFIG_KVM_GUEST_MEMFD=y`：`gmem_abort()` 会走 `kvm_gmem_get_pfn()` 而非用户态 VMA 固定页。
- 未启用 pKVM：`KVM_PGT_FN(fn)` 恒为 `fn`，`pkvm_*` 分支不执行。
- dirty ring 已编译（`CONFIG_HAVE_KVM_DIRTY_RING`），需用户态通过 `KVM_CAP_DIRTY_LOG_RING`
  显式启用；否则仍用 `KVM_GET_DIRTY_LOG` 位图。

## 10. 关键调用链速查

```
创建：  ioctl(/dev/kvm, KVM_CREATE_VM) → kvm_dev_ioctl_create_vm → kvm_create_vm
        → kvm_arch_init_vm (arm.c) → kvm_init_stage2_mmu (mmu.c)
加 vCPU：ioctl(vm_fd, KVM_CREATE_VCPU) → kvm_vm_ioctl_create_vcpu → kvm_arch_vcpu_create
初始化：ioctl(vcpu_fd, KVM_ARM_VCPU_INIT) → kvm_arch_vcpu_ioctl_vcpu_init
运行：  ioctl(vcpu_fd, KVM_RUN) → kvm_vcpu_ioctl → kvm_arch_vcpu_ioctl_run
        → kvm_arm_vcpu_enter_exit → kvm_call_hyp_ret(__kvm_vcpu_run)
        → __kvm_vcpu_run_vhe / __kvm_vcpu_run（nVHE）→ __guest_enter
退出：  __guest_enter 返回 → fixup_guest_exit → handle_exit
        → arm_exit_handlers[EC] → kvm_handle_guest_abort / handle_hvc / kvm_handle_wfx
MMIO：  kvm_handle_guest_abort → io_mem_abort → kvm_io_bus_read/write
        → run->exit_reason = KVM_EXIT_MMIO → 用户态模拟后再次 KVM_RUN
        → kvm_handle_mmio_return
```

## 11. 关键文件清单

| 路径 | 作用 |
| --- | --- |
| `include/uapi/linux/kvm.h` | 用户态 ABI：ioctl 号、`struct kvm_run`、`KVM_EXIT_*`、`KVM_CAP_*` |
| `include/linux/kvm_host.h` | `struct kvm`、`struct kvm_vcpu`、`struct kvm_memory_slot`、`kvm_io_bus` |
| `virt/kvm/kvm_main.c` | VM/vCPU 创建与销毁、ioctl 分发、memslot、io bus |
| `virt/kvm/eventfd.c` | irqfd / ioeventfd |
| `virt/kvm/irqchip.c` | 中断路由（`KVM_SET_GSI_ROUTING`） |
| `virt/kvm/guest_memfd.c` | guest_memfd 后端 |
| `arch/arm64/kvm/arm.c` | ARM64 VM/vCPU 生命周期、`KVM_RUN` 主循环 |
| `arch/arm64/kvm/mmu.c` | stage-2 页表、缺页处理、MMU notifier |
| `arch/arm64/kvm/mmio.c` | MMIO 模拟与返回处理 |
| `arch/arm64/kvm/handle_exit.c` | `arm_exit_handlers[]`、HVC/SMC/WFx |
| `arch/arm64/kvm/guest.c` | 通用寄存器与 `ONE_REG` 访问 |
| `arch/arm64/kvm/sys_regs.c` | 系统寄存器陷出与模拟 |
| `arch/arm64/kvm/vgic/vgic.c` | 中断注入、flush/sync hwstate |
| `arch/arm64/kvm/vgic/vgic-init.c` | VGIC 创建与初始化 |
| `arch/arm64/kvm/vgic/vgic-its.c` | ITS/LPI 模拟 |
| `arch/arm64/kvm/hyp/vhe/switch.c` | VHE 下 vCPU 上下文切换与 trap 配置 |
| `arch/arm64/kvm/hyp/nvhe/switch.c` | nVHE 下对应实现 |
| `arch/arm64/kvm/hyp/pgtable.c` | stage-2 页表操作原语 |
| `arch/arm64/kvm/va_layout.c` | hyp VA 掩码/向量补丁 |
| `arch/arm64/include/asm/kvm_host.h` | `struct kvm_arch`、`struct kvm_vcpu_arch` |
| `arch/arm64/include/asm/kvm_mmu.h` | `struct kvm_s2_mmu`、stage-2 辅助函数 |
| `include/kvm/arm_vgic.h` | `struct vgic_irq`、`struct vgic_dist` |
