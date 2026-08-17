# shmat 系统调用分析

## 1. 概述

`shmat` 用于将 System V 共享内存段附加（映射）到调用进程的地址空间。成功返回映射后的地址。

**原型：**

```c
SYSCALL_DEFINE3(shmat, int, shmid, char __user *, shmaddr, int, shmflg)
// 实际调用:
err = do_shmat(shmid, shmaddr, shmflg, &ret, SHMLBA);
// 返回值 ret 为映射地址
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `shmid` | 共享内存段 ID |
| `shmaddr` | 期望的映射地址（可为 NULL 让内核选择） |
| `shmflg` | 标志位 |

**shmflg 标志：**

| 标志 | 说明 |
|------|------|
| `SHM_RDONLY` | 只读附加（默认为读写） |
| `SHM_REMAP` | 替换已有映射（需指定 shmaddr） |
| `SHM_EXEC` | 可执行映射 |

**shmaddr 选择规则：**

| shmaddr | 行为 |
|---------|------|
| NULL | 内核自动选择地址（通常靠近栈底或堆顶） |
| 非 NULL | 尝试在指定地址映射（需对齐到 SHMLBA） |
| 非 NULL + SHM_REMAP | 替换指定地址处的已有映射 |

## 3. 函数调用链

```
shmat (系统调用入口)
  └─ do_shmat(shmid, shmaddr, shmflg, &ret, SHMLBA)
       │
       ├─ shm_find(shmid, &shp)                            // 查找共享内存段
       │    └─ shp = shm_obtain_object_check(ns, shmid)
       │
       ├─ ipc_lock_object(&shp->shm_perm)
       │
       ├─ 参数检查:
       │    ├─ shmflg & SHM_EXEC && !(shmflg & SHM_RDONLY) → 可执行映射
       │    └─ shmflg & SHM_RDONLY → 只读映射
       │
       ├─ security_shm_shmat(shp, shmaddr, shmflg)          // LSM 安全检查
       │
       ├─ file = shp->shm_file                              // 共享内存的 tmpfs 文件
       │
       ├─ ipc_unlock_object(&shp->shm_perm)
       │
       ├─ [SHM_REMAP] do_munmap(mm, shmaddr, size)          // 先解除旧映射
       │
       ├─ get_unmapped_area(file, shmaddr, size, ...)        // 获取未映射区域
       │    └─ 根据 shmaddr 和 flags 计算映射地址
       │
       ├─ mmap_file: shm_mmap(file, vma)                    // 执行映射
       │    └─ shmem_file_mmap(file, vma)                    // tmpfs 文件映射
       │         ├─ file->f_op->mmap(file, vma)              // 调用 shmem_mmap
       │         └─ vma->vm_ops = &shmem_vm_ops             // 设置 VMA 操作
       │
       ├─ ipc_update_pid(&shp->shm_lprid, current)          // 更新最后操作 PID
       ├─ shp->shm_nattch++                                  // 增加附加计数
       ├─ shp->shm_atim = ktime_get_real_seconds()           // 更新附加时间
       │
       └─ 返回映射地址 (通过 put_user 写入用户空间)
```

## 4. 关键数据结构

### 4.1 struct shmid_kernel（内核共享内存段）

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

### 4.2 struct shmid_ds（用户态共享内存状态）

```c
// include/uapi/linux/shm.h
struct shmid_ds {
    struct ipc_perm shm_perm;          /* 操作权限 */
    size_t          shm_segsz;         /* 段大小 */
    __kernel_time_t shm_atime;         /* 最后附加时间 */
    __kernel_time_t shm_dtime;         /* 最后分离时间 */
    __kernel_time_t shm_ctime;         /* 最后修改时间 */
    __kernel_pid_t  shm_cpid;          /* 创建者 PID */
    __kernel_pid_t  shm_lpid;          /* 最后操作者 PID */
    unsigned long   shm_nattch;        /* 当前附加计数 */
    /* ... */
};
```

## 5. 流程图

```
用户态调用 shmat(shmid, shmaddr, shmflg)
  │
  v
do_shmat(shmid, shmaddr, shmflg, &ret, SHMLBA)
  │
  ├── 查找共享内存段 (shm_find)
  │
  ├── 安全检查 (LSM)
  │
  ├── 获取 tmpfs 文件 (shp->shm_file)
  │
  ├── [SHM_REMAP] 先解除旧映射
  │
  ├── get_unmapped_area() 计算映射地址
  │
  ├── shm_mmap() 执行 mmap
  │    └── shmem_file_mmap() → tmpfs 页面映射
  │
  ├── 更新统计 (shm_nattch++, shm_atim, shm_lprid)
  │
  └── 返回映射地址
```

## 6. 进程地址空间布局

```
进程虚拟地址空间:

  +------------------+  高地址
  |      栈          |
  |  ↓ 向下增长      |
  |                  |
  |  SHM 映射区域    |  ← shmat 返回的地址
  |  ↑ 向上增长      |
  |      堆          |
  |      数据        |
  |      代码        |
  +------------------+  低地址
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | shmid 无效、shmaddr 未对齐、SHM_REMAP 但地址为 NULL |
| `EACCES` | 权限不足 | 无读写权限 |
| `EIDRM` | 段已删除 | 映射时段已被 IPC_RMID 标记删除 |
| `ENOMEM` | 内存不足 | 无法分配 VMA 结构 |
| `EMFILE` | 映射数超限 | 进程的 VMA 数量已达上限 |
| `ENFILE` | 系统文件数超限 | 系统文件总数已达上限 |
| `EFAULT` | 地址错误 | 地址不可访问 |

## 8. 使用示例

```c
#include <sys/shm.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SHM_SIZE 1024

int main() {
    key_t key = ftok("/tmp", 'M');
    int shmid = shmget(key, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }

    // 附加共享内存（读写模式）
    char *data = shmat(shmid, NULL, 0);
    if (data == (char *)-1) {
        perror("shmat");
        exit(1);
    }
    printf("Shared memory attached at: %p\n", data);

    // 写入数据
    strcpy(data, "Hello, shared memory!");

    // 只读附加（同一段可多次附加）
    char *ro_data = shmat(shmid, NULL, SHM_RDONLY);
    if (ro_data != (char *)-1) {
        printf("Read-only attached at: %p\n", ro_data);
        printf("Read: %s\n", ro_data);
        shmdt(ro_data);
    }

    // 分离
    shmdt(data);

    // 删除
    shmctl(shmid, IPC_RMID, NULL);
    return 0;
}
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/shm.c`
- 内核头文件：`include/linux/shm.h`
- 用户态头文件：`sys/shm.h`