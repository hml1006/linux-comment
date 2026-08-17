# semctl 系统调用分析

## 1. 概述

`semctl` 是 System V 信号量的控制操作，用于对信号量集合执行各种控制命令，包括获取/设置值、获取状态、删除集合等。

**原型：**

```c
SYSCALL_DEFINE4(semctl, int, semid, int, semnum, int, cmd, unsigned long, arg)
// 实际调用:
return ksys_semctl(semid, semnum, cmd, arg, IPC_64);
```

## 2. 支持的 cmd 命令

| 命令 | 说明 |
|------|------|
| `IPC_STAT` | 获取信号量集合的 `semid_ds` 状态信息 |
| `IPC_SET` | 设置信号量集合的 `sem_perm.uid`、`sem_perm.gid`、`sem_perm.mode` |
| `IPC_RMID` | 删除信号量集合 |
| `IPC_INFO` | 获取系统级信号量限制信息（`seminfo` 结构） |
| `SEM_INFO` | 获取系统级信号量消耗信息 |
| `SEM_STAT` | 获取指定索引的信号量集合状态（遍历用） |
| `SEM_STAT_ANY` | 类似 SEM_STAT，但无需拥有权限 |
| `GETVAL` | 获取信号量集合中第 semnum 个信号量的值 |
| `SETVAL` | 设置信号量集合中第 semnum 个信号量的值 |
| `GETPID` | 获取最后一个操作第 semnum 个信号量的进程 PID |
| `GETNCNT` | 获取等待第 semnum 个信号量值增加的进程数 |
| `GETZCNT` | 获取等待第 semnum 个信号量值变为 0 的进程数 |
| `GETALL` | 获取集合中所有信号量的值 |
| `SETALL` | 设置集合中所有信号量的值 |

## 3. 函数调用链

```
semctl (系统调用入口)
  └─ ksys_semctl(semid, semnum, cmd, arg, IPC_64)
       │
       ├─ case IPC_INFO / SEM_INFO:
       │    └─ semctl_info(ns, semid, cmd, p)              // 获取系统限制信息
       │         ├─ 获取 semmni / semmsl / semmns 等限制
       │         └─ copy_to_user(p, &seminfo, sizeof(seminfo))
       │
       ├─ case IPC_STAT / SEM_STAT / SEM_STAT_ANY:
       │    ├─ semctl_stat(ns, semid, cmd, &semid64)       // 获取集合状态
       │    │    ├─ sma = sem_obtain_object_check(ns, semid)
       │    │    ├─ ipc_lock_object(&sma->sem_perm)
       │    │    ├─ 填充 semid64_ds 各字段
       │    │    └─ ipc_unlock_object(&sma->sem_perm)
       │    └─ copy_semid_to_user(p, &semid64, version)
       │
       ├─ case GETALL:
       │    └─ semctl_main(ns, semid, semnum, cmd, p)
       │         ├─ sma = sem_obtain_object_check(ns, semid)
       │         ├─ ipc_lock_object(&sma->sem_perm)
       │         ├─ 遍历 sem_base 数组，读取所有 semval
       │         ├─ copy_to_user(p, array, ...)
       │         └─ ipc_unlock_object(&sma->sem_perm)
       │
       ├─ case GETVAL / GETPID / GETNCNT / GETZCNT:
       │    └─ semctl_main(ns, semid, semnum, cmd, p)
       │         ├─ 检查 semnum 范围 [0, sma->sem_nsems)
       │         ├─ 读取对应字段
       │         └─ 返回整数值
       │
       ├─ case SETALL:
       │    └─ semctl_main(ns, semid, semnum, cmd, p)
       │         ├─ copy_from_user(array, p, ...)
       │         ├─ ipc_lock_object(&sma->sem_perm)
       │         ├─ 遍历并设置 sem_base 数组
       │         ├─ 唤醒等待的进程（值变化可能满足等待条件）
       │         └─ ipc_unlock_object(&sma->sem_perm)
       │
       ├─ case SETVAL:
       │    └─ semctl_setval(ns, semid, semnum, val)
       │         ├─ ipc_lock_object(&sma->sem_perm)
       │         ├─ 设置 sem_base[semnum].semval = val
       │         ├─ 唤醒等待的进程
       │         └─ ipc_unlock_object(&sma->sem_perm)
       │
       ├─ case IPC_SET:
       │    ├─ copy_semid_from_user(&semid64, p, version)
       │    └─ fallthrough → semctl_down(ns, semid, cmd, &semid64)
       │         ├─ ipc_lock_object(&sma->sem_perm)
       │         ├─ ipcctl_pre_down(&sma->sem_perm, ...)
       │         ├─ 更新 sem_perm.uid/gid/mode
       │         └─ ipc_unlock_object(&sma->sem_perm)
       │
       ├─ case IPC_RMID:
       │    └─ semctl_down(ns, semid, cmd, &semid64)
       │         ├─ freeary(ns, semid)                      // 释放信号量集合
       │         │    ├─ 遍历所有等待队列，唤醒所有进程
       │         │    ├─ ipc_rmid(&sem_ids(ns), &sma->sem_perm)
       │         │    └─ kvfree(sma->sem_base)
       │         └─ ipc_unlock_object(&sma->sem_perm)
       │
       └─ default:
            return -EINVAL
```

## 4. 关键数据结构

### 4.1 struct semid64_ds（信号量集合用户态数据结构）

```c
// include/uapi/linux/sem.h
struct semid64_ds {
    struct ipc64_perm sem_perm;        /* 所有权和权限 */
    __kernel_time_t sem_otime;         /* 最后 semop 时间 */
    __kernel_ulong_t sem_otime_high;   /* 高 32 位 */
    __kernel_time_t sem_ctime;         /* 最后修改时间 */
    __kernel_ulong_t sem_ctime_high;   /* 高 32 位 */
    __kernel_ulong_t sem_nsems;        /* 集合中信号量数量 */
    __kernel_ulong_t __unused3;
    __kernel_ulong_t __unused4;
};
```

### 4.2 struct seminfo（系统级信号量限制）

```c
// include/uapi/linux/sem.h
struct seminfo {
    int semmap;    /* 信号量映射条目数，未使用 */
    int semmni;    /* 最大信号量集合数 */
    int semmns;    /* 最大信号量总数 */
    int semmnu;    /* 最大 undo 结构数，未使用 */
    int semmsl;    /* 每个集合最大信号量数 */
    int semopm;    /* 每次 semop 最大操作数 */
    int semume;    /* 每个进程最大 undo 条目数，未使用 */
    int semusz;    /* sem_undo 结构大小，未使用 */
    int semvmx;    /* 信号量最大值 */
    int semaem;    /* 最大调整值，未使用 */
};
```

### 4.3 struct sem_array（内核信号量集合）

```c
// include/linux/sem.h
struct sem_array {
    struct kern_ipc_perm sem_perm;     /* IPC 权限结构 */
    time64_t sem_ctime;                /* 最后修改时间 */
    struct sem *sem_base;              /* 信号量数组基址 */
    struct list_head pending_alter;    /* 待修改操作队列 */
    struct list_head pending_const;    /* 待常量操作队列 */
    struct list_head list_id;          /* 散列表 */
    int sem_nsems;                     /* 信号量数量 */
    int complex_count;                 /* 复杂操作计数 */
};
```

### 4.4 struct sem（单个信号量）

```c
// include/linux/sem.h
struct sem {
    int semval;                        /* 信号量当前值 */
    struct pid *sempid;                /* 最后操作进程 PID */
    struct list_head sem_pending_alter; /* 待修改操作链表 */
    struct list_head sem_pending_const; /* 待常量操作链表 */
};
```

## 5. 流程图

```
用户态调用 semctl(semid, semnum, cmd, arg)
  │
  v
ksys_semctl(semid, semnum, cmd, arg, IPC_64)
  │
  ├── IPC_INFO ──── semctl_info() ──── copy_to_user(seminfo)
  ├── SEM_INFO ──── semctl_info() ──── copy_to_user(seminfo)
  │
  ├── IPC_STAT ──── semctl_stat() ──── copy_semid_to_user()
  ├── SEM_STAT ───── semctl_stat() ──── copy_semid_to_user()
  ├── SEM_STAT_ANY ── semctl_stat() ──── copy_semid_to_user()
  │
  ├── GETVAL ─────── semctl_main() ──── 返回 sem_base[semnum].semval
  ├── GETPID ─────── semctl_main() ──── 返回 sem_base[semnum].sempid
  ├── GETNCNT ────── semctl_main() ──── 返回 pending_alter 计数
  ├── GETZCNT ────── semctl_main() ──── 返回 pending_const 计数
  ├── GETALL ─────── semctl_main() ──── copy_to_user(所有 semval)
  │
  ├── SETVAL ─────── semctl_setval() ── 设置单个值 → 唤醒等待进程
  ├── SETALL ─────── semctl_main() ──── 设置所有值 → 唤醒等待进程
  │
  ├── IPC_SET ────── copy_from_user() ── semctl_down() 更新权限
  ├── IPC_RMID ───── semctl_down() ──── freeary() → 释放集合
  │
  └── default ────── return -EINVAL
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | semid < 0 或未知 cmd 或 semnum 超出范围 |
| `EACCES` | 权限不足 | 无访问权限 |
| `EIDRM` | 集合已删除 | 操作时集合已被删除 |
| `EFAULT` | 用户空间拷贝失败 | 指针不可访问 |
| `ERANGE` | 值超出范围 | SETVAL/SETALL 时 semval 超出 SEMVMX |
| `EPERM` | 操作不允许 | IPC_SET/IPC_RMID 时权限不足 |
| `ENOMEM` | 内存不足 | 无法分配临时数组 |

## 7. 使用示例

```c
#include <sys/sem.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    key_t key = ftok("/tmp", 'S');
    int semid;

    // 创建包含 1 个信号量的集合
    semid = semget(key, 1, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("semget");
        exit(1);
    }

    // 设置信号量值为 5
    if (semctl(semid, 0, SETVAL, 5) == -1) {
        perror("semctl SETVAL");
        exit(1);
    }

    // 获取信号量值
    int val = semctl(semid, 0, GETVAL);
    printf("Semaphore value: %d\n", val);

    // 获取最后一个操作的 PID
    int pid = semctl(semid, 0, GETPID);
    printf("Last PID: %d\n", pid);

    // 获取集合状态
    struct semid_ds buf;
    if (semctl(semid, 0, IPC_STAT, &buf) == -1) {
        perror("semctl IPC_STAT");
        exit(1);
    }
    printf("NSems: %lu, uid: %d\n", buf.sem_nsems, buf.sem_perm.uid);

    // 删除信号量集合
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("semctl IPC_RMID");
        exit(1);
    }
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/sem.c`
- 用户态头文件：`sys/sem.h`