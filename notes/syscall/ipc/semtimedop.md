# semtimedop 系统调用分析

## 1. 概述

`semtimedop` 是 `semop` 的增强版，支持指定超时时间。如果操作无法立即执行，进程将阻塞至多指定的时间。

**原型：**

```c
SYSCALL_DEFINE4(semtimedop, int, semid, struct sembuf __user *, tsops,
                unsigned int, nsops,
                const struct __kernel_timespec __user *, timeout)
// 实际调用:
return ksys_semtimedop(semid, tsops, nsops, timeout);
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `semid` | 信号量集合 ID |
| `tsops` | `struct sembuf` 数组，描述要执行的操作 |
| `nsops` | 操作数组大小（最多 `SEMOPM` 个） |
| `timeout` | 超时时间（绝对时间），NULL 表示无限等待 |

**semop 与 semtimedop 的关系：**

```c
// semop 是 semtimedop 的特例（timeout = NULL）
SYSCALL_DEFINE3(semop, int, semid, struct sembuf __user *, tsops,
                unsigned, nsops)
{
    return ksys_semtimedop(semid, tsops, nsops, NULL);
}
```

## 3. 函数调用链

```
semtimedop (系统调用入口)
  └─ ksys_semtimedop(semid, tsops, nsops, timeout)
       │
       ├─ 如果 timeout != NULL:
       │    ├─ copy_from_user(&ts, timeout, sizeof(ts))
       │    └─ timespec64_valid(&ts) 检查有效性
       │
       └─ do_semtimedop(semid, tsops, nsops, timeout ? &ts : NULL)
            │
            ├─ 参数检查:
            │    ├─ nsops > SEMOPM → -E2BIG
            │    ├─ nsops <= 0 → -EINVAL
            │    └─ semid < 0 → -EINVAL
            │
            ├─ sops = copy_sops_from_user(tsops, nsops, ...)  // 拷贝操作数组
            │    ├─ nsops <= SEMOPM_FAST → 使用栈上数组
            │    └─ nsops > SEMOPM_FAST → kmalloc 分配
            │
            ├─ rcu_read_lock()
            ├─ sma = sem_obtain_object_check(ns, semid)       // 查找集合
            ├─ ipc_lock_object(&sma->sem_perm)
            │
            ├─ try_atomic_semop(sma, sops, nsops, ...)        // 尝试原子执行
            │    ├─ 遍历所有操作:
            │    │    ├─ 检查 sem_num 范围
            │    │    ├─ 检查 sem_op + semval 结果
            │    │    └─ 所有可执行 → 更新值并返回 0
            │    │
            │    └─ 不可执行 → 返回要阻塞的操作
            │
            ├─ 可执行? → 成功返回
            │
            ├─ [慢路径] 创建 sem_queue 结构:
            │    ├─ 分配 sem_queue
            │    ├─ 设置操作参数
            │    ├─ 加入等待队列:
            │    │    ├─ 操作包含修改 → pending_alter
            │    │    └─ 操作只读 → pending_const
            │    │
            │    ├─ set_current_state(TASK_INTERRUPTIBLE)
            │    ├─ ipc_unlock_object(&sma->sem_perm)
            │    ├─ rcu_read_unlock()
            │    │
            │    ├─ schedule_timeout(timeout)                  // 阻塞等待
            │    │    └─ timeout 为 NULL → MAX_SCHEDULE_TIMEOUT
            │    │
            │    ├─ [唤醒后] 检查结果:
            │    │    ├─ queue.status == 1 → 操作成功 → 返回 0
            │    │    ├─ queue.status == -EINTR → 信号中断
            │    │    ├─ queue.status == -EAGAIN → 超时
            │    │    └─ 其他错误
            │    │
            │    └─ 从等待队列移除
            │
            └─ update_stats(sma, ...)                         // 更新统计
```

## 4. 关键数据结构

### 4.1 struct sembuf（信号量操作）

```c
// include/uapi/linux/sem.h
struct sembuf {
    unsigned short sem_num;  /* 信号量编号 */
    short          sem_op;   /* 操作值 */
    short          sem_flg;  /* 标志 */
};
```

### 4.2 struct sem_queue（等待队列）

```c
// ipc/sem.c
struct sem_queue {
    struct list_head  list;           /* 链表节点 */
    struct task_struct *sleeper;      /* 等待进程 */
    struct sem_undo   *undo;          /* UNDO 结构 */
    struct sembuf     *sops;          /* 操作数组 */
    struct sembuf     *blocking;      /* 导致阻塞的操作 */
    int               nsops;          /* 操作数 */
    int               error;          /* 错误状态 */
    bool              alter;          /* 是否为修改操作 */
    bool              dupsop;         /* 是否拷贝了 sops */
};
```

## 5. 流程图

```
用户态调用 semtimedop(semid, sops, nsops, timeout)
  │
  v
do_semtimedop(semid, sops, nsops, timeout)
  │
  ├── 拷贝操作数组
  │
  ├── 查找并锁定信号量集合
  │
  ├── try_atomic_semop() 尝试原子执行
  │    │
  │    ├── 成功? → 更新值 → 返回 0
  │    │
  │    └── 失败?
  │         ├── IPC_NOWAIT → -EAGAIN
  │         │
  │         └── 阻塞:
  │              ├── 创建 sem_queue
  │              ├── 加入等待队列
  │              ├── schedule_timeout(timeout)
  │              │    │
  │              │    ├── 超时 → -EAGAIN
  │              │    ├── 信号中断 → -EINTR
  │              │    └── 操作成功 (其他进程 V 操作唤醒) → 0
  │              │
  │              └── 清理
  │
  └── 返回结果
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | semid < 0 或 nsops <= 0 或超时时间无效 |
| `E2BIG` | 操作过多 | nsops > SEMOPM |
| `EACCES` | 权限不足 | 无访问权限 |
| `EAGAIN` | 暂时不可执行 | IPC_NOWAIT 或超时 |
| `EIDRM` | 集合已删除 | 等待期间集合被删除 |
| `EINTR` | 信号中断 | 等待时被信号中断 |
| `EFBIG` | 编号越界 | sem_num >= sem_nsems |
| `ENOMEM` | 内存不足 | 分配 sem_queue 失败 |
| `EFAULT` | 地址错误 | sops 或 timeout 指针不可访问 |

## 7. 使用示例

```c
#include <sys/sem.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

int main() {
    key_t key = ftok("/tmp", 'S');
    int semid = semget(key, 1, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("semget");
        exit(1);
    }

    // 初始化信号量为 0
    semctl(semid, 0, SETVAL, 0);

    struct sembuf sb = {0, -1, 0};  // P 操作：等待直到值 >= 1

    // 设置 5 秒超时
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;

    printf("Waiting for semaphore (timeout 5s)...\n");
    int ret = semtimedop(semid, &sb, 1, &ts);
    if (ret == -1) {
        if (errno == EAGAIN) {
            printf("Timeout: semaphore not available within 5s\n");
        } else {
            perror("semtimedop");
        }
        exit(1);
    }
    printf("Semaphore acquired\n");

    // 释放
    sb.sem_op = 1;
    semop(semid, &sb, 1);

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/sem.c`
- 用户态头文件：`sys/sem.h`