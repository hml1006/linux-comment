# lsm_get_self_attr 系统调用分析

## 1. 概述

获取当前进程的 Linux Security Module (LSM) 属性。该调用是 LSM 用户空间 API 的一部分，允许进程查询其安全上下文，例如 SELinux 上下文、AppArmor 标签或 Smack 标签。

**原型：**

```c
SYSCALL_DEFINE4(lsm_get_self_attr,
    unsigned int, attr,
    struct lsm_ctx __user *, ctx,
    u32 __user *, size,
    u32, flags)
```

**参数：**
- `attr`：要获取的属性类型（LSM 特定）
- `ctx`：指向用户空间 `lsm_ctx` 结构体数组的指针，用于接收数据（可为 NULL 用于查询大小）
- `size`：指向 `u32` 的指针，输入时表示缓冲区大小，输出时表示所需大小
- `flags`：标志位，支持 `LSM_FLAG_SINGLE` 表示仅返回指定 LSM 的属性

**返回值：**
- 成功时返回 `lsm_ctx` 数组元素个数
- `size` 所指向的值被更新为实际需要的字节数
- 失败时返回负的错误码

## 2. 使用场景

- 查询当前进程的 SELinux 上下文（类似 `getcon()`）
- 检查进程所属的 AppArmor 标签
- 安全策略框架的监控和管理工具

## 3. 函数调用栈

```
lsm_get_self_attr(attr, ctx, size, flags)              // security/lsm_syscalls.c
  └─ security_getselfattr(attr, ctx, size, flags)       // security/security.c
       └─ 遍历所有活跃 LSM 模块的 getselfattr 钩子
            ├─ selinux_getselfattr()                    // security/selinux/hooks.c
            ├─ apparmor_getselfattr()                   // security/apparmor/lsm.c
            ├─ smack_getselfattr()                      // security/smack/smack_lsm.c
            └─ 其他 LSM 模块...
```

## 4. 关键数据结构

### 4.1 struct lsm_ctx（LSM 上下文）

```c
// include/uapi/linux/lsm.h
/**
 * struct lsm_ctx - LSM context information
 * @id: the LSM id number, see LSM_ID_XXX
 * @flags: LSM specific flags
 * @len: length of the lsm_ctx struct, @ctx and any other data or padding
 * @ctx_len: the size of @ctx
 * @ctx: the LSM context value
 */
struct lsm_ctx {
    __u64 id;
    __u64 flags;
    __u64 len;
    __u64 ctx_len;
    __u8 ctx[] __counted_by(ctx_len);
};
```

### 4.2 LSM ID 定义

```c
// include/uapi/linux/lsm.h
#define LSM_ID_UNDEF     0
#define LSM_ID_SELINUX   1
#define LSM_ID_SMACK     2
#define LSM_ID_TOMOYO    3
#define LSM_ID_APPARMOR  4
// ... 更多 ID
```

## 5. 流程图

```
用户态: lsm_get_self_attr(LSM_ATTR_CURRENT, ctx, &size, 0)
    │
    v
┌─────────────────────────────────────┐
│ 检查 flags 是否有效                  │
│ 无效 → 返回 -EINVAL                  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 遍历所有活跃 LSM 模块               │
│ 调用每个 LSM 的 getselfattr 钩子    │
│ 收集安全上下文数据                  │
│ ┌─────────────────────────────────┐ │
│ │ selinux: 获取当前进程的 SID     │ │
│ │ apparmor: 获取当前进程的标签    │ │
│ │ smack: 获取当前进程的 Smack 值  │ │
│ └─────────────────────────────────┘ │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 计算总大小并写入 *size              │
│ 如果缓冲区不足 → 返回 -E2BIG       │
│ 否则将 lsm_ctx 数组拷贝到用户空间  │
└─────────────────────────────────────┘
    │
    v
返回 lsm_ctx 元素个数
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | flags 包含未知位 |
| `-E2BIG` | 缓冲区不足 | 提供的 size 小于所需值（size 被更新为所需值） |
| `-EFAULT` | 内存错误 | ctx 或 size 指针不可访问 |
| `-EOPNOTSUPP` | 不支持 | 没有 LSM 实现该属性 |

## 7. 使用示例

```c
#include <linux/lsm.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef lsm_get_self_attr
static inline int
lsm_get_self_attr(unsigned int attr, struct lsm_ctx *ctx,
                  u32 *size, u32 flags)
{
    return syscall(__NR_lsm_get_self_attr, attr, ctx, size, flags);
}
#endif

int main(void)
{
    u32 size = 0;
    struct lsm_ctx *ctx;
    int count;

    /* 第一次调用：查询所需缓冲区大小 */
    count = lsm_get_self_attr(0, NULL, &size, 0);
    if (count < 0 && size == 0) {
        perror("lsm_get_self_attr");
        return 1;
    }
    printf("Need %u bytes for LSM contexts\n", size);

    /* 分配缓冲区 */
    ctx = malloc(size);
    if (!ctx) {
        perror("malloc");
        return 1;
    }

    /* 第二次调用：获取实际数据 */
    count = lsm_get_self_attr(0, ctx, &size, 0);
    if (count < 0) {
        perror("lsm_get_self_attr");
        free(ctx);
        return 1;
    }

    printf("Got %d LSM context(s):\n", count);
    struct lsm_ctx *c = ctx;
    for (int i = 0; i < count; i++) {
        printf("  LSM id=%llu, flags=%llu, ctx_len=%llu\n",
               (unsigned long long)c->id,
               (unsigned long long)c->flags,
               (unsigned long long)c->ctx_len);
        /* ctx 数据在 c->ctx 中 */
        c = (struct lsm_ctx *)((char *)c + c->len);
    }

    free(ctx);
    return 0;
}
```

## 8. 参考

- 源码位置：`security/lsm_syscalls.c`
- 头文件：`include/uapi/linux/lsm.h`
- LSM 框架：`security/security.c`
- [ARM64 系统调用表](../arm64-syscall-table.md#权限与安全)