# listxattr 系统调用分析

## 1. 概述

列出文件的扩展属性名（跟踪符号链接）

**原型：**

```c
SYSCALL_DEFINE3(listxattr, const char __user *, pathname, char __user *, list,
		__size_t, size)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pathname` | - | 文件名 / fd / dfd（取决于变体） |
| `list` | `char *` | 接收属性名列表的缓冲区（每个名以 `\0` 分隔） |
| `size` | `size_t` | 缓冲区大小 |

**返回值：**

- 成功返回属性名列表的总长度（字节数），每个属性名以 `\0` 分隔
- 如果 `size` 为 0 则返回实际大小用于查询
- 失败返回负值错误码：
  - `-EACCES` — 权限不足
  - `-ERANGE` — 缓冲区太小
  - `-EFAULT` — 用户态指针无效

## 2. 使用场景

- **查看所有属性**: 列出文件上设置的所有扩展属性名称
- **备份恢复**: 在备份前获取所有属性名，用于后续 `getxattr` 逐一读取
- **文件系统审计**: 检查文件是否携带了额外的元数据
- **调试诊断**: 确认特定命名空间的属性是否存在

## 3. 函数调用栈

```
listxattr (系统调用入口)
└─ path_listxattrat(AT_FDCWD, pathname, 0（跟踪符号链接）, list, size)
   └─ listxattr(dentry, list, size)
      └─ vfs_listxattr(dentry, klist, size)
         ├─ security_inode_listxattr(dentry)             // LSM 检查
         └─ inode->i_op->listxattr(dentry, klist, size)  // ext4 → ext4_listxattr()
            └─ ext4_listxattr()                          // fs/ext4/xattr.c
               ├─ ext4_xattr_ibody_list()                // 列出 inode body 中的 xattr
               └─ ext4_xattr_block_list()                // 列出 xattr 块中的 xattr
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
路径解析：`AT_FDCWD` + `LOOKUP_FOLLOW`（跟踪符号链接）
  │
  v
path_listxattrat()
  │
  ├─ [路径解析]
  │    └─ filename_lookup()   // 根据 at_flags 解析路径
  │
  └─ listxattr()
       └─ vfs_listxattr()
            ├─ security_inode_listxattr()  // LSM 安全检查
            └─ inode->i_op->listxattr()   // ext4 → ext4_listxattr()
               ├─ ext4_xattr_ibody_list()  // 列出 inode body 中的 xattr
               └─ ext4_xattr_block_list()  // 列出 xattr 块中的 xattr
```

## 6. 使用示例

```c
#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>

int main(void)
{
    const char *path = "/tmp/testfile";
    char list[1024];
    ssize_t len;

    // 设置多个属性
    setxattr(path, "user.color", "red", 4, 0);
    setxattr(path, "user.size", "large", 6, 0);
    setxattr(path, "user.tags", "important", 10, 0);

    // 列出所有属性
    len = listxattr(path, list, sizeof(list));
    if (len > 0) {
        printf("xattrs (%zd bytes):\n", len);
        for (char *p = list; p < list + len; p += strlen(p) + 1)
            printf("  - %s\n", p);
    }

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
