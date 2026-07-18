# lsm_set_self_attr 系统调用分析

## 1. 概述

设置当前进程的 Linux Security Module (LSM) 属性。该调用是 LSM 用户空间 API 的一部分，允许进程修改其安全上下文（如 SELinux 上下文转换）。

**原型：**

```c
SYSCALL_DEFINE4(lsm_set_self_attr,
    unsigned int, attr,
    struct lsm_ctx __user *, ctx,
    u32, size,
    u32, flags)
```

**参数：**
- `attr`：要设置的属性类型
- `ctx`：指向用户空间 `lsm_ctx` 结构体的指针，包含要设置的安全上下文
- `size`：`ctx` 结构体的大小
- `flags`：预留，必须为 0

## 2. 使用场景

- SELinux 上下文转换（类似 `setcon()`）
- 进程改变其安全标签
- 安全感知的应用程序在特定操作前后切换安全域

## 3. 函数调用栈

```
lsm_set_self_attr(attr, ctx, size, flags)             // security/lsm_syscalls.c
  ├─ 检查 flags 是否有效
  ├─ 检查 ctx 和 size 的有效性
  └─ security_setselfattr(attr, ctx, size, flags)      // security/security.c
       └─ 遍历 LSM 模块的 setselfattr 钩子
            ├─ selinux_setselfattr()                   // security/selinux/hooks.c
            ├─ apparmor_setselfattr()                  // security/apparmor/lsm.c
            └─ 其他 LSM 模块...
```

## 4. 关键数据结构

### 4.1 struct lsm_ctx（LSM 上下文）

```c
// include/uapi/linux/lsm.h
struct lsm_ctx {
    __u64 id;
    __u64 flags;
    __u64 len;
    __u64 ctx_len;
    __u8 ctx[] __counted_by(ctx_len);
};
```

## 5. 流程图

```
用户态: lsm_set_self_attr(attr, &ctx, sizeof(ctx), 0)
    │
    v
┌─────────────────────────────────────┐
│ 参数验证                             │
│ - 检查 flags 是否有效               │
│ - 检查 ctx 是否为 NULL              │
│ - 检查 size 是否有效               │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ copy_from_user() 拷贝 ctx 到内核    │
│ 验证 lsm_ctx 结构完整性             │
│ - id 是否有效                       │
│ - len 是否 >= sizeof(lsm_ctx)        │
│ - ctx_len 是否匹配                  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 遍历 LSM 模块                       │
│ 将上下文传递给对应的 LSM 钩子      │
│ ┌─────────────────────────────────┐ │
│ │ selinux: 检查权限并设置新 SID   │ │
│ │ apparmor: 检查并设置新标签      │ │
│ └─────────────────────────────────┘ │
│ LSM 内部会进行权限检查（如 CAP_MAC_ADMIN）│
└─────────────────────────────────────┘
    │
    v
返回 0 (成功) 或错误码
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | flags 非零 / attr 无效 / ctx 结构无效 |
| `-EFAULT` | 内存错误 | ctx 指针不可读 |
| `-EPERM` | 权限不足 | 无权修改安全上下文（如缺少 CAP_MAC_ADMIN） |
| `-EOPNOTSUPP` | 不支持 | 没有 LSM 支持该操作 |

## 7. 使用示例

```c
#include <linux/lsm.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef lsm_set_self_attr
static inline int
lsm_set_self_attr(unsigned int attr, struct lsm_ctx *ctx,
                  u32 size, u32 flags)
{
    return syscall(__NR_lsm_set_self_attr, attr, ctx, size, flags);
}
#endif

int main(void)
{
    /* 注意：此示例需要适当的安全权限 */
    /* 在实际使用中，需要根据 LSM 类型构造正确的上下文 */

    /* 准备一个 SELinux 上下文示例 */
    const char *selinux_context = "user_u:role_r:type_t:s0";
    size_t ctx_len = strlen(selinux_context) + 1;

    /* 分配 lsm_ctx，包含动态 ctx 数据 */
    struct lsm_ctx *ctx = malloc(sizeof(struct lsm_ctx) + ctx_len);
    if (!ctx) {
        perror("malloc");
        return 1;
    }

    ctx->id = LSM_ID_SELINUX;
    ctx->flags = 0;
    ctx->ctx_len = ctx_len;
    ctx->len = sizeof(struct lsm_ctx) + ctx_len;
    memcpy(ctx->ctx, selinux_context, ctx_len);

    /* 尝试设置安全上下文 */
    int ret = lsm_set_self_attr(0, ctx, ctx->len, 0);
    if (ret < 0) {
        perror("lsm_set_self_attr");
        printf("Note: This operation typically requires CAP_MAC_ADMIN\n");
        free(ctx);
        return 1;
    }

    printf("Security context updated successfully\n");
    free(ctx);
    return 0;
}
```

## 8. 参考

- 源码位置：`security/lsm_syscalls.c`
- 头文件：`include/uapi/linux/lsm.h`
- LSM 框架：`security/security.c`
- [ARM64 系统调用表](../arm64-syscall-table.md#权限与安全)