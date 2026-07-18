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

- [close](./notes/syscall/fd-ops/close.md) | [close_range](./notes/syscall/fd-ops/close_range.md) | [dup](./notes/syscall/fd-ops/dup.md) | [dup3](./notes/syscall/fd-ops/dup3.md)

### 5.2 文件 I/O

- [copy_file_range](./notes/syscall/file-io/copy_file_range.md) | [pread64](./notes/syscall/file-io/pread64.md) | [preadv](./notes/syscall/file-io/preadv.md) | [pwrite64](./notes/syscall/file-io/pwrite64.md) | [pwritev](./notes/syscall/file-io/pwritev.md)
- [read](./notes/syscall/file-io/read.md) | [readv](./notes/syscall/file-io/readv.md) | [sendfile](./notes/syscall/file-io/sendfile.md) | [splice](./notes/syscall/file-io/splice.md) | [tee](./notes/syscall/file-io/tee.md)
- [vmsplice](./notes/syscall/file-io/vmsplice.md) | [write](./notes/syscall/file-io/write.md) | [writev](./notes/syscall/file-io/writev.md)

### 5.3 文件元数据与属性

- [chdir](./notes/syscall/file-meta/chdir.md) | [faccessat](./notes/syscall/file-meta/faccessat.md) | [faccessat2](./notes/syscall/file-meta/faccessat2.md) | [fallocate](./notes/syscall/file-meta/fallocate.md) | [fchdir](./notes/syscall/file-meta/fchdir.md)
- [fchmod](./notes/syscall/file-meta/fchmod.md) | [fchmodat](./notes/syscall/file-meta/fchmodat.md) | [fchmodat2](./notes/syscall/file-meta/fchmodat2.md) | [fchown](./notes/syscall/file-meta/fchown.md) | [fchownat](./notes/syscall/file-meta/fchownat.md)
- [file_getattr](./notes/syscall/file-meta/file_getattr.md) | [file_setattr](./notes/syscall/file-meta/file_setattr.md) | [fstat](./notes/syscall/file-meta/fstat.md) | [fstatat](./notes/syscall/file-meta/fstatat.md) | [fstatfs](./notes/syscall/file-meta/fstatfs.md)
- [ftruncate](./notes/syscall/file-meta/ftruncate.md) | [getdents64](./notes/syscall/file-meta/getdents64.md) | [linkat](./notes/syscall/file-meta/linkat.md) | [mkdirat](./notes/syscall/file-meta/mkdirat.md) | [mknodat](./notes/syscall/file-meta/mknodat.md)
- [newfstatat](./notes/syscall/file-meta/newfstatat.md) | [readlinkat](./notes/syscall/file-meta/readlinkat.md) | [renameat](./notes/syscall/file-meta/renameat.md) | [renameat2](./notes/syscall/file-meta/renameat2.md) | [statfs](./notes/syscall/file-meta/statfs.md)
- [statx](./notes/syscall/file-meta/statx.md) | [access](./notes/syscall/file-meta/access.md) | [chmod](./notes/syscall/file-meta/chmod.md) | [chown](./notes/syscall/file-meta/chown.md) | [stat](./notes/syscall/file-meta/stat.md)
- [symlinkat](./notes/syscall/file-meta/symlinkat.md) | [truncate](./notes/syscall/file-meta/truncate.md) | [umask](./notes/syscall/file-meta/umask.md) | [unlinkat](./notes/syscall/file-meta/unlinkat.md)

### 5.4 扩展属性 (xattr)

- [扩展属性总览](./notes/syscall/xattr/xattr-overview.md)
- [fgetxattr](./notes/syscall/xattr/fgetxattr.md) | [flistxattr](./notes/syscall/xattr/flistxattr.md) | [fremovexattr](./notes/syscall/xattr/fremovexattr.md) | [fsetxattr](./notes/syscall/xattr/fsetxattr.md) | [getxattr](./notes/syscall/xattr/getxattr.md)
- [getxattrat](./notes/syscall/xattr/getxattrat.md) | [lgetxattr](./notes/syscall/xattr/lgetxattr.md) | [listxattr](./notes/syscall/xattr/listxattr.md) | [listxattrat](./notes/syscall/xattr/listxattrat.md) | [llistxattr](./notes/syscall/xattr/llistxattr.md)
- [lremovexattr](./notes/syscall/xattr/lremovexattr.md) | [lsetxattr](./notes/syscall/xattr/lsetxattr.md) | [removexattr](./notes/syscall/xattr/removexattr.md) | [removexattrat](./notes/syscall/xattr/removexattrat.md) | [setxattr](./notes/syscall/xattr/setxattr.md)
- [setxattrat](./notes/syscall/xattr/setxattrat.md)

### 5.5 文件系统挂载与结构

- [acct](./notes/syscall/filesystem-mount/acct.md) | [chroot](./notes/syscall/filesystem-mount/chroot.md) | [fdatasync](./notes/syscall/filesystem-mount/fdatasync.md) | [fsync](./notes/syscall/filesystem-mount/fsync.md) | [listmount](./notes/syscall/filesystem-mount/listmount.md)
- [mount](./notes/syscall/filesystem-mount/mount.md) | [pivot_root](./notes/syscall/filesystem-mount/pivot_root.md) | [quotactl](./notes/syscall/filesystem-mount/quotactl.md) | [quotactl_fd](./notes/syscall/filesystem-mount/quotactl_fd.md) | [statmount](./notes/syscall/filesystem-mount/statmount.md)
- [swapoff](./notes/syscall/filesystem-mount/swapoff.md) | [swapon](./notes/syscall/filesystem-mount/swapon.md) | [sync](./notes/syscall/filesystem-mount/sync.md) | [sync_file_range](./notes/syscall/filesystem-mount/sync_file_range.md) | [syncfs](./notes/syscall/filesystem-mount/syncfs.md)
- [umount](./notes/syscall/filesystem-mount/umount.md) | [umount2](./notes/syscall/filesystem-mount/umount2.md)

### 5.6 目录与路径操作

- [flock](./notes/syscall/directory-path/flock.md) | [getcwd](./notes/syscall/directory-path/getcwd.md) | [ioctl](./notes/syscall/directory-path/ioctl.md) | [name_to_handle_at](./notes/syscall/directory-path/name_to_handle_at.md) | [open](./notes/syscall/directory-path/open.md)
- [open_by_handle_at](./notes/syscall/directory-path/open_by_handle_at.md) | [open_tree](./notes/syscall/directory-path/open_tree.md) | [open_tree_attr](./notes/syscall/directory-path/open_tree_attr.md) | [openat](./notes/syscall/directory-path/openat.md) | [openat2](./notes/syscall/directory-path/openat2.md)

### 5.7 内存管理

- [brk](./notes/syscall/memory/brk.md) | [madvise](./notes/syscall/memory/madvise.md) | [map_shadow_stack](./notes/syscall/memory/map_shadow_stack.md) | [mincore](./notes/syscall/memory/mincore.md) | [mlock](./notes/syscall/memory/mlock.md)
- [mlockall](./notes/syscall/memory/mlockall.md) | [mmap](./notes/syscall/memory/mmap.md) | [mprotect](./notes/syscall/memory/mprotect.md) | [mremap](./notes/syscall/memory/mremap.md) | [mseal](./notes/syscall/memory/mseal.md)
- [msync](./notes/syscall/memory/msync.md) | [munlock](./notes/syscall/memory/munlock.md) | [munlockall](./notes/syscall/memory/munlockall.md) | [munmap](./notes/syscall/memory/munmap.md) | [pkey_alloc](./notes/syscall/memory/pkey_alloc.md)
- [pkey_free](./notes/syscall/memory/pkey_free.md) | [pkey_mprotect](./notes/syscall/memory/pkey_mprotect.md) | [process_madvise](./notes/syscall/memory/process_madvise.md) | [remap_file_pages](./notes/syscall/memory/remap_file_pages.md)

### 5.8 进程控制

- [clone](./notes/syscall/process/clone.md) | [clone3](./notes/syscall/process/clone3.md) | [execve](./notes/syscall/process/execve.md) | [execveat](./notes/syscall/process/execveat.md) | [exit](./notes/syscall/process/exit.md)
- [exit_group](./notes/syscall/process/exit_group.md) | [getpid](./notes/syscall/process/getpid.md) | [getppid](./notes/syscall/process/getppid.md) | [gettid](./notes/syscall/process/gettid.md) | [prctl](./notes/syscall/process/prctl.md)
- [process_mrelease](./notes/syscall/process/process_mrelease.md) | [set_tid_address](./notes/syscall/process/set_tid_address.md) | [sysinfo](./notes/syscall/process/sysinfo.md) | [unshare](./notes/syscall/process/unshare.md) | [wait4](./notes/syscall/process/wait4.md)
- [waitid](./notes/syscall/process/waitid.md) | [fork](./notes/syscall/process/fork.md)

### 5.9 进程调度

- [sched_get_priority_max](./notes/syscall/sched/sched_get_priority_max.md) | [sched_get_priority_min](./notes/syscall/sched/sched_get_priority_min.md) | [sched_getaffinity](./notes/syscall/sched/sched_getaffinity.md) | [sched_getattr](./notes/syscall/sched/sched_getattr.md) | [sched_getparam](./notes/syscall/sched/sched_getparam.md)
- [sched_getscheduler](./notes/syscall/sched/sched_getscheduler.md) | [sched_rr_get_interval](./notes/syscall/sched/sched_rr_get_interval.md) | [sched_setaffinity](./notes/syscall/sched/sched_setaffinity.md) | [sched_setattr](./notes/syscall/sched/sched_setattr.md) | [sched_setparam](./notes/syscall/sched/sched_setparam.md)
- [sched_setscheduler](./notes/syscall/sched/sched_setscheduler.md) | [sched_yield](./notes/syscall/sched/sched_yield.md) | [nice](./notes/syscall/sched/nice.md)

### 5.10 进程凭证与权限

- [capget](./notes/syscall/cred-perm/capget.md) | [capset](./notes/syscall/cred-perm/capset.md) | [getegid](./notes/syscall/cred-perm/getegid.md) | [geteuid](./notes/syscall/cred-perm/geteuid.md) | [getgid](./notes/syscall/cred-perm/getgid.md)
- [getgroups](./notes/syscall/cred-perm/getgroups.md) | [getpgid](./notes/syscall/cred-perm/getpgid.md) | [getresgid](./notes/syscall/cred-perm/getresgid.md) | [getresuid](./notes/syscall/cred-perm/getresuid.md) | [getsid](./notes/syscall/cred-perm/getsid.md)
- [getuid](./notes/syscall/cred-perm/getuid.md) | [personality](./notes/syscall/cred-perm/personality.md) | [setfsgid](./notes/syscall/cred-perm/setfsgid.md) | [setfsuid](./notes/syscall/cred-perm/setfsuid.md) | [setgid](./notes/syscall/cred-perm/setgid.md)
- [setgroups](./notes/syscall/cred-perm/setgroups.md) | [setpgid](./notes/syscall/cred-perm/setpgid.md) | [setregid](./notes/syscall/cred-perm/setregid.md) | [setresgid](./notes/syscall/cred-perm/setresgid.md) | [setresuid](./notes/syscall/cred-perm/setresuid.md)
- [setegid](./notes/syscall/cred-perm/setegid.md) | [seteuid](./notes/syscall/cred-perm/seteuid.md) | [setreuid](./notes/syscall/cred-perm/setreuid.md) | [setsid](./notes/syscall/cred-perm/setsid.md) | [setuid](./notes/syscall/cred-perm/setuid.md)

### 5.11 信号处理

- [kill](./notes/syscall/signal/kill.md) | [pidfd_send_signal](./notes/syscall/signal/pidfd_send_signal.md) | [rt_sigaction](./notes/syscall/signal/rt_sigaction.md) | [rt_sigpending](./notes/syscall/signal/rt_sigpending.md) | [rt_sigprocmask](./notes/syscall/signal/rt_sigprocmask.md)
- [rt_sigqueueinfo](./notes/syscall/signal/rt_sigqueueinfo.md) | [rt_sigreturn](./notes/syscall/signal/rt_sigreturn.md) | [rt_sigsuspend](./notes/syscall/signal/rt_sigsuspend.md) | [rt_sigtimedwait](./notes/syscall/signal/rt_sigtimedwait.md) | [sigaltstack](./notes/syscall/signal/sigaltstack.md)
- [signalfd4](./notes/syscall/signal/signalfd4.md) | [tgkill](./notes/syscall/signal/tgkill.md) | [tkill](./notes/syscall/signal/tkill.md)

### 5.12 定时器与时间

- [adjtimex](./notes/syscall/timer-time/adjtimex.md) | [clock_adjtime](./notes/syscall/timer-time/clock_adjtime.md) | [clock_getres](./notes/syscall/timer-time/clock_getres.md) | [clock_gettime](./notes/syscall/timer-time/clock_gettime.md) | [clock_nanosleep](./notes/syscall/timer-time/clock_nanosleep.md)
- [clock_settime](./notes/syscall/timer-time/clock_settime.md) | [gettimeofday](./notes/syscall/timer-time/gettimeofday.md) | [nanosleep](./notes/syscall/timer-time/nanosleep.md) | [settimeofday](./notes/syscall/timer-time/settimeofday.md) | [timer_create](./notes/syscall/timer-time/timer_create.md)
- [timer_delete](./notes/syscall/timer-time/timer_delete.md) | [timer_getoverrun](./notes/syscall/timer-time/timer_getoverrun.md) | [timer_gettime](./notes/syscall/timer-time/timer_gettime.md) | [timer_settime](./notes/syscall/timer-time/timer_settime.md) | [timerfd_create](./notes/syscall/timer-time/timerfd_create.md)
- [timerfd_gettime](./notes/syscall/timer-time/timerfd_gettime.md) | [timerfd_settime](./notes/syscall/timer-time/timerfd_settime.md)

### 5.13 文件与目录事件监控

- [fanotify_init](./notes/syscall/fs-notify/fanotify_init.md) | [fanotify_mark](./notes/syscall/fs-notify/fanotify_mark.md) | [inotify_add_watch](./notes/syscall/fs-notify/inotify_add_watch.md) | [inotify_init1](./notes/syscall/fs-notify/inotify_init1.md) | [inotify_rm_watch](./notes/syscall/fs-notify/inotify_rm_watch.md)

### 5.14 事件通知 epoll

- [epoll_create1](./notes/syscall/epoll/epoll_create1.md) | [epoll_ctl](./notes/syscall/epoll/epoll_ctl.md) | [epoll_pwait](./notes/syscall/epoll/epoll_pwait.md) | [epoll_pwait2](./notes/syscall/epoll/epoll_pwait2.md) | [eventfd2](./notes/syscall/epoll/eventfd2.md)

### 5.15 异步 I/O (AIO)

- [io_cancel](./notes/syscall/aio/io_cancel.md) | [io_destroy](./notes/syscall/aio/io_destroy.md) | [io_getevents](./notes/syscall/aio/io_getevents.md) | [io_pgetevents](./notes/syscall/aio/io_pgetevents.md) | [io_setup](./notes/syscall/aio/io_setup.md)
- [io_submit](./notes/syscall/aio/io_submit.md)

### 5.16 异步 I/O (io_uring)

- [io_uring_enter](./notes/syscall/io-uring/io_uring_enter.md) | [io_uring_register](./notes/syscall/io-uring/io_uring_register.md) | [io_uring_setup](./notes/syscall/io-uring/io_uring_setup.md)

### 5.17 网络与 Socket

- [accept](./notes/syscall/net-socket/accept.md) | [accept4](./notes/syscall/net-socket/accept4.md) | [bind](./notes/syscall/net-socket/bind.md) | [connect](./notes/syscall/net-socket/connect.md) | [getpeername](./notes/syscall/net-socket/getpeername.md)
- [getsockname](./notes/syscall/net-socket/getsockname.md) | [getsockopt](./notes/syscall/net-socket/getsockopt.md) | [listen](./notes/syscall/net-socket/listen.md) | [recvfrom](./notes/syscall/net-socket/recvfrom.md) | [recvmmsg](./notes/syscall/net-socket/recvmmsg.md)
- [recvmsg](./notes/syscall/net-socket/recvmsg.md) | [sendmmsg](./notes/syscall/net-socket/sendmmsg.md) | [sendmsg](./notes/syscall/net-socket/sendmsg.md) | [sendto](./notes/syscall/net-socket/sendto.md) | [setsockopt](./notes/syscall/net-socket/setsockopt.md)
- [shutdown](./notes/syscall/net-socket/shutdown.md) | [socket](./notes/syscall/net-socket/socket.md) | [socketpair](./notes/syscall/net-socket/socketpair.md)

### 5.18 进程间通信 IPC

- [msgctl](./notes/syscall/ipc/msgctl.md) | [msgget](./notes/syscall/ipc/msgget.md) | [msgrcv](./notes/syscall/ipc/msgrcv.md) | [msgsnd](./notes/syscall/ipc/msgsnd.md) | [pipe2](./notes/syscall/ipc/pipe2.md)
- [semctl](./notes/syscall/ipc/semctl.md) | [semget](./notes/syscall/ipc/semget.md) | [semop](./notes/syscall/ipc/semop.md) | [semtimedop](./notes/syscall/ipc/semtimedop.md) | [shmat](./notes/syscall/ipc/shmat.md)
- [shmctl](./notes/syscall/ipc/shmctl.md) | [shmdt](./notes/syscall/ipc/shmdt.md) | [shmget](./notes/syscall/ipc/shmget.md)

### 5.19 POSIX 消息队列

- [mq_getsetattr](./notes/syscall/posix-mq/mq_getsetattr.md) | [mq_notify](./notes/syscall/posix-mq/mq_notify.md) | [mq_open](./notes/syscall/posix-mq/mq_open.md) | [mq_timedreceive](./notes/syscall/posix-mq/mq_timedreceive.md) | [mq_timedsend](./notes/syscall/posix-mq/mq_timedsend.md)
- [mq_unlink](./notes/syscall/posix-mq/mq_unlink.md)

### 5.20 权限与安全

- [getrandom](./notes/syscall/security/getrandom.md) | [landlock_add_rule](./notes/syscall/security/landlock_add_rule.md) | [landlock_create_ruleset](./notes/syscall/security/landlock_create_ruleset.md) | [landlock_restrict_self](./notes/syscall/security/landlock_restrict_self.md) | [lsm_get_self_attr](./notes/syscall/security/lsm_get_self_attr.md)
- [lsm_list_modules](./notes/syscall/security/lsm_list_modules.md) | [lsm_set_self_attr](./notes/syscall/security/lsm_set_self_attr.md) | [seccomp](./notes/syscall/security/seccomp.md)

### 5.21 密钥管理

- [add_key](./notes/syscall/key-management/add_key.md) | [keyctl](./notes/syscall/key-management/keyctl.md) | [request_key](./notes/syscall/key-management/request_key.md)

### 5.22 BPF 与追踪

- [bpf](./notes/syscall/bpf-trace/bpf.md) | [perf_event_open](./notes/syscall/bpf-trace/perf_event_open.md) | [ptrace](./notes/syscall/bpf-trace/ptrace.md)

### 5.23 内核模块与 kexec

- [delete_module](./notes/syscall/module-kexec/delete_module.md) | [finit_module](./notes/syscall/module-kexec/finit_module.md) | [init_module](./notes/syscall/module-kexec/init_module.md) | [kexec_file_load](./notes/syscall/module-kexec/kexec_file_load.md) | [kexec_load](./notes/syscall/module-kexec/kexec_load.md)

### 5.24 线程同步 (futex)

- [futex](./notes/syscall/futex/futex.md) | [futex_requeue](./notes/syscall/futex/futex_requeue.md) | [futex_wait](./notes/syscall/futex/futex_wait.md) | [futex_waitv](./notes/syscall/futex/futex_waitv.md) | [futex_wake](./notes/syscall/futex/futex_wake.md)
- [get_robust_list](./notes/syscall/futex/get_robust_list.md) | [set_robust_list](./notes/syscall/futex/set_robust_list.md)

### 5.25 内核通知与监控

- [dmesg](./notes/syscall/kernel-notify/dmesg.md) | [reboot](./notes/syscall/kernel-notify/reboot.md) | [sysfs](./notes/syscall/kernel-notify/sysfs.md) | [sysctl](./notes/syscall/kernel-notify/sysctl.md) | [syslog](./notes/syscall/kernel-notify/syslog.md)

### 5.26 NUMA 内存策略

- [get_mempolicy](./notes/syscall/numa-memory/get_mempolicy.md) | [mbind](./notes/syscall/numa-memory/mbind.md) | [migrate_pages](./notes/syscall/numa-memory/migrate_pages.md) | [move_pages](./notes/syscall/numa-memory/move_pages.md) | [set_mempolicy](./notes/syscall/numa-memory/set_mempolicy.md)
- [set_mempolicy_home_node](./notes/syscall/numa-memory/set_mempolicy_home_node.md)

### 5.27 系统标识与信息

- [cachestat](./notes/syscall/system-info/cachestat.md) | [getcpu](./notes/syscall/system-info/getcpu.md) | [listns](./notes/syscall/system-info/listns.md) | [setdomainname](./notes/syscall/system-info/setdomainname.md) | [sethostname](./notes/syscall/system-info/sethostname.md)
- [times](./notes/syscall/system-info/times.md) | [uname](./notes/syscall/system-info/uname.md)

### 5.28 用户与组关系

- [kcmp](./notes/syscall/user-group/kcmp.md) | [membarrier](./notes/syscall/user-group/membarrier.md) | [pidfd_getfd](./notes/syscall/user-group/pidfd_getfd.md) | [pidfd_open](./notes/syscall/user-group/pidfd_open.md) | [rseq](./notes/syscall/user-group/rseq.md)
- [rseq_slice_yield](./notes/syscall/user-group/rseq_slice_yield.md) | [setns](./notes/syscall/user-group/setns.md)

### 5.29 内存文件系统

- [memfd_create](./notes/syscall/memfd/memfd_create.md) | [memfd_secret](./notes/syscall/memfd/memfd_secret.md)

### 5.30 其他/杂项

- [getitimer](./notes/syscall/misc/getitimer.md) | [getrlimit](./notes/syscall/misc/getrlimit.md) | [getrusage](./notes/syscall/misc/getrusage.md) | [prlimit64](./notes/syscall/misc/prlimit64.md) | [process_vm_readv](./notes/syscall/misc/process_vm_readv.md)
- [process_vm_writev](./notes/syscall/misc/process_vm_writev.md) | [readahead](./notes/syscall/misc/readahead.md) | [restart_syscall](./notes/syscall/misc/restart_syscall.md) | [setitimer](./notes/syscall/misc/setitimer.md) | [setrlimit](./notes/syscall/misc/setrlimit.md)
- [arc_gettls](./notes/syscall/misc/arc_gettls.md) | [arc_settls](./notes/syscall/misc/arc_settls.md) | [arc_usr_cmpxchg](./notes/syscall/misc/arc_usr_cmpxchg.md) | [cacheflush](./notes/syscall/misc/cacheflush.md) | [or1k_atomic](./notes/syscall/misc/or1k_atomic.md)
- [riscv_flush_icache](./notes/syscall/misc/riscv_flush_icache.md) | [riscv_hwprobe](./notes/syscall/misc/riscv_hwprobe.md) | [set_thread_area](./notes/syscall/misc/set_thread_area.md) | [syscall](./notes/syscall/misc/syscall.md) | [vhangup](./notes/syscall/misc/vhangup.md)

## 6. 内存管理

### 6.1 概述与核心框架
- [内存管理总体概览与核心框架](./notes/memory/01-overview-core.md)

### 6.2 内存回收与交换
- [内存回收与交换](./notes/memory/02-reclaim-swap.md)

### 6.3 内核对象与页缓存
- [内核对象与页缓存](./notes/memory/03-page-cache-objects.md)

### 6.4 控制与隔离
- [控制与隔离](./notes/memory/04-control-isolation.md)

### 6.5 高级特性
- [高级特性](./notes/memory/05-advanced-features.md)

### 6.6 附录
- [总结](./notes/memory/06-summary.md)

## 7. 时间子系统

- [硬件定时器模块分析](./notes/time/arm64-timer-hardware-config.md)
- [时间子系统概览](./notes/time/time_subsystem_analysis.md)

## 8. 进程调度子系统

- [调度子系统概览](./notes/schedule/schedule_subsystem_analysis.md)
- [PREEMPT_RT实时抢占分析](./notes/schedule/preempt_rt_analysis.md)

## 9. PCIe驱动

- [PCI驱动相关结构体](./notes/driver/pcie-struct.md)
- [PCIe总线初始化流程](./notes/driver/pcie.md)

### 9.1 块设备驱动分析 - 核心框架
- [块设备核心框架](./notes/driver/block-01-core-framework.md)

### 9.2 I/O调度与策略控制
- [I/O调度与策略控制](./notes/driver/block-02-io-scheduling.md)

### 9.3 设备管理与调试
- [设备管理与调试](./notes/driver/block-03-device-management.md)

### 9.4 文件系统交互
- [文件系统交互](./notes/driver/block-04-fs-interaction.md)

### 9.5 驱动实例分析
- [NVMe驱动实例分析](./notes/driver/block-05-nvme-driver.md)
- [NVMe PCIe Host驱动深度分析](./notes/driver/nvme-pcie-host-analysis.md)

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
