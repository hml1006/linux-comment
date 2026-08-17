# lsetxattr 系统调用分析

## 1. 概述

设置文件的扩展属性（不跟踪符号链接）

**原型：**

```c
SYSCALL_DEFINE5(lsetxattr, const char __user *, pathname,
		const char __user *, name, const void __user *, value,
		__size_t, size, int, flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pathname` | - | 文件名 / fd / dfd（取决于变体） |
| `name` | `const char *` | xattr 名称（含命名空间前缀，如 `user.myattr`） |
| `value` | `const void *` | 属性值缓冲区 |
| `size` | `size_t` | 属性值大小（字节） |
| `flags` | `int` | `XATTR_CREATE`(0x1) / `XATTR_REPLACE`(0x2) / 0(upsert) |

**flags 说明：**

| 标志 | 值 | 说明 |
|------|-----|------|
| `XATTR_CREATE` | `0x1` | 创建新属性，如果属性已存在则返回 `-EEXIST` |
| `XATTR_REPLACE` | `0x2` | 替换现有属性，如果属性不存在则返回 `-ENODATA` |
| 0 | - | 不存在时创建，已存在时替换（upsert 语义） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-ENODATA` — 使用 `XATTR_REPLACE` 但属性不存在
  - `-EEXIST` — 使用 `XATTR_CREATE` 但属性已存在
  - `-EACCES` — 权限不足（如非特权进程访问 `trusted.` 命名空间）
  - `-ENOSPC` — 磁盘空间不足
  - `-EFAULT` — 用户态指针无效

## 2. 使用场景

- **SELinux 标签**: 设置文件的安全上下文（如 `security.selinux`）
- **ACL 扩展**: 设置 POSIX ACL（如 `system.posix_acl_access`）
- **用户自定义元数据**: 记录文件额外信息（如 `user.checksum`、`user.tags`）
- **IMA 完整性度量**: 设置文件的哈希值（如 `security.ima`）
- **可信属性**: 仅限特权进程设置（如 `trusted.` 命名空间）

## 3. 函数调用栈

```
lsetxattr (系统调用入口)
└─ path_setxattrat(AT_FDCWD, pathname, AT_SYMLINK_NOFOLLOW, name, value, size, flags)
   └─ do_setxattr(idmap, dentry, ctx)
      └─ vfs_setxattr(idmap, dentry, name, value, size, flags)
         └─ __vfs_setxattr(idmap, dentry, inode, name, value, size, flags)
            ├─ xattr_resolve_name(inode, &name)          // fs/xattr.c
            └─ handler->set()                             // ext4 → ext4_xattr_set()
               └─ ext4_xattr_set_handle()                 // fs/ext4/xattr.c
                  ├─ ext4_xattr_ibody_find()              // 尝试 inode body 存储
                  │  └─ ext4_xattr_find_entry()           // 在 ibody 中查找/定位插入点
                  └─ [空间不足] ext4_xattr_block_find()   // 分配 xattr 块
                     └─ ext4_xattr_find_entry()           // 在 block 中查找/定位插入点
```

## 4. 关键数据结构

```c
// ===== xattr 命名空间索引 (fs/ext4/xattr.h) =====
#define EXT4_XATTR_INDEX_USER           1  // user. 命名空间
#define EXT4_XATTR_INDEX_TRUSTED        4  // trusted. 命名空间
#define EXT4_XATTR_INDEX_SECURITY       6  // security. 命名空间

// ===== xattr 条目结构 (fs/ext4/xattr.h) =====
// 核心结构：描述一个 xattr 键值对，存储在 inode body 或独立 xattr 块中
struct ext4_xattr_entry {
    __u8    e_name_len;     // 名称长度（不含前缀）
    __u8    e_name_index;   // 名称索引（EXT4_XATTR_INDEX_*）
    __le16  e_value_offs;   // 值在数据块/ibody 中的偏移
    __le32  e_value_inum;   // 值存储的外部 inode 号（大值场景）
    __le32  e_value_size;   // 值大小
    __le32  e_hash;         // 名称和值的哈希值
    char    e_name[];       // 名称（变长，不含命名空间前缀）
};

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
// 嵌入在 inode 扩展空间中，紧跟在标准 inode 字段之后
struct ext4_xattr_ibody_header {
    __le32  h_magic;        // 魔数 (EA_MAGIC = 0xEA020000)
};

// ===== xattr_handler (VFS 层，include/linux/xattr.h) =====
// 连接 VFS 和具体文件系统的桥梁
struct xattr_handler {
    const char *name;        // 命名空间前缀（如 "user."）
    const char *prefix;      // 前缀别名
    int flags;               // 标志位
    bool (*list)(struct dentry *dentry);  // 是否列出该命名空间属性
    int (*get)(const struct xattr_handler *, struct dentry *dentry,
               struct inode *inode, const char *name,
               void *buffer, size_t size);
    int (*set)(const struct xattr_handler *,
               struct mnt_idmap *idmap,
               struct dentry *dentry, struct inode *inode,
               const char *name, const void *buffer,
               size_t size, int flags);
};

// ===== xattr 搜索上下文 (fs/ext4/xattr.h) =====
struct ext4_xattr_search {
    struct ext4_xattr_entry *first;  // 第一个条目
    void   *base;                    // 数据区域基地址
    void   *end;                     // 数据区域结束地址
    struct ext4_xattr_entry *here;   // 找到/插入点
    int     not_found;               // 是否未找到
};

// ===== xattr 标志 (include/uapi/linux/xattr.h) =====
#define XATTR_CREATE  0x1  // 创建，属性已存在则失败
#define XATTR_REPLACE 0x2  // 替换，属性不存在则失败
```

## 5. 流程图

```
路径解析：`AT_FDCWD` + `AT_SYMLINK_NOFOLLOW`（不跟踪符号链接）
  │
  v
path_setxattrat()
  │
  ├─ setxattr_copy()          // 从用户态拷贝 name 和 value
  │
  ├─ [路径解析]
  │    └─ filename_lookup()   // 根据 at_flags 解析路径
  │       └─ 获取 dentry
  │
  └─ do_setxattr()
       └─ vfs_setxattr()
            ├─ xattr_permission()          // 权限检查
            ├─ security_inode_setxattr()   // LSM 安全钩子
            └─ __vfs_setxattr()
                 └─ xattr_resolve_name()   // 解析命名空间
                    └─ handler->set()      // ext4 → ext4_xattr_set_handle()
                       ├─ ext4_xattr_ibody_find()  // 尝试 inode body
                       └─ [空间不足] ext4_xattr_block_find()  // 分配 xattr 块
```

## 6. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>
#include <errno.h>

int main(void)
{
    const char *path = "/tmp/testfile";
    const char *name = "user.myattr";
    const char *value = "hello_xattr";
    int ret;

    // 创建文件
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); return 1; }
    fclose(f);

    // 设置扩展属性（upsert 语义：不存在则创建，已存在则更新）
    ret = setxattr(path, name, value, strlen(value) + 1, 0);
    if (ret == -1) {
        perror("setxattr");
        return 1;
    }
    printf("setxattr: %s = %s\n", name, value);

    // 使用 XATTR_CREATE 尝试创建（已存在则失败）
    ret = setxattr(path, name, "new_value", 10, XATTR_CREATE);
    if (ret == -1 && errno == EEXIST)
        printf("XATTR_CREATE: EEXIST (expected)\n");

    // 使用 XATTR_REPLACE 尝试替换
    ret = setxattr(path, name, "updated", 8, XATTR_REPLACE);
    if (ret == 0)
        printf("XATTR_REPLACE: success\n");

    // 验证
    char buf[64] = {0};
    ret = getxattr(path, name, buf, sizeof(buf));
    if (ret > 0)
        printf("read back: %s = %s\n", name, buf);

    return 0;
}
```

## 7. 参考

- `fs/xattr.c` — VFS 层 xattr 系统调用实现
- `fs/ext4/xattr.c` — ext4 扩展属性实现
- `fs/ext4/xattr.h` — ext4 扩展属性数据结构定义
- `include/uapi/linux/xattr.h` — 用户态 xattr 常量（`XATTR_CREATE`/`XATTR_REPLACE`）
- `include/linux/xattr.h` — 内核态 xattr 接口（`struct xattr_handler`）
- [ARM64 系统调用表](../arm64-syscall-table.md#扩展属性-xattr)
