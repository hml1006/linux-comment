# add_key 系统调用分析

## 1. 概述

向内核的密钥管理子系统添加一个密钥。密钥可以添加到指定的密钥环（keyring）中，用于内核安全服务（如 NFS、CIFS、dm-crypt 等）。

**原型：**

```c
SYSCALL_DEFINE5(add_key, const char __user *, _type,
    const char __user *, _description,
    const void __user *, _payload,
    size_t, plen,
    key_serial_t, ringid)
```

**参数：**
- `_type`：密钥类型名称（如 "user", "logon", "kerberos" 等）
- `_description`：密钥描述（唯一标识符）
- `_payload`：密钥数据负载
- `plen`：负载长度（最大 1MB - 1 字节）
- `ringid`：目标密钥环的 ID

## 2. 使用场景

- 存储加密密钥（dm-crypt、NFS、CIFS）
- 用户空间向内核注册密钥
- 密钥管理工具（`keyctl` 命令）

## 3. 函数调用栈

```
add_key(_type, _description, _payload, plen, ringid)   // security/keys/keyctl.c
  ├─ key_get_type_from_user(type, _type, ...)           // 从用户空间拷贝类型名
  ├─ strndup_user(_description, ...)                    // 拷贝描述
  ├─ [plen > 1024*1024-1] → -EINVAL                    // 负载大小限制
  ├─ [plen > 0] → 拷贝 payload 到内核空间
  └─ ksys_add_key(type, description, payload, plen, ringid)
       └─ key_create_or_update(keyring_ref, type, description, ...)
            ├─ key_type_lookup(type)                    // 查找密钥类型
            ├─ key_alloc(keyring, type, desc, ...)      // 分配 key 结构
            │    └─ kmem_cache_alloc(KEY_STRUCT_CACHE, ...)
            ├─ key_type->instantiate(key, prep, ...)    // 实例化密钥（调用类型特定方法）
            └─ key_link(keyring, key)                   // 将密钥链接到密钥环
```

## 4. 关键数据结构

### 4.1 struct key（密钥对象）

```c
// include/linux/key.h
struct key {
    refcount_t ref;                  // 引用计数
    key_serial_t serial;             // 密钥序列号（唯一标识）
    struct rw_semaphore sem;         // 读写信号量
    struct key_user *user;           // 密钥所有者
    key_perm_t perm;                 // 权限位
    unsigned short datalen;          // 数据长度
    unsigned long flags;             // 标志位
    struct key_type *type;           // 密钥类型
    struct keyring_index_key index_key; // 索引键
    union {
        struct list_head name_link;  // 名称链表
        struct list_head keys;       // 密钥链表（密钥环）
    };
    struct key_restriction *restrict_link; // 链接限制
};
```

### 4.2 struct key_type（密钥类型）

```c
// include/linux/key-type.h
struct key_type {
    const char *name;                // 类型名称（如 "user"）
    int (*instantiate)(struct key *key, struct key_preparsed_payload *prep);
    int (*update)(struct key *key, struct key_preparsed_payload *prep);
    int (*match)(const struct key *key, const struct key_match_data *data);
    void (*revoke)(struct key *key);
    void (*destroy)(struct key *key);
    long (*read)(const struct key *key, char *buffer, size_t buflen);
    // ... 更多操作
};
```

## 5. 流程图

```
用户态: add_key("user", "mykey", payload, plen, KEY_SPEC_SESSION_KEYRING)
    │
    v
┌─────────────────────────────────────┐
│ 从用户空间拷贝 type, description,  │
│ payload 到内核空间                  │
│ plen 不能超过 1MB - 1               │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ key_type_lookup("user")             │
│ 在内核中查找 "user" 密钥类型的实现  │
│ 未找到 → 返回 -ENOKEY              │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ key_alloc()                         │
│ 分配 struct key 对象                │
│ 分配序列号进行检查                  │
│ 检查配额限制                        │
│ 失败 → -ENOMEM / -EDQUOT           │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ key_type->instantiate(key, prep)    │
│ 调用 user 类型的 instantiate 方法   │
│ 将 payload 拷贝到 key 的 data 字段  │
│ 失败 → key_put() 并返回错误        │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ key_link(keyring, key)              │
│ 将密钥链接到目标密钥环              │
│ 检查链接限制                        │
│ 处理重复密钥（更新或拒绝）          │
└─────────────────────────────────────┘
    │
    v
返回密钥序列号 (key_serial_t)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | plen 超过 1MB / 类型名为空 |
| `-ENOKEY` | 密钥类型不存在 | 指定的密钥类型未注册 |
| `-ENOMEM` | 内存不足 | 分配失败 |
| `-EDQUOT` | 配额超限 | 用户密钥配额已满 |
| `-EFAULT` | 内存错误 | 用户空间指针不可访问 |
| `-EPERM` | 权限不足 | 无权向目标密钥环添加密钥 |
| `-EKEYREJECTED` | 拒绝 | 类型特定的验证失败 |

## 7. 使用示例

```c
#include <keyutils.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
    key_serial_t key;
    const char *payload = "my_secret_key_data";

    /* 添加一个 "user" 类型密钥到会话密钥环 */
    key = add_key("user", "mykey",
                  payload, strlen(payload) + 1,
                  KEY_SPEC_SESSION_KEYRING);
    if (key == -1) {
        perror("add_key");
        return 1;
    }

    printf("Added key with serial: %d\n", key);

    /* 使用 keyctl 描述密钥 */
    char desc[256];
    if (keyctl(KEYCTL_DESCRIBE, key, desc, sizeof(desc)) == -1) {
        perror("keyctl DESCRIBE");
        return 1;
    }
    printf("Key description: %s\n", desc);

    return 0;
}
```

## 8. 参考

- 源码位置：`security/keys/keyctl.c`
- 密钥管理：`security/keys/key.c`
- 头文件：`include/linux/key.h`, `include/uapi/linux/keyctl.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#密钥管理)