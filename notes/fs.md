# Linux 文件系统 (VFS) 核心流程分析

> 本文档覆盖 Linux 内核 VFS 层的 5 个核心流程:
> 每个流程均以函数调用栈 ASCII 树形式呈现, 并附有关键数据结构变更注释

## 目录

- [1. 根目录创建过程](#1-根目录创建过程)
- [2. 文件系统类型链表](#2-文件系统类型链表)
- [3. 挂载树结构](#3-挂载树结构)
- [4. 文件打开路径](#4-文件打开路径)
- [5. 路径查找过程](#5-路径查找过程)
- [6. 文件读写路径](#6-文件读写路径)
- [7. 挂载过程](#7-挂载过程)

---

## 1. 根目录创建过程

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

## 2. 文件系统类型 - 链表结构

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
  │
  ├─ shmem_fs_type
  │    name          = "tmpfs"
  │    init_fs_context = shmem_init_fs_context
  │    kill_sb       = kill_litter_super
  │    fs_flags      = FS_USERNS_MOUNT | FS_ALLOW_IDMAP | FS_MGTIME
  │
  ├─ bd_type
  │    name          = "bdev"
  │    init_fs_context = bd_init_fs_context
  │    kill_sb       = kill_anon_super
  │
  ├─ proc_fs_type
  │    name          = "proc"
  │    init_fs_context = proc_init_fs_context
  │    kill_sb       = proc_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT | FS_DISALLOW_NOTIFY_PERM
  │
  ├─ cgroup_fs_type
  │    name          = "cgroup"
  │    init_fs_context = cgroup_init_fs_context
  │    kill_sb       = cgroup_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT
  │
  ├─ cgroup2_fs_type
  │    name          = "cgroup2"
  │    init_fs_context = cgroup_init_fs_context
  │    kill_sb       = cgroup_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT
  │
  ├─ dev_fs_type
  │    name          = "devtmpfs"
  │    init_fs_context = devtmpfs_init_fs_context
  │
  ├─ configfs_fs_type
  │    name          = "configfs"
  │    init_fs_context = configfs_init_fs_context
  │    kill_sb       = kill_litter_super
  │
  ├─ debug_fs_type
  │    name          = "debugfs"
  │    init_fs_context = debugfs_init_fs_context
  │    kill_sb       = kill_litter_super
  │
  ├─ fs_type (securityfs)
  │    name          = "securityfs"
  │    init_fs_context = securityfs_init_fs_context
  │    kill_sb       = kill_litter_super
  │
  ├─ sock_fs_type
  │    name          = "sockfs"
  │    init_fs_context = sockfs_init_fs_context
  │    kill_sb       = kill_anon_super
  │
  ├─ bpf_fs_type
  │    name          = "bpf"
  │    init_fs_context = bpf_init_fs_context
  │    kill_sb       = bpf_kill_super
  │    fs_flags      = FS_USERNS_MOUNT
  │
  ├─ pipe_fs_type
  │    name          = "pipefs"
  │    init_fs_context = pipefs_init_fs_context
  │    kill_sb       = kill_anon_super
  │
  ├─ ramfs_fs_type
  │    name          = "ramfs"
  │    init_fs_context = ramfs_init_fs_context
  │    kill_sb       = ramfs_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT
  │
  ├─ hugetlbfs_fs_type
  │    name          = "hugetlbfs"
  │    init_fs_context = hugetlbfs_init_fs_context
  │    kill_sb       = kill_litter_super
  │    fs_flags      = FS_ALLOW_IDMAP
  │
  ├─ rpc_pipe_fs_type
  │    name          = "rpc_pipefs"
  │    init_fs_context = rpc_init_fs_context
  │    kill_sb       = rpc_kill_sb
  │
  ├─ devpts_fs_type
  │    name          = "devpts"
  │    init_fs_context = devpts_init_fs_context
  │    kill_sb       = devpts_kill_sb
  │    fs_flags      = FS_USERNS_MOUNT
  │
  └─ ext4_fs_type
       name          = "ext4"
       init_fs_context = ext4_init_fs_context
       kill_sb       = ext4_kill_sb
       fs_flags      = FS_REQUIRES_DEV | FS_ALLOW_IDMAP | FS_MGTIME
       next          = NULL (链表尾)
```

---

## 3. 挂载树结构

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

## 4. 文件打开路径

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

## 5. 路径查找过程

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

## 6. 文件读写路径

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

## 7. 挂载过程

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
```