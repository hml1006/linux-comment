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

- [初始启动代码 head.S](./notes/boot/head.S.md)
- [R_AARCH64_RELATIVE 重定位](./notes/boot/rela.dyn.md)
- [start_kernel 流程](./notes/boot/start_kernel.md)
- [SMP 启动过程](./notes/boot/smp_start.md)
- [PCI 初始化](./notes/driver/pcie.md)

## 4. 中断管理

- [GIC中断控制器](./notes/interrupt/arm64_interrupt_subsystem_analysis.md)
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

- [网络 Socket 概览](./notes/boot/net-syscall.md) | [epoll / aio / io_uring 概览](./notes/syscall/epoll-aio-io_uring-syscall.md)

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

## 11. 并发与同步

- [并发与同步总览](./notes/sync/concurrency_sync_analysis.md)
  - [01-概述与选择指南](./notes/sync/01-overview.md)
  - [02-原子操作](./notes/sync/02-atomic-ops.md)
  - [03-自旋锁](./notes/sync/03-spinlock.md)
  - [04-互斥锁](./notes/sync/04-mutex.md)
  - [05-RT互斥锁与优先级继承](./notes/sync/05-rtmutex.md)
  - [06-读写信号量](./notes/sync/06-rwsem.md)
  - [07-信号量](./notes/sync/07-semaphore.md)
  - [08-RCU机制](./notes/sync/08-rcu.md)
  - [09-顺序锁](./notes/sync/09-seqlock.md)
  - [10-Per-CPU变量](./notes/sync/10-percpu.md)
  - [11-完成量](./notes/sync/11-completion.md)
  - [12-等待队列](./notes/sync/12-waitqueue.md)
  - [13-本地锁](./notes/sync/13-local_lock.md)
  - [14-Per-CPU RWSEM](./notes/sync/14-percpu-rwsem.md)
  - [15-Lockdep锁验证](./notes/sync/15-lockdep.md)
  - [16-内存屏障](./notes/sync/16-memory-barriers.md)
  - [17-Guard作用域管理](./notes/sync/17-guard-scope.md)

## 12. 异步执行机制

- [异步执行机制概要](./notes/async/async-execution-summary.md)
  - [01-irq_work](./notes/async/01-irq_work.md)
  - [02-softirq](./notes/async/02-softirq.md)
  - [03-bh-workqueue](./notes/async/03-bh-workqueue.md)
  - [04-tasklet](./notes/async/04-tasklet.md)
  - [05-timer-hrtimer](./notes/async/05-timer-hrtimer.md)
  - [06-rcu-callback](./notes/async/06-rcu-callback.md)
  - [07-workqueue](./notes/async/07-workqueue.md)
  - [08-threaded-irq](./notes/async/08-threaded-irq.md)
  - [09-kthread](./notes/async/09-kthread.md)
  - [10-async_schedule](./notes/async/10-async_schedule.md)
  - [11-task_work](./notes/async/11-task_work.md)
  - [12-notifier_chain](./notes/async/12-notifier_chain.md)
  - [13-background_kthread](./notes/async/13-background_kthread.md)
  - [14-io_uring](./notes/async/14-io_uring.md)
  - [15-fasync_sigio](./notes/async/15-fasync_sigio.md)

## 13. 内核数据结构与算法

- [数据结构与算法分析](./notes/dsaa/dsaa_analysis.md)
  - [双向循环链表 (list_head)](#) | [哈希链表 (hlist_head)](#) | [红黑树 (rb_node)](#) | [XArray](#) | [伙伴系统](#) | [位图](#) | [引用计数 (kref)](#) | [Maple Tree](#)


## 14. 文件系统

- [文件系统总览](./notes/fs/fs.md)
  - [01-procfs — 进程文件系统](./notes/fs/special/01-procfs.md)
  - [02-sysfs — 内核对象文件系统](./notes/fs/special/02-sysfs.md)
  - [03-debugfs — 内核调试文件系统](./notes/fs/special/03-debugfs.md)
  - [04-ramfs/tmpfs — 内存文件系统](./notes/fs/special/04-ramfs-tmpfs.md)
  - [05-bdevfs — 块设备文件系统](./notes/fs/special/05-bdevfs.md)
  - [06-sockfs — Socket 文件系统](./notes/fs/special/06-sockfs.md)
  - [07-ext4 — 第四代扩展文件系统](./notes/fs/special/07-ext4.md)
  - [08-cgroup — 控制组文件系统](./notes/fs/special/08-cgroup.md)
  - [09-devtmpfs — 设备节点文件系统](./notes/fs/special/09-devtmpfs.md)
  - [10-configfs — 用户空间配置内核对象文件系统](./notes/fs/special/10-configfs.md)
  - [11-securityfs — 安全模块文件系统](./notes/fs/special/11-securityfs.md)
  - [12-bpf — BPF 文件系统](./notes/fs/special/12-bpf.md)
  - [13-pipefs — 管道文件系统](./notes/fs/special/13-pipefs.md)
  - [14-hugetlbfs — 大页文件系统](./notes/fs/special/14-hugetlbfs.md)
  - [15-rpc_pipefs — RPC 管道文件系统](./notes/fs/special/15-rpc_pipefs.md)
  - [16-devpts — 伪终端文件系统](./notes/fs/special/16-devpts.md)