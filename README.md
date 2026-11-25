# Linux 7.2 内核源码分析笔记

面向 ARM64（aarch64）的 Linux 7.2 内核源码分析笔记，源码版本见 [Makefile](Makefile#L2-L4)。

> **配置边界**：仓库中的 `.config` 是为缩短编译时间刻意裁剪的调试配置，不是通用发行版配置。笔记分析的是 Linux 7.2 源码能力；涉及当前配置未启用的功能时，会标注“当前 `.config` 未启用”。完整裁剪清单见[附录 B](#附录-b配置裁剪清单)。

## 快速入口

- [内核子系统地图](notes/00-overview/kernel-map.md) — 源码目录、分层视图、四条典型路径
- [编译快照与文件统计](notes/00-overview/analysis_report.md) — 基于当前 Linux 7.2 树与裁剪配置的编译快照（含复现命令）
- [文档状态矩阵](#附录-a文档状态矩阵) — 每个子系统的覆盖程度与配置状态
- 文档规模：525 篇 Markdown，约 23.9 万行（含拆分篇与总篇）

## 目录

1. [ARM64 体系结构](#1-arm64-体系结构)
2. [启动与中断](#2-启动与中断)
3. [系统调用](#3-系统调用)
4. [内存管理](#4-内存管理)
5. [进程管理与调度](#5-进程管理与调度)
6. [时间子系统](#6-时间子系统)
7. [并发与同步](#7-并发与同步)
8. [异步执行](#8-异步执行)
9. [内核数据结构与算法](#9-内核数据结构与算法)
10. [驱动模型、总线与电源](#10-驱动模型总线与电源)
11. [存储栈](#11-存储栈)
12. [文件系统](#12-文件系统)
13. [网络子系统](#13-网络子系统)
14. [安全子系统](#14-安全子系统)
15. [虚拟化](#15-虚拟化)
16. [容器与隔离](#16-容器与隔离)
17. [调试与追踪](#17-调试与追踪)
18. [构建、模块与 Rust](#18-构建模块与-rust)
19. [附录](#附录-a文档状态矩阵)

## 文档目录结构

```
notes/
├── 00-overview/        总览、内核子系统地图、编译快照
├── 01-architecture/    ARMv8a、启动、中断
│   ├── armv8a/
│   ├── boot/
│   └── interrupt/
├── 02-core/            系统调用、内存、调度、时间、同步、异步、数据结构
│   ├── syscall/
│   ├── mm/
│   ├── sched/
│   ├── time/
│   ├── sync/
│   ├── async/
│   ├── dsaa/
│   └── kernel-core/
├── 03-drivers/         驱动模型、总线、电源、PCIe、设备类全景
│   ├── driver-model/
│   ├── bus/
│   ├── power/
│   └── pcie/
├── 04-storage/         块层、NVMe、SCSI/UFS/eMMC、DM/MD、FUSE/DAX、SSD
│   ├── block/
│   ├── nvme/
│   ├── scsi-ufs/
│   ├── dm-md/
│   └── ssd/
├── 05-filesystems/     VFS、特殊文件系统、真实文件系统
│   ├── vfs/
│   ├── special/
│   └── real/
├── 06-networking/      网络核心、协议栈、高级网络
│   ├── core/
│   └── advanced/
├── 07-security/        LSM、MAC、IMA/EVM、crypto/keyring、加固
├── 08-virtualization/  KVM/ARM64、virtio/vhost、VFIO/IOMMU
├── 09-containers/      cgroup v2、namespace、seccomp
├── 10-debug/           调试、追踪、崩溃分析
├── 11-build/           Kbuild/Kconfig、模块加载、livepatch/kexec
└── 12-rust/            Rust for Linux
```

---

## 1. ARM64 体系结构

- [arm cortex-a55 简介](notes/01-architecture/armv8a/1.summary.md)
- [电源管理](notes/01-architecture/armv8a/2.powermanage.md)
- [MMU](notes/01-architecture/armv8a/3.MMU.md)
- [RAS](notes/01-architecture/armv8a/4.RAS.md)
- [GIC 中断控制器](notes/01-architecture/armv8a/5.GIC.md)
- [Debug](notes/01-architecture/armv8a/6.Debug.md)
- [寄存器](notes/01-architecture/armv8a/7.register.md)
- [指令集](notes/01-architecture/armv8a/8.instructions.md)
- [ABI](notes/01-architecture/armv8a/9.ABI.md)
- [异常和中断](notes/01-architecture/armv8a/10.exception_intr.md)
- [Cache](notes/01-architecture/armv8a/11.Cache.md)
- [Memory Order](notes/01-architecture/armv8a/12.Memory_Order.md)

## 2. 启动与中断

### 2.1 启动过程

- [初始启动代码 head.S](notes/01-architecture/boot/head.S.md)
- [R_AARCH64_RELATIVE 重定位](notes/01-architecture/boot/rela.dyn.md)
- [start_kernel 流程](notes/01-architecture/boot/start_kernel.md)
- [SMP 启动过程](notes/01-architecture/boot/smp_start.md)
- [网络系统调用速查](notes/01-architecture/boot/net-syscall.md)
- [设备树与 DT 传递](notes/01-architecture/boot/06-device-tree.md)
- [ACPI 与 UEFI 启动路径](notes/01-architecture/boot/07-acpi-uefi.md)
- [initramfs 与 KASLR](notes/01-architecture/boot/08-initramfs-kaslr.md)

### 2.2 中断管理

- [GIC 中断控制器](notes/01-architecture/interrupt/arm64_interrupt_subsystem_analysis.md)
- [Exception 和 Interrupt 流程分析](notes/01-architecture/interrupt/arm64-interrupt-entry-analysis.md)
- [generic IRQ 层 (genirq)](notes/01-architecture/interrupt/03-genirq.md)


## 3. 系统调用

- [系统调用分类列表](notes/02-core/syscall/arm64-syscall-table.md)
- [vdso 实现机制](notes/02-core/syscall/vdso.md)

### 3.1 文件描述符操作

- [close](notes/02-core/syscall/fd-ops/close.md) | [close_range](notes/02-core/syscall/fd-ops/close_range.md) | [dup](notes/02-core/syscall/fd-ops/dup.md) | [dup3](notes/02-core/syscall/fd-ops/dup3.md)
- [fcntl](notes/02-core/syscall/fd-ops/fcntl.md)
### 3.2 文件 I/O

- [copy_file_range](notes/02-core/syscall/file-io/copy_file_range.md) | [pread64](notes/02-core/syscall/file-io/pread64.md) | [preadv](notes/02-core/syscall/file-io/preadv.md) | [pwrite64](notes/02-core/syscall/file-io/pwrite64.md) | [pwritev](notes/02-core/syscall/file-io/pwritev.md)
- [read](notes/02-core/syscall/file-io/read.md) | [readv](notes/02-core/syscall/file-io/readv.md) | [sendfile](notes/02-core/syscall/file-io/sendfile.md) | [splice](notes/02-core/syscall/file-io/splice.md) | [tee](notes/02-core/syscall/file-io/tee.md)
- [vmsplice](notes/02-core/syscall/file-io/vmsplice.md) | [write](notes/02-core/syscall/file-io/write.md) | [writev](notes/02-core/syscall/file-io/writev.md)
- [lseek](notes/02-core/syscall/file-io/lseek.md) | [fadvise64](notes/02-core/syscall/file-io/fadvise64.md) | [preadv2](notes/02-core/syscall/file-io/preadv2.md) | [pwritev2](notes/02-core/syscall/file-io/pwritev2.md)
### 3.3 文件元数据与属性

- [chdir](notes/02-core/syscall/file-meta/chdir.md) | [faccessat](notes/02-core/syscall/file-meta/faccessat.md) | [faccessat2](notes/02-core/syscall/file-meta/faccessat2.md) | [fallocate](notes/02-core/syscall/file-meta/fallocate.md) | [fchdir](notes/02-core/syscall/file-meta/fchdir.md)
- [fchmod](notes/02-core/syscall/file-meta/fchmod.md) | [fchmodat](notes/02-core/syscall/file-meta/fchmodat.md) | [fchmodat2](notes/02-core/syscall/file-meta/fchmodat2.md) | [fchown](notes/02-core/syscall/file-meta/fchown.md) | [fchownat](notes/02-core/syscall/file-meta/fchownat.md)
- [file_getattr](notes/02-core/syscall/file-meta/file_getattr.md) | [file_setattr](notes/02-core/syscall/file-meta/file_setattr.md) | [fstat](notes/02-core/syscall/file-meta/fstat.md) | [fstatat](notes/02-core/syscall/file-meta/fstatat.md) | [fstatfs](notes/02-core/syscall/file-meta/fstatfs.md)
- [ftruncate](notes/02-core/syscall/file-meta/ftruncate.md) | [getdents64](notes/02-core/syscall/file-meta/getdents64.md) | [linkat](notes/02-core/syscall/file-meta/linkat.md) | [mkdirat](notes/02-core/syscall/file-meta/mkdirat.md) | [mknodat](notes/02-core/syscall/file-meta/mknodat.md)
- [newfstatat](notes/02-core/syscall/file-meta/newfstatat.md) | [readlinkat](notes/02-core/syscall/file-meta/readlinkat.md) | [renameat](notes/02-core/syscall/file-meta/renameat.md) | [renameat2](notes/02-core/syscall/file-meta/renameat2.md) | [statfs](notes/02-core/syscall/file-meta/statfs.md)
- [statx](notes/02-core/syscall/file-meta/statx.md) | [access](notes/02-core/syscall/file-meta/access.md) | [chmod](notes/02-core/syscall/file-meta/chmod.md) | [chown](notes/02-core/syscall/file-meta/chown.md) | [stat](notes/02-core/syscall/file-meta/stat.md)
- [symlinkat](notes/02-core/syscall/file-meta/symlinkat.md) | [truncate](notes/02-core/syscall/file-meta/truncate.md) | [umask](notes/02-core/syscall/file-meta/umask.md) | [unlinkat](notes/02-core/syscall/file-meta/unlinkat.md)
- [utimensat](notes/02-core/syscall/file-meta/utimensat.md)
### 3.4 扩展属性 (xattr)

- [扩展属性总览](notes/02-core/syscall/xattr/xattr-overview.md)
- [fgetxattr](notes/02-core/syscall/xattr/fgetxattr.md) | [flistxattr](notes/02-core/syscall/xattr/flistxattr.md) | [fremovexattr](notes/02-core/syscall/xattr/fremovexattr.md) | [fsetxattr](notes/02-core/syscall/xattr/fsetxattr.md) | [getxattr](notes/02-core/syscall/xattr/getxattr.md)
- [getxattrat](notes/02-core/syscall/xattr/getxattrat.md) | [lgetxattr](notes/02-core/syscall/xattr/lgetxattr.md) | [listxattr](notes/02-core/syscall/xattr/listxattr.md) | [listxattrat](notes/02-core/syscall/xattr/listxattrat.md) | [llistxattr](notes/02-core/syscall/xattr/llistxattr.md)
- [lremovexattr](notes/02-core/syscall/xattr/lremovexattr.md) | [lsetxattr](notes/02-core/syscall/xattr/lsetxattr.md) | [removexattr](notes/02-core/syscall/xattr/removexattr.md) | [removexattrat](notes/02-core/syscall/xattr/removexattrat.md) | [setxattr](notes/02-core/syscall/xattr/setxattr.md)
- [setxattrat](notes/02-core/syscall/xattr/setxattrat.md)

### 3.5 文件系统挂载与结构

- [acct](notes/02-core/syscall/filesystem-mount/acct.md) | [chroot](notes/02-core/syscall/filesystem-mount/chroot.md) | [fdatasync](notes/02-core/syscall/filesystem-mount/fdatasync.md) | [fsync](notes/02-core/syscall/filesystem-mount/fsync.md) | [listmount](notes/02-core/syscall/filesystem-mount/listmount.md)
- [mount](notes/02-core/syscall/filesystem-mount/mount.md) | [pivot_root](notes/02-core/syscall/filesystem-mount/pivot_root.md) | [quotactl](notes/02-core/syscall/filesystem-mount/quotactl.md) | [quotactl_fd](notes/02-core/syscall/filesystem-mount/quotactl_fd.md) | [statmount](notes/02-core/syscall/filesystem-mount/statmount.md)
- [swapoff](notes/02-core/syscall/filesystem-mount/swapoff.md) | [swapon](notes/02-core/syscall/filesystem-mount/swapon.md) | [sync](notes/02-core/syscall/filesystem-mount/sync.md) | [sync_file_range](notes/02-core/syscall/filesystem-mount/sync_file_range.md) | [syncfs](notes/02-core/syscall/filesystem-mount/syncfs.md)
- [umount](notes/02-core/syscall/filesystem-mount/umount.md) | [umount2](notes/02-core/syscall/filesystem-mount/umount2.md)
- [move_mount](notes/02-core/syscall/filesystem-mount/move_mount.md) | [fsopen](notes/02-core/syscall/filesystem-mount/fsopen.md) | [fsconfig](notes/02-core/syscall/filesystem-mount/fsconfig.md) | [fsmount](notes/02-core/syscall/filesystem-mount/fsmount.md) | [fspick](notes/02-core/syscall/filesystem-mount/fspick.md) | [mount_setattr](notes/02-core/syscall/filesystem-mount/mount_setattr.md)
### 3.6 目录与路径操作

- [flock](notes/02-core/syscall/directory-path/flock.md) | [getcwd](notes/02-core/syscall/directory-path/getcwd.md) | [ioctl](notes/02-core/syscall/directory-path/ioctl.md) | [name_to_handle_at](notes/02-core/syscall/directory-path/name_to_handle_at.md) | [open](notes/02-core/syscall/directory-path/open.md)
- [open_by_handle_at](notes/02-core/syscall/directory-path/open_by_handle_at.md) | [open_tree](notes/02-core/syscall/directory-path/open_tree.md) | [open_tree_attr](notes/02-core/syscall/directory-path/open_tree_attr.md) | [openat](notes/02-core/syscall/directory-path/openat.md) | [openat2](notes/02-core/syscall/directory-path/openat2.md)

### 3.7 内存管理

- [brk](notes/02-core/syscall/memory/brk.md) | [madvise](notes/02-core/syscall/memory/madvise.md) | [map_shadow_stack](notes/02-core/syscall/memory/map_shadow_stack.md) | [mincore](notes/02-core/syscall/memory/mincore.md) | [mlock](notes/02-core/syscall/memory/mlock.md)
- [mlockall](notes/02-core/syscall/memory/mlockall.md) | [mmap](notes/02-core/syscall/memory/mmap.md) | [mprotect](notes/02-core/syscall/memory/mprotect.md) | [mremap](notes/02-core/syscall/memory/mremap.md) | [mseal](notes/02-core/syscall/memory/mseal.md)
- [msync](notes/02-core/syscall/memory/msync.md) | [munlock](notes/02-core/syscall/memory/munlock.md) | [munlockall](notes/02-core/syscall/memory/munlockall.md) | [munmap](notes/02-core/syscall/memory/munmap.md) | [pkey_alloc](notes/02-core/syscall/memory/pkey_alloc.md)
- [pkey_free](notes/02-core/syscall/memory/pkey_free.md) | [pkey_mprotect](notes/02-core/syscall/memory/pkey_mprotect.md) | [process_madvise](notes/02-core/syscall/memory/process_madvise.md) | [remap_file_pages](notes/02-core/syscall/memory/remap_file_pages.md)
- [userfaultfd](notes/02-core/syscall/memory/userfaultfd.md) | [mlock2](notes/02-core/syscall/memory/mlock2.md)
### 3.8 进程控制

- [clone](notes/02-core/syscall/process/clone.md) | [clone3](notes/02-core/syscall/process/clone3.md) | [execve](notes/02-core/syscall/process/execve.md) | [execveat](notes/02-core/syscall/process/execveat.md) | [exit](notes/02-core/syscall/process/exit.md)
- [exit_group](notes/02-core/syscall/process/exit_group.md) | [getpid](notes/02-core/syscall/process/getpid.md) | [getppid](notes/02-core/syscall/process/getppid.md) | [gettid](notes/02-core/syscall/process/gettid.md) | [prctl](notes/02-core/syscall/process/prctl.md)
- [process_mrelease](notes/02-core/syscall/process/process_mrelease.md) | [set_tid_address](notes/02-core/syscall/process/set_tid_address.md) | [sysinfo](notes/02-core/syscall/process/sysinfo.md) | [unshare](notes/02-core/syscall/process/unshare.md) | [wait4](notes/02-core/syscall/process/wait4.md)
- [waitid](notes/02-core/syscall/process/waitid.md) | [fork](notes/02-core/syscall/process/fork.md)
- [setpriority](notes/02-core/syscall/process/setpriority.md) | [getpriority](notes/02-core/syscall/process/getpriority.md)
### 3.9 进程调度

- [sched_get_priority_max](notes/02-core/syscall/sched/sched_get_priority_max.md) | [sched_get_priority_min](notes/02-core/syscall/sched/sched_get_priority_min.md) | [sched_getaffinity](notes/02-core/syscall/sched/sched_getaffinity.md) | [sched_getattr](notes/02-core/syscall/sched/sched_getattr.md) | [sched_getparam](notes/02-core/syscall/sched/sched_getparam.md)
- [sched_getscheduler](notes/02-core/syscall/sched/sched_getscheduler.md) | [sched_rr_get_interval](notes/02-core/syscall/sched/sched_rr_get_interval.md) | [sched_setaffinity](notes/02-core/syscall/sched/sched_setaffinity.md) | [sched_setattr](notes/02-core/syscall/sched/sched_setattr.md) | [sched_setparam](notes/02-core/syscall/sched/sched_setparam.md)
- [sched_setscheduler](notes/02-core/syscall/sched/sched_setscheduler.md) | [sched_yield](notes/02-core/syscall/sched/sched_yield.md) | [nice](notes/02-core/syscall/sched/nice.md)
- [ioprio_set](notes/02-core/syscall/sched/ioprio_set.md) | [ioprio_get](notes/02-core/syscall/sched/ioprio_get.md)
### 3.10 进程凭证与权限

- [capget](notes/02-core/syscall/cred-perm/capget.md) | [capset](notes/02-core/syscall/cred-perm/capset.md) | [getegid](notes/02-core/syscall/cred-perm/getegid.md) | [geteuid](notes/02-core/syscall/cred-perm/geteuid.md) | [getgid](notes/02-core/syscall/cred-perm/getgid.md)
- [getgroups](notes/02-core/syscall/cred-perm/getgroups.md) | [getpgid](notes/02-core/syscall/cred-perm/getpgid.md) | [getresgid](notes/02-core/syscall/cred-perm/getresgid.md) | [getresuid](notes/02-core/syscall/cred-perm/getresuid.md) | [getsid](notes/02-core/syscall/cred-perm/getsid.md)
- [getuid](notes/02-core/syscall/cred-perm/getuid.md) | [personality](notes/02-core/syscall/cred-perm/personality.md) | [setfsgid](notes/02-core/syscall/cred-perm/setfsgid.md) | [setfsuid](notes/02-core/syscall/cred-perm/setfsuid.md) | [setgid](notes/02-core/syscall/cred-perm/setgid.md)
- [setgroups](notes/02-core/syscall/cred-perm/setgroups.md) | [setpgid](notes/02-core/syscall/cred-perm/setpgid.md) | [setregid](notes/02-core/syscall/cred-perm/setregid.md) | [setresgid](notes/02-core/syscall/cred-perm/setresgid.md) | [setresuid](notes/02-core/syscall/cred-perm/setresuid.md)
- [setegid](notes/02-core/syscall/cred-perm/setegid.md) | [seteuid](notes/02-core/syscall/cred-perm/seteuid.md) | [setreuid](notes/02-core/syscall/cred-perm/setreuid.md) | [setsid](notes/02-core/syscall/cred-perm/setsid.md) | [setuid](notes/02-core/syscall/cred-perm/setuid.md)

### 3.11 信号处理

- [kill](notes/02-core/syscall/signal/kill.md) | [pidfd_send_signal](notes/02-core/syscall/signal/pidfd_send_signal.md) | [rt_sigaction](notes/02-core/syscall/signal/rt_sigaction.md) | [rt_sigpending](notes/02-core/syscall/signal/rt_sigpending.md) | [rt_sigprocmask](notes/02-core/syscall/signal/rt_sigprocmask.md)
- [rt_sigqueueinfo](notes/02-core/syscall/signal/rt_sigqueueinfo.md) | [rt_sigreturn](notes/02-core/syscall/signal/rt_sigreturn.md) | [rt_sigsuspend](notes/02-core/syscall/signal/rt_sigsuspend.md) | [rt_sigtimedwait](notes/02-core/syscall/signal/rt_sigtimedwait.md) | [sigaltstack](notes/02-core/syscall/signal/sigaltstack.md)
- [signalfd4](notes/02-core/syscall/signal/signalfd4.md) | [tgkill](notes/02-core/syscall/signal/tgkill.md) | [tkill](notes/02-core/syscall/signal/tkill.md)
- [rt_tgsigqueueinfo](notes/02-core/syscall/signal/rt_tgsigqueueinfo.md)
### 3.12 定时器与时间

- [adjtimex](notes/02-core/syscall/timer-time/adjtimex.md) | [clock_adjtime](notes/02-core/syscall/timer-time/clock_adjtime.md) | [clock_getres](notes/02-core/syscall/timer-time/clock_getres.md) | [clock_gettime](notes/02-core/syscall/timer-time/clock_gettime.md) | [clock_nanosleep](notes/02-core/syscall/timer-time/clock_nanosleep.md)
- [clock_settime](notes/02-core/syscall/timer-time/clock_settime.md) | [gettimeofday](notes/02-core/syscall/timer-time/gettimeofday.md) | [nanosleep](notes/02-core/syscall/timer-time/nanosleep.md) | [settimeofday](notes/02-core/syscall/timer-time/settimeofday.md) | [timer_create](notes/02-core/syscall/timer-time/timer_create.md)
- [timer_delete](notes/02-core/syscall/timer-time/timer_delete.md) | [timer_getoverrun](notes/02-core/syscall/timer-time/timer_getoverrun.md) | [timer_gettime](notes/02-core/syscall/timer-time/timer_gettime.md) | [timer_settime](notes/02-core/syscall/timer-time/timer_settime.md) | [timerfd_create](notes/02-core/syscall/timer-time/timerfd_create.md)
- [timerfd_gettime](notes/02-core/syscall/timer-time/timerfd_gettime.md) | [timerfd_settime](notes/02-core/syscall/timer-time/timerfd_settime.md)

### 3.13 文件与目录事件监控

- [fanotify_init](notes/02-core/syscall/fs-notify/fanotify_init.md) | [fanotify_mark](notes/02-core/syscall/fs-notify/fanotify_mark.md) | [inotify_add_watch](notes/02-core/syscall/fs-notify/inotify_add_watch.md) | [inotify_init1](notes/02-core/syscall/fs-notify/inotify_init1.md) | [inotify_rm_watch](notes/02-core/syscall/fs-notify/inotify_rm_watch.md)

### 3.14 事件通知 epoll

- [epoll_create1](notes/02-core/syscall/epoll/epoll_create1.md) | [epoll_ctl](notes/02-core/syscall/epoll/epoll_ctl.md) | [epoll_pwait](notes/02-core/syscall/epoll/epoll_pwait.md) | [epoll_pwait2](notes/02-core/syscall/epoll/epoll_pwait2.md) | [eventfd2](notes/02-core/syscall/epoll/eventfd2.md)
- [pselect6](notes/02-core/syscall/epoll/pselect6.md) | [ppoll](notes/02-core/syscall/epoll/ppoll.md)
### 3.15 异步 I/O (AIO)

- [io_cancel](notes/02-core/syscall/aio/io_cancel.md) | [io_destroy](notes/02-core/syscall/aio/io_destroy.md) | [io_getevents](notes/02-core/syscall/aio/io_getevents.md) | [io_pgetevents](notes/02-core/syscall/aio/io_pgetevents.md) | [io_setup](notes/02-core/syscall/aio/io_setup.md)
- [io_submit](notes/02-core/syscall/aio/io_submit.md)

### 3.16 异步 I/O (io_uring)

- [io_uring_enter](notes/02-core/syscall/io-uring/io_uring_enter.md) | [io_uring_register](notes/02-core/syscall/io-uring/io_uring_register.md) | [io_uring_setup](notes/02-core/syscall/io-uring/io_uring_setup.md)

### 3.17 网络与 Socket

- [accept](notes/02-core/syscall/net-socket/accept.md) | [accept4](notes/02-core/syscall/net-socket/accept4.md) | [bind](notes/02-core/syscall/net-socket/bind.md) | [connect](notes/02-core/syscall/net-socket/connect.md) | [getpeername](notes/02-core/syscall/net-socket/getpeername.md)
- [getsockname](notes/02-core/syscall/net-socket/getsockname.md) | [getsockopt](notes/02-core/syscall/net-socket/getsockopt.md) | [listen](notes/02-core/syscall/net-socket/listen.md) | [recvfrom](notes/02-core/syscall/net-socket/recvfrom.md) | [recvmmsg](notes/02-core/syscall/net-socket/recvmmsg.md)
- [recvmsg](notes/02-core/syscall/net-socket/recvmsg.md) | [sendmmsg](notes/02-core/syscall/net-socket/sendmmsg.md) | [sendmsg](notes/02-core/syscall/net-socket/sendmsg.md) | [sendto](notes/02-core/syscall/net-socket/sendto.md) | [setsockopt](notes/02-core/syscall/net-socket/setsockopt.md)
- [shutdown](notes/02-core/syscall/net-socket/shutdown.md) | [socket](notes/02-core/syscall/net-socket/socket.md) | [socketpair](notes/02-core/syscall/net-socket/socketpair.md)

### 3.18 进程间通信 IPC

- [msgctl](notes/02-core/syscall/ipc/msgctl.md) | [msgget](notes/02-core/syscall/ipc/msgget.md) | [msgrcv](notes/02-core/syscall/ipc/msgrcv.md) | [msgsnd](notes/02-core/syscall/ipc/msgsnd.md) | [pipe2](notes/02-core/syscall/ipc/pipe2.md)
- [semctl](notes/02-core/syscall/ipc/semctl.md) | [semget](notes/02-core/syscall/ipc/semget.md) | [semop](notes/02-core/syscall/ipc/semop.md) | [semtimedop](notes/02-core/syscall/ipc/semtimedop.md) | [shmat](notes/02-core/syscall/ipc/shmat.md)
- [shmctl](notes/02-core/syscall/ipc/shmctl.md) | [shmdt](notes/02-core/syscall/ipc/shmdt.md) | [shmget](notes/02-core/syscall/ipc/shmget.md)

### 3.19 POSIX 消息队列

- [mq_getsetattr](notes/02-core/syscall/posix-mq/mq_getsetattr.md) | [mq_notify](notes/02-core/syscall/posix-mq/mq_notify.md) | [mq_open](notes/02-core/syscall/posix-mq/mq_open.md) | [mq_timedreceive](notes/02-core/syscall/posix-mq/mq_timedreceive.md) | [mq_timedsend](notes/02-core/syscall/posix-mq/mq_timedsend.md)
- [mq_unlink](notes/02-core/syscall/posix-mq/mq_unlink.md)

### 3.20 权限与安全

- [getrandom](notes/02-core/syscall/security/getrandom.md) | [landlock_add_rule](notes/02-core/syscall/security/landlock_add_rule.md) | [landlock_create_ruleset](notes/02-core/syscall/security/landlock_create_ruleset.md) | [landlock_restrict_self](notes/02-core/syscall/security/landlock_restrict_self.md) | [lsm_get_self_attr](notes/02-core/syscall/security/lsm_get_self_attr.md)
- [lsm_list_modules](notes/02-core/syscall/security/lsm_list_modules.md) | [lsm_set_self_attr](notes/02-core/syscall/security/lsm_set_self_attr.md) | [seccomp](notes/02-core/syscall/security/seccomp.md)

### 3.21 密钥管理

- [add_key](notes/02-core/syscall/key-management/add_key.md) | [keyctl](notes/02-core/syscall/key-management/keyctl.md) | [request_key](notes/02-core/syscall/key-management/request_key.md)

### 3.22 BPF 与追踪

- [bpf](notes/02-core/syscall/bpf-trace/bpf.md) | [perf_event_open](notes/02-core/syscall/bpf-trace/perf_event_open.md) | [ptrace](notes/02-core/syscall/bpf-trace/ptrace.md)

### 3.23 内核模块与 kexec

- [delete_module](notes/02-core/syscall/module-kexec/delete_module.md) | [finit_module](notes/02-core/syscall/module-kexec/finit_module.md) | [init_module](notes/02-core/syscall/module-kexec/init_module.md) | [kexec_file_load](notes/02-core/syscall/module-kexec/kexec_file_load.md) | [kexec_load](notes/02-core/syscall/module-kexec/kexec_load.md)

### 3.24 线程同步 (futex)

- [futex](notes/02-core/syscall/futex/futex.md) | [futex_requeue](notes/02-core/syscall/futex/futex_requeue.md) | [futex_wait](notes/02-core/syscall/futex/futex_wait.md) | [futex_waitv](notes/02-core/syscall/futex/futex_waitv.md) | [futex_wake](notes/02-core/syscall/futex/futex_wake.md)
- [get_robust_list](notes/02-core/syscall/futex/get_robust_list.md) | [set_robust_list](notes/02-core/syscall/futex/set_robust_list.md)

### 3.25 内核通知与监控

- [dmesg](notes/02-core/syscall/kernel-notify/dmesg.md) | [reboot](notes/02-core/syscall/kernel-notify/reboot.md) | [sysfs](notes/02-core/syscall/kernel-notify/sysfs.md) | [sysctl](notes/02-core/syscall/kernel-notify/sysctl.md) | [syslog](notes/02-core/syscall/kernel-notify/syslog.md)

### 3.26 NUMA 内存策略

- [get_mempolicy](notes/02-core/syscall/numa-memory/get_mempolicy.md) | [mbind](notes/02-core/syscall/numa-memory/mbind.md) | [migrate_pages](notes/02-core/syscall/numa-memory/migrate_pages.md) | [move_pages](notes/02-core/syscall/numa-memory/move_pages.md) | [set_mempolicy](notes/02-core/syscall/numa-memory/set_mempolicy.md)
- [set_mempolicy_home_node](notes/02-core/syscall/numa-memory/set_mempolicy_home_node.md)

### 3.27 系统标识与信息

- [cachestat](notes/02-core/syscall/system-info/cachestat.md) | [getcpu](notes/02-core/syscall/system-info/getcpu.md) | [listns](notes/02-core/syscall/system-info/listns.md) | [setdomainname](notes/02-core/syscall/system-info/setdomainname.md) | [sethostname](notes/02-core/syscall/system-info/sethostname.md)
- [times](notes/02-core/syscall/system-info/times.md) | [uname](notes/02-core/syscall/system-info/uname.md)

### 3.28 用户与组关系

- [kcmp](notes/02-core/syscall/user-group/kcmp.md) | [membarrier](notes/02-core/syscall/user-group/membarrier.md) | [pidfd_getfd](notes/02-core/syscall/user-group/pidfd_getfd.md) | [pidfd_open](notes/02-core/syscall/user-group/pidfd_open.md) | [rseq](notes/02-core/syscall/user-group/rseq.md)
- [rseq_slice_yield](notes/02-core/syscall/user-group/rseq_slice_yield.md) | [setns](notes/02-core/syscall/user-group/setns.md)

### 3.29 内存文件系统

- [memfd_create](notes/02-core/syscall/memfd/memfd_create.md) | [memfd_secret](notes/02-core/syscall/memfd/memfd_secret.md)

### 3.30 其他/杂项

- [getitimer](notes/02-core/syscall/misc/getitimer.md) | [getrlimit](notes/02-core/syscall/misc/getrlimit.md) | [getrusage](notes/02-core/syscall/misc/getrusage.md) | [prlimit64](notes/02-core/syscall/misc/prlimit64.md) | [process_vm_readv](notes/02-core/syscall/misc/process_vm_readv.md)
- [process_vm_writev](notes/02-core/syscall/misc/process_vm_writev.md) | [readahead](notes/02-core/syscall/misc/readahead.md) | [restart_syscall](notes/02-core/syscall/misc/restart_syscall.md) | [setitimer](notes/02-core/syscall/misc/setitimer.md) | [setrlimit](notes/02-core/syscall/misc/setrlimit.md)
- [arc_gettls](notes/02-core/syscall/misc/arc_gettls.md) | [arc_settls](notes/02-core/syscall/misc/arc_settls.md) | [arc_usr_cmpxchg](notes/02-core/syscall/misc/arc_usr_cmpxchg.md) | [cacheflush](notes/02-core/syscall/misc/cacheflush.md) | [or1k_atomic](notes/02-core/syscall/misc/or1k_atomic.md)
- [riscv_flush_icache](notes/02-core/syscall/misc/riscv_flush_icache.md) | [riscv_hwprobe](notes/02-core/syscall/misc/riscv_hwprobe.md) | [set_thread_area](notes/02-core/syscall/misc/set_thread_area.md) | [syscall](notes/02-core/syscall/misc/syscall.md) | [vhangup](notes/02-core/syscall/misc/vhangup.md)
- [lookup_dcookie](notes/02-core/syscall/misc/lookup_dcookie.md) | [nfsservctl](notes/02-core/syscall/misc/nfsservctl.md)

## 4. 内存管理

### 4.1 拆分阅读版

- [01 概述与核心框架](notes/02-core/mm/01-overview-core.md)
- [02 内存回收与交换](notes/02-core/mm/02-reclaim-swap.md)
- [03 内核对象与页缓存](notes/02-core/mm/03-page-cache-objects.md)
- [04 控制与隔离](notes/02-core/mm/04-control-isolation.md)
- [05 高级特性](notes/02-core/mm/05-advanced-features.md)
- [06 总结与附录](notes/02-core/mm/06-summary.md)

### 4.2 合并总篇

- [Linux 7.2 内存管理代码分析报告](notes/02-core/mm/memory_management.md) — 拆分版的来源，内容最完整

## 5. 进程管理与调度

- [进程创建退出](notes/02-core/sched/fork_exec_analysis.md)
- [调度子系统概览](notes/02-core/sched/schedule_subsystem_analysis.md)
- [PREEMPT_RT 实时抢占分析](notes/02-core/sched/preempt_rt_analysis.md)

## 6. 时间子系统

- [硬件定时器模块分析](notes/02-core/time/arm64-timer-hardware-config.md)
- [时间子系统概览](notes/02-core/time/time_subsystem_analysis.md)

## 7. 并发与同步

- [并发与同步总览](notes/02-core/sync/concurrency_sync_analysis.md)
  - [01-概述与选择指南](notes/02-core/sync/01-overview.md)
  - [02-原子操作](notes/02-core/sync/02-atomic-ops.md)
  - [03-自旋锁](notes/02-core/sync/03-spinlock.md)
  - [04-互斥锁](notes/02-core/sync/04-mutex.md)
  - [05-RT互斥锁与优先级继承](notes/02-core/sync/05-rtmutex.md)
  - [06-读写信号量](notes/02-core/sync/06-rwsem.md)
  - [07-信号量](notes/02-core/sync/07-semaphore.md)
  - [08-RCU机制](notes/02-core/sync/08-rcu.md)
  - [09-顺序锁](notes/02-core/sync/09-seqlock.md)
  - [10-Per-CPU变量](notes/02-core/sync/10-percpu.md)
  - [11-完成量](notes/02-core/sync/11-completion.md)
  - [12-等待队列](notes/02-core/sync/12-waitqueue.md)
  - [13-本地锁](notes/02-core/sync/13-local_lock.md)
  - [14-Per-CPU RWSEM](notes/02-core/sync/14-percpu-rwsem.md)
  - [15-Lockdep锁验证](notes/02-core/sync/15-lockdep.md)
  - [16-内存屏障](notes/02-core/sync/16-memory-barriers.md)
  - [17-Guard作用域管理](notes/02-core/sync/17-guard-scope.md)

## 8. 异步执行

- [异步执行机制概要](notes/02-core/async/async-execution-summary.md)
  - [01-irq_work](notes/02-core/async/01-irq_work.md)
  - [02-softirq](notes/02-core/async/02-softirq.md)
  - [03-bh-workqueue](notes/02-core/async/03-bh-workqueue.md)
  - [04-tasklet](notes/02-core/async/04-tasklet.md)
  - [05-timer-hrtimer](notes/02-core/async/05-timer-hrtimer.md)
  - [06-rcu-callback](notes/02-core/async/06-rcu-callback.md)
  - [07-workqueue](notes/02-core/async/07-workqueue.md)
  - [08-threaded-irq](notes/02-core/async/08-threaded-irq.md)
  - [09-kthread](notes/02-core/async/09-kthread.md)
  - [10-async_schedule](notes/02-core/async/10-async_schedule.md)
  - [11-task_work](notes/02-core/async/11-task_work.md)
  - [12-notifier_chain](notes/02-core/async/12-notifier_chain.md)
  - [13-background_kthread](notes/02-core/async/13-background_kthread.md)
  - [14-io_uring](notes/02-core/async/14-io_uring.md)
  - [15-fasync_sigio](notes/02-core/async/15-fasync_sigio.md)

## 9. 内核数据结构与算法

- [数据结构与算法分析](notes/02-core/dsaa/dsaa_analysis.md)
  - 双向循环链表、哈希链表、红黑树、基数树/Maple Tree、B 树变体、哈希表、堆、位图/sbitmap、Trie、IDR、XArray、eBPF Maps、kfifo、伙伴系统、kref
- [审计子系统 (audit)](notes/02-core/kernel-core/01-audit.md)

## 10. 驱动模型、总线与电源

### 10.1 驱动模型

- [driver core：device/driver/bus 与 probe](notes/03-drivers/driver-model/01-driver-model.md)
- [platform bus 与 platform driver](notes/03-drivers/driver-model/02-platform-bus.md)

### 10.2 总线与外设

- [I2C/SPI/GPIO/pinctrl/regmap/USB/MMC/UFS 核心框架](notes/03-drivers/bus/01-bus-framework.md)

### 10.3 电源管理

- [cpufreq/cpuidle/OPP/genpd/runtime PM/suspend](notes/03-drivers/power/01-power-management.md)

### 10.4 设备类全景

- [输入/HID、DRM/KMS、ALSA/ASoC、thermal、regulator、hwmon、IIO、V4L2](notes/03-drivers/01-subsystem-panorama.md)

### 10.5 PCIe

- [PCIe 总线初始化流程](notes/03-drivers/pcie/pcie.md)
- [PCI 驱动相关结构体](notes/03-drivers/pcie/pcie-struct.md)

## 11. 存储栈

### 11.1 块层

- [块设备核心框架](notes/04-storage/block/block-01-core-framework.md)
- [I/O 调度与策略控制](notes/04-storage/block/block-02-io-scheduling.md)
- [设备管理与调试](notes/04-storage/block/block-03-device-management.md)
- [文件系统交互](notes/04-storage/block/block-04-fs-interaction.md)
- [NVMe 驱动实例分析](notes/04-storage/block/block-05-nvme-driver.md)
- [Linux 7.2 块层代码分析报告（合并总篇）](notes/04-storage/block/block_layer_analysis.md)

### 11.2 NVMe

- [NVMe PCIe Host 驱动深度分析](notes/04-storage/nvme/nvme-pcie-host-analysis.md)

### 11.3 SCSI / UFS / eMMC

- [SCSI midlayer、UFS 与 MMC/eMMC](notes/04-storage/scsi-ufs/01-scsi-ufs-emmc.md)

### 11.4 Device Mapper 与 MD RAID

- [device-mapper target 与 MD RAID](notes/04-storage/dm-md/01-dm-md.md)

### 11.5 FUSE 与 DAX

- [FUSE 协议、virtiofs 与 DAX](notes/04-storage/fuse-dax.md)

### 11.6 SSD 专题

- [SSD 笔记阅读说明](notes/04-storage/ssd/README.md)
- [NAND Flash 解密](notes/04-storage/ssd/nand-flash.md)
- NAND 原理
  - [简介](notes/04-storage/ssd/nand原理/README.md) | [闪存物理特性](notes/04-storage/ssd/nand原理/physical.md) | [闪存内部结构原理](notes/04-storage/ssd/nand原理/arch.md) | [命令](notes/04-storage/ssd/nand原理/cmd.md) | [FTL](notes/04-storage/ssd/nand原理/ftl.md)
- NVMe 原理
  - [简介](notes/04-storage/ssd/nvme原理/intro.md) | [nvme设备基本原理](notes/04-storage/ssd/nvme原理/arch.md) | [nvme数据结构定义](notes/04-storage/ssd/nvme原理/define.md) | [nvme命令](notes/04-storage/ssd/nvme原理/cmd.md)

## 12. 文件系统

### 12.1 VFS 核心

- [VFS 核心流程分析](notes/05-filesystems/vfs/fs.md)

### 12.2 特殊文件系统

- [01-procfs](notes/05-filesystems/special/01-procfs.md) | [02-sysfs](notes/05-filesystems/special/02-sysfs.md) | [03-debugfs](notes/05-filesystems/special/03-debugfs.md) | [04-ramfs/tmpfs](notes/05-filesystems/special/04-ramfs-tmpfs.md)
- [05-bdevfs](notes/05-filesystems/special/05-bdevfs.md) | [06-sockfs](notes/05-filesystems/special/06-sockfs.md) | [07-cgroup](notes/05-filesystems/special/07-cgroup.md) | [08-devtmpfs](notes/05-filesystems/special/08-devtmpfs.md)
- [09-configfs](notes/05-filesystems/special/09-configfs.md) | [10-securityfs](notes/05-filesystems/special/10-securityfs.md) | [11-bpf](notes/05-filesystems/special/11-bpf.md) | [12-pipefs](notes/05-filesystems/special/12-pipefs.md)
- [13-hugetlbfs](notes/05-filesystems/special/13-hugetlbfs.md) | [14-rpc_pipefs](notes/05-filesystems/special/14-rpc_pipefs.md) | [15-devpts](notes/05-filesystems/special/15-devpts.md)

### 12.3 真实文件系统

- [ext4](notes/05-filesystems/real/01-ext4.md)
- [XFS 与 Btrfs](notes/05-filesystems/real/02-xfs-btrfs.md)
- [overlayfs 与网络文件系统 (netfs/NFS/9p/Ceph)](notes/05-filesystems/real/03-overlayfs-netfs.md)
- [嵌入式与闪存文件系统 (F2FS/EROFS/SquashFS/UBIFS/FAT)](notes/05-filesystems/real/04-embedded-fs.md)

## 13. 网络子系统

### 13.1 核心与协议

- [网络子系统概览](notes/06-networking/core/network_subsystem_analysis.md)
- [VFS 与 Socket 层分析](notes/06-networking/core/socket_layer_analysis.md)
- [TCP/IP 协议栈分析](notes/06-networking/core/tcp_ip_protocol_stack_analysis.md)
- [Intel 网卡驱动分析](notes/06-networking/core/intel_nic_driver_analysis.md)
- [完整数据包收发流程](notes/06-networking/core/complete_data_flow_analysis.md)

### 13.2 高级网络

- [netfilter、conntrack、NAT、iptables/nftables](notes/06-networking/advanced/01-netfilter.md)
- [XDP、AF_XDP、tc/qdisc、netlink 与 netns](notes/06-networking/advanced/02-xdp-tc-netlink.md)
- [cfg80211 / mac80211 / Bluetooth](notes/06-networking/advanced/03-wireless-bluetooth.md)

## 14. 安全子系统

- [LSM 框架与 hook 分发](notes/07-security/01-lsm.md)
- [SELinux / AppArmor / Smack / Tomoyo](notes/07-security/02-mac.md)
- [IMA 与 EVM](notes/07-security/03-ima-evm.md)
- [crypto API 与 keyring](notes/07-security/04-crypto-keyring.md)
- [内核加固：PAC/BTI、CFI、KASLR、Spectre 缓解](notes/07-security/05-hardening.md)

## 15. 虚拟化

- [KVM/ARM64：stage-2、VGIC、VHE](notes/08-virtualization/01-kvm-arm64.md)
- [virtio、vhost 与 vDPA](notes/08-virtualization/02-virtio-vhost.md)
- [VFIO 与 IOMMU/SMMU](notes/08-virtualization/03-vfio-iommu.md)

## 16. 容器与隔离

- [cgroup v2 核心与控制器](notes/09-containers/01-cgroup-v2.md)
- [namespace 与 seccomp](notes/09-containers/02-namespaces-seccomp.md)


## 17. 调试与追踪

### 17.1 调试功能总览

- [调试功能总览](notes/10-debug/debug.md)

### 17.2 编译时静态检查

- [编译时静态检查](notes/10-debug/25-compile_check.md)
  - **Clang 上下文与锁分析 (Context & Locking Analysis)** — Linux 7.0 引入，利用 Clang 22+ 在编译时静态检查内核同步原语使用是否正确
  - **Clang 静态分析支持** — 内核增强对 Clang 静态分析工具的支持，编译阶段发现代码缺陷
  - **`DEBUG_BUGVERBOSE_DETAILED`** — 触发 `BUG()` 或 `WARN()` 时提供更详细的错误信息

### 17.3 内存错误检测

- [KASAN — 内存错误检测](notes/10-debug/11-kasan.md)
  — 检测内核中的越界访问和释放后使用等内存错误 (mm/kasan/)
- [UBSAN — 未定义行为检测](notes/10-debug/12-ubsan.md)
  — 检测整数溢出、除零错误等未定义行为
- [SLUB Debug — 内存分配器调试](notes/10-debug/13-slub_debug.md)
  — 内存分配器 SLUB 自带调试选项，检测内存损坏、越界访问等
- [kmemleak — 内存泄漏检测](notes/10-debug/06-kmemleak.md)
- [KCSAN — 并发数据竞争检测](notes/10-debug/07-kcsan.md)
- [DMA 调试 — DMA API 调试](notes/10-debug/14-dma_debug.md)
- [debug_objects — 对象生命周期调试](notes/10-debug/15-debug_objects.md)

### 17.4 并发与同步调试

- [lockdep — 锁依赖分析](notes/10-debug/08-lockdep.md)
- [lock_debug — 锁调试功能](notes/10-debug/16-lock_debug.md)
  — 包含 spinlock_debug、mutex-debug、irqflag-debug、lock_events
- [traceoff_on_warning](notes/10-debug/26-trace_debug.md)
  — 内核启动参数，触发 `WARN()` 时自动关闭追踪功能 (`/proc/sys/kernel/traceoff_on_warning`)

### 17.5 跟踪与性能分析

- [ftrace — 函数级追踪](notes/10-debug/04-ftrace.md)
- [ftrace 子功能](notes/10-debug/27-ftrace_features.md)
  — 函数调用图、事件追踪、调度追踪、中断关闭追踪、抢占延迟追踪、硬件延迟追踪、操作系统噪声追踪
- [perf — 性能分析工具](notes/10-debug/17-perf.md)
  — 性能分析工具，基于 `perf_events`，分析 CPU 性能计数器、软件事件、tracepoint 等
- [Tracepoints — 静态跟踪点](notes/10-debug/18-tracepoints.md)
  — 内核中静态定义的跟踪点，使用 `TRACE_EVENT` 宏埋点，开销极低
- [BPF — 内核可编程框架](notes/10-debug/19-bpf.md)
  — 允许用户在不修改内核源码的情况下注入安全、高性能的代码
- [hist_debug](notes/10-debug/26-trace_debug.md)
  — 启用 `CONFIG_HIST_TRIGGERS_DEBUG` 后，显示每个事件直方图的内部数据（`/sys/kernel/tracing/events/<event>/hist_debug`）

### 17.6 动态插桩

- [kprobe/uprobe — 内核与用户空间探针](notes/10-debug/05-kprobe.md)

### 17.7 日志与打印

- [printk — 内核日志系统](notes/10-debug/20-printk.md)
  — 内核基础日志输出函数，配合 `dmesg` 查看内核日志
- [动态调试 (Dynamic Debug) — 运行时调试打印](notes/10-debug/20-printk.md)
  — 运行时动态开启/关闭特定文件、函数或行的 `pr_debug()`/`dev_dbg()` 打印

### 17.8 内核调试器

- [KDB — 内核内置调试器](notes/10-debug/02-kdb.md)
- [KGDB — GDB 远程调试](notes/10-debug/03-kgdb.md)

### 17.9 Panic 相关调试

- [Panic 触发与行为控制](notes/10-debug/28-panic_debug.md)
  — BUG_ON()、panic_on_oops、panic_on_warn、panic_timeout、panic_print、panic_on_taint 等

#### 17.9.1 触发 Panic

- **BUG_ON()** ([详细说明](notes/10-debug/28-panic_debug.md))
  — 开发者主动触发的 panic，条件为真时调用 `panic()`
- **Kernel Oops → Panic** ([详细说明](notes/10-debug/28-panic_debug.md))
  — 通过 `CONFIG_PANIC_ON_OOPS=y` 或内核启动参数 `oops=panic`，将 Oops 升级为 panic
- **`panic_on_warn`** ([详细说明](notes/10-debug/28-panic_debug.md))
  — 让 `WARN_ON` 警告也触发 panic
- **SysRq 手动触发** ([详细说明](notes/10-debug/28-panic_debug.md))
  — `echo c > /proc/sysrq-trigger` 人为触发 panic，用于测试崩溃转储机制

#### 17.9.2 控制 Panic 行为

- **`panic_timeout`** ([详细说明](notes/10-debug/28-panic_debug.md))
  — panic 后自动重启的等待秒数，可运行时配置、启动参数或编译时预设
- **`panic_on_oops`** ([详细说明](notes/10-debug/28-panic_debug.md))
  — 控制发生 Oops 时是否 panic
- **`panic_print`** ([详细说明](notes/10-debug/28-panic_debug.md))
  — 控制 panic 时打印哪些额外调试信息
- **`panic_on_taint`** ([详细说明](notes/10-debug/28-panic_debug.md))
  — 系统被"污染"（tainted）后触发 panic

#### 17.9.3 崩溃后诊断与信息收集

- [Kdump + crash — 崩溃转储与分析](notes/10-debug/21-kdump.md)
  — Kdump 是内核崩溃转储机制，panic 时启动第二个内核保存内存镜像（vmcore）；crash 工具用于分析 vmcore 文件
- [pstore / Ramoops — 崩溃日志持久化](notes/10-debug/22-pstore.md)
  — 利用保留的 RAM 区域存储崩溃日志，重启后可从 `/sys/fs/pstore/` 读取上次崩溃信息
- [pvpanic — 虚拟化崩溃通知](notes/10-debug/23-pvpanic.md)
  — 虚拟化场景下，Guest panic 时通知 Host

### 17.10 动态调试

- [debugfs — 调试文件系统](notes/10-debug/09-debugfs.md)

### 17.11 运行时验证

- [Runtime Verification — 运行时行为验证框架](notes/10-debug/10-runtime_verification.md)
  - [nrp — 不可重入监视器](notes/10-debug/10-runtime_verification.md)
  - [opid — 进程 ID 监视器](notes/10-debug/10-runtime_verification.md)
  - [pagefault — 页故障监视器](notes/10-debug/10-runtime_verification.md)
  - [sched — 调度监视器](notes/10-debug/10-runtime_verification.md)
  - [rtapp — 实时应用监视器](notes/10-debug/10-runtime_verification.md)
  - [sco — 系统调用顺序监视器](notes/10-debug/10-runtime_verification.md)

### 17.12 虚拟化相关调试

- [kvm_stat — KVM 运行时统计](notes/10-debug/24-kvm_stat.md)
  — 检索 KVM 虚拟机运行时统计信息
- [KVM 调试](notes/10-debug/29-kvm_debug.md)
  — KVM debugfs 接口、Guest Debug 支持、ARM64 VGIC 调试、Hyp 调试等

### 17.13 特殊调试功能

- [特殊调试功能](notes/10-debug/30-special_debug.md)
  — 调度器调试、时间子系统调试、内核符号表 (kallsyms)、VGIC 调试、Hyp 调试等

## 18. 构建、模块与 Rust

- [Kbuild 与 Kconfig](notes/11-build/01-kbuild-kconfig.md)
- [模块加载器与模块签名](notes/11-build/02-module-loader.md)
- [livepatch 与 kexec/kdump](notes/11-build/03-livepatch-kexec.md)
- [Rust for Linux](notes/12-rust/01-rust-for-linux.md)

---

## 附录 A：文档状态矩阵

| 领域 | 覆盖状态 | 当前 `.config` | 说明 |
|---|---|---|---|
| ARM64 体系结构 | 完整 | 启用 | 架构基础、MMU、Cache、RAS、内存序 |
| 启动流程 | 完整 | 启用 | head.S、重定位、start_kernel、SMP；新增 DT/ACPI/UEFI/initramfs/KASLR |
| 中断 | 完整 | 启用 | GICv3/ITS/MSI、异常入口；新增 generic IRQ 层 |
| 系统调用 | 完整 | 启用 | 353 篇，覆盖 arm64 64 位表；本轮补 23 个遗漏调用 |
| 内存管理 | 完整 | 启用 | 伙伴系统、SLUB、页表、回收、swap、THP/KSM、memcg、NUMA、CMA、DAMON |
| 进程与调度 | 完整 | 启用 | EEVDF、RT、DL、负载均衡、PELT、cgroup、core scheduling、sched_ext |
| 时间 | 完整 | 启用 | clocksource/clockevent、tick、hrtimer、NTP、POSIX timer、vDSO |
| 并发与同步 | 完整 | 启用 | 原子、锁、RCU、seqlock、per-CPU、lockdep、屏障 |
| 异步执行 | 完整 | 启用 | softirq、tasklet、workqueue、hrtimer、task_work、io_uring |
| 数据结构 | 完整 | 不适用 | 链表、树、哈希、位图、XArray、kfifo、伙伴系统、kref |
| 驱动模型/总线 | 新增 | 部分启用 | driver core、platform、I2C/SPI/GPIO/pinctrl/regmap/USB/MMC/UFS 概览；设备类全景覆盖 input/HID、DRM、ALSA、thermal、regulator、hwmon、IIO、V4L2 |
| 电源管理 | 新增 | 部分启用 | CPU_FREQ=y；CPU_IDLE/PM/SUSPEND 未启用 |
| PCIe | 部分 | 启用 | 总线初始化与结构体；设备树/ACPI 绑定见启动章节 |
| 块层/NVMe | 完整 | 启用 | bio、blk-mq、I/O 调度、QoS、NVMe host |
| SCSI/UFS/eMMC | 新增 | 部分启用 | SCSI=y；MMC/UFS 未启用 |
| DM/MD | 新增 | 未启用 | 源码存在，当前配置未开启 |
| FUSE/DAX | 新增 | 部分启用 | FUSE_FS=y；DAX 取决于具体 FS |
| VFS/特殊 FS | 完整 | 启用 | VFS 核心 + 15 种特殊文件系统 |
| ext4/XFS/Btrfs/overlayfs | 部分 | 部分启用 | ext4=y；XFS/Btrfs/overlayfs 未启用；新增 F2FS/EROFS/SquashFS/UBIFS/FAT 概览 |
| 网络核心 | 完整 | 启用 | socket、TCP/IP、netdevice、NAPI、网卡驱动 |
| 高级网络 | 新增 | 部分启用 | netfilter/bridge/tc 启用；nftables/XDP/VLAN/TLS 未启用；新增 wireless/Bluetooth 概览 |
| 安全 | 新增 | 未启用 | SECURITY/CRYPTO/KEYS 未启用 |
| 虚拟化 | 新增 | 部分启用 | KVM/VIRTIO/VFIO=y；IOMMU/ACPI 未启用 |
| 容器与隔离 | 新增 | 启用 | cgroups、memcg、namespaces、seccomp 启用 |
| 调试与追踪 | 完整 | 部分启用 | 文档全面；FTRACE/KPROBES/KGDB/KASAN/KCSAN 未启用 |
| 构建/模块 | 新增 | 部分启用 | KEXEC_FILE=y；MODULES 未启用 |
| Rust | 新增 | 未启用 | 源码有 rust/；当前 .config 无 CONFIG_RUST |
| SSD 专题 | 完整 | 不适用 | NAND/NVMe 原理入门（含物理特性、ONFI 命令、FTL/GC/磨损均衡、NVMe 队列与 PRP/SGL）；lightnvm/ZNS 子书已移除，nand/nvme 已并入本书 |

## 附录 B：配置裁剪清单

当前 `.config` 是刻意裁剪的调试配置，编译快、但功能不完整。阅读笔记时请区分“源码能力”和“本次编译启用能力”。

<details>
<summary>点击展开：关键配置项</summary>

| 配置 | 状态 | 影响 |
|---|---|---|
| `CONFIG_MODULES` | 未启用 | 全部内置，不能 insmod；模块加载器文档仅作源码分析 |
| `CONFIG_SECURITY` | 未启用 | 无 LSM/SELinux/AppArmor/IMA/EVM |
| `CONFIG_CRYPTO` | 未启用 | 无 crypto API、无 AF_ALG |
| `CONFIG_KEYS` | 未启用 | 无 keyring |
| `CONFIG_ACPI` | 未启用 | ACPI 启动路径不可用，ARM64 走 DT |
| `CONFIG_IOMMU_SUPPORT` | 未启用 | 无 SMMU/VFIO IOMMU 后端 |
| `CONFIG_FTRACE` | 未启用 | 无 function/event tracer |
| `CONFIG_KPROBES` | 未启用 | 无 kprobe/uprobe |
| `CONFIG_KGDB` | 未启用 | 无远程 GDB 调试 |
| `CONFIG_KASAN` | 未启用 | 无地址消毒器 |
| `CONFIG_KCSAN` | 未启用 | 无数据竞争检测 |
| `CONFIG_RANDOMIZE_BASE` | 未启用 | 无 KASLR |
| `CONFIG_BTRFS_FS` | 未启用 | 无 Btrfs |
| `CONFIG_XFS_FS` | 未启用 | 无 XFS |
| `CONFIG_OVERLAY_FS` | 未启用 | 无 overlayfs |
| `CONFIG_USB_SUPPORT` | 未启用 | 无 USB |
| `CONFIG_MMC` | 未启用 | 无 MMC/eMMC/SD |
| `CONFIG_SPI` | 未启用 | 无 SPI |
| `CONFIG_DRM` | 未启用 | 无 GPU/显示 |
| `CONFIG_SOUND` | 未启用 | 无 ALSA |
| `CONFIG_THERMAL` | 未启用 | 无温控 |
| `CONFIG_HWMON` | 未启用 | 无硬件监控 |
| `CONFIG_REGULATOR` | 未启用 | 无稳压器框架 |
| `CONFIG_CPU_IDLE` | 未启用 | 无 cpuidle |
| `CONFIG_PM` / `CONFIG_SUSPEND` | 未启用 | 无系统休眠 |
| `CONFIG_XDP_SOCKETS` | 未启用 | 无 AF_XDP |
| `CONFIG_VLAN_8021Q` | 未启用 | 无 VLAN |
| `CONFIG_NF_TABLES` | 未启用 | 无 nftables |
| `CONFIG_TLS` | 未启用 | 无内核 TLS |
| `CONFIG_USERFAULTFD` | 未启用 | 无 userfaultfd |
| `CONFIG_ZRAM` | 未启用 | 无 zram |
| `CONFIG_ZSWAP` | 不存在 | 无 zswap |
| `CONFIG_RUST` | 不存在 | 无 Rust 支持 |

</details>

## 附录 C：维护约定

1. **目录编号**：`00-overview` 到 `12-rust` 按“总览 → 架构 → 核心 → 驱动/存储/FS/网络 → 安全/虚拟化/容器 → 调试/构建”排序；新增文档放入对应子目录，不再新增顶级目录。
2. **文档命名**：同一目录内用 `NN-topic.md` 编号；单篇专题可用 `topic.md`。
3. **链接规则**：README 使用仓库相对路径；笔记之间使用相对路径；引用源码使用 `` `path/to/file.c` ``，关键位置可加 `file:///` 链接。源码行号会随注释增补漂移，视为近似定位，引用前以当前树 `grep -n` 核实。
4. **版本规则**：文档标题和“本文基于”统一写当前源码版本（Linux 7.2）；描述“自 7.0 引入”的历史特性时保留原版本号。
5. **配置说明**：涉及当前 `.config` 未启用的功能，必须在文档开头或相关章节注明“源码存在，当前配置未启用”。
6. **新增文档模板**：概述 → 核心数据结构 → 初始化/关键流程 → 调用链 → 与其他子系统交互 → 配置与调试 → 关键文件清单。
7. **校验**：提交前用 Markdown 链接检查工具或脚本校验 README 链接、笔记相对链接与锚点，确保没有指向不存在文件的链接。
