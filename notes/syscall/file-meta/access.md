# access 系统调用

## 原理与功能

`access` 系统调用根据进程的**实际用户 ID 和实际组 ID**（而非有效 ID）检查文件访问权限。这在 `setuid` 程序中特别有用：程序可以以实际用户的身份检查权限，避免以提升的权限误判。

在 ARM64 架构上，`access` 没有独立的系统调用号，通过 `faccessat`（syscall #48）实现，glibc 封装为 `faccessat(AT_FDCWD, path, mode, 0)`。

### 功能说明

- 检查文件是否可读（`R_OK`）
- 检查文件是否可写（`W_OK`）
- 检查文件是否可执行（`X_OK`）
- 检查文件是否存在（`F_OK`）
- 使用实际用户/组 ID 而非有效 ID

## 使用场景

- setuid 程序以实际用户身份检查权限
- 在操作前预检查文件是否存在
- 安全检查工具验证文件访问能力

## API 及使用案例

### 函数原型

```c
#include <unistd.h>

int access(const char *pathname, int mode);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `pathname` | `const char*` | 文件路径 |
| `mode` | `int` | 检查模式：`F_OK`(0), `R_OK`(4), `W_OK`(2), `X_OK`(1) |

### 返回值

- 成功（所有请求的权限都允许）返回 0
- 失败（任一权限不满足）返回 -1 并设置 `errno`

### 使用示例

```c
#include <stdio.h>
#include <unistd.h>

int main() {
    const char *path = "/etc/passwd";

    if (access(path, F_OK) == 0)
        printf("文件存在\n");
    else
        printf("文件不存在\n");

    if (access(path, R_OK) == 0)
        printf("文件可读\n");
    else
        perror("不可读");

    if (access(path, W_OK) == 0)
        printf("文件可写\n");
    else
        perror("不可写");

    if (access(path, X_OK) == 0)
        printf("文件可执行\n");
    else
        perror("不可执行");

    return 0;
}
```

## 执行流程

```
用户进程                          内核
    |                               |
    | access(path, mode)            |
    |-----> syscall(#48) ---------->|
    |   faccessat(AT_FDCWD, path,   |
    |             mode, 0)          |
    |                               |
    |    +---------------------+    |
    |    | do_faccessat()      |    |
    |    | fs/open.c           |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | 切换到实际用户凭证:  |    |
    |    | override_cred =     |    |
    |    |   current->real_cred |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | user_path()         |    |
    |    | 路径解析            |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | inode_permission()  |    |
    |    | 检查 inode 权限     |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | __inode_permission()|    |
    |    | 递归检查目录权限    |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | do_inode_permission()|    |
    |    | 检查 inode->i_mode  |    |
    |    | vs 进程凭证         |    |
    |    +---------+-----------+    |
    |              |                |
    |    +---------v-----------+    |
    |    | 恢复原始凭证        |    |
    |    +---------+-----------+    |
    |              |                |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
access(pathname, mode)
  └─ syscall(__NR_faccessat, AT_FDCWD, pathname, mode, 0)
       └─ __arm64_sys_faccessat()                  // arch/arm64/kernel/syscall.c
            └─ do_faccessat(AT_FDCWD, pathname, mode, 0)  // fs/open.c
                 ├─ 临时切换 cred → real_cred       // 使用实际用户/组 ID
                 ├─ user_path(pathname, &path)      // 路径解析
                 └─ inode_perention(idmap, inode, mask)  // 权限检查
                      └─ __inode_permission(inode, mask)
                           └─ do_inode_permission(inode, mask)
                                └─ generic_permission()  // 通用的权限检查
```

## 关键数据结构

### struct cred（进程凭证）

```c
// include/linux/cred.h
struct cred {
    kuid_t      uid;          // 实际用户 ID（access 使用这个）
    kgid_t      gid;          // 实际组 ID（access 使用这个）
    kuid_t      suid;         // 保存的用户 ID
    kgid_t      sgid;         // 保存的组 ID
    kuid_t      euid;         // 有效用户 ID
    kgid_t      egid;         // 有效组 ID
    unsigned    securebits;   // 安全位
    // ...
};
```

## 备注

- ARM64 上无独立 `access` 系统调用号，通过 `faccessat` 实现
- `access` 使用实际用户/组 ID，这与大多数其他文件操作（使用有效 ID）不同
- 存在 TOCTOU 竞态条件：检查和使用之间文件可能变化
- 在 setuid 程序中特别有用：脚本解释器可以检查实际用户是否有权限读取脚本