# capset 系统调用分析

## 1. 概述

设置进程的能力（capability）集。`capset` 允许进程修改其 effective、permitted 和 inheritable 能力集。当前实现中，`capset` 只能影响当前进程（PID 必须为 0 或当前进程的 PID）。

**原型：**

```c
SYSCALL_DEFINE2(capset, cap_user_header_t, header, const cap_user_data_t, data)
```

**参数：**
- `header`：指向 `__user_cap_header_struct` 的指针，包含版本号和目标 PID
- `data`：指向 `__user_cap_data_struct` 数组的指针，包含要设置的能力值

## 2. 使用场景

- 进程在启动时丢弃不需要的能力
- 特权程序临时提升能力以执行特定操作
- 容器运行时配置进程的初始能力集

## 3. 函数调用栈

```
capset(header, data)                                     // kernel/capability.c
  ├─ cap_validate_magic(header, &tocopy)                 // 验证版本号
  ├─ get_user(pid, &header->pid)                         // 读取目标 PID
  ├─ [pid != 0 && pid != task_pid_vnr(current)] → -EPERM  // 只能设置当前进程
  ├─ copy_from_user(&kdata, data, ...)                   // 拷贝能力数据
  ├─ 组装内核能力值:
  │    effective   = mk_kernel_cap(kdata[0].effective,   kdata[1].effective)
  │    permitted   = mk_kernel_cap(kdata[0].permitted,   kdata[1].permitted)
  │    inheritable = mk_kernel_cap(kdata[0].inheritable, kdata[1].inheritable)
  ├─ prepare_creds()                                     // 准备新凭证
  ├─ cap_capset(new, old, effective, permitted, inheritable) // 内核能力检查
  │    └─ 验证新能力集是旧能力集的子集（除非有 CAP_SETPCAP）
  ├─ security_capset() → LSM 安全检查
  └─ commit_creds(new)                                   // 提交凭证变更
```

## 4. 关键数据结构

### 4.1 struct __user_cap_header_struct（能力请求头）

```c
// include/uapi/linux/capability.h
typedef struct __user_cap_header_struct {
    __u32 version;      // 能力版本号
    int pid;            // 目标 PID（必须为 0 或当前 PID）
} __user *cap_user_header_t;
```

### 4.2 struct __user_cap_data_struct（能力数据）

```c
// include/uapi/linux/capability.h
struct __user_cap_data_struct {
    __u32 effective;    // 新有效能力集
    __u32 permitted;    // 新许可能力集
    __u32 inheritable;  // 新可继承能力集
};
```

## 5. 流程图

```
用户态: capset(&header, data)
    │
    v
┌─────────────────────────────────────┐
│ 验证版本号                           │
│ 无效 → 返回 -EINVAL                  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 检查 PID: 只能设置当前进程          │
│ pid != 0 && pid != current → -EPERM │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ copy_from_user() → 拷贝能力数据     │
│ 组装 64 位内核能力值                │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ prepare_creds()                     │
│ 创建当前凭据的副本                  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ cap_capset(new, old, ...)           │
│ 安全检查: 新能力集必须是旧能力的子集│
│ 除非有 CAP_SETPCAP                   │
│ 失败 → 返回 -EPERM                  │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ security_capset() → LSM 检查        │
│ commit_creds() → 提交凭证变更       │
└─────────────────────────────────────┘
    │
    v
返回 0 (成功)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | header->version 无效 |
| `-EFAULT` | 内存错误 | header 或 data 不可访问 |
| `-EPERM` | 权限不足 | 试图设置其他进程的能力 / 试图获取未允许的能力 |
| `-ENOMEM` | 内存不足 | 凭证分配失败 |

## 7. 使用示例

```c
#include <sys/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    struct __user_cap_header_struct header = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,
    };
    struct __user_cap_data_struct data[2];

    /* 先获取当前能力 */
    if (capget(&header, data) < 0) {
        perror("capget");
        return 1;
    }

    printf("Original capabilities:\n");
    printf("  Effective:   0x%08x%08x\n",
           data[1].effective, data[0].effective);

    /* 丢弃所有 effective 能力 */
    memset(data, 0, sizeof(data));
    /* 注意: 需要 CAP_SETPCAP 才能设置 permitted 能力 */
    /* 此处仅清除 effective 位 */
    if (capset(&header, data) < 0) {
        perror("capset");
        return 1;
    }

    printf("After dropping all capabilities:\n");
    printf("  Effective:   0x%08x%08x\n",
           data[1].effective, data[0].effective);

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/capability.c`
- 头文件：`include/uapi/linux/capability.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)