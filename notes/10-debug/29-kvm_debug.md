# KVM 调试功能

## 1. 概述

KVM 提供了多种调试功能，用于调试虚拟机和 KVM 本身，包括 debugfs 接口、Guest Debug 支持等。

---

## 2. KVM debugfs 接口

### 2.1 编译配置

```kconfig
config KVM_DEBUG_FS
    bool "KVM debugfs support"
    depends on DEBUG_FS
    help
      Provide a debugfs interface for KVM.
```

### 2.2 目录结构

```
/sys/kernel/debug/kvm/
├── vmid/                  # 每个 VM 的目录
│   ├── vcpuid/            # 每个 VCPU 的目录
│   │   ├── guest_mode     # guest 模式统计
│   │   ├── tsc-offset     # TSC 偏移
│   │   ├── lapic_timer_advance_ns  # APIC 定时器提前量
│   │   └── tsc-scaling-ratio       # TSC 缩放比例
│   └── stats              # VM 统计信息
└── stats                  # 全局 KVM 统计信息
```

### 2.3 核心实现

```c
static struct dentry *kvm_debugfs_dir;

static __init int kvm_init(void)
{
    ...
    kvm_debugfs_dir = debugfs_create_dir("kvm", NULL);
    ...
}

static void __exit kvm_exit(void)
{
    ...
    debugfs_remove_recursive(kvm_debugfs_dir);
}
```

### 2.4 统计信息

**全局统计：**
```bash
cat /sys/kernel/debug/kvm/stats
```

**VM 级统计：**
```bash
cat /sys/kernel/debug/kvm/<vmid>/stats
```

**VCPU 级统计：**
```bash
cat /sys/kernel/debug/kvm/<vmid>/<vcpuid>/guest_mode
```

---

## 3. Guest Debug 支持

### 3.1 功能概述

KVM 支持通过 `KVM_SET_GUEST_DEBUG` ioctl 对 Guest 虚拟机进行调试，包括断点设置、单步执行等。

### 3.2 ioctl 接口

```c
struct kvm_guest_debug {
    __u32 control;
    __u32 pad;
    union {
        struct {
            __u64 addr;
            __u32 len;
            __u32 type;
        } breakpoint;
        struct {
            __u64 addr;
            __u32 len;
            __u32 type;
        } watchpoint;
        ...
    };
};
```

### 3.3 使用流程

```c
int kvm_arch_vcpu_ioctl_set_guest_debug(struct kvm_vcpu *vcpu,
                                         struct kvm_guest_debug *dbg)
{
    // 设置 guest 调试参数
    // 配置断点、观察点等
    ...
}
```

---

## 4. ARM64 KVM 调试

### 4.1 MDCR_EL2 配置

```c
static void kvm_arm_setup_mdcr_el2(struct kvm_vcpu *vcpu)
{
    preempt_disable();

    vcpu->arch.mdcr_el2 = FIELD_PREP(MDCR_EL2_HPMN,
                                     *host_data_ptr(nr_event_counters));
    vcpu->arch.mdcr_el2 |= (MDCR_EL2_TPM |
                            MDCR_EL2_TPMS |
                            MDCR_EL2_TTRF |
                            MDCR_EL2_TPMCR |
                            MDCR_EL2_TDRA |
                            MDCR_EL2_TDOSA);

    if (vcpu->guest_debug)
        vcpu->arch.mdcr_el2 |= MDCR_EL2_TDE;

    if (!kvm_guest_owns_debug_regs(vcpu))
        vcpu->arch.mdcr_el2 |= MDCR_EL2_TDA;

    if (has_vhe())
        write_sysreg(vcpu->arch.mdcr_el2, mdcr_el2);

    preempt_enable();
}
```

### 4.2 调试寄存器控制

**MDCR_EL2 位域说明：**

| 位域 | 说明 |
|------|------|
| `MDCR_EL2_TPM` | 陷阱性能监视器 |
| `MDCR_EL2_TPMS` | 陷阱统计分析器 |
| `MDCR_EL2_TTRF` | 陷阱追踪过滤器 |
| `MDCR_EL2_TPMCR` | 陷阱 PMCR_EL0 |
| `MDCR_EL2_TDRA` | 陷阱调试 ROM 地址 |
| `MDCR_EL2_TDOSA` | 陷阱 OS 相关寄存器 |
| `MDCR_EL2_TDE` | 陷阱调试异常 |
| `MDCR_EL2_TDA` | 陷阱调试寄存器 |

### 4.3 调试资源初始化

```c
void kvm_init_host_debug_data(void)
{
    u64 dfr0 = read_sysreg(id_aa64dfr0_el1);

    if (cpuid_feature_extract_signed_field(dfr0, ID_AA64DFR0_EL1_PMUVer_SHIFT) > 0)
        *host_data_ptr(nr_event_counters) = FIELD_GET(ARMV8_PMU_PMCR_N,
                                                      read_sysreg(pmcr_el0));

    *host_data_ptr(debug_brps) = SYS_FIELD_GET(ID_AA64DFR0_EL1, BRPs, dfr0);
    *host_data_ptr(debug_wrps) = SYS_FIELD_GET(ID_AA64DFR0_EL1, WRPs, dfr0);

    if (cpu_has_spe(dfr0))
        host_data_set_flag(HAS_SPE);

    if (has_vhe())
        return;

    if (cpuid_feature_extract_unsigned_field(dfr0, ID_AA64DFR0_EL1_BRBE_SHIFT))
        host_data_set_flag(HAS_BRBE);
    ...
}
```

---

## 5. x86 KVM debugfs

### 5.1 VCPU debugfs 文件

```c
void kvm_arch_create_vcpu_debugfs(struct kvm_vcpu *vcpu, struct dentry *debugfs_dentry)
{
    debugfs_create_file("guest_mode", 0444, debugfs_dentry, vcpu,
                        &vcpu_guest_mode_fops);
    debugfs_create_file("tsc-offset", 0444, debugfs_dentry, vcpu,
                        &vcpu_tsc_offset_fops);

    if (lapic_in_kernel(vcpu))
        debugfs_create_file("lapic_timer_advance_ns", 0444,
                            debugfs_dentry, vcpu,
                            &vcpu_timer_advance_ns_fops);

    if (kvm_caps.has_tsc_control) {
        debugfs_create_file("tsc-scaling-ratio", 0444,
                            debugfs_dentry, vcpu,
                            &vcpu_tsc_scaling_fops);
        debugfs_create_file("tsc-scaling-ratio-frac-bits", 0444,
                            debugfs_dentry, vcpu,
                            &vcpu_tsc_scaling_frac_fops);
    }
}
```

### 5.2 debugfs 文件说明

| 文件 | 说明 |
|------|------|
| `guest_mode` | guest 模式时间统计 |
| `tsc-offset` | TSC 偏移量 |
| `lapic_timer_advance_ns` | APIC 定时器提前量（纳秒） |
| `tsc-scaling-ratio` | TSC 缩放比例 |
| `tsc-scaling-ratio-frac-bits` | TSC 缩放比例小数位数 |

---

## 6. KVM 调试工具

### 6.1 kvm_stat

已在 [24-kvm_stat.md](./24-kvm_stat.md) 中详细说明。

### 6.2 kvm_check

```bash
# 检查 KVM 功能
kvm-ok

# 查看 KVM 信息
cat /proc/cpuinfo | grep -E "(vmx|svm)"
```

### 6.3 QEMU 调试

**启用 Guest 调试：**
```bash
qemu-system-aarch64 -s -S \
    -machine virt,gic-version=3 \
    -cpu cortex-a55 \
    -kernel Image \
    -append "console=ttyAMA0"
```

**连接 GDB：**
```bash
gdb vmlinux
(gdb) target remote localhost:1234
```

---

## 7. 代码位置

| 平台 | 文件 |
|------|------|
| 通用 | `virt/kvm/kvm_main.c` |
| ARM64 | `arch/arm64/kvm/debug.c` |
| x86 | `arch/x86/kvm/debugfs.c` |