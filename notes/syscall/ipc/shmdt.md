# shmdt 系统调用分析

## 1. 概述

`shmdt` 用于将之前通过 `shmat` 附加的共享内存段从调用进程的地址空间中分离（解除映射）。分离后，进程不能再访问该段，但共享内存段本身仍然存在，直到被删除或所有附加进程都分离。

**原型：**

```c
SYSCALL_DEFINE1(shmdt, char __user *, shmaddr)
// 实际调用:
return ksys_shmdt(shmaddr);
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `shmaddr` | 之前 `shmat` 返回的映射地址 |

## 3. 函数调用链

```
shmdt (系统调用入口)
  └─ ksys_shmdt(shmaddr)
       │
       ├─ mm = current->mm
       ├─ mmap_write_lock(mm)
       │
       ├─ for (vma = mm->mmap; vma; vma = vma->vm_next) {
       │    │
       │    ├─ 检查 vma 是否为共享内存段:
       │    │    ├─ vma->vm_ops == &shmem_vm_ops?          // tmpfs VMA
       │    │    └─ vma->vm_file->f_op == &shm_file_operations?
       │    │
       │    ├─ 匹配 vma 地址（以 SHMLBA 对齐）:
       │    │    ├─ shmaddr 位于 [vma->vm_start, vma->vm_end) 范围内
       │    │    └─ vma->vm_start <= shmaddr < vma->vm_end
       │    │
       │    ├─ 找到匹配的 VMA:
       │    │    ├─ shm_destroy_vma(vma)                    // 处理分离
       │    │    │    ├─ 获取 shp = shm_file->private_data
       │    │    │    ├─ ipc_lock_object(&shp->shm_perm)
       │    │    │    ├─ shp->shm_nattch--                  // 减少附加计数
       │    │    │    ├─ shp->shm_dtim = ktime_get_real_seconds()  // 更新分离时间
       │    │    │    ├─ ipc_update_pid(&shp->shm_lprid, current)
       │    │    │    └─ [nattch==0 且已标记删除] → shm_destroy_immediate(shp)
       │    │    │
       │    │    └─ do_munmap(mm, vma->vm_start, size)     // 解除 VMA 映射
       │    │
       │    └─ 继续查找下一个 vma
       │  }
       │
       ├─ mmap_write_unlock(mm)
       │
       └─ 返回结果（分离段数，0 表示未找到匹配）
```

## 4. 关键数据结构

### 4.1 struct vm_area_struct (VMA，进程地址空间区域)

```c
// include/linux/mm_types.h
struct vm_area_struct {
    unsigned long vm_start;            /* 起始地址 */
    unsigned long vm_end;              /* 结束地址 */
    struct file *vm_file;              /* 映射的文件（共享内存的 tmpfs 文件） */
    const struct vm_operations_struct *vm_ops; /* VMA 操作函数 */
    /* ... */
};
```

## 5. 流程图

```
用户态调用 shmdt(shmaddr)
  │
  v
ksys_shmdt(shmaddr)
  │
  ├── 获取进程 mm 锁
  │
  ├── 遍历进程的 VMA 链表
  │    │
  │    ├── 检查每个 VMA:
  │    │    ├── 是共享内存 VMA? (检查 vm_ops)
  │    │    └── 地址匹配? (shmaddr 在范围内)
  │    │
  │    ├── 找到匹配:
  │    │    ├── shm_nattch-- (减少附加计数)
  │    │    ├── 更新 shm_dtim, shm_lprid
  │    │    ├── [nattch==0 && 已标记删除] 立即释放
  │    │    └── do_munmap() 解除映射
  │    │
  │    └── 继续查找下一个 VMA
  │
  └── 释放 mm 锁，返回结果
```

## 6. 共享内存生命周期

```
shmget() → 创建共享内存段
  │
  ├── shmat() → 附加到进程 A → nattch=1
  ├── shmat() → 附加到进程 B → nattch=2
  │
  ├── shmctl(IPC_RMID) → 标记删除（段仍存在，nattch=2）
  │
  ├── shmdt() → 进程 A 分离 → nattch=1
  ├── shmdt() → 进程 B 分离 → nattch=0 → 真正释放
  │
  └── 段被销毁，资源回收
```

## 7. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | shmaddr 不是有效的共享内存映射地址 |
| `ENOMEM` | 内存不足 | 解除映射时内存操作失败 |

## 8. 使用示例

```c
#include <sys/shm.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    key_t key = ftok("/tmp", 'M');
    int shmid = shmget(key, 1024, IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }

    // 附加共享内存
    char *data = shmat(shmid, NULL, 0);
    if (data == (char *)-1) {
        perror("shmat");
        exit(1);
    }

    strcpy(data, "Hello, shared memory!");

    // 分离共享内存
    if (shmdt(data) == -1) {
        perror("shmdt");
        exit(1);
    }
    printf("Shared memory detached\n");

    // 分离后不能再访问 data
    // 删除共享内存段
    shmctl(shmid, IPC_RMID, NULL);
    return 0;
}
```

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程间通信-ipc)
- 源码位置：`ipc/shm.c`
- 用户态头文件：`sys/shm.h`