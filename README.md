# armv8a CPU 知识详解

## [arm cortex-a55简介](notes/armv8a-arch/1.summary.md)

## [电源管理](notes/armv8a-arch/2.powermanage.md)

## [MMU](notes/armv8a-arch/3.MMU.md)

## [RAS](notes/armv8a-arch/4.RAS.md)

## [GIC中断控制器](notes/armv8a-arch/5.GIC.md)

## [Debug](notes/armv8a-arch/6.Debug.md)

## [寄存器](notes/armv8a-arch/7.register.md)

## [指令集](notes/armv8a-arch/8.instructions.md)

## [ABI](notes/armv8a-arch/9.ABI.md)

## [异常和中断](notes/armv8a-arch/10.exception_intr.md)

## [cache](notes/armv8a-arch/11.Cache.md)

## [memory order](notes/armv8a-arch/12.Memory_Order.md)

# Linux内核代码分析

## [内核代码结构](notes/analysis_report.md)

# arm64 Linux启动过程

## [初始启动代码head.S](./notes/head.S.md)

## [R_AARCH64_RELATIVE类型的.rela.dyn 重定位项](./notes/rela.dyn.md)

## [start_kernel流程](./notes/start_kernel.md)

## [SMP启动过程](./notes/smp_start.md)

## [pci初始化](./notes/device.md)

## [文件系统](./notes/fs.md)

# Exception和Interrupt过程

## [Exception和Interrupt流程分析](./notes/arm64-interrupt-entry-analysis.md)

# 系统调用

## [系统调用分类列表](./notes/syscall/arm64-syscall-table.md)

## 文件描述符操作

### [close](./notes/syscall/close-syscall-full-path.md)

### [close_range](./notes/syscall/close_range-syscall.md)

### [dup](./notes/syscall/dup-syscall.md)

### [dup3](./notes/syscall/dup3-syscall.md)

## 文件I/O

### [read](./notes/syscall/read-syscall.md)

### [write](./notes/syscall/write-syscall.md)

### [readv](./notes/syscall/readv-syscall-full-path.md)

### [writev](./notes/syscall/writev-syscall-full-path.md)

### [pread64](./notes/syscall/pread64-syscall.md)

### [pwrite64](./notes/syscall/pwrite64-syscall.md)

### [preadv / pwritev](./notes/syscall/preadv-pwritev-syscall.md)

### [splice / tee / vmsplice](./notes/syscall/splice-syscall.md)

### [sendfile / copy_file_range](./notes/syscall/sendfile-copy-range-syscall.md)

## 文件元数据与属性

### [stat / fstat / statx / xattr](./notes/syscall/stat-xattr-syscall.md)

## 扩展属性

## 文件系统挂载与结构

### [fsync / fdatasync / sync / syncfs / sync_file_range](./notes/syscall/sync-syscall.md)

## 目录与路径操作

### [open](./notes/syscall/open-syscall.md)

## 内存管理

### [mmap / munmap / mprotect / madvise / brk](./notes/syscall/memory-syscall-analysis.md)

## 定时器与时间

### [timer / time](./notes/syscall/timer-time-syscall.md)

## 进程控制

### [概览](./notes/syscall/process-syscall-analysis.md)

## 进程调度

### [概览](./notes/syscall/sched-cred-signal-syscall.md)

## 网络与Socket

### [概览](./notes/net-syscall.md)

## 事件通知

### [概览](./notes/syscall/epoll-aio-io_uring-syscall.md)

---

# 驱动

## 驱动相关结构体

### [device](./notes/driver/device.md)

## 块设备驱动

### [块设备驱动分析](./notes/driver/block_layer_analysis.md)

## nvme驱动

### [nvme执行过程](./notes/driver/nvme-pcie-host-analysis.md)
