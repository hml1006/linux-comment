# get_robust_list 系统调用分析

## 1. 概述

`get_robust_list` 用于获取指定线程的健壮 futex 链表（robust list）头指针。该链表由 `set_robust_list` 注册，内核在 `task_struct` 中保存该指针。`get_robust_list` 允许调试器或其他监控工具查看目标线程的 robust list，了解该线程持有的所有 futex 锁。

这是一个 `ptrace` 类操作，调用者需要对目标进程具有 `PTRACE_MODE_READ_REALCREDS` 权限。

## 2. 函数原型

```c
#include <linux/futex.h>
#include <sys/syscall.h>

long ret = syscall(SYS_get_robust_list,
    int pid,                               // 目标进程 PID（0 表示当前进程）
    struct robust_list_head **head_ptr,    // 输出：链表头指针
    size_t *len_ptr);                      // 输出：链表头结构体大小
```

## 3. 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `pid` | `int` | 目标进程 PID，为 0 时获取当前进程的 robust list |
| `head_ptr` | `struct robust_list_head**` | 输出参数，内核填充链表头指针 |
| `len_ptr` | `size_t*` | 输出参数，内核填充 `sizeof(struct robust_list_head)` |

## 4. 内核实现

```c
// kernel/futex/syscalls.c
SYSCALL_DEFINE3(get_robust_list, int, pid,
                struct robust_list_head __user * __user *, head_ptr,
                size_t __user *, len_ptr)
{
    struct robust_list_head __user *head = futex_get_robust_list_common(pid, false);

    if (IS_ERR(head))
        return PTR_ERR(head);

    if (put_user(sizeof(*head), len_ptr))
        return -EFAULT;
    return put_user(head, head_ptr);
}
```

### 4.1 核心辅助函数

```c
// kernel/futex/syscalls.c
static void __user *futex_get_robust_list_common(int pid, bool compat)
{
    struct task_struct *p = current;
    void __user *head;
    int ret;

    // 1. 查找目标进程
    scoped_guard(rcu) {
        if (pid) {
            p = find_task_by_vpid(pid);     // 通过 PID 查找 task_struct
            if (!p)
                return (void __user *)ERR_PTR(-ESRCH);
        }
        get_task_struct(p);                 // 增加引用计数
    }

    // 2. 序列化 exec() 操作
    // 持有 exec_update_lock 防止并发 exec() 更改凭证
    ret = down_read_killable(&p->signal->exec_update_lock);
    if (ret)
        goto err_put;

    // 3. 权限检查
    ret = -EPERM;
    if (!ptrace_may_access(p, PTRACE_MODE_READ_REALCREDS))
        goto err_unlock;

    // 4. 获取 robust list 指针
    head = futex_task_robust_list(p, compat);

    up_read(&p->signal->exec_update_lock);
    put_task_struct(p);
    return head;

err_unlock:
    up_read(&p->signal->exec_update_lock);
err_put:
    put_task_struct(p);
    return (void __user *)ERR_PTR(ret);
}
```

## 5. 详细调用链

```
sys_get_robust_list(pid, head_ptr, len_ptr)          // kernel/futex/syscalls.c
  └─ futex_get_robust_list_common(pid, false)        // 获取列表头
       ├─ [pid != 0] find_task_by_vpid(pid)          // 通过 PID 查找进程
       │    └─ [未找到] → return ERR_PTR(-ESRCH)
       ├─ get_task_struct(p)                          // 增加引用计数
       ├─ down_read_killable(&p->signal->exec_update_lock)  // 锁 exec
       ├─ ptrace_may_access(p, PTRACE_MODE_READ_REALCREDS)  // 权限检查
       │    └─ [无权限] → return ERR_PTR(-EPERM)
       ├─ futex_task_robust_list(p, false)            // 获取 robust_list 字段
       │    └─ return p->robust_list                  // 返回保存的指针
       ├─ up_read(&p->signal->exec_update_lock)        // 解锁 exec
       └─ put_task_struct(p)                           // 释放引用
  ├─ put_user(sizeof(*head), len_ptr)                 // 输出结构体大小
  └─ put_user(head, head_ptr)                         // 输出链表头指针
```

## 6. 流程图

```
用户态                         内核态
   |                              |
   | syscall(SYS_get_robust_list, |
   |   pid, &head_ptr, &len_ptr)  |
   |----------------------------->|
   |                          futex_get_robust_list_common():
   |                            ├─ find_task_by_vpid(pid)
   |                            │    └─ [未找到] → -ESRCH
   |                            ├─ get_task_struct(task)
   |                            ├─ down_read(exec_update_lock)
   |                            ├─ ptrace_may_access()
   |                            │    └─ [无权限] → -EPERM
   |                            ├─ task->robust_list
   |                            ├─ up_read(exec_update_lock)
   |                            └─ put_task_struct(task)
   |                              |
   |        return (head, len)   |
   |<-----------------------------|
   |                              |
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `ESRCH` | 进程不存在 | 指定的 `pid` 未找到 |
| `EPERM` | 权限不足 | 调用者无权访问目标进程的 robust list |
| `EFAULT` | 用户空间地址错误 | `head_ptr` 或 `len_ptr` 不可写 |
| `EINTR` | 被信号中断 | 等待 `exec_update_lock` 时被信号中断 |

## 8. 使用示例

```c
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int get_robust_list(pid_t pid, struct robust_list_head **head,
                    size_t *len) {
    return syscall(SYS_get_robust_list, pid, head, len);
}

int main() {
    struct robust_list_head *head = NULL;
    size_t len = 0;

    // 获取当前进程的 robust list
    int ret = get_robust_list(0, &head, &len);
    if (ret == 0) {
        printf("robust list head: %p\n", (void *)head);
        printf("sizeof(struct robust_list_head): %zu\n", len);
        printf("futex_offset: %ld\n", head->futex_offset);
        printf("list_op_pending: %p\n",
               (void *)head->list_op_pending);
    } else {
        printf("错误: %d\n", ret);
    }

    // 获取指定进程的 robust list（需要权限）
    ret = get_robust_list(1234, &head, &len);
    if (ret == -EPERM) {
        printf("无权限访问 PID 1234 的 robust list\n");
    }

    return 0;
}
```

## 9. 关键要点

1. **权限控制**：通过 `ptrace_may_access(p, PTRACE_MODE_READ_REALCREDS)` 检查，非特权进程只能获取自己的 robust list
2. **exec 竞态**：使用 `exec_update_lock` 序列化，防止并发 `exec()` 更改进程凭证后绕过权限检查
3. **引用计数**：`get_task_struct`/`put_task_struct` 确保目标进程的 `task_struct` 在使用期间不会释放
4. **兼容性**：32 位兼容模式下使用 `compat_robust_list_head`，通过 `COMPAT_SYSCALL_DEFINE3(get_robust_list, ...)` 实现
5. **调试用途**：`get_robust_list` 主要用于调试器和监控工具，普通应用程序很少直接调用

## 10. 源码位置

| 文件 | 说明 |
|------|------|
| [kernel/futex/syscalls.c](file:///home/louis/code/linux/kernel/futex/syscalls.c) | `sys_get_robust_list`、`futex_get_robust_list_common` 实现 |
| [include/linux/sched.h](file:///home/louis/code/linux/include/linux/sched.h) | `task_struct` 中的 `robust_list` 字段 |
| [include/uapi/linux/futex.h](file:///home/louis/code/linux/include/uapi/linux/futex.h) | `struct robust_list_head`、`struct robust_list` 定义 |