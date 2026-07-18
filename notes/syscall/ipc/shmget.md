# shmget 系统调用分析

## 1. 概述

`shmget` 用于获取或创建一个 System V 共享内存段。如果 key 对应的段已存在，则返回其标识符；否则根据 shmflg 创建新段。

**原型：**

```c
SYSCALL_DEFINE3(shmget, key_t, key, size_t, size, int, shmflg)
// 实际调用:
return ksys_shmget(key, size, shmflg);
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `key` | IPC 键值（`IPC_PRIVATE` 或通过 `ftok()` 生成） |
| `size` | 共享内存段大小（字节，向上取整到 PAGE_SIZE 的倍数） |
| `shmflg` | 标志位，包含权限和创建选项 |

**shmflg 标志：**

| 标志 | 说明 |
|------|------|
| `IPC_CREAT` | 若段不存在则创建 |
| `IPC_EXCL` | 与 IPC_CREAT 一起使用时，若段已存在则返回 EEXIST |
| `SHM_HUGETLB` | 使用大页面（hugetlbfs） |
| `SHM_NORESERVE` | 不预留交换空间 |
| `0xxx` | 低 9 位为权限位 |

## 3. 函数调用链

```
shmget (系统调用入口)
  └─ ksys_shmget(key, size, shmflg)
       └─ ipcget(ns, &shm_ids(ns), &shm_ops, &shm_params)
            │
            ├─ 参数检查:
            │    ├─ size < SHMMIN (1) 或 size > SHMMAX → -EINVAL
            │    └─ size 向上对齐到 PAGE_SIZE
            │
            ├─ 如果 key == IPC_PRIVATE:
            │    └─ newseg(ns, &shm_params)              // 创建私有段
            │         ├─ 检查 shmall 限制
            │         ├─ file = shmem_kernel_file_setup(name, size, flags)
            │         │    └─ 创建 tmpfs 文件（inode + 页面缓存）
            │         ├─ shp = ipc_rcu_alloc(sizeof(*shp))
            │         ├─ shp->shm_file = file
            │         ├─ ipc_addid(&shm_ids(ns), &shp->shm_perm, ns->shm_ctlmni)
            │         ├─ shp->shm_segsz = size
            │         ├─ shp->shm_cprid = task_tgid(current)
            │         └─ return shp->shm_perm.id
            │
            └─ 如果 key != IPC_PRIVATE:
                 └─ ipc_findkey(&shm_ids(ns), key)        // 查找已有段
                      ├─ 找到且 IPC_CREAT|IPC_EXCL → return -EEXIST
                      ├─ 找到:
                      │    ├─ size > shp->shm_segsz → -EINVAL
                      │    └─ 返回 shp->shm_perm.id
                      └─ 未找到且 IPC_CREAT → newseg(ns, &shm_params)
```

## 4. 关键数据结构

### 4.1 struct ipc_ops（共享内存操作函数表）

```c
// ipc/shm.c
static const struct ipc_ops shm_ops = {
    .getnew = newseg,                    // 创建新共享内存段
    .associate = shm_associate,          // 关联检查
    .more_checks = shm_more_checks,      // 额外检查
};
```

### 4.2 struct shmid_kernel（内核共享内存段）

```c
// include/linux/shm.h
struct shmid_kernel {
    struct kern_ipc_perm shm_perm;     /* IPC 权限结构 */
    struct file *shm_file;             /* 指向 tmpfs 文件的 file 结构 */
    unsigned long shm_nattch;          /* 附加计数 */
    unsigned long shm_segsz;           /* 段大小（字节） */
    time64_t shm_atim;                 /* 最后附加时间 */
    time64_t shm_dtim;                 /* 最后分离时间 */
    time64_t shm_ctim;                 /* 最后修改时间 */
    struct pid *shm_cprid;             /* 创建进程 PID */
    struct pid *shm_lprid;             /* 最后操作进程 PID */
    struct user_struct *mlock_user;    /* 锁定用户 */
};
```

## 5. 流程图

```
用户态调用 shmget(key, size, shmflg)
  │
  v
ksys_shmget(key, size, shmflg)
  │
  ├── 检查 size 范围 (SHMMIN..SHMMAX)
  │
  v
ipcget(ns, &shm_ids(ns), &shm_ops, &shm_params)
  │
  ├── key == IPC_PRIVATE?
  │    └── 是 → newseg() 创建新段
  │         ├── 检查 shmall 限制
  │         ├── shmem_kernel_file_setup() 创建 tmpfs 文件
  │         ├── 分配 shmid_kernel
  │         ├── ipc_addid() 分配 ID
  │         └── 返回 shmid
  │
  └── key != IPC_PRIVATE?
       ├── ipc_findkey() 查找
       │    ├── 找到?
       │    │    ├── (IPC_CREAT|IPC_EXCL) → -EEXIST
       │    │    ├── size > shp->shm_segsz → -EINVAL
       │    │    ├── 权限检查通过 → 返回 shmid
       │    │    └── 权限检查失败 → -EACCES
       │    │
       │    └── 未找到?
       │         ├── (IPC_CREAT) → newseg() → 返回 shmid
       │         └── (无 IPC_CREAT) → -ENOENT
       │
       └── newseg() 内部:
            ├── 创建 tmpfs 匿名文件
            ├── 分配 shmid_kernel
            ├── ipc_addid() 分配 ID
            └── 返回 shmid
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | size < SHMMIN 或 size > SHMMAX 或 size 与已有段不匹配 |
| `EACCES` | 权限不足 | 段存在但无访问权限 |
| `EEXIST` | 段已存在 | 指定了 IPC_CREAT|IPC_EXCL 且段已存在 |
| `ENOENT` | 段不存在 | 未指定 IPC_CREAT 且段不存在 |
| `ENOMEM` | 内存不足 | 无法分配 shmid_kernel 或 tmpfs 文件 |
| `ENOSPC` | 超出系统限制 | 段数已达 shmmni 上限或总页数达 shmall 上限 |
| `ENFILE` | 系统文件数超限 | 创建 tmpfs 文件时系统文件数已达上限 |

## 7. 使用示例

```c
#include <sys/shm.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main() {
    key_t key = ftok("/tmp", 'M');
    int shmid;

    // 创建 4KB 共享内存段（如已存在则直接获取）
    shmid = shmget(key, 4096, IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }
    printf("Shared memory created: shmid=%d, size=4096\n", shmid);

    // 尝试独占创建（应失败，因为段已存在）
    shmid = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0666);
    if (shmid == -1 && errno == EEXIST) {
        printf("Segment already exists (expected)\n");
    }

    // 创建私有共享内存段
    int priv_id = shmget(IPC_PRIVATE, 1024, 0666);
    if (priv_id == -1) {
        perror("shmget IPC_PRIVATE");
        exit(1);
    }
    printf("Private shared memory: shmid=%d\n", priv_id);

    return 0;
}
```

## 8. 内存映射示意

```
shmget 创建共享内存段:

  ┌──────────────────┐
  │  shmid_kernel     │  内核结构
  │  shm_file ───────┼──┐
  │  shm_segsz = 4KB  │  │
  │  shm_nattch = 0   │  │
  └──────────────────┘  │
                        │
  ┌─────────────────────┘
  ▼
  ┌──────────────────┐
  │  tmpfs file       │  tmpfs 文件（页面缓存）
  │  ┌───┬───┬───┬──┐│
  │  │ 0 │ 1 │ 2 │ 3││  ← 物理页（通过 page fault 按需分配）
  │  └───┴───┴───┴──┘│
  └──────────────────┘
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/shm.c`、`ipc/util.c`
- 内核头文件：`include/linux/shm.h`
- 用户态头文件：`sys/shm.h`