# semop 系统调用分析

## 1. 概述

`semop` 用于对 System V 信号量集合执行一组操作。每个操作可以增加、减少信号量值，或等待其变为 0。`semop` 是 `semtimedop` 的特殊形式（不带超时参数）。

**原型：**

```c
SYSCALL_DEFINE3(semop, int, semid, struct sembuf __user *, tsops,
                unsigned, nsops)
// 实际调用:
return ksys_semtimedop(semid, tsops, nsops, NULL);  // timeout = NULL
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `semid` | 信号量集合 ID |
| `tsops` | `struct sembuf` 数组，描述要执行的操作 |
| `nsops` | 操作数组大小（最多 `SEMOPM` 个，通常 500） |

**struct sembuf：**

```c
struct sembuf {
    unsigned short sem_num;  /* 信号量编号（从 0 开始） */
    short          sem_op;   /* 操作值 */
    short          sem_flg;  /* 标志：IPC_NOWAIT, SEM_UNDO */
};
```

**sem_op 操作语义：**

| sem_op 值 | 行为 |
|-----------|------|
| `> 0` | 将值加到信号量上（释放资源），唤醒等待的进程 |
| `0` | 等待信号量值变为 0（若当前不为 0 则阻塞） |
| `< 0` | 尝试减少信号量值（获取资源），若不够则阻塞 |

**sem_flg 标志：**

| 标志 | 说明 |
|------|------|
| `IPC_NOWAIT` | 无法立即执行时立即返回 EAGAIN，不阻塞 |
| `SEM_UNDO` | 进程退出时自动撤销操作（防止资源泄漏） |

## 3. 函数调用链

```
semop (系统调用入口)
  └─ ksys_semtimedop(semid, tsops, nsops, NULL)          // timeout = NULL
       └─ do_semtimedop(semid, tsops, nsops, NULL)
            │
            ├─ 参数检查:
            │    ├─ nsops > SEMOPM → -E2BIG
            │    ├─ nsops <= 0 → -EINVAL
            │    └─ semid < 0 → -EINVAL
            │
            ├─ sops = 从用户空间拷贝 sembuf 数组（或使用栈上的 fast_sops）
            │
            ├─ rcu_read_lock()
            ├─ sma = sem_obtain_object_check(ns, semid)   // 查找集合
            │
            ├─ ipc_lock_object(&sma->sem_perm)
            │
            ├─ try_atomic_semop(sma, sops, nsops, ...)    // 尝试原子执行所有操作
            │    ├─ 遍历所有 sembuf:
            │    │    ├─ sem_op + semval >= 0?  // 检查是否可执行
            │    │    └─ 所有操作可执行 →
            │    │         ├─ 更新 semval (sem_op + semval)
            │    │         ├─ 更新 sempid
            │    │         ├─ [SEM_UNDO] 更新 undo 结构
            │    │         └─ 返回 0 (成功)
            │    │
            │    └─ 有操作不可执行:
            │         └─ 返回 -EAGAIN (需要阻塞)
            │
            ├─ 可原子执行? → 成功返回
            │
            ├─ 不可执行 → 进入慢路径:
            │    ├─ [IPC_NOWAIT] → return -EAGAIN
            │    │
            │    ├─ 创建 sem_queue 结构，加入等待队列
            │    │    ├─ [简单操作] 加入 sem_pending_const 或 sem_pending_alter
            │    │    └─ [复杂操作] complex_count++
            │    │
            │    ├─ set_current_state(TASK_INTERRUPTIBLE)
            │    ├─ ipc_unlock_object(&sma->sem_perm)
            │    ├─ rcu_read_unlock()
            │    │
            │    ├─ schedule_timeout(timeout)             // 阻塞（timeout=NULL 表示无限等待）
            │    │
            │    ├─ [被唤醒后] 检查状态:
            │    │    ├─ 操作成功 (sem_op 完成) → 返回 0
            │    │    ├─ 信号中断 → -EINTR / -ERESTARTSYS
            │    │    └─ 超时 → -EAGAIN
            │    │
            │    └─ 从等待队列移除
            │
            └─ update_stats(sma, ...)                     // 更新统计信息
```

## 4. 关键数据结构

### 4.1 struct sembuf（用户态信号量操作）

```c
// include/uapi/linux/sem.h
struct sembuf {
    unsigned short sem_num;  /* 信号量编号（0 ~ sem_nsems-1） */
    short          sem_op;   /* 操作值（正/负/零） */
    short          sem_flg;  /* 标志（IPC_NOWAIT / SEM_UNDO） */
};
```

### 4.2 struct sem_queue（等待队列项）

```c
// ipc/sem.c (内部结构)
struct sem_queue {
    struct list_head list;               /* 等待队列链表节点 */
    struct task_struct *sleeper;         /* 等待的进程 */
    struct sem_undo *undo;               /* SEM_UNDO 结构 */
    struct sembuf *sops;                 /* 操作数组 */
    struct sembuf *blocking;             /* 导致阻塞的操作 */
    int nsops;                           /* 操作数 */
    int error;                           /* 错误码 */
    bool alter;                          /* 是否为修改操作 */
    /* ... */
};
```

### 4.3 struct sem_undo（UNDO 结构）

```c
// ipc/sem.c
struct sem_undo {
    struct list_head list_proc;          /* 进程链表 */
    struct rcu_head rcu;                 /* RCU 头 */
    struct semid_list *semid_list;       /* 所属集合 */
    struct sem_undo_list *ulp;           /* 所属 undo 列表 */
    short *semadj;                       /* 调整数组（每个信号量一个调整值） */
};
```

## 5. 流程图

```
用户态调用 semop(semid, sops, nsops)
  │
  v
do_semtimedop(semid, sops, nsops, NULL)
  │
  ├── 拷贝操作数组到内核
  │
  ├── 查找信号量集合 (sem_obtain_object_check)
  │
  ├── try_atomic_semop() 尝试原子执行
  │    │
  │    ├── 所有操作可执行?
  │    │    ├── 是 → 更新 semval → 返回 0
  │    │    │
  │    │    └── 否 → 需要阻塞
  │    │         ├── IPC_NOWAIT → -EAGAIN
  │    │         │
  │    │         └── 进入慢路径:
  │    │              ├── 创建 sem_queue
  │    │              ├── 加入等待队列
  │    │              ├── schedule() 阻塞
  │    │              │
  │    │              ├── 被唤醒:
  │    │              │    ├── 操作成功 → 返回 0
  │    │              │    ├── 信号中断 → -EINTR
  │    │              │    └── 超时 → -EAGAIN
  │    │              │
  │    │              └── 清理等待队列
  │    │
  │    └── update_stats()
  │
  └── 返回结果
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | semid < 0 或 nsops <= 0 或 sem_num 超出范围 |
| `E2BIG` | 操作过多 | nsops > SEMOPM |
| `EACCES` | 权限不足 | 无访问权限 |
| `EAGAIN` | 暂时不可执行 | IPC_NOWAIT 且操作无法立即执行或超时 |
| `EIDRM` | 集合已删除 | 等待期间集合被删除 |
| `EINTR` | 信号中断 | 等待时被信号中断 |
| `EFBIG` | 编号越界 | sem_num >= sem_nsems |
| `ERANGE` | 值溢出 | 操作导致 semval 超出 SEMVMX 范围 |
| `ENOMEM` | 内存不足 | 无法分配 sem_queue 结构 |

## 7. 使用示例

```c
#include <sys/sem.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    key_t key = ftok("/tmp", 'S');
    int semid = semget(key, 1, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("semget");
        exit(1);
    }

    // 初始化信号量为 1（二进制信号量/互斥锁）
    if (semctl(semid, 0, SETVAL, 1) == -1) {
        perror("semctl SETVAL");
        exit(1);
    }

    struct sembuf sb;

    // P 操作（获取锁）：sem_op = -1
    sb.sem_num = 0;
    sb.sem_op = -1;
    sb.sem_flg = SEM_UNDO;  // 进程退出时自动释放
    if (semop(semid, &sb, 1) == -1) {
        perror("semop P");
        exit(1);
    }
    printf("Lock acquired\n");

    // 临界区操作...

    // V 操作（释放锁）：sem_op = +1
    sb.sem_op = 1;
    sb.sem_flg = SEM_UNDO;
    if (semop(semid, &sb, 1) == -1) {
        perror("semop V");
        exit(1);
    }
    printf("Lock released\n");

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/sem.c`
- 用户态头文件：`sys/sem.h`