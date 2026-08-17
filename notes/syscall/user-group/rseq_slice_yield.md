# rseq_slice_yield 系统调用分析

## 1. 概述

`rseq_slice_yield` 是 rseq（Restartable Sequences）切片扩展机制的一部分。当线程被授予时间片扩展（Time Slice Extension）时，如果线程在扩展到期前完成了临界区工作，可以调用此系统调用无副作用地让出 CPU，避免被强制调度出去。

**原型：**

```c
SYSCALL_DEFINE0(rseq_slice_yield)
```

**参数：** 无

**返回值：**
- 返回 1：成功在授予的切片内让出 CPU
- 返回 0：切片扩展从未被授予，或已被撤销

## 2. 使用场景

- rseq 临界区中使用时间片扩展，完成后主动让出 CPU
- 实时/低延迟应用中避免被强制调度造成性能抖动
- 多线程 per-CPU 数据结构操作完成后主动让出

## 3. 函数调用栈

### 3.1 rseq_slice_yield 系统调用

```
SYSCALL_DEFINE0(rseq_slice_yield)                      // kernel/rseq.c
  └─ 返回 current->rseq.slice.yielded 的值
  └─ 将 yielded 重置为 0
```

### 3.2 实际工作发生在 syscall 入口处

```
系统调用入口 (syscall entry)
  └─ rseq_syscall_enter_work(__NR_rseq_slice_yield)     // kernel/rseq.c
       ├─ 检查是否在 granted 状态
       ├─ scoped_guard(preempt) {
       │    ├─ rseq_cancel_slice_extension_timer()       // 取消切片扩展定时器
       │    ├─ 检查是否已被调度
       │    ├─ [未被调度] rseq_slice_set_need_resched()   // 设置需要重新调度
       │    ├─ rseq_stat_inc(rseq_stats.s_yielded)       // 统计
       │    └─ current->rseq.slice.yielded = 1           // 标记 yielded
       │  }
       ├─ cond_resched()                                 // 让出 CPU
       └─ 清除 grant 状态（内核和用户空间）
```

## 4. 关键数据结构

### 4.1 struct rseq（rseq 用户态结构体）

```c
// include/uapi/linux/rseq.h
struct rseq {
    __u32 cpu_id_start;          // 初始 CPU 编号（由内核更新）
    __u32 cpu_id;                // 当前 CPU 编号（由内核更新）
    __u64 rseq_cs;               // 指向当前 rseq_cs 的指针
    __u32 flags;                 // 标志位（含 RSEQ_CS_FLAG_SLICE_EXT_*）
    __u32 node_id;               // NUMA 节点 ID
    __u32 mm_cid;                // 内存空间内的 CPU ID
    struct rseq_slice_ctrl slice_ctrl;  // 切片控制（request/granted）
};
```

### 4.2 struct rseq_slice_ctrl（切片控制结构体）

```c
// include/uapi/linux/rseq.h
struct rseq_slice_ctrl {
    __u32 all;              // 整体控制字
    // bit 0: request  - 用户态请求时间片扩展
    // bit 1: granted  - 内核授予时间片扩展
};
```

### 4.3 内核端 rseq slice 状态

```c
// kernel/rseq.c (内部状态)
struct task_struct {
    // ...
    struct {
        struct {
            struct {
                bool enabled: 1;     // 切片扩展是否启用
                bool granted: 1;     // 当前是否已授予扩展
            } state;
            bool yielded;            // 是否已调用 yield
        } slice;
        struct rseq __user *usrptr;  // 指向用户空间 rseq 结构
        struct rseq_event event;     // rseq 事件标记
    } rseq;
    // ...
};
```

## 5. 流程图

```
用户态：rseq 临界区执行
    │
    ▼
┌─────────────────────────────────────┐
│  用户态设置 slice_ctrl.request = 1 │
│  （通过 RSEQ_WRITE_ONCE）           │
└─────────────────────────────────────┘
    │
    ▼ (中断/系统调用返回用户空间时)
┌─────────────────────────────────────┐
│  内核 rseq_grant_slice_extension() │
│  ├─ 检查 request 位                │
│  ├─ 设置 granted = 1              │
│  ├─ 设置定时器                     │
│  └─ 返回用户空间继续执行           │
└─────────────────────────────────────┘
    │
    ▼ (临界区完成)
┌─────────────────────────────────────┐
│  用户态检查 granted 位              │
│  │                                  │
│  ├─ granted=1: 调用 rseq_slice_yield│
│  │    │                             │
│  │    ▼                             │
│  │  syscall entry                   │
│  │  ├─ 取消定时器                   │
│  │  ├─ 设置 need_resched            │
│  │  ├─ cond_resched() 让出 CPU      │
│  │  └─ 清除 granted                 │
│  │                                  │
│  └─ granted=0: 正常继续             │
└─────────────────────────────────────┘
```

## 6. 错误处理

`rseq_slice_yield` 本身不产生错误码。状态通过返回值反映：

| 返回值 | 含义 |
|--------|------|
| 1 | 在授予的切片内成功让出 CPU |
| 0 | 从未被授予切片扩展，或扩展已被撤销 |

如果用户态在 `granted` 状态下调用了 `rseq_slice_yield` 之外的其他系统调用，该其他系统调用也会触发 `rseq_syscall_enter_work()`，但会标记为 `aborted` 而非 `yielded`。

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/prctl.h>
#include <sys/prctl.h>

#ifndef __NR_rseq_slice_yield
#define __NR_rseq_slice_yield 471
#endif

#ifndef PR_RSEQ_SLICE_EXTENSION
#define PR_RSEQ_SLICE_EXTENSION 79
#define PR_RSEQ_SLICE_EXTENSION_GET 1
#define PR_RSEQ_SLICE_EXTENSION_SET 2
#define PR_RSEQ_SLICE_EXT_ENABLE 0x01
#endif

#include <linux/rseq.h>

/* 需要链接 -lrseq 或手动实现 rseq 注册 */
extern struct rseq *rseq_get_abi(void);
extern int rseq_register_current_thread(void);

int main(void)
{
    struct rseq *rseq_abi;

    // 注册 rseq
    if (rseq_register_current_thread()) {
        perror("rseq_register");
        return 1;
    }

    // 启用切片扩展
    if (prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_SET,
              PR_RSEQ_SLICE_EXT_ENABLE, 0, 0)) {
        perror("prctl slice extension");
        return 1;
    }

    rseq_abi = rseq_get_abi();

    // 请求时间片扩展
    __atomic_store_n(&rseq_abi->slice_ctrl.request, 1, __ATOMIC_RELAXED);

    // 执行临界区工作...
    // 如果内核授予了扩展，critical section 不会被打断

    // 检查是否获得了扩展，主动让出 CPU
    if (__atomic_load_n(&rseq_abi->slice_ctrl.granted, __ATOMIC_RELAXED)) {
        int ret = syscall(__NR_rseq_slice_yield);
        if (ret == 1) {
            printf("Successfully yielded within slice extension\n");
        } else {
            printf("Slice extension was revoked before yield\n");
        }
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#用户与组关系)
- 源码: `kernel/rseq.c`（`SYSCALL_DEFINE0(rseq_slice_yield)` 和 `rseq_syscall_enter_work()`）
- 头文件: `include/uapi/linux/rseq.h`, `include/linux/rseq.h`, `include/linux/rseq_entry.h`
- 文档: `Documentation/userspace-api/rseq.rst`
- 测试用例: `tools/testing/selftests/rseq/slice_test.c`
- 系统调用号: 471（所有架构通用）
- 相关系统调用: `rseq()`, `prctl(PR_RSEQ_SLICE_EXTENSION, ...)`