# shmctl 系统调用分析

## 1. 概述

`shmctl` 是 System V 共享内存的控制操作，用于对共享内存段执行各种控制命令，包括获取状态、设置属性、加锁/解锁、删除等。

**原型：**

```c
SYSCALL_DEFINE3(shmctl, int, shmid, int, cmd, struct shmid_ds __user *, buf)
// 实际调用:
return ksys_shmctl(shmid, cmd, buf, IPC_64);
```

## 2. 支持的 cmd 命令

| 命令 | 说明 |
|------|------|
| `IPC_STAT` | 获取共享内存段的 `shmid_ds` 状态信息 |
| `IPC_SET` | 设置 `shm_perm.uid`、`shm_perm.gid`、`shm_perm.mode` |
| `IPC_RMID` | 删除共享内存段（标记删除，nattch=0 时真正释放） |
| `IPC_INFO` | 获取系统级共享内存限制信息（`shminfo` 结构） |
| `SHM_INFO` | 获取系统级共享内存消耗信息 |
| `SHM_STAT` | 获取指定索引的共享内存段状态（遍历用） |
| `SHM_STAT_ANY` | 类似 SHM_STAT，但无需拥有权限 |
| `SHM_LOCK` | 锁定共享内存段（禁止换出） |
| `SHM_UNLOCK` | 解锁共享内存段 |

## 3. 函数调用链

```
shmctl (系统调用入口)
  └─ ksys_shmctl(shmid, cmd, buf, IPC_64)
       │
       ├─ case IPC_INFO:
       │    ├─ shmctl_ipc_info(ns, &shminfo)              // 获取系统限制
       │    │    └─ shmmni / shmmax / shmall 等
       │    └─ copy_shminfo_to_user(buf, &shminfo, version)
       │
       ├─ case SHM_INFO:
       │    ├─ shmctl_shm_info(ns, &shm_info)             // 获取消耗信息
       │    │    └─ used_ids / shm_rss / shm_swp 等
       │    └─ copy_to_user(buf, &shm_info, sizeof(shm_info))
       │
       ├─ case IPC_STAT / SHM_STAT / SHM_STAT_ANY:
       │    ├─ shmctl_stat(ns, shmid, cmd, &sem64)        // 获取段状态
       │    │    ├─ shp = shm_obtain_object_check(ns, shmid)
       │    │    ├─ ipc_lock_object(&shp->shm_perm)
       │    │    ├─ 填充 shmid64_ds 各字段
       │    │    └─ ipc_unlock_object(&shp->shm_perm)
       │    └─ copy_shmid_to_user(buf, &sem64, version)
       │
       ├─ case IPC_SET:
       │    ├─ copy_shmid_from_user(&sem64, buf, version)  // 拷贝新属性
       │    └─ fallthrough → shmctl_down(ns, shmid, cmd, &sem64)
       │         ├─ shp = shm_obtain_object_check(ns, shmid)
       │         ├─ ipcctl_pre_down(&shp->shm_perm, ...)
       │         ├─ 更新 shm_perm.uid/gid/mode
       │         └─ ipc_unlock_object(&shp->shm_perm)
       │
       ├─ case IPC_RMID:
       │    └─ shmctl_down(ns, shmid, cmd, &sem64)
       │         ├─ shm_destroy(ns, shp)                   // 销毁段
       │         │    ├─ shm_rmid(ns, shp)                 // 从 IDR 移除
       │         │    ├─ shm_unlink(shp)                   // unlink tmpfs 文件
       │         │    └─ 若 nattch == 0 → 立即释放
       │         │         └─ shm_destroy_immediate(shp)
       │         └─ ipc_unlock_object(&shp->shm_perm)
       │
       ├─ case SHM_LOCK:
       │    └─ shmctl_do_lock(ns, shmid, cmd)
       │         ├─ shp = shm_obtain_object_check(ns, shmid)
       │         ├─ 遍历 shm_file 的页面
       │         │    └─ shmem_lock(shp->shm_file, 0, ...)  // 锁定页面
       │         └─ shp->shm_perm.mode |= SHM_LOCKED
       │
       ├─ case SHM_UNLOCK:
       │    └─ shmctl_do_lock(ns, shmid, cmd)
       │         ├─ shp = shm_obtain_object_check(ns, shmid)
       │         └─ shmem_lock(shp->shm_file, 1, ...)      // 解锁页面
       │
       └─ default:
            return -EINVAL
```

## 4. 关键数据结构

### 4.1 struct shminfo64（系统级共享内存限制）

```c
// include/uapi/linux/shm.h
struct shminfo64 {
    unsigned long shmmax;     /* 最大共享内存段大小（字节） */
    unsigned long shmmin;     /* 最小共享内存段大小（1 字节） */
    unsigned long shmmni;     /* 最大共享内存段数量 */
    unsigned long shmseg;     /* 每进程最大附加数 */
    unsigned long shmall;     /* 系统级共享内存总页数上限 */
    unsigned long __unused1;
    unsigned long __unused2;
    unsigned long __unused3;
    unsigned long __unused4;
};
```

### 4.2 struct shm_info（消耗信息）

```c
// include/uapi/linux/shm.h
struct shm_info {
    int used_ids;             /* 当前使用的段数 */
    unsigned long shm_tot;    /* 总页数 */
    unsigned long shm_rss;    /* 驻留页数 */
    unsigned long shm_swp;    /* 交换页数 */
    unsigned long swap_attempts;
    unsigned long swap_successes;
};
```

## 5. 流程图

```
用户态调用 shmctl(shmid, cmd, buf)
  │
  v
ksys_shmctl(shmid, cmd, buf, IPC_64)
  │
  ├── IPC_INFO ──── shmctl_ipc_info() ──── copy_to_user(shminfo)
  ├── SHM_INFO ──── shmctl_shm_info() ──── copy_to_user(shm_info)
  │
  ├── IPC_STAT ──── shmctl_stat() ──── copy_shmid_to_user()
  ├── SHM_STAT ───── shmctl_stat() ──── copy_shmid_to_user()
  ├── SHM_STAT_ANY ─ shmctl_stat() ──── copy_shmid_to_user()
  │
  ├── IPC_SET ────── copy_from_user() ── shmctl_down() 更新权限
  │
  ├── IPC_RMID ───── shmctl_down() ──── shm_destroy()
  │                      │
  │                      ├── shm_rmid() 从 IDR 移除
  │                      ├── shm_unlink() unlink tmpfs 文件
  │                      └── nattch==0 ? 立即释放 : 等待 nattch 归零
  │
  ├── SHM_LOCK ───── shmctl_do_lock() ── shmem_lock() 锁定页面
  ├── SHM_UNLOCK ─── shmctl_do_lock() ── shmem_lock() 解锁页面
  │
  └── default ────── return -EINVAL
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | shmid < 0 或 cmd < 0 或未知 cmd |
| `EACCES` | 权限不足 | IPC_STAT 时无读权限 |
| `EPERM` | 操作不允许 | IPC_SET/IPC_RMID/SHM_LOCK/SHM_UNLOCK 时权限不足 |
| `EIDRM` | 段已删除 | 操作时段已被标记删除 |
| `EFAULT` | 用户空间拷贝失败 | buf 指针不可访问 |
| `ENOMEM` | 内存不足 | SHM_LOCK 时无法锁定页面 |

## 7. 使用示例

```c
#include <sys/shm.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>

#define SHM_SIZE 4096

int main() {
    key_t key = ftok("/tmp", 'M');
    int shmid = shmget(key, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }

    struct shmid_ds buf;

    // 获取共享内存状态
    if (shmctl(shmid, IPC_STAT, &buf) == -1) {
        perror("shmctl IPC_STAT");
        exit(1);
    }
    printf("Size: %lu, nattch: %lu\n", buf.shm_segsz, buf.shm_nattch);
    printf("Creator PID: %d, Last PID: %d\n",
           buf.shm_cpid, buf.shm_lpid);

    // 锁定共享内存（防止换出）
    if (shmctl(shmid, SHM_LOCK, NULL) == -1) {
        perror("shmctl SHM_LOCK");
    }

    // 解锁
    if (shmctl(shmid, SHM_UNLOCK, NULL) == -1) {
        perror("shmctl SHM_UNLOCK");
    }

    // 删除共享内存段
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl IPC_RMID");
        exit(1);
    }
    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/shm.c`
- 用户态头文件：`sys/shm.h`