# Linux 内核调试功能总览

## 概述

Linux 内核提供了丰富的调试功能，涵盖从编译时静态检查到运行时动态追踪的全链路工具。这些调试功能帮助开发者定位问题、分析性能瓶颈、验证系统正确性。

## 调试功能分类

### 1. 编译时静态检查

| 功能 | 描述 |
|------|------|
| **Clang 上下文与锁分析 (Context & Locking Analysis)** | Linux 7.0 引入的重要特性，利用 Clang 22+ 编译器在编译时静态检查内核同步原语（自旋锁、互斥锁等）的使用是否正确，提前发现锁错误 |
| **Clang 静态分析支持** | 内核增强对 Clang 静态分析工具的支持，编译阶段发现更多代码缺陷 |
| **`DEBUG_BUGVERBOSE_DETAILED`** | 触发 `BUG()` 或 `WARN()` 时提供更详细的错误信息 |

### 2. 内存错误检测

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| **KASAN (Kernel Address SANitizer)** | 检测内核中的越界访问（out-of-bounds）和释放后使用（use-after-free）等内存错误 | mm/kasan/ |
| **UBSAN (Undefined Behavior SANitizer)** | 检测整数溢出、除零错误等未定义行为 | - |
| **SLUB Debug** | 内存分配器 SLUB 自带调试选项，检测内存损坏、越界访问等 | mm/slub.c |
| kmemleak | 自动检测内存泄漏 | kernel/module/debug_kmemleak.c |
| KCSAN | 检测数据竞争问题 | kernel/kcsan/ |
| DMA 调试 | DMA 操作调试和验证 | kernel/dma/debug.c |

### 3. 并发与同步调试

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| lockdep | 检测死锁和锁滥用 | kernel/locking/lockdep.c |
| spinlock_debug | 自旋锁使用检测 | kernel/locking/spinlock_debug.c |
| mutex-debug | 互斥锁使用检测 | kernel/locking/mutex-debug.c |
| irqflag-debug | 中断标志调试 | kernel/locking/irqflag-debug.c |
| lock_events | 锁事件追踪 | kernel/locking/lock_events.c |
| **`traceoff_on_warning`** | 内核启动参数，触发 `WARN()` 时自动关闭追踪功能，防止调试信息被覆盖 | - |

### 4. 跟踪与性能分析

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| ftrace | 函数级追踪，支持函数入口/出口追踪 | kernel/trace/ftrace.c |
| **perf** | 性能分析工具，基于 `perf_events`，分析 CPU 性能计数器、软件事件、tracepoint 等 | tools/perf/ |
| **Tracepoints** | 内核中静态定义的跟踪点，使用 `TRACE_EVENT` 宏埋点，开销极低 | kernel/trace/trace_events.c |
| **BPF (Berkeley Packet Filter)** | 允许用户在不修改内核源码的情况下注入安全、高性能的代码来观察和修改内核行为 | kernel/bpf/ |
| **`hist_debug`** | 启用 `CONFIG_HIST_TRIGGERS_DEBUG` 后，显示每个事件直方图的内部数据 | kernel/trace/ |
| 函数调用图 | 可视化函数调用关系 | kernel/trace/trace_functions_graph.c |
| 事件追踪 | 系统事件追踪（调度、中断、系统调用等） | kernel/trace/trace_events.c |
| 调度追踪 | 调度器事件追踪 | kernel/trace/trace_sched_switch.c |
| 中断关闭追踪 | 中断关闭延迟分析 | kernel/trace/trace_irqsoff.c |
| 抢占延迟追踪 | 抢占延迟分析 | kernel/trace/trace_preemptirq.c |
| 硬件延迟追踪 | 硬件中断延迟分析 | kernel/trace/trace_hwlat.c |
| 操作系统噪声追踪 | 操作系统噪声分析 | kernel/trace/trace_osnoise.c |

### 5. 动态插桩

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| kprobe | 内核探针，可在任意指令地址设置探测点 | kernel/trace/trace_kprobe.c |
| kretprobe | 内核返回探针，在函数返回时触发 | kernel/trace/trace_kprobe.c |
| uprobe | 用户空间探针，支持用户态程序调试 | kernel/trace/trace_uprobe.c |

### 6. 日志与打印

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| **printk** | 内核基础日志输出函数，配合 `dmesg` 查看内核日志 | kernel/printk/ |
| **动态调试 (Dynamic Debug)** | 运行时动态开启/关闭特定文件、函数或行的 `pr_debug()`/`dev_dbg()` 打印，无需重新编译内核 | Documentation/admin-guide/dynamic-debug-howto.rst |

### 7. 内核调试器

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| KDB | 内核内置调试器，支持命令行交互 | kernel/debug/kdb/ |
| KGDB | 通过 GDB 远程调试内核 | kernel/debug/gdbstub.c |

### 8. Panic 相关调试

#### 8.1 触发 Panic

| 功能 | 描述 |
|------|------|
| **BUG_ON()** | 开发者主动触发的 panic，条件为真时调用 `panic()` |
| **Kernel Oops → Panic** | 通过 `CONFIG_PANIC_ON_OOPS=y` 或内核启动参数 `oops=panic`，将 Oops 升级为 panic |
| **`panic_on_warn`** | 让 `WARN_ON` 警告也触发 panic |
| **SysRq 手动触发** | `echo c > /proc/sysrq-trigger` 人为触发 panic，用于测试崩溃转储机制 |

#### 8.2 控制 Panic 行为

| 功能 | 描述 |
|------|------|
| **`panic_timeout`** | panic 后自动重启的等待秒数。可运行时配置（`/proc/sys/kernel/panic`）、启动参数（`panic=N`）或编译时预设（`CONFIG_PANIC_TIMEOUT=N`，Linux 7.0 新增） |
| **`panic_on_oops`** | 控制发生 Oops 时是否 panic |
| **`panic_print`** | 控制 panic 时打印哪些额外调试信息（如所有 CPU 堆栈、内存信息） |
| **`panic_on_taint`** | 系统被"污染"（tainted）后触发 panic |

#### 8.3 崩溃后诊断与信息收集

| 功能 | 描述 |
|------|------|
| **Kdump + crash** | Kdump 是内核崩溃转储机制，panic 时启动第二个内核保存内存镜像（vmcore）；crash 工具用于分析 vmcore 文件 |
| **pstore / Ramoops** | 利用保留的 RAM 区域存储崩溃日志，重启后可从 `/sys/fs/pstore/` 读取上次崩溃信息 |
| **pvpanic** | 虚拟化场景下，Guest panic 时通知 Host，由 Host 执行预设操作（如保存内存转储或自动重启） |

### 9. 动态调试

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| debugfs | 调试文件系统，提供内核状态查询接口 | fs/debugfs/ |

### 10. 运行时验证

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| Runtime Verification | 运行时行为验证框架 | kernel/trace/rv/ |

### 11. 虚拟化相关调试

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| **kvm_stat** | 检索 KVM 虚拟机运行时统计信息 | - |
| KVM 调试 | KVM 虚拟机调试支持 | arch/arm64/kvm/debug.c |

### 12. 特殊调试功能

| 功能 | 描述 | 代码位置 |
|------|------|----------|
| 调度器调试 | 调度器运行时状态查询 | kernel/sched/debug.c |
| 时间子系统调试 | 时间子系统状态查询 | kernel/time/timekeeping_debug.c |
| 内核符号表 | 内核符号信息管理 | kernel/module/kallsyms.c |

## 调试核心架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                      Kernel Debug Core                             │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     Debug Entry Point                        │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │   │
│  │  │  Breakpoint  │  │   Exception  │  │    NMI       │     │   │
│  │  │   Handler    │  │    Handler   │  │   Handler    │     │   │
│  │  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘     │   │
│  │         │                 │                 │              │   │
│  │         └────────┬────────┴────────┬────────┘              │   │
│  │                  ▼                 ▼                       │   │
│  │  ┌─────────────────────────────────────────────────────┐   │   │
│  │  │            kgdb_handle_exception()                  │   │   │
│  │  │         (kernel/debug/debug_core.c)                 │   │   │
│  │  │                                                     │   │   │
│  │  │  • 保存寄存器状态到 debuggerinfo_struct             │   │   │
│  │  │  • 处理递归调试入口 (exception_level)                │   │   │
│  │  │  • 选择调试模式 (KDB vs KGDB)                       │   │   │
│  │  │  • 协调多 CPU 调试                                   │   │   │
│  │  └──────────────────────┬──────────────────────────────┘   │   │
│  └─────────────────────────┼───────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                      Debug Modes                            │   │
│  │                                                             │   │
│  │  ┌─────────────────────┐     ┌─────────────────────┐       │   │
│  │  │       KDB           │     │       KGDB          │       │   │
│  │  │  (内核内置调试器)    │     │   (GDB 远程调试)     │       │   │
│  │  │                     │     │                     │       │   │
│  │  │  • 命令行交互        │     │  • GDB 协议通信      │       │   │
│  │  │  • 断点管理          │     │  • 源码级调试        │       │   │
│  │  │  • 内存查看          │     │  • 变量检查          │       │   │
│  │  │  • 堆栈追踪          │     │  • 条件断点          │       │   │
│  │  │  • 寄存器查看        │     │  • 远程执行          │       │   │
│  │  │  • 任务管理          │     │                     │       │   │
│  │  └─────────────────────┘     └─────────────────────┘       │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     IO Layer                                │   │
│  │  ┌─────────────────────────────────────────────────────┐   │   │
│  │  │  struct kgdb_io (kernel/debug/debug_core.h)          │   │   │
│  │  │                                                     │   │   │
│  │  │  • read_char() / write_char()                       │   │   │
│  │  │  • read_buffer() / write_buffer()                   │   │   │
│  │  │  • register_kgdb_io() / unregister_kgdb_io()        │   │   │
│  │  │                                                     │   │   │
│  │  │  支持的传输方式:                                     │   │   │
│  │  │  • 串口 (kgdboc)                                    │   │   │
│  │  │  • 以太网 (kgdboe)                                  │   │   │
│  │  │  • USB (kgdbus)                                     │   │   │
│  │  │  • FireWire (kgdb over IEEE 1394)                   │   │   │
│  │  └─────────────────────────────────────────────────────┘   │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 核心数据结构

### debuggerinfo_struct

```
struct debuggerinfo_struct {
    int                     kmagic;         /* Magic number */
    struct pt_regs          *debuggerregs;  /* Saved registers */
    int                     exception_level;/* Recursion level */
    int                     task_state;     /* Task state */
    unsigned long           retpc;          /* Return PC */
    int                     cpu;            /* CPU number */
};
```

该结构保存每个 CPU 的调试状态信息，包括寄存器状态、异常级别等。

### kgdb_io

```
struct kgdb_io {
    const char              *name;          /* IO module name */
    int                     (*read_char)(void);
    void                    (*write_char)(u8);
    int                     (*read_buffer)(char *, int);
    int                     (*write_buffer)(const char *, int);
    void                    (*pre_exception)(void);
    void                    (*post_exception)(void);
    struct list_head        list;
};
```

该结构定义了调试 IO 层的接口，支持多种传输方式。

## 关键函数

### kgdb_handle_exception()

```
int kgdb_handle_exception(int ex_vector, int signo, int err_code,
                          struct pt_regs *regs)
```

处理调试异常的核心函数，负责：
- 保存寄存器状态
- 处理递归调试入口
- 协调多 CPU 调试
- 选择并进入调试模式

### register_kgdb_io()

```
int register_kgdb_io(struct kgdb_io *io_ops)
```

注册调试 IO 模块，支持动态添加不同的传输方式。

### kgdb_breakpoint()

```
void kgdb_breakpoint(void)
```

触发调试断点，进入调试器。

## 调试模式切换

```
┌─────────────────────────────────────────────────────────────────────┐
│                   Debug Mode Switching                           │
│                                                                     │
│  kgdb_kdb_mode (kernel/debug/debug_core.c):                       │
│  • 0 = KGDB 模式 (GDB 远程调试)                                   │
│  • 1 = KDB 模式 (内核内置调试器，默认)                            │
│                                                                     │
│  切换方式:                                                          │
│  1. 编译时配置: CONFIG_KDB                                        │
│  2. 内核参数: kgdbwait, kgdboc, kgdboe                           │
│  3. 运行时: 通过 /sys/module/kgdboc/parameters/kgdboc           │
│                                                                     │
│  KDB 进入方式:                                                      │
│  • 内核崩溃自动进入                                                │
│  • SysRq-g 键触发                                                  │
│  • 主动调用 kdb_breakpoint()                                      │
│  • 硬件断点触发                                                    │
│                                                                     │
│  KGDB 进入方式:                                                     │
│  • 远程 GDB 连接时触发                                              │
│  • 内核参数 kgdbwait 使系统启动时等待连接                          │
│  • 远程发送 break 命令                                              │
└─────────────────────────────────────────────────────────────────────┘
```

## 编译配置

| 配置项 | 说明 |
|--------|------|
| CONFIG_KGDB | 启用 KGDB 调试支持 |
| CONFIG_KDB | 启用 KDB 调试支持 |
| CONFIG_KGDB_SERIAL_CONSOLE | KGDB 串口支持 |
| CONFIG_KGDB_KDB | KGDB 和 KDB 共存 |
| CONFIG_KGDB_DEBUGGER | KGDB 调试器 |
| CONFIG_KGDB_TESTS | KGDB 测试 |
| CONFIG_KASAN | 启用 KASAN 内存检测 |
| CONFIG_UBSAN | 启用 UBSAN 未定义行为检测 |
| CONFIG_KCSAN | 启用 KCSAN 数据竞争检测 |
| CONFIG_SLUB_DEBUG | 启用 SLUB 内存分配器调试 |
| CONFIG_DEBUG_BUGVERBOSE_DETAILED | BUG/WARN 详细信息 |
| CONFIG_HIST_TRIGGERS_DEBUG | 直方图调试信息 |

## 使用流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                       Debugging Workflow                          │
│                                                                     │
│  步骤1: 配置内核                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  1. 启用 CONFIG_KGDB / CONFIG_KDB                           │   │
│  │  2. 选择 IO 方式 (串口/以太网/USB)                          │   │
│  │  3. 配置内核参数 (kgdboc=ttyS0,115200)                      │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                           │                                         │
│                           ▼                                         │
│  步骤2: 触发调试                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • 内核崩溃自动进入                                          │   │
│  │  • SysRq-g 手动触发                                         │   │
│  │  • 远程 GDB 连接                                            │   │
│  │  • 硬件断点触发                                              │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                           │                                         │
│                           ▼                                         │
│  步骤3: 调试操作                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  KDB:                                                       │   │
│  │  • bt - 堆栈追踪                                            │   │
│  │  • md - 内存查看                                            │   │
│  │  • rd - 寄存器查看                                          │   │
│  │  • bp - 断点管理                                            │   │
│  │  • ps - 任务列表                                            │   │
│  │  • go - 继续执行                                            │   │
│  │                                                             │   │
│  │  KGDB:                                                      │   │
│  │  • break - 设置断点                                         │   │
│  │  • next/step - 单步执行                                     │   │
│  │  • print - 查看变量                                         │   │
│  │  • continue - 继续执行                                      │   │
│  │  • info registers - 查看寄存器                              │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                           │                                         │
│                           ▼                                         │
│  步骤4: 分析问题                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • 分析堆栈信息                                             │   │
│  │  • 检查寄存器状态                                           │   │
│  │  • 查看内存内容                                             │   │
│  │  • 定位问题代码                                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 使用阶段分类总览

| 阶段 | 主要工具 |
|------|----------|
| **开发/编译阶段** | Clang 上下文分析、Clang 静态分析、`DEBUG_BUGVERBOSE_DETAILED` |
| **测试阶段** | KASAN、UBSAN、KCSAN、kmemleak、SLUB Debug、lockdep、Kdump |
| **运行时动态追踪** | Ftrace、perf、Tracepoints、BPF、Kprobes/Uprobes、动态调试 |
| **崩溃/panic 处理** | `panic_timeout`、`panic_on_oops`、Kdump、crash、pstore/Ramoops、pvpanic |
| **深度调试** | KGDB、crash |

## 总结

Linux 内核调试功能体系完整，从底层的调试核心到高层的追踪系统，提供了多层次的调试能力：

1. **编译时静态检查**：Clang 上下文分析、静态分析等在编译阶段发现问题
2. **内存错误检测**：KASAN、UBSAN、SLUB Debug、kmemleak、KCSAN 帮助发现内存问题
3. **并发与同步调试**：lockdep 检测死锁和锁滥用问题
4. **跟踪与性能分析**：ftrace、perf、BPF、tracepoints 提供非侵入式性能分析
5. **动态插桩**：kprobe/uprobe 提供内核和用户空间的动态插桩能力
6. **日志与打印**：printk 和动态调试提供运行时日志输出
7. **内核调试器**：KDB 和 KGDB 提供交互式调试能力
8. **Panic 相关调试**：Kdump、pstore 等提供崩溃后的诊断能力
9. **动态调试**：debugfs 提供运行时状态查询接口
10. **运行时验证**：Runtime Verification 提供运行时行为验证
11. **虚拟化相关调试**：kvm_stat 等提供虚拟化场景下的调试支持

这些功能相互配合，构成了一个强大的内核调试工具链，帮助开发者高效地定位和解决问题。