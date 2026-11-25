# cgroup / cgroup2 — 控制组文件系统

## 1. 概述与实现机制

cgroup 文件系统将进程分组层次结构暴露给用户空间，用于资源限制和监控。通过写入 cgroup 伪文件来配置 CPU/内存/IO/PID 等资源限制。基于 **kernfs** 框架构建。

### cgroup v1

- **每个控制器独立挂载**：`/sys/fs/cgroup/cpu/`, `/sys/fs/cgroup/memory/` 等
- **不同控制器可挂载在不同目录**，形成各自独立的层次树
- **进程可同时加入多个不同控制器的 cgroup**
- 存在**资源统计冲突**问题（如一个进程在 memory cgroup A 和 cpu cgroup B）

### cgroup v2

- **统一挂载**，所有控制器在同一层次结构中
- **线程模式 (threaded)**：支持进程内不同线程分属不同 cgroup
- **无内部进程 (no internal processes)**：非根 cgroup 不能有进程
- **子节点继承父节点的资源限制**

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  mkdir /sys/fs/cgroup/mygroup                                │
│  echo 50000 > /sys/fs/cgroup/mygroup/cpu.max                │
│  echo 1234 > /sys/fs/cgroup/mygroup/cgroup.procs            │
└────────────────────────┬─────────────────────────────────────┘
                         │ VFS 系统调用
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                cgroup 层 (kernel/cgroup/)                    │
│  cgroup_mkdir() → cgroup_create()                           │
│  cgroup_attach_task() → cgroup_attach_task_transfer()        │
│  cgroup_file_write() → 控制器回调 (cpu_cfs_write, mem_write) │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                kernfs 框架层 (fs/kernfs/)                    │
│  kernfs_create_root() → kernfs_create_dir()                 │
│  kernfs_create_file() → 文件读写回调                         │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 cgroup — 控制组

```c
// 文件: include/linux/cgroup-defs.h
struct cgroup {
    struct cgroup_subsys_state   self;       // 自身的控制组状态
    struct kernfs_node          *kn;         // 对应的 kernfs 节点
    struct cgroup               *parent;     // 父 cgroup
    struct list_head             children;   // 子 cgroup 列表
    struct list_head             cset_links; // 关联的 cgroup_set 链接
    struct list_head             release_list; // 释放列表
    struct list_head             pidlists;   // PID 列表
    struct cgroup_file           procs_file; // cgroup.procs 文件
    struct cgroup_file           events_file; // cgroup.events 文件
    struct cgroup_file           psi_files[NR_PSI_RESOURCES]; // PSI 文件
    u64                          serial_nr;  // 序列号
    struct cgroup_rstat_cpu __percpu *rstat_cpu; // 每个 CPU 的统计
    struct list_head             rstat_css_list; // 统计 CSS 列表
    struct cgroup_base_stat      bstat;      // 基础统计
    struct cgroup_bstat          bstat_last; // 上次统计
    struct cgroup_freezer_state  freezer;    // 冻结状态
    struct cgroup_pids           pids;       // PID 限制
    unsigned int                 flags;      // 标志位
    struct cgroup_psi            psi;        // PSI 压力信息
};
```

### 2.2 cgroup_subsys_state — 控制器状态

```c
// 文件: include/linux/cgroup-defs.h
struct cgroup_subsys_state {
    struct cgroup               *cgroup;    // 所属 cgroup
    struct cgroup_subsys        *ss;        // 所属控制器
    struct list_head             sibling;   // 兄弟节点链表
    struct list_head             children;  // 子节点链表
    int                          id;        // CSS ID
    unsigned int                 flags;     // 标志位
    atomic_t                     online_cnt; // 在线计数
    struct percpu_ref            refcnt;    // 引用计数
    struct work_struct           destroy_work; // 销毁工作
};
```

### 2.3 cgroup_subsys — 控制器

```c
// 文件: include/linux/cgroup-defs.h
struct cgroup_subsys {
    struct cgroup_subsys_state *(*css_alloc)(struct cgroup_subsys_state *parent_css); // 分配 CSS
    int (*css_online)(struct cgroup_subsys_state *css);  // CSS 上线
    void (*css_offline)(struct cgroup_subsys_state *css); // CSS 下线
    void (*css_released)(struct cgroup_subsys_state *css); // CSS 释放
    void (*css_free)(struct cgroup_subsys_state *css);   // CSS 释放
    int (*can_attach)(struct cgroup_taskset *tset);      // 是否可以附加
    void (*cancel_attach)(struct cgroup_taskset *tset);  // 取消附加
    void (*attach)(struct cgroup_taskset *tset);         // 附加
    int (*can_fork)(struct task_struct *task);           // 是否可以 fork
    void (*cancel_fork)(struct task_struct *task);       // 取消 fork
    void (*fork)(struct task_struct *task);              // fork
    void (*exit)(struct task_struct *task);              // 进程退出
    void (*release)(struct task_struct *task);           // 进程释放
    void (*bind)(struct cgroup_subsys_state *root_css);  // 绑定
    int early_init;                                      // 是否早期初始化
    int id;                                              // 控制器 ID
    const char *name;                                    // 控制器名称
    const char *legacy_name;                             // 旧版名称
    struct cgroup_subsys_state *root;                    // 根 CSS
    struct list_head cfts;                               // cgroup 文件表
    unsigned int depends_on;                             // 依赖的控制器
};
```

### 2.4 css_set — cgroup_set (进程关联)

```c
// 文件: include/linux/cgroup-defs.h
struct css_set {
    struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT]; // 每个控制器的 CSS 指针
    refcount_t                  refcount;                   // 引用计数
    struct list_head            tasks;                      // 当前 set 中的进程列表
    struct list_head            mg_tasks;                   // 迁移中的进程列表
    struct list_head            cgrp_links;                 // 到 cgroup 的链接
    struct cgroup               *dfl_cgrp;                  // v2 默认 cgroup
    struct list_head            task_iters;                 // 任务迭代器
    struct list_head            mg_preload_node;            // 迁移预加载节点
    struct list_head            mg_src_preload_node;        // 迁移源预加载节点
    struct mg_host              *mg_src_cgrp;               // 迁移源 cgroup
};
```

---

## 3. API 与使用方法

### 3.1 用户空间使用

```bash
# cgroup v2 (推荐)
# 创建 cgroup 并限制 CPU
mkdir -p /sys/fs/cgroup/mygroup
echo "+cpu +memory" > /sys/fs/cgroup/cgroup.subtree_control
echo "50000 100000" > /sys/fs/cgroup/mygroup/cpu.max    # 限制 50% CPU
echo "100M" > /sys/fs/cgroup/mygroup/memory.max          # 限制 100MB 内存
echo 1234 > /sys/fs/cgroup/mygroup/cgroup.procs          # 将 PID 1234 加入

# 查看 cgroup 统计
cat /sys/fs/cgroup/mygroup/memory.current
cat /sys/fs/cgroup/mygroup/cpu.stat

# cgroup v1 (旧版)
# 限制 CPU
mkdir -p /sys/fs/cgroup/cpu/mygroup
echo 50000 > /sys/fs/cgroup/cpu/mygroup/cpu.cfs_quota_us
echo 100000 > /sys/fs/cgroup/cpu/mygroup/cpu.cfs_period_us
echo 1234 > /sys/fs/cgroup/cpu/mygroup/tasks

# 限制内存
mkdir -p /sys/fs/cgroup/memory/mygroup
echo 104857600 > /sys/fs/cgroup/memory/mygroup/memory.limit_in_bytes
echo 1234 > /sys/fs/cgroup/memory/mygroup/tasks
```

### 3.2 内核内部 API

```c
#include <linux/cgroup.h>

// 初始化
int cgroup_init_early(void);
int cgroup_init(void);

// 创建 cgroup
int cgroup_mkdir(struct kernfs_node *parent_kn, const char *name, umode_t mode);

// 进程迁移
int cgroup_attach_task(struct cgroup *dst_cgrp, struct task_struct *leader,
                       bool threadgroup);
int cgroup_transfer_tasks(struct cgroup *to, struct cgroup *from);

// fork/exit 钩子
void cgroup_post_fork(struct task_struct *task, struct kernel_clone_args *args);
void cgroup_exit(struct task_struct *task);
void cgroup_release(struct task_struct *task);

// 冻结
int cgroup_freezer_freeze(struct cgroup *cgrp);
int cgroup_freezer_thaw(struct cgroup *cgrp);
```

### 3.3 控制器实现示例

```c
// 示例: 简单 CPU 控制器的实现框架
// 文件: kernel/sched/core.c

static struct cgroup_subsys_state *cpu_css_alloc(struct cgroup_subsys_state *parent_css)
{
    struct task_group *tg;
    tg = sched_create_group(parent ? parent_tg(parent_css) : NULL);
    if (IS_ERR(tg))
        return ERR_PTR(-ENOMEM);
    return &tg->css;
}

static void cpu_css_attach(struct cgroup_taskset *tset)
{
    struct task_struct *task;
    struct cgroup_subsys_state *css;

    cgroup_taskset_for_each(task, css, tset)
        sched_move_task(task);  // 将任务移动到新的调度组
}

struct cgroup_subsys cpu_cgrp_subsys = {
    .css_alloc      = cpu_css_alloc,
    .css_online     = cpu_css_online,
    .css_offline    = cpu_css_offline,
    .css_free       = cpu_css_free,
    .can_attach     = cpu_can_attach,
    .attach         = cpu_css_attach,
    .fork           = cpu_cgroup_fork,
    .exit           = cpu_cgroup_exit,
    .legacy_cftypes = cpu_files,
    .dfl_cftypes    = cpu_cftypes,
    .name           = "cpu",
    .early_init     = true,
};
```

---

## 4. 函数调用栈

### 4.1 cgroup 初始化

```
start_kernel()
  → cgroup_init_early()                       // 早期初始化
    → init_cgroup_root(&cgrp_dfl_root)          // 初始化默认根
    → cgroup_root->cgrp.self.cgroup = &cgrp_dfl_root->cgrp
    → for_each_subsys(ss, i)                    // 初始化控制器
      → ss->css_alloc(NULL)                     // 分配根 CSS
    → cgroup_init_subsys(ss, true)              // 初始化控制器子系统

  → cgroup_init()                               // 主要初始化
    → cgroup_setup_root(root, 0)                 // 设置根
      → kernfs_create_root()                     // 创建 kernfs 根
      → css_populate_dir(&root->cgrp.self)       // 创建默认文件
    → for_each_subsys(ss, i)                     // 初始化所有控制器
      → cgroup_init_subsys(ss, false)            // 初始化
    → register_filesystem(&cgroup_fs_type)       // 注册 cgroup 文件系统
```

### 4.2 创建 cgroup

```
mkdir /sys/fs/cgroup/mygroup
  ↓ sys_mkdir() → vfs_mkdir() → cgroup_mkdir()
    → cgroup_mkdir(parent_kn, name, mode)
      → cgroup_create(parent, name, mode)        // 核心创建逻辑
        → kzalloc(struct cgroup)                  // 分配 cgroup 结构
        → cgroup->parent = parent                 // 设置父 cgroup
        → cgroup_kn_set_live(cgroup)              // 设置 kernfs 节点
        → kernfs_create_dir(parent->kn, name, ...) // 创建 kernfs 目录
        → cgroup->kn = kn                          // 关联 kernfs 节点
        → for_each_subsys(ss, i)                   // 初始化每个控制器
          → css = ss->css_alloc(parent_css)        // 分配 CSS
          → cgroup->subsys[i] = css                // 关联 CSS
          → css_populate_dir(css)                  // 创建控制器文件
        → cgroup_apply_control(cgroup)             // 应用控制
        → kernfs_activate(cgroup->kn)              // 激活
        → cgroup_file_notify()                     // 通知文件变更
```

### 4.3 进程迁移

```
echo 1234 > /sys/fs/cgroup/mygroup/cgroup.procs
  ↓ cgroup_file_write() → cgroup_procs_write()
    → cgroup_attach_task(cgrp, task, threadgroup)
      → cgroup_attach_task_check(dst_cgrp, leader, threadgroup, tset)
        → for_each_subsys(ss, i)                // 检查每个控制器
          → ss->can_attach(tset)                // 是否允许迁移
      → cgroup_attach_lock(CGROUP_ATTACH_LOCK_EXCL, leader) // 加锁
      → cgroup_attach_task_transfer(dst_cgrp, tset)  // 实际迁移
        → cgroup_migrate_add_src(dst_cgrp, src_cset, tset) // 添加源
        → cgroup_migrate_prepare_dst(dst_cgrp, tset) // 准备目标
        → cgroup_migrate(dst_cgrp, tset)         // 执行迁移
          → for_each_mg_cgroup(src_cset, dst_cgrp)
            → cgroup_migrate_execute(src_cset, dst_cgrp, tset)
              → for_each_subsys(ss, i)
                → ss->attach(tset)               // 通知控制器
              → cgroup_task_migrate(to_cset, ...) // 迁移进程
                → css_set_move_task(task, ...)   // 移动任务
                → task->cgroups = to_cset        // 更新任务关联
      → cgroup_attach_unlock(CGROUP_ATTACH_LOCK_EXCL, leader) // 解锁
```

---

## 5. 流程图

### 5.1 cgroup v2 架构

```
/sys/fs/cgroup/ (根 cgroup)
│
├── cgroup.controllers          # 可用控制器列表 [cpu, memory, io, pids, ...]
├── cgroup.subtree_control      # 子 cgroup 的控制器 [+cpu +memory]
├── cgroup.procs                # 根 cgroup 中的进程
├── cpu.max                     # CPU 配额 (max usages period)
├── cpu.stat                    # CPU 统计
├── memory.max                  # 内存上限
├── memory.current              # 当前内存使用
├── pids.max                    # PID 数量限制
│
├── system.slice/               # 系统服务 cgroup
│   ├── cgroup.subtree_control
│   ├── sshd.service/
│   │   ├── cgroup.procs
│   │   ├── cpu.max
│   │   └── memory.max
│   ├── systemd-journald.service/
│   └── ...
│
├── user.slice/                 # 用户会话 cgroup
│   └── user-1000.slice/
│       └── session-1.scope/
│
└── mygroup/                    # 用户创建的 cgroup
    ├── cgroup.procs            # 包含的进程
    ├── cgroup.subtree_control
    ├── cpu.max                 # 50% CPU
    ├── cpu.stat
    ├── memory.max              # 100MB
    └── memory.current
```

### 5.2 进程资源限制流程

```
进程 P 尝试分配内存
    │
    ▼
do_anonymous_page() / do_fault()
    │
    ▼
charge_memcg(objcg, pages)
    │
    ▼
mem_cgroup_charge()
    │
    ├── try_charge()                    # 尝试扣费
    │     │
    │     ├── page_counter_try_charge()  # 检查是否超过 memory.max
    │     │     │
    │     │     ├── 未超限 → 成功
    │     │     │
    │     │     └── 超限 → 返回 -ENOMEM
    │     │           │
    │     │           ├── 内存回收尝试
    │     │           │     → mem_cgroup_reclaim()
    │     │           │
    │     │           └── OOM 检查
    │     │                 → mem_cgroup_oom()
    │     │
    │     └── 成功 → page_counter_charge() 更新计数
    │
    └── 分配物理内存, 更新统计
```

---

## 6. 使用场景

| 场景 | 控制器 | 描述 |
|------|--------|------|
| **CPU 限制** | cpu | 限制进程组的 CPU 使用率（配额/周期） |
| **内存限制** | memory | 限制进程组的内存使用量，防止 OOM |
| **IO 限制** | io | 限制进程组的磁盘读写带宽和 IOPS |
| **PID 限制** | pids | 限制进程组可创建的 PID 数，防止 fork 炸弹 |
| **CPUSET** | cpuset | 将进程绑定到特定 CPU 和内存节点 |
| **冷冻** | freezer | 暂停/恢复进程组（容器暂停） |
| **hugetlb** | hugetlb | 限制大页内存使用 |
| **rdma** | rdma | 限制 RDMA 资源使用 |
| **psi** | - | 压力延迟信息监控 |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `kernel/cgroup/cgroup.c` | cgroup 核心实现（创建、挂载、进程迁移） |
| `kernel/cgroup/cgroup-v1.c` | cgroup v1 兼容实现 |
| `kernel/cgroup/freezer.c` | freezer 控制器（进程冻结） |
| `kernel/cgroup/legacy_freezer.c` | v1 freezer 控制器 |
| `kernel/cgroup/rdma.c` | RDMA 控制器 |
| `kernel/cgroup/cpuset.c` | CPUSET 控制器 |
| `kernel/cgroup/pids.c` | PID 控制器 |
| `kernel/cgroup/misc.c` | 杂项控制器 |
| `kernel/sched/core.c` | CPU 调度控制器实现 |
| `mm/memcontrol.c` | 内存控制器实现 |
| `block/blk-cgroup.c` | IO 控制器实现 |
| `include/linux/cgroup-defs.h` | 核心数据结构定义 |
| `include/linux/cgroup.h` | 对外 API 头文件 |