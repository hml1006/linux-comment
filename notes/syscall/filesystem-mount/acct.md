# acct 系统调用分析

## 1. 概述

启用或禁用进程记账（process accounting）。当启用时，内核在每个进程终止时向记账文件写入一条记录，记录该进程的资源使用情况。

**原型：**

```c
SYSCALL_DEFINE1(acct, const char __user *, name)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `name` | `const char *` | 记账文件的路径名；传入 NULL 表示停止记账 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_PACCT` 权限
  - `-ENOMEM` — 内存不足
  - `-EFAULT` — 用户态指针无效
  - `-EACCES` — 指定文件不可写

## 2. 使用场景

- **系统审计**: 记录所有用户进程的 CPU 时间、内存使用、I/O 操作等
- **资源计费**: 多用户环境下按使用量计费
- **性能分析**: 收集进程生命周期统计信息
- **安全监控**: 记录异常退出的进程（core dump、信号终止）

## 3. 函数调用栈

```
acct(name) (系统调用入口)
└─ ksys_acct(name)                                    // kernel/acct.c
   ├─ [name == NULL] → acct_file = NULL               // 关闭记账
   │
   └─ [name != NULL] → 打开记账文件
      ├─ filp_open(name, O_WRONLY|O_APPEND, 0)         // 打开记账文件
      ├─ acct_file = file                              // 保存记账文件
      │
      └─ [进程退出时]
         └─ acct_process()                             // kernel/acct.c
            └─ do_acct_process(acct_file, ...)         // 写记账记录
               ├─ fill_ac(acct)                        // 填充 acct 结构
               │  ├─ ac->ac_uid = from_kuid(uid)       // 用户 ID
               │  ├─ ac->ac_gid = from_kgid(gid)       // 组 ID
               │  ├─ ac->ac_pid = pid_nr(task_pid)     // 进程 ID
               │  ├─ ac->ac_btime = task->start_time   // 开始时间
               │  ├─ ac->ac_utime = ...                // 用户态 CPU 时间
               │  ├─ ac->ac_stime = ...                // 内核态 CPU 时间
               │  └─ ac->ac_comm = task->comm          // 命令名
               └─ file_write(acct_file, &ac, sizeof(ac)) // 写入文件
```

## 4. 关键数据结构

```c
// ===== struct acct (进程记账记录, include/uapi/linux/acct.h) =====
struct acct {
    char ac_flag;               // 记账标志（AFORK/ASU/ACOMPAT/ACORE/AXSIG）
    char ac_version;            // 版本号
    char ac_16bit_spare[2];     // 预留
    __u16 ac_tty;               // 控制终端
    __u32 ac_exitcode;          // 退出码
    __u32 ac_uid;               // 用户 ID
    __u32 ac_gid;               // 组 ID
    __u32 ac_pid;               // 进程 ID
    __u32 ac_ppid;              // 父进程 ID
    __u32 ac_btime;             // 开始时间（秒）
    float ac_etime;             // 已执行时间
    comp_t ac_utime;            // 用户态 CPU 时间
    comp_t ac_stime;            // 内核态 CPU 时间
    comp_t ac_mem;              // 平均内存使用量
    comp_t ac_io;               // 读写操作数
    comp_t ac_rw;               // 读写字节数
    char ac_comm[16];           // 命令名
};

// ===== struct pacct_struct (进程记账信息, kernel/acct.c) =====
struct pacct_struct {
    struct acct ac;             // 记账记录
    unsigned long ac_flag;      // 记账标志
    unsigned long ac_emul;      // 模拟标志
    atomic_t ac_count;          // 引用计数
};
```

## 5. 流程图

```
用户态调用 acct("/var/log/account/pacct")
  │
  v
ksys_acct(name)
  │
  ├─ filp_open(name, O_WRONLY|O_APPEND)  // 打开记账文件
  │
  ├─ 保存到全局 acct_file
  │
  └─ 进程退出时 ────────────────────┐
                                   v
                            do_acct_process()
                                   │
                                   ├─ fill_ac() 填充 acct 记录
                                   │   ├─ 进程基本信息 (pid, uid, gid, comm)
                                   │   ├─ CPU 时间 (utime, stime)
                                   │   └─ 内存使用 (mem)
                                   │
                                   └─ file_write() 写入记账文件
```

## 6. 使用示例

```c
#include <unistd.h>
#include <sys/acct.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 启用进程记账，记录到 /var/log/account/pacct
    if (acct("/var/log/account/pacct") == -1) {
        perror("acct");
        return 1;
    }
    printf("Accounting enabled\n");

    // 执行一些操作...
    system("sleep 1");
    system("ls -l /tmp");

    // 停止记账
    if (acct(NULL) == -1) {
        perror("acct stop");
        return 1;
    }
    printf("Accounting disabled\n");
    return 0;
}
```

## 7. 参考

- `kernel/acct.c` — 进程记账核心实现
- `include/uapi/linux/acct.h` — 记账记录结构定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)