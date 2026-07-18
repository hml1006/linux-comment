# landlock_add_rule 系统调用分析

## 1. 概述

向已创建的 Landlock 规则集添加一条规则。规则类型目前支持路径规则（`LANDLOCK_RULE_PATH_BENEATH`）和网络端口规则（`LANDLOCK_RULE_NET_PORT`）。规则定义了在特定文件系统路径或网络端口上允许的操作。

**原型：**

```c
SYSCALL_DEFINE4(landlock_add_rule,
    const int, ruleset_fd,
    const enum landlock_rule_type, rule_type,
    const void __user *const, rule_attr,
    const __u32, flags)
```

**参数：**
- `ruleset_fd`：由 `landlock_create_ruleset` 返回的规则集文件描述符
- `rule_type`：规则类型，当前支持：
  - `LANDLOCK_RULE_PATH_BENEATH` (1)：路径规则
  - `LANDLOCK_RULE_NET_PORT` (2)：网络端口规则
- `rule_attr`：指向规则属性结构体的指针（取决于规则类型）
- `flags`：预留，必须为 0

## 2. 使用场景

- 限制进程只能访问特定目录树（如仅允许读取 `/usr/share`）
- 允许进程绑定特定的网络端口
- 构建白名单访问控制策略

## 3. 函数调用栈

```
landlock_add_rule(ruleset_fd, rule_type, rule_attr, flags)  // security/landlock/syscalls.c
  ├─ is_initialized() → 检查 landlock 是否已初始化
  ├─ flags 检查 → 必须为 0
  ├─ get_ruleset_from_fd(ruleset_fd, FMODE_CAN_WRITE)      // 获取规则集文件
  │    └─ fget(ruleset_fd)
  │    └─ 验证文件操作表匹配 ruleset_fops
  ├─ switch (rule_type):
  │    ├─ LANDLOCK_RULE_PATH_BENEATH:
  │    │    └─ add_rule_path_beneath(ruleset, rule_attr)
  │    │         ├─ copy_struct_from_user(&path_beneath_attr, ...)
  │    │         ├─ get_path_from_fd(path_beneath_attr.parent_fd)
  │    │         │    └─ fget(path_beneath_attr.parent_fd)  // 获取父目录 fd
  │    │         ├─ landlock_append_rule(ruleset, &path_beneath_attr, ...)
  │    │         │    └─ 创建 landlock_rule 并插入红黑树
  │    │         └─ fput(path_beneath_attr.parent_fd)
  │    └─ LANDLOCK_RULE_NET_PORT:
  │         └─ add_rule_net_port(ruleset, rule_attr)
  │              ├─ copy_struct_from_user(&net_port_attr, ...)
  │              └─ landlock_append_rule(ruleset, &net_port_attr, ...)
  └─ fput(ruleset_fd)  // 释放文件引用（由 __free 自动处理）
```

## 4. 关键数据结构

### 4.1 enum landlock_rule_type（规则类型枚举）

```c
// include/uapi/linux/landlock.h
enum landlock_rule_type {
    LANDLOCK_RULE_PATH_BENEATH = 1,   // 路径规则
    LANDLOCK_RULE_NET_PORT,            // 网络端口规则
};
```

### 4.2 struct landlock_path_beneath_attr（路径规则属性）

```c
// include/uapi/linux/landlock.h
struct landlock_path_beneath_attr {
    __u64 allowed_access;               // 允许的访问权限位掩码
    __s32 parent_fd;                    // 父目录的文件描述符
} __packed;
```

### 4.3 struct landlock_net_port_attr（网络端口规则属性）

```c
// include/uapi/linux/landlock.h
struct landlock_net_port_attr {
    __u64 allowed_access;               // 允许的网络操作位掩码
    __u64 port;                         // 网络端口号（主机字节序）
};
```

## 5. 流程图

```
用户态: landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &attr, 0)
    │
    v
┌─────────────────────────────────────┐
│ 检查 Landlock 是否已初始化          │
│ 否 → 返回 -EOPNOTSUPP                │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ get_ruleset_from_fd(ruleset_fd)     │
│ 通过 fd 获取内核 landlock_ruleset  │
│ 验证 fd 类型为规则集                │
│ 失败 → 返回 -EBADF / -EINVAL        │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 根据 rule_type 分发:                │
├──────────────┬──────────────────────┘
│ PATH_BENEATH │ NET_PORT
│              │
v              v
┌──────────┐  ┌──────────┐
│copy_struct│  │copy_struct│
│_from_user │  │_from_user │
│parent_fd  │  │port       │
│→ fget()   │  │           │
└──────────┘  └──────────┘
    │              │
    └──────┬───────┘
           v
┌─────────────────────────────────────┐
│ landlock_append_rule()              │
│ 创建规则节点                        │
│ 插入到规则集的红黑树中              │
│ 处理权限掩码的交集                  │
└─────────────────────────────────────┘
    │
    v
返回 0 (成功)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EOPNOTSUPP` | 不支持 | Landlock 未启用 |
| `-EINVAL` | 无效参数 | flags 非零 / rule_type 未知 |
| `-EBADF` | 无效 fd | ruleset_fd 或 parent_fd 无效 |
| `-EFAULT` | 内存错误 | rule_attr 指针不可读 |
| `-ENOMEM` | 内存不足 | 规则节点分配失败 |
| `-EACCES` | 权限不足 | 无权限访问指定路径 |
| `-ENOENT` | 路径不存在 | parent_fd 对应的路径不存在 |
| `-E2BIG` | 规则过多 | 规则集已满 |

## 7. 使用示例

```c
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#ifndef landlock_add_rule
static inline int
landlock_add_rule(int ruleset_fd, enum landlock_rule_type rule_type,
                  const void *rule_attr, __u32 flags)
{
    return syscall(__NR_landlock_add_rule, ruleset_fd, rule_type,
                   rule_attr, flags);
}
#endif

int main(void)
{
    int ruleset_fd, dir_fd;
    struct landlock_ruleset_attr attr = {
        .handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE |
                             LANDLOCK_ACCESS_FS_OPEN_DIR,
    };

    /* 创建规则集 */
    ruleset_fd = syscall(__NR_landlock_create_ruleset, &attr,
                         sizeof(attr), 0);
    if (ruleset_fd < 0) {
        perror("landlock_create_ruleset");
        return 1;
    }

    /* 打开要限制的目录 */
    dir_fd = open("/usr/share", O_RDONLY | O_CLOEXEC);
    if (dir_fd < 0) {
        perror("open");
        close(ruleset_fd);
        return 1;
    }

    /* 添加路径规则：允许读取 /usr/share 下的文件 */
    struct landlock_path_beneath_attr path_attr = {
        .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE |
                          LANDLOCK_ACCESS_FS_OPEN_DIR,
        .parent_fd = dir_fd,
    };

    if (landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
                          &path_attr, 0)) {
        perror("landlock_add_rule");
        close(dir_fd);
        close(ruleset_fd);
        return 1;
    }

    printf("Added path rule for /usr/share\n");
    close(dir_fd);
    close(ruleset_fd);
    return 0;
}
```

## 8. 参考

- 源码位置：`security/landlock/syscalls.c`
- 头文件：`include/uapi/linux/landlock.h`
- 规则管理：`security/landlock/ruleset.c`
- [ARM64 系统调用表](../arm64-syscall-table.md#权限与安全)