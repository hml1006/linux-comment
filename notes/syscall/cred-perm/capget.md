# capget 系统调用分析

## 1. 概述

获取进程的能力（capability）集。Linux 能力机制将传统的超级用户权限拆分为独立的可授予单元，`capget` 可以查询任意进程的 effective、permitted 和 inheritable 能力集。

**原型：**

```c
SYSCALL_DEFINE2(capget, cap_user_header_t, header, cap_user_data_t, dataptr)
```

**参数：**
- `header`：指向 `__user_cap_header_struct` 的指针，包含版本号和目标 PID
- `dataptr`：指向 `__user_cap_data_struct` 数组的指针，用于接收能力数据（可为 NULL 用于探测版本）

## 2. 使用场景

- 查询当前进程或指定进程的能力状态
- 安全检查工具（如 `capsh`, `getcap`）
- 容器运行时确认进程的权限边界

## 3. 函数调用栈

```
capget(header, dataptr)                                  // kernel/capability.c
  ├─ cap_validate_magic(header, &tocopy)                 // 验证版本号，获取拷贝计数
  │    └─ 检查 header->version 是否与 _LINUX_CAPABILITY_VERSION 兼容
  ├─ [dataptr == NULL] → 返回 0（仅探测版本兼容性）
  ├─ get_user(pid, &header->pid)                         // 读取目标 PID
  ├─ [pid < 0] → 返回 -EINVAL
  ├─ cap_get_target_pid(pid, &pE, &pI, &pP)             // 获取目标进程能力
  │    └─ find_task_by_vpid(pid) → 查找进程
  │    └─ security_capget() → LSM 能力检查
  └─ 将 64 位能力拆分为两个 32 位字段写入用户空间
       kdata[0].effective = pE.val; kdata[1].effective = pE.val >> 32;
       kdata[0].permitted = pP.val; kdata[1].permitted = pP.val >> 32;
       kdata[0].inheritable = pI.val; kdata[1].inheritable = pI.val >> 32;
```

## 4. 关键数据结构

### 4.1 struct __user_cap_header_struct（能力请求头）

```c
// include/uapi/linux/capability.h
typedef struct __user_cap_header_struct {
    __u32 version;      // 能力版本号（必须为 _LINUX_CAPABILITY_VERSION_x）
    int pid;            // 目标 PID（0 表示当前进程）
} __user *cap_user_header_t;
```

### 4.2 struct __user_cap_data_struct（能力数据）

```c
// include/uapi/linux/capability.h
struct __user_cap_data_struct {
    __u32 effective;    // 有效能力集（当前生效的能力）
    __u32 permitted;    // 许可能力集（可被使用的上限）
    __u32 inheritable;  // 可继承能力集（通过 execve 传递给子进程）
};
typedef struct __user_cap_data_struct __user *cap_user_data_t;
```

## 5. 流程图

```
用户态: capget(&header, data)
    │
    v
┌─────────────────────────────────────┐
│ cap_validate_magic(header, &tocopy) │
│ 检查 header->version 是否有效       │
│ 返回应拷贝的 cap_data 结构数量      │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ dataptr == NULL?                    │
│ 是 → 返回 0 (仅探测版本)           │
│ 否 → 继续                           │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ get_user(pid, &header->pid)         │
│ 从用户空间读取目标 PID              │
│ pid < 0 → 返回 -EINVAL              │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ cap_get_target_pid(pid, &pE, ...)  │
│ 通过 find_task_by_vpid() 查找进程  │
│ 获取进程凭证中的能力集              │
│ 调用 security_capget() LSM 钩子    │
│ 失败 → 返回 -ESRCH / -EPERM        │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 拆分 64 位能力为 2×32 位           │
│ copy_to_user(data, kdata, ...)     │
│ 失败 → 返回 -EFAULT                 │
└─────────────────────────────────────┘
    │
    v
返回 0 (成功)
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | header->version 无效 / pid 为负数 |
| `-EFAULT` | 内存错误 | header 或 dataptr 不可访问 |
| `-ESRCH` | 进程不存在 | 指定 PID 的进程不存在 |
| `-EPERM` | 权限不足 | 无权访问目标进程的能力信息 |

## 7. 使用示例

```c
#include <sys/capability.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct __user_cap_header_struct header = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,  // 当前进程
    };
    struct __user_cap_data_struct data[2];

    if (capget(&header, data) < 0) {
        perror("capget");
        return 1;
    }

    printf("Capabilities for PID %d:\n", getpid());
    printf("  Effective:   0x%08x%08x\n",
           data[1].effective, data[0].effective);
    printf("  Permitted:   0x%08x%08x\n",
           data[1].permitted, data[0].permitted);
    printf("  Inheritable: 0x%08x%08x\n",
           data[1].inheritable, data[0].inheritable);

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/capability.c`
- 头文件：`include/uapi/linux/capability.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)