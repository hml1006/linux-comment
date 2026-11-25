# kvm_stat — KVM 运行时统计

## 概述

kvm_stat 是一个 top 风格的用户空间工具，用于显示 KVM 虚拟机的运行时统计信息。它通过读取 debugfs 或 perf tracepoints 获取 KVM 内核模块的事件计数器，帮助用户从 Host 视角观察 Guest 的行为，分析性能问题或调试 bug。

### 工作原理

```
┌─────────────────────────────────────────────────────────────┐
│                        Host                                │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                    KVM 内核模块                       │   │
│  │  - VMX/SVM 虚拟化扩展                                │   │
│  │  - vCPU 调度与管理                                   │   │
│  │  - 内存虚拟化 (EPT/NPT)                             │   │
│  │  - IO 虚拟化                                        │   │
│  └──────────────────────────────────────────────────────┘   │
│           ↓                                                   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              debugfs / tracepoints                  │   │
│  │  /sys/kernel/debug/kvm/                             │   │
│  │  /sys/kernel/debug/tracing/events/kvm/              │   │
│  └──────────────────────────────────────────────────────┘   │
│           ↓                                                   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                kvm_stat (Python)                     │   │
│  │  - 读取 debugfs 文件                                │   │
│  │  - 读取 perf tracepoints                           │   │
│  │  - 计算增量值和速率                                 │   │
│  │  - 格式化显示                                       │   │
│  └──────────────────────────────────────────────────────┘   │
│           ↓                                                   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              终端输出 (top 风格)                     │   │
│  │  event                    Total       CurAvg/s      │   │
│  │  exit_reasons             100000      1000          │   │
│  │    HLT                    50000       500           │   │
│  │    IO_INSTRUCTION         30000       300           │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 数据来源

kvm_stat 支持两种数据来源：

### debugfs 模式

```bash
# debugfs 路径
/sys/kernel/debug/kvm/
├── vcpu-<pid>-<vcpu>          # 每个 vCPU 的统计目录
│   ├── exits                  # VM 退出次数
│   ├── exits_reasons          # 退出原因统计
│   ├── halt_poll_success      # halt poll 成功次数
│   ├── halt_poll_fail         # halt poll 失败次数
│   ├── halt_wakeup            # halt 唤醒次数
│   └── ...
└── ...
```

### tracepoints 模式

```bash
# tracepoints 路径
/sys/kernel/debug/tracing/events/kvm/
├── kvm_entry                  # VM 进入事件
├── kvm_exit                   # VM 退出事件
├── kvm_halt_poll_ns           # halt poll 耗时
├── kvm_mmu_pte_write          # MMU PTE 写入
├── kvm_mmu_pte_age            # MMU PTE 老化
└── ...
```

## 架构支持

kvm_stat 支持多种架构，每种架构有不同的 VM 退出原因：

### x86_64 (VMX)

```python
VMX_EXIT_REASONS = {
    'EXCEPTION_NMI':        0,
    'EXTERNAL_INTERRUPT':   1,
    'CPUID':                10,
    'HLT':                  12,
    'IO_INSTRUCTION':       30,
    'MSR_READ':             31,
    'MSR_WRITE':            32,
    'EPT_VIOLATION':        48,
    'EPT_MISCONFIG':        49,
    'APIC_ACCESS':          44,
    'PREEMPTION_TIMER':     52,
    ...
}
```

### x86_64 (SVM)

```python
SVM_EXIT_REASONS = {
    'READ_CR0':       0x000,
    'WRITE_CR0':      0x010,
    'CPUID':          0x072,
    'HLT':            0x078,
    'IOIO':           0x07b,
    'MSR':            0x07c,
    'NPF':            0x400,      # Nested Page Fault
    ...
}
```

### ARM64

```python
AARCH64_EXIT_REASONS = {
    'WFx':          0x01,        # Wait for interrupt/event
    'CP15_32':      0x03,        # CP15 32-bit access
    'CP15_64':      0x04,        # CP15 64-bit access
    'FP_ASIMD':     0x07,        # FP/SIMD access
    'SVC64':        0x15,        # 64-bit SVC
    'HVC64':        0x16,        # 64-bit HVC
    'SMC64':        0x17,        # 64-bit SMC
    'SYS64':        0x18,        # 64-bit system instruction
    'IABT_LOW':     0x20,        # Instruction abort (lower EL)
    'DABT_LOW':     0x24,        # Data abort (lower EL)
    'DABT_CUR':     0x25,        # Data abort (current EL)
    'BRK64':        0x3C,        # 64-bit breakpoint
    ...
}
```

### s390x

```python
ArchS390() - 特殊处理指令类型的统计
```

### PowerPC

```python
ArchPPC() - 特殊的 ioctl 编号处理
```

## 核心数据结构

### perf_event_attr

用于配置 perf 事件的结构：

```python
class perf_event_attr(ctypes.Structure):
    _fields_ = [('type', ctypes.c_uint32),        # 事件类型
                ('size', ctypes.c_uint32),        # 结构大小
                ('config', ctypes.c_uint64),      # 事件配置
                ('sample_freq', ctypes.c_uint64), # 采样频率
                ('sample_type', ctypes.c_uint64), # 采样类型
                ('read_format', ctypes.c_uint64), # 读取格式
                ('flags', ctypes.c_uint64),       # 标志
                ('wakeup_events', ctypes.c_uint32), # 唤醒事件数
                ('bp_type', ctypes.c_uint32),     # 断点类型
                ('bp_addr', ctypes.c_uint64),     # 断点地址
                ('bp_len', ctypes.c_uint64),      # 断点长度
                ]
```

### Group

表示一个 perf 事件组：

```python
class Group(object):
    def __init__(self):
        self.events = []
    
    def add_event(self, event):
        self.events.append(event)
    
    def read(self):
        """返回所有事件的字典 {event_name: value}"""
```

### Arch

封装架构特定的数据：

```python
class Arch(object):
    @staticmethod
    def get_arch():
        """根据系统架构返回对应的 Arch 实例"""
    
    def tracepoint_is_child(self, field):
        """判断是否为子事件"""
```

## 工作流程

### 1. 初始化阶段

```
kvm_stat 启动
    → Arch.get_arch()           # 检测架构类型
    → 选择数据来源 (debugfs/tracepoints)
    → 加载事件列表
    → 初始化 curses 界面 (交互式模式)
```

### 2. 数据收集阶段

**debugfs 模式**:
```
while True:
    → 扫描 /sys/kernel/debug/kvm/ 目录
    → 读取每个 vCPU 的统计文件
    → 解析退出原因
    → 计算增量值
    → 计算速率 (CurAvg/s)
```

**tracepoints 模式**:
```
while True:
    → 通过 perf_event_open() 创建事件
    → 通过 ioctl() 读取事件计数
    → 解析退出原因
    → 计算增量值
    → 计算速率 (CurAvg/s)
```

### 3. 显示阶段

```
┌─────────────────────────────────────────────────────────────┐
│ kvm_stat - KVM statistics                                  │
│                                                             │
│ PID: 12345  Guest: vm01  Duration: 10s                     │
│                                                             │
│ event                    Total       CurAvg/s      %         │
│ ──────────────────────────────────────────────────────────  │
│ exit_reasons             100000      1000          100%     │
│   HLT                    50000       500           50%      │
│   IO_INSTRUCTION         30000       300           30%      │
│   MSR_WRITE              10000       100           10%      │
│   EPT_VIOLATION          5000        50            5%       │
│   CPUID                  5000        50            5%       │
│ ──────────────────────────────────────────────────────────  │
│ kvm_entry               990000      9900                    │
│ kvm_exit                100000      1000                    │
│                                                             │
│ Press 'h' for help, 'q' to quit                            │
└─────────────────────────────────────────────────────────────┘
```

## 使用方法

### 基本用法

```bash
# 交互式模式 (默认)
kvm_stat

# 批量模式 (运行一次)
kvm_stat -1
kvm_stat --batch

# 日志模式 (类似 vmstat)
kvm_stat -l
kvm_stat --log

# 保存到文件
kvm_stat -L /var/log/kvm_stat.log
```

### 指定数据来源

```bash
# 使用 debugfs
kvm_stat -d
kvm_stat --debugfs

# 使用 tracepoints
kvm_stat -t
kvm_stat --tracepoints
```

### 过滤选项

```bash
# 按 Guest 名称过滤
kvm_stat -g vm01
kvm_stat --guest=vm01

# 按 PID 过滤
kvm_stat -p 12345
kvm_stat --pid=12345

# 按字段过滤 (正则表达式)
kvm_stat -f "exit_reason|HLT"

# 查看可用字段
kvm_stat -f help
```

### 显示选项

```bash
# 设置刷新间隔 (秒)
kvm_stat -s 2

# CSV 格式输出
kvm_stat -l -c

# 跳过零记录
kvm_stat -l -z
```

## 交互式命令

在交互式模式下，按以下键执行操作：

| 键 | 功能 |
|----|------|
| `b` | 按 Guest 切换事件显示 (仅 debugfs) |
| `c` | 清除过滤器 |
| `f` | 设置字段过滤器 (正则表达式) |
| `g` | 按 Guest 名称/PID 过滤 |
| `h` | 显示帮助信息 |
| `o` | 切换排序方式 (Total vs CurAvg/s) |
| `p` | 按 Guest PID 过滤 |
| `q` | 退出 |
| `r` | 重置统计 |
| `s` | 设置刷新间隔 |
| `x` | 切换子事件显示 |

## 常用统计事件

### VM 退出相关

| 事件 | 描述 |
|------|------|
| `exit_reasons` | VM 退出总次数 |
| `kvm_entry` | VM 进入次数 |
| `kvm_exit` | VM 退出次数 |

### x86 退出原因

| 退出原因 | 描述 |
|---------|------|
| `HLT` | Guest 执行 HLT 指令 |
| `IO_INSTRUCTION` | IO 指令 |
| `CPUID` | CPUID 指令 |
| `MSR_READ` | MSR 读取 |
| `MSR_WRITE` | MSR 写入 |
| `EPT_VIOLATION` | EPT 违规 |
| `EPT_MISCONFIG` | EPT 配置错误 |
| `APIC_ACCESS` | APIC 访问 |
| `INTR_WINDOW` | 中断窗口打开 |
| `NMI_WINDOW` | NMI 窗口打开 |
| `PREEMPTION_TIMER` | 抢占定时器到期 |

### ARM64 退出原因

| 退出原因 | 描述 |
|---------|------|
| `WFx` | 等待中断/事件 |
| `CP15_32` | CP15 32位访问 |
| `CP15_64` | CP15 64位访问 |
| `FP_ASIMD` | FP/SIMD 访问 |
| `SVC64` | 64位 SVC 调用 |
| `HVC64` | 64位 HVC 调用 |
| `SMC64` | 64位 SMC 调用 |
| `DABT_LOW` | 数据中止 (lower EL) |
| `DABT_CUR` | 数据中止 (current EL) |

### 性能相关

| 事件 | 描述 |
|------|------|
| `halt_poll_success` | halt poll 成功 |
| `halt_poll_fail` | halt poll 失败 |
| `halt_wakeup` | halt 唤醒 |
| `kvm_mmu_pte_write` | MMU PTE 写入 |
| `kvm_mmu_pte_age` | MMU PTE 老化 |

## 性能分析示例

### 分析 VM 退出原因

```bash
# 查看退出原因分布
kvm_stat -1 -f "exit_reason"

# 按 HLT 退出过滤
kvm_stat -1 -f "HLT"
```

### 分析 MMU 性能

```bash
# 查看 MMU 相关事件
kvm_stat -1 -f "mmu"
```

### 长期监控

```bash
# 后台监控并记录到文件
nohup kvm_stat -L /var/log/kvm_stat.log -s 10 &

# 分析日志
cat /var/log/kvm_stat.log | grep "exit_reasons"
```

## 编译配置

```
CONFIG_KVM=y                      # KVM 核心支持
CONFIG_KVM_INTEL=y                # Intel VT-x 支持 (x86)
CONFIG_KVM_AMD=y                  # AMD-V 支持 (x86)
CONFIG_KVM_ARM_HOST=y             # ARM 主机支持
CONFIG_KVM_DEBUG_FS=y             # debugfs 支持
CONFIG_PERF_EVENTS=y              # perf 事件支持
CONFIG_EVENT_TRACING=y            # 事件追踪支持
```

## 权限要求

```bash
# 需要 root 权限或 CAP_SYS_ADMIN 能力
sudo kvm_stat

# 或设置能力
sudo setcap cap_sys_admin+ep /usr/bin/kvm_stat
```

## 代码位置

```
tools/kvm/kvm_stat/kvm_stat          # 主程序 (Python)
tools/kvm/kvm_stat/kvm_stat.service  # systemd 服务文件
tools/kvm/kvm_stat/kvm_stat.txt      # 帮助文档
Documentation/admin-guide/kvm/index.rst  # KVM 文档
```