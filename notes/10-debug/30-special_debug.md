# 特殊调试功能

## 1. 调度器调试 (Scheduler Debug)

### 1.1 功能概述

调度器调试功能提供详细的调度器状态信息，帮助分析调度器行为和性能问题。

### 1.2 编译配置

```kconfig
config SCHED_DEBUG
    bool "Scheduler debugging"
    depends on DEBUG_KERNEL
    help
      Enable scheduler debugging features.
```

### 1.3 debugfs 接口

```bash
# 查看调度器统计信息
cat /sys/kernel/debug/sched/debug

# 查看每个 CPU 的运行队列
cat /sys/kernel/debug/sched/cpu0/rq

# 查看任务信息
cat /sys/kernel/debug/sched/task/<pid>
```

### 1.4 输出内容

| 文件 | 说明 |
|------|------|
| `debug` | 调度器全局状态 |
| `cpuN/rq` | CPU N 的运行队列信息 |
| `task/<pid>` | 指定进程的调度信息 |
| `latency_ns` | 调度延迟统计 |

### 1.5 代码位置

- `kernel/sched/debug.c`

---

## 2. 时间子系统调试

### 2.1 功能概述

时间子系统调试提供时钟、定时器和时间相关的调试信息。

### 2.2 debugfs 接口

```bash
# 查看时钟设备信息
cat /sys/kernel/debug/clocksource

# 查看定时器信息
cat /sys/kernel/debug/timer_list

# 查看 hrtimer 信息
cat /sys/kernel/debug/hrtimer

# 查看时钟事件设备
cat /sys/kernel/debug/clockevents
```

### 2.3 常用接口说明

| 文件 | 说明 |
|------|------|
| `clocksource` | 时钟源信息 |
| `timer_list` | 系统定时器列表 |
| `hrtimer` | 高精度定时器信息 |
| `clockevents` | 时钟事件设备信息 |

### 2.4 代码位置

- `kernel/time/debug.c`

---

## 3. 内核符号表 (kallsyms)

### 3.1 功能概述

kallsyms 提供内核符号表，将内存地址转换为符号名称，用于调试和分析。

### 3.2 编译配置

```kconfig
config KALLSYMS
    bool "Load all symbols for debugging/ksymoops"
    default y
    help
      If you say Y here, the entire kernel symbol table will be loaded into
      the kernel image.

config KALLSYMS_ALL
    bool "Include all symbols in kallsyms"
    depends on KALLSYMS
    help
      If you say Y here, kallsyms will include all symbols, not just those
      needed for backtraces.

config KALLSYMS_BASE_RELATIVE
    bool "Base kallsyms on relative addresses"
    depends on KALLSYMS
    help
      If you say Y here, kallsyms will use relative addresses instead of
      absolute addresses.
```

### 3.3 核心数据结构

```c
extern const unsigned long kallsyms_addresses[];
extern const unsigned int kallsyms_offsets[];
extern const u8 kallsyms_names[];
extern const u8 kallsyms_token_table[];
extern const u16 kallsyms_token_index[];
extern const unsigned int kallsyms_num_syms;
```

### 3.4 符号查找机制

```c
static unsigned int kallsyms_expand_symbol(unsigned int off,
                                           char *result, size_t maxlen)
{
    int len, skipped_first = 0;
    const char *tptr;
    const u8 *data;

    data = &kallsyms_names[off];
    len = *data;
    data++;
    off++;

    if ((len & 0x80) != 0) {
        len = (len & 0x7F) | (*data << 7);
        data++;
        off++;
    }

    off += len;

    while (len) {
        tptr = &kallsyms_token_table[kallsyms_token_index[*data]];
        data++;
        len--;

        while (*tptr) {
            ...
        }
    }
    ...
}
```

### 3.5 用户接口

```bash
# 查看内核符号表
cat /proc/kallsyms

# 将地址转换为符号
cat /proc/kallsyms | grep <address>

# 查看模块符号
cat /proc/modules

# 使用 kallsyms_lookup_name() 查找符号地址
```

### 3.6 代码位置

- `kernel/kallsyms.c`
- `scripts/kallsyms.c`

---

## 4. VGIC 调试

### 4.1 功能概述

VGIC (Virtual Generic Interrupt Controller) 调试提供虚拟化中断控制器的状态信息。

### 4.2 debugfs 接口

```bash
# 查看 VGIC 状态
cat /sys/kernel/debug/kvm/<vmid>/vgic

# 查看 VGIC 中断状态
cat /sys/kernel/debug/kvm/<vmid>/vgic_state
```

### 4.3 核心数据结构

```c
struct vgic_state_iter {
    int nr_cpus;
    int nr_spis;
    int dist_id;
    int vcpu_id;
    unsigned long intid;
};
```

### 4.4 状态遍历机制

```c
static void iter_next(struct kvm *kvm, struct vgic_state_iter *iter)
{
    struct vgic_dist *dist = &kvm->arch.vgic;

    if (iter->dist_id == 0) {
        iter->dist_id++;
        return;
    }

    if (iter->intid >= (iter->nr_spis + VGIC_NR_PRIVATE_IRQS - 1)) {
        if (iter->intid == VGIC_LPI_MAX_INTID + 1)
            return;

        rcu_read_lock();
        if (!xa_find_after(&dist->lpi_xa, &iter->intid,
                           VGIC_LPI_MAX_INTID, XA_PRESENT))
            iter->intid = VGIC_LPI_MAX_INTID + 1;
        rcu_read_unlock();
        return;
    }

    iter->intid++;
    if (iter->intid == VGIC_NR_PRIVATE_IRQS &&
        ++iter->vcpu_id < iter->nr_cpus)
        iter->intid = 0;
}
```

### 4.5 中断类型

| 类型 | INTID 范围 | 说明 |
|------|-----------|------|
| PPI | 0-15 | 私有外设中断 |
| SGI | 16-31 | 软件生成中断 |
| SPI | 32-1019 | 共享外设中断 |
| LPI | 8192-1048575 | 本地外设中断 |

### 4.6 代码位置

- `arch/arm64/kvm/vgic/vgic-debug.c`

---

## 5. Hyp 调试

### 5.1 功能概述

Hyp (Hypervisor) 调试支持 EL2 级别的调试寄存器保存和恢复。

### 5.2 调试寄存器管理

```c
#define read_debug(r,n)     read_sysreg(r##n##_el1)
#define write_debug(v,r,n)  write_sysreg(v, r##n##_el1)

#define save_debug(ptr,reg,nr)                          \
    switch (nr) {                                       \
    case 15:    ptr[15] = read_debug(reg, 15);         \
                fallthrough;                            \
    case 14:    ptr[14] = read_debug(reg, 14);         \
                fallthrough;                            \
    ...
    default:    ptr[0] = read_debug(reg, 0);           \
    }

#define restore_debug(ptr,reg,nr)                       \
    switch (nr) {                                       \
    case 15:    write_debug(ptr[15], reg, 15);         \
                fallthrough;                            \
    ...
    }
```

### 5.3 Hyp 调试模式

**VHE (Virtualization Host Extensions) 模式：**
- 主机直接运行在 EL2
- 调试寄存器访问更加直接

**nVHE (non-VHE) 模式：**
- 需要切换到 EL2 才能访问调试寄存器
- 需要保存/恢复调试状态

### 5.4 代码位置

| 模式 | 文件 |
|------|------|
| VHE | `arch/arm64/kvm/hyp/vhe/debug-sr.c` |
| nVHE | `arch/arm64/kvm/hyp/nvhe/debug-sr.c` |
| 头文件 | `arch/arm64/kvm/hyp/include/hyp/debug-sr.h` |

---

## 6. 其他调试功能

### 6.1 lockstat

```bash
# 查看锁统计信息
cat /proc/lock_stat
```

### 6.2 schedstat

```bash
# 查看调度统计信息
cat /proc/schedstat
```

### 6.3 softirqs

```bash
# 查看软中断统计
cat /proc/softirqs
```

### 6.4 interrupts

```bash
# 查看中断统计
cat /proc/interrupts
```