# mount_setattr 系统调用

> 所属分类：文件系统挂载与结构 | [ARM64 系统调用表](../arm64-syscall-table.md#5-文件系统挂载与结构)

## 1. 概述

`mount_setattr` 在已挂载的挂载点上修改**挂载属性**，或改变挂载传播类型，并可选择递归作用到整棵子树。它补充了 `mount(MS_REMOUNT|MS_BIND)` 无法表达的属性（如 idmapped mount、`nosymfollow`），也是新 mount API 中唯一能修改已有挂载属性的入口。

- ARM64 编号 **442**（`include/uapi/asm-generic/unistd.h:793`）。
- 属性以 `attr_set` / `attr_clr` 两个位图表达"置位"与"清位"。
- `MOUNT_ATTR_IDMAP` 需要额外提供 user namespace fd，把挂载转换为 idmapped mount。
- `propagation` 字段用于 `MS_SHARED` / `MS_SLAVE` / `MS_PRIVATE` / `MS_UNBINDABLE`，与 `attr_*` 互不影响。
- 相关：创建挂载时可用 [`fsmount`](./fsmount.md) 的 `attr_flags` 直接指定一部分属性。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE5(mount_setattr, int, dfd, const char __user *, path,
		unsigned int, flags, struct mount_attr __user *, uattr,
		size_t, usize)
{
	int err;
	struct path target;
	struct mount_kattr kattr;
	unsigned int lookup_flags = LOOKUP_AUTOMOUNT | LOOKUP_FOLLOW;

	if (flags & ~(AT_EMPTY_PATH |
		      AT_RECURSIVE |
		      AT_SYMLINK_NOFOLLOW |
		      AT_NO_AUTOMOUNT))
		return -EINVAL;

	if (flags & AT_NO_AUTOMOUNT)
		lookup_flags &= ~LOOKUP_AUTOMOUNT;
	if (flags & AT_SYMLINK_NOFOLLOW)
		lookup_flags &= ~LOOKUP_FOLLOW;

	kattr = (struct mount_kattr) {
		.lookup_flags	= lookup_flags,
	};

	if (flags & AT_RECURSIVE)
		kattr.kflags |= MOUNT_KATTR_RECURSE;
	...
}
```

（`fs/namespace.c:5153`）

用户态原型：

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <sys/syscall.h>
int mount_setattr(int dfd, const char *path, unsigned int flags,
                  struct mount_attr *attr, size_t size);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `dfd` | `int` | 目录 fd 或 `AT_FDCWD` |
| `path` | `const char *` | 挂载点路径；`AT_EMPTY_PATH` 时可为空串 |
| `flags` | `unsigned int` | `AT_EMPTY_PATH`、`AT_RECURSIVE`、`AT_SYMLINK_NOFOLLOW`、`AT_NO_AUTOMOUNT` |
| `uattr` | `struct mount_attr *` | 属性描述 |
| `usize` | `size_t` | 结构体大小，必须 ≥ `MOUNT_ATTR_SIZE_VER0`（32） |

`struct mount_attr`（`include/uapi/linux/mount.h:133`）：

```c
struct mount_attr {
	__u64 attr_set;		/* 要置位的属性 */
	__u64 attr_clr;		/* 要清除的属性 */
	__u64 propagation;	/* 传播类型，见 MS_* */
	__u64 userns_fd;	/* MOUNT_ATTR_IDMAP 使用的 user ns fd */
};
#define MOUNT_ATTR_SIZE_VER0	32
```

可设置属性（`include/uapi/linux/mount.h:118`）：

| 属性 | 值 | 说明 |
| --- | --- | --- |
| `MOUNT_ATTR_RDONLY` | 0x00000001 | 只读 |
| `MOUNT_ATTR_NOSUID` | 0x00000002 | 忽略 suid/sgid |
| `MOUNT_ATTR_NODEV` | 0x00000004 | 禁止设备文件 |
| `MOUNT_ATTR_NOEXEC` | 0x00000008 | 禁止执行 |
| `MOUNT_ATTR__ATIME` | 0x00000070 | atime 策略掩码 |
| `MOUNT_ATTR_RELATIME` | 0x00000000 | 相对 atime |
| `MOUNT_ATTR_NOATIME` | 0x00000010 | 不更新 atime |
| `MOUNT_ATTR_STRICTATIME` | 0x00000020 | 严格 atime |
| `MOUNT_ATTR_NODIRATIME` | 0x00000080 | 不更新目录 atime |
| `MOUNT_ATTR_IDMAP` | 0x00100000 | idmapped mount |
| `MOUNT_ATTR_NOSYMFOLLOW` | 0x00200000 | 不跟随符号链接 |

## 3. 内核实现路径

```
mount_setattr(dfd, path, flags, uattr, usize) fs/namespace.c:5153
├─ flags & ~(AT_EMPTY_PATH|AT_RECURSIVE|AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT) → -EINVAL
├─ 调整 lookup_flags
├─ AT_RECURSIVE → kattr.kflags |= MOUNT_KATTR_RECURSE
├─ wants_mount_setattr(uattr, usize, &kattr)     fs/namespace.c:5120
│    ├─ usize > PAGE_SIZE → -E2BIG
│    ├─ usize < MOUNT_ATTR_SIZE_VER0 → -EINVAL
│    ├─ !may_mount() → -EPERM
│    ├─ copy_struct_from_user(&attr, ...)
│    ├─ 三个字段全 0 → 返回 0（nop，不查找路径）
│    └─ build_mount_kattr(&attr, usize, &kattr)  fs/namespace.c:5056
│         ├─ propagation 只允许 MS_SHARED|MS_SLAVE|MS_PRIVATE|MS_UNBINDABLE
│         │  且至多一个
│         ├─ attr_set|attr_clr 只允许 MOUNT_SETATTR_VALID_FLAGS
│         ├─ attr_flags_to_mnt_flags() 转成 MNT_*
│         ├─ atime 枚举规则校验
│         └─ build_mount_idmapped()                fs/namespace.c:4995
├─ filename_lookup(dfd, path, kattr.lookup_flags, &target)
├─ do_mount_setattr(&target, &kattr)              fs/namespace.c:4936
└─ finish_mount_kattr(&kattr)                     // put_user_ns/mnt_idmap_put
```

### 3.1 属性合法性

`build_mount_kattr()` 的关键约束：

```c
if (attr->propagation & ~MOUNT_SETATTR_PROPAGATION_FLAGS)
	return -EINVAL;
if (hweight32(attr->propagation & MOUNT_SETATTR_PROPAGATION_FLAGS) > 1)
	return -EINVAL;

if ((attr->attr_set | attr->attr_clr) & ~MOUNT_SETATTR_VALID_FLAGS)
	return -EINVAL;
```

`MOUNT_SETATTR_VALID_FLAGS` = `FSMOUNT_VALID_FLAGS | MOUNT_ATTR_IDMAP`，即比 `fsmount` 多一个 idmap。

atime 是枚举而非位图，规则更严格：

- 设置 atime 时必须在 `attr_clr` 中**完整**给出 `MOUNT_ATTR__ATIME`（0x70），不能只清部分位。
- 清除 `MOUNT_ATTR__ATIME` 时先清 `MNT_RELATIME | MNT_NOATIME`，再按 `attr_set & MOUNT_ATTR__ATIME` 选择 `RELATIME`/`NOATIME`/`STRICTATIME`。
- `attr_clr` 未包含 `MOUNT_ATTR__ATIME` 时，`attr_set` 不得含任何 atime 位。

### 3.2 idmapped mount

`build_mount_idmapped()`（`fs/namespace.c:4995`）：

1. `userns_fd > INT_MAX` → `-EINVAL`。
2. 用 `CLASS(fd, f)` 取 fd，必须 `proc_ns_file()` 且 `ns_type == CLONE_NEWUSER`，否则 `-EINVAL`。
3. 不允许 `init_user_ns`，否则 `-EPERM`。
4. 需要对目标 user namespace 有 `CAP_SYS_ADMIN`。
5. `attr_clr & MOUNT_ATTR_IDMAP` 只能在 `MOUNT_KATTR_IDMAP_REPLACE` 下使用（`open_tree(OPEN_TREE_CLONE)` + `open_tree_attr` 的克隆场景），且清除等价于设置 `nop_mnt_idmap`。

`can_idmap_mount()`（`fs/namespace.c:4807`）进一步要求：文件系统声明 `FS_ALLOW_IDMAP`、未设置 `SB_I_NOIDMAP`、挂载处于匿名命名空间（尚未暴露给用户），并且不能使用 sb 自身的 `s_user_ns` 作为 idmap。

### 3.3 两阶段提交

`do_mount_setattr()` 在 `lock_mount_hash()` 下执行：

```
mount_setattr_prepare()   // 只检查并 mnt_hold_writers()
   ├─ can_change_locked_flags()：不能清除被 MNT_LOCK_* 锁定的属性
   ├─ can_idmap_mount()
   └─ 需要时 mnt_hold_writers() 阻塞新写者
mount_setattr_commit()    // 不会失败
   ├─ do_idmap_mount()
   ├─ recalc_flags() = (mnt_flags & ~attr_clr) | attr_set
   ├─ WRITE_ONCE(mnt->mnt.mnt_flags, flags)
   ├─ mnt_unhold_writers()
   └─ propagation 变化时 change_mnt_propagation()
```

准备阶段失败会撤销已持有的 writer 计数；传播类型为 `MS_SHARED` 时，先在 `namespace_lock()` 下调用 `invent_group_ids()` 分配组 ID，失败则 `cleanup_group_ids()`。

## 4. 核心数据结构

```c
/* fs/namespace.c:106 */
struct mount_kattr {
	unsigned int attr_set;		/* 已转换为 MNT_* */
	unsigned int attr_clr;
	unsigned int propagation;
	unsigned int lookup_flags;
	enum mount_kattr_flags_t kflags;	/* RECURSE / IDMAP_REPLACE */
	struct user_namespace *mnt_userns;
	struct mnt_idmap *mnt_idmap;
};

/* fs/namespace.c:101 */
enum mount_kattr_flags_t {
	MOUNT_KATTR_RECURSE		= (1 << 0),
	MOUNT_KATTR_IDMAP_REPLACE	= (1 << 1),
};

/* include/linux/mount.h:58 */
struct vfsmount {
	struct dentry *mnt_root;
	struct super_block *mnt_sb;
	int mnt_flags;			/* 被 recalc_flags() 更新 */
	struct mnt_idmap *mnt_idmap;	/* 被 do_idmap_mount() 替换 */
};
```

锁定属性位（`include/linux/mount.h` 的 `MNT_LOCK_*`）表示该挂载在创建时被父命名空间"锁定"，`can_change_locked_flags()`（`fs/namespace.c:3257`）拒绝对应的清除操作，防止容器突破限制。

## 5. 关键流程

```
mount_setattr(AT_FDCWD, "/mnt", AT_RECURSIVE,
              { .attr_set = MOUNT_ATTR_RDONLY|MOUNT_ATTR_NOSYMFOLLOW,
                .attr_clr = 0,
                .propagation = 0 }, sizeof(struct mount_attr))
  │
  ├─ 校验 flags、usize
  ├─ copy_struct_from_user
  ├─ build_mount_kattr：位图合法性
  ├─ filename_lookup("/mnt")
  ├─ do_mount_setattr
  │    ├─ path_mounted：必须是挂载根
  │    ├─ kattr.mnt_userns ? alloc_mnt_idmap
  │    ├─ propagation ? namespace_lock
  │    ├─ lock_mount_hash
  │    ├─ check_mnt / anon_ns_root
  │    ├─ mount_setattr_prepare
  │    │    ├─ 逐个挂载检查 MNT_LOCK_*
  │    │    ├─ 只读化或 idmap 时 mnt_hold_writers
  │    │    └─ AT_RECURSIVE 时遍历 next_mnt
  │    └─ mount_setattr_commit
  │         ├─ recalc_flags + WRITE_ONCE
  │         ├─ 递归时逐个子挂载
  │         └─ touch_mnt_namespace
  └─ finish_mount_kattr：释放 userns/idmap 引用
```

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `0` | 属性修改成功；三个字段全 0 时直接成功 |
| `-EPERM` | 缺少 `CAP_SYS_ADMIN`；`can_change_locked_flags()` 拒绝；idmap 的 `init_user_ns` 或权限不足 |
| `-EINVAL` | `flags`/属性位非法；`usize` 太小；atime 组合非法；propagation 多个位；路径不是挂载根；idmap 的 fd 不是 user namespace；文件系统无 `FS_ALLOW_IDMAP` |
| `-E2BIG` | `usize > PAGE_SIZE` |
| `-EBADF` | `userns_fd` 无效 |
| `-ENOMEM` | `alloc_mnt_idmap()`、组 ID 分配失败 |
| `-ENOENT` / `-ENOTDIR` / `-ELOOP` | 路径解析失败 |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/mount.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	struct mount_attr attr = {
		.attr_set = MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSYMFOLLOW,
		.attr_clr = 0,
		.propagation = 0,
		.userns_fd = 0,
	};

	if (syscall(SYS_mount_setattr, AT_FDCWD, "/tmp/demo",
		    AT_RECURSIVE, &attr, sizeof(attr)) < 0) {
		perror("mount_setattr");
		return 1;
	}
	printf("mount attributes updated\n");
	return 0;
}
```

切换 atime 策略（必须同时清 `MOUNT_ATTR__ATIME`）：

```c
struct mount_attr a = {
	.attr_set = MOUNT_ATTR_NOATIME,
	.attr_clr = MOUNT_ATTR__ATIME,
};
syscall(SYS_mount_setattr, AT_FDCWD, "/mnt", 0, &a, sizeof(a));
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/namespace.c` | `struct mount_kattr`（106）、`can_change_locked_flags()`（3257）、`recalc_flags()`（4795）、`can_idmap_mount()`（4807）、`do_mount_setattr()`（4936）、`build_mount_idmapped()`（4995）、`build_mount_kattr()`（5056）、`wants_mount_setattr()`（5120）、`SYSCALL_DEFINE5(mount_setattr)`（5153） |
| `include/uapi/linux/mount.h` | `struct mount_attr`（133）、`MOUNT_ATTR_*`（118） |
| `include/linux/mount.h` | `struct vfsmount`（58）、`MNT_LOCK_*` |
| `fs/mount.h` | `struct mount`、`mnt_hold_writers()` 相关状态 |
| `fs/pnode.c` | `change_mnt_propagation()`、`invent_group_ids()` |
| `include/uapi/asm-generic/unistd.h` | `__NR_mount_setattr`（793） |
