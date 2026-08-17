# getxattr 系统调用分析

## 1. 概述

获取文件的扩展属性（跟踪符号链接）

**原型：**

```c
SYSCALL_DEFINE4(getxattr, const char __user *, pathname,
		const char __user *, name, void __user *, value, __size_t, size)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `pathname` | - | 文件名 / fd / dfd（取决于变体） |
| `name` | `const char *` | xattr 名称（含命名空间前缀） |
| `value` | `void *` | 接收属性值的缓冲区（可为 NULL 用于查询大小） |
| `size` | `size_t` | 缓冲区大小 |

**返回值：**

- 成功返回属性值的长度（字节数），如果 `size` 为 0 则返回实际大小用于查询
- 失败返回负值错误码：
  - `-ENODATA` — 指定的属性不存在
  - `-EACCES` — 权限不足
  - `-ERANGE` — `value` 缓冲区大小不足
  - `-EFAULT` — 用户态指针无效

## 2. 使用场景

- **SELinux 权限检查**: 获取文件安全上下文，判断访问权限
- **ACL 读取**: 获取文件的 POSIX ACL 规则
- **用户自定义元数据**: 读取文件扩展信息（如下载来源、校验和）
- **IMA 完整性验证**: 读取文件的完整性度量哈希值
- **调试诊断**: 查看文件系统支持的扩展属性

## 3. 函数调用栈

```
getxattr (系统调用入口)
└─ path_getxattrat(AT_FDCWD, pathname, 0（跟踪符号链接）, name, value, size)
   └─ do_getxattr(idmap, dentry, ctx)
      └─ vfs_getxattr(dentry, name, value, size)
         └─ __vfs_getxattr(dentry, inode, name, value, size)
            ├─ xattr_resolve_name(inode, &name)          // fs/xattr.c
            └─ handler->get()                             // ext4 → ext4_xattr_get()
               └─ ext4_xattr_get()                        // fs/ext4/xattr.c
                  ├─ ext4_xattr_ibody_get()               // 先查 inode body
                  │  └─ ext4_xattr_find_entry()           // 在 ibody 中查找匹配条目
                  │     └─ 找到 → memcpy 拷贝值
                  └─ [未找到] ext4_xattr_block_get()      // 再查 xattr 块
                     └─ ext4_xattr_find_entry()           // 在 block 中查找匹配条目
                        └─ 找到 → memcpy 拷贝值
                        └─ 未找到 → -ENODATA
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
path_getxattrat()
  │
  ├─ import_xattr_name()      // 从用户态拷贝 name
  │
  ├─ [路径解析]
  │    └─ filename_lookup()   // 根据 at_flags 解析路径
  │
  └─ do_getxattr()
       └─ vfs_getxattr()
            ├─ security_inode_getxattr()  // LSM 安全检查
            └─ __vfs_getxattr()
                 └─ xattr_resolve_name()  // 解析命名空间
                    └─ handler->get()     // ext4 → ext4_xattr_get()
                       ├─ ext4_xattr_ibody_get()   // 先查 inode body
                       └─ [未找到] ext4_xattr_block_get()  // 再查 xattr 块
                          └─ 未找到 → -ENODATA
```

## 6. 使用示例

```c
#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>

int main(void)
{
    const char *path = "/tmp/testfile";
    char buf[256];
    ssize_t len;

    // 先设置一个属性
    setxattr(path, "user.myattr", "hello", 6, 0);

    // 查询属性值大小（size = 0）
    len = getxattr(path, "user.myattr", NULL, 0);
    if (len > 0) {
        printf("value size: %zd bytes\n", len);

        // 读取属性值
        getxattr(path, "user.myattr", buf, sizeof(buf));
        printf("value: %s\n", buf);
    }

    // 读取不存在的属性
    len = getxattr(path, "user.nonexist", buf, sizeof(buf));
    if (len == -1)
        printf("ENODATA: user.nonexist not found\n");

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
