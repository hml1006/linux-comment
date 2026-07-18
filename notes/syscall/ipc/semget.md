# semget 系统调用分析

## 1. 概述

`semget` 用于获取或创建一个 System V 信号量集合。如果 key 对应的集合已存在，则返回其标识符；否则根据 semflg 创建新集合。

**原型：**

```c
SYSCALL_DEFINE3(semget, key_t, key, int, nsems, int, semflg)
// 实际调用:
return ksys_semget(key, nsems, semflg);
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `key` | IPC 键值（`IPC_PRIVATE` 或通过 `ftok()` 生成） |
| `nsems` | 集合中信号量数量（创建时必需 > 0，获取时可为 0） |
| `semflg` | 标志位，包含权限和创建选项 |

**semflg 标志：**

| 标志 | 说明 |
|------|------|
| `IPC_CREAT` | 若集合不存在则创建 |
| `IPC_EXCL` | 与 IPC_CREAT 一起使用时，若集合已存在则返回 EEXIST |
| `0xxx` | 低 9 位为权限位 |

## 3. 函数调用链

```
semget (系统调用入口)
  └─ ksys_semget(key, nsems, semflg)
       └─ ipcget(ns, &sem_ids(ns), &sem_ops, &sem_params)
            │
            ├─ 如果 key == IPC_PRIVATE:
            │    └─ newary(ns, &sem_params)              // 创建私有信号量集合
            │         ├─ 检查 nsems 范围（>0 且 <= semmsl）
            │         ├─ ipc_rcu_alloc(sizeof(*sma))       // 分配 sem_array
            │         ├─ sma->sem_base = kvmalloc(nsems * sizeof(struct sem))
            │         ├─ 初始化每个信号量（semval=0, sempid=NULL）
            │         ├─ ipc_addid(&sem_ids(ns), &sma->sem_perm, ns->sem_ctlmni)
            │         ├─ sma->sem_nsems = nsems
            │         └─ return sma->sem_perm.id
            │
            └─ 如果 key != IPC_PRIVATE:
                 └─ ipc_findkey(&sem_ids(ns), key)        // 查找已有集合
                      ├─ 找到且 IPC_CREAT|IPC_EXCL → return -EEXIST
                      ├─ 找到:
                      │    ├─ [创建时] nsems > sma->sem_nsems → -EINVAL
                      │    └─ 返回 sma->sem_perm.id
                      └─ 未找到且 IPC_CREAT → newary(ns, &sem_params)
```

## 4. 关键数据结构

### 4.1 struct ipc_ops（信号量操作函数表）

```c
// ipc/sem.c
static const struct ipc_ops sem_ops = {
    .getnew = newary,                    // 创建新信号量集合
    .associate = sem_associate,          // 关联检查
    .more_checks = sem_more_checks,      // 额外检查
};
```

### 4.2 struct sem_array（内核信号量集合）

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

## 5. 流程图

```
用户态调用 semget(key, nsems, semflg)
  │
  v
ksys_semget(key, nsems, semflg)
  │
  v
ipcget(ns, &sem_ids(ns), &sem_ops, &sem_params)
  │
  ├── key == IPC_PRIVATE?
  │    └── 是 → newary() 创建新集合
  │         ├── 检查 nsems (1..semmsl)
  │         ├── 分配 sem_array + sem_base 数组
  │         ├── ipc_addid() 分配 ID
  │         └── 返回 semid
  │
  └── key != IPC_PRIVATE?
       ├── ipc_findkey() 查找
       │    ├── 找到?
       │    │    ├── (IPC_CREAT|IPC_EXCL) → -EEXIST
       │    │    ├── nsems > sma->sem_nsems → -EINVAL
       │    │    ├── 权限检查通过 → 返回 semid
       │    │    └── 权限检查失败 → -EACCES
       │    │
       │    └── 未找到?
       │         ├── (IPC_CREAT) → newary() → 返回 semid
       │         └── (无 IPC_CREAT) → -ENOENT
       │
       └── newary() 内部:
            ├── 分配 sem_array
            ├── 分配 sem 数组 (kvmalloc)
            ├── 初始化 semval = 0, sempid = NULL
            ├── ipc_addid() 分配 ID
            └── 返回 semid
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | nsems < 0 或 nsems > semmsl 或 nsems 与已有集合不匹配 |
| `EACCES` | 权限不足 | 集合存在但无访问权限 |
| `EEXIST` | 集合已存在 | 指定了 IPC_CREAT|IPC_EXCL 且集合已存在 |
| `ENOENT` | 集合不存在 | 未指定 IPC_CREAT 且集合不存在 |
| `ENOMEM` | 内存不足 | 无法分配 sem_array 或 sem 数组 |
| `ENOSPC` | 超出系统限制 | 集合数已达 semmni 上限或信号量总数达 semmns 上限 |

## 7. 使用示例

```c
#include <sys/sem.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main() {
    key_t key = ftok("/tmp", 'S');
    int semid;

    // 创建包含 3 个信号量的集合（如已存在则直接获取）
    semid = semget(key, 3, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("semget");
        exit(1);
    }
    printf("Semaphore set created: semid=%d, nsems=3\n", semid);

    // 尝试独占创建（应失败）
    semid = semget(key, 3, IPC_CREAT | IPC_EXCL | 0666);
    if (semid == -1 && errno == EEXIST) {
        printf("Set already exists (expected)\n");
    }

    // 创建私有信号量集合
    int priv_id = semget(IPC_PRIVATE, 1, 0666);
    if (priv_id == -1) {
        perror("semget IPC_PRIVATE");
        exit(1);
    }
    printf("Private semaphore set: semid=%d\n", priv_id);

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/sem.c`、`ipc/util.c`
- 用户态头文件：`sys/sem.h`