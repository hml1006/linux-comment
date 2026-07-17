# Linux Kernel 源码分析笔记

## 1. ARMv8a 体系结构

- [arm cortex-a55 简介](notes/armv8a-arch/1.summary.md)
- [电源管理](notes/armv8a-arch/2.powermanage.md)
- [MMU](notes/armv8a-arch/3.MMU.md)
- [RAS](notes/armv8a-arch/4.RAS.md)
- [GIC 中断控制器](notes/armv8a-arch/5.GIC.md)
- [Debug](notes/armv8a-arch/6.Debug.md)
- [寄存器](notes/armv8a-arch/7.register.md)
- [指令集](notes/armv8a-arch/8.instructions.md)
- [ABI](notes/armv8a-arch/9.ABI.md)
- [异常和中断](notes/armv8a-arch/10.exception_intr.md)
- [Cache](notes/armv8a-arch/11.Cache.md)
- [Memory Order](notes/armv8a-arch/12.Memory_Order.md)

## 2. Linux 内核概览

- [内核代码结构](notes/analysis_report.md)

## 3. arm64 启动过程

- [初始启动代码 head.S](./notes/head.S.md)
- [R_AARCH64_RELATIVE 重定位](./notes/rela.dyn.md)
- [start_kernel 流程](./notes/start_kernel.md)
- [SMP 启动过程](./notes/smp_start.md)
- [PCI 初始化](./notes/driver/pcie.md)
- [文件系统](./notes/fs.md)

## 4. 异常与中断

- [中断子系统概览](./notes/interrupt/interrupt_subsystem_analysis.md)
- [Exception 和 Interrupt 流程分析](./notes/interrupt/arm64-interrupt-entry-analysis.md)

## 5. 系统调用

- [系统调用分类列表](./notes/syscall/arm64-syscall-table.md)

### 5.1 文件描述符操作

- [close](./notes/syscall/close-syscall-full-path.md) | [close_range](./notes/syscall/close_range-syscall.md) | [dup](./notes/syscall/dup-syscall.md) | [dup3](./notes/syscall/dup3-syscall.md)

### 5.2 文件 I/O

- [read](./notes/syscall/read-syscall.md) | [write](./notes/syscall/write-syscall.md) | [readv](./notes/syscall/readv-syscall-full-path.md) | [writev](./notes/syscall/writev-syscall-full-path.md)
- [pread64](./notes/syscall/pread64-syscall.md) | [pwrite64](./notes/syscall/pwrite64-syscall.md) | [preadv / pwritev](./notes/syscall/preadv-pwritev-syscall.md)
- [splice / tee / vmsplice](./notes/syscall/splice-syscall.md) | [sendfile / copy_file_range](./notes/syscall/sendfile-copy-range-syscall.md)

### 5.3 文件元数据与同步

- [stat / fstat / statx / xattr](./notes/syscall/stat-xattr-syscall.md) | [fsync / fdatasync / sync / syncfs / sync_file_range](./notes/syscall/sync-syscall.md)

### 5.4 目录与路径操作

- [open](./notes/syscall/open-syscall.md)

### 5.5 内存管理

- [mmap / munmap / mprotect / madvise / brk](./notes/syscall/memory-syscall-analysis.md)

### 5.6 定时器与时间

- [timer / time](./notes/syscall/timer-time-syscall.md)

### 5.7 进程与调度

- [进程控制概览](./notes/syscall/process-syscall-analysis.md) | [调度概览](./notes/syscall/sched-cred-signal-syscall.md)

### 5.8 网络与事件通知

- [网络 Socket 概览](./notes/net-syscall.md) | [epoll / aio / io_uring 概览](./notes/syscall/epoll-aio-io_uring-syscall.md)

## 6. 内存管理

- [内存分配与分析](./notes/memory/memory_management.md)

## 7. 时间子系统

- [硬件定时器模块分析](./notes/time/arm64-timer-hardware-config.md)

- [时间子系统概览](./notes/time/time_subsystem_analysis.md)

## 8. 进程调度子系统

- [调度子系统概览](./notes/schedule/schedule_subsystem_analysis.md)
- [PREEMPT_RT实时抢占分析](./notes/schedule/preempt_rt_analysis.md)

## 9. PCIe驱动

- [PCI驱动相关结构体](./notes/driver/pcie-struct.md)
- [块设备驱动分析](./notes/driver/block_layer_analysis.md)
- [NVMe 驱动执行过程](./notes/driver/nvme-pcie-host-analysis.md)

## 10. 网络子系统

- [网络子系统概览](./notes/network/network_subsystem_analysis.md)
- [VFS 与 Socket 层分析](./notes/network/socket_layer_analysis.md)
- [TCP/IP 协议栈分析](./notes/network/tcp_ip_protocol_stack_analysis.md)
- [Intel 网卡驱动分析](./notes/network/intel_nic_driver_analysis.md)