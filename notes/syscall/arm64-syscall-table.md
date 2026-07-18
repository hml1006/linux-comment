# ARM64 Linux 系统调用表

来源：`arch/arm64/tools/syscall_64.tbl`

---

## 分类索引

| 分类 | 范围 | 系统调用数 |
|--|--|--|
| [文件描述符操作](#1-文件描述符操作) | close, dup, dup3, close_range | 4 |
| [文件 I/O](#2-文件-io) | read, write, pread64, pwrite64, readv, writev, preadv, pwritev, splice, tee, vmsplice, sendfile, copy_file_range | 13 |
| [文件元数据与属性](#3-文件元数据与属性) | stat, fstat, fstatat, statx, getdents64, access, faccessat, faccessat2, chdir, fchdir, chmod, fchmod, fchmodat, fchmodat2, chown, fchown, fchownat, umask, linkat, unlinkat, symlinkat, mknodat, mkdirat, renameat, renameat2, readlinkat, truncate, ftruncate, fallocate, file_getattr, file_setattr | 33 |
| [扩展属性](#4-扩展属性-xattr) | setxattr, lsetxattr, fsetxattr, getxattr, lgetxattr, fgetxattr, listxattr, llistxattr, flistxattr, removexattr, lremovexattr, fremovexattr, setxattrat, getxattrat, listxattrat, removexattrat | 16 |
| [文件系统挂载与结构](#5-文件系统挂载与结构) | mount, umount, pivot_root, statfs, fstatfs, chroot, sync, syncfs, fsync, fdatasync, sync_file_range, swapon, swapoff, acct, quotactl, quotactl_fd, statmount, listmount | 17 |
| [目录与路径操作](#6-目录与路径操作) | getcwd, openat, openat2, open_tree, open_tree_attr, name_to_handle_at, open_by_handle_at, ioctl, flock, open | 10 |
| [内存管理](#7-内存管理) | mmap, munmap, mremap, brk, mprotect, msync, mlock, munlock, mlockall, munlockall, mincore, madvise, process_madvise, remap_file_pages, mbind, set_mempolicy, get_mempolicy, migrate_pages, move_pages, set_mempolicy_home_node, pkey_mprotect, pkey_alloc, pkey_free, mseal, map_shadow_stack | 25 |
| [进程控制](#8-进程控制) | exit, exit_group, clone, clone3, fork, execve, execveat, wait4, waitid, set_tid_address, unshare, getpid, getppid, gettid, sysinfo, prctl, process_mrelease | 19 |
| [进程调度](#9-进程调度) | sched_setparam, sched_setscheduler, sched_getscheduler, sched_getparam, sched_setaffinity, sched_getaffinity, sched_yield, sched_get_priority_max, sched_get_priority_min, sched_rr_get_interval, sched_setattr, sched_getattr, nice | 14 |
| [进程凭证与权限](#10-进程凭证与权限) | setuid, getuid, setgid, getgid, seteuid, geteuid, setegid, getegid, setreuid, setregid, setresuid, getresuid, setresgid, getresgid, setfsuid, setfsgid, setpgid, getpgid, setsid, getsid, setgroups, getgroups, capget, capset, personality | 25 |
| [信号处理](#11-信号处理) | kill, tkill, tgkill, rt_sigaction, rt_sigprocmask, rt_sigpending, rt_sigtimedwait, rt_sigqueueinfo, rt_sigreturn, rt_sigsuspend, sigaltstack, pidfd_send_signal, signalfd4 | 13 |
| [定时器与时间](#12-定时器与时间) | timer_create, timer_settime, timer_gettime, timer_getoverrun, timer_delete, timerfd_create, timerfd_settime, timerfd_gettime, clock_settime, clock_gettime, clock_getres, clock_nanosleep, clock_adjtime, nanosleep, gettimeofday, settimeofday, adjtimex | 17 |
| [文件与目录事件监控](#13-文件与目录事件监控) | inotify_init1, inotify_add_watch, inotify_rm_watch, fanotify_init, fanotify_mark | 5 |
| [事件通知 epoll](#14-事件通知-epoll) | epoll_create1, epoll_ctl, epoll_pwait, epoll_pwait2, eventfd2 | 5 |
| [异步 I/O (AIO)](#15-异步-io-aio) | io_setup, io_destroy, io_submit, io_cancel, io_getevents, io_pgetevents | 6 |
| [异步 I/O (io_uring)](#16-异步-io-io_uring) | io_uring_setup, io_uring_enter, io_uring_register | 3 |
| [网络与Socket](#17-网络与socket) | socket, socketpair, bind, listen, accept, accept4, connect, getsockname, getpeername, sendto, recvfrom, setsockopt, getsockopt, shutdown, sendmsg, recvmsg, sendmmsg, recvmmsg | 18 |
| [进程间通信 IPC](#18-进程间通信-ipc) | msgget, msgctl, msgrcv, msgsnd, semget, semctl, semtimedop, semop, shmget, shmctl, shmat, shmdt, pipe2 | 13 |
| [POSIX 消息队列](#19-posix-消息队列) | mq_open, mq_unlink, mq_timedsend, mq_timedreceive, mq_notify, mq_getsetattr | 6 |
| [权限与安全](#20-权限与安全) | prctl, seccomp, landlock_create_ruleset, landlock_add_rule, landlock_restrict_self, lsm_get_self_attr, lsm_set_self_attr, lsm_list_modules, getrandom | 9 |
| [密钥管理](#21-密钥管理) | add_key, request_key, keyctl | 3 |
| [BPF 与追踪](#22-bpf-与追踪) | bpf, perf_event_open, ptrace | 3 |
| [内核模块与 kexec](#23-内核模块与-kexec) | init_module, finit_module, delete_module, kexec_load, kexec_file_load | 5 |
| [线程同步 (futex)](#24-线程同步-futex) | futex, futex_waitv, futex_wake, futex_wait, futex_requeue, set_robust_list, get_robust_list | 7 |
| [内核通知与监控](#25-内核通知与监控) | syslog, dmesg, reboot, sysfs, sysctl | 8 |
| [NUMA 内存策略](#26-numa-内存策略) | mbind, set_mempolicy, get_mempolicy, migrate_pages, move_pages, set_mempolicy_home_node | 6 |
| [系统标识与信息](#27-系统标识与信息) | uname, sethostname, setdomainname, getcpu, gettid, times, sysinfo, cachestat, statmount, listmount, listns | 11 |
| [用户与组关系](#28-用户与组关系) | pidfd_open, pidfd_getfd, setns, unshare, rseq, rseq_slice_yield, membarrier, kcmp | 8 |
| [内存文件系统](#29-内存文件系统) | memfd_create, memfd_secret | 2 |
| [其他/杂项](#30-其他杂项) | restart_syscall, syscall, vhangup, readahead, getitimer, setitimer, getrlimit, setrlimit, getrusage, prlimit64, process_vm_readv, process_vm_writev, cacheflush, arc_settls, arc_gettls, arc_usr_cmpxchg, set_thread_area, riscv_hwprobe, riscv_flush_icache, or1k_atomic | 20 |

---

## 详细系统调用表

### 1. 文件描述符操作

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 57 | close | `fd: int` | 关闭一个文件描述符 |
| 23 | dup | `oldfd: int` | 复制文件描述符（返回最小可用 fd） |
| 24 | dup3 | `oldfd: int, newfd: int, flags: int` | 复制到指定 fd，flags 支持 O_CLOEXEC |
| 436 | close_range | `first: unsigned int, last: unsigned int, flags: unsigned int` | 批量关闭 [first, last] 范围内的 fd |

### 2. 文件 I/O

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 63 | read | `fd: int, buf: void*, count: size_t` | 从 fd 读数据到缓冲区 |
| 64 | write | `fd: int, buf: const void*, count: size_t` | 从缓冲区写数据到 fd |
| 65 | readv | `fd: int, iov: const struct iovec*, iovcnt: int` | 分散读（scatter-gather） |
| 66 | writev | `fd: int, iov: const struct iovec*, iovcnt: int` | 集中写（scatter-gather） |
| 67 | pread64 | `fd: int, buf: void*, count: size_t, offset: off_t` | 指定偏移读（不改变文件位置） |
| 68 | pwrite64 | `fd: int, buf: const void*, count: size_t, offset: off_t` | 指定偏移写（不改变文件位置） |
| 69 | preadv | `fd: int, iov: const struct iovec*, iovcnt: int, offset: off_t` | 指定偏移的分散读 |
| 70 | pwritev | `fd: int, iov: const struct iovec*, iovcnt: int, offset: off_t` | 指定偏移的集中写 |
| 76 | splice | `fd_in: int, off_in: loff_t*, fd_out: int, off_out: loff_t*, len: size_t, flags: int` | 在两个 fd 间零拷贝传输数据 |
| 77 | tee | `fd_in: int, fd_out: int, len: size_t, flags: int` | 在管道间复制数据（不消费输入） |
| 75 | vmsplice | `fd: int, iov: const struct iovec*, nr_segs: int, flags: int` | 将用户页拼接至管道 |
| 71 | sendfile | `out_fd: int, in_fd: int, offset: off_t*, count: size_t` | 零拷贝文件传输 |
| 285 | copy_file_range | `fd_in: int, off_in: loff_t*, fd_out: int, off_out: loff_t*, len: size_t, flags: int` | 内核内文件区间拷贝（跨文件系统） |

### 3. 文件元数据与属性

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 43 | statfs | `path: const char*, buf: struct statfs*` | 获取文件系统统计信息 |
| 44 | fstatfs | `fd: int, buf: struct statfs*` | 通过 fd 获取文件系统统计信息 |
| 79 | newfstatat | `dfd: int, path: const char*, statbuf: struct stat*, flags: int` | 获取文件状态（at 系列, 替代 fstatat） |
| 80 | fstat | `fd: int, statbuf: struct stat*` | 通过 fd 获取文件状态 |
| 291 | statx | `dfd: int, path: const char*, flags: int, mask: unsigned int, statxbuf: struct statx*` | 增强版 stat（返回额外属性） |
| 61 | getdents64 | `fd: int, dirp: void*, count: unsigned int` | 读取目录条目 |
| 48 | faccessat | `dfd: int, path: const char*, mode: int, flags: int` | 检查文件访问权限 |
| 439 | faccessat2 | `dfd: int, path: const char*, mode: int, flags: int` | faccessat 的新版本 |
| 49 | chdir | `path: const char*` | 切换进程当前工作目录 |
| 50 | fchdir | `fd: int` | 通过 fd 切换工作目录 |
| 52 | fchmod | `fd: int, mode: mode_t` | 改变文件访问权限 |
| 53 | fchmodat | `dfd: int, path: const char*, mode: mode_t, flags: int` | at 系列 chmod |
| 452 | fchmodat2 | `dfd: int, path: const char*, mode: mode_t, flags: int` | fchmodat 的扩展版本 |
| 54 | fchownat | `dfd: int, path: const char*, owner: uid_t, group: gid_t, flags: int` | at 系列 chown |
| 55 | fchown | `fd: int, owner: uid_t, group: gid_t` | 改变文件所有者 |
| 166 | umask | `mask: mode_t` | 设置文件创建掩码 |
| 37 | linkat | `olddfd: int, oldpath: const char*, newdfd: int, newpath: const char*, flags: int` | 创建硬链接 |
| 35 | unlinkat | `dfd: int, path: const char*, flags: int` | 删除目录条目（AT_REMOVEDIR 可删除目录） |
| 36 | symlinkat | `target: const char*, newdfd: int, linkpath: const char*` | 创建符号链接 |
| 33 | mknodat | `dfd: int, path: const char*, mode: mode_t, dev: dev_t` | 创建特殊文件（设备节点等） |
| 34 | mkdirat | `dfd: int, path: const char*, mode: mode_t` | 创建目录 |
| 38 | renameat | `olddfd: int, oldpath: const char*, newdfd: int, newpath: const char*` | 重命名文件（旧版） |
| 276 | renameat2 | `olddfd: int, oldpath: const char*, newdfd: int, newpath: const char*, flags: int` | 重命名文件（支持 RENAME_NOREPLACE/EXCHANGE） |
| 78 | readlinkat | `dfd: int, path: const char*, buf: char*, bufsiz: size_t` | 读取符号链接目标 |
| 45 | truncate | `path: const char*, length: off_t` | 截断/扩展文件到指定长度 |
| 46 | ftruncate | `fd: int, length: off_t` | 通过 fd 截断文件 |
| 47 | fallocate | `fd: int, mode: int, offset: off_t, len: off_t` | 预分配/释放文件空间 |
| 468 | file_getattr | `fd: int, path: const char*, flags: unsigned int, ...` | 获取文件属性（扩展） |
| 469 | file_setattr | `fd: int, path: const char*, flags: unsigned int, ...` | 设置文件属性（扩展） |
| — | stat | `path: const char*, statbuf: struct stat*` | 获取文件状态（通过 newfstatat 实现，无独立 ARM64 编号） |
| — | access | `path: const char*, mode: int` | 检查文件访问权限（通过 faccessat 实现，无独立 ARM64 编号） |
| — | chmod | `path: const char*, mode: mode_t` | 改变文件权限（通过 fchmodat 实现，无独立 ARM64 编号） |
| — | chown | `path: const char*, owner: uid_t, group: gid_t` | 改变文件所有者（通过 fchownat 实现，无独立 ARM64 编号） |
| — | fstatat | `dfd: int, path: const char*, statbuf: struct stat*, flags: int` | 获取文件状态（通过 newfstatat 实现，无独立 ARM64 编号） |

### 4. 扩展属性 (xattr)

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 5 | setxattr | `path: const char*, name: const char*, value: const void*, size: size_t, flags: int` | 设置文件扩展属性 |
| 6 | lsetxattr | `path: const char*, name: const char*, value: const void*, size: size_t, flags: int` | 设置扩展属性（不跟踪符号链接） |
| 7 | fsetxattr | `fd: int, name: const char*, value: const void*, size: size_t, flags: int` | 通过 fd 设置扩展属性 |
| 8 | getxattr | `path: const char*, name: const char*, value: void*, size: size_t` | 获取扩展属性 |
| 9 | lgetxattr | `path: const char*, name: const char*, value: void*, size: size_t` | 获取扩展属性（不跟踪符号链接） |
| 10 | fgetxattr | `fd: int, name: const char*, value: void*, size: size_t` | 通过 fd 获取扩展属性 |
| 11 | listxattr | `path: const char*, list: char*, size: size_t` | 列出扩展属性名 |
| 12 | llistxattr | `path: const char*, list: char*, size: size_t` | 列出扩展属性名（不跟踪符号链接） |
| 13 | flistxattr | `fd: int, list: char*, size: size_t` | 通过 fd 列扩展属性名 |
| 14 | removexattr | `path: const char*, name: const char*` | 删除扩展属性 |
| 15 | lremovexattr | `path: const char*, name: const char*` | 删除扩展属性（不跟踪符号链接） |
| 16 | fremovexattr | `fd: int, name: const char*` | 通过 fd 删除扩展属性 |
| 463 | setxattrat | `dfd: int, path: const char*, name: const char*, value: const void*, size: size_t, flags: int` | at 系列设置扩展属性 |
| 464 | getxattrat | `dfd: int, path: const char*, name: const char*, value: void*, size: size_t, flags: int` | at 系列获取扩展属性 |
| 465 | listxattrat | `dfd: int, path: const char*, list: char*, size: size_t, flags: int` | at 系列列扩展属性名 |
| 466 | removexattrat | `dfd: int, path: const char*, name: const char*, flags: int` | at 系列删除扩展属性 |

### 5. 文件系统挂载与结构

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 40 | mount | `source: const char*, target: const char*, fstype: const char*, flags: unsigned long, data: const void*` | 挂载文件系统 |
| 39 | umount2 | `target: const char*, flags: int` | 卸载文件系统 |
| 41 | pivot_root | `new_root: const char*, put_old: const char*` | 切换根文件系统（容器命名空间） |
| 51 | chroot | `path: const char*` | 改变进程根目录 |
| 81 | sync | 无参数 | 刷新所有文件系统缓冲区 |
| 267 | syncfs | `fd: int` | 刷新指定 fd 所在文件系统缓冲区 |
| 82 | fsync | `fd: int` | 文件数据和元数据同步到磁盘 |
| 83 | fdatasync | `fd: int` | 文件数据同步（仅必要元数据） |
| 84 | sync_file_range | `fd: int, offset: off_t, nbytes: off_t, flags: int` | 同步文件部分区间 |
| 224 | swapon | `path: const char*, swap_flags: int` | 启用交换分区/文件 |
| 225 | swapoff | `path: const char*` | 停用交换分区/文件 |
| 89 | acct | `filename: const char*` | 启用/禁用进程记账 |
| 60 | quotactl | `cmd: int, special: const char*, id: int, addr: void*` | 磁盘配额操作 |
| 443 | quotactl_fd | `fd: int, cmd: int, id: unsigned int, addr: void*` | 通过 fd 进行磁盘配额操作 |
| 457 | statmount | `mnt_id: __u64, buf: struct statmnt*, size: size_t, flags: unsigned int` | 获取挂载点统计信息 |
| 458 | listmount | `root_mnt_id: __u64, last_mnt_id: __u64, list: __u64*, size: size_t, flags: unsigned int` | 列出已挂载文件系统 |
| — | umount | `target: const char*` | 卸载文件系统（旧版，通过 umount2 实现，无独立 ARM64 编号） |

### 6. 目录与路径操作

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 17 | getcwd | `buf: char*, size: size_t` | 获取当前工作目录路径 |
| 56 | openat | `dfd: int, path: const char*, flags: int, mode: mode_t` | 打开文件（at 系列，替代 open） |
| 437 | openat2 | `dfd: int, path: const char*, how: struct open_how*, usize: size_t` | 扩展版 openat（更多标志控制） |
| 428 | open_tree | `dfd: int, path: const char*, flags: unsigned int` | 打开挂载树获取 fd |
| 467 | open_tree_attr | `dfd: int, path: const char*, flags: unsigned int` | 打开挂载树并获取属性 fd |
| 264 | name_to_handle_at | `dfd: int, path: const char*, handle: struct file_handle*, mount_id: int*, flags: int` | 获取文件句柄（NFS 风格） |
| 265 | open_by_handle_at | `mount_fd: int, handle: struct file_handle*, flags: int` | 通过文件句柄打开文件 |
| 29 | ioctl | `fd: int, cmd: unsigned int, arg: ...` | 设备控制接口 |
| 32 | flock | `fd: int, operation: int` | 文件建议锁 |
| — | open | `path: const char*, flags: int, mode: mode_t` | 打开文件（通过 openat 实现，无独立 ARM64 编号） |

### 7. 内存管理

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 222 | mmap | `addr: void*, length: size_t, prot: int, flags: int, fd: int, offset: off_t` | 内存映射文件/匿名映射 |
| 215 | munmap | `addr: void*, length: size_t` | 解除内存映射 |
| 216 | mremap | `old_addr: void*, old_size: size_t, new_size: size_t, flags: int, ...` | 重映射/扩展虚存区域 |
| 214 | brk | `addr: void*` | 改变堆（heap）边界（sbrk/brk） |
| 226 | mprotect | `addr: void*, len: size_t, prot: int` | 设置内存区域访问权限 |
| 227 | msync | `addr: void*, length: size_t, flags: int` | 同步映射内存与文件 |
| 228 | mlock | `addr: const void*, len: size_t` | 锁住物理页（防止被换出） |
| 229 | munlock | `addr: const void*, len: size_t` | 解锁物理页 |
| 230 | mlockall | `flags: int` | 锁住所有进程内存页 |
| 231 | munlockall | 无参数 | 解锁所有进程内存页 |
| 232 | mincore | `addr: const void*, size: size_t, vec: unsigned char*` | 检测内存页是否驻留 |
| 233 | madvise | `addr: void*, length: size_t, advice: int` | 内存使用模式建议（提升性能） |
| 440 | process_madvise | `pidfd: int, iov: const struct iovec*, iovcnt: size_t, advice: int, flags: unsigned int` | 向其他进程发内存模式建议 |
| 234 | remap_file_pages | `addr: void*, size: size_t, prot: int, pgoff: off_t, flags: int` | 非线性文件映射（已废弃） |
| 288 | pkey_mprotect | `addr: void*, len: size_t, prot: int, pkey: int` | 设置带保护密钥的内存权限 |
| 289 | pkey_alloc | `flags: unsigned int, access_rights: unsigned long` | 分配一个内存保护密钥 |
| 290 | pkey_free | `pkey: int` | 释放内存保护密钥 |
| 462 | mseal | `addr: void*, len: size_t, flags: unsigned long` | 密封内存区域（禁止后续修改） |
| 453 | map_shadow_stack | `addr: void*, size: unsigned long, flags: unsigned int` | 映射影子栈（Shadow Stack，硬件安全） |

### 8. 进程控制

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 93 | exit | `error_code: int` | 终止当前进程 |
| 94 | exit_group | `error_code: int` | 终止整个线程组 |
| 220 | clone | `flags: unsigned long, stack: void*, parent_tid: int*, child_tid: int*, tls: unsigned long` | 创建子进程/线程 |
| 435 | clone3 | `args: struct clone_args*, size: size_t` | 扩展版 clone（更安全的参数结构） |
| 221 | execve | `path: const char*, argv: const char*const*, envp: const char*const*` | 执行新程序 |
| 281 | execveat | `dfd: int, path: const char*, argv: const char*const*, envp: const char*const*, flags: int` | at 系列 exec |
| 260 | wait4 | `pid: pid_t, status: int*, options: int, rusage: struct rusage*` | 等待子进程状态变更 |
| 95 | waitid | `idtype: int, id: id_t, infop: siginfo_t*, options: int` | 等待进程状态变更（扩展版） |
| 96 | set_tid_address | `tidptr: int*` | 设置线程 ID 指针（用于 set_tid_address 系统调用） |
| 97 | unshare | `flags: int` | 解命名空间共享 |
| 172 | getpid | 无参数 | 获取进程 ID |
| 173 | getppid | 无参数 | 获取父进程 ID |
| 178 | gettid | 无参数 | 获取线程 ID |
| 179 | sysinfo | `info: struct sysinfo*` | 获取系统总体信息 |
| 167 | prctl | `option: int, arg2: unsigned long, arg3: unsigned long, arg4: unsigned long, arg5: unsigned long` | 进程控制操作（多功能） |
| 448 | process_mrelease | `pidfd: int, flags: unsigned int` | 释放进程死亡时的内存 |
| — | fork | 无参数 | 创建子进程（通过 clone 实现，无独立 ARM64 编号，vfork 也映射至此） |

### 9. 进程调度

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 118 | sched_setparam | `pid: pid_t, param: const struct sched_param*` | 设置调度参数 |
| 119 | sched_setscheduler | `pid: pid_t, policy: int, param: const struct sched_param*` | 设置调度策略和参数 |
| 120 | sched_getscheduler | `pid: pid_t` | 获取调度策略 |
| 121 | sched_getparam | `pid: pid_t, param: struct sched_param*` | 获取调度参数 |
| 122 | sched_setaffinity | `pid: pid_t, cpusetsize: size_t, mask: const cpu_set_t*` | 设置 CPU 亲和性 |
| 123 | sched_getaffinity | `pid: pid_t, cpusetsize: size_t, mask: cpu_set_t*` | 获取 CPU 亲和性 |
| 124 | sched_yield | 无参数 | 主动让出 CPU |
| 125 | sched_get_priority_max | `policy: int` | 获取指定策略的最高优先级 |
| 126 | sched_get_priority_min | `policy: int` | 获取指定策略的最低优先级 |
| 127 | sched_rr_get_interval | `pid: pid_t, tp: struct timespec*` | 获取 RR 调度的时间片 |
| 274 | sched_setattr | `pid: pid_t, attr: struct sched_attr*, flags: unsigned int` | 扩展版设置调度属性 |
| 275 | sched_getattr | `pid: pid_t, attr: struct sched_attr*, size: unsigned int, flags: unsigned int` | 扩展版获取调度属性 |
| — | nice | `inc: int` | 降低/恢复进程优先级（通过 setpriority 模拟，无独立 ARM64 编号） |

### 10. 进程凭证与权限

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 146 | setuid | `uid: uid_t` | 设置用户 ID |
| 174 | getuid | 无参数 | 获取实际用户 ID |
| 144 | setgid | `gid: gid_t` | 设置组 ID |
| 176 | getgid | 无参数 | 获取实际组 ID |
| 175 | geteuid | 无参数 | 获取有效用户 ID |
| 177 | getegid | 无参数 | 获取有效组 ID |
| 145 | setreuid | `ruid: uid_t, euid: uid_t` | 设置实际和有效用户 ID |
| 143 | setregid | `rgid: gid_t, egid: gid_t` | 设置实际和有效组 ID |
| 147 | setresuid | `ruid: uid_t, euid: uid_t, suid: uid_t` | 设置用户 ID（实/有/保） |
| 148 | getresuid | `ruid: uid_t*, euid: uid_t*, suid: uid_t*` | 获取用户 ID（实/有/保） |
| 149 | setresgid | `rgid: gid_t, egid: gid_t, sgid: gid_t` | 设置组 ID（实/有/保） |
| 150 | getresgid | `rgid: gid_t*, egid: gid_t*, sgid: gid_t*` | 获取组 ID（实/有/保） |
| 151 | setfsuid | `fsuid: uid_t` | 设置文件系统访问 UID |
| 152 | setfsgid | `fsgid: gid_t` | 设置文件系统访问 GID |
| 154 | setpgid | `pid: pid_t, pgid: pid_t` | 设置进程组 ID |
| 155 | getpgid | `pid: pid_t` | 获取进程组 ID |
| 157 | setsid | 无参数 | 创建新会话 |
| 156 | getsid | `pid: pid_t` | 获取会话 ID |
| 158 | getgroups | `size: int, list: gid_t[]` | 获取附属组列表 |
| 159 | setgroups | `size: size_t, list: const gid_t[]` | 设置附属组列表 |
| 90 | capget | `header: cap_user_header_t, data: cap_user_data_t` | 获取进程能力 |
| 91 | capset | `header: cap_user_header_t, data: const cap_user_data_t` | 设置进程能力 |
| 92 | personality | `persona: unsigned long` | 设置进程执行域 |
| — | seteuid | `euid: uid_t` | 设置有效用户 ID（通过 setreuid 实现，无独立 ARM64 编号） |
| — | setegid | `egid: gid_t` | 设置有效组 ID（通过 setregid 实现，无独立 ARM64 编号） |

### 11. 信号处理

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 129 | kill | `pid: pid_t, sig: int` | 向进程发送信号 |
| 130 | tkill | `tid: int, sig: int` | 向线程发送信号 |
| 131 | tgkill | `tgid: int, tid: int, sig: int` | 向指定线程组中的线程发信号 |
| 134 | rt_sigaction | `sig: int, act: const struct sigaction*, oldact: struct sigaction*, sigsetsize: size_t` | 设置信号处理函数 |
| 135 | rt_sigprocmask | `how: int, set: const sigset_t*, oldset: sigset_t*, sigsetsize: size_t` | 操作信号掩码 |
| 136 | rt_sigpending | `set: sigset_t*, sigsetsize: size_t` | 获取待处理信号 |
| 137 | rt_sigtimedwait | `set: const sigset_t*, info: siginfo_t*, timeout: const struct timespec*, sigsetsize: size_t` | 等待信号 |
| 138 | rt_sigqueueinfo | `pid: pid_t, sig: int, uinfo: siginfo_t*` | 向进程发送信号附带信息 |
| 139 | rt_sigreturn | 无参数 | 从信号处理函数返回 |
| 133 | rt_sigsuspend | `mask: sigset_t*, sigsetsize: size_t` | 挂起进程等待信号 |
| 132 | sigaltstack | `ss: const stack_t*, old_ss: stack_t*` | 设置/获取替代信号栈 |
| 424 | pidfd_send_signal | `pidfd: int, sig: int, info: siginfo_t*, flags: unsigned int` | 通过 pidfd 发送信号 |
| 74 | signalfd4 | `fd: int, mask: const sigset_t*, flags: int` | 创建信号文件描述符 |

### 12. 定时器与时间

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 107 | timer_create | `clockid: clockid_t, sevp: struct sigevent*, timerid: timer_t*` | 创建 POSIX 定时器 |
| 110 | timer_settime | `timerid: timer_t, flags: int, new: const struct itimerspec*, old: struct itimerspec*` | 设置定时器 |
| 108 | timer_gettime | `timerid: timer_t, curr: struct itimerspec*` | 获取定时器剩余时间 |
| 109 | timer_getoverrun | `timerid: timer_t` | 获取定时器溢出计数 |
| 111 | timer_delete | `timerid: timer_t` | 删除定时器 |
| 85 | timerfd_create | `clockid: int, flags: int` | 创建定时器文件描述符 |
| 86 | timerfd_settime | `fd: int, flags: int, new: const struct itimerspec*, old: struct itimerspec*` | 设置 timerfd |
| 87 | timerfd_gettime | `fd: int, curr: struct itimerspec*` | 获取 timerfd 剩余时间 |
| 112 | clock_settime | `clockid: clockid_t, tp: const struct timespec*` | 设置时钟时间 |
| 113 | clock_gettime | `clockid: clockid_t, tp: struct timespec*` | 获取时钟时间 |
| 114 | clock_getres | `clockid: clockid_t, res: struct timespec*` | 获取时钟分辨率 |
| 115 | clock_nanosleep | `clockid: clockid_t, flags: int, rqtp: const struct timespec*, rmtp: struct timespec*` | 高精度睡眠 |
| 266 | clock_adjtime | `clockid: clockid_t, tx: struct timex*` | 调整时钟（NTP） |
| 101 | nanosleep | `req: const struct timespec*, rem: struct timespec*` | 高精度睡眠（CLOCK_MONOTONIC） |
| 169 | gettimeofday | `tv: struct timeval*, tz: struct timezone*` | 获取当前时间 |
| 170 | settimeofday | `tv: const struct timeval*, tz: const struct timezone*` | 设置当前时间 |
| 171 | adjtimex | `tx: struct timex*` | 调整时钟（adjtimex 风格） |

### 13. 文件与目录事件监控

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 26 | inotify_init1 | `flags: int` | 创建 inotify 实例 |
| 27 | inotify_add_watch | `fd: int, path: const char*, mask: uint32_t` | 添加 inotify 监控项 |
| 28 | inotify_rm_watch | `fd: int, wd: int` | 删除 inotify 监控项 |
| 262 | fanotify_init | `flags: unsigned int, event_f_flags: unsigned int` | 创建 fanotify 实例 |
| 263 | fanotify_mark | `fanotify_fd: int, flags: unsigned int, mask: __u64, dirfd: int, path: const char*` | 添加/删除 fanotify 监控标记 |

### 14. 事件通知 (epoll)

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 20 | epoll_create1 | `flags: int` | 创建 epoll 实例 |
| 21 | epoll_ctl | `epfd: int, op: int, fd: int, event: struct epoll_event*` | 控制 epoll 事件 |
| 22 | epoll_pwait | `epfd: int, events: struct epoll_event*, maxevents: int, timeout: int, sigmask: const sigset_t*` | epoll 等待事件 |
| 441 | epoll_pwait2 | `epfd: int, events: struct epoll_event*, maxevents: int, timeout: const struct timespec*, sigmask: const sigset_t*` | epoll 等待事件（纳秒超时） |
| 19 | eventfd2 | `initval: unsigned int, flags: int` | 创建 eventfd 实例 |

### 15. 异步 I/O (AIO)

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 0 | io_setup | `maxevents: unsigned int, ctxp: aio_context_t*` | 创建异步 I/O 上下文 |
| 1 | io_destroy | `ctx: aio_context_t` | 销毁异步 I/O 上下文 |
| 2 | io_submit | `ctx: aio_context_t, nr: long, iocbpp: struct iocb**` | 提交异步 I/O 请求 |
| 3 | io_cancel | `ctx: aio_context_t, iocb: struct iocb*, result: struct io_event*` | 取消异步 I/O 请求 |
| 4 | io_getevents | `ctx: aio_context_t, min: long, max: long, events: struct io_event*, timeout: struct timespec*` | 读取完成的 AIO 事件 |
| 292 | io_pgetevents | `ctx: aio_context_t, min: long, max: long, events: struct io_event*, timeout: struct timespec*, sigmask: const __sigset_t*` | 带信号掩码的 io_getevents |

### 16. 异步 I/O (io_uring)

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 425 | io_uring_setup | `entries: unsigned int, params: struct io_uring_params*` | 设置 io_uring 实例 |
| 426 | io_uring_enter | `fd: unsigned int, to_submit: unsigned int, min_complete: unsigned int, flags: unsigned int, sig: sigset_t*` | 提交 SQ 请求并/或等待 CQ 完成 |
| 427 | io_uring_register | `fd: unsigned int, opcode: unsigned int, arg: void*, nr_args: unsigned int` | 注册文件/缓冲区/事件fd 到 io_uring |

### 17. 网络与Socket

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 198 | socket | `domain: int, type: int, protocol: int` | 创建套接字端点 |
| 199 | socketpair | `domain: int, type: int, protocol: int, sv: int[2]` | 创建成对的套接字 |
| 200 | bind | `sockfd: int, addr: const struct sockaddr*, addrlen: socklen_t` | 绑定地址到套接字 |
| 201 | listen | `sockfd: int, backlog: int` | 监听套接字连接 |
| 202 | accept | `sockfd: int, addr: struct sockaddr*, addrlen: socklen_t*` | 接受连接（旧版） |
| 242 | accept4 | `sockfd: int, addr: struct sockaddr*, addrlen: socklen_t*, flags: int` | 接受连接（支持 SOCK_NONBLOCK） |
| 203 | connect | `sockfd: int, addr: const struct sockaddr*, addrlen: socklen_t` | 连接套接字 |
| 206 | sendto | `sockfd: int, buf: const void*, len: size_t, flags: int, dest_addr: const struct sockaddr*, addrlen: socklen_t` | 发送数据到指定地址 |
| 207 | recvfrom | `sockfd: int, buf: void*, len: size_t, flags: int, src_addr: struct sockaddr*, addrlen: socklen_t*` | 从指定地址接收数据 |
| 204 | getsockname | `sockfd: int, addr: struct sockaddr*, addrlen: socklen_t*` | 获取本地套接字地址 |
| 205 | getpeername | `sockfd: int, addr: struct sockaddr*, addrlen: socklen_t*` | 获取对方套接字地址 |
| 208 | setsockopt | `sockfd: int, level: int, optname: int, optval: const void*, optlen: socklen_t` | 设置套接字选项 |
| 209 | getsockopt | `sockfd: int, level: int, optname: int, optval: void*, optlen: socklen_t*` | 获取套接字选项 |
| 210 | shutdown | `sockfd: int, how: int` | 关闭套接字部分/全部连接 |
| 211 | sendmsg | `sockfd: int, msg: const struct msghdr*, flags: int` | 通过 msg 结构发送数据 |
| 212 | recvmsg | `sockfd: int, msg: struct msghdr*, flags: int` | 通过 msg 结构接收数据 |
| 269 | sendmmsg | `sockfd: int, msgvec: struct mmsghdr*, vlen: unsigned int, flags: int` | 批量发送消息 |
| 243 | recvmmsg | `sockfd: int, msgvec: struct mmsghdr*, vlen: unsigned int, flags: int, timeout: struct timespec*` | 批量接收消息 |

### 18. 进程间通信 (IPC)

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 186 | msgget | `key: key_t, msgflg: int` | 获取 System V 消息队列标识 |
| 187 | msgctl | `msqid: int, cmd: int, buf: struct msqid_ds*` | 消息队列控制操作 |
| 188 | msgrcv | `msqid: int, msgp: void*, msgsz: size_t, msgtyp: long, msgflg: int` | 从消息队列接收消息 |
| 189 | msgsnd | `msqid: int, msgp: const void*, msgsz: size_t, msgflg: int` | 向消息队列发送消息 |
| 190 | semget | `key: key_t, nsems: int, semflg: int` | 获取 System V 信号量集合 |
| 191 | semctl | `semid: int, semnum: int, cmd: int, ...` | 信号量控制 |
| 192 | semtimedop | `semid: int, sops: struct sembuf*, nsops: size_t, timeout: const struct timespec*` | 超时信号量操作 |
| 193 | semop | `semid: int, sops: struct sembuf*, nsops: size_t` | 信号量操作 |
| 194 | shmget | `key: key_t, size: size_t, shmflg: int` | 获取共享内存段 |
| 195 | shmctl | `shmid: int, cmd: int, buf: struct shmid_ds*` | 共享内存控制 |
| 196 | shmat | `shmid: int, shmaddr: const void*, shmflg: int` | 附加共享内存段 |
| 197 | shmdt | `shmaddr: const void*` | 分离共享内存段 |
| 59 | pipe2 | `pipefd: int[2], flags: int` | 创建管道（支持 O_CLOEXEC/NONBLOCK） |

### 19. POSIX 消息队列

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 180 | mq_open | `name: const char*, oflag: int, ...` | 打开/创建消息队列 |
| 181 | mq_unlink | `name: const char*` | 删除消息队列 |
| 182 | mq_timedsend | `mqdes: mqd_t, msg_ptr: const char*, msg_len: size_t, msg_prio: unsigned int, abs_timeout: const struct timespec*` | 定时发送消息 |
| 183 | mq_timedreceive | `mqdes: mqd_t, msg_ptr: char*, msg_len: size_t, msg_prio: unsigned int*, abs_timeout: const struct timespec*` | 定时接收消息 |
| 184 | mq_notify | `mqdes: mqd_t, sevp: const struct sigevent*` | 注册消息到达通知 |
| 185 | mq_getsetattr | `mqdes: mqd_t, newattr: const struct mq_attr*, oldattr: struct mq_attr*` | 获取/设置消息队列属性 |

### 20. 权限与安全

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 277 | seccomp | `operation: unsigned int, flags: unsigned int, args: void*` | 设置安全计算模式（seccomp 过滤器） |
| 444 | landlock_create_ruleset | `attr: const struct landlock_ruleset_attr*, size: size_t, flags: __u32` | 创建 Landlock 规则集 |
| 445 | landlock_add_rule | `ruleset_fd: int, rule_type: __u64, rule_attr: const void*, flags: __u64` | 向 Landlock 规则集添加规则 |
| 446 | landlock_restrict_self | `ruleset_fd: int, flags: __u64` | 对自身施加 Landlock 规则集 |
| 459 | lsm_get_self_attr | `attr: unsigned int, ctx: struct lsm_ctx*, size: size_t*, flags: __u64` | 获取当前进程 LSM 属性 |
| 460 | lsm_set_self_attr | `attr: unsigned int, ctx: struct lsm_ctx*, size: size_t, flags: __u64` | 设置当前进程 LSM 属性 |
| 461 | lsm_list_modules | `ids: unsigned int*, size: size_t*, flags: __u64` | 列出已加载的 LSM 模块 |
| 278 | getrandom | `buf: void*, count: size_t, flags: unsigned int` | 获取随机数 |

### 21. 密钥管理

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 217 | add_key | `type: const char*, description: const char*, payload: const void*, plen: size_t, keyring: key_serial_t` | 添加密钥到内核密钥管理 |
| 218 | request_key | `type: const char*, description: const char*, callout: const char*, dest_keyring: key_serial_t` | 请求查找密钥 |
| 219 | keyctl | `operation: int, arg2: unsigned long, arg3: unsigned long, arg4: unsigned long, arg5: unsigned long` | 密钥管理控制（多功能） |

### 22. BPF 与追踪

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 280 | bpf | `cmd: int, attr: union bpf_attr*, size: unsigned int` | 执行 BPF 命令（加载/创建 map/查询） |
| 241 | perf_event_open | `attr: struct perf_event_attr*, pid: pid_t, cpu: int, group_fd: int, flags: unsigned long` | 打开性能监控事件 |
| 117 | ptrace | `request: enum __ptrace_request, pid: pid_t, addr: void*, data: void*` | 进程跟踪/调试 |

### 23. 内核模块与 kexec

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 105 | init_module | `module_image: void*, len: unsigned long, param_values: const char*` | 加载内核模块 |
| 273 | finit_module | `fd: int, param_values: const char*, flags: int` | 从 fd 加载内核模块 |
| 106 | delete_module | `name: const char*, flags: int` | 卸载内核模块 |
| 104 | kexec_load | `entry: unsigned long, nr_segments: unsigned int, segments: struct kexec_segment*, flags: unsigned long` | 加载 kexec 内核 |
| 294 | kexec_file_load | `kernel_fd: int, initrd_fd: int, cmdline: const char*, flags: unsigned long` | 从文件加载 kexec 内核 |

### 24. 线程同步 (futex)

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 98 | futex | `uaddr: int*, op: int, val: int, timeout: const struct timespec*, uaddr2: int*, val3: int` | 快速用户态互斥锁（传统接口） |
| 449 | futex_waitv | `waiters: struct futex_waitv*, nr_waiters: unsigned int, flags: unsigned int, timeout: struct timespec*, clockid: clockid_t` | 批量 futex 等待（多 uaddr） |
| 454 | futex_wake | `uaddr: int*, flags: unsigned long, val: int, ...` | futex 唤醒 |
| 455 | futex_wait | `uaddr: int*, val: int, timeout: struct timespec*, flags: unsigned long, ...` | futex 等待 |
| 456 | futex_requeue | `uaddr: int*, flags: unsigned long, val: int, val2: int, uaddr2: int*, ...` | futex 重排（移动等待者） |
| 99 | set_robust_list | `head: struct robust_list_head*, len: size_t` | 设置健壮 futex 列表 |
| 100 | get_robust_list | `pid: pid_t, head: struct robust_list_head**, len: size_t*` | 获取健壮 futex 列表 |

### 25. 内核通知与监控

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 116 | syslog | `type: int, buf: char*, len: int` | 内核日志管理（dmesg） |
| 142 | reboot | `magic: int, magic2: int, cmd: unsigned int, arg: void*` | 重启/关机/暂停系统 |
| — | dmesg | `type: int, buf: char*, len: int` | 内核日志管理（syslog 的别名，通过 syslog 实现，无独立 ARM64 编号） |
| — | sysfs | `option: int, ...` | 获取文件系统类型信息（已废弃，无独立 ARM64 编号） |
| — | sysctl | `args: struct __sysctl_args*` | 内核参数查询/设置（已废弃，通过 /proc/sys 替代，无独立 ARM64 编号） |

### 26. NUMA 内存策略

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 235 | mbind | `addr: const void*, len: unsigned long, mode: int, nmask: const unsigned long*, maxnode: unsigned long, flags: unsigned int` | 设置内存绑定策略 |
| 236 | get_mempolicy | `policy: int*, nmask: unsigned long*, maxnode: unsigned long, addr: unsigned long, flags: unsigned long` | 获取内存策略 |
| 237 | set_mempolicy | `mode: int, nmask: const unsigned long*, maxnode: unsigned long` | 设置内存策略 |
| 238 | migrate_pages | `pid: pid_t, maxnode: unsigned long, old_nodes: const unsigned long*, new_nodes: const unsigned long*` | 迁移进程页到指定节点 |
| 239 | move_pages | `pid: pid_t, nr_pages: unsigned long, pages: const void**, nodes: const int*, status: int*, flags: int` | 移动指定页到节点 |
| 450 | set_mempolicy_home_node | `addr: unsigned long, len: unsigned long, home_node: unsigned long, flags: unsigned long` | 设置内存策略 home 节点 |

### 27. 系统标识与信息

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 160 | uname | `buf: struct old_utsname*` | 获取系统名和版本信息 |
| 161 | sethostname | `name: const char*, len: size_t` | 设置主机名 |
| 162 | setdomainname | `name: const char*, len: size_t` | 设置域名 |
| 168 | getcpu | `cpu: unsigned int*, node: unsigned int*, cache: struct getcpu_cache*` | 获取当前 CPU/节点 ID |
| 153 | times | `buf: struct tms*` | 获取进程时间信息 |
| 451 | cachestat | `fd: unsigned int, cstat_range: struct cachestat_range*, cstat: struct cachestat*, flags: unsigned int` | 查询文件页缓存状态 |
| 470 | listns | `fd: int, last_ns_id: __u64, ns_ids: struct mnt_id_req*, size: size_t, flags: unsigned int` | 列出命名空间 |

### 28. 用户与组关系

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 434 | pidfd_open | `pid: pid_t, flags: unsigned int` | 通过 pid 获取文件描述符 |
| 438 | pidfd_getfd | `pidfd: int, targetfd: int, flags: unsigned int` | 通过 pidfd 获取其他进程的 fd 副本 |
| 268 | setns | `fd: int, nstype: int` | 加入指定命名空间 |
| 293 | rseq | `rseq: struct rseq*, rseq_len: __u32, flags: int, sig: __u32` | 注册可重启序列（用户态快速锁） |
| 471 | rseq_slice_yield | `rseq_slice: struct rseq_slice*, rseq_slice_len: __u32, flags: unsigned long` | rseq 切片让出 |
| 283 | membarrier | `cmd: int, flags: unsigned int, cpu_id: int` | 内存屏障指令 |
| 272 | kcmp | `pid1: pid_t, pid2: pid_t, type: int, idx1: unsigned long, idx2: unsigned long` | 比较两个进程的 kernel 资源是否相同 |

### 29. 内存文件系统

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 279 | memfd_create | `name: const char*, flags: unsigned int` | 创建匿名文件（内存文件描述符） |
| 447 | memfd_secret | `name: const char*, flags: unsigned int` | 创建保密内存区域 fd |

### 30. 其他杂项

| 序号 | 名称 | 参数 | 说明 |
|--|--|--|--|
| 128 | restart_syscall | 无参数 | 重启被信号中断的系统调用 |
| 58 | vhangup | 无参数 | 挂起当前终端 |
| 213 | readahead | `fd: int, offset: off_t, count: size_t` | 预读文件数据到页缓存 |
| 102 | getitimer | `which: int, curr: struct itimerval*` | 获取定时器值（旧式） |
| 103 | setitimer | `which: int, new: const struct itimerval*, old: struct itimerval*` | 设置定时器（旧式） |
| 163 | getrlimit | `resource: int, rlim: struct rlimit*` | 获取资源限制 |
| 164 | setrlimit | `resource: int, rlim: const struct rlimit*` | 设置资源限制 |
| 165 | getrusage | `who: int, usage: struct rusage*` | 获取资源使用情况 |
| 261 | prlimit64 | `pid: pid_t, resource: int, new_limit: const struct rlimit64*, old_limit: struct rlimit64*` | 扩展版 get/setrlimit（64位） |
| 270 | process_vm_readv | `pid: pid_t, liov: const struct iovec*, liovcnt: int, riov: const struct iovec*, riovcnt: int, flags: unsigned long` | 跨进程读内存 |
| 271 | process_vm_writev | `pid: pid_t, liov: const struct iovec*, liovcnt: int, riov: const struct iovec*, riovcnt: int, flags: unsigned long` | 跨进程写内存 |
| — | syscall | `number: unsigned long, ...` | 间接系统调用（通过编号调用任意 syscall，无独立 ARM64 编号） |
| 244 | cacheflush | `addr: unsigned long, scope: unsigned long, flags: unsigned long` | 缓存刷写（ARC/CSKY/NIOS2 架构专用，ARM64 上为 stub） |
| 245 | arc_settls | `tls: unsigned long` | 设置线程本地存储（ARC 架构专用） |
| 246 | arc_gettls | 无参数 | 获取线程本地存储（ARC 架构专用） |
| 248 | arc_usr_cmpxchg | `oldval: unsigned long, newval: unsigned long, ...` | 用户态比较交换（ARC 架构专用） |
| 244 | set_thread_area | `u_info: struct user_desc*` | 设置线程本地存储段（CSKY 架构专用） |
| 258 | riscv_hwprobe | `pairs: struct riscv_hwprobe*, pair_count: size_t, cpu: unsigned long, flags: unsigned long` | RISC-V 硬件探测 |
| 259 | riscv_flush_icache | `start: unsigned long, end: unsigned long, flags: unsigned long` | 刷写指令缓存（RISC-V 专用） |
| 244 | or1k_atomic | `type: unsigned long, ...` | OpenRISC 原子操作 |

---

## 参数类型速查

| 类型 | 说明 |
|--|--|
| `int` / `unsigned int` | 32 位整数/无符号整数 |
| `long` / `unsigned long` | 64 位整数（ARM64 上） |
| `size_t` | 大小（64 位无符号） |
| `off_t` / `loff_t` | 文件偏移（有符号 64 位） |
| `pid_t` / `uid_t` / `gid_t` | 进程 ID / 用户 ID / 组 ID（32 位） |
| `mode_t` | 文件权限模式（32 位） |
| `dev_t` | 设备号（32 位） |
| `key_t` | System V IPC 键（32 位） |
| `mqd_t` | 消息队列描述符（int） |
| `clockid_t` | 时钟 ID（int） |
| `timer_t` | POSIX 定时器 ID（指针） |
| `const char*` / `char*` | 用户空间字符串指针 |
| `void*` / `const void*` | 用户空间缓冲区指针 |
| `struct iovec*` | 分散/聚集 I/O 向量 |
| `struct timespec*` | 纳秒精度时间结构 |
| `sigset_t*` | 信号集 |
| `struct sockaddr*` | 套接字地址 |
| `socklen_t` | 套接字地址长度（32 位） |
| `cpu_set_t*` | CPU 集合（亲和性掩码） |
| `struct rusage*` | 资源使用统计 |
| `struct sched_param*` | 调度参数 |
| `union bpf_attr*` | BPF 属性（命令特定） |

---

## 备注

1. 本表基于 `arch/arm64/tools/syscall_64.tbl` 自动构建，覆盖编号 0-471
2. 同一栏位出现两个不同序号（如 62 llseek/lseek）表示 32 位与 64 位使用不同实现
3. 标记为 `compat` 的 syscall 用于 32 位（AArch32）兼容层
4. 带 `at` 后缀的系统调用（如 `openat`）使用目录 fd 作为参考点，避免 TOCTOU 竞争
5. 最新内核（6.x）新增系统调用包括：`futex_wake`/`futex_wait`/`futex_requeue`、`mseal`、`map_shadow_stack`、`lsm_*`、`cachestat` 等
