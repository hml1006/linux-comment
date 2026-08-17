# process_mrelease 系统调用分析

## 1. 概述

`process_mrelease` 系统调用用于在进程死亡后尽快释放其内存。它是在 Linux 5.12 中引入的，主要用于用户空间 OOM 处理程序（如 systemd-oomd），在杀死进程后立即回收其内存，避免 OOM 处理程序本身成为 OOM 的受害者。

### 关键特点

- 只能对已标记 `MMF_OOM_SKIP` 的进程（即已被 OOM killer 杀死或正在退出的进程）操作
- 通过 `pidfd` 引用目标进程，避免 PID 重用问题
- 调用 `__oom_reap_task_mm` 执行实际的内存回收
- 仅当目标进程的 `task_will_free_mem` 为真时才能成功回收

---

## 2. 函数原型

```c
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>

int process_mrelease(int pidfd, unsigned int flags);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `pidfd` | 目标进程的文件描述符（通过 `pidfd_open` 获取） |
| `flags` | 保留，必须为 0 |

### 内核入口

```c
// mm/oom_kill.c:1211
SYSCALL_DEFINE2(process_mrelease, int, pidfd, unsigned int, flags)
{
#ifdef CONFIG_MMU
    struct mm_struct *mm = NULL;
    struct task_struct *task;
    struct task_struct *p;
    unsigned int f_flags;
    bool reap = false;
    long ret = 0;

    if (flags)
        return -EINVAL;

    task = pidfd_get_task(pidfd, &f_flags);
    if (IS_ERR(task))
        return PTR_ERR(task);

    p = find_lock_task_mm(task);
    if (!p) {
        ret = -ESRCH;
        goto put_task;
    }

    mm = p->mm;
    mmgrab(mm);

    if (task_will_free_mem(p))
        reap = true;
    else {
        if (!mm_flags_test(MMF_OOM_SKIP, mm))
            ret = -EINVAL;
    }
    task_unlock(p);

    if (!reap)
        goto drop_mm;

    if (mmap_read_lock_killable(mm)) {
        ret = -EINTR;
        goto drop_mm;
    }
    if (!mm_flags_test(MMF_OOM_SKIP, mm) && !__oom_reap_task_mm(mm))
        ret = -EAGAIN;
    mmap_read_unlock(mm);

drop_mm:
    mmdrop(mm);
put_task:
    put_task_struct(task);
    return ret;
#else
    return -ENOSYS;
#endif
}
```

---

## 3. 调用链分析

### 完整调用链

```
process_mrelease(pidfd, flags)
└─ syscall(__NR_process_mrelease, pidfd, flags)
   └─ SYSCALL_DEFINE2(process_mrelease)            // mm/oom_kill.c:1211
      ├─ [flags != 0] → -EINVAL                     // flags 必须为 0
      ├─ pidfd_get_task(pidfd, &f_flags)             // 通过 pidfd 获取 task_struct
      │  └─ pidfd_fdget() → fget()
      │  └─ pidfd_get_pid()
      │  └─ get_pid_task(pid, PIDTYPE_TGID)
      ├─ find_lock_task_mm(task)                     // 查找有 mm 的线程
      ├─ mmgrab(mm)                                  // 增加 mm 引用计数
      ├─ task_will_free_mem(p)                       // 检查进程是否将释放内存
      │  ├─ [p->flags & PF_EXITING]                  // 正在退出
      │  └─ [atomic_read(&p->signal->live) > 1]      // 线程组中还有存活线程
      ├─ [reap] → mmap_read_lock_killable(mm)        // 获取 mmap 读锁
      ├─ [reap] → __oom_reap_task_mm(mm)              // 执行内存回收
      │  └─ oom_reap_task_mm(mm, NULL)                // 核心回收函数
      │     ├─ mmap_read_lock(mm)
      │     ├─ io_uring_reap_mm(mm)                   // 清理 io_uring 内存
      │     ├─ for each VMA:
      │     │  └─ unmap_page_range()                  // 解除页表映射
      │     │     └─ zap_page_range()                 // 释放页表
      │     ├─ mmap_read_unlock(mm)
      │     └─ set_bit(MMF_OOM_SKIP, &mm->flags)     // 标记完成
      ├─ mmap_read_unlock(mm)                         // 释放 mmap 读锁
      ├─ mmdrop(mm)                                   // 释放 mm 引用
      └─ put_task_struct(task)                        // 释放 task 引用
```

---

## 4. 关键数据结构

```c
// ========== mm_struct 标志 (include/linux/mm_types.h) ==========

#define MMF_OOM_SKIP      19  // OOM 回收已完成，跳过（由 __oom_reap_task_mm 设置）

// ========== 任务内存释放检查 (include/linux/oom.h) ==========

// 检查进程是否将释放内存（正在退出且有线程存活）
static inline bool task_will_free_mem(struct task_struct *task)
{
    // 进程必须设置 PF_EXITING 标志
    // 线程组中必须有存活线程（signal->live > 0）
    return (task->flags & PF_EXITING) &&
           (atomic_read(&task->signal->live) > 0);
}

// ========== PF_EXITING 标志 (include/linux/sched.h) ==========

#define PF_EXITING      0x00000004  // 进程正在退出
```

---

## 5. 流程图

```
                    process_mrelease(pidfd, flags)
                                      |
                            +---------v----------+
                            | SYSCALL_DEFINE2     |
                            | (mm/oom_kill.c)     |
                            +---------+----------+
                                      |
                     +----------------+----------------+
                     |                                |
              +------v------+                  +------v------+
              | pidfd_get_  |                  | 检查 flags  |
              | task(pidfd) |                  | (必须为 0)  |
              | → 获取目标  |                  +------+------+
              |   task      |                         |
              +------+------+                  +------v------+
                     |                         | 返回 -EINVAL|
              +------v------+                  +-------------+
              | find_lock_  |
              | task_mm(p)  |
              | → 查找有 mm |
              |   的线程    |
              +------+------+
                     |
              +------v------+
              | task_will_  |
              | free_mem(p) |
              +------+------+
                     |
         +-----------+-----------+
         |                       |
    +----v----+             +----v----+
    | true    |             | false   |
    | (reap)  |             | 检查    |
    +----+----+             |MMF_OOM_|
         |                  |SKIP    |
    +----v----+             +----+---+
    |__oom_   |                 |
    |reap_task|          +------v------+
    |_mm(mm)  |          | 已标记?     |
    +----+----+          | 是: 返回 0  |
         |               | 否: -EINVAL |
    +----v----+          +-------------+
    | 设置     |
    |MMF_OOM_ |
    |SKIP     |
    +----+----+
         |
    +----v----+
    | 返回 0  |
    | (成功)   |
    +---------+
```

---

## 6. 错误处理

| 错误码 | 条件 | 触发位置 |
|--------|------|----------|
| `-EINVAL` | `flags` 参数非零 | `SYSCALL_DEFINE2` |
| `-EBADF` | `pidfd` 无效 | `pidfd_get_task` |
| `-ESRCH` | 目标进程没有 mm（已退出或内核线程） | `find_lock_task_mm` |
| `-EINVAL` | 目标进程未标记 `PF_EXITING` 且未设置 `MMF_OOM_SKIP` | `task_will_free_mem` 检查 |
| `-EAGAIN` | 内存回收失败（`__oom_reap_task_mm` 返回 false） | `__oom_reap_task_mm` |
| `-EINTR` | `mmap_read_lock_killable` 被信号中断 | `mmap_read_lock_killable` |
| `-ENOSYS` | 未配置 `CONFIG_MMU` | `#ifdef CONFIG_MMU` |

---

## 7. 使用示例

```c
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

static int pidfd_open(pid_t pid, unsigned int flags)
{
    return syscall(SYS_pidfd_open, pid, flags);
}

int main() {
    pid_t pid = fork();

    if (pid == 0) {
        // 子进程：分配大量内存
        size_t size = 1024 * 1024 * 100;  // 100 MB
        char *p = malloc(size);
        if (p) {
            for (size_t i = 0; i < size; i += 4096)
                p[i] = 1;  // 触发缺页
        }
        sleep(60);  // 保持存活
        exit(0);
    }

    // 父进程：等待子进程退出后释放其内存
    sleep(1);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    // 打开 pidfd 并调用 process_mrelease
    int pidfd = pidfd_open(pid, 0);
    if (pidfd == -1) {
        perror("pidfd_open");
        return 1;
    }

    int ret = syscall(SYS_process_mrelease, pidfd, 0);
    if (ret == 0)
        printf("内存释放成功\n");
    else
        perror("process_mrelease");

    close(pidfd);
    return 0;
}
```

---

## 8. 与 OOM 机制的关系

`process_mrelease` 是用户空间 OOM 处理框架的一部分：

| 组件 | 角色 |
|------|------|
| **OOM killer（内核）** | 选择要杀死的进程，发送 SIGKILL |
| **systemd-oomd（用户态）** | 用户空间 OOM 检测和处理程序 |
| **process_mrelease** | 杀死进程后立即回收其内存 |
| **__oom_reap_task_mm** | 核心内存回收函数（与内核 OOM 复用） |

传统流程：OOM killer 杀死进程 → 进程退出 → `exit_mm` → 释放内存
新流程：systemd-oomd 杀死进程 → `process_mrelease` → 立即回收内存（无需等待进程退出）

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `mm/oom_kill.c:1211` - SYSCALL_DEFINE2(process_mrelease)
- `mm/oom_kill.c` - __oom_reap_task_mm 实现
- `include/linux/oom.h` - task_will_free_mem 定义