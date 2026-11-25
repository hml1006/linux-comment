# kernfs — 伪文件系统框架

## 1. 定位与设计

### 1.1 kernfs 是什么

kernfs 是内核内部的伪文件系统框架，头文件注释点明目标：

```c
// include/linux/kernfs.h:1-4
/* kernfs.h - pseudo filesystem decoupled from vfs locking */
```

它向 VFS 提供一棵"像文件系统"的树：inode、dentry、`file_operations`、`inode_operations` 全由 kernfs 实现；但条目不是磁盘数据，而是使用者（sysfs、cgroup、resctrl）运行时创建/销毁的 `kernfs_node`。kernfs 把节点映射成 VFS 对象，并把 `read`/`write`/`mmap`/`poll` 交给使用者的 `struct kernfs_ops`。

[sysfs 篇](../special/02-sysfs.md) 从 kobject 视角看 `/sys`；本篇从 kernfs 自身视角讲节点、锁、VFS 对接与文件 I/O，不重复属性组、uevent。

### 1.2 它解决什么问题

| 问题 | 传统做法 | kernfs 的做法 |
|------|----------|----------------|
| 节点生命周期与 VFS 不同步 | 自维护 dirent 树再对齐 inode/dentry | `kernfs_node` 是唯一事实来源，inode 按需创建并持节点引用 |
| VFS 锁粒度太粗 | 依赖 `s_vfs_rename_mutex`、inode 锁 | 按 root 的 `kernfs_rwsem`；VFS 侧只做重建式 lookup/revalidate |
| 一棵内核树要多命名空间视图 | 复制目录树 | 节点带 `ns`，查找与 readdir 按 `sb` 的 ns 过滤 |
| 各家重复实现文件 I/O | 各写一套 file_operations | 统一 `kernfs_ops`：seq_file 与裸 read/write |

关键取舍：**删除只是把节点从兄弟树摘下并置为不可见**，inode/dentry 可能仍在 dcache；内存释放推迟到最后一个 `count` 消失后经 RCU。并发模型因此分成两条正交的线：`count` 保对象存活，`active` 保内容可用。

### 1.3 与 sysfs、cgroup、resctrl、configfs 的关系

本树只有三处调用 `kernfs_create_root()`：

```c
// fs/sysfs/mount.c:91
sysfs_root = kernfs_create_root(NULL, KERNFS_ROOT_EXTRA_OPEN_PERM_CHECK, NULL);
// kernel/cgroup/cgroup.c:2182
root->kf_root = kernfs_create_root(kf_sops,
        KERNFS_ROOT_CREATE_DEACTIVATED | KERNFS_ROOT_SUPPORT_EXPORTOP |
        KERNFS_ROOT_SUPPORT_USER_XATTR | KERNFS_ROOT_INVARIANT_PARENT,
        root_cgrp);
```

第三处是 `fs/resctrl/rdtgroup.c:4286`，flag 为 `CREATE_DEACTIVATED | EXTRA_OPEN_PERM_CHECK`。sysfs 的 `scops` 为 `NULL`，用户不能 `mkdir`；cgroup 与 resctrl 提供私有 `kernfs_syscall_ops`。三者都以 `kernfs_get_tree()` 为挂载入口。**configfs 不是 kernfs 用户**：它有独立的 `configfs_dirent`/inode/删除语义，常被并列讨论但无实现继承。

### 1.4 分层

```
用户空间：ls /sys/fs/cgroup、cat /sys/block/sda/size
                    │
                    ▼
┌──────────────────────────────────────────────────────┐
│ VFS：路径解析 / dcache / inode / file                │
└──────────────────┬───────────────────────────────────┘
                   │ i_fop / i_op / d_op
                   ▼
┌──────────────────────────────────────────────────────┐
│ kernfs：kernfs_node 树 + VFS 胶水                    │
│  dir.c lookup/mkdir/rmdir/rename/readdir             │
│  file.c I/O    inode.c iattr    mount.c super_block  │
│  symlink.c get_link                                  │
└──────────────────┬───────────────────────────────────┘
                   │ struct kernfs_ops
                   ▼
┌──────────────────────────────────────────────────────┐
│ 使用者：sysfs(kobject) / cgroup(cftype) / resctrl     │
└──────────────────────────────────────────────────────┘
```

kernfs **不是文件系统类型**：头文件里没有 `file_system_type`，`sb->s_type` 仍是 sysfs/cgroup/cgroup2/resctrl，magic 与 `mount -t` 名字由使用者决定。

### 1.5 本树文件构成

| 文件 | 行数 | 内容 |
|------|------|------|
| `fs/kernfs/kernfs-internal.h` | 236 | `kernfs_root`/`kernfs_super_info`/`kernfs_iattrs`（私有） |
| `fs/kernfs/dir.c` | 2015 | 层级、生命周期、查找、readdir、rename |
| `fs/kernfs/file.c` | 1085 | open、seq/裸 I/O、mmap/poll、notify |
| `fs/kernfs/inode.c` | 406 | inode 初始化、iattr、xattr |
| `fs/kernfs/mount.c` | 472 | super_block、exportfs、slab 初始化 |
| `fs/kernfs/symlink.c` | 155 | 符号链接、相对路径计算 |

本树**没有** `fs/kernfs/sysfs.c`。上游较早版本用它承载 kernfs 与 sysfs 之间的辅助接口（把每个 kernfs 超级块的调试信息以 sysfs 目录形式暴露、并在挂载/卸载时登记）。本树已无此文件，也没有任何 `kernfs_sysfs_*` 符号；"每个超级块向根登记"的职责改由 `mount.c` 直接用 `root->supers` 链表完成：`kernfs_get_tree()` 中 `list_add()`（mount.c:404），`kernfs_kill_sb()` 中 `list_del()`（mount.c:433）。因此 kernfs 自身不再向 sysfs 暴露调试目录，`/sys/fs/kernfs` 不存在。

---

## 2. 核心数据结构

### 2.1 结构体归属

| 结构体 | 定义 | 可见性 | 作用 |
|--------|------|--------|------|
| `kernfs_node` | kernfs.h:199 | 公开 | 树节点，inode/dirent 蓝本 |
| `kernfs_elem_dir` | kernfs.h:162 | 公开 | 目录数据（子树、root、rev） |
| `kernfs_elem_symlink` | kernfs.h:179 | 公开 | 链接数据（target_kn） |
| `kernfs_elem_attr` | kernfs.h:183 | 公开 | 文件数据（ops、open、size） |
| `kernfs_syscall_ops` | kernfs.h:246 | 公开 | 使用者的 mkdir/rmdir/rename |
| `kernfs_open_file` | kernfs.h:260 | 公开 | 每次 open 一份的上下文 |
| `kernfs_ops` | kernfs.h:280 | 公开 | 属性文件回调集合 |
| `kernfs_fs_context` | kernfs.h:336 | 公开 | 挂载参数 |
| `kernfs_root` | kernfs-internal.h:33 | **私有** | 层级根：ino_idr 与锁 |
| `kernfs_super_info` | kernfs-internal.h:86 | **私有** | `sb->s_fs_info` |
| `kernfs_iattrs` | kernfs-internal.h:22 | **私有** | uid/gid/时间戳/xattr |
| `kernfs_open_node` | file.c:21 | **私有** | 节点级 poll/event 状态 |

`kernfs_root` 私有是因为含 ino 分配器与锁；`kernfs_node` 必须公开，因为使用者要把对象挂到 `priv` 并在回调里反查。

### 2.2 kernfs_node

```c
// include/linux/kernfs.h:199（节选）
struct kernfs_node {
    atomic_t        count;
    atomic_t        active;
    struct kernfs_node __rcu *__parent;
    const char      __rcu *name;
    struct rb_node  rb;
    unsigned int    hash;      /* ns + name hash */
    unsigned short  flags;
    union {
        struct kernfs_elem_dir      dir;
        struct kernfs_elem_symlink  symlink;
        struct kernfs_elem_attr     attr;
    };
    u64             id;
    void            *priv;
    struct kernfs_iattrs *iattr;
};
```

（为节省篇幅省略了 `ns`、`mode`、`dep_map`、`rcu` 等字段；`ns` 是命名空间标签，`dep_map` 用于 lockdep，`rcu` 用于回收。）

- `__parent`/`name` 是 `__rcu` 指针，rename 可替换；头文件要求用 `kernfs_get_parent()`/`kernfs_name()` 而非直接读。
- `rb` 既表示"在父目录红黑树中的位置"，也表示"是否已链接"：`RB_EMPTY_NODE(&kn->rb)` 是删除路径的关键判据。
- `hash` 是 `ns_id + name` 的 31 位哈希；兄弟树排序键 `(hash, ns_id, name)`，同父目录可并存同名不同 ns 的节点。
- `id` 64 位唯一：64 位 ino 时即 inode 号；32 位 ino 时低 32 位 ino、高 32 位 generation（`kernfs_id_ino()`/`kernfs_id_gen()`）。
- `priv` 是使用者钩子；`iattr` 惰性分配。

### 2.3 三种 elem 与 root 反查

```c
// include/linux/kernfs.h:162,179,183（节选）
struct kernfs_elem_dir {
    unsigned long       subdirs;      /* 子目录计数，用于 nlink */
    struct rb_root      children;     /* 子节点，经 kn->rb 挂入 */
    struct kernfs_root  *root;        /* 放这里为省空间 */
    unsigned long       rev;          /* 目录变更版本号 */
};
struct kernfs_elem_symlink { struct kernfs_node *target_kn; };
struct kernfs_elem_attr {
    const struct kernfs_ops *ops;
    struct kernfs_open_node __rcu *open;
    loff_t              size;
    struct kernfs_node  *notify_next; /* notify 侵入式链表 */
};
```

`dir.root` 只是"某处存了 root"，取 root 时不看自己而看父节点：

```c
// fs/kernfs/kernfs-internal.h:72
static inline struct kernfs_root *kernfs_root(const struct kernfs_node *kn)
{
    const struct kernfs_node *knp;
    /* if parent exists, it's always a dir; otherwise, @sd is a dir */
    guard(rcu)();
    knp = rcu_dereference(kn->__parent);
    if (knp)
        kn = knp;
    return kn->dir.root;
}
```

因此只有根节点的 `dir.root` 指向自己。

### 2.4 引用计数与 active

```c
// include/linux/kernfs.h:195-197
 * As long as count reference is held, the kernfs_node itself is
 * accessible.  Dereferencing elem or any other outer entity requires
 * active reference.
```

| 计数 | 语义 | 获取 | 释放 | 失败 |
|------|------|------|------|------|
| `count` | 节点对象存活 | `kernfs_get()` | `kernfs_put()` | 不失败 |
| `active` | 节点可用（已激活未移除） | `kernfs_get_active()` | `kernfs_put_active()` | 已 deactivate 返回 NULL |

`active` 用负值表示 deactivated：

```c
// fs/kernfs/kernfs-internal.h:61-62
/* +1 to avoid triggering overflow warning when negating it */
#define KN_DEACTIVATED_BIAS      (INT_MIN + 1)
```

新建时 `active = KN_DEACTIVATED_BIAS`；激活即 `atomic_sub(KN_DEACTIVATED_BIAS, &kn->active)`（dir.c:1441），删除时加回。好处：`kernfs_get_active()` 用 `atomic_inc_unless_negative()` 一步"判断+加引用"，与 deactivate 无竞争（dir.c:467）；`kernfs_put_active()` 递减后等于 bias 就唤醒 `deactivate_waitq`（dir.c:491-495），正是 `kernfs_drain()` 等的信号；`count` 与 `active` 解耦，节点可以"活着但不可用"，也可以"count 归零但 RCU 未回收"。

### 2.5 kernfs_root

```c
// fs/kernfs/kernfs-internal.h:33（节选）
struct kernfs_root {
    struct kernfs_node     *kn;
    unsigned int           flags;
    struct idr             ino_idr;            /* inode 号分配 */
    u32                    id_highbits;        /* 32 位 ino 的 generation */
    struct kernfs_syscall_ops *syscall_ops;
    struct list_head       supers;             /* 挂载此树的 sb 列表 */
    wait_queue_head_t      deactivate_waitq;
    struct rw_semaphore    kernfs_rwsem;
    struct rw_semaphore    kernfs_iattr_rwsem;
    struct rw_semaphore    kernfs_supers_rwsem;
    rwlock_t               kernfs_rename_lock; /* __parent / name */
};
```

- `ino_idr` 既分配 inode 号，也支持 `kernfs_find_and_get_node_by_id()` 反查（exportfs 依赖）。
- `supers` 记录"被哪些超级块挂载"；同一节点可能在多个 sb 各有一个 inode，所以 xattr 不能靠 inode 锁，`kernfs_clear_inode_nlink()` 与 notify work 都要遍历它。
- 锁层次 `supers_rwsem` → `kernfs_rwsem` → `iattr_rwsem`；`rename_lock` 独立，只保护指针替换。

### 2.6 kernfs_super_info 与 kernfs_iattrs

```c
// fs/kernfs/kernfs-internal.h:86,22（节选）
struct kernfs_super_info {
    struct super_block     *sb;
    struct kernfs_root     *root;
    const struct ns_common *ns;
    struct list_head       node;   /* 挂在 root->supers */
};
#define kernfs_info(SB) ((struct kernfs_super_info *)(SB->s_fs_info))

struct kernfs_iattrs {
    kuid_t ia_uid;  kgid_t ia_gid;
    struct timespec64 ia_atime, ia_mtime, ia_ctime;
    struct list_head xattrs;
    struct simple_xattr_limits xattr_limits;
};
```

`sb` 身份由 `(root, ns)` 决定：`return sb_info->root == info->root && sb_info->ns == info->ns;`（mount.c:331）。同一 root 挂多次复用同一 sb；不同 netns 得到两个 sb，共享同一批节点——"一个 `kn` 对应多个 inode"的由来。`iattrs` 默认 `NULL`（uid/gid 视为 root），首次 `chown`/`utimes`/`setxattr` 或创建时传入非 root uid/gid 才分配，分配用 `try_cmpxchg()` 乐观锁（inode.c:52-53）。

### 2.7 kernfs_open_file 与 kernfs_ops

```c
// include/linux/kernfs.h:260（节选）
struct kernfs_open_file {
    struct kernfs_node *kn;
    struct file        *file;
    struct seq_file    *seq_file;
    void               *priv;
    struct mutex       mutex;          /* 同一打开文件的 ops 串行 */
    struct mutex       prealloc_mutex;
    int                event;          /* 读时的 on->event 快照 */
    struct list_head   list;           /* 挂入 on->files */
    char               *prealloc_buf;
    size_t             atomic_write_len;
    bool               mmapped:1;
    bool               released:1;
    const struct vm_operations_struct *vm_ops;
};
```

| 回调 | 触发 | 说明 |
|------|------|------|
| `open(of)` | `kernfs_fop_open()` 尾部 | `of->seq_file` 已就绪，无人访问 `of` |
| `release(of)` | close 或 drain | 保证恰好一次（`of->released`） |
| `seq_show(sf,v)` | `seq_read_iter()` | 存在即走 seq 路径，缺省等价单次输出 |
| `seq_start/next/stop` | seq 迭代 | 不实现则 `single_start` / `++*ppos` |
| `read(of,buf,n,off)` | 裸读 | 经内核缓冲，单次 ≤ `PAGE_SIZE` |
| `write(of,buf,n,off)` | 写 | `buf` 已补 `'\0'` |
| `poll(of,pt)` | `poll(2)` | 缺省 `kernfs_generic_poll()` |
| `mmap(of,vma)` | `mmap(2)` | 需 `KERNFS_HAS_MMAP` |
| `llseek(of,off,w)` | `kernfs_fop_llseek()` | 缺省 `generic_file_llseek()` |

`kernfs_ops` 还有 `atomic_write_len`（原子写上限）与 `prealloc` 两个数据字段，open 时被缓存/校验，因为写路径要在不持 active 引用时就知道上限（file.c:668-672）。

---

## 3. 节点生命周期

### 3.1 全景

```
create_root/create_dir/create_file/create_link
        ↓
__kernfs_new_node()   分配 kn、ino、name，填 elem
        ↓
kernfs_add_one()      写锁检查 ns / 父类型 / REMOVING
        ↓
kernfs_link_sibling() 插入父 rb 树，subdirs++，rev++
        ↓
kernfs_activate()     后序遍历，atomic_sub(KN_DEACTIVATED_BIAS)
        ↓
     节点可见
        ↓
kernfs_remove() / kernfs_remove_by_name_ns() / kernfs_remove_self()
        ↓
__kernfs_remove()     标记 REMOVING + deactivate 子树
        │            逐个 kernfs_drain()：等 active 归零、清 mmap、调 release
        ▼
unlink_sibling() → kernfs_put() 到 count==0 → call_rcu → 释放
```

### 3.2 创建根与分配节点

`kernfs_create_root()` 初始化锁与 idr，再 `__kernfs_new_node(root, NULL, "", ...)` 造一个无父、空名的根节点，最后视 flag 决定是否立刻 `kernfs_activate(kn)`（dir.c:1017-1067）。`kernfs_name()` 对根返回 `"/"`（dir.c:203）。`kernfs_destroy_root()` 先 `kernfs_get(root->kn)`、再 `kernfs_remove()`、最后 `kernfs_put()`；最后一次 put 会 `idr_destroy()` + `kfree_rcu(root)`（dir.c:624-628），调用者不能自己 `kfree(root)`。

```c
// fs/kernfs/dir.c:665-694（节选）
kn = kmem_cache_zalloc(kernfs_node_cache, GFP_KERNEL);

idr_preload(GFP_KERNEL);
spin_lock(&root->kernfs_idr_lock);
ret = idr_alloc_cyclic(&root->ino_idr, kn, 1, 0, GFP_ATOMIC);
if (ret >= 0 && ret < root->last_id_lowbits)
    root->id_highbits++;
id_highbits = root->id_highbits;
root->last_id_lowbits = ret;
spin_unlock(&root->kernfs_idr_lock);
idr_preload_end();

kn->id = (u64)id_highbits << 32 | ret;
atomic_set(&kn->count, 1);
atomic_set(&kn->active, KN_DEACTIVATED_BIAS);
RB_CLEAR_NODE(&kn->rb);
```

循环分配 + `id_highbits` 自增实现 32 位 ino 的 generation：低 32 位回绕后高 32 位递增，老 fhandle 失效（`kernfs_find_and_get_node_by_id()` 中 gen 为 0 表示通配）。非 root uid/gid 立刻转成 `iattr`，失败回滚 idr。有父时先 `kernfs_get(parent)`、`rcu_assign_pointer(kn->__parent, parent)`，**再**调 `security_kernfs_init_security(parent, kn)`（dir.c:703-710）——这是 7.2 的 "link kn to its parent before the LSM init hook"：LSM 可能沿父链取标签，父指针必须已有效。`kernfs_new_node()` 另实现 setgid 继承（dir.c:739-749）。

### 3.3 挂入父目录

```c
// fs/kernfs/dir.c:825-846（节选）
down_write(&root->kernfs_rwsem);
parent = kernfs_parent(kn);

has_ns = kernfs_ns_enabled(parent);
if (WARN(has_ns != (bool)kn->ns, KERN_WARNING "kernfs: ns %s in '%s' for '%s'\n", ...))
    goto out_unlock;
if (kernfs_type(parent) != KERNFS_DIR)
    goto out_unlock;
if (parent->flags & (KERNFS_REMOVING | KERNFS_EMPTY_DIR))
    goto out_unlock;

kn->hash = kernfs_name_hash(kernfs_rcu_name(kn), kn->ns);
ret = kernfs_link_sibling(kn);
```

三条不变量：**ns 一致**（父开 `KERNFS_NS` 则子必须带 ns，反之必须不带，违反即 `WARN` + `-EINVAL`）；`KERNFS_EMPTY_DIR`（挂载点目录）下不能建节点；激活在释放写锁后完成（`kernfs_add_one()` 末尾视根 flag 调 `kernfs_activate()`，dir.c:867-868），因为 `kernfs_activate()` 要重新取写锁。

`kernfs_link_sibling()` 是纯红黑树插入：按 `kernfs_sd_compare()` 逐层比较，相等返回 `-EEXIST`；成功后 `rb_link_node()`/`rb_insert_color()`，再在 `kernfs_iattr_rwsem` 下 `subdirs++` 与 `kernfs_inc_rev(parent)`（dir.c:382-418）。`-EEXIST` 是最常见错误：sysfs 会翻成 `sysfs_warn_dup()` 的 "cannot create duplicate filename" 并 `dump_stack()`（fs/sysfs/dir.c:21-33）。

### 3.4 激活与隐藏

```c
// fs/kernfs/dir.c:1429（节选）
static void kernfs_activate_one(struct kernfs_node *kn)
{
    lockdep_assert_held_write(&kernfs_root(kn)->kernfs_rwsem);
    kn->flags |= KERNFS_ACTIVATED;
    if (kernfs_active(kn) || (kn->flags & (KERNFS_HIDDEN | KERNFS_REMOVING)))
        return;
    WARN_ON_ONCE(atomic_read(&kn->active) != KN_DEACTIVATED_BIAS);
    atomic_sub(KN_DEACTIVATED_BIAS, &kn->active);
}
```

`kernfs_activate()` 用后序遍历（`kernfs_next_descendant_post()`，dir.c:1405）对整棵子树逐节点调用它，因此一次调用即可让一批已建好的节点原子地一起可见；函数内另有 `WARN_ON_ONCE(... RB_EMPTY_NODE(&kn->rb))` 检查尚未链接的节点。`kernfs_show(kn, false)` 是临时隐藏：deactivate 并 drain 但保留在树上；`kernfs_show(kn, true)` 时若曾 `ACTIVATED` 就重新激活；目录不支持隐藏（dir.c:1487）。

### 3.5 删除与 drain

```c
// fs/kernfs/dir.c:1557-1565（节选）
/* prevent new usage by marking all nodes removing and deactivating */
down_write(&kernfs_root(kn)->kernfs_iattr_rwsem);
pos = NULL;
while ((pos = kernfs_next_descendant_post(pos, kn))) {
    pos->flags |= KERNFS_REMOVING;
    if (kernfs_active(pos))
        atomic_add(KN_DEACTIVATED_BIAS, &pos->active);
}
up_write(&kernfs_root(kn)->kernfs_iattr_rwsem);
```

第一遍"标记+关门"：整棵子树打 `KERNFS_REMOVING` 并加回 bias，新的 `kernfs_get_active()` 立刻失败。第二遍从 `kernfs_leftmost_descendant()` 开始，逐个 `kernfs_get(pos)` 保活、`kernfs_drain(pos, true)`、`kernfs_unlink_sibling()`，只有成功摘除者负责 `kernfs_clear_inode_nlink()` 与时间戳更新，最后 `kernfs_put(pos)`（dir.c:1567-1604）。

`kernfs_drain()` 是核心（dir.c:508-551）：先 `WARN_ON_ONCE(kernfs_active(kn))` 确认节点已不可用（active 为负）；若"恰好等于 bias 且无需清打开文件"直接返回（错误路径可无脑 remove）；否则 `up_write(kernfs_rwsem)`（并按 `drop_supers` 放掉 `supers_rwsem`），`wait_event(root->deactivate_waitq, active == bias)`，需要时 `kernfs_drain_open_files()`，最后重新取锁返回。必须"放锁等待、返回前恢复"：否则持 active 引用的线程拿不到 `kernfs_rwsem`，永远无法 `put_active`；等待期间 `kn` 可能被 put 到 0，所以调用方预留引用。

### 3.6 释放与 RCU

```c
// fs/kernfs/dir.c:590-618（节选）
if (!kn || !atomic_dec_and_test(&kn->count))
    return;
root = kernfs_root(kn);
 repeat:
parent = kernfs_parent(kn);
if (kernfs_type(kn) == KERNFS_LINK)
    kernfs_put(kn->symlink.target_kn);
if (kn->iattr)
    simple_xattrs_free(&root->xa_cache, &kn->iattr->xattrs, NULL);

spin_lock(&root->kernfs_idr_lock);
idr_remove(&root->ino_idr, (u32)kernfs_ino(kn));
spin_unlock(&root->kernfs_idr_lock);
call_rcu(&kn->rcu, kernfs_free_rcu);
```

子节点持有父节点的 `count`（`__kernfs_new_node()` 中 `kernfs_get(parent)`），故 `kn = parent; if (atomic_dec_and_test(&kn->count)) goto repeat;` 会级联释放到根，根节点的 put 再 `idr_destroy()` + `kfree_rcu(root)`（dir.c:620-629）。符号链接持 target 引用；xattr 元素来自 `root->xa_cache`，必须在外层释放；节点、名字、iattr 都在 `kernfs_free_rcu()` 统一回收。`WARN_ONCE` 专抓"get_active 后忘 put_active"或"强行释放仍在使用的节点"。

### 3.7 rename

`kernfs_rename_ns()`（dir.c:1819-1904）的流程是"先摘、改指针、再插"：拒绝根节点与 `INVARIANT_PARENT` 下的换父（`WARN_ON_ONCE(old_parent != new_parent)`）；目标父不可用或重名则 `-ENOENT`/`-EEXIST`；`kernfs_unlink_sibling()` 后，若换父则 `kernfs_get(new_parent)` 并在 `kernfs_rename_lock` 写锁下 `rcu_assign_pointer(kn->__parent, ...)`，同时更新 `ns` 与（若改名）`name`；同父改名则只换 `name`（RCU 保护）。随后 `kn->hash = kernfs_name_hash(...)` 并 `kernfs_link_sibling(kn)` 重新插入；旧名若动态分配且非 rodata 则 `kfree_rcu_mightsleep()`。

`INVARIANT_PARENT` 的保证是 `kernfs_node_dentry()` 与 cgroup 无锁读 `__parent` 的前提；hash 必须在写锁内重算并重插，否则兄弟树排序失效。

---

## 4. kernfs_ops 与文件 I/O

### 4.1 open

```c
// fs/kernfs/file.c:617-619,672（节选）
has_read = ops->seq_show || ops->read || ops->mmap;
has_write = ops->write || ops->mmap;
has_mmap = ops->mmap;
...
of->atomic_write_len = ops->atomic_write_len;
```

`kernfs_fop_open()`（file.c:603-736）先 `kernfs_get_active(kn)` 取不到就 `-ENODEV`；若根带 `EXTRA_OPEN_PERM_CHECK`，则即使有 `CAP_DAC_OVERRIDE`，模式里完全没有 `S_IRUGO`/`S_IWUGO` 或对应回调未实现也返回 `-EACCES`；接着分配 `kernfs_open_file`、校验 `prealloc` 与 `seq_show` 不共存、`prealloc` 时预分配 `atomic_write_len ?: PAGE_SIZE` 字节、`seq_open()`、`kernfs_get_open_node()`，最后在 active 引用仍持有时调 `ops->open(of)`。

**不论是否走 seq 都 `seq_open()`**：注释说明是为统一 `private_data` 访问，且可读普通文件占多数。于是 `file->private_data` 总是 `seq_file`，`kernfs_of()` 即 `((struct seq_file *)file->private_data)->private`（file.c:108）。open 尾部还会补回被 `seq_file` 清掉的 `FMODE_PWRITE`。本树 `mutex_init(&of->mutex)` 的三分支（has_mmap/可写/只读）代码完全相同，注释保留的是"给三类文件不同 lockdep key"的原意。

### 4.2 数据结构关系

```
struct file
 ├─ f_path.dentry → dentry → inode → i_private = kernfs_node
 ├─ f_op = kernfs_file_fops
 └─ private_data → seq_file → private → kernfs_open_file
                                            ├─ kn / file / seq_file
                                            ├─ list ──┐
                                            └─ event  │
                                                      ▼
                    kn->attr.open → kernfs_open_node
                                      ├─ event（原子计数）
                                      ├─ poll（等待队列）
                                      ├─ files（所有 of 的链表）
                                      ├─ nr_mmapped
                                      └─ nr_to_release
```

`kernfs_open_node` **按节点**分配：N 次 open 只有一个 `on`，N 个 `of` 挂在 `on->files`。`nr_to_release` 记录"还有几个 release 未调"，drain 据此决定是否遍历。

### 4.3 seq 读

```
read(2) → kernfs_fop_read_iter
  ├─ KERNFS_HAS_SEQ_SHOW → seq_read_iter → kernfs_seq_ops
  │    start → kernfs_seq_start: lock(of->mutex) → get_active → seq_start/single_start
  │    show  → kernfs_seq_show: of->event = on->event → ops->seq_show
  │    next  → kernfs_seq_next（无 seq_next 则 ++*ppos, NULL）
  │    stop  → kernfs_seq_stop → seq_stop + put_active → unlock
  └─ 否则 → kernfs_file_read_iter
```

`of->mutex` 在 active 引用**外层**，目的只是"同一打开文件的 ops 不并发"：

```c
// fs/kernfs/file.c:161-167
/*
 * @of->mutex nests outside active ref and is primarily to ensure that
 * the ops aren't called concurrently for the same open file.
 */
mutex_lock(&of->mutex);
if (!kernfs_get_active_of(of))
    return ERR_PTR(-ENODEV);
```

`ERR_PTR(-ENODEV)` 是特殊标记，用来区分"get_active 失败"与"自定义 `seq_start` 返回 `ERR_PTR(-ENODEV)`"。file.c:124-145 解释了绕法：`kernfs_seq_stop()` 见到该值就跳过 `kernfs_seq_stop_active()`，因为 start 失败时已直接调用过。

### 4.4 裸读与写

裸读（file.c:232-285）先用 `min_t(size_t, iov_iter_count(iter), PAGE_SIZE)` 定长，取 `of->prealloc_buf` 或临时 `kmalloc`；随后 `mutex_lock(&of->mutex)`、`kernfs_get_active_of()`、记录 `of->event`、调 `ops->read()`，再 `put_active` 并解锁，最后 `copy_to_iter()` 并推进 `ki_pos`。裸读存在的理由（file.c:226-231）：bin 文件读取可能有副作用，`read(2)` 的精确 offset/长度必须原样传回调，seq_file 做不到。

写路径（file.c:304-361）先判 `atomic_write_len`：超长 `-E2BIG`，否则 `PAGE_SIZE` 分片；`buf = kmalloc(len + 1)` 后 `copy_from_iter()`，并 `buf[len] = '\0'` 给 `store()` 准备 C 字符串；然后取 active 引用调 `ops->write()`。它是"整块写"（file.c:294-302 注释明确不支持部分写）：用户应"读整个文件、改值、再整体写回"。`copy_from_iter()` 放在 active 引用之前以缩短临界区；cgroup 控制文件都设 `atomic_write_len = PAGE_SIZE`，保证一次 `write(2)` 不被拆开。

### 4.5 mmap

`kernfs_fop_mmap()`（file.c:450-509）先靠 `KERNFS_HAS_MMAP` 判断支持性，刻意不拿 `of->mutex`，避免 mmap 路径与 `of->mutex` 之间产生 lockdep 假阳性。拿到锁与 active 引用后调 `ops->mmap()`，再检查：`of->mmapped` 时 `vm_ops` 必须一致、`vma->vm_ops->close` 不允许（无法包装）。首次映射置 `of->mmapped = true`、`of_on(of)->nr_mmapped++`、记下 `of->vm_ops`，最后 `vma->vm_ops = &kernfs_vm_ops`。此后 `fault`/`page_mkwrite`/`access`/`open` 都先取 active 引用再转发给使用者的 `of->vm_ops`（file.c:363-448）。

### 4.6 poll 与 notify

```c
// fs/kernfs/file.c:848
__poll_t kernfs_generic_poll(struct kernfs_open_file *of, poll_table *wait)
{
    struct kernfs_open_node *on = of_on(of);

    poll_wait(of->file, &on->poll, wait);
    if (of->event != atomic_read(&on->event))
        return DEFAULT_POLLMASK|EPOLLERR|EPOLLPRI;
    return DEFAULT_POLLMASK;
}
```

约定是"读一次拿快照，再 poll 等变化"（file.c:834-847）：`kernfs_fop_poll()` 先取 active 引用，有 `ops->poll` 就调它、否则调 `kernfs_generic_poll()`；读路径把当前 `on->event` 记进 `of->event`；`kernfs_notify()` 让 `on->event++` 并唤醒，poll 返回 `EPOLLERR|EPOLLPRI`，用户需重新 read 或 `lseek(0)` 再读。

`kernfs_notify()`（file.c:978-1006）做两件事：在 RCU 下 `atomic_inc(&on->event)` 并 `wake_up_interruptible()`（立即唤醒 poll）；再用 `kernfs_notify_lock` 把节点压入以 `notify_next` 串成的侵入式单链表，表尾是 `KERNFS_NOTIFY_EOL`（`&kernfs_notify_list` 自身，file.c:38），判空即判"已在队列"，无需额外分配，并 `schedule_work()` 跑 `kernfs_notify_workfn()`。work 遍历 `root->supers`，对每个 sb 用 `ilookup()` 找 inode 再手工发 `FS_MODIFY`（`fsnotify_modify()` 需要 `struct file`，此处没有）。

### 4.7 release 与 drain

`kernfs_fop_release()`（file.c:763-782）在 `KERNFS_HAS_RELEASE` 时用 `kernfs_open_file_mutex_lock(kn)` 串行化并调 `kernfs_release_file()`，再 `kernfs_unlink_open_file()`、`seq_release()`、释放 `prealloc_buf` 与 `of`。`release` 回调由 `kernfs_release_file()` 收口，靠 `of->released` 保证**恰好一次**——正常 close 与 drain 可能同时想调它。

`kernfs_drain_open_files()`（file.c:804-832）遍历 `on->files`：对 `mmapped` 的 `of` 调 `unmap_mapping_range(inode->i_mapping, 0, 0, 1)` 并清标志、减 `nr_mmapped`；有 release 回调的调 `kernfs_release_file()`；最后 `WARN_ON_ONCE(on->nr_mmapped || on->nr_to_release)`。砍掉映射后，后续 fault 因 `kernfs_get_active_of()` 失败而 `SIGBUS`，这就是"节点删除后 mmap 失效"的实现。是否需要 drain 由 `kernfs_should_drain_open_files()` 的无锁 RCU 快照判断（file.c:784-802），并在放锁**之前**调用——节点已 deactivate，`attr.open` 不再变。

---

## 5. VFS 对接

### 5.1 建立 super_block

`kernfs_get_tree()`（mount.c:366-410）先分配 `kernfs_super_info`、填入 `root`/`ns`，**在 `sget_fc()` 之前**赋给 `fc->s_fs_info`（因为 `kernfs_test_super()` 要从它取 `root`/`ns` 比较；`kernfs_set_super()` 则把 `kfc->ns_tag` 置 NULL 后调 `set_anon_super_fc()`）。`sget_fc()` 返回的 sb 若还没有根，就置 `kfc->new_sb_created = true`、`kernfs_fill_super()`、`SB_ACTIVE`、生成 uuid 并把 `info->node` 挂上 `root->supers`（写锁）；最后 `fc->root = dget(sb->s_root)`。`new_sb_created` 被使用者读取（cgroup 据此决定是否多 put 一个 cgroup 引用，cgroup.c:2293）。

`SB_I_NOEXEC | SB_I_NODEV` 是历史包袱：用户空间曾假设 sysfs 不会出现可执行文件或设备节点。`sb->s_op = &kernfs_sops`、`sb->s_xattr = kernfs_xattr_handlers`；`SUPPORT_EXPORTOP` 时再挂 `kernfs_export_ops`。`s_shrink->seeks = 0` 告诉回收器这些 inode 不需 IO 重建，可积极回收；根 inode 由 `kernfs_get_inode(sb, info->root->kn)` 取得（读锁），再 `d_make_root()` 与 `set_default_d_op(sb, &kernfs_dops)`。

### 5.2 super_operations

```c
// fs/kernfs/mount.c:58
const struct super_operations kernfs_sops = {
    .statfs       = kernfs_statfs,
    .drop_inode   = inode_just_drop,
    .evict_inode  = kernfs_evict_inode,
    .show_options = kernfs_sop_show_options,
    .show_path    = kernfs_sop_show_path,
    .freeze_fs    = NULL,
    .unfreeze_fs  = NULL,
    .freeze_super = NULL,
    .thaw_super   = NULL,
};
```

`show_options`/`show_path` 只转发给 `root->syscall_ops`，cgroup 借此在 `/proc/mounts` 展示 `nsdelegate` 等选项。**freeze/thaw 刻意不实现**：注释说明 sysfs 承担 suspend/hibernate 电源接口，若 kernfs 支持冻结，挂起任务会等在自己持有的 VFS 锁上而死锁。`drop_inode = inode_just_drop` 表示 inode 可立即回收；`kernfs_evict_inode()` 做 `truncate_inode_pages_final()` + `clear_inode()` + `kernfs_put(inode->i_private)`（inode.c:266-273）。

### 5.3 exportfs

只在 `KERNFS_ROOT_SUPPORT_EXPORTOP` 时启用。`kernfs_encode_fh()`（mount.c:82-95）要求 `*max_len >= 2`，把 `*(u64 *)fh = kn->id` 并返回 `FILEID_KERNFS`——handle 就是 8 字节的节点 id。反向解析由 `kernfs_fh_to_dentry()`/`kernfs_fh_to_parent()` 共用的 `__kernfs_fh_to_dentry()` 完成，支持 `FILEID_KERNFS`、`FILEID_INO32_GEN`、`FILEID_INO32_GEN_PARENT`：后两种为兼容 `blk_log_action()` 把 id 拆成 `"LOW32,HIGH32"` 暴露、用户再拼成通用 fid 回来的场景，`id = ((u64)fid->i32.gen << 32) | fid->i32.ino;`；随后 `kernfs_find_and_get_node_by_id(info->root, id)`，失败 `-ESTALE`（mount.c:97-144）。

`kernfs_node_dentry()`（mount.c:222-281）是另一条"节点 → dentry"的路，供 cgroup namespace 挂载把根指向 `cgroupns->root_cgrp`；它要求 `KERNFS_ROOT_INVARIANT_PARENT`（mount.c:243），因为注释明确 cgroup 的 `kn` 父节点不可替换，可在 RCU 区外继续使用 `knparent`，随后逐级 `lookup_noperm_positive_unlocked()` 正实例化路径。

### 5.4 inode 初始化

```c
// fs/kernfs/inode.c:201-221（节选）
kernfs_get(kn);
inode->i_private = kn;
inode->i_mapping->a_ops = &ram_aops;
inode->i_op = &kernfs_iops;
inode->i_generation = kernfs_gen(kn);

set_default_inode_attr(inode, kn->mode);
kernfs_refresh_inode(kn, inode);

switch (kernfs_type(kn)) {
case KERNFS_DIR:
    inode->i_op = &kernfs_dir_iops;
    inode->i_fop = &kernfs_dir_fops;
    break;
case KERNFS_FILE:
    inode->i_size = kn->attr.size;
    inode->i_fop = &kernfs_file_fops;
    break;
case KERNFS_LINK:
    inode->i_op = &kernfs_symlink_iops;
    break;
/* default: BUG(); 之后 unlock_new_inode(inode); */
```

`inode->i_private = kn` 是全篇最关键的一行：所有 VFS 回调靠它从 inode 回到节点。`ram_aops` 使 page cache 行为像 ramfs（无回写）。`kernfs_init_inode()` 按 `kernfs_type(kn)` 选 `i_op`/`i_fop`，`KERNFS_EMPTY_DIR` 还会 `make_empty_dir_inode()`。`kernfs_get_inode()` 用 `iget_locked(sb, kernfs_ino(kn))`，仅 `I_NEW` 才初始化，且可能返回 `NULL`（inode.c:248-257）。

### 5.5 属性刷新与权限

```c
// fs/kernfs/inode.c:167
static void kernfs_refresh_inode(struct kernfs_node *kn, struct inode *inode)
{
    struct kernfs_iattrs *attrs;

    inode->i_mode = kn->mode;
    attrs = kernfs_iattrs_noalloc(kn);
    if (attrs)
        set_inode_attr(inode, attrs);

    if (kernfs_type(kn) == KERNFS_DIR && !(kn->flags & KERNFS_REMOVING))
        set_nlink(inode, kn->dir.subdirs + 2);
}
```

uid/gid/时间戳有"持久（`kn->iattr`）+ 缓存（inode）"两份，`kernfs_iop_getattr()`、`kernfs_iop_permission()`、`kernfs_iop_setattr()` 每次都在 `kernfs_iattr_rwsem` 下刷新。`subdirs + 2` 是 POSIX 目录 nlink 惯例（`.` 与父目录的 `..`）；删除中目录不刷新，避免 `clear_nlink()` 后又被加回。`kernfs_iop_permission()` 用 `generic_permission(&nop_mnt_idmap, ...)`，即不支持 idmapped mount；`MAY_NOT_BLOCK` 直接 `-ECHILD`，因为刷新 iattr 需要 `kernfs_iattr_rwsem`。

### 5.6 lookup 与 revalidate

`kernfs_iop_lookup()`（dir.c:1248-1289）在读锁下按父目录的 ns 调 `kernfs_find_ns()`；找到但 `!kernfs_active(kn)` 时**直接返回 `NULL` 而不是负 dentry**——注释写明"inactive 节点对 VFS 不可见"，若缓存负 dentry 会阻止后续激活的同名节点被看见，对 `CREATE_DEACTIVATED` 是致命的。成功取到 inode 后 `kernfs_set_rev(parent, dentry)` 记录父目录版本，再 `d_splice_alias()`。

负 dentry 用 `dir.rev` 失效：`kernfs_set_rev()` 把 `parent->dir.rev` 存进 `dentry->d_time`；`kernfs_link_sibling()`/`kernfs_unlink_sibling()` 每次都 `kernfs_inc_rev(parent)`，父目录一有增删，`kernfs_dir_changed()` 就为真，`kernfs_dop_revalidate()` 返回 0 触发重新 lookup（kernfs-internal.h:147-164）。正 dentry 的 revalidate 检查四件事（dir.c:1215-1236）：仍 active、父未变、名字未变、ns 未变。`kernfs_dops` 只有 `d_revalidate`（dir.c:1244），因为 dentry 释放不需要 kernfs 侧动作——节点引用由 inode 持有。

---

## 6. 目录与查找

### 6.1 排序键与哈希

`kernfs_name_hash()`（dir.c:329-344）以 `kernfs_ns_id(ns)` 为种子，逐字节 `partial_name_hash()`，`end_name_hash()` 后截成 31 位，并把小于 2 的值加 2、`>= INT_MAX` 的压到 `INT_MAX - 1`——`0/1/INT_MAX` 保留给 `.`、`..` 与"遍历结束"，这是 readdir 用 hash 当游标的前提。ns_id 混入种子使同名不同 ns 天然分到不同位置。比较函数是严格三段式（dir.c:346-361）：先 hash、再 `ns_id`、最后 `strcmp(name)`；ns 用 `ns->ns_id` 而非指针，避免把内核地址经 `ctx->pos` 泄露给用户（dir.c:310-320）。

### 6.2 find 与 walk

`kernfs_find_ns()`（dir.c:886-917）先 `lockdep_assert_held(&root->kernfs_rwsem)`，校验"父目录是否启用 ns"与传入 `ns` 一致（否则 `WARN` 返回 NULL），算出 hash 后在 `parent->dir.children` 上二分，用 `kernfs_name_compare()` 决定左/右/命中。`kernfs_find_and_get_ns()` 在读锁内调用并 `kernfs_get()`；`kernfs_walk_and_get_ns()` 按 `/` 切分逐级下行（`strsep` 循环，dir.c:939-943）。`kernfs_walk_ns()` 借全局 `kernfs_pr_cont_buf` 切分，必须持 `kernfs_pr_cont_lock`（dir.c:928），因此不能并发执行。

### 6.3 readdir 的游标

`kernfs_fop_readdir()`（dir.c:1970-2008）先 `dir_emit_dots()`，再在读锁下按 `kernfs_info(dentry->d_sb)->ns` 过滤，用 `for (pos = kernfs_dir_pos(ns, parent, ctx->pos, pos); pos; pos = kernfs_dir_next_pos(...))` 遍历，每项把 `ctx->pos` 置为 `pos->hash`、`file->private_data = pos` 并 `kernfs_get(pos)`，然后 `dir_emit()`；缓冲满就带着当前 `ctx->pos` 返回，正常结束置 `INT_MAX`。

1. `ctx->pos` 是上一项的 hash。`kernfs_dir_pos()` 在 rb 树按 hash 二分定位起点并跳过"已死/ns 不匹配"项，`kernfs_dir_next_pos()` 用 `rb_next()` 前进。两次 `getdents64()` 之间目录增删可能漏掉 hash 之前的插入；它只保证"按 hash 单调前进"，不保证快照一致——与 [`getdents64`](../../02-core/syscall/file-meta/getdents64.md) 的通用契约一致。
2. `file->private_data` 缓存上一项以减少二分；`kernfs_dir_pos()` 校验"仍 active、父仍是 parent、hash 仍是 ctx->pos"，不满足则丢弃重找（dir.c:1912-1922）。缓存持有一个 `count`，由 `kernfs_dir_fop_release()` 还掉。
3. `kernfs_dir_fops` 用 `.iterate_shared = kernfs_fop_readdir`（dir.c:2010-2015）：并发控制由 `kernfs_rwsem` 负责，多个 `getdents` 可并行，不需要 VFS 的 `i_rwsem`。

### 6.4 symlink 的 target 路径

`kernfs_create_link()`（symlink.c:25-54）用 `kernfs_new_node(parent, name, S_IFLNK|0777, uid, gid, KERNFS_LINK)` 建节点，属主跟随 target（target 有 iattr 就取它的 uid/gid），`kernfs_get(target)` 把引用交给链接，只存 `target_kn` 指针，**不存字符串**。

链接内容在 `get_link()` 时算：`kernfs_get_target_path()`（symlink.c:56-110）先沿着 `parent` 上溯，同时在 target 一侧上溯找共同祖先，每退一层就往路径里写 `"../"`；然后从 target 反向填名字（`len -= slen; memcpy(s + len, name, slen); if (len) s[--len] = '/';`）。之所以反向填，是因为只有子→父指针，无法正向遍历。`kernfs_getlink()` 在读锁下调用；`kernfs_iop_get_link()` 分配 `PAGE_SIZE` 并用 `set_delayed_call(done, kfree_link, body)` 交 VFS 释放（symlink.c:128-147）。父目录启用 ns 时链接继承 `target->ns`，否则会指向本 sb 看不见的节点。

---

## 7. 并发与锁

### 7.1 锁的层次

| 锁 | 位置 | 保护对象 | 特点 |
|----|------|----------|------|
| `kernfs_supers_rwsem` | `kernfs_root` | `root->supers` | 最外层，读锁可嵌套 `kernfs_rwsem` |
| `kernfs_rwsem` | `kernfs_root` | 树拓扑、active 检查、rb 树 | 写锁改拓扑，读锁查找/遍历 |
| `kernfs_iattr_rwsem` | `kernfs_root` | `iattr`、`subdirs`、`rev` | 内层 |
| `kernfs_rename_lock` | `kernfs_root` | `__parent`/`name` 替换 | rwlock，供不能睡眠的读者 |
| `node_mutex[i]` | 全局 `kernfs_locks` | `attr.open`、`of->list`、xattr | 以 `kn` 地址哈希选槽 |
| `of->mutex` | `kernfs_open_file` | 同一打开文件的 ops（`prealloc_mutex` 另护缓冲） | active 引用内层 |
| `kernfs_notify_lock` / `kernfs_pr_cont_lock` | 静态 | notify 链 / 打印缓冲 | spinlock irqsave |

历史提示：早期内核用一把全局 `kernfs_mutex` 串行化几乎所有操作。本树已拆成"按 root 的读写信号量 + 全局哈希互斥锁数组"；kernfs.h:39-95 的长注释解释了哈希锁数量随 CPU 数变化（1 CPU → 2 把，32+ CPU → 1024 把），以及"以节点地址为哈希键"的安全性依据：这些锁不承担全局互斥语义。

```c
// fs/kernfs/kernfs-internal.h:218
static inline struct mutex *kernfs_node_lock_ptr(struct kernfs_node *kn)
{
    int idx = hash_ptr(kn, NR_KERNFS_LOCK_BITS);

    return &kernfs_locks->node_mutex[idx];
}
```

### 7.2 典型加锁顺序

```
kernfs_remove():  down_read(supers_rwsem) → down_write(kernfs_rwsem)
                    __kernfs_remove()（drain 可能临时放掉两者）
                  up_write(kernfs_rwsem) → up_read(supers_rwsem)

kernfs_add_one(): down_write(kernfs_rwsem) → link_sibling 内 down_write(iattr_rwsem)
                  up_write(kernfs_rwsem) → kernfs_activate() 再取写锁

kernfs_fop_open(): kernfs_get_active() → node_mutex[i] → ops->open(of)
                   → kernfs_put_active()

kernfs_xattr_set(): CLASS(kernfs_node_lock) → node_mutex[i]
```

方向固定为 `supers_rwsem → kernfs_rwsem → iattr_rwsem`、`rwsem → rename_lock`，没有反向路径。文件 I/O 完全不持 `kernfs_rwsem`，高并发读 `/sys` 不会在层级锁上排队。

### 7.3 RCU 与内存序

| 位置 | 机制 |
|------|------|
| `kn->name` / `kn->__parent` | `rcu_assign_pointer()` + `rcu_dereference_check()` |
| 节点释放 | `call_rcu(&kn->rcu, kernfs_free_rcu)` |
| `kn->iattr` | `READ_ONCE()` + `try_cmpxchg()` |
| `kn->attr.open` | `rcu_assign_pointer()` / `rcu_dereference_protected()`（需 `node_mutex[i]`） |
| `kernfs_open_node` / 根 | `kfree_rcu()` |
| notify 队列 | `spin_lock_irqsave` + `notify_next` |

### 7.4 打破 active 保护

active 保护保证"节点不会在使用中被删除"，但两种场景要主动打破：文件操作要拿会与删除路径成环的锁（cgroup 的 `cgroup_mutex`），或文件操作要删除自己。`kernfs_break_active_protection(kn)` 的实现就是 `kernfs_put_active(kn)`（dir.c:1643），注释强调"必须在一个持有 active 引用的 kernfs 操作里调用，否则 lockdep 会报错"；`kernfs_unbreak_active_protection(kn)` 在收尾时 `atomic_inc(&kn->active)` 并补一次 `rwsem_acquire()`（dir.c:1668）。它只是把计数加回去配平外层 `put_active`，**不能**恢复保护——期间节点可能已被 drain。

cgroup 的用法（cgroup.c:1695-1724）是：`cgroup_tryget(cgrp)` 用自己的引用计数保活 → `kernfs_break_active_protection(kn)` → `cgroup_lock()`（或 `cgroup_lock_and_drain_offline()`）→ `cgroup_is_dead()` 重新确认 → 失败则 `cgroup_kn_unlock()`。这样避免 "kernfs active → cgroup_mutex" 与 "cgroup_mutex → kernfs_remove" 的锁环。

`kernfs_remove_self()`（dir.c:1712-1766）用 `SUICIDAL`/`SUICIDED` 仲裁：第一个进入者置 `SUICIDAL`、`__kernfs_remove(kn)`、置 `SUICIDED` 并返回 true；后来者在 `deactivate_waitq` 上等"`SUICIDED` 且 active == bias"，期间放掉 `kernfs_rwsem`/`supers_rwsem` 再 `schedule()`，醒来重取。最后的 `kernfs_unbreak_active_protection(kn)` 必须在写锁内执行，否则等待条件可能提前满足。

---

## 8. 使用方分析

### 8.1 sysfs：kobject → kernfs

```c
// fs/sysfs/dir.c:49-60（节选）
if (kobj->parent)
    parent = kobj->parent->sd;
else
    parent = sysfs_root_kn;
...
kn = kernfs_create_dir_ns(parent, kobject_name(kobj), 0755, uid, gid,
                          kobj, ns);
kobj->sd = kn;
```

`priv = kobj` 是 sysfs 全部魔法的来源。文件回调反查 kobject 时不用 `of->kn->priv`（那是 `attribute`），而取**父目录**的 `priv`：

```c
// fs/sysfs/file.c:22
static struct kobject *sysfs_file_kobj(struct kernfs_node *kn)
{
    guard(rcu)();
    return rcu_dereference(kn->__parent)->priv;
}
```

属性文件从 8 个静态 `kernfs_ops` 里挑一个（`SYSFS_PREALLOC` 决定 seq 还是 prealloc），再统一 `__kernfs_create_file(parent, attr->name, mode & 0777, uid, gid, PAGE_SIZE, ops, (void *)attr, ns, key);`（fs/sysfs/file.c:316-317）。`priv = (void *)attr`，`size = PAGE_SIZE`，`key` 来自 `attr->key`/`attr->skey`。读路径最终落到 `read(2) → kernfs_fop_read_iter → seq_read_iter → kernfs_seq_show → sysfs_kf_seq_show → kobj->ktype->sysfs_ops->show(kobj, of->kn->priv, buf)`；`sysfs_kf_seq_show()` 用 `seq_get_buf()` 拿 ≥ `PAGE_SIZE` 缓冲、清零、调 `show()`，并对返回值 ≥ `PAGE_SIZE` 做 `WARN` 截断（fs/sysfs/file.c:46-80）。

### 8.2 cgroup：控制文件的生命周期

| 特性 | cgroup 用法 |
|------|-------------|
| `CREATE_DEACTIVATED` | 建完整棵树后一次性 `kernfs_activate()` |
| `SUPPORT_EXPORTOP` | `name_to_handle_at`/`open_by_handle_at`（systemd 需要） |
| `SUPPORT_USER_XATTR` | 允许 `user.*` xattr |
| `INVARIANT_PARENT` | 目录不可移动，`kernfs_node_dentry()` 依赖此保证 |
| 自定义 `syscall_ops` | mkdir/rmdir/show_options/show_path |
| `seq_start/next/stop` | `cgroup.procs` 等多行文件 |
| `atomic_write_len` | 所有控制文件 `PAGE_SIZE` |

`cgroup_add_file()`（cgroup.c:4437-4464）用 `__kernfs_create_file(cgrp->kn, name, mode, current_fsuid(), current_fsgid(), 0, cft->kf_ops, cft, NULL, key)`：`priv = cft`、`size = 0`（不支持 seek-end/stat 大小）、`ns = NULL`（命名空间靠挂载根 dentry 实现）。`cft->file_offset` 非 0 时把 `kn` 缓存在 `struct cgroup_file`，供 `cgroup_file_notify()` 调 `kernfs_notify()`（cgroup.c:4716-4739）。

`cgroup_kf_single_ops` 与 `cgroup_kf_ops`（cgroup.c:4410-4429）都设 `atomic_write_len = PAGE_SIZE`、`open/release/write/poll` 用 cgroup 自己的函数；区别是后者提供 `seq_start/next/stop`，用于 `cgroup.procs` 等多行文件，前者靠 kernfs 默认单次输出。`open` 回调把 per-open 的 cgroup namespace 存进 `of->priv`（cgroup.c:4277-4299），说明 `of->priv` 是使用者自由字段，与节点级共享的 `kn->priv` 不同。

目录操作经 `cgroup_kf_syscall_ops`（cgroup.c:6337）进入 `cgroup_mkdir()`，再由 `cgroup_create()` 调 `kernfs_create_dir_ns(parent->kn, name, mode, current_fsuid(), current_fsgid(), cgrp, NULL)`（cgroup.c:5908-5911）。因为根是 `CREATE_DEACTIVATED`，`add_one` 不会激活；真正可见是 `cgroup_mkdir()` 末尾的 `kernfs_activate(cgrp->kn)`（cgroup.c:6080），此时控制文件已全部建好——这正是 `CREATE_DEACTIVATED` 的目的：让一批节点原子地一起可见（kernfs.h:123-131）。删除则相反：先摘文件，再 `kernfs_remove(cgrp->kn)`（cgroup.c:6285），由 kernfs drain 进行中的读写与 mmap。

### 8.3 resctrl 与共同模式

resctrl 的根 flag 是 `CREATE_DEACTIVATED | EXTRA_OPEN_PERM_CHECK`，`priv` 指向 `rdtgroup`，目录操作由 `rdtgroup_kf_syscall_ops`（mkdir/rmdir/rename/show_options，rdtgroup.c:4277-4282）提供。它是第三个 in-tree 用户，说明只要数据模型是"内核驱动增删、用户按文件读写"的树，就可以直接复用 kernfs。

| 步骤 | sysfs | cgroup | resctrl |
|------|-------|--------|---------|
| 建根 | 启动一次，`scops = NULL` | 每层级一个，4 个 flag | 初始化时一个 |
| 建目录 | `create_dir_ns(..., priv=kobj)` | `(..., priv=cgrp)` | mkdir 回调里建 |
| 建文件 | `__kernfs_create_file(..., priv=attr, size=PAGE_SIZE)` | `(..., priv=cft, size=0)` | 各自 ops |
| 可见性 | 立即（非 deactivated） | `kernfs_activate()` | `kernfs_activate()` |
| 删除/通知 | `kernfs_remove(kobj->sd)` / `sysfs_notify()` | `kernfs_remove(cgrp->kn)` / `cgroup_file_notify()` | 回调里删 |

---

## 9. 配置与调试

### 9.1 Kconfig

```kconfig
# fs/kernfs/Kconfig
config KERNFS
        bool
        default n
```

没有 `prompt`，用户不能直接选，只能由 `SYSFS`/`CGROUPS`/`RESCTRL` 等使用者 `select`。Makefile 固定编 5 个目标：

```make
# fs/kernfs/Makefile
obj-y   := mount.o inode.o dir.o file.o symlink.o
```

即只要 `CONFIG_KERNFS=y`，整个框架就全编进去，没有子选项。

### 9.2 调试手段

| 手段 | 触发 | 能看到什么 |
|------|------|------------|
| `CONFIG_DEBUG_LOCK_ALLOC` + `KERNFS_LOCKDEP` | 使用者传 `lock_class_key` | `kn->dep_map` 名为 `"kn->active"`，每个属性文件的 active 引用是独立读锁 |
| `lockdep_assert_held(&root->kernfs_rwsem)` | 大量函数入口 | 漏锁直接 splat，而非静默竞争 |
| `pr_cont_kernfs_path(kn)` | 内核打印 | 在 `pr_cont` 中安全输出节点全路径 |
| `WARN_ONCE` in `kernfs_put()` | active 计数不配平 | `"released with incorrect active_ref %d"` |
| `WARN` in `kernfs_add_one()`/`kernfs_find_ns()` | ns 不一致 | `"kernfs: ns required/invalid in '%s' for '%s'"` |
| `/proc/mounts` options | `show_options`/`show_path` | cgroup 挂载选项与路径重写 |

`kernfs_pr_cont_name/path` 使用独立的 `kernfs_pr_cont_lock`，刻意不与 `kernfs_rename_lock` 混用：dir.c:21-28 的注释说明 `pr_cont()` 释放 `console_sem` 时可能唤醒调度器，调度器若读 `kernfs_name` 会与 rename 锁形成环。

### 9.3 常见错误

| 错误码 | 出处 | 含义与排查 |
|--------|------|------------|
| `-EEXIST` | `kernfs_link_sibling()` | `(hash, ns_id, name)` 重复；sysfs 额外 `dump_stack()` 打印路径 |
| `-ENODEV` | `kernfs_get_active()` 失败 | 节点已 deactivate（remove/hide），文件正在消失 |
| `-EACCES` | `kernfs_fop_open()` 的额外权限检查 | 模式无 `S_IRUGO`/`S_IWUGO`，或回调未实现 |
| `-E2BIG` | `kernfs_fop_write_iter()` | 写超过 `atomic_write_len` |
| `-EINVAL` | `kernfs_add_one()` | 父不是目录，或 ns 不匹配 |
| `-ENOENT` | `kernfs_add_one()`/`kernfs_rename_ns()` | 父节点在 `KERNFS_REMOVING`，或目标不可用 |
| `-ENAMETOOLONG` | `kernfs_get_target_path()` | 符号链接相对路径超 `PATH_MAX` |
| `-ENODATA` / `-EOPNOTSUPP` | `kernfs_xattr_get()` / 用户 xattr 钩子 | 无该 xattr；根未设 `KERNFS_ROOT_SUPPORT_USER_XATTR` |
| `-ESTALE` | exportfs 路径 | fhandle 指向的节点已删，或 generation 不匹配 |

### 9.4 排查"文件消失"

1. `ls` 可见但 `cat` 返回 `ENODEV` → 节点已 deactivate，检查是否有 `kernfs_remove()` 在跑。
2. nlink 异常 → 检查 `dir.subdirs` 是否与实际子目录数一致；不一致通常是绕过 `kernfs_link_sibling()` 直接改树。
3. `EEXIST` → 多为真正重名；`0/1/INT_MAX` 被保留，名字与魔数冲突概率极低。
4. 读写卡死 → 可能有人在 `kernfs_ops->write` 里调 `kernfs_remove()` 却没先 `kernfs_break_active_protection()`，drain 在等自己。

---

## 10. 关键文件清单

| 文件 | 行数 | 核心内容 |
|------|------|----------|
| `include/linux/kernfs.h` | 654 | 公开结构体、flag、API、`kernfs_ops`、内联辅助 |
| `fs/kernfs/kernfs-internal.h` | 236 | root/super_info/iattrs、`KN_DEACTIVATED_BIAS`、哈希节点锁 |
| `fs/kernfs/dir.c` | 2015 | 分配/链接/激活/删除/改名、查找、readdir、syscall_ops 转发 |
| `fs/kernfs/file.c` | 1085 | `kernfs_file_fops`、open/release、seq/裸 I/O、mmap/poll、notify |
| `fs/kernfs/inode.c` | 406 | `kernfs_iops`、get/init/evict_inode、iattr、xattr handlers |
| `fs/kernfs/mount.c` | 472 | `kernfs_sops`、get_tree/kill_sb、`kernfs_node_dentry`、exportfs |
| `fs/kernfs/symlink.c` | 155 | `kernfs_create_link`、相对路径、`kernfs_symlink_iops` |
| `fs/kernfs/Kconfig`/`Makefile` | 7 / 6 | 隐藏 bool 开关；5 个编译单元 |
| `fs/sysfs/{dir,file,mount,symlink}.c` | — | kobject ↔ kernfs_node、attribute ↔ kernfs_ops |
| `kernel/cgroup/cgroup.c` | — | 层级根、cftype ↔ kernfs_ops、`cgroup_kn_lock_live()` |
| `fs/resctrl/rdtgroup.c` | — | 第三个 kernfs 用户 |

---

## 11. 延伸阅读

- [sysfs — 内核对象文件系统](../special/02-sysfs.md)：kobject 视角的 kernfs 用法。
- [cgroup v1 文件系统](../special/07-cgroup.md)：cgroup 如何组织控制文件。
- [VFS 总览](../vfs/fs.md)：`super_block`、`inode`、`dentry`、`file_operations` 的通用语义。
- [cgroup v2](../../09-containers/01-cgroup-v2.md)：`cgroup.kill`、`cgroup.freeze` 等文件背后的实现。
- [getdents64](../../02-core/syscall/file-meta/getdents64.md)：目录遍历与 `ctx->pos` 契约，对照本篇 6.3 节。
