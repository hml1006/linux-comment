# gettid 系统调用分析

## 1. 概述

`gettid` 系统调用用于获取当前线程的线程 ID（TID）。在多线程进程中，每个线程有唯一的 TID，而 `getpid` 返回的线程组 ID（tgid）对所有线程相同。

### 关键特点

- 返回值为 `task_pid_vnr(current)`，即当前线程的 PID
- 在单线程进程中，gettid 返回值与 getpid 相同
- 在多线程进程中，每个线程有不同的 TID，但所有线程共享相同的 tgid（getpid 返回值）
- 实现极其简单，仅读取 `task_struct` 中的 `pid` 字段

---

## 2. 函数原型

```c
#include <unistd.h>

pid_t gettid(void);
```

### 内核入口

```c
// kernel/sys.c:1005
SYSCALL_DEFINE0(gettid)
{
    return task_pid_vnr(current);
}
```

---

## 3. 调用链分析

### 完整调用链

```
gettid()
└─ syscall(__NR_gettid)
   └─ SYSCALL_DEFINE0(gettid)              // kernel/sys.c:1005
      └─ task_pid_vnr(current)              // include/linux/sched.h
         └─ pid_vnr(task->pids[PIDTYPE_PID].pid)
            └─ pid_nr_ns(pid, ns)           // 转换为命名空间中的编号
```

### task_pid_vnr 实现

```c
// include/linux/sched.h
static inline pid_t task_pid_vnr(struct task_struct *tsk)
{
    return pid_vnr(task_pid(tsk));
}

// 线程的 PID 即 task_struct 中的 pid
static inline struct pid *task_pid(struct task_struct *task)
{
    return task->pids[PIDTYPE_PID].pid;
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
// ========== task_struct 中的 PID 相关字段 (include/linux/sched.h) ==========

struct task_struct {
    pid_t pid;                              // 线程 ID（TID）
    // ...
    struct task_struct *group_leader;       // 线程组组长（tgid 通过此获取）
    // ...
    struct pid *pids[PIDTYPE_MAX];          // 各种 PID 类型的 PID 结构
    // ...
};

// ========== PID 类型说明 ==========

// PIDTYPE_PID:  线程 ID（TID），每个线程唯一
// PIDTYPE_TGID: 线程组 ID（tgid），线程组组长（group_leader）的 PID
// PIDTYPE_PGID: 进程组 ID
// PIDTYPE_SID:  会话 ID

// 多线程示例:
// 进程 A: 主线程 task_struct.pid = 1000, group_leader = 自身
// 进程 A: 子线程 task_struct.pid = 1001, group_leader = 主线程
// getpid() 返回 1000（所有线程相同）
// gettid() 在主线程返回 1000，在子线程返回 1001
```

---

## 5. 流程图

```
                     gettid()
                        |
                +-------v--------+
                | SYSCALL_DEFINE0 |
                | (kernel/sys.c)  |
                +-------+--------+
                        |
                +-------v--------+
                | task_pid_vnr   |
                | (current)      |
                +-------+--------+
                        |
                +-------v--------+
                | task_pid()     |
                | → current->    |
                |   pids[        |
                |    PIDTYPE_PID |
                |    ].pid       |
                +-------+--------+
                        |
                +-------v--------+
                | pid_vnr(pid)   |
                | → pid_nr_ns()  |
                +-------+--------+
                        |
                +-------v--------+
                | 返回 TID 值    |
                +----------------+
```

---

## 6. 错误处理

`gettid` 系统调用总是成功，没有错误返回值。

---

## 7. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>

void *thread_func(void *arg) {
    printf("线程: PID=%d, TID=%d\n", getpid(), gettid());
    return NULL;
}

int main() {
    pthread_t thread;

    printf("主线程: PID=%d, TID=%d\n", getpid(), gettid());

    pthread_create(&thread, NULL, thread_func, NULL);
    pthread_join(thread, NULL);

    return 0;
}
```

---

## 8. 与 getpid 对比

| 特性 | getpid | gettid |
|------|--------|--------|
| **返回值** | 线程组 ID（tgid） | 线程 ID（TID） |
| **内核函数** | `task_tgid_vnr(current)` | `task_pid_vnr(current)` |
| **单线程** | 返回进程 PID | 相同值 |
| **多线程** | 所有线程相同 | 每个线程不同 |
| **glibc 封装** | `getpid()` | 无（需 syscall） |
| **系统调用号** | 172（ARM64） | 178（ARM64） |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `kernel/sys.c:1005` - SYSCALL_DEFINE0(gettid)
- `include/linux/pid.h` - struct pid 定义
- `include/linux/sched.h` - task_pid_vnr 宏定义