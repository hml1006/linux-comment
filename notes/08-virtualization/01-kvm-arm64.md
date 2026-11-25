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

## 12. 全局初始化：kvm_init() 与 /dev/kvm 的诞生

前面第 2 节从「用户态视角」列出了 ioctl 入口，但没有回答一个问题：这些 fops、
`kvm_vcpu_cache`、per-CPU 的 kick 掩码是在什么时候准备好的？答案在
`virt/kvm/kvm_main.c` 的 `kvm_init()`（本树约 6481 行）。它由
`arch/arm64/kvm/arm.c` 的模块初始化路径调用，在 ARM64 上还负责把 `kvm_arch_init()`
得到的硬件能力（IPA 上限、VGIC 型号）接到通用框架上。

`kvm_init()` 的执行顺序有严格约束，可以分成五段：

| 阶段 | 主要动作 | 失败后回退 |
| --- | --- | --- |
| ① 对象缓存 | `kmem_cache_create_usercopy("kvm_vcpu", vcpu_size, vcpu_align, SLAB_ACCOUNT, ...)` | 直接返回 `-ENOMEM` |
| ② per-CPU 资源 | `for_each_possible_cpu()` 分配 `per_cpu(cpu_kick_mask, cpu)` | `err_cpu_kick_mask` |
| ③ 子系统注册 | `kvm_irqfd_init()`、`kvm_async_pf_init()` | `err_irqfd` / `err_async_pf` |
| ④ 框架接线 | 设置四个 fops 的 `owner`、`kvm_preempt_ops.sched_in/sched_out`、`kvm_init_debug()`、`kvm_vfio_ops_init()`、`kvm_gmem_init()`、`kvm_init_virtualization()` | 逐级 goto |
| ⑤ 暴露设备 | `misc_register(&kvm_dev)` | 先 `kvm_uninit_virtualization()` |

几个值得注意的细节：

1. `kvm_vcpu` 缓存使用 `kmem_cache_create_usercopy()` 而不是普通的
   `kmem_cache_create()`。第 5 个参数 `offsetof(struct kvm_vcpu, arch)` 与第 6 个参数
   `offsetofend(struct kvm_vcpu, stats_id) - offsetof(struct kvm_vcpu, arch)` 圈出了一块
   usercopy 白名单区域。也就是说，`copy_to_user()`/`copy_from_user()` 只允许触碰
   `arch` 到 `stats_id` 这一段，其余字段（链表指针、`kvm` 反向指针、`run` 页指针）
   即使被越界访问也会被 `CONFIG_HARDENED_USERCOPY` 拦下。这是 vCPU ioctl 大量
   「整块拷贝架构状态」的前提。
2. `kvm_preempt_ops.sched_in = kvm_sched_in`、`sched_out = kvm_sched_out` 把 KVM 挂进
   调度器的 preempt notifier。这样当持有 vCPU 的任务被抢占时，KVM 能在
   `kvm_sched_out()` 里把 vCPU 从「运行中」状态摘出来，避免另一个 CPU 上的
   `kvm_vcpu_kick()` 打到一个已经不在 guest 里的线程。
3. `kvm_init_virtualization()` 有两种实现（本树 5744 与 5766 行附近的
   `#else` 分支）：支持「全局一次性使能」的架构会在模块加载时就
   `kvm_enable_virtualization()`；否则延迟到第一次 `KVM_CREATE_VM`。
   而 `kvm_create_vm()` 内部还会再调一次 `kvm_enable_virtualization()`
   （本树 1180 行），所以「首次创建 VM 时才真正打开 EL2」这条老规则在
   per-VM 使能的实现下依然成立。
4. 注册 `/dev/kvm` 必须是最后一步，源码注释写得很直白：
   `Registration _must_ be the very last thing done, as this exposes /dev/kvm to userspace`。
   一旦 `misc_register()` 成功，用户态立刻可以 `open("/dev/kvm")`，任何尚未初始化的
   全局状态都会变成可触发的空指针。

对应的销毁函数 `kvm_exit()` 顺序大致相反：`misc_deregister(&kvm_dev)` →
`kvm_uninit_virtualization()` → `debugfs_remove_recursive(kvm_debugfs_dir)` →
释放 `cpu_kick_mask` → `kmem_cache_destroy(kvm_vcpu_cache)` →
`kvm_gmem_exit()`、`kvm_vfio_ops_exit()`、`kvm_async_pf_deinit()`、`kvm_irqfd_exit()`。
注意 `kvm_exit()` 里没有显式销毁 `kvm_dev` 背后仍存活的 VM：那些 VM 由各自的
`kvm_put_kvm()` 引用计数负责，模块卸载在还有 VM 时会被 `try_module_get()` 挡住。

`kvm_init()` 与 `kvm_exit()` 都用 `EXPORT_SYMBOL_FOR_KVM_INTERNAL()` 导出，
而不是 `EXPORT_SYMBOL_GPL()`。这个宏在本树里表示「只对编进同一 KVM 复合对象的
代码可见」，避免架构代码通过模块符号表反向依赖通用核心。

## 13. kvm_create_vm() 逐步剖析

`kvm_dev_ioctl(KVM_CREATE_VM)` 最终落到 `kvm_create_vm(type, fdname)`
（`virt/kvm/kvm_main.c` 约 1098 行）。它是理解 KVM 内存模型的起点，
因为 memslot 的双份数组、`srcu`、io bus 都在这里一次性建好。

```c
static struct kvm *kvm_create_vm(unsigned long type, const char *fdname)
{
    struct kvm *kvm = kvm_arch_alloc_vm();      /* 架构决定 struct kvm 的实际大小 */
    ...
    KVM_MMU_LOCK_INIT(kvm);                     /* mmu_lock: rwlock 或 spinlock */
    mmgrab(current->mm);
    kvm->mm = current->mm;                      /* 绑定创建者的地址空间 */
    kvm_eventfd_init(kvm);
    mutex_init(&kvm->lock);
    mutex_init(&kvm->irq_lock);
    mutex_init(&kvm->slots_lock);
    mutex_init(&kvm->slots_arch_lock);
    spin_lock_init(&kvm->mn_invalidate_lock);
    rcuwait_init(&kvm->mn_memslots_update_rcuwait);
    xa_init(&kvm->vcpu_array);
    ...
}
```

逐项说明：

- `kvm_arch_alloc_vm()` 而不是 `kzalloc(sizeof(struct kvm))`。ARM64 的
  `struct kvm_arch` 里挂着 `vgic_dist`、`timer_data`、`kvm_s2_mmu` 等大对象，
  架构代码可以用 `kzalloc(sizeof(struct kvm) + arch_extra)` 的方式一次性分配，
  所以通用代码不能假设 `struct kvm` 的确切尺寸。
- `mmgrab(current->mm)` 让 `struct mm_struct` 在 VM 生命周期内不被释放。
  KVM 的 memslot 保存的是 HVA，缺页处理时要拿这个 `mm` 去 `get_user_pages`；
  如果 VMM 线程退出导致 `mm` 被销毁而 VM 还活着，就会出现 use-after-free。
  引用在 `kvm_destroy_vm()` 里通过 `mmdrop()` 归还。
- `KVM_MMU_LOCK_INIT(kvm)` 展开后取决于 `KVM_HAVE_MMU_RWLOCK`：
  `include/linux/kvm_host.h` 中 `struct kvm` 的第一个字段就是
  `rwlock_t mmu_lock` 或 `spinlock_t mmu_lock`（本树 770–775 行）。
  选 `rwlock_t` 的架构允许「缺页处理读锁 + 批量 unmap 写锁」并发，
  这也是 ARM64 stage-2 缺页路径里能看到 `read_lock(&vcpu->kvm->mmu_lock)` 的原因。
- `init_srcu_struct(&kvm->srcu)` 与 `init_srcu_struct(&kvm->irq_srcu)` 是两套独立的
  SRCU 域：前者保护 memslot 指针，后者保护中断路由表。分开的好处是
  「删除 memslot 要等所有 vCPU 退出 SRCU 读临界区」，而「改中断路由」不必被
  长时间的缺页处理拖住。
- memslot 双份数组的初始化循环如下：

```c
for (i = 0; i < kvm_arch_nr_memslot_as_ids(kvm); i++) {
    for (j = 0; j < 2; j++) {
        slots = &kvm->__memslots[i][j];
        atomic_long_set(&slots->last_used_slot, (unsigned long)NULL);
        slots->hva_tree = RB_ROOT_CACHED;
        slots->gfn_tree = RB_ROOT;
        hash_init(slots->id_hash);
        slots->node_idx = j;
        /* Generations must be different for each address space. */
        slots->generation = i;
    }
    rcu_assign_pointer(kvm->memslots[i], &kvm->__memslots[i][0]);
}
```

  这里有一个容易被忽略的设计：`generation` 取的是 **address space id** `i`，
  而不是「第几次更新」的计数器。原因是每个地址空间（ARM64 上通常只有
  `KVM_ADDRESS_SPACE_NUM == 1`，但架构可支持多个）各自有一对 active/inactive
  数组，读者只需要判断「我缓存的 slot 属于哪个 as_id、哪个 node_idx」即可，
  不需要全局单调计数。真正的「这次更新是第几代」由
  `kvm_swap_active_memslots()` 在切换时重算（详见第 15 节）。
- `kvm->max_vcpus = KVM_MAX_VCPUS` 只是通用上限占位，ARM64 会在
  `kvm_arch_init_vm()` 末尾用 `kvm_arm_default_max_vcpus()` 覆盖，
  再被 `kvm_vgic_create()` 按 GIC 型号（V2/V3/V5）二次收紧。
  所以「能创建多少 vCPU」是三层限制取最小值的产物。
- `kvm_arch_init_vm()` 在 bus 分配之后、`kvm_enable_virtualization()` 之前调用。
  这个顺序不能反：约束是 stage-2 页表（`kvm_init_stage2_mmu()`）必须先建好，
  而 per-VM 使能虚拟化又需要 VM 结构已经完整。
- `kvm_init_mmu_notifier()` 注册 `kvm->mmu_notifier`，把 VMM 进程的
  `mmu_notifier_invalidate_range_start/end`、`invalidate_range` 接到
  `kvm_unmap_gfn_range()`、`kvm_set_spte_gfn()` 等回调上。这是 stage-2 与
  宿主页表保持一致的唯一通道。
- 错误路径是一条「逆序直梯」：`out_err_no_debugfs` → `out_no_coalesced_mmio` →
  `out_err_no_mmu_notifier` → `out_err_no_disable` → `out_err_no_arch_destroy_vm`
  → `out_err_no_irq_routing` → `out_err_no_irq_srcu` → `out_err_no_srcu`。
  每一级只回退自己成功过的那一步，因此新增初始化步骤时必须同时插入新标签，
  否则会出现「部分初始化」的 VM 结构泄漏。

VM 创建成功后，`kvm_dev_ioctl_create_vm()` 会用
`anon_inode_getfile("kvm-vm", &kvm_vm_fops, kvm, O_RDWR)` 把 `kvm` 指针塞进
一个匿名 inode 的 `private_data`，再把 fd 返回用户态。此后所有 VM 级 ioctl
都是 `filp->private_data` 上的操作，不再需要全局查表。

## 14. vCPU 创建：一个刻意留出的「半可见窗口」

`kvm_vm_ioctl_create_vcpu()`（`virt/kvm/kvm_main.c` 约 4152 行）比 VM 创建更微妙，
因为它必须同时满足两个互相冲突的要求：vCPU 的创建是**重量级**的（要建 VGIC CPU 接口、
定时器、PMU、浮点状态、可能的 stage-2 影子页表），但 `kvm->lock` 是全局互斥的，
长时间持锁会阻塞同 VM 其它 vCPU 的 ioctl。源码的解决办法是「先占坑、放锁、再补票」：

```
mutex_lock(&kvm->lock);
    if (kvm->created_vcpus >= kvm->max_vcpus) -> -EINVAL
    kvm_arch_vcpu_precreate(kvm, id)         /* ARM64: 校验 vcpu_features 未被冻结 */
    kvm->created_vcpus++                     /* 占坑：容量已计入 */
mutex_unlock(&kvm->lock);

vcpu = kmem_cache_zalloc(kvm_vcpu_cache, GFP_KERNEL_ACCOUNT);
page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);   /* vcpu->run 共享页 */
kvm_vcpu_init(vcpu, kvm, id);
kvm_arch_vcpu_create(vcpu);                  /* 真正建 vgic_cpu/timer/pmu/fp */
kvm_dirty_ring_alloc(...)                    /* 仅当 kvm->dirty_ring_size != 0 */

mutex_lock(&kvm->lock);
    if (kvm_get_vcpu_by_id(kvm, id)) -> -EEXIST
    vcpu->vcpu_idx = atomic_read(&kvm->online_vcpus);
    xa_insert(&kvm->vcpu_array, vcpu->vcpu_idx, vcpu, GFP_KERNEL_ACCOUNT);
    mutex_lock(&vcpu->mutex);                 /* 见下方注释 */
    kvm_get_kvm(kvm);
    create_vcpu_fd(vcpu);                     /* anon_inode: kvm-vcpu */
    smp_wmb();                                /* 配对 kvm_get_vcpu() 的 smp_rmb() */
    atomic_inc(&kvm->online_vcpus);
    mutex_unlock(&vcpu->mutex);
mutex_unlock(&kvm->lock);
kvm_arch_vcpu_postcreate(vcpu);
kvm_create_vcpu_debugfs(vcpu);
```

这个「窗口」里有三处细节值得逐步解释：

1. **`created_vcpus` 与 `online_vcpus` 的差值就是窗口。** `created_vcpus` 在放锁前
   自增，`online_vcpus` 在最后才自增，两者不相等意味着「有 vCPU 正在创建但用户态
   还看不见」。这个差值被两处代码当作状态位使用：`kvm_vgic_create()` 要求
   `kvm->created_vcpus == atomic_read(&kvm->online_vcpus)` 才允许创建 VGIC
   （否则 `-EBUSY`），从而避免「VGIC 已经按 V2 建好，却冒出一个需要 V3 的 vCPU」；
   `kvm_trylock_all_vcpus()` 也只对已上线 vCPU 加锁，正因为创建中的 vCPU 还不可达。
2. **为什么要在 `xa_insert()` 之后立刻 `mutex_lock(&vcpu->mutex)`。** 源码注释解释了
   两层原因：其一是让用户态在 vCPU 完全可见（`online_vcpus` 已增加）之前无法
   对 vCPU fd 发起 ioctl，防止 KVM 自己认为「当前 vCPU 不存在」而空指针解引用；
   其二是给 lockdep 一个明确的嵌套关系——`vcpu->mutex` 是在 `kvm->lock` 之内取得的。
3. **`smp_wmb()` 配对的读者是 `kvm_get_vcpu()`。** 顺序必须是「先让 `vcpu_array`
   里的指针可见，再让 `online_vcpus` 变大」；读侧用 `smp_rmb()` 保证「先看到计数
   再读数组」时不会读到空槽。这是一个典型的「发布-订阅」式无锁读路径，
   `kvm_for_each_vcpu()` 就是它的消费者。

失败路径同样值得一读：`vcpu_decrement` 会把 `created_vcpus--` 补回来，其余
`arch_vcpu_destroy` → `vcpu_free_run_page` → `vcpu_free` 逐级释放。注意
`kvm_put_xa_erase` 分支里先 `kvm_put_kvm_no_destroy(kvm)` 再 `xa_erase()`，
因为 `create_vcpu_fd()` 失败意味着「VM 引用已加但 fd 未建」，必须成对回退。

vCPU fd 的 ioctl 入口 `kvm_vcpu_ioctl()`（约 4406 行）在分发前有两道闸门：

```c
if (vcpu->kvm->mm != current->mm || vcpu->kvm->vm_dead)
    return -EIO;
if (unlikely(_IOC_TYPE(ioctl) != KVMIO))
    return -EINVAL;
r = kvm_wait_for_vcpu_online(vcpu);      /* 等 online_vcpus 覆盖到自己 */
if (r) return r;
r = kvm_arch_vcpu_unlocked_ioctl(filp, ioctl, arg);
if (r != -ENOIOCTLCMD) return r;
if (mutex_lock_killable(&vcpu->mutex)) return -EINTR;
```

第一道闸门是「跨进程使用 vCPU fd」的防护：`kvm->mm` 绑定的是创建者，
若另一个进程（不同 `mm`）拿着继承来的 fd 调 ioctl，直接 `-EIO`。
第二道闸门 `kvm_wait_for_vcpu_online()` 正是为第 14 节开头那个窗口准备的：
它等到 `online_vcpus` 覆盖本 vCPU 后才继续，用户态无法在「半初始化」状态插入操作。
`kvm_arch_vcpu_unlocked_ioctl()` 是给架构留的「不加 `vcpu->mutex` 也能处理」的逃生口
（例如某些不需要串行化的查询），其余 ioctl 一律在 `vcpu->mutex` 下执行。

`KVM_ARM_VCPU_INIT` 的语义与这套并发模型耦合很紧：`kvm_arch_vcpu_ioctl_vcpu_init()`
首次调用时把 `init.features` 写入 `kvm->arch.vcpu_features`，之后任何不一致的
特性组合都会被拒绝。因为 `kvm->arch.vcpu_features` 是**整个 VM 共享**的位图，
所以「第一个 vCPU 的 INIT 决定了全 VM 的能力集」。`kvm_vgic_create()` 又要求
所有 vCPU 都还没 `vcpu_has_run_once()`，两者合起来保证了「能力集 → VGIC 型号 →
vCPU 数量」这条依赖链不会在中途被改写。

## 15. memslot 更新协议：kvm_set_memslot()

`KVM_SET_USER_MEMORY_REGION` 的名字很容易让人以为是「改一个结构体字段」，
实际上它要在**不停止 vCPU** 的前提下替换 stage-2 映射、等待所有正在进行的
SRCU 读临界区退出，并且允许失败回滚。`kvm_set_memslot()`（约 1886 行）是这套协议的核心。

入口先按「新旧 slot 的 gfn 范围是否相同」把操作归为四类：

| `enum kvm_mr_change` | 触发条件 | 关键动作 |
| --- | --- | --- |
| `KVM_MR_CREATE` | 旧 slot 不存在 | `kvm_create_memslot()` |
| `KVM_MR_DELETE` | 新 slot 被清空 | 先 `kvm_invalidate_memslot()`，再 `kvm_delete_memslot()` |
| `KVM_MR_MOVE` | gfn 变了但 slot 仍存在 | 先 invalid，再 `kvm_move_memslot()` |
| `KVM_MR_FLAGS_ONLY` | 只有 flags 变化 | `kvm_update_flags_memslot()` |

协议分四步：

1. **取 `slots_arch_lock`。** 源码注释明确了持有窗口：从「拷贝当前 memslots 之前」
   一直到「新 memslots 通过 `rcu_assign_pointer()` 安装之后」，然后在
   `kvm_swap_active_memslots()` 里 `synchronize_srcu()` **之前**释放。
   这个顺序是为了避免「另一个线程在 SRCU 临界区里再取 `slots_arch_lock`」造成死锁。
2. **预先失效旧 slot（仅 DELETE/MOVE）。** 分配一个临时的 `invalid_slot`，
   调 `kvm_invalidate_memslot(kvm, old, invalid_slot)`。目的是在真正删除/移动之前
   就让架构代码把该 slot 的 stage-2 映射和影子页表清掉，否则「删除已生效、
   架构回调还没跑完」的窗口里，vCPU 可能访问到一个已经不存在的 memslot。
   失败时用 `kvm_activate_memslot(kvm, invalid_slot, old)` 把旧值装回去。
3. **`kvm_prepare_memory_region()`。** 内部调用
   `kvm_arch_prepare_memory_region(kvm, old, new, change)`，让架构做「这次变更是否
   允许」的判定（ARM64 上包括检查是否会与现有 stage-2 冲突、是否要求
   `KVM_MEM_READONLY` 等），此时还没有任何可见状态被修改。
4. **提交。** 按四类分别调用 create/delete/move/update_flags，然后
   `kvm_commit_memory_region()` 通知架构「变更已生效」。最后
   `kvm_swap_active_memslots()` 完成指针切换。

`kvm_swap_active_memslots()`（约 1600 行）里有一段容易被漏读的等待逻辑：

```c
spin_lock(&kvm->mn_invalidate_lock);
prepare_to_rcuwait(&kvm->mn_memslots_update_rcuwait);
while (kvm->mn_active_invalidate_count) {
    set_current_state(TASK_UNINTERRUPTIBLE);
    spin_unlock(&kvm->mn_invalidate_lock);
    schedule();
    spin_lock(&kvm->mn_invalidate_lock);
}
finish_rcuwait(&kvm->mn_memslots_update_rcuwait);
rcu_assign_pointer(kvm->memslots[as_id], slots);
spin_unlock(&kvm->mn_invalidate_lock);
```

`mn_active_invalidate_count` 统计「正在进行中的 MMU notifier invalidate_range 回调」。
如果允许在 notifier 跑到一半时切换 memslots，`invalidate_range_start/end` 的配对
就会被打破（start 用的是旧 memslots，end 已经是新的），所以这里必须睡等。
这也是 `struct kvm` 里同时存在 `mn_invalidate_lock` 与 `mn_memslots_update_rcuwait`
的原因。

generation 的编码同样精巧。切换时先把 inactive 数组的 generation 置为
`gen | KVM_MEMSLOT_GEN_UPDATE_IN_PROGRESS`，安装指针并 `synchronize_srcu_expedited()`
之后，再去掉「更新中」位并加上 `kvm_arch_nr_memslot_as_ids(kvm)`：

```c
gen = slots->generation & ~KVM_MEMSLOT_GEN_UPDATE_IN_PROGRESS;
gen += kvm_arch_nr_memslot_as_ids(kvm);
kvm_arch_memslots_updated(kvm, gen);
slots->generation = gen;
```

源码注释给出了完整的理由：generation 空间按地址空间**均分**，
两个地址空间时 as_id=0 用 0、2、4…，as_id=1 用 1、3、5…。
这样「generation 唯一」不需要全局计数器，而读侧
（`gfn_to_memslot()` 的 `ghc->generation` 比较，约 3429/3467 行）只要发现
generation 变了，就知道自己缓存的 memslot 指针已经过期，需要重查。
`KVM_MEMSLOT_GEN_UPDATE_IN_PROGRESS` 位则让「数组正在被改写」这个中间状态
也能被读侧识别出来——这正是第 3.1 节提到的「双份 + generation」方案的完整实现。

## 16. KVM_RUN 循环的精确时序

第 4.4 节给出了 `kvm_arch_vcpu_ioctl_run()` 的骨架。这里按实际代码
（`arch/arm64/kvm/arm.c` 约 1264 行）补上时序细节，并解释几处「顺序不能动」的地方。

入口先处理「上一次 MMIO 的尾巴」：

```c
if (run->exit_reason == KVM_EXIT_MMIO) {
    ret = kvm_handle_mmio_return(vcpu);
    if (ret <= 0)
        return ret;
}
vcpu_load(vcpu);
if (!vcpu->wants_to_run) { ret = -EINTR; goto out; }
kvm_sigset_activate(vcpu);
```

`vcpu->wants_to_run` 由 `kvm_vcpu_ioctl()` 在调进来之前设置：
`!READ_ONCE(vcpu->run->immediate_exit__unsafe)`。这个字段存在的意义是让
「另一个线程已经请求立即退出」时，本线程连 `vcpu_load()` 之后的准备工作都省掉。
`kvm_sigset_activate()` 则把信号处理推迟到退出 guest 之后，
避免在 EL2 上下文里跑用户态信号处理函数。

循环体可以拆成「进入前」「进入」「返回后」三段：

| 段 | 代码 | 说明 |
| --- | --- | --- |
| 进入前 | `kvm_xfer_to_guest_mode_handle_work(vcpu)` | 处理 TIF 置位的待办工作（如 `TIF_NOTIFY_SIGNAL`） |
| 进入前 | `check_vcpu_requests(vcpu)` | 消费 `KVM_REQ_*` 位图，见下表 |
| 进入前 | `preempt_disable()` | 之后到 `preempt_enable()` 之间不可抢占 |
| 进入前 | `kvm_nested_flush_hwstate()` / `kvm_pmu_flush_hwstate()` | NV 与 PMU 的硬件状态落地 |
| 进入前 | `local_irq_disable()` | 关中断，为 `kvm_vgic_flush_hwstate()` 做准备 |
| 进入前 | `kvm_vgic_flush_hwstate(vcpu)` | 把 `ap_list` 上的 pending 中断写进 LR / `ICH_LR*_EL2` |
| 进入前 | `smp_store_mb(vcpu->mode, IN_GUEST_MODE)` | 与 `kvm_vcpu_kick()` 的读侧配对 |
| 进入前 | `kvm_vcpu_exit_request(vcpu, &ret)` | 最后一次「要不要真的进 guest」的判定 |
| 进入 | `kvm_arch_vcpu_ctxflush_fp()` → `kvm_arm_vcpu_enter_exit()` | 保存浮点、`kvm_call_hyp_ret(__kvm_vcpu_run, vcpu)` |
| 返回后 | `vcpu->mode = OUTSIDE_GUEST_MODE`；`vcpu->stat.exits++` | 先摘状态再统计 |
| 返回后 | `kvm_pmu_sync_hwstate()` → `kvm_vgic_sync_hwstate()` | PMU 必须先于 VGIC，见下 |
| 返回后 | `kvm_arch_vcpu_ctxsync_fp()` | 回收 FP 状态 |
| 返回后 | `handle_exit_early(vcpu, ret)` | 必须在 `preempt_enable()` 之前处理 |
| 返回后 | `kvm_nested_sync_hwstate()` / `preempt_enable()` | 恢复可抢占 |
| 返回后 | `handle_exit(vcpu, ret)` | 按 ESR 分发，返回值 > 0 继续循环 |

`check_vcpu_requests()`（约 1121 行）消费的请求及语义：

| 请求 | 处理 |
| --- | --- |
| `KVM_REQ_VM_DEAD` | 直接返回 `-EIO`，不再进 guest |
| `KVM_REQ_SLEEP` | `kvm_vcpu_sleep()`，让出 CPU 直到被 kick |
| `KVM_REQ_VCPU_RESET` | `kvm_reset_vcpu()`，重新加载复位状态 |
| `KVM_REQ_IRQ_PENDING` | 只清除，作用是保证 vCPU 至少走一次退出检查 |
| `KVM_REQ_VGIC_PROCESS_UPDATE` | `kvm_vgic_process_async_update()`，处理 DIR trap 延迟的 deactivate |
| `KVM_REQ_RECORD_STEAL` | `kvm_update_stolen_time()`，pvtime 抢占时间记账 |
| `KVM_REQ_RELOAD_GICv4` | `vgic_v4_put()`/`vgic_v4_load()`，distributor 使能位变化后重配 |
| `KVM_REQ_RELOAD_PMU` / `KVM_REQ_RESYNC_PMU_EL0` | 重载 PMU 与 `PMUSERENR_EL0` |
| `KVM_REQ_SUSPEND` | `kvm_vcpu_suspend()` |
| dirty ring 请求 | `kvm_dirty_ring_check_request()`，需要时先返回用户态 |
| 嵌套相关 | `check_nested_vcpu_requests()` |

三处顺序约束的源码依据：

1. **PMU 同步必须在 VGIC 之前。** 注释写的是
   `We must sync the PMU state before the vgic state so that the vgic can properly
   sample the updated state of the interrupt line`。VGIC 的 `sync_hwstate()` 要读
   中断线电平（level-triggered 的 PPI 依赖它），而 PMU 溢出会改变该电平。
2. **`handle_exit_early()` 必须在 `preempt_enable()` 之前。** 它处理的是
   「一旦可抢占就会出错」的退出类型（例如需要立即读取的硬件状态），
   所以被夹在 `local_irq_enable()` 之后、`preempt_enable()` 之前。
3. **`smp_store_mb(vcpu->mode, IN_GUEST_MODE)` 必须在最后一次请求检查之前。**
   这是 `kvm_vcpu_exiting_guest_mode()` 与 `kvm_vcpu_kick()` 的经典握手：
   kick 侧若看到 `IN_GUEST_MODE` 就发 IPI；vCPU 侧在设完 mode 之后再检查请求，
   保证「要么我看到请求，要么 kick 看到 mode 并打断我」，不会双输。

`kvm_vcpu_exit_request()`（约 1206 行）区分三类「不进 guest」：

```c
if (unlikely(!irqchip_in_kernel(vcpu->kvm))) {
    if (unlikely(kvm_irq_update_run(vcpu))) {
        *ret = -EINTR;
        run->exit_reason = KVM_EXIT_INTR;      /* 用户态 irqchip 需要同步 timer/PMU 电平 */
        return true;
    }
}
if (unlikely(vcpu_on_unsupported_cpu(vcpu))) {
    run->exit_reason = KVM_EXIT_FAIL_ENTRY;
    run->fail_entry.hardware_entry_failure_reason = KVM_EXIT_FAIL_ENTRY_CPU_UNSUPPORTED;
    run->fail_entry.cpu = smp_processor_id();
    *ret = 0;
    return true;
}
return kvm_request_pending(vcpu) || xfer_to_guest_mode_work_pending();
```

注意第三种情况**不写 `*ret`**：它表示「回到可抢占、可中断的内核上下文处理待办，
然后重新进 guest」，而不是「返回用户态」。这个区分在注释里被特别强调，
因为误把 `ret` 写成 0 会让循环退出，把一次本该内部消化的调度点变成一次
昂贵的用户态往返。

另外，循环末尾还有一段 32 位兼容性检查：

```c
if (vcpu_mode_is_bad_32bit(vcpu)) {
    vcpu_clear_flag(vcpu, VCPU_INITIALIZED);
    ret = ARM_EXCEPTION_IL;
}
```

ARMv8 没有机制阻止 guest 自己掉到 AArch32 EL0，所以在「宿主不支持 32 位 EL0」
或「开启了 NV」的场景下，KVM 只能事后抓现行：清掉 `VCPU_INITIALIZED`，
返回 `ARM_EXCEPTION_IL`（非法异常），让 VMM 通过重新 `KVM_ARM_VCPU_INIT` 来修复。

**`CONFIG_PREEMPT_RT` 视角。** 本树开启了 `CONFIG_PREEMPT_RT`，这会改变上面若干同步
原语的语义，但 KVM 的这段代码基本不受影响，原因有三：

- 循环里用的是 `preempt_disable()`/`local_irq_disable()` 这对**原始**原语，
  在 RT 下依然是「真正禁止抢占/关中断」，不会变成可睡眠锁。
- VGIC 的 `struct vgic_irq` 用 `raw_spinlock_t irq_lock`、
  `struct vgic_cpu` 用 `raw_spinlock_t ap_list_lock`（`include/kvm/arm_vgic.h`
  234 与 508 行）。`raw_spinlock_t` 在 RT 上保持自旋语义，因此
  `kvm_vgic_flush_hwstate()` 里 `DEBUG_SPINLOCK_BUG_ON(!irqs_disabled())` 的前提成立。
- 真正会变成睡眠锁的是 `mutex` 系列（`kvm->lock`、`vcpu->mutex`、
  `kvm->slots_lock`、`kvm->slots_arch_lock`）以及可能的 `spinlock_t`
  （`kvm->mmu_lock` 在不支持 `KVM_HAVE_MMU_RWLOCK` 的架构上就是 `spinlock_t`）。
  因此 RT 下必须避免「持有这些锁时关中断」的写法；本笔记涉及的路径里，
  关中断只包裹 `kvm_vgic_flush_hwstate()` 这一段，且其间不获取 mutex。

需要强调的是：RT 不会改变 KVM 的 ABI 或退出原因，只改变「持锁期间能否被抢占」。
对 VMM 来说可观察的差异主要是延迟抖动与 `KVM_EXIT_INTR` 出现的频率。

## 17. vcpu_load()/vcpu_put()：一次世界切换的准备工作

`kvm_arch_vcpu_load()`（`arch/arm64/kvm/arm.c` 约 664 行）在每次 `KVM_RUN` 入口执行，
它承担的是「把这个 vCPU 绑定到当前物理 CPU，并把此 CPU 上的遗留状态清理干净」的职责。
理解它的关键是把「per-CPU 硬件状态」和「per-vCPU 软件状态」区分开：
同一个物理 CPU 可能先后运行同 VM 的不同 vCPU，也可能运行不同 VM 的 vCPU。

```c
void kvm_arch_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
    struct kvm_s2_mmu *mmu;
    int *last_ran;

    if (is_protected_kvm_enabled())
        goto nommu;
    if (vcpu_has_nv(vcpu))
        kvm_vcpu_load_hw_mmu(vcpu);

    mmu = vcpu->arch.hw_mmu;
    last_ran = this_cpu_ptr(mmu->last_vcpu_ran);
    kvm_arm_vmid_update(&mmu->vmid);
    if (*last_ran != vcpu->vcpu_idx) {
        kvm_call_hyp(__kvm_flush_cpu_context, mmu);
        *last_ran = vcpu->vcpu_idx;
    }
nommu:
    vcpu->cpu = cpu;
    kvm_timer_vcpu_load(vcpu);
    kvm_vgic_load(vcpu);
    kvm_vcpu_load_debug(vcpu);
    kvm_vcpu_load_fgt(vcpu);
    if (has_vhe())
        kvm_vcpu_load_vhe(vcpu);
    kvm_arch_vcpu_load_fp(vcpu);
    kvm_vcpu_pmu_restore_guest(vcpu);
    ...
}
```

分点说明：

1. **VMID 分配。** `kvm_arm_vmid_update()` 必须在写 `VTTBR_EL2` 之前完成。
   源码注释指出两个约束：一是 VHE 下 `VTTBR_EL2` 是**提前**编程的
   （`__load_stage2()` 在 `kvm_vcpu_load_vhe()` 里），所以 VMID 必须先就绪；
   二是 VMID 分配器在回绕时只保留「当时活跃」的 VMID，因此从
   `kvm_sched_in()` 这条路径进来时可能需要重新取号。
2. **`last_vcpu_ran` 与 TLB 失效。** `struct kvm_s2_mmu` 里有一份 per-CPU 的
   `last_vcpu_ran`（第 5.1 节提到的字段）。若本 CPU 上次跑的是同 VM 的**别的** vCPU，
   就需要 `__kvm_flush_cpu_context(mmu)`：因为 KVM 保证「TLB 与 I-cache 对每个 vCPU
   私有」，换 vCPU 就像换地址空间。注释还说明「可能在 vCPU 真正运行前就被抢占，
   多失效一次不影响正确性」——这是典型的「宁可多做不可少做」。
3. **`hw_mmu` 与 NV 的多 stage-2。** 非 NV 场景 `vcpu->arch.hw_mmu` 恒指向
   `kvm->arch.mmu`；开启 NV 后由 `kvm_vcpu_load_hw_mmu()` 在 canonical 与 nested
   两套 `kvm_s2_mmu` 之间切换。这也解释了 `struct kvm_arch` 里为什么要有
   `nested_mmus` 这样指向「额外 stage-2」的字段。
4. **timer 必须在 VGIC 之前 load。** 源码注释：
   `The timer must be loaded before the vgic to correctly set up physical interrupt
   deactivation in nested state (e.g. timer interrupt)`。在嵌套虚拟化下，
   物理定时器中断的 deactivate 依赖 VGIC 已经知道「这条线属于谁」，
   而这条信息由 timer 代码在 load 阶段建立。
5. **`kvm_vcpu_load_fgt()`** 装载 Fine-Grained Trap 配置。`struct kvm_arch` 里
   `fgu[__NR_FGT_GROUP_IDS__]` 记录的是「哪些 FGT 组需要 UNDEF 注入」，
   而 `struct kvm_vcpu_arch` 的 `fgt[]` 是每个 vCPU 的实际值。
6. **TWE/TWI 的按需设置。** `kvm_vcpu_should_clear_twe/twi()` 决定是否在
   `vcpu->arch.hcr_el2` 里清掉 `HCR_TWE`/`HCR_TWI`。这是「WFI/WFE 是否真的陷入 EL2」
   的开关：只有需要拦截时才 trap，否则让 guest 直接执行以省一次退出。
7. **`supported_cpus` 检查。** 若当前 CPU 不在 `kvm->arch.supported_cpus` 里，
   置 `vcpu_on_unsupported_cpu(vcpu)`，下一次 `kvm_vcpu_exit_request()` 就会以
   `KVM_EXIT_FAIL_ENTRY` + `KVM_EXIT_FAIL_ENTRY_CPU_UNSUPPORTED` 返回用户态。
   这条机制用于「big.LITTLE 或 CPU 热插拔后，某个 vCPU 被迁到不具备所需特性的核」。

`kvm_arch_vcpu_put()`（约 746 行）严格逆序：pkvm 的 APRS 保存 →
`kvm_vcpu_put_debug()` → `kvm_arch_vcpu_put_fp()` → `kvm_vcpu_put_vhe()` →
`kvm_timer_vcpu_put()` → `kvm_vgic_put()` → `kvm_vcpu_pmu_restore_host()` →
`kvm_vcpu_put_hw_mmu()`（NV）→ `kvm_arm_vmid_clear_active()` →
清 `on_unsupported_cpu` 标志 → `vcpu->cpu = -1`。注意 `kvm_vgic_put()` 在
timer 之后：put 阶段的顺序同样要保证「VGIC 最后失去上下文」，
否则可能出现在 VGIC 已放弃、timer 仍在处理物理中断的窗口。

## 18. Stage-2 缺页的完整决策树

第 5.3 节列出了 `kvm_handle_guest_abort()` 的步骤。这里补上实际代码中的判定顺序
（`arch/arm64/kvm/mmu.c` 约 2247 行），因为「先判断什么」直接决定错误被注入给谁。

```c
if (kvm_vcpu_abt_issea(vcpu))
    return kvm_handle_guest_sea(vcpu);

esr = kvm_vcpu_get_esr(vcpu);
ipa = fault_ipa = kvm_vcpu_get_fault_ipa(vcpu);
if (KVM_BUG_ON(ipa == INVALID_GPA, vcpu->kvm))
    return -EFAULT;
is_iabt = kvm_vcpu_trap_is_iabt(vcpu);

if (esr_fsc_is_translation_fault(esr)) {
    if (fault_ipa >= BIT_ULL(get_kvm_ipa_limit())) {      /* 超出硬件 IPA 上限 */
        kvm_inject_size_fault(vcpu);
        return 1;
    }
    if (fault_ipa >= BIT_ULL(VTCR_EL2_IPA(vcpu->arch.hw_mmu->vtcr))) {
        fault_ipa |= FAR_TO_FIPA_OFFSET(kvm_vcpu_get_hfar(vcpu));
        return kvm_inject_sea(vcpu, is_iabt, fault_ipa);
    }
}
```

这里的三个阈值必须分清：

| 阈值 | 来源 | 超出后的语义 |
| --- | --- | --- |
| `get_kvm_ipa_limit()` | 宿主 sanitised PARange | 硬件根本翻译不了这么大的 IPA，注入 size fault |
| `VTCR_EL2_IPA(vtcr)` | guest 可见 IPA 宽度 | 落在「客可见 IPA」与「物理 PARange」之间，属于外部中止语义，注入 SEA |
| `memslot` 覆盖范围 | 用户态注册的 memory region | 未注册 → MMIO 模拟或 `-ENOEXEC` |

接着是 FSC 过滤：只接受 translation / permission / access-flag / excl-atomic 四类，
否则打印 `Unsupported FSC` 并返回 `-EFAULT`。这一步的用意是「不认识的异常码不要猜」，
因为错误的翻译会产生比直接报错更危险的行为。

随后进入 `srcu_read_lock(&vcpu->kvm->srcu)` 保护的临界区。嵌套场景下的处理顺序是：
先用 `kvm_walk_nested_s2()` 把 L2 IPA 翻译成 L1 IPA，再走 canonical 翻译。
三个分支值得注意：

- `ret == -EAGAIN`：影子页表还没准备好，返回 1 让 guest 重试（不是错误）。
- `kvm_s2_trans_esr()` + `kvm_inject_s2_fault()`：影子页表自己缺页，
  把故障注入给 **L1 guest**，由 L1 hypervisor 决定怎么补。
- `kvm_s2_handle_perm_fault()`：影子页表权限不足，同样注入给 L1。

canonical 路径的核心是 `gfn_to_memslot()` + `gfn_to_hva_memslot_prot()`，
之后的判定非常关键：

```c
write_fault = kvm_is_write_fault(vcpu);
if (kvm_is_error_hva(hva) || (write_fault && !writable)) {
    if (is_iabt) { ret = -ENOEXEC; goto out; }            /* 取指失败：无法模拟 */
    if (kvm_vcpu_abt_iss1tw(vcpu)) {                      /* 页表遍历中的写 */
        ret = kvm_inject_sea_dabt(vcpu, kvm_vcpu_get_hfar(vcpu));
        goto out_unlock;
    }
    ... /* 其余交给 io_mem_abort() 走 MMIO */
}
```

`-ENOEXEC` 这个返回值最终会让 `handle_exit()` 把异常重新注入 guest，
因为「guest 把指令放在了不可执行的地方」是 guest 自己的 bug，宿主无从模拟。

真正映射页面的函数是 `user_mem_abort()`（约 2104 行），它在 `mmu_lock` 读锁下
做三件独立的事：

```c
memcache = get_mmu_memcache(s2fd->vcpu);
if (!perm_fault || (memslot_is_logging(s2fd->memslot) && kvm_is_write_fault(s2fd->vcpu))) {
    ret = topup_mmu_memcache(s2fd->vcpu, memcache);       /* 可能需要新页表页 */
    if (ret) return ret;
}
ret = kvm_s2_fault_pin_pfn(s2fd, &s2vi);                  /* 固定 pfn / 取 hugepage */
if (ret != 1) return ret;
ret = kvm_s2_fault_compute_prot(s2fd, &s2vi, &prot);      /* 计算 KVM_PGTABLE_PROT_* */
if (ret) { kvm_release_page_unused(s2vi.page); return ret; }
return kvm_s2_fault_map(s2fd, &s2vi, prot, memcache);
```

三点说明：

- **只有「非权限故障」或「dirty logging 下的写故障」才需要 topup memcache。**
  纯权限故障（比如 write→read 的降级）只需改现有 leaf entry，不需要新页表页。
  但 dirty logging 开启时，写故障可能需要把 block entry 拆成 table，
  这时就要新页表页——注释把这条例外写得很清楚。
- **`kvm_s2_fault_pin_pfn()` 的返回值 `1` 是「成功且只处理了一页」的约定。**
  返回 0 表示「已经处理完，不需要再 map」（例如 hugepage 已经覆盖），
  负值才是错误。这种「0/1/负」的三态返回值在 KVM 缺页路径里很常见。
- **`kvm_s2_fault_map()` 内部才会调 `KVM_PGT_FN(kvm_pgtable_stage2_map)`。**
  包一层 `KVM_PGT_FN()` 是为了 pKVM：在受保护模式下要改成
  `pkvm_pgtable_stage2_map()`，走 EL2 侧的页表。当前配置未启用 pKVM，
  宏展开为普通函数。

`handle_access_fault()`（约 2144 行）是另一条捷径：hardware access flag 故障
不需要分配任何东西，只需把对应 leaf 标 young。

```c
read_lock(&vcpu->kvm->mmu_lock);
mmu = vcpu->arch.hw_mmu;
KVM_PGT_FN(kvm_pgtable_stage2_mkyoung)(mmu->pgt, fault_ipa, flags);
read_unlock(&vcpu->kvm->mmu_lock);
```

注意它读的是 `vcpu->arch.hw_mmu` 而不是 `kvm->arch.mmu`：NV 场景下故障发生在
影子页表上，要标 young 的也是影子页表。

最后是 SEA 的归属判定 `host_owns_sea()`（约 2161 行）。这里有一个反直觉的点：
**没有 FEAT_RAS 时，函数直接返回 true**。原因是此时 `HCR_EL2.TEA` 是 RES0，
「从 guest EL 陷入 EL2 的外部中止」只可能来自宿主自己发起的访问（例如
stage-2 页表遍历），所以必须由宿主处理而不能注入给 guest。有 FEAT_RAS 时，
还要看 `ESR_ELx_VNCR` 与 `ESR_ELx_S1PTW` 才能区分「stage-1 遍历的中止」与
「stage-2 遍历的中止」。

stage-2 的建立与销毁在 `kvm_init_stage2_mmu()`（约 981 行）与
`kvm_uninit_stage2_mmu()`（约 1049 行）里：

```c
if (mmu->pgt != NULL) {
    if (kvm_is_nested_s2_mmu(kvm, mmu))
        return 0;                       /* 重复 INIT 是合法的，直接放过 */
    kvm_err("kvm_arch already initialized?\n");
    return -EINVAL;
}
err = kvm_init_ipa_range(mmu, type);
pgt = kzalloc_obj(*pgt, GFP_KERNEL_ACCOUNT);
mmu->arch = &kvm->arch;
err = KVM_PGT_FN(kvm_pgtable_stage2_init)(pgt, mmu, &kvm_s2_mm_ops);
mmu->pgt = pgt;
if (is_protected_kvm_enabled())
    return 0;                            /* pKVM 路径不需要 last_vcpu_ran */
mmu->last_vcpu_ran = alloc_percpu(typeof(*mmu->last_vcpu_ran));
for_each_possible_cpu(cpu) *per_cpu_ptr(mmu->last_vcpu_ran, cpu) = -1;
mmu->split_page_chunk_size = KVM_ARM_EAGER_SPLIT_CHUNK_SIZE_DEFAULT;
mmu->split_page_cache.gfp_zero = __GFP_ZERO;
mmu->pgd_phys = __pa(pgt->pgd);
if (kvm_is_nested_s2_mmu(kvm, mmu))
    kvm_init_nested_s2_mmu(mmu);
```

两个细节：其一，`mmu->pgt != NULL` 时若这是 nested MMU 则**直接返回成功**，
因为用户态对同一个 vCPU 重复调 `KVM_ARM_VCPU_INIT` 是合法的，
不应该因此报错；只有 canonical MMU 重复初始化才是内核 bug。
其二，`split_page_chunk_size` 初始化为
`KVM_ARM_EAGER_SPLIT_CHUNK_SIZE_DEFAULT`，注释写的是
`The eager page splitting is disabled by default`——也就是说默认值表示「不预先拆分
block mapping」，只有 dirty logging 真正开启时才按 chunk 拆分。
`mmu->split_page_cache` 是拆页时用的 memcache，`gfp_zero = __GFP_ZERO` 保证
新页表页被清零。

## 19. VGICv3 深入：从 gic_kvm_info 到 LR 注入

### 19.1 宿主 GIC 驱动交给 KVM 的东西

`include/linux/irqchip/arm-vgic-info.h` 定义了这份「交接单」：

```c
struct gic_kvm_info {
    enum gic_type   type;             /* GIC_V2 / GIC_V3 / GIC_V5 */
    struct resource vcpu;             /* 虚拟 CPU 接口 */
    void __iomem   *gicc_base;        /* GICv2 GICC 的 VA */
    unsigned int    maint_irq;        /* 维护中断号 */
    bool            no_maint_irq_mask;/* 无中断屏蔽位时无需 maint_irq */
    struct resource vctrl;            /* 虚拟控制接口（GICv4 用） */
    bool            has_v4;           /* 支持 VLPI */
    bool            has_v4_1;         /* 支持 RVPEID */
    bool            no_hw_deactivation;/* 非架构实现，deactivate 受损 */
};
```

`drivers/irqchip/irq-gic-v3.c` 在初始化时填充 `gic_v3_kvm_info` 并调用
`vgic_set_kvm_info(&gic_v3_kvm_info)`（本树 2175、2534 行两处，分别对应
不同的 probe 路径）。注意 `vgic_set_kvm_info()` 在 `CONFIG_KVM=n` 时是空函数
（`arm-vgic-info.h` 第 46 行），所以 GIC 驱动不需要为 KVM 加条件编译。

KVM 侧的消费者是 `kvm_vgic_hyp_init()`（`arch/arm64/kvm/vgic/vgic-init.c` 约 778 行）：

```c
if (!gic_kvm_info) return -ENODEV;
has_mask = !gic_kvm_info->no_maint_irq_mask;
if (has_mask && !gic_kvm_info->maint_irq) {
    kvm_err("No vgic maintenance irq\n");
    return -ENXIO;
}
if (gic_kvm_info->no_hw_deactivation) {
    kvm_info("Non-architectural vgic, tainting kernel\n");
    add_taint(TAINT_CPU_OUT_OF_SPEC, LOCKDEP_STILL_OK);
    kvm_vgic_global_state.no_hw_deactivation = true;
}
switch (gic_kvm_info->type) {
case GIC_V2: ret = vgic_v2_probe(gic_kvm_info); break;
case GIC_V3: ret = vgic_v3_probe(gic_kvm_info);
             if (!ret) {
                 static_branch_enable(&kvm_vgic_global_state.gicv3_cpuif);
                 kvm_info("GIC system register CPU interface enabled\n");
             }
             break;
case GIC_V5: ret = vgic_v5_probe(gic_kvm_info); break;
default:     ret = -ENODEV;
}
kvm_vgic_global_state.maint_irq = gic_kvm_info->maint_irq;
kfree(gic_kvm_info);
gic_kvm_info = NULL;
```

注意最后 `kfree(gic_kvm_info)` 并置空：`gic_kvm_info` 是 `__initdata` 指向的
静态结构，KVM 只在初始化阶段读一次，读完立刻丢掉指针，防止后续误用。
`static_branch_enable(&kvm_vgic_global_state.gicv3_cpuif)` 是一个静态键：
一旦确认宿主是 GICv3，后续所有「用系统寄存器还是 MMIO 访问 CPU 接口」的判断
都变成一次 NOP 补丁，不再有分支开销。这就是第 7.1 节所说
`vgic_lazy_init()`/`vgic_init()` 能被快速路径跳过的原因。

### 19.2 kvm_vgic_create() 的三重加锁

`kvm_vgic_create()`（约 71 行）是 `KVM_CREATE_DEVICE(KVM_DEV_TYPE_ARM_VGIC_V3)`
的落点。它在并发上做了三层保护，注释逐条解释了为什么缺一不可：

| 锁 | 作用 | 单靠它不够的原因 |
| --- | --- | --- |
| `kvm->lock`（调用者已持） | 阻止 `KVM_CREATE_VCPU` 走到 `kvm_arch_vcpu_precreate()` | 创建 vCPU 时会在中途释放它 |
| 所有在线 vCPU 的 `vcpu->mutex` | 阻止已可见 vCPU 的并发 ioctl | 对「创建中」的 vCPU 不可见 |
| `kvm->arch.config_lock` | 保护 per-vCPU 私有中断数组（SGI/PPI） | 需要前三者共同保证 |

在这三层之上，还有一个「计数一致性」检查：

```c
if (kvm->created_vcpus != atomic_read(&kvm->online_vcpus))
    goto out_unlock;
if (irqchip_in_kernel(kvm)) { ret = -EEXIST; goto out_unlock; }
kvm_for_each_vcpu(i, vcpu, kvm)
    if (vcpu_has_run_once(vcpu))
        goto out_unlock;
```

第一条正是第 14 节那个「半可见窗口」的用途：只要有 vCPU 正在创建，就不允许
创建 VGIC，否则新 vCPU 可能拿到与 VM 不一致的 VGIC 配置。第二条拒绝重复创建。
第三条要求所有 vCPU 都还没跑过——一旦跑过，`ICH_*` 与 redistributor 状态已经
建立，无法再改 GIC 型号。

随后按型号收紧 `max_vcpus`：

```c
if (type == KVM_DEV_TYPE_ARM_VGIC_V2) kvm->max_vcpus = VGIC_V2_MAX_CPUS;
else if (type == KVM_DEV_TYPE_ARM_VGIC_V3) kvm->max_vcpus = VGIC_V3_MAX_CPUS;
else if (type == KVM_DEV_TYPE_ARM_VGIC_V5)
    kvm->max_vcpus = min(VGIC_V5_MAX_CPUS, kvm_vgic_global_state.max_gic_vcpus);
if (atomic_read(&kvm->online_vcpus) > kvm->max_vcpus) {
    ret = -E2BIG;
    goto out_unlock;
}
```

V5 之所以要取 `min()`，是因为 GICv5 的 vCPU 上限还受宿主实际实现的
`max_gic_vcpus` 限制；前两者是架构规定的常量。

### 19.3 资源映射与 flush/sync

`kvm_vgic_map_resources()` 的 V3 实现在 `vgic_v3_map_resources()`
（`arch/arm64/kvm/vgic/vgic-v3.c` 约 752 行），它的检查顺序是：

```c
kvm_for_each_vcpu(c, vcpu, kvm) {
    if (IS_VGIC_ADDR_UNDEF(vgic_cpu->rd_iodev.base_addr)) {
        kvm_debug("vcpu %ld redistributor base not set\n", c);
        return -ENXIO;
    }
}
if (IS_VGIC_ADDR_UNDEF(dist->vgic_dist_base)) {
    kvm_debug("Need to set vgic distributor addresses first\n");
    return -ENXIO;
}
if (!vgic_v3_check_base(kvm)) {
    kvm_debug("VGIC redist and dist frames overlap\n");
    return -EINVAL;
}
if (!vgic_initialized(kvm))
    return -EBUSY;
if (kvm_vgic_global_state.has_gicv4_1)
    vgic_v4_configure_vsgis(kvm);
```

这段代码把「用户态必须先做什么」写成了错误信息：每个 vCPU 的 redistributor 基址
（`KVM_DEV_ARM_VGIC_GRP_ADDR` + `KVM_VGIC_V3_ADDR_TYPE_REDIST`）必须先设，
distributor 基址必须后设且不能重叠，VGIC 必须已经 `vgic_init()`。
`has_gicv4_1` 时还要额外配置 vSGI，因为 GICv4.1 允许虚拟 SGI 直接注入。

运行期的 `kvm_vgic_flush_hwstate()`（`vgic.c` 约 1128 行）与
`kvm_vgic_sync_hwstate()`（约 1064 行）是一对。flush 的关键前置是
`DEBUG_SPINLOCK_BUG_ON(!irqs_disabled())`：它必须在 `local_irq_disable()` 之后调用，
否则「读 `ap_list` → 写 LR」之间可能被中断打断，导致同一个中断被注入两次。
flush 的三种形态：

| 场景 | 路径 |
| --- | --- |
| 嵌套 guest（`vgic_state_is_nested()`） | 有 pending 则 `kvm_make_request(KVM_REQ_GUEST_HYP_IRQ_PENDING)`，再 `vgic_v3_flush_nested()` |
| GICv5 | `vgic_v5_flush_ppi_state()` |
| 其他（V2/V3/V4） | `scoped_guard(raw_spinlock, &ap_list_lock)` 下 `vgic_flush_lr_state()` |

flush 之后还有两步：`can_access_vgic_from_kernel()` 为真时 `vgic_restore_state()`，
以及 GICv4 且支持 direct IRQ 时 `vgic_v4_commit()`。
`vgic_restore_state()` 内部按「V5 / 非 gicv3_cpuif（即 V2 接口）/ V3」三态分派，
注释解释了为什么不做成 switch：`__vgic_v3_restore_state()` 要同时覆盖
「GICv3-on-GICv3」「GICv2-on-GICv3」「GICv3-on-GICv5」以及
「GICv3 硬件但无内核 irqchip」四种组合。

sync 方向的顺序是 `vgic_save_state()` → `vgic_fold_state()` →
`vgic_prune_ap_list()`（V5 跳过 prune）。`vgic_fold_state()` 把 LR 中
「已经不再 active」的条目合并回 `vgic_irq` 的软件状态；
`vgic_prune_ap_list()` 则清理 `ap_list` 上已完成的条目。
如果宿主是 GICv4，VLPI 可能由硬件直接投递，`vgic_v4_commit()` 在 flush 侧
完成的配置要在 sync 侧被确认，这也是 V4 路径额外复杂的原因。

### 19.4 注入语义：level 与 edge

`kvm_vgic_inject_irq()`（约 530 行）的语义差异只有一行，但影响很大：

```c
if (irq->config == VGIC_CONFIG_LEVEL)
    irq->line_level = level;
else
    irq->pending_latch = true;
```

- level-triggered：记录**电平**。同一个中断线保持高电平时反复注入不会累积，
  只有电平从高变低（`level == false`）才会清除。这对应真实硬件的行为。
- edge-triggered：置 **latch**。即使连续两次注入，也只算「至少有一个 pending」，
  不会排队成两个。这是 GIC 对 SGI/PPI 的标准语义。

注入前的 `vgic_validate_injection(irq, level, owner)` 会拒绝「同一个 owner 重复
拉高同一条 level 线」的情况，避免 irqfd 侧的事件重复计数。注入后
`vgic_queue_irq_unlock()` 负责把中断挂到目标 vCPU 的 `ap_list`，
并在必要时 `kvm_vcpu_kick()`。

与宿主中断子系统的衔接点在 `kvm_vgic_map_irq()`（约 590 行）：
它通过 `irq_to_desc(host_irq)` 找到宿主 `irq_desc`，再沿 `parent_data` 链走到
最底层 `irq_data`，取 `data->hwirq` 作为 `irq->hwintid`。这条「走父链」的循环
是为了兼容「GIC 作为二级控制器挂在其它 irqchip 之下」的拓扑。
之后若 `irq->ops->set_direct_injection` 存在（GICv4 场景），就把这条虚拟中断
标记为可直接投递。

GVIC 与宿主 GIC 驱动的完整关系（`irq-gic-v3.c` → `vgic_set_kvm_info()` →
`kvm_vgic_hyp_init()` → `vgic_v3_probe()` → `kvm_vgic_create()`）在
[ARM64 中断子系统分析](../01-architecture/interrupt/arm64_interrupt_subsystem_analysis.md)
中有宿主侧的展开，两篇配合阅读可以看清「物理 GIC 如何被切成 guest 可见的
distributor + redistributor」这条主线。

## 20. 系统寄存器陷入：sys_regs.c 的三张表

EL2 并不拦截所有系统寄存器访问，只有被 `HCR_EL2` 或 FGT 打开 trap 的才会陷入。
一旦陷入，`handle_exit()` 会把它交给 `kvm_handle_sys_reg()`
（`arch/arm64/kvm/sys_regs.c` 约 5295 行）。这条路径的结构可以概括为
「先查表、再模拟、最后回写」：

```c
int kvm_handle_sys_reg(struct kvm_vcpu *vcpu)
{
    const struct sys_reg_desc *desc = NULL;
    struct sys_reg_params params;
    unsigned long esr = kvm_vcpu_get_esr(vcpu);
    int Rt = kvm_vcpu_sys_get_rt(vcpu);
    int sr_idx;

    if (triage_sysreg_trap(vcpu, &sr_idx))
        return 1;                       /* 已被 triage 内部消化（例如直接注入 UNDEF） */

    params = esr_sys64_to_params(esr);
    params.regval = vcpu_get_reg(vcpu, Rt);

    /* System registers have Op0=={2,3}, as per DDI487 J.a C5.1.2 */
    if (params.Op0 == 2 || params.Op0 == 3)
        desc = &sys_reg_descs[sr_idx];
    else
        desc = &sys_insn_descs[sr_idx];

    perform_access(vcpu, &params, desc);

    if (!params.is_write && (params.Op0 == 2 || params.Op0 == 3))
        vcpu_set_reg(vcpu, Rt, params.regval);
    return 1;
}
```

要点：

1. **`esr_sys64_to_params()` 把 ESR 拆成 Op0/Op1/CRn/CRm/Op2 五元组**，
   同时设置 `is_write`、`regval` 等字段。`params.regval` 先装入 `Rt` 的当前值，
   这样「写」操作可以直接从 `params.regval` 取值，「读」操作则在模拟后把它回写。
2. **`Op0 == 2 || Op0 == 3` 是「系统寄存器 vs 系统指令」的分水岭。**
   Op0 为 2/3 表示 MRS/MSR 访问系统寄存器；其它编码属于系统指令
   （如 `AT`、`DC`、`IC`），走 `sys_insn_descs[]`。两张表的元素类型相同
   （`struct sys_reg_desc`），但 access 回调的语义不同。
3. **`triage_sysreg_trap()` 是「快速拒绝」层。** 它先做一次索引计算与合法性判断，
   对明显非法的编码直接返回 1（表示「已处理」），避免走完整的查表与模拟。
   这层存在的意义是让 guest 的探测型访问（很多 OS 会试探性地读未知寄存器）
   便宜地失败，而不是每次都让 KVM 走一遍 `perform_access()`。
4. **`perform_access()` 是真正的模拟入口。** 它按 `desc->access` 回调执行：
   有的寄存器直接读写 `vcpu->arch.ctxt.sys_regs[]`，有的做范围掩码，
   有的返回 UNDEF，有的触发副作用（例如写 `ICC_SGI1R_EL1` 会产生一个 SGI）。
5. **回写只对「读系统寄存器」发生。** 写操作的值已经由 access 回调消费，
   不需要回写 `Rt`；而对系统指令（Op0 非 2/3），`Rt` 的语义不同，
   更不能盲目回写。

`sys_reg_descs[]` 从约 3195 行开始。表项形如：

```c
{ SYS_DESC(SYS_MDCCINT_EL1), trap_debug_regs, reset_val, MDCCINT_EL1, 0 },
{ SYS_DESC(SYS_MDRAR_EL1),   trap_raz_wi },
{ SYS_DESC(SYS_OSLAR_EL1),   trap_oslar_el1 },
```

`struct sys_reg_desc` 的字段可以分成三类：

| 字段 | 作用 |
| --- | --- |
| `SYS_DESC(...)` 展开的 Op0/Op1/CRn/CRm/Op2 | 查表键 |
| `access` 回调 | 读/写时的行为 |
| `reset` 回调 + `reg` + `val` | 复位时的默认值来源 |

`trap_raz_wi`（约 766 行）是「Read-As-Zero, Write-Ignore」的标准实现：
读返回 0，写直接丢弃。很多保留或未实现的寄存器都用它，优点是行为确定、
不会因为宿主实现差异而泄漏信息。相反，`trap_debug_regs` 会把访问转发到
真正的调试寄存器状态，`trap_oslar_el1` 则模拟 OS Lock 的锁定语义。

复位路径 `kvm_reset_sys_regs()`（约 5262 行）遍历 `sys_reg_descs[]`，
对每一项调用 `reset` 回调（若有），把 `reg/val` 写进 vCPU 上下文。
这就是 `KVM_ARM_VCPU_INIT` 之后 guest 能看到「架构规定默认值」的原因——
不是硬件重置，而是软件逐项填表。

用户态侧的 `KVM_GET_ONE_REG`/`KVM_SET_ONE_REG` 走的是另一条路：
`get_reg_by_id()`（约 5360 行）先用 `index_to_params()` 把 64 位 ID 拆成五元组，
再用 `find_reg()` 在表里二分/线性查找。`index_to_params()` 会严格校验
「除已知掩码位外不允许有其它位为 1」，这是防止用户态用畸形 ID 越界访问的手段。
参数拆解的位域定义（`KVM_REG_ARM64_SYSREG_OP0_MASK` 等）与 ESR 的编码一一对应，
所以「调试器读到的 ONE_REG ID」与「guest 触发 trap 的 ESR」可以互相翻译。

## 21. VHE 世界切换：三条入口与一条返回路径

第 8.2 节给出了 `__kvm_vcpu_run_vhe()` 的骨架。这里补上三个入口函数与
它们各自负责的状态，因为「谁在什么时候写 `VTTBR_EL2`/`HCR_EL2`/`VBAR_EL1`」
是理解 VHE 的关键。

### 21.1 kvm_vcpu_load_vhe()

```c
void kvm_vcpu_load_vhe(struct kvm_vcpu *vcpu)
{
    host_data_ptr(host_ctxt)->__hyp_running_vcpu = vcpu;
    __vcpu_load_switch_sysregs(vcpu);
    __vcpu_load_activate_traps(vcpu);
    __load_stage2(vcpu->arch.hw_mmu);
}
```

三件事的顺序值得注意：

- `__hyp_running_vcpu` 是 hyp 侧「当前在跑谁」的唯一权威来源，
  `kvm_get_running_vcpu()` 与 VGIC 的维护中断处理都依赖它。
- `__vcpu_load_switch_sysregs()` 切换那些「必须在进 guest 前就生效」的系统寄存器。
- `__load_stage2(vcpu->arch.hw_mmu)` 在这里就写 `VTTBR_EL2`。这就是第 17 节
  强调「VMID 必须先就绪」的原因：VHE 下 stage-2 是提前装载的，
  而不是在 `__activate_traps()` 里才写。
- `__vcpu_load_activate_traps()` 内部用 `local_irq_save()` 包住
  `__activate_traps_common()`。源码注释解释了原因：`PMUSERENR_EL0` 的切换与
  试图更新它的 IPI 之间存在竞争，必须关中断。

### 21.2 __activate_traps() 与 __deactivate_traps()

```c
static void __activate_traps(struct kvm_vcpu *vcpu)
{
    ___activate_traps(vcpu, __compute_hcr(vcpu));   /* 写 HCR_EL2/HCRX_EL2/MDCR_EL2 */
    if (has_cntpoff()) {
        struct timer_map map;
        get_timer_map(vcpu, &map);
        if (map.direct_ptimer == vcpu_ptimer(vcpu))
            val = __vcpu_sys_reg(vcpu, CNTP_CVAL_EL0);
        if (map.direct_ptimer == vcpu_hptimer(vcpu))
            val = __vcpu_sys_reg(vcpu, CNTHP_CVAL_EL2);
        if (map.direct_ptimer) {
            write_sysreg_el0(val, SYS_CNTP_CVAL);
            isb();
        }
    }
    __activate_cptr_traps(vcpu);                    /* FPSIMD/SVE/SME 的 CPTR 配置 */
    write_sysreg(__this_cpu_read(kvm_hyp_vector), vbar_el1);
}
```

三个细节：

1. `__compute_hcr(vcpu)` 把 `vcpu->arch.hcr_el2` 与「当前是否需要额外 trap」
   合并成最终值。`HCR_EL2.TGE` 的清零是 VHE 世界切换的标志动作——
   TGE=1 时宿主在 EL2 用 EL0 的视角执行，TGE=0 后 guest 的 EL0/EL1 才生效。
   源码注释特别提到 ARM erratum 1165522：必须在清 TGE **之前**把 stage-1 与
   stage-2 都配置好，这就是 `__load_stage2()` 提前到 load 阶段的原因。
2. `has_cntpoff()` 分支处理 `CNTPOFF_EL2`。这个寄存器允许「物理计数器偏移」，
   使 guest 看到的 `CNTPCT` 与宿主不同。因为 `TGE` 清掉后 `CNTP_CVAL_EL0`
   的语义变化，必须在切换点重新装载，并加 `isb()` 保证生效。
3. `write_sysreg(__this_cpu_read(kvm_hyp_vector), vbar_el1)` 把异常向量指向
   KVM 的 hyp 向量。VHE 下 `VBAR_EL1` 同时服务 EL1 与 EL2（因为宿主就在 EL2），
   所以这一句是「guest 异常能落进 KVM」的前提。

`__deactivate_traps()` 做反向操作，其中最关键的一句是：

```c
___deactivate_traps(vcpu);
write_sysreg_hcr(HCR_HOST_VHE_FLAGS);
```

`HCR_HOST_VHE_FLAGS` 是「宿主自己运行时的 HCR 配置」，与 guest 的
`__compute_hcr()` 结果完全不同。如果不恢复它，返回宿主后第一条指令就可能
落进错误的异常级别。之后还会处理 `CNTPOFF` 的保存（把 guest 的 CVAL 存回
`vcpu->arch.ctxt`，并给物理 CVAL 加上 offset），以及
`ARM64_WORKAROUND_SPECULATIVE_AT` 要求的 `isb`。注释写明：ARM erratum
1165522 与 1530923 要求在切换到宿主的 EL2/EL0 翻译机制**之前**真正执行完上述写入。

最后是向量恢复：

```c
if (!arm64_kernel_unmapped_at_el0())
    host_vectors = __this_cpu_read(this_cpu_vector);
write_sysreg(host_vectors, vbar_el1);
```

`arm64_kernel_unmapped_at_el0()`（KPTI）为真时，宿主向量就在 `vectors` 标签处；
否则要用 per-CPU 的 `this_cpu_vector`（因为 KPTI 关闭时向量页可能被映射到
不同的地址）。这一句是「返回用户态后异常还能正确处理」的保证。

### 21.3 __kvm_vcpu_run_vhe() 的收尾顺序

```c
do {
    exit_code = __guest_enter(vcpu);
} while (fixup_guest_exit(vcpu, &exit_code));

sysreg_save_guest_state_vhe(guest_ctxt);
__deactivate_traps(vcpu);
sysreg_restore_host_state_vhe(host_ctxt);
__debug_switch_to_host(vcpu);
isb();
fpsimd_lazy_switch_to_host(vcpu);
if (guest_owns_fp_regs())
    __fpsimd_save_fpexc32(vcpu);
```

`fixup_guest_exit()` 是「原地处理」的核心：某些退出（例如 WFI 且无需调度、
或可以直接在 hyp 内完成的 sysreg 模拟）不需要回到宿主内核，直接在
`__guest_enter()` 的循环里处理完再 `eret` 回去。这避免了完整的世界切换，
是 KVM/ARM64 性能的关键优化之一。

`isb()` 的位置有明确理由（源码注释）：
`In VHE mode, CPTR traps for FPSIMD/SVE/SME also apply to EL2, so FPSIMD/SVE/SME
state must be manipulated after the ISB`。也就是说，`CPTR_EL2` 的 trap 配置在
VHE 下同样约束 EL2 自己，必须先让前面的系统寄存器写入生效，才能安全地
操作浮点状态。这个细节很容易被误认为「多余的屏障」。

`__kvm_vcpu_run()`（约 630 行）是更外层的包装：

```c
int __kvm_vcpu_run(struct kvm_vcpu *vcpu)
{
    local_daif_mask();
    pmr_sync();                    /* 让 redistributor 把 EL2 中断转发给 CPU */
    ret = __kvm_vcpu_run_vhe(vcpu);
    local_daif_restore(DAIF_PROCCTX_NOIRQ);
    return ret;
}
```

`pmr_sync()` 的必要性在注释里说得很清楚：`local_daif_mask()` 会置
`GIC_PRIO_PSR_I_SET`，如果此时 PMR（Priority Mask Register）仍屏蔽着低优先级中断，
GIC 就不会向 CPU 发信号，guest 只能靠自身异常退出，等于「中断被静默丢弃」。
`dsb` 确保 redistributor 及时把 EL2 中断转发过来。

### 21.4 nVHE：同一逻辑的另一种落地

nVHE 下没有「宿主已在 EL2」这个前提，所以同样的逻辑被拆成两半：
宿主侧通过 `kvm_call_hyp_ret()` 发 `hvc`，EL2 侧在
`arch/arm64/kvm/hyp/nvhe/switch.c` 的 `__kvm_vcpu_run()`（约 258 行）执行；
`arch/arm64/kvm/hyp/nvhe/hyp-main.c` 的 `handle___kvm_vcpu_run()`（约 217 行）
是 `hvc` 的处理入口，并通过 `HANDLE_FUNC(__kvm_vcpu_run)` 注册到 hyp 调用表
（约 723 行）。pKVM 则在 nVHE 的基础上把 EL2 代码与宿主完全隔离，
此时 `is_protected_kvm_enabled()` 为真，`kvm_call_hyp_nvhe()` 走的是
受保护的 hypercall 通道。

## 22. 调试与观测

KVM 的可观测性主要来自三处：debugfs、tracepoint、以及 `/proc` 里的统计。

### 22.1 debugfs

`kvm_init()` 调用 `kvm_init_debug()` 创建 `kvm_debugfs_dir`（通常在
`/sys/kernel/debug/kvm/`）。之后每个 VM 在 `kvm_create_vm_debugfs()` 下建一个
以 `kvm->stats_id` 命名的目录，`stats_id` 的格式是 `"kvm-%d"` +
`task_pid_nr(current)`（`kvm_create_vm()` 中 `snprintf`），因此同一进程创建的
多个 VM 会共享前缀、靠 pid 区分。每个 vCPU 再在
`kvm_create_vcpu_debugfs()` 下建自己的子目录。

架构相关的调试入口由 `kvm_arch_create_vm_debugfs()`（`arm.c` 约 295 行）补充：

```c
void kvm_arch_create_vm_debugfs(struct kvm *kvm)
{
    kvm_sys_regs_create_debugfs(kvm);      /* 查看/模拟系统寄存器状态 */
    kvm_s2_ptdump_create_debugfs(kvm);     /* dump stage-2 页表 */
}
```

`kvm_s2_ptdump_create_debugfs()` 对排错特别有用：当 guest 报「访问了不该访问的
IPA」时，可以直接 dump stage-2 页表看该 IPA 是否被映射、权限是什么，
而不必从 `kvm_handle_guest_abort()` 的 trace 反推。

### 22.2 tracepoint

本笔记涉及的关键 tracepoint 及其触发点：

| tracepoint | 触发位置 | 用途 |
| --- | --- | --- |
| `trace_kvm_entry` | `kvm_arch_vcpu_ioctl_run()` 进入 guest 前，参数是 guest PC | 统计进入次数、定位死循环 |
| `trace_kvm_exit` | 退出后，参数含 `ret`、trap class、PC | 分析退出原因分布 |
| `trace_kvm_guest_fault` | `kvm_handle_guest_abort()` 判定 IPA 合法后 | 记录 ESR/FAR/IPA 三元组 |
| `trace_kvm_access_fault` | `handle_access_fault()` | access flag 故障频率 |
| `trace_vgic_update_irq_pending` | `kvm_vgic_inject_irq()` | 中断注入溯源 |
| `trace_kvm_handle_sys_reg` | `kvm_handle_sys_reg()` 入口，参数是 ESR | 找出「谁在频繁 trap 系统寄存器」 |

一个实用的排查组合：用 `kvm_exit` 看出「退出太多」，用 `kvm_guest_fault` 看
「是不是 stage-2 缺页」，再用 `kvm_s2_ptdump` 确认页表状态。如果
`kvm_handle_sys_reg` 频率异常高，通常是 guest 在轮询某个未实现的寄存器，
或者 FGT 配置把不该 trap 的寄存器也 trap 了。

### 22.3 常见错误信息对照

| 输出 | 位置 | 含义 |
| --- | --- | --- |
| `Unsupported FSC: EC=... xFSC=...` | `kvm_handle_guest_abort()` | FSC 不属于四类已知故障，通常是硬件/固件异常 |
| `kvm_arch already initialized?` | `kvm_init_stage2_mmu()` | canonical stage-2 被重复初始化，内核 bug |
| `No vgic maintenance irq` | `kvm_vgic_hyp_init()` | GIC 驱动没给维护中断号，VGIC 无法工作 |
| `Non-architectural vgic, tainting kernel` | `kvm_vgic_hyp_init()` | 宿主 GIC 实现不符合架构，deactivate 受损 |
| `VGIC redist and dist frames overlap` | `vgic_v3_map_resources()` | 用户态给的地址区间重叠 |
| `Need to set vgic distributor addresses first` | `vgic_v3_map_resources()` | 调用顺序错误：应先设 redistributor 再设 distributor |

## 23. 当前配置矩阵

把本篇涉及的特性按「源码存在 / 当前启用 / 影响」三列整理，避免把
「代码里有」误读成「运行时可用」：

| 特性 | 源码位置 | 当前 `.config` | 影响 |
| --- | --- | --- | --- |
| KVM 核心 | `virt/kvm/`、`arch/arm64/kvm/` | `CONFIG_KVM=y` | 正常可用 |
| MMIO 模拟 | `arch/arm64/kvm/mmio.c` | `CONFIG_KVM_MMIO=y` | `KVM_EXIT_MMIO` 路径可用 |
| 内核 irqchip / 路由 | `virt/kvm/irqchip.c` | `CONFIG_HAVE_KVM_IRQCHIP/IRQ_ROUTING=y` | VGIC + irqfd 可用 |
| dirty ring | `virt/kvm/dirty_ring.c` 等 | `CONFIG_HAVE_KVM_DIRTY_RING=y` | 需用户态用 `KVM_CAP_DIRTY_LOG_RING` 开启 |
| guest_memfd | `virt/kvm/guest_memfd.c` | `CONFIG_KVM_GUEST_MEMFD=y` | `gmem_abort()` 路径可用 |
| KVM-VFIO 协作 | `virt/kvm/vfio.c` | `CONFIG_KVM_VFIO=y` | 但受 IOMMU 缺失限制，见 03 篇 |
| LPA2（52 位 IPA） | `arch/arm64/kvm/mmu.c` | `CONFIG_ARM64_LPA2=y` | `VTCR_EL2` 可配 52 位 IPA |
| pKVM | `arch/arm64/kvm/hyp/nvhe/` | 未启用 | `KVM_PGT_FN()` 恒等，`pkvm_*` 不执行 |
| NV（嵌套虚拟化） | `arch/arm64/kvm/nested.c`、`sys_regs.c` | 视 `kvm-arm.nested` 参数 | 未开启时 `nested_mmus` 为空 |
| IOMMU 支撑 | `drivers/iommu/` | `CONFIG_IOMMU_SUPPORT` 未启用 | 不影响 stage-2，但阻断 VFIO 直通 |
| ACPI | `drivers/acpi/` | `CONFIG_ACPI` 未启用 | 无法走 ACPI 的 GIC 描述（本树以 DT 为主） |
| 实时抢占 | 全局 | `CONFIG_PREEMPT_RT=y` | 锁语义变化见第 16 节 |

补一句关于 NV 的说明：`kvm_init_nested(kvm)` 在 `kvm_arch_init_vm()` 里被无条件
调用，但它是否真正建立影子 stage-2，取决于模块参数 `kvm-arm.nested` 与
硬件是否支持。当前配置下即使 `CONFIG_ARM64_LPA2=y`，也不代表 NV 一定可用。

## 24. 补充文件清单（本篇新增章节涉及）

| 路径 | 作用 |
| --- | --- |
| `virt/kvm/kvm_main.c` | `kvm_init()`/`kvm_exit()`、`kvm_create_vm()`、`kvm_vm_ioctl_create_vcpu()`、`kvm_set_memslot()`、`kvm_swap_active_memslots()`、`kvm_vcpu_ioctl()` |
| `include/linux/kvm_host.h` | `struct kvm` 的 `mmu_lock` 类型选择、memslots 结构、`KVM_MEMSLOT_GEN_UPDATE_IN_PROGRESS` |
| `include/linux/irqchip/arm-vgic-info.h` | `struct gic_kvm_info`、`vgic_set_kvm_info()` |
| `drivers/irqchip/irq-gic-v3.c` | 填充 `gic_v3_kvm_info` 并交给 KVM |
| `arch/arm64/kvm/arm.c` | `kvm_arch_init_vm()`、`kvm_arch_destroy_vm()`、`kvm_arch_vcpu_load/put()`、`kvm_arch_vcpu_ioctl_run()`、`kvm_arch_create_vm_debugfs()` |
| `arch/arm64/kvm/mmu.c` | `kvm_handle_guest_abort()`、`user_mem_abort()`、`handle_access_fault()`、`host_owns_sea()`、`kvm_handle_guest_sea()`、`kvm_init_stage2_mmu()`、`kvm_uninit_stage2_mmu()` |
| `arch/arm64/kvm/vgic/vgic-init.c` | `kvm_vgic_create()`、`kvm_vgic_hyp_init()`、`kvm_vgic_map_resources()` |
| `arch/arm64/kvm/vgic/vgic-v3.c` | `vgic_v3_map_resources()`、`vgic_v3_load()` |
| `arch/arm64/kvm/vgic/vgic.c` | `kvm_vgic_inject_irq()`、`kvm_vgic_map_irq()`、`kvm_vgic_flush_hwstate()`、`kvm_vgic_sync_hwstate()`、`kvm_vgic_process_async_update()` |
| `arch/arm64/kvm/sys_regs.c` | `kvm_handle_sys_reg()`、`sys_reg_descs[]`、`trap_raz_wi`、`kvm_reset_sys_regs()`、`get_reg_by_id()` |
| `arch/arm64/kvm/hyp/vhe/switch.c` | `kvm_vcpu_load_vhe()`、`kvm_vcpu_put_vhe()`、`__activate_traps()`、`__deactivate_traps()`、`__kvm_vcpu_run_vhe()`、`__kvm_vcpu_run()` |
| `arch/arm64/kvm/hyp/nvhe/switch.c` | nVHE 下的 `__kvm_vcpu_run()` |
| `arch/arm64/kvm/hyp/nvhe/hyp-main.c` | `handle___kvm_vcpu_run()`、hyp 调用表注册 |
