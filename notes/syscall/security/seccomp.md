# seccomp 系统调用分析

## 1. 概述

seccomp（secure computing mode）是 Linux 内核的安全机制，允许进程限制自身可以使用的系统调用及其参数。seccomp 提供两种模式：严格模式（SECCOMP_MODE_STRICT）和过滤模式（SECCOMP_MODE_FILTER）。过滤模式使用 BPF（Berkeley Packet Filter）程序定义系统调用过滤规则。

**原型：**

```c
SYSCALL_DEFINE3(seccomp, unsigned int, op, unsigned int, flags,
                void __user *, uargs)
```

**参数：**
- `op`：操作类型
  - `SECCOMP_SET_MODE_STRICT` (0)：启用严格模式，只允许 read、write、exit、sigreturn 四个系统调用
  - `SECCOMP_SET_MODE_FILTER` (1)：启用过滤模式，使用 BPF 程序过滤
  - `SECCOMP_GET_ACTION_AVAIL` (2)：查询某个 action 是否可用
  - `SECCOMP_GET_NOTIF_SIZES` (3)：获取 seccomp 通知结构体大小
- `flags`：标志位
  - `SECCOMP_FILTER_FLAG_TSYNC`：跨线程同步
  - `SECCOMP_FILTER_FLAG_LOG`：记录违反规则的日志
  - `SECCOMP_FILTER_FLAG_NEW_LISTENER`：创建通知监听 fd
  - `SECCOMP_FILTER_FLAG_SPEC_ALLOW`：允许推测执行缓解
  - `SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV`：等待可被信号杀死的通知
- `uargs`：参数指针，对于 `SECCOMP_SET_MODE_FILTER` 指向 `struct sock_fprog`

## 2. 使用场景

- 沙箱化进程：限制不可信代码只能使用特定系统调用
- 容器运行时：限制容器内进程的系统调用表
- 浏览器渲染进程：Chrome/Chromium 的沙箱核心机制
- SSH 服务：限制未授权访问后的系统调用面

## 3. 函数调用栈

### 3.1 设置过滤器

```
seccomp(op, flags, uargs)                              // kernel/seccomp.c
  └─ do_seccomp(op, flags, uargs)
       ├─ [SECCOMP_SET_MODE_STRICT]
       │    └─ seccomp_set_mode_strict()
       │         └─ current->seccomp.mode = SECCOMP_MODE_STRICT
       │
       ├─ [SECCOMP_SET_MODE_FILTER]
       │    └─ seccomp_set_mode_filter(flags, uargs)
       │         ├─ seccomp_prepare_filter(uargs)       // 准备 BPF 过滤器
       │         │    ├─ copy_from_user(&fprog, uargs)  // 拷贝用户 BPF 程序
       │         │    ├─ seccomp_check_filter(fprog)    // 验证 BPF 指令
       │         │    └─ bpf_prog_create_from_user()    // 创建 BPF 程序
       │         ├─ seccomp_attach_filter(flags, filter)// 附加过滤器
       │         │    ├─ seccomp_may_assign_mode()      // 检查模式是否可分配
       │         │    └─ sp_install_filter(filter)      // 安装到线程
       │         └─ seccomp_notify_start(flags...)      // 可选创建通知 fd
       │
       ├─ [SECCOMP_GET_ACTION_AVAIL]
       │    └─ seccomp_get_action_avail(uargs)
       │
       └─ [SECCOMP_GET_NOTIF_SIZES]
            └─ seccomp_get_notif_sizes(uargs)
```

### 3.2 系统调用过滤执行路径

```
syscall_trace_enter()                                   // arch/.../ptrace.c
  └─ __secure_computing()                              // kernel/seccomp.c
       └─ seccomp_run_filters(desc, &match)
            ├─ BPF 程序执行
            └─ 根据返回值决定 action:
                 ├─ SECCOMP_RET_ALLOW → 允许执行
                 ├─ SECCOMP_RET_KILL_PROCESS → 终止进程
                 ├─ SECCOMP_RET_KILL_THREAD → 终止线程
                 ├─ SECCOMP_RET_ERRNO → 返回错误码
                 ├─ SECCOMP_RET_TRAP → 发送 SIGSYS 信号
                 ├─ SECCOMP_RET_TRACE → 通知 ptrace 追踪器
                 └─ SECCOMP_RET_USER_NOTIF → 通知用户空间监听器
```

## 4. 关键数据结构

### 4.1 struct seccomp_filter（seccomp 过滤器）

```c
// kernel/seccomp.c
struct seccomp_filter {
    refcount_t refs;                    // 引用计数
    refcount_t users;                   // 用户计数
    bool log;                           // 是否记录日志
    struct seccomp_filter *prev;        // 前一个过滤器（嵌套）
    struct bpf_prog *prog;              // BPF 过滤程序
    struct notification *notif;         // 通知机制（SECCOMP_USER_NOTIF_FLAG）
};
```

### 4.2 struct sock_fprog（用户空间传入的 BPF 程序）

```c
// include/uapi/linux/filter.h
struct sock_fprog {
    unsigned short len;     // BPF 指令数量
    struct sock_filter *filter; // BPF 指令数组
};

struct sock_filter {        // BPF 指令
    __u16 code;             // 指令码（如 BPF_LD | BPF_W | BPF_ABS）
    __u8 jt;                // 跳转偏移（真）
    __u8 jf;                // 跳转偏移（假）
    __u32 k;                // 常量值
};
```

### 4.3 seccomp_data（BPF 程序可访问的数据）

```c
// include/uapi/linux/seccomp.h
struct seccomp_data {
    int nr;                     // 系统调用号
    __u32 arch;                 // 架构标识（如 AUDIT_ARCH_X86_64）
    __u64 instruction_pointer;  // 指令指针
    __u64 args[6];              // 系统调用参数（最多 6 个）
};
```

## 5. 流程图

```
用户态: seccomp(SECCOMP_SET_MODE_FILTER, 0, &prog)
    │
    v
┌─────────────────────────────────────┐
│ seccomp_prepare_filter(&prog)      │
│ 1. 从用户空间拷贝 BPF 程序         │
│ 2. seccomp_check_filter() 验证指令 │
│ 3. bpf_prog_create_from_user()     │
│    创建内核可执行 BPF 程序          │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ seccomp_may_assign_mode()           │
│ 1. 检查 no_new_privs 或 CAP_SYS_ADMIN│
│ 2. 检查是否已设置过滤器            │
│ 3. 检查 CAP_SYS_ADMIN 能力          │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ seccomp_attach_filter()             │
│ 1. sp_install_filter(filter)        │
│    - 将过滤器添加到进程过滤器链     │
│ 2. 如果设置了 TSYNC 标志:           │
│    - 同步到所有线程                 │
│ 3. 如果设置了 NEW_LISTENER 标志:   │
│    - 创建通知监听 fd                │
└─────────────────────────────────────┘
    │
    v
返回 0 (成功)

--- 后续每次系统调用时的检查 ---
    │
    v
┌─────────────────────────────────────┐
│ __secure_computing()                │
│ 1. 读取当前系统调用号与参数         │
│ 2. 执行过滤器链中的每个 BPF 程序    │
│ 3. 根据返回值决定 action            │
│ 4. 执行 action 对应的操作           │
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | op 未知 / flags 无效 / BPF 程序无效 |
| `-EACCES` | 权限不足 | 没有 no_new_privs 也没有 CAP_SYS_ADMIN |
| `-EFAULT` | 内存错误 | uargs 不可读 |
| `-ENOMEM` | 内存不足 | BPF 程序分配失败 |
| `-EOPNOTSUPP` | 不支持 | 请求的 action 不可用 |
| `-ESRCH` | 线程错误 | TSYNC 时某个线程不存在 |

## 7. 使用示例

```c
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main(void)
{
    /* 定义 BPF 过滤规则:
     * 只允许: read(0), write(1), exit_group(0)
     */
    struct sock_filter filter[] = {
        /* 加载系统调用架构 */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, arch)),
        /* 检查是否为 x86_64 */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        /* 架构不匹配 → 终止进程 */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        /* 加载系统调用号 */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, nr)),
        /* 允许 read */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_read, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        /* 允许 write */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_write, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        /* 允许 exit_group */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit_group, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        /* 其他系统调用 → 返回 ENOSYS */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
    };

    struct sock_fprog prog = {
        .len = sizeof(filter) / sizeof(filter[0]),
        .filter = filter,
    };

    /* 设置 no_new_privs */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        perror("prctl");
        return 1;
    }

    /* 应用 seccomp 过滤器 */
    if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog)) {
        perror("seccomp");
        return 1;
    }

    printf("Seccomp filter installed!\n");
    printf("Only read, write, and exit_group are allowed.\n");

    /* 尝试一个被禁止的系统调用 */
    FILE *f = fopen("/tmp/test", "w");
    if (f == NULL) {
        /* open 被禁止，errno 为 ENOSYS */
        printf("Expected: open() failed with: %m\n");
    }

    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/seccomp.c`
- 头文件：`include/uapi/linux/seccomp.h`
- BPF 头文件：`include/uapi/linux/filter.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#权限与安全)