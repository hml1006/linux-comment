# Linux 文件系统 (VFS) 核心流程分析

> 本文档覆盖 Linux 内核 VFS 层的核心流程、数据结构与特殊文件系统。
> 每个流程均以函数调用栈 ASCII 树形式呈现, 并附有关键数据结构变更注释。

## 目录

- **Part I: VFS 框架总览**
  - [1. VFS 框架结构总览](#1-vfs-框架结构总览)
- **Part II: 文件系统与挂载**
  - [2. 根目录创建过程](#2-根目录创建过程)
  - [3. 文件系统类型链表](#3-文件系统类型链表)
  - [4. 挂载树结构](#4-挂载树结构)
  - [5. 挂载过程](#5-挂载过程)
- **Part III: 文件操作流程**
  - [6. 文件打开路径](#6-文件打开路径)
  - [7. 路径查找过程](#7-路径查找过程)
  - [8. 文件读写路径](#8-文件读写路径)
- **Part IV: 核心数据结构**
  - [9. VFS 核心数据结构详解](#9-vfs-核心数据结构详解)
    - [9.1 super_block](#91-super_block--超级块)
    - [9.2 inode](#92-inode--索引节点)
    - [9.3 dentry](#93-dentry--目录项)
    - [9.4 file](#94-file--文件对象)
    - [9.5 vfsmount / mount](#95-vfsmount--mount--挂载实例)
    - [9.6 file_system_type](#96-file_system_type--文件系统类型)
    - [9.7 address_space](#97-address_space--地址空间-页缓存核心)
    - [9.8 数据结构关系汇总](#98-数据结构关系汇总)
- **Part V: 特殊文件系统**
  - [10. 特殊文件系统概述](#10-特殊文件系统概述)
    - [10.1 procfs](#101-procfs--进程文件系统)
    - [10.2 sysfs](#102-sysfs--内核对象文件系统)
    - [10.3 debugfs](#103-debugfs--调试文件系统)
    - [10.4 ramfs / tmpfs](#104-ramfs--tmpfs--内存文件系统)
    - [10.5 bdevfs](#105-bdevfs--块设备文件系统)
    - [10.6 sockfs](#106-sockfs--套接字文件系统)
    - [10.7 ext4](#107-ext4--第四代扩展文件系统)
    - [10.8 cgroup / cgroup2](#108-cgroup--cgroup2--控制组文件系统)
    - [10.9 devtmpfs](#109-devtmpfs--设备节点文件系统)
    - [10.10 configfs](#1010-configfs--配置对象文件系统)
    - [10.11 securityfs](#1011-securityfs--安全模块文件系统)
    - [10.12 bpf](#1012-bpf--bpf-文件系统)
    - [10.13 pipefs](#1013-pipefs--管道文件系统)
    - [10.14 hugetlbfs](#1014-hugetlbfs--大页文件系统)
    - [10.15 rpc_pipefs](#1015-rpc_pipefs--rpc-管道文件系统)
    - [10.16 devpts](#1016-devpts--伪终端文件系统)
- **Part VI: 文件系统与块设备链路**
  - [11. 文件读写与 NVMe 块设备挂载链路关键数据结构](#11-文件读写与-nvme-块设备挂载链路关键数据结构)
    - [11.1 文件打开链路 (sys_openat → PCIe)](#111-文件打开链路-sys_openat--pcie-设备)
    - [11.2 文件读写链路 (sys_read/sys_write → PCIe)](#112-文件读写链路-sys_readsys_write--pcie-设备)
    - [11.3 NVMe 块设备挂载链路 (mount → PCIe)](#113-nvme-块设备挂载链路-mount--pcie-设备)
    - [11.4 数据结构关系图](#114-数据结构关系图)
    - [11.5 关键数据结构关系表](#115-关键数据结构关系表)
    - [11.6 关键链路总结](#116-关键链路总结)

---

## Part I: VFS 框架总览

## 1. VFS 框架结构总览

### 1.1 VFS 分层架构

```text
# Linux VFS 分层架构
#
# VFS 是内核中文件系统访问的统一抽象层, 它将不同文件系统的实现差异
# 隐藏在统一的操作接口之下。用户态程序通过系统调用与 VFS 交互,
# VFS 再将请求分派到具体的文件系统实现。

                       用户态
┌───────────────────────────────────────────────┐
│  应用程序 (open/read/write/stat/...)          │
└──────────────────┬────────────────────────────┘
                   │ 系统调用 (syscall)
                   ▼
┌───────────────────────────────────────────────┐
│  系统调用层 (sys_openat / sys_read / ...)      │
│  fs/read_write.c, fs/open.c, fs/stat.c        │
├───────────────────────────────────────────────┤
│  VFS 通用层                                    │
│  ┌──────────────┐  ┌──────────────────┐      │
│  │  VFS 核心     │  │  路径查找        │      │
│  │  vfs_open,   │  │  path_openat,    │      │
│  │  vfs_read,   │  │  link_path_walk, │      │
│  │  vfs_iterate │  │  walk_component  │      │
│  └──────┬───────┘  └────────┬─────────┘      │
│         │                   │                 │
│         ▼                   ▼                 │
│  ┌──────────────────────────────────────┐     │
│  │  通用服务层                          │     │
│  │  ┌───────────┐ ┌────────────────┐   │     │
│  │  │ 页缓存    │ │  文件锁       │    │     │
│  │  │ (page     │ │ (flock/       │    │     │
│  │  │  cache)   │ │  fcntl)       │    │     │
│  │  └───────────┘ └────────────────┘   │     │
│  │  ┌───────────┐ ┌────────────────┐   │     │
│  │  │ mount     │ │ 命名空间       │    │     │
│  │  │ 子系统    │ │ (namespace)    │    │     │
│  │  └───────────┘ └────────────────┘   │     │
│  └──────────────────────────────────────┘     │
├───────────────────────────────────────────────┤
│  文件系统接口层 (VFS 对象)                     │
│  ┌──────────┐ ┌──────────┐ ┌──────────────┐  │
│  │super_ops  │ │inode_ops  │ │dentry_ops   │  │
│  │file_ops   │ │address_   │ │export_ops   │  │
│  │           │ │space_ops  │ │             │  │
│  └─────┬─────┘ └─────┬─────┘ └──────┬──────┘  │
└────────┼──────────────┼──────────────┼─────────┘
         │              │              │
         ▼              ▼              ▼
┌───────────────────────────────────────────────┐
│  具体文件系统实现                              │
│  ┌────────┐ ┌────────┐ ┌──────┐ ┌─────────┐ │
│  │  ext4  │ │  btrfs │ │ xfs  │ │  procfs │ │
│  │  ext4_ │ │  btrfs_│ │ xfs_ │ │  proc_  │ │
│  │  *.c   │ │  *.c   │ │ *.c  │ │  *.c    │ │
│  └────────┘ └────────┘ └──────┘ └─────────┘ │
│  ┌────────┐ ┌────────┐ ┌──────┐ ┌─────────┐ │
│  │  ramfs │ │  sysfs │ │sockfs│ │  debugfs│ │
│  └────────┘ └────────┘ └──────┘ └─────────┘ │
├───────────────────────────────────────────────┤
│  块设备层 (Block Layer)                       │
│  bio, request_queue, IO scheduler             │
├───────────────────────────────────────────────┤
│  设备驱动层 (Device Driver)                   │
│  NVMe, AHCI, virtio-blk, ...                  │
└───────────────────────────────────────────────┘
                       内核态
```

### 1.2 VFS 四大对象关系

```text
# VFS 基于四个核心对象构建整个文件系统抽象:
#
#    super_block: 已挂载文件系统的实例描述
#    inode:      文件/目录的元数据 (权限, 大小, 位置等)
#    dentry:     目录项, 路径名与 inode 之间的桥梁
#    file:       进程打开的文件描述 (读写位置, 标志等)
#
# 关系图:

┌─────────────────────────────────────────────────────────────────┐
│  file_system_type (文件系统类型)                                  │
│  name = "ext4", init_fs_context, kill_sb                         │
│  fs_supers: 链表, 指向所有该类型的 super_block 实例               │
└──────────────────────────┬──────────────────────────────────────┘
                           │ s_type
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  super_block (超级块 - 每个文件系统实例一个)                       │
│  s_dev, s_blocksize, s_magic, s_root(根 dentry)                  │
│  s_op: super_operations (alloc_inode, write_inode, ...)          │
│  s_inodes: 链表, 该文件系统所有 inode                             │
│  s_dentry_lru: dentry LRU 缓存                                   │
│  s_bdi: 后备设备信息 (backing device info)                        │
│  s_fs_info: 指向文件系统私有数据 (如 ext4_sb_info)                │
└──────┬───────────────────────────────────────────────────────────┘
       │ s_bdev / s_bdev_file
       │ s_mounts → mount 链表
       ▼
┌─────────────────────────────────────────────────────────────────┐
│  mount / vfsmount (挂载实例 - 每个挂载点一个)                    │
│  mnt_root: 该挂载的根 dentry                                     │
│  mnt_sb:   指向 super_block                                       │
│  mnt_parent: 父挂载 (形成挂载树)                                  │
│  mnt_mountpoint: 在父挂载中的挂载点 dentry                        │
└──────┬───────────────────────────────────────────────────────────┘
       │ mnt_root
       ▼
┌─────────────────────────────────────────────────────────────────┐
│  dentry (目录项 - 路径名组件缓存)                                │
│  d_name: 文件名 (如 "usr", "bin")                                │
│  d_parent: 父目录 dentry                                          │
│  d_inode: 指向对应的 inode (NULL 表示负 dentry)                   │
│  d_op: dentry_operations (d_revalidate, d_hash, ...)             │
│  d_sb: 所属 super_block                                           │
│  d_sib: 兄弟链表 (同一父目录的子目录项)                           │
│  d_subdirs: 子目录链表                                            │
│  d_flags: DCACHE_MOUNTED (挂载点), DCACHE_DISCONNECTED 等        │
└──────┬───────────────────────────────────────────────────────────┘
       │ d_inode
       ▼
┌─────────────────────────────────────────────────────────────────┐
│  inode (索引节点 - 文件元数据)                                    │
│  i_mode: 文件类型 + 权限                                          │
│  i_uid, i_gid: 属主/属组                                          │
│  i_size: 文件大小                                                  │
│  i_ino: inode 编号                                                │
│  i_nlink: 硬链接计数                                               │
│  i_op: inode_operations (lookup, create, mkdir, ...)             │
│  i_fop: file_operations (open, read_iter, write_iter, ...)       │
│  i_mapping → address_space (页缓存)                              │
│  i_data: 内嵌的 address_space 实例                                │
└──────┬───────────────────────────────────────────────────────────┘
       │ i_mapping
       ▼
┌─────────────────────────────────────────────────────────────────┐
│  address_space (地址空间 - 页缓存核心)                           │
│  host: 所属 inode                                                 │
│  i_pages: XArray, 存储该文件的所有缓存页                          │
│  a_ops: address_space_operations (read_folio, writepages,...)    │
│  nrpages: 缓存页数量                                              │
│  i_mmap: 共享/私有内存映射红黑树                                  │
└─────────────────────────────────────────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────────────────────────────────┐
│  file (文件对象 - 进程打开的文件)                                 │
│  f_path: 文件路径 (dentry + vfsmount)                            │
│  f_inode: 指向文件的 inode                                        │
│  f_op: file_operations 操作表                                     │
│  f_pos: 当前读写位置                                               │
│  f_flags: 打开标志 (O_RDONLY, O_SYNC, ...)                       │
│  f_count: 引用计数                                                 │
│  private_data: 文件系统私有数据                                    │
│  f_ra: 预读状态 (readahead state)                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 1.3 系统调用到 VFS 的映射

```text
# 常见系统调用与 VFS 操作表的对应关系

  系统调用          VFS 函数            文件系统回调
  ──────────       ────────────        ───────────────────
  open()      →    do_filp_open()  →   inode->i_op->lookup()
                                        file->f_op->open()
  read()      →    vfs_read()      →   file->f_op->read_iter()
  write()     →    vfs_write()     →   file->f_op->write_iter()
  stat()      →    vfs_statx()     →   inode->i_op->getattr()
  mkdir()     →    vfs_mkdir()     →   inode->i_op->mkdir()
  unlink()    →    vfs_unlink()    →   inode->i_op->unlink()
  rename()    →    vfs_rename()    →   inode->i_op->rename()
  mmap()      →    mmap_region()   →   file->f_op->mmap()
  ioctl()     →    vfs_ioctl()     →   file->f_op->unlocked_ioctl()
  fsync()     →    vfs_fsync()     →   file->f_op->fsync()
  mount()     →    do_mount()      →   fs_type->init_fs_context()
                                        fill_super()
  umount()    →    path_umount()   →   sb->s_op->put_super()
  sync()      →    ksys_sync()     →   sb->s_op->sync_fs()
```

---

## Part II: 文件系统与挂载

## 2. 根目录创建过程

```text
# 根目录创建过程 - 函数调用栈 (start_kernel → 根文件系统挂载)

start_kernel
  │  # 内核 C 语言入口, 初始化所有子系统
  │
  ├─ vfs_caches_init
  │   │  # 初始化 VFS 层缓存结构:
  │   │  #   - dentry_cache (目录项缓存)
  │   │  #   - filp_cache  (文件对象缓存)
  │   │  #   - inode_cache (inode 缓存)
  │   │  #   - mnt_cache   (挂载结构缓存)
  │   │  #   - 注册文件系统类型链表 (file_systems)
  │   │
  │   └─ mnt_init
  │       │  # 初始化挂载子系统:
  │       │   - 创建 sysfs 挂载条目 (sysfs_mount)
  │       │   - 创建 /proc/fs 目录
  │       │   - 初始化共享挂载树 (mount_hashtable)
  │       │
  │       └─ init_mount_tree
  │           │  # 创建初始根文件系统挂载树
  │           │  # 分配 rootfs 的 super_block, root dentry, mount 结构
  │           │  # rootfs 是 ramfs 的一种, 在内存中创建
  │           │
  │           ├─ mnt_add_to_ns
  │           │   # 将 rootfs 挂载添加到 init_mnt_ns 命名空间
  │           │   # init_mnt_ns 是全局初始挂载命名空间
  │           │   # 所有后续挂载都以 rootfs 为根
  │           │
  │           ├─ set_fs_root
  │           │   # 设置 init_task 进程的根目录:
  │           │   #   current->fs->root = rootfs 的 dentry
  │           │   #   current->fs->pwd  = rootfs 的 dentry
  │           │   # 此时进程的根目录和当前目录都是 "/"
  │           │
  │           └─ ns_tree_add
  │               # 将 init_mnt_ns 添加到全局命名空间红黑树
  │               # 便于通过 namespace id 快速查找
  │
  # 至此, 内核拥有了最基本的根文件系统, 可以访问 "/"
  # 后续 init 进程会在此基础上挂载 proc/sysfs/devtmpfs 等
  # 形成完整的文件系统树
```

---

## 3. 文件系统类型 - 链表结构

```text
# 已注册文件系统类型链表 (file_systems 全局链表)
# 所有文件系统通过 register_filesystem() 注册到该链表
# 挂载时根据名称遍历链表查找匹配的 file_system_type

file_systems (链表头)
  │
  ├─ sysfs_fs_type
  │    name          = "sysfs"
  │    init_fs_context = sysfs_init_fs_context
  │    kill_sb       = sysfs_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT
  │    # 使用场景: 暴露内核对象层次结构 (kobject), 提供设备/驱动/总线/类等
  │    #           用户空间通过 /sys 访问硬件拓扑和内核参数
  │
  ├─ shmem_fs_type
  │    name          = "tmpfs"
  │    init_fs_context = shmem_init_fs_context
  │    kill_sb       = kill_litter_super
  │    fs_flags      = FS_USERNS_MOUNT | FS_ALLOW_IDMAP | FS_MGTIME
  │    # 使用场景: 内存文件系统, 用于 /tmp, /dev/shm, 容器 overlay 上层
  │    #           支持大小限制和 swap 后援, 重启后数据丢失
  │
  ├─ bd_type
  │    name          = "bdev"
  │    init_fs_context = bd_init_fs_context
  │    kill_sb       = kill_anon_super
  │    # 使用场景: 管理块设备 inode 和页缓存, 内核内部使用不直接挂载
  │    #           文件系统通过 sb_bread() 读取块设备元数据 (super_block, inode 表等)
  │
  ├─ proc_fs_type
  │    name          = "proc"
  │    init_fs_context = proc_init_fs_context
  │    kill_sb       = proc_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT | FS_DISALLOW_NOTIFY_PERM
  │    # 使用场景: 暴露进程信息 (/proc/[pid]/) 和内核参数 (/proc/sys/),
  │    #           每个进程对应一个目录, 内容读取时动态生成
  │
  ├─ cgroup_fs_type
  │    name          = "cgroup"
  │    init_fs_context = cgroup_init_fs_context
  │    kill_sb       = cgroup_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT
  │    # 使用场景: cgroup v1 接口, 对进程分组并限制资源 (CPU/内存/IO)
  │    #           挂载到 /sys/fs/cgroup/, 每个控制器对应一个子目录
  │
  ├─ cgroup2_fs_type
  │    name          = "cgroup2"
  │    init_fs_context = cgroup_init_fs_context
  │    kill_sb       = cgroup_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT
  │    # 使用场景: cgroup v2 统一接口, 替代 v1 的单一层次结构
  │    #           提供统一的资源控制树, 支持线程模式, 无控制器冲突
  │
  ├─ dev_fs_type
  │    name          = "devtmpfs"
  │    init_fs_context = devtmpfs_init_fs_context
  │    # 使用场景: 管理 /dev 下的设备节点, 内核自动创建设备文件
  │    #           配合 udev/mdev 在用户空间完成设备命名和权限管理
  │
  ├─ configfs_fs_type
  │    name          = "configfs"
  │    init_fs_context = configfs_init_fs_context
  │    kill_sb       = kill_litter_super
  │    # 使用场景: 内核对象配置接口, 用户空间通过 mkdir/rmdir 创建/删除内核对象
  │    #           典型用途: 内核 target (iSCSI), 创建目录即创建对象
  │
  ├─ debug_fs_type
  │    name          = "debugfs"
  │    init_fs_context = debugfs_init_fs_context
  │    kill_sb       = kill_litter_super
  │    # 使用场景: 内核调试接口, 开发时暴露寄存器/计数器/运行时参数
  │    #           挂载到 /sys/kernel/debug/, 生产环境可能关闭 CONFIG_DEBUG_FS
  │
  ├─ fs_type (securityfs)
  │    name          = "securityfs"
  │    init_fs_context = securityfs_init_fs_context
  │    kill_sb       = kill_litter_super
  │    # 使用场景: LSM (Linux Security Module) 接口, 如 SELinux/AppArmor/IMA
  │    #           挂载到 /sys/kernel/security/, 暴露安全策略和属性文件
  │
  ├─ sock_fs_type
  │    name          = "sockfs"
  │    init_fs_context = sockfs_init_fs_context
  │    kill_sb       = kill_anon_super
  │    # 使用场景: 为 socket 提供 VFS 文件接口, 使 socket() 返回文件描述符
  │    #           内核内部使用, 不直接挂载, 通过 file->private_data 指向 socket
  │
  ├─ bpf_fs_type
  │    name          = "bpf"
  │    init_fs_context = bpf_init_fs_context
  │    kill_sb       = bpf_kill_super
  │    fs_flags      = FS_USERNS_MOUNT
  │    # 使用场景: BPF 文件系统, 挂载到 /sys/fs/bpf/, 管理 BPF 程序和 map
  │    #           BPF 程序可通过 pin 操作持久化, 供其他进程访问
  │
  ├─ pipe_fs_type
  │    name          = "pipefs"
  │    init_fs_context = pipefs_init_fs_context
  │    kill_sb       = kill_anon_super
  │    # 使用场景: 为 pipe() 系统调用提供 VFS 文件接口, 内核内部使用
  │    #           管道文件对象通过 pipefs 分配 inode 和 file 结构
  │
  ├─ ramfs_fs_type
  │    name          = "ramfs"
  │    init_fs_context = ramfs_init_fs_context
  │    kill_sb       = ramfs_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT
  │    # 使用场景: 极简内存文件系统, 用于 rootfs (初始根文件系统)
  │    #           无大小限制, 不可 swap, 数据占物理内存直至重启
  │
  ├─ hugetlbfs_fs_type
  │    name          = "hugetlbfs"
  │    init_fs_context = hugetlbfs_init_fs_context
  │    kill_sb       = kill_litter_super
  │    fs_flags      = FS_ALLOW_IDMAP
  │    # 使用场景: 大页文件系统, 挂载到 /dev/hugepages/, 支持 2MB/1GB 大页
  │    #           用于数据库/虚拟化等需要大块连续物理内存的场景
  │
  ├─ rpc_pipe_fs_type
  │    name          = "rpc_pipefs"
  │    init_fs_context = rpc_init_fs_context
  │    kill_sb       = rpc_kill_sb
  │    # 使用场景: NFS 客户端与 RPC 服务之间的通信管道
  │    #           挂载到 /var/lib/nfs/rpc_pipefs/, 管理 NFS 认证和凭据
  │
  ├─ devpts_fs_type
  │    name          = "devpts"
  │    init_fs_context = devpts_init_fs_context
  │    kill_sb       = devpts_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT
  │    # 使用场景: 伪终端 (PTY) 文件系统, 挂载到 /dev/pts/
  │    #           每个 SSH/telnet/终端模拟器会话对应一个 /dev/pts/N
  │
  └─ ext4_fs_type
       name          = "ext4"
       init_fs_context = ext4_init_fs_context
       kill_sb       = ext4_kill_sb
       fs_flags      = FS_REQUIRES_DEV | FS_ALLOW_IDMAP | FS_MGTIME
       # 使用场景: 通用磁盘文件系统, 广泛用于 Linux 根文件系统和数据盘
       #           支持日志/延迟分配/extents/Htree 目录索引等特性
       next          = NULL (链表尾)
```

---

## 4. 挂载树结构

```text
# Linux 挂载树结构 - 命名空间 / mount / dentry 关系图
#
# 核心概念:
#   init_mnt_ns: 全局初始挂载命名空间, 所有进程共享
#   mount: 每个挂载点对应一个 mount 结构, 通过 mnt_parent 形成树
#   dentry: 目录项, 通过 d_parent 形成路径
#   mountpoint: 连接 mount 和 dentry 的桥梁

┌─────────────────────────────────────────────────────────────┐
│  init_mnt_ns (初始挂载命名空间)                               │
│  root ───────────────────────────────────┐                    │
└──────────────────────────────────────────│────────────────────┘
                                           │
                                           ▼
                          ┌───────────────────────────────────────┐
                          │  mount_root (根挂载 - rootfs)         │
                          │  mnt_parent  = self (指向自身)         │
                          │  mnt_mountpoint = dentry_root (/)      │
                          │  mnt_root       = dentry_root          │
                          │  mnt.mnt_sb     = sb_rootfs            │
                          └──────────┬────────────────────────────┘
                                     │
              ┌──────────────────────┼──────────────────────┐
              │                      │                      │
              ▼                      ▼                      ▼
┌──────────────────────┐ ┌──────────────────────┐ ┌──────────────────────┐
│ mount_proc            │ │ mount_sys             │ │ mount_nvme            │
│ (/proc 挂载)          │ │ (/sys 挂载)           │ │ (/mnt/nvme 挂载)      │
│ mnt_parent= mount_root│ │ mnt_parent= mount_root│ │ mnt_parent= mount_root│
│ mnt_mountpoint= /proc │ │ mnt_mountpoint= /sys  │ │ mnt_mountpoint= /mnt/ │
│ mnt_root= dentry_proc │ │ mnt_root= dentry_sys  │ │   nvme                │
│   _root               │ │   _root               │ │ mnt_root= dentry_nvme │
│ mnt.mnt_sb = sb_proc  │ │ mnt.mnt_sb = sb_sysfs │ │   _root               │
│                       │ │                       │ │ mnt.mnt_sb = sb_ext4  │
└──────────┬────────────┘ └──────────┬────────────┘ └──────────┬────────────┘
           │                        │                        │
           │  mountpoint           │  mountpoint            │  mountpoint
           ▼                        ▼                        ▼
┌──────────────────────┐ ┌──────────────────────┐ ┌──────────────────────┐
│ mp_proc               │ │ mp_sys                │ │ mp_nvme               │
│ 挂载点: /proc          │ │ 挂载点: /sys          │ │ 挂载点: /mnt/nvme     │
│ m_dentry = dentry_proc│ │ m_dentry = dentry_sys │ │ m_dentry = dentry_nvme│
└──────────┬────────────┘ └──────────┬────────────┘ └──────────┬────────────┘
           │                        │                        │
           │ 关联                    │ 关联                    │ 关联
           ▼                        ▼                        ▼
┌──────────────────────┐ ┌──────────────────────┐ ┌──────────────────────┐
│ dentry: /proc         │ │ dentry: /sys          │ │ dentry: /mnt/nvme    │
│ d_parent = dentry_root│ │ d_parent = dentry_root│ │ d_parent = dentry_mnt│
│ d_inode = inode_proc  │ │ d_inode = inode_sys   │ │ d_inode = inode_nvme │
│ flags = DCACHE_MOUNTED│ │ flags = DCACHE_MOUNTED│ │ flags = DCACHE_MOUNTED
└──────────────────────┘ └──────────────────────┘ └──────────────────────┘
                                                                    │
                                                                    │ d_parent
                                                                    ▼
                                                          ┌──────────────────────┐
                                                          │ dentry: /mnt         │
                                                          │ d_parent = dentry_root│
                                                          │ d_inode = inode_mnt   │
                                                          │ flags = 0             │
                                                          └──────────┬───────────┘
                                                                     │
                                                                     │ d_parent
                                                                     ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ dentry_root (/)                                                              │
│ d_parent = dentry_root (根目录指向自身)                                       │
│ d_inode = inode_root                                                         │
│                                                                              │
│ 子目录: /proc, /sys, /mnt                                                    │
│                                                                              │
│ DCACHE_MOUNTED 标志说明:                                                     │
│ 当访问标记了 DCACHE_MOUNTED 的 dentry 时, VFS 会触发"穿越"(cross-mount)       │
│ 即跳转到该挂载点对应的子文件系统的根目录 dentry, 而不是返回父 FS 中的 dentry    │
│ 例如访问 /proc 时, 实际进入的是 proc 文件系统的根目录 (dentry_proc_root)       │
└──────────────────────────────────────────────────────────────────────────────┘

# 子文件系统根目录 (当发生穿越时进入)

  dentry_proc_root (proc 根目录)     dentry_sys_root (sys 根目录)
  d_name = "/"                       d_name = "/"
  d_parent = self                    d_parent = self
  mounts: proc 文件系统               mounts: sysfs 文件系统

  dentry_nvme_root (NVMe 根目录)
  d_name = "/"
  d_parent = self
  mounts: ext4 文件系统 (NVMe 盘)
```

---
## 5. 挂载过程

```text
# 挂载过程 - 函数调用栈 (mount 系统调用 → ext4 挂载完成)

mount
  │  # 系统调用入口: sys_mount
  │  # 从用户空间拷贝文件系统类型, 设备路径, 挂载参数到内核空间
  │
  └─ do_mount
      │  # 检查挂载参数合法性, 开始挂载流程
      │
      ├─ user_path_at
      │   │  # 拷贝用户空间传递的目录路径到内核 struct path
      │   │
      │   └─ filename_lookup
      │       │  # 查找挂载目录, 例如 /mnt/nvme/
      │       │  # 返回目标目录的 dentry 和 vfsmount
      │       │
      │       └─ path_lookupat
      │           # 路径查找, 解析符号链接, 逐级查找 dentry
      │           # 参考文件 open 过程中的路径查找逻辑
      │
      └─ path_mount
          │  # 获取当前挂载点信息, 进入新挂载创建流程
          │
          └─ do_new_mount
              │  # 核心: 创建新挂载并添加到挂载树
              │
              ├─ file_system_type
              │   # 根据名称查找已注册的文件系统类型
              │   # 遍历 file_systems 链表, 如 "ext4"
              │
              ├─ fs_context_for_mount
              │   # 创建文件系统上下文 (fs_context)
              │   # 用于在挂载过程中传递参数和状态
              │
              └─ do_new_mount_fc
                  │  # 根据文件系统上下文创建新的挂载
                  │
                  ├─ fc_mount
                  │   │  # 挂载文件系统上下文
                  │   │
                  │   └─ vfs_get_tree
                  │       │  # 获取文件系统的根 dentry 和 super_block
                  │       │
                  │       └─ fc->ops->get_tree()
                  │           │  # 调用文件系统特定的 get_tree 操作
                  │           │  # 例如 ext4_get_tree
                  │           │
                  │           └─ ext4_get_tree
                  │               │  # ext4 文件系统获取挂载树
                  │               │
                  │               └─ get_tree_bdev
                  │                   │  # 块设备文件系统通用挂载流程
                  │                   │
                  │                   └─ get_tree_bdev_flags
                  │                       │  # 实际的块设备挂载流程
                  │                       │
                  │                       ├─ lookup_bdev
                  │                       │   │  # 查找块设备路径, 获取设备号
                  │                       │   │
                  │                       │   ├─ kern_path
                  │                       │   │   │  # 构造 struct filename 并查找路径
                  │                       │   │   │
                  │                       │   │   └─ filename_lookup
                  │                       │   │       │  # 查找块设备文件
                  │                       │   │       │
                  │                       │   │       └─ path_lookupat
                  │                       │   │           # 路径查找, 解析块设备路径
                  │                       │   │
                  │                       │   └─ d_backing_inode
                  │                       │       # 获取块设备文件的 inode 和 dev_t 设备号
                  │                       │
                  │                       ├─ sget_dev
                  │                       │   │  # 根据设备号查找或创建 super_block
                  │                       │   │
                  │                       │   └─ sget_fc
                  │                       │       │  # 查找或创建 super_block
                  │                       │       │
                  │                       │       ├─ super_s_dev_test
                  │                       │       │   # 根据 dev_t 检查设备是否已挂载
                  │                       │       │   # 防止同一个设备挂载多次
                  │                       │       │
                  │                       │       ├─ alloc_super
                  │                       │       │   # 未找到已挂载的 super_block
                  │                       │       │   # 分配新的 super_block 结构
                  │                       │       │
                  │                       │       └─ super_s_dev_set
                  │                       │           # 设置 super_block 的 dev_t 设备号
                  │                       │
                  │                       ├─ setup_bdev_super
                  │                       │   │  # 设置块设备 super_block
                  │                       │   │  # 关联块设备与 super_block
                  │                       │   │
                  │                       │   ├─ bdev_file_open_by_dev
                  │                       │   │   │  # 打开块设备文件
                  │                       │   │   │
                  │                       │   │   ├─ blkdev_get_no_open
                  │                       │   │   │   │  # 获取块设备 block_device 结构
                  │                       │   │   │   │
                  │                       │   │   │   └─ ilookup
                  │                       │   │   │       # 在 inode 缓存中查找块设备 inode
                  │                       │   │   │
                  │                       │   │   └─ bdev_open
                  │                       │   │       # 打开块设备, 设置 file 的 address_space
                  │                       │   │
                  │                       │   └─ fill_super (ext4_fill_super)
                  │                       │       │  # 填充初始化 super_block
                  │                       │       │  # 调用具体文件系统的 fill_super 回调
                  │                       │       │
                  │                       │       └─ ext4_fill_super
                  │                       │           │  # ext4 文件系统填充 super_block
                  │                       │           │
                  │                       │           ├─ ext4_alloc_sbi
                  │                       │           │   # 分配 ext4 的 ext4_sb_info 结构
                  │                       │           │
                  │                       │           └─ __ext4_fill_super
                  │                       │               │  # 填充 ext4 的 super_block 细节
                  │                       │               │
                  │                       │               ├─ ext4_load_super
                  │                       │               │   # 从磁盘加载 ext4 super_block
                  │                       │               │
                  │                       │               ├─ ext4_init_metadata_csum
                  │                       │               │   # 初始化元数据校验 CRC
                  │                       │               │
                  │                       │               ├─ ext4_inode_info_init
                  │                       │               │   # 初始化 inode 相关信息
                  │                       │               │
                  │                       │               ├─ ext4_block_group_meta_init
                  │                       │               │   # 初始化块组元数据
                  │                       │               │
                  │                       │               ├─ ext4_hash_info_init
                  │                       │               │   # 初始化 hash 信息 (dx_dir)
                  │                       │               │
                  │                       │               ├─ ext4_handle_clustersize
                  │                       │               │   # 初始化 cluster size
                  │                       │               │   # bigalloc 特性: 每个 cluster = 2^cluster_bits
                  │                       │               │   # 复用 block bitmap, 实验性质
                  │                       │               │
                  │                       │               ├─ ext4_check_geometry
                  │                       │               │   # 检查文件系统几何结构
                  │                       │               │   # 如 block size, block count 等
                  │                       │               │
                  │                       │               ├─ ext4_group_desc_init
                  │                       │               │   # 初始化块组描述符
                  │                       │               │   # 将每个块组描述符从磁盘读入
                  │                       │               │   # 存放到 group_desc buffer_head 数组
                  │                       │               │
                  │                       │               ├─ ext4_es_register_shrinker
                  │                       │               │   # 注册 extent 状态树内存回收 shrinker
                  │                       │               │
                  │                       │               ├─ ext4_get_stripe_size
                  │                       │               │   # 获取 stripe size (针对 RAID 调优)
                  │                       │               │
                  │                       │               ├─ ext4_setup_super
                  │                       │               │   # 提交 super_block 变更到磁盘
                  │                       │               │
                  │                       │               ├─ ext4_mb_init
                  │                       │               │   # 初始化多块分配器 (multiblock allocator)
                  │                       │               │
                  │                       │               ├─ ext4_register_li_request
                  │                       │               │   # 注册延迟初始化请求
                  │                       │               │   # lazytime: mkfs 清零延迟到 mount 后做
                  │                       │               │
                  │                       │               ├─ ext4_init_orphan_info
                  │                       │               │   # 初始化 orphan inode 信息
                  │                       │               │   # 处理异常关机后遗留的孤儿 inode
                  │                       │               │
                  │                       │               ├─ ext4_superblock_csum_set
                  │                       │               │   # 设置 super_block 的校验和
                  │                       │               │
                  │                       │               └─ ext4_register_sysfs
                  │                       │                   # 注册 ext4 相关 sysfs 接口
                  │                       │
                  │                       └─ vfs_create_mount
                  │                           │  # 创建新的 vfsmount 结构
                  │                           │
                  │                           ├─ alloc_vfsmnt
                  │                           │   # 分配 vfsmount (mount) 结构体
                  │                           │   # 初始化引用计数, 挂载标志等
                  │                           │
                  │                           └─ setup_mnt
                  │                               │  # 设置挂载点信息
                  │                               │
                  │                               └─ mnt_add_instance
                  │                                   # 将 mount 添加到 super_block 的 s_mounts 链表
                  │                                   # 一个文件系统可同时挂载到多个挂载点
                  │
                  ├─ mount_too_revealing
                  │   # 检查挂载是否过于暴露
                  │   # 禁止非 root 用户挂载 proc/sysfs 等
                  │   # 防止暴露内部内核信息
                  │
                  ├─ mnt_warn_timestamp_expiry
                  │   # 检查文件系统时间戳是否即将到期
                  │   # 当前系统时间 + 30 年超过 s_time_max 时触发警告
                  │
                  └─ do_add_mount
                      # 将新创建的 mount 添加到命名空间的挂载树
                      #
                      # 内部流程:
                      #   1. lock_mount - 锁定挂载点 dentry
                      #   2. graft_tree  - 将新 mount 挂接到挂载树
                      #      ├─ __attach_mnt - 将 mount 添加到父 mount 的子列表
                      #      ├─ mnt_add_to_ns - 将 mount 添加到命名空间
                      #      └─ set_mnt_point - 设置 dentry 的 DCACHE_MOUNTED 标志
                      #   3. unlock_mount - 解锁
                      #
                      # 完成后, 通过该挂载点访问时触发"穿越"到子文件系统

---

## Part III: 文件操作流程

## 6. 文件打开路径

```text
# 文件打开路径 - 函数调用栈 (sys_openat → do_dentry_open)
#
# 整个路径分为两个阶段:
#   阶段1: 路径查找 (path_openat) - 从根目录逐级查找目标文件 dentry
#   阶段2: 文件打开 (vfs_open)    - 创建 file 结构, 调用具体文件系统

sys_openat
  │  # 系统调用: openat(AT_FDCWD, pathname, flags, mode)
  │  # 返回 fd (文件描述符)
  │
  └─ do_sys_openat2
      │  # 解析标志位, 分配 fd, 创建 struct file
      │
      └─ do_filp_open
          │  # 打开文件, 返回 struct file 指针
          │
          └─ path_openat
              │  # 核心路径打开函数
              │  # 处理打开标志 (O_CREAT, O_TRUNC 等)
              │  # 处理符号链接/挂载点穿越
              │
              ├─ get_nameidata
              │   # 获取/分配 nameidata 结构
              │   # 用于跟踪路径查找过程中的状态
              │
              ├─ open_last_lookups
              │   │  # 打开路径最后一个分量
              │   │  # 处理 O_CREAT|O_EXCL 等特殊标志
              │   │
              │   ├─ lookup_open
              │   │   │  # 查找最后一个路径分量 dentry
              │   │   │  # 如果文件不存在且指定 O_CREAT, 则创建
              │   │   │
              │   │   ├─ walk_component
              │   │   │   │  # 路径分量查找 (不含最后一个)
              │   │   │   │
              │   │   │   └─ lookup_slow
              │   │   │       │  # 慢路径查找: 调用具体文件系统的 lookup
              │   │   │       │
              │   │   │       └─ inode->i_op->lookup
              │   │   │           # 调用文件系统特定的 lookup 操作
              │   │   │           # ext4: ext4_lookup
              │   │   │           # 在目录中查找文件名, 返回 dentry
              │   │   │
              │   │   └─ __lookup_open
              │   │       # 打开 dentry (dentry_open)
              │   │       # 如果文件不存在, 创建 dentry 并设置 DCACHE_NEW
              │   │
              │   └─ do_last
              │       # 处理路径最后一个分量
              │       # 设置 file->f_path, 检查权限
              │
              └─ vfs_open
                  │  # 真正打开文件: 调用具体的 open 操作
                  │
                  └─ do_dentry_open
                      │  # 打开 dentry, 填充 file 结构
                      │  # 初始化 file->f_op, file->f_mode
                      │
                      ├─ get_file_rcu
                      │   # 通过 RCU 获取 file 引用
                      │
                      ├─ file->f_op->open
                      │   # 调用具体文件系统的 open 操作
                      │   # ext4: ext4_file_open
                      │   # 检查文件状态, 更新访问时间等
                      │
                      ├─ security_file_open
                      │   # LSM (Linux Security Module) 安全检查
                      #   # 如 SELinux, AppArmor 等
                      │
                      └─ fsnotify_file
                          # 文件系统通知: inotify/fanotify 事件
                          # 通知监听者文件被打开

# 关键数据结构变更:
#   struct file:
#     f_path  = 目标 dentry 的路径
#     f_op    = 文件系统操作表 (如 ext4_file_operations)
#     f_mode  = 根据 open flags 设置的读写模式
#     f_pos   = 初始化为 0
#     private_data = NULL (由具体文件系统设置)
#
#   struct dentry:
#     d_count += 1 (引用计数增加)
#     d_inode->i_count += 1 (inode 引用计数增加)
```

---

## 7. 路径查找过程

```text
# 路径查找过程 - 函数调用栈 (path_lookupat 内部)
#
# 路径查找是 VFS 中最核心的公共操作之一
# 几乎所有的文件系统操作 (open/stat/read/write) 都依赖它
# 查找过程需要处理以下关键场景:
#   - 符号链接解析 (symlink)
#   - 挂载点穿越 (cross-mount)
#   - 路径分量缓存 (dcache)
#   - 权限检查

path_lookupat
  │  # 路径查找入口
  │  # 参数: nd (nameidata), flags (查找标志)
  │
  ├─ filename_parentat
  │   │  # 查找父目录
  │   │  # 例如 "/a/b/c" 查找 "/a/b" 目录
  │   │
  │   └─ path_parentat
  │       │  # 查找路径的父目录部分
  │       │
  │       └─ link_path_walk
  │           │  # 核心: 逐分量遍历路径
  │           │  # 处理 "/a/b/c/d" 的中间分量
  │           │
  │           └─ walk_component
  │               │  # 处理单个路径分量
  │               │
  │               ├─ lookup_fast
  │               │   │  # 快速路径: 在 dcache 中查找
  │               │   │  # 利用 RCU (Read-Copy-Update) 无锁查找
  │               │   │  # 性能关键路径
  │               │   │
  │               │   └─ __d_lookup_rcu
  │               │       # RCU 模式下的 dentry 查找
  │               │       # 在 dentry 哈希表中查找匹配名称
  │               │
  │               └─ lookup_slow
  │                   │  # 慢速路径: dcache 未命中, 调用文件系统
  │                   │
  │                   ├─ inode->i_op->lookup
  │                   │   # 调用文件系统特定 lookup
  │                   │   # ext4: ext4_lookup → ext4_find_entry
  │                   │   # 从磁盘目录中查找文件名
  │                   │
  │                   └─ d_splice_alias
  │                       # 将新 dentry 与已存在的 inode 关联
  │                       # 处理硬链接场景
  │
  └─ complete_walk
      # 完成路径查找, 返回最终 dentry
      # 检查 seqlock (RCU 期间是否发生变化)
      # 处理最后一级的符号链接和挂载点

# 路径查找中 dentry 的 DCACHE 标志变化:
#   - 每次穿越挂载点时, 设置 dentry->flags |= DCACHE_MOUNTED
#   - 当 dentry 有 DCACHE_MOUNTED 标志时
#     VFS 跳转到子挂载的根目录 dentry
# 这就是 mount 穿越的实现机制
```

---

## 8. 文件读写路径

```text
# 文件读写路径 - 函数调用栈 (sys_read → submit_bio)
#
# 读操作流程: 用户态 → VFS → 页缓存 → 块设备
# 写入流程类似, 但多了回写 (writeback) 机制

sys_read
  │  # 系统调用: read(fd, buf, count)
  │
  ├─ ksys_read
  │   │  # 通过 fd 获取 struct file
  │   │  # 检查文件是否可读, 锁定位
  │   │
  │   └─ vfs_read
  │       │  # VFS 读入口
  │       │  # 检查文件访问权限
  │       │  # 调用具体文件系统的读操作
  │       │
  │       ├─ rw_verify_area
  │       │   # 检查读写区域是否合法
  │       │   # 检查文件是否有强制锁
  │       │   # 检查文件大小限制 (RLIMIT_FSIZE)
  │       │
  │       └─ new_sync_read
  │           │  # 同步读包装
  │           │  # 创建 struct iov_iter 描述用户缓冲区
  │           │
  │           └─ call_read_iter
  │               │  # 调用 file->f_op->read_iter
  │               │  # ext4: ext4_file_read_iter
  │               │
  │               └─ ext4_file_read_iter
  │                   │  # ext4 读入口
  │                   │  # 处理直接 IO (O_DIRECT) 和缓存 IO
  │                   │
  │                   └─ generic_file_read_iter
  │                       │  # 通用文件读取函数
  │                       │
  │                       ├─ filemap_read
  │                       │   │  # 从页缓存 (page cache) 读取数据
  │                       │   │  # 核心逻辑:
  │                       │   │  #   1. 计算需要读取的页范围
  │                       │   │  #   2. 在页缓存中查找/创建页
  │                       │   │  #   3. 如果页不在缓存中, 触发回 IO
  │                       │   │  #   4. 等待页 IO 完成
  │                       │   │  #   5. 将页数据拷贝到用户空间
  │                       │   │
  │                       │   └─ filemap_get_pages
  │                       │       │  # 获取页缓存页
  │                       │       │
  │                       │       ├─ filemap_create_folio
  │                       │       │   │  # 创建新的 folio (页缓存页)
  │                       │       │   │  # 触发同步回 IO
  │                       │       │   │
  │                       │       │   └─ filemap_read_folio
  │                       │       │       │  # 从磁盘读取一个 folio 的数据
  │                       │       │       │
  │                       │       │       └─ mpage_read_folio
  │                       │       │           │  # 多页读: 将 folio 的 IO 请求提交
  │                       │       │           │  # 创建 bio 并提交到块设备层
  │                       │       │           │
  │                       │       │           └─ submit_bio
  │                       │       │               # 提交 bio 到块设备层
  │                       │       │               # 经过 IO 调度器到驱动
  │                       │       │               # 实际读取磁盘数据
  │                       │       │
  │                       │       └─ filemap_update_page
  │                       │           # 更新已有的页缓存页
  │                       │           # 等待页的 IO 完成
  │                       │           # 检查页是否是最新的
  │                       │
  │                       └─ filemap_put_pages
  │                           # 释放页引用
  │                           # 更新文件的访问时间
  │
  └─ fput_light
      # 减少文件引用计数
      # 如果引用为 0, 释放 file 结构

# 页缓存 (Page Cache) 工作流程:
#
#   读未命中: 进程 → 缺页 → 分配页 → 提交 bio → 磁盘读 → 页就绪 → 拷贝
#              \___________________/         \_____/         \_________/
#              filemap_read_folio           submit_bio    copy_page_to_iter
#
#   读命中:   进程 → 在页缓存中找到 → 直接拷贝到用户空间
#              \___________________/  \_______________________/
#              filemap_get_page       copy_page_to_iter
#
#   写:       进程 → 写入页缓存 → 页标记为脏 → 周期性回写 → 提交 bio
#              \_____/  \__________/  \__________/  \_________/
#            copy_page   mark_page_accessed  writeback  submit_bio
```

---


## Part IV: 核心数据结构

## 9. VFS 核心数据结构详解

### 9.1 `super_block` — 超级块

```c
// 定义位置: include/linux/fs/super_types.h
// 每个已挂载的文件系统实例对应一个 super_block
// 是文件系统与 VFS 之间的桥梁

struct super_block {
    struct list_head            s_list;         // 全局 super_block 链表节点
    dev_t                       s_dev;          // 设备号 (搜索索引)
    unsigned char               s_blocksize_bits; // 块大小的 log2 值
    unsigned long               s_blocksize;    // 块大小 (字节)
    loff_t                      s_maxbytes;     // 最大文件大小
    struct file_system_type     *s_type;        // 所属文件系统类型
    const struct super_operations *s_op;        // super_block 操作函数表
    const struct dquot_operations *dq_op;       // 磁盘配额操作
    const struct quotactl_ops   *s_qcop;        // 配额控制操作
    const struct export_operations *s_export_op; // NFS 导出操作
    unsigned long               s_flags;        // 挂载标志 (SB_RDONLY, SB_SYNCHRONOUS 等)
    unsigned long               s_iflags;       // 内部标志 (SB_I_*)
    unsigned long               s_magic;        // 文件系统魔数 (EXT4_SUPER_MAGIC 等)
    struct dentry               *s_root;        // 文件系统根目录 dentry
    struct rw_semaphore         s_umount;       // 卸载同步信号量
    int                         s_count;        // 引用计数
    atomic_t                    s_active;       // 活跃计数
    struct hlist_bl_head        s_roots;        // 备用根 dentry (NFS 使用)
    struct mount                *s_mounts;      // 该 super_block 的挂载链表
    struct block_device         *s_bdev;        // 关联的块设备 (磁盘文件系统)
    struct file                 *s_bdev_file;   // 块设备文件
    struct backing_dev_info     *s_bdi;         // 后备设备信息 (回写能力)
    struct mtd_info             *s_mtd;         // MTD 设备 (闪存文件系统)
    struct hlist_node           s_instances;    // 在 fs_type->fs_supers 链表中的节点
    struct quota_info           s_dquot;        // 磁盘配额信息
    struct sb_writers           s_writers;      // 写入者计数 (冻结/解冻)
    void                        *s_fs_info;     // 文件系统私有数据指针
    u32                         s_time_gran;    // 时间戳粒度 (ns)
    time64_t                    s_time_min;     // 最小时间戳
    time64_t                    s_time_max;     // 最大时间戳
    char                        s_id[32];       // 标识名 (日志用)
    uuid_t                      s_uuid;         // UUID
    // ...
    struct list_lru             s_dentry_lru;   // dentry LRU 缓存
    struct list_lru             s_inode_lru;    // inode LRU 缓存
    struct list_head            s_inodes;       // 该文件系统所有 inode 链表
    struct list_head            s_inodes_wb;    // 待回写 inode 链表
    // ...
};
```

**关键流程关联**: super_block 在挂载过程中创建 (Section 7)，在读写过程中通过 `dentry->d_sb` 访问，在文件系统中通过 `s_fs_info` 获取私有数据。

### 9.2 `inode` — 索引节点

```c
// 定义位置: include/linux/fs.h
// 每个文件/目录在内存中对应一个 inode, 存储元数据
// 磁盘文件系统从磁盘读取 inode 缓存在内存中

struct inode {
    umode_t                     i_mode;         // 文件类型 + 权限位 (S_IFREG, S_IFDIR 等)
    unsigned short              i_opflags;      // 操作标志
    unsigned int                i_flags;        // 文件标志 (S_SYNC, S_NOATIME 等)
    kuid_t                      i_uid;          // 属主 UID
    kgid_t                      i_gid;          // 属组 GID
    const struct inode_operations  *i_op;        // inode 操作函数表
    struct super_block          *i_sb;          // 所属 super_block
    struct address_space        *i_mapping;     // 指向 address_space (页缓存)
    unsigned long               i_ino;          // inode 编号
    unsigned int                i_nlink;        // 硬链接计数
    dev_t                       i_rdev;         // 设备号 (设备文件专用)
    loff_t                      i_size;         // 文件大小 (字节)
    time64_t                    i_atime_sec;    // 最后访问时间 (秒)
    time64_t                    i_mtime_sec;    // 最后修改时间 (秒)
    time64_t                    i_ctime_sec;    // 状态变更时间 (秒)
    spinlock_t                  i_lock;         // 保护 i_blocks, i_bytes 等
    blkcnt_t                    i_blocks;       // 文件占用的块数 (512 字节为单位)
    struct rw_semaphore         i_rwsem;        // 保护 inode 序列化
    struct hlist_node           i_hash;         // inode 哈希表节点
    struct list_head            i_lru;          // LRU 链表节点 (缓存回收)
    struct list_head            i_sb_list;      // super_block 的 inode 链表节点
    atomic64_t                  i_version;      // 版本号 (NFS 使用)
    atomic_t                    i_count;        // 引用计数
    atomic_t                    i_dio_count;    // 直接 IO 计数
    atomic_t                    i_writecount;   // 写入者计数
    const struct file_operations *i_fop;        // 默认文件操作表
    struct address_space        i_data;         // 内嵌 address_space 实例
    union {
        struct pipe_inode_info  *i_pipe;        // 管道文件
        struct cdev             *i_cdev;        // 字符设备
        char                    *i_link;        // 符号链接目标
    };
    void                        *i_private;     // 文件系统私有数据
};
```

**inode 状态机**:

```text
# inode 的 i_state 状态转换:

  NEW (新分配, 尚未初始化)
    │
    ▼
  INODE (正在被读取/磁盘同步中)
    │
    ├── DIRTY_SYNC → 需要同步元数据 (时间戳等)
    ├── DIRTY_DATA → 数据页脏 (用户数据未写回)
    ├── DIRTY_PAGES → 页缓存脏 (与 DIRTY_DATA 类似)
    ├── NEW → 新创建, 尚未写入磁盘
    └── FREEING → 正在被销毁

  I_FREEING (引用为 0, 等待回收)
    │
    ▼
  I_CLEAR (inode 已清除, 等待 slab 回收)
```

### 9.3 `dentry` — 目录项

```c
// 定义位置: include/linux/dcache.h
// dentry 是路径名和 inode 之间的桥梁, 缓存目录层次结构
// 不存储在磁盘上, 仅在内存中存在

struct dentry {
    unsigned int                d_flags;        // 标志位: DCACHE_MOUNTED, DCACHE_DISCONNECTED 等
    seqcount_spinlock_t         d_seq;          // 每 dentry seqlock (RCU 路径查找用)
    struct hlist_bl_node        d_hash;         // dcache 哈希表节点 (快速查找)
    struct dentry               *d_parent;      // 父目录 dentry
    const struct qstr           d_name;         // 文件名 (hash + len + name)
    struct inode                *d_inode;       // 指向对应 inode (NULL = 负 dentry)
    const struct dentry_operations *d_op;        // dentry 操作函数表
    struct super_block          *d_sb;          // 所属 super_block
    unsigned long               d_time;         // 由 d_revalidate 使用
    void                        *d_fsdata;      // 文件系统私有数据
    struct lockref              d_lockref;      // 锁 + 引用计数
    union {
        struct list_head        d_lru;          // LRU 链表 (未使用 dentry)
        wait_queue_head_t       *d_wait;        // 等待队列 (in-lookup 状态)
    };
    struct hlist_node           d_sib;          // 兄弟链表 (同父目录的子 dentry)
    struct list_head            d_subdirs;      // 子目录链表
    struct hlist_node           d_alias;        // 指向同一 inode 的别名链表
};
```

**dentry 状态机**:

```text
# dentry 的 4 种状态:

  空闲 (slab 缓存)
    │
    ▼
  DCACHE_UNUSED (未使用, 在 LRU 上)
    │
    ├── [访问命中] → USED (d_lockref.count > 0, 脱离 LRU)
    ├── [内存回收] → 回到空闲
    │
    ▼
  USED (正在使用, d_lockref.count > 0)
    │
    ├── [dput() 后 count=0] → DCACHE_UNUSED (加入 LRU 尾部)
    └── [路径查找] → 引用计数增加, 保持 USED

  NEGATIVE (负 dentry, d_inode = NULL)
    │ 表示该文件名不存在, 用于缓存"不存在"的结果
    ├── [文件创建] → 设置 d_inode → USED
    └── [dput()] → DCACHE_UNUSED

  DCACHE_DISCONNECTED (断开连接)
    │ 父目录未知 (如 NFS 路径遍历)
    └── [d_instantiate/d_splice_alias] → USED
```

### 9.4 `file` — 文件对象

```c
// 定义位置: include/linux/fs.h
// 每个进程打开的文件对应一个 file 结构
// 存储在进程的 fdtable (文件描述符表) 中

struct file {
    spinlock_t                  f_lock;         // 保护 f_ep, f_flags
    fmode_t                     f_mode;         // 文件模式 (FMODE_READ, FMODE_WRITE 等)
    const struct file_operations *f_op;         // 文件操作函数表
    struct address_space        *f_mapping;     // 页缓存地址空间
    void                        *private_data;  // 文件系统私有数据
    struct inode                *f_inode;       // 缓存的 inode
    unsigned int                f_flags;        // 打开标志 (O_RDONLY, O_SYNC 等)
    unsigned int                f_iocb_flags;   // IOCB 标志
    const struct cred           *f_cred;        // 打开者的凭据
    struct fown_struct          *f_owner;       // 文件所有者 (SIGIO/SIGURG)
    const struct path           f_path;         // 文件路径 (dentry + vfsmount)
    struct mutex                f_pos_lock;     // 位置锁 (FMODE_ATOMIC_POS)
    loff_t                      f_pos;          // 当前读写位置
    errseq_t                    f_wb_err;       // 写回错误
    struct file_ra_state        f_ra;           // 预读状态
    file_ref_t                  f_ref;          // 引用计数
};
```

**file 与 inode 的关系**:

```text
# 进程打开文件时, file 和 inode 的关系:

  进程 A                进程 B                进程 C
    │                     │                     │
    │ fd=3                │ fd=5                │ fd=7
    ▼                     ▼                     ▼
  file A                file B                file C
  f_pos=0               f_pos=100             f_pos=50
  f_flags=O_RDONLY      f_flags=O_WRONLY      f_flags=O_RDWR
    │                     │                     │
    └────────────────────┬┴─────────────────────┘
                         │ f_inode
                         ▼
                       inode (同一文件)
                       i_size=4096
                       i_count=3
                       i_writecount=2
                         │
                         ▼
                       address_space (共享页缓存)
                       i_pages: XArray
                       nrpages: 16
```

### 9.5 `vfsmount` / `mount` — 挂载实例

```c
// 定义位置: include/linux/mount.h (vfsmount)
//          fs/mount.h (mount)

// 公共挂载结构 (通过 VFS 接口暴露)
struct vfsmount {
    struct dentry       *mnt_root;      // 该挂载的根 dentry
    struct super_block  *mnt_sb;        // 该挂载的 super_block
    int                 mnt_flags;      // 挂载标志 (MNT_READONLY, MNT_NOSUID 等)
    struct mnt_idmap    *mnt_idmap;     // 挂载 ID 映射
};

// 内部挂载结构 (核心 VFS 使用)
struct mount {
    struct hlist_node   mnt_hash;               // 挂载哈希表节点
    struct mount        *mnt_parent;             // 父挂载
    struct dentry       *mnt_mountpoint;         // 挂载点 dentry (在父 FS 中)
    struct vfsmount     mnt;                     // 公共 vfsmount 部分
    struct rb_node      mnt_node;                // 命名空间红黑树节点
    struct list_head    mnt_mounts;              // 子挂载链表
    struct list_head    mnt_child;               // 在父挂载的子链表中
    struct mount        *mnt_next_for_sb;        // 同一 super_block 的挂载链表
    const char          *mnt_devname;            // 设备名 (如 /dev/sda1)
    struct list_head    mnt_list;                // 命名空间挂载链表
    struct list_head    mnt_share;               // 共享挂载环链表
    struct hlist_head   mnt_slave_list;          // 从挂载链表
    struct hlist_node   mnt_slave;               // 从挂载节点
    struct mount        *mnt_master;             // 主挂载
    struct mnt_namespace *mnt_ns;                // 所属命名空间
    struct mountpoint   *mnt_mp;                 // 挂载点对象
};
```

**挂载传播关系**:

```text
# mount 传播类型 (mnt_flags 中的 MNT_PROPAGATION 掩码):

  共享挂载 (MS_SHARED):
    ┌──────┐    ┌──────┐    ┌──────┐
    │mount A│────│mount B│────│mount C│   ← 环状链表 (mnt_share)
    └──────┘    └──────┘    └──────┘
    在一个挂载中的操作会传播到所有共享挂载

  从挂载 (MS_SLAVE):
    ┌──────────┐
    │mount M   │ (主挂载)
    └────┬─────┘
         │ mnt_slave_list
    ┌────┴─────┐  ┌──────────┐
    │mount S1  │  │mount S2  │  ← 从挂载链表
    └──────────┘  └──────────┘
    主挂载的操作传播到从挂载, 反之不传播

  私有挂载 (MS_PRIVATE):
    ┌──────┐
    │mount X│  ← 完全独立, 不参与任何传播
    └──────┘

  不可绑定挂载 (MS_UNBINDABLE):
    ┌──────┐
    │mount Y│  ← 不能作为 bind mount 的源
    └──────┘
```

### 9.6 `file_system_type` — 文件系统类型

```c
// 定义位置: include/linux/fs.h
// 描述一种文件系统类型, 所有已注册的文件系统形成全局链表

struct file_system_type {
    const char *name;               // 文件系统名称 (如 "ext4", "proc")
    int fs_flags;                   // 标志位:
                                    //   FS_REQUIRES_DEV       - 需要块设备
                                    //   FS_USERNS_MOUNT       - 允许用户命名空间挂载
                                    //   FS_ALLOW_IDMAP       - 支持 ID 映射
                                    //   FS_MGTIME            - 支持多粒度时间戳
                                    //   FS_BINARY_MOUNTDATA  - 二进制挂载数据
                                    //   FS_LBS               - 支持大块大小
                                    //   FS_POWER_FREEZE      - 挂起时冻结
    int (*init_fs_context)(struct fs_context *);  // 创建文件系统上下文
    const struct fs_parameter_spec *parameters;   // 挂载参数规格
    void (*kill_sb)(struct super_block *);        // 销毁 super_block
    struct module *owner;               // 所属模块 (动态加载时)
    struct file_system_type *next;       // 链表下一个
    struct hlist_head fs_supers;         // 该类型的所有 super_block 链表
    // ... lockdep 键 ...
};
```

**文件系统类型注册流程**:

```text
register_filesystem(fs_type)
  │
  ├─ 检查 fs_type->name 是否已注册 (遍历 file_systems 链表)
  │
  ├─ 将 fs_type 插入 file_systems 链表头部
  │
  └─ 返回 0 (成功)

# 挂载时, VFS 根据名称查找:
#   get_fs_type(name) → 遍历 file_systems 链表 → 匹配 name → 返回 fs_type
```

### 9.7 `address_space` — 地址空间 (页缓存核心)

```c
// 定义位置: include/linux/fs.h
// 每个可缓存、可映射的对象 (文件或块设备) 对应一个 address_space
// 管理该文件在内存中的所有缓存页

struct address_space {
    struct inode                        *host;          // 所属 inode
    struct xarray                       i_pages;        // 页缓存 XArray (存储所有缓存页)
    struct rw_semaphore                 invalidate_lock; // 缓存失效锁
    gfp_t                               gfp_mask;       // 内存分配掩码
    atomic_t                            i_mmap_writable; // 共享可写映射计数
    struct rb_root_cached               i_mmap;         // 内存映射红黑树
    unsigned long                       nrpages;        // 缓存页数量
    pgoff_t                             writeback_index; // 回写起始位置
    const struct address_space_operations *a_ops;        // 地址空间操作函数表
    unsigned long                       flags;          // 标志 (AS_*)
    errseq_t                            wb_err;         // 写回错误
    spinlock_t                          i_private_lock; // 保护私有链表
    struct list_head                    i_private_list; // 私有链表
    struct rw_semaphore                 i_mmap_rwsem;   // 保护 i_mmap
    void                                *i_private_data; // 私有数据
};
```

**address_space_operations 回调函数表**:

```c
struct address_space_operations {
    // 从磁盘读取一个 folio 到缓存
    int (*read_folio)(struct file *, struct folio *);

    // 回写脏页到磁盘
    int (*writepages)(struct address_space *, struct writeback_control *);

    // 标记 folio 为脏
    bool (*dirty_folio)(struct address_space *, struct folio *);

    // 预读 (批量读取相邻页)
    void (*readahead)(struct readahead_control *);

    // 写入开始 (write 系统调用触发的缺页)
    int (*write_begin)(const struct kiocb *, struct address_space *mapping,
                       loff_t pos, unsigned len, struct folio **foliop,
                       void **fsdata);
    // 写入结束
    int (*write_end)(const struct kiocb *, struct address_space *mapping,
                     loff_t pos, unsigned len, unsigned copied,
                     struct folio *folio, void *fsdata);

    // 使缓存页失效 (truncate 时调用)
    void (*invalidate_folio)(struct folio *, size_t offset, size_t len);

    // 释放缓存页
    bool (*release_folio)(struct folio *, gfp_t);

    // 直接 IO (绕过页缓存)
    ssize_t (*direct_IO)(struct kiocb *, struct iov_iter *iter);
};
```

**页缓存生命周期**:

```text
# 页缓存 (Page Cache) 的生命周期:

  读操作:
  filemap_read() → filemap_get_pages()
    ├── i_pages XArray 中查找 → 命中? → 直接返回缓存页
    │                              → 未命中? → 分配新页 → read_folio() → 提交 bio
    └── 数据从缓存页拷贝到用户缓冲区

  写操作:
  generic_perform_write() → a_ops->write_begin()
    ├── i_pages XArray 中查找 → 未命中? → 分配新页
    ├── 数据从用户缓冲区拷贝到缓存页
    └── a_ops->write_end()  → 标记页为脏 (dirty_folio)

  回写 (Writeback):
  writeback_single_inode() → a_ops->writepages()
    ├── 遍历标记为脏的页
    ├── 提交 bio 写入磁盘
    └── 清除脏标志, 释放回写锁

  回收 (Reclaim):
  shrink_folio_list() → a_ops->release_folio()
    ├── 如果页是脏的 → 先回写
    ├── 从 XArray 中移除
    └── 释放页到 buddy 系统
```

### 9.8 数据结构关系汇总

```text
# VFS 六大核心数据结构关系一览

                                       超级块
                                    ┌──────────────┐
                    ┌───────────────│  super_block │───────────────┐
                    │ s_type        │  s_root      │ s_fs_info     │
                    ▼               └──────┬───────┘               ▼
              ┌───────────┐               │               ┌──────────────┐
              │file_system│               │ s_root        │ 文件系统私有  │
              │_type      │               ▼               │ (ext4_sb_info)│
              │ name=ext4 │        ┌──────────────┐       └──────────────┘
              │ fs_supers │        │  dentry "/"  │
              └───────────┘        │ d_inode=root │
                                  │ d_sb=sb      │
                                   └──────┬───────┘
                ┌─────────────────────────┼─────────────────────────┐
                │ d_parent                │ d_subdirs               │
                ▼                         ▼                         ▼
        ┌──────────────┐         ┌──────────────┐         ┌──────────────┐
        │ dentry "usr" │         │ dentry "bin" │         │ dentry "etc" │
        │ d_inode=ino1 │         │ d_inode=ino2 │         │ d_inode=ino3 │
        └──────┬───────┘         └──────┬───────┘         └──────┬───────┘
               │ d_inode                 │ d_inode                 │ d_inode
               ▼                         ▼                         ▼
        ┌──────────────┐         ┌──────────────┐         ┌──────────────┐
        │  inode       │         │  inode       │         │  inode       │
        │ i_size=4096  │         │ i_size=1048576│        │ i_size=2048  │
        │ i_mapping ───┼──┐      │ i_mapping ───┼──┐      │ i_mapping ───┼──┐
        └──────────────┘  │      └──────────────┘  │      └──────────────┘  │
                          ▼                        ▼                        ▼
                   ┌──────────────┐         ┌──────────────┐         ┌──────────────┐
                   │address_space │         │address_space │         │address_space │
                   │ i_pages=XArr │         │ i_pages=XArr │         │ i_pages=XArr │
                   │ nrpages=4    │         │ nrpages=256  │         │ nrpages=1    │
                   └──────────────┘         └──────────────┘         └──────────────┘

  进程打开文件:
        ┌──────────────┐
        │  task_struct │
        │ files_struct │
        │ fdtable[]    │
        └──────┬───────┘
               │ fd=3
               ▼
        ┌──────────────┐        ┌──────────────┐
        │    file      │───────▶│  dentry "usr" │
        │ f_pos=0      │        │ /usr/bin/foo  │
        │ f_op=ext4_ops│        └──────┬───────┘
        │ f_inode ─────┼──────────────▶│ inode
        │ f_mapping ───┼──────────────▶│ address_space
        └──────────────┘               └──────────────┘
```

---

## Part V: 特殊文件系统

## 10. 特殊文件系统概述

### 10.1 procfs — 进程文件系统

```text
# 挂载点: /proc
# 源文件: fs/proc/
# 类型: 伪文件系统 (pseudo filesystem), 基于 kernfs

procfs 是一个虚拟文件系统, 以文件形式暴露内核和进程信息。
不占用磁盘空间, 内容在读取时动态生成。

# 核心结构:
#   每个进程对应 /proc/[pid]/ 目录
#   内核参数通过 /proc/sys/ 暴露 (可通过 sysctl 修改)
#   硬件信息通过 /proc/cpuinfo, /proc/meminfo 等暴露

# 关键实现:
#   proc_fs_type → proc_init_fs_context → proc_fill_super
#     创建 root proc 目录
#     注册 proc 文件系统根 inode

# 目录结构:
/proc/
  ├── 1/            # init 进程信息
  ├── 1234/         # PID 1234 的进程信息
  │   ├── cmdline   # 命令行参数
  │   ├── cwd →     # 当前工作目录 (符号链接)
  │   ├── exe →     # 可执行文件 (符号链接)
  │   ├── fd/       # 文件描述符
  │   ├── maps      # 内存映射
  │   ├── stat      # 进程状态
  │   ├── status    # 进程状态 (可读格式)
  │   └── ...       # 其他进程信息
  ├── cpuinfo       # CPU 信息
  ├── meminfo       # 内存信息
  ├── mounts        # 挂载信息
  ├── diskstats     # 磁盘 I/O 统计
  ├── net/          # 网络信息
  ├── sys/          # 内核参数 (sysctl)
  ├── uptime        # 系统运行时间
  └── version       # 内核版本

# 读取流程:
#   read /proc/meminfo
#     → proc_reg_read()       (proc 通用读取)
#     → single_open()         (一次生成所有内容)
#     → proc_meminfo_show()   (生成 meminfo 内容)
#     → 调用 si_meminfo(), si_swapinfo() 等获取内存信息
#     → seq_printf() 格式化输出
```

### 10.2 sysfs — 内核对象文件系统

```text
# 挂载点: /sys
# 源文件: fs/sysfs/
# 类型: 伪文件系统, 基于 kernfs

sysfs 将内核对象模型 (kobject) 层次结构以文件系统形式暴露。
每个 kobject 对应 /sys/ 下的一个目录, 每个属性 (attribute) 对应一个文件。

# 核心概念:
#   kobject: 内核对象 (设备、驱动、总线等)
#   kset:   kobject 的集合
#   attribute: kobject 的可读/写属性文件

# 目录结构:
/sys/
  ├── block/        # 块设备
  ├── bus/          # 总线 (pci, usb, spi 等)
  │   ├── pci/
  │   │   ├── devices/   # PCI 设备
  │   │   └── drivers/   # PCI 驱动
  │   └── usb/
  ├── class/        # 设备类 (net, input, tty 等)
  ├── dev/          # 设备号
  ├── devices/      # 设备树 (ACPI/DT 设备层次)
  ├── firmware/     # 固件信息
  ├── fs/           # 文件系统信息
  ├── hypervisor/   # 虚拟机监控器
  ├── kernel/       # 内核信息
  ├── module/       # 内核模块
  └── power/        # 电源管理

# 关键实现:
#   sysfs_fs_type → sysfs_init_fs_context → sysfs_fill_super
#     创建 sysfs 根目录
#     kobject 创建时自动在 sysfs 中创建目录
#     属性文件通过 sysfs_create_file() 创建

# 示例: 读取 NVMe 设备信息
#   /sys/block/nvme0n1/queue/    →  IO 队列参数
#   /sys/block/nvme0n1/size      →  设备大小
#   /sys/devices/pci0000:00/.../ →  PCI 设备树
```

### 10.3 debugfs — 调试文件系统

```text
# 挂载点: /sys/kernel/debug/ (通常)
# 源文件: fs/debugfs/
# 类型: 伪文件系统, 用于内核调试接口

debugfs 是一个简单的内存文件系统, 为内核开发者提供快速创建调试接口的途径。
API 简单, 适合临时/调试用途, 不适合稳定的用户空间 ABI。

# 核心 API:
#   debugfs_create_dir(name, parent)     → 创建目录
#   debugfs_create_file(name, mode, ...) → 创建文件
#   debugfs_create_u32/u64/...           → 创建单值文件
#   debugfs_create_bool()                → 创建布尔文件
#   debugfs_create_x32()                 → 创建十六进制文件
#   debugfs_create_blob()                → 创建二进制文件

# 使用示例:
#   struct dentry *dir = debugfs_create_dir("my_driver", NULL);
#   debugfs_create_u32("counter", 0644, dir, &my_counter);
#   debugfs_create_bool("enable", 0644, dir, &my_enable);

# 常见使用场景:
#   - 驱动调试寄存器/状态显示
#   - 性能计数器
#   - 运行时参数调节
#   - 内核内部状态检查
#   - 某时刻的跟踪/日志输出

# 注意: debugfs 在内核配置 CONFIG_DEBUG_FS 启用时可用
#       生产环境可能关闭此选项
```

### 10.4 ramfs / tmpfs — 内存文件系统

```text
# ramfs: 最早的内存文件系统, 简单直接
# tmpfs: ramfs 的增强版, 支持大小限制和 swap 后备
# 源文件: fs/ramfs/, mm/shmem.c

ramfs 和 tmpfs 将数据完全存储在内存中, 不关联任何块设备。
写入的数据会占用物理内存, 系统重启后数据丢失。

# ramfs 特点:
#   - 极简实现, 所有数据在页缓存中
#   - 无大小限制 (可以写满所有内存)
#   - 不可 swap (数据始终在物理内存中)
#   - 用于 rootfs (初始根文件系统)

# tmpfs 特点:
#   - 支持大小限制 (通过 mount -o size=N)
#   - 支持 swap 后备 (不活跃页面可换出)
#   - 支持文件链接和权限
#   - 用于 /tmp, /dev/shm 等

# ramfs 关键实现:
#   ramfs 使用 simple 文件系统框架:
#   - ramfs_mkdir → simple_mkdir
#   - ramfs_create → simple_create
#   - ramfs_get_inode → 分配 inode, 设置 i_op/i_fop
#   - 所有数据通过页缓存管理

# tmpfs 关键实现:
#   tmpfs 基于 shmem (共享内存) 子系统:
#   - shmem_get_folio() → 分配 folio 到页缓存
#   - 当 memory pressure 时, 不活跃页可 swap 出去
#   - 支持 mmap 共享内存 (POSIX SHM)

# 使用场景对比:
# ┌──────────┬──────────────┬──────────────┐
# │          │    ramfs     │    tmpfs     │
# ├──────────┼──────────────┼──────────────┤
# │ 大小限制 │    无        │  mount -o    │
# │          │              │  size=1G     │
# ├──────────┼──────────────┼──────────────┤
# │ Swap备份 │    无        │    有        │
# ├──────────┼──────────────┼──────────────┤
# │ 典型用途 │ rootfs/boot │  /tmp,/dev/shm│
# └──────────┴──────────────┴──────────────┘
```

### 10.5 bdevfs — 块设备文件系统

```text
# 挂载点: 不直接挂载, 通过 bd_acquire() 使用
# 源文件: fs/block_dev.c
# 类型: 伪文件系统, 管理块设备 inode

bdevfs (bdev) 是一个特殊的伪文件系统, 用于管理块设备的 inode 和页缓存。
不直接挂载到文件系统树中, 而是通过内核内部函数访问。

# 核心作用:
#   1. 为每个块设备 (如 /dev/sda1) 分配 inode 和 address_space
#   2. 管理块设备的页缓存 (块设备本身也有页缓存!)
#   3. 提供块设备打开/关闭操作

# 关键数据结构:
#   每个块设备对应一个 struct block_device:
#     bd_inode → 块设备的 inode (通过 i_mapping 访问页缓存)
#     bd_disk  → 通用磁盘结构
#     bd_part  → 分区信息

# 块设备页缓存的作用:
#   - 缓存文件系统元数据 (super_block, 块组描述符, inode 表等)
#   - 通过 buffer_head 访问块设备页缓存
#   - 文件系统通过 sb_bread() 读取元数据:
#     sb_bread(sb, block) → __getblk() → 在 bdev 页缓存中查找/创建 buffer_head

# 读取流程:
#   ext4_read_inode(sb, ino, raw_inode)
#     → sb_bread(sb, block)                    # 读取包含 inode 的块
#     → __getblk(sb->s_bdev, block, size)      # 在 bdev 页缓存中查找
#     → 如果未命中: __bread() → submit_bh()    # 发起块设备 IO
#     → 从 buffer_head 中解析 inode 数据
```

### 10.6 sockfs — 套接字文件系统

```text
# 挂载点: 不直接挂载, 内核内部使用
# 源文件: net/socket.c
# 类型: 伪文件系统, 为 socket 提供 VFS 接口

sockfs 是一个伪文件系统, 使 socket 能够通过 VFS 文件接口访问。
这就是为什么 socket() 返回一个文件描述符的原因。

# 核心原理:
#   socket() 系统调用:
#   → sock_create() 创建 socket
#   → sock_alloc_file() 在 sockfs 中创建 file/inode
#   → 返回 fd (文件描述符)

# 关键实现:
#   sockfs 的 file_operations = socket_file_ops:
#   - .read_iter  = sock_read_iter
#   - .write_iter = sock_write_iter
#   - .poll       = sock_poll
#   - .unlocked_ioctl = sock_ioctl
#   - .mmap       = sock_mmap
#   - .release    = sock_close

# socket 文件对象的特殊之处:
#   file->private_data → 指向 struct socket
#   socket->sk → 指向 struct sock (协议栈核心)
#   file->f_op → 始终为 socket_file_ops
#   通过 VFS 的 read/write/poll/ioctl 操作, 最终调用协议栈函数

# 调用路径:
#   read(fd, buf, len) → vfs_read() → file->f_op->read_iter()
#     → sock_read_iter() → sock_recvmsg() → tcp_recvmsg()/udp_recvmsg()
```

### 10.7 ext4 — 第四代扩展文件系统

```text
# 挂载点: 通常是 /mnt 或根文件系统
# 源文件: fs/ext4/
# 类型: 磁盘文件系统, 日志型

ext4 是 Linux 最广泛使用的磁盘文件系统, 是 ext3 的演进版本。
支持最大 1EB 文件系统和 16TB 单个文件。

# 磁盘布局:
#
# ┌──────────┬──────────┬──────────┬──────────┬──────────┐
# │ Super    │ Group    │ Data     │ Inode    │ 数据块   │
# │ Block    │ Descript │ Block    │ Table    │ (文件    │
# │ 0        │ ors      │ Bitmap   │ (inode)  │ 内容)    │
# ├──────────┼──────────┼──────────┼──────────┼──────────┤
# │ 块组 0   │ 块组 0   │ 块组 0   │ 块组 0   │ 块组 0   │
# └──────────┴──────────┴──────────┴──────────┴──────────┘
# ┌──────────┬──────────┬──────────┬──────────┬──────────┐
# │ 块组 1   │ 块组 1   │ 块组 1   │ 块组 1   │ 块组 1   │
# ├──────────┼──────────┼──────────┼──────────┼──────────┤
# │ ...      │ ...      │ ...      │ ...      │ ...      │
# └──────────┴──────────┴──────────┴──────────┴──────────┘
#
# 每个块组 (block group) 包含:
#   - super_block: 文件系统全局信息 (通常每个块组有备份)
#   - group descriptors: 块组描述符表
#   - data block bitmap: 数据块使用位图 (1 bit/块)
#   - inode bitmap: inode 使用位图
#   - inode table: inode 数组 (每个 inode 256 字节)
#   - data blocks: 实际文件数据

# ext4 关键特性:
#   - 日志 (Journal): 元数据事务日志, 保证崩溃一致性
#   - 延迟分配 (Delayed Allocation): 写回时批量分配磁盘块
#   - 多块分配 (Multiblock Allocator): 一次分配多个连续块
#   - extents: 用 extent 树替代传统 block map 管理大文件
#   - Htree 目录索引: 通过 B-Tree 变体加速大目录查找
#   - 在线调整大小: 文件系统挂载时扩展
#   - 纳秒时间戳: 支持纳秒级时间精度
#   - 预分配: fallocate() 系统调用支持

# ext4 写操作流程:
#   write() → ext4_file_write_iter()
#     → ext4_buffered_write_iter() 或 ext4_dio_write_iter()
#     → ext4_write_begin()
#       → ext4_da_write_begin() (延迟分配模式)
#         → ext4_da_reserve_space()  # 预留空间
#         → block_write_begin()       # 准备页缓存
#     → iov_iter_copy_from_user_atomic()  # 拷贝数据
#     → ext4_write_end()
#       → ext4_da_write_end()        # 标记脏页
#     → 回写时: ext4_writepages()
#       → ext4_map_blocks()          # 分配物理块
#       → mpage_map_and_submit_extent()  # 提交 IO

# ext4 核心数据结构:
#   ext4_sb_info:   super_block 的私有数据 (块组、挂载选项等)
#   ext4_inode_info: inode 的私有数据 (extent 树、flags 等)
#   ext4_extent:     extent 结构 (逻辑块→物理块映射)
#   ext4_group_desc: 块组描述符
#   ext4_dir_entry:  目录项结构

# ext4 文件系统操作:
#   ext4_fill_super(): 挂载时初始化 (读 super_block, 检查特征, 初始化结构)
#   ext4_lookup():     目录项查找 (Htree 或线性搜索)
#   ext4_file_open():  打开文件 (检查 ACL, 更新状态)
#   ext4_read_inode(): 从磁盘读取 inode 到内存
#   ext4_write_inode(): 将 inode 写回磁盘
#   ext4_evict_inode(): 驱逐 inode (释放空间, 删除文件)
```
```

### 10.8 cgroup / cgroup2 — 控制组文件系统

```text
# 挂载点: /sys/fs/cgroup/ (cgroup v1: 每个控制器一个子挂载; cgroup v2: 统一挂载)
# 源文件: kernel/cgroup/
# 类型: 伪文件系统, 基于 kernfs

cgroup 文件系统将进程分组层次结构暴露给用户空间, 用于资源限制和监控。
通过写入 cgroup 伪文件来配置 CPU/内存/IO/PID 等资源限制。

# cgroup v1 特点:
#   - 每个控制器独立挂载: /sys/fs/cgroup/cpu/, /sys/fs/cgroup/memory/ 等
#   - 不同控制器可挂载在不同目录, 形成各自独立的层次树
#   - 进程可同时加入多个不同控制器的 cgroup
#   - 控制器间存在资源统计冲突问题

# cgroup v2 特点:
#   - 统一挂载, 所有控制器在同一层次结构中
#   - 线程模式 (threaded): 支持进程内不同线程分属不同 cgroup
#   - 无内部进程 (no internal processes): 非根 cgroup 不能有进程
#   - 子节点继承父节点的资源限制

# 目录结构 (cgroup v2):
/sys/fs/cgroup/
  ├── cgroup.controllers      # 可用控制器列表
  ├── cgroup.subtree_control  # 子 cgroup 的控制器
  ├── cpu.max                 # CPU 配额 (max usages period)
  ├── memory.max              # 内存上限
  ├── memory.current          # 当前内存使用
  ├── io.max                  # IO 带宽限制
  ├── pids.max                # PID 数量限制
  ├── system.slice/           # 系统服务 cgroup
  │   ├── sshd.service/
  │   ├── systemd-journald.service/
  │   └── ...
  └── user.slice/             # 用户会话 cgroup

# 使用示例:
#   # 创建控制组并限制 CPU 使用
#   mkdir /sys/fs/cgroup/mygroup
#   echo 50000 100000 > /sys/fs/cgroup/mygroup/cpu.max   # 限制 50% CPU
#   echo 1234 > /sys/fs/cgroup/mygroup/cgroup.procs      # 将 PID 1234 加入

# 关键实现:
#   cgroup_init_early(): 初始化 cgroup 子系统
#   cgroup_init():      注册 cgroup 文件系统, 创建根 cgroup
#   cgroup_mkdir():     用户空间 mkdir 创建新 cgroup
#   cgroup_attach_task(): 将进程移入 cgroup
```

### 10.9 devtmpfs — 设备节点文件系统

```text
# 挂载点: /dev
# 源文件: drivers/base/devtmpfs.c
# 类型: 伪文件系统, 基于 ramfs

devtmpfs 是内核自动管理 /dev 目录下设备节点文件系统的机制。
当内核检测到新设备时, 自动在 devtmpfs 中创建设备文件,
无需等待用户空间的 udev/mdev 响应。

# 核心工作流程:
#   设备注册 → 内核创建 devtmpfs 设备节点 → udev 收到通知
#   → udev 设置权限/创建符号链接 → 用户空间可访问

# 设备节点创建流程:
#   device_add()
#     → devtmpfs_create_node()
#       → devtmpfs_work() (工作队列)
#         → devtmpfsd() (内核线程)
#           → device_create_file()
#             → kernfs_create_file() 创建 /dev/xxx 文件

# 关键实现:
#   devtmpfs 在系统启动早期由内核挂载:
#   init_devtmpfs():
#     → mount(MNT_DEVFS) → 挂载 devtmpfs 到 /dev
#     → 填充初始设备节点 (console, null, zero 等)
#     → 启动 devtmpfsd 内核线程监听设备事件

# 与 udev 的关系:
#   devtmpfs 负责创建设备文件 (最小权限, root:root)
#   udev 负责设备命名规则、权限管理、符号链接
#   udev 收到内核 uevent 后, 修改 devtmpfs 中已存在文件的属性
```

### 10.10 configfs — 配置对象文件系统

```text
# 挂载点: /sys/kernel/config/ (通常)
# 源文件: fs/configfs/
# 类型: 伪文件系统, 基于 RAM

configfs 是一个基于对象的配置文件系统, 用户空间通过
mkdir/rmdir 创建/删除内核对象, 通过写入属性文件来配置对象。
与 sysfs 不同, sysfs 暴露已有对象, configfs 创建新对象。

# 核心概念:
#   用户空间 mkdir → 内核创建配置对象 (如 iSCSI target)
#   用户空间写入属性文件 → 修改对象配置
#   用户空间 rmdir → 内核销毁对象

# 目录结构示例 (iSCSI target):
/sys/kernel/config/
  └── target/
      └── iscsi/
          └── iqn.2023-01.com.example:target/
              ├── tpgt_1/
              │   ├── luns/
              │   │   └── lun_0/
              │   │       └── ... (LUN 配置)
              │   └── ... (ACL 等)
              └── ...

# 创建对象示例:
#   mkdir /sys/kernel/config/target/iscsi/iqn.2023-01.../tpgt_1
#   # 内核自动创建对应的配置属性文件
#   echo "0" > /sys/kernel/config/target/iscsi/.../tpgt_1/attrib/...

# 关键实现:
#   configfs 基于 config_item / config_group 对象模型:
#   - config_group: 目录 (可包含子项)
#   - config_item: 项 (目录, 包含属性文件)
#   - configfs_attribute: 属性文件 (show/store 回调)
#   - configfs_subsystem: 顶层子系统 (自包含 config_group)

# 与 sysfs 的区别:
#   sysfs: 暴露已有内核对象, 只读/可写属性
#   configfs: 创建/销毁内核对象, 通过 mkdir/rmdir 控制生命周期
```

### 10.11 securityfs — 安全模块文件系统

```text
# 挂载点: /sys/kernel/security/
# 源文件: security/inode.c
# 类型: 伪文件系统, 基于 RAM

securityfs 为 LSM (Linux Security Module) 提供文件系统接口,
用于暴露安全策略、属性文件和统计信息。

# 核心 API:
#   securityfs_create_file(name, mode, parent, data, ops) → dentry
#   securityfs_create_dir(name, parent) → dentry
#   securityfs_remove(dentry) → 删除文件/目录

# 使用场景:
#   SELinux: /sys/kernel/security/selinux/
#     ├── access           # 访问向量缓存
#     ├── avc              # AVC 统计
#     ├── booleans/        # 布尔策略开关
#     ├── policy           # 加载安全策略
#     ├── status           # SELinux 状态
#     └── ... 
#   
#   AppArmor: /sys/kernel/security/apparmor/
#     ├── .access          # 访问控制
#     ├── features         # 支持的特性
#     ├── profiles/        # 安全配置文件
#     ├── tasks/           # 进程与 profile 映射
#     └── ...
#   
#   IMA: /sys/kernel/security/ima/
#     ├── binary_runtime_measurements  # 运行时度量列表
#     ├── policy                       # IMA 策略
#     └── ...

# 关键实现:
#   securityfs 基于 simple_fs (与 debugfs 类似):
#   - 内核初始化时挂载 securityfs 到 /sys/kernel/security/
#   - 各 LSM 模块通过 securityfs_create_*() 创建自己的文件/目录
#   - 安全策略文件通常由用户空间的安全管理工具写入
```

### 10.12 bpf — BPF 文件系统

```text
# 挂载点: /sys/fs/bpf/
# 源文件: kernel/bpf/inode.c
# 类型: 伪文件系统, 基于 RAM

bpf 文件系统 (BPF 文件系统) 用于持久化 BPF 程序和 map,
使得 BPF 资源在创建它们的进程退出后仍然存在, 并可供其他进程访问。

# 核心功能:
#   BPF 对象 pin 操作:
#     bpf_obj_pin(fd, path) → 将 BPF 程序/map 固定到文件系统
#     bpf_obj_get(path) → 通过路径获取 BPF 对象 fd
#
#   持久化生命周期:
#     BPF 程序创建 (仅当前进程可访问)
#       → pin 到 /sys/fs/bpf/ (持久化)
#         → 创建进程退出 (BPF 对象仍存活)
#           → 其他进程通过路径获取并访问

# 目录结构:
/sys/fs/bpf/
  ├── xdp/              # XDP 程序 pin 目录
  │   ├── pass          # XDP_PASS 程序
  │   └── drop          # XDP_DROP 程序
  ├── tc/               # TC (traffic control) 程序 pin 目录
  ├── tracing/          # tracing BPF 程序 pin 目录
  └── maps/             # BPF map pin 目录
      ├── config_map    # 配置 map
      └── stats_map     # 统计 map

# 使用示例:
#   # Pin BPF map
#   int map_fd = bpf_map_create(BPF_MAP_TYPE_HASH, ...);
#   bpf_obj_pin(map_fd, "/sys/fs/bpf/maps/my_map");
#   
#   # 另一个进程获取
#   int map_fd = bpf_obj_get("/sys/fs/bpf/maps/my_map");
#   bpf_map_lookup_elem(map_fd, &key, &value);

# 关键实现:
#   bpf 文件系统基于 simple_fs:
#   - bpf_fs_type → bpf_init_fs_context → bpf_fill_super
#   - BPF 对象通过 bpf_mkobj() 创建 inode 和 dentry
#   - 文件系统支持 mount, 允许用户命名空间挂载 (FS_USERNS_MOUNT)
```

### 10.13 pipefs — 管道文件系统

```text
# 挂载点: 不直接挂载, 内核内部使用
# 源文件: fs/pipe.c
# 类型: 伪文件系统, 为 pipe() 提供 VFS 接口

pipefs 是 Linux 管道机制的基础, 为 pipe() 系统调用返回的文件描述符
提供 VFS 文件对象支持。当进程调用 pipe() 时, 内核在 pipefs 中
分配 inode 和一对 file 结构 (读端和写端)。

# 核心原理:
#   pipe() 系统调用:
#     → do_pipe2()
#       → __do_pipe_flags()
#         → create_pipe_files()   # 创建管道文件对象
#           → get_pipe_inode()    # 在 pipefs 中分配 inode
#           → alloc_file()        # 创建读端 file 和写端 file
#         → __alloc_fd() × 2      # 分配两个 fd

# 管道 inode 的特殊性:
#   struct inode 的 i_pipe 字段指向 struct pipe_inode_info:
#     - pipe->head: 写入位置 (生产者)
#     - pipe->tail: 读取位置 (消费者)
#     - pipe->bufs[]: 环形缓冲区
#     - pipe->readers: 读端引用计数
#     - pipe->writers: 写端引用计数
#     - pipe->wait: 等待队列 (读写阻塞)

# 管道 file_operations:
#   读端: pipe_read() — 从 pipe->bufs 读取数据
#   写端: pipe_write() — 写入数据到 pipe->bufs
#   共同: pipe_poll(), pipe_ioctl(), pipe_release()

# 数据流:
#   写进程 → pipe_write() → pipe->bufs[tail→head] → pipe_read() → 读进程
#   (生产者)  (环形缓冲区写入)  (环形缓冲区读取)  (消费者)

# 管道大小:
#   - 默认 16 页 (64KB, 取决于 PAGE_SIZE)
#   - 可通过 fcntl(fd, F_SETPIPE_SZ, size) 调整
#   - 最大 /proc/sys/fs/pipe-max-size (默认 1MB)
```

### 10.14 hugetlbfs — 大页文件系统

```text
# 挂载点: /dev/hugepages/ (通常)
# 源文件: mm/hugetlb.c, fs/hugetlbfs/inode.c
# 类型: 伪文件系统, 基于内存

hugetlbfs 是大页 (HugeTLB) 机制的文件系统接口, 允许进程通过
mmap 映射大页内存。大页降低 TLB miss 概率, 提升大量内存访问的性能。

# 大页尺寸:
#   - x86: 2MB (默认), 1GB (可选)
#   - ARM64: 64KB, 2MB, 32MB, 1GB (取决于配置)
#   - PowerPC: 64KB, 2MB, 16MB, 1GB

# 目录结构:
/dev/hugepages/
  └── (文件由用户创建, 通过 mmap 映射到进程地址空间)

# 使用示例:
#   # 挂载 hugetlbfs (系统启动时自动完成)
#   mount -t hugetlbfs none /dev/hugepages
#   
#   # 预留大页 (需 root)
#   echo 20 > /proc/sys/vm/nr_hugepages   # 预留 20 个 2MB 大页
#   
#   # 使用大页
#   fd = open("/dev/hugepages/myfile", O_CREAT | O_RDWR);
#   addr = mmap(NULL, 2*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
#   # 使用 addr 访问大页内存
#   # 写时触发缺页, 分配大页

# 关键实现:
#   hugetlbfs 的 file_operations:
#   - read_iter / write_iter: 不支持 (大页文件通常通过 mmap 访问)
#   - mmap: hugetlbfs_file_mmap() → 设置 VMA 为大页映射
#   - fallocate: 预分配大页空间
#
#   大页分配流程:
#     mmap → hugetlbfs_file_mmap()
#     → 缺页 → hugetlb_fault()
#       → hugetlb_no_page() → alloc_huge_page() → 从预留池分配
#       → 设置页表项 (PMD/PUD 级别, 跳过 PT)

# 使用场景:
#   - 数据库 (Oracle, PostgreSQL 可配置大页)
#   - DPDK (数据平面开发套件)
#   - KVM 虚拟化 (guest 大页内存)
#   - 高性能计算 (HPC) 大内存工作负载
```

### 10.15 rpc_pipefs — RPC 管道文件系统

```text
# 挂载点: /var/lib/nfs/rpc_pipefs/ (通常)
# 源文件: net/sunrpc/rpc_pipe.c
# 类型: 伪文件系统, 基于 pipe

rpc_pipefs 是 SUNRPC 服务 (NFS 客户端) 与内核 RPC 层之间的
通信管道文件系统, 主要用于 NFS 认证和凭据管理。

# 核心作用:
#   NFS 客户端挂载远程文件系统时, 需要与 rpc.gssd 等用户空间
#   守护进程通信以获取 Kerberos 认证凭据。rpc_pipefs 提供
#   内核与用户空间之间的双向通信通道。

# 目录结构:
/var/lib/nfs/rpc_pipefs/
  ├── nfsd/              # NFS 服务端信息
  │   └── ...            # 通过 rpc_pipefs 暴露 NFS 服务器状态
  ├── nfs/               # NFS 客户端信息
  │   └── <server>/      # 每个 NFS 服务器对应的目录
  │       ├── rpc        # RPC 管道
  │       └── ...        # 凭据缓存等
  ├── gssd/              # rpc.gssd 通信管道
  │   └── clntXX         # Kerberos 认证通道
  └── cache/             # 缓存管理
      └── ...            # 认证缓存/映射缓存

# 关键实现:
#   rpc_pipefs 基于 pipe 机制:
#   - rpc_mkpipe() 创建管道, 用于内核与用户空间通信
#   - 用户空间守护进程 (rpc.gssd, rpc.idmapd) 通过读写管道
#     与内核 RPC 层交换认证凭据
#   - 文件系统在 NFS 客户端初始化时自动挂载

# 工作流程:
#   NFS 挂载 → rpc_pipefs 已在 /var/lib/nfs/rpc_pipefs/
#   → 需要 Kerberos 认证时, 内核通过管道向 rpc.gssd 发送请求
#   → rpc.gssd 读取请求, 获取 Kerberos ticket, 写回管道
#   → 内核读取凭据, 完成认证挂载
```

### 10.16 devpts — 伪终端文件系统

```text
# 挂载点: /dev/pts/
# 源文件: fs/devpts/inode.c
# 类型: 伪文件系统, 基于 RAM

devpts 管理伪终端 (PTY) 设备文件, 每个 SSH/telnet/终端模拟器
会话对应 /dev/pts/ 下一个编号文件 (如 /dev/pts/0)。

# PTY 对:
#   每个伪终端由一对设备组成:
#   - 主端 (master): /dev/ptmx 或 /dev/pts/ptmx — 终端模拟器打开
#   - 从端 (slave):  /dev/pts/N — 被 shell/程序作为标准 I/O 使用

# 目录结构:
/dev/pts/
  ├── ptmx             # PTY 复用器 (打开获取新 PTY)
  ├── 0                # 第 0 个伪终端从端
  ├── 1                # 第 1 个伪终端从端
  ├── 2                # 第 2 个伪终端从端
  └── ...              # 从端编号递增

# 工作流程:
#   SSH 连接建立:
#     SSH 服务器进程打开 /dev/ptmx → 获得新的 PTY 对
#     → 主端 fd 返回给 SSH 服务器, 从端 /dev/pts/N 分配给 shell
#     → SSH 服务器读取主端: 从网络接收键盘输入, 写入主端
#     → shell 从从端读取: 获得键盘输入
#     → shell 写入从端: 输出到终端
#     → SSH 服务器从主端读取: 获得终端输出, 发送到网络

# 关键实现:
#   devpts 的 file_operations:
#   - /dev/ptmx: ptmx_open() → 分配新 PTY 对
#   - /dev/pts/N: pts_open() → 打开从端
#   - 读写: 通过 pty_unix98_ops (pty 驱动) 在主从端之间传递数据
#
#   devpts 支持多实例:
#   - 容器场景中, 每个容器可有独立的 devpts 挂载
#   - mount -t devpts newinstance /dev/pts (创建新实例)
#   - 不同容器的 /dev/pts/N 互不干扰

# 使用场景:
#   - SSH 远程登录
#   - telnet 终端会话
#   - xterm / gnome-terminal / konsole 等终端模拟器
#   - screen / tmux 终端复用器
#   - Docker/lxc 容器中的终端交互
```

## Part VI: 文件系统与块设备链路

## 11. 文件读写与 NVMe 块设备挂载链路关键数据结构

本节从系统调用入口出发，逐层追踪文件打开/读写流程和 NVMe 块设备挂载流程中
涉及的关键数据结构，并分析数据结构之间的关联关系。

### 11.1 文件打开链路 (sys_openat → PCIe 设备)

```text
# 完整链路: 用户态 open() → 系统调用 → VFS → 具体文件系统 → 块设备层 → NVMe 驱动 → PCIe

# 层级 1: 系统调用层
sys_openat(dfd, pathname, flags, mode)
  ↓
  struct pt_regs:    保存系统调用参数的寄存器上下文
  struct filename:   用户态路径名的内核拷贝 (name, uptr, refcnt)

# 层级 2: VFS 路径查找层
path_openat(nd, open_flag, op)
  ↓
  struct nameidata (nd):  路径查找上下文 (path, inode, flags, seq)
  ├─ struct path:         当前查找路径
  │   ├─ struct vfsmount *mnt  → 挂载实例
  │   └─ struct dentry   *dentry  → 目录项
  └─ struct inode  *inode    → 当前 inode

link_path_walk(name, nd)  →  walk_component()  →  lookup_fast() / lookup_slow()
  ↓
  struct dentry:           路径组件对应的目录项
  ├─ d_hash:               hash 表节点 (dcache 快速查找)
  ├─ d_parent:             父目录 dentry
  ├─ d_name:               文件名 (struct qstr)
  ├─ d_inode:              指向的 inode (NULL 表示负 dentry)
  └─ d_op->d_compare():    自定义名称比较函数

# 层级 3: VFS 文件对象层
do_dentry_open(file, inode, open_flag)
  ↓
  struct file:             已打开的文件对象
  ├─ f_mode:               打开模式 (FMODE_READ/WRITE 等)
  ├─ f_op:                 file_operations 函数表 (read_iter/write_iter/mmap 等)
  ├─ f_mapping:            address_space (页缓存映射)
  ├─ f_inode:              关联的 inode
  ├─ f_pos:                当前读写位置
  ├─ private_data:         文件系统私有数据 (如 ext4_file_info)
  └─ f_cred:               打开时的进程凭证

  ↓ 调用 f_op->open() → ext4_file_open() (具体文件系统)

# 层级 4: 具体文件系统层 (以 ext4 为例)
ext4_file_open(inode, file)
  ↓
  struct ext4_inode_info:  ext4 扩展 inode 信息
  ├─ vfs_inode:            嵌入的 VFS inode 结构
  ├─ i_data:               ext4 的 address_space
  ├─ i_disksize:           磁盘上文件大小
  ├─ i_block_group:        所在块组
  ├─ i_block:              ext4 块映射 (ext4_extent 或 block map)
  └─ i_cached_seek_hole:   预分配/空洞查找缓存

# 层级 5: 块设备层 (ext4 需要读取元数据时)
sb_bread(sb, block)  →  __bread_gfp(bdev, block, size, gfp)
  ↓
  struct block_device:     块设备实例
  ├─ bd_disk:              gendisk (通用磁盘)
  ├─ bd_queue:             request_queue (请求队列)
  ├─ bd_mapping:           块设备页缓存
  └─ bd_start_sect:        分区起始扇区

  struct buffer_head:      缓冲区头 (块设备 I/O 基本单位)
  ├─ b_page/b_folio:       映射到页/folio
  ├─ b_blocknr:            块号
  ├─ b_size:               块大小
  ├─ b_data:               指向页内数据的指针
  └─ b_bdev:               关联的 block_device

  → 缺页或缓存未命中时发起 bio (见 11.2 读写链路)

# 层级 6: NVMe 驱动层 (块设备请求处理)
nvme_queue_rq(hctx, bd)  →  nvme_setup_cmd(ns, req)  →  nvme_submit_cmd()
  ↓
  struct nvme_ns:          NVMe 命名空间
  ├─ ctrl:                 nvme_ctrl (控制器)
  ├─ queue:                request_queue (块层请求队列)
  ├─ disk:                 gendisk (通用磁盘)
  └─ head:                 nvme_ns_head (多路径命名空间头)

  struct nvme_request:     NVMe 请求私有数据 (嵌入在 struct request 的 pdu 中)
  ├─ cmd:                  nvme_command (NVMe 命令)
  ├─ result:               命令完成结果
  ├─ retries:              重试次数
  └─ ctrl:                 所属控制器

  struct nvme_command:     NVMe 命令 (union)
  ├─ rw.opcode:            命令操作码 (0x01=write, 0x02=read)
  ├─ rw.nsid:              命名空间 ID
  ├─ rw.slba:              起始逻辑块地址
  ├─ rw.length:            传输块数 (以块为单位, 0-based)
  └─ rw.dptr:              数据指针 (PRP 或 SGL)

  struct nvme_queue:       NVMe 提交/完成队列
  ├─ dev:                  nvme_dev (PCIe 设备)
  ├─ sq_cmds:              提交队列内存 (DMA)
  ├─ cqes:                 完成队列内存 (DMA)
  ├─ sq_dma_addr:          提交队列 DMA 地址
  ├─ cq_dma_addr:          完成队列 DMA 地址
  └─ q_db:                 门铃寄存器 MMIO 地址

# 层级 7: PCIe 设备层
  struct nvme_dev:         NVMe PCIe 设备实例
  ├─ pci_dev (dev):        父 PCIe 设备 (struct pci_dev)
  ├─ bar:                  BAR0 MMIO 基地址 (寄存器访问)
  ├─ dbs:                  门铃寄存器 MMIO 地址
  ├─ ctrl:                 嵌入的 nvme_ctrl 结构
  ├─ queues[]:             NVMe 队列数组
  ├─ online_queues:        在线队列数
  └─ q_depth:              队列深度

  struct pci_dev:          PCIe 设备描述
  ├─ dev:                  通用设备模型
  ├─ vendor:               Vendor ID (如 0x8086 Intel)
  ├─ device:               Device ID (如 0x0a54 NVMe)
  ├─ bus:                  所属 PCIe 总线
  ├─ devfn:                设备功能号
  ├─ driver:               绑定的 PCIe 驱动 (nvme_driver)
  └─ resource[]:           BAR 资源 (MMIO/I/O 地址范围)

  # NVMe 门铃机制:
  #   写 SQ 尾指针到门铃寄存器 (MMIO) → 通知 NVMe 控制器处理
  #   控制器完成 → 写 CQ 头指针 → 中断/轮询通知驱动
  #   nvmeq->q_db = dev->dbs + 2 * qid * (dev->db_stride + 1)
```

### 11.2 文件读写链路 (sys_read/sys_write → PCIe 设备)

```text
# 完整链路: 用户态 read() → 系统调用 → VFS → 页缓存 → 块设备层 → NVMe 驱动 → PCIe

# 路径一: 缓存 I/O (Buffered I/O) — 默认路径

sys_read(fd, buf, count)
  ↓
  ksys_read() → vfs_read()
  ↓
  struct fd:          文件描述符封装 (file + flags)
  struct kiocb:       I/O 控制块
  ├─ ki_filp:         文件对象指针
  ├─ ki_pos:          文件偏移
  ├─ ki_flags:        IOCB_* 标志
  └─ ki_complete:     异步完成回调

  →  file->f_op->read_iter() 或 generic_file_read_iter()
    ↓
    filemap_read(iocb, iter, already_read)
      ↓
      # 页缓存查找
      filemap_get_folios_contig(mapping, index, end, &fbatch)
        ↓
        struct address_space:     地址空间 (页缓存根)
        ├─ host:                  所属 inode
        ├─ i_pages:               XArray (页缓存索引树)
        ├─ a_ops:                 address_space_operations
        └─ nrpages:              页缓存页数

        struct folio:             内存页 (folio, 页缓存基本单位)
        ├─ flags:                 页标志 (PG_locked, PG_uptodate, PG_dirty 等)
        ├─ mapping:               所属 address_space
        ├─ index:                 文件页索引 (pgoff_t)
        └─ lru:                   LRU 链表 (内存回收)

      → 缺页 (cache miss) →  page_cache_sync_ra()  →  filemap_read_folio()
        ↓
        a_ops->read_folio(file, folio)  →  ext4_read_folio()
          ↓
          # 提交 bio 到块设备层
          struct bio:           块 I/O 基本单位
          ├─ bi_bdev:           目标 block_device
          ├─ bi_opf:            I/O 操作 (REQ_OP_READ/WRITE)
          ├─ bi_iter:           迭代器 (bi_sector 起始扇区, bi_size 大小)
          ├─ bi_io_vec:         数据向量数组 (page + offset + len)
          ├─ bi_vcnt:           向量数
          ├─ bi_end_io:         完成回调
          └─ bi_next:           链表 (bio 链)

          submit_bio(bio)
            ↓
            # 块层处理
            struct request_queue:   请求队列 (blk-mq)
            ├─ queue_hw_ctx[]:      硬件分发队列
            ├─ mq_ops:              blk_mq_ops
            ├─ queuedata:           驱动私有数据 (nvme_ns 等)
            └─ limits:              队列限制 (最大扇区/段数等)

            blk_mq_submit_bio(bio)
              ↓
              struct request:       块层请求 (一个或多个 bio 合并)
              ├─ q:                 request_queue
              ├─ mq_hctx:           硬件上下文
              ├─ cmd_flags:         命令标志 (REQ_OP_READ 等)
              ├─ bio:               bio 链表头
              ├─ biotail:           bio 链表尾
              ├─ __sector:          起始扇区
              └─ __data_len:        数据总长度

              → 驱动 queue_rq() → nvme_queue_rq()
                ↓
                # NVMe 命令构建 (见 11.1 层级 6-7)
                nvme_setup_cmd(ns, req) → nvme_submit_cmd()
                  ↓
                  # 写门铃寄存器 → PCIe MMIO → NVMe 控制器处理
                  writel(val, nvmeq->q_db)

# 路径二: 直接 I/O (Direct I/O) — 绕过页缓存

sys_read() → vfs_read() → file->f_op->read_iter()
  →  ext4_dio_read_iter() 或 iomap_dio_rw()
    ↓
    struct iomap_dio:    直接 I/O 描述符
    ├─ iocb:             原始 I/O 控制块
    ├─ bio:              直接提交的 bio
    ├─ size:             总大小
    ├─ flags:            IOMAP_DIO_* 标志
    └─ error:            错误码

    → 直接构建 bio → submit_bio() → blk_mq_submit_bio() → nvme_queue_rq()

# 路径三: 写路径 (sys_write)

sys_write() → vfs_write() → file->f_op->write_iter() 或 generic_file_write_iter()
  ↓
  # 缓存写入
  generic_perform_write(iocb, i)
    ↓
    a_ops->write_begin() → 获取 folio (缺页分配)
    → 拷贝用户数据到 folio
    → a_ops->write_end() → 标记 folio dirty
    ↓
    # 写回 (writeback)
    folio_mark_dirty() → 后台 bdi_writeback 线程
    → wb_workfn() → writeback_sb_inodes()
      → ext4_writepages() → iomap_writepages()
        → 构建 bio → submit_bio() → ... → nvme_queue_rq()

  # 直接写入
  ext4_dio_write_iter() → iomap_dio_rw()
    → 直接构建 bio → submit_bio() → ... → nvme_queue_rq()
```

### 11.3 NVMe 块设备挂载链路 (mount → PCIe 设备)

```text
# 完整链路: 用户态 mount() → 系统调用 → VFS → 具体文件系统 → 块设备 → NVMe 驱动 → PCIe

# 层级 1: 系统调用层
sys_mount(src, target, type, flags, data)
  ↓
  do_mount() → path_mount()
    → do_new_mount() → parse_monolithic_mount_data()
    → vfs_kern_mount() → vfs_get_tree(fs_type, flags)
      ↓
      # 调用文件系统的 mount 回调 → ext4_init_fs_context()
      → ext4_get_tree() → ext4_fill_super()
        ↓
        # 打开块设备
        ext4_fill_super(sb, descr, ro)
          → ext4_mount() → bdev_get_active_path()
          → blkdev_get_by_dev() → bd_start_claiming()
            ↓
            struct block_device:   块设备 (详见 11.1)
            ├─ bd_disk:           通用磁盘
            ├─ bd_queue:          请求队列
            ├─ bd_mapping:        块设备页缓存
            ├─ bd_openers:        打开计数
            └─ bd_holder:         持有者 (文件系统)

            struct gendisk:       通用磁盘
            ├─ disk_name:         磁盘名 (如 "nvme0n1")
            ├─ part0:             分区 0 (整个磁盘)
            ├─ fops:              block_device_operations
            ├─ queue:             request_queue
            ├─ private_data:      驱动私有数据 (nvme_ns)
            └─ part_tbl:          分区表 XArray

        # 读取 ext4 超级块
        → ext4_read_super() → sb_bread(sb, 1)  (读取块设备第 1 块)
          ↓
          struct buffer_head:     缓冲区头
          ├─ b_blocknr:           块号 (1, super_block)
          ├─ b_data:              指向包含 ext4_super_block 的页
          └─ b_bdev:              block_device

          → 解析 ext4_super_block → 初始化 ext4_sb_info
            ↓
            struct ext4_super_block:  ext4 磁盘超级块
            ├─ s_inodes_count:        inode 总数
            ├─ s_blocks_count_lo:     块总数
            ├─ s_first_data_block:    第一个数据块
            ├─ s_log_block_size:      块大小 log2
            ├─ s_log_cluster_size:    簇大小 log2
            ├─ s_blocks_per_group:    每块组块数
            ├─ s_clusters_per_group:  每块组簇数
            ├─ s_inodes_per_group:    每块组 inode 数
            ├─ s_mtime:               最后挂载时间
            ├─ s_wtime:               最后写入时间
            ├─ s_magic:               EXT4_SUPER_MAGIC (0xEF53)
            ├─ s_state:               文件系统状态
            ├─ s_errors:              错误处理方式
            ├─ s_minor_rev_level:     次版本号
            ├─ s_lastcheck:           最后检查时间
            ├─ s_checkinterval:       检查间隔
            ├─ s_creator_os:          创建者 OS
            ├─ s_rev_level:           版本号
            ├─ s_def_resuid:          默认保留用户
            ├─ s_def_resgid:          默认保留组
            ├─ s_first_ino:           第一个非保留 inode
            ├─ s_inode_size:          inode 结构大小
            ├─ s_block_group_nr:      当前块组号
            ├─ s_feature_compat:      兼容特征集
            ├─ s_feature_incompat:    不兼容特征集 (ext4 必需)
            ├─ s_feature_ro_compat:   只读兼容特征集
            ├─ s_uuid:               UUID
            ├─ s_volume_name:         卷名
            ├─ s_last_mounted:        最后挂载路径
            ├─ s_algorithm_usage_bitmap: 算法位图
            ├─ s_checksum_type:       校验和类型
            ├─ s_checksum:            超级块校验和
            └─ s_encryption_level:    加密级别

            struct ext4_sb_info:     ext4 超级块内存信息
            ├─ s_sb:                 VFS super_block
            ├─ s_es:                 ext4_super_block 磁盘副本
            ├─ s_groups_count:       块组总数
            ├─ s_blockgroup_lock:    块组锁
            ├─ s_group_desc:         块组描述符缓存
            ├─ s_mb_alloc:           多块分配器
            ├─ s_extent_cache:       extent 缓存
            ├─ s_journal:            日志 (journal_t)
            ├─ s_commit_interval:    提交间隔
            ├─ s_max_batch_time:     最大批量时间
            └─ s_min_batch_time:     最小批量时间

        # 读取根目录 inode
        → ext4_read_root_inode() → ext4_iget(sb, EXT4_ROOT_INO)
          ↓
          struct inode:             VFS inode
          ├─ i_mode:               文件类型和权限
          ├─ i_uid/i_gid:          用户/组 ID
          ├─ i_size:               文件大小
          ├─ i_blocks:             文件占用的块数
          ├─ i_atime/i_mtime/i_ctime: 时间戳
          ├─ i_op:                 inode_operations
          ├─ i_fop:                file_operations
          ├─ i_sb:                 super_block
          ├─ i_mapping:            address_space
          └─ i_private:            文件系统私有数据 (ext4_inode_info)

          struct ext4_inode_info:   ext4 inode 扩展信息
          ├─ vfs_inode:            嵌入的 VFS inode
          ├─ i_data:               地址空间 (ext4 页缓存)
          ├─ i_disksize:           磁盘上文件大小
          ├─ i_flags:              ext4 文件标志
          ├─ i_file_acl:           文件 ACL 块
          ├─ i_dtime:              删除时间
          ├─ i_block_group:        所在块组
          ├─ i_block:              块映射数组 (ext4_extent 或间接块)
          ├─ i_prealloc_list:      预分配列表
          └─ i_cached_extent:      缓存的 extent

# 层级 2: 块设备层 (I/O 请求路径, 同 10.1)
  → sb_bread() → __bread_gfp() → submit_bh()
    → bio_alloc() → submit_bio()
      → blk_mq_submit_bio() → blk_mq_try_issue_directly()
        → blk_mq_queue_rq() → nvme_queue_rq()

# 层级 3: NVMe 驱动层
  nvme_queue_rq(hctx, bd)
    ↓
    struct nvme_ns:             NVMe 命名空间
    ├─ queue:                   request_queue
    ├─ disk:                    gendisk
    ├─ ctrl:                    nvme_ctrl
    └─ head:                    nvme_ns_head

    struct nvme_request:        NVMe 请求 (嵌入 request)
    struct nvme_command:        NVMe 命令 (见 11.1)
    struct nvme_queue:          NVMe 队列 (见 11.1)

    → nvme_submit_cmd(nvmeq, cmd, sq_tail)
      → writel(sq_tail, nvmeq->q_db)  # 门铃寄存器 MMIO 写

# 层级 4: PCIe 设备层
    struct nvme_dev:            NVMe PCIe 设备 (见 11.1)
    ├─ dev:                     struct pci_dev *
    ├─ bar:                     BAR0 MMIO
    ├─ dbs:                     门铃寄存器
    ├─ ctrl:                    嵌入的 nvme_ctrl
    └─ queues[]:                队列数组

    struct pci_dev:             PCIe 设备 (见 11.1)

# 层级 5: NVMe 设备初始化流程 (PCIe 探测 → 命名空间上线)
  # 系统启动/设备热插拔时:
  pci_register_driver(&nvme_driver)
    → pci_match_device() → 匹配 vendor/device ID
    → nvme_probe(pci_dev, id)
      ↓
      struct pci_dev:           匹配的 PCIe NVMe 设备
      ├─ vendor:                0x8086 (Intel) / 0x144d (Samsung) 等
      ├─ device:                具体 NVMe 控制器型号
      └─ irq:                   MSI/MSI-X 中断号

      nvme_probe():
        → pcim_enable_device(pci_dev)      # 开启 PCIe 总线主控
        → pcim_iomap_regions(pci_dev, ...) # BAR0 MMIO 映射
        → dma_set_mask_and_coherent()      # 设置 DMA 掩码
        → nvme_init_ctrl(&dev->ctrl, ...)  # 初始化 NVMe 控制器
        → nvme_init_ctrl_finish()          # 识别控制器 (Identify)
          → nvme_scan_work()               # 扫描命名空间
            → nvme_scan_ns_list()          # 获取命名空间列表
            → nvme_alloc_ns(ctrl, nsid)    # 分配命名空间
              → nvme_ns_alloc()            # 分配 nvme_ns + gendisk
              → nvme_alloc_io_tag_set()    # 设置 blk-mq tag set
              → device_add_disk()          # 注册 gendisk → 出现 /dev/nvme0n1
```

### 11.4 数据结构关系图

```text
# 文件打开/读写链路数据结构关系 (从用户态到 PCIe)

┌──────────────────────────────────────────────────────────────────────────┐
│                            用户态应用程序                                  │
│  fd = open("/mnt/data/file.txt", O_RDWR)                                 │
│  read(fd, buf, 4096) / write(fd, buf, 4096)                             │
└──────────────────────────┬───────────────────────────────────────────────┘
                           │ 系统调用
                           ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                     系统调用层                                            │
│  struct pt_regs → struct filename (路径名拷贝)                           │
│  struct fd → struct file (fd 到 file 的映射)                             │
└──────────────────────────┬───────────────────────────────────────────────┘
                           │ VFS 通用层
                           ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                     VFS 核心层                                            │
│  struct nameidata ──→ struct path ──→ struct vfsmount (挂载实例)          │
│       │                  └─→ struct dentry (目录项缓存)                    │
│       │                        │                                          │
│       └─→ struct inode  ◄──────┘  (磁盘元数据内存表示)                    │
│              │                                                           │
│              ├─ i_sb → struct super_block (文件系统实例)                   │
│              ├─ i_mapping → struct address_space (页缓存根)               │
│              │    └─ i_pages → struct xarray (页缓存索引树)               │
│              │         └─ struct folio (内存页)                          │
│              │              └─ struct page (底层页结构)                   │
│              └─ i_private → ext4_inode_info (文件系统私有)                 │
│                                                                          │
│  struct file ←─ do_dentry_open()                                         │
│  ├─ f_op → file_operations (read_iter/write_iter/mmap)                   │
│  ├─ f_mapping → address_space (同 inode->i_mapping)                     │
│  ├─ f_inode → inode                                                      │
│  └─ private_data → ext4_file_info                                        │
└──────────────────────────┬───────────────────────────────────────────────┘
                           │ 块设备层
                           ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                     块设备层 (Block Layer)                                │
│  struct block_device ◄── struct gendisk ◄── struct nvme_ns               │
│  ├─ bd_disk → gendisk        ├─ disk_name  "nvme0n1"                    │
│  ├─ bd_queue → request_queue  ├─ queue → request_queue                   │
│  └─ bd_mapping → address_space ├─ private_data → nvme_ns                 │
│                                 └─ fops → block_device_operations         │
│                                                                          │
│  struct bio ──→ struct request ──→ blk_mq_hw_ctx                         │
│  (bi_bdev,      (q, bio,         (queue, driver_data,                    │
│   bi_opf,        __sector,        ctx_map, tags)                         │
│   bi_iter)       __data_len)                                              │
│                                                                          │
│  struct buffer_head: 块 I/O 桥梁 (b_bdev, b_blocknr, b_data)            │
└──────────────────────────┬───────────────────────────────────────────────┘
                           │ NVMe 驱动
                           ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                     NVMe 驱动层                                           │
│  struct nvme_ns ──→ struct nvme_ctrl ──→ struct nvme_dev                 │
│  ├─ queue → req_queue  ├─ admin_q         ├─ ctrl (嵌入)                 │
│  ├─ disk → gendisk     ├─ namespaces      ├─ bar → BAR0 MMIO            │
│  └─ ctrl → nvme_ctrl   └─ tagset          ├─ dbs → 门铃寄存器           │
│                                           └─ queues[] → nvme_queue[]    │
│  struct nvme_request (嵌入在 request 中)                                  │
│  ├─ cmd → nvme_command (opcode, nsid, slba, length, dptr)               │
│  └─ ctrl → nvme_ctrl                                                     │
│                                                                          │
│  struct nvme_queue: 提交/完成队列                                         │
│  ├─ sq_cmds:         提交队列 DMA 内存                                    │
│  ├─ cqes:            完成队列 DMA 内存                                    │
│  └─ q_db:            门铃寄存器 MMIO 地址                                │
│                                                                          │
│  # 门铃机制: 写 SQ tail → 门铃寄存器 (MMIO 写) → NVMe 控制器处理         │
│  # 完成: 控制器写 CQ → 中断/轮询 → 驱动读取完成项                         │
└──────────────────────────┬───────────────────────────────────────────────┘
                           │ PCIe 总线
                           ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                     PCIe 设备层                                           │
│  struct pci_dev                                                          │
│  ├─ vendor:device  → 匹配 nvme_driver.id_table                          │
│  ├─ bus → pci_bus (PCIe 总线拓扑)                                        │
│  ├─ devfn (设备功能号)                                                   │
│  ├─ irq (MSI/MSI-X 中断向量)                                             │
│  ├─ resource[] (BAR 空间: MMIO, I/O 端口)                                │
│  └─ driver → pci_driver (nvme_driver)                                    │
│                                                                          │
│  struct pci_driver:                                                      │
│  ├─ name: "nvme"                                                         │
│  ├─ id_table: pci_device_id[] 表                                         │
│  ├─ probe: nvme_probe()  ← 入口点                                       │
│  └─ remove: nvme_remove()                                               │
│                                                                          │
│  # PCIe 配置空间: 通过 pci_read_config_*() 访问 BAR/IRQ 等               │
│  # MMIO BAR: 通过 ioremap() 映射到内核虚拟地址空间                        │
│  # DMA: 通过 dma_alloc_coherent() / streaming DMA API                   │
└──────────────────────────────────────────────────────────────────────────┘
```

### 11.5 关键数据结构关系表

| 层级 | 数据结构 | 核心关联 | 生命周期 |
|------|---------|---------|---------|
| 系统调用 | `struct pt_regs` | 保存 syscall 参数寄存器 | 单次系统调用 |
| 系统调用 | `struct filename` | 指向用户态路径名内核拷贝 | 路径查找期间 |
| VFS 路径 | `struct nameidata` | 持有 `path` + `inode` | 路径查找期间 |
| VFS 路径 | `struct path` | `vfsmount` + `dentry` | 路径查找期间 |
| VFS 核心 | `struct dentry` | `d_inode` → `inode`, `d_parent` → 父 dentry | dcache 缓存 |
| VFS 核心 | `struct inode` | `i_sb` → `super_block`, `i_mapping` → `address_space` | 文件存在期间 |
| VFS 核心 | `struct super_block` | `s_bdev` → `block_device`, `s_root` → 根 dentry | 挂载期间 |
| VFS 核心 | `struct file` | `f_inode` → `inode`, `f_mapping` → `address_space` | 打开期间 |
| VFS 核心 | `struct address_space` | `host` → `inode`, `i_pages` → `xarray` (页缓存) | 与 inode 一致 |
| 页缓存 | `struct folio` / `struct page` | `mapping` → `address_space`, `index` → 文件偏移 | 内存回收 |
| 块设备 | `struct block_device` | `bd_disk` → `gendisk`, `bd_queue` → `request_queue` | 设备存在期间 |
| 块设备 | `struct gendisk` | `private_data` → `nvme_ns`, `queue` → `request_queue` | 设备存在期间 |
| 块设备 | `struct buffer_head` | `b_bdev` → `block_device`, `b_page` → `folio` | I/O 期间 |
| 块层 I/O | `struct bio` | `bi_bdev` → `block_device`, `bi_io_vec` → 数据页 | 单次 I/O |
| 块层 I/O | `struct request` | `bio` → bio 链, `q` → `request_queue`, `mq_hctx` → hctx | 单次 I/O |
| 块层 | `struct request_queue` | `mq_ops` → `blk_mq_ops`, `queuedata` → `nvme_ns` | 设备存在期间 |
| NVMe | `struct nvme_ns` | `ctrl` → `nvme_ctrl`, `disk` → `gendisk`, `queue` → `request_queue` | 命名空间存在期间 |
| NVMe | `struct nvme_ctrl` | `dev` → `pci_dev`, `tagset` → `blk_mq_tag_set` | 控制器存在期间 |
| NVMe | `struct nvme_request` | `cmd` → `nvme_command`, `ctrl` → `nvme_ctrl` | 单次 I/O |
| NVMe | `struct nvme_command` | `rw.opcode/nsid/slba/length/dptr` | 单次命令 |
| NVMe | `struct nvme_queue` | `sq_cmds` (DMA), `cqes` (DMA), `q_db` (MMIO) | 队列存在期间 |
| NVMe PCIe | `struct nvme_dev` | `bar` (MMIO), `dbs` (门铃), `queues[]`, `ctrl` (嵌入) | 设备存在期间 |
| PCIe | `struct pci_dev` | `resource[]` (BAR), `irq`, `driver` → `nvme_driver` | 设备存在期间 |
| PCIe | `struct pci_driver` | `probe` → `nvme_probe()`, `id_table` → 设备匹配表 | 驱动加载期间 |
| ext4 | `struct ext4_sb_info` | `s_sb` → `super_block`, `s_es` → `ext4_super_block` | 挂载期间 |
| ext4 | `struct ext4_inode_info` | `vfs_inode` → `inode`, `i_data` → `address_space` | 文件存在期间 |
| ext4 | `struct ext4_super_block` | 磁盘超级块, 解析后填充 `ext4_sb_info` | 磁盘上持久 |

### 11.6 关键链路总结

```text
# 文件 open 核心链路 (完整路径)
sys_openat()
  → path_openat()            [struct nameidata → path → dentry → inode]
    → do_dentry_open()       [struct file ← f_op ← inode->i_fop]
      → ext4_file_open()     [struct ext4_inode_info, 初始化]
        → sb_bread()         [struct buffer_head, 读取元数据]
          → submit_bh()      [struct bio → struct request]
            → nvme_queue_rq() [struct nvme_request → nvme_command]
              → writel()     [PCIe MMIO 门铃寄存器]

# 文件 read 核心链路 (缓存 I/O, 缺页路径)
sys_read()
  → vfs_read()               [struct fd → struct file → kiocb]
    → filemap_read()          [struct address_space → xarray]
      → filemap_get_folio()  [struct folio, 页缓存命中则返回]
        → filemap_read_folio() [缺页, 调用 a_ops->read_folio]
          → ext4_read_folio() [ext4 读页回调]
            → submit_bio()    [struct bio → struct request]
              → nvme_queue_rq() [struct nvme_command → nvme_submit_cmd]
                → writel()    [PCIe MMIO 门铃 → NVMe 控制器 DMA 读]

# NVMe 块设备挂载核心链路
sys_mount("/dev/nvme0n1", "/mnt", "ext4", ...)
  → path_mount() → do_new_mount() → vfs_get_tree()
    → ext4_get_tree() → ext4_fill_super()
      → blkdev_get_by_dev()  [struct block_device → gendisk → nvme_ns]
      → ext4_read_super()    [sb_bread() → buffer_head → ext4_super_block]
      → ext4_iget(ROOT_INO)  [struct inode → ext4_inode_info]
      → sb->s_root = d_make_root() [struct dentry: 根目录 dentry]
    → do_add_mount()         [struct mount → vfsmount, 挂载树]

# NVMe 设备初始化核心链路
pci_register_driver(&nvme_driver)
  → nvme_probe(pci_dev)      [struct pci_dev → struct nvme_dev]
    → pcim_enable_device()    [PCIe 配置空间: 开启总线主控]
    → pcim_iomap_regions()    [BAR0 MMIO 映射 → nvme_dev->bar]
    → nvme_init_ctrl()        [struct nvme_ctrl, 初始化控制器状态机]
    → nvme_init_ctrl_finish() [NVMe Identify 命令 → 获取控制器信息]
      → nvme_scan_work()      [扫描命名空间]
        → nvme_alloc_ns()     [struct nvme_ns + gendisk + request_queue]
          → device_add_disk() [注册 gendisk → /dev/nvme0n1]
```

**数据结构关系总结:**
- 系统调用层通过 `struct pt_regs` 获取参数, 转换为 VFS 对象
- VFS 层通过 `struct dentry` → `struct inode` → `struct super_block` 建立文件系统实例
- 页缓存通过 `struct address_space` → `struct xarray` → `struct folio` 管理内存数据
- 块设备层通过 `struct block_device` → `struct gendisk` → `struct nvme_ns` 桥接文件系统与驱动
- NVMe 驱动通过 `struct nvme_dev` → `struct pci_dev` 将块 I/O 转化为 PCIe 事务
- 最终通过 MMIO 门铃寄存器写入, 触发 NVMe 控制器 DMA 操作完成数据传输
```