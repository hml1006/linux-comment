# move_mount 系统调用

> 所属分类：文件系统挂载与结构 | [ARM64 系统调用表](../arm64-syscall-table.md#5-文件系统挂载与结构)

## 1. 概述

`move_mount` 把一棵已存在的挂载树从 `from` 位置移动到 `to` 位置。它是新 mount API 的"安装/搬迁"步骤：

- 配合 [`fsmount`](./fsmount.md) 返回的挂载 fd，把新建但尚未接入命名空间的挂载安装到目标目录。
- 配合 `open_tree(OPEN_TREE_CLONE)` 克隆出的挂载树，实现"复制子树 + 重新挂载"。
- 通过 `MOVE_MOUNT_SET_GROUP` 把两个挂载设为同一传播组，通过 `MOVE_MOUNT_BENEATH` 挂到目标挂载之下。
- ARM64 编号 **429**（`include/uapi/asm-generic/unistd.h:767`）。

它替代了 `mount(MS_MOVE)` 的字符串接口，允许源和目的都用 fd 指定，避免路径解析竞态。

## 2. 函数原型与参数

```c
SYSCALL_DEFINE5(move_mount,
		int, from_dfd, const char __user *, from_pathname,
		int, to_dfd, const char __user *, to_pathname,
		unsigned int, flags)
```

（`fs/namespace.c:4585`）

用户态原型：

```c
#define _GNU_SOURCE
#include <linux/mount.h>
#include <sys/syscall.h>
int move_mount(int from_dfd, const char *from_pathname,
               int to_dfd, const char *to_pathname, unsigned int flags);
```

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `from_dfd` | `int` | 源 dirfd；`from_pathname == NULL` 时直接使用该 fd 的 `f_path` |
| `from_pathname` | `const char *` | 源路径；可为 `NULL`（需 `MOVE_MOUNT_F_EMPTY_PATH` 或直接给 fd） |
| `to_dfd` | `int` | 目标 dirfd；`to_pathname == NULL` 时直接使用该 fd 的 `f_path` |
| `to_pathname` | `const char *` | 目标路径；可为 `NULL` |
| `flags` | `unsigned int` | `MOVE_MOUNT_*` |

`include/uapi/linux/mount.h:71` 的 flags：

| 标志 | 值 | 含义 |
| --- | --- | --- |
| `MOVE_MOUNT_F_SYMLINKS` | 0x01 | 源路径跟随符号链接 |
| `MOVE_MOUNT_F_AUTOMOUNTS` | 0x02 | 源路径跟随自动挂载 |
| `MOVE_MOUNT_F_EMPTY_PATH` | 0x04 | 允许源路径为空串 |
| `MOVE_MOUNT_T_SYMLINKS` | 0x10 | 目标路径跟随符号链接 |
| `MOVE_MOUNT_T_AUTOMOUNTS` | 0x20 | 目标路径跟随自动挂载 |
| `MOVE_MOUNT_T_EMPTY_PATH` | 0x40 | 允许目标路径为空串 |
| `MOVE_MOUNT_SET_GROUP` | 0x100 | 只设置共享传播组，不移动 |
| `MOVE_MOUNT_BENEATH` | 0x200 | 把源挂载到目标挂载之下 |
| `MOVE_MOUNT__MASK` | 0x377 | 全部合法位 |

`MOVE_MOUNT_BENEATH` 与 `MOVE_MOUNT_SET_GROUP` 互斥。

## 3. 内核实现路径

```
move_mount(from_dfd, from_name, to_dfd, to_name, flags)  fs/namespace.c:4585
├─ !may_mount() → -EPERM                       fs/namespace.c:2008
├─ flags & ~MOVE_MOUNT__MASK → -EINVAL
├─ BENEATH|SET_GROUP 同时出现 → -EINVAL
├─ 解析 to：
│    ├─ to_name == NULL && to_dfd >= 0 → fd_file(to_dfd)->f_path
│    └─ 否则 filename_lookup(to_dfd, to_name, lflags, &to_path)
├─ 解析 from：
│    ├─ from_name == NULL && from_dfd >= 0
│    │    └─ vfs_move_mount(&fd_file(from_dfd)->f_path, &to_path, mflags)
│    └─ 否则 filename_lookup(...) → vfs_move_mount(&from_path, &to_path, mflags)
└─ vfs_move_mount()                            fs/namespace.c:4561
   ├─ security_move_mount()
   ├─ MNT_TREE_PROPAGATION → do_set_group()    fs/namespace.c:3434
   └─ 否则 → do_move_mount()                   fs/namespace.c:3633
        ├─ path_mounted / 目录类型一致性检查
        ├─ LOCK_MOUNT_MAYBE_BENEATH
        ├─ check_mnt / anon_ns_root / may_use_mount
        ├─ can_move_mount_beneath（BENEATH）
        ├─ tree_contains_unbindable / check_for_nsfs_mounts
        ├─ mount_is_ancestor 防环
        └─ attach_recursive_mnt()              fs/namespace.c:2565
```

`vfs_move_mount()`：

```c
static inline int vfs_move_mount(const struct path *from_path,
				 const struct path *to_path,
				 enum mnt_tree_flags_t mflags)
{
	int ret;

	ret = security_move_mount(from_path, to_path);
	if (ret)
		return ret;

	if (mflags & MNT_TREE_PROPAGATION)
		return do_set_group(from_path, to_path);

	return do_move_mount(from_path, to_path, mflags);
}
```

### 3.1 do_move_mount 的关键检查

- `path_mounted(old_path)`：源路径必须正好是某个挂载的根，否则 `-EINVAL`。
- 源与目标的"目录/非目录"属性必须一致。
- 源在当前命名空间时：必须有父挂载、未被 locked，且父挂载不能是 shared；目标也必须在当前命名空间。
- 源是匿名命名空间根时：目标必须属于本命名空间或可接受的匿名命名空间（`may_use_mount()`），否则 `-EINVAL`。
- `MOVE_MOUNT_BENEATH` 下用 `can_move_mount_beneath()` 检查是否允许形成"下挂"。
- `tree_contains_unbindable()` 阻止把含 unbindable 挂载的树移到 shared 目标下。
- `check_for_nsfs_mounts()` 与 `mount_is_ancestor()` 防止把挂载 namespace 文件或祖先挂载移入自身子树形成环（`-ELOOP`）。

### 3.2 do_set_group

`MOVE_MOUNT_SET_GROUP` 不移动挂载，只修改传播关系（`fs/namespace.c:3434`）：

- 两侧都必须是某个挂载的根。
- 必须在同一超级块上（`from->mnt.mnt_sb == to->mnt.mnt_sb`）。
- `to` 的根必须位于 `from` 的根之下（`is_subdir()`）。
- `to` 必须是 private 挂载，`from` 必须是 shared 或 slave。
- 成功后把 `to` 加入 `from` 的 `mnt_share` 链表或设置 `mnt_master`。

## 4. 核心数据结构

```c
/* fs/mount.h:46 */
struct mount {
	struct hlist_node mnt_hash;
	struct mount *mnt_parent;		/* 父挂载 */
	struct dentry *mnt_mountpoint;		/* 挂载点 dentry */
	struct vfsmount mnt;			/* 通用 VFS 挂载 */
	...
	struct list_head mnt_mounts;		/* 子挂载链表 */
	struct list_head mnt_child;
	struct list_head mnt_share;		/* 共享传播组 */
	struct hlist_head mnt_slave_list;
	struct hlist_node mnt_slave;
	struct mount *mnt_master;
	struct mnt_namespace *mnt_ns;
	struct mountpoint *mnt_mp;
	...
};

/* include/linux/mount.h:58 */
struct vfsmount {
	struct dentry *mnt_root;
	struct super_block *mnt_sb;
	int mnt_flags;
	struct mnt_idmap *mnt_idmap;
};
```

`enum mnt_tree_flags_t` 定义在 `fs/namespace.c:2495`：`MNT_TREE_BENEATH` 与 `MNT_TREE_PROPAGATION` 由 `move_mount` 的 flags 转换而来。

## 5. 关键流程

### 5.1 安装 fsmount 生成的挂载

```
fd = fsopen("ext4", FSOPEN_CLOEXEC)
fsconfig(fd, FSCONFIG_SET_STRING, "source", "/dev/sda1", 0)
fsconfig(fd, FSCONFIG_CMD_CREATE, NULL, NULL, 0)
mfd = fsmount(fd, FSMOUNT_CLOEXEC, 0)        // O_PATH 挂载 fd
move_mount(mfd, "", AT_FDCWD, "/mnt", MOVE_MOUNT_F_EMPTY_PATH)
  │
  ├─ from 路径取自 mfd 的 f_path（fc->root + 新 vfsmount）
  ├─ to 路径解析 "/mnt"
  └─ do_move_mount → attach_recursive_mnt
       ├─ count_mounts：检查 mnt_namespace 容量
       ├─ 目标 shared 时 propagate_mnt
       └─ 把 source 挂到目标 mountpoint
```

`fsmount()` 返回的 fd 带 `FMODE_NEED_UNMOUNT`，`close()` 时若挂载尚未接入命名空间会被自动销毁。

### 5.2 克隆并移动子树

```
tree = open_tree(AT_FDCWD, "/src", OPEN_TREE_CLONE | AT_RECURSIVE)
move_mount(tree, "", AT_FDCWD, "/dst", MOVE_MOUNT_F_EMPTY_PATH)
```

克隆得到的挂载树属于匿名命名空间，`do_move_mount()` 走 `anon_ns_root(old)` 分支。

## 6. 返回值与错误码

| 返回值 | 含义 |
| --- | --- |
| `0` | 移动或设置传播组成功 |
| `-EPERM` | 缺少 `CAP_SYS_ADMIN`，或 LSM 拒绝 |
| `-EINVAL` | flags 非法；源不是挂载根；目录属性不一致；命名空间/传播关系不允许；`SET_GROUP` 与 `BENEATH` 同时设置 |
| `-EBADF` | `from_dfd`/`to_dfd` 指向的 fd 无效 |
| `-ENOENT` / `-ENOTDIR` | 路径不存在或路径分量不是目录 |
| `-ELOOP` | 路径符号链接层数过多，或移动会形成挂载环 |
| `-ENOMEM` | 分配 `mountpoint` 或传播结构失败 |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/mount.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	int fsfd = syscall(SYS_fsopen, "tmpfs", FSOPEN_CLOEXEC);
	if (fsfd < 0) { perror("fsopen"); return 1; }

	if (syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING,
		    "size", "1m", 0) < 0)
		perror("fsconfig size");
	if (syscall(SYS_fsconfig, fsfd, FSCONFIG_CMD_CREATE,
		    NULL, NULL, 0) < 0) {
		perror("fsconfig create");
		return 1;
	}

	int mfd = syscall(SYS_fsmount, fsfd, FSMOUNT_CLOEXEC, 0);
	if (mfd < 0) { perror("fsmount"); return 1; }

	/* 把新挂载安装到 /tmp/demo-mnt */
	if (syscall(SYS_move_mount, mfd, "", AT_FDCWD, "/tmp/demo-mnt",
		    MOVE_MOUNT_F_EMPTY_PATH) < 0) {
		perror("move_mount");
		return 1;
	}

	printf("mounted via move_mount\n");
	close(mfd);
	close(fsfd);
	return 0;
}
```

## 8. 涉及文件

| 路径 | 作用 |
| --- | --- |
| `fs/namespace.c` | `SYSCALL_DEFINE5(move_mount)`（4585）、`vfs_move_mount()`（4561）、`do_move_mount()`（3633）、`do_set_group()`（3434）、`attach_recursive_mnt()`（2565）、`may_mount()`（2008） |
| `fs/pnode.c` / `fs/pnode.h` | 挂载传播、`propagate_mnt()` |
| `fs/mount.h` | `struct mount`、挂载树内部接口 |
| `include/linux/mount.h` | `struct vfsmount` |
| `include/uapi/linux/mount.h` | `MOVE_MOUNT_*`（71-79） |
| `include/uapi/asm-generic/unistd.h` | `__NR_move_mount`（767） |
| `security/security.c` | `security_move_mount()` LSM 钩子 |
