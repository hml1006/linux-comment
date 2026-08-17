# lremovexattr 系统调用分析

## 1. 概述

删除文件的扩展属性（不跟踪符号链接）

**原型：**

```c
SYSCALL_DEFINE2(lremovexattr, const char __user *, pathname,
		const char __user *, name)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pathname` | - | 文件名 / fd / dfd（取决于变体） |
| `name` | `const char *` | 要删除的 xattr 名称（含命名空间前缀） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-ENODATA` — 指定的属性不存在
  - `-EACCES` — 权限不足
  - `-EFAULT` — 用户态指针无效

## 2. 使用场景

- **清理元数据**: 删除不再需要的用户自定义属性
- **安全上下文重置**: 删除安全属性后重新设置
- **ACL 清理**: 删除 POSIX ACL 恢复默认权限
- **IMA 完整性重置**: 删除完整性度量值
- **文件迁移**: 清理源文件的扩展属性后再复制

## 3. 函数调用栈

```
lremovexattr (系统调用入口)
└─ path_removexattrat(AT_FDCWD, pathname, AT_SYMLINK_NOFOLLOW, name)
   └─ removexattr(idmap, dentry, name)
      └─ vfs_removexattr(idmap, dentry, name)
         └─ __vfs_removexattr(idmap, dentry, name)
            ├─ xattr_resolve_name(inode, &name)          // fs/xattr.c
            └─ handler->set(handler, idmap, dentry, inode, name, NULL, 0,
                 XATTR_REPLACE)                          // value=NULL 表示删除
               └─ [ext4] → ext4_xattr_set()              // fs/ext4/xattr.c
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
path_removexattrat()
  │
  ├─ import_xattr_name()      // 从用户态拷贝 name
  │
  ├─ [路径解析]
  │    └─ filename_lookup()   // 根据 at_flags 解析路径
  │
  └─ removexattr()
       └─ vfs_removexattr()
            ├─ xattr_permission()            // 权限检查
            ├─ security_inode_removexattr()  // LSM 安全检查
            └─ __vfs_removexattr()
                 └─ xattr_resolve_name()     // 解析命名空间
                    └─ handler->set(handler, ..., NULL, 0, XATTR_REPLACE)
                       // value=NULL 表示删除操作
                       └─ [ext4] → ext4_xattr_set()
```

## 6. 使用示例

```c
#include <stdio.h>
#include <sys/xattr.h>

int main(void)
{
    const char *path = "/tmp/testfile";

    // 先设置属性
    setxattr(path, "user.myattr", "value", 6, 0);

    // 删除属性
    int ret = removexattr(path, "user.myattr");
    if (ret == 0)
        printf("removexattr: success\n");

    // 验证删除
    char buf[64];
    ret = getxattr(path, "user.myattr", buf, sizeof(buf));
    if (ret == -1)
        printf("confirmed: ENODATA after removal\n");

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
