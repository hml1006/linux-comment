# arc_settls

## 原理与功能

`arc_settls` 是 ARC（Argonaut RISC Core）架构专用的系统调用，用于设置线程本地存储（TLS）指针。在 ARM64 架构上，此系统调用存在于编号表中，但仅为 ARC 架构提供支持，ARM64 上为 stub 实现（返回 -ENOSYS）。

### 功能说明

- 设置用户态线程本地存储（TLS）寄存器
- ARC 架构编号为 245（`__NR_arc_settls`）
- 定义为 `__NR_arc_settls` = `__NR_arch_specific_syscall + 1`

## 函数原型

```c
// ARC 架构原生实现
SYSCALL_DEFINE1(arc_settls, void *, user_tls_data_ptr)
{
    task_thread_info(current)->thr_ptr = (unsigned int)user_tls_data_ptr;
    return 0;
}
```

## 调用链分析

```
arc_settls(user_tls_data_ptr)
  └─ task_thread_info(current)->thr_ptr = (unsigned int)user_tls_data_ptr
       └─ 返回 0 表示成功
```

## 关键数据结构

```c
// arch/arc/include/asm/thread_info.h
struct thread_info {
    unsigned long thr_ptr;    // 线程本地存储指针
    // ... 其他字段
};
```

数据结构关系：

```
task_struct
  └─ thread_info
       └─ thr_ptr  ───→ 用户态 TLS 数据区域
```

## 流程图

```
用户态调用 arc_settls(ptr)
  │
  ▼
系统调用入口 (trap)
  │
  ▼
SYSCALL_DEFINE1(arc_settls, void *, user_tls_data_ptr)  // arch/arc/kernel/process.c
  │
  ▼
task_thread_info(current)->thr_ptr = (unsigned int)ptr  // 保存 TLS 指针
  │
  ▼
返回 0 (成功)
```

## 设计特点

- 实现极其简洁：仅将用户态传入的 TLS 指针保存到 `thread_info.thr_ptr` 字段
- 不需要任何权限检查，任何用户态进程都可以设置自己的 TLS 指针
- 该指针在进程上下文切换时由内核自动保存和恢复

## 使用场景

- 线程创建时初始化 TLS 区域
- 用户态线程库（如 pthreads）实现
- 动态链接器（ld-linux）设置线程私有存储

## 相关系统调用

| 系统调用 | 编号 | 功能 |
|--|--|--|
| `arc_settls` | 245 | 设置 TLS 指针 |
| `arc_gettls` | 246 | 获取 TLS 指针 |
| `arc_usr_cmpxchg` | 248 | 用户态 CAS 操作 |

## 与 ARM64 对比

| 特性 | ARC | ARM64 |
|--|--|--|
| 实现方式 | 系统调用 | `msr tpidr_el0, x0` 指令 |
| 编号 | 245 | stub |
| 存储位置 | `thread_info.thr_ptr` | `tpidr_el0` 系统寄存器 |
| 性能 | 较高（系统调用开销） | 极低（单条指令） |

## 注意事项

- ARM64 上此系统调用仅为 ARC 架构兼容性保留
- ARM64 通过 `msr tpidr_el0, x0` 指令直接设置 TLS 寄存器

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/arc/kernel/process.c](/home/louis/code/linux/arch/arc/kernel/process.c) | arc_settls 实现 |
| [arch/arc/include/asm/syscalls.h](/home/louis/code/linux/arch/arc/include/asm/syscalls.h) | 系统调用声明 |
| [tools/arch/arc/include/uapi/asm/unistd.h](/home/louis/code/linux/tools/arch/arc/include/uapi/asm/unistd.h) | 系统调用编号定义 |