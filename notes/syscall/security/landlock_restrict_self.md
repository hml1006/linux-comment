# landlock_restrict_self 系统调用分析

## 1. 概述

对当前进程施加 Landlock 规则集。一旦调用成功，当前进程及其未来的子进程将被规则集约束，无法执行规则之外的操作。该调用是 Landlock 安全策略的"启用"步骤，类似于 seccomp 的 `SECCOMP_SET_MODE_FILTER`，且不可逆（规则只能增加，不能减少）。

**原型：**

```c
SYSCALL_DEFINE2(landlock_restrict_self,
    const int, ruleset_fd,
    const __u32, flags)
```

**参数：**
- `ruleset_fd`：由 `landlock_create_ruleset` 创建的规则集文件描述符
- `flags`：控制日志行为的标志位，支持：
  - `LANDLOCK_RESTRICT_SELF_LOG_SAME_EXEC_OFF` (1 << 0)：关闭同一次 exec 的日志
  - `LANDLOCK_RESTRICT_SELF_LOG_NEW_EXEC_ON` (1 << 1)：开启新 exec 的日志
  - `LANDLOCK_RESTRICT_SELF_LOG_SUBDOMAINS_OFF` (1 << 2)：关闭子域日志

## 2. 使用场景

- 进程沙箱化：在启动不可信代码前限制其权限
- 浏览器沙箱：渲染进程仅允许访问特定目录
- 容器运行时：在容器内限制进程的文件系统访问

## 3. 函数调用栈

```
landlock_restrict_self(ruleset_fd, flags)              // security/landlock/syscalls.c
  ├─ is_initialized() → 检查 landlock 是否已初始化
  ├─ 权限检查:
  │    ├─ 如果进程没有 no_new_privs 且没有 CAP_SYS_ADMIN
  │    │    └─ 返回 -EPERM
  │    └─ 需要 no_new_privs=1 或 CAP_SYS_ADMIN
  ├─ flags 校验（仅允许 LANDLOCK_MASK_RESTRICT_SELF 内的位）
  ├─ 解析日志标志位:
  │    ├─ log_same_exec = !(flags & LANDLOCK_RESTRICT_SELF_LOG_SAME_EXEC_OFF)
  │    ├─ log_new_exec = !!(flags & LANDLOCK_RESTRICT_SELF_LOG_NEW_EXEC_ON)
  │    └─ log_subdomains = !(flags & LANDLOCK_RESTRICT_SELF_LOG_SUBDOMAINS_OFF)
  ├─ get_ruleset_from_fd(ruleset_fd, FMODE_CAN_READ)   // 获取规则集
  ├─ prepare_creds()                                    // 准备新的凭证
  ├─ landlock_install_ruleset(cred, ruleset)             // 安装规则集到凭证
  │    └─ cred->security (landlock) = ruleset
  ├─ commit_creds(cred)                                  // 提交凭证变更
  └─ 返回 0
```

## 4. 关键数据结构

### 4.1 struct landlock_cred_security（凭证中的 Landlock 安全域）

```c
// security/landlock/cred.h
struct landlock_cred_security {
    struct landlock_ruleset *domain;  // 当前进程的 Landlock 域（规则集）
};
```

### 4.2 日志标志位定义

```c
// include/uapi/linux/landlock.h
/* 日志标志位 */
#define LANDLOCK_RESTRICT_SELF_LOG_SAME_EXEC_OFF    (1ULL << 0)
#define LANDLOCK_RESTRICT_SELF_LOG_NEW_EXEC_ON      (1ULL << 1)
#define LANDLOCK_RESTRICT_SELF_LOG_SUBDOMAINS_OFF   (1ULL << 2)

/* 所有有效标志位的掩码 */
#define LANDLOCK_MASK_RESTRICT_SELF                 \
    (LANDLOCK_RESTRICT_SELF_LOG_SAME_EXEC_OFF |     \
     LANDLOCK_RESTRICT_SELF_LOG_NEW_EXEC_ON |       \
     LANDLOCK_RESTRICT_SELF_LOG_SUBDOMAINS_OFF)
```

## 5. 流程图

```
用户态: landlock_restrict_self(ruleset_fd, 0)
    │
    v
┌─────────────────────────────────────┐
│ 权限检查                             │
│ no_new_privs=1 或 CAP_SYS_ADMIN ？  │
│ 否 → 返回 -EPERM                     │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 解析 flags 中的日志控制位            │
│ 设置 log_same_exec / log_new_exec   │
│ log_subdomains 等布尔标志            │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ get_ruleset_from_fd(ruleset_fd)     │
│ 获取规则集内核对象                   │
│ 失败 → 返回 -EBADF                   │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ prepare_creds()                     │
│ 创建当前进程凭证的副本              │
│ 失败 → 返回 -ENOMEM                 │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ landlock_install_ruleset()          │
│ 将规则集链接到新凭证的安全域中      │
│ 处理规则集的分层（prev 指针）       │
│ 规则集只能增加，不能减少            │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ commit_creds(cred)                  │
│ 原子切换进程的凭证                  │
│ Landlock 域变为不可逆的永久约束     │
└─────────────────────────────────────┘
    │
    v
返回 0 (成功)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EOPNOTSUPP` | 不支持 | Landlock 未启用 |
| `-EPERM` | 权限不足 | 没有 no_new_privs 也没有 CAP_SYS_ADMIN |
| `-EINVAL` | 无效参数 | flags 包含无效位 |
| `-EBADF` | 无效 fd | ruleset_fd 无效 |
| `-ENOMEM` | 内存不足 | 凭证分配失败 |

## 7. 使用示例

```c
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

#ifndef landlock_restrict_self
static inline int
landlock_restrict_self(int ruleset_fd, __u32 flags)
{
    return syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}
#endif

int main(void)
{
    int ruleset_fd, dir_fd;
    struct landlock_ruleset_attr attr = {
        .handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE |
                             LANDLOCK_ACCESS_FS_OPEN_DIR,
    };

    /* 第一步：创建规则集 */
    ruleset_fd = syscall(__NR_landlock_create_ruleset, &attr,
                         sizeof(attr), 0);
    if (ruleset_fd < 0) {
        perror("landlock_create_ruleset");
        return 1;
    }

    /* 第二步：打开允许访问的目录 */
    dir_fd = open("/usr/share", O_RDONLY | O_CLOEXEC);
    if (dir_fd < 0) {
        perror("open");
        close(ruleset_fd);
        return 1;
    }

    /* 第三步：添加路径规则 */
    struct landlock_path_beneath_attr path_attr = {
        .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE |
                          LANDLOCK_ACCESS_FS_OPEN_DIR,
        .parent_fd = dir_fd,
    };
    if (syscall(__NR_landlock_add_rule, ruleset_fd,
                LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0)) {
        perror("landlock_add_rule");
        close(dir_fd);
        close(ruleset_fd);
        return 1;
    }
    close(dir_fd);

    /* 第四步：设置 no_new_privs 并应用规则集 */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        perror("prctl(NO_NEW_PRIVS)");
        close(ruleset_fd);
        return 1;
    }

    if (landlock_restrict_self(ruleset_fd, 0)) {
        perror("landlock_restrict_self");
        close(ruleset_fd);
        return 1;
    }
    close(ruleset_fd);

    printf("Landlock sandbox enabled!\n");
    printf("Now only /usr/share is accessible\n");

    /* 此时进程已被限制，尝试访问 /etc 会失败 */
    FILE *f = fopen("/etc/passwd", "r");
    if (f == NULL) {
        printf("Expected: cannot open /etc/passwd: %m\n");
    }

    return 0;
}
```

## 8. 参考

- 源码位置：`security/landlock/syscalls.c`
- 凭证集成：`security/landlock/cred.h`
- 安全钩子：`security/landlock/setup.c`
- [ARM64 系统调用表](../arm64-syscall-table.md#权限与安全)