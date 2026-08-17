# start_kernel 流程

## 概述

由 `head.S` 的 `__primary_switched` 跳转到 `start_kernel` 函数，开始执行架构无关的内核初始化流程。`start_kernel` 位于 `init/main.c`。

## 启动流程总览

```
start_kernel()                                                    [init/main.c]
│
│  ========== 阶段一: 基础环境初始化（关中断） ==========
│
├─ set_task_stack_end_magic(&init_task)
│     在 init_task 栈顶设置 STACK_END_MAGIC，用于检测栈溢出
│
├─ smp_setup_processor_id()
│     读取并保存 core 0 的 cpuid（ARM64: 从 MPIDR 寄存器读取）
│     设置 __cpu_logical_map[0] = mpidr
│
├─ debug_objects_early_init()
│     初始化 obj_hash 和 obj_static_pool，用于调试对象跟踪
│     ├─ raw_spin_lock_init() —— 初始化 obj_hash 表每个 bucket 的 spinlock
│     └─ hlist_add_head() —— 将 obj_static_pool[i].node 加入 obj_pool
│
├─ init_vmlinux_build_id()
│     从 .note.gnu.build-id 段查找 build ID
│
├─ cgroup_init_early()
│     cgroups 基本初始化
│     ├─ init_cgroup_root() —— 初始化 cgroup 树 root
│     └─ cgroup_init_subsys() —— 初始化 subsystem
│
├─ local_irq_disable()
│     关中断（early_boot_irqs_disabled = true）
│
├─ boot_cpu_init()
│     把 boot CPU 添加到 online/present/active/possible CPU mask
│
├─ page_address_init()
│     HIGHMEM 页表初始化（64 位 CPU 为空函数）
│
├─ pr_notice(linux_banner)
│     打印内核版本信息（如 "Linux version 7.0.0 ..."）
│
│  ========== 阶段二: 架构相关初始化（setup_arch） ==========
│
├─ setup_arch(&command_line)                                       [arch/arm64/kernel/setup.c]
│  │  根据 BootLoader 传递的参数收集系统硬件信息
│  │
│  ├─ setup_initial_init_mm(_text, _etext, _edata, _end)
│  │     初始化 init 进程的内存描述符（mm_struct）
│  │
│  ├─ kaslr_init()
│  │     内核地址空间布局随机化初始化
│  │     ├─ 检查 cmdline 是否禁用 kaslr
│  │     └─ 检查 kaslr_offset 是否 >= MIN_KIMG_ALIGN
│  │
│  ├─ early_fixmap_init()
│  │     初始化 fixmap 区域的 L0/L1/L2 页表 entry
│  │     ├─ early_fixmap_init_pud() —— 初始化 PUD 级页表
│  │     │   ├─ [5 级页表] __p4d_populate() → set_p4d()
│  │     │   └─ pud_offset_kimg() —— 获取 PUD 地址
│  │     ├─ early_fixmap_init_pmd() —— 初始化 PMD 级页表
│  │     │   └─ __pud_populate() → set_pud() → set_swapper_pgd()
│  │     └─ early_fixmap_init_pte() —— 初始化 PTE 级页表
│  │         └─ __pmd_populate() → set_pmd()
│  │
│  ├─ early_ioremap_init()
│  │     初始化 7 个虚拟地址 slot，每个 slot 指向一段 fixmap 区域
│  │     └─ early_ioremap_setup() —— 循环初始化 slot
│  │
│  ├─ setup_machine_fdt(__fdt_pointer)
│  │     映射并解析 FDT（Flattened Device Tree）
│  │     ├─ fixmap_remap_fdt() —— 将 FDT 映射到 fixmap 的 PTE
│  │     │   ├─ create_mapping_noalloc() —— 映射第一个 chunk 读取 header
│  │     │   │   └─ __create_pgd_mapping() → alloc_init_p4d() → alloc_init_pud()
│  │     │   │       → alloc_init_cont_pmd() → alloc_init_cont_pte() → __set_pte_nosync()
│  │     │   ├─ fdt_size() —— 从 FDT header 获取 size
│  │     │   └─ create_mapping_noalloc_reset() —— 映射剩余数据
│  │     ├─ memblock_reserve() —— memblock reserve FDT 物理地址
│  │     │   └─ memblock_add_range() → memblock_insert_region()
│  │     ├─ early_init_dt_scan() —— 扫描设备树
│  │     ├─ fixmap_remap_fdt() —— 映射完成后设为只读，防止 FDT 被修改
│  │     ├─ of_flat_dt_get_machine_name() —— 从 FDT 查找 machine 信息
│  │     └─ dump_stack_set_arch_desc() —— 设置 arch 描述信息
│  │
│  ├─ jump_label_init()
│  │     排序并初始化 jump label section
│  │     ├─ jump_label_sort_entries() —— 堆排序 jump table entries
│  │     └─ 遍历 jump table:
│  │         ├─ arch_jump_label_transform_static() —— 重写 nop 指令
│  │         ├─ jump_entry_set_init() —— key 设置 init 标志
│  │         └─ static_key_set_entries() —— 初始化 static_key 字段
│  │
│  ├─ parse_early_param()
│  │     解析早期启动参数（如 quiet、console= 等）
│  │
│  ├─ dynamic_scs_init()
│  │     Shadow Call Stack 影子调用栈初始化
│  │     把 FP 和 LR 放入影子栈，防止缓冲区溢出攻击
│  │
│  ├─ local_daif_restore(DAIF_PROCCTX_NOIRQ)
│  │     恢复 DAIF 状态: mask IRQ/FIQ，unmask Debug/SError
│  │
│  ├─ cpu_uninstall_idmap()
│  │     取消 identity mapping 的 TTBR0 映射，防止旁路攻击
│  │     ├─ cpu_set_reserved_ttbr0() —— TTBR0 设置空页
│  │     ├─ local_flush_tlb_all() —— 刷 TLB
│  │     ├─ cpu_set_default_tcr_t0sz() —— 确保 T0SZ 设置正确
│  │     └─ cpu_switch_mm() → cpu_do_switch_mm() —— 更新 TTBR0/TTBR1
│  │
│  ├─ xen_early_init()
│  │     裸机虚拟化（Xen）早期初始化
│  │
│  ├─ efi_init()
│  │     EFI 初始化，根据 EFI 表构造 memory map
│  │     ├─ efi_get_fdt_params() —— 从 FDT 取出 EFI 信息
│  │     │   └─ efi_get_fdt_prop() → fdt_getprop()
│  │     ├─ efi_memmap_init_early() —— 映射 EFI data
│  │     │   └─ __efi_memmap_init() → early_memremap() → __early_ioremap()
│  │     ├─ uefi_init()
│  │     │   ├─ early_memremap_ro() —— 只读映射 header
│  │     │   ├─ efi_systab_check_header() —— 校验签名
│  │     │   ├─ efi_systab_report_header() —— 打印 EFI header 信息
│  │     │   ├─ early_memremap_ro() —— 只读映射 body
│  │     │   └─ efi_config_parse_tables() —— 解析 EFI table
│  │     │       └─ early_memunmap() → early_iounmap() → __late_clear_fixmap()
│  │     ├─ reserve_regions() —— 保留 EFI 区域
│  │     ├─ early_init_dt_check_for_usable_mem_range()
│  │     ├─ efi_find_mirror()
│  │     ├─ efi_esrt_init()
│  │     ├─ efi_mokvar_table_init()
│  │     └─ memblock_reserve()
│
│  ├─ [if !efi_enabled(EFI_BOOT)] 检查 kernel 对齐和 MMU 状态
│  │     ├─ 检查 _text % MIN_KIMG_ALIGN == 0
│  │     │    非对齐则打印 FW_BUG 警告
│  │     └─ WARN_TAINT(mmu_enabled_at_boot, ...)
│  │          如果启动时 MMU 已开启（EL2 透传场景），标记 TAINT_FIRMWARE_WORKAROUND
│  │
│  ├─ arm64_memblock_init()
│  │     内存块初始化: 移除 no-map 区域，reserve 内核/FDT/ramdisk 等内存
│  │     ├─ memblock_remove() —— 移除超过支持范围的物理地址
│  │     ├─ memblock_remove() —— 移除 linear region 外的物理地址
│  │     ├─ memblock_remove() —— 移除 memstart 前的物理地址
│  │     ├─ memblock_mem_limit_remove_map() —— 如存在 limit 则移除超限 region
│  │     ├─ memblock_add() —— 如有 limit，把 kernel region 重新加回来
│  │     ├─ memblock_reserve() —— 把 kernel 加入 reserve
│  │     └─ early_init_fdt_scan_reserved_mem() —— 扫描 FDT 中 reserved memory
│  │
│  ├─ paging_init()
│  │     页表初始化
│  │     ├─ map_mem() —— 映射 memblock 中的物理内存
│  │     │   ├─ arm64_kfence_alloc_pool() —— KFENCE 内存池分配
│  │     │   ├─ memblock_mark_nomap() —— 标记 kernel start/end 为 nomap
│  │     │   ├─ for_each_mem_range() → __map_memblock() —— 映射到 linear region
│  │     │   ├─ __map_memblock() —— kernel start/end alias 映射并设为 RO
│  │     │   ├─ memblock_clear_nomap() —— 清除 kernel start/end nomap 标记
│  │     │   └─ arm64_kfence_map_pool() —— 映射 KFENCE pool
│  │     ├─ memblock_allow_resize() —— 允许 memblock resize
│  │     ├─ create_idmap() —— 创建 identity mapping
│  │     │   ├─ __pi_map_range() —— 映射 __idmap_text_start ~ __idmap_text_end
│  │     │   └─ __pi_map_range() —— 映射 __idmap_kpti_flag
│  │     └─ declare_kernel_vmas()
│  │         ├─ declare_vma(.text)
│  │         ├─ declare_vma(.rodata)
│  │         ├─ declare_vma(.init.text)
│  │         ├─ declare_vma(.init.data)
│  │         └─ declare_vma(.data)
│  │
│  ├─ acpi_table_upgrade()
│  │     ACPI 表升级（服务器场景）
│  │
│  ├─ acpi_boot_table_init()
│  │     一般启用 FDT 后会 disable ACPI
│  │
│  ├─ [if acpi_disabled] unflatten_device_tree()
│  │     将 FDT 转换为 device_node 树结构
│  │
│  ├─ bootmem_init()
│  │     Boot memory 初始化，把 FDT 中的 memory node 转换为 memblock 并 reserve
│  │
│  ├─ kasan_init()
│  │     KASAN 内存检测工具初始化（需要编译器支持）
│  │
│  ├─ request_standard_resources()
│  │     请求标准资源（如 PCI、USB 等）
│  │
│  ├─ early_ioremap_reset()
│  │     重置 early ioremap 的 slot
│  │
│  ├─ [if acpi_disabled] psci_dt_init()
│  │     PSCI 电源管理接口初始化（设备树方式）
│  ├─ [else] psci_acpi_init()
│  │     PSCI 初始化（ACPI 方式）
│  │
│  ├─ arm64_rsi_init()
│  │     RSI（Realm Management）初始化
│  │
│  ├─ init_bootcpu_ops()
│  │     初始化 boot CPU 操作函数
│  │
│  ├─ smp_init_cpus()
│  │     扫描 FDT/ACPI 中的 CPU 节点，初始化 SMP CPU 信息
│  │
│  └─ smp_build_mpidr_hash()
│        预计算 MPIDR 各 affinity level 的移位值，用于构建线性索引
│
│  ├─ [if CONFIG_ARM64_SW_TTBR0_PAN] 设置 init_task TTBR0
│  │     init_task.thread_info.ttbr0 = phys_to_ttbr(reserved_pg_dir)
│  │     确保 init 线程的 TTBR0 始终产生缺页，防止 uaccess_enable() 被误调用
│  │
│  └─ [if boot_args[1..3] 非零] 打印启动协议违规警告
│        BootLoader 传递的 x1-x3 非零，说明违反 arm64 启动协议
│
│  ========== 阶段三: 核心子系统初始化（部分仍关中断） ==========
│
├─ mm_core_init_early()                                            [init/main.c]
│     内存管理核心早期初始化（mm_struct、pgdat 等）
│
├─ jump_label_init()
│     如果 setup_arch 中已调用则跳过（检查 static_key_initialized）
│     ├─ jump_label_sort_entries() —— 堆排序
│     └─ 遍历 jump table: 重写 nop 指令、设置 init 标志
│
├─ static_call_init()
│     静态调用初始化（函数指针调用优化）
│
├─ early_security_init()
│     早期安全模块初始化
│
├─ setup_boot_config()
│     获取启动配置（bootconfig）
│
├─ setup_command_line(command_line)
│     保存命令行参数到 saved_command_line 等全局变量
│
├─ setup_nr_cpu_ids()
│     设置 nr_cpu_ids 变量
│
├─ setup_per_cpu_areas()
│     为每个 CPU 分配 per-CPU 内存，拷贝 .data..percpu 段数据
│     ARM64: per-CPU 基址通过 tpidr_el1 寄存器访问
│
├─ smp_prepare_boot_cpu()
│     架构相关 boot CPU 钩子函数
│
├─ early_numa_node_init()
│     NUMA 节点早期初始化
│
├─ boot_cpu_hotplug_init()
│     设置 boot CPU 热插拔状态
│
├─ print_kernel_cmdline(saved_command_line)
│     打印内核命令行参数
│
├─ parse_early_param()
│     解析早期参数（可能影响 static key）
│
├─ parse_args("Booting kernel", ...)
│     解析内核启动参数
│     ├─ print_unknown_bootoptions() —— 打印未知的启动选项
│     ├─ parse_args("Setting init args", ...) —— 设置 init 进程参数
│     └─ parse_args("Setting extra init args", ...) —— 设置额外 init 参数
│
├─ random_init_early(command_line)
│     架构相关及非时间相关的随机数初始化（早于分配器初始化）
│
├─ setup_log_buf(0)
│     为 console log buffer 申请内存
│
├─ vfs_caches_init_early()
│     申请 dentry cache、inode hash 表缓存
│
├─ sort_main_extable()
│     排序内核异常表
│     异常表保存内核访问用户空间地址的指令和 fixup 地址
│     发生 page fault 时据此判断是内核访问越界还是系统调用传递的地址错误
│     例: copy_{to,from}_user()
│
├─ trap_init()
│     系统保留中断向量初始化
│
├─ mm_core_init()
│     内存管理核心初始化（slab 分配器、内存调试模块等）
│
├─ maple_tree_init()
│     红黑树替代数据结构 maple tree 初始化
│
├─ poking_init()
│     内核代码动态修改基础设施初始化
│
├─ ftrace_init()
│     函数跟踪器初始化
│
├─ early_trace_init()
│     tracepoint trace_printk ring buffer 申请
│
│  ========== 阶段四: 调度器与时间子系统初始化 ==========
│
├─ sched_init()
│     调度器初始化（此时尚无中断，中断后才有完整拓扑）
│
├─ radix_tree_init()
│     基数树初始化（用于页缓存等）
│
├─ housekeeping_init()
│     未绑定 CPU 的 workqueue/timer/kthread 等 CPU 属性管理
│
├─ workqueue_init_early()
│     工作队列早期初始化（允许创建和取消 work item，但执行依赖 kthread）
│
├─ rcu_init()
│     RCU 锁机制初始化
│
├─ kvfree_rcu_init()
│     异步 RCU 回收初始化
│
├─ trace_init()
│     跟踪事件系统初始化（trace events 可用）
│
├─ [if initcall_debug] initcall_debug_enable()
│     启用 initcall 调试
│
├─ context_tracking_init()
│     上下文跟踪初始化（用于 NO_HZ 和 RCU）
│
│  ========== 阶段五: 中断与时钟初始化 ==========
│
├─ early_irq_init()
│     IRQ 中断描述符表初始化
│
├─ init_IRQ()
│     架构相关中断控制器初始化
│
├─ tick_init()
│     时钟事件设备通知机制初始化
│
├─ rcu_init_nohz()
│     NO_HZ 模式 RCU 初始化
│
├─ timers_init()
│     低精度定时器初始化
│
├─ srcu_init()
│     SRCU（Sleepable RCU）初始化
│
├─ hrtimers_init()
│     高精度定时器初始化
│
├─ softirq_init()
│     软中断初始化
│
├─ timekeeping_init()
│     时钟源和时间计量初始化
│
├─ time_init()
│     时间戳计数器初始化（ARM64: 系统计时器）
│
├─ random_init()
│     随机数子系统初始化（必须在 timekeeping 之后）
│
├─ kfence_init()
│     KFENCE 内核内存错误检测器初始化
│
├─ boot_init_stack_canary()
│     栈溢出保护: 给当前 boot task 设置 canary
│     依赖编译器 -fstack-protector 特性，函数入口设置 canary，返回时检查
│
├─ perf_event_init()
│     性能事件子系统初始化
│
├─ profile_init()
│     内核剖析初始化
│
├─ call_function_init()
│     SMP 函数调用机制初始化
│
│  ========== 阶段六: 开中断后初始化 ==========
│
├─ local_irq_enable()
│     开中断（early_boot_irqs_disabled = false）
│
├─ kmem_cache_init_late()
│     slab 分配器后期初始化
│
├─ console_init()
│     控制台初始化（尽早打开，以便输出错误信息）
│
├─ lockdep_init()
│     死锁检测初始化
│
├─ locking_selftest()
│     锁自检（需要 irq 开启，测试 hard/soft irq 开关反转）
│
├─ [if initrd 被覆盖] 禁用 initrd
│     检查 initrd 是否被覆盖
│
├─ setup_per_cpu_pageset()
│     为每个 CPU 初始化页分配器的 pageset
│
├─ numa_policy_init()
│     NUMA 内存分配策略初始化（从哪个 node 分配内存）
│
├─ acpi_early_init()
│     高级电源管理早期初始化
│
├─ [if late_time_init] late_time_init()
│     后期时间初始化钩子
│
├─ sched_clock_init()
│     调度时钟初始化
│
├─ calibrate_delay()
│     校准延时循环（通过 busy loop 测量 CPU 频率）
│
├─ arch_cpu_finalize_init()
│     架构相关 CPU 最终初始化
│
│  ========== 阶段七: 进程与文件系统初始化 ==========
│
├─ pid_idr_init()
│     PID 分配器 IDR 树初始化
│
├─ anon_vma_init()
│     匿名虚拟内存区域初始化
│
├─ thread_stack_cache_init()
│     线程栈缓存初始化
│
├─ cred_init()
│     进程凭证（credential）初始化
│
├─ fork_init()
│     进程创建子系统初始化（设置最大进程数）
│
├─ proc_caches_init()
│     proc 文件系统相关 slab cache 初始化
│
├─ uts_ns_init()
│     UTS namespace 初始化（主机名等）
│
├─ time_ns_init()
│     时间 namespace 初始化
│
├─ key_init()
│     内核密钥管理初始化
│
├─ security_init()
│     安全框架初始化（LSM 等）
│
├─ dbg_late_init()
│     调试后期初始化
│
├─ net_ns_init()
│     网络 namespace 初始化
│
├─ vfs_caches_init()
│     VFS 缓存初始化（dcache、inode 等）
│
├─ pagecache_init()
│     页缓存初始化
│
├─ signals_init()
│     信号处理相关缓存初始化
│
├─ seq_file_init()
│     顺序文件（seq_file）初始化（procfs/sysfs/debugfs 用）
│
├─ proc_root_init()
│     proc 根文件系统初始化
│
├─ nsfs_init()
│     namespace 文件系统初始化
│
├─ pidfs_init()
│     PID 文件系统初始化
│
├─ cpuset_init()
│     cpuset 子系统初始化
│
├─ mem_cgroup_init()
│     内存 cgroup 初始化
│
├─ cgroup_init()
│     cgroup 完整初始化
│
├─ taskstats_init_early()
│     任务状态统计早期初始化
│
├─ delayacct_init()
│     延迟统计初始化
│
├─ acpi_subsystem_init()
│     ACPI 子系统完整初始化
│
├─ arch_post_acpi_subsys_init()
│     架构相关 ACPI 后处理
│
├─ kcsan_init()
│     内核竞争检测（KCSAN）初始化
│
│  ========== 阶段八: 启动 init 进程 ==========
│
└─ rest_init()                                                          [init/main.c]
    │
    ├─ rcu_scheduler_starting()
    │     RCU 调度器启动
    │
    ├─ pid = user_mode_thread(kernel_init, NULL, CLONE_FS)
    │     创建 kernel_init 进程（pid=1）
    │     │
    │     └─ 创建后立即 pin 到 boot CPU:
    │           ├─ rcu_read_lock()
    │           ├─ tsk = find_task_by_pid_ns(pid, &init_pid_ns)
    │           ├─ tsk->flags |= PF_NO_SETAFFINITY
    │           │   禁止用户空间修改此进程的 CPU affinity
    │           ├─ set_cpus_allowed_ptr(tsk, cpumask_of(smp_processor_id()))
    │           │   将 kernel_init 固定在 boot CPU 上，防止 init 进程迁移
    │           ├─ rcu_read_unlock()
    │           └─ numa_default_policy() —— 设置默认 NUMA 策略
    │
    ├─ pid = kernel_thread(kthreadd, NULL, NULL, CLONE_FS | CLONE_FILES)
    │     创建 kthreadd 进程（pid=2）
    │     统一管理内核线程的创建和销毁
    │     │
    │     └─ 设置 kthreadd_task 全局变量:
    │           ├─ rcu_read_lock()
    │           ├─ kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns)
    │           └─ rcu_read_unlock()
    │
    ├─ system_state = SYSTEM_SCHEDULING
    │     系统状态切换为可调度
    │     在此之后 might_sleep() 和 smp_processor_id() 检查才生效
    │
    ├─ complete(&kthreadd_done)
    │     通知 kthreadd 完成初始化
    │     kthreadd 被阻塞在 wait_for_completion(&kthreadd_done) 上
    │     收到通知后开始创建和管理内核线程
    │
    ├─ schedule_preempt_disabled()
    │     调度一次，让 kernel_init 和 kthreadd 获得运行机会
    │     使 idle 线程得以运行
    │
    └─ cpu_startup_entry(CPUHP_ONLINE)
           idle 进程（pid=0）进入 idle 循环
           ├─ 循环检查是否需要调度
           ├─ 没有任务可运行时执行 WFI（Wait For Interrupt）指令
           └─ CPU 进入低功耗状态，等待中断唤醒
```

## kernel_init 流程（pid=1 入口）

rest_init 创建 kernel_init 线程后，当 `schedule_preempt_disabled()` 调度到它时开始执行：

```
kernel_init(NULL)                                                  [init/main.c]
│
├─ wait_for_completion(&kthreadd_done)
│     等待 kthreadd 完成初始化（kthreadd 在此被阻塞等待）
│
├─ kernel_init_freeable()
│  │  调度器完整初始化前的准备工作
│  │
│  ├─ gfp_allowed_mask = __GFP_BITS_MASK
│  │     允许所有 GFP 类型的阻塞分配
│  │
│  ├─ set_mems_allowed(node_states[N_MEMORY])
│  │     init 可以在任何 NUMA 节点上分配内存
│  │
│  ├─ cad_pid = get_pid(task_pid(current))
│  │     设置 Ctrl-Alt-Del 操作的 PID
│  │
│  ├─ smp_prepare_cpus(setup_max_cpus)
│  │     准备 SMP 其他 CPU 的启动
│  │
│  ├─ workqueue_init()
│  │     工作队列完整初始化（kthread 已就绪，work 可以真正执行）
│  │
│  ├─ init_mm_internals()
│  │     内存管理内部数据结构初始化
│  │
│  ├─ do_pre_smp_initcalls()
│  │     执行 pre-SMP 阶段的 initcall（early_initcall、core_initcall 等）
│  │     ├─ early_initcall 级别的函数
│  │     └─ core_initcall 级别的函数
│  │
│  ├─ lockup_detector_init()
│  │     内核锁死检测器初始化（检测 CPU 长时间不调度）
│  │
│  ├─ smp_init()
│  │     启动其他 CPU core（从 boot CPU 唤醒其他 CPU）
│  │     └─ 调用 arch 相关 cpu_ops 启动其他 core
│  │
│  ├─ sched_init_smp()
│  │     调度器 SMP 拓扑初始化
│  │     └─ 设置 CPU domain、group，建立负载均衡拓扑
│  │
│  ├─ workqueue_init_topology()
│  │     根据 CPU 拓扑重新初始化 workqueue
│  │
│  ├─ async_init()
│  │     异步初始化框架初始化
│  │
│  ├─ padata_init()
│  │     并行数据提交框架初始化
│  │
│  ├─ page_alloc_init_late()
│  │     页分配器后期初始化
│  │
│  ├─ do_basic_setup()
│  │     执行所有 initcall 级别函数
│  │     ├─ do_ctors() —— 运行构造函数
│  │     ├─ do_initcalls() —— 依次执行所有 initcall 函数
│  │     │   ├─ pure_initcall
│  │     │   ├─ core_initcall（如 PCI 子系统初始化）
│  │     │   ├─ postcore_initcall
│  │     │   ├─ arch_initcall
│  │     │   ├─ subsys_initcall
│  │     │   ├─ fs_initcall
│  │     │   ├─ rootfs_initcall
│  │     │   ├─ device_initcall（大多数驱动在此）
│  │     │   └─ late_initcall
│  │     └─ 设备驱动程序在此阶段被探测和初始化
│  │
│  ├─ kunit_run_all_tests()
│  │     运行所有 KUnit 测试
│  │
│  ├─ wait_for_initramfs()
│  │     等待 initramfs 加载完成
│  │
│  ├─ console_on_rootfs()
│  │     确保 rootfs 上控制台已配置
│  │
│  ├─ [if init_eaccess(ramdisk_execute_command) 失败]
│  │     ├─ 清除 ramdisk_execute_command
│  │     └─ prepare_namespace() —— 准备根文件系统
│  │
│  └─ integrity_load_keys()
│        加载完整性校验密钥（IMA/EVM 等）
│
├─ async_synchronize_full()
│     等待所有异步 __init 代码完成
│
├─ system_state = SYSTEM_FREEING_INITMEM
│     系统状态切换为释放 init 内存
│
├─ kprobe_free_init_mem()
│     释放 kprobe 相关的 init 内存
│
├─ ftrace_free_init_mem()
│     释放 ftrace 相关的 init 内存
│
├─ kgdb_free_init_mem()
│     释放 kgdb 相关的 init 内存
│
├─ exit_boot_config()
│     退出 boot config 阶段
│
├─ free_initmem()
│     释放 .init 段内存（init text/data 等）
│     此内存可被回收复用
│
├─ mark_readonly()
│     将内核 .text/.rodata 段设为只读
│     ├─ set_memory_ro() —— 设置只读
│     └─ mark_rodata_ro() —— 确认 rodata 只读
│
├─ system_state = SYSTEM_RUNNING
│     系统进入运行状态
│
├─ rcu_end_inkernel_boot()
│     RCU 结束内核启动阶段
│
├─ do_sysctl_args()
│     处理 sysctl 内核参数
│
├─ [if ramdisk_execute_command] run_init_process(ramdisk_execute_command)
│     尝试执行指定的 init 程序（如 /init）
│
├─ [if execute_command] run_init_process(execute_command)
│     尝试执行内核参数指定的 init 程序（如 /sbin/init）
│
├─ run_init_process("/sbin/init")
│     尝试执行 /sbin/init
│
├─ run_init_process("/etc/init")
│     尝试执行 /etc/init
│
└─ run_init_process("/bin/init")
      尝试执行 /bin/init
      如果以上全部失败，报 panic("No init found")
```

## 当前进程状态

执行完 `rest_init()` 后，系统存在 3 个进程：

| 进程 | PID | 描述 |
|------|-----|------|
| **idle 进程** (swapper) | 0 | 启动时使用的进程，最终进入 idle 循环，CPU 空闲时执行 |
| **kernel_init 进程** | 1 | 用户空间 init 进程的前身，负责启动用户态 init 程序 |
| **kthreadd 进程** | 2 | 内核线程守护进程，接收其他模块请求，统一管理内核线程创建/销毁 |

## 启动阶段划分

| 阶段 | 函数范围 | 说明 |
|------|----------|------|
| 一 | set_task_stack_end_magic ~ boot_cpu_init | 基础环境初始化，关中断 |
| 二 | setup_arch | 架构相关初始化（ARM64 特有） |
| 三 | mm_core_init_early ~ early_trace_init | 核心子系统早期初始化 |
| 四 | sched_init ~ context_tracking_init | 调度器与时间子系统 |
| 五 | early_irq_init ~ call_function_init | 中断与时钟初始化 |
| 六 | local_irq_enable ~ arch_cpu_finalize_init | 开中断后初始化 |
| 七 | pid_idr_init ~ kcsan_init | 进程与文件系统初始化 |
| 八 | rest_init | 启动 init 进程，进入用户态 |

## 关键设计点

1. **中断管理**: 整个早期初始化过程（阶段一~五）都在关中断状态下执行，直到阶段六才 `local_irq_enable()`
2. **setup_arch 的 ARM64 特有工作**: 包含 FDT 映射、EFI 初始化、memblock 管理、paging_init、KASLR、KASAN 等架构相关初始化
3. **`smp_init_cpus` 和 `smp_build_mpidr_hash` 在 setup_arch 内部调用**，不是 `start_kernel` 的直接调用
4. **jump_label_init 可能被调用两次**: 第一次在 `setup_arch` 中，第二次在 `start_kernel` 中，通过 `static_key_initialized` 判断是否已执行
5. **页表切换**: `setup_arch` 中的 `paging_init()` 负责建立完整的页表映射，`cpu_uninstall_idmap()` 取消早期 identity mapping
6. **rest_init 后的系统状态**: idle 进程（pid=0）循环执行 idle，kernel_init 最终执行用户态 `/sbin/init`，kthreadd 管理内核线程
7. **initcall 执行时机**: 真正的驱动和设备初始化发生在 `kernel_init_freeable` → `do_basic_setup` → `do_initcalls()` 中，按优先级从高到低依次执行所有 initcall 函数
8. **SMP 启动时机**: 其他 CPU core 在 `kernel_init_freeable` → `smp_init()` 中才被唤醒，在此之前只有 boot CPU 运行
9. **init 内存释放**: `kernel_init` 中调用 `free_initmem()` 释放 `.init` 段，此内存可被回收用作普通内存
10. **init 进程搜索顺序**: kernel_init 依次尝试执行 `ramdisk_execute_command`（默认 `/init`）→ `execute_command`（内核参数指定）→ `/sbin/init` → `/etc/init` → `/bin/init`，全部失败则 panic