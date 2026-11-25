# kobject — 内核对象模型

`kobject` 是 driver core 的地基：它不承载业务语义，只提供四件事——**引用计数、父子拓扑（同时就是 sysfs 目录树）、名字、事件源**。`struct device`、`struct bus_type` 的注册状态、模块的 `/sys/module` 目录，都是"某个更大的结构体内嵌一个 `kobject`（或 `kset`）"。

本文只讲 kobject 自身的语义与实现，不重复 sysfs 的用法与 device 的注册流程：sysfs 视角（`attribute` 怎么写、`sysfs_create_group()` 怎么用）见 [sysfs 专章](../../05-filesystems/special/02-sysfs.md) §2–§4；device 模型视角（bus/driver/probe）见 [driver-model](01-driver-model.md) §3 与 §9；目录树的 kernfs 实现见 [kernfs 专章](../../05-filesystems/kernfs/01-kernfs.md)。本树为 Linux 7.2 源码（带中文注释增补），`CONFIG_PREEMPT_RT=y`、`CONFIG_SYSFS=y`、`CONFIG_NET=y`、`CONFIG_UEVENT_HELPER` 关闭；文中字段、函数与行号均取自本树。

---

## 1. 定位与设计

### 1.1 kobject 解决什么问题

内核里有大量"要出现在 sysfs、要被引用计数、要发 uevent"的对象。没有统一抽象，每个子系统都要重复实现同样的四套机制：

| 职责 | 各自为政的做法 | kobject 的机制 |
|------|----------------|----------------|
| 命名与拓扑 | 维护父指针、手工拼路径 | `name`/`parent` + `kobject_get_path()` 向上拼 |
| 生命周期 | 自定义 refcount 与释放时机 | `kref` + `kobj_type->release` 单一收敛点 |
| sysfs 呈现 | 直接调 kernfs、自己处理并发删除 | `sd` + `ktype->default_groups` 自动建删 |
| 事件通知 | 手拼 netlink 报文 | `kobject_uevent()` 统一 ACTION/DEVPATH/SUBSYSTEM/SEQNUM |

因此 kobject 是**最小公约数**：只放共有的字段，业务数据留在外层结构体，用 `container_of()` 反查（`kobj_to_dev()`、`to_kset()`、`to_class_dir()`）。这带来两个反直觉的设计：内嵌者自己分配内存，kobject 从不 `kzalloc` 外层结构（唯一例外是 `kobject_create()` 的裸 kobject）；kobject 被销毁的唯一合法途径是 `kobject_put()` 掉到 0，`kfree()` 一律错误。

### 1.2 与 kset / kobj_type / sysfs / uevent 的关系

```text
       外层对象（struct device / net_device / module ...）
                        │ 内嵌
                        ▼
   struct kobj_type ◄── ktype ── struct kobject ──sd──► struct kernfs_node
   ├ release()                 │      │                 (sysfs 目录, priv=kobj)
   ├ sysfs_ops                 │      └─ name / kref / 状态位
   ├ default_groups            └─ parent ──► 父 kobject ──► … ──► sysfs 根
   ├ child_ns_type()                  │
   ├ namespace()                      └─ kset ──► struct kset
   └ get_ownership()                              ├ list/list_lock（同类成员）
                                                  ├ kobj（自嵌，接父）
                                                  └ uevent_ops（filter/name/uevent）
```

一句话分工：**kobject** 是实例，管"我是谁、我在哪、我还能活多久"；**kobj_type** 是虚函数表，管"这类对象怎么释放、属性怎么读写"；**kset** 是同类集合与子系统身份，管"这一族的 uevent 怎么加工"；**kernfs** 是存储层，kobject 只是 `kernfs_node->priv` 指向的载荷。注意本树 `struct kobj_type` 已无旧版的 `child_sysfs_direntry`，现在是六个回调（`include/linux/kobject.h:116-123`）。

### 1.3 本树文件构成

| 文件 | 行数 | 内容 |
|------|------|------|
| `include/linux/kobject.h` | 222 | `kobject`/`kset`/`kobj_type`/`kobj_attribute`、action 枚举 |
| `lib/kobject.c` | 1107 | 生命周期、注册、命名、路径、kset、ns 类型表 |
| `include/linux/kref.h` | 135 | 引用计数，**纯内联**；本树无 `lib/kref.c` |
| `include/linux/kobject_ns.h` | 58 | `enum kobj_ns_type` 与 `kobj_ns_type_operations` |
| `lib/kobject_uevent.c` | 851 | netlink 广播、synth uevent、`uevent_helper`、per-net socket |
| `fs/sysfs/dir.c` / `file.c` | 161 / 821 | kobject→目录节点、属性文件→`sysfs_ops` |
| `samples/kobject/*.c` | — | 裸 kobject 与 kset 两个最小示例 |

`kref` 没有 `.c` 文件这点常被记错：`kref_init/get/put/get_unless_zero` 全是 `static inline`，编译进每个使用者。本树也没有 `kref_replace_release()`，只有 `kref_put_mutex()`/`kref_put_lock()` 两个"持锁发布"变体（`kref.h:81-113`）。

---

## 2. 核心数据结构

### 2.1 struct kobject

`include/linux/kobject.h:64-82`，逐字如下：

```c
struct kobject {
	const char		*name;
	struct list_head	entry;
	struct kobject		*parent;
	struct kset		*kset;
	const struct kobj_type	*ktype;
	struct kernfs_node	*sd; /* sysfs directory entry */
	struct kref		kref;

	unsigned int state_initialized:1;
	unsigned int state_in_sysfs:1;
	unsigned int state_add_uevent_sent:1;
	unsigned int state_remove_uevent_sent:1;
	unsigned int uevent_suppress:1;

#ifdef CONFIG_DEBUG_KOBJECT_RELEASE
	struct delayed_work	release;
#endif
};
```

| 字段 | 语义 | 维护者 |
|------|------|--------|
| `name` | 目录名、`DEVPATH` 末段 | `kobject_set_name*()`，堆分配 |
| `entry` | 挂进 `kset->list` 的节点 | `kobj_kset_join()`/`kobj_kset_leave()` |
| `parent` | 父对象；`NULL` 表示挂 sysfs 根 | `kobject_add_internal()`，持有引用 |
| `kset` | 所属集合，add 前必须设好 | 使用者（如 `device_initialize()`） |
| `ktype` | 类型回调，`kobject_init()` 强制非空 | `kobject_init()` |
| `sd` | 对应 sysfs 目录节点，未注册为 `NULL` | `sysfs_create_dir_ns()`/`sysfs_remove_dir()` |
| `kref` | 引用计数，初始 1 | `kref_*` |
| `state_initialized` | 已 `kobject_init()` | init 置 1，之后不清 |
| `state_in_sysfs` | `sd` 有效 | add/del 成对置位 |
| `state_add_uevent_sent` | 发过 `KOBJ_ADD` | `kobject_uevent_env()` |
| `state_remove_uevent_sent` | 发过 `KOBJ_REMOVE` | `kobject_uevent_env()` |
| `uevent_suppress` | 静默该对象的 uevent | 使用者直接置位（网卡常用） |

状态位不是装饰，它们让 core 能"替使用者补做"动作：`__kobject_del()` 发现发过 add 却没发 remove 时会自动补发；`kobject_cleanup()` 发现还在 sysfs 里会自动 `__kobject_del()`。容错变好，但也掩盖使用者的生命周期 bug。`entry` 只有加入 kset 才有意义；`parent` 与 `kset` 是两套并行组织方式——**kset 决定"属于哪个子系统、uevent 谁加工"，parent 决定"在 sysfs 树的哪个位置"**，两者常指向同一对象（kset 自身的 kobject 往往是成员的父目录）。

### 2.2 kref

```c
/* include/linux/kref.h:19-21 */
struct kref {
	refcount_t refcount;
};
```

用 `refcount_t` 而非 `atomic_t`，是为了"从 0 再加"和"减成负数"时由 `refcount_inc()`/`refcount_dec_and_test()` 触发告警。初始值由 `kref_init()` 设为 **1**（`kref.h:29-32`），所以 `kobject_init()` 之后调用者天然持有一个引用。

### 2.3 struct kset

```c
/* include/linux/kobject.h:168-173 */
struct kset {
	struct list_head list;
	spinlock_t list_lock;
	struct kobject kobj;
	const struct kset_uevent_ops *uevent_ops;
} __randomize_layout;
```

`__randomize_layout` 表明它是内部结构，字段顺序不保证。要点：`kobj` 是自嵌的 kobject，因此 **kset 自己也是一个 sysfs 目录**（`/sys/bus`、`/sys/class`、`/sys/devices` 都是 kset），`to_kset()` 就是 `container_of`；`list` 是 `kset` 指向它的所有成员，受 `list_lock` 保护，`kset_find_obj()`（`kobject.c:904`）在锁内遍历并 `kobject_get_unless_zero()`；`uevent_ops` 是这一族的 uevent 加工点（见 §7）。引用是双向的：成员 add 时 `kset_get()`、离开时 `kset_put()`；`kset_unregister()` 就是 `kobject_del()` + `kobject_put()`。

### 2.4 struct kobj_type

```c
/* include/linux/kobject.h:116-123 */
struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;
	const struct attribute_group **default_groups;
	const struct kobj_ns_type_operations *(*child_ns_type)(const struct kobject *kobj);
	const struct ns_common *(*namespace)(const struct kobject *kobj);
	void (*get_ownership)(const struct kobject *kobj, kuid_t *uid, kgid_t *gid);
};
```

| 回调 | 调用点 | 作用 |
|------|--------|------|
| `release` | `kobject_cleanup()`（kobject.c:686） | **唯一**释放外层对象的地方 |
| `sysfs_ops` | `sysfs_add_file_mode_ns()`、`fs/sysfs/file.c` | 属性文件 show/store 分派 |
| `default_groups` | `create_dir()`（kobject.c:78） | 注册时自动建组、删除时自动摘 |
| `child_ns_type` | `kobj_child_ns_ops()`（kobject.c:1059） | 声明"我的子对象带哪种 ns tag" |
| `namespace` | `kobject_namespace()`（kobject.c:30） | 给出本对象自己的 ns tag |
| `get_ownership` | `kobject_get_ownership()`（kobject.c:50） | 覆盖 sysfs 节点属主，容器场景用 |

`release` 可以缺省，`kobject_cleanup()` 只 `pr_debug` 抱怨，但代价是外层对象永不释放——这正是裸 kobject 必须自带 `dynamic_kobj_ktype` 的原因。

### 2.5 kobj_attribute 与 sysfs_ops

```c
/* include/linux/kobject.h:139-147 */
struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count);
};
extern const struct sysfs_ops kobj_sysfs_ops;
```

sysfs 只认识 `struct attribute`（`name`/`mode`），回调靠 `ktype->sysfs_ops` 分派，`kobj_sysfs_ops` 用 `container_of` 把 `attribute*` 转回 `kobj_attribute` 再调 `show/store`（`lib/kobject.c:820-848`）。所以"自定义属性类型" = 一个内嵌 `attribute` 的结构体 + 一个反向转换的 `sysfs_ops`；`device_attribute` 走的是平行的 `dev_sysfs_ops`（`drivers/base/core.c:2511`）。

### 2.6 kobj_uevent_env 与 kset_uevent_ops

```c
/* include/linux/kobject.h:31-33, 125-137 */
#define UEVENT_NUM_ENVP		64	/* number of env pointers */
#define UEVENT_BUFFER_SIZE	2048	/* buffer for the variables */

struct kobj_uevent_env {
	char *argv[3];
	char *envp[UEVENT_NUM_ENVP];
	int envp_idx;
	char buf[UEVENT_BUFFER_SIZE];
	int buflen;
};

struct kset_uevent_ops {
	int (* const filter)(const struct kobject *kobj);
	const char *(* const name)(const struct kobject *kobj);
	int (* const uevent)(const struct kobject *kobj, struct kobj_uevent_env *env);
};
```

`envp[]` 存的是指向同一块 `buf[]` 的指针，`add_uevent_var()` 顺序追加、`envp_idx` 计数。这种"指针数组 + 连续缓冲区"让报文可以一次 `skb_put_data(env->buf, env->buflen)` 发出，代价是 `zap_modalias_env()` 必须手工 `memmove` 压缩缓冲区（`kobject_uevent.c:423-464`）。64 个变量 / 2048 字节是硬上限，超了直接 `WARN` + `-ENOMEM`（`:671-695`）——驱动往 uevent 里塞大块数据要非常克制。

### 2.7 kobj_ns_type_operations

```c
/* include/linux/kobject_ns.h:40-47 */
struct kobj_ns_type_operations {
	enum kobj_ns_type type;
	bool (*current_may_mount)(void);
	struct ns_common *(*grab_current_ns)(void);
	const struct ns_common *(*netlink_ns)(struct sock *sk);
	const struct ns_common *(*initial_ns)(void);
	void (*drop_ns)(struct ns_common *);
};
```

只有两种类型：`KOBJ_NS_TYPE_NONE` 与 `KOBJ_NS_TYPE_NET`（`kobject_ns.h:27-31`）。除 net ns 外没有别的使用者，这解释了为什么 `kobject_uevent_net_broadcast()` 能直接把 `ns_common` `container_of` 成 `struct net`。

---

## 3. 引用计数与生命周期

### 3.1 三个入口

`kobject_get()`（`kobject.c:636`）对未初始化对象 `WARN` 后仍 `kref_get()`，传 `NULL` 静默返回 `NULL`——这让 `kobject_get(kobj->parent)` 可以无条件写。`kobject_get_unless_zero()`（`:649`）用于从查找结构里捞对象，先 `kset_find_obj()` 在锁内比名字、再 `kref_get_unless_zero()`，避免与释放竞态，`__must_check` 强制检查返回值。`kobject_put()`（`:730`）同样对未初始化对象 `WARN`，然后 `kref_put(&kobj->kref, kobject_release)`。

### 3.2 release 与 cleanup 的区别

```text
kobject_put()  ——refcount==0——►  kobject_release(struct kref *)
                                      │
                     ┌────────────────┴─────────────────┐
       CONFIG_DEBUG_KOBJECT_RELEASE               （默认路径）
                     │                                  │
       延迟 HZ..4*HZ 的 delayed_work            kobject_cleanup(kobj)
                     └──────────► kobject_cleanup ◄──────┘
                                      │
                     ┌────────────────┼──────────────────┬────────────────┐
            state_in_sysfs?      ktype->release()   kfree_const(name)   kobject_put(parent)
             是 → __kobject_del()
             否 → parent=NULL（避免多余 put）
```

`kobject_release()` 是 `kref_put()` 的回调（`:709-722`），`kobject_cleanup()` 才真正干活（`:663-699`）。职责划分：**cleanup 负责 kobject 自身资源**（sysfs 目录、名字串、父引用），**`ktype->release` 负责外层对象**（`kfree(dev)`、`kfree(kset)`）。因为 cleanup 先删 sysfs 再调 release，`release` 里不能再 `kobject_del()`、不该访问已置 NULL 的 `sd`、也不能再 `kobject_put(parent)`。

cleanup 里那段 `parent = NULL` 是精髓：

```c
/* lib/kobject.c:676-684 */
	/* remove from sysfs if the caller did not do it */
	if (kobj->state_in_sysfs) {
		pr_debug("'%s' (%p): auto cleanup kobject_del\n",
			 kobject_name(kobj), kobj);
		__kobject_del(kobj);
	} else {
		/* avoid dropping the parent reference unnecessarily */
		parent = NULL;
	}
```

`__kobject_del()` 会把 `kobj->parent` 置 `NULL`，外层 `kobject_del()` 负责 `kobject_put(parent)`。对象若从未进过 sysfs，parent 引用就从未被 `kobject_add_internal()` 取过，此时再 put 就是多减一次。

### 3.3 延迟清理

`CONFIG_DEBUG_KOBJECT_RELEASE` 打开时 `kobject_release()` 不立刻 cleanup，而是把 `kobj->release`（`delayed_work`）挂队列，随机延迟 1–4 秒（`kobject.c:709-722` 的 `HZ + HZ * get_random_u32_below(4)`）。作用是**放大 UAF**：任何仍在使用已 put 对象的代码都会在这几秒内崩出来，而不是偶尔踩到已复用内存。本树 `.config` 无此符号（只有 `# CONFIG_DEBUG_KOBJECT is not set`），`kobject_delayed_cleanup()`（`:702-706`）**源码存在但未编译**，不要把延迟清理当默认行为。

### 3.4 "无 release 不得 init"的约束

`kobject_init()`（`:333-360`）有三道检查：`kobj == NULL` 报错；`ktype == NULL` 报错；已经 `state_initialized` 则 `pr_err` + `dump_stack_lvl()` 但**继续执行**（注释直言 "sometimes we can recover"）。第三点是刻意宽容——重复 init 会把 kref 重置为 1，若对象已被引用则计数错乱，所以只"响亮抱怨"不阻止。真正的强制约束在 release：cleanup 发现 `t && !t->release` 只打 `pr_debug` 然后什么也不释放，内存静默泄漏（`:672-674`）。device 层把它升级成 `WARN(1, KERN_ERR ...)`（`core.c:2638`），因为它确定 `dev` 必须被释放。

### 3.5 dynamic_kobj：裸 kobject

```c
/* lib/kobject.c:742-751, 764-774（节选） */
static void dynamic_kobj_release(struct kobject *kobj)
{
	kfree(kobj);
}

static const struct kobj_type dynamic_kobj_ktype = {
	.release	= dynamic_kobj_release,
	.sysfs_ops	= &kobj_sysfs_ops,
};

	kobj = kzalloc_obj(*kobj);
	kobject_init(kobj, &dynamic_kobj_ktype);
```

`kobject_create()` 是 **static**，对外只暴露 `kobject_create_and_add()`（`:789`）：失败时自清，成功则把"一个引用"交给调用者。`/sys/kernel`、`/sys/dev`、`/sys/devices/virtual` 这些纯目录都是这么来的（`kernel/ksysfs.c:221`、`core.c:3253`、`core.c:4231`）。

### 3.6 全量状态机

```text
     [外层结构体 kzalloc]
             │  kobject_init(kobj, ktype)
             ▼
   state_initialized=1, kref=1
             │  （可选）kobj->kset = …; kobject_set_name()
             │  kobject_add() / kobject_init_and_add()
             ├──失败──► 调用者 kobject_put() ──► cleanup（无 sd，直接 release）
             ▼
   state_in_sysfs=1, parent 与 kset 各持一个引用
             │  kobject_uevent(kobj, KOBJ_ADD)  ← 由使用者/kset_register 发
             ▼
   state_add_uevent_sent=1
             │  kobject_del()
             ├─ 若发过 add 未发 remove：自动 kobject_uevent(KOBJ_REMOVE)
             ▼
   state_in_sysfs=0, parent 置 NULL 并 put
             │  kobject_put()（最后一个引用）
             ▼
   kobject_release → kobject_cleanup → ktype->release → kfree(name)
```

关键次序：**add 只建目录不发 uevent；del 会补发 remove**。这个不对称是历史约定，`kobject_add()` 的 kerneldoc 要求调用者自己建好属性再发 `KOBJ_ADD`（`:392-395`）。

---

## 4. 创建与注册

### 4.1 两段式接口

| API | 语义 | 备注 |
|-----|------|------|
| `kobject_init(kobj, ktype)` | 只初始化 | `state_initialized=1`，kref=1 |
| `kobject_add(kobj, parent, fmt, ...)` | 设名 + 建目录 | 要求已 init |
| `kobject_init_and_add(...)` | 二者合一 | 失败后仍必须 put |
| `kobject_create_and_add(name, parent)` | 分配 + init + add | 返回裸 kobject |
| `kobject_del(kobj)` | 卸载目录 | 不释放对象 |
| `kobject_rename`/`kobject_move` | 改名/换父 | 只对已注册对象 |

两段式的意义是"能在 add 之前拿到引用并慢慢初始化"：`device_initialize()` 先 `kobject_init()`，调用者随后可 `get_device()`，字段填好再 `device_add()`。

### 4.2 kobject_add_internal：父与 kset 的解析

```c
/* lib/kobject.c:210-258（节选） */
	if (!kobj->name || !kobj->name[0]) {
		WARN(1, "kobject: (%p): attempted to be registered with empty name!\n", kobj);
		return -EINVAL;
	}

	parent = kobject_get(kobj->parent);

	/* join kset if set, use it as parent if we do not already have one */
	if (kobj->kset) {
		if (!parent)
			parent = kobject_get(&kobj->kset->kobj);
		kobj_kset_join(kobj);
		kobj->parent = parent;
	}
	...
	error = create_dir(kobj);
	if (error) {
		kobj_kset_leave(kobj);
		kobject_put(parent);
		kobj->parent = NULL;
	} else
		kobj->state_in_sysfs = 1;
```

四条规则：名字必须非空；**parent 优先于 kset**，无显式 parent 才退化为 `&kset->kobj`（这就是"设了 kset 就不用设 parent"的由来，见 `samples/kobject/kset-example.c:229-237`）；只有设了 kset 才 `kobj_kset_join()`，它先 `kset_get()` 再在 `list_lock` 下 `list_add_tail(&kobj->entry, &kset->list)`；失败回滚三件事（离开 kset、put parent、parent 置 NULL），`-EEXIST` 会额外打印"不要在同一个目录里重名注册"。

### 4.3 create_dir：目录与默认属性组

```c
/* lib/kobject.c:67-104（节选） */
	error = sysfs_create_dir_ns(kobj, kobject_namespace(kobj));
	if (error)
		return error;

	if (ktype) {
		error = sysfs_create_groups(kobj, ktype->default_groups);
		if (error) {
			sysfs_remove_dir(kobj);
			return error;
		}
	}

	/* @kobj->sd may be deleted by an ancestor going away.  Hold an
	 * extra reference so that it stays until @kobj is gone. */
	sysfs_get(kobj->sd);

	ops = kobj_child_ns_ops(kobj);
	if (ops) {
		BUG_ON(!kobj_ns_type_is_valid(ops->type));
		BUG_ON(!kobj_ns_type_registered(ops->type));
		sysfs_enable_ns(kobj->sd);
	}
```

三个非显然点：**默认属性组在 add 时建**，所以想"先建目录后建文件"就直接 `kobject_add()` 再 `sysfs_create_file()`，想让 core 代劳就填 `default_groups`（`__ATTRIBUTE_GROUPS()` 生成数组）；**额外持有一个 `kernfs_node` 引用**，父目录先被删时子节点会 deactivate，但 kobject 还活着、可能还要发 uevent（需要路径），这个引用保证 `kobj->sd` 全程可用；`sysfs_enable_ns()` 只在"子对象带 ns tag"时对本目录开启（见 §8）。

### 4.4 名字格式化在 add 里完成

```c
/* lib/kobject.c:362-375 */
static __printf(3, 0) int kobject_add_varg(struct kobject *kobj,
					   struct kobject *parent,
					   const char *fmt, va_list vargs)
{
	retval = kobject_set_name_vargs(kobj, fmt, vargs);
	if (retval) {
		pr_err("can not set name properly!\n");
		return retval;
	}
	kobj->parent = parent;
	return kobject_add_internal(kobj);
}
```

因此 `kobject_add(kobj, parent, "eth%d", idx)` 合法。`fmt` 传 `NULL` 表示名字已设过，`kobject_set_name_vargs()` 首行短路返回 0（`kobject.c:271-272`）——`device_add()` 正是这样调：`kobject_add(&dev->kobj, dev->kobj.parent, NULL)`（`core.c:3695`）。

### 4.5 错误处理不变量

`kobject_add()` 的 kerneldoc（`:397-408`）把规则写死：**无论成功失败，调用后必须 `kobject_put()`；任何情况下不得直接 `kfree()`**。原因是 err 路径里对象已 `state_initialized=1`、kref=1、名字可能已分配，只有 put 才能走 cleanup 收干净。`kobject_create_and_add()`（`:798-803`）与 `device_add()` 的 `put_device(dev)` 都遵守这条。

---

## 5. 命名与路径

### 5.1 kobject_set_name_vargs

```c
/* lib/kobject.c:266-297（节选） */
	if (kobj->name && !fmt)
		return 0;

	s = kvasprintf_const(GFP_KERNEL, fmt, vargs);
	if (!s)
		return -ENOMEM;

	/* ... kvasprintf_const may have returned something from .rodata. */
	if (strchr(s, '/')) {
		t = kstrdup(s, GFP_KERNEL);
		kfree_const(s);
		if (!t)
			return -ENOMEM;
		s = strreplace(t, '/', '!');
	}
	kfree_const(kobj->name);
	kobj->name = s;
```

两个细节：`kvasprintf_const()` 在格式串无 `%` 时直接返回原指针（可能在 `.rodata`），所以释放必须用 `kfree_const()`——这解释了代码里所有 `kfree_const(kobj->name)`；名字里的 `/` 会被静默替换成 `!`，因为 `name` 直接用作目录名与 `DEVPATH` 路径段，含斜杠会破坏树结构（调试时看到目录名有 `!` 就知道某处传了带斜杠的名字）。

### 5.2 kobject_name 与 kobject_get_path

`kobject_name()` 内联返回 `kobj->name`（`kobject.h:87-90`），无锁，调用者须自保引用。路径拼接走两趟：先算长度、再倒填。

```c
/* lib/kobject.c:124-143（节选） */
	--length;
	for (parent = kobj; parent; parent = parent->parent) {
		int cur = strlen(kobject_name(parent));
		length -= cur;
		if (length <= 0)
			return -EINVAL;
		memcpy(path + length, kobject_name(parent), cur);
		*(path + --length) = '/';
	}
```

`kobject_get_path()`（`:152-170`）用 `goto retry` 处理"算长度时名字还有效、填的时候被 rename 了"的竞态。返回串由调用者 `kfree()`，格式如 `/devices/pci0000:00/.../ttyS0`（无 `/sys` 前缀），被用于 uevent 的 `DEVPATH`、rename/move 的 `DEVPATH_OLD` 以及错误日志。

### 5.3 重命名

`kobject_rename()`（`:474-531`）顺序不可颠倒：先 `kobject_get_path()` 取旧路径存成 `DEVPATH_OLD`；`sysfs_rename_dir_ns()` 成功后才把 `kobj->name` 换成新串；最后 `kobject_uevent_env(kobj, KOBJ_MOVE, envp)`。要点是失败时 kobject 状态不变（sysfs 改名会因重名失败），改名发的是 **`KOBJ_MOVE`** 而不是 remove/add（注释说明主要用于网络接口，热插拔工具按名字索引设备），旧名字在 `out` 里 `kfree_const(dup_name)`。函数自己 `get/put` 保护了 kobject，但**不保护调用者之间的并发**——kerneldoc 要求调用者自行串行化同一对象的 rename；且 `kobj->parent` 必须非空（根对象名字就是 sysfs 根，改不了）。

### 5.4 移动

`kobject_move()`（`:538-584`）与 rename 结构相同，差异在：`new_parent` 为 `NULL` 时回落到 `kobj->kset->kobj`，再 NULL 就由 `sysfs_move_dir_ns()` 挂到 `sysfs_root_kn`（`fs/sysfs/dir.c:117-127`）；成功后 `old_parent = kobj->parent; kobj->parent = new_parent; kobject_put(old_parent);`——父引用是"换手"而不是增减；同样发 `KOBJ_MOVE` 并带 `DEVPATH_OLD`。

---

## 6. sysfs / kernfs 映射

### 6.1 kobj->sd 的来历

```c
/* fs/sysfs/dir.c:49-68（节选） */
	if (kobj->parent)
		parent = kobj->parent->sd;
	else
		parent = sysfs_root_kn;

	if (!parent)
		return -ENOENT;

	kobject_get_ownership(kobj, &uid, &gid);

	kn = kernfs_create_dir_ns(parent, kobject_name(kobj), 0755, uid, gid,
				  kobj, ns);
	if (IS_ERR(kn)) {
		if (PTR_ERR(kn) == -EEXIST)
			sysfs_warn_dup(parent, kobject_name(kobj));
		return PTR_ERR(kn);
	}

	kobj->sd = kn;
```

`parent` 用的是 **kobject 的 parent 的 `sd`**，不是 kernfs 的父节点查找——这保证 kobject 树与 sysfs 目录树同构。`priv = kobj` 是 sysfs 全部魔法的来源（见 kernfs 章 §8.1）。`0755` 是目录权限，uid/gid 默认 `GLOBAL_ROOT_UID/GID`，可被 `ktype->get_ownership` 覆盖（`kobject.c:50-57`），容器场景用它把 `/sys/devices/...` 的属主映射到容器 root。

### 6.2 属性文件如何找到 kobject

```c
/* fs/sysfs/file.c:22-39（节选） */
static struct kobject *sysfs_file_kobj(struct kernfs_node *kn)
{
	guard(rcu)();
	return rcu_dereference(kn->__parent)->priv;
}

static const struct sysfs_ops *sysfs_file_ops(struct kernfs_node *kn)
{
	struct kobject *kobj = sysfs_file_kobj(kn);
	...
	return kobj->ktype ? kobj->ktype->sysfs_ops : NULL;
}
```

文件节点的 `priv` 是 `struct attribute`，kobject 则从**父目录**取。完整读路径：

```text
read(2) → kernfs_fop_read_iter → seq_read_iter → kernfs_seq_show
 → sysfs_kf_seq_show(of)                      fs/sysfs/file.c:46
     ├ kobj = sysfs_file_kobj(of->kn)          ← 父目录的 priv
     ├ ops  = sysfs_file_ops(of->kn)           ← kobj->ktype->sysfs_ops
     └ ops->show(kobj, of->kn->priv, buf)      ← priv 是 attribute*
          → kobj_attr_show → container_of → kattr->show(kobj, kattr, buf)
```

写路径经 `sysfs_kf_write()`（`file.c:136-146`）→ `ops->store()`。创建文件时若 `ktype->sysfs_ops` 为空，`sysfs_add_file_mode_ns()` 直接 `WARN("missing sysfs attribute operations for kobject")` 并返回 `-EINVAL`——所以"有 `sd` 没有 `sysfs_ops`"时目录能建、文件建不了；裸 `dynamic_kobj` 之所以能用，就是因为它的 ktype 带了 `kobj_sysfs_ops`。

### 6.3 删除、改名、移动的另一半

| kobject 操作 | sysfs 函数 | kernfs 最终动作 |
|--------------|-----------|------------------|
| `create_dir()` | `sysfs_create_dir_ns` | `kernfs_create_dir_ns` |
| `__kobject_del` | `sysfs_remove_groups` + `sysfs_remove_dir` | `kernfs_remove` |
| `kobject_rename` | `sysfs_rename_dir_ns` | `kernfs_rename_ns` |
| `kobject_move` | `sysfs_move_dir_ns` | `kernfs_rename_ns(new_parent)` |

`sysfs_remove_dir()`（`fs/sysfs/dir.c:79-103`）在 `sysfs_symlink_target_lock` 下把 `kobj->sd = NULL` 再 `kernfs_remove(kn)`。那把锁存在的原因是：kobject 可能被别人当作 symlink 目标却没被"拥有"，删除方无法阻止对方并发解引用 `sd`；锁保证 symlink 代码读到的 `sd` 要么有效要么是 `NULL`。`kobject_cleanup()` 里若 `state_in_sysfs` 仍为 1 会自动 `__kobject_del()`（`:677-680`），这是"忘了 del 也不留空目录"的兜底，但若还有子对象就会变成孤儿，所以**仍应在 put 之前显式 del**。

### 6.4 与 kernfs 的分界

kobject 层只关心"有没有目录、名字是什么、谁当父"；kernfs 才关心 ns 查找键、激活/去激活、`active` 引用、drain 与 RCU 释放。分辨方法：出现 `kobj->sd`、`sysfs_create_dir_ns`、`sysfs_remove_dir` 就是 kobject 层；出现 `kernfs_node`、`kernfs_rwsem`、`KN_DEACTIVATED_BIAS` 就是 kernfs 层。目录树的删除顺序、`kernfs_remove_self()` 的 `SUICIDAL` 仲裁、open file 的延迟释放见 [kernfs 专章](../../05-filesystems/kernfs/01-kernfs.md) §3；`/dev` 节点的生成（`devtmpfs_create_node()` 在 `device_add()` 里被调）见 [devtmpfs](../../05-filesystems/special/08-devtmpfs.md)。

---

## 7. uevent

### 7.1 action 字符串表

```c
/* lib/kobject_uevent.c:49-59 */
/* the strings here must match the enum in include/linux/kobject.h */
static const char *kobject_actions[] = {
	[KOBJ_ADD] =		"add",
	[KOBJ_REMOVE] =		"remove",
	[KOBJ_CHANGE] =		"change",
	[KOBJ_MOVE] =		"move",
	[KOBJ_ONLINE] =	"online",
	[KOBJ_OFFLINE] =	"offline",
	[KOBJ_BIND] =		"bind",
	[KOBJ_UNBIND] =	"unbind",
};
```

`bind`/`unbind` 是后期新增的（驱动绑定通知）。头文件注释特别警告：不要为子系统私有语义新增 action，应该用 `KOBJ_CHANGE` 加自定义环境变量（`kobject.h:43-52`）。

### 7.2 kobject_uevent_env 全流程

```text
kobject_uevent(kobj, action)
 └ kobject_uevent_env(kobj, action, NULL)          lib/kobject_uevent.c:476
    ├ 若 action==KOBJ_REMOVE：state_remove_uevent_sent = 1（无论成败）
    ├ 向上找 kset：while (!top->kset && top->parent) top = top->parent
    │    └ 找不到 → pr_debug + return -EINVAL
    ├ uevent_suppress → return 0
    ├ uevent_ops->filter()==0 → return 0
    ├ subsystem = uevent_ops->name(kobj) ?: kobject_name(&kset->kobj)
    ├ env = kzalloc_obj(struct kobj_uevent_env)
    ├ devpath = kobject_get_path(kobj, GFP_KERNEL)      → NULL 则 -ENOENT
    ├ add_uevent_var: ACTION / DEVPATH / SUBSYSTEM
    ├ 逐条 add_uevent_var(envp_ext[i])
    ├ uevent_ops->uevent(kobj, env)                     ← kset 加工
    ├ KOBJ_ADD → state_add_uevent_sent = 1
    │ KOBJ_UNBIND → zap_modalias_env(env)               ← 抹掉 MODALIAS
    ├ SEQNUM=atomic64_inc_return(&uevent_seqnum)
    ├ kobject_uevent_net_broadcast(kobj, env, action_string, devpath)
    └ [CONFIG_UEVENT_HELPER] uevent_helper 非空 → call_usermodehelper
```

四个容易忽略的语义：**filter 返回 0 不是错误**，函数返回 0（`dev_uevent_filter()` 只放行属于 `device_ktype` 且有 bus 或 class 的对象，`core.c:2669-2681`）；**subsystem 名来自 kset 而非 kobj**，`devices_kset` 的 `uevent_ops->name` 是 `dev_uevent_name()`，返回 `bus->name` 或 `class->name`，所以同一条 `/sys/devices/...` 路径能按子系统分发；**`state_remove_uevent_sent` 在入口就置位**，即使后面失败也不再重发（某些子系统不希望自动清理再补一次 remove）；环境变量顺序固定为 ACTION、DEVPATH、SUBSYSTEM，再是调用者的、kset 加工的，最后 SEQNUM。

### 7.3 netlink 广播

```text
kobject_uevent_net_broadcast()                     lib/kobject_uevent.c:382（static）
 ├ ops = kobj_ns_ops(kobj)，为 NULL 时再看 kset 的父
 ├ ops->type == KOBJ_NS_TYPE_NET 且 kobj->ktype->namespace
 │    → ns = kobj->ktype->namespace(kobj)
 ├ ns == NULL → uevent_net_broadcast_untagged()
 │                └ 遍历全局 uevent_sock_list（仅 init_user_ns 的 socket）
 └ ns != NULL → container_of(ns, struct net, ns)
                  → uevent_net_broadcast_tagged(net->uevent_sock->sk, ...)
```

报文头部是 `sprintf(scratch, "%s@%s", action_string, devpath)`，后面紧跟 `env->buf`（`alloc_uevent_skb()`，`:278-306`）；`NETLINK_CB` 里 `portid=0`、`dst_group=1`，告诉用户态"来自内核、发给 group 1"。`ENOBUFS`/`-ESRCH` 被吞掉转成 0，注释说"缓冲区溢出由用户态处理"。

每个 netns 在 `uevent_net_init()`（`:773-803`）里创建自己的 `NETLINK_KOBJECT_UEVENT` socket，经 `net->uevent_sock` 访问；只有 `init_user_ns` 的 socket 会被挂进全局 `uevent_sock_list` 参与 untagged 广播（`:796-800`）——这就是容器里 `udev` 收不到别的 netns 事件的原因。

### 7.4 uevent_helper

`CONFIG_UEVENT_HELPER`（`.config:1478` 为 not set）控制老式 `/sbin/hotplug`：源码在 `kobject_uevent.c:234-275`（`kobj_usermode_filter`/`init_uevent_argv`）、`:614-639`（调用点）、`:833-851`（`/sys/kernel/hotplug` sysctl，`postcore_initcall`）。**本树未启用，`/sys/kernel/hotplug` 不存在，`uevent_helper[]` 数组也不编译**。启用时它只应在早期启动阶段使用，且 `kobj_usermode_filter()` 会跳过非初始 ns 的对象。

### 7.5 synth uevent

用户态往对象的 `uevent` 文件写 `action [uuid key=value ...]` 就能"重放"事件，入口 `kobject_synth_uevent()`（`:191-232`）：`kobject_action_type()` 查表匹配 action（用 `strncmp` + 检查 `'\0'` 做整词匹配，所以 `addx` 不匹配 `add`）；无参数时带 `SYNTH_UUID=0` 直接发；有参数时 `kobject_action_args()` 解析 UUID 与 `KEY=VALUE`，生成 `SYNTH_UUID=<uuid>` 与 `SYNTH_ARG_<key>=<value>` 后调用 `kobject_uevent_env()`。失败时 `kobject_get_path()` 只为打一行警告（`:224-230`）。注意本树**没有** `kobject_synthetic_uevent()` 这个名字，只有 `kobject_synth_uevent()`；也没有导出的 `kobject_uevent_init()`——它是 `static int __init`，在 `CONFIG_NET` 下注册 pernet 子系统（`:824-831`）。

### 7.6 uevent_suppress

`uevent_suppress` 是唯一由使用者直接写的状态位，典型用法是网卡注册过程中先抑制、配置完成后统一放行。检查点在 kset 解析**之后**、filter **之前**（`:514-520`），返回 0 而非错误——对调用者表现为"成功但没发"。

---

## 8. 命名空间

### 8.1 类型表与注册

```c
/* lib/kobject.c:1021-1045（节选） */
static DEFINE_SPINLOCK(kobj_ns_type_lock);
static const struct kobj_ns_type_operations *kobj_ns_ops_tbl[KOBJ_NS_TYPES];

int kobj_ns_type_register(const struct kobj_ns_type_operations *ops)
{
	enum kobj_ns_type type = ops->type;

	spin_lock(&kobj_ns_type_lock);
	error = -EINVAL;
	if (!kobj_ns_type_is_valid(type))
		goto out;
	error = -EBUSY;
	if (kobj_ns_ops_tbl[type])
		goto out;
	error = 0;
	kobj_ns_ops_tbl[type] = ops;
out:
	spin_unlock(&kobj_ns_type_lock);
	return error;
}
```

表只有 `KOBJ_NS_TYPES == 2` 项（NONE + NET），同一类型只能注册一次（第二次 `-EBUSY`）。net ns 的实现是 `net_ns_type_operations`（`net/core/net-sysfs.c:2187-2195`），在 `netdev_kobject_init()` 里 `kobj_ns_type_register()` 后 `class_register(&net_class)`（`net-sysfs.c:2411-2415`）。

### 8.2 取 tag 的两段式

```c
/* lib/kobject.c:30-38 */
const struct ns_common *kobject_namespace(const struct kobject *kobj)
{
	const struct kobj_ns_type_operations *ns_ops = kobj_ns_ops(kobj);

	if (!ns_ops || ns_ops->type == KOBJ_NS_TYPE_NONE)
		return NULL;

	return kobj->ktype->namespace(kobj);
}
```

kobject 的 ns tag **不是自己的属性，而是父对象的声明 + 自己的回调**：`kobj_ns_ops(kobj)` 实际就是 `kobj_child_ns_ops(kobj->parent)`（`:1059-1072`），即"问父对象：你的子对象带哪种 ns"，答案来自 `parent->ktype->child_ns_type()`。链路是：

```text
kobject_namespace(child)
 ← kobj_ns_ops(child) = kobj_child_ns_ops(parent)
      ← parent->ktype->child_ns_type(parent)   → 得到 ops（校验 type）
 ← child->ktype->namespace(child)               → 得到 ns_common*
```

`create_dir()` 则反过来用 `kobj_child_ns_ops(kobj)` 判断"我的子目录要不要开 ns 过滤"并 `sysfs_enable_ns(kobj->sd)`（`kobject.c:95-101`），两条 `BUG_ON` 分别校验类型合法与已注册——ns 类型必须先注册再挂使用者。

### 8.3 net ns 实例

`struct class` 的 `ns_type` 与 `namespace` 必须成对，`class_register()` 入口即检查（`drivers/base/class.c:186-195`）：只给其一就 `pr_err` 并返回 `-EINVAL`。`net_class`（`net/core/net-sysfs.c:2249-2257`）同时提供 `.ns_type = &net_ns_type_operations`、`.namespace = net_namespace`、`.get_ownership = net_get_ownership`，于是网络设备的 sysfs 目录在非初始 netns 下带 ns tag：同名 `eth0` 可存在于不同 netns，kernfs 用 `(name, ns)` 二元组做查找键，互不冲突。`class_dir_ktype` 的 `child_ns_type` 用来把这个信息传给 class 目录下的设备（`core.c:3272-3283`）：

```c
static const struct kobj_ns_type_operations *
class_dir_child_ns_type(const struct kobject *kobj)
{
	const struct class_dir *dir = to_class_dir(kobj);
	return dir->class->ns_type;
}
```

### 8.4 ns 在 uevent 上的效果

`kobject_uevent_net_broadcast()` 只对 `KOBJ_NS_TYPE_NET` 做定向（`kobject_uevent.c:401-417`），注释直言 "kobjects currently only carry network namespace tags"；其余类型即使将来加了也不影响广播目标。`CONFIG_UEVENT_HELPER` 路径下，非初始 ns 的对象会被 `kobj_usermode_filter()` 拦掉（`:235-249`）。

---

## 9. 在 device 模型中的嵌入

### 9.1 struct device 的第一个字段

`kobj` 是 `struct device` 的首字段（`include/linux/device.h:703-710`），`kobj_to_dev()` 用 `container_of_const` 反查（`device.h:863`）。`dev_name()` 仍优先返回 `init_name`：

```c
/* include/linux/device.h:931-937 */
static inline const char *dev_name(const struct device *dev)
{
	/* Use the init name until the kobject becomes available */
	if (dev->init_name)
		return dev->init_name;

	return kobject_name(&dev->kobj);
}
```

`init_name` 在 `device_add()` 里被 `dev_set_name()` 消费后立即置 `NULL`（`core.c:3663-3666`），之后名字只来自 kobject——这是为兼容"静态分配、编译期给名字"的老驱动。

### 9.2 initialize / add / del 的生命周期对应

| device 侧 | kobject 侧 | 说明 |
|-----------|-----------|------|
| `device_initialize()` | `kobj.kset = devices_kset; kobject_init(&kobj, &device_ktype)` | kref=1，调用者持有 |
| `device_add()` 开头 | `dev_set_name()` → `kobject_add(&kobj, kobj.parent, NULL)` | 名字与 parent 在 add 前定好 |
| `device_add()` 末尾 | `kobject_uevent(&kobj, KOBJ_ADD)` | 属性/链接都建完才发 |
| `device_del()` | `kobject_uevent(KOBJ_REMOVE)` → `kobject_del(&kobj)` | 先发事件再摘目录 |
| `device_unregister()` | `device_del()` + `put_device()` | put 到 0 才 `device_release` |
| `get_device()/put_device()` | `kobject_get()/kobject_put()` | 纯转发 |

```c
/* drivers/base/core.c:3227-3230 */
void device_initialize(struct device *dev)
{
	dev->kobj.kset = devices_kset;
	kobject_init(&dev->kobj, &device_ktype);
```

`kobj.kset` 在 `kobject_init()` **之前**赋值，而 `kobj.parent` 要到 `device_add()` 里由 `get_device_parent()` 算出（`core.c:3680-3687`）。后者的决策：有 class 且无 parent → `/sys/devices/virtual`；父设备有 class 且无 ns → 直接用父的 kobject；否则在 class 下建 glue dir（`core.c:3310-3400`）。这就解释了 `/sys/devices/virtual/net/...` 与 `/sys/devices/pci.../...` 两种布局。

### 9.3 device_ktype 的四个回调

```c
/* drivers/base/core.c:2661-2666 */
static const struct kobj_type device_ktype = {
	.release	= device_release,
	.sysfs_ops	= &dev_sysfs_ops,
	.namespace	= device_namespace,
	.get_ownership	= device_get_ownership,
};
```

`device_release()`（`:2612-2641`）是 kref 归零后的最终释放：先 `devres_release_all()`，再按 `dev->release` → `dev->type->release` → `dev->class->dev_release` 三级回退，全无就 `WARN`，最后 `kfree(p)` 释放 `device_private`。**`kfree(dev)` 不在这里**——由 device 自身的 release 回调决定。之所以要三级回退，是因为历史上不同子系统用不同方式注册 device，缺回调就是必现泄漏，所以用 `WARN` 而非静默。`dev_sysfs_ops`（`:2511`）把 `attribute*` 转成 `device_attribute` 并支持 `store_const`；`device_namespace()`/`device_get_ownership()` 分别转发到 `class->namespace`/`class->get_ownership`。

### 9.4 class / bus 的 kset 角色

| 全局 kset | 创建点 | 作用 |
|-----------|--------|------|
| `devices_kset`（`/sys/devices`） | `kset_create_and_add("devices", &device_uevent_ops, NULL)`，core.c:4228 | 所有 device 的 `kobj.kset` |
| `bus_kset`（`/sys/bus`） | `kset_create_and_add("bus", &bus_uevent_ops, NULL)`，bus.c:1480 | 所有 bus 子系统 |
| `class_kset`（`/sys/class`） | `kset_create_and_add("class", NULL, NULL)`，class.c:650 | 所有 class 子系统 |
| `system_kset`（`/sys/devices/system`） | bus.c:1484 | CPU/memory/node |

每个 `bus_type`/`class` 的注册状态藏在 `subsys_private`（`drivers/base/base.h:43-60`）：`subsys`（自己的 kset）、`devices_kset`、`drivers_kset`、`glue_dirs`。`class_register()` 是典型写法（`class.c:197-224`）：`kset_init(&cp->glue_dirs)` → `kobject_set_name(&cp->subsys.kobj, "%s", cls->name)` → 手工设 `cp->subsys.kobj.kset = class_kset` 与 `ktype = &class_ktype` → `kset_register(&cp->subsys)`。`kset_register()`（`kobject.c:857-880`）内部只做 `kset_init()` + `kobject_add_internal()` + `kobject_uevent(KOBJ_ADD)`，并拒绝没有 ktype 的 kset（`:864-867`）——所以 kset 的 `kobj.ktype` 必须在注册前由使用者指定。

### 9.5 glue dir 的引用计数陷阱

class 设备的中间目录是"kobject 引用计数 + kernfs 节点引用"两套生命周期交织的经典案例。`cleanup_glue_dir()`（`core.c:3423-3485`）的注释用一张并发图说明：CPU1 删最后一个子设备、CPU2 同时往 glue dir 加设备，两者对 `sd` 的 `sysfs_put()` 与 `kernfs_new_node()` 交错，会用到已释放的父节点。解法是只在"没有子目录且 kref 归 1"时才删：

```c
	ref = kref_read(&glue_dir->kref);
	if (!kobject_has_children(glue_dir) && !--ref)
		kobject_del(glue_dir);
	kobject_put(glue_dir);
```

`kobject_has_children()`（`:3411-3416`）判断 `kobj->sd->dir.subdirs`（kernfs 层还有没有子目录），并 `WARN_ON_ONCE(kref_read()==0)`。这段代码直接读写 `kref` 与 `sd` 内部，属于 driver core 的特权用法，普通驱动不要模仿。

---

## 10. 常见错误与调试

| 症状 | 根因 | 排查 |
|------|------|------|
| sysfs 目录在、`kfree()` 后随机崩 | 绕过 `kobject_put()` 直接释放 | 搜 `kobject_init` 与释放是否配对 |
| 内存只涨不降 | `ktype->release` 缺失 | cleanup 的 `pr_debug`；device 侧看 `WARN ... does not have a release()` |
| `-EEXIST`，目录建不出来 | 同名注册到同一父 | `sysfs_warn_dup()` 打印父路径 + `dump_stack()` |
| `kobject_get() ... is not initialized` | 未 init 就 add/get | 检查 `kobject_init()` 是否被跳过 |
| `tried to init an initialized object` | 重复 init，kref 被重置 | 调用点是否跑了两遍（probe 重入） |
| `attempted to be registered with empty name` | `fmt` 为 NULL 且名字未设 | `dev_set_name()` 失败是否被忽略 |
| 目录名出现 `!` | 名字含 `/` 被替换 | `kobject_set_name_vargs()` |
| 事件发了但收不到 | 无 kset / 被 filter / 在别的 netns | `kobject_uevent_env` 的 `pr_debug` |
| `uevent_suppress` 后事件全无 | 使用者置位未清 | 检查网卡驱动的 suppress 逻辑 |
| 删了目录但文件仍可读写 | kernfs active 保护延迟移除 | 见 kernfs 章 §7.4、`sysfs_break_active_protection()` |
| `-ENOENT` 但 parent 正常 | `parent->sd == NULL`（父未注册） | `fs/sysfs/dir.c:49-55` |

几条独立说明：`name` 的生命周期归 kobject，任何 `kobj->name = buf` 式直接赋值都是 bug（cleanup 会 `kfree_const()` 它），名字只能经 `kobject_set_name*()` 设置；`kobject_del()` 之后对象仍可 `get`/`put`，但 `sd` 已 NULL、`parent` 已清空，此时发 uevent 会因找不到 kset 返回 `-EINVAL`，重建 sysfs 只能重新 add（不推荐，会与旧引用竞态）；`kobject_add()` 失败后不要跳过 `put`，也不要 `kfree`；`kset_register()` 失败会顺手 `kfree_const(k->kobj.name)` 并把 name 置 NULL（`kobject.c:871-876`），调用者不能再访问这个名字；`class_register()` 的 err 路径因此只释放 `cp` 本身（外加注销 lockdep key），不去碰已被清空的名字。

观测点：

```text
ls -l /sys/devices/.../            # 没有 sd 的对象不会出现
cat /sys/class/net/eth0/uevent     # 读 uevent 文件会触发一次 synth 事件
udevadm monitor --kernel --property # 与 udev 争抢同一个 netlink 组
grep -i kobject .config            # 本树两套 DEBUG_KOBJECT 均未开
cat /proc/slabinfo | grep -E 'kernfs_node_cache|kmalloc-'
```

本树 `CONFIG_DEBUG_KOBJECT` 与 `CONFIG_DEBUG_KOBJECT_RELEASE` 均未设置，延迟释放与相关断言不可用；能用的只有 `pr_debug`（需 dynamic debug 打开 `lib/kobject.c`、`lib/kobject_uevent.c`）以及 `sysfs_warn_dup()` 自带的 `dump_stack()`。

---

## 11. 关键文件清单

| 文件 | 关键内容 |
|------|----------|
| `include/linux/kobject.h` | `struct kobject`/`kset`/`kobj_type`/`kobj_attribute`、action 枚举、全局 `*_kobj` |
| `include/linux/kref.h` | `kref_init/get/put/put_mutex/put_lock/get_unless_zero/read`（全内联，无 `.c`） |
| `include/linux/kobject_ns.h` | `enum kobj_ns_type`、`struct kobj_ns_type_operations` |
| `lib/kobject.c` | init/add/del、rename/move、get/put、cleanup/release、kset、ns 表、路径 |
| `lib/kobject_uevent.c` | action 表、`kobject_uevent_env`、netlink 广播、synth、per-net socket |
| `fs/sysfs/dir.c` | `sysfs_create_dir_ns`/`remove_dir`/`rename_dir_ns`/`move_dir_ns` |
| `fs/sysfs/file.c` | `sysfs_file_kobj`、`sysfs_kf_seq_show/read/write`、`sysfs_add_file_mode_ns` |
| `fs/sysfs/group.c` | `sysfs_create_groups`（`default_groups` 的执行者） |
| `drivers/base/core.c` | `device_initialize/add/del`、`device_ktype`、`dev_uevent*`、`devices_kset` |
| `drivers/base/class.c` / `bus.c` | `class_register`/`class_kset`/`class_ktype`、`bus_kset`、`subsys_private` |
| `kernel/ksysfs.c` | `kernel_kobj = kobject_create_and_add("kernel", NULL)` |
| `samples/kobject/*.c` | 最小 kobject / kset 用法 |

---

## 12. 延伸阅读

- [driver-model](01-driver-model.md) —— `struct device`、bus/driver/class 与 probe 全流程；本文 §9 是其 kobject 侧对偶。
- [platform-bus](02-platform-bus.md) —— platform bus 如何经 `device_register()`/`driver_register()` 落到 kobject 树。
- [sysfs 专章](../../05-filesystems/special/02-sysfs.md) —— §2 数据结构、§3 API 用法、§4 调用栈，偏"如何写属性文件"。
- [kernfs 专章](../../05-filesystems/kernfs/01-kernfs.md) —— `kernfs_node` 生命周期、`kernfs_ops`、`active` 保护与 ns 查找键；本文 §6 的下一层。
- [devtmpfs](../../05-filesystems/special/08-devtmpfs.md) —— `device_add()` 里 `devtmpfs_create_node()` 如何生成 `/dev` 节点。
