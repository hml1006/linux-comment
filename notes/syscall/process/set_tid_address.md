# set_tid_address 系统调用分析

## 1. 概述

`set_tid_address` 系统调用用于设置 `clear_child_tid` 指针。当线程退出时，内核会向该地址写入 0 并唤醒等待在该地址上的 futex，从而通知其他线程（如 `pthread_join` 的等待者）该线程已退出。

### 关键特点

- 设置 `current->clear_child_tid = tidptr`
- 当线程退出时（`mm_release` 中），内核向 `tidptr` 写入 0 并唤醒 futex
- 返回值是调用线程的 TID（与 `gettid` 相同）
- 主要用于 `pthread` 库的线程清理机制

---

## 2. 函数原型

```c
#include <linux/unistd.h>
#include <sys/syscall.h>

pid_t set_tid_address(int *tidptr);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `tidptr` | 指向用户空间整数的指针，线程退出时写入 0 |

### 内核入口

```c
// kernel/fork.c:1782
SYSCALL_DEFINE1(set_tid_address, int __user *, tidptr)
{
    current->clear_child_tid = tidptr;

    return task_pid_vnr(current);
}
```

---

## 3. 调用链分析

### 完整调用链

```
set_tid_address(tidptr)
└─ syscall(__NR_set_tid_address, tidptr)
   └─ SYSCALL_DEFINE1(set_tid_address)            // kernel/fork.c:1782
      ├─ current->clear_child_tid = tidptr         // 设置指针
      └─ return task_pid_vnr(current)              // 返回当前线程 TID
```

### 线程退出时的工作流程

当线程退出时，`exit_mm` → `mm_release` 中会检查 `clear_child_tid`：

```c
// kernel/fork.c
void mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
    // ...
    // 如果设置了 clear_child_tid，通知等待线程
    if (tsk->clear_child_tid) {
        int __user *tidptr = tsk->clear_child_tid;

        // 向用户空间地址写入 0
        put_user(0, tidptr);
        // 唤醒等待在该 futex 地址上的线程
        futex_wake(tidptr, 1, FUTEX_WAKE);
    }
    // ...
}
```

---

## 4. 关键数据结构

```c
// ========== task_struct 中的 clear_child_tid 字段 (include/linux/sched.h) ==========

struct task_struct {
    // ...
    struct mm_struct *mm;               // 地址空间
    // ...
    int __user *clear_child_tid;        // 线程退出时写入 0 的用户空间地址
    // ...
};

// ========== CLONE_CHILD_CLEARTID 标志 (include/uapi/linux/sched.h) ==========

#define CLONE_CHILD_CLEARTID    0x00200000  // 子进程退出时清除 TID

// 该标志在 clone/clone3 中设置，内核自动将 child_tid 地址设置到 clear_child_tid
```

---

## 5. 流程图

```
                     set_tid_address(tidptr)
                              |
                     +--------v--------+
                     | SYSCALL_DEFINE1 |
                     | (kernel/fork.c) |
                     +--------+--------+
                              |
                     +--------v--------+
                     | current->clear_ |
                     | child_tid =     |
                     | tidptr          |
                     +--------+--------+
                              |
                     +--------v--------+
                     | 返回 task_pid_  |
                     | vnr(current)    |
                     +-----------------+

   线程退出时 (do_exit → exit_mm → mm_release):
                              |
                     +--------v--------+
                     | mm_release()    |
                     | (kernel/fork.c) |
                     +--------+--------+
                              |
                     +--------v--------+
                     | clear_child_tid |
                     | 是否为 NULL?    |
                     +--------+--------+
                              |
                     +--------v--------+
                     | put_user(0,     |
                     | tidptr)         |
                     | → 写入 0 到用户 |
                     |   空间地址      |
                     +--------+--------+
                              |
                     +--------v--------+
                     | futex_wake(     |
                     | tidptr, 1)      |
                     | → 唤醒等待线程  |
                     +-----------------+
```

---

## 6. 错误处理

`set_tid_address` 系统调用总是成功，没有错误返回值。它仅仅设置一个指针并返回当前 TID。

---

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/time.h>

int main() {
    int tid;
    int futex_val = 1;

    // 设置 clear_child_tid 地址
    pid_t tid_result = syscall(SYS_set_tid_address, &futex_val);
    printf("set_tid_address 返回: %d\n", tid_result);

    // 模拟线程退出时，内核会向 futex_val 写入 0
    // 此时 futex_val 变为 0，等待者可以检测到

    return 0;
}
```

---

## 8. 与 CLONE_CHILD_CLEARTID 的关系

| 机制 | 说明 |
|------|------|
| **CLONE_CHILD_CLEARTID** | `clone` 标志，子线程退出时自动清除 TID |
| **set_tid_address** | 手动设置 `clear_child_tid` 指针 |
| **mm_release** | 线程退出时自动执行清除操作 |

`set_tid_address` 通常由 `pthread` 库在创建线程时调用，用于设置退出通知机制。

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/fork.c:1782` - SYSCALL_DEFINE1(set_tid_address)
- `kernel/fork.c` - mm_release 中的 clear_child_tid 处理