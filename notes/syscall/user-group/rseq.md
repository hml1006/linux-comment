# rseq 系统调用分析

## 1. 概述

`rseq`（Restartable Sequences，可重启序列）注册或注销当前线程的 rseq 结构。rseq 允许用户态代码执行原子性的 per-CPU 操作，无需使用重量级的原子指令或锁。当线程被抢占或信号中断时，内核自动回滚到 abort 处理程序。

**原型：**

```c
SYSCALL_DEFINE4(rseq, struct rseq __user *, rseq, u32, rseq_len,
                int, flags, u32, sig)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `rseq` | `struct rseq __user *` | 指向用户空间 `rseq` 结构体的指针（或 NULL 用于查询） |
| `rseq_len` | `u32` | `rseq` 结构体的大小 |
| `flags` | `int` | 标志位（`RSEQ_FLAG_UNREGISTER` 等） |
| `sig` | `u32` | 签名，用于验证 rseq 临界区的所有权 |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- per-CPU 数据结构的无锁操作（如计数器、内存分配器）
- 用户态 RCU 读端关键区
- 用户态 per-CPU 内存分配器
- 性能关键路径上的原子操作替代方案

## 3. 函数调用栈

### 3.1 rseq 注册（无 RSEQ_FLAG_UNREGISTER）

```
SYSCALL_DEFINE4(rseq, rseq, rseq_len, flags, sig)       // kernel/rseq.c
  ├─ flags & ~(RSEQ_FLAG_SLICE_EXT_DEFAULT_ON) → 返回 -EINVAL
  ├─ [current->rseq.usrptr != NULL]
  │    ├─ 检查地址是否与已注册的一致
  │    │    不一致 → 返回 -EINVAL
  │    ├─ 检查 sig 是否匹配
  │    │    不匹配 → 返回 -EPERM
  │    └─ 已注册 → 返回 -EBUSY
  ├─ 检查 rseq 对齐和长度
  ├─ current->rseq.usrptr = rseq      // 设置用户空间 rseq 指针
  ├─ current->rseq.sig = sig          // 保存签名
  ├─ current->rseq.len = rseq_len     // 保存长度
  ├─ rseq_force_update()              // 立即更新 cpu_id 和 cpu_id_start
  └─ return 0
```

### 3.2 rseq 注销（RSEQ_FLAG_UNREGISTER）

```
SYSCALL_DEFINE4(rseq, rseq, rseq_len, flags, sig)
  ├─ flags & RSEQ_FLAG_UNREGISTER
  │    ├─ 检查 rseq 指针是否匹配 → 不匹配返回 -EINVAL
  │    ├─ 检查 rseq_len 是否匹配 → 不匹配返回 -EINVAL
  │    ├─ 检查 sig 是否匹配 → 不匹配返回 -EPERM
  │    ├─ rseq_reset_ids()            // 重置 cpu_id 和 cpu_id_start
  │    ├─ rseq_reset(current)         // 清空内核 rseq 状态
  │    └─ return 0
```

## 4. 关键数据结构

### 4.1 struct rseq（用户态 rseq 结构体）

```c
// include/uapi/linux/rseq.h
struct rseq {
    __u32 cpu_id_start;          // 初始 CPU 编号（由内核在每次进入用户空间时更新）
    __u32 cpu_id;                // 当前 CPU 编号（由内核更新，-1 表示未注册）
    __u64 rseq_cs;               // 指向当前 rseq_cs 的指针（用户态设置）
    __u32 flags;                 // 标志位
    __u32 node_id;               // NUMA 节点 ID
    __u32 mm_cid;                // 内存空间内的 CPU ID
    struct rseq_slice_ctrl slice_ctrl;  // 切片控制（仅当启用 CONFIG_RSEQ_SLICE_EXTENSION）
};
```

### 4.2 struct rseq_cs（rseq 临界区描述符）

```c
// include/uapi/linux/rseq.h
struct rseq_cs {
    __u32 version;               // 版本号（当前为 0）
    __u32 flags;                 // 标志位
    __u64 start_ip;              // 临界区起始地址
    __u64 post_commit_offset;    // 提交后偏移（相对于 start_ip）
    __u64 abort_ip;              // abort 处理地址（必须在临界区范围外）
};
```

### 4.3 rseq 临界区协议

```
用户态代码流程：
                     init(rseq_cs)
                     cpu = TLS->rseq::cpu_id_start
   [1]               TLS->rseq::rseq_cs = rseq_cs
   [start_ip]        ----------------------------
   [2]               if (cpu != TLS->rseq::cpu_id)
                             goto abort_ip;
   [3]               <last_instruction_in_cs>
   [post_commit_ip]  ----------------------------
   [4]               <success>

   内核在以下情况会触发 abort：
   - 在 [1] 和 [3] 之间发生抢占
   - 在 [1] 和 [3] 之间接收到信号
   内核会：清除 TLS->rseq::rseq_cs，并将返回 IP 设为 abort_ip
```

## 5. 流程图

```
rseq 系统调用入口
    │
    ├── RSEQ_FLAG_UNREGISTER 设置?
    │    ├── 是 → 注销流程
    │    │    ├─ 验证 rseq 指针、长度、签名
    │    │    ├─ 重置 cpu_id_start
    │    │    └─ 清空内核状态
    │    │
    │    └── 否 → 注册流程
    │         ├─ 检查是否已注册
    │         │    ├─ 是且参数一致 → 返回 -EBUSY
    │         │    └─ 是但参数不一致 → 返回 -EINVAL
    │         ├─ 检查对齐和长度
    │         ├─ 保存 rseq 指针、长度、签名
    │         ├─ rseq_force_update()
    │         └─ 返回 0
    │
    ▼
用户态执行 rseq 临界区
    │
    ├── 正常执行完毕
    │    └─ 继续运行
    │
    ├── 被抢占/中断
    │    ├─ 内核检查 rseq_cs 是否在临界区中
    │    ├─ 清除 rseq_cs 字段
    │    ├─ 设置返回 IP 为 abort_ip
    │    └─ 恢复执行 → 跳转到 abort_ip
    │
    └── 信号处理
         └─ 同上
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | `flags` 包含未定义的位，或 `rseq_len` 不匹配，或对齐错误 |
| `-EBUSY` | 已注册 | 线程已注册 rseq，且参数与现有注册一致 |
| `-EPERM` | 签名不匹配 | `sig` 参数与注册时的签名不一致 |
| `-EFAULT` | 地址错误 | 访问用户空间 `rseq` 结构体失败 |
| `-ENOSYS` | 不支持 | 内核未配置 `CONFIG_RSEQ` |

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/rseq.h>

#ifndef __NR_rseq
#define __NR_rseq 334
#endif

static struct rseq *__rseq_abi;
static int rseq_registered;

int rseq_register_current_thread(void)
{
    int rc;

    __rseq_abi = (struct rseq *)aligned_alloc(sizeof(struct rseq), sizeof(struct rseq));
    if (!__rseq_abi)
        return -1;

    memset(__rseq_abi, 0, sizeof(*__rseq_abi));

    rc = syscall(__NR_rseq, __rseq_abi, sizeof(*__rseq_abi), 0, 0);
    if (rc) {
        free(__rseq_abi);
        __rseq_abi = NULL;
        return -1;
    }

    rseq_registered = 1;
    return 0;
}

int main(void)
{
    if (rseq_register_current_thread()) {
        perror("rseq_register");
        return 1;
    }

    printf("rseq registered successfully\n");
    printf("cpu_id_start: %u\n", __rseq_abi->cpu_id_start);

    // 使用 rseq 临界区（示例：原子递增 per-CPU 计数器）
    // 实际应用中应使用内联汇编实现 rseq 临界区协议

    // 注销 rseq
    if (syscall(__NR_rseq, __rseq_abi, sizeof(*__rseq_abi),
                RSEQ_FLAG_UNREGISTER, 0)) {
        perror("rseq unregister");
        return 1;
    }

    printf("rseq unregistered successfully\n");
    free(__rseq_abi);
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#用户与组关系)
- 源码: `kernel/rseq.c`
- 头文件: `include/uapi/linux/rseq.h`, `include/linux/rseq.h`, `include/linux/rseq_entry.h`
- 文档: `Documentation/userspace-api/rseq.rst`
- 测试用例: `tools/testing/selftests/rseq/`
- 相关系统调用: `rseq_slice_yield()`, `membarrier()`