# 扩展属性 (xattr) 系统调用总览

## 1. 扩展属性概述

扩展属性（Extended Attributes, xattr）是一种以 **键值对** 形式附加在文件 inode 上的元数据机制。与 `stat` 的固定字段不同，xattr 可以存储任意名称和值的扩展信息。

### 1.1 命名空间

Linux xattr 使用命名空间前缀来区分不同用途：

| 命名空间 | 前缀 | 用途 | 权限要求 |
|---------|------|------|---------|
| `user` | `user.` | 用户自定义属性 | 普通文件权限 |
| `trusted` | `trusted.` | 可信属性 | `CAP_SYS_ADMIN` |
| `security` | `security.` | 安全模块（SELinux, Smack, IMA） | 内核/LSM 管理 |
| `system` | `system.` | 系统属性（如 POSIX ACL） | 内核管理 |

### 1.2 系统调用分类

所有 xattr 系统调用分为 4 组操作，每组有 4 个变体：

| 操作 | 路径（跟踪符号链接） | 路径（不跟踪） | 文件描述符 | at 系列 |
|-----|-------------------|--------------|-----------|--------|
| **设置** | `setxattr` | `lsetxattr` | `fsetxattr` | `setxattrat` |
| **获取** | `getxattr` | `lgetxattr` | `fgetxattr` | `getxattrat` |
| **列出** | `listxattr` | `llistxattr` | `flistxattr` | `listxattrat` |
| **删除** | `removexattr` | `lremovexattr` | `fremovexattr` | `removexattrat` |

### 1.3 变体差异

```
setxattr(path, ...)          → path_setxattrat(AT_FDCWD, path, 0, ...)
lsetxattr(path, ...)         → path_setxattrat(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, ...)
fsetxattr(fd, ...)           → path_setxattrat(fd, NULL, AT_EMPTY_PATH, ...)
setxattrat(dfd, path, fl, ...)→ path_setxattrat(dfd, path, at_flags, ...)
```

各变体仅在**路径解析**阶段有差异，后续的 VFS 和文件系统操作完全相同。

---

## 2. VFS 层通用流程

### 2.1 setxattr 系列 — VFS 层

```
path_setxattrat(dfd, pathname, at_flags, name, value, size, flags)
  │
  ├─ setxattr_copy(name, &ctx)              // 从用户态拷贝 xattr 名称
  │
  ├─ CLASS(filename)(pathname, at_flags)    // 路径解析（根据 at_flags 决定）
  │    └─ getname_uflags() / getname()      // 拷贝路径字符串
  │
  ├─ CLASS(fd, f)(dfd)                      // fd 变体：通过 fd 获取文件
  │
  └─ vfs_setxattr(idmap, dentry, name, value, size, flags)
       │
       ├─ xattr_permission(inode, name, MAY_WRITE)  // 权限检查
       ├─ security_inode_setxattr()                 // LSM 安全钩子
       │
       └─ __vfs_setxattr(idmap, dentry, inode, name, value, size, flags)
            │
            ├─ xattr_resolve_name(inode, &name)  // ★ 解析命名空间
            │    └─ 遍历 inode->i_sb->s_xattr[] 数组
            │    └─ 匹配前缀: "user." / "trusted." / "security." / "system."
            │    └─ 返回对应的 xattr_handler
            │
            └─ handler->set(handler, idmap, dentry, inode, name, value, size, flags)
                 └─ [ext4] → ext4_xattr_set()
```

### 2.2 getxattr 系列 — VFS 层

```
path_getxattrat(dfd, pathname, at_flags, name, value, size)
  │
  ├─ import_xattr_name(&kname, name)        // 从用户态拷贝 xattr 名称
  ├─ CLASS(filename)(pathname, at_flags)    // 路径解析
  │
  └─ vfs_getxattr(path, name, value, size)
       │
       ├─ security_inode_getxattr(dentry, name)  // LSM 检查
       └─ __vfs_getxattr(dentry, inode, name, value, size)
            ├─ xattr_resolve_name(inode, &name)  // 命名空间解析
            └─ handler->get(handler, dentry, inode, name, value, size)
                 └─ [ext4] → ext4_xattr_get()
```

### 2.3 listxattr 系列 — VFS 层

```
path_listxattrat(dfd, pathname, at_flags, list, size)
  │
  ├─ CLASS(filename)(pathname, at_flags)    // 路径解析
  │
  └─ listxattr(dentry, list, size)
       │
       └─ vfs_listxattr(dentry, klist, size)
            └─ dentry->d_inode->i_op->listxattr(dentry, klist, size)
                 └─ [ext4] → ext4_listxattr()
```

### 2.4 removexattr 系列 — VFS 层

```
path_removexattrat(dfd, pathname, at_flags, name)
  │
  ├─ import_xattr_name(&kname, name)        // 拷贝 xattr 名称
  ├─ CLASS(filename)(pathname, at_flags)    // 路径解析
  │
  └─ vfs_removexattr(idmap, dentry, name)
       ├─ xattr_permission(inode, name, MAY_WRITE)  // 权限检查
       ├─ security_inode_removexattr()               // LSM 检查
       └─ __vfs_removexattr(idmap, dentry, name)
            ├─ xattr_resolve_name(inode, &name)      // 命名空间解析
            └─ handler->set(handler, idmap, dentry, inode, name, NULL, 0, XATTR_REPLACE)
                 └─ [ext4] → ext4_xattr_set()  // value=NULL 表示删除
```

---

## 3. ext4 扩展属性存储布局

ext4 扩展属性可以存储在**两个位置**：

```
┌─────────────────────────────────────────────────────────────┐
│ inode (256 字节)                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ 标准 inode 字段 (128 或 160 字节)                      │   │
│  │ i_mode, i_uid, i_size, i_blocks, i_ctime, ...        │   │
│  │ i_extra_isize (扩展大小)                               │   │
│  ├──────────────────────────────────────────────────────┤   │
│  │ ★ inode-in-body xattr (ibody)                         │   │
│  │  ┌─────────────────────────────────────────────────┐  │   │
│  │  │ ext4_xattr_ibody_header (h_magic=4B)             │  │   │
│  │  │ ext4_xattr_entry[] (名称+值偏移)                   │  │   │
│  │  │ 值数据 (从尾部向头部增长)                           │  │   │
│  │  └─────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ 独立 xattr 块 (1 个 block, 如 4K)                            │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ ext4_xattr_header (h_magic, h_refcount, h_hash, ...) │   │
│  │ ext4_xattr_entry[]                                    │   │
│  │ 值数据 (从尾部向头部增长)                               │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### 3.1 选择策略

```
ext4_xattr_set_handle()
  │
  ├─ ext4_xattr_ibody_find()    // 尝试在 inode body 中查找/存储
  │    └─ 空间足够 → 存 ibody
  │
  └─ 空间不足 → ext4_xattr_block_find()
       └─ 分配/复用 xattr 块 → 存 block
```

### 3.2 ext4 查询流程

```
ext4_xattr_get(inode, name_index, name, buffer, buffer_size)
  │
  ├─ ext4_xattr_ibody_get()     // 先从 inode body 中查找
  │    └─ 找到 → 返回
  │
  └─ 未找到 → ext4_xattr_block_get()  // 再到 xattr 块中查找
       └─ 找到 → 返回
       └─ 未找到 → -ENODATA
```

---

## 4. 关键数据结构

```c
// ===== xattr 命名空间索引 (fs/ext4/xattr.h) =====
#define EXT4_XATTR_INDEX_USER           1
#define EXT4_XATTR_INDEX_POSIX_ACL_ACCESS 2
#define EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT 3
#define EXT4_XATTR_INDEX_TRUSTED        4
#define EXT4_XATTR_INDEX_SECURITY       6
#define EXT4_XATTR_INDEX_HURD           7

// ===== xattr 名称前缀常量 (include/uapi/linux/xattr.h) =====
#define XATTR_USER_PREFIX     "user."
#define XATTR_USER_PREFIX_LEN (sizeof(XATTR_USER_PREFIX) - 1)
#define XATTR_TRUSTED_PREFIX  "trusted."
#define XATTR_TRUSTED_PREFIX_LEN (sizeof(XATTR_TRUSTED_PREFIX) - 1)
#define XATTR_SECURITY_PREFIX "security."
#define XATTR_SECURITY_PREFIX_LEN (sizeof(XATTR_SECURITY_PREFIX) - 1)
#define XATTR_SYSTEM_PREFIX   "system."
#define XATTR_SYSTEM_PREFIX_LEN (sizeof(XATTR_SYSTEM_PREFIX) - 1)

// ===== xattr 块头部 (fs/ext4/xattr.h) =====
// 独立 xattr 数据块的开头
struct ext4_xattr_header {
    __le32  h_magic;        // 魔数 (EA_MAGIC = 0xEA020000)
    __le32  h_refcount;     // 引用计数（多个 inode 可共享 xattr 块）
    __le32  h_blocks;       // 使用的磁盘块数
    __le32  h_hash;         // 所有条目的哈希值
    __le32  h_checksum;     // CRC32C 校验和
    __u32   h_reserved[3];  // 保留
};

// ===== inode body xattr 头部 (fs/ext4/xattr.h) =====
// 嵌入在 inode 扩展空间中
struct ext4_xattr_ibody_header {
    __le32  h_magic;        // 魔数 (EA_MAGIC = 0xEA020000)
};

// ★ 宏：从 inode body 获取 ibody xattr 头部
#define IHDR(inode, raw_inode)                                         \
    ((struct ext4_xattr_ibody_header *)                                \
        ((void *)raw_inode +                                           \
         EXT4_GOOD_OLD_INODE_SIZE +                                    \
         EXT4_I(inode)->i_extra_isize))

// ★ 宏：从 ibody 头部获取第一个条目
#define IFIRST(hdr) ((struct ext4_xattr_entry *)((hdr) + 1))

// ★ 宏：从 buffer_head 获取 block xattr 头部
#define BHDR(bh) ((struct ext4_xattr_header *)((bh)->b_data))

// ★ 宏：从 block 头部获取第一个条目
#define BFIRST(bh) ((struct ext4_xattr_entry *)(BHDR(bh) + 1))

// ===== xattr 条目结构 (fs/ext4/xattr.h) =====
// 核心结构：描述一个 xattr 键值对
struct ext4_xattr_entry {
    __u8    e_name_len;     // 名称长度（不含前缀）
    __u8    e_name_index;   // 名称索引（EXT4_XATTR_INDEX_*）
    __le16  e_value_offs;   // 值在数据块/ibody 中的偏移
    __le32  e_value_inum;   // 值存储的外部 inode 号（大值场景）
    __le32  e_value_size;   // 值大小
    __le32  e_hash;         // 名称和值的哈希值
    char    e_name[];       // 名称（变长，不含前缀）
};

// 判断是否为最后一个条目（空条目标记结束）
#define IS_LAST_ENTRY(entry) (*(__u32 *)(entry) == 0)

// 获取下一个条目
#define EXT4_XATTR_NEXT(entry)                                         \
    ((struct ext4_xattr_entry *)((char *)(entry) +                     \
      EXT4_XATTR_LEN((entry)->e_name_len)))

// 条目对齐长度
#define EXT4_XATTR_LEN(name_len)                                       \
    (((name_len) + EXT4_XATTR_ROUND + sizeof(struct ext4_xattr_entry)) \
     & ~EXT4_XATTR_ROUND)

// 值对齐大小
#define EXT4_XATTR_SIZE(size)                                          \
    (((size) + EXT4_XATTR_ROUND) & ~EXT4_XATTR_ROUND)

// ===== xattr 搜索上下文 (fs/ext4/xattr.h) =====
struct ext4_xattr_search {
    struct ext4_xattr_entry *first;  // 第一个条目
    void   *base;                    // 数据区域基地址
    void   *end;                     // 数据区域结束地址
    struct ext4_xattr_entry *here;   // 找到/插入点
    int     not_found;               // 是否未找到
};

struct ext4_xattr_ibody_find {
    struct ext4_xattr_search s;
    struct ext4_iloc iloc;           // inode 在磁盘上的位置
};

struct ext4_xattr_block_find {
    struct ext4_xattr_search s;
    struct buffer_head *bh;          // xattr 块 buffer_head
};

// ===== xattr_handler (VFS 层，连接 VFS 和具体文件系统) =====
struct xattr_handler {
    const char *name;        // 命名空间前缀（如 "user."）
    const char *prefix;      // 前缀别名
    int flags;               // 标志位
    // 文件系统实现的方法
    bool (*list)(struct dentry *dentry);
    int (*get)(const struct xattr_handler *, struct dentry *dentry,
               struct inode *inode, const char *name,
               void *buffer, size_t size);
    int (*set)(const struct xattr_handler *,
               struct mnt_idmap *idmap,
               struct dentry *dentry, struct inode *inode,
               const char *name, const void *buffer,
               size_t size, int flags);
};

// ===== ext4 xattr handler 数组 =====
// fs/ext4/xattr.c:88-109
const struct xattr_handler * const ext4_xattr_handler_map[] = {
    [EXT4_XATTR_INDEX_USER]         = &ext4_xattr_user_handler,
    [EXT4_XATTR_INDEX_POSIX_ACL_ACCESS] = &nop_posix_acl_access,
    [EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT] = &nop_posix_acl_default,
    [EXT4_XATTR_INDEX_TRUSTED]      = &ext4_xattr_trusted_handler,
    [EXT4_XATTR_INDEX_SECURITY]     = &ext4_xattr_security_handler,
    [EXT4_XATTR_INDEX_HURD]         = &ext4_xattr_hurd_handler,
};
```

---

## 5. 各文件系统 xattr 操作函数

| 操作 | VFS 通用函数 | ext4 实现 |
|------|------------|-----------|
| set | `handler->set()` | `ext4_xattr_set()` → `ext4_xattr_set_handle()` |
| get | `handler->get()` | `ext4_xattr_get()` → `ext4_xattr_ibody_get()` / `ext4_xattr_block_get()` |
| list | `inode->i_op->listxattr()` | `ext4_listxattr()` → `ext4_xattr_ibody_list()` / `ext4_xattr_block_list()` |
| remove | `handler->set()` (value=NULL, XATTR_REPLACE) | `ext4_xattr_set()` 内部处理 |

---

## 6. 参考

- `fs/xattr.c` — VFS 层 xattr 系统调用实现
- `fs/ext4/xattr.c` — ext4 扩展属性实现
- `fs/ext4/xattr.h` — ext4 扩展属性数据结构
- `include/uapi/linux/xattr.h` — 用户态 xattr 常量
- `include/linux/xattr.h` — 内核态 xattr 接口
- [ARM64 系统调用表](../arm64-syscall-table.md#扩展属性-xattr)