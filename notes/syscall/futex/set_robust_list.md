# set_robust_list 系统调用分析

## 1. 概述

`set_robust_list` 用于注册当前线程的健壮 futex 链表（robust list）。当线程意外退出（如被信号杀死、段错误等）时，内核会遍历该链表，标记所有该线程持有的 futex 为 "所有者已死亡"（`FUTEX_OWNER_DIED`），并唤醒等待在这些 futex 上的其他线程，从而避免死锁。

每个线程可以注册一个 robust list，内核在 `task_struct` 中保存该指针。线程退出时，`futex_cleanup()` 函数会调用 `exit_robust_list()` 遍历链表并清理。

## 2. 函数原型

```c
#include <linux/futex.h>
#include <sys/syscall.h>

long ret = syscall(SYS_set_robust_list,
    struct robust_list_head *head,  // 链表头指针
    size_t len);                    // 链表头大小
```

## 3. 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `head` | `struct robust_list_head*` | 用户空间链表头指针，存储到 `current->robust_list` |
| `len` | `size_t` | 链表头结构体大小，必须等于 `sizeof(struct robust_list_head)` |

## 4. 内核实现

```c
// kernel/futex/syscalls.c
SYSCALL_DEFINE2(set_robust_list, struct robust_list_head __user *, head,
                size_t, len)
{
    /*
     * The kernel knows only one size for now:
     */
    if (unlikely(len != sizeof(*head)))
        return -EINVAL;

    current->robust_list = head;

    return 0;
}
```

实现非常简单：
1. 验证 `len` 等于 `sizeof(struct robust_list_head)`，否则返回 `-EINVAL`
2. 将 `head` 指针存储到当前进程的 `task_struct->robust_list` 字段

## 5. 核心数据结构

### 5.1 struct robust_list_head

```c
// include/uapi/linux/futex.h
struct robust_list_head {
    struct robust_list list;             // 链表头
    long futex_offset;                   // futex 在锁结构体中的偏移量
    struct robust_list *list_op_pending; // 待处理操作链表指针
};
```

### 5.2 struct robust_list

```c
// include/uapi/linux/futex.h
struct robust_list {
    struct robust_list *next;  // 链表中的下一个节点
};
```

### 5.3 task_struct 中的相关字段

```c
// include/linux/sched.h
struct task_struct {
    // ...
    struct robust_list_head __user *robust_list;       // 健壮 futex 链表头
#ifdef CONFIG_COMPAT
    struct compat_robust_list_head __user *compat_robust_list; // 32 位兼容模式
#endif
    // ...
};
```

## 6. 线程退出时的清理流程

```
do_exit()                                          // kernel/exit.c
  └─ exit_task_work()                              // 执行退出任务
       └─ futex_cleanup(tsk)                       // kernel/futex/core.c
            ├─ exit_robust_list(tsk)               // 遍历 robust list
            │    └─ 循环遍历链表中的每个锁:
            │         ├─ fetch_robust_entry()       // 获取链表项
            │         ├─ handle_futex_death()       // 标记所有者死亡
            │         │    ├─ futex_get_value_locked()  // 读 futex 值
            │         │    ├─ 检查 TID 匹配
            │         │    ├─ cmpxchg 设置 FUTEX_OWNER_DIED
            │         │    └─ futex_wake()          // 唤醒等待者
            │         └─ 处理 list_op_pending
            │
            ├─ [compat] compat_exit_robust_list(tsk) // 32 位兼容模式
            │
            └─ exit_pi_state_list(tsk)              // 清理 PI 状态
                 └─ 清理 PI futex 的 pi_state
```

## 7. 流程图

```
set_robust_list 注册流程:
=========================

用户态线程:
  1. 分配 struct robust_list_head
  2. 初始化链表（head->list.next = &head->list）
  3. 调用 set_robust_list(&head, sizeof(head))
  4. 内核保存指针到 current->robust_list

线程退出时的清理流程:
======================

  线程意外退出 (do_exit)
       │
       v
  futex_cleanup()
       │
       v
  exit_robust_list()
       │
       ├─ 读取链表头 (head->list.next)
       ├─ 读取 futex_offset
       ├─ 读取 list_op_pending
       │
       └─ 循环遍历链表:
            │
            ├─ 获取下一个节点
            ├─ [非 pending 项] 调用 handle_futex_death()
            │    ├─ 读取 futex 值
            │    ├─ TID == current->pid?
            │    │    ├─ 是: 设置 FUTEX_OWNER_DIED 位
            │    │    │       唤醒等待者 (futex_wake)
            │    │    └─ 否: 跳过（已不属于该线程）
            │    └─ 继续遍历
            │
            └─ 处理 pending 项
                 └─ 同样调用 handle_futex_death()
```

## 8. handle_futex_death 详解

```c
// kernel/futex/core.c
/*
 * handle_futex_death() - 处理一个死亡线程持有的 futex
 * 检查 futex 值是否仍属于当前线程（TID 匹配），
 * 如果是，则设置 FUTEX_OWNER_DIED 位并唤醒等待者
 *
 * @uaddr: futex 用户空间地址
 * @curr: 死亡线程的 task_struct
 * @pi: 是否为 PI futex
 */
static int handle_futex_death(u32 __user *uaddr, struct task_struct *curr,
                              int pi, unsigned int flags)
{
    u32 uval, nval, mval;
    // PI futex 的处理
    if (pi) {
        // ...
    }

retry:
    if (get_user(uval, uaddr))
        return -1;
    // 检查 TID 是否匹配（低 30 位）
    if ((uval & FUTEX_TID_MASK) != task_pid_vnr(curr))
        return 0;

    // 设置 FUTEX_OWNER_DIED 位
    mval = uval & FUTEX_OWNER_DIED;
    nval = uval & ~FUTEX_TID_MASK;
    nval |= FUTEX_OWNER_DIED;

    // 原子 CAS 更新
    if (cmpxchg_futex_value_locked(&uval, uaddr, uval, nval)) {
        // 更新失败，重试
        goto retry;
    }

    // 唤醒等待者
    if (mval != FUTEX_OWNER_DIED)
        futex_wake(uaddr, 1, 1, FUTEX_BITSET_MATCH_ANY);

    return 0;
}
```

## 9. 使用示例

```c
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

// 健壮的 futex 锁结构
typedef struct {
    atomic_int lock;           // futex 字
    // 必须包含 robust_list 链表的嵌入
    struct robust_list list;   // 链表节点（嵌入到锁结构中）
} robust_futex_t;

// 全局链表头
static struct robust_list_head rhead;

// 当前线程持有的锁列表（简化示例）
static robust_futex_t *held_lock = NULL;

// 初始化 robust list
void setup_robust_list(void) {
    rhead.list.next = &rhead.list;  // 空链表
    rhead.futex_offset = offsetof(robust_futex_t, lock)
                         - offsetof(robust_futex_t, list);
    rhead.list_op_pending = NULL;

    int ret = syscall(SYS_set_robust_list, &rhead, sizeof(rhead));
    if (ret != 0) {
        perror("set_robust_list");
        exit(1);
    }
}

// 加锁（需要将锁加入 robust list）
void robust_lock(robust_futex_t *rf) {
    // 将锁加入链表
    rf->list.next = rhead.list.next;
    rhead.list.next = &rf->list;
    rhead.list_op_pending = &rf->list;

    // 尝试获取锁
    while (atomic_exchange(&rf->lock, 1) != 0) {
        // futex 等待
        syscall(SYS_futex, &rf->lock, FUTEX_WAIT, 1, NULL, NULL, 0);
    }

    // 获取锁成功，清除 pending
    rhead.list_op_pending = NULL;
}

// 解锁（从链表移除）
void robust_unlock(robust_futex_t *rf) {
    atomic_store(&rf->lock, 0);
    // 从链表移除（简化实现）
    rhead.list.next = rf->list.next;
    // 唤醒等待者
    syscall(SYS_futex, &rf->lock, FUTEX_WAKE, 1, NULL, NULL, 0);
}

// 测试：线程函数
void *worker(void *arg) {
    char *name = (char *)arg;
    robust_futex_t rf = { .lock = 0 };

    setup_robust_list();
    printf("%s: robust list 已注册\n", name);

    robust_lock(&rf);
    printf("%s: 获取锁，模拟工作...\n", name);
    sleep(2);  // 模拟工作，如果在此处崩溃，内核会清理
    robust_unlock(&rf);

    return NULL;
}

int main() {
    pthread_t t;
    pthread_create(&t, NULL, worker, "线程1");
    pthread_join(t, NULL);
    return 0;
}
```

## 10. 关键要点

1. **用户空间链表**：robust list 完全位于用户空间，内核在退出时读取该链表，因此链表必须对内核可访问
2. **链表限制**：内核遍历链表时设置了 `ROBUST_LIST_LIMIT` 限制，防止无限循环或过长的链表
3. **PI 检测**：链表项的最低有效位（bit 0）用于标识该 futex 是否为 PI futex
4. **list_op_pending**：用于处理"获取锁但尚未加入链表"的竞态窗口，确保该情况也能被正确清理
5. **兼容性**：32 位程序在 64 位内核上使用 `compat_robust_list` 和 `compat_robust_list_head`

## 11. 源码位置

| 文件 | 说明 |
|------|------|
| [kernel/futex/syscalls.c](file:///home/louis/code/linux/kernel/futex/syscalls.c) | `sys_set_robust_list` 实现 |
| [kernel/futex/core.c](file:///home/louis/code/linux/kernel/futex/core.c) | `exit_robust_list`、`handle_futex_death`、`futex_cleanup` |
| [include/uapi/linux/futex.h](file:///home/louis/code/linux/include/uapi/linux/futex.h) | `struct robust_list`、`struct robust_list_head` 定义 |
| [include/linux/sched.h](file:///home/louis/code/linux/include/linux/sched.h) | `task_struct` 中的 `robust_list` 字段 |