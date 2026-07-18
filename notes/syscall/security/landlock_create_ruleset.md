# landlock_create_ruleset 系统调用分析

## 1. 概述

创建 Landlock 规则集。Landlock 是一种非特权的基于路径和网络的强制访问控制（MAC）机制，允许进程为其自身创建沙箱安全策略。该调用是 Landlock 使用的第一步，创建一个空的规则集，后续通过 `landlock_add_rule` 添加规则，最后通过 `landlock_restrict_self` 应用。

**原型：**

```c
SYSCALL_DEFINE3(landlock_create_ruleset,
    const struct landlock_ruleset_attr __user *const, attr,
    const size_t, size,
    const __u32, flags)
```

**参数：**
- `attr`：指向 `landlock_ruleset_attr` 结构体的指针，指定规则集处理哪些访问权限
- `size`：`attr` 结构体的大小（用于向前兼容）
- `flags`：特殊标志，当前支持：
  - `0`：正常创建规则集
  - `LANDLOCK_CREATE_RULESET_VERSION`：查询 Landlock ABI 版本
  - `LANDLOCK_CREATE_RULESET_ERRATA`：查询 Landlock 勘误信息

**返回值：**
- 成功时返回新规则集的文件描述符
- 失败时返回负的错误码

## 2. 使用场景

- 沙箱初始化：创建规则集以定义允许的文件系统操作
- 查询 ABI 版本：在不支持 Landlock 的系统上优雅降级
- 容器运行时：创建隔离的规则集后限制子进程

## 3. 函数调用栈

```
landlock_create_ruleset(attr, size, flags)            // security/landlock/syscalls.c
  ├─ build_check_abi()                                 // 编译时检查 ABI 一致性
  ├─ is_initialized() → 检查 landlock 是否已初始化     // 未初始化返回 -EOPNOTSUPP
  ├─ [flags 处理]
  │    ├─ LANDLOCK_CREATE_RULESET_VERSION → 返回 landlock_abi_version
  │    ├─ LANDLOCK_CREATE_RULESET_ERRATA  → 返回 landlock_errata
  │    └─ 其他非零 flags → 返回 -EINVAL
  ├─ copy_min_struct_from_user(&ruleset_attr, ...)     // 从用户空间安全拷贝属性
  ├─ landlock_create_ruleset(&ruleset_attr, ...)       // 内核创建规则集
  │    ├─ kzalloc(sizeof(struct landlock_ruleset), GFP_KERNEL_ACCOUNT)
  │    ├─ ruleset->handled_access_fs = attr.handled_access_fs
  │    ├─ ruleset->handled_access_net = attr.handled_access_net
  │    └─ refcount_set(&ruleset->refs, 1)
  └─ anon_inode_getfd("landlock-ruleset", &ruleset_fops, ruleset, ...)  // 创建匿名 fd
       └─ 返回 fd
```

## 4. 关键数据结构

### 4.1 struct landlock_ruleset_attr（用户空间属性）

```c
// include/uapi/linux/landlock.h
/**
 * struct landlock_ruleset_attr - Ruleset definition.
 * Argument of sys_landlock_create_ruleset().
 */
struct landlock_ruleset_attr {
    /**
     * @handled_access_fs: Bitmask of handled filesystem actions
     * (cf. `Filesystem flags`_).
     */
    __u64 handled_access_fs;
    /**
     * @handled_access_net: Bitmask of handled network actions (cf. `Network
     * flags`_).
     */
    __u64 handled_access_net;
};
```

### 4.2 struct landlock_ruleset（内核规则集）

```c
// security/landlock/ruleset.h
struct landlock_ruleset {
    refcount_t refs;                    // 引用计数
    struct landlock_ruleset *prev;      // 前一个规则集（用于分层）
    struct rb_root root;                // 路径规则红黑树根
    struct work_struct work_free;       // 延迟释放工作项
    struct rcu_head rcu;                // RCU 销毁回调
    struct landlock_ruleset_attr attr;  // 规则集属性（handled accesses）
    // ... 更多内部字段
};
```

## 5. 流程图

```
用户态: landlock_create_ruleset(&attr, sizeof(attr), 0)
    │
    v
┌─────────────────────────────────────┐
│ build_check_abi()                   │
│ 编译时检查内核结构与 UAPI 一致性     │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ is_initialized()                    │
│ 否 → 返回 -EOPNOTSUPP               │
├─────────────────────────────────────┤
│ flags 处理:                         │
│ - VERSION → 返回 ABI 版本号         │
│ - ERRATA  → 返回勘误信息            │
│ - 非零无效 flags → 返回 -EINVAL    │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ copy_min_struct_from_user()         │
│ 从用户空间安全拷贝 landlock_ruleset_attr│
│ 处理大小不匹配的兼容性问题          │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ landlock_create_ruleset()           │
│ 内核分配: kzalloc(sizeof(ruleset))  │
│ 初始化 handled_access_fs/net        │
│ 设置引用计数为 1                    │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ anon_inode_getfd()                  │
│ 创建匿名 inode 文件描述符           │
│ 关联 fops 操作集（只读/写管理）    │
│ 返回 fd 给用户空间                  │
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EOPNOTSUPP` | 不支持 | Landlock 未编译或未在引导时启用 |
| `-EINVAL` | 无效参数 | flags 无效 / attr 大小无效 |
| `-EFAULT` | 内存错误 | attr 指针不可读 |
| `-ENOMEM` | 内存不足 | 规则集分配失败 |
| `-ENFILE` | 文件表满 | 系统文件描述符表耗尽 |
| `-EMFILE` | 进程文件表满 | 进程文件描述符表耗尽 |
| `-ENOMSG` | 无消息 | 请求的 ABI 版本不可用（flags 查询） |

## 7. 使用示例

```c
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#ifndef landlock_create_ruleset
static inline int
landlock_create_ruleset(const struct landlock_ruleset_attr *attr,
                        size_t size, __u32 flags)
{
    return syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
#endif

int main(void)
{
    int ruleset_fd;
    struct landlock_ruleset_attr attr = {
        .handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE |
                             LANDLOCK_ACCESS_FS_WRITE_FILE |
                             LANDLOCK_ACCESS_FS_READ_FILE |
                             LANDLOCK_ACCESS_FS_OPEN_DIR,
    };

    /* 首先查询 ABI 版本 */
    int abi = landlock_create_ruleset(NULL, 0,
                     LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) {
        perror("Landlock is not supported");
        return 1;
    }
    printf("Landlock ABI version: %d\n", abi);

    /* 创建规则集 */
    ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (ruleset_fd < 0) {
        perror("Failed to create ruleset");
        return 1;
    }
    printf("Created ruleset fd: %d\n", ruleset_fd);

    /* ... 后续使用 landlock_add_rule 添加规则 ... */
    /* ... 最后使用 landlock_restrict_self 应用 ... */

    close(ruleset_fd);
    return 0;
}
```

## 8. 参考

- 源码位置：`security/landlock/syscalls.c`
- 头文件：`include/uapi/linux/landlock.h`
- 内核规则集：`security/landlock/ruleset.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#权限与安全)