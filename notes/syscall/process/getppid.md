# getppid 系统调用分析

## 1. 概述

`getppid` 系统调用用于获取当前进程的父进程 ID（PPID）。它返回调用进程的`real_parent` 的线程组 ID。

### 关键特点

- 返回值是 `real_parent` 的线程组 ID（tgid），而非 `parent`
- `real_parent` 是实际创建本进程的父进程，而 `parent` 可能因 ptrace 而不同
- 通过 `rcu_read_lock` 保护，防止父进程在读取期间被回收
- 在 PID 命名空间中，返回的是相对于当前命名空间的虚拟 PID

---

## 2. 函数原型

```c
#include <unistd.h>

pid_t getppid(void);
```

### 内核入口

```c
// kernel/sys.c:1016
SYSCALL_DEFINE0(getppid)
{
    int pid;

    rcu_read_lock();
    pid = task_tgid_vnr(rcu_dereference(current->real_parent));
    rcu_read_unlock();

    return pid;
}
```

---

## 3. 调用链分析

### 完整调用链

```
getppid()
└─ syscall(__NR_getppid)
   └─ SYSCALL_DEFINE0(getppid)             // kernel/sys.c:1016
      ├─ rcu_read_lock()                    // RCU 读锁定
      ├─ rcu_dereference(current->real_parent)  // 安全读取 real_parent
      ├─ task_tgid_vnr(parent)              // 获取父进程的线程组 ID
      │  └─ pid_vnr(parent->group_leader->pids[PIDTYPE_TGID].pid)
      └─ rcu_read_unlock()                  // RCU 读解锁
```

### 为什么需要 RCU 保护

`real_parent` 指针可能被并发修改（如父进程退出时通过 `forget_original_parent` 重新指定父进程）。使用 `rcu_dereference` 确保读取的一致性，同时 `rcu_read_lock` 防止父进程的 `task_struct` 在读取期间被释放。

```c
// include/linux/sched.h
/*
 * 注意: 访问 ->real_parent 不是 SMP 安全的，
 * 它可能在运行时改变。
 * 然而，在 rcu_read_lock() 下可以使用过时的值，
 * 参见 release_task() → call_rcu(delayed_put_task_struct)。
 */
```

---

## 4. 关键数据结构

```c
// ========== task_struct 中的父子关系字段 (include/linux/sched.h) ==========

struct task_struct {
    // ...
    struct task_struct __rcu *real_parent;  // 实际父进程（创建本进程的进程）
    struct task_struct __rcu *parent;       // 接收信号的父进程（ptrace 时可能不同）
    struct list_head children;              // 子进程链表
    struct list_head sibling;               // 兄弟进程节点
    // ...
    struct signal_struct *signal;           // 信号结构
    // ...
};

// ========== 父进程死亡时的处理 ==========

// 当父进程退出时，forget_original_parent 会重新分配孤儿进程的父进程
// 通过 find_new_reaper 找到新的父进程（通常是 init 或最近的子 reaper）
static void forget_original_parent(struct task_struct *parent,
                                    struct list_head *dead_reaper)
{
    struct task_struct *p, *reaper;

    // 遍历所有子进程，重新分配父进程
    list_for_each_entry_safe(p, ...) {
        reaper = find_new_reaper(parent, dead_reaper);
        // 将子进程的 real_parent 指向 reaper
        rcu_assign_pointer(p->real_parent, reaper);
    }
}
```

---

## 5. 流程图

```
                     getppid()
                        |
                +-------v--------+
                | SYSCALL_DEFINE0 |
                | (kernel/sys.c)  |
                +-------+--------+
                        |
                +-------v--------+
                | rcu_read_lock()|  RCU 读锁定
                +-------+--------+
                        |
                +-------v--------+
                | rcu_dereference|
                | (current->     |
                |  real_parent)  |  安全读取父进程指针
                +-------+--------+
                        |
                +-------v--------+
                | task_tgid_vnr  |
                | (parent)       |  获取父进程的 tgid
                +-------+--------+
                        |
                +-------v--------+
                | rcu_read_unlock|
                +-------+--------+
                        |
                +-------v--------+
                | 返回 PPID 值   |
                +----------------+
```

---

## 6. 错误处理

`getppid` 系统调用总是成功，没有错误返回值。

---

## 7. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>

int main() {
    pid_t pid = fork();

    if (pid == 0) {
        // 子进程
        printf("子进程: PID=%d, PPID=%d\n", getpid(), getppid());
        return 0;
    } else if (pid > 0) {
        printf("父进程: PID=%d, PPID=%d\n", getpid(), getppid());
        wait(NULL);
    }

    return 0;
}
```

---

## 8. 与 getpid 的关系

| 特性 | getpid | getppid |
|------|--------|---------|
| **返回值** | 当前进程的 PID | 父进程的 PID |
| **内核函数** | `task_tgid_vnr(current)` | `task_tgid_vnr(current->real_parent)` |
| **同步保护** | 不需要 | 需要 RCU 保护 |
| **失败情况** | 不会失败 | 不会失败 |
| **系统调用号** | 172（ARM64） | 173（ARM64） |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/sys.c:1016` - SYSCALL_DEFINE0(getppid)
- `include/linux/sched.h` - task_struct 定义