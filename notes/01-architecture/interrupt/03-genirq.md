# genirq 通用中断层

## 1. 概述

`kernel/irq/` 实现架构无关的通用中断层（genirq），主要职责：

1. `struct irq_desc` 描述一条 Linux 虚拟中断（virq）；
2. `struct irq_chip` 抽象中断控制器寄存器操作；
3. `struct irq_domain` 完成 hwirq ↔ virq 的翻译与映射；
4. `struct irqaction` 管理驱动注册的处理函数；
5. 提供 flow handler、threaded IRQ、affinity、统计与 spurious 检测。

当前 `.config`：

| 配置 | 值 | 影响 |
| --- | --- | --- |
| `CONFIG_SPARSE_IRQ` | y | 描述符按需分配，maple tree 管理 |
| `CONFIG_IRQ_DOMAIN_HIERARCHY` | y | 层次化 irq domain |
| `CONFIG_IRQ_TIME_ACCOUNTING` | y | 中断时间计入调度统计 |
| `CONFIG_GENERIC_IRQ_DEBUGFS` | n | 无 `/sys/kernel/debug/irq` |
| `CONFIG_SMP` | y | affinity、IPI、per-CPU 中断生效 |

ARM64 的根中断控制器（通常是 GICv3）通过 `set_handle_irq()` 注册
`handle_arch_irq`，异常入口最终进入 `generic_handle_arch_irq()`。

## 2. 核心数据结构

### 2.1 irq_desc

`include/linux/irqdesc.h:81`，关键字段：

| 字段 | 作用 |
| --- | --- |
| `irq_common_data` | 所有 chip 层共享：state、affinity、msi_desc、handler_data |
| `irq_data` | 当前 chip 层：irq、hwirq、chip、domain、parent_data |
| `kstat_irqs` | per-CPU `struct irqstat`，热路径计数 |
| `handle_irq` | flow handler，如 `handle_fasteoi_irq` |
| `action` | `irqaction` 链表，支持共享中断 |
| `depth` | `disable_irq()` 嵌套深度，降到 0 才真正 unmask |
| `lock` | 保护 desc 状态的 raw spinlock |
| `request_mutex` | 串行化 request/free |
| `threads_*` | threaded IRQ 的 oneshot/active 状态 |
| `affinity_hint` / `affinity_notify` | 亲和性提示与变更通知 |
| `refcnt` / `kobj` | 引用计数与 sysfs 表示（SPARSE_IRQ） |

`irq_common_data`（`include/linux/irq.h:150`）含 `state_use_accessors`、
NUMA node、`handler_data`、`msi_desc`、`affinity`、`effective_affinity`；
`IRQD_*` 状态位（`IRQD_IRQ_DISABLED/MASKED/INPROGRESS/LEVEL/PER_CPU/
AFFINITY_MANAGED/...`）都在这里。

### 2.2 irq_data 与 irq_chip

`struct irq_data`（`include/linux/irq.h:182`）保存 `mask`、`irq`（virq）、
`hwirq`、`common`、`chip`、`domain`，层次 domain 下用 `parent_data` 串到
父控制器，`chip_data` 是控制器私有数据。

`struct irq_chip`（`include/linux/irq.h:499`）的回调分几类：

| 类别 | 回调 |
| --- | --- |
| 生命周期 | `irq_startup`、`irq_shutdown`、`irq_enable`、`irq_disable` |
| 触发控制 | `irq_ack`、`irq_mask`、`irq_mask_ack`、`irq_unmask`、`irq_eoi` |
| 配置 | `irq_set_type`、`irq_set_wake`、`irq_set_affinity`、`irq_retrigger` |
| 总线锁 | `irq_bus_lock`、`irq_bus_sync_unlock` |
| 资源 | `irq_request_resources`、`irq_release_resources` |
| MSI | `irq_compose_msi_msg`、`irq_write_msi_msg`、`irq_set_vcpu_affinity` |
| IPI | `ipi_send_single`、`ipi_send_mask` |
| NMI | `irq_nmi_setup`、`irq_nmi_teardown` |
| 通用 | `irq_print_chip`、`irq_get/set_irqchip_state`、`flags` |

`flags` 用 `IRQCHIP_*` 表达语义，如 `IRQCHIP_SET_TYPE_MASKED`、
`IRQCHIP_EOI_IF_HANDLED`、`IRQCHIP_MASK_ON_SUSPEND`、`IRQCHIP_IMMUTABLE`。

### 2.3 irqaction

`struct irqaction`（`include/linux/interrupt.h:123`）关键字段：`handler`
（硬中断上下文）、`dev_id`/`percpu_dev_id`、`next`（共享中断链）、
`thread_fn`/`thread`（线程化）、`secondary`、`irq`、`flags`（`IRQF_*`）、
`thread_flags`、`thread_mask`、`name`、`dir`。

常用标志：`IRQF_SHARED`、`IRQF_ONESHOT`、`IRQF_NO_THREAD`、`IRQF_PERCPU`、
`IRQF_NO_AUTOEN`、`IRQF_TRIGGER_*`、`IRQF_NO_SUSPEND`、`IRQF_COND_SUSPEND`。

### 2.4 irq_domain

`include/linux/irqdomain.h:168`，核心字段：

| 字段 | 作用 |
| --- | --- |
| `ops` | `map/unmap/alloc/free/translate/xlate` 回调 |
| `host_data` | 控制器私有数据 |
| `root` | 层次 domain 的根 |
| `parent` | 父 domain（HIERARCHY） |
| `fwnode` | 对应的 firmware node（DT/ACPI） |
| `hwirq_max` | hwirq 上限 |
| `revmap` | hwirq → irq_data 反向映射（线性数组或 radix tree） |
| `flags` | `HIERARCHY`、`MSI`、`IPI_PER_CPU`、`NO_MAP` 等 |

## 3. 描述符分配与查找

`CONFIG_SPARSE_IRQ=y` 使用 maple tree（`kernel/irq/irqdesc.c:168`）：

```c
static DEFINE_MUTEX(sparse_irq_lock);
static struct maple_tree sparse_irqs = MTREE_INIT_EXT(sparse_irqs,
					MT_FLAGS_ALLOC_RANGE |
					MT_FLAGS_LOCK_EXTERN |
					MT_FLAGS_USE_RCU,
					sparse_irq_lock);

struct irq_desc *irq_to_desc(unsigned int irq)
{
	return mtree_load(&sparse_irqs, irq);
}
```

- `irq_find_desc_at_or_after()` 用 `mt_find()` 支持 `/proc/interrupts` 遍历；
- `__irq_alloc_descs()`（891 行）在锁内找空洞、`alloc_desc()` 并插入；
- `alloc_desc()`（213 行附近）初始化 per-CPU kstat、masks、spinlock、
  `request_mutex`、`wait_for_threads`；
- 非 SPARSE 配置退化为 `irq_desc[NR_IRQS]` 静态数组。

## 4. irq_domain 与映射

### 4.1 翻译

`irq_domain_translate()`（`kernel/irq/irqdomain.c:853`）：

```c
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	if (d->ops->translate)
		return d->ops->translate(d, fwspec, hwirq, type);
#endif
	if (d->ops->xlate)
		return d->ops->xlate(d, to_of_node(fwspec->fwnode),
				     fwspec->param, fwspec->param_count,
				     hwirq, type);
	*hwirq = fwspec->param[0];
	return 0;
```

通用实现有 `irq_domain_translate_onecell()`（1216）、`twocell()`（1241）、
`twothreecell()`（1266）。

### 4.2 映射

```
irq_create_fwspec_mapping(fwspec)                    [irqdomain.c:913]
├─ fwspec_to_domain()                                fwnode → domain
├─ irq_domain_translate() → hwirq/type
├─ irq_find_mapping()                                已映射则复用/补类型
├─ 层次 domain → irq_domain_alloc_irqs_locked()
│  └─ 逐层 ops->alloc()/map()，父域递归
└─ 非层次 → irq_create_mapping_affinity_locked()
   ├─ irq_domain_alloc_descs()
   └─ irq_domain_associate_locked()                  virq ↔ hwirq
```

`irq_create_mapping_affinity()`（822）是只给 hwirq 的入口；
`irq_create_of_mapping()`（1009）把 `of_phandle_args` 转 `irq_fwspec` 后调用。
`irq_resolve_mapping(domain, hwirq)` 可从域+硬件号直接查 `irq_desc`。

### 4.3 分发入口

`generic_handle_domain_irq()`（`irqdesc.c:733`）：

```c
int generic_handle_domain_irq(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	return handle_irq_desc(irq_resolve_mapping(domain, hwirq));
}
```

`handle_irq_desc()` 最终调用 `desc->handle_irq(desc)`。
`generic_handle_domain_irq_safe()`（751）临时关中断，
`generic_handle_domain_nmi()`（774）校验 NMI 上下文。

## 5. flow handler 与 chip 绑定

| flow handler | 场景 |
| --- | --- |
| `handle_simple_irq()` | 无 mask/ack 要求的简单控制器 |
| `handle_level_irq()` | 电平触发：mask → ack → handle → unmask |
| `handle_edge_irq()` | 边沿触发：ack 后处理，处理期间 mask |
| `handle_fasteoi_irq()` | 现代控制器：入口 ack、出口 eoi |
| `handle_fasteoi_ack_irq()` / `handle_fasteoi_mask_irq()` | fasteoi 变体 |
| `handle_percpu_irq()` / `handle_percpu_devid_irq()` | per-CPU 中断 |
| `handle_nested_irq()` | 线程化子中断 |
| `handle_fasteoi_nmi()` | NMI |

`handle_fasteoi_irq()`（`kernel/irq/chip.c:740`）主干：持 `desc->lock`
检查 PM/action 可用性，必要时置 `IRQS_PENDING` 并 EOI；否则
`kstat_incr_irqs_this_cpu()`，`IRQF_ONESHOT` 时先 mask，调用
`handle_irq_event()`，出口 `cond_unmask_eoi_irq()`；若有 pending 则
`check_irq_resend()`。

绑定 API：`irq_set_chip()`（43 行）、`irq_set_chip_data()`（128）、
`irq_set_chip_and_handler_name()`（1041）、`irq_domain_set_info()`
（domain map 回调里最常用）、`irq_set_chained_handler_and_data()`。

### GICv3 示例

`drivers/irqchip/irq-gic-v3.c:1547`：

SGI/PPI/EPPI 区间先 `irq_set_percpu_devid()`，再
`irq_domain_set_info(..., handle_percpu_devid_irq, ...)`；SPI/ESPI/LPI
使用 `handle_fasteoi_irq`，SPI 还 `irq_set_probe()` 并设置 single target；
最后 `irqd_set_handle_enforce_irqctx(irqd)` 禁止软件重触发打乱 ACK/EOI。

`gic_irq_domain_translate()`（1589）把 DT 三 cell（type, number, flags）翻成
hwirq：SPI +32、PPI +16、ESPI/EPPI 加到各自基址；单 cell 且号 < 16 视为
SGI 边沿触发。

硬件入口 `__gic_handle_irq()`（818）：

```c
if (gic_irqnr_is_special(irqnr))
	return;
gic_complete_ack(irqnr);	/* EOI mode 1 写 ICC_EOIR1_EL1 + isb */
if (generic_handle_domain_irq(gic_data.domain, irqnr)) {
	WARN_ONCE(true, "Unexpected interrupt (irqnr %u)\n", irqnr);
	gic_deactivate_unhandled(irqnr);
}
```

## 6. 中断处理路径

```
ARM64 异常入口
└─ handle_arch_irq(regs)                         set_handle_irq() 注册
   └─ gic_handle_irq()
      ├─ gic_complete_ack()
      └─ generic_handle_domain_irq(domain, irqnr)
         └─ handle_irq_desc(desc)
            └─ desc->handle_irq(desc)            flow handler
               └─ handle_irq_event()             [handle.c:255]
                  ├─ desc->istate &= ~IRQS_PENDING
                  ├─ irqd_set(IRQD_IRQ_INPROGRESS)
                  ├─ raw_spin_unlock(&desc->lock)
                  ├─ handle_irq_event_percpu()
                  │  ├─ __handle_irq_event_percpu()
                  │  │  └─ for_each_action_of_desc()
                  │  │     ├─ trace_irq_handler_entry()
                  │  │     ├─ action->handler(irq, dev_id)
                  │  │     ├─ irqhandler_duration_check()（可选）
                  │  │     ├─ trace_irq_handler_exit()
                  │  │     └─ IRQ_WAKE_THREAD → __irq_wake_thread()
                  │  ├─ add_interrupt_randomness()
                  │  └─ note_interrupt()            spurious.c:222
                  └─ raw_spin_lock(&desc->lock)
                     irqd_clear(IRQD_IRQ_INPROGRESS)
```

`generic_handle_arch_irq()`（handle.c:285）负责最外层记账：

```c
irq_enter();
old_regs = set_irq_regs(regs);
handle_arch_irq(regs);
set_irq_regs(old_regs);
irq_exit();
```

`irq_enter()/irq_exit()` 触发 `irqtime_account_irq()`
（`kernel/sched/cputime.c:59`，`CONFIG_IRQ_TIME_ACCOUNTING=y`）。
`note_interrupt()` 做坏中断检测：跳过 polled，`bad_action_ret()` 时
`report_bad_irq()`，线程化场景把分析推迟到下一次硬中断。

## 7. threaded IRQ

`request_threaded_irq()`（`kernel/irq/manage.c:2123`）要点：

校验顺序：`IRQ_NOTCONNECTED` 返回 `-ENOTCONN`；`IRQF_SHARED` 必须有
`dev_id` 且不能与 `IRQF_NO_AUTOEN` 同用；非共享不能带
`IRQF_COND_SUSPEND`；`IRQF_NO_SUSPEND` 不能与 `IRQF_COND_SUSPEND` 同用；
`irq_to_desc()`/`irq_settings_can_request()`/per-CPU devid 检查；未给
`handler` 时用 `irq_default_primary_handler`；最后
`kzalloc` `irqaction` 并调用 `__setup_irq()`。

`__setup_irq()` 负责共享兼容性、`__irq_set_trigger()`、`irq_request_resources()`、
`irq_startup()`、创建 `irq_thread()`、挂入 `desc->action`。`request_irq()`
就是 `thread_fn = NULL` 的包装。

线程模型：

- 硬中断 `handler` 返回 `IRQ_WAKE_THREAD` → `__irq_wake_thread()` 置
  `IRQTF_RUNTHREAD` 并唤醒 `action->thread`；
- `irq_thread()`（1244）循环调用 `irq_thread_fn()` 执行 `thread_fn`；
- `IRQF_ONESHOT`：硬中断先 mask，线程结束由 `irq_finalize_oneshot()` unmask；
- `threadirqs` 参数（`setup_forced_irqthreads()`，30 行）强制线程化，
  `irq_setup_forced_threading()`（1312）构造 forced handler。

## 8. affinity

```
irq_set_affinity(irq, cpumask)                    [manage.c:481]
└─ __irq_set_affinity(irq, mask, force=false)
   └─ irq_set_affinity_locked()
      ├─ irq_do_set_affinity()                    [manage.c:232]
      │  ├─ 过滤 offline CPU
      │  ├─ 更新 IRQD_AFFINITY_MANAGED 等状态
      │  └─ chip->irq_set_affinity() / pending
      ├─ irq_set_thread_affinity()
      └─ irq_affinity_schedule_notify_work()
```

- `irq_update_affinity_desc()`（419）用于 managed IRQ 激活/去激活；
- `irq_setup_affinity()`（592）在启动时设置默认亲和性；
- `irq_force_affinity()`（498）跳过 online 检查，仅供 CPU 热插拔底层；
- 用户态通过 `/proc/irq/N/smp_affinity` 与 `smp_affinity_list` 读写
  （`kernel/irq/proc.c`），`no_irq_affinity` 可全局禁用；
- 层次 domain 下 affinity 沿 `parent_data` 传给父控制器。

## 9. 统计与时间戳

`struct irqstat`（`include/linux/irqdesc.h:26`）只有 `cnt`，可选
`ref`（`CONFIG_GENERIC_IRQ_STAT_SNAPSHOT`）。flow handler 入口调用
`kstat_incr_irqs_this_cpu(desc)` 增本 CPU 计数；
`kstat_incr_irq_this_cpu(irq)`（`irqdesc.c:989`）是带查找的包装；
`kstat_irqs_cpu(irq, cpu)` 读单 CPU，`kstat_irqs()` 求和。
`/proc/interrupts` 由 `show_interrupts()` 输出每 CPU 计数与
`irq_print_chip()` 的控制器名；`arch_show_interrupts()` 弱符号供架构追加 IPI。

时间相关：

- `irqhandler.duration_warn_us=N` 启用 static key，处理函数前后
  `local_clock()`，超阈值打印 `long duration of IRQ[...]`（handle.c:173）；
- `trace_irq_handler_entry/exit` 供 ftrace/perf 记录耗时；
- `CONFIG_IRQ_TIME_ACCOUNTING=y` 时 `irqtime_account_irq()` 把 hardirq/softirq
  时间累加到 per-CPU `irqtime`，影响 `/proc/stat` 与调度统计；
- `CONFIG_GENERIC_IRQ_DEBUGFS=n`，无 debugfs 逐 IRQ 视图。

## 10. 当前配置下的差异

`SPARSE_IRQ=y` 使描述符动态分配、virq 可不连续（maple tree + RCU 读）；
`IRQ_DOMAIN_HIERARCHY=y` 支持 GICv3 ITS/LPI、GPIO 扩展等层次 domain；
`IRQ_TIME_ACCOUNTING=y` 把中断时间计入调度统计；
`GENERIC_IRQ_DEBUGFS=n` 无 `/sys/kernel/debug/irq/irqs/*`；`SMP=y` 使
affinity、IPI、per-CPU 中断与 CPU 热插拔路径全部编译。

## 11. 关键文件清单

| 文件 | 作用 |
| --- | --- |
| `kernel/irq/irqdesc.c` / `irqdomain.c` | 描述符分配、maple tree、domain 创建/translate/映射 |
| `kernel/irq/handle.c` / `chip.c` | `handle_irq_event()`、各种 flow handler、chip/handler 绑定 |
| `kernel/irq/manage.c` | `request_threaded_irq()`、`__setup_irq()`、affinity、线程 |
| `kernel/irq/spurious.c` / `kernel/sched/cputime.c` | `note_interrupt()` 与 `irqtime_account_irq()` |
| `drivers/irqchip/irq-gic-v3.c` | GICv3 irq_chip/irq_domain 与硬件入口 |
| `include/linux/irq.h` / `irqdesc.h` / `irqdomain.h` / `interrupt.h` | 核心数据结构与注册 API |
