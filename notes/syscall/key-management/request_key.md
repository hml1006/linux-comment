# request_key 系统调用分析

## 1. 概述

请求查找或创建密钥。`request_key` 搜索指定的密钥环和密钥类型，如果找到则返回密钥序列号；如果未找到，可以调用用户空间的 `request-key` 程序来创建密钥。

**原型：**

```c
SYSCALL_DEFINE4(request_key, const char __user *, _type,
    const char __user *, _description,
    const char __user *, _callout_info,
    key_serial_t, destringid)
```

**参数：**
- `_type`：密钥类型名称
- `_description`：密钥描述
- `_callout_info`：传递给用户空间协助程序的信息（可为 NULL）
- `destringid`：目标密钥环 ID（新密钥被链接到此密钥环）

## 2. 使用场景

- 按需获取密钥（如 CIFS/NFS 挂载时自动获取密钥）
- 内核密钥管理工具
- 密钥缓存服务

## 3. 函数调用栈

```
request_key(_type, _description, _callout_info, destringid) // security/keys/keyctl.c
  └─ ksys_request_key(type, description, callout_info, destringid)
       └─ request_key_and_link(type, description, callout_info, ...)
            ├─ search_process_keyrings(keyring, type, description, ...)
            │    └─ recursively_search_keyrings(keyring, ...)   // 递归搜索密钥环
            │         ├─ 搜索进程密钥环
            │         ├─ 搜索线程密钥环
            │         └─ 搜索会话密钥环
            ├─ [未找到] → 构造授权密钥
            ├─ [未找到] → call_sbin_request_key(cred, key, ...)
            │    └─ umh_keys[].path = /sbin/request-key          // 执行用户空间协助程序
            │         └─ 调用用户空间 /sbin/request-key 创建密钥
            └─ wait_for_key_construction(key, 1)                 // 等待密钥构建完成
```

## 4. 关键数据结构

### 4.1 struct key（密钥对象）

```c
// include/linux/key.h
struct key {
    refcount_t ref;                  // 引用计数
    key_serial_t serial;             // 密钥序列号
    struct rw_semaphore sem;         // 读写信号量
    struct key_user *user;           // 密钥所有者
    key_perm_t perm;                 // 权限
    unsigned short datalen;          // 数据长度
    struct key_type *type;           // 密钥类型
    struct keyring_index_key index_key; // 索引键
    // ...
};
```

## 5. 流程图

```
用户态: request_key("user", "mykey", NULL, KEY_SPEC_SESSION_KEYRING)
    │
    v
┌─────────────────────────────────────┐
│ 搜索进程密钥环链                    │
│ search_process_keyrings()           │
│ 递归搜索: 线程密钥环 → 进程密钥环  │
│ → 会话密钥环                        │
└─────────────────────────────────────┘
    │
    ├─────── 找到密钥 ───────→ 返回密钥序列号
    │
    │ 未找到
    v
┌─────────────────────────────────────┐
│ 构造授权密钥 (authorization key)   │
│ 该密钥用于授权 /sbin/request-key   │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ call_sbin_request_key()             │
│ 执行 /sbin/request-key 程序        │
│ 传递: 类型、描述、授权密钥等信息   │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ wait_for_key_construction(key, 1)   │
│ 等待用户空间程序完成密钥创建       │
│ 超时或被杀死 → 返回错误            │
└─────────────────────────────────────┘
    │
    v
返回密钥序列号 或 错误码
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-ENOKEY` | 密钥不存在 | 搜索未找到密钥，且没有设置协助程序 |
| `-EKEYEXPIRED` | 密钥已过期 | 找到的密钥已过期 |
| `-EKEYREVOKED` | 密钥已吊销 | 找到的密钥已被吊销 |
| `-EINTR` | 信号中断 | 等待密钥构建被信号中断 |
| `-ENOMEM` | 内存不足 | 分配失败 |
| `-EFAULT` | 内存错误 | 参数指针不可访问 |
| `-EPERM` | 权限不足 | 无权访问指定密钥环 |

## 7. 使用示例

```c
#include <keyutils.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
    key_serial_t key;

    /* 先添加一个密钥，以便后续可以找到它 */
    key = add_key("user", "mykey", "data", 5,
                  KEY_SPEC_SESSION_KEYRING);
    if (key == -1) {
        perror("add_key");
        return 1;
    }

    /* 请求查找该密钥 */
    key_serial_t found = request_key("user", "mykey", NULL,
                                      KEY_SPEC_SESSION_KEYRING);
    if (found == -1) {
        perror("request_key");
        return 1;
    }

    printf("Found key serial: %d (original: %d)\n", found, key);

    /* 请求一个不存在的密钥 */
    key_serial_t missing = request_key("user", "nonexistent",
                                       NULL, KEY_SPEC_SESSION_KEYRING);
    if (missing == -1) {
        printf("Expected: Key not found: %m\n");
    }

    return 0;
}
```

## 8. 参考

- 源码位置：`security/keys/keyctl.c`（系统调用入口）
- 请求执行：`security/keys/request_key.c`
- 头文件：`include/linux/key.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#密钥管理)