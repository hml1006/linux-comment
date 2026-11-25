# Linux 电源管理：cpufreq / cpuidle / OPP / genpd / runtime PM / suspend / PSCI

## 1. 概述

Linux 电源管理可分为几个层次：

| 层次 | 目标 | 主要源码 |
|------|------|----------|
| 频率 | 按负载调整 CPU 频率/电压 | `drivers/cpufreq/`、`kernel/sched/cpufreq_schedutil.c` |
| CPU 空闲 | 选择 C-state 进入低功耗 | `drivers/cpuidle/` |
| 性能点 | 频率-电压-带宽约束的统一描述 | `drivers/opp/` |
| 电源域 | 按硬件电源域聚合开关与性能状态 | `drivers/pmdomain/` |
| 运行时 PM | 设备空闲时自动挂起 | `drivers/base/power/runtime.c` |
| 系统睡眠 | suspend-to-idle / suspend-to-RAM / hibernate | `kernel/power/`、`drivers/base/power/main.c` |
| 固件接口 | PSCI CPU_SUSPEND、OSI/PC 模式 | `drivers/firmware/psci/` |

当前 `.config` 是刻意裁剪的调试配置：`CONFIG_CPU_FREQ=y`（默认 schedutil），
`CONFIG_PM_OPP=y`；而 `CONFIG_PM`、`CONFIG_SUSPEND`、`CONFIG_CPU_IDLE` 均未启用。
因此大量电源管理代码虽然存在于源码树，但不会被编译；本文会逐项标注。

## 2. cpufreq

### 2.1 核心对象

`include/linux/cpufreq.h`：

`struct cpufreq_policy`（53）描述一个“共享时钟的 CPU 组”，关键字段：
`cpus/related_cpus/real_cpus`、`cpu`、`clk`、`min/max/cur`（kHz）、
`governor`/`governor_data`、`update` 工作、`freq_constraints` 与三个
`freq_qos_request`（min/max/boost）、`freq_table`、`policy_list`、`kobj`、
`rwsem`、`fast_switch_possible/enabled`。

`struct cpufreq_driver`（352）由 SoC 驱动实现，关键回调：

- `init(policy)` / `verify(policy_data)`：必须实现；初始化 policy 与校验限制。
- 二选一：`setpolicy()`，或 `target()`/`target_index()`/`fast_switch()`。
- 可选：`adjust_perf()`（需 `fast_switch` 配套）、`get_intermediate()` +
  `target_intermediate()`、`get()`、`update_limits()`、`online/offline`、
  `suspend/resume`、`ready()`、`set_boost()`。

驱动二选一：`setpolicy`（硬件事务型，如 intel_pstate 的 passive 模式），
或 `target_index`/`fast_switch`（软件调频器选频）。

### 2.2 驱动注册

`cpufreq_register_driver()`（`drivers/cpufreq/cpufreq.c:2907`）：

1. `cpufreq_disabled()` 时返回 `-ENODEV`；`get_cpu_device(0)` 不存在则
   `-EPROBE_DEFER`。
2. 校验回调组合：必须 `verify` + `init`；`setpolicy` 与 `target/target_index`
   互斥；`get_intermediate/target_intermediate`、`online/offline` 必须成对；
   `adjust_perf` 必须伴随 `fast_switch`。
3. 全局只允许一个 `cpufreq_driver`；`setpolicy` 驱动自动加 `CPUFREQ_CONST_LOOPS`；
   支持 boost 时创建 sysfs；非 `setpolicy` 驱动打开 `cpufreq_freq_invariance`。
4. `subsys_interface_register(&cpufreq_interface)` 为每个 CPU 设备调用
   `cpufreq_add_dev()` → `cpufreq_online(cpu)`。
5. 注册 CPU hotplug 状态 `CPUHP_AP_ONLINE_DYN`，online/offline 时增删 policy。

### 2.3 policy 建立与调频器

`cpufreq_online()`（1585）为 CPU 分配或复用 policy，调用 `driver->init(policy)`
填充 `freq_table`、`min/max`、`fast_switch_possible`，建立 sysfs，最后
`cpufreq_init_policy()`（1104）：`has_target()` 时优先恢复
`policy->last_governor`，否则用 `get_governor(default_governor)`；
`setpolicy` 型则取 `last_policy` 或解析默认 governor，最终调用
`cpufreq_set_policy()`。

`cpufreq_set_policy()`（2625）先从 PM QoS `freq_constraints` 读取 min/max，
`driver->verify()` 校验，再用 `__resolve_freq()` 把限制对齐到频率表
（max 用 `CPUFREQ_RELATION_H`，min 用 `CPUFREQ_RELATION_L`），
然后 `setpolicy` 或切换 governor（stop/exit 旧的、init/start 新的）。

调频器最终调用 `__cpufreq_driver_target()`（2349）：把目标频率解析为索引，
若与 `policy->cur` 相同且无需更新限制则直接返回；否则走 `driver->target()`，
或 `__target_index()` → `target_index()/fast_switch()`。

### 2.4 schedutil

schedutil 位于 `kernel/sched/cpufreq_schedutil.c`（不是 `drivers/cpufreq/`），
其 `struct cpufreq_governor schedutil_gov`（927）注册到 cpufreq 核心。
调度器在 `update_rq_clock`/util 变化时通过 `cpufreq_update_util()` 回调：

- `sugov_update_single_freq()`（419）：单 CPU 共享 policy 时直接按 util 选频。
- `sugov_update_single_perf()`（460）：驱动提供 `adjust_perf()` 时传性能提示。
- `sugov_update_shared()` / `sugov_next_freq_shared()`（493）：多 CPU 共享
  policy 时取最大 util，结合 `map_util_freq()` 与 `sugov_cpu_is_busy()` 决定频率。

当前 `.config`：`CONFIG_CPU_FREQ=y`、`CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y`，
所有传统 governor（performance/powersave/userspace/ondemand/conservative）
也都编译为 y。注意 `cpufreq-dt`、`scmi-cpufreq` 等具体驱动取决于平台配置，
本树未逐一启用，具体以当前 `.config` 与 `drivers/cpufreq/Kconfig.arm` 为准。

## 3. cpuidle

### 3.1 核心对象

`include/linux/cpuidle.h`：

- `struct cpuidle_driver`（152）：`name`、`owner`、`states[CPUIDLE_STATE_MAX]`、
  `state_count`、`safe_state_index`、`cpumask`、`governor`。
- `struct cpuidle_state`：`exit_latency`、`target_residency`、`power_usage`、
  `flags`（`CPUIDLE_FLAG_POLLING`/`TIMER_STOP` 等）、`enter()`、`enter_s2idle()`。
- `struct cpuidle_governor`（292）：`name`、`rating`、`enable/disable`、
  `select()`、`reflect()`。
- `struct cpuidle_device`：每个 CPU 一份，含 `state_usage`、`last_residency`、
  `enabled`、`registered`。

### 3.2 选择与进入

```c
int cpuidle_select(drv, dev, stop_tick)   /* cpuidle.c:359 */
{
    return cpuidle_curr_governor->select(drv, dev, stop_tick);
}
```

`cpuidle_enter()` 调用所选状态的 `enter(dev, drv, index)`，返回后
`cpuidle_reflect()` 让 governor 修正统计。`tick_nohz_idle_enter()` 等
空闲循环入口在 `kernel/sched/idle.c`，通过 `cpuidle_select/enter` 接入。

### 3.3 governor 与驱动注册

- governor：`drivers/cpuidle/governors/` 下 `menu.c`（tickless 默认）、
  `teo.c`（timer events oriented）、`ladder.c`（周期 tick）、`haltpoll.c`。
  `cpuidle_register_governor()`（governor.c:79）按 `rating` 选最高者，
  `cpuidle_switch_governor()`（47）切换。
- 驱动：`cpuidle_register_driver()`（driver.c）注册 `cpuidle_driver`；
  若命令行未指定 governor 且驱动给了 `drv->governor`，尝试切换。
  `cpuidle_register_device()`（cpuidle.c:675）把 `cpuidle_device` 加入 sysfs 与
  CPU hotplug；`cpuidle_enable_device()` 回调 governor 的 `enable()`。

ARM64 常见驱动是 `drivers/cpuidle/cpuidle-psci.c`，它与 PSCI 及 genpd 拓扑配合，
详见第 8 节。当前 `.config` 未启用 `CONFIG_CPU_IDLE`，因此
`cpuidle-psci.c`、`cpuidle-arm.c` 等源码存在但不会编译。

## 4. OPP（Operating Performance Points）

### 4.1 概念

OPP 描述设备在某个频率下所需的电压、带宽、时钟延迟等资源，是 cpufreq、
devfreq、genpd 共用的“性能点”抽象。`drivers/opp/opp.h:108`：

```c
struct dev_pm_opp {
    struct list_head node;
    struct kref kref;
    bool available, dynamic, turbo, suspend, removed;
    unsigned long *rates;          /* 每路时钟的频率 */
    unsigned int level;
    struct dev_pm_opp_supply *supplies;   /* 每路 regulator 的电压 */
    struct dev_pm_opp_icc_bw *bandwidth;
    unsigned long clock_latency_ns;
    struct dev_pm_opp **required_opps;    /* 依赖的其他设备 OPP */
    struct opp_table *opp_table;
};
```

`struct opp_table` 挂在设备上，保存 `clk`、regulators、`opp_list`、
`rate_clk_single`、`current_rate`、`supported_hw`、`prop_name` 等。

### 4.2 注册与设置

- `dev_pm_opp_add_dynamic(dev, data)`（`drivers/opp/core.c:2837`）：
  向设备的 opp_table 添加一个动态 OPP；`dev_pm_opp_remove()`（1798）删除。
- `dev_pm_opp_of_add_table(dev)`（`drivers/opp/of.c:1159`）：解析 DT
  `operating-points-v2`、`opp-hz`、`opp-microvolt`、`opp-supported-hw`、
  `required-opps` 等属性，建立完整表。
- `dev_pm_opp_set_rate(dev, target_freq)`（core.c:1413）：
  `_find_opp_table()` 找表；空表（只有 clk）时直接 `config_clks()` 调 `clk_set_rate()`；
  否则 `clk_round_rate()` 后 `_find_freq_ceil()` 找到 OPP，再 `_set_opp()`：
  按顺序设置 regulator 电压、时钟频率、带宽，并处理 `required_opps` 的级联。
- `dev_pm_opp_set_opp(dev, opp)`（1481）：直接按 OPP 设置；
  `dev_pm_opp_adjust_voltage()`（2930）用于电压微调。
- `dev_pm_opp_remove_table()`（3105）在驱动解绑时清空。

当前 `.config`：`CONFIG_PM_OPP=y`。OPP 本身不依赖 `CONFIG_PM`，因此
`dev_pm_opp_set_rate()` 可用于纯时钟/regulator 场景（如 UFS、GPU）。
`CONFIG_PM_DEVFREQ` 未启用，devfreq 侧源码存在但未编译。

## 5. genpd / pmdomain

### 5.1 数据结构

`include/linux/pm_domain.h:195` 的 `struct generic_pm_domain` 关键字段：
`dev`、`domain`（`dev_pm_domain`）、`gpd_list_node`、`parent_links/child_links`、
`dev_list`、`gov`、`power_off_work`、`name`、`sd_count`（处于 on 的子域数）、
`status`、`device_count`、`performance_state`、`cpus`、`power_on/power_off`、
`power_notifiers`、`opp_table`、`set_performance_state()`、
`attach_dev/detach_dev`、`states/state_count/state_idx`。
`struct gpd_link` 记录父子域关系；`struct gpd_timing_data` 记录
`suspend_latency_ns/resume_latency_ns` 等约束。

### 5.2 注册与开关

- `pm_genpd_init(genpd, gov, is_off)`（`drivers/pmdomain/core.c:2395`）：
  初始化链表、锁、`sd_count`、`status`，并填充
  `genpd->domain.ops.runtime_suspend = genpd_runtime_suspend`、
  `runtime_resume`、`prepare`、`suspend_noirq`、`resume_noirq`、
  `freeze_noirq` 等，使 genpd 可作为设备的 `dev_pm_domain`。
- `pm_genpd_add_subdomain(parent, child)`（2205）：建立 `gpd_link`，
  子域开关会级联影响父域引用计数。
- `_genpd_power_on()`（826）执行 `genpd->power_on()` 并统计 `on_time`；
  `genpd_power_on(genpd, depth)`（1038）先递归打开父域，再打开自身；
  `genpd_power_off()` 反向，全部子域/设备空闲才真正断电。
- `dev_pm_genpd_set_performance_state(dev, state)`（579）：聚合设备性能状态，
  通过 `set_performance_state()` 下发给硬件；可结合 `opp_table` 选电压/频率。

### 5.3 与设备模型衔接

`dev_pm_domain_attach(dev, flags)`（`drivers/base/power/common.c:103`）：
若 `dev->pm_domain` 已存在直接返回；否则先 `acpi_dev_pm_attach()`，
再 `genpd_dev_pm_attach()`；成功后按 `PD_FLAG_DETACH_POWER_OFF` 设置
`dev->power.detach_power_off`。

platform/I2C/SPI probe 都会调用它；`dev_pm_domain_detach()`（360）在解绑时
按 `detach_power_off` 决定是否断电。当前 `.config` 未启用 `CONFIG_PM`，
`CONFIG_PM_GENERIC_DOMAINS` 未设置，genpd 核心逻辑被条件编译裁掉；
`drivers/pmdomain/core.c` 源码仍完整存在。

## 6. runtime PM

### 6.1 状态与计数

`struct dev_pm_info`（`include/linux/pm.h:666`）在 `CONFIG_PM` 下包含：
`usage_count`（>0 不允许 suspend）、`child_count`、`disable_depth`、
`runtime_auto`、`ignore_children`、`irq_safe`、`use_autosuspend`、
`links_count`、`runtime_status`（RPM_ACTIVE/SUSPENDED/RESUMING/SUSPENDING）、
`autosuspend_delay`、`last_busy` 等。

### 6.2 核心流程

`__pm_runtime_resume(dev, rpmflags)`（`drivers/base/power/runtime.c:1185`）
在带 `RPM_GET_PUT` 时先 `atomic_inc(&dev->power.usage_count)`，
再在 `dev->power.lock` 下调用 `rpm_resume()`。

`rpm_resume()`（786）：若 `runtime_error` 返回 `-EINVAL`；`disable_depth > 0`
且状态非 ACTIVE 返回 `-EACCES`；取消 pending 请求；已 ACTIVE 返回 1；
若正在 SUSPENDING/RESUMING，按 flags 等待或返回 `-EINPROGRESS`；
否则置 `RPM_RESUMING`，递归唤醒 parent，调用 `runtime_resume` 回调，
成功后置 `RPM_ACTIVE` 并唤醒等待者。

`rpm_suspend()`（586）对称：usage_count 非 0、child_count 非 0、
`ignore_children` 等条件会阻止挂起；满足条件后置 `RPM_SUSPENDING`，
调用 `runtime_suspend`，再 `rpm_idle()`（491）判断是否继续 autosuspend。

`pm_runtime_get_sync()` 是 `__pm_runtime_resume(dev, RPM_GET_PUT)` 的内联封装；
`pm_runtime_put()`/`pm_runtime_put_autosuspend()` 减少计数并按需调度 idle。
`pm_runtime_enable()`（1556）减少 `disable_depth`，`pm_runtime_disable()` 增加。

### 6.3 当前配置

`CONFIG_PM` 未启用时，`include/linux/pm_runtime.h` 提供内联桩：
`__pm_runtime_resume()` 返回 1（视为成功），`__pm_runtime_idle()` /
`__pm_runtime_suspend()` 返回 `-ENOSYS`，`pm_runtime_enable()` 为空操作。
因此驱动代码无需 `#ifdef`，但实际不会进入低功耗。源码
`drivers/base/power/runtime.c` 完整存在。

## 7. system suspend / hibernate

### 7.1 用户入口

`/sys/power/state` 的 `state_store()`（`kernel/power/main.c:799`）解析字符串：

- `freeze` → `PM_SUSPEND_TO_IDLE`
- `mem` → `mem_sleep_current`（`s2idle` 或 `deep`）
- `disk` → `PM_SUSPEND_MAX`，走 `hibernate()`

普通 suspend 调用 `pm_suspend(state)`（`kernel/power/suspend.c:636`），
进一步 `enter_state()`（576）。

### 7.2 suspend 流程

```text
enter_state(state)
├─ system_transition_mutex；s2idle_begin()；pm_sleep_fs_sync()
├─ suspend_prepare(): 冻结进程、设备 prepare、通知链
├─ suspend_devices_and_enter():
│  platform_suspend_begin → console_suspend_all
│  → dpm_suspend_start(PMSG_SUSPEND)（prepare/suspend/late/noirq）
│  → suspend_enter() 循环 → dpm_resume_end(PMSG_RESUME)
└─ suspend_finish(): 解冻进程、文件系统 thaw、PM_POST_SUSPEND
```

设备回调按 `struct dev_pm_ops`（`include/linux/pm.h:288`）分阶段：
`prepare/complete`、`suspend/resume`、`suspend_late/resume_early`、
`suspend_noirq/resume_noirq`、`freeze/thaw/poweroff/restore` 及对应
`_late/_noirq`。`dpm_suspend_start()`（`drivers/base/power/main.c:2334`）
负责按设备树/依赖顺序遍历，失败会回滚已挂起设备（`dpm_resume_end`）。

### 7.3 hibernate 流程

`hibernate()`（`kernel/power/hibernate.c:760`）：

1. `hibernation_available()` 检查 swap 设备与平台支持。
2. `lock_system_sleep()`、`hibernate_acquire()` 防止并发。
3. `pm_notifier_call_chain(PM_HIBERNATION_PREPARE)`、`pm_sleep_fs_sync()`、
   `filesystems_freeze()`、`freeze_processes()`。
4. `create_basic_memory_bitmaps()`。
5. `hibernation_snapshot(platform_mode)`：`dpm_suspend_start(PMSG_FREEZE)`，
   在 `snapshot.c` 中分配/复制内存镜像并做页表与 CRC 处理。
6. 若 `in_suspend`，把镜像写入 swap（`swap.c`），再 `power_down()`。
7. 恢复时 `software_resume()`（1013）读回镜像、`hibernation_restore()`，
   解冻进程与文件系统。

### 7.4 当前配置

当前 `.config`：`# CONFIG_SUSPEND is not set`、`# CONFIG_PM is not set`，
`CONFIG_HIBERNATION` 也不会启用。因此 `kernel/power/` 下 `suspend.c`、
`hibernate.c`、`snapshot.c`、`swap.c`、`main.c` 的电源管理部分不会被编译；
源码完整存在于树中，不存在“内核缺少 suspend 实现”的问题。

## 8. PSCI OS-initiated（OSI）

### 8.1 固件能力探测

`drivers/firmware/psci/psci.c`：

- `psci_cpu_suspend_feature` 保存 `PSCI_FEATURES(CPU_SUSPEND)` 返回值；
  `psci_has_osi_support()`（89）返回
  `psci_cpu_suspend_feature & PSCI_1_0_OS_INITIATED`。
- `psci_set_osi_mode(enable)`（161）按 enable 选择
  `PSCI_1_0_SUSPEND_MODE_OSI` 或 `PSCI_1_0_SUSPEND_MODE_PC`，
  调用 `PSCI_1_0_FN_SET_SUSPEND_MODE` 并经 `psci_to_linux_errno()` 转换返回值。

PSCI 调用经 `arm_smccc_smc()` 或 `arm_smccc_hvc()`（`__invoke_psci_fn_smc/hvc`）。
`psci_cpu_suspend()` 最终把状态与入口地址传给固件。

### 8.2 OSI 与 PC 的区别

- **PC（Platform Coordinated）**：OS 只提交“本 CPU 要进入的层级”，
  平台协调器负责整簇/系统级掉电，OS 对共享电源域控制较弱。
- **OSI（OS Initiated）**：OS 负责按 CPU 拓扑逐级决策，把状态描述交给 PSCI；
  需要 genpd 层次（cluster/core）与 cpuidle-psci 配合，能更精细地控制
  共享电源域与唤醒延迟。

`drivers/cpuidle/cpuidle-psci-domain.c:130` 的
`psci_cpuidle_domain_probe()`：

1. `use_osi = psci_has_osi_support()`。
2. 遍历 DT 子节点，凡有 `#power-domain-cells` 的节点调用 `psci_pd_init()`
   创建 genpd provider。
3. `dt_idle_pd_init_topology()` 按 DT 建立父子域。
4. `psci_set_osi_mode(use_osi)` 切到 OSI 或 PC 模式，并打印
   `"Initialized CPU PM domain topology using OSI/PC mode"`。

`cpuidle-psci.c` 则注册 `cpuidle_driver`，其 `enter` 回调调用
`psci_cpu_suspend()`；genpd 的 `power_on/off` 负责域级开关。
当前 `.config`：`CONFIG_ARM_PSCI_FW=y`，但 `CONFIG_CPU_IDLE` 未启用，
因此 `cpuidle-psci.c` / `cpuidle-psci-domain.c` 不会编译，
`psci_set_osi_mode()` 也因缺少调用者而不执行。PSCI 源码完整存在。

## 9. 配置矩阵

| 配置项 | 当前值 | 影响 |
|--------|--------|------|
| `CONFIG_CPU_FREQ` | y | cpufreq 核心、policy、sysfs 生效 |
| `CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL` | y | 默认调频器为 schedutil |
| `CONFIG_CPU_FREQ_GOV_*` | y | performance/powersave/userspace/ondemand/conservative 均编译 |
| `CONFIG_CPU_IDLE` | 未启用 | cpuidle 核心与 governor、cpuidle-psci 不编译 |
| `CONFIG_PM` | 未启用 | runtime PM、system suspend、genpd 核心被裁剪 |
| `CONFIG_SUSPEND` | 未启用 | `/sys/power/state` 的 suspend 路径不编译 |
| `CONFIG_HIBERNATION` | 未启用 | hibernate/snapshot/swap 不编译 |
| `CONFIG_PM_OPP` | y | OPP 表与 `dev_pm_opp_set_rate()` 可用 |
| `CONFIG_PM_DEVFREQ` | 未启用 | devfreq 源码存在但不编译 |
| `CONFIG_ARM_PSCI_FW` | y | PSCI 调用、`psci_set_osi_mode()` 存在 |
| `CONFIG_ENERGY_MODEL` | 未启用 | 能效模型、EAS 相关代码不编译 |

## 10. 关键调用链

```text
cpufreq 初始化
cpufreq_register_driver()
  ├─ 校验回调、设置 cpufreq_driver
  ├─ subsys_interface_register(&cpufreq_interface)
  │    └─ cpufreq_add_dev() → cpufreq_online()
  │         ├─ driver->init(policy)
  │         └─ cpufreq_init_policy()
  │              └─ cpufreq_set_policy()
  │                   ├─ driver->verify()
  │                   ├─ __resolve_freq()
  │                   └─ governor start / driver->setpolicy()
  └─ cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN)

schedutil 调频
调度器 util 变化 → cpufreq_update_util()
  → sugov_update_single_freq()/sugov_update_shared()
  → sugov_next_freq_shared()
  → cpufreq_driver_target()
     → __cpufreq_driver_target()
        → target_index() / fast_switch()

runtime PM
pm_runtime_get_sync()
  → __pm_runtime_resume(RPM_GET_PUT)
     ├─ usage_count++
     └─ rpm_resume()
        ├─ 唤醒 parent
        ├─ runtime_resume()
        └─ RPM_ACTIVE

system suspend
echo mem > /sys/power/state
  → state_store() → pm_suspend(PM_SUSPEND_MEM)
     → enter_state()
        ├─ suspend_prepare()
        ├─ suspend_devices_and_enter()
        │    ├─ dpm_suspend_start(PMSG_SUSPEND)
        │    └─ suspend_enter()
        └─ suspend_finish()

PSCI OSI
psci_cpuidle_domain_probe()
  ├─ psci_has_osi_support()
  ├─ psci_pd_init() / dt_idle_pd_init_topology()
  └─ psci_set_osi_mode()
       → PSCI_1_0_FN_SET_SUSPEND_MODE(OSI/PC)
```

## 11. 关键文件清单

| 路径 | 内容 |
|------|------|
| `include/linux/cpufreq.h` | `cpufreq_policy`、`cpufreq_driver`、governor 接口 |
| `drivers/cpufreq/cpufreq.c` | 驱动注册、policy 生命周期、`__cpufreq_driver_target()` |
| `drivers/cpufreq/cpufreq_governor.c` | governor 通用框架（on-demand/conservative） |
| `kernel/sched/cpufreq_schedutil.c` | schedutil 调频器 |
| `drivers/cpuidle/cpuidle.c` | `cpuidle_select/enter`、设备注册 |
| `drivers/cpuidle/governor.c` | governor 注册与选择 |
| `drivers/cpuidle/governors/menu.c` | menu governor |
| `drivers/cpuidle/governors/teo.c` | teo governor |
| `drivers/cpuidle/cpuidle-psci.c` | PSCI cpuidle 驱动 |
| `drivers/cpuidle/cpuidle-psci-domain.c` | PSCI genpd 拓扑与 OSI 切换 |
| `drivers/opp/core.c` | OPP 表、`dev_pm_opp_set_rate()` |
| `drivers/opp/of.c` | DT `operating-points-v2` 解析 |
| `drivers/opp/opp.h` | `dev_pm_opp`、`opp_table` |
| `drivers/pmdomain/core.c` | `pm_genpd_init()`、域开关与性能状态 |
| `include/linux/pm_domain.h` | `generic_pm_domain`、`gpd_link` |
| `drivers/base/power/common.c` | `dev_pm_domain_attach/detach()` |
| `drivers/base/power/runtime.c` | runtime PM 状态机 |
| `drivers/base/power/main.c` | system PM 设备遍历 `dpm_*` |
| `include/linux/pm.h` | `dev_pm_info`、`dev_pm_ops`、`dev_pm_domain` |
| `kernel/power/main.c` | `/sys/power/state` 与 autosleep |
| `kernel/power/suspend.c` | `enter_state()`、`suspend_devices_and_enter()` |
| `kernel/power/hibernate.c` | `hibernate()`、快照与恢复 |
| `kernel/power/snapshot.c` / `swap.c` | 内存镜像与 swap 读写 |
| `drivers/firmware/psci/psci.c` | PSCI 调用、`psci_set_osi_mode()` |
| `include/linux/psci.h` | PSCI 版本、功能号与接口声明 |
