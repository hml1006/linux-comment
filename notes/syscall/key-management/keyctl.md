# keyctl 系统调用分析

## 1. 概述

多功能密钥管理控制接口。`keyctl` 是一个多路复用的系统调用，通过 `option` 参数选择具体操作，涵盖密钥的整个生命周期管理。

**原型：**

```c
SYSCALL_DEFINE5(keyctl, int, option, unsigned long, arg2,
    unsigned long, arg3, unsigned long, arg4, unsigned long, arg5)
```

**参数：**
- `option`：操作类型（见下方列表）
- `arg2-arg5`：操作特定的参数

## 2. 支持的操作

| option 值 | 功能 | 描述 |
|-----------|------|------|
| `KEYCTL_GET_KEYRING_ID` | 获取密钥环 ID | 根据特殊 ID 获取实际密钥环序列号 |
| `KEYCTL_JOIN_SESSION_KEYRING` | 加入会话密钥环 | 加入或创建会话密钥环 |
| `KEYCTL_UPDATE` | 更新密钥 | 更新密钥的 payload 数据 |
| `KEYCTL_REVOKE` | 吊销密钥 | 吊销密钥，使其不可用 |
| `KEYCTL_DESCRIBE` | 描述密钥 | 获取密钥的元数据描述 |
| `KEYCTL_CLEAR` | 清空密钥环 | 清除密钥环中的所有密钥 |
| `KEYCTL_LINK` | 链接密钥 | 将密钥链接到密钥环 |
| `KEYCTL_UNLINK` | 解链密钥 | 从密钥环中移除密钥 |
| `KEYCTL_SEARCH` | 搜索密钥 | 在密钥环中搜索指定密钥 |
| `KEYCTL_READ` | 读取密钥 | 读取密钥的 payload 数据 |
| `KEYCTL_CHOWN` | 更改所有者 | 更改密钥的所有者和组 |
| `KEYCTL_SETPERM` | 设置权限 | 设置密钥的访问权限 |
| `KEYCTL_INSTANTIATE` | 实例化密钥 | 实例化一个未完成的密钥 |
| `KEYCTL_NEGATE` | 否定密钥 | 将密钥标记为否定（不存在） |
| `KEYCTL_SET_REQKEY_KEYRING` | 设置默认密钥环 | 设置 request_key 的默认目标密钥环 |
| `KEYCTL_SET_TIMEOUT` | 设置超时 | 设置密钥的过期时间 |
| `KEYCTL_ASSUME_AUTHORITY` | 承担授权 | 承担 request_key 操作的授权 |
| `KEYCTL_GET_SECURITY` | 获取安全上下文 | 获取密钥的 LSM 安全上下文 |
| `KEYCTL_SESSION_TO_PARENT` | 会话继承 | 将当前会话密钥环传递给父进程 |
| `KEYCTL_REJECT` | 拒绝密钥 | 带超时和错误码拒绝密钥 |
| `KEYCTL_INVALIDATE` | 失效密钥 | 立即失效密钥 |
| `KEYCTL_GET_PERSISTENT` | 获取持久密钥环 | 获取用户持久密钥环 |
| `KEYCTL_WATCH_KEY` | 监视密钥 | 设置密钥变化通知 |
| `KEYCTL_MOVE` | 移动密钥 | 在密钥环之间移动密钥 |

## 3. 函数调用栈

```
keyctl(option, arg2, arg3, arg4, arg5)                  // security/keys/keyctl.c
  └─ switch (option):
       ├─ KEYCTL_GET_KEYRING_ID → keyctl_get_keyring_ID()
       ├─ KEYCTL_JOIN_SESSION_KEYRING → keyctl_join_session_keyring()
       ├─ KEYCTL_UPDATE → keyctl_update_key()
       ├─ KEYCTL_REVOKE → keyctl_revoke_key()
       ├─ KEYCTL_DESCRIBE → keyctl_describe_key()
       ├─ KEYCTL_LINK → keyctl_keyring_link()
       ├─ KEYCTL_UNLINK → keyctl_keyring_unlink()
       ├─ KEYCTL_READ → keyctl_read_key()
       ├─ KEYCTL_CHOWN → keyctl_chown_key()
       ├─ KEYCTL_SETPERM → keyctl_setperm_key()
       ├─ KEYCTL_INSTANTIATE → keyctl_instantiate_key()
       ├─ ... (每种操作对应一个处理函数)
       └─ 默认 → 返回 -EOPNOTSUPP
```

## 4. 使用示例

```c
#include <keyutils.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
    key_serial_t key;
    const char *payload = "secret_data";

    /* 添加密钥 */
    key = add_key("user", "testkey", payload,
                  strlen(payload) + 1,
                  KEY_SPEC_SESSION_KEYRING);
    if (key == -1) {
        perror("add_key");
        return 1;
    }

    /* 描述密钥 */
    char desc[256];
    if (keyctl(KEYCTL_DESCRIBE, key, desc, sizeof(desc)) == -1) {
        perror("keyctl DESCRIBE");
        return 1;
    }
    printf("Description: %s\n", desc);

    /* 读取密钥数据 */
    char buffer[256];
    long ret = keyctl(KEYCTL_READ, key, buffer, sizeof(buffer));
    if (ret < 0) {
        perror("keyctl READ");
        return 1;
    }
    printf("Payload: %s\n", buffer);

    /* 更新密钥数据 */
    const char *new_payload = "new_secret_data";
    if (keyctl(KEYCTL_UPDATE, key, new_payload,
               strlen(new_payload) + 1) == -1) {
        perror("keyctl UPDATE");
        return 1;
    }
    printf("Key updated\n");

    /* 设置权限（只有所有者可读） */
    keyctl(KEYCTL_SETPERM, key, KEY_POS_VIEW | KEY_POS_READ |
           KEY_USR_VIEW | KEY_USR_READ);

    /* 吊销密钥 */
    keyctl(KEYCTL_REVOKE, key);
    printf("Key revoked\n");

    return 0;
}
```

## 5. 参考

- 源码位置：`security/keys/keyctl.c`
- 头文件：`include/uapi/linux/keyctl.h`
- 密钥管理：`security/keys/`
- [ARM64 系统调用表](../arm64-syscall-table.md#密钥管理)