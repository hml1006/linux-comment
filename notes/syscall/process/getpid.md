# getpid 系统调用分析

## 1. 概述

`getpid` 系统调用用于获取当前进程的进程 ID（PID）。它返回调用进程的线程组 ID（tgid），即进程组组长的 PID。

### 关键特点

- 返回值为 `task_tgid_vnr(current)`，即当前进程的线程组 ID
- 在 PID 命名空间中，返回的是相对于当前命名空间的虚拟 PID
- 实现极其简单，仅需读取 `task_struct` 中的 `tgid` 字段

---

## 2. 函数原型

```c
#include <unistd.h>

pid_t getpid(void);
```

### 内核入口

```c
// kernel/sys.c:999
SYSCALL_DEFINE0(getpid)
{
    return task_tgid_vnr(current);
}
```

---

## 3. 调用链分析

### 完整调用链

```
getpid()
└─ syscall(__NR_getpid)
   └─ SYSCALL_DEFINE0(getpid)              // kernel/sys.c:999
      └─ task_tgid_vnr(current)             // include/linux/sched.h
         └─ pid_vnr(task->group_leader->pid) // 或通过 task_tgid() 获取
            └─ pid_nr_ns(pid, ns)           // 转换为命名空间中的编号
```

### task_tgid_vnr 实现

```c
// include/linux/sched.h
static inline pid_t task_tgid_vnr(struct task_struct *tsk)
{
    return pid_vnr(task_tgid(tsk));
}

// 线程组 ID 即 group_leader 的 PID
static inline struct pid *task_tgid(struct task_struct *task)
{
    return task->group_leader->pids[PIDTYPE_TGID].pid;
}

// 转换为 PID 命名空间中的虚拟编号
pid_t pid_vnr(struct pid *pid)
{
    return pid_nr_ns(pid, current->nsproxy->pid_ns_for_children);
}
```

---

## 4. 关键数据结构

```c
// ========== PID 结构 (include/linux/pid.h) ==========

struct pid {
    struct hlist_node entries[PIDTYPE_MAX];  // 每种 PID 类型一个哈希表节点
    struct hlist_head tasks[PIDTYPE_MAX];    // 链表头
    struct callback_head rcu;                // RCU 回调
    unsigned int level;                      // PID 命名空间层级
    struct upid numbers[1];                  // 各级命名空间的 PID 值（变长数组）
};

struct upid {
    struct hlist_node pid_chain;             // PID 哈希链表节点
    int nr;                                  // 实际的 PID 值
    struct pid_namespace *ns;                // 所属 PID 命名空间
};

// PID 类型枚举
enum pid_type {
    PIDTYPE_PID,     // 进程 ID
    PIDTYPE_TGID,    // 线程组 ID
    PIDTYPE_PGID,    // 进程组 ID
    PIDTYPE_SID,     // 会话 ID
    PIDTYPE_MAX,     // 边界
};

// ========== task_struct 中的 PID 相关字段 (include/linux/sched.h) ==========

struct task_struct {
    pid_t pid;                              // 线程 ID（TID，轻量级进程标识）
    // ...
    struct task_struct *group_leader;       // 线程组组长
    // ...
    struct pid *pids[PIDTYPE_MAX];          // 各种 PID 类型的 PID 结构
    // ...
};
```

---

## 5. 流程图

```
                     getpid()
                        |
                +-------v--------+
                | SYSCALL_DEFINE0 |
                | (kernel/sys.c)  |
                +-------+--------+
                        |
                +-------v--------+
                | task_tgid_vnr  |
                | (current)      |
                +-------+--------+
                        |
                +-------v--------+
                | task_tgid()    |
                | → current->    |
                |   group_leader |
                |   ->pids[      |
                |    PIDTYPE_TGID|
                |    ].pid       |
                +-------+--------+
                        |
                +-------v--------+
                | pid_vnr(pid)   |
                | → pid_nr_ns()  |
                |   (转换为当前  |
                |    PID 命名    |
                |    空间的编号)  |
                +-------+--------+
                        |
                +-------v--------+
                | 返回 PID 值    |
                +----------------+
```

---

## 6. 错误处理

`getpid` 系统调用总是成功，没有错误返回值。

---

## 7. 使用示例

```c
#include <unistd.h>
#include <stdio.h>

int main() {
    pid_t pid = getpid();
    pid_t ppid = getppid();

    printf("当前进程 PID: %d\n", pid);
    printf("父进程 PPID: %d\n", ppid);

    return 0;
}
```

---

## 8. 与 gettid 对比

| 特性 | getpid | gettid |
|------|--------|--------|
| **返回值** | 线程组 ID（tgid） | 线程 ID（pid） |
| **内核函数** | `task_tgid_vnr(current)` | `task_pid_vnr(current)` |
| **单线程进程** | 与 gettid 相同 | 与 getpid 相同 |
| **多线程进程** | 所有线程相同 | 每个线程不同 |
| **系统调用号** | 172（ARM64） | 178（ARM64） |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/sys.c:999` - SYSCALL_DEFINE0(getpid)
- `include/linux/pid.h` - struct pid 定义
- `include/linux/sched.h` - task_tgid_vnr 宏定义